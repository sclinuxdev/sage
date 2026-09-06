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

    #[test]
    fn hex_encode_and_decode_round_trip() {
        let raw = b"Sage Linux Declarative Packaging \x00\xff\x7f\x80";
        let encoded = hex::encode(raw);
        let decoded = hex::decode(&encoded).unwrap();
        assert_eq!(decoded, raw);

        // Case insensitivity in decode
        assert_eq!(hex::decode("4142").unwrap(), b"AB");
        assert_eq!(hex::decode("4142").unwrap(), hex::decode("4142").unwrap());

        // Error cases
        assert!(hex::decode("abc").is_err()); // Odd length
        assert!(hex::decode("zz").is_err()); // Invalid character
    }

    #[test]
    fn mmap_reads_file_contents_and_handles_empty_files() {
        let dir = tempfile::tempdir().unwrap();
        let file_path = dir.path().join("test.bin");
        let content = b"Hello Sage Mmap zero-copy payload!";
        std::fs::write(&file_path, content).unwrap();

        let file = std::fs::File::open(&file_path).unwrap();
        let mmap = unsafe { Mmap::map(&file).unwrap() };
        assert_eq!(&*mmap, content);
        assert_eq!(mmap.len(), content.len());

        // Empty file handling
        let empty_path = dir.path().join("empty.bin");
        std::fs::write(&empty_path, b"").unwrap();
        let empty_file = std::fs::File::open(&empty_path).unwrap();
        let empty_mmap = unsafe { Mmap::map(&empty_file).unwrap() };
        assert_eq!(&*empty_mmap, b"");
        assert_eq!(empty_mmap.len(), 0);
    }

    #[test]
    fn walkdir_traverses_directory_tree() {
        let dir = tempfile::tempdir().unwrap();
        let sub = dir.path().join("sub");
        std::fs::create_dir(&sub).unwrap();
        std::fs::write(sub.join("file1.txt"), b"1").unwrap();
        std::fs::write(sub.join("file2.txt"), b"2").unwrap();
        std::fs::write(dir.path().join("root.txt"), b"root").unwrap();

        let mut names: Vec<String> = walkdir::WalkDir::new(dir.path())
            .into_iter()
            .map(|entry| {
                entry
                    .unwrap()
                    .file_name()
                    .to_string_lossy()
                    .into_owned()
            })
            .collect();
        names.sort();
        assert!(names.contains(&"file1.txt".to_string()));
        assert!(names.contains(&"file2.txt".to_string()));
        assert!(names.contains(&"root.txt".to_string()));
    }

    #[test]
    fn glob_pattern_matching_semantics() {
        let pat = glob::Pattern::new("usr/lib*/**/*.so").unwrap();
        assert!(pat.matches_path(std::path::Path::new("usr/lib/libfoo.so")));
        assert!(pat.matches_path(std::path::Path::new("usr/lib64/sub/libbar.so")));
        assert!(!pat.matches_path(std::path::Path::new("usr/bin/libfoo.so")));
        assert!(!pat.matches_path(std::path::Path::new("usr/lib/libfoo.a")));

        let icon_pat = glob::Pattern::new("usr/share/icons/*/**").unwrap();
        assert!(icon_pat.matches_path(std::path::Path::new("usr/share/icons/hicolor/index.theme")));
        assert!(!icon_pat.matches_path(std::path::Path::new("usr/share/icons/hicolor")));

        let class_pat = glob::Pattern::new("foo[0-9].txt").unwrap();
        assert!(class_pat.matches_path(std::path::Path::new("foo3.txt")));
        assert!(!class_pat.matches_path(std::path::Path::new("fooa.txt")));

        assert!(glob::Pattern::new("unclosed[bracket").is_err());
    }
}
