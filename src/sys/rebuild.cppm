module;
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

export module sage.rebuild;

// Declarative reconcile engine: diff /etc/sage/system.toml against LMDB
// state, plan provider swaps, and commit guarded transitions. Since issue #9
// every disk mutation rides the durable transaction protocol: payloads are
// staged inside a filesystem transaction, described by a journalled plan,
// and committed together with an operation record in one LMDB write
// transaction. Publication and post-processing are driven afterwards by
// resume_pending_operations() -- the very function every mutating command
// calls on entry, so recovery and the happy path share one driver.

import std;
import sage.archive;
import sage.channel;
import sage.config;
import sage.package;
import sage.repo;
import sage.service;
import sage.service_registry;
import sage.db;
import sage.solver;
import sage.triggers;
import sage.util;
import sage.vendor.lmdb;

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
    service::InitType target_init{service::InitType::Systemd};
    bool has_changes{false};
};

struct RecoveryOutcome {
    std::size_t finalized{0};
};

// Module-internal helpers. They live in an unexported namespace because the
// enclosing export block would otherwise try to export them (and `static`
// inside an export block is ill-formed).
namespace detail {

std::expected<std::string, std::string> digest_text(std::string_view text) {
    util::Sha256 hasher;
    hasher.update(text.data(), text.size());
    return hasher.finalize();
}


// Retire (anchored rm -rf) a transaction directory. Used by orphan GC,
// abandon, and the terminal step of a successful operation.
std::expected<void, std::string> retire_transaction(
    const std::filesystem::path& target_root, std::string_view relative_dir)
{
    auto attached = archive::FilesystemTransaction::attach(target_root, relative_dir);
    if (!attached) return std::unexpected(attached.error());
    return attached->discard();
}

// Post-processing half of an operation: replay the toolchain activations and
// profile regeneration recorded in the journal context, then rebuild the
// trigger context and run the trigger engine. Any failure leaves the
// operation at postprocess_pending for the next attempt.
//
// Two different roots are in play. Toolchain/profile state physically lives
// under the target root (opt/channels, etc/sage/profiles), so those steps use
// target_root. The journal's `sysroot` field is the chroot view the package
// was installed with and belongs to the trigger context only -- commands like
// `sage install --root /mnt --trigger-sysroot /` legitimately differ.
std::expected<void, std::string> run_postprocess(
    db::Database& db,
    const std::filesystem::path& target_root,
    const archive::ParsedJournal& parsed)
{
    const auto cfg = config::SystemConfig::load_from_root(target_root);
    if (!cfg) return std::unexpected(cfg.error());

    for (const auto& activation : parsed.ctx.toolchain_activations) {
        const auto colon = activation.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 == activation.size()) {
            return std::unexpected(std::format(
                "Malformed toolchain_activate entry '{}' in journal", activation));
        }
        auto switched = channel::ProfileManager::switch_active_toolchain(
            target_root, activation.substr(0, colon), activation.substr(colon + 1));
        if (!switched) return std::unexpected(switched.error());
    }
    if (parsed.ctx.regenerate_profile) {
        std::vector<channel::Channel> active_channels;
        active_channels.reserve(cfg->channels.size());
        for (const auto& ch_cfg : cfg->channels) {
            channel::Channel ch;
            ch.name = ch_cfg.name;
            ch.scope = channel::parse_scope(ch_cfg.scope);
            ch.enabled = ch_cfg.enabled;
            active_channels.push_back(std::move(ch));
        }
        auto regen = channel::ProfileManager::regenerate_fhs_profile(
            target_root, active_channels);
        if (!regen) return std::unexpected(regen.error());
    }

    const auto under = [](std::string_view path) {
        return path.starts_with("usr/lib/systemd/system/")
            || path.starts_with("etc/systemd/system/");
    };
    const bool systemd_unit_tree_changed = std::ranges::any_of(
        parsed.ctx.touched, [&](const auto& pair) { return under(pair.second); });
    if (systemd_unit_tree_changed && target_root == "/"
        && std::filesystem::is_directory("/run/systemd/system")) {
        const int status = std::system("/usr/bin/systemctl daemon-reload");
        if (status != 0) {
            return std::unexpected(std::format("systemctl daemon-reload failed with status {}", status));
        }
    }

    const std::filesystem::path trigger_sysroot = parsed.ctx.sysroot.empty()
        ? target_root : std::filesystem::path(parsed.ctx.sysroot);
    triggers::TriggerContext trig_ctx;
    trig_ctx.sysroot = trigger_sysroot;
    for (const auto& [marker, rel] : parsed.ctx.touched) {
        package::FileEntry entry;
        entry.path = rel;
        entry.type = package::parse_file_type(std::string_view(&marker, 1));
        trig_ctx.touched_files.push_back(std::move(entry));
    }
    for (const auto& block : parsed.ctx.package_manifests_toml) {
        auto manifest = package::PackageManifest::parse_toml(block);
        if (!manifest) return std::unexpected(std::format("Manifest block in journal failed to parse: {}", manifest.error()));
        trig_ctx.transaction_packages.push_back(std::move(*manifest));
    }
    auto installed = db.list_installed_package_summaries();
    if (!installed) return std::unexpected(installed.error());
    trig_ctx.installed_packages = std::move(*installed);
    trig_ctx.providers = cfg->providers;

    auto result = triggers::TriggerEngine::run(trig_ctx);
    if (!result) return std::unexpected("Post-transaction trigger failed during recovery: " + result.error());
    return {};
}

std::expected<void, std::string> stage_text(
    archive::FilesystemTransaction& fsx,
    std::string_view stage_rel,
    std::string_view content,
    std::uint32_t mode)
{
    auto fd = fsx.open_staged_file(stage_rel, mode);
    if (!fd) return std::unexpected(fd.error());
    size_t written = 0;
    while (written < content.size()) {
        const auto n = ::write(*fd, content.data() + written, content.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            const std::string reason = std::strerror(errno);
            ::close(*fd);
            return std::unexpected(std::format(
                "Cannot stage '{}': {}", stage_rel, reason));
        }
        written += static_cast<size_t>(n);
    }
    ::close(*fd);
    return {};
}
} // namespace detail

using namespace detail; // unexported helpers, called from the API below

// Read-only peek at the pending-operation table for status/query surfaces.
// Never attempts recovery.
std::expected<std::vector<db::FilesystemOperationRecord>, std::string>
pending_operations(db::Database& db)
{
    auto txn = db.begin_read_txn();
    if (!txn) return std::unexpected(std::string("Failed to open database read transaction"));
    return db.list_operations(*txn);
}

// Push every pending operation to its terminal state. Called at the entry of
// each mutating command (inside the writer lock, before solving) and again on
// the normal path right after the metadata commit -- recovery and the happy
// path are literally the same code, so a crashed transaction resumes exactly
// as an in-flight one would have continued.
//
// Per-operation state machine (record phase -> work -> next phase):
//   filesystem_pending  -> publish(journal)   -> postprocess_pending
//   postprocess_pending -> triggers/profiles  -> record deleted, dir retired
// Every step is idempotent or guarded, so any failure simply leaves the
// record at its current phase with all evidence intact; the next invocation
// retries it. abandon_id is the only destructive escape hatch.
std::expected<RecoveryOutcome, std::string> resume_pending_operations(
    db::Database& db,
    const std::filesystem::path& target_root,
    std::optional<std::string_view> abandon_id = std::nullopt)
{
    RecoveryOutcome outcome;

    // Orphan GC: transaction directories without an LMDB record are pre-commit
    // debris; anything recorded is evidence and must never be touched here.
    const auto dirs = archive::list_transaction_dirs(target_root);
    if (!dirs.empty()) {
        auto records = db.list_operations();
        if (!records) {
            return std::unexpected("Failed to list operations for orphan GC: " + records.error());
        }
        for (const auto& leaf : dirs) {
            // list_transaction_dirs yields bare leaves; records and attach()
            // speak full target-root-relative paths.
            const std::string dir = std::format("var/lib/sage/transactions/{}", leaf);
            const bool owned = std::ranges::any_of(*records,
                [&](const auto& r) { return r.transaction_dir == dir; });
            if (owned) continue;
            if (auto retired = retire_transaction(target_root, dir); !retired) {
                util::log_warn("Could not discard orphaned transaction directory '{}': {}",
                    dir, retired.error());
            } else {
                util::log_info("Discarded orphaned transaction directory '{}'", dir);
            }
        }
    }

    // Explicit abandon: the only escape hatch short of fixing the underlying
    // failure. Delete the record, retire the directory, and shout.
    if (abandon_id) {
        auto records = db.list_operations();
        if (!records) {
            return std::unexpected("Failed to list operations for abandon: " + records.error());
        }
        const auto it = std::ranges::find_if(*records,
            [&](const auto& r) { return r.id == *abandon_id; });
        if (it == records->end()) {
            util::log_warn("Abandon requested for '{}' but no pending operation carries that id",
                *abandon_id);
        } else {
            auto wtxn = db.begin_write_txn();
            if (!wtxn) {
                return std::unexpected(std::string(
                    "Failed to open database write transaction for abandon"));
            }
            auto del = db.delete_operation(*wtxn, *abandon_id);
            if (!del) return std::unexpected(del.error());
            auto commit = wtxn->commit();
            if (!commit) return std::unexpected("Database commit failed: " + commit.error());
            if (auto retired = retire_transaction(target_root, it->transaction_dir); !retired) {
                util::log_warn("Abandoned operation '{}' but could not retire '{}': {}",
                    *abandon_id, it->transaction_dir, retired.error());
            }
            util::log_warn(
                "Operation '{}' ({}) was explicitly ABANDONED: record deleted and "
                "transaction directory '{}' retired. The target root may now be in an "
                "inconsistent state -- files may be partially published or post-processing "
                "(ldconfig, triggers, profiles) skipped. Run 'sage rebuild' to re-reconcile.",
                it->id, it->kind, it->transaction_dir);
        }
    }

    auto records = db.list_operations();
    if (!records) return std::unexpected(records.error());

    for (const auto& record : *records) {
        if (record.phase != db::phase_filesystem_pending
            && record.phase != db::phase_postprocess_pending) {
            return std::unexpected(std::format(
                "Operation '{}' carries unknown phase '{}'", record.id, record.phase));
        }

        // Evidence integrity gate: both phases verify the journal against the
        // digest recorded at commit time before acting on it.
        auto journal = archive::FilesystemTransaction::read_journal(
            target_root, record.transaction_dir);
        if (!journal) {
            return std::unexpected(std::format(
                "Operation '{}': cannot read journal: {}", record.id, journal.error()));
        }
        auto digest = digest_text(*journal);
        if (!digest) return std::unexpected(digest.error());
        if (*digest != record.journal_sha256) {
            return std::unexpected(std::format(
                "Operation '{}': journal digest mismatch (recorded {}, computed {}) -- "
                "evidence corrupted; refusing to proceed",
                record.id, record.journal_sha256, *digest));
        }
        auto parsed = archive::parse_journal(*journal);
        if (!parsed) {
            return std::unexpected(std::format(
                "Operation '{}': journal failed to parse: {}", record.id, parsed.error()));
        }

        if (record.phase == db::phase_filesystem_pending) {
            auto txn = archive::FilesystemTransaction::attach(
                target_root, record.transaction_dir);
            if (!txn) {
                return std::unexpected(std::format(
                    "Operation '{}': cannot attach transaction directory: {}",
                    record.id, txn.error()));
            }
            auto published = txn->publish(*journal);
            if (!published) {
                return std::unexpected(std::format(
                    "Operation '{}': publishing journal failed: {}",
                    record.id, published.error()));
            }

            // Crash-boundary invariant: the phase is promoted only AFTER the
            // files are on disk. Post-processing (ldconfig, triggers) must
            // never observe a half-published root, and re-publishing an
            // already-published journal is a no-op by per-entry idempotency
            // (same-size target skip; ENOENT/ENOTEMPTY removal tolerance), so
            // a crash anywhere in here replays safely.
            //
            // Batch intermediate state: `final false` marks every commit of a
            // batch install except the last. Post-processing belongs to the
            // batch as a whole, so such records deliberately stay at
            // filesystem_pending until the batch coordinator rewrites their
            // journal with `final true`; publication above still ran because
            // it is idempotent.
            if (!parsed->ctx.final) continue;


            // Short write txn: only the phase flips here. The journal hash
            // and transaction dir were frozen at the original commit and must
            // survive untouched -- they are the recovery evidence.
            auto wtxn = db.begin_write_txn();
            if (!wtxn) {
                return std::unexpected(std::string(
                    "Failed to open database write transaction"));
            }
            auto promoted = db.update_operation_phase(
                *wtxn, record.id, db::phase_postprocess_pending);
            if (!promoted) return std::unexpected(promoted.error());
            auto commit = wtxn->commit();
            if (!commit) return std::unexpected("Database commit failed: " + commit.error());
        }

        auto post = run_postprocess(db, target_root, *parsed);
        if (!post) {
            return std::unexpected(std::format(
                "Operation '{}': {}", record.id, post.error()));
        }

        // Terminal step. Ordering matters: the record is deleted BEFORE the
        // directory is retired. If we crash in between, the directory is
        // orphaned (harmless: entry GC reclaims it) but no live record ever
        // points at missing evidence. The reverse order would leave a record
        // whose journal has vanished -- an unrecoverable state.
        auto wtxn = db.begin_write_txn();
        if (!wtxn) {
            return std::unexpected(std::string("Failed to open database write transaction"));
        }
        auto del = db.delete_operation(*wtxn, record.id);
        if (!del) return std::unexpected(del.error());
        auto commit = wtxn->commit();
        if (!commit) return std::unexpected("Database commit failed: " + commit.error());
        if (auto retired = retire_transaction(target_root, record.transaction_dir); !retired) {
            util::log_warn("Operation '{}' completed but '{}' could not be retired ({}); \
orphan GC will retry next run",
                record.id, record.transaction_dir, retired.error());
        }
        ++outcome.finalized;
    }

    return outcome;
}

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
                                desired_config.providers.at("virtual/init") : "systemd";
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

    // True when some installed package declares a service that has no
    // definition on disk yet. Packages shipping their own native unit are not
    // pending: the reconcile pass lets package data win over the generated
    // form, so a present owner means there is nothing to generate. Unparsable
    // service.toml is not pending for backends that warn and skip it. Loom is
    // fail-closed, however, so its parse/destination errors must enter execute()
    // and preserve that error instead of being hidden by the no-op fast path.
    static std::expected<bool, std::string> services_awaiting_generation(
        db::Database& db,
        service::InitType target_init,
        const std::filesystem::path& sysroot)
    {
        auto txn = db.begin_read_txn();
        if (!txn) {
            return std::unexpected(
                "Failed to open service ownership read transaction: " + txn.error());
        }
        auto installed = db.list_installed_package_summaries(*txn);
        if (!installed) {
            return std::unexpected(
                "Installed package database is inconsistent: " + installed.error());
        }
        for (const auto& pkg : *installed) {
            if (pkg.service_toml.empty()) continue;
            auto spec = service::ServiceSpec::parse_toml(pkg.service_toml);
            if (!spec) {
                if (target_init == service::InitType::Loom) return true;
                continue;
            }
            auto dest = service::service_destination(spec->name, target_init, sysroot);
            if (!dest) {
                if (target_init == service::InitType::Loom) return true;
                continue;
            }
            const auto target = dest->lexically_relative(sysroot).generic_string();
            auto owners = db.get_path_owners(*txn, target);
            if (!owners) return std::unexpected(owners.error());
            if (!owners->empty()) continue;
            std::error_code ec;
            if (!std::filesystem::exists(*dest, ec)) return true;
            if (target_init == service::InitType::Loom) continue;
            const auto expected_mode = static_cast<std::filesystem::perms>(
                service::script_is_executable(target_init) ? 0755 : 0644);
            const auto actual_mode = std::filesystem::status(*dest, ec).permissions();
            if (ec || (actual_mode & std::filesystem::perms::mask) != expected_mode) return true;
            auto expected = service::render_service(*spec, target_init);
            if (!expected) continue;
            std::ifstream current(*dest);
            if (!current) return true;
            std::stringstream content;
            content << current.rdbuf();
            if (content.str() != *expected) return true;
        }
        return false;
    }

    static std::expected<void, std::string> execute(
        db::Database& db,
        const ReconcilePlan& plan,
        const std::filesystem::path& sysroot = "/",
        bool dry_run = false,
        const std::map<std::string, std::string>& providers = {})
    {
        (void)providers;
        if (!plan.has_changes) {
            // Providers already match, but that is not the only reason to
            // reconcile: nothing in the install path renders service scripts,
            // so a package carrying a service.toml has no definition on disk
            // until a reconcile runs. Returning here unconditionally meant
            // such a daemon was never wired up unless an unrelated provider
            // swap happened to come along.
            //
            // The check stays cheap deliberately. Entering the body now costs
            // a staging transaction, a journal and an fsync, so a genuine
            // no-op must still cost nothing.
            auto pending = services_awaiting_generation(db, plan.target_init, sysroot);
            if (!pending) return std::unexpected(pending.error());
            if (!*pending) {
                util::log_info("System state matches desired configuration. No reconcile needed.");
                return {};
            }
            util::log_info(
                "System providers already match; generating missing service definitions.");
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

        // Crash windows, in order (issue #9 protocol):
        //   1. Everything before persist_journal() only touches the staging
        //      area; a failure here lets the RAII destructor discard the
        //      transaction directory and nothing on disk or in LMDB changed.
        //   2. persist_journal() fsyncs the staged payload plus the journal
        //      text itself, so the evidence is durable BEFORE any database
        //      record can name it.
        //   3. put_operation() rides the SAME write txn as the provider swap
        //      and package metadata: LMDB commit is the single atomic point
        //      after which the operation "exists". A crash before it leaves an
        //      unrecorded directory for entry orphan GC; after it, resume()
        //      finishes the work.
        //   4. Only then does resume_pending_operations() publish to the live
        //      tree and run post-processing -- the exact code path a crashed
        //      reconcile would take on next entry.
        auto fsx_res = archive::FilesystemTransaction::create(sysroot);
        if (!fsx_res) {
            return std::unexpected(
                "Failed to create reconcile transaction: " + fsx_res.error());
        }
        auto& fsx = *fsx_res;

        auto wtxn = db.begin_write_txn();
        if (!wtxn) return std::unexpected(std::string("Failed to open database write transaction"));
        std::vector<std::pair<char, std::string>> service_touched;

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

        // 2. Outgoing providers: plan their physical removal as journal
        // entries (an exclusive capability allows exactly one provider on
        // disk, so the outgoing package's files must go before the incoming
        // ones land -- journal order guarantees that). Metadata withdrawal
        // happens right here; deletion itself is deferred to publication.
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
                if (!old_manifest.service_toml.empty()) {
                    auto old_service = service::ServiceSpec::parse_toml(
                        old_manifest.service_toml);
                    if (old_service) {
                        auto retired = service_registry::plan_remove_scripts(
                            db, *wtxn, fsx, old_service->name, old_manifest.name);
                        if (!retired) return std::unexpected(retired.error());
                        service_touched.insert(
                            service_touched.end(), retired->begin(), retired->end());
                    } else {
                        util::log_warn(
                            "Cannot identify generated service paths for '{}': {}",
                            old_manifest.name, old_service.error());
                    }
                }
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
                    // Trust what is actually on the ground when it can be
                    // seen; fall back to the manifest declaration otherwise.
                    std::error_code ec;
                    auto on_disk = std::filesystem::symlink_status(sysroot / path, ec);
                    const bool as_dir = !ec
                        ? on_disk.type() == std::filesystem::file_type::directory
                        : declared_directory;
                    if (as_dir) {
                        fsx.plan_remove_dir(path);
                    } else {
                        fsx.plan_remove_file(path);
                    }
                }

                auto file_res = db.unregister_files(*wtxn, old_manifest.files, my_owner);
                if (!file_res) return std::unexpected(file_res.error());
                auto provide_res = db.unregister_provides(*wtxn, old_manifest.provides);
                if (!provide_res) return std::unexpected(provide_res.error());
                auto delete_res = db.del_package(*wtxn, removal.name);
                if (!delete_res) return std::unexpected(delete_res.error());
            }
        }

        // 3. Incoming packages: unpack the archives into the transaction's
        // staging area and plan their publication. Channel index manifests
        // carry names and versions but not files or capability hooks, so the
        // archive manifest's hooks/triggers are adopted here -- otherwise
        // e.g. an initramfs generator installed by a swap would be invisible
        // to the very trigger that has to run it.
        if (!plan.packages_to_install.empty()) {
            auto payload = fsx.ensure_staged_dir("payload");
            if (!payload) return std::unexpected(payload.error());
        }
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
            auto service_registration = service_registry::validate_unique(
                db, *wtxn, selected.name, inspect_res->manifest.service_toml);
            if (!service_registration) return std::unexpected(service_registration.error());
            if (*existing
                && (**existing).service_toml != inspect_res->manifest.service_toml
                && !(**existing).service_toml.empty()) {
                auto old_service = service::ServiceSpec::parse_toml(
                    (**existing).service_toml);
                if (old_service) {
                    auto retired = service_registry::plan_remove_scripts(
                        db, *wtxn, fsx, old_service->name, selected.name);
                    if (!retired) return std::unexpected(retired.error());
                    service_touched.insert(
                        service_touched.end(), retired->begin(), retired->end());
                } else {
                    util::log_warn(
                        "Cannot identify generated service paths for '{}': {}",
                        selected.name, old_service.error());
                }
            }

            const std::filesystem::path staging_root =
                sysroot / fsx.relative_dir() / "payload";
            util::log_info("Staging {} -> {}...", selected.name, sysroot.string());
            auto ext_res = archive::extract_package(
                *archive_res, staging_root, &selected, &*inspect_res, nullptr,
                archive::ExtractionDurability::Batch);
            if (!ext_res) {
                return std::unexpected(std::format(
                    "Failed to stage package '{}': {}", selected.name, ext_res.error()));
            }

            auto& files = ext_res->extracted_files;
            std::ranges::stable_sort(files, {},
                [](const package::FileEntry& f) { return util::path_depth(f.path); });
            for (const auto& f : files) {
                const auto target = util::clean_rel_path(f.path);
                const std::string staged = std::format("payload/{}", target);
                switch (f.type) {
                    case package::FileType::Directory:
                        fsx.plan_ensure_dir(target);
                        break;
                    case package::FileType::Symlink:
                        fsx.plan_put_symlink(staged, target);
                        break;
                    case package::FileType::Hardlink:
                        fsx.plan_put_hardlink(target, f.link_target);
                        break;
                    default:
                        fsx.plan_put_file(target, staged, f.mode);
                        break;
                }
            }

            auto installed_pkg = selected;
            installed_pkg.files = std::move(ext_res->extracted_files);
            installed_pkg.capability_hooks = ext_res->manifest.capability_hooks;
            installed_pkg.triggers = ext_res->manifest.triggers;
            // Same archive-only metadata the install path adopts: the channel
            // index carries neither, and the service pass below reads the
            // service definition back out of the database.
            installed_pkg.conffiles = ext_res->manifest.conffiles;
            installed_pkg.service_toml = ext_res->manifest.service_toml;

            auto p_res = db.put_package(*wtxn, installed_pkg);
            if (!p_res) return std::unexpected(p_res.error());
            auto f_res = db.register_files(*wtxn, installed_pkg.name, installed_pkg.channel, installed_pkg.files);
            if (!f_res) return std::unexpected(f_res.error());
            auto prov_res = db.register_provides(*wtxn, installed_pkg.name, installed_pkg.provides);
            if (!prov_res) return std::unexpected(prov_res.error());
            installed_now.push_back(std::move(installed_pkg));
        }

        // 4. Native service configurations, staged this time: every daemon
        // (a package whose manifest carries a universal service.toml) gets
        // its script rendered into the transaction unless the package ships
        // its own native unit for the active init. Loom needs its external
        // compiler, so those stay post-publication.
        auto installed_after = db.list_installed_package_summaries(*wtxn);
        if (!installed_after) {
            return std::unexpected(
                "Installed package database is inconsistent during reconcile: " + installed_after.error());
        }
        size_t gen_count = 0;
        size_t staged_scripts = 0;
        std::vector<std::pair<std::string, std::string>> loom_services;
        for (const auto& pkg : *installed_after) {
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
            auto dest = service::service_destination(spec->name, plan.target_init, sysroot);
            if (!dest) {
                if (plan.target_init == service::InitType::Loom) {
                    return std::unexpected(std::format(
                        "Invalid Loom destination for '{}': {}", pkg.name, dest.error()));
                }
                util::log_warn("Skipping service for '{}': {}", pkg.name, dest.error());
                continue;
            }
            // Contract deviation (deliberate): Loom compilation shells out to
            // an external `loom compile-service` that writes its output
            // directly, so it cannot render into the staged area. Those
            // services are generated physically after publication, preserving
            // the pre-protocol behavior for this backend.
            if (plan.target_init == service::InitType::Loom) {
                loom_services.emplace_back(spec->name, pkg.service_toml);
                continue;
            }
            // A package may ship its own native script for this init (the
            // systemd split packages keep their upstream units): package data
            // wins over the generated form.
            const auto target = dest->lexically_relative(sysroot).generic_string();
            auto owners = db.get_path_owners(*wtxn, target);
            if (!owners) return std::unexpected(owners.error());
            if (!owners->empty()) {
                util::log_info("  · {:<20} ships its own {} script", spec->name,
                    service::to_string(plan.target_init));
                continue;
            }
            auto content = service::render_service(*spec, plan.target_init);
            if (!content) {
                util::log_warn("Cannot render {} script for '{}': {}",
                    service::to_string(plan.target_init), pkg.name, content.error());
                continue;
            }
            const std::string stage_rel = std::format("service-{}", staged_scripts++);
            const std::uint32_t script_mode =
                service::script_is_executable(plan.target_init) ? 0755 : 0644;
            auto staged = stage_text(fsx, stage_rel, *content, script_mode);
            if (!staged) {
                util::log_warn("Cannot stage {} script for '{}': {}",
                    service::to_string(plan.target_init), pkg.name, staged.error());
                continue;
            }
            const auto target_path = dest->lexically_relative(sysroot);
            std::vector<std::filesystem::path> parents;
            for (auto parent = target_path.parent_path(); !parent.empty() && parent != ".";
                 parent = parent.parent_path()) {
                parents.push_back(parent);
            }
            for (auto parent = parents.rbegin(); parent != parents.rend(); ++parent) {
                fsx.plan_ensure_dir(parent->generic_string());
            }
            fsx.plan_put_file(
                target_path.generic_string(), stage_rel, script_mode);
            service_touched.emplace_back('F', target_path.generic_string());
            gen_count++;
        }

        // 5. Journal the whole plan and commit metadata + record atomically.
        archive::JournalContext jctx;
        jctx.kind = "reconcile";
        jctx.final = true;
        jctx.sysroot = sysroot.string();
        jctx.touched.insert(
            jctx.touched.end(), service_touched.begin(), service_touched.end());
        for (const auto& pkg : installed_now) {
            for (const auto& f : pkg.files) {
                jctx.touched.emplace_back(package::to_string(f.type)[0],
                    util::clean_rel_path(f.path));
            }
            jctx.package_manifests_toml.push_back(pkg.serialize_summary_toml());
        }

        auto synced = fsx.sync_staging();
        if (!synced) {
            return std::unexpected("Failed to sync reconcile staging: " + synced.error());
        }
        auto sha = fsx.persist_journal(archive::render_journal(jctx, fsx.journal_entries()));
        if (!sha) {
            return std::unexpected("Failed to persist reconcile journal: " + sha.error());
        }

        auto op_id = archive::detail::random_hex(16);
        if (!op_id) return std::unexpected(op_id.error());
        db::FilesystemOperationRecord record{
            .id = *op_id,
            .kind = "reconcile",
            .phase = std::string(db::phase_filesystem_pending),
            .transaction_dir = fsx.relative_dir(),
            .journal_sha256 = *sha,
        };
        auto rec_res = db.put_operation(*wtxn, record);
        if (!rec_res) return std::unexpected(rec_res.error());

        auto commit_res = wtxn->commit();
        if (!commit_res) return std::unexpected("Database commit failed: " + commit_res.error());

        // 6. Publish and post-process through the shared recovery driver --
        // the exact code path a crashed reconcile would take on next entry.
        auto resumed = resume_pending_operations(db, sysroot);
        if (!resumed) return std::unexpected(resumed.error());

        if (!loom_services.empty()) {
            for (const auto& [name, document] : loom_services) {
                auto gen = service::generate_loom_service(document, name, sysroot);
                if (!gen) {
                    return std::unexpected(std::format(
                        "Cannot generate Loom service for '{}': {}", name, gen.error()));
                }
                gen_count++;
            }
            auto valid = service::validate_loom_services(sysroot);
            if (!valid) return std::unexpected(valid.error());
        }

        util::log_success("Reconcile completed! Regenerated {} native service scripts for {}",
            gen_count, service::to_string(plan.target_init));
        return {};
    }

    // A package removal can withdraw a native unit owned separately from the
    // daemon that declares it. Repair only the now-missing generated form; a
    // pending provider transition remains an explicit `sage rebuild` action.
    static std::expected<void, std::string> repair_missing_services(
        db::Database& db,
        const std::filesystem::path& sysroot)
    {
        auto cfg = config::SystemConfig::load_from_root(sysroot);
        if (!cfg) return std::unexpected(cfg.error());
        auto current = db.get_all_system_providers();
        if (!current) return std::unexpected(current.error());
        for (const auto& [iface, desired] : cfg->exclusive_providers()) {
            const auto installed = current->contains(iface) ? current->at(iface) : "";
            if (installed != desired) return {};
        }
        const auto init_provider = cfg->providers.contains("virtual/init")
            ? cfg->providers.at("virtual/init") : "systemd";
        ReconcilePlan plan;
        plan.target_init = service::parse_init_type(init_provider);
        if (plan.target_init == service::InitType::Unknown) {
            return std::unexpected(std::format(
                "Unsupported virtual/init provider '{}'", init_provider));
        }
        return execute(db, plan, sysroot, false, cfg->providers);
    }
};

} // namespace sage::rebuild
