module;
#include <sys/stat.h>
#include <unistd.h>

export module sage.tests.build;

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

namespace build {
export int run_build_config_tests(const std::filesystem::path& temp_dir) {
    // 13. v1 remains command-compatible while v2 is planned entirely by Sage.
    {
        // Pure-model defaults: an empty document yields the built-in baseline,
        // and the shipped /etc/sage/build.toml must keep parsing to exactly
        // that -- the two are kept in lockstep by this assertion.
        auto empty_cfg = sage::config::BuildConfig::parse_toml("");
        auto shipped_cfg = sage::config::BuildConfig::parse_toml(R"(schema_version = 1
fakeroot = "fakeroot"
cc = "clang"
cxx = "clang++"
fallback_cc = "gcc"
fallback_cxx = "g++"
cflags = "-O3 -march=x86-64-v3"
)");
        if (!empty_cfg || !shipped_cfg || *shipped_cfg != *empty_cfg
            || empty_cfg->fakeroot != "fakeroot"
            || empty_cfg->cc != "clang" || empty_cfg->cflags != "-O3 -march=x86-64-v3"
            || !empty_cfg->cxxflags.empty()) {
            sage::util::log_error("BuildConfig defaults or shipped-default parse drifted");
            return 1;
        }
        // compile_jobs is the single-package setting. It inherits legacy jobs
        // only when absent; explicit zero means hardware concurrency.
        auto jobs_cfg = sage::config::BuildConfig::parse_toml(
            "jobs = 4\ncompile_jobs = 2\n");
        auto legacy_jobs_cfg = sage::config::BuildConfig::parse_toml("jobs = 4\n");
        auto auto_jobs_cfg = sage::config::BuildConfig::parse_toml("compile_jobs = 0\n");
        auto invalid_jobs_cfg = sage::config::BuildConfig::parse_toml(
            "compile_jobs = -1\n");
        if (!jobs_cfg || jobs_cfg->jobs != 4 || jobs_cfg->compile_jobs != 2
            || jobs_cfg->configured_compile_jobs() != 2
            || !legacy_jobs_cfg || legacy_jobs_cfg->configured_compile_jobs() != 4
            || !auto_jobs_cfg || auto_jobs_cfg->configured_compile_jobs() != 0
            || invalid_jobs_cfg || empty_cfg->jobs != 0
            || empty_cfg->compile_jobs.has_value()) {
            sage::util::log_error("BuildConfig compile_jobs parsing drifted");
            return 1;
        }

        auto write_build_toml = [&](const std::filesystem::path& root, std::string_view body) {
            std::filesystem::create_directories(root / "etc/sage");
            std::ofstream(root / "etc/sage/system.toml") << "schema_version = 1\n";
            std::ofstream f(root / "etc/sage/build.toml");
            f << body;
            return f.good();
        };
        auto write_canary_recipe = [&](const std::filesystem::path& dir, std::string_view toml_body) {
            std::filesystem::create_directories(dir);
            std::ofstream recipe(dir / "recipe.toml");
            recipe << toml_body;
            return recipe.good();
        };
        auto build_with_root = [&](const std::filesystem::path& recipe_dir,
                                   const std::filesystem::path& target_root,
                                   const std::filesystem::path& extract_dir,
                                   std::string_view pkg_filename)
            -> std::expected<sage::package::PackageManifest, std::string> {
            CliOptions build_opts;
            build_opts.args = {recipe_dir.string()};
            build_opts.target_root = target_root;
            if (cmd_build(build_opts) != 0) {
                return std::unexpected("cmd_build failed for " + recipe_dir.string());
            }
            auto extracted = sage::archive::extract_package(recipe_dir / pkg_filename, extract_dir);
            if (!extracted) return std::unexpected(extracted.error());
            return std::move(extracted->manifest);
        };
        auto read_text = [](const std::filesystem::path& p) {
            std::ifstream f(p);
            std::stringstream ss;
            ss << f.rdbuf();
            return ss.str();
        };

        // (a) The Sage-owned global baseline reaches the v1 recipe shell.
        auto canary_root = temp_dir / "bcfg-target-a";
        auto canary_dir = temp_dir / "bcfg-canary";
        if (!write_build_toml(canary_root, R"(cc = "cc"
cxx = "c++"
fallback_cc = "cc"
fallback_cxx = "c++"
cflags = "-DGLOBAL_CFLAG=1"
)")
            || !write_canary_recipe(canary_dir, R"(schema_version = 1
[package]
name = "flagcanary"
version = "1.0.0"
release = "1"
description = "build-config canary"
license = "MIT"
channel = "system"
install = [
    'mkdir -p "$DESTDIR/usr/share"',
    'printf "int sage_canary;\n" > canary.c',
    '$CC -c canary.c -o "$DESTDIR/usr/share/canary.o"',
    'printf "%s" "$CFLAGS" > "$DESTDIR/usr/share/cflags.txt"',
]
)")) {
            sage::util::log_error("Failed to create build-config canary fixtures");
            return 1;
        }
        auto canary = build_with_root(canary_dir, canary_root, temp_dir / "bcfg-canary-x",
                                      "flagcanary-1.0.0-1-x86_64.pkg.tar.zst");
        if (!canary
            || read_text(temp_dir / "bcfg-canary-x/usr/share/cflags.txt") != "-DGLOBAL_CFLAG=1"
            || read_text(temp_dir / "bcfg-canary-x/usr/share/canary.o").empty()
            || !canary->managed_build_tools.empty()) {
            sage::util::log_error("Global build-config injection failed");
            return 1;
        }

        // (b) The recipe's [build] table replaces the baseline and carries the
        // phase commands themselves.
        auto override_root = temp_dir / "bcfg-target-b";
        auto override_dir = temp_dir / "bcfg-override";
        if (!write_build_toml(override_root, R"(cc = "cc"
fallback_cc = "cc"
cflags = "-DGLOBAL_CFLAG=1"
)")
            || !write_canary_recipe(override_dir, R"(schema_version = 1
[package]
name = "flagoverride"
version = "1.0.0"
release = "1"
description = "build-config override canary"
license = "MIT"
channel = "system"

[build]
cflags = "-DLOCAL_CFLAG=2"
install = [
    'mkdir -p "$DESTDIR/usr/share"',
    'printf "int sage_canary;\n" > canary.c',
    '$CC -c canary.c -o "$DESTDIR/usr/share/canary.o"',
    'printf "%s" "$CFLAGS" > "$DESTDIR/usr/share/cflags.txt"',
]
)")) {
            sage::util::log_error("Failed to create build-config override fixtures");
            return 1;
        }
        auto overridden = build_with_root(override_dir, override_root, temp_dir / "bcfg-override-x",
                                          "flagoverride-1.0.0-1-x86_64.pkg.tar.zst");
        if (!overridden
            || read_text(temp_dir / "bcfg-override-x/usr/share/cflags.txt") != "-DLOCAL_CFLAG=2") {
            sage::util::log_error("Per-recipe [build] override did not replace the global baseline");
            return 1;
        }

        // (c) An unusable primary compiler degrades to the fallback pair.
        auto fallback_root = temp_dir / "bcfg-target-c";
        auto fallback_dir = temp_dir / "bcfg-fallback";
        if (!write_build_toml(fallback_root, R"(cc = "/nonexistent/sage-no-such-cc"
fallback_cc = "cc"
fallback_cxx = "c++"
cflags = "-DFALLBACK_CFLAG=3"
)")
            || !write_canary_recipe(fallback_dir, R"(schema_version = 1
[package]
name = "flagfallback"
version = "1.0.0"
release = "1"
description = "build-config fallback canary"
license = "MIT"
channel = "system"
install = [
    'mkdir -p "$DESTDIR/usr/share"',
    'printf "int sage_canary;\n" > canary.c',
    '$CC -c canary.c -o "$DESTDIR/usr/share/canary.o"',
    'printf "%s" "$CFLAGS" > "$DESTDIR/usr/share/cflags.txt"',
]
)")) {
            sage::util::log_error("Failed to create build-config fallback fixtures");
            return 1;
        }
        auto fallback = build_with_root(fallback_dir, fallback_root, temp_dir / "bcfg-fallback-x",
                                        "flagfallback-1.0.0-1-x86_64.pkg.tar.zst");
        if (!fallback
            || read_text(temp_dir / "bcfg-fallback-x/usr/share/cflags.txt") != "-DFALLBACK_CFLAG=3") {
            sage::util::log_error("Compiler fallback did not degrade to the configured fallback pair");
            return 1;
        }

        // (d) compile_jobs, not package-operation jobs, reaches one package's
        // phase shells through MAKEFLAGS and CARGO_BUILD_JOBS.
        constexpr unsigned expect_jobs = 2;
        auto job_root = temp_dir / "bcfg-jobs-root";
        auto job_dir = temp_dir / "bcfg-jobs";
        if (!write_build_toml(job_root, R"(cc = "cc"
cxx = "c++"
fallback_cc = "cc"
fallback_cxx = "c++"
jobs = 7
compile_jobs = 2
)") || !write_canary_recipe(job_dir, R"(schema_version = 1
[package]
name = "jobcanary"
version = "1.0.0"
release = "1"
description = "build-config jobs canary"
license = "MIT"
channel = "system"
install = [
    'mkdir -p "$DESTDIR/usr/share"',
    'printf "%s" "$MAKEFLAGS" > "$DESTDIR/usr/share/makeflags.txt"',
    'printf "%s" "$CARGO_BUILD_JOBS" > "$DESTDIR/usr/share/cargojobs.txt"',
    'printf "%s" "$SAGE_TEST_FAKEROOT_ACTIVE" > "$DESTDIR/usr/share/fakeroot.txt"',
]
)")) {
            sage::util::log_error("Failed to create jobs canary fixture");
            return 1;
        }
        auto jobcanary = build_with_root(job_dir, job_root, temp_dir / "bcfg-jobs-x",
                                         "jobcanary-1.0.0-1-x86_64.pkg.tar.zst");
        if (!jobcanary
            || read_text(temp_dir / "bcfg-jobs-x/usr/share/makeflags.txt") != std::format("-j{}", expect_jobs)
            || read_text(temp_dir / "bcfg-jobs-x/usr/share/cargojobs.txt") != std::to_string(expect_jobs)
            || read_text(temp_dir / "bcfg-jobs-x/usr/share/fakeroot.txt") != "1") {
            sage::util::log_error(
                "v1 phase did not receive compile_jobs or the fakeroot environment");
            return 1;
        }

        // The execution boundary is mandatory. A configured executable that
        // cannot be probed must stop before even the first recipe command.
        auto missing_fakeroot_root = temp_dir / "bcfg-missing-fakeroot-root";
        auto missing_fakeroot_dir = temp_dir / "bcfg-missing-fakeroot";
        if (!write_build_toml(missing_fakeroot_root,
                "fakeroot = \"/nonexistent/sage-fakeroot\"\n")
            || !write_canary_recipe(missing_fakeroot_dir, R"(schema_version = 1
[package]
name = "missingfakeroot"
version = "1.0.0"
release = "1"
description = "missing fakeroot canary"
license = "MIT"
channel = "system"
install = ['touch "$RECIPE_DIR/phase-ran"']
)")) {
            sage::util::log_error("Failed to create missing-fakeroot fixture");
            return 1;
        }
        {
            CliOptions build_opts;
            build_opts.args = {missing_fakeroot_dir.string()};
            build_opts.target_root = missing_fakeroot_root;
            if (cmd_build(build_opts) == 0
                || std::filesystem::exists(missing_fakeroot_dir / "phase-ran")) {
                sage::util::log_error(
                    "Build did not stop before recipe execution when fakeroot was unusable");
                return 1;
            }
        }

        // (f) A pinned compiler never falls back: an unusable pin fails the
        // build even though the global fallback pair is alive and well.
        auto pin_dir = temp_dir / "bcfg-pin-bad";
        if (!write_canary_recipe(pin_dir, R"(schema_version = 1
[package]
name = "pinbad"
version = "1.0.0"
release = "1"
description = "pinned-compiler failure canary"
license = "MIT"
channel = "system"

[build]
cc = "/nonexistent/sage-no-such-cc"
install = [
    'mkdir -p "$DESTDIR/usr/share"',
    'printf ok > "$DESTDIR/usr/share/done.txt"',
]
)")) {
            sage::util::log_error("Failed to create pinned-compiler fixture");
            return 1;
        }
        {
            CliOptions build_opts;
            build_opts.args = {pin_dir.string()};
            build_opts.target_root = canary_root;  // its global pair probes fine
            if (cmd_build(build_opts) == 0) {
                sage::util::log_error("A broken pinned compiler must fail, not fall back");
                return 1;
            }
        }

        if (run_service_lifecycle_tests(temp_dir, canary_root) != 0) return 1;

        // (f) A failing phase stops the build under the toolchain that ran --
        // no silent retry under another compiler may manufacture a package.
        auto stop_dir = temp_dir / "bcfg-stop";
        if (!write_canary_recipe(stop_dir, R"(schema_version = 1
[package]
name = "stopcanary"
version = "1.0.0"
release = "1"
description = "failure-stop canary"
license = "MIT"
channel = "system"
install = [
    'false',
]
)")) {
            sage::util::log_error("Failed to create failure-stop fixture");
            return 1;
        }
        {
            CliOptions stop_opts;
            stop_opts.args = {stop_dir.string()};
            stop_opts.target_root = canary_root;
            const int rc = cmd_build(stop_opts);
            const auto archive_after =
                canary_dir / "stopcanary-1.0.0-1-x86_64.pkg.tar.zst";
            if (rc == 0 || std::filesystem::exists(archive_after)) {
                sage::util::log_error(
                    "A failing install phase must stop without an archive");
                return 1;
            }
        }

        auto managed = sage::package::Recipe::parse_toml(R"(
schema_version = 2
[package]
name = "managed"
version = "1.0.0"
release = "1"
upstream = "https://github.com/example/managed/tags"
upstream_regex = 'v(\d+\.\d+\.\d+)'

[build]
system = "cmake"
configure_options = ["-DBUILD_TESTING=OFF"]
allowed_compilers = ["clang", "gcc"]
allowed_linkers = ["lld", "mold", "ld"]

[build.flag_env]
cflags = ["KCFLAGS"]
ldflags = ["KBUILD_LDFLAGS"]

[build.tool_env]
cc = ["HOSTCC"]
linker = ["HOSTLD"]

[build.toolchain.compiler]
family = "clang"
package = "clang"
minimum_version = "1"

[build.toolchain.linker]
family = "lld"
package = "lld"
minimum_version = "1"
)");
        if (!managed || managed->upstream.url.empty()
            || managed->upstream.version_regex.empty()
            || !std::ranges::contains(managed->build_deps, "clang >= 1")
            || !std::ranges::contains(managed->build_deps, "lld >= 1")) {
            sage::util::log_error("Recipe v2 upstream/build model did not parse");
            return 1;
        }
        auto managed_cargo = sage::package::Recipe::parse_toml(R"(
schema_version = 2
[package]
name = "managed-cargo"
version = "1.0.0"
release = "1"
[build]
system = "cargo"
[build.toolchain.compiler]
family = "clang"
package = "clang"
minimum_version = "20"
[build.toolchain.linker]
family = "lld"
package = "lld"
minimum_version = "20"
[build.toolchain.rust]
family = "rustc"
package = "rust"
minimum_version = "1.90"
)");
        if (!managed_cargo
            || !std::ranges::contains(managed_cargo->build_deps, "rust >= 1.90")) {
            sage::util::log_error("Cargo recipe did not derive its Rust compiler dependency");
            return 1;
        }
        auto rust_only = sage::package::Recipe::parse_toml(R"(
schema_version = 2
[package]
name = "rust-only"
version = "1.0.0"
release = "1"
[build]
system = "cargo"
[build.toolchain.rust]
family = "rustc"
package = "rust"
minimum_version = "1.96.1"
)");
        if (!rust_only || !rust_only->managed_build.compiler.family.empty()
            || !rust_only->managed_build.linker.family.empty()
            || !std::ranges::contains(rust_only->build_deps, "rust >= 1.96.1")) {
            sage::util::log_error(
                "Cargo recipe could not declare a Rust-only tool requirement");
            return 1;
        }
        sage::config::BuildConfig managed_cfg;
        managed_cfg.cc = "clang";
        managed_cfg.cxx = "clang++";
        managed_cfg.linker = "lld";
        auto managed_plan = sage::build::plan_v2(
            *managed, managed_cfg,
            {.source = "/tmp/sage-v2-src", .package = "/tmp/sage-v2-pkg"},
            {.cc = "clang", .cxx = "clang++", .linker = "lld", .rustc = "rustc",
             .compiler_version = "22.1.8", .cxx_version = "22.1.8",
             .linker_version = "22.1.8", .rustc_version = "1.90.0",
             .compiler_family = "clang", .cxx_family = "clang",
             .linker_family = "lld", .rustc_family = "rustc"}, 8);
        if (!managed_plan || managed_plan->steps.size() != 3
            || managed_plan->environment["KCFLAGS"] != managed_cfg.cflags
            || managed_plan->environment["KBUILD_LDFLAGS"].find("-fuse-ld=lld")
                == std::string::npos
            || managed_plan->environment["HOSTCC"] != "clang"
            || managed_plan->environment["HOSTLD"] != "lld"
            || managed_plan->environment.contains("RUSTC")
            || managed_plan->steps[0].command.find("cmake -S") == std::string::npos) {
            sage::util::log_error("Recipe v2 CMake plan is incomplete");
            return 1;
        }
        for (auto system : {sage::package::BuildSystem::Meson,
                            sage::package::BuildSystem::Xmake,
                            sage::package::BuildSystem::Cargo,
                            sage::package::BuildSystem::Make,
                            sage::package::BuildSystem::Autotools}) {
            auto variant = *managed;
            variant.managed_build.system = system;
            if (system == sage::package::BuildSystem::Cargo
                || system == sage::package::BuildSystem::Make)
                variant.managed_build.configure_options.clear();
            auto variant_plan = sage::build::plan_v2(
                    variant, managed_cfg,
                    {.source = "/tmp/sage-v2-src", .package = "/tmp/sage-v2-pkg"},
                    {.cc = "clang", .cxx = "clang++", .linker = "lld", .rustc = "rustc",
                     .compiler_version = "22.1.8", .cxx_version = "22.1.8",
                     .linker_version = "22.1.8", .rustc_version = "1.90.0",
                     .compiler_family = "clang", .cxx_family = "clang",
                     .linker_family = "lld", .rustc_family = "rustc"}, 8);
            if (!variant_plan) {
                sage::util::log_error("A managed recipe v2 backend failed to plan");
                return 1;
            }
            if (variant_plan->environment["MAKEFLAGS"] != "-j8"
                || variant_plan->environment["CARGO_BUILD_JOBS"] != "8") {
                sage::util::log_error(
                    "Managed recipe v2 environment lost single-package parallelism");
                return 1;
            }
            const auto build_step = std::ranges::find(
                variant_plan->steps, "build", &sage::build::BuildStep::name);
            if (build_step == variant_plan->steps.end()
                || (system == sage::package::BuildSystem::CMake
                    && !build_step->command.contains("--parallel 8"))
                || (system == sage::package::BuildSystem::Meson
                    && !build_step->command.contains(" -j 8"))
                || (system == sage::package::BuildSystem::Xmake
                    && (!build_step->command.contains("xmake -j 8")
                        || !variant_plan->steps.front().command.contains(
                            "--ld='clang++'")
                        || !std::ranges::all_of(variant_plan->steps,
                            [](const auto& step) {
                                return step.command.contains("--root");
                            })))) {
                sage::util::log_error(
                    "Managed recipe v2 backend lost its explicit job count");
                return 1;
            }
            if (system == sage::package::BuildSystem::Make
                && variant_plan->steps[0].command.find("HOSTCC='clang'")
                    == std::string::npos) {
                sage::util::log_error("Make plan did not enforce custom tool channels");
                return 1;
            }
            if (system == sage::package::BuildSystem::Cargo
                && variant_plan->environment["RUSTC"] != "rustc") {
                sage::util::log_error("Cargo plan did not enforce Sage's Rust compiler");
                return 1;
            }
        }
        auto override = *managed;
        override.managed_build.variables["CC"] = "gcc";
        if (sage::build::plan_v2(
                override, managed_cfg,
                {.source = "/tmp/sage-v2-src", .package = "/tmp/sage-v2-pkg"},
                {.cc = "clang", .cxx = "clang++", .linker = "lld", .rustc = "rustc",
                 .compiler_version = "22.1.8", .cxx_version = "22.1.8",
                 .linker_version = "22.1.8", .rustc_version = "1.90.0",
                 .compiler_family = "clang", .cxx_family = "clang",
                 .linker_family = "lld", .rustc_family = "rustc"}, 8)) {
            sage::util::log_error("Recipe v2 overrode a Sage-managed compiler");
            return 1;
        }
        if (sage::build::plan_v2(
                *managed, managed_cfg,
                {.source = "/tmp/sage-v2-src", .package = "/tmp/sage-v2-pkg"},
                {.cc = "clang", .cxx = "clang++", .linker = "lld", .rustc = "rustc",
                 .compiler_version = "0.9", .cxx_version = "22.1.8",
                 .linker_version = "22.1.8", .rustc_version = "1.90.0",
                 .compiler_family = "clang", .cxx_family = "clang",
                 .linker_family = "lld", .rustc_family = "rustc"}, 8)) {
            sage::util::log_error("Recipe v2 accepted a compiler below its declared minimum");
            return 1;
        }

        // End to end: a v2 Make recipe has no shell phases and observes the
        // exact compiler/linker/flag policy through Make command-line values.
        auto v2_root = temp_dir / "bcfg-v2-root";
        auto v2_dir = temp_dir / "bcfg-v2-make";
        if (!write_build_toml(v2_root, R"(cc = "gcc"
cxx = "g++"
linker = "ld"
fallback_cc = "clang"
fallback_cxx = "clang++"
fallback_linker = "ld"
cflags = "-DV2_POLICY=1"
jobs = 7
compile_jobs = 2
)") || !write_canary_recipe(v2_dir, R"(schema_version = 2
[package]
name = "v2makecanary"
version = "1.0.0"
release = "1"
description = "managed Make canary"
license = "MIT"
channel = "system"

[build]
system = "make"

[build.flag_env]
cflags = ["KCFLAGS"]
ldflags = ["KBUILD_LDFLAGS"]

[build.toolchain.compiler]
family = "gcc"
package = "gcc"
minimum_version = "1"

[build.toolchain.linker]
family = "ld"
package = "binutils"
minimum_version = "1"
)") ) {
            sage::util::log_error("Failed to create v2 Make fixtures");
            return 1;
        }
        {
            std::ofstream makefile(v2_dir / "Makefile");
            makefile << R"(all:
	printf 'int main(void) { return 0; }\n' > canary.c
	$(CC) $(CPPFLAGS) $(CFLAGS) canary.c $(LDFLAGS) -o canary
install:
	mkdir -p $(DESTDIR)/usr/bin $(DESTDIR)/usr/share/v2makecanary
	cp canary $(DESTDIR)/usr/bin/v2makecanary
	printf '%s|%s|%s' '$(CC)' '$(LD)' '$(CFLAGS)' > $(DESTDIR)/usr/share/v2makecanary/policy
	printf '%s' '$(MAKEFLAGS)' > $(DESTDIR)/usr/share/v2makecanary/jobs
	printf '%s' "$$SAGE_TEST_FAKEROOT_ACTIVE" > $(DESTDIR)/usr/share/v2makecanary/fakeroot
)";
        }
        auto v2_built = build_with_root(
            v2_dir, v2_root, temp_dir / "bcfg-v2-make-x",
            "v2makecanary-1.0.0-1-x86_64.pkg.tar.zst");
        const auto observed_tool = [&](std::string_view role)
            -> const sage::package::ManagedBuildTool* {
            if (!v2_built) return nullptr;
            auto it = std::ranges::find(v2_built->managed_build_tools, role,
                &sage::package::ManagedBuildTool::role);
            return it == v2_built->managed_build_tools.end() ? nullptr : &*it;
        };
        const auto* observed_cc = observed_tool("cc");
        const auto* observed_cxx = observed_tool("cxx");
        const auto* observed_ld = observed_tool("linker");
        if (!v2_built
            || read_text(temp_dir / "bcfg-v2-make-x/usr/share/v2makecanary/policy")
                != "gcc|ld|-DV2_POLICY=1"
            || !read_text(temp_dir / "bcfg-v2-make-x/usr/share/v2makecanary/jobs")
                .contains("-j2")
            || read_text(temp_dir / "bcfg-v2-make-x/usr/share/v2makecanary/fakeroot") != "1"
            || v2_built->schema_version != 2
            || !observed_cc || observed_cc->executable != "gcc"
            || observed_cc->family != "gcc" || observed_cc->version.empty()
            || !observed_cxx || observed_cxx->executable != "g++"
            || observed_cxx->family != "gcc" || observed_cxx->version.empty()
            || !observed_ld || observed_ld->executable != "ld"
            || observed_ld->family != "ld" || observed_ld->version.empty()
            || !std::ranges::contains(observed_cc->parameters,
                "CFLAGS=-DV2_POLICY=1")
            || !std::ranges::contains(observed_cc->parameters,
                "KCFLAGS=-DV2_POLICY=1")
            || !std::ranges::contains(observed_cxx->parameters,
                "CXXFLAGS=-DV2_POLICY=1")
            || !std::ranges::contains(observed_ld->parameters,
                "LDFLAGS=-fuse-ld=bfd")
            || !std::ranges::contains(observed_ld->parameters,
                "KBUILD_LDFLAGS=-fuse-ld=bfd")) {
            sage::util::log_error("Managed v2 Make build did not use Sage policy");
            return 1;
        }
        auto v2_indexed = sage::archive::generate_repo_index(v2_dir, "managed-v2");
        std::ifstream v2_index_file(v2_dir / "index.toml");
        std::stringstream v2_index_text;
        v2_index_text << v2_index_file.rdbuf();
        auto v2_index = sage::channel::ChannelIndex::parse_toml(v2_index_text.str());
        if (!v2_indexed || !v2_index
            || v2_index->available_packages.size() != 1
            || v2_index->available_packages.front().managed_build_tools
                != v2_built->managed_build_tools) {
            sage::util::log_error(
                "Managed v2 tool observations did not survive repository indexing");
            return 1;
        }

        // Cargo adds the actual Sage-configured rustc observation; it must not
        // relabel the native CC used as Rust's linker driver as the language
        // compiler.
        auto cargo_root = temp_dir / "bcfg-v2-cargo-root";
        auto cargo_dir = temp_dir / "bcfg-v2-cargo";
        if (!write_build_toml(cargo_root, R"(cc = "gcc"
cxx = "g++"
linker = "ld"
rustc = "rustc"
)") || !write_canary_recipe(cargo_dir, R"(schema_version = 2
[package]
name = "v2cargocanary"
version = "1.0.0"
release = "1"
description = "managed Cargo canary"
license = "MIT"
channel = "system"
[build]
system = "cargo"
[build.toolchain.compiler]
family = "gcc"
package = "gcc"
minimum_version = "1"
[build.toolchain.linker]
family = "ld"
package = "binutils"
minimum_version = "1"
[build.toolchain.rust]
family = "rustc"
package = "rust"
minimum_version = "1"
)") ) {
            sage::util::log_error("Failed to create v2 Cargo fixtures");
            return 1;
        }
        std::filesystem::create_directories(cargo_dir / "src");
        std::ofstream(cargo_dir / "Cargo.toml") << R"([package]
name = "v2cargocanary"
version = "1.0.0"
edition = "2024"
)";
        std::ofstream(cargo_dir / "Cargo.lock") << R"(# This file is automatically @generated by Cargo.
version = 4

[[package]]
name = "v2cargocanary"
version = "1.0.0"
)";
        std::ofstream(cargo_dir / "src/main.rs")
            << "fn main() { println!(\"managed cargo\"); }\n";
        auto cargo_built = build_with_root(
            cargo_dir, cargo_root, temp_dir / "bcfg-v2-cargo-x",
            "v2cargocanary-1.0.0-1-x86_64.pkg.tar.zst");
        const auto cargo_rustc = cargo_built
            ? std::ranges::find(cargo_built->managed_build_tools, "rustc",
                &sage::package::ManagedBuildTool::role)
            : std::vector<sage::package::ManagedBuildTool>::iterator{};
        const auto cargo_cc = cargo_built
            ? std::ranges::find(cargo_built->managed_build_tools, "cc",
                &sage::package::ManagedBuildTool::role)
            : std::vector<sage::package::ManagedBuildTool>::iterator{};
        const auto cargo_cxx = cargo_built
            ? std::ranges::find(cargo_built->managed_build_tools, "cxx",
                &sage::package::ManagedBuildTool::role)
            : std::vector<sage::package::ManagedBuildTool>::iterator{};
        const auto cargo_linker = cargo_built
            ? std::ranges::find(cargo_built->managed_build_tools, "linker",
                &sage::package::ManagedBuildTool::role)
            : std::vector<sage::package::ManagedBuildTool>::iterator{};
        if (!cargo_built
            || cargo_rustc == cargo_built->managed_build_tools.end()
            || cargo_cc != cargo_built->managed_build_tools.end()
            || cargo_cxx != cargo_built->managed_build_tools.end()
            || cargo_linker == cargo_built->managed_build_tools.end()
            || cargo_linker->executable != "ld"
            || !std::ranges::contains(cargo_linker->parameters,
                "RUSTFLAGS=-C linker=gcc -C link-arg=-fuse-ld=bfd")
            || cargo_rustc->executable != "rustc"
            || cargo_rustc->family != "rustc" || cargo_rustc->version.empty()
            || !std::ranges::contains(cargo_rustc->parameters,
                "RUSTFLAGS=-C linker=gcc -C link-arg=-fuse-ld=bfd")
            || !std::filesystem::exists(
                temp_dir / "bcfg-v2-cargo-x/usr/bin/v2cargocanary")) {
            sage::util::log_error(
                "Managed Cargo build did not preserve its observed rustc identity");
            return 1;
        }
        auto unsafe_v2 = sage::package::Recipe::parse_toml(R"(
schema_version = 2
[package]
name = "unsafe"
version = "1"
release = "1"
[build]
system = "make"
install = ["make install"]
)");
        if (unsafe_v2) {
            sage::util::log_error("Recipe v2 accepted a legacy shell command array");
            return 1;
        }


        sage::util::log_success("13. Recipe v1 compatibility and Sage-managed v2 planning OK");
    }

    return 0;
}

export int run_multisource_fetch_tests() {
    // 15. Multi-source recipes: `[[source]]` arrays fetch every entry beside
    // the primary archive and stage the extras at src/distfiles/, while the
    // scope collectors keep seeing keys written after the last block.
    {
        auto temp_dir = std::filesystem::temp_directory_path() / "sage_multisrc_test";
        std::filesystem::remove_all(temp_dir);
        std::filesystem::create_directories(temp_dir / "target/etc/sage");
        std::ofstream(temp_dir / "target/etc/sage/system.toml") << "schema_version = 1\n";

        // Model level: the first [[source]] fills the primary slot, the rest
        // become extras, and trailing keys still land in host_deps/provides --
        // they live inside the last array element's TOML table.
        auto multi = sage::package::Recipe::parse_toml(R"(schema_version = 1
[package]
name = "m"
version = "1.0.0"
release = "1"

[[source]]
url = "https://a.example/main.tar.gz"
sha256 = "aaaa"

[[source]]
url = "https://b.example/fix.patch"
sha256 = "bbbb"

dependencies = ["zlib >= 1.3"]
provides = ["virtual/m"]
)");
        if (!multi || multi->source_url != "https://a.example/main.tar.gz"
            || multi->source_sha256 != "aaaa"
            || multi->extra_sources.size() != 1
            || multi->extra_sources[0].url != "https://b.example/fix.patch"
            || multi->extra_sources[0].sha256 != "bbbb"
            || multi->host_deps.size() != 1 || multi->host_deps[0].name != "zlib"
            || multi->provides.size() != 1 || multi->provides[0] != "virtual/m") {
            sage::util::log_error("Multi-source recipe parse dropped an entry or a trailing scope key");
            return 1;
        }
        // Backward compat: the single [source] table yields no extras.
        auto single = sage::package::Recipe::parse_toml(R"(schema_version = 1
[package]
name = "s"
version = "1.0.0"
release = "1"

[source]
url = "https://a.example/main.tar.gz"
sha256 = "aaaa"
)");
        if (!single || single->source_url.empty() || !single->extra_sources.empty()) {
            sage::util::log_error("Single-source recipe parse regressed");
            return 1;
        }

        // End to end over file:// URLs: a primary tarball unpacked to src/ and
        // one plain extra file staged at src/distfiles/, both consumed by the
        // install phase relative to the work directory.
        auto dist_dir = temp_dir / "dist";
        std::filesystem::create_directories(dist_dir / "main-1.0");
        {
            std::ofstream f(dist_dir / "main-1.0/payload.txt");
            f << "primary payload\n";
        }
        {
            std::ofstream f(dist_dir / "extra.txt");
            f << "extra payload\n";
        }
        if (std::system(std::format("tar -czf \"{}\" -C \"{}\" main-1.0",
                (dist_dir / "main.tar.gz").string(), dist_dir.string()).c_str()) != 0) {
            sage::util::log_error("Failed to pack multi-source fixture tarball");
            return 1;
        }
        auto sha_of = [](const std::filesystem::path& p) -> std::string {
            auto h = sage::util::compute_file_sha256(p);
            return h ? *h : "";
        };
        const std::string main_sha = sha_of(dist_dir / "main.tar.gz");
        const std::string extra_sha = sha_of(dist_dir / "extra.txt");

        auto write_recipe = [&](const std::filesystem::path& dir, std::string_view name,
                                const std::string& extra_hash) {
            std::filesystem::create_directories(dir);
            std::ofstream recipe(dir / "recipe.toml");
            recipe << std::format(R"(schema_version = 1
[package]
name = "{}"
version = "1.0.0"
release = "1"
description = "multi-source canary"
license = "MIT"
channel = "system"

[[source]]
url = "file://{}/main.tar.gz"
sha256 = "{}"

[[source]]
url = "file://{}/extra.txt"
sha256 = "{}"

install = [
    'mkdir -p "$DESTDIR/usr/share"',
    'cp payload.txt "$DESTDIR/usr/share/primary.txt"',
    'cp distfiles/extra.txt "$DESTDIR/usr/share/extra.txt"',
]
)", name, dist_dir.string(), main_sha, dist_dir.string(), extra_hash);
            return recipe.good();
        };

        auto ms_dir = temp_dir / "multisrc";
        if (!write_recipe(ms_dir, "multisrc", extra_sha)) {
            sage::util::log_error("Failed to write multi-source fixture recipe");
            return 1;
        }
        CliOptions build_ok;
        build_ok.args = {ms_dir.string()};
        build_ok.target_root = temp_dir / "target";
        if (cmd_build(build_ok) != 0) {
            sage::util::log_error("Failed to build a multi-source recipe");
            return 1;
        }
        auto unpacked = sage::archive::extract_package(
            ms_dir / "multisrc-1.0.0-1-x86_64.pkg.tar.zst", temp_dir / "unpacked");
        auto read_text = [](const std::filesystem::path& p) {
            std::ifstream f(p);
            std::stringstream ss;
            ss << f.rdbuf();
            return ss.str();
        };
        if (!unpacked
            || read_text(temp_dir / "unpacked/usr/share/primary.txt") != "primary payload\n"
            || read_text(temp_dir / "unpacked/usr/share/extra.txt") != "extra payload\n") {
            sage::util::log_error("Extra sources did not reach src/distfiles/ during build");
            return 1;
        }
        // The staged copies are consumed, not shipped: no distfiles leak into
        // the package payload or the installed tree.
        if (std::filesystem::exists(temp_dir / "unpacked/usr/share/distfiles")) {
            sage::util::log_error("distfiles staging leaked into the package payload");
            return 1;
        }

        // A wrong hash on any entry -- including extras -- is fatal.
        auto bad_dir = temp_dir / "multisrc-bad";
        if (!write_recipe(bad_dir, "multisrcbad",
                          std::string(extra_sha.size(), 'f'))) {
            sage::util::log_error("Failed to write bad-hash fixture recipe");
            return 1;
        }
        CliOptions build_bad;
        build_bad.args = {bad_dir.string()};
        build_bad.target_root = temp_dir / "target";
        if (cmd_build(build_bad) == 0) {
            sage::util::log_error("A bad sha256 on an extra source was not fatal");
            return 1;
        }

        std::filesystem::remove_all(temp_dir);
        sage::util::log_success("15. Multi-Source Fetch, Verification & Staging OK");
    }
    return 0;
}

} // namespace build
} // namespace sage::tests
