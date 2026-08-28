//! Recipe, package, version, and bootstrap-plan models.

use serde::Deserialize;
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};

use crate::sources::validate_source_destination;
use crate::{source_archive_name, validate_schema, BuildError};

/// Schema-v1 source input.
#[derive(Debug, Clone, Deserialize)]
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
}

fn default_source_destination() -> PathBuf {
    PathBuf::from(".")
}

/// Shared and main-package metadata from a recipe.
#[derive(Debug, Clone, Deserialize)]
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

impl RecipePackage {
    /// Returns the unified package coordinate used by build and repository code.
    pub fn coordinate(&self) -> sage_core::PackageCoordinate {
        sage_core::PackageCoordinate::new(
            sage_core::PackageKey::new(&self.channel, &self.name, &self.slot),
            sage_core::Version::new(self.epoch, &self.version, self.release),
        )
    }
}

fn default_slot() -> String {
    sage_core::DEFAULT_SLOT.into()
}

/// Ordered glob claims used to partition one DESTDIR.
#[derive(Debug, Clone, Default, Deserialize)]
pub struct PayloadSpec {
    #[serde(default)]
    pub files: Vec<String>,
    #[serde(default)]
    pub excludes: Vec<String>,
    #[serde(default)]
    pub default: String,
}

#[derive(Debug, Clone, Default, Deserialize)]
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

/// One declarative account emitted in systemd-sysusers compatible syntax.
#[derive(Debug, Clone, Deserialize)]
pub struct SysuserSpec {
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

fn default_home() -> String {
    "/".into()
}

fn default_shell() -> String {
    "/usr/bin/nologin".into()
}

/// Complete single-build, multiple-output recipe.
#[derive(Debug, Clone, Deserialize)]
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
        if recipe.source.is_none() && recipe.sources.is_empty() {
            return Err(BuildError::InvalidSpec(
                "at least one source is required".into(),
            ));
        }
        for source in recipe.source_inputs() {
            validate_source_destination(&source.destination)?;
            source.validate()?;
        }
        for user in &recipe.sysusers {
            user.validate()?;
        }
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
            let strip = if source.kind == SourceKind::Git {
                0
            } else {
                source
                    .strip_components
                    .unwrap_or(if index == 0 { 1 } else { 0 })
            };
            manifest.push_str(&format!(
                "{}\t{strip}\t{}\n",
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
            result.args.extend(rule.args.clone());
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

fn valid_feature_name(name: &str) -> bool {
    !name.is_empty()
        && name.bytes().all(|byte| {
            byte.is_ascii_lowercase() || byte.is_ascii_digit() || matches!(byte, b'_' | b'-')
        })
}

impl SysuserSpec {
    fn validate(&self) -> Result<(), BuildError> {
        if !matches!(self.kind.as_str(), "user" | "group")
            || self.name.is_empty()
            || !self
                .name
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-'))
            || self.description.contains(['\n', '\r', '\0', '"'])
            || !Path::new(&self.home).is_absolute()
            || !Path::new(&self.shell).is_absolute()
            || [&self.home, &self.shell].iter().any(|value| {
                value
                    .bytes()
                    .any(|byte| byte.is_ascii_whitespace() || matches!(byte, b'"' | b'\\'))
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

/// Writes all recipe accounts into one deterministic sysusers payload file.
pub fn stage_sysusers(root: &Path, recipe: &RecipeSpec) -> Result<(), BuildError> {
    if recipe.sysusers.is_empty() {
        return Ok(());
    }
    let directory = root.join("usr/lib/sysusers.d");
    fs::create_dir_all(&directory)?;
    let mut output = String::new();
    for account in &recipe.sysusers {
        let id = account.id.map_or_else(|| "-".into(), |id| id.to_string());
        if account.kind == "group" {
            output.push_str(&format!("g {} {id}\n", account.name));
        } else {
            let description = account.description.replace('\\', "\\\\");
            output.push_str(&format!(
                "u {} {id} \"{}\" {} {}\n",
                account.name, description, account.home, account.shell
            ));
        }
    }
    fs::write(
        directory.join(format!("{}.conf", recipe.package.name)),
        output,
    )?;
    Ok(())
}
