//! Declarative rclass execution, Bubblewrap isolation, payload carving, and ELF scans.

use serde::Deserialize;
use std::collections::BTreeMap;
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
    fn shell_quote_does_not_open_single_quotes() {
        assert_eq!(shell_quote("a'b"), "'a'\\''b'");
    }
}
