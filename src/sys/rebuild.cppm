export module sage.rebuild;

// Declarative reconcile engine: diff /etc/sage/system.toml against LMDB
// state, plan provider swaps, and commit guarded transitions.

import std;
import sage.archive;
import sage.channel;
import sage.config;
import sage.package;
import sage.repo;
import sage.service;
import sage.db;
import sage.solver;
import sage.triggers;
import sage.util;

export namespace sage::rebuild {

using std::size_t;

struct ProviderSwap {
    std::string iface;
    std::string current_provider;
    std::string target_provider;
};

struct PlannedPackageRemoval {
    std::string name;
    std::optional<package::PackageIdentity> expected_identity;
};

struct ReconcilePlan {
    std::vector<ProviderSwap> swaps;
    std::vector<package::PackageManifest> packages_to_install;
    std::vector<PlannedPackageRemoval> packages_to_remove;
    // Channel snapshot fetched during planning; execute unpacks installs from
    // plan.repos.archives, so planning and payload can never disagree.
    repo::RepoSnapshot repos;
    service::InitType target_init{service::InitType::OpenRC};
    bool has_changes{false};
};

class ReconcileEngine {
public:
    static std::expected<ReconcilePlan, std::string> calculate_diff(
        db::Database& db,
        const config::SystemConfig& desired_config,
        bool persist_cache = true)
    {
        ReconcilePlan plan;
        auto current_providers = db.get_all_system_providers();
        if (!current_providers) {
            return std::unexpected(
                "Failed to read current system providers: " + current_providers.error());
        }

        // 1. Calculate provider diffs.
        //
        // Only *exclusive* capabilities take part: they are the ones where at
        // most one provider may exist, so a changed binding means packages
        // must actually be swapped. Retargeting a shared default such as
        // virtual/initramfs-generator changes which tool later transactions
        // call, not what is installed -- reconciling on it would uninstall a
        // perfectly valid coexisting provider.
        for (const auto& [iface, target_prov] : desired_config.exclusive_providers()) {
            std::string cur = current_providers->contains(iface) ? current_providers->at(iface) : "";
            if (cur != target_prov) {
                plan.swaps.push_back(ProviderSwap{
                    .iface = iface,
                    .current_provider = cur,
                    .target_provider = target_prov
                });
                plan.has_changes = true;
            }
        }

        // Determine active init system
        std::string init_prov = desired_config.providers.contains("virtual/init") ? 
                                desired_config.providers.at("virtual/init") : "openrc";
        plan.target_init = service::parse_init_type(init_prov);
        if (plan.target_init == service::InitType::Unknown) {
            return std::unexpected(std::format(
                "Unsupported virtual/init provider '{}'", init_prov));
        }

        if (!plan.has_changes) {
            return plan;
        }

        // 2. Fetch channels: the solve pool and the archives behind it. Only
        // reached when a swap is pending, so the common no-op reconcile never
        // touches the network.
        auto snapshot_res = repo::fetch_repo_snapshot(
            desired_config, {}, persist_cache);
        if (!snapshot_res) return std::unexpected(snapshot_res.error());
        plan.repos = std::move(*snapshot_res);

        // 3. Solve dependencies for new providers
        std::vector<package::Dependency> root_reqs;
        for (const auto& swap : plan.swaps) {
            root_reqs.push_back(package::Dependency{
                .name = swap.target_provider,
                .op = package::ConstraintOp::Any,
                .version = {}
            });
            if (!swap.current_provider.empty() && swap.current_provider != swap.target_provider) {
                auto current_package = db.get_package(swap.current_provider);
                if (!current_package) {
                    return std::unexpected(std::format(
                        "Failed to read current provider package '{}': {}",
                        swap.current_provider, current_package.error()));
                }
                plan.packages_to_remove.push_back(PlannedPackageRemoval{
                    .name = swap.current_provider,
                    .expected_identity = *current_package
                        ? std::optional{package::package_identity(**current_package)}
                        : std::nullopt,
                });
            }
        }

        solver::DependencySolver solver(plan.repos.pool, desired_config.providers);
        auto solve_res = solver.solve(root_reqs);
        if (!solve_res) {
            return std::unexpected("Reconcile dependency resolution failed: " + solve_res.error());
        }

        plan.packages_to_install = *solve_res;
        return plan;
    }

    static std::expected<void, std::string> execute(
        db::Database& db,
        const ReconcilePlan& plan,
        const std::filesystem::path& sysroot = "/",
        bool dry_run = false,
        const std::map<std::string, std::string>& providers = {})
    {
        if (!plan.has_changes) {
            util::log_info("System state matches desired configuration. No reconcile needed.");
            return {};
        }

        util::log_info("Executing Declarative System Reconcile (Target Init: {})...", service::to_string(plan.target_init));

        for (const auto& swap : plan.swaps) {
            util::log_info("  • Swapping interface '{}': [{}] -> [{}]", 
                swap.iface, 
                swap.current_provider.empty() ? "none" : swap.current_provider, 
                swap.target_provider);
        }

        if (dry_run) {
            util::log_info("Dry-run preview completed successfully (no changes applied).");
            return {};
        }

        auto wtxn = db.begin_write_txn();
        if (!wtxn) return std::unexpected(std::string("Failed to open database write transaction"));

        // The plan was computed before taking the writer lock. Validate every
        // provider binding before changing any of them so a stale reconcile
        // cannot overwrite a concurrently committed provider choice.
        for (const auto& swap : plan.swaps) {
            auto current = db.get_system_provider(*wtxn, swap.iface);
            if (!current) {
                return std::unexpected(std::format(
                    "Failed to revalidate provider '{}': {}", swap.iface, current.error()));
            }
            const std::string current_name = *current ? **current : std::string{};
            if (current_name != swap.current_provider) {
                return std::unexpected(std::format(
                    "System provider '{}' changed after the reconcile plan was created",
                    swap.iface));
            }
        }

        // 1. Update system provider locks in LMDB
        for (const auto& swap : plan.swaps) {
            auto set_res = db.set_system_provider(*wtxn, swap.iface, swap.target_provider);
            if (!set_res) return std::unexpected(set_res.error());
        }

        // 2. Remove obsolete providers -- physically first: an exclusive
        // capability allows exactly one provider on disk, so the outgoing
        // package's files must go before the incoming ones land.
        for (const auto& removal : plan.packages_to_remove) {
            auto old_pkg = db.get_package(*wtxn, removal.name);
            if (!old_pkg) {
                return std::unexpected(std::format(
                    "Failed to read package '{}' in reconcile transaction: {}",
                    removal.name, old_pkg.error()));
            }
            auto current_identity = *old_pkg
                ? std::optional{package::package_identity(**old_pkg)}
                : std::nullopt;
            if (current_identity != removal.expected_identity) {
                return std::unexpected(std::format(
                    "Installed package '{}' changed after the reconcile plan was created",
                    removal.name));
            }
            if (*old_pkg) {
                const auto& old_manifest = **old_pkg;
                auto my_owner = std::format("{}:{}", removal.name, old_manifest.channel);

                // Manifest files plus anything still registered to this
                // package; children sort before parents so directories empty
                // out and can be pruned.
                std::set<std::string> paths;
                for (const auto& f : old_manifest.files) paths.insert(util::clean_rel_path(f.path));
                auto registered = db.get_package_files(*wtxn, removal.name);
                if (!registered) return std::unexpected(registered.error());
                paths.insert(registered->begin(), registered->end());
                std::vector<std::string> ordered(paths.begin(), paths.end());
                std::ranges::stable_sort(ordered, [&](const std::string& a, const std::string& b) {
                    return util::path_depth(a) > util::path_depth(b);
                });

                std::unordered_map<std::string, package::FileType> declared_types;
                for (const auto& f : old_manifest.files) {
                    declared_types.emplace(util::clean_rel_path(f.path), f.type);
                }
                for (const auto& path : ordered) {
                    auto owners = db.get_path_owners(*wtxn, path);
                    if (!owners) return std::unexpected(owners.error());
                    const bool mine =
                        std::ranges::find(*owners, my_owner) != owners->end();
                    if (!mine && !owners->empty()) continue;
                    if (mine && owners->size() > 1) continue;
                    const bool declared_directory =
                        declared_types.contains(path)
                        && declared_types.at(path) == package::FileType::Directory;
                    auto rm_res = archive::remove_path_anchored(
                        sysroot, path, !mine || declared_directory);
                    if (!rm_res) {
                        return std::unexpected(std::format(
                            "Failed to remove '{}' of outgoing provider '{}': {}",
                            path, removal.name, rm_res.error()));
                    }
                }

                auto file_res = db.unregister_files(*wtxn, old_manifest.files, my_owner);
                if (!file_res) return std::unexpected(file_res.error());
                auto provide_res = db.unregister_provides(*wtxn, old_manifest.provides);
                if (!provide_res) return std::unexpected(provide_res.error());
                auto delete_res = db.del_package(*wtxn, removal.name);
                if (!delete_res) return std::unexpected(delete_res.error());
                // Remove legacy service scripts
                service::remove_service(removal.name, service::InitType::OpenRC, sysroot);
                service::remove_service(removal.name, service::InitType::Systemd, sysroot);
                service::remove_service(removal.name, service::InitType::Runit, sysroot);
                service::remove_service(removal.name, service::InitType::Dinit, sysroot);
                service::remove_service(removal.name, service::InitType::S6, sysroot);
                service::remove_service(removal.name, service::InitType::Loom, sysroot);
            }
        }

        // 3. Install newly selected packages: unpack the archive into the
        // target root, then register the real result in LMDB. Channel index
        // manifests carry names and versions but not files or capability
        // hooks, so the archive manifest's hooks/triggers are adopted here --
        // otherwise e.g. an initramfs generator installed by a swap would be
        // invisible to the very trigger that has to run it.
        std::vector<package::PackageManifest> installed_now;
        for (const auto& selected : plan.packages_to_install) {
            const auto identity = package::package_identity(selected);

            // Exact version already installed and registered: keep its record,
            // file list and hooks; a swap only needs the new provider itself.
            auto existing = db.get_package(*wtxn, selected.name);
            if (!existing) return std::unexpected(existing.error());
            if (*existing && package::package_identity(**existing) == identity) continue;

            auto archive_res = repo::ensure_local_archive(plan.repos, identity);
            if (!archive_res) {
                return std::unexpected(archive_res.error());
            }
            auto inspect_res = archive::inspect_package(*archive_res);
            if (!inspect_res) {
                return std::unexpected(std::format(
                    "Invalid package archive for '{}': {}", selected.name, inspect_res.error()));
            }
            if (package::package_identity(inspect_res->manifest) != identity) {
                return std::unexpected(std::format(
                    "Archive identity does not match selected package '{}'", selected.name));
            }

            util::log_info("Unpacking {} -> {}...", selected.name, sysroot.string());
            auto ext_res = archive::extract_package(*archive_res, sysroot, &selected, &*inspect_res);
            if (!ext_res) {
                return std::unexpected(std::format(
                    "Failed to extract package '{}': {}", selected.name, ext_res.error()));
            }

            auto installed_pkg = selected;
            installed_pkg.files = ext_res->extracted_files;
            installed_pkg.capability_hooks = ext_res->manifest.capability_hooks;
            installed_pkg.triggers = ext_res->manifest.triggers;

            auto p_res = db.put_package(*wtxn, installed_pkg);
            if (!p_res) return std::unexpected(p_res.error());
            auto f_res = db.register_files(*wtxn, installed_pkg.name, installed_pkg.channel, installed_pkg.files);
            if (!f_res) return std::unexpected(f_res.error());
            auto prov_res = db.register_provides(*wtxn, installed_pkg.name, installed_pkg.provides);
            if (!prov_res) return std::unexpected(prov_res.error());
            installed_now.push_back(std::move(installed_pkg));
        }

        auto commit_res = wtxn->commit();
        if (!commit_res) return std::unexpected("Database commit failed: " + commit_res.error());

        // 4. Automatically re-generate native service configurations for all
        // installed daemons. A daemon is a package whose manifest carries a
        // universal service.toml; everything else owns no init scripts.
        auto installed = db.list_installed_packages();
        if (!installed) {
            return std::unexpected("Installed package database is inconsistent after reconcile: " + installed.error());
        }
        size_t gen_count = 0;
        for (const auto& pkg : *installed) {
            if (pkg.service_toml.empty()) continue;
            auto spec = service::ServiceSpec::parse_toml(pkg.service_toml);
            if (!spec) {
                if (plan.target_init == service::InitType::Loom) {
                    return std::unexpected(std::format(
                        "Invalid service for '{}': {}", pkg.name, spec.error()));
                }
                util::log_warn("Skipping service for '{}': {}", pkg.name, spec.error());
                continue;
            }
            // A package may ship its own native script for this init (the
            // systemd split packages keep their upstream units): package data
            // wins over the generated form.
            auto dest = service::service_destination(spec->name, plan.target_init, sysroot);
            if (!dest) {
                if (plan.target_init == service::InitType::Loom) {
                    return std::unexpected(std::format(
                        "Invalid Loom destination for '{}': {}", pkg.name, dest.error()));
                }
                util::log_warn("Skipping service for '{}': {}", pkg.name, dest.error());
                continue;
            }
            auto owners = db.get_path_owners(dest->string());
            if (owners && !owners->empty()) {
                util::log_info("  · {:<20} ships its own {} script", spec->name,
                    service::to_string(plan.target_init));
                continue;
            }
            auto gen_res = plan.target_init == service::InitType::Loom
                ? service::generate_loom_service(
                    pkg.service_toml, spec->name, sysroot)
                : service::generate_service(*spec, plan.target_init, sysroot);
            if (gen_res) {
                gen_count++;
            } else if (plan.target_init == service::InitType::Loom) {
                return std::unexpected(std::format(
                    "Cannot generate Loom service for '{}': {}", pkg.name, gen_res.error()));
            } else {
                util::log_warn("Cannot generate {} script for '{}': {}",
                    service::to_string(plan.target_init), pkg.name, gen_res.error());
            }
        }
        if (plan.target_init == service::InitType::Loom) {
            auto valid = service::validate_loom_services(sysroot);
            if (!valid) return std::unexpected(valid.error());
        }

        // 5. Execute post-transaction triggers
        triggers::TriggerContext trig_ctx;
        trig_ctx.sysroot = sysroot;
        trig_ctx.transaction_packages = std::move(installed_now);
        trig_ctx.installed_packages = std::move(*installed);
        trig_ctx.providers = providers;
        for (const auto& pkg : trig_ctx.transaction_packages) {
            trig_ctx.touched_files.insert(trig_ctx.touched_files.end(), pkg.files.begin(), pkg.files.end());
        }
        auto trigger_result = triggers::TriggerEngine::run(trig_ctx);
        if (!trigger_result) {
            return std::unexpected(
                "Post-transaction trigger failed: " + trigger_result.error());
        }

        util::log_success("Reconcile completed! Regenerated {} native service scripts for {}", 
            gen_count, service::to_string(plan.target_init));
        return {};
    }
};

} // namespace sage::rebuild
