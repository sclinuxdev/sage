module;
#include <cstdint>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

export module sage.cli.build:cmd;

import std;
import sage;
import sage.vendor.libarchive;
import sage.cli;

import :probe;
import :transforms;
import :source;
import :sandbox;
import :audit;
import :toolchain;
import :elf;
import :pack;
import :repro;

namespace sage::cli {

struct HermeticCleanup {
    std::filesystem::path root;
    ~HermeticCleanup() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

export inline int cmd_build(const CliOptions& opts) {
    if (opts.args.empty()) {
        std::println("Usage: sage build <RECIPE_DIR>");
        return 1;
    }
    std::filesystem::path recipe_dir = opts.args[0];
    std::filesystem::path recipe_file = recipe_dir / "recipe.toml";
    if (!std::filesystem::exists(recipe_file)) {
        sage::util::log_error("Recipe not found: {}", recipe_file.string());
        return 1;
    }

    std::ifstream file(recipe_file);
    std::stringstream ss;
    ss << file.rdbuf();
    auto recipe_res = sage::package::Recipe::parse_toml(ss.str());
    if (!recipe_res) {
        sage::util::log_error("Failed to parse recipe: {}", recipe_res.error());
        return 1;
    }
    auto r = std::move(*recipe_res);
    if (!opts.target_triplet.empty()) {
        r.arch = sage::config::triplet_to_arch(opts.target_triplet);
    } else if (!opts.target_arch.empty()) {
        r.arch = opts.target_arch;
    }

    // Load configuration before choosing the identity: configured channel
    // indexes are Recipedia's published state and therefore the authority for
    // releases. Local build artifacts are deliberately irrelevant -- a clean
    // builder and a dirty builder must choose the same release.
    auto cfg_res = sage::config::SystemConfig::load_from_root(opts.target_root);
    if (!cfg_res) {
        sage::util::log_error("Failed to load configuration: {}", cfg_res.error());
        return 1;
    }
    const auto& cfg = *cfg_res;
    const std::string host_arch = sage::config::native_package_architecture();
    const std::string host_triplet = sage::config::native_target_triplet();
    auto target_arch = sage::config::canonical_architecture(r.arch);
    if (!opts.target_triplet.empty()) {
        target_arch = sage::config::triplet_to_arch(opts.target_triplet);
        r.arch = target_arch;
    } else if (!opts.target_arch.empty()) {
        target_arch = sage::config::canonical_architecture(opts.target_arch);
        auto architecture = sage::package::validate_package_architecture(target_arch);
        if (!architecture) {
            sage::util::log_error("{}", architecture.error());
            return 1;
        }
        r.arch = target_arch;
    }
    auto target_architecture = sage::package::validate_package_architecture(
        target_arch);
    if (!target_architecture) {
        sage::util::log_error("{}", target_architecture.error());
        return 1;
    }
    std::string target_triplet;
    if (!opts.target_triplet.empty()) {
        target_triplet = opts.target_triplet;
    } else if (!opts.target_arch.empty()) {
        target_triplet = sage::config::architecture_to_triplet(target_arch);
    } else if (target_arch == "any" || target_arch == host_arch) {
        target_triplet = host_triplet;
    } else {
        sage::util::log_error(
            "Recipe architecture '{}' differs from builder '{}'; provide --target <triplet> for a cross build",
            target_arch, host_arch);
        return 1;
    }
    std::uint64_t highest_published_release = 0;
    bool has_published_release = false;
    std::size_t active_channels = 0;
    for (const auto& configured : cfg.channels) {
        if (!configured.enabled || configured.url.empty()) continue;
        ++active_channels;
        sage::channel::Channel channel;
        channel.name = configured.name;
        channel.url = configured.url;
        channel.scope = sage::channel::parse_scope(configured.scope);
        channel.priority = configured.priority;
        channel.enabled = true;
        auto published = sage::channel::ProfileManager::sync_channel(channel, cfg.cache_dir);
        if (!published) {
            // A dead or offline channel must not kill the build: the recipe
            // declares its own release, so degrade to a warning and let that
            // value stand instead of failing every offline invocation.
            sage::util::log_warn("Cannot determine published releases from Recipedia channel '{}': {}; "
                                 "using the recipe-declared release",
                                 configured.name, published.error());
            continue;
        }
        for (std::size_t pkg_i = 0; pkg_i < published->available_packages.size(); ++pkg_i) {
            const auto& package = published->available_packages[pkg_i];
            if (package.name != r.name
                || package.channel != r.channel
                || package.version.epoch != r.version.epoch
                || package.version.ver != r.version.ver) continue;
            auto release = sage::package::parse_release(package.version.rel);
            if (!release) continue;  // Channel parsing already rejects this; defensive for constructed indexes.
            highest_published_release = std::max(highest_published_release, *release);
            has_published_release = true;
        }
    }
    if (active_channels == 0) {
        sage::util::log_info("No channels configured; release uses the recipe-declared value");
    }
    if (has_published_release) {
        if (highest_published_release == std::numeric_limits<std::uint64_t>::max()) {
            sage::util::log_error(
                "Published release range exhausted for package '{}' in channel '{}' at epoch {} version '{}': highest published release is UINT64_MAX",
                r.name, r.channel, r.version.epoch, r.version.ver);
            return 1;
        }
        auto declared_release = sage::package::parse_release(r.version.rel);
        r.version.rel = std::format("{}", std::max(
            *declared_release, highest_published_release + 1));
    }
    sage::util::log_info("Building package '{}' version {} (channel: {})...", r.name, r.version.to_string(), r.channel);

    // Load the build configuration up front: the phases need the compiler
    // and flags, and the DT_NEEDED check below reuses the same handle.
    const sage::config::BuildConfig& bcfg = cfg.build;
    if (!probe_fakeroot(bcfg.fakeroot)) {
        sage::util::log_error(
            "Configured fakeroot executable '{}' is not usable; install fakeroot "
            "or set fakeroot in {}",
            bcfg.fakeroot, cfg.build_config_path.string());
        return 1;
    }
    if (bcfg.sysroot != "/") {
        auto fakeroot_path = ToolAudit::resolve(bcfg.fakeroot);
        std::error_code root_ec;
        const auto root = std::filesystem::weakly_canonical(bcfg.sysroot, root_ec);
        std::error_code fakeroot_ec;
        const auto fakeroot = fakeroot_path
            ? std::filesystem::weakly_canonical(*fakeroot_path, fakeroot_ec)
            : std::filesystem::path{};
        std::error_code directory_ec;
        if (root_ec || fakeroot.empty()
            || fakeroot_ec || !std::filesystem::is_directory(root, directory_ec)
            || !(fakeroot == root
                || fakeroot.string().starts_with(root.string() + "/"))) {
            sage::util::log_error(
                "Configured fakeroot '{}' must be inside the complete build sysroot '{}'",
                bcfg.fakeroot, bcfg.sysroot.string());
            return 1;
        }
    }
    if (auto check = validate_check_dependencies(r, cfg); !check) {
        sage::util::log_error("Cannot provision check dependencies for '{}': {}",
                              r.name, check.error());
        return 1;
    }
    const bool needs_managed_toolchain =
        r.schema_version == 2
        && (r.managed_build.system != sage::package::BuildSystem::Script
            || r.managed_build.script_managed_tools);
    const auto requested_cache = needs_managed_toolchain
        ? bcfg.compiler_cache_mode() : std::string_view{"none"};
    auto cache_mode_res = select_compiler_cache(
        requested_cache, r.managed_build.system, bcfg.sysroot);
    if (!cache_mode_res) {
        sage::util::log_error("Cannot configure compiler cache: {}",
                              cache_mode_res.error());
        return 1;
    }
    const std::string active_cache_mode = std::move(*cache_mode_res);
    if (requested_cache == "auto" && active_cache_mode == "none")
        sage::util::log_warn(
            "compiler_cache=auto requested, but neither sccache nor ccache is available; continuing without a cache");

    // Per-recipe [build] overrides replace the global baseline; cxxflags
    // mirror cflags at whichever level does not spell them out.
    const std::string eff_cflags = !r.cflags.empty() ? r.cflags : bcfg.cflags;
    const std::string eff_cxxflags = !r.cxxflags.empty() ? r.cxxflags
                                   : !r.cflags.empty() ? r.cflags
                                   : !bcfg.cxxflags.empty() ? bcfg.cxxflags
                                   : bcfg.cflags;

    // Parallelism inside this one package build. compile_jobs is independent
    // from Sage's multi-package I/O concurrency; when absent it inherits the
    // historical `jobs` setting so existing build.toml files keep working.
    const int requested_compile_jobs = bcfg.configured_compile_jobs();
    const unsigned compile_jobs = requested_compile_jobs > 0
        ? static_cast<unsigned>(requested_compile_jobs)
        : std::max(1u, std::thread::hardware_concurrency());
    const std::string jobs_makeflags = std::format("-j{} --jobserver-style=pipe", compile_jobs);

    // Candidate toolchains in priority order: the configured pair first, the
    // fallback pair second. Each is probed once up front -- `<cc> --version`
    // doubles as the existence check. Successful v2 builds preserve these
    // direct observations in their package manifest; v1 never does.
    //
    // A recipe-level [build] cc pins the toolchain outright: exactly that
    // pair runs and the global fallback never does -- a pinned build that
    // fails must fail the recipe rather than silently produce core system
    // packages from the wrong compiler.
    auto candidates_res = discover_candidate_toolchains(
        r, bcfg, opts.verbose);
    if (!candidates_res) return candidates_res.error();
    auto candidates = std::move(*candidates_res);

    const auto hermetic_root = std::filesystem::temp_directory_path()
        / std::format("sage-build-{}", sage::util::current_pid());
    std::filesystem::path build_home = hermetic_root / "home";
    std::filesystem::path build_temp = hermetic_root / "tmp";
    std::error_code hermetic_ec;
    std::filesystem::create_directories(build_home, hermetic_ec);
    std::filesystem::create_directories(build_temp, hermetic_ec);
    if (hermetic_ec) {
        sage::util::log_error("Cannot create hermetic build environment: {}",
                              hermetic_ec.message());
        return 1;
    }
    const auto cache_dir = active_cache_mode == "none"
        ? std::filesystem::path{} : bcfg.ccache_dir;
    HermeticCleanup hermetic_cleanup{hermetic_root};

    std::optional<ToolAudit> tool_audit;
    if (r.schema_version == 2 && !candidates.empty()) {
        auto& selected = candidates.front();
        auto canonical = sage::build::Toolchain{
            .cc = selected.cc, .cxx = selected.cxx, .linker = selected.linker,
            .rustc = selected.rustc, .go = selected.go,
            .target_triplet = target_triplet,
            .compiler_cache_mode = active_cache_mode,
            .compiler_version = selected.compiler_version,
            .cxx_version = selected.cxx_version,
            .linker_version = selected.linker_version,
            .rustc_version = selected.rustc_version,
            .go_version = selected.go_version,
            .compiler_family = selected.compiler_family,
            .cxx_family = selected.cxx_family,
            .linker_family = selected.linker_family,
            .rustc_family = selected.rustc_family,
            .go_family = selected.go_family};
        tool_audit = ToolAudit::create(
            canonical, hermetic_root, bcfg.sysroot, cache_dir, active_cache_mode);
        if (!tool_audit) {
            sage::util::log_error(
                "Cannot create the v2 tool audit fence; refusing to build without actual execution evidence");
            return 1;
        }
        if (r.managed_build.network) {
            tool_audit->sandbox = sage::build::replace_all(
                std::move(tool_audit->sandbox), " --unshare-net", "");
        }
        selected.cc_for_build = tool_audit->cc;
        selected.cxx_for_build = tool_audit->cxx;
        selected.linker_for_build = tool_audit->linker;
        selected.rustc_for_build = tool_audit->rustc;
        selected.cc_cache_for_build = tool_audit->cc_cache;
        selected.cxx_cache_for_build = tool_audit->cxx_cache;
        selected.cache_for_build = tool_audit->cache_for_build;
        selected.path_for_build = tool_audit->path;
    }

    std::filesystem::path dist_dir = recipe_dir / "distfiles";
    std::filesystem::path src_dir = r.source_url.empty()
        ? recipe_dir : recipe_dir / "src";
    std::filesystem::path pkg_dir = recipe_dir / "pkg";
    if (r.schema_version == 2) {
        src_dir = hermetic_root / "src";
        pkg_dir = hermetic_root / "pkg";
    }
    if (r.schema_version == 2 && r.source_url.empty()) {
        std::error_code source_ec;
        std::filesystem::create_directories(src_dir, source_ec);
        if (source_ec) {
            sage::util::log_error("Cannot create v2 local source directory '{}': {}",
                                  src_dir.string(), source_ec.message());
            return 1;
        }
    }
    if (tool_audit && !tool_audit->sandbox.empty()) {
        tool_audit->sandbox += " --ro-bind "
            + sage::build::shell_quote(recipe_dir.string()) + " "
            + sage::build::shell_quote(recipe_dir.string())
            + " --bind " + sage::build::shell_quote(src_dir.string()) + " "
            + sage::build::shell_quote(src_dir.string())
            + " --bind " + sage::build::shell_quote(pkg_dir.string()) + " "
            + sage::build::shell_quote(pkg_dir.string())
            + " --bind " + sage::build::shell_quote(build_home.string()) + " "
            + sage::build::shell_quote(build_home.string())
            + " --bind " + sage::build::shell_quote(build_temp.string()) + " "
            + sage::build::shell_quote(build_temp.string());
        if (auto fakeroot_path = ToolAudit::resolve(bcfg.fakeroot)) {
            std::error_code sysroot_ec;
            const auto sysroot_root = std::filesystem::weakly_canonical(
                bcfg.sysroot, sysroot_ec);
            const auto relative_root = sysroot_ec ? bcfg.sysroot : sysroot_root;
            const auto namespace_fakeroot = (std::filesystem::path("/")
                / fakeroot_path->lexically_relative(relative_root)).lexically_normal();
            const auto namespace_parent = namespace_fakeroot.parent_path();
            const bool hidden_tmp = namespace_parent.string().starts_with("/tmp/");
            tool_audit->sandbox += (hidden_tmp
                ? " --dir " + sage::build::shell_quote(namespace_parent.string()) : "")
                + " --ro-bind " + sage::build::shell_quote(fakeroot_path->string())
                + " " + sage::build::shell_quote(namespace_fakeroot.string());
        }
    }

    const bool resume_src = opts.reuse_src && !r.source_url.empty();
    if (resume_src && !std::filesystem::exists(src_dir)) {
        sage::util::log_error(
            "--reuse-src: no extracted source tree at {} to resume from",
            src_dir.string());
        return 1;
    }

    // 1. Source Fetch & SHA256 Verification -- the primary archive plus any
    // extra `[[source]]` entries. Extras land verbatim in distfiles/ next to
    // the primary and are staged into src/distfiles/ on every attempt below.
    std::filesystem::path archive_path;  // hoisted: the attempt loop re-extracts per candidate
    std::vector<std::filesystem::path> extra_paths;  // parallel to r.extra_sources
    auto fetch_source = [&](std::string_view url, std::string_view sha256,
                            std::filesystem::path& out_path) -> bool {
        if (url.empty()) return true;
        std::filesystem::create_directories(dist_dir);
        // Derive the on-disk name from the URL path alone: query strings
        // (?inline=false on GitLab raw links) and fragments are not part of
        // it and must not leak into distfiles/ filenames.
        std::string path = std::string(url.substr(0, url.find_first_of("?#")));
        std::string filename = std::filesystem::path(path).filename().string();
        if (filename.empty()) filename = "source.tar.gz";
        out_path = dist_dir / filename;

        if (!std::filesystem::exists(out_path)) {
            sage::util::log_info("Fetching source from {}...", url);
            auto dl_res = sage::vendor::curl::download_file(url, out_path);
            if (!dl_res) {
                sage::util::log_error("Failed to download source: {}", dl_res.error());
                return false;
            }
        }

        // Verify SHA256
        if (!sha256.empty()) {
            auto hash_res = sage::util::compute_file_sha256(out_path);
            if (!hash_res) {
                sage::util::log_error("Failed to compute SHA256 for source: {}", hash_res.error());
                return false;
            }
            if (*hash_res != sha256) {
                sage::util::log_error("SHA256 checksum mismatch!\n  Expected: {}\n  Actual:   {}", sha256, *hash_res);
                return false;
            }
            sage::util::log_success("Source SHA256 checksum verified: {}", *hash_res);
        }
        return true;
    };
    if (!fetch_source(r.source_url, r.source_sha256, archive_path)) return 1;
    for (const auto& extra : r.extra_sources) {
        auto& path = extra_paths.emplace_back();
        if (!fetch_source(extra.url, extra.sha256, path)) return 1;
    }
    std::vector<std::pair<std::filesystem::path, std::string>> vendor_paths;
    for (const auto& v : r.vendors) {
        std::filesystem::path vpath;
        if (!fetch_source(v.url, v.sha256, vpath)) return 1;
        vendor_paths.emplace_back(std::move(vpath), v.target.empty() ? "vendor" : v.target);
    }

    auto unpack_vendors = [&](const std::filesystem::path& base_dir) -> bool {
        for (const auto& [vpath, target_rel] : vendor_paths) {
            const auto target_dir = base_dir / target_rel;
            std::error_code vec;
            std::filesystem::create_directories(target_dir, vec);
            sage::util::log_info("Unpacking vendor archive to {}...", target_dir.string());
            auto extracted = extract_source_archive(vpath, target_dir);
            if (!extracted) {
                sage::util::log_error("Failed to unpack vendor archive {}: {}", vpath.filename().string(), extracted.error());
                return false;
            }
        }
        return true;
    };

    // These are populated only from the v2 plan Sage actually executes. They
    // remain empty for v1, and empty flag channels are deliberately omitted.
    std::vector<std::string> managed_cc_parameters;
    std::vector<std::string> managed_cxx_parameters;
    std::vector<std::string> managed_linker_parameters;
    std::vector<std::string> managed_rustc_parameters;

    // 2. Prepare, Build & Install Phases -- exactly one toolchain, the first
    // usable candidate.
    {
        const Toolchain& cand = candidates.front();

        std::error_code ec;
        std::filesystem::remove_all(pkg_dir, ec);
        std::filesystem::create_directories(pkg_dir);
        if (resume_src) {
            sage::util::log_info("Reusing existing source tree at {} (--reuse-src)",
                                 src_dir.string());
        } else if (!archive_path.empty() || !extra_paths.empty() || !vendor_paths.empty()) {
            std::filesystem::remove_all(src_dir, ec);
            std::filesystem::create_directories(src_dir / "distfiles");
            if (!archive_path.empty()) {
                sage::util::log_info("Unpacking source to {}...", src_dir.string());
                auto extracted = extract_source_archive(archive_path, src_dir);
                if (!extracted) {
                    sage::util::log_error(
                        "Failed to unpack source archive: {}. Cleaning up...",
                        extracted.error());
                    std::filesystem::remove(archive_path, ec);
                    std::filesystem::remove_all(src_dir, ec);
                    return 1;
                }
            }
            if (!unpack_vendors(src_dir)) return 1;
            for (const auto& path : extra_paths) {
                std::error_code copy_ec;
                std::filesystem::copy_file(path, src_dir / "distfiles" / path.filename(),
                                           std::filesystem::copy_options::overwrite_existing, copy_ec);
                if (copy_ec) {
                    sage::util::log_error("Failed to stage extra source {}: {}",
                        path.filename().string(), copy_ec.message());
                    return 1;
                }
            }
        } else if (r.schema_version == 2 && r.source_url.empty()) {
            std::filesystem::remove_all(src_dir, ec);
            std::filesystem::create_directories(src_dir, ec);
            for (const auto& entry : std::filesystem::directory_iterator(recipe_dir)) {
                const auto name = entry.path().filename().string();
                if (name == "recipe.toml" || name == "distfiles"
                    || name == "pkg" || name == ".sage-source") continue;
                std::filesystem::copy(entry.path(), src_dir / entry.path().filename(),
                    std::filesystem::copy_options::recursive
                    | std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) {
                    sage::util::log_error(
                        "Failed to stage local v2 source '{}': {}",
                        entry.path().string(), ec.message());
                    return 1;
                }
            }
            if (!unpack_vendors(src_dir)) return 1;
        }
        if (r.schema_version == 2) {
            // Local patch attachments live beside recipe.toml in the source
            // tree. Make them first-class distfiles so the same `patch`
            // command works for downloaded and local-source recipes.
            std::filesystem::create_directories(src_dir / "distfiles", ec);
            for (const auto& patch : r.managed_build.patches_spec) {
                const auto beside_recipe = recipe_dir / patch.file;
                const auto attached = std::filesystem::is_regular_file(beside_recipe, ec)
                    ? beside_recipe : recipe_dir / "distfiles" / patch.file;
                if (!std::filesystem::is_regular_file(attached, ec)) continue;
                if (patch.sha256.empty()) {
                    sage::util::log_error(
                        "Local v2 patch '{}' has no SHA-256 declaration", patch.file);
                    return 1;
                }
                auto patch_hash = sage::util::compute_file_sha256(attached);
                if (!patch_hash || *patch_hash != patch.sha256) {
                    sage::util::log_error(
                        "Local v2 patch SHA256 mismatch for '{}'", patch.file);
                    return 1;
                }
                std::filesystem::copy_file(attached, src_dir / "distfiles" / patch.file,
                    std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) {
                    sage::util::log_error(
                        "Failed to stage local patch '{}': {}", patch.file, ec.message());
                    return 1;
                }
            }
        }
        // A local v2 project is staged into the private source root. Legacy
        // local recipes keep their historical recipe-dir cwd, while any
        // archive-backed recipe (v1 or v2) builds from the extracted tree.
        std::filesystem::path work_dir =
            (r.schema_version == 2 || !r.source_url.empty()) ? src_dir : recipe_dir;

        auto run_phase = [&](std::string_view phase_name, const std::vector<std::string>& cmds) -> bool {
            if (cmds.empty()) return true;
            sage::util::log_info("Executing {} phase...", phase_name);
            for (const auto& cmd_line : cmds) {
                std::string full_cmd = std::format(
                    "export CC=\"{}\" CXX=\"{}\" CPPFLAGS=\"{}\" CFLAGS=\"{}\" CXXFLAGS=\"{}\" LDFLAGS=\"{}\" "
                    "MAKEFLAGS=\"{}\" CARGO_BUILD_JOBS=\"{}\" "
                    "DESTDIR=\"{}\" PREFIX=\"/usr\" RECIPE_DIR=\"{}\" SRCDIR=\"{}\" PKGDIR=\"{}\" "
                    "PATH=\"/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\" "
                    "HOME=\"{}\" TMPDIR=\"{}\" LC_ALL=C LANG=C TZ=UTC SOURCE_DATE_EPOCH=\"{}\" "
                    "FORCE_SOURCE_DATE=1 PYTHONHASHSEED=0 ARFLAGS=crD ZERO_AR_DATE=1 "
                    "CARGO_INCREMENTAL=0 CARGO_TERM_COLOR=never DEBUGINFOD_URLS= "
                    "GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null "
                    "TERM=dumb SHELL=/bin/sh USER=builder LOGNAME=builder PAGER=cat; "
                    "cd \"{}\" && {}",
                    cand.cc, cand.cxx, bcfg.cppflags, eff_cflags, eff_cxxflags, bcfg.ldflags,
                    jobs_makeflags, compile_jobs,
                    pkg_dir.string(), recipe_dir.string(), src_dir.string(), pkg_dir.string(),
                    build_home.string(), build_temp.string(), bcfg.source_date_epoch,
                    work_dir.string(), cmd_line);
                const auto fakeroot_cmd = sage::build::fakeroot_command(
                    bcfg.fakeroot, hermetic_shell(full_cmd));
                if (opts.verbose)
                    sage::util::log_info("CMD: {}", fakeroot_cmd);
                int ret = std::system(fakeroot_cmd.c_str());
                if (ret != 0) {
                    sage::util::log_error("Command failed in {} phase: {}", phase_name, cmd_line);
                    return false;
                }
            }
            return true;
        };

        bool phases_ok = true;
        std::string ran_cflags = eff_cflags;
        std::string ran_cxxflags = eff_cxxflags;
        std::string ran_cppflags = bcfg.cppflags;
        std::string ran_ldflags = bcfg.ldflags;
        std::string ran_rustflags;
        if (r.schema_version == 1) {
            phases_ok = run_phase("prepare", r.prepare_cmds)
                && run_phase("build", r.build_cmds)
                && run_phase("install", r.install_cmds);
        } else {
            auto plan = sage::build::plan_v2(
                r, bcfg, {.source = work_dir, .package = pkg_dir,
                           .home = build_home, .temp = build_temp},
                {.cc = cand.cc, .cxx = cand.cxx, .linker = cand.linker,
                 .rustc = cand.rustc, .go = cand.go,
                 .cc_for_build = cand.cc_for_build,
                 .cxx_for_build = cand.cxx_for_build,
                 .linker_for_build = cand.linker_for_build,
                 .rustc_for_build = cand.rustc_for_build,
                 .cc_cache_for_build = cand.cc_cache_for_build,
                 .cxx_cache_for_build = cand.cxx_cache_for_build,
                 .cache_for_build = cand.cache_for_build,
                 .compiler_cache_mode = active_cache_mode,
                 .path_for_build = cand.path_for_build,
                 .compiler_version = cand.compiler_version,
                 .cxx_version = cand.cxx_version,
                 .linker_version = cand.linker_version,
                 .rustc_version = cand.rustc_version,
                 .go_version = cand.go_version,
                 .compiler_family = cand.compiler_family,
                 .cxx_family = cand.cxx_family,
                 .linker_family = cand.linker_family,
                 .rustc_family = cand.rustc_family,
                 .go_family = cand.go_family}, compile_jobs);
            if (!plan) {
                sage::util::log_error("Cannot plan recipe v2 build: {}", plan.error());
                return 1;
            }
            ran_cflags = plan->environment.contains("CFLAGS")
                ? plan->environment.at("CFLAGS") : std::string{};
            ran_cxxflags = plan->environment.contains("CXXFLAGS")
                ? plan->environment.at("CXXFLAGS") : std::string{};
            ran_cppflags = plan->environment.contains("CPPFLAGS")
                ? plan->environment.at("CPPFLAGS") : std::string{};
            ran_ldflags = plan->environment.contains("LDFLAGS")
                ? plan->environment.at("LDFLAGS") : std::string{};
            ran_rustflags = plan->environment.contains("RUSTFLAGS")
                ? plan->environment.at("RUSTFLAGS") : std::string{};
            auto capture_parameter = [&](std::vector<std::string>& parameters,
                                         std::string_view name) {
                auto it = plan->environment.find(std::string(name));
                if (it == plan->environment.end() || it->second.empty()) return;
                auto observed = std::format("{}={}", name, it->second);
                if (std::ranges::find(parameters, observed) == parameters.end())
                    parameters.push_back(std::move(observed));
            };
            const bool cargo = r.managed_build.system
                == sage::package::BuildSystem::Cargo;
            if (cargo) {
                // Cargo does not imply that this package compiled C or C++.
                // Its linker selection is carried by RUSTFLAGS, so do not
                // manufacture cc/cxx observations from generic environment
                // slots which a pure-Rust build never consumed.
                capture_parameter(managed_linker_parameters, "RUSTFLAGS");
                capture_parameter(managed_rustc_parameters, "RUSTFLAGS");
                for (const auto& name : r.managed_build.rustflags_env) {
                    capture_parameter(managed_linker_parameters, name);
                    capture_parameter(managed_rustc_parameters, name);
                }
            } else {
                capture_parameter(managed_cc_parameters, "CPPFLAGS");
                capture_parameter(managed_cc_parameters, "CFLAGS");
                capture_parameter(managed_cxx_parameters, "CPPFLAGS");
                capture_parameter(managed_cxx_parameters, "CXXFLAGS");
                capture_parameter(managed_linker_parameters, "LDFLAGS");
                for (const auto& name : r.managed_build.cppflags_env) {
                    capture_parameter(managed_cc_parameters, name);
                    capture_parameter(managed_cxx_parameters, name);
                }
                for (const auto& name : r.managed_build.cflags_env)
                    capture_parameter(managed_cc_parameters, name);
                for (const auto& name : r.managed_build.cxxflags_env)
                    capture_parameter(managed_cxx_parameters, name);
                for (const auto& name : r.managed_build.ldflags_env)
                    capture_parameter(managed_linker_parameters, name);
                if (r.managed_build.kernel) {
                    // Kbuild receives the same Sage policy through its native
                    // channels. Preserve those channels in the observed
                    // record so a kernel build cannot be mistaken for one
                    // that only exported generic CFLAGS/LDFLAGS.
                    capture_parameter(managed_cc_parameters, "KCFLAGS");
                    capture_parameter(managed_cc_parameters, "KCPPFLAGS");
                    capture_parameter(managed_linker_parameters, "KBUILD_LDFLAGS");
                }
            }
            plan->environment["RECIPE_DIR"] = recipe_dir.string();
            plan->environment["SRCDIR"] = work_dir.string();
            plan->environment["PKGDIR"] = pkg_dir.string();
            std::string exports;
            for (const auto& [name, value] : plan->environment) {
                exports += std::format("export {}={}; ", name,
                    sage::build::shell_quote(value));
            }
            for (const auto& step : plan->steps) {
                sage::util::log_info("Executing managed {} step...", step.name);
                const auto full_cmd = exports + "cd "
                    + sage::build::shell_quote(step.work_dir.string())
                    + " && " + step.command;
                const auto configured_fakeroot = ToolAudit::resolve(bcfg.fakeroot)
                    .value_or(std::filesystem::path(bcfg.fakeroot));
                std::error_code sysroot_ec;
                const auto sysroot_root = std::filesystem::weakly_canonical(
                    bcfg.sysroot, sysroot_ec);
                const auto relative_root = sysroot_ec ? bcfg.sysroot : sysroot_root;
                const auto sandbox_fakeroot = (std::filesystem::path("/")
                    / configured_fakeroot.lexically_relative(relative_root)).lexically_normal();
                const auto fakeroot_cmd = tool_audit && !tool_audit->sandbox.empty()
                    ? sandboxed_fakeroot_shell(
                        sandbox_fakeroot.string(),
                        full_cmd,
                                               tool_audit->sandbox)
                    : sage::build::fakeroot_command(bcfg.fakeroot,
                        hermetic_shell(full_cmd));
                if (opts.verbose) sage::util::log_info("CMD: {}", fakeroot_cmd);
                const auto command_result = ProcessExecAudit::run(
                    fakeroot_cmd, tool_audit->process_exec_log,
                    bcfg.memory_limit, bcfg.pids_limit);
                if (!command_result || *command_result != 0) {
                    sage::util::log_error("Managed {} step failed: {}",
                        step.name, step.command);
                    if (!command_result)
                        sage::util::log_error("Process exec audit failed: {}",
                                              command_result.error());
                    phases_ok = false;
                    break;
                }
            }
        }

        if (!phases_ok) {
            // Never retry a failed recipe under a different environment. V2
            // can truthfully report the managed toolchain it executed; opaque
            // v1 shell phases cannot establish compiler provenance.
            if (r.schema_version == 1) {
                sage::util::log_error(
                    "Legacy recipe phase failed; no compiler provenance is inferred from its environment");
                sage::util::log_error(
                    "Stopping without retrying under a different build environment");
            } else {
                sage::util::log_error(
                    "Managed build failed under CC='{}' {} CXX='{}' {} LD='{}' {} with CFLAGS=\"{}\" CXXFLAGS=\"{}\" LDFLAGS=\"{}\" CPPFLAGS=\"{}\" RUSTFLAGS=\"{}\"",
                    cand.cc, cand.compiler_version, cand.cxx, cand.cxx_version, cand.linker,
                    cand.linker_version, ran_cflags, ran_cxxflags, ran_ldflags,
                    ran_cppflags, ran_rustflags);
                sage::util::log_error(
                    "The v2 toolchain is controlled by Sage; adjust build.toml or "
                    "the recipe's allowed tool families, then rebuild.");
            }
            return 1;
        }
    }

    // A backend's DESTDIR is an intermediate install tree, not the package
    // boundary.  Enforce the recipe's explicit v2 payload policy before ELF
    // scanning and archive creation so omitted files cannot reappear in the
    // manifest through a later packaging pass.
    std::vector<std::string> go_command_lines;
    std::uint64_t go_executions = 0;
    if (r.schema_version == 2) {
        const auto transform_source = src_dir / r.managed_build.source_subdir;
        auto transforms = apply_install_transforms(
            transform_source, pkg_dir, r.managed_build.install_copies,
            r.managed_build.install_symlinks,
            r.managed_build.install_moves,
            r.managed_build.install_removes,
            r.managed_build.install_generates);
        if (!transforms) {
            sage::util::log_error("Invalid install transform for '{}': {}",
                                  r.name, transforms.error());
            return 1;
        }
        auto file_perms_res = apply_file_permissions(pkg_dir, r.managed_build.file_permissions);
        if (!file_perms_res) {
            sage::util::log_error("Invalid file permissions for '{}': {}",
                                  r.name, file_perms_res.error());
            return 1;
        }
        // [build.content] post-processing runs on the common staging tree, so
        // every output view inherits the same stripped, compressed payload.
        auto content_res = apply_content_policy(pkg_dir, r.managed_build.content);
        if (!content_res) {
            sage::util::log_error("Cannot apply content policy for '{}': {}",
                                  r.name, content_res.error());
            return 1;
        }
        if (r.managed_build.outputs.empty()) {
            auto payload = apply_payload_policy(
                pkg_dir, r.managed_build.install_files,
                r.managed_build.install_excludes,
                r.managed_build.optional_excludes);
            if (!payload) {
                sage::util::log_error("Invalid staged payload for '{}': {}",
                                      r.name, payload.error());
                return 1;
            }
            auto sysusers_res = apply_sysusers_fragment(pkg_dir, r.name, r.sysusers);
            if (!sysusers_res) {
                sage::util::log_error("Cannot stage sysusers for '{}': {}",
                                      r.name, sysusers_res.error());
                return 1;
            }
        }
        // An alternatives link is created by sage at install time; a payload
        // file occupying the same path would defeat the arbitration.
        for (const auto& alt : r.alternatives) {
            std::error_code alt_ec;
            const auto occupied = pkg_dir / alt.link;
            if (std::filesystem::exists(occupied, alt_ec)
                || std::filesystem::is_symlink(occupied, alt_ec)) {
                sage::util::log_error(
                    "Package '{}' declares alternative link '{}' but its payload "
                    "already occupies that path", r.name, alt.link);
                return 1;
            }
        }
        if (!tool_audit) {
            sage::util::log_error("Managed build has no execution audit");
            return 1;
        }
        const auto process_execs = tool_audit->process_execs();
        if (process_execs.empty()) {
            sage::util::log_error(
                "Managed build produced no process exec audit records");
            return 1;
        }
        const auto process_exec_text = [&] {
            std::string text;
            for (const auto& event : process_execs) text += event + "\n";
            return text;
        }();
        const auto audit_path_equal = [&](std::string_view actual,
                                          std::string_view expected) {
            if (actual == expected) return true;
            // When Sage itself is entered through a host-side chroot or container,
            // the ptrace observer can report the host mount prefix while the
            // child sees the sysroot as `/`. Accept configured prefix rewrites and
            // outer mount prefixes.
            if (bcfg.sysroot != "/") {
                const auto host_path = (bcfg.sysroot
                    / std::filesystem::path(expected).relative_path()).lexically_normal();
                if (actual == host_path.string()) return true;
            }
            if (expected.starts_with('/') && actual.ends_with(expected)) {
                return true;
            }
            const auto hr_pos = expected.find("/host-root/");
            if (hr_pos != std::string_view::npos) {
                const auto sys_subpath = expected.substr(hr_pos + 10);
                if (actual.ends_with(sys_subpath)) return true;
            }
            return false;
        };
        const auto require_real_exec = [&](std::string_view role,
                                           std::uint64_t executions) {
            if (executions == 0) return true;
            const auto expected_it = tool_audit->expected_real_execs.find(std::string(role));
            if (expected_it == tool_audit->expected_real_execs.end()) return false;
            const auto& expected = expected_it->second;
            const auto observed = [&](const std::string& path) {
                for (size_t begin = 0;;) {
                    const auto found = process_exec_text.find("path=", begin);
                    if (found == std::string::npos) return false;
                    const auto path_begin = found + 5;
                    const auto path_end = process_exec_text.find_first_of(" \n", path_begin);
                    if (audit_path_equal(process_exec_text.substr(path_begin,
                            path_end == std::string::npos
                                ? std::string::npos : path_end - path_begin), path))
                        return true;
                    begin = path_begin;
                }
            };
            if (!std::ranges::any_of(expected, observed)) {
                std::string expected_paths;
                for (const auto& value : expected)
                    expected_paths += (expected_paths.empty() ? "" : ",") + value;
                sage::util::log_error(
                    "Selected Sage {} did not reach a real execve transition: {}\nExpected paths: {}\nAudit:\n{}",
                    role, expected.front(), expected_paths, process_exec_text);
                return false;
            }
            return true;
        };
        if (!require_real_exec("cc", tool_audit->executions("cc"))
            || !require_real_exec("cxx", tool_audit->executions("cxx"))
            || !require_real_exec("linker", tool_audit->executions("linker"))
            || !require_real_exec("rustc", tool_audit->executions("rustc")))
            return 1;
        for (const auto& event : process_execs) {
            const auto path_marker = event.find(" path=");
            if (path_marker == std::string::npos) continue;
            const auto begin = path_marker + 6;
            const auto end = event.find(' ', begin);
            const auto path = event.substr(begin,
                end == std::string::npos ? std::string::npos : end - begin);
            if (!compiler_like_executable(path)) continue;
            const auto norm_path = std::filesystem::path(path).lexically_normal().string();
            if (norm_path.starts_with(src_dir.string())
                || norm_path.starts_with(pkg_dir.string())
                || norm_path.starts_with(src_dir.parent_path().string())
                || norm_path.find(src_dir.parent_path().filename().string()) != std::string::npos)
                continue;
            bool selected = false;
            for (const auto& [_, expected] : tool_audit->expected_real_execs)
                if (std::ranges::any_of(expected, [&](const auto& value) {
                        return audit_path_equal(path, value);
                    })) selected = true;
            if (!selected) {
                sage::util::log_error(
                    "Process exec audit observed an unmanaged compiler/linker: {}",
                    path);
                return 1;
            }
        }
        if (tool_audit->executions("unexpected") != 0) {
            sage::util::log_error(
                "The build attempted to execute a compiler/linker outside Sage's selected toolchain");
            return 1;
        }
        const bool cargo = r.managed_build.system == sage::package::BuildSystem::Cargo;
        const bool go_backend = r.managed_build.system == sage::package::BuildSystem::Go;
        const bool script = r.managed_build.system == sage::package::BuildSystem::Script
            && !r.managed_build.script_managed_tools;
        // The go toolchain is not PATH-fenced: prove it executed through the
        // ptrace process log instead, and keep its observed command lines as
        // the manifest's execution evidence.
        if (go_backend) {
            auto resolved_go = ToolAudit::resolve("go");
            if (!resolved_go) {
                sage::util::log_error("Go recipe requires a 'go' executable");
                return 1;
            }
            for (const auto& event : process_execs) {
                const auto path_marker = event.find(" path=");
                if (path_marker == std::string::npos) continue;
                const auto begin = path_marker + 6;
                const auto end = event.find(' ', begin);
                const auto path = event.substr(begin,
                    end == std::string::npos ? std::string::npos : end - begin);
                if (!audit_path_equal(path, resolved_go->string())) continue;
                ++go_executions;
                const auto argv_marker = event.find("argv=");
                if (argv_marker != std::string::npos)
                    go_command_lines.push_back(event.substr(argv_marker + 5));
            }
        }
        const auto compiler_execs = tool_audit->executions("cc")
            + tool_audit->executions("cxx");
        const auto rustc_execs = tool_audit->executions("rustc");
        if (!r.managed_build.header_only) {
            if ((!script && cargo && rustc_execs == 0)
                || (!script && !cargo && !go_backend && compiler_execs == 0)
                || (!script && go_backend && go_executions == 0)) {
                sage::util::log_error(
                    "No Sage audit marker proves that the configured compiler executed");
                return 1;
            }
        }
        const auto link_driver_execs = tool_audit->executions("linker-driver");
        const auto linker_execs = tool_audit->executions("linker");
        if (script && (compiler_execs != 0 || rustc_execs != 0
                       || link_driver_execs != 0 || linker_execs != 0)) {
            sage::util::log_error(
                "Script recipe executed a compiler or linker; use a managed build backend for compiled sources");
            return 1;
        }
        if (!script && link_driver_execs != 0 && linker_execs == 0) {
            sage::util::log_error(
                "A compiler linker-driver executed without the selected linker backend");
            return 1;
        }
        bool has_elf = false;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(pkg_dir)) {
            if (!entry.is_regular_file()) continue;
            if (sage::util::scan_elf(entry.path())) { has_elf = true; break; }
        }
        if (r.managed_build.header_only && has_elf) {
            sage::util::log_error(
                "build.header_only=true is declared, but ELF binary payload was found");
            return 1;
        }
        if (!script && !go_backend && !r.managed_build.header_only && has_elf && linker_execs == 0) {
            sage::util::log_error(
                "ELF payload exists but no selected linker execution was observed");
            return 1;
        }
    }
    auto elf_res = scan_and_validate_elf(
        r, candidates, tool_audit, go_command_lines, go_executions,
        managed_cc_parameters, managed_cxx_parameters,
        managed_linker_parameters, managed_rustc_parameters,
        hermetic_root, pkg_dir, src_dir, recipe_dir, opts.no_elf_check,
        bcfg, cfg, host_arch, target_arch, host_triplet, target_triplet);
    if (!elf_res) return elf_res.error();
    const auto& elf_result = *elf_res;

    auto pack_res = pack_build_outputs(
        r, elf_result, hermetic_root, pkg_dir, src_dir, recipe_dir);
    if (!pack_res) return pack_res.error();
    const auto created_packages = std::move(*pack_res);

    if (opts.check_reproducible) {
        auto repro_res = run_reproducibility_check(
            r, elf_result.manifest, candidates, recipe_dir, archive_path, extra_paths,
            created_packages, opts.target_root,
            bcfg, target_triplet, active_cache_mode, cache_dir, vendor_paths);
        if (!repro_res) return repro_res.error();
    }
    return 0;
}

export inline int cmd_repo(const CliOptions& opts) {
    if (opts.args.empty() || opts.args[0] != "index" || opts.args.size() < 2) {
        std::println("Usage: sage repo index <REPO_DIR> [CHANNEL_NAME]");
        return 1;
    }
    std::filesystem::path repo_dir = opts.args[1];
    std::string ch_name = (opts.args.size() >= 3) ? opts.args[2] : "core";
    sage::util::log_info("Generating index.toml for local repository at {}...", repo_dir.string());
    auto res = sage::archive::generate_repo_index(repo_dir, ch_name);
    if (!res) {
        sage::util::log_error("Failed to generate repository index: {}", res.error());
        return 1;
    }
    sage::util::log_success("Repository index successfully created: {}", (repo_dir / "index.toml").string());
    return 0;
}

} // namespace sage::cli
