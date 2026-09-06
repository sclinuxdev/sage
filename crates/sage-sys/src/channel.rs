//! Software repository channels, package availability pools, and archive downloads.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};
use sage_core::under_root;
pub use sage_repo::{ReleaseLocation, ReleaseSource};

/// Synchronizes remote channel indices according to system configuration.
pub async fn sync_channels(root: &Path, selected: Option<&str>, dry_run: bool) -> Result<()> {
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

/// Checks whether package architecture matches target host architecture or architecture-independent rules.
pub(crate) fn arch_matches(arch: &str, wanted: &str) -> bool {
    arch == wanted || arch == "any" || arch == "noarch"
}

/// Identifies whether a given path has standard Sage package archive extension.
pub(crate) fn is_package_archive(path: &Path) -> bool {
    path.file_name()
        .and_then(|name| name.to_str())
        .is_some_and(|name| name.ends_with(".pkg.tar.zst"))
}

/// Normalizes channel alias into a canonical channel namespace format.
pub(crate) fn qualify_channel(channel: &str) -> String {
    if channel.contains('/') {
        channel.to_string()
    } else {
        format!("main/{channel}")
    }
}

/// Index of available packages across all enabled remote channels and optional local pools.
pub struct AvailablePackages {
    /// In-memory solver universe representing solvable package dependencies.
    pub universe: sage_solver::PackageUniverse,
    /// Location and metadata for each available release coordinate.
    pub releases: BTreeMap<(sage_core::PackageKey, sage_core::Version), ReleaseSource>,
    /// Channel alias mapping table for fast canonical resolution.
    pub aliases: BTreeMap<String, String>,
}

impl AvailablePackages {
    /// Injects installed packages into universe if missing, guaranteeing base state visibility.
    pub(crate) fn register_installed(&mut self, installed: &[sage_db::InstalledPackage]) {
        for package in installed {
            if !self
                .universe
                .versions(&package.key)
                .any(|version| version == &package.version)
            {
                let mut release = sage_core::Package::from_release(
                    package.key.clone(),
                    package.version.clone(),
                    package.dependencies.clone(),
                    package.provides.clone(),
                );
                release.arch.clone_from(&package.arch);
                release.conflicts.clone_from(&package.conflicts);
                release.installed_size = package.installed_size;
                self.universe.insert(release);
            }
        }
    }
}

/// Loads available packages from channel index databases and an optional local package pool.
pub fn load_available_with_pool(
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
            // Register configured names even before their remote index has
            // been synchronized. This is required during a staged bootstrap,
            // where the local output pool is the authoritative source and
            // intentionally starts without an LMDB index.
            aliases.insert(alias.into(), canonical.clone());
            aliases.insert(canonical.clone(), canonical.clone());
            target_roots.insert(canonical.clone(), subchannel.target_root.clone());
            // Source builds with an explicit pool form a closed bootstrap
            // universe. Falling back to a synchronized channel here can mix
            // old-format or newer binary releases into an otherwise local,
            // reproducible self-hosting graph. Configured aliases are still
            // registered above so local package identities canonicalize in
            // exactly the same way as normal repository packages.
            if local_pool.is_some() {
                continue;
            }
            let index_path = cache.join(&channel_name).join(alias).join("index.mdb");
            if !index_path.exists() {
                continue;
            }
            let url = sage_repo::subchannel_url(&channel, sub_name, subchannel);
            for release in sage_repo::RepositoryIndex::open(&index_path)?.all_releases()? {
                if architecture.is_some_and(|wanted| !arch_matches(&release.package.arch, wanted)) {
                    continue;
                }
                let coordinate = release.coordinate_for_channel(&canonical);
                universe.insert(release.package.for_channel(&canonical));
                releases.insert(
                    (coordinate.key, coordinate.version),
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
        let mut packages: Vec<_> = sage_core::walkdir::WalkDir::new(pool)
            .follow_links(false)
            .into_iter()
            .filter_map(|entry| entry.ok())
            .map(|entry| entry.into_path())
            .filter(|path| is_package_archive(path))
            .collect();
        packages.sort();
        for path in packages {
            let inspection = sage_archive::inspect_package(&path)
                .with_context(|| format!("failed to inspect local package {}", path.display()))?;
            if architecture.is_some_and(|wanted| !arch_matches(&inspection.manifest.arch, wanted)) {
                continue;
            }
            let canonical = aliases
                .get(&inspection.manifest.channel)
                .cloned()
                .unwrap_or_else(|| qualify_channel(&inspection.manifest.channel));
            aliases
                .entry(inspection.manifest.channel.clone())
                .or_insert_with(|| canonical.clone());
            aliases
                .entry(canonical.clone())
                .or_insert_with(|| canonical.clone());
            let package = inspection.manifest.for_channel(&canonical);
            let coordinate = package.coordinate();
            universe.insert(package);
            releases.insert(
                (coordinate.key, coordinate.version),
                ReleaseSource {
                    release: sage_repo::IndexedRelease {
                        package: inspection.manifest,
                        archive: path
                            .strip_prefix(pool)
                            .unwrap_or(path.as_path())
                            .to_string_lossy()
                            .into_owned(),
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

/// Resolves an alias or canonical channel name to its registered canonical coordinate.
pub fn canonical_channel(available: &AvailablePackages, selected: Option<&str>) -> Result<String> {
    let selected = selected.unwrap_or("system");
    available
        .aliases
        .get(selected)
        .cloned()
        .with_context(|| format!("channel '{selected}' has no synchronized index"))
}

/// Retrieves package archive, downloading remote artifacts into cache or returning local path.
pub async fn obtain_release_archive(
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
