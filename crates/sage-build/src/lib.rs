//! Declarative rclass execution, Bubblewrap isolation, payload carving, and ELF scans.

use serde::Deserialize;
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
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
}

/// Schema-v1 source input.
#[derive(Debug, Clone, Deserialize)]
pub struct SourceSpec {
    pub url: String,
    pub sha256: String,
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
    #[serde(default)]
    pub payload: PayloadSpec,
    #[serde(default)]
    pub allow_network: bool,
    /// Extra private-library directories, relative to the installed channel root.
    #[serde(default)]
    pub private_library_dirs: Vec<PathBuf>,
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
        if recipe.source_inputs().any(|source| {
            source.sha256.len() != 64 || !source.sha256.bytes().all(|byte| byte.is_ascii_hexdigit())
        }) {
            return Err(BuildError::InvalidSpec(
                "every source SHA-256 must contain 64 hex digits".into(),
            ));
        }
        for user in &recipe.sysusers {
            user.validate()?;
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
        Ok(recipe)
    }

    /// Iterates the legacy singleton or the schema-v1 multi-source array.
    pub fn source_inputs(&self) -> impl Iterator<Item = &SourceSpec> {
        self.source.iter().chain(self.sources.iter())
    }

    /// Returns whether the package is installed outside the system channel root.
    pub fn uses_private_channel(&self) -> bool {
        self.package.channel != "system" && !self.package.channel.ends_with("/system")
    }
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

/// Global reproducible-build and sandbox policy.
#[derive(Debug, Clone, Deserialize)]
pub struct BuildConfig {
    pub schema_version: u32,
    pub fakeroot: PathBuf,
    pub bwrap: PathBuf,
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
}

fn default_pids() -> u32 {
    2048
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
    for class in classes {
        env.extend(class.env.clone());
        phases.extend(class.phases.clone());
    }
    let mut script = String::from(
        "#!/bin/bash\nset -euo pipefail\ntrap 'printf >&2 \"sage-build: line %s failed\\n\" \"$LINENO\"' ERR\n",
    );
    for (name, value) in env {
        validate_shell_name(&name)?;
        script.push_str("export ");
        script.push_str(&name);
        script.push('=');
        script.push_str(&shell_quote(&expand(&value, variables)?));
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
        script.push_str(&expand(body, variables)?);
        if !body.ends_with('\n') {
            script.push('\n');
        }
    }
    Ok(script)
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
        }
        command.arg("--clearenv");
        for (name, value) in self.environment(paths.toolchain.is_some()) {
            command.args(["--setenv", &name, &value]);
        }
        command.arg("--").arg(&self.config.fakeroot).args([
            "--",
            "/bin/bash",
            "/run/sage-build-runner.sh",
        ]);
        command
    }

    pub fn run(&self, paths: &SandboxPaths, allow_network: bool) -> Result<(), BuildError> {
        let status = self.command(paths, allow_network).status()?;
        if status.success() {
            Ok(())
        } else {
            Err(BuildError::SandboxFailed(status))
        }
    }

    fn environment(&self, toolchain: bool) -> BTreeMap<String, String> {
        let path = if toolchain {
            "/toolchain/bin:/usr/bin:/bin"
        } else {
            "/usr/bin:/bin"
        };
        BTreeMap::from([
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
            ("CC".into(), self.config.cc.clone()),
            ("CXX".into(), self.config.cxx.clone()),
            ("LD".into(), self.config.linker.clone()),
            ("RUSTC".into(), self.config.rustc.clone()),
            ("CFLAGS".into(), self.config.cflags.clone()),
            ("CXXFLAGS".into(), self.config.cxxflags.clone()),
            ("CPPFLAGS".into(), self.config.cppflags.clone()),
            ("LDFLAGS".into(), self.config.ldflags.clone()),
            ("RUSTFLAGS".into(), self.config.rustflags.clone()),
        ])
    }
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

    #[test]
    fn runner_orders_phases_and_rejects_unknown_variables() {
        let class = Rclass {
            schema_version: 1,
            name: "demo".into(),
            description: "demo".into(),
            implicit_build_dependencies: vec![],
            allowed_compilers: vec![],
            allowed_linkers: vec![],
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
                url: "https://example.invalid/x".into(),
                sha256: "00".repeat(32),
            }),
            sources: vec![],
            build: RecipeBuild::default(),
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
