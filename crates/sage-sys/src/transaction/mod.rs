//! Transactional package deployment, system reconciliation, preflight validation, and service planning.

pub mod package;
pub mod plan;
pub mod preflight;
mod providers;
pub mod rebuild;

pub(crate) use package::{PackageDeclarations, package_ownership};
pub use package::{apply_packages, remove_packages, upgrade_packages};
pub use plan::TransactionPlan;
pub(crate) use preflight::write_atomic_under_root;
pub use providers::select_virtual_provider;
pub(crate) use rebuild::cleanup_services;
pub use rebuild::rebuild_system;
