//! Source builds, mass rebuilds, bootstrap orchestration, and package staging.

use anyhow::{bail, Context, Result};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use crate::package_model::{
    canonical_channel, load_available_with_pool, obtain_release_archive, ReleaseLocation,
};
use crate::paths::under_root;

pub(crate) struct BuildManager<'a> {
    root: &'a Path,
    dry_run: bool,
}

impl<'a> BuildManager<'a> {
    pub(crate) fn new(root: &'a Path, dry_run: bool) -> Self {
        Self { root, dry_run }
    }

    pub(crate) async fn build(
        &self,
        recipe_dir: &Path,
        features: &[String],
        use_default_features: bool,
    ) -> Result<()> {
        build_recipe(
            self.root,
            recipe_dir,
            features,
            use_default_features,
            self.dry_run,
            BuildInvocation::default(),
        )
        .await
    }

    pub(crate) async fn mass_rebuild(
        &self,
        recipe_root: &Path,
        output: Option<&Path>,
        jobs: usize,
    ) -> Result<()> {
        mass_rebuild(self.root, recipe_root, output, jobs, self.dry_run).await
    }

    pub(crate) async fn bootstrap(
        &self,
        plan: &Path,
        output: Option<&Path>,
        jobs: usize,
    ) -> Result<()> {
        bootstrap_sources(self.root, plan, output, jobs, self.dry_run).await
    }
}

pub async fn mass_rebuild(
    root: &Path,
    recipe_root: &Path,
    output: Option<&Path>,
    jobs: usize,
    dry_run: bool,
) -> Result<()> {
    let units = source_build_units(root, sage_build::BuildGraph::discover(recipe_root)?)?;
    let layers = sage_build::BuildGraph::layers(units)?;
    let pool = output
        .map(Path::to_path_buf)
        .unwrap_or_else(|| recipe_root.join(".sage-packages"));
    prepare_source_pool(&pool, dry_run)?;
    execute_source_layers(root, &layers, &pool, jobs, dry_run).await
}

/// Executes the explicitly staged bootstrap plan.
pub async fn bootstrap_sources(
    root: &Path,
    plan_path: &Path,
    output: Option<&Path>,
    jobs: usize,
    dry_run: bool,
) -> Result<()> {
    let plan = sage_build::BootstrapPlan::load(plan_path)?;
    let base = plan_path.parent().unwrap_or(Path::new("."));
    let pool = output
        .map(Path::to_path_buf)
        .unwrap_or_else(|| base.join(".sage-bootstrap"));
    prepare_source_pool(&pool, dry_run)?;
    for stage in plan.stages {
        println!("Bootstrap stage {}", stage.name);
        let mut units = Vec::new();
        for declared in stage.recipes {
            let path = base.join(declared);
            let recipe_path = if path.is_dir() {
                path.join("recipe.toml")
            } else {
                path
            };
            let recipe = sage_build::RecipeSpec::load(&recipe_path)?;
            units.push(sage_build::BuildUnit::from_recipe(recipe_path, &recipe)?);
        }
        let units = source_build_units(root, units)?;
        let layers = sage_build::BuildGraph::layers(units)?;
        execute_source_layers(root, &layers, &pool, jobs, dry_run).await?;
    }
    Ok(())
}

fn source_build_units(
    root: &Path,
    mut units: Vec<sage_build::BuildUnit>,
) -> Result<Vec<sage_build::BuildUnit>> {
    for unit in &mut units {
        let recipe = sage_build::RecipeSpec::load(&unit.recipe)?;
        let mut dependencies = Vec::new();
        for inherited in &recipe.build.inherit {
            let class = sage_build::Rclass::load(find_rclass(
                unit.recipe.parent().unwrap_or(Path::new(".")),
                root,
                inherited,
            )?)?;
            dependencies.extend(class.implicit_build_dependencies);
        }
        unit.include_dependencies(dependencies)?;
    }
    Ok(units)
}

fn prepare_source_pool(pool: &Path, dry_run: bool) -> Result<()> {
    if pool.exists()
        && std::fs::read_dir(pool)?.any(|entry| {
            entry.ok().is_some_and(|entry| {
                entry
                    .file_name()
                    .to_str()
                    .is_some_and(|name| name.ends_with(".pkg.tar.zst"))
            })
        })
    {
        bail!(
            "source build output must not contain existing packages: {}",
            pool.display()
        );
    }
    if !dry_run {
        std::fs::create_dir_all(pool)?;
    }
    Ok(())
}

async fn execute_source_layers(
    root: &Path,
    layers: &[Vec<sage_build::BuildUnit>],
    pool: &Path,
    requested_jobs: usize,
    dry_run: bool,
) -> Result<()> {
    if dry_run {
        for (index, layer) in layers.iter().enumerate() {
            println!(
                "Layer {}: {}",
                index + 1,
                layer
                    .iter()
                    .map(|unit| unit.name.as_str())
                    .collect::<Vec<_>>()
                    .join(", ")
            );
        }
        return Ok(());
    }
    let config =
        sage_build::BuildConfig::load(under_root(root, Path::new("/etc/sage/build.toml")))?;
    for (layer_index, layer) in layers.iter().enumerate() {
        let concurrency = if requested_jobs == 0 {
            config.jobs.min(layer.len()).max(1)
        } else {
            requested_jobs.min(layer.len()).max(1)
        };
        let inner_jobs = (config.jobs / concurrency).max(1);
        println!("Build layer {} ({} packages)", layer_index + 1, layer.len());
        for chunk in layer.chunks(concurrency) {
            let outputs = tempfile::Builder::new()
                .prefix(".sage-layer-")
                .tempdir_in(pool)?;
            let mut tasks = tokio::task::JoinSet::new();
            for (index, unit) in chunk.iter().enumerate() {
                let root = root.to_path_buf();
                let recipe = unit.recipe.clone();
                let output = outputs.path().join(format!("{index:04}"));
                let pool = pool.to_path_buf();
                std::fs::create_dir(&output)?;
                tasks.spawn(async move {
                    build_recipe(
                        &root,
                        &recipe,
                        &[],
                        true,
                        false,
                        BuildInvocation {
                            output_dir: Some(output.clone()),
                            local_pool: Some(pool),
                            jobs_override: Some(inner_jobs),
                        },
                    )
                    .await
                    .map(|_| output)
                });
            }
            let mut completed = Vec::new();
            while let Some(result) = tasks.join_next().await {
                completed.push(result??);
            }
            completed.sort();
            for directory in completed {
                let mut artifacts: Vec<_> = std::fs::read_dir(directory)?
                    .collect::<Result<Vec<_>, _>>()?
                    .into_iter()
                    .map(|entry| entry.path())
                    .filter(|path| {
                        path.file_name()
                            .and_then(|name| name.to_str())
                            .is_some_and(|name| name.ends_with(".pkg.tar.zst"))
                    })
                    .collect();
                artifacts.sort();
                for artifact in artifacts {
                    let target = pool.join(artifact.file_name().unwrap());
                    std::fs::rename(&artifact, &target)?;
                    println!("Published {}", target.display());
                }
            }
        }
    }
    Ok(())
}

#[derive(Default)]
struct BuildInvocation {
    output_dir: Option<PathBuf>,
    local_pool: Option<PathBuf>,
    jobs_override: Option<usize>,
}

async fn build_recipe(
    root: &Path,
    recipe_dir: &Path,
    requested_features: &[String],
    use_default_features: bool,
    dry_run: bool,
    invocation: BuildInvocation,
) -> Result<()> {
    let recipe_path = if recipe_dir.is_dir() {
        recipe_dir.join("recipe.toml")
    } else {
        recipe_dir.to_path_buf()
    };
    let mut recipe = sage_build::RecipeSpec::load(&recipe_path)?;
    let features = recipe.effective_features(requested_features, use_default_features)?;
    recipe.package.dependencies.extend(
        features
            .dependencies
            .iter()
            .map(|value| value.parse::<sage_core::Dependency>())
            .collect::<Result<Vec<_>, _>>()?,
    );
    recipe.package.dependencies.sort();
    recipe.package.dependencies.dedup();
    recipe.build.args.extend(features.args.clone());
    let build_config_path = under_root(root, Path::new("/etc/sage/build.toml"));
    let mut config = sage_build::BuildConfig::load(&build_config_path)
        .with_context(|| format!("failed to load {}", build_config_path.display()))?;
    if let Some(jobs) = invocation.jobs_override {
        config.jobs = jobs.max(1);
    }
    let cross = config.cross_target(&recipe.build.target)?.cloned();
    if let Some(target) = &cross {
        recipe.build.args.extend([
            (
                "configure_triples".into(),
                format!("--build={} --host={}", config.build, recipe.build.target),
            ),
            (
                "cross_args".into(),
                format!(
                    "-DCMAKE_SYSTEM_NAME={} -DCMAKE_SYSTEM_PROCESSOR={}",
                    target.cmake_system_name, target.arch
                ),
            ),
            (
                "cross_arg".into(),
                "--cross-file /build/sage-cross.ini".into(),
            ),
            (
                "target_arg".into(),
                format!("--target {}", recipe.build.target),
            ),
        ]);
    }
    let mut classes = Vec::new();
    for inherited in &recipe.build.inherit {
        classes.push(sage_build::Rclass::load(find_rclass(
            recipe_path.parent().unwrap_or(Path::new(".")),
            root,
            inherited,
        )?)?);
    }
    if !features.env.is_empty() {
        classes.push(sage_build::Rclass {
            schema_version: sage_core::SCHEMA_VERSION,
            name: "selected-features".into(),
            description: "Environment selected by recipe features".into(),
            implicit_build_dependencies: Vec::new(),
            allowed_compilers: Vec::new(),
            allowed_linkers: Vec::new(),
            defaults: BTreeMap::new(),
            env: features.env.clone(),
            phases: BTreeMap::new(),
        });
    }
    let mut selected_config = config.clone();
    if let Some(target) = &cross {
        selected_config.cc.clone_from(&target.cc);
        selected_config.cxx.clone_from(&target.cxx);
        let mut cross_env = BTreeMap::from([
            ("CC".into(), target.cc.clone()),
            ("CXX".into(), target.cxx.clone()),
            ("AR".into(), target.ar.clone()),
            ("STRIP".into(), target.strip.clone()),
            ("GOOS".into(), target.goos.clone()),
            ("GOARCH".into(), target.goarch.clone()),
            (
                "RUSTFLAGS".into(),
                format!("{} {}", config.rustflags, target.rustflags)
                    .trim()
                    .into(),
            ),
        ]);
        if !recipe.build.target_dependencies.is_empty() || !features.target_dependencies.is_empty()
        {
            cross_env.extend([
                (
                    "CFLAGS".into(),
                    format!("{} --sysroot=/sysroot", config.cflags),
                ),
                (
                    "CXXFLAGS".into(),
                    format!("{} --sysroot=/sysroot", config.cxxflags),
                ),
                (
                    "LDFLAGS".into(),
                    format!("{} --sysroot=/sysroot", config.ldflags),
                ),
            ]);
        }
        classes.push(sage_build::Rclass {
            schema_version: sage_core::SCHEMA_VERSION,
            name: "cross-target".into(),
            description: "Configured cross compilation environment".into(),
            implicit_build_dependencies: Vec::new(),
            allowed_compilers: Vec::new(),
            allowed_linkers: Vec::new(),
            defaults: BTreeMap::new(),
            env: cross_env,
            phases: BTreeMap::new(),
        });
    }
    sage_build::validate_toolchain(&classes, &selected_config)?;
    let build_dependencies = sage_build::build_dependencies(&recipe, &classes, &features)?;
    let target_dependencies = sage_build::target_dependencies(&recipe, &features)?;
    if cross.is_none() && !target_dependencies.is_empty() {
        bail!("target_dependencies require [build].target");
    }
    if dry_run {
        println!(
            "Would build {}-{}-{} for {} using {:?}",
            recipe.package.name,
            recipe.package.version,
            recipe.package.release,
            recipe.package.arch,
            recipe.build.inherit
        );
        for dependency in &build_dependencies {
            println!("Build-depends: {}", dependency.name);
        }
        for dependency in &target_dependencies {
            println!("Target-depends: {}", dependency.name);
        }
        return Ok(());
    }
    let workspace = tempfile::Builder::new().prefix("sage-build-").tempdir()?;
    let source = workspace.path().join("source");
    let build = workspace.path().join("build");
    let destdir = workspace.path().join("dest");
    std::fs::create_dir_all(&source)?;
    std::fs::create_dir(&build)?;
    std::fs::create_dir(&destdir)?;
    let toolchain = prepare_build_toolchain(
        root,
        workspace.path(),
        &recipe.package.channel,
        &build_dependencies,
        invocation.local_pool.as_deref(),
    )
    .await?;
    let target_sysroot = if let Some(target) = &cross {
        prepare_package_tree(
            root,
            workspace.path(),
            "sysroot",
            &recipe.package.channel,
            &target_dependencies,
            Some(&target.arch),
            invocation.local_pool.as_deref(),
        )
        .await?
    } else {
        None
    };
    let distfiles = source.join(".distfiles");
    std::fs::create_dir(&distfiles)?;
    let engine = sage_repo::DownloadEngine::new(workspace.path().join("cache"))?;
    for (index, input) in recipe.source_inputs().enumerate() {
        let staged = distfiles.join(sage_build::source_archive_name(index));
        match input.kind {
            sage_build::SourceKind::Archive => {
                engine
                    .download_url(&input.url, &staged, &input.sha256)
                    .await?;
            }
            sage_build::SourceKind::Git => {
                let git = config.git.clone();
                let input = input.clone();
                let checkout = workspace.path().join(format!("git-checkout-{index:03}"));
                tokio::task::spawn_blocking(move || {
                    sage_build::fetch_git_source(&git, &input, &checkout, &staged)
                })
                .await??;
            }
        }
    }
    std::fs::write(distfiles.join("manifest"), recipe.source_manifest())?;
    stage_patches(recipe_path.parent().unwrap_or(Path::new(".")), &source)?;
    let variables = build_variables(&config, &recipe);
    let runner_contents = sage_build::compose_runner(&classes, &variables)?;
    let runner = workspace.path().join("sage-build-runner.sh");
    std::fs::write(&runner, runner_contents)?;
    let paths = sage_build::SandboxPaths {
        source,
        build,
        destdir: destdir.clone(),
        runner,
        toolchain,
        target_sysroot,
    };
    let managed_build_tools =
        sage_build::SandboxRunner::new(&selected_config).run(&paths, recipe.build.allow_network)?;
    sage_build::stage_sysusers(&destdir, &recipe)?;
    sage_build::validate_kernel_module_slot(&destdir, &recipe.package.slot)?;
    if recipe.uses_private_channel() {
        let report = sage_build::ElfScanner::rewrite_private_runpaths(
            &destdir,
            &recipe.build.private_library_dirs,
            &config.patchelf,
        )?;
        tracing::info!(
            files = report.rewritten.len(),
            directories = report.library_dirs.len(),
            "rewrote private-channel ELF RUNPATHs"
        );
    }
    let areas = sage_build::PayloadCarver::carve_packages(&destdir, &recipe)?;
    let output_dir = invocation
        .output_dir
        .as_deref()
        .unwrap_or_else(|| recipe_path.parent().unwrap_or(Path::new(".")));
    for area in areas {
        package_staging(
            &recipe,
            &area,
            recipe_path.parent().unwrap_or(Path::new(".")),
            output_dir,
            config.source_date_epoch,
            &features.enabled,
            &managed_build_tools,
        )?;
    }
    Ok(())
}

/// Resolves, downloads, and extracts build-only packages into an ephemeral
/// prefix. The resulting tree is mounted read-only and is never registered in
/// the host database, so failed builds leave no installed state behind.
async fn prepare_build_toolchain(
    root: &Path,
    workspace: &Path,
    channel: &str,
    dependencies: &[sage_core::Dependency],
    local_pool: Option<&Path>,
) -> Result<Option<PathBuf>> {
    prepare_package_tree(
        root,
        workspace,
        "toolchain",
        channel,
        dependencies,
        None,
        local_pool,
    )
    .await
}

async fn prepare_package_tree(
    root: &Path,
    workspace: &Path,
    tree_name: &str,
    channel: &str,
    dependencies: &[sage_core::Dependency],
    architecture: Option<&str>,
    local_pool: Option<&Path>,
) -> Result<Option<PathBuf>> {
    if dependencies.is_empty() {
        return Ok(None);
    }
    let available = load_available_with_pool(root, architecture, local_pool)?;
    let channel = canonical_channel(&available, Some(channel))?;
    let local_locks = available
        .releases
        .iter()
        .filter(|(_, source)| matches!(source.location, ReleaseLocation::Local(_)))
        .map(|(coordinate, _)| (coordinate.key.clone(), coordinate.version.clone()));
    let solution = sage_solver::SageSolver::with_locked(&available.universe, local_locks)
        .resolve_dependencies(&channel, dependencies)?;
    let tree = workspace.join(tree_name);
    std::fs::create_dir(&tree)?;
    let package_cache = under_root(root, Path::new("/var/cache/sage/packages"));
    let engine = sage_repo::DownloadEngine::new(&package_cache)?;
    let mut owned = std::collections::BTreeSet::new();
    for (key, version) in solution {
        let coordinate = sage_core::PackageCoordinate::new(key.clone(), version.clone());
        let source = available
            .releases
            .get(&coordinate)
            .with_context(|| format!("index record disappeared for build dependency {key}"))?;
        let archive = obtain_release_archive(&engine, &package_cache, source).await?;
        let inspection = sage_archive::inspect_package(&archive)?;
        if let Some(path) = inspection
            .files
            .iter()
            .map(|record| &record.path)
            .find(|path| !owned.insert((*path).clone()))
        {
            bail!("build dependency file conflict at {}", path.display());
        }
        sage_archive::extract_package(&archive, &tree, &inspection.files)?;
        println!("{} dependency {} {}", tree_name, key, version);
    }
    Ok(Some(tree))
}

fn stage_patches(recipe_dir: &Path, source: &Path) -> Result<()> {
    let patches = recipe_dir.join("patches");
    if !patches.exists() {
        return Ok(());
    }
    let target = source.join(".patches");
    std::fs::create_dir(&target)?;
    let mut entries: Vec<_> = std::fs::read_dir(&patches)?.collect::<Result<_, _>>()?;
    entries.sort_by_key(|entry| entry.file_name());
    for entry in entries {
        if !entry.file_type()?.is_file() {
            bail!(
                "patch entry must be a regular file: {}",
                entry.path().display()
            );
        }
        std::fs::copy(entry.path(), target.join(entry.file_name()))?;
    }
    Ok(())
}

fn find_rclass(recipe_dir: &Path, root: &Path, name: &str) -> Result<PathBuf> {
    let filename = format!("{name}.toml");
    for ancestor in recipe_dir.ancestors() {
        let candidate = ancestor.join("rclass").join(&filename);
        if candidate.exists() {
            return Ok(candidate);
        }
    }
    let installed = under_root(root, &Path::new("/usr/share/sage/rclass").join(filename));
    if installed.exists() {
        Ok(installed)
    } else {
        bail!("rclass '{name}' was not found")
    }
}

fn build_variables(
    config: &sage_build::BuildConfig,
    recipe: &sage_build::RecipeSpec,
) -> std::collections::BTreeMap<String, String> {
    let mut variables = std::collections::BTreeMap::from([
        ("JOBS".into(), config.jobs.to_string()),
        ("CFLAGS".into(), config.cflags.clone()),
        ("CXXFLAGS".into(), config.cxxflags.clone()),
        ("CPPFLAGS".into(), config.cppflags.clone()),
        ("LDFLAGS".into(), config.ldflags.clone()),
        ("RUSTFLAGS".into(), config.rustflags.clone()),
        ("SRC_DIR".into(), "/source".into()),
        ("BUILD_DIR".into(), "/build".into()),
        ("DESTDIR".into(), "/dest".into()),
        ("PACKAGE_SLOT".into(), recipe.package.slot.clone()),
        ("BUILD_TRIPLE".into(), config.build.clone()),
        ("TARGET_TRIPLE".into(), recipe.build.target.clone()),
        (
            "TARGET_ARCH".into(),
            config
                .targets
                .get(&recipe.build.target)
                .map(|target| target.arch.clone())
                .unwrap_or_default(),
        ),
        (
            "TARGET_ENDIAN".into(),
            config
                .targets
                .get(&recipe.build.target)
                .map(|target| target.endian.clone())
                .unwrap_or_default(),
        ),
        (
            "GOOS".into(),
            config
                .targets
                .get(&recipe.build.target)
                .map(|target| target.goos.clone())
                .unwrap_or_default(),
        ),
    ]);
    variables.extend(
        recipe
            .build
            .args
            .iter()
            .map(|(key, value)| (format!("args.{key}"), value.clone())),
    );
    variables
}

fn package_staging(
    recipe: &sage_build::RecipeSpec,
    area: &sage_build::PackageStagingArea,
    recipe_dir: &Path,
    output_dir: &Path,
    build_time: u64,
    features: &std::collections::BTreeSet<String>,
    managed_build_tools: &[sage_archive::ManagedBuildTool],
) -> Result<()> {
    let data = area.path().join("data");
    let records = sage_archive::build_file_index(&data)?;
    let elf = sage_build::ElfScanner::scan(&data)?;
    let metadata = area.path().join(".METADATA");
    std::fs::create_dir(&metadata)?;
    if area.name == recipe.package.name {
        stage_declarative_metadata(recipe_dir, &metadata)?;
    }
    std::fs::write(
        metadata.join("files.idx"),
        sage_archive::format_file_index(&records),
    )?;
    let subpackage = recipe
        .subpackages
        .iter()
        .find(|subpackage| subpackage.name == area.name);
    let mut dependency_strings = area.dependencies.clone();
    dependency_strings.extend(elf.dependencies);
    dependency_strings.sort();
    dependency_strings.dedup();
    let dependencies = dependency_strings
        .into_iter()
        .map(|value| value.parse::<sage_core::Dependency>())
        .collect::<Result<Vec<_>, _>>()?;
    let mut provides = area.provides.clone();
    provides.extend(elf.provides);
    provides.sort();
    provides.dedup();
    let manifest = sage_archive::PackageManifest {
        schema_version: sage_core::SCHEMA_VERSION,
        name: area.name.clone(),
        slot: recipe.package.slot.clone(),
        version: recipe.package.version.clone(),
        release: recipe.package.release,
        epoch: recipe.package.epoch,
        arch: recipe.package.arch.clone(),
        channel: recipe.package.channel.clone(),
        description: subpackage
            .and_then(|package| package.description.clone())
            .unwrap_or_else(|| recipe.package.description.clone()),
        license: subpackage
            .and_then(|package| package.license.clone())
            .unwrap_or_else(|| recipe.package.license.clone()),
        installed_size: records.iter().map(|record| record.size).sum(),
        build_time,
        dependencies,
        provides,
        conflicts: Vec::new(),
        features: features.iter().cloned().collect(),
        managed_build_tools: managed_build_tools.to_vec(),
    };
    std::fs::write(
        metadata.join("manifest.toml"),
        toml::to_string_pretty(&manifest)?,
    )?;
    let output = output_dir.join(format!(
        "{}-{}-{}-{}.pkg.tar.zst",
        area.name, recipe.package.version, recipe.package.release, recipe.package.arch
    ));
    sage_archive::create_package(area.path(), &output, 15)?;
    println!("Created {}", output.display());
    Ok(())
}

/// Validates and copies declarative service and trigger metadata into a package.
pub fn stage_declarative_metadata(recipe_dir: &Path, metadata: &Path) -> Result<()> {
    let service = recipe_dir.join("service.toml");
    if service.exists() {
        sage_sys::ServiceSpec::load(&service)?;
        std::fs::copy(service, metadata.join("service.toml"))?;
    }
    let triggers = recipe_dir.join("triggers.toml");
    if triggers.exists() {
        sage_sys::TriggerSpec::load(&triggers)?;
        std::fs::copy(triggers, metadata.join("triggers.toml"))?;
    }
    Ok(())
}
