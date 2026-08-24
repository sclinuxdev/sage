module;

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

export module sage.util:lock;  // process-wide operation lock (flock-based)

import std;

export namespace sage::util {

inline std::uint32_t current_effective_uid() {
    return static_cast<std::uint32_t>(::geteuid());
}

// Contention is the only lock failure a caller can wait out. Open and flock
// failures retain their original cause instead of being reported as a peer.
enum class LockFailure { Busy, Unusable };
enum class LockMode { Shared, Exclusive };

struct LockError {
    LockFailure kind{LockFailure::Unusable};
    std::string message;
};

// Host-wide operation lock. Linux /run is root-owned mode 0755, so an
// unprivileged user cannot pre-create the sage namespace. The namespace and
// lock file remain root-only, and every successful open enters an RAII owner
// before validation or flock.
class OperationLock {
public:
    using Deadline = std::chrono::steady_clock::time_point;
    using FlockCall = std::expected<void, int> (*)(int, int);

    static Deadline deadline_after(int wait_seconds) {
        return std::chrono::steady_clock::now()
            + std::chrono::seconds(std::max(0, wait_seconds));
    }

    static std::expected<OperationLock, LockError> acquire(
        const std::filesystem::path& path,
        LockMode mode,
        int wait_seconds = 0)
    {
        return acquire_until(path, mode, deadline_after(wait_seconds));
    }

    static std::expected<OperationLock, LockError> acquire_until(
        const std::filesystem::path& path,
        LockMode mode,
        Deadline deadline)
    {
        return acquire_until(path, mode, deadline, call_flock);
    }

    // Low-level overload used by the regression suite to inject a fatal flock
    // result and verify that it is not collapsed into contention.
    static std::expected<OperationLock, LockError> acquire_until(
        const std::filesystem::path& path,
        LockMode mode,
        Deadline deadline,
        FlockCall flock_call)
    {
        const auto parent = path.parent_path();
        const auto filename = path.filename();
        if (parent.empty() || filename.empty()) {
            return std::unexpected(LockError{LockFailure::Unusable,
                "operation lock path must name a file inside a directory"});
        }

        bool created_directory = false;
        if (::mkdir(parent.c_str(), 0700) == 0) {
            created_directory = true;
        } else if (errno != EEXIST) {
            const int mkdir_errno = errno;
            return std::unexpected(LockError{LockFailure::Unusable, std::format(
                "cannot create operation lock directory '{}': {}",
                parent.string(), std::strerror(mkdir_errno))});
        }

        OperationLock directory;
        directory.fd_ = ::open(
            parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (directory.fd_ < 0) {
            const int open_errno = errno;
            return std::unexpected(LockError{LockFailure::Unusable, std::format(
                "cannot open operation lock directory '{}': {}",
                parent.string(), std::strerror(open_errno))});
        }
        if (created_directory
            && (::fchown(directory.fd_, 0, 0) != 0
                || ::fchmod(directory.fd_, 0700) != 0)) {
            const int metadata_errno = errno;
            return std::unexpected(LockError{LockFailure::Unusable, std::format(
                "cannot secure operation lock directory '{}': {}",
                parent.string(), std::strerror(metadata_errno))});
        }
        if (auto valid = validate_fd(
                directory.fd_, parent, true, 0700); !valid) {
            return std::unexpected(std::move(valid.error()));
        }

        OperationLock lock;
        lock.fd_ = ::openat(
            directory.fd_, filename.c_str(),
            O_RDONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            0600);
        bool created_file = lock.fd_ >= 0;
        if (lock.fd_ < 0 && errno == EEXIST) {
            lock.fd_ = ::openat(
                directory.fd_, filename.c_str(),
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            created_file = false;
        }
        if (lock.fd_ < 0) {
            const int open_errno = errno;
            return std::unexpected(LockError{LockFailure::Unusable, std::format(
                "cannot open operation lock file '{}': {}",
                path.string(), std::strerror(open_errno))});
        }
        if (created_file
            && (::fchown(lock.fd_, 0, 0) != 0
                || ::fchmod(lock.fd_, 0600) != 0)) {
            const int metadata_errno = errno;
            return std::unexpected(LockError{LockFailure::Unusable, std::format(
                "cannot secure operation lock file '{}': {}",
                path.string(), std::strerror(metadata_errno))});
        }
        if (auto valid = validate_fd(lock.fd_, path, false, 0600); !valid) {
            return std::unexpected(std::move(valid.error()));
        }

        auto try_lock = [&]() -> std::expected<bool, LockError> {
            const int operation = mode == LockMode::Shared ? LOCK_SH : LOCK_EX;
            while (true) {
                auto result = flock_call(lock.fd_, operation | LOCK_NB);
                if (result) return true;
                const int lock_errno = result.error();
                if (lock_errno == EINTR) continue;
                if (lock_errno == EWOULDBLOCK || lock_errno == EAGAIN) return false;
                return std::unexpected(LockError{LockFailure::Unusable, std::format(
                    "cannot lock operation file '{}': {}",
                    path.string(), std::strerror(lock_errno))});
            }
        };

        auto locked = try_lock();
        if (!locked) return std::unexpected(std::move(locked.error()));
        while (!*locked && std::chrono::steady_clock::now() < deadline) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= Deadline::duration::zero()) break;
            std::this_thread::sleep_for(std::min(
                std::chrono::duration_cast<std::chrono::milliseconds>(remaining),
                std::chrono::milliseconds(50)));
            locked = try_lock();
            if (!locked) return std::unexpected(std::move(locked.error()));
        }
        if (!*locked) return std::unexpected(LockError{LockFailure::Busy, {}});
        return lock;
    }

    OperationLock() = default;
    OperationLock(OperationLock&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)) {}
    OperationLock& operator=(OperationLock&& other) noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = std::exchange(other.fd_, -1);
        return *this;
    }
    OperationLock(const OperationLock&) = delete;
    OperationLock& operator=(const OperationLock&) = delete;
    ~OperationLock() { if (fd_ >= 0) ::close(fd_); }

private:
    static std::expected<void, LockError> validate_fd(
        int fd,
        const std::filesystem::path& path,
        bool expect_directory,
        std::uint32_t expected_mode)
    {
        struct stat info {};
        if (::fstat(fd, &info) != 0) {
            const int stat_errno = errno;
            return std::unexpected(LockError{LockFailure::Unusable, std::format(
                "cannot inspect operation lock '{}': {}",
                path.string(), std::strerror(stat_errno))});
        }
        const bool right_type = expect_directory
            ? S_ISDIR(info.st_mode)
            : S_ISREG(info.st_mode);
        const auto actual_mode = static_cast<std::uint32_t>(info.st_mode & 07777);
        if (!right_type
            || info.st_uid != 0
            || info.st_gid != 0
            || actual_mode != expected_mode) {
            return std::unexpected(LockError{LockFailure::Unusable, std::format(
                "operation lock '{}' must be root:root mode {:04o} {}",
                path.string(), expected_mode,
                expect_directory ? "directory" : "regular file")});
        }
        return {};
    }

    static std::expected<void, int> call_flock(int fd, int operation) {
        if (::flock(fd, operation) == 0) return {};
        return std::unexpected(errno);
    }

    int fd_{-1};
};

} // namespace sage::util
