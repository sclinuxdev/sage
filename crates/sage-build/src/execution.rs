/// Global reproducible-build and sandbox policy.
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
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
#[serde(deny_unknown_fields)]
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
        for tool in [&config.cc, &config.cxx, &config.linker, &config.rustc] {
            validate_tool_name(tool)?;
        }
        for target in config.targets.values() {
            for tool in [&target.cc, &target.cxx, &target.ar, &target.strip] {
                validate_tool_name(tool)?;
            }
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
        "#!/usr/bin/bash\nset -euo pipefail\ntrap 'printf >&2 \"sage-build: line %s failed\\n\" \"$LINENO\"' ERR\n",
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

fn validate_tool_name(value: &str) -> Result<(), BuildError> {
    if !value.is_empty()
        && Path::new(value).components().count() == 1
        && !value.contains(['\n', '\r', '\0'])
    {
        Ok(())
    } else {
        Err(BuildError::InvalidSpec(format!(
            "managed build tool must be an executable name, got '{value}'"
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
            "/usr/bin/bash",
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
        for (name, role, tool, driver, rustc) in [
            (&self.config.cc, "cc", &self.config.cc, true, false),
            (&self.config.cxx, "cxx", &self.config.cxx, true, false),
            (&self.config.linker, "linker", &self.config.linker, false, false),
            (&self.config.rustc, "rustc", &self.config.rustc, false, true),
        ] {
            self.write_tool_wrapper(&directory.join(name), role, tool, driver, rustc)?;
        }
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
        validate_tool_name(tool)?;
        let search_path = "/usr/bin";
        let mut script = format!(
            "#!/usr/bin/bash\nset -euo pipefail\nresolved=$(PATH={} command -v -- {})\nprintf '%s\\t%s\\n' {} \"$resolved\" >> /build/.sage-tool-usage\n",
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
        let path = "/build/.sage-tools:/usr/bin";
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
