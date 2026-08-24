export module sage.cli.pkg;

// State transitions on the target root: install, remove, declarative rebuild.
import std;
import sage;

import sage.cli;

namespace sage::cli {

// ============================================================================
// End-to-End `sage install <PKG...>` Implementation
// ============================================================================

export inline std::expected<std::optional<sage::package::PackageManifest>, std::string>
load_install_snapshot(
    sage::db::Database& db,
    auto& txn,
    std::string_view package_name,
    const std::optional<sage::package::PackageIdentity>& expected_identity)
{
    auto current = db.get_package(txn, package_name);
    if (!current) return std::unexpected(current.error());

    auto current_identity = *current
        ? std::optional{sage::package::package_identity(**current)}
        : std::nullopt;
    if (current_identity != expected_identity) {
        return std::unexpected(std::format(
            "Installed package '{}' changed after dependency resolution", package_name));
    }
    return current;
}

template <typename Result, typename Work>
std::expected<std::vector<Result>, std::string> collect_parallel(
    size_t count,
    int configured_jobs,
    Work&& work)
{
    if (count == 0) return std::vector<Result>{};
    const auto available = std::max(1U, std::thread::hardware_concurrency());
    const auto requested = configured_jobs > 0
        ? static_cast<unsigned>(configured_jobs)
        : available;
    const auto worker_count = std::min<size_t>(count, std::max(1U, requested));
    std::vector<std::optional<Result>> slots(count);
    std::vector<std::string> errors(count);
    std::atomic_size_t next{0};
    std::atomic_bool failed{false};
    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    for (size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&] {
            while (!failed.load(std::memory_order_relaxed)) {
                const auto index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= count) return;
                auto result = work(index);
                if (!result) {
                    errors[index] = std::move(result.error());
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
                slots[index] = std::move(*result);
            }
        });
    }
    workers.clear();
    if (failed.load(std::memory_order_relaxed)) {
        auto error = std::ranges::find_if(errors, [](const auto& value) {
            return !value.empty();
        });
        return std::unexpected(error == errors.end()
            ? "Parallel package operation failed"
            : std::move(*error));
    }
    std::vector<Result> results;
    results.reserve(count);
    for (auto& slot : slots) results.push_back(std::move(*slot));
    return results;
}

// Self-tests may execute trigger commands on the host while package payloads
// remain isolated under their temporary target root.
export int cmd_install(
    const CliOptions& opts,
    std::optional<std::filesystem::path> trigger_sysroot = std::nullopt,
    DatabaseSnapshot database_snapshot = DatabaseSnapshot::Unchecked)
{
    if (opts.args.empty()) {
        std::println("Usage: sage install <PKG...>");
        return 1;
    }
    if (opts.dry_run && database_snapshot == DatabaseSnapshot::Unchecked) {
        sage::util::log_error("Dry-run install requires a synchronized database snapshot");
        return 1;
    }

    auto cfg_res = sage::config::SystemConfig::load_from_root(opts.target_root);
    if (!cfg_res) {
        sage::util::log_error("Failed to load configuration: {}", cfg_res.error());
        return 1;
    }
    const auto& cfg = *cfg_res;

    sage::db::Database db;
    std::vector<sage::package::PackageManifest> installed_packages;
    if (!opts.dry_run || database_snapshot == DatabaseSnapshot::Present) {
        auto db_res = opts.dry_run
            ? sage::db::Database::open_existing_read_only_no_lock(cfg.db_path)
            : sage::db::Database::open(cfg.db_path);
        if (!db_res) {
            sage::util::log_error(
                "Failed to open database at {}: {}", cfg.db_path.string(), db_res.error());
            return 1;
        }
        db = std::move(*db_res);
        auto installed_res = db.list_installed_packages();
        if (!installed_res) {
            sage::util::log_error(
                "Installed package database is inconsistent: {}", installed_res.error());
            return 1;
        }
        installed_packages = std::move(*installed_res);
    }

    // 1. Gather Available Package Pool from Channels and Local Repos
    auto snapshot_res = sage::rebuild::fetch_repo_snapshot(
        cfg, opts.channel_filter, !opts.dry_run);
    if (!snapshot_res) {
        sage::util::log_error("{}", snapshot_res.error());
        return 1;
    }
    auto snapshot = std::move(*snapshot_res);
    auto& pool = snapshot.pool;
    auto& package_archive_map = snapshot.archives;

    // Include already-installed packages in the pool so that installed providers
    // (e.g. glibc providing so:libc.so.6) satisfy dependencies during bootstrap,
    // when the repository may not yet contain the providing package. Candidates
    // that are already installed are filtered out after solving.
    for (const auto& pkg : installed_packages) {
        pool.push_back(pkg);
    }

    // Check if any arguments are direct .pkg.tar.zst archive files
    std::vector<sage::package::Dependency> root_reqs;
    std::unordered_set<std::string> direct_package_names;
    for (const auto& arg : opts.args) {
        if (arg.ends_with(".pkg.tar.zst") && std::filesystem::exists(arg)) {
            auto inspect_res = sage::archive::inspect_package(arg);
            if (!inspect_res) {
                sage::util::log_error("Invalid package archive '{}': {}", arg, inspect_res.error());
                return 1;
            }
            if (!sage::package::package_architecture_matches(
                    inspect_res->manifest.arch, cfg.architecture)) {
                sage::util::log_error(
                    "Package archive '{}' targets architecture '{}', but this system targets '{}'",
                    arg, inspect_res->manifest.arch, cfg.architecture);
                return 1;
            }
            if (!direct_package_names.insert(inspect_res->manifest.name).second) {
                sage::util::log_error(
                    "Multiple direct archives were provided for package '{}'",
                    inspect_res->manifest.name);
                return 1;
            }
            std::erase_if(pool, [&](const auto& candidate) {
                return candidate.name == inspect_res->manifest.name;
            });
            pool.push_back(inspect_res->manifest);
            package_archive_map[sage::package::package_identity(inspect_res->manifest)] =
                std::filesystem::absolute(arg);
            root_reqs.push_back(sage::package::Dependency{
                .name = inspect_res->manifest.name,
                .op = sage::package::ConstraintOp::Equal,
                .version = inspect_res->manifest.version,
            });
        } else {
            root_reqs.push_back(sage::package::Dependency::parse(arg));
        }
    }

    // 2. PubGrub SAT Dependency Solver
    sage::solver::DependencySolver solver(pool, cfg.providers);
    auto solve_res = solver.solve(root_reqs);
    if (!solve_res) {
        sage::util::log_error("Dependency resolution failed:\n{}", solve_res.error());
        return 1;
    }

    std::vector<sage::package::PackageManifest> unique_to_install;
    std::unordered_set<std::string> seen_install_names;
    for (const auto& pkg : *solve_res) {
        if (seen_install_names.insert(pkg.name).second) {
            unique_to_install.push_back(pkg);
        }
    }

    // Filter out packages already installed at a satisfying version: the pool
    // includes installed packages purely as dependency providers, and the solver
    // may have selected them as candidates. Only repo-newer versions are installed
    // (upgrades); everything else is already satisfied by the current system.
    std::unordered_map<std::string, sage::package::PackageManifest> installed_by_name;
    for (auto& p : installed_packages) {
        installed_by_name.emplace(p.name, std::move(p));
    }
    std::vector<sage::package::PackageManifest> to_install;
    for (auto& pkg : unique_to_install) {
        auto it = installed_by_name.find(pkg.name);
        const bool exact_direct_request = direct_package_names.contains(pkg.name);
        if (!exact_direct_request
            && it != installed_by_name.end() && it->second.version >= pkg.version) {
            std::vector<std::string> db_files;
            if (it->second.files.empty()) {
                auto db_files_res = db.get_package_files(pkg.name);
                if (!db_files_res) {
                    sage::util::log_error(
                        "Failed to read files owned by '{}': {}", pkg.name, db_files_res.error());
                    return 1;
                }
                db_files = std::move(*db_files_res);
            }
            bool files_present = true;
            if (!it->second.files.empty()) {
                for (const auto& f : it->second.files) {
                    if (f.type != sage::package::FileType::Directory) {
                        if (!std::filesystem::exists(opts.target_root / f.path)) {
                            files_present = false;
                            break;
                        }
                    }
                }
            } else if (!db_files.empty()) {
                for (const auto& fpath : db_files) {
                    if (!std::filesystem::exists(opts.target_root / fpath)) {
                        files_present = false;
                        break;
                    }
                }
            } else {
                files_present = false;
            }
            if (files_present) {
                sage::util::log_info("  ~ {:<20} {:<15} [already installed]", pkg.name, pkg.version.to_string());
                continue;
            } else {
                sage::util::log_warn("  ! {:<20} {:<15} [files missing on disk, reinstalling]", pkg.name, pkg.version.to_string());
            }
        }
        to_install.push_back(std::move(pkg));
    }
    unique_to_install = std::move(to_install);

    // Enforce exclusive capabilities.
    //
    // An exclusive capability -- virtual/init, virtual/udev, virtual/libc --
    // is one where two providers on the same root cannot both work. Shared
    // capabilities are exempt by construction: two initramfs builders or two
    // kernels coexisting is the normal case, and rejecting them here is
    // exactly the mistake this distinction exists to prevent.
    {
        std::map<std::string, std::string> claimed;
        for (const auto& [name, pkg] : installed_by_name) {
            for (const auto& prov : pkg.provides) {
                if (cfg.is_exclusive_capability(prov)) claimed.emplace(prov, name);
            }
        }
        for (const auto& pkg : unique_to_install) {
            for (const auto& prov : pkg.provides) {
                if (!cfg.is_exclusive_capability(prov)) continue;
                auto [it, fresh] = claimed.emplace(prov, pkg.name);
                if (!fresh && it->second != pkg.name) {
                    sage::util::log_error("Exclusive capability '{}' would have two providers: '{}' and '{}'",
                        prov, it->second, pkg.name);
                    sage::util::log_error("Swap the provider through '{}' [providers] and 'sage rebuild', "
                        "or drop the capability to \"shared\" under [capabilities] if they really do coexist.",
                        cfg.system_config_path.string());
                    return 1;
                }
            }
        }
    }

    sage::util::log_info("Resolved {} packages to install into target root '{}':", unique_to_install.size(), opts.target_root.string());
    for (const auto& pkg : unique_to_install) {
        std::println("  + {:<20} {:<15} [{}]", pkg.name, pkg.version.to_string(), pkg.channel);
    }

    if (opts.dry_run) {
        sage::util::log_info("Dry-run preview completed successfully (no changes written).");
        return 0;
    }

    std::vector<std::filesystem::path> archive_paths;
    archive_paths.reserve(unique_to_install.size());
    for (const auto& pkg : unique_to_install) {
        auto archive = sage::rebuild::ensure_local_archive(
            snapshot, sage::package::package_identity(pkg));
        if (!archive) {
            sage::util::log_error("{}", archive.error());
            return 1;
        }
        archive_paths.push_back(std::move(*archive));
    }
    auto inspections = collect_parallel<sage::archive::InspectedPackage>(
        unique_to_install.size(), cfg.build.jobs, [&](size_t index)
            -> std::expected<sage::archive::InspectedPackage, std::string> {
            const auto& pkg = unique_to_install[index];
            auto inspected = sage::archive::inspect_package(archive_paths[index]);
            if (!inspected) {
                return std::unexpected(std::format(
                    "Invalid package archive for '{}': {}", pkg.name, inspected.error()));
            }
            if (sage::package::package_identity(inspected->manifest)
                != sage::package::package_identity(pkg)) {
                return std::unexpected(std::format(
                    "Package archive identity does not match selected package '{} {} [{}; {}]'",
                    pkg.name, pkg.version.to_string(), pkg.arch, pkg.channel));
            }
            return std::move(*inspected);
        });
    if (!inspections) {
        sage::util::log_error("{}", inspections.error());
        return 1;
    }
    std::map<sage::package::PackageIdentity, sage::archive::InspectedPackage> inspected_packages;
    for (size_t index = 0; index < unique_to_install.size(); ++index) {
        inspected_packages.emplace(
            sage::package::package_identity(unique_to_install[index]),
            std::move((*inspections)[index]));
    }

    // Self-heal: prune file registrations owned by packages that are no longer
    // installed (leftovers from previous versions with incomplete file lists),
    // so the paths can be claimed by the packages about to be installed.
    auto prune_txn = db.begin_write_txn();
    if (!prune_txn) {
        sage::util::log_error("Failed to open orphan-pruning transaction: {}", prune_txn.error());
        return 1;
    }
    auto pruned = db.prune_orphaned_files(*prune_txn);
    if (!pruned) {
        sage::util::log_error("Failed to prune orphaned file registrations: {}", pruned.error());
        return 1;
    }
    auto prune_commit = prune_txn->commit();
    if (!prune_commit) {
        sage::util::log_error("Failed to commit orphan-pruning transaction: {}", prune_commit.error());
        return 1;
    }
    if (*pruned > 0) {
        sage::util::log_info("  ~ pruned {} orphaned file registration(s)", *pruned);
    }

    std::vector<sage::channel::Channel> active_channels;
    for (const auto& ch_cfg : cfg.channels) {
        sage::channel::Channel ch;
        ch.name = ch_cfg.name;
        ch.scope = sage::channel::parse_scope(ch_cfg.scope);
        ch.enabled = ch_cfg.enabled;
        active_channels.push_back(std::move(ch));
    }
    std::vector<sage::package::FileEntry> all_touched_files;
    std::vector<sage::package::PackageManifest> committed_packages;
    bool any_package_committed = false;
    bool aggregate_done = false;

    auto run_package_postprocessing = [&](const sage::package::PackageManifest& installed_pkg,
                                           bool regenerate_profile) {
        auto spec = sage::channel::SubChannelSpec::parse(installed_pkg.channel);
        if (spec.scope == sage::channel::ChannelScope::Toolchain
            && !spec.category.empty() && !spec.slot.empty()) {
            auto act_res = sage::channel::ProfileManager::switch_active_toolchain(
                opts.target_root, spec.category, spec.slot);
            if (!act_res) {
                sage::util::log_warn(
                    "Failed to activate toolchain '{}:{}': {}",
                    spec.category, spec.slot, act_res.error());
            }
        }
        if (regenerate_profile) {
            (void)sage::channel::ProfileManager::regenerate_fhs_profile(
                opts.target_root, active_channels);
        }
    };

    auto run_aggregate_triggers = [&]() -> bool {
        sage::rebuild::TriggerContext trig_ctx;
        trig_ctx.sysroot = trigger_sysroot.value_or(opts.target_root);
        trig_ctx.touched_files = all_touched_files;
        trig_ctx.transaction_packages = committed_packages;
        auto current_packages = db.list_installed_packages();
        if (!current_packages) {
            sage::util::log_error(
                "Cannot run aggregate triggers: {}", current_packages.error());
            return false;
        }
        trig_ctx.installed_packages = std::move(*current_packages);
        trig_ctx.providers = cfg.providers;
        trig_ctx.dry_run = opts.dry_run;
        auto trigger_result = sage::rebuild::TriggerEngine::run(trig_ctx);
        if (!trigger_result) {
            sage::util::log_error(
                "Aggregate post-install trigger failed: {}", trigger_result.error());
            return false;
        }
        return true;
    };

    struct TriggerGuard {
        std::function<void()> action;
        ~TriggerGuard() {
            if (action) action();
        }
    } trigger_guard{[&] {
        if (any_package_committed && !aggregate_done) {
            (void)run_aggregate_triggers();
        }
    }};

    bool batch_has_payload_conflict = false;
    bool batch_has_unsafe_replacement = false;
    std::unordered_map<std::string, bool> batch_path_types;
    for (const auto& pkg : unique_to_install) {
        const auto& inspected = inspected_packages.at(
            sage::package::package_identity(pkg));
        for (const auto& file : inspected.data_files) {
            const auto path = sage::util::clean_rel_path(file.path);
            if (path == "usr/share/info/dir" || path.ends_with("/info/dir")) continue;
            const bool directory = file.type == sage::package::FileType::Directory;
            if (!directory) {
                std::error_code status_error;
                const auto existing = std::filesystem::symlink_status(
                    opts.target_root / path, status_error);
                if (!status_error
                    && std::filesystem::exists(existing)
                    && std::filesystem::is_directory(existing)) {
                    batch_has_unsafe_replacement = true;
                    break;
                }
            }
            auto [existing, inserted] = batch_path_types.emplace(path, directory);
            if (!inserted && (!directory || !existing->second)) {
                batch_has_payload_conflict = true;
                break;
            }
        }
        if (batch_has_payload_conflict || batch_has_unsafe_replacement) break;
    }

    // A fresh root has no upgrade/conffile cleanup ordering to preserve. Reserve
    // every ownership claim in one uncommitted LMDB transaction, extract
    // independent package payloads concurrently, flush the filesystem once,
    // then commit the registry. A crash can leave unregistered payload files,
    // but can never publish registry state ahead of durable files.
    if (installed_packages.empty() && unique_to_install.size() > 1
        && !batch_has_payload_conflict
        && !batch_has_unsafe_replacement) {
        auto batch_txn = db.begin_write_txn();
        if (!batch_txn) {
            sage::util::log_error(
                "Failed to open batch database transaction: {}", batch_txn.error());
            return 1;
        }
        std::vector<sage::package::PackageManifest> batch_manifests;
        batch_manifests.reserve(unique_to_install.size());
        for (const auto& pkg : unique_to_install) {
            const auto identity = sage::package::package_identity(pkg);
            const auto& inspected = inspected_packages.at(identity);
            auto installed_pkg = pkg;
            installed_pkg.files = inspected.data_files;
            installed_pkg.capability_hooks = inspected.manifest.capability_hooks;
            installed_pkg.triggers = inspected.manifest.triggers;
            installed_pkg.conffiles = inspected.manifest.conffiles;
            installed_pkg.service_toml = inspected.manifest.service_toml;
            auto files = db.register_files(
                *batch_txn, installed_pkg.name, installed_pkg.channel,
                installed_pkg.files);
            if (!files) {
                sage::util::log_error(
                    "Cannot reserve files for '{}': {}", pkg.name, files.error());
                return 1;
            }
            auto package = db.put_package(*batch_txn, installed_pkg);
            if (!package) {
                sage::util::log_error(
                    "Cannot stage package '{}': {}", pkg.name, package.error());
                return 1;
            }
            auto provides = db.register_provides(
                *batch_txn, installed_pkg.name, installed_pkg.provides);
            if (!provides) {
                sage::util::log_error(
                    "Cannot stage provides for '{}': {}", pkg.name, provides.error());
                return 1;
            }
            batch_manifests.push_back(std::move(installed_pkg));
        }

        auto extracted = collect_parallel<sage::archive::ExtractedPackage>(
            unique_to_install.size(), cfg.build.jobs, [&](size_t index)
                -> std::expected<sage::archive::ExtractedPackage, std::string> {
                const auto& pkg = unique_to_install[index];
                const auto& inspected = inspected_packages.at(
                    sage::package::package_identity(pkg));
                auto result = sage::archive::extract_package(
                    archive_paths[index], opts.target_root, &pkg, &inspected,
                    nullptr, sage::archive::ExtractionDurability::Batch);
                if (!result) {
                    return std::unexpected(std::format(
                        "Failed to extract package '{}': {}", pkg.name, result.error()));
                }
                return std::move(*result);
            });
        if (!extracted) {
            sage::util::log_error("{}", extracted.error());
            return 1;
        }
        auto durable = sage::archive::sync_extracted_root(opts.target_root);
        if (!durable) {
            sage::util::log_error("{}", durable.error());
            return 1;
        }
        for (size_t index = 0; index < batch_manifests.size(); ++index) {
            batch_manifests[index].files = std::move((*extracted)[index].extracted_files);
            auto package = db.put_package(*batch_txn, batch_manifests[index]);
            if (!package) {
                sage::util::log_error(
                    "Cannot finalize package '{}': {}",
                    batch_manifests[index].name, package.error());
                return 1;
            }
        }
        auto committed = batch_txn->commit();
        if (!committed) {
            sage::util::log_error(
                "Failed to commit package batch: {}", committed.error());
            return 1;
        }
        for (auto& installed_pkg : batch_manifests) {
            all_touched_files.insert(
                all_touched_files.end(), installed_pkg.files.begin(), installed_pkg.files.end());
            run_package_postprocessing(installed_pkg, false);
            committed_packages.push_back(std::move(installed_pkg));
        }
        any_package_committed = true;
        (void)sage::channel::ProfileManager::regenerate_fhs_profile(
            opts.target_root, active_channels);
        if (!run_aggregate_triggers()) {
            aggregate_done = true;
            return 1;
        }
        aggregate_done = true;
        sage::util::log_success(
            "Successfully installed {} packages into {}",
            unique_to_install.size(), opts.target_root.string());
        return 0;
    }

    // 3. Streaming Unpack & LMDB State Registration
    for (const auto& pkg : unique_to_install) {
        const auto identity = sage::package::package_identity(pkg);
        auto archive_res = sage::rebuild::ensure_local_archive(snapshot, identity);
        if (!archive_res) {
            sage::util::log_error("{}", archive_res.error());
            return 1;
        }
        auto inspected_it = inspected_packages.find(identity);
        auto package_txn = db.begin_write_txn();
        if (!package_txn) {
            sage::util::log_error("Failed to open database transaction for '{}': {}", pkg.name, package_txn.error());
            return 1;
        }

        auto expected_it = installed_by_name.find(pkg.name);
        auto expected_previous_identity = expected_it != installed_by_name.end()
            ? std::optional{sage::package::package_identity(expected_it->second)}
            : std::nullopt;
        auto previous_package = load_install_snapshot(
            db, *package_txn, pkg.name, expected_previous_identity);
        if (!previous_package) {
            sage::util::log_error(
                "Cannot install package '{}': {}", pkg.name, previous_package.error());
            return 1;
        }

        std::optional<std::string> previous_owner;
        std::vector<std::string> previous_paths;
        if (*previous_package) {
            previous_owner = std::format("{}:{}", pkg.name, (**previous_package).channel);
            auto previous_paths_res = db.get_package_files(*package_txn, pkg.name);
            if (!previous_paths_res) {
                sage::util::log_error(
                    "Failed to read previous files for '{}': {}",
                    pkg.name, previous_paths_res.error());
                return 1;
            }
            previous_paths = std::move(*previous_paths_res);
            for (const auto& path : previous_paths) {
                auto owners = db.get_path_owners(*package_txn, path);
                if (!owners) {
                    sage::util::log_error(
                        "Failed to read ownership for '{}': {}", path, owners.error());
                    return 1;
                }
                // Shared directories legitimately carry several owners, so the
                // guard is membership, not equality: only a concurrent takeover
                // of the whole claim must abort the migration.
                if (std::ranges::find(*owners, *previous_owner) == owners->end()) {
                    sage::util::log_error(
                        "Cannot migrate package '{}': file '{}' is owned by '{}' instead of '{}'",
                        pkg.name, path,
                        owners->empty() ? "<none>" : sage::util::join(*owners, ", "),
                        *previous_owner);
                    return 1;
                }
            }
        }

        auto conflict_res = db.check_file_conflicts(
            *package_txn,
            previous_owner
                ? std::optional<std::string_view>{*previous_owner}
                : std::nullopt,
            inspected_it->second.data_files);
        if (!conflict_res) {
            sage::util::log_error(
                "Cannot install package '{}': {}", pkg.name, conflict_res.error());
            return 1;
        }

        // Archives always contain paths relative to sysroot (e.g. usr/bin/bash or
        // opt/channels/gcc/15/bin/gcc), so always extract to the target root directly.
        sage::util::log_info("Unpacking {} -> {}...", pkg.name, opts.target_root.string());
        auto ext_res = sage::archive::extract_package(
            *archive_res, opts.target_root, &pkg, &inspected_it->second,
            *previous_package ? &**previous_package : nullptr);
        if (!ext_res) {
            sage::util::log_error("Failed to extract package '{}': {}", pkg.name, ext_res.error());
            return 1;
        }

        auto installed_pkg = pkg;
        installed_pkg.files = ext_res->extracted_files;

        // The channel index is only a solving summary. Archive-only metadata
        // must be adopted before this package record is persisted.
        installed_pkg.capability_hooks = ext_res->manifest.capability_hooks;
        installed_pkg.triggers = ext_res->manifest.triggers;
        // Conffile declarations likewise ride in the archive manifest, so the
        // stale-claim cleanup below can honor them even if the channel index
        // predates the declaration.
        installed_pkg.conffiles = ext_res->manifest.conffiles;
        installed_pkg.service_toml = ext_res->manifest.service_toml;
        auto package_touched_files = installed_pkg.files;

        // Reinstall/upgrade cleanup: release ownership of paths the new payload
        // dropped so they can transition to other packages (e.g. split -dev/-libs
        // children claim headers/libs the old monolithic version used to own).
        // Shared directories keep their remaining owners and stay on disk; sole
        // claims are physically removed here.
        std::vector<sage::package::FileEntry> stale_claims;
        size_t stale_removed = 0;
        if (*previous_package) {
            std::unordered_set<std::string> new_paths;
            for (const auto& f : installed_pkg.files) {
                new_paths.insert(sage::util::clean_rel_path(f.path));
            }
            std::unordered_map<std::string, sage::package::FileType> old_types;
            for (const auto& f : (**previous_package).files) {
                old_types.emplace(sage::util::clean_rel_path(f.path), f.type);
            }
            const auto& old_owner = *previous_owner;
            for (const auto& old_path : previous_paths) {
                // Compare in the canonicalized domain: a legacy claim spelled
                // usr/sbin/iconvconfig denotes the same physical file the new
                // payload installs as usr/bin/iconvconfig. Such claims are
                // released from the registry but their (freshly extracted)
                // files stay put.
                auto canonical_old = sage::archive::canonicalize_merge_claim(old_path);
                if (!canonical_old || new_paths.contains(*canonical_old)) {
                    sage::package::FileEntry fe;
                    fe.path = old_path;
                    fe.type = !canonical_old
                        || (old_types.contains(old_path)
                            && old_types.at(old_path) == sage::package::FileType::Directory)
                        ? sage::package::FileType::Directory
                        : sage::package::FileType::Regular;
                    stale_claims.push_back(fe);
                    package_touched_files.push_back(std::move(fe));
                    continue;
                }
                auto owners = db.get_path_owners(*package_txn, old_path);
                if (!owners) {
                    sage::util::log_error(
                        "Failed to read ownership for '{}': {}", old_path, owners.error());
                    return 1;
                }
                if (std::ranges::find(*owners, old_owner) == owners->end()) continue;
                // Only directories accumulate several owners, so a shared
                // claim is a directory by construction.
                const bool shared_directory = owners->size() > 1;
                // A directory another package has since filled must survive
                // the upgrade; only genuinely empty ones go away.
                const bool declared_directory =
                    old_types.contains(old_path)
                    && old_types.at(old_path) == sage::package::FileType::Directory;
                if (shared_directory) {
                    sage::util::log_info(
                        "  ~ released shared directory '{}' ({} owner(s) remain)",
                        old_path, owners->size() - 1);
                } else if (!installed_pkg.conffiles.empty()
                    && sage::archive::conffile_modified(
                        opts.target_root, old_path, installed_pkg.conffiles,
                        &**previous_package)) {
                    // The payload dropped this path, but it is a conffile the
                    // admin has since edited: release the claim, keep the file.
                    sage::util::log_info("  ~ keeping locally modified config '{}'", old_path);
                } else {
                    auto remove_res = sage::archive::remove_path_anchored(
                        opts.target_root, old_path, declared_directory);
                    if (!remove_res) {
                        sage::util::log_error(
                            "Failed to remove stale file '{}' for '{}': {}",
                            old_path, pkg.name, remove_res.error());
                        return 1;
                    }
                    ++stale_removed;
                }
                sage::package::FileEntry fe;
                fe.path = old_path;
                fe.type = shared_directory || declared_directory
                    ? sage::package::FileType::Directory
                    : sage::package::FileType::Regular;
                stale_claims.push_back(fe);
                package_touched_files.push_back(std::move(fe));
            }
            if (!stale_claims.empty()) {
                auto unregister_res = db.unregister_files(*package_txn, stale_claims, old_owner);
                if (!unregister_res) {
                    sage::util::log_error(
                        "Failed to unregister stale files for '{}': {}",
                        pkg.name, unregister_res.error());
                    return 1;
                }
                sage::util::log_info("  ~ removed {} stale file(s) from previous {} {}",
                    stale_removed, pkg.name, (**previous_package).version.to_string());
            }
        }

        auto p_res = db.put_package(*package_txn, installed_pkg);
        if (!p_res) {
            sage::util::log_error("Failed to register package '{}' in DB: {}", installed_pkg.name, p_res.error());
            return 1;
        }
        auto f_res = db.register_files(
            *package_txn,
            installed_pkg.name,
            installed_pkg.channel,
            installed_pkg.files,
            previous_owner
                ? std::optional<std::string_view>{*previous_owner}
                : std::nullopt);
        if (!f_res) {
            sage::util::log_error("Failed to register files for '{}': {}", installed_pkg.name, f_res.error());
            return 1;
        }
        auto prov_res = db.register_provides(*package_txn, installed_pkg.name, installed_pkg.provides);
        if (!prov_res) {
            sage::util::log_error("Failed to register provides for '{}': {}", installed_pkg.name, prov_res.error());
            return 1;
        }

        auto package_commit = package_txn->commit();
        if (!package_commit) {
            sage::util::log_error("Failed to commit package '{}': {}", installed_pkg.name, package_commit.error());
            return 1;
        }

        committed_packages.push_back(installed_pkg);
        all_touched_files.insert(
            all_touched_files.end(),
            package_touched_files.begin(), package_touched_files.end());
        any_package_committed = true;

        // Toolchain activation and profile generation are needed immediately:
        // a later package may depend on the aliases created for this commit.
        run_package_postprocessing(installed_pkg, true);
    }

    // Run triggers once after the complete install set. This happens after
    // toolchain activation so freshly written
    // /etc/ld.so.conf.d/sage-*.conf entries are picked up by ldconfig, and
    // after the DB commit so a capability installed in this very transaction
    // (mkinitcpio arriving alongside the kernel that needs it) is already
    // visible when the initramfs trigger looks for its provider.
    if (!run_aggregate_triggers()) {
        aggregate_done = true;
        return 1;
    }
    aggregate_done = true;

    sage::util::log_success("Successfully installed {} packages into {}", unique_to_install.size(), opts.target_root.string());
    return 0;
}
// ============================================================================
// End-to-End `sage remove <PKG...>` Implementation
// ============================================================================

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
    sage::rebuild::TriggerContext trig_ctx;
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
    auto trigger_result = sage::rebuild::TriggerEngine::run(trig_ctx);
    if (!trigger_result) {
        sage::util::log_error(
            "Post-remove trigger failed: {}", trigger_result.error());
        return 1;
    }

    sage::util::log_success("Successfully removed {} packages (including orphaned dependencies) from {}",
        to_remove_set.size(), opts.target_root.string());
    return 0;
}

export int cmd_rebuild(
    const CliOptions& opts,
    DatabaseSnapshot database_snapshot = DatabaseSnapshot::Unchecked)
{
    if (opts.dry_run && database_snapshot == DatabaseSnapshot::Unchecked) {
        sage::util::log_error("Dry-run rebuild requires a synchronized database snapshot");
        return 1;
    }

    auto cfg_res = sage::config::SystemConfig::load_from_root(opts.target_root);
    if (!cfg_res) {
        sage::util::log_error("Failed to load configuration: {}", cfg_res.error());
        return 1;
    }

    if (opts.dry_run && database_snapshot == DatabaseSnapshot::Absent) {
        sage::util::log_error(
            "Cannot calculate rebuild preview: package database '{}' is not initialized",
            cfg_res->db_path.string());
        return 1;
    }

    auto db_res = opts.dry_run
        ? sage::db::Database::open_existing_read_only_no_lock(cfg_res->db_path)
        : sage::db::Database::open(cfg_res->db_path);
    if (!db_res) {
        sage::util::log_error("Failed to open database: {}", db_res.error());
        return 1;
    }

    auto plan_res = sage::rebuild::ReconcileEngine::calculate_diff(
        *db_res, *cfg_res, !opts.dry_run);
    if (!plan_res) {
        sage::util::log_error("Failed to calculate reconcile plan: {}", plan_res.error());
        return 1;
    }

    auto exec_res = sage::rebuild::ReconcileEngine::execute(*db_res, *plan_res, opts.target_root, opts.dry_run, cfg_res->providers);
    if (!exec_res) {
        sage::util::log_error("Reconcile execution failed: {}", exec_res.error());
        return 1;
    }
    return 0;
}
} // namespace sage::cli
