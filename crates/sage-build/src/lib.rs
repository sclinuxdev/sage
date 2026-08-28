//! Declarative rclass execution, Bubblewrap isolation, payload carving, and ELF scans.

use serde::Deserialize;
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitStatus};
use thiserror::Error;

const PHASE_ORDER: &[&str] = &[
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
    Glob(#[from] glob::PatternError),
    #[error("filesystem traversal failed: {0}")]
    Walk(#[from] walkdir::Error),
    #[error("ELF parse failed: {0}")]
    Elf(#[from] goblin::error::Error),
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

include!("recipe.rs");
include!("execution.rs");
include!("payload.rs");

#[cfg(test)]
#[path = "../tests_internal/build.rs"]
mod tests;
