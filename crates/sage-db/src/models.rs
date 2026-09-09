//! Installed package states, journal transitions, and recovery records.

use crate::error::DbError;
use sage_core::{Dependency, PackageKey, Version, hex};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;

/// Complete installed state required for removal and reconciliation.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct InstalledPackage {
    pub key: PackageKey,
    pub version: Version,
    pub arch: String,
    pub installed_size: u64,
    pub dependencies: Vec<Dependency>,
    pub provides: Vec<String>,
    pub conflicts: Vec<String>,
    pub files: Vec<String>,
    /// Original package hashes keyed by exact physical ownership path.
    pub config_hashes: BTreeMap<String, String>,
}

/// The remaining rebuild work travels with package publication so any mutating
/// command can finish the original transition without reading a newer config.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct RebuildContinuation {
    pub provider_bindings: BTreeMap<String, PackageKey>,
    pub retired_packages: Vec<InstalledPackage>,
    /// True retirement paths, excluding files handed to a replacement owner.
    pub removed_paths: Vec<String>,
    pub removal_trigger_documents: Vec<Vec<u8>>,
    /// Serialized planned native-service generation, opaque to the database.
    pub rendered_services: Vec<u8>,
}

/// A declaration mutation applied only after the corresponding external state
/// transition reaches its durable stage.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct FileMutation {
    /// Sysroot-relative path, using `/` separators in the journal payload.
    pub path: String,
    pub previous: Option<Vec<u8>>,
    pub next: Option<Vec<u8>>,
}

/// Provider-side transition retained by a service lifecycle journal.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum ServiceProviderAction {
    /// Render and enable a service through the active init provider.
    Enable,
    /// Disable a service through the active init provider.
    Disable,
    /// Render an already enabled external service without changing activation.
    Adopt,
}

/// Recovery inputs; metadata stays opaque to avoid reverse crate dependencies.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum JournalAction {
    Install {
        architecture: String,
        changes: Vec<(PackageKey, Version)>,
        /// Pre-upgrade records retained until obsolete paths are removed.
        previous_packages: Vec<InstalledPackage>,
        modified_paths: Vec<String>,
        previous_alternative_documents: Vec<Vec<u8>>,
        rebuild: Option<RebuildContinuation>,
    },
    Remove {
        packages: Vec<InstalledPackage>,
        modified_paths: Vec<String>,
        trigger_documents: Vec<Vec<u8>>,
        alternative_documents: Vec<Vec<u8>>,
    },
    /// A service transition whose provider mutation precedes declaration
    /// publication. Opaque TOML snapshots make recovery independent of newer
    /// package metadata or provider selection.
    ServiceLifecycle {
        service: Vec<u8>,
        generator: Vec<u8>,
        provider_action: ServiceProviderAction,
        mutations: Vec<FileMutation>,
    },
}

/// Durable operation marker used for idempotent forward recovery.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct JournalRecord {
    pub op_id: String,
    pub stage: String,
    pub journal_sha256: String,
    pub action: JournalAction,
    /// Declaration bytes committed after package publication and recovery
    /// stages. Legacy records are upgraded by `decode_journal`.
    #[serde(default)]
    pub declaration: Option<FileMutation>,
}

/// On-disk layout written before declaration mutations joined the journal.
#[derive(Debug, Deserialize)]
pub(crate) struct LegacyJournalRecord {
    pub op_id: String,
    pub stage: String,
    pub journal_sha256: String,
    pub action: JournalAction,
}

impl From<LegacyJournalRecord> for JournalRecord {
    fn from(record: LegacyJournalRecord) -> Self {
        Self {
            op_id: record.op_id,
            stage: record.stage,
            journal_sha256: record.journal_sha256,
            action: record.action,
            declaration: None,
        }
    }
}

impl JournalRecord {
    /// Creates a sealed journal that startup can integrity-check.
    pub fn new(op_id: String, stage: &str, action: JournalAction) -> Self {
        let mut record = Self {
            op_id,
            stage: stage.into(),
            journal_sha256: String::new(),
            action,
            declaration: None,
        };
        record.seal();
        record
    }

    /// Attaches a declaration mutation and reseals the journal record.
    pub fn set_declaration(&mut self, declaration: Option<FileMutation>) {
        self.declaration = declaration;
        self.seal();
    }

    pub fn advance(&mut self, stage: &str) {
        self.stage = stage.into();
        self.seal();
    }

    pub fn validate(&self) -> Result<(), DbError> {
        let digest = record_digest(
            &self.op_id,
            &self.stage,
            &self.action,
            self.declaration.as_ref(),
        )?;
        let legacy_digest = self
            .declaration
            .is_none()
            .then(|| legacy_record_digest(&self.op_id, &self.stage, &self.action));
        if self.journal_sha256 == digest
            || legacy_digest
                .transpose()?
                .is_some_and(|digest| self.journal_sha256 == digest)
        {
            Ok(())
        } else {
            Err(DbError::InvalidJournal(self.op_id.clone()))
        }
    }

    fn seal(&mut self) {
        self.journal_sha256 = record_digest(
            &self.op_id,
            &self.stage,
            &self.action,
            self.declaration.as_ref(),
        )
        .expect("serializing an in-memory journal action cannot fail");
    }
}

pub fn record_digest(
    op_id: &str,
    stage: &str,
    action: &JournalAction,
    declaration: Option<&FileMutation>,
) -> Result<String, DbError> {
    Ok(hex::encode(Sha256::digest(bincode::serialize(&(
        op_id,
        stage,
        action,
        declaration,
    ))?)))
}

pub fn legacy_record_digest(
    op_id: &str,
    stage: &str,
    action: &JournalAction,
) -> Result<String, DbError> {
    Ok(hex::encode(Sha256::digest(bincode::serialize(&(
        op_id, stage, action,
    ))?)))
}

pub(crate) fn decode_journal(bytes: &[u8]) -> Result<JournalRecord, DbError> {
    match bincode::deserialize(bytes) {
        Ok(record) => Ok(record),
        Err(current_error) => bincode::deserialize::<LegacyJournalRecord>(bytes)
            .map(Into::into)
            .map_err(|_| DbError::Serialization(current_error)),
    }
}
