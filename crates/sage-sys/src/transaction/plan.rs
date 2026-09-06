//! Declarative execution plan for atomic package transactions.

use std::collections::BTreeMap;

/// Explicit, atomic execution plan for package mutations before journal creation and execution.
///
/// Encapsulates ordered installations/upgrades, retired package removals, and system
/// provider bindings determined during dependency resolution and preflight verification.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TransactionPlan {
    /// Ordered packages to install or upgrade: (PackageKey, Version).
    pub install: Vec<(sage_core::PackageKey, sage_core::Version)>,
    /// Packages to be removed or retired.
    pub remove: Vec<sage_db::InstalledPackage>,
    /// System provider bindings to commit on completion (e.g. init provider).
    pub provider_bindings: BTreeMap<String, sage_core::PackageKey>,
}

impl TransactionPlan {
    /// Creates a complete transaction plan with install, remove, and provider binding actions.
    pub fn new(
        install: Vec<(sage_core::PackageKey, sage_core::Version)>,
        remove: Vec<sage_db::InstalledPackage>,
        provider_bindings: BTreeMap<String, sage_core::PackageKey>,
    ) -> Self {
        Self {
            install,
            remove,
            provider_bindings,
        }
    }

    /// Creates an install-only transaction plan.
    pub fn for_install(install: Vec<(sage_core::PackageKey, sage_core::Version)>) -> Self {
        Self {
            install,
            remove: Vec::new(),
            provider_bindings: BTreeMap::new(),
        }
    }

    /// Creates a removal-only transaction plan.
    pub fn for_remove(remove: Vec<sage_db::InstalledPackage>) -> Self {
        Self {
            install: Vec::new(),
            remove,
            provider_bindings: BTreeMap::new(),
        }
    }

    /// Returns true if the plan contains no install, remove, or binding mutations.
    pub fn is_empty(&self) -> bool {
        self.install.is_empty() && self.remove.is_empty() && self.provider_bindings.is_empty()
    }
}
