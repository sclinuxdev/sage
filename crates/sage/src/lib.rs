//! Application services for the Sage package manager CLI.
//!
//! Provides fast, pure CLI argument parsing based on `clap` derive,
//! supporting package installation, atomic upgrades, source builds,
//! repository index generation, and declarative system reconciliation.

use clap::{Parser, Subcommand};
use std::path::PathBuf;

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

mod build_ops;
mod commands;
mod package_model;
mod package_ops;
mod paths;

pub use build_ops::stage_declarative_metadata;
pub use build_ops::{bootstrap_sources, mass_rebuild};
pub use package_model::{
    load_available_with_pool, AvailablePackages, ReleaseLocation, ReleaseSource,
};

/// Executes one parsed Sage command.
pub async fn execute(cli: Cli) -> anyhow::Result<()> {
    commands::execute(cli).await
}
