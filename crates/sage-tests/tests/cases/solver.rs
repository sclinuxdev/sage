mod solver_tests {
    use sage_core::PackageKey;
    use sage_solver::*;

    fn release(channel: &str, name: &str, version: &str, dependencies: &[&str]) -> PackageRelease {
        PackageRelease {
            key: PackageKey::new(channel, name, "0"),
            version: version.parse().unwrap(),
            dependencies: dependencies
                .iter()
                .map(|value| value.parse().unwrap())
                .collect(),
            provides: vec![],
        }
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
}
