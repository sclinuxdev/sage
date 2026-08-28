/// Schema-v1 source input.
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SourceSpec {
    #[serde(default)]
    pub kind: SourceKind,
    pub url: String,
    #[serde(default)]
    pub sha256: String,
    /// Exact SHA-1 or SHA-256 object ID required for Git sources.
    #[serde(default)]
    pub commit: String,
    /// Recursively materialize submodules at superproject-pinned commits.
    #[serde(default)]
    pub submodules: bool,
    /// Leading archive components removed during extraction.
    #[serde(default)]
    pub strip_components: Option<u32>,
    /// Extraction directory relative to the shared source root.
    #[serde(default = "default_source_destination")]
    pub destination: PathBuf,
}

/// Fetch protocol selected by one source declaration.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum SourceKind {
    #[default]
    Archive,
    Git,
    File,
}

fn default_source_destination() -> PathBuf {
    PathBuf::from(".")
}

/// Shared and main-package metadata from a recipe.
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RecipePackage {
    pub name: String,
    #[serde(default = "default_slot")]
    pub slot: String,
    pub version: String,
    pub release: u32,
    #[serde(default)]
    pub epoch: u32,
    pub description: String,
    pub license: String,
    pub channel: String,
    pub arch: String,
    #[serde(default)]
    pub dependencies: Vec<String>,
    #[serde(default)]
    pub provides: Vec<String>,
}

fn default_slot() -> String {
    sage_core::DEFAULT_SLOT.into()
}

/// Ordered glob claims used to partition one DESTDIR.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PayloadSpec {
    #[serde(default)]
    pub files: Vec<String>,
    #[serde(default)]
    pub excludes: Vec<String>,
    #[serde(default)]
    pub default: String,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RecipeBuild {
    #[serde(default)]
    pub inherit: Vec<String>,
    #[serde(default)]
    pub args: BTreeMap<String, String>,
    /// Explicit packages needed only while building this recipe.
    #[serde(default)]
    pub dependencies: Vec<String>,
    /// Target-architecture headers and libraries mounted below `/sysroot`.
    #[serde(default)]
    pub target_dependencies: Vec<String>,
    #[serde(default)]
    pub payload: PayloadSpec,
    #[serde(default)]
    pub allow_network: bool,
    /// Extra private-library directories, relative to the installed channel root.
    #[serde(default)]
    pub private_library_dirs: Vec<PathBuf>,
    /// Optional target triple. An empty value means a native build.
    #[serde(default)]
    pub target: String,
}

/// One opt-in build feature. Rules are folded before dependency solving or
/// runner generation, keeping feature checks out of all execution hot paths.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FeatureSpec {
    #[serde(default)]
    pub default: bool,
    #[serde(default)]
    pub dependencies: Vec<String>,
    #[serde(default)]
    pub build_dependencies: Vec<String>,
    #[serde(default)]
    pub target_dependencies: Vec<String>,
    #[serde(default)]
    pub args: BTreeMap<String, String>,
    #[serde(default)]
    pub env: BTreeMap<String, String>,
}

/// Fully folded feature selection consumed by the solver and runner.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct EffectiveFeatures {
    pub enabled: BTreeSet<String>,
    pub dependencies: Vec<String>,
    pub build_dependencies: Vec<String>,
    pub target_dependencies: Vec<String>,
    pub args: BTreeMap<String, String>,
    pub env: BTreeMap<String, String>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SubpackageSpec {
    pub name: String,
    pub description: Option<String>,
    pub license: Option<String>,
    #[serde(default)]
    pub dependencies: Vec<String>,
    #[serde(default)]
    pub provides: Vec<String>,
    #[serde(default)]
    pub payload: PayloadSpec,
}

/// One declarative account staged as Sage-owned package metadata.
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SysuserSpec {
    /// Output package that owns this declaration; empty selects the main one.
    #[serde(default)]
    pub package: String,
    #[serde(rename = "type")]
    pub kind: String,
    pub name: String,
    pub id: Option<u32>,
    #[serde(default)]
    pub description: String,
    #[serde(default = "default_home")]
    pub home: String,
    #[serde(default = "default_shell")]
    pub shell: String,
}

/// One command-name provider managed transactionally by Sage.
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct AlternativeSpec {
    /// Output package that carries this declaration; empty selects the main one.
    #[serde(default)]
    pub package: String,
    pub link: PathBuf,
    pub target: PathBuf,
    pub priority: i32,
}

/// Declarative filesystem payload for source-free data and policy packages.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct InstallSpec {
    #[serde(default)]
    pub directories: Vec<InstallDirectory>,
    #[serde(default)]
    pub files: Vec<InstallFile>,
    #[serde(default)]
    pub symlinks: Vec<InstallSymlink>,
    /// Source-tree files copied after the sandbox build without shell hooks.
    #[serde(default)]
    pub copies: Vec<InstallCopy>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct InstallDirectory {
    pub path: PathBuf,
    #[serde(default = "default_directory_mode")]
    pub mode: u32,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct InstallFile {
    pub path: PathBuf,
    pub content: String,
    #[serde(default = "default_file_mode")]
    pub mode: u32,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct InstallSymlink {
    pub path: PathBuf,
    pub target: PathBuf,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct InstallCopy {
    /// Safe path below the extracted source root.
    pub source: PathBuf,
    /// Safe package path below DESTDIR.
    pub path: PathBuf,
    #[serde(default)]
    pub recursive: bool,
}

fn default_directory_mode() -> u32 {
    0o755
}

fn default_file_mode() -> u32 {
    0o644
}

fn default_home() -> String {
    "/".into()
}

fn default_shell() -> String {
    "/usr/bin/nologin".into()
}

/// Complete single-build, multiple-output recipe.
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RecipeSpec {
    pub schema_version: u32,
    pub package: RecipePackage,
    #[serde(default)]
    pub source: Option<SourceSpec>,
    #[serde(default)]
    pub sources: Vec<SourceSpec>,
    #[serde(default)]
    pub build: RecipeBuild,
    #[serde(default)]
    pub subpackages: Vec<SubpackageSpec>,
    #[serde(default)]
    pub sysusers: Vec<SysuserSpec>,
    #[serde(default)]
    pub alternatives: Vec<AlternativeSpec>,
    #[serde(default)]
    pub install: InstallSpec,
    #[serde(default)]
    pub features: BTreeMap<String, FeatureSpec>,
}

/// One source-build vertex and the package symbols it consumes and produces.
#[derive(Debug, Clone)]
pub struct BuildUnit {
    pub recipe: PathBuf,
    pub name: String,
    packages: BTreeSet<String>,
    produces: BTreeSet<String>,
    consumes: BTreeSet<String>,
}

impl BuildUnit {
    /// Folds default features and converts all dependency classes into graph edges.
    pub fn from_recipe(path: PathBuf, recipe: &RecipeSpec) -> Result<Self, BuildError> {
        let features = recipe.effective_features(&[], true)?;
        let mut packages = BTreeSet::from([recipe.package.name.clone()]);
        packages.extend(
            recipe
                .subpackages
                .iter()
                .map(|package| package.name.clone()),
        );
        let mut produces = packages.clone();
        produces.extend(recipe.package.provides.clone());
        for package in &recipe.subpackages {
            produces.extend(package.provides.clone());
        }
        let mut declarations = recipe.package.dependencies.clone();
        declarations.extend(recipe.build.dependencies.clone());
        declarations.extend(recipe.build.target_dependencies.clone());
        declarations.extend(features.dependencies);
        declarations.extend(features.build_dependencies);
        declarations.extend(features.target_dependencies);
        for package in &recipe.subpackages {
            declarations.extend(package.dependencies.clone());
        }
        let consumes = declarations
            .into_iter()
            .map(|value| {
                value
                    .parse::<sage_core::Dependency>()
                    .map(|dependency| dependency.name)
                    .map_err(|error| BuildError::InvalidSpec(error.to_string()))
            })
            .collect::<Result<_, _>>()?;
        Ok(Self {
            recipe: path,
            name: recipe.package.name.clone(),
            packages,
            produces,
            consumes,
        })
    }

    /// Adds rclass-provided dependency declarations to the topology graph.
    pub fn include_dependencies(
        &mut self,
        declarations: impl IntoIterator<Item = String>,
    ) -> Result<(), BuildError> {
        for value in declarations {
            let dependency: sage_core::Dependency =
                value.parse().map_err(|error: sage_core::CoreError| {
                    BuildError::InvalidSpec(error.to_string())
                })?;
            self.consumes.insert(dependency.name);
        }
        Ok(())
    }
}

/// Deterministic dependency layers for parallel source builds.
pub struct BuildGraph;

impl BuildGraph {
    pub fn discover(root: &Path) -> Result<Vec<BuildUnit>, BuildError> {
        let mut recipes: Vec<_> = walkdir::WalkDir::new(root)
            .follow_links(false)
            .into_iter()
            .filter_map(|entry| match entry {
                Ok(entry) if entry.file_type().is_file() && entry.file_name() == "recipe.toml" => {
                    Some(Ok(entry.into_path()))
                }
                Ok(_) => None,
                Err(error) => Some(Err(BuildError::Walk(error))),
            })
            .collect::<Result<_, _>>()?;
        recipes.sort();
        let mut latest = BTreeMap::new();
        for path in recipes {
            let recipe = RecipeSpec::load(&path)?;
            let key = (recipe.package.name.clone(), recipe.package.arch.clone());
            let version = sage_core::Version::new(
                recipe.package.epoch,
                recipe.package.version.clone(),
                recipe.package.release,
            );
            let replace = latest
                .get(&key)
                .is_none_or(|(selected, _, _)| version > *selected);
            if replace {
                latest.insert(key, (version, path, recipe));
            }
        }
        latest
            .into_values()
            .map(|(_, path, recipe)| BuildUnit::from_recipe(path, &recipe))
            .collect()
    }

    /// Applies Kahn's algorithm and returns maximal deterministic parallel layers.
    pub fn layers(units: Vec<BuildUnit>) -> Result<Vec<Vec<BuildUnit>>, BuildError> {
        let mut package_owners = BTreeMap::new();
        for unit in &units {
            for package in &unit.packages {
                if let Some(owner) = package_owners.insert(package, unit.name.as_str()) {
                    return Err(BuildError::InvalidSpec(format!(
                        "package '{package}' is produced by both {owner} and {}",
                        unit.name
                    )));
                }
            }
        }
        let mut producers: BTreeMap<String, Vec<usize>> = BTreeMap::new();
        for (index, unit) in units.iter().enumerate() {
            for symbol in &unit.produces {
                producers.entry(symbol.clone()).or_default().push(index);
            }
        }
        let mut outgoing = vec![BTreeSet::new(); units.len()];
        let mut indegree = vec![0usize; units.len()];
        for (consumer, unit) in units.iter().enumerate() {
            for symbol in &unit.consumes {
                for &producer in producers.get(symbol).into_iter().flatten() {
                    if producer != consumer && outgoing[producer].insert(consumer) {
                        indegree[consumer] += 1;
                    }
                }
            }
        }
        let mut ready: BTreeSet<_> = indegree
            .iter()
            .enumerate()
            .filter(|(_, degree)| **degree == 0)
            .map(|(index, _)| index)
            .collect();
        let mut layers = Vec::new();
        let mut completed = 0;
        while !ready.is_empty() {
            let current: Vec<_> = std::mem::take(&mut ready).into_iter().collect();
            let mut next = BTreeSet::new();
            for &producer in &current {
                completed += 1;
                for &consumer in &outgoing[producer] {
                    indegree[consumer] -= 1;
                    if indegree[consumer] == 0 {
                        next.insert(consumer);
                    }
                }
            }
            layers.push(
                current
                    .into_iter()
                    .map(|index| units[index].clone())
                    .collect(),
            );
            ready = next;
        }
        if completed != units.len() {
            let cycle = units
                .iter()
                .enumerate()
                .filter(|(index, _)| indegree[*index] != 0)
                .map(|(_, unit)| unit.name.as_str())
                .collect::<Vec<_>>()
                .join(", ");
            return Err(BuildError::InvalidSpec(format!(
                "source build graph contains a cycle: {cycle}"
            )));
        }
        Ok(layers)
    }
}

/// Explicit stage boundary used to break compiler/bootstrap dependency cycles.
#[derive(Debug, Clone, Deserialize)]
pub struct BootstrapPlan {
    pub schema_version: u32,
    pub stages: Vec<BootstrapStage>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct BootstrapStage {
    pub name: String,
    pub recipes: Vec<PathBuf>,
}

impl BootstrapPlan {
    pub fn load(path: impl AsRef<Path>) -> Result<Self, BuildError> {
        let plan: Self = toml::from_str(&fs::read_to_string(path)?)?;
        validate_schema(plan.schema_version)?;
        let mut names = BTreeSet::new();
        if plan.stages.is_empty()
            || plan.stages.iter().any(|stage| {
                stage.name.is_empty()
                    || stage.recipes.is_empty()
                    || !names.insert(stage.name.as_str())
            })
        {
            return Err(BuildError::InvalidSpec(
                "bootstrap stages require unique names and non-empty recipe lists".into(),
            ));
        }
        Ok(plan)
    }
}

impl RecipeSpec {
    pub fn load(path: impl AsRef<Path>) -> Result<Self, BuildError> {
        let path = path.as_ref();
        let recipe: Self = toml::from_str(&fs::read_to_string(path)?)?;
        validate_schema(recipe.schema_version)?;
        if !valid_package_name(&recipe.package.name) || recipe.package.arch.is_empty() {
            return Err(BuildError::InvalidSpec(
                "package name and architecture are required".into(),
            ));
        }
        sage_core::validate_spdx_expression(&recipe.package.license)
            .map_err(|error| BuildError::InvalidSpec(error.to_string()))?;
        if recipe.package.slot.is_empty()
            || !recipe.package.slot.bytes().all(|byte| {
                byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'+' | b'-')
            })
        {
            return Err(BuildError::InvalidSpec(
                "package slot must contain only ASCII letters, digits, '.', '_', '+', or '-'"
                    .into(),
            ));
        }
        if recipe.source.is_some() && !recipe.sources.is_empty() {
            return Err(BuildError::InvalidSpec(
                "use either [source] or [[sources]], not both".into(),
            ));
        }
        for source in recipe.source_inputs() {
            validate_source_destination(&source.destination)?;
            source.validate()?;
        }
        for user in &recipe.sysusers {
            user.validate()?;
        }
        for alternative in &recipe.alternatives {
            if (!alternative.package.is_empty() && !valid_package_name(&alternative.package))
                || alternative.link.as_os_str().is_empty()
                || alternative.link.is_absolute()
                || alternative.target.as_os_str().is_empty()
                || alternative.target.is_absolute()
                || alternative.link.components().any(|component| {
                    matches!(
                        component,
                        std::path::Component::ParentDir | std::path::Component::RootDir
                    )
                })
                || alternative.target.components().any(|component| {
                    matches!(
                        component,
                        std::path::Component::ParentDir | std::path::Component::RootDir
                    )
                })
            {
                return Err(BuildError::InvalidSpec(
                    "alternatives require safe relative package, link, and target values".into(),
                ));
            }
        }
        recipe.install.validate()?;
        if recipe.features.keys().any(|name| !valid_feature_name(name)) {
            return Err(BuildError::InvalidSpec(
                "feature names must use lowercase ASCII letters, digits, '_' or '-'".into(),
            ));
        }
        reject_lifecycle_scripts(path.parent().unwrap_or_else(|| Path::new(".")))?;
        let mut names = BTreeSet::from([recipe.package.name.as_str()]);
        if recipe.subpackages.iter().any(|subpackage| {
            !valid_package_name(&subpackage.name) || !names.insert(&subpackage.name)
        }) {
            return Err(BuildError::InvalidSpec(
                "subpackage names must be unique".into(),
            ));
        }
        if recipe
            .sysusers
            .iter()
            .any(|account| !account.package.is_empty() && !names.contains(account.package.as_str()))
        {
            return Err(BuildError::InvalidSpec(
                "sysuser owner must name an output package".into(),
            ));
        }
        if recipe.alternatives.iter().any(|alternative| {
            !alternative.package.is_empty() && !names.contains(alternative.package.as_str())
        }) {
            return Err(BuildError::InvalidSpec(
                "alternative owner must name an output package".into(),
            ));
        }
        for subpackage in &recipe.subpackages {
            if let Some(license) = &subpackage.license {
                sage_core::validate_spdx_expression(license)
                    .map_err(|error| BuildError::InvalidSpec(error.to_string()))?;
            }
        }
        Ok(recipe)
    }

    /// Iterates the legacy singleton or the schema-v1 multi-source array.
    pub fn source_inputs(&self) -> impl Iterator<Item = &SourceSpec> {
        self.source.iter().chain(self.sources.iter())
    }

    /// Produces the ordered materialization plan consumed by every build rclass.
    pub fn source_manifest(&self) -> String {
        let mut manifest = String::new();
        for (index, source) in self.source_inputs().enumerate() {
            let strip = if matches!(source.kind, SourceKind::Git | SourceKind::File) {
                0
            } else {
                source
                    .strip_components
                    .unwrap_or(if index == 0 { 1 } else { 0 })
            };
            let kind = match source.kind {
                SourceKind::Archive => "archive",
                SourceKind::Git => "tree",
                SourceKind::File => "file",
            };
            manifest.push_str(&format!(
                "{}\t{kind}\t{strip}\t{}\n",
                source_archive_name(index),
                source.destination.display()
            ));
        }
        manifest
    }

    /// Returns whether the package is installed outside the system channel root.
    pub fn uses_private_channel(&self) -> bool {
        self.package.channel != "system" && !self.package.channel.ends_with("/system")
    }

    /// Validates and folds a feature selection in bytewise name order.
    pub fn effective_features(
        &self,
        requested: &[String],
        use_defaults: bool,
    ) -> Result<EffectiveFeatures, BuildError> {
        let mut enabled: BTreeSet<_> = requested.iter().cloned().collect();
        if use_defaults {
            enabled.extend(
                self.features
                    .iter()
                    .filter(|(_, rule)| rule.default)
                    .map(|(name, _)| name.clone()),
            );
        }
        if let Some(name) = enabled
            .iter()
            .find(|name| !self.features.contains_key(*name))
        {
            return Err(BuildError::InvalidSpec(format!("unknown feature '{name}'")));
        }
        let mut result = EffectiveFeatures {
            enabled,
            ..EffectiveFeatures::default()
        };
        for name in &result.enabled {
            let rule = &self.features[name];
            result.dependencies.extend(rule.dependencies.clone());
            result
                .build_dependencies
                .extend(rule.build_dependencies.clone());
            result
                .target_dependencies
                .extend(rule.target_dependencies.clone());
            merge_build_arguments(&mut result.args, &rule.args);
            result.env.extend(rule.env.clone());
        }
        result.dependencies.sort();
        result.dependencies.dedup();
        result.build_dependencies.sort();
        result.build_dependencies.dedup();
        result.target_dependencies.sort();
        result.target_dependencies.dedup();
        Ok(result)
    }
}

impl InstallSpec {
    fn validate(&self) -> Result<(), BuildError> {
        let mut paths = BTreeSet::new();
        for (path, mode) in self
            .directories
            .iter()
            .map(|entry| (&entry.path, entry.mode))
            .chain(self.files.iter().map(|entry| (&entry.path, entry.mode)))
        {
            validate_install_path(path)?;
            if mode > 0o7777 || !paths.insert(path) {
                return Err(BuildError::InvalidSpec(format!(
                    "duplicate path or invalid mode in declarative install: {}",
                    path.display()
                )));
            }
        }
        for entry in &self.symlinks {
            validate_install_path(&entry.path)?;
            if entry.target.as_os_str().is_empty()
                || entry.target.to_string_lossy().contains(['\n', '\r', '\0'])
                || !paths.insert(&entry.path)
            {
                return Err(BuildError::InvalidSpec(format!(
                    "invalid declarative symlink {}",
                    entry.path.display()
                )));
            }
        }
        for entry in &self.copies {
            validate_install_path(&entry.source)?;
            validate_install_path(&entry.path)?;
            if !paths.insert(&entry.path) {
                return Err(BuildError::InvalidSpec(format!(
                    "duplicate declarative copy destination {}",
                    entry.path.display()
                )));
            }
        }
        Ok(())
    }
}

fn validate_install_path(path: &Path) -> Result<(), BuildError> {
    if path.as_os_str().is_empty()
        || path.is_absolute()
        || path.components().any(|component| {
            matches!(
                component,
                std::path::Component::ParentDir | std::path::Component::RootDir
            )
        })
    {
        return Err(BuildError::InvalidSpec(format!(
            "declarative install path must be safe and relative: {}",
            path.display()
        )));
    }
    Ok(())
}

/// Materializes a validated declarative payload into DESTDIR without invoking
/// a shell or allowing a recipe path to escape the staging root.
pub fn stage_declarative_install(
    root: &Path,
    source_root: &Path,
    recipe: &RecipeSpec,
) -> Result<(), BuildError> {
    recipe.install.validate()?;
    for entry in &recipe.install.directories {
        let path = root.join(&entry.path);
        fs::create_dir_all(&path)?;
        fs::set_permissions(path, fs::Permissions::from_mode(entry.mode))?;
    }
    for entry in &recipe.install.files {
        let path = root.join(&entry.path);
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }
        fs::write(&path, entry.content.as_bytes())?;
        fs::set_permissions(path, fs::Permissions::from_mode(entry.mode))?;
    }
    for entry in &recipe.install.symlinks {
        let path = root.join(&entry.path);
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }
        match fs::symlink_metadata(&path) {
            Ok(_) => fs::remove_file(&path)?,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error) => return Err(error.into()),
        }
        std::os::unix::fs::symlink(&entry.target, path)?;
    }
    for entry in &recipe.install.copies {
        let source = source_root.join(&entry.source);
        let metadata = fs::symlink_metadata(&source).map_err(|error| {
            BuildError::InvalidSpec(format!(
                "declarative copy source {} is unavailable: {error}",
                entry.source.display()
            ))
        })?;
        if metadata.is_dir() {
            if !entry.recursive {
                return Err(BuildError::InvalidSpec(format!(
                    "declarative directory copy {} requires recursive = true",
                    entry.source.display()
                )));
            }
            for child in walkdir::WalkDir::new(&source).follow_links(false) {
                let child = child?;
                let relative = child.path().strip_prefix(&source).map_err(|_| {
                    BuildError::InvalidSpec("declarative copy escaped its source root".into())
                })?;
                copy_install_entry(child.path(), &root.join(&entry.path).join(relative))?;
            }
        } else {
            if entry.recursive {
                return Err(BuildError::InvalidSpec(format!(
                    "recursive declarative copy source {} is not a directory",
                    entry.source.display()
                )));
            }
            copy_install_entry(&source, &root.join(&entry.path))?;
        }
    }
    Ok(())
}

fn copy_install_entry(source: &Path, target: &Path) -> Result<(), BuildError> {
    let metadata = fs::symlink_metadata(source)?;
    if metadata.is_dir() {
        fs::create_dir_all(target)?;
        fs::set_permissions(target, metadata.permissions())?;
    } else {
        if let Some(parent) = target.parent() {
            fs::create_dir_all(parent)?;
        }
        if metadata.file_type().is_symlink() {
            let link = fs::read_link(source)?;
            if link.is_absolute()
                || link.components().any(|component| {
                    matches!(component, std::path::Component::ParentDir)
                })
            {
                return Err(BuildError::InvalidSpec(format!(
                    "unsafe symlink in declarative copy: {} -> {}",
                    source.display(),
                    link.display()
                )));
            }
            std::os::unix::fs::symlink(link, target)?;
        } else if metadata.is_file() {
            fs::copy(source, target)?;
            fs::set_permissions(target, metadata.permissions())?;
        } else {
            return Err(BuildError::InvalidSpec(format!(
                "unsupported declarative copy input {}",
                source.display()
            )));
        }
    }
    Ok(())
}

/// Folds rclass-style free-form argument channels without allowing one feature
/// to erase arguments selected by the recipe or an earlier feature. Keys ending
/// in `_args` are ordered command fragments and therefore concatenate; scalar
/// keys retain ordinary last-writer-wins semantics.
pub fn merge_build_arguments(
    target: &mut BTreeMap<String, String>,
    additions: &BTreeMap<String, String>,
) {
    for (key, value) in additions {
        if key.ends_with("_args") && !value.is_empty() {
            target
                .entry(key.clone())
                .and_modify(|current| {
                    if !current.is_empty() {
                        current.push(' ');
                    }
                    current.push_str(value);
                })
                .or_insert_with(|| value.clone());
        } else {
            target.insert(key.clone(), value.clone());
        }
    }
}

impl SourceSpec {
    fn validate(&self) -> Result<(), BuildError> {
        match self.kind {
            SourceKind::Archive => {
                if self.sha256.len() != 64
                    || !self
                        .sha256
                        .bytes()
                        .all(|byte| byte.is_ascii_digit() || matches!(byte, b'a'..=b'f'))
                    || !self.commit.is_empty()
                    || self.submodules
                {
                    return Err(BuildError::InvalidSpec(
                        "archive sources require one SHA-256 and no Git fields".into(),
                    ));
                }
            }
            SourceKind::Git => {
                let exact_commit = matches!(self.commit.len(), 40 | 64)
                    && self.commit.bytes().all(|byte| byte.is_ascii_hexdigit());
                let network_url = self.url.starts_with("https://")
                    || self.url.starts_with("http://")
                    || self.url.starts_with("ssh://")
                    || self.url.starts_with("git://")
                    || (self.url.starts_with("git@") && self.url.contains(':'));
                if !exact_commit
                    || !network_url
                    || !self.sha256.is_empty()
                    || self.strip_components.is_some()
                {
                    return Err(BuildError::InvalidSpec(
                        "Git sources require a full commit ID and a network URL".into(),
                    ));
                }
            }
            SourceKind::File => {
                if self.sha256.len() != 64
                    || !self
                        .sha256
                        .bytes()
                        .all(|byte| byte.is_ascii_digit() || matches!(byte, b'a'..=b'f'))
                    || !self.commit.is_empty()
                    || self.submodules
                    || self.strip_components.is_some()
                    || self.destination == Path::new(".")
                {
                    return Err(BuildError::InvalidSpec(
                        "file sources require one SHA-256, a destination, and no extraction fields"
                            .into(),
                    ));
                }
            }
        }
        Ok(())
    }
}

/// Fetches an exact Git object and exports its worktree without VCS metadata.
pub fn fetch_git_source(
    git: &Path,
    source: &SourceSpec,
    checkout: &Path,
    destination: &Path,
) -> Result<(), BuildError> {
    source.validate()?;
    if source.kind != SourceKind::Git {
        return Err(BuildError::InvalidSpec(
            "fetch_git_source requires kind='git'".into(),
        ));
    }
    run_git(git, checkout, "init", ["init", "--quiet"])?;
    run_git(
        git,
        checkout,
        "remote add",
        ["remote", "add", "origin", source.url.as_str()],
    )?;
    run_git(
        git,
        checkout,
        "fetch commit",
        ["fetch", "--quiet", "--depth=1", "origin", &source.commit],
    )?;
    run_git(
        git,
        checkout,
        "checkout commit",
        ["checkout", "--quiet", "--detach", "FETCH_HEAD"],
    )?;
    if source.submodules {
        run_git(
            git,
            checkout,
            "submodule checkout",
            [
                "submodule",
                "update",
                "--quiet",
                "--init",
                "--recursive",
                "--depth=1",
            ],
        )?;
    }
    export_git_tree(checkout, destination)
}

fn run_git<const N: usize>(
    git: &Path,
    checkout: &Path,
    operation: &str,
    arguments: [&str; N],
) -> Result<(), BuildError> {
    fs::create_dir_all(checkout)?;
    let status = Command::new(git)
        .args(["-c", "protocol.file.allow=never", "-C"])
        .arg(checkout)
        .args(arguments)
        .env_clear()
        .env("PATH", "/usr/bin:/bin")
        .env("GIT_CONFIG_NOSYSTEM", "1")
        .env("HOME", checkout)
        .status()?;
    if status.success() {
        Ok(())
    } else {
        Err(BuildError::GitFailed {
            operation: operation.into(),
            status,
        })
    }
}

fn export_git_tree(checkout: &Path, destination: &Path) -> Result<(), BuildError> {
    fs::create_dir_all(destination)?;
    for entry in walkdir::WalkDir::new(checkout).follow_links(false) {
        let entry = entry?;
        let relative = entry
            .path()
            .strip_prefix(checkout)
            .expect("Git walk remains below checkout");
        if relative.as_os_str().is_empty()
            || relative.components().any(|part| part.as_os_str() == ".git")
        {
            continue;
        }
        let first = relative.components().next().map(|part| part.as_os_str());
        if matches!(first, Some(name) if name == ".distfiles" || name == ".patches") {
            return Err(BuildError::InvalidSpec(
                "Git source uses a reserved build-input path".into(),
            ));
        }
        let target = destination.join(relative);
        if entry.file_type().is_dir() {
            fs::create_dir_all(target)?;
        } else if entry.file_type().is_symlink() {
            if let Some(parent) = target.parent() {
                fs::create_dir_all(parent)?;
            }
            std::os::unix::fs::symlink(fs::read_link(entry.path())?, target)?;
        } else if entry.file_type().is_file() {
            if let Some(parent) = target.parent() {
                fs::create_dir_all(parent)?;
            }
            fs::copy(entry.path(), &target)?;
            fs::set_permissions(&target, fs::metadata(entry.path())?.permissions())?;
        }
    }
    Ok(())
}

fn valid_feature_name(name: &str) -> bool {
    !name.is_empty()
        && name.bytes().all(|byte| {
            byte.is_ascii_lowercase() || byte.is_ascii_digit() || matches!(byte, b'_' | b'-')
        })
}

/// Stable sandbox filename for an independently verified source archive.
pub fn source_archive_name(index: usize) -> String {
    format!("{index:03}-source")
}

fn validate_source_destination(path: &Path) -> Result<(), BuildError> {
    let valid = !path.as_os_str().is_empty()
        && path.components().all(|component| {
            matches!(
                component,
                std::path::Component::CurDir | std::path::Component::Normal(_)
            )
        })
        && !path.starts_with(".distfiles")
        && !path.starts_with(".patches")
        && !path
            .as_os_str()
            .as_encoded_bytes()
            .iter()
            .any(|byte| matches!(byte, b'\t' | b'\n' | b'\r' | b'\0'));
    if valid {
        Ok(())
    } else {
        Err(BuildError::InvalidSpec(format!(
            "source destination must stay below the source root: {}",
            path.display()
        )))
    }
}

impl SysuserSpec {
    fn validate(&self) -> Result<(), BuildError> {
        if (!self.package.is_empty() && !valid_package_name(&self.package))
            || !matches!(self.kind.as_str(), "user" | "group")
            || self.name.is_empty()
            || !self
                .name
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-'))
            || self.description.contains(['\n', '\r', '\0', '"', ':'])
            || !Path::new(&self.home).is_absolute()
            || !Path::new(&self.shell).is_absolute()
            || [&self.home, &self.shell].iter().any(|value| {
                value
                    .bytes()
                    .any(|byte| byte.is_ascii_whitespace() || matches!(byte, b'"' | b'\\' | b':'))
            })
        {
            return Err(BuildError::InvalidSpec(format!(
                "invalid sysusers declaration for '{}'",
                self.name
            )));
        }
        Ok(())
    }
}

fn valid_package_name(name: &str) -> bool {
    !name.is_empty()
        && name
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'+' | b'-'))
}

fn reject_lifecycle_scripts(recipe_dir: &Path) -> Result<(), BuildError> {
    const FORBIDDEN: &[&str] = &["preinst", "postinst", "prerm", "postrm"];
    if let Some(name) = FORBIDDEN.iter().find(|name| recipe_dir.join(name).exists()) {
        return Err(BuildError::InvalidSpec(format!(
            "interactive lifecycle script '{name}' is not supported; use declarative metadata"
        )));
    }
    Ok(())
}
