mod sage_tests {
    use sage::*;

    #[test]
    fn declarative_metadata_is_validated_and_staged() {
        let directory = tempfile::tempdir().unwrap();
        let metadata = directory.path().join("metadata");
        std::fs::create_dir(&metadata).unwrap();
        std::fs::write(
            directory.path().join("service.toml"),
            r#"schema_version=1
[service]
name="daemon"
description="Daemon"
command=["/usr/bin/daemon","--foreground"]
user="daemon"
group="daemon"
working_dir="/"
restart="on-failure"
type="simple"
"#,
        )
        .unwrap();
        std::fs::write(
            directory.path().join("triggers.toml"),
            r#"schema_version=1
name="daemon-cache"
description="Refresh daemon cache"
on_paths=["usr/share/daemon/**"]
exec=["/usr/bin/daemon","--refresh-cache"]
priority=50
ignore_missing_binary=false
"#,
        )
        .unwrap();

        stage_declarative_metadata(directory.path(), &metadata, "daemon", "daemon").unwrap();
        sage_sys::ServiceSpec::load(metadata.join("service.toml")).unwrap();
        sage_sys::TriggerSpec::load(metadata.join("triggers.toml")).unwrap();
    }

    #[tokio::test]
    async fn mass_rebuild_dry_run_discovers_dependency_tree_without_output() {
        let root = tempfile::tempdir().unwrap();
        let recipes = tempfile::tempdir().unwrap();
        for (name, dependencies) in [("lib", ""), ("app", "dependencies=[\"lib\"]")] {
            let directory = recipes.path().join(name);
            std::fs::create_dir(&directory).unwrap();
            std::fs::write(
                directory.join("recipe.toml"),
                format!(
                    r#"schema_version=1
[package]
name="{name}"
version="1"
release=1
description="{name}"
license="MIT"
channel="system"
arch="any"
{dependencies}
[source]
url="https://example.invalid/{name}.tar"
sha256="{}"
"#,
                    "00".repeat(32)
                ),
            )
            .unwrap();
        }
        mass_rebuild(root.path(), recipes.path(), None, 0, true)
            .await
            .unwrap();
        assert!(!recipes.path().join(".sage-packages").exists());
        let plan = recipes.path().join("bootstrap.toml");
        std::fs::write(
            &plan,
            r#"schema_version=1
[[stages]]
name="seed"
recipes=["lib/recipe.toml"]
[[stages]]
name="world"
recipes=["app/recipe.toml"]
"#,
        )
        .unwrap();
        bootstrap_sources(root.path(), &plan, None, 0, true)
            .await
            .unwrap();
        assert!(!recipes.path().join(".sage-bootstrap").exists());
    }

    #[test]
    fn source_pool_overlays_local_artifacts_into_solver_universe() {
        let root = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(root.path().join("etc/sage")).unwrap();
        std::fs::write(
            root.path().join("etc/sage/channels.toml"),
            "schema_version=1\n[channels]\n",
        )
        .unwrap();
        let pool = root.path().join("pool");
        let stage = root.path().join("stage");
        std::fs::create_dir_all(stage.join(".METADATA")).unwrap();
        std::fs::create_dir_all(stage.join("data/usr/lib")).unwrap();
        std::fs::write(stage.join("data/usr/lib/libseed.so"), b"seed").unwrap();
        let records = sage_archive::build_file_index(&stage.join("data")).unwrap();
        std::fs::write(
            stage.join(".METADATA/files.idx"),
            sage_archive::format_file_index(&records),
        )
        .unwrap();
        let manifest = sage_archive::PackageManifest {
            schema_version: 1,
            name: "seed".into(),
            slot: "0".into(),
            version: "1".into(),
            release: 1,
            epoch: 0,
            arch: "any".into(),
            channel: "system".into(),
            description: "seed".into(),
            license: "MIT".into(),
            installed_size: 4,
            build_time: 1,
            dependencies: Vec::new(),
            provides: Vec::new(),
            conflicts: Vec::new(),
            features: Vec::new(),
            managed_build_tools: Vec::new(),
        };
        std::fs::write(
            stage.join(".METADATA/manifest.toml"),
            toml::to_string(&manifest).unwrap(),
        )
        .unwrap();
        std::fs::create_dir(&pool).unwrap();
        let package = pool.join("seed-1-1-any.pkg.tar.zst");
        sage_archive::create_package(&stage, &package, 1).unwrap();

        let available = load_available_with_pool(root.path(), None, Some(&pool)).unwrap();
        let key = sage_core::PackageKey::new("main/system", "seed", "0");
        let version = sage_core::Version::new(0, "1", 1);
        assert!(matches!(
            available.releases[&(key, version)].location,
            ReleaseLocation::Local(_)
        ));
    }
}
