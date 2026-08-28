//! Command orchestration behind the parsed CLI.

use anyhow::{Context, Result};
use std::path::Path;

use crate::build_ops::BuildManager;
use crate::package_ops::PackageManager;
use crate::paths::under_root;
use crate::{Cli, Commands, RepoAction};

/// Executes one parsed command after establishing the operation boundary.
pub(crate) async fn execute(cli: Cli) -> Result<()> {
    if cli.verbose {
        tracing_subscriber::fmt::init();
    }

    let read_only = matches!(
        &cli.command,
        Commands::Channel {
            action: crate::ChannelAction::List
        } | Commands::Query { .. }
    );
    let lock_path = under_root(&cli.root, Path::new("/run/sage/operation.lock"));
    let _lock = if cli.dry_run || read_only {
        sage_core::HostLock::acquire_shared(lock_path)?
    } else {
        sage_core::HostLock::acquire_exclusive(lock_path)?
    };

    let packages = PackageManager::new(&cli.root, cli.dry_run);
    let builds = BuildManager::new(&cli.root, cli.dry_run);
    if !cli.dry_run && !read_only {
        packages.settle_journals()?;
    }
    dispatch(&packages, &builds, cli.command).await
}

async fn dispatch(
    packages: &PackageManager<'_>,
    builds: &BuildManager<'_>,
    command: Commands,
) -> Result<()> {
    match command {
        Commands::Install {
            packages: names,
            channel,
            no_save,
        } => packages.install(&names, channel.as_deref(), no_save).await,
        Commands::Remove {
            packages: names,
            channel,
        } => packages.remove(&names, channel.as_deref()),
        Commands::Upgrade {
            packages: names,
            channel,
            sync,
        } => {
            if sync {
                packages.sync_channels(channel.as_deref()).await?;
            }
            packages.upgrade(&names, channel.as_deref()).await
        }
        Commands::Sync { channel } => packages.sync_channels(channel.as_deref()).await,
        Commands::Rebuild { no_prune } => packages.rebuild(no_prune).await,
        Commands::Repo { action } => dispatch_repo(action, cli_dry_run(packages)),
        Commands::Build {
            recipe_dir,
            features,
            no_default_features,
        } => {
            builds
                .build(&recipe_dir, &features, !no_default_features)
                .await
        }
        Commands::MassRebuild {
            recipe_root,
            output,
            jobs,
        } => {
            builds
                .mass_rebuild(&recipe_root, output.as_deref(), jobs)
                .await
        }
        Commands::Bootstrap { plan, output, jobs } => {
            builds.bootstrap(&plan, output.as_deref(), jobs).await
        }
        Commands::Channel { action } => match action {
            crate::ChannelAction::List => packages.list_channels(),
        },
        Commands::Toolchain { action } => match action {
            crate::ToolchainAction::Use { channel } => packages.use_toolchain(&channel),
        },
        Commands::Query { action } => packages.query(action),
    }
}

fn cli_dry_run(packages: &PackageManager<'_>) -> bool {
    packages.is_dry_run()
}

fn dispatch_repo(action: RepoAction, dry_run: bool) -> Result<()> {
    match action {
        RepoAction::Index { dir, sign_key } => {
            let key = sign_key.context("repo index requires --sign-key")?;
            if dry_run {
                println!("Would index packages in {}", dir.display());
            } else {
                let artifacts = sage_repo::build_index(&dir, &dir, &key)?;
                println!(
                    "Indexed {} packages into {}",
                    artifacts.packages,
                    artifacts.index.display()
                );
            }
            Ok(())
        }
    }
}
