//! Slot-aware PubGrub dependency resolution with channel inheritance.

use pubgrub::{
    resolve, DefaultStringReporter, Dependencies, DependencyProvider, Map,
    PackageResolutionStatistics, PubGrubError, Ranges, Reporter,
};
use sage_core::{ConstraintOp, Dependency, PackageKey, Version, DEFAULT_SLOT};
use std::cmp::Reverse;
use std::collections::{BTreeMap, HashMap};
use std::convert::Infallible;
use thiserror::Error;

type VersionRange = Ranges<Version>;
type DependencyMap = Map<PackageKey, VersionRange>;
/// Selected version for each channel/name/slot identity.
pub type Solution = BTreeMap<PackageKey, Version>;

/// Failures returned with a human-readable PubGrub causality report.
#[derive(Debug, Error)]
pub enum SolverError {
    #[error("dependency resolution failed:\n{0}")]
    NoSolution(String),
    #[error("internal solver error: {0}")]
    Internal(String),
}

/// One repository release and the dependency edges published with it.
#[derive(Debug, Clone)]
pub struct PackageRelease {
    pub key: PackageKey,
    pub version: Version,
    pub dependencies: Vec<Dependency>,
    pub provides: Vec<String>,
}

/// Compact package universe assembled from mmap-backed repository point queries.
#[derive(Debug, Default)]
pub struct PackageUniverse {
    releases: BTreeMap<PackageKey, BTreeMap<Version, PackageRelease>>,
    providers: HashMap<String, Vec<PackageKey>>,
}

impl PackageUniverse {
    /// Inserts or replaces a release while maintaining the virtual-provider index.
    pub fn insert(&mut self, release: PackageRelease) {
        for symbol in &release.provides {
            let providers = self.providers.entry(symbol.clone()).or_default();
            if !providers.contains(&release.key) {
                providers.push(release.key.clone());
                providers.sort();
            }
        }
        self.releases
            .entry(release.key.clone())
            .or_default()
            .insert(release.version.clone(), release);
    }

    pub fn versions(&self, key: &PackageKey) -> impl DoubleEndedIterator<Item = &Version> {
        self.releases
            .get(key)
            .into_iter()
            .flat_map(|items| items.keys())
    }
}

/// Resolver configuration including versions pinned by the installed system.
pub struct SageSolver<'a> {
    universe: &'a PackageUniverse,
    locked: BTreeMap<PackageKey, Version>,
}

impl<'a> SageSolver<'a> {
    pub fn new(universe: &'a PackageUniverse) -> Self {
        Self {
            universe,
            locked: BTreeMap::new(),
        }
    }

    pub fn with_locked(
        universe: &'a PackageUniverse,
        locked: impl IntoIterator<Item = (PackageKey, Version)>,
    ) -> Self {
        Self {
            universe,
            locked: locked.into_iter().collect(),
        }
    }

    /// Resolves all requested roots together so shared dependencies cannot diverge.
    pub fn resolve(&self, requested: &[PackageKey]) -> Result<Solution, SolverError> {
        let dependencies = requested
            .iter()
            .cloned()
            .map(|key| (key, VersionRange::full()))
            .collect();
        self.resolve_root(dependencies)
    }

    /// Resolves arbitrary root constraints in one pass. Source builds use this
    /// for explicit and rclass-provided build dependencies without installing
    /// them into the host package database.
    pub fn resolve_dependencies(
        &self,
        channel: &str,
        requested: &[Dependency],
    ) -> Result<Solution, SolverError> {
        let root = PackageKey::new("__sage", "root", DEFAULT_SLOT);
        let root_version = Version::new(0, "0", 0);
        let parent = PackageKey::new(channel, "__build", DEFAULT_SLOT);
        let mut dependencies = DependencyMap::default();
        for dependency in requested {
            let key = dependency_key(self.universe, &self.locked, &parent, dependency);
            let range = dependency_range(dependency);
            dependencies
                .entry(key)
                .and_modify(|current| *current = current.intersection(&range))
                .or_insert(range);
        }
        self.resolve_root_with(&root, &root_version, dependencies)
    }

    fn resolve_root(&self, dependencies: DependencyMap) -> Result<Solution, SolverError> {
        let root = PackageKey::new("__sage", "root", DEFAULT_SLOT);
        let root_version = Version::new(0, "0", 0);
        self.resolve_root_with(&root, &root_version, dependencies)
    }

    fn resolve_root_with(
        &self,
        root: &PackageKey,
        root_version: &Version,
        dependencies: DependencyMap,
    ) -> Result<Solution, SolverError> {
        let provider = SageProvider::build(
            self.universe,
            &self.locked,
            root,
            root_version,
            dependencies,
        );
        match resolve(&provider, root.clone(), root_version.clone()) {
            Ok(selected) => Ok(selected
                .into_iter()
                .filter(|(key, _)| key != root)
                .collect()),
            Err(PubGrubError::NoSolution(mut tree)) => {
                tree.collapse_no_versions();
                Err(SolverError::NoSolution(DefaultStringReporter::report(
                    &tree,
                )))
            }
            Err(error) => Err(SolverError::Internal(format!("{error:?}"))),
        }
    }
}

struct SageProvider {
    releases: BTreeMap<PackageKey, BTreeMap<Version, DependencyMap>>,
    locked: BTreeMap<PackageKey, Version>,
}

impl SageProvider {
    fn build(
        universe: &PackageUniverse,
        locked: &BTreeMap<PackageKey, Version>,
        root: &PackageKey,
        root_version: &Version,
        root_dependencies: DependencyMap,
    ) -> Self {
        let mut releases = BTreeMap::new();
        for (key, versions) in &universe.releases {
            for release in versions.values() {
                let mut dependencies = DependencyMap::default();
                for dependency in &release.dependencies {
                    let target = dependency_key(universe, locked, key, dependency);
                    let range = dependency_range(dependency);
                    dependencies
                        .entry(target)
                        .and_modify(|current| *current = current.intersection(&range))
                        .or_insert(range);
                }
                releases
                    .entry(key.clone())
                    .or_insert_with(BTreeMap::new)
                    .insert(release.version.clone(), dependencies);
            }
        }
        releases
            .entry(root.clone())
            .or_insert_with(BTreeMap::new)
            .insert(root_version.clone(), root_dependencies);
        Self {
            releases,
            locked: locked.clone(),
        }
    }
}

impl DependencyProvider for SageProvider {
    type P = PackageKey;
    type V = Version;
    type VS = VersionRange;
    type Priority = (bool, u32, Reverse<usize>);
    type M = String;
    type Err = Infallible;

    fn prioritize(
        &self,
        package: &Self::P,
        range: &Self::VS,
        statistics: &PackageResolutionStatistics,
    ) -> Self::Priority {
        let count = self
            .releases
            .get(package)
            .map(|versions| {
                versions
                    .keys()
                    .filter(|version| range.contains(version))
                    .count()
            })
            .unwrap_or(0);
        (
            self.locked.contains_key(package),
            statistics.conflict_count(),
            Reverse(count),
        )
    }

    fn choose_version(
        &self,
        package: &Self::P,
        range: &Self::VS,
    ) -> Result<Option<Self::V>, Self::Err> {
        let Some(versions) = self.releases.get(package) else {
            return Ok(None);
        };
        if let Some(locked) = self.locked.get(package) {
            if versions.contains_key(locked) && range.contains(locked) {
                return Ok(Some(locked.clone()));
            }
        }
        Ok(versions
            .keys()
            .rev()
            .find(|version| range.contains(version))
            .cloned())
    }

    fn get_dependencies(
        &self,
        package: &Self::P,
        version: &Self::V,
    ) -> Result<Dependencies<Self::P, Self::VS, Self::M>, Self::Err> {
        Ok(
            match self
                .releases
                .get(package)
                .and_then(|versions| versions.get(version))
            {
                Some(dependencies) => Dependencies::Available(dependencies.clone()),
                None => Dependencies::Unavailable("repository metadata is unavailable".into()),
            },
        )
    }
}

fn dependency_key(
    universe: &PackageUniverse,
    locked: &BTreeMap<PackageKey, Version>,
    parent: &PackageKey,
    dependency: &Dependency,
) -> PackageKey {
    if dependency.name.starts_with("virtual/") || dependency.name.starts_with("so:") {
        if let Some(providers) = universe.providers.get(&dependency.name) {
            if let Some(provider) = providers.iter().find(|key| locked.contains_key(*key)) {
                return provider.clone();
            }
            if let Some(provider) = providers
                .iter()
                .find(|key| key.channel == system_channel(&parent.channel))
            {
                return provider.clone();
            }
            if let Some(provider) = providers.first() {
                return provider.clone();
            }
        }
        return PackageKey::new(
            system_channel(&parent.channel),
            &dependency.name,
            DEFAULT_SLOT,
        );
    }
    PackageKey::new(
        dependency.channel.as_deref().unwrap_or(&parent.channel),
        &dependency.name,
        dependency.slot.as_deref().unwrap_or(DEFAULT_SLOT),
    )
}

fn system_channel(channel: &str) -> String {
    channel
        .rsplit_once('/')
        .map_or_else(|| "system".into(), |(root, _)| format!("{root}/system"))
}

fn dependency_range(dependency: &Dependency) -> VersionRange {
    let Some(version) = dependency.version.clone() else {
        return VersionRange::full();
    };
    match dependency.op {
        ConstraintOp::Any => VersionRange::full(),
        ConstraintOp::Equal => VersionRange::singleton(version),
        ConstraintOp::NotEqual => VersionRange::singleton(version).complement(),
        ConstraintOp::Greater => VersionRange::strictly_higher_than(version),
        ConstraintOp::GreaterOrEqual => VersionRange::higher_than(version),
        ConstraintOp::Less => VersionRange::strictly_lower_than(version),
        ConstraintOp::LessOrEqual => VersionRange::lower_than(version),
    }
}
