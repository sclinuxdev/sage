//! Declarative triggers, init-template rendering, profiles, and reconciliation models.

use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Component, Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use thiserror::Error;

static TEMP_ID: AtomicU64 = AtomicU64::new(0);

/// System orchestration failures.
#[derive(Debug, Error)]
pub enum SysError {
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
    #[error("TOML parsing error: {0}")]
    Toml(#[from] toml::de::Error),
    #[error("JSON encoding error: {0}")]
    Json(#[from] serde_json::Error),
    #[error("unsupported schema version {0}")]
    Schema(u32),
    #[error("invalid declaration: {0}")]
    Invalid(String),
    #[error("trigger '{name}' exited with status {status}")]
    Trigger {
        name: String,
        status: std::process::ExitStatus,
    },
    #[error("template contains unknown variable '{0}'")]
    UnknownVariable(String),
    #[error("dependency solver failed: {0}")]
    Solver(#[from] sage_solver::SolverError),
}

fn validate_schema(version: u32) -> Result<(), SysError> {
    if version == sage_core::SCHEMA_VERSION {
        Ok(())
    } else {
        Err(SysError::Schema(version))
    }
}

/// One data-driven command activated by changed path globs.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TriggerSpec {
    pub schema_version: u32,
    pub name: String,
    pub description: String,
    pub on_paths: Vec<String>,
    pub exec: Vec<String>,
    pub priority: u32,
    /// Transaction boundaries at which this trigger is eligible to run.
    #[serde(default = "default_trigger_events")]
    pub events: Vec<TriggerEvent>,
    #[serde(default)]
    pub ignore_missing_binary: bool,
}

/// Safe transaction boundaries exposed to declarative lifecycle handlers.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum TriggerEvent {
    PostChange,
    PostRemove,
    Rebuild,
}

fn default_trigger_events() -> Vec<TriggerEvent> {
    vec![TriggerEvent::PostChange, TriggerEvent::PostRemove]
}

pub struct TriggerEngine;

impl TriggerEngine {
    /// Loads vendor triggers, then applies administrator overrides by name.
    pub fn load_triggers(sysroot: &Path) -> Result<Vec<TriggerSpec>, SysError> {
        let mut triggers = BTreeMap::new();
        for relative in ["usr/share/sage/triggers", "etc/sage/triggers.d"] {
            let directory = sysroot.join(relative);
            if !directory.exists() {
                continue;
            }
            let mut files: Vec<_> = fs::read_dir(directory)?.collect::<Result<_, _>>()?;
            files.sort_by_key(|entry| entry.file_name());
            for file in files {
                if file.path().extension().and_then(|value| value.to_str()) != Some("toml") {
                    continue;
                }
                let trigger = TriggerSpec::load(file.path())?;
                triggers.insert(trigger.name.clone(), trigger);
            }
        }
        let mut triggers: Vec<_> = triggers.into_values().collect();
        triggers.sort_by_key(|trigger| (trigger.priority, trigger.name.clone()));
        Ok(triggers)
    }

    /// Executes each distinct expanded command once in priority order.
    pub fn execute_triggers(
        triggers: &[TriggerSpec],
        modified_paths: &[PathBuf],
        sysroot: &Path,
    ) -> Result<Vec<String>, SysError> {
        Self::execute_triggers_for(triggers, modified_paths, sysroot, TriggerEvent::PostChange)
    }

    /// Executes triggers eligible for one explicit transaction boundary.
    pub fn execute_triggers_for(
        triggers: &[TriggerSpec],
        modified_paths: &[PathBuf],
        sysroot: &Path,
        event: TriggerEvent,
    ) -> Result<Vec<String>, SysError> {
        let mut executed = Vec::new();
        for trigger in triggers {
            if !trigger.events.contains(&event) {
                continue;
            }
            let patterns: Vec<_> = trigger
                .on_paths
                .iter()
                .map(|pattern| {
                    glob::Pattern::new(pattern).map_err(|error| {
                        SysError::Invalid(format!("trigger {}: {error}", trigger.name))
                    })
                })
                .collect::<Result<_, _>>()?;
            let matching_paths = modified_paths
                .iter()
                .filter(|path| patterns.iter().any(|pattern| pattern.matches_path(path)));
            let commands = matching_paths
                .map(|path| {
                    trigger
                        .exec
                        .iter()
                        .map(|argument| expand_trigger_argument(argument, path, sysroot))
                        .collect::<Result<Vec<_>, _>>()
                })
                .collect::<Result<BTreeSet<_>, _>>()?;
            if commands.is_empty() {
                continue;
            }
            let mut ran = false;
            for command in commands {
                let binary = target_path(sysroot, Path::new(&command[0]))?;
                if !binary.exists() && trigger.ignore_missing_binary {
                    continue;
                }
                ensure_existing_beneath(sysroot, &binary)?;
                let status = Command::new(binary)
                    .args(&command[1..])
                    .current_dir(sysroot)
                    .env_clear()
                    .env("PATH", "/usr/bin:/bin")
                    .env("SAGE_SYSROOT", sysroot)
                    .stdin(Stdio::null())
                    .status()?;
                if !status.success() {
                    return Err(SysError::Trigger {
                        name: trigger.name.clone(),
                        status,
                    });
                }
                ran = true;
            }
            if ran {
                executed.push(trigger.name.clone());
            }
        }
        Ok(executed)
    }
}

impl TriggerSpec {
    /// Loads and validates one schema-v1 trigger declaration.
    pub fn load(path: impl AsRef<Path>) -> Result<Self, SysError> {
        Self::parse(&fs::read(path)?)
    }

    /// Parses and validates one trigger without executing its command.
    pub fn parse(bytes: &[u8]) -> Result<Self, SysError> {
        let text = std::str::from_utf8(bytes)
            .map_err(|_| SysError::Invalid("trigger document is not UTF-8".into()))?;
        let trigger: Self = toml::from_str(text)?;
        validate_schema(trigger.schema_version)?;
        if !valid_declaration_name(&trigger.name)
            || trigger.exec.is_empty()
            || trigger.on_paths.is_empty()
            || trigger.exec.iter().any(|value| value.contains('\0'))
        {
            return Err(SysError::Invalid(format!(
                "incomplete or invalid trigger '{}'",
                trigger.name
            )));
        }
        for pattern in &trigger.on_paths {
            glob::Pattern::new(pattern)
                .map_err(|error| SysError::Invalid(format!("trigger {}: {error}", trigger.name)))?;
        }
        Ok(trigger)
    }
}

/// Expands path data without invoking a shell. Only `${path}` and the indexed
/// `${path[N]}` form are accepted, keeping trigger commands deterministic and
/// preventing configuration typos from silently reaching privileged tools.
fn expand_trigger_argument(
    template: &str,
    path: &Path,
    sysroot: &Path,
) -> Result<String, SysError> {
    let mut output = String::with_capacity(template.len());
    let mut rest = template;
    while let Some(start) = rest.find("${") {
        output.push_str(&rest[..start]);
        let expression = &rest[start + 2..];
        let end = expression.find('}').ok_or_else(|| {
            SysError::Invalid(format!("unterminated trigger variable in '{template}'"))
        })?;
        let variable = &expression[..end];
        let value = if variable == "path" {
            path.to_str()
        } else if variable == "sysroot" {
            sysroot.to_str()
        } else if let Some(index) = variable
            .strip_prefix("path[")
            .and_then(|value| value.strip_suffix(']'))
            .and_then(|value| value.parse::<usize>().ok())
        {
            path.components()
                .nth(index)
                .and_then(|part| part.as_os_str().to_str())
        } else {
            return Err(SysError::Invalid(format!(
                "unknown trigger variable '{variable}'"
            )));
        }
        .ok_or_else(|| {
            SysError::Invalid(format!(
                "trigger variable '{variable}' is unavailable for {}",
                path.display()
            ))
        })?;
        output.push_str(value);
        rest = &expression[end + 1..];
    }
    output.push_str(rest);
    Ok(output)
}

/// Init-independent daemon declaration carried by a package.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ServiceSpec {
    pub name: String,
    pub description: String,
    pub command: Vec<String>,
    #[serde(default)]
    pub stop_command: Vec<String>,
    #[serde(default)]
    pub reload_command: Vec<String>,
    pub user: String,
    pub group: String,
    pub working_dir: String,
    #[serde(default)]
    pub pid_file: String,
    pub restart: String,
    #[serde(rename = "type")]
    pub service_type: String,
    #[serde(default)]
    pub after: Vec<String>,
    #[serde(default)]
    pub before: Vec<String>,
    #[serde(default)]
    pub runtime: String,
}

#[derive(Debug, Deserialize)]
struct ServiceDocument {
    schema_version: u32,
    service: ServiceSpec,
}

impl ServiceSpec {
    pub fn load(path: impl AsRef<Path>) -> Result<Self, SysError> {
        Self::parse(&fs::read(path)?)
    }

    pub fn parse(bytes: &[u8]) -> Result<Self, SysError> {
        let text = std::str::from_utf8(bytes)
            .map_err(|_| SysError::Invalid("service document is not UTF-8".into()))?;
        let document: ServiceDocument = toml::from_str(text)?;
        validate_schema(document.schema_version)?;
        document.service.validate()?;
        Ok(document.service)
    }

    fn validate(&self) -> Result<(), SysError> {
        if !valid_declaration_name(&self.name) || self.command.is_empty() {
            return Err(SysError::Invalid(
                "service name and command are required".into(),
            ));
        }
        if !["always", "on-failure", "no"].contains(&self.restart.as_str())
            || !["simple", "forking"].contains(&self.service_type.as_str())
        {
            return Err(SysError::Invalid(format!(
                "invalid policy for service {}",
                self.name
            )));
        }
        let text = [
            &self.name,
            &self.description,
            &self.user,
            &self.group,
            &self.working_dir,
        ];
        if text.iter().any(|value| value.contains(['\n', '\r', '\0'])) {
            return Err(SysError::Invalid(format!(
                "control character in service {}",
                self.name
            )));
        }
        if self
            .command
            .iter()
            .chain(&self.stop_command)
            .chain(&self.reload_command)
            .any(|value| value.contains(['\n', '\r', '\0']))
        {
            return Err(SysError::Invalid(format!(
                "control character in service command {}",
                self.name
            )));
        }
        Ok(())
    }
}

fn valid_declaration_name(name: &str) -> bool {
    !name.is_empty()
        && name
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-' | b'.'))
}

#[derive(Debug, Deserialize)]
struct InitRclass {
    schema_version: u32,
    service_generator: TemplateServiceGenerator,
}

/// Generic target/template pair loaded from an `init-*.toml` rclass.
#[derive(Debug, Clone, Deserialize)]
pub struct TemplateServiceGenerator {
    #[serde(rename = "target_path")]
    pub target_path_template: String,
    pub mode: u32,
    pub template: String,
    pub validate_command: Option<String>,
    #[serde(alias = "enable_cmd")]
    pub enable_command: Option<String>,
    #[serde(alias = "disable_cmd")]
    pub disable_command: Option<String>,
}

impl TemplateServiceGenerator {
    pub fn from_rclass(path: &Path) -> Result<Self, SysError> {
        let class: InitRclass = toml::from_str(&fs::read_to_string(path)?)?;
        validate_schema(class.schema_version)?;
        Ok(class.service_generator)
    }

    /// Renders and atomically publishes a service definition under `sysroot`.
    pub fn render_service(
        &self,
        service: &ServiceSpec,
        sysroot: &Path,
    ) -> Result<PathBuf, SysError> {
        service.validate()?;
        let variables = service_variables(service, sysroot)?;
        let relative = expand_template(&self.target_path_template, &variables)?;
        let target = target_path(sysroot, Path::new(&relative))?;
        let rendered = expand_template(&self.template, &variables)?;
        let parent = target
            .parent()
            .ok_or_else(|| SysError::Invalid("service target has no parent".into()))?;
        ensure_directory_beneath(sysroot, parent)?;
        let temporary = parent.join(format!(
            ".sage-service-{}-{}-{}",
            service.name,
            std::process::id(),
            TEMP_ID.fetch_add(1, Ordering::Relaxed)
        ));
        let mut options = fs::OpenOptions::new();
        options.write(true).create_new(true);
        std::io::Write::write_all(&mut options.open(&temporary)?, rendered.as_bytes())?;
        fs::set_permissions(&temporary, fs::Permissions::from_mode(self.mode))?;
        fs::rename(&temporary, &target)?;
        if let Some(command) = &self.validate_command {
            run_validation(&expand_template(command, &variables)?, sysroot)?;
        }
        Ok(target)
    }

    /// Executes the init class's generic enable action, when it declares one.
    pub fn enable_service(&self, service: &ServiceSpec, sysroot: &Path) -> Result<(), SysError> {
        if let Some(command) = &self.enable_command {
            run_validation(
                &expand_template(command, &service_variables(service, sysroot)?)?,
                sysroot,
            )?;
        }
        Ok(())
    }
}

fn service_variables(
    service: &ServiceSpec,
    sysroot: &Path,
) -> Result<BTreeMap<String, String>, SysError> {
    let command_tail = service.command.get(1..).unwrap_or_default();
    Ok(BTreeMap::from([
        ("service.name".into(), service.name.clone()),
        ("service.description".into(), service.description.clone()),
        ("service.command[0]".into(), service.command[0].clone()),
        ("service.command[1:]".into(), command_tail.join(" ")),
        ("service.command_str".into(), service.command.join(" ")),
        (
            "service.command_json".into(),
            serde_json::to_string(&service.command)?,
        ),
        ("service.user".into(), service.user.clone()),
        ("service.group".into(), service.group.clone()),
        ("service.working_dir".into(), service.working_dir.clone()),
        ("service.pid_file".into(), service.pid_file.clone()),
        ("service.restart".into(), service.restart.clone()),
        ("service.type".into(), service.service_type.clone()),
        ("service.after".into(), service.after.join(" ")),
        ("service.after_space".into(), service.after.join(" ")),
        (
            "service.after_json".into(),
            serde_json::to_string(&service.after)?,
        ),
        ("SYSROOT".into(), sysroot.display().to_string()),
    ]))
}

fn expand_template(
    template: &str,
    variables: &BTreeMap<String, String>,
) -> Result<String, SysError> {
    let mut output = String::with_capacity(template.len());
    let mut rest = template;
    while let Some(start) = rest.find("${") {
        output.push_str(&rest[..start]);
        let tail = &rest[start + 2..];
        let end = tail
            .find('}')
            .ok_or_else(|| SysError::Invalid("unterminated template variable".into()))?;
        let name = &tail[..end];
        output.push_str(
            variables
                .get(name)
                .ok_or_else(|| SysError::UnknownVariable(name.into()))?,
        );
        rest = &tail[end + 1..];
    }
    output.push_str(rest);
    Ok(output)
}

fn target_path(sysroot: &Path, declared: &Path) -> Result<PathBuf, SysError> {
    let mut relative = PathBuf::new();
    for component in declared.components() {
        match component {
            Component::RootDir | Component::CurDir => {}
            Component::Normal(value) => relative.push(value),
            _ => {
                return Err(SysError::Invalid(format!(
                    "unsafe target path {}",
                    declared.display()
                )))
            }
        }
    }
    if relative.as_os_str().is_empty() {
        return Err(SysError::Invalid("empty target path".into()));
    }
    Ok(sysroot.join(relative))
}

fn run_validation(command: &str, sysroot: &Path) -> Result<(), SysError> {
    let mut words = command.split_whitespace();
    let program = words
        .next()
        .ok_or_else(|| SysError::Invalid("empty validation command".into()))?;
    let program = target_path(sysroot, Path::new(program))?;
    ensure_existing_beneath(sysroot, &program)?;
    let status = Command::new(program)
        .args(words)
        .env_clear()
        .env("PATH", "/usr/bin:/bin")
        .stdin(Stdio::null())
        .status()?;
    if status.success() {
        Ok(())
    } else {
        Err(SysError::Trigger {
            name: format!("validation in {}", sysroot.display()),
            status,
        })
    }
}

fn ensure_directory_beneath(sysroot: &Path, directory: &Path) -> Result<(), SysError> {
    let relative = directory
        .strip_prefix(sysroot)
        .map_err(|_| SysError::Invalid(format!("path escapes sysroot: {}", directory.display())))?;
    let mut current = sysroot.to_path_buf();
    for component in relative.components() {
        let Component::Normal(value) = component else {
            return Err(SysError::Invalid(format!(
                "unsafe path {}",
                directory.display()
            )));
        };
        current.push(value);
        match fs::symlink_metadata(&current) {
            Ok(metadata) if metadata.file_type().is_symlink() || !metadata.is_dir() => {
                return Err(SysError::Invalid(format!(
                    "unsafe directory {}",
                    current.display()
                )));
            }
            Ok(_) => {}
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => fs::create_dir(&current)?,
            Err(error) => return Err(error.into()),
        }
    }
    Ok(())
}

fn ensure_existing_beneath(sysroot: &Path, path: &Path) -> Result<(), SysError> {
    let root = fs::canonicalize(sysroot)?;
    let resolved = fs::canonicalize(path)?;
    if resolved.starts_with(root) {
        Ok(())
    } else {
        Err(SysError::Invalid(format!(
            "path escapes sysroot: {}",
            path.display()
        )))
    }
}

/// Desired system state from `/etc/sage/system.toml`.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SystemConfig {
    pub schema_version: u32,
    pub system: SystemMetadata,
    #[serde(default)]
    pub providers: BTreeMap<String, String>,
    #[serde(default)]
    pub packages: BTreeSet<String>,
    #[serde(default)]
    pub services: BTreeSet<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SystemMetadata {
    pub architecture: String,
    pub profile: String,
}

impl SystemConfig {
    pub fn load(path: impl AsRef<Path>) -> Result<Self, SysError> {
        let config: Self = toml::from_str(&fs::read_to_string(path)?)?;
        validate_schema(config.schema_version)?;
        Ok(config)
    }
}

/// Minimal transaction needed to converge installed state on the declaration.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct ReconcilePlan {
    pub install: Vec<(sage_core::PackageKey, sage_core::Version)>,
    pub remove: Vec<sage_core::PackageKey>,
    pub provider_bindings: BTreeMap<String, sage_core::PackageKey>,
    pub services: BTreeSet<String>,
}

impl ReconcilePlan {
    /// Solves desired roots, then computes deterministic install/remove differences.
    pub fn compute(
        config: &SystemConfig,
        installed: &[sage_db::InstalledPackage],
        universe: &sage_solver::PackageUniverse,
        no_prune: bool,
    ) -> Result<Self, SysError> {
        let mut desired_names = config.packages.clone();
        desired_names.extend(config.providers.values().cloned());
        let roots: Vec<_> = desired_names
            .iter()
            .map(|name| sage_core::PackageKey::new("main/system", name, sage_core::DEFAULT_SLOT))
            .collect();
        let locks = installed
            .iter()
            .map(|package| (package.key.clone(), package.version.clone()));
        let solution = sage_solver::SageSolver::with_locked(universe, locks).resolve(&roots)?;
        let current: BTreeMap<_, _> = installed
            .iter()
            .map(|package| (package.key.clone(), package.version.clone()))
            .collect();
        let install = solution
            .iter()
            .filter(|(key, version)| current.get(*key) != Some(*version))
            .map(|(key, version)| (key.clone(), version.clone()))
            .collect();
        let remove = if no_prune {
            Vec::new()
        } else {
            current
                .keys()
                .filter(|key| key.channel == "main/system" && !solution.contains_key(*key))
                .cloned()
                .collect()
        };
        let provider_bindings = config
            .providers
            .iter()
            .map(|(interface, package)| {
                (
                    interface.clone(),
                    sage_core::PackageKey::new("main/system", package, sage_core::DEFAULT_SLOT),
                )
            })
            .collect();
        Ok(Self {
            install,
            remove,
            provider_bindings,
            services: config.services.clone(),
        })
    }
}

/// One candidate for a declarative alternatives link.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Alternative {
    pub package: sage_core::PackageKey,
    pub link: PathBuf,
    pub target: PathBuf,
    pub priority: i32,
}

pub struct ProfileEngine;

impl ProfileEngine {
    /// Selects the highest priority candidate for each link and publishes symlinks atomically.
    pub fn apply_alternatives(
        sysroot: &Path,
        alternatives: &[Alternative],
    ) -> Result<BTreeMap<PathBuf, PathBuf>, SysError> {
        let mut selected: BTreeMap<PathBuf, &Alternative> = BTreeMap::new();
        for candidate in alternatives {
            selected
                .entry(candidate.link.clone())
                .and_modify(|current| {
                    if (candidate.priority, &candidate.package)
                        > (current.priority, &current.package)
                    {
                        *current = candidate;
                    }
                })
                .or_insert(candidate);
        }
        let mut active = BTreeMap::new();
        for (link, candidate) in selected {
            atomic_symlink(sysroot, &link, &candidate.target)?;
            active.insert(link, candidate.target.clone());
        }
        Ok(active)
    }

    /// Atomically refreshes an active profile from a complete link map.
    pub fn apply_profile(
        sysroot: &Path,
        profile: &str,
        links: &BTreeMap<PathBuf, PathBuf>,
    ) -> Result<(), SysError> {
        if profile.is_empty()
            || !profile
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_'))
        {
            return Err(SysError::Invalid(format!(
                "invalid profile name '{profile}'"
            )));
        }
        let base = PathBuf::from("etc/sage/profiles").join(profile);
        for (link, target) in links {
            atomic_symlink(sysroot, &base.join(link), target)?;
        }
        Ok(())
    }
}

fn atomic_symlink(sysroot: &Path, declared: &Path, target: &Path) -> Result<(), SysError> {
    if target
        .components()
        .any(|component| component == Component::ParentDir)
    {
        return Err(SysError::Invalid(format!(
            "unsafe symlink target {}",
            target.display()
        )));
    }
    let link = target_path(sysroot, declared)?;
    let parent = link
        .parent()
        .ok_or_else(|| SysError::Invalid("symlink has no parent".into()))?;
    ensure_directory_beneath(sysroot, parent)?;
    let temporary = parent.join(format!(
        ".sage-link-{}-{}",
        std::process::id(),
        TEMP_ID.fetch_add(1, Ordering::Relaxed)
    ));
    std::os::unix::fs::symlink(target, &temporary)?;
    fs::rename(temporary, link)?;
    Ok(())
}
