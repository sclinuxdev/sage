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
    valid_channel_name, valid_package_component, valid_provider_symbol, valid_version_string,
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

    #[test]
    fn package_validation_checks_provides_conflicts_and_dependencies() {
        let mut pkg = Package {
            schema_version: 1,
            name: "testpkg".into(),
            slot: "0".into(),
            version: "1.0.0".into(),
            release: 1,
            epoch: 0,
            arch: "x86_64".into(),
            channel: "system".into(),
            description: "desc".into(),
            license: "MIT".into(),
            dependencies: vec![],
            provides: vec![],
            conflicts: vec![],
            features: vec![],
            installed_size: 0,
            build_time: 0,
            managed_build_tools: vec![],
        };
        assert!(pkg.validate().is_ok());

        // Invalid provide
        pkg.provides.push("bad/provide/nested".into());
        assert!(pkg.validate().is_err());
        pkg.provides.clear();

        // Valid provide
        pkg.provides.push("virtual/init".into());
        pkg.provides.push("so:libc.so.6".into());
        assert!(pkg.validate().is_ok());

        // Invalid conflict
        pkg.conflicts.push("bad >= not-a-version".into());
        assert!(pkg.validate().is_err());
        pkg.conflicts.clear();

        // Valid conflict
        pkg.conflicts.push("other:1 >= 2.0-1".into());
        assert!(pkg.validate().is_ok());
    }
}
