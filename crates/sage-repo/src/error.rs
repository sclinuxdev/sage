//! Error types for repository operations.

use thiserror::Error;

/// Repository transfer, verification, and index failures.
#[derive(Debug, Error)]
pub enum RepoError {
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
    #[error("network error: {0}")]
    Network(#[from] reqwest::Error),
    #[error("LMDB error: {0}")]
    Heed(#[from] heed::Error),
    #[error("invalid repository configuration: {0}")]
    InvalidConfig(String),
    #[error("signature verification failed")]
    Signature,
    #[error("SHA-256 mismatch: expected {expected}, calculated {actual}")]
    Checksum { expected: String, actual: String },
    #[error("all mirrors failed: {0}")]
    Mirrors(String),
    #[error("download task failed: {0}")]
    Join(#[from] tokio::task::JoinError),
    #[error("index serialization failed: {0}")]
    Serialization(#[from] bincode::Error),
    #[error(
        "index replay or downgrade attack detected: current index timestamp {current}, incoming timestamp {incoming}"
    )]
    ReplayAttack { current: u64, incoming: u64 },
}
