//! Declarative rclass execution, Bubblewrap isolation, payload carving, and ELF scans.

use serde::Deserialize;
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitStatus};
use thiserror::Error;

pub(crate) const PHASE_ORDER: &[&str] = &[
    "src_unpack",
    "src_prepare",
    "src_configure",
    "src_compile",
    "src_test",
    "src_install",
];

/// Build description or execution failures.
#[derive(Debug, Error)]
pub enum BuildError {
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
    #[error("TOML error: {0}")]
    Toml(#[from] toml::de::Error),
    #[error("unsupported schema version {0}")]
    Schema(u32),
    #[error("unknown template variable '{0}'")]
    UnknownVariable(String),
    #[error("invalid template expression at byte {0}")]
    InvalidTemplate(usize),
    #[error("sandbox process exited with {0}")]
    SandboxFailed(ExitStatus),
    #[error("invalid build specification: {0}")]
    InvalidSpec(String),
    #[error("invalid glob pattern: {0}")]
    Glob(#[from] sage_core::glob::PatternError),
    #[error("filesystem traversal failed: {0}")]
    Walk(std::io::Error),
    #[error("tool '{tool}' is not allowed by inherited rclasses")]
    UnauthorizedTool { tool: String },
    #[error("patchelf failed for {path}: {message}")]
    Patchelf { path: PathBuf, message: String },
    #[error("git operation '{operation}' exited with {status}")]
    GitFailed {
        operation: String,
        status: ExitStatus,
    },
}

pub use anyhow::{Context, Result, bail};
pub use sage_core::{glob, walkdir};

pub mod cgroup;
pub mod execution;
pub mod operations;
pub mod payload;
pub mod recipe;
pub mod sources;

pub use cgroup::{CgroupScope, parse_memory_limit};
pub use execution::*;
pub use operations::*;
pub use payload::*;
pub use recipe::*;
pub use sources::*;

pub(crate) fn validate_schema(version: u32) -> Result<(), BuildError> {
    if version == sage_core::SCHEMA_VERSION {
        Ok(())
    } else {
        Err(BuildError::Schema(version))
    }
}
