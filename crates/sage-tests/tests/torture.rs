#[tokio::test]
async fn hermetic_release_gate_covers_core_invariants() {
    sage_tests::run_quick().await.unwrap();
}

#[tokio::test]
async fn fixed_seed_state_machine_matches_the_reference_model() {
    sage_tests::run_random(0x5a6e_2026, 24).await.unwrap();
}

#[tokio::test]
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
        format!("usr//bin/tool\t0644\t1\t{hash}\nusr/bin/tool\t0644\t1\t{hash}\n")
            .as_bytes()
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
fn host_lock_contention_is_nonblocking_and_recoverable() {
    let directory = tempfile::tempdir().unwrap();
    let path = directory.path().join("operation.lock");
    let exclusive = sage_core::HostLock::acquire_exclusive(&path).unwrap();
    assert!(matches!(
        sage_core::HostLock::try_acquire_exclusive(&path),
        Err(sage_core::CoreError::LockBusy(_))
    ));
    assert!(matches!(
        sage_core::HostLock::try_acquire_shared(&path),
        Err(sage_core::CoreError::LockBusy(_))
    ));
    drop(exclusive);
    let first_reader = sage_core::HostLock::try_acquire_shared(&path).unwrap();
    let second_reader = sage_core::HostLock::try_acquire_shared(&path).unwrap();
    assert!(matches!(
        sage_core::HostLock::try_acquire_exclusive(&path),
        Err(sage_core::CoreError::LockBusy(_))
    ));
    drop((first_reader, second_reader));
    sage_core::HostLock::try_acquire_exclusive(&path).unwrap();
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
    lab.publish().unwrap();
    let gate =
        sage_core::HostLock::acquire_exclusive(lab.root().join("run/sage/operation.lock")).unwrap();
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

    // A writer racing with a removal still converges to one explainable serial order.
    let gate =
        sage_core::HostLock::acquire_exclusive(lab.root().join("run/sage/operation.lock")).unwrap();
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
        command: sage::Commands::Verify,
    })
    .await
    .unwrap();
    sage::execute(sage::Cli {
        verbose: false,
        dry_run: false,
        root: lab.root().into(),
        command: sage::Commands::Count,
    })
    .await
    .unwrap();
    std::fs::remove_file(lab.root().join("usr/lib/torture/verify-me")).unwrap();
    assert!(sage::execute(sage::Cli {
        verbose: false,
        dry_run: false,
        root: lab.root().into(),
        command: sage::Commands::Verify,
    })
    .await
    .is_err());
}
