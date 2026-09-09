//! Garbage collection: cache cleaning and orphan package detection.

use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::fs;
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
pub fn clean_cache(root: &Path, all: bool) -> Result<CleanReport, SysError> {
    let mut report = CleanReport::default();
    let cache_dir = root.join("var/cache/sage");
    if cache_dir.is_dir() {
        clean_dir_recursive(&cache_dir, all, &mut report)?;
    }

    // Clean ephemeral temporary files in /var/lib/sage
    let lib_dir = root.join("var/lib/sage");
    if lib_dir.is_dir() {
        clean_temp_files(&lib_dir, &mut report)?;
    }

    Ok(report)
}

fn clean_dir_recursive(dir: &Path, all: bool, report: &mut CleanReport) -> Result<(), SysError> {
    let entries = match fs::read_dir(dir) {
        Ok(entries) => entries,
        Err(_) => return Ok(()),
    };

    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            clean_dir_recursive(&path, all, report)?;
            // Remove empty subdirectories
            let _ = fs::remove_dir(&path);
        } else if path.is_file() {
            let name = path.file_name().and_then(|n| n.to_str()).unwrap_or("");
            let is_temp = name.contains("sage-tmp")
                || name.contains(".part-")
                || name.starts_with(".services-config-")
                || name.starts_with(".rendered-services-");

            let is_package = name.ends_with(".pkg.tar.zst");

            if is_temp || (all && is_package) {
                if let Ok(meta) = fs::metadata(&path) {
                    report.bytes_freed += meta.len();
                }
                if fs::remove_file(&path).is_ok() {
                    report.files_removed += 1;
                }
            }
        }
    }

    Ok(())
}

fn clean_temp_files(dir: &Path, report: &mut CleanReport) -> Result<(), SysError> {
    let entries = match fs::read_dir(dir) {
        Ok(entries) => entries,
        Err(_) => return Ok(()),
    };

    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_file() {
            let name = path.file_name().and_then(|n| n.to_str()).unwrap_or("");
            if name.starts_with(".services-config-") || name.starts_with(".rendered-services-") {
                if let Ok(meta) = fs::metadata(&path) {
                    report.bytes_freed += meta.len();
                }
                if fs::remove_file(&path).is_ok() {
                    report.files_removed += 1;
                }
            }
        }
    }

    Ok(())
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

    // 1. Mark all explicitly declared root packages from system.toml as roots
    for selector in &system_config.packages {
        let (name, slot) = selector
            .split_once(':')
            .map_or((selector.as_str(), sage_core::DEFAULT_SLOT), |(n, s)| {
                (n, s)
            });

        for pkg in installed {
            if pkg.key.name == name
                && (pkg.key.slot == slot || selector == &pkg.key.name)
                && needed.insert(pkg.key.clone())
            {
                worklist.push_back(pkg.key.clone());
            }
        }
    }

    // 2. Mark configured provider packages as roots (e.g. init provider)
    for provider_name in system_config.providers.values() {
        for pkg in installed {
            if &pkg.key.name == provider_name && needed.insert(pkg.key.clone()) {
                worklist.push_back(pkg.key.clone());
            }
        }
    }

    // 3. Compute transitive closure of dependencies
    while let Some(current_key) = worklist.pop_front() {
        if let Some(pkg) = installed_map.get(&current_key) {
            for dep in &pkg.dependencies {
                for candidate in installed {
                    let direct = dep.name == candidate.key.name;
                    let provides_ok = direct || candidate.provides.contains(&dep.name);
                    let slot_ok = dep
                        .slot
                        .as_deref()
                        .is_none_or(|slot| slot == candidate.key.slot);
                    let channel_ok = dep
                        .channel
                        .as_deref()
                        .is_none_or(|chan| chan == candidate.key.channel);
                    let version_ok = dep.op.matches(&candidate.version, dep.version.as_ref());
                    if provides_ok
                        && slot_ok
                        && channel_ok
                        && version_ok
                        && needed.insert(candidate.key.clone())
                    {
                        worklist.push_back(candidate.key.clone());
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
