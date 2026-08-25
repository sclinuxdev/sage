export module sage.tests;

// Master architecture & subsystem integration suite (`sage-tests` binary).
// Drives the public engine surface end to end; the raw LMDB probes verify the
// on-disk key layout itself, which is exactly what a regression must pin.
//
// This unit is a thin facade: each subsystem's checks live in a focused
// sage.tests.<subsystem> module unit re-exported below, and `run_all()` merely
// orchestrates them in the original section order, propagating the first
// failure.
import std;
import sage;
import sage.cli;

export import sage.tests.core;
export import sage.tests.provenance;
export import sage.tests.solver;
export import sage.tests.service;
export import sage.tests.database;
export import sage.tests.channels;
export import sage.tests.query;
export import sage.tests.install;
export import sage.tests.durability;
export import sage.tests.transactions;
export import sage.tests.build;

namespace sage::tests {

using sage::cli::CliOptions;

// The development image need not carry the distribution's fakeroot package.
// This adapter proves that every build command crosses Sage's fakeroot
// execution boundary; real fakeroot behaviour remains the package's own
// integration responsibility.
class FakerootTestAdapter {
    std::filesystem::path directory_;
    std::string old_path_;
    bool ready_{false};

public:
    FakerootTestAdapter() {
        const char* path = sage::util::get_env("PATH");
        old_path_ = path ? path : "";
        directory_ = std::filesystem::temp_directory_path()
            / std::format("sage-fakeroot-test-{}", sage::util::current_pid());
        std::error_code ec;
        std::filesystem::create_directories(directory_, ec);
        if (ec) return;

        const auto executable = directory_ / "fakeroot";
        std::ofstream script(executable);
        script << R"(#!/bin/sh
if [ "$1" = "--version" ]; then
    echo "fakeroot test adapter 1.0"
    exit 0
fi
if [ "$1" = "--" ]; then shift; fi
export SAGE_TEST_FAKEROOT_ACTIVE=1
exec "$@"
)";
        script.close();
        if (!script) return;
        std::filesystem::permissions(executable,
            std::filesystem::perms::owner_read
                | std::filesystem::perms::owner_write
                | std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::replace, ec);
        if (ec) return;
        ready_ = sage::util::set_env(
            "PATH", directory_.string() + ":" + old_path_);
    }

    ~FakerootTestAdapter() {
        sage::util::set_env("PATH", old_path_);
        std::error_code ec;
        std::filesystem::remove_all(directory_, ec);
    }

    [[nodiscard]] bool ready() const noexcept { return ready_; }
};

export int run_all() {
    sage::util::log_info("Running Sage Master Architecture & Subsystem Integration Test Suite...");

    FakerootTestAdapter fakeroot;
    if (!fakeroot.ready()) {
        sage::util::log_error("Cannot create the fakeroot execution adapter");
        return 1;
    }

    // Original section order: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15.

    // Shared fixtures: artifacts that an earlier section creates and later
    // sections consume, threaded through explicitly so each partition stays a
    // pure function of its inputs. The archive scratch root is created by the
    // provenance section and shared read-mostly until final cleanup.
    std::filesystem::path temp_dir;
    std::filesystem::path dummy_pkg_path;
    sage::package::PackageManifest dummy_manifest;
    std::filesystem::path extract_root;
    sage::package::PackageManifest openrc_fixture;
    std::filesystem::path local_repo;
    std::filesystem::path isolated_target;
    CliOptions inst_opts;
    CliOptions rem_opts;
    bool (*write_test_channel)(const std::filesystem::path&, const std::filesystem::path&) = nullptr;
    std::string (*read_test_file)(const std::filesystem::path&) = nullptr;
    std::vector<std::string> (*host_trigger_args)(
        const std::vector<std::filesystem::path>&, const std::filesystem::path&) = nullptr;

    if (core::run_versioning_tests() != 0) return 1;
    if (provenance::run_archive_integrity_tests(temp_dir, dummy_pkg_path, dummy_manifest, extract_root) != 0) return 1;
    if (solver::run_solver_tests(openrc_fixture) != 0) return 1;
    if (service::run_service_generation_tests(extract_root) != 0) return 1;
    if (database::run_database_rebuild_tests(temp_dir, extract_root, openrc_fixture) != 0) return 1;
    if (channels::run_channel_swap_tests(extract_root) != 0) return 1;
    if (service::run_ephemeral_shell_tests(extract_root) != 0) return 1;
    if (query::run_local_repo_index_tests(temp_dir, dummy_pkg_path, dummy_manifest, local_repo) != 0) return 1;
    if (install::run_e2e_install_remove_tests(temp_dir, local_repo, isolated_target,
            inst_opts, rem_opts, write_test_channel, read_test_file,
            database::sole_owner, host_trigger_args) != 0) return 1;
    if (durability::run_durable_install_tests(temp_dir, write_test_channel, read_test_file,
            database::sole_owner, host_trigger_args) != 0) return 1;
    if (install::run_closed_loop_tests(temp_dir) != 0) return 1;
    if (transactions::run_lock_dry_run_tests(temp_dir, local_repo, isolated_target,
            inst_opts, rem_opts, write_test_channel, read_test_file) != 0) return 1;
    if (build::run_build_config_tests(temp_dir) != 0) return 1;
    if (transactions::run_conffile_protection_tests() != 0) return 1;
    if (build::run_multisource_fetch_tests() != 0) return 1;

    std::filesystem::remove_all(temp_dir);
    sage::util::log_success("🎉 All Sage Master Architecture & Subsystem Integration Tests Passed Successfully!");
    return 0;
}
} // namespace sage::tests
