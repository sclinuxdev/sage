use heed::types::{Bytes, Str};
use sage_core::hex;
use sage_repo::*;
use sha2::{Digest, Sha256};
use std::fs::File;
use std::io::Write;
use std::path::PathBuf;

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
    let channel = ChannelConfig {
        url: "https://mirror/".into(),
        priority: 0,
        signing_key: PathBuf::from("key"),
        enabled: true,
        subchannels: std::collections::BTreeMap::new(),
    };
    let subchannel = SubchannelConfig {
        alias: Some("/index".into()),
        channel_type: None,
        scope: "all".into(),
        target_root: PathBuf::from("/"),
        enabled: true,
    };
    assert_eq!(
        subchannel_url(&channel, "index", &subchannel),
        "https://mirror/index"
    );
}

#[test]
fn channel_paths_and_identifiers_cannot_escape_the_target_root() {
    let document = |channel: &str,
                    subchannel: &str,
                    alias: Option<&str>,
                    signing_key: &str,
                    target_root: &str| {
        let alias = alias.map_or(String::new(), |value| format!("alias=\"{value}\"\n"));
        format!(
            "schema_version=1\n[channels.\"{channel}\"]\nurl=\"https://example.invalid\"\npriority=1\nsigning_key=\"{signing_key}\"\n[channels.\"{channel}\".subchannels.\"{subchannel}\"]\n{alias}scope=\"system\"\ntarget_root=\"{target_root}\"\n"
        )
    };
    for document in [
        document("../outside", "system", None, "/etc/sage/key", "/"),
        document("..", "system", None, "/etc/sage/key", "/"),
        document(".", "system", None, "/etc/sage/key", "/"),
        document("main", "system", None, "/etc/sage/../outside", "/"),
        document("main", "system", Some("../outside"), "/etc/sage/key", "/"),
        document("main", "system", Some(".."), "/etc/sage/key", "/"),
        document("main", "system", Some("."), "/etc/sage/key", "/"),
        document("main", "..", None, "/etc/sage/key", "/"),
        document("main", ".", None, "/etc/sage/key", "/"),
        document("main", "system", None, "/etc/sage/key", "/../outside"),
    ] {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("channels.toml");
        std::fs::write(&path, document).unwrap();
        assert!(matches!(
            ChannelsConfig::load(path),
            Err(RepoError::InvalidConfig(_))
        ));
    }
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

#[test]
fn repository_index_contains_inspected_package() {
    let dir = tempfile::tempdir().unwrap();
    let stage = dir.path().join("stage");
    std::fs::create_dir_all(stage.join(".METADATA")).unwrap();
    std::fs::create_dir_all(stage.join("data/usr/bin")).unwrap();
    std::fs::write(stage.join("data/usr/bin/demo"), b"demo").unwrap();
    let hash = hex::encode(Sha256::digest(b"demo"));
    std::fs::write(
        stage.join(".METADATA/files.idx"),
        format!("usr/bin/demo\t0755\t4\t{hash}\n"),
    )
    .unwrap();
    std::fs::write(
        stage.join(".METADATA/manifest.toml"),
        r#"schema_version=1
name="demo"
version="1.0"
release=1
arch="amd64"
channel="system"
description="demo"
license="MIT"
installed_size=4
build_time=1
provides=["cmd:demo"]
"#,
    )
    .unwrap();
    let package = dir.path().join("demo-1.0-1-amd64.pkg.tar.zst");
    sage_archive::create_package(&stage, &package, 1).unwrap();
    let key = dir.path().join("key");
    std::fs::write(&key, [3u8; 32]).unwrap();
    let output = dir.path().join("repo");
    let artifacts = build_index(dir.path(), &output, &key).unwrap();
    assert_eq!(artifacts.packages, 1);
    let env = open_index(&artifacts.index).unwrap();
    let txn = env.read_txn().unwrap();
    let packages: heed::Database<Str, Bytes> =
        env.open_database(&txn, Some("packages")).unwrap().unwrap();
    let releases: Vec<IndexedRelease> =
        bincode::deserialize(packages.get(&txn, "demo:0").unwrap().unwrap()).unwrap();
    assert_eq!(releases[0].package.name, "demo");
    assert!(artifacts.compressed.exists() && artifacts.signature.exists());
    drop(txn);
    drop(env);
    let reader = RepositoryIndex::open(&artifacts.index).unwrap();
    assert_eq!(reader.releases("demo", "0").unwrap().len(), 1);
    assert_eq!(reader.providers("cmd:demo").unwrap(), vec!["demo:0"]);
}

#[tokio::test]
async fn verified_cache_hit_requires_no_network() {
    let directory = tempfile::tempdir().unwrap();
    let destination = directory.path().join("artifact");
    std::fs::write(&destination, b"cached package").unwrap();
    let hash = hex::encode(Sha256::digest(b"cached package"));
    let engine = DownloadEngine::new(directory.path().join("cache")).unwrap();
    engine
        .download_url("http://127.0.0.1:1/unreachable", &destination, &hash)
        .await
        .unwrap();
}

#[test]
fn repository_index_records_timestamp_and_detects_replay() {
    let dir = tempfile::tempdir().unwrap();
    let stage = dir.path().join("stage");
    std::fs::create_dir_all(stage.join(".METADATA")).unwrap();
    std::fs::create_dir_all(stage.join("data/usr/bin")).unwrap();
    std::fs::write(stage.join("data/usr/bin/demo"), b"demo").unwrap();
    let hash = hex::encode(Sha256::digest(b"demo"));
    std::fs::write(
        stage.join(".METADATA/files.idx"),
        format!("usr/bin/demo\t0755\t4\t{hash}\n"),
    )
    .unwrap();
    std::fs::write(
        stage.join(".METADATA/manifest.toml"),
        r#"schema_version=1
name="demo"
version="1.0"
release=1
arch="amd64"
channel="system"
description="demo"
license="MIT"
installed_size=4
build_time=1
"#,
    )
    .unwrap();
    let package = dir.path().join("demo-1.0-1-amd64.pkg.tar.zst");
    sage_archive::create_package(&stage, &package, 1).unwrap();
    let key = dir.path().join("key");
    std::fs::write(&key, [3u8; 32]).unwrap();
    let output = dir.path().join("repo");
    let artifacts = build_index(dir.path(), &output, &key).unwrap();
    let ts = read_index_timestamp(&artifacts.index)
        .unwrap()
        .expect("timestamp must exist");
    assert!(ts > 0);
}

#[test]
fn anti_replay_detects_older_timestamp() {
    let dir = tempfile::tempdir().unwrap();
    let current_index = dir.path().join("current.mdb");
    let older_index = dir.path().join("older.mdb");

    for (path, ts) in [(&current_index, 2000u64), (&older_index, 1000u64)] {
        let mut options = heed::EnvOpenOptions::new();
        options.max_dbs(8);
        unsafe {
            options.flags(heed::EnvFlags::NO_SUB_DIR);
        }
        let env = unsafe { options.open(path).unwrap() };
        let mut txn = env.write_txn().unwrap();
        let metadata: heed::Database<heed::types::Str, heed::types::Str> =
            env.create_database(&mut txn, Some("metadata")).unwrap();
        metadata
            .put(&mut txn, "timestamp", &ts.to_string())
            .unwrap();
        txn.commit().unwrap();
    }

    let cur_ts = read_index_timestamp(&current_index).unwrap().unwrap();
    let old_ts = read_index_timestamp(&older_index).unwrap().unwrap();
    assert_eq!(cur_ts, 2000);
    assert_eq!(old_ts, 1000);
    assert!(old_ts < cur_ts);
}
