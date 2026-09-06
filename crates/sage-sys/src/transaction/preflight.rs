//! Preflight verification, file conflict inspection, and program validity checks before transaction commit.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};
use sage_core::{hex, under_root};
use sha2::{Digest, Sha256};

use super::package::{PackageDeclarations, package_ownership};
use crate::channel::{AvailablePackages, arch_matches, obtain_release_archive};
use crate::recovery::should_preserve_config;

/// Computes a hash-addressed relative path for package declaration artifacts.
pub(crate) fn declaration_path(dir: &str, key: &sage_core::PackageKey) -> PathBuf {
    let digest = hex::encode(Sha256::digest(key.canonical_id().as_bytes()));
    PathBuf::from(dir).join(format!("{digest}.toml"))
}

/// Atomically writes content to a target path beneath sysroot using tempfile and rename.
pub(crate) fn write_atomic_under_root(root: &Path, relative: &Path, bytes: &[u8]) -> Result<()> {
    if relative.components().any(|component| {
        matches!(
            component,
            std::path::Component::ParentDir | std::path::Component::RootDir
        )
    }) {
        bail!("unsafe state path {}", relative.display());
    }
    let target = root.join(relative);
    let parent = target.parent().context("state path has no parent")?;
    std::fs::create_dir_all(parent)?;
    let canonical_root = std::fs::canonicalize(root)?;
    let canonical_parent = std::fs::canonicalize(parent)?;
    if !canonical_parent.starts_with(canonical_root) {
        bail!("state path escapes sysroot: {}", target.display());
    }
    let temporary = parent.join(format!(".sage-state-{}", std::process::id()));
    let mut options = std::fs::OpenOptions::new();
    use std::io::Write as _;
    options
        .write(true)
        .create_new(true)
        .open(&temporary)?
        .write_all(bytes)?;
    std::fs::rename(temporary, target)?;
    Ok(())
}

/// Preflights candidates, inspecting archives, verifying checksums, and catching file conflicts.
pub(crate) async fn preflight_packages(
    root: &Path,
    database: &sage_db::SageDatabase,
    available: &AvailablePackages,
    architecture: &str,
    changes: &[(sage_core::PackageKey, sage_core::Version)],
    retired: &[sage_db::InstalledPackage],
) -> Result<Vec<(sage_core::PackageKey, sage_core::Version)>> {
    let package_cache = under_root(root, Path::new("/var/cache/sage/packages"));
    let engine = sage_repo::DownloadEngine::new(&package_cache)?;
    let mut planned = BTreeMap::<String, sage_core::PackageKey>::new();
    let mut final_paths = BTreeMap::<sage_core::PackageKey, BTreeSet<String>>::new();
    for (key, version) in changes {
        let source = available
            .releases
            .get(&(key.clone(), version.clone()))
            .with_context(|| format!("index record disappeared for {key} {version}"))?;
        let archive = obtain_release_archive(&engine, &package_cache, source).await?;
        let inspection = sage_archive::inspect_package(&archive)?;
        let coordinate = inspection.manifest.coordinate_for_channel(&key.channel);
        if coordinate.key != *key || coordinate.version != *version {
            bail!(
                "archive identity {} {} does not match selected {} {}",
                coordinate.key,
                coordinate.version,
                key,
                version
            );
        }
        if !arch_matches(&inspection.manifest.arch, architecture) {
            bail!(
                "package {} has architecture {}, expected {}",
                key,
                inspection.manifest.arch,
                architecture
            );
        }
        sage_archive::validate_package_payload(&archive, &inspection.files)?;
        let prefix = source
            .target_root
            .strip_prefix("/")
            .unwrap_or(&source.target_root);
        let declarations = PackageDeclarations::parse(&inspection, key)?;
        let ownership = package_ownership(prefix, &inspection.files, &declarations);
        for path in &ownership {
            if let Some(owner) = planned.insert(path.clone(), key.clone()) {
                bail!("transaction packages {owner} and {key} both own {path}");
            }
        }
        final_paths.insert(key.clone(), ownership.into_iter().collect());
    }

    // Reject hierarchy replacements before journaling. Publishing cannot create
    // a directory below a retained file, and recovery must not discover that late.
    let mut installed_paths = BTreeMap::<String, BTreeSet<sage_core::PackageKey>>::new();
    for package in database.packages()? {
        for path in package.files {
            installed_paths
                .entry(path)
                .or_default()
                .insert(package.key.clone());
        }
    }
    // Only paths that retirement really deletes may become directories. Shared
    // retained ownership and preserved administrator configuration still block
    // a handoff, even when one of their owners is in the retirement set.
    let mut retiring_paths = BTreeSet::new();
    for (path, owners) in &installed_paths {
        if owners
            .iter()
            .all(|owner| retired.iter().any(|package| package.key == *owner))
        {
            let mut removable = true;
            for package in retired
                .iter()
                .filter(|package| owners.contains(&package.key))
            {
                if should_preserve_config(&root.join(path), path, &package.config_hashes)? {
                    removable = false;
                    break;
                }
            }
            if removable {
                retiring_paths.insert(path.clone());
            }
        }
    }
    for (path, claimant) in &planned {
        let components = Path::new(path)
            .components()
            .filter_map(|component| match component {
                std::path::Component::Normal(name) => Some(name),
                _ => None,
            })
            .collect::<Vec<_>>();
        let mut destination = root.to_path_buf();
        for (index, component) in components.iter().enumerate() {
            destination.push(component);
            match std::fs::symlink_metadata(&destination) {
                Ok(metadata) if index + 1 < components.len() && !metadata.is_dir() => {
                    if retiring_paths
                        .contains(destination.strip_prefix(root)?.to_string_lossy().as_ref())
                    {
                        // Retirement unlinks this ancestor before extraction. Do
                        // not walk into a stale file or follow its symlink target.
                        break;
                    }
                    bail!("file hierarchy conflict for {path}: an ancestor is not a directory")
                }
                Ok(metadata) if index + 1 == components.len() && metadata.is_dir() => {
                    bail!("file conflict for {path}: destination is a directory")
                }
                Ok(_) => {}
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => break,
                Err(error) => return Err(error.into()),
            }
        }
        for ancestor in Path::new(path).ancestors().skip(1) {
            if ancestor.as_os_str().is_empty() {
                break;
            }
            let ancestor = ancestor.to_string_lossy();
            if let Some(owner) = planned.get(ancestor.as_ref()) {
                bail!(
                    "transaction ownership paths conflict: {owner} owns ancestor {ancestor} of {claimant}'s {path}"
                );
            }
            if let Some(owners) = installed_paths.get(ancestor.as_ref())
                && !retiring_paths.contains(ancestor.as_ref())
            {
                bail!(
                    "file hierarchy conflict for {path}: ancestor {ancestor} is currently owned by {owners:?}"
                );
            }
        }
        let descendant_prefix = format!("{path}/");
        if let Some((descendant, owners)) = installed_paths
            .range(descendant_prefix.clone()..)
            .next()
            .filter(|(descendant, _)| descendant.starts_with(&descendant_prefix))
        {
            bail!(
                "file hierarchy conflict for {path}: descendant {descendant} is currently owned by {owners:?}"
            );
        }
    }

    // A current owner may release a path in this same transaction. Add a
    // publication edge so its replacement commits before the new claimant;
    // owners absent from the plan, or retaining the path, remain conflicts.
    let mut successors = changes
        .iter()
        .map(|(key, _)| (key.clone(), BTreeSet::new()))
        .collect::<BTreeMap<_, _>>();
    let mut indegree = changes
        .iter()
        .map(|(key, _)| (key.clone(), 0_usize))
        .collect::<BTreeMap<_, _>>();
    for (path, claimant) in &planned {
        for owner in database.owners(path)? {
            if owner == *claimant || retired.iter().any(|package| package.key == owner) {
                continue;
            }
            let releases = final_paths
                .get(&owner)
                .is_some_and(|paths| !paths.contains(path));
            if !releases {
                bail!("file conflict for {path}: currently owned by {owner}");
            }
            if successors
                .get_mut(&owner)
                .is_some_and(|targets| targets.insert(claimant.clone()))
            {
                *indegree
                    .get_mut(claimant)
                    .expect("planned claimant has an indegree") += 1;
            }
        }
    }

    let positions = changes
        .iter()
        .enumerate()
        .map(|(index, (key, _))| (key.clone(), index))
        .collect::<BTreeMap<_, _>>();
    let versions = changes.iter().cloned().collect::<BTreeMap<_, _>>();
    let mut ready = indegree
        .iter()
        .filter(|(_, degree)| **degree == 0)
        .map(|(key, _)| (positions[key], key.clone()))
        .collect::<BTreeSet<_>>();
    let mut ordered = Vec::with_capacity(changes.len());
    while let Some((_, key)) = ready.pop_first() {
        ordered.push((key.clone(), versions[&key].clone()));
        for claimant in &successors[&key] {
            let degree = indegree
                .get_mut(claimant)
                .expect("planned claimant has an indegree");
            *degree -= 1;
            if *degree == 0 {
                ready.insert((positions[claimant], claimant.clone()));
            }
        }
    }
    if ordered.len() != changes.len() {
        bail!("cyclic file ownership handoff in package transaction");
    }
    Ok(ordered)
}

/// Checks a program against the final payload/retained-file overlay without publishing it.
pub(crate) fn validate_planned_program(
    root: &Path,
    program: &Path,
    payloads: &BTreeMap<PathBuf, (PathBuf, PathBuf, u32)>,
    removed: &BTreeSet<PathBuf>,
) -> Result<PathBuf> {
    use std::os::unix::fs::PermissionsExt as _;
    let mut pending = program
        .strip_prefix(root)?
        .components()
        .map(|component| PathBuf::from(component.as_os_str()))
        .collect::<std::collections::VecDeque<_>>();
    let mut relative = PathBuf::new();
    let mut links = BTreeSet::new();
    while let Some(component) = pending.pop_front() {
        if component == Path::new(".") {
            continue;
        }
        if component == Path::new("..") {
            if !relative.pop() {
                bail!("program escapes sysroot: {}", program.display());
            }
            continue;
        }
        relative.push(component);
        let planned_directory = payloads
            .keys()
            .any(|path| path != &relative && path.starts_with(&relative));
        if removed.contains(&relative) && !planned_directory {
            bail!(
                "planned program {} uses removed path {}",
                program.display(),
                relative.display()
            );
        }
        let (link, regular, mode) = if let Some((archive, path, mode)) = payloads.get(&relative) {
            (
                sage_archive::payload_link_target(archive, path)?,
                true,
                *mode,
            )
        } else if planned_directory {
            // Payload ancestors are created by extraction, even on a fresh root.
            (None, false, 0)
        } else {
            let path = root.join(&relative);
            let metadata = std::fs::symlink_metadata(&path)
                .with_context(|| format!("missing planned program {}", program.display()))?;
            (
                metadata
                    .is_symlink()
                    .then(|| std::fs::read_link(&path))
                    .transpose()?,
                metadata.is_file(),
                metadata.permissions().mode(),
            )
        };
        if let Some(link) = link {
            if link.is_absolute() || !links.insert(relative.clone()) {
                bail!("unsafe program symlink {}", relative.display());
            }
            relative.pop();
            for component in link.components().rev() {
                pending.push_front(PathBuf::from(component.as_os_str()));
            }
        } else if pending.is_empty() {
            if !regular || mode & 0o111 == 0 {
                bail!("planned program is not executable: {}", program.display());
            }
            return Ok(root.join(relative));
        } else if regular {
            bail!(
                "program ancestor is not a directory: {}",
                relative.display()
            );
        }
    }
    bail!(
        "planned program is not a regular file: {}",
        program.display()
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn planned_program_links_resolve_in_the_final_filesystem() {
        use std::os::unix::{fs::PermissionsExt as _, fs::symlink};
        let root = tempfile::tempdir().unwrap();
        let stage = tempfile::tempdir().unwrap();
        let data = stage.path().join("data");
        std::fs::create_dir_all(data.join("usr/bin")).unwrap();
        std::fs::create_dir_all(stage.path().join(".METADATA")).unwrap();
        std::fs::write(data.join("usr/bin/real"), b"#!/bin/sh\nexit 0\n").unwrap();
        std::fs::set_permissions(
            data.join("usr/bin/real"),
            std::fs::Permissions::from_mode(0o755),
        )
        .unwrap();
        symlink("real", data.join("usr/bin/ctl")).unwrap();
        std::fs::write(
            stage.path().join(".METADATA/manifest.toml"),
            "schema_version=1\nname=\"program\"\nversion=\"1\"\nrelease=1\narch=\"noarch\"\nchannel=\"system\"\ndescription=\"Program\"\nlicense=\"MIT\"\n",
        )
        .unwrap();
        let records = sage_archive::build_file_index(&data).unwrap();
        std::fs::write(
            stage.path().join(".METADATA/files.idx"),
            sage_archive::format_file_index(&records),
        )
        .unwrap();
        let archive = stage.path().join("program.pkg.tar.zst");
        sage_archive::create_package(stage.path(), &archive, 1).unwrap();
        sage_archive::validate_package_payload(&archive, &records).unwrap();
        let mut payloads = records
            .into_iter()
            .map(|record| {
                (
                    record.path.clone(),
                    (archive.clone(), record.path, record.mode),
                )
            })
            .collect::<BTreeMap<_, _>>();
        let program = root.path().join("usr/bin/ctl");
        validate_planned_program(root.path(), &program, &payloads, &BTreeSet::new()).unwrap();
        payloads.remove(Path::new("usr/bin/real"));
        let removed = BTreeSet::from([PathBuf::from("usr/bin/real")]);
        assert!(validate_planned_program(root.path(), &program, &payloads, &removed).is_err());
        std::fs::create_dir_all(root.path().join("usr/bin")).unwrap();
        symlink("real", &program).unwrap();
        std::fs::copy(data.join("usr/bin/real"), root.path().join("usr/bin/real")).unwrap();
        validate_planned_program(root.path(), &program, &BTreeMap::new(), &BTreeSet::new())
            .unwrap();
        assert!(
            validate_planned_program(root.path(), &program, &BTreeMap::new(), &removed).is_err()
        );
        std::fs::remove_file(&program).unwrap();
        symlink(data.join("usr/bin/real"), &program).unwrap();
        assert!(
            validate_planned_program(root.path(), &program, &BTreeMap::new(), &BTreeSet::new())
                .is_err()
        );
    }
}
