#[tokio::test]
async fn hermetic_release_gate_covers_core_invariants() {
    sage_tests::run_quick().await.unwrap();
}

#[tokio::test]
async fn fixed_seed_state_machine_matches_the_reference_model() {
    sage_tests::run_random(0x5a6e_2026, 24).await.unwrap();
}

#[tokio::test]
#[ignore = "full benchmarks run in the Torture Lab workflow"]
async fn benchmark_smoke_records_database_metrics() {
    let report = sage_tests::run_bench(100).await.unwrap();
    assert_eq!(report["packages"], 100.0);
    assert!(report["database_bytes"] > 0.0);
    assert!(report["lookup_ns_each"] >= 0.0);
}

#[test]
fn archive_fault_boundaries_retry_without_temporary_debris() {
    let mut lab = sage_tests::TortureLab::new().unwrap();
    let package = lab
        .add_package(sage_tests::PackageSpec::new(
            "system",
            "faulty",
            1,
            "usr/lib/torture/faulty",
            "0123456789abcdef",
        ))
        .unwrap();
    let inspection = sage_archive::inspect_package(&package).unwrap();
    for fault in [
        sage_archive::ExtractionFault::BeforeTemporary,
        sage_archive::ExtractionFault::AfterPartialWrite,
        sage_archive::ExtractionFault::BeforeRename,
        sage_archive::ExtractionFault::AfterRename,
    ] {
        let root = tempfile::tempdir().unwrap();
        let result = sage_archive::extract_package_with_fault(
            &package,
            root.path(),
            &inspection.files,
            fault,
        );
        assert!(matches!(
            result,
            Err(sage_archive::ArchiveError::InjectedFault(point)) if point == fault
        ));
        sage_archive::extract_package(&package, root.path(), &inspection.files).unwrap();
        assert_eq!(
            std::fs::read(root.path().join("usr/lib/torture/faulty")).unwrap(),
            b"0123456789abcdef"
        );
        let directory = root.path().join("usr/lib/torture");
        assert!(std::fs::read_dir(directory).unwrap().all(|entry| !entry
            .unwrap()
            .file_name()
            .to_string_lossy()
            .starts_with(".sage-tmp-")));
    }
}

#[test]
fn database_faults_abort_the_entire_reverse_index_transaction() {
    use std::collections::BTreeMap;
    let directory = tempfile::tempdir().unwrap();
    let database = sage_db::SageDatabase::open(directory.path()).unwrap();
    let package = sage_db::InstalledPackage {
        key: sage_core::PackageKey::new("main/system", "faulty", "0"),
        version: sage_core::Version::new(0, "1", 1),
        arch: "noarch".into(),
        installed_size: 1,
        dependencies: Vec::new(),
        provides: vec!["virtual/faulty".into()],
        conflicts: Vec::new(),
        files: vec!["usr/lib/torture/faulty".into()],
        config_hashes: BTreeMap::new(),
    };
    for fault in [
        sage_db::DbFault::BeforeWrite,
        sage_db::DbFault::BeforeCommit,
    ] {
        assert!(matches!(
            database.install_with_fault(&package, false, Some(fault)),
            Err(sage_db::DbError::InjectedFault(point)) if point == fault
        ));
        assert!(database.package(&package.key).unwrap().is_none());
        assert!(database.owners(&package.files[0]).unwrap().is_empty());
        assert!(database.providers(&package.provides[0]).unwrap().is_empty());
    }
    database.install(&package, false).unwrap();
    assert_eq!(database.package(&package.key).unwrap(), Some(package));
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
        .add_package(sage_tests::PackageSpec::new(
            "system",
            "escape",
            1,
            "usr/lib/torture/payload",
            "payload",
        ))
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
    let (_archive_root, archive) = raw_archive_entries(
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
    );
    let inspection = sage_archive::inspect_package(&archive).unwrap();
    let root = tempfile::tempdir().unwrap();
    assert!(matches!(
        sage_archive::extract_package(&archive, root.path(), &inspection.files),
        Err(sage_archive::ArchiveError::ChecksumMismatch { .. })
    ));
    assert!(!root.path().join("usr/first").exists());
    assert!(!root.path().join("usr/second").exists());

    let parent = b"parent";
    let child = b"child";
    let parent_hash = hex::encode(sha2::Sha256::digest(parent));
    let child_hash = hex::encode(sha2::Sha256::digest(child));
    let (_archive_root, archive) = raw_archive_entries(
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
    );
    let inspection = sage_archive::inspect_package(&archive).unwrap();
    let root = tempfile::tempdir().unwrap();
    assert!(sage_archive::extract_package(&archive, root.path(), &inspection.files).is_err());
    assert!(!root.path().join("opt/app").exists());
    assert!(!root.path().join("opt/app/bin/tool").exists());
}

#[tokio::test]
async fn malformed_package_preflight_does_not_leave_a_recovery_journal() {
    let mut lab = sage_tests::TortureLab::new().unwrap();
    let malformed = lab
        .add_package(sage_tests::PackageSpec::new(
            "system", "raw", 1, "usr/hard", "payload",
        ))
        .unwrap();
    lab.add_package(sage_tests::PackageSpec::new(
        "system",
        "healthy",
        1,
        "usr/lib/torture/healthy",
        "healthy",
    ))
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
    lab.install("healthy", "system").await.unwrap();
    lab.audit().unwrap();
}

#[test]
fn filesystem_type_permission_and_length_boundaries_are_fail_closed() {
    use sha2::Digest as _;
    use std::os::unix::fs::PermissionsExt as _;

    let mut lab = sage_tests::TortureLab::new().unwrap();
    let package = lab
        .add_package(sage_tests::PackageSpec::new(
            "system",
            "swap",
            1,
            "usr/lib/torture/swap",
            "file",
        ))
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
        .add_package(sage_tests::PackageSpec::new(
            "system",
            "nested",
            1,
            "usr/lib/torture/parent/child",
            "child",
        ))
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
    let (_archive_root, archive) = raw_archive(
        &format!("{relative}\t0644\t{}\t{hash}\n", bytes.len()),
        &format!("data/{relative}"),
        tar::EntryType::Regular,
        bytes,
        None,
    );
    let long_inspection = sage_archive::inspect_package(&archive).unwrap();
    let long_target = tempfile::tempdir().unwrap();
    assert!(
        sage_archive::extract_package(archive, long_target.path(), &long_inspection.files).is_err()
    );
    assert!(!long_target.path().join("usr/lib").join(component).exists());
}

#[test]
fn hardlinks_and_equal_content_cross_package_claims_are_rejected() {
    let hash = "0".repeat(64);
    let (_archive_root, archive) = raw_archive(
        &format!("usr/hard\t0644\t0\t{hash}\n"),
        "data/usr/hard",
        tar::EntryType::Link,
        &[],
        Some("data/usr/source"),
    );
    let inspection = sage_archive::inspect_package(&archive).unwrap();
    let root = tempfile::tempdir().unwrap();
    assert!(sage_archive::extract_package(&archive, root.path(), &inspection.files).is_err());
    assert!(!root.path().join("usr/hard").exists());

    let mut lab = sage_tests::TortureLab::new().unwrap();
    for name in ["same-a", "same-b"] {
        lab.add_package(sage_tests::PackageSpec::new(
            "system",
            name,
            1,
            "usr/lib/torture/equal",
            "identical",
        ))
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
async fn ownership_handoffs_are_ordered_and_cycles_are_atomic() {
    let mut lab = sage_tests::TortureLab::new().unwrap();
    lab.add_package(sage_tests::PackageSpec::new(
        "system",
        "z-owner",
        1,
        "usr/lib/torture/handoff",
        "old-owner",
    ))
    .unwrap();
    lab.add_package(sage_tests::PackageSpec::new(
        "system",
        "a-claimant",
        1,
        "usr/lib/torture/claimant-old",
        "old-claimant",
    ))
    .unwrap();
    lab.publish().unwrap();
    lab.install("z-owner", "system").await.unwrap();
    lab.install("a-claimant", "system").await.unwrap();

    lab.add_package(sage_tests::PackageSpec::new(
        "system",
        "z-owner",
        2,
        "usr/lib/torture/owner-new",
        "new-owner",
    ))
    .unwrap();
    lab.add_package(sage_tests::PackageSpec::new(
        "system",
        "a-claimant",
        2,
        "usr/lib/torture/handoff",
        "new-claimant",
    ))
    .unwrap();
    lab.publish().unwrap();
    sage::execute(sage::Cli {
        verbose: false,
        dry_run: false,
        root: lab.root().into(),
        lock_timeout: None,
        command: sage::Commands::Upgrade {
            packages: vec!["a-claimant".into(), "z-owner".into()],
            channel: Some("system".into()),
            sync: false,
        },
    })
    .await
    .unwrap();
    let state = lab.audit().unwrap();
    assert_eq!(state.packages["main/system:a-claimant:0"], "2-1");
    assert_eq!(state.packages["main/system:z-owner:0"], "2-1");
    assert_eq!(
        std::fs::read(lab.root().join("usr/lib/torture/handoff")).unwrap(),
        b"new-claimant"
    );
    assert_eq!(
        sage_db::read_owners(&lab.root().join("var/lib/sage"), "usr/lib/torture/handoff").unwrap(),
        [sage_core::PackageKey::new("main/system", "a-claimant", "0")]
    );

    let mut cycle = sage_tests::TortureLab::new().unwrap();
    cycle
        .add_package(sage_tests::PackageSpec::new(
            "system",
            "cycle-a",
            1,
            "usr/lib/torture/cycle-p",
            "a-v1",
        ))
        .unwrap();
    cycle
        .add_package(sage_tests::PackageSpec::new(
            "system",
            "cycle-b",
            1,
            "usr/lib/torture/cycle-q",
            "b-v1",
        ))
        .unwrap();
    cycle.publish().unwrap();
    cycle.install("cycle-a", "system").await.unwrap();
    cycle.install("cycle-b", "system").await.unwrap();
    cycle
        .add_package(sage_tests::PackageSpec::new(
            "system",
            "cycle-a",
            2,
            "usr/lib/torture/cycle-q",
            "a-v2",
        ))
        .unwrap();
    cycle
        .add_package(sage_tests::PackageSpec::new(
            "system",
            "cycle-b",
            2,
            "usr/lib/torture/cycle-p",
            "b-v2",
        ))
        .unwrap();
    cycle.publish().unwrap();
    let before = cycle.audit().unwrap();
    assert!(sage::execute(sage::Cli {
        verbose: false,
        dry_run: false,
        root: cycle.root().into(),
        lock_timeout: None,
        command: sage::Commands::Upgrade {
            packages: vec!["cycle-a".into(), "cycle-b".into()],
            channel: Some("system".into()),
            sync: false,
        },
    })
    .await
    .is_err());
    assert_eq!(cycle.audit().unwrap(), before);
}

#[test]
fn host_lock_contention_is_nonblocking_and_recoverable() {
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
    assert!(matches!(
        sage_core::HostLock::acquire_exclusive_for(&path, std::time::Duration::ZERO),
        Err(sage_core::CoreError::LockTimedOut(_))
    ));
    assert!(matches!(
        sage_core::HostLock::acquire_shared_for(&path, std::time::Duration::ZERO),
        Err(sage_core::CoreError::LockTimedOut(_))
    ));
    let wait_started = std::time::Instant::now();
    assert!(matches!(
        sage_core::HostLock::acquire_exclusive_for(&path, std::time::Duration::from_millis(25)),
        Err(sage_core::CoreError::LockTimedOut(_))
    ));
    let waited = wait_started.elapsed();
    assert!(waited >= std::time::Duration::from_millis(20));
    assert!(waited < std::time::Duration::from_secs(2));
    let runtime = tokio::runtime::Runtime::new().unwrap();
    let error = runtime
        .block_on(sage::execute(sage::Cli {
            verbose: false,
            dry_run: false,
            root: canonical.clone(),
            lock_timeout: Some(0),
            command: sage::Commands::Verify,
        }))
        .unwrap_err();
    assert!(error.to_string().contains("timed out"));
    drop(exclusive);
    let first_reader =
        sage_core::HostLock::acquire_shared_for(&path, std::time::Duration::ZERO).unwrap();
    let second_reader =
        sage_core::HostLock::acquire_shared_for(&path, std::time::Duration::ZERO).unwrap();
    assert!(matches!(
        sage_core::HostLock::acquire_exclusive_for(&path, std::time::Duration::ZERO),
        Err(sage_core::CoreError::LockTimedOut(_))
    ));
    drop((first_reader, second_reader));
    sage_core::HostLock::acquire_exclusive_for(&path, std::time::Duration::ZERO).unwrap();

    let outside = tempfile::tempdir().unwrap();
    std::fs::create_dir_all(canonical.join("run")).unwrap();
    let redirected = canonical.join("run/redirected");
    std::os::unix::fs::symlink(outside.path(), &redirected).unwrap();
    assert!(sage_core::HostLock::acquire_exclusive_for(
        redirected.join("operation.lock"),
        std::time::Duration::ZERO
    )
    .is_err());
    assert!(!outside.path().join("operation.lock").exists());

    let final_directory = canonical.join("run/final-link");
    std::fs::create_dir_all(&final_directory).unwrap();
    let outside_file = outside.path().join("outside-lock");
    std::fs::write(&outside_file, b"outside").unwrap();
    std::os::unix::fs::symlink(&outside_file, final_directory.join("operation.lock")).unwrap();
    assert!(sage_core::HostLock::acquire_exclusive_for(
        final_directory.join("operation.lock"),
        std::time::Duration::ZERO
    )
    .is_err());
    assert_eq!(std::fs::read(outside_file).unwrap(), b"outside");
}

#[test]
fn concurrent_process_writers_serialize_real_package_operations() {
    let mut lab = sage_tests::TortureLab::new().unwrap();
    for name in ["left", "right"] {
        lab.add_package(sage_tests::PackageSpec::new(
            "system",
            name,
            1,
            &format!("usr/lib/torture/{name}"),
            name,
        ))
        .unwrap();
    }
    lab.add_package(sage_tests::PackageSpec::new(
        "runtime",
        "parallel",
        1,
        "bin/parallel",
        "runtime",
    ))
    .unwrap();
    lab.add_package(sage_tests::PackageSpec::new(
        "toolchain",
        "parallel",
        1,
        "bin/parallel",
        "toolchain",
    ))
    .unwrap();
    lab.publish().unwrap();
    let operation_lock = lab
        .root()
        .canonicalize()
        .unwrap()
        .join("run/sage/operation.lock");
    let gate = sage_core::HostLock::acquire_exclusive(&operation_lock).unwrap();
    let binary = env!("CARGO_BIN_EXE_sage-torture");
    let mut left = std::process::Command::new(binary)
        .args([
            "worker-install",
            lab.root().to_str().unwrap(),
            "left",
            "system",
        ])
        .spawn()
        .unwrap();
    let mut right = std::process::Command::new(binary)
        .args([
            "worker-install",
            lab.root().to_str().unwrap(),
            "right",
            "system",
        ])
        .spawn()
        .unwrap();
    drop(gate);
    assert!(left.wait().unwrap().success());
    assert!(right.wait().unwrap().success());
    let state = lab.audit().unwrap();
    assert!(state.packages.contains_key("main/system:left:0"));
    assert!(state.packages.contains_key("main/system:right:0"));

    // Two processes upgrading the same package converge on one version.
    lab.add_package(sage_tests::PackageSpec::new(
        "system",
        "left",
        2,
        "usr/lib/torture/left",
        "left-v2",
    ))
    .unwrap();
    lab.publish().unwrap();
    let gate = sage_core::HostLock::acquire_exclusive(&operation_lock).unwrap();
    let mut first_upgrade = std::process::Command::new(binary)
        .args([
            "worker-upgrade",
            lab.root().to_str().unwrap(),
            "left",
            "system",
        ])
        .spawn()
        .unwrap();
    let mut second_upgrade = std::process::Command::new(binary)
        .args([
            "worker-upgrade",
            lab.root().to_str().unwrap(),
            "left",
            "system",
        ])
        .spawn()
        .unwrap();
    drop(gate);
    assert!(first_upgrade.wait().unwrap().success());
    assert!(second_upgrade.wait().unwrap().success());
    assert_eq!(lab.audit().unwrap().packages["main/system:left:0"], "2-1");

    // Physically isolated channels may publish concurrently under the same global lock.
    let gate = sage_core::HostLock::acquire_exclusive(&operation_lock).unwrap();
    let mut runtime_install = std::process::Command::new(binary)
        .args([
            "worker-install",
            lab.root().to_str().unwrap(),
            "parallel",
            "runtime",
        ])
        .spawn()
        .unwrap();
    let mut toolchain_install = std::process::Command::new(binary)
        .args([
            "worker-install",
            lab.root().to_str().unwrap(),
            "parallel",
            "toolchain",
        ])
        .spawn()
        .unwrap();
    drop(gate);
    assert!(runtime_install.wait().unwrap().success());
    assert!(toolchain_install.wait().unwrap().success());
    let state = lab.audit().unwrap();
    assert!(state.packages.contains_key("main/runtime:parallel:0"));
    assert!(state.packages.contains_key("main/toolchain:parallel:0"));

    // A shared read-only verifier and a writer each observe one complete serial state.
    lab.add_package(sage_tests::PackageSpec::new(
        "system",
        "query-race",
        1,
        "usr/lib/torture/query-race",
        "query-race",
    ))
    .unwrap();
    lab.publish().unwrap();
    let gate = sage_core::HostLock::acquire_exclusive(&operation_lock).unwrap();
    let mut verify = std::process::Command::new(binary)
        .args(["worker-verify", lab.root().to_str().unwrap()])
        .spawn()
        .unwrap();
    let mut query_race = std::process::Command::new(binary)
        .args([
            "worker-install",
            lab.root().to_str().unwrap(),
            "query-race",
            "system",
        ])
        .spawn()
        .unwrap();
    drop(gate);
    assert!(verify.wait().unwrap().success());
    assert!(query_race.wait().unwrap().success());
    lab.audit().unwrap();

    // A writer racing with a removal still converges to one explainable serial order.
    let gate = sage_core::HostLock::acquire_exclusive(&operation_lock).unwrap();
    let mut remove = std::process::Command::new(binary)
        .args([
            "worker-remove",
            lab.root().to_str().unwrap(),
            "left",
            "system",
        ])
        .spawn()
        .unwrap();
    let mut reinstall = std::process::Command::new(binary)
        .args([
            "worker-install",
            lab.root().to_str().unwrap(),
            "left",
            "system",
        ])
        .spawn()
        .unwrap();
    drop(gate);
    let remove_ok = remove.wait().unwrap().success();
    let install_ok = reinstall.wait().unwrap().success();
    assert!(remove_ok && install_ok);
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
    lab.add_package(sage_tests::PackageSpec::new(
        "system",
        "abrupt",
        1,
        "usr/lib/torture/abrupt",
        "durable",
    ))
    .unwrap();
    lab.publish().unwrap();
    lab.inject("abort:before-lmdb-write").unwrap();
    let binary = env!("CARGO_BIN_EXE_sage-torture");
    let status = std::process::Command::new(binary)
        .args([
            "worker-install",
            lab.root().to_str().unwrap(),
            "abrupt",
            "system",
        ])
        .status()
        .unwrap();
    assert!(!status.success());
    let retry = std::process::Command::new(binary)
        .args([
            "worker-install",
            lab.root().to_str().unwrap(),
            "abrupt",
            "system",
        ])
        .status()
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
async fn verify_detects_database_and_rootfs_divergence() {
    let mut lab = sage_tests::TortureLab::new().unwrap();
    lab.add_package(sage_tests::PackageSpec::new(
        "system",
        "verify-me",
        1,
        "usr/lib/torture/verify-me",
        "present",
    ))
    .unwrap();
    lab.publish().unwrap();
    lab.install("verify-me", "system").await.unwrap();
    sage::execute(sage::Cli {
        verbose: false,
        dry_run: false,
        root: lab.root().into(),
        lock_timeout: None,
        command: sage::Commands::Verify,
    })
    .await
    .unwrap();
    std::fs::remove_file(lab.root().join("usr/lib/torture/verify-me")).unwrap();
    assert!(sage::execute(sage::Cli {
        verbose: false,
        dry_run: false,
        root: lab.root().into(),
        lock_timeout: None,
        command: sage::Commands::Verify,
    })
    .await
    .is_err());
}

#[tokio::test]
async fn remove_repairs_a_database_record_whose_filesystem_subtree_is_missing() {
    let mut lab = sage_tests::TortureLab::new().unwrap();
    lab.add_package(sage_tests::PackageSpec::new(
        "system",
        "missing-tree",
        1,
        "usr/lib/torture/missing/subtree/file",
        "payload",
    ))
    .unwrap();
    lab.publish().unwrap();
    lab.install("missing-tree", "system").await.unwrap();
    std::fs::remove_dir_all(lab.root().join("usr/lib/torture/missing")).unwrap();
    assert!(lab.audit().is_err());
    lab.remove("missing-tree", "system").await.unwrap();
    let state = lab.audit().unwrap();
    assert!(!state.packages.contains_key("main/system:missing-tree:0"));
}

fn raw_archive(
    index: &str,
    payload_path: &str,
    entry_type: tar::EntryType,
    payload: &[u8],
    link: Option<&str>,
) -> (tempfile::TempDir, std::path::PathBuf) {
    raw_archive_entries(index, &[(payload_path, entry_type, payload, link)])
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
