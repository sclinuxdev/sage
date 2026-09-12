//! Provider choices must survive the actual CLI/application transaction boundary.

use clap::Parser;
use sage_core::PackageKey;
use sage_tests::{PackageSpec, TortureLab};
use std::fs;

fn package(
    name: &str,
    slot: &str,
    version: u32,
    provides: &[&str],
    dependencies: &[&str],
) -> PackageSpec {
    let mut package = PackageSpec::new(
        "system",
        name,
        version,
        &format!("usr/share/{name}-{slot}"),
        name,
    );
    package.slot = slot.into();
    package.provides = provides.iter().map(|value| (*value).into()).collect();
    package.dependencies = dependencies.iter().map(|value| (*value).into()).collect();
    package
}

async fn install(
    lab: &TortureLab,
    name: &str,
    overrides: &[(&str, &str)],
    save: bool,
    dry_run: bool,
) -> anyhow::Result<()> {
    sage::execute(sage::Cli {
        verbose: false,
        dry_run,
        root: lab.root().into(),
        command: sage::Commands::Install {
            packages: vec![name.into()],
            channel: None,
            no_save: !save,
            providers: overrides
                .iter()
                .map(|(symbol, key)| ((*symbol).into(), (*key).into()))
                .collect(),
        },
    })
    .await
}

#[tokio::test]
async fn virtual_install_validates_provides_and_overrides_before_mutation() {
    let mut lab = TortureLab::new().unwrap();
    lab.add_package(package("gawk", "0", 1, &["virtual/awk"], &[]))
        .unwrap();
    lab.add_package(package("impostor", "0", 1, &[], &[]))
        .unwrap();
    lab.publish().unwrap();
    let before = lab.snapshot().unwrap();
    let config = fs::read(lab.root().join("etc/sage/system.toml")).unwrap();
    for dry_run in [true, false] {
        for overrides in [
            vec![("awk", "impostor")],
            vec![("awk", "gawk"), ("virtual/awk", "gawk")],
            vec![("", "gawk")],
        ] {
            assert!(
                install(&lab, "virtual/awk", &overrides, true, dry_run)
                    .await
                    .is_err()
            );
            assert_eq!(lab.snapshot().unwrap(), before);
            assert_eq!(
                fs::read(lab.root().join("etc/sage/system.toml")).unwrap(),
                config
            );
        }
    }
}

#[tokio::test]
async fn virtual_install_preserves_slots_and_selected_provider_transitive_dependencies() {
    let mut lab = TortureLab::new().unwrap();
    lab.add_package(package("gawk", "2", 1, &["virtual/awk"], &["virtual/libc"]))
        .unwrap();
    lab.add_package(package("libc", "3", 1, &["virtual/libc"], &[]))
        .unwrap();
    lab.publish().unwrap();
    let before = lab.snapshot().unwrap();
    let config_path = lab.root().join("etc/sage/system.toml");
    let before_config = fs::read(&config_path).unwrap();
    install(
        &lab,
        "virtual/awk",
        &[("virtual/awk", "gawk:2")],
        true,
        true,
    )
    .await
    .unwrap();
    assert_eq!(lab.snapshot().unwrap(), before);
    assert_eq!(fs::read(&config_path).unwrap(), before_config);
    install(
        &lab,
        "virtual/awk",
        &[("virtual/awk", "gawk:2")],
        true,
        false,
    )
    .await
    .unwrap();
    let config = sage_sys::SystemConfig::load(&config_path).unwrap();
    assert_eq!(config.providers["awk"], "gawk:2");
    assert_eq!(config.providers["libc"], "libc:3");
    assert!(!config.providers.contains_key("virtual/awk"));
    let db = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
    assert!(
        db.package(&PackageKey::new("main/system", "gawk", "2"))
            .unwrap()
            .is_some()
    );
    assert!(
        db.package(&PackageKey::new("main/system", "libc", "3"))
            .unwrap()
            .is_some()
    );
    assert!(db.pending_journals().unwrap().is_empty());
    let installed = db.packages().unwrap();
    let mut universe = sage_solver::PackageUniverse::default();
    for installed in &installed {
        universe.insert(sage_core::Package::from_release(
            installed.key.clone(),
            installed.version.clone(),
            installed.dependencies.clone(),
            installed.provides.clone(),
        ));
    }
    let plan = sage_sys::ReconcilePlan::compute(&config, &installed, &universe, false).unwrap();
    assert!(plan.install.is_empty() && plan.remove.is_empty());
    assert_eq!(
        plan.provider_bindings["awk"],
        PackageKey::new("main/system", "gawk", "2")
    );
}

#[tokio::test]
async fn abandoned_versions_do_not_select_or_persist_unneeded_providers() {
    let mut lab = TortureLab::new().unwrap();
    lab.add_package(package("app", "0", 2, &[], &["virtual/missing"]))
        .unwrap();
    lab.add_package(package("app", "0", 1, &[], &[])).unwrap();
    lab.publish().unwrap();
    install(&lab, "app", &[], true, false).await.unwrap();
    let config = sage_sys::SystemConfig::load(lab.root().join("etc/sage/system.toml")).unwrap();
    assert!(config.providers.is_empty());
    let db = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
    assert_eq!(
        db.package(&PackageKey::new("main/system", "app", "0"))
            .unwrap()
            .unwrap()
            .version
            .upstream,
        "1"
    );
}

#[tokio::test]
async fn automatic_choice_respects_versions_conflicts_and_no_save() {
    let mut lab = TortureLab::new().unwrap();
    lab.add_package(package("app", "0", 1, &[], &["virtual/awk >= 2-1"]))
        .unwrap();
    lab.add_package(package("a-incompatible", "0", 1, &["virtual/awk"], &[]))
        .unwrap();
    let mut conflicting = package("z-conflicting", "0", 3, &["virtual/awk"], &[]);
    conflicting.conflicts.push("app".into());
    lab.add_package(conflicting).unwrap();
    lab.add_package(package("gawk", "2", 2, &["virtual/awk"], &[]))
        .unwrap();
    lab.publish().unwrap();
    let config_path = lab.root().join("etc/sage/system.toml");
    let before = fs::read(&config_path).unwrap();
    install(&lab, "app", &[], false, false).await.unwrap();
    assert_eq!(fs::read(&config_path).unwrap(), before);
    let db = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
    let keys: Vec<_> = db
        .packages()
        .unwrap()
        .into_iter()
        .map(|package| package.key.name)
        .collect();
    assert_eq!(keys, vec!["app", "gawk"]);
}

#[tokio::test]
async fn runtime_virtual_dependencies_use_system_channel_providers() {
    let mut lab = TortureLab::new().unwrap();
    let mut app = package("app", "0", 1, &[], &["virtual/awk"]);
    app.channel = "runtime".into();
    lab.add_package(app).unwrap();
    lab.add_package(package("gawk", "2", 1, &["virtual/awk"], &[]))
        .unwrap();
    lab.publish().unwrap();
    sage_sys::apply_packages(
        lab.root(),
        &["app".into()],
        Some("runtime"),
        &[("awk".into(), "gawk:2".into())],
        false,
        false,
        false,
        false,
    )
    .await
    .unwrap();
    let db = sage_db::SageDatabase::open(lab.root().join("var/lib/sage")).unwrap();
    assert!(
        db.package(&PackageKey::new("main/runtime", "app", "0"))
            .unwrap()
            .is_some()
    );
    assert!(
        db.package(&PackageKey::new("main/system", "gawk", "2"))
            .unwrap()
            .is_some()
    );
}

#[tokio::test]
async fn provider_selectors_accept_package_and_slot_punctuation() {
    let mut lab = TortureLab::new().unwrap();
    lab.add_package(package("libc++", "abi+debug", 1, &["virtual/libc++"], &[]))
        .unwrap();
    lab.add_package(package("unused", "0", 1, &[], &[]))
        .unwrap();
    lab.publish().unwrap();
    install(&lab, "unused", &[], false, false).await.unwrap();
    install(
        &lab,
        "virtual/libc++",
        &[("libc++", "libc++:abi+debug")],
        true,
        false,
    )
    .await
    .unwrap();
    let config = sage_sys::SystemConfig::load(lab.root().join("etc/sage/system.toml")).unwrap();
    let key = PackageKey::new("main/system", "libc++", "abi+debug");
    assert_eq!(
        config.provider_preferences("main/system").unwrap()["virtual/libc++"],
        key
    );
    let installed = sage_db::read_packages(&lab.root().join("var/lib/sage")).unwrap();
    let mut universe = sage_solver::PackageUniverse::default();
    for package in &installed {
        universe.insert(sage_core::Package::from_release(
            package.key.clone(),
            package.version.clone(),
            package.dependencies.clone(),
            package.provides.clone(),
        ));
    }
    let plan = sage_sys::ReconcilePlan::compute(&config, &installed, &universe, false).unwrap();
    assert!(plan.install.is_empty());
    assert_eq!(plan.provider_bindings["libc++"], key);
    let orphans = sage_sys::find_orphans(&installed, &config);
    assert_eq!(orphans.len(), 1);
    assert_eq!(orphans[0].key.name, "unused");
}

#[tokio::test]
async fn shared_library_cli_overrides_survive_install_and_rebuild() {
    for (flag, symbol) in [
        ("-P", "so:libfoo.so.1"),
        ("--provider", "so:libC++.so.1@ABI"),
    ] {
        let mut lab = TortureLab::new().unwrap();
        lab.add_package(package("app", "0", 1, &[], &[symbol]))
            .unwrap();
        // The concrete package named `so` still uses ordinary name:slot parsing.
        lab.add_package(package("so", "abi+debug", 1, &[symbol], &[]))
            .unwrap();
        lab.add_package(package("alternative", "0", 2, &[symbol], &[]))
            .unwrap();
        lab.publish().unwrap();
        let config_path = lab.root().join("etc/sage/system.toml");
        let before_config = fs::read(&config_path).unwrap();
        let before = lab.snapshot().unwrap();
        for dry_run in [true, false] {
            let mut cli = sage::Cli::try_parse_from([
                "sage",
                "--root",
                lab.root().to_str().unwrap(),
                "install",
                "app",
                flag,
                &format!("{symbol}=so:abi+debug"),
            ])
            .unwrap();
            cli.dry_run = dry_run;
            sage::execute(cli).await.unwrap();
            if dry_run {
                assert_eq!(lab.snapshot().unwrap(), before);
                assert_eq!(fs::read(&config_path).unwrap(), before_config);
            }
        }
        let config = sage_sys::SystemConfig::load(&config_path).unwrap();
        assert_eq!(config.providers[symbol], "so:abi+debug");
        assert_eq!(config.packages, ["app".into()].into());
        let snapshot = lab.snapshot().unwrap();
        assert_eq!(snapshot.packages.len(), 2);
        assert_eq!(snapshot.packages["main/system:so:abi+debug"], "1-1");
        assert_rebuild_keeps_installed(&lab);
        // Validate the persisted mapping on a later ordinary install as well.
        install(&lab, "app", &[], true, false).await.unwrap();
        assert_eq!(lab.snapshot().unwrap(), snapshot);
    }
}

#[tokio::test]
async fn virtual_upgrade_updates_only_the_selected_provider() {
    for save in [true, false] {
        let mut lab = TortureLab::new().unwrap();
        lab.add_package(package("gawk", "0", 1, &["virtual/awk"], &["virtual/libc"]))
            .unwrap();
        lab.add_package(package("libc", "0", 1, &["virtual/libc"], &[]))
            .unwrap();
        lab.publish().unwrap();
        install(&lab, "virtual/awk", &[], save, false)
            .await
            .unwrap();
        lab.add_package(package("gawk", "0", 2, &["virtual/awk"], &["virtual/libc"]))
            .unwrap();
        lab.add_package(package("libc", "0", 2, &["virtual/libc"], &[]))
            .unwrap();
        lab.publish().unwrap();
        let before = lab.snapshot().unwrap();
        let config_path = lab.root().join("etc/sage/system.toml");
        let config = fs::read(&config_path).unwrap();
        sage_sys::upgrade_packages(lab.root(), &["virtual/awk".into()], None, true)
            .await
            .unwrap();
        assert_eq!(lab.snapshot().unwrap(), before);
        assert_eq!(fs::read(&config_path).unwrap(), config);
        lab.upgrade("virtual/awk", "system").await.unwrap();
        let after = lab.snapshot().unwrap();
        assert_eq!(after.packages["main/system:gawk:0"], "2-1");
        assert_eq!(after.packages["main/system:libc:0"], "1-1");
        assert_eq!(fs::read(&config_path).unwrap(), config);
    }
}

#[tokio::test]
async fn virtual_upgrade_preserves_other_provider_slots_and_channels() {
    let mut lab = TortureLab::new().unwrap();
    for slot in ["0", "2"] {
        lab.add_package(package("gawk", slot, 1, &["virtual/awk"], &[]))
            .unwrap();
    }
    let mut runtime = package("gawk", "2", 1, &["virtual/awk"], &[]);
    runtime.channel = "runtime".into();
    lab.add_package(runtime.clone()).unwrap();
    lab.publish().unwrap();
    install(&lab, "gawk", &[], false, false).await.unwrap();
    lab.install("gawk:2", "runtime").await.unwrap();
    install(&lab, "virtual/awk:2", &[("awk", "gawk:2")], true, false)
        .await
        .unwrap();
    for slot in ["0", "2"] {
        lab.add_package(package("gawk", slot, 2, &["virtual/awk"], &[]))
            .unwrap();
    }
    runtime.version = 2;
    lab.add_package(runtime).unwrap();
    lab.publish().unwrap();
    lab.upgrade("virtual/awk:2", "runtime").await.unwrap();
    let after = lab.snapshot().unwrap();
    assert_eq!(after.packages["main/system:gawk:2"], "2-1");
    assert_eq!(after.packages["main/system:gawk:0"], "1-1");
    assert_eq!(after.packages["main/runtime:gawk:2"], "1-1");
}

#[tokio::test]
async fn virtual_upgrade_selects_dependencies_of_the_new_release() {
    let mut lab = TortureLab::new().unwrap();
    lab.add_package(package(
        "gawk",
        "0",
        1,
        &["virtual/awk"],
        &["virtual/codec = 1-1"],
    ))
    .unwrap();
    lab.add_package(package("codec-old", "0", 1, &["virtual/codec"], &[]))
        .unwrap();
    lab.publish().unwrap();
    install(&lab, "virtual/awk", &[], false, false)
        .await
        .unwrap();
    lab.add_package(package(
        "gawk",
        "0",
        2,
        &["virtual/awk"],
        &["virtual/codec >= 2-1"],
    ))
    .unwrap();
    lab.add_package(package("codec-new", "0", 2, &["virtual/codec"], &[]))
        .unwrap();
    lab.publish().unwrap();
    lab.upgrade("virtual/awk", "system").await.unwrap();
    let after = lab.snapshot().unwrap();
    assert_eq!(after.packages["main/system:gawk:0"], "2-1");
    assert_eq!(after.packages["main/system:codec-new:0"], "2-1");
}

fn assert_rebuild_keeps_installed(lab: &TortureLab) {
    let config = sage_sys::SystemConfig::load(lab.root().join("etc/sage/system.toml")).unwrap();
    let installed = sage_db::read_packages(&lab.root().join("var/lib/sage")).unwrap();
    let mut universe = sage_solver::PackageUniverse::default();
    for package in &installed {
        universe.insert(sage_core::Package::from_release(
            package.key.clone(),
            package.version.clone(),
            package.dependencies.clone(),
            package.provides.clone(),
        ));
    }
    let plan = sage_sys::ReconcilePlan::compute(&config, &installed, &universe, false).unwrap();
    assert!(plan.install.is_empty() && plan.remove.is_empty());
    assert!(sage_sys::find_orphans(&installed, &config).is_empty());
}

#[tokio::test]
async fn foreign_provider_choices_do_not_override_main_policy() {
    for explicit in [false, true] {
        let mut lab = TortureLab::new().unwrap();
        lab.add_package(package("main-app", "0", 1, &[], &["virtual/libc < 2-1"]))
            .unwrap();
        let mut app = package("vendor-app", "0", 1, &[], &["virtual/libc >= 2-1"]);
        app.channel = "runtime".into();
        lab.add_package(app).unwrap();
        lab.add_package(package("main-libc", "0", 1, &["virtual/libc"], &[]))
            .unwrap();
        lab.add_package(package("vendor-libc", "2", 2, &["virtual/libc"], &[]))
            .unwrap();
        lab.add_package(package("extra", "0", 1, &[], &[])).unwrap();
        lab.publish().unwrap();
        // Mirror the fixture indexes under another configured repository root.
        // Loading them through channels.toml exercises canonical identity routing.
        let channels_path = lab.root().join("etc/sage/channels.toml");
        let mut channels = fs::read_to_string(&channels_path).unwrap();
        channels.push_str(
            r#"
[channels.vendor]
url="https://invalid.example/vendor"
priority=100
signing_key="/etc/sage/repo.pub"
[channels.vendor.subchannels.system]
scope="system"
target_root="/opt/vendor/system"
[channels.vendor.subchannels.runtime]
scope="runtime"
target_root="/opt/vendor/runtime"
"#,
        );
        fs::write(channels_path, channels).unwrap();
        for channel in ["system", "runtime"] {
            let cache = lab.root().join("var/cache/sage/channels");
            let destination = cache.join("vendor").join(channel);
            fs::create_dir_all(&destination).unwrap();
            fs::copy(
                cache.join("main").join(channel).join("index.mdb"),
                destination.join("index.mdb"),
            )
            .unwrap();
        }
        sage_sys::apply_packages(
            lab.root(),
            &["main-app".into()],
            Some("main/system"),
            &[("libc".into(), "main-libc".into())],
            false,
            false,
            true,
            false,
        )
        .await
        .unwrap();
        let config_path = lab.root().join("etc/sage/system.toml");
        let config_bytes = fs::read(&config_path).unwrap();
        let before = lab.snapshot().unwrap();
        let overrides = if explicit {
            vec![("libc".into(), "vendor-libc:2".into())]
        } else {
            vec![]
        };
        for dry_run in [true, false] {
            sage_sys::apply_packages(
                lab.root(),
                &["vendor-app".into()],
                Some("vendor/runtime"),
                &overrides,
                true,
                false,
                true,
                dry_run,
            )
            .await
            .unwrap();
            assert_eq!(fs::read(&config_path).unwrap(), config_bytes);
            if dry_run {
                assert_eq!(lab.snapshot().unwrap(), before);
            }
        }
        let snapshot = lab.snapshot().unwrap();
        assert_eq!(snapshot.packages["main/system:main-libc:0"], "1-1");
        assert_eq!(snapshot.packages["vendor/system:vendor-libc:2"], "2-1");
        assert_eq!(snapshot.packages["vendor/runtime:vendor-app:0"], "1-1");
        assert_rebuild_keeps_installed(&lab);
        // A later saved main transaction must retain both dependency graphs and
        // must not persist a foreign choice under main's unqualified symbol.
        sage_sys::apply_packages(
            lab.root(),
            &["extra".into()],
            Some("main/system"),
            &[],
            false,
            false,
            true,
            false,
        )
        .await
        .unwrap();
        let config = sage_sys::SystemConfig::load(&config_path).unwrap();
        assert_eq!(config.providers.len(), 1);
        assert_eq!(config.providers["libc"], "main-libc:0");
        assert_rebuild_keeps_installed(&lab);
    }
}

#[tokio::test]
async fn automatic_provider_choices_keep_simultaneous_slots_independent() {
    for interactive in [false, true] {
        let mut lab = TortureLab::new().unwrap();
        for slot in ["1", "2"] {
            lab.add_package(package("codec", slot, 1, &["virtual/codec"], &[]))
                .unwrap();
        }
        lab.add_package(package(
            "app",
            "0",
            1,
            &[],
            &["virtual/codec:1", "virtual/codec:2"],
        ))
        .unwrap();
        lab.publish().unwrap();
        let before = lab.snapshot().unwrap();
        let path = lab.root().join("etc/sage/system.toml");
        let config = fs::read(&path).unwrap();
        for dry_run in [true, false] {
            sage_sys::apply_packages(
                lab.root(),
                &["app".into()],
                None,
                &[],
                interactive,
                false,
                true,
                dry_run,
            )
            .await
            .unwrap();
            if dry_run {
                assert_eq!(lab.snapshot().unwrap(), before);
                assert_eq!(fs::read(&path).unwrap(), config);
            }
        }
        let after = lab.snapshot().unwrap();
        assert_eq!(after.packages["main/system:codec:1"], "1-1");
        assert_eq!(after.packages["main/system:codec:2"], "1-1");
        assert!(
            sage_sys::SystemConfig::load(&path)
                .unwrap()
                .providers
                .is_empty()
        );
        assert_rebuild_keeps_installed(&lab);
    }
}

#[tokio::test]
async fn saved_slot_specific_choices_do_not_constrain_later_installs() {
    let mut lab = TortureLab::new().unwrap();
    for slot in ["1", "2"] {
        lab.add_package(package("codec", slot, 1, &["virtual/codec"], &[]))
            .unwrap();
        lab.add_package(package(
            &format!("app{slot}"),
            "0",
            1,
            &[],
            &[&format!("virtual/codec:{slot}")],
        ))
        .unwrap();
    }
    lab.publish().unwrap();
    for app in ["app1", "app2"] {
        install(&lab, app, &[], true, false).await.unwrap();
        let config = sage_sys::SystemConfig::load(lab.root().join("etc/sage/system.toml")).unwrap();
        assert!(config.providers.is_empty());
    }
    assert_rebuild_keeps_installed(&lab);
}

#[tokio::test]
async fn direct_scoped_virtual_requests_persist_independent_package_roots() {
    let mut lab = TortureLab::new().unwrap();
    for slot in ["1", "2"] {
        lab.add_package(package("codec", slot, 1, &["virtual/codec"], &[]))
            .unwrap();
    }
    lab.publish().unwrap();
    sage_sys::apply_packages(
        lab.root(),
        &["virtual/codec:1".into(), "virtual/codec:2".into()],
        None,
        &[],
        false,
        false,
        true,
        false,
    )
    .await
    .unwrap();
    let config = sage_sys::SystemConfig::load(lab.root().join("etc/sage/system.toml")).unwrap();
    assert!(config.providers.is_empty());
    assert_eq!(config.packages, ["codec:1".into(), "codec:2".into()].into());
    assert_rebuild_keeps_installed(&lab);
}
