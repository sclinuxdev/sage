//! LMDB-based persistent state storage, file ownership tracking, and transaction journaling.
//!
//! This crate utilizes `heed` (LMDB Rust binding) to deliver zero-copy, mmap-backed access
//! to local installed state, file ownership tables, and crash recovery records.

use heed::types::Str;
use heed::{Database, Env, EnvOpenOptions};
use sage_core::PackageKey;
use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};
use thiserror::Error;

/// Database error variants.
#[derive(Debug, Error)]
pub enum DbError {
    #[error("LMDB error: {0}")]
    Heed(#[from] heed::Error),

    #[error("Serialization error: {0}")]
    Serialization(#[from] bincode::Error),

    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
}

/// Record representing an in-flight operation for crash recovery.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct JournalRecord {
    pub op_id: String,
    pub stage: String,
    pub affected_packages: Vec<PackageKey>,
    pub timestamp: u64,
}

/// Primary database container encapsulating named tables (DBIs).
pub struct SageDatabase {
    pub env: Env,
    pub db_path: PathBuf,
}

impl SageDatabase {
    /// Opens or creates the primary state database at the specified directory.
    pub fn open(dir_path: impl AsRef<Path>) -> Result<Self, DbError> {
        let dir_path = dir_path.as_ref().to_path_buf();
        fs::create_dir_all(&dir_path)?;

        let env = unsafe {
            EnvOpenOptions::new()
                .map_size(10 * 1024 * 1024 * 1024) // 10 GB virtual map size
                .max_dbs(16)
                .open(&dir_path)?
        };

        Ok(Self {
            env,
            db_path: dir_path,
        })
    }

    /// Opens the packages table.
    pub fn open_packages_table(&self) -> Result<Database<Str, Str>, DbError> {
        let mut wtxn = self.env.write_txn()?;
        let db = self.env.create_database(&mut wtxn, Some("packages"))?;
        wtxn.commit()?;
        Ok(db)
    }

    /// Opens the files ownership table.
    pub fn open_files_table(&self) -> Result<Database<Str, Str>, DbError> {
        let mut wtxn = self.env.write_txn()?;
        let db = self.env.create_database(&mut wtxn, Some("files"))?;
        wtxn.commit()?;
        Ok(db)
    }

    /// Opens the operations journal table.
    pub fn open_operations_table(&self) -> Result<Database<Str, Str>, DbError> {
        let mut wtxn = self.env.write_txn()?;
        let db = self.env.create_database(&mut wtxn, Some("operations"))?;
        wtxn.commit()?;
        Ok(db)
    }
}
