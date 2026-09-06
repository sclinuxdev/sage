//! End-to-end init transitions using package-owned renderers, commands, and services.
use sage_core::PackageKey;
use sage_tests::{PackageSpec, StateSnapshot, TortureLab};
use std::collections::BTreeMap;
use std::fs;
use std::path::Path;

fn service(name: &str) -> String {
    format!(
        "schema_version=1\n[service]\nname={name:?}\ndescription=\"Fixture daemon\"\ncommand=[\"/usr/bin/daemon\"]\nuser=\"root\"\ngroup=\"root\"\nworking_dir=\"/\"\nrestart=\"no\"\ntype=\"simple\"\n"
    )
}

fn renderer(name: &str, generation: &str) -> String {
    format!(
        "schema_version=1\n[service_generator]\ntarget_path=\"/etc/native-{generation}/${{service.name}}\"\nmode=420\ntemplate=\"{generation}: ${{service.name}}\"\nvalidate_command=\"/usr/bin/{name}ctl ${{SYSROOT}} validate ${{service.name}}\"\nenable_command=\"/usr/bin/{name}ctl ${{SYSROOT}} enable ${{service.name}}\"\ndisable_command=\"/usr/bin/{name}ctl ${{SYSROOT}} disable ${{service.name}}\"\nis_enabled_command=\"/usr/bin/{name}ctl ${{SYSROOT}} is-enabled ${{service.name}}\"\n"
    )
}

fn provider(name: &str, slot: &str, generation: &str) -> PackageSpec {
    let mut package = PackageSpec::new(
        "system",
        name,
        1,
        &format!("usr/share/sage/rclass/init-{name}.toml"),
        &renderer(name, generation),
    );
    package.slot = slot.into();
    package.provides.push("virtual/init".into());
    let program = format!("usr/bin/{name}ctl");
    // The disable operation deliberately fails for inactive services. This
    // distinguishes deleting an old native definition from disabling a service.
    package.files.insert(
        program.clone(),
        format!(
            "#!/bin/sh\nset -eu\nroot=$1\naction=$2\nservice=$3\nmarker=\"$root/var/lib/sage/enabled-{generation}-$service\"\ncase $action in\nvalidate) test -f \"$root/etc/native-{generation}/$service\" ;;\nenable)\n  test ! -f \"$root/var/lib/sage/fail-enable\"\n  printf enabled > \"$marker\"\n  ;;\nis-enabled)\n  if test -f \"$root/var/lib/sage/fail-is-enabled\"; then\n    exit 2\n  fi\n  test -f \"$marker\"\n  ;;\ndisable)\n  test ! -f \"$root/var/lib/sage/fail-disable\"\n  test -f \"$root/etc/native-{generation}/$service\"\n  test -f \"$marker\"\n  /bin/rm \"$marker\"\n  printf '%s\\n' \"{generation}:$service\" >> \"$root/var/lib/sage/disable-log\"\n  ;;\n*) exit 2 ;;\nesac\n"
        )
        .into_bytes(),
    );
    package.executable_files.insert(program);
    package
}

fn configure(lab: &TortureLab, packages: &[&str], init: &str, enabled: &[&str]) {
    fs::write(
        lab.root().join("etc/sage/system.toml"),
        format!(
            "schema_version=1\npackages={packages:?}\n[system]\narchitecture=\"amd64\"\nprofile=\"default\"\n[providers]\ninit={init:?}\n"
        ),
    )
    .unwrap();
    fs::write(
        lab.root().join("etc/sage/services.toml"),
        format!("schema_version=1\nenabled={enabled:?}\n"),
    )
    .unwrap();
}

async fn rebuild(lab: &TortureLab, dry_run: bool) -> anyhow::Result<()> {
    sage::execute(sage::Cli {
        verbose: false,
        dry_run,
        root: lab.root().into(),
        command: sage::Commands::Rebuild { no_prune: false },
    })
    .await
}

fn add_daemons(lab: &mut TortureLab) {
    let mut daemon = PackageSpec::new(
        "system",
        "daemon",
        1,
        "usr/share/sage/services/daemon.toml",
        &service("daemon"),
    );
    daemon.files.insert(
        "usr/share/sage/services/inactive.toml".into(),
        service("inactive").into_bytes(),
    );
    daemon
        .files
        .insert("usr/bin/daemon".into(), b"#!/bin/sh\nexit 0\n".to_vec());
    daemon.executable_files.insert("usr/bin/daemon".into());
    lab.add_package(daemon).unwrap();
}

async fn initial_system() -> TortureLab {
    let mut lab = TortureLab::new().unwrap();
    add_daemons(&mut lab);
    lab.add_package(provider("loom", "0", "old")).unwrap();
    lab.publish().unwrap();
    configure(&lab, &["loom", "daemon"], "loom", &["daemon"]);
    rebuild(&lab, false).await.unwrap();
    assert_eq!(
        fs::read(lab.root().join("etc/native-old/daemon")).unwrap(),
        b"old: daemon"
    );
    assert!(lab.root().join("etc/native-old/inactive").is_file());
    assert!(lab.root().join("var/lib/sage/enabled-old-daemon").is_file());
    assert!(
        !lab.root()
            .join("var/lib/sage/enabled-old-inactive")
            .exists()
    );
    lab
}

fn assert_settled(lab: &TortureLab, name: &str, slot: &str, generation: &str) {
    let key = PackageKey::new("main/system", name, slot);
    let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
    assert_eq!(database.system_provider("init").unwrap(), Some(key.clone()));
    assert_eq!(database.system_provider("sound").unwrap(), None);
    assert!(database.pending_journals().unwrap().is_empty());
    let state: toml::Value = toml::from_str(
        &fs::read_to_string(lab.root().join("var/lib/sage/rendered-services.toml")).unwrap(),
    )
    .unwrap();
    assert_eq!(state["provider"], toml::Value::try_from(&key).unwrap());
    assert_eq!(
        state["enabled"].as_array().unwrap(),
        &[toml::Value::from("daemon")]
    );
    let mut expected_owners = BTreeMap::<String, Vec<String>>::new();
    for package in database.packages().unwrap() {
        for path in package.files {
            expected_owners
                .entry(path)
                .or_default()
                .push(package.key.canonical_id());
        }
    }
    for owners in expected_owners.values_mut() {
        owners.sort();
    }
    drop(database);
    let snapshot = lab.snapshot().unwrap();
    assert_eq!(snapshot.owners, expected_owners);
    assert_eq!(
        fs::read(lab.root().join(format!("etc/native-{generation}/daemon"))).unwrap(),
        format!("{generation}: daemon").as_bytes()
    );
    assert!(
        lab.root()
            .join(format!("etc/native-{generation}/inactive"))
            .is_file()
    );
    assert!(
        lab.root()
            .join(format!("var/lib/sage/enabled-{generation}-daemon"))
            .is_file()
    );
    assert!(
        !lab.root()
            .join(format!("var/lib/sage/enabled-{generation}-inactive"))
            .exists()
    );
}

#[derive(Debug, PartialEq, Eq)]
struct LifecycleSnapshot {
    packages: StateSnapshot,
    bindings: BTreeMap<String, Option<PackageKey>>,
    state: Vec<u8>,
    outputs: Vec<Option<Vec<u8>>>,
}

fn lifecycle_snapshot(lab: &TortureLab) -> LifecycleSnapshot {
    let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
    assert!(database.pending_journals().unwrap().is_empty());
    let bindings = ["init", "sound"]
        .into_iter()
        .map(|interface| {
            (
                interface.to_owned(),
                database.system_provider(interface).unwrap(),
            )
        })
        .collect();
    drop(database);
    LifecycleSnapshot {
        packages: lab.snapshot().unwrap(),
        bindings,
        state: fs::read(lab.root().join("var/lib/sage/rendered-services.toml")).unwrap(),
        outputs: [
            "etc/native-old/daemon",
            "etc/native-old/inactive",
            "etc/native-new/daemon",
            "etc/native-new/inactive",
            "var/lib/sage/enabled-old-daemon",
            "var/lib/sage/enabled-new-daemon",
            "var/lib/sage/disable-log",
        ]
        .iter()
        .map(|path| fs::read(lab.root().join(path)).ok())
        .collect(),
    }
}

#[tokio::test]
async fn renderer_uses_the_solver_resolved_provider_and_full_slot_identity() {
    let mut lab = initial_system().await;
    lab.add_package(provider("loom", "1", "new")).unwrap();
    let mut selector = PackageSpec::new(
        "system",
        "select-new-init",
        1,
        "usr/lib/torture/selector",
        "select loom slot 1",
    );
    selector.dependencies.push("loom:1".into());
    selector.conflicts.push("loom:0".into());
    lab.add_package(selector).unwrap();
    lab.publish().unwrap();
    // The requested name has no slot; dependency/conflict solving selects slot 1.
    configure(&lab, &["daemon", "select-new-init"], "loom", &["daemon"]);
    rebuild(&lab, false).await.unwrap();
    assert_settled(&lab, "loom", "1", "new");
    assert!(
        !lab.snapshot()
            .unwrap()
            .packages
            .contains_key("main/system:loom:0")
    );
    assert!(!lab.root().join("etc/native-old/daemon").exists());
    assert!(!lab.root().join("etc/native-old/inactive").exists());
    assert!(!lab.root().join("var/lib/sage/enabled-old-daemon").exists());
    assert_eq!(
        fs::read(lab.root().join("var/lib/sage/disable-log")).unwrap(),
        b"old:daemon\n"
    );
}

#[tokio::test]
async fn ordinary_remove_rejects_the_bound_provider_even_with_an_installed_alternative() {
    let mut lab = initial_system().await;
    lab.add_package(provider("fir", "0", "new")).unwrap();
    lab.publish().unwrap();
    lab.install("fir", "system").await.unwrap();
    let before = lifecycle_snapshot(&lab);
    let config = fs::read(lab.root().join("etc/sage/system.toml")).unwrap();
    for dry_run in [true, false] {
        let error = sage::execute(sage::Cli {
            verbose: false,
            dry_run,
            root: lab.root().into(),
            command: sage::Commands::Remove {
                packages: vec!["loom".into()],
                channel: Some("system".into()),
            },
        })
        .await
        .unwrap_err();
        assert!(error.to_string().contains("bound provider"), "{error:#}");
        assert_eq!(lifecycle_snapshot(&lab), before);
        assert_eq!(
            fs::read(lab.root().join("etc/sage/system.toml")).unwrap(),
            config
        );
    }
    lab.remove("fir", "system").await.unwrap();
    assert_settled(&lab, "loom", "0", "old");
}

#[tokio::test]
async fn invalid_planned_renderers_services_and_commands_preserve_the_working_system() {
    let good = renderer("loom", "new");
    let invalid = [
        ("missing renderer", None),
        ("malformed renderer", Some("invalid TOML [".into())),
        (
            "unsafe output",
            Some(good.replace("/etc/native-new/", "../../etc/")),
        ),
        (
            "output overwrites provider executable",
            Some(good.replace("/etc/native-new/${service.name}", "/usr/bin/loomctl")),
        ),
        (
            "duplicate native output",
            Some(good.replace("/etc/native-new/${service.name}", "/etc/native-new/shared")),
        ),
        ("symlink escaped output", Some(good.clone())),
        (
            "unknown template variable",
            Some(good.replace("new: ${service.name}", "${missing}")),
        ),
        (
            "unsupported service type",
            Some(format!("{good}supported_types=[\"forking\"]\n")),
        ),
        (
            "missing validator",
            Some(good.replace(
                "/usr/bin/loomctl ${SYSROOT} validate",
                "/usr/bin/missing ${SYSROOT} validate",
            )),
        ),
        (
            "missing enable command",
            Some(good.replace(
                "/usr/bin/loomctl ${SYSROOT} enable",
                "/usr/bin/missing ${SYSROOT} enable",
            )),
        ),
        (
            "missing disable command",
            Some(good.replace(
                "/usr/bin/loomctl ${SYSROOT} disable",
                "/usr/bin/missing ${SYSROOT} disable",
            )),
        ),
        (
            "missing compiler",
            Some(format!(
                "{good}compile_command=[\"/usr/bin/missing\",\"${{INPUT}}\",\"${{OUTPUT}}\"]\n"
            )),
        ),
        ("nonexecutable replacement", Some(good.clone())),
        ("retired live command", Some(good.clone())),
        ("malformed service", Some(good.clone())),
        ("invalid service", Some(good.clone())),
        ("missing enabled declaration", Some(good)),
    ];
    for (case, class) in invalid {
        let mut lab = initial_system().await;
        let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
        database
            .set_system_provider("sound", &PackageKey::new("main/system", "loom", "0"))
            .unwrap();
        drop(database);
        let outside = tempfile::tempdir().unwrap();
        if case == "symlink escaped output" {
            std::os::unix::fs::symlink(outside.path(), lab.root().join("etc/native-new")).unwrap();
        }
        let mut replacement = provider("loom", "0", "new");
        replacement.version = 2;
        let class_path = "usr/share/sage/rclass/init-loom.toml";
        replacement.files.remove(class_path);
        if let Some(class) = class {
            replacement
                .files
                .insert(class_path.into(), class.into_bytes());
        }
        if case == "nonexecutable replacement" {
            replacement.executable_files.clear();
        } else if case == "retired live command" {
            replacement.files.remove("usr/bin/loomctl");
            replacement.executable_files.clear();
        } else if case == "malformed service" || case == "invalid service" {
            replacement.files.insert(
                "usr/share/sage/services/invalid.toml".into(),
                if case == "malformed service" {
                    b"invalid TOML [".to_vec()
                } else {
                    service("invalid")
                        .replace("type=\"simple\"", "type=\"invalid\"")
                        .into_bytes()
                },
            );
        }
        lab.add_package(replacement).unwrap();
        let mut force = PackageSpec::new(
            "system",
            "force-renderer",
            1,
            "usr/lib/torture/force-renderer",
            "force replacement",
        );
        force.dependencies.push("loom >= 2-1".into());
        lab.add_package(force).unwrap();
        lab.publish().unwrap();
        configure(
            &lab,
            &["loom", "daemon", "force-renderer"],
            "loom",
            if case == "missing enabled declaration" {
                &["missing"]
            } else {
                &["daemon"]
            },
        );
        let before = lifecycle_snapshot(&lab);
        let result = rebuild(&lab, false).await;
        assert!(result.is_err(), "invalid {case} accepted");
        let after = lifecycle_snapshot(&lab);
        assert!(
            after == before,
            "{case} changed lifecycle state: {result:?}; packages before={:?}, after={:?}",
            before.packages.packages,
            after.packages.packages
        );
        assert_eq!(fs::read_dir(outside.path()).unwrap().count(), 0, "{case}");
    }
}

#[tokio::test]
async fn dry_run_previews_a_valid_uninstalled_provider_without_mutation() {
    let mut lab = initial_system().await;
    lab.add_package(provider("fir", "0", "new")).unwrap();
    lab.publish().unwrap();
    configure(&lab, &["fir", "daemon"], "fir", &["daemon"]);
    let before = lifecycle_snapshot(&lab);
    rebuild(&lab, true).await.unwrap();
    assert_eq!(lifecycle_snapshot(&lab), before);
}

#[tokio::test]
async fn an_empty_service_set_still_rejects_a_managed_directory_under_a_planned_executable() {
    let mut lab = initial_system().await;
    let mut replacement = provider("fir", "0", "new");
    replacement.files.insert(
        "usr/share/sage/rclass/init-fir.toml".into(),
        format!(
            "{}managed_directory=\"/usr/bin/firctl/definitions\"\n",
            renderer("fir", "new")
        )
        .into_bytes(),
    );
    lab.add_package(replacement).unwrap();
    lab.publish().unwrap();
    configure(&lab, &["fir"], "fir", &[]);
    let before = lifecycle_snapshot(&lab);
    let result = rebuild(&lab, false).await;
    assert!(
        result.is_err(),
        "invalid empty managed directory was accepted"
    );
    assert!(
        lifecycle_snapshot(&lab) == before,
        "invalid empty managed directory changed lifecycle state: {result:?}"
    );
}

#[tokio::test]
async fn switching_removes_inactive_definitions_without_disabling_them() {
    let mut lab = initial_system().await;
    lab.add_package(provider("fir", "0", "new")).unwrap();
    lab.publish().unwrap();
    let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
    database
        .set_system_provider("sound", &PackageKey::new("main/system", "loom", "0"))
        .unwrap();
    drop(database);
    // Retain the old provider so a missing renderer cannot hide an erroneous
    // disable call for the installed but inactive declaration.
    configure(&lab, &["loom", "fir", "daemon"], "fir", &["daemon"]);
    rebuild(&lab, false).await.unwrap();
    assert_settled(&lab, "fir", "0", "new");
    assert!(!lab.root().join("etc/native-old/daemon").exists());
    assert!(!lab.root().join("etc/native-old/inactive").exists());
    assert_eq!(
        fs::read(lab.root().join("var/lib/sage/disable-log")).unwrap(),
        b"old:daemon\n"
    );
}

#[tokio::test]
async fn adding_an_unrelated_package_keeps_existing_services_enabled() {
    let mut lab = initial_system().await;
    lab.add(
        "system",
        "unrelated",
        1,
        "usr/lib/torture/unrelated",
        "unrelated",
    )
    .unwrap();
    lab.publish().unwrap();
    configure(&lab, &["loom", "daemon", "unrelated"], "loom", &["daemon"]);
    rebuild(&lab, false).await.unwrap();
    assert_settled(&lab, "loom", "0", "old");
    assert!(!lab.root().join("var/lib/sage/disable-log").exists());
}

#[tokio::test]
async fn rebuild_pruning_preserves_captured_removal_triggers_and_their_recovery_checkpoint() {
    for checkpoint in [
        None,
        Some("triggers"),
        Some("rebuild-removal-triggers"),
        Some("rebuild-bindings"),
    ] {
        let mut lab = initial_system().await;
        let counter = lab.install_counting_trigger().unwrap();
        let admin_trigger = lab.root().join("etc/sage/triggers.d/torture.toml");
        let declaration = fs::read_to_string(&admin_trigger)
            .unwrap()
            .replace("post-change", "post-remove")
            .replace(
                "exec=[\"/usr/bin/torture-trigger\"]",
                "exec=[\"/usr/bin/torture-trigger\",\"${path}\"]",
            );
        fs::remove_file(admin_trigger).unwrap();
        fs::write(
            lab.root().join("usr/bin/torture-trigger"),
            "#!/bin/sh\nset -eu\ntest \"$1\" = usr/lib/torture/retired\ntest ! -e \"$SAGE_SYSROOT/$1\"\ntest -f \"$SAGE_SYSROOT/usr/lib/torture/retained\"\ntest -f \"$SAGE_SYSROOT/usr/lib/torture/added\"\nprintf x >> \"$SAGE_SYSROOT/var/lib/sage/torture-trigger-count\"\n",
        )
        .unwrap();
        let mut retired = PackageSpec::new(
            "system",
            "retired",
            1,
            "usr/lib/torture/retired",
            "retired payload",
        );
        retired.files.insert(
            "usr/lib/torture/retained".into(),
            b"shared payload".to_vec(),
        );
        retired.files.insert(
            "usr/share/sage/triggers/retired.toml".into(),
            declaration.into_bytes(),
        );
        lab.add_package(retired).unwrap();
        lab.publish().unwrap();
        lab.install("retired", "system").await.unwrap();
        assert!(!counter.exists());
        let mut claimant = PackageSpec::new(
            "system",
            "claimant",
            1,
            "usr/lib/torture/retained",
            "shared payload",
        );
        claimant
            .files
            .insert("usr/lib/torture/added".into(), b"added payload".to_vec());
        lab.add_package(claimant).unwrap();
        lab.add(
            "system",
            "unrelated",
            1,
            "usr/lib/torture/unrelated",
            "unrelated",
        )
        .unwrap();
        lab.publish().unwrap();
        configure(&lab, &["loom", "daemon", "claimant"], "loom", &["daemon"]);
        if let Some(checkpoint) = checkpoint {
            lab.inject(&format!("abort:{checkpoint}")).unwrap();
            let crashed = aborting_rebuild(lab.root());
            assert!(!crashed.status.success(), "{checkpoint}");
            assert!(
                !lab.root().join("run/sage/crash-point").exists(),
                "checkpoint {checkpoint} was not reached: {}",
                String::from_utf8_lossy(&crashed.stderr)
            );
            if checkpoint != "triggers" {
                assert_eq!(fs::read(&counter).unwrap(), b"x", "{checkpoint}");
            }
            lab.install("unrelated", "system")
                .await
                .unwrap_or_else(|error| panic!("recover {checkpoint}: {error:#}"));
        } else {
            rebuild(&lab, false).await.unwrap();
        }
        assert_settled(&lab, "loom", "0", "old");
        assert!(!lab.root().join("usr/lib/torture/retired").exists());
        assert!(lab.root().join("usr/lib/torture/retained").is_file());
        assert!(lab.root().join("usr/lib/torture/added").is_file());
        assert!(
            !lab.root()
                .join("usr/share/sage/triggers/retired.toml")
                .exists()
        );
        let snapshot = lab.snapshot().unwrap();
        assert!(!snapshot.packages.contains_key("main/system:retired:0"));
        assert_eq!(
            snapshot.owners["usr/lib/torture/retained"],
            ["main/system:claimant:0"]
        );
        assert_eq!(fs::read(&counter).unwrap(), b"x", "{checkpoint:?}");
    }
}

#[tokio::test]
async fn rebuild_rejects_a_required_removal_trigger_executable_that_will_be_retired() {
    let mut lab = initial_system().await;
    let mut retired = PackageSpec::new(
        "system",
        "retired",
        1,
        "usr/bin/retired-recorder",
        "#!/bin/sh\nprintf x >> \"$SAGE_SYSROOT/var/lib/sage/retired-trigger-count\"\n",
    );
    retired
        .executable_files
        .insert("usr/bin/retired-recorder".into());
    retired.files.insert(
        "usr/lib/torture/retired".into(),
        b"retired payload".to_vec(),
    );
    retired.files.insert(
        "usr/share/sage/triggers/retired.toml".into(),
        b"schema_version=1\nname=\"retired-recorder\"\ndescription=\"Required retired command\"\non_paths=[\"usr/lib/torture/retired\"]\nexec=[\"/usr/bin/retired-recorder\"]\npriority=1\nevents=[\"post-remove\"]\nignore_missing_binary=false\n".to_vec(),
    );
    lab.add_package(retired).unwrap();
    lab.publish().unwrap();
    lab.install("retired", "system").await.unwrap();
    let before = lifecycle_snapshot(&lab);
    let result = rebuild(&lab, false).await;
    assert!(result.is_err(), "retired trigger executable was accepted");
    assert!(
        lifecycle_snapshot(&lab) == before,
        "rejected pruning changed the working system: {result:?}"
    );
    assert!(
        !lab.root()
            .join("var/lib/sage/retired-trigger-count")
            .exists()
    );
}

#[tokio::test]
async fn preserved_administrator_config_does_not_activate_a_retired_removal_trigger() {
    let mut lab = initial_system().await;
    let mut retired = PackageSpec::new(
        "system",
        "retired",
        1,
        "etc/retired.conf",
        "packaged configuration",
    );
    retired.files.insert(
        "usr/bin/retired-tool".into(),
        b"#!/bin/sh\nprintf x >> \"$SAGE_SYSROOT/var/lib/sage/retired-trigger-count\"\nexit 99\n"
            .to_vec(),
    );
    retired
        .executable_files
        .insert("usr/bin/retired-tool".into());
    retired.files.insert(
        "usr/share/sage/triggers/retired.toml".into(),
        b"schema_version=1\nname=\"retired-config\"\ndescription=\"Only removed configuration activates this command\"\non_paths=[\"etc/retired.conf\"]\nexec=[\"/usr/bin/retired-tool\"]\npriority=1\nevents=[\"post-remove\"]\nignore_missing_binary=false\n".to_vec(),
    );
    lab.add_package(retired).unwrap();
    lab.publish().unwrap();
    lab.install("retired", "system").await.unwrap();
    fs::write(lab.root().join("etc/retired.conf"), b"administrator change").unwrap();
    rebuild(&lab, false).await.unwrap();
    assert_settled(&lab, "loom", "0", "old");
    let snapshot = lab.snapshot().unwrap();
    assert!(!snapshot.packages.contains_key("main/system:retired:0"));
    assert!(!snapshot.owners.contains_key("etc/retired.conf"));
    assert_eq!(
        fs::read(lab.root().join("etc/retired.conf")).unwrap(),
        b"administrator change"
    );
    assert!(!lab.root().join("usr/bin/retired-tool").exists());
    assert!(
        !lab.root()
            .join("usr/share/sage/triggers/retired.toml")
            .exists()
    );
    assert!(
        !lab.root()
            .join("var/lib/sage/retired-trigger-count")
            .exists()
    );
}

/// A fresh process runs this one test only, so abort injection cannot kill the
/// parent harness or carry in-memory renderer state into the recovery command.
#[test]
fn lifecycle_rebuild_worker() {
    let Some(root) = std::env::var_os("SAGE_LIFECYCLE_TEST_ROOT") else {
        return;
    };
    tokio::runtime::Runtime::new()
        .unwrap()
        .block_on(sage::execute(sage::Cli {
            verbose: false,
            dry_run: false,
            root: root.into(),
            command: sage::Commands::Rebuild { no_prune: false },
        }))
        .unwrap();
}

fn aborting_rebuild(root: &Path) -> std::process::Output {
    std::process::Command::new(std::env::current_exe().unwrap())
        .args([
            "--exact",
            "lifecycle_rebuild_worker",
            "--nocapture",
            "--test-threads=1",
        ])
        .env("SAGE_LIFECYCLE_TEST_ROOT", root)
        .output()
        .unwrap()
}

#[tokio::test]
async fn cleanup_keeps_old_commands_and_runtime_alive_and_recovery_finishes_the_rebuild() {
    for point in [
        "rebuild-cleanup",
        "rebuild-cleanup-complete",
        "extraction",
        "lmdb-publication",
        "rebuild-bindings",
        "rebuild-services",
        "rebuild-triggers",
    ] {
        let mut lab = TortureLab::new().unwrap();
        add_daemons(&mut lab);
        lab.add(
            "system",
            "control-runtime",
            1,
            "usr/lib/torture/control-runtime",
            "generation=old\n",
        )
        .unwrap();
        let mut old = provider("loom", "0", "old");
        old.dependencies.push("control-runtime >= 1-1".into());
        let program = old.files.get_mut("usr/bin/loomctl").unwrap();
        *program = String::from_utf8(program.clone()).unwrap().replace(
            "disable)\n",
            "disable)\n  . \"$root/usr/lib/torture/control-runtime\"\n  test \"$generation\" = old\n  test ! -e \"$root/usr/bin/firctl\"\n",
        ).into_bytes();
        lab.add_package(old).unwrap();
        lab.publish().unwrap();
        configure(&lab, &["loom", "daemon"], "loom", &["daemon"]);
        rebuild(&lab, false).await.unwrap();
        lab.add(
            "system",
            "control-runtime",
            2,
            "usr/lib/torture/control-runtime",
            "generation=new\n",
        )
        .unwrap();
        let mut new = provider("fir", "0", "new");
        new.dependencies.push("control-runtime >= 2-1".into());
        lab.add_package(new).unwrap();
        lab.add(
            "system",
            "unrelated",
            1,
            "usr/lib/torture/unrelated",
            "unrelated",
        )
        .unwrap();
        lab.publish().unwrap();
        // Only the Rebuild event writes this marker; normal package triggers
        // cannot accidentally satisfy the continuation assertion.
        let trigger = lab.root().join("etc/sage/triggers.d/rebuild-tail.toml");
        fs::create_dir_all(trigger.parent().unwrap()).unwrap();
        fs::write(&trigger, "schema_version=1\nname=\"rebuild-tail\"\ndescription=\"Record completed rebuild tail\"\non_paths=[\"etc/sage/system.toml\"]\nexec=[\"/usr/bin/rebuild-tail\"]\npriority=1\nevents=[\"rebuild\"]\nignore_missing_binary=false\n").unwrap();
        let tail = lab.root().join("usr/bin/rebuild-tail");
        fs::write(
            &tail,
            "#!/bin/sh\nprintf x >> \"$SAGE_SYSROOT/var/lib/sage/rebuild-tail-count\"\n",
        )
        .unwrap();
        use std::os::unix::fs::PermissionsExt as _;
        fs::set_permissions(&tail, fs::Permissions::from_mode(0o755)).unwrap();
        configure(&lab, &["fir", "daemon"], "fir", &["daemon"]);
        lab.inject(&format!("abort:{point}")).unwrap();
        let crashed = aborting_rebuild(lab.root());
        assert!(
            !crashed.status.success(),
            "checkpoint {point} did not interrupt rebuild"
        );
        assert!(
            !lab.root().join("run/sage/crash-point").exists(),
            "checkpoint {point} was never reached: {}",
            String::from_utf8_lossy(&crashed.stderr)
        );
        // Change the live configuration after the crash. Recovery must finish
        // the recorded transition instead of replanning the unrelated command.
        configure(&lab, &["loom", "daemon"], "loom", &["daemon"]);
        lab.install("unrelated", "system")
            .await
            .unwrap_or_else(|error| panic!("recover {point}: {error:#}"));
        assert_settled(&lab, "fir", "0", "new");
        let state = lab.snapshot().unwrap();
        assert_eq!(state.packages["main/system:control-runtime:0"], "2-1");
        assert!(state.packages.contains_key("main/system:unrelated:0"));
        assert!(!state.packages.contains_key("main/system:loom:0"));
        assert!(!lab.root().join("usr/bin/loomctl").exists());
        assert!(
            !lab.root()
                .join("usr/share/sage/rclass/init-loom.toml")
                .exists()
        );
        assert!(!lab.root().join("etc/native-old/daemon").exists());
        assert!(!lab.root().join("etc/native-old/inactive").exists());
        assert!(!lab.root().join("var/lib/sage/enabled-old-daemon").exists());
        assert_eq!(
            fs::read(lab.root().join("var/lib/sage/disable-log")).unwrap(),
            b"old:daemon\n",
            "{point}"
        );
        assert_eq!(
            fs::read(lab.root().join("var/lib/sage/rebuild-tail-count")).unwrap(),
            b"x",
            "{point}"
        );
    }
}

#[tokio::test]
async fn configured_provider_symbols_survive_rebuild_and_recovery() {
    for (spelling, symbol, binding) in [
        ("libc", "virtual/libc", "libc"),
        ("virtual/libc", "virtual/libc", "libc"),
        ("so:libc.so.6", "so:libc.so.6", "so:libc.so.6"),
    ] {
        let mut lab = initial_system().await;
        let mut library = PackageSpec::new(
            "system",
            "libc-provider",
            1,
            "usr/lib/torture/libc-provider",
            "library",
        );
        library.slot = "1".into();
        library.provides.push(symbol.into());
        lab.add_package(library).unwrap();
        lab.publish().unwrap();
        configure(&lab, &["loom", "daemon"], "loom", &["daemon"]);
        let config_path = lab.root().join("etc/sage/system.toml");
        let config = format!(
            "{}\n{spelling:?}=\"libc-provider:1\"\n",
            fs::read_to_string(&config_path).unwrap()
        );
        fs::write(&config_path, &config).unwrap();
        let before = lifecycle_snapshot(&lab);
        rebuild(&lab, true).await.unwrap();
        assert_eq!(lifecycle_snapshot(&lab), before);
        lab.inject("rebuild-bindings").unwrap();
        assert!(
            rebuild(&lab, false)
                .await
                .unwrap_err()
                .to_string()
                .contains("injected crash")
        );
        configure(&lab, &["loom", "daemon"], "loom", &["daemon"]);
        lab.install("daemon", "system").await.unwrap();
        let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
        assert_eq!(
            database.system_provider(binding).unwrap(),
            Some(PackageKey::new("main/system", "libc-provider", "1"))
        );
        assert_eq!(
            database
                .system_provider(&format!("virtual/{binding}"))
                .unwrap(),
            None
        );
        assert!(database.pending_journals().unwrap().is_empty());
        drop(database);
        let error = lab.remove("libc-provider:1", "system").await.unwrap_err();
        assert!(
            error.to_string().contains(&format!("for {symbol};")),
            "{error:#}"
        );
        rebuild(&lab, false).await.unwrap();
        let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
        assert_eq!(database.system_provider(binding).unwrap(), None);
    }
}

#[tokio::test]
async fn retired_file_to_directory_handoffs_recover_without_repeating_retirement() {
    for (point, symlink) in [
        (None, false),
        (None, true),
        (Some("removal"), false),
        (Some("rebuild-retirement"), false),
        (Some("extraction"), false),
        (Some("before-lmdb-write"), false),
        (Some("lmdb-publication"), false),
    ] {
        let mut lab = initial_system().await;
        lab.add("system", "old-tool", 1, "usr/lib/torture/tool", "old file")
            .unwrap();
        lab.publish().unwrap();
        lab.install("old-tool", "system").await.unwrap();
        let outside = tempfile::tempdir().unwrap();
        fs::write(outside.path().join("sentinel"), b"outside").unwrap();
        if symlink {
            let old = lab.root().join("usr/lib/torture/tool");
            fs::remove_file(&old).unwrap();
            std::os::unix::fs::symlink(outside.path(), old).unwrap();
        }
        lab.add(
            "system",
            "new-tool",
            1,
            "usr/lib/torture/tool/helper",
            "new helper",
        )
        .unwrap();
        lab.publish().unwrap();
        configure(&lab, &["loom", "daemon", "new-tool"], "loom", &["daemon"]);
        if let Some(point) = point {
            lab.inject(&format!("abort:{point}")).unwrap();
            let interrupted = aborting_rebuild(lab.root());
            assert!(!interrupted.status.success());
            assert!(
                !lab.root().join("run/sage/crash-point").exists(),
                "{point}: {}",
                String::from_utf8_lossy(&interrupted.stderr)
            );
            lab.install("daemon", "system")
                .await
                .unwrap_or_else(|error| panic!("{point}: {error:#}"));
        } else {
            rebuild(&lab, false).await.unwrap();
        }
        assert_settled(&lab, "loom", "0", "old");
        assert_eq!(
            fs::read(outside.path().join("sentinel")).unwrap(),
            b"outside"
        );
        assert!(!outside.path().join("helper").exists());
        let state = lab.snapshot().unwrap();
        assert!(!state.packages.contains_key("main/system:old-tool:0"));
        assert!(lab.root().join("usr/lib/torture/tool").is_dir());
        assert_eq!(
            fs::read(lab.root().join("usr/lib/torture/tool/helper")).unwrap(),
            b"new helper"
        );
        assert!(!state.owners.contains_key("usr/lib/torture/tool"));
        assert_eq!(
            state.owners["usr/lib/torture/tool/helper"],
            ["main/system:new-tool:0"]
        );
    }
}

#[tokio::test]
async fn renderer_programs_and_outputs_use_directories_created_by_retirement_handoffs() {
    let mut lab = initial_system().await;
    lab.add("system", "old-tool", 1, "usr/lib/torture/tool", "old file")
        .unwrap();
    lab.publish().unwrap();
    lab.install("old-tool", "system").await.unwrap();
    let mut new = provider("fir", "0", "new");
    let program = new.files.remove("usr/bin/firctl").unwrap();
    new.files.insert(
        "usr/lib/torture/tool/control".into(),
        String::from_utf8(program)
            .unwrap()
            .replace("/etc/native-new", "/usr/lib/torture/tool/native")
            .into_bytes(),
    );
    new.executable_files = ["usr/lib/torture/tool/control".into()]
        .into_iter()
        .collect();
    new.files.insert(
        "usr/share/sage/rclass/init-fir.toml".into(),
        renderer("fir", "new")
            .replace("/usr/bin/firctl", "/usr/lib/torture/tool/control")
            .replace("/etc/native-new", "/usr/lib/torture/tool/native")
            .into_bytes(),
    );
    lab.add_package(new).unwrap();
    lab.publish().unwrap();
    configure(&lab, &["fir", "daemon"], "fir", &["daemon"]);
    rebuild(&lab, false).await.unwrap();
    let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
    assert_eq!(
        database.system_provider("init").unwrap(),
        Some(PackageKey::new("main/system", "fir", "0"))
    );
    assert!(database.pending_journals().unwrap().is_empty());
    assert_eq!(
        database.owners("usr/lib/torture/tool/control").unwrap(),
        [PackageKey::new("main/system", "fir", "0")]
    );
    assert_eq!(
        fs::read(lab.root().join("usr/lib/torture/tool/native/daemon")).unwrap(),
        b"new: daemon"
    );
    assert!(lab.root().join("var/lib/sage/enabled-new-daemon").is_file());
    assert!(!lab.root().join("etc/native-old/daemon").exists());
}

#[tokio::test]
async fn hierarchy_handoffs_keep_retained_and_preserved_paths_protected() {
    for case in [
        "retained",
        "shared",
        "modified-config",
        "unowned-symlink",
        "directory-to-file",
    ] {
        let mut lab = initial_system().await;
        let outside = tempfile::tempdir().unwrap();
        fs::write(outside.path().join("sentinel"), b"outside").unwrap();
        let path = if case == "modified-config" {
            "etc/torture-tool"
        } else {
            "usr/lib/torture/tool"
        };
        if case == "unowned-symlink" {
            fs::create_dir_all(lab.root().join("usr/lib/torture")).unwrap();
            std::os::unix::fs::symlink(outside.path(), lab.root().join(path)).unwrap();
        } else {
            let owned = if case == "directory-to-file" {
                format!("{path}/old-helper")
            } else {
                path.into()
            };
            lab.add("system", "old-tool", 1, &owned, "original")
                .unwrap();
            lab.publish().unwrap();
            lab.install("old-tool", "system").await.unwrap();
        }
        if case == "modified-config" {
            fs::write(lab.root().join(path), b"administrator edit").unwrap();
        }
        if case == "shared" {
            let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
            let mut owner = database
                .package(&PackageKey::new("main/system", "old-tool", "0"))
                .unwrap()
                .unwrap();
            owner.key = PackageKey::new("main/system", "retained-owner", "0");
            database.install(&owner, true).unwrap();
        }
        let new_path = if case == "directory-to-file" {
            path.into()
        } else {
            format!("{path}/helper")
        };
        lab.add("system", "new-tool", 1, &new_path, "new helper")
            .unwrap();
        lab.publish().unwrap();
        let mut desired = vec!["loom", "daemon", "new-tool"];
        if case == "retained" {
            desired.push("old-tool");
        }
        if case == "shared" {
            desired.push("retained-owner");
        }
        configure(&lab, &desired, "loom", &["daemon"]);
        let before = lifecycle_snapshot(&lab);
        let error = rebuild(&lab, false).await.unwrap_err();
        assert_eq!(lifecycle_snapshot(&lab), before, "{case}: {error:#}");
        assert_eq!(
            fs::read(outside.path().join("sentinel")).unwrap(),
            b"outside"
        );
        assert!(!outside.path().join("helper").exists());
        if case == "modified-config" {
            assert_eq!(
                fs::read(lab.root().join(path)).unwrap(),
                b"administrator edit"
            );
        }
    }
}

#[tokio::test]
async fn dry_run_removal_reads_bindings_without_writing_the_database() {
    use std::os::unix::fs::PermissionsExt as _;
    let lab = initial_system().await;
    let data = lab.root().join("var/lib/sage/data.mdb");
    let before = fs::read(&data).unwrap();
    let permissions = fs::metadata(&data).unwrap().permissions();
    fs::set_permissions(&data, fs::Permissions::from_mode(0o444)).unwrap();
    let preview = |package: &str| {
        sage::execute(sage::Cli {
            verbose: false,
            dry_run: true,
            root: lab.root().into(),
            command: sage::Commands::Remove {
                packages: vec![package.into()],
                channel: Some("system".into()),
            },
        })
    };
    let removable = preview("daemon").await;
    let bound = preview("loom").await;
    let after = fs::read(&data).unwrap();
    fs::set_permissions(&data, permissions).unwrap();
    removable.unwrap();
    assert!(bound.unwrap_err().to_string().contains("bound provider"));
    assert_eq!(after, before);
    assert_settled(&lab, "loom", "0", "old");
}

fn native_handoff_provider(directory: &str, managed: bool) -> PackageSpec {
    let mut package = provider("fir", "0", "new");
    for bytes in package.files.values_mut() {
        *bytes = String::from_utf8(bytes.clone())
            .unwrap()
            .replace("/etc/native-new", directory)
            .into_bytes();
    }
    if managed {
        package
            .files
            .get_mut("usr/share/sage/rclass/init-fir.toml")
            .unwrap()
            .extend_from_slice(format!("managed_directory={directory:?}\n").as_bytes());
    }
    package
}

#[tokio::test]
async fn native_outputs_create_directories_after_retirement_without_payload_children() {
    for (managed, empty, checkpoint) in [
        (false, false, None),
        (false, false, Some("rebuild-retirement")),
        (true, false, None),
        (true, false, Some("rebuild-services")),
        (true, true, None),
        (true, true, Some("rebuild-bindings")),
    ] {
        let mut lab = initial_system().await;
        lab.add("system", "old-layout", 1, "etc/init", "old layout")
            .unwrap();
        lab.publish().unwrap();
        lab.install("old-layout", "system").await.unwrap();
        // The managed empty generation covers replacing the directory itself;
        // nonempty managed output also covers a parent of the managed directory.
        let directory = if managed && !empty {
            "/etc/init/services"
        } else {
            "/etc/init"
        };
        lab.add_package(native_handoff_provider(directory, managed))
            .unwrap();
        lab.publish().unwrap();
        configure(
            &lab,
            if empty { &["fir"] } else { &["fir", "daemon"] },
            "fir",
            if empty { &[] } else { &["daemon"] },
        );
        if let Some(point) = checkpoint {
            lab.inject(&format!("abort:{point}")).unwrap();
            let interrupted = aborting_rebuild(lab.root());
            assert!(!interrupted.status.success());
            assert!(
                !lab.root().join("run/sage/crash-point").exists(),
                "{point}: {}",
                String::from_utf8_lossy(&interrupted.stderr)
            );
            lab.install("fir", "system").await.unwrap();
        } else {
            rebuild(&lab, false).await.unwrap();
        }
        let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
        assert_eq!(
            database.system_provider("init").unwrap(),
            Some(PackageKey::new("main/system", "fir", "0"))
        );
        assert!(
            database
                .package(&PackageKey::new("main/system", "old-layout", "0"))
                .unwrap()
                .is_none()
        );
        assert!(database.owners("etc/init").unwrap().is_empty());
        assert!(database.pending_journals().unwrap().is_empty());
        let output = lab.root().join(directory.trim_start_matches('/'));
        assert!(output.is_dir());
        if empty {
            assert_eq!(fs::read_dir(output).unwrap().count(), 0);
        } else {
            assert_eq!(fs::read(output.join("daemon")).unwrap(), b"new: daemon");
            assert!(lab.root().join("var/lib/sage/enabled-new-daemon").is_file());
        }
        assert!(!lab.root().join("etc/native-old/daemon").exists());
        assert!(!lab.root().join("var/lib/sage/enabled-old-daemon").exists());
    }
}

#[tokio::test]
async fn native_directory_handoffs_reject_protected_ancestors_before_publication() {
    for managed in [false, true] {
        for case in [
            "modified-config",
            "retained-owner",
            "shared-owner",
            "unowned-symlink",
        ] {
            let mut lab = initial_system().await;
            let outside = tempfile::tempdir().unwrap();
            fs::write(outside.path().join("sentinel"), b"outside").unwrap();
            if case == "unowned-symlink" {
                std::os::unix::fs::symlink(outside.path(), lab.root().join("etc/init")).unwrap();
            } else {
                lab.add("system", "old-layout", 1, "etc/init", "old layout")
                    .unwrap();
                lab.publish().unwrap();
                lab.install("old-layout", "system").await.unwrap();
            }
            if case == "modified-config" {
                fs::write(lab.root().join("etc/init"), b"administrator edit").unwrap();
            }
            if case == "shared-owner" {
                let database =
                    sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
                let mut shared = database
                    .package(&PackageKey::new("main/system", "old-layout", "0"))
                    .unwrap()
                    .unwrap();
                shared.key = PackageKey::new("main/system", "shared-layout", "0");
                database.install(&shared, true).unwrap();
            }
            lab.add_package(native_handoff_provider("/etc/init", managed))
                .unwrap();
            lab.publish().unwrap();
            let mut desired = vec!["fir", "daemon"];
            if case == "retained-owner" {
                desired.push("old-layout");
            }
            if case == "shared-owner" {
                desired.push("shared-layout");
            }
            configure(&lab, &desired, "fir", &["daemon"]);
            let before = lifecycle_snapshot(&lab);
            let error = rebuild(&lab, false).await.unwrap_err();
            assert_eq!(
                lifecycle_snapshot(&lab),
                before,
                "{case}, managed={managed}: {error:#}"
            );
            if case == "modified-config" {
                assert_eq!(
                    fs::read(lab.root().join("etc/init")).unwrap(),
                    b"administrator edit"
                );
            }
            assert_eq!(
                fs::read(outside.path().join("sentinel")).unwrap(),
                b"outside"
            );
            assert_eq!(fs::read_dir(outside.path()).unwrap().count(), 1);
        }
    }
}

#[tokio::test]
async fn unmanaged_manually_enabled_service_survives_rebuild_and_detects_drift() {
    let lab = initial_system().await;
    let manual_marker = lab.root().join("var/lib/sage/enabled-old-inactive");
    assert!(!manual_marker.exists());

    // 1. Simulate administrator manually enabling an unmanaged service outside Sage
    fs::write(&manual_marker, b"enabled").unwrap();
    assert!(manual_marker.is_file());

    // 2. Rebuild the system: Sage must NOT revoke/disable the unmanaged service
    rebuild(&lab, false).await.unwrap();
    assert!(
        manual_marker.is_file(),
        "Sage must not revoke administrator state created outside Sage"
    );

    // 3. Inspect service list: inactive must be detected as drift
    let services = sage_sys::list_services(lab.root()).unwrap();
    let inactive = services.iter().find(|s| s.name == "inactive").unwrap();
    assert_eq!(inactive.state, "unmanaged (drift)");
    let daemon = services.iter().find(|s| s.name == "daemon").unwrap();
    assert_eq!(daemon.state, "managed-enabled");

    // 4. Adopt the service into Sage declarative management
    sage_sys::service_adopt(lab.root(), "inactive", false).unwrap();
    let services = sage_sys::list_services(lab.root()).unwrap();
    let inactive = services.iter().find(|s| s.name == "inactive").unwrap();
    assert_eq!(inactive.state, "managed-enabled");

    // 5. Disable the service via Sage: now that Sage manages it, it should be disabled
    sage_sys::service_disable(lab.root(), "inactive", false).unwrap();
    assert!(
        !manual_marker.exists(),
        "Sage disables managed services upon explicit request"
    );
    let services = sage_sys::list_services(lab.root()).unwrap();
    let inactive = services.iter().find(|s| s.name == "inactive").unwrap();
    assert_eq!(inactive.state, "managed-disabled");

    // 6. Re-enable the service via Sage
    sage_sys::service_enable(lab.root(), "inactive", false).unwrap();
    assert!(manual_marker.is_file());
    let services = sage_sys::list_services(lab.root()).unwrap();
    let inactive = services.iter().find(|s| s.name == "inactive").unwrap();
    assert_eq!(inactive.state, "managed-enabled");
}

#[tokio::test]
async fn adopting_an_actually_disabled_service_does_not_change_declarations() {
    let lab = initial_system().await;
    let config_path = lab.root().join("etc/sage/services.toml");
    let before = fs::read(&config_path).unwrap();

    let dry_error = sage_sys::service_adopt(lab.root(), "inactive", true).unwrap_err();

    assert!(dry_error.to_string().contains("not externally enabled"));
    let error = sage_sys::service_adopt(lab.root(), "inactive", false).unwrap_err();

    assert!(error.to_string().contains("not externally enabled"));
    assert_eq!(fs::read(config_path).unwrap(), before);
    assert!(
        !lab.root()
            .join("var/lib/sage/enabled-old-inactive")
            .exists()
    );
}

#[tokio::test]
async fn provider_enable_failure_does_not_publish_managed_enabled_state() {
    let lab = initial_system().await;
    let config_path = lab.root().join("etc/sage/services.toml");
    let before = fs::read(&config_path).unwrap();
    fs::write(lab.root().join("var/lib/sage/fail-enable"), b"fail").unwrap();

    let error = sage_sys::service_enable(lab.root(), "inactive", false).unwrap_err();

    assert!(error.to_string().contains("exited with"));
    assert_eq!(fs::read(&config_path).unwrap(), before);
    assert!(
        !lab.root()
            .join("var/lib/sage/enabled-old-inactive")
            .exists()
    );
    fs::remove_file(lab.root().join("var/lib/sage/fail-enable")).unwrap();

    sage_sys::settle_journals(lab.root()).await.unwrap();

    let config = sage_sys::ServicesConfig::load(config_path).unwrap();
    assert!(config.enabled.contains("inactive"));
    assert!(
        lab.root()
            .join("var/lib/sage/enabled-old-inactive")
            .exists()
    );
}

#[tokio::test]
async fn provider_disable_failure_does_not_publish_managed_disabled_state() {
    let lab = initial_system().await;
    let config_path = lab.root().join("etc/sage/services.toml");
    let before = fs::read(&config_path).unwrap();
    fs::write(lab.root().join("var/lib/sage/fail-disable"), b"fail").unwrap();

    let error = sage_sys::service_disable(lab.root(), "daemon", false).unwrap_err();

    assert!(error.to_string().contains("exited with"));
    assert_eq!(fs::read(&config_path).unwrap(), before);
    assert!(lab.root().join("var/lib/sage/enabled-old-daemon").exists());
    fs::remove_file(lab.root().join("var/lib/sage/fail-disable")).unwrap();

    sage_sys::settle_journals(lab.root()).await.unwrap();

    let config = sage_sys::ServicesConfig::load(config_path).unwrap();
    assert!(!config.enabled.contains("daemon"));
    assert!(config.disabled.contains("daemon"));
    assert!(!lab.root().join("var/lib/sage/enabled-old-daemon").exists());
}

#[tokio::test]
async fn state_query_failure_when_disabling_service_leaves_journal_pending_and_preserves_declarations()
 {
    let lab = initial_system().await;
    let config_path = lab.root().join("etc/sage/services.toml");
    let before = fs::read(&config_path).unwrap();
    fs::write(lab.root().join("var/lib/sage/fail-is-enabled"), b"fail").unwrap();

    let dry_run_error = sage_sys::service_disable(lab.root(), "daemon", true).unwrap_err();
    assert!(dry_run_error.to_string().contains("exited with"));
    assert_eq!(fs::read(&config_path).unwrap(), before);
    assert!(lab.root().join("var/lib/sage/enabled-old-daemon").exists());

    let error = sage_sys::service_disable(lab.root(), "daemon", false).unwrap_err();
    assert!(error.to_string().contains("exited with"));
    assert_eq!(fs::read(&config_path).unwrap(), before);
    assert!(lab.root().join("var/lib/sage/enabled-old-daemon").exists());

    let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
    let pending = database.pending_journals().unwrap();
    assert_eq!(pending.len(), 1);
    assert_eq!(pending[0].stage, "provider");
    drop(database);

    fs::remove_file(lab.root().join("var/lib/sage/fail-is-enabled")).unwrap();

    sage_sys::settle_journals(lab.root()).await.unwrap();

    let config = sage_sys::ServicesConfig::load(config_path).unwrap();
    assert!(!config.enabled.contains("daemon"));
    assert!(config.disabled.contains("daemon"));
    assert!(!lab.root().join("var/lib/sage/enabled-old-daemon").exists());

    let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
    assert!(database.pending_journals().unwrap().is_empty());
}

#[tokio::test]
async fn state_query_failure_when_adopting_service_fails_loudly_without_claiming_not_enabled() {
    let lab = initial_system().await;
    let marker = lab.root().join("var/lib/sage/enabled-old-inactive");
    fs::write(&marker, b"enabled").unwrap();
    let config_path = lab.root().join("etc/sage/services.toml");
    let before = fs::read(&config_path).unwrap();
    fs::write(lab.root().join("var/lib/sage/fail-is-enabled"), b"fail").unwrap();

    let dry_run_error = sage_sys::service_adopt(lab.root(), "inactive", true).unwrap_err();
    assert!(dry_run_error.to_string().contains("exited with"));
    assert!(!dry_run_error.to_string().contains("not externally enabled"));
    assert_eq!(fs::read(&config_path).unwrap(), before);

    let error = sage_sys::service_adopt(lab.root(), "inactive", false).unwrap_err();
    assert!(error.to_string().contains("exited with"));
    assert!(!error.to_string().contains("not externally enabled"));
    assert_eq!(fs::read(&config_path).unwrap(), before);

    let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
    assert!(database.pending_journals().unwrap().is_empty());
    drop(database);

    fs::remove_file(lab.root().join("var/lib/sage/fail-is-enabled")).unwrap();

    sage_sys::service_adopt(lab.root(), "inactive", false).unwrap();

    let config = sage_sys::ServicesConfig::load(config_path).unwrap();
    assert!(config.enabled.contains("inactive"));
    assert!(!config.disabled.contains("inactive"));
    assert!(marker.exists());
}

#[tokio::test]
async fn lifecycle_dry_runs_reject_a_malformed_init_generator_without_mutation() {
    let lab = initial_system().await;
    let config_path = lab.root().join("etc/sage/services.toml");
    let rendered_path = lab.root().join("var/lib/sage/rendered-services.toml");
    let generator_path = lab.root().join("usr/share/sage/rclass/init-loom.toml");
    fs::remove_file(rendered_path).unwrap();
    fs::write(generator_path, b"invalid TOML [").unwrap();
    let before = fs::read(&config_path).unwrap();

    let enable_error = sage_sys::service_enable(lab.root(), "inactive", true).unwrap_err();
    let disable_error = sage_sys::service_disable(lab.root(), "daemon", true).unwrap_err();

    assert!(enable_error.to_string().contains("TOML"));
    assert!(disable_error.to_string().contains("TOML"));
    assert_eq!(fs::read(config_path).unwrap(), before);
    assert!(lab.root().join("var/lib/sage/enabled-old-daemon").exists());
    assert!(
        !lab.root()
            .join("var/lib/sage/enabled-old-inactive")
            .exists()
    );
}

#[tokio::test]
async fn lifecycle_operations_validate_the_generator_before_journaling() {
    for invalid in ["target", "template", "service-type"] {
        let lab = initial_system().await;
        let config_path = lab.root().join("etc/sage/services.toml");
        let rendered_path = lab.root().join("var/lib/sage/rendered-services.toml");
        let mut rendered = sage_sys::RenderedServicesState::load(&rendered_path).unwrap();
        let expected = match invalid {
            "target" => {
                rendered.generator.target_path_template = "/etc/${unknown}".into();
                "unknown variable"
            }
            "template" => {
                rendered.generator.template = "${unknown}".into();
                "unknown variable"
            }
            "service-type" => {
                rendered.generator.supported_types = vec!["oneshot".into()];
                "does not support service type"
            }
            _ => unreachable!(),
        };
        rendered.save(&rendered_path).unwrap();
        let inactive_marker = lab.root().join("var/lib/sage/enabled-old-inactive");
        fs::write(&inactive_marker, b"enabled").unwrap();
        let before_config = fs::read(&config_path).unwrap();
        let before = lifecycle_snapshot(&lab);

        for dry_run in [true, false] {
            for error in [
                sage_sys::service_enable(lab.root(), "inactive", dry_run).unwrap_err(),
                sage_sys::service_disable(lab.root(), "daemon", dry_run).unwrap_err(),
                sage_sys::service_adopt(lab.root(), "inactive", dry_run).unwrap_err(),
            ] {
                assert!(
                    error.to_string().contains(expected),
                    "{invalid}, dry_run={dry_run}: {error}"
                );
            }
        }

        assert_eq!(fs::read(config_path).unwrap(), before_config);
        assert_eq!(lifecycle_snapshot(&lab), before);
        assert!(inactive_marker.is_file());
        let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
        assert!(database.pending_journals().unwrap().is_empty());
    }
}

#[tokio::test]
async fn service_enable_recovers_declarations_after_a_post_provider_failure() {
    let mut lab = initial_system().await;
    let config_path = lab.root().join("etc/sage/services.toml");
    let before = fs::read(&config_path).unwrap();
    lab.inject("service-provider").unwrap();

    let error = sage_sys::service_enable(lab.root(), "inactive", false).unwrap_err();

    assert!(error.to_string().contains("injected crash"));
    assert_eq!(fs::read(&config_path).unwrap(), before);
    assert!(
        lab.root()
            .join("var/lib/sage/enabled-old-inactive")
            .exists()
    );
    let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
    let pending = database.pending_journals().unwrap();
    assert_eq!(pending.len(), 1);
    assert_eq!(pending[0].stage, "declaration");
    drop(database);

    sage_sys::settle_journals(lab.root()).await.unwrap();

    let config = sage_sys::ServicesConfig::load(config_path).unwrap();
    assert!(config.enabled.contains("inactive"));
    assert!(!config.disabled.contains("inactive"));
    let rendered = sage_sys::RenderedServicesState::load(
        lab.root().join("var/lib/sage/rendered-services.toml"),
    )
    .unwrap();
    assert!(rendered.enabled.contains("inactive"));
    let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
    assert!(database.pending_journals().unwrap().is_empty());
}

#[tokio::test]
async fn service_disable_recovers_declarations_after_a_post_provider_failure() {
    let mut lab = initial_system().await;
    let config_path = lab.root().join("etc/sage/services.toml");
    let before = fs::read(&config_path).unwrap();
    lab.inject("service-provider").unwrap();

    let error = sage_sys::service_disable(lab.root(), "daemon", false).unwrap_err();

    assert!(error.to_string().contains("injected crash"));
    assert_eq!(fs::read(&config_path).unwrap(), before);
    assert!(!lab.root().join("var/lib/sage/enabled-old-daemon").exists());
    let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
    let pending = database.pending_journals().unwrap();
    assert_eq!(pending.len(), 1);
    assert_eq!(pending[0].stage, "declaration");
    drop(database);

    sage_sys::settle_journals(lab.root()).await.unwrap();

    let config = sage_sys::ServicesConfig::load(config_path).unwrap();
    assert!(!config.enabled.contains("daemon"));
    assert!(config.disabled.contains("daemon"));
    let rendered = sage_sys::RenderedServicesState::load(
        lab.root().join("var/lib/sage/rendered-services.toml"),
    )
    .unwrap();
    assert!(!rendered.enabled.contains("daemon"));
    let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
    assert!(database.pending_journals().unwrap().is_empty());
}

#[tokio::test]
async fn service_adopt_recovers_declarations_after_a_post_provider_failure() {
    let mut lab = initial_system().await;
    let marker = lab.root().join("var/lib/sage/enabled-old-inactive");
    fs::write(&marker, b"enabled").unwrap();
    let config_path = lab.root().join("etc/sage/services.toml");
    let before = fs::read(&config_path).unwrap();
    lab.inject("service-provider").unwrap();

    let error = sage_sys::service_adopt(lab.root(), "inactive", false).unwrap_err();

    assert!(error.to_string().contains("injected crash"));
    assert_eq!(fs::read(&config_path).unwrap(), before);
    assert!(marker.exists());

    sage_sys::settle_journals(lab.root()).await.unwrap();

    let config = sage_sys::ServicesConfig::load(config_path).unwrap();
    assert!(config.enabled.contains("inactive"));
    assert!(!config.disabled.contains("inactive"));
    assert!(marker.exists());
    let database = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
    assert!(database.pending_journals().unwrap().is_empty());
}

#[tokio::test]
async fn managed_disabled_external_enablement_is_reported_as_managed_drift() {
    let lab = initial_system().await;
    let manual_marker = lab.root().join("var/lib/sage/enabled-old-inactive");
    fs::write(&manual_marker, b"enabled").unwrap();
    sage_sys::service_adopt(lab.root(), "inactive", false).unwrap();
    sage_sys::service_disable(lab.root(), "inactive", false).unwrap();
    fs::write(&manual_marker, b"enabled").unwrap();

    let services = sage_sys::list_services(lab.root()).unwrap();
    let inactive = services
        .iter()
        .find(|service| service.name == "inactive")
        .unwrap();

    assert_eq!(inactive.state, "managed-disabled (drift)");
    assert!(manual_marker.is_file());
}

#[test]
fn services_config_rejects_an_overlapping_enablement_declaration() {
    let directory = tempfile::tempdir().unwrap();
    let path = directory.path().join("services.toml");
    fs::write(
        &path,
        "schema_version=1\nenabled=[\"daemon\"]\ndisabled=[\"daemon\"]\n",
    )
    .unwrap();

    let error = sage_sys::ServicesConfig::load(path).unwrap_err();

    assert!(error.to_string().contains("both enabled and disabled"));
}

#[test]
fn missing_init_provider_does_not_fallback_to_systemd_or_the_workspace() {
    let lab = TortureLab::new().unwrap();

    let error = sage_sys::load_active_generator(lab.root()).unwrap_err();

    assert!(
        error
            .to_string()
            .contains("no active init provider is known; run sage rebuild first")
    );
}

#[test]
fn malformed_installed_service_document_fails_loudly() {
    let lab = TortureLab::new().unwrap();
    let directory = lab.root().join("usr/share/sage/services");
    fs::create_dir_all(&directory).unwrap();
    fs::write(directory.join("broken.toml"), b"invalid TOML [").unwrap();

    let error = sage_sys::load_available_services(lab.root()).unwrap_err();

    assert!(error.to_string().contains("invalid service document"));
}
