mod core_tests {
    use sage_core::*;
    use std::cmp::Ordering;

    #[test]
    fn versions_parse_round_trip_and_sort() {
        let parsed: Vec<Version> = ["1.9-3", "1.10-1", "2:0.1-1"]
            .iter()
            .map(|value| value.parse().unwrap())
            .collect();
        assert!(parsed[0] < parsed[1] && parsed[1] < parsed[2]);
        assert_eq!(parsed[2].to_string(), "2:0.1-1");
        assert_ne!(
            "1.01-1"
                .parse::<Version>()
                .unwrap()
                .cmp(&"1.1-1".parse().unwrap()),
            Ordering::Equal
        );
    }

    #[test]
    fn package_key_defaults_slot() {
        assert_eq!(
            "main/system:ripgrep".parse::<PackageKey>().unwrap().slot,
            "0"
        );
        assert!("broken".parse::<PackageKey>().is_err());
    }

    #[test]
    fn dependencies_parse_and_match() {
        let dep: Dependency = "main/python3.13/requests:0 >= 2.32-1".parse().unwrap();
        assert_eq!(dep.channel.as_deref(), Some("main/python3.13"));
        assert_eq!(dep.slot.as_deref(), Some("0"));
        assert!(dep
            .op
            .matches(&"2.33-1".parse().unwrap(), dep.version.as_ref()));
        let virtual_dep: Dependency = "virtual/libc".parse().unwrap();
        assert_eq!(virtual_dep.name, "virtual/libc");
        assert!(virtual_dep.channel.is_none());
        let soname: Dependency = "so:libc.so.6".parse().unwrap();
        assert_eq!(soname.name, "so:libc.so.6");
        assert!(soname.slot.is_none());
    }

    #[test]
    fn spdx_expressions_are_strictly_validated() {
        for expression in [
            "Apache-2.0 OR MIT",
            "GPL-2.0-or-later WITH Classpath-exception-2.0",
            "LicenseRef-Public-Domain",
        ] {
            validate_spdx_expression(expression).unwrap();
        }
        assert!(validate_spdx_expression("").is_err());
        assert!(validate_spdx_expression("public-domain").is_err());
    }

    #[test]
    fn package_coordinate_unifies_identity_and_version() {
        let coordinate = PackageCoordinate::new(
            PackageKey::new("main/system", "sage", "0"),
            Version::new(1, "0.4", 2),
        );
        assert_eq!(coordinate.to_string(), "main/system:sage:0@1:0.4-2");
    }

    #[test]
    fn interning_reuses_stable_ids() {
        let mut symbols = SymbolTable::default();
        let first = symbols.intern("system").unwrap();
        assert_eq!(first, symbols.intern("system").unwrap());
        assert_eq!(symbols.resolve(first), Some("system"));
        assert_eq!(symbols.len(), 1);
    }
}
