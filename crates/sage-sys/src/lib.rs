//! Declarative triggers, init-template rendering, profiles, and reconciliation models.

use std::sync::atomic::AtomicU64;
use thiserror::Error;

mod channel;
mod query;
mod recovery;
mod services;
mod state;
mod transaction;
mod triggers;

// 1. Transaction execution entry points
pub use transaction::{
    TransactionPlan, apply_packages, rebuild_system, remove_packages, upgrade_packages,
};

// 2. Recovery
pub use recovery::settle_journals;

// 3. State query and environment activation
pub use query::{
    QueryAction, list_channels, query_info, query_installed, query_owner, query_state,
    use_toolchain,
};

// 4. Repository channels and availability pools
pub use channel::{
    AvailablePackages, canonical_channel, load_available_with_pool, obtain_release_archive,
    sync_channels,
};

// 5. Metadata and configuration specifications
pub use services::{
    RenderedServicesState, ServiceDocument, ServiceDrift, ServiceSpec, ServiceStatusInfo,
    ServicesConfig, TemplateServiceGenerator, detect_service_drift, list_services,
    load_active_generator, load_available_services, service_adopt, service_disable, service_enable,
    warn_service_drift,
};
pub use state::{
    Alternative, AlternativeDeclaration, AlternativesDocument, ProfileEngine, ReconcilePlan,
    SystemConfig, SystemMetadata, SysuserDeclaration, SysusersDocument, SysusersEngine,
    provider_symbol,
};
pub use triggers::{TriggerEngine, TriggerEvent, TriggerSpec};

pub(crate) static TEMP_ID: AtomicU64 = AtomicU64::new(0);

/// System orchestration failures.
#[derive(Debug, Error)]
pub enum SysError {
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
    #[error("database error: {0}")]
    Database(#[from] sage_db::DbError),
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
