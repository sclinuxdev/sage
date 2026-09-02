//! Slot-aware PubGrub dependency resolution with channel inheritance.
use pubgrub::{
    resolve, DefaultStringReporter, Dependencies, DependencyProvider, Map,
    PackageResolutionStatistics, PubGrubError, Ranges, Reporter,
};
use sage_core::{ConstraintOp, Dependency, Package, PackageKey, Version, DEFAULT_SLOT};
use std::cmp::Reverse;
use std::collections::{BTreeMap, BTreeSet, HashMap};
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
    #[error("invalid package metadata: {0}")]
    InvalidMetadata(String),
    #[error("internal solver error: {0}")]
    Internal(String),
}
/// Solver releases use the canonical package record without a conversion layer.
pub type PackageRelease = Package;
/// Compact package universe assembled from mmap-backed repository point queries.
#[derive(Debug, Default)]
pub struct PackageUniverse {
    releases: BTreeMap<PackageKey, BTreeMap<Version, PackageRelease>>,
    providers: HashMap<String, Vec<PackageKey>>,
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

    /// Returns one exact release selected by the solver.
    pub fn release(&self, key: &PackageKey, version: &Version) -> Option<&PackageRelease> {
        self.releases.get(key)?.get(version)
    }
}
/// Resolver configuration including versions pinned by the installed system.
pub struct SageSolver<'a> {
    universe: &'a PackageUniverse,
    locked: BTreeMap<PackageKey, Version>,
    preferred_providers: BTreeMap<String, PackageKey>,
}
impl<'a> SageSolver<'a> {
    pub fn new(universe: &'a PackageUniverse) -> Self {
        Self {
            universe,
            locked: BTreeMap::new(),
            preferred_providers: BTreeMap::new(),
        }
    }
    pub fn with_locked(
        universe: &'a PackageUniverse,
        locked: impl IntoIterator<Item = (PackageKey, Version)>,
    ) -> Self {
        Self {
            universe,
            locked: locked.into_iter().collect(),
            preferred_providers: BTreeMap::new(),
        }
    }
    /// Ranks configured providers without preventing PubGrub backtracking.
    pub fn prefer_providers(
        mut self,
        providers: impl IntoIterator<Item = (String, PackageKey)>,
    ) -> Self {
        self.preferred_providers = providers.into_iter().collect();
        self
    }
    /// Resolves all requested roots together so shared dependencies cannot diverge.
    pub fn resolve(&self, requested: &[PackageKey]) -> Result<Solution, SolverError> {
        let dependencies = requested
            .iter()
            .cloned()
            .map(|key| (key, VersionRange::full()))
            .collect();
        self.resolve_root(dependencies)
            .map(|(solution, _)| solution)
    }

    /// Resolves roots and returns the concrete package selected by each virtual proxy.
    ///
    /// Provider bindings are recovered from PubGrub's selected private proxy
    /// releases before those implementation-only packages are removed from the
    /// public solution. A symbol that resolves to different concrete packages
    /// cannot be represented by the single system-provider binding and is
    /// rejected instead of choosing one by map order.
    pub fn resolve_with_provider_bindings(
        &self,
        requested: &[PackageKey],
    ) -> Result<(Solution, BTreeMap<String, PackageKey>), SolverError> {
        let dependencies = requested
            .iter()
            .cloned()
            .map(|key| (key, VersionRange::full()))
            .collect();
        let (solution, choices) = self.resolve_root(dependencies)?;
        let mut bindings = BTreeMap::new();
        for (symbol, key) in choices {
            if let Some(previous) = bindings.insert(symbol.clone(), key.clone()) {
                if previous != key {
                    return Err(SolverError::Internal(format!(
                        "virtual symbol {symbol} selected both {previous} and {key}"
                    )));
                }
            }
        }
        Ok((solution, bindings))
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
            let key = dependency_key(self.universe, &parent, dependency);
            let range = if is_proxy_key(&key) {
                VersionRange::full()
            } else {
                dependency_range(dependency)
            };
            dependencies
                .entry(key)
                .and_modify(|current| *current = current.intersection(&range))
                .or_insert(range);
        }
        self.resolve_root_with(&root, &root_version, dependencies)
            .map(|(solution, _)| solution)
    }
    fn resolve_root(
        &self,
        dependencies: DependencyMap,
    ) -> Result<(Solution, Vec<(String, PackageKey)>), SolverError> {
        let root = PackageKey::new("__sage", "root", DEFAULT_SLOT);
        let root_version = Version::new(0, "0", 0);
        self.resolve_root_with(&root, &root_version, dependencies)
    }
    fn resolve_root_with(
        &self,
        root: &PackageKey,
        root_version: &Version,
        dependencies: DependencyMap,
    ) -> Result<(Solution, Vec<(String, PackageKey)>), SolverError> {
        let provider = SageProvider::build(
            self.universe,
            &self.locked,
            &self.preferred_providers,
            root,
            root_version,
            dependencies,
        )?;
        match resolve(&provider, root.clone(), root_version.clone()) {
            Ok(selected) => {
                let mut provider_choices = Vec::new();
                for (key, version) in &selected {
                    let Some((_, _, requirement)) = virtual_requirement(key) else {
                        continue;
                    };
                    let dependencies = provider
                        .releases
                        .get(key)
                        .and_then(|versions| versions.get(version))
                        .ok_or_else(|| {
                            SolverError::Internal(format!(
                                "selected virtual proxy {key} {version} has no release"
                            ))
                        })?;
                    let concrete = dependencies
                        .keys()
                        .filter(|candidate| candidate.channel != "__sage")
                        .cloned()
                        .collect::<Vec<_>>();
                    let [concrete] = concrete.as_slice() else {
                        return Err(SolverError::Internal(format!(
                            "selected virtual proxy {key} {version} has {} concrete providers",
                            concrete.len()
                        )));
                    };
                    provider_choices
                        .push((provider_symbol(&requirement.name).into(), concrete.clone()));
                }
                Ok((
                    selected
                        .into_iter()
                        .filter(|(key, _)| key != root && key.channel != "__sage")
                        .collect(),
                    provider_choices,
                ))
            }
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
        preferred_providers: &BTreeMap<String, PackageKey>,
        root: &PackageKey,
        root_version: &Version,
        root_dependencies: DependencyMap,
    ) -> Result<Self, SolverError> {
        let mut releases = BTreeMap::new();
        let mut conflicts = Vec::new();
        for (key, versions) in &universe.releases {
            for (version, release) in versions {
                let mut dependencies = DependencyMap::default();
                for dependency in &release.dependencies {
                    let target = dependency_key(universe, key, dependency);
                    // The private proxy key encodes this exact requirement;
                    // candidates are filtered below using the real provider version.
                    let range = if is_proxy_key(&target) {
                        VersionRange::full()
                    } else {
                        dependency_range(dependency)
                    };
                    dependencies
                        .entry(target)
                        .and_modify(|current| *current = current.intersection(&range))
                        .or_insert(range);
                }
                releases
                    .entry(key.clone())
                    .or_insert_with(BTreeMap::new)
                    .insert(version.clone(), dependencies);
                for declaration in &release.conflicts {
                    let conflict = declaration.parse::<Dependency>().map_err(|error| {
                        SolverError::InvalidMetadata(format!(
                            "invalid conflict '{declaration}' in {key} {version}: {error}"
                        ))
                    })?;
                    conflicts.push((key.clone(), version.clone(), conflict));
                }
            }
        }
        releases
            .entry(root.clone())
            .or_insert_with(BTreeMap::new)
            .insert(root_version.clone(), root_dependencies);
        // Proxy releases turn a virtual dependency into a PubGrub choice among
        // exact concrete releases. Each constrained virtual requirement gets a
        // private key, so its candidate set can be filtered before resolution.
        let virtuals: BTreeSet<_> = releases
            .values()
            .flat_map(|versions| versions.values())
            .flat_map(|dependencies| dependencies.keys())
            .filter_map(virtual_requirement)
            .collect();
        for (target, channel, requirement) in virtuals {
            let provider_name = provider_symbol(&requirement.name);
            let Some(providers) = universe.providers.get(provider_name) else {
                continue;
            };
            for (provider_index, key) in providers.iter().enumerate() {
                if key.channel != channel {
                    continue;
                }
                if requirement
                    .slot
                    .as_deref()
                    .is_some_and(|slot| key.slot != slot)
                {
                    continue;
                }
                for (version_index, version) in universe.versions(key).enumerate() {
                    if !dependency_range(&requirement).contains(version) {
                        continue;
                    }
                    let preferred = preferred_providers.get(provider_name) == Some(key);
                    let exact_lock = locked.get(key) == Some(version);
                    let preference = match (preferred, exact_lock) {
                        (true, true) => 4,
                        (true, false) => 3,
                        (false, true) => 2,
                        (false, false) => 1,
                    };
                    let proxy_version =
                        Version::new(preference, format!("{provider_index}.{version_index}"), 0);
                    releases
                        .entry(target.clone())
                        .or_insert_with(BTreeMap::new)
                        .insert(
                            proxy_version,
                            Map::from_iter([(
                                key.clone(),
                                VersionRange::singleton(version.clone()),
                            )]),
                        );
                }
            }
        }
        // Opposite private-marker versions put conflicts in PubGrub's normal
        // backtracking and incompatibility report.
        for (index, (owner, owner_version, conflict)) in conflicts.into_iter().enumerate() {
            let marker = PackageKey::new("__sage", format!("conflict/{index}"), DEFAULT_SLOT);
            let zero = Version::new(0, "0", 0);
            let one = Version::new(0, "1", 0);
            releases.entry(marker.clone()).or_default().extend([
                (zero.clone(), DependencyMap::default()),
                (one.clone(), DependencyMap::default()),
            ]);
            releases
                .get_mut(&owner)
                .and_then(|versions| versions.get_mut(&owner_version))
                .expect("conflict owner was inserted above")
                .insert(marker.clone(), VersionRange::singleton(one));
            let targets: Vec<_> = if is_virtual(&conflict) {
                universe
                    .providers
                    .get(&conflict.name)
                    .into_iter()
                    .flatten()
                    .filter(|key| key.channel == system_channel(&owner.channel))
                    .cloned()
                    .collect()
            } else {
                vec![dependency_key(universe, &owner, &conflict)]
            };
            for target in targets {
                if let Some(versions) = releases.get_mut(&target) {
                    for (version, dependencies) in versions {
                        if dependency_range(&conflict).contains(version) {
                            dependencies
                                .insert(marker.clone(), VersionRange::singleton(zero.clone()));
                        }
                    }
                }
            }
        }
        Ok(Self {
            releases,
            locked: locked.clone(),
        })
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
    parent: &PackageKey,
    dependency: &Dependency,
) -> PackageKey {
    if is_virtual(dependency) {
        return virtual_key(&system_channel(&parent.channel), dependency);
    }
    let concrete = PackageKey::new(
        dependency_channel(parent, dependency.channel.as_deref()),
        &dependency.name,
        dependency.slot.as_deref().unwrap_or(DEFAULT_SLOT),
    );
    if universe.versions(&concrete).next().is_none()
        && universe
            .providers
            .get(&dependency.name)
            .into_iter()
            .flatten()
            .any(|provider| provider.channel == concrete.channel)
    {
        return virtual_key(
            &concrete.channel,
            &Dependency {
                name: format!("virtual/provider/{}", dependency.name),
                slot: dependency.slot.clone(),
                channel: None,
                op: dependency.op,
                version: dependency.version.clone(),
            },
        );
    }
    concrete
}

/// Resolves a dependency channel alias in the root repository of its parent.
///
/// Recipe metadata intentionally uses short channel names such as `system` or
/// `gcc16`. Once a repository index is loaded, those names become
/// `main/system` and `main/gcc16`. The dependency parser passes the channel
/// portion of `gcc16/gcc` as `gcc16`, so both short and `subchannel/name`
/// declarations must inherit the repository root from the parent package.
fn dependency_channel(parent: &PackageKey, requested: Option<&str>) -> String {
    let Some(requested) = requested else {
        return parent.channel.clone();
    };
    parent.channel.rsplit_once('/').map_or_else(
        || requested.into(),
        |(root, _)| {
            if requested == root || requested.starts_with(&format!("{root}/")) {
                requested.into()
            } else {
                format!("{root}/{requested}")
            }
        },
    )
}
fn is_virtual(dependency: &Dependency) -> bool {
    dependency.name.starts_with("virtual/") || dependency.name.starts_with("so:")
}
fn is_proxy_key(key: &PackageKey) -> bool {
    key.channel == "__sage"
}
fn provider_symbol(name: &str) -> &str {
    name.strip_prefix("virtual/provider/").unwrap_or(name)
}
fn virtual_key(channel: &str, dependency: &Dependency) -> PackageKey {
    PackageKey::new("__sage", format!("{channel}/{dependency}"), DEFAULT_SLOT)
}
fn virtual_requirement(key: &PackageKey) -> Option<(PackageKey, String, Dependency)> {
    (key.channel == "__sage").then_some(())?;
    let boundary = key
        .name
        .find("/virtual/")
        .or_else(|| key.name.find("/so:"))?;
    let (channel, dependency) = key.name.split_at(boundary);
    Some((
        key.clone(),
        channel.into(),
        dependency.strip_prefix('/')?.parse().ok()?,
    ))
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
