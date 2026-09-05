mod sys_tests {
    use sage_sys::*;
    use std::collections::{BTreeMap, BTreeSet};
    use std::fs;
    use std::os::unix::fs::PermissionsExt;
    use std::path::{Path, PathBuf};

    fn release(name: &str, version: &str, dependencies: &[&str], provides: &[&str]) -> sage_core::Package {
        sage_core::Package::from_release(
            sage_core::PackageKey::new("main/system", name, "0"),
            version.parse().unwrap(),
            dependencies.iter().map(|value| value.parse().unwrap()).collect(),
            provides.iter().map(|value| (*value).into()).collect(),
        )
    }

    fn provider_config(provider: &str, packages: &[&str]) -> SystemConfig {
        SystemConfig {
            schema_version: 1,
            system: SystemMetadata {
                architecture: "amd64".into(),
                profile: "default".into(),
            },
            providers: BTreeMap::from([("libc".into(), provider.into())]),
            packages: packages.iter().map(|value| (*value).into()).collect(),
            services: BTreeSet::new(),
        }
    }

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
        assert!(names.len() >= 8);
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
            "#!/bin/sh\nprintf '%s\\n' \"$2\" >> \"$SAGE_SYSROOT/result\"\n",
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

        assert!(TriggerEngine::execute_triggers(
            std::slice::from_ref(&trigger),
            &modified,
            root.path()
        )
        .unwrap()
        .is_empty());
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
        assert!(TriggerEngine::execute_triggers_for(
            &[invalid],
            &modified,
            root.path(),
            TriggerEvent::PostRemove,
        )
        .is_err());
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
        universe.insert(sage_core::Package::from_release(
            sage_core::PackageKey::new("main/system", "libc", "0"),
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
            services: BTreeSet::new(),
        };
        let plan = ReconcilePlan::compute(&config, std::slice::from_ref(&old), &universe, false)
            .unwrap();
        assert_eq!(plan.install.len(), 2);
        assert_eq!(
            plan.remove,
            vec![sage_core::PackageKey::new("main/system", "old", "0")]
        );
        let retained = ReconcilePlan::compute(&config, std::slice::from_ref(&old), &universe, true)
            .unwrap();
        assert!(retained.remove.is_empty());
        let mut installed_app = old.clone();
        installed_app.key = sage_core::PackageKey::new("main/system", "app", "0");
        installed_app.version = "2.0-1".parse().unwrap();
        installed_app.dependencies.clear();
        let mut stale_universe = sage_solver::PackageUniverse::default();
        stale_universe.insert(release("app", "1.0-1", &[], &[]));
        let retained = ReconcilePlan::compute(&config, &[installed_app], &stale_universe, false)
            .unwrap();
        assert!(retained.install.is_empty());
        assert!(retained.remove.is_empty());
        let mut runtime = old.clone();
        runtime.key = sage_core::PackageKey::new("main/runtime", "foo", "0");
        runtime.dependencies = vec!["main/system/libc".parse().unwrap()];
        let mut empty = config.clone();
        empty.packages.clear();
        let retained =
            ReconcilePlan::compute(&empty, &[runtime.clone()], &universe, false).unwrap();
        assert!(retained.install.iter().any(|(key, _)| key.name == "libc"));
        let mut concrete_root = runtime.clone();
        concrete_root.dependencies = vec!["main/system/lib".parse().unwrap()];
        let mut alias = old.clone();
        alias.key = sage_core::PackageKey::new("main/system", "alias", "0");
        alias.provides = vec!["lib".into()];
        alias.conflicts = vec!["lib".into()];
        let retained = ReconcilePlan::compute(
            &empty,
            &[concrete_root, alias.clone()],
            &universe,
            false,
        )
        .unwrap();
        assert!(retained.install.iter().any(|(key, _)| key.name == "lib"));
        assert_eq!(retained.remove, vec![alias.key]);
        let installed_lib = sage_db::InstalledPackage {
            key: sage_core::PackageKey::new("main/system", "libc", "0"),
            version: "1.0-1".parse().unwrap(),
            arch: "amd64".into(),
            installed_size: 0,
            dependencies: vec![],
            provides: vec![],
            conflicts: vec![],
            files: vec![],
            config_hashes: BTreeMap::new(),
        };
        let mut current_universe = sage_solver::PackageUniverse::default();
        current_universe.insert(sage_core::Package::from_release(
            runtime.key.clone(),
            runtime.version.clone(),
            runtime.dependencies.clone(),
            vec![],
        ));
        current_universe.insert(sage_core::Package::from_release(
            installed_lib.key.clone(),
            "2.0-1".parse().unwrap(),
            vec![],
            vec![],
        ));
        let retained = ReconcilePlan::compute(
            &empty,
            &[runtime.clone(), installed_lib.clone()],
            &current_universe,
            false,
        )
        .unwrap();
        assert!(retained.install.is_empty());
        assert!(retained.remove.is_empty());

        let mut compatible_runtime = runtime.clone();
        compatible_runtime.dependencies = vec!["main/system/libc >= 1.0-1".parse().unwrap()];
        let mut compatible_universe = sage_solver::PackageUniverse::default();
        compatible_universe.insert(sage_core::Package::from_release(
            compatible_runtime.key.clone(),
            compatible_runtime.version.clone(),
            compatible_runtime.dependencies.clone(),
            vec![],
        ));
        compatible_universe.insert(sage_core::Package::from_release(
            installed_lib.key.clone(),
            "2.0-1".parse().unwrap(),
            vec![],
            vec![],
        ));
        compatible_universe.insert(release(
            "new-consumer",
            "1.0-1",
            &["libc >= 2.0-1"],
            &[],
        ));
        let mut compatible_config = empty.clone();
        compatible_config.packages.insert("new-consumer".into());
        let retained = ReconcilePlan::compute(
            &compatible_config,
            &[compatible_runtime, installed_lib],
            &compatible_universe,
            false,
        )
        .unwrap();
        assert!(retained
            .install
            .iter()
            .any(|(key, version)| key.name == "libc" && *version == "2.0-1".parse().unwrap()));

        let mut virtual_root = runtime.clone();
        virtual_root.dependencies = vec!["virtual/libc".parse().unwrap()];
        let mut glibc = old.clone();
        glibc.key = sage_core::PackageKey::new("main/system", "glibc", "0");
        glibc.provides = vec!["virtual/libc".into()];
        let mut musl = glibc.clone();
        musl.key = sage_core::PackageKey::new("main/system", "musl", "0");
        let mut provider_universe = sage_solver::PackageUniverse::default();
        for package in [&virtual_root, &glibc, &musl] {
            provider_universe.insert(sage_core::Package::from_release(
                package.key.clone(),
                package.version.clone(),
                package.dependencies.clone(),
                package.provides.clone(),
            ));
        }
        let mut provider_config = empty.clone();
        provider_config.providers.insert("libc".into(), "musl".into());
        let retained = ReconcilePlan::compute(
            &provider_config,
            &[virtual_root.clone(), glibc.clone(), musl.clone()],
            &provider_universe,
            false,
        )
        .unwrap();
        assert_eq!(retained.remove, vec![glibc.key.clone()]);

        let mut guarded_universe = provider_universe.clone();
        let mut guard = release("guard", "1.0-1", &[], &[]);
        guard.conflicts.push("glibc".into());
        guarded_universe.insert(guard);
        let mut unconfigured = empty.clone();
        unconfigured.packages.insert("guard".into());
        let retained = ReconcilePlan::compute(
            &unconfigured,
            &[virtual_root.clone(), glibc.clone(), musl.clone()],
            &guarded_universe,
            false,
        )
        .unwrap();
        assert!(retained.install.iter().any(|(key, _)| key.name == "guard"));
        assert_eq!(retained.remove, vec![glibc.key.clone()]);
        let retained = ReconcilePlan::compute(
            &unconfigured,
            &[virtual_root.clone(), glibc.clone()],
            &guarded_universe,
            false,
        )
        .unwrap();
        assert!(retained.install.iter().any(|(key, _)| key.name == "musl"));
        assert_eq!(retained.remove, vec![glibc.key.clone()]);

        let mut switch_universe = provider_universe.clone();
        let mut conflicting_musl = sage_core::Package::from_release(
            musl.key.clone(),
            musl.version.clone(),
            vec![],
            musl.provides.clone(),
        );
        conflicting_musl.conflicts.push("glibc".into());
        switch_universe.insert(conflicting_musl);
        let retained = ReconcilePlan::compute(
            &provider_config,
            &[virtual_root, glibc.clone()],
            &switch_universe,
            false,
        )
        .unwrap();
        assert_eq!(retained.install, vec![(musl.key, musl.version)]);
        assert_eq!(retained.remove, vec![glibc.key]);

        // Reconstructing an installed provider release adds its key to the
        // symbol index. A repository release under that key which dropped the
        // symbol must not satisfy the virtual dependency.
        let mut stale_provider = old.clone();
        stale_provider.key = sage_core::PackageKey::new("main/system", "provider", "0");
        stale_provider.provides = vec!["virtual/runtime-api".into()];
        let mut virtual_consumer = runtime.clone();
        virtual_consumer.dependencies = vec!["virtual/runtime-api".parse().unwrap()];
        let mut changed_provider_universe = sage_solver::PackageUniverse::default();
        changed_provider_universe.insert(sage_core::Package::from_release(
            virtual_consumer.key.clone(),
            virtual_consumer.version.clone(),
            virtual_consumer.dependencies.clone(),
            vec![],
        ));
        changed_provider_universe.insert(release(
            "provider",
            "2.0-1",
            &[],
            &[],
        ));
        changed_provider_universe.insert(release(
            "requires-new-provider",
            "1.0-1",
            &["provider >= 2.0-1"],
            &[],
        ));
        let mut changed_provider_config = empty.clone();
        changed_provider_config
            .packages
            .insert("requires-new-provider".into());
        assert!(ReconcilePlan::compute(
            &changed_provider_config,
            &[virtual_consumer, stale_provider.clone()],
            &changed_provider_universe,
            false,
        )
        .is_err());

        // Virtual conflicts must also use exact release metadata: the retained
        // provider can upgrade to v2, which no longer advertises the symbol.
        let mut guard = release("guard", "1.0-1", &[], &[]);
        guard.conflicts.push("virtual/runtime-api".into());
        changed_provider_universe.insert(guard.clone());
        let mut conflict_config = empty.clone();
        conflict_config.packages.insert("guard".into());
        conflict_config.packages.insert("provider".into());
        let plan = ReconcilePlan::compute(
            &conflict_config,
            std::slice::from_ref(&stale_provider),
            &changed_provider_universe,
            false,
        )
        .unwrap();
        assert!(plan.install.contains(&(
            stale_provider.key.clone(),
            "2.0-1".parse().unwrap(),
        )));
        let guard_coordinate = guard.coordinate();
        assert!(plan.install.contains(&(guard_coordinate.key, guard_coordinate.version)));

        // Without the non-providing release, the real conflict still rejects v1.
        let mut conflict_universe = sage_solver::PackageUniverse::default();
        conflict_universe.insert(guard);
        assert!(ReconcilePlan::compute(
            &conflict_config,
            &[stale_provider],
            &conflict_universe,
            false,
        )
        .is_err());

        let mut old_release = release("old", "1.0-1", &[], &[]);
        old_release.conflicts.push("app".into());
        universe.insert(old_release);
        universe.insert(release("old", "2.0-1", &[], &[]));
        assert!(ReconcilePlan::compute(&config, std::slice::from_ref(&old), &universe, true).is_err());
    }

    #[test]
    fn reconciliation_switches_and_prunes_virtual_provider() {
        let mut universe = sage_solver::PackageUniverse::default();
        universe.insert(release("app", "1-1", &["virtual/libc"], &[]));
        for name in ["glibc", "musl"] {
            universe.insert(release(name, "1-1", &[], &["virtual/libc"]));
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
        let config = provider_config("musl", &["app"]);
        let plan = ReconcilePlan::compute(&config, &[glibc], &universe, false).unwrap();
        assert!(plan
            .install
            .iter()
            .any(|(key, _)| key.name == "musl"));
        assert_eq!(plan.remove[0].name, "glibc");
    }

    #[test]
    fn reconciliation_backtracks_from_conflicting_provider_preference() {
        let mut universe = sage_solver::PackageUniverse::default();
        let mut app = release("app", "1-1", &["virtual/libc"], &[]);
        app.conflicts.push("musl".into());
        universe.insert(app);
        for name in ["glibc", "musl"] {
            universe.insert(release(name, "1-1", &[], &["virtual/libc"]));
        }
        let config = provider_config("musl", &["app"]);

        let plan = ReconcilePlan::compute(&config, &[], &universe, false).unwrap();
        assert!(plan.install.iter().any(|(key, _)| key.name == "glibc"));
        assert!(!plan.install.iter().any(|(key, _)| key.name == "musl"));
        assert_eq!(plan.provider_bindings["libc"].name, "glibc");

        let mut universe = sage_solver::PackageUniverse::default();
        let mut guard = release("guard", "1-1", &[], &[]);
        guard.conflicts.push("systemd".into());
        universe.insert(guard);
        for name in ["systemd", "loom"] {
            universe.insert(release(name, "1-1", &[], &["virtual/init"]));
        }
        let mut config = provider_config("systemd", &["guard"]);
        config.providers = BTreeMap::from([("init".into(), "systemd".into())]);
        let plan = ReconcilePlan::compute(&config, &[], &universe, false).unwrap();
        assert_eq!(plan.provider_bindings["init"].name, "loom");
    }

    #[test]
    fn reconciliation_binds_the_provider_selected_by_the_virtual_proxy() {
        let mut universe = sage_solver::PackageUniverse::default();
        universe.insert(release("app", "1-1", &["virtual/libc >= 2-1"], &[]));
        universe.insert(release("a-explicit", "1-1", &[], &["virtual/libc"]));
        universe.insert(release("z-selected", "2-1", &[], &["virtual/libc"]));
        let config = provider_config("a-explicit", &["app", "a-explicit"]);

        let plan = ReconcilePlan::compute(&config, &[], &universe, false).unwrap();
        assert_eq!(plan.provider_bindings["libc"].name, "z-selected");
    }

    #[test]
    fn reconciliation_rejects_multiple_bindings_for_one_interface() {
        let mut universe = sage_solver::PackageUniverse::default();
        universe.insert(release("old-app", "1-1", &["virtual/libc < 2-1"], &[]));
        universe.insert(release("new-app", "1-1", &["virtual/libc >= 2-1"], &[]));
        universe.insert(release("old-libc", "1-1", &[], &["virtual/libc"]));
        universe.insert(release("new-libc", "2-1", &[], &["virtual/libc"]));
        let config = provider_config("new-libc", &["old-app", "new-app"]);
        assert!(ReconcilePlan::compute(&config, &[], &universe, false).is_err());

        let mut universe = sage_solver::PackageUniverse::default();
        universe.insert(release("systemd", "1-1", &[], &["virtual/init"]));
        let mut config = provider_config("systemd", &[]);
        config.providers = BTreeMap::from([("init".into(), "systemd".into())]);
        let plan = ReconcilePlan::compute(&config, &[], &universe, false).unwrap();
        assert_eq!(plan.install[0].0.name, "systemd");
        assert_eq!(plan.provider_bindings["init"].name, "systemd");
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
}
