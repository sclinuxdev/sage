//! File index parsing, inspection metadata records, and formatting.

use crate::error::ArchiveError;
use crate::security::{clean_archive_path, validate_link_target, validate_metadata_path};
use sage_core::hex;
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};

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

/// Result of extraction including preserved and review-required configuration files.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct ExtractionReport {
    pub written: Vec<PathBuf>,
    pub preserved: Vec<PathBuf>,
    pub sage_new: Vec<PathBuf>,
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
    let records = text
        .lines()
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
        .collect::<Result<Vec<_>, _>>()?;
    let mut paths = BTreeSet::new();
    if records
        .iter()
        .any(|record| !paths.insert(record.path.clone()))
    {
        return Err(ArchiveError::InvalidMetadata(
            "files.idx contains duplicate canonical paths".into(),
        ));
    }
    Ok(records)
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
                let relative = path
                    .strip_prefix(root)
                    .expect("recursive walk stays below root");
                validate_link_target(relative, &target)?;
                let bytes = target.as_os_str().as_encoded_bytes();
                output.push(FileRecord {
                    path: relative.to_path_buf(),
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

pub(crate) struct HashOnly<'a>(pub &'a mut Sha256);

impl Write for HashOnly<'_> {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        self.0.update(bytes);
        Ok(bytes.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
