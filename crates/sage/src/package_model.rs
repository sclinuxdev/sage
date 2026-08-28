//! Package catalog and artifact-source model used by install and build operations.

use anyhow::{Context, Result};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use crate::paths::under_root;
pub use sage_repo::{ReleaseLocation, ReleaseSource};

pub(crate) async fn obtain_release_archive(
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

/// Package candidates and channel aliases visible to the application.
pub struct AvailablePackages {
    pub universe: sage_solver::PackageUniverse,
    pub releases: BTreeMap<sage_core::PackageCoordinate, ReleaseSource>,
    pub aliases: BTreeMap<String, String>,
}

pub(crate) fn load_available(root: &Path) -> Result<AvailablePackages> {
    load_available_for_arch(root, None)
}

pub(crate) fn load_available_for_arch(
    root: &Path,
    architecture: Option<&str>,
) -> Result<AvailablePackages> {
    load_available_with_pool(root, architecture, None)
}

/// Loads synchronized repository releases and optional local build artifacts.
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
                    release.package.arch != wanted
                        && release.package.arch != "any"
                        && release.package.arch != "noarch"
                }) {
                    continue;
                }
                let coordinate = release.coordinate_for_channel(&canonical);
                universe.insert(release.package.for_channel(&canonical));
                releases.insert(
                    coordinate,
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
            let package = inspection.manifest.for_channel(&canonical);
            let coordinate = package.coordinate();
            universe.insert(package);
            releases.insert(
                coordinate,
                ReleaseSource {
                    release: sage_repo::IndexedRelease {
                        package: inspection.manifest,
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

pub(crate) fn canonical_channel(
    available: &AvailablePackages,
    selected: Option<&str>,
) -> Result<String> {
    let selected = selected.unwrap_or("system");
    available
        .aliases
        .get(selected)
        .cloned()
        .with_context(|| format!("channel '{selected}' has no synchronized index"))
}
