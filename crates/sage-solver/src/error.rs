use thiserror::Error;

/// Failures returned with a human-readable PubGrub causality report.
#[derive(Debug, Error)]
pub enum SolverError {
    #[error("dependency resolution failed:\n{0}")]
    NoSolution(String),
    #[error("invalid package metadata: {0}")]
    InvalidMetadata(String),
    #[error("internal solver error: {0}")]
    Internal(String),
}
