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
    // 13. Build configuration: global flag injection, the per-recipe [build]
    // override (which doubles as the fourth phase-command scope), and the
    // compiler fallback -- the provenance Recipedia reads from the manifest
    // and the repository index.
    {
        // Pure-model defaults: an empty document yields the built-in baseline,
        // and the shipped /etc/sage/build.toml must keep parsing to exactly
        // that -- the two are kept in lockstep by this assertion.
        auto empty_cfg = sage::config::BuildConfig::parse_toml("");
        auto shipped_cfg = sage::config::BuildConfig::parse_toml(R"(schema_version = 1
cc = "clang"
cxx = "clang++"
fallback_cc = "gcc"
fallback_cxx = "g++"
cflags = "-O3 -march=x86-64-v3"
)");
        if (!empty_cfg || !shipped_cfg || *shipped_cfg != *empty_cfg
            || empty_cfg->cc != "clang" || empty_cfg->cflags != "-O3 -march=x86-64-v3"
            || !empty_cfg->cxxflags.empty()) {
            sage::util::log_error("BuildConfig defaults or shipped-default parse drifted");
            return 1;
        }
        // Explicit job count parses through; absence stays auto (0 = nproc).
        auto jobs_cfg = sage::config::BuildConfig::parse_toml("jobs = 4\n");
        if (!jobs_cfg || jobs_cfg->jobs != 4 || empty_cfg->jobs != 0) {
            sage::util::log_error("BuildConfig jobs parsing drifted");
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

        // (a) Global baseline reaches the recipe shell and is stamped.
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
        // The compiled object carries its producer's fingerprint, so the
        // stamp names the real compiler family rather than the injected CC.
        if (!canary || canary->build_compiler.empty() || canary->build_compiler_version.empty()
            || canary->build_cflags != "-DGLOBAL_CFLAG=1"
            || canary->build_cxxflags != "-DGLOBAL_CFLAG=1"  // cxxflags mirror cflags
            || read_text(temp_dir / "bcfg-canary-x/usr/share/cflags.txt") != "-DGLOBAL_CFLAG=1"
            || read_text(temp_dir / "bcfg-canary-x/usr/share/canary.o").empty()) {
            sage::util::log_error("Global build-config injection or provenance stamping failed");
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
        if (!overridden || overridden->build_compiler.empty()
            || overridden->build_cflags != "-DLOCAL_CFLAG=2"
            || overridden->build_cxxflags != "-DLOCAL_CFLAG=2"  // mirrors the recipe cflags
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
        if (!fallback || fallback->build_compiler.empty()
            || fallback->build_cflags != "-DFALLBACK_CFLAG=3"
            || read_text(temp_dir / "bcfg-fallback-x/usr/share/cflags.txt") != "-DFALLBACK_CFLAG=3") {
            sage::util::log_error("Compiler fallback did not degrade to the configured fallback pair");
            return 1;
        }

        // (d) A package that never compiled anything claims no provenance:
        // os-release-style recipes stay silent about compilers and flags.
        auto plain_dir = temp_dir / "bcfg-plain";
        if (!write_canary_recipe(plain_dir, R"(schema_version = 1
[package]
name = "notcanary"
version = "1.0.0"
release = "1"
description = "build-config silence canary"
license = "MIT"
channel = "system"
install = [
    'mkdir -p "$DESTDIR/usr/share"',
    'printf "%s" "$CFLAGS" > "$DESTDIR/usr/share/cflags.txt"',
]
)")) {
            sage::util::log_error("Failed to create provenance-silence fixture");
            return 1;
        }
        auto plain = build_with_root(plain_dir, canary_root, temp_dir / "bcfg-plain-x",
                                     "notcanary-1.0.0-1-x86_64.pkg.tar.zst");
        if (!plain || !plain->build_compiler.empty() || !plain->build_compiler_version.empty()
            || !plain->build_cflags.empty() || !plain->build_cxxflags.empty()
            || !plain->build_ldflags.empty()) {
            sage::util::log_error("A compiler-free package must not carry build provenance");
            return 1;
        }

        // (e) The job count reaches the phase shells as MAKEFLAGS="-jN" and
        // CARGO_BUILD_JOBS="N"; unset in build.toml means one per hardware
        // thread.
        const unsigned expect_jobs = std::max(1u, std::thread::hardware_concurrency());
        auto job_dir = temp_dir / "bcfg-jobs";
        if (!write_canary_recipe(job_dir, R"(schema_version = 1
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
]
)")) {
            sage::util::log_error("Failed to create jobs canary fixture");
            return 1;
        }
        auto jobcanary = build_with_root(job_dir, canary_root, temp_dir / "bcfg-jobs-x",
                                         "jobcanary-1.0.0-1-x86_64.pkg.tar.zst");
        if (!jobcanary
            || read_text(temp_dir / "bcfg-jobs-x/usr/share/makeflags.txt") != std::format("-j{}", expect_jobs)
            || read_text(temp_dir / "bcfg-jobs-x/usr/share/cargojobs.txt") != std::to_string(expect_jobs)) {
            sage::util::log_error("MAKEFLAGS/CARGO_BUILD_JOBS did not carry the configured job count");
            return 1;
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

        // (h) Artifact-verified switches: compiling with -g makes GCC record
        // its exact command line in DW_AT_producer (.debug_str), so the
        // manifest must carry the REAL switches (-O2, -DFOO=7), not merely
        // the injected environment.
        auto vflags_dir = temp_dir / "bcfg-vflags";
        if (!write_canary_recipe(vflags_dir, R"(schema_version = 1
[package]
name = "vflagcanary"
version = "1.0.0"
release = "1"
description = "verified-switch canary"
license = "MIT"
channel = "system"

[build]
cflags = "-g -O2 -DFOO=7"
install = [
    'mkdir -p "$DESTDIR/usr/share"',
    'printf "int sage_vflag;\n" > vflag.c',
    '$CC -c $CFLAGS vflag.c -o "$DESTDIR/usr/share/vflag.o"',
]
)")) {
            sage::util::log_error("Failed to create verified-switch fixture");
            return 1;
        }
        auto vflag_pkg = build_with_root(vflags_dir, canary_root,
                                         temp_dir / "bcfg-vflags-x",
                                         "vflagcanary-1.0.0-1-x86_64.pkg.tar.zst");
        {
            bool verified = false;
            if (vflag_pkg) {
                for (const auto& p : vflag_pkg->build_producers) {
                    // GCC's -grecord-gcc-switches deliberately omits -D
                    // defines, so -O2/-g are the honest assertions here.
                    if (p.flags.find("-O2") != std::string::npos
                        && p.flags.find("-g") != std::string::npos) {
                        verified = true;
                    }
                }
            }
            if (!verified) {
                sage::util::log_error(
                    "DW_AT_producer switches were not recorded as verified flags");
                return 1;
            }
        }

        // (i) Mixed producers: a payload naming TWO compiler families must
        // yield one build_producers entry per family. A second .comment is
        // spliced in with objcopy so the test does not depend on which
        // compilers the host ships.
        if (std::system("command -v objcopy >/dev/null 2>&1") == 0) {
            auto mix_dir = temp_dir / "bcfg-mix";
            if (!write_canary_recipe(mix_dir, R"(schema_version = 1
[package]
name = "mixcanary"
version = "1.0.0"
release = "1"
description = "mixed-producer canary"
license = "MIT"
channel = "system"
install = [
    'mkdir -p "$DESTDIR/usr/share"',
    'printf "int sage_mix;\n" > mix.c',
    '$CC -c mix.c -o "$DESTDIR/usr/share/mix.o"',
]
)")) {
                sage::util::log_error("Failed to create mixed-producer fixture");
                return 1;
            }
            if (!build_with_root(mix_dir, canary_root, temp_dir / "bcfg-mix-x",
                                 "mixcanary-1.0.0-1-x86_64.pkg.tar.zst")) {
                sage::util::log_error("Mixed-producer base build failed");
                return 1;
            }
            const auto inject_dir = temp_dir / "bcfg-mix-inject";
            std::filesystem::create_directories(inject_dir);
            // Splice a second producer fingerprint by overwriting the GCC
            // .comment bytes in place (same length keeps the ELF valid) --
            // no external tools, fully deterministic.
            {
                const auto mix_obj_src = temp_dir / "bcfg-mix-x/usr/share/mix.o";
                std::ifstream in(mix_obj_src, std::ios::binary);
                std::string bytes((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
                in.close();
                const size_t at = bytes.find("GCC: (");
                if (at == std::string::npos) {
                    sage::util::log_error("No GCC fingerprint to overwrite in mix.o");
                    return 1;
                }
                const std::string inject = "rustc version 9.9.9";
                for (size_t k = 0; k < inject.size(); ++k) bytes[at + k] = inject[k];
                std::filesystem::create_directories(inject_dir);
                std::ofstream outb(mix_dir / "rust.o", std::ios::binary);
                outb.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            }
            // Keep both producer assets beside the recipe; phase shells get
            // RECIPE_DIR injected, making the copy source explicit.
            std::filesystem::copy_file(
                temp_dir / "bcfg-mix-x/usr/share/mix.o", mix_dir / "native.o",
                std::filesystem::copy_options::overwrite_existing);
            if (!write_canary_recipe(mix_dir, R"(schema_version = 1
[package]
name = "mixcanary"
version = "1.0.0"
release = "2"
description = "mixed-producer canary"
license = "MIT"
channel = "system"
install = [
    'mkdir -p "$DESTDIR/usr/share"',
    'mkdir -p "$DESTDIR/usr/share"',
    'cp "$RECIPE_DIR/native.o" "$DESTDIR/usr/share/mix.o"',
    'cp "$RECIPE_DIR/rust.o" "$DESTDIR/usr/share/mix-rust.o"',
]
)")) {
                sage::util::log_error("Failed to rewrite mixed-producer recipe");
                return 1;
            }
            auto mixed = build_with_root(mix_dir, canary_root, temp_dir / "bcfg-mix-x",
                                         "mixcanary-1.0.0-2-x86_64.pkg.tar.zst");
            bool has_rustc = false;
            bool has_native = false;
            if (mixed) {
                for (const auto& p : mixed->build_producers) {
                    if (p.name == "rustc") {
                        for (const auto& v : p.versions)
                            if (v.starts_with("9.9.9")) has_rustc = true;
                    } else if (p.name == "gcc" || p.name == "clang") {
                        has_native = true;
                    }
                }
            }
            if (!has_rustc || !has_native) {
                sage::util::log_error(
                    "Mixed rustc+C-family build did not produce one producer entry per family");
                return 1;
            }
        }

        // (j) Flag passthrough annotation round-trips: kernel-style recipes
        // forward the very same flags through KCFLAGS/KAFLAGS, and the
        // manifest must say so instead of leaving the coincidence suspicious.
        auto pass_dir = temp_dir / "bcfg-passthrough";
        if (!write_canary_recipe(pass_dir, R"(schema_version = 1
[package]
name = "passcanary"
version = "1.0.0"
release = "1"
description = "passthrough annotation canary"
license = "MIT"
channel = "system"

[build]
passthrough_flags = ["KCFLAGS", "KAFLAGS"]
install = [
    'mkdir -p "$DESTDIR/usr/share"',
    'printf ok > "$DESTDIR/usr/share/done.txt"',
]
)")) {
            sage::util::log_error("Failed to create passthrough annotation fixture");
            return 1;
        }
        auto passpkg = build_with_root(pass_dir, canary_root, temp_dir / "bcfg-pass-x",
                                       "passcanary-1.0.0-1-x86_64.pkg.tar.zst");
        if (!passpkg || passpkg->build_flag_passthrough.size() != 2
            || passpkg->build_flag_passthrough[0] != "KCFLAGS"
            || passpkg->build_flag_passthrough[1] != "KAFLAGS") {
            sage::util::log_error("Passthrough channel annotation failed to round-trip");
            return 1;
        }

        // (k) A failing phase stops the build under the toolchain that ran --
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


        sage::util::log_success("13. Build Config Injection, Recipe Override & Compiler Fallback OK");
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
