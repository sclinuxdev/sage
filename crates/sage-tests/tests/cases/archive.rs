mod archive_tests {
    use sage_archive::*;
    use std::collections::BTreeMap;
    use std::fs::{self, File};
    use std::path::PathBuf;
    use sha2::{Digest, Sha256};

    #[test]
    fn index_rejects_parent_paths() {
        assert!(parse_file_index(b"../etc/passwd\t0644\t1\t0000000000000000000000000000000000000000000000000000000000000000\n").is_err());
    }

    #[test]
    fn generated_index_round_trips() {
        let root = tempfile::tempdir().unwrap();
        fs::write(root.path().join("file"), b"data").unwrap();
        let records = build_file_index(root.path()).unwrap();
        assert_eq!(
            parse_file_index(format_file_index(&records).as_bytes()).unwrap(),
            records
        );
    }

    #[test]
    fn package_creation_rejects_lifecycle_scripts() {
        let temp = tempfile::tempdir().unwrap();
        let stage = temp.path().join("stage");
        fs::create_dir_all(stage.join(".METADATA")).unwrap();
        fs::create_dir(stage.join("data")).unwrap();
        fs::write(stage.join(".METADATA/manifest.toml"), b"").unwrap();
        fs::write(stage.join(".METADATA/files.idx"), b"").unwrap();
        fs::write(stage.join(".METADATA/preinst"), b"#!/bin/sh").unwrap();
        let error = create_package(&stage, temp.path().join("bad.pkg.tar.zst"), 1).unwrap_err();
        assert!(error
            .to_string()
            .contains("lifecycle scripts are not permitted"));
    }

    #[test]
    fn package_inspection_rejects_remote_lifecycle_scripts() {
        let temp = tempfile::tempdir().unwrap();
        let package = temp.path().join("hostile.pkg.tar.zst");
        let encoder = zstd::Encoder::new(File::create(&package).unwrap(), 1).unwrap();
        let mut builder = tar::Builder::new(encoder);
        let body = b"#!/bin/sh\nread answer\n";
        let mut header = tar::Header::new_gnu();
        header.set_entry_type(tar::EntryType::Regular);
        header.set_mode(0o755);
        header.set_size(body.len() as u64);
        header.set_cksum();
        builder
            .append_data(&mut header, ".METADATA/preinst", &body[..])
            .unwrap();
        builder.into_inner().unwrap().finish().unwrap();

        let error = inspect_package(package).unwrap_err();
        assert!(error
            .to_string()
            .contains("lifecycle scripts are not permitted"));
    }

    #[test]
    fn package_round_trip_inspection_and_extraction() {
        let temp = tempfile::tempdir().unwrap();
        let stage = temp.path().join("stage");
        fs::create_dir_all(stage.join(".METADATA")).unwrap();
        fs::create_dir_all(stage.join("data/usr/bin")).unwrap();
        fs::create_dir_all(stage.join("data/var/lib/app")).unwrap();
        fs::write(stage.join("data/usr/bin/hello"), b"hello").unwrap();
        std::os::unix::fs::symlink("bin/hello", stage.join("data/usr/hello")).unwrap();
        let hash = hex::encode(Sha256::digest(b"hello"));
        let link_hash = hex::encode(Sha256::digest(b"bin/hello"));
        fs::write(
            stage.join(".METADATA/files.idx"),
            format!("usr/bin/hello\t0755\t5\t{hash}\nusr/hello\t0777\t9\t{link_hash}\n"),
        )
        .unwrap();
        fs::write(
            stage.join(".METADATA/manifest.toml"),
            r#"schema_version=1
name="hello"
version="1.0"
release=1
arch="amd64"
channel="system"
description="hello"
license="MIT"
installed_size=5
build_time=1
"#,
        )
        .unwrap();
        let package = temp.path().join("hello.pkg.tar.zst");
        create_package(&stage, &package, 1).unwrap();
        let inspection = inspect_package(&package).unwrap();
        assert_eq!(inspection.manifest.name, "hello");
        let root = temp.path().join("root");
        fs::create_dir(&root).unwrap();
        extract_package(&package, &root, &inspection.files).unwrap();
        assert_eq!(fs::read(root.join("usr/bin/hello")).unwrap(), b"hello");
        assert_eq!(
            fs::read_link(root.join("usr/hello")).unwrap(),
            PathBuf::from("bin/hello")
        );
    }

    #[test]
    fn modified_configuration_is_written_as_sage_new() {
        let temp = tempfile::tempdir().unwrap();
        let stage = temp.path().join("stage");
        fs::create_dir_all(stage.join(".METADATA")).unwrap();
        fs::create_dir_all(stage.join("data/etc")).unwrap();
        fs::write(stage.join("data/etc/demo.conf"), b"new upstream").unwrap();
        let new_hash = hex::encode(Sha256::digest(b"new upstream"));
        fs::write(
            stage.join(".METADATA/files.idx"),
            format!("etc/demo.conf\t0644\t12\t{new_hash}\n"),
        )
        .unwrap();
        fs::write(
            stage.join(".METADATA/manifest.toml"),
            r#"schema_version=1
name="demo"
version="2.0"
release=1
arch="amd64"
channel="system"
description="demo"
license="MIT"
installed_size=12
build_time=1
"#,
        )
        .unwrap();
        let package = temp.path().join("demo.pkg.tar.zst");
        create_package(&stage, &package, 1).unwrap();
        let root = temp.path().join("root");
        fs::create_dir_all(root.join("etc")).unwrap();
        fs::write(root.join("etc/demo.conf"), b"user edit").unwrap();
        let previous = BTreeMap::from([(
            "etc/demo.conf".into(),
            hex::encode(Sha256::digest(b"old upstream")),
        )]);
        let inspection = inspect_package(&package).unwrap();
        let report =
            extract_package_with_config(&package, &root, &inspection.files, &previous).unwrap();
        assert_eq!(fs::read(root.join("etc/demo.conf")).unwrap(), b"user edit");
        assert_eq!(
            fs::read(root.join("etc/demo.conf.sage-new")).unwrap(),
            b"new upstream"
        );
        assert_eq!(
            report.sage_new,
            vec![PathBuf::from("etc/demo.conf.sage-new")]
        );
    }
}
