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

    // 3b. Diamond Dependency Backtracking Test
    // Scenario:
    // Root -> App
    // App -> LibA >= 1.0.0, LibB >= 1.0.0
    // LibA 2.0.0 (latest) -> LibC >= 2.0.0
    // LibA 1.0.0 (older)  -> LibC = 1.0.0
    // LibB 1.0.0          -> LibC < 2.0.0
    // Greedy DFS would choose LibA 2.0.0 first and fail when LibB demands LibC < 2.0.0.
    // PubGrub CDCL solver must backtrack, learn the incompatibility, and resolve with LibA 1.0.0!
    {
        std::vector<sage::package::PackageManifest> diamond_pool;
        sage::package::PackageManifest libc1, libc2, liba1, liba2, libb1, root_app;
        
        libc1.name = "libc-core";
        libc1.version = sage::package::Version::parse("1.0.0-1");

        libc2.name = "libc-core";
        libc2.version = sage::package::Version::parse("2.0.0-1");

        liba1.name = "liba";
        liba1.version = sage::package::Version::parse("1.0.0-1");
        liba1.dependencies.push_back(sage::package::Dependency::parse("libc-core = 1.0.0-1"));

        liba2.name = "liba";
        liba2.version = sage::package::Version::parse("2.0.0-1");
        liba2.dependencies.push_back(sage::package::Dependency::parse("libc-core >= 2.0.0"));

        libb1.name = "libb";
        libb1.version = sage::package::Version::parse("1.0.0-1");
        libb1.dependencies.push_back(sage::package::Dependency::parse("libc-core < 2.0.0"));

        root_app.name = "diamond-app";
        root_app.version = sage::package::Version::parse("1.0.0-1");
        root_app.dependencies.push_back(sage::package::Dependency::parse("liba >= 1.0.0"));
        root_app.dependencies.push_back(sage::package::Dependency::parse("libb >= 1.0.0"));

        diamond_pool.push_back(libc1);
        diamond_pool.push_back(libc2);
        diamond_pool.push_back(liba1);
        diamond_pool.push_back(liba2);
        diamond_pool.push_back(libb1);
        diamond_pool.push_back(root_app);

        sage::solver::DependencySolver diamond_solver(diamond_pool);
        auto diamond_res = diamond_solver.solve({sage::package::Dependency::parse("diamond-app")});
        if (!diamond_res) {
            sage::util::log_error("Diamond backtracking test failed to find solution: {}", diamond_res.error());
            return 1;
        }
        // Verify that LibA 1.0.0 and LibC 1.0.0 were selected
        auto a_it = std::ranges::find(*diamond_res, "liba", &sage::package::PackageManifest::name);
        auto c_it = std::ranges::find(*diamond_res, "libc-core", &sage::package::PackageManifest::name);
        if (a_it == diamond_res->end() || a_it->version.to_string() != "1.0.0-1"
            || c_it == diamond_res->end() || c_it->version.to_string() != "1.0.0-1") {
            sage::util::log_error("Diamond backtracking resolved wrong package version combination");
            return 1;
        }
        sage::util::log_success("3b. PubGrub CDCL Conflict Backtracking & Resolution OK");
    }

    // 3c. Unsolvable Conflict Diagnostic Tree Test
    {
        std::vector<sage::package::PackageManifest> conflict_pool;
        sage::package::PackageManifest p1;
        p1.name = "pkg-x";
        p1.version = sage::package::Version::parse("1.0.0-1");
        p1.dependencies.push_back(sage::package::Dependency::parse("pkg-y >= 2.0.0"));

        sage::package::PackageManifest p2;
        p2.name = "pkg-y";
        p2.version = sage::package::Version::parse("1.0.0-1");

        conflict_pool.push_back(p1);
        conflict_pool.push_back(p2);

        sage::solver::DependencySolver conflict_solver(conflict_pool);
        auto conflict_res = conflict_solver.solve({sage::package::Dependency::parse("pkg-x")});
        if (conflict_res) {
            sage::util::log_error("Conflict solver unexpectedly succeeded for unsatisfiable graph");
            return 1;
        }
        if (conflict_res.error().find("Conflict") == std::string::npos
            && conflict_res.error().find("conflict") == std::string::npos) {
            sage::util::log_error("Conflict diagnostic did not produce expected error tree: {}", conflict_res.error());
            return 1;
        }
        sage::util::log_success("3c. PubGrub CDCL Conflict Diagnostic Tree OK");
    }

    // 3d. Versioned Provides Match & Fallback Prevention Test
    {
        std::vector<sage::package::PackageManifest> prov_pool;
        sage::package::PackageManifest glibc;
        glibc.name = "glibc";
        glibc.version = sage::package::Version::parse("2.45.0-1");
        glibc.provides = {"virtual/libc = 2.44.0-1"}; // explicitly older provides than glibc version

        sage::package::PackageManifest musl;
        musl.name = "musl";
        musl.version = sage::package::Version::parse("1.2.5-1");
        musl.provides = {"virtual/libc = 2.46.0-1"};

        sage::package::PackageManifest app;
        app.name = "libc-app";
        app.version = sage::package::Version::parse("1.0.0-1");
        app.dependencies.push_back(sage::package::Dependency::parse("virtual/libc >= 2.45.0"));

        prov_pool.push_back(glibc);
        prov_pool.push_back(musl);
        prov_pool.push_back(app);

        sage::solver::DependencySolver prov_solver(prov_pool);
        auto prov_res = prov_solver.solve({sage::package::Dependency::parse("libc-app")});
        if (!prov_res) {
            sage::util::log_error("Versioned provides solver test failed: {}", prov_res.error());
            return 1;
        }
        // musl must be selected because glibc's explicit provides is 2.44.0-1 (< 2.45.0)
        auto musl_it = std::ranges::find(*prov_res, "musl", &sage::package::PackageManifest::name);
        auto glibc_it = std::ranges::find(*prov_res, "glibc", &sage::package::PackageManifest::name);
        if (musl_it == prov_res->end() || glibc_it != prov_res->end()) {
            sage::util::log_error("Versioned provides incorrectly fell back to glibc package version");
            return 1;
        }
        sage::util::log_success("3d. Versioned Provides Exact Match & Fallback Prevention OK");
    }

    // 3e. Package Explicit Conflicts SAT Resolution Test
    {
        std::vector<sage::package::PackageManifest> conf_pool;
        sage::package::PackageManifest p_a, p_b, p_alt_b, p_root;
        p_a.name = "pkg-alpha";
        p_a.version = sage::package::Version::parse("1.0.0-1");
        p_a.conflicts.push_back(sage::package::Dependency::parse("pkg-beta-bad"));

        p_b.name = "pkg-beta-bad";
        p_b.version = sage::package::Version::parse("2.0.0-1");
        p_b.provides = {"service-beta"};

        p_alt_b.name = "pkg-beta-good";
        p_alt_b.version = sage::package::Version::parse("1.0.0-1");
        p_alt_b.provides = {"service-beta"};

        p_root.name = "app-combined";
        p_root.version = sage::package::Version::parse("1.0.0-1");
        p_root.dependencies.push_back(sage::package::Dependency::parse("pkg-alpha"));
        p_root.dependencies.push_back(sage::package::Dependency::parse("service-beta"));

        conf_pool.push_back(p_a);
        conf_pool.push_back(p_b);
        conf_pool.push_back(p_alt_b);
        conf_pool.push_back(p_root);

        sage::solver::DependencySolver conf_solver(conf_pool);
        auto conf_res = conf_solver.solve({sage::package::Dependency::parse("app-combined")});
        if (!conf_res) {
            sage::util::log_error("Explicit conflicts solver test failed: {}", conf_res.error());
            return 1;
        }
        auto good_it = std::ranges::find(*conf_res, "pkg-beta-good", &sage::package::PackageManifest::name);
        auto bad_it = std::ranges::find(*conf_res, "pkg-beta-bad", &sage::package::PackageManifest::name);
        if (good_it == conf_res->end() || bad_it != conf_res->end()) {
            sage::util::log_error("Explicit conflicts failed to exclude conflicting package in favor of alternative provider");
            return 1;
        }
        sage::util::log_success("3e. Explicit Package Conflicts SAT Resolution OK");
    }

    return 0;
}

} // namespace solver
} // namespace sage::tests
