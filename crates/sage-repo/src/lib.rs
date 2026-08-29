//! Conditional repository synchronization, signature verification, and range downloads.

use ed25519_dalek::{Signature, Signer, SigningKey, Verifier, VerifyingKey};
use heed::types::{Bytes, Str};
use heed::{Env, EnvFlags, EnvOpenOptions};
use reqwest::header::{ACCEPT_RANGES, CONTENT_LENGTH, ETAG, IF_NONE_MATCH, RANGE};
use reqwest::{Client, Method, StatusCode};
use serde::Deserialize;
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::fs::File;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;
use thiserror::Error;
use tokio::io::AsyncWriteExt;

const CHUNKS: u64 = 4;
const MAX_REQUEST_ATTEMPTS: usize = 4;
const RETRY_BACKOFF_MS: u64 = 250;
static TEMP_ID: AtomicU64 = AtomicU64::new(0);

/// Repository transfer, verification, and index failures.
#[derive(Debug, Error)]
pub enum RepoError {
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
    #[error("network error: {0}")]
    Network(#[from] reqwest::Error),
    #[error("LMDB error: {0}")]
    Heed(#[from] heed::Error),
    #[error("invalid repository configuration: {0}")]
    InvalidConfig(String),
    #[error("signature verification failed")]
    Signature,
    #[error("SHA-256 mismatch: expected {expected}, calculated {actual}")]
    Checksum { expected: String, actual: String },
    #[error("all mirrors failed: {0}")]
    Mirrors(String),
    #[error("download task failed: {0}")]
    Join(#[from] tokio::task::JoinError),
    #[error("index serialization failed: {0}")]
    Serialization(#[from] bincode::Error),
}

/// One root repository and its nested subchannels.
#[derive(Debug, Clone, Deserialize)]
pub struct ChannelConfig {
    pub url: String,
    pub priority: i32,
    pub signing_key: PathBuf,
    #[serde(default = "enabled")]
    pub enabled: bool,
    #[serde(default)]
    pub subchannels: std::collections::BTreeMap<String, SubchannelConfig>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct SubchannelConfig {
    pub alias: Option<String>,
    #[serde(rename = "type")]
    pub channel_type: Option<String>,
    pub scope: String,
    pub target_root: PathBuf,
    #[serde(default = "enabled")]
    pub enabled: bool,
}

/// Complete `/etc/sage/channels.toml` document.
#[derive(Debug, Clone, Deserialize)]
pub struct ChannelsConfig {
    pub schema_version: u32,
    pub channels: std::collections::BTreeMap<String, ChannelConfig>,
}

impl ChannelsConfig {
    pub fn load(path: impl AsRef<Path>) -> Result<Self, RepoError> {
        let config: Self = toml::from_str(&std::fs::read_to_string(path)?)
            .map_err(|error| RepoError::InvalidConfig(error.to_string()))?;
        if config.schema_version != sage_core::SCHEMA_VERSION {
            return Err(RepoError::InvalidConfig(format!(
                "unsupported schema version {}",
                config.schema_version
            )));
        }
        for (name, channel) in &config.channels {
            if !valid_identifier(name)
                || channel.url.is_empty()
                || !safe_absolute_path(&channel.signing_key)
            {
                return Err(RepoError::InvalidConfig(format!(
                    "unsafe or incomplete channel '{name}'"
                )));
            }
            for (sub_name, subchannel) in &channel.subchannels {
                if !valid_identifier(sub_name)
                    || subchannel
                        .alias
                        .as_deref()
                        .is_some_and(|alias| !valid_identifier(alias))
                    || !safe_absolute_path(&subchannel.target_root)
                {
                    return Err(RepoError::InvalidConfig(format!(
                        "unsafe subchannel '{name}/{sub_name}'"
                    )));
                }
            }
        }
        Ok(config)
    }
}

fn valid_identifier(value: &str) -> bool {
    !value.is_empty()
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-'))
}

fn safe_absolute_path(path: &Path) -> bool {
    path.is_absolute()
        && path.components().all(|component| {
            matches!(
                component,
                std::path::Component::RootDir | std::path::Component::Normal(_)
            )
        })
}

/// Derives a subchannel URL solely from configuration values.
pub fn subchannel_url(
    channel: &ChannelConfig,
    name: &str,
    subchannel: &SubchannelConfig,
) -> String {
    join_url(&channel.url, subchannel.alias.as_deref().unwrap_or(name))
}

fn enabled() -> bool {
    true
}

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
    let mut package_files: Vec<_> = walkdir::WalkDir::new(pool)
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
    let mut releases: std::collections::BTreeMap<String, Vec<IndexedRelease>> =
        std::collections::BTreeMap::new();
    let mut providers: std::collections::BTreeMap<String, Vec<String>> =
        std::collections::BTreeMap::new();
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

fn sign_file(path: &Path, key: &SigningKey) -> Result<Signature, RepoError> {
    let file = File::open(path)?;
    // SAFETY: the published index is not mutated while the map is alive.
    let bytes = unsafe { memmap2::Mmap::map(&file)? };
    Ok(key.sign(&bytes))
}

fn compress_file(source: &Path, destination: &Path) -> Result<(), RepoError> {
    let mut input = File::open(source)?;
    let mut encoder = zstd::Encoder::new(File::create(destination)?, 15)?;
    encoder.include_checksum(true)?;
    std::io::copy(&mut input, &mut encoder)?;
    encoder.finish()?.sync_all()?;
    Ok(())
}

fn unix_timestamp() -> Result<u64, RepoError> {
    Ok(std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(|error| RepoError::InvalidConfig(error.to_string()))?
        .as_secs())
}

/// Reusable HTTP client and cache location.
pub struct DownloadEngine {
    client: Client,
    cache_dir: PathBuf,
}

impl DownloadEngine {
    pub fn new(cache_dir: impl Into<PathBuf>) -> Result<Self, RepoError> {
        let client = Client::builder()
            .user_agent(concat!("sage/", env!("CARGO_PKG_VERSION")))
            .http1_only()
            .connect_timeout(Duration::from_secs(15))
            .timeout(Duration::from_secs(300))
            .build()?;
        Ok(Self {
            client,
            cache_dir: cache_dir.into(),
        })
    }

    /// Downloads from the first working mirror and verifies the final stream.
    pub async fn download(
        &self,
        mirrors: &[String],
        relative: &str,
        destination: &Path,
        sha256: &str,
    ) -> Result<(), RepoError> {
        if sha256.len() != 64 || !sha256.bytes().all(|byte| byte.is_ascii_hexdigit()) {
            return Err(RepoError::InvalidConfig(
                "SHA-256 must contain 64 hex digits".into(),
            ));
        }
        let mut failures = Vec::new();
        for mirror in mirrors {
            let url = join_url(mirror, relative);
            match self.download_one(&url, destination, sha256).await {
                Ok(()) => return Ok(()),
                Err(error) => failures.push(format!("{url}: {error}")),
            }
        }
        Err(RepoError::Mirrors(failures.join("; ")))
    }

    /// Downloads one fully-qualified URL with the same atomic hash gate.
    pub async fn download_url(
        &self,
        url: &str,
        destination: &Path,
        sha256: &str,
    ) -> Result<(), RepoError> {
        self.download_one(url, destination, sha256).await
    }

    async fn download_one(
        &self,
        url: &str,
        destination: &Path,
        sha256: &str,
    ) -> Result<(), RepoError> {
        // The package cache is content-addressed. A verified local hit avoids
        // even a HEAD request while preserving the same integrity boundary as
        // a fresh transfer. Corrupt or stale files are atomically replaced.
        if destination.exists() && verify_hash(destination, sha256).await.is_ok() {
            return Ok(());
        }
        if let Some(parent) = destination.parent() {
            tokio::fs::create_dir_all(parent).await?;
        }
        // HEAD is an optimization only; mirrors that reject it still work through GET.
        let head = send_with_retries(&self.client, Method::HEAD, url, None)
            .await
            .ok()
            .filter(|response| response.status().is_success());
        let length = head
            .as_ref()
            .and_then(|response| response.headers().get(CONTENT_LENGTH))
            .and_then(|value| value.to_str().ok())
            .and_then(|value| value.parse::<u64>().ok());
        let ranges = head
            .as_ref()
            .and_then(|response| response.headers().get(ACCEPT_RANGES))
            .and_then(|value| value.to_str().ok())
            == Some("bytes");
        let temporary = temporary_path(destination);
        let mut cleanup = TempFiles::new([temporary.clone()]);
        if ranges && length.is_some_and(|value| value >= 4 * 1024 * 1024) {
            self.download_ranges(url, &temporary, length.unwrap())
                .await?;
        } else {
            download_response_with_retries(&self.client, url, None, &temporary, None).await?;
        }
        verify_hash(&temporary, sha256).await?;
        tokio::fs::rename(&temporary, destination).await?;
        cleanup.keep = true;
        Ok(())
    }

    async fn download_ranges(
        &self,
        url: &str,
        destination: &Path,
        length: u64,
    ) -> Result<(), RepoError> {
        let part_size = length.div_ceil(CHUNKS);
        let mut tasks = tokio::task::JoinSet::new();
        let mut parts = Vec::new();
        for index in 0..CHUNKS {
            let start = index * part_size;
            if start >= length {
                break;
            }
            let end = (start + part_size - 1).min(length - 1);
            let part = destination.with_extension(format!("part-{index}"));
            parts.push(part.clone());
            let client = self.client.clone();
            let url = url.to_owned();
            tasks.spawn(async move {
                let range = format!("bytes={start}-{end}");
                download_response_with_retries(
                    &client,
                    &url,
                    Some(&range),
                    &part,
                    Some(StatusCode::PARTIAL_CONTENT),
                )
                .await
            });
        }
        let _cleanup = TempFiles::new(parts.clone());
        while let Some(result) = tasks.join_next().await {
            result??;
        }
        let mut output = tokio::fs::File::create(destination).await?;
        for part in &parts {
            let mut input = tokio::fs::File::open(part).await?;
            tokio::io::copy(&mut input, &mut output).await?;
            tokio::fs::remove_file(part).await?;
        }
        output.sync_all().await?;
        Ok(())
    }

    /// Conditionally refreshes and verifies a channel's LMDB data file.
    pub async fn sync_index(
        &self,
        channel_url: &str,
        signing_key: &Path,
        destination: &Path,
    ) -> Result<bool, RepoError> {
        tokio::fs::create_dir_all(&self.cache_dir).await?;
        let etag_path = destination.with_extension("etag");
        let mut request = self.client.get(join_url(channel_url, "index.mdb.sig"));
        if let Ok(etag) = tokio::fs::read_to_string(&etag_path).await {
            request = request.header(IF_NONE_MATCH, etag);
        }
        let mut response = request.send().await?;
        if response.status() == StatusCode::NOT_MODIFIED && destination.exists() {
            return Ok(false);
        }
        if response.status() == StatusCode::NOT_MODIFIED {
            response = self
                .client
                .get(join_url(channel_url, "index.mdb.sig"))
                .send()
                .await?;
        }
        let response = response.error_for_status()?;
        let etag = response
            .headers()
            .get(ETAG)
            .and_then(|value| value.to_str().ok())
            .map(str::to_owned);
        let signature = response.bytes().await?.to_vec();
        let compressed = temporary_path(destination).with_extension("zst");
        let uncompressed = temporary_path(destination);
        let mut cleanup = TempFiles::new([compressed.clone(), uncompressed.clone()]);
        stream_response(
            self.client
                .get(join_url(channel_url, "index.mdb.zst"))
                .send()
                .await?
                .error_for_status()?,
            &compressed,
        )
        .await?;
        let source = compressed.clone();
        let target = uncompressed.clone();
        tokio::task::spawn_blocking(move || decompress(&source, &target)).await??;
        tokio::fs::remove_file(&compressed).await?;
        verify_signature(&uncompressed, signing_key, &signature).await?;
        tokio::fs::rename(&uncompressed, destination).await?;
        if let Some(etag) = etag {
            tokio::fs::write(etag_path, etag).await?;
        }
        cleanup.keep = true;
        Ok(true)
    }
}

fn retryable_status(status: StatusCode) -> bool {
    status == StatusCode::REQUEST_TIMEOUT
        || status == StatusCode::TOO_MANY_REQUESTS
        || status.is_server_error()
}

async fn send_with_retries(
    client: &Client,
    method: Method,
    url: &str,
    range: Option<&str>,
) -> Result<reqwest::Response, reqwest::Error> {
    for attempt in 0..MAX_REQUEST_ATTEMPTS {
        let mut request = client.request(method.clone(), url);
        if let Some(range) = range {
            request = request.header(RANGE, range);
        }
        match request.send().await {
            Ok(response)
                if retryable_status(response.status()) && attempt + 1 < MAX_REQUEST_ATTEMPTS =>
            {
                tokio::time::sleep(Duration::from_millis(
                    RETRY_BACKOFF_MS * 2u64.pow(attempt as u32),
                ))
                .await;
            }
            Ok(response) => return Ok(response),
            Err(_error) if attempt + 1 < MAX_REQUEST_ATTEMPTS => {
                tokio::time::sleep(Duration::from_millis(
                    RETRY_BACKOFF_MS * 2u64.pow(attempt as u32),
                ))
                .await;
            }
            Err(error) => return Err(error),
        }
    }
    unreachable!("request retry loop must return on its final attempt")
}

async fn stream_response(mut response: reqwest::Response, path: &Path) -> Result<(), RepoError> {
    let mut output = tokio::fs::File::create(path).await?;
    while let Some(chunk) = response.chunk().await? {
        output.write_all(&chunk).await?;
    }
    output.sync_all().await?;
    Ok(())
}

/// Downloads a response body with retries that cover both response setup and
/// streaming. A successful HTTP response can still fail while reading its
/// body, especially through slow mirrors or proxy connections, so retrying
/// only `send()` is insufficient. Each attempt writes to the same temporary
/// path and removes a partial body before the next attempt.
async fn download_response_with_retries(
    client: &Client,
    url: &str,
    range: Option<&str>,
    path: &Path,
    expected_status: Option<StatusCode>,
) -> Result<(), RepoError> {
    for attempt in 0..MAX_REQUEST_ATTEMPTS {
        let response = match send_with_retries(client, Method::GET, url, range).await {
            Ok(response) => response,
            Err(_error) if attempt + 1 < MAX_REQUEST_ATTEMPTS => {
                tokio::time::sleep(Duration::from_millis(
                    RETRY_BACKOFF_MS * 2u64.pow(attempt as u32),
                ))
                .await;
                continue;
            }
            Err(error) => return Err(error.into()),
        };
        if let Some(expected_status) = expected_status {
            if response.status() != expected_status {
                return Err(RepoError::InvalidConfig(
                    "server ignored a range request".into(),
                ));
            }
        } else if !response.status().is_success() {
            return Err(response.error_for_status().unwrap_err().into());
        }
        match stream_response(response, path).await {
            Ok(()) => return Ok(()),
            Err(_error) if attempt + 1 < MAX_REQUEST_ATTEMPTS => {
                let _ = tokio::fs::remove_file(path).await;
                tokio::time::sleep(Duration::from_millis(
                    RETRY_BACKOFF_MS * 2u64.pow(attempt as u32),
                ))
                .await;
            }
            Err(error) => return Err(error),
        }
    }
    unreachable!("response retry loop must return on its final attempt")
}

async fn verify_hash(path: &Path, expected: &str) -> Result<(), RepoError> {
    let path = path.to_path_buf();
    let actual = tokio::task::spawn_blocking(move || hash_file(&path)).await??;
    if actual.eq_ignore_ascii_case(expected) {
        Ok(())
    } else {
        Err(RepoError::Checksum {
            expected: expected.to_ascii_lowercase(),
            actual,
        })
    }
}

fn hash_file(path: &Path) -> Result<String, RepoError> {
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

/// Decompresses one zstd file into a durable destination.
pub fn decompress(source: &Path, destination: &Path) -> Result<(), RepoError> {
    let mut decoder = zstd::Decoder::new(File::open(source)?)?;
    let mut output = File::create(destination)?;
    std::io::copy(&mut decoder, &mut output)?;
    output.flush()?;
    output.sync_all()?;
    Ok(())
}

async fn verify_signature(path: &Path, key_path: &Path, signature: &[u8]) -> Result<(), RepoError> {
    let key = decode_fixed::<32>(&tokio::fs::read(key_path).await?)?;
    let signature = decode_fixed::<64>(signature)?;
    let path = path.to_path_buf();
    tokio::task::spawn_blocking(move || {
        let file = File::open(path)?;
        // SAFETY: the temporary index is immutable for the lifetime of this map.
        let bytes = unsafe { memmap2::Mmap::map(&file)? };
        let key = VerifyingKey::from_bytes(&key).map_err(|_| RepoError::Signature)?;
        key.verify(&bytes, &Signature::from_bytes(&signature))
            .map_err(|_| RepoError::Signature)
    })
    .await?
}

/// Decodes a fixed-size raw or hexadecimal byte sequence.
pub fn decode_fixed<const N: usize>(bytes: &[u8]) -> Result<[u8; N], RepoError> {
    let decoded = if bytes.len() == N {
        bytes.to_vec()
    } else {
        let text = std::str::from_utf8(bytes)
            .map_err(|_| RepoError::Signature)?
            .trim();
        hex::decode(text).map_err(|_| RepoError::Signature)?
    };
    decoded.try_into().map_err(|_| RepoError::Signature)
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

fn join_url(base: &str, relative: &str) -> String {
    format!(
        "{}/{}",
        base.trim_end_matches('/'),
        relative.trim_start_matches('/')
    )
}

fn temporary_path(destination: &Path) -> PathBuf {
    destination.with_extension(format!(
        "sage-tmp-{}-{}",
        std::process::id(),
        TEMP_ID.fetch_add(1, Ordering::Relaxed)
    ))
}

struct TempFiles {
    paths: Vec<PathBuf>,
    keep: bool,
}

impl TempFiles {
    fn new(paths: impl IntoIterator<Item = PathBuf>) -> Self {
        Self {
            paths: paths.into_iter().collect(),
            keep: false,
        }
    }
}

impl Drop for TempFiles {
    fn drop(&mut self) {
        if !self.keep {
            for path in &self.paths {
                let _ = std::fs::remove_file(path);
            }
        }
    }
}
