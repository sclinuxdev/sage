//! Main entry point for the Sage package manager CLI.
//!
//! Provides fast, pure CLI argument parsing based on `clap` derive,
//! supporting package installation, atomic upgrades, source builds,
//! repository index generation, and declarative system reconciliation.

use anyhow::{bail, Context, Result};
use clap::{Parser, Subcommand};
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
    Build { recipe_dir: PathBuf },
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

#[tokio::main]
async fn main() -> Result<()> {
    let cli = Cli::parse();

    if cli.verbose {
        tracing_subscriber::fmt::init();
    }

    let lock_path = under_root(&cli.root, Path::new("/run/sage/operation.lock"));
    let _lock = if cli.dry_run {
        sage_core::HostLock::acquire_shared(lock_path)?
    } else {
        sage_core::HostLock::acquire_exclusive(lock_path)?
    };

    match cli.command {
        Commands::Install {
            packages,
            channel,
            no_save,
        } => {
            println!(
                "Install request for {:?} (channel: {:?}, no_save: {})",
                packages, channel, no_save
            );
        }
        Commands::Remove { packages, channel } => {
            println!("Remove request for {:?} (channel: {:?})", packages, channel);
        }
        Commands::Upgrade {
            packages,
            channel,
            sync,
        } => {
            println!(
                "Upgrade request (packages: {:?}, channel: {:?}, sync: {})",
                packages, channel, sync
            );
        }
        Commands::Sync { channel } => {
            sync_channels(&cli.root, channel.as_deref(), cli.dry_run).await?;
        }
        Commands::Rebuild { no_prune } => {
            println!("Rebuild system state (no_prune: {})", no_prune);
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
        Commands::Build { recipe_dir } => {
            build_recipe(&cli.root, &recipe_dir, cli.dry_run).await?;
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

fn under_root(root: &Path, path: &Path) -> PathBuf {
    root.join(path.strip_prefix("/").unwrap_or(path))
}

async fn build_recipe(root: &Path, recipe_dir: &Path, dry_run: bool) -> Result<()> {
    let recipe_path = if recipe_dir.is_dir() {
        recipe_dir.join("recipe.toml")
    } else {
        recipe_dir.to_path_buf()
    };
    let recipe = sage_build::RecipeSpec::load(&recipe_path)?;
    let build_config_path = under_root(root, Path::new("/etc/sage/build.toml"));
    let config = sage_build::BuildConfig::load(&build_config_path)
        .with_context(|| format!("failed to load {}", build_config_path.display()))?;
    let mut classes = Vec::new();
    for inherited in &recipe.build.inherit {
        classes.push(sage_build::Rclass::load(find_rclass(
            recipe_path.parent().unwrap_or(Path::new(".")),
            root,
            inherited,
        )?)?);
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
        return Ok(());
    }
    let workspace = tempfile::Builder::new().prefix("sage-build-").tempdir()?;
    let source = workspace.path().join("source");
    let build = workspace.path().join("build");
    let destdir = workspace.path().join("dest");
    std::fs::create_dir_all(&source)?;
    std::fs::create_dir(&build)?;
    std::fs::create_dir(&destdir)?;
    let source_name = recipe
        .source
        .url
        .rsplit('/')
        .next()
        .filter(|name| !name.is_empty())
        .filter(|name| {
            Path::new(name)
                .components()
                .all(|part| matches!(part, std::path::Component::Normal(_)))
        })
        .unwrap_or("source.archive");
    let engine = sage_repo::DownloadEngine::new(workspace.path().join("cache"))?;
    engine
        .download_url(
            &recipe.source.url,
            &source.join(source_name),
            &recipe.source.sha256,
        )
        .await?;
    let variables = build_variables(&config, &recipe);
    let runner_contents = sage_build::compose_runner(&classes, &variables)?;
    let runner = workspace.path().join("sage-build-runner.sh");
    std::fs::write(&runner, runner_contents)?;
    let paths = sage_build::SandboxPaths {
        source,
        build,
        destdir: destdir.clone(),
        runner,
        toolchain: None,
    };
    sage_build::SandboxRunner::new(&config).run(&paths, recipe.build.allow_network)?;
    let areas = sage_build::PayloadCarver::carve_packages(&destdir, &recipe)?;
    let output_dir = recipe_path.parent().unwrap_or(Path::new("."));
    for area in areas {
        package_staging(&recipe, &area, output_dir, config.source_date_epoch)?;
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
    output_dir: &Path,
    build_time: u64,
) -> Result<()> {
    let data = area.path().join("data");
    let records = sage_archive::build_file_index(&data)?;
    let elf = sage_build::ElfScanner::scan(&data)?;
    let metadata = area.path().join(".METADATA");
    std::fs::create_dir(&metadata)?;
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
        slot: sage_core::DEFAULT_SLOT.into(),
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
