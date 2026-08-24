module;

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

export module sage.util:fs;

import std;

export namespace sage::util {

using std::size_t;

inline std::filesystem::path normalize_path(const std::filesystem::path& p) {
    return p.lexically_normal();
}

inline std::string clean_rel_path(std::string_view path) {
    while (path.starts_with('/') || path.starts_with('.')) {
        if (path.starts_with('/')) path.remove_prefix(1);
        else if (path.starts_with("./")) path.remove_prefix(2);
        else break;
    }
    return std::string(path);
}

// Number of components in a relative package path; file removal walks deepest
// first so parent directories empty out and can be pruned.
inline size_t path_depth(std::string_view path) {
    const auto relative = std::filesystem::path(clean_rel_path(path));
    return static_cast<size_t>(std::distance(relative.begin(), relative.end()));
}

// Process environment. The CLI layer stays pure `import sage;`, so the one
// POSIX call it needs (setenv, absent from std) lives here.
inline bool set_env(std::string_view key, std::string_view value) {
    return ::setenv(std::string(key).c_str(), std::string(value).c_str(), 1) == 0;
}

inline const char* get_env(std::string_view key) {
    return std::getenv(std::string(key).c_str());
}

// Same rationale as set_env above.
inline long current_pid() {
    return static_cast<long>(::getpid());
}

struct FileMetadataSnapshot {
    std::uintmax_t size{0};
    std::int64_t mtime_nanoseconds{0};
    std::int64_t ctime_nanoseconds{0};
    std::uint32_t owner_uid{0};
    std::uint32_t owner_gid{0};
    std::uint32_t mode{0};
    bool operator==(const FileMetadataSnapshot&) const = default;
};

inline std::expected<FileMetadataSnapshot, std::string> snapshot_file_metadata(
    const std::filesystem::path& path)
{
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) {
        const int stat_errno = errno;
        return std::unexpected(std::format(
            "cannot stat '{}': {}", path.string(), std::strerror(stat_errno)));
    }
    return FileMetadataSnapshot{
        .size = static_cast<std::uintmax_t>(info.st_size),
        .mtime_nanoseconds = static_cast<std::int64_t>(info.st_mtim.tv_sec) * 1'000'000'000
            + info.st_mtim.tv_nsec,
        .ctime_nanoseconds = static_cast<std::int64_t>(info.st_ctim.tv_sec) * 1'000'000'000
            + info.st_ctim.tv_nsec,
        .owner_uid = static_cast<std::uint32_t>(info.st_uid),
        .owner_gid = static_cast<std::uint32_t>(info.st_gid),
        .mode = static_cast<std::uint32_t>(info.st_mode & 07777),
    };
}

} // namespace sage::util
