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
        auto sysroot_cfg = sage::config::BuildConfig::parse_toml("sysroot = \"/opt/sage-sysroot\"\n");
        auto relative_sysroot_cfg = sage::config::BuildConfig::parse_toml("sysroot = \"sysroot\"\n");
        auto invalid_jobs_cfg = sage::config::BuildConfig::parse_toml(
            "compile_jobs = -1\n");
        if (!jobs_cfg || jobs_cfg->jobs != 4 || jobs_cfg->compile_jobs != 2
            || jobs_cfg->configured_compile_jobs() != 2
            || !legacy_jobs_cfg || legacy_jobs_cfg->configured_compile_jobs() != 4
            || !auto_jobs_cfg || auto_jobs_cfg->configured_compile_jobs() != 0
            || !sysroot_cfg || sysroot_cfg->sysroot != "/opt/sage-sysroot"
            || relative_sysroot_cfg
            || invalid_jobs_cfg || empty_cfg->jobs != 0
            || empty_cfg->compile_jobs.has_value()) {
            sage::util::log_error("BuildConfig compile_jobs parsing drifted");
            return 1;
        }

        auto cache_cfg = sage::config::BuildConfig::parse_toml(
            "compiler_cache = \"auto\"\nccache_dir = \"/tmp/sage-cache\"\n"
            "memory_limit = \"1048576\"\npids_limit = 8\n");
        auto empty_limit_cfg = sage::config::BuildConfig::parse_toml(
            "memory_limit = \"\"\n");
        auto invalid_cache_cfg = sage::config::BuildConfig::parse_toml(
            "compiler_cache = \"bad\"\n");
        auto invalid_memory_cfg = sage::config::BuildConfig::parse_toml(
            "memory_limit = \"1G\"\n");
        auto invalid_pids_cfg = sage::config::BuildConfig::parse_toml(
            "pids_limit = -1\n");
        if (!cache_cfg || cache_cfg->compiler_cache_mode() != "auto"
            || cache_cfg->ccache_dir != "/tmp/sage-cache"
            || cache_cfg->memory_limit != "1048576"
            || cache_cfg->pids_limit != 8
            || !empty_limit_cfg || !empty_limit_cfg->memory_limit.empty()
            || invalid_cache_cfg || invalid_memory_cfg || invalid_pids_cfg) {
            sage::util::log_error("BuildConfig cache/resource-limit parsing drifted");
            return 1;
        }
        if (!sage::package::validate_package_architecture("riscv64")
            || !sage::package::validate_package_architecture("armv7")
            || sage::config::triplet_to_arch("riscv64-linux-gnu") != "riscv64"
            || sage::config::triplet_to_arch("arm-linux-gnueabihf") != "armv7") {
            sage::util::log_error("Cross-target architecture support is inconsistent");
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
license = "MIT"
channel = "system"
arch = "x86_64"
upstream = "https://github.com/example/managed/tags"
upstream_regex = 'v(\d+\.\d+\.\d+)'

[build]
system = "cmake"
payload = "allowlist"
configure_options = ["-DBUILD_TESTING=OFF"]
install_files = ["usr/bin/**"]
patches = [{ file = "fix.patch", strip = 0, sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" }]
install_copies = [{ from = "built/tool", to = "usr/bin/tool" }]
install_symlinks = [{ path = "usr/bin/cc", target = "clang" }]
install_moves = [{ from = "usr/libexec/helper", to = "usr/bin/helper" }]
install_removes = ["usr/share/doc/**"]
install_generates = [{ path = "usr/lib/tool.conf", content = "x\n", mode = 420 }]
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
            || managed->managed_build.install_files != std::vector<std::string>{"usr/bin/**"}
            || managed->managed_build.install_copies.size() != 1
            || managed->managed_build.install_copies.front().source != "built/tool"
            || managed->managed_build.install_symlinks.size() != 1
            || managed->managed_build.install_symlinks.front().target != "clang"
            || managed->managed_build.install_moves.size() != 1
            || managed->managed_build.install_removes.size() != 1
            || managed->managed_build.install_generates.size() != 1
            || managed->managed_build.patches_spec.size() != 1
            || managed->managed_build.patches_spec.front().file != "fix.patch"
            || managed->managed_build.patches_spec.front().strip != 0
            || managed->managed_build.patches_spec.front().sha256
                != "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            || !std::ranges::contains(managed->build_deps, "clang >= 1")
            || !std::ranges::contains(managed->build_deps, "lld >= 1")) {
            sage::util::log_error("Recipe v2 upstream/build model did not parse");
            return 1;
        }
        auto checked = sage::package::Recipe::parse_toml(R"(
schema_version = 2
[package]
name = "checked"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "x86_64"
check_dependencies = ["pkg-config >= 1"]
[build]
system = "script"
payload = "allowlist"
install_files = ["usr/share/checked/**"]
[[build.steps]]
name = "run-tests"
phase = "check"
cwd = "source"
command = "true"
)");
        auto missing_check_phase = sage::package::Recipe::parse_toml(R"(
schema_version = 2
[package]
name = "missing-check"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "x86_64"
check_dependencies = ["pkg-config >= 1"]
[build]
system = "make"
payload = "all"
)");
        auto conflicting_patch = sage::package::Recipe::parse_toml(R"(
schema_version = 2
[package]
name = "conflicting-patch"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "x86_64"
[source]
url = "https://example.invalid/fix.patch"
sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
[build]
system = "script"
payload = "allowlist"
install_files = ["usr/share/conflicting/**"]
patches = [{ file = "fix.patch", sha256 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" }]
[[build.steps]]
name = "build"
phase = "install"
command = "true"
)");
        auto duplicate_patch = sage::package::Recipe::parse_toml(R"(
schema_version = 2
[package]
name = "duplicate-patch"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "x86_64"
[source]
url = "https://example.invalid/fix.patch"
sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
[build]
system = "script"
payload = "allowlist"
install_files = ["usr/share/duplicate/**"]
patches = ["fix.patch", "fix.patch"]
[[build.steps]]
name = "build"
phase = "install"
command = "true"
)");
        if (!checked || checked->check_deps != std::vector<std::string>{"pkg-config >= 1"}
            || checked->managed_build.steps.front().phase != "check"
            || missing_check_phase || conflicting_patch || duplicate_patch) {
            sage::util::log_error(
                "Recipe v2 check phase or normalized patch semantics failed");
            return 1;
        }
        auto managed_outputs = sage::package::Recipe::parse_toml(R"(
schema_version = 2
[package]
name = "managed-outputs"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "x86_64"
[build]
system = "make"
payload = "outputs"
outputs = [
  { name = "managed-libs", install_files = ["usr/lib/libmanaged.so.*"] },
  { name = "managed-dev", install_files = ["usr/include/**"] },
]
)");
        if (!managed_outputs || managed_outputs->managed_build.outputs.size() != 2) {
            sage::util::log_error("Recipe v2 multi-output model did not parse");
            return 1;
        }
        auto bad_step_key = sage::package::Recipe::parse_toml(R"(
schema_version = 2
[package]
name = "bad-step-key"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "x86_64"
[build]
system = "script"
payload = "allowlist"
install_files = ["usr/share/bad-step-key/**"]
[[build.steps]]
name = "run"
phase = "install"
command = "true"
unexpected = "must be rejected"
)");
        auto bad_toolchain_type = sage::package::Recipe::parse_toml(R"(
schema_version = 2
[package]
name = "bad-toolchain-type"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "x86_64"
[build]
system = "make"
payload = "allowlist"
install_files = ["usr/bin/bad-toolchain-type"]
[build.toolchain]
compiler = "clang"
)");
        if (bad_step_key || bad_toolchain_type) {
            sage::util::log_error(
                "Recipe v2 accepted unknown step/toolchain fields");
            return 1;
        }
        auto scripted = sage::package::Recipe::parse_toml(R"(
schema_version = 2
[package]
name = "scripted"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "x86_64"
[build]
system = "script"
payload = "allowlist"
install_files = ["usr/share/scripted/**"]
[[build.steps]]
name = "post-install-fixup"
phase = "install"
cwd = "package"
command = "mkdir -p usr/share/scripted && printf ok > usr/share/scripted/result"
)");
        if (!scripted
            || scripted->managed_build.system != sage::package::BuildSystem::Script
            || scripted->managed_build.steps.size() != 1
            || scripted->managed_build.steps.front().cwd != "package") {
            sage::util::log_error("Recipe v2 arbitrary script step did not parse");
            return 1;
        }
        auto managed_cargo = sage::package::Recipe::parse_toml(R"(
schema_version = 2
[package]
name = "managed-cargo"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "x86_64"
[build]
system = "cargo"
payload = "all"
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
license = "MIT"
channel = "system"
arch = "x86_64"
[build]
system = "cargo"
payload = "all"
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
        for (const auto& bad : {
                std::string(R"(schema_version = 2
[package]
name = "bad-meson"
version = "1"
license = "MIT"
channel = "system"
arch = "x86_64"
[build]
system = "meson"
payload = "allowlist"
install_files = ["usr/bin/bad-meson"]
configure_options = ["-Dprefix=/tmp/escape"]
)"),
                std::string(R"(schema_version = 2
[package]
name = "bad-meson-cli"
version = "1"
license = "MIT"
channel = "system"
arch = "x86_64"
[build]
system = "meson"
payload = "allowlist"
install_files = ["usr/bin/bad-meson-cli"]
configure_options = ["--prefix=/tmp/escape"]
)") ,
                std::string(R"(schema_version = 2
[package]
name = "bad-cargo"
version = "1"
license = "MIT"
channel = "system"
arch = "x86_64"
[build]
system = "cargo"
payload = "all"
build_targets = ["--config=/tmp/escape"]
)"),
                std::string(R"(schema_version = 2
[package]
name = "bad-autotools"
version = "1"
license = "MIT"
channel = "system"
arch = "x86_64"
[build]
system = "autotools"
payload = "all"
configure_options = ["--exec-prefix=/tmp/escape"]
)" )}) {
            auto parsed_bad = sage::package::Recipe::parse_toml(bad);
            auto planned_bad = parsed_bad
                ? sage::build::plan_v2(*parsed_bad, managed_cfg,
                    {.source = "/tmp/bad-src", .package = "/tmp/bad-pkg"},
                    {.cc = "clang", .cxx = "clang++", .linker = "lld",
                     .compiler_version = "22.1", .cxx_version = "22.1",
                     .linker_version = "22.1", .compiler_family = "clang",
                     .cxx_family = "clang", .linker_family = "lld"}, 1)
                : std::expected<sage::build::BuildPlan, std::string>(
                    std::unexpected(std::string{"parse"}));
            if (planned_bad) {
                sage::util::log_error("v2 accepted a backend installation-directory override");
                return 1;
            }
        }
        auto managed_plan = sage::build::plan_v2(
            *managed, managed_cfg,
            {.source = "/tmp/sage-v2-src", .package = "/tmp/sage-v2-pkg"},
            {.cc = "clang", .cxx = "clang++", .linker = "lld", .rustc = "rustc",
             .compiler_version = "22.1.8", .cxx_version = "22.1.8",
             .linker_version = "22.1.8", .rustc_version = "1.90.0",
             .compiler_family = "clang", .cxx_family = "clang",
             .linker_family = "lld", .rustc_family = "rustc"}, 8);
        const auto managed_configure = managed_plan
            ? std::ranges::find(managed_plan->steps, "configure",
                &sage::build::BuildStep::name)
            : std::vector<sage::build::BuildStep>::iterator{};
        if (!managed_plan || managed_plan->steps.size() != 5
            || managed_plan->environment["KCFLAGS"] != managed_cfg.cflags
            || managed_plan->environment["KBUILD_LDFLAGS"].find("-fuse-ld=lld")
                == std::string::npos
            || managed_plan->environment["HOSTCC"] != "clang"
            || managed_plan->environment["HOSTLD"] != "lld"
            || managed_plan->environment.contains("RUSTC")
            || managed_configure == managed_plan->steps.end()
            || managed_configure->command.find("cmake -S") == std::string::npos) {
            sage::util::log_error("Recipe v2 CMake plan is incomplete");
            return 1;
        }
        auto kernel_recipe = sage::package::Recipe::parse_toml(R"(
schema_version = 2
[package]
name = "linux"
version = "6.18.1"
release = "1"
license = "GPL-2.0-only"
channel = "system"
arch = "x86_64"
[build]
system = "make"
payload = "allowlist"
kernel = true
install_files = ["boot/vmlinuz-*"]
)");
        auto bad_kernel_recipe = sage::package::Recipe::parse_toml(R"(
schema_version = 2
[package]
name = "not-kernel"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "x86_64"
[build]
system = "cmake"
payload = "allowlist"
kernel = true
install_files = ["usr/bin/tool"]
)");
        if (!kernel_recipe || !kernel_recipe->managed_build.kernel
            || bad_kernel_recipe) {
            sage::util::log_error("Recipe v2 kernel marker validation failed");
            return 1;
        }
        auto kernel_cfg = managed_cfg;
        kernel_cfg.cflags = "-DKERNEL_C=1";
        kernel_cfg.cppflags = "-DKERNEL_CPP=1";
        kernel_cfg.ldflags = "-z relro";
        kernel_cfg.rustflags = "-C opt-level=2";
        const auto kernel_tools = sage::build::Toolchain{
            .cc = "clang", .cxx = "clang++", .linker = "lld", .rustc = "rustc",
            .compiler_version = "22.1.8", .cxx_version = "22.1.8",
            .linker_version = "22.1.8", .rustc_version = "1.90.0",
            .compiler_family = "clang", .cxx_family = "clang",
            .linker_family = "lld", .rustc_family = "rustc"};
        auto kernel_plan = sage::build::plan_v2(
            *kernel_recipe, kernel_cfg,
            {.source = "/tmp/sage-kernel-src", .package = "/tmp/sage-kernel-pkg"},
            kernel_tools, 8);
        const auto kernel_build = kernel_plan
            ? std::ranges::find(kernel_plan->steps, "build",
                &sage::build::BuildStep::name)
            : std::vector<sage::build::BuildStep>::iterator{};
        if (!kernel_plan || !kernel_plan->environment.contains("LLVM")
            || kernel_plan->environment.at("LLVM") != "1"
            || kernel_plan->environment.at("KCFLAGS") != kernel_cfg.cflags
            || kernel_plan->environment.at("KCPPFLAGS") != kernel_cfg.cppflags
            || kernel_plan->environment.at("KBUILD_LDFLAGS") != kernel_cfg.ldflags
            || kernel_plan->environment.at("KRUSTFLAGS") != kernel_cfg.rustflags
            || kernel_build == kernel_plan->steps.end()
            || !kernel_build->command.contains("LLVM='1'")
            || !kernel_build->command.contains("KCFLAGS='-DKERNEL_C=1'")) {
            sage::util::log_error(
                "Kernel Make plan did not derive LLVM/Kbuild flag channels from Sage config");
            return 1;
        }
        auto gcc_kernel_tools = kernel_tools;
        gcc_kernel_tools.cc = "gcc";
        gcc_kernel_tools.cxx = "g++";
        gcc_kernel_tools.linker = "ld";
        gcc_kernel_tools.compiler_family = "gcc";
        gcc_kernel_tools.cxx_family = "gcc";
        gcc_kernel_tools.linker_family = "ld";
        auto gcc_kernel_plan = sage::build::plan_v2(
            *kernel_recipe, kernel_cfg,
            {.source = "/tmp/sage-kernel-gcc-src", .package = "/tmp/sage-kernel-gcc-pkg"},
            gcc_kernel_tools, 8);
        if (!gcc_kernel_plan || gcc_kernel_plan->environment.contains("LLVM")
            || gcc_kernel_plan->environment.at("KCFLAGS") != kernel_cfg.cflags) {
            sage::util::log_error(
                "Kernel Make plan forced LLVM or lost KCFLAGS for a GCC configuration");
            return 1;
        }
        auto out_of_tree = *managed;
        out_of_tree.managed_build.system = sage::package::BuildSystem::Autotools;
        out_of_tree.managed_build.build_dir = "build";
        out_of_tree.managed_build.configure_options.clear();
        out_of_tree.managed_build.install_targets = {"install"};
        auto out_of_tree_plan = sage::build::plan_v2(
            out_of_tree, managed_cfg,
            {.source = "/tmp/sage-autotools-src", .package = "/tmp/sage-autotools-pkg"},
            {.cc = "clang", .cxx = "clang++", .linker = "lld", .rustc = "rustc",
             .compiler_version = "22.1.8", .cxx_version = "22.1.8",
             .linker_version = "22.1.8", .rustc_version = "1.90.0",
             .compiler_family = "clang", .cxx_family = "clang",
             .linker_family = "lld", .rustc_family = "rustc"}, 8);
        const auto out_of_tree_configure = out_of_tree_plan
            ? std::ranges::find(out_of_tree_plan->steps, "configure",
                &sage::build::BuildStep::name)
            : std::vector<sage::build::BuildStep>::iterator{};
        const auto out_of_tree_build = out_of_tree_plan
            ? std::ranges::find(out_of_tree_plan->steps, "build",
                &sage::build::BuildStep::name)
            : std::vector<sage::build::BuildStep>::iterator{};
        if (!out_of_tree_plan
            || out_of_tree_configure == out_of_tree_plan->steps.end()
            || !out_of_tree_configure->command.contains("../configure")
            || out_of_tree_build == out_of_tree_plan->steps.end()
            || out_of_tree_build->work_dir
                != std::filesystem::path("/tmp/sage-autotools-src/build")) {
            sage::util::log_error("Managed Autotools out-of-tree plan is incomplete");
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
            const auto configure_step = std::ranges::find(
                variant_plan->steps, "configure", &sage::build::BuildStep::name);
            const auto install_step = std::ranges::find(
                variant_plan->steps, "install", &sage::build::BuildStep::name);
            if (build_step == variant_plan->steps.end()
                || (system == sage::package::BuildSystem::CMake
                    && !build_step->command.contains("--parallel 8"))
                || (system == sage::package::BuildSystem::Meson
                    && !build_step->command.contains(" -j 8"))
                || (system == sage::package::BuildSystem::Xmake
                    && (!build_step->command.contains("xmake -j 8")
                        || configure_step == variant_plan->steps.end()
                        || !configure_step->command.contains(
                            "--ld='clang++'")
                        || install_step == variant_plan->steps.end()
                        || !configure_step->command.contains("--root")
                        || !build_step->command.contains("--root")
                        || !install_step->command.contains("--root")))) {
                sage::util::log_error(
                    "Managed recipe v2 backend lost its explicit job count");
                return 1;
            }
            if (system == sage::package::BuildSystem::Make
                && build_step->command.find("HOSTCC='clang'")
                    == std::string::npos) {
                sage::util::log_error("Make plan did not enforce custom tool channels");
                return 1;
            }
            if (system == sage::package::BuildSystem::CMake
                && (configure_step == variant_plan->steps.end()
                    || !configure_step->command.contains(
                        "CMAKE_INSTALL_LIBDIR=lib"))) {
                sage::util::log_error(
                    "CMake plan did not enforce Sage's canonical library directory");
                return 1;
            }
            if (system == sage::package::BuildSystem::Meson
                && (configure_step == variant_plan->steps.end()
                    || !configure_step->command.contains("--libdir=lib"))) {
                sage::util::log_error(
                    "Meson plan did not enforce Sage's canonical library directory");
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
arch = "x86_64"
conflicts = ["v2makecanary-legacy < 2"]

[build]
system = "make"
payload = "allowlist"
install_files = [
    "usr/bin/v2makecanary",
    "usr/share/v2makecanary/**",
]

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
	mkdir -p $(DESTDIR)/usr/include
	cp canary $(DESTDIR)/usr/bin/v2makecanary
	printf 'not part of this package\n' > $(DESTDIR)/usr/include/not-selected.h
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
        const auto* observed_ld = observed_tool("linker");
        const auto v2_policy = read_text(
            temp_dir / "bcfg-v2-make-x/usr/share/v2makecanary/policy");
        if (!v2_built
            // The audit alias keeps the selected compiler's requested
            // basename (gcc/clang/...) so build systems retain their driver
            // semantics; it still resolves to Sage's role wrapper.
            || !v2_policy.contains("/tool-audit/gcc|")
            || !v2_policy.contains("/tool-audit/sage-linker|-DV2_POLICY=1")
            || !read_text(temp_dir / "bcfg-v2-make-x/usr/share/v2makecanary/jobs")
                .contains("-j2")
            || read_text(temp_dir / "bcfg-v2-make-x/usr/share/v2makecanary/fakeroot") != "1"
            || v2_built->schema_version != 2
            || !observed_cc || observed_cc->executable != "gcc"
            || observed_cc->family != "gcc" || observed_cc->version.empty()
            || !observed_ld || observed_ld->executable != "ld"
            || observed_ld->family != "ld" || observed_ld->version.empty()
            || v2_built->conflicts.size() != 1
            || v2_built->conflicts.front().to_string() != "v2makecanary-legacy < 2-1"
            || !std::ranges::contains(observed_cc->parameters,
                "CFLAGS=-DV2_POLICY=1")
            || !std::ranges::contains(observed_cc->parameters,
                "KCFLAGS=-DV2_POLICY=1")
            || !std::ranges::any_of(observed_ld->parameters,
                [](const auto& value) { return value.starts_with("LDFLAGS=-fuse-ld=bfd"); })
            || !std::ranges::any_of(observed_ld->parameters,
                [](const auto& value) { return value.starts_with("KBUILD_LDFLAGS=-fuse-ld=bfd"); })) {
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
                != v2_built->managed_build_tools
            || v2_index->available_packages.front().conflicts.size() != 1
            || v2_index->available_packages.front().conflicts.front().to_string()
                != "v2makecanary-legacy < 2-1") {
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
arch = "x86_64"
[build]
system = "cargo"
payload = "all"
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
        const auto cargo_linker_driver = cargo_built
            ? std::ranges::find(cargo_built->managed_build_tools, "linker-driver",
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
            || cargo_linker_driver == cargo_built->managed_build_tools.end()
            || cargo_linker_driver->executable != "gcc"
            || cargo_linker_driver->family != "gcc"
            || cargo_linker == cargo_built->managed_build_tools.end()
            || cargo_linker->executable != "ld"
            || !std::ranges::any_of(cargo_linker->parameters,
                [](const auto& value) {
                    return value.starts_with("RUSTFLAGS=-C linker=<sage-build>/tool-audit/gcc")
                        && value.contains("-C link-arg=-fuse-ld=bfd");
                })
            || cargo_rustc->executable != "rustc"
            || cargo_rustc->family != "rustc" || cargo_rustc->version.empty()
            || !std::ranges::any_of(cargo_rustc->parameters,
                [](const auto& value) {
                    return value.starts_with("RUSTFLAGS=-C linker=<sage-build>/tool-audit/gcc")
                        && value.contains("-C link-arg=-fuse-ld=bfd");
                })
            || !std::filesystem::exists(
                temp_dir / "bcfg-v2-cargo-x/usr/bin/v2cargocanary")
            || std::filesystem::exists(
                temp_dir / "bcfg-v2-make-x/usr/include/not-selected.h")) {
            sage::util::log_error(
                "Managed Cargo build did not preserve its observed rustc identity");
            return 1;
        }
        // Script is the explicit v2 escape hatch for deterministic
        // repackaging/fixups. It may perform arbitrary package-tree logic but
        // must not claim a compiler or linker was used, and two clean builds
        // must produce byte-identical archives.
        auto script_root = temp_dir / "bcfg-v2-script-root";
        auto script_dir = temp_dir / "bcfg-v2-script";
        if (!write_build_toml(script_root, R"(cc = "gcc"
cxx = "g++"
linker = "ld"
)") || !write_canary_recipe(script_dir, R"(schema_version = 2
[package]
name = "v2scriptcanary"
version = "1.0.0"
release = "1"
description = "deterministic repackaging canary"
license = "MIT"
channel = "system"
arch = "x86_64"
[build]
system = "script"
payload = "allowlist"
install_files = ["usr/share/v2script/**"]
[[build.steps]]
name = "write-payload"
phase = "install"
cwd = "package"
command = "set -eu; test ! -w /etc; mkdir -p usr/share/v2script; printf '%s|%s|%s' \"$CC\" \"$LD\" \"$SOURCE_DATE_EPOCH\" > usr/share/v2script/result"
)") ) {
            sage::util::log_error("Failed to create v2 script fixture");
            return 1;
        }
        auto script_built = build_with_root(
            script_dir, script_root, temp_dir / "bcfg-v2-script-x1",
            "v2scriptcanary-1.0.0-1-x86_64.pkg.tar.zst");
        const auto script_archive = script_dir
            / "v2scriptcanary-1.0.0-1-x86_64.pkg.tar.zst";
        const auto copy_archive_1 = temp_dir / "script-copy-1.pkg.tar.zst";
        std::filesystem::copy_file(script_archive, copy_archive_1, std::filesystem::copy_options::overwrite_existing);
        const auto script_hash_1 = script_built
            ? sage::util::compute_file_sha256(copy_archive_1)
            : std::expected<std::string, std::string>(std::unexpected(std::string{"not-built"}));
        auto script_built_again = build_with_root(
            script_dir, script_root, temp_dir / "bcfg-v2-script-x2",
            "v2scriptcanary-1.0.0-1-x86_64.pkg.tar.zst");
        const auto script_hash_2 = script_built_again
            ? sage::util::compute_file_sha256(script_archive)
            : std::expected<std::string, std::string>(std::unexpected(std::string{"not-built"}));
        if (!script_built || !script_built_again || !script_hash_1 || !script_hash_2
            || *script_hash_1 != *script_hash_2
            || !script_built->managed_build_tools.empty()
            || read_text(temp_dir / "bcfg-v2-script-x1/usr/share/v2script/result")
                != "||0"
            || !std::filesystem::exists(
                temp_dir / "bcfg-v2-script-x2/usr/share/v2script/result")) {
            sage::util::log_error(
                "Managed v2 script repackaging was not hermetic and compiler-free");
            return 1;
        }
        auto check_failure_dir = temp_dir / "bcfg-v2-check-failure";
        if (!write_canary_recipe(check_failure_dir, R"(schema_version = 2
[package]
name = "v2checkfailure"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "x86_64"
[build]
system = "script"
payload = "allowlist"
install_files = ["usr/share/check-failure/**"]
[[build.steps]]
name = "run-tests"
phase = "check"
command = "false"
[[build.steps]]
name = "write-payload"
phase = "install"
cwd = "package"
command = "mkdir -p usr/share/check-failure; printf x > usr/share/check-failure/result"
)")) {
            sage::util::log_error("Failed to create check-failure fixture");
            return 1;
        }
        CliOptions check_failure_opts;
        check_failure_opts.args = {check_failure_dir.string()};
        check_failure_opts.target_root = script_root;
        const auto check_failure_archive = check_failure_dir
            / "v2checkfailure-1.0.0-1-x86_64.pkg.tar.zst";
        if (cmd_build(check_failure_opts) == 0
            || std::filesystem::exists(check_failure_archive)
            || std::filesystem::exists(
                check_failure_dir / "pkg/usr/share/check-failure/result")) {
            sage::util::log_error(
                "A failed v2 check phase must block packaging");
            return 1;
        }
        auto check_root = temp_dir / "bcfg-v2-check-root";
        auto check_db = sage::db::Database::open(
            check_root / "var/lib/sage/data.mdb");
        if (!check_db) {
            sage::util::log_error("Failed to create check dependency database");
            return 1;
        }
        auto check_txn = check_db->begin_write_txn();
        sage::package::PackageManifest check_pkg;
        check_pkg.name = "pkg-config";
        check_pkg.version = sage::package::Version::parse("2.0");
        check_pkg.arch = "x86_64";
        if (!check_txn
            || !check_db->put_package(*check_txn, check_pkg)
            || !check_txn->commit()) {
            sage::util::log_error("Failed to seed check dependency database");
            return 1;
        }
        if (!write_build_toml(check_root, "")
            || !write_canary_recipe(check_root / "recipe-input", R"(schema_version = 2
[package]
name = "v2checkcanary"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "x86_64"
check_dependencies = ["pkg-config >= 1"]
[build]
system = "script"
payload = "allowlist"
install_files = ["usr/share/check-canary/**"]
[[build.steps]]
name = "run-tests"
phase = "check"
command = "true"
[[build.steps]]
name = "install"
phase = "install"
cwd = "package"
command = "mkdir -p usr/share/check-canary; printf x > usr/share/check-canary/result"
)")) {
            sage::util::log_error("Failed to create check dependency fixture");
            return 1;
        }
        auto check_built = build_with_root(
            check_root / "recipe-input", check_root,
            temp_dir / "bcfg-v2-check-extract",
            "v2checkcanary-1.0.0-1-x86_64.pkg.tar.zst");
        auto check_attestation = check_built
            ? sage::package::BuildAttestation::parse_toml(
                check_built->attestation_toml)
            : std::expected<sage::package::BuildAttestation, std::string>(
                std::unexpected(std::string{"not-built"}));
        if (!check_built || !check_attestation
            || check_attestation->check_dependencies
                != std::vector<std::string>{"pkg-config >= 1"}) {
            sage::util::log_error(
                "check_dependencies were not resolved and attested");
            return 1;
        }
        auto bypass_dir = temp_dir / "bcfg-v2-script-bypass";
        if (!write_canary_recipe(bypass_dir, R"(schema_version = 2
[package]
name = "v2scriptbypass"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "x86_64"
[build]
system = "script"
payload = "allowlist"
install_files = ["usr/share/v2bypass/**"]
[[build.steps]]
name = "absolute-compiler"
phase = "install"
cwd = "package"
command = "/usr/bin/gcc --version >/dev/null; mkdir -p usr/share/v2bypass; printf x > usr/share/v2bypass/result"
)")) {
            sage::util::log_error("Failed to create v2 absolute-tool bypass fixture");
            return 1;
        }
        CliOptions bypass_opts;
        bypass_opts.args = {bypass_dir.string()};
        bypass_opts.target_root = script_root;
        const auto bypass_archive = bypass_dir
            / "v2scriptbypass-1.0.0-1-x86_64.pkg.tar.zst";
        if (cmd_build(bypass_opts) == 0 || std::filesystem::exists(bypass_archive)) {
            sage::util::log_error(
                "A v2 script absolute compiler invocation bypassed Sage's audit fence");
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
        auto unbounded_split = sage::package::Recipe::parse_toml(R"(
schema_version = 2
[package]
name = "unsafe-libs"
version = "1"
release = "1"
[build]
system = "make"
)");
        if (unbounded_split) {
            sage::util::log_error(
                "Recipe v2 accepted a split package without build.install_files");
            return 1;
        }

        // (g) Typed Backend Specs TOML parsing & plan_v2 validation
        {
            // CMake typed backend spec
            auto cmake_spec_recipe = sage::package::Recipe::parse_toml(R"(
schema_version = 2
[package]
name = "typed-cmake"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "x86_64"
[build]
system = "cmake"
payload = "allowlist"
install_files = ["usr/bin/**"]
[build.cmake]
build_type = "Debug"
definitions = { "ENABLE_FOO" = "ON", "FOO_DIR" = "/usr/share/foo" }
features = ["lto", "STATIC=OFF"]
raw_options = ["-Wno-dev"]
)");
            if (!cmake_spec_recipe || !cmake_spec_recipe->managed_build.cmake
                || cmake_spec_recipe->managed_build.cmake->build_type != "Debug"
                || cmake_spec_recipe->managed_build.cmake->definitions.at("ENABLE_FOO") != "ON"
                || cmake_spec_recipe->managed_build.cmake->definitions.at("FOO_DIR") != "/usr/share/foo"
                || cmake_spec_recipe->managed_build.cmake->features.size() != 2
                || cmake_spec_recipe->managed_build.cmake->raw_options != std::vector<std::string>{"-Wno-dev"}) {
                sage::util::log_error("CMake typed backend spec did not parse correctly");
                return 1;
            }
            auto cmake_plan = sage::build::plan_v2(
                *cmake_spec_recipe, managed_cfg,
                {.source = "/tmp/cmake-src", .package = "/tmp/cmake-pkg"},
                kernel_tools, 4);
            const auto cmake_conf = cmake_plan
                ? std::ranges::find(cmake_plan->steps, "configure", &sage::build::BuildStep::name)
                : std::vector<sage::build::BuildStep>::iterator{};
            if (!cmake_plan || cmake_conf == cmake_plan->steps.end()
                || !cmake_conf->command.contains("-DCMAKE_BUILD_TYPE=Debug")
                || !cmake_conf->command.contains("-DENABLE_FOO=ON")
                || !cmake_conf->command.contains("-DFOO_DIR=/usr/share/foo")
                || !cmake_conf->command.contains("-Dlto=ON")
                || !cmake_conf->command.contains("-DSTATIC=OFF")
                || !cmake_conf->command.contains("-Wno-dev")) {
                sage::util::log_error("CMake typed backend plan did not emit expected configure arguments");
                return 1;
            }
            auto checked_plan_recipe = *cmake_spec_recipe;
            checked_plan_recipe.check_deps = {"pkg-config >= 1"};
            checked_plan_recipe.managed_build.steps.push_back({
                .name = "run-tests", .phase = "check", .cwd = "build",
                .command = "ctest --output-on-failure"});
            auto checked_plan = sage::build::plan_v2(
                checked_plan_recipe, managed_cfg,
                {.source = "/tmp/cmake-src", .package = "/tmp/cmake-pkg"},
                kernel_tools, 4);
            const auto check_index = checked_plan
                ? std::ranges::find(checked_plan->steps, "custom-run-tests",
                    &sage::build::BuildStep::name)
                : std::vector<sage::build::BuildStep>::iterator{};
            const auto build_index = checked_plan
                ? std::ranges::find(checked_plan->steps, "build",
                    &sage::build::BuildStep::name)
                : std::vector<sage::build::BuildStep>::iterator{};
            const auto install_index = checked_plan
                ? std::ranges::find(checked_plan->steps, "install",
                    &sage::build::BuildStep::name)
                : std::vector<sage::build::BuildStep>::iterator{};
            if (!checked_plan || check_index == checked_plan->steps.end()
                || build_index == checked_plan->steps.end()
                || install_index == checked_plan->steps.end()
                || !(build_index < check_index && check_index < install_index)
                || check_index->work_dir != "/tmp/cmake-src/build") {
                sage::util::log_error(
                    "Recipe v2 check phase was not placed between build and install");
                return 1;
            }

            auto cached_tools = kernel_tools;
            cached_tools.cc_cache_for_build = "/tmp/cache-bin/clang";
            cached_tools.cxx_cache_for_build = "/tmp/cache-bin/clang++";
            cached_tools.cache_for_build = "/usr/bin/ccache";
            cached_tools.compiler_cache_mode = "ccache";
            auto cached_plan = sage::build::plan_v2(
                *cmake_spec_recipe, managed_cfg,
                {.source = "/tmp/cmake-src", .package = "/tmp/cmake-pkg"},
                cached_tools, 4);
            const auto cached_conf = cached_plan
                ? std::ranges::find(cached_plan->steps, "configure",
                    &sage::build::BuildStep::name)
                : std::vector<sage::build::BuildStep>::iterator{};
            if (!cached_plan || cached_plan->environment["CC"] != "/tmp/cache-bin/clang"
                || cached_plan->environment["CXX"] != "/tmp/cache-bin/clang++"
                || cached_conf == cached_plan->steps.end()
                || !cached_conf->command.contains(
                    "CMAKE_C_COMPILER_LAUNCHER=/usr/bin/ccache")
                || !cached_conf->command.contains(
                    "CMAKE_CXX_COMPILER_LAUNCHER=/usr/bin/ccache")) {
                sage::util::log_error(
                    "Recipe v2 compiler cache was not wired into CMake");
                return 1;
            }

            // Meson typed backend spec
            auto meson_spec_recipe = sage::package::Recipe::parse_toml(R"(
schema_version = 2
[package]
name = "typed-meson"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "x86_64"
[build]
system = "meson"
payload = "allowlist"
install_files = ["usr/bin/**"]
[build.meson]
build_type = "debugoptimized"
options = { "b_lto" = "true", "feature_x" = "enabled" }
raw_options = ["--warnlevel=2"]
)");
            if (!meson_spec_recipe || !meson_spec_recipe->managed_build.meson
                || meson_spec_recipe->managed_build.meson->build_type != "debugoptimized"
                || meson_spec_recipe->managed_build.meson->options.at("b_lto") != "true"
                || meson_spec_recipe->managed_build.meson->options.at("feature_x") != "enabled"
                || meson_spec_recipe->managed_build.meson->raw_options != std::vector<std::string>{"--warnlevel=2"}) {
                sage::util::log_error("Meson typed backend spec did not parse correctly");
                return 1;
            }
            auto meson_plan = sage::build::plan_v2(
                *meson_spec_recipe, managed_cfg,
                {.source = "/tmp/meson-src", .package = "/tmp/meson-pkg"},
                kernel_tools, 4);
            const auto meson_conf = meson_plan
                ? std::ranges::find(meson_plan->steps, "configure", &sage::build::BuildStep::name)
                : std::vector<sage::build::BuildStep>::iterator{};
            if (!meson_plan || meson_conf == meson_plan->steps.end()
                || !meson_conf->command.contains("--buildtype=debugoptimized")
                || !meson_conf->command.contains("-Db_lto=true")
                || !meson_conf->command.contains("-Dfeature_x=enabled")
                || !meson_conf->command.contains("--warnlevel=2")) {
                sage::util::log_error("Meson typed backend plan did not emit expected configure arguments");
                return 1;
            }

            // Cargo typed backend spec
            auto cargo_spec_recipe = sage::package::Recipe::parse_toml(R"(
schema_version = 2
[package]
name = "typed-cargo"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "x86_64"
[build]
system = "cargo"
payload = "all"
[build.cargo]
features = ["derive", "alloc"]
default_features = false
locked = true
raw_options = ["--verbose"]
)");
            if (!cargo_spec_recipe || !cargo_spec_recipe->managed_build.cargo
                || cargo_spec_recipe->managed_build.cargo->features != std::vector<std::string>{"derive", "alloc"}
                || cargo_spec_recipe->managed_build.cargo->default_features != false
                || !cargo_spec_recipe->managed_build.cargo->locked
                || cargo_spec_recipe->managed_build.cargo->raw_options != std::vector<std::string>{"--verbose"}) {
                sage::util::log_error("Cargo typed backend spec did not parse correctly");
                return 1;
            }
            auto cargo_plan = sage::build::plan_v2(
                *cargo_spec_recipe, managed_cfg,
                {.source = "/tmp/cargo-src", .package = "/tmp/cargo-pkg"},
                kernel_tools, 4);
            const auto cargo_bld = cargo_plan
                ? std::ranges::find(cargo_plan->steps, "build", &sage::build::BuildStep::name)
                : std::vector<sage::build::BuildStep>::iterator{};
            if (!cargo_plan || cargo_bld == cargo_plan->steps.end()
                || !cargo_bld->command.contains("--no-default-features")
                || !cargo_bld->command.contains("--features 'derive,alloc'")
                || !cargo_bld->command.contains("--locked")
                || !cargo_bld->command.contains("--verbose")) {
                sage::util::log_error("Cargo typed backend plan did not emit expected build arguments");
                return 1;
            }

            // Autotools typed backend spec
            auto autotools_spec_recipe = sage::package::Recipe::parse_toml(R"(
schema_version = 2
[package]
name = "typed-autotools"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "x86_64"
[build]
system = "autotools"
payload = "all"
[build.autotools]
enable = ["silent-rules", "shared"]
disable = ["static", "nls"]
with = ["zlib"]
without = ["readline"]
raw_options = ["--disable-rpath"]
)");
            if (!autotools_spec_recipe || !autotools_spec_recipe->managed_build.autotools
                || autotools_spec_recipe->managed_build.autotools->enable != std::vector<std::string>{"silent-rules", "shared"}
                || autotools_spec_recipe->managed_build.autotools->disable != std::vector<std::string>{"static", "nls"}
                || autotools_spec_recipe->managed_build.autotools->with != std::vector<std::string>{"zlib"}
                || autotools_spec_recipe->managed_build.autotools->without != std::vector<std::string>{"readline"}
                || autotools_spec_recipe->managed_build.autotools->raw_options != std::vector<std::string>{"--disable-rpath"}) {
                sage::util::log_error("Autotools typed backend spec did not parse correctly");
                return 1;
            }
            auto autotools_plan = sage::build::plan_v2(
                *autotools_spec_recipe, managed_cfg,
                {.source = "/tmp/autotools-src", .package = "/tmp/autotools-pkg"},
                kernel_tools, 4);
            const auto autotools_conf = autotools_plan
                ? std::ranges::find(autotools_plan->steps, "configure", &sage::build::BuildStep::name)
                : std::vector<sage::build::BuildStep>::iterator{};
            if (!autotools_plan || autotools_conf == autotools_plan->steps.end()
                || !autotools_conf->command.contains("--enable-silent-rules")
                || !autotools_conf->command.contains("--enable-shared")
                || !autotools_conf->command.contains("--disable-static")
                || !autotools_conf->command.contains("--disable-nls")
                || !autotools_conf->command.contains("--with-zlib")
                || !autotools_conf->command.contains("--without-readline")
                || !autotools_conf->command.contains("--disable-rpath")) {
                sage::util::log_error("Autotools typed backend plan did not emit expected configure arguments");
                return 1;
            }

            // Make typed backend spec
            auto make_spec_recipe = sage::package::Recipe::parse_toml(R"(
schema_version = 2
[package]
name = "typed-make"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "x86_64"
[build]
system = "make"
payload = "all"
[build.make]
targets = ["all", "docs"]
install_targets = ["install", "install-data"]
variables = { "VERBOSE" = "1", "DEBUG" = "0" }
raw_options = ["--keep-going"]
)");
            if (!make_spec_recipe || !make_spec_recipe->managed_build.make
                || make_spec_recipe->managed_build.make->targets != std::vector<std::string>{"all", "docs"}
                || make_spec_recipe->managed_build.make->install_targets != std::vector<std::string>{"install", "install-data"}
                || make_spec_recipe->managed_build.make->variables.at("VERBOSE") != "1"
                || make_spec_recipe->managed_build.make->variables.at("DEBUG") != "0"
                || make_spec_recipe->managed_build.make->raw_options != std::vector<std::string>{"--keep-going"}) {
                sage::util::log_error("Make typed backend spec did not parse correctly");
                return 1;
            }
            auto make_plan = sage::build::plan_v2(
                *make_spec_recipe, managed_cfg,
                {.source = "/tmp/make-src", .package = "/tmp/make-pkg"},
                kernel_tools, 4);
            const auto make_bld = make_plan
                ? std::ranges::find(make_plan->steps, "build", &sage::build::BuildStep::name)
                : std::vector<sage::build::BuildStep>::iterator{};
            const auto make_inst = make_plan
                ? std::ranges::find(make_plan->steps, "install", &sage::build::BuildStep::name)
                : std::vector<sage::build::BuildStep>::iterator{};
            if (!make_plan || make_bld == make_plan->steps.end() || make_inst == make_plan->steps.end()
                || !make_bld->command.contains("VERBOSE='1'")
                || !make_bld->command.contains("DEBUG='0'")
                || !make_bld->command.contains("--keep-going")
                || !make_bld->command.contains("'all'")
                || !make_bld->command.contains("'docs'")
                || !make_inst->command.contains("'install'")
                || !make_inst->command.contains("'install-data'")
                || !make_inst->command.contains("--keep-going")) {
                sage::util::log_error("Make typed backend plan did not emit expected build/install arguments: build='{}' install='{}'",
                    make_bld == make_plan->steps.end() ? "" : make_bld->command,
                    make_inst == make_plan->steps.end() ? "" : make_inst->command);
                return 1;
            }
            sage::util::log_success("   Typed Backend Specs ([build.cmake], [build.meson], etc.) Planning OK");
        }

        // (h) Multi-Output Exhaustiveness & Unassigned Payload Error
        {
            auto exhaust_root = temp_dir / "bcfg-exhaust-root";
            auto exhaust_dir = temp_dir / "bcfg-exhaust";
            if (!write_build_toml(exhaust_root, R"(cc = "gcc"
cxx = "g++"
linker = "ld"
)") || !write_canary_recipe(exhaust_dir, R"(schema_version = 2
[package]
name = "exhaust-canary"
version = "1.0.0"
release = "1"
description = "exhaustiveness canary"
license = "MIT"
channel = "system"
arch = "x86_64"

[build]
system = "make"
payload = "outputs"
outputs = [
  { name = "exhaust-bin", install_files = ["usr/bin/**"] },
]
)")) {
                sage::util::log_error("Failed to create exhaustiveness fixture");
                return 1;
            }
            {
                std::ofstream makefile(exhaust_dir / "Makefile");
                makefile << R"(all:
	printf 'int main(void){return 0;}\n' > test.c
	$(CC) test.c -o test-bin
install:
	mkdir -p $(DESTDIR)/usr/bin $(DESTDIR)/usr/share/doc
	cp test-bin $(DESTDIR)/usr/bin/test-bin
	printf 'unassigned doc\n' > $(DESTDIR)/usr/share/doc/unassigned.txt
)";
            }
            CliOptions exhaust_opts;
            exhaust_opts.args = {exhaust_dir.string()};
            exhaust_opts.target_root = exhaust_root;
            // Build MUST fail because usr/share/doc/unassigned.txt is unassigned in outputs mode
            if (cmd_build(exhaust_opts) == 0) {
                sage::util::log_error("Multi-output build did not reject unassigned payload in DESTDIR");
                return 1;
            }

            // Now add an output claiming usr/share/** and verify build succeeds
            if (!write_canary_recipe(exhaust_dir, R"(schema_version = 2
[package]
name = "exhaust-canary"
version = "1.0.0"
release = "1"
description = "exhaustiveness canary"
license = "MIT"
channel = "system"
arch = "x86_64"

[build]
system = "make"
payload = "outputs"
outputs = [
  { name = "exhaust-bin", install_files = ["usr/bin/**"] },
  { name = "exhaust-doc", install_files = ["usr/share/**"] },
]
)")) {
                sage::util::log_error("Failed to update exhaustiveness fixture");
                return 1;
            }
            if (cmd_build(exhaust_opts) != 0
                || !std::filesystem::exists(exhaust_dir / "exhaust-bin-1.0.0-1-x86_64.pkg.tar.zst")
                || !std::filesystem::exists(exhaust_dir / "exhaust-doc-1.0.0-1-x86_64.pkg.tar.zst")) {
                sage::util::log_error("Complete multi-output build failed to produce both packages");
                return 1;
            }
            sage::util::log_success("   Multi-Output Exhaustiveness & Unassigned Payload Check OK");
        }

        // (i) Exclude Matching & optional_excludes Validation
        {
            auto excl_root = temp_dir / "bcfg-excl-root";
            auto excl_dir = temp_dir / "bcfg-excl";
            if (!write_build_toml(excl_root, R"(cc = "gcc"
cxx = "g++"
linker = "ld"
)") || !write_canary_recipe(excl_dir, R"(schema_version = 2
[package]
name = "excl-canary"
version = "1.0.0"
release = "1"
description = "exclude matching canary"
license = "MIT"
channel = "system"
arch = "x86_64"

[build]
system = "make"
payload = "allowlist"
install_files = ["usr/bin/**"]
install_excludes = ["usr/bin/nonexistent*"]
)")) {
                sage::util::log_error("Failed to create exclude matching fixture");
                return 1;
            }
            {
                std::ofstream makefile(excl_dir / "Makefile");
                makefile << R"(all:
	printf 'int main(void){return 0;}\n' > test.c
	$(CC) test.c -o real-bin
install:
	mkdir -p $(DESTDIR)/usr/bin
	cp real-bin $(DESTDIR)/usr/bin/real-bin
)";
            }
            CliOptions excl_opts;
            excl_opts.args = {excl_dir.string()};
            excl_opts.target_root = excl_root;
            // Non-matching exclude MUST trigger error when not listed in optional_excludes
            if (cmd_build(excl_opts) == 0) {
                sage::util::log_error("Non-matching install_excludes was not rejected");
                return 1;
            }

            // Adding it to optional_excludes makes the build succeed
            if (!write_canary_recipe(excl_dir, R"(schema_version = 2
[package]
name = "excl-canary"
version = "1.0.0"
release = "1"
description = "exclude matching canary"
license = "MIT"
channel = "system"
arch = "x86_64"

[build]
system = "make"
payload = "allowlist"
install_files = ["usr/bin/**"]
install_excludes = ["usr/bin/nonexistent*"]
optional_excludes = ["usr/bin/nonexistent*"]
)")) {
                sage::util::log_error("Failed to update optional_excludes fixture");
                return 1;
            }
            if (cmd_build(excl_opts) != 0
                || !std::filesystem::exists(excl_dir / "excl-canary-1.0.0-1-x86_64.pkg.tar.zst")) {
                sage::util::log_error("Build failed despite non-matching exclude being in optional_excludes");
                return 1;
            }
            sage::util::log_success("   Exclude Matching & optional_excludes Validation OK");
        }

        // (j) Output Metadata Inheritance & Safe Defaults
        {
            auto inherit_root = temp_dir / "bcfg-inherit-root";
            auto inherit_dir = temp_dir / "bcfg-inherit";
            if (!write_build_toml(inherit_root, R"(cc = "gcc"
cxx = "g++"
linker = "ld"
)") || !write_canary_recipe(inherit_dir, R"(schema_version = 2
[package]
name = "inherit-root"
version = "2.0.0"
release = "1"
description = "Root package description"
license = "Apache-2.0"
channel = "system"
arch = "x86_64"
dependencies = ["glibc >= 2.40"]
provides = ["so:libinherit.so.1"]
conflicts = ["legacy-inherit < 2"]
conffiles = ["/etc/inherit.conf"]

[build]
system = "make"
payload = "outputs"
outputs = [
  { name = "inherit-default", install_files = ["usr/bin/**"] },
  { name = "inherit-custom", inherit = ["dependencies", "provides", "conflicts", "conffiles"], description = "Custom sub description", install_files = ["usr/lib/**", "etc/**"] },
]
)")) {
                sage::util::log_error("Failed to create metadata inheritance fixture");
                return 1;
            }
            {
                std::ofstream makefile(inherit_dir / "Makefile");
                makefile << R"(all:
	printf 'int main(void){return 0;}\n' > test.c
	$(CC) test.c -o test-bin
install:
	mkdir -p $(DESTDIR)/usr/bin $(DESTDIR)/usr/lib $(DESTDIR)/etc
	cp test-bin $(DESTDIR)/usr/bin/test-bin
	printf 'fake lib\n' > $(DESTDIR)/usr/lib/libinherit.so
	printf 'conf\n' > $(DESTDIR)/etc/inherit.conf
)";
            }
            CliOptions inherit_opts;
            inherit_opts.args = {inherit_dir.string()};
            inherit_opts.target_root = inherit_root;
            inherit_opts.no_elf_check = true;
            if (cmd_build(inherit_opts) != 0) {
                sage::util::log_error("Failed to build multi-output inheritance fixture");
                return 1;
            }
            auto def_pkg = sage::archive::extract_package(
                inherit_dir / "inherit-default-2.0.0-1-x86_64.pkg.tar.zst",
                temp_dir / "inherit-default-extracted");
            auto cust_pkg = sage::archive::extract_package(
                inherit_dir / "inherit-custom-2.0.0-1-x86_64.pkg.tar.zst",
                temp_dir / "inherit-custom-extracted");
            if (!def_pkg || !cust_pkg) {
                sage::util::log_error("Failed to extract built multi-output packages");
                return 1;
            }
            // Default output inherits name, version, license, description, but NOT root dependencies/provides/conflicts/conffiles
            if (def_pkg->manifest.name != "inherit-default"
                || def_pkg->manifest.description != "Root package description"
                || def_pkg->manifest.license != "Apache-2.0"
                || std::ranges::any_of(def_pkg->manifest.dependencies, [](const auto& d) { return d.name == "glibc"; })
                || std::ranges::any_of(def_pkg->manifest.provides, [](const auto& p) { return p == "so:libinherit.so.1"; })
                || !def_pkg->manifest.conflicts.empty()
                || !def_pkg->manifest.conffiles.empty()) {
                sage::util::log_error("Default output did not isolate dependencies/provides/conflicts/conffiles");
                return 1;
            }
            // Custom output with inherit = [...] inherits dependencies, provides, conflicts, conffiles
            if (cust_pkg->manifest.name != "inherit-custom"
                || cust_pkg->manifest.description != "Custom sub description"
                || cust_pkg->manifest.dependencies.empty()
                || cust_pkg->manifest.dependencies.front().name != "glibc"
                || !std::ranges::contains(cust_pkg->manifest.provides, "so:libinherit.so.1")
                || cust_pkg->manifest.conflicts.empty()
                || cust_pkg->manifest.conflicts.front().name != "legacy-inherit"
                || cust_pkg->manifest.conffiles != std::vector<std::string>{"/etc/inherit.conf"}) {
                sage::util::log_error("Custom output did not inherit requested metadata channels");
                return 1;
            }
            sage::util::log_success("   Output Metadata Inheritance & Safe Defaults OK");
        }

        // (k) Per-Output Transforms & File Permissions
        {
            auto tf_root = temp_dir / "bcfg-tf-root";
            auto tf_dir = temp_dir / "bcfg-tf";
            if (!write_build_toml(tf_root, R"(cc = "gcc"
cxx = "g++"
linker = "ld"
)") || !write_canary_recipe(tf_dir, R"(schema_version = 2
[package]
name = "tf-canary"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "x86_64"

[build]
system = "make"
payload = "outputs"
[[build.outputs]]
name = "tf-pkg"
install_files = ["usr/**"]
install_copies = [{ from = "extra.dat", to = "usr/share/extra.dat" }]
install_symlinks = [{ path = "usr/bin/tool-symlink", target = "tool" }]
install_moves = [{ from = "usr/share/old.txt", to = "usr/share/new.txt" }]
install_removes = ["usr/share/unwanted/**"]
install_generates = [{ path = "etc/generated.conf", content = "magic_key = 12345\n", mode = 420 }]
file_permissions = [{ path = "usr/bin/admin-tool", mode = 2541, uid = 0, gid = 0, caps = "cap_net_admin=+ep" }]
)")) {
                sage::util::log_error("Failed to create per-output transforms fixture");
                return 1;
            }
            {
                std::ofstream extra_file(tf_dir / "extra.dat");
                extra_file << "copied extra content\n";
                std::ofstream makefile(tf_dir / "Makefile");
                makefile << R"(all:
	printf 'int main(void){return 0;}\n' > test.c
	$(CC) test.c -o tool
	cp tool admin-tool
install:
	mkdir -p $(DESTDIR)/usr/bin $(DESTDIR)/usr/share/unwanted
	cp tool $(DESTDIR)/usr/bin/tool
	cp admin-tool $(DESTDIR)/usr/bin/admin-tool
	printf 'old doc\n' > $(DESTDIR)/usr/share/old.txt
	printf 'garbage\n' > $(DESTDIR)/usr/share/unwanted/junk.txt
)";
            }
            CliOptions tf_opts;
            tf_opts.args = {tf_dir.string()};
            tf_opts.target_root = tf_root;
            tf_opts.no_elf_check = true;
            if (cmd_build(tf_opts) != 0) {
                sage::util::log_error("Failed to build per-output transforms recipe");
                return 1;
            }
            auto tf_pkg_path = tf_dir / "tf-pkg-1.0.0-1-x86_64.pkg.tar.zst";
            auto tf_inspected = sage::archive::inspect_package(tf_pkg_path);
            auto tf_extracted = sage::archive::extract_package(tf_pkg_path, temp_dir / "tf-extracted");
            if (!tf_inspected || !tf_extracted) {
                sage::util::log_error("Failed to inspect/extract per-output transforms package");
                return 1;
            }
            auto read_tf_file = [](const std::filesystem::path& p) {
                std::ifstream f(p);
                std::stringstream ss;
                ss << f.rdbuf();
                return ss.str();
            };
            if (read_tf_file(temp_dir / "tf-extracted/usr/share/extra.dat") != "copied extra content\n"
                || !std::filesystem::is_symlink(temp_dir / "tf-extracted/usr/bin/tool-symlink")
                || std::filesystem::read_symlink(temp_dir / "tf-extracted/usr/bin/tool-symlink").generic_string() != "tool"
                || read_tf_file(temp_dir / "tf-extracted/usr/share/new.txt") != "old doc\n"
                || std::filesystem::exists(temp_dir / "tf-extracted/usr/share/old.txt")
                || std::filesystem::exists(temp_dir / "tf-extracted/usr/share/unwanted")
                || read_tf_file(temp_dir / "tf-extracted/etc/generated.conf") != "magic_key = 12345\n") {
                sage::util::log_error("Per-output transforms (copies/symlinks/moves/removes/generates) did not apply accurately");
                return 1;
            }
            auto admin_entry = std::ranges::find(tf_inspected->data_files, "usr/bin/admin-tool",
                &sage::package::FileEntry::path);
            if (admin_entry == tf_inspected->data_files.end()
                || admin_entry->mode != 04755
                || admin_entry->caps != "cap_net_admin=+ep") {
                sage::util::log_error("Per-output file_permissions was not preserved in files.idx");
                return 1;
            }
            sage::util::log_success("   Per-Output Transforms & File Permissions OK");
        }

        // (l) Build Attestation Generation (.METADATA/build-attestation.toml)
        {
            auto att_root = temp_dir / "bcfg-att-root";
            auto att_dir = temp_dir / "bcfg-att";
            if (!write_build_toml(att_root, R"(cc = "gcc"
cxx = "g++"
linker = "ld"
cflags = "-O2 -pipe"
)") || !write_canary_recipe(att_dir, R"(schema_version = 2
[package]
name = "att-canary"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "x86_64"

[build]
system = "make"
payload = "allowlist"
install_files = ["usr/bin/**"]

[build.flag_env]
cflags = ["KCFLAGS"]

[build.toolchain.compiler]
family = "gcc"
package = "gcc"
minimum_version = "1"

[build.toolchain.linker]
family = "ld"
package = "binutils"
minimum_version = "1"
)")) {
                sage::util::log_error("Failed to create build attestation fixture");
                return 1;
            }
            {
                std::ofstream makefile(att_dir / "Makefile");
                makefile << R"(all:
	printf 'int main(void){return 0;}\n' > test.c
	$(CC) $(CFLAGS) test.c -o att-tool
install:
	mkdir -p $(DESTDIR)/usr/bin
	cp att-tool $(DESTDIR)/usr/bin/att-tool
)";
            }
            CliOptions att_opts;
            att_opts.args = {att_dir.string()};
            att_opts.target_root = att_root;
            att_opts.no_elf_check = true;
            if (cmd_build(att_opts) != 0) {
                sage::util::log_error("Failed to build attestation test package");
                return 1;
            }
            auto att_pkg_path = att_dir / "att-canary-1.0.0-1-x86_64.pkg.tar.zst";
            auto att_inspected = sage::archive::inspect_package(att_pkg_path);
            if (!att_inspected || att_inspected->manifest.attestation_toml.empty()) {
                sage::util::log_error("Built package manifest did not carry build-attestation.toml");
                return 1;
            }
            auto parsed_att = sage::package::BuildAttestation::parse_toml(
                att_inspected->manifest.attestation_toml);
            if (!parsed_att
                || parsed_att->schema_version != 2
                || parsed_att->package.name != "att-canary"
                || parsed_att->package.version != "1.0.0"
                || parsed_att->host_arch != sage::config::native_package_architecture()
                || parsed_att->target_arch != sage::config::native_package_architecture()
                || parsed_att->host_triplet != sage::config::native_target_triplet()
                || parsed_att->target_triplet != sage::config::native_target_triplet()
                || parsed_att->exec_audit_digest.empty()
                || !parsed_att->check_dependencies.empty()
                || parsed_att->tools.empty()) {
                sage::util::log_error("Build attestation in built archive is invalid or incomplete");
                return 1;
            }
            sage::util::log_success("   Build Attestation Generation (.METADATA/build-attestation.toml) OK");
        }

        // (m) --check-reproducible Mode Validation
        {
            auto repro_root = temp_dir / "bcfg-repro-root";
            auto repro_dir = temp_dir / "bcfg-repro";
            if (!write_build_toml(repro_root, R"(cc = "gcc"
cxx = "g++"
linker = "ld"
)") || !write_canary_recipe(repro_dir, R"(schema_version = 2
[package]
name = "repro-canary"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "x86_64"

[build]
system = "make"
payload = "allowlist"
install_files = ["usr/bin/**"]
)")) {
                sage::util::log_error("Failed to create reproducibility fixture");
                return 1;
            }
            {
                std::ofstream makefile(repro_dir / "Makefile");
                makefile << R"(all:
	printf 'int main(void){return 0;}\n' > test.c
	$(CC) test.c -o repro-bin
install:
	mkdir -p $(DESTDIR)/usr/bin
	cp repro-bin $(DESTDIR)/usr/bin/repro-bin
)";
            }
            // (1) Deterministic build with --check-reproducible MUST pass
            CliOptions repro_opts;
            repro_opts.args = {repro_dir.string()};
            repro_opts.target_root = repro_root;
            repro_opts.no_elf_check = true;
            repro_opts.check_reproducible = true;
            if (cmd_build(repro_opts) != 0) {
                sage::util::log_error("--check-reproducible failed on a deterministic build");
                return 1;
            }

            // (2) Non-deterministic build with --check-reproducible MUST fail
            auto nonrepro_dir = temp_dir / "bcfg-nonrepro";
            if (!write_canary_recipe(nonrepro_dir, R"(schema_version = 2
[package]
name = "nonrepro-canary"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "x86_64"

[build]
system = "make"
payload = "allowlist"
install_files = ["usr/share/**"]
)")) {
                sage::util::log_error("Failed to create non-reproducible fixture");
                return 1;
            }
            {
                std::ofstream makefile(nonrepro_dir / "Makefile");
                // Writing a nanosecond timestamp or random token ensures bit divergence across 2 passes
                makefile << R"(all:
install:
	mkdir -p $(DESTDIR)/usr/share
	head -c 32 /dev/urandom > $(DESTDIR)/usr/share/stamp.txt
)";
            }
            CliOptions nonrepro_opts;
            nonrepro_opts.args = {nonrepro_dir.string()};
            nonrepro_opts.target_root = repro_root;
            nonrepro_opts.check_reproducible = true;
            if (cmd_build(nonrepro_opts) == 0) {
                sage::util::log_error("--check-reproducible did not catch non-deterministic build artifact divergence");
                return 1;
            }
            sage::util::log_success("   --check-reproducible Mode Validation OK");
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
    {
        // 16. Header-only Exemption & [[vendor]] Declarative Dependencies
        auto temp_dir = std::filesystem::temp_directory_path()
            / std::format("sage_header_vendor_test_{}", sage::util::current_pid());
        std::error_code ec;
        std::filesystem::remove_all(temp_dir, ec);
        std::filesystem::create_directories(temp_dir, ec);

        // Test 16a: Header-only Recipe v2
        auto ho_dir = temp_dir / "headerpkg";
        std::filesystem::create_directories(ho_dir / "include/mylib");
        {
            std::ofstream h(ho_dir / "include/mylib/header.h");
            h << "#pragma once\ninline int answer() { return 42; }\n";
        }
        {
            std::ofstream r(ho_dir / "recipe.toml");
            r << R"(schema_version = 2
[package]
name = "headerpkg"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "any"

[build]
system = "script"
payload = "all"
header_only = true
steps = [{ name = "stage", phase = "install", command = "true" }]
install_copies = [{ from = "include/mylib/header.h", to = "usr/include/mylib/header.h" }]
)";
        }

        CliOptions ho_build;
        ho_build.args = {ho_dir.string()};
        ho_build.target_root = temp_dir / "target";
        if (cmd_build(ho_build) != 0) {
            sage::util::log_error("Failed to build header_only recipe");
            return 1;
        }

        auto ho_pkg = ho_dir / "headerpkg-1.0.0-1-any.pkg.tar.zst";
        auto ho_unpacked = sage::archive::extract_package(ho_pkg, temp_dir / "ho_unpacked");
        if (!ho_unpacked || !std::filesystem::exists(temp_dir / "ho_unpacked/usr/include/mylib/header.h")) {
            sage::util::log_error("Header-only package contents missing or invalid");
            return 1;
        }

        // Test 16b: Declarative [[vendor]] Pre-fetching and Unpacking
        auto dist_dir = temp_dir / "dist";
        std::filesystem::create_directories(dist_dir);
        auto make_tar_gz = [&](const std::filesystem::path& src_dir, std::string_view entry, const std::filesystem::path& out_tar_gz) {
            std::string cmd = std::format("tar -czf {} -C {} {}",
                sage::build::shell_quote(out_tar_gz.string()),
                sage::build::shell_quote(src_dir.string()),
                sage::build::shell_quote(entry));
            return std::system(cmd.c_str()) == 0;
        };

        auto vendor_staging = temp_dir / "vstaging/vendordep";
        std::filesystem::create_directories(vendor_staging);
        {
            std::ofstream vf(vendor_staging / "lib.rs");
            vf << "pub fn vendor_hello() {}\n";
        }
        auto vendor_tar = dist_dir / "vendordep-1.0.0.tar.gz";
        if (!make_tar_gz(temp_dir / "vstaging", "vendordep", vendor_tar)) {
            sage::util::log_error("Failed to create mock vendor tarball");
            return 1;
        }
        auto vendor_sha = *sage::util::compute_file_sha256(vendor_tar);

        auto vendorpkg_dir = temp_dir / "vendorpkg";
        std::filesystem::create_directories(vendorpkg_dir);
        {
            std::ofstream r(vendorpkg_dir / "recipe.toml");
            r << std::format(R"(schema_version = 2
[package]
name = "vendorpkg"
version = "1.0.0"
release = "1"
license = "MIT"
channel = "system"
arch = "any"

[[vendor]]
url = "file://{}"
sha256 = "{}"
target = "vendor"

[build]
system = "script"
payload = "all"
header_only = true
steps = [{{ name = "stage", phase = "install", command = "true" }}]
install_copies = [{{ from = "vendor/lib.rs", to = "usr/share/vendor/lib.rs" }}]
)", vendor_tar.string(), vendor_sha);
        }

        CliOptions vendor_build;
        vendor_build.args = {vendorpkg_dir.string()};
        vendor_build.target_root = temp_dir / "target";
        if (cmd_build(vendor_build) != 0) {
            sage::util::log_error("Failed to build recipe with [[vendor]] archive");
            return 1;
        }

        auto vendor_pkg = vendorpkg_dir / "vendorpkg-1.0.0-1-any.pkg.tar.zst";
        auto vendor_unpacked = sage::archive::extract_package(vendor_pkg, temp_dir / "vendor_unpacked");
        if (!vendor_unpacked || !std::filesystem::exists(temp_dir / "vendor_unpacked/usr/share/vendor/lib.rs")) {
            sage::util::log_error("Vendor archive files were not extracted or staged properly");
            return 1;
        }

        std::filesystem::remove_all(temp_dir);
        sage::util::log_success("16. Header-only Exemption & [[vendor]] Declarative Dependencies OK");
    }
    return 0;
}

} // namespace build
} // namespace sage::tests
