export module sage.cli.install;

// End-to-end `sage install <PKG...>`: resolve, fetch, unpack, register.
import std;
import sage;

import sage.cli;
import sage.repo;

namespace sage::cli {

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
    // Plain size_t + __atomic_fetch_add: GCC's modules implementation loses
    // the always_inline body of std::atomic's fetch_add across a module
    // import, which fails instantiation of this template in test binaries.
    size_t next{0};
    std::atomic_bool failed{false};
    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    for (size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&] {
            while (!failed.load(std::memory_order_relaxed)) {
                const auto index = __atomic_fetch_add(
                    &next, static_cast<size_t>(1), __ATOMIC_RELAXED);
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

    // Recovery-first: finish any interrupted operation from a previous run
    // (publish committed plans, run their post-processing, retire their
    // transaction directories) before solving against the registry state.
    if (!opts.dry_run) {
        auto recovered = sage::rebuild::resume_pending_operations(db, opts.target_root);
        if (!recovered) {
            sage::util::log_error(
                "Failed to recover pending filesystem operations: {}", recovered.error());
            return 1;
        }
    }

    // 1. Gather Available Package Pool from Channels and Local Repos
    auto snapshot_res = sage::repo::fetch_repo_snapshot(
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
        auto archive = sage::repo::ensure_local_archive(
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

    // Staged transaction protocol (issue #9). State machine per operation:
    // no record -> `filesystem_pending` -> `postprocess_pending` -> no record.
    // The whole command is ONE operation sharing one on-disk transaction
    // directory: each package stages its payload there, appends its plan to
    // the accumulated journal, and enters the `filesystem_pending` phase by
    // committing registry changes + the record (journal sha256) in one LMDB
    // write transaction. Only the final package sets `final true` -- the
    // resume driver publishes every plan entry idempotently but runs the
    // expensive aggregate post-processing (toolchain switches, FHS profile,
    // triggers) once, for `final` journals only. A crash anywhere leaves the
    // committed prefix in a consistent registry state whose publication is
    // completed by the next command's entry-point resume; uncommitted
    // staging is RAII-discarded or orphan-GCed.
    auto transaction_res =
        sage::archive::FilesystemTransaction::create(opts.target_root);
    if (!transaction_res) {
        sage::util::log_error(
            "Failed to create filesystem transaction: {}", transaction_res.error());
        return 1;
    }
    auto& filesystem_transaction = *transaction_res;

    sage::archive::JournalContext journal_ctx;
    journal_ctx.kind = "install";
    journal_ctx.sysroot = trigger_sysroot.value_or(opts.target_root).string();
    journal_ctx.regenerate_profile = true;
    bool any_package_committed = false;
    // Failure path. The batch is aborted for good, but a committed prefix
    // must not stay stranded at `filesystem_pending`: the journal is
    // rewritten with `final true` (record hash refreshed in a short write
    // transaction) and the resume driver takes the committed prefix to its
    // terminal state -- publish, post-process, retire -- mirroring what the
    // old scope-exit trigger guard guaranteed. Declared BEFORE the rewind
    // guard so destruction rewinds a torn journal first and finalization
    // then validates against evidence matching the committed record hash.
    struct FinalizePrefixGuard {
        sage::db::Database* db;
        const std::filesystem::path* target_root;
        sage::archive::FilesystemTransaction* transaction;
        sage::archive::JournalContext* journal;
        const bool* committed;
        bool disarmed{false};
        ~FinalizePrefixGuard() {
            if (disarmed || !*committed) return;
            journal->final = true;
            const auto text =
                sage::archive::render_journal(*journal, transaction->journal_entries());
            auto hash = transaction->persist_journal(text);
            if (!hash) {
                sage::util::log_warn(
                    "Could not finalize journal after failed install: {}", hash.error());
                return;
            }
            auto txn = db->begin_write_txn();
            if (!txn) {
                sage::util::log_warn(
                    "Could not refresh operation record after failed install: {}",
                    txn.error());
                return;
            }
            auto existing = db->get_operation(*txn, transaction->id());
            if (!existing || !*existing) {
                sage::util::log_warn("Operation record vanished during abort handling");
                return;
            }
            (**existing).journal_sha256 = *hash;
            if (auto put = db->put_operation(*txn, **existing); !put) {
                sage::util::log_warn(
                    "Could not refresh operation record after failed install: {}",
                    put.error());
                return;
            }
            if (auto commit = txn->commit(); !commit) {
                sage::util::log_warn(
                    "Could not commit operation record refresh: {}", commit.error());
                return;
            }
            if (auto resumed = sage::rebuild::resume_pending_operations(*db, *target_root);
                !resumed) {
                sage::util::log_warn(
                    "Committed packages stay pending after failed install: {}",
                    resumed.error());
            }
        }
    } finalize_prefix_guard{
        &db, &opts.target_root, &filesystem_transaction, &journal_ctx,
        &any_package_committed};

    // Journal text matching the currently COMMITTED operation record. The
    // window between persist_journal(N) and the LMDB commit of package N is
    // a clean-error hazard: the journal would describe one package more than
    // the record's sha256 attests, and resume verifies hash-vs-record before
    // publishing, so recovery would refuse to run. Arming this guard across
    // exactly that window rewinds the journal file on error return. A hard
    // crash inside the window remains possible (contract-accepted); it only
    // costs an explicit abandon, never silent corruption.
    std::string committed_journal_text;
    struct JournalRewindGuard {
        sage::archive::FilesystemTransaction* transaction;
        const std::string* text;
        bool armed{false};
        ~JournalRewindGuard() {
            if (armed) (void)transaction->persist_journal(*text);
        }
    } journal_rewind{&filesystem_transaction, &committed_journal_text};

    const auto t_phase0 = std::chrono::steady_clock::now();

    // Projected-ownership conflict detection. A file moving between two
    // packages inside this transaction -- the classic monolithic -> split
    // upgrade (foo -> foo + foo-libs + foo-dev) -- must not be judged
    // against the pre-transaction database: when foo-libs is checked, the
    // library is still registered to the old monolithic foo. Record every
    // path each upgrading package gives up (its registered files minus what
    // its new payload still ships, compared after usr/sbin-style merge
    // canonicalization exactly like the stale-claim cleanup below) so the
    // ownership checks tolerate precisely those in-transaction handovers.
    sage::db::ReleasedClaims batch_released_claims;
    for (const auto& pkg : unique_to_install) {
        if (!installed_by_name.contains(pkg.name)) continue;
        std::unordered_set<std::string> kept_paths;
        for (const auto& f :
            inspected_packages.at(sage::package::package_identity(pkg)).data_files) {
            kept_paths.insert(sage::util::clean_rel_path(f.path));
        }
        auto previous_paths = db.get_package_files(pkg.name);
        if (!previous_paths) {
            sage::util::log_error(
                "Failed to read previous files for '{}': {}",
                pkg.name, previous_paths.error());
            return 1;
        }
        for (const auto& old_path : *previous_paths) {
            auto canonical_old = sage::archive::canonicalize_merge_claim(old_path);
            if (!canonical_old || kept_paths.contains(*canonical_old)) continue;
            batch_released_claims[sage::util::clean_rel_path(old_path)].insert(pkg.name);
        }
    }
    // 3. Staged unpack, journaled publication plan, per-package registry commit
    for (auto pkg_it = unique_to_install.begin();
         pkg_it != unique_to_install.end(); ++pkg_it) {
        auto& pkg = *pkg_it;
        const bool last_package = std::next(pkg_it) == unique_to_install.end();
        const auto identity = sage::package::package_identity(pkg);
        auto archive_res = sage::repo::ensure_local_archive(snapshot, identity);
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
            inspected_it->second.data_files,
            batch_released_claims);
        if (!conflict_res) {
            sage::util::log_error(
                "Cannot install package '{}': {}", pkg.name, conflict_res.error());
            return 1;
        }

        // Stage the payload inside the transaction directory; the live tree
        // is only ever touched later, by the idempotent publish pass.
        sage::util::log_info(
            "Staging {} -> {}...", pkg.name, filesystem_transaction.relative_dir());
        const auto staged_root = opts.target_root
            / std::filesystem::path(filesystem_transaction.relative_dir())
            / "staged";
        auto ext_res = sage::archive::extract_package(
            *archive_res, staged_root, &pkg, &inspected_it->second);
        if (!ext_res) {
            sage::util::log_error("Failed to extract package '{}': {}", pkg.name, ext_res.error());
            return 1;
        }

        auto installed_pkg = pkg;
        installed_pkg.files = ext_res->extracted_files;

        // The channel index is a solving summary: it carries names, versions,
        // dependencies and provides, but not capability hooks or triggers.
        // Those live in the archive's own manifest, and the post-transaction
        // trigger pass reads them back out of the database -- so adopt them
        // here, or an installed initramfs generator would be invisible to the
        // very trigger that has to run it.
        installed_pkg.capability_hooks = ext_res->manifest.capability_hooks;
        installed_pkg.triggers = ext_res->manifest.triggers;
        // Conffile declarations likewise ride in the archive manifest, so the
        // stale-claim cleanup below can honor them even if the channel index
        // predates the declaration.
        installed_pkg.conffiles = ext_res->manifest.conffiles;
        // And the universal service definition, for the same reason: the
        // reconcile pass renders init scripts from what the database holds,
        // so a daemon whose service.toml never reaches LMDB is a daemon no
        // init system will ever be told about.
        installed_pkg.service_toml = ext_res->manifest.service_toml;

        // Conffile protection under staging: extraction cannot compare against
        // the live tree (its fifth parameter would probe the empty staging
        // root), so redirect locally modified configs to "<path>.new" here,
        // exactly as extract_package used to do inline against the live tree.
        if (*previous_package && !installed_pkg.conffiles.empty()) {
            for (auto& file_entry : installed_pkg.files) {
                if (file_entry.type == sage::package::FileType::Directory) continue;
                const auto key = sage::util::clean_rel_path(file_entry.path);
                const bool declared = std::ranges::find(
                    installed_pkg.conffiles, key,
                    [](const std::string& candidate) {
                        return sage::util::clean_rel_path(candidate);
                    }) != installed_pkg.conffiles.end();
                if (!declared
                    || !sage::archive::conffile_modified(
                        opts.target_root, key, installed_pkg.conffiles,
                        &**previous_package)) {
                    continue;
                }
                std::error_code rename_ec;
                std::filesystem::rename(
                    staged_root / std::filesystem::path(key),
                    staged_root / std::filesystem::path(key + ".new"), rename_ec);
                if (rename_ec) {
                    sage::util::log_error(
                        "Failed to protect modified config '{}' for '{}': {}",
                        key, pkg.name, rename_ec.message());
                    return 1;
                }
                file_entry.path = key + ".new";
                sage::util::log_warn(
                    "Protecting modified config '{}': new version saved as '{}.new'",
                    key, key);
            }
        }
        auto package_touched_files = installed_pkg.files;

        // Publication plan: ancestors first (the publisher resolves target
        // parents strictly), then payloads with their real final modes.
        std::set<std::string> ensured_dirs;
        for (const auto& file_entry : installed_pkg.files) {
            const auto rel = sage::util::clean_rel_path(file_entry.path);
            if (file_entry.type == sage::package::FileType::Directory) {
                ensured_dirs.insert(std::string(rel));
                continue;
            }
            for (auto ancestor = std::filesystem::path(rel).parent_path();
                 !ancestor.empty() && ancestor != ".";
                 ancestor = ancestor.parent_path()) {
                ensured_dirs.insert(ancestor.generic_string());
            }
        }

        // Prepare-time compatibility against the LIVE tree: staging never
        // touches it, so clashes that extraction used to hit physically must
        // be caught before this package's entries join the shared journal and
        // its registry commit -- a rejected package leaves no trace in
        // database or journal. A payload file/symlink may replace another
        // file or symlink but never an existing directory; an ensure-dir
        // target must not be an existing non-directory.
        for (const auto& dir : ensured_dirs) {
            std::error_code status_ec;
            const auto existing = std::filesystem::symlink_status(
                opts.target_root / std::filesystem::path(dir), status_ec);
            if (!status_ec && std::filesystem::exists(existing)
                && !std::filesystem::is_directory(existing)) {
                sage::util::log_error(
                    "Cannot install package '{}': directory '{}' collides with an existing non-directory",
                    pkg.name, dir);
                return 1;
            }
        }
        for (const auto& file_entry : installed_pkg.files) {
            if (file_entry.type == sage::package::FileType::Directory) continue;
            const auto rel = sage::util::clean_rel_path(file_entry.path);
            std::error_code status_ec;
            const auto existing = std::filesystem::symlink_status(
                opts.target_root / std::filesystem::path(rel), status_ec);
            if (!status_ec && std::filesystem::is_directory(existing)) {
                sage::util::log_error(
                    "Cannot install package '{}': payload '{}' would replace an existing directory",
                    pkg.name, rel);
                return 1;
            }
        }
        for (const auto& dir : ensured_dirs)
            filesystem_transaction.plan_ensure_dir(dir);
        for (const auto& file_entry : installed_pkg.files) {
            const auto rel = sage::util::clean_rel_path(file_entry.path);
            switch (file_entry.type) {
            case sage::package::FileType::Regular:
                filesystem_transaction.plan_put_file(
                    rel, std::format("staged/{}", rel), file_entry.mode);
                break;
            case sage::package::FileType::Symlink:
                filesystem_transaction.plan_put_symlink(
                    rel, std::format("staged/{}", rel));
                break;
            case sage::package::FileType::Directory:
                break;
            }
        }

        // Reinstall/upgrade cleanup: release ownership of paths the new payload
        // dropped so they can transition to other packages (e.g. split -dev/-libs
        // children claim headers/libs the old monolithic version used to own).
        // Shared directories keep their remaining owners and stay on disk; sole
        // claims become journaled removals executed by the publish pass.
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
                // released from the registry but their files stay put.
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
                    // Physical removal becomes a journaled R F/R D entry for
                    // the publish pass. Alias-spelled claims (usr/sbin vs
                    // usr/bin) denote the merged domain: release the registry
                    // claim but leave the physical file alone, exactly like
                    // the old remove_path_anchored alias skip.
                    const auto clean_old = sage::util::clean_rel_path(old_path);
                    if (*canonical_old == clean_old) {
                        std::error_code status_ec;
                        const auto live = std::filesystem::symlink_status(
                            opts.target_root / std::filesystem::path(clean_old),
                            status_ec);
                        const bool live_directory =
                            !status_ec && std::filesystem::is_directory(live);
                        if (live_directory || declared_directory) {
                            filesystem_transaction.plan_remove_dir(clean_old);
                        } else {
                            filesystem_transaction.plan_remove_file(clean_old);
                        }
                        ++stale_removed;
                    }
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

        // Accumulate the journal context for this package: touched files,
        // committed manifest, and toolchain activations (the switch itself is
        for (const auto& fe : package_touched_files) {
            journal_ctx.touched.emplace_back(
                fe.type == sage::package::FileType::Directory ? 'D'
                : fe.type == sage::package::FileType::Symlink ? 'L' : 'F',
                fe.path);
        }
        journal_ctx.package_manifests_toml.push_back(installed_pkg.serialize_toml());
        {
            auto spec = sage::channel::SubChannelSpec::parse(installed_pkg.channel);
            if (spec.scope == sage::channel::ChannelScope::Toolchain
                && !spec.category.empty() && !spec.slot.empty()) {
                journal_ctx.toolchain_activations.push_back(
                    std::format("{}:{}", spec.category, spec.slot));
            }
        }
        if (last_package) journal_ctx.final = true;

        // Evidence before commitment: the durable journal (payload already
        // fsynced by sync_staging below) must exist before the operation
        // record becomes visible in LMDB.
        if (auto synced = filesystem_transaction.sync_staging(); !synced) {
            sage::util::log_error(
                "Failed to sync staged payload for '{}': {}", pkg.name, synced.error());
            return 1;
        }
        const auto journal_text = sage::archive::render_journal(
            journal_ctx, filesystem_transaction.journal_entries());
        auto journal_hash = filesystem_transaction.persist_journal(journal_text);
        if (!journal_hash) {
            sage::util::log_error(
                "Failed to persist journal for '{}': {}", pkg.name, journal_hash.error());
            return 1;
        }
        journal_rewind.armed = true;

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
                : std::nullopt,
            batch_released_claims);
        if (!f_res) {
            sage::util::log_error("Failed to register files for '{}': {}", installed_pkg.name, f_res.error());
            return 1;
        }
        auto prov_res = db.register_provides(*package_txn, installed_pkg.name, installed_pkg.provides);
        if (!prov_res) {
            sage::util::log_error("Failed to register provides for '{}': {}", installed_pkg.name, prov_res.error());
            return 1;
        }

        sage::db::FilesystemOperationRecord operation;
        operation.id = filesystem_transaction.id();
        operation.kind = "install";
        operation.phase = std::string(sage::db::phase_filesystem_pending);
        operation.transaction_dir = filesystem_transaction.relative_dir();
        operation.journal_sha256 = *journal_hash;
        auto op_res = db.put_operation(*package_txn, operation);
        if (!op_res) {
            sage::util::log_error(
                "Failed to record operation for '{}': {}", installed_pkg.name, op_res.error());
            return 1;
        }

        auto package_commit = package_txn->commit();
        if (!package_commit) {
            sage::util::log_error("Failed to commit package '{}': {}", installed_pkg.name, package_commit.error());
            return 1;
        }
        journal_rewind.armed = false;
        committed_journal_text = journal_text;
        any_package_committed = true;

        std::println("[timing] staged '{}': {:.1f}s",
            installed_pkg.name,
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t_phase0).count());
    }

    // Publish every staged plan and run the aggregate post-install triggers
    // through the shared recovery/resume driver: toolchain switches, FHS
    // profile regeneration, triggers, record deletion, txn retirement.
    // Normal path: the explicit resume below supersedes the failure guard.
    finalize_prefix_guard.disarmed = true;
    const auto t_resume = std::chrono::steady_clock::now();
    auto resumed = sage::rebuild::resume_pending_operations(db, opts.target_root);
    if (!resumed) {
        std::println("[timing] publish+postprocess (failed): {:.1f}s",
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t_resume).count());
        sage::util::log_error(
            "Failed to complete pending filesystem operations: {}", resumed.error());
        return 1;
    }
    std::println("[timing] publish+postprocess: {:.1f}s",
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t_resume).count());

    sage::util::log_success("Successfully installed {} packages into {}", unique_to_install.size(), opts.target_root.string());
    return 0;
}

} // namespace sage::cli
