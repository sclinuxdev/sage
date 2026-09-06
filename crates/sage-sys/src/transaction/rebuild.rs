//! Whole-system reconciliation, declarative init rebuild, and service configuration generation.

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};

use super::package::{PackageDeclarations, installation_order};
use super::plan::TransactionPlan;
use super::preflight::{preflight_packages, validate_planned_program, write_atomic_under_root};
use crate::channel::{AvailablePackages, load_available_with_pool, obtain_release_archive};
use crate::recovery::{
    installed_packages, operation_id, read_documents, read_toml_files, resume_install,
    should_preserve_config, trigger_documents,
};
use crate::services::{
    RenderedServicesState, ServiceSpec, ServicesConfig, TemplateServiceGenerator, target_path,
};
use crate::state::{ReconcilePlan, SystemConfig, provider_symbol};
use crate::triggers::{TriggerEvent, TriggerSpec};

/// Plans the complete transition before publication, then journals its service
/// generation alongside package changes for config-independent forward recovery.
pub async fn rebuild_system(root: &Path, no_prune: bool, dry_run: bool) -> Result<()> {
    let config = SystemConfig::load(root.join("etc/sage/system.toml"))?;
    let services_config = ServicesConfig::load(root.join("etc/sage/services.toml"))?;
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
    if dry_run {
        // A dry run may inspect a planned provider that is not installed yet,
        // but malformed installed service state must still fail loudly.
        let services = crate::services::load_available_services(root)?;
        if root.join("var/lib/sage/rendered-services.toml").is_file() {
            let (provider_name, generator) = crate::services::load_active_generator(root)?;
            let drifts = crate::services::detect_service_drift(
                root,
                &generator,
                &provider_name,
                &services,
                &services_config.enabled,
                &services_config.disabled,
            );
            if !drifts.is_empty() {
                crate::services::warn_service_drift(&drifts, Some(&generator), root);
            }
        }
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
        root,
        &services_config.enabled,
        &provider,
        &available,
        &changes,
        &installed,
        &work,
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
    resume_install(root, &database, &available, &mut journal, true).await?;
    let drifts = crate::services::detect_service_drift(
        root,
        &next.generator,
        &provider.name,
        &next.services,
        &services_config.enabled,
        &services_config.disabled,
    );
    if !drifts.is_empty() {
        crate::services::warn_service_drift(&drifts, Some(&next.generator), root);
    }
    Ok(())
}

/// Resolves renderer inputs and executables against the final package overlay.
/// Files owned by replaced or retired releases cannot masquerade as replacements.
pub(crate) async fn plan_services(
    root: &Path,
    enabled_services: &BTreeSet<String>,
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
    if let Some(name) = enabled_services.iter().find(|name| !names.contains(name)) {
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
    for program in generator.required_programs(&services, enabled_services, root)? {
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
        enabled: enabled_services.clone(),
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
