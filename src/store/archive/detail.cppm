module;
#include <fcntl.h>
#include <dirent.h>
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
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

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
inline std::expected<void, std::string> fsync_fd(int fd) {
    while (::fsync(fd) != 0) {
        if (errno == EINTR) continue;
        return std::unexpected(std::string(std::strerror(errno)));
    }
    return {};
}

inline std::expected<void, std::string> write_fd_all(int fd, std::string_view data) {
    size_t done = 0;
    while (done < data.size()) {
        ssize_t written = ::write(fd, data.data() + done, data.size() - done);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0)
            return std::unexpected(std::string(std::strerror(errno)));
        done += static_cast<size_t>(written);
    }
    return {};
}

// Split a validated relative path into its components; "." and empty pieces
// (duplicated slashes, trailing slash) vanish. ".." must be rejected by the
// caller beforehand (normalize_data_path).
inline std::vector<std::string> rel_components(std::string_view relative) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= relative.size()) {
        const auto slash = relative.find('/', start);
        const auto piece = relative.substr(
            start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (!piece.empty() && piece != ".") out.emplace_back(piece);
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return out;
}

inline std::expected<std::string, std::string> random_hex(size_t byte_count) {
    static constexpr char hex[] = "0123456789abcdef";
    std::random_device device;
    std::string out;
    out.reserve(byte_count * 2);
    for (size_t i = 0; i < byte_count; ++i) {
        const auto value = static_cast<unsigned>(device());
        out.push_back(hex[(value >> 4) & 0xf]);
        out.push_back(hex[value & 0xf]);
    }
    return out;
}

inline std::expected<std::vector<std::string>, std::string> list_dir_names(int dir_fd) {
    const int duplicate = ::fcntl(dir_fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) return std::unexpected(std::string(std::strerror(errno)));
    DIR* dir = ::fdopendir(duplicate);
    if (!dir) {
        ::close(duplicate);
        return std::unexpected(std::string(std::strerror(errno)));
    }
    std::vector<std::string> names;
    while (const auto* entry = ::readdir(dir)) {
        const std::string_view name = entry->d_name;
        if (name == "." || name == "..") continue;
        names.emplace_back(entry->d_name);
    }
    ::closedir(dir);
    return names;
}

// Same anchored walk as open_anchored_dir but never creates anything: used
// where existence itself is part of the contract (attach, journal reads).
inline std::expected<UniqueFd, std::string> open_anchored_dir_strict(
    int root_fd, const std::vector<std::string>& components) {
    int duplicated = ::fcntl(root_fd, F_DUPFD_CLOEXEC, 0);
    if (duplicated < 0)
        return std::unexpected("Cannot duplicate root descriptor");
    UniqueFd current(duplicated);
    for (const auto& name : components) {
        int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        int next = ::openat(current.get(), name.c_str(), flags);
        if (next < 0) {
            return std::unexpected(std::format(
                "Cannot anchor directory '{}': {}", name, std::strerror(errno)));
        }
        current = UniqueFd(next);
    }
    return current;
}

// Anchored mkdir -p: descends without ever following a symlink, creates
// missing components with `mode`, and fsyncs each parent immediately after a
// new component materializes so the chain survives power loss.
inline std::expected<UniqueFd, std::string> create_anchored_dir_chain(
    int root_fd, std::string_view relative, uint32_t mode = 0755) {
    const auto components = rel_components(relative);
    int duplicated = ::fcntl(root_fd, F_DUPFD_CLOEXEC, 0);
    if (duplicated < 0)
        return std::unexpected("Cannot duplicate root descriptor");
    UniqueFd current(duplicated);
    for (const auto& name : components) {
        int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        int next = ::openat(current.get(), name.c_str(), flags);
        if (next < 0 && errno == ENOENT) {
            if (::mkdirat(current.get(), name.c_str(), mode & 07777) != 0
                && errno != EEXIST) {
                return std::unexpected(std::format(
                    "Cannot create directory '{}': {}", name, std::strerror(errno)));
            }
            // A fresh component's directory entry must reach storage before
            // anything is placed inside it.
            if (auto synced = fsync_fd(current.get()); !synced)
                return std::unexpected(synced.error());
            next = ::openat(current.get(), name.c_str(), flags);
        }
        if (next < 0) {
            return std::unexpected(std::format(
                "Cannot anchor directory '{}': {}", name, std::strerror(errno)));
        }
        current = UniqueFd(next);
    }
    return current;
}

// Stream-copy exactly `expected_size` bytes from one fd to another. Prefers
// in-kernel copy_file_range() (reflink-friendly on btrfs/XFS); falls back to
// a bounded user-space buffer on ENOSYS/EINVAL/EXDEV/EOPNOTSUPP. EINTR is
// retried, short transfers loop, and the final total length is verified.
inline std::expected<void, std::string> copy_fd_exact(
    int source_fd, int dest_fd, uint64_t expected_size) {
    uint64_t copied = 0;
#ifdef SYS_copy_file_range
    while (copied < expected_size) {
        const ssize_t moved = ::syscall(SYS_copy_file_range, source_fd,
            static_cast<off_t*>(nullptr), dest_fd, static_cast<off_t*>(nullptr),
            static_cast<size_t>(expected_size - copied), 0u);
        if (moved < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOSYS || errno == EINVAL || errno == EXDEV
                || errno == EOPNOTSUPP || errno == EBADF)
                break; // kernel or filesystem cannot help: buffered fallback,
                       // resuming from wherever the fast path stopped
            return std::unexpected(
                std::format("copy_file_range failed: {}", std::strerror(errno)));
        }
        if (moved == 0)
            return std::unexpected(std::string("Source ended prematurely during copy"));
        copied += static_cast<uint64_t>(moved);
    }
    if (copied == expected_size) return {};
#else
    (void)copied;
#endif
    std::array<uint8_t, 256 * 1024> buffer{};
    while (copied < expected_size) {
        const auto wanted = static_cast<size_t>(
            std::min<uint64_t>(buffer.size(), expected_size - copied));
        ssize_t received = ::read(source_fd, buffer.data(), wanted);
        if (received < 0 && errno == EINTR) continue;
        if (received < 0)
            return std::unexpected(std::format("Read failed: {}", std::strerror(errno)));
        if (received == 0)
            return std::unexpected(std::string("Source ended prematurely during copy"));
        size_t written = 0;
        while (written < static_cast<size_t>(received)) {
            ssize_t sent = ::write(dest_fd, buffer.data() + written,
                static_cast<size_t>(received) - written);
            if (sent < 0 && errno == EINTR) continue;
            if (sent <= 0)
                return std::unexpected(std::format("Write failed: {}", std::strerror(errno)));
            written += static_cast<size_t>(sent);
        }
        copied += static_cast<uint64_t>(received);
    }
    return {};
}


// Anchored rm -rf of one named child below `parent_fd`; missing names are
// already gone and therefore success.
inline std::expected<void, std::string> remove_tree_anchored(
    int parent_fd, const std::string& name) {
    struct stat status {};
    if (::fstatat(parent_fd, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return {};
        return std::unexpected(std::string(std::strerror(errno)));
    }
    if (S_ISDIR(status.st_mode)) {
        int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        UniqueFd sub(::openat(parent_fd, name.c_str(), flags));
        if (sub.get() >= 0) {
            auto names = list_dir_names(sub.get());
            if (!names) return std::unexpected(names.error());
            for (const auto& child : *names) {
                if (auto removed = remove_tree_anchored(sub.get(), child); !removed)
                    return removed;
            }
        } else if (errno != ENOENT) {
            return std::unexpected(std::string(std::strerror(errno)));
        }
        if (::unlinkat(parent_fd, name.c_str(), AT_REMOVEDIR) != 0 && errno != ENOENT)
            return std::unexpected(std::string(std::strerror(errno)));
    } else if (::unlinkat(parent_fd, name.c_str(), 0) != 0 && errno != ENOENT) {
        return std::unexpected(std::string(std::strerror(errno)));
    }
    return {};
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
// A private, same-filesystem copy of a package archive under
// `<target_root>/var/lib/sage/tmp`, immune to concurrent rewrites of the
// repository cache between inspection and extraction.
//
// Why the target root and not /tmp: the snapshot must share the target
// root's filesystem so downstream staging can flip files into place with
// renameat(2) -- a cross-filesystem copy would turn every publication into
// EXDEV, and a host tmpfs would silently drop that guarantee while also
// making `--root` depend on host state. Creation is fully anchored
// (openat/mkdirat/O_NOFOLLOW -- never path-based create_directories/mkdtemp,
// which would follow symlinks planted at <root>/var), the copy prefers
// copy_file_range(), and the snapshot file is fsynced before it may be
// consumed: a reader must never see a name whose bytes are not yet durable.
// Destruction removes every trace of the snapshot directory.
class PrivateArchiveSnapshot {
public:
    PrivateArchiveSnapshot(const PrivateArchiveSnapshot&) = delete;
    PrivateArchiveSnapshot& operator=(const PrivateArchiveSnapshot&) = delete;

    PrivateArchiveSnapshot(PrivateArchiveSnapshot&& other) noexcept
        : parent_fd_(std::move(other.parent_fd_)),
          dir_fd_(std::move(other.dir_fd_)),
          directory_name_(std::exchange(other.directory_name_, {})),
          archive_path_(std::move(other.archive_path_)) {}

    ~PrivateArchiveSnapshot() noexcept {
        if (dir_fd_.get() >= 0)
            (void)::unlinkat(dir_fd_.get(), snapshot_leaf, 0);
        if (parent_fd_.get() >= 0 && !directory_name_.empty())
            (void)::unlinkat(
                parent_fd_.get(), directory_name_.c_str(), AT_REMOVEDIR);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return archive_path_;
    }

    static std::expected<PrivateArchiveSnapshot, std::string> create(
        const std::filesystem::path& source_path,
        const std::filesystem::path& target_root)
    {
        int source_flags = O_RDONLY;
#ifdef O_CLOEXEC
        source_flags |= O_CLOEXEC;
#endif
        detail::UniqueFd source(::open(source_path.c_str(), source_flags));
        if (source.get() < 0) {
            return std::unexpected(
                "Cannot open package archive: " + source_path.string());
        }

        struct stat source_status {};
        if (::fstat(source.get(), &source_status) != 0) {
            return std::unexpected("Cannot stat package archive: "
                + std::string(std::strerror(errno)));
        }
        if (!S_ISREG(source_status.st_mode) || source_status.st_size < 0) {
            return std::unexpected("Package archive is not a regular file");
        }

        detail::UniqueFd root(detail::open_root_fd(target_root));
        if (root.get() < 0) {
            return std::unexpected("Cannot securely open target root: "
                + std::string(std::strerror(errno)));
        }

        auto tmp = detail::create_anchored_dir_chain(
            root.get(), "var/lib/sage/tmp", 0700);
        if (!tmp) return std::unexpected(tmp.error());

        auto suffix = detail::random_hex(8);
        if (!suffix) return std::unexpected(suffix.error());
        const std::string directory_name =
            std::string(temp_file_prefix) + "archive-" + *suffix;
        if (::mkdirat(tmp->get(), directory_name.c_str(), 0700) != 0) {
            return std::unexpected("Cannot create private archive directory: "
                + std::string(std::strerror(errno)));
        }
        if (auto synced = detail::fsync_fd(tmp->get()); !synced)
            return std::unexpected(synced.error());

        int dir_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
        dir_flags |= O_CLOEXEC;
#endif
        detail::UniqueFd dir(::openat(tmp->get(), directory_name.c_str(), dir_flags));
        if (dir.get() < 0) {
            return std::unexpected("Cannot reopen private archive directory: "
                + std::string(std::strerror(errno)));
        }

        // Owns the directories from here on: any failure below cleans up.
        PrivateArchiveSnapshot snapshot{
            std::move(*tmp), std::move(dir), directory_name,
            target_root / "var/lib/sage/tmp" / directory_name / snapshot_leaf};

        int dest_flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW;
#ifdef O_CLOEXEC
        dest_flags |= O_CLOEXEC;
#endif
        detail::UniqueFd destination(
            ::openat(snapshot.dir_fd_.get(), snapshot_leaf, dest_flags, 0600));
        if (destination.get() < 0) {
            return std::unexpected("Cannot create private archive snapshot: "
                + std::string(std::strerror(errno)));
        }

        auto copied = detail::copy_fd_exact(source.get(), destination.get(),
            static_cast<uint64_t>(source_status.st_size));
        if (!copied) return std::unexpected(copied.error());
        // Durability before consumption: a reader must never observe a file
        // name whose contents are still in page-cache limbo.
        if (auto synced = detail::fsync_fd(destination.get()); !synced) {
            return std::unexpected(
                "Cannot sync private archive snapshot: " + synced.error());
        }
        return snapshot;
    }

private:
    PrivateArchiveSnapshot(detail::UniqueFd parent, detail::UniqueFd dir,
        std::string directory_name, std::filesystem::path archive_path)
        : parent_fd_(std::move(parent)),
          dir_fd_(std::move(dir)),
          directory_name_(std::move(directory_name)),
          archive_path_(std::move(archive_path)) {}

    static constexpr const char* snapshot_leaf = "archive.pkg.tar.zst";

    detail::UniqueFd parent_fd_{-1}; // var/lib/sage/tmp, for dir removal
    detail::UniqueFd dir_fd_{-1};    // the private snapshot directory itself
    std::string directory_name_;
    std::filesystem::path archive_path_;
};

} // namespace sage::archive
