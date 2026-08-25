module;
#include <sys/stat.h>
#include <unistd.h>

export module sage.tests.durability;

import std;
import sage;

import sage.cli;
import sage.cli.build;
import sage.cli.install;
import sage.cli.rebuild;
import sage.cli.remove;
import sage.tests.service_lifecycle;

namespace sage::tests {

using namespace sage::cli;
using std::size_t;
using std::uint8_t;
using std::uint32_t;
using std::uint64_t;

namespace durability {

export int run_durable_install_tests(const std::filesystem::path& temp_dir,
    bool (*write_test_channel)(const std::filesystem::path&, const std::filesystem::path&),
    std::string (*read_test_file)(const std::filesystem::path&),
    std::optional<std::string> (*sole_owner)(sage::db::Database&, std::string_view),
    std::vector<std::string> (*host_trigger_args)(
        const std::vector<std::filesystem::path>&, const std::filesystem::path&)) {
    // ---- Issue #9 durable state machine: crash-boundary coverage ---------
    // Invariants under test: a committed operation is always recoverable
    // (journal fsynced before the LMDB commit, replay idempotent), orphaned
    // staging is garbage-collected at the next entry, and abandon is the only
    // destructive exit.
    {
        auto recovery_root = temp_dir / "recovery-root";
        std::filesystem::create_directories(recovery_root);
        auto recovery_db_res = sage::db::Database::open(
            recovery_root / "var/lib/sage/data.mdb");
        if (!recovery_db_res) {
            sage::util::log_error("Failed to create recovery fixture database: {}",
                recovery_db_res.error());
            return 1;
        }
        auto& recovery_db = *recovery_db_res;

        // Stage a one-file install and persist its journal WITHOUT publishing,
        // then commit the operation record. This is the exact on-disk shape
        // left by a hard kill between the LMDB commit and the publish step.
        auto make_pending = [&](std::string_view id, std::string_view leaf) -> std::expected<void, std::string> {
            auto txn = sage::archive::FilesystemTransaction::create(recovery_root);
            if (!txn) return std::unexpected(txn.error());
            const std::string stage_rel =
                std::format("staged/usr/bin/{}", leaf);
            // Barrier publication renames the staged payload as-is, so the
            // staged file must already carry its final mode.
            auto fd = txn->open_staged_file(stage_rel, 0755);
            std::string payload = std::format("{} payload\n", leaf);
            if (::write(*fd, payload.data(), payload.size())
                != static_cast<ssize_t>(payload.size())) {
                return std::unexpected(std::format("short staged write for {}", leaf));
            }
            ::close(*fd);
            txn->plan_ensure_dir("usr/bin");
            txn->plan_put_file(
                std::format("usr/bin/{}", leaf), stage_rel, 0755);
            if (auto synced = txn->sync_staging(); !synced) return synced;
            sage::archive::JournalContext ctx;
            ctx.kind = "install";
            ctx.final = true;
            ctx.sysroot = recovery_root.string();
            auto sha = txn->persist_journal(
                sage::archive::render_journal(ctx, txn->journal_entries()));
            if (!sha) return std::unexpected(sha.error());
            auto wtxn = recovery_db.begin_write_txn();
            if (!wtxn) return std::unexpected("recovery fixture write txn failed");
            auto put = recovery_db.put_operation(*wtxn,
                {std::string{id}, "install",
                 std::string{sage::db::phase_filesystem_pending},
                 txn->relative_dir(), *sha});
            if (!put) return put;
            // The instance dies here after persist_journal(): the destructor
            // must preserve the evidence directory for recovery.
            if (!wtxn->commit()) return std::unexpected("recovery fixture commit failed");
            return {};
        };
        const std::string recoverable_id(32, 'a');
        const std::string abandoned_id(32, 'b');
        if (auto made = make_pending(recoverable_id, "tool-a"); !made) {
            sage::util::log_error("Failed to stage recoverable fixture: {}", made.error());
            return 1;
        }

        // Resume publishes the journal and finalizes: live tree has the file
        // with its real mode, the record is gone, the staging dir retired.
        auto resumed = sage::rebuild::resume_pending_operations(recovery_db, recovery_root);
        if (!resumed || resumed->finalized != 1) {
            sage::util::log_error("Pending operation was not recovered: {}",
                resumed ? std::format("finalized {}", resumed->finalized) : resumed.error());
            return 1;
        }
        auto tool_path = recovery_root / "usr/bin/tool-a";
        std::error_code mode_ec;
        const bool executable = (std::filesystem::status(tool_path, mode_ec).permissions()
            & std::filesystem::perms::owner_exec) != std::filesystem::perms::none;
        if (mode_ec || read_test_file(tool_path) != "tool-a payload\n" || !executable) {
            sage::util::log_error("Recovered publish lost content or mode of usr/bin/tool-a");
            return 1;
        }
        // Read probes run inside a scoped txn: an open reader must never
        // overlap the next resume call's own transactions.
        {
            auto settled_txn = recovery_db.begin_read_txn();
            auto record_after = settled_txn
                ? recovery_db.get_operation(*settled_txn, recoverable_id)
                : std::expected<std::optional<sage::db::FilesystemOperationRecord>, std::string>(
                    std::unexpected("read txn failed"));
            if (!record_after || record_after->has_value()
                || !sage::archive::list_transaction_dirs(recovery_root).empty()) {
                sage::util::log_error("Finalized operation left a record or staging behind");
                return 1;
            }
        }

        // Replay is idempotent: resuming again finds nothing to do.
        auto again = sage::rebuild::resume_pending_operations(recovery_db, recovery_root);
        if (!again || again->finalized != 0) {
            sage::util::log_error("Second resume was not idempotent: {}",
                again ? std::format("finalized {}", again->finalized) : again.error());
            return 1;
        }

        // A transaction directory with no LMDB record is an orphan from a
        // pre-commit crash: silently collected at the next entry.
        {
            auto orphan = sage::archive::FilesystemTransaction::create(recovery_root);
            if (!orphan) {
                sage::util::log_error("Failed to create orphan fixture: {}", orphan.error());
                return 1;
            }
            sage::archive::JournalContext ctx;
            ctx.kind = "remove";
            if (auto persisted = orphan->persist_journal(
                    sage::archive::render_journal(ctx, {}));
                !persisted) {
                sage::util::log_error("Failed to persist orphan journal: {}", persisted.error());
                return 1;
            }
        }
        auto gc = sage::rebuild::resume_pending_operations(recovery_db, recovery_root);
        if (!gc || !sage::archive::list_transaction_dirs(recovery_root).empty()) {
            sage::util::log_error("Orphan transaction directory survived recovery GC");
            return 1;
        }

        // Abandon is the explicit destructive escape: the record disappears,
        // the evidence directory retires, and nothing reaches the live tree.
        if (auto made = make_pending(abandoned_id, "tool-b"); !made) {
            sage::util::log_error("Failed to stage abandon fixture: {}", made.error());
            return 1;
        }
        auto abandoned = sage::rebuild::resume_pending_operations(
            recovery_db, recovery_root, abandoned_id);
        auto abandoned_txn = recovery_db.begin_read_txn();
        auto abandoned_record = abandoned_txn
            ? recovery_db.get_operation(*abandoned_txn, abandoned_id)
            : std::expected<std::optional<sage::db::FilesystemOperationRecord>, std::string>(
                std::unexpected("read txn failed"));
        if (!abandoned || abandoned->finalized != 0
            || std::filesystem::exists(recovery_root / "usr/bin/tool-b")
            || !abandoned_record || abandoned_record->has_value()
            || !sage::archive::list_transaction_dirs(recovery_root).empty()) {
            sage::util::log_error("Abandon path did not retire the stuck operation cleanly");
            return 1;
        }
        sage::util::log_success("   Durable Operation Recovery & Orphan GC OK");
    }

    // Issue #28 repro: a failed plan entry used to be swallowed because the
    // failure sentinel was a default-constructed std::expected<void, E> --
    // which holds a value -- so both `if (failure)` checks ran inverted and
    // publish() fell through to the parent-flush result. Publish a journal
    // whose only entry targets a parent no EnsureDir created; publication
    // must fail instead of reporting success while nothing reached the tree.
    {
        auto sentinel_root = temp_dir / "sentinel-root";
        std::filesystem::create_directories(sentinel_root);
        auto sentinel_txn = sage::archive::FilesystemTransaction::create(sentinel_root);
        if (!sentinel_txn) {
            sage::util::log_error("Failed to create sentinel fixture: {}",
                sentinel_txn.error());
            return 1;
        }
        const std::string orphan_stage = "staged/orphan.bin";
        auto staged_fd = sentinel_txn->open_staged_file(orphan_stage, 0644);
        if (!staged_fd) {
            sage::util::log_error("Failed to open staged file: {}", staged_fd.error());
            return 1;
        }
        std::string payload = "orphan payload\n";
        if (::write(*staged_fd, payload.data(), payload.size())
            != static_cast<ssize_t>(payload.size())) {
            return 1;
        }
        ::close(*staged_fd);
        // Deliberately NO plan_ensure_dir("opt/orphan"): resolve_target must
        // reject the entry, and that rejection must reach the caller.
        sentinel_txn->plan_put_file("opt/orphan/bin", orphan_stage, 0644);
        if (auto synced = sentinel_txn->sync_staging(); !synced) {
            sage::util::log_error("Failed to sync staging: {}", synced.error());
            return 1;
        }
        sage::archive::JournalContext ctx;
        ctx.kind = "install";
        ctx.final = true;
        ctx.sysroot = sentinel_root.string();
        auto published = sentinel_txn->publish(
            sage::archive::render_journal(ctx, sentinel_txn->journal_entries()));
        if (published) {
            sage::util::log_error(
                "Publication of a parentless PutFile was reported as success");
            return 1;
        }
        if (std::filesystem::exists(sentinel_root / "opt/orphan/bin")) {
            sage::util::log_error("Failed publication materialized its target");
            return 1;
        }
        sage::util::log_success("   Transaction Publication Failure Propagation OK");
    }

    // Issue #9 repro: the old revision owns usr/lib/foo/ and the admin dropped
    // foreign state inside it; the new payload drops that subtree. The staged
    // protocol preserves the non-empty directory while the upgrade succeeds --
    // the pre-protocol code failed here with ENOTEMPTY and forked DB from disk.
    {
        auto fork_repo = temp_dir / "fork-repo";
        auto fork_v1_data = temp_dir / "fork-v1-data";
        auto fork_v2_data = temp_dir / "fork-v2-data";
        std::filesystem::create_directories(fork_repo);
        std::filesystem::create_directories(fork_v1_data / "usr/bin");
        std::filesystem::create_directories(fork_v1_data / "usr/lib/foo");
        std::filesystem::create_directories(fork_v2_data / "usr/bin");
        std::ofstream(fork_v1_data / "usr/bin/foo") << "foo v1\n";
        std::ofstream(fork_v1_data / "usr/lib/foo/plugin.dat") << "plugin\n";
        std::ofstream(fork_v2_data / "usr/bin/foo") << "foo v2\n";
        sage::package::PackageManifest fork_old;
        fork_old.name = "foo";
        fork_old.version = sage::package::Version::parse("1.0.0-1");
        sage::package::PackageManifest fork_new = fork_old;
        fork_new.version = sage::package::Version::parse("2.0.0-1");
        if (!sage::archive::create_package(
                fork_old, fork_v1_data, fork_repo / "foo-1.0.0-1-x86_64.pkg.tar.zst")
            || !sage::archive::generate_repo_index(fork_repo, "core")) {
            sage::util::log_error("Failed to create fork-scenario monolith fixture");
            return 1;
        }
        auto fork_target = temp_dir / "fork-target";
        if (!write_test_channel(fork_target, fork_repo)) {
            sage::util::log_error("Failed to write fork-scenario channel");
            return 1;
        }
        CliOptions fork_install;
        fork_install.target_root = fork_target;
        fork_install.args = {"foo"};
        if (cmd_install(fork_install) != 0
            || read_test_file(fork_target / "usr/lib/foo/plugin.dat") != "plugin\n") {
            sage::util::log_error("Fork-scenario monolith failed to install");
            return 1;
        }
        // Foreign state appears inside the package-owned directory.
        std::ofstream(fork_target / "usr/lib/foo/user.conf") << "admin state\n";
        if (!sage::archive::create_package(
                fork_new, fork_v2_data, fork_repo / "foo-2.0.0-1-x86_64.pkg.tar.zst")
            || !sage::archive::generate_repo_index(fork_repo, "core")) {
            sage::util::log_error("Failed to create fork-scenario upgrade fixture");
            return 1;
        }
        auto fork_db = sage::db::Database::open(
            fork_target / "var/lib/sage/data.mdb", true);
        if (cmd_install(fork_install) != 0
            || read_test_file(fork_target / "usr/bin/foo") != "foo v2\n"
            || read_test_file(fork_target / "usr/lib/foo/user.conf") != "admin state\n"
            || std::filesystem::exists(fork_target / "usr/lib/foo/plugin.dat")
            || !fork_db
            || sole_owner(*fork_db, "usr/lib/foo")) {
            sage::util::log_error("Non-empty stale directory forked the upgrade or disk state");
            return 1;
        }
        sage::util::log_success("   Non-Empty Stale Directory Upgrade Survival OK");
    }

    auto transaction_repo = temp_dir / "transaction-repo";
    auto transaction_a_data = temp_dir / "transaction-a-data";
    auto transaction_b_data = temp_dir / "transaction-b-data";
    auto transaction_target = temp_dir / "transaction-target";
    auto failed_install_trigger_count = temp_dir / "failed-install-trigger-count";
    std::filesystem::create_directories(transaction_repo);
    std::filesystem::create_directories(transaction_a_data / "usr/bin");
    std::filesystem::create_directories(
        transaction_a_data / "usr/share/transaction-trigger");
    std::ofstream(transaction_a_data / "usr/bin/transaction-a") << "committed package\n";
    std::ofstream(transaction_a_data / "usr/share/transaction-trigger/committed")
        << "committed trigger input\n";
    auto transaction_clang = transaction_a_data / "opt/channels/llvm/99/bin/clang";
    std::filesystem::create_directories(transaction_clang.parent_path());
    std::ofstream(transaction_clang) << "#!/bin/sh\nexit 0\n";
    std::filesystem::permissions(
        transaction_clang,
        std::filesystem::perms::owner_all
            | std::filesystem::perms::group_read
            | std::filesystem::perms::group_exec
            | std::filesystem::perms::others_read
            | std::filesystem::perms::others_exec);
    std::filesystem::create_directories(transaction_b_data / "usr/share");
    std::filesystem::create_symlink("elsewhere", transaction_b_data / "usr/share/blocked");

    sage::package::PackageManifest transaction_a;
    transaction_a.name = "transaction-a";
    transaction_a.version = sage::package::Version::parse("1.0.0-1");
    transaction_a.channel = "toolchain/llvm:99";
    sage::package::Trigger failed_install_trigger;
    failed_install_trigger.name = "failed-install-cache";
    failed_install_trigger.on_paths = {"usr/share/transaction-trigger/"};
    failed_install_trigger.exec = "/bin/sh";
    failed_install_trigger.args = host_trigger_args(
        {transaction_target / "usr/share/transaction-trigger/committed"},
        failed_install_trigger_count);
    transaction_a.triggers = {failed_install_trigger};
    sage::package::PackageManifest transaction_b;
    transaction_b.name = "transaction-b";
    transaction_b.version = sage::package::Version::parse("1.0.0-1");
    transaction_b.channel = "system";
    transaction_b.dependencies.push_back(sage::package::Dependency::parse("transaction-a"));

    auto transaction_a_pkg = transaction_repo / "transaction-a-1.0.0-1-x86_64.pkg.tar.zst";
    auto transaction_b_pkg = transaction_repo / "transaction-b-1.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(transaction_a, transaction_a_data, transaction_a_pkg)
        || !sage::archive::create_package(transaction_b, transaction_b_data, transaction_b_pkg)
        || !sage::archive::generate_repo_index(transaction_repo, "core")) {
        sage::util::log_error("Failed to create multi-package transaction fixture");
        return 1;
    }

    std::filesystem::create_directories(transaction_target / "etc/sage");
    std::filesystem::create_directories(transaction_target / "usr/share/blocked");
    std::ofstream(transaction_target / "usr/share/blocked/keep") << "must survive\n";
    std::ofstream transaction_channels(transaction_target / "etc/sage/channels.toml");
    transaction_channels
        << "schema_version = 1\n\n[[channels]]\nname = \"core\"\nurl = \"file://"
        << transaction_repo.string()
        << "\"\nscope = \"system\"\npriority = 100\nenabled = true\n";
    transaction_channels.close();

    CliOptions transaction_install;
    transaction_install.target_root = transaction_target;
    transaction_install.args = {"transaction-b"};
    if (cmd_install(transaction_install, "/") == 0) {
        sage::util::log_error("Multi-package install accepted a later package path conflict");
        return 1;
    }
    auto transaction_db = sage::db::Database::open(
        transaction_target / "var/lib/sage/data.mdb", true);
    if (!transaction_db) {
        sage::util::log_error("Failed to inspect multi-package transaction database");
        return 1;
    }
    auto transaction_a_record = transaction_db->get_package("transaction-a");
    auto transaction_b_record = transaction_db->get_package("transaction-b");
    auto transaction_cc_link = transaction_target / "etc/sage/profiles/default/bin/cc";
    std::error_code transaction_cc_ec;
    if (!transaction_a_record || !*transaction_a_record
        || !transaction_b_record || *transaction_b_record
        || !std::filesystem::exists(transaction_target / "usr/bin/transaction-a")
        || !std::filesystem::exists(transaction_target / "usr/share/blocked/keep")
        || !std::filesystem::is_symlink(transaction_cc_link, transaction_cc_ec)
        || std::filesystem::read_symlink(transaction_cc_link, transaction_cc_ec)
            != "/opt/channels/llvm/99/bin/clang"
        || !std::filesystem::exists(transaction_target / "etc/profile.d/sage-channels.sh")
        || read_test_file(failed_install_trigger_count) != "x\n") {
        sage::util::log_error(
            "A later package failure desynchronized an earlier committed package or skipped its post-processing");
        return 1;
    }

    // Issue #18 end-to-end: a fixed-exec trigger pointing at an executable
    // the target root does not have must not fail an install when optional
    // -- every package is committed, so only a warning is warranted -- while
    // a required trigger still aborts. The round-trip through
    // serialize_triggers_toml -> .METADATA/triggers.toml -> parse keeps the
    // required flag visible in the installed manifest.
    auto policy_repo = temp_dir / "trigger-policy-repo";
    auto optional_policy_data = temp_dir / "trigger-optional-data";
    auto required_policy_data = temp_dir / "trigger-required-data";
    auto policy_target = temp_dir / "trigger-policy-target";
    std::filesystem::create_directories(policy_repo);
    std::filesystem::create_directories(optional_policy_data / "usr/share/trigger-policy");
    std::filesystem::create_directories(required_policy_data / "usr/share/trigger-policy");
    std::ofstream(optional_policy_data / "usr/share/trigger-policy/payload") << "optional\n";
    std::ofstream(required_policy_data / "usr/share/trigger-policy/payload") << "required\n";

    sage::package::PackageManifest optional_policy_pkg;
    optional_policy_pkg.name = "optional-trigger-pkg";
    optional_policy_pkg.version = sage::package::Version::parse("1.0.0-1");
    optional_policy_pkg.triggers.push_back(sage::package::Trigger{
        .name = "optional-missing-exec",
        .on_paths = {"usr/share/trigger-policy/"},
        .on_capability = {},
        .exec = "/usr/bin/sage-no-such-post-transaction-tool",
        .args = {},
        .run_capability = {},
        .required = false,
    });
    sage::package::PackageManifest required_policy_pkg = optional_policy_pkg;
    required_policy_pkg.name = "required-trigger-pkg";
    required_policy_pkg.triggers.front().name = "required-missing-exec";
    required_policy_pkg.triggers.front().required = true;

    auto optional_policy_archive =
        policy_repo / "optional-trigger-pkg-1.0.0-1-x86_64.pkg.tar.zst";
    auto required_policy_archive =
        policy_repo / "required-trigger-pkg-1.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(
            optional_policy_pkg, optional_policy_data, optional_policy_archive)
        || !sage::archive::create_package(
            required_policy_pkg, required_policy_data, required_policy_archive)
        || !sage::archive::generate_repo_index(policy_repo, "core")) {
        sage::util::log_error("Failed to create trigger policy fixtures");
        return 1;
    }
    std::filesystem::create_directories(policy_target / "etc/sage");
    std::ofstream policy_channels(policy_target / "etc/sage/channels.toml");
    policy_channels
        << "schema_version = 1\n\n[[channels]]\nname = \"core\"\nurl = \"file://"
        << policy_repo.string()
        << "\"\nscope = \"system\"\npriority = 100\nenabled = true\n";
    policy_channels.close();

    CliOptions optional_policy_install;
    optional_policy_install.target_root = policy_target;
    optional_policy_install.args = {"optional-trigger-pkg"};
    if (cmd_install(optional_policy_install, "/") != 0) {
        sage::util::log_error("Optional trigger with a missing executable failed the install");
        return 1;
    }
    {
        auto policy_db = sage::db::Database::open(
            policy_target / "var/lib/sage/data.mdb", true);
        if (!policy_db) {
            sage::util::log_error("Failed to inspect trigger policy database");
            return 1;
        }
        auto optional_record = policy_db->get_package("optional-trigger-pkg");
        if (!optional_record || !*optional_record
            || (**optional_record).triggers.size() != 1
            || (**optional_record).triggers.front().required
            || (**optional_record).triggers.front().exec
                != "/usr/bin/sage-no-such-post-transaction-tool"
            || !std::filesystem::exists(policy_target / "usr/share/trigger-policy/payload")) {
            sage::util::log_error(
                "Optional-missing-exec trigger did not survive packaging or the install diverged");
            return 1;
        }
    }

    CliOptions required_policy_install;
    required_policy_install.target_root = policy_target;
    required_policy_install.args = {"required-trigger-pkg"};
    if (cmd_install(required_policy_install, "/") == 0) {
        sage::util::log_error("Required trigger with a missing executable did not fail the install");
        return 1;
    }
    sage::util::log_success("   Trigger Required/Optional Exec Policy OK");

    // Split packages in one toolchain slot must refresh activation as each
    // package commits. The dependency installs libraries first; the compiler
    // package that follows is what makes the cc alias possible. Aggregate
    // triggers run only after both packages are present, and duplicate trigger
    // declarations resolving to one command execute once.
    auto split_repo = temp_dir / "split-toolchain-repo";
    auto split_libs_data = temp_dir / "split-toolchain-libs-data";
    auto split_compiler_data = temp_dir / "split-toolchain-compiler-data";
    auto split_guard_data = temp_dir / "split-toolchain-guard-data";
    auto split_target = temp_dir / "split-toolchain-target";
    auto split_trigger_count = temp_dir / "split-trigger-count";
    auto split_library = split_libs_data / "opt/channels/llvm/77/lib/libsplit.so";
    auto split_compiler_library =
        split_compiler_data / "opt/channels/llvm/77/lib/libsplit-compiler.so";
    auto split_clang = split_compiler_data / "opt/channels/llvm/77/bin/clang";
    std::filesystem::create_directories(split_repo);
    std::filesystem::create_directories(split_library.parent_path());
    std::filesystem::create_directories(split_compiler_library.parent_path());
    std::filesystem::create_directories(split_clang.parent_path());
    std::filesystem::create_directories(split_guard_data / "usr/share");
    std::ofstream(split_library) << "split toolchain library\n";
    std::ofstream(split_compiler_library) << "split compiler library\n";
    std::ofstream(split_clang) << "#!/bin/sh\nexit 0\n";
    std::ofstream(split_guard_data / "usr/share/split-trigger-guard") << "guard\n";
    std::filesystem::permissions(
        split_clang,
        std::filesystem::perms::owner_all
            | std::filesystem::perms::group_read
            | std::filesystem::perms::group_exec
            | std::filesystem::perms::others_read
            | std::filesystem::perms::others_exec);

    sage::package::PackageManifest split_libs;
    split_libs.name = "split-toolchain-libs";
    split_libs.version = sage::package::Version::parse("1.0.0-1");
    split_libs.channel = "toolchain/llvm:77";
    sage::package::Trigger split_trigger;
    split_trigger.name = "split-cache-primary";
    split_trigger.on_paths = {"opt/channels/llvm/77/lib/"};
    split_trigger.exec = "/bin/sh";
    split_trigger.args = host_trigger_args(
        {
            split_target / "opt/channels/llvm/77/lib/libsplit.so",
            split_target / "opt/channels/llvm/77/lib/libsplit-compiler.so",
        },
        split_trigger_count);
    auto duplicate_split_trigger = split_trigger;
    duplicate_split_trigger.name = "split-cache-duplicate";
    split_libs.triggers = {split_trigger, duplicate_split_trigger};
    sage::package::PackageManifest split_compiler;
    split_compiler.name = "split-toolchain-compiler";
    split_compiler.version = sage::package::Version::parse("1.0.0-1");
    split_compiler.channel = "toolchain/llvm:77";
    split_compiler.dependencies.push_back(
        sage::package::Dependency::parse("split-toolchain-libs"));
    sage::package::PackageManifest split_guard;
    split_guard.name = "split-trigger-guard";
    split_guard.version = sage::package::Version::parse("1.0.0-1");
    sage::package::Trigger failed_remove_trigger;
    failed_remove_trigger.name = "failed-remove-trigger";
    failed_remove_trigger.on_paths = {"opt/channels/llvm/77/bin/"};
    failed_remove_trigger.required = true; // must hard-fail removal's trigger pass
    failed_remove_trigger.exec = "/usr/bin/sage-missing-remove-trigger";
    split_guard.triggers = {failed_remove_trigger};

    auto split_libs_archive =
        split_repo / "split-toolchain-libs-1.0.0-1-x86_64.pkg.tar.zst";
    auto split_compiler_archive =
        split_repo / "split-toolchain-compiler-1.0.0-1-x86_64.pkg.tar.zst";
    auto split_guard_archive =
        split_repo / "split-trigger-guard-1.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(split_libs, split_libs_data, split_libs_archive)
        || !sage::archive::create_package(
            split_compiler, split_compiler_data, split_compiler_archive)
        || !sage::archive::create_package(split_guard, split_guard_data, split_guard_archive)
        || !sage::archive::generate_repo_index(split_repo, "core")) {
        sage::util::log_error("Failed to create split toolchain fixtures");
        return 1;
    }

    if (!write_test_channel(split_target, split_repo)) {
        sage::util::log_error("Failed to write split toolchain test channel");
        return 1;
    }
    CliOptions split_install;
    split_install.target_root = split_target;
    split_install.args = {"split-toolchain-compiler"};
    if (cmd_install(split_install, "/") != 0) {
        sage::util::log_error("Failed to install split toolchain packages");
        return 1;
    }
    split_install.args = {"split-trigger-guard"};
    if (cmd_install(split_install, "/") != 0) {
        sage::util::log_error("Failed to install post-remove trigger guard");
        return 1;
    }
    auto split_cc_link = split_target / "etc/sage/profiles/default/bin/cc";
    std::error_code split_cc_ec;
    if (!std::filesystem::is_symlink(split_cc_link, split_cc_ec)
        || std::filesystem::read_symlink(split_cc_link, split_cc_ec)
            != "/opt/channels/llvm/77/bin/clang"
        || read_test_file(split_trigger_count) != "x\n") {
        sage::util::log_error(
            "Split toolchain activation or aggregate trigger timing is incorrect");
        return 1;
    }

    auto split_owner_db = sage::db::Database::open(
        split_target / "var/lib/sage/data.mdb", true);
    if (!split_owner_db
        || sole_owner(*split_owner_db, "opt/channels/llvm/77/bin/clang")
            != "split-toolchain-compiler:toolchain/llvm:77") {
        sage::util::log_error("Split toolchain removal fixture has no registered file owner");
        return 1;
    }

    // Replacing an owned regular file with a non-empty directory forces a real
    // filesystem removal error. The command must fail and keep both DB records.
    auto installed_split_clang = split_target / "opt/channels/llvm/77/bin/clang";
    std::filesystem::remove(installed_split_clang);
    std::filesystem::create_directories(installed_split_clang);
    std::ofstream(installed_split_clang / "keep") << "block removal\n";
    CliOptions split_remove;
    split_remove.target_root = split_target;
    split_remove.args = {"split-toolchain-compiler"};
    if (cmd_remove(split_remove) == 0) {
        sage::util::log_error("Package removal ignored a real filesystem error");
        return 1;
    }
    {
        auto split_db = sage::db::Database::open(
            split_target / "var/lib/sage/data.mdb", true);
        auto split_compiler_record = split_db
            ? split_db->get_package("split-toolchain-compiler")
            : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
                std::unexpected("database open failed"));
        auto split_libs_record = split_db
            ? split_db->get_package("split-toolchain-libs")
            : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
                std::unexpected("database open failed"));
        if (!split_compiler_record || !*split_compiler_record
            || !split_libs_record || !*split_libs_record) {
            sage::util::log_error("Failed split removal discarded installed database records");
            return 1;
        }
    }

    // Restore the owned file and verify removal uses the sysroot-relative paths
    // stored in the manifest instead of prepending the toolchain root twice.
    // A remaining package deliberately fails its post-remove trigger; removal
    // still commits, and profile regeneration must discard the now-dangling cc
    // link before that trigger error is returned.
    std::error_code restore_ec;
    std::filesystem::remove_all(installed_split_clang, restore_ec);
    if (restore_ec) {
        sage::util::log_error("Failed to restore split toolchain fixture: {}", restore_ec.message());
        return 1;
    }
    std::ofstream(installed_split_clang) << "#!/bin/sh\nexit 0\n";
    if (cmd_remove(split_remove) == 0
        || std::filesystem::exists(split_target / "opt/channels/llvm/77/bin/clang")
        || std::filesystem::exists(split_target / "opt/channels/llvm/77/lib/libsplit.so")
        || std::filesystem::exists(
            split_target / "opt/channels/llvm/77/lib/libsplit-compiler.so")
        || std::filesystem::is_symlink(split_cc_link)) {
        sage::util::log_error(
            "Failed post-remove trigger left package paths or a stale profile link");
        return 1;
    }
    auto removed_split_db = sage::db::Database::open(
        split_target / "var/lib/sage/data.mdb", true);
    auto removed_split_packages = removed_split_db
        ? removed_split_db->list_installed_packages()
        : std::expected<std::vector<sage::package::PackageManifest>, std::string>(
            std::unexpected("database open failed"));
    if (!removed_split_packages || removed_split_packages->size() != 1
        || removed_split_packages->front().name != "split-trigger-guard") {
        sage::util::log_error("Failed trigger changed the committed removal state");
        return 1;
    }
    // An empty directory declared by two packages is shared: both register as
    // owners, one removal only releases its claim, and the last removal takes
    // the directory away.
    auto shared_repo = temp_dir / "shared-dir-repo";
    auto shared_a_data = temp_dir / "shared-a-data";
    auto shared_b_data = temp_dir / "shared-b-data";
    std::filesystem::create_directories(shared_repo);
    std::filesystem::create_directories(shared_a_data / "usr/bin");
    std::filesystem::create_directories(shared_a_data / "usr/share/common");
    std::filesystem::create_directories(shared_b_data / "usr/bin");
    std::filesystem::create_directories(shared_b_data / "usr/share/common");
    std::ofstream(shared_a_data / "usr/bin/share-a") << "a\n";
    std::ofstream(shared_b_data / "usr/bin/share-b") << "b\n";
    sage::package::PackageManifest share_a;
    share_a.name = "share-a";
    share_a.version = sage::package::Version::parse("1.0.0-1");
    sage::package::PackageManifest share_b;
    share_b.name = "share-b";
    share_b.version = sage::package::Version::parse("1.0.0-1");
    auto share_a_pkg = shared_repo / "share-a-1.0.0-1-x86_64.pkg.tar.zst";
    auto share_b_pkg = shared_repo / "share-b-1.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(share_a, shared_a_data, share_a_pkg)
        || !sage::archive::create_package(share_b, shared_b_data, share_b_pkg)
        || !sage::archive::generate_repo_index(shared_repo, "core")) {
        sage::util::log_error("Failed to create shared directory fixtures");
        return 1;
    }
    auto shared_target = temp_dir / "shared-dir-target";
    if (!write_test_channel(shared_target, shared_repo)) {
        sage::util::log_error("Failed to write shared directory test channel");
        return 1;
    }
    CliOptions shared_install;
    shared_install.target_root = shared_target;
    shared_install.args = {"share-a", "share-b"};
    if (cmd_install(shared_install) != 0
        || !std::filesystem::is_directory(shared_target / "usr/share/common")) {
        sage::util::log_error("Shared empty directory install failed");
        return 1;
    }
    auto shared_db = sage::db::Database::open(
        shared_target / "var/lib/sage/data.mdb", true);
    auto common_owners = shared_db
        ? shared_db->get_path_owners("usr/share/common")
        : std::expected<std::vector<std::string>, std::string>(
            std::unexpected("database open failed"));
    if (!common_owners || common_owners->size() != 2) {
        sage::util::log_error("Shared directory was not registered with both owners");
        return 1;
    }
    CliOptions shared_remove_a;
    shared_remove_a.target_root = shared_target;
    shared_remove_a.args = {"share-a"};
    if (cmd_remove(shared_remove_a) != 0
        || !std::filesystem::is_directory(shared_target / "usr/share/common")) {
        sage::util::log_error("Removing one owner deleted the shared directory");
        return 1;
    }
    CliOptions shared_remove_b;
    shared_remove_b.target_root = shared_target;
    shared_remove_b.args = {"share-b"};
    if (cmd_remove(shared_remove_b) != 0
        || std::filesystem::exists(shared_target / "usr/share/common")) {
        sage::util::log_error("Last owner removal left the shared directory behind");
        return 1;
    }

    // A reinstall that drops directories releases their claims: a shared
    // directory survives with its remaining owner, a sole directory goes away.
    auto dropper_repo = temp_dir / "dropper-repo";
    auto dropper_v1_data = temp_dir / "dropper-v1-data";
    auto dropper_v2_data = temp_dir / "dropper-v2-data";
    auto keeper_data = temp_dir / "keeper-data";
    std::filesystem::create_directories(dropper_repo);
    std::filesystem::create_directories(dropper_v1_data / "usr/bin");
    std::filesystem::create_directories(dropper_v1_data / "usr/share/extra");
    std::filesystem::create_directories(dropper_v1_data / "usr/share/common");
    std::filesystem::create_directories(dropper_v2_data / "usr/bin");
    std::filesystem::create_directories(keeper_data / "usr/bin");
    std::filesystem::create_directories(keeper_data / "usr/share/common");
    std::ofstream(dropper_v1_data / "usr/bin/dtool") << "dropper v1\n";
    std::ofstream(dropper_v2_data / "usr/bin/dtool") << "dropper v2\n";
    std::ofstream(dropper_v2_data / "usr/bin/dtool2") << "dropper v2 extra\n";
    std::ofstream(keeper_data / "usr/bin/ktool") << "keeper\n";
    sage::package::PackageManifest dropper_v1;
    dropper_v1.name = "dropper";
    dropper_v1.version = sage::package::Version::parse("1.0.0-1");
    sage::package::PackageManifest dropper_v2 = dropper_v1;
    dropper_v2.version = sage::package::Version::parse("2.0.0-1");
    sage::package::PackageManifest keeper;
    keeper.name = "keeper";
    keeper.version = sage::package::Version::parse("1.0.0-1");
    auto dropper_v1_pkg = dropper_repo / "dropper-1.0.0-1-x86_64.pkg.tar.zst";
    auto dropper_v2_pkg = dropper_repo / "dropper-2.0.0-1-x86_64.pkg.tar.zst";
    auto keeper_pkg = dropper_repo / "keeper-1.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(dropper_v1, dropper_v1_data, dropper_v1_pkg)
        || !sage::archive::create_package(dropper_v2, dropper_v2_data, dropper_v2_pkg)
        || !sage::archive::create_package(keeper, keeper_data, keeper_pkg)
        || !sage::archive::generate_repo_index(dropper_repo, "core")) {
        sage::util::log_error("Failed to create dropped-directory fixtures");
        return 1;
    }
    auto dropper_target = temp_dir / "dropper-target";
    if (!write_test_channel(dropper_target, dropper_repo)) {
        sage::util::log_error("Failed to write dropped-directory test channel");
        return 1;
    }
    CliOptions dropper_install;
    dropper_install.target_root = dropper_target;
    dropper_install.args = {dropper_v1_pkg.string()};
    if (cmd_install(dropper_install) != 0) {
        sage::util::log_error("Failed to install dropper version 1");
        return 1;
    }
    dropper_install.args = {"keeper"};
    if (cmd_install(dropper_install) != 0) {
        sage::util::log_error("Failed to install the shared-directory keeper");
        return 1;
    }
    dropper_install.args = {"dropper"};
    if (cmd_install(dropper_install) != 0
        || std::filesystem::exists(dropper_target / "usr/share/extra")
        || !std::filesystem::is_directory(dropper_target / "usr/share/common")
        || read_test_file(dropper_target / "usr/bin/dtool2") != "dropper v2 extra\n") {
        sage::util::log_error("Reinstall did not release dropped directories correctly");
        return 1;
    }
    auto dropper_db = sage::db::Database::open(
        dropper_target / "var/lib/sage/data.mdb", true);
    if (sole_owner(*dropper_db, "usr/share/common") != "keeper:system") {
        sage::util::log_error("Released shared directory kept the stale owner claim");
        return 1;
    }

    // A declared directory that gained foreign content survives a reinstall
    // that no longer ships it; only genuinely empty dropped directories go.
    auto tool_repo = temp_dir / "tool-repo";
    auto tool_v1_data = temp_dir / "tool-v1-data";
    auto tool_v2_data = temp_dir / "tool-v2-data";
    std::filesystem::create_directories(tool_repo);
    std::filesystem::create_directories(tool_v1_data / "usr/bin");
    std::filesystem::create_directories(tool_v1_data / "usr/share/data");
    std::filesystem::create_directories(tool_v2_data / "usr/bin");
    std::ofstream(tool_v1_data / "usr/bin/tool") << "tool v1\n";
    std::ofstream(tool_v2_data / "usr/bin/tool2") << "tool v2\n";
    sage::package::PackageManifest tool_v1;
    tool_v1.name = "dtool";
    tool_v1.version = sage::package::Version::parse("1.0.0-1");
    sage::package::PackageManifest tool_v2 = tool_v1;
    tool_v2.version = sage::package::Version::parse("2.0.0-1");
    auto tool_v1_pkg = tool_repo / "dtool-1.0.0-1-x86_64.pkg.tar.zst";
    auto tool_v2_pkg = tool_repo / "dtool-2.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(tool_v1, tool_v1_data, tool_v1_pkg)
        || !sage::archive::create_package(tool_v2, tool_v2_data, tool_v2_pkg)
        || !sage::archive::generate_repo_index(tool_repo, "core")) {
        sage::util::log_error("Failed to create declared-directory upgrade fixtures");
        return 1;
    }
    auto tool_target = temp_dir / "tool-target";
    if (!write_test_channel(tool_target, tool_repo)) {
        sage::util::log_error("Failed to write declared-directory test channel");
        return 1;
    }
    CliOptions tool_install;
    tool_install.target_root = tool_target;
    tool_install.args = {tool_v1_pkg.string()};
    if (cmd_install(tool_install) != 0) {
        sage::util::log_error("Failed to install declared-directory fixture version 1");
        return 1;
    }
    std::ofstream(tool_target / "usr/share/data/foreign") << "user data\n";
    tool_install.args = {"dtool"};
    auto tool_record = std::optional<sage::package::PackageManifest>{};
    if (cmd_install(tool_install) == 0) {
        auto tool_db = sage::db::Database::open(
            tool_target / "var/lib/sage/data.mdb", true);
        auto tool_pkg = tool_db ? tool_db->get_package("dtool")
            : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
                std::unexpected("database open failed"));
        tool_record = tool_pkg ? *tool_pkg : std::nullopt;
    }
    if (!tool_record
        || tool_record->version != tool_v2.version
        || std::filesystem::exists(tool_target / "usr/bin/tool")
        || read_test_file(tool_target / "usr/bin/tool2") != "tool v2\n"
        || read_test_file(tool_target / "usr/share/data/foreign") != "user data\n") {
        sage::util::log_error("Reinstall destroyed a declared directory with foreign content");
        return 1;
    }

    sage::util::log_success("9. End-to-End `sage install` & `sage remove` to Target Root OK");

    return 0;
}

} // namespace durability
} // namespace sage::tests
