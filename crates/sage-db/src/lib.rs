//! Transactional LMDB state, ownership indexes, and crash journals.
use heed::types::{Bytes, Str};
use heed::{Database, Env, EnvFlags, EnvOpenOptions, RoTxn, RwTxn};
use sage_core::{CoreError, Dependency, PackageKey, Version};
use serde::de::DeserializeOwned;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use thiserror::Error;
const MAP_SIZE: usize = 8 * 1024 * 1024 * 1024;
/// Persistent-state failures.
#[derive(Debug, Error)]
pub enum DbError {
    #[error("LMDB error: {0}")]
    Heed(#[from] heed::Error),
    #[error("serialization error: {0}")]
    Serialization(#[from] bincode::Error),
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
    #[error("invalid core value: {0}")]
    Core(#[from] CoreError),
    #[error("path '{path}' is already owned by {owners:?}")]
    FileConflict {
        path: String,
        owners: Vec<PackageKey>,
    },
    #[error("operation journal '{0}' failed its integrity check")]
    InvalidJournal(String),
}
/// Complete installed state required for removal and reconciliation.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct InstalledPackage {
    pub key: PackageKey,
    pub version: Version,
    pub arch: String,
    pub installed_size: u64,
    pub dependencies: Vec<Dependency>,
    pub provides: Vec<String>,
    pub conflicts: Vec<String>,
    pub files: Vec<String>,
    /// Original package hashes keyed by exact physical ownership path.
    pub config_hashes: BTreeMap<String, String>,
}
/// Exact declarative rebuild tail replayed after package publication recovers.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct RebuildContinuation {
    pub provider_bindings: BTreeMap<String, PackageKey>,
    pub system_config: Vec<u8>,
}
/// Recovery inputs; metadata stays opaque to avoid reverse crate dependencies.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum JournalAction {
    Install {
        architecture: String,
        changes: Vec<(PackageKey, Version)>,
        /// Pre-upgrade records retained until obsolete paths are removed.
        previous_packages: Vec<InstalledPackage>,
        retired_packages: Vec<InstalledPackage>,
        modified_paths: Vec<String>,
        removed_paths: Vec<String>,
        previous_alternative_documents: Vec<Vec<u8>>,
        removal_trigger_documents: Vec<Vec<u8>>,
        rebuild: Option<RebuildContinuation>,
    },
    Remove {
        packages: Vec<InstalledPackage>,
        modified_paths: Vec<String>,
        trigger_documents: Vec<Vec<u8>>,
        alternative_documents: Vec<Vec<u8>>,
    },
}
/// Durable operation marker used for idempotent forward recovery.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct JournalRecord {
    pub op_id: String,
    pub stage: String,
    pub journal_sha256: String,
    pub action: JournalAction,
}
impl JournalRecord {
    /// Creates a sealed journal that startup can integrity-check.
    pub fn new(op_id: String, stage: &str, action: JournalAction) -> Self {
        let mut record = Self {
            op_id,
            stage: stage.into(),
            journal_sha256: String::new(),
            action,
        };
        record.seal();
        record
    }
    pub fn advance(&mut self, stage: &str) {
        self.stage = stage.into();
        self.seal();
    }
    pub fn validate(&self) -> Result<(), DbError> {
        if self.journal_sha256 == record_digest(&self.op_id, &self.stage, &self.action)? {
            Ok(())
        } else {
            Err(DbError::InvalidJournal(self.op_id.clone()))
        }
    }
    fn seal(&mut self) {
        self.journal_sha256 = record_digest(&self.op_id, &self.stage, &self.action)
            .expect("serializing an in-memory journal action cannot fail");
    }
}
fn record_digest(op_id: &str, stage: &str, action: &JournalAction) -> Result<String, DbError> {
    Ok(hex::encode(Sha256::digest(bincode::serialize(&(
        op_id, stage, action,
    ))?)))
}
/// Named LMDB tables sharing one ACID environment.
pub struct SageDatabase {
    env: Env,
    packages: Database<Str, Bytes>,
    files: Database<Str, Bytes>,
    provides: Database<Str, Bytes>,
    system: Database<Str, Str>,
    operations: Database<Str, Bytes>,
    db_path: PathBuf,
}
impl SageDatabase {
    /// Opens the state directory and creates all schema-v1 tables atomically.
    pub fn open(path: impl AsRef<Path>) -> Result<Self, DbError> {
        Self::open_inner(path.as_ref(), MAP_SIZE)
    }
    /// Opens the state directory with an explicit LMDB map size for boundary tests.
    #[cfg(feature = "torture")]
    pub fn open_with_map_size(path: impl AsRef<Path>, map_size: usize) -> Result<Self, DbError> {
        Self::open_inner(path.as_ref(), map_size)
    }
    fn open_inner(path: &Path, map_size: usize) -> Result<Self, DbError> {
        let db_path = path.to_path_buf();
        fs::create_dir_all(&db_path)?;
        // SAFETY: Sage owns this directory, uses one fixed map size for every opener,
        // and never opens the same LMDB files through a second environment in-process.
        let env = unsafe {
            EnvOpenOptions::new()
                .map_size(map_size)
                .max_dbs(8)
                .open(&db_path)?
        };
        let mut txn = env.write_txn()?;
        let packages = env.create_database(&mut txn, Some("packages"))?;
        let files = env.create_database(&mut txn, Some("files"))?;
        let provides = env.create_database(&mut txn, Some("provides"))?;
        let system = env.create_database(&mut txn, Some("system"))?;
        let operations = env.create_database(&mut txn, Some("operations"))?;
        txn.commit()?;
        Ok(Self {
            env,
            packages,
            files,
            provides,
            system,
            operations,
            db_path,
        })
    }
    /// Returns the environment directory for diagnostics and cache management.
    pub fn path(&self) -> &Path {
        &self.db_path
    }
    /// Fetches one installed package with a single B+ tree lookup.
    pub fn package(&self, key: &PackageKey) -> Result<Option<InstalledPackage>, DbError> {
        let txn = self.env.read_txn()?;
        get_owned(&self.packages, &txn, &key.canonical_id())
    }
    /// Returns every installed package in LMDB key order.
    pub fn packages(&self) -> Result<Vec<InstalledPackage>, DbError> {
        let txn = self.env.read_txn()?;
        let packages: Vec<InstalledPackage> = self
            .packages
            .iter(&txn)?
            .map(|item| {
                let (_, bytes) = item?;
                decode::<InstalledPackage>(bytes)
            })
            .collect::<Result<Vec<_>, DbError>>()?;
        Ok(packages)
    }
    /// Publishes a package and all reverse indexes in one write transaction.
    pub fn install(&self, package: &InstalledPackage, allow_shared: bool) -> Result<(), DbError> {
        let mut txn = self.env.write_txn()?;
        self.install_in_txn(&mut txn, package, allow_shared)?;
        txn.commit()?;
        Ok(())
    }
    /// Publishes a complete package set in one all-or-nothing LMDB transaction.
    #[cfg(feature = "torture")]
    pub fn install_batch(
        &self,
        packages: &[InstalledPackage],
        allow_shared: bool,
    ) -> Result<(), DbError> {
        let mut txn = self.env.write_txn()?;
        for package in packages {
            self.install_in_txn(&mut txn, package, allow_shared)?;
        }
        txn.commit()?;
        Ok(())
    }
    fn install_in_txn(
        &self,
        txn: &mut RwTxn<'_>,
        package: &InstalledPackage,
        allow_shared: bool,
    ) -> Result<(), DbError> {
        let id = package.key.canonical_id();
        // Replacing an installed version is one transaction: remove reverse
        // indexes that disappeared before validating and adding the new set.
        if let Some(previous) = get_owned::<InstalledPackage>(&self.packages, txn, &id)? {
            for path in previous.files {
                if !package.files.contains(&path) {
                    remove_member(&self.files, txn, &path, &package.key)?;
                }
            }
            for symbol in previous.provides {
                if !package.provides.contains(&symbol) {
                    remove_member(&self.provides, txn, &symbol, &package.key)?;
                }
            }
        }
        for path in &package.files {
            let mut owners: Vec<PackageKey> =
                get_owned(&self.files, txn, path)?.unwrap_or_default();
            if !owners.is_empty() && !owners.contains(&package.key) && !allow_shared {
                return Err(DbError::FileConflict {
                    path: path.clone(),
                    owners,
                });
            }
            if !owners.contains(&package.key) {
                owners.push(package.key.clone());
                put_encoded(&self.files, txn, path, &owners)?;
            }
        }
        for symbol in &package.provides {
            let mut providers: Vec<PackageKey> =
                get_owned(&self.provides, txn, symbol)?.unwrap_or_default();
            if !providers.contains(&package.key) {
                providers.push(package.key.clone());
                put_encoded(&self.provides, txn, symbol, &providers)?;
            }
        }
        put_encoded(&self.packages, txn, &id, package)?;
        Ok(())
    }
    /// Removes a package and prunes empty ownership/provider entries atomically.
    pub fn remove(&self, key: &PackageKey) -> Result<Option<InstalledPackage>, DbError> {
        let mut txn = self.env.write_txn()?;
        let id = key.canonical_id();
        let Some(package): Option<InstalledPackage> = get_owned(&self.packages, &txn, &id)? else {
            return Ok(None);
        };
        for path in &package.files {
            remove_member(&self.files, &mut txn, path, key)?;
        }
        for symbol in &package.provides {
            remove_member(&self.provides, &mut txn, symbol, key)?;
        }
        self.packages.delete(&mut txn, &id)?;
        txn.commit()?;
        Ok(Some(package))
    }
    pub fn owners(&self, path: &str) -> Result<Vec<PackageKey>, DbError> {
        let txn = self.env.read_txn()?;
        Ok(get_owned(&self.files, &txn, path)?.unwrap_or_default())
    }
    /// Returns the complete reverse file-ownership index in LMDB key order.
    pub fn file_owners(&self) -> Result<BTreeMap<String, Vec<PackageKey>>, DbError> {
        let txn = self.env.read_txn()?;
        let owners = self
            .files
            .iter(&txn)?
            .map(|entry| {
                let (path, bytes) = entry?;
                Ok((path.into(), decode(bytes)?))
            })
            .collect();
        owners
    }
    pub fn providers(&self, symbol: &str) -> Result<Vec<PackageKey>, DbError> {
        let txn = self.env.read_txn()?;
        Ok(get_owned(&self.provides, &txn, symbol)?.unwrap_or_default())
    }
    /// Replaces every persisted system-provider binding in one transaction.
    pub fn replace_system_providers(
        &self,
        bindings: &BTreeMap<String, PackageKey>,
    ) -> Result<(), DbError> {
        let mut txn = self.env.write_txn()?;
        self.system.clear(&mut txn)?;
        for (interface, key) in bindings {
            self.system.put(&mut txn, interface, &key.canonical_id())?;
        }
        txn.commit()?;
        Ok(())
    }
    pub fn system_provider(&self, interface: &str) -> Result<Option<PackageKey>, DbError> {
        let txn = self.env.read_txn()?;
        Ok(self
            .system
            .get(&txn, interface)?
            .map(str::parse)
            .transpose()?)
    }
    /// Starts or advances an operation by replacing its durable journal record.
    pub fn write_journal(&self, record: &JournalRecord) -> Result<(), DbError> {
        record.validate()?;
        let mut txn = self.env.write_txn()?;
        put_encoded(&self.operations, &mut txn, &record.op_id, record)?;
        txn.commit()?;
        Ok(())
    }
    pub fn finish_journal(&self, op_id: &str) -> Result<bool, DbError> {
        let mut txn = self.env.write_txn()?;
        let removed = self.operations.delete(&mut txn, op_id)?;
        txn.commit()?;
        Ok(removed)
    }
    /// Lists unfinished operations for the startup forward-recovery pass.
    pub fn pending_journals(&self) -> Result<Vec<JournalRecord>, DbError> {
        let txn = self.env.read_txn()?;
        let records: Vec<JournalRecord> = self
            .operations
            .iter(&txn)?
            .map(|item| {
                let (_, bytes) = item?;
                decode::<JournalRecord>(bytes)
            })
            .collect::<Result<Vec<_>, DbError>>()?;
        Ok(records)
    }
}
/// Reads installed packages without creating or writing the state environment.
pub fn read_packages(path: &Path) -> Result<Vec<InstalledPackage>, DbError> {
    if !path.exists() {
        return Ok(Vec::new());
    }
    let mut options = EnvOpenOptions::new();
    options.max_dbs(8);
    // SAFETY: this environment performs no writes and retains LMDB locking.
    unsafe {
        options.flags(EnvFlags::READ_ONLY);
    }
    let env = unsafe { options.open(path)? };
    let txn = env.read_txn()?;
    let packages: Database<Str, Bytes> =
        env.open_database(&txn, Some("packages"))?.ok_or_else(|| {
            DbError::Io(std::io::Error::new(
                std::io::ErrorKind::InvalidData,
                "missing packages table",
            ))
        })?;
    let mut records = Vec::new();
    for item in packages.iter(&txn)? {
        let (_, bytes) = item?;
        records.push(decode(bytes)?);
    }
    Ok(records)
}
/// Reads resolved system bindings without creating or writing the environment.
/// Returns an empty map for an absent environment, or an error for invalid state.
pub fn read_system_providers(path: &Path) -> Result<BTreeMap<String, PackageKey>, DbError> {
    if !path.exists() {
        return Ok(BTreeMap::new());
    }
    let mut options = EnvOpenOptions::new();
    options.max_dbs(8);
    // SAFETY: this environment performs no writes and retains LMDB locking.
    unsafe {
        options.flags(EnvFlags::READ_ONLY);
    }
    let env = unsafe { options.open(path)? };
    let txn = env.read_txn()?;
    let system: Database<Str, Str> = env.open_database(&txn, Some("system"))?.ok_or_else(|| {
        DbError::Io(std::io::Error::new(
            std::io::ErrorKind::InvalidData,
            "missing system table",
        ))
    })?;
    let mut bindings = BTreeMap::new();
    for entry in system.iter(&txn)? {
        let (interface, key) = entry?;
        bindings.insert(interface.into(), key.parse()?);
    }
    Ok(bindings)
}
/// Reads one file-owner list without opening a write transaction.
pub fn read_owners(path: &Path, file: &str) -> Result<Vec<PackageKey>, DbError> {
    if !path.exists() {
        return Ok(Vec::new());
    }
    let mut options = EnvOpenOptions::new();
    options.max_dbs(8);
    // SAFETY: this environment is strictly read-only and keeps LMDB locking enabled.
    unsafe {
        options.flags(EnvFlags::READ_ONLY);
    }
    let env = unsafe { options.open(path)? };
    let txn = env.read_txn()?;
    let files: Database<Str, Bytes> = env.open_database(&txn, Some("files"))?.ok_or_else(|| {
        DbError::Io(std::io::Error::new(
            std::io::ErrorKind::InvalidData,
            "missing files table",
        ))
    })?;
    Ok(files
        .get(&txn, file)?
        .map(decode)
        .transpose()?
        .unwrap_or_default())
}
fn encode<T: Serialize + ?Sized>(value: &T) -> Result<Vec<u8>, DbError> {
    Ok(bincode::serialize(value)?)
}
fn decode<T: DeserializeOwned>(bytes: &[u8]) -> Result<T, DbError> {
    Ok(bincode::deserialize(bytes)?)
}
fn get_owned<T: DeserializeOwned>(
    database: &Database<Str, Bytes>,
    txn: &RoTxn<'_>,
    key: &str,
) -> Result<Option<T>, DbError> {
    database.get(txn, key)?.map(decode).transpose()
}
fn put_encoded<T: Serialize + ?Sized>(
    database: &Database<Str, Bytes>,
    txn: &mut RwTxn<'_>,
    key: &str,
    value: &T,
) -> Result<(), DbError> {
    database.put(txn, key, &encode(value)?)?;
    Ok(())
}
fn remove_member(
    database: &Database<Str, Bytes>,
    txn: &mut RwTxn<'_>,
    key: &str,
    member: &PackageKey,
) -> Result<(), DbError> {
    let mut members: Vec<PackageKey> = get_owned(database, txn, key)?.unwrap_or_default();
    members.retain(|candidate| candidate != member);
    if members.is_empty() {
        database.delete(txn, key)?;
    } else {
        put_encoded(database, txn, key, &members)?;
    }
    Ok(())
}
