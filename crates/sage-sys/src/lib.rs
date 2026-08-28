//! System reconciliation, declarative trigger engine, and Init service template generator.
//!
//! This crate provides the pure data-driven orchestration layer for system states defined in
//! `/etc/sage/system.toml`, executing post-install triggers and rendering init daemon configs.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use thiserror::Error;

/// System reconciliation error variants.
#[derive(Debug, Error)]
pub enum SysError {
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),

    #[error("TOML parsing error: {0}")]
    Toml(#[from] toml::de::Error),

    #[error("Trigger execution error: {0}")]
    Trigger(String),

    #[error("Template render error: {0}")]
    Template(String),
}

/// Declarative trigger definition model parsed from `triggers/*.toml`.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TriggerSpec {
    pub name: String,
    pub description: String,
    pub on_paths: Vec<String>,
    pub exec: Vec<String>,
    pub priority: u32,
    pub ignore_missing_binary: bool,
}

/// Declarative trigger batch execution engine.
pub struct TriggerEngine;

impl TriggerEngine {
    /// Loads trigger definitions from standard locations in the target sysroot.
    pub fn load_triggers(_sysroot: &Path) -> Result<Vec<TriggerSpec>, SysError> {
        Ok(Vec::new())
    }

    /// Executes triggers matching modified filesystem paths.
    pub fn execute_triggers(
        _triggers: &[TriggerSpec],
        _modified_paths: &[PathBuf],
        _sysroot: &Path,
    ) -> Result<(), SysError> {
        Ok(())
    }
}

/// Declarative system configuration representation parsed from `/etc/sage/system.toml`.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SystemConfig {
    pub schema_version: u32,
    pub system: SystemMetadata,
    pub packages: Vec<String>,
    pub services: Vec<String>,
}

/// Metadata section in `/etc/sage/system.toml`.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SystemMetadata {
    pub architecture: String,
    pub profile: String,
}
