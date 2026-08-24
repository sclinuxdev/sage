module;
#include <fcntl.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <unistd.h>
export module sage.archive:detail;

// Anchored path vocabulary: never follow a symlink outside the target root.

import std;
import sage.package;
import sage.util;
import :core;

export namespace sage::archive {


inline std::expected<std::string, std::string> normalize_data_path(std::string_view raw_path) {
    std::filesystem::path path(raw_path);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return std::unexpected("Package data path must be relative: " + std::string(raw_path));
    }
    for (const auto& component : path) {
        if (component == "..") {
            return std::unexpected("Package data path escapes the target root: " + std::string(raw_path));
        }
    }

    auto normalized = path.lexically_normal();
    auto text = normalized.generic_string();
    while (text.size() > 1 && text.back() == '/') text.pop_back();
    if (text.empty() || text == "." || text == "/") {
        return std::unexpected("Package data path is empty");
    }
    return text;
}

inline std::expected<std::string, bool> canonicalize_merge_claim(std::string_view path) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 6> aliases{{
        {"usr/sbin", "usr/bin"},
        {"usr/lib64", "usr/lib"},
        {"sbin", "usr/bin"},
        {"lib64", "usr/lib"},
        {"bin", "usr/bin"},
        {"lib", "usr/lib"},
    }};
    for (const auto& [from, to] : aliases) {
        if (path == from) return std::unexpected(true);
        const auto prefix = std::format("{}/", from);
        if (path.starts_with(prefix))
            return std::format("{}/{}", to, path.substr(prefix.size()));
    }
    return std::string{path};
}

namespace detail {

class UniqueFd {
public:
    explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~UniqueFd() noexcept { reset(); }
    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    int fd_;
};

inline int open_root_fd(const std::filesystem::path& target_root) {
    int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    return ::open(target_root.c_str(), flags);
}

inline void close_fd(int fd) noexcept { ::close(fd); }

// Walks every component below the root without following symlinks; missing
// intermediate directories are created (0755) so payload trees materialize.
inline std::expected<int, std::string> open_anchored_dir(
    int root_fd, const std::vector<std::string>& components) {
    int current = ::dup(root_fd);
    if (current < 0) return std::unexpected("Cannot duplicate target root descriptor");
    for (const auto& name : components) {
        int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        int next = ::openat(current, name.c_str(), flags);
        if (next < 0 && errno == ENOENT) {
            if (::mkdirat(current, name.c_str(), 0755) == 0) {
                next = ::openat(current, name.c_str(), flags);
            } else if (errno == EEXIST) {
                next = ::openat(current, name.c_str(), flags);
            }
        }
        if (next < 0) {
            close_fd(current);
            return std::unexpected(std::format(
                "Cannot anchor directory '{}': {}", name, std::strerror(errno)));
        }
        close_fd(current);
        current = next;
    }
    return current;
}

} // namespace detail

inline std::expected<void, std::string> remove_path_anchored(
    const std::filesystem::path& target_root,
    std::string_view raw_path,
    bool ignore_nonempty_directory = false)
{
    auto normalized = normalize_data_path(raw_path);
    if (!normalized) return std::unexpected(normalized.error());

    auto canonical = canonicalize_merge_claim(*normalized);
    if (!canonical.has_value()) return {};
    if (*canonical != *normalized) {
        sage::util::log_info(
            "  ~ claim '{}' predates the usr merge; cleaning as '{}'",
            raw_path, *canonical);
    }

    detail::UniqueFd root(detail::open_root_fd(target_root));
    if (root.get() < 0) {
        return std::unexpected(
            "Cannot securely open target root: " + std::string(std::strerror(errno)));
    }
    const auto relative = std::filesystem::path(*canonical);
    std::vector<std::string> components;
    for (const auto& component : relative.parent_path()) {
        if (component == ".") continue;
        components.push_back(component.string());
    }
    auto parent = detail::open_anchored_dir(root.get(), components);
    if (!parent) {
        if (parent.error().find("No such file") != std::string::npos) return {};
        return std::unexpected(parent.error());
    }

    const auto leaf = relative.filename().string();
    struct stat status {};
    if (::fstatat(*parent, leaf.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return {};
        return std::unexpected(std::strerror(errno));
    }
    const bool is_directory = S_ISDIR(status.st_mode);
    if (::unlinkat(*parent, leaf.c_str(), is_directory ? AT_REMOVEDIR : 0) != 0) {
        if (ignore_nonempty_directory && is_directory
            && (errno == ENOTEMPTY || errno == EEXIST)) {
            return {};
        }
        return std::unexpected(std::strerror(errno));
    }
    return {};
}

// A declared conffile whose on-disk contents no longer match the recorded hash
// must not be overwritten: the admin edited it.
inline bool conffile_modified(
    const std::filesystem::path& target_root,
    std::string_view rel_path,
    const std::vector<std::string>& conffiles,
    const package::PackageManifest* previous)
{
    if (!previous) return false;
    const auto key = util::clean_rel_path(rel_path);
    const auto declared = std::ranges::find_if(conffiles, [&](const auto& c) {
        return util::clean_rel_path(c) == key;
    });
    if (declared == conffiles.end()) return false;

    std::error_code ec;
    const auto disk_path = target_root / key;
    if (!std::filesystem::is_regular_file(disk_path, ec)) return false;
    auto disk_hash = util::compute_file_sha256(disk_path);
    if (!disk_hash) return true; // unreadable -> assume local changes

    for (const auto& f : previous->files) {
        if (util::clean_rel_path(f.path) != key) continue;
        return f.sha256 != *disk_hash;
    }
    return true;
}


namespace detail {

struct SourceIdentity {
    uint64_t device{0};
    uint64_t inode{0};
    uint64_t size{0};
    uint64_t mtime_ns{0};
    uint64_t ctime_ns{0};

    static std::expected<SourceIdentity, std::string> of(const std::filesystem::path& path) {
        int flags = O_RDONLY | O_NOFOLLOW;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        UniqueFd fd(::open(path.c_str(), flags));
        if (fd.get() < 0) {
            return std::unexpected("Cannot open package archive: " + path.string());
        }
        struct stat status {};
        if (::fstat(fd.get(), &status) != 0 || !S_ISREG(status.st_mode)) {
            return std::unexpected("Package archive is not a regular file");
        }
        auto ns = [](const timespec& ts) {
            return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000
                + static_cast<uint64_t>(ts.tv_nsec);
        };
        return SourceIdentity{
            .device = static_cast<uint64_t>(status.st_dev),
            .inode = static_cast<uint64_t>(status.st_ino),
            .size = static_cast<uint64_t>(status.st_size),
            .mtime_ns = ns(status.st_mtim),
            .ctime_ns = ns(status.st_ctim),
        };
    }
};

// Structural rules every data/ member must satisfy, wherever it is seen.
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 6> usr_merge_aliases{{
    {"bin", "usr/bin"},
    {"sbin", "usr/bin"},
    {"lib", "usr/lib"},
    {"lib64", "usr/lib"},
    {"usr/sbin", "bin"},
    {"usr/lib64", "lib"},
}};

inline std::expected<void, std::string> validate_payload_path(
    std::string_view path, char typeflag, std::string_view link_target,
    std::string_view package_name)
{
    if (path.starts_with("usr/sbin/") || path.starts_with("usr/lib64/")) {
        return std::unexpected(std::format(
            "Package '{}' must not install payload below compatibility path 'data/{}'",
            package_name, path));
    }
    for (const auto& component : std::filesystem::path(path)) {
        if (component.string().starts_with(temp_file_prefix)) {
            return std::unexpected(std::format(
                "Package data path uses reserved temporary-file namespace: {}", path));
        }
    }
    auto alias = std::ranges::find_if(usr_merge_aliases, [&](const auto& candidate) {
        return path == candidate.first || path.starts_with(std::string(candidate.first) + "/");
    });
    if (alias == usr_merge_aliases.end()) return {};
    const bool is_base_merge_link = package_name == "base-files"
        && path == alias->first && typeflag == '2' && link_target == alias->second;
    if (!is_base_merge_link) {
        return std::unexpected(std::format(
            "Package '{}' must use canonical usr/ paths instead of '{}'",
            package_name, path));
    }
    return {};
}

} // namespace detail
} // namespace sage::archive
