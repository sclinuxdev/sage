use std::path::PathBuf;
use thiserror::Error;

/// Schema version understood by the Sage 0.4 metadata readers.
pub const SCHEMA_VERSION: u32 = 1;

/// Slot selected when metadata omits one.
pub const DEFAULT_SLOT: &str = "0";

/// Failures shared by core domain operations.
#[derive(Debug, Error)]
pub enum CoreError {
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
    #[error("failed to acquire host lock at {path}: {source}")]
    LockFailed {
        path: PathBuf,
        source: std::io::Error,
    },
    #[error("invalid version string '{0}'")]
    InvalidVersion(String),
    #[error("invalid package key string '{0}'")]
    InvalidPackageKey(String),
    #[error("invalid dependency string '{0}'")]
    InvalidDependency(String),
    #[error("invalid metadata: {0}")]
    InvalidMetadata(String),
    #[error("unsupported schema version {found}; expected {SCHEMA_VERSION}")]
    UnsupportedSchema { found: u32 },
    #[error("symbol table exhausted its 32-bit address space")]
    SymbolTableFull,
}

/// Validates a strict SPDX license expression against the license-list version
/// embedded in the `spdx` crate.
pub fn validate_spdx_expression(expression: &str) -> Result<(), CoreError> {
    if expression.is_empty() {
        return Err(CoreError::InvalidMetadata(
            "SPDX license expression is required".into(),
        ));
    }
    spdx::Expression::parse(expression)
        .map(|_| ())
        .map_err(|error| {
            CoreError::InvalidMetadata(format!("invalid SPDX license expression: {error}"))
        })
}

/// Rejects metadata that does not use the current exact schema.
pub fn validate_schema(found: u32) -> Result<(), CoreError> {
    (found == SCHEMA_VERSION)
        .then_some(())
        .ok_or(CoreError::UnsupportedSchema { found })
}
