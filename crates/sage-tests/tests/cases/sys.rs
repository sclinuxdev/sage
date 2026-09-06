mod sys_tests {
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
            services: BTreeSet::new(),
        }
    }

    #[test]
    fn configured_provider_backtracks_with_and_without_a_consumer() {
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
            let plan = ReconcilePlan::compute(
                &config(&["guard"], &[("libc", "musl")]),
                &[],
                &universe,
                false,
            )
            .unwrap();
            assert_eq!(
                plan.provider_bindings,
                BTreeMap::from([("libc".into(), PackageKey::new("main/system", "glibc", "0"))])
            );
            assert_eq!(
                plan.install,
                vec![
                    (
                        PackageKey::new("main/system", "glibc", "0"),
                        "1-1".parse().unwrap()
                    ),
                    (
                        PackageKey::new("main/system", "guard", "0"),
                        "1-1".parse().unwrap()
                    ),
                ]
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
        let plan = ReconcilePlan::compute(
            &config(&["app", "preferred:1"], &[("libc", "preferred:1")]),
            &[],
            &universe,
            false,
        )
        .unwrap();
        assert_eq!(
            plan.provider_bindings["libc"],
            PackageKey::new("main/system", "selected", "2")
        );
        assert!(plan.install.contains(&(
            PackageKey::new("main/system", "selected", "2"),
            "2-1".parse().unwrap()
        )));
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
                ReconcilePlan::compute(&config(&["app"], &[]), &current, &universe, no_prune)
                    .unwrap();
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
            services: BTreeSet::new(),
        };
        let plan = ReconcilePlan::compute(&config, &[glibc], &universe, false).unwrap();
        assert!(plan
            .install
            .iter()
            .any(|(key, _)| key.name == "musl"));
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
}
