//! PubGrub solver adapter, virtual proxy routing, and conflict translation.

use crate::error::SolverError;
use crate::universe::{DependencyMap, PackageUniverse, ProviderBindings, Solution, VersionRange};
use pubgrub::{
    DefaultStringReporter, Dependencies, DependencyProvider, Map, PackageResolutionStatistics,
    PubGrubError, Reporter, resolve,
};
use sage_core::{ConstraintOp, DEFAULT_SLOT, Dependency, PackageKey, Version};
use std::cmp::Reverse;
use std::collections::{BTreeMap, BTreeSet};
use std::convert::Infallible;

/// Resolver configuration including versions pinned by the installed system.
pub struct SageSolver<'a> {
    universe: &'a PackageUniverse,
    locked: BTreeMap<PackageKey, Version>,
    preferred_providers: ProviderBindings,
    bound_providers: ProviderBindings,
    bound_requirements: BTreeMap<Dependency, PackageKey>,
}

impl<'a> SageSolver<'a> {
    pub fn new(universe: &'a PackageUniverse) -> Self {
        Self {
            universe,
            locked: BTreeMap::new(),
            preferred_providers: BTreeMap::new(),
            bound_providers: BTreeMap::new(),
            bound_requirements: BTreeMap::new(),
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
            bound_providers: BTreeMap::new(),
            bound_requirements: BTreeMap::new(),
        }
    }

    /// Ranks providers within their channel without preventing PubGrub backtracking.
    pub fn prefer_providers(
        mut self,
        providers: impl IntoIterator<Item = (String, PackageKey)>,
    ) -> Self {
        self.preferred_providers = providers
            .into_iter()
            .map(|(symbol, key)| ((key.channel.clone(), symbol), key))
            .collect();
        self
    }

    /// Strictly binds each interface within the concrete provider's channel.
    /// Other channels retain independent candidates for the same symbol.
    pub fn bind_providers(
        mut self,
        providers: impl IntoIterator<Item = (String, PackageKey)>,
    ) -> Self {
        self.bound_providers = providers
            .into_iter()
            .map(|(symbol, key)| ((key.channel.clone(), symbol), key))
            .collect();
        self
    }

    /// Binds transaction-local choices to their exact virtual requirements.
    /// The dependency channel is the resolved provider channel; slot and version
    /// constraints remain distinct so one automatic choice cannot constrain other edges.
    pub fn bind_provider_requirements(
        mut self,
        bindings: impl IntoIterator<Item = (Dependency, PackageKey)>,
    ) -> Self {
        self.bound_requirements = bindings.into_iter().collect();
        self
    }

    /// Resolves exact root identities together, including an explicit slot `0`.
    /// Use root requirements when a virtual interface has no slot restriction.
    pub fn resolve(&self, requested: &[PackageKey]) -> Result<Solution, SolverError> {
        self.resolve_with_provider_choices(requested)
            .map(|(solution, _)| solution)
    }

    /// Returns actual virtual requirements and choices from the solved graph.
    /// Each requirement carries its resolved provider channel and original constraints.
    pub fn resolve_with_provider_choices(
        &self,
        requested: &[PackageKey],
    ) -> Result<(Solution, Vec<(Dependency, PackageKey)>), SolverError> {
        self.resolve_root(self.root_dependencies(requested))
    }

    fn root_dependencies(&self, requested: &[PackageKey]) -> DependencyMap {
        requested
            .iter()
            .map(|key| {
                let target = if key.name.starts_with("virtual/") || key.name.starts_with("so:") {
                    virtual_key(
                        &system_channel(&key.channel),
                        &Dependency {
                            name: key.name.clone(),
                            slot: Some(key.slot.clone()),
                            channel: None,
                            op: ConstraintOp::Any,
                            version: None,
                        },
                    )
                } else {
                    key.clone()
                };
                (target, VersionRange::full())
            })
            .collect()
    }

    /// Resolves desired roots and configured interfaces, returning bindings keyed
    /// by resolved channel and symbol so independent repositories cannot collide.
    pub fn resolve_with_provider_bindings(
        &self,
        requested: &[PackageKey],
    ) -> Result<(Solution, ProviderBindings), SolverError> {
        let mut dependencies = self.root_dependencies(requested);
        let (solution, choices) = loop {
            let (solution, choices) = self.resolve_root(dependencies.clone())?;
            let mut all_providers = self.preferred_providers.clone();
            all_providers.extend(self.bound_providers.clone());
            let missing: Vec<_> = all_providers
                .into_iter()
                .filter(|((channel, symbol), _)| {
                    !choices.iter().any(|(choice, _)| {
                        &choice.name == symbol && choice.channel.as_ref() == Some(channel)
                    })
                })
                .collect();
            if missing.is_empty() {
                break (solution, choices);
            }
            for ((_, symbol), preferred) in missing {
                let key = virtual_key(
                    &preferred.channel,
                    &Dependency {
                        name: symbol.clone(),
                        slot: None,
                        channel: None,
                        op: ConstraintOp::Any,
                        version: None,
                    },
                );
                if dependencies.insert(key, VersionRange::full()).is_some() {
                    return Err(SolverError::InvalidMetadata(format!(
                        "configured interface {symbol} has no virtual provider binding"
                    )));
                }
            }
        };
        let mut bindings = BTreeMap::new();
        for (requirement, key) in choices {
            let symbol = requirement.name;
            let scope = (key.channel.clone(), symbol.clone());
            if !self.preferred_providers.contains_key(&scope)
                && !self.bound_providers.contains_key(&scope)
            {
                continue;
            }
            if let Some(previous) = bindings.insert(scope, key.clone())
                && previous != key
            {
                return Err(SolverError::NoSolution(format!(
                    "configured interface {symbol} requires both {previous} and {key}"
                )));
            }
        }
        Ok((solution, bindings))
    }

    /// Resolves arbitrary root constraints in one pass.
    pub fn resolve_dependencies(
        &self,
        channel: &str,
        requested: &[Dependency],
    ) -> Result<Solution, SolverError> {
        self.resolve_with_root_requirements(&[], channel, requested)
            .map(|(solution, _)| solution)
    }

    /// Solves exact package roots together with optional-slot/version requirements
    /// interpreted in `channel`, returning the actual virtual choices. Keeping
    /// requirements separate from identities preserves `None` versus `Some("0")`
    /// and avoids merging an unqualified virtual root with an explicit slot `0`.
    pub fn resolve_with_root_requirements(
        &self,
        requested: &[PackageKey],
        channel: &str,
        requirements: &[Dependency],
    ) -> Result<(Solution, Vec<(Dependency, PackageKey)>), SolverError> {
        let parent = PackageKey::new(channel, "__build", DEFAULT_SLOT);
        let mut dependencies = self.root_dependencies(requested);
        for dependency in requirements {
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
        self.resolve_root(dependencies)
    }

    fn resolve_root(
        &self,
        dependencies: DependencyMap,
    ) -> Result<(Solution, Vec<(Dependency, PackageKey)>), SolverError> {
        let root = PackageKey::new("__sage", "root", DEFAULT_SLOT);
        let root_version = Version::new(0, "0", 0);
        self.resolve_root_with(&root, &root_version, dependencies)
    }

    fn resolve_root_with(
        &self,
        root: &PackageKey,
        root_version: &Version,
        dependencies: DependencyMap,
    ) -> Result<(Solution, Vec<(Dependency, PackageKey)>), SolverError> {
        let provider = SageProvider::build(self, root, root_version, dependencies)?;
        match resolve(&provider, root.clone(), root_version.clone()) {
            Ok(selected) => {
                let mut choices = Vec::new();
                for (key, version) in &selected {
                    let Some((_, channel, mut requirement)) = virtual_requirement(key) else {
                        continue;
                    };
                    if requirement.name.starts_with("virtual/provider/") {
                        continue;
                    }
                    let concrete = provider.releases[key][version]
                        .keys()
                        .find(|candidate| !is_proxy_key(candidate))
                        .expect("a virtual proxy depends on one concrete release");
                    requirement.channel = Some(channel);
                    choices.push((requirement, concrete.clone()));
                }
                Ok((
                    selected
                        .into_iter()
                        .filter(|(key, _)| key != root && key.channel != "__sage")
                        .collect(),
                    choices,
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

impl PackageUniverse {
    /// Finds installed dependency candidates using the resolver's channel, default
    /// slot, virtual routing, concrete fallback and version semantics.
    pub fn matching_dependency_keys(
        &self,
        parent: &PackageKey,
        dependency: &Dependency,
    ) -> Vec<PackageKey> {
        let target = dependency_key(self, parent, dependency);
        if let Some((_, channel, requirement)) = virtual_requirement(&target) {
            let symbol = provider_symbol(&requirement.name);
            self.providers_for(symbol)
                .iter()
                .filter(|key| {
                    key.channel == channel
                        && requirement
                            .slot
                            .as_ref()
                            .is_none_or(|slot| &key.slot == slot)
                        && self.versions(key).any(|version| {
                            requirement
                                .op
                                .matches(version, requirement.version.as_ref())
                                && self.release(key, version).is_some_and(|release| {
                                    release.provides.iter().any(|provided| provided == symbol)
                                })
                        })
                })
                .cloned()
                .collect()
        } else if self
            .versions(&target)
            .any(|version| dependency.op.matches(version, dependency.version.as_ref()))
        {
            vec![target]
        } else {
            Vec::new()
        }
    }
}

pub(crate) struct SageProvider {
    pub(crate) releases: BTreeMap<PackageKey, BTreeMap<Version, DependencyMap>>,
    pub(crate) locked: BTreeMap<PackageKey, Version>,
}

impl SageProvider {
    fn build(
        solver: &SageSolver<'_>,
        root: &PackageKey,
        root_version: &Version,
        root_dependencies: DependencyMap,
    ) -> Result<Self, SolverError> {
        let universe = solver.universe;
        let locked = &solver.locked;
        let preferred_providers = &solver.preferred_providers;
        let bound_providers = &solver.bound_providers;
        let mut releases = BTreeMap::new();
        let mut conflicts = Vec::new();
        for (key, versions) in &universe.releases {
            for (version, release) in versions {
                let mut dependencies = DependencyMap::default();
                for dependency in &release.dependencies {
                    let target = dependency_key(universe, key, dependency);
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
        let virtuals: BTreeSet<_> = releases
            .values()
            .flat_map(|versions| versions.values())
            .flat_map(|dependencies| dependencies.keys())
            .filter_map(virtual_requirement)
            .collect();
        for (target, channel, requirement) in virtuals {
            let provider_name = provider_symbol(&requirement.name);
            let scope = (channel.clone(), provider_name.to_owned());
            let bound = bound_providers.get(&scope);
            let preferred = preferred_providers.get(&scope);
            let mut scoped = requirement.clone();
            scoped.channel = Some(channel.clone());
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
                // When a virtual interface is strictly bound to a provider, only that
                // provider is eligible. All alternative providers are filtered out.
                if bound.is_some_and(|bound| bound != key)
                    || solver
                        .bound_requirements
                        .get(&scoped)
                        .is_some_and(|bound| bound != key)
                {
                    continue;
                }
                for (version_index, version) in universe.versions(key).enumerate() {
                    if !dependency_range(&requirement).contains(version)
                        || !universe.release(key, version).is_some_and(|release| {
                            release
                                .provides
                                .iter()
                                .any(|symbol| symbol == provider_name)
                        })
                    {
                        continue;
                    }
                    let preferred = preferred == Some(key) || bound == Some(key);
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
                    .filter(|key| conflict.slot.as_ref().is_none_or(|slot| &key.slot == slot))
                    .cloned()
                    .collect()
            } else {
                vec![dependency_key(universe, &owner, &conflict)]
            };
            for target in targets {
                if let Some(versions) = releases.get_mut(&target) {
                    for (version, dependencies) in versions {
                        if dependency_range(&conflict).contains(version)
                            && (!is_virtual(&conflict)
                                || universe.release(&target, version).is_some_and(|release| {
                                    release.provides.contains(&conflict.name)
                                }))
                        {
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
        if let Some(locked) = self.locked.get(package)
            && versions.contains_key(locked)
            && range.contains(locked)
        {
            return Ok(Some(locked.clone()));
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
    // Sonames are opaque, so appending a slot to their textual spelling would
    // change the symbol. Store the optional constraint in the synthetic key's
    // slot instead: empty means None, and a leading ':' identifies Some(slot).
    // The prefix also distinguishes a malformed empty slot from an absent one.
    let mut requirement = dependency.clone();
    let slot = requirement
        .slot
        .take()
        .map_or_else(String::new, |slot| format!(":{slot}"));
    PackageKey::new("__sage", format!("{channel}/{requirement}"), slot)
}

fn virtual_requirement(key: &PackageKey) -> Option<(PackageKey, String, Dependency)> {
    (key.channel == "__sage").then_some(())?;
    let boundary = key
        .name
        .find("/virtual/")
        .or_else(|| key.name.find("/so:"))?;
    let (channel, dependency) = key.name.split_at(boundary);
    let mut requirement: Dependency = dependency.strip_prefix('/')?.parse().ok()?;
    requirement.slot = key.slot.strip_prefix(':').map(str::to_owned);
    Some((key.clone(), channel.into(), requirement))
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
