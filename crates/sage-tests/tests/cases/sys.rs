use sage_sys::*;
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};

fn release(
    key: sage_core::PackageKey,
    version: &str,
    dependencies: &[&str],
    provides: &[&str],
) -> sage_core::Package {
    sage_core::Package::from_release(
        key,
        version.parse().unwrap(),
        dependencies
            .iter()
            .map(|value| value.parse().unwrap())
            .collect(),
        provides.iter().map(|value| (*value).into()).collect(),
    )
}

fn installed(package: &sage_core::Package) -> sage_db::InstalledPackage {
    let coordinate = package.coordinate();
    sage_db::InstalledPackage {
        key: coordinate.key,
        version: coordinate.version,
        arch: "amd64".into(),
        installed_size: 0,
        dependencies: package.dependencies.clone(),
        provides: package.provides.clone(),
        conflicts: package.conflicts.clone(),
        files: vec![],
        config_hashes: BTreeMap::new(),
    }
}

fn config(packages: &[&str], providers: &[(&str, &str)]) -> SystemConfig {
    SystemConfig {
        schema_version: 1,
        system: SystemMetadata {
            architecture: "amd64".into(),
            profile: "default".into(),
        },
        packages: packages.iter().map(|name| (*name).into()).collect(),
        providers: providers
            .iter()
            .map(|(symbol, key)| ((*symbol).into(), (*key).into()))
            .collect(),
    }
}

#[test]
fn provider_symbols_share_cli_and_configuration_validation() {
    for symbol in ["so:libfoo.so.1", "so:libC++.so.1@ABI"] {
        let preferences = config(&[], &[(symbol, "so:abi+debug")])
            .provider_preferences("main/system")
            .unwrap();
        assert_eq!(
            preferences[symbol],
            sage_core::PackageKey::new("main/system", "so", "abi+debug")
        );
    }
    for symbol in [
        "so:",
        "so:lib foo.so",
        "so:lib/foo.so",
        "so:lib\nfoo.so",
        "so:lib=foo.so",
        "virtual/so:libfoo.so",
    ] {
        assert!(
            config(&[], &[(symbol, "foo")])
                .provider_preferences("main/system")
                .is_err(),
            "{symbol}"
        );
    }
}

#[test]
fn rebuild_retains_foreign_consumers_without_exporting_their_bindings() {
    use sage_core::PackageKey;
    for symbol in ["virtual/libc", "so:libc.so.6"] {
        let app = release(
            PackageKey::new("vendor/runtime", "app", "0"),
            "1-1",
            &[symbol],
            &[],
        );
        let foreign = release(
            PackageKey::new("vendor/system", "libc", "2"),
            "1-1",
            &[],
            &[symbol],
        );
        let main = PackageKey::new("main/system", "libc", "0");
        let mut universe = sage_solver::PackageUniverse::default();
        universe.insert(release(main.clone(), "1-1", &[], &[symbol]));
        universe.insert(app.clone());
        universe.insert(foreign.clone());
        let installed = vec![installed(&app), installed(&foreign)];
        let desired = config(&[], &[(symbol, "libc")]);
        let plan = ReconcilePlan::compute(&desired, &installed, &universe, false).unwrap();
        assert_eq!(plan.install, vec![(main.clone(), "1-1".parse().unwrap())]);
        assert!(plan.remove.is_empty());
        assert_eq!(
            plan.provider_bindings,
            BTreeMap::from([(
                symbol.strip_prefix("virtual/").unwrap_or(symbol).into(),
                main
            ),])
        );
        assert!(find_orphans(&installed, &desired).is_empty());
    }
}

#[test]
fn configured_provider_is_strict_with_and_without_a_consumer() {
    use sage_core::PackageKey;
    for dependency in [vec![], vec!["virtual/libc"]] {
        let mut universe = sage_solver::PackageUniverse::default();
        let mut guard = release(
            PackageKey::new("main/system", "guard", "0"),
            "1-1",
            &dependency,
            &[],
        );
        guard.conflicts.push("musl".into());
        universe.insert(guard);
        for name in ["glibc", "musl"] {
            universe.insert(release(
                PackageKey::new("main/system", name, "0"),
                "1-1",
                &[],
                &["virtual/libc"],
            ));
        }
        assert!(
            ReconcilePlan::compute(
                &config(&["guard"], &[("libc", "musl")]),
                &[],
                &universe,
                false,
            )
            .is_err()
        );
    }
}

#[test]
fn configured_binding_comes_from_the_constrained_virtual_choice() {
    use sage_core::PackageKey;
    let mut universe = sage_solver::PackageUniverse::default();
    for package in [
        release(
            PackageKey::new("main/system", "app", "0"),
            "1-1",
            &["virtual/libc:2 >= 2-1"],
            &[],
        ),
        release(
            PackageKey::new("main/system", "preferred", "1"),
            "1-1",
            &[],
            &["virtual/libc"],
        ),
        release(
            PackageKey::new("main/system", "selected", "2"),
            "2-1",
            &[],
            &["virtual/libc"],
        ),
    ] {
        universe.insert(package);
    }
    assert!(
        ReconcilePlan::compute(
            &config(&["app", "preferred:1"], &[("libc", "preferred:1")]),
            &[],
            &universe,
            false,
        )
        .is_err()
    );
    let plan = ReconcilePlan::compute(
        &config(&["app", "preferred:1"], &[("libc", "selected:2")]),
        &[],
        &universe,
        false,
    )
    .unwrap();
    assert_eq!(
        plan.provider_bindings["libc"],
        PackageKey::new("main/system", "selected", "2")
    );
}

#[test]
fn configured_bindings_reject_invalid_and_ambiguous_providers() {
    use sage_core::PackageKey;
    let mut universe = sage_solver::PackageUniverse::default();
    universe.insert(release(
        PackageKey::new("main/system", "not-a-provider", "0"),
        "1-1",
        &[],
        &[],
    ));
    assert!(
        ReconcilePlan::compute(
            &config(&[], &[("libc", "not-a-provider")]),
            &[],
            &universe,
            false
        )
        .is_err()
    );
    for (name, dependency) in [
        ("old-app", "virtual/libc < 2-1"),
        ("new-app", "virtual/libc >= 2-1"),
    ] {
        universe.insert(release(
            PackageKey::new("main/system", name, "0"),
            "1-1",
            &[dependency],
            &[],
        ));
    }
    for (name, version) in [("old-libc", "1-1"), ("new-libc", "2-1")] {
        universe.insert(release(
            PackageKey::new("main/system", name, "0"),
            version,
            &[],
            &["virtual/libc"],
        ));
    }
    assert!(
        ReconcilePlan::compute(
            &config(&["old-app", "new-app"], &[("libc", "new-libc")]),
            &[],
            &universe,
            false
        )
        .is_err()
    );
}

#[test]
fn reconciliation_keeps_missing_installed_desired_release() {
    use sage_core::PackageKey;
    let key = PackageKey::new("main/system", "app", "2");
    let current = installed(&release(key.clone(), "2-1", &[], &[]));
    let mut universe = sage_solver::PackageUniverse::default();
    universe.insert(release(key, "1-1", &[], &[]));
    let plan =
        ReconcilePlan::compute(&config(&["app:2"], &[]), &[current], &universe, false).unwrap();
    assert!(plan.install.is_empty());
    assert!(plan.remove.is_empty());
}

#[test]
fn configured_interfaces_remain_required_after_consumer_backtracking() {
    use sage_core::PackageKey;
    let mut universe = sage_solver::PackageUniverse::default();
    universe.insert(release(
        PackageKey::new("main/system", "app", "0"),
        "2-1",
        &["virtual/libc"],
        &[],
    ));
    universe.insert(release(
        PackageKey::new("main/system", "app", "0"),
        "1-1",
        &[],
        &[],
    ));
    universe.insert(release(
        PackageKey::new("main/system", "libc", "0"),
        "1-1",
        &[],
        &["virtual/libc"],
    ));
    let mut init = release(
        PackageKey::new("main/system", "init", "0"),
        "1-1",
        &[],
        &["virtual/init"],
    );
    init.conflicts.push("app >= 2-1".into());
    universe.insert(init);
    let plan = ReconcilePlan::compute(
        &config(&["app"], &[("libc", "libc"), ("init", "init")]),
        &[],
        &universe,
        false,
    )
    .unwrap();
    assert_eq!(
        plan.provider_bindings,
        BTreeMap::from([
            ("libc".into(), PackageKey::new("main/system", "libc", "0")),
            ("init".into(), PackageKey::new("main/system", "init", "0")),
        ])
    );
    assert!(plan.install.contains(&(
        PackageKey::new("main/system", "app", "0"),
        "1-1".parse().unwrap()
    )));
}

#[test]
fn retained_cross_channel_consumer_can_switch_its_virtual_provider() {
    use sage_core::PackageKey;
    let old = release(
        PackageKey::new("main/system", "old-libc", "0"),
        "1-1",
        &[],
        &["virtual/libc"],
    );
    let consumer = release(
        PackageKey::new("main/python", "consumer", "3"),
        "1-1",
        &["virtual/libc"],
        &[],
    );
    let replacement = PackageKey::new("main/system", "new-libc", "2");
    let mut universe = sage_solver::PackageUniverse::default();
    universe.insert(release(replacement.clone(), "2-1", &[], &["virtual/libc"]));
    let plan = ReconcilePlan::compute(
        &config(&[], &[("libc", "new-libc:2")]),
        &[installed(&old), installed(&consumer)],
        &universe,
        false,
    )
    .unwrap();
    assert_eq!(plan.provider_bindings["libc"], replacement);
    assert_eq!(plan.install, vec![(replacement, "2-1".parse().unwrap())]);
    assert_eq!(plan.remove, vec![old.coordinate().key]);
}

#[test]
fn removal_rejects_an_empty_shared_library_replacement() {
    let root = tempfile::tempdir().unwrap();
    let database = sage_db::SageDatabase::open(root.path().join("var/lib/sage")).unwrap();
    let compiler = sage_db::InstalledPackage {
        key: sage_core::PackageKey::new("main/gcc16", "gcc", "16"),
        version: "16.2.0-1".parse().unwrap(),
        arch: "amd64".into(),
        installed_size: 1,
        dependencies: vec![],
        provides: vec!["so:libgcc_s.so.1".into()],
        conflicts: vec![],
        files: vec!["usr/lib64/libgcc_s.so.1".into()],
        config_hashes: BTreeMap::new(),
    };
    let mut runtime = sage_db::InstalledPackage {
        key: sage_core::PackageKey::new("main/system", "gcc-libs", "16"),
        version: "16.2.0-1".parse().unwrap(),
        arch: "amd64".into(),
        installed_size: 0,
        dependencies: vec!["so:libgcc_s.so.1".parse().unwrap()],
        provides: vec!["so:libgcc_s.so.1".into()],
        conflicts: vec![],
        files: vec![],
        config_hashes: BTreeMap::new(),
    };
    database.install(&compiler, false).unwrap();
    database.install(&runtime, false).unwrap();
    drop(database);

    let error =
        remove_packages(root.path(), &["gcc".into()], Some("gcc16"), false, true).unwrap_err();
    assert!(
        error
            .to_string()
            .contains("required by main/system:gcc-libs:16"),
        "{error:#}"
    );

    runtime.files.push("usr/lib/libgcc_s.so.1".into());
    let database = sage_db::SageDatabase::open(root.path().join("var/lib/sage")).unwrap();
    database.install(&runtime, false).unwrap();
    drop(database);
    remove_packages(root.path(), &["gcc".into()], Some("gcc16"), false, true).unwrap();
}

#[test]
fn unconfigured_virtual_and_concrete_fallback_choices_are_not_bindings() {
    use sage_core::PackageKey;
    let mut universe = sage_solver::PackageUniverse::default();
    universe.insert(release(
        PackageKey::new("main/system", "app", "0"),
        "1-1",
        &["virtual/libc", "libz"],
        &[],
    ));
    universe.insert(release(
        PackageKey::new("main/system", "libc", "0"),
        "1-1",
        &[],
        &["virtual/libc"],
    ));
    universe.insert(release(
        PackageKey::new("main/system", "zlib", "2"),
        "1-1",
        &[],
        &["libz"],
    ));
    let plan = ReconcilePlan::compute(&config(&["app"], &[]), &[], &universe, false).unwrap();
    assert_eq!(plan.install.len(), 3);
    assert!(plan.provider_bindings.is_empty());
}

#[test]
fn retained_consumers_keep_dependencies_in_the_solve_and_allow_upgrades() {
    use sage_core::PackageKey;
    for (channel, no_prune) in [("main/python", false), ("main/system", true)] {
        let consumer = release(
            PackageKey::new(channel, "consumer", "3"),
            "1-1",
            &["main/system/lib:2 >= 1-1"],
            &[],
        );
        let old_lib = release(PackageKey::new("main/system", "lib", "2"), "1-1", &[], &[]);
        let mut universe = sage_solver::PackageUniverse::default();
        universe.insert(release(
            PackageKey::new("main/system", "lib", "2"),
            "2-1",
            &[],
            &[],
        ));
        universe.insert(release(
            PackageKey::new("main/system", "app", "0"),
            "1-1",
            &["lib:2 >= 2-1"],
            &[],
        ));
        let current = [installed(&consumer), installed(&old_lib)];
        let retained_plan =
            ReconcilePlan::compute(&config(&[], &[]), &current, &universe, no_prune).unwrap();
        assert!(retained_plan.install.is_empty());
        assert!(retained_plan.remove.is_empty());
        let plan =
            ReconcilePlan::compute(&config(&["app"], &[]), &current, &universe, no_prune).unwrap();
        assert_eq!(
            plan.install,
            vec![
                (
                    PackageKey::new("main/system", "app", "0"),
                    "1-1".parse().unwrap()
                ),
                (
                    PackageKey::new("main/system", "lib", "2"),
                    "2-1".parse().unwrap()
                ),
            ]
        );
        assert!(plan.remove.is_empty());
    }
}

#[test]
fn administrator_trigger_overrides_vendor_definition() {
    let root = tempfile::tempdir().unwrap();
    for directory in ["usr/share/sage/triggers", "etc/sage/triggers.d"] {
        fs::create_dir_all(root.path().join(directory)).unwrap();
    }
    let trigger = |priority| {
        format!(
            "schema_version=1\nname=\"cache\"\ndescription=\"x\"\non_paths=[\"usr/lib/*\"]\nexec=[\"/missing\"]\npriority={priority}\nignore_missing_binary=true\n"
        )
    };
    fs::write(
        root.path().join("usr/share/sage/triggers/cache.toml"),
        trigger(10),
    )
    .unwrap();
    fs::write(
        root.path().join("etc/sage/triggers.d/cache.toml"),
        trigger(20),
    )
    .unwrap();
    assert_eq!(
        TriggerEngine::load_triggers(root.path()).unwrap()[0].priority,
        20
    );
}

#[test]
fn standard_trigger_library_is_valid_and_unique() {
    let directory = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../triggers");
    let mut names = BTreeSet::new();
    let mut files: Vec<_> = fs::read_dir(directory)
        .unwrap()
        .collect::<Result<_, _>>()
        .unwrap();
    files.sort_by_key(|entry| entry.file_name());
    for file in files {
        let trigger = TriggerSpec::load(file.path()).unwrap();
        assert!(names.insert(trigger.name));
    }
    assert!(names.len() >= 8);
}

#[test]
fn standard_init_rclasses_are_valid() {
    let directory = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../rclass");
    for name in ["init-loom.toml", "init-systemd.toml"] {
        let path = directory.join(name);
        let generator = TemplateServiceGenerator::from_rclass(&path).unwrap();
        assert!(!generator.target_path_template.is_empty());
        assert!(!generator.template.is_empty());
    }
}

fn activation_service(name: &str, activation: &str) -> ServiceSpec {
    let document = format!(
        "schema_version=1\n[service]\nname={name:?}\ndescription=\"Demo daemon\"\ncommand=[\"/usr/bin/demo\",\"--run\"]\nuser=\"daemon\"\ngroup=\"daemon\"\nworking_dir=\"/\"\nrestart=\"on-failure\"\ntype=\"notify\"\n{activation}\n"
    );
    ServiceDocument::parse(document.as_bytes())
        .unwrap()
        .into_services()
        .remove(0)
}

#[test]
fn unsupported_activation_contracts_fail_closed() {
    let per_connection = "schema_version=1\n[service]\nname=\"demo\"\ndescription=\"Demo\"\ncommand=[\"/usr/bin/demo\"]\nuser=\"daemon\"\ngroup=\"daemon\"\nworking_dir=\"/\"\nrestart=\"no\"\ntype=\"simple\"\n[service.activation]\nkind=\"socket\"\nlisten_stream=\"/run/demo.sock\"\naccept=true\n";
    let session_bus = "schema_version=1\n[service]\nname=\"demo\"\ndescription=\"Demo\"\ncommand=[\"/usr/bin/demo\"]\nuser=\"daemon\"\ngroup=\"daemon\"\nworking_dir=\"/\"\nrestart=\"no\"\ntype=\"simple\"\n[service.activation]\nkind=\"dbus\"\nname=\"org.example.Demo\"\nbus=\"session\"\n";
    let invalid_socket_mode = "schema_version=1\n[service]\nname=\"demo\"\ndescription=\"Demo\"\ncommand=[\"/usr/bin/demo\"]\nuser=\"daemon\"\ngroup=\"daemon\"\nworking_dir=\"/\"\nrestart=\"no\"\ntype=\"simple\"\n[service.activation]\nkind=\"socket\"\nlisten_stream=\"/run/demo.sock\"\nmode=4095\n";
    let invalid_dbus_name = "schema_version=1\n[service]\nname=\"demo\"\ndescription=\"Demo\"\ncommand=[\"/usr/bin/demo\"]\nuser=\"daemon\"\ngroup=\"daemon\"\nworking_dir=\"/\"\nrestart=\"no\"\ntype=\"simple\"\n[service.activation]\nkind=\"dbus\"\nname=\"org.7zip.Demo\"\n";

    assert!(
        ServiceDocument::parse(per_connection.as_bytes())
            .unwrap_err()
            .to_string()
            .contains("per-connection socket activation")
    );
    assert!(
        ServiceDocument::parse(session_bus.as_bytes())
            .unwrap_err()
            .to_string()
            .contains("only supports system D-Bus activation")
    );
    assert!(
        ServiceDocument::parse(invalid_socket_mode.as_bytes())
            .unwrap_err()
            .to_string()
            .contains("permission bits only")
    );
    assert!(
        ServiceDocument::parse(invalid_dbus_name.as_bytes())
            .unwrap_err()
            .to_string()
            .contains("invalid D-Bus name")
    );
}

#[test]
fn systemd_renders_socket_and_dbus_activation_artifacts() {
    let root = tempfile::tempdir().unwrap();
    let generator = TemplateServiceGenerator::from_rclass(
        &Path::new(env!("CARGO_MANIFEST_DIR")).join("../../rclass/init-systemd.toml"),
    )
    .unwrap();
    let socket = activation_service(
        "socket-demo",
        "[service.activation]\nkind=\"socket\"\nlisten_stream=\"/run/demo.sock\"\naccept=false",
    );
    let dbus = activation_service(
        "dbus-demo",
        "[service.activation]\nkind=\"dbus\"\nname=\"org.example.Demo\"\nbus=\"system\"\nuser=\"root\"",
    );

    generator
        .render_service_set(&[socket.clone(), dbus.clone()], root.path())
        .unwrap();

    let service_unit = fs::read_to_string(
        root.path()
            .join("usr/lib/systemd/system/socket-demo.service"),
    )
    .unwrap();
    let socket_unit = fs::read_to_string(
        root.path()
            .join("usr/lib/systemd/system/socket-demo.socket"),
    )
    .unwrap();
    assert!(service_unit.contains("Requires=socket-demo.socket"));
    assert!(socket_unit.contains("ListenStream=/run/demo.sock"));
    assert!(socket_unit.contains("SocketMode=0666"));
    assert!(socket_unit.contains("WantedBy=sockets.target"));
    assert!(!generator.is_automatic(&socket).unwrap());

    let activation = fs::read_to_string(
        root.path()
            .join("usr/share/dbus-1/system-services/org.example.Demo.service"),
    )
    .unwrap();
    assert!(activation.contains("Name=org.example.Demo"));
    assert!(activation.contains("Exec=\"/usr/bin/demo\" \"--run\""));
    assert!(activation.contains("User=root"));
    assert!(generator.is_automatic(&dbus).unwrap());
    generator.remove_service(&socket, root.path()).unwrap();
    generator.remove_service(&dbus, root.path()).unwrap();
    assert!(
        !root
            .path()
            .join("usr/lib/systemd/system/socket-demo.service")
            .exists()
    );
    assert!(
        !root
            .path()
            .join("usr/lib/systemd/system/socket-demo.socket")
            .exists()
    );
    assert!(
        !root
            .path()
            .join("usr/lib/systemd/system/dbus-demo.service")
            .exists()
    );
    assert!(
        !root
            .path()
            .join("usr/share/dbus-1/system-services/org.example.Demo.service")
            .exists()
    );
}

#[test]
fn loom_compiler_receives_portable_activation_contracts() {
    let root = tempfile::tempdir().unwrap();
    let compiler = root.path().join("usr/lib/loom/loom");
    fs::create_dir_all(compiler.parent().unwrap()).unwrap();
    fs::write(
        &compiler,
        "#!/bin/sh\ncase \"$1\" in\ncompile-service) cp \"$3\" \"$5\" ;;\nvalidate) exit 0 ;;\n*) exit 2 ;;\nesac\n",
    )
    .unwrap();
    fs::set_permissions(&compiler, fs::Permissions::from_mode(0o755)).unwrap();
    let generator = TemplateServiceGenerator::from_rclass(
        &Path::new(env!("CARGO_MANIFEST_DIR")).join("../../rclass/init-loom.toml"),
    )
    .unwrap();
    let socket = activation_service(
        "socket-demo",
        "[service.activation]\nkind=\"socket\"\nlisten_stream=\"/run/demo.sock\"\naccept=false",
    );
    let dbus = activation_service(
        "dbus-demo",
        "[service.activation]\nkind=\"dbus\"\nname=\"org.example.Demo\"\nbus=\"system\"\nuser=\"root\"",
    );

    generator
        .render_service_set(&[socket, dbus], root.path())
        .unwrap();

    let compiled =
        fs::read_to_string(root.path().join("usr/lib/loom/services/socket-demo.toml")).unwrap();
    assert!(compiled.contains("kind = \"socket\""));
    assert!(compiled.contains("listen_stream = \"/run/demo.sock\""));
    assert!(compiled.contains("mode = 438"));
    assert!(compiled.contains("fd_protocol = \"sd-listen-fds\""));

    let compiled =
        fs::read_to_string(root.path().join("usr/lib/loom/services/dbus-demo.toml")).unwrap();
    let descriptor = fs::read_to_string(
        root.path()
            .join("usr/share/dbus-1/system-services/org.example.Demo.service"),
    )
    .unwrap();
    assert!(compiled.contains("kind = \"dbus\""));
    assert!(compiled.contains("automatic = true"));
    assert!(descriptor.contains("Name=org.example.Demo"));
}

#[test]
fn sclinux_recipes_service_contracts_render_on_systemd_and_loom() {
    let recipes_dir = Path::new("/home/ir/sclinux-recipes");
    if !recipes_dir.exists() {
        return;
    }

    let rclass_dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../rclass");
    let systemd_gen =
        TemplateServiceGenerator::from_rclass(&rclass_dir.join("init-systemd.toml")).unwrap();
    let loom_gen =
        TemplateServiceGenerator::from_rclass(&rclass_dir.join("init-loom.toml")).unwrap();

    let service_paths = [
        recipes_dir.join("recipes/system/dbus/amd64/dbus-1.16.2-2/service.toml"),
        recipes_dir.join("recipes/security/polkit/amd64/polkit-127-2/service.toml"),
        recipes_dir.join("recipes/system/seatd/amd64/seatd-0.9.3-2/service.toml"),
        recipes_dir.join("recipes/system/eudev/amd64/eudev-3.2.14-2/service.toml"),
        recipes_dir.join("recipes/net/dhcpcd/amd64/dhcpcd-10.5.2-2/service.toml"),
        recipes_dir.join("templates/service.template.toml"),
    ];

    for path in &service_paths {
        let docs = ServiceDocument::load(path).unwrap().into_services();
        assert!(
            !docs.is_empty(),
            "failed to load service from {}",
            path.display()
        );
        for service in docs {
            // 1. Systemd rendering test
            let sys_root = tempfile::tempdir().unwrap();
            systemd_gen
                .render_service_unvalidated(&service, sys_root.path())
                .unwrap();
            let primary = systemd_gen
                .rendered_path(&service, sys_root.path())
                .unwrap();
            assert!(primary.is_file());

            if service.name == "dbus" {
                let socket_unit = sys_root.path().join("usr/lib/systemd/system/dbus.socket");
                assert!(socket_unit.is_file());
                let content = fs::read_to_string(&socket_unit).unwrap();
                assert!(content.contains("ListenStream=/run/dbus/system_bus_socket"));
                assert!(!systemd_gen.is_automatic(&service).unwrap());
            } else if service.name == "polkit" {
                let dbus_act = sys_root
                    .path()
                    .join("usr/share/dbus-1/system-services/org.freedesktop.PolicyKit1.service");
                assert!(dbus_act.is_file());
                let content = fs::read_to_string(&dbus_act).unwrap();
                assert!(content.contains("Name=org.freedesktop.PolicyKit1"));
                assert!(content.contains("User=root"));
                assert!(systemd_gen.is_automatic(&service).unwrap());
            }

            // 2. Loom rendering test (mock loom compiler)
            let loom_root = tempfile::tempdir().unwrap();
            let compiler = loom_root.path().join("usr/lib/loom/loom");
            fs::create_dir_all(compiler.parent().unwrap()).unwrap();
            fs::write(
                &compiler,
                "#!/bin/sh\ncase \"$1\" in\ncompile-service) cp \"$3\" \"$5\" ;;\nvalidate) exit 0 ;;\n*) exit 2 ;;\nesac\n",
            )
            .unwrap();
            fs::set_permissions(&compiler, fs::Permissions::from_mode(0o755)).unwrap();

            loom_gen
                .render_service_unvalidated(&service, loom_root.path())
                .unwrap();
            let primary_loom = loom_gen.rendered_path(&service, loom_root.path()).unwrap();
            assert!(primary_loom.is_file());

            let compiled_text = fs::read_to_string(&primary_loom).unwrap();
            if service.name == "dbus" {
                assert!(compiled_text.contains("kind = \"socket\""));
                assert!(compiled_text.contains("listen_stream = \"/run/dbus/system_bus_socket\""));
                assert!(compiled_text.contains("fd_protocol = \"sd-listen-fds\""));
            } else if service.name == "polkit" {
                assert!(compiled_text.contains("kind = \"dbus\""));
                assert!(compiled_text.contains("name = \"org.freedesktop.PolicyKit1\""));
                let dbus_act = loom_root
                    .path()
                    .join("usr/share/dbus-1/system-services/org.freedesktop.PolicyKit1.service");
                assert!(dbus_act.is_file());
                let content = fs::read_to_string(&dbus_act).unwrap();
                assert!(content.contains("Name=org.freedesktop.PolicyKit1"));
            }
        }
    }
}

#[test]
fn loom_render_rolls_back_atomically_on_validation_failure() {
    let root = tempfile::tempdir().unwrap();
    let compiler = root.path().join("usr/lib/loom/loom");
    fs::create_dir_all(compiler.parent().unwrap()).unwrap();
    // A compiler that compiles successfully but fails validation!
    fs::write(
        &compiler,
        "#!/bin/sh\ncase \"$1\" in\ncompile-service) cp \"$3\" \"$5\" ;;\nvalidate) exit 1 ;;\n*) exit 2 ;;\nesac\n",
    )
    .unwrap();
    fs::set_permissions(&compiler, fs::Permissions::from_mode(0o755)).unwrap();

    let generator = TemplateServiceGenerator::from_rclass(
        &Path::new(env!("CARGO_MANIFEST_DIR")).join("../../rclass/init-loom.toml"),
    )
    .unwrap();

    let dbus = activation_service(
        "dbus-demo",
        "[service.activation]\nkind=\"dbus\"\nname=\"org.example.Demo\"\nbus=\"system\"\nuser=\"root\"",
    );

    // Initial state: pre-existing safe activation file
    let dbus_act_file = root
        .path()
        .join("usr/share/dbus-1/system-services/org.example.Demo.service");
    fs::create_dir_all(dbus_act_file.parent().unwrap()).unwrap();
    fs::write(&dbus_act_file, b"existing safe activation").unwrap();

    let res = generator.render_service_set(&[dbus], root.path());
    assert!(res.is_err());

    // Verify atomic rollback: managed directory has no compiled artifact
    let compiled = root.path().join("usr/lib/loom/services/dbus-demo.toml");
    assert!(!compiled.exists());

    // Verify pre-existing activation artifact was restored!
    assert!(dbus_act_file.exists());
    assert_eq!(
        fs::read(&dbus_act_file).unwrap(),
        b"existing safe activation"
    );
}

#[test]
fn service_template_renders_atomically() {
    let root = tempfile::tempdir().unwrap();
    let generator = TemplateServiceGenerator {
        target_path_template: "/etc/init/${service.name}".into(),
        mode: 0o755,
        template: "exec ${service.command_json}\n".into(),
        dependency_aliases: BTreeMap::new(),
        service_dependency_suffix: String::new(),
        supported_types: vec![],
        compile_command: vec![],
        managed_directory: None,
        validate_command: None,
        enable_command: None,
        disable_command: None,
        is_enabled_command: None,
        activations: BTreeMap::new(),
    };
    let service = ServiceSpec {
        package: String::new(),
        name: "demo".into(),
        description: "demo".into(),
        command: vec!["/usr/bin/demo".into(), "--run".into()],
        stop_command: vec![],
        reload_command: vec![],
        user: "root".into(),
        group: "root".into(),
        working_dir: "/".into(),
        pid_file: String::new(),
        restart: "always".into(),
        service_type: "simple".into(),
        after: vec![],
        before: vec![],
        runtime: String::new(),
        activation: sage_sys::ServiceActivation::Service,
    };
    let path = generator.render_service(&service, root.path()).unwrap();
    assert_eq!(
        fs::read_to_string(path).unwrap(),
        "exec [\"/usr/bin/demo\",\"--run\"]\n"
    );
}

#[test]
fn target_paths_cannot_escape_sysroot() {
    let generator = TemplateServiceGenerator {
        target_path_template: "../../etc/passwd".into(),
        mode: 0o755,
        template: "service".into(),
        dependency_aliases: BTreeMap::new(),
        service_dependency_suffix: String::new(),
        supported_types: vec![],
        compile_command: vec![],
        managed_directory: None,
        validate_command: None,
        enable_command: None,
        disable_command: None,
        is_enabled_command: None,
        activations: BTreeMap::new(),
    };
    let service = ServiceSpec {
        package: String::new(),
        name: "demo".into(),
        description: "demo".into(),
        command: vec!["/usr/bin/demo".into()],
        stop_command: vec![],
        reload_command: vec![],
        user: "root".into(),
        group: "root".into(),
        working_dir: "/".into(),
        pid_file: String::new(),
        restart: "no".into(),
        service_type: "simple".into(),
        after: vec![],
        before: vec![],
        runtime: String::new(),
        activation: sage_sys::ServiceActivation::Service,
    };
    let root = tempfile::tempdir().unwrap();
    assert!(generator.render_service(&service, root.path()).is_err());
}

#[test]
fn trigger_path_variables_execute_once_per_kernel_slot() {
    let root = tempfile::tempdir().unwrap();
    fs::create_dir_all(root.path().join("usr/bin")).unwrap();
    let recorder = root.path().join("usr/bin/record-slot");
    fs::write(
        &recorder,
        "#!/bin/sh\nprintf '%s\\n' \"$4\" >> \"$SAGE_SYSROOT/result\"\n",
    )
    .unwrap();
    fs::set_permissions(&recorder, fs::Permissions::from_mode(0o755)).unwrap();
    let mut trigger: TriggerSpec =
        toml::from_str(include_str!("../../../../triggers/depmod.toml")).unwrap();
    trigger.exec[0] = "/usr/bin/record-slot".into();
    trigger.ignore_missing_binary = false;
    trigger.events = vec![TriggerEvent::PostRemove];
    let modified = [
        PathBuf::from("usr/lib/modules/6.12/a.ko"),
        PathBuf::from("usr/lib/modules/6.12/b.ko"),
        PathBuf::from("usr/lib/modules/6.13/c.ko"),
    ];

    assert!(
        TriggerEngine::execute_triggers(std::slice::from_ref(&trigger), &modified, root.path())
            .unwrap()
            .is_empty()
    );
    assert_eq!(
        TriggerEngine::execute_triggers_for(
            &[trigger.clone()],
            &modified,
            root.path(),
            TriggerEvent::PostRemove,
        )
        .unwrap(),
        ["depmod"]
    );
    assert_eq!(
        fs::read_to_string(root.path().join("result")).unwrap(),
        "6.12\n6.13\n"
    );
    let mut invalid = trigger.clone();
    invalid.exec.push("${unknown}".into());
    assert!(
        TriggerEngine::execute_triggers_for(
            &[invalid],
            &modified,
            root.path(),
            TriggerEvent::PostRemove,
        )
        .is_err()
    );
}

#[test]
fn sysusers_trigger_receives_the_transaction_root() {
    let root = tempfile::tempdir().unwrap();
    fs::create_dir_all(root.path().join("usr/bin")).unwrap();
    let recorder = root.path().join("usr/bin/record-root");
    fs::write(
        &recorder,
        "#!/bin/sh\nprintf '%s\\n' \"$SAGE_SYSROOT\" > \"$SAGE_SYSROOT/trigger-root\"\n",
    )
    .unwrap();
    fs::set_permissions(&recorder, fs::Permissions::from_mode(0o755)).unwrap();
    let mut trigger =
        TriggerSpec::parse(include_bytes!("../../../../triggers/depmod.toml")).unwrap();
    trigger.exec[0] = "/usr/bin/record-root".into();

    TriggerEngine::execute_triggers(
        &[trigger],
        &[PathBuf::from("usr/lib/modules/6.12/a.ko")],
        root.path(),
    )
    .unwrap();
    assert_eq!(
        fs::read_to_string(root.path().join("trigger-root")).unwrap(),
        format!("{}\n", root.path().display())
    );
}

#[test]
fn reconciliation_computes_dependency_closed_difference() {
    let mut universe = sage_solver::PackageUniverse::default();
    universe.insert(sage_core::Package::from_release(
        sage_core::PackageKey::new("main/system", "app", "0"),
        "1.0-1".parse().unwrap(),
        vec!["lib".parse().unwrap()],
        vec![],
    ));
    universe.insert(sage_core::Package::from_release(
        sage_core::PackageKey::new("main/system", "lib", "0"),
        "1.0-1".parse().unwrap(),
        vec![],
        vec![],
    ));
    let old = sage_db::InstalledPackage {
        key: sage_core::PackageKey::new("main/system", "old", "0"),
        version: "1.0-1".parse().unwrap(),
        arch: "amd64".into(),
        installed_size: 0,
        dependencies: vec![],
        provides: vec![],
        conflicts: vec![],
        files: vec![],
        config_hashes: BTreeMap::new(),
    };
    let config = SystemConfig {
        schema_version: 1,
        system: SystemMetadata {
            architecture: "amd64".into(),
            profile: "default".into(),
        },
        providers: BTreeMap::new(),
        packages: BTreeSet::from(["app".into()]),
    };
    let plan = ReconcilePlan::compute(&config, &[old], &universe, false).unwrap();
    assert_eq!(plan.install.len(), 2);
    assert_eq!(
        plan.remove,
        vec![sage_core::PackageKey::new("main/system", "old", "0")]
    );
}

#[test]
fn reconciliation_switches_and_prunes_virtual_provider() {
    let mut universe = sage_solver::PackageUniverse::default();
    universe.insert(sage_core::Package::from_release(
        sage_core::PackageKey::new("main/system", "app", "0"),
        "1-1".parse().unwrap(),
        vec!["virtual/libc".parse().unwrap()],
        vec![],
    ));
    for name in ["glibc", "musl"] {
        universe.insert(sage_core::Package::from_release(
            sage_core::PackageKey::new("main/system", name, "0"),
            "1-1".parse().unwrap(),
            vec![],
            vec!["virtual/libc".into()],
        ));
    }
    let glibc = sage_db::InstalledPackage {
        key: sage_core::PackageKey::new("main/system", "glibc", "0"),
        version: "1-1".parse().unwrap(),
        arch: "amd64".into(),
        installed_size: 0,
        dependencies: vec![],
        provides: vec!["virtual/libc".into()],
        conflicts: vec![],
        files: vec![],
        config_hashes: BTreeMap::new(),
    };
    let config = SystemConfig {
        schema_version: 1,
        system: SystemMetadata {
            architecture: "amd64".into(),
            profile: "default".into(),
        },
        providers: BTreeMap::from([("libc".into(), "musl".into())]),
        packages: BTreeSet::from(["app".into()]),
    };
    let plan = ReconcilePlan::compute(&config, &[glibc], &universe, false).unwrap();
    assert!(plan.install.iter().any(|(key, _)| key.name == "musl"));
    assert_eq!(plan.remove[0].name, "glibc");
}

#[test]
fn alternatives_choose_priority_and_publish_atomically() {
    let root = tempfile::tempdir().unwrap();
    let candidates = [
        Alternative {
            package: sage_core::PackageKey::new("main/system", "small", "0"),
            link: "usr/bin/vi".into(),
            target: "small-vi".into(),
            priority: 10,
        },
        Alternative {
            package: sage_core::PackageKey::new("main/system", "vim", "0"),
            link: "usr/bin/vi".into(),
            target: "vim".into(),
            priority: 50,
        },
    ];
    ProfileEngine::apply_alternatives(root.path(), &candidates).unwrap();
    assert_eq!(
        fs::read_link(root.path().join("usr/bin/vi")).unwrap(),
        PathBuf::from("vim")
    );
}

#[test]
fn transaction_diff_computes_accurate_preview() {
    let installed = vec![
        sage_db::InstalledPackage {
            key: sage_core::PackageKey::new("main/system", "curl", "0"),
            version: "8.0.0-1".parse().unwrap(),
            arch: "amd64".into(),
            installed_size: 1000,
            dependencies: vec![],
            provides: vec![],
            conflicts: vec![],
            files: vec![],
            config_hashes: BTreeMap::new(),
        },
        sage_db::InstalledPackage {
            key: sage_core::PackageKey::new("main/system", "old-tool", "0"),
            version: "1.0.0-1".parse().unwrap(),
            arch: "amd64".into(),
            installed_size: 500,
            dependencies: vec![],
            provides: vec![],
            conflicts: vec![],
            files: vec![],
            config_hashes: BTreeMap::new(),
        },
    ];

    let plan = sage_sys::TransactionPlan::new(
        vec![
            (
                sage_core::PackageKey::new("main/system", "curl", "0"),
                "8.5.0-1".parse().unwrap(),
            ),
            (
                sage_core::PackageKey::new("main/system", "new-app", "0"),
                "2.0.0-1".parse().unwrap(),
            ),
        ],
        vec![
            installed[1].clone(), // old-tool removed
        ],
        BTreeMap::from([(
            "init".into(),
            sage_core::PackageKey::new("main/system", "openrc", "0"),
        )]),
    );

    let diff = sage_sys::compute_transaction_diff(&plan, &installed, None);
    assert_eq!(diff.new_installs.len(), 1);
    assert_eq!(diff.new_installs[0].key.name, "new-app");
    assert_eq!(diff.upgrades.len(), 1);
    assert_eq!(diff.upgrades[0].key.name, "curl");
    assert_eq!(diff.upgrades[0].old_version.to_string(), "8.0.0-1");
    assert_eq!(diff.upgrades[0].new_version.to_string(), "8.5.0-1");
    assert_eq!(diff.removals.len(), 1);
    assert_eq!(diff.removals[0].key.name, "old-tool");
    assert_eq!(diff.provider_bindings.len(), 1);
}

#[test]
fn orphan_detection_identifies_unreferenced_packages() {
    let installed = vec![
        sage_db::InstalledPackage {
            key: sage_core::PackageKey::new("main/system", "root-app", "0"),
            version: "1.0.0-1".parse().unwrap(),
            arch: "amd64".into(),
            installed_size: 1000,
            dependencies: vec!["libfoo".parse().unwrap()],
            provides: vec![],
            conflicts: vec![],
            files: vec![],
            config_hashes: BTreeMap::new(),
        },
        sage_db::InstalledPackage {
            key: sage_core::PackageKey::new("main/system", "libfoo", "0"),
            version: "1.0.0-1".parse().unwrap(),
            arch: "amd64".into(),
            installed_size: 500,
            dependencies: vec![],
            provides: vec![],
            conflicts: vec![],
            files: vec![],
            config_hashes: BTreeMap::new(),
        },
        sage_db::InstalledPackage {
            key: sage_core::PackageKey::new("main/system", "abandoned-lib", "0"),
            version: "1.0.0-1".parse().unwrap(),
            arch: "amd64".into(),
            installed_size: 300,
            dependencies: vec![],
            provides: vec![],
            conflicts: vec![],
            files: vec![],
            config_hashes: BTreeMap::new(),
        },
    ];

    let config = SystemConfig {
        schema_version: 1,
        system: SystemMetadata {
            architecture: "amd64".into(),
            profile: "default".into(),
        },
        providers: BTreeMap::new(),
        packages: BTreeSet::from(["root-app".into()]),
    };

    let orphans = sage_sys::find_orphans(&installed, &config);
    assert_eq!(orphans.len(), 1);
    assert_eq!(orphans[0].key.name, "abandoned-lib");
}

#[test]
fn cache_clean_removes_temporary_and_package_files() {
    let root = tempfile::tempdir().unwrap();
    let cache_dir = root.path().join("var/cache/sage/channels/system");
    let lib_dir = root.path().join("var/lib/sage");
    fs::create_dir_all(&cache_dir).unwrap();
    fs::create_dir_all(&lib_dir).unwrap();

    fs::write(cache_dir.join("temp.sage-tmp-123"), b"garbage").unwrap();
    fs::write(cache_dir.join("partial.part-0"), b"partial").unwrap();
    fs::write(cache_dir.join("real.pkg.tar.zst"), b"package data").unwrap();
    fs::write(lib_dir.join(".services-config-old"), b"old config").unwrap();

    // Cleaning without `all` cleans temporary files only
    let report1 = sage_sys::clean_cache(root.path(), false).unwrap();
    assert_eq!(report1.files_removed, 3);
    assert!(cache_dir.join("real.pkg.tar.zst").exists());

    // Cleaning with `all = true` also cleans cached packages
    let report2 = sage_sys::clean_cache(root.path(), true).unwrap();
    assert_eq!(report2.files_removed, 1);
    assert!(!cache_dir.join("real.pkg.tar.zst").exists());
}

#[test]
fn live_upgrade_process_audit_scans_proc_safely() {
    let root = tempfile::tempdir().unwrap();
    let audits = sage_sys::audit_running_processes(root.path());
    sage_sys::print_process_audit(&audits);
}

#[test]
fn select_virtual_provider_handles_candidates_and_overrides() {
    use sage_core::PackageKey;

    let sys = PackageKey::new("main/system", "systemd", "0");
    let sysv = PackageKey::new("main/system", "sysvinit", "0");
    let candidates = vec![sys.clone(), sysv.clone()];

    // 1. Explicit CLI override matches candidate
    let chosen =
        sage_sys::select_virtual_provider("virtual/init", &candidates, Some("systemd"), false)
            .unwrap();
    assert_eq!(chosen, sys);

    let chosen_sysv =
        sage_sys::select_virtual_provider("virtual/init", &candidates, Some("sysvinit"), false)
            .unwrap();
    assert_eq!(chosen_sysv, sysv);

    // 2. Explicit CLI override does not match candidates -> returns error
    let err =
        sage_sys::select_virtual_provider("virtual/init", &candidates, Some("nonexistent"), false)
            .unwrap_err();
    assert!(err.to_string().contains("does not satisfy virtual/init"));

    // 3. Single candidate is automatically chosen without prompting
    let single = vec![sys.clone()];
    let chosen_single =
        sage_sys::select_virtual_provider("virtual/init", &single, None, false).unwrap();
    assert_eq!(chosen_single, sys);

    // 4. In non-interactive environment, default candidate is chosen
    let chosen_default =
        sage_sys::select_virtual_provider("virtual/init", &candidates, None, false).unwrap();
    assert_eq!(chosen_default, sys);
}

#[tokio::test]
async fn system_config_providers_are_dynamically_bound_for_any_interface() {
    // Tests that any interface configured in system.toml under [providers]
    // (e.g. awk = "gawk", cron = "cronie") is dynamically respected and bound
    // without hardcoding any specific interface in the codebase.
    let root = tempfile::tempdir().unwrap();
    let config_dir = root.path().join("etc/sage");
    std::fs::create_dir_all(&config_dir).unwrap();
    std::fs::write(
        config_dir.join("system.toml"),
        r#"schema_version = 1
packages = []

[system]
architecture = "amd64"
profile = "default"

[providers]
awk = "gawk"
cron = "cronie"
"#,
    )
    .unwrap();

    let cfg = sage_sys::SystemConfig::load(config_dir.join("system.toml")).unwrap();
    let preferences = cfg.provider_preferences("main/system").unwrap();
    assert_eq!(preferences.len(), 2);
    assert_eq!(preferences.get("virtual/awk").unwrap().name, "gawk");
    assert_eq!(preferences.get("virtual/cron").unwrap().name, "cronie");
}

#[test]
fn orphan_detection_uses_rebuild_channels_slots_and_virtual_routing() {
    use sage_core::PackageKey;
    let key = |channel, name, slot| PackageKey::new(channel, name, slot);
    let packages = vec![
        release(
            key("main/system", "app", "0"),
            "1-1",
            &["runtime/helper", "lib", "virtual/awk"],
            &[],
        ),
        release(key("main/system", "app", "1"), "1-1", &[], &[]),
        release(
            key("main/runtime", "helper", "0"),
            "1-1",
            &["virtual/libc"],
            &[],
        ),
        release(key("main/system", "lib", "0"), "1-1", &[], &[]),
        release(key("main/system", "lib", "2"), "1-1", &[], &[]),
        release(
            key("main/system", "libc", "2"),
            "1-1",
            &[],
            &["virtual/libc"],
        ),
        release(key("main/system", "awk", "2"), "1-1", &[], &["virtual/awk"]),
        release(
            key("main/system", "cron", "3"),
            "1-1",
            &[],
            &["virtual/cron"],
        ),
        release(
            key("main/system", "cron", "0"),
            "1-1",
            &[],
            &["virtual/cron"],
        ),
        release(key("main/system", "unused", "0"), "1-1", &[], &[]),
    ];
    let installed: Vec<_> = packages.iter().map(installed).collect();
    let orphans: BTreeSet<_> = find_orphans(&installed, &config(&["app"], &[("cron", "cron:3")]))
        .into_iter()
        .map(|package| package.key)
        .collect();
    assert_eq!(
        orphans,
        BTreeSet::from([
            key("main/system", "app", "1"),
            key("main/system", "lib", "2"),
            key("main/system", "cron", "0"),
            key("main/system", "unused", "0"),
        ])
    );
}

#[test]
fn transaction_preview_reports_binding_only_changes() {
    let key = sage_core::PackageKey::new("main/system", "gawk", "2");
    let plan = TransactionPlan::new(
        vec![],
        vec![],
        BTreeMap::from([("awk".into(), key.clone())]),
    );
    let diff = compute_transaction_diff(&plan, &[], None);
    let summary = diff.render_summary();
    assert!(summary.contains(&format!("awk -> {key}")));
    assert!(!summary.contains("No packages"));
    assert!(!summary.contains("virtual:so:"));
}

#[test]
fn clean_cache_does_not_traverse_symlinks_escaping_sysroot() {
    let sysroot = tempfile::tempdir().unwrap();
    let outside = tempfile::tempdir().unwrap();
    let victim_file = outside.path().join("victim.txt");
    fs::write(&victim_file, b"important host file").unwrap();

    let cache_dir = sysroot.path().join("var/cache/sage/main/pkg");
    fs::create_dir_all(&cache_dir).unwrap();

    // Create a symlink inside cache pointing to the external directory
    let escape_link = cache_dir.join("external_link");
    std::os::unix::fs::symlink(outside.path(), &escape_link).unwrap();

    // Create a regular cache file that should be cleaned
    let cache_file = cache_dir.join("old.pkg.tar.zst");
    fs::write(&cache_file, b"stale cache").unwrap();

    // Run clean_cache with all = true
    let report = clean_cache(sysroot.path(), true).unwrap();
    assert!(report.files_removed > 0);

    // The victim file outside sysroot MUST NOT be touched
    assert!(victim_file.exists());
    assert_eq!(fs::read(&victim_file).unwrap(), b"important host file");
}

#[test]
fn system_config_deserialization_fails_closed_on_invalid_providers_or_unknown_fields() {
    // Valid configuration
    let valid_toml = r#"
        schema_version = 1
        packages = ["curl"]

        [system]
        architecture = "x86_64"
        profile = "default"

        [providers]
        "virtual/editor" = "main/system:nano:0"
    "#;
    let config: Result<SystemConfig, _> = toml::from_str(valid_toml);
    assert!(
        config.is_ok(),
        "failed to parse valid config: {:?}",
        config.err()
    );

    // Fail-closed on non-table [providers]
    let invalid_providers = r#"
        schema_version = 1
        packages = ["curl"]
        providers = "malformed_string"

        [system]
        architecture = "x86_64"
        profile = "default"
    "#;
    let config: Result<SystemConfig, _> = toml::from_str(invalid_providers);
    assert!(config.is_err());

    // Fail-closed on unknown root keys
    let unknown_field = r#"
        schema_version = 1
        packages = ["curl"]
        unexpected_field = true

        [system]
        architecture = "x86_64"
        profile = "default"
    "#;
    let config: Result<SystemConfig, _> = toml::from_str(unknown_field);
    assert!(config.is_err());
}

#[test]
fn services_config_rejects_unknown_fields() {
    // Typo: `enable` instead of `enabled`
    let typo_toml = r#"
        schema_version = 1
        enable = ["sshd"]
    "#;
    let config: Result<ServicesConfig, _> = toml::from_str(typo_toml);
    assert!(config.is_err());

    let valid_toml = r#"
        schema_version = 1
        enabled = ["sshd"]
    "#;
    let config: Result<ServicesConfig, _> = toml::from_str(valid_toml);
    assert!(config.is_ok());
}

#[test]
fn available_services_prefers_installed_spec_over_rendered_history() {
    let sysroot = tempfile::tempdir().unwrap();

    // Installed service definition from package
    let installed_dir = sysroot.path().join("usr/share/sage/services");
    fs::create_dir_all(&installed_dir).unwrap();
    let installed_doc = r#"
        schema_version = 1
        [service]
        name = "demo"
        description = "newly installed package version"
        command = ["/usr/bin/demo"]
        user = "root"
        group = "root"
        working_dir = "/"
        restart = "always"
        type = "simple"
    "#;
    fs::write(installed_dir.join("demo.toml"), installed_doc).unwrap();

    // Historical rendered services document
    let lib_dir = sysroot.path().join("var/lib/sage");
    fs::create_dir_all(&lib_dir).unwrap();
    let old_service = ServiceSpec {
        package: String::new(),
        name: "demo".into(),
        description: "old historical rendered spec".into(),
        command: vec!["/usr/bin/demo-old".into()],
        stop_command: vec![],
        reload_command: vec![],
        user: "root".into(),
        group: "root".into(),
        working_dir: "/".into(),
        pid_file: String::new(),
        restart: "always".into(),
        service_type: "simple".into(),
        after: vec![],
        before: vec![],
        runtime: String::new(),
        activation: ServiceActivation::Service,
    };
    let generator = TemplateServiceGenerator::from_rclass(
        &Path::new(env!("CARGO_MANIFEST_DIR")).join("../../rclass/init-systemd.toml"),
    )
    .unwrap();
    let rendered_state = RenderedServicesState {
        schema_version: 1,
        provider: sage_core::PackageKey::new("main/system", "systemd", "0"),
        generator,
        services: vec![old_service],
        enabled: BTreeSet::new(),
    };
    let rendered_doc = toml::to_string_pretty(&rendered_state).unwrap();
    fs::write(lib_dir.join("rendered-services.toml"), rendered_doc).unwrap();

    let services = load_available_services(sysroot.path()).unwrap();
    let demo = services
        .iter()
        .find(|s| s.name == "demo")
        .expect("demo found");
    assert_eq!(demo.description, "newly installed package version");
}
