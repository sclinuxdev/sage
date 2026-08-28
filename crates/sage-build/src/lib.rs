//! Hermetic build descriptions, execution, and package payload preparation.

use std::path::PathBuf;
use std::process::ExitStatus;
use thiserror::Error;

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

pub(crate) fn validate_schema(version: u32) -> Result<(), BuildError> {
    if version == sage_core::SCHEMA_VERSION {
        Ok(())
    } else {
        Err(BuildError::Schema(version))
    }
}

mod execution;
mod payload;
mod recipe;
mod sources;

pub use execution::*;
pub use payload::*;
pub use recipe::*;
pub use sources::*;
