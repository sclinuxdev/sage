//! Package identifiers, coordinates, dependencies, constraints, and manifests.

use crate::error::{CoreError, DEFAULT_SLOT, SCHEMA_VERSION};
use crate::version::Version;
use serde::{Deserialize, Serialize};
use std::fmt;
use std::str::FromStr;

/// Unique package identity `(channel, name, slot)` independent of its version.
#[derive(Debug, Clone, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
pub struct PackageKey {
    pub channel: String,
    pub name: String,
    #[serde(default = "default_slot")]
    pub slot: String,
}

fn default_slot() -> String {
    DEFAULT_SLOT.into()
}

/// Checks the shared alphabet for concrete package coordinates: channel, name, slot, arch.
/// Both require non-empty ASCII letters, digits, '.', '_', '+', or '-', and forbid '.' and '..'
/// path traversal components.
pub fn valid_package_component(value: &str) -> bool {
    !value.is_empty()
        && value != "."
        && value != ".."
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'+' | b'-'))
}

/// Checks the alphabet for upstream version strings.
/// Requires non-empty ASCII alphanumeric characters, '.', '_', '+', '-', or '~',
/// must start with an ASCII alphanumeric character, and forbids '.' and '..'
/// path traversal components.
pub fn valid_version_string(value: &str) -> bool {
    !value.is_empty()
        && value != "."
        && value != ".."
        && !value.contains("..")
        && value.starts_with(|c: char| c.is_ascii_alphanumeric())
        && value.bytes().all(|byte| {
            byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'+' | b'-' | b'~')
        })
}

/// Validates a channel coordinate component, which may be hierarchical
/// (e.g. `main/system`, `vendor/system`, or `system`).
/// Each segment must be non-empty and satisfy `valid_package_component`.
pub fn valid_channel_name(value: &str) -> bool {
    !value.is_empty()
        && !value.starts_with('/')
        && !value.ends_with('/')
        && !value.contains("//")
        && value.split('/').all(valid_package_component)
}

/// Validates a short virtual interface, `virtual/name`, command `cmd:name`,
/// synthetic solver interface `virtual/provider/name`, or an opaque `so:soname`.
/// Sonames must be non-empty and exclude whitespace, controls, paths, and the
/// override separator; they are not restricted to the package-name alphabet.
pub fn valid_provider_symbol(value: &str) -> bool {
    if let Some(soname) = value.strip_prefix("so:") {
        !soname.is_empty()
            && !soname
                .chars()
                .any(|ch| ch.is_whitespace() || ch.is_control() || matches!(ch, '/' | '='))
    } else if let Some(cmd) = value.strip_prefix("cmd:") {
        valid_package_component(cmd)
    } else if let Some(inner) = value.strip_prefix("virtual/provider/") {
        valid_provider_symbol(inner)
    } else {
        valid_package_component(value.strip_prefix("virtual/").unwrap_or(value))
    }
}

/// Returns whether a dependency or interface name refers to a provided virtual symbol
/// (such as `virtual/name`, shared library `so:soname`, or command `cmd:binary`).
pub fn is_virtual_symbol(symbol: &str) -> bool {
    symbol.starts_with("virtual/") || symbol.starts_with("so:") || symbol.starts_with("cmd:")
}

impl PackageKey {
    /// Constructs a package key from caller-owned string-like values.
    pub fn new(
        channel: impl Into<String>,
        name: impl Into<String>,
        slot: impl Into<String>,
    ) -> Self {
        Self {
            channel: channel.into(),
            name: name.into(),
            slot: slot.into(),
        }
    }

    /// Parses the shared user-facing `name[:slot]` selector inside one channel.
    pub fn in_channel(channel: impl Into<String>, selector: &str) -> Result<Self, CoreError> {
        let (name, slot) = selector
            .split_once(':')
            .map_or((selector, DEFAULT_SLOT), |(name, slot)| (name, slot));
        if name.is_empty() || slot.is_empty() || slot.contains(':') {
            return Err(CoreError::InvalidPackageKey(selector.into()));
        }
        Ok(Self::new(channel, name, slot))
    }

    /// Returns the stable key used in LMDB and diagnostics.
    pub fn canonical_id(&self) -> String {
        self.to_string()
    }
}

impl FromStr for PackageKey {
    type Err = CoreError;

    fn from_str(input: &str) -> Result<Self, Self::Err> {
        let mut parts = input.split(':');
        let (Some(channel), Some(name)) = (parts.next(), parts.next()) else {
            return Err(CoreError::InvalidPackageKey(input.into()));
        };
        let slot = parts.next().unwrap_or(DEFAULT_SLOT);
        if channel.is_empty() || name.is_empty() || slot.is_empty() || parts.next().is_some() {
            return Err(CoreError::InvalidPackageKey(input.into()));
        }
        Ok(Self::new(channel, name, slot))
    }
}

impl fmt::Display for PackageKey {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}:{}:{}", self.channel, self.name, self.slot)
    }
}

/// A package identity paired with its ordered release version.
#[derive(Debug, Clone, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
pub struct PackageCoordinate {
    pub key: PackageKey,
    pub version: Version,
}

impl PackageCoordinate {
    /// Creates a coordinate from an identity and version.
    pub fn new(key: PackageKey, version: Version) -> Self {
        Self { key, version }
    }
}

impl fmt::Display for PackageCoordinate {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}@{}", self.key, self.version)
    }
}

/// Operator applied to an optional dependency version.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
pub enum ConstraintOp {
    Any,
    Equal,
    NotEqual,
    Greater,
    GreaterOrEqual,
    Less,
    LessOrEqual,
}

impl ConstraintOp {
    /// Tests a candidate against the supplied constraint version.
    pub fn matches(self, candidate: &Version, constraint: Option<&Version>) -> bool {
        let Some(wanted) = constraint else {
            return self == Self::Any;
        };
        match self {
            Self::Any => true,
            Self::Equal => candidate == wanted,
            Self::NotEqual => candidate != wanted,
            Self::Greater => candidate > wanted,
            Self::GreaterOrEqual => candidate >= wanted,
            Self::Less => candidate < wanted,
            Self::LessOrEqual => candidate <= wanted,
        }
    }
}

/// Dependency with optional channel, slot, and version restrictions.
#[derive(Debug, Clone, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
pub struct Dependency {
    pub name: String,
    pub slot: Option<String>,
    pub channel: Option<String>,
    pub op: ConstraintOp,
    pub version: Option<Version>,
}

impl Dependency {
    /// Validates dependency component formats (provider or package name, channel, slot, and version).
    pub fn validate(&self) -> Result<(), CoreError> {
        if !valid_provider_symbol(&self.name) {
            return Err(CoreError::InvalidDependency(format!(
                "invalid dependency name: '{}'",
                self.name
            )));
        }
        if let Some(channel) = &self.channel
            && !valid_channel_name(channel)
        {
            return Err(CoreError::InvalidDependency(format!(
                "invalid dependency channel: '{channel}'"
            )));
        }
        if let Some(slot) = &self.slot
            && !valid_package_component(slot)
        {
            return Err(CoreError::InvalidDependency(format!(
                "invalid dependency slot: '{slot}'"
            )));
        }
        if self.op != ConstraintOp::Any {
            let Some(version) = &self.version else {
                return Err(CoreError::InvalidDependency(format!(
                    "missing constraint version for dependency: '{}'",
                    self.name
                )));
            };
            if !valid_version_string(&version.upstream) {
                return Err(CoreError::InvalidVersion(format!(
                    "invalid dependency version: '{}'",
                    version.upstream
                )));
            }
        }
        Ok(())
    }

    /// Returns true if this dependency targets a virtual interface, library SONAME, or command symbol.
    pub fn is_virtual(&self) -> bool {
        is_virtual_symbol(&self.name)
    }
}

impl FromStr for Dependency {
    type Err = CoreError;

    fn from_str(input: &str) -> Result<Self, Self::Err> {
        let fields: Vec<_> = input.split_whitespace().collect();
        if fields.is_empty() || fields.len() > 3 {
            return Err(CoreError::InvalidDependency(input.into()));
        }
        // Provider symbols are opaque. For concrete packages the final slash
        // separates an optional channel, and the colon separates an optional slot.
        let (channel, package) = if fields[0].starts_with("virtual/") {
            (None, fields[0])
        } else if let Some(idx) = fields[0].find("/virtual/") {
            (Some(fields[0][..idx].into()), &fields[0][idx + 1..])
        } else if let Some(idx) = fields[0].find("/so:") {
            (Some(fields[0][..idx].into()), &fields[0][idx + 1..])
        } else if let Some(idx) = fields[0].find("/cmd:") {
            (Some(fields[0][..idx].into()), &fields[0][idx + 1..])
        } else {
            fields[0]
                .rsplit_once('/')
                .map_or((None, fields[0]), |(channel, package)| {
                    (Some(channel.into()), package)
                })
        };
        let (name, slot) = if package.starts_with("so:") || package.starts_with("cmd:") {
            (package, None)
        } else {
            package
                .split_once(':')
                .map_or((package, None), |(name, slot)| (name, Some(slot.into())))
        };
        if name.is_empty() {
            return Err(CoreError::InvalidDependency(input.into()));
        }
        let (op, version) = match fields.as_slice() {
            [_] => (ConstraintOp::Any, None),
            [_, op, version] => (parse_op(op, input)?, Some(version.parse()?)),
            _ => return Err(CoreError::InvalidDependency(input.into())),
        };
        let dep = Self {
            name: name.into(),
            slot,
            channel,
            op,
            version,
        };
        dep.validate()?;
        Ok(dep)
    }
}

impl fmt::Display for ConstraintOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let value = match self {
            Self::Any => "",
            Self::Equal => "=",
            Self::NotEqual => "!=",
            Self::Greater => ">",
            Self::GreaterOrEqual => ">=",
            Self::Less => "<",
            Self::LessOrEqual => "<=",
        };
        f.write_str(value)
    }
}

impl fmt::Display for Dependency {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if let Some(channel) = &self.channel {
            write!(f, "{channel}/")?;
        }
        f.write_str(&self.name)?;
        if let Some(slot) = &self.slot {
            write!(f, ":{slot}")?;
        }
        if self.op != ConstraintOp::Any {
            let Some(version) = &self.version else {
                return Err(fmt::Error);
            };
            write!(f, " {} {version}", self.op)?;
        }
        Ok(())
    }
}

/// Toolchain process observed while producing one package artifact.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ManagedBuildTool {
    pub role: String,
    pub executable: String,
    pub family: String,
    pub version: String,
    pub version_argument: String,
    /// Non-empty Sage-configured flag channels stored as `NAME=value`.
    #[serde(default)]
    pub parameters: Vec<String>,
}

fn default_schema_version() -> u32 {
    SCHEMA_VERSION
}

/// Canonical package record shared by recipes, archives, indexes, and solving.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Package {
    #[serde(default = "default_schema_version")]
    pub schema_version: u32,
    pub name: String,
    #[serde(default = "default_slot")]
    pub slot: String,
    pub version: String,
    pub release: u32,
    #[serde(default)]
    pub epoch: u32,
    pub arch: String,
    pub channel: String,
    pub description: String,
    pub license: String,
    #[serde(default, with = "dependency_strings")]
    pub dependencies: Vec<Dependency>,
    #[serde(default)]
    pub provides: Vec<String>,
    #[serde(default)]
    pub conflicts: Vec<String>,
    #[serde(default)]
    pub features: Vec<String>,
    #[serde(default)]
    pub installed_size: u64,
    #[serde(default)]
    pub build_time: u64,
    #[serde(default)]
    pub managed_build_tools: Vec<ManagedBuildTool>,
}

impl Package {
    /// Validates all coordinate fields (channel, name, slot, arch, version, and license).
    /// Enforces the shared alphabet and prevents directory traversal or path injection.
    pub fn validate(&self) -> Result<(), CoreError> {
        if !valid_channel_name(&self.channel) {
            return Err(CoreError::InvalidPackageKey(format!(
                "invalid channel: '{}'",
                self.channel
            )));
        }
        if !valid_package_component(&self.name) {
            return Err(CoreError::InvalidPackageKey(format!(
                "invalid package name: '{}'",
                self.name
            )));
        }
        if !valid_package_component(&self.slot) {
            return Err(CoreError::InvalidPackageKey(format!(
                "invalid slot: '{}'",
                self.slot
            )));
        }
        if !valid_package_component(&self.arch) {
            return Err(CoreError::InvalidPackageKey(format!(
                "invalid arch: '{}'",
                self.arch
            )));
        }
        if !valid_version_string(&self.version) {
            return Err(CoreError::InvalidVersion(format!(
                "invalid version string: '{}'",
                self.version
            )));
        }
        crate::error::validate_spdx_expression(&self.license)?;
        for dep in &self.dependencies {
            dep.validate()?;
        }
        for provide in &self.provides {
            if !valid_provider_symbol(provide) {
                return Err(CoreError::InvalidPackageKey(format!(
                    "invalid provides symbol: '{provide}'"
                )));
            }
        }
        for conflict in &self.conflicts {
            let dep = conflict.parse::<Dependency>()?;
            dep.validate()?;
        }
        for feature in &self.features {
            if !valid_package_component(feature) {
                return Err(CoreError::InvalidPackageKey(format!(
                    "invalid feature: '{feature}'"
                )));
            }
        }
        Ok(())
    }

    /// Returns the package identity and ordered version represented by the record.
    pub fn coordinate(&self) -> PackageCoordinate {
        PackageCoordinate::new(
            PackageKey::new(&self.channel, &self.name, &self.slot),
            Version::new(self.epoch, &self.version, self.release),
        )
    }

    /// Returns this package's coordinate with an index-selected channel.
    pub fn coordinate_for_channel(&self, channel: &str) -> PackageCoordinate {
        PackageCoordinate::new(
            PackageKey::new(channel, &self.name, &self.slot),
            Version::new(self.epoch, &self.version, self.release),
        )
    }

    /// Clones the package while assigning the channel used by a repository index.
    pub fn for_channel(&self, channel: &str) -> Self {
        let mut package = self.clone();
        package.channel = channel.into();
        package
    }

    /// Builds the compact package record used by solver-focused callers.
    pub fn from_release(
        key: PackageKey,
        version: Version,
        dependencies: Vec<Dependency>,
        provides: Vec<String>,
    ) -> Self {
        Self {
            schema_version: SCHEMA_VERSION,
            name: key.name,
            slot: key.slot,
            version: version.upstream,
            release: version.release,
            epoch: version.epoch,
            arch: String::new(),
            channel: key.channel,
            description: String::new(),
            license: String::new(),
            dependencies,
            provides,
            conflicts: Vec::new(),
            features: Vec::new(),
            installed_size: 0,
            build_time: 0,
            managed_build_tools: Vec::new(),
        }
    }
}

mod dependency_strings {
    use super::Dependency;
    use serde::{Deserialize, Deserializer, Serialize, Serializer, de::Error};

    pub fn serialize<S>(dependencies: &[Dependency], serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        dependencies
            .iter()
            .map(ToString::to_string)
            .collect::<Vec<_>>()
            .serialize(serializer)
    }

    pub fn deserialize<'de, D>(deserializer: D) -> Result<Vec<Dependency>, D::Error>
    where
        D: Deserializer<'de>,
    {
        Vec::<String>::deserialize(deserializer)?
            .into_iter()
            .map(|value| value.parse().map_err(D::Error::custom))
            .collect()
    }
}

fn parse_op(value: &str, whole: &str) -> Result<ConstraintOp, CoreError> {
    match value {
        "=" | "==" => Ok(ConstraintOp::Equal),
        "!=" => Ok(ConstraintOp::NotEqual),
        ">" => Ok(ConstraintOp::Greater),
        ">=" => Ok(ConstraintOp::GreaterOrEqual),
        "<" => Ok(ConstraintOp::Less),
        "<=" => Ok(ConstraintOp::LessOrEqual),
        _ => Err(CoreError::InvalidDependency(whole.into())),
    }
}
