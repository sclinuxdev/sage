module;
#include <sys/stat.h>
#include <unistd.h>

export module sage.tests.solver;

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

namespace solver {
export int run_solver_tests(sage::package::PackageManifest& openrc_pkg) {
    // 3. PubGrub Dependency Solver & Toolchain MSRV Resolution Test
    std::vector<sage::package::PackageManifest> repo_pool;
    sage::package::PackageManifest libfoo;
    libfoo.name = "libfoo";
    libfoo.version = sage::package::Version::parse("2.1.0-1");
    libfoo.provides = {"libfoo", "so:libfoo.so.2"};

    sage::package::PackageManifest gcc15;
    gcc15.name = "gcc";
    gcc15.version = sage::package::Version::parse("15.3.0-1");
    gcc15.channel = "toolchain/gcc:15";
    gcc15.provides = {"cc", "c++", "gcc", "toolchain/gcc"};

    sage::package::PackageManifest app;
    app.name = "demo-app";
    app.version = sage::package::Version::parse("1.0.0-1");
    app.dependencies.push_back(sage::package::Dependency::parse("libfoo >= 2.0.0"));
    app.dependencies.push_back(sage::package::Dependency::parse("toolchain/gcc >= 14.0"));

    openrc_pkg = sage::package::PackageManifest{};
    openrc_pkg.name = "openrc";
    openrc_pkg.version = sage::package::Version::parse("0.54.0-1");
    openrc_pkg.provides = {"openrc", "virtual/init"};
    openrc_pkg.service_toml = R"(schema_version = 1
[service]
name = "openrc"
description = "OpenRC native-unit ownership canary"
exec_start = "/usr/bin/openrc"
)";

    repo_pool.push_back(libfoo);
    repo_pool.push_back(gcc15);
    repo_pool.push_back(app);
    repo_pool.push_back(openrc_pkg);

    sage::solver::DependencySolver solver(repo_pool);
    auto solve_res = solver.solve({sage::package::Dependency::parse("demo-app")});
    if (!solve_res || solve_res->size() != 3) {
        sage::util::log_error("Dependency & Toolchain MSRV solver test failed (resolved size: {})", solve_res ? solve_res->size() : 0);
        return 1;
    }
    auto dependency_before = [&](std::string_view dependency, std::string_view dependent) {
        auto dependency_it = std::ranges::find(*solve_res, dependency, &sage::package::PackageManifest::name);
        auto dependent_it = std::ranges::find(*solve_res, dependent, &sage::package::PackageManifest::name);
        return dependency_it != solve_res->end()
            && dependent_it != solve_res->end()
            && dependency_it < dependent_it;
    };
    if (!dependency_before("libfoo", "demo-app") || !dependency_before("gcc", "demo-app")) {
        sage::util::log_error("Dependency solver did not return dependency-first install order");
        return 1;
    }
    sage::util::log_success("3. Native PubGrub / CDCL SAT Dependency & MSRV Solver OK");

    return 0;
}

} // namespace solver
} // namespace sage::tests
