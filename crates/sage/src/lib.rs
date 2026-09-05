//! Main entry point for the Sage package manager CLI.
//!
//! Provides fast, pure CLI argument parsing based on `clap` derive,
//! supporting package installation, atomic upgrades, source builds,
//! repository index generation, and declarative system reconciliation.
use anyhow::{bail, Context, Result};
use clap::{Parser, Subcommand};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
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
pub async fn run() -> Result<()> {
    execute(Cli::parse()).await
}
/// Executes one parsed command through the binary's production interface.
pub async fn execute(mut cli: Cli) -> Result<()> {
    if cli.verbose {
        tracing_subscriber::fmt::init();
    }
    let read_only = matches!(
        &cli.command,
        Commands::Channel {
            action: ChannelAction::List
        } | Commands::Query { .. }
    );
    cli.root = std::fs::canonicalize(&cli.root)
        .with_context(|| format!("cannot resolve target root {}", cli.root.display()))?;
    let lock_path = under_root(&cli.root, Path::new("/run/sage/operation.lock"));
    let _lock = if cli.dry_run || read_only {
        sage_core::HostLock::acquire_shared(lock_path)?
    } else {
        sage_core::HostLock::acquire_exclusive(lock_path)?
    };
    if !cli.dry_run && !read_only {
        settle_journals(&cli.root).await?;
    }
    match cli.command {
        Commands::Install {
            packages,
            channel,
            no_save,
        } => {
            apply_packages(
                &cli.root,
                &packages,
                channel.as_deref(),
                false,
                !no_save,
                cli.dry_run,
            )
            .await?;
        }
        Commands::Remove { packages, channel } => {
            remove_packages(&cli.root, &packages, channel.as_deref(), true, cli.dry_run)?;
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
include!("package_ops.rs");
include!("build_ops.rs");
