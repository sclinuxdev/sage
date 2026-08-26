module;
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

export module sage.cli.build;

// Package authoring: recipe builds and local repository indexing.
import std;
import sage;
import sage.vendor.libarchive;

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

    // Version probing is part of the reproducibility boundary.  A target
    // sysroot may not ship the caller's locale (for example C.UTF-8), and
    // shells can print a locale warning before the tool's real first line.
    // Force the portable C locale so that the version parser observes the
    // selected executable rather than an environment diagnostic.
    // Use shell-quoting even for administrator-provided executable names.
    // Double quotes would still expand `$()`/backticks from build.toml and
    // make version probing an unintended command-execution surface.
    const auto probe_command = "LC_ALL=C LANG=C "
        + sage::build::shell_quote(tool) + " --version > "
        + sage::build::shell_quote(out.string()) + " 2>&1";
    int rc = std::system(probe_command.c_str());
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
    const std::vector<std::string>& excludes,
    const std::vector<std::string>& optional_excludes = {})
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

    std::vector<bool> include_seen(includes.size(), false);
    std::vector<bool> exclude_seen(excludes.size(), false);
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
        bool excluded = false;
        for (std::size_t i = 0; i < excludes.size(); ++i) {
            if (sage::util::glob_match(excludes[i], entry.relative)) {
                excluded = true;
                exclude_seen[i] = true;
            }
        }
        if (excluded) selected = false;
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
    for (std::size_t i = 0; i < exclude_seen.size(); ++i) {
        if (!exclude_seen[i]) {
            const auto& pat = excludes[i];
            if (!std::ranges::contains(optional_excludes, pat)) {
                return std::unexpected(std::format(
                    "build.install_excludes pattern '{}' matched no installed payload",
                    pat));
            }
        }
    }
    return {};
}

std::expected<void, std::string> apply_file_permissions(
    const std::filesystem::path& pkg_dir,
    const std::vector<sage::package::FilePermission>& perms)
{
    std::error_code ec;
    for (const auto& fp : perms) {
        if (fp.path.empty()) continue;
        const auto target = pkg_dir / fp.path;
        if (!std::filesystem::exists(target, ec) && !std::filesystem::is_symlink(target, ec)) {
            return std::unexpected("File permission target does not exist: " + fp.path);
        }
        std::filesystem::permissions(target,
            static_cast<std::filesystem::perms>(fp.mode),
            std::filesystem::perm_options::replace, ec);
        if (ec) {
            return std::unexpected("Cannot set permissions for '" + fp.path + "': " + ec.message());
        }
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
    const auto path_inside = [&](const std::filesystem::path& root,
                                 const std::filesystem::path& relative) {
        if (!safe_relative(relative)) return false;
        std::error_code path_ec;
        const auto root_canonical = std::filesystem::weakly_canonical(root, path_ec);
        if (path_ec) return false;
        const auto parent_canonical = std::filesystem::weakly_canonical(
            (root / relative).parent_path(), path_ec);
        if (path_ec) return false;
        return parent_canonical == root_canonical
            || parent_canonical.string().starts_with(root_canonical.string() + "/");
    };
    const auto existing_inside = [&](const std::filesystem::path& root,
                                     const std::filesystem::path& relative) {
        if (!path_inside(root, relative)) return false;
        std::error_code path_ec;
        const auto status = std::filesystem::symlink_status(root / relative, path_ec);
        if (path_ec || status.type() != std::filesystem::file_type::symlink) return !path_ec;
        const auto root_canonical = std::filesystem::weakly_canonical(root, path_ec);
        const auto target_canonical = std::filesystem::weakly_canonical(
            root / relative, path_ec);
        return !path_ec && (target_canonical == root_canonical
            || target_canonical.string().starts_with(root_canonical.string() + "/"));
    };
    for (const auto& copy : copies) {
        if (!existing_inside(source_dir, copy.source)
            || !path_inside(pkg_dir, copy.destination))
            return std::unexpected("Install copy path follows a symlink outside its root");
        const auto source = source_dir / copy.source;
        const auto destination = pkg_dir / copy.destination;
        auto destination_status = std::filesystem::symlink_status(destination, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
            return std::unexpected("Cannot inspect install copy destination: " + ec.message());
        if (!ec && destination_status.type() == std::filesystem::file_type::symlink) {
            std::filesystem::remove(destination, ec);
            if (ec) return std::unexpected("Cannot replace install copy destination: "
                                           + ec.message());
        }
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
        if (!path_inside(pkg_dir, move.source) || !path_inside(pkg_dir, move.destination))
            return std::unexpected("Install move path follows a symlink outside the staging root");
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
        std::vector<std::filesystem::path> dirs;
        for (const auto& item : std::filesystem::recursive_directory_iterator(
                 pkg_dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (item.is_directory(ec) && !item.is_symlink(ec)) dirs.push_back(item.path());
        }
        std::ranges::sort(dirs, [](const auto& a, const auto& b) { return a.string().size() > b.string().size(); });
        for (const auto& dir : dirs) {
            if (std::filesystem::is_empty(dir, ec)) {
                std::filesystem::remove(dir, ec);
            }
        }
    }
    for (const auto& generate : generates) {
        if (!path_inside(pkg_dir, generate.path))
            return std::unexpected("Install generate path follows a symlink outside the staging root");
        const auto destination = pkg_dir / generate.path;
        auto destination_status = std::filesystem::symlink_status(destination, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
            return std::unexpected("Cannot inspect generated payload destination: " + ec.message());
        if (!ec && destination_status.type() == std::filesystem::file_type::symlink) {
            std::filesystem::remove(destination, ec);
            if (ec) return std::unexpected("Cannot replace generated payload symlink: "
                                           + ec.message());
        }
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
        if (!path_inside(pkg_dir, link.path))
            return std::unexpected("Install symlink path follows a symlink outside the staging root");
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

// Source archives are untrusted build inputs.  Use the same libarchive reader
// boundary as package archives instead of shelling out to tar, reject path and
// symlink escapes, and choose a top-level strip only when every member agrees
// on the same directory.  The second pass is intentional: archive readers are
// streaming, so no payload is retained in memory while we inspect the names.
std::expected<void, std::string> extract_source_archive(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination)
{
    struct Fd {
        int value{-1};
        explicit Fd(int fd) : value(fd) {}
        ~Fd() { if (value >= 0) ::close(value); }
        Fd(const Fd&) = delete;
        Fd& operator=(const Fd&) = delete;
        Fd(Fd&& other) noexcept : value(std::exchange(other.value, -1)) {}
        Fd& operator=(Fd&& other) noexcept {
            if (this != &other) {
                if (value >= 0) ::close(value);
                value = std::exchange(other.value, -1);
            }
            return *this;
        }
    };
    const auto open_reader = [&]() -> std::expected<
        std::pair<Fd, sage::vendor::libarchive::ArchiveReader>, std::string> {
        Fd fd{::open(archive_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
        if (fd.value < 0) return std::unexpected(
            "Cannot open source archive: " + archive_path.string());
        auto reader = sage::vendor::libarchive::ArchiveReader::open_fd(fd.value);
        if (!reader) return std::unexpected(reader.error());
        return std::pair<Fd, sage::vendor::libarchive::ArchiveReader>{
            std::move(fd), std::move(*reader)};
    };
    const auto clean_name = [](std::string_view raw)
        -> std::expected<std::filesystem::path, std::string> {
        if (raw.empty()) return std::unexpected("Source archive contains an empty path");
        const std::filesystem::path path{std::string(raw)};
        if (path.is_absolute() || path.has_root_path()
            || std::ranges::any_of(path, [](const auto& part) { return part == ".."; }))
            return std::unexpected("Source archive contains an unsafe path: "
                                   + std::string(raw));
        const auto normalized = path.lexically_normal();
        if (normalized.empty() || normalized == ".")
            return std::unexpected("Source archive contains an empty normalized path");
        return normalized;
    };
    const auto drain = [](sage::vendor::libarchive::ArchiveReader& reader)
        -> std::expected<void, std::string> {
        std::array<std::uint8_t, 64 * 1024> buffer{};
        for (;;) {
            auto count = reader.read_data(buffer);
            if (!count) return std::unexpected(count.error());
            if (*count == 0) break;
        }
        return {};
    };

    auto first = open_reader();
    if (!first) return std::unexpected(first.error());
    std::set<std::string> raw_names;
    std::string common_root;
    bool all_nested = true;
    bool saw_member = false;
    for (;;) {
        auto header = first->second.next_header();
        if (!header) return std::unexpected(header.error());
        if (!*header) break;
        auto name = clean_name((*header)->pathname);
        if (!name) return std::unexpected(name.error());
        const auto text = name->generic_string();
        if (!raw_names.insert(text).second)
            return std::unexpected("Source archive contains duplicate path: " + text);
        saw_member = true;
        const auto slash = text.find('/');
        // Tar writers normally include an explicit top-level directory entry
        // (`project-1.0/`) before its children.  That entry is not evidence
        // of a flat archive; only a root-level non-directory payload disables
        // the common-root strip.
        if (slash == std::string::npos
            && (*header)->filetype != sage::vendor::libarchive::type_directory)
            all_nested = false;
        else {
            const auto root = text.substr(0, slash);
            if (common_root.empty()) common_root = root;
            else if (common_root != root) all_nested = false;
        }
        if ((*header)->filetype == sage::vendor::libarchive::type_regular) {
            if (auto result = drain(first->second); !result)
                return std::unexpected(result.error());
        }
    }
    const bool strip_root = saw_member && all_nested && !common_root.empty();

    auto second = open_reader();
    if (!second) return std::unexpected(second.error());
    std::set<std::string> extracted;
    for (;;) {
        auto header = second->second.next_header();
        if (!header) return std::unexpected(header.error());
        if (!*header) break;
        auto name = clean_name((*header)->pathname);
        if (!name) return std::unexpected(name.error());
        auto relative = name->generic_string();
        if (strip_root) {
            const auto prefix = common_root + "/";
            if (relative == common_root) relative.clear();
            else if (relative.starts_with(prefix)) relative.erase(0, prefix.size());
            else return std::unexpected(
                "Source archive has inconsistent top-level paths");
        }
        if (relative.empty()) {
            if ((*header)->filetype == sage::vendor::libarchive::type_regular) {
                auto result = drain(second->second);
                if (!result) return std::unexpected(
                    "Cannot discard source archive root payload");
            }
            continue;
        }
        const std::filesystem::path out_rel(relative);
        if (out_rel.is_absolute() || out_rel.has_root_path()
            || std::ranges::any_of(out_rel,
                [](const auto& part) { return part == ".."; }))
            return std::unexpected("Source archive path escapes destination: " + relative);
        if (!extracted.insert(relative).second)
            return std::unexpected("Source archive contains duplicate extracted path: " + relative);
        const auto out = destination / out_rel;
        std::error_code ec;
        if ((*header)->filetype == sage::vendor::libarchive::type_directory) {
            std::filesystem::create_directories(out, ec);
            if (ec) return std::unexpected("Cannot create source directory '"
                                           + out.string() + "': " + ec.message());
            std::filesystem::permissions(out,
                static_cast<std::filesystem::perms>((*header)->perm),
                std::filesystem::perm_options::replace, ec);
            if (ec) return std::unexpected("Cannot set source directory mode: "
                                           + ec.message());
        } else if ((*header)->filetype == sage::vendor::libarchive::type_symlink) {
            const std::filesystem::path target((*header)->symlink);
            const auto resolved = out_rel.parent_path() / target;
            if (target.empty() || target.is_absolute() || target.has_root_path()
                || std::ranges::any_of(resolved.lexically_normal(),
                    [](const auto& part) { return part == ".."; }))
                return std::unexpected("Source archive symlink escapes destination: "
                                       + relative);
            std::filesystem::create_directories(out.parent_path(), ec);
            if (ec) return std::unexpected("Cannot create source symlink directory: "
                                           + ec.message());
            std::filesystem::create_symlink((*header)->symlink, out, ec);
            if (ec) return std::unexpected("Cannot create source symlink '"
                                           + out.string() + "': " + ec.message());
        } else if ((*header)->filetype == sage::vendor::libarchive::type_regular) {
            std::filesystem::create_directories(out.parent_path(), ec);
            if (ec) return std::unexpected("Cannot create source file directory: "
                                           + ec.message());
            std::ofstream file(out, std::ios::binary | std::ios::trunc);
            if (!file) return std::unexpected("Cannot create source file: " + out.string());
            std::array<std::uint8_t, 64 * 1024> buffer{};
            std::uint64_t written = 0;
            for (;;) {
                auto count = second->second.read_data(buffer);
                if (!count) return std::unexpected(count.error());
                if (*count == 0) break;
                file.write(reinterpret_cast<const char*>(buffer.data()),
                           static_cast<std::streamsize>(*count));
                if (!file) return std::unexpected("Cannot write source file: " + out.string());
                written += *count;
            }
            if (written != (*header)->size) return std::unexpected(
                "Source archive file size changed while extracting: " + relative);
            file.close();
            std::filesystem::permissions(out,
                static_cast<std::filesystem::perms>((*header)->perm),
                std::filesystem::perm_options::replace, ec);
            if (ec) return std::unexpected("Cannot set source file mode: " + ec.message());
        } else {
            return std::unexpected("Source archive contains unsupported special file: "
                                   + relative);
        }
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
        if ((!tools.cc.empty() && !cc) || (!tools.cxx.empty() && !cxx)
            || (!tools.linker.empty() && !linker)
            || (!tools.rustc.empty() && !rustc)) return std::nullopt;
        std::error_code sysroot_ec;
        const auto sysroot_root = std::filesystem::weakly_canonical(
            build_sysroot, sysroot_ec);
        if (build_sysroot.empty() || !build_sysroot.is_absolute()
            || sysroot_ec || !std::filesystem::is_directory(sysroot_root, sysroot_ec))
            return std::nullopt;
        if (build_sysroot != "/") {
            const auto inside = [&](const std::optional<std::filesystem::path>& path) {
                if (!path) return true;
                std::error_code path_ec;
                const auto canonical = std::filesystem::weakly_canonical(*path, path_ec);
                if (path_ec) return false;
                return canonical == sysroot_root
                    || canonical.string().starts_with(sysroot_root.string() + "/");
            };
            if (!inside(cc) || !inside(cxx) || !inside(linker) || !inside(rustc))
                return std::nullopt;
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
        if (!sandboxed) return std::nullopt;
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
            "compiler_cache must be ccache, sccache, auto, or none");
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
            "check_dependencies contains a dependency with an empty package name");
        requests.push_back(std::move(request));
    }
    sage::solver::DependencySolver solver(*installed, cfg.providers);
    auto resolved = solver.solve(requests);
    if (!resolved) return std::unexpected(
        "check_dependencies are not satisfied by the configured build sysroot: "
        + resolved.error());
    return {};
}


// Trace the complete process tree for every managed step.  PATH wrappers are
// still useful for role attribution, but they cannot prove that a child did
// not invoke an absolute path or a helper which clears LD_PRELOAD.  The
// ptrace/seccomp pair below observes both successful exec transitions and the
// execve/execveat syscall boundary for every descendant, including processes
// created by fakeroot and bubblewrap.
struct CgroupScope {
    std::filesystem::path cgroup_path;
    bool active{false};

    CgroupScope() = default;
    CgroupScope(const CgroupScope&) = delete;
    CgroupScope& operator=(const CgroupScope&) = delete;
    CgroupScope(CgroupScope&& other) noexcept
        : cgroup_path(std::move(other.cgroup_path)),
          active(std::exchange(other.active, false)) {}
    CgroupScope& operator=(CgroupScope&& other) noexcept {
        if (this != &other) {
            cleanup();
            cgroup_path = std::move(other.cgroup_path);
            active = std::exchange(other.active, false);
        }
        return *this;
    }

    static std::expected<std::optional<CgroupScope>, std::string> create(
        std::string_view mem_limit, std::uint64_t pids_limit, pid_t pid) {
        if (mem_limit.empty() && pids_limit == 0)
            return std::optional<CgroupScope>{};

        const std::filesystem::path base_cgroup = "/sys/fs/cgroup";
        std::ifstream controllers_file(base_cgroup / "cgroup.controllers");
        if (!controllers_file) return std::unexpected(
            "cgroups v2 is unavailable: cannot read cgroup.controllers");
        std::set<std::string> controllers;
        for (std::string controller; controllers_file >> controller;)
            controllers.insert(std::move(controller));
        if (!mem_limit.empty() && !controllers.contains("memory"))
            return std::unexpected(
                "configured memory_limit requires the cgroups v2 memory controller");
        if (pids_limit > 0 && !controllers.contains("pids"))
            return std::unexpected(
                "configured pids_limit requires the cgroups v2 pids controller");

        std::filesystem::path slice =
            base_cgroup / std::format("sage-build-{}", pid);
        std::error_code ec;
        std::filesystem::create_directories(slice, ec);
        if (ec) return std::unexpected(std::format(
            "cannot create build cgroup '{}': {}", slice.string(), ec.message()));

        CgroupScope scope;
        scope.cgroup_path = slice;
        scope.active = true;
        const auto write_value = [&](std::string_view name,
                                     std::string_view value)
            -> std::expected<void, std::string> {
            std::ofstream file(slice / std::string(name));
            if (!file) return std::unexpected(std::format(
                "cannot open build cgroup control '{}'", (slice / std::string(name)).string()));
            file << value << '\n';
            if (!file) return std::unexpected(std::format(
                "cannot write build cgroup control '{}'", (slice / std::string(name)).string()));
            return {};
        };
        if (!mem_limit.empty()) {
            if (auto result = write_value("memory.max", mem_limit); !result)
                return std::unexpected(result.error());
        }
        if (pids_limit > 0) {
            if (auto result = write_value("pids.max", std::to_string(pids_limit));
                !result)
                return std::unexpected(result.error());
        }
        {
            std::ofstream procs_file(slice / "cgroup.procs");
            if (!procs_file) return std::unexpected(
                "cannot open build cgroup cgroup.procs");
            procs_file << pid << '\n';
            if (!procs_file) return std::unexpected(
                "cannot move build audit supervisor into its cgroup");
        }
        return std::optional<CgroupScope>(std::move(scope));
    }

    ~CgroupScope() { cleanup(); }

private:
    void cleanup() noexcept {
        if (!active || cgroup_path.empty()) return;
        std::ifstream procs(cgroup_path / "cgroup.procs");
        pid_t p;
        while (procs >> p) {
            if (p > 1) (void)::kill(p, SIGKILL);
        }
        std::error_code ec;
        std::filesystem::remove(cgroup_path, ec);
        active = false;
    }
};

struct ProcessExecAudit {
    static bool install_seccomp() noexcept {
        const sock_filter filter[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                     static_cast<std::uint32_t>(offsetof(struct seccomp_data, nr))),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                     static_cast<std::uint32_t>(__NR_execve), 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRACE),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                     static_cast<std::uint32_t>(__NR_execveat), 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRACE),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
        const sock_fprog program{
            .len = static_cast<unsigned short>(std::size(filter)),
            .filter = const_cast<sock_filter*>(filter),
        };
        return ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0
            && ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
    }

    static std::string process_cmdline(pid_t pid) {
        std::ifstream in(std::format("/proc/{}/cmdline", pid), std::ios::binary);
        std::string value((std::istreambuf_iterator<char>(in)), {});
        for (auto& c : value) if (c == '\0') c = ' ';
        while (!value.empty() && value.back() == ' ') value.pop_back();
        return value;
    }

    static std::string process_executable(pid_t pid) {
        std::error_code ec;
        auto path = std::filesystem::read_symlink(
            std::filesystem::path("/proc") / std::to_string(pid) / "exe", ec);
        return ec ? std::string{"<unavailable>"} : path.string();
    }

    static std::expected<int, std::string> run(std::string_view command,
                                                const std::filesystem::path& log,
                                                std::string_view memory_limit = {},
                                                std::uint64_t pids_limit = 0) {
        const auto command_copy = std::string(command);
        const pid_t child = ::fork();
        if (child < 0) return std::unexpected(
            std::format("cannot fork build audit supervisor: {}", std::strerror(errno)));
        if (child == 0) {
            if (pids_limit > 0) {
                if (pids_limit > static_cast<std::uint64_t>(
                        std::numeric_limits<rlim_t>::max()))
                    _exit(125);
                struct rlimit rl{static_cast<rlim_t>(pids_limit),
                                 static_cast<rlim_t>(pids_limit)};
                if (::setrlimit(RLIMIT_NPROC, &rl) != 0) _exit(125);
            }
            if (::ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) != 0
                || !install_seccomp()) _exit(125);
            ::raise(SIGSTOP);
            ::execl("/bin/sh", "/bin/sh", "-c", command_copy.c_str(), nullptr);
            _exit(127);
        }
        auto cgroup_res = CgroupScope::create(memory_limit, pids_limit, child);
        if (!cgroup_res) {
            ::kill(child, SIGKILL);
            int cleanup_status = 0;
            (void)::waitpid(child, &cleanup_status, 0);
            return std::unexpected(
                "cannot apply configured build resource limits: "
                + cgroup_res.error());
        }
        auto cgroup_scope = std::move(*cgroup_res);
        std::ofstream audit_log(log, std::ios::out | std::ios::app);
        if (!audit_log) {
            ::kill(child, SIGKILL);
            return std::unexpected("cannot open process exec audit log: " + log.string());
        }
        int status = 0;
        if (::waitpid(child, &status, 0) < 0 || !WIFSTOPPED(status)) {
            ::kill(child, SIGKILL);
            return std::unexpected("build audit child did not enter tracing stop");
        }
        constexpr long trace_options = PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK
            | PTRACE_O_TRACEVFORK | PTRACE_O_TRACEEXEC | PTRACE_O_TRACESECCOMP
            | PTRACE_O_EXITKILL;
        if (::ptrace(PTRACE_SETOPTIONS, child, nullptr,
                     reinterpret_cast<void*>(trace_options)) != 0) {
            ::kill(child, SIGKILL);
            return std::unexpected(std::format(
                "cannot enable process exec tracing: {}", std::strerror(errno)));
        }
        if (::ptrace(PTRACE_CONT, child, nullptr, nullptr) != 0) {
            ::kill(child, SIGKILL);
            return std::unexpected("cannot continue build audit child");
        }

        std::set<pid_t> tracees{child};
        bool root_done = false;
        int root_status = 125 << 8;
        for (;;) {
            const pid_t pid = ::waitpid(-1, &status, __WALL);
            if (pid < 0) {
                if (errno == ECHILD) break;
                if (errno == EINTR) continue;
                return std::unexpected(std::format(
                    "process exec audit wait failed: {}", std::strerror(errno)));
            }
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                tracees.erase(pid);
                if (pid == child) {
                    root_done = true;
                    root_status = status;
                    // fakeroot starts a helper daemon which can outlive the
                    // shell in a PID namespace.  It is still a traced
                    // descendant, so wait for it forever would make a
                    // successful phase appear hung.  Terminate only the
                    // descendants of this audit root, then reap them below.
                    for (const auto descendant : tracees)
                        ::kill(descendant, SIGKILL);
                }
                continue;
            }
            if (!WIFSTOPPED(status)) continue;
            const auto event = static_cast<unsigned>(status) >> 16;
            if (event == PTRACE_EVENT_SECCOMP) {
                audit_log << "execve-boundary pid=" << pid << " syscall=execve/execveat\n";
            } else if (event == PTRACE_EVENT_EXEC) {
                audit_log << "execve pid=" << pid << " path="
                          << process_executable(pid) << " argv="
                          << process_cmdline(pid) << "\n";
            }
            // A ptrace fork/clone event stops both the event parent and the
            // newly-created child.  The child is not continued implicitly;
            // leaving it stopped deadlocks fakeroot/build-system helpers and
            // makes a complete process-tree audit unusable for real builds.
            if (event == PTRACE_EVENT_CLONE || event == PTRACE_EVENT_FORK
                || event == PTRACE_EVENT_VFORK) {
                unsigned long child_word = 0;
                if (::ptrace(PTRACE_GETEVENTMSG, pid, nullptr, &child_word) == 0
                    && child_word != 0) {
                    const auto descendant = static_cast<pid_t>(child_word);
                    tracees.insert(descendant);
                    ::ptrace(PTRACE_SETOPTIONS, descendant, nullptr,
                             reinterpret_cast<void*>(trace_options));
                    ::ptrace(PTRACE_CONT, descendant, nullptr, nullptr);
                }
            }
            // Options are inherited by traced descendants on Linux, but
            // setting them again is harmless and covers kernels that only
            // copy the tracing relationship at clone time.
            ::ptrace(PTRACE_SETOPTIONS, pid, nullptr,
                     reinterpret_cast<void*>(trace_options));
            ::ptrace(PTRACE_CONT, pid, nullptr, nullptr);
        }
        if (!root_done) return std::unexpected("build audit supervisor lost its root child");
        if (WIFEXITED(root_status)) return WEXITSTATUS(root_status);
        return 128 + WTERMSIG(root_status);
    }
};

bool compiler_like_executable(std::string_view path) {
    const auto base = std::filesystem::path(path).filename().string();
    if (base == "cc1" || base == "cc1plus" || base == "collect2"
        || base == "as" || base == "ar" || base == "ranlib"
        || base == "gcc-ar" || base == "gcc-nm" || base == "gcc-ranlib"
        || base == "llvm-ar" || base == "llvm-nm" || base == "llvm-ranlib"
        // clang-scan-deps is an analysis helper launched by the selected
        // clang driver, not an alternative compiler/linker.  It must be
        // visible to module dependency generation without being mistaken for
        // an unmanaged compiler implementation.
        || base == "clang-scan-deps"
        // Every dynamically linked executable enters through the system ELF
        // loader; it is not a linker invocation and is outside Sage's tool
        // selection boundary.
        || base.starts_with("ld-linux")) return false;
    for (const auto prefix : {std::string_view{"cc"}, std::string_view{"c++"},
                              std::string_view{"gcc"}, std::string_view{"g++"},
                              std::string_view{"clang"}, std::string_view{"clang++"},
                              std::string_view{"ld"}, std::string_view{"lld"},
                              std::string_view{"mold"}, std::string_view{"rustc"}}) {
        if (base.starts_with(prefix)
            && (base.size() == prefix.size() || base[prefix.size()] == '-'
                || base[prefix.size()] == '.'
                || std::isdigit(static_cast<unsigned char>(base[prefix.size()]))))
            return true;
    }
    return false;
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
    const auto requested_cache = r.schema_version == 2
        && r.managed_build.system != sage::package::BuildSystem::Script
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
            cc_cache_for_build, cxx_cache_for_build, cache_for_build,
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
                 .target_triplet = target_triplet,
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
    const auto cache_dir = active_cache_mode == "none"
        ? std::filesystem::path{} : bcfg.ccache_dir;
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
            .target_triplet = target_triplet,
            .compiler_cache_mode = active_cache_mode,
            .compiler_version = selected.compiler_version,
            .cxx_version = selected.cxx_version,
            .linker_version = selected.linker_version,
            .rustc_version = selected.rustc_version,
            .compiler_family = selected.compiler_family,
            .cxx_family = selected.cxx_family,
            .linker_family = selected.linker_family,
            .rustc_family = selected.rustc_family};
        tool_audit = ToolAudit::create(
            canonical, hermetic_root, bcfg.sysroot, cache_dir, active_cache_mode);
        if (!tool_audit) {
            sage::util::log_error(
                "Cannot create the v2 tool audit fence; refusing to build without actual execution evidence");
            return 1;
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
                 .rustc = cand.rustc,
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
            // When Sage itself is entered through a host-side chroot, the
            // ptrace observer can report the host mount prefix while the
            // child sees the sysroot as `/`. Accept exactly that configured
            // prefix rewrite; a generic suffix match would let
            // `/untrusted/usr/bin/gcc` masquerade as the selected compiler.
            if (bcfg.sysroot == "/") return false;
            const auto host_path = (bcfg.sysroot
                / std::filesystem::path(expected).relative_path()).lexically_normal();
            return actual == host_path.string();
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
    manifest.conflicts = r.conflicts;
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

        std::string exec_audit_digest;
        if (!manifest.managed_build_commands.empty()) {
            sage::util::Sha256 hasher;
            for (const auto& cmd : manifest.managed_build_commands) {
                hasher.update(cmd.data(), cmd.size());
                hasher.update("\n", 1);
            }
            exec_audit_digest = "sha256:" + hasher.finalize();
        }

        sage::package::BuildAttestation attestation;
        attestation.schema_version = 2;
        const auto epoch_tp = std::chrono::sys_seconds(
            std::chrono::seconds(bcfg.source_date_epoch));
        attestation.built_at = std::format("{:%FT%TZ}", epoch_tp);
        attestation.builder = "sage " SAGE_VERSION;
        attestation.host_arch = host_arch;
        attestation.target_arch = target_arch == "any" ? host_arch : target_arch;
        attestation.host_triplet = host_triplet;
        attestation.target_triplet = target_triplet;
        attestation.check_dependencies = r.check_deps;
        attestation.exec_audit_digest = exec_audit_digest;
        attestation.package = {
            .name = r.name,
            .version = r.version.ver,
            .release = r.version.rel,
            .channel = r.channel,
            .arch = r.arch,
            .sha256 = ""
        };
        if (!r.source_url.empty()) {
            attestation.sources.push_back({
                .url = r.source_url,
                .sha256 = r.source_sha256
            });
        }
        for (const auto& extra : r.extra_sources) {
            attestation.sources.push_back({
                .url = extra.url,
                .sha256 = extra.sha256
            });
        }
        if (tool_audit) {
            for (auto command : tool_audit->commands()) {
                attestation.audit_commands.push_back(normalize_audit_text(std::move(command)));
            }
            for (const auto& tool_entry : manifest.managed_build_tools) {
                sage::package::BuildAttestationTool atool;
                atool.role = tool_entry.role;
                atool.executable = tool_entry.executable;
                atool.family = tool_entry.family;
                atool.version = tool_entry.version;
                atool.executions = tool_entry.executions;
                atool.parameters = tool_entry.parameters;
                if (auto resolved = ToolAudit::resolve(tool_entry.executable)) {
                    atool.path = resolved->string();
                    if (auto h = sage::util::compute_file_sha256(*resolved)) {
                        atool.sha256 = *h;
                    }
                    struct stat st{};
                    if (::stat(resolved->c_str(), &st) == 0) {
                        atool.inode = static_cast<std::uint64_t>(st.st_ino);
                    }
                }
                attestation.tools.push_back(std::move(atool));
            }
        }
        if (bcfg.sysroot != "/" || std::filesystem::exists(cfg_res->db_path)) {
            auto sysroot_db_path = (bcfg.sysroot != "/")
                ? bcfg.sysroot / "var/lib/sage/packages.lmdb"
                : cfg_res->db_path;
            if (auto sdb = sage::db::Database::open(sysroot_db_path, true)) {
                if (auto pkgs = sdb->list_installed_packages()) {
                    for (const auto& pkg : *pkgs) {
                        attestation.sysroot_packages.push_back({
                            .name = pkg.name,
                            .version = pkg.version.ver,
                            .release = pkg.version.rel,
                            .sha256 = pkg.file
                        });
                    }
                }
            }
        }
        manifest.attestation_toml = attestation.serialize_toml();
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
    std::map<std::string, std::set<std::string>> self_verdefs;
    std::map<std::string, std::set<std::string>> needed_verneeds;
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
            for (const auto& rpath : elf_res->rpaths) {
                if (rpath.contains(hermetic_root.string())
                    || rpath.starts_with("/tmp/")
                    || rpath == "/tmp"
                    || rpath.contains(recipe_dir.string())
                    || rpath.contains(src_dir.string())) {
                    sage::util::log_error("ELF binary '{}' contains insecure RPATH: '{}'", rel, rpath);
                    return 1;
                }
            }
            for (const auto& runpath : elf_res->runpaths) {
                if (runpath.contains(hermetic_root.string())
                    || runpath.starts_with("/tmp/")
                    || runpath == "/tmp"
                    || runpath.contains(recipe_dir.string())
                    || runpath.contains(src_dir.string())) {
                    sage::util::log_error("ELF binary '{}' contains insecure RUNPATH: '{}'", rel, runpath);
                    return 1;
                }
            }
            if (!elf_res->soname.empty()) {
                self_sonames.insert(elf_res->soname);
                for (const auto& v : elf_res->verdef_versions) {
                    self_verdefs[elf_res->soname].insert(v);
                }
            }
            for (const auto& needed : elf_res->needed) {
                needed_sonames.insert(needed);
                needed_by[needed].push_back(rel);
            }
            for (const auto& [fname, vname] : elf_res->verneed_entries) {
                needed_verneeds[fname].insert(vname);
            }
        }
    }

    for (const auto& soname : self_sonames) {
        manifest.provides.push_back("so:" + soname);
        if (auto it = self_verdefs.find(soname); it != self_verdefs.end()) {
            for (const auto& v : it->second) {
                manifest.provides.push_back(std::format("so:{}({})", soname, v));
            }
        }
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
        if (auto it = needed_verneeds.find(soname); it != needed_verneeds.end()) {
            for (const auto& v : it->second) {
                manifest.dependencies.push_back(
                    sage::package::Dependency::parse(std::format("so:{}({})", soname, v)));
            }
        }
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
                                                 output.install_excludes,
                                                 output.optional_excludes);
            if (!payload) {
                sage::util::log_error("Invalid payload for output '{}': {}",
                                      output.name, payload.error());
                return 1;
            }
            const auto transform_source = src_dir / r.managed_build.source_subdir;
            auto output_transforms = apply_install_transforms(
                transform_source, data,
                output.install_copies,
                output.install_symlinks,
                output.install_moves,
                output.install_removes,
                output.install_generates);
            if (!output_transforms) {
                sage::util::log_error("Invalid install transform for output '{}': {}",
                                      output.name, output_transforms.error());
                return 1;
            }
            auto output_perms = apply_file_permissions(data, output.file_permissions);
            if (!output_perms) {
                sage::util::log_error("Invalid file permissions for output '{}': {}",
                                      output.name, output_perms.error());
                return 1;
            }
            output_paths.push_back({output.name, data});
        }
    } else {
        output_paths.push_back({r.name, pkg_dir});
    }

    // Exhaustiveness check in outputs mode
    if (r.schema_version == 2 && (r.managed_build.payload == sage::package::PayloadMode::Outputs || !r.managed_build.outputs.empty())) {
        std::error_code pkg_ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 pkg_dir, std::filesystem::directory_options::skip_permission_denied, pkg_ec)) {
            if (pkg_ec) {
                sage::util::log_error("Cannot inspect staged payload: {}", pkg_ec.message());
                return 1;
            }
            if (entry.is_directory(pkg_ec) && !entry.is_symlink(pkg_ec)) continue;
            const auto rel = entry.path().lexically_relative(pkg_dir).generic_string();
            bool claimed = false;
            for (size_t out_idx = 0; out_idx < output_paths.size(); ++out_idx) {
                const auto& out = output_paths[out_idx];
                const auto out_file = out.data / rel;
                std::error_code check_ec;
                if (std::filesystem::exists(out_file, check_ec) || std::filesystem::is_symlink(out_file, check_ec)) {
                    claimed = true;
                    break;
                }
                if (out_idx < r.managed_build.outputs.size()) {
                    const auto& out_spec = r.managed_build.outputs[out_idx];
                    for (const auto& move : out_spec.install_moves) {
                        if (move.source == rel || std::filesystem::path(move.source) == std::filesystem::path(rel)) {
                            claimed = true;
                            break;
                        }
                    }
                    if (claimed) break;
                    for (const auto& rm_pat : out_spec.install_removes) {
                        if (sage::util::glob_match(rm_pat.path, rel)) {
                            claimed = true;
                            break;
                        }
                    }
                    if (claimed) break;
                }
            }
            if (!claimed) {
                sage::util::log_error("Unassigned payload path '{}' was not claimed by any output", rel);
                return 1;
            }
        }
    }

    if (output_paths.size() > 1) {
        std::map<std::string, std::string> owners;
        for (const auto& output : output_paths) {
            std::error_code output_ec;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(
                     output.data, std::filesystem::directory_options::skip_permission_denied,
                     output_ec)) {
                if (output_ec) {
                    sage::util::log_error("Cannot inspect output '{}': {}",
                                          output.name, output_ec.message());
                    return 1;
                }
                if (entry.is_directory(output_ec) && !entry.is_symlink(output_ec)) continue;
                const auto rel = entry.path().lexically_relative(output.data).generic_string();
                const auto [it, inserted] = owners.emplace(rel, output.name);
                if (!inserted) {
                    sage::util::log_error(
                        "Outputs '{}' and '{}' both claim payload path '{}'",
                        it->second, output.name, rel);
                    return 1;
                }
            }
        }
    }

    std::vector<std::pair<std::string, std::filesystem::path>> created_packages;

    for (const auto& output : output_paths) {
        auto output_manifest = manifest;
        output_manifest.name = output.name;
        if (!r.managed_build.outputs.empty()) {
            const auto output_spec = std::ranges::find(r.managed_build.outputs,
                output.name, &sage::package::InstallOutput::name);
            if (output_spec == r.managed_build.outputs.end()) {
                sage::util::log_error("Output '{}' has no recipe metadata", output.name);
                return 1;
            }
            output_manifest.description = output_spec->description.value_or(r.description);
            output_manifest.license = output_spec->license.value_or(r.license);
            if (output_spec->version) {
                output_manifest.version = sage::package::Version::parse(*output_spec->version);
            } else {
                output_manifest.version = r.version;
            }
            if (output_spec->release) {
                output_manifest.version.rel = *output_spec->release;
            }
            if (output_spec->channel) {
                output_manifest.channel = *output_spec->channel;
            } else {
                output_manifest.channel = r.channel;
            }
            if (output_spec->arch) {
                output_manifest.arch = *output_spec->arch;
            } else {
                output_manifest.arch = r.arch;
            }
            const auto inherits = [&](std::string_view key) {
                return std::ranges::contains(output_spec->inherit, key);
            };
            if (output_spec->dependencies) {
                output_manifest.dependencies = *output_spec->dependencies;
            } else if (inherits("dependencies")) {
                output_manifest.dependencies = r.host_deps;
            } else {
                output_manifest.dependencies.clear();
            }

            if (output_spec->provides) {
                output_manifest.provides = *output_spec->provides;
            } else if (inherits("provides")) {
                output_manifest.provides = r.provides;
            } else {
                output_manifest.provides.clear();
            }

            if (output_spec->conflicts) {
                output_manifest.conflicts = *output_spec->conflicts;
            } else if (inherits("conflicts")) {
                output_manifest.conflicts = r.conflicts;
            } else {
                output_manifest.conflicts.clear();
            }

            if (output_spec->conffiles) {
                output_manifest.conffiles = *output_spec->conffiles;
            } else if (inherits("conffiles")) {
                output_manifest.conffiles = r.conffiles;
            } else {
                output_manifest.conffiles.clear();
            }
            output_manifest.files.clear();
            const auto& perms_list = !output_spec->file_permissions.empty()
                ? output_spec->file_permissions
                : r.managed_build.file_permissions;
            for (const auto& perm : perms_list) {
                sage::package::FileEntry fe;
                fe.path = perm.path;
                fe.mode = perm.mode;
                fe.uid = perm.uid;
                fe.gid = perm.gid;
                fe.caps = perm.caps;
                output_manifest.files.push_back(std::move(fe));
            }

            if (!manifest.attestation_toml.empty()) {
                auto parsed_att = sage::package::BuildAttestation::parse_toml(manifest.attestation_toml);
                if (parsed_att) {
                    parsed_att->package.name = output_manifest.name;
                    parsed_att->package.version = output_manifest.version.ver;
                    parsed_att->package.release = output_manifest.version.rel;
                    parsed_att->package.channel = output_manifest.channel;
                    parsed_att->package.arch = output_manifest.arch;
                    output_manifest.attestation_toml = parsed_att->serialize_toml();
                }
            }

            std::set<std::string> output_self_sonames;
            std::set<std::string> output_needed_sonames;
            std::map<std::string, std::set<std::string>> out_self_verdefs;
            std::map<std::string, std::set<std::string>> out_needed_verneeds;
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
                for (const auto& rpath : elf->rpaths) {
                    if (rpath.contains(hermetic_root.string())
                        || rpath.starts_with("/tmp/")
                        || rpath == "/tmp"
                        || rpath.contains(recipe_dir.string())
                        || rpath.contains(src_dir.string())) {
                        sage::util::log_error("ELF binary in output '{}' contains insecure RPATH: '{}'",
                                              output.name, rpath);
                        return 1;
                    }
                }
                for (const auto& runpath : elf->runpaths) {
                    if (runpath.contains(hermetic_root.string())
                        || runpath.starts_with("/tmp/")
                        || runpath == "/tmp"
                        || runpath.contains(recipe_dir.string())
                        || runpath.contains(src_dir.string())) {
                        sage::util::log_error("ELF binary in output '{}' contains insecure RUNPATH: '{}'",
                                              output.name, runpath);
                        return 1;
                    }
                }
                if (!elf->soname.empty()) {
                    output_self_sonames.insert(elf->soname);
                    for (const auto& v : elf->verdef_versions) {
                        out_self_verdefs[elf->soname].insert(v);
                    }
                }
                output_needed_sonames.insert(elf->needed.begin(), elf->needed.end());
                for (const auto& [fname, vname] : elf->verneed_entries) {
                    out_needed_verneeds[fname].insert(vname);
                }
            }
            for (const auto& soname : output_self_sonames) {
                output_manifest.provides.push_back("so:" + soname);
                if (auto it = out_self_verdefs.find(soname); it != out_self_verdefs.end()) {
                    for (const auto& v : it->second) {
                        output_manifest.provides.push_back(std::format("so:{}({})", soname, v));
                    }
                }
            }
            for (const auto& soname : output_needed_sonames) {
                if (!output_self_sonames.contains(soname)) {
                    output_manifest.dependencies.push_back(
                        sage::package::Dependency::parse("so:" + soname));
                    if (auto it = out_needed_verneeds.find(soname); it != out_needed_verneeds.end()) {
                        for (const auto& v : it->second) {
                            output_manifest.dependencies.push_back(
                                sage::package::Dependency::parse(std::format("so:{}({})", soname, v)));
                        }
                    }
                }
            }
            std::unordered_set<std::string> seen_provides;
            std::erase_if(output_manifest.provides, [&](const std::string& value) {
                return !seen_provides.insert(value).second;
            });
            std::unordered_set<std::string> seen_dependencies;
            std::erase_if(output_manifest.dependencies,
                [&](const sage::package::Dependency& value) {
                    return !seen_dependencies.insert(value.to_string()).second;
                });
        } else {
            output_manifest.files.clear();
            for (const auto& perm : r.managed_build.file_permissions) {
                sage::package::FileEntry fe;
                fe.path = perm.path;
                fe.mode = perm.mode;
                fe.uid = perm.uid;
                fe.gid = perm.gid;
                fe.caps = perm.caps;
                output_manifest.files.push_back(std::move(fe));
            }
        }
        std::string out_name = std::format("{}-{}-{}-{}.pkg.tar.zst",
            output_manifest.name, output_manifest.version.ver, output_manifest.version.rel, output_manifest.arch);
        std::filesystem::path out_path = recipe_dir / out_name;
        auto pack_res = sage::archive::create_package(output_manifest, output.data, out_path);
        if (!pack_res) {
            sage::util::log_error("Package '{}' packaging failed: {}",
                                  output.name, pack_res.error());
            return 1;
        }
        sage::util::log_success("Package built successfully: {}", out_path.string());
        created_packages.push_back({output.name, out_path});
    }

    if (opts.check_reproducible) {
        sage::util::log_info("Running second build pass to verify reproducibility (--check-reproducible)...");
        const auto repro_root = std::filesystem::temp_directory_path()
            / std::format("sage-repro-{}", sage::util::current_pid());
        std::error_code repro_ec;
        std::filesystem::create_directories(repro_root, repro_ec);
        struct ReproCleanup {
            std::filesystem::path root;
            ~ReproCleanup() {
                std::error_code ec;
                std::filesystem::remove_all(root, ec);
            }
        } repro_cleanup{repro_root};

        std::filesystem::path repro_home = repro_root / "home";
        std::filesystem::path repro_temp = repro_root / "tmp";
        std::filesystem::path repro_src = repro_root / "src";
        std::filesystem::path repro_pkg = repro_root / "pkg";
        std::filesystem::create_directories(repro_home, repro_ec);
        std::filesystem::create_directories(repro_temp, repro_ec);
        std::filesystem::create_directories(repro_pkg, repro_ec);
        std::filesystem::create_directories(repro_src / "distfiles", repro_ec);

        if (!archive_path.empty()) {
            auto extracted = extract_source_archive(archive_path, repro_src);
            if (!extracted) {
                sage::util::log_error("Reproducibility pass failed to unpack source: {}", extracted.error());
                return 1;
            }
        }
        for (const auto& path : extra_paths) {
            std::filesystem::copy_file(path, repro_src / "distfiles" / path.filename(),
                std::filesystem::copy_options::overwrite_existing, repro_ec);
        }
        if (r.schema_version == 2 && r.source_url.empty()) {
            for (const auto& entry : std::filesystem::directory_iterator(recipe_dir)) {
                const auto name = entry.path().filename().string();
                if (name == "recipe.toml" || name == "distfiles"
                    || name == "pkg" || name == ".sage-source"
                    || name.ends_with(".pkg.tar.zst")) continue;
                std::filesystem::copy(entry.path(), repro_src / entry.path().filename(),
                    std::filesystem::copy_options::recursive
                    | std::filesystem::copy_options::overwrite_existing, repro_ec);
            }
        }
        if (r.schema_version == 2) {
            for (const auto& patch : r.managed_build.patches_spec) {
                const auto beside_recipe = recipe_dir / patch.file;
                const auto attached = std::filesystem::is_regular_file(beside_recipe, repro_ec)
                    ? beside_recipe : recipe_dir / "distfiles" / patch.file;
                if (!std::filesystem::is_regular_file(attached, repro_ec)) continue;
                std::filesystem::copy_file(attached, repro_src / "distfiles" / patch.file,
                    std::filesystem::copy_options::overwrite_existing, repro_ec);
            }
        }
        std::filesystem::path repro_work =
            (r.schema_version == 2 || !r.source_url.empty()) ? repro_src : recipe_dir;

        std::optional<ToolAudit> repro_tool_audit;
        if (r.schema_version == 2 && !candidates.empty()) {
            auto& selected = candidates.front();
            auto canonical = sage::build::Toolchain{
                .cc = selected.cc, .cxx = selected.cxx, .linker = selected.linker,
                .rustc = selected.rustc,
                .target_triplet = target_triplet,
                .compiler_cache_mode = active_cache_mode,
                .compiler_version = selected.compiler_version,
                .cxx_version = selected.cxx_version,
                .linker_version = selected.linker_version,
                .rustc_version = selected.rustc_version,
                .compiler_family = selected.compiler_family,
                .cxx_family = selected.cxx_family,
                .linker_family = selected.linker_family,
                .rustc_family = selected.rustc_family};
            repro_tool_audit = ToolAudit::create(
                canonical, repro_root, bcfg.sysroot, cache_dir, active_cache_mode);
            if (repro_tool_audit && !repro_tool_audit->sandbox.empty()) {
                repro_tool_audit->sandbox += " --ro-bind "
                    + sage::build::shell_quote(recipe_dir.string()) + " "
                    + sage::build::shell_quote(recipe_dir.string())
                    + " --bind " + sage::build::shell_quote(repro_src.string()) + " "
                    + sage::build::shell_quote(repro_src.string())
                    + " --bind " + sage::build::shell_quote(repro_pkg.string()) + " "
                    + sage::build::shell_quote(repro_pkg.string())
                    + " --bind " + sage::build::shell_quote(repro_home.string()) + " "
                    + sage::build::shell_quote(repro_home.string())
                    + " --bind " + sage::build::shell_quote(repro_temp.string()) + " "
                    + sage::build::shell_quote(repro_temp.string());
                if (auto fakeroot_path = ToolAudit::resolve(bcfg.fakeroot)) {
                    std::error_code sysroot_ec;
                    const auto sysroot_root = std::filesystem::weakly_canonical(
                        bcfg.sysroot, sysroot_ec);
                    const auto relative_root = sysroot_ec ? bcfg.sysroot : sysroot_root;
                    const auto namespace_fakeroot = (std::filesystem::path("/")
                        / fakeroot_path->lexically_relative(relative_root)).lexically_normal();
                    const auto namespace_parent = namespace_fakeroot.parent_path();
                    const bool hidden_tmp = namespace_parent.string().starts_with("/tmp/");
                    repro_tool_audit->sandbox += (hidden_tmp
                        ? " --dir " + sage::build::shell_quote(namespace_parent.string()) : "")
                        + " --ro-bind " + sage::build::shell_quote(fakeroot_path->string())
                        + " " + sage::build::shell_quote(namespace_fakeroot.string());
                }
            }
        }

        const Toolchain& cand = candidates.front();
        if (r.schema_version == 2) {
            auto repro_plan = sage::build::plan_v2(
                r, bcfg, {.source = repro_work, .package = repro_pkg,
                           .home = repro_home, .temp = repro_temp},
                {.cc = cand.cc, .cxx = cand.cxx, .linker = cand.linker,
                 .rustc = cand.rustc,
                 .target_triplet = target_triplet,
                 .cc_for_build = repro_tool_audit ? repro_tool_audit->cc : cand.cc,
                 .cxx_for_build = repro_tool_audit ? repro_tool_audit->cxx : cand.cxx,
                 .linker_for_build = repro_tool_audit ? repro_tool_audit->linker : cand.linker,
                 .rustc_for_build = repro_tool_audit ? repro_tool_audit->rustc : cand.rustc,
                 .cc_cache_for_build = repro_tool_audit
                     ? repro_tool_audit->cc_cache : cand.cc_cache_for_build,
                 .cxx_cache_for_build = repro_tool_audit
                     ? repro_tool_audit->cxx_cache : cand.cxx_cache_for_build,
                 .cache_for_build = repro_tool_audit
                     ? repro_tool_audit->cache_for_build : cand.cache_for_build,
                 .compiler_cache_mode = active_cache_mode,
                 .path_for_build = repro_tool_audit ? repro_tool_audit->path : cand.path_for_build,
                 .compiler_version = cand.compiler_version,
                 .cxx_version = cand.cxx_version,
                 .linker_version = cand.linker_version,
                 .rustc_version = cand.rustc_version,
                 .compiler_family = cand.compiler_family,
                 .cxx_family = cand.cxx_family,
                 .linker_family = cand.linker_family,
                 .rustc_family = cand.rustc_family}, compile_jobs);
            if (!repro_plan) {
                sage::util::log_error("Reproducibility pass failed to plan: {}", repro_plan.error());
                return 1;
            }
            repro_plan->environment["RECIPE_DIR"] = recipe_dir.string();
            repro_plan->environment["SRCDIR"] = repro_work.string();
            repro_plan->environment["PKGDIR"] = repro_pkg.string();
            std::string exports;
            for (const auto& [name, value] : repro_plan->environment) {
                exports += std::format("export {}={}; ", name,
                    sage::build::shell_quote(value));
            }
            for (const auto& step : repro_plan->steps) {
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
                const auto fakeroot_cmd = repro_tool_audit && !repro_tool_audit->sandbox.empty()
                    ? sandboxed_fakeroot_shell(
                        sandbox_fakeroot.string(),
                        full_cmd,
                        repro_tool_audit->sandbox)
                    : sage::build::fakeroot_command(bcfg.fakeroot,
                        hermetic_shell(full_cmd));
                const auto command_result = ProcessExecAudit::run(
                    fakeroot_cmd, repro_tool_audit->process_exec_log,
                    bcfg.memory_limit, bcfg.pids_limit);
                if (!command_result || *command_result != 0) {
                    sage::util::log_error("Reproducibility pass step failed: {}", step.name);
                    return 1;
                }
            }
        }

        const auto repro_transform_source = repro_src / r.managed_build.source_subdir;
        apply_install_transforms(
            repro_transform_source, repro_pkg, r.managed_build.install_copies,
            r.managed_build.install_symlinks,
            r.managed_build.install_moves,
            r.managed_build.install_removes,
            r.managed_build.install_generates);
        apply_file_permissions(repro_pkg, r.managed_build.file_permissions);

        for (const auto& [pkg_name, pass1_file] : created_packages) {
            std::filesystem::path repro_pkg_data = repro_pkg;
            if (!r.managed_build.outputs.empty()) {
                const auto output_spec = std::ranges::find(r.managed_build.outputs,
                    pkg_name, &sage::package::InstallOutput::name);
                if (output_spec != r.managed_build.outputs.end()) {
                    repro_pkg_data = repro_root / ("output-" + pkg_name);
                    std::filesystem::create_directories(repro_pkg_data, repro_ec);
                    std::filesystem::copy(repro_pkg, repro_pkg_data,
                        std::filesystem::copy_options::recursive
                            | std::filesystem::copy_options::copy_symlinks, repro_ec);
                    apply_payload_policy(repro_pkg_data, output_spec->install_files,
                                         output_spec->install_excludes,
                                         output_spec->optional_excludes);
                    apply_install_transforms(
                        repro_transform_source, repro_pkg_data,
                        output_spec->install_copies,
                        output_spec->install_symlinks,
                        output_spec->install_moves,
                        output_spec->install_removes,
                        output_spec->install_generates);
                    apply_file_permissions(repro_pkg_data, output_spec->file_permissions);
                }
            } else {
                apply_payload_policy(repro_pkg_data, r.managed_build.install_files,
                                     r.managed_build.install_excludes,
                                     r.managed_build.optional_excludes);
            }
            std::filesystem::path repro_pkg_file = repro_root / pass1_file.filename();
            auto pack_res = sage::archive::create_package(manifest, repro_pkg_data, repro_pkg_file);
            if (!pack_res) {
                sage::util::log_error("Reproducibility check packaging failed: {}", pack_res.error());
                return 1;
            }
            auto hash1 = sage::util::compute_file_sha256(pass1_file);
            auto hash2 = sage::util::compute_file_sha256(repro_pkg_file);
            if (!hash1 || !hash2 || *hash1 != *hash2) {
                sage::util::log_error("Reproducibility check failed for package '{}':\n  Pass 1 SHA256: {}\n  Pass 2 SHA256: {}",
                    pkg_name,
                    hash1 ? *hash1 : "error",
                    hash2 ? *hash2 : "error");
                return 1;
            }
            sage::util::log_success("Reproducibility check passed for package '{}' (SHA256: {})",
                pkg_name, *hash1);
        }
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
