//! Transactional LMDB state, ownership indexes, and crash journals.

pub mod env;
pub mod error;
pub mod models;

pub use env::{SageDatabase, read_owners, read_packages, read_system_providers};
pub use error::DbError;
pub use models::{
    FileMutation, InstalledPackage, JournalAction, JournalRecord, RebuildContinuation,
    ServiceProviderAction,
};

#[cfg(test)]
mod tests {
    use super::models::{JournalAction, legacy_record_digest};
    use super::*;
    use serde::Serialize;

    #[derive(Serialize)]
    struct LegacyJournalRecordForTest<'a> {
        op_id: &'a str,
        stage: &'a str,
        journal_sha256: &'a str,
        action: &'a JournalAction,
    }

    #[test]
    fn legacy_journal_layout_decodes_and_validates() {
        let action = JournalAction::Remove {
            packages: vec![],
            modified_paths: vec![],
            trigger_documents: vec![],
            alternative_documents: vec![],
        };
        let digest = legacy_record_digest("legacy-op", "packages", &action).unwrap();
        let bytes = bincode::serialize(&LegacyJournalRecordForTest {
            op_id: "legacy-op",
            stage: "packages",
            journal_sha256: &digest,
            action: &action,
        })
        .unwrap();

        let record = models::decode_journal(&bytes).unwrap();

        assert_eq!(record.op_id, "legacy-op");
        assert_eq!(record.declaration, None);
        record.validate().unwrap();
    }
}
