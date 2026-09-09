use sage_core::{CoreError, PackageKey};
use thiserror::Error;

/// Persistent-state failures.
#[derive(Debug, Error)]
pub enum DbError {
    #[error("LMDB error: {0}")]
    Heed(#[from] heed::Error),
    #[error("serialization error: {0}")]
    Serialization(#[from] bincode::Error),
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
    #[error("invalid core value: {0}")]
    Core(#[from] CoreError),
    #[error("path '{path}' is already owned by {owners:?}")]
    FileConflict {
        path: String,
        owners: Vec<PackageKey>,
    },
    #[error("operation journal '{0}' failed its integrity check")]
    InvalidJournal(String),
}
