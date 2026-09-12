//! Signed HTTP fixtures exercise freshness checks before atomic publication.

use super::*;
use ed25519_dalek::SigningKey;
use heed::types::Str;
use std::io::{BufRead, BufReader, Write};
use std::net::{TcpListener, TcpStream};
use std::time::{Duration, Instant};

fn index(path: &Path, timestamp: Option<&str>, content: &str) {
    let mut options = heed::EnvOpenOptions::new();
    options.max_dbs(8);
    // SAFETY: each fixture has one writer and is closed before signing/serving.
    let env = unsafe {
        options
            .flags(heed::EnvFlags::NO_SUB_DIR)
            .open(path)
            .unwrap()
    };
    let mut txn = env.write_txn().unwrap();
    let metadata: heed::Database<Str, Str> =
        env.create_database(&mut txn, Some("metadata")).unwrap();
    metadata.put(&mut txn, "content", content).unwrap();
    if let Some(timestamp) = timestamp {
        metadata.put(&mut txn, "timestamp", timestamp).unwrap();
    }
    txn.commit().unwrap();
}

fn accept_request(listener: &TcpListener) -> (TcpStream, Vec<String>) {
    let deadline = Instant::now() + Duration::from_secs(10);
    let stream = loop {
        match listener.accept() {
            Ok((stream, _)) => break stream,
            Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
                assert!(Instant::now() < deadline, "HTTP fixture timed out");
                std::thread::sleep(Duration::from_millis(5));
            }
            Err(error) => panic!("{error}"),
        }
    };
    stream
        .set_read_timeout(Some(Duration::from_secs(5)))
        .unwrap();
    let mut reader = BufReader::new(stream.try_clone().unwrap());
    let mut request = Vec::new();
    loop {
        let mut line = String::new();
        assert!(reader.read_line(&mut line).unwrap() > 0);
        if line == "\r\n" {
            break;
        }
        request.push(line.trim_end().to_owned());
    }
    (stream, request)
}

async fn serve_index(
    incoming: &Path,
    destination: &Path,
    corrupt_signature: bool,
) -> Result<bool, RepoError> {
    let key = SigningKey::from_bytes(&[29; 32]);
    let key_path = destination.with_extension("pub");
    std::fs::write(&key_path, key.verifying_key().to_bytes()).unwrap();
    let mut signature = crate::sign_file(incoming, &key).unwrap().to_bytes();
    if corrupt_signature {
        signature[0] ^= 1;
    }
    let compressed = zstd::encode_all(std::fs::read(incoming).unwrap().as_slice(), 1).unwrap();
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    listener.set_nonblocking(true).unwrap();
    let url = format!("http://{}", listener.local_addr().unwrap());
    let server = std::thread::spawn(move || {
        for body in [signature.to_vec(), compressed] {
            let (mut stream, _) = accept_request(&listener);
            write!(
                stream,
                "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nETag: new\r\nConnection: close\r\n\r\n",
                body.len()
            )
            .unwrap();
            stream.write_all(&body).unwrap();
        }
    });
    let result = DownloadEngine::new(destination.with_extension("cache"))
        .unwrap()
        .sync_index(&url, &key_path, destination)
        .await;
    server.join().unwrap();
    result
}

#[tokio::test]
async fn identical_refresh_updates_etag_and_next_sync_uses_304() {
    let dir = tempfile::tempdir().unwrap();
    let current = dir.path().join("current.mdb");
    index(&current, Some("20"), "current");
    let before = std::fs::read(&current).unwrap();
    let etag_path = current.with_extension("etag");
    std::fs::write(&etag_path, "\"old\"").unwrap();
    let key = SigningKey::from_bytes(&[29; 32]);
    let key_path = current.with_extension("pub");
    std::fs::write(&key_path, key.verifying_key().to_bytes()).unwrap();
    let signature = crate::sign_file(&current, &key)
        .unwrap()
        .to_bytes()
        .to_vec();
    let compressed = zstd::encode_all(before.as_slice(), 1).unwrap();
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    listener.set_nonblocking(true).unwrap();
    let url = format!("http://{}", listener.local_addr().unwrap());
    let server = std::thread::spawn(move || {
        for (path, validator, status, body) in [
            ("index.mdb.sig", Some("\"old\""), "200 OK", signature),
            ("index.mdb.zst", None, "200 OK", compressed),
            (
                "index.mdb.sig",
                Some("\"new\""),
                "304 Not Modified",
                Vec::new(),
            ),
        ] {
            let (mut stream, request) = accept_request(&listener);
            assert_eq!(request[0], format!("GET /{path} HTTP/1.1"));
            let actual = request.iter().find_map(|header| {
                let (name, value) = header.split_once(':')?;
                name.eq_ignore_ascii_case("if-none-match")
                    .then(|| value.trim())
            });
            assert_eq!(actual, validator);
            write!(stream, "HTTP/1.1 {status}\r\nContent-Length: {}\r\nETag: \"new\"\r\nConnection: close\r\n\r\n", body.len()).unwrap();
            stream.write_all(&body).unwrap();
        }
    });
    let engine = DownloadEngine::new(dir.path().join("cache")).unwrap();
    assert!(!engine.sync_index(&url, &key_path, &current).await.unwrap());
    assert_eq!(std::fs::read(&current).unwrap(), before);
    assert_eq!(std::fs::read_to_string(&etag_path).unwrap(), "\"new\"");
    assert!(!engine.sync_index(&url, &key_path, &current).await.unwrap());
    server.join().unwrap();
    assert_eq!(std::fs::read(&current).unwrap(), before);
}

#[tokio::test]
async fn signed_sync_rejects_missing_invalid_older_and_equal_changed_timestamps() {
    for timestamp in [None, Some("0"), Some("garbage"), Some("19"), Some("20")] {
        let dir = tempfile::tempdir().unwrap();
        let current = dir.path().join("current.mdb");
        let incoming = dir.path().join("incoming.mdb");
        index(&current, Some("20"), "current");
        index(&incoming, timestamp, "different signed contents");
        let before = std::fs::read(&current).unwrap();
        std::fs::write(current.with_extension("etag"), "old").unwrap();
        assert!(
            serve_index(&incoming, &current, false).await.is_err(),
            "{timestamp:?}"
        );
        assert_eq!(std::fs::read(&current).unwrap(), before);
        assert_eq!(
            std::fs::read_to_string(current.with_extension("etag")).unwrap(),
            "old"
        );
        assert!(!std::fs::read_dir(dir.path()).unwrap().any(|entry| {
            entry
                .unwrap()
                .file_name()
                .to_string_lossy()
                .contains("sage-tmp")
        }));
    }
}

#[tokio::test]
async fn signed_sync_accepts_identical_refresh_newer_index_and_legacy_upgrade() {
    for (current_ts, next_ts, identical) in [
        (Some("20"), "20", true),
        (Some("20"), "21", false),
        (None, "21", false),
    ] {
        let dir = tempfile::tempdir().unwrap();
        let current = dir.path().join("current.mdb");
        let incoming = dir.path().join("incoming.mdb");
        index(&current, current_ts, "current");
        if identical {
            std::fs::copy(&current, &incoming).unwrap();
        } else {
            index(&incoming, Some(next_ts), "next");
        }
        assert_eq!(
            serve_index(&incoming, &current, false).await.unwrap(),
            !identical
        );
        assert_eq!(
            std::fs::read(&current).unwrap(),
            std::fs::read(&incoming).unwrap()
        );
    }
}

#[tokio::test]
async fn first_sync_requires_timestamp_and_rejects_bad_signature() {
    for (timestamp, bad_signature) in [(None, false), (Some("21"), true)] {
        let dir = tempfile::tempdir().unwrap();
        let current = dir.path().join("current.mdb");
        let incoming = dir.path().join("incoming.mdb");
        index(&incoming, timestamp, "incoming");
        assert!(
            serve_index(&incoming, &current, bad_signature)
                .await
                .is_err()
        );
        assert!(!current.exists());
        assert!(!current.with_extension("etag").exists());
    }
}

#[test]
fn publication_timestamp_advances_after_clock_rollback() {
    let dir = tempfile::tempdir().unwrap();
    let pool = dir.path().join("pool");
    let output = dir.path().join("repo");
    std::fs::create_dir_all(&pool).unwrap();
    std::fs::create_dir_all(&output).unwrap();
    let future = u64::MAX - 2;
    index(
        &output.join("index.mdb"),
        Some(&future.to_string()),
        "previous",
    );
    let key = dir.path().join("key");
    std::fs::write(&key, [29; 32]).unwrap();
    let first = crate::build_index(&pool, &output, &key).unwrap();
    assert_eq!(
        read_index_timestamp(&first.index).unwrap(),
        Some(future + 1)
    );
    let second = crate::build_index(&pool, &output, &key).unwrap();
    assert_eq!(
        read_index_timestamp(&second.index).unwrap(),
        Some(future + 2)
    );
    assert!(crate::build_index(&pool, &output, &key).is_err());
}
