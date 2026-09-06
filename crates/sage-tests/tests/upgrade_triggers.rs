use sage_db::{JournalAction, SageDatabase};
use sage_tests::{PackageSpec, StateSnapshot, TortureLab};
use std::collections::BTreeMap;
use std::fs;
use std::path::Path;

const OLD_DATA: &str = "usr/lib/torture/cache-v1/data";
const OLD_RUNTIME: &str = "usr/lib/torture/cache-v1/runtime.sh";
const OLD_COMMAND: &str = "usr/bin/update-cache-v1";
const OLD_DECLARATION: &str = "usr/share/sage/triggers/cache-v1.toml";
const NEW_DATA: &str = "usr/lib/torture/cache-v2/data";
const NEW_RUNTIME: &str = "usr/lib/torture/cache-v2/runtime.sh";
const NEW_COMMAND: &str = "usr/bin/update-cache-v2";
const NEW_DECLARATION: &str = "usr/share/sage/triggers/cache-v2.toml";

/// Builds an ordinary package-owned vendor declaration, without lifecycle hooks.
fn trigger(name: &str, path: &str, command: &str, event: &str) -> Vec<u8> {
    format!(
        "schema_version=1\nname={name:?}\ndescription=\"Lifecycle regression\"\n\
         on_paths=[{path:?}]\nexec=[{command:?}]\npriority=1\n\
         events=[{event:?}]\nignore_missing_binary=false\n"
    )
    .into_bytes()
}

/// Checks the exact published payload and ownership without assuming fixture paths.
fn assert_payload(lab: &TortureLab, specs: &[&PackageSpec]) -> StateSnapshot {
    let snapshot = lab.snapshot().unwrap();
    let mut files = BTreeMap::new();
    let mut owners = BTreeMap::new();
    let mut packages = BTreeMap::new();
    for spec in specs {
        let key = format!("main/{}:{}:{}", spec.channel, spec.name, spec.slot);
        packages.insert(key.clone(), format!("{}-1", spec.version));
        for (path, content) in &spec.files {
            files.insert(path.clone(), content.clone());
            owners.insert(path.clone(), vec![key.clone()]);
        }
    }
    assert_eq!(snapshot.files, files);
    assert_eq!(snapshot.owners, owners);
    assert_eq!(snapshot.packages, packages);
    snapshot
}

/// Checks durable completion separately from payload state at an injected fault.
fn assert_journals(root: &Path, checkpoint: Option<&str>, installing: bool, changed: &str) {
    let database = SageDatabase::open(root.join("var/lib/sage")).unwrap();
    let pending = database.pending_journals().unwrap();
    let Some(checkpoint) = checkpoint else {
        assert!(pending.is_empty());
        return;
    };
    assert_eq!(pending.len(), 1);
    pending[0].validate().unwrap();
    assert_eq!(
        pending[0].stage,
        if checkpoint == "trigger-complete" {
            "complete"
        } else {
            "triggers"
        }
    );
    let paths = match &pending[0].action {
        JournalAction::Install { modified_paths, .. } if installing => modified_paths,
        JournalAction::Remove { modified_paths, .. } if !installing => modified_paths,
        action => panic!("unexpected recovery action: {action:?}"),
    };
    assert!(paths.iter().any(|path| path == changed));
}

#[tokio::test]
async fn upgrades_use_final_post_change_handlers_when_old_command_and_runtime_are_retired() {
    for checkpoint in [None, Some("triggers"), Some("trigger-complete")] {
        let mut lab = TortureLab::new().unwrap();
        let mut old = PackageSpec::new("system", "cache", 1, OLD_DATA, "old cache");
        old.files.insert(
            OLD_RUNTIME.into(),
            b"CACHE_RESULT=obsolete-removal\n".to_vec(),
        );
        old.files.insert(
            OLD_COMMAND.into(),
            format!(
                "#!/bin/sh\nset -eu\n. \"$SAGE_SYSROOT/{OLD_RUNTIME}\"\n\
                 printf '%s' \"$CACHE_RESULT\" >> \"$SAGE_SYSROOT/var/lib/sage/old-trigger-count\"\n"
            )
            .into_bytes(),
        );
        old.executable_files.insert(OLD_COMMAND.into());
        old.files.insert(
            OLD_DECLARATION.into(),
            trigger(
                "cache-v1",
                OLD_DATA,
                "/usr/bin/update-cache-v1",
                "post-remove",
            ),
        );
        lab.add_package(old.clone()).unwrap();
        lab.publish().unwrap();
        lab.install("cache", "system").await.unwrap();
        assert_payload(&lab, &[&old]);

        let mut new = PackageSpec::new("system", "cache", 2, NEW_DATA, "new cache");
        new.files
            .insert(NEW_RUNTIME.into(), b"CACHE_RESULT=post-change\n".to_vec());
        new.files.insert(
            NEW_COMMAND.into(),
            format!(
                "#!/bin/sh\nset -eu\n. \"$SAGE_SYSROOT/{NEW_RUNTIME}\"\n\
                 test -f \"$SAGE_SYSROOT/{NEW_DATA}\"\n\
                 test ! -e \"$SAGE_SYSROOT/{OLD_DATA}\"\n\
                 test ! -e \"$SAGE_SYSROOT/{OLD_COMMAND}\"\n\
                 test ! -e \"$SAGE_SYSROOT/{OLD_RUNTIME}\"\n\
                 printf '%s' \"$CACHE_RESULT\" >> \"$SAGE_SYSROOT/var/lib/sage/new-trigger-count\"\n"
            )
            .into_bytes(),
        );
        new.executable_files.insert(NEW_COMMAND.into());
        // Matching only an obsolete path proves upgrade recovery preserves removals
        // in its post-change input, even though v2 no longer owns that path.
        new.files.insert(
            NEW_DECLARATION.into(),
            trigger(
                "cache-v2",
                OLD_DATA,
                "/usr/bin/update-cache-v2",
                "post-change",
            ),
        );
        let unrelated = PackageSpec::new(
            "system",
            "unrelated",
            1,
            "usr/lib/torture/unrelated",
            "unrelated",
        );
        lab.add_package(new.clone()).unwrap();
        lab.add_package(unrelated.clone()).unwrap();
        lab.publish().unwrap();
        if let Some(checkpoint) = checkpoint {
            lab.inject(checkpoint).unwrap();
        }
        let result = lab.upgrade("cache", "system").await;
        if let Some(checkpoint) = checkpoint {
            assert!(
                format!("{:#}", result.unwrap_err())
                    .contains(&format!("injected crash after {checkpoint}"))
            );
        } else {
            result.unwrap();
        }
        assert_payload(&lab, &[&new]);
        for retired in [OLD_DATA, OLD_COMMAND, OLD_RUNTIME, OLD_DECLARATION] {
            assert!(!lab.root().join(retired).exists());
        }
        assert!(!lab.root().join("var/lib/sage/old-trigger-count").exists());
        let counter = lab.root().join("var/lib/sage/new-trigger-count");
        if checkpoint == Some("triggers") {
            assert!(!counter.exists());
        } else {
            assert_eq!(fs::read(&counter).unwrap(), b"post-change");
        }
        assert_journals(lab.root(), checkpoint, true, OLD_DATA);

        lab.install("unrelated", "system").await.unwrap();
        let recovered = assert_payload(&lab, &[&new, &unrelated]);
        assert_journals(lab.root(), None, true, OLD_DATA);
        for retired in [OLD_DATA, OLD_COMMAND, OLD_RUNTIME, OLD_DECLARATION] {
            assert!(!lab.root().join(retired).exists());
        }
        assert_eq!(fs::read(&counter).unwrap(), b"post-change");
        assert!(!lab.root().join("var/lib/sage/old-trigger-count").exists());
        lab.upgrade("cache", "system").await.unwrap();
        assert_eq!(assert_payload(&lab, &[&new, &unrelated]), recovered);
        assert_eq!(fs::read(&counter).unwrap(), b"post-change");
    }
}

#[tokio::test]
async fn ordinary_remove_replays_captured_post_remove_declaration_only_before_completion() {
    for checkpoint in [None, Some("triggers"), Some("trigger-complete")] {
        let mut lab = TortureLab::new().unwrap();
        let mut recorder = PackageSpec::new(
            "system",
            "recorder",
            1,
            "usr/lib/torture/recorder/runtime.sh",
            "REMOVE_RESULT=post-remove\n",
        );
        recorder.files.insert(
            "usr/bin/record-removal".into(),
            b"#!/bin/sh\nset -eu\n. \"$SAGE_SYSROOT/usr/lib/torture/recorder/runtime.sh\"\n\
              test ! -e \"$SAGE_SYSROOT/usr/lib/torture/remove-target\"\n\
              printf '%s' \"$REMOVE_RESULT\" >> \"$SAGE_SYSROOT/var/lib/sage/remove-trigger-count\"\n"
                .to_vec(),
        );
        recorder
            .executable_files
            .insert("usr/bin/record-removal".into());
        let removed_path = "usr/lib/torture/remove-target";
        let declaration = "usr/share/sage/triggers/remove-target.toml";
        let mut target = PackageSpec::new("system", "target", 1, removed_path, "remove me");
        target.files.insert(
            declaration.into(),
            trigger(
                "remove-target",
                removed_path,
                "/usr/bin/record-removal",
                "post-remove",
            ),
        );
        let unrelated = PackageSpec::new(
            "system",
            "unrelated",
            1,
            "usr/lib/torture/unrelated",
            "unrelated",
        );
        for spec in [&recorder, &target, &unrelated] {
            lab.add_package(spec.clone()).unwrap();
        }
        lab.publish().unwrap();
        lab.install("recorder", "system").await.unwrap();
        lab.install("target", "system").await.unwrap();
        assert_payload(&lab, &[&recorder, &target]);
        let counter = lab.root().join("var/lib/sage/remove-trigger-count");
        assert!(!counter.exists());
        if let Some(checkpoint) = checkpoint {
            lab.inject(checkpoint).unwrap();
        }
        let result = lab.remove("target", "system").await;
        if let Some(checkpoint) = checkpoint {
            assert!(
                format!("{:#}", result.unwrap_err())
                    .contains(&format!("injected crash after {checkpoint}"))
            );
        } else {
            result.unwrap();
        }
        assert_payload(&lab, &[&recorder]);
        assert!(!lab.root().join(removed_path).exists());
        assert!(!lab.root().join(declaration).exists());
        assert_journals(lab.root(), checkpoint, false, removed_path);
        if checkpoint == Some("triggers") {
            assert!(!counter.exists());
        } else {
            assert_eq!(fs::read(&counter).unwrap(), b"post-remove");
        }

        lab.install("unrelated", "system").await.unwrap();
        let recovered = assert_payload(&lab, &[&recorder, &unrelated]);
        assert_journals(lab.root(), None, false, removed_path);
        assert!(!lab.root().join(removed_path).exists());
        assert!(!lab.root().join(declaration).exists());
        assert_eq!(fs::read(&counter).unwrap(), b"post-remove");
        lab.install("unrelated", "system").await.unwrap();
        assert_eq!(assert_payload(&lab, &[&recorder, &unrelated]), recovered);
        assert_eq!(fs::read(&counter).unwrap(), b"post-remove");
    }
}
