//! Main entry point for the Sage package manager CLI.
//!
//! Provides fast, pure CLI argument parsing based on `clap` derive,
//! supporting package installation, atomic upgrades, source builds,
//! repository index generation, and declarative system reconciliation.

use anyhow::{bail, Context, Result};
use clap::{Parser, Subcommand};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

#[derive(Parser)]
#[command(
    name = "sage",
    version = "0.4.0",
    about = "Ultra-fast declarative package manager"
)]
pub struct Cli {
    /// Enable verbose diagnostic logs.
    #[arg(short, long, global = true)]
    pub verbose: bool,

    /// Execute dry-run without mutating filesystem state.
    #[arg(long, global = true)]
    pub dry_run: bool,

    /// Target filesystem sysroot prefix.
    #[arg(long, global = true, default_value = "/")]
    pub root: PathBuf,

    #[command(subcommand)]
    pub command: Commands,
}

#[derive(Subcommand)]
pub enum Commands {
    /// Solve and install specified packages into system or versioned sub-channel.
    Install {
        #[arg(required = true)]
        packages: Vec<String>,
        #[arg(long)]
        channel: Option<String>,
        #[arg(long)]
        no_save: bool,
    },
    /// Remove specified packages from system.
    Remove {
        #[arg(required = true)]
        packages: Vec<String>,
        #[arg(long)]
        channel: Option<String>,
    },
    /// Perform transactional safe upgrade for system or channel packages.
    Upgrade {
        packages: Vec<String>,
        #[arg(long)]
        channel: Option<String>,
        #[arg(long)]
        sync: bool,
    },
    /// Synchronize channel LMDB index databases.
    Sync {
        #[arg(long)]
        channel: Option<String>,
    },
    /// Reconcile declarative system state against /etc/sage/system.toml.
    Rebuild {
        #[arg(long)]
        no_prune: bool,
    },
    /// Manage software channels and repository indexes.
    Repo {
        #[command(subcommand)]
        action: RepoAction,
    },
    /// Build binary package archive (*.pkg.tar.zst) from recipe.
    Build {
        recipe_dir: PathBuf,
        /// Enable a named recipe feature; may be repeated.
        #[arg(long = "feature")]
        features: Vec<String>,
        /// Do not enable features marked as default by the recipe.
        #[arg(long)]
        no_default_features: bool,
    },
    /// Rebuild every discovered recipe in dependency-topological layers.
    MassRebuild {
        recipe_root: PathBuf,
        #[arg(long)]
        output: Option<PathBuf>,
        /// Concurrent packages; zero divides available CPUs automatically.
        #[arg(long, default_value_t = 0)]
        jobs: usize,
    },
    /// Execute an explicitly staged self-hosting bootstrap plan.
    Bootstrap {
        plan: PathBuf,
        #[arg(long)]
        output: Option<PathBuf>,
        #[arg(long, default_value_t = 0)]
        jobs: usize,
    },
    /// Inspect configured software channels.
    Channel {
        #[command(subcommand)]
        action: ChannelAction,
    },
    /// Switch the active toolchain profile.
    Toolchain {
        #[command(subcommand)]
        action: ToolchainAction,
    },
    /// Query installed package and ownership state.
    Query {
        #[command(subcommand)]
        action: QueryAction,
    },
}

#[derive(Subcommand)]
pub enum RepoAction {
    /// Index a pool of *.pkg.tar.zst packages into index.mdb and create signature.
    Index {
        dir: PathBuf,
        #[arg(long)]
        sign_key: Option<PathBuf>,
    },
}

#[derive(Subcommand)]
pub enum ChannelAction {
    /// List configured root channels and subchannels.
    List,
}

#[derive(Subcommand)]
pub enum ToolchainAction {
    /// Populate the active profile from a versioned toolchain.
    Use { channel: String },
}

#[derive(Subcommand)]
pub enum QueryAction {
    /// List all installed package instances.
    Installed,
    /// Find every owner of a physical path.
    Owner { path: PathBuf },
    /// Show one installed package instance.
    Info {
        package: String,
        #[arg(long, default_value = "system")]
        channel: String,
    },
}

#[tokio::main]
async fn main() -> Result<()> {
    let cli = Cli::parse();

    if cli.verbose {
        tracing_subscriber::fmt::init();
    }

    let read_only = matches!(
        &cli.command,
        Commands::Channel {
            action: ChannelAction::List
        } | Commands::Query { .. }
    );
    let lock_path = under_root(&cli.root, Path::new("/run/sage/operation.lock"));
    let _lock = if cli.dry_run || read_only {
        sage_core::HostLock::acquire_shared(lock_path)?
    } else {
        sage_core::HostLock::acquire_exclusive(lock_path)?
    };
    if !cli.dry_run && !read_only {
        settle_journals(&cli.root)?;
    }

    match cli.command {
        Commands::Install {
            packages,
            channel,
            no_save: _,
        } => {
            apply_packages(&cli.root, &packages, channel.as_deref(), false, cli.dry_run).await?;
        }
        Commands::Remove { packages, channel } => {
            remove_packages(&cli.root, &packages, channel.as_deref(), cli.dry_run)?;
        }
        Commands::Upgrade {
            packages,
            channel,
            sync,
        } => {
            if sync {
                sync_channels(&cli.root, channel.as_deref(), cli.dry_run).await?;
            }
            upgrade_packages(&cli.root, &packages, channel.as_deref(), cli.dry_run).await?;
        }
        Commands::Sync { channel } => {
            sync_channels(&cli.root, channel.as_deref(), cli.dry_run).await?;
        }
        Commands::Rebuild { no_prune } => {
            rebuild_system(&cli.root, no_prune, cli.dry_run).await?;
        }
        Commands::Repo { action } => match action {
            RepoAction::Index { dir, sign_key } => {
                let key = sign_key.context("repo index requires --sign-key")?;
                if cli.dry_run {
                    println!("Would index packages in {}", dir.display());
                } else {
                    let artifacts = sage_repo::build_index(&dir, &dir, &key)?;
                    println!(
                        "Indexed {} packages into {}",
                        artifacts.packages,
                        artifacts.index.display()
                    );
                }
            }
        },
        Commands::Build {
            recipe_dir,
            features,
            no_default_features,
        } => {
            build_recipe(
                &cli.root,
                &recipe_dir,
                &features,
                !no_default_features,
                cli.dry_run,
                BuildInvocation::default(),
            )
            .await?;
        }
        Commands::MassRebuild {
            recipe_root,
            output,
            jobs,
        } => {
            mass_rebuild(
                &cli.root,
                &recipe_root,
                output.as_deref(),
                jobs,
                cli.dry_run,
            )
            .await?;
        }
        Commands::Bootstrap { plan, output, jobs } => {
            bootstrap_sources(&cli.root, &plan, output.as_deref(), jobs, cli.dry_run).await?;
        }
        Commands::Channel { action } => match action {
            ChannelAction::List => list_channels(&cli.root)?,
        },
        Commands::Toolchain { action } => match action {
            ToolchainAction::Use { channel } => use_toolchain(&cli.root, &channel, cli.dry_run)?,
        },
        Commands::Query { action } => query_state(&cli.root, action)?,
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

fn under_root(root: &Path, path: &Path) -> PathBuf {
    root.join(path.strip_prefix("/").unwrap_or(path))
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

#[derive(Clone)]
struct ReleaseSource {
    release: sage_repo::IndexedRelease,
    location: ReleaseLocation,
    target_root: PathBuf,
}

#[derive(Clone)]
enum ReleaseLocation {
    Remote(String),
    Local(PathBuf),
}

struct AvailablePackages {
    universe: sage_solver::PackageUniverse,
    releases: BTreeMap<(sage_core::PackageKey, sage_core::Version), ReleaseSource>,
    aliases: BTreeMap<String, String>,
}

fn load_available(root: &Path) -> Result<AvailablePackages> {
    load_available_for_arch(root, None)
}

fn load_available_for_arch(root: &Path, architecture: Option<&str>) -> Result<AvailablePackages> {
    load_available_with_pool(root, architecture, None)
}

fn load_available_with_pool(
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
            let index_path = cache.join(&channel_name).join(alias).join("index.mdb");
            if !index_path.exists() {
                continue;
            }
            aliases.insert(alias.into(), canonical.clone());
            aliases.insert(canonical.clone(), canonical.clone());
            target_roots.insert(canonical.clone(), subchannel.target_root.clone());
            let url = sage_repo::subchannel_url(&channel, sub_name, subchannel);
            for release in sage_repo::RepositoryIndex::open(&index_path)?.all_releases()? {
                if architecture.is_some_and(|wanted| {
                    release.manifest.arch != wanted
                        && release.manifest.arch != "any"
                        && release.manifest.arch != "noarch"
                }) {
                    continue;
                }
                let key = sage_core::PackageKey::new(
                    &canonical,
                    &release.manifest.name,
                    &release.manifest.slot,
                );
                let version = sage_core::Version::new(
                    release.manifest.epoch,
                    &release.manifest.version,
                    release.manifest.release,
                );
                universe.insert(sage_solver::PackageRelease {
                    key: key.clone(),
                    version: version.clone(),
                    dependencies: release.dependencies.clone(),
                    provides: release.manifest.provides.clone(),
                });
                releases.insert(
                    (key, version),
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
        let mut packages: Vec<_> = std::fs::read_dir(pool)?
            .collect::<Result<Vec<_>, _>>()?
            .into_iter()
            .map(|entry| entry.path())
            .filter(|path| {
                path.file_name()
                    .and_then(|name| name.to_str())
                    .is_some_and(|name| name.ends_with(".pkg.tar.zst"))
            })
            .collect();
        packages.sort();
        for path in packages {
            let inspection = sage_archive::inspect_package(&path)?;
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
            let key = sage_core::PackageKey::new(
                &canonical,
                &inspection.manifest.name,
                &inspection.manifest.slot,
            );
            let version = sage_core::Version::new(
                inspection.manifest.epoch,
                &inspection.manifest.version,
                inspection.manifest.release,
            );
            let dependencies = inspection
                .manifest
                .dependencies
                .iter()
                .map(|dependency| dependency.parse())
                .collect::<Result<Vec<_>, _>>()?;
            universe.insert(sage_solver::PackageRelease {
                key: key.clone(),
                version: version.clone(),
                dependencies: dependencies.clone(),
                provides: inspection.manifest.provides.clone(),
            });
            releases.insert(
                (key, version),
                ReleaseSource {
                    release: sage_repo::IndexedRelease {
                        manifest: inspection.manifest,
                        dependencies,
                        archive: path.file_name().unwrap().to_string_lossy().into_owned(),
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
        .filter(|(key, version)| current.get(key) != Some(version))
        .collect();
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
    changes: &[(sage_core::PackageKey, sage_core::Version)],
) -> Result<()> {
    let timestamp = unix_timestamp()?;
    let op_id = format!("install-{}-{timestamp}", std::process::id());
    let digest = hex::encode(Sha256::digest(format!("{changes:?}").as_bytes()));
    database.write_journal(&sage_db::JournalRecord {
        op_id: op_id.clone(),
        stage: "publish".into(),
        affected_packages: changes.iter().map(|(key, _)| key.clone()).collect(),
        journal_sha256: digest,
        timestamp,
    })?;
    let package_cache = under_root(root, Path::new("/var/cache/sage/packages"));
    let engine = sage_repo::DownloadEngine::new(&package_cache)?;
    let mut modified = Vec::new();
    for (key, version) in changes {
        let source = available
            .releases
            .get(&(key.clone(), version.clone()))
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

async fn mass_rebuild(
    root: &Path,
    recipe_root: &Path,
    output: Option<&Path>,
    jobs: usize,
    dry_run: bool,
) -> Result<()> {
    let units = source_build_units(root, sage_build::BuildGraph::discover(recipe_root)?)?;
    let layers = sage_build::BuildGraph::layers(units)?;
    let pool = output
        .map(Path::to_path_buf)
        .unwrap_or_else(|| recipe_root.join(".sage-packages"));
    prepare_source_pool(&pool, dry_run)?;
    execute_source_layers(root, &layers, &pool, jobs, dry_run).await
}

async fn bootstrap_sources(
    root: &Path,
    plan_path: &Path,
    output: Option<&Path>,
    jobs: usize,
    dry_run: bool,
) -> Result<()> {
    let plan = sage_build::BootstrapPlan::load(plan_path)?;
    let base = plan_path.parent().unwrap_or(Path::new("."));
    let pool = output
        .map(Path::to_path_buf)
        .unwrap_or_else(|| base.join(".sage-bootstrap"));
    prepare_source_pool(&pool, dry_run)?;
    for stage in plan.stages {
        println!("Bootstrap stage {}", stage.name);
        let mut units = Vec::new();
        for declared in stage.recipes {
            let path = base.join(declared);
            let recipe_path = if path.is_dir() {
                path.join("recipe.toml")
            } else {
                path
            };
            let recipe = sage_build::RecipeSpec::load(&recipe_path)?;
            units.push(sage_build::BuildUnit::from_recipe(recipe_path, &recipe)?);
        }
        let units = source_build_units(root, units)?;
        let layers = sage_build::BuildGraph::layers(units)?;
        execute_source_layers(root, &layers, &pool, jobs, dry_run).await?;
    }
    Ok(())
}

fn source_build_units(
    root: &Path,
    mut units: Vec<sage_build::BuildUnit>,
) -> Result<Vec<sage_build::BuildUnit>> {
    for unit in &mut units {
        let recipe = sage_build::RecipeSpec::load(&unit.recipe)?;
        let mut dependencies = Vec::new();
        for inherited in &recipe.build.inherit {
            let class = sage_build::Rclass::load(find_rclass(
                unit.recipe.parent().unwrap_or(Path::new(".")),
                root,
                inherited,
            )?)?;
            dependencies.extend(class.implicit_build_dependencies);
        }
        unit.include_dependencies(dependencies)?;
    }
    Ok(units)
}

fn prepare_source_pool(pool: &Path, dry_run: bool) -> Result<()> {
    if pool.exists()
        && std::fs::read_dir(pool)?.any(|entry| {
            entry.ok().is_some_and(|entry| {
                entry
                    .file_name()
                    .to_str()
                    .is_some_and(|name| name.ends_with(".pkg.tar.zst"))
            })
        })
    {
        bail!(
            "source build output must not contain existing packages: {}",
            pool.display()
        );
    }
    if !dry_run {
        std::fs::create_dir_all(pool)?;
    }
    Ok(())
}

async fn execute_source_layers(
    root: &Path,
    layers: &[Vec<sage_build::BuildUnit>],
    pool: &Path,
    requested_jobs: usize,
    dry_run: bool,
) -> Result<()> {
    if dry_run {
        for (index, layer) in layers.iter().enumerate() {
            println!(
                "Layer {}: {}",
                index + 1,
                layer
                    .iter()
                    .map(|unit| unit.name.as_str())
                    .collect::<Vec<_>>()
                    .join(", ")
            );
        }
        return Ok(());
    }
    let config =
        sage_build::BuildConfig::load(under_root(root, Path::new("/etc/sage/build.toml")))?;
    for (layer_index, layer) in layers.iter().enumerate() {
        let concurrency = if requested_jobs == 0 {
            config.jobs.min(layer.len()).max(1)
        } else {
            requested_jobs.min(layer.len()).max(1)
        };
        let inner_jobs = (config.jobs / concurrency).max(1);
        println!("Build layer {} ({} packages)", layer_index + 1, layer.len());
        for chunk in layer.chunks(concurrency) {
            let outputs = tempfile::Builder::new()
                .prefix(".sage-layer-")
                .tempdir_in(pool)?;
            let mut tasks = tokio::task::JoinSet::new();
            for (index, unit) in chunk.iter().enumerate() {
                let root = root.to_path_buf();
                let recipe = unit.recipe.clone();
                let output = outputs.path().join(format!("{index:04}"));
                let pool = pool.to_path_buf();
                std::fs::create_dir(&output)?;
                tasks.spawn(async move {
                    build_recipe(
                        &root,
                        &recipe,
                        &[],
                        true,
                        false,
                        BuildInvocation {
                            output_dir: Some(output.clone()),
                            local_pool: Some(pool),
                            jobs_override: Some(inner_jobs),
                        },
                    )
                    .await
                    .map(|_| output)
                });
            }
            let mut completed = Vec::new();
            while let Some(result) = tasks.join_next().await {
                completed.push(result??);
            }
            completed.sort();
            for directory in completed {
                let mut artifacts: Vec<_> = std::fs::read_dir(directory)?
                    .collect::<Result<Vec<_>, _>>()?
                    .into_iter()
                    .map(|entry| entry.path())
                    .filter(|path| {
                        path.file_name()
                            .and_then(|name| name.to_str())
                            .is_some_and(|name| name.ends_with(".pkg.tar.zst"))
                    })
                    .collect();
                artifacts.sort();
                for artifact in artifacts {
                    let target = pool.join(artifact.file_name().unwrap());
                    std::fs::rename(&artifact, &target)?;
                    println!("Published {}", target.display());
                }
            }
        }
    }
    Ok(())
}

#[derive(Default)]
struct BuildInvocation {
    output_dir: Option<PathBuf>,
    local_pool: Option<PathBuf>,
    jobs_override: Option<usize>,
}

async fn build_recipe(
    root: &Path,
    recipe_dir: &Path,
    requested_features: &[String],
    use_default_features: bool,
    dry_run: bool,
    invocation: BuildInvocation,
) -> Result<()> {
    let recipe_path = if recipe_dir.is_dir() {
        recipe_dir.join("recipe.toml")
    } else {
        recipe_dir.to_path_buf()
    };
    let mut recipe = sage_build::RecipeSpec::load(&recipe_path)?;
    let features = recipe.effective_features(requested_features, use_default_features)?;
    recipe
        .package
        .dependencies
        .extend(features.dependencies.clone());
    recipe.package.dependencies.sort();
    recipe.package.dependencies.dedup();
    sage_build::merge_build_arguments(&mut recipe.build.args, &features.args);
    let build_config_path = under_root(root, Path::new("/etc/sage/build.toml"));
    let mut config = sage_build::BuildConfig::load(&build_config_path)
        .with_context(|| format!("failed to load {}", build_config_path.display()))?;
    if let Some(jobs) = invocation.jobs_override {
        config.jobs = jobs.max(1);
    }
    let cross = config.cross_target(&recipe.build.target)?.cloned();
    if let Some(target) = &cross {
        recipe.build.args.extend([
            (
                "configure_triples".into(),
                format!("--build={} --host={}", config.build, recipe.build.target),
            ),
            (
                "cross_args".into(),
                format!(
                    "-DCMAKE_SYSTEM_NAME={} -DCMAKE_SYSTEM_PROCESSOR={}",
                    target.cmake_system_name, target.arch
                ),
            ),
            (
                "cross_arg".into(),
                "--cross-file /build/sage-cross.ini".into(),
            ),
            (
                "target_arg".into(),
                format!("--target {}", recipe.build.target),
            ),
        ]);
    }
    let mut classes = Vec::new();
    for inherited in &recipe.build.inherit {
        classes.push(sage_build::Rclass::load(find_rclass(
            recipe_path.parent().unwrap_or(Path::new(".")),
            root,
            inherited,
        )?)?);
    }
    if !features.env.is_empty() {
        classes.push(sage_build::Rclass {
            schema_version: sage_core::SCHEMA_VERSION,
            name: "selected-features".into(),
            description: "Environment selected by recipe features".into(),
            implicit_build_dependencies: Vec::new(),
            allowed_compilers: Vec::new(),
            allowed_linkers: Vec::new(),
            defaults: BTreeMap::new(),
            env: features.env.clone(),
            phases: BTreeMap::new(),
        });
    }
    let mut selected_config = config.clone();
    if let Some(target) = &cross {
        selected_config.cc.clone_from(&target.cc);
        selected_config.cxx.clone_from(&target.cxx);
        let mut cross_env = BTreeMap::from([
            ("CC".into(), target.cc.clone()),
            ("CXX".into(), target.cxx.clone()),
            ("AR".into(), target.ar.clone()),
            ("STRIP".into(), target.strip.clone()),
            ("GOOS".into(), target.goos.clone()),
            ("GOARCH".into(), target.goarch.clone()),
            (
                "RUSTFLAGS".into(),
                format!("{} {}", config.rustflags, target.rustflags)
                    .trim()
                    .into(),
            ),
        ]);
        if !recipe.build.target_dependencies.is_empty() || !features.target_dependencies.is_empty()
        {
            cross_env.extend([
                (
                    "CFLAGS".into(),
                    format!("{} --sysroot=/sysroot", config.cflags),
                ),
                (
                    "CXXFLAGS".into(),
                    format!("{} --sysroot=/sysroot", config.cxxflags),
                ),
                (
                    "LDFLAGS".into(),
                    format!("{} --sysroot=/sysroot", config.ldflags),
                ),
            ]);
        }
        classes.push(sage_build::Rclass {
            schema_version: sage_core::SCHEMA_VERSION,
            name: "cross-target".into(),
            description: "Configured cross compilation environment".into(),
            implicit_build_dependencies: Vec::new(),
            allowed_compilers: Vec::new(),
            allowed_linkers: Vec::new(),
            defaults: BTreeMap::new(),
            env: cross_env,
            phases: BTreeMap::new(),
        });
    }
    sage_build::validate_toolchain(&classes, &selected_config)?;
    let build_dependencies = sage_build::build_dependencies(&recipe, &classes, &features)?;
    let target_dependencies = sage_build::target_dependencies(&recipe, &features)?;
    if cross.is_none() && !target_dependencies.is_empty() {
        bail!("target_dependencies require [build].target");
    }
    if dry_run {
        println!(
            "Would build {}-{}-{} for {} using {:?}",
            recipe.package.name,
            recipe.package.version,
            recipe.package.release,
            recipe.package.arch,
            recipe.build.inherit
        );
        for dependency in &build_dependencies {
            println!("Build-depends: {}", dependency.name);
        }
        for dependency in &target_dependencies {
            println!("Target-depends: {}", dependency.name);
        }
        return Ok(());
    }
    let workspace = tempfile::Builder::new().prefix("sage-build-").tempdir()?;
    let source = workspace.path().join("source");
    let build = workspace.path().join("build");
    let destdir = workspace.path().join("dest");
    std::fs::create_dir_all(&source)?;
    std::fs::create_dir(&build)?;
    std::fs::create_dir(&destdir)?;
    let toolchain = prepare_build_toolchain(
        root,
        workspace.path(),
        &recipe.package.channel,
        &build_dependencies,
        invocation.local_pool.as_deref(),
    )
    .await?;
    let target_sysroot = if let Some(target) = &cross {
        prepare_package_tree(
            root,
            workspace.path(),
            "sysroot",
            &recipe.package.channel,
            &target_dependencies,
            Some(&target.arch),
            invocation.local_pool.as_deref(),
        )
        .await?
    } else {
        None
    };
    let distfiles = source.join(".distfiles");
    std::fs::create_dir(&distfiles)?;
    let engine = sage_repo::DownloadEngine::new(workspace.path().join("cache"))?;
    for (index, input) in recipe.source_inputs().enumerate() {
        let staged = distfiles.join(sage_build::source_archive_name(index));
        match input.kind {
            sage_build::SourceKind::Archive => {
                engine
                    .download_url(&input.url, &staged, &input.sha256)
                    .await?;
            }
            sage_build::SourceKind::File => {
                engine
                    .download_url(&input.url, &staged, &input.sha256)
                    .await?;
            }
            sage_build::SourceKind::Git => {
                let git = config.git.clone();
                let input = input.clone();
                let checkout = workspace.path().join(format!("git-checkout-{index:03}"));
                tokio::task::spawn_blocking(move || {
                    sage_build::fetch_git_source(&git, &input, &checkout, &staged)
                })
                .await??;
            }
        }
    }
    std::fs::write(distfiles.join("manifest"), recipe.source_manifest())?;
    stage_patches(recipe_path.parent().unwrap_or(Path::new(".")), &source)?;
    let variables = build_variables(&config, &recipe);
    let runner_contents = sage_build::compose_runner(&classes, &variables)?;
    let runner = workspace.path().join("sage-build-runner.sh");
    std::fs::write(&runner, runner_contents)?;
    let paths = sage_build::SandboxPaths {
        source,
        build,
        destdir: destdir.clone(),
        runner,
        toolchain,
        target_sysroot,
    };
    sage_build::SandboxRunner::new(&config).run(&paths, recipe.build.allow_network)?;
    sage_build::stage_sysusers(&destdir, &recipe)?;
    sage_build::validate_kernel_module_slot(&destdir, &recipe.package.slot)?;
    if recipe.uses_private_channel() {
        let report = sage_build::ElfScanner::rewrite_private_runpaths(
            &destdir,
            &recipe.build.private_library_dirs,
            &config.patchelf,
        )?;
        tracing::info!(
            files = report.rewritten.len(),
            directories = report.library_dirs.len(),
            "rewrote private-channel ELF RUNPATHs"
        );
    }
    let areas = sage_build::PayloadCarver::carve_packages(&destdir, &recipe)?;
    let output_dir = invocation
        .output_dir
        .as_deref()
        .unwrap_or_else(|| recipe_path.parent().unwrap_or(Path::new(".")));
    for area in areas {
        package_staging(
            &recipe,
            &area,
            recipe_path.parent().unwrap_or(Path::new(".")),
            output_dir,
            config.source_date_epoch,
            &features.enabled,
        )?;
    }
    Ok(())
}

/// Resolves, downloads, and extracts build-only packages into an ephemeral
/// prefix. The resulting tree is mounted read-only and is never registered in
/// the host database, so failed builds leave no installed state behind.
async fn prepare_build_toolchain(
    root: &Path,
    workspace: &Path,
    channel: &str,
    dependencies: &[sage_core::Dependency],
    local_pool: Option<&Path>,
) -> Result<Option<PathBuf>> {
    prepare_package_tree(
        root,
        workspace,
        "toolchain",
        channel,
        dependencies,
        None,
        local_pool,
    )
    .await
}

async fn prepare_package_tree(
    root: &Path,
    workspace: &Path,
    tree_name: &str,
    channel: &str,
    dependencies: &[sage_core::Dependency],
    architecture: Option<&str>,
    local_pool: Option<&Path>,
) -> Result<Option<PathBuf>> {
    if dependencies.is_empty() {
        return Ok(None);
    }
    let available = load_available_with_pool(root, architecture, local_pool)?;
    let channel = canonical_channel(&available, Some(channel))?;
    let local_locks = available
        .releases
        .iter()
        .filter(|(_, source)| matches!(source.location, ReleaseLocation::Local(_)))
        .map(|((key, version), _)| (key.clone(), version.clone()));
    let solution = sage_solver::SageSolver::with_locked(&available.universe, local_locks)
        .resolve_dependencies(&channel, dependencies)?;
    let tree = workspace.join(tree_name);
    std::fs::create_dir(&tree)?;
    let package_cache = under_root(root, Path::new("/var/cache/sage/packages"));
    let engine = sage_repo::DownloadEngine::new(&package_cache)?;
    let mut owned = std::collections::BTreeSet::new();
    for (key, version) in solution {
        let source = available
            .releases
            .get(&(key.clone(), version.clone()))
            .with_context(|| format!("index record disappeared for build dependency {key}"))?;
        let archive = obtain_release_archive(&engine, &package_cache, source).await?;
        let inspection = sage_archive::inspect_package(&archive)?;
        if let Some(path) = inspection
            .files
            .iter()
            .map(|record| &record.path)
            .find(|path| !owned.insert((*path).clone()))
        {
            bail!("build dependency file conflict at {}", path.display());
        }
        sage_archive::extract_package(&archive, &tree, &inspection.files)?;
        println!("{} dependency {} {}", tree_name, key, version);
    }
    Ok(Some(tree))
}

async fn obtain_release_archive(
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

fn stage_patches(recipe_dir: &Path, source: &Path) -> Result<()> {
    let patches = recipe_dir.join("patches");
    if !patches.exists() {
        return Ok(());
    }
    let target = source.join(".patches");
    std::fs::create_dir(&target)?;
    let mut entries: Vec<_> = std::fs::read_dir(&patches)?.collect::<Result<_, _>>()?;
    entries.sort_by_key(|entry| entry.file_name());
    for entry in entries {
        if !entry.file_type()?.is_file() {
            bail!(
                "patch entry must be a regular file: {}",
                entry.path().display()
            );
        }
        std::fs::copy(entry.path(), target.join(entry.file_name()))?;
    }
    Ok(())
}

fn find_rclass(recipe_dir: &Path, root: &Path, name: &str) -> Result<PathBuf> {
    let filename = format!("{name}.toml");
    for ancestor in recipe_dir.ancestors() {
        let candidate = ancestor.join("rclass").join(&filename);
        if candidate.exists() {
            return Ok(candidate);
        }
    }
    let installed = under_root(root, &Path::new("/usr/share/sage/rclass").join(filename));
    if installed.exists() {
        Ok(installed)
    } else {
        bail!("rclass '{name}' was not found")
    }
}

fn build_variables(
    config: &sage_build::BuildConfig,
    recipe: &sage_build::RecipeSpec,
) -> std::collections::BTreeMap<String, String> {
    let mut variables = std::collections::BTreeMap::from([
        ("JOBS".into(), config.jobs.to_string()),
        ("CFLAGS".into(), config.cflags.clone()),
        ("CXXFLAGS".into(), config.cxxflags.clone()),
        ("CPPFLAGS".into(), config.cppflags.clone()),
        ("LDFLAGS".into(), config.ldflags.clone()),
        ("RUSTFLAGS".into(), config.rustflags.clone()),
        ("SRC_DIR".into(), "/source".into()),
        ("BUILD_DIR".into(), "/build".into()),
        ("DESTDIR".into(), "/dest".into()),
        ("PACKAGE_SLOT".into(), recipe.package.slot.clone()),
        ("BUILD_TRIPLE".into(), config.build.clone()),
        ("TARGET_TRIPLE".into(), recipe.build.target.clone()),
        (
            "TARGET_ARCH".into(),
            config
                .targets
                .get(&recipe.build.target)
                .map(|target| target.arch.clone())
                .unwrap_or_default(),
        ),
        (
            "TARGET_ENDIAN".into(),
            config
                .targets
                .get(&recipe.build.target)
                .map(|target| target.endian.clone())
                .unwrap_or_default(),
        ),
        (
            "GOOS".into(),
            config
                .targets
                .get(&recipe.build.target)
                .map(|target| target.goos.clone())
                .unwrap_or_default(),
        ),
    ]);
    variables.extend(
        recipe
            .build
            .args
            .iter()
            .map(|(key, value)| (format!("args.{key}"), value.clone())),
    );
    variables
}

fn package_staging(
    recipe: &sage_build::RecipeSpec,
    area: &sage_build::PackageStagingArea,
    recipe_dir: &Path,
    output_dir: &Path,
    build_time: u64,
    features: &std::collections::BTreeSet<String>,
) -> Result<()> {
    let data = area.path().join("data");
    let records = sage_archive::build_file_index(&data)?;
    let elf = sage_build::ElfScanner::scan(&data)?;
    let metadata = area.path().join(".METADATA");
    std::fs::create_dir(&metadata)?;
    if area.name == recipe.package.name {
        stage_declarative_metadata(recipe_dir, &metadata)?;
    }
    std::fs::write(
        metadata.join("files.idx"),
        sage_archive::format_file_index(&records),
    )?;
    let subpackage = recipe
        .subpackages
        .iter()
        .find(|subpackage| subpackage.name == area.name);
    let mut dependencies = area.dependencies.clone();
    dependencies.extend(elf.dependencies);
    dependencies.sort();
    dependencies.dedup();
    let mut provides = area.provides.clone();
    provides.extend(elf.provides);
    provides.sort();
    provides.dedup();
    let manifest = sage_archive::PackageManifest {
        schema_version: sage_core::SCHEMA_VERSION,
        name: area.name.clone(),
        slot: recipe.package.slot.clone(),
        version: recipe.package.version.clone(),
        release: recipe.package.release,
        epoch: recipe.package.epoch,
        arch: recipe.package.arch.clone(),
        channel: recipe.package.channel.clone(),
        description: subpackage
            .and_then(|package| package.description.clone())
            .unwrap_or_else(|| recipe.package.description.clone()),
        license: subpackage
            .and_then(|package| package.license.clone())
            .unwrap_or_else(|| recipe.package.license.clone()),
        installed_size: records.iter().map(|record| record.size).sum(),
        build_time,
        dependencies,
        provides,
        conflicts: Vec::new(),
        features: features.iter().cloned().collect(),
    };
    std::fs::write(
        metadata.join("manifest.toml"),
        toml::to_string_pretty(&manifest)?,
    )?;
    let output = output_dir.join(format!(
        "{}-{}-{}-{}.pkg.tar.zst",
        area.name, recipe.package.version, recipe.package.release, recipe.package.arch
    ));
    sage_archive::create_package(area.path(), &output, 15)?;
    println!("Created {}", output.display());
    Ok(())
}

fn stage_declarative_metadata(recipe_dir: &Path, metadata: &Path) -> Result<()> {
    let service = recipe_dir.join("service.toml");
    if service.exists() {
        sage_sys::ServiceSpec::load(&service)?;
        std::fs::copy(service, metadata.join("service.toml"))?;
    }
    let triggers = recipe_dir.join("triggers.toml");
    if triggers.exists() {
        sage_sys::TriggerSpec::load(&triggers)?;
        std::fs::copy(triggers, metadata.join("triggers.toml"))?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn declarative_metadata_is_validated_and_staged() {
        let directory = tempfile::tempdir().unwrap();
        let metadata = directory.path().join("metadata");
        std::fs::create_dir(&metadata).unwrap();
        std::fs::write(
            directory.path().join("service.toml"),
            r#"schema_version=1
[service]
name="daemon"
description="Daemon"
command=["/usr/bin/daemon","--foreground"]
user="daemon"
group="daemon"
working_dir="/"
restart="on-failure"
type="simple"
"#,
        )
        .unwrap();
        std::fs::write(
            directory.path().join("triggers.toml"),
            r#"schema_version=1
name="daemon-cache"
description="Refresh daemon cache"
on_paths=["usr/share/daemon/**"]
exec=["/usr/bin/daemon","--refresh-cache"]
priority=50
ignore_missing_binary=false
"#,
        )
        .unwrap();

        stage_declarative_metadata(directory.path(), &metadata).unwrap();
        sage_sys::ServiceSpec::load(metadata.join("service.toml")).unwrap();
        sage_sys::TriggerSpec::load(metadata.join("triggers.toml")).unwrap();
    }

    #[tokio::test]
    async fn mass_rebuild_dry_run_discovers_dependency_tree_without_output() {
        let root = tempfile::tempdir().unwrap();
        let recipes = tempfile::tempdir().unwrap();
        for (name, dependencies) in [("lib", ""), ("app", "dependencies=[\"lib\"]")] {
            let directory = recipes.path().join(name);
            std::fs::create_dir(&directory).unwrap();
            std::fs::write(
                directory.join("recipe.toml"),
                format!(
                    r#"schema_version=1
[package]
name="{name}"
version="1"
release=1
description="{name}"
license="MIT"
channel="system"
arch="any"
{dependencies}
[source]
url="https://example.invalid/{name}.tar"
sha256="{}"
"#,
                    "00".repeat(32)
                ),
            )
            .unwrap();
        }
        mass_rebuild(root.path(), recipes.path(), None, 0, true)
            .await
            .unwrap();
        assert!(!recipes.path().join(".sage-packages").exists());
        let plan = recipes.path().join("bootstrap.toml");
        std::fs::write(
            &plan,
            r#"schema_version=1
[[stages]]
name="seed"
recipes=["lib/recipe.toml"]
[[stages]]
name="world"
recipes=["app/recipe.toml"]
"#,
        )
        .unwrap();
        bootstrap_sources(root.path(), &plan, None, 0, true)
            .await
            .unwrap();
        assert!(!recipes.path().join(".sage-bootstrap").exists());
    }

    #[test]
    fn source_pool_overlays_local_artifacts_into_solver_universe() {
        let root = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(root.path().join("etc/sage")).unwrap();
        std::fs::write(
            root.path().join("etc/sage/channels.toml"),
            "schema_version=1\n[channels]\n",
        )
        .unwrap();
        let pool = root.path().join("pool");
        let stage = root.path().join("stage");
        std::fs::create_dir_all(stage.join(".METADATA")).unwrap();
        std::fs::create_dir_all(stage.join("data/usr/lib")).unwrap();
        std::fs::write(stage.join("data/usr/lib/libseed.so"), b"seed").unwrap();
        let records = sage_archive::build_file_index(&stage.join("data")).unwrap();
        std::fs::write(
            stage.join(".METADATA/files.idx"),
            sage_archive::format_file_index(&records),
        )
        .unwrap();
        let manifest = sage_archive::PackageManifest {
            schema_version: 1,
            name: "seed".into(),
            slot: "0".into(),
            version: "1".into(),
            release: 1,
            epoch: 0,
            arch: "any".into(),
            channel: "system".into(),
            description: "seed".into(),
            license: "MIT".into(),
            installed_size: 4,
            build_time: 1,
            dependencies: Vec::new(),
            provides: Vec::new(),
            conflicts: Vec::new(),
            features: Vec::new(),
        };
        std::fs::write(
            stage.join(".METADATA/manifest.toml"),
            toml::to_string(&manifest).unwrap(),
        )
        .unwrap();
        std::fs::create_dir(&pool).unwrap();
        let package = pool.join("seed-1-1-any.pkg.tar.zst");
        sage_archive::create_package(&stage, &package, 1).unwrap();

        let available = load_available_with_pool(root.path(), None, Some(&pool)).unwrap();
        let key = sage_core::PackageKey::new("main/system", "seed", "0");
        let version = sage_core::Version::new(0, "1", 1);
        assert!(matches!(
            available.releases[&(key, version)].location,
            ReleaseLocation::Local(_)
        ));
    }
}
