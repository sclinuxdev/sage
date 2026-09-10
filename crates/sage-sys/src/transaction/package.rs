//! Package deployment, removal, upgrades, and publication operations.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{Result, bail};
use sage_core::under_root;

use super::plan::TransactionPlan;
use super::preflight::{declaration_path, preflight_packages, write_atomic_under_root};
use super::providers::resolve_virtual_selections;
use crate::channel::{
    AvailablePackages, canonical_channel, load_available_with_pool, qualify_channel,
};
use crate::recovery::{
    installed_packages, operation_id, read_documents, resume_install, resume_remove,
    trigger_documents,
};
use crate::services::ServiceDocument;
use crate::state::{AlternativesDocument, SystemConfig, SysusersDocument, provider_symbol};
use crate::triggers::TriggerSpec;

/// Prompts the user or selects a provider when multiple providers exist for an unconfigured virtual interface.
pub fn select_virtual_provider(
    symbol: &str,
    candidates: &[sage_core::PackageKey],
    cli_override: Option<&str>,
    interactive: bool,
) -> Result<sage_core::PackageKey> {
    if candidates.is_empty() {
        bail!("no provider available in repository for {symbol}");
    }

    // 1. If explicit CLI override is provided, select it if valid
    if let Some(override_pkg) = cli_override {
        if let Some(matched) = candidates.iter().find(|k| k.name == override_pkg) {
            return Ok(matched.clone());
        } else {
            bail!(
                "specified provider '{override_pkg}' does not satisfy {symbol} (available: {})",
                candidates
                    .iter()
                    .map(|k| k.name.as_str())
                    .collect::<Vec<_>>()
                    .join(", ")
            );
        }
    }

    if candidates.len() == 1 {
        return Ok(candidates[0].clone());
    }

    // 2. If interactive terminal is attached and requested, prompt the user to choose
    if interactive && std::io::stdin().is_terminal() && std::io::stdout().is_terminal() {
        use std::io::{self, BufRead, Write};

        println!("\nThere are multiple providers available for {symbol}:");
        for (i, key) in candidates.iter().enumerate() {
            println!("  {}) {} ({})", i + 1, key.name, key.channel);
        }
        print!("Select a provider [1-{}] (default 1): ", candidates.len());
        io::stdout().flush().ok();

        let mut line = String::new();
        let stdin = io::stdin();
        if stdin.lock().read_line(&mut line).is_ok() {
            let trimmed = line.trim();
            if trimmed.is_empty() {
                return Ok(candidates[0].clone());
            }
            if let Some(idx) = trimmed
                .parse::<usize>()
                .ok()
                .filter(|&idx| idx >= 1 && idx <= candidates.len())
            {
                return Ok(candidates[idx - 1].clone());
            }
        }
        println!(
            "Invalid selection, selecting default provider: {}",
            candidates[0].name
        );
    } else {
        println!(
            "Notice: multiple providers available for {symbol}; selecting default provider '{}'",
            candidates[0].name
        );
    }

    Ok(candidates[0].clone())
}

type VirtualSelections = (
    Vec<sage_core::PackageKey>,
    BTreeMap<String, sage_core::PackageKey>,
    BTreeMap<String, sage_core::PackageKey>,
    BTreeMap<String, String>,
);

/// Identifies requested package keys and resolves virtual selections (both direct and transitive).
fn resolve_virtual_selections(
    universe: &sage_solver::PackageUniverse,
    channel: &str,
    names: &[String],
    config: &SystemConfig,
    installed: &[sage_db::InstalledPackage],
    provider_overrides: &[(String, String)],
    interactive: bool,
) -> Result<VirtualSelections> {
    let mut preferences = config.provider_preferences("main/system")?;
    let mut bound_providers = BTreeMap::new();
    let mut new_config_providers = BTreeMap::new();

    // All provider mappings declared in system.toml under [providers] are strictly bound
    // so the solver adheres to user configuration without backtracking to alternative packages.
    for (symbol, key) in &preferences {
        bound_providers.insert(symbol.clone(), key.clone());
    }

    // Process explicit CLI provider overrides first
    for (interface, pkg) in provider_overrides {
        let symbol = provider_symbol(interface);
        let key = sage_core::PackageKey::in_channel(channel, pkg)?;
        bound_providers.insert(symbol.clone(), key.clone());
        preferences.insert(symbol.clone(), key);
        new_config_providers.insert(interface.clone(), pkg.clone());
    }

    let mut concrete_requested = Vec::new();

    // Process explicitly requested package names
    for name in names {
        if name.starts_with("virtual/") {
            let symbol = name.as_str();
            let interface = symbol.strip_prefix("virtual/").unwrap_or(symbol);

            let chosen = if let Some(key) = bound_providers
                .get(symbol)
                .or_else(|| preferences.get(symbol))
            {
                key.clone()
            } else {
                let candidates: Vec<_> = universe
                    .providers_for(symbol)
                    .iter()
                    .filter(|k| k.channel == channel)
                    .cloned()
                    .collect();

                let cli_override = provider_overrides
                    .iter()
                    .find(|(k, _)| k == interface || k == symbol)
                    .map(|(_, v)| v.as_str());

                let selected =
                    select_virtual_provider(symbol, &candidates, cli_override, interactive)?;
                bound_providers.insert(symbol.to_string(), selected.clone());
                preferences.insert(symbol.to_string(), selected.clone());
                new_config_providers.insert(interface.to_string(), selected.name.clone());
                selected
            };

            concrete_requested.push(chosen);
        } else {
            let key = if name.contains(':') {
                sage_core::PackageKey::in_channel(channel, name)?
            } else {
                let default_key = sage_core::PackageKey::in_channel(channel, name)?;
                if universe.contains_key(&default_key) {
                    default_key
                } else if let Some(installed_pkg) = installed
                    .iter()
                    .find(|pkg| pkg.key.channel == channel && pkg.key.name == *name)
                {
                    installed_pkg.key.clone()
                } else {
                    let mut matching: Vec<_> = universe
                        .keys()
                        .filter(|k| k.channel == channel && k.name == *name)
                        .cloned()
                        .collect();
                    matching.sort();
                    matching.pop().unwrap_or(default_key)
                }
            };
            concrete_requested.push(key);
        }
    }

    // Inspect direct and transitive virtual dependencies of requested packages
    let mut visited = BTreeSet::new();
    let mut queue: Vec<_> = concrete_requested.clone();
    while let Some(key) = queue.pop() {
        if !visited.insert(key.clone()) {
            continue;
        }
        for version in universe.versions(&key) {
            if let Some(release) = universe.release(&key, version) {
                for dep in &release.dependencies {
                    if dep.name.starts_with("virtual/") {
                        let symbol = &dep.name;
                        let interface = symbol.strip_prefix("virtual/").unwrap_or(symbol);
                        if preferences.contains_key(symbol) || bound_providers.contains_key(symbol)
                        {
                            continue;
                        }
                        // Check if an installed package already provides this virtual symbol
                        let already_provided = installed
                            .iter()
                            .any(|pkg| pkg.provides.iter().any(|s| s == symbol));
                        if already_provided {
                            continue;
                        }
                        let candidates: Vec<_> = universe
                            .providers_for(symbol)
                            .iter()
                            .filter(|k| k.channel == channel)
                            .cloned()
                            .collect();
                        if candidates.len() > 1 {
                            let cli_override = provider_overrides
                                .iter()
                                .find(|(k, _)| k == interface || k == symbol)
                                .map(|(_, v)| v.as_str());
                            let selected = select_virtual_provider(
                                symbol,
                                &candidates,
                                cli_override,
                                interactive,
                            )?;
                            bound_providers.insert(symbol.clone(), selected.clone());
                            preferences.insert(symbol.clone(), selected.clone());
                            new_config_providers
                                .insert(interface.to_string(), selected.name.clone());
                        }
                    } else {
                        let dep_channel = dep.channel.as_deref().unwrap_or(channel);
                        let target_key = sage_core::PackageKey::new(
                            dep_channel,
                            &dep.name,
                            dep.slot.as_deref().unwrap_or(sage_core::DEFAULT_SLOT),
                        );
                        if !visited.contains(&target_key) {
                            queue.push(target_key);
                        }
                    }
                }
            }
        }
    }

    Ok((
        concrete_requested,
        bound_providers,
        preferences,
        new_config_providers,
    ))
}

/// Resolves dependencies and applies packages to the sysroot transactionally.
#[allow(clippy::too_many_arguments)]
pub async fn apply_packages(
    root: &Path,
    names: &[String],
    channel: Option<&str>,
    provider_overrides: &[(String, String)],
    interactive: bool,
    prefer_latest: bool,
    save: bool,
    dry_run: bool,
) -> Result<()> {
    let config_path = under_root(root, Path::new("/etc/sage/system.toml"));
    let config = SystemConfig::load(&config_path)?;
    let mut available = load_available_with_pool(root, Some(&config.system.architecture), None)?;
    let channel = canonical_channel(&available, channel)?;
    let db_path = under_root(root, Path::new("/var/lib/sage"));
    let installed = installed_packages(&db_path, dry_run)?;
    available.register_installed(&installed);
    let selections = resolve_virtual_selections(
        &available.universe,
        &channel,
        names,
        &config,
        &installed,
        provider_overrides,
        interactive,
        prefer_latest,
    )?;
    let current: BTreeMap<_, _> = installed
        .iter()
        .map(|package| (package.key.clone(), package.version.clone()))
        .collect();
    let changes = selections
        .solution
        .into_iter()
        .filter(|(key, version)| current.get(key) != Some(version))
        .collect();
    let changes = installation_order(&available, changes)?;
    let plan = TransactionPlan::new(changes, Vec::new(), selections.providers);
    let diff = crate::diff::compute_transaction_diff(&plan, &installed, Some(&available));
    diff.print_summary();
    if dry_run {
        return Ok(());
    }
    let declaration = if save && channel == "main/system" {
        let mut next = config.clone();
        for name in names {
            if !name.starts_with("virtual/") && !next.packages.contains(name) {
                next.packages.insert(name.clone());
            }
        }
        for key in selections.package_roots {
            next.packages.insert(format!("{}:{}", key.name, key.slot));
        }
        for (iface, prov) in selections.declarations {
            // Canonicalize aliases so a later rebuild sees exactly one binding.
            next.providers.remove(&format!("virtual/{iface}"));
            next.providers.insert(iface, prov);
        }
        if next.packages == config.packages && next.providers == config.providers {
            None
        } else {
            Some(sage_db::FileMutation {
                path: "etc/sage/system.toml".into(),
                previous: Some(fs::read(&config_path)?),
                next: Some(toml::to_string_pretty(&next)?.into_bytes()),
            })
        }
    } else {
        None
    };
    if !plan.install.is_empty() || declaration.is_some() {
        let database = sage_db::SageDatabase::open(&db_path)?;
        publish_packages(
            root,
            &database,
            &available,
            &config.system.architecture,
            &plan,
            declaration,
        )
        .await?;
        let audits = crate::process::audit_running_processes(root);
        crate::process::print_process_audit(&audits);
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
    declaration: Option<sage_db::FileMutation>,
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
    journal.set_declaration(declaration);
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
    apply_packages(
        root,
        &names,
        Some(&canonical),
        &[],
        false,
        true,
        false,
        dry_run,
    )
    .await
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
        .map(|selector| {
            if selector.contains(':') {
                sage_core::PackageKey::in_channel(&canonical, selector)
            } else {
                let default_key = sage_core::PackageKey::in_channel(&canonical, selector)?;
                if installed.iter().any(|pkg| pkg.key == default_key) {
                    Ok(default_key)
                } else if let Some(pkg) = installed
                    .iter()
                    .find(|pkg| pkg.key.channel == canonical && pkg.key.name == *selector)
                {
                    Ok(pkg.key.clone())
                } else {
                    Ok(default_key)
                }
            }
        })
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
    let diff = crate::diff::compute_transaction_diff(&plan, &installed, None);
    diff.print_summary();
    if dry_run {
        return Ok(());
    }
    let declaration = if update_saved && canonical == "main/system" {
        let config_path = under_root(root, Path::new("/etc/sage/system.toml"));
        let mut config = SystemConfig::load(&config_path)?;
        let original_packages = config.packages.clone();
        config.packages.retain(|selector| {
            sage_core::PackageKey::in_channel(&canonical, selector)
                .map_or(true, |key| !requested.contains(&key))
        });
        if config.packages == original_packages {
            None
        } else {
            let previous = fs::read(&config_path)?;
            let next = toml::to_string_pretty(&config)?.into_bytes();
            Some(sage_db::FileMutation {
                path: "etc/sage/system.toml".into(),
                previous: Some(previous),
                next: Some(next),
            })
        }
    } else {
        None
    };
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
    journal.set_declaration(declaration);
    database.write_journal(&journal)?;
    let result = resume_remove(root, &database, &mut journal);
    let audits = crate::process::audit_running_processes(root);
    crate::process::print_process_audit(&audits);
    result
}
