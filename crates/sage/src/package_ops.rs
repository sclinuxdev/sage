//! Package installation, removal, upgrade, reconciliation, and state queries.

use anyhow::{bail, Context, Result};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use crate::package_model::{
    canonical_channel, load_available, obtain_release_archive, AvailablePackages,
};
use crate::paths::under_root;
use crate::QueryAction;

pub(crate) struct PackageManager<'a> {
    root: &'a Path,
    dry_run: bool,
}

impl<'a> PackageManager<'a> {
    pub(crate) fn new(root: &'a Path, dry_run: bool) -> Self {
        Self { root, dry_run }
    }

    pub(crate) fn settle_journals(&self) -> Result<()> {
        settle_journals(self.root)
    }

    pub(crate) fn is_dry_run(&self) -> bool {
        self.dry_run
    }

    pub(crate) async fn install(
        &self,
        names: &[String],
        channel: Option<&str>,
        _no_save: bool,
    ) -> Result<()> {
        apply_packages(self.root, names, channel, false, self.dry_run).await
    }

    pub(crate) fn remove(&self, names: &[String], channel: Option<&str>) -> Result<()> {
        remove_packages(self.root, names, channel, self.dry_run)
    }

    pub(crate) async fn upgrade(&self, names: &[String], channel: Option<&str>) -> Result<()> {
        upgrade_packages(self.root, names, channel, self.dry_run).await
    }

    pub(crate) async fn rebuild(&self, no_prune: bool) -> Result<()> {
        rebuild_system(self.root, no_prune, self.dry_run).await
    }

    pub(crate) async fn sync_channels(&self, selected: Option<&str>) -> Result<()> {
        sync_channels(self.root, selected, self.dry_run).await
    }

    pub(crate) fn list_channels(&self) -> Result<()> {
        list_channels(self.root)
    }

    pub(crate) fn use_toolchain(&self, channel: &str) -> Result<()> {
        use_toolchain(self.root, channel, self.dry_run)
    }

    pub(crate) fn query(&self, action: QueryAction) -> Result<()> {
        query_state(self.root, action)
    }
}

fn settle_journals(root: &Path) -> Result<()> {
    let path = under_root(root, Path::new("/var/lib/sage"));
    if !path.exists() {
        return Ok(());
    }
    let database = sage_db::SageDatabase::open(path)?;
    for journal in database.pending_journals()? {
        let present = journal
            .affected_packages
            .iter()
            .map(|key| database.package(key))
            .collect::<Result<Vec<_>, _>>()?;
        let complete = match journal.stage.as_str() {
            "publish" => present.iter().all(Option::is_some),
            "remove" => present.iter().all(Option::is_none),
            _ => false,
        };
        if complete {
            database.finish_journal(&journal.op_id)?;
            eprintln!("Recovered completed operation {}", journal.op_id);
        } else {
            eprintln!(
                "Resuming convergence with unfinished operation {} ({})",
                journal.op_id, journal.stage
            );
        }
    }
    Ok(())
}

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

async fn apply_packages(
    root: &Path,
    names: &[String],
    channel: Option<&str>,
    prefer_latest: bool,
    dry_run: bool,
) -> Result<()> {
    let available = load_available(root)?;
    let channel = canonical_channel(&available, channel)?;
    let requested: Vec<_> = names
        .iter()
        .map(|name| sage_core::PackageKey::new(&channel, name, sage_core::DEFAULT_SLOT))
        .collect();
    let db_path = under_root(root, Path::new("/var/lib/sage"));
    let installed = if dry_run {
        sage_db::read_packages(&db_path)?
    } else {
        sage_db::SageDatabase::open(&db_path)?.packages()?
    };
    let solution = if prefer_latest {
        sage_solver::SageSolver::new(&available.universe).resolve(&requested)?
    } else {
        let locks = installed
            .iter()
            .map(|package| (package.key.clone(), package.version.clone()));
        sage_solver::SageSolver::with_locked(&available.universe, locks).resolve(&requested)?
    };
    let current: BTreeMap<_, _> = installed
        .iter()
        .map(|package| (package.key.clone(), package.version.clone()))
        .collect();
    let changes: Vec<_> = solution
        .into_iter()
        .map(|(key, version)| sage_core::PackageCoordinate::new(key, version))
        .filter(|coordinate| current.get(&coordinate.key) != Some(&coordinate.version))
        .collect();
    for coordinate in &changes {
        println!(
            "{} {} {}",
            if current.contains_key(&coordinate.key) {
                "Upgrade"
            } else {
                "Install"
            },
            coordinate.key,
            coordinate.version
        );
    }
    if dry_run || changes.is_empty() {
        return Ok(());
    }
    let database = sage_db::SageDatabase::open(&db_path)?;
    publish_packages(root, &database, &available, &changes).await
}

async fn publish_packages(
    root: &Path,
    database: &sage_db::SageDatabase,
    available: &AvailablePackages,
    changes: &[sage_core::PackageCoordinate],
) -> Result<()> {
    let timestamp = unix_timestamp()?;
    let op_id = format!("install-{}-{timestamp}", std::process::id());
    let digest = hex::encode(Sha256::digest(format!("{changes:?}").as_bytes()));
    database.write_journal(&sage_db::JournalRecord {
        op_id: op_id.clone(),
        stage: "publish".into(),
        affected_packages: changes
            .iter()
            .map(|coordinate| coordinate.key.clone())
            .collect(),
        journal_sha256: digest,
        timestamp,
    })?;
    let package_cache = under_root(root, Path::new("/var/cache/sage/packages"));
    let engine = sage_repo::DownloadEngine::new(&package_cache)?;
    let mut modified = Vec::new();
    for coordinate in changes {
        let key = &coordinate.key;
        let version = &coordinate.version;
        let source = available
            .releases
            .get(coordinate)
            .with_context(|| format!("index record disappeared for {key} {version}"))?;
        let archive = obtain_release_archive(&engine, &package_cache, source).await?;
        let inspection = sage_archive::inspect_package(&archive)?;
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
        let service = if let Some(bytes) = inspection.optional.get(".METADATA/service.toml") {
            Some((sage_sys::ServiceSpec::parse(bytes)?, bytes.clone()))
        } else {
            None
        };
        let trigger = if let Some(bytes) = inspection.optional.get(".METADATA/triggers.toml") {
            Some((sage_sys::TriggerSpec::parse(bytes)?, bytes.clone()))
        } else {
            None
        };
        if let Some((service, _)) = &service {
            ownership.push(format!("usr/share/sage/services/{}.toml", service.name));
        }
        if let Some((trigger, _)) = &trigger {
            ownership.push(format!("usr/share/sage/triggers/{}.toml", trigger.name));
        }
        for path in &ownership {
            let owners = database.owners(path)?;
            if owners.iter().any(|owner| owner != key) {
                bail!("file conflict for {path}: {owners:?}");
            }
        }
        let previous_package = database.package(key)?;
        let previous = previous_package
            .as_ref()
            .map(|package| package.config_hashes.clone())
            .unwrap_or_default();
        let report = sage_archive::extract_package_with_config(
            &archive,
            &target,
            &inspection.files,
            &previous,
        )?;
        if let Some((service, bytes)) = service {
            write_atomic_under_root(
                root,
                &Path::new("usr/share/sage/services").join(format!("{}.toml", service.name)),
                &bytes,
            )?;
        }
        if let Some((trigger, bytes)) = trigger {
            write_atomic_under_root(
                root,
                &Path::new("usr/share/sage/triggers").join(format!("{}.toml", trigger.name)),
                &bytes,
            )?;
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
        database.install(
            &sage_db::InstalledPackage {
                key: key.clone(),
                version: version.clone(),
                arch: source.release.manifest.arch.clone(),
                installed_size: source.release.manifest.installed_size,
                dependencies: source.release.dependencies.clone(),
                provides: source.release.manifest.provides.clone(),
                files: ownership.clone(),
                config_hashes,
            },
            false,
        )?;
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
    let triggers = sage_sys::TriggerEngine::load_triggers(root)?;
    sage_sys::TriggerEngine::execute_triggers_for(
        &triggers,
        &modified,
        root,
        sage_sys::TriggerEvent::PostChange,
    )?;
    database.finish_journal(&op_id)?;
    Ok(())
}

async fn upgrade_packages(
    root: &Path,
    names: &[String],
    channel: Option<&str>,
    dry_run: bool,
) -> Result<()> {
    let available = load_available(root)?;
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
            .map(|package| package.key.name)
            .collect()
    } else {
        names.to_vec()
    };
    apply_packages(root, &names, Some(&canonical), true, dry_run).await
}

fn remove_packages(
    root: &Path,
    names: &[String],
    channel: Option<&str>,
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
    let selected: Vec<_> = installed
        .iter()
        .filter(|package| {
            package.key.channel == canonical && names.iter().any(|name| name == &package.key.name)
        })
        .cloned()
        .collect();
    if selected.len() != names.len() {
        bail!("one or more requested packages are not installed in {canonical}");
    }
    for dependent in &installed {
        if selected.iter().any(|removed| {
            dependent.key != removed.key
                && dependent.dependencies.iter().any(|dependency| {
                    (dependency.name == removed.key.name
                        || removed.provides.contains(&dependency.name))
                        && dependency
                            .channel
                            .as_deref()
                            .is_none_or(|channel| channel == removed.key.channel)
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
    // Keep declarations in memory before package-owned trigger files disappear;
    // post-remove actions must observe the completed filesystem transaction.
    let triggers = sage_sys::TriggerEngine::load_triggers(root)?;
    let database = sage_db::SageDatabase::open(&db_path)?;
    let timestamp = unix_timestamp()?;
    let op_id = format!("remove-{}-{timestamp}", std::process::id());
    database.write_journal(&sage_db::JournalRecord {
        op_id: op_id.clone(),
        stage: "remove".into(),
        affected_packages: selected.iter().map(|package| package.key.clone()).collect(),
        journal_sha256: hex::encode(Sha256::digest(format!("{names:?}").as_bytes())),
        timestamp,
    })?;
    let mut modified = Vec::new();
    for package in selected {
        database.remove(&package.key)?;
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
        }
    }
    sage_sys::TriggerEngine::execute_triggers_for(
        &triggers,
        &modified,
        root,
        sage_sys::TriggerEvent::PostRemove,
    )?;
    database.finish_journal(&op_id)?;
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
    let mut desired: Vec<_> = config.packages.iter().cloned().collect();
    desired.extend(config.providers.values().cloned());
    desired.sort();
    desired.dedup();
    apply_packages(root, &desired, Some("system"), false, dry_run).await?;
    let available = load_available(root)?;
    let db_path = under_root(root, Path::new("/var/lib/sage"));
    let installed = if dry_run {
        sage_db::read_packages(&db_path)?
    } else {
        sage_db::SageDatabase::open(&db_path)?.packages()?
    };
    let plan =
        sage_sys::ReconcilePlan::compute(&config, &installed, &available.universe, no_prune)?;
    let names: Vec<_> = plan.remove.into_iter().map(|key| key.name).collect();
    if !names.is_empty() {
        remove_packages(root, &names, Some("main/system"), dry_run)?;
    }
    let database = if dry_run {
        None
    } else {
        Some(sage_db::SageDatabase::open(&db_path)?)
    };
    for (interface, key) in plan.provider_bindings {
        if dry_run {
            println!("Would bind virtual/{interface} to {key}");
        } else {
            database
                .as_ref()
                .expect("non-dry rebuild opens the database")
                .set_system_provider(&interface, &key)?;
        }
    }
    render_services(root, &config, dry_run)?;
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

fn render_services(root: &Path, config: &sage_sys::SystemConfig, dry_run: bool) -> Result<()> {
    let provider = config
        .providers
        .get("init")
        .context("system providers must select an init implementation")?;
    let rclass = under_root(
        root,
        &Path::new("/usr/share/sage/rclass").join(format!("init-{provider}.toml")),
    );
    if dry_run {
        for service in &config.services {
            println!("Would render service {service} with {}", rclass.display());
        }
        return Ok(());
    }
    let generator = sage_sys::TemplateServiceGenerator::from_rclass(&rclass)?;
    let service_dir = under_root(root, Path::new("/usr/share/sage/services"));
    for service in &config.services {
        let path = service_dir.join(format!("{service}.toml"));
        if !path.exists() {
            bail!("enabled service '{service}' has no installed declaration");
        }
        let service = sage_sys::ServiceSpec::load(path)?;
        generator.render_service(&service, root)?;
        generator.enable_service(&service, root)?;
    }
    Ok(())
}

fn unix_timestamp() -> Result<u64> {
    Ok(std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)?
        .as_secs())
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
            let relative = path.strip_prefix(root).unwrap_or(&path);
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
            let key = sage_core::PackageKey::new(channel, package, sage_core::DEFAULT_SLOT);
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
