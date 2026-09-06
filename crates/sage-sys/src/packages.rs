use crate as sage_sys;
pub use sage_repo::{ReleaseLocation, ReleaseSource};

pub async fn sync_channels(root: &Path, selected: Option<&str>, dry_run: bool) -> Result<()> {
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
    sage_core::under_root(root, path)
}
fn declaration_path(dir: &str, key: &sage_core::PackageKey) -> PathBuf {
    let digest = hex::encode(Sha256::digest(key.canonical_id().as_bytes()));
    PathBuf::from(dir).join(format!("{digest}.toml"))
}
fn arch_matches(arch: &str, wanted: &str) -> bool {
    arch == wanted || arch == "any" || arch == "noarch"
}
fn is_package_archive(path: &Path) -> bool {
    path.file_name()
        .and_then(|name| name.to_str())
        .is_some_and(|name| name.ends_with(".pkg.tar.zst"))
}
fn qualify_channel(channel: &str) -> String {
    if channel.contains('/') {
        channel.to_string()
    } else {
        format!("main/{channel}")
    }
}
fn installed_packages(db_path: &Path, dry_run: bool) -> Result<Vec<sage_db::InstalledPackage>> {
    if dry_run {
        Ok(sage_db::read_packages(db_path)?)
    } else {
        Ok(sage_db::SageDatabase::open(db_path)?.packages()?)
    }
}
fn read_toml_files(dir: &Path) -> Result<Vec<PathBuf>> {
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
fn read_documents(root: &Path, relative: &Path) -> Result<Vec<Vec<u8>>> {
    read_toml_files(&under_root(root, relative))?
        .into_iter()
        .map(|path| std::fs::read(path).map_err(Into::into))
        .collect()
}
fn alternatives_from_documents(documents: &[Vec<u8>]) -> Result<Vec<sage_sys::Alternative>> {
    documents.iter().try_fold(Vec::new(), |mut acc, doc| {
        acc.extend(sage_sys::AlternativesDocument::parse(doc)?.alternatives());
        Ok(acc)
    })
}
fn trigger_documents(root: &Path) -> Result<Vec<Vec<u8>>> {
    sage_sys::TriggerEngine::load_triggers(root)?
        .into_iter()
        .map(|trigger| Ok(toml::to_string(&trigger)?.into_bytes()))
        .collect()
}
fn settle_journal_alternatives(
    root: &Path,
    database: &sage_db::SageDatabase,
    journal: &mut sage_db::JournalRecord,
    previous_alternatives: &[sage_sys::Alternative],
) -> Result<()> {
    if journal.stage == "alternatives" {
        let current = sage_sys::AlternativesDocument::load_installed(root)?;
        sage_sys::ProfileEngine::reconcile_alternatives(root, previous_alternatives, &current)?;
        let accounts = sage_sys::SysusersDocument::load_installed(root)?;
        sage_sys::SysusersEngine::reconcile(root, &accounts)?;
        journal.advance("triggers");
        database.write_journal(journal)?;
        crash_point(root, "alternatives")?;
    }
    Ok(())
}
fn settle_journal_triggers(
    root: &Path,
    database: &sage_db::SageDatabase,
    journal: &mut sage_db::JournalRecord,
    modified: &[PathBuf],
    triggers: &[sage_sys::TriggerSpec],
    event: sage_sys::TriggerEvent,
) -> Result<()> {
    if journal.stage == "triggers" {
        crash_point(root, "triggers")?;
        sage_sys::TriggerEngine::execute_triggers_for(triggers, modified, root, event)?;
        journal.advance(
            if matches!(
                &journal.action,
                sage_db::JournalAction::Install {
                    rebuild: Some(_),
                    ..
                }
            ) {
                "rebuild-removal-triggers"
            } else {
                "complete"
            },
        );
        database.write_journal(journal)?;
        crash_point(root, "trigger-complete")?;
    }
    Ok(())
}
/// One-shot sysroot-local crash injection; the next startup exercises recovery.
#[cfg(feature = "torture")]
fn crash_point(root: &Path, stage: &str) -> Result<()> {
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
fn crash_point(_root: &Path, _stage: &str) -> Result<()> {
    Ok(())
}
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
pub struct AvailablePackages {
    pub universe: sage_solver::PackageUniverse,
    pub releases: BTreeMap<(sage_core::PackageKey, sage_core::Version), ReleaseSource>,
    pub aliases: BTreeMap<String, String>,
}

impl AvailablePackages {
    fn register_installed(&mut self, installed: &[sage_db::InstalledPackage]) {
        for package in installed {
            if !self
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
                self.universe.insert(release);
            }
        }
    }
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
                if architecture.is_some_and(|wanted| !arch_matches(&release.package.arch, wanted)) {
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
        let mut packages: Vec<_> = sage_core::walkdir::WalkDir::new(pool)
            .follow_links(false)
            .into_iter()
            .filter_map(|entry| entry.ok())
            .map(|entry| entry.into_path())
            .filter(|path| is_package_archive(path))
            .collect();
        packages.sort();
        for path in packages {
            let inspection = sage_archive::inspect_package(&path)
                .with_context(|| format!("failed to inspect local package {}", path.display()))?;
            if architecture.is_some_and(|wanted| !arch_matches(&inspection.manifest.arch, wanted)) {
                continue;
            }
            let canonical = aliases
                .get(&inspection.manifest.channel)
                .cloned()
                .unwrap_or_else(|| qualify_channel(&inspection.manifest.channel));
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

pub fn canonical_channel(available: &AvailablePackages, selected: Option<&str>) -> Result<String> {
    let selected = selected.unwrap_or("system");
    available
        .aliases
        .get(selected)
        .cloned()
        .with_context(|| format!("channel '{selected}' has no synchronized index"))
}
pub async fn apply_packages(
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
async fn publish_packages(
    root: &Path,
    database: &sage_db::SageDatabase,
    available: &AvailablePackages,
    architecture: &str,
    changes: &[(sage_core::PackageKey, sage_core::Version)],
) -> Result<()> {
    let changes = preflight_packages(root, database, available, architecture, changes, &[]).await?;
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

struct PackageDeclarations {
    entries: Vec<(PathBuf, Vec<u8>)>,
}

impl PackageDeclarations {
    fn parse(
        inspection: &sage_archive::PackageInspection,
        key: &sage_core::PackageKey,
    ) -> Result<Self> {
        let mut entries = Vec::new();
        if let Some(bytes) = inspection.optional.get(".METADATA/service.toml") {
            for service in sage_sys::ServiceDocument::parse(bytes)?.into_services() {
                let path = PathBuf::from(format!("usr/share/sage/services/{}.toml", service.name));
                let doc = sage_sys::ServiceDocument {
                    schema_version: sage_core::SCHEMA_VERSION,
                    service: Some(service),
                    services: Vec::new(),
                };
                entries.push((path, toml::to_string_pretty(&doc)?.into_bytes()));
            }
        }
        if let Some(bytes) = inspection.optional.get(".METADATA/triggers.toml") {
            let trigger = sage_sys::TriggerSpec::parse(bytes)?;
            entries.push((
                PathBuf::from(format!("usr/share/sage/triggers/{}.toml", trigger.name)),
                bytes.clone(),
            ));
        }
        if let Some(bytes) = inspection.optional.get(".METADATA/alternatives.toml") {
            let mut document = sage_sys::AlternativesDocument::parse(bytes)?;
            document.package.clone_from(key);
            entries.push((
                declaration_path("usr/share/sage/alternatives", key),
                toml::to_string_pretty(&document)?.into_bytes(),
            ));
        }
        if let Some(bytes) = inspection.optional.get(".METADATA/sysusers.toml") {
            let mut document = sage_sys::SysusersDocument::parse(bytes)?;
            document.package.clone_from(key);
            entries.push((
                declaration_path("usr/share/sage/sysusers", key),
                toml::to_string_pretty(&document)?.into_bytes(),
            ));
        }
        Ok(Self { entries })
    }

    fn ownership_paths(&self) -> impl Iterator<Item = String> + '_ {
        self.entries
            .iter()
            .map(|(path, _)| path.to_string_lossy().into_owned())
    }

    fn write_under_root(&self, root: &Path) -> Result<()> {
        for (path, bytes) in &self.entries {
            write_atomic_under_root(root, path, bytes)?;
        }
        Ok(())
    }
}

fn package_ownership(
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

/// Validates and orders the complete transaction before creating a durable recovery record.
/// A rejected archive or ownership conflict has made no filesystem or LMDB
/// mutation, so it must not become an endlessly retried startup journal.
pub async fn obtain_release_archive(
    engine: &sage_repo::DownloadEngine,
    cache: &Path,
    source: &ReleaseSource,
) -> Result<PathBuf> {
    match &source.location {
        ReleaseLocation::Local(path) => Ok(path.clone()),
        ReleaseLocation::Remote(base) => {
            let archive = cache.join(&source.release.sha256);
            let url = format!("{}/{}", base.trim_end_matches('/'), source.release.archive);
            engine
                .download_url(&url, &archive, &source.release.sha256)
                .await?;
            Ok(archive)
        }
    }
}

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
async fn resume_install(
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
        sage_sys::TriggerEngine::load_triggers(root)?
    } else {
        Vec::new()
    };
    settle_journal_triggers(
        root,
        database,
        journal,
        &modified,
        &triggers,
        sage_sys::TriggerEvent::PostChange,
    )?;
    if let Some(work) = &rebuild {
        resume_rebuild(root, database, journal, work)?;
    }
    database.finish_journal(&journal.op_id)?;
    Ok(())
}
pub async fn upgrade_packages(
    root: &Path,
    names: &[String],
    channel: Option<&str>,
    dry_run: bool,
) -> Result<()> {
    let config =
        sage_sys::SystemConfig::load(under_root(root, Path::new("/etc/sage/system.toml")))?;
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
    let database = sage_db::SageDatabase::open(&db_path)?;
    for (interface, provider) in database.system_providers()? {
        if requested.contains(&provider) {
            bail!(
                "cannot remove bound provider {provider} for {}; switch providers with rebuild first",
                provider_symbol(&interface)
            );
        }
    }
    drop(database);
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
            alternative_documents: read_documents(root, Path::new("usr/share/sage/alternatives"))?,
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
                .map(|bytes| sage_sys::TriggerSpec::parse(bytes))
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
        sage_sys::TriggerEvent::PostRemove,
    )?;
    database.finish_journal(&journal.op_id)?;
    Ok(())
}
fn should_preserve_config(
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
/// Checks a program against the final payload/retained-file overlay without
/// publishing it. Resolve every symlink component in that same overlay so a
/// retained link cannot conceal a removed target or escape the target root.
fn validate_planned_program(
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
    for (key, version) in &changes {
        println!("Install {key} {version}");
    }
    for package in &retired {
        println!("Remove {} {}", package.key, package.version);
    }
    for (interface, key) in &plan.provider_bindings {
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
        &changes,
        &retired,
    )
    .await?;
    let mut work = sage_db::RebuildContinuation {
        provider_bindings: plan.provider_bindings,
        retired_packages: retired,
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
    let final_owned = installed
        .iter()
        .filter(|package| {
            !changes.iter().any(|(key, _)| *key == package.key)
                && !retired.iter().any(|old| old.key == package.key)
        })
        .flat_map(|package| package.files.iter().map(PathBuf::from))
        .chain(payloads.keys().chain(documents.keys()).cloned())
        .collect::<BTreeSet<_>>();
    let mut targets = BTreeSet::<PathBuf>::new();
    for service in &services {
        let target = generator.rendered_path(service, root)?;
        let relative = target.strip_prefix(root)?;
        // Native output must not replace a package's command, data, or parent
        // directory. This checks the final overlay, before any old cleanup.
        if final_owned
            .iter()
            .any(|path| path.starts_with(relative) || relative.starts_with(path))
        {
            bail!(
                "native service output conflicts with package ownership: {}",
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
        if final_owned
            .iter()
            .any(|path| path.starts_with(relative) || relative.starts_with(path))
        {
            bail!(
                "managed service directory conflicts with package-owned files: {}",
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
                    if removed.contains(relative)
                        && payloads
                            .keys()
                            .any(|path| path != relative && path.starts_with(relative))
                    {
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
/// Persist progress after each action; an interrupted command itself must be
/// retry-safe, while a checkpointed disable is never replayed after retirement.
fn cleanup_services(
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

/// Completes the original rebuild tail, including when recovery was triggered by
/// another command or the live system.toml has changed since the interruption.
fn resume_rebuild(
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
        journal.advance("complete");
        database.write_journal(journal)?;
        crash_point(root, "rebuild-triggers")?;
    }
    Ok(())
}
fn operation_id(kind: &str) -> Result<String> {
    static COUNTER: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)?
        .as_nanos();
    let sequence = COUNTER.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    Ok(format!("{kind}-{}-{nanos}-{sequence}", std::process::id()))
}
pub fn list_channels(root: &Path) -> Result<()> {
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
pub fn use_toolchain(root: &Path, channel: &str, dry_run: bool) -> Result<()> {
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

/// Query actions for installed system state.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum QueryAction {
    Installed,
    Owner { path: PathBuf },
    Info { package: String, channel: String },
}

pub fn query_installed(root: &Path) -> Result<()> {
    let db_path = under_root(root, Path::new("/var/lib/sage"));
    for package in sage_db::read_packages(&db_path)? {
        println!("{}\t{}\t{}", package.key, package.version, package.arch);
    }
    Ok(())
}

pub fn query_owner(root: &Path, path: &Path) -> Result<()> {
    let db_path = under_root(root, Path::new("/var/lib/sage"));
    let relative = path.strip_prefix(root).unwrap_or(path);
    let relative = relative.strip_prefix("/").unwrap_or(relative);
    for owner in sage_db::read_owners(&db_path, &relative.to_string_lossy())? {
        println!("{owner}");
    }
    Ok(())
}

pub fn query_info(root: &Path, package: &str, channel: &str) -> Result<()> {
    let db_path = under_root(root, Path::new("/var/lib/sage"));
    let channel = qualify_channel(channel);
    let key = sage_core::PackageKey::in_channel(channel, package)?;
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
    Ok(())
}

pub fn query_state(root: &Path, action: QueryAction) -> Result<()> {
    match action {
        QueryAction::Installed => query_installed(root),
        QueryAction::Owner { path } => query_owner(root, &path),
        QueryAction::Info { package, channel } => query_info(root, &package, &channel),
    }
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
        std::fs::write(stage.path().join(".METADATA/manifest.toml"),
            "schema_version=1\nname=\"program\"\nversion=\"1\"\nrelease=1\narch=\"noarch\"\nchannel=\"system\"\ndescription=\"Program\"\nlicense=\"MIT\"\n").unwrap();
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
