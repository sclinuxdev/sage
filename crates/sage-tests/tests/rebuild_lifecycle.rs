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
        "schema_version=1\n[service_generator]\ntarget_path=\"/etc/native-{generation}/${{service.name}}\"\nmode=420\ntemplate=\"{generation}: ${{service.name}}\"\nvalidate_command=\"/usr/bin/{name}ctl ${{SYSROOT}} validate ${{service.name}}\"\nenable_command=\"/usr/bin/{name}ctl ${{SYSROOT}} enable ${{service.name}}\"\ndisable_command=\"/usr/bin/{name}ctl ${{SYSROOT}} disable ${{service.name}}\"\n"
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
            "#!/bin/sh\nset -eu\nroot=$1\naction=$2\nservice=$3\nmarker=\"$root/var/lib/sage/enabled-{generation}-$service\"\ncase $action in\nvalidate) test -f \"$root/etc/native-{generation}/$service\" ;;\nenable) printf enabled > \"$marker\" ;;\ndisable)\n  test -f \"$root/etc/native-{generation}/$service\"\n  test -f \"$marker\"\n  /bin/rm \"$marker\"\n  printf '%s\\n' \"{generation}:$service\" >> \"$root/var/lib/sage/disable-log\"\n  ;;\n*) exit 2 ;;\nesac\n"
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
            "schema_version=1\npackages={packages:?}\nservices={enabled:?}\n[system]\narchitecture=\"amd64\"\nprofile=\"default\"\n[providers]\ninit={init:?}\n"
        ),
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
