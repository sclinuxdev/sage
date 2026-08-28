//! Conditional repository synchronization, signature verification, and range downloads.

use ed25519_dalek::{Signature, Verifier, VerifyingKey};
use heed::{Env, EnvFlags, EnvOpenOptions};
use reqwest::header::{ACCEPT_RANGES, CONTENT_LENGTH, ETAG, IF_NONE_MATCH, RANGE};
use reqwest::{Client, StatusCode};
use serde::Deserialize;
use sha2::{Digest, Sha256};
use std::fs::File;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use thiserror::Error;
use tokio::io::AsyncWriteExt;

const CHUNKS: u64 = 4;
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

fn enabled() -> bool {
    true
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

    async fn download_one(
        &self,
        url: &str,
        destination: &Path,
        sha256: &str,
    ) -> Result<(), RepoError> {
        if let Some(parent) = destination.parent() {
            tokio::fs::create_dir_all(parent).await?;
        }
        // HEAD is an optimization only; mirrors that reject it still work through GET.
        let head = self
            .client
            .head(url)
            .send()
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
            stream_response(
                self.client.get(url).send().await?.error_for_status()?,
                &temporary,
            )
            .await?;
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
                let response = client
                    .get(url)
                    .header(RANGE, format!("bytes={start}-{end}"))
                    .send()
                    .await?;
                if response.status() != StatusCode::PARTIAL_CONTENT {
                    return Err(RepoError::InvalidConfig(
                        "server ignored a range request".into(),
                    ));
                }
                stream_response(response, &part).await?;
                Ok::<_, RepoError>(())
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

async fn stream_response(mut response: reqwest::Response, path: &Path) -> Result<(), RepoError> {
    let mut output = tokio::fs::File::create(path).await?;
    while let Some(chunk) = response.chunk().await? {
        output.write_all(&chunk).await?;
    }
    output.sync_all().await?;
    Ok(())
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

fn decompress(source: &Path, destination: &Path) -> Result<(), RepoError> {
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

fn decode_fixed<const N: usize>(bytes: &[u8]) -> Result<[u8; N], RepoError> {
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
    // SAFETY: repository indexes are immutable after atomic publication. NO_LOCK
    // is therefore safe, and NO_SUB_DIR matches the protocol's single data file.
    unsafe {
        options.flags(EnvFlags::READ_ONLY | EnvFlags::NO_LOCK | EnvFlags::NO_SUB_DIR);
    }
    Ok(unsafe { options.open(path)? })
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fixed_values_accept_raw_and_hex() {
        let raw = [7u8; 32];
        assert_eq!(decode_fixed::<32>(&raw).unwrap(), raw);
        assert_eq!(
            decode_fixed::<32>(hex::encode(raw).as_bytes()).unwrap(),
            raw
        );
    }

    #[test]
    fn urls_have_one_separator() {
        assert_eq!(
            join_url("https://mirror/", "/index"),
            "https://mirror/index"
        );
    }

    #[test]
    fn decompression_is_streaming_and_exact() {
        let dir = tempfile::tempdir().unwrap();
        let compressed = dir.path().join("data.zst");
        let mut encoder = zstd::Encoder::new(File::create(&compressed).unwrap(), 1).unwrap();
        encoder.write_all(b"index").unwrap();
        encoder.finish().unwrap();
        let output = dir.path().join("data");
        decompress(&compressed, &output).unwrap();
        assert_eq!(std::fs::read(output).unwrap(), b"index");
    }
}
