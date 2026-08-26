module;
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

export module sage.cli.build:transforms;

import std;
import sage;

namespace sage::cli {

// The backend is allowed to install into the isolated staging root, but the
// recipe remains the authority over which of those files become package
// payload.  Apply the allowlist only after the backend has finished: this
// keeps Autotools/CMake/Meson/etc. free to run their normal install logic while
// making split packages explicit and reviewable.  All matching is against
// canonical relative paths below pkg_dir; no pattern can escape that root.
inline std::expected<void, std::string> apply_payload_policy(
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

inline std::expected<void, std::string> apply_file_permissions(
    const std::filesystem::path& pkg_dir,
    const std::vector<sage::package::FilePermission>& perms)
{
    std::error_code ec;
    // Symbolic owner spellings resolve through the host user database at
    // build time; a name Sage cannot resolve is a packaging error rather
    // than a silent uid 0.
    const auto resolve_user = [&](const std::string& name) -> std::expected<uint32_t, std::string> {
        if (name.empty()) return 0u;
        if (const auto* pw = ::getpwnam(name.c_str())) {
            if (pw->pw_uid <= 0xFFFF)
                return static_cast<uint32_t>(pw->pw_uid);
            return std::unexpected(std::format(
                "file_permissions user '{}' resolves to non-system uid {}", name, pw->pw_uid));
        }
        return std::unexpected("file_permissions user '" + name + "' does not exist");
    };
    const auto resolve_group = [&](const std::string& name) -> std::expected<uint32_t, std::string> {
        if (name.empty()) return 0u;
        if (const auto* gr = ::getgrnam(name.c_str())) {
            if (gr->gr_gid <= 0xFFFF)
                return static_cast<uint32_t>(gr->gr_gid);
            return std::unexpected(std::format(
                "file_permissions group '{}' resolves to non-system gid {}", name, gr->gr_gid));
        }
        return std::unexpected("file_permissions group '" + name + "' does not exist");
    };
    for (const auto& fp : perms) {
        if (fp.path.empty()) continue;
        uint32_t uid = fp.uid;
        uint32_t gid = fp.gid;
        if (auto resolved = resolve_user(fp.user)) uid = *resolved;
        else return std::unexpected(resolved.error());
        if (auto resolved = resolve_group(fp.group)) gid = *resolved;
        else return std::unexpected(resolved.error());
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
        if (!fp.user.empty() || !fp.group.empty()) {
            if (uid == 0 && gid == 0) continue;
            if (::chown(target.c_str(), uid, gid) != 0) {
                // Non-root builders cannot change ownership; declaring a
                // non-root owner is therefore a hard error, not a warning.
                return std::unexpected("Cannot set owner for '" + fp.path + "': "
                                       + std::strerror(errno));
            }
        }
    }
    return {};
}

inline std::expected<void, std::string> apply_install_transforms(
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

} // namespace sage::cli
