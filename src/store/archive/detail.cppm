module;

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <cerrno>
#include <cstring>

// Internal partition: resource handles, anchored path traversal and the
// shared data vocabulary. Nothing here escapes the sage.archive module.
module sage.archive:detail;

import std;

namespace sage::archive {

using std::size_t;
using std::uint8_t;
using std::uint32_t;
using std::uint64_t;

struct PackageDataEntry {
    std::string path;
    uint64_t size{0};
    uint32_t mode{0};
    char typeflag{'0'};
    std::string link_target;
    std::string sha256;
};

class UniqueFd {
public:
    explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)) {}

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

    int fd_{-1};
};

class PrivateArchiveSnapshot {
public:
    PrivateArchiveSnapshot(const PrivateArchiveSnapshot&) = delete;
    PrivateArchiveSnapshot& operator=(const PrivateArchiveSnapshot&) = delete;

    PrivateArchiveSnapshot(PrivateArchiveSnapshot&& other) noexcept
        : directory_(std::exchange(other.directory_, {})),
          archive_(std::exchange(other.archive_, {})) {}

    PrivateArchiveSnapshot& operator=(PrivateArchiveSnapshot&&) = delete;

    ~PrivateArchiveSnapshot() noexcept {
        std::error_code ec;
        std::filesystem::remove(archive_, ec);
        ec.clear();
        std::filesystem::remove(directory_, ec);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return archive_;
    }

    static std::expected<PrivateArchiveSnapshot, std::string> create(
        const std::filesystem::path& source_path,
        const std::filesystem::path& snapshot_root,
        std::optional<std::array<uint64_t, 5>> expected_source = std::nullopt)
    {
        int source_flags = O_RDONLY;
#ifdef O_CLOEXEC
        source_flags |= O_CLOEXEC;
#endif
        UniqueFd source(::open(source_path.c_str(), source_flags));
        if (source.get() < 0) {
            return std::unexpected(
                "Cannot open package archive: " + source_path.string());
        }

        struct stat source_status {};
        if (::fstat(source.get(), &source_status) != 0) {
            return std::unexpected(
                "Cannot stat package archive: " + std::string(std::strerror(errno)));
        }
        if (!S_ISREG(source_status.st_mode) || source_status.st_size < 0) {
            return std::unexpected("Package archive is not a regular file");
        }
        const auto mtime_ns = static_cast<uint64_t>(source_status.st_mtim.tv_sec) * 1'000'000'000
            + static_cast<uint64_t>(source_status.st_mtim.tv_nsec);
        const auto ctime_ns = static_cast<uint64_t>(source_status.st_ctim.tv_sec) * 1'000'000'000
            + static_cast<uint64_t>(source_status.st_ctim.tv_nsec);
        const std::array<uint64_t, 5> actual_source{
            static_cast<uint64_t>(source_status.st_dev),
            static_cast<uint64_t>(source_status.st_ino),
            static_cast<uint64_t>(source_status.st_size),
            mtime_ns,
            ctime_ns,
        };
        if (expected_source && *expected_source != actual_source) {
            return std::unexpected(
                "Package archive changed after ownership preflight");
        }

        std::error_code directory_ec;
        std::filesystem::create_directories(snapshot_root, directory_ec);
        if (directory_ec) {
            return std::unexpected(
                "Cannot create private archive root: " + directory_ec.message());
        }

        std::string directory_template =
            (snapshot_root / "archive-XXXXXX").string();
        std::vector<char> mutable_template(
            directory_template.begin(), directory_template.end());
        mutable_template.push_back('\0');
        char* directory = ::mkdtemp(mutable_template.data());
        if (!directory) {
            return std::unexpected(
                "Cannot create private archive directory: "
                + std::string(std::strerror(errno)));
        }

        PrivateArchiveSnapshot snapshot{std::filesystem::path(directory)};
        int destination_flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW;
#ifdef O_CLOEXEC
        destination_flags |= O_CLOEXEC;
#endif
        UniqueFd destination(::open(
            snapshot.archive_.c_str(), destination_flags, 0600));
        if (destination.get() < 0) {
            return std::unexpected(
                "Cannot create private archive snapshot: "
                + std::string(std::strerror(errno)));
        }

        uint64_t remaining = static_cast<uint64_t>(source_status.st_size);
#ifdef __linux__
        bool use_copy_file_range = true;
        while (remaining > 0 && use_copy_file_range) {
            const auto requested = static_cast<size_t>(
                std::min<uint64_t>(remaining, 1024 * 1024));
            ssize_t count = ::copy_file_range(
                source.get(), nullptr, destination.get(), nullptr, requested, 0);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == ENOSYS || errno == EINVAL || errno == EXDEV
                              || errno == EOPNOTSUPP)) {
                use_copy_file_range = false;
                break;
            }
            if (count < 0) {
                return std::unexpected(
                    "Cannot copy package archive: " + std::string(std::strerror(errno)));
            }
            if (count == 0) {
                return std::unexpected(
                    "Package archive changed while creating a private snapshot");
            }
            remaining -= static_cast<uint64_t>(count);
        }
#endif
        std::array<uint8_t, 64 * 1024> buffer {};
        while (remaining > 0) {
            const auto requested = static_cast<size_t>(
                std::min<uint64_t>(remaining, buffer.size()));
            ssize_t count = ::read(source.get(), buffer.data(), requested);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) {
                return std::unexpected(
                    "Cannot read package archive: "
                    + std::string(std::strerror(errno)));
            }
            if (count == 0) {
                return std::unexpected(
                    "Package archive changed while creating a private snapshot");
            }

            size_t written = 0;
            while (written < static_cast<size_t>(count)) {
                ssize_t result = ::write(
                    destination.get(), buffer.data() + written,
                    static_cast<size_t>(count) - written);
                if (result < 0 && errno == EINTR) continue;
                if (result <= 0) {
                    return std::unexpected(
                        "Cannot write private archive snapshot: "
                        + std::string(std::strerror(errno)));
                }
                written += static_cast<size_t>(result);
            }
            remaining -= static_cast<uint64_t>(count);
        }
        struct stat final_status {};
        if (::fstat(source.get(), &final_status) != 0) {
            return std::unexpected("Cannot recheck package archive identity");
        }
        const auto final_mtime_ns = static_cast<uint64_t>(final_status.st_mtim.tv_sec)
                * 1'000'000'000
            + static_cast<uint64_t>(final_status.st_mtim.tv_nsec);
        const auto final_ctime_ns = static_cast<uint64_t>(final_status.st_ctim.tv_sec)
                * 1'000'000'000
            + static_cast<uint64_t>(final_status.st_ctim.tv_nsec);
        const std::array<uint64_t, 5> final_source{
            static_cast<uint64_t>(final_status.st_dev),
            static_cast<uint64_t>(final_status.st_ino),
            static_cast<uint64_t>(final_status.st_size),
            final_mtime_ns,
            final_ctime_ns,
        };
        if (final_source != actual_source) {
            return std::unexpected(
                "Package archive changed while creating private snapshot");
        }

        return snapshot;
    }

private:
    explicit PrivateArchiveSnapshot(std::filesystem::path directory)
        : directory_(std::move(directory)),
          archive_(directory_ / "archive.pkg.tar.zst") {}

    std::filesystem::path directory_;
    std::filesystem::path archive_;
};

struct AnchoredPath {
    UniqueFd directory;
    std::string leaf;
};

inline std::expected<AnchoredPath, std::string> open_anchored_parent(
    int root_fd,
    const std::filesystem::path& relative,
    bool durable)
{
    int duplicate = ::fcntl(root_fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) {
        return std::unexpected(
            "Cannot duplicate target-root directory: " + std::string(std::strerror(errno)));
    }
    UniqueFd current(duplicate);

    for (const auto& component : relative.parent_path()) {
        if (component == ".") continue;
        const auto name = component.string();
        int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        int next = ::openat(current.get(), name.c_str(), flags);
        if (next < 0 && errno == ENOENT) {
            const bool created = ::mkdirat(current.get(), name.c_str(), 0755) == 0;
            if (!created && errno != EEXIST) {
                return std::unexpected(std::format(
                    "Cannot create parent directory '{}': {}", name, std::strerror(errno)));
            }
            // Persist the directory entry while its containing directory is
            // still anchored.  Fsyncing the child alone does not make the
            // parent's mkdir entry durable across power loss.
            if (created && durable && ::fsync(current.get()) != 0) {
                return std::unexpected(std::format(
                    "Cannot sync parent after creating directory '{}': {}",
                    name, std::strerror(errno)));
            }
            next = ::openat(current.get(), name.c_str(), flags);
        }
        if (next < 0) {
            return std::unexpected(std::format(
                "Cannot securely open parent directory '{}': {}", name, std::strerror(errno)));
        }
        current = UniqueFd(next);
    }

    return AnchoredPath{
        .directory = std::move(current),
        .leaf = relative.filename().string(),
    };
}

inline std::expected<void, std::string> ensure_anchored_directory(
    int parent_fd,
    std::string_view leaf,
    bool durable)
{
    const auto name = std::string(leaf);
    if (::mkdirat(parent_fd, name.c_str(), 0755) == 0) {
        if (durable && ::fsync(parent_fd) != 0) return std::unexpected(std::strerror(errno));
        return {};
    }
    if (errno != EEXIST) {
        return std::unexpected(std::strerror(errno));
    }

    struct stat status {};
    if (::fstatat(parent_fd, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        return std::unexpected(std::strerror(errno));
    }
    if (!S_ISDIR(status.st_mode)) {
        return std::unexpected("existing path is not a directory");
    }
    return {};
}

inline std::expected<void, std::string> remove_anchored_leaf(
    int parent_fd,
    std::string_view leaf,
    bool durable)
{
    const auto name = std::string(leaf);
    struct stat status {};
    if (::fstatat(parent_fd, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return {};
        return std::unexpected(std::strerror(errno));
    }

    const int flags = S_ISDIR(status.st_mode) ? AT_REMOVEDIR : 0;
    if (::unlinkat(parent_fd, name.c_str(), flags) != 0) {
        return std::unexpected(std::strerror(errno));
    }
    if (durable && ::fsync(parent_fd) != 0) return std::unexpected(std::strerror(errno));
    return {};
}

class UniqueTempFile {
public:
    UniqueTempFile(const UniqueTempFile&) = delete;
    UniqueTempFile& operator=(const UniqueTempFile&) = delete;

    UniqueTempFile(UniqueTempFile&& other) noexcept
        : directory_(std::move(other.directory_)),
          fd_(std::exchange(other.fd_, -1)),
          name_(std::exchange(other.name_, {})) {}

    UniqueTempFile& operator=(UniqueTempFile&& other) noexcept {
        if (this != &other) {
            cleanup();
            directory_ = std::move(other.directory_);
            fd_ = std::exchange(other.fd_, -1);
            name_ = std::exchange(other.name_, {});
        }
        return *this;
    }

    ~UniqueTempFile() noexcept { cleanup(); }

    static std::expected<UniqueTempFile, std::string> create(UniqueFd directory)
    {
        static std::atomic<uint64_t> sequence{0};
        for (size_t attempt = 0; attempt < 128; ++attempt) {
            auto name = std::format(
                ".sage-tmp-{:x}-{:x}-{:x}",
                static_cast<uint64_t>(::getpid()),
                sequence.fetch_add(1, std::memory_order_relaxed),
                attempt);
            int flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW;
#ifdef O_CLOEXEC
            flags |= O_CLOEXEC;
#endif
            int fd = ::openat(directory.get(), name.c_str(), flags, 0600);
            if (fd >= 0) {
                return UniqueTempFile(std::move(directory), fd, std::move(name));
            }
            if (errno != EEXIST) {
                return std::unexpected(std::format(
                    "Cannot create temporary file: {}", std::strerror(errno)));
            }
        }
        return std::unexpected("Cannot create a unique temporary file after repeated collisions");
    }

    std::expected<void, std::string> write_all(std::span<const uint8_t> payload) {
        size_t written = 0;
        while (written < payload.size()) {
            ssize_t count = ::write(fd_, payload.data() + written, payload.size() - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                return std::unexpected(std::strerror(errno));
            }
            written += static_cast<size_t>(count);
        }
        return {};
    }

    std::expected<void, std::string> install(
        std::string_view destination,
        uint32_t mode,
        bool durable)
    {
        if (::fchmod(fd_, mode ? mode : 0644) != 0) {
            return std::unexpected(std::format(
                "Cannot set mode: {}", std::strerror(errno)));
        }
        if (durable && ::fsync(fd_) != 0) {
            return std::unexpected(std::format(
                "Cannot sync temporary file: {}", std::strerror(errno)));
        }
        if (::close(fd_) != 0) {
            auto message = std::string(std::strerror(errno));
            fd_ = -1;
            return std::unexpected("Cannot close temporary file: " + message);
        }
        fd_ = -1;
        auto destination_name = std::string(destination);
        if (::renameat(
                directory_.get(), name_.c_str(), directory_.get(), destination_name.c_str()) != 0) {
            return std::unexpected(
                "Cannot atomically install temporary file: " + std::string(std::strerror(errno)));
        }
        name_.clear();
        if (durable && ::fsync(directory_.get()) != 0) {
            return std::unexpected(
                "Cannot sync destination directory: " + std::string(std::strerror(errno)));
        }
        return {};
    }

private:
    UniqueTempFile(UniqueFd directory, int fd, std::string name)
        : directory_(std::move(directory)), fd_(fd), name_(std::move(name)) {}

    void cleanup() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        if (directory_.get() >= 0 && !name_.empty()) {
            (void)::unlinkat(directory_.get(), name_.c_str(), 0);
            name_.clear();
        }
    }

    UniqueFd directory_;
    int fd_{-1};
    std::string name_;
};

inline constexpr std::string_view temp_file_prefix = ".sage-tmp-";

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
    // Directory entries arrive as "usr/" and lexically_normal preserves the
    // trailing separator -- which would make filename() empty downstream
    // (mkdirat(fd, "") fails with ENOENT). Strip trailing separators so a
    // single-component directory anchors on the target root itself.
    auto text = normalized.generic_string();
    while (text.size() > 1 && text.back() == '/') text.pop_back();
    if (text.empty() || text == "." || text == "/") {
        return std::unexpected("Package data path is empty");
    }
    return text;
}
inline std::expected<void, std::string> validate_target_path(
    const PackageDataEntry& entry,
    const std::filesystem::path& target_root,
    const std::filesystem::path& resolved_root)
{
    const auto relative = std::filesystem::path(entry.path);
    auto current = resolved_root;
    for (const auto& component : relative.parent_path()) {
        if (component == ".") continue;
        current /= component;

        std::error_code status_ec;
        auto status = std::filesystem::symlink_status(current, status_ec);
        if (status_ec == std::errc::no_such_file_or_directory) {
            status_ec.clear();
            continue;
        }
        if (status_ec) {
            return std::unexpected(std::format(
                "Cannot inspect parent directory for '{}': {}", entry.path, status_ec.message()));
        }
        if (std::filesystem::is_symlink(status)) {
            return std::unexpected(std::format(
                "Parent symlink for '{}' is not allowed", entry.path));
        } else if (!std::filesystem::is_directory(status)) {
            return std::unexpected(std::format(
                "Parent path for '{}' is not a directory", entry.path));
        }
    }

    const auto destination = target_root / relative;
    std::error_code status_ec;
    auto status = std::filesystem::symlink_status(destination, status_ec);
    if (status_ec == std::errc::no_such_file_or_directory) return {};
    if (status_ec) {
        return std::unexpected(std::format(
            "Cannot inspect '{}': {}", entry.path, status_ec.message()));
    }

    if (entry.typeflag == '5') {
        if (!std::filesystem::is_directory(status)) {
            return std::unexpected(std::format(
                "Cannot replace '{}' with directory", entry.path));
        }
    } else if (entry.typeflag == '2' && std::filesystem::is_directory(status)) {
        std::error_code empty_ec;
        if (!std::filesystem::is_empty(destination, empty_ec) || empty_ec) {
            return std::unexpected(std::format(
                "Cannot replace non-empty directory '{}' with symlink", entry.path));
        }
    } else if (entry.typeflag != '2' && std::filesystem::is_directory(status)) {
        return std::unexpected(std::format(
            "Cannot replace directory '{}' with regular file", entry.path));
    }
    return {};
}
} // namespace sage::archive
