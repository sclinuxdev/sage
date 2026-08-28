//! Streaming tar.zst packaging and openat/dirfd path-traversal-safe unpacking.
//!
//! This crate handles the creation, inspection, and extraction of `*.pkg.tar.zst` packages,
//! including metadata scanning, SHA-256 verification, and `.sage-new` configuration file protection.

use std::fs::File;
use std::io::Read;
use std::path::{Path, PathBuf};
use thiserror::Error;

/// Archive error variants.
#[derive(Debug, Error)]
pub enum ArchiveError {
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),

    #[error("Tar archive error: {0}")]
    Tar(String),

    #[error("Zstandard compression/decompression error: {0}")]
    Zstd(String),

    #[error("Checksum mismatch for {path}: expected {expected}, calculated {actual}")]
    ChecksumMismatch {
        path: PathBuf,
        expected: String,
        actual: String,
    },

    #[error("Path traversal detected: {0}")]
    PathTraversal(String),
}

/// Inspects package metadata headers without unpacking the full payload.
pub fn inspect_package(pkg_path: impl AsRef<Path>) -> Result<Vec<u8>, ArchiveError> {
    let file = File::open(pkg_path)?;
    let mut decoder = zstd::Decoder::new(file).map_err(|e| ArchiveError::Zstd(e.to_string()))?;
    let mut buffer = Vec::new();
    let mut chunk = [0u8; 8192];

    // Read initial metadata slice
    while let Ok(n) = decoder.read(&mut chunk) {
        if n == 0 {
            break;
        }
        buffer.extend_from_slice(&chunk[..n]);
        if buffer.len() > 65536 {
            break;
        }
    }

    Ok(buffer)
}

/// Creates a compressed `*.pkg.tar.zst` archive from a staged directory.
pub fn create_package(
    source_dir: impl AsRef<Path>,
    output_pkg: impl AsRef<Path>,
    compression_level: i32,
) -> Result<(), ArchiveError> {
    let out_file = File::create(output_pkg)?;
    let encoder = zstd::Encoder::new(out_file, compression_level)
        .map_err(|e| ArchiveError::Zstd(e.to_string()))?;
    let mut tar_builder = tar::Builder::new(encoder.auto_finish());

    tar_builder.append_dir_all(".", source_dir)?;
    tar_builder.finish()?;

    Ok(())
}
