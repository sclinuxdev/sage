mod db_tests {
    use sage_core::{PackageKey, Version};
    use sage_db::*;
    use std::collections::BTreeMap;

    fn package(name: &str, file: &str) -> InstalledPackage {
        InstalledPackage {
            key: PackageKey::new("main/system", name, "0"),
            version: Version::new(0, "1.0", 1),
            arch: "amd64".into(),
            installed_size: 10,
            dependencies: vec![],
            provides: vec![format!("cmd:{name}")],
            conflicts: vec![],
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
        let record = JournalRecord::new(
            "op-1".into(),
            "packages",
            JournalAction::Install {
                architecture: "amd64".into(),
                changes: vec![],
                previous_packages: vec![],
                modified_paths: vec![],
                previous_alternative_documents: vec![],
            },
        );
        SageDatabase::open(dir.path())
            .unwrap()
            .write_journal(&record)
            .unwrap();
        let reopened = SageDatabase::open(dir.path()).unwrap();
        assert_eq!(reopened.pending_journals().unwrap(), vec![record]);
        assert!(reopened.finish_journal("op-1").unwrap());
    }

    #[test]
    fn journal_integrity_covers_identity_and_recovery_stage() {
        let action = JournalAction::Install {
            architecture: "amd64".into(),
            changes: vec![],
            previous_packages: vec![],
            modified_paths: vec![],
            previous_alternative_documents: vec![],
        };
        let mut record = JournalRecord::new("op-1".into(), "packages", action.clone());
        record.stage = "triggers".into();
        assert!(matches!(record.validate(), Err(DbError::InvalidJournal(_))));
        let mut record = JournalRecord::new("op-1".into(), "packages", action);
        record.op_id = "op-2".into();
        assert!(matches!(record.validate(), Err(DbError::InvalidJournal(_))));
    }

}
