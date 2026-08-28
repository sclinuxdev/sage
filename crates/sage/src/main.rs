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
            println!("Build recipe from {:?}", recipe_dir);
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
