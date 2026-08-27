module;
#include <sys/stat.h>
#include <unistd.h>

export module sage.tests.install;

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

namespace install {


export int run_e2e_install_remove_tests(const std::filesystem::path& temp_dir,
    const std::filesystem::path& local_repo, std::filesystem::path& isolated_target,
    CliOptions& inst_opts, CliOptions& rem_opts,
    bool (*&write_test_channel)(const std::filesystem::path&, const std::filesystem::path&),
    std::string (*&read_test_file)(const std::filesystem::path&),
    std::optional<std::string> (*sole_owner)(sage::db::Database&, std::string_view),
    std::vector<std::string> (*&host_trigger_args)(
        const std::vector<std::filesystem::path>&, const std::filesystem::path&)) {
    // 9. End-to-End `sage install` & `sage remove` into isolated Target Root Test
    isolated_target = temp_dir / "target_root";
    std::filesystem::create_directories(isolated_target / "etc/sage");
    std::ofstream chan_f(isolated_target / "etc/sage/channels.toml");
    chan_f << "schema_version = 1\n\n[[channels]]\nname = \"core\"\nurl = \"file://" << local_repo.string() << "\"\nscope = \"system\"\npriority = 100\nenabled = true\n";
    chan_f.close();

    inst_opts = CliOptions{};
    inst_opts.target_root = isolated_target;
    inst_opts.args = {"dummy-tool"};
    int inst_ret = cmd_install(inst_opts);
    if (inst_ret != 0 || !std::filesystem::exists(isolated_target / "usr/bin/dummy")) {
        sage::util::log_error("End-to-end sage install to target root failed");
        return 1;
    }

    rem_opts = CliOptions{};
    rem_opts.target_root = isolated_target;
    rem_opts.args = {"dummy-tool"};
    int rem_ret = cmd_remove(rem_opts);
    if (rem_ret != 0 || std::filesystem::exists(isolated_target / "usr/bin/dummy")) {
        sage::util::log_error("End-to-end sage remove from target root failed");
        return 1;
    }

    write_test_channel = [](const std::filesystem::path& target,
                                 const std::filesystem::path& repo) {
        std::filesystem::create_directories(target / "etc/sage");
        std::ofstream channels(target / "etc/sage/channels.toml");
        channels
            << "schema_version = 1\n\n[[channels]]\nname = \"core\"\nurl = \"file://"
            << repo.string()
            << "\"\nscope = \"system\"\npriority = 100\nenabled = true\n";
        return channels.good();
    };
    read_test_file = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };
    // Trigger timing fixtures execute on the host with sysroot "/". Their
    // commands touch only this test's temporary directory, so the suite does
    // not need a compiler or static libc merely to build a chroot-local probe.
    host_trigger_args = [](const std::vector<std::filesystem::path>& required,
                           const std::filesystem::path& counter) {
        std::string script;
        for (const auto& path : required) {
            script += std::format("test -e \"{}\" && ", path.string());
        }
        script += std::format("echo x >> \"{}\"", counter.string());
        return std::vector<std::string>{"-c", "'" + script + "'"};
    };


    // A solver selection, direct archive request, and extracted archive must
    // all refer to the same complete package identity.
    auto version_repo = temp_dir / "version-repo";
    auto version_1_data = temp_dir / "version-1-data";
    auto version_2_data = temp_dir / "version-2-data";
    std::filesystem::create_directories(version_repo);
    std::filesystem::create_directories(version_1_data / "usr/bin");
    std::filesystem::create_directories(version_1_data / "usr/share/version-trigger");
    std::filesystem::create_directories(version_2_data / "usr/bin");
    std::filesystem::create_directories(version_2_data / "usr/share/version-trigger");
    std::ofstream(version_1_data / "usr/bin/versioned") << "version 1\n";
    std::ofstream(version_1_data / "usr/share/version-trigger/libversioned.so")
        << "version 1 library\n";
    std::ofstream(version_2_data / "usr/bin/versioned") << "version 2\n";
    std::ofstream(version_2_data / "usr/share/version-trigger/libversioned.so")
        << "version 2 library\n";

    sage::package::PackageManifest version_1;
    version_1.name = "versioned-package";
    version_1.version = sage::package::Version::parse("1.0.0-1");
    sage::package::Trigger version_trigger;
    version_trigger.name = "version-cache";
    version_trigger.on_paths = {"usr/share/version-trigger/"};
    version_trigger.exec = "/bin/sh";
    auto version_trigger_count = temp_dir / "version-trigger-count";
    version_trigger.args = host_trigger_args({}, version_trigger_count);
    version_1.triggers = {version_trigger};
    sage::package::PackageManifest version_2 = version_1;
    version_2.version = sage::package::Version::parse("2.0.0-1");
    version_2.triggers.clear();
    auto version_1_pkg = version_repo / "versioned-package-1.0.0-1-x86_64.pkg.tar.zst";
    auto version_2_pkg = version_repo / "versioned-package-2.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(version_1, version_1_data, version_1_pkg)
        || !sage::archive::create_package(version_2, version_2_data, version_2_pkg)
        || !sage::archive::generate_repo_index(version_repo, "core")) {
        sage::util::log_error("Failed to create package identity fixtures");
        return 1;
    }

    auto version_target = temp_dir / "version-target";
    if (!write_test_channel(version_target, version_repo)) {
        sage::util::log_error("Failed to write package identity test channel");
        return 1;
    }
    CliOptions version_install;
    version_install.target_root = version_target;
    version_install.args = {"versioned-package"};
    if (cmd_install(version_install, "/") != 0
        || read_test_file(version_target / "usr/bin/versioned") != "version 2\n") {
        sage::util::log_error("Solver selection did not install the selected archive version");
        return 1;
    }
    auto version_db = sage::db::Database::open(version_target / "var/lib/sage/data.mdb", true);
    auto selected_version = version_db
        ? version_db->get_package("versioned-package")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected(std::string{"database open failed"}));
    if (!selected_version || !*selected_version
        || (**selected_version).version != version_2.version) {
        sage::util::log_error("Installed database version differs from extracted archive version");
        return 1;
    }

    // Equal identities from multiple sources must keep solver metadata and the
    // extracted archive bound to the same highest-priority source.
    auto high_priority_repo = temp_dir / "source-high";
    auto low_priority_repo = temp_dir / "source-low";
    auto high_priority_data = temp_dir / "source-high-data";
    auto low_priority_data = temp_dir / "source-low-data";
    std::filesystem::create_directories(high_priority_repo);
    std::filesystem::create_directories(low_priority_repo);
    std::filesystem::create_directories(high_priority_data / "usr/bin");
    std::filesystem::create_directories(low_priority_data / "usr/bin");
    std::ofstream(high_priority_data / "usr/bin/source-bound") << "high priority\n";
    std::ofstream(low_priority_data / "usr/bin/source-bound") << "low priority\n";
    sage::package::PackageManifest source_bound;
    source_bound.name = "source-bound";
    source_bound.version = sage::package::Version::parse("1.0.0-1");
    auto high_priority_archive =
        high_priority_repo / "source-bound-1.0.0-1-x86_64.pkg.tar.zst";
    auto low_priority_archive =
        low_priority_repo / "source-bound-1.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(
            source_bound, high_priority_data, high_priority_archive)
        || !sage::archive::create_package(
            source_bound, low_priority_data, low_priority_archive)
        || !sage::archive::generate_repo_index(high_priority_repo, "high")
        || !sage::archive::generate_repo_index(low_priority_repo, "low")) {
        sage::util::log_error("Failed to create multi-source identity fixtures");
        return 1;
    }
    auto source_target = temp_dir / "source-target";
    std::filesystem::create_directories(source_target / "etc/sage");
    std::ofstream source_channels(source_target / "etc/sage/channels.toml");
    source_channels
        << "schema_version = 1\n\n"
        << "[[channels]]\nname = \"low\"\nurl = \"file://"
        << low_priority_repo.string()
        << "\"\nscope = \"system\"\npriority = 10\nenabled = true\n\n"
        << "[[channels]]\nname = \"high\"\nurl = \"file://"
        << high_priority_repo.string()
        << "\"\nscope = \"system\"\npriority = 100\nenabled = true\n";
    source_channels.close();
    CliOptions source_install;
    source_install.target_root = source_target;
    source_install.args = {source_bound.name};
    if (cmd_install(source_install) != 0
        || read_test_file(source_target / "usr/bin/source-bound") != "high priority\n") {
        sage::util::log_error("Solver metadata and archive source were not bound together");
        return 1;
    }

    auto direct_target = temp_dir / "direct-version-target";
    if (!write_test_channel(direct_target, version_repo)) {
        sage::util::log_error("Failed to write direct archive test channel");
        return 1;
    }
    CliOptions direct_install;
    direct_install.target_root = direct_target;
    direct_install.args = {version_1_pkg.string()};
    if (cmd_install(direct_install, "/") != 0
        || read_test_file(direct_target / "usr/bin/versioned") != "version 1\n"
        || read_test_file(version_trigger_count) != "x\n") {
        sage::util::log_error("Direct archive install was not locked to its exact version");
        return 1;
    }
    {
        auto direct_db = sage::db::Database::open(
            direct_target / "var/lib/sage/data.mdb", true);
        auto direct_version = direct_db
            ? direct_db->get_package("versioned-package")
            : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
                std::unexpected(std::string{"database open failed"}));
        if (!direct_version || !*direct_version
            || (**direct_version).version != version_1.version) {
            sage::util::log_error("Direct archive manifest identity was not preserved in the database");
            return 1;
        }
    }

    // A direct archive may intentionally rebuild the same identity with a
    // different payload. Paths dropped by that rebuild must be removed from
    // both the target root and the ownership database.
    auto same_identity_data = temp_dir / "same-identity-data";
    std::filesystem::create_directories(same_identity_data / "usr/bin");
    std::ofstream(same_identity_data / "usr/bin/replacement")
        << "same identity replacement\n";
    auto same_identity_archive =
        temp_dir / "versioned-package-same-identity.pkg.tar.zst";
    if (!sage::archive::create_package(
            version_1, same_identity_data, same_identity_archive)) {
        sage::util::log_error("Failed to create same-identity reinstall fixture");
        return 1;
    }
    direct_install.args = {same_identity_archive.string()};
    if (cmd_install(direct_install, "/") != 0
        || std::filesystem::exists(direct_target / "usr/bin/versioned")
        || std::filesystem::exists(
            direct_target / "usr/share/version-trigger/libversioned.so")
        || read_test_file(direct_target / "usr/bin/replacement")
            != "same identity replacement\n"
        || read_test_file(version_trigger_count) != "x\nx\n") {
        sage::util::log_error("Same-identity reinstall left a stale payload path");
        return 1;
    }
    auto same_identity_db = sage::db::Database::open(
        direct_target / "var/lib/sage/data.mdb", true);
    if (!same_identity_db) {
        sage::util::log_error("Failed to open same-identity reinstall database");
        return 1;
    }
    if (sole_owner(*same_identity_db, "usr/bin/versioned")
        || sole_owner(*same_identity_db, "usr/share/version-trigger/libversioned.so")
        || sole_owner(*same_identity_db, "usr/bin/replacement")
            != "versioned-package:system") {
        sage::util::log_error("Same-identity reinstall left stale file ownership");
        return 1;
    }

    // A normal same-package upgrade may replace files already owned by that
    // package, while still selecting the exact newer archive.
    direct_install.args = {"versioned-package"};
    if (cmd_install(direct_install, "/") != 0
        || read_test_file(direct_target / "usr/bin/versioned") != "version 2\n") {
        sage::util::log_error("Same-package upgrade was blocked or extracted the wrong archive");
        return 1;
    }
    auto upgraded_db = sage::db::Database::open(
        direct_target / "var/lib/sage/data.mdb", true);
    auto upgraded_version = upgraded_db
        ? upgraded_db->get_package("versioned-package")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected(std::string{"database open failed"}));
    if (!upgraded_version || !*upgraded_version
        || (**upgraded_version).version != version_2.version) {
        sage::util::log_error("Same-package upgrade did not record the extracted version");
        return 1;
    }

    // An explicit archive remains exact even when a newer same-name package is
    // installed. This is the local-package downgrade path used for rollback.
    direct_install.args = {version_1_pkg.string()};
    if (cmd_install(direct_install, "/") != 0
        || read_test_file(direct_target / "usr/bin/versioned") != "version 1\n") {
        sage::util::log_error("Direct archive downgrade was silently skipped");
        return 1;
    }
    auto downgraded_db = sage::db::Database::open(
        direct_target / "var/lib/sage/data.mdb", true);
    auto downgraded_version = downgraded_db
        ? downgraded_db->get_package("versioned-package")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected(std::string{"database open failed"}));
    if (!downgraded_version || !*downgraded_version
        || (**downgraded_version).version != version_1.version) {
        sage::util::log_error("Direct archive downgrade did not update installed metadata");
        return 1;
    }

    // The same version from a different architecture/channel is also a distinct
    // direct archive identity, and must replace stale files owned by the old one.
    auto alternate_data = temp_dir / "alternate-identity-data";
    auto alternate_binary = alternate_data / "opt/channels/llvm/42/bin/versioned";
    std::filesystem::create_directories(alternate_binary.parent_path());
    std::ofstream(alternate_binary) << "alternate identity\n";
    sage::package::PackageManifest alternate_identity = version_1;
    alternate_identity.arch = "any";
    alternate_identity.channel = "toolchain/llvm:42";
    auto alternate_archive = temp_dir / "versioned-package-alternate.pkg.tar.zst";
    if (!sage::archive::create_package(
            alternate_identity, alternate_data, alternate_archive)) {
        sage::util::log_error("Failed to create alternate direct archive identity fixture");
        return 1;
    }
    direct_install.args = {alternate_archive.string()};
    if (cmd_install(direct_install, "/") != 0
        || std::filesystem::exists(direct_target / "usr/bin/versioned")
        || read_test_file(direct_target / "opt/channels/llvm/42/bin/versioned")
            != "alternate identity\n") {
        sage::util::log_error("Direct archive identity replacement was silently skipped");
        return 1;
    }
    auto alternate_db = sage::db::Database::open(
        direct_target / "var/lib/sage/data.mdb", true);
    auto alternate_record = alternate_db
        ? alternate_db->get_package("versioned-package")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected(std::string{"database open failed"}));
    if (!alternate_record || !*alternate_record
        || sage::package::package_identity(**alternate_record)
            != sage::package::package_identity(alternate_identity)
        || sole_owner(*alternate_db, "opt/channels/llvm/42/bin/versioned")
            != "versioned-package:toolchain/llvm:42"
        || sole_owner(*alternate_db, "usr/bin/versioned")) {
        sage::util::log_error("Alternate direct archive identity was not recorded");
        return 1;
    }

    auto mismatch_repo = temp_dir / "identity-mismatch-repo";
    std::filesystem::create_directories(mismatch_repo);
    auto mismatch_v1 = mismatch_repo / version_1_pkg.filename();
    auto mismatch_v2 = mismatch_repo / version_2_pkg.filename();
    std::filesystem::copy_file(version_1_pkg, mismatch_v1);
    std::filesystem::copy_file(version_2_pkg, mismatch_v2);
    if (!sage::archive::generate_repo_index(mismatch_repo, "core")) {
        sage::util::log_error("Failed to generate archive identity mismatch index");
        return 1;
    }
    std::filesystem::copy_file(
        version_1_pkg, mismatch_v2, std::filesystem::copy_options::overwrite_existing);
    auto mismatch_target = temp_dir / "identity-mismatch-target";
    if (!write_test_channel(mismatch_target, mismatch_repo)) {
        sage::util::log_error("Failed to write archive identity mismatch channel");
        return 1;
    }
    CliOptions mismatch_install;
    mismatch_install.target_root = mismatch_target;
    mismatch_install.args = {"versioned-package"};
    if (cmd_install(mismatch_install) == 0
        || std::filesystem::exists(mismatch_target / "usr/bin/versioned")) {
        sage::util::log_error("Mismatched selected and archive identities mutated the target root");
        return 1;
    }

    // Different packages must not overwrite the same ordinary file. The first
    // package remains committed because the second fails before extraction.
    auto owner_repo = temp_dir / "owner-conflict-repo";
    auto owner_a_data = temp_dir / "owner-a-data";
    auto owner_b_data = temp_dir / "owner-b-data";
    std::filesystem::create_directories(owner_repo);
    std::filesystem::create_directories(owner_a_data / "usr/bin");
    std::filesystem::create_directories(owner_b_data / "usr/bin");
    std::ofstream(owner_a_data / "usr/bin/shared-file") << "owned by A\n";
    std::ofstream(owner_b_data / "usr/bin/shared-file") << "owned by B\n";
    sage::package::PackageManifest owner_a;
    owner_a.name = "owner-a";
    owner_a.version = sage::package::Version::parse("1.0.0-1");
    sage::package::PackageManifest owner_b;
    owner_b.name = "owner-b";
    owner_b.version = sage::package::Version::parse("1.0.0-1");
    owner_b.dependencies.push_back(sage::package::Dependency::parse("owner-a"));
    auto owner_a_pkg = owner_repo / "owner-a-1.0.0-1-x86_64.pkg.tar.zst";
    auto owner_b_pkg = owner_repo / "owner-b-1.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(owner_a, owner_a_data, owner_a_pkg)
        || !sage::archive::create_package(owner_b, owner_b_data, owner_b_pkg)
        || !sage::archive::generate_repo_index(owner_repo, "core")) {
        sage::util::log_error("Failed to create file ownership conflict fixtures");
        return 1;
    }
    auto owner_target = temp_dir / "owner-conflict-target";
    if (!write_test_channel(owner_target, owner_repo)) {
        sage::util::log_error("Failed to write file ownership conflict channel");
        return 1;
    }
    CliOptions owner_install;
    owner_install.target_root = owner_target;
    owner_install.args = {"owner-b"};
    if (cmd_install(owner_install) == 0) {
        sage::util::log_error("Different packages silently overwrote the same regular file");
        return 1;
    }
    auto owner_db = sage::db::Database::open(owner_target / "var/lib/sage/data.mdb", true);
    auto owner_a_record = owner_db
        ? owner_db->get_package("owner-a")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected(std::string{"database open failed"}));
    auto owner_b_record = owner_db
        ? owner_db->get_package("owner-b")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
                std::unexpected(std::string{"database open failed"}));
    if (!owner_db || !owner_a_record || !*owner_a_record
        || !owner_b_record || *owner_b_record
        || sole_owner(*owner_db, "usr/bin/shared-file") != "owner-a:system"
        || read_test_file(owner_target / "usr/bin/shared-file") != "owned by A\n") {
        sage::util::log_error("File conflict changed the first package or its ownership record");
        return 1;
    }
    // Issue #26 end-to-end: splitting a monolithic package must install --
    // foo -> foo + foo-fw moves usr/lib/firmware/foo/fw.bin between
    // identities inside one transaction instead of colliding with the old
    // revision. (A .so payload would drag in the host ldconfig trigger.)
    auto handover_repo = temp_dir / "split-repo";
    auto foo_monolith_data = temp_dir / "split-monolith-data";
    auto foo_split_bin_data = temp_dir / "split-bin-data";
    auto foo_split_fw_data = temp_dir / "split-fw-data";
    std::filesystem::create_directories(handover_repo);
    std::filesystem::create_directories(foo_monolith_data / "usr/bin");
    std::filesystem::create_directories(foo_monolith_data / "usr/lib/firmware/foo");
    std::filesystem::create_directories(foo_split_bin_data / "usr/bin");
    std::filesystem::create_directories(foo_split_fw_data / "usr/lib/firmware/foo");
    std::ofstream(foo_monolith_data / "usr/bin/foo") << "foo 1\n";
    std::ofstream(foo_monolith_data / "usr/lib/firmware/foo/fw.bin") << "fw 1\n";
    std::ofstream(foo_split_bin_data / "usr/bin/foo") << "foo 2\n";
    std::ofstream(foo_split_fw_data / "usr/lib/firmware/foo/fw.bin") << "fw 2 fw-pkg\n";
    sage::package::PackageManifest foo_monolith;
    foo_monolith.name = "foo";
    foo_monolith.version = sage::package::Version::parse("1.0.0-1");
    sage::package::PackageManifest foo_split = foo_monolith;
    foo_split.version = sage::package::Version::parse("2.0.0-1");
    foo_split.dependencies.push_back(
        sage::package::Dependency::parse("foo-fw >= 2.0.0"));
    sage::package::PackageManifest foo_fw;
    foo_fw.name = "foo-fw";
    foo_fw.version = sage::package::Version::parse("2.0.0-1");
    if (!sage::archive::create_package(
            foo_monolith, foo_monolith_data, handover_repo / "foo-1.0.0-1-x86_64.pkg.tar.zst")
        || !sage::archive::generate_repo_index(handover_repo, "core")) {
        sage::util::log_error("Failed to create monolithic pre-split fixture");
        return 1;
    }
    auto handover_target = temp_dir / "split-target";
    if (!write_test_channel(handover_target, handover_repo)) {
        sage::util::log_error("Failed to write split handover channel");
        return 1;
    }
    CliOptions handover_install;
    handover_install.target_root = handover_target;
    handover_install.args = {"foo"};
    if (cmd_install(handover_install) != 0
        || read_test_file(handover_target / "usr/bin/foo") != "foo 1\n") {
        sage::util::log_error("Monolithic pre-split fixture failed to install");
        return 1;
    }
    if (!sage::archive::create_package(
            foo_split, foo_split_bin_data, handover_repo / "foo-2.0.0-1-x86_64.pkg.tar.zst")
        || !sage::archive::create_package(
            foo_fw, foo_split_fw_data,
            handover_repo / "foo-fw-2.0.0-1-x86_64.pkg.tar.zst")
        || !sage::archive::generate_repo_index(handover_repo, "core")) {
        sage::util::log_error("Failed to create split upgrade fixtures");
        return 1;
    }
    if (cmd_install(handover_install) != 0
        || read_test_file(handover_target / "usr/bin/foo") != "foo 2\n"
        || read_test_file(handover_target / "usr/lib/firmware/foo/fw.bin")
            != "fw 2 fw-pkg\n") {
        sage::util::log_error("Split-package upgrade rejected an in-transaction file handover");
        return 1;
    }
    {
        auto handover_db = sage::db::Database::open(
            handover_target / "var/lib/sage/data.mdb", true);
        if (!handover_db
            || sole_owner(*handover_db, "usr/bin/foo") != "foo:system"
            || sole_owner(*handover_db, "usr/lib/firmware/foo/fw.bin")
                != "foo-fw:system") {
            sage::util::log_error("Split upgrade recorded wrong file ownership");
            return 1;
        }
    }
    sage::util::log_success("   Split-Package Transactional Handover Upgrade OK");
    return 0;
}

export int run_closed_loop_tests(const std::filesystem::path& temp_dir) {
    // 10. Complete Closed-Loop: `sage build` -> `sage repo index` -> `sage install` -> `sage remove` with Orphan Cleanup
    auto build_test_dir = temp_dir / "build_test";
    std::filesystem::create_directories(build_test_dir / "libsample");
    std::filesystem::create_directories(build_test_dir / "sample-app");
    std::filesystem::create_directories(build_test_dir / "repo");
    // Both recipes produce their payload from an `install` phase writing into
    // $DESTDIR. `cmd_build` clears <recipe>/pkg/ before running the phases, so
    // a payload staged there beforehand would be deleted and the package would
    // come out empty; going through the phase is also what exercises the
    // DESTDIR contract these packages are meant to demonstrate.

    // 1. Write libsample recipe
    std::ofstream lib_recipe(build_test_dir / "libsample/recipe.toml");
    lib_recipe << R"(schema_version = 1
[package]
name = "libsample"
version = "2:2.0"
release = "1"
description = "Sample dynamic library"
license = "MIT"
channel = "system"

provides = ["libsample", "so:libsample.so.1"]

install = [
    'mkdir -p "$DESTDIR/usr/share/libsample"',
    'printf "/* libsample fixture */\n" > "$DESTDIR/usr/share/libsample/payload"',
]
)";
    lib_recipe.close();

    // 2. Write sample-app recipe
    std::ofstream app_recipe(build_test_dir / "sample-app/recipe.toml");
    app_recipe << R"(schema_version = 1
[package]
name = "sample-app"
version = "2.0.0"
release = "1"
description = "Sample user application"
license = "GPL-3.0"
channel = "system"

dependencies = ["libsample >= 1.0.0"]

install = [
    'mkdir -p "$DESTDIR/usr/bin"',
    'printf "#!/bin/sh\necho running sample-app\n" > "$DESTDIR/usr/bin/sample-app"',
    'chmod 755 "$DESTDIR/usr/bin/sample-app"',
]
)";
    app_recipe.close();

    // 3. Execute `sage build` on both packages
    auto recipedia = build_test_dir / "recipedia";
    auto recipedia_root = build_test_dir / "builder-root";
    std::filesystem::create_directories(recipedia);
    std::filesystem::create_directories(recipedia_root / "etc/sage");
    auto publish_index = [&](std::string_view packages) {
        std::ofstream index(recipedia / "index.toml");
        index << "schema_version = 1\n[channel]\nname = \"core\"\n" << packages;
        return index.good();
    };
    std::ofstream(recipedia_root / "etc/sage/channels.toml")
        << "schema_version = 1\n[[channels]]\nname = \"core\"\nurl = \"file://"
        << recipedia.string() << "\"\nenabled = true\n";
    if (!publish_index(R"(
[[packages]]
name = "libsample"
version = "2.0"
release = "20"
epoch = 2
channel = "toolchain/foo"
)")) {
        sage::util::log_error("Failed to create cross-channel Recipedia fixture");
        return 1;
    }
    CliOptions build_lib_opts;
    build_lib_opts.args = {(build_test_dir / "libsample").string()};
    build_lib_opts.target_root = recipedia_root;
    if (cmd_build(build_lib_opts) != 0) {
        sage::util::log_error("Failed to build libsample");
        return 1;
    }
    // A package published in another package channel is not this identity,
    // and the local artifact is not a publication. Rebuilding must therefore
    // retain the recipe's release and may replace the local archive.
    if (cmd_build(build_lib_opts) != 0) {
        sage::util::log_error("Failed to rebuild unpublished local libsample");
        return 1;
    }
    auto local_rebuild = sage::archive::inspect_package(
        build_test_dir / "libsample/libsample-2.0-1-x86_64.pkg.tar.zst");
    if (!local_rebuild || local_rebuild->manifest.version.rel != "1") {
        sage::util::log_error("Local or cross-channel publication incorrectly advanced the system release");
        return 1;
    }
    if (!publish_index(R"(
[[packages]]
name = "libsample"
version = "2.0"
release = "1"
epoch = 2
channel = "system"
[[packages]]
name = "libsample"
version = "2.0"
release = "3"
epoch = 2
channel = "system"
[[packages]]
name = "libsample"
version = "2.0"
release = "2"
epoch = 2
channel = "system"
[[packages]]
name = "libsample"
version = "2.0"
release = "99"
epoch = 1
channel = "system"
)")) {
        sage::util::log_error("Failed to publish Recipedia release fixtures");
        return 1;
    }
    if (cmd_build(build_lib_opts) != 0) {
        sage::util::log_error("Failed to rebuild published libsample");
        return 1;
    }
    auto rebuilt_lib = sage::archive::inspect_package(
        build_test_dir / "libsample/libsample-2.0-4-x86_64.pkg.tar.zst");
    if (!rebuilt_lib || rebuilt_lib->manifest.version.rel != "4") {
        sage::util::log_error("Rebuild did not advance beyond the highest published release");
        return 1;
    }

    // A packager may intentionally skip releases. Published 2:2.0-1..3 must
    // not pull an explicitly declared 2:2.0-10 backwards to release 4.
    {
        std::ifstream recipe_in(build_test_dir / "libsample/recipe.toml");
        std::stringstream recipe_text;
        recipe_text << recipe_in.rdbuf();
        auto updated = recipe_text.str();
        auto release = updated.find("release = \"1\"");
        if (release == std::string::npos) {
            sage::util::log_error("Failed to locate declared release in libsample fixture");
            return 1;
        }
        updated.replace(release, std::string_view("release = \"1\"").size(), "release = \"10\"");
        std::ofstream(build_test_dir / "libsample/recipe.toml") << updated;
    }
    if (cmd_build(build_lib_opts) != 0) {
        sage::util::log_error("Failed to build explicitly higher libsample release");
        return 1;
    }
    auto declared_lib = sage::archive::inspect_package(
        build_test_dir / "libsample/libsample-2.0-10-x86_64.pkg.tar.zst");
    if (!declared_lib || declared_lib->manifest.version.rel != "10") {
        sage::util::log_error("Published release lookup regressed an explicitly higher recipe release");
        return 1;
    }
    if (!publish_index(R"(
[[packages]]
name = "libsample"
version = "2.0"
release = "18446744073709551615"
epoch = 2
channel = "system"
)")) {
        sage::util::log_error("Failed to publish exhausted release fixture");
        return 1;
    }
    if (cmd_build(build_lib_opts) == 0) {
        sage::util::log_error("UINT64_MAX published release wrapped or reused a package identity");
        return 1;
    }

    CliOptions build_app_opts;
    build_app_opts.args = {(build_test_dir / "sample-app").string()};
    build_app_opts.target_root = recipedia_root;
    if (cmd_build(build_app_opts) != 0) {
        sage::util::log_error("Failed to build sample-app");
        return 1;
    }

    // Move built packages to repo/. The names carry the arch suffix that
    // `cmd_build` emits; the recipes above declare no `arch`, so both land on
    // the PackageManifest default.
    auto stage_package = [&](const std::filesystem::path& built,
                             const std::filesystem::path& into) -> bool {
        std::error_code ec;
        std::filesystem::copy_file(built, into, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            sage::util::log_error("Failed to stage {} into the test repo: {}",
                built.filename().string(), ec.message());
            return false;
        }
        return true;
    };

    if (!stage_package(build_test_dir / "libsample/libsample-2.0-1-x86_64.pkg.tar.zst",
                       build_test_dir / "repo/libsample-2.0-1-x86_64.pkg.tar.zst")) {
        return 1;
    }
    if (!stage_package(build_test_dir / "sample-app/sample-app-2.0.0-1-x86_64.pkg.tar.zst",
                       build_test_dir / "repo/sample-app-2.0.0-1-x86_64.pkg.tar.zst")) {
        return 1;
    }

    // 4. Generate local repo index
    auto build_idx_res = sage::archive::generate_repo_index(build_test_dir / "repo", "core");
    if (!build_idx_res) {
        sage::util::log_error("Failed to generate index for built packages");
        return 1;
    }

    // 5. Point isolated target to the newly built repo
    auto loop_target = build_test_dir / "target_root";
    std::filesystem::create_directories(loop_target / "etc/sage");
    std::ofstream loop_chan(loop_target / "etc/sage/channels.toml");
    loop_chan << "schema_version = 1\n\n[[channels]]\nname = \"core\"\nurl = \"file://" << (build_test_dir / "repo").string() << "\"\nscope = \"system\"\npriority = 100\nenabled = true\n";
    loop_chan.close();

    // 6. Install sample-app (which requires libsample)
    CliOptions loop_inst_opts;
    loop_inst_opts.target_root = loop_target;
    loop_inst_opts.args = {"sample-app"};
    if (cmd_install(loop_inst_opts, "/") != 0) {
        sage::util::log_error("Failed to install built sample-app");
        return 1;
    }

    // Verify files on disk and packages in LMDB
    if (!std::filesystem::exists(loop_target / "usr/bin/sample-app") || 
        !std::filesystem::exists(loop_target / "usr/share/libsample/payload")) {
        sage::util::log_error("Files from sample-app and libsample were not properly installed to target root");
        return 1;
    }

    // 7. Verify Reverse Dependency Protection: Attempting to remove libsample directly while sample-app depends on it must fail!
    CliOptions direct_lib_rem;
    direct_lib_rem.target_root = loop_target;
    direct_lib_rem.args = {"libsample"};
    if (cmd_remove(direct_lib_rem) == 0) {
        sage::util::log_error("Direct removal of libsample should have been blocked by reverse dependency protection!");
        return 1;
    }

    // 8. Remove with --cascade: should remove both libsample and sample-app!
    direct_lib_rem.cascade = true;
    if (cmd_remove(direct_lib_rem) != 0) {
        sage::util::log_error("Failed to cascaded-remove libsample and sample-app");
        return 1;
    }

    // Verify all files from both sample-app and libsample are gone from disk!
    if (std::filesystem::exists(loop_target / "usr/bin/sample-app") ||
        std::filesystem::exists(loop_target / "usr/share/libsample/payload")) {
        sage::util::log_error("Files still exist on disk after cascade removal");
        return 1;
    }

    // Verify LMDB is clean
    auto loop_db = sage::db::Database::open(loop_target / "var/lib/sage/data.mdb");
    if (!loop_db) {
        sage::util::log_error("LMDB unavailable after cascaded removal");
        return 1;
    }
    auto loop_packages = loop_db->list_installed_packages();
    if (!loop_packages || !loop_packages->empty()) {
        sage::util::log_error("LMDB still contains packages after cascaded removal");
        return 1;
    }

    sage::util::log_success("10. Complete Build -> Index -> Install -> Remove (Auto Orphan Cleanup) Closed-Loop OK");
    sage::util::log_success("11. Reverse Dependency Protection & Cascade Removal Safety Locks OK");

    return 0;
}

} // namespace install
} // namespace sage::tests
