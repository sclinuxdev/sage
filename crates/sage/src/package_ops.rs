pub use sage_repo::{ReleaseLocation, ReleaseSource};
async fn sync_channels(root: &Path, selected: Option<&str>, dry_run: bool) -> Result<()> {
    let config_path = under_root(root, Path::new("/etc/sage/channels.toml"));
    let config = sage_repo::ChannelsConfig::load(&config_path)
        .with_context(|| format!("failed to load {}", config_path.display()))?;
    let cache = under_root(root, Path::new("/var/cache/sage/channels"));
    let engine = sage_repo::DownloadEngine::new(&cache)?;
    let mut matched = false;
    for (channel_name, channel) in &config.channels {
        if !channel.enabled {
            continue;
        }
        for (sub_name, subchannel) in &channel.subchannels {
            if !subchannel.enabled {
                continue;
            }
            let alias = subchannel.alias.as_deref().unwrap_or(sub_name);
            let canonical = format!("{channel_name}/{alias}");
            if selected.is_some_and(|value| value != alias && value != canonical) {
                continue;
            }
            matched = true;
            let destination = cache.join(channel_name).join(alias).join("index.mdb");
            let url = sage_repo::subchannel_url(channel, sub_name, subchannel);
            let key = under_root(root, &channel.signing_key);
            if dry_run {
                println!("Would sync {canonical} from {url}");
            } else {
                let changed = engine.sync_index(&url, &key, &destination).await?;
                println!(
                    "{canonical}: {}",
                    if changed { "updated" } else { "current" }
                );
            }
        }
    }
    if selected.is_some() && !matched {
        bail!("selected channel was not found or is disabled");
    }
    Ok(())
}
fn under_root(root: &Path, path: &Path) -> PathBuf {
    root.join(path.strip_prefix("/").unwrap_or(path))
}
fn alternative_declaration_path(key: &sage_core::PackageKey) -> PathBuf {
    let digest = hex::encode(Sha256::digest(key.canonical_id().as_bytes()));
    PathBuf::from("usr/share/sage/alternatives").join(format!("{digest}.toml"))
}
fn sysusers_declaration_path(key: &sage_core::PackageKey) -> PathBuf {
    let digest = hex::encode(Sha256::digest(key.canonical_id().as_bytes()));
    PathBuf::from("usr/share/sage/sysusers").join(format!("{digest}.toml"))
}
fn read_documents(root: &Path, relative: &Path) -> Result<Vec<Vec<u8>>> {
    let directory = under_root(root, relative);
    if !directory.exists() {
        return Ok(Vec::new());
    }
    let mut entries = std::fs::read_dir(directory)?.collect::<Result<Vec<_>, _>>()?;
    entries.sort_by_key(std::fs::DirEntry::file_name);
    entries
        .into_iter()
        .filter(|entry| entry.path().extension().and_then(|value| value.to_str()) == Some("toml"))
        .map(|entry| std::fs::read(entry.path()).map_err(Into::into))
        .collect()
}
fn alternatives_from_documents(documents: &[Vec<u8>]) -> Result<Vec<sage_sys::Alternative>> {
    let mut alternatives = Vec::new();
    for document in documents {
        alternatives.extend(sage_sys::AlternativesDocument::parse(document)?.alternatives());
    }
    Ok(alternatives)
}
fn trigger_documents(root: &Path) -> Result<Vec<Vec<u8>>> {
    sage_sys::TriggerEngine::load_triggers(root)?
        .into_iter()
        .map(|trigger| Ok(toml::to_string(&trigger)?.into_bytes()))
        .collect()
}
/// One-shot sysroot-local crash injection; the next startup exercises recovery.
#[cfg(feature = "torture")]
fn crash_point(root: &Path, stage: &str) -> Result<()> {
    let marker = under_root(root, Path::new("/run/sage/crash-point"));
    let requested = std::fs::read_to_string(&marker).ok();
    let graceful = requested.as_deref().is_some_and(|value| value.trim() == stage);
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
fn crash_point(_root: &Path, _stage: &str) -> Result<()> {
    Ok(())
}
async fn settle_journals(root: &Path) -> Result<()> {
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
                resume_install(root, &database, &available, &mut journal).await?;
            }
            sage_db::JournalAction::Remove { .. } => {
                resume_remove(root, &database, &mut journal)?;
            }
        }
        eprintln!("Recovered operation {}", journal.op_id);
    }
    Ok(())
}
pub struct AvailablePackages {
    pub universe: sage_solver::PackageUniverse,
    pub releases: BTreeMap<(sage_core::PackageKey, sage_core::Version), ReleaseSource>,
    pub aliases: BTreeMap<String, String>,
}
pub fn load_available_with_pool(
    root: &Path,
    architecture: Option<&str>,
    local_pool: Option<&Path>,
) -> Result<AvailablePackages> {
    let config =
        sage_repo::ChannelsConfig::load(under_root(root, Path::new("/etc/sage/channels.toml")))?;
    let cache = under_root(root, Path::new("/var/cache/sage/channels"));
    let mut universe = sage_solver::PackageUniverse::default();
    let mut releases = BTreeMap::new();
    let mut aliases = BTreeMap::new();
    let mut target_roots = BTreeMap::new();
    for (channel_name, channel) in config.channels {
        if !channel.enabled {
            continue;
        }
        for (sub_name, subchannel) in &channel.subchannels {
            if !subchannel.enabled {
                continue;
            }
            let alias = subchannel.alias.as_deref().unwrap_or(sub_name);
            let canonical = format!("{channel_name}/{alias}");
            // Register configured names even before their remote index has
            // been synchronized. This is required during a staged bootstrap,
            // where the local output pool is the authoritative source and
            // intentionally starts without an LMDB index.
            aliases.insert(alias.into(), canonical.clone());
            aliases.insert(canonical.clone(), canonical.clone());
            target_roots.insert(canonical.clone(), subchannel.target_root.clone());
            // Source builds with an explicit pool form a closed bootstrap
            // universe.  Falling back to a synchronized channel here can mix
            // old-format or newer binary releases into an otherwise local,
            // reproducible self-hosting graph.  Configured aliases are still
            // registered above so local package identities canonicalize in
            // exactly the same way as normal repository packages.
            if local_pool.is_some() {
                continue;
            }
            let index_path = cache.join(&channel_name).join(alias).join("index.mdb");
            if !index_path.exists() {
                continue;
            }
            let url = sage_repo::subchannel_url(&channel, sub_name, subchannel);
            for release in sage_repo::RepositoryIndex::open(&index_path)?.all_releases()? {
                if architecture.is_some_and(|wanted| {
                    release.package.arch != wanted
                        && release.package.arch != "any"
                        && release.package.arch != "noarch"
                }) {
                    continue;
                }
                let coordinate = release.coordinate_for_channel(&canonical);
                universe.insert(release.package.for_channel(&canonical));
                releases.insert(
                    (coordinate.key, coordinate.version),
                    ReleaseSource {
                        release,
                        location: ReleaseLocation::Remote(url.clone()),
                        target_root: subchannel.target_root.clone(),
                    },
                );
            }
        }
    }
    if let Some(pool) = local_pool.filter(|pool| pool.exists()) {
        let mut packages: Vec<_> = walkdir::WalkDir::new(pool)
            .follow_links(false)
            .into_iter()
            .filter_map(|entry| entry.ok())
            .map(|entry| entry.into_path())
            .filter(|path| {
                path.file_name()
                    .and_then(|name| name.to_str())
                    .is_some_and(|name| name.ends_with(".pkg.tar.zst"))
            })
            .collect();
        packages.sort();
        for path in packages {
            let inspection = sage_archive::inspect_package(&path)
                .with_context(|| format!("failed to inspect local package {}", path.display()))?;
            if architecture.is_some_and(|wanted| {
                inspection.manifest.arch != wanted
                    && inspection.manifest.arch != "any"
                    && inspection.manifest.arch != "noarch"
            }) {
                continue;
            }
            let canonical = aliases
                .get(&inspection.manifest.channel)
                .cloned()
                .unwrap_or_else(|| {
                if inspection.manifest.channel.contains('/') {
                    inspection.manifest.channel.clone()
                } else {
                    format!("main/{}", inspection.manifest.channel)
                }
            });
            aliases
                .entry(inspection.manifest.channel.clone())
                .or_insert_with(|| canonical.clone());
            aliases
                .entry(canonical.clone())
                .or_insert_with(|| canonical.clone());
            let package = inspection.manifest.for_channel(&canonical);
            let coordinate = package.coordinate();
            universe.insert(package);
            releases.insert(
                (coordinate.key, coordinate.version),
                ReleaseSource {
                    release: sage_repo::IndexedRelease {
                        package: inspection.manifest,
                        archive: path
                            .strip_prefix(pool)
                            .unwrap_or(path.as_path())
                            .to_string_lossy()
                            .into_owned(),
                        sha256: String::new(),
                    },
                    location: ReleaseLocation::Local(path),
                    target_root: target_roots
                        .get(&canonical)
                        .cloned()
                        .unwrap_or_else(|| PathBuf::from("/")),
                },
            );
        }
    }
    Ok(AvailablePackages {
        universe,
        releases,
        aliases,
    })
}

fn canonical_channel(available: &AvailablePackages, selected: Option<&str>) -> Result<String> {
    let selected = selected.unwrap_or("system");
    available
        .aliases
        .get(selected)
        .cloned()
        .with_context(|| format!("channel '{selected}' has no synchronized index"))
}
async fn apply_packages(
    root: &Path,
    names: &[String],
    channel: Option<&str>,
    prefer_latest: bool,
    save: bool,
    dry_run: bool,
) -> Result<()> {
    let config_path = under_root(root, Path::new("/etc/sage/system.toml"));
    let mut config = sage_sys::SystemConfig::load(&config_path)?;
    let mut available = load_available_with_pool(root, Some(&config.system.architecture), None)?;
    let channel = canonical_channel(&available, channel)?;
    let requested: Vec<_> = names
        .iter()
        .map(|name| sage_core::PackageKey::in_channel(&channel, name))
        .collect::<Result<_, _>>()?;
    let db_path = under_root(root, Path::new("/var/lib/sage"));
    let installed = if dry_run {
        sage_db::read_packages(&db_path)?
    } else {
        sage_db::SageDatabase::open(&db_path)?.packages()?
    };
    // Installed packages remain solver roots even when unrelated to the new
    // request. This makes their conflicts active and keeps orphaned releases
    // present through compact records reconstructed from LMDB.
    for package in &installed {
        if !available
            .universe
            .versions(&package.key)
            .any(|version| version == &package.version)
        {
            let mut release = sage_core::Package::from_release(
                package.key.clone(),
                package.version.clone(),
                package.dependencies.clone(),
                package.provides.clone(),
            );
            release.arch.clone_from(&package.arch);
            release.conflicts.clone_from(&package.conflicts);
            release.installed_size = package.installed_size;
            available.universe.insert(release);
        }
    }
    let roots = requested
        .iter()
        .cloned()
        .chain(installed.iter().map(|package| package.key.clone()))
        .collect::<BTreeSet<_>>()
        .into_iter()
        .collect::<Vec<_>>();
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
    for (key, version) in &changes {
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
    if !changes.is_empty() {
        let database = sage_db::SageDatabase::open(&db_path)?;
        publish_packages(
            root,
            &database,
            &available,
            &config.system.architecture,
            &changes,
            &[],
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
fn installation_order(
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
                pending.keys().any(|candidate| {
                    if candidate == key {
                        return false;
                    }
                    if dependency.name.starts_with("virtual/") || dependency.name.starts_with("so:")
                    {
                        return pending
                            .get(candidate)
                            .and_then(|candidate_version| {
                                available
                                    .releases
                                    .get(&(candidate.clone(), candidate_version.clone()))
                            })
                            .is_some_and(|source| {
                                source.release.package.provides.contains(&dependency.name)
                            });
                    }
                    candidate.name == dependency.name
                        && candidate.slot
                            == dependency.slot.as_deref().unwrap_or(sage_core::DEFAULT_SLOT)
                        && candidate.channel
                            == dependency.channel.as_deref().unwrap_or(&key.channel)
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
async fn publish_packages(
    root: &Path,
    database: &sage_db::SageDatabase,
    available: &AvailablePackages,
    architecture: &str,
    changes: &[(sage_core::PackageKey, sage_core::Version)],
    retired: &[sage_db::InstalledPackage],
) -> Result<()> {
    let changes =
        preflight_packages(root, database, available, architecture, changes, retired).await?;
    let op_id = operation_id("install")?;
    // Recovery must not consult records that installation may already have replaced.
    let mut previous_packages = retired
        .iter()
        .cloned()
        .map(|package| (package.key.clone(), package))
        .collect::<BTreeMap<_, _>>();
    for (key, _) in &changes {
        if let Some(package) = database.package(key)? {
            previous_packages.insert(key.clone(), package);
        }
    }
    let mut journal = sage_db::JournalRecord::new(
        op_id,
        "packages",
        sage_db::JournalAction::Install {
            architecture: architecture.into(),
            changes: changes.clone(),
            previous_packages: previous_packages.into_values().collect(),
            retired_packages: retired.to_vec(),
            modified_paths: Vec::new(),
            removed_paths: Vec::new(),
            previous_alternative_documents: read_documents(
                root,
                Path::new("usr/share/sage/alternatives"),
            )?,
            removal_trigger_documents: trigger_documents(root)?,
        },
    );
    database.write_journal(&journal)?;
    resume_install(root, database, available, &mut journal).await
}

struct PackageDeclarations {
    services: Vec<(String, Vec<u8>)>,
    trigger: Option<(String, Vec<u8>)>,
    alternatives: Option<Vec<u8>>,
    sysusers: Option<Vec<u8>>,
}

impl PackageDeclarations {
    fn parse(inspection: &sage_archive::PackageInspection, key: &sage_core::PackageKey) -> Result<Self> {
        let services = if let Some(bytes) = inspection.optional.get(".METADATA/service.toml") {
            sage_sys::ServiceDocument::parse(bytes)?
                .into_services()
                .into_iter()
                .map(|service| {
                    let name = service.name.clone();
                    let bytes = toml::to_string_pretty(&sage_sys::ServiceDocument {
                        schema_version: sage_core::SCHEMA_VERSION,
                        service: Some(service),
                        services: Vec::new(),
                    })?
                    .into_bytes();
                    Ok((name, bytes))
                })
                .collect::<Result<Vec<_>>>()?
        } else {
            Vec::new()
        };
        let trigger = inspection
            .optional
            .get(".METADATA/triggers.toml")
            .map(|bytes| -> Result<(String, Vec<u8>)> {
                let trigger = sage_sys::TriggerSpec::parse(bytes)?;
                Ok((trigger.name, bytes.clone()))
            })
            .transpose()?;
        let alternatives = inspection
            .optional
            .get(".METADATA/alternatives.toml")
            .map(|bytes| -> Result<Vec<u8>> {
                let mut document = sage_sys::AlternativesDocument::parse(bytes)?;
                document.package.clone_from(key);
                Ok(toml::to_string_pretty(&document)?.into_bytes())
            })
            .transpose()?;
        let sysusers = inspection
            .optional
            .get(".METADATA/sysusers.toml")
            .map(|bytes| -> Result<Vec<u8>> {
                let mut document = sage_sys::SysusersDocument::parse(bytes)?;
                document.package.clone_from(key);
                Ok(toml::to_string_pretty(&document)?.into_bytes())
            })
            .transpose()?;
        Ok(Self {
            services,
            trigger,
            alternatives,
            sysusers,
        })
    }

    fn ownership_paths(&self, key: &sage_core::PackageKey) -> Vec<String> {
        let mut paths = self
            .services
            .iter()
            .map(|(name, _)| format!("usr/share/sage/services/{name}.toml"))
            .collect::<Vec<_>>();
        if let Some((name, _)) = &self.trigger {
            paths.push(format!("usr/share/sage/triggers/{name}.toml"));
        }
        if self.alternatives.is_some() {
            paths.push(alternative_declaration_path(key).to_string_lossy().into_owned());
        }
        if self.sysusers.is_some() {
            paths.push(sysusers_declaration_path(key).to_string_lossy().into_owned());
        }
        paths
    }
}

/// Validates and orders the complete transaction before creating a durable recovery record.
/// A rejected archive or ownership conflict has made no filesystem or LMDB
/// mutation, so it must not become an endlessly retried startup journal.
async fn preflight_packages(
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
    let mut final_paths = retired
        .iter()
        .map(|package| (package.key.clone(), BTreeSet::new()))
        .collect::<BTreeMap<_, _>>();
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
        if inspection.manifest.arch != architecture
            && inspection.manifest.arch != "any"
            && inspection.manifest.arch != "noarch"
        {
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
        let mut ownership: Vec<_> = inspection
            .files
            .iter()
            .map(|record| prefix.join(&record.path).to_string_lossy().into_owned())
            .collect();
        ownership.extend(PackageDeclarations::parse(&inspection, key)?.ownership_paths(key));
        for path in &ownership {
            if let Some(owner) = planned.insert(path.clone(), key.clone()) {
                if owner != *key {
                    bail!("transaction packages {owner} and {key} both own {path}");
                }
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
    for (path, claimant) in &planned {
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
            if let Some(owners) = installed_paths.get(ancestor.as_ref()) {
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
            if owner == *claimant {
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
async fn resume_install(
    root: &Path,
    database: &sage_db::SageDatabase,
    available: &AvailablePackages,
    journal: &mut sage_db::JournalRecord,
) -> Result<()> {
    journal.validate()?;
    let (architecture, changes, previous_packages, retired_packages) = match &journal.action {
        sage_db::JournalAction::Install {
            architecture,
            changes,
            previous_packages,
            retired_packages,
            ..
        } => (
            architecture.clone(),
            changes.clone(),
            previous_packages
                .iter()
                .cloned()
                .map(|package| (package.key.clone(), package))
                .collect::<BTreeMap<_, _>>(),
            retired_packages.clone(),
        ),
        _ => bail!("install recovery received a removal journal"),
    };
    let package_cache = under_root(root, Path::new("/var/cache/sage/packages"));
    let engine = sage_repo::DownloadEngine::new(&package_cache)?;
    let mut previous_config = BTreeMap::new();
    for package in previous_packages.values() {
        for (relative, hash) in &package.config_hashes {
            if let Some(path) = package.files.iter().find(|path| Path::new(path).ends_with(relative)) {
                previous_config.insert(path.clone(), hash.clone());
            }
        }
    }
    let mut modified = Vec::new();
    let mut removed_paths = Vec::new();
    if journal.stage == "packages" {
    for package in &retired_packages {
        database.remove(&package.key)?;
    }
    for (key, version) in &changes {
        let source = available
            .releases
            .get(&(key.clone(), version.clone()))
            .with_context(|| format!("index record disappeared for {key} {version}"))?;
        let archive = obtain_release_archive(&engine, &package_cache, source).await?;
        let inspection = sage_archive::inspect_package(&archive)?;
        if inspection.manifest.arch != architecture
            && inspection.manifest.arch != "any"
            && inspection.manifest.arch != "noarch"
        {
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
        let mut ownership: Vec<_> = inspection
            .files
            .iter()
            .map(|record| prefix.join(&record.path).to_string_lossy().into_owned())
            .collect();
        let declarations = PackageDeclarations::parse(&inspection, key)?;
        ownership.extend(declarations.ownership_paths(key));
        for path in &ownership {
            let owners = database.owners(path)?;
            if owners.iter().any(|owner| owner != key) {
                bail!("file conflict for {path}: {owners:?}");
            }
        }
        let previous_package = previous_packages.get(key).cloned();
        let mut previous = previous_package
            .as_ref()
            .map(|package| package.config_hashes.clone())
            .unwrap_or_default();
        for record in inspection.files.iter().filter(|record| record.path.starts_with("etc")) {
            let physical = prefix.join(&record.path).to_string_lossy().into_owned();
            if let Some(hash) = previous_config.get(&physical) {
                previous.entry(record.path.to_string_lossy().into_owned()).or_insert(hash.clone());
            }
        }
        let report = sage_archive::extract_package_with_config(
            &archive,
            &target,
            &inspection.files,
            &previous,
        )?;
        crash_point(root, "extraction")?;
        for (name, bytes) in declarations.services {
            write_atomic_under_root(
                root,
                &Path::new("usr/share/sage/services").join(format!("{name}.toml")),
                &bytes,
            )?;
        }
        if let Some((name, bytes)) = declarations.trigger {
            write_atomic_under_root(
                root,
                &Path::new("usr/share/sage/triggers").join(format!("{name}.toml")),
                &bytes,
            )?;
        }
        if let Some(bytes) = declarations.alternatives {
            write_atomic_under_root(root, &alternative_declaration_path(key), &bytes)?;
        }
        if let Some(bytes) = declarations.sysusers {
            write_atomic_under_root(root, &sysusers_declaration_path(key), &bytes)?;
        }
        modified.extend(ownership.iter().map(PathBuf::from));
        let config_hashes = inspection
            .files
            .iter()
            .filter(|record| record.path.starts_with("etc"))
            .map(|record| {
                (
                    record.path.to_string_lossy().into_owned(),
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
                    if !should_preserve_config(&path, obsolete, &previous_package.config_hashes)? {
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
    for package in &retired_packages {
        for relative in &package.files {
            if database.owners(relative)?.is_empty() {
                let path = under_root(root, Path::new(relative));
                if !should_preserve_config(&path, relative, &package.config_hashes)? {
                    remove_file_beneath(root, &path)?;
                    modified.push(PathBuf::from(relative));
                    removed_paths.push(relative.clone());
                }
            }
        }
    }
    if let sage_db::JournalAction::Install {
        modified_paths,
        removed_paths: journal_removed,
        ..
    } = &mut journal.action
    {
        *modified_paths = modified
            .iter()
            .map(|path| path.to_string_lossy().into_owned())
            .collect();
        *journal_removed = removed_paths;
    }
    journal.advance("alternatives");
    database.write_journal(journal)?;
    }
    let modified: Vec<PathBuf> = match &journal.action {
        sage_db::JournalAction::Install { modified_paths, .. } => {
            modified_paths.iter().map(PathBuf::from).collect()
        }
        _ => unreachable!(),
    };
    if journal.stage == "alternatives" {
    let previous_alternatives = alternatives_from_documents(match &journal.action {
        sage_db::JournalAction::Install {
            previous_alternative_documents,
            ..
        } => previous_alternative_documents,
        _ => unreachable!(),
    })?;
    let current_alternatives = sage_sys::AlternativesDocument::load_installed(root)?;
    sage_sys::ProfileEngine::reconcile_alternatives(
        root,
        &previous_alternatives,
        &current_alternatives,
    )?;
    let accounts = sage_sys::SysusersDocument::load_installed(root)?;
    sage_sys::SysusersEngine::reconcile(root, &accounts)?;
    journal.advance("triggers");
    database.write_journal(journal)?;
    crash_point(root, "alternatives")?;
    }
    if journal.stage == "triggers" {
    let (removal_triggers, removed_paths) = match &journal.action {
        sage_db::JournalAction::Install {
            removal_trigger_documents,
            removed_paths,
            ..
        } => (
            removal_trigger_documents
                .iter()
                .map(|bytes| sage_sys::TriggerSpec::parse(bytes))
                .collect::<Result<Vec<_>, _>>()?,
            removed_paths.iter().map(PathBuf::from).collect::<Vec<_>>(),
        ),
        _ => unreachable!(),
    };
    sage_sys::TriggerEngine::execute_triggers_for(
        &removal_triggers,
        &removed_paths,
        root,
        sage_sys::TriggerEvent::PostRemove,
    )?;
    let triggers = sage_sys::TriggerEngine::load_triggers(root)?;
    crash_point(root, "triggers")?;
    sage_sys::TriggerEngine::execute_triggers_for(
        &triggers,
        &modified,
        root,
        sage_sys::TriggerEvent::PostChange,
    )?;
    journal.advance("complete");
    database.write_journal(journal)?;
    crash_point(root, "trigger-complete")?;
    }
    database.finish_journal(&journal.op_id)?;
    Ok(())
}
async fn upgrade_packages(
    root: &Path,
    names: &[String],
    channel: Option<&str>,
    dry_run: bool,
) -> Result<()> {
    let config = sage_sys::SystemConfig::load(under_root(
        root,
        Path::new("/etc/sage/system.toml"),
    ))?;
    let available = load_available_with_pool(root, Some(&config.system.architecture), None)?;
    let canonical = canonical_channel(&available, channel)?;
    let names = if names.is_empty() {
        let db_path = under_root(root, Path::new("/var/lib/sage"));
        let packages = if dry_run {
            sage_db::read_packages(&db_path)?
        } else {
            sage_db::SageDatabase::open(&db_path)?.packages()?
        };
        packages
            .into_iter()
            .filter(|package| package.key.channel == canonical)
            .map(|package| format!("{}:{}", package.key.name, package.key.slot))
            .collect()
    } else {
        names.to_vec()
    };
    apply_packages(root, &names, Some(&canonical), true, false, dry_run).await
}
fn remove_packages(
    root: &Path,
    names: &[String],
    channel: Option<&str>,
    update_saved: bool,
    dry_run: bool,
) -> Result<()> {
    let db_path = under_root(root, Path::new("/var/lib/sage"));
    let installed = if dry_run {
        sage_db::read_packages(&db_path)?
    } else {
        sage_db::SageDatabase::open(&db_path)?.packages()?
    };
    let canonical = channel.map_or_else(
        || "main/system".into(),
        |value| {
            if value.contains('/') {
                value.into()
            } else {
                format!("main/{value}")
            }
        },
    );
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
    for dependent in &installed {
        if selected.iter().any(|removed| {
            dependent.key != removed.key
                && dependent.dependencies.iter().any(|dependency| {
                    let virtual_dependency = dependency.name.starts_with("virtual/")
                        || dependency.name.starts_with("so:");
                    let removed_direct = dependency.name == removed.key.name;
                    let matched = (removed_direct
                        || removed.provides.contains(&dependency.name))
                        && dependency.slot.as_deref().map_or_else(
                            || {
                                virtual_dependency
                                    || !removed_direct
                                    || removed.key.slot == sage_core::DEFAULT_SLOT
                            },
                            |slot| slot == removed.key.slot,
                        )
                        && dependency
                            .channel
                            .as_deref()
                            .is_none_or(|channel| channel == removed.key.channel);
                    let replacement = installed.iter().any(|candidate| {
                        let candidate_direct = dependency.name == candidate.key.name;
                        !selected.iter().any(|removed| removed.key == candidate.key)
                            && (candidate_direct
                                || ((!removed_direct || virtual_dependency)
                                    && candidate.provides.contains(&dependency.name)))
                            && dependency.slot.as_deref().map_or_else(
                                || {
                                    virtual_dependency
                                        || !candidate_direct
                                        || candidate.key.slot == sage_core::DEFAULT_SLOT
                                },
                                |slot| slot == candidate.key.slot,
                            )
                            && candidate.key.channel == removed.key.channel
                            && dependency.op.matches(&candidate.version, dependency.version.as_ref())
                    });
                    matched && !replacement
                })
        }) {
            bail!("cannot remove packages required by {}", dependent.key);
        }
    }
    for package in &selected {
        println!("Remove {} {}", package.key, package.version);
    }
    if dry_run {
        return Ok(());
    }
    if update_saved && canonical == "main/system" {
        let config_path = under_root(root, Path::new("/etc/sage/system.toml"));
        let mut config = sage_sys::SystemConfig::load(&config_path)?;
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
            packages: selected,
            modified_paths: Vec::new(),
            trigger_documents: trigger_documents(root)?,
            alternative_documents: read_documents(
                root,
                Path::new("usr/share/sage/alternatives"),
            )?,
        },
    );
    database.write_journal(&journal)?;
    resume_remove(root, &database, &mut journal)
}
fn resume_remove(
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
    let modified: Vec<PathBuf> = match &journal.action {
        sage_db::JournalAction::Remove { modified_paths, .. } => {
            modified_paths.iter().map(PathBuf::from).collect()
        }
        _ => unreachable!(),
    };
    if journal.stage == "alternatives" {
    let previous_alternatives = alternatives_from_documents(match &journal.action {
        sage_db::JournalAction::Remove {
            alternative_documents,
            ..
        } => alternative_documents,
        _ => unreachable!(),
    })?;
    let current_alternatives = sage_sys::AlternativesDocument::load_installed(root)?;
    sage_sys::ProfileEngine::reconcile_alternatives(
        root,
        &previous_alternatives,
        &current_alternatives,
    )?;
    let accounts = sage_sys::SysusersDocument::load_installed(root)?;
    sage_sys::SysusersEngine::reconcile(root, &accounts)?;
    journal.advance("triggers");
    database.write_journal(journal)?;
    crash_point(root, "alternatives")?;
    }
    if journal.stage == "triggers" {
    let triggers = match &journal.action {
        sage_db::JournalAction::Remove {
            trigger_documents, ..
        } => trigger_documents
            .iter()
            .map(|bytes| sage_sys::TriggerSpec::parse(bytes))
            .collect::<Result<Vec<_>, _>>()?,
        _ => unreachable!(),
    };
    crash_point(root, "triggers")?;
    sage_sys::TriggerEngine::execute_triggers_for(
        &triggers,
        &modified,
        root,
        sage_sys::TriggerEvent::PostRemove,
    )?;
    journal.advance("complete");
    database.write_journal(journal)?;
    crash_point(root, "trigger-complete")?;
    }
    database.finish_journal(&journal.op_id)?;
    Ok(())
}
fn should_preserve_config(
    path: &Path,
    physical: &str,
    hashes: &BTreeMap<String, String>,
) -> Result<bool> {
    let Some((_, expected)) = hashes
        .iter()
        .find(|(relative, _)| Path::new(physical).ends_with(relative))
    else {
        return Ok(false);
    };
    if !path.exists() {
        return Ok(false);
    }
    let bytes = std::fs::read(path)?;
    Ok(hex::encode(Sha256::digest(bytes)) != *expected)
}
fn remove_file_beneath(root: &Path, path: &Path) -> Result<()> {
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
fn write_atomic_under_root(root: &Path, relative: &Path, bytes: &[u8]) -> Result<()> {
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
async fn rebuild_system(root: &Path, no_prune: bool, dry_run: bool) -> Result<()> {
    let config_path = under_root(root, Path::new("/etc/sage/system.toml"));
    let config = sage_sys::SystemConfig::load(&config_path)?;
    let available = load_available_with_pool(root, Some(&config.system.architecture), None)?;
    let db_path = under_root(root, Path::new("/var/lib/sage"));
    let installed = if dry_run {
        sage_db::read_packages(&db_path)?
    } else {
        sage_db::SageDatabase::open(&db_path)?.packages()?
    };
    let plan =
        sage_sys::ReconcilePlan::compute(&config, &installed, &available.universe, no_prune)?;
    let current = installed
        .iter()
        .map(|package| (package.key.clone(), package.version.clone()))
        .collect::<BTreeMap<_, _>>();
    let changes = installation_order(&available, plan.install.iter().cloned().collect())?;
    let retired = installed
        .iter()
        .filter(|package| plan.remove.contains(&package.key))
        .cloned()
        .collect::<Vec<_>>();
    for (key, version) in &changes {
        println!(
            "{} {key} {version}",
            if current.contains_key(key) { "Upgrade" } else { "Install" }
        );
    }
    for package in &retired {
        println!("Remove {} {}", package.key, package.version);
    }
    if !dry_run && (!changes.is_empty() || !retired.is_empty()) {
        let database = sage_db::SageDatabase::open(&db_path)?;
        publish_packages(
            root,
            &database,
            &available,
            &config.system.architecture,
            &changes,
            &retired,
        )
        .await?;
    }
    if dry_run {
        for (interface, key) in &plan.provider_bindings {
            println!("Would bind virtual/{interface} to {key}");
        }
    } else {
        sage_db::SageDatabase::open(&db_path)?
            .replace_system_providers(&plan.provider_bindings)?;
    }
    let init_provider = plan
        .provider_bindings
        .get("init")
        .context("system providers must select an init implementation")?;
    render_services(root, &config, &init_provider.name, dry_run)?;
    if !dry_run {
        let triggers = sage_sys::TriggerEngine::load_triggers(root)?;
        sage_sys::TriggerEngine::execute_triggers_for(
            &triggers,
            &[PathBuf::from("etc/sage/system.toml")],
            root,
            sage_sys::TriggerEvent::Rebuild,
        )?;
    }
    Ok(())
}
fn render_services(
    root: &Path,
    config: &sage_sys::SystemConfig,
    provider: &str,
    dry_run: bool,
) -> Result<()> {
    let rclass = under_root(
        root,
        &Path::new("/usr/share/sage/rclass").join(format!("init-{provider}.toml")),
    );
    let generator = sage_sys::TemplateServiceGenerator::from_rclass(&rclass)?;
    let service_dir = under_root(root, Path::new("/usr/share/sage/services"));
    let mut installed = Vec::new();
    if service_dir.exists() {
        let mut declarations = std::fs::read_dir(&service_dir)?.collect::<Result<Vec<_>, _>>()?;
        declarations.sort_by_key(std::fs::DirEntry::file_name);
        for declaration in declarations {
            if declaration.path().extension().and_then(|value| value.to_str()) == Some("toml") {
                installed.push(sage_sys::ServiceSpec::load(declaration.path())?);
            }
        }
    }
    let installed_names = installed
        .iter()
        .map(|service| service.name.as_str())
        .collect::<std::collections::BTreeSet<_>>();
    if let Some(service) = config
        .services
        .iter()
        .find(|service| !installed_names.contains(service.as_str()))
    {
        bail!("enabled service '{service}' has no installed declaration");
    }
    let state_relative = Path::new("var/lib/sage/rendered-services.toml");
    let state_path = under_root(root, state_relative);
    let previous = if state_path.exists() {
        Some(sage_sys::RenderedServicesState::load(&state_path)?)
    } else {
        None
    };
    if dry_run {
        if let Some(previous) = &previous {
            for service in &previous.services {
                let provider_changed = previous.provider != provider;
                let removed = !installed
                    .iter()
                    .any(|candidate| candidate.name == service.name);
                let disabled = previous.enabled.contains(&service.name)
                    && !config.services.contains(&service.name);
                if provider_changed || removed || disabled {
                    println!(
                        "Would disable service {} from init provider {}",
                        service.name, previous.provider
                    );
                }
                if provider_changed || removed {
                    println!(
                        "Would remove stale native definition for service {}",
                        service.name
                    );
                }
            }
        }
        for service in &installed {
            println!("Would render service {} with {}", service.name, rclass.display());
        }
        return Ok(());
    }
    if let Some(previous) = &previous {
        let previous_rclass = under_root(
            root,
            &Path::new("/usr/share/sage/rclass")
                .join(format!("init-{}.toml", previous.provider)),
        );
        let previous_generator =
            sage_sys::TemplateServiceGenerator::from_rclass(&previous_rclass)?;
        for service in &previous.services {
            let provider_changed = previous.provider != provider;
            let removed = !installed
                .iter()
                .any(|candidate| candidate.name == service.name);
            let disabled = previous.enabled.contains(&service.name)
                && !config.services.contains(&service.name);
            if provider_changed || removed || disabled {
                previous_generator.disable_service(service, root)?;
            }
            if provider_changed || removed {
                previous_generator.remove_service(service, root)?;
            }
        }
    }
    generator.render_service_set(&installed, root)?;
    for service in &installed {
        if config.services.contains(&service.name) {
            generator.enable_service(service, root)?;
        }
    }
    let state = sage_sys::RenderedServicesState {
        schema_version: sage_core::SCHEMA_VERSION,
        provider: provider.into(),
        services: installed,
        enabled: config.services.clone(),
    };
    write_atomic_under_root(root, state_relative, toml::to_string_pretty(&state)?.as_bytes())?;
    Ok(())
}
fn operation_id(kind: &str) -> Result<String> {
    static COUNTER: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)?
        .as_nanos();
    let sequence = COUNTER.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    Ok(format!(
        "{kind}-{}-{nanos}-{sequence}",
        std::process::id()
    ))
}
fn list_channels(root: &Path) -> Result<()> {
    let config =
        sage_repo::ChannelsConfig::load(under_root(root, Path::new("/etc/sage/channels.toml")))?;
    for (name, channel) in config.channels {
        println!(
            "{}\t{}\t{}",
            name,
            if channel.enabled {
                "enabled"
            } else {
                "disabled"
            },
            channel.url
        );
        for (sub_name, subchannel) in channel.subchannels {
            println!(
                "  {}/{}\t{}\t{}",
                name,
                subchannel.alias.as_deref().unwrap_or(&sub_name),
                if subchannel.enabled {
                    "enabled"
                } else {
                    "disabled"
                },
                subchannel.target_root.display()
            );
        }
    }
    Ok(())
}
fn use_toolchain(root: &Path, channel: &str, dry_run: bool) -> Result<()> {
    if channel.is_empty()
        || !channel
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'_'))
    {
        bail!("invalid toolchain channel '{channel}'");
    }
    let source = under_root(root, &Path::new("/opt/channels").join(channel).join("bin"));
    let mut entries: Vec<_> = std::fs::read_dir(&source)
        .with_context(|| format!("toolchain has no bin directory: {}", source.display()))?
        .collect::<Result<_, _>>()?;
    entries.sort_by_key(|entry| entry.file_name());
    let links: BTreeMap<_, _> = entries
        .into_iter()
        .filter_map(|entry| {
            entry
                .file_type()
                .ok()
                .filter(|kind| kind.is_file() || kind.is_symlink())
                .map(|_| {
                    (
                        PathBuf::from("bin").join(entry.file_name()),
                        Path::new("/opt/channels")
                            .join(channel)
                            .join("bin")
                            .join(entry.file_name()),
                    )
                })
        })
        .collect();
    let system =
        sage_sys::SystemConfig::load(under_root(root, Path::new("/etc/sage/system.toml")))?;
    if dry_run {
        println!(
            "Would activate {} tools from {} in profile {}",
            links.len(),
            channel,
            system.system.profile
        );
    } else {
        sage_sys::ProfileEngine::apply_profile(root, &system.system.profile, &links)?;
        println!("Activated toolchain {channel}");
    }
    Ok(())
}
fn query_state(root: &Path, action: QueryAction) -> Result<()> {
    let db_path = under_root(root, Path::new("/var/lib/sage"));
    match action {
        QueryAction::Installed => {
            for package in sage_db::read_packages(&db_path)? {
                println!("{}\t{}\t{}", package.key, package.version, package.arch);
            }
        }
        QueryAction::Owner { path } => {
            let relative = path.strip_prefix(root).unwrap_or(path.as_path());
            let relative = relative.strip_prefix("/").unwrap_or(relative);
            for owner in sage_db::read_owners(&db_path, &relative.to_string_lossy())? {
                println!("{owner}");
            }
        }
        QueryAction::Info { package, channel } => {
            let channel = if channel.contains('/') {
                channel
            } else {
                format!("main/{channel}")
            };
            let key = sage_core::PackageKey::in_channel(channel, &package)?;
            let record = sage_db::read_packages(&db_path)?
                .into_iter()
                .find(|record| record.key == key)
                .with_context(|| format!("package {key} is not installed"))?;
            println!("Package: {}", record.key);
            println!("Version: {}", record.version);
            println!("Architecture: {}", record.arch);
            println!("Installed size: {}", record.installed_size);
            println!("Files: {}", record.files.len());
            for dependency in record.dependencies {
                println!("Depends: {}", dependency.name);
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod package_ops_tests {
    use super::*;

    #[test]
    fn service_rendering_uses_the_resolved_init_provider() {
        let root = tempfile::tempdir().unwrap();
        let rclass = root.path().join("usr/share/sage/rclass");
        std::fs::create_dir_all(&rclass).unwrap();
        std::fs::write(
            rclass.join("init-loom.toml"),
            "schema_version=1\n[service_generator]\ntarget_path=\"/etc/loom/${service.name}\"\nmode=420\ntemplate=\"\"\n",
        )
        .unwrap();
        let config = sage_sys::SystemConfig {
            schema_version: 1,
            system: sage_sys::SystemMetadata {
                architecture: "amd64".into(),
                profile: "default".into(),
            },
            providers: BTreeMap::from([("init".into(), "systemd".into())]),
            packages: BTreeSet::new(),
            services: BTreeSet::new(),
        };

        render_services(root.path(), &config, "loom", false).unwrap();
        let state = sage_sys::RenderedServicesState::load(
            root.path().join("var/lib/sage/rendered-services.toml"),
        )
        .unwrap();
        assert_eq!(state.provider, "loom");
    }
}
