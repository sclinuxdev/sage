use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Component, Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::Ordering;

use serde::{Deserialize, Serialize};

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
        Ok(config)
    }

    /// Saves service configuration atomically to the target path.
    pub fn save(&self, path: impl AsRef<Path>) -> Result<(), SysError> {
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
        Ok(state)
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

    fn validate(&self) -> Result<(), SysError> {
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
        Ok(())
    }
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

#[derive(Debug, Deserialize)]
struct InitRclass {
    schema_version: u32,
    service_generator: TemplateServiceGenerator,
}

/// Generic target/template pair loaded from an `init-*.toml` rclass.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct TemplateServiceGenerator {
    #[serde(rename = "target_path")]
    pub target_path_template: String,
    pub mode: u32,
    pub template: String,
    /// Provider-owned translations from init-independent dependency names to
    /// native dependency identifiers (for example `network` to a target).
    #[serde(default)]
    pub dependency_aliases: BTreeMap<String, String>,
    /// Provider-owned suffix for service dependencies that have no explicit
    /// alias or native-looking extension.
    #[serde(default)]
    pub service_dependency_suffix: String,
    /// Service process types implemented by this provider. An empty list keeps
    /// compatibility with older provider classes that accepted every type.
    #[serde(default)]
    pub supported_types: Vec<String>,
    /// Optional argv adapter that compiles the generic template into the
    /// provider-native output. `${INPUT}` and `${OUTPUT}` are temporary files.
    #[serde(default)]
    pub compile_command: Vec<String>,
    /// Provider-owned directory replaced as one validated generation. This is
    /// used by managers such as Loom whose complete service graph is generated.
    #[serde(default)]
    pub managed_directory: Option<String>,
    pub validate_command: Option<String>,
    #[serde(alias = "enable_cmd")]
    pub enable_command: Option<String>,
    #[serde(alias = "disable_cmd")]
    pub disable_command: Option<String>,
    #[serde(alias = "is_enabled_cmd", default)]
    pub is_enabled_command: Option<String>,
}

impl TemplateServiceGenerator {
    pub fn from_rclass(path: &Path) -> Result<Self, SysError> {
        Self::parse(&fs::read(path)?)
    }

    /// Parses a renderer before package publication; rejects invalid UTF-8,
    /// malformed TOML, missing renderer fields, and unsupported schema versions.
    pub fn parse(bytes: &[u8]) -> Result<Self, SysError> {
        let text = std::str::from_utf8(bytes)
            .map_err(|error| SysError::Invalid(format!("init rclass is not UTF-8: {error}")))?;
        let class: InitRclass = toml::from_str(text)?;
        validate_schema(class.schema_version)?;
        Ok(class.service_generator)
    }

    /// Renders and atomically publishes a service definition under `sysroot`.
    pub fn render_service(
        &self,
        service: &ServiceSpec,
        sysroot: &Path,
    ) -> Result<PathBuf, SysError> {
        let target = self.render_service_unvalidated(service, sysroot)?;
        self.validate_rendered_services(service, sysroot)?;
        Ok(target)
    }

    /// Publishes one definition while deferring provider-wide validation. This
    /// lets reconciliation render a complete dependency graph before checking it.
    pub fn render_service_unvalidated(
        &self,
        service: &ServiceSpec,
        sysroot: &Path,
    ) -> Result<PathBuf, SysError> {
        service.validate()?;
        self.validate_service_type(service)?;
        let target = self.rendered_path(service, sysroot)?;
        self.render_service_to(service, sysroot, &target)?;
        Ok(target)
    }

    fn render_service_to(
        &self,
        service: &ServiceSpec,
        sysroot: &Path,
        target: &Path,
    ) -> Result<(), SysError> {
        let variables = self.service_variables(service, sysroot)?;
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
        if self.compile_command.is_empty() {
            let mut options = fs::OpenOptions::new();
            options.write(true).create_new(true);
            std::io::Write::write_all(&mut options.open(&temporary)?, rendered.as_bytes())?;
        } else {
            let input = temporary.with_extension("input.toml");
            fs::write(&input, rendered)?;
            let mut compile_variables = variables.clone();
            compile_variables.insert("INPUT".into(), input.display().to_string());
            compile_variables.insert("OUTPUT".into(), temporary.display().to_string());
            let result = run_argv_template(&self.compile_command, &compile_variables, sysroot);
            let _ = fs::remove_file(input);
            result?;
            if !temporary.is_file() {
                return Err(SysError::Invalid(format!(
                    "service compiler did not create {}",
                    temporary.display()
                )));
            }
        }
        fs::set_permissions(&temporary, fs::Permissions::from_mode(self.mode))?;
        fs::rename(&temporary, target)?;
        Ok(())
    }

    /// Renders a complete provider generation and validates it before keeping
    /// the new tree. Managed directories are swapped and rolled back as a unit.
    pub fn render_service_set(
        &self,
        services: &[ServiceSpec],
        sysroot: &Path,
    ) -> Result<(), SysError> {
        let Some(directory) = &self.managed_directory else {
            for service in services {
                self.render_service_unvalidated(service, sysroot)?;
            }
            if let Some(service) = services.first() {
                self.validate_rendered_services(service, sysroot)?;
            }
            return Ok(());
        };
        let target_directory = target_path(sysroot, Path::new(directory))?;
        let parent = target_directory
            .parent()
            .ok_or_else(|| SysError::Invalid("managed service directory has no parent".into()))?;
        ensure_directory_beneath(sysroot, parent)?;
        let generation = TEMP_ID.fetch_add(1, Ordering::Relaxed);
        let leaf = target_directory
            .file_name()
            .and_then(|value| value.to_str())
            .ok_or_else(|| SysError::Invalid("invalid managed service directory".into()))?;
        let staging = parent.join(format!(
            ".{leaf}.sage-stage-{}-{generation}",
            std::process::id()
        ));
        let backup = parent.join(format!(
            ".{leaf}.sage-backup-{}-{generation}",
            std::process::id()
        ));
        fs::create_dir(&staging)?;
        let staged = (|| {
            for service in services {
                service.validate()?;
                self.validate_service_type(service)?;
                let target = self.rendered_path(service, sysroot)?;
                if target.parent() != Some(target_directory.as_path()) {
                    return Err(SysError::Invalid(format!(
                        "service {} renders outside managed directory {}",
                        service.name,
                        target_directory.display()
                    )));
                }
                self.render_service_to(
                    service,
                    sysroot,
                    &staging.join(target.file_name().ok_or_else(|| {
                        SysError::Invalid("rendered service target has no filename".into())
                    })?),
                )?;
            }
            Ok::<_, SysError>(())
        })();
        if let Err(error) = staged {
            let _ = fs::remove_dir_all(&staging);
            return Err(error);
        }
        let had_previous = target_directory.exists();
        if had_previous {
            fs::rename(&target_directory, &backup)?;
        }
        if let Err(error) = fs::rename(&staging, &target_directory) {
            if had_previous {
                let _ = fs::rename(&backup, &target_directory);
            }
            return Err(error.into());
        }
        if let Some(service) = services.first()
            && let Err(error) = self.validate_rendered_services(service, sysroot)
        {
            let rejected = parent.join(format!(
                ".{leaf}.sage-rejected-{}-{generation}",
                std::process::id()
            ));
            let _ = fs::rename(&target_directory, &rejected);
            if had_previous {
                let _ = fs::rename(&backup, &target_directory);
            }
            let _ = fs::remove_dir_all(rejected);
            return Err(error);
        }
        if had_previous {
            fs::remove_dir_all(backup)?;
        }
        Ok(())
    }

    /// Validates a complete provider generation without writing files or
    /// executing provider commands. Package publication uses this pass before
    /// it creates a recovery journal, so an invalid target, template, or
    /// service type cannot strand a transaction after the package database has
    /// changed.
    pub fn validate_service_set(
        &self,
        services: &[ServiceSpec],
        sysroot: &Path,
    ) -> Result<(), SysError> {
        let managed_directory = self
            .managed_directory
            .as_deref()
            .map(|directory| target_path(sysroot, Path::new(directory)))
            .transpose()?;
        for service in services {
            service.validate()?;
            self.validate_service_type(service)?;
            let variables = self.service_variables(service, sysroot)?;
            expand_template(&self.template, &variables)?;
            let target = self.rendered_path(service, sysroot)?;
            if let Some(directory) = &managed_directory
                && target.parent() != Some(directory.as_path())
            {
                return Err(SysError::Invalid(format!(
                    "service {} renders outside managed directory {}",
                    service.name,
                    directory.display()
                )));
            }
            self.validate_compile_command(&variables, sysroot)?;
            for (kind, command) in [
                ("validate", self.validate_command.as_deref()),
                ("enable", self.enable_command.as_deref()),
                ("disable", self.disable_command.as_deref()),
                ("is-enabled", self.is_enabled_command.as_deref()),
            ] {
                if let Some(command) = command {
                    self.validate_command_path(kind, command, &variables, sysroot)?;
                }
            }
        }
        Ok(())
    }

    /// Returns the normalized provider programs needed by this generation.
    /// Template and path errors are returned without executing any command.
    pub fn required_programs(
        &self,
        services: &[ServiceSpec],
        enabled: &BTreeSet<String>,
        sysroot: &Path,
    ) -> Result<BTreeSet<PathBuf>, SysError> {
        let mut programs = BTreeSet::new();
        for service in services {
            let variables = self.service_variables(service, sysroot)?;
            if let Some(program) = self.validate_compile_command(&variables, sysroot)? {
                programs.insert(program);
            }
            for (kind, command) in [
                ("validate", self.validate_command.as_deref()),
                (
                    "enable",
                    self.enable_command
                        .as_deref()
                        .filter(|_| enabled.contains(&service.name)),
                ),
                // Only enabled services need an eventual disable command.
                (
                    "disable",
                    self.disable_command
                        .as_deref()
                        .filter(|_| enabled.contains(&service.name)),
                ),
            ] {
                if let Some(command) = command {
                    programs
                        .insert(self.validate_command_path(kind, command, &variables, sysroot)?);
                }
            }
        }
        Ok(programs)
    }

    /// Returns the old generation's cleanup program without executing it.
    /// Invalid templates and paths are returned as errors.
    pub fn disable_program(
        &self,
        service: &ServiceSpec,
        sysroot: &Path,
    ) -> Result<Option<PathBuf>, SysError> {
        self.disable_command
            .as_deref()
            .map(|command| {
                self.validate_command_path(
                    "disable",
                    command,
                    &self.service_variables(service, sysroot)?,
                    sysroot,
                )
            })
            .transpose()
    }

    /// Runs the provider's whole-tree validator after all definitions exist.
    pub fn validate_rendered_services(
        &self,
        service: &ServiceSpec,
        sysroot: &Path,
    ) -> Result<(), SysError> {
        if let Some(command) = &self.validate_command {
            run_validation(
                &expand_template(command, &self.service_variables(service, sysroot)?)?,
                sysroot,
            )?;
        }
        Ok(())
    }

    /// Executes the init class's generic enable action, when it declares one.
    pub fn enable_service(&self, service: &ServiceSpec, sysroot: &Path) -> Result<(), SysError> {
        self.validate_service_type(service)?;
        if let Some(command) = &self.enable_command {
            run_validation(
                &expand_template(command, &self.service_variables(service, sysroot)?)?,
                sysroot,
            )?;
        }
        Ok(())
    }

    /// Executes the provider's offline disable action, when present.
    pub fn disable_service(&self, service: &ServiceSpec, sysroot: &Path) -> Result<(), SysError> {
        self.validate_service_type(service)?;
        if let Some(command) = &self.disable_command {
            run_validation(
                &expand_template(command, &self.service_variables(service, sysroot)?)?,
                sysroot,
            )?;
        }
        Ok(())
    }

    /// Checks whether a service is enabled in the host init system.
    /// Returns `Ok(Some(true))` if enabled, `Ok(Some(false))` if disabled,
    /// or `Ok(None)` if no check command is declared or executable is missing.
    pub fn is_service_enabled(
        &self,
        service: &ServiceSpec,
        sysroot: &Path,
    ) -> Result<Option<bool>, SysError> {
        let Some(command) = &self.is_enabled_command else {
            return Ok(None);
        };
        let variables = self.service_variables(service, sysroot)?;
        let expanded = expand_template(command, &variables)?;
        let mut words = expanded.split_whitespace();
        let Some(program_str) = words.next() else {
            return Ok(None);
        };
        let program = match target_path(sysroot, Path::new(program_str)) {
            Ok(p) => p,
            Err(_) => return Ok(None),
        };
        if !program.exists() {
            return Ok(None);
        }
        let root = match fs::canonicalize(sysroot) {
            Ok(r) => r,
            Err(_) => return Ok(None),
        };
        let resolved = match fs::canonicalize(&program) {
            Ok(p) => p,
            Err(_) => return Ok(None),
        };
        if !resolved.starts_with(&root) {
            return Ok(None);
        }
        let status = Command::new(&resolved)
            .args(words)
            .env_clear()
            .env("PATH", "/usr/bin")
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .status();
        match status {
            Ok(status) => Ok(Some(status.success())),
            Err(_) => Ok(None),
        }
    }

    /// Resolves the provider-owned native file for a generic service.
    pub fn rendered_path(
        &self,
        service: &ServiceSpec,
        sysroot: &Path,
    ) -> Result<PathBuf, SysError> {
        let relative = expand_template(
            &self.target_path_template,
            &self.service_variables(service, sysroot)?,
        )?;
        target_path(sysroot, Path::new(&relative))
    }

    /// Removes a previously rendered provider file without following links.
    pub fn remove_service(&self, service: &ServiceSpec, sysroot: &Path) -> Result<(), SysError> {
        let path = self.rendered_path(service, sysroot)?;
        match fs::symlink_metadata(&path) {
            Ok(metadata) if metadata.file_type().is_file() || metadata.file_type().is_symlink() => {
                fs::remove_file(path)?;
            }
            Ok(_) => {
                return Err(SysError::Invalid(format!(
                    "rendered service target is not a file: {}",
                    path.display()
                )));
            }
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error) => return Err(error.into()),
        }
        Ok(())
    }

    fn validate_service_type(&self, service: &ServiceSpec) -> Result<(), SysError> {
        if self.supported_types.is_empty()
            || self
                .supported_types
                .iter()
                .any(|value| value == &service.service_type)
        {
            Ok(())
        } else {
            Err(SysError::Invalid(format!(
                "init provider does not support service type {} for {}",
                service.service_type, service.name
            )))
        }
    }

    fn validate_compile_command(
        &self,
        variables: &BTreeMap<String, String>,
        sysroot: &Path,
    ) -> Result<Option<PathBuf>, SysError> {
        let Some((program, arguments)) = self.compile_command.split_first() else {
            return Ok(None);
        };
        let mut variables = variables.clone();
        variables.insert(
            "INPUT".into(),
            sysroot
                .join("var/lib/sage/.sage-preflight-input")
                .display()
                .to_string(),
        );
        variables.insert(
            "OUTPUT".into(),
            sysroot
                .join("var/lib/sage/.sage-preflight-output")
                .display()
                .to_string(),
        );
        let program = expand_template(program, &variables)?;
        let program = target_path(sysroot, Path::new(&program))?;
        for argument in arguments {
            expand_template(argument, &variables)?;
        }
        Ok(Some(program))
    }

    fn validate_command_path(
        &self,
        kind: &str,
        command: &str,
        variables: &BTreeMap<String, String>,
        sysroot: &Path,
    ) -> Result<PathBuf, SysError> {
        let command = expand_template(command, variables)?;
        let program = command
            .split_whitespace()
            .next()
            .ok_or_else(|| SysError::Invalid(format!("empty {kind} command")))?;
        target_path(sysroot, Path::new(program))
            .map_err(|error| SysError::Invalid(format!("invalid {kind} command program: {error}")))
    }

    fn service_variables(
        &self,
        service: &ServiceSpec,
        sysroot: &Path,
    ) -> Result<BTreeMap<String, String>, SysError> {
        service_variables(
            service,
            sysroot,
            &self.dependency_aliases,
            &self.service_dependency_suffix,
        )
    }
}

fn json_quote_str(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\x08' => out.push_str("\\b"),
            '\x0C' => out.push_str("\\f"),
            c if c < ' ' => {
                use std::fmt::Write;
                let _ = write!(out, "\\u{:04x}", c as u32);
            }
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

fn json_quote_array<'a>(items: impl IntoIterator<Item = &'a String>) -> String {
    let mut out = String::from("[");
    let mut first = true;
    for item in items {
        if !first {
            out.push(',');
        }
        first = false;
        out.push_str(&json_quote_str(item));
    }
    out.push(']');
    out
}

fn service_variables(
    service: &ServiceSpec,
    sysroot: &Path,
    dependency_aliases: &BTreeMap<String, String>,
    service_dependency_suffix: &str,
) -> Result<BTreeMap<String, String>, SysError> {
    let command_tail = service.command.get(1..).unwrap_or_default();
    let after = map_service_dependencies(
        &service.after,
        dependency_aliases,
        service_dependency_suffix,
    );
    let before = map_service_dependencies(
        &service.before,
        dependency_aliases,
        service_dependency_suffix,
    );
    Ok(BTreeMap::from([
        ("service.name".into(), service.name.clone()),
        ("service.description".into(), service.description.clone()),
        (
            "service.description_json".into(),
            json_quote_str(&service.description),
        ),
        ("service.command[0]".into(), service.command[0].clone()),
        ("service.command[1:]".into(), command_tail.join(" ")),
        ("service.command_str".into(), service.command.join(" ")),
        (
            "service.command_quoted".into(),
            quote_command(&service.command)?,
        ),
        (
            "service.command_json".into(),
            json_quote_array(&service.command),
        ),
        (
            "service.stop_command_str".into(),
            service.stop_command.join(" "),
        ),
        (
            "service.stop_command_quoted".into(),
            quote_command(&service.stop_command)?,
        ),
        (
            "service.stop_command_json".into(),
            json_quote_array(&service.stop_command),
        ),
        (
            "service.stop_action_toml".into(),
            if service.stop_command.is_empty() {
                String::new()
            } else {
                format!("stop = {}", json_quote_array(&service.stop_command))
            },
        ),
        (
            "service.stop_input_toml".into(),
            if service.stop_command.is_empty() {
                String::new()
            } else {
                format!("stop_command = {}", json_quote_array(&service.stop_command))
            },
        ),
        (
            "service.reload_command_str".into(),
            service.reload_command.join(" "),
        ),
        (
            "service.reload_command_quoted".into(),
            quote_command(&service.reload_command)?,
        ),
        (
            "service.reload_command_json".into(),
            json_quote_array(&service.reload_command),
        ),
        (
            "service.reload_action_toml".into(),
            if service.reload_command.is_empty() {
                String::new()
            } else {
                format!("reload = {}", json_quote_array(&service.reload_command))
            },
        ),
        (
            "service.reload_input_toml".into(),
            if service.reload_command.is_empty() {
                String::new()
            } else {
                format!(
                    "reload_command = {}",
                    json_quote_array(&service.reload_command)
                )
            },
        ),
        ("service.user".into(), service.user.clone()),
        ("service.user_json".into(), json_quote_str(&service.user)),
        ("service.group".into(), service.group.clone()),
        ("service.group_json".into(), json_quote_str(&service.group)),
        ("service.working_dir".into(), service.working_dir.clone()),
        (
            "service.working_dir_json".into(),
            json_quote_str(&service.working_dir),
        ),
        ("service.pid_file".into(), service.pid_file.clone()),
        ("service.restart".into(), service.restart.clone()),
        ("service.type".into(), service.service_type.clone()),
        (
            "service.process_type".into(),
            if service.service_type == "oneshot" {
                "oneshot".into()
            } else {
                "simple".into()
            },
        ),
        (
            "service.readiness".into(),
            if service.service_type == "notify" {
                "notify".into()
            } else {
                "exec".into()
            },
        ),
        ("service.after".into(), after.join(" ")),
        ("service.after_space".into(), after.join(" ")),
        ("service.after_json".into(), json_quote_array(&after)),
        ("service.before".into(), before.join(" ")),
        ("service.before_space".into(), before.join(" ")),
        ("service.before_json".into(), json_quote_array(&before)),
        ("service.runtime".into(), service.runtime.clone()),
        (
            "service.runtime_json".into(),
            json_quote_str(&service.runtime),
        ),
        ("SYSROOT".into(), sysroot.display().to_string()),
    ]))
}

fn map_service_dependencies(
    dependencies: &[String],
    aliases: &BTreeMap<String, String>,
    service_suffix: &str,
) -> Vec<String> {
    dependencies
        .iter()
        .map(|dependency| {
            aliases.get(dependency).cloned().unwrap_or_else(|| {
                if !service_suffix.is_empty() && !dependency.contains('.') {
                    format!("{dependency}{service_suffix}")
                } else {
                    dependency.clone()
                }
            })
        })
        .collect()
}

fn quote_command(command: &[String]) -> Result<String, SysError> {
    Ok(command
        .iter()
        .map(|argument| json_quote_str(argument))
        .collect::<Vec<_>>()
        .join(" "))
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

pub(crate) fn target_path(sysroot: &Path, declared: &Path) -> Result<PathBuf, SysError> {
    let mut relative = PathBuf::new();
    for component in declared.components() {
        match component {
            Component::RootDir | Component::CurDir => {}
            Component::Normal(value) => relative.push(value),
            _ => {
                return Err(SysError::Invalid(format!(
                    "unsafe target path {}",
                    declared.display()
                )));
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
        .env("PATH", "/usr/bin")
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

fn run_argv_template(
    command: &[String],
    variables: &BTreeMap<String, String>,
    sysroot: &Path,
) -> Result<(), SysError> {
    let (program, arguments) = command
        .split_first()
        .ok_or_else(|| SysError::Invalid("empty service compiler command".into()))?;
    let program = expand_template(program, variables)?;
    let program = target_path(sysroot, Path::new(&program))?;
    ensure_existing_beneath(sysroot, &program)?;
    let arguments = arguments
        .iter()
        .map(|argument| expand_template(argument, variables))
        .collect::<Result<Vec<_>, _>>()?;
    let status = Command::new(program)
        .args(arguments)
        .env_clear()
        .env("PATH", "/usr/bin")
        .stdin(Stdio::null())
        .status()?;
    if status.success() {
        Ok(())
    } else {
        Err(SysError::Trigger {
            name: format!("service compiler in {}", sysroot.display()),
            status,
        })
    }
}

pub(crate) fn ensure_directory_beneath(sysroot: &Path, directory: &Path) -> Result<(), SysError> {
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

pub(crate) fn ensure_existing_beneath(sysroot: &Path, path: &Path) -> Result<(), SysError> {
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

/// Detected drift between actual init state and Sage declarative management.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ServiceDrift {
    pub service: String,
    pub provider: String,
    pub init_state: String,
    pub sage_state: String,
}

/// Information about a known service's management status.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ServiceStatusInfo {
    pub name: String,
    pub package: String,
    pub state: String,
    pub provider: String,
}

/// Scans installed services for drift where a service is enabled in the host init
/// system but is not managed in `/etc/sage/services.toml`.
pub fn detect_service_drift(
    root: &Path,
    generator: &TemplateServiceGenerator,
    provider_name: &str,
    services: &[ServiceSpec],
    enabled: &BTreeSet<String>,
) -> Vec<ServiceDrift> {
    let mut drifts = Vec::new();
    for service in services {
        if enabled.contains(&service.name) {
            continue;
        }
        if let Ok(Some(true)) = generator.is_service_enabled(service, root) {
            drifts.push(ServiceDrift {
                service: service.name.clone(),
                provider: provider_name.to_string(),
                init_state: "enabled".to_string(),
                sage_state: "unmanaged".to_string(),
            });
        }
    }
    drifts
}

/// Emits human-friendly warning and action hint when drift is detected.
pub fn warn_service_drift(
    drifts: &[ServiceDrift],
    generator: Option<&TemplateServiceGenerator>,
    root: &Path,
) {
    for drift in drifts {
        println!(
            "warning: service '{}' is enabled outside Sage",
            drift.service
        );
        println!("         {}: {}", drift.provider, drift.init_state);
        println!("         services.toml: {}", drift.sage_state);
        println!("Hint:");
        println!("  sage service adopt {}", drift.service);
        if drift.provider == "systemd" {
            println!("  systemctl disable {}", drift.service);
        } else if let Some(generator_inst) = generator
            && let Some(cmd) = &generator_inst.disable_command
        {
            let hint = cmd
                .replace("${service.name}", &drift.service)
                .replace("${SYSROOT}", &root.display().to_string());
            println!("  {hint}");
        }
    }
}

/// Loads all available service specifications from `/usr/share/sage/services/`
/// and the active rendered services state.
pub fn load_available_services(root: &Path) -> Result<Vec<ServiceSpec>, SysError> {
    let mut services_map = BTreeMap::new();
    let rendered_path = root.join("var/lib/sage/rendered-services.toml");
    if rendered_path.exists()
        && let Ok(rendered) = RenderedServicesState::load(&rendered_path)
    {
        for svc in rendered.services {
            services_map.insert(svc.name.clone(), svc);
        }
    }
    let services_dir = root.join("usr/share/sage/services");
    if services_dir.exists()
        && let Ok(entries) = fs::read_dir(&services_dir)
    {
        for entry in entries.flatten() {
            let path = entry.path();
            if path.extension().is_some_and(|ext| ext == "toml")
                && let Ok(bytes) = fs::read(&path)
                && let Ok(doc) = ServiceDocument::parse(&bytes)
            {
                for svc in doc.into_services() {
                    services_map.entry(svc.name.clone()).or_insert(svc);
                }
            }
        }
    }
    Ok(services_map.into_values().collect())
}

/// Loads the active init provider name and generator.
pub fn load_active_generator(root: &Path) -> Result<(String, TemplateServiceGenerator), SysError> {
    let rendered_path = root.join("var/lib/sage/rendered-services.toml");
    if rendered_path.exists()
        && let Ok(rendered) = RenderedServicesState::load(&rendered_path)
    {
        return Ok((rendered.provider.name, rendered.generator));
    }
    let system_config_path = root.join("etc/sage/system.toml");
    let provider_name = if system_config_path.exists() {
        let content = fs::read_to_string(&system_config_path)?;
        let toml_val: toml::Value =
            toml::from_str(&content).map_err(|e| SysError::Invalid(e.to_string()))?;
        toml_val
            .get("providers")
            .and_then(|p| p.get("init"))
            .and_then(|i| i.as_str())
            .unwrap_or("systemd")
            .to_string()
    } else {
        "systemd".to_string()
    };
    let rclass_path = root.join(format!("usr/share/sage/rclass/init-{provider_name}.toml"));
    if rclass_path.exists() {
        let generator = TemplateServiceGenerator::from_rclass(&rclass_path)?;
        return Ok((provider_name, generator));
    }
    let workspace_rclass = Path::new("rclass").join(format!("init-{provider_name}.toml"));
    if workspace_rclass.exists() {
        let generator = TemplateServiceGenerator::from_rclass(&workspace_rclass)?;
        return Ok((provider_name, generator));
    }
    Err(SysError::Invalid(format!(
        "could not find init generator for provider '{provider_name}'"
    )))
}

/// Enables a service in `/etc/sage/services.toml` and activates it in the init provider.
pub fn service_enable(root: &Path, service_name: &str, dry_run: bool) -> Result<(), SysError> {
    let services = load_available_services(root)?;
    let spec = services
        .into_iter()
        .find(|s| s.name == service_name)
        .ok_or_else(|| {
            SysError::Invalid(format!(
                "service '{service_name}' not found in installed packages"
            ))
        })?;
    let config_path = root.join("etc/sage/services.toml");
    let mut config = ServicesConfig::load(&config_path)?;
    if config.enabled.contains(service_name) {
        println!("Service '{service_name}' is already enabled in services.toml");
    } else if !dry_run {
        config.enabled.insert(service_name.to_string());
        config.disabled.remove(service_name);
        config.save(&config_path)?;
    }

    if let Ok((_provider, generator)) = load_active_generator(root)
        && !dry_run
    {
        let _ = generator.render_service(&spec, root);
        generator.enable_service(&spec, root)?;
        let rendered_path = root.join("var/lib/sage/rendered-services.toml");
        if rendered_path.exists()
            && let Ok(mut rendered) = RenderedServicesState::load(&rendered_path)
        {
            rendered.enabled.insert(service_name.to_string());
            if !rendered.services.iter().any(|s| s.name == spec.name) {
                rendered.services.push(spec);
            }
            let _ = fs::write(
                &rendered_path,
                toml::to_string_pretty(&rendered).map_err(|e| SysError::Invalid(e.to_string()))?,
            );
        }
    }
    println!("Enabled service '{service_name}' (managed-enabled)");
    Ok(())
}

/// Disables a service in `/etc/sage/services.toml` and deactivates it in the init provider.
pub fn service_disable(root: &Path, service_name: &str, dry_run: bool) -> Result<(), SysError> {
    let config_path = root.join("etc/sage/services.toml");
    let mut config = ServicesConfig::load(&config_path)?;
    if !config.enabled.contains(service_name) && config.disabled.contains(service_name) {
        println!("Service '{service_name}' was not enabled in services.toml");
    } else if !dry_run {
        config.enabled.remove(service_name);
        config.disabled.insert(service_name.to_string());
        config.save(&config_path)?;
    }

    let services = load_available_services(root)?;
    if let Some(spec) = services.into_iter().find(|s| s.name == service_name)
        && let Ok((_provider, generator)) = load_active_generator(root)
        && !dry_run
    {
        let _ = generator.disable_service(&spec, root);
        let rendered_path = root.join("var/lib/sage/rendered-services.toml");
        if rendered_path.exists()
            && let Ok(mut rendered) = RenderedServicesState::load(&rendered_path)
        {
            rendered.enabled.remove(service_name);
            let _ = fs::write(
                &rendered_path,
                toml::to_string_pretty(&rendered).map_err(|e| SysError::Invalid(e.to_string()))?,
            );
        }
    }
    println!("Disabled service '{service_name}' (managed-disabled)");
    Ok(())
}

/// Adopts an externally enabled service into `/etc/sage/services.toml` without modifying host state.
pub fn service_adopt(root: &Path, service_name: &str, dry_run: bool) -> Result<(), SysError> {
    let services = load_available_services(root)?;
    let spec = services
        .into_iter()
        .find(|s| s.name == service_name)
        .ok_or_else(|| {
            SysError::Invalid(format!(
                "service '{service_name}' not found in installed packages"
            ))
        })?;
    let config_path = root.join("etc/sage/services.toml");
    let mut config = ServicesConfig::load(&config_path)?;
    if config.enabled.contains(service_name) {
        println!("Service '{service_name}' is already managed-enabled in services.toml");
        return Ok(());
    }
    if !dry_run {
        config.enabled.insert(service_name.to_string());
        config.disabled.remove(service_name);
        config.save(&config_path)?;
        if let Ok((_provider, generator)) = load_active_generator(root) {
            let _ = generator.render_service(&spec, root);
            let rendered_path = root.join("var/lib/sage/rendered-services.toml");
            if rendered_path.exists()
                && let Ok(mut rendered) = RenderedServicesState::load(&rendered_path)
            {
                rendered.enabled.insert(service_name.to_string());
                if !rendered.services.iter().any(|s| s.name == spec.name) {
                    rendered.services.push(spec);
                }
                let _ = fs::write(
                    &rendered_path,
                    toml::to_string_pretty(&rendered)
                        .map_err(|e| SysError::Invalid(e.to_string()))?,
                );
            }
        }
    }
    println!("Adopted service '{service_name}' into Sage declarative management (managed-enabled)");
    Ok(())
}

/// Lists all known services and their management lifecycle status.
pub fn list_services(root: &Path) -> Result<Vec<ServiceStatusInfo>, SysError> {
    let services = load_available_services(root)?;
    let config_path = root.join("etc/sage/services.toml");
    let config = ServicesConfig::load(&config_path)?;
    let rendered_path = root.join("var/lib/sage/rendered-services.toml");
    let previous_rendered = if rendered_path.exists() {
        RenderedServicesState::load(&rendered_path).ok()
    } else {
        None
    };
    let active_gen = load_active_generator(root).ok();

    let mut result = Vec::new();
    for service in services {
        let (provider_name, is_init_enabled) = if let Some((name, generator)) = &active_gen {
            let enabled = generator.is_service_enabled(&service, root).unwrap_or(None);
            (name.clone(), enabled)
        } else {
            ("unknown".to_string(), None)
        };

        let state = if config.enabled.contains(&service.name) {
            "managed-enabled".to_string()
        } else if config.disabled.contains(&service.name)
            || previous_rendered
                .as_ref()
                .is_some_and(|r| r.enabled.contains(&service.name))
        {
            "managed-disabled".to_string()
        } else if is_init_enabled == Some(true) {
            "unmanaged (drift)".to_string()
        } else {
            "unmanaged".to_string()
        };

        result.push(ServiceStatusInfo {
            name: service.name,
            package: if service.package.is_empty() {
                "system".to_string()
            } else {
                service.package
            },
            state,
            provider: provider_name,
        });
    }
    Ok(result)
}
