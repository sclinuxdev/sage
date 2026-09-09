//! Package universe representation and multi-channel version index.

use pubgrub::{Map, Ranges};
use sage_core::{Package, PackageKey, Version};
use std::collections::{BTreeMap, HashMap};

pub type VersionRange = Ranges<Version>;
pub type DependencyMap = Map<PackageKey, VersionRange>;

/// Selected version for each channel/name/slot identity.
pub type Solution = BTreeMap<PackageKey, Version>;

/// Solver releases use the canonical package record without a conversion layer.
pub type PackageRelease = Package;

/// Compact package universe assembled from mmap-backed repository point queries.
#[derive(Debug, Default, Clone)]
pub struct PackageUniverse {
    pub(crate) releases: BTreeMap<PackageKey, BTreeMap<Version, PackageRelease>>,
    pub(crate) providers: HashMap<String, Vec<PackageKey>>,
}

impl PackageUniverse {
    /// Inserts or replaces a release while maintaining the virtual-provider index.
    pub fn insert(&mut self, release: PackageRelease) {
        let coordinate = release.coordinate();
        for symbol in &release.provides {
            let providers = self.providers.entry(symbol.clone()).or_default();
            if !providers.contains(&coordinate.key) {
                providers.push(coordinate.key.clone());
                providers.sort();
            }
        }
        self.releases
            .entry(coordinate.key)
            .or_default()
            .insert(coordinate.version, release);
    }

    pub fn versions(&self, key: &PackageKey) -> impl DoubleEndedIterator<Item = &Version> {
        self.releases
            .get(key)
            .into_iter()
            .flat_map(|items| items.keys())
    }

    /// Returns metadata for one exact channel/name/slot/version candidate.
    pub fn release(&self, key: &PackageKey, version: &Version) -> Option<&PackageRelease> {
        self.releases.get(key)?.get(version)
    }
}
