//! Declarative rclass execution, Bubblewrap isolation, payload carving, and ELF scans.

use serde::Deserialize;
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitStatus};
use thiserror::Error;

const PHASE_ORDER: &[&str] = &[
    "src_unpack",
    "src_prepare",
    "src_configure",
    "src_compile",
    "src_test",
    "src_install",
];

/// Build description or execution failures.
#[derive(Debug, Error)]
pub enum BuildError {
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
    #[error("TOML error: {0}")]
    Toml(#[from] toml::de::Error),
    #[error("unsupported schema version {0}")]
    Schema(u32),
    #[error("unknown template variable '{0}'")]
    UnknownVariable(String),
    #[error("invalid template expression at byte {0}")]
    InvalidTemplate(usize),
    #[error("sandbox process exited with {0}")]
    SandboxFailed(ExitStatus),
    #[error("invalid build specification: {0}")]
    InvalidSpec(String),
    #[error("invalid glob pattern: {0}")]
    Glob(#[from] glob::PatternError),
    #[error("filesystem traversal failed: {0}")]
    Walk(#[from] walkdir::Error),
    #[error("ELF parse failed: {0}")]
    Elf(#[from] goblin::error::Error),
    #[error("tool '{tool}' is not allowed by inherited rclasses")]
    UnauthorizedTool { tool: String },
    #[error("patchelf failed for {path}: {message}")]
    Patchelf { path: PathBuf, message: String },
    #[error("git operation '{operation}' exited with {status}")]
    GitFailed {
        operation: String,
        status: ExitStatus,
    },
}

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
        recipes
            .into_iter()
            .map(|path| {
                let recipe = RecipeSpec::load(&path)?;
                BuildUnit::from_recipe(path, &recipe)
            })
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
pub fn stage_declarative_install(root: &Path, recipe: &RecipeSpec) -> Result<(), BuildError> {
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

/// Global reproducible-build and sandbox policy.
#[derive(Debug, Clone, Deserialize)]
pub struct BuildConfig {
    pub schema_version: u32,
    pub fakeroot: PathBuf,
    pub bwrap: PathBuf,
    #[serde(default = "default_git")]
    pub git: PathBuf,
    pub sysroot: PathBuf,
    pub cc: String,
    pub cxx: String,
    pub linker: String,
    pub rustc: String,
    #[serde(default = "default_patchelf")]
    pub patchelf: PathBuf,
    #[serde(default)]
    pub cflags: String,
    #[serde(default)]
    pub cxxflags: String,
    #[serde(default)]
    pub cppflags: String,
    #[serde(default)]
    pub ldflags: String,
    #[serde(default)]
    pub rustflags: String,
    pub source_date_epoch: u64,
    #[serde(default)]
    pub jobs: usize,
    #[serde(default)]
    pub memory_limit: String,
    #[serde(default = "default_pids")]
    pub pids_limit: u32,
    #[serde(default)]
    pub compiler_cache: String,
    #[serde(default)]
    pub ccache_dir: PathBuf,
    /// Data-owned tools needed to enter every build sandbox. They are solved
    /// into the ephemeral toolchain and never registered as host state.
    #[serde(default)]
    pub sandbox_dependencies: Vec<String>,
    /// Native machine triple used as the GNU build/host default.
    #[serde(default)]
    pub build: String,
    /// Entirely data-driven cross toolchains keyed by target triple.
    #[serde(default)]
    pub targets: BTreeMap<String, CrossTarget>,
}

/// Commands and platform facts for one configured cross-compilation target.
#[derive(Debug, Clone, Deserialize)]
pub struct CrossTarget {
    pub cc: String,
    pub cxx: String,
    pub ar: String,
    pub strip: String,
    pub arch: String,
    pub goos: String,
    pub goarch: String,
    pub cmake_system_name: String,
    #[serde(default = "default_endian")]
    pub endian: String,
    #[serde(default)]
    pub rustflags: String,
}

fn default_endian() -> String {
    "little".into()
}

fn default_pids() -> u32 {
    2048
}

fn default_git() -> PathBuf {
    PathBuf::from("git")
}

fn default_patchelf() -> PathBuf {
    PathBuf::from("patchelf")
}

impl BuildConfig {
    pub fn load(path: impl AsRef<Path>) -> Result<Self, BuildError> {
        let mut config: Self = toml::from_str(&fs::read_to_string(path)?)?;
        validate_schema(config.schema_version)?;
        if config.jobs == 0 {
            config.jobs = std::thread::available_parallelism().map_or(1, usize::from);
        }
        if config.cxxflags.is_empty() {
            config.cxxflags.clone_from(&config.cflags);
        }
        Ok(config)
    }

    /// Selects a configured target and rejects unsafe triple spelling.
    pub fn cross_target(&self, triple: &str) -> Result<Option<&CrossTarget>, BuildError> {
        if triple.is_empty() {
            return Ok(None);
        }
        if self.build.is_empty()
            || !triple.bytes().all(|byte| {
                byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'+' | b'.' | b'-')
            })
        {
            return Err(BuildError::InvalidSpec(
                "cross builds require a safe target triple and configured build triple".into(),
            ));
        }
        self.targets.get(triple).map(Some).ok_or_else(|| {
            BuildError::InvalidSpec(format!("cross target '{triple}' is not configured"))
        })
    }
}

/// Declarative build class. No tool-specific behavior exists in Rust.
#[derive(Debug, Clone, Deserialize)]
pub struct Rclass {
    pub schema_version: u32,
    pub name: String,
    pub description: String,
    #[serde(default)]
    pub implicit_build_dependencies: Vec<String>,
    #[serde(default)]
    pub allowed_compilers: Vec<String>,
    #[serde(default)]
    pub allowed_linkers: Vec<String>,
    /// Data-owned defaults overridden by later rclasses and recipe arguments.
    #[serde(default)]
    pub defaults: BTreeMap<String, String>,
    #[serde(default)]
    pub env: BTreeMap<String, String>,
    #[serde(default)]
    pub phases: BTreeMap<String, String>,
}

impl Rclass {
    pub fn load(path: impl AsRef<Path>) -> Result<Self, BuildError> {
        let class: Self = toml::from_str(&fs::read_to_string(path)?)?;
        validate_schema(class.schema_version)?;
        Ok(class)
    }
}

/// Verifies the selected compiler and linker families against rclass policy.
pub fn validate_toolchain(classes: &[Rclass], config: &BuildConfig) -> Result<(), BuildError> {
    let compilers: BTreeSet<_> = classes
        .iter()
        .flat_map(|class| class.allowed_compilers.iter().map(|name| name.as_str()))
        .collect();
    let linkers: BTreeSet<_> = classes
        .iter()
        .flat_map(|class| class.allowed_linkers.iter().map(|name| name.as_str()))
        .collect();
    if !compilers.is_empty() {
        for tool in [&config.cc, &config.cxx] {
            let family = tool_family(tool);
            if !compilers.contains(family.as_str()) {
                return Err(BuildError::UnauthorizedTool { tool: tool.clone() });
            }
        }
    }
    if !linkers.is_empty() && !linkers.contains(tool_family(&config.linker).as_str()) {
        return Err(BuildError::UnauthorizedTool {
            tool: config.linker.clone(),
        });
    }
    Ok(())
}

fn tool_family(tool: &str) -> String {
    let name = Path::new(tool)
        .file_name()
        .and_then(|name| name.to_str())
        .unwrap_or(tool);
    if name.contains("clang") {
        "clang".into()
    } else if name.contains("gcc") || name == "g++" {
        "gcc".into()
    } else if name.contains("lld") {
        "lld".into()
    } else if name.contains("mold") {
        "mold".into()
    } else if name == "ld" || name.ends_with("-ld") {
        "ld".into()
    } else {
        name.into()
    }
}

fn validate_schema(version: u32) -> Result<(), BuildError> {
    if version == sage_core::SCHEMA_VERSION {
        Ok(())
    } else {
        Err(BuildError::Schema(version))
    }
}

/// Merges inherited classes and emits one fail-fast shell program.
pub fn compose_runner(
    classes: &[Rclass],
    variables: &BTreeMap<String, String>,
) -> Result<String, BuildError> {
    let mut env = BTreeMap::new();
    let mut phases = BTreeMap::new();
    let mut expanded_variables = BTreeMap::new();
    for class in classes {
        expanded_variables.extend(class.defaults.clone());
        env.extend(class.env.clone());
        phases.extend(class.phases.clone());
    }
    expanded_variables.extend(variables.clone());
    let mut script = String::from(
        "#!/bin/bash\nset -euo pipefail\ntrap 'printf >&2 \"sage-build: line %s failed\\n\" \"$LINENO\"' ERR\n",
    );
    for (name, value) in env {
        validate_shell_name(&name)?;
        script.push_str("export ");
        script.push_str(&name);
        script.push('=');
        script.push_str(&shell_quote(&expand(&value, &expanded_variables)?));
        script.push('\n');
    }
    for phase in PHASE_ORDER.iter().copied().chain(
        phases
            .keys()
            .map(String::as_str)
            .filter(|name| !PHASE_ORDER.contains(name)),
    ) {
        let Some(body) = phases.get(phase) else {
            continue;
        };
        validate_shell_name(phase)?;
        script.push_str("\n# rclass phase: ");
        script.push_str(phase);
        script.push('\n');
        script.push_str(&expand(body, &expanded_variables)?);
        if !body.ends_with('\n') {
            script.push('\n');
        }
    }
    Ok(script)
}

/// Returns the unique package constraints required to construct `/toolchain`.
pub fn build_dependencies(
    recipe: &RecipeSpec,
    classes: &[Rclass],
    features: &EffectiveFeatures,
) -> Result<Vec<sage_core::Dependency>, BuildError> {
    let mut values = recipe.build.dependencies.clone();
    values.extend(features.build_dependencies.clone());
    values.extend(
        classes
            .iter()
            .flat_map(|class| class.implicit_build_dependencies.clone()),
    );
    values.sort();
    values.dedup();
    values
        .into_iter()
        .map(|value| {
            value
                .parse()
                .map_err(|error: sage_core::CoreError| BuildError::InvalidSpec(error.to_string()))
        })
        .collect()
}

/// Returns target-architecture package constraints for the cross sysroot.
pub fn target_dependencies(
    recipe: &RecipeSpec,
    features: &EffectiveFeatures,
) -> Result<Vec<sage_core::Dependency>, BuildError> {
    let mut values = recipe.build.target_dependencies.clone();
    values.extend(features.target_dependencies.clone());
    values.sort();
    values.dedup();
    values
        .into_iter()
        .map(|value| {
            value
                .parse()
                .map_err(|error: sage_core::CoreError| BuildError::InvalidSpec(error.to_string()))
        })
        .collect()
}

fn validate_shell_name(value: &str) -> Result<(), BuildError> {
    if !value.is_empty()
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_')
        && !value.as_bytes()[0].is_ascii_digit()
    {
        Ok(())
    } else {
        Err(BuildError::InvalidSpec(format!(
            "invalid shell name '{value}'"
        )))
    }
}

/// Expands `${name}` tokens without invoking a shell or accepting partial syntax.
pub fn expand(template: &str, variables: &BTreeMap<String, String>) -> Result<String, BuildError> {
    let mut result = String::with_capacity(template.len());
    let mut remaining = template;
    while let Some(start) = remaining.find("${") {
        result.push_str(&remaining[..start]);
        let tail = &remaining[start + 2..];
        let end = tail.find('}').ok_or(BuildError::InvalidTemplate(
            template.len() - remaining.len() + start,
        ))?;
        let name = &tail[..end];
        let value = variables
            .get(name)
            .ok_or_else(|| BuildError::UnknownVariable(name.into()))?;
        result.push_str(value);
        remaining = &tail[end + 1..];
    }
    result.push_str(remaining);
    Ok(result)
}

fn shell_quote(value: &str) -> String {
    format!("'{}'", value.replace('\'', "'\\''"))
}

/// Host paths mounted read-write into the otherwise immutable sandbox.
#[derive(Debug, Clone)]
pub struct SandboxPaths {
    pub source: PathBuf,
    pub build: PathBuf,
    pub destdir: PathBuf,
    pub runner: PathBuf,
    pub toolchain: Option<PathBuf>,
    pub target_sysroot: Option<PathBuf>,
}

/// Assembles and executes one hermetic Bubblewrap process.
pub struct SandboxRunner<'a> {
    config: &'a BuildConfig,
}

impl<'a> SandboxRunner<'a> {
    pub fn new(config: &'a BuildConfig) -> Self {
        Self { config }
    }

    /// Returns the exact command so callers can audit or print dry runs.
    pub fn command(&self, paths: &SandboxPaths, allow_network: bool) -> Command {
        let mut command = Command::new(&self.config.bwrap);
        command.args(["--die-with-parent", "--new-session", "--unshare-all"]);
        if allow_network {
            command.arg("--share-net");
        }
        command
            .args(["--ro-bind"])
            .arg(&self.config.sysroot)
            .arg("/");
        command.args(["--bind"]).arg(&paths.source).arg("/source");
        // Overlay immutable inputs after the writable source bind. An archive may
        // populate the source tree, but it cannot replace later distfiles, the
        // extraction manifest, or patches before their declared turn.
        command
            .args(["--ro-bind"])
            .arg(paths.source.join(".distfiles"))
            .arg("/source/.distfiles");
        if paths.source.join(".patches").exists() {
            command
                .args(["--ro-bind"])
                .arg(paths.source.join(".patches"))
                .arg("/source/.patches");
        }
        command.args(["--bind"]).arg(&paths.build).arg("/build");
        command.args(["--bind"]).arg(&paths.destdir).arg("/dest");
        command
            .args(["--ro-bind"])
            .arg(&paths.runner)
            .arg("/run/sage-build-runner.sh");
        command.args([
            "--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp", "--chdir", "/build",
        ]);
        if let Some(toolchain) = &paths.toolchain {
            command.args(["--ro-bind"]).arg(toolchain).arg("/toolchain");
            command
                .args(["--ro-bind"])
                .arg(toolchain.join("usr"))
                .arg("/usr");
        }
        if let Some(target_sysroot) = &paths.target_sysroot {
            command
                .args(["--ro-bind"])
                .arg(target_sysroot)
                .arg("/sysroot");
        }
        command.arg("--clearenv");
        for (name, value) in self.environment(paths) {
            command.args(["--setenv", &name, &value]);
        }
        command.arg("--").arg(&self.config.fakeroot).args([
            "--",
            "/bin/bash",
            "/run/sage-build-runner.sh",
        ]);
        command
    }

    pub fn run(
        &self,
        paths: &SandboxPaths,
        allow_network: bool,
    ) -> Result<Vec<sage_archive::ManagedBuildTool>, BuildError> {
        self.prepare_tool_wrappers(paths)?;
        let status = self.command(paths, allow_network).status()?;
        if !status.success() {
            return Err(BuildError::SandboxFailed(status));
        }
        self.read_build_tools(paths)
    }

    /// Creates narrow observation wrappers for configured compilers and the
    /// selected linker. A role is attested only when its wrapper is executed;
    /// merely configuring a tool never adds it to the package manifest.
    fn prepare_tool_wrappers(&self, paths: &SandboxPaths) -> Result<(), BuildError> {
        let directory = paths.build.join(".sage-tools");
        fs::create_dir(&directory)?;
        fs::write(paths.build.join(".sage-tool-usage"), [])?;
        self.write_tool_wrapper(&directory.join("cc"), "cc", &self.config.cc, true, false)?;
        self.write_tool_wrapper(&directory.join("cxx"), "cxx", &self.config.cxx, true, false)?;
        self.write_tool_wrapper(
            &directory.join("ld"),
            "linker",
            &self.config.linker,
            false,
            false,
        )?;
        self.write_tool_wrapper(
            &directory.join("rustc"),
            "rustc",
            &self.config.rustc,
            false,
            true,
        )?;
        Ok(())
    }

    fn write_tool_wrapper(
        &self,
        path: &Path,
        role: &str,
        tool: &str,
        compiler_driver: bool,
        rustc: bool,
    ) -> Result<(), BuildError> {
        if tool.contains(['\n', '\r', '\0']) || role.contains(['\n', '\r', '\0']) {
            return Err(BuildError::InvalidSpec(
                "tool names may not contain control characters".into(),
            ));
        }
        let search_path = "/usr/bin:/bin";
        let mut script = format!(
            "#!/bin/bash\nset -euo pipefail\nresolved=$(PATH={} command -v -- {})\nprintf '%s\\t%s\\n' {} \"$resolved\" >> /build/.sage-tool-usage\n",
            shell_quote(search_path),
            shell_quote(tool),
            shell_quote(role),
        );
        if compiler_driver {
            script.push_str("exec \"$resolved\" -B/build/.sage-tools \"$@\"\n");
        } else if rustc {
            script.push_str("exec \"$resolved\" -C linker=/build/.sage-tools/ld \"$@\"\n");
        } else {
            script.push_str("exec \"$resolved\" \"$@\"\n");
        }
        fs::write(path, script)?;
        fs::set_permissions(path, fs::Permissions::from_mode(0o755))?;
        Ok(())
    }

    fn read_build_tools(
        &self,
        paths: &SandboxPaths,
    ) -> Result<Vec<sage_archive::ManagedBuildTool>, BuildError> {
        let log = fs::read_to_string(paths.build.join(".sage-tool-usage"))?;
        let mut observed = BTreeMap::new();
        for line in log.lines() {
            let Some((role, executable)) = line.split_once('\t') else {
                return Err(BuildError::InvalidSpec(
                    "malformed managed build-tool observation".into(),
                ));
            };
            if !matches!(role, "cc" | "cxx" | "linker" | "rustc") || executable.is_empty() {
                return Err(BuildError::InvalidSpec(
                    "unknown managed build-tool observation".into(),
                ));
            }
            observed
                .entry(role.to_string())
                .or_insert_with(|| executable.to_string());
        }
        observed
            .into_iter()
            .map(|(role, executable)| {
                let host_path = self.host_tool_path(paths, &executable)?;
                let output = Command::new(&host_path).arg("--version").output()?;
                if !output.status.success() {
                    return Err(BuildError::InvalidSpec(format!(
                        "failed to probe observed build tool {executable}"
                    )));
                }
                let version_output = String::from_utf8_lossy(&output.stdout);
                let version = version_output
                    .lines()
                    .next()
                    .unwrap_or_default()
                    .trim()
                    .to_string();
                if version.is_empty() {
                    return Err(BuildError::InvalidSpec(format!(
                        "observed build tool {executable} returned no version"
                    )));
                }
                let parameters = match role.as_str() {
                    "cc" => flag_parameters([
                        ("CPPFLAGS", self.config.cppflags.as_str()),
                        ("CFLAGS", self.config.cflags.as_str()),
                    ]),
                    "cxx" => flag_parameters([
                        ("CPPFLAGS", self.config.cppflags.as_str()),
                        ("CXXFLAGS", self.config.cxxflags.as_str()),
                    ]),
                    "linker" => flag_parameters([("LDFLAGS", self.config.ldflags.as_str())]),
                    "rustc" => flag_parameters([("RUSTFLAGS", self.config.rustflags.as_str())]),
                    _ => unreachable!(),
                };
                Ok(sage_archive::ManagedBuildTool {
                    role,
                    executable,
                    family: tool_family(&host_path.to_string_lossy()),
                    version,
                    version_argument: "--version".into(),
                    parameters,
                })
            })
            .collect()
    }

    fn host_tool_path(
        &self,
        paths: &SandboxPaths,
        executable: &str,
    ) -> Result<PathBuf, BuildError> {
        let sandbox_path = Path::new(executable);
        if let Ok(relative) = sandbox_path.strip_prefix("/toolchain") {
            return paths
                .toolchain
                .as_ref()
                .map(|root| root.join(relative))
                .ok_or_else(|| {
                    BuildError::InvalidSpec(
                        "toolchain observation exists without a toolchain mount".into(),
                    )
                });
        }
        if let (Some(toolchain), Ok(relative)) =
            (&paths.toolchain, sandbox_path.strip_prefix("/usr"))
        {
            return Ok(toolchain.join("usr").join(relative));
        }
        if sandbox_path.is_absolute() {
            return Ok(self
                .config
                .sysroot
                .join(sandbox_path.strip_prefix("/").unwrap()));
        }
        Ok(sandbox_path.to_path_buf())
    }

    fn environment(&self, paths: &SandboxPaths) -> BTreeMap<String, String> {
        let toolchain = paths.toolchain.is_some();
        let path = "/usr/bin:/bin";
        let mut environment = BTreeMap::from([
            ("LC_ALL".into(), "C".into()),
            ("TZ".into(), "UTC".into()),
            ("HOME".into(), "/build".into()),
            ("PATH".into(), path.into()),
            ("SRC_DIR".into(), "/source".into()),
            ("BUILD_DIR".into(), "/build".into()),
            ("DESTDIR".into(), "/dest".into()),
            ("JOBS".into(), self.config.jobs.to_string()),
            (
                "SOURCE_DATE_EPOCH".into(),
                self.config.source_date_epoch.to_string(),
            ),
            ("CC".into(), "/build/.sage-tools/cc".into()),
            ("CXX".into(), "/build/.sage-tools/cxx".into()),
            ("LD".into(), "/build/.sage-tools/ld".into()),
            ("RUSTC".into(), "/build/.sage-tools/rustc".into()),
            ("CFLAGS".into(), self.config.cflags.clone()),
            ("CXXFLAGS".into(), self.config.cxxflags.clone()),
            ("CPPFLAGS".into(), self.config.cppflags.clone()),
            ("LDFLAGS".into(), self.config.ldflags.clone()),
            ("RUSTFLAGS".into(), self.config.rustflags.clone()),
        ]);
        if toolchain {
            environment.extend([
                (
                    "PKG_CONFIG_PATH".into(),
                    "/usr/lib/pkgconfig:/usr/share/pkgconfig".into(),
                ),
                ("CMAKE_PREFIX_PATH".into(), "/usr".into()),
                ("ACLOCAL_PATH".into(), "/usr/share/aclocal".into()),
                ("PYTHONPATH".into(), "/usr/lib/python/site-packages".into()),
                ("LD_LIBRARY_PATH".into(), "/usr/lib:/usr/lib64".into()),
            ]);
        }
        if paths.target_sysroot.is_some() {
            environment.extend([
                ("PKG_CONFIG_SYSROOT_DIR".into(), "/sysroot".into()),
                (
                    "PKG_CONFIG_LIBDIR".into(),
                    "/sysroot/usr/lib/pkgconfig:/sysroot/usr/share/pkgconfig".into(),
                ),
                ("CMAKE_FIND_ROOT_PATH".into(), "/sysroot".into()),
            ]);
        }
        environment
    }
}

fn flag_parameters<const N: usize>(values: [(&str, &str); N]) -> Vec<String> {
    values
        .into_iter()
        .filter(|(_, value)| !value.is_empty())
        .map(|(name, value)| format!("{name}={value}"))
        .collect()
}

/// One mutually exclusive package tree carved from a shared DESTDIR.
pub struct PackageStagingArea {
    pub name: String,
    root: tempfile::TempDir,
    pub dependencies: Vec<String>,
    pub provides: Vec<String>,
}

impl PackageStagingArea {
    pub fn path(&self) -> &Path {
        self.root.path()
    }
}

/// Declaratively partitions build output using first-match ownership.
pub struct PayloadCarver;

impl PayloadCarver {
    pub fn carve_packages(
        destdir: &Path,
        recipe: &RecipeSpec,
    ) -> Result<Vec<PackageStagingArea>, BuildError> {
        let claims: Vec<_> = recipe
            .subpackages
            .iter()
            .map(|subpackage| {
                Ok((
                    compile_patterns(&subpackage.payload.files)?,
                    compile_patterns(&subpackage.payload.excludes)?,
                ))
            })
            .collect::<Result<_, BuildError>>()?;
        let main_patterns = compile_patterns(&recipe.build.payload.files)?;
        let main_excludes = compile_patterns(&recipe.build.payload.excludes)?;
        let mut areas = Vec::with_capacity(recipe.subpackages.len() + 1);
        areas.push(staging(
            &recipe.package.name,
            &recipe.package.dependencies,
            &recipe.package.provides,
        )?);
        for subpackage in &recipe.subpackages {
            areas.push(staging(
                &subpackage.name,
                &subpackage.dependencies,
                &subpackage.provides,
            )?);
        }
        let mut paths: Vec<_> = walkdir::WalkDir::new(destdir)
            .follow_links(false)
            .into_iter()
            .filter_map(|entry| match entry {
                Ok(entry) if entry.path() != destdir && !entry.file_type().is_dir() => {
                    Some(Ok(entry))
                }
                Ok(_) => None,
                Err(error) => Some(Err(error)),
            })
            .collect::<Result<_, _>>()?;
        paths.sort_by_key(|entry| entry.path().to_path_buf());
        for entry in paths {
            let relative = entry
                .path()
                .strip_prefix(destdir)
                .expect("walkdir keeps entries beneath its root");
            let owner = claims
                .iter()
                .position(|(includes, excludes)| {
                    matches_any(includes, relative) && !matches_any(excludes, relative)
                })
                .map(|index| index + 1)
                .or_else(|| {
                    let allowed = main_patterns.is_empty() || matches_any(&main_patterns, relative);
                    (allowed && !matches_any(&main_excludes, relative)).then_some(0)
                });
            if let Some(owner) = owner {
                link_entry(
                    entry.path(),
                    &areas[owner].path().join("data").join(relative),
                )?;
            }
        }
        Ok(areas)
    }
}

fn staging(
    name: &str,
    dependencies: &[String],
    provides: &[String],
) -> Result<PackageStagingArea, BuildError> {
    let root = tempfile::Builder::new().prefix("sage-package-").tempdir()?;
    fs::create_dir(root.path().join("data"))?;
    Ok(PackageStagingArea {
        name: name.into(),
        root,
        dependencies: dependencies.to_vec(),
        provides: provides.to_vec(),
    })
}

fn compile_patterns(patterns: &[String]) -> Result<Vec<glob::Pattern>, BuildError> {
    patterns
        .iter()
        .map(|pattern| Ok(glob::Pattern::new(pattern)?))
        .collect()
}

fn matches_any(patterns: &[glob::Pattern], path: &Path) -> bool {
    patterns.iter().any(|pattern| pattern.matches_path(path))
}

fn link_entry(source: &Path, target: &Path) -> Result<(), BuildError> {
    if let Some(parent) = target.parent() {
        fs::create_dir_all(parent)?;
    }
    let metadata = fs::symlink_metadata(source)?;
    if metadata.file_type().is_symlink() {
        let link = fs::read_link(source)?;
        if link.is_absolute() {
            return Err(BuildError::InvalidSpec(format!(
                "unsafe payload symlink {} -> {}",
                source.display(),
                link.display()
            )));
        }
        std::os::unix::fs::symlink(link, target)?;
    } else if fs::hard_link(source, target).is_err() {
        fs::copy(source, target)?;
        fs::set_permissions(target, metadata.permissions())?;
    }
    Ok(())
}

/// Dynamic symbols discovered from one independently carved package.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct ElfSymbols {
    pub provides: BTreeSet<String>,
    pub dependencies: BTreeSet<String>,
}

pub struct ElfScanner;

/// Summary of deterministic RUNPATH updates made below one DESTDIR.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct RunpathReport {
    pub library_dirs: Vec<PathBuf>,
    pub rewritten: Vec<PathBuf>,
}

impl ElfScanner {
    /// Scans regular files once and ignores non-ELF payloads without error.
    pub fn scan(root: &Path) -> Result<ElfSymbols, BuildError> {
        let mut symbols = ElfSymbols::default();
        for entry in walkdir::WalkDir::new(root).follow_links(false) {
            let entry = entry?;
            if !entry.file_type().is_file() {
                continue;
            }
            let bytes = fs::read(entry.path())?;
            if !bytes.starts_with(b"\x7fELF") {
                continue;
            }
            let goblin::Object::Elf(elf) = goblin::Object::parse(&bytes)? else {
                continue;
            };
            if let Some(soname) = elf.soname {
                symbols.provides.insert(format!("so:{soname}"));
            }
            symbols
                .dependencies
                .extend(elf.libraries.into_iter().map(|name| format!("so:{name}")));
        }
        Ok(symbols)
    }

    /// Replaces host-dependent ELF search paths with paths relative to each file.
    ///
    /// Directories containing a shared object with a SONAME are discovered in one
    /// pass. `extra_dirs` covers libraries supplied by another package in the same
    /// private channel. Every input must be relative to `root`, so generated paths
    /// cannot escape the future channel installation root.
    pub fn rewrite_private_runpaths(
        root: &Path,
        extra_dirs: &[PathBuf],
        patchelf: &Path,
    ) -> Result<RunpathReport, BuildError> {
        let mut library_dirs = BTreeSet::new();
        for directory in extra_dirs {
            validate_relative_path(directory)?;
            library_dirs.insert(directory.clone());
        }

        let mut dynamic_files = Vec::new();
        for entry in walkdir::WalkDir::new(root).follow_links(false) {
            let entry = entry?;
            if !entry.file_type().is_file() {
                continue;
            }
            let bytes = fs::read(entry.path())?;
            if !bytes.starts_with(b"\x7fELF") {
                continue;
            }
            let goblin::Object::Elf(elf) = goblin::Object::parse(&bytes)? else {
                continue;
            };
            let relative = entry
                .path()
                .strip_prefix(root)
                .map_err(|_| BuildError::InvalidSpec("ELF escaped DESTDIR".into()))?
                .to_path_buf();
            if elf.soname.is_some() {
                library_dirs.insert(relative.parent().unwrap_or(Path::new("")).to_path_buf());
            }
            if !elf.libraries.is_empty() {
                let retained = elf
                    .runpaths
                    .iter()
                    .chain(elf.rpaths.iter())
                    .flat_map(|paths| paths.split(':'))
                    .filter(|path| path == &"$ORIGIN" || path.starts_with("$ORIGIN/"))
                    .map(str::to_owned)
                    .collect::<BTreeSet<_>>();
                dynamic_files.push((relative, retained));
            }
        }

        let mut report = RunpathReport {
            library_dirs: library_dirs.iter().cloned().collect(),
            rewritten: Vec::new(),
        };
        if library_dirs.is_empty() {
            return Ok(report);
        }
        for (file, mut runpaths) in dynamic_files {
            let parent = file.parent().unwrap_or(Path::new(""));
            for directory in &library_dirs {
                runpaths.insert(origin_path(parent, directory)?);
            }
            let value = runpaths.into_iter().collect::<Vec<_>>().join(":");
            let output = Command::new(patchelf)
                .arg("--set-rpath")
                .arg(value)
                .arg(root.join(&file))
                .output()?;
            if !output.status.success() {
                return Err(BuildError::Patchelf {
                    path: file,
                    message: String::from_utf8_lossy(&output.stderr).trim().to_owned(),
                });
            }
            report.rewritten.push(file);
        }
        Ok(report)
    }
}

/// Ensures every kernel module tree belongs to the package's declared Slot.
///
/// Kernel packages and out-of-tree modules can therefore coexist without a
/// special package identity: `usr/lib/modules/<version>` and the manifest Slot
/// must agree. Packages without a modules directory take the fast no-op path.
pub fn validate_kernel_module_slot(root: &Path, slot: &str) -> Result<(), BuildError> {
    let modules = root.join("usr/lib/modules");
    if !modules.exists() {
        return Ok(());
    }
    for entry in fs::read_dir(modules)? {
        let entry = entry?;
        let version = entry.file_name();
        if version != std::ffi::OsStr::new(slot) {
            return Err(BuildError::InvalidSpec(format!(
                "kernel module directory '{}' does not match package slot '{slot}'",
                version.to_string_lossy()
            )));
        }
    }
    Ok(())
}

fn validate_relative_path(path: &Path) -> Result<(), BuildError> {
    if path.as_os_str().is_empty()
        || path
            .components()
            .any(|part| !matches!(part, std::path::Component::Normal(_)))
    {
        return Err(BuildError::InvalidSpec(format!(
            "private library directory must be a non-empty relative path: {}",
            path.display()
        )));
    }
    Ok(())
}

/// Computes a lexical relative path because both inputs are already confined to
/// one DESTDIR. No filesystem canonicalization is used, so symlinks cannot change
/// the result or make a reproducible build depend on the host filesystem.
fn origin_path(from: &Path, to: &Path) -> Result<String, BuildError> {
    let from = from.components().collect::<Vec<_>>();
    let to = to.components().collect::<Vec<_>>();
    let common = from
        .iter()
        .zip(&to)
        .take_while(|(left, right)| left == right)
        .count();
    let mut parts = vec!["..".to_owned(); from.len() - common];
    for part in &to[common..] {
        let std::path::Component::Normal(part) = part else {
            return Err(BuildError::InvalidSpec(
                "invalid private library path".into(),
            ));
        };
        parts.push(
            part.to_str()
                .ok_or_else(|| BuildError::InvalidSpec("non-UTF-8 private library path".into()))?
                .to_owned(),
        );
    }
    Ok(if parts.is_empty() {
        "$ORIGIN".into()
    } else {
        format!("$ORIGIN/{}", parts.join("/"))
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_build_config() -> BuildConfig {
        BuildConfig {
            schema_version: 1,
            fakeroot: "/usr/bin/fakeroot".into(),
            bwrap: "/usr/bin/bwrap".into(),
            git: "/usr/bin/git".into(),
            sysroot: "/".into(),
            cc: "gcc".into(),
            cxx: "g++".into(),
            linker: "ld.lld".into(),
            rustc: "rustc".into(),
            patchelf: "/usr/bin/patchelf".into(),
            cflags: "-O2 -pipe".into(),
            cxxflags: "-O2 -pipe".into(),
            cppflags: "-DNDEBUG".into(),
            ldflags: "-Wl,--as-needed".into(),
            rustflags: "-C debuginfo=0".into(),
            source_date_epoch: 0,
            jobs: 1,
            memory_limit: String::new(),
            pids_limit: 128,
            compiler_cache: String::new(),
            ccache_dir: PathBuf::new(),
            sandbox_dependencies: Vec::new(),
            build: "x86_64-pc-linux-gnu".into(),
            targets: BTreeMap::new(),
        }
    }

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
    fn ordered_argument_channels_append_instead_of_erasing_prior_flags() {
        let mut arguments = BTreeMap::from([
            ("meson_args".into(), "-Dbase=enabled".into()),
            ("mode".into(), "release".into()),
        ]);
        merge_build_arguments(
            &mut arguments,
            &BTreeMap::from([
                ("meson_args".into(), "-Dtls=enabled".into()),
                ("mode".into(), "debug".into()),
            ]),
        );
        assert_eq!(arguments["meson_args"], "-Dbase=enabled -Dtls=enabled");
        assert_eq!(arguments["mode"], "debug");
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
    fn verified_file_sources_are_copied_without_archive_extraction() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("recipe.toml");
        fs::write(
            &path,
            format!(
                r#"schema_version=1
[package]
name="patched"
version="1"
release=1
description="patched"
license="MIT"
channel="system"
arch="any"

[[sources]]
url="https://example.invalid/source.tar.xz"
sha256="{}"

[[sources]]
kind="file"
url="https://example.invalid/fix.patch"
sha256="{}"
destination=".source-patches/001"
"#,
                "00".repeat(32),
                "11".repeat(32)
            ),
        )
        .unwrap();
        let recipe = RecipeSpec::load(path).unwrap();
        assert_eq!(
            recipe.source_inputs().nth(1).unwrap().kind,
            SourceKind::File
        );
        assert_eq!(
            recipe.source_manifest(),
            concat!(
                "000-source\tarchive\t1\t.\n",
                "001-source\tfile\t0\t.source-patches/001\n",
            )
        );
    }

    #[test]
    fn source_free_declarative_install_materializes_modes_and_usr_merge_links() {
        let directory = tempfile::tempdir().unwrap();
        let recipe_path = directory.path().join("recipe.toml");
        fs::write(
            &recipe_path,
            r#"schema_version=1
[package]
name="base-files"
version="1"
release=1
description="base"
license="MIT"
channel="system"
arch="any"

[[install.directories]]
path="tmp"
mode=1023

[[install.files]]
path="etc/issue"
content="Sage\n"

[[install.symlinks]]
path="bin"
target="usr/bin"
"#,
        )
        .unwrap();
        let recipe = RecipeSpec::load(recipe_path).unwrap();
        let destdir = directory.path().join("dest");
        fs::create_dir(&destdir).unwrap();

        stage_declarative_install(&destdir, &recipe).unwrap();

        assert_eq!(
            fs::read_to_string(destdir.join("etc/issue")).unwrap(),
            "Sage\n"
        );
        assert_eq!(
            fs::read_link(destdir.join("bin")).unwrap(),
            Path::new("usr/bin")
        );
        assert_eq!(
            fs::metadata(destdir.join("tmp"))
                .unwrap()
                .permissions()
                .mode()
                & 0o7777,
            0o1777
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
        assert_eq!(
            recipe.source_manifest(),
            "000-source\ttree\t0\tvendor/project\n"
        );

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
        BuildUnit {
            recipe: PathBuf::from(format!("{name}/recipe.toml")),
            name: name.into(),
            packages: produces.iter().map(|value| (*value).into()).collect(),
            produces: produces.iter().map(|value| (*value).into()).collect(),
            consumes: consumes.iter().map(|value| (*value).into()).collect(),
        }
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
    fn sysusers_are_validated_and_lifecycle_scripts_are_rejected() {
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
        RecipeSpec::load(&path).unwrap();

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
    fn build_tool_metadata_contains_only_observed_roles_and_their_flags() {
        let directory = tempfile::tempdir().unwrap();
        let build = directory.path().join("build");
        let toolchain = directory.path().join("toolchain");
        fs::create_dir(&build).unwrap();
        fs::create_dir_all(toolchain.join("usr/bin")).unwrap();
        for name in ["gcc", "ld.lld"] {
            let path = toolchain.join("usr/bin").join(name);
            fs::write(&path, format!("#!/bin/sh\necho '{name} version 1'\n")).unwrap();
            fs::set_permissions(&path, fs::Permissions::from_mode(0o755)).unwrap();
        }
        fs::write(
            build.join(".sage-tool-usage"),
            "cc\t/usr/bin/gcc\nlinker\t/usr/bin/ld.lld\ncc\t/usr/bin/gcc\n",
        )
        .unwrap();
        let paths = SandboxPaths {
            source: directory.path().join("source"),
            build,
            destdir: directory.path().join("dest"),
            runner: directory.path().join("runner"),
            toolchain: Some(toolchain),
            target_sysroot: None,
        };
        let config = test_build_config();

        let tools = SandboxRunner::new(&config)
            .read_build_tools(&paths)
            .unwrap();

        assert_eq!(tools.len(), 2);
        assert_eq!(tools[0].role, "cc");
        assert_eq!(tools[0].family, "gcc");
        assert_eq!(
            tools[0].parameters,
            ["CPPFLAGS=-DNDEBUG", "CFLAGS=-O2 -pipe"]
        );
        assert_eq!(tools[1].role, "linker");
        assert_eq!(tools[1].family, "lld");
        assert_eq!(tools[1].parameters, ["LDFLAGS=-Wl,--as-needed"]);
        assert!(!tools
            .iter()
            .any(|tool| tool.role == "cxx" || tool.role == "rustc"));
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
                destination: default_source_destination(),
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
            alternatives: vec![],
            install: InstallSpec::default(),
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
        assert!(origin_path(Path::new("lib"), Path::new("lib")).unwrap() == "$ORIGIN");
        assert!(validate_relative_path(Path::new("../lib")).is_err());
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
