mod repo_tests {
    use sage_repo::*;
    use heed::types::{Bytes, Str};
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
        for document in [
            r#"schema_version=1
[channels."../outside"]
url="https://example.invalid"
priority=1
signing_key="/etc/sage/key"
"#,
            r#"schema_version=1
[channels.main]
url="https://example.invalid"
priority=1
signing_key="/etc/sage/../outside"
"#,
            r#"schema_version=1
[channels.main]
url="https://example.invalid"
priority=1
signing_key="/etc/sage/key"
[channels.main.subchannels.system]
alias="../outside"
scope="system"
target_root="/"
"#,
            r#"schema_version=1
[channels.main]
url="https://example.invalid"
priority=1
signing_key="/etc/sage/key"
[channels.main.subchannels.system]
scope="system"
target_root="/../outside"
"#,
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
}
