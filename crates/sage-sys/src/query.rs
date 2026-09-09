//! Package database inspection, channel enumeration, file ownership queries, and profile toolchain activation.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};
use sage_core::under_root;

use crate::channel::qualify_channel;
use crate::state::{ProfileEngine, SystemConfig};

/// Lists all configured repository channels, their enabled status, and target roots.
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

/// Activates bin toolchain symlinks from an installed opt channel into system profile.
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
    let system = SystemConfig::load(under_root(root, Path::new("/etc/sage/system.toml")))?;
    if dry_run {
        println!(
            "Would activate {} tools from {} in profile {}",
            links.len(),
            channel,
            system.system.profile
        );
    } else {
        ProfileEngine::apply_profile(root, &system.system.profile, &links)?;
        println!("Activated toolchain {channel}");
    }
    Ok(())
}

/// Query actions for installed system state.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum QueryAction {
    /// List all installed packages.
    Installed,
    /// Find packages owning a specific file path.
    Owner { path: PathBuf },
    /// Detailed package inspection for a specific coordinate.
    Info { package: String, channel: String },
    /// Find installed orphan packages.
    Orphans,
}

/// Displays installed packages that are no longer referenced by system configuration or its dependencies.
pub fn query_orphans(root: &Path) -> Result<()> {
    let db_path = under_root(root, Path::new("/var/lib/sage"));
    let installed = sage_db::read_packages(&db_path)?;
    let config_path = under_root(root, Path::new("/etc/sage/system.toml"));
    let config = SystemConfig::load(&config_path)?;
    let orphans = crate::gc::find_orphans(&installed, &config);
    if orphans.is_empty() {
        println!("No orphan packages found.");
        return Ok(());
    }
    println!("Orphan packages ({}):", orphans.len());
    for orphan in orphans {
        println!(
            "  {}\t{}\t({})",
            orphan.key,
            orphan.version,
            crate::diff::format_bytes(orphan.installed_size as i64)
        );
    }
    Ok(())
}

/// Displays all packages installed in the target root LMDB state store.
pub fn query_installed(root: &Path) -> Result<()> {
    let db_path = under_root(root, Path::new("/var/lib/sage"));
    for package in sage_db::read_packages(&db_path)? {
        println!("{}\t{}\t{}", package.key, package.version, package.arch);
    }
    Ok(())
}

/// Displays package keys that claim ownership of a given file path.
pub fn query_owner(root: &Path, path: &Path) -> Result<()> {
    let db_path = under_root(root, Path::new("/var/lib/sage"));
    let relative = path.strip_prefix(root).unwrap_or(path);
    let relative = relative.strip_prefix("/").unwrap_or(relative);
    for owner in sage_db::read_owners(&db_path, &relative.to_string_lossy())? {
        println!("{owner}");
    }
    Ok(())
}

/// Displays detailed metadata and dependencies for an installed package.
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

/// Dispatches requested query action against target sysroot.
pub fn query_state(root: &Path, action: QueryAction) -> Result<()> {
    match action {
        QueryAction::Installed => query_installed(root),
        QueryAction::Owner { path } => query_owner(root, &path),
        QueryAction::Info { package, channel } => query_info(root, &package, &channel),
        QueryAction::Orphans => query_orphans(root),
    }
}
