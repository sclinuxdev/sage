//! Transaction crash recovery, journal advancement, and atomic state reconciliation.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};
use sage_core::{hex, under_root};
use sha2::{Digest, Sha256};

use crate::channel::{
    AvailablePackages, arch_matches, load_available_with_pool, obtain_release_archive,
};
use crate::services::RenderedServicesState;
use crate::state::{
    Alternative, AlternativesDocument, ProfileEngine, SysusersDocument, SysusersEngine,
};
use crate::transaction::{
    PackageDeclarations, cleanup_services, package_ownership, write_atomic_under_root,
};
use crate::triggers::{TriggerEngine, TriggerEvent, TriggerSpec};

/// Reads installed packages from state database, handling dry-run mode.
pub(crate) fn installed_packages(
    db_path: &Path,
    dry_run: bool,
) -> Result<Vec<sage_db::InstalledPackage>> {
    if dry_run {
        Ok(sage_db::read_packages(db_path)?)
    } else {
        Ok(sage_db::SageDatabase::open(db_path)?.packages()?)
    }
}

/// Reads all TOML files in a directory, returning paths sorted deterministically.
pub(crate) fn read_toml_files(dir: &Path) -> Result<Vec<PathBuf>> {
    if !dir.exists() {
        return Ok(Vec::new());
    }
    let mut entries = std::fs::read_dir(dir)?.collect::<Result<Vec<_>, _>>()?;
    entries.sort_by_key(std::fs::DirEntry::file_name);
    Ok(entries
        .into_iter()
        .map(|entry| entry.path())
        .filter(|path| path.extension().is_some_and(|ext| ext == "toml"))
        .collect())
}

/// Reads all raw TOML documents under a sysroot-relative directory.
pub(crate) fn read_documents(root: &Path, relative: &Path) -> Result<Vec<Vec<u8>>> {
    read_toml_files(&under_root(root, relative))?
        .into_iter()
        .map(|path| std::fs::read(path).map_err(Into::into))
        .collect()
}

/// Parses documents into combined alternative specifications.
pub(crate) fn alternatives_from_documents(documents: &[Vec<u8>]) -> Result<Vec<Alternative>> {
    documents.iter().try_fold(Vec::new(), |mut acc, doc| {
        acc.extend(AlternativesDocument::parse(doc)?.alternatives());
        Ok(acc)
    })
}

/// Serializes all currently loaded sysroot triggers into documents.
pub(crate) fn trigger_documents(root: &Path) -> Result<Vec<Vec<u8>>> {
    TriggerEngine::load_triggers(root)?
        .into_iter()
        .map(|trigger| Ok(toml::to_string(&trigger)?.into_bytes()))
        .collect()
}

/// Reconciles alternatives and sysuser definitions at transaction boundary.
pub(crate) fn settle_journal_alternatives(
    root: &Path,
    database: &sage_db::SageDatabase,
    journal: &mut sage_db::JournalRecord,
    previous_alternatives: &[Alternative],
) -> Result<()> {
    if journal.stage == "alternatives" {
        let current = AlternativesDocument::load_installed(root)?;
        ProfileEngine::reconcile_alternatives(root, previous_alternatives, &current)?;
        let accounts = SysusersDocument::load_installed(root)?;
        SysusersEngine::reconcile(root, &accounts)?;
        journal.advance("triggers");
        database.write_journal(journal)?;
        crash_point(root, "alternatives")?;
    }
    Ok(())
}

/// Executes triggers associated with modified paths and advances journal stage.
pub(crate) fn settle_journal_triggers(
    root: &Path,
    database: &sage_db::SageDatabase,
    journal: &mut sage_db::JournalRecord,
    modified: &[PathBuf],
    triggers: &[TriggerSpec],
    event: TriggerEvent,
) -> Result<()> {
    if journal.stage == "triggers" {
        crash_point(root, "triggers")?;
        TriggerEngine::execute_triggers_for(triggers, modified, root, event)?;
        let next_stage = if matches!(
            &journal.action,
            sage_db::JournalAction::Install {
                rebuild: Some(_),
                ..
            }
        ) {
            "rebuild-removal-triggers"
        } else if journal.declaration.is_some() {
            "declaration"
        } else {
            "complete"
        };
        journal.advance(next_stage);
        database.write_journal(journal)?;
        crash_point(root, "trigger-complete")?;
    }
    Ok(())
}

/// Applies a journaled declaration mutation after package and provider state
/// are durable. The current bytes are checked so an administrator edit is
/// never silently overwritten during recovery.
fn settle_journal_declaration(
    root: &Path,
    database: &sage_db::SageDatabase,
    journal: &mut sage_db::JournalRecord,
) -> Result<()> {
    if journal.stage != "declaration" {
        return Ok(());
    }
    crash_point(root, "declaration")?;
    if let Some(mutation) = journal.declaration.as_ref() {
        apply_file_mutation(root, mutation)?;
    }
    journal.advance("complete");
    database.write_journal(journal)?;
    crash_point(root, "declaration-complete")?;
    Ok(())
}

fn apply_file_mutation(root: &Path, mutation: &sage_db::FileMutation) -> Result<()> {
    let relative = Path::new(&mutation.path);
    if relative.as_os_str().is_empty()
        || relative.components().any(|component| {
            matches!(
                component,
                std::path::Component::RootDir | std::path::Component::ParentDir
            )
        })
    {
        bail!("unsafe declaration path {}", mutation.path);
    }
    let target = under_root(root, relative);
    let current = match std::fs::read(&target) {
        Ok(bytes) => Some(bytes),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => None,
        Err(error) => return Err(error.into()),
    };
    if current == mutation.next {
        return Ok(());
    }
    if current != mutation.previous {
        bail!(
            "declaration {} changed outside Sage; refusing to overwrite",
            mutation.path
        );
    }
    match &mutation.next {
        Some(bytes) => write_atomic_under_root(root, relative, bytes)?,
        None => remove_file_beneath(root, &target)?,
    }
    Ok(())
}

/// One-shot sysroot-local crash injection; the next startup exercises recovery.
#[cfg(feature = "torture")]
pub(crate) fn crash_point(root: &Path, stage: &str) -> Result<()> {
    let marker = under_root(root, Path::new("/run/sage/crash-point"));
    let requested = std::fs::read_to_string(&marker).ok();
    let graceful = requested
        .as_deref()
        .is_some_and(|value| value.trim() == stage);
    let aborting = requested
        .as_deref()
        .is_some_and(|value| value.trim() == format!("abort:{stage}"));
    if graceful || aborting {
        std::fs::remove_file(marker)?;
        if aborting {
            std::process::abort();
        }
        bail!("injected crash after {stage}");
    }
    Ok(())
}

#[cfg(not(feature = "torture"))]
pub(crate) fn crash_point(_root: &Path, _stage: &str) -> Result<()> {
    Ok(())
}

/// Scans for pending journal records left by interrupted operations and executes forward recovery.
pub async fn settle_journals(root: &Path) -> Result<()> {
    let path = under_root(root, Path::new("/var/lib/sage"));
    if !path.exists() {
        return Ok(());
    }
    let database = sage_db::SageDatabase::open(path)?;
    for mut journal in database.pending_journals()? {
        journal.validate()?;
        eprintln!(
            "Recovering unfinished operation {} ({})",
            journal.op_id, journal.stage
        );
        match &journal.action {
            sage_db::JournalAction::Install { architecture, .. } => {
                let architecture = architecture.clone();
                let available = load_available_with_pool(root, Some(&architecture), None)?;
                resume_install(root, &database, &available, &mut journal, false).await?;
            }
            sage_db::JournalAction::Remove { .. } => {
                resume_remove(root, &database, &mut journal)?;
            }
        }
        eprintln!("Recovered operation {}", journal.op_id);
    }
    Ok(())
}

/// Resumes an interrupted package installation/upgrade journal forward to completion.
pub(crate) async fn resume_install(
    root: &Path,
    database: &sage_db::SageDatabase,
    available: &AvailablePackages,
    journal: &mut sage_db::JournalRecord,
    prevalidated: bool,
) -> Result<()> {
    journal.validate()?;
    let (architecture, changes, previous_packages) = match &journal.action {
        sage_db::JournalAction::Install {
            architecture,
            changes,
            previous_packages,
            ..
        } => (
            architecture.clone(),
            changes.clone(),
            previous_packages
                .iter()
                .cloned()
                .map(|package| (package.key.clone(), package))
                .collect::<BTreeMap<_, _>>(),
        ),
        _ => bail!("install recovery received a removal journal"),
    };
    let mut rebuild = match &journal.action {
        sage_db::JournalAction::Install { rebuild, .. } => rebuild.clone(),
        _ => unreachable!(),
    };
    if journal.stage == "rebuild-cleanup" {
        crash_point(root, "rebuild-cleanup")?;
        let work = rebuild.as_ref().context("missing rebuild continuation")?;
        let next: RenderedServicesState =
            toml::from_str(std::str::from_utf8(&work.rendered_services)?)?;
        cleanup_services(
            root,
            &next,
            !previous_packages.is_empty() || !work.retired_packages.is_empty(),
        )?;
        journal.advance("rebuild-retirement");
        database.write_journal(journal)?;
        crash_point(root, "rebuild-cleanup-complete")?;
    }
    let package_cache = under_root(root, Path::new("/var/cache/sage/packages"));
    let engine = sage_repo::DownloadEngine::new(&package_cache)?;
    let mut previous_config = BTreeMap::new();
    for package in previous_packages.values() {
        previous_config.extend(package.config_hashes.clone());
    }
    if let Some(work) = &rebuild {
        for package in &work.retired_packages {
            previous_config.extend(package.config_hashes.clone());
        }
    }
    if journal.stage == "rebuild-retirement" {
        let work = rebuild.as_mut().context("missing rebuild continuation")?;
        let mut retired_paths = Vec::new();
        for package in &work.retired_packages {
            database.remove(&package.key)?;
            crash_point(root, "removal")?;
        }
        for package in &work.retired_packages {
            for relative in &package.files {
                if database.owners(relative)?.is_empty() {
                    let path = root.join(relative);
                    if !should_preserve_config(&path, relative, &package.config_hashes)? {
                        remove_file_beneath(root, &path)?;
                        retired_paths.push(relative.clone());
                    }
                }
            }
        }
        work.removed_paths = retired_paths;
        if let sage_db::JournalAction::Install { rebuild: saved, .. } = &mut journal.action {
            *saved = rebuild.clone();
        }
        // Once extraction starts, old file paths may be new directories. A
        // durable boundary prevents recovery from unlinking those paths again.
        journal.advance("packages");
        database.write_journal(journal)?;
        crash_point(root, "rebuild-retirement")?;
    }
    let mut modified = Vec::new();
    if journal.stage == "packages" {
        for (key, version) in &changes {
            let source = available
                .releases
                .get(&(key.clone(), version.clone()))
                .with_context(|| format!("index record disappeared for {key} {version}"))?;
            let archive = obtain_release_archive(&engine, &package_cache, source).await?;
            let inspection = sage_archive::inspect_package(&archive)?;
            if !arch_matches(&inspection.manifest.arch, &architecture) {
                bail!(
                    "package {} has architecture {}, expected {}",
                    key,
                    inspection.manifest.arch,
                    architecture
                );
            }
            let target = under_root(root, &source.target_root);
            std::fs::create_dir_all(&target)?;
            let prefix = source
                .target_root
                .strip_prefix("/")
                .unwrap_or(&source.target_root);
            let declarations = PackageDeclarations::parse(&inspection, key)?;
            let ownership = package_ownership(prefix, &inspection.files, &declarations);
            for path in &ownership {
                let owners = database.owners(path)?;
                if owners.iter().any(|owner| owner != key) {
                    bail!("file conflict for {path}: {owners:?}");
                }
            }
            let previous_package = previous_packages.get(key).cloned();
            let mut previous = BTreeMap::new();
            for record in inspection
                .files
                .iter()
                .filter(|record| record.path.starts_with("etc"))
            {
                let physical = prefix.join(&record.path).to_string_lossy().into_owned();
                let hash = previous_package
                    .as_ref()
                    .and_then(|package| package.config_hashes.get(&physical))
                    .or_else(|| previous_config.get(&physical));
                if let Some(hash) = hash {
                    previous.insert(record.path.to_string_lossy().into_owned(), hash.clone());
                }
            }
            let report = if prevalidated {
                sage_archive::extract_prevalidated_package(
                    &archive,
                    &target,
                    &inspection.files,
                    &previous,
                )?
            } else {
                sage_archive::extract_package_with_config(
                    &archive,
                    &target,
                    &inspection.files,
                    &previous,
                )?
            };
            crash_point(root, "extraction")?;
            declarations.write_under_root(root)?;
            modified.extend(ownership.iter().map(PathBuf::from));
            let config_hashes = inspection
                .files
                .iter()
                .filter(|record| record.path.starts_with("etc"))
                .map(|record| {
                    (
                        prefix.join(&record.path).to_string_lossy().into_owned(),
                        record.sha256.clone(),
                    )
                })
                .collect();
            crash_point(root, "before-lmdb-write")?;
            database.install(
                &sage_db::InstalledPackage {
                    key: key.clone(),
                    version: version.clone(),
                    arch: source.release.package.arch.clone(),
                    installed_size: source.release.package.installed_size,
                    dependencies: source.release.package.dependencies.clone(),
                    provides: source.release.package.provides.clone(),
                    conflicts: source.release.package.conflicts.clone(),
                    files: ownership.clone(),
                    config_hashes,
                },
                false,
            )?;
            crash_point(root, "lmdb-publication")?;
            if let Some(previous_package) = previous_package {
                for obsolete in previous_package
                    .files
                    .iter()
                    .filter(|path| !ownership.contains(path))
                {
                    if database.owners(obsolete)?.is_empty() {
                        let path = under_root(root, Path::new(obsolete));
                        if !should_preserve_config(
                            &path,
                            obsolete,
                            &previous_package.config_hashes,
                        )? {
                            remove_file_beneath(root, &path)?;
                            modified.push(PathBuf::from(obsolete));
                        }
                    }
                }
            }
            for path in report.sage_new {
                eprintln!("Configuration update requires review: {}", path.display());
            }
        }
        if let Some(work) = &mut rebuild {
            let mut removed = Vec::new();
            for path in &work.removed_paths {
                if database.owners(path)?.is_empty() {
                    removed.push(path.clone());
                }
            }
            work.removed_paths = removed;
        }
        if let sage_db::JournalAction::Install {
            modified_paths,
            rebuild: saved,
            ..
        } = &mut journal.action
        {
            *modified_paths = modified
                .iter()
                .map(|path| path.to_string_lossy().into_owned())
                .collect();
            *saved = rebuild.clone();
        }
        journal.advance("alternatives");
        database.write_journal(journal)?;
    }
    let (previous_alternatives, modified): (_, Vec<PathBuf>) = match &journal.action {
        sage_db::JournalAction::Install {
            previous_alternative_documents,
            modified_paths,
            ..
        } => (
            alternatives_from_documents(previous_alternative_documents)?,
            modified_paths.iter().map(PathBuf::from).collect(),
        ),
        _ => unreachable!(),
    };
    settle_journal_alternatives(root, database, journal, &previous_alternatives)?;
    let triggers = if journal.stage == "triggers" {
        TriggerEngine::load_triggers(root)?
    } else {
        Vec::new()
    };
    settle_journal_triggers(
        root,
        database,
        journal,
        &modified,
        &triggers,
        TriggerEvent::PostChange,
    )?;
    if let Some(work) = &rebuild {
        resume_rebuild(root, database, journal, work)?;
    }
    settle_journal_declaration(root, database, journal)?;
    database.finish_journal(&journal.op_id)?;
    Ok(())
}

/// Resumes an interrupted package removal journal forward to completion.
pub(crate) fn resume_remove(
    root: &Path,
    database: &sage_db::SageDatabase,
    journal: &mut sage_db::JournalRecord,
) -> Result<()> {
    journal.validate()?;
    if journal.stage == "packages" {
        let packages = match &journal.action {
            sage_db::JournalAction::Remove { packages, .. } => packages.clone(),
            _ => bail!("remove recovery received an install journal"),
        };
        let mut modified = Vec::new();
        for package in packages {
            database.remove(&package.key)?;
            crash_point(root, "removal")?;
            for relative in &package.files {
                if !database.owners(relative)?.is_empty() {
                    continue;
                }
                let path = under_root(root, Path::new(relative));
                if should_preserve_config(&path, relative, &package.config_hashes)? {
                    eprintln!("Preserving modified configuration {}", path.display());
                    continue;
                }
                remove_file_beneath(root, &path)?;
                modified.push(PathBuf::from(relative));
                crash_point(root, "remove-after-path")?;
            }
        }
        if let sage_db::JournalAction::Remove { modified_paths, .. } = &mut journal.action {
            *modified_paths = modified
                .iter()
                .map(|path| path.to_string_lossy().into_owned())
                .collect();
        }
        journal.advance("alternatives");
        database.write_journal(journal)?;
    }
    let (previous_alternatives, modified, triggers): (_, Vec<PathBuf>, _) = match &journal.action {
        sage_db::JournalAction::Remove {
            alternative_documents,
            modified_paths,
            trigger_documents,
            ..
        } => (
            alternatives_from_documents(alternative_documents)?,
            modified_paths.iter().map(PathBuf::from).collect(),
            trigger_documents
                .iter()
                .map(|bytes| TriggerSpec::parse(bytes))
                .collect::<Result<Vec<_>, _>>()?,
        ),
        _ => unreachable!(),
    };
    settle_journal_alternatives(root, database, journal, &previous_alternatives)?;
    settle_journal_triggers(
        root,
        database,
        journal,
        &modified,
        &triggers,
        TriggerEvent::PostRemove,
    )?;
    settle_journal_declaration(root, database, journal)?;
    database.finish_journal(&journal.op_id)?;
    Ok(())
}

/// Checks whether an existing configuration file differs from original package hash, indicating administrator edits.
pub(crate) fn should_preserve_config(
    path: &Path,
    physical: &str,
    hashes: &BTreeMap<String, String>,
) -> Result<bool> {
    let Some(expected) = hashes.get(physical) else {
        return Ok(false);
    };
    if !path.exists() {
        return Ok(false);
    }
    let bytes = std::fs::read(path)?;
    Ok(hex::encode(Sha256::digest(bytes)) != *expected)
}

/// Safely removes a single file, verifying that it resides within the sysroot tree.
pub(crate) fn remove_file_beneath(root: &Path, path: &Path) -> Result<()> {
    match std::fs::symlink_metadata(path) {
        Ok(_) => {}
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(()),
        Err(error) => return Err(error.into()),
    }
    let parent = path.parent().context("installed path has no parent")?;
    let canonical_root = std::fs::canonicalize(root)?;
    let canonical_parent = std::fs::canonicalize(parent)?;
    if !canonical_parent.starts_with(canonical_root) {
        bail!(
            "refusing to remove path outside sysroot: {}",
            path.display()
        );
    }
    match std::fs::remove_file(path) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(error.into()),
    }
}

/// Resumes post-publication stages of system rebuild (removal triggers, provider bindings, service generation).
pub(crate) fn resume_rebuild(
    root: &Path,
    database: &sage_db::SageDatabase,
    journal: &mut sage_db::JournalRecord,
    work: &sage_db::RebuildContinuation,
) -> Result<()> {
    if journal.stage == "rebuild-removal-triggers" {
        let triggers = work
            .removal_trigger_documents
            .iter()
            .map(|document| TriggerSpec::parse(document))
            .collect::<Result<Vec<_>, _>>()?;
        let paths = work
            .removed_paths
            .iter()
            .map(PathBuf::from)
            .collect::<Vec<_>>();
        TriggerEngine::execute_triggers_for(&triggers, &paths, root, TriggerEvent::PostRemove)?;
        journal.advance("rebuild-bindings");
        database.write_journal(journal)?;
        crash_point(root, "rebuild-removal-triggers")?;
    }
    if journal.stage == "rebuild-bindings" {
        database.replace_system_providers(&work.provider_bindings)?;
        journal.advance("rebuild-services");
        database.write_journal(journal)?;
        crash_point(root, "rebuild-bindings")?;
    }
    if journal.stage == "rebuild-services" {
        let next: RenderedServicesState =
            toml::from_str(std::str::from_utf8(&work.rendered_services)?)?;
        next.generator.render_service_set(&next.services, root)?;
        for service in &next.services {
            if next.enabled.contains(&service.name) {
                next.generator.enable_service(service, root)?;
            }
        }
        write_atomic_under_root(
            root,
            Path::new("var/lib/sage/rendered-services.toml"),
            &work.rendered_services,
        )?;
        journal.advance("rebuild-triggers");
        database.write_journal(journal)?;
        crash_point(root, "rebuild-services")?;
    }
    if journal.stage == "rebuild-triggers" {
        TriggerEngine::execute_triggers_for(
            &TriggerEngine::load_triggers(root)?,
            &[PathBuf::from("etc/sage/system.toml")],
            root,
            TriggerEvent::Rebuild,
        )?;
        journal.advance(if journal.declaration.is_some() {
            "declaration"
        } else {
            "complete"
        });
        database.write_journal(journal)?;
        crash_point(root, "rebuild-triggers")?;
    }
    Ok(())
}

/// Generates a globally unique, deterministic operation ID for journal tracking.
pub(crate) fn operation_id(kind: &str) -> Result<String> {
    static COUNTER: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)?
        .as_nanos();
    let sequence = COUNTER.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    Ok(format!("{kind}-{}-{nanos}-{sequence}", std::process::id()))
}
