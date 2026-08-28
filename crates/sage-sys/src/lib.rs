//! Declarative triggers, init-template rendering, profiles, and reconciliation models.

use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Component, Path, PathBuf};
use std::process::Command;
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
    #[serde(default)]
    pub ignore_missing_binary: bool,
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
                let trigger: TriggerSpec = toml::from_str(&fs::read_to_string(file.path())?)?;
                validate_schema(trigger.schema_version)?;
                if trigger.name.is_empty() || trigger.exec.is_empty() || trigger.on_paths.is_empty()
                {
                    return Err(SysError::Invalid(format!(
                        "incomplete trigger {}",
                        file.path().display()
                    )));
                }
                triggers.insert(trigger.name.clone(), trigger);
            }
        }
        let mut triggers: Vec<_> = triggers.into_values().collect();
        triggers.sort_by_key(|trigger| (trigger.priority, trigger.name.clone()));
        Ok(triggers)
    }

    /// Executes each matching trigger at most once in priority order.
    pub fn execute_triggers(
        triggers: &[TriggerSpec],
        modified_paths: &[PathBuf],
        sysroot: &Path,
    ) -> Result<Vec<String>, SysError> {
        let mut executed = Vec::new();
        for trigger in triggers {
            let patterns: Vec<_> = trigger
                .on_paths
                .iter()
                .map(|pattern| {
                    glob::Pattern::new(pattern).map_err(|error| {
                        SysError::Invalid(format!("trigger {}: {error}", trigger.name))
                    })
                })
                .collect::<Result<_, _>>()?;
            if !modified_paths
                .iter()
                .any(|path| patterns.iter().any(|pattern| pattern.matches_path(path)))
            {
                continue;
            }
            let binary = target_path(sysroot, Path::new(&trigger.exec[0]))?;
            if !binary.exists() && trigger.ignore_missing_binary {
                continue;
            }
            ensure_existing_beneath(sysroot, &binary)?;
            let status = Command::new(binary)
                .args(&trigger.exec[1..])
                .current_dir(sysroot)
                .env_clear()
                .env("PATH", "/usr/bin:/bin")
                .env("SAGE_SYSROOT", sysroot)
                .status()?;
            if !status.success() {
                return Err(SysError::Trigger {
                    name: trigger.name.clone(),
                    status,
                });
            }
            executed.push(trigger.name.clone());
        }
        Ok(executed)
    }
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
        let document: ServiceDocument = toml::from_str(&fs::read_to_string(path)?)?;
        validate_schema(document.schema_version)?;
        document.service.validate()?;
        Ok(document.service)
    }

    fn validate(&self) -> Result<(), SysError> {
        if self.name.is_empty() || self.command.is_empty() {
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
        Ok(())
    }
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn administrator_trigger_overrides_vendor_definition() {
        let root = tempfile::tempdir().unwrap();
        for directory in ["usr/share/sage/triggers", "etc/sage/triggers.d"] {
            fs::create_dir_all(root.path().join(directory)).unwrap();
        }
        let trigger = |priority| {
            format!("schema_version=1\nname=\"cache\"\ndescription=\"x\"\non_paths=[\"usr/lib/*\"]\nexec=[\"/missing\"]\npriority={priority}\nignore_missing_binary=true\n")
        };
        fs::write(
            root.path().join("usr/share/sage/triggers/cache.toml"),
            trigger(10),
        )
        .unwrap();
        fs::write(
            root.path().join("etc/sage/triggers.d/cache.toml"),
            trigger(20),
        )
        .unwrap();
        assert_eq!(
            TriggerEngine::load_triggers(root.path()).unwrap()[0].priority,
            20
        );
    }

    #[test]
    fn service_template_renders_atomically() {
        let root = tempfile::tempdir().unwrap();
        let generator = TemplateServiceGenerator {
            target_path_template: "/etc/init/${service.name}".into(),
            mode: 0o755,
            template: "exec ${service.command_json}\n".into(),
            validate_command: None,
        };
        let service = ServiceSpec {
            name: "demo".into(),
            description: "demo".into(),
            command: vec!["/bin/demo".into(), "--run".into()],
            stop_command: vec![],
            reload_command: vec![],
            user: "root".into(),
            group: "root".into(),
            working_dir: "/".into(),
            pid_file: String::new(),
            restart: "always".into(),
            service_type: "simple".into(),
            after: vec![],
            before: vec![],
            runtime: String::new(),
        };
        let path = generator.render_service(&service, root.path()).unwrap();
        assert_eq!(
            fs::read_to_string(path).unwrap(),
            "exec [\"/bin/demo\",\"--run\"]\n"
        );
    }

    #[test]
    fn target_paths_cannot_escape_sysroot() {
        assert!(target_path(Path::new("/root"), Path::new("../../etc/passwd")).is_err());
    }
}
