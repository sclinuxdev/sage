#[tokio::test]
async fn hermetic_release_gate_covers_core_invariants() {
    sage_tests::run_quick().await.unwrap();
}

#[tokio::test]
async fn fixed_seed_state_machine_matches_the_reference_model() {
    sage_tests::run_random(0x5a6e_2026, 24).await.unwrap();
}

#[test]
fn batch_conflicts_rollback_every_package_record() {
    use std::collections::BTreeMap;
    let directory = tempfile::tempdir().unwrap();
    let database = sage_db::SageDatabase::open(directory.path()).unwrap();
    let package = |name: &str| sage_db::InstalledPackage {
        key: sage_core::PackageKey::new("main/system", name, "0"),
        version: sage_core::Version::new(0, "1", 1),
        arch: "noarch".into(),
        installed_size: 1,
        dependencies: Vec::new(),
        provides: Vec::new(),
        conflicts: Vec::new(),
        files: vec!["usr/lib/torture/shared".into()],
        config_hashes: BTreeMap::new(),
    };
    let packages = [package("first"), package("second")];
    assert!(matches!(
        database.install_batch(&packages, false),
        Err(sage_db::DbError::FileConflict { .. })
    ));
    assert!(database.packages().unwrap().is_empty());
    assert!(database
        .owners("usr/lib/torture/shared")
        .unwrap()
        .is_empty());
}

#[test]
fn archive_attack_matrix_is_fail_closed() {
    for line in [
        "../escape\t0644\t1\t0000000000000000000000000000000000000000000000000000000000000000\n",
        "../../escape\t0644\t1\t0000000000000000000000000000000000000000000000000000000000000000\n",
        "/absolute\t0644\t1\t0000000000000000000000000000000000000000000000000000000000000000\n",
        "\t0644\t1\t0000000000000000000000000000000000000000000000000000000000000000\n",
    ] {
        assert!(sage_archive::parse_file_index(line.as_bytes()).is_err());
    }
    assert!(sage_archive::parse_file_index(&[0xff, b'\n']).is_err());
    let hash = "0".repeat(64);
    assert!(sage_archive::parse_file_index(
        format!("usr//bin/tool\t0644\t1\t{hash}\nusr/bin/tool\t0644\t1\t{hash}\n").as_bytes()
    )
    .is_err());

    let mut lab = sage_tests::TortureLab::new().unwrap();
    let package = lab
        .add("system", "escape", 1, "usr/lib/torture/payload", "payload")
        .unwrap();
    let inspection = sage_archive::inspect_package(&package).unwrap();
    let target = tempfile::tempdir().unwrap();
    let outside = tempfile::tempdir().unwrap();
    std::fs::create_dir_all(target.path().join("usr/lib")).unwrap();
    std::os::unix::fs::symlink(outside.path(), target.path().join("usr/lib/torture")).unwrap();
    assert!(sage_archive::extract_package(&package, target.path(), &inspection.files).is_err());
    assert!(!outside.path().join("payload").exists());
}

#[test]
fn payload_validation_precedes_all_filesystem_writes() {
    use sha2::Digest as _;

    let first = b"first";
    let expected_second = b"right";
    let actual_second = b"wrong";
    let first_hash = hex::encode(sha2::Sha256::digest(first));
    let second_hash = hex::encode(sha2::Sha256::digest(expected_second));
    let error = assert_archive_rejected(
        &format!(
            "usr/first\t0644\t{}\t{first_hash}\nusr/second\t0644\t{}\t{second_hash}\n",
            first.len(),
            expected_second.len()
        ),
        &[
            ("data/usr/first", tar::EntryType::Regular, first, None),
            (
                "data/usr/second",
                tar::EntryType::Regular,
                actual_second,
                None,
            ),
        ],
        &["usr/first", "usr/second"],
    );
    assert!(matches!(
        error,
        sage_archive::ArchiveError::ChecksumMismatch { .. }
    ));

    let parent = b"parent";
    let child = b"child";
    let parent_hash = hex::encode(sha2::Sha256::digest(parent));
    let child_hash = hex::encode(sha2::Sha256::digest(child));
    assert_archive_rejected(
        &format!(
            "opt/app\t0644\t{}\t{parent_hash}\nopt/app/bin/tool\t0644\t{}\t{child_hash}\n",
            parent.len(),
            child.len()
        ),
        &[
            ("data/opt/app", tar::EntryType::Regular, parent, None),
            (
                "data/opt/app/bin/tool",
                tar::EntryType::Regular,
                child,
                None,
            ),
        ],
        &["opt/app", "opt/app/bin/tool"],
    );

    let payload = b"owned";
    let payload_hash = hex::encode(sha2::Sha256::digest(payload));
    assert_archive_rejected(
        &format!("usr/owned/file\t0644\t{}\t{payload_hash}\n", payload.len()),
        &[
            ("data/var/unowned", tar::EntryType::Directory, b"", None),
            (
                "data/usr/owned/file",
                tar::EntryType::Regular,
                payload,
                None,
            ),
        ],
        &["var/unowned", "usr/owned/file"],
    );

    assert_archive_rejected(
        &format!("usr/collision\t0644\t{}\t{payload_hash}\n", payload.len()),
        &[
            ("data/usr/collision", tar::EntryType::Regular, payload, None),
            ("data/usr/collision", tar::EntryType::Directory, b"", None),
        ],
        &["usr/collision"],
    );
}

#[tokio::test]
async fn malformed_package_preflight_does_not_leave_a_recovery_journal() {
    let mut lab = sage_tests::TortureLab::new().unwrap();
    let malformed = lab.add("system", "raw", 1, "usr/hard", "payload").unwrap();
    lab.add("system", "healthy", 1, "usr/lib/torture/healthy", "healthy")
        .unwrap();
    lab.add("system", "blocked", 1, "usr/lib/torture/blocked", "blocked")
        .unwrap();
    lab.add("system", "linked", 1, "opt/app-link/file", "linked")
        .unwrap();
    write_raw_archive(
        &malformed,
        &format!("usr/hard\t0644\t0\t{}\n", "0".repeat(64)),
        &[(
            "data/usr/hard",
            tar::EntryType::Link,
            b"",
            Some("data/usr/source"),
        )],
    );
    lab.publish().unwrap();

    assert!(lab.install("raw", "system").await.is_err());
    {
        let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
        assert!(database.pending_journals().unwrap().is_empty());
        assert!(database.packages().unwrap().is_empty());
    }
    assert!(!lab.root().join("usr/hard").exists());
    std::fs::create_dir_all(lab.root().join("usr/lib/torture/blocked")).unwrap();
    assert!(lab.install("blocked", "system").await.is_err());
    {
        let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
        assert!(database.pending_journals().unwrap().is_empty());
        assert!(database.packages().unwrap().is_empty());
    }
    assert!(lab.root().join("usr/lib/torture/blocked").is_dir());
    let outside = tempfile::tempdir().unwrap();
    std::fs::create_dir_all(lab.root().join("opt")).unwrap();
    std::os::unix::fs::symlink(outside.path(), lab.root().join("opt/app-link")).unwrap();
    assert!(lab.install("linked", "system").await.is_err());
    {
        let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
        assert!(database.pending_journals().unwrap().is_empty());
        assert!(database.packages().unwrap().is_empty());
    }
    assert!(!outside.path().join("file").exists());
    lab.install("healthy", "system").await.unwrap();
    lab.audit().unwrap();
}

#[tokio::test]
async fn declaration_payload_collision_is_rejected_before_publication() {
    use sha2::{Digest, Sha256};

    let mut lab = sage_tests::TortureLab::new().unwrap();
    let path = "usr/share/sage/services/foo.toml";
    let payload = b"verified payload";
    let package = lab.add("system", "raw", 1, path, "payload").unwrap();
    write_raw_archive(
        &package,
        &format!("{path}\t0644\t{}\t{:x}\n", payload.len(), Sha256::digest(payload)),
        &[
            (".METADATA/service.toml", tar::EntryType::Regular,
             b"schema_version=1\n[service]\nname=\"foo\"\ndescription=\"Foo\"\ncommand=[\"/usr/bin/foo\"]\nuser=\"root\"\ngroup=\"root\"\nworking_dir=\"/\"\nrestart=\"no\"\ntype=\"simple\"\n", None),
            ("data/usr/share/sage/services/foo.toml", tar::EntryType::Regular, payload, None),
        ],
    );
    lab.publish().unwrap();

    let error = lab.install("raw", "system").await.unwrap_err();
    assert!(format!("{error:#}").contains("both own"));
    assert!(!lab.root().join(path).exists());
    let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
    assert!(database.pending_journals().unwrap().is_empty());
    assert!(database.packages().unwrap().is_empty());
    assert!(database.file_owners().unwrap().is_empty());
}

#[test]
fn filesystem_type_permission_and_length_boundaries_are_fail_closed() {
    use sha2::Digest as _;
    use std::os::unix::fs::PermissionsExt as _;

    let mut lab = sage_tests::TortureLab::new().unwrap();
    let package = lab
        .add("system", "swap", 1, "usr/lib/torture/swap", "file")
        .unwrap();
    let inspection = sage_archive::inspect_package(&package).unwrap();

    let directory_target = tempfile::tempdir().unwrap();
    std::fs::create_dir_all(directory_target.path().join("usr/lib/torture/swap")).unwrap();
    std::fs::write(
        directory_target.path().join("usr/lib/torture/swap/keep"),
        b"keep",
    )
    .unwrap();
    assert!(
        sage_archive::extract_package(&package, directory_target.path(), &inspection.files)
            .is_err()
    );
    assert!(directory_target
        .path()
        .join("usr/lib/torture/swap/keep")
        .exists());

    let mut nested_lab = sage_tests::TortureLab::new().unwrap();
    let nested = nested_lab
        .add(
            "system",
            "nested",
            1,
            "usr/lib/torture/parent/child",
            "child",
        )
        .unwrap();
    let nested_inspection = sage_archive::inspect_package(&nested).unwrap();
    let file_target = tempfile::tempdir().unwrap();
    std::fs::create_dir_all(file_target.path().join("usr/lib/torture")).unwrap();
    std::fs::write(file_target.path().join("usr/lib/torture/parent"), b"parent").unwrap();
    assert!(
        sage_archive::extract_package(&nested, file_target.path(), &nested_inspection.files)
            .is_err()
    );
    assert_eq!(
        std::fs::read(file_target.path().join("usr/lib/torture/parent")).unwrap(),
        b"parent"
    );

    let read_only = tempfile::tempdir().unwrap();
    let protected = read_only.path().join("usr/lib/torture");
    std::fs::create_dir_all(&protected).unwrap();
    std::fs::set_permissions(&protected, std::fs::Permissions::from_mode(0o555)).unwrap();
    let result = sage_archive::extract_package(&package, read_only.path(), &inspection.files);
    std::fs::set_permissions(&protected, std::fs::Permissions::from_mode(0o755)).unwrap();
    assert!(result.is_err());

    let component = "x".repeat(300);
    let relative = format!("usr/lib/{component}/file");
    let bytes = b"long";
    let hash = hex::encode(sha2::Sha256::digest(bytes));
    let (_archive_root, archive) = raw_archive_entries(
        &format!("{relative}\t0644\t{}\t{hash}\n", bytes.len()),
        &[(
            &format!("data/{relative}"),
            tar::EntryType::Regular,
            bytes,
            None,
        )],
    );
    let long_inspection = sage_archive::inspect_package(&archive).unwrap();
    let long_target = tempfile::tempdir().unwrap();
    assert!(
        sage_archive::extract_package(archive, long_target.path(), &long_inspection.files).is_err()
    );
    assert!(!long_target.path().join("usr/lib").join(component).exists());
}

#[test]
fn equal_content_cross_package_claims_are_rejected() {
    let mut lab = sage_tests::TortureLab::new().unwrap();
    for name in ["same-a", "same-b"] {
        lab.add("system", name, 1, "usr/lib/torture/equal", "identical")
            .unwrap();
    }
    lab.publish().unwrap();
    let runtime = tokio::runtime::Runtime::new().unwrap();
    runtime.block_on(lab.install("same-a", "system")).unwrap();
    let before = lab.audit().unwrap();
    assert!(runtime.block_on(lab.install("same-b", "system")).is_err());
    assert_eq!(lab.audit().unwrap(), before);
}

#[tokio::test]
async fn dependency_removal_uses_solver_provider_semantics() {
    let mut lab = sage_tests::TortureLab::new().unwrap();
    lab.add("system", "lib", 1, "usr/lib/torture/lib", "lib")
        .unwrap();
    let mut alias =
        sage_tests::PackageSpec::new("system", "alias", 1, "usr/lib/torture/alias", "alias");
    alias.provides.push("lib".into());
    lab.add_package(alias).unwrap();
    let mut concrete_user = sage_tests::PackageSpec::new(
        "system",
        "concrete-user",
        1,
        "usr/lib/torture/concrete-user",
        "user",
    );
    concrete_user.dependencies.push("lib".into());
    lab.add_package(concrete_user).unwrap();

    let mut libc =
        sage_tests::PackageSpec::new("system", "libc", 1, "usr/lib/torture/libc-1", "libc");
    libc.slot = "1".into();
    libc.provides.push("virtual/libc".into());
    lab.add_package(libc).unwrap();
    let mut virtual_user = sage_tests::PackageSpec::new(
        "system",
        "virtual-user",
        1,
        "usr/lib/torture/virtual-user",
        "user",
    );
    virtual_user.dependencies.push("virtual/libc".into());
    lab.add_package(virtual_user).unwrap();
    lab.publish().unwrap();

    lab.install("concrete-user", "system").await.unwrap();
    lab.install("alias", "system").await.unwrap();
    assert!(lab.remove("lib", "system").await.is_err());
    lab.install("virtual-user", "system").await.unwrap();
    assert!(lab.remove("libc:1", "system").await.is_err());
}

#[tokio::test]
async fn provider_changes_fail_before_mutating_the_working_system() {
    let mut lab = sage_tests::TortureLab::new().unwrap();
    let renderer = "schema_version=1\n[service_generator]\ntarget_path=\"/etc/native/${service.name}\"\nmode=420\ntemplate=\"\"\n";
    for name in ["systemd", "loom"] {
        let mut provider = sage_tests::PackageSpec::new(
            "system",
            name,
            1,
            &format!("usr/share/sage/rclass/init-{name}.toml"),
            renderer,
        );
        provider.provides.push("virtual/init".into());
        if name == "systemd" {
            provider.files.insert(
                "usr/share/sage/services/daemon.toml".into(),
                b"schema_version=1\n[service]\nname=\"daemon\"\ndescription=\"Daemon\"\ncommand=[\"/usr/bin/daemon\"]\nuser=\"daemon\"\ngroup=\"daemon\"\nworking_dir=\"/\"\nrestart=\"no\"\ntype=\"simple\"\n".to_vec(),
            );
        }
        lab.add_package(provider).unwrap();
    }
    lab.publish().unwrap();
    lab.install("systemd", "system").await.unwrap();
    lab.install("loom", "system").await.unwrap();
    let config_path = lab.root().join("etc/sage/system.toml");
    let config = "schema_version=1\npackages=[\"systemd\",\"loom\"]\nservices=[\"daemon\"]\n[system]\narchitecture=\"amd64\"\nprofile=\"default\"\n[providers]\ninit=\"systemd\"\n";
    std::fs::write(&config_path, config).unwrap();
    sage::execute(sage::Cli {
        verbose: false,
        dry_run: false,
        root: lab.root().into(),
        command: sage::Commands::Rebuild { no_prune: false },
    })
    .await
    .unwrap();
    let before = lab.snapshot().unwrap();
    let state_path = lab.root().join("var/lib/sage/rendered-services.toml");
    let state = std::fs::read(&state_path).unwrap();
    let native_path = lab.root().join("etc/native/daemon");
    let native = std::fs::read(&native_path).unwrap();
    for dry_run in [false, true] {
        let error = sage::execute(sage::Cli {
            verbose: false,
            dry_run,
            root: lab.root().into(),
            command: sage::Commands::Remove {
                packages: vec!["systemd".into()],
                channel: None,
            },
        })
        .await
        .unwrap_err();
        assert!(error.to_string().contains("bound provider"), "{error:#}");
        assert_eq!(lab.snapshot().unwrap(), before);
        assert_eq!(std::fs::read_to_string(&config_path).unwrap(), config);
        assert_eq!(std::fs::read(&state_path).unwrap(), state);
        assert_eq!(std::fs::read(&native_path).unwrap(), native);
        let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
        assert_eq!(
            database.system_provider("init").unwrap(),
            Some(sage_core::PackageKey::new("main/system", "systemd", "0"))
        );
        assert!(database.pending_journals().unwrap().is_empty());
    }
    // An unused provider can still be removed without switching the binding.
    lab.remove("loom", "system").await.unwrap();

    // Its old same-name template must not mask a missing file in the new slot.
    lab.install("loom", "system").await.unwrap();
    let before = lab.snapshot().unwrap();
    let config = config
        .replace(
            "packages=[\"systemd\",\"loom\"]",
            "packages=[\"systemd\",\"loom:1\"]",
        )
        .replace("init=\"systemd\"", "init=\"loom:1\"");
    std::fs::write(&config_path, &config).unwrap();
    let unsafe_renderer = "schema_version=1\n[service_generator]\ntarget_path=\"../../etc/${service.name}\"\nmode=420\ntemplate=\"\"\n";
    let unsupported_renderer = "schema_version=1\n[service_generator]\ntarget_path=\"/etc/native/${service.name}\"\nmode=420\ntemplate=\"\"\nsupported_types=[\"forking\"]\n";
    for (index, (renderer, valid)) in [
        (None, false),
        (Some("invalid TOML ["), false),
        (Some(unsafe_renderer), false),
        (Some(unsupported_renderer), false),
        (Some(renderer), true),
    ]
    .into_iter()
    .enumerate()
    {
        let mut provider = sage_tests::PackageSpec::new(
            "system",
            "loom",
            index as u32 + 2,
            "usr/lib/torture/loom",
            "new provider",
        );
        provider.slot = "1".into();
        provider.provides.push("virtual/init".into());
        if let Some(renderer) = renderer {
            provider.files.insert(
                "usr/share/sage/rclass/init-loom.toml".into(),
                renderer.as_bytes().to_vec(),
            );
        }
        lab.add_package(provider).unwrap();
        lab.publish().unwrap();
        let result = sage::execute(sage::Cli {
            verbose: false,
            dry_run: false,
            root: lab.root().into(),
            command: sage::Commands::Rebuild { no_prune: false },
        })
        .await;
        if valid {
            result.unwrap();
            let state = sage_sys::RenderedServicesState::load(&state_path).unwrap();
            assert_eq!(
                state.provider,
                sage_core::PackageKey::new("main/system", "loom", "1")
            );
            assert!(native_path.is_file());
            let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
            assert_eq!(
                database.system_provider("init").unwrap(),
                Some(state.provider)
            );
            assert!(database.pending_journals().unwrap().is_empty());
            continue;
        }
        assert!(result.is_err());
        assert_eq!(lab.snapshot().unwrap(), before);
        assert_eq!(std::fs::read_to_string(&config_path).unwrap(), config);
        assert_eq!(std::fs::read(&state_path).unwrap(), state);
        assert_eq!(std::fs::read(&native_path).unwrap(), native);
        let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
        assert_eq!(
            database.system_provider("init").unwrap(),
            Some(sage_core::PackageKey::new("main/system", "systemd", "0"))
        );
        assert!(database.pending_journals().unwrap().is_empty());
    }
}

#[tokio::test]
async fn ownership_handoffs_are_ordered_and_cycles_are_atomic() {
    let mut lab = sage_tests::TortureLab::new().unwrap();
    let mut owner = sage_tests::PackageSpec::new(
        "system",
        "z-owner",
        1,
        "etc/torture-handoff.conf",
        "old-owner",
    );
    owner.files.insert(
        "a/etc/torture-handoff.conf".into(),
        b"nested-owner".to_vec(),
    );
    lab.add_package(owner).unwrap();
    lab.add(
        "system",
        "a-claimant",
        1,
        "usr/lib/torture/claimant-old",
        "old-claimant",
    )
    .unwrap();
    lab.publish().unwrap();
    lab.install("z-owner", "system").await.unwrap();
    lab.install("a-claimant", "system").await.unwrap();
    std::fs::write(lab.root().join("etc/torture-handoff.conf"), b"user-edit").unwrap();

    for (name, path, content) in [
        ("z-owner", "usr/lib/torture/owner-new", "new-owner"),
        ("a-claimant", "etc/torture-handoff.conf", "new-claimant"),
    ] {
        lab.add("system", name, 2, path, content).unwrap();
    }
    lab.publish().unwrap();
    sage::execute(sage::Cli {
        verbose: false,
        dry_run: false,
        root: lab.root().into(),
        command: sage::Commands::Upgrade {
            packages: vec!["a-claimant".into(), "z-owner".into()],
            channel: Some("system".into()),
            sync: false,
        },
    })
    .await
    .unwrap();
    let state = lab.snapshot().unwrap();
    assert_eq!(state.packages["main/system:a-claimant:0"], "2-1");
    assert_eq!(state.packages["main/system:z-owner:0"], "2-1");
    assert_eq!(
        std::fs::read(lab.root().join("etc/torture-handoff.conf")).unwrap(),
        b"user-edit"
    );
    assert_eq!(
        std::fs::read(lab.root().join("etc/torture-handoff.conf.sage-new")).unwrap(),
        b"new-claimant"
    );
    assert_eq!(
        sage_db::read_owners(&lab.root().join("var/lib/sage"), "etc/torture-handoff.conf").unwrap(),
        [sage_core::PackageKey::new("main/system", "a-claimant", "0")]
    );

    let mut cycle = sage_tests::TortureLab::new().unwrap();
    for (name, path, content) in [
        ("cycle-a", "usr/lib/torture/cycle-p", "a-v1"),
        ("cycle-b", "usr/lib/torture/cycle-q", "b-v1"),
    ] {
        cycle.add("system", name, 1, path, content).unwrap();
    }
    cycle.publish().unwrap();
    cycle.install("cycle-a", "system").await.unwrap();
    cycle.install("cycle-b", "system").await.unwrap();
    for (name, path, content) in [
        ("cycle-a", "usr/lib/torture/cycle-q", "a-v2"),
        ("cycle-b", "usr/lib/torture/cycle-p", "b-v2"),
    ] {
        cycle.add("system", name, 2, path, content).unwrap();
    }
    cycle.publish().unwrap();
    let before = cycle.audit().unwrap();
    assert!(sage::execute(sage::Cli {
        verbose: false,
        dry_run: false,
        root: cycle.root().into(),
        command: sage::Commands::Upgrade {
            packages: vec!["cycle-a".into(), "cycle-b".into()],
            channel: Some("system".into()),
            sync: false,
        },
    })
    .await
    .is_err());
    assert_eq!(cycle.audit().unwrap(), before);

    let mut hierarchy = sage_tests::TortureLab::new().unwrap();
    for (name, path) in [
        ("z-node", "usr/lib/torture/app"),
        ("a-node", "usr/lib/torture/claimant-v1"),
    ] {
        hierarchy.add("system", name, 1, path, "v1").unwrap();
    }
    hierarchy.publish().unwrap();
    hierarchy.install("z-node", "system").await.unwrap();
    hierarchy.install("a-node", "system").await.unwrap();
    for (name, path) in [
        ("z-node", "usr/lib/torture/owner-v2"),
        ("a-node", "usr/lib/torture/app/bin/tool"),
    ] {
        hierarchy.add("system", name, 2, path, "v2").unwrap();
    }
    hierarchy.publish().unwrap();
    let before = hierarchy.audit().unwrap();
    let error = sage::execute(sage::Cli {
        verbose: false,
        dry_run: false,
        root: hierarchy.root().into(),
        command: sage::Commands::Upgrade {
            packages: vec!["z-node".into(), "a-node".into()],
            channel: Some("system".into()),
            sync: false,
        },
    })
    .await
    .unwrap_err();
    assert!(error.to_string().contains("file hierarchy conflict"));
    assert_eq!(hierarchy.audit().unwrap(), before);
}

#[test]
fn host_lock_namespace_is_anchored_and_private() {
    use std::os::unix::fs::PermissionsExt as _;
    let directory = tempfile::tempdir().unwrap();
    let canonical = directory.path().canonicalize().unwrap();
    let path = canonical.join("run/sage/operation.lock");
    let exclusive = sage_core::HostLock::acquire_exclusive(&path).unwrap();
    assert_eq!(
        std::fs::metadata(canonical.join("run"))
            .unwrap()
            .permissions()
            .mode()
            & 0o777,
        0o755
    );
    assert_eq!(
        std::fs::metadata(canonical.join("run/sage"))
            .unwrap()
            .permissions()
            .mode()
            & 0o777,
        0o700
    );
    assert_eq!(
        std::fs::metadata(&path).unwrap().permissions().mode() & 0o777,
        0o600
    );
    drop(exclusive);

    let outside = tempfile::tempdir().unwrap();
    std::fs::create_dir_all(canonical.join("run")).unwrap();
    let redirected = canonical.join("run/redirected");
    std::os::unix::fs::symlink(outside.path(), &redirected).unwrap();
    assert!(sage_core::HostLock::acquire_exclusive(redirected.join("operation.lock")).is_err());
    assert!(!outside.path().join("operation.lock").exists());

    let final_directory = canonical.join("run/final-link");
    std::fs::create_dir_all(&final_directory).unwrap();
    let outside_file = outside.path().join("outside-lock");
    std::fs::write(&outside_file, b"outside").unwrap();
    std::os::unix::fs::symlink(&outside_file, final_directory.join("operation.lock")).unwrap();
    assert!(
        sage_core::HostLock::acquire_exclusive(final_directory.join("operation.lock")).is_err()
    );
    assert_eq!(std::fs::read(&outside_file).unwrap(), b"outside");

    let hard_directory = canonical.join("run/final-hardlink");
    std::fs::create_dir_all(&hard_directory).unwrap();
    std::fs::hard_link(&outside_file, hard_directory.join("operation.lock")).unwrap();
    assert!(sage_core::HostLock::acquire_exclusive(hard_directory.join("operation.lock")).is_err());

    let insecure_directory = canonical.join("run/insecure");
    std::fs::create_dir(&insecure_directory).unwrap();
    std::fs::set_permissions(&insecure_directory, std::fs::Permissions::from_mode(0o777)).unwrap();
    let insecure_lock =
        sage_core::HostLock::acquire_exclusive(insecure_directory.join("operation.lock")).unwrap();
    assert_eq!(
        std::fs::metadata(&insecure_directory)
            .unwrap()
            .permissions()
            .mode()
            & 0o777,
        0o700
    );
    drop(insecure_lock);

    let unsafe_root = tempfile::tempdir().unwrap();
    let unsafe_run = unsafe_root.path().join("run");
    std::fs::create_dir(&unsafe_run).unwrap();
    std::fs::set_permissions(&unsafe_run, std::fs::Permissions::from_mode(0o777)).unwrap();
    assert!(
        sage_core::HostLock::acquire_exclusive(unsafe_run.join("sage/operation.lock")).is_err()
    );

    let unsafe_tree = unsafe_root.path().join("shared");
    std::fs::create_dir(&unsafe_tree).unwrap();
    std::fs::set_permissions(&unsafe_tree, std::fs::Permissions::from_mode(0o777)).unwrap();
    assert!(sage_core::HostLock::acquire_exclusive(
        unsafe_tree.join("root/run/sage/operation.lock")
    )
    .is_err());
}

#[test]
fn concurrent_process_writers_serialize_real_package_operations() {
    let mut lab = sage_tests::TortureLab::new().unwrap();
    for name in ["left", "right"] {
        lab.add("system", name, 1, &format!("usr/lib/torture/{name}"), name)
            .unwrap();
    }
    lab.publish().unwrap();
    let operation_lock = lab
        .root()
        .canonicalize()
        .unwrap()
        .join("run/sage/operation.lock");
    let gate = sage_core::HostLock::acquire_exclusive(&operation_lock).unwrap();
    let mut left = worker(lab.root(), "install", "left", "system");
    let mut right = worker(lab.root(), "install", "right", "system");
    drop(gate);
    assert!(left.wait().unwrap().success());
    assert!(right.wait().unwrap().success());
    let state = lab.audit().unwrap();
    assert!(state.packages.contains_key("main/system:left:0"));
    assert!(state.packages.contains_key("main/system:right:0"));

    // A shared read-only query and a writer each observe one complete serial state.
    lab.add(
        "system",
        "query-race",
        1,
        "usr/lib/torture/query-race",
        "query-race",
    )
    .unwrap();
    lab.publish().unwrap();
    let gate = sage_core::HostLock::acquire_exclusive(&operation_lock).unwrap();
    let mut query = std::process::Command::new(env!("CARGO_BIN_EXE_sage-torture"))
        .args(["worker-query", lab.root().to_str().unwrap()])
        .spawn()
        .unwrap();
    let mut query_race = worker(lab.root(), "install", "query-race", "system");
    drop(gate);
    assert!(query.wait().unwrap().success());
    assert!(query_race.wait().unwrap().success());
    lab.audit().unwrap();
}

#[test]
fn lmdb_map_full_rolls_back_all_indexes() {
    use std::collections::BTreeMap;
    let directory = tempfile::tempdir().unwrap();
    let database =
        sage_db::SageDatabase::open_with_map_size(directory.path(), 1024 * 1024).unwrap();
    let files = (0..30_000)
        .map(|index| format!("usr/lib/torture/map-full/{index:05}-{}", "x".repeat(48)))
        .collect::<Vec<_>>();
    let package = sage_db::InstalledPackage {
        key: sage_core::PackageKey::new("main/system", "map-full", "0"),
        version: sage_core::Version::new(0, "1", 1),
        arch: "noarch".into(),
        installed_size: 1,
        dependencies: Vec::new(),
        provides: vec!["virtual/map-full".into()],
        conflicts: Vec::new(),
        files: files.clone(),
        config_hashes: BTreeMap::new(),
    };
    assert!(database.install(&package, false).is_err());
    assert!(database.packages().unwrap().is_empty());
    assert!(database.owners(&files[0]).unwrap().is_empty());
    assert!(database.providers("virtual/map-full").unwrap().is_empty());
    drop(database);
    let reopened =
        sage_db::SageDatabase::open_with_map_size(directory.path(), 8 * 1024 * 1024).unwrap();
    assert!(reopened.packages().unwrap().is_empty());
}

#[test]
fn abrupt_process_termination_recovers_on_next_start() {
    let mut lab = sage_tests::TortureLab::new().unwrap();
    lab.add("system", "abrupt", 1, "usr/lib/torture/abrupt", "durable")
        .unwrap();
    lab.publish().unwrap();
    lab.inject("abort:before-lmdb-write").unwrap();
    let status = worker(lab.root(), "install", "abrupt", "system")
        .wait()
        .unwrap();
    assert!(!status.success());
    let retry = worker(lab.root(), "install", "abrupt", "system")
        .wait()
        .unwrap();
    assert!(retry.success());
    let state = lab.audit().unwrap();
    assert_eq!(state.packages["main/system:abrupt:0"], "1-1");
}

#[tokio::test]
async fn recovery_failure_is_retryable_more_than_once() {
    let mut lab = sage_tests::TortureLab::new().unwrap();
    let mut package =
        sage_tests::PackageSpec::new("system", "retry-twice", 1, "usr/lib/torture/retry-a", "a");
    package
        .files
        .insert("usr/lib/torture/retry-b".into(), b"b".to_vec());
    lab.add_package(package).unwrap();
    lab.publish().unwrap();

    lab.inject("before-lmdb-write").unwrap();
    assert!(lab.install("retry-twice", "system").await.is_err());
    let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
    let journal = database.pending_journals().unwrap();
    assert_eq!(journal.len(), 1);
    let operation = journal[0].op_id.clone();
    drop(database);

    lab.inject("before-lmdb-write").unwrap();
    assert!(lab.install("retry-twice", "system").await.is_err());
    let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
    let journal = database.pending_journals().unwrap();
    assert_eq!(journal.len(), 1);
    assert_eq!(journal[0].op_id, operation);
    journal[0].validate().unwrap();
    drop(database);

    lab.install("retry-twice", "system").await.unwrap();
    lab.audit().unwrap();

    lab.inject("remove-after-path").unwrap();
    assert!(lab.remove("retry-twice", "system").await.is_err());
    lab.inject("remove-after-path").unwrap();
    assert!(lab.remove("retry-twice", "system").await.is_err());
    assert!(lab.remove("retry-twice", "system").await.is_err());
    let state = lab.audit().unwrap();
    assert!(!state.packages.contains_key("main/system:retry-twice:0"));
}

#[tokio::test]
async fn remove_repairs_a_database_record_whose_filesystem_subtree_is_missing() {
    let mut lab = sage_tests::TortureLab::new().unwrap();
    lab.add(
        "system",
        "missing-tree",
        1,
        "usr/lib/torture/missing/subtree/file",
        "payload",
    )
    .unwrap();
    lab.publish().unwrap();
    lab.install("missing-tree", "system").await.unwrap();
    std::fs::remove_dir_all(lab.root().join("usr/lib/torture/missing")).unwrap();
    assert!(lab.audit().is_err());
    lab.remove("missing-tree", "system").await.unwrap();
    let state = lab.audit().unwrap();
    assert!(!state.packages.contains_key("main/system:missing-tree:0"));
}

#[tokio::test]
async fn audit_rejects_orphaned_reverse_ownership_rows() {
    let mut lab = sage_tests::TortureLab::new().unwrap();
    lab.add("system", "owned", 1, "usr/lib/torture/owned", "payload")
        .unwrap();
    lab.publish().unwrap();
    lab.install("owned", "system").await.unwrap();

    let db_path = lab.root().join("var/lib/sage");
    let mut options = heed::EnvOpenOptions::new();
    options.max_dbs(8);
    // SAFETY: the test owns this isolated environment and has dropped every
    // previous handle before opening it for deliberate corruption.
    let environment = unsafe { options.open(&db_path).unwrap() };
    let mut transaction = environment.write_txn().unwrap();
    let files: heed::Database<heed::types::Str, heed::types::Bytes> = environment
        .open_database(&transaction, Some("files"))
        .unwrap()
        .unwrap();
    let nonexistent = vec![sage_core::PackageKey::new(
        "main/system",
        "nonexistent",
        "0",
    )];
    files
        .put(
            &mut transaction,
            "usr/lib/torture/orphaned-index-row",
            &bincode::serialize(&nonexistent).unwrap(),
        )
        .unwrap();
    transaction.commit().unwrap();
    drop(environment);

    assert!(lab.audit().is_err());
}

fn assert_archive_rejected(
    index: &str,
    entries: &[(&str, tar::EntryType, &[u8], Option<&str>)],
    absent: &[&str],
) -> sage_archive::ArchiveError {
    let (_directory, archive) = raw_archive_entries(index, entries);
    let inspection = sage_archive::inspect_package(&archive).unwrap();
    let root = tempfile::tempdir().unwrap();
    let error =
        sage_archive::extract_package(&archive, root.path(), &inspection.files).unwrap_err();
    assert!(absent.iter().all(|path| !root.path().join(path).exists()));
    error
}

fn worker(
    root: &std::path::Path,
    action: &str,
    package: &str,
    channel: &str,
) -> std::process::Child {
    std::process::Command::new(env!("CARGO_BIN_EXE_sage-torture"))
        .args([
            &format!("worker-{action}"),
            root.to_str().unwrap(),
            package,
            channel,
        ])
        .spawn()
        .unwrap()
}

fn raw_archive_entries(
    index: &str,
    entries: &[(&str, tar::EntryType, &[u8], Option<&str>)],
) -> (tempfile::TempDir, std::path::PathBuf) {
    let directory = tempfile::tempdir().unwrap();
    let package = directory.path().join("raw.pkg.tar.zst");
    write_raw_archive(&package, index, entries);
    (directory, package)
}

fn write_raw_archive(
    package: &std::path::Path,
    index: &str,
    entries: &[(&str, tar::EntryType, &[u8], Option<&str>)],
) {
    use std::fs::File;
    let encoder = zstd::Encoder::new(File::create(package).unwrap(), 1).unwrap();
    let mut builder = tar::Builder::new(encoder);
    append_raw_entry(
        &mut builder,
        ".METADATA/manifest.toml",
        tar::EntryType::Regular,
        br#"schema_version=1
name="raw"
version="1"
release=1
arch="noarch"
channel="system"
description="raw"
license="MIT"
"#,
        None,
    );
    append_raw_entry(
        &mut builder,
        ".METADATA/files.idx",
        tar::EntryType::Regular,
        index.as_bytes(),
        None,
    );
    for (path, entry_type, payload, link) in entries {
        append_raw_entry(&mut builder, path, *entry_type, payload, *link);
    }
    builder.into_inner().unwrap().finish().unwrap();
}

fn append_raw_entry<W: std::io::Write>(
    builder: &mut tar::Builder<W>,
    path: &str,
    entry_type: tar::EntryType,
    payload: &[u8],
    link: Option<&str>,
) {
    let mut header = tar::Header::new_gnu();
    header.set_entry_type(entry_type);
    header.set_mode(0o644);
    header.set_uid(0);
    header.set_gid(0);
    header.set_mtime(0);
    header.set_size(if entry_type.is_file() {
        payload.len() as u64
    } else {
        0
    });
    if let Some(link) = link {
        header.set_link_name(link).unwrap();
    }
    header.set_cksum();
    builder.append_data(&mut header, path, payload).unwrap();
}

#[tokio::test]
async fn obsolete_upgrade_paths_run_the_old_removal_trigger() {
    let mut lab = sage_tests::TortureLab::new().unwrap();
    let mut tool = sage_tests::PackageSpec::new(
        "system",
        "record",
        1,
        "usr/bin/record",
        "#!/bin/sh\nprintf removed > \"$1\"\n",
    );
    tool.executables.insert("usr/bin/record".into());
    lab.add_package(tool).unwrap();
    let mut package =
        sage_tests::PackageSpec::new("system", "old-path", 1, "usr/lib/torture/obsolete", "old");
    package.files.insert("usr/share/sage/triggers/obsolete.toml".into(),
        b"schema_version=1\nname=\"obsolete\"\ndescription=\"Record removal\"\non_paths=[\"usr/lib/torture/obsolete\"]\nexec=[\"/usr/bin/record\",\"${sysroot}/removal-log\"]\npriority=1\nevents=[\"post-remove\"]\n".to_vec());
    lab.add_package(package).unwrap();
    lab.publish().unwrap();
    lab.install("record", "system").await.unwrap();
    lab.install("old-path", "system").await.unwrap();
    assert!(!lab.root().join("removal-log").exists());
    lab.add(
        "system",
        "old-path",
        2,
        "usr/lib/torture/replacement",
        "new",
    )
    .unwrap();
    lab.publish().unwrap();
    lab.inject("triggers").unwrap();
    assert!(lab.upgrade("old-path", "system").await.is_err());
    lab.upgrade("old-path", "system").await.unwrap();
    assert_eq!(
        std::fs::read(lab.root().join("removal-log")).unwrap(),
        b"removed"
    );
    assert!(!lab.root().join("usr/lib/torture/obsolete").exists());
    lab.snapshot().unwrap();
}

