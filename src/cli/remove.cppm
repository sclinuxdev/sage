export module sage.cli.remove;

// End-to-end `sage remove <PKG...>`: cascade expansion, reverse-dependency
// protection, anchored file deletion and post-remove triggers.
import std;
import sage;

import sage.cli;

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
    for (std::size_t i = 0; i < all_installed->size(); ++i) {
        const auto& pkg = (*all_installed)[i];
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

    auto transaction_res =
        sage::archive::FilesystemTransaction::create(opts.target_root);
    if (!transaction_res) {
        sage::util::log_error(
            "Failed to create filesystem transaction: {}", transaction_res.error());
        return 1;
    }
    auto& filesystem_transaction = *transaction_res;

    // One remove command is one journaled operation: the entire deletion plan
    // is prepared up front, committed together with the metadata deregistration
    // in a single LMDB transaction, and only then published by the shared
    // recovery/resume driver (which also runs the post-remove triggers).
    sage::archive::JournalContext journal_ctx;
    journal_ctx.kind = "remove";
    journal_ctx.final = true;
    journal_ctx.sysroot = opts.target_root.string();
    journal_ctx.regenerate_profile = true;

    for (const auto& pkg_name : to_remove_set) {
        const auto& pkg = installed_map.at(pkg_name);
        if (std::find(opts.args.begin(), opts.args.end(), pkg_name) != opts.args.end()) {
            sage::util::log_info("Removing package '{}'...", pkg_name);
        } else {
            sage::util::log_info("Auto-removing orphaned dependency '{}' (version {})...", pkg_name, pkg.version.to_string());
        }
        if (!pkg.service_toml.empty()) {
            auto service = sage::service::ServiceSpec::parse_toml(pkg.service_toml);
            if (service) {
                auto retired = sage::service_registry::plan_remove_scripts(
                    db, *wtxn, filesystem_transaction, service->name, pkg_name);
                if (!retired) {
                    sage::util::log_error(
                        "Cannot retire generated service paths for '{}': {}",
                        pkg_name, retired.error());
                    return 1;
                }
                journal_ctx.touched.insert(
                    journal_ctx.touched.end(), retired->begin(), retired->end());
            } else {
                sage::util::log_warn(
                    "Cannot identify generated service paths for '{}': {}",
                    pkg_name, service.error());
            }
        }

        // The LMDB files table is the authoritative owner registry: merge the
        // installed manifest's file list with all files still registered to
        // this package (a previous version's leftovers may not be present in
        // the current manifest), so stale ownership records are purged.
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

            const auto normalized = relative_path.generic_string();
            // Journaled removal instead of immediate physical deletion. The
            // live node decides R F vs R D. Alias-spelled claims (usr/sbin vs
            // usr/bin) release the registry claim without touching the
            // physical file, like remove_path_anchored did.
            auto canonical = sage::archive::canonicalize_merge_claim(normalized);
            if (canonical && *canonical == normalized) {
                std::error_code status_ec;
                const auto live = std::filesystem::symlink_status(
                    opts.target_root / std::filesystem::path(normalized), status_ec);
                if (!status_ec && std::filesystem::is_directory(live)) {
                    // remove_path_anchored semantics, checked at plan time:
                    // a sole claim on an undeclared directory that has since
                    // gained contents was a hard removal error -- it must
                    // abort HERE, before the single metadata transaction
                    // commits, so the package records stay intact. Declared
                    // or unowned directories tolerate foreign leftovers via
                    // R D's ENOTEMPTY-preserve rule.
                    const bool declared_directory =
                        declared_types.contains(normalized)
                        && declared_types.at(normalized)
                            == sage::package::FileType::Directory;
                    const bool ignore_nonempty = owners->empty() || declared_directory;
                    std::error_code empty_ec;
                    if (!ignore_nonempty && !std::filesystem::is_empty(
                            opts.target_root / std::filesystem::path(normalized),
                            empty_ec)) {
                        sage::util::log_error(
                            "Failed to remove '{}' from package '{}': Directory not empty",
                            normalized, pkg_name);
                        return 1;
                    }
                    filesystem_transaction.plan_remove_dir(normalized);
                } else {
                    filesystem_transaction.plan_remove_file(normalized);
                }
            }
            journal_ctx.touched.emplace_back(
                file_entry.type == sage::package::FileType::Directory ? 'D'
                : file_entry.type == sage::package::FileType::Symlink ? 'L' : 'F',
                normalized);
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
                "Failed to unregister provides for '{}': {}", pkg_name, unregister_provides.error());
            return 1;
        }
        auto delete_package = db.del_package(*wtxn, pkg_name);
        if (!delete_package) {
            sage::util::log_error(
                "Failed to delete package record for '{}': {}",
                pkg_name, delete_package.error());
            return 1;
        }

        // The removed manifests become the trigger context's transaction
        // packages: it is their capabilities that make kernel triggers fire.
        journal_ctx.package_manifests_toml.push_back(pkg.serialize_toml());
    }

    // Evidence first: the journal (fsynced) must exist before the operation
    // record becomes visible in LMDB.
    const auto journal_text = sage::archive::render_journal(
        journal_ctx, filesystem_transaction.journal_entries());
    auto journal_hash = filesystem_transaction.persist_journal(journal_text);
    if (!journal_hash) {
        sage::util::log_error(
            "Failed to persist removal journal: {}", journal_hash.error());
        return 1;
    }

    sage::db::FilesystemOperationRecord operation;
    operation.id = filesystem_transaction.id();
    operation.kind = "remove";
    operation.phase = std::string(sage::db::phase_filesystem_pending);
    operation.transaction_dir = filesystem_transaction.relative_dir();
    operation.journal_sha256 = *journal_hash;
    auto op_res = db.put_operation(*wtxn, operation);
    if (!op_res) {
        sage::util::log_error(
            "Failed to record removal operation: {}", op_res.error());
        return 1;
    }

    auto commit_res = wtxn->commit();
    if (!commit_res) {
        sage::util::log_error(
            "Failed to commit removal transaction: {}", commit_res.error());
        return 1;
    }

    // Publish the removals and run post-remove processing through the shared
    // driver: profile regeneration, triggers, record deletion and transaction
    // retirement on success.
    auto resumed = sage::rebuild::resume_pending_operations(db, opts.target_root);
    if (!resumed) {
        sage::util::log_error(
            "Failed to complete pending filesystem operations: {}", resumed.error());
        return 1;
    }
    auto repaired_services = sage::rebuild::ReconcileEngine::repair_missing_services(
        db, opts.target_root);
    if (!repaired_services) {
        sage::util::log_error(
            "Package removal completed, but service repair failed: {}",
            repaired_services.error());
        return 1;
    }

    sage::util::log_success("Successfully removed {} packages (including orphaned dependencies) from {}",
        to_remove_set.size(), opts.target_root.string());
    return 0;
}
} // namespace sage::cli
