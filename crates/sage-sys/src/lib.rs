//! Declarative triggers, init-template rendering, profiles, and reconciliation models.

use std::sync::atomic::AtomicU64;
use thiserror::Error;

pub mod channel;
pub mod query;
pub mod recovery;
pub mod services;
pub mod state;
pub mod transaction;
pub mod triggers;

pub use channel::*;
pub use query::*;
pub use recovery::*;
pub use services::*;
pub use state::*;
pub use transaction::*;
pub use triggers::*;

pub(crate) static TEMP_ID: AtomicU64 = AtomicU64::new(0);

/// System orchestration failures.
#[derive(Debug, Error)]
pub enum SysError {
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
    #[error("TOML parsing error: {0}")]
    Toml(#[from] toml::de::Error),
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

pub(crate) fn validate_schema(version: u32) -> Result<(), SysError> {
    if version == sage_core::SCHEMA_VERSION {
        Ok(())
    } else {
        Err(SysError::Schema(version))
    }
}
