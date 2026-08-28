mod build_tests {
    use sage_build::*;
    use std::collections::BTreeMap;
    use std::fs;
    use std::path::{Path, PathBuf};
    use std::process::Command;

    #[test]
    fn runner_orders_phases_and_rejects_unknown_variables() {
        let class = Rclass {
            schema_version: 1,
            name: "demo".into(),
            description: "demo".into(),
            implicit_build_dependencies: vec![],
            allowed_compilers: vec![],
            allowed_linkers: vec![],
            defaults: BTreeMap::new(),
            env: BTreeMap::from([("PARALLEL".into(), "${JOBS}".into())]),
            phases: BTreeMap::from([
                ("src_compile".into(), "make -j ${JOBS}".into()),
                ("src_configure".into(), "configure ${args.flags}".into()),
            ]),
        };
        let vars = BTreeMap::from([
            ("JOBS".into(), "8".into()),
            ("args.flags".into(), "--safe".into()),
        ]);
        let script = compose_runner(&[class], &vars).unwrap();
        assert!(script.find("configure").unwrap() < script.find("make -j").unwrap());
        assert!(script.contains("export PARALLEL='8'"));
        assert!(expand("${missing}", &vars).is_err());
    }

    #[test]
    fn mainstream_rclasses_expand_for_native_builds() {
        let variables = BTreeMap::from([
            ("JOBS".into(), "4".into()),
            ("CFLAGS".into(), "-O2".into()),
            ("CXXFLAGS".into(), "-O2".into()),
            ("LDFLAGS".into(), "".into()),
            ("RUSTFLAGS".into(), "".into()),
            ("SRC_DIR".into(), "/source".into()),
            ("BUILD_DIR".into(), "/build".into()),
            ("DESTDIR".into(), "/dest".into()),
            ("TARGET_TRIPLE".into(), "".into()),
            ("TARGET_ARCH".into(), "".into()),
            ("TARGET_ENDIAN".into(), "".into()),
            ("GOOS".into(), "".into()),
        ]);
        for name in [
            "autotools",
            "meson",
            "python",
            "go",
            "cmake",
            "cargo",
            "npm",
            "pnpm",
            "gradle",
            "maven",
        ] {
            let class = Rclass::load(
                Path::new(env!("CARGO_MANIFEST_DIR")).join(format!("../../rclass/{name}.toml")),
            )
            .unwrap();
            compose_runner(&[class], &variables).unwrap();
        }
    }

    #[test]
    fn features_fold_defaults_and_requested_rules_deterministically() {
        let directory = tempfile::tempdir().unwrap();
        fs::write(directory.path().join("source.tar"), b"unused").unwrap();
        fs::write(
            directory.path().join("recipe.toml"),
            format!(
                r#"schema_version=1
[package]
name="demo"
version="1.0"
release=1
description="demo"
license="MIT"
channel="system"
arch="amd64"
[source]
url="file://{}"
sha256="{}"
[features.tls]
default=true
dependencies=["openssl"]
build_dependencies=["pkgconf"]
[features.gui]
dependencies=["gtk4"]
target_dependencies=["gtk4-dev"]
[features.gui.args]
frontend="gtk"
"#,
                directory.path().join("source.tar").display(),
                "00".repeat(32)
            ),
        )
        .unwrap();
        let recipe = RecipeSpec::load(directory.path().join("recipe.toml")).unwrap();
        let selected = recipe.effective_features(&["gui".into()], true).unwrap();
        assert_eq!(
            selected.enabled.into_iter().collect::<Vec<_>>(),
            ["gui", "tls"]
        );
        assert_eq!(selected.dependencies, ["gtk4", "openssl"]);
        assert_eq!(selected.build_dependencies, ["pkgconf"]);
        assert_eq!(selected.target_dependencies, ["gtk4-dev"]);
        assert_eq!(selected.args["frontend"], "gtk");
        assert!(recipe
            .effective_features(&["missing".into()], false)
            .is_err());
    }

    #[test]
    fn recipe_accepts_ordered_multiple_sources() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("recipe.toml");
        fs::write(
            &path,
            format!(
                r#"schema_version=1
[package]
name="demo"
version="1"
release=1
description="demo"
license="MIT"
channel="system"
arch="any"

[[sources]]
url="https://example.invalid/main.tar"
sha256="{}"

[[sources]]
url="https://example.invalid/languages.tar"
sha256="{}"
"#,
                "00".repeat(32),
                "11".repeat(32)
            ),
        )
        .unwrap();
        let recipe = RecipeSpec::load(path).unwrap();
        assert_eq!(recipe.package.slot, sage_core::DEFAULT_SLOT);
        assert_eq!(recipe.source_inputs().count(), 2);
        assert_eq!(
            recipe.source_inputs().nth(1).unwrap().url,
            "https://example.invalid/languages.tar"
        );
    }

    #[test]
    fn git_sources_require_pins_and_join_the_ordered_manifest() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("recipe.toml");
        fs::write(
            &path,
            r#"schema_version=1
[package]
name="demo"
version="1"
release=1
description="demo"
license="MIT"
channel="system"
arch="any"
[source]
kind="git"
url="https://example.invalid/project.git"
commit="0123456789abcdef0123456789abcdef01234567"
submodules=true
destination="vendor/project"
"#,
        )
        .unwrap();
        let recipe = RecipeSpec::load(&path).unwrap();
        let source = recipe.source_inputs().next().unwrap();
        assert_eq!(source.kind, SourceKind::Git);
        assert!(source.submodules);
        assert_eq!(recipe.source_manifest(), "000-source\t0\tvendor/project\n");

        let invalid = fs::read_to_string(&path)
            .unwrap()
            .replace("0123456789abcdef0123456789abcdef01234567", "main");
        fs::write(&path, invalid).unwrap();
        assert!(RecipeSpec::load(path).is_err());
    }

    #[test]
    fn git_export_omits_repository_metadata() {
        let directory = tempfile::tempdir().unwrap();
        let checkout = directory.path().join("checkout");
        let output = directory.path().join("output");
        fs::create_dir_all(checkout.join(".git/objects")).unwrap();
        fs::create_dir_all(checkout.join("submodule")).unwrap();
        fs::write(checkout.join(".git/config"), b"metadata").unwrap();
        fs::write(checkout.join("submodule/.git"), b"gitdir: elsewhere").unwrap();
        fs::write(checkout.join("submodule/source.c"), b"source").unwrap();
        export_git_tree(&checkout, &output).unwrap();
        assert!(!output.join(".git").exists());
        assert!(!output.join("submodule/.git").exists());
        assert_eq!(
            fs::read(output.join("submodule/source.c")).unwrap(),
            b"source"
        );
    }

    fn test_git(directory: &Path, arguments: &[&str]) {
        let status = Command::new("git")
            .args(["-c", "commit.gpgsign=false"])
            .arg("-C")
            .arg(directory)
            .args(arguments)
            .env("GIT_AUTHOR_NAME", "Sage Test")
            .env("GIT_AUTHOR_EMAIL", "sage@example.invalid")
            .env("GIT_COMMITTER_NAME", "Sage Test")
            .env("GIT_COMMITTER_EMAIL", "sage@example.invalid")
            .status()
            .unwrap();
        assert!(status.success(), "git command failed: {arguments:?}");
    }

    #[test]
    fn git_fetch_materializes_recursive_network_submodules() {
        let directory = tempfile::tempdir().unwrap();
        let repositories = directory.path().join("repositories");
        fs::create_dir(&repositories).unwrap();
        for name in ["child", "project"] {
            let work = directory.path().join(name);
            fs::create_dir(&work).unwrap();
            test_git(&work, &["init", "--quiet"]);
            fs::write(work.join(format!("{name}.txt")), name).unwrap();
            test_git(&work, &["add", "."]);
            test_git(&work, &["commit", "--quiet", "-m", name]);
            let bare = repositories.join(format!("{name}.git"));
            fs::create_dir(&bare).unwrap();
            test_git(&bare, &["init", "--quiet", "--bare"]);
            test_git(&work, &["remote", "add", "origin", bare.to_str().unwrap()]);
            test_git(&work, &["push", "--quiet", "origin", "HEAD:master"]);
            test_git(&bare, &["symbolic-ref", "HEAD", "refs/heads/master"]);
        }
        let project = directory.path().join("project");
        test_git(
            &project,
            &[
                "-c",
                "protocol.file.allow=always",
                "submodule",
                "add",
                "--quiet",
                repositories.join("child.git").to_str().unwrap(),
                "child",
            ],
        );
        test_git(
            &project,
            &[
                "config",
                "-f",
                ".gitmodules",
                "submodule.child.url",
                "../child.git",
            ],
        );
        test_git(&project, &["add", ".gitmodules", "child"]);
        test_git(&project, &["commit", "--quiet", "-m", "add child"]);
        test_git(&project, &["push", "--quiet", "origin", "HEAD:master"]);
        let commit = String::from_utf8(
            Command::new("git")
                .args(["-C", project.to_str().unwrap(), "rev-parse", "HEAD"])
                .output()
                .unwrap()
                .stdout,
        )
        .unwrap();

        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        drop(listener);
        let mut daemon = Command::new("git")
            .args([
                "daemon",
                "--reuseaddr",
                "--export-all",
                "--listen=127.0.0.1",
                &format!("--port={port}"),
                &format!("--base-path={}", repositories.display()),
                repositories.to_str().unwrap(),
            ])
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .spawn()
            .unwrap();
        for _ in 0..100 {
            if std::net::TcpStream::connect(("127.0.0.1", port)).is_ok() {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(5));
        }
        let source = SourceSpec {
            kind: SourceKind::Git,
            url: format!("git://127.0.0.1:{port}/project.git"),
            sha256: String::new(),
            commit: commit.trim().into(),
            submodules: true,
            strip_components: None,
            destination: PathBuf::from("."),
        };
        let result = fetch_git_source(
            Path::new("git"),
            &source,
            &directory.path().join("checkout"),
            &directory.path().join("export"),
        );
        daemon.kill().unwrap();
        daemon.wait().unwrap();
        result.unwrap();
        assert!(directory.path().join("export/project.txt").exists());
        assert!(directory.path().join("export/child/child.txt").exists());
        assert!(!directory.path().join("export/.git").exists());
        assert!(!directory.path().join("export/child/.git").exists());
    }

    fn unit(name: &str, produces: &[&str], consumes: &[&str]) -> BuildUnit {
        assert_eq!(produces, [name]);
        let recipe = RecipeSpec {
            schema_version: 1,
            package: RecipePackage {
                name: name.into(),
                slot: sage_core::DEFAULT_SLOT.into(),
                version: "1".into(),
                release: 1,
                epoch: 0,
                description: name.into(),
                license: "MIT".into(),
                channel: "system".into(),
                arch: "any".into(),
                dependencies: consumes.iter().map(|value| (*value).into()).collect(),
                provides: vec![],
            },
            source: None,
            sources: vec![],
            build: RecipeBuild::default(),
            subpackages: vec![],
            sysusers: vec![],
            features: BTreeMap::new(),
        };
        BuildUnit::from_recipe(PathBuf::from(format!("{name}/recipe.toml")), &recipe).unwrap()
    }

    #[test]
    fn source_graph_returns_parallel_dependency_layers() {
        let layers = BuildGraph::layers(vec![
            unit("compiler", &["compiler"], &[]),
            unit("runtime", &["runtime"], &["compiler"]),
            unit("docs", &["docs"], &[]),
        ])
        .unwrap();
        assert_eq!(
            layers[0]
                .iter()
                .map(|unit| unit.name.as_str())
                .collect::<Vec<_>>(),
            ["compiler", "docs"]
        );
        assert_eq!(layers[1][0].name, "runtime");
    }

    #[test]
    fn source_graph_reports_bootstrap_cycles() {
        let error = BuildGraph::layers(vec![
            unit("compiler", &["compiler"], &["libc"]),
            unit("libc", &["libc"], &["compiler"]),
        ])
        .unwrap_err();
        assert!(error.to_string().contains("compiler, libc"));
    }

    #[test]
    fn bootstrap_plan_preserves_explicit_stage_order() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("bootstrap.toml");
        fs::write(
            &path,
            r#"schema_version=1
[[stages]]
name="seed"
recipes=["compiler/recipe.toml"]
[[stages]]
name="self-host"
recipes=["libc/recipe.toml", "compiler/recipe.toml"]
"#,
        )
        .unwrap();
        let plan = BootstrapPlan::load(path).unwrap();
        assert_eq!(plan.stages[0].name, "seed");
        assert_eq!(plan.stages[1].recipes.len(), 2);
    }

    #[test]
    fn five_tarballs_follow_the_declarative_extraction_plan() {
        let directory = tempfile::tempdir().unwrap();
        let recipe_path = directory.path().join("recipe.toml");
        let mut document = r#"schema_version=1
[package]
name="aggregate"
version="1"
release=1
description="aggregate"
license="MIT"
channel="system"
arch="any"
"#
        .to_owned();
        for index in 0..5 {
            let destination = if index == 4 { "vendor/component" } else { "." };
            document.push_str(&format!(
                r#"
[[sources]]
url="https://example.invalid/source-{index}.tar"
sha256="{}"
strip_components=1
destination="{destination}"
"#,
                format!("{index:x}").repeat(64)
            ));
        }
        fs::write(&recipe_path, document).unwrap();
        let recipe = RecipeSpec::load(recipe_path).unwrap();
        assert_eq!(recipe.source_inputs().count(), 5);

        let source = directory.path().join("source");
        let distfiles = source.join(".distfiles");
        fs::create_dir_all(&distfiles).unwrap();
        for index in 0..5 {
            let input = directory.path().join(format!("input-{index}/top"));
            fs::create_dir_all(&input).unwrap();
            fs::write(input.join(format!("file-{index}")), index.to_string()).unwrap();
            let status = Command::new("tar")
                .arg("-cf")
                .arg(distfiles.join(source_archive_name(index)))
                .arg("-C")
                .arg(input.parent().unwrap())
                .arg("top")
                .status()
                .unwrap();
            assert!(status.success());
        }
        fs::write(distfiles.join("manifest"), recipe.source_manifest()).unwrap();

        let mut class =
            Rclass::load(Path::new(env!("CARGO_MANIFEST_DIR")).join("../../rclass/cmake.toml"))
                .unwrap();
        class.phases.retain(|phase, _| phase == "src_unpack");
        let runner = compose_runner(
            &[class],
            &BTreeMap::from([
                ("SRC_DIR".into(), source.display().to_string()),
                ("JOBS".into(), "1".into()),
            ]),
        )
        .unwrap();
        let runner_path = directory.path().join("runner.sh");
        fs::write(&runner_path, runner).unwrap();
        assert!(Command::new("/bin/sh")
            .arg(runner_path)
            .status()
            .unwrap()
            .success());
        for index in 0..4 {
            assert_eq!(
                fs::read_to_string(source.join(format!("file-{index}"))).unwrap(),
                index.to_string()
            );
        }
        assert_eq!(
            fs::read_to_string(source.join("vendor/component/file-4")).unwrap(),
            "4"
        );
    }

    #[test]
    fn sysusers_are_staged_and_lifecycle_scripts_are_rejected() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("recipe.toml");
        fs::write(
            &path,
            format!(
                r#"schema_version=1
[package]
name="daemon"
version="1"
release=1
description="daemon"
license="MIT"
channel="system"
arch="any"

[source]
url="https://example.invalid/daemon.tar"
sha256="{}"

[[sysusers]]
type="user"
name="daemon"
id=75
description="Daemon User"
home="/var/lib/daemon"
shell="/usr/bin/nologin"
"#,
                "00".repeat(32)
            ),
        )
        .unwrap();
        let recipe = RecipeSpec::load(&path).unwrap();
        let root = directory.path().join("dest");
        fs::create_dir(&root).unwrap();
        stage_sysusers(&root, &recipe).unwrap();
        assert_eq!(
            fs::read_to_string(root.join("usr/lib/sysusers.d/daemon.conf")).unwrap(),
            "u daemon 75 \"Daemon User\" /var/lib/daemon /usr/bin/nologin\n"
        );

        fs::write(directory.path().join("preinst"), "#!/bin/sh\nread answer\n").unwrap();
        assert!(RecipeSpec::load(path)
            .unwrap_err()
            .to_string()
            .contains("preinst"));
    }

    #[test]
    fn shell_quote_does_not_open_single_quotes() {
        assert_eq!(shell_quote("a'b"), "'a'\\''b'");
        assert_eq!(tool_family("/usr/bin/clang++"), "clang");
        assert_eq!(tool_family("ld.lld"), "lld");
    }

    #[test]
    fn carving_assigns_each_file_to_first_matching_package() {
        let dest = tempfile::tempdir().unwrap();
        fs::create_dir_all(dest.path().join("usr/lib")).unwrap();
        fs::create_dir_all(dest.path().join("usr/include")).unwrap();
        fs::create_dir_all(dest.path().join("usr/bin")).unwrap();
        fs::write(dest.path().join("usr/lib/libx.so.1"), b"lib").unwrap();
        fs::write(dest.path().join("usr/include/x.h"), b"header").unwrap();
        fs::write(dest.path().join("usr/bin/x"), b"bin").unwrap();
        let recipe = RecipeSpec {
            schema_version: 1,
            package: RecipePackage {
                name: "x".into(),
                slot: sage_core::DEFAULT_SLOT.into(),
                version: "1.0".into(),
                release: 1,
                epoch: 0,
                description: "x".into(),
                license: "MIT".into(),
                channel: "system".into(),
                arch: "amd64".into(),
                dependencies: vec![],
                provides: vec![],
            },
            source: Some(SourceSpec {
                kind: SourceKind::Archive,
                url: "https://example.invalid/x".into(),
                sha256: "00".repeat(32),
                commit: String::new(),
                submodules: false,
                strip_components: None,
                destination: PathBuf::from("."),
            }),
            sources: vec![],
            build: RecipeBuild::default(),
            features: BTreeMap::new(),
            subpackages: vec![
                SubpackageSpec {
                    name: "x-libs".into(),
                    description: None,
                    license: None,
                    dependencies: vec![],
                    provides: vec![],
                    payload: PayloadSpec {
                        files: vec!["usr/lib/*.so.*".into()],
                        ..PayloadSpec::default()
                    },
                },
                SubpackageSpec {
                    name: "x-dev".into(),
                    description: None,
                    license: None,
                    dependencies: vec![],
                    provides: vec![],
                    payload: PayloadSpec {
                        files: vec!["usr/include/**".into()],
                        ..PayloadSpec::default()
                    },
                },
            ],
            sysusers: vec![],
        };
        let areas = PayloadCarver::carve_packages(dest.path(), &recipe).unwrap();
        assert!(areas[0].path().join("data/usr/bin/x").exists());
        assert!(areas[1].path().join("data/usr/lib/libx.so.1").exists());
        assert!(areas[2].path().join("data/usr/include/x.h").exists());
        assert!(!areas[0].path().join("data/usr/lib/libx.so.1").exists());
    }

    #[test]
    fn elf_scanner_reads_dynamic_dependencies() {
        let symbols = ElfScanner::scan(Path::new("/bin/ls")).unwrap();
        assert!(!symbols.dependencies.is_empty());
    }

    #[test]
    fn private_runpaths_are_relative_and_passed_without_a_shell() {
        use std::os::unix::fs::PermissionsExt;

        let directory = tempfile::tempdir().unwrap();
        let root = directory.path().join("dest");
        fs::create_dir_all(root.join("usr/bin")).unwrap();
        fs::create_dir_all(root.join("usr/lib")).unwrap();
        fs::copy("/bin/ls", root.join("usr/bin/tool")).unwrap();

        let patchelf = directory.path().join("patchelf");
        fs::write(
            &patchelf,
            "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$(dirname \"$0\")/args\"\n",
        )
        .unwrap();
        fs::set_permissions(&patchelf, fs::Permissions::from_mode(0o755)).unwrap();

        let report =
            ElfScanner::rewrite_private_runpaths(&root, &[PathBuf::from("usr/lib")], &patchelf)
                .unwrap();
        assert_eq!(report.rewritten, [PathBuf::from("usr/bin/tool")]);
        assert_eq!(report.library_dirs, [PathBuf::from("usr/lib")]);
        let arguments = fs::read_to_string(directory.path().join("args")).unwrap();
        assert!(arguments.lines().any(|line| line == "$ORIGIN/../lib"));
        assert!(ElfScanner::rewrite_private_runpaths(
            &root,
            &[PathBuf::from("../lib")],
            &patchelf,
        )
        .is_err());
    }

    #[test]
    fn kernel_module_tree_must_match_the_package_slot() {
        let directory = tempfile::tempdir().unwrap();
        fs::create_dir_all(directory.path().join("usr/lib/modules/6.12.4/updates")).unwrap();
        validate_kernel_module_slot(directory.path(), "6.12.4").unwrap();
        assert!(validate_kernel_module_slot(directory.path(), "6.13.0").is_err());
    }

    #[test]
    fn kmod_rclass_uses_the_package_slot_as_kernel_release() {
        let class =
            Rclass::load(Path::new(env!("CARGO_MANIFEST_DIR")).join("../../rclass/kmod.toml"))
                .unwrap();
        let variables = BTreeMap::from([
            ("PACKAGE_SLOT".into(), "6.12.4".into()),
            ("SRC_DIR".into(), "/source".into()),
            ("DESTDIR".into(), "/dest".into()),
            ("JOBS".into(), "8".into()),
            ("args.make_args".into(), String::new()),
        ]);
        let runner = compose_runner(&[class], &variables).unwrap();
        assert!(runner.contains("/usr/lib/modules/6.12.4/build"));
        assert!(runner.contains("INSTALL_MOD_PATH=\"/dest\""));
    }
}
