//! Append-only string interner with stable identifiers and O(1) lookup.

use crate::error::CoreError;
use std::collections::HashMap;

/// Compact identifier used instead of strings on hot paths.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct SymbolId(u32);

impl SymbolId {
    pub fn get(self) -> u32 {
        self.0
    }
}

/// Append-only string interner with stable identifiers and O(1) lookup.
#[derive(Debug, Default)]
pub struct SymbolTable {
    ids: HashMap<String, SymbolId>,
    values: Vec<String>,
}

impl SymbolTable {
    pub fn intern(&mut self, value: &str) -> Result<SymbolId, CoreError> {
        if let Some(id) = self.ids.get(value) {
            return Ok(*id);
        }
        let id = SymbolId(
            self.values
                .len()
                .try_into()
                .map_err(|_| CoreError::SymbolTableFull)?,
        );
        let owned = value.to_owned();
        self.values.push(owned.clone());
        self.ids.insert(owned, id);
        Ok(id)
    }

    pub fn resolve(&self, id: SymbolId) -> Option<&str> {
        self.values.get(id.0 as usize).map(String::as_str)
    }

    pub fn len(&self) -> usize {
        self.values.len()
    }

    pub fn is_empty(&self) -> bool {
        self.values.is_empty()
    }
}
