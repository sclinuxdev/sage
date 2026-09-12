//! Main entry point for the Sage package manager CLI.
//!
//! Provides fast, pure CLI argument parsing based on `clap` derive,
//! delegating system reconciliation to `sage-sys` and hermetic
//! builds to `sage-build`.

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};
use std::io::IsTerminal;
use std::path::{Path, PathBuf};

pub use sage_build::{
    BuildInvocation, bootstrap_sources, build_recipe, mass_rebuild, stage_declarative_metadata,
    stage_sysusers,
};
pub use sage_repo::{ReleaseLocation, ReleaseSource};
pub use sage_sys::{
    AvailablePackages, apply_packages, canonical_channel, load_available_with_pool, rebuild_system,
    remove_packages, sync_channels, upgrade_packages,
};

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
        /// Explicit virtual interface provider selection (e.g. --provider init=systemd).
        #[arg(short = 'P', long = "provider", value_parser = parse_provider_override)]
        providers: Vec<(String, String)>,
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
    /// Manage system services and inspect drift.
    Service {
        #[command(subcommand)]
        action: ServiceAction,
    },
    /// Clean package caches, temporary files, and obsolete artifacts.
    Clean {
        /// Remove all cached package archives rather than only temporary/stale files.
        #[arg(long)]
        all: bool,
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

#[derive(Subcommand, Debug, Clone, PartialEq, Eq)]
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
    /// Find installed orphan packages.
    Orphans,
}

#[derive(Subcommand, Debug, Clone, PartialEq, Eq)]
pub enum ServiceAction {
    /// Enable a service in Sage and activate it in the init system.
    Enable { service: String },
    /// Disable a service in Sage and deactivate it in the init system.
    Disable { service: String },
    /// Adopt an externally-enabled service into Sage declarative management.
    Adopt { service: String },
    /// List known services and their management status.
    List,
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
            | Commands::Service {
                action: ServiceAction::List
            }
    );
    cli.root = std::fs::canonicalize(&cli.root)
        .with_context(|| format!("cannot resolve target root {}", cli.root.display()))?;
    let lock_path = sage_core::under_root(&cli.root, Path::new("/run/sage/operation.lock"));
    let _lock = if cli.dry_run || read_only {
        sage_core::HostLock::acquire_shared(lock_path)?
    } else {
        sage_core::HostLock::acquire_exclusive(lock_path)?
    };
    if !cli.dry_run && !read_only {
        sage_sys::settle_journals(&cli.root).await?;
    }
    match cli.command {
        Commands::Install {
            packages,
            channel,
            no_save,
            providers,
        } => {
            let interactive = std::io::stdin().is_terminal() && std::io::stdout().is_terminal();
            sage_sys::apply_packages(
                &cli.root,
                &packages,
                channel.as_deref(),
                &providers,
                interactive,
                false,
                !no_save,
                cli.dry_run,
            )
            .await?;
        }
        Commands::Remove { packages, channel } => {
            sage_sys::remove_packages(&cli.root, &packages, channel.as_deref(), true, cli.dry_run)?;
        }
        Commands::Upgrade {
            packages,
            channel,
            sync,
        } => {
            if sync {
                sage_sys::sync_channels(&cli.root, channel.as_deref(), cli.dry_run).await?;
            }
            sage_sys::upgrade_packages(&cli.root, &packages, channel.as_deref(), cli.dry_run)
                .await?;
        }
        Commands::Sync { channel } => {
            sage_sys::sync_channels(&cli.root, channel.as_deref(), cli.dry_run).await?;
        }
        Commands::Rebuild { no_prune } => {
            sage_sys::rebuild_system(&cli.root, no_prune, cli.dry_run).await?;
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
            sage_build::build_recipe(
                &cli.root,
                &recipe_dir,
                &features,
                !no_default_features,
                cli.dry_run,
                sage_build::BuildInvocation::default(),
            )
            .await?;
        }
        Commands::MassRebuild {
            recipe_root,
            output,
            jobs,
        } => {
            sage_build::mass_rebuild(
                &cli.root,
                &recipe_root,
                output.as_deref(),
                jobs,
                cli.dry_run,
            )
            .await?;
        }
        Commands::Bootstrap { plan, output, jobs } => {
            sage_build::bootstrap_sources(&cli.root, &plan, output.as_deref(), jobs, cli.dry_run)
                .await?;
        }
        Commands::Channel { action } => match action {
            ChannelAction::List => sage_sys::list_channels(&cli.root)?,
        },
        Commands::Toolchain { action } => match action {
            ToolchainAction::Use { channel } => {
                sage_sys::use_toolchain(&cli.root, &channel, cli.dry_run)?
            }
        },
        Commands::Query { action } => match action {
            QueryAction::Installed => sage_sys::query_installed(&cli.root)?,
            QueryAction::Owner { path } => sage_sys::query_owner(&cli.root, &path)?,
            QueryAction::Info { package, channel } => {
                sage_sys::query_info(&cli.root, &package, &channel)?
            }
            QueryAction::Orphans => sage_sys::query_orphans(&cli.root)?,
        },
        Commands::Clean { all } => {
            if cli.dry_run {
                println!("Would clean Sage cache and temporary files (all: {all})");
            } else {
                let report = sage_sys::clean_cache(&cli.root, all)?;
                println!(
                    "Cleaned {} files, freed {}",
                    report.files_removed,
                    sage_sys::format_bytes(report.bytes_freed as i64)
                );
            }
        }
        Commands::Service { action } => match action {
            ServiceAction::Enable { service } => {
                sage_sys::service_enable(&cli.root, &service, cli.dry_run)?;
            }
            ServiceAction::Disable { service } => {
                sage_sys::service_disable(&cli.root, &service, cli.dry_run)?;
            }
            ServiceAction::Adopt { service } => {
                sage_sys::service_adopt(&cli.root, &service, cli.dry_run)?;
            }
            ServiceAction::List => {
                let services = sage_sys::list_services(&cli.root)?;
                println!(
                    "{:<20} {:<20} {:<15} {:<15}",
                    "SERVICE", "STATUS", "PROVIDER", "PACKAGE"
                );
                for s in services {
                    println!(
                        "{:<20} {:<20} {:<15} {:<15}",
                        s.name, s.state, s.provider, s.package
                    );
                }
            }
        },
    }
    Ok(())
}

fn parse_provider_override(input: &str) -> std::result::Result<(String, String), String> {
    let (interface, selector) = input.split_once('=').ok_or_else(|| {
        "provider override must follow format interface=package (e.g. init=systemd)".to_string()
    })?;
    let interface = interface.trim();
    let selector = selector.trim();
    use sage_core::valid_package_component;
    let key = sage_core::PackageKey::in_channel("main/system", selector)
        .map_err(|error| error.to_string())?;
    if !sage_core::valid_provider_symbol(interface)
        || !valid_package_component(&key.name)
        || !valid_package_component(&key.slot)
    {
        return Err("provider override requires a valid interface and package[:slot]".into());
    }
    Ok((
        interface
            .strip_prefix("virtual/")
            .unwrap_or(interface)
            .into(),
        selector.into(),
    ))
}

#[cfg(test)]
mod provider_cli_tests {
    use super::*;

    #[test]
    fn provider_flags_accept_slots_and_reject_malformed_mappings() {
        for flag in ["-P", "--provider"] {
            let cli = Cli::try_parse_from([
                "sage",
                "install",
                "virtual/libc++",
                flag,
                "libc++=libc++:abi+debug",
            ])
            .unwrap();
            let Commands::Install { providers, .. } = cli.command else {
                panic!("wrong command")
            };
            assert_eq!(
                providers,
                vec![("libc++".into(), "libc++:abi+debug".into())]
            );
            let cli =
                Cli::try_parse_from(["sage", "install", "virtual/awk", flag, "virtual/awk=gawk:2"])
                    .unwrap();
            let Commands::Install { providers, .. } = cli.command else {
                panic!("wrong command")
            };
            assert_eq!(providers, vec![("awk".into(), "gawk:2".into())]);
            for symbol in ["so:libfoo.so.1", "so:libC++.so.1@ABI"] {
                let mapping = format!("{symbol}=so:abi+debug");
                let cli = Cli::try_parse_from(["sage", "install", "app", flag, &mapping]).unwrap();
                let Commands::Install { providers, .. } = cli.command else {
                    panic!("wrong command")
                };
                assert_eq!(providers, vec![(symbol.into(), "so:abi+debug".into())]);
            }
            for input in [
                "awk",
                "=gawk",
                "awk=",
                "awk=gawk:",
                "awk=gawk=bad",
                "virtual/=gawk",
                "awk=a/b",
                "so:=foo",
                "so:lib foo.so=foo",
                "so:lib/foo.so=foo",
                "so:lib\nfoo.so=foo",
                "virtual/so:libfoo.so=foo",
            ] {
                assert!(
                    Cli::try_parse_from(["sage", "install", "app", flag, input]).is_err(),
                    "{input}"
                );
            }
        }
    }
}
