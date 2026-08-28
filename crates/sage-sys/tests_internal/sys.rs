use super::*;

#[test]
fn administrator_trigger_overrides_vendor_definition() {
    let root = tempfile::tempdir().unwrap();
    for directory in ["usr/share/sage/triggers", "etc/sage/triggers.d"] {
        fs::create_dir_all(root.path().join(directory)).unwrap();
    }
    let trigger = |priority| {
        format!("schema_version=1\nname=\"cache\"\ndescription=\"x\"\non_paths=[\"usr/lib/*\"]\nexec=[\"/missing\"]\npriority={priority}\nignore_missing_binary=true\n")
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
    assert!(!names.is_empty());
}

#[test]
fn standard_init_rclasses_are_parseable() {
    let directory = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../rclass");
    let mut files: Vec<_> = fs::read_dir(directory)
        .unwrap()
        .collect::<Result<_, _>>()
        .unwrap();
    files.sort_by_key(|entry| entry.file_name());
    let mut count = 0;
    for file in files {
        let name = file.file_name();
        if !name.to_string_lossy().starts_with("init-") {
            continue;
        }
        TemplateServiceGenerator::from_rclass(&file.path()).unwrap();
        count += 1;
    }
    assert!(count >= 2);
}

#[test]
fn service_template_renders_atomically() {
    let root = tempfile::tempdir().unwrap();
    let generator = TemplateServiceGenerator {
        supported_types: vec!["simple".into()],
        target_path_template: "/etc/init/${service.name}".into(),
        mode: 0o755,
        template: "exec ${service.command_json}\n".into(),
        dependency_aliases: BTreeMap::new(),
        service_dependency_suffix: String::new(),
        validate_command: None,
        enable_command: None,
        disable_command: None,
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
    };
    let path = generator.render_service(&service, root.path()).unwrap();
    assert_eq!(
        fs::read_to_string(path).unwrap(),
        "exec [\"/usr/bin/demo\",\"--run\"]\n"
    );
}

#[test]
fn service_template_maps_dependencies_and_exposes_lifecycle_commands() {
    let root = tempfile::tempdir().unwrap();
    let generator = TemplateServiceGenerator {
        supported_types: vec!["simple".into()],
        target_path_template: "/etc/init/${service.name}".into(),
        mode: 0o644,
        template: concat!(
            "after=${service.after_space}\n",
            "before=${service.before_space}\n",
            "stop=${service.stop_command_quoted}\n",
            "reload=${service.reload_command_json}\n",
            "runtime=${service.runtime_json}\n",
        )
        .into(),
        dependency_aliases: BTreeMap::from([("network".into(), "network.target".into())]),
        service_dependency_suffix: ".service".into(),
        validate_command: None,
        enable_command: None,
        disable_command: None,
    };
    let service = ServiceSpec {
        package: String::new(),
        name: "demo".into(),
        description: "demo".into(),
        command: vec!["/usr/bin/demo".into(), "two words".into()],
        stop_command: vec!["/usr/bin/demo".into(), "--stop".into()],
        reload_command: vec!["/usr/bin/demo".into(), "--reload".into()],
        user: "root".into(),
        group: "root".into(),
        working_dir: "/".into(),
        pid_file: "/run/demo.pid".into(),
        restart: "on-failure".into(),
        service_type: "simple".into(),
        after: vec!["network".into(), "logger".into()],
        before: vec!["consumer".into()],
        runtime: "runtime/python:3.14".into(),
    };

    let path = generator.render_service(&service, root.path()).unwrap();
    assert_eq!(
        fs::read_to_string(path).unwrap(),
        concat!(
            "after=network.target logger.service\n",
            "before=consumer.service\n",
            "stop=\"/usr/bin/demo\" \"--stop\"\n",
            "reload=[\"/usr/bin/demo\",\"--reload\"]\n",
            "runtime=\"runtime/python:3.14\"\n",
        )
    );
}

#[test]
fn target_paths_cannot_escape_sysroot() {
    assert!(target_path(Path::new("/root"), Path::new("../../etc/passwd")).is_err());
}

#[test]
fn trigger_path_variables_execute_once_per_kernel_slot() {
    let root = tempfile::tempdir().unwrap();
    fs::create_dir_all(root.path().join("usr/bin")).unwrap();
    let recorder = root.path().join("usr/bin/record-slot");
    fs::write(
        &recorder,
        "#!/bin/sh\nprintf '%s\\n' \"$2\" >> \"$SAGE_SYSROOT/result\"\n",
    )
    .unwrap();
    fs::set_permissions(&recorder, fs::Permissions::from_mode(0o755)).unwrap();
    let mut trigger: TriggerSpec =
        toml::from_str(include_str!("../../../triggers/depmod.toml")).unwrap();
    trigger.exec[0] = "/usr/bin/record-slot".into();
    trigger.ignore_missing_binary = false;
    trigger.events = vec![TriggerEvent::PostRemove];
    let modified = [
        PathBuf::from("usr/lib/modules/6.12/a.ko"),
        PathBuf::from("usr/lib/modules/6.12/b.ko"),
        PathBuf::from("usr/lib/modules/6.13/c.ko"),
    ];

    assert!(TriggerEngine::execute_triggers(
        std::slice::from_ref(&trigger),
        &modified,
        root.path()
    )
    .unwrap()
    .is_empty());
    assert_eq!(
        TriggerEngine::execute_triggers_for(
            &[trigger],
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
    assert!(expand_trigger_argument("${unknown}", &modified[0], root.path()).is_err());
}

#[test]
fn reconciliation_computes_dependency_closed_difference() {
    let mut universe = sage_solver::PackageUniverse::default();
    universe.insert(sage_solver::PackageRelease {
        key: sage_core::PackageKey::new("main/system", "app", "0"),
        version: "1.0-1".parse().unwrap(),
        dependencies: vec!["lib".parse().unwrap()],
        provides: vec![],
    });
    universe.insert(sage_solver::PackageRelease {
        key: sage_core::PackageKey::new("main/system", "lib", "0"),
        version: "1.0-1".parse().unwrap(),
        dependencies: vec![],
        provides: vec![],
    });
    let old = sage_db::InstalledPackage {
        key: sage_core::PackageKey::new("main/system", "old", "0"),
        version: "1.0-1".parse().unwrap(),
        arch: "amd64".into(),
        installed_size: 0,
        dependencies: vec![],
        provides: vec![],
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
        services: BTreeSet::new(),
    };
    let plan = ReconcilePlan::compute(&config, &[old], &universe, false).unwrap();
    assert_eq!(plan.install.len(), 2);
    assert_eq!(
        plan.remove,
        vec![sage_core::PackageKey::new("main/system", "old", "0")]
    );
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
fn sysusers_are_applied_without_an_init_provider() {
    let root = tempfile::tempdir().unwrap();
    fs::create_dir(root.path().join("etc")).unwrap();
    fs::write(
        root.path().join("etc/passwd"),
        "root:x:0:0:root:/root:/usr/bin/sh\n",
    )
    .unwrap();
    fs::write(root.path().join("etc/group"), "root:x:0:\n").unwrap();
    fs::write(root.path().join("etc/shadow"), "root:!*:::::::\n").unwrap();
    let declarations = vec![SysuserDeclaration {
        kind: "user".into(),
        name: "messagebus".into(),
        id: Some(18),
        description: "D-Bus Message Bus".into(),
        home: "/run/dbus".into(),
        shell: "/usr/bin/nologin".into(),
    }];

    SysusersEngine::reconcile(root.path(), &declarations).unwrap();
    SysusersEngine::reconcile(root.path(), &declarations).unwrap();

    assert_eq!(
        fs::read_to_string(root.path().join("etc/passwd")).unwrap(),
        concat!(
            "root:x:0:0:root:/root:/usr/bin/sh\n",
            "messagebus:x:18:18:D-Bus Message Bus:/run/dbus:/usr/bin/nologin\n",
        )
    );
    assert_eq!(
        fs::read_to_string(root.path().join("etc/group")).unwrap(),
        "root:x:0:\nmessagebus:x:18:\n"
    );
    assert_eq!(
        fs::metadata(root.path().join("etc/shadow"))
            .unwrap()
            .permissions()
            .mode()
            & 0o777,
        0o600
    );
}

#[test]
fn sysusers_reject_conflicting_numeric_ids() {
    let root = tempfile::tempdir().unwrap();
    fs::create_dir(root.path().join("etc")).unwrap();
    fs::write(
        root.path().join("etc/passwd"),
        "other:x:18:18::/:/usr/bin/nologin\n",
    )
    .unwrap();
    fs::write(root.path().join("etc/group"), "other:x:18:\n").unwrap();
    let declaration = SysuserDeclaration {
        kind: "user".into(),
        name: "messagebus".into(),
        id: Some(18),
        description: String::new(),
        home: "/".into(),
        shell: "/usr/bin/nologin".into(),
    };
    assert!(SysusersEngine::reconcile(root.path(), &[declaration]).is_err());
}
