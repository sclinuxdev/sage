//! Transaction diff preview: computes and renders install, upgrade, remove, and size diffs.

use sage_core::{PackageKey, Version};
use sage_db::InstalledPackage;
use std::collections::BTreeMap;
use std::fmt::Write;

use crate::channel::AvailablePackages;
use crate::transaction::TransactionPlan;

/// Detailed categorization of changes in a transaction plan.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TransactionDiff {
    pub new_installs: Vec<InstallDiff>,
    pub upgrades: Vec<UpgradeDiff>,
    pub removals: Vec<RemoveDiff>,
    pub provider_bindings: Vec<(String, PackageKey)>,
    pub net_size_bytes: i64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InstallDiff {
    pub key: PackageKey,
    pub version: Version,
    pub size: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UpgradeDiff {
    pub key: PackageKey,
    pub old_version: Version,
    pub new_version: Version,
    pub old_size: u64,
    pub new_size: u64,
    pub size_delta: i64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RemoveDiff {
    pub key: PackageKey,
    pub version: Version,
    pub size: u64,
}

/// Computes a human-readable and structured diff from a planned transaction.
pub fn compute_transaction_diff(
    plan: &TransactionPlan,
    installed: &[InstalledPackage],
    available: Option<&AvailablePackages>,
) -> TransactionDiff {
    let installed_map: BTreeMap<&PackageKey, &InstalledPackage> =
        installed.iter().map(|pkg| (&pkg.key, pkg)).collect();

    let mut new_installs = Vec::new();
    let mut upgrades = Vec::new();
    let mut net_size_bytes: i64 = 0;

    for (key, version) in &plan.install {
        let new_size = available
            .and_then(|avail| avail.universe.release(key, version))
            .map(|rel| rel.installed_size)
            .unwrap_or(0);

        if let Some(existing) = installed_map.get(key) {
            let old_size = existing.installed_size;
            let size_delta = (new_size as i64) - (old_size as i64);
            net_size_bytes += size_delta;
            upgrades.push(UpgradeDiff {
                key: key.clone(),
                old_version: existing.version.clone(),
                new_version: version.clone(),
                old_size,
                new_size,
                size_delta,
            });
        } else {
            net_size_bytes += new_size as i64;
            new_installs.push(InstallDiff {
                key: key.clone(),
                version: version.clone(),
                size: new_size,
            });
        }
    }

    let install_keys: BTreeMap<&PackageKey, ()> =
        plan.install.iter().map(|(key, _)| (key, ())).collect();

    let mut removals = Vec::new();
    for pkg in &plan.remove {
        if !install_keys.contains_key(&pkg.key) {
            net_size_bytes -= pkg.installed_size as i64;
            removals.push(RemoveDiff {
                key: pkg.key.clone(),
                version: pkg.version.clone(),
                size: pkg.installed_size,
            });
        }
    }

    let provider_bindings = plan
        .provider_bindings
        .iter()
        .map(|(iface, key)| (iface.clone(), key.clone()))
        .collect();

    TransactionDiff {
        new_installs,
        upgrades,
        removals,
        provider_bindings,
        net_size_bytes,
    }
}

impl TransactionDiff {
    /// Renders the complete transaction preview for terminals and callers.
    pub fn render_summary(&self) -> String {
        let mut output = String::new();
        if self.new_installs.is_empty()
            && self.upgrades.is_empty()
            && self.removals.is_empty()
            && self.provider_bindings.is_empty()
        {
            writeln!(output, "No packages to install, upgrade, or remove.")
                .expect("writing to a String cannot fail");
            return output;
        }

        writeln!(output, "Transaction Preview:").expect("writing to a String cannot fail");
        if !self.new_installs.is_empty() {
            writeln!(output, "  Install ({} packages):", self.new_installs.len())
                .expect("writing to a String cannot fail");
            for item in &self.new_installs {
                writeln!(
                    output,
                    "    [+] {} {} ({})",
                    item.key,
                    item.version,
                    format_bytes(item.size as i64)
                )
                .expect("writing to a String cannot fail");
            }
        }

        if !self.upgrades.is_empty() {
            writeln!(output, "  Upgrade ({} packages):", self.upgrades.len())
                .expect("writing to a String cannot fail");
            for item in &self.upgrades {
                let sign = if item.size_delta >= 0 { "+" } else { "" };
                writeln!(
                    output,
                    "    [^] {} {} -> {} ({}{})",
                    item.key,
                    item.old_version,
                    item.new_version,
                    sign,
                    format_bytes(item.size_delta)
                )
                .expect("writing to a String cannot fail");
            }
        }

        if !self.removals.is_empty() {
            writeln!(output, "  Remove ({} packages):", self.removals.len())
                .expect("writing to a String cannot fail");
            for item in &self.removals {
                writeln!(
                    output,
                    "    [-] {} {} (-{})",
                    item.key,
                    item.version,
                    format_bytes(item.size as i64)
                )
                .expect("writing to a String cannot fail");
            }
        }

        if !self.provider_bindings.is_empty() {
            writeln!(output, "  Provider Bindings:").expect("writing to a String cannot fail");
            for (iface, key) in &self.provider_bindings {
                writeln!(output, "    [*] {iface} -> {key}")
                    .expect("writing to a String cannot fail");
            }
        }

        let net_sign = if self.net_size_bytes >= 0 { "+" } else { "" };
        writeln!(
            output,
            "\nNet disk space change: {}{}",
            net_sign,
            format_bytes(self.net_size_bytes)
        )
        .expect("writing to a String cannot fail");
        output
    }

    /// Prints every planned change, including transactions with only bindings.
    pub fn print_summary(&self) {
        print!("{}", self.render_summary());
    }
}

/// Formats a byte count into human-readable string.
pub fn format_bytes(bytes: i64) -> String {
    let abs_b = bytes.unsigned_abs() as f64;
    if abs_b < 1024.0 {
        format!("{bytes} B")
    } else if abs_b < 1024.0 * 1024.0 {
        format!("{:.1} KB", bytes as f64 / 1024.0)
    } else if abs_b < 1024.0 * 1024.0 * 1024.0 {
        format!("{:.1} MB", bytes as f64 / (1024.0 * 1024.0))
    } else {
        format!("{:.2} GB", bytes as f64 / (1024.0 * 1024.0 * 1024.0))
    }
}
