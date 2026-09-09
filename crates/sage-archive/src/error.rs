use nix::errno::Errno;
use std::io;
use std::path::PathBuf;
use thiserror::Error;

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
