mod sage_tests {
    use sage::*;
    use sha2::{Digest, Sha256};
    use std::path::{Path, PathBuf};

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

    fn cli(root: &Path, command: Commands) -> Cli {
        Cli {
            verbose: false,
            dry_run: false,
            root: root.into(),
            command,
        }
    }

    fn configure_root(root: &Path) {
        std::fs::create_dir_all(root.join("etc/sage")).unwrap();
        std::fs::create_dir_all(root.join("run/sage")).unwrap();
        std::fs::write(
            root.join("etc/sage/build.toml"),
            format!(
                r#"schema_version=1
fakeroot="/bin/false"
bwrap="/bin/false"
sysroot="{}"
cc="cc"
cxx="c++"
linker="ld"
rustc="rustc"
source_date_epoch=1
"#,
                root.display()
            ),
        )
        .unwrap();
        std::fs::write(
            root.join("etc/sage/system.toml"),
            "schema_version=1\n[system]\narchitecture=\"amd64\"\nprofile=\"default\"\n",
        )
        .unwrap();
        std::fs::write(
            root.join("etc/sage/channels.toml"),
            r#"schema_version=1
[channels.main]
url="https://invalid.example/repo"
priority=100
signing_key="/etc/sage/repo.pub"
[channels.main.subchannels.system]
scope="system"
target_root="/"
"#,
        )
        .unwrap();
    }

    struct TestPackage<'a> {
        name: &'a str,
        slot: &'a str,
        version: &'a str,
        arch: &'a str,
        dependency: Option<&'a str>,
        conflict: Option<&'a str>,
        files: &'a [(&'a str, &'a str)],
    }

    async fn build_data_package(
        root: &Path,
        recipes: &Path,
        pool: &Path,
        package: TestPackage<'_>,
    ) -> PathBuf {
        let TestPackage {
            name,
            slot,
            version,
            arch,
            dependency,
            conflict,
            files,
        } = package;
        let directory = recipes.join(format!("{name}-{slot}-{version}-{arch}"));
        std::fs::create_dir_all(&directory).unwrap();
        let dependencies = dependency.map_or(String::new(), |value| {
            format!("dependencies=[\"{value}\"]\n")
        });
        let conflicts =
            conflict.map_or(String::new(), |value| format!("conflicts=[\"{value}\"]\n"));
        let declarations = files
            .iter()
            .map(|(path, content)| {
                format!("[[install.files]]\npath=\"{path}\"\ncontent=\"{content}\"\n")
            })
            .collect::<String>();
        std::fs::write(
            directory.join("recipe.toml"),
            format!(
                "schema_version=1\n[package]\nname=\"{name}\"\nslot=\"{slot}\"\nversion=\"{version}\"\nrelease=1\narch=\"{arch}\"\nchannel=\"system\"\ndescription=\"{name}\"\nlicense=\"MIT\"\n{dependencies}{conflicts}{declarations}"
            ),
        )
        .unwrap();
        execute(cli(
            root,
            Commands::Build {
                recipe_dir: directory.clone(),
                features: vec![],
                no_default_features: false,
            },
        ))
        .await
        .unwrap();
        let archive = std::fs::read_dir(&directory)
            .unwrap()
            .map(Result::unwrap)
            .map(|entry| entry.path())
            .find(|path| path.to_string_lossy().ends_with(".pkg.tar.zst"))
            .unwrap();
        let destination = pool.join(archive.file_name().unwrap());
        std::fs::copy(archive, &destination).unwrap();
        destination
    }

    async fn publish_repository(root: &Path, pool: &Path, signing_key: &Path) {
        execute(cli(
            root,
            Commands::Repo {
                action: RepoAction::Index {
                    dir: pool.into(),
                    sign_key: Some(signing_key.into()),
                },
            },
        ))
        .await
        .unwrap();
        let index = root.join("var/cache/sage/channels/main/system/index.mdb");
        std::fs::create_dir_all(index.parent().unwrap()).unwrap();
        std::fs::copy(pool.join("index.mdb"), index).unwrap();
        let cache = root.join("var/cache/sage/packages");
        std::fs::create_dir_all(&cache).unwrap();
        for entry in std::fs::read_dir(pool).unwrap().map(Result::unwrap) {
            let path = entry.path();
            if path.to_string_lossy().ends_with(".pkg.tar.zst") {
                let digest = hex::encode(Sha256::digest(std::fs::read(&path).unwrap()));
                std::fs::copy(path, cache.join(digest)).unwrap();
            }
        }
    }

    fn inject(root: &Path, stage: &str) {
        std::fs::write(root.join("run/sage/crash-point"), stage).unwrap();
    }

    #[tokio::test]
    async fn signed_repository_lifecycle_recovers_and_preserves_configuration() {
        let root = tempfile::tempdir().unwrap();
        let recipes = tempfile::tempdir().unwrap();
        let pool = tempfile::tempdir().unwrap();
        configure_root(root.path());
        let key = root.path().join("signing.key");
        std::fs::write(&key, [7_u8; 32]).unwrap();
        build_data_package(
            root.path(),
            recipes.path(),
            pool.path(),
            TestPackage {
                name: "dep",
                slot: "0",
                version: "1",
                arch: "noarch",
                dependency: None,
                conflict: None,
                files: &[("usr/lib/libdep", "dep")],
            },
        )
        .await;
        build_data_package(
            root.path(),
            recipes.path(),
            pool.path(),
            TestPackage {
                name: "app",
                slot: "1",
                version: "1",
                arch: "aarch64",
                dependency: Some("dep"),
                conflict: None,
                files: &[("usr/bin/app-1", "aarch64")],
            },
        )
        .await;
        let app1 = build_data_package(
            root.path(),
            recipes.path(),
            pool.path(),
            TestPackage {
                name: "app",
                slot: "1",
                version: "1",
                arch: "amd64",
                dependency: Some("dep"),
                conflict: None,
                files: &[
                    ("usr/bin/app-1", "v1"),
                    ("usr/share/app-obsolete", "obsolete"),
                    ("etc/app.conf", "v1"),
                ],
            },
        )
        .await;
        let app2_slot = build_data_package(
            root.path(),
            recipes.path(),
            pool.path(),
            TestPackage {
                name: "app",
                slot: "2",
                version: "1",
                arch: "amd64",
                dependency: None,
                conflict: Some("foreign"),
                files: &[("usr/bin/app-2", "slot2")],
            },
        )
        .await;
        assert_ne!(app1.file_name(), app2_slot.file_name());
        assert_eq!(
            sage_archive::inspect_package(&app2_slot)
                .unwrap()
                .manifest
                .conflicts,
            ["foreign"]
        );
        build_data_package(
            root.path(),
            recipes.path(),
            pool.path(),
            TestPackage {
                name: "foreign",
                slot: "0",
                version: "1",
                arch: "aarch64",
                dependency: None,
                conflict: None,
                files: &[("usr/bin/foreign", "wrong architecture")],
            },
        )
        .await;
        let aarch64 = load_available_with_pool(root.path(), Some("aarch64"), Some(pool.path()))
            .unwrap();
        assert!(aarch64
            .releases
            .keys()
            .any(|(key, _)| key.name == "foreign"));
        assert!(aarch64.releases.keys().any(|(key, _)| key.name == "dep"));
        assert!(aarch64.releases.iter().any(|((key, _), source)| {
            key.name == "app" && source.release.package.arch == "aarch64"
        }));
        publish_repository(root.path(), pool.path(), &key).await;

        inject(root.path(), "lmdb-publication");
        assert!(execute(cli(
            root.path(),
            Commands::Install {
                packages: vec!["app:1".into()],
                channel: None,
                no_save: true,
            },
        ))
        .await
        .is_err());
        let installed = sage_db::read_packages(&root.path().join("var/lib/sage")).unwrap();
        assert_eq!(installed.len(), 1);
        assert_eq!(installed[0].key.name, "dep");
        execute(cli(
            root.path(),
            Commands::Install {
                packages: vec!["app:1".into(), "app:2".into()],
                channel: None,
                no_save: true,
            },
        ))
        .await
        .unwrap();
        assert!(!std::fs::read_to_string(root.path().join("etc/sage/system.toml"))
            .unwrap()
            .contains("app:1"));
        execute(cli(
            root.path(),
            Commands::Install {
                packages: vec!["app:1".into(), "app:2".into()],
                channel: None,
                no_save: false,
            },
        ))
        .await
        .unwrap();
        execute(cli(
            root.path(),
            Commands::Query {
                action: QueryAction::Info {
                    package: "app:2".into(),
                    channel: "system".into(),
                },
            },
        ))
        .await
        .unwrap();
        assert!(execute(cli(
            root.path(),
            Commands::Install {
                packages: vec!["foreign".into()],
                channel: None,
                no_save: true,
            },
        ))
        .await
        .is_err());
        assert!(!root.path().join("usr/bin/foreign").exists());

        std::fs::write(root.path().join("etc/app.conf"), "administrator").unwrap();
        build_data_package(
            root.path(),
            recipes.path(),
            pool.path(),
            TestPackage {
                name: "app",
                slot: "1",
                version: "2",
                arch: "amd64",
                dependency: Some("dep"),
                conflict: None,
                files: &[("usr/bin/app-1", "v2"), ("etc/app.conf", "v2")],
            },
        )
        .await;
        publish_repository(root.path(), pool.path(), &key).await;
        inject(root.path(), "extraction");
        assert!(execute(cli(
            root.path(),
            Commands::Upgrade {
                packages: vec!["app:1".into()],
                channel: None,
                sync: false,
            },
        ))
        .await
        .is_err());
        inject(root.path(), "lmdb-publication");
        assert!(execute(cli(
            root.path(),
            Commands::Install {
                packages: vec!["app:2".into()],
                channel: None,
                no_save: true,
            },
        ))
        .await
        .is_err());
        inject(root.path(), "alternatives");
        assert!(execute(cli(
            root.path(),
            Commands::Install {
                packages: vec!["app:2".into()],
                channel: None,
                no_save: true,
            },
        ))
            .await
            .is_err());
        inject(root.path(), "triggers");
        assert!(execute(cli(
            root.path(),
            Commands::Install {
                packages: vec!["app:2".into()],
                channel: None,
                no_save: true,
            },
        ))
            .await
            .is_err());
        execute(cli(
            root.path(),
            Commands::Install {
                packages: vec!["app:2".into()],
                channel: None,
                no_save: true,
            },
        ))
            .await
            .unwrap();
        assert_eq!(
            std::fs::read_to_string(root.path().join("etc/app.conf")).unwrap(),
            "administrator"
        );
        assert_eq!(
            std::fs::read_to_string(root.path().join("etc/app.conf.sage-new")).unwrap(),
            "v2"
        );
        assert!(!root.path().join("usr/share/app-obsolete").exists());

        inject(root.path(), "removal");
        assert!(execute(cli(
            root.path(),
            Commands::Remove {
                packages: vec!["app:1".into()],
                channel: None,
            },
        ))
        .await
        .is_err());
        execute(cli(
            root.path(),
            Commands::Install {
                packages: vec!["app:2".into()],
                channel: None,
                no_save: true,
            },
        ))
            .await
            .unwrap();
        assert!(!root.path().join("usr/bin/app-1").exists());
        assert!(root.path().join("usr/bin/app-2").exists());
        assert!(sage_db::read_owners(
            &root.path().join("var/lib/sage"),
            "usr/bin/app-1"
        )
        .unwrap()
        .is_empty());
    }

    #[tokio::test]
    async fn installed_conflicts_cycles_and_canonical_removal_are_safe() {
        let root = tempfile::tempdir().unwrap();
        let recipes = tempfile::tempdir().unwrap();
        let pool = tempfile::tempdir().unwrap();
        configure_root(root.path());
        let key = root.path().join("signing.key");
        std::fs::write(&key, [9_u8; 32]).unwrap();
        for package in [
            TestPackage {
                name: "resident",
                slot: "0",
                version: "1",
                arch: "amd64",
                dependency: None,
                conflict: Some("newcomer"),
                files: &[("usr/lib/resident", "resident")],
            },
            TestPackage {
                name: "newcomer",
                slot: "0",
                version: "1",
                arch: "amd64",
                dependency: None,
                conflict: None,
                files: &[("usr/lib/newcomer", "newcomer")],
            },
            TestPackage {
                name: "cycle-a",
                slot: "0",
                version: "1",
                arch: "amd64",
                dependency: Some("cycle-b"),
                conflict: None,
                files: &[("usr/lib/cycle-a", "a")],
            },
            TestPackage {
                name: "cycle-b",
                slot: "0",
                version: "1",
                arch: "amd64",
                dependency: Some("cycle-a"),
                conflict: None,
                files: &[("usr/lib/cycle-b", "b")],
            },
        ] {
            build_data_package(root.path(), recipes.path(), pool.path(), package).await;
        }
        publish_repository(root.path(), pool.path(), &key).await;

        execute(cli(
            root.path(),
            Commands::Install {
                packages: vec!["resident".into()],
                channel: None,
                no_save: false,
            },
        ))
        .await
        .unwrap();
        assert!(execute(cli(
            root.path(),
            Commands::Install {
                packages: vec!["newcomer".into()],
                channel: None,
                no_save: true,
            },
        ))
        .await
        .is_err());
        execute(cli(
            root.path(),
            Commands::Remove {
                packages: vec!["resident:0".into()],
                channel: None,
            },
        ))
        .await
        .unwrap();
        assert!(!std::fs::read_to_string(root.path().join("etc/sage/system.toml"))
            .unwrap()
            .contains("resident"));

        execute(cli(
            root.path(),
            Commands::Install {
                packages: vec!["cycle-a".into()],
                channel: None,
                no_save: true,
            },
        ))
        .await
        .unwrap();
        assert!(root.path().join("usr/lib/cycle-a").exists());
        assert!(root.path().join("usr/lib/cycle-b").exists());
    }
}
