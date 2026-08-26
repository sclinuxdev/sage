export module sage.cli.build;

// Package authoring: recipe builds and local repository indexing.
import std;
import sage;

import sage.cli;

namespace sage::cli {

// -- Sage-managed build toolchain probing -----------------------------------
//
// Probe the executable itself instead of treating its configured filename as
// provenance: `cc` may be GCC or Clang, and a cross linker is commonly named
// `<triple>-ld`. The name is only a conservative fallback when --version does
// not identify a known family.
struct ToolProbe {
    std::string family;
    std::string version;
};

std::optional<ToolProbe> probe_tool(std::string_view tool, bool linker = false) {
    if (tool.empty()) return std::nullopt;
    std::filesystem::path out = std::filesystem::temp_directory_path()
        / std::format("sage-cc-probe-{}.txt", sage::util::current_pid());
    struct Remover {  // RAII: the scratch file cleans itself up
        const std::filesystem::path& p;
        ~Remover() { std::error_code ec; std::filesystem::remove(p, ec); }
    } remover{out};

    int rc = std::system(std::format("\"{}\" --version > \"{}\" 2>&1", tool, out.string()).c_str());
    if (rc != 0) return std::nullopt;
    std::ifstream f(out);
    std::stringstream captured;
    captured << f.rdbuf();
    const std::string output = captured.str();
    if (output.empty()) return std::nullopt;
    const std::string line = output.substr(0, output.find('\n'));
    std::string lower = output;
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::string family;
    if (linker) {
        if (lower.contains("mold")) family = "mold";
        else if (lower.contains("lld")) family = "lld";
        else if (lower.contains("gnu ld") || lower.contains("gnu binutils"))
            family = "ld";
    } else {
        if (lower.contains("clang")) family = "clang";
        else if (lower.contains("rustc")) family = "rustc";
        else if (lower.contains("gcc")
                 || lower.contains("free software foundation")) family = "gcc";
    }
    if (family.empty()) family = sage::build::tool_family(tool, linker);

    size_t i = 0;
    while (i < line.size()) {
        size_t j = line.find(' ', i);
        std::string_view tok(line.data() + i, (j == std::string::npos ? line.size() : j) - i);
        if (!tok.empty() && std::isdigit(static_cast<unsigned char>(tok.front()))) {
            return ToolProbe{std::move(family), std::string(tok)};
        }
        i = (j == std::string::npos) ? line.size() : j + 1;
    }
    return ToolProbe{std::move(family), "unknown"};
}

bool probe_fakeroot(std::string_view executable) {
    if (executable.empty()) return false;
    const auto command = sage::build::shell_quote(executable)
        + " --version >/dev/null 2>&1";
    return std::system(command.c_str()) == 0;
}

export int cmd_build(const CliOptions& opts) {
    if (opts.args.empty()) {
        std::println("Usage: sage build <RECIPE_DIR>");
        return 1;
    }
    std::filesystem::path recipe_dir = opts.args[0];
    std::filesystem::path recipe_file = recipe_dir / "recipe.toml";
    if (!std::filesystem::exists(recipe_file)) {
        sage::util::log_error("recipe.toml not found in directory: {}", recipe_dir.string());
        return 1;
    }

    // Make the recipe directory absolute before anything derives from it.
    // The build phases run with the working directory changed to src/ (or the
    // recipe directory), so a relative DESTDIR such as "./foo/pkg" would be
    // resolved against *that* directory instead of sage's own -- the install
    // phase writes into a phantom nested path, packing then finds pkg/ empty,
    // and the package ships with no payload and no error anywhere.
    {
        std::error_code ec;
        auto abs = std::filesystem::canonical(recipe_dir, ec);
        if (ec) {
            sage::util::log_error("Cannot resolve recipe directory '{}': {}", recipe_dir.string(), ec.message());
            return 1;
        }
        recipe_dir = std::move(abs);
        recipe_file = recipe_dir / "recipe.toml";
    }

    std::ifstream rf(recipe_file);
    std::stringstream ss;
    ss << rf.rdbuf();
    auto recipe_res = sage::package::Recipe::parse_toml(ss.str());
    if (!recipe_res) {
        sage::util::log_error("Failed to parse recipe: {}", recipe_res.error());
        return 1;
    }
    auto r = std::move(*recipe_res);

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
        for (const auto& package : published->available_packages) {
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
    const std::string jobs_makeflags = std::format("-j{}", compile_jobs);

    // Candidate toolchains in priority order: the configured pair first, the
    // fallback pair second. Each is probed once up front -- `<cc> --version`
    // doubles as the existence check. Successful v2 builds preserve these
    // direct observations in their package manifest; v1 never does.
    //
    // A recipe-level [build] cc pins the toolchain outright: exactly that
    // pair runs and the global fallback never does -- a pinned build that
    // fails must fail the recipe rather than silently produce core system
    // packages from the wrong compiler.
    struct Toolchain {
        std::string cc, cxx, linker, rustc;
        std::string compiler_version, cxx_version, linker_version, rustc_version;
        std::string compiler_family, cxx_family, linker_family, rustc_family;
    };
    std::vector<Toolchain> candidates;
    auto try_candidate = [&](const std::string& cc_name, const std::string& cxx_name,
                             const std::string& linker_name) {
        if (cc_name.empty() || std::ranges::any_of(candidates, [&](const Toolchain& t) { return t.cc == cc_name; })) {
            return;
        }
        auto compiler = probe_tool(cc_name);
        if (!compiler) {
            sage::util::log_warn("Compiler '{}' not usable, skipping", cc_name);
            return;
        }
        ToolProbe cxx;
        ToolProbe linker;
        ToolProbe rustc;
        if (r.schema_version == 2) {
            const auto& spec = r.managed_build;
            const auto& cf = compiler->family;
            auto probed_linker = probe_tool(linker_name, true);
            if (!probed_linker) {
                sage::util::log_warn("Linker '{}' not usable, skipping toolchain", linker_name);
                return;
            }
            linker = std::move(*probed_linker);
            const auto& lf = linker.family;
            if ((!spec.allowed_compilers.empty()
                 && !std::ranges::contains(spec.allowed_compilers, cf))
                || (!spec.allowed_linkers.empty()
                    && !std::ranges::contains(spec.allowed_linkers, lf))) return;
            auto probed_cxx = probe_tool(cxx_name);
            if (!probed_cxx) {
                sage::util::log_warn("C++ compiler '{}' not usable, skipping toolchain", cxx_name);
                return;
            }
            cxx = std::move(*probed_cxx);
            if (cxx.family != compiler->family) {
                if (opts.verbose) sage::util::log_info(
                    "Skipping mixed compiler suite: CC '{}' is {}, CXX '{}' is {}",
                    cc_name, compiler->family, cxx_name, cxx.family);
                return;
            }
            if (spec.system == sage::package::BuildSystem::Cargo) {
                auto probed_rustc = probe_tool(bcfg.rustc);
                if (!probed_rustc || probed_rustc->family != "rustc"
                    || probed_rustc->version == "unknown") {
                    sage::util::log_warn(
                        "Rust compiler '{}' is not usable or has no parseable --version output",
                        bcfg.rustc);
                    return;
                }
                rustc = std::move(*probed_rustc);
            }
            if (compiler->version == "unknown" || cxx.version == "unknown"
                || linker.version == "unknown") {
                if (opts.verbose) sage::util::log_info(
                    "Skipping toolchain whose --version output cannot be parsed: CC='{}' CXX='{}' LD='{}'",
                    cc_name, cxx_name, linker_name);
                return;
            }
            auto requirement = sage::build::validate_toolchain(r,
                {.cc = cc_name, .cxx = cxx_name, .linker = linker_name,
                 .rustc = spec.system == sage::package::BuildSystem::Cargo
                    ? bcfg.rustc : "",
                 .compiler_version = compiler->version,
                 .cxx_version = cxx.version,
                 .linker_version = linker.version,
                 .rustc_version = rustc.version,
                 .compiler_family = compiler->family,
                 .cxx_family = cxx.family,
                 .linker_family = linker.family,
                 .rustc_family = rustc.family});
            if (!requirement) {
                if (opts.verbose) sage::util::log_info(
                    "Skipping configured toolchain: {}", requirement.error());
                return;
            }
        }
        candidates.push_back(Toolchain{
            .cc = cc_name, .cxx = cxx_name,
            .linker = r.schema_version == 2 ? linker_name : "",
            .rustc = r.managed_build.system == sage::package::BuildSystem::Cargo
                ? bcfg.rustc : "",
            .compiler_version = std::move(compiler->version),
            .cxx_version = std::move(cxx.version),
            .linker_version = std::move(linker.version),
            .rustc_version = std::move(rustc.version),
            .compiler_family = std::move(compiler->family),
            .cxx_family = std::move(cxx.family),
            .linker_family = std::move(linker.family),
            .rustc_family = std::move(rustc.family)});
    };
    if (!r.cc.empty()) {
        // Pinned: exactly this pair, no fallback. A pinned build that cannot
        // even probe its compiler fails the recipe outright.
        const std::string pin_cxx = r.cxx.empty() ? bcfg.cxx : r.cxx;
        auto compiler = probe_tool(r.cc);
        if (!compiler) {
            sage::util::log_error("Recipe pins compiler '{}' but it is not usable", r.cc);
            return 1;
        }
        candidates.push_back(Toolchain{
            .cc = r.cc, .cxx = pin_cxx, .linker = "", .rustc = "",
            .compiler_version = std::move(compiler->version),
            .cxx_version = "", .linker_version = "", .rustc_version = "",
            .compiler_family = std::move(compiler->family),
            .cxx_family = "", .linker_family = "", .rustc_family = ""});
    } else {
        try_candidate(bcfg.cc, bcfg.cxx, bcfg.linker);
        try_candidate(bcfg.fallback_cc, bcfg.fallback_cxx, bcfg.fallback_linker);
    }
    if (candidates.empty()) {
        // Script-only v1 recipes must keep building on hosts without any
        // probeable compiler: preserve their historical environment.
        if (r.schema_version == 2) {
            sage::util::log_warn(
                "No usable C compiler found; managed build cannot continue");
            const auto& compiler = r.managed_build.compiler;
            const auto& linker = r.managed_build.linker;
            if (!compiler.family.empty()) sage::util::log_error(
                "Recipe defaults to compiler package '{}' family '{}' >= {} and linker package '{}' family '{}' >= {}, but no configured Sage toolchain satisfies it",
                compiler.package, compiler.family, compiler.minimum_version,
                linker.package, linker.family, linker.minimum_version);
            sage::util::log_error("No Sage-managed compiler/linker pair satisfies recipe v2");
            return 1;
        }
        candidates.push_back(Toolchain{
            .cc = bcfg.cc, .cxx = bcfg.cxx, .linker = "", .rustc = "",
            .compiler_version = "", .cxx_version = "", .linker_version = "",
            .rustc_version = "", .compiler_family = "", .cxx_family = "",
            .linker_family = "", .rustc_family = ""});
    } else {
        if (r.schema_version == 2) {
            const auto& selected = candidates.front();
            if (r.managed_build.system == sage::package::BuildSystem::Cargo) {
                sage::util::log_info(
                    "Using managed Rust toolchain: RUSTC='{}' ({}) linker-driver='{}' ({}) LD='{}' ({})",
                    selected.rustc, selected.rustc_version,
                    selected.cc, selected.compiler_version,
                    selected.linker, selected.linker_version);
            } else {
                sage::util::log_info(
                    "Using managed toolchain: CC='{}' ({}) CXX='{}' ({}) LD='{}' ({})",
                    selected.cc, selected.compiler_version,
                    selected.cxx, selected.cxx_version,
                    selected.linker, selected.linker_version);
            }
        }
    }

    std::filesystem::path dist_dir = recipe_dir / "distfiles";
    std::filesystem::path src_dir = recipe_dir / "src";
    std::filesystem::path pkg_dir = recipe_dir / "pkg";

    // Resume mode: keep an already-extracted (and possibly already built)
    // tree so incremental build systems only pay for what changed. Only
    // recipes with a primary archive have a src/ to resume on.
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

    // These are populated only from the v2 plan Sage actually executes. They
    // remain empty for v1, and empty flag channels are deliberately omitted.
    std::vector<std::string> managed_cc_parameters;
    std::vector<std::string> managed_cxx_parameters;
    std::vector<std::string> managed_linker_parameters;
    std::vector<std::string> managed_rustc_parameters;

    // 2. Prepare, Build & Install Phases -- exactly one toolchain, the first
    // usable candidate. A build failure is caused by the compiler or the
    // configured flags often enough that silently retrying under another
    // compiler would manufacture a package nobody asked for: sage stops and
    // says what ran instead. The fallback pair still exists at probe time
    // (an absent primary is skipped before any compilation starts).
    {
        const Toolchain& cand = candidates.front();

        std::error_code ec;
        std::filesystem::remove_all(pkg_dir, ec);
        std::filesystem::create_directories(pkg_dir);
        // A pristine tree per attempt: the primary archive is re-extracted and
        // the extra sources re-staged, so patches consumed during prepare are
        // applied to an untouched tree on every retry. --reuse-src opts out:
        // the existing tree is kept as-is for incremental build systems.
        if (resume_src) {
            sage::util::log_info("Reusing existing source tree at {} (--reuse-src)",
                                 src_dir.string());
        } else if (!archive_path.empty() || !extra_paths.empty()) {
            std::filesystem::remove_all(src_dir, ec);
            std::filesystem::create_directories(src_dir / "distfiles");
            if (!archive_path.empty()) {
                sage::util::log_info("Unpacking source to {}...", src_dir.string());
                // Strip the top-level directory only when there is one: flat
                // archives (tzcode, tzdata) carry their files at the root,
                // and stripping a component of those silently extracts
                // nothing -- every entry loses its only path element.
                const auto probe = std::format(
                    "/tmp/sage-tarhead-{}.txt", sage::util::current_pid());
                std::system(std::format("tar -tf \"{}\" 2>/dev/null | head -n1 > \"{}\"",
                    archive_path.string(), probe).c_str());
                std::ifstream head_in(probe);
                std::string first_entry;
                std::getline(head_in, first_entry);
                std::filesystem::remove(probe, ec);
                const bool flat = !first_entry.empty()
                    && first_entry.find('/') == std::string::npos;
                std::string cmd = flat
                    ? std::format("tar -xf \"{}\" -C \"{}\"",
                        archive_path.string(), src_dir.string())
                    : std::format("tar -xf \"{}\" -C \"{}\" --strip-components=1 2>/dev/null || tar -xf \"{}\" -C \"{}\"",
                        archive_path.string(), src_dir.string(), archive_path.string(), src_dir.string());
                if (std::system(cmd.c_str()) != 0) {
                    sage::util::log_error("Failed to unpack source archive! Archive may be corrupted. Cleaning up...");
                    std::filesystem::remove(archive_path, ec);
                    std::filesystem::remove_all(src_dir, ec);
                    return 1;
                }
            }
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
        }
        // A local project (notably Cargo) legitimately has recipe_dir/src/;
        // that directory is source code, not Sage's extracted-source root.
        // Only a primary source archive makes recipe_dir/src the work root.
        std::filesystem::path work_dir = !r.source_url.empty() ? src_dir : recipe_dir;

        auto run_phase = [&](std::string_view phase_name, const std::vector<std::string>& cmds) -> bool {
            if (cmds.empty()) return true;
            sage::util::log_info("Executing {} phase...", phase_name);
            for (const auto& cmd_line : cmds) {
                std::string full_cmd = std::format(
                    "export CC=\"{}\" CXX=\"{}\" CPPFLAGS=\"{}\" CFLAGS=\"{}\" CXXFLAGS=\"{}\" LDFLAGS=\"{}\" "
                    "MAKEFLAGS=\"{}\" CARGO_BUILD_JOBS=\"{}\" "
                    "DESTDIR=\"{}\" PREFIX=\"/usr\" RECIPE_DIR=\"{}\" SRCDIR=\"{}\" PKGDIR=\"{}\"; cd \"{}\" && {}",
                    cand.cc, cand.cxx, bcfg.cppflags, eff_cflags, eff_cxxflags, bcfg.ldflags,
                    jobs_makeflags, compile_jobs,
                    pkg_dir.string(), recipe_dir.string(), src_dir.string(), pkg_dir.string(),
                    work_dir.string(), cmd_line);
                const auto fakeroot_cmd = sage::build::fakeroot_command(
                    bcfg.fakeroot, full_cmd);
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
                r, bcfg, {.source = work_dir, .package = pkg_dir},
                {.cc = cand.cc, .cxx = cand.cxx, .linker = cand.linker,
                 .rustc = cand.rustc,
                 .compiler_version = cand.compiler_version,
                 .cxx_version = cand.cxx_version,
                 .linker_version = cand.linker_version,
                 .rustc_version = cand.rustc_version,
                 .compiler_family = cand.compiler_family,
                 .cxx_family = cand.cxx_family,
                 .linker_family = cand.linker_family,
                 .rustc_family = cand.rustc_family}, compile_jobs);
            if (!plan) {
                sage::util::log_error("Cannot plan recipe v2 build: {}", plan.error());
                return 1;
            }
            ran_cflags = plan->environment.at("CFLAGS");
            ran_cxxflags = plan->environment.at("CXXFLAGS");
            ran_cppflags = plan->environment.at("CPPFLAGS");
            ran_ldflags = plan->environment.at("LDFLAGS");
            ran_rustflags = plan->environment.at("RUSTFLAGS");
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
                const auto fakeroot_cmd = sage::build::fakeroot_command(
                    bcfg.fakeroot, full_cmd);
                if (opts.verbose) sage::util::log_info("CMD: {}", fakeroot_cmd);
                if (std::system(fakeroot_cmd.c_str()) != 0) {
                    sage::util::log_error("Managed {} step failed: {}",
                        step.name, step.command);
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
    // 3. Automated ELF Scanner for DT_SONAME & DT_NEEDED
    sage::package::PackageManifest manifest;
    manifest.schema_version = r.schema_version;
    manifest.name = r.name;
    manifest.version = r.version;
    manifest.description = r.description;
    manifest.license = r.license;
    manifest.channel = r.channel;
    manifest.dependencies = r.host_deps;
    manifest.provides = r.provides;
    manifest.conffiles = r.conffiles;
    manifest.arch = r.arch;
    manifest.capability_hooks = r.capability_hooks;
    manifest.triggers = r.triggers;
    if (r.schema_version == 2) {
        const auto& tools = candidates.front();
        if (r.managed_build.system == sage::package::BuildSystem::Cargo) {
            manifest.managed_build_tools = {
                {.role = "linker", .executable = tools.linker,
                 .family = tools.linker_family, .version = tools.linker_version,
                 .parameters = managed_linker_parameters},
                {.role = "rustc", .executable = tools.rustc,
                 .family = tools.rustc_family, .version = tools.rustc_version,
                 .parameters = managed_rustc_parameters},
            };
        } else {
            manifest.managed_build_tools = {
                {.role = "cc", .executable = tools.cc,
                 .family = tools.compiler_family, .version = tools.compiler_version,
                 .parameters = managed_cc_parameters},
                {.role = "cxx", .executable = tools.cxx,
                 .family = tools.cxx_family, .version = tools.cxx_version,
                 .parameters = managed_cxx_parameters},
                {.role = "linker", .executable = tools.linker,
                 .family = tools.linker_family, .version = tools.linker_version,
                 .parameters = managed_linker_parameters},
            };
        }
    }

    // A service.toml beside the recipe declares a daemon: validated here,
    // then carried verbatim on the manifest so `sage rebuild` can regenerate
    // native init scripts for whichever init system is active.
    std::filesystem::path svc_path = recipe_dir / "service.toml";
    std::error_code svc_ec;
    if (std::filesystem::exists(svc_path, svc_ec)) {
        std::ifstream svc_in(svc_path);
        std::ostringstream svc_buf;
        svc_buf << svc_in.rdbuf();
        auto spec = sage::service::ServiceSpec::parse_toml(svc_buf.str());
        if (!spec) {
            sage::util::log_error("Invalid service.toml for '{}': {}", r.name, spec.error());
            return 1;
        }
        manifest.service_toml = svc_buf.str();
    }

    // Every soname this package satisfies by itself, and every soname it still
    // needs from elsewhere -- remembering which file asked, so a failure can
    // name the offender rather than just the missing library.
    std::set<std::string> self_sonames;
    std::set<std::string> needed_sonames;
    std::map<std::string, std::vector<std::string>> needed_by;

    if (std::filesystem::exists(pkg_dir)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(pkg_dir, std::filesystem::directory_options::skip_permission_denied)) {
            auto rel = entry.path().lexically_relative(pkg_dir).generic_string();

            // A soname is normally reached through the versioned symlink the
            // package installs next to the real file (libz.so.1 ->
            // libz.so.1.3.1). Skipping symlinks meant those names never made
            // it into `provides`, which is why repository indexes ended up
            // full of so: constraints nothing satisfied.
            if (entry.is_symlink()) {
                auto base = entry.path().filename().string();
                if (base.starts_with("lib") && base.find(".so") != std::string::npos) {
                    self_sonames.insert(base);
                }
                continue;
            }
            if (!entry.is_regular_file()) continue;

            auto base = entry.path().filename().string();
            if (base.starts_with("lib") && base.find(".so") != std::string::npos) {
                self_sonames.insert(base);
            }

            auto elf_res = sage::util::scan_elf(entry.path());
            if (!elf_res) continue;
            if (!elf_res->soname.empty()) {
                self_sonames.insert(elf_res->soname);
            }
            for (const auto& needed : elf_res->needed) {
                needed_sonames.insert(needed);
                needed_by[needed].push_back(rel);
            }
        }
    }

    for (const auto& soname : self_sonames) {
        manifest.provides.push_back("so:" + soname);
    }

    // A package does not depend on itself: a soname it installs is not an
    // external constraint, and emitting it as one makes the solver chase a
    // cycle through the package being built.
    std::vector<std::string> external_sonames;
    for (const auto& soname : needed_sonames) {
        if (!self_sonames.contains(soname)) external_sonames.push_back(soname);
    }
    for (const auto& soname : external_sonames) {
        manifest.dependencies.push_back(sage::package::Dependency::parse("so:" + soname));
    }

    // Deduplicate. The same soname is routinely reached from a dozen binaries
    // in one package, and a repeated provides entry makes the index unreadable.
    {
        std::unordered_set<std::string> seen;
        std::erase_if(manifest.provides, [&](const std::string& p) { return !seen.insert(p).second; });
    }
    {
        std::unordered_set<std::string> seen;
        std::erase_if(manifest.dependencies, [&](const sage::package::Dependency& d) {
            return !seen.insert(d.to_string()).second;
        });
    }

    // 3b. Validate every remaining DT_NEEDED against what is actually
    // installed. Without this the build happily links against a library that
    // only exists on the build host -- xfsprogs picking up the host's
    // libdevmapper -- and the failure surfaces at install time on a machine
    // that has no such file.
    if (!opts.no_elf_check && !external_sonames.empty()) {
        auto host_db = sage::db::Database::open(cfg_res->db_path, true);

        if (!host_db) {
            sage::util::log_warn("Cannot verify DT_NEEDED: no package database at '{}'. "
                "{} external soname(s) go unchecked -- expected while bootstrapping, a bug otherwise.",
                cfg_res->db_path.string(), external_sonames.size());
        } else {
            std::vector<std::string> unsatisfied;
            for (const auto& soname : external_sonames) {
                if (host_db->get_provider("so:" + soname)) continue;
                unsatisfied.push_back(soname);
            }
            if (!unsatisfied.empty()) {
                sage::util::log_error("Build linked against {} library/libraries no installed package provides:", unsatisfied.size());
                for (const auto& soname : unsatisfied) {
                    sage::util::log_error("  so:{}  needed by: {}", soname, sage::util::join(needed_by[soname], ", "));
                }
                sage::util::log_error("These came from the build host, not from the repository. Either package them, "
                    "or configure the build to not use them. Pass --no-elf-check to override.");
                return 1;
            }
        }
    }

    // 4. Archive Creation
    std::string out_name = std::format("{}-{}-{}-{}.pkg.tar.zst", r.name, r.version.ver, r.version.rel, manifest.arch);
    std::filesystem::path out_path = recipe_dir / out_name;
    auto pack_res = sage::archive::create_package(manifest, pkg_dir, out_path);
    if (!pack_res) {
        sage::util::log_error("Package packaging failed: {}", pack_res.error());
        return 1;
    }

    sage::util::log_success("Package built successfully: {}", out_path.string());
    return 0;
}
export int cmd_repo(const CliOptions& opts) {
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
