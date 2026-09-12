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

/// Checks the shared alphabet for concrete package names and slots.
/// Both require non-empty ASCII letters, digits, '.', '_', '+', or '-'.
pub fn valid_package_component(value: &str) -> bool {
    !value.is_empty()
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'+' | b'-'))
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
        } else {
            fields[0]
                .rsplit_once('/')
                .map_or((None, fields[0]), |(channel, package)| {
                    (Some(channel.into()), package)
                })
        };
        let (name, slot) = if package.starts_with("so:") {
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
        Ok(Self {
            name: name.into(),
            slot,
            channel,
            op,
            version,
        })
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
