//! Safe dirfd-relative extraction, hash validation, and three-way config preservation.

use crate::error::ArchiveError;
use crate::format::{ExtractionReport, FileRecord, HashOnly};
use crate::security::{
    TempGuard, archive_mode, clean_archive_path, ensure_directory, validate_link_target,
    validate_metadata_path,
};
use nix::errno::Errno;
use nix::fcntl::{OFlag, open, openat, renameat};
use nix::sys::stat::{Mode, fchmod};
use sage_core::hex;
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::fs::File;
use std::io::{self, Read, Write};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

static TEMP_ID: AtomicU64 = AtomicU64::new(0);

/// Reads one regular file from a payload already checked by `validate_package_payload`.
pub fn read_payload_file(package: &Path, relative: &Path) -> Result<Vec<u8>, ArchiveError> {
    let wanted = Path::new("data").join(relative);
    let decoder = zstd::Decoder::new(File::open(package)?)?;
    let mut archive = tar::Archive::new(decoder);
    for entry in archive.entries()? {
        let mut entry = entry?;
        if clean_archive_path(&entry.path()?)? != wanted {
            continue;
        }
        if !entry.header().entry_type().is_file() {
            return Err(ArchiveError::InvalidMetadata(format!(
                "expected a regular payload file: {}",
                relative.display()
            )));
        }
        let mut bytes = Vec::new();
        entry.read_to_end(&mut bytes)?;
        return Ok(bytes);
    }
    Err(ArchiveError::InvalidMetadata(format!(
        "missing payload file: {}",
        relative.display()
    )))
}

/// Reads the link target of a verified payload entry, or `None` for a regular file.
pub fn payload_link_target(
    package: &Path,
    relative: &Path,
) -> Result<Option<PathBuf>, ArchiveError> {
    let wanted = Path::new("data").join(relative);
    let mut archive = tar::Archive::new(zstd::Decoder::new(File::open(package)?)?);
    for entry in archive.entries()? {
        let entry = entry?;
        if clean_archive_path(&entry.path()?)? == wanted {
            if entry.header().entry_type().is_file() {
                return Ok(None);
            }
            if entry.header().entry_type().is_symlink() {
                return entry
                    .link_name()?
                    .map(|path| Some(path.into_owned()))
                    .ok_or_else(|| ArchiveError::InvalidMetadata("symlink has no target".into()));
            }
            break;
        }
    }
    Err(ArchiveError::InvalidMetadata(format!(
        "missing regular file or symlink: {}",
        relative.display()
    )))
}

/// Extracts verified regular payload files through dirfd-relative operations.
pub fn extract_package(
    package: impl AsRef<Path>,
    sysroot: impl AsRef<Path>,
    index: &[FileRecord],
) -> Result<Vec<PathBuf>, ArchiveError> {
    Ok(extract_package_with_config(package, sysroot, index, &BTreeMap::new())?.written)
}

/// Extracts a package and applies three-way hashes to files below `etc/`.
pub fn extract_package_with_config(
    package: impl AsRef<Path>,
    sysroot: impl AsRef<Path>,
    index: &[FileRecord],
    previous_hashes: &BTreeMap<String, String>,
) -> Result<ExtractionReport, ArchiveError> {
    validate_package_payload(package.as_ref(), index)?;
    extract_prevalidated_package(package.as_ref(), sysroot.as_ref(), index, previous_hashes)
}

/// Publishes a payload after the caller validated the same archive and index.
pub fn extract_prevalidated_package(
    package: &Path,
    sysroot: &Path,
    index: &[FileRecord],
    previous_hashes: &BTreeMap<String, String>,
) -> Result<ExtractionReport, ArchiveError> {
    let expected: BTreeMap<_, _> = index
        .iter()
        .map(|record| (record.path.clone(), record))
        .collect();
    let root_raw = open(
        sysroot,
        OFlag::O_RDONLY | OFlag::O_DIRECTORY | OFlag::O_CLOEXEC,
        Mode::empty(),
    )?;
    // SAFETY: `open` returned a new descriptor whose ownership transfers here once.
    let root = unsafe { OwnedFd::from_raw_fd(root_raw) };
    let decoder = zstd::Decoder::new(File::open(package)?)?;
    let mut archive = tar::Archive::new(decoder);
    let mut report = ExtractionReport::default();
    let mut seen = 0;
    for entry in archive.entries()? {
        let mut entry = entry?;
        let path = clean_archive_path(&entry.path()?)?;
        if path.starts_with(".METADATA") {
            if entry.header().entry_type().is_file() {
                validate_metadata_path(&path)?;
            }
            continue;
        }
        let Ok(relative) = path.strip_prefix("data") else {
            return Err(ArchiveError::InvalidMetadata(format!(
                "unsupported top-level entry {}",
                path.display()
            )));
        };
        if relative.as_os_str().is_empty() {
            continue;
        }
        if entry.header().entry_type().is_dir() {
            ensure_directory(&root, relative)?;
            continue;
        }
        if entry.header().entry_type().is_symlink() {
            let record = expected.get(relative).ok_or_else(|| {
                ArchiveError::InvalidMetadata(format!("unindexed link {}", relative.display()))
            })?;
            let target = entry
                .link_name()?
                .ok_or_else(|| ArchiveError::InvalidMetadata("symlink has no target".into()))?;
            write_verified_symlink(&root, relative, record, &target)?;
            report.written.push(relative.to_path_buf());
            seen += 1;
            continue;
        }
        if !entry.header().entry_type().is_file() {
            return Err(ArchiveError::UnsafePath(format!(
                "unsupported entry {}",
                relative.display()
            )));
        }
        let record = expected.get(relative).ok_or_else(|| {
            ArchiveError::InvalidMetadata(format!("unindexed file {}", relative.display()))
        })?;
        if entry.size() != record.size {
            return Err(ArchiveError::InvalidMetadata(format!(
                "size mismatch for {}",
                relative.display()
            )));
        }
        let previous = previous_hashes
            .get(&relative.to_string_lossy().into_owned())
            .map(String::as_str);
        match write_verified(&root, relative, record, previous, &mut entry)? {
            WriteOutcome::Written => report.written.push(relative.to_path_buf()),
            WriteOutcome::Preserved => report.preserved.push(relative.to_path_buf()),
            WriteOutcome::SageNew => report.sage_new.push(relative.with_file_name(format!(
                "{}.sage-new",
                relative.file_name().unwrap().to_string_lossy()
            ))),
        }
        seen += 1;
    }
    if seen != expected.len() {
        return Err(ArchiveError::InvalidMetadata(
            "archive payload does not match files.idx".into(),
        ));
    }
    Ok(report)
}

/// Validates every payload entry without writing to the target filesystem.
pub fn validate_package_payload(
    package: impl AsRef<Path>,
    index: &[FileRecord],
) -> Result<(), ArchiveError> {
    let expected: BTreeMap<_, _> = index
        .iter()
        .map(|record| (record.path.clone(), record))
        .collect();
    if expected.len() != index.len() {
        return Err(ArchiveError::InvalidMetadata(
            "files.idx contains duplicate canonical paths".into(),
        ));
    }
    let mut allowed_directories = BTreeSet::new();
    for path in expected.keys() {
        for ancestor in path.ancestors().skip(1) {
            if ancestor.as_os_str().is_empty() {
                break;
            }
            if expected.contains_key(ancestor) {
                return Err(ArchiveError::InvalidMetadata(format!(
                    "indexed payload {} has non-directory ancestor {}",
                    path.display(),
                    ancestor.display()
                )));
            }
            allowed_directories.insert(ancestor.to_path_buf());
        }
    }
    let decoder = zstd::Decoder::new(File::open(package)?)?;
    let mut archive = tar::Archive::new(decoder);
    let mut seen = 0;
    let mut payload_files = BTreeSet::new();
    let mut payload_ancestors = BTreeSet::new();
    for entry in archive.entries()? {
        let mut entry = entry?;
        let path = clean_archive_path(&entry.path()?)?;
        if path.starts_with(".METADATA") {
            if entry.header().entry_type().is_file() {
                validate_metadata_path(&path)?;
            }
            continue;
        }
        let Ok(relative) = path.strip_prefix("data") else {
            return Err(ArchiveError::InvalidMetadata(format!(
                "unsupported top-level entry {}",
                path.display()
            )));
        };
        if relative.as_os_str().is_empty() {
            continue;
        }
        if entry.header().entry_type().is_dir() {
            if !allowed_directories.contains(relative) {
                return Err(ArchiveError::InvalidMetadata(format!(
                    "unindexed payload directory {}",
                    relative.display()
                )));
            }
            continue;
        }
        if !payload_files.insert(relative.to_path_buf()) {
            return Err(ArchiveError::InvalidMetadata(format!(
                "duplicate payload file: {}",
                relative.display()
            )));
        }
        for ancestor in relative.ancestors().skip(1) {
            if ancestor.as_os_str().is_empty() {
                continue;
            }
            if payload_files.contains(ancestor) {
                return Err(ArchiveError::InvalidMetadata(format!(
                    "payload path collision between directory and file: {}",
                    ancestor.display()
                )));
            }
            payload_ancestors.insert(ancestor.to_path_buf());
        }
        if payload_ancestors.contains(relative) {
            return Err(ArchiveError::InvalidMetadata(format!(
                "payload path collision between directory and file: {}",
                relative.display()
            )));
        }
        if entry.header().entry_type().is_symlink() {
            let target = entry
                .link_name()?
                .ok_or_else(|| ArchiveError::InvalidMetadata("symlink has no target".into()))?;
            validate_link_target(relative, &target)?;
            let record = expected.get(relative).ok_or_else(|| {
                ArchiveError::InvalidMetadata(format!("unindexed link {}", relative.display()))
            })?;
            let bytes = target.as_os_str().as_encoded_bytes();
            if record.size != bytes.len() as u64 {
                return Err(ArchiveError::InvalidMetadata(format!(
                    "size mismatch for {}",
                    relative.display()
                )));
            }
            let actual = hex::encode(Sha256::digest(bytes));
            if actual != record.sha256 {
                return Err(ArchiveError::ChecksumMismatch {
                    path: relative.to_path_buf(),
                    expected: record.sha256.clone(),
                    actual,
                });
            }
            seen += 1;
            continue;
        }
        if !entry.header().entry_type().is_file() {
            return Err(ArchiveError::UnsafePath(format!(
                "unsupported entry {}",
                relative.display()
            )));
        }
        let record = expected.get(relative).ok_or_else(|| {
            ArchiveError::InvalidMetadata(format!("unindexed file {}", relative.display()))
        })?;
        if entry.size() != record.size {
            return Err(ArchiveError::InvalidMetadata(format!(
                "size mismatch for {}",
                relative.display()
            )));
        }
        verify_reader(relative, record, &mut entry)?;
        seen += 1;
    }
    if seen != expected.len() {
        return Err(ArchiveError::InvalidMetadata(
            "archive payload does not match files.idx".into(),
        ));
    }
    Ok(())
}

fn write_verified_symlink(
    root: &OwnedFd,
    path: &Path,
    record: &FileRecord,
    target: &Path,
) -> Result<(), ArchiveError> {
    validate_link_target(path, target)?;
    let parent = path.parent().unwrap_or(Path::new(""));
    let directory = ensure_directory(root, parent)?;
    let name = path
        .file_name()
        .and_then(|n| n.to_str())
        .ok_or_else(|| ArchiveError::UnsafePath(path.display().to_string()))?;
    let bytes = target.as_os_str().as_encoded_bytes();
    let actual = hex::encode(Sha256::digest(bytes));
    if actual != record.sha256 {
        return Err(ArchiveError::ChecksumMismatch {
            path: path.into(),
            expected: record.sha256.clone(),
            actual,
        });
    }
    let temporary = format!(
        ".sage-tmp-{}-{}",
        std::process::id(),
        TEMP_ID.fetch_add(1, Ordering::Relaxed)
    );
    let mut guard = TempGuard {
        dirfd: directory.as_raw_fd(),
        name: temporary.clone(),
        active: true,
    };
    nix::unistd::symlinkat(target, Some(directory.as_raw_fd()), temporary.as_str())?;
    renameat(
        Some(directory.as_raw_fd()),
        temporary.as_str(),
        Some(directory.as_raw_fd()),
        name,
    )?;
    guard.active = false;
    Ok(())
}

enum WriteOutcome {
    Written,
    Preserved,
    SageNew,
}

fn write_verified(
    root: &OwnedFd,
    path: &Path,
    record: &FileRecord,
    previous_hash: Option<&str>,
    reader: &mut impl Read,
) -> Result<WriteOutcome, ArchiveError> {
    let parent = path.parent().unwrap_or(Path::new(""));
    let directory = ensure_directory(root, parent)?;
    let name = path
        .file_name()
        .ok_or_else(|| ArchiveError::UnsafePath(path.display().to_string()))?;
    let live_hash = if path.starts_with("etc") && previous_hash.is_some() {
        hash_at(&directory, name)?
    } else {
        None
    };
    if let (Some(previous), Some(live)) = (previous_hash, live_hash.as_deref())
        && live != previous
        && record.sha256 == previous
    {
        verify_reader(path, record, reader)?;
        return Ok(WriteOutcome::Preserved);
    }
    let conflict = previous_hash
        .zip(live_hash.as_deref())
        .is_some_and(|(previous, live)| live != previous && record.sha256 != previous);
    let destination = if conflict {
        format!("{}.sage-new", name.to_string_lossy())
    } else {
        name.to_string_lossy().into_owned()
    };
    let temp = format!(
        ".sage-tmp-{}-{}",
        std::process::id(),
        TEMP_ID.fetch_add(1, Ordering::Relaxed)
    );
    let raw = openat(
        Some(directory.as_raw_fd()),
        temp.as_str(),
        OFlag::O_WRONLY | OFlag::O_CREAT | OFlag::O_EXCL | OFlag::O_CLOEXEC | OFlag::O_NOFOLLOW,
        archive_mode(record.mode),
    )?;
    let mut temporary = TempGuard {
        dirfd: directory.as_raw_fd(),
        name: temp.clone(),
        active: true,
    };
    // SAFETY: `openat` returned a fresh descriptor transferred exactly once.
    let mut output = unsafe { File::from_raw_fd(raw) };
    let mut hasher = Sha256::new();
    let copied = io::copy(
        &mut reader.take(record.size + 1),
        &mut HashWriter {
            output: &mut output,
            hash: &mut hasher,
        },
    )?;
    if copied != record.size {
        return Err(ArchiveError::InvalidMetadata(format!(
            "short payload for {}",
            path.display()
        )));
    }
    fchmod(output.as_raw_fd(), archive_mode(record.mode))?;
    output.sync_all()?;
    let actual = hex::encode(hasher.finalize());
    if actual != record.sha256 {
        return Err(ArchiveError::ChecksumMismatch {
            path: path.into(),
            expected: record.sha256.clone(),
            actual,
        });
    }
    drop(output);
    renameat(
        Some(directory.as_raw_fd()),
        temp.as_str(),
        Some(directory.as_raw_fd()),
        destination.as_str(),
    )?;
    temporary.active = false;
    Ok(if conflict {
        WriteOutcome::SageNew
    } else {
        WriteOutcome::Written
    })
}

fn hash_at(directory: &OwnedFd, name: &std::ffi::OsStr) -> Result<Option<String>, ArchiveError> {
    let raw = match openat(
        Some(directory.as_raw_fd()),
        name,
        OFlag::O_RDONLY | OFlag::O_CLOEXEC | OFlag::O_NOFOLLOW,
        nix::sys::stat::Mode::empty(),
    ) {
        Ok(raw) => raw,
        Err(Errno::ENOENT) => return Ok(None),
        Err(error) => return Err(error.into()),
    };
    // SAFETY: `openat` returned one fresh descriptor transferred to `File`.
    let mut file = unsafe { File::from_raw_fd(raw) };
    let mut hasher = Sha256::new();
    io::copy(&mut file, &mut HashOnly(&mut hasher))?;
    Ok(Some(hex::encode(hasher.finalize())))
}

fn verify_reader(
    path: &Path,
    record: &FileRecord,
    reader: &mut impl Read,
) -> Result<(), ArchiveError> {
    let mut hasher = Sha256::new();
    let copied = io::copy(
        &mut reader.take(record.size + 1),
        &mut HashOnly(&mut hasher),
    )?;
    let actual = hex::encode(hasher.finalize());
    if copied == record.size && actual == record.sha256 {
        Ok(())
    } else {
        Err(ArchiveError::ChecksumMismatch {
            path: path.into(),
            expected: record.sha256.clone(),
            actual,
        })
    }
}

struct HashWriter<'a> {
    output: &'a mut File,
    hash: &'a mut Sha256,
}

impl Write for HashWriter<'_> {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        let count = self.output.write(bytes)?;
        self.hash.update(&bytes[..count]);
        Ok(count)
    }
    fn flush(&mut self) -> io::Result<()> {
        self.output.flush()
    }
}
