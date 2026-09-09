//! Repository index generation, querying, and verification.

use std::collections::BTreeMap;
use std::fs::File;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use ed25519_dalek::SigningKey;
use heed::types::{Bytes, Str};
use heed::{Env, EnvFlags, EnvOpenOptions};
use sage_core::hex;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

use crate::error::RepoError;
use crate::sign::{decode_fixed, sign_file};

static TEMP_ID: AtomicU64 = AtomicU64::new(0);

/// Release record stored in the repository packages table.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct IndexedRelease {
    pub package: sage_core::Package,
    pub archive: String,
    pub sha256: String,
}

impl IndexedRelease {
    /// Returns the coordinate encoded by the indexed package manifest.
    pub fn coordinate(&self) -> sage_core::PackageCoordinate {
        self.package.coordinate()
    }

    /// Returns the coordinate with the canonical channel assigned by its index.
    pub fn coordinate_for_channel(&self, channel: &str) -> sage_core::PackageCoordinate {
        self.package.coordinate_for_channel(channel)
    }
}

/// Retrieval location attached to a package after resolution.
#[derive(Debug, Clone)]
pub enum ReleaseLocation {
    Remote(String),
    Local(PathBuf),
}

/// Repository release plus the transport and installation context supplied by the caller.
#[derive(Debug, Clone)]
pub struct ReleaseSource {
    pub release: IndexedRelease,
    pub location: ReleaseLocation,
    pub target_root: PathBuf,
}

/// Files emitted by one repository indexing pass.
#[derive(Debug, Clone)]
pub struct IndexArtifacts {
    pub index: PathBuf,
    pub compressed: PathBuf,
    pub signature: PathBuf,
    pub packages: usize,
}

/// Builds, signs, and compresses the schema-v1 single-file LMDB index.
pub fn build_index(
    pool: &Path,
    output_dir: &Path,
    signing_key: &Path,
) -> Result<IndexArtifacts, RepoError> {
    std::fs::create_dir_all(output_dir)?;
    let mut package_files: Vec<_> = sage_core::walkdir::WalkDir::new(pool)
        .follow_links(false)
        .into_iter()
        .filter_map(|entry| entry.ok())
        .map(|entry| entry.into_path())
        .filter(|path| {
            path.file_name()
                .and_then(|name| name.to_str())
                .is_some_and(|name| name.ends_with(".pkg.tar.zst"))
        })
        .collect();
    package_files.sort();
    let mut releases: BTreeMap<String, Vec<IndexedRelease>> = BTreeMap::new();
    let mut providers: BTreeMap<String, Vec<String>> = BTreeMap::new();
    for path in &package_files {
        let inspection = sage_archive::inspect_package(path)
            .map_err(|error| RepoError::InvalidConfig(error.to_string()))?;
        let key = format!("{}:{}", inspection.manifest.name, inspection.manifest.slot);
        for symbol in &inspection.manifest.provides {
            let entries = providers.entry(symbol.clone()).or_default();
            if !entries.contains(&key) {
                entries.push(key.clone());
            }
        }
        releases.entry(key).or_default().push(IndexedRelease {
            package: inspection.manifest,
            archive: path
                .strip_prefix(pool)
                .unwrap_or(path.as_path())
                .to_string_lossy()
                .into_owned(),
            sha256: hash_file(path)?,
        });
    }
    for versions in releases.values_mut() {
        versions.sort_by_key(release_version);
    }
    let temporary = temporary_path(&output_dir.join("index.mdb"));
    {
        let mut options = EnvOpenOptions::new();
        options.map_size(1024 * 1024 * 1024).max_dbs(8);
        // SAFETY: this new temporary file has exactly one writer until publication.
        unsafe {
            options.flags(EnvFlags::NO_SUB_DIR);
        }
        let env = unsafe { options.open(&temporary)? };
        let mut txn = env.write_txn()?;
        let packages: heed::Database<Str, Bytes> =
            env.create_database(&mut txn, Some("packages"))?;
        let provides: heed::Database<Str, Bytes> =
            env.create_database(&mut txn, Some("provides"))?;
        let dependencies: heed::Database<Str, Bytes> =
            env.create_database(&mut txn, Some("dependencies"))?;
        let metadata: heed::Database<Str, Str> = env.create_database(&mut txn, Some("metadata"))?;
        for (key, versions) in &releases {
            packages.put(&mut txn, key, &bincode::serialize(versions)?)?;
            let latest_dependencies = versions
                .last()
                .map(|release| &release.package.dependencies)
                .unwrap();
            dependencies.put(&mut txn, key, &bincode::serialize(latest_dependencies)?)?;
        }
        for (symbol, entries) in &providers {
            provides.put(&mut txn, symbol, &bincode::serialize(entries)?)?;
        }
        metadata.put(
            &mut txn,
            "schema_version",
            &sage_core::SCHEMA_VERSION.to_string(),
        )?;
        metadata.put(&mut txn, "timestamp", &unix_timestamp()?.to_string())?;
        txn.commit()?;
        env.force_sync()?;
    }
    let key = decode_fixed::<32>(&std::fs::read(signing_key)?)?;
    let signature = sign_file(&temporary, &SigningKey::from_bytes(&key))?;
    let signature_temporary = temporary.with_extension("sig");
    std::fs::write(&signature_temporary, signature.to_bytes())?;
    let compressed_temporary = temporary.with_extension("zst");
    compress_file(&temporary, &compressed_temporary)?;
    let index = output_dir.join("index.mdb");
    let signature_path = output_dir.join("index.mdb.sig");
    let compressed = output_dir.join("index.mdb.zst");
    // The signature is the commit marker fetched by clients, so publish it last.
    std::fs::rename(compressed_temporary, &compressed)?;
    std::fs::rename(temporary, &index)?;
    std::fs::rename(signature_temporary, &signature_path)?;
    Ok(IndexArtifacts {
        index,
        compressed,
        signature: signature_path,
        packages: package_files.len(),
    })
}

fn release_version(release: &IndexedRelease) -> sage_core::Version {
    release.package.coordinate().version
}

pub(crate) fn compress_file(source: &Path, destination: &Path) -> Result<(), RepoError> {
    let mut input = File::open(source)?;
    let mut encoder = zstd::Encoder::new(File::create(destination)?, 15)?;
    encoder.include_checksum(true)?;
    std::io::copy(&mut input, &mut encoder)?;
    encoder.finish()?.sync_all()?;
    Ok(())
}

/// Decompresses one zstd file into a durable destination.
pub fn decompress(source: &Path, destination: &Path) -> Result<(), RepoError> {
    let mut decoder = zstd::Decoder::new(File::open(source)?)?;
    let mut output = File::create(destination)?;
    std::io::copy(&mut decoder, &mut output)?;
    output.flush()?;
    output.sync_all()?;
    Ok(())
}

fn unix_timestamp() -> Result<u64, RepoError> {
    Ok(std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(|error| RepoError::InvalidConfig(error.to_string()))?
        .as_secs())
}

/// Opens an immutable single-file LMDB index through a read-only mmap.
pub fn open_index(path: &Path) -> Result<Env, RepoError> {
    let mut options = EnvOpenOptions::new();
    options.max_dbs(8);
    // SAFETY: repository indexes are immutable after atomic publication. NO_LOCK
    // is therefore safe, and NO_SUB_DIR matches the protocol's single data file.
    unsafe {
        options.flags(EnvFlags::READ_ONLY | EnvFlags::NO_LOCK | EnvFlags::NO_SUB_DIR);
    }
    Ok(unsafe { options.open(path)? })
}

/// Reads the publication timestamp embedded inside an index's metadata table.
pub fn read_index_timestamp(path: &Path) -> Result<Option<u64>, RepoError> {
    if !path.exists() {
        return Ok(None);
    }
    let env = match open_index(path) {
        Ok(env) => env,
        Err(_) => return Ok(None),
    };
    let txn = env.read_txn()?;
    let metadata: Option<heed::Database<Str, Str>> = env.open_database(&txn, Some("metadata"))?;
    if let Some(db) = metadata
        && let Some(ts_str) = db.get(&txn, "timestamp")?
        && let Ok(ts) = ts_str.parse::<u64>()
    {
        return Ok(Some(ts));
    }
    Ok(None)
}

/// Read-only typed view over a verified repository index.
pub struct RepositoryIndex {
    env: Env,
}

impl RepositoryIndex {
    pub fn open(path: &Path) -> Result<Self, RepoError> {
        let env = open_index(path)?;
        let txn = env.read_txn()?;
        env.open_database::<Str, Bytes>(&txn, Some("packages"))?
            .ok_or_else(|| RepoError::InvalidConfig("index has no packages table".into()))?;
        env.open_database::<Str, Bytes>(&txn, Some("provides"))?
            .ok_or_else(|| RepoError::InvalidConfig("index has no provides table".into()))?;
        drop(txn);
        Ok(Self { env })
    }

    /// Performs one point lookup for all versions of `name:slot`.
    pub fn releases(&self, name: &str, slot: &str) -> Result<Vec<IndexedRelease>, RepoError> {
        let txn = self.env.read_txn()?;
        let packages: heed::Database<Str, Bytes> = self
            .env
            .open_database(&txn, Some("packages"))?
            .ok_or_else(|| RepoError::InvalidConfig("index has no packages table".into()))?;
        let key = format!("{name}:{slot}");
        packages
            .get(&txn, &key)?
            .map(bincode::deserialize)
            .transpose()
            .map(Option::unwrap_or_default)
            .map_err(Into::into)
    }

    /// Iterates owned records while keeping mmap slices inside the read transaction.
    pub fn all_releases(&self) -> Result<Vec<IndexedRelease>, RepoError> {
        let txn = self.env.read_txn()?;
        let packages: heed::Database<Str, Bytes> = self
            .env
            .open_database(&txn, Some("packages"))?
            .ok_or_else(|| RepoError::InvalidConfig("index has no packages table".into()))?;
        let mut releases = Vec::new();
        for item in packages.iter(&txn)? {
            let (_, bytes) = item?;
            releases.extend(bincode::deserialize::<Vec<IndexedRelease>>(bytes)?);
        }
        Ok(releases)
    }

    pub fn providers(&self, symbol: &str) -> Result<Vec<String>, RepoError> {
        let txn = self.env.read_txn()?;
        let provides: heed::Database<Str, Bytes> = self
            .env
            .open_database(&txn, Some("provides"))?
            .ok_or_else(|| RepoError::InvalidConfig("index has no provides table".into()))?;
        provides
            .get(&txn, symbol)?
            .map(bincode::deserialize)
            .transpose()
            .map(Option::unwrap_or_default)
            .map_err(Into::into)
    }
}

pub(crate) fn hash_file(path: &Path) -> Result<String, RepoError> {
    let mut file = File::open(path)?;
    let mut hasher = Sha256::new();
    let mut buffer = [0; 128 * 1024];
    loop {
        let read = file.read(&mut buffer)?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
    }
    Ok(hex::encode(hasher.finalize()))
}

pub(crate) fn temporary_path(destination: &Path) -> PathBuf {
    destination.with_extension(format!(
        "sage-tmp-{}-{}",
        std::process::id(),
        TEMP_ID.fetch_add(1, Ordering::Relaxed)
    ))
}
