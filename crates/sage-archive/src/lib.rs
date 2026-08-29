//! Deterministic tar.zst packaging, constant-cost inspection, and safe extraction.

use nix::errno::Errno;
use nix::fcntl::{open, openat, renameat, OFlag};
use nix::sys::stat::{fchmod, mkdirat, Mode};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::os::unix::fs::MetadataExt;
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use thiserror::Error;

static TEMP_ID: AtomicU64 = AtomicU64::new(0);

/// Archive format and extraction failures.
#[derive(Debug, Error)]
pub enum ArchiveError {
    #[error("I/O error: {0}")]
    Io(#[from] io::Error),
    #[error("system call failed: {0}")]
    Nix(#[from] Errno),
    #[error("TOML metadata error: {0}")]
    Toml(#[from] toml::de::Error),
    #[error("invalid archive metadata: {0}")]
    InvalidMetadata(String),
    #[error("checksum mismatch for {path}: expected {expected}, calculated {actual}")]
    ChecksumMismatch {
        path: PathBuf,
        expected: String,
        actual: String,
    },
    #[error("unsafe or unsupported archive path: {0}")]
    UnsafePath(String),
}

/// Schema-v1 package manifest shared with recipes and solver records.
pub type PackageManifest = sage_core::Package;

/// Build-tool provenance shared with package manifests.
pub use sage_core::ManagedBuildTool;

/// One integrity record from `.METADATA/files.idx`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FileRecord {
    pub path: PathBuf,
    pub mode: u32,
    pub size: u64,
    pub sha256: String,
}

/// Metadata read before the first payload entry.
#[derive(Debug, Clone)]
pub struct PackageInspection {
    pub manifest: PackageManifest,
    pub files: Vec<FileRecord>,
    pub optional: BTreeMap<String, Vec<u8>>,
}

/// Reads only the leading metadata section and stops at the first `data/` entry.
pub fn inspect_package(path: impl AsRef<Path>) -> Result<PackageInspection, ArchiveError> {
    let file = File::open(path)?;
    let decoder = zstd::Decoder::new(file)?;
    let mut archive = tar::Archive::new(decoder);
    let mut metadata = BTreeMap::new();
    for entry in archive.entries()? {
        let mut entry = entry?;
        let path = clean_archive_path(&entry.path()?)?;
        if path.starts_with("data") {
            break;
        }
        if path.starts_with(".METADATA") && entry.header().entry_type().is_file() {
            validate_metadata_path(&path)?;
            let mut bytes = Vec::new();
            entry.read_to_end(&mut bytes)?;
            metadata.insert(path.to_string_lossy().into_owned(), bytes);
        }
    }
    let manifest_bytes = metadata
        .remove(".METADATA/manifest.toml")
        .ok_or_else(|| ArchiveError::InvalidMetadata("missing manifest.toml".into()))?;
    let manifest: PackageManifest = toml::from_str(
        std::str::from_utf8(&manifest_bytes)
            .map_err(|_| ArchiveError::InvalidMetadata("manifest is not UTF-8".into()))?,
    )?;
    sage_core::validate_schema(manifest.schema_version)
        .map_err(|error| ArchiveError::InvalidMetadata(error.to_string()))?;
    sage_core::validate_spdx_expression(&manifest.license)
        .map_err(|error| ArchiveError::InvalidMetadata(error.to_string()))?;
    let index = metadata
        .remove(".METADATA/files.idx")
        .ok_or_else(|| ArchiveError::InvalidMetadata("missing files.idx".into()))?;
    let files = parse_file_index(&index)?;
    Ok(PackageInspection {
        manifest,
        files,
        optional: metadata,
    })
}

/// Parses the compact TSV index while validating all paths and hashes.
pub fn parse_file_index(bytes: &[u8]) -> Result<Vec<FileRecord>, ArchiveError> {
    let text = std::str::from_utf8(bytes)
        .map_err(|_| ArchiveError::InvalidMetadata("files.idx is not UTF-8".into()))?;
    text.lines()
        .filter(|line| !line.is_empty() && !line.starts_with('#'))
        .map(|line| {
            let mut fields = line.split('\t');
            let path = clean_archive_path(Path::new(fields.next().unwrap_or_default()))?;
            let mode = u32::from_str_radix(fields.next().unwrap_or_default(), 8)
                .map_err(|_| ArchiveError::InvalidMetadata(format!("invalid mode in '{line}'")))?;
            let size =
                fields.next().unwrap_or_default().parse().map_err(|_| {
                    ArchiveError::InvalidMetadata(format!("invalid size in '{line}'"))
                })?;
            let sha256 = fields.next().unwrap_or_default();
            if fields.next().is_some()
                || sha256.len() != 64
                || !sha256.bytes().all(|byte| byte.is_ascii_hexdigit())
            {
                return Err(ArchiveError::InvalidMetadata(format!(
                    "invalid SHA-256 in '{line}'"
                )));
            }
            Ok(FileRecord {
                path,
                mode,
                size,
                sha256: sha256.to_ascii_lowercase(),
            })
        })
        .collect()
}

/// Builds a sorted integrity index for a staged payload tree.
pub fn build_file_index(root: &Path) -> Result<Vec<FileRecord>, ArchiveError> {
    fn visit(
        root: &Path,
        directory: &Path,
        output: &mut Vec<FileRecord>,
    ) -> Result<(), ArchiveError> {
        let mut entries: Vec<_> = fs::read_dir(directory)?.collect::<Result<_, _>>()?;
        entries.sort_by_key(|entry| entry.file_name());
        for entry in entries {
            let path = entry.path();
            let metadata = fs::symlink_metadata(&path)?;
            if metadata.is_dir() {
                visit(root, &path, output)?;
            } else if metadata.is_file() {
                let mut file = File::open(&path)?;
                let mut hasher = Sha256::new();
                io::copy(&mut file, &mut HashOnly(&mut hasher))?;
                output.push(FileRecord {
                    path: path
                        .strip_prefix(root)
                        .expect("recursive walk stays below root")
                        .to_path_buf(),
                    mode: metadata.mode() & 0o7777,
                    size: metadata.len(),
                    sha256: hex::encode(hasher.finalize()),
                });
            } else if metadata.file_type().is_symlink() {
                let target = fs::read_link(&path)?;
                let bytes = target.as_os_str().as_encoded_bytes();
                output.push(FileRecord {
                    path: path
                        .strip_prefix(root)
                        .expect("recursive walk stays below root")
                        .to_path_buf(),
                    mode: 0o777,
                    size: bytes.len() as u64,
                    sha256: hex::encode(Sha256::digest(bytes)),
                });
            } else {
                return Err(ArchiveError::UnsafePath(path.display().to_string()));
            }
        }
        Ok(())
    }
    let mut records = Vec::new();
    visit(root, root, &mut records)?;
    Ok(records)
}

/// Encodes index records using the schema-v1 compact TSV representation.
pub fn format_file_index(records: &[FileRecord]) -> String {
    let mut output = String::from("# path\tmode\tsize\tsha256\n");
    for record in records {
        use std::fmt::Write as _;
        let _ = writeln!(
            output,
            "{}\t{:04o}\t{}\t{}",
            record.path.display(),
            record.mode,
            record.size,
            record.sha256
        );
    }
    output
}

/// Creates a deterministic archive with metadata entries before payload entries.
pub fn create_package(
    source_dir: impl AsRef<Path>,
    output: impl AsRef<Path>,
    compression_level: i32,
) -> Result<(), ArchiveError> {
    let source = source_dir.as_ref();
    for required in [".METADATA/manifest.toml", ".METADATA/files.idx", "data"] {
        if !source.join(required).exists() {
            return Err(ArchiveError::InvalidMetadata(format!("missing {required}")));
        }
    }
    let mut paths = collect_paths(source)?;
    for path in &paths {
        let relative = path
            .strip_prefix(source)
            .expect("collected path stays below source");
        if relative.starts_with(".METADATA") && path.is_file() {
            validate_metadata_path(relative)?;
        }
    }
    paths.sort_by_key(|path| {
        let relative = path.strip_prefix(source).unwrap();
        (!relative.starts_with(".METADATA"), relative.to_path_buf())
    });
    let file = File::create(output)?;
    let mut encoder = zstd::Encoder::new(file, compression_level)?;
    encoder.include_checksum(true)?;
    let mut builder = tar::Builder::new(encoder);
    builder.mode(tar::HeaderMode::Deterministic);
    for path in paths {
        append_deterministic(&mut builder, source, &path)?;
    }
    let encoder = builder.into_inner()?;
    encoder.finish()?;
    Ok(())
}

/// Keeps package installation declarative by rejecting executable lifecycle
/// hooks and every other metadata extension not defined by schema v1.
fn validate_metadata_path(path: &Path) -> Result<(), ArchiveError> {
    const ALLOWED: &[&str] = &[
        ".METADATA/manifest.toml",
        ".METADATA/files.idx",
        ".METADATA/service.toml",
        ".METADATA/triggers.toml",
        ".METADATA/alternatives.toml",
        ".METADATA/sysusers.toml",
    ];
    if ALLOWED.iter().any(|allowed| path == Path::new(allowed)) {
        Ok(())
    } else {
        Err(ArchiveError::InvalidMetadata(format!(
            "unsupported metadata entry {}; lifecycle scripts are not permitted",
            path.display()
        )))
    }
}

fn collect_paths(root: &Path) -> Result<Vec<PathBuf>, ArchiveError> {
    fn visit(dir: &Path, paths: &mut Vec<PathBuf>) -> io::Result<()> {
        let mut entries: Vec<_> = fs::read_dir(dir)?.collect::<Result<_, _>>()?;
        entries.sort_by_key(|entry| entry.file_name());
        for entry in entries {
            let path = entry.path();
            paths.push(path.clone());
            if entry.file_type()?.is_dir() {
                visit(&path, paths)?;
            }
        }
        Ok(())
    }
    let mut paths = Vec::new();
    visit(root, &mut paths)?;
    Ok(paths)
}

fn append_deterministic<W: Write>(
    builder: &mut tar::Builder<W>,
    root: &Path,
    path: &Path,
) -> Result<(), ArchiveError> {
    let relative = path
        .strip_prefix(root)
        .expect("collected path stays below root");
    let metadata = fs::symlink_metadata(path)?;
    let mut header = tar::Header::new_gnu();
    header.set_uid(0);
    header.set_gid(0);
    header.set_mtime(0);
    header.set_mode(metadata.mode() & 0o7777);
    if metadata.is_dir() {
        header.set_entry_type(tar::EntryType::Directory);
        header.set_size(0);
        header.set_cksum();
        builder.append_data(&mut header, relative, io::empty())?;
    } else if metadata.is_file() {
        header.set_entry_type(tar::EntryType::Regular);
        header.set_size(metadata.len());
        header.set_cksum();
        builder.append_data(&mut header, relative, File::open(path)?)?;
    } else if metadata.file_type().is_symlink() {
        header.set_entry_type(tar::EntryType::Symlink);
        header.set_size(0);
        header.set_link_name(fs::read_link(path)?)?;
        header.set_cksum();
        builder.append_data(&mut header, relative, io::empty())?;
    } else {
        return Err(ArchiveError::UnsafePath(relative.display().to_string()));
    }
    Ok(())
}

/// Extracts verified regular payload files through dirfd-relative operations.
///
/// Intermediate directories are opened with `O_NOFOLLOW`; writes land in an
/// exclusive temporary file and become visible through an atomic `renameat`.
pub fn extract_package(
    package: impl AsRef<Path>,
    sysroot: impl AsRef<Path>,
    index: &[FileRecord],
) -> Result<Vec<PathBuf>, ArchiveError> {
    Ok(extract_package_with_config(package, sysroot, index, &BTreeMap::new())?.written)
}

/// Result of extraction including preserved and review-required configuration files.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct ExtractionReport {
    pub written: Vec<PathBuf>,
    pub preserved: Vec<PathBuf>,
    pub sage_new: Vec<PathBuf>,
}

/// Extracts a package and applies three-way hashes to files below `etc/`.
pub fn extract_package_with_config(
    package: impl AsRef<Path>,
    sysroot: impl AsRef<Path>,
    index: &[FileRecord],
    previous_hashes: &BTreeMap<String, String>,
) -> Result<ExtractionReport, ArchiveError> {
    let expected: BTreeMap<_, _> = index
        .iter()
        .map(|record| (record.path.clone(), record))
        .collect();
    let root_raw = open(
        sysroot.as_ref(),
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

fn write_verified_symlink(
    root: &OwnedFd,
    path: &Path,
    record: &FileRecord,
    target: &Path,
) -> Result<(), ArchiveError> {
    validate_link_target(path, target)?;
    let bytes = target.as_os_str().as_encoded_bytes();
    let actual = hex::encode(Sha256::digest(bytes));
    if bytes.len() as u64 != record.size || actual != record.sha256 {
        return Err(ArchiveError::ChecksumMismatch {
            path: path.into(),
            expected: record.sha256.clone(),
            actual,
        });
    }
    let directory = ensure_directory(root, path.parent().unwrap_or(Path::new("")))?;
    let name = path
        .file_name()
        .ok_or_else(|| ArchiveError::UnsafePath(path.display().to_string()))?;
    let temporary = format!(
        ".sage-tmp-{}-{}",
        std::process::id(),
        TEMP_ID.fetch_add(1, Ordering::Relaxed)
    );
    nix::unistd::symlinkat(target, Some(directory.as_raw_fd()), temporary.as_str())?;
    let mut guard = TempGuard {
        dirfd: directory.as_raw_fd(),
        name: temporary.clone(),
        active: true,
    };
    renameat(
        Some(directory.as_raw_fd()),
        temporary.as_str(),
        Some(directory.as_raw_fd()),
        name,
    )?;
    guard.active = false;
    Ok(())
}

fn validate_link_target(link: &Path, target: &Path) -> Result<(), ArchiveError> {
    if target.is_absolute() {
        return Err(ArchiveError::UnsafePath(target.display().to_string()));
    }
    let mut depth = link
        .parent()
        .map_or(0, |parent| parent.components().count());
    for component in target.components() {
        match component {
            Component::CurDir => {}
            Component::Normal(_) => depth += 1,
            Component::ParentDir if depth > 0 => depth -= 1,
            _ => return Err(ArchiveError::UnsafePath(target.display().to_string())),
        }
    }
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
    if let (Some(previous), Some(live)) = (previous_hash, live_hash.as_deref()) {
        if live != previous && record.sha256 == previous {
            verify_reader(path, record, reader)?;
            return Ok(WriteOutcome::Preserved);
        }
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

/// Converts the portable `u32` mode stored in `files.idx` into the host
/// `mode_t`. Darwin defines `mode_t` as `u16` while Linux uses `u32`; the
/// archive schema intentionally stores the wider representation so packages
/// remain architecture independent. `Mode` discards bits it does not support,
/// so narrowing here preserves every portable permission and special bit.
fn archive_mode(mode: u32) -> Mode {
    Mode::from_bits_truncate(mode as nix::libc::mode_t)
}

fn hash_at(directory: &OwnedFd, name: &std::ffi::OsStr) -> Result<Option<String>, ArchiveError> {
    let raw = match openat(
        Some(directory.as_raw_fd()),
        name,
        OFlag::O_RDONLY | OFlag::O_CLOEXEC | OFlag::O_NOFOLLOW,
        Mode::empty(),
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

struct HashOnly<'a>(&'a mut Sha256);

impl Write for HashOnly<'_> {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        self.0.update(bytes);
        Ok(bytes.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

struct TempGuard {
    dirfd: i32,
    name: String,
    active: bool,
}

impl Drop for TempGuard {
    fn drop(&mut self) {
        if self.active {
            let _ = nix::unistd::unlinkat(
                Some(self.dirfd),
                self.name.as_str(),
                nix::unistd::UnlinkatFlags::NoRemoveDir,
            );
        }
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

fn ensure_directory(root: &OwnedFd, path: &Path) -> Result<OwnedFd, ArchiveError> {
    let duplicate = openat(
        Some(root.as_raw_fd()),
        ".",
        OFlag::O_RDONLY | OFlag::O_DIRECTORY | OFlag::O_CLOEXEC,
        Mode::empty(),
    )?;
    // SAFETY: the newly duplicated descriptor has one owner.
    let mut current = unsafe { OwnedFd::from_raw_fd(duplicate) };
    for component in path.components() {
        let Component::Normal(name) = component else {
            return Err(ArchiveError::UnsafePath(path.display().to_string()));
        };
        match mkdirat(
            Some(current.as_raw_fd()),
            name,
            Mode::from_bits_truncate(0o755),
        ) {
            Ok(()) | Err(Errno::EEXIST) => {}
            Err(error) => return Err(error.into()),
        }
        let next = openat(
            Some(current.as_raw_fd()),
            name,
            OFlag::O_RDONLY | OFlag::O_DIRECTORY | OFlag::O_CLOEXEC | OFlag::O_NOFOLLOW,
            Mode::empty(),
        )?;
        // SAFETY: `openat` returned a new descriptor replacing the previous guard.
        current = unsafe { OwnedFd::from_raw_fd(next) };
    }
    Ok(current)
}

fn clean_archive_path(path: &Path) -> Result<PathBuf, ArchiveError> {
    let mut clean = PathBuf::new();
    for component in path.components() {
        match component {
            Component::CurDir => {}
            Component::Normal(value) => clean.push(value),
            _ => return Err(ArchiveError::UnsafePath(path.display().to_string())),
        }
    }
    if clean.as_os_str().is_empty() {
        return Err(ArchiveError::UnsafePath(path.display().to_string()));
    }
    Ok(clean)
}
