//! Main entry point for the Sage package manager CLI.
//!
//! Provides fast, pure CLI argument parsing based on `clap` derive,
//! supporting package installation, atomic upgrades, source builds,
//! repository index generation, and declarative system reconciliation.

use anyhow::Result;
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

fn main() -> Result<()> {
    let cli = Cli::parse();

    if cli.verbose {
        tracing_subscriber::fmt::init();
    }

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
            println!("Sync request (channel: {:?})", channel);
        }
        Commands::Rebuild { no_prune } => {
            println!("Rebuild system state (no_prune: {})", no_prune);
        }
        Commands::Repo { action } => match action {
            RepoAction::Index { dir, sign_key } => {
                println!(
                    "Index repository packages in {:?} with key {:?}",
                    dir, sign_key
                );
            }
        },
        Commands::Build { recipe_dir } => {
            println!("Build recipe from {:?}", recipe_dir);
        }
    }

    Ok(())
}
