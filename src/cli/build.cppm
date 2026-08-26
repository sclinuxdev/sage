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

// The backend is allowed to install into the isolated staging root, but the
// recipe remains the authority over which of those files become package
// payload.  Apply the allowlist only after the backend has finished: this
// keeps Autotools/CMake/Meson/etc. free to run their normal install logic while
// making split packages explicit and reviewable.  All matching is against
// canonical relative paths below pkg_dir; no pattern can escape that root.
std::expected<void, std::string> apply_payload_policy(
    const std::filesystem::path& pkg_dir,
    const std::vector<std::string>& includes,
    const std::vector<std::string>& excludes)
{
    if (includes.empty() && excludes.empty()) return {};
    if (!std::filesystem::exists(pkg_dir))
        return std::unexpected("Install payload staging root does not exist: "
                               + pkg_dir.string());

    struct Entry {
        std::filesystem::path disk;
        std::string relative;
        bool directory{false};
        bool symlink{false};
    };
    std::vector<Entry> entries;
    std::error_code ec;
    for (const auto& item : std::filesystem::recursive_directory_iterator(
             pkg_dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) return std::unexpected("Cannot inspect staged payload: " + ec.message());
        const auto relative = item.path().lexically_relative(pkg_dir).generic_string();
        if (relative.empty() || relative == ".") continue;
        const bool symlink = item.is_symlink(ec);
        if (ec) return std::unexpected("Cannot inspect staged payload entry '" + relative
                                      + "': " + ec.message());
        const bool directory = !symlink && item.is_directory(ec);
        if (ec) return std::unexpected("Cannot inspect staged payload entry '" + relative
                                      + "': " + ec.message());
        entries.push_back({item.path(), relative, directory, symlink});
    }

    const auto matches = [](const std::vector<std::string>& patterns,
                            std::string_view path) {
        return std::ranges::any_of(patterns, [&](const auto& pattern) {
            return sage::util::glob_match(pattern, path);
        });
    };
    std::vector<bool> include_seen(includes.size(), false);
    std::size_t payload_entries = 0;
    for (const auto& entry : entries) {
        // Directories are retained only when at least one selected child
        // remains; they are pruned in the reverse walk below.
        if (entry.directory && !entry.symlink) continue;
        bool selected = includes.empty();
        for (std::size_t i = 0; i < includes.size(); ++i) {
            if (!sage::util::glob_match(includes[i], entry.relative)) continue;
            selected = true;
            include_seen[i] = true;
        }
        if (matches(excludes, entry.relative)) selected = false;
        if (!selected) {
            std::filesystem::remove(entry.disk, ec);
            if (ec) return std::unexpected("Cannot remove excluded staged payload '"
                                           + entry.relative + "': " + ec.message());
        } else {
            ++payload_entries;
        }
    }

    // Remove directories which became empty.  Never recurse through a
    // symlink, and never remove the staging root itself.
    std::ranges::sort(entries, [](const Entry& left, const Entry& right) {
        return left.relative.size() > right.relative.size();
    });
    for (const auto& entry : entries) {
        if (!entry.directory || entry.symlink) continue;
        const bool empty = std::filesystem::is_empty(entry.disk, ec);
        if (ec) return std::unexpected("Cannot inspect staged payload directory '"
                                       + entry.relative + "': " + ec.message());
        if (empty) {
            std::filesystem::remove(entry.disk, ec);
            if (ec) return std::unexpected("Cannot prune staged payload directory '"
                                           + entry.relative + "': " + ec.message());
        }
    }
    if (payload_entries == 0)
        return std::unexpected("build.install_files selected no payload files");
    for (std::size_t i = 0; i < include_seen.size(); ++i) {
        if (!include_seen[i]) return std::unexpected(std::format(
            "build.install_files pattern '{}' matched no installed payload",
            includes[i]));
    }
    return {};
}

std::expected<void, std::string> apply_install_transforms(
    const std::filesystem::path& source_dir,
    const std::filesystem::path& pkg_dir,
    const std::vector<sage::package::InstallCopy>& copies,
    const std::vector<sage::package::InstallSymlink>& symlinks,
    const std::vector<sage::package::InstallMove>& moves,
    const std::vector<sage::package::InstallRemove>& removes,
    const std::vector<sage::package::InstallGenerate>& generates)
{
    std::error_code ec;
    const auto safe_relative = [](const std::filesystem::path& path) {
        return !path.empty() && !path.is_absolute() && !path.has_root_path()
            && std::ranges::none_of(path, [](const auto& part) { return part == ".."; });
    };
    for (const auto& copy : copies) {
        const auto source = source_dir / copy.source;
        const auto destination = pkg_dir / copy.destination;
        if (!std::filesystem::is_regular_file(source, ec)) {
            if (ec) return std::unexpected("Cannot inspect install copy source '"
                                           + source.string() + "': " + ec.message());
            return std::unexpected("Install copy source does not exist: "
                                   + source.string());
        }
        std::filesystem::create_directories(destination.parent_path(), ec);
        if (ec) return std::unexpected("Cannot create install copy directory for '"
                                       + destination.string() + "': " + ec.message());
        if (!std::filesystem::copy_file(source, destination,
                std::filesystem::copy_options::overwrite_existing, ec)) {
            if (ec) return std::unexpected("Cannot copy install artifact '"
                                           + source.string() + "' to '"
                                           + destination.string() + "': " + ec.message());
            return std::unexpected("Cannot copy install artifact '" + source.string()
                                   + "' to '" + destination.string() + "'");
        }
    }
    for (const auto& move : moves) {
        if (!safe_relative(move.source) || !safe_relative(move.destination))
            return std::unexpected("Install move path escapes the staging root");
        const auto source = pkg_dir / move.source;
        const auto destination = pkg_dir / move.destination;
        if (!std::filesystem::exists(source, ec)) {
            if (ec) return std::unexpected("Cannot inspect install move source '"
                                           + source.string() + "': " + ec.message());
            return std::unexpected("Install move source does not exist: " + source.string());
        }
        std::filesystem::create_directories(destination.parent_path(), ec);
        if (ec) return std::unexpected("Cannot create install move directory: " + ec.message());
        std::filesystem::remove_all(destination, ec);
        if (ec) return std::unexpected("Cannot replace install move destination '"
                                       + destination.string() + "': " + ec.message());
        std::filesystem::rename(source, destination, ec);
        if (ec) return std::unexpected("Cannot move staged payload '" + source.string()
                                       + "' to '" + destination.string() + "': " + ec.message());
    }
    for (const auto& remove : removes) {
        if (!safe_relative(remove.path))
            return std::unexpected("Install remove path escapes the staging root");
        std::vector<std::filesystem::path> matches;
        for (const auto& item : std::filesystem::recursive_directory_iterator(
                 pkg_dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) return std::unexpected("Cannot inspect staged payload for removal: "
                                           + ec.message());
            const auto rel = item.path().lexically_relative(pkg_dir).generic_string();
            if (sage::util::glob_match(remove.path, rel)) matches.push_back(item.path());
        }
        if (matches.empty()) return std::unexpected(
            "Install remove pattern matched no staged payload: " + remove.path);
        std::ranges::sort(matches, [](const auto& left, const auto& right) {
            return left.string().size() > right.string().size();
        });
        for (const auto& path : matches) {
            std::filesystem::remove_all(path, ec);
            if (ec) return std::unexpected("Cannot remove generated payload '"
                                           + path.string() + "': " + ec.message());
        }
    }
    for (const auto& generate : generates) {
        if (!safe_relative(generate.path))
            return std::unexpected("Install generate path escapes the staging root");
        const auto destination = pkg_dir / generate.path;
        std::filesystem::create_directories(destination.parent_path(), ec);
        if (ec) return std::unexpected("Cannot create generated payload directory: "
                                       + ec.message());
        std::ofstream out(destination, std::ios::binary | std::ios::trunc);
        if (!out) return std::unexpected("Cannot create generated payload: "
                                         + destination.string());
        out << generate.content;
        out.close();
        std::filesystem::permissions(destination,
            static_cast<std::filesystem::perms>(generate.mode),
            std::filesystem::perm_options::replace, ec);
        if (ec) return std::unexpected("Cannot set generated payload mode for '"
                                       + destination.string() + "': " + ec.message());
    }
    for (const auto& link : symlinks) {
        if (!safe_relative(link.path))
            return std::unexpected("Install symlink path escapes the staging root");
        const std::filesystem::path target(link.target);
        const auto resolved = (std::filesystem::path(link.path).parent_path()
                               / target).lexically_normal();
        if (target.is_absolute() || target.has_root_path() || resolved.is_absolute()
            || std::ranges::any_of(resolved, [](const auto& part) { return part == ".."; }))
            return std::unexpected("Install symlink target escapes the staging root: "
                                   + link.target);
        const auto destination = pkg_dir / link.path;
        std::filesystem::create_directories(destination.parent_path(), ec);
        if (ec) return std::unexpected("Cannot create install symlink directory for '"
                                       + destination.string() + "': " + ec.message());
        std::filesystem::remove(destination, ec);
        if (ec) return std::unexpected("Cannot replace install symlink '"
                                       + destination.string() + "': " + ec.message());
        std::filesystem::create_symlink(link.target, destination, ec);
        if (ec) return std::unexpected("Cannot create install symlink '"
                                       + destination.string() + "' -> '"
                                       + link.target + "': " + ec.message());
    }
    return {};
}

bool probe_fakeroot(std::string_view executable) {
    if (executable.empty()) return false;
    const auto command = sage::build::shell_quote(executable)
        + " --version >/dev/null 2>&1";
    return std::system(command.c_str()) == 0;
}

// Keep fakeroot's two control variables, but discard the caller's shell
// environment before entering a recipe.  This prevents PATH, locale, Cargo
// config, compiler flags and proxy variables from silently changing a build.
// Sage's deterministic variables are exported by the managed plan (or by the
// legacy phase adapter) inside the clean shell.
std::string hermetic_shell(std::string_view script,
                           std::string_view sandbox_prefix = {}) {
    const std::string runner = sandbox_prefix.empty()
        ? "/bin/sh" : std::string(sandbox_prefix) + " -- /bin/sh";
    return "umask 022; exec env -i FAKEROOTKEY=\"$FAKEROOTKEY\" "
        "LD_PRELOAD=\"$LD_PRELOAD\" "
        "SAGE_TEST_FAKEROOT_ACTIVE=\"$SAGE_TEST_FAKEROOT_ACTIVE\" "
        + runner + " -c "
        + sage::build::shell_quote(script);
}

// fakeroot itself cannot create a user namespace when it is the outer
// process (its LD_PRELOAD state is already active).  For the strict v2 path,
// bubblewrap is therefore outermost and launches the configured fakeroot
// inside the namespace; the recipe shell remains the fakeroot child.
std::string sandboxed_fakeroot_shell(std::string_view fakeroot,
                                     std::string_view script,
                                     std::string_view sandbox_prefix) {
    const char* inherited_path = std::getenv("PATH");
    const std::string launcher_path = inherited_path && *inherited_path
        ? inherited_path
        : "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    const auto inner = "exec " + sage::build::shell_quote(fakeroot)
        + " -- /bin/sh -c " + sage::build::shell_quote(script);
    return "umask 022; exec env -i PATH="
        + sage::build::shell_quote(launcher_path) + " "
        + std::string(sandbox_prefix) + " -- /bin/sh -c "
        + sage::build::shell_quote(inner);
}

struct ToolAudit {
    std::filesystem::path root;
    std::filesystem::path log;
    std::filesystem::path events;
    std::string cc;
    std::string cxx;
    std::string linker;
    std::string rustc;
    std::string path;
    std::string sandbox;

    ToolAudit() = default;
    ToolAudit(const ToolAudit&) = delete;
    ToolAudit& operator=(const ToolAudit&) = delete;
    ToolAudit(ToolAudit&& other) noexcept
        : root(std::move(other.root)), log(std::move(other.log)),
          events(std::move(other.events)),
          cc(std::move(other.cc)), cxx(std::move(other.cxx)),
          linker(std::move(other.linker)), rustc(std::move(other.rustc)),
          path(std::move(other.path)), sandbox(std::move(other.sandbox)) {
        other.root.clear();
    }
    ToolAudit& operator=(ToolAudit&& other) noexcept {
        if (this != &other) {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
            root = std::move(other.root); log = std::move(other.log);
            events = std::move(other.events);
            cc = std::move(other.cc); cxx = std::move(other.cxx);
            linker = std::move(other.linker); rustc = std::move(other.rustc);
            path = std::move(other.path); sandbox = std::move(other.sandbox);
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

    static std::optional<ToolAudit> create(const sage::build::Toolchain& tools,
                                           const std::filesystem::path& parent) {
        auto cc = resolve(tools.cc);
        auto cxx = resolve(tools.cxx);
        auto linker = resolve(tools.linker);
        auto rustc = tools.rustc.empty() ? std::optional<std::filesystem::path>{}
                                         : resolve(tools.rustc);
        if ((!tools.cc.empty() && !cc) || (!tools.cxx.empty() && !cxx)
            || (!tools.linker.empty() && !linker)
            || (!tools.rustc.empty() && !rustc)) return std::nullopt;
        ToolAudit audit;
        audit.root = parent / "tool-audit";
        audit.log = audit.root / "executions.log";
        audit.events = audit.root / "events";
        std::error_code ec;
        std::filesystem::create_directories(audit.events, ec);
        if (ec) return std::nullopt;
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
        if (!sandboxed) return std::nullopt;
        const auto execution_path = [&](std::string_view role,
                                        const std::filesystem::path& real) {
            (void)role;
            // The namespace later bind-mounts the host root at this mirror.
            // Keeping the compiler's original directory layout is important
            // for GCC's cc1 lookup and for Clang's resource-dir discovery.
            return (real_root / real.relative_path()).string();
        };
        const auto fmt = [](std::string_view pattern, auto&&... values) {
            return std::vformat(pattern, std::make_format_args(values...));
        };
        const auto make_driver = [&](std::string_view role,
                                     const std::filesystem::path& real,
                                     std::string_view name) {
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
                sage::build::shell_quote(execution_path(role, real)));
            return write_executable(audit.root / std::string(name), script);
        };
        if (cc && !make_driver("cc", *cc, "sage-cc")) return std::nullopt;
        if (cxx && !make_driver("cxx", *cxx, "sage-cxx")) return std::nullopt;
        if (rustc && !make_driver("rustc", *rustc, "sage-rustc")) return std::nullopt;
        if (linker) {
            const auto script = fmt(
                "#!/bin/sh\nprintf '%s ' \"$@\" > {}/exec-linker-$$\n"
                "printf '%s\\n' x > {}/mark-linker-$$\n"
                "exec {} \"$@\"\n", events, events,
                sage::build::shell_quote(execution_path("linker", *linker)));
            if (!write_executable(audit.root / "sage-linker", script)) return std::nullopt;
        }
        const std::array<std::string_view, 15> base_fenced{
            "cc", "c++", "gcc", "g++", "clang", "clang++", "ld", "ld.bfd",
            "ld.gold", "ld.lld", "lld", "mold", "ld.mold", "rustc", "ccache"};
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
            if (name.starts_with("rustc")) return audit.rustc;
            if (name == "c++" || name.starts_with("c++-")
                || name.starts_with("g++") || name.starts_with("clang++"))
                return audit.cxx;
            if (name == "cc" || name.starts_with("cc-")
                || name.starts_with("gcc") || name.starts_with("clang"))
                return audit.cc;
            if (name == "ld" || name.starts_with("ld.") || name.starts_with("ld-")
                || name == "lld" || name.starts_with("lld-")
                || name == "mold" || name.starts_with("mold-"))
                return audit.linker;
            return {};
        };
        const auto alias = [&](std::string_view name, std::string_view target) {
            if (target.empty()) return false;
            const auto script = "#!/bin/sh\nexec "
                + sage::build::shell_quote(target) + " \"$@\"\n";
            return write_executable(audit.root / std::string(name), script);
        };
        for (const auto& name : fenced) {
            const auto target = compiler_alias(name);
            if (!(target.empty() ? deny(name) : alias(name, target)))
                return std::nullopt;
        }
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
                const auto script = fmt(
                    "#!/bin/sh\nprintf '%s ' \"$@\" > {}/exec-linker-$$\n"
                    "printf '%s\\n' x > {}/mark-linker-$$\n"
                    "exec {} \"$@\"\n", events, events,
                    sage::build::shell_quote(execution_path("linker", *linker)));
                if (!write_executable(audit.root / std::string(name), script))
                    return std::nullopt;
            }
        }
        std::set<std::string> bound_targets;
        if (sandboxed) {
            const auto parent_name = parent.string();
            audit.sandbox = "bwrap --die-with-parent --new-session --unshare-net"
                " --ro-bind / / --tmpfs /tmp"
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
                + " --ro-bind / " + sage::build::shell_quote(real_root.string())
                + " --dev /dev --proc /proc";
            for (const auto& name : fenced) {
                if (auto resolved = resolve(name)) {
                    if (!bound_targets.insert(resolved->string()).second) continue;
                    // Bind selected and non-selected aliases alike. Selected
                    // wrappers execute the mirrored host-root path above, so
                    // an absolute compiler/linker path cannot bypass observation.
                    audit.sandbox += " --bind "
                        + sage::build::shell_quote((audit.root / std::string(name)).string())
                        + " " + sage::build::shell_quote(resolved->string());
                }
            }
            const auto bind_selected = [&](const std::optional<std::filesystem::path>& real,
                                           std::string_view wrapper) {
                if (!real || !bound_targets.insert(real->string()).second) return;
                audit.sandbox += " --bind "
                    + sage::build::shell_quote((audit.root / std::string(wrapper)).string())
                    + " " + sage::build::shell_quote(real->string());
            };
            bind_selected(cc, "sage-cc");
            bind_selected(cxx, "sage-cxx");
            bind_selected(linker, "sage-linker");
            bind_selected(rustc, "sage-rustc");
        }
        audit.cc = cc ? (audit.root / "sage-cc").string() : "";
        audit.cxx = cxx ? (audit.root / "sage-cxx").string() : "";
        audit.linker = linker ? (audit.root / "sage-linker").string() : "";
        audit.rustc = rustc ? (audit.root / "sage-rustc").string() : "";
        std::set<std::string> tool_dirs;
        for (const auto& real : {cc, cxx, linker, rustc})
            if (real) tool_dirs.insert(real->parent_path().string());
        audit.path = audit.root.string();
        for (const auto& directory : tool_dirs) audit.path += ":" + directory;
        audit.path += ":/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
        return std::optional<ToolAudit>(std::move(audit));
    }
};

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
        std::string cc_for_build, cxx_for_build, linker_for_build, rustc_for_build,
            path_for_build;
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
            // `lld` is a generic dispatcher and exits non-zero on --version;
            // the actual linker executable is ld.lld. Keep accepting the
            // friendly config alias but record and execute the real driver.
            const std::string actual_linker = linker_name == "lld"
                ? "ld.lld" : linker_name;
            auto probed_linker = probe_tool(actual_linker, true);
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
                    cc_name, cxx_name, actual_linker);
                return;
            }
            auto requirement = sage::build::validate_toolchain(r,
                {.cc = cc_name, .cxx = cxx_name, .linker = actual_linker,
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
            .linker = r.schema_version == 2
                ? (linker_name == "lld" ? "ld.lld" : linker_name) : "",
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
    const bool needs_managed_toolchain =
        r.schema_version == 2
        && r.managed_build.system != sage::package::BuildSystem::Script;
    const bool script_recipe =
        r.schema_version == 2
        && r.managed_build.system == sage::package::BuildSystem::Script;
    if (script_recipe) {
        // Script recipes are for deterministic repackaging and package fixups;
        // they must not manufacture compiler/linker provenance.
        candidates.push_back(Toolchain{});
    } else if (!r.cc.empty()) {
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
        if (needs_managed_toolchain) {
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
    struct HermeticCleanup {
        std::filesystem::path root;
        ~HermeticCleanup() {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }
    } hermetic_cleanup{hermetic_root};

    std::optional<ToolAudit> tool_audit;
    if (r.schema_version == 2 && !candidates.empty()) {
        auto& selected = candidates.front();
        auto canonical = sage::build::Toolchain{
            .cc = selected.cc, .cxx = selected.cxx, .linker = selected.linker,
            .rustc = selected.rustc,
            .compiler_version = selected.compiler_version,
            .cxx_version = selected.cxx_version,
            .linker_version = selected.linker_version,
            .rustc_version = selected.rustc_version,
            .compiler_family = selected.compiler_family,
            .cxx_family = selected.cxx_family,
            .linker_family = selected.linker_family,
            .rustc_family = selected.rustc_family};
        tool_audit = ToolAudit::create(canonical, hermetic_root);
        if (!tool_audit) {
            sage::util::log_error(
                "Cannot create the v2 tool audit fence; refusing to build without actual execution evidence");
            return 1;
        }
        selected.cc_for_build = tool_audit->cc;
        selected.cxx_for_build = tool_audit->cxx;
        selected.linker_for_build = tool_audit->linker;
        selected.rustc_for_build = tool_audit->rustc;
        selected.path_for_build = tool_audit->path;
    }

    std::filesystem::path dist_dir = recipe_dir / "distfiles";
    // A local v2 project may itself contain a conventional `src/` directory
    // (Cargo does), so never reuse that path as Sage's writable build root.
    // The recipe tree stays read-only in the sandbox; local source is copied
    // into this private staging tree before Sage normalizes and builds it.
    std::filesystem::path src_dir = r.source_url.empty()
        ? recipe_dir / ".sage-source" : recipe_dir / "src";
    std::filesystem::path pkg_dir = recipe_dir / "pkg";
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
        // The recipe tree is immutable input. Only the extracted source and
        // package staging directories are writable; /tmp is a private tmpfs
        // with Sage's hermetic HOME/TMP subtrees mounted back into it.
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
        // The sandbox owns a private /tmp, so a test adapter or an
        // administrator-provided fakeroot under /tmp would otherwise vanish
        // before the inner command starts. Bind the exact executable (and
        // create its parent in the namespace) rather than exposing its whole
        // host directory.
        if (auto fakeroot_path = ToolAudit::resolve(bcfg.fakeroot)) {
            const auto parent = fakeroot_path->parent_path();
            const bool hidden_tmp = parent.string().starts_with("/tmp/");
            tool_audit->sandbox += (hidden_tmp
                ? " --dir " + sage::build::shell_quote(parent.string()) : "")
                + " --ro-bind " + sage::build::shell_quote(fakeroot_path->string())
                + " " + sage::build::shell_quote(fakeroot_path->string());
        }
    }
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
        } else if (r.schema_version == 2 && r.source_url.empty()) {
            // Local v2 projects are immutable recipe inputs. Copy everything
            // except Sage metadata/staging directories into a writable source
            // tree so arbitrary prepare/build/install steps can modify it.
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
        }
        if (r.schema_version == 2) {
            // Local patch attachments live beside recipe.toml in the source
            // tree. Make them first-class distfiles so the same `patch`
            // command works for downloaded and local-source recipes.
            std::filesystem::create_directories(src_dir / "distfiles", ec);
            for (const auto& patch : r.managed_build.patches) {
                const auto beside_recipe = recipe_dir / patch;
                const auto attached = std::filesystem::is_regular_file(beside_recipe, ec)
                    ? beside_recipe : recipe_dir / "distfiles" / patch;
                if (!std::filesystem::is_regular_file(attached, ec)) continue;
                std::filesystem::copy_file(attached, src_dir / "distfiles" / patch,
                    std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) {
                    sage::util::log_error(
                        "Failed to stage local patch '{}': {}", patch, ec.message());
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
                 .rustc = cand.rustc,
                 .cc_for_build = cand.cc_for_build,
                 .cxx_for_build = cand.cxx_for_build,
                 .linker_for_build = cand.linker_for_build,
                 .rustc_for_build = cand.rustc_for_build,
                 .path_for_build = cand.path_for_build,
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
                const auto fakeroot_cmd = tool_audit && !tool_audit->sandbox.empty()
                    ? sandboxed_fakeroot_shell(
                        ToolAudit::resolve(bcfg.fakeroot)
                            .value_or(std::filesystem::path(bcfg.fakeroot)).string(),
                        full_cmd,
                                               tool_audit->sandbox)
                    : sage::build::fakeroot_command(bcfg.fakeroot,
                        hermetic_shell(full_cmd));
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

    // A backend's DESTDIR is an intermediate install tree, not the package
    // boundary.  Enforce the recipe's explicit v2 payload policy before ELF
    // scanning and archive creation so omitted files cannot reappear in the
    // manifest through a later packaging pass.
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
        auto payload = apply_payload_policy(
            pkg_dir, r.managed_build.install_files,
            r.managed_build.install_excludes);
        if (!payload) {
            sage::util::log_error("Invalid staged payload for '{}': {}",
                                  r.name, payload.error());
            return 1;
        }
        if (!tool_audit) {
            sage::util::log_error("Managed build has no execution audit");
            return 1;
        }
        if (tool_audit->executions("unexpected") != 0) {
            sage::util::log_error(
                "The build attempted to execute a compiler/linker outside Sage's selected toolchain");
            return 1;
        }
        const bool cargo = r.managed_build.system
            == sage::package::BuildSystem::Cargo;
        const bool script = r.managed_build.system
            == sage::package::BuildSystem::Script;
        const auto compiler_execs = tool_audit->executions("cc")
            + tool_audit->executions("cxx");
        const auto rustc_execs = tool_audit->executions("rustc");
        if ((!script && cargo && rustc_execs == 0)
            || (!script && !cargo && compiler_execs == 0)) {
            sage::util::log_error(
                "No Sage audit marker proves that the configured compiler executed");
            return 1;
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
        if (!script && has_elf && linker_execs == 0) {
            sage::util::log_error(
                "ELF payload exists but no selected linker execution was observed");
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
        const auto normalize_audit_text = [&](std::string value) {
            return sage::build::replace_all(std::move(value), hermetic_root.string(),
                                            "<sage-build>");
        };
        for (auto command : tool_audit->commands())
            manifest.managed_build_commands.push_back(normalize_audit_text(std::move(command)));
        const auto normalize_parameters = [&](std::vector<std::string>& parameters) {
            for (auto& parameter : parameters)
                parameter = normalize_audit_text(std::move(parameter));
        };
        normalize_parameters(managed_cc_parameters);
        normalize_parameters(managed_cxx_parameters);
        normalize_parameters(managed_linker_parameters);
        normalize_parameters(managed_rustc_parameters);
        const auto add_tool = [&](std::string role, std::string executable,
                                  std::string family, std::string version,
                                  std::vector<std::string> parameters,
                                  std::uint64_t executions) {
            if (executions == 0) return;
            manifest.managed_build_tools.push_back({
                .role = std::move(role), .executable = std::move(executable),
                .family = std::move(family), .version = std::move(version),
                .executions = executions, .parameters = std::move(parameters)});
        };
        if (r.managed_build.system == sage::package::BuildSystem::Cargo) {
            add_tool("linker-driver", tools.cc, tools.compiler_family,
                     tools.compiler_version, managed_linker_parameters,
                     tool_audit->executions("linker-driver"));
            add_tool("linker", tools.linker, tools.linker_family,
                     tools.linker_version, managed_linker_parameters,
                     tool_audit->executions("linker"));
            add_tool("rustc", tools.rustc, tools.rustc_family,
                     tools.rustc_version, managed_rustc_parameters,
                     tool_audit->executions("rustc"));
        } else {
            add_tool("cc", tools.cc, tools.compiler_family, tools.compiler_version,
                     managed_cc_parameters, tool_audit->executions("cc"));
            add_tool("cxx", tools.cxx, tools.cxx_family, tools.cxx_version,
                     managed_cxx_parameters, tool_audit->executions("cxx"));
            const auto driver = tool_audit->executions("linker-driver");
            add_tool("linker-driver",
                     tool_audit->executions("linker-driver:cc")
                         ? tools.cc : tools.cxx,
                     tool_audit->executions("linker-driver:cc")
                         ? tools.compiler_family : tools.cxx_family,
                     tool_audit->executions("linker-driver:cc")
                         ? tools.compiler_version : tools.cxx_version,
                     managed_linker_parameters, driver);
            add_tool("linker", tools.linker, tools.linker_family,
                     tools.linker_version, managed_linker_parameters,
                     tool_audit->executions("linker"));
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

    // 4. Archive creation.  A v2 recipe may name several output views of the
    // same install tree.  Each view is copied before filtering, so one output
    // cannot delete a file another output owns; every archive receives its own
    // ELF-provided sonames and dependencies.
    struct OutputPath {
        std::string name;
        std::filesystem::path data;
    };
    std::vector<OutputPath> output_paths;
    if (r.schema_version == 2 && !r.managed_build.outputs.empty()) {
        for (const auto& output : r.managed_build.outputs) {
            const auto data = hermetic_root / ("output-" + output.name);
            std::error_code output_ec;
            std::filesystem::remove_all(data, output_ec);
            std::filesystem::copy(pkg_dir, data,
                std::filesystem::copy_options::recursive
                    | std::filesystem::copy_options::copy_symlinks, output_ec);
            if (output_ec) {
                sage::util::log_error("Cannot clone payload for output '{}': {}",
                                      output.name, output_ec.message());
                return 1;
            }
            auto payload = apply_payload_policy(data, output.install_files,
                                                 output.install_excludes);
            if (!payload) {
                sage::util::log_error("Invalid payload for output '{}': {}",
                                      output.name, payload.error());
                return 1;
            }
            output_paths.push_back({output.name, data});
        }
    } else {
        output_paths.push_back({r.name, pkg_dir});
    }

    for (const auto& output : output_paths) {
        auto output_manifest = manifest;
        output_manifest.name = output.name;
        if (output.name != r.name) {
            output_manifest.provides = r.provides;
            output_manifest.dependencies = r.host_deps;
            std::set<std::string> output_self_sonames;
            std::set<std::string> output_needed_sonames;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(output.data)) {
                if (entry.is_symlink()) {
                    const auto base = entry.path().filename().string();
                    if (base.starts_with("lib") && base.find(".so") != std::string::npos)
                        output_self_sonames.insert(base);
                    continue;
                }
                if (!entry.is_regular_file()) continue;
                const auto base = entry.path().filename().string();
                if (base.starts_with("lib") && base.find(".so") != std::string::npos)
                    output_self_sonames.insert(base);
                auto elf = sage::util::scan_elf(entry.path());
                if (!elf) continue;
                if (!elf->soname.empty()) output_self_sonames.insert(elf->soname);
                output_needed_sonames.insert(elf->needed.begin(), elf->needed.end());
            }
            for (const auto& soname : output_self_sonames)
                output_manifest.provides.push_back("so:" + soname);
            for (const auto& soname : output_needed_sonames)
                if (!output_self_sonames.contains(soname))
                    output_manifest.dependencies.push_back(
                        sage::package::Dependency::parse("so:" + soname));
            std::unordered_set<std::string> seen_provides;
            std::erase_if(output_manifest.provides, [&](const std::string& value) {
                return !seen_provides.insert(value).second;
            });
            std::unordered_set<std::string> seen_dependencies;
            std::erase_if(output_manifest.dependencies,
                [&](const sage::package::Dependency& value) {
                    return !seen_dependencies.insert(value.to_string()).second;
                });
        }
        std::string out_name = std::format("{}-{}-{}-{}.pkg.tar.zst",
            output_manifest.name, r.version.ver, r.version.rel, output_manifest.arch);
        std::filesystem::path out_path = recipe_dir / out_name;
        auto pack_res = sage::archive::create_package(output_manifest, output.data, out_path);
        if (!pack_res) {
            sage::util::log_error("Package '{}' packaging failed: {}",
                                  output.name, pack_res.error());
            return 1;
        }
        sage::util::log_success("Package built successfully: {}", out_path.string());
    }
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
