module;
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

export module sage.cli.build:audit;

import std;
import sage;
import :probe;

namespace sage::cli {

struct ToolAudit {
    std::filesystem::path root;
    std::filesystem::path log;
    std::filesystem::path events;
    std::filesystem::path process_exec_log;
    std::filesystem::path sysroot;
    std::map<std::string, std::vector<std::string>> expected_real_execs;
    std::string cc;
    std::string cxx;
    std::string linker;
    std::string rustc;
    std::string path;
    std::string sandbox;
    std::string cc_cache;
    std::string cxx_cache;
    std::string cache_for_build;
    ToolAudit() = default;
    ToolAudit(const ToolAudit&) = delete;
    ToolAudit& operator=(const ToolAudit&) = delete;
    ToolAudit(ToolAudit&& other) noexcept
        : root(std::move(other.root)), log(std::move(other.log)),
          events(std::move(other.events)),
          process_exec_log(std::move(other.process_exec_log)),
          sysroot(std::move(other.sysroot)),
          expected_real_execs(std::move(other.expected_real_execs)),
          cc(std::move(other.cc)), cxx(std::move(other.cxx)),
          linker(std::move(other.linker)), rustc(std::move(other.rustc)),
          path(std::move(other.path)), sandbox(std::move(other.sandbox)),
          cc_cache(std::move(other.cc_cache)),
          cxx_cache(std::move(other.cxx_cache)),
          cache_for_build(std::move(other.cache_for_build)) {
        other.root.clear();
    }
    ToolAudit& operator=(ToolAudit&& other) noexcept {
        if (this != &other) {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
            root = std::move(other.root); log = std::move(other.log);
            events = std::move(other.events);
            process_exec_log = std::move(other.process_exec_log);
            sysroot = std::move(other.sysroot);
            expected_real_execs = std::move(other.expected_real_execs);
            cc = std::move(other.cc); cxx = std::move(other.cxx);
            linker = std::move(other.linker); rustc = std::move(other.rustc);
            path = std::move(other.path); sandbox = std::move(other.sandbox);
            cc_cache = std::move(other.cc_cache);
            cxx_cache = std::move(other.cxx_cache);
            cache_for_build = std::move(other.cache_for_build);
            other.root.clear();
        }
        return *this;
    }

    ~ToolAudit() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    static std::optional<std::filesystem::path> resolve(
        std::string_view executable) {
        if (executable.empty()) return std::nullopt;
        const std::filesystem::path candidate(executable);
        if (candidate.has_parent_path()) {
            std::error_code ec;
            auto absolute = std::filesystem::weakly_canonical(candidate, ec);
            if (!ec && std::filesystem::is_regular_file(absolute, ec)) return absolute;
            return std::nullopt;
        }
        const std::string search_path = std::getenv("PATH")
            ? std::getenv("PATH")
            : "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
        size_t start = 0;
        while (start <= search_path.size()) {
            const auto colon = search_path.find(':', start);
            const auto dir = search_path.substr(start,
                colon == std::string::npos ? std::string::npos : colon - start);
            std::filesystem::path found = std::filesystem::path(dir.empty() ? "." : dir)
                / candidate;
            std::error_code ec;
            if (std::filesystem::is_regular_file(found, ec)) {
                auto absolute = std::filesystem::weakly_canonical(found, ec);
                if (!ec) return absolute;
            }
            if (colon == std::string::npos) break;
            start = colon + 1;
        }
        return std::nullopt;
    }

    // Resolve the administrator's spelling without collapsing a driver
    // symlink.  Clang deliberately uses argv[0] (clang vs clang++) to select
    // C versus C++ defaults, while `resolve()` above must canonicalize the
    // path for the actual-exec proof.
    static std::optional<std::filesystem::path> resolve_spelling(
        std::string_view executable) {
        if (executable.empty()) return std::nullopt;
        const std::filesystem::path candidate(executable);
        const auto absolute_file = [](const std::filesystem::path& path)
            -> std::optional<std::filesystem::path> {
            std::error_code ec;
            auto absolute = std::filesystem::absolute(path, ec);
            if (ec || !std::filesystem::is_regular_file(absolute, ec))
                return std::nullopt;
            return absolute.lexically_normal();
        };
        if (candidate.has_parent_path()) return absolute_file(candidate);
        const std::string search_path = std::getenv("PATH")
            ? std::getenv("PATH")
            : "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
        size_t start = 0;
        while (start <= search_path.size()) {
            const auto colon = search_path.find(':', start);
            const auto dir = search_path.substr(start,
                colon == std::string::npos ? std::string::npos : colon - start);
            if (auto found = absolute_file(
                    std::filesystem::path(dir.empty() ? "." : dir) / candidate))
                return found;
            if (colon == std::string::npos) break;
            start = colon + 1;
        }
        return std::nullopt;
    }

    static bool write_executable(const std::filesystem::path& file,
                                 std::string_view body) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << body;
        out.close();
        std::error_code ec;
        std::filesystem::permissions(file,
            std::filesystem::perms::owner_read
                | std::filesystem::perms::owner_write
                | std::filesystem::perms::owner_exec
                | std::filesystem::perms::group_read
                | std::filesystem::perms::group_exec
                | std::filesystem::perms::others_read
                | std::filesystem::perms::others_exec,
            std::filesystem::perm_options::replace, ec);
        return !ec;
    }

    bool marker(std::string_view name) const {
        return executions(name) != 0;
    }

    std::uint64_t executions(std::string_view name) const {
        std::uint64_t count = 0;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(events, ec)) {
            if (entry.path().filename().string().starts_with(
                    "mark-" + std::string(name) + "-")) ++count;
        }
        return count;
    }

    std::vector<std::string> commands() const {
        std::vector<std::string> result;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(events, ec)) {
            if (!entry.path().filename().string().starts_with("exec-")) continue;
            std::ifstream in(entry.path());
            std::ostringstream line;
            line << in.rdbuf();
            if (!line.str().empty()) result.push_back(line.str());
        }
        std::ranges::sort(result);
        return result;
    }

    std::vector<std::string> process_execs() const {
        std::vector<std::string> result;
        std::ifstream in(process_exec_log);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) result.push_back(std::move(line));
        }
        return result;
    }

    static std::optional<ToolAudit> create(const sage::build::Toolchain& tools,
                                           const std::filesystem::path& parent,
                                           const std::filesystem::path& build_sysroot,
                                           const std::filesystem::path& ccache_dir = {},
                                           std::string_view compiler_cache = {}) {
        auto cc = resolve(tools.cc);
        auto cxx = resolve(tools.cxx);
        auto linker = resolve(tools.linker);
        auto cc_driver = resolve_spelling(tools.cc);
        auto cxx_driver = resolve_spelling(tools.cxx);
        auto linker_driver = resolve_spelling(tools.linker);
        auto rustc = tools.rustc.empty() ? std::optional<std::filesystem::path>{}
                                         : resolve(tools.rustc);
        if (!tools.cc.empty() && !cc) {
            sage::util::log_error("ToolAudit: C compiler '{}' could not be resolved in PATH or sysroot", tools.cc);
            return std::nullopt;
        }
        if (!tools.cxx.empty() && !cxx) {
            sage::util::log_error("ToolAudit: C++ compiler '{}' could not be resolved in PATH or sysroot", tools.cxx);
            return std::nullopt;
        }
        if (!tools.linker.empty() && !linker) {
            sage::util::log_error("ToolAudit: Linker '{}' could not be resolved in PATH or sysroot", tools.linker);
            return std::nullopt;
        }
        if (!tools.rustc.empty() && !rustc) {
            sage::util::log_error("ToolAudit: Rust compiler '{}' could not be resolved in PATH or sysroot", tools.rustc);
            return std::nullopt;
        }
        std::error_code sysroot_ec;
        const auto sysroot_root = std::filesystem::weakly_canonical(
            build_sysroot, sysroot_ec);
        if (build_sysroot.empty() || !build_sysroot.is_absolute()
            || sysroot_ec || !std::filesystem::is_directory(sysroot_root, sysroot_ec)) {
            sage::util::log_error("ToolAudit: Build sysroot '{}' is invalid or does not exist", build_sysroot.string());
            return std::nullopt;
        }
        if (build_sysroot != "/") {
            const auto inside = [&](const std::optional<std::filesystem::path>& path) {
                if (!path) return true;
                std::error_code path_ec;
                const auto canonical = std::filesystem::weakly_canonical(*path, path_ec);
                if (path_ec) return false;
                return canonical == sysroot_root
                    || canonical.string().starts_with(sysroot_root.string() + "/");
            };
            if (!inside(cc) || !inside(cxx) || !inside(linker) || !inside(rustc)) {
                sage::util::log_error("ToolAudit: Configured toolchain binaries reside outside the specified build sysroot '{}'", build_sysroot.string());
                return std::nullopt;
            }
        }
        std::optional<std::filesystem::path> cache;
        if (!compiler_cache.empty() && compiler_cache != "none") {
            if (compiler_cache != "ccache" && compiler_cache != "sccache")
                return std::nullopt;
            cache = resolve(compiler_cache);
            if (!cache) return std::nullopt;
            if (build_sysroot != "/") {
                std::error_code cache_ec;
                const auto cache_root = std::filesystem::weakly_canonical(
                    *cache, cache_ec);
                if (cache_ec || !(cache_root == sysroot_root
                    || cache_root.string().starts_with(sysroot_root.string() + "/")))
                    return std::nullopt;
            }
        }
        ToolAudit audit;
        audit.root = parent / "tool-audit";
        audit.log = audit.root / "executions.log";
        audit.events = audit.root / "events";
        audit.process_exec_log = audit.root / "process-exec.log";
        audit.sysroot = build_sysroot;
        std::error_code ec;
        std::filesystem::create_directories(audit.events, ec);
        if (ec) return std::nullopt;
        {
            std::ofstream process_log(audit.process_exec_log,
                std::ios::out | std::ios::trunc);
            if (!process_log) return std::nullopt;
        }
        std::filesystem::permissions(audit.root,
            std::filesystem::perms::owner_all | std::filesystem::perms::group_all
                | std::filesystem::perms::others_all,
            std::filesystem::perm_options::replace, ec);
        if (ec) return std::nullopt;
        std::filesystem::permissions(audit.events,
            std::filesystem::perms::owner_all | std::filesystem::perms::group_all
                | std::filesystem::perms::others_all,
            std::filesystem::perm_options::replace, ec);
        if (ec) return std::nullopt;
        const auto real_root = audit.root / "host-root";
        std::filesystem::create_directories(real_root, ec);
        if (ec) return std::nullopt;
        const auto events = sage::build::shell_quote(audit.events.string());
        // Absolute-path compiler bypasses cannot be audited with PATH alone.
        // Fail closed when bubblewrap is unavailable instead of claiming
        // provenance that a recipe could evade.
        const bool sandboxed = resolve("bwrap").has_value();
        if (!sandboxed) {
            sage::util::log_error("ToolAudit: bubblewrap ('bwrap') executable not found in PATH; recipe v2 requires bwrap for hermetic execution fencing");
            return std::nullopt;
        }
        const auto execution_path = [&](std::string_view role,
                                        const std::filesystem::path& real) {
            (void)role;
            // The namespace later bind-mounts the configured sysroot at this
            // mirror. Keeping the compiler's original directory layout is important
            // for GCC's cc1 lookup and for Clang's resource-dir discovery.
            return (real_root / real.lexically_relative(sysroot_root)).string();
        };
        const auto execution_paths = [&](std::string_view role,
                                         const std::filesystem::path& real) {
            std::vector<std::string> paths{execution_path(role, real)};
            std::ifstream in(real);
            std::string first;
            if (!in || !std::getline(in, first) || !first.starts_with("#!"))
                return paths;
            const auto dir = real.parent_path();
            std::string line;
            while (std::getline(in, line)) {
                constexpr std::string_view marker = "exec \"$dir/";
                const auto begin = line.find(marker);
                if (begin == std::string::npos) continue;
                const auto target_begin = begin + marker.size();
                const auto target_end = line.find('"', target_begin);
                if (target_end == std::string::npos || target_end == target_begin)
                    continue;
                std::error_code ec;
                const auto target = std::filesystem::weakly_canonical(
                    dir / line.substr(target_begin, target_end - target_begin), ec);
                const auto relative = target.lexically_relative(sysroot_root);
                if (ec || relative.empty() || relative == ".."
                    || relative.string().starts_with("../")
                    || !std::filesystem::is_regular_file(target, ec)) continue;
                const auto path = execution_path(role, target);
                if (std::ranges::find(paths, path) == paths.end())
                    paths.push_back(path);
            }
            return paths;
        };
        const auto namespace_path = [&](const std::filesystem::path& real) {
            return (std::filesystem::path("/")
                / real.lexically_relative(sysroot_root)).lexically_normal();
        };
        const auto fmt = [](std::string_view pattern, auto&&... values) {
            return std::vformat(pattern, std::make_format_args(values...));
        };
        // Execute the canonical file from the read-only host-root mirror,
        // but retain the administrator's driver spelling in argv[0].  The
        // canonical compiler path itself may be a bind-mounted Sage wrapper
        // (for example /usr/bin/gcc -> .../gcc), so following that symlink in
        // the namespace would recurse into the wrapper forever.  A relative
        // symlink under tool-audit keeps resolution inside host-root and does
        // not cross the namespace's bind mounts.
        const auto real_driver_link = [&](std::string_view role,
                                          const std::filesystem::path& real,
                                          const std::filesystem::path& spelling)
            -> std::optional<std::filesystem::path> {
            const auto basename = spelling.filename().string();
            if (basename.empty()) return std::nullopt;
            const auto link = audit.root / "driver-bin" / std::string(role) / basename;
            std::filesystem::create_directories(link.parent_path(), ec);
            if (ec) return std::nullopt;
            const auto target = std::filesystem::path(execution_path(role, real));
            const auto relative = target.lexically_relative(link.parent_path());
            if (relative.empty() || relative.is_absolute()) return std::nullopt;
            std::filesystem::remove(link, ec);
            if (ec) return std::nullopt;
            std::filesystem::create_symlink(relative, link, ec);
            if (ec) return std::nullopt;
            return link;
        };
        const auto make_driver = [&](std::string_view role,
                                     const std::filesystem::path& real_driver,
                                     const std::filesystem::path& spelling,
                                     std::string_view name) {
            const auto driver_link = real_driver_link(role, real_driver, spelling);
            if (!driver_link) return false;
            const auto script = fmt(
                "#!/bin/sh\nprintf '%s ' \"$@\" > {}/exec-{}-$$\n"
                "link=1\nbackend=0\nfor arg do\n"
                " case \"$arg\" in -c|-E|-S) link=0;; esac\n"
                " case \"$arg\" in *-fuse-ld=*|*-fuse-ld-*) backend=1;; esac\n"
                "done\nprintf '%s\\n' '{}' > {}/mark-{}-$$\n"
                "[ \"$link\" -eq 1 ] && printf '%s\\n' x > {}/mark-linker-driver-$$\n"
                "[ \"$link\" -eq 1 ] && printf '%s\\n' x > {}/mark-linker-driver:{}-$$\n"
                "[ \"$backend\" -eq 1 ] && printf '%s\\n' x > {}/mark-linker-selected-$$\n"
                "exec {} \"$@\"\n", events, role, role, events, role, events,
                events, role, events,
                sage::build::shell_quote(driver_link->string()));
            return write_executable(audit.root / std::string(name), script);
        };
        if (cc && cc_driver && !make_driver("cc", *cc, *cc_driver, "sage-cc")) return std::nullopt;
        if (cxx && cxx_driver && !make_driver("cxx", *cxx, *cxx_driver, "sage-cxx")) return std::nullopt;
        if (rustc && !make_driver("rustc", *rustc, *rustc, "sage-rustc")) return std::nullopt;
        if (cache) {
            const auto cache_exec = namespace_path(*cache).string();
            const auto make_cache_driver = [&](std::string_view requested,
                                               std::string_view driver,
                                               std::string& output) {
                const auto basename =
                    std::filesystem::path(requested).filename().string();
                if (basename.empty()) return false;
                const auto wrapper = audit.root / "cache-bin" / basename;
                std::filesystem::create_directories(wrapper.parent_path(), ec);
                if (ec) return false;
                const auto script = "#!/bin/sh\nexec "
                    + sage::build::shell_quote(cache_exec) + " "
                    + sage::build::shell_quote(
                        (audit.root / std::string(driver)).string())
                    + " \"$@\"\n";
                if (!write_executable(wrapper, script)) return false;
                output = wrapper.string();
                return true;
            };
            if (cc && cc_driver
                && !make_cache_driver(tools.cc, "sage-cc", audit.cc_cache))
                return std::nullopt;
            if (cxx && cxx_driver
                && !make_cache_driver(tools.cxx, "sage-cxx", audit.cxx_cache))
                return std::nullopt;
            audit.cache_for_build = cache_exec;
        }
        if (linker) {
            const auto linker_link = real_driver_link(
                "linker", *linker, linker_driver.value_or(*linker));
            if (!linker_link) return std::nullopt;
            const auto script = fmt(
                "#!/bin/sh\nprintf '%s ' \"$@\" > {}/exec-linker-$$\n"
                "printf '%s\\n' x > {}/mark-linker-$$\n"
                "exec {} \"$@\"\n", events, events,
                sage::build::shell_quote(linker_link->string()));
            if (!write_executable(audit.root / "sage-linker", script)) return std::nullopt;
        }
        if (cc) audit.expected_real_execs.emplace("cc", execution_paths("cc", *cc));
        if (cxx) audit.expected_real_execs.emplace("cxx", execution_paths("cxx", *cxx));
        if (linker) audit.expected_real_execs.emplace("linker", execution_paths("linker", *linker));
        if (rustc) audit.expected_real_execs.emplace("rustc", execution_paths("rustc", *rustc));
        const std::array<std::string_view, 14> base_fenced{
            "cc", "c++", "gcc", "g++", "clang", "clang++", "ld", "ld.bfd",
            "ld.gold", "ld.lld", "lld", "mold", "ld.mold", "rustc"};
        std::vector<std::string> fenced;
        for (const auto name : base_fenced) fenced.emplace_back(name);
        const auto looks_like_tool = [](std::string_view name) {
            for (const auto prefix : {std::string_view{"cc"}, std::string_view{"c++"},
                                      std::string_view{"gcc"}, std::string_view{"g++"},
                                      std::string_view{"clang"}, std::string_view{"clang++"},
                                      std::string_view{"ld"}, std::string_view{"lld"},
                                      std::string_view{"mold"}, std::string_view{"rustc"}}) {
                if (name.starts_with(prefix)
                    && (name.size() == prefix.size()
                        || name[prefix.size()] == '-'
                        || std::isdigit(static_cast<unsigned char>(name[prefix.size()]))))
                    return true;
            }
            return false;
        };
        for (const auto& directory : {std::filesystem::path{"/usr/bin"},
                                      std::filesystem::path{"/bin"},
                                      std::filesystem::path{"/usr/local/bin"}}) {
            std::error_code scan_ec;
            for (const auto& entry : std::filesystem::directory_iterator(directory, scan_ec)) {
                const auto name = entry.path().filename().string();
                if (looks_like_tool(name)
                    && std::ranges::find(fenced, name) == fenced.end())
                    fenced.push_back(name);
            }
        }
        const auto deny = [&](std::string_view name) {
            const auto script = fmt(
                "#!/bin/sh\nprintf '%s\\n' x > {}/mark-unexpected-$$\nprintf '%s\\n' "
                "'Sage rejected unmanaged tool {}' >&2\nexit 125\n", events, name);
            return write_executable(audit.root / std::string(name), script);
        };
        const auto compiler_alias = [&](std::string_view name) -> std::string_view {
            if (name.starts_with("rustc")) return rustc ? "sage-rustc" : "";
            if (name == "c++" || name.starts_with("c++-")
                || name.starts_with("g++") || name.starts_with("clang++"))
                return cxx ? "sage-cxx" : "";
            if (name == "cc" || name.starts_with("cc-")
                || name.starts_with("gcc") || name.starts_with("clang"))
                return cc ? "sage-cc" : "";
            if (name == "ld" || name.starts_with("ld.") || name.starts_with("ld-")
                || name == "lld" || name.starts_with("lld-")
                || name == "mold" || name.starts_with("mold-"))
                return linker ? "sage-linker" : "";
            return {};
        };
        const auto alias = [&](std::string_view name, std::string_view target) {
            if (target.empty()) return false;
            const auto target_path = target.starts_with("sage-")
                ? (audit.root / std::string(target)).string()
                : std::string(target);
            const auto script = "#!/bin/sh\nexec "
                + sage::build::shell_quote(target_path) + " \"$@\"\n";
            return write_executable(audit.root / std::string(name), script);
        };
        for (const auto& name : fenced) {
            const auto target = compiler_alias(name);
            if (!(target.empty() ? deny(name) : alias(name, target)))
                return std::nullopt;
        }
        // Canonical compiler paths can be shared by multiple driver
        // spellings (clang/clang++, gcc/g++).  A single bind target therefore
        // dispatches on argv[0], preserving the requested C versus C++ role
        // even when the kernel resolves the symlink before exec.
        const auto compiler_dispatch = "#!/bin/sh\ncase \"$0\" in\n"
            "*c++*|*g++*|*clang++*) target="
            + sage::build::shell_quote((audit.root / "sage-cxx").string())
            + ";;\n* ) target="
            + sage::build::shell_quote((audit.root / "sage-cc").string())
            + ";;\nesac\n"
            "if [ ! -x \"$target\" ]; then printf '%s\\n' x > "
            + sage::build::shell_quote((audit.events / "mark-unexpected-").string())
            + "$$; exit 125; fi\nexec \"$target\" \"$@\"\n";
        if (!write_executable(audit.root / "sage-compiler-dispatch", compiler_dispatch))
            return std::nullopt;
        // The selected backend is allowed through PATH and still logs its
        // real execution.  GCC/Clang look up lld as ld.lld and mold as
        // ld.mold when -fuse-ld is used.
        const auto backend_names = tools.linker_family == "lld"
            ? std::array<std::string_view, 2>{"ld.lld", "lld"}
            : tools.linker_family == "mold"
                ? std::array<std::string_view, 2>{"ld.mold", "mold"}
                : std::array<std::string_view, 2>{"ld", "ld.bfd"};
        for (const auto name : backend_names) {
            if (linker) {
                const auto backend_link = real_driver_link(
                    "linker-backend", *linker, linker_driver.value_or(*linker));
                if (!backend_link) return std::nullopt;
                const auto script = fmt(
                    "#!/bin/sh\nprintf '%s ' \"$@\" > {}/exec-linker-$$\n"
                    "printf '%s\\n' x > {}/mark-linker-$$\n"
                    "exec {} \"$@\"\n", events, events,
                    sage::build::shell_quote(backend_link->string()));
                if (!write_executable(audit.root / std::string(name), script))
                    return std::nullopt;
            }
        }
        std::set<std::string> bound_targets;
        if (sandboxed) {
            const auto parent_name = parent.string();
            audit.sandbox = "bwrap --die-with-parent --new-session --unshare-net"
                " --ro-bind " + sage::build::shell_quote(build_sysroot.string())
                + " / --tmpfs /tmp"
                " --dir " + sage::build::shell_quote(parent_name)
                + " --dir " + sage::build::shell_quote((parent / "home").string())
                + " --dir " + sage::build::shell_quote((parent / "tmp").string())
                + " --dir " + sage::build::shell_quote(audit.root.string())
                + " --bind " + sage::build::shell_quote((parent / "home").string())
                + " " + sage::build::shell_quote((parent / "home").string())
                + " --bind " + sage::build::shell_quote((parent / "tmp").string())
                + " " + sage::build::shell_quote((parent / "tmp").string())
                + " --bind " + sage::build::shell_quote(audit.root.string())
                + " " + sage::build::shell_quote(audit.root.string())
                + " --ro-bind " + sage::build::shell_quote(build_sysroot.string())
                + " " + sage::build::shell_quote(real_root.string())
                + " --dev /dev --proc /proc";
            if (!ccache_dir.empty()) {
                std::error_code ccache_ec;
                std::filesystem::create_directories(ccache_dir, ccache_ec);
                if (ccache_ec) return std::nullopt;
                audit.sandbox += " --dir " + sage::build::shell_quote(ccache_dir.string())
                    + " --bind " + sage::build::shell_quote(ccache_dir.string())
                    + " " + sage::build::shell_quote(ccache_dir.string());
            }
            for (const auto& name : fenced) {
                if (auto resolved = resolve(name)) {
                    const auto target = namespace_path(*resolved);
                    if (!bound_targets.insert(target.string()).second) continue;
                    // Bind selected and non-selected aliases alike. Selected
                    // wrappers execute the mirrored sysroot path above, so
                    // an absolute compiler/linker path cannot bypass observation.
                    const bool is_linker_name = name == "ld"
                        || name.starts_with("ld.") || name.starts_with("ld-")
                        || name == "lld" || name.starts_with("lld-")
                        || name == "mold" || name.starts_with("mold-");
                    const bool is_rustc_name = name.starts_with("rustc");
                    const auto source = is_linker_name
                        ? audit.root / std::string(name)
                        : is_rustc_name
                            ? (rustc ? audit.root / "sage-rustc"
                                     : audit.root / std::string(name))
                            : audit.root / "sage-compiler-dispatch";
                    audit.sandbox += " --bind "
                        + sage::build::shell_quote(source.string())
                        + " " + sage::build::shell_quote(target.string());
                }
            }
            const auto bind_selected = [&](const std::optional<std::filesystem::path>& real,
                                           std::string_view wrapper) {
                if (!real) return;
                const auto target = namespace_path(*real);
                if (!bound_targets.insert(target.string()).second) return;
                audit.sandbox += " --bind "
                    + sage::build::shell_quote((audit.root / std::string(wrapper)).string())
                    + " " + sage::build::shell_quote(target.string());
            };
            bind_selected(cc, "sage-cc");
            bind_selected(cxx, "sage-cxx");
            bind_selected(linker, "sage-linker");
            bind_selected(rustc, "sage-rustc");
        }
        // Keep the selected executable's conventional basename in the
        // command presented to build systems.  Xmake (and similar tools)
        // choose their compiler capability module from that basename; passing
        // `sage-cxx` makes it treat a perfectly valid clang++ wrapper as an
        // unknown compiler and can reject C++23 modules before compiling.
        // Each alias still dispatches to the role wrapper above, so the
        // execution marker and full-process audit remain intact.
        const auto selected_alias = [&](const std::optional<std::filesystem::path>& real,
                                        std::string_view requested,
                                        std::string_view role_wrapper) {
            if (!real) return std::string{};
            // `resolve()` canonicalizes symlinks for the actual-exec proof
            // (clang++ commonly resolves to clang-22), but build systems use
            // the requested basename to select the C vs C++ driver. Preserve
            // that spelling for the audit alias.
            const auto name = std::filesystem::path(requested).filename().string();
            const auto path = audit.root / name;
            if (!std::filesystem::exists(path)
                && !alias(name, role_wrapper)) return std::string{};
            return path.string();
        };
        audit.cc = selected_alias(cc, tools.cc, "sage-cc");
        audit.cxx = selected_alias(cxx, tools.cxx, "sage-cxx");
        audit.linker = linker ? (audit.root / "sage-linker").string() : "";
        audit.rustc = selected_alias(rustc, tools.rustc, "sage-rustc");
        std::set<std::string> tool_dirs;
        for (const auto& real : {cc, cxx, linker, rustc})
            if (real) tool_dirs.insert(real->parent_path().string());
        audit.path = audit.root.string();
        for (const auto& directory : tool_dirs) audit.path += ":" + directory;
        audit.path += ":/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
        return std::optional<ToolAudit>(std::move(audit));
    }
};

std::expected<void, std::string> apply_content_policy(
    const std::filesystem::path& pkg_dir,
    const sage::package::ContentPolicy& content)
{
    if (content.empty()) return {};
    std::error_code ec;

    if (content.strip != "none") {
        auto strip = ToolAudit::resolve("strip");
        if (!strip) return std::unexpected(
            std::string{"build.content.strip requires the strip utility inside the build environment"});
        const std::string mode = content.strip == "debug"
            ? "--strip-debug" : "--strip-unneeded";
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 pkg_dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) return std::unexpected("Cannot inspect payload for strip: " + ec.message());
            if (entry.is_symlink(ec) || !entry.is_regular_file(ec)) continue;
            if (!sage::util::scan_elf(entry.path())) continue;
            const auto command = std::format("{} {} {}",
                sage::build::shell_quote(strip->string()), mode,
                sage::build::shell_quote(entry.path().string()));
            if (std::system(command.c_str()) != 0)
                return std::unexpected("Cannot strip payload ELF file: "
                                       + entry.path().string());
        }
    }

    if (content.man_compress == "gzip") {
        auto gzip = ToolAudit::resolve("gzip");
        if (!gzip) return std::unexpected(
            std::string{"build.content.man_compress requires the gzip utility inside the build environment"});
        const auto man_root = pkg_dir / "usr/share/man";
        if (std::filesystem::is_directory(man_root, ec)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(
                     man_root, std::filesystem::directory_options::skip_permission_denied, ec)) {
                if (ec) return std::unexpected("Cannot inspect man pages: " + ec.message());
                if (entry.is_symlink(ec) || !entry.is_regular_file(ec)) continue;
                if (entry.path().extension() == ".gz") continue;
                // -n omits the timestamp and original name: the archive stays
                // byte-reproducible across builds.
                const auto command = std::format("{} -n {}",
                    sage::build::shell_quote(gzip->string()),
                    sage::build::shell_quote(entry.path().string()));
                if (std::system(command.c_str()) != 0)
                    return std::unexpected("Cannot compress man page: "
                                           + entry.path().string());
            }
        }
    }

    if (!content.locales.empty()) {
        const auto locale_root = pkg_dir / "usr/share/locale";
        if (std::filesystem::is_directory(locale_root, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(
                     locale_root, ec)) {
                if (!entry.is_directory(ec)) continue;
                if (!std::ranges::contains(content.locales,
                        entry.path().filename().string())) {
                    std::filesystem::remove_all(entry.path(), ec);
                    if (ec) return std::unexpected("Cannot prune locale: " + ec.message());
                }
            }
        }
    }

    if (content.shebangs == "absolute") {
        static constexpr std::pair<std::string_view, std::string_view> interpreters[]{
            {"sh", "/bin/sh"},
            {"bash", "/usr/bin/bash"},
            {"python3", "/usr/bin/python3"},
            {"perl", "/usr/bin/perl"},
            {"awk", "/usr/bin/awk"},
        };
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 pkg_dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) return std::unexpected("Cannot inspect payload for shebangs: " + ec.message());
            if (entry.is_symlink(ec) || !entry.is_regular_file(ec)) continue;
            std::ifstream in(entry.path(), std::ios::binary);
            std::string first(256, '\0');
            in.read(first.data(), static_cast<std::streamsize>(first.size()));
            first.resize(static_cast<std::size_t>(in.gcount()));
            const auto newline = first.find('\n');
            if (newline != std::string::npos) first.resize(newline);
            if (!first.empty() && first.back() == '\r') first.pop_back();
            constexpr std::string_view env_prefix = "#!/usr/bin/env ";
            if (!first.starts_with(env_prefix)) continue;
            auto rest = std::string_view(first).substr(env_prefix.size());
            if (rest.starts_with("-S ")) rest = rest.substr(3);
            while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
            const auto space = rest.find(' ');
            const auto interpreter = (space == std::string_view::npos) ? rest : rest.substr(0, space);
            const auto args = (space == std::string_view::npos) ? std::string_view{} : rest.substr(space);
            std::string_view absolute;
            for (const auto& [name, path] : interpreters) {
                if (name == interpreter) { absolute = path; break; }
            }
            if (absolute.empty()) {
                return std::unexpected(std::format(
                    "Payload script '{}' uses '#!/usr/bin/env {}' but the interpreter "
                    "is not a Sage-standard one; pin it in the recipe",
                    entry.path().filename().string(), interpreter));
            }
            std::string replacement = "#!" + std::string(absolute) + std::string(args);
            std::ifstream full(entry.path(), std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(full)),
                             std::istreambuf_iterator<char>());
            body.replace(0, first.size(), replacement);
            std::ofstream out(entry.path(), std::ios::binary | std::ios::trunc);
            out.write(body.data(), static_cast<std::streamsize>(body.size()));
            if (!out) return std::unexpected(
                "Cannot rewrite shebang: " + entry.path().string());
        }
    }
    return {};
}

// Materialize the recipe's sysusers declaration as a sysusers.d fragment
// inside the payload. The fragment is injected after the payload filter so a
// split package cannot accidentally drop its own user declaration.
std::expected<void, std::string> apply_sysusers_fragment(
    const std::filesystem::path& pkg_dir,
    const std::string& package_name,
    const std::vector<sage::package::SysUserEntry>& sysusers)
{
    if (sysusers.empty()) return {};
    std::error_code ec;
    const auto fragment_dir = pkg_dir / "usr/lib/sysusers.d";
    std::filesystem::create_directories(fragment_dir, ec);
    if (ec) return std::unexpected("Cannot create sysusers.d directory: " + ec.message());
    std::ostringstream ss;
    ss << "# Generated by sage for " << package_name << "\n";
    for (const auto& entry : sysusers) {
        ss << (entry.type == "group" ? 'g' : 'u') << ' ' << entry.name << ' '
           << (entry.id ? std::to_string(*entry.id) : "-");
        if (entry.type == "user") {
            ss << " \"" << entry.description << "\" "
               << (entry.home.empty() ? "-" : entry.home) << ' '
               << (entry.shell.empty() ? "/usr/bin/nologin" : entry.shell);
            if (!entry.group.empty()) ss << ' ' << entry.group;
        } else {
            ss << " -";
        }
        ss << '\n';
    }
    const auto fragment = fragment_dir / (package_name + ".conf");
    std::ofstream out(fragment, std::ios::binary | std::ios::trunc);
    if (!out) return std::unexpected("Cannot write sysusers fragment: " + fragment.string());
    out << ss.str();
    out.close();
    std::filesystem::permissions(fragment,
        std::filesystem::perms::owner_read | std::filesystem::perms::group_read
            | std::filesystem::perms::others_read,
        std::filesystem::perm_options::replace, ec);
    if (ec) return std::unexpected("Cannot set sysusers fragment mode: " + ec.message());
    return {};
}


std::expected<std::string, std::string> select_compiler_cache(
    std::string_view requested,
    sage::package::BuildSystem system,
    const std::filesystem::path& build_sysroot)
{
    if (requested.empty() || requested == "none") return std::string{"none"};
    std::error_code root_ec;
    const auto sysroot = std::filesystem::weakly_canonical(
        build_sysroot, root_ec);
    if (root_ec) return std::unexpected(std::format(
        "Cannot resolve compiler-cache build sysroot '{}': {}",
        build_sysroot.string(), root_ec.message()));
    const auto available = [&](std::string_view name) {
        auto path = ToolAudit::resolve(name);
        if (!path) return false;
        if (build_sysroot == "/") return true;
        std::error_code ec;
        const auto canonical = std::filesystem::weakly_canonical(*path, ec);
        return !ec && (canonical == sysroot
            || canonical.string().starts_with(sysroot.string() + "/"));
    };
    if (requested == "auto") {
        const std::array<std::string_view, 2> preference =
            system == sage::package::BuildSystem::Cargo
                ? std::array<std::string_view, 2>{"sccache", "ccache"}
                : std::array<std::string_view, 2>{"ccache", "sccache"};
        for (const auto mode : preference)
            if (available(mode)) return std::string(mode);
        return std::string{"none"};
    }
    if (requested != "ccache" && requested != "sccache")
        return std::unexpected(
            std::string{"compiler_cache must be ccache, sccache, auto, or none"});
    if (!available(requested))
        return std::unexpected(std::format(
            "Configured compiler cache '{}' is not available inside build sysroot '{}'",
            requested, build_sysroot.string()));
    return std::string(requested);
}
std::expected<void, std::string> validate_check_dependencies(
    const sage::package::Recipe& recipe,
    const sage::config::SystemConfig& cfg)
{
    if (recipe.check_deps.empty()) return {};
    const auto db_path = cfg.build.sysroot == "/"
        ? cfg.db_path : cfg.build.sysroot / "var/lib/sage/data.mdb";
    auto db = sage::db::Database::open_existing_read_only_no_lock(db_path);
    if (!db) return std::unexpected(std::format(
        "check_dependencies require a readable package database in build sysroot '{}': {}",
        cfg.build.sysroot.string(), db.error()));
    auto installed = db->list_installed_package_summaries();
    if (!installed) return std::unexpected(std::format(
        "cannot read check dependency metadata from build sysroot '{}': {}",
        cfg.build.sysroot.string(), installed.error()));
    std::vector<sage::package::Dependency> requests;
    requests.reserve(recipe.check_deps.size());
    for (const auto& raw : recipe.check_deps) {
        auto request = sage::package::Dependency::parse(raw);
        if (request.name.empty()) return std::unexpected(
            std::string{"check_dependencies contains a dependency with an empty package name"});
        requests.push_back(std::move(request));
    }
    sage::solver::DependencySolver solver(*installed, cfg.providers);
    auto resolved = solver.solve(requests);
    if (!resolved) return std::unexpected(
        "check_dependencies are not satisfied by the configured build sysroot: "
        + resolved.error());
    return {};
}


} // namespace sage::cli
