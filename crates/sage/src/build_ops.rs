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
    reset_failure_report(&pool, dry_run)?;
    let mut blocked_symbols = std::collections::BTreeSet::new();
    execute_source_layers(root, &layers, &pool, jobs, dry_run, &mut blocked_symbols).await
}
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
    reset_failure_report(&pool, dry_run)?;
    let mut blocked_symbols = std::collections::BTreeSet::new();
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
        execute_source_layers(root, &layers, &pool, jobs, dry_run, &mut blocked_symbols).await?;
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
    // A bootstrap output pool may contain explicitly preserved seed archives.
    // New artifacts are published atomically by the layer executor and replace
    // the matching seed filename when a package is rebuilt.
    if !dry_run {
        std::fs::create_dir_all(pool)?;
    }
    Ok(())
}

fn reset_failure_report(pool: &Path, dry_run: bool) -> Result<()> {
    if !dry_run {
        let report = pool.join("build-failures.log");
        if report.exists() {
            std::fs::remove_file(report)?;
        }
    }
    Ok(())
}

fn record_failure(report: &Path, message: &str) -> Result<()> {
    let mut file = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(report)?;
    std::io::Write::write_all(&mut file, format!("{message}\n").as_bytes())?;
    Ok(())
}

async fn execute_source_layers(
    root: &Path,
    layers: &[Vec<sage_build::BuildUnit>],
    pool: &Path,
    requested_jobs: usize,
    dry_run: bool,
    blocked_symbols: &mut std::collections::BTreeSet<String>,
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
    let failure_report = pool.join("build-failures.log");
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
                if unit
                    .consumed_symbol_ids()
                    .iter()
                    .any(|symbol| blocked_symbols.contains(symbol))
                {
                    let message = format!(
                        "{}: blocked by a failed dependency; not scheduled",
                        unit.name
                    );
                    println!("Skip {message}");
                    record_failure(&failure_report, &message)?;
                    blocked_symbols.extend(unit.produced_symbol_ids());
                    continue;
                }
                let root = root.to_path_buf();
                let recipe = unit.recipe.clone();
                let unit_name = unit.name.clone();
                let produced_symbols = unit.produced_symbol_ids();
                let output = outputs.path().join(format!("{index:04}"));
                let pool = pool.to_path_buf();
                std::fs::create_dir(&output)?;
                tasks.spawn(async move {
                    let result = build_recipe(
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
                    .map(|_| output);
                    (unit_name, produced_symbols, result)
                });
            }
            let mut completed = Vec::new();
            while let Some(result) = tasks.join_next().await {
                match result {
                    Ok((unit_name, produced_symbols, Ok(output))) => {
                        completed.push((unit_name, produced_symbols, output));
                    }
                    Ok((unit_name, produced_symbols, Err(error))) => {
                        let message = format!("{unit_name}: {error:#}");
                        eprintln!("Build failed: {message}");
                        record_failure(&failure_report, &message)?;
                        blocked_symbols.extend(produced_symbols);
                    }
                    Err(error) => {
                        let message = format!("build task panicked or was cancelled: {error}");
                        eprintln!("Build failed: {message}");
                        record_failure(&failure_report, &message)?;
                    }
                }
            }
            completed.sort_by(|left, right| left.2.cmp(&right.2));
            for (unit_name, produced_symbols, directory) in completed {
                let artifacts = match std::fs::read_dir(&directory) {
                    Ok(entries) => {
                        let mut artifacts: Vec<_> = entries
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
                        artifacts
                    }
                    Err(error) => {
                        let message = format!(
                            "{unit_name}: failed to read build output {}: {error}",
                            directory.display()
                        );
                        eprintln!("Build failed: {message}");
                        record_failure(&failure_report, &message)?;
                        blocked_symbols.extend(produced_symbols);
                        continue;
                    }
                };
                let mut publish_failed = false;
                for artifact in artifacts {
                    let inspection = match sage_archive::inspect_package(&artifact) {
                        Ok(inspection) => inspection,
                        Err(error) => {
                            let message = format!(
                                "{unit_name}: failed to inspect {}: {error}",
                                artifact.display()
                            );
                            eprintln!("Build failed: {message}");
                            record_failure(&failure_report, &message)?;
                            publish_failed = true;
                            continue;
                        }
                    };
                    let Some(file_name) = artifact.file_name() else {
                        let message = format!(
                            "{unit_name}: build output has no filename: {}",
                            artifact.display()
                        );
                        eprintln!("Build failed: {message}");
                        record_failure(&failure_report, &message)?;
                        publish_failed = true;
                        continue;
                    };
                    let target = pool
                        .join(".slots")
                        .join(&inspection.manifest.channel)
                        .join(&inspection.manifest.name)
                        .join(&inspection.manifest.slot)
                        .join(file_name);
                    if let Some(parent) = target.parent() {
                        if let Err(error) = std::fs::create_dir_all(parent) {
                            let message = format!(
                                "{unit_name}: failed to create package destination {}: {error}",
                                parent.display()
                            );
                            eprintln!("Build failed: {message}");
                            record_failure(&failure_report, &message)?;
                            publish_failed = true;
                            continue;
                        }
                    }
                    if let Err(error) = std::fs::rename(&artifact, &target) {
                        let message = format!(
                            "{unit_name}: failed to publish {}: {error}",
                            artifact.display()
                        );
                        eprintln!("Build failed: {message}");
                        record_failure(&failure_report, &message)?;
                        publish_failed = true;
                        continue;
                    }
                    if let Err(error) = remove_superseded_pool_artifacts(
                        pool,
                        &target,
                        &inspection.manifest,
                    ) {
                        let message = format!(
                            "{unit_name}: failed to retire superseded bootstrap seed: {error}"
                        );
                        eprintln!("Build failed: {message}");
                        record_failure(&failure_report, &message)?;
                        publish_failed = true;
                        continue;
                    }
                    println!("Published {}", target.display());
                }
                if publish_failed {
                    blocked_symbols.extend(produced_symbols);
                }
            }
        }
    }
    if failure_report.exists() {
        bail!(
            "source build failed; review {}",
            failure_report.display()
        );
    }
    Ok(())
}

/// Keeps exactly one local release for each package identity. A seed remains
/// available until its replacement has been renamed into the pool, then all
/// older flat or slotted copies of that channel/name/Slot are retired.
fn remove_superseded_pool_artifacts(
    pool: &Path,
    replacement: &Path,
    manifest: &sage_archive::PackageManifest,
) -> Result<()> {
    let replacement = replacement.canonicalize()?;
    for entry in walkdir::WalkDir::new(pool).follow_links(false) {
        let entry = entry?;
        let path = entry.path();
        if !entry.file_type().is_file()
            || !path
                .file_name()
                .and_then(|name| name.to_str())
                .is_some_and(|name| name.ends_with(".pkg.tar.zst"))
            || path.canonicalize()? == replacement
        {
            continue;
        }
        let candidate = sage_archive::inspect_package(path)?;
        if candidate.manifest.channel == manifest.channel
            && candidate.manifest.name == manifest.name
            && candidate.manifest.slot == manifest.slot
        {
            std::fs::remove_file(path)?;
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
    let service_path = recipe_path
        .parent()
        .unwrap_or(Path::new("."))
        .join("service.toml");
    if service_path.exists() {
        let outputs = std::collections::BTreeSet::from_iter(
            std::iter::once(recipe.package.name.clone()).chain(
                recipe
                    .subpackages
                    .iter()
                    .map(|package| package.name.clone()),
            ),
        );
        sage_sys::ServiceDocument::load(service_path)?.validate_output_packages(&outputs)?;
    }
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
    sage_build::merge_build_arguments(&mut recipe.build.args, &features.args);
    let build_config_path = under_root(root, Path::new("/etc/sage/build.toml"));
    let mut config = sage_build::BuildConfig::load(&build_config_path)
        .with_context(|| format!("failed to load {}", build_config_path.display()))?;
    if let Some(jobs) = invocation.jobs_override {
        config.jobs = jobs.max(1);
    }
    if let Some(linker) = &recipe.build.linker {
        config.linker.clone_from(linker);
    }
    if let Some(compiler) = &recipe.build.compiler {
        match sage_build::tool_family(compiler).as_str() {
            "clang" => {
                config.cc = "clang".into();
                config.cxx = "clang++".into();
            }
            "gcc" => {
                config.cc = "gcc".into();
                config.cxx = "g++".into();
            }
            _ => unreachable!("recipe compiler was validated during load"),
        }
    }
    if let Some(flags) = &recipe.build.cflags {
        config.cflags.clone_from(flags);
    }
    if let Some(flags) = &recipe.build.cxxflags {
        config.cxxflags.clone_from(flags);
    }
    if let Some(flags) = &recipe.build.cppflags {
        config.cppflags.clone_from(flags);
    }
    if let Some(flags) = &recipe.build.ldflags {
        config.ldflags.clone_from(flags);
    }
    if let Some(flags) = &recipe.build.rustflags {
        config.rustflags.clone_from(flags);
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
            ("CC".into(), "/build/.sage-tools/cc".into()),
            ("CXX".into(), "/build/.sage-tools/cxx".into()),
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
    let mut build_dependencies = sage_build::build_dependencies(&recipe, &classes, &features)?;
    build_dependencies.extend(
        config
            .sandbox_dependencies
            .iter()
            .map(|value| value.parse::<sage_core::Dependency>())
            .collect::<Result<Vec<_>, _>>()?,
    );
    let mut unique_build_dependencies = Vec::new();
    for dependency in build_dependencies {
        if !unique_build_dependencies.contains(&dependency) {
            unique_build_dependencies.push(dependency);
        }
    }
    let build_dependencies = unique_build_dependencies;
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
    // Source inputs are content-addressed independently of the installed
    // package database. A failed build can therefore resume without fetching
    // the same verified tarballs again, while stale bytes still fail the hash
    // gate and are atomically replaced by DownloadEngine.
    let source_cache = under_root(root, Path::new("/var/cache/sage/sources"));
    std::fs::create_dir_all(&source_cache)?;
    let engine = sage_repo::DownloadEngine::new(&source_cache)?;
    for (index, input) in recipe.source_inputs().enumerate() {
        let staged = distfiles.join(sage_build::source_archive_name(index));
        match input.kind {
            sage_build::SourceKind::Archive => {
                let cached = source_cache.join(&input.sha256);
                engine
                    .download_url(&input.url, &cached, &input.sha256)
                    .await?;
                if std::fs::hard_link(&cached, &staged).is_err() {
                    std::fs::copy(&cached, &staged)?;
                }
            }
            sage_build::SourceKind::File => {
                let cached = source_cache.join(&input.sha256);
                engine
                    .download_url(&input.url, &cached, &input.sha256)
                    .await?;
                if std::fs::hard_link(&cached, &staged).is_err() {
                    std::fs::copy(&cached, &staged)?;
                }
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
        source: source.clone(),
        build,
        destdir: destdir.clone(),
        runner,
        toolchain,
        target_sysroot,
    };
    // Pure data packages run no command, so entering bwrap adds no isolation.
    let managed_build_tools = if recipe.source_inputs().next().is_none()
        && classes.iter().all(|class| class.phases.is_empty())
    {
        Vec::new()
    } else {
        sage_build::SandboxRunner::new(&selected_config).run(&paths, recipe.build.allow_network)?
    };
    sage_build::stage_declarative_install(&destdir, &source, &recipe)?;
    sage_build::validate_kernel_module_slot(&destdir, &recipe.package.slot)?;
    let areas = sage_build::PayloadCarver::carve_packages(&destdir, &recipe)?;
    let output_dir = invocation
        .output_dir
        .as_deref()
        .unwrap_or_else(|| recipe_path.parent().unwrap_or(Path::new(".")));
    for area in areas {
        let subpackage = recipe
            .subpackages
            .iter()
            .find(|subpackage| subpackage.name == area.name);
        let package_channel = subpackage
            .and_then(|package| package.channel.as_deref())
            .unwrap_or(&recipe.package.channel);
        if package_channel != "system" && !package_channel.ends_with("/system") {
            let report = sage_build::ElfScanner::rewrite_private_runpaths(
                &area.path().join("data"),
                &recipe.build.private_library_dirs,
                &config.patchelf,
            )?;
            tracing::info!(
                package = area.name,
                files = report.rewritten.len(),
                directories = report.library_dirs.len(),
                "rewrote private-channel ELF RUNPATHs"
            );
        }
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
        .map(|((key, version), _)| (key.clone(), version.clone()));
    let solution = sage_solver::SageSolver::with_locked(&available.universe, local_locks)
        .resolve_dependencies(&channel, dependencies)?;
    let tree = workspace.join(tree_name);
    std::fs::create_dir(&tree)?;
    let package_cache = under_root(root, Path::new("/var/cache/sage/packages"));
    let engine = sage_repo::DownloadEngine::new(&package_cache)?;
    let mut owned: std::collections::BTreeMap<
        PathBuf,
        (sage_core::PackageKey, sage_core::Version),
    > = std::collections::BTreeMap::new();
    for (key, version) in solution {
        let source = available
            .releases
            .get(&(key.clone(), version.clone()))
            .with_context(|| format!("index record disappeared for build dependency {key}"))?;
        let archive = obtain_release_archive(&engine, &package_cache, source).await?;
        let inspection = sage_archive::inspect_package(&archive).with_context(|| {
            format!(
                "failed to inspect build dependency archive {} for {} {}",
                archive.display(),
                key,
                version
            )
        })?;
        if let Some((path, owner)) = inspection
            .files
            .iter()
            .map(|record| record.path.clone())
            .filter(|path| !is_shared_build_metadata(path))
            .find_map(|path| {
                if let Some(owner) = owned.get(&path) {
                    Some((path, owner.clone()))
                } else {
                    owned.insert(path, (key.clone(), version.clone()));
                    None
                }
            })
        {
            bail!(
                "build dependency file conflict at {} between {} and {}",
                path.display(),
                key,
                owner.0
            );
        }
        sage_archive::extract_package(&archive, &tree, &inspection.files)?;
        println!("{} dependency {} {}", tree_name, key, version);
    }
    Ok(Some(tree))
}

/// Info directory indexes are intentionally shared and regenerated by the
/// GNU documentation tools. They must not make otherwise independent build
/// dependency packages look like a payload collision.
fn is_shared_build_metadata(path: &Path) -> bool {
    path == Path::new("usr/share/info/dir")
}

async fn obtain_release_archive(
    engine: &sage_repo::DownloadEngine,
    cache: &Path,
    source: &ReleaseSource,
) -> Result<PathBuf> {
    match &source.location {
        ReleaseLocation::Local(path) => Ok(path.clone()),
        ReleaseLocation::Remote(base) => {
            let archive = cache.join(&source.release.sha256);
            let url = format!("{}/{}", base.trim_end_matches('/'), source.release.archive);
            engine
                .download_url(&url, &archive, &source.release.sha256)
                .await?;
            Ok(archive)
        }
    }
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
    let compiler = recipe
        .build
        .target
        .is_empty()
        .then_some(&config.cc)
        .or_else(|| config.targets.get(&recipe.build.target).map(|target| &target.cc))
        .unwrap_or(&config.cc);
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
        ("CC_FAMILY".into(), sage_build::tool_family(compiler)),
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
    stage_declarative_metadata(recipe_dir, &metadata, &area.name, &recipe.package.name)?;
    let subpackage = recipe
        .subpackages
        .iter()
        .find(|subpackage| subpackage.name == area.name);
    let package_channel = subpackage
        .and_then(|package| package.channel.as_deref())
        .unwrap_or(&recipe.package.channel);
    let package_slot = subpackage
        .and_then(|package| package.slot.as_deref())
        .unwrap_or(&recipe.package.slot);
    stage_alternatives(
        recipe,
        &metadata,
        &area.name,
        package_channel,
        package_slot,
    )?;
    stage_sysusers(
        recipe,
        &metadata,
        &area.name,
        package_channel,
        package_slot,
    )?;
    std::fs::write(
        metadata.join("files.idx"),
        sage_archive::format_file_index(&records),
    )?;
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
        slot: package_slot.into(),
        version: recipe.package.version.clone(),
        release: recipe.package.release,
        epoch: recipe.package.epoch,
        arch: recipe.package.arch.clone(),
        channel: package_channel.into(),
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
        conflicts: if area.name == recipe.package.name {
            recipe.package.conflicts.clone()
        } else {
            Vec::new()
        },
        features: features.iter().cloned().collect(),
        managed_build_tools: managed_build_tools.to_vec(),
    };
    std::fs::write(
        metadata.join("manifest.toml"),
        toml::to_string_pretty(&manifest)?,
    )?;
    let output = output_dir.join(format!(
        "{}-{}-{}-{}.pkg.tar.zst",
        area.name,
        recipe.package.version,
        recipe.package.release,
        recipe.package.arch
    ));
    if output.exists() {
        bail!(
            "package archive filename collision at {}; slot is metadata, so outputs with identical name/version/release/arch must use separate directories",
            output.display()
        );
    }
    sage_archive::create_package(area.path(), &output, 15)?;
    println!("Created {}", output.display());
    Ok(())
}

fn stage_alternatives(
    recipe: &sage_build::RecipeSpec,
    metadata: &Path,
    output_package: &str,
    package_channel: &str,
    package_slot: &str,
) -> Result<()> {
    let alternatives: Vec<_> = recipe
        .alternatives
        .iter()
        .filter(|alternative| {
            alternative.package == output_package
                || (alternative.package.is_empty() && output_package == recipe.package.name)
        })
        .map(|alternative| sage_sys::AlternativeDeclaration {
            link: alternative.link.clone(),
            target: alternative.target.clone(),
            priority: alternative.priority,
        })
        .collect();
    if alternatives.is_empty() {
        return Ok(());
    }
    let document = sage_sys::AlternativesDocument {
        schema_version: sage_core::SCHEMA_VERSION,
        package: sage_core::PackageKey::new(
            package_channel,
            output_package,
            package_slot,
        ),
        alternatives,
    };
    std::fs::write(
        metadata.join("alternatives.toml"),
        toml::to_string_pretty(&document)?,
    )?;
    Ok(())
}

pub fn stage_sysusers(
    recipe: &sage_build::RecipeSpec,
    metadata: &Path,
    output_package: &str,
    package_channel: &str,
    package_slot: &str,
) -> Result<()> {
    let accounts: Vec<_> = recipe
        .sysusers
        .iter()
        .filter(|account| {
            account.package == output_package
                || (account.package.is_empty() && output_package == recipe.package.name)
        })
        .map(|account| sage_sys::SysuserDeclaration {
            kind: account.kind.clone(),
            name: account.name.clone(),
            id: account.id,
            description: account.description.clone(),
            home: account.home.clone(),
            shell: account.shell.clone(),
        })
        .collect();
    if accounts.is_empty() {
        return Ok(());
    }
    let document = sage_sys::SysusersDocument {
        schema_version: sage_core::SCHEMA_VERSION,
        package: sage_core::PackageKey::new(
            package_channel,
            output_package,
            package_slot,
        ),
        accounts,
    };
    std::fs::write(
        metadata.join("sysusers.toml"),
        toml::to_string_pretty(&document)?,
    )?;
    Ok(())
}

pub fn stage_declarative_metadata(
    recipe_dir: &Path,
    metadata: &Path,
    output_package: &str,
    main_package: &str,
) -> Result<()> {
    let service = recipe_dir.join("service.toml");
    if service.exists() {
        let document =
            sage_sys::ServiceDocument::load(&service)?.for_package(output_package, main_package);
        if document.services().next().is_some() {
            std::fs::write(
                metadata.join("service.toml"),
                toml::to_string_pretty(&document)?,
            )?;
        }
    }
    let triggers = recipe_dir.join("triggers.toml");
    if output_package == main_package && triggers.exists() {
        sage_sys::TriggerSpec::load(&triggers)?;
        std::fs::copy(triggers, metadata.join("triggers.toml"))?;
    }
    Ok(())
}
