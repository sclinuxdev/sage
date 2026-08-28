//! Transactional LMDB state, ownership indexes, and crash journals.

use heed::types::{Bytes, Str};
use heed::{Database, Env, EnvFlags, EnvOpenOptions, RoTxn, RwTxn};
use sage_core::{CoreError, Dependency, PackageKey, Version};
use serde::de::DeserializeOwned;
use serde::{Deserialize, Serialize};
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
    pub files: Vec<String>,
    /// Original package hashes for three-way configuration upgrades.
    pub config_hashes: BTreeMap<String, String>,
}

/// Durable operation marker used for idempotent forward recovery.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct JournalRecord {
    pub op_id: String,
    pub stage: String,
    pub affected_packages: Vec<PackageKey>,
    pub journal_sha256: String,
    pub timestamp: u64,
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
        let db_path = path.as_ref().to_path_buf();
        fs::create_dir_all(&db_path)?;
        // SAFETY: Sage owns this directory, uses one fixed map size for every opener,
        // and never opens the same LMDB files through a second environment in-process.
        let env = unsafe {
            EnvOpenOptions::new()
                .map_size(MAP_SIZE)
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
        let id = package.key.canonical_id();
        // Replacing an installed version is one transaction: remove reverse
        // indexes that disappeared before validating and adding the new set.
        if let Some(previous) = get_owned::<InstalledPackage>(&self.packages, &txn, &id)? {
            for path in previous.files {
                if !package.files.contains(&path) {
                    remove_member(&self.files, &mut txn, &path, &package.key)?;
                }
            }
            for symbol in previous.provides {
                if !package.provides.contains(&symbol) {
                    remove_member(&self.provides, &mut txn, &symbol, &package.key)?;
                }
            }
        }
        for path in &package.files {
            let mut owners: Vec<PackageKey> =
                get_owned(&self.files, &txn, path)?.unwrap_or_default();
            if !owners.is_empty() && !owners.contains(&package.key) && !allow_shared {
                return Err(DbError::FileConflict {
                    path: path.clone(),
                    owners,
                });
            }
            if !owners.contains(&package.key) {
                owners.push(package.key.clone());
                put_encoded(&self.files, &mut txn, path, &owners)?;
            }
        }
        for symbol in &package.provides {
            let mut providers: Vec<PackageKey> =
                get_owned(&self.provides, &txn, symbol)?.unwrap_or_default();
            if !providers.contains(&package.key) {
                providers.push(package.key.clone());
                put_encoded(&self.provides, &mut txn, symbol, &providers)?;
            }
        }
        put_encoded(&self.packages, &mut txn, &id, package)?;
        txn.commit()?;
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

    pub fn providers(&self, symbol: &str) -> Result<Vec<PackageKey>, DbError> {
        let txn = self.env.read_txn()?;
        Ok(get_owned(&self.provides, &txn, symbol)?.unwrap_or_default())
    }

    /// Pins a system interface such as `virtual/libc` to one package instance.
    pub fn set_system_provider(&self, interface: &str, key: &PackageKey) -> Result<(), DbError> {
        let mut txn = self.env.write_txn()?;
        self.system.put(&mut txn, interface, &key.canonical_id())?;
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

#[cfg(test)]
mod tests {
    use super::*;

    fn package(name: &str, file: &str) -> InstalledPackage {
        InstalledPackage {
            key: PackageKey::new("main/system", name, "0"),
            version: Version::new(0, "1.0", 1),
            arch: "amd64".into(),
            installed_size: 10,
            dependencies: vec![],
            provides: vec![format!("cmd:{name}")],
            files: vec![file.into()],
            config_hashes: BTreeMap::new(),
        }
    }

    #[test]
    fn install_indexes_and_remove_are_atomic() {
        let dir = tempfile::tempdir().unwrap();
        let db = SageDatabase::open(dir.path()).unwrap();
        let rg = package("ripgrep", "usr/bin/rg");
        db.install(&rg, false).unwrap();
        drop(db);
        assert_eq!(read_packages(dir.path()).unwrap(), vec![rg.clone()]);
        let db = SageDatabase::open(dir.path()).unwrap();
        assert_eq!(db.package(&rg.key).unwrap(), Some(rg.clone()));
        assert_eq!(db.owners("usr/bin/rg").unwrap(), vec![rg.key.clone()]);
        assert_eq!(db.providers("cmd:ripgrep").unwrap(), vec![rg.key.clone()]);
        assert_eq!(db.remove(&rg.key).unwrap(), Some(rg));
        assert!(db.owners("usr/bin/rg").unwrap().is_empty());
    }

    #[test]
    fn conflicts_do_not_leave_partial_indexes() {
        let dir = tempfile::tempdir().unwrap();
        let db = SageDatabase::open(dir.path()).unwrap();
        let first = package("one", "usr/bin/shared");
        let second = package("two", "usr/bin/shared");
        db.install(&first, false).unwrap();
        assert!(matches!(
            db.install(&second, false),
            Err(DbError::FileConflict { .. })
        ));
        assert!(db.package(&second.key).unwrap().is_none());
        assert!(db.providers("cmd:two").unwrap().is_empty());
    }

    #[test]
    fn upgrade_prunes_obsolete_reverse_indexes() {
        let dir = tempfile::tempdir().unwrap();
        let db = SageDatabase::open(dir.path()).unwrap();
        let first = package("demo", "usr/bin/old");
        let mut second = package("demo", "usr/bin/new");
        second.version = Version::new(0, "2.0", 1);
        second.provides = vec!["cmd:new-demo".into()];
        db.install(&first, false).unwrap();
        db.install(&second, false).unwrap();
        assert!(db.owners("usr/bin/old").unwrap().is_empty());
        assert!(db.providers("cmd:demo").unwrap().is_empty());
        assert_eq!(db.owners("usr/bin/new").unwrap(), vec![second.key]);
    }

    #[test]
    fn journals_survive_reopen() {
        let dir = tempfile::tempdir().unwrap();
        let record = JournalRecord {
            op_id: "op-1".into(),
            stage: "publish".into(),
            affected_packages: vec![],
            journal_sha256: "00".repeat(32),
            timestamp: 1,
        };
        SageDatabase::open(dir.path())
            .unwrap()
            .write_journal(&record)
            .unwrap();
        let reopened = SageDatabase::open(dir.path()).unwrap();
        assert_eq!(reopened.pending_journals().unwrap(), vec![record]);
        assert!(reopened.finish_journal("op-1").unwrap());
    }
}
