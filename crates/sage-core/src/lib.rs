//! Core domain models, version algebra, and locking primitives for Sage.
//!
//! This crate provides foundational structures including [`PackageKey`], [`Version`],
//! [`Dependency`], [`HostLock`], and error types used across all Sage crates.

use serde::{Deserialize, Serialize};
use std::fmt;
use std::fs::File;
use std::path::{Path, PathBuf};
use thiserror::Error;

/// Error types occurring within core domain operations.
#[derive(Debug, Error)]
pub enum CoreError {
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),

    #[error("Failed to acquire host lock at {path}: {message}")]
    LockFailed { path: PathBuf, message: String },

    #[error("Invalid version string '{0}'")]
    InvalidVersion(String),

    #[error("Invalid package key string '{0}'")]
    InvalidPackageKey(String),
}

/// Unique package instance identifier defined as `(Channel, Name, Slot)`.
///
/// This composite key natively supports versioned sub-channels (e.g. `main/python3.12`)
/// and parallel multi-version slots (e.g. `gcc` with slot `14` and slot `15`).
#[derive(Debug, Clone, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
pub struct PackageKey {
    /// Versioned channel identifier (e.g. "main/system", "main/python3.12").
    pub channel: String,
    /// Package name (e.g. "numpy", "gcc", "ripgrep").
    pub name: String,
    /// Slot identifier for multi-version co-existence (defaults to "0").
    pub slot: String,
}

impl PackageKey {
    /// Creates a new `PackageKey` instance.
    pub fn new(
        channel: impl Into<String>,
        name: impl Into<String>,
        slot: impl Into<String>,
    ) -> Self {
        Self {
            channel: channel.into(),
            name: name.into(),
            slot: slot.into(),
        }
    }

    /// Formats the key into the canonical `channel:name:slot` string.
    pub fn canonical_id(&self) -> String {
        format!("{}:{}:{}", self.channel, self.name, self.slot)
    }
}

impl fmt::Display for PackageKey {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}:{}:{}", self.channel, self.name, self.slot)
    }
}

/// Package version representation adhering to `(Epoch, Upstream, Release)`.
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct Version {
    /// Epoch counter for monotonic version bumps overriding upstream version schemes.
    pub epoch: u32,
    /// Upstream version string (e.g. "1.2.3").
    pub upstream: String,
    /// Packaging release revision number.
    pub release: u32,
}

impl Version {
    /// Creates a new `Version` instance.
    pub fn new(epoch: u32, upstream: impl Into<String>, release: u32) -> Self {
        Self {
            epoch,
            upstream: upstream.into(),
            release,
        }
    }
}

impl fmt::Display for Version {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.epoch > 0 {
            write!(f, "{}:{}-{}", self.epoch, self.upstream, self.release)
        } else {
            write!(f, "{}-{}", self.upstream, self.release)
        }
    }
}

/// Version comparison operator for dependency constraints.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum ConstraintOp {
    Any,
    Equal,
    NotEqual,
    Greater,
    GreaterOrEqual,
    Less,
    LessOrEqual,
}

/// Dependency specification model with optional channel and slot constraints.
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct Dependency {
    /// Target package or virtual interface name.
    pub name: String,
    /// Optional slot constraint.
    pub slot: Option<String>,
    /// Optional explicit channel scope constraint.
    pub channel: Option<String>,
    /// Version constraint operator.
    pub op: ConstraintOp,
    /// Target constraint version.
    pub version: Option<Version>,
}

/// Host-level mutual exclusion lock for safe concurrent operations.
pub struct HostLock {
    _file: File,
    path: PathBuf,
}

impl HostLock {
    /// Acquires a shared lock on the specified path for read-only operations.
    pub fn acquire_shared(path: impl AsRef<Path>) -> Result<Self, CoreError> {
        let path = path.as_ref().to_path_buf();
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let file = File::create(&path)?;
        fs2::FileExt::lock_shared(&file).map_err(|e| CoreError::LockFailed {
            path: path.clone(),
            message: e.to_string(),
        })?;
        Ok(Self { _file: file, path })
    }

    /// Acquires an exclusive lock on the specified path for mutating operations.
    pub fn acquire_exclusive(path: impl AsRef<Path>) -> Result<Self, CoreError> {
        let path = path.as_ref().to_path_buf();
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let file = File::create(&path)?;
        fs2::FileExt::lock_exclusive(&file).map_err(|e| CoreError::LockFailed {
            path: path.clone(),
            message: e.to_string(),
        })?;
        Ok(Self { _file: file, path })
    }

    /// Returns the path to the underlying lock file.
    pub fn path(&self) -> &Path {
        &self.path
    }
}

impl Drop for HostLock {
    fn drop(&mut self) {
        let _ = fs2::FileExt::unlock(&self._file);
    }
}
