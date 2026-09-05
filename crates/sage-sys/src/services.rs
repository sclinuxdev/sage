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

/// Last successfully reconciled native-service state. Keeping the generic
/// declarations lets Sage disable and remove stale output after an init switch.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RenderedServicesState {
    pub schema_version: u32,
    /// Full identity distinguishes same-name providers across slots and channels.
    pub provider: sage_core::PackageKey,
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
            return Err(SysError::Invalid("invalid rendered-service provider".into()));
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

fn validate_service_command(field: &str, command: &[String], service: &str) -> Result<(), SysError> {
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
        self.validate_service_set(services, sysroot)?;
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
        if let Some(service) = services.first() {
            if let Err(error) = self.validate_rendered_services(service, sysroot) {
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
            if let Some(directory) = &managed_directory {
                if target.parent() != Some(directory.as_path()) {
                    return Err(SysError::Invalid(format!(
                        "service {} renders outside managed directory {}",
                        service.name,
                        directory.display()
                    )));
                }
            }
            self.validate_compile_command(&variables, sysroot)?;
            for (kind, command) in [
                ("validate", self.validate_command.as_deref()),
                ("enable", self.enable_command.as_deref()),
                ("disable", self.disable_command.as_deref()),
            ] {
                if let Some(command) = command {
                    self.validate_command_path(kind, command, &variables, sysroot)?;
                }
            }
        }
        Ok(())
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

    /// Resolves the provider-owned native file for a generic service.
    pub fn rendered_path(&self, service: &ServiceSpec, sysroot: &Path) -> Result<PathBuf, SysError> {
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
    ) -> Result<(), SysError> {
        let Some((program, arguments)) = self.compile_command.split_first() else {
            return Ok(());
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
        target_path(sysroot, Path::new(&program))?;
        for argument in arguments {
            expand_template(argument, &variables)?;
        }
        Ok(())
    }

    fn validate_command_path(
        &self,
        kind: &str,
        command: &str,
        variables: &BTreeMap<String, String>,
        sysroot: &Path,
    ) -> Result<(), SysError> {
        let command = expand_template(command, variables)?;
        let program = command
            .split_whitespace()
            .next()
            .ok_or_else(|| SysError::Invalid(format!("empty {kind} command")))?;
        target_path(sysroot, Path::new(program)).map_err(|error| {
            SysError::Invalid(format!("invalid {kind} command program: {error}"))
        })?;
        Ok(())
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
            serde_json::to_string(&service.description)?,
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
            serde_json::to_string(&service.command)?,
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
            serde_json::to_string(&service.stop_command)?,
        ),
        (
            "service.stop_action_toml".into(),
            if service.stop_command.is_empty() {
                String::new()
            } else {
                format!("stop = {}", serde_json::to_string(&service.stop_command)?)
            },
        ),
        (
            "service.stop_input_toml".into(),
            if service.stop_command.is_empty() {
                String::new()
            } else {
                format!(
                    "stop_command = {}",
                    serde_json::to_string(&service.stop_command)?
                )
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
            serde_json::to_string(&service.reload_command)?,
        ),
        (
            "service.reload_action_toml".into(),
            if service.reload_command.is_empty() {
                String::new()
            } else {
                format!(
                    "reload = {}",
                    serde_json::to_string(&service.reload_command)?
                )
            },
        ),
        (
            "service.reload_input_toml".into(),
            if service.reload_command.is_empty() {
                String::new()
            } else {
                format!(
                    "reload_command = {}",
                    serde_json::to_string(&service.reload_command)?
                )
            },
        ),
        ("service.user".into(), service.user.clone()),
        (
            "service.user_json".into(),
            serde_json::to_string(&service.user)?,
        ),
        ("service.group".into(), service.group.clone()),
        (
            "service.group_json".into(),
            serde_json::to_string(&service.group)?,
        ),
        ("service.working_dir".into(), service.working_dir.clone()),
        (
            "service.working_dir_json".into(),
            serde_json::to_string(&service.working_dir)?,
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
        ("service.after_json".into(), serde_json::to_string(&after)?),
        ("service.before".into(), before.join(" ")),
        ("service.before_space".into(), before.join(" ")),
        (
            "service.before_json".into(),
            serde_json::to_string(&before)?,
        ),
        ("service.runtime".into(), service.runtime.clone()),
        (
            "service.runtime_json".into(),
            serde_json::to_string(&service.runtime)?,
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
    command
        .iter()
        .map(|argument| serde_json::to_string(argument).map_err(SysError::from))
        .collect::<Result<Vec<_>, _>>()
        .map(|arguments| arguments.join(" "))
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
