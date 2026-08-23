export module sage.cli.build;

// Package authoring: recipe builds and local repository indexing.
import std;
import sage;

import sage.cli;
import sage.vendor.zstd;

namespace sage::cli {

// -- Build toolchain probing -------------------------------------------------
//
// Existence check and version capture in one shot: run `<cc> --version` with
// output redirected into a scratch file (this codebase speaks plain
// std::system; popen would be its own RAII project), then take the first
// whitespace token starting with a digit -- "clang version 19.1.7" and
// "gcc (GCC) 15.1.1" both yield their version that way. nullopt means the
// compiler is not usable at all.
std::optional<std::string> probe_compiler(std::string_view cc) {
    if (cc.empty()) return std::nullopt;
    std::filesystem::path out = std::filesystem::temp_directory_path()
        / std::format("sage-cc-probe-{}.txt", sage::util::current_pid());
    struct Remover {  // RAII: the scratch file cleans itself up
        const std::filesystem::path& p;
        ~Remover() { std::error_code ec; std::filesystem::remove(p, ec); }
    } remover{out};

    int rc = std::system(std::format("\"{}\" --version > \"{}\" 2>&1", cc, out.string()).c_str());
    if (rc != 0) return std::nullopt;
    std::ifstream f(out);
    std::string line;
    if (!std::getline(f, line)) return std::nullopt;
    size_t i = 0;
    while (i < line.size()) {
        size_t j = line.find(' ', i);
        std::string_view tok(line.data() + i, (j == std::string::npos ? line.size() : j) - i);
        if (!tok.empty() && std::isdigit(static_cast<unsigned char>(tok.front()))) {
            return std::string(tok);
        }
        i = (j == std::string::npos) ? line.size() : j + 1;
    }
    return std::string("unknown");
}

// -- Build provenance by artifact evidence -----------------------------------
//
// Provenance is stamped only for packages that actually compiled something,
// and the compiler named is the one the artifacts themselves identify --
// never merely what CC was injected. A pure-data or script-only package
// (os-release) stays silent, and a rust build is labeled rustc, not clang.
struct Provenance {
    bool compiled = false;          // any ELF / object-like file in the payload
    std::set<std::string> producers;
    // Versions per producer: linked executables embed the crt startup files'
    // .comment too, so a clang-built package honestly carries a gcc trace --
    // each producer keeps its own version instead of one ambiguous value.
    std::map<std::string, std::set<std::string>> producer_versions;
};

// First dotted numeric token at/after `pos`: "clang version 22.1.8 (...)",
// "GCC: (GNU) 15.3.0" and "rustc version 1.90.0" all yield their version.
std::string version_after(std::string_view text, size_t pos) {
    for (size_t i = pos; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) continue;
        size_t j = i;
        while (j < text.size() && (std::isdigit(static_cast<unsigned char>(text[j])) || text[j] == '.')) ++j;
        if (j > i + 1) return std::string(text.substr(i, j - i));  // skip lone digits
        i = j;
    }
    return {};
}

// Producer fingerprints live in .comment-style strings, which sit in the
// leading or trailing slabs of real artifacts; capping the read keeps huge
// libraries (libLLVM) cheap to inspect without parsing section tables.
constexpr std::streamoff kSlabBytes = 1 << 20;
// Kernel modules ship zstd-compressed (.ko.zst), hiding their ELF image --
// and its producer string -- behind one frame. Frames cannot be seeked
// into, so framed members are proved from their decompressed lead alone;
// modules sit far under this cap and their whole image is seen.
constexpr size_t kFrameLead = 4u << 20;

void scan_producers(Provenance& prov, std::string_view window) {
    // The needles assemble at runtime: a spelled-out "clang version" literal
    // would live in sage's own .rodata, and scanning a sage binary would
    // then prove sage was built by clang, gcc AND rustc at once.
    static const std::vector<std::pair<std::string, std::string_view>> SIGS = {
        {std::string("clang vers") + "ion", "clang"},
        {std::string("GC") + "C: (", "gcc"},
        {std::string("rustc vers") + "ion", "rustc"},
    };
    for (const auto& [sig, name] : SIGS) {
        const size_t at = window.find(sig);
        if (at == std::string_view::npos) continue;
        std::string producer{name};
        prov.producers.insert(producer);
        // "GCC: (GNU) 15.3.0" and distro variants alike carry the first
        // dotted token after the signature; a failed parse just means the
        // producer is listed without a version.
        if (auto ver = version_after(window, at + sig.size()); !ver.empty())
            prov.producer_versions[std::move(producer)].insert(std::move(ver));
    }
}

std::string read_slabs(std::ifstream& in) {
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::streamoff>(in.tellg());
    const auto head_len = std::min(size, kSlabBytes);
    std::string window(static_cast<size_t>(head_len), '\0');
    in.seekg(0);
    in.read(window.data(), head_len);
    window.resize(static_cast<size_t>(in.gcount()));
    if (size > kSlabBytes) {  // tail slab too: markers often live near the end
        const auto tail_len = std::min(size - kSlabBytes, kSlabBytes);
        const auto before = window.size();
        window.resize(before + static_cast<size_t>(tail_len));
        in.seekg(-tail_len, std::ios::end);
        in.read(window.data() + before, tail_len);
        window.resize(before + static_cast<size_t>(in.gcount()));
    }
    return window;
}

// True when a payload file proves compilation, fingerprinting whatever
// proves it. Plain files qualify by ELF magic or an object-archive suffix
// and are scanned head+tail; zstd-framed ones (.ko.zst) only when the ELF
// image inside their decompressed lead says so -- compressed data alone is
// no evidence of anything.
bool compiled_artifact(Provenance& prov, const std::filesystem::path& path,
                       std::string_view base) {
    static constexpr std::string_view ELF_MAGIC = "\x7f" "ELF";
    static constexpr std::string_view ZSTD_FRAME = "\x28\xB5\x2F\xFD";
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    char magic[4] = {};
    if (!in.read(magic, 4)) return false;
    const std::string_view head(magic, 4);

    if (head == ZSTD_FRAME) {
        in.clear();
        in.seekg(0);
        auto lead = sage::vendor::zstd::ZstdDecompressStream{}.decompress_lead(in, kFrameLead);
        if (!lead || lead->empty()
            || !std::string_view(*lead).starts_with(ELF_MAGIC)) return false;
        scan_producers(prov, *lead);
        return true;
    }

    const bool object = base.ends_with(".o") || base.ends_with(".a")
                     || base.find(".so") != std::string_view::npos;
    if (head != ELF_MAGIC && !object) return false;
    scan_producers(prov, read_slabs(in));
    return true;
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
    for (const auto& configured : cfg.channels) {
        if (!configured.enabled || configured.url.empty()) continue;
        sage::channel::Channel channel;
        channel.name = configured.name;
        channel.url = configured.url;
        channel.scope = sage::channel::parse_scope(configured.scope);
        channel.priority = configured.priority;
        channel.enabled = true;
        auto published = sage::channel::ProfileManager::sync_channel(channel, cfg.cache_dir);
        if (!published) {
            sage::util::log_error("Cannot determine published releases from Recipedia channel '{}': {}",
                                  configured.name, published.error());
            return 1;
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

    // Per-recipe [build] overrides replace the global baseline; cxxflags
    // mirror cflags at whichever level does not spell them out.
    const std::string eff_cflags = !r.cflags.empty() ? r.cflags : bcfg.cflags;
    const std::string eff_cxxflags = !r.cxxflags.empty() ? r.cxxflags
                                   : !r.cflags.empty() ? r.cflags
                                   : !bcfg.cxxflags.empty() ? bcfg.cxxflags
                                   : bcfg.cflags;

    // Parallelism for the phase shells: an explicit build.toml `jobs`, else
    // one per hardware thread. MAKEFLAGS reaches make-based recipes,
    // CARGO_BUILD_JOBS the cargo ones.
    const unsigned jobs = bcfg.jobs > 0 ? static_cast<unsigned>(bcfg.jobs)
                                        : std::max(1u, std::thread::hardware_concurrency());
    const std::string jobs_makeflags = std::format("-j{}", jobs);

    // Candidate toolchains in priority order: the configured pair first, the
    // fallback pair second. Each is probed once up front -- `<cc> --version`
    // doubles as the existence check; what ends up stamped is derived from
    // the built artifacts themselves, not from this probe.
    //
    // A recipe-level [build] cc pins the toolchain outright: exactly that
    // pair runs and the global fallback never does -- a pinned build that
    // fails must fail the recipe rather than silently produce core system
    // packages from the wrong compiler.
    struct Toolchain { std::string cc, cxx, version; };
    std::vector<Toolchain> candidates;
    auto try_candidate = [&](const std::string& cc_name, const std::string& cxx_name) {
        if (cc_name.empty() || std::ranges::any_of(candidates, [&](const Toolchain& t) { return t.cc == cc_name; })) {
            return;
        }
        auto ver = probe_compiler(cc_name);
        if (!ver) {
            sage::util::log_warn("Compiler '{}' not usable, skipping", cc_name);
            return;
        }
        candidates.push_back({cc_name, cxx_name, std::move(*ver)});
    };
    if (!r.cc.empty()) {
        // Pinned: exactly this pair, no fallback. A pinned build that cannot
        // even probe its compiler fails the recipe outright.
        const std::string pin_cxx = r.cxx.empty() ? bcfg.cxx : r.cxx;
        auto ver = probe_compiler(r.cc);
        if (!ver) {
            sage::util::log_error("Recipe pins compiler '{}' but it is not usable", r.cc);
            return 1;
        }
        candidates.push_back({r.cc, pin_cxx, std::move(*ver)});
        sage::util::log_info("Using pinned compiler: {} ({})", r.cc, candidates.front().version);
    } else {
        try_candidate(bcfg.cc, bcfg.cxx);
        try_candidate(bcfg.fallback_cc, bcfg.fallback_cxx);
    }
    if (candidates.empty()) {
        // Script-only recipes must keep building on hosts without any
        // probeable compiler: run unlabeled, stamping nothing.
        sage::util::log_warn("No usable C compiler found; building without CC/CXX injection");
        candidates.push_back({bcfg.cc, bcfg.cxx, ""});
    } else {
        sage::util::log_info("Using compiler: {} ({})", candidates.front().cc, candidates.front().version);
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

    // 2. Prepare, Build & Install Phases -- retried whole-hog per candidate
    // toolchain. Every attempt starts from a pristine tree: a half-built src/
    // left behind by a failed pass would poison the next candidate.
    Toolchain used;
    bool built = false;
    for (size_t attempt = 0; attempt < candidates.size(); ++attempt) {
        const Toolchain& cand = candidates[attempt];
        if (candidates.size() > 1) {
            sage::util::log_info("Build attempt {} of {} with {} {}...",
                attempt + 1, candidates.size(), cand.cc, cand.version);
        }

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
        std::filesystem::path work_dir = std::filesystem::exists(src_dir) ? src_dir : recipe_dir;

        auto run_phase = [&](std::string_view phase_name, const std::vector<std::string>& cmds) -> bool {
            if (cmds.empty()) return true;
            sage::util::log_info("Executing {} phase...", phase_name);
            for (const auto& cmd_line : cmds) {
                std::string full_cmd = std::format(
                    "export CC=\"{}\" CXX=\"{}\" CPPFLAGS=\"{}\" CFLAGS=\"{}\" CXXFLAGS=\"{}\" LDFLAGS=\"{}\" "
                    "MAKEFLAGS=\"{}\" CARGO_BUILD_JOBS=\"{}\" "
                    "DESTDIR=\"{}\" PREFIX=\"/usr\" RECIPE_DIR=\"{}\" SRCDIR=\"{}\" PKGDIR=\"{}\"; cd \"{}\" && {}",
                    cand.cc, cand.cxx, bcfg.cppflags, eff_cflags, eff_cxxflags, bcfg.ldflags,
                    jobs_makeflags, jobs,
                    pkg_dir.string(), recipe_dir.string(), src_dir.string(), pkg_dir.string(),
                    work_dir.string(), cmd_line);
                if (opts.verbose)
                    sage::util::log_info("CMD: {}", full_cmd);
                int ret = std::system(full_cmd.c_str());
                if (ret != 0) {
                    sage::util::log_error("Command failed in {} phase: {}", phase_name, cmd_line);
                    return false;
                }
            }
            return true;
        };

        built = run_phase("prepare", r.prepare_cmds)
             && run_phase("build", r.build_cmds)
             && run_phase("install", r.install_cmds);
        if (built) {
            used = cand;
            break;
        }
        if (attempt + 1 < candidates.size()) {
            sage::util::log_warn("{} build failed; falling back to {}", cand.cc, candidates[attempt + 1].cc);
        }
    }
    if (!built) {
        sage::util::log_error("Build failed under every configured compiler ({} tried)", candidates.size());
        return 1;
    }

    // 3. Automated ELF Scanner for DT_SONAME & DT_NEEDED
    sage::package::PackageManifest manifest;
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

    // Stamp build provenance from what the payload proves (filled below):
    // nothing compiled -> nothing claimed; compiled -> the producers the
    // artifacts identify, plus the flags the recipe shell really saw.
    Provenance provenance;

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

            // Compiled-artifact evidence for provenance, with the proving
            // bytes fingerprinted for their producer string while we are
            // already walking every file.
            if (compiled_artifact(provenance, entry.path(), base))
                provenance.compiled = true;

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

    // The provenance verdict, now that every payload file has been seen.
    if (provenance.compiled) {
        // One compiler, the one that actually produced the objects. Every
        // clang- or rustc-linked binary also carries a gcc trace in its crt
        // startup files' .comment, so a producer *set* is disambiguated by
        // toolchain precedence rather than joined: gcc only wins when it is
        // all there is (i.e. gcc really did build it).
        static constexpr std::array<std::string_view, 3> PRECEDENCE{"rustc", "clang", "gcc"};
        for (const auto& want : PRECEDENCE) {
            if (!provenance.producers.contains(std::string{want})) continue;
            manifest.build_compiler = std::string{want};
            if (auto it = provenance.producer_versions.find(std::string{want});
                it != provenance.producer_versions.end() && !it->second.empty()) {
                std::string versions;
                for (const auto& v : it->second)
                    versions += (versions.empty() ? "" : "+") + v;
                manifest.build_compiler_version = std::move(versions);
            }
            break;
        }
        if (!eff_cflags.empty()) manifest.build_cflags = eff_cflags;
        if (!eff_cxxflags.empty()) manifest.build_cxxflags = eff_cxxflags;
        if (!bcfg.ldflags.empty()) manifest.build_ldflags = bcfg.ldflags;
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
