//! Core domain models, version algebra, symbol interning, and host locking.

pub mod error;
pub mod glob;
pub mod hex;
pub mod lock;
pub mod mmap;
pub mod package;
pub mod symbol;
pub mod version;
pub mod walkdir;

pub use error::{
    CoreError, DEFAULT_SLOT, SCHEMA_VERSION, validate_schema, validate_spdx_expression,
};
pub use lock::{HostLock, under_root};
pub use mmap::Mmap;
pub use package::{
    ConstraintOp, Dependency, ManagedBuildTool, Package, PackageCoordinate, PackageKey,
    valid_package_component,
};
pub use symbol::{SymbolId, SymbolTable};
pub use version::Version;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn version_comparison_orders_components() {
        let v1: Version = "1.0.0-1".parse().unwrap();
        let v2: Version = "1.0.1-1".parse().unwrap();
        let v3: Version = "1:1.0.0-1".parse().unwrap();
        assert!(v1 < v2);
        assert!(v2 < v3);
    }

    #[test]
    fn package_key_parsing() {
        let key = PackageKey::in_channel("system", "gcc:14").unwrap();
        assert_eq!(key.channel, "system");
        assert_eq!(key.name, "gcc");
        assert_eq!(key.slot, "14");
    }

    #[test]
    fn dependency_parsing_and_matching() {
        let dep: Dependency = "system/glibc:0 >= 2.38-1".parse().unwrap();
        assert_eq!(dep.name, "glibc");
        assert_eq!(dep.channel.as_deref(), Some("system"));
        assert_eq!(dep.slot.as_deref(), Some("0"));
        let candidate: Version = "2.41-1".parse().unwrap();
        assert!(dep.op.matches(&candidate, dep.version.as_ref()));
    }
}
