//! Core domain models, version algebra, symbol interning, and host locking.

use nix::errno::Errno;
use nix::fcntl::{open, openat, OFlag};
use nix::sys::stat::{fchmod, fstat, mkdirat, Mode};
use nix::unistd::{fchown, geteuid};
use serde::{Deserialize, Serialize};
use std::cmp::Ordering;
use std::collections::HashMap;
use std::fmt;
use std::fs::File;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::os::unix::fs::PermissionsExt;
use std::path::{Component, Path, PathBuf};
use std::str::FromStr;
use thiserror::Error;

/// Schema version understood by the Sage 0.4 metadata readers.
pub const SCHEMA_VERSION: u32 = 1;

/// Validates a strict SPDX license expression against the license-list version
/// embedded in the `spdx` crate. Recipe and archive readers share this gate so
/// invalid identifiers can never enter a repository index or installed state.
pub fn validate_spdx_expression(expression: &str) -> Result<(), CoreError> {
    if expression.is_empty() {
        return Err(CoreError::InvalidMetadata(
            "SPDX license expression is required".into(),
        ));
    }
    spdx::Expression::parse(expression)
        .map(|_| ())
        .map_err(|error| {
            CoreError::InvalidMetadata(format!("invalid SPDX license expression: {error}"))
        })
}

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
    #[error("timed out waiting for host lock at {0}")]
    LockTimedOut(PathBuf),
    #[error("invalid version string '{0}'")]
    InvalidVersion(String),
    #[error("invalid package key string '{0}'")]
    InvalidPackageKey(String),
    #[error("invalid dependency string '{0}'")]
    InvalidDependency(String),
    #[error("invalid metadata: {0}")]
    InvalidMetadata(String),
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
///
/// Dependencies stay in their compact string form at TOML and bincode seams,
/// while callers use one parsed domain type after deserialization.
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
    use serde::{de::Error, Deserialize, Deserializer, Serialize, Serializer};

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

enum TryLockError {
    Busy,
    Fatal(CoreError),
}

impl HostLock {
    pub fn acquire_shared(path: impl AsRef<Path>) -> Result<Self, CoreError> {
        Self::acquire(path, false)
    }
    pub fn acquire_exclusive(path: impl AsRef<Path>) -> Result<Self, CoreError> {
        Self::acquire(path, true)
    }
    pub fn acquire_shared_for(
        path: impl AsRef<Path>,
        timeout: std::time::Duration,
    ) -> Result<Self, CoreError> {
        Self::acquire_for(path, false, timeout)
    }
    pub fn acquire_exclusive_for(
        path: impl AsRef<Path>,
        timeout: std::time::Duration,
    ) -> Result<Self, CoreError> {
        Self::acquire_for(path, true, timeout)
    }

    fn acquire(path: impl AsRef<Path>, exclusive: bool) -> Result<Self, CoreError> {
        let path = path.as_ref().to_path_buf();
        let file = open_lock_file(&path)?;
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

    fn try_acquire(path: impl AsRef<Path>, exclusive: bool) -> Result<Self, TryLockError> {
        let path = path.as_ref().to_path_buf();
        let file = open_lock_file(&path).map_err(TryLockError::Fatal)?;
        let result = if exclusive {
            fs2::FileExt::try_lock_exclusive(&file)
        } else {
            fs2::FileExt::try_lock_shared(&file)
        };
        match result {
            Ok(()) => Ok(Self { file, path }),
            Err(source) if source.kind() == std::io::ErrorKind::WouldBlock => {
                Err(TryLockError::Busy)
            }
            Err(source) => Err(TryLockError::Fatal(CoreError::LockFailed { path, source })),
        }
    }

    fn acquire_for(
        path: impl AsRef<Path>,
        exclusive: bool,
        timeout: std::time::Duration,
    ) -> Result<Self, CoreError> {
        let path = path.as_ref().to_path_buf();
        let deadline = std::time::Instant::now()
            .checked_add(timeout)
            .ok_or_else(|| {
                CoreError::InvalidMetadata("operation lock timeout is out of range".into())
            })?;
        loop {
            match Self::try_acquire(&path, exclusive) {
                Ok(lock) => return Ok(lock),
                Err(TryLockError::Busy) if std::time::Instant::now() < deadline => {
                    let remaining = deadline.saturating_duration_since(std::time::Instant::now());
                    std::thread::sleep(remaining.min(std::time::Duration::from_millis(10)));
                }
                Err(TryLockError::Busy) => return Err(CoreError::LockTimedOut(path)),
                Err(TryLockError::Fatal(error)) => return Err(error),
            }
        }
    }

    pub fn path(&self) -> &Path {
        &self.path
    }
}

/// Opens the lock through an anchored directory walk. Every component after
/// the filesystem root is opened with `O_NOFOLLOW`, so an attacker cannot
/// redirect two Sage processes onto different lock inodes through a symlink.
fn open_lock_file(path: &Path) -> Result<File, CoreError> {
    let parent = path
        .parent()
        .ok_or_else(|| CoreError::InvalidMetadata("operation lock has no parent".into()))?;
    let file_name = path
        .file_name()
        .ok_or_else(|| CoreError::InvalidMetadata("operation lock has no file name".into()))?;
    let base = if path.is_absolute() {
        Path::new("/")
    } else {
        Path::new(".")
    };
    let raw = open(
        base,
        OFlag::O_RDONLY | OFlag::O_DIRECTORY | OFlag::O_CLOEXEC,
        Mode::empty(),
    )
    .map_err(errno_io)?;
    // SAFETY: `open` returned a fresh descriptor transferred exactly once.
    let mut current = unsafe { OwnedFd::from_raw_fd(raw) };
    let normal_components = parent
        .components()
        .filter(|component| matches!(component, Component::Normal(_)))
        .count();
    let mut normal_index = 0_usize;
    for component in parent.components() {
        let name = match component {
            Component::RootDir | Component::CurDir => continue,
            Component::Normal(name) => {
                normal_index += 1;
                name
            }
            _ => {
                return Err(CoreError::InvalidMetadata(format!(
                    "unsafe operation lock path {}",
                    path.display()
                )))
            }
        };
        match mkdirat(
            Some(current.as_raw_fd()),
            name,
            Mode::from_bits_truncate(if normal_index == normal_components {
                0o700
            } else {
                0o755
            }),
        ) {
            Ok(()) | Err(Errno::EEXIST) => {}
            Err(error) => return Err(errno_io(error)),
        }
        let next = openat(
            Some(current.as_raw_fd()),
            name,
            OFlag::O_RDONLY | OFlag::O_DIRECTORY | OFlag::O_CLOEXEC | OFlag::O_NOFOLLOW,
            Mode::empty(),
        )
        .map_err(errno_io)?;
        // SAFETY: `openat` returned a fresh descriptor transferred exactly once.
        let next = unsafe { OwnedFd::from_raw_fd(next) };
        if normal_index == normal_components {
            harden_lock_directory(&next, path)?;
        }
        current = next;
    }
    let raw = openat(
        Some(current.as_raw_fd()),
        file_name,
        OFlag::O_RDWR | OFlag::O_CREAT | OFlag::O_CLOEXEC | OFlag::O_NOFOLLOW,
        Mode::from_bits_truncate(0o600),
    )
    .map_err(errno_io)?;
    // SAFETY: `openat` returned a fresh descriptor transferred exactly once.
    let file = unsafe { File::from_raw_fd(raw) };
    if !file.metadata()?.is_file() {
        return Err(CoreError::InvalidMetadata(format!(
            "operation lock is not a regular file: {}",
            path.display()
        )));
    }
    file.set_permissions(std::fs::Permissions::from_mode(0o600))?;
    Ok(file)
}

/// Makes the private lock namespace replace-safe before opening its lock file.
///
/// Root repairs a pre-created directory to its own ownership. An unprivileged
/// caller can repair its own mode, while a directory owned by another account
/// fails closed when `fchown` is denied. The descriptor remains anchored and
/// `O_NOFOLLOW`, so validation never races through a replacement path.
fn harden_lock_directory(directory: &OwnedFd, path: &Path) -> Result<(), CoreError> {
    let expected_owner = geteuid();
    let mut metadata = fstat(directory.as_raw_fd()).map_err(errno_io)?;
    if metadata.st_uid != expected_owner.as_raw() {
        fchown(directory.as_raw_fd(), Some(expected_owner), None).map_err(errno_io)?;
        metadata = fstat(directory.as_raw_fd()).map_err(errno_io)?;
    }
    if metadata.st_mode & 0o7777 != 0o700 {
        fchmod(directory.as_raw_fd(), Mode::from_bits_truncate(0o700)).map_err(errno_io)?;
        metadata = fstat(directory.as_raw_fd()).map_err(errno_io)?;
    }
    if metadata.st_uid != expected_owner.as_raw() || metadata.st_mode & 0o7777 != 0o700 {
        return Err(CoreError::InvalidMetadata(format!(
            "operation lock directory is not private: {}",
            path.parent().unwrap_or(path).display()
        )));
    }
    Ok(())
}

fn errno_io(error: Errno) -> CoreError {
    CoreError::Io(std::io::Error::from_raw_os_error(error as i32))
}

impl Drop for HostLock {
    fn drop(&mut self) {
        let _ = fs2::FileExt::unlock(&self.file);
    }
}
