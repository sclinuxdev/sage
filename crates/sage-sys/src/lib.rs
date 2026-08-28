//! Declarative triggers, init-template rendering, profiles, and reconciliation models.

use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Component, Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use thiserror::Error;

static TEMP_ID: AtomicU64 = AtomicU64::new(0);

/// System orchestration failures.
#[derive(Debug, Error)]
pub enum SysError {
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
    #[error("TOML parsing error: {0}")]
    Toml(#[from] toml::de::Error),
    #[error("JSON encoding error: {0}")]
    Json(#[from] serde_json::Error),
    #[error("unsupported schema version {0}")]
    Schema(u32),
    #[error("invalid declaration: {0}")]
    Invalid(String),
    #[error("trigger '{name}' exited with status {status}")]
    Trigger {
        name: String,
        status: std::process::ExitStatus,
    },
    #[error("template contains unknown variable '{0}'")]
    UnknownVariable(String),
    #[error("dependency solver failed: {0}")]
    Solver(#[from] sage_solver::SolverError),
}

fn validate_schema(version: u32) -> Result<(), SysError> {
    if version == sage_core::SCHEMA_VERSION {
        Ok(())
    } else {
        Err(SysError::Schema(version))
    }
}

include!("services.rs");
include!("state.rs");

#[cfg(test)]
#[path = "../tests_internal/sys.rs"]
mod tests;
