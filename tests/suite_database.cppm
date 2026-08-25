module;
#include <sys/stat.h>
#include <unistd.h>

export module sage.tests.database;

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

namespace database {

// The files table keeps one owner per line so shared directories can carry
// several; sole_owner() sees through that encoding for single-owner paths.
export std::optional<std::string> sole_owner(sage::db::Database& db, std::string_view path) {
    auto owners = db.get_path_owners(path);
    if (!owners || owners->size() != 1) return std::nullopt;
    return std::move(owners->front());
}

export int run_database_rebuild_tests(const std::filesystem::path& temp_dir,
    const std::filesystem::path& extract_root,
    const sage::package::PackageManifest& openrc_pkg) {
    // 5. LMDB Database & Rebuild Engine Test
    auto db_dir = temp_dir / "db";
    auto db_res = sage::db::Database::open(db_dir);
    if (!db_res) {
        sage::util::log_error("DB open failed: {}", db_res.error());
        return 1;
    }

    sage::package::PackageManifest escaped_manifest;
    escaped_manifest.name = "escaped-metadata";
    escaped_manifest.version = sage::package::Version::parse("7:1.0.0-1");
    escaped_manifest.description = "quoted \"value\" with \\ slash\nnext line";
    escaped_manifest.dependencies.push_back(
        sage::package::Dependency::parse("runtime-lib >= 2.0"));
    escaped_manifest.provides = {"escaped-metadata", "virtual/escaped"};
    escaped_manifest.conflicts.push_back(
        sage::package::Dependency::parse("legacy-metadata < 2.0"));
    escaped_manifest.conffiles = {"etc/escaped.conf"};
    sage::package::FileEntry escaped_file;
    escaped_file.path = R"(usr/lib/systemd/system/system-systemd\x2dmute.slice)";
    escaped_manifest.files.push_back(std::move(escaped_file));
    auto escaped_round_trip = sage::package::PackageManifest::parse_toml(escaped_manifest.serialize_toml());
    if (!escaped_round_trip
        || escaped_round_trip->version != escaped_manifest.version
        || escaped_round_trip->description != escaped_manifest.description
        || escaped_round_trip->dependencies.size() != 1
        || escaped_round_trip->dependencies.front().to_string() != "runtime-lib >= 2.0-1"
        || escaped_round_trip->provides != escaped_manifest.provides
        || escaped_round_trip->conflicts.size() != 1
        || escaped_round_trip->conflicts.front().to_string() != "legacy-metadata < 2.0-1"
        || escaped_round_trip->conffiles != escaped_manifest.conffiles
        || escaped_round_trip->files.size() != 1
        || escaped_round_trip->files.front().path != escaped_manifest.files.front().path) {
        sage::util::log_error("Package metadata TOML escaping round-trip failed");
        return 1;
    }
    const auto summary_text = escaped_manifest.serialize_summary_toml();
    auto escaped_summary =
        sage::package::PackageManifest::parse_summary_toml(summary_text);
    if (!escaped_summary
        || summary_text.contains("[[files]]")
        || escaped_summary->name != escaped_manifest.name
        || escaped_summary->version != escaped_manifest.version
        || escaped_summary->dependencies.size() != 1
        || escaped_summary->dependencies.front().to_string()
            != escaped_manifest.dependencies.front().to_string()
        || escaped_summary->provides != escaped_manifest.provides
        || !escaped_summary->files.empty()) {
        sage::util::log_error(
            "Package summary serialization retained files or lost solver metadata");
        return 1;
    }

    // Old archives may still contain the removed, inferred build-provenance
    // model. It is deliberately ignored on read and must never be emitted
    // again: repackaging an upstream binary cannot establish who compiled it.
    auto legacy_build_info = sage::package::PackageManifest::parse_toml(R"(
schema_version = 1
[package]
name = "rust-bin"
version = "1.90.0"
release = "1"
compiler = "rustc"
compiler_version = "1.90.0"

[[build_producers]]
name = "rustc"
versions = ["1.90.0"]
flags = "-C opt-level=3"
)");
    if (!legacy_build_info) {
        sage::util::log_error("Legacy manifest with obsolete build information did not parse");
        return 1;
    }
    const auto cleaned_manifest = legacy_build_info->serialize_toml();
    for (const auto marker : {"compiler", "build_producers", "rustc",
                              "opt-level"}) {
        if (cleaned_manifest.contains(marker)) {
            sage::util::log_error(
                "Obsolete inferred build information survived manifest serialization: {}",
                marker);
            return 1;
        }
    }

    auto embedded_epoch_manifest = sage::package::PackageManifest::parse_toml(R"(
schema_version = 1
[package]
name = "embedded-epoch"
version = "1:2.0-3"
)");
    auto embedded_epoch_index = sage::channel::ChannelIndex::parse_toml(R"(
schema_version = 1
[channel]
name = "core"
[[packages]]
name = "embedded-epoch"
version = "1:2.0-3"
)");
    if (!embedded_epoch_manifest
        || embedded_epoch_manifest->version.epoch != 1
        || embedded_epoch_manifest->version.rel != "3"
        || !embedded_epoch_index
        || embedded_epoch_index->available_packages.size() != 1
        || embedded_epoch_index->available_packages.front().version.epoch != 1
        || embedded_epoch_index->available_packages.front().version.rel != "3") {
        sage::util::log_error("Embedded version epoch/release was not preserved");
        return 1;
    }

    auto absent_release_manifest = sage::package::PackageManifest::parse_toml(R"(
schema_version = 1
[package]
name = "absent-release"
version = "1.0"
)");
    for (const std::string_view architecture : {"amd64", "aarch64", "any", "x86_64"}) {
        auto recipe = sage::package::Recipe::parse_toml(std::format(R"(
schema_version = 1
[package]
name = "architecture-test"
version = "1.0.0"
release = "1"
arch = "{}"
)", architecture));
        if (!recipe || recipe->arch != architecture) {
            sage::util::log_error("Valid package architecture '{}' was rejected", architecture);
            return 1;
        }
    }
    auto invalid_architecture = sage::package::Recipe::parse_toml(R"(
schema_version = 1
[package]
name = "architecture-test"
version = "1.0.0"
release = "1"
arch = "mips"
)");
    if (invalid_architecture
        || !sage::package::package_architecture_matches("any", "aarch64")
        || !sage::package::package_architecture_matches("x86_64", "amd64")
        || sage::package::package_architecture_matches("aarch64", "amd64")) {
        sage::util::log_error("Package architecture validation or matching failed");
        return 1;
    }

    auto absent_release_recipe = sage::package::Recipe::parse_toml(R"(
schema_version = 1
[package]
name = "absent-release"
version = "1.0"
)");
    auto absent_release_index = sage::channel::ChannelIndex::parse_toml(R"(
schema_version = 1
[channel]
name = "core"
[[packages]]
name = "absent-release"
version = "1.0"
)");
    if (!absent_release_manifest || absent_release_manifest->version.rel != "1"
        || !absent_release_recipe || absent_release_recipe->version.rel != "1"
        || !absent_release_index || absent_release_index->available_packages.size() != 1
        || absent_release_index->available_packages.front().version.rel != "1") {
        sage::util::log_error("Absent release did not default to one");
        return 1;
    }

    for (std::string_view wrong_type : {"0", "1", "false"}) {
        auto manifest = sage::package::PackageManifest::parse_toml(std::format(R"(
schema_version = 1
[package]
name = "wrong-release-type"
version = "1.0"
release = {}
)", wrong_type));
        auto recipe = sage::package::Recipe::parse_toml(std::format(R"(
schema_version = 1
[package]
name = "wrong-release-type"
version = "1.0"
release = {}
)", wrong_type));
        auto index = sage::channel::ChannelIndex::parse_toml(std::format(R"(
schema_version = 1
[channel]
name = "core"
[[packages]]
name = "wrong-release-type"
version = "1.0"
release = {}
)", wrong_type));
        if (manifest || recipe || index) {
            sage::util::log_error(
                "Package, recipe, or channel parser accepted wrong-typed release {}", wrong_type);
            return 1;
        }
    }

    for (std::string_view invalid : {"0", "alpha", "1x", "-1"}) {
        auto manifest = sage::package::PackageManifest::parse_toml(std::format(R"(
schema_version = 1
[package]
name = "bad-release"
version = "1.0"
release = "{}"
)", invalid));
        auto recipe = sage::package::Recipe::parse_toml(std::format(R"(
schema_version = 1
[package]
name = "bad-release"
version = "1.0"
release = "{}"
)", invalid));
        auto index = sage::channel::ChannelIndex::parse_toml(std::format(R"(
schema_version = 1
[channel]
name = "core"
[[packages]]
name = "bad-release"
version = "1.0"
release = "{}"
)", invalid));
        if (manifest || recipe || index) {
            sage::util::log_error("Package, recipe, or channel parser accepted invalid release '{}'", invalid);
            return 1;
        }
    }
    auto positive_release_recipe = sage::package::Recipe::parse_toml(R"(
schema_version = 1
[package]
name = "positive-release"
version = "1.0"
release = "10"
)");
    if (!positive_release_recipe || positive_release_recipe->version.rel != "10") {
        sage::util::log_error("Recipe parser rejected a positive decimal release");
        return 1;
    }


    sage::package::FileEntry owned_file;
    owned_file.path = "usr/bin/database-owned";
    {
        auto owner_txn = db_res->begin_write_txn();
        if (!owner_txn
            || !db_res->register_files(*owner_txn, "database-owner", "system", {owned_file})
            || !owner_txn->commit()) {
            sage::util::log_error("Failed to create database file ownership fixture");
            return 1;
        }
    }
    sage::package::FileEntry unowned_file;
    unowned_file.path = "usr/bin/database-unowned";
    bool database_conflict_rejected = false;
    {
        auto conflict_txn = db_res->begin_write_txn();
        if (!conflict_txn) {
            sage::util::log_error("Failed to open database file conflict transaction");
            return 1;
        }
        auto conflict_registration = db_res->register_files(
            *conflict_txn, "database-challenger", "system", {unowned_file, owned_file});
        database_conflict_rejected = !conflict_registration;
    }
    if (!database_conflict_rejected
        || sole_owner(*db_res, owned_file.path) != "database-owner:system"
        || sole_owner(*db_res, unowned_file.path)) {
        sage::util::log_error("Database file conflict registration was not atomic");
        return 1;
    }

    // A package identity captured before the writer lock must be revalidated
    // inside that write transaction, and same-name ownership is not sufficient.
    auto migration_db = sage::db::Database::open(temp_dir / "migration-race-db");
    sage::package::FileEntry migration_file;
    migration_file.path = "usr/bin/migration-race";
    sage::package::PackageManifest migration_old;
    migration_old.name = "migration-race";
    migration_old.version = sage::package::Version::parse("1.0.0-1");
    migration_old.channel = "system";
    migration_old.files = {migration_file};
    if (!migration_db) {
        sage::util::log_error("Failed to create migration race database");
        return 1;
    }
    {
        auto setup_txn = migration_db->begin_write_txn();
        if (!setup_txn
            || !migration_db->put_package(*setup_txn, migration_old)
            || !migration_db->register_files(
                *setup_txn, migration_old.name, migration_old.channel, migration_old.files)
            || !setup_txn->commit()) {
            sage::util::log_error("Failed to populate migration race database");
            return 1;
        }
    }
    const auto expected_migration_identity = std::optional{
        sage::package::package_identity(migration_old)};
    auto migration_new = migration_old;
    migration_new.version = sage::package::Version::parse("2.0.0-1");
    migration_new.channel = "runtime/python:3.12";
    {
        auto concurrent_txn = migration_db->begin_write_txn();
        if (!concurrent_txn
            || !migration_db->unregister_files(
                *concurrent_txn, migration_old.files, "migration-race:system")
            || !migration_db->put_package(*concurrent_txn, migration_new)
            || !migration_db->register_files(
                *concurrent_txn, migration_new.name, migration_new.channel, migration_new.files)
            || !concurrent_txn->commit()) {
            sage::util::log_error("Failed to simulate concurrent channel migration");
            return 1;
        }
    }
    {
        auto install_txn = migration_db->begin_write_txn();
        const std::string stale_owner = "migration-race:system";
        auto stale_snapshot = install_txn
            ? load_install_snapshot(
                *migration_db,
                *install_txn,
                migration_old.name,
                expected_migration_identity)
            : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
                std::unexpected("transaction failed"));
        auto stale_owner_check = install_txn
            ? migration_db->check_file_conflicts(
                *install_txn,
                std::optional<std::string_view>{stale_owner},
                migration_new.files)
            : std::expected<void, std::string>(std::unexpected("transaction failed"));
        if (stale_snapshot || stale_owner_check) {
            sage::util::log_error("Concurrent same-name channel migration bypassed identity revalidation");
            return 1;
        }
    }
    auto preserved_migration = migration_db->get_package(migration_new.name);
    if (!preserved_migration || !*preserved_migration
        || sage::package::package_identity(**preserved_migration)
            != sage::package::package_identity(migration_new)
        || sole_owner(*migration_db, migration_file.path)
            != "migration-race:runtime/python:3.12") {
        sage::util::log_error("Rejected stale migration changed the concurrent package state");
        return 1;
    }
    // Split-package upgrades hand files between package identities inside a
    // single install transaction (issue #26): when foo-libs claims the
    // library, the monolithic foo's claim is still on record. The projected
    // transaction ownership tolerates exactly that handover, while every
    // owner that survives the transaction stays fatal.
    {
        auto handover_db = sage::db::Database::open(temp_dir / "split-handover-db");
        if (!handover_db) {
            sage::util::log_error("Failed to create split handover database");
            return 1;
        }
        sage::package::FileEntry handover_bin;
        handover_bin.path = "usr/bin/handover";
        sage::package::FileEntry handover_lib;
        handover_lib.path = "usr/lib/libhandover.so";
        sage::package::PackageManifest handover_monolith;
        handover_monolith.name = "handover";
        handover_monolith.version = sage::package::Version::parse("1.0.0-1");
        handover_monolith.channel = "system";
        handover_monolith.files = {handover_bin, handover_lib};
        {
            auto setup_txn = handover_db->begin_write_txn();
            if (!setup_txn
                || !handover_db->put_package(*setup_txn, handover_monolith)
                || !handover_db->register_files(
                    *setup_txn, handover_monolith.name,
                    handover_monolith.channel, handover_monolith.files)
                || !setup_txn->commit()) {
                sage::util::log_error("Failed to populate split handover fixture");
                return 1;
            }
        }
        const std::string monolith_owner = "handover:system";
        bool strict_claim_rejected = false;
        bool wrong_releasor_rejected = false;
        bool handover_accepted = false;
        bool kept_path_allowed = false;
        bool surviving_owner_fatal = false;
        {
            auto txn = handover_db->begin_write_txn();
            if (!txn) {
                sage::util::log_error("Failed to open split handover transaction");
                return 1;
            }
            // Without handover context the previous owner's claim is fatal...
            strict_claim_rejected = !handover_db->register_files(
                *txn, "handover-libs", "system", {handover_lib});
            // ...a release attributed to a bystander opens no door either...
            wrong_releasor_rejected = !handover_db->register_files(
                *txn, "handover-libs", "system", {handover_lib},
                std::nullopt,
                sage::db::ReleasedClaims{
                    {std::string{handover_lib.path}, {"bystander"}}});
            // ...and the genuine handover goes through, reowning the path.
            auto handover_registration = handover_db->register_files(
                *txn, "handover-libs", "system", {handover_lib},
                std::nullopt,
                sage::db::ReleasedClaims{
                    {std::string{handover_lib.path}, {"handover"}}});
            handover_accepted = handover_registration.has_value();
            if (!handover_accepted || !txn->commit()) {
                sage::util::log_error("Failed to commit split handover registration");
                return 1;
            }
        }
        if (!strict_claim_rejected || !wrong_releasor_rejected || !handover_accepted
            || sole_owner(*handover_db, handover_lib.path) != "handover-libs:system") {
            sage::util::log_error("In-transaction file handover conflict semantics were wrong");
            return 1;
        }
        {
            auto txn = handover_db->begin_write_txn();
            if (!txn) {
                sage::util::log_error("Failed to reopen split handover transaction");
                return 1;
            }
            // The upgrading monolith keeps its own remaining paths through the
            // allowed-owner rule, but a path the new owner holds is still fatal.
            kept_path_allowed = handover_db->check_file_conflicts(
                *txn,
                std::optional<std::string_view>{monolith_owner},
                {handover_bin}).has_value();
            surviving_owner_fatal = !handover_db->check_file_conflicts(
                *txn,
                std::optional<std::string_view>{monolith_owner},
                {handover_lib}).has_value();
        }
        if (!kept_path_allowed || !surviving_owner_fatal
            || sole_owner(*handover_db, handover_bin.path) != monolith_owner) {
            sage::util::log_error("Allowed-owner rule broke during the handover regression");
            return 1;
        }
    }

    auto corrupt_target = temp_dir / "corrupt-target";
    auto corrupt_db_dir = corrupt_target / "var/lib/sage";
    {
        auto raw_env = sage::vendor::lmdb::MdbEnv::create(corrupt_db_dir);
        if (!raw_env) {
            sage::util::log_error("Failed to create corrupt database fixture");
            return 1;
        }
        auto raw_txn = sage::vendor::lmdb::MdbTxn::begin(*raw_env);
        if (!raw_txn) {
            sage::util::log_error("Failed to begin corrupt database fixture transaction");
            return 1;
        }
        auto raw_packages = sage::vendor::lmdb::MdbDbi::open(
            *raw_txn, "packages", sage::vendor::lmdb::flag_create);
        auto raw_files = sage::vendor::lmdb::MdbDbi::open(
            *raw_txn, "files", sage::vendor::lmdb::flag_create);
        if (!raw_packages || !raw_files
            || !raw_packages->put(
                *raw_txn, "aaa-mismatch", "[package]\nname = \"different-name\"\nversion = \"1.0.0\"\n")
            || !raw_packages->put(*raw_txn, "broken", "[package]\nname = \"broken\n")
            || !raw_files->put(*raw_txn, "usr/bin/broken", "broken:system")) {
            sage::util::log_error("Failed to populate corrupt database fixture");
            return 1;
        }
        auto raw_commit = raw_txn->commit();
        if (!raw_commit) {
            sage::util::log_error("Failed to commit corrupt database fixture");
            return 1;
        }
    }
    std::filesystem::create_directories(corrupt_target / "etc/sage");
    CliOptions corrupt_install_opts;
    corrupt_install_opts.target_root = corrupt_target;
    corrupt_install_opts.args = {"dummy-tool"};
    if (cmd_install(corrupt_install_opts) == 0) {
        sage::util::log_error("Install accepted a corrupt installed package manifest");
        return 1;
    }
    auto corrupt_db = sage::db::Database::open(corrupt_db_dir, true);
    auto corrupt_package = corrupt_db
        ? corrupt_db->get_package("broken")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected("database open failed"));
    auto mismatched_package = corrupt_db
        ? corrupt_db->get_package("aaa-mismatch")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected("database open failed"));
    if (!corrupt_db
        || corrupt_db->list_installed_packages()
        || corrupt_package
        || mismatched_package
        || sole_owner(*corrupt_db, "usr/bin/broken") != "broken:system") {
        sage::util::log_error("Corrupt manifest failure changed existing file ownership");
        return 1;
    }

    sage::config::SystemConfig sys_cfg;
    sys_cfg.providers["virtual/init"] = "openrc";
    sys_cfg.capabilities["virtual/init"] = sage::config::CapabilityKind::Exclusive;
    sys_cfg.cache_dir = temp_dir / "cache";
    auto read_reconcile_file = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };

    // Reconcile installs provider packages for real now, so back it with a
    // local file:// channel whose openrc archive carries an actual payload.
    auto reconcile_repo = temp_dir / "reconcile-repo";
    auto openrc_pkg_dir = temp_dir / "openrc-pkg";
    std::filesystem::create_directories(openrc_pkg_dir / "usr/bin");
    std::filesystem::create_directories(openrc_pkg_dir / "etc/init.d");
    {
        std::ofstream openrc_bin(openrc_pkg_dir / "usr/bin/openrc");
        openrc_bin << "#!/bin/sh\nexit 0\n";
        std::ofstream openrc_init(openrc_pkg_dir / "etc/init.d/openrc");
        openrc_init << "#!/bin/sh\n# native openrc fixture\n";
    }
    std::filesystem::create_directories(reconcile_repo);
    if (!sage::archive::create_package(openrc_pkg, openrc_pkg_dir,
            reconcile_repo / "openrc-0.54.0-1-x86_64.pkg.tar.zst")) {
        sage::util::log_error("Failed to pack the reconcile fixture archive");
        return 1;
    }
    {
        std::ofstream idx(reconcile_repo / "index.toml");
        idx << "schema_version = 1\n\n[channel]\nname = \"reconcile-test\"\n\n[[packages]]\n"
            << "name = \"openrc\"\nversion = \"0.54.0\"\nrelease = \"1\"\n"
            << "channel = \"system\"\narch = \"x86_64\"\n"
            << "file = \"openrc-0.54.0-1-x86_64.pkg.tar.zst\"\n";
    }
    sage::config::ChannelConfig reconcile_channel;
    reconcile_channel.name = "reconcile-test";
    reconcile_channel.url = "file://" + reconcile_repo.string();
    reconcile_channel.enabled = true;
    sys_cfg.channels.push_back(reconcile_channel);

    auto plan_res = sage::rebuild::ReconcileEngine::calculate_diff(*db_res, sys_cfg);
    if (!plan_res) {
        sage::util::log_error("Reconcile plan failed: {}", plan_res.error());
        return 1;
    }
    auto exec_res = sage::rebuild::ReconcileEngine::execute(*db_res, *plan_res, extract_root, false);
    if (!exec_res || !std::filesystem::exists(extract_root / "usr/bin/openrc")
        || read_reconcile_file(extract_root / "etc/init.d/openrc")
            != "#!/bin/sh\n# native openrc fixture\n") {
        sage::util::log_error("Reconcile execute did not install the new provider payload: {}",
            exec_res.error_or("payload missing"));
        return 1;
    }

    // A reconcile plan must not remove a same-name package that changed before
    // its write transaction began. Preserve both the newer package and the old
    // provider lock so the caller can recalculate from a fresh snapshot.
    auto reconcile_race_db = sage::db::Database::open(temp_dir / "reconcile-race-db");
    sage::package::PackageManifest old_init;
    old_init.name = "old-init";
    old_init.version = sage::package::Version::parse("1.0.0-1");
    old_init.provides = {"old-init", "virtual/init"};
    if (!reconcile_race_db) {
        sage::util::log_error("Failed to create reconcile snapshot fixture");
        return 1;
    }
    {
        auto setup_txn = reconcile_race_db->begin_write_txn();
        if (!setup_txn
            || !reconcile_race_db->put_package(*setup_txn, old_init)
            || !reconcile_race_db->set_system_provider(*setup_txn, "virtual/init", old_init.name)
            || !setup_txn->commit()) {
            sage::util::log_error("Failed to populate reconcile snapshot fixture");
            return 1;
        }
    }
    auto stale_plan = sage::rebuild::ReconcileEngine::calculate_diff(
        *reconcile_race_db, sys_cfg);
    auto replacement_init = old_init;
    replacement_init.version = sage::package::Version::parse("2.0.0-1");
    {
        auto update_txn = reconcile_race_db->begin_write_txn();
        if (!stale_plan || !update_txn
            || !reconcile_race_db->put_package(*update_txn, replacement_init)
            || !update_txn->commit()) {
            sage::util::log_error("Failed to update reconcile snapshot fixture");
            return 1;
        }
    }
    auto stale_execute = sage::rebuild::ReconcileEngine::execute(
        *reconcile_race_db, *stale_plan, extract_root, false);
    auto preserved_init = reconcile_race_db->get_package(old_init.name);
    auto preserved_provider = reconcile_race_db->get_system_provider("virtual/init");
    if (stale_execute
        || !preserved_init || !*preserved_init
        || (**preserved_init).version != replacement_init.version
        || !preserved_provider || !*preserved_provider
        || **preserved_provider != old_init.name) {
        sage::util::log_error("Reconcile executed against a stale package snapshot");
        return 1;
    }

    // A provider lock can also change without changing the package record.
    // Reject the stale plan before it overwrites that newer binding.
    {
        auto reset_txn = reconcile_race_db->begin_write_txn();
        if (!reset_txn
            || !reconcile_race_db->put_package(*reset_txn, old_init)
            || !reconcile_race_db->set_system_provider(*reset_txn, "virtual/init", old_init.name)
            || !reset_txn->commit()) {
            sage::util::log_error("Failed to reset reconcile provider fixture");
            return 1;
        }
    }
    auto stale_provider_plan = sage::rebuild::ReconcileEngine::calculate_diff(
        *reconcile_race_db, sys_cfg);
    {
        auto update_txn = reconcile_race_db->begin_write_txn();
        if (!stale_provider_plan || !update_txn
            || !reconcile_race_db->set_system_provider(
                *update_txn, "virtual/init", "concurrent-init")
            || !update_txn->commit()) {
            sage::util::log_error("Failed to update reconcile provider fixture");
            return 1;
        }
    }
    auto stale_provider_execute = sage::rebuild::ReconcileEngine::execute(
        *reconcile_race_db, *stale_provider_plan, extract_root, false);
    auto concurrent_provider = reconcile_race_db->get_system_provider("virtual/init");
    auto provider_package = reconcile_race_db->get_package(old_init.name);
    if (stale_provider_execute
        || !concurrent_provider || !*concurrent_provider
        || **concurrent_provider != "concurrent-init"
        || !provider_package || !*provider_package
        || (**provider_package).version != old_init.version) {
        sage::util::log_error("Reconcile overwrote a concurrently changed provider lock");
        return 1;
    }
    sage::util::log_success("5. Declarative System Reconcile & Triggers Engine (sage rebuild) OK");

    return 0;
}

} // namespace database
} // namespace sage::tests
