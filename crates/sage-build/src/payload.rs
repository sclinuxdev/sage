//! Payload carving, ELF inspection, and kernel-module validation.

use std::collections::BTreeSet;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::recipe::RecipeSpec;
use crate::BuildError;

pub struct PackageStagingArea {
    pub name: String,
    root: tempfile::TempDir,
    pub dependencies: Vec<String>,
    pub provides: Vec<String>,
}

impl PackageStagingArea {
    pub fn path(&self) -> &Path {
        self.root.path()
    }
}

/// Declaratively partitions build output using first-match ownership.
pub struct PayloadCarver;

impl PayloadCarver {
    pub fn carve_packages(
        destdir: &Path,
        recipe: &RecipeSpec,
    ) -> Result<Vec<PackageStagingArea>, BuildError> {
        let claims: Vec<_> = recipe
            .subpackages
            .iter()
            .map(|subpackage| {
                Ok((
                    compile_patterns(&subpackage.payload.files)?,
                    compile_patterns(&subpackage.payload.excludes)?,
                ))
            })
            .collect::<Result<_, BuildError>>()?;
        let main_patterns = compile_patterns(&recipe.build.payload.files)?;
        let main_excludes = compile_patterns(&recipe.build.payload.excludes)?;
        let mut areas = Vec::with_capacity(recipe.subpackages.len() + 1);
        areas.push(staging(
            &recipe.package.name,
            &recipe.package.dependencies,
            &recipe.package.provides,
        )?);
        for subpackage in &recipe.subpackages {
            areas.push(staging(
                &subpackage.name,
                &subpackage.dependencies,
                &subpackage.provides,
            )?);
        }
        let mut paths: Vec<_> = walkdir::WalkDir::new(destdir)
            .follow_links(false)
            .into_iter()
            .filter_map(|entry| match entry {
                Ok(entry) if entry.path() != destdir && !entry.file_type().is_dir() => {
                    Some(Ok(entry))
                }
                Ok(_) => None,
                Err(error) => Some(Err(error)),
            })
            .collect::<Result<_, _>>()?;
        paths.sort_by_key(|entry| entry.path().to_path_buf());
        for entry in paths {
            let relative = entry
                .path()
                .strip_prefix(destdir)
                .expect("walkdir keeps entries beneath its root");
            let owner = claims
                .iter()
                .position(|(includes, excludes)| {
                    matches_any(includes, relative) && !matches_any(excludes, relative)
                })
                .map(|index| index + 1)
                .or_else(|| {
                    let allowed = main_patterns.is_empty() || matches_any(&main_patterns, relative);
                    (allowed && !matches_any(&main_excludes, relative)).then_some(0)
                });
            if let Some(owner) = owner {
                link_entry(
                    entry.path(),
                    &areas[owner].path().join("data").join(relative),
                )?;
            }
        }
        Ok(areas)
    }
}

fn staging(
    name: &str,
    dependencies: &[String],
    provides: &[String],
) -> Result<PackageStagingArea, BuildError> {
    let root = tempfile::Builder::new().prefix("sage-package-").tempdir()?;
    fs::create_dir(root.path().join("data"))?;
    Ok(PackageStagingArea {
        name: name.into(),
        root,
        dependencies: dependencies.to_vec(),
        provides: provides.to_vec(),
    })
}

fn compile_patterns(patterns: &[String]) -> Result<Vec<glob::Pattern>, BuildError> {
    patterns
        .iter()
        .map(|pattern| Ok(glob::Pattern::new(pattern)?))
        .collect()
}

fn matches_any(patterns: &[glob::Pattern], path: &Path) -> bool {
    patterns.iter().any(|pattern| pattern.matches_path(path))
}

fn link_entry(source: &Path, target: &Path) -> Result<(), BuildError> {
    if let Some(parent) = target.parent() {
        fs::create_dir_all(parent)?;
    }
    let metadata = fs::symlink_metadata(source)?;
    if metadata.file_type().is_symlink() {
        let link = fs::read_link(source)?;
        if link.is_absolute() {
            return Err(BuildError::InvalidSpec(format!(
                "unsafe payload symlink {} -> {}",
                source.display(),
                link.display()
            )));
        }
        std::os::unix::fs::symlink(link, target)?;
    } else if fs::hard_link(source, target).is_err() {
        fs::copy(source, target)?;
        fs::set_permissions(target, metadata.permissions())?;
    }
    Ok(())
}

/// Dynamic symbols discovered from one independently carved package.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct ElfSymbols {
    pub provides: BTreeSet<String>,
    pub dependencies: BTreeSet<String>,
}

pub struct ElfScanner;

/// Summary of deterministic RUNPATH updates made below one DESTDIR.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct RunpathReport {
    pub library_dirs: Vec<PathBuf>,
    pub rewritten: Vec<PathBuf>,
}

impl ElfScanner {
    /// Scans regular files once and ignores non-ELF payloads without error.
    pub fn scan(root: &Path) -> Result<ElfSymbols, BuildError> {
        let mut symbols = ElfSymbols::default();
        for entry in walkdir::WalkDir::new(root).follow_links(false) {
            let entry = entry?;
            if !entry.file_type().is_file() {
                continue;
            }
            let bytes = fs::read(entry.path())?;
            if !bytes.starts_with(b"\x7fELF") {
                continue;
            }
            let goblin::Object::Elf(elf) = goblin::Object::parse(&bytes)? else {
                continue;
            };
            if let Some(soname) = elf.soname {
                symbols.provides.insert(format!("so:{soname}"));
            }
            symbols
                .dependencies
                .extend(elf.libraries.into_iter().map(|name| format!("so:{name}")));
        }
        Ok(symbols)
    }

    /// Replaces host-dependent ELF search paths with paths relative to each file.
    ///
    /// Directories containing a shared object with a SONAME are discovered in one
    /// pass. `extra_dirs` covers libraries supplied by another package in the same
    /// private channel. Every input must be relative to `root`, so generated paths
    /// cannot escape the future channel installation root.
    pub fn rewrite_private_runpaths(
        root: &Path,
        extra_dirs: &[PathBuf],
        patchelf: &Path,
    ) -> Result<RunpathReport, BuildError> {
        let mut library_dirs = BTreeSet::new();
        for directory in extra_dirs {
            validate_relative_path(directory)?;
            library_dirs.insert(directory.clone());
        }

        let mut dynamic_files = Vec::new();
        for entry in walkdir::WalkDir::new(root).follow_links(false) {
            let entry = entry?;
            if !entry.file_type().is_file() {
                continue;
            }
            let bytes = fs::read(entry.path())?;
            if !bytes.starts_with(b"\x7fELF") {
                continue;
            }
            let goblin::Object::Elf(elf) = goblin::Object::parse(&bytes)? else {
                continue;
            };
            let relative = entry
                .path()
                .strip_prefix(root)
                .map_err(|_| BuildError::InvalidSpec("ELF escaped DESTDIR".into()))?
                .to_path_buf();
            if elf.soname.is_some() {
                library_dirs.insert(relative.parent().unwrap_or(Path::new("")).to_path_buf());
            }
            if !elf.libraries.is_empty() {
                let retained = elf
                    .runpaths
                    .iter()
                    .chain(elf.rpaths.iter())
                    .flat_map(|paths| paths.split(':'))
                    .filter(|path| path == &"$ORIGIN" || path.starts_with("$ORIGIN/"))
                    .map(str::to_owned)
                    .collect::<BTreeSet<_>>();
                dynamic_files.push((relative, retained));
            }
        }

        let mut report = RunpathReport {
            library_dirs: library_dirs.iter().cloned().collect(),
            rewritten: Vec::new(),
        };
        if library_dirs.is_empty() {
            return Ok(report);
        }
        for (file, mut runpaths) in dynamic_files {
            let parent = file.parent().unwrap_or(Path::new(""));
            for directory in &library_dirs {
                runpaths.insert(origin_path(parent, directory)?);
            }
            let value = runpaths.into_iter().collect::<Vec<_>>().join(":");
            let output = Command::new(patchelf)
                .arg("--set-rpath")
                .arg(value)
                .arg(root.join(&file))
                .output()?;
            if !output.status.success() {
                return Err(BuildError::Patchelf {
                    path: file,
                    message: String::from_utf8_lossy(&output.stderr).trim().to_owned(),
                });
            }
            report.rewritten.push(file);
        }
        Ok(report)
    }
}

/// Ensures every kernel module tree belongs to the package's declared Slot.
///
/// Kernel packages and out-of-tree modules can therefore coexist without a
/// special package identity: `usr/lib/modules/<version>` and the manifest Slot
/// must agree. Packages without a modules directory take the fast no-op path.
pub fn validate_kernel_module_slot(root: &Path, slot: &str) -> Result<(), BuildError> {
    let modules = root.join("usr/lib/modules");
    if !modules.exists() {
        return Ok(());
    }
    for entry in fs::read_dir(modules)? {
        let entry = entry?;
        let version = entry.file_name();
        if version != std::ffi::OsStr::new(slot) {
            return Err(BuildError::InvalidSpec(format!(
                "kernel module directory '{}' does not match package slot '{slot}'",
                version.to_string_lossy()
            )));
        }
    }
    Ok(())
}

fn validate_relative_path(path: &Path) -> Result<(), BuildError> {
    if path.as_os_str().is_empty()
        || path
            .components()
            .any(|part| !matches!(part, std::path::Component::Normal(_)))
    {
        return Err(BuildError::InvalidSpec(format!(
            "private library directory must be a non-empty relative path: {}",
            path.display()
        )));
    }
    Ok(())
}

/// Computes a lexical relative path because both inputs are already confined to
/// one DESTDIR. No filesystem canonicalization is used, so symlinks cannot change
/// the result or make a reproducible build depend on the host filesystem.
fn origin_path(from: &Path, to: &Path) -> Result<String, BuildError> {
    let from = from.components().collect::<Vec<_>>();
    let to = to.components().collect::<Vec<_>>();
    let common = from
        .iter()
        .zip(&to)
        .take_while(|(left, right)| left == right)
        .count();
    let mut parts = vec!["..".to_owned(); from.len() - common];
    for part in &to[common..] {
        let std::path::Component::Normal(part) = part else {
            return Err(BuildError::InvalidSpec(
                "invalid private library path".into(),
            ));
        };
        parts.push(
            part.to_str()
                .ok_or_else(|| BuildError::InvalidSpec("non-UTF-8 private library path".into()))?
                .to_owned(),
        );
    }
    Ok(if parts.is_empty() {
        "$ORIGIN".into()
    } else {
        format!("$ORIGIN/{}", parts.join("/"))
    })
}
