module;
#include <sys/stat.h>
#include <unistd.h>

export module sage.tests.transactions;

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

namespace transactions {

export int run_lock_dry_run_tests(const std::filesystem::path& temp_dir,
    const std::filesystem::path& local_repo, const std::filesystem::path& isolated_target,
    const CliOptions& inst_opts, const CliOptions& rem_opts,
    bool (*write_test_channel)(const std::filesystem::path&, const std::filesystem::path&),
    std::string (*read_test_file)(const std::filesystem::path&)) {
    // 12. Host operation lock and zero-write dry-run protocol.
    {
        // Lock provisioning chowns state to root, and the install/remove
        // dry-run protocol gates on root as well. CI covers this scenario
        // fully as root; local non-root runs skip rather than fail spuriously.
        if (sage::util::current_effective_uid() != 0) {
            sage::util::log_warn(
                "12. Global Operation Lock & Zero-Write Dry-Run Protocol SKIPPED (requires root)");
            goto scenario_12_done;
        }
        const auto operation_lock_root = temp_dir / "operation-lock-host";
        const auto operation_lock_path = operation_lock_root / "sage/operation.lock";
        std::filesystem::create_directory(operation_lock_root);
        std::filesystem::permissions(
            operation_lock_root,
            std::filesystem::perms::owner_all
                | std::filesystem::perms::group_read
                | std::filesystem::perms::group_exec
                | std::filesystem::perms::others_read
                | std::filesystem::perms::others_exec,
            std::filesystem::perm_options::replace);

        auto non_root = validate_operation_user(1000);
        if (non_root || !validate_operation_user(0)) {
            sage::util::log_error("Package-state operation root requirement failed");
            return 1;
        }

        // Shared previews coexist, but their root-only file lock excludes the first
        // writer just as it excludes an established writer.
        {
            auto shared_a = sage::util::OperationLock::acquire(
                operation_lock_path, sage::util::LockMode::Shared);
            auto shared_b = sage::util::OperationLock::acquire(
                operation_lock_path, sage::util::LockMode::Shared);
            auto blocked_writer = sage::util::OperationLock::acquire(
                operation_lock_path, sage::util::LockMode::Exclusive);
            if (!shared_a || !shared_b || blocked_writer
                || blocked_writer.error().kind != sage::util::LockFailure::Busy) {
                sage::util::log_error("SH/SH coexistence or SH-to-EX exclusion failed");
                return 1;
            }
        }

        const auto public_metadata = sage::util::snapshot_file_metadata(
            operation_lock_root);
        const auto namespace_metadata = sage::util::snapshot_file_metadata(
            operation_lock_path.parent_path());
        const auto lock_metadata = sage::util::snapshot_file_metadata(operation_lock_path);
        if (!public_metadata || !namespace_metadata || !lock_metadata
            || !std::filesystem::is_directory(operation_lock_root)
            || !std::filesystem::is_directory(operation_lock_path.parent_path())
            || !std::filesystem::is_regular_file(operation_lock_path)
            || public_metadata->owner_uid != 0
            || public_metadata->owner_gid != 0
            || public_metadata->mode != 0755
            || namespace_metadata->owner_uid != 0
            || namespace_metadata->owner_gid != 0
            || namespace_metadata->mode != 0700
            || lock_metadata->owner_uid != 0
            || lock_metadata->owner_gid != 0
            || lock_metadata->mode != 0600) {
            sage::util::log_error("Operation lock namespace is not securely provisioned");
            return 1;
        }

        // An exclusive writer excludes both modes, then release of its fd makes
        // the same inode immediately available again.
        {
            auto exclusive = sage::util::OperationLock::acquire(
                operation_lock_path, sage::util::LockMode::Exclusive);
            auto blocked_reader = sage::util::OperationLock::acquire(
                operation_lock_path, sage::util::LockMode::Shared);
            auto blocked_writer = sage::util::OperationLock::acquire(
                operation_lock_path, sage::util::LockMode::Exclusive);
            if (!exclusive || blocked_reader || blocked_writer
                || blocked_reader.error().kind != sage::util::LockFailure::Busy
                || blocked_writer.error().kind != sage::util::LockFailure::Busy) {
                sage::util::log_error("EX did not exclude SH and EX contenders");
                return 1;
            }
        }
        if (!sage::util::OperationLock::acquire(
                operation_lock_path, sage::util::LockMode::Shared)) {
            sage::util::log_error("Operation lock was not reacquirable after release");
            return 1;
        }

        // The caller supplies one absolute deadline. Repeated EWOULDBLOCK
        // results consume it instead of starting a fresh wait on each retry.
        {
            auto exclusive = sage::util::OperationLock::acquire(
                operation_lock_path, sage::util::LockMode::Exclusive);
            const auto started = std::chrono::steady_clock::now();
            const auto deadline = started + std::chrono::milliseconds(150);
            auto waited = sage::util::OperationLock::acquire_until(
                operation_lock_path, sage::util::LockMode::Shared, deadline);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            if (!exclusive || waited
                || waited.error().kind != sage::util::LockFailure::Busy
                || elapsed < std::chrono::milliseconds(100)
                || elapsed > std::chrono::milliseconds(500)) {
                sage::util::log_error("Operation lock wait did not honor its absolute deadline");
                return 1;
            }
        }

        // Fatal open/flock failures keep their real classification and reason.
        auto missing_lock = sage::util::OperationLock::acquire(
            temp_dir / "no-such-run/sage/operation.lock",
            sage::util::LockMode::Shared);
        const auto wrong_lock_root = temp_dir / "wrong-operation-lock-host";
        const auto wrong_lock_file = wrong_lock_root / "operation.lock";
        std::filesystem::create_directory(wrong_lock_root);
        std::filesystem::permissions(
            wrong_lock_root, std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace);
        std::filesystem::create_directory(wrong_lock_file);
        auto wrong_lock = sage::util::OperationLock::acquire(
            wrong_lock_file, sage::util::LockMode::Shared);
        auto fatal_flock_call = +[](int, int) -> std::expected<void, int> {
            return std::unexpected(static_cast<int>(std::errc::io_error));
        };
        auto fatal_flock = sage::util::OperationLock::acquire_until(
            operation_lock_path,
            sage::util::LockMode::Shared,
            sage::util::OperationLock::deadline_after(0),
            fatal_flock_call);
        if (missing_lock || wrong_lock || fatal_flock
            || missing_lock.error().kind != sage::util::LockFailure::Unusable
            || missing_lock.error().message.find("No such file or directory")
                == std::string::npos
            || wrong_lock.error().kind != sage::util::LockFailure::Unusable
            || fatal_flock.error().kind != sage::util::LockFailure::Unusable
            || fatal_flock.error().message.find("Input/output") == std::string::npos) {
            sage::util::log_error("Fatal operation-lock errors were reported as contention");
            return 1;
        }

        const auto loose_namespace_host = temp_dir / "loose-namespace-host";
        const auto loose_namespace = loose_namespace_host / "sage";
        std::filesystem::create_directory(loose_namespace_host);
        std::filesystem::create_directory(loose_namespace);
        std::filesystem::permissions(
            loose_namespace,
            std::filesystem::perms::owner_all
                | std::filesystem::perms::group_read
                | std::filesystem::perms::group_exec
                | std::filesystem::perms::others_read
                | std::filesystem::perms::others_exec,
            std::filesystem::perm_options::replace);
        auto loose_namespace_lock = sage::util::OperationLock::acquire(
            loose_namespace / "operation.lock", sage::util::LockMode::Shared);
        if (loose_namespace_lock
            || loose_namespace_lock.error().kind != sage::util::LockFailure::Unusable
            || loose_namespace_lock.error().message.find("mode 0700 directory")
                == std::string::npos) {
            sage::util::log_error("Insecure operation lock namespace was accepted");
            return 1;
        }

        const auto loose_lock_root = temp_dir / "loose-operation-lock-host";
        const auto loose_lock_file = loose_lock_root / "operation.lock";
        std::filesystem::create_directory(loose_lock_root);
        std::filesystem::permissions(
            loose_lock_root, std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace);
        std::ofstream(loose_lock_file) << "insecure\n";
        std::filesystem::permissions(
            loose_lock_file,
            std::filesystem::perms::owner_read
                | std::filesystem::perms::owner_write
                | std::filesystem::perms::group_read
                | std::filesystem::perms::others_read,
            std::filesystem::perm_options::replace);
        if (sage::util::OperationLock::acquire(
                loose_lock_file, sage::util::LockMode::Shared)) {
            sage::util::log_error("Insecure operation lock mode was accepted");
            return 1;
        }

        // Probe classification: only ENOENT is absent; permissions and wrong
        // entry types remain errors. symlink_status also rejects redirected
        // target roots and data.mdb entries.
        auto absent_probe = classify_path_probe(
            "missing", {}, std::make_error_code(std::errc::no_such_file_or_directory),
            std::filesystem::file_type::regular, "fixture");
        auto permission_probe = classify_path_probe(
            "denied", {}, std::make_error_code(std::errc::permission_denied),
            std::filesystem::file_type::regular, "fixture");
        auto wrong_type_probe = classify_path_probe(
            "directory", std::filesystem::file_status{std::filesystem::file_type::directory}, {},
            std::filesystem::file_type::regular, "fixture");
        if (!absent_probe || *absent_probe || permission_probe || wrong_type_probe
            || permission_probe.error().find("Permission denied") == std::string::npos
            || wrong_type_probe.error().find("wrong file type") == std::string::npos) {
            sage::util::log_error("Database probe error classification failed");
            return 1;
        }

        // A configured *.mdb name denotes an LMDB environment directory; both
        // Database::open and the synchronized probe must resolve its real
        // data.mdb file through the same helper.
        {
            const auto configured_db_path = temp_dir / "custom-lmdb/state.mdb";
            const auto resolved_db_paths = sage::db::resolve_lmdb_paths(configured_db_path);
            auto custom_db = sage::db::Database::open(configured_db_path);
            auto custom_probe = probe_package_database(configured_db_path);
            if (!custom_db || !custom_probe || !*custom_probe
                || resolved_db_paths.environment != configured_db_path.parent_path()
                || resolved_db_paths.data_file != configured_db_path.parent_path() / "data.mdb"
                || resolved_db_paths.lock_file != configured_db_path.parent_path() / "lock.mdb"
                || std::filesystem::exists(configured_db_path)
                || !std::filesystem::is_regular_file(resolved_db_paths.data_file)
                || !std::filesystem::is_regular_file(resolved_db_paths.lock_file)) {
                sage::util::log_error("Custom LMDB path probe/open resolution diverged");
                return 1;
            }
        }

        // An absent target can be previewed without creating the root. The held
        // SH lock also excludes the first real writer.
        const auto absent_root = temp_dir / "dry-run-absent-root";
        {
            CliOptions absent_opts;
            absent_opts.target_root = absent_root;
            absent_opts.args = {"dummy-tool"};
            absent_opts.dry_run = true;
            auto context = acquire_operation_context(absent_opts, operation_lock_path);
            auto first_writer = sage::util::OperationLock::acquire(
                operation_lock_path, sage::util::LockMode::Exclusive);
            if (!context
                || context->target_root_snapshot != TargetRootSnapshot::Absent
                || context->database_snapshot != DatabaseSnapshot::Absent
                || first_writer
                || first_writer.error().kind != sage::util::LockFailure::Busy
                || cmd_remove(absent_opts, context->database_snapshot) != 0
                || cmd_rebuild(absent_opts, context->database_snapshot) == 0
                || std::filesystem::exists(absent_root)) {
                sage::util::log_error("Absent-root dry-run created state or admitted a writer");
                return 1;
            }
        }
        if (std::filesystem::exists(absent_root)) {
            sage::util::log_error("Absent-root preview persisted a target path");
            return 1;
        }

        // A present root with no DB uses an empty installed set. Install parses
        // the channel in memory, remove is a no-op, and rebuild is explicit.
        const auto empty_root = temp_dir / "dry-run-empty-db-root";
        if (!write_test_channel(empty_root, local_repo)) {
            sage::util::log_error("Failed to create empty-DB dry-run fixture");
            return 1;
        }
        {
            CliOptions dry_install;
            dry_install.target_root = empty_root;
            dry_install.args = {"dummy-tool"};
            dry_install.dry_run = true;
            auto context = acquire_operation_context(dry_install, operation_lock_path);
            if (!context
                || context->target_root_snapshot != TargetRootSnapshot::Present
                || context->database_snapshot != DatabaseSnapshot::Absent
                || cmd_install(dry_install, std::nullopt, context->database_snapshot) != 0
                || cmd_remove(dry_install, context->database_snapshot) != 0
                || cmd_rebuild(dry_install, context->database_snapshot) == 0) {
                sage::util::log_error("Absent-database dry-run semantics failed");
                return 1;
            }
        }
        if (std::filesystem::exists(empty_root / "var/lib/sage")
            || std::filesystem::exists(empty_root / "var/cache/sage")
            || std::filesystem::exists(empty_root / "usr/bin/dummy")) {
            sage::util::log_error("Empty-database dry-run persisted target state");
            return 1;
        }

        // The operation context is a frozen snapshot. Even if a test bypasses
        // the protocol and creates a directory-shaped data.mdb afterwards, an
        // Absent command path neither re-probes nor opens it.
        const auto frozen_root = temp_dir / "frozen-absent-snapshot-root";
        if (!write_test_channel(frozen_root, local_repo)) return 1;
        {
            CliOptions frozen;
            frozen.target_root = frozen_root;
            frozen.args = {"dummy-tool"};
            frozen.dry_run = true;
            auto context = acquire_operation_context(frozen, operation_lock_path);
            if (!context || context->database_snapshot != DatabaseSnapshot::Absent) return 1;
            std::filesystem::create_directories(frozen_root / "var/lib/sage/data.mdb");
            if (cmd_install(frozen, std::nullopt, context->database_snapshot) != 0
                || cmd_remove(frozen, context->database_snapshot) != 0) {
                sage::util::log_error("Commands re-probed a frozen Absent database snapshot");
                return 1;
            }
        }

        // Wrong target/data types and ENOTDIR are never treated as an empty DB.
        const auto wrong_db_root = temp_dir / "wrong-db-type-root";
        std::filesystem::create_directories(wrong_db_root / "var/lib/sage/data.mdb");
        CliOptions wrong_db;
        wrong_db.target_root = wrong_db_root;
        wrong_db.dry_run = true;
        if (acquire_operation_context(wrong_db, operation_lock_path)) {
            sage::util::log_error("Directory-shaped data.mdb was treated as absent");
            return 1;
        }
        const auto symlink_db_root = temp_dir / "symlink-db-root";
        std::filesystem::create_directories(symlink_db_root / "var/lib/sage");
        std::ofstream(temp_dir / "regular-db-decoy") << "not LMDB\n";
        std::filesystem::create_symlink(
            temp_dir / "regular-db-decoy", symlink_db_root / "var/lib/sage/data.mdb");
        wrong_db.target_root = symlink_db_root;
        if (acquire_operation_context(wrong_db, operation_lock_path)) {
            sage::util::log_error("Symlink data.mdb was accepted by the synchronized probe");
            return 1;
        }
        const auto enotdir_root = temp_dir / "enotdir-root";
        std::filesystem::create_directory(enotdir_root);
        std::ofstream(enotdir_root / "var") << "not a directory\n";
        wrong_db.target_root = enotdir_root;
        if (acquire_operation_context(wrong_db, operation_lock_path)) {
            sage::util::log_error("ENOTDIR database path was treated as absent");
            return 1;
        }
        const auto symlink_root = temp_dir / "symlink-root";
        std::filesystem::create_directory(temp_dir / "real-root");
        std::filesystem::create_directory_symlink(temp_dir / "real-root", symlink_root);
        wrong_db.target_root = symlink_root;
        if (acquire_operation_context(wrong_db, operation_lock_path)) {
            sage::util::log_error("Symlink target root was accepted by the synchronized probe");
            return 1;
        }

        // Seed a normal DB and package, then prove every relevant byte and
        // size/mtime/ctime remains stable across all three previews. atime is
        // intentionally excluded because reads may update it.
        if (cmd_install(inst_opts) != 0) {
            sage::util::log_error("Failed to seed present-DB dry-run fixture");
            return 1;
        }
        {
            auto fixture_cfg = sage::config::SystemConfig::load_from_root(isolated_target);
            auto fixture_db = sage::db::Database::open(
                isolated_target / "var/lib/sage/data.mdb");
            if (!fixture_cfg || !fixture_db) return 1;
            auto txn = fixture_db->begin_write_txn();
            if (!txn) return 1;
            for (const auto& [iface, provider] : fixture_cfg->exclusive_providers()) {
                auto stored = fixture_db->set_system_provider(*txn, iface, provider);
                if (!stored) return 1;
            }
            if (!txn->commit()) return 1;
        }
        const auto legacy_pid_lock = isolated_target / "var/lib/sage/lock";
        std::ofstream(legacy_pid_lock) << "legacy-pid-sentinel\n";
        const std::vector<std::filesystem::path> watched_files{
            isolated_target / "var/lib/sage/data.mdb",
            isolated_target / "var/lib/sage/lock.mdb",
            legacy_pid_lock,
            isolated_target / "var/cache/sage/channels/core.toml",
            isolated_target / "usr/bin/dummy",
        };
        std::map<std::filesystem::path,
            std::pair<std::string, sage::util::FileMetadataSnapshot>> before;
        for (const auto& path : watched_files) {
            auto metadata = sage::util::snapshot_file_metadata(path);
            if (!metadata) {
                sage::util::log_error("Missing present-DB dry-run fixture '{}': {}",
                    path.string(), metadata.error());
                return 1;
            }
            before.emplace(path, std::pair{read_test_file(path), *metadata});
        }
        {
            CliOptions dry_install = inst_opts;
            dry_install.dry_run = true;
            auto context = acquire_operation_context(dry_install, operation_lock_path);
            if (!context || context->database_snapshot != DatabaseSnapshot::Present
                || cmd_install(dry_install, std::nullopt, context->database_snapshot) != 0) {
                sage::util::log_error("Present-DB install preview failed");
                return 1;
            }
            CliOptions dry_remove = rem_opts;
            dry_remove.dry_run = true;
            if (cmd_remove(dry_remove, context->database_snapshot) != 0) {
                sage::util::log_error("Present-DB remove preview failed");
                return 1;
            }
            CliOptions dry_rebuild;
            dry_rebuild.target_root = isolated_target;
            dry_rebuild.dry_run = true;
            if (cmd_rebuild(dry_rebuild, context->database_snapshot) != 0) {
                sage::util::log_error("Present-DB rebuild preview failed");
                return 1;
            }
        }
        for (const auto& [path, expected] : before) {
            auto metadata = sage::util::snapshot_file_metadata(path);
            if (!metadata || *metadata != expected.second
                || read_test_file(path) != expected.first) {
                sage::util::log_error("Dry-run modified content or metadata for '{}'", path.string());
                return 1;
            }
        }

        // A real EX-held operation still initializes a fresh root, and another
        // EX-held operation mutates the established state normally.
        const auto fresh_root = temp_dir / "fresh-operation-root";
        CliOptions fresh_install;
        fresh_install.target_root = fresh_root;
        fresh_install.args = {"dummy-tool"};
        {
            auto context = acquire_operation_context(fresh_install, operation_lock_path);
            if (!context
                || context->target_root_snapshot != TargetRootSnapshot::Absent
                || context->database_snapshot != DatabaseSnapshot::Absent
                || !write_test_channel(fresh_root, local_repo)
                || cmd_install(fresh_install, std::nullopt, context->database_snapshot) != 0) {
                sage::util::log_error("Real operation failed to initialize a fresh target root");
                return 1;
            }
        }
        if (!std::filesystem::is_regular_file(fresh_root / "var/lib/sage/data.mdb")
            || !std::filesystem::exists(fresh_root / "usr/bin/dummy")
            || std::filesystem::exists(fresh_root / "var/lib/sage/lock")) {
            sage::util::log_error("Fresh mutation did not create the expected package state");
            return 1;
        }
        CliOptions fresh_remove;
        fresh_remove.target_root = fresh_root;
        fresh_remove.args = {"dummy-tool"};
        {
            auto context = acquire_operation_context(fresh_remove, operation_lock_path);
            if (!context || context->database_snapshot != DatabaseSnapshot::Present
                || cmd_remove(fresh_remove, context->database_snapshot) != 0) {
                sage::util::log_error("Real operation failed to mutate established state");
                return 1;
            }
        }
        if (std::filesystem::exists(fresh_root / "usr/bin/dummy")) {
            sage::util::log_error("Real remove left the installed package file behind");
            return 1;
        }
    }
scenario_12_done:
    sage::util::log_success("12. Global Operation Lock & Zero-Write Dry-Run Protocol OK");

    return 0;
}

export int run_conffile_protection_tests() {
    // 14. Conffile Protection: reinstall keeps locally modified configuration
    {
        auto temp_dir = std::filesystem::temp_directory_path() / "sage_conffile_test";
        std::filesystem::remove_all(temp_dir);
        std::filesystem::create_directories(temp_dir / "etc/sage");
        std::ofstream(temp_dir / "etc/sage/system.toml") << "schema_version = 1\n";
        auto conf_dir = temp_dir / "confpkg";
        std::filesystem::create_directories(conf_dir);
        auto read_text = [](const std::filesystem::path& p) {
            std::ifstream f(p);
            std::stringstream ss;
            ss << f.rdbuf();
            return ss.str();
        };
        {
            std::ofstream recipe(conf_dir / "recipe.toml");
            recipe << R"(schema_version = 1
[package]
name = "confpkg"
version = "3.0.0"
release = "1"
description = "conffile protection canary"
license = "MIT"
channel = "system"
conffiles = ["/etc/myapp.conf", "/etc/plain.conf"]
install = [
    'mkdir -p "$DESTDIR/etc"',
    'printf "v1\n" > "$DESTDIR/etc/myapp.conf"',
    'printf "v1\n" > "$DESTDIR/etc/plain.conf"',
]
)";
        }

        // Build once; the archive manifest must carry the declaration.
        CliOptions build_opts;
        build_opts.args = {conf_dir.string()};
        build_opts.target_root = temp_dir;
        if (cmd_build(build_opts) != 0) {
            sage::util::log_error("Failed to build confpkg");
            return 1;
        }
        auto conf_inspect = sage::archive::inspect_package(
            conf_dir / "confpkg-3.0.0-1-x86_64.pkg.tar.zst");
        const std::vector<std::string> expected_conffiles{"/etc/myapp.conf", "/etc/plain.conf"};
        if (!conf_inspect || conf_inspect->manifest.conffiles != expected_conffiles) {
            sage::util::log_error("Built archive manifest does not carry the conffiles declaration");
            return 1;
        }
        // Fresh install through a local channel: both files land normally.
        auto repo_dir = temp_dir / "repo";
        std::filesystem::create_directories(repo_dir);
        std::filesystem::copy_file(
            conf_dir / "confpkg-3.0.0-1-x86_64.pkg.tar.zst",
            repo_dir / "confpkg-3.0.0-1-x86_64.pkg.tar.zst",
            std::filesystem::copy_options::overwrite_existing);
        if (!sage::archive::generate_repo_index(repo_dir, "core")) {
            sage::util::log_error("Failed to index conffile test repo");
            return 1;
        }
        auto conf_root = temp_dir / "target";
        std::filesystem::create_directories(conf_root / "etc/sage");
        {
            std::ofstream chan(conf_root / "etc/sage/channels.toml");
            chan << "schema_version = 1\n\n[[channels]]\nname = \"core\"\nurl = \"file://"
                 << repo_dir.string() << "\"\nscope = \"system\"\npriority = 100\nenabled = true\n";
        }
        CliOptions install_opts;
        install_opts.target_root = conf_root;
        // Reinstall goes through the archive argument so it bypasses the
        // same-version no-op filter: a plain name request for an already
        // satisfied version resolves to nothing by design.
        const auto conf_archive_path = repo_dir / "confpkg-3.0.0-1-x86_64.pkg.tar.zst";
        install_opts.args = {conf_archive_path.string()};
        if (cmd_install(install_opts) != 0) {
            sage::util::log_error("Fresh confpkg install failed");
            return 1;
        }
        if (read_text(conf_root / "etc/myapp.conf") != "v1\n"
            || read_text(conf_root / "etc/plain.conf") != "v1\n") {
            sage::util::log_error("Conffiles missing after fresh install");
            return 1;
        }

        // The admin edits one of them, then the same version is reinstalled.
        { std::ofstream f(conf_root / "etc/myapp.conf"); f << "admin-edited"; }
        if (cmd_install(install_opts) != 0) {
            sage::util::log_error("Reinstall over modified config failed");
            return 1;
        }
        if (read_text(conf_root / "etc/myapp.conf") != "admin-edited"
            || read_text(conf_root / "etc/myapp.conf.new") != "v1\n") {
            sage::util::log_error("Modified conffile was not protected across reinstall");
            return 1;
        }
        // The untouched conffile is overwritten in place -- no stray .new.
        if (read_text(conf_root / "etc/plain.conf") != "v1\n"
            || std::filesystem::exists(conf_root / "etc/plain.conf.new")) {
            sage::util::log_error("Unmodified conffile was not cleanly replaced");
            return 1;
        }

        std::filesystem::remove_all(temp_dir);
        sage::util::log_success("14. Conffile Protection on Reinstall OK");
    }

    return 0;
}

} // namespace transactions
} // namespace sage::tests
