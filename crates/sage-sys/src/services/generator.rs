//! Template-based service file generator and init adapter.

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Component, Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::Ordering;

use serde::{Deserialize, Serialize};

use super::spec::{ServiceActivation, ServiceSpec};
use crate::{SysError, TEMP_ID, validate_schema};

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
    /// Provider-specific adapters for non-direct activation contracts. The
    /// engine selects by the generic activation kind and otherwise remains
    /// unaware of systemd, Loom, or any future init format.
    #[serde(default)]
    pub activations: BTreeMap<String, TemplateActivationAdapter>,
}

/// Provider rendering and lifecycle overrides for one activation kind.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct TemplateActivationAdapter {
    /// Automatically available activations, such as D-Bus service files, are
    /// rendered but cannot be placed in the boot enablement policy.
    #[serde(default)]
    pub automatic: bool,
    /// Optional replacement for the primary native service definition.
    #[serde(default)]
    pub service_template: Option<String>,
    /// Additional provider-owned files, for example a systemd `.socket` unit
    /// or a D-Bus activation descriptor.
    #[serde(default)]
    pub artifacts: Vec<TemplateArtifact>,
    #[serde(default, alias = "enable_cmd")]
    pub enable_command: Option<String>,
    #[serde(default, alias = "disable_cmd")]
    pub disable_command: Option<String>,
    #[serde(default, alias = "is_enabled_cmd")]
    pub is_enabled_command: Option<String>,
}

/// One extra file emitted alongside the primary native service definition.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct TemplateArtifact {
    #[serde(rename = "target_path")]
    pub target_path_template: String,
    pub mode: u32,
    pub template: String,
}

#[derive(Clone, Copy)]
enum LifecycleCommand {
    Enable,
    Disable,
    IsEnabled,
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
        self.activation_adapter(service)?;
        let target = self.rendered_path(service, sysroot)?;
        self.render_primary_to(service, sysroot, &target)?;
        self.render_activation_artifacts(service, sysroot)?;
        Ok(target)
    }

    fn render_primary_to(
        &self,
        service: &ServiceSpec,
        sysroot: &Path,
        target: &Path,
    ) -> Result<(), SysError> {
        let variables = self.service_variables(service, sysroot)?;
        let rendered = expand_template(self.service_template(service)?, &variables)?;
        self.publish_rendered(
            service,
            sysroot,
            target,
            &rendered,
            self.mode,
            Some(&variables),
        )
    }

    fn render_activation_artifacts(
        &self,
        service: &ServiceSpec,
        sysroot: &Path,
    ) -> Result<(), SysError> {
        let Some(adapter) = self.activation_adapter(service)? else {
            return Ok(());
        };
        let variables = self.service_variables(service, sysroot)?;
        for artifact in &adapter.artifacts {
            let target = self.artifact_path(artifact, &variables, sysroot)?;
            let rendered = expand_template(&artifact.template, &variables)?;
            self.publish_rendered(service, sysroot, &target, &rendered, artifact.mode, None)?;
        }
        Ok(())
    }

    fn publish_rendered(
        &self,
        service: &ServiceSpec,
        sysroot: &Path,
        target: &Path,
        rendered: &str,
        mode: u32,
        compile_variables: Option<&BTreeMap<String, String>>,
    ) -> Result<(), SysError> {
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
        if let Some(compile_variables) =
            compile_variables.filter(|_| !self.compile_command.is_empty())
        {
            let input = temporary.with_extension("input.toml");
            fs::write(&input, rendered)?;
            let mut compile_variables = compile_variables.clone();
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
        } else {
            let mut options = fs::OpenOptions::new();
            options.write(true).create_new(true);
            std::io::Write::write_all(&mut options.open(&temporary)?, rendered.as_bytes())?;
        }
        fs::set_permissions(&temporary, fs::Permissions::from_mode(mode))?;
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
                self.activation_adapter(service)?;
                let target = self.rendered_path(service, sysroot)?;
                if target.parent() != Some(target_directory.as_path()) {
                    return Err(SysError::Invalid(format!(
                        "service {} renders outside managed directory {}",
                        service.name,
                        target_directory.display()
                    )));
                }
                self.render_primary_to(
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
        for service in services {
            self.render_activation_artifacts(service, sysroot)?;
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
    /// service type cannot strand a package or standalone lifecycle transaction
    /// after its recovery journal has been persisted.
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
        let mut rendered_paths = BTreeSet::new();
        for service in services {
            service.validate()?;
            self.validate_service_type(service)?;
            self.activation_adapter(service)?;
            let variables = self.service_variables(service, sysroot)?;
            expand_template(self.service_template(service)?, &variables)?;
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
            for target in self.rendered_paths(service, sysroot)? {
                if !rendered_paths.insert(target.clone()) {
                    return Err(SysError::Invalid(format!(
                        "duplicate native service output {}",
                        target.display()
                    )));
                }
            }
            if let Some(adapter) = self.activation_adapter(service)? {
                for artifact in &adapter.artifacts {
                    expand_template(&artifact.template, &variables)?;
                }
            }
            self.validate_compile_command(&variables, sysroot)?;
            for (kind, command) in [
                ("validate", self.validate_command.as_deref()),
                (
                    "enable",
                    self.lifecycle_command(service, LifecycleCommand::Enable)?,
                ),
                (
                    "disable",
                    self.lifecycle_command(service, LifecycleCommand::Disable)?,
                ),
                (
                    "is-enabled",
                    self.lifecycle_command(service, LifecycleCommand::IsEnabled)?,
                ),
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
                    self.lifecycle_command(service, LifecycleCommand::Enable)?
                        .filter(|_| enabled.contains(&service.name)),
                ),
                // Only enabled services need an eventual disable command.
                (
                    "disable",
                    self.lifecycle_command(service, LifecycleCommand::Disable)?
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
        self.lifecycle_command(service, LifecycleCommand::Disable)?
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
        if let Some(command) = self.lifecycle_command(service, LifecycleCommand::Enable)? {
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
        if let Some(command) = self.lifecycle_command(service, LifecycleCommand::Disable)? {
            run_validation(
                &expand_template(command, &self.service_variables(service, sysroot)?)?,
                sysroot,
            )?;
        }
        Ok(())
    }

    /// Checks whether a service is enabled in the host init system.
    pub fn is_service_enabled(
        &self,
        service: &ServiceSpec,
        sysroot: &Path,
    ) -> Result<Option<bool>, SysError> {
        let Some(command) = self.lifecycle_command(service, LifecycleCommand::IsEnabled)? else {
            return Ok(None);
        };
        let variables = self.service_variables(service, sysroot)?;
        let expanded = expand_template(command, &variables)?;
        let mut words = expanded.split_whitespace();
        let Some(program_str) = words.next() else {
            return Ok(None);
        };
        let program = target_path(sysroot, Path::new(program_str))?;
        if !program.exists() {
            return Ok(None);
        }
        ensure_existing_beneath(sysroot, &program)?;
        let resolved = fs::canonicalize(&program)?;
        let status = Command::new(&resolved)
            .args(words)
            .env_clear()
            .env("PATH", "/usr/bin")
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .status()?;
        match status.code() {
            Some(0) => Ok(Some(true)),
            Some(1) => Ok(Some(false)),
            _ => Err(SysError::Trigger {
                name: format!("is-enabled for '{}'", service.name),
                status,
            }),
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

    /// Resolves every provider-owned output for a service, including activation
    /// artifacts. Callers use the complete set for collision checks, previews,
    /// stale cleanup, and crash-safe provider transitions.
    pub fn rendered_paths(
        &self,
        service: &ServiceSpec,
        sysroot: &Path,
    ) -> Result<Vec<PathBuf>, SysError> {
        let variables = self.service_variables(service, sysroot)?;
        let mut paths = vec![self.rendered_path(service, sysroot)?];
        if let Some(adapter) = self.activation_adapter(service)? {
            for artifact in &adapter.artifacts {
                paths.push(self.artifact_path(artifact, &variables, sysroot)?);
            }
        }
        Ok(paths)
    }

    /// Returns true when all native files required for a service exist.
    pub fn is_rendered(&self, service: &ServiceSpec, sysroot: &Path) -> Result<bool, SysError> {
        Ok(self
            .rendered_paths(service, sysroot)?
            .iter()
            .all(|path| path.is_file()))
    }

    /// Whether this service is installed as an automatically available
    /// activation rather than a boot-policy-controlled service.
    pub fn is_automatic(&self, service: &ServiceSpec) -> Result<bool, SysError> {
        self.activation_adapter(service)?;
        Ok(service.activation.is_automatic())
    }

    /// Removes all previously rendered provider files without following links.
    pub fn remove_service(&self, service: &ServiceSpec, sysroot: &Path) -> Result<(), SysError> {
        for path in self.rendered_paths(service, sysroot)? {
            match fs::symlink_metadata(&path) {
                Ok(metadata)
                    if metadata.file_type().is_file() || metadata.file_type().is_symlink() =>
                {
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
        }
        Ok(())
    }

    fn service_template<'a>(&'a self, service: &ServiceSpec) -> Result<&'a str, SysError> {
        Ok(self
            .activation_adapter(service)?
            .and_then(|adapter| adapter.service_template.as_deref())
            .unwrap_or(&self.template))
    }

    fn activation_adapter(
        &self,
        service: &ServiceSpec,
    ) -> Result<Option<&TemplateActivationAdapter>, SysError> {
        if matches!(service.activation, ServiceActivation::Service) {
            return Ok(None);
        }
        let kind = service.activation.kind();
        let adapter = self.activations.get(kind).ok_or_else(|| {
            SysError::Invalid(format!(
                "init provider does not support {kind} activation for {}",
                service.name
            ))
        })?;
        if adapter.automatic != service.activation.is_automatic() {
            return Err(SysError::Invalid(format!(
                "init provider has invalid automatic policy for {kind} activation"
            )));
        }
        Ok(Some(adapter))
    }

    fn artifact_path(
        &self,
        artifact: &TemplateArtifact,
        variables: &BTreeMap<String, String>,
        sysroot: &Path,
    ) -> Result<PathBuf, SysError> {
        let relative = expand_template(&artifact.target_path_template, variables)?;
        target_path(sysroot, Path::new(&relative))
    }

    fn lifecycle_command<'a>(
        &'a self,
        service: &ServiceSpec,
        command: LifecycleCommand,
    ) -> Result<Option<&'a str>, SysError> {
        if self.is_automatic(service)? {
            return Ok(None);
        }
        let adapter = self.activation_adapter(service)?;
        Ok(match command {
            LifecycleCommand::Enable => adapter
                .and_then(|value| value.enable_command.as_deref())
                .or(self.enable_command.as_deref()),
            LifecycleCommand::Disable => adapter
                .and_then(|value| value.disable_command.as_deref())
                .or(self.disable_command.as_deref()),
            LifecycleCommand::IsEnabled => adapter
                .and_then(|value| value.is_enabled_command.as_deref())
                .or(self.is_enabled_command.as_deref()),
        })
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
    let (listen_stream, accept, socket_mode, dbus_name, dbus_bus, dbus_user) =
        match &service.activation {
            ServiceActivation::Service => (
                String::new(),
                false,
                0,
                String::new(),
                String::new(),
                String::new(),
            ),
            ServiceActivation::Socket {
                listen_stream,
                accept,
                mode,
            } => (
                listen_stream.clone(),
                *accept,
                *mode,
                String::new(),
                String::new(),
                String::new(),
            ),
            ServiceActivation::Dbus { name, bus, user } => (
                String::new(),
                false,
                0,
                name.clone(),
                bus.clone(),
                if user.is_empty() {
                    service.user.clone()
                } else {
                    user.clone()
                },
            ),
        };
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
        ("activation.kind".into(), service.activation.kind().into()),
        (
            "activation.kind_json".into(),
            json_quote_str(service.activation.kind()),
        ),
        ("activation.listen_stream".into(), listen_stream.clone()),
        (
            "activation.listen_stream_json".into(),
            json_quote_str(&listen_stream),
        ),
        ("activation.accept".into(), accept.to_string()),
        (
            "activation.socket_mode".into(),
            format!("{socket_mode:04o}"),
        ),
        ("activation.socket_mode_int".into(), socket_mode.to_string()),
        ("activation.dbus_name".into(), dbus_name.clone()),
        (
            "activation.dbus_name_json".into(),
            json_quote_str(&dbus_name),
        ),
        ("activation.dbus_bus".into(), dbus_bus.clone()),
        ("activation.dbus_bus_json".into(), json_quote_str(&dbus_bus)),
        ("activation.dbus_user".into(), dbus_user.clone()),
        (
            "activation.dbus_user_json".into(),
            json_quote_str(&dbus_user),
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
