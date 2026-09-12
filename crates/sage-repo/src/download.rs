//! Resilient, chunked, and mirror-aware file download engine.

use std::path::{Path, PathBuf};
use std::time::Duration;

use reqwest::header::{ACCEPT_RANGES, CONTENT_LENGTH, ETAG, IF_NONE_MATCH, RANGE};
use reqwest::{Client, Method, StatusCode};
use tokio::io::AsyncWriteExt;

use crate::config::join_url;
use crate::error::RepoError;
use crate::index::{decompress, hash_file, read_index_timestamp, temporary_path};
use crate::sign::verify_signature;

const CHUNKS: u64 = 4;
const MAX_REQUEST_ATTEMPTS: usize = 4;
const RETRY_BACKOFF_MS: u64 = 250;

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

    /// Conditionally refreshes, verifies, and protects against anti-replay for a channel's index.
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

        // Require signed freshness metadata even on first sync. Legacy local
        // indexes may be upgraded, but incoming indexes may never omit it.
        let incoming_ts = read_index_timestamp(&uncompressed)?.ok_or_else(|| {
            RepoError::InvalidConfig("incoming index has no publication timestamp".into())
        })?;
        if let Some(current_ts) = read_index_timestamp(destination)? {
            if incoming_ts == current_ts && hash_file(destination)? == hash_file(&uncompressed)? {
                // An authenticated no-op may still rotate the HTTP validator.
                if let Some(etag) = etag {
                    tokio::fs::write(etag_path, etag).await?;
                }
                return Ok(false);
            }
            if incoming_ts <= current_ts {
                return Err(RepoError::ReplayAttack {
                    current: current_ts,
                    incoming: incoming_ts,
                });
            }
        }

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
mod tests;
