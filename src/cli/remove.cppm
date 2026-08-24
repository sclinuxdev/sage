export module sage.cli.remove;

// End-to-end `sage remove <PKG...>`: cascade expansion, reverse-dependency
// protection, anchored file deletion and post-remove triggers.
import std;
import sage;

import sage.cli;
import sage.triggers;

namespace sage::cli {

export int cmd_remove(
    const CliOptions& opts,
    DatabaseSnapshot database_snapshot = DatabaseSnapshot::Unchecked)
{
    if (opts.args.empty()) {
        std::println("Usage: sage remove [--cascade|-c] [--nodeps|-d] <PKG...>");
        return 1;
    }
    if (opts.dry_run && database_snapshot == DatabaseSnapshot::Unchecked) {
        sage::util::log_error("Dry-run remove requires a synchronized database snapshot");
        return 1;
    }

    auto cfg_res = sage::config::SystemConfig::load_from_root(opts.target_root);
    if (!cfg_res) {
        sage::util::log_error("Failed to load configuration: {}", cfg_res.error());
        return 1;
    }
    const auto& cfg = *cfg_res;

    if (opts.dry_run && database_snapshot == DatabaseSnapshot::Absent) {
        for (const auto& pkg_name : opts.args) {
            sage::util::log_warn("Package '{}' is not installed, skipping", pkg_name);
        }
        sage::util::log_info("No matching installed packages found to remove.");
        return 0;
    }

    auto db_res = opts.dry_run
        ? sage::db::Database::open_existing_read_only_no_lock(cfg.db_path)
        : sage::db::Database::open(cfg.db_path);
    if (!db_res) {
        sage::util::log_error("Failed to open database: {}", db_res.error());
        return 1;
    }
    auto& db = *db_res;

    auto all_installed = db.list_installed_packages();
    if (!all_installed) {
        sage::util::log_error("Installed package database is inconsistent: {}", all_installed.error());
        return 1;
    }
    std::map<std::string, sage::package::PackageManifest> installed_map;
    for (const auto& pkg : *all_installed) {
        installed_map[pkg.name] = pkg;
    }

    std::set<std::string> to_remove_set;
    for (const auto& pkg_name : opts.args) {
        if (installed_map.contains(pkg_name)) {
            to_remove_set.insert(pkg_name);
        } else {
            sage::util::log_warn("Package '{}' is not installed, skipping", pkg_name);
        }
    }

    if (to_remove_set.empty()) {
        sage::util::log_info("No matching installed packages found to remove.");
        return 0;
    }

    // 1. Core System Provider Protection (unless --nodeps/--force is specified)
    if (!opts.force) {
        for (const auto& pkg_name : to_remove_set) {
            for (const auto& [iface, prov_target] : cfg.providers) {
                if (pkg_name == prov_target) {
                    sage::util::log_error("Cannot remove core system package '{}' (active provider for interface '{}').", pkg_name, iface);
                    sage::util::log_info("Tip: update /etc/sage/system.toml and run 'sage rebuild' to swap providers, or pass '--nodeps' to bypass.");
                    return 1;
                }
            }
        }
    }

    // 2. Cascade Expansion OR Reverse Dependency Protection Check
    if (opts.cascade) {
        // Cascade: recursively add all packages that depend on anything in to_remove_set
        bool cascaded = true;
        while (cascaded) {
            cascaded = false;
            for (const auto& [inst_name, inst_pkg] : installed_map) {
                if (to_remove_set.contains(inst_name)) continue;
                for (const auto& dep : inst_pkg.dependencies) {
                    for (const auto& r_pkg_name : to_remove_set) {
                        const auto& r_pkg = installed_map.at(r_pkg_name);
                        bool match = (dep.name == r_pkg.name);
                        if (!match) {
                            for (const auto& prov : r_pkg.provides) {
                                if (prov == dep.name || prov.starts_with(dep.name + " ")) {
                                    match = true;
                                    break;
                                }
                            }
                        }
                        if (match && dep.satisfies(r_pkg.version)) {
                            to_remove_set.insert(inst_name);
                            cascaded = true;
                            break;
                        }
                    }
                    if (cascaded) break;
                }
            }
        }
    } else if (!opts.force) {
        // Reverse Dependency Protection: ensure no remaining package depends on the package(s) being removed
        std::map<std::string, std::vector<std::string>> broken_deps;

        for (const auto& r_pkg_name : to_remove_set) {
            const auto& r_pkg = installed_map.at(r_pkg_name);
            for (const auto& [inst_name, inst_pkg] : installed_map) {
                if (to_remove_set.contains(inst_name)) continue;

                for (const auto& dep : inst_pkg.dependencies) {
                    bool match = (dep.name == r_pkg.name);
                    if (!match) {
                        for (const auto& prov : r_pkg.provides) {
                            if (prov == dep.name || prov.starts_with(dep.name + " ")) {
                                match = true;
                                break;
                            }
                        }
                    }
                    if (match && dep.satisfies(r_pkg.version)) {
                        // Check if another remaining package satisfies this dependency (alternative provider)
                        bool alt_available = false;
                        for (const auto& [alt_name, alt_pkg] : installed_map) {
                            if (to_remove_set.contains(alt_name) || alt_name == r_pkg_name) continue;
                            bool alt_match = (dep.name == alt_pkg.name);
                            if (!alt_match) {
                                for (const auto& prov : alt_pkg.provides) {
                                    if (prov == dep.name || prov.starts_with(dep.name + " ")) {
                                        alt_match = true;
                                        break;
                                    }
                                }
                            }
                            if (alt_match && dep.satisfies(alt_pkg.version)) {
                                alt_available = true;
                                break;
                            }
                        }

                        if (!alt_available) {
                            broken_deps[r_pkg_name].push_back(std::format("{} (requires '{}')", inst_name, dep.to_string()));
                        }
                    }
                }
            }
        }

        if (!broken_deps.empty()) {
            sage::util::log_error("Failed to prepare transaction: breaking dependencies for installed packages");
            for (const auto& [pkg_name, requirers] : broken_deps) {
                std::println(std::cerr, "  :: Unable to remove '{}', required by:", pkg_name);
                for (const auto& req : requirers) {
                    std::println(std::cerr, "     - {}", req);
                }
            }
            std::println(std::cerr, "Tip: use 'sage remove --cascade <PKG>' to remove dependent packages as well, or '--nodeps' to force removal.");
            return 1;
        }
    }

    // 3. Iteratively discover orphaned dependencies
    if (!opts.no_recursive) {
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& r_pkg_name : to_remove_set) {
                const auto& r_pkg = installed_map.at(r_pkg_name);
                for (const auto& dep : r_pkg.dependencies) {
                    for (const auto& [inst_name, inst_pkg] : installed_map) {
                        if (to_remove_set.contains(inst_name)) continue;

                        bool match = (inst_name == dep.name);
                        if (!match) {
                            for (const auto& prov : inst_pkg.provides) {
                                if (prov == dep.name || prov.starts_with(dep.name + " ")) {
                                    match = true;
                                    break;
                                }
                            }
                        }

                        if (match && dep.satisfies(inst_pkg.version)) {
                            // Check if any remaining installed package outside to_remove_set still needs inst_name
                            bool needed_by_others = false;
                            for (const auto& [other_name, other_pkg] : installed_map) {
                                if (to_remove_set.contains(other_name) || other_name == inst_name) continue;
                                for (const auto& other_dep : other_pkg.dependencies) {
                                    bool other_match = (other_dep.name == inst_name);
                                    if (!other_match) {
                                        for (const auto& prov : other_pkg.provides) {
                                            if (prov == other_dep.name || prov.starts_with(other_dep.name + " ")) {
                                                other_match = true;
                                                break;
                                            }
                                        }
                                    }
                                    if (other_match && other_dep.satisfies(inst_pkg.version)) {
                                        needed_by_others = true;
                                        break;
                                    }
                                }
                                if (needed_by_others) break;
                            }

                            // Also protect virtual system provider locks (e.g. virtual/init, virtual/libc)
                            for (const auto& [iface, prov_target] : cfg.providers) {
                                if (inst_name == prov_target) {
                                    needed_by_others = true;
                                    break;
                                }
                            }

                            if (!needed_by_others) {
                                to_remove_set.insert(inst_name);
                                changed = true;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    if (opts.dry_run) {
        sage::util::log_info("Dry-run removal preview for {} packages (including orphaned dependencies):", to_remove_set.size());
        for (const auto& name : to_remove_set) {
            std::println("  - {}", name);
        }
        return 0;
    }

    auto wtxn = db.begin_write_txn();
    if (!wtxn) return 1;

    // The dependency/removal plan was built before taking the writer lock.
    // Abort if any package was installed, removed, or replaced in that window;
    // otherwise stale plans can delete a newer same-name package or break a
    // dependency introduced concurrently.
    auto current_installed = db.list_installed_packages(*wtxn);
    if (!current_installed) {
        sage::util::log_error(
            "Failed to revalidate installed packages before removal: {}",
            current_installed.error());
        return 1;
    }
    if (current_installed->size() != installed_map.size()) {
        sage::util::log_error(
            "Installed package state changed while planning removal; retry the command");
        return 1;
    }
    for (const auto& current : *current_installed) {
        auto planned = installed_map.find(current.name);
        if (planned == installed_map.end()
            || planned->second.serialize_toml() != current.serialize_toml()) {
            sage::util::log_error(
                "Installed package '{}' changed while planning removal; retry the command",
                current.name);
            return 1;
        }
    }

    std::vector<sage::package::FileEntry> removed_files;

    for (const auto& pkg_name : to_remove_set) {
        const auto& pkg = installed_map.at(pkg_name);
        if (std::find(opts.args.begin(), opts.args.end(), pkg_name) != opts.args.end()) {
            sage::util::log_info("Removing package '{}'...", pkg_name);
        } else {
            sage::util::log_info("Auto-removing orphaned dependency '{}' (version {})...", pkg_name, pkg.version.to_string());
        }

        // Delete physical files. The LMDB files table is the authoritative owner
        // registry: merge the installed manifest's file list with all files still
        // registered to this package (a previous version's leftovers may not be
        // present in the current manifest), so stale ownership records are purged.
        std::unordered_map<std::string, sage::package::FileType> declared_types;
        for (const auto& f : pkg.files) {
            declared_types.emplace(sage::util::clean_rel_path(f.path), f.type);
        }
        auto files_to_delete = pkg.files;
        std::unordered_set<std::string> seen_paths;
        for (const auto& f : files_to_delete) {
            seen_paths.insert(sage::util::clean_rel_path(f.path));
        }
        auto registered_files = db.get_package_files(*wtxn, pkg_name);
        if (!registered_files) {
            sage::util::log_error(
                "Failed to read files owned by '{}': {}", pkg_name, registered_files.error());
            return 1;
        }
        for (const auto& fp : *registered_files) {
            if (seen_paths.insert(fp).second) {
                sage::package::FileEntry fe;
                fe.path = fp;
                const auto declared = declared_types.find(fp);
                if (declared != declared_types.end()) fe.type = declared->second;
                files_to_delete.push_back(std::move(fe));
            }
        }
        std::ranges::stable_sort(files_to_delete, [&](const auto& lhs, const auto& rhs) {
            return sage::util::path_depth(lhs.path) > sage::util::path_depth(rhs.path);
        });

        std::string my_owner = std::format("{}:{}", pkg_name, pkg.channel);
        for (const auto& file_entry : files_to_delete) {
            auto owners = db.get_path_owners(*wtxn, file_entry.path);
            if (!owners) {
                sage::util::log_error(
                    "Failed to read ownership for '{}': {}",
                    file_entry.path, owners.error());
                return 1;
            }
            const bool mine =
                std::ranges::find(*owners, my_owner) != owners->end();
            if (!mine && !owners->empty()) {
                continue;
            }
            if (mine && owners->size() > 1) {
                // Shared directory with surviving owners: release only this
                // package's claim below and leave the directory in place.
                continue;
            }

            auto relative_path = std::filesystem::path(
                sage::util::clean_rel_path(file_entry.path)).lexically_normal();
            bool escapes_root = relative_path.empty() || relative_path == "."
                || relative_path.is_absolute() || relative_path.has_root_name()
                || relative_path.has_root_directory();
            for (const auto& component : relative_path) {
                if (component == "..") {
                    escapes_root = true;
                    break;
                }
            }
            if (escapes_root) {
                sage::util::log_error(
                    "Refusing to remove invalid package path '{}' for '{}'",
                    file_entry.path, pkg_name);
                return 1;
            }

            // Unregistered debris is removed best-effort; a declared directory
            // may legitimately have gained foreign files since install.
            const auto normalized = relative_path.generic_string();
            const bool declared_directory =
                declared_types.contains(normalized)
                && declared_types.at(normalized) == sage::package::FileType::Directory;
            auto remove_res = sage::archive::remove_path_anchored(
                opts.target_root,
                normalized,
                owners->empty() || declared_directory);
            if (!remove_res) {
                sage::util::log_error(
                    "Failed to remove '{}' from package '{}': {}",
                    normalized, pkg_name, remove_res.error());
                return 1;
            }

            auto removed_entry = file_entry;
            removed_entry.path = normalized;
            removed_files.push_back(std::move(removed_entry));
        }

        auto unregister_files = db.unregister_files(*wtxn, files_to_delete, my_owner);
        if (!unregister_files) {
            sage::util::log_error(
                "Failed to unregister files for '{}': {}", pkg_name, unregister_files.error());
            return 1;
        }
        auto unregister_provides = db.unregister_provides(*wtxn, pkg.provides);
        if (!unregister_provides) {
            sage::util::log_error(
                "Failed to unregister provides for '{}': {}",
                pkg_name, unregister_provides.error());
            return 1;
        }
        auto delete_package = db.del_package(*wtxn, pkg_name);
        if (!delete_package) {
            sage::util::log_error(
                "Failed to delete package record for '{}': {}",
                pkg_name, delete_package.error());
            return 1;
        }
    }

    auto commit_res = wtxn->commit();
    if (!commit_res) return 1;

    // The filesystem and package database are committed at this point, so the
    // generated profile must reflect the new state even when a post-remove
    // trigger fails below.
    std::vector<sage::channel::Channel> active_channels;
    for (const auto& ch_cfg : cfg.channels) {
        sage::channel::Channel ch;
        ch.name = ch_cfg.name;
        ch.scope = sage::channel::parse_scope(ch_cfg.scope);
        ch.enabled = ch_cfg.enabled;
        active_channels.push_back(std::move(ch));
    }
    (void)sage::channel::ProfileManager::regenerate_fhs_profile(
        opts.target_root, active_channels);

    // Removing a kernel is as much a reason to regenerate the initramfs and
    // the bootloader entries as installing one, so removal runs the same
    // triggers -- with the removed packages as the transaction set, since it
    // is their capabilities that make the kernel triggers fire.
    sage::triggers::TriggerContext trig_ctx;
    trig_ctx.sysroot = opts.target_root;
    trig_ctx.touched_files = removed_files;
    auto remaining_packages = db.list_installed_packages();
    if (!remaining_packages) {
        sage::util::log_error(
            "Cannot run post-remove triggers: {}", remaining_packages.error());
        return 1;
    }
    trig_ctx.installed_packages = std::move(*remaining_packages);
    trig_ctx.providers = cfg.providers;
    trig_ctx.dry_run = opts.dry_run;
    for (const auto& [name, pkg] : installed_map) {
        if (to_remove_set.contains(name)) trig_ctx.transaction_packages.push_back(pkg);
    }
    auto trigger_result = sage::triggers::TriggerEngine::run(trig_ctx);
    if (!trigger_result) {
        sage::util::log_error(
            "Post-remove trigger failed: {}", trigger_result.error());
        return 1;
    }

    sage::util::log_success("Successfully removed {} packages (including orphaned dependencies) from {}",
        to_remove_set.size(), opts.target_root.string());
    return 0;
}
} // namespace sage::cli
