mod solver_tests {
    use sage_core::PackageKey;
    use sage_solver::*;
    use std::collections::BTreeMap;

    fn release(channel: &str, name: &str, version: &str, dependencies: &[&str]) -> PackageRelease {
        sage_core::Package::from_release(
            PackageKey::new(channel, name, "0"),
            version.parse().unwrap(),
            dependencies
                .iter()
                .map(|value| value.parse().unwrap())
                .collect(),
            vec![],
        )
    }

    #[test]
    fn backtracks_from_newest_conflicting_release() {
        let mut universe = PackageUniverse::default();
        universe.insert(release("main/system", "app", "1.0-1", &["lib < 2.0-1"]));
        universe.insert(release("main/system", "lib", "1.0-1", &[]));
        universe.insert(release("main/system", "lib", "2.0-1", &[]));
        let app = PackageKey::new("main/system", "app", "0");
        let solution = SageSolver::new(&universe).resolve(&[app]).unwrap();
        assert_eq!(
            solution[&PackageKey::new("main/system", "lib", "0")],
            "1.0-1".parse().unwrap()
        );
    }

    #[test]
    fn subchannel_inherits_system_virtual_provider() {
        let mut universe = PackageUniverse::default();
        universe.insert(release(
            "main/python3.13",
            "numpy",
            "2.0-1",
            &["virtual/libc"],
        ));
        let mut libc = release("main/system", "glibc", "2.40-1", &[]);
        libc.provides.push("virtual/libc".into());
        universe.insert(libc);
        let numpy = PackageKey::new("main/python3.13", "numpy", "0");
        let solution = SageSolver::new(&universe).resolve(&[numpy]).unwrap();
        assert!(solution.contains_key(&PackageKey::new("main/system", "glibc", "0")));
    }

    #[test]
    fn virtual_dependencies_filter_concrete_provider_versions() {
        let mut universe = PackageUniverse::default();
        universe.insert(release(
            "main/system",
            "app",
            "1-1",
            &["virtual/libc >= 2-1"],
        ));
        let mut old = release("main/system", "glibc", "1-1", &[]);
        old.provides.push("virtual/libc".into());
        universe.insert(old);
        let mut current = release("main/system", "musl", "2-1", &[]);
        current.provides.push("virtual/libc".into());
        universe.insert(current);
        let solution = SageSolver::new(&universe)
            .resolve(&[PackageKey::new("main/system", "app", "0")])
            .unwrap();
        assert!(solution.contains_key(&PackageKey::new("main/system", "musl", "0")));
        assert!(!solution.contains_key(&PackageKey::new("main/system", "glibc", "0")));
    }

    #[test]
    fn provider_symbol_dependencies_resolve_when_no_concrete_package_exists() {
        let mut universe = PackageUniverse::default();
        universe.insert(release(
            "main/system",
            "app",
            "1-1",
            &["system/zlib-libs >= 1.3-1"],
        ));
        let mut zlib = release("main/system", "zlib", "1.3.2-1", &[]);
        zlib.provides.push("zlib-libs".into());
        universe.insert(zlib);
        let solution = SageSolver::new(&universe)
            .resolve(&[PackageKey::new("main/system", "app", "0")])
            .unwrap();
        assert_eq!(
            solution[&PackageKey::new("main/system", "zlib", "0")],
            "1.3.2-1".parse().unwrap()
        );
    }

    #[test]
    fn locked_satisfying_version_wins_over_newest() {
        let mut universe = PackageUniverse::default();
        universe.insert(release("main/system", "lib", "1.0-1", &[]));
        universe.insert(release("main/system", "lib", "2.0-1", &[]));
        let key = PackageKey::new("main/system", "lib", "0");
        let solver = SageSolver::with_locked(&universe, [(key.clone(), "1.0-1".parse().unwrap())]);
        assert_eq!(
            solver.resolve(std::slice::from_ref(&key)).unwrap()[&key],
            "1.0-1".parse().unwrap()
        );
    }

    #[test]
    fn locked_virtual_provider_version_wins_over_newest() {
        let mut universe = PackageUniverse::default();
        universe.insert(release("main/system", "app", "1-1", &["virtual/libc"]));
        let mut seed = release("main/system", "glibc", "2.44-0", &[]);
        seed.provides.push("virtual/libc".into());
        universe.insert(seed);
        let mut repository = release("main/system", "glibc", "2.44-5", &[]);
        repository.provides.push("virtual/libc".into());
        universe.insert(repository);
        let key = PackageKey::new("main/system", "glibc", "0");
        let solution = SageSolver::with_locked(
            &universe,
            [(key.clone(), "2.44-0".parse().unwrap())],
        )
        .resolve(&[PackageKey::new("main/system", "app", "0")])
        .unwrap();
        assert_eq!(solution[&key], "2.44-0".parse().unwrap());
    }

    #[test]
    fn root_dependency_constraints_apply_to_build_environments() {
        let mut universe = PackageUniverse::default();
        universe.insert(release("main/system", "cmake", "3.20-1", &[]));
        universe.insert(release("main/system", "cmake", "4.0-1", &[]));
        let dependency = "cmake < 4.0-1".parse().unwrap();
        let solution = SageSolver::new(&universe)
            .resolve_dependencies("main/system", &[dependency])
            .unwrap();
        assert_eq!(
            solution[&PackageKey::new("main/system", "cmake", "0")],
            "3.20-1".parse().unwrap()
        );
    }

    #[test]
    fn short_subchannel_dependencies_inherit_repository_root() {
        let mut universe = PackageUniverse::default();
        universe.insert(sage_core::Package::from_release(
            PackageKey::new("main/gcc16", "gcc", "16"),
            "16.2.0-1".parse().unwrap(),
            vec![],
            vec![],
        ));
        let dependency = "gcc16/gcc:16".parse().unwrap();
        let solution = SageSolver::new(&universe)
            .resolve_dependencies("main/system", &[dependency])
            .unwrap();
        assert!(solution.contains_key(&PackageKey::new("main/gcc16", "gcc", "16")));
    }

    #[test]
    fn canonical_cross_channel_dependencies_are_not_double_prefixed() {
        let mut universe = PackageUniverse::default();
        universe.insert(release("main/system", "gmp", "6.3.0-1", &[]));
        let dependency = "main/system/gmp".parse().unwrap();
        let solution = SageSolver::new(&universe)
            .resolve_dependencies("main/gcc16", &[dependency])
            .unwrap();
        assert!(solution.contains_key(&PackageKey::new("main/system", "gmp", "0")));
    }

    #[test]
    fn configured_virtual_provider_is_preferred_but_can_backtrack() {
        let mut universe = PackageUniverse::default();
        universe.insert(release("main/system", "app", "1-1", &["virtual/libc"]));
        let mut glibc = release("main/system", "glibc", "1-1", &[]);
        glibc.provides.push("virtual/libc".into());
        universe.insert(glibc);
        let mut musl = release("main/system", "musl", "1-1", &[]);
        musl.provides.push("virtual/libc".into());
        universe.insert(musl);
        let app = PackageKey::new("main/system", "app", "0");
        let preferred = PackageKey::new("main/system", "musl", "0");
        let solution = SageSolver::new(&universe)
            .prefer_providers([("virtual/libc".into(), preferred.clone())])
            .resolve(std::slice::from_ref(&app))
            .unwrap();
        assert!(solution.contains_key(&preferred));

        let mut guard = release("main/system", "guard", "1-1", &[]);
        guard.conflicts.push("musl".into());
        universe.insert(guard);
        let solution = SageSolver::new(&universe)
            .prefer_providers([("virtual/libc".into(), preferred)])
            .resolve(&[app, PackageKey::new("main/system", "guard", "0")])
            .unwrap();
        assert!(solution.contains_key(&PackageKey::new("main/system", "glibc", "0")));
        assert!(!solution.contains_key(&PackageKey::new("main/system", "musl", "0")));
    }

    #[test]
    fn concrete_provider_fallbacks_do_not_create_system_bindings() {
        let mut universe = PackageUniverse::default();
        universe.insert(release("main/system", "old-app", "1-1", &["lib < 2-1"]));
        universe.insert(release("main/system", "new-app", "1-1", &["lib >= 2-1"]));
        let mut old_provider = release("main/system", "old-lib", "1-1", &[]);
        old_provider.provides.push("lib".into());
        universe.insert(old_provider);
        let mut new_provider = release("main/system", "new-lib", "2-1", &[]);
        new_provider.provides.push("lib".into());
        universe.insert(new_provider);

        let (solution, bindings) = SageSolver::new(&universe)
            .resolve_with_provider_bindings(
                &[
                    PackageKey::new("main/system", "old-app", "0"),
                    PackageKey::new("main/system", "new-app", "0"),
                ],
                &BTreeMap::new(),
            )
            .unwrap();
        assert!(solution.contains_key(&PackageKey::new("main/system", "old-lib", "0")));
        assert!(solution.contains_key(&PackageKey::new("main/system", "new-lib", "0")));
        assert!(bindings.is_empty());
    }

    #[test]
    fn unconfigured_virtuals_do_not_create_system_bindings() {
        let mut universe = PackageUniverse::default();
        universe.insert(release(
            "main/system",
            "old-app",
            "1-1",
            &["virtual/libc < 2-1"],
        ));
        universe.insert(release(
            "main/system",
            "new-app",
            "1-1",
            &["virtual/libc >= 2-1"],
        ));
        let mut old_provider = release("main/system", "old-libc", "1-1", &[]);
        old_provider.provides.push("virtual/libc".into());
        universe.insert(old_provider);
        let mut new_provider = release("main/system", "new-libc", "2-1", &[]);
        new_provider.provides.push("virtual/libc".into());
        universe.insert(new_provider);

        let (solution, bindings) = SageSolver::new(&universe)
            .resolve_with_provider_bindings(
                &[
                    PackageKey::new("main/system", "old-app", "0"),
                    PackageKey::new("main/system", "new-app", "0"),
                ],
                &BTreeMap::new(),
            )
            .unwrap();
        assert!(solution.contains_key(&PackageKey::new("main/system", "old-libc", "0")));
        assert!(solution.contains_key(&PackageKey::new("main/system", "new-libc", "0")));
        assert!(bindings.is_empty());
    }

    #[test]
    fn package_conflict_participates_in_version_backtracking() {
        let mut universe = PackageUniverse::default();
        universe.insert(release("main/system", "app", "1-1", &[]));
        let mut newest = release("main/system", "app", "2-1", &[]);
        newest.conflicts.push("lib".into());
        universe.insert(newest);
        universe.insert(release("main/system", "lib", "1-1", &[]));
        let app = PackageKey::new("main/system", "app", "0");
        let solution = SageSolver::new(&universe)
            .resolve(&[app.clone(), PackageKey::new("main/system", "lib", "0")])
            .unwrap();
        assert_eq!(solution[&app], "1-1".parse().unwrap());
    }

    #[test]
    fn malformed_conflicts_are_rejected_instead_of_ignored() {
        let mut universe = PackageUniverse::default();
        let mut app = release("main/system", "app", "1-1", &[]);
        app.conflicts.push("lib >= invalid".into());
        universe.insert(app);
        let error = SageSolver::new(&universe)
            .resolve(&[PackageKey::new("main/system", "app", "0")])
            .unwrap_err();
        assert!(matches!(error, SolverError::InvalidMetadata(_)));
        assert!(error.to_string().contains("lib >= invalid"));
    }
}
