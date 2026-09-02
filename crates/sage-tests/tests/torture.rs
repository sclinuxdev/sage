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
async fn ownership_handoffs_are_ordered_and_cycles_are_atomic() {
    let mut lab = sage_tests::TortureLab::new().unwrap();
    for (name, path, content) in [
        ("z-owner", "etc/torture-handoff.conf", "old-owner"),
        ("a-claimant", "usr/lib/torture/claimant-old", "old-claimant"),
    ] {
        lab.add("system", name, 1, path, content).unwrap();
    }
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
