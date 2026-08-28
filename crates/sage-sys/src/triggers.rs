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
                    .env("PATH", "/usr/bin")
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
