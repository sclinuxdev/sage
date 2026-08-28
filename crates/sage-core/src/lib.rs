//! Core domain models, version algebra, symbol interning, and host locking.

use serde::{Deserialize, Serialize};
use std::cmp::Ordering;
use std::collections::HashMap;
use std::fmt;
use std::fs::{File, OpenOptions};
use std::path::{Path, PathBuf};
use std::str::FromStr;
use thiserror::Error;

/// Schema version understood by the Sage 0.4 metadata readers.
pub const SCHEMA_VERSION: u32 = 1;
/// Slot selected when metadata omits one.
pub const DEFAULT_SLOT: &str = "0";

/// Failures shared by core domain operations.
#[derive(Debug, Error)]
pub enum CoreError {
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
    #[error("failed to acquire host lock at {path}: {source}")]
    LockFailed {
        path: PathBuf,
        source: std::io::Error,
    },
    #[error("invalid version string '{0}'")]
    InvalidVersion(String),
    #[error("invalid package key string '{0}'")]
    InvalidPackageKey(String),
    #[error("invalid dependency string '{0}'")]
    InvalidDependency(String),
    #[error("unsupported schema version {found}; expected {SCHEMA_VERSION}")]
    UnsupportedSchema { found: u32 },
    #[error("symbol table exhausted its 32-bit address space")]
    SymbolTableFull,
}

/// Rejects metadata that does not use the current exact schema.
pub fn validate_schema(found: u32) -> Result<(), CoreError> {
    (found == SCHEMA_VERSION)
        .then_some(())
        .ok_or(CoreError::UnsupportedSchema { found })
}

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

/// Package version ordered by epoch, upstream version, then package release.
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct Version {
    #[serde(default)]
    pub epoch: u32,
    pub upstream: String,
    pub release: u32,
}

impl Version {
    pub fn new(epoch: u32, upstream: impl Into<String>, release: u32) -> Self {
        Self {
            epoch,
            upstream: upstream.into(),
            release,
        }
    }
}

impl FromStr for Version {
    type Err = CoreError;

    fn from_str(input: &str) -> Result<Self, Self::Err> {
        let (epoch, rest) = match input.split_once(':') {
            Some((value, rest)) => (parse_number(value, input)?, rest),
            None => (0, input),
        };
        let (upstream, release) = rest
            .rsplit_once('-')
            .ok_or_else(|| CoreError::InvalidVersion(input.into()))?;
        if upstream.is_empty() {
            return Err(CoreError::InvalidVersion(input.into()));
        }
        Ok(Self::new(epoch, upstream, parse_number(release, input)?))
    }
}

fn parse_number(value: &str, whole: &str) -> Result<u32, CoreError> {
    value
        .parse()
        .map_err(|_| CoreError::InvalidVersion(whole.into()))
}

impl fmt::Display for Version {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.epoch {
            0 => write!(f, "{}-{}", self.upstream, self.release),
            epoch => write!(f, "{epoch}:{}-{}", self.upstream, self.release),
        }
    }
}

impl PartialOrd for Version {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for Version {
    fn cmp(&self, other: &Self) -> Ordering {
        self.epoch
            .cmp(&other.epoch)
            .then_with(|| compare_upstream(&self.upstream, &other.upstream))
            .then_with(|| self.release.cmp(&other.release))
    }
}

/// Compares alternating numeric and alphabetic runs without integer conversion.
/// Numeric runs ignore leading zeroes and compare by significant length first,
/// so even adversarially long version components cannot overflow.
fn compare_upstream(left: &str, right: &str) -> Ordering {
    let (mut a, mut b) = (left.as_bytes(), right.as_bytes());
    loop {
        a = trim_separators(a);
        b = trim_separators(b);
        if a.is_empty() || b.is_empty() {
            return a.len().cmp(&b.len());
        }
        let numeric = a[0].is_ascii_digit() && b[0].is_ascii_digit();
        if a[0].is_ascii_digit() != b[0].is_ascii_digit() {
            return a[0].is_ascii_digit().cmp(&b[0].is_ascii_digit());
        }
        let (arun, arest) = take_run(a, numeric);
        let (brun, brest) = take_run(b, numeric);
        let order = if numeric {
            compare_numeric(arun, brun)
        } else {
            arun.cmp(brun)
        };
        if order != Ordering::Equal {
            return order;
        }
        (a, b) = (arest, brest);
    }
}

fn trim_separators(mut value: &[u8]) -> &[u8] {
    while value
        .first()
        .is_some_and(|byte| !byte.is_ascii_alphanumeric())
    {
        value = &value[1..];
    }
    value
}

fn take_run(value: &[u8], numeric: bool) -> (&[u8], &[u8]) {
    let end = value
        .iter()
        .position(|byte| byte.is_ascii_digit() != numeric || !byte.is_ascii_alphanumeric())
        .unwrap_or(value.len());
    value.split_at(end)
}

fn compare_numeric(left: &[u8], right: &[u8]) -> Ordering {
    let significant_left = &left[left.iter().take_while(|byte| **byte == b'0').count()..];
    let significant_right = &right[right.iter().take_while(|byte| **byte == b'0').count()..];
    significant_left
        .len()
        .cmp(&significant_right.len())
        .then_with(|| significant_left.cmp(significant_right))
        // Preserve the Ord/Eq contract for differently spelled equal numbers.
        .then_with(|| left.cmp(right))
}

/// Operator applied to an optional dependency version.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
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
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
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

/// Compact identifier used instead of strings on hot paths.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct SymbolId(u32);

impl SymbolId {
    pub fn get(self) -> u32 {
        self.0
    }
}

/// Append-only string interner with stable identifiers and O(1) lookup.
#[derive(Debug, Default)]
pub struct SymbolTable {
    ids: HashMap<String, SymbolId>,
    values: Vec<String>,
}

impl SymbolTable {
    pub fn intern(&mut self, value: &str) -> Result<SymbolId, CoreError> {
        if let Some(id) = self.ids.get(value) {
            return Ok(*id);
        }
        let id = SymbolId(
            self.values
                .len()
                .try_into()
                .map_err(|_| CoreError::SymbolTableFull)?,
        );
        let owned = value.to_owned();
        self.values.push(owned.clone());
        self.ids.insert(owned, id);
        Ok(id)
    }

    pub fn resolve(&self, id: SymbolId) -> Option<&str> {
        self.values.get(id.0 as usize).map(String::as_str)
    }

    pub fn len(&self) -> usize {
        self.values.len()
    }
    pub fn is_empty(&self) -> bool {
        self.values.is_empty()
    }
}

/// RAII guard around a host-wide advisory file lock.
pub struct HostLock {
    file: File,
    path: PathBuf,
}

impl HostLock {
    pub fn acquire_shared(path: impl AsRef<Path>) -> Result<Self, CoreError> {
        Self::acquire(path, false)
    }
    pub fn acquire_exclusive(path: impl AsRef<Path>) -> Result<Self, CoreError> {
        Self::acquire(path, true)
    }

    fn acquire(path: impl AsRef<Path>, exclusive: bool) -> Result<Self, CoreError> {
        let path = path.as_ref().to_path_buf();
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        // Never truncate: all processes must continue locking the same inode.
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .open(&path)?;
        let result = if exclusive {
            fs2::FileExt::lock_exclusive(&file)
        } else {
            fs2::FileExt::lock_shared(&file)
        };
        result.map_err(|source| CoreError::LockFailed {
            path: path.clone(),
            source,
        })?;
        Ok(Self { file, path })
    }

    pub fn path(&self) -> &Path {
        &self.path
    }
}

impl Drop for HostLock {
    fn drop(&mut self) {
        let _ = fs2::FileExt::unlock(&self.file);
    }
}
