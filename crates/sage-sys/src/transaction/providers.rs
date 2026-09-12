//! Virtual selection based on feasible solver results and exact provider identities.

use std::collections::{BTreeMap, BTreeSet};
use std::io::IsTerminal;

use anyhow::{Result, bail};
use sage_core::{ConstraintOp, Dependency, PackageKey, Version};
use sage_solver::{PackageUniverse, ProviderBindings, SageSolver, Solution};

use crate::state::{SystemConfig, provider_symbol};

/// Prompts the user or selects a provider when multiple providers exist for an unconfigured virtual interface.
pub fn select_virtual_provider(
    symbol: &str,
    candidates: &[sage_core::PackageKey],
    cli_override: Option<&str>,
    interactive: bool,
) -> Result<sage_core::PackageKey> {
    if candidates.is_empty() {
        bail!("no provider available in repository for {symbol}");
    }

    // 1. If explicit CLI override is provided, select it if valid
    if let Some(override_pkg) = cli_override {
        if let Some(matched) = candidates.iter().find(|key| {
            sage_core::PackageKey::in_channel(&key.channel, override_pkg)
                .ok()
                .as_ref()
                == Some(*key)
        }) {
            return Ok(matched.clone());
        } else {
            bail!(
                "specified provider '{override_pkg}' does not satisfy {symbol} (available: {})",
                candidates
                    .iter()
                    .map(ToString::to_string)
                    .collect::<Vec<_>>()
                    .join(", ")
            );
        }
    }

    if candidates.len() == 1 {
        return Ok(candidates[0].clone());
    }

    // 2. If interactive terminal is attached and requested, prompt the user to choose
    if interactive && std::io::stdin().is_terminal() && std::io::stdout().is_terminal() {
        use std::io::{self, BufRead, Write};

        println!("\nThere are multiple providers available for {symbol}:");
        for (i, key) in candidates.iter().enumerate() {
            println!("  {}) {key}", i + 1);
        }
        print!("Select a provider [1-{}] (default 1): ", candidates.len());
        io::stdout().flush()?;

        let mut line = String::new();
        let stdin = io::stdin();
        if stdin.lock().read_line(&mut line)? == 0 {
            bail!("provider selection cancelled for {symbol}");
        }
        let trimmed = line.trim();
        if trimmed.is_empty() {
            return Ok(candidates[0].clone());
        }
        let index = trimmed
            .parse::<usize>()
            .ok()
            .filter(|&index| index >= 1 && index <= candidates.len())
            .ok_or_else(|| anyhow::anyhow!("invalid provider selection for {symbol}"))?;
        return Ok(candidates[index - 1].clone());
    } else {
        println!(
            "Notice: multiple providers available for {symbol}; selecting default provider '{}'",
            candidates[0].name
        );
    }

    Ok(candidates[0].clone())
}

/// Solved install state and declarations selected from its actual virtual edges.
pub(super) struct VirtualSelections {
    pub solution: Solution,
    pub providers: BTreeMap<String, PackageKey>,
    pub declarations: BTreeMap<String, String>,
    pub package_roots: BTreeSet<PackageKey>,
}

/// Solves before prompting so abandoned releases and incompatible providers never
/// become persistent policy. Each chosen binding is re-solved with all roots and
/// locks; newly selected providers can introduce further virtual dependencies.
#[allow(clippy::too_many_arguments)]
pub(super) fn resolve_virtual_selections(
    universe: &PackageUniverse,
    channel: &str,
    names: &[String],
    config: &SystemConfig,
    installed: &[sage_db::InstalledPackage],
    provider_overrides: &[(String, String)],
    interactive: bool,
    prefer_latest: bool,
) -> Result<VirtualSelections> {
    let requested = names
        .iter()
        .map(|name| PackageKey::in_channel(channel, name))
        .collect::<Result<Vec<_>, _>>()?;
    // PackageKey always contains a concrete slot, so retain optional virtual
    // constraints from the original selector before deduplicating exact roots.
    let requested_virtuals: Vec<_> = requested
        .iter()
        .zip(names)
        .filter(|(key, _)| key.name.starts_with("virtual/"))
        .map(|(key, selector)| Dependency {
            name: key.name.clone(),
            slot: selector.split_once(':').map(|(_, slot)| slot.into()),
            channel: None,
            op: ConstraintOp::Any,
            version: None,
        })
        .collect();
    let mut bindings: ProviderBindings = config
        .provider_preferences("main/system")?
        .into_iter()
        .map(|(symbol, key)| ((key.channel.clone(), symbol), key))
        .collect();
    let system_channel = channel.rsplit_once('/').map_or_else(
        || "system".to_string(),
        |(root, _)| format!("{root}/system"),
    );
    let mut overridden = BTreeSet::new();
    for (interface, selector) in provider_overrides {
        let symbol = provider_symbol(interface);
        if !overridden.insert(symbol.clone()) {
            bail!("duplicate provider override for {symbol}");
        }
        // Reuse configuration validation, including empty names and duplicate aliases.
        let mut override_config = config.clone();
        override_config.providers = BTreeMap::from([(interface.clone(), selector.clone())]);
        let key = override_config.provider_preferences(&system_channel)?[&symbol].clone();
        bindings.insert((system_channel.clone(), symbol), key);
    }
    let mut roots: Vec<_> = requested
        .iter()
        .filter(|key| !key.name.starts_with("virtual/"))
        .cloned()
        .collect();
    roots.extend(installed.iter().map(|package| package.key.clone()));
    // Explicit overrides must be validated even without a consumer. Existing
    // declarations constrain reached edges; only rebuild converges all declared
    // roots, so an unrelated install cannot resurrect a retired provider.
    let mut requirements = requested_virtuals.clone();
    requirements.extend(overridden.iter().map(|symbol| Dependency {
        name: symbol.clone(),
        slot: None,
        channel: None,
        op: ConstraintOp::Any,
        version: None,
    }));
    roots.sort();
    roots.dedup();
    let mut locks: Vec<_> = installed
        .iter()
        .filter(|package| !prefer_latest || !requested.contains(&package.key))
        .map(|package| (package.key.clone(), package.version.clone()))
        .collect();
    let mut automatic = BTreeMap::<Dependency, PackageKey>::new();
    let solve = |automatic: &BTreeMap<Dependency, PackageKey>, locks: &[(PackageKey, Version)]| {
        SageSolver::with_locked(universe, locks.iter().cloned())
            .bind_providers(
                bindings
                    .iter()
                    .map(|((_, symbol), key)| (symbol.clone(), key.clone())),
            )
            .bind_provider_requirements(automatic.clone())
            .resolve_with_root_requirements(&roots, channel, &requirements)
    };
    let is_requested = |requirement: &Dependency| {
        requirement.channel.as_deref() == Some(&system_channel)
            && requirement.op == ConstraintOp::Any
            && requirement.version.is_none()
            && requested_virtuals
                .iter()
                .any(|root| root.name == requirement.name && root.slot == requirement.slot)
    };
    let is_configured = |requirement: &Dependency| {
        requirement.channel.as_ref().is_some_and(|channel| {
            bindings.contains_key(&(channel.clone(), requirement.name.clone()))
        })
    };
    loop {
        let (solution, choices) = solve(&automatic, &locks)?;
        let is_bound = |requirement: &Dependency| {
            is_configured(requirement) || automatic.contains_key(requirement)
        };
        // Bind directly requested providers before unlocking their releases, so
        // another installed provider's lock cannot displace the selected identity.
        // Re-solve before binding transitive choices: the upgraded release may
        // require different providers than the old release did.
        let previous_locks = locks.len();
        locks.retain(|(key, _)| {
            !choices.iter().any(|(requirement, chosen)| {
                prefer_latest && key == chosen && is_bound(requirement) && is_requested(requirement)
            })
        });
        if locks.len() != previous_locks {
            continue;
        }
        let unbound = choices
            .iter()
            .filter(|(requirement, _)| {
                requirement.name.starts_with("virtual/") && !is_bound(requirement)
            })
            .min_by_key(|(requirement, _)| !(prefer_latest && is_requested(requirement)));
        let Some((requirement, chosen)) = unbound else {
            let mut providers: BTreeMap<String, PackageKey> = BTreeMap::new();
            for (requirement, key) in &choices {
                // The system provider table belongs to main/system. Foreign
                // choices remain scoped to their graph and must not overwrite it.
                if key.channel != "main/system" {
                    continue;
                }
                // The persisted table describes global policy. A slot-specific
                // edge, or a symbol with different feasible choices, cannot be
                // promoted to one global binding without narrowing the graph.
                if is_configured(requirement)
                    || (automatic.contains_key(requirement)
                        && requirement.slot.is_none()
                        && choices.iter().all(|(other, selected)| {
                            other.channel != requirement.channel
                                || other.name != requirement.name
                                || selected == key
                        }))
                {
                    providers.insert(
                        requirement
                            .name
                            .strip_prefix("virtual/")
                            .unwrap_or(&requirement.name)
                            .into(),
                        key.clone(),
                    );
                }
            }
            let declarations: BTreeMap<_, _> = providers
                .iter()
                .filter(|(_, key)| key.channel == "main/system")
                .map(|(interface, key)| (interface.clone(), format!("{}:{}", key.name, key.slot)))
                .collect();
            // Direct scoped virtual requests still need declarative roots even
            // when their choices cannot be represented by the global table.
            let package_roots = choices
                .iter()
                .filter(|(requirement, key)| {
                    is_requested(requirement)
                        && key.channel == "main/system"
                        && !declarations.contains_key(
                            requirement
                                .name
                                .strip_prefix("virtual/")
                                .unwrap_or(&requirement.name),
                        )
                })
                .map(|(_, key)| key.clone())
                .collect();
            return Ok(VirtualSelections {
                solution,
                providers,
                declarations,
                package_roots,
            });
        };
        let selected = if interactive {
            let mut candidates = Vec::new();
            for candidate in universe.providers_for(&requirement.name) {
                if candidate.channel != chosen.channel {
                    continue;
                }
                let mut trial = automatic.clone();
                trial.insert(requirement.clone(), candidate.clone());
                if solve(&trial, &locks).is_ok() {
                    candidates.push(candidate.clone());
                }
            }
            // The current solution is the default, preserving installed choices.
            candidates.sort_by_key(|key| (key != chosen, key.clone()));
            select_virtual_provider(&requirement.to_string(), &candidates, None, true)?
        } else {
            chosen.clone()
        };
        automatic.insert(requirement.clone(), selected);
    }
}
