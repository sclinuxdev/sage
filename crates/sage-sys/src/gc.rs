//! Garbage collection: cache cleaning and orphan package detection.

use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::path::Path;

use sage_core::PackageKey;
use sage_db::InstalledPackage;

use crate::SysError;
use crate::state::SystemConfig;

/// Summary statistics of a cache cleaning operation.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct CleanReport {
    pub files_removed: usize,
    pub bytes_freed: u64,
}

/// Cleans temporary files, obsolete downloads, and optionally all cached packages.
/// Anchored to directory descriptors with O_NOFOLLOW to guarantee that crafted
/// symlinks within the cache cannot escape the target sysroot.
pub fn clean_cache(root: &Path, all: bool) -> Result<CleanReport, SysError> {
    crate::fs::clean_cache_beneath(root, all)
}

/// Computes the list of installed packages that are not roots and not required transitively.
pub fn find_orphans(
    installed: &[InstalledPackage],
    system_config: &SystemConfig,
) -> Vec<InstalledPackage> {
    let installed_map: BTreeMap<&PackageKey, &InstalledPackage> =
        installed.iter().map(|pkg| (&pkg.key, pkg)).collect();

    let mut needed = BTreeSet::new();
    let mut worklist = VecDeque::new();

    // Rebuild only prunes main/system. Other channels remain independent roots.
    let (Ok(mut roots), Ok(providers)) = (
        system_config.package_keys("main/system"),
        system_config.provider_preferences("main/system"),
    ) else {
        // Invalid declarations cannot safely establish that anything is unused.
        return Vec::new();
    };
    roots.extend(providers.into_values());
    roots.extend(
        installed
            .iter()
            .filter(|pkg| pkg.key.channel != "main/system")
            .map(|pkg| pkg.key.clone()),
    );
    for key in roots {
        if installed_map.contains_key(&key) && needed.insert(key.clone()) {
            worklist.push_back(key);
        }
    }
    let mut universe = sage_solver::PackageUniverse::default();
    for pkg in installed {
        universe.insert(sage_core::Package::from_release(
            pkg.key.clone(),
            pkg.version.clone(),
            pkg.dependencies.clone(),
            pkg.provides.clone(),
        ));
    }
    // All matching virtual providers are retained conservatively because installed
    // dependency rows do not record which alternative satisfied each edge.
    while let Some(current_key) = worklist.pop_front() {
        if let Some(pkg) = installed_map.get(&current_key) {
            for dep in &pkg.dependencies {
                for key in universe.matching_dependency_keys(&current_key, dep) {
                    if needed.insert(key.clone()) {
                        worklist.push_back(key);
                    }
                }
            }
        }
    }

    // 4. Any installed package not in `needed` is an orphan
    installed
        .iter()
        .filter(|pkg| !needed.contains(&pkg.key))
        .cloned()
        .collect()
}
