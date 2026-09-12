//! Virtual selection based on feasible solver results and exact provider identities.

use std::collections::{BTreeMap, BTreeSet};
use std::io::IsTerminal;

use anyhow::{Result, bail};
use sage_core::{DEFAULT_SLOT, PackageKey, Version};
use sage_solver::{PackageUniverse, SageSolver, Solution};

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
    let mut bindings = config.provider_preferences("main/system")?;
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
        bindings.insert(symbol, key);
    }
    let mut roots = requested.clone();
    roots.extend(installed.iter().map(|package| package.key.clone()));
    // Explicit overrides must be validated even without a consumer. Existing
    // declarations constrain reached edges; only rebuild converges all declared
    // roots, so an unrelated install cannot resurrect a retired provider.
    roots.extend(
        overridden
            .iter()
            .map(|symbol| PackageKey::new(&bindings[symbol].channel, symbol, DEFAULT_SLOT)),
    );
    roots.sort();
    roots.dedup();
    let mut locks: Vec<_> = installed
        .iter()
        .filter(|package| !prefer_latest || !requested.contains(&package.key))
        .map(|package| (package.key.clone(), package.version.clone()))
        .collect();
    let solve = |bindings: &BTreeMap<String, PackageKey>, locks: &[(PackageKey, Version)]| {
        SageSolver::with_locked(universe, locks.iter().cloned())
            .bind_providers(bindings.clone())
            .resolve_with_provider_choices(&roots)
    };
    loop {
        let (solution, choices) = solve(&bindings, &locks)?;
        let is_upgrade_root = |symbol: &str, key: &PackageKey| {
            prefer_latest
                && key.channel == system_channel
                && requested.iter().any(|root| {
                    root.name == symbol && (root.slot == DEFAULT_SLOT || root.slot == key.slot)
                })
        };
        // Bind directly requested providers before unlocking their releases, so
        // another installed provider's lock cannot displace the selected identity.
        // Re-solve before binding transitive choices: the upgraded release may
        // require different providers than the old release did.
        let previous_locks = locks.len();
        locks.retain(|(key, _)| {
            !choices.iter().any(|(symbol, chosen)| {
                key == chosen && bindings.contains_key(symbol) && is_upgrade_root(symbol, key)
            })
        });
        if locks.len() != previous_locks {
            continue;
        }
        let unbound = choices
            .iter()
            .filter(|(symbol, _)| symbol.starts_with("virtual/") && !bindings.contains_key(symbol))
            .min_by_key(|(symbol, key)| !is_upgrade_root(symbol, key));
        let Some((symbol, chosen)) = unbound else {
            let mut providers: BTreeMap<String, PackageKey> = BTreeMap::new();
            for (symbol, key) in choices {
                if bindings.contains_key(&symbol) {
                    providers.insert(
                        symbol.strip_prefix("virtual/").unwrap_or(&symbol).into(),
                        key,
                    );
                }
            }
            let declarations = providers
                .iter()
                .filter(|(_, key)| key.channel == "main/system")
                .map(|(interface, key)| (interface.clone(), format!("{}:{}", key.name, key.slot)))
                .collect();
            return Ok(VirtualSelections {
                solution,
                providers,
                declarations,
            });
        };
        let selected = if interactive {
            let mut candidates = Vec::new();
            for candidate in universe.providers_for(symbol) {
                if candidate.channel != chosen.channel {
                    continue;
                }
                let mut trial = bindings.clone();
                trial.insert(symbol.clone(), candidate.clone());
                if solve(&trial, &locks).is_ok() {
                    candidates.push(candidate.clone());
                }
            }
            // The current solution is the default, preserving installed choices.
            candidates.sort_by_key(|key| (key != chosen, key.clone()));
            select_virtual_provider(symbol, &candidates, None, true)?
        } else {
            chosen.clone()
        };
        bindings.insert(symbol.clone(), selected);
    }
}
