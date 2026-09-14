//! Declarative service specifications and state documents.

use std::collections::BTreeSet;
use std::fs;
use std::path::{Component, Path};
use std::sync::atomic::Ordering;

use serde::{Deserialize, Serialize};

use super::generator::TemplateServiceGenerator;
use crate::{SysError, TEMP_ID, validate_schema};

/// Init-independent daemon declaration carried by a package.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ServiceSpec {
    /// Output package that owns this declaration when a recipe emits multiple
    /// packages. Empty in installed singleton documents and for the main output.
    #[serde(default)]
    pub package: String,
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
    /// Process activation contract. Boot enablement remains a separate system
    /// policy in `/etc/sage/services.toml`; this field only describes how the
    /// selected init provider or D-Bus reaches the process.
    #[serde(default)]
    pub activation: ServiceActivation,
}

/// Init-independent activation contract attached to one service process.
///
/// `Service` is the backwards-compatible direct-start mode. `Socket` lets the
/// init provider own a listening UNIX socket and start the process on demand.
/// `Dbus` publishes a system-bus activation descriptor and is automatically
/// available after rebuild while its package remains installed, so it is
/// deliberately excluded from the boot enable/disable policy.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "kebab-case", deny_unknown_fields)]
pub enum ServiceActivation {
    #[default]
    Service,
    Socket {
        listen_stream: String,
        #[serde(default)]
        accept: bool,
        #[serde(default = "default_socket_mode")]
        mode: u32,
    },
    Dbus {
        name: String,
        #[serde(default = "default_dbus_bus")]
        bus: String,
        #[serde(default)]
        user: String,
    },
}

fn default_dbus_bus() -> String {
    "system".into()
}

fn default_socket_mode() -> u32 {
    0o666
}

impl ServiceActivation {
    /// Stable key consumed by init rclass activation adapters.
    pub fn kind(&self) -> &'static str {
        match self {
            Self::Service => "service",
            Self::Socket { .. } => "socket",
            Self::Dbus { .. } => "dbus",
        }
    }

    /// D-Bus activation is present whenever the package definition is
    /// rendered; it does not have a boot enablement transition.
    pub fn is_automatic(&self) -> bool {
        matches!(self, Self::Dbus { .. })
    }

    fn validate(&self, service: &ServiceSpec) -> Result<(), SysError> {
        match self {
            Self::Service => Ok(()),
            Self::Socket {
                listen_stream,
                accept,
                mode,
            } => {
                if !Path::new(listen_stream).is_absolute()
                    || listen_stream.contains(['\n', '\r', '\0'])
                {
                    return Err(SysError::Invalid(format!(
                        "service {} socket listen_stream must be an absolute path",
                        service.name
                    )));
                }
                if *accept {
                    return Err(SysError::Invalid(format!(
                        "service {} requests unsupported per-connection socket activation",
                        service.name
                    )));
                }
                if *mode > 0o777 {
                    return Err(SysError::Invalid(format!(
                        "service {} socket mode must contain permission bits only",
                        service.name
                    )));
                }
                Ok(())
            }
            Self::Dbus { name, bus, user } => {
                if bus != "system" {
                    return Err(SysError::Invalid(format!(
                        "service {} only supports system D-Bus activation",
                        service.name
                    )));
                }
                if !valid_dbus_name(name) {
                    return Err(SysError::Invalid(format!(
                        "service {} has invalid D-Bus name {}",
                        service.name, name
                    )));
                }
                if !user.is_empty() && !valid_declaration_name(user) {
                    return Err(SysError::Invalid(format!(
                        "service {} has invalid D-Bus activation user {}",
                        service.name, user
                    )));
                }
                Ok(())
            }
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ServiceDocument {
    pub schema_version: u32,
    #[serde(default)]
    pub service: Option<ServiceSpec>,
    #[serde(default)]
    pub services: Vec<ServiceSpec>,
}

/// Desired service enablement state from `/etc/sage/services.toml`.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ServicesConfig {
    pub schema_version: u32,
    #[serde(alias = "services", default)]
    pub enabled: BTreeSet<String>,
    #[serde(default)]
    pub disabled: BTreeSet<String>,
}

impl ServicesConfig {
    /// Loads service configuration from path, returning an empty default if the file is absent.
    pub fn load(path: impl AsRef<Path>) -> Result<Self, SysError> {
        let path = path.as_ref();
        if !path.exists() {
            return Ok(Self {
                schema_version: sage_core::SCHEMA_VERSION,
                enabled: BTreeSet::new(),
                disabled: BTreeSet::new(),
            });
        }
        let config: Self = toml::from_str(&fs::read_to_string(path)?)?;
        validate_schema(config.schema_version)?;
        config.validate()?;
        Ok(config)
    }

    /// Saves service configuration atomically to the target path.
    pub fn save(&self, path: impl AsRef<Path>) -> Result<(), SysError> {
        self.validate()?;
        let path = path.as_ref();
        let parent = path.parent().ok_or_else(|| {
            SysError::Invalid(format!(
                "services config path has no parent: {}",
                path.display()
            ))
        })?;
        fs::create_dir_all(parent)?;
        let temporary = parent.join(format!(
            ".services-config-{}",
            TEMP_ID.fetch_add(1, Ordering::Relaxed)
        ));
        let content = toml::to_string_pretty(self).map_err(|e| SysError::Invalid(e.to_string()))?;
        fs::write(&temporary, content)?;
        fs::rename(temporary, path)?;
        Ok(())
    }

    fn validate(&self) -> Result<(), SysError> {
        if let Some(name) = self.enabled.intersection(&self.disabled).next() {
            return Err(SysError::Invalid(format!(
                "service {name} cannot be both enabled and disabled"
            )));
        }
        Ok(())
    }
}

/// Last successfully reconciled native-service state. Keeping the generic
/// declarations lets Sage disable and remove stale output after an init switch.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RenderedServicesState {
    pub schema_version: u32,
    pub provider: sage_core::PackageKey,
    /// Renderer inputs must survive a package replacing its own rclass.
    pub generator: TemplateServiceGenerator,
    pub services: Vec<ServiceSpec>,
    #[serde(default)]
    pub enabled: BTreeSet<String>,
}

impl RenderedServicesState {
    pub fn load(path: impl AsRef<Path>) -> Result<Self, SysError> {
        let state: Self = toml::from_str(&fs::read_to_string(path)?)?;
        validate_schema(state.schema_version)?;
        if !valid_declaration_name(&state.provider.name) {
            return Err(SysError::Invalid(
                "invalid rendered-service provider".into(),
            ));
        }
        let mut names = BTreeSet::new();
        for service in &state.services {
            service.validate()?;
            if !names.insert(&service.name) {
                return Err(SysError::Invalid(format!(
                    "duplicate rendered service {}",
                    service.name
                )));
            }
        }
        if let Some(name) = state.enabled.iter().find(|name| !names.contains(*name)) {
            return Err(SysError::Invalid(format!(
                "enabled service {name} has no rendered definition"
            )));
        }
        if let Some(service) = state.services.iter().find(|service| {
            service.activation.is_automatic() && state.enabled.contains(&service.name)
        }) {
            return Err(SysError::Invalid(format!(
                "automatic {} service {} cannot be boot-enabled",
                service.activation.kind(),
                service.name
            )));
        }
        Ok(state)
    }

    /// Saves the last reconciled native-service state atomically.
    pub fn save(&self, path: impl AsRef<Path>) -> Result<(), SysError> {
        let path = path.as_ref();
        let parent = path.parent().ok_or_else(|| {
            SysError::Invalid(format!(
                "rendered services path has no parent: {}",
                path.display()
            ))
        })?;
        fs::create_dir_all(parent)?;
        let temporary = parent.join(format!(
            ".rendered-services-{}",
            TEMP_ID.fetch_add(1, Ordering::Relaxed)
        ));
        let content = toml::to_string_pretty(self).map_err(|e| SysError::Invalid(e.to_string()))?;
        fs::write(&temporary, content)?;
        fs::rename(temporary, path)?;
        Ok(())
    }
}

impl ServiceSpec {
    pub fn load(path: impl AsRef<Path>) -> Result<Self, SysError> {
        Self::parse(&fs::read(path)?)
    }

    pub fn parse(bytes: &[u8]) -> Result<Self, SysError> {
        let mut services = ServiceDocument::parse(bytes)?.into_services();
        if services.len() != 1 {
            return Err(SysError::Invalid(
                "installed service document must contain exactly one service".into(),
            ));
        }
        Ok(services.remove(0))
    }

    pub(crate) fn validate(&self) -> Result<(), SysError> {
        if !self.package.is_empty() && !valid_declaration_name(&self.package) {
            return Err(SysError::Invalid(format!(
                "invalid owning package for service {}",
                self.name
            )));
        }
        if !valid_declaration_name(&self.name) || self.command.is_empty() {
            return Err(SysError::Invalid(
                "service name and command are required".into(),
            ));
        }
        if !valid_declaration_name(&self.user) || !valid_declaration_name(&self.group) {
            return Err(SysError::Invalid(format!(
                "service {} requires valid user and group names",
                self.name
            )));
        }
        if !["always", "on-failure", "no"].contains(&self.restart.as_str())
            || !["simple", "forking", "notify", "oneshot"].contains(&self.service_type.as_str())
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
        validate_service_command("command", &self.command, &self.name)?;
        if !self.stop_command.is_empty() {
            validate_service_command("stop_command", &self.stop_command, &self.name)?;
        }
        if !self.reload_command.is_empty() {
            validate_service_command("reload_command", &self.reload_command, &self.name)?;
        }
        if !Path::new(&self.working_dir).is_absolute()
            || (!self.pid_file.is_empty() && !Path::new(&self.pid_file).is_absolute())
        {
            return Err(SysError::Invalid(format!(
                "service {} paths must be absolute",
                self.name
            )));
        }
        let mut edges = BTreeSet::new();
        for dependency in self.after.iter().chain(&self.before) {
            if !valid_declaration_name(dependency) || !edges.insert(dependency) {
                return Err(SysError::Invalid(format!(
                    "service {} has an invalid or duplicate dependency {}",
                    self.name, dependency
                )));
            }
        }
        self.activation.validate(self)?;
        Ok(())
    }
}

fn valid_dbus_name(name: &str) -> bool {
    name.len() <= 255
        && name.contains('.')
        && !name.starts_with('.')
        && !name.ends_with('.')
        && name.split('.').all(|part| {
            !part.is_empty()
                && !part.as_bytes()[0].is_ascii_digit()
                && part
                    .bytes()
                    .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-'))
        })
}

fn validate_service_command(
    field: &str,
    command: &[String],
    service: &str,
) -> Result<(), SysError> {
    let executable = Path::new(&command[0]);
    if !executable.is_absolute()
        || matches!(
            executable.components().nth(1),
            Some(Component::Normal(value))
                if matches!(value.to_str(), Some("bin" | "sbin" | "lib" | "lib64"))
        )
    {
        return Err(SysError::Invalid(format!(
            "service {service} {field} must use an absolute usr-merged executable"
        )));
    }
    Ok(())
}

impl ServiceDocument {
    pub fn load(path: impl AsRef<Path>) -> Result<Self, SysError> {
        Self::parse(&fs::read(path)?)
    }

    pub fn parse(bytes: &[u8]) -> Result<Self, SysError> {
        let text = std::str::from_utf8(bytes)
            .map_err(|_| SysError::Invalid("service document is not UTF-8".into()))?;
        let document: Self = toml::from_str(text)?;
        validate_schema(document.schema_version)?;
        if document.services().next().is_none() {
            return Err(SysError::Invalid(
                "service document must contain at least one service".into(),
            ));
        }
        let mut names = BTreeSet::new();
        for service in document.services() {
            service.validate()?;
            if !names.insert(&service.name) {
                return Err(SysError::Invalid(format!(
                    "duplicate service name {}",
                    service.name
                )));
            }
        }
        Ok(document)
    }

    pub fn services(&self) -> impl Iterator<Item = &ServiceSpec> {
        self.service.iter().chain(self.services.iter())
    }

    pub fn into_services(self) -> Vec<ServiceSpec> {
        self.service.into_iter().chain(self.services).collect()
    }

    pub fn for_package(&self, package: &str, main_package: &str) -> Self {
        let services = self
            .services()
            .filter(|service| {
                service.package == package
                    || (service.package.is_empty() && package == main_package)
            })
            .cloned()
            .collect();
        Self {
            schema_version: self.schema_version,
            service: None,
            services,
        }
    }

    pub fn validate_output_packages(&self, outputs: &BTreeSet<String>) -> Result<(), SysError> {
        if let Some(service) = self
            .services()
            .find(|service| !service.package.is_empty() && !outputs.contains(&service.package))
        {
            return Err(SysError::Invalid(format!(
                "service {} names unknown output package {}",
                service.name, service.package
            )));
        }
        Ok(())
    }
}

pub(crate) fn valid_declaration_name(name: &str) -> bool {
    !name.is_empty()
        && name
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-' | b'.'))
}
