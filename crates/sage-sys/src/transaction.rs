//! Transactional package deployment, system reconciliation, preflight validation, and service planning.

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};
use sage_core::{hex, under_root};
use sha2::{Digest, Sha256};

use crate::channel::{
    AvailablePackages, arch_matches, canonical_channel, load_available_with_pool,
    obtain_release_archive, qualify_channel,
};
use crate::recovery::{
    installed_packages, operation_id, read_documents, read_toml_files, resume_install,
    resume_remove, should_preserve_config, trigger_documents,
};
use crate::services::{
    RenderedServicesState, ServiceDocument, ServiceSpec, TemplateServiceGenerator, target_path,
};
use crate::state::{
    AlternativesDocument, ReconcilePlan, SystemConfig, SysusersDocument, provider_symbol,
};
use crate::triggers::{TriggerEvent, TriggerSpec};

/// Explicit, atomic execution plan for package mutations before journal creation and execution.
///
/// Encapsulates ordered installations/upgrades, retired package removals, and system
/// provider bindings determined during dependency resolution and preflight verification.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TransactionPlan {
    /// Ordered packages to install or upgrade: (PackageKey, Version).
    pub install: Vec<(sage_core::PackageKey, sage_core::Version)>,
    /// Packages to be removed or retired.
    pub remove: Vec<sage_db::InstalledPackage>,
    /// System provider bindings to commit on completion (e.g. init provider).
    pub provider_bindings: BTreeMap<String, sage_core::PackageKey>,
}

impl TransactionPlan {
    /// Creates a complete transaction plan with install, remove, and provider binding actions.
    pub fn new(
        install: Vec<(sage_core::PackageKey, sage_core::Version)>,
        remove: Vec<sage_db::InstalledPackage>,
        provider_bindings: BTreeMap<String, sage_core::PackageKey>,
    ) -> Self {
        Self {
            install,
            remove,
            provider_bindings,
        }
    }

    /// Creates an install-only transaction plan.
    pub fn for_install(install: Vec<(sage_core::PackageKey, sage_core::Version)>) -> Self {
        Self {
            install,
            remove: Vec::new(),
            provider_bindings: BTreeMap::new(),
        }
    }

    /// Creates a removal-only transaction plan.
    pub fn for_remove(remove: Vec<sage_db::InstalledPackage>) -> Self {
        Self {
            install: Vec::new(),
            remove,
            provider_bindings: BTreeMap::new(),
        }
    }

    /// Returns true if the plan contains no install, remove, or binding mutations.
    pub fn is_empty(&self) -> bool {
        self.install.is_empty() && self.remove.is_empty() && self.provider_bindings.is_empty()
    }
}

/// Computes a hash-addressed relative path for package declaration artifacts.
pub(crate) fn declaration_path(dir: &str, key: &sage_core::PackageKey) -> PathBuf {
    let digest = hex::encode(Sha256::digest(key.canonical_id().as_bytes()));
    PathBuf::from(dir).join(format!("{digest}.toml"))
}

/// Resolves dependencies and applies packages to the sysroot transactionally.
pub async fn apply_packages(
    root: &Path,
    names: &[String],
    channel: Option<&str>,
    prefer_latest: bool,
    save: bool,
    dry_run: bool,
) -> Result<()> {
    let config_path = under_root(root, Path::new("/etc/sage/system.toml"));
    let mut config = SystemConfig::load(&config_path)?;
    let mut available = load_available_with_pool(root, Some(&config.system.architecture), None)?;
    let channel = canonical_channel(&available, channel)?;
    let requested: Vec<_> = names
        .iter()
        .map(|name| sage_core::PackageKey::in_channel(&channel, name))
        .collect::<Result<_, _>>()?;
    let db_path = under_root(root, Path::new("/var/lib/sage"));
    let installed = installed_packages(&db_path, dry_run)?;
    available.register_installed(&installed);
    let mut roots: Vec<_> = requested
        .iter()
        .cloned()
        .chain(installed.iter().map(|package| package.key.clone()))
        .collect();
    roots.sort();
    roots.dedup();
    let locks = installed
        .iter()
        .filter(|package| !prefer_latest || !requested.contains(&package.key))
        .map(|package| (package.key.clone(), package.version.clone()));
    let solution = sage_solver::SageSolver::with_locked(&available.universe, locks)
        .prefer_providers(config.provider_preferences("main/system")?)
        .resolve(&roots)?;
    let current: BTreeMap<_, _> = installed
        .iter()
        .map(|package| (package.key.clone(), package.version.clone()))
        .collect();
    let changes = solution
        .into_iter()
        .filter(|(key, version)| current.get(key) != Some(version))
        .collect();
    let changes = installation_order(&available, changes)?;
    let plan = TransactionPlan::for_install(changes);
    for (key, version) in &plan.install {
        println!(
            "{} {} {}",
            if current.contains_key(key) {
                "Upgrade"
            } else {
                "Install"
            },
            key,
            version
        );
    }
    if dry_run {
        return Ok(());
    }
    if !plan.is_empty() {
        let database = sage_db::SageDatabase::open(&db_path)?;
        publish_packages(
            root,
            &database,
            &available,
            &config.system.architecture,
            &plan,
        )
        .await?;
    }
    if save && channel == "main/system" {
        config.packages.extend(names.iter().cloned());
        write_atomic_under_root(
            root,
            Path::new("etc/sage/system.toml"),
            toml::to_string_pretty(&config)?.as_bytes(),
        )?;
    }
    Ok(())
}

/// Orders planned package changes according to dependencies and virtual provides.
pub fn installation_order(
    available: &AvailablePackages,
    changes: BTreeMap<sage_core::PackageKey, sage_core::Version>,
) -> Result<Vec<(sage_core::PackageKey, sage_core::Version)>> {
    let mut pending = changes;
    let mut ordered = Vec::with_capacity(pending.len());
    while !pending.is_empty() {
        let ready = pending.iter().find_map(|(key, version)| {
            let package = &available.releases[&(key.clone(), version.clone())]
                .release
                .package;
            let blocked = package.dependencies.iter().any(|dependency| {
                let is_virtual =
                    dependency.name.starts_with("virtual/") || dependency.name.starts_with("so:");
                pending.iter().any(|(candidate, candidate_version)| {
                    if candidate == key {
                        return false;
                    }
                    if is_virtual {
                        available
                            .releases
                            .get(&(candidate.clone(), candidate_version.clone()))
                            .is_some_and(|source| {
                                source.release.package.provides.contains(&dependency.name)
                            })
                    } else {
                        candidate.name == dependency.name
                            && candidate.slot
                                == dependency
                                    .slot
                                    .as_deref()
                                    .unwrap_or(sage_core::DEFAULT_SLOT)
                            && candidate.channel
                                == dependency.channel.as_deref().unwrap_or(&key.channel)
                    }
                })
            });
            (!blocked).then(|| key.clone())
        });
        // Package publication has no dependency-time lifecycle scripts, so a
        // cycle is safe: break it by canonical key for reproducible results.
        let key = ready.unwrap_or_else(|| pending.keys().next().unwrap().clone());
        ordered.push((key.clone(), pending.remove(&key).unwrap()));
    }
    Ok(ordered)
}

/// Preflights transaction plan and creates an initial journal record before resuming execution.
pub(crate) async fn publish_packages(
    root: &Path,
    database: &sage_db::SageDatabase,
    available: &AvailablePackages,
    architecture: &str,
    plan: &TransactionPlan,
) -> Result<()> {
    let changes = preflight_packages(
        root,
        database,
        available,
        architecture,
        &plan.install,
        &plan.remove,
    )
    .await?;
    let op_id = operation_id("install")?;
    // Recovery must not consult records that installation may already have replaced.
    let previous_packages = changes
        .iter()
        .map(|(key, _)| database.package(key))
        .collect::<Result<Vec<_>, _>>()?
        .into_iter()
        .flatten()
        .collect();
    let mut journal = sage_db::JournalRecord::new(
        op_id,
        "packages",
        sage_db::JournalAction::Install {
            rebuild: None,
            architecture: architecture.into(),
            changes,
            previous_packages,
            modified_paths: Vec::new(),
            previous_alternative_documents: read_documents(
                root,
                Path::new("usr/share/sage/alternatives"),
            )?,
        },
    );
    database.write_journal(&journal)?;
    resume_install(root, database, available, &mut journal, true).await
}

/// Parsed metadata documents embedded within a package inspection.
pub(crate) struct PackageDeclarations {
    pub(crate) entries: Vec<(PathBuf, Vec<u8>)>,
}

impl PackageDeclarations {
    pub(crate) fn parse(
        inspection: &sage_archive::PackageInspection,
        key: &sage_core::PackageKey,
    ) -> Result<Self> {
        let mut entries = Vec::new();
        if let Some(bytes) = inspection.optional.get(".METADATA/service.toml") {
            for service in ServiceDocument::parse(bytes)?.into_services() {
                let path = PathBuf::from(format!("usr/share/sage/services/{}.toml", service.name));
                let doc = ServiceDocument {
                    schema_version: sage_core::SCHEMA_VERSION,
                    service: Some(service),
                    services: Vec::new(),
                };
                entries.push((path, toml::to_string_pretty(&doc)?.into_bytes()));
            }
        }
        if let Some(bytes) = inspection.optional.get(".METADATA/triggers.toml") {
            let trigger = TriggerSpec::parse(bytes)?;
            entries.push((
                PathBuf::from(format!("usr/share/sage/triggers/{}.toml", trigger.name)),
                bytes.clone(),
            ));
        }
        if let Some(bytes) = inspection.optional.get(".METADATA/alternatives.toml") {
            let mut document = AlternativesDocument::parse(bytes)?;
            document.package.clone_from(key);
            entries.push((
                declaration_path("usr/share/sage/alternatives", key),
                toml::to_string_pretty(&document)?.into_bytes(),
            ));
        }
        if let Some(bytes) = inspection.optional.get(".METADATA/sysusers.toml") {
            let mut document = SysusersDocument::parse(bytes)?;
            document.package.clone_from(key);
            entries.push((
                declaration_path("usr/share/sage/sysusers", key),
                toml::to_string_pretty(&document)?.into_bytes(),
            ));
        }
        Ok(Self { entries })
    }

    pub(crate) fn ownership_paths(&self) -> impl Iterator<Item = String> + '_ {
        self.entries
            .iter()
            .map(|(path, _)| path.to_string_lossy().into_owned())
    }

    pub(crate) fn write_under_root(&self, root: &Path) -> Result<()> {
        for (path, bytes) in &self.entries {
            write_atomic_under_root(root, path, bytes)?;
        }
        Ok(())
    }
}

/// Collects physical files and declarative metadata paths claimed by a package.
pub(crate) fn package_ownership(
    prefix: &Path,
    files: &[sage_archive::FileRecord],
    declarations: &PackageDeclarations,
) -> Vec<String> {
    files
        .iter()
        .map(|record| prefix.join(&record.path).to_string_lossy().into_owned())
        .chain(declarations.ownership_paths())
        .collect()
}

/// Preflights candidates, inspecting archives, verifying checksums, and catching file conflicts.
pub(crate) async fn preflight_packages(
    root: &Path,
    database: &sage_db::SageDatabase,
    available: &AvailablePackages,
    architecture: &str,
    changes: &[(sage_core::PackageKey, sage_core::Version)],
    retired: &[sage_db::InstalledPackage],
) -> Result<Vec<(sage_core::PackageKey, sage_core::Version)>> {
    let package_cache = under_root(root, Path::new("/var/cache/sage/packages"));
    let engine = sage_repo::DownloadEngine::new(&package_cache)?;
    let mut planned = BTreeMap::<String, sage_core::PackageKey>::new();
    let mut final_paths = BTreeMap::<sage_core::PackageKey, BTreeSet<String>>::new();
    for (key, version) in changes {
        let source = available
            .releases
            .get(&(key.clone(), version.clone()))
            .with_context(|| format!("index record disappeared for {key} {version}"))?;
        let archive = obtain_release_archive(&engine, &package_cache, source).await?;
        let inspection = sage_archive::inspect_package(&archive)?;
        let coordinate = inspection.manifest.coordinate_for_channel(&key.channel);
        if coordinate.key != *key || coordinate.version != *version {
            bail!(
                "archive identity {} {} does not match selected {} {}",
                coordinate.key,
                coordinate.version,
                key,
                version
            );
        }
        if !arch_matches(&inspection.manifest.arch, architecture) {
            bail!(
                "package {} has architecture {}, expected {}",
                key,
                inspection.manifest.arch,
                architecture
            );
        }
        sage_archive::validate_package_payload(&archive, &inspection.files)?;
        let prefix = source
            .target_root
            .strip_prefix("/")
            .unwrap_or(&source.target_root);
        let declarations = PackageDeclarations::parse(&inspection, key)?;
        let ownership = package_ownership(prefix, &inspection.files, &declarations);
        for path in &ownership {
            if let Some(owner) = planned.insert(path.clone(), key.clone()) {
                bail!("transaction packages {owner} and {key} both own {path}");
            }
        }
        final_paths.insert(key.clone(), ownership.into_iter().collect());
    }

    // Reject hierarchy replacements before journaling. Publishing cannot create
    // a directory below a retained file, and recovery must not discover that late.
    let mut installed_paths = BTreeMap::<String, BTreeSet<sage_core::PackageKey>>::new();
    for package in database.packages()? {
        for path in package.files {
            installed_paths
                .entry(path)
                .or_default()
                .insert(package.key.clone());
        }
    }
    // Only paths that retirement really deletes may become directories. Shared
    // retained ownership and preserved administrator configuration still block
    // a handoff, even when one of their owners is in the retirement set.
    let mut retiring_paths = BTreeSet::new();
    for (path, owners) in &installed_paths {
        if owners
            .iter()
            .all(|owner| retired.iter().any(|package| package.key == *owner))
        {
            let mut removable = true;
            for package in retired
                .iter()
                .filter(|package| owners.contains(&package.key))
            {
                if should_preserve_config(&root.join(path), path, &package.config_hashes)? {
                    removable = false;
                    break;
                }
            }
            if removable {
                retiring_paths.insert(path.clone());
            }
        }
    }
    for (path, claimant) in &planned {
        let components = Path::new(path)
            .components()
            .filter_map(|component| match component {
                std::path::Component::Normal(name) => Some(name),
                _ => None,
            })
            .collect::<Vec<_>>();
        let mut destination = root.to_path_buf();
        for (index, component) in components.iter().enumerate() {
            destination.push(component);
            match std::fs::symlink_metadata(&destination) {
                Ok(metadata) if index + 1 < components.len() && !metadata.is_dir() => {
                    if retiring_paths
                        .contains(destination.strip_prefix(root)?.to_string_lossy().as_ref())
                    {
                        // Retirement unlinks this ancestor before extraction. Do
                        // not walk into a stale file or follow its symlink target.
                        break;
                    }
                    bail!("file hierarchy conflict for {path}: an ancestor is not a directory")
                }
                Ok(metadata) if index + 1 == components.len() && metadata.is_dir() => {
                    bail!("file conflict for {path}: destination is a directory")
                }
                Ok(_) => {}
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => break,
                Err(error) => return Err(error.into()),
            }
        }
        for ancestor in Path::new(path).ancestors().skip(1) {
            if ancestor.as_os_str().is_empty() {
                break;
            }
            let ancestor = ancestor.to_string_lossy();
            if let Some(owner) = planned.get(ancestor.as_ref()) {
                bail!(
                    "transaction ownership paths conflict: {owner} owns ancestor {ancestor} of {claimant}'s {path}"
                );
            }
            if let Some(owners) = installed_paths.get(ancestor.as_ref())
                && !retiring_paths.contains(ancestor.as_ref())
            {
                bail!(
                    "file hierarchy conflict for {path}: ancestor {ancestor} is currently owned by {owners:?}"
                );
            }
        }
        let descendant_prefix = format!("{path}/");
        if let Some((descendant, owners)) = installed_paths
            .range(descendant_prefix.clone()..)
            .next()
            .filter(|(descendant, _)| descendant.starts_with(&descendant_prefix))
        {
            bail!(
                "file hierarchy conflict for {path}: descendant {descendant} is currently owned by {owners:?}"
            );
        }
    }

    // A current owner may release a path in this same transaction. Add a
    // publication edge so its replacement commits before the new claimant;
    // owners absent from the plan, or retaining the path, remain conflicts.
    let mut successors = changes
        .iter()
        .map(|(key, _)| (key.clone(), BTreeSet::new()))
        .collect::<BTreeMap<_, _>>();
    let mut indegree = changes
        .iter()
        .map(|(key, _)| (key.clone(), 0_usize))
        .collect::<BTreeMap<_, _>>();
    for (path, claimant) in &planned {
        for owner in database.owners(path)? {
            if owner == *claimant || retired.iter().any(|package| package.key == owner) {
                continue;
            }
            let releases = final_paths
                .get(&owner)
                .is_some_and(|paths| !paths.contains(path));
            if !releases {
                bail!("file conflict for {path}: currently owned by {owner}");
            }
            if successors
                .get_mut(&owner)
                .is_some_and(|targets| targets.insert(claimant.clone()))
            {
                *indegree
                    .get_mut(claimant)
                    .expect("planned claimant has an indegree") += 1;
            }
        }
    }

    let positions = changes
        .iter()
        .enumerate()
        .map(|(index, (key, _))| (key.clone(), index))
        .collect::<BTreeMap<_, _>>();
    let versions = changes.iter().cloned().collect::<BTreeMap<_, _>>();
    let mut ready = indegree
        .iter()
        .filter(|(_, degree)| **degree == 0)
        .map(|(key, _)| (positions[key], key.clone()))
        .collect::<BTreeSet<_>>();
    let mut ordered = Vec::with_capacity(changes.len());
    while let Some((_, key)) = ready.pop_first() {
        ordered.push((key.clone(), versions[&key].clone()));
        for claimant in &successors[&key] {
            let degree = indegree
                .get_mut(claimant)
                .expect("planned claimant has an indegree");
            *degree -= 1;
            if *degree == 0 {
                ready.insert((positions[claimant], claimant.clone()));
            }
        }
    }
    if ordered.len() != changes.len() {
        bail!("cyclic file ownership handoff in package transaction");
    }
    Ok(ordered)
}

/// Upgrades specified packages or all installed packages within a channel to their latest versions.
pub async fn upgrade_packages(
    root: &Path,
    names: &[String],
    channel: Option<&str>,
    dry_run: bool,
) -> Result<()> {
    let config = SystemConfig::load(under_root(root, Path::new("/etc/sage/system.toml")))?;
    let available = load_available_with_pool(root, Some(&config.system.architecture), None)?;
    let canonical = canonical_channel(&available, channel)?;
    let names = if names.is_empty() {
        let db_path = under_root(root, Path::new("/var/lib/sage"));
        installed_packages(&db_path, dry_run)?
            .into_iter()
            .filter(|package| package.key.channel == canonical)
            .map(|package| format!("{}:{}", package.key.name, package.key.slot))
            .collect()
    } else {
        names.to_vec()
    };
    apply_packages(root, &names, Some(&canonical), true, false, dry_run).await
}

/// Removes specified packages transactionally after dependency and provider validation.
pub fn remove_packages(
    root: &Path,
    names: &[String],
    channel: Option<&str>,
    update_saved: bool,
    dry_run: bool,
) -> Result<()> {
    let db_path = under_root(root, Path::new("/var/lib/sage"));
    let installed = sage_db::read_packages(&db_path)?;
    let canonical = channel.map_or_else(|| "main/system".into(), qualify_channel);
    let requested = names
        .iter()
        .map(|selector| sage_core::PackageKey::in_channel(&canonical, selector))
        .collect::<Result<Vec<_>, _>>()?;
    let selected: Vec<_> = installed
        .iter()
        .filter(|package| requested.contains(&package.key))
        .cloned()
        .collect();
    if selected.len() != names.len() {
        bail!("one or more requested packages are not installed in {canonical}");
    }
    for (interface, provider) in sage_db::read_system_providers(&db_path)? {
        if requested.contains(&provider) {
            bail!(
                "cannot remove bound provider {provider} for {}; switch providers with rebuild first",
                provider_symbol(&interface)
            );
        }
    }
    for dependent in &installed {
        if selected.iter().any(|removed| {
            dependent.key != removed.key
                && dependent.dependencies.iter().any(|dependency| {
                    let virtual_dependency = dependency.name.starts_with("virtual/")
                        || dependency.name.starts_with("so:");
                    let removed_direct = dependency.name == removed.key.name;
                    let matches_pkg =
                        |pkg: &sage_db::InstalledPackage, direct_only_non_virtual: bool| {
                            let direct = dependency.name == pkg.key.name;
                            let provides_ok = direct
                                || (!direct_only_non_virtual
                                    && pkg.provides.contains(&dependency.name));
                            let slot_ok = dependency.slot.as_deref().map_or_else(
                                || {
                                    virtual_dependency
                                        || !direct
                                        || pkg.key.slot == sage_core::DEFAULT_SLOT
                                },
                                |slot| slot == pkg.key.slot,
                            );
                            let channel_ok = dependency
                                .channel
                                .as_deref()
                                .is_none_or(|channel| channel == pkg.key.channel)
                                && pkg.key.channel == removed.key.channel;
                            provides_ok && slot_ok && channel_ok
                        };
                    let matched = matches_pkg(removed, false);
                    let replacement = installed.iter().any(|candidate| {
                        !selected.iter().any(|removed| removed.key == candidate.key)
                            && matches_pkg(candidate, removed_direct && !virtual_dependency)
                            && dependency
                                .op
                                .matches(&candidate.version, dependency.version.as_ref())
                    });
                    matched && !replacement
                })
        }) {
            bail!("cannot remove packages required by {}", dependent.key);
        }
    }
    let plan = TransactionPlan::for_remove(selected);
    for package in &plan.remove {
        println!("Remove {} {}", package.key, package.version);
    }
    if dry_run {
        return Ok(());
    }
    if update_saved && canonical == "main/system" {
        let config_path = under_root(root, Path::new("/etc/sage/system.toml"));
        let mut config = SystemConfig::load(&config_path)?;
        config.packages.retain(|selector| {
            sage_core::PackageKey::in_channel(&canonical, selector)
                .map_or(true, |key| !requested.contains(&key))
        });
        write_atomic_under_root(
            root,
            Path::new("etc/sage/system.toml"),
            toml::to_string_pretty(&config)?.as_bytes(),
        )?;
    }
    let database = sage_db::SageDatabase::open(&db_path)?;
    let op_id = operation_id("remove")?;
    let mut journal = sage_db::JournalRecord::new(
        op_id,
        "packages",
        sage_db::JournalAction::Remove {
            packages: plan.remove,
            modified_paths: Vec::new(),
            trigger_documents: trigger_documents(root)?,
            alternative_documents: read_documents(root, Path::new("usr/share/sage/alternatives"))?,
        },
    );
    database.write_journal(&journal)?;
    resume_remove(root, &database, &mut journal)
}

/// Atomically writes content to a target path beneath sysroot using tempfile and rename.
pub(crate) fn write_atomic_under_root(root: &Path, relative: &Path, bytes: &[u8]) -> Result<()> {
    if relative.components().any(|component| {
        matches!(
            component,
            std::path::Component::ParentDir | std::path::Component::RootDir
        )
    }) {
        bail!("unsafe state path {}", relative.display());
    }
    let target = root.join(relative);
    let parent = target.parent().context("state path has no parent")?;
    std::fs::create_dir_all(parent)?;
    let canonical_root = std::fs::canonicalize(root)?;
    let canonical_parent = std::fs::canonicalize(parent)?;
    if !canonical_parent.starts_with(canonical_root) {
        bail!("state path escapes sysroot: {}", target.display());
    }
    let temporary = parent.join(format!(".sage-state-{}", std::process::id()));
    let mut options = std::fs::OpenOptions::new();
    use std::io::Write as _;
    options
        .write(true)
        .create_new(true)
        .open(&temporary)?
        .write_all(bytes)?;
    std::fs::rename(temporary, target)?;
    Ok(())
}

/// Checks a program against the final payload/retained-file overlay without publishing it.
pub(crate) fn validate_planned_program(
    root: &Path,
    program: &Path,
    payloads: &BTreeMap<PathBuf, (PathBuf, PathBuf, u32)>,
    removed: &BTreeSet<PathBuf>,
) -> Result<PathBuf> {
    use std::os::unix::fs::PermissionsExt as _;
    let mut pending = program
        .strip_prefix(root)?
        .components()
        .map(|component| PathBuf::from(component.as_os_str()))
        .collect::<std::collections::VecDeque<_>>();
    let mut relative = PathBuf::new();
    let mut links = BTreeSet::new();
    while let Some(component) = pending.pop_front() {
        if component == Path::new(".") {
            continue;
        }
        if component == Path::new("..") {
            if !relative.pop() {
                bail!("program escapes sysroot: {}", program.display());
            }
            continue;
        }
        relative.push(component);
        let planned_directory = payloads
            .keys()
            .any(|path| path != &relative && path.starts_with(&relative));
        if removed.contains(&relative) && !planned_directory {
            bail!(
                "planned program {} uses removed path {}",
                program.display(),
                relative.display()
            );
        }
        let (link, regular, mode) = if let Some((archive, path, mode)) = payloads.get(&relative) {
            (
                sage_archive::payload_link_target(archive, path)?,
                true,
                *mode,
            )
        } else if planned_directory {
            // Payload ancestors are created by extraction, even on a fresh root.
            (None, false, 0)
        } else {
            let path = root.join(&relative);
            let metadata = std::fs::symlink_metadata(&path)
                .with_context(|| format!("missing planned program {}", program.display()))?;
            (
                metadata
                    .is_symlink()
                    .then(|| std::fs::read_link(&path))
                    .transpose()?,
                metadata.is_file(),
                metadata.permissions().mode(),
            )
        };
        if let Some(link) = link {
            if link.is_absolute() || !links.insert(relative.clone()) {
                bail!("unsafe program symlink {}", relative.display());
            }
            relative.pop();
            for component in link.components().rev() {
                pending.push_front(PathBuf::from(component.as_os_str()));
            }
        } else if pending.is_empty() {
            if !regular || mode & 0o111 == 0 {
                bail!("planned program is not executable: {}", program.display());
            }
            return Ok(root.join(relative));
        } else if regular {
            bail!(
                "program ancestor is not a directory: {}",
                relative.display()
            );
        }
    }
    bail!(
        "planned program is not a regular file: {}",
        program.display()
    )
}

/// Plans the complete transition before publication, then journals its service
/// generation alongside package changes for config-independent forward recovery.
pub async fn rebuild_system(root: &Path, no_prune: bool, dry_run: bool) -> Result<()> {
    let config = SystemConfig::load(root.join("etc/sage/system.toml"))?;
    let mut available = load_available_with_pool(root, Some(&config.system.architecture), None)?;
    let db_path = root.join("var/lib/sage");
    let installed = installed_packages(&db_path, dry_run)?;
    available.register_installed(&installed);
    let plan = ReconcilePlan::compute(&config, &installed, &available.universe, no_prune)?;
    for (interface, key) in &plan.provider_bindings {
        let version = plan
            .install
            .iter()
            .find(|(candidate, _)| candidate == key)
            .map(|(_, version)| version)
            .or_else(|| {
                installed
                    .iter()
                    .find(|package| package.key == *key)
                    .map(|package| &package.version)
            })
            .with_context(|| format!("resolved provider {key} is absent from the plan"))?;
        let release = available
            .universe
            .release(key, version)
            .with_context(|| format!("resolved provider release {key} {version} is missing"))?;
        let symbol = provider_symbol(interface);
        if !release.provides.contains(&symbol) {
            bail!("resolved provider {key} {version} does not provide {symbol}");
        }
    }
    let provider = plan
        .provider_bindings
        .get("init")
        .cloned()
        .context("system providers must resolve an init implementation")?;
    let changes = installation_order(&available, plan.install.into_iter().collect())?;
    let retired = installed
        .iter()
        .filter(|package| plan.remove.contains(&package.key))
        .cloned()
        .collect::<Vec<_>>();
    let tx_plan = TransactionPlan::new(changes, retired, plan.provider_bindings);
    for (key, version) in &tx_plan.install {
        println!("Install {key} {version}");
    }
    for package in &tx_plan.remove {
        println!("Remove {} {}", package.key, package.version);
    }
    for (interface, key) in &tx_plan.provider_bindings {
        println!("Bind {} to {key}", provider_symbol(interface));
    }
    // A dry-run remains useful before a new provider has been downloaded or
    // installed. Solving and binding validation still happen without writes.
    if dry_run {
        return Ok(());
    }
    let database = sage_db::SageDatabase::open(&db_path)?;
    let changes = preflight_packages(
        root,
        &database,
        &available,
        &config.system.architecture,
        &tx_plan.install,
        &tx_plan.remove,
    )
    .await?;
    let mut work = sage_db::RebuildContinuation {
        provider_bindings: tx_plan.provider_bindings,
        retired_packages: tx_plan.remove,
        removed_paths: Vec::new(),
        removal_trigger_documents: trigger_documents(root)?,
        rendered_services: Vec::new(),
    };
    let next = plan_services(
        root, &config, &provider, &available, &changes, &installed, &work,
    )
    .await?;
    work.rendered_services = toml::to_string(&next)?.into_bytes();
    // The package preflight and service preflight above form the trusted input
    // boundary. Do not repeat payload validation when publishing this plan.
    let previous_packages = installed
        .into_iter()
        .filter(|package| changes.iter().any(|(key, _)| *key == package.key))
        .collect();
    let mut journal = sage_db::JournalRecord::new(
        operation_id("rebuild")?,
        "rebuild-cleanup",
        sage_db::JournalAction::Install {
            architecture: config.system.architecture,
            changes,
            previous_packages,
            modified_paths: Vec::new(),
            previous_alternative_documents: read_documents(
                root,
                Path::new("usr/share/sage/alternatives"),
            )?,
            rebuild: Some(work),
        },
    );
    database.write_journal(&journal)?;
    resume_install(root, &database, &available, &mut journal, true).await
}

/// Resolves renderer inputs and executables against the final package overlay.
/// Files owned by replaced or retired releases cannot masquerade as replacements.
async fn plan_services(
    root: &Path,
    config: &SystemConfig,
    provider: &sage_core::PackageKey,
    available: &AvailablePackages,
    changes: &[(sage_core::PackageKey, sage_core::Version)],
    installed: &[sage_db::InstalledPackage],
    work: &sage_db::RebuildContinuation,
) -> Result<RenderedServicesState> {
    let retired = &work.retired_packages;
    let cache = root.join("var/cache/sage/packages");
    let engine = sage_repo::DownloadEngine::new(&cache)?;
    let mut payloads = BTreeMap::new();
    let mut documents = BTreeMap::new();
    let mut removed = installed
        .iter()
        .filter(|package| {
            changes.iter().any(|(key, _)| *key == package.key)
                || retired.iter().any(|old| old.key == package.key)
        })
        .flat_map(|package| package.files.iter().map(PathBuf::from))
        .collect::<BTreeSet<_>>();
    for (key, version) in changes {
        let source = &available.releases[&(key.clone(), version.clone())];
        let archive = obtain_release_archive(&engine, &cache, source).await?;
        let inspection = sage_archive::inspect_package(&archive)?;
        let prefix = source
            .target_root
            .strip_prefix("/")
            .unwrap_or(&source.target_root);
        for record in &inspection.files {
            let path = prefix.join(&record.path);
            removed.remove(&path);
            payloads.insert(path, (archive.clone(), record.path.clone(), record.mode));
        }
        for (path, bytes) in PackageDeclarations::parse(&inspection, key)?.entries {
            removed.remove(&path);
            documents.insert(path, bytes);
        }
    }
    let renderer_path = PathBuf::from(format!("usr/share/sage/rclass/init-{}.toml", provider.name));
    let read_final = |path: &Path| -> Result<Vec<u8>> {
        if let Some((archive, relative, _)) = payloads.get(path) {
            Ok(sage_archive::read_payload_file(archive, relative)?)
        } else if let Some(bytes) = documents.get(path) {
            Ok(bytes.clone())
        } else {
            if removed.contains(path) {
                bail!("planned service input is removed: {}", path.display());
            }
            Ok(fs::read(root.join(path))
                .with_context(|| format!("missing planned service input {}", path.display()))?)
        }
    };
    let generator = TemplateServiceGenerator::parse(&read_final(&renderer_path)?)?;
    let service_dir = Path::new("usr/share/sage/services");
    let paths = read_toml_files(&root.join(service_dir))?
        .into_iter()
        .map(|path| {
            path.strip_prefix(root)
                .map(Path::to_path_buf)
                .map_err(Into::into)
        })
        .collect::<Result<BTreeSet<_>>>()?;
    let paths = paths
        .into_iter()
        .filter(|path| !removed.contains(path))
        .chain(
            payloads
                .keys()
                .chain(documents.keys())
                .filter(|path| {
                    path.parent() == Some(service_dir)
                        && path
                            .extension()
                            .is_some_and(|extension| extension == "toml")
                })
                .cloned(),
        )
        .collect::<BTreeSet<_>>();
    let services = paths
        .iter()
        .map(|path| Ok(ServiceSpec::parse(&read_final(path)?)?))
        .collect::<Result<Vec<_>>>()?;
    let names = services
        .iter()
        .map(|service| &service.name)
        .collect::<BTreeSet<_>>();
    if names.len() != services.len() {
        bail!("duplicate installed service names");
    }
    if let Some(name) = config.services.iter().find(|name| !names.contains(name)) {
        bail!("enabled service '{name}' has no planned declaration");
    }
    generator.validate_service_set(&services, root)?;
    // Removed package ownership does not imply physical removal: administrator
    // configuration edits survive retirement and obsolete-path cleanup. Protect
    // those files before accepting a native output or managed directory.
    let mut preserved_configs = BTreeSet::new();
    for package in installed {
        for path in package.config_hashes.keys() {
            if removed.contains(Path::new(path))
                && should_preserve_config(&root.join(path), path, &package.config_hashes)?
            {
                preserved_configs.insert(PathBuf::from(path));
            }
        }
    }
    let protected_paths = installed
        .iter()
        .filter(|package| {
            !changes.iter().any(|(key, _)| *key == package.key)
                && !retired.iter().any(|old| old.key == package.key)
        })
        .flat_map(|package| package.files.iter().map(PathBuf::from))
        .chain(payloads.keys().chain(documents.keys()).cloned())
        .chain(preserved_configs.iter().cloned())
        .collect::<BTreeSet<_>>();
    let mut targets = BTreeSet::<PathBuf>::new();
    for service in &services {
        let target = generator.rendered_path(service, root)?;
        let relative = target.strip_prefix(root)?;
        // Native output must not replace a package's command, data, or parent
        // directory. This checks the final overlay, before any old cleanup.
        if protected_paths
            .iter()
            .any(|path| path.starts_with(relative) || relative.starts_with(path))
        {
            bail!(
                "native service output conflicts with an existing or planned file: {}",
                target.display()
            );
        }
        if targets
            .iter()
            .any(|path| path.starts_with(&target) || target.starts_with(path))
        {
            bail!("native service outputs overlap: {}", target.display());
        }
        targets.insert(target);
    }
    let managed_directory = generator
        .managed_directory
        .as_deref()
        .map(|directory| target_path(root, Path::new(directory)))
        .transpose()?;
    if let Some(directory) = &managed_directory {
        let relative = directory.strip_prefix(root)?;
        if protected_paths
            .iter()
            .any(|path| path.starts_with(relative) || relative.starts_with(path))
        {
            bail!(
                "managed service directory conflicts with an existing or planned file: {}",
                directory.display()
            );
        }
    }
    // A managed directory is published even for an empty service set. Check
    // its ancestors independently, using the same no-write filesystem walk.
    for (target, directory) in targets
        .iter()
        .map(|path| (path, false))
        .chain(managed_directory.iter().map(|path| (path, true)))
    {
        let mut physical = root.to_path_buf();
        for component in target.strip_prefix(root)?.components() {
            physical.push(component);
            match fs::symlink_metadata(&physical) {
                Ok(metadata)
                    if (physical != *target || directory)
                        && (!metadata.is_dir() || metadata.is_symlink()) =>
                {
                    let relative = physical.strip_prefix(root)?;
                    if removed.contains(relative) {
                        // This walk already identifies a required native
                        // directory, including an empty managed generation.
                        // Surviving owners and preserved configs were rejected
                        // above; publication removes this obsolete leaf first.
                        break;
                    }
                    bail!(
                        "unsafe native service output directory: {}",
                        physical.display()
                    );
                }
                Ok(metadata) if physical == *target && !directory && metadata.is_dir() => {
                    bail!("native service output is a directory: {}", target.display());
                }
                Ok(_) => {}
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => break,
                Err(error) => return Err(error.into()),
            }
        }
    }
    for program in generator.required_programs(&services, &config.services, root)? {
        let resolved = validate_planned_program(root, &program, &payloads, &removed)?;
        if targets.contains(&resolved)
            || managed_directory
                .as_ref()
                .is_some_and(|path| resolved.starts_with(path))
        {
            bail!(
                "native service output replaces a required program: {}",
                resolved.display()
            );
        }
    }
    // Preserve PostRemove for whole packages pruned by rebuild. Upgrade-only
    // obsolete paths never enter this set. Reject a captured mandatory command
    // that cannot exist at the post-publication execution point.
    let mut retirement_paths = Vec::new();
    for package in retired {
        for path in &package.files {
            if removed.contains(Path::new(path))
                && !should_preserve_config(&root.join(path), path, &package.config_hashes)?
            {
                retirement_paths.push(PathBuf::from(path));
            }
        }
    }
    for document in &work.removal_trigger_documents {
        let trigger = TriggerSpec::parse(document)?;
        for command in trigger.commands_for(&retirement_paths, root, TriggerEvent::PostRemove)? {
            if !trigger.ignore_missing_binary {
                let program = target_path(root, Path::new(&command[0]))?;
                let resolved = validate_planned_program(root, &program, &payloads, &removed)?;
                if targets.contains(&resolved)
                    || managed_directory
                        .as_ref()
                        .is_some_and(|path| resolved.starts_with(path))
                {
                    bail!(
                        "native service output replaces removal trigger program: {}",
                        resolved.display()
                    );
                }
            }
        }
    }
    let previous_path = root.join("var/lib/sage/rendered-services.toml");
    if previous_path.exists() {
        let previous = RenderedServicesState::load(previous_path)?;
        for service in previous
            .services
            .iter()
            .filter(|service| previous.enabled.contains(&service.name))
        {
            if let Some(program) = previous.generator.disable_program(service, root)? {
                validate_planned_program(root, &program, &BTreeMap::new(), &BTreeSet::new())?;
            }
        }
    }
    Ok(RenderedServicesState {
        schema_version: sage_core::SCHEMA_VERSION,
        provider: provider.clone(),
        generator,
        services,
        enabled: config.services.clone(),
    })
}

/// Removes stale definitions while every old command and runtime file survives.
pub(crate) fn cleanup_services(
    root: &Path,
    next: &RenderedServicesState,
    packages_change: bool,
) -> Result<()> {
    let relative = Path::new("var/lib/sage/rendered-services.toml");
    let path = root.join(relative);
    if !path.exists() {
        return Ok(());
    }
    let mut previous = RenderedServicesState::load(path)?;
    let replace = packages_change
        || previous.provider != next.provider
        || previous.generator != next.generator;
    for service in previous.services.clone() {
        let stale = replace
            || !next
                .services
                .iter()
                .any(|candidate| candidate.name == service.name);
        if previous.enabled.contains(&service.name)
            && (stale || !next.enabled.contains(&service.name))
        {
            previous.generator.disable_service(&service, root)?;
            previous.enabled.remove(&service.name);
            write_atomic_under_root(root, relative, toml::to_string(&previous)?.as_bytes())?;
        }
        if stale {
            previous.generator.remove_service(&service, root)?;
            previous
                .services
                .retain(|candidate| candidate.name != service.name);
            write_atomic_under_root(root, relative, toml::to_string(&previous)?.as_bytes())?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod package_ops_tests {
    use super::*;

    #[test]
    fn planned_program_links_resolve_in_the_final_filesystem() {
        use std::os::unix::{fs::PermissionsExt as _, fs::symlink};
        let root = tempfile::tempdir().unwrap();
        let stage = tempfile::tempdir().unwrap();
        let data = stage.path().join("data");
        std::fs::create_dir_all(data.join("usr/bin")).unwrap();
        std::fs::create_dir_all(stage.path().join(".METADATA")).unwrap();
        std::fs::write(data.join("usr/bin/real"), b"#!/bin/sh\nexit 0\n").unwrap();
        std::fs::set_permissions(
            data.join("usr/bin/real"),
            std::fs::Permissions::from_mode(0o755),
        )
        .unwrap();
        symlink("real", data.join("usr/bin/ctl")).unwrap();
        std::fs::write(
            stage.path().join(".METADATA/manifest.toml"),
            "schema_version=1\nname=\"program\"\nversion=\"1\"\nrelease=1\narch=\"noarch\"\nchannel=\"system\"\ndescription=\"Program\"\nlicense=\"MIT\"\n",
        )
        .unwrap();
        let records = sage_archive::build_file_index(&data).unwrap();
        std::fs::write(
            stage.path().join(".METADATA/files.idx"),
            sage_archive::format_file_index(&records),
        )
        .unwrap();
        let archive = stage.path().join("program.pkg.tar.zst");
        sage_archive::create_package(stage.path(), &archive, 1).unwrap();
        sage_archive::validate_package_payload(&archive, &records).unwrap();
        let mut payloads = records
            .into_iter()
            .map(|record| {
                (
                    record.path.clone(),
                    (archive.clone(), record.path, record.mode),
                )
            })
            .collect::<BTreeMap<_, _>>();
        let program = root.path().join("usr/bin/ctl");
        validate_planned_program(root.path(), &program, &payloads, &BTreeSet::new()).unwrap();
        payloads.remove(Path::new("usr/bin/real"));
        let removed = BTreeSet::from([PathBuf::from("usr/bin/real")]);
        assert!(validate_planned_program(root.path(), &program, &payloads, &removed).is_err());
        std::fs::create_dir_all(root.path().join("usr/bin")).unwrap();
        symlink("real", &program).unwrap();
        std::fs::copy(data.join("usr/bin/real"), root.path().join("usr/bin/real")).unwrap();
        validate_planned_program(root.path(), &program, &BTreeMap::new(), &BTreeSet::new())
            .unwrap();
        assert!(
            validate_planned_program(root.path(), &program, &BTreeMap::new(), &removed).is_err()
        );
        std::fs::remove_file(&program).unwrap();
        symlink(data.join("usr/bin/real"), &program).unwrap();
        assert!(
            validate_planned_program(root.path(), &program, &BTreeMap::new(), &BTreeSet::new())
                .is_err()
        );
    }
}
