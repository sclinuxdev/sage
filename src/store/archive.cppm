module;

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <cerrno>

// libarchive-backed streaming archive engine. One decompression pass per
// operation, direct anchored writes, no install-time hashing: file hashes are
// adopted from the archive's own .METADATA/files.idx (computed at pack time
// with hardware-accelerated SHA-256), exactly like pacman trusts its download
// verification instead of rehashing during extraction.
export module sage.archive;

import std;
import sage.vendor.libarchive;
import sage.package;
import sage.service;
import sage.util;

export namespace sage::archive {

using std::size_t;
using std::uint8_t;
using std::uint32_t;
using std::uint64_t;

inline constexpr std::string_view temp_file_prefix = ".sage-tmp-";
inline constexpr uint64_t canonical_build_epoch = 1700000000;

// ============================================================================
// .METADATA/files.idx -- per-file integrity index
// ============================================================================

inline std::string serialize_files_idx(const std::vector<package::FileEntry>& files) {
    std::ostringstream ss;
    ss << "# sage files index v1\n";
    ss << "# type\tmode\tsize\tsha256\tpath\ttarget\n";
    for (const auto& f : files) {
        ss << package::to_string(f.type) << '\t'
           << std::format("{:o}", f.mode) << '\t'
           << f.size << '\t'
           << (f.sha256.empty() ? "-" : f.sha256) << '\t'
           << f.path << '\t'
           << (f.link_target.empty() ? "-" : f.link_target) << '\n';
    }
    return ss.str();
}

inline std::vector<package::FileEntry> parse_files_idx(std::string_view content) {
    std::vector<package::FileEntry> out;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        if (eol == std::string_view::npos) eol = content.size();
        std::string_view line = content.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty() || line.front() == '#') continue;

        std::array<std::string_view, 6> fields{};
        size_t fpos = 0;
        size_t idx = 0;
        for (; idx < fields.size(); ++idx) {
            if (idx + 1 == fields.size()) {
                fields[idx] = line.substr(fpos);
                break;
            }
            size_t tab = line.find('\t', fpos);
            if (tab == std::string_view::npos) break;
            fields[idx] = line.substr(fpos, tab + 1 - fpos - 1);
            fpos = tab + 1;
        }
        if (idx + 1 != fields.size()) continue; // malformed line

        package::FileEntry fe;
        fe.type = package::parse_file_type(fields[0]);
        fe.mode = static_cast<uint32_t>(std::stoull(std::string(fields[1]), nullptr, 8));
        for (char c : fields[2]) {
            if (c >= '0' && c <= '9') fe.size = fe.size * 10 + static_cast<uint64_t>(c - '0');
        }
        if (fields[3] != "-") fe.sha256 = std::string(fields[3]);
        fe.path = std::string(fields[4]);
        if (fields[5] != "-") fe.link_target = std::string(fields[5]);
        if (!fe.path.empty()) out.push_back(std::move(fe));
    }
    return out;
}

// ============================================================================
// Anchored path vocabulary -- never follow a symlink outside the target root
// ============================================================================

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

// ============================================================================
// Inspection results
// ============================================================================

struct ExtractedPackage {
    package::PackageManifest manifest;
    std::vector<package::FileEntry> extracted_files;
    std::vector<package::FileEntry> declared_files;
};

struct InspectedPackage {
    package::PackageManifest manifest;
    std::vector<package::FileEntry> data_files;
    std::vector<package::FileEntry> declared_files;
    uint64_t source_device{0};
    uint64_t source_inode{0};
    uint64_t source_size{0};
    uint64_t source_mtime_ns{0};
    uint64_t source_ctime_ns{0};
};

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

// ============================================================================
// Package inspection -- reads only the leading .METADATA section, then stops.
// The metadata blocks lead every sage archive, so inspection cost is constant
// regardless of payload size. No whole-archive hashing happens here.
// ============================================================================

namespace detail {

struct MetadataContents {
    std::string manifest;
    std::string service;
    std::string triggers;
    std::string files_idx;
};

inline std::expected<std::string, std::string> read_member_payload(
    vendor::libarchive::ArchiveReader& reader, uint64_t size)
{
    std::string content(size, '\0');
    size_t done = 0;
    while (done < content.size()) {
        auto chunk = reader.read_data(
            std::span<uint8_t>(reinterpret_cast<uint8_t*>(content.data()) + done,
                content.size() - done));
        if (!chunk) return std::unexpected(chunk.error());
        if (*chunk == 0) break;
        done += *chunk;
    }
    return content;
}

// Consumes `info`'s payload when it names a .METADATA member; returns false
// for anything else (payload untouched, member still "current" downstream).
inline std::expected<bool, std::string> capture_metadata_member(
    vendor::libarchive::ArchiveReader& reader,
    const vendor::libarchive::EntryInfo& info,
    MetadataContents& out)
{
    const bool is_metadata = info.pathname == ".METADATA/manifest.toml"
        || info.pathname == ".METADATA/service.toml"
        || info.pathname == ".METADATA/triggers.toml"
        || info.pathname == ".METADATA/files.idx";
    if (!is_metadata || info.filetype != vendor::libarchive::type_regular) {
        return false;
    }
    auto content = read_member_payload(reader, info.size);
    if (!content) return std::unexpected(content.error());
    if (info.pathname == ".METADATA/manifest.toml") {
        if (!out.manifest.empty()) {
            return std::unexpected(std::string("Package archive contains multiple manifests"));
        }
        out.manifest = std::move(*content);
    } else if (info.pathname == ".METADATA/service.toml") {
        out.service = std::move(*content);
    } else if (info.pathname == ".METADATA/triggers.toml") {
        out.triggers = std::move(*content);
    } else {
        out.files_idx = std::move(*content);
    }
    return true;
}

inline std::expected<package::PackageManifest, std::string> finalize_manifest(
    MetadataContents& meta)
{
    auto manifest_res = package::PackageManifest::parse_toml(meta.manifest);
    if (!manifest_res) {
        return std::unexpected("Failed to parse manifest.toml: " + manifest_res.error());
    }
    package::PackageManifest manifest = std::move(*manifest_res);
    if (!meta.triggers.empty()) {
        auto trigger_res = package::parse_triggers_toml(meta.triggers);
        if (!trigger_res) {
            return std::unexpected("Failed to parse triggers.toml: " + trigger_res.error());
        }
        manifest.triggers = std::move(*trigger_res);
    }
    if (!meta.service.empty()) {
        auto service_res = service::ServiceSpec::parse_toml(meta.service);
        if (!service_res) {
            return std::unexpected("Failed to parse service.toml: " + service_res.error());
        }
        manifest.service_toml = std::move(meta.service);
    }
    return manifest;
}

} // namespace detail

inline std::expected<InspectedPackage, std::string> inspect_package(
    const std::filesystem::path& archive_path,
    const std::filesystem::path* target_root = nullptr)
{
    auto identity = detail::SourceIdentity::of(archive_path);
    if (!identity) return std::unexpected(identity.error());

    int flags = O_RDONLY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    detail::UniqueFd fd(::open(archive_path.c_str(), flags));
    if (fd.get() < 0) {
        return std::unexpected("Cannot open package archive: " + archive_path.string());
    }
    auto reader_res = vendor::libarchive::ArchiveReader::open_fd(fd.get());
    if (!reader_res) return std::unexpected(reader_res.error());
    auto& reader = *reader_res;

    detail::MetadataContents meta;
    while (true) {
        auto header = reader.next_header();
        if (!header) return std::unexpected(header.error());
        if (!header->has_value()) break; // clean end of archive
        auto captured = detail::capture_metadata_member(reader, **header, meta);
        if (!captured) return std::unexpected(captured.error());
        if (*captured) continue;      // was a .METADATA member
        break;                        // first payload member: section over
    }
    if (meta.manifest.empty()) {
        return std::unexpected(std::string("Package archive is missing .METADATA/manifest.toml"));
    }

    InspectedPackage result;
    auto manifest_res = detail::finalize_manifest(meta);
    if (!manifest_res) return std::unexpected(manifest_res.error());
    result.manifest = std::move(*manifest_res);
    result.source_device = identity->device;
    result.source_inode = identity->inode;
    result.source_size = identity->size;
    result.source_mtime_ns = identity->mtime_ns;
    result.source_ctime_ns = identity->ctime_ns;

    if (!meta.files_idx.empty()) {
        result.declared_files = parse_files_idx(meta.files_idx);
        result.data_files = result.declared_files;
    }
    for (const auto& file : result.data_files) {
        auto check = detail::validate_payload_path(
            file.path,
            file.type == package::FileType::Directory ? '5'
                : file.type == package::FileType::Symlink ? '2' : '0',
            file.link_target,
            result.manifest.name);
        if (!check) return std::unexpected(check.error());
    }

    (void)target_root;
    return result;
}

// ============================================================================
// Extraction -- one decompression pass, parallel anchored writes, zero fsync
// ============================================================================

enum class ExtractionDurability {
    Immediate,
    Batch,
};

namespace detail {

// Bounded worker pool with backpressure: the decode thread stays ahead of the
// disk by at most `capacity` queued payloads.
class WritePool {
public:
    using Task = std::function<std::expected<void, std::string>()>;

    explicit WritePool(unsigned workers, size_t capacity)
        : capacity_(std::max<size_t>(capacity, workers)) {
        for (unsigned i = 0; i < workers; ++i) {
            workers_.emplace_back([this](std::stop_token stop) { loop(stop); });
        }
    }

    WritePool(const WritePool&) = delete;
    WritePool& operator=(const WritePool&) = delete;

    ~WritePool() {
        for (auto& worker : workers_) worker.request_stop();
        {
            std::lock_guard lock(mutex_);
            drained_ = true;
        }
        available_.notify_all();
        finished_.notify_all();
    }

    void submit(Task task) {
        std::unique_lock lock(mutex_);
        available_.wait(lock, [&] { return queue_.size() < capacity_ || drained_; });
        queue_.push(std::move(task));
        finished_.notify_one();
    }

    // Blocks until every submitted task completed; returns the first error.
    std::expected<void, std::string> drain() {
        std::unique_lock lock(mutex_);
        finished_.wait(lock, [&] { return queue_.empty() && busy_ == 0; });
        drained_ = true;
        available_.notify_all();
        if (first_error_) {
            auto error = std::move(*first_error_);
            first_error_.reset();
            return std::unexpected(error);
        }
        return {};
    }

private:
    void loop(std::stop_token stop) {
        std::unique_lock lock(mutex_);
        while (!stop.stop_requested()) {
            finished_.wait(lock, [&] {
                return !queue_.empty() || (queue_.empty() && busy_ == 0 && stop.stop_requested());
            });
            if (queue_.empty()) return;
            auto task = std::move(queue_.front());
            queue_.pop();
            ++busy_;
            available_.notify_one();
            lock.unlock();

            auto outcome = task();

            lock.lock();
            --busy_;
            if (!outcome && !first_error_) first_error_ = std::move(outcome).error();
            finished_.notify_all();
        }
    }

    std::vector<std::jthread> workers_;
    std::queue<Task> queue_;
    std::mutex mutex_;
    std::condition_variable available_;
    std::condition_variable finished_;
    std::optional<std::string> first_error_;
    size_t capacity_;
    size_t busy_{0};
    bool drained_{false};
};

inline std::expected<void, std::string> write_regular_anchored(
    int root_fd, std::string_view rel_path, std::string_view leaf,
    uint32_t mode, uint64_t mtime, std::span<const uint8_t> payload)
{
    const std::filesystem::path relative(rel_path);
    std::vector<std::string> components;
    for (const auto& component : relative.parent_path()) {
        if (component == ".") continue;
        components.push_back(component.string());
    }
    auto parent = open_anchored_dir(root_fd, components);
    if (!parent) return std::unexpected(parent.error());

    const auto leaf_bytes = std::string(leaf);
    int open_flags = O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW;
#ifdef O_CLOEXEC
    open_flags |= O_CLOEXEC;
#endif
    int fd = ::openat(*parent, leaf_bytes.c_str(), open_flags, mode & 07777);
    if (fd < 0) {
        // A foreign leaf symlink must never be followed: replace it, once.
        if ((errno == ELOOP || errno == EEXIST)
            && ::unlinkat(*parent, leaf_bytes.c_str(), 0) == 0) {
            fd = ::openat(*parent, leaf_bytes.c_str(), open_flags, mode & 07777);
        }
        if (fd < 0) {
            return std::unexpected(std::format(
                "Cannot install '{}': {}", rel_path, std::strerror(errno)));
        }
    }
    UniqueFd guard(fd);

    size_t done = 0;
    while (done < payload.size()) {
        ssize_t written = ::write(guard.get(), payload.data() + done, payload.size() - done);
        if (written <= 0) {
            if (written < 0 && errno == EINTR) continue;
            return std::unexpected(std::format(
                "Cannot write '{}': {}", rel_path, std::strerror(errno)));
        }
        done += static_cast<size_t>(written);
    }
    if (::fchmod(guard.get(), mode & 07777) != 0) {
        return std::unexpected(std::format(
            "Cannot set mode on '{}': {}", rel_path, std::strerror(errno)));
    }
    // Restore the archive's (deterministic) timestamp; durability still
    // rides on kernel writeback.
    const struct timespec times[2] = {
        {static_cast<time_t>(mtime), 0}, {static_cast<time_t>(mtime), 0}};
    if (::futimens(guard.get(), times) != 0) {
        return std::unexpected(std::format(
            "Cannot set mtime on '{}': {}", rel_path, std::strerror(errno)));
    }
    return {}; // durability rides on kernel writeback, like pacman
}

inline std::expected<void, std::string> sync_extracted_root(
    const std::filesystem::path& target_root)
{
    int flags = O_RDONLY | O_DIRECTORY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    detail::UniqueFd root(::open(target_root.c_str(), flags));
    if (root.get() < 0) {
        return std::unexpected("Cannot open target root for durability sync");
    }
#ifdef __linux__
    if (::syncfs(root.get()) != 0) {
        return std::unexpected(std::string("Cannot sync installed package: ") + std::strerror(errno));
    }
#endif
    return {};
}

} // namespace detail

// Batch installs leave the shared install-info index to a post-transaction
// trigger; serial upgrades overwrite it directly.
inline bool batch_skips_info_dir(
    ExtractionDurability durability, std::string_view clean_path)
{
    return durability == ExtractionDurability::Batch
        && (clean_path == "usr/share/info/dir" || clean_path.ends_with("/info/dir"));
}

inline std::expected<ExtractedPackage, std::string> extract_package(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& target_root,
    const package::PackageManifest* expected_manifest = nullptr,
    const InspectedPackage* expected_inspection = nullptr,
    const package::PackageManifest* previous_manifest = nullptr,
    ExtractionDurability durability = ExtractionDurability::Immediate)
{
    auto identity = detail::SourceIdentity::of(archive_path);
    if (!identity) return std::unexpected(identity.error());
    if (expected_inspection
        && (identity->device != expected_inspection->source_device
            || identity->inode != expected_inspection->source_inode
            || identity->size != expected_inspection->source_size
            || identity->mtime_ns != expected_inspection->source_mtime_ns)) {
        return std::unexpected("Package archive changed after ownership preflight");
    }

    // Open and locate the metadata section first: manifest parsing must gate
    // any filesystem mutation.
    int flags = O_RDONLY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    detail::UniqueFd fd(::open(archive_path.c_str(), flags));
    if (fd.get() < 0) {
        return std::unexpected("Cannot open package archive: " + archive_path.string());
    }
    auto reader_res = vendor::libarchive::ArchiveReader::open_fd(fd.get());
    if (!reader_res) return std::unexpected(reader_res.error());

    ExtractedPackage result;
    detail::MetadataContents meta;
    bool manifest_parsed = expected_inspection != nullptr;
    if (expected_inspection) {
        result.manifest = expected_inspection->manifest;
        result.declared_files = expected_inspection->declared_files;
    }

    // Deferred until the leading manifest.toml has been captured and parsed:
    // nothing touches the filesystem before the manifest gates it.
    std::optional<std::string> package_name;
    std::unordered_map<std::string, std::string> declared_hashes;
    auto build_hash_map = [&] {
        for (const auto& file : result.declared_files) {
            if (file.type == package::FileType::Regular && !file.sha256.empty()) {
                declared_hashes.emplace(util::clean_rel_path(file.path), file.sha256);
            }
        }
    };
    if (manifest_parsed) {
        build_hash_map();
        package_name = result.manifest.name;
    }
    auto adopt_manifest = [&]() -> std::expected<void, std::string> {
        auto manifest_res = detail::finalize_manifest(meta);
        if (!manifest_res) return std::unexpected(manifest_res.error());
        result.manifest = std::move(*manifest_res);
        result.declared_files = parse_files_idx(meta.files_idx);
        build_hash_map();
        package_name = result.manifest.name;
        if (expected_manifest
            && package::package_identity(result.manifest)
                != package::package_identity(*expected_manifest)) {
            return std::unexpected(std::format(
                "Package archive identity mismatch: selected {} {} [{}; {}], archive contains {} {} [{};{}]",
                expected_manifest->name, expected_manifest->version.to_string(),
                expected_manifest->arch, expected_manifest->channel,
                result.manifest.name, result.manifest.version.to_string(),
                result.manifest.arch, result.manifest.channel));
        }
        return {};
    };

    std::filesystem::create_directories(target_root);
    detail::UniqueFd root_fd(detail::open_root_fd(target_root));
    if (root_fd.get() < 0) {
        return std::unexpected(
            "Cannot securely open target root: " + std::string(std::strerror(errno)));
    }

    detail::WritePool pool{
        std::min<unsigned>(4u, std::max(1u, std::thread::hardware_concurrency())),
        64};
    std::mutex state_mutex;
    std::unordered_map<std::string, char> seen_paths;
    bool failed = false;

    auto record_entry = [&](package::FileEntry entry, std::string_view rel_raw,
                            std::string_view leaf_name, std::string* redirect_leaf) {
        if (previous_manifest && !result.manifest.conffiles.empty()
            && conffile_modified(target_root, rel_raw, result.manifest.conffiles,
                previous_manifest)) {
            util::log_warn("Protecting modified config '{}': new version saved as '{}.new'",
                rel_raw, leaf_name);
            entry.path = std::string(entry.path) + ".new";
            *redirect_leaf = std::string(leaf_name) + ".new";
        }
        result.extracted_files.push_back(std::move(entry));
    };

    // Single decompression pass: leading .METADATA members are captured
    // inline, then every data member streams straight through the pipeline.
    std::optional<vendor::libarchive::EntryInfo> info;
    while (!failed) {
        auto header = reader_res->next_header();
        if (!header) {
            failed = true;
            pool.drain();
            return std::unexpected(header.error());
        }
        if (!header->has_value()) break;
        info = std::move(**header);

        auto captured = detail::capture_metadata_member(*reader_res, *info, meta);
        if (!captured) {
            failed = true;
            pool.drain();
            return std::unexpected(captured.error());
        }
        if (*captured) {
            if (!manifest_parsed && !meta.manifest.empty()) {
                auto adopted = adopt_manifest();
                if (!adopted) {
                    failed = true;
                    pool.drain();
                    return std::unexpected(adopted.error());
                }
                manifest_parsed = true;
            }
            continue;
        }
        if (!info->pathname.starts_with("data/") || info->pathname.size() == 5) continue;
        if (!manifest_parsed) {
            failed = true;
            pool.drain();
            return std::unexpected("Package archive carries payload before .METADATA/manifest.toml");
        }

        auto normalized = normalize_data_path(
            std::string_view(info->pathname).substr(5));
        if (!normalized) {
            failed = true;
            pool.drain();
            return std::unexpected(normalized.error());
        }
        std::string rel_path = std::move(*normalized);
        const auto clean_path = util::clean_rel_path(rel_path);
        const bool is_dir = info->filetype == vendor::libarchive::type_directory;
        const bool is_symlink = info->filetype == vendor::libarchive::type_symlink;
        const char typeflag = is_dir ? '5' : is_symlink ? '2' : info->filetype == vendor::libarchive::type_regular ? '0' : '?';

        if (typeflag == '?') {
            failed = true;
            pool.drain();
            return std::unexpected(std::format(
                "Unsupported tar entry type '{}' for '{}'", typeflag, info->pathname));
        }
        if (batch_skips_info_dir(durability, clean_path)) {
            continue;
        }
        auto check = detail::validate_payload_path(clean_path, typeflag,
            is_symlink ? std::string_view(info->symlink) : std::string_view{}, *package_name);
        if (!check) {
            failed = true;
            pool.drain();
            return std::unexpected(check.error());
        }
        {
            std::lock_guard lock(state_mutex);
            if (!seen_paths.emplace(clean_path, typeflag).second) {
                failed = true;
                pool.drain();
                return std::unexpected(
                    "Package archive contains duplicate data path: " + clean_path);
            }
            // A path whose ancestor was already seen as a non-directory
            // member can never materialize; reject it structurally.
            for (auto parent = std::filesystem::path(clean_path).parent_path();
                 !parent.empty() && parent != ".";
                 parent = parent.parent_path()) {
                auto ancestor = seen_paths.find(parent.generic_string());
                if (ancestor != seen_paths.end() && ancestor->second != '5') {
                    failed = true;
                    pool.drain();
                    return std::unexpected(std::format(
                        "Package data path '{}' traverses non-directory archive entry '{}'",
                        clean_path, ancestor->first));
                }
            }
        }

        if (is_dir) {
            package::FileEntry entry;
            entry.path = rel_path;
            entry.type = package::FileType::Directory;
            entry.mode = 0755;
            result.extracted_files.push_back(std::move(entry)); // dirs are cheap; create inline
            const auto components_view = std::filesystem::path(rel_path);
            std::vector<std::string> components;
            for (const auto& component : components_view.parent_path()) {
                if (component != ".") components.push_back(component.string());
            }
            auto parent = detail::open_anchored_dir(root_fd.get(), components);
            if (!parent) {
                failed = true;
                pool.drain();
                return std::unexpected(parent.error());
            }
            if (::mkdirat(*parent, components_view.filename().c_str(), 0755) != 0
                && errno != EEXIST) {
                failed = true;
                pool.drain();
                return std::unexpected(std::format(
                    "Cannot create directory '{}': {}", rel_path, std::strerror(errno)));
            }
            detail::close_fd(*parent);
            continue;
        }

        if (is_symlink) {
            package::FileEntry entry;
            entry.path = rel_path;
            entry.type = package::FileType::Symlink;
            entry.mode = 0777;
            entry.link_target = info->symlink;
            std::string redirect_leaf;
            record_entry(std::move(entry), rel_path, std::filesystem::path(rel_path).filename().string(), &redirect_leaf);
            const auto leaf = redirect_leaf.empty()
                ? std::filesystem::path(rel_path).filename().string() : redirect_leaf;
            const auto components_view = std::filesystem::path(rel_path);
            std::vector<std::string> components;
            for (const auto& component : components_view.parent_path()) {
                if (component != ".") components.push_back(component.string());
            }
            auto parent = detail::open_anchored_dir(root_fd.get(), components);
            if (!parent) {
                failed = true;
                pool.drain();
                return std::unexpected(parent.error());
            }
            (void)::unlinkat(*parent, leaf.c_str(), 0);
            if (::symlinkat(info->symlink.c_str(), *parent, leaf.c_str()) != 0) {
                failed = true;
                pool.drain();
                return std::unexpected(std::format(
                    "Cannot create symlink '{}' -> '{}': {}",
                    rel_path, info->symlink, std::strerror(errno)));
            }
            detail::close_fd(*parent);
            continue;
        }

        // Regular file: buffer payload, adopt recorded hash, dispatch write.
        std::vector<uint8_t> payload(info->size, 0);
        size_t done = 0;
        while (done < payload.size()) {
            auto chunk = reader_res->read_data(
                std::span<uint8_t>(payload.data() + done, payload.size() - done));
            if (!chunk) {
                failed = true;
                pool.drain();
                return std::unexpected(chunk.error());
            }
            if (*chunk == 0) break;
            done += *chunk;
        }
        if (done != payload.size()) {
            failed = true;
            pool.drain();
            return std::unexpected("Truncated member payload in package archive");
        }

        package::FileEntry entry;
        entry.path = rel_path;
        entry.type = package::FileType::Regular;
        entry.mode = (info->perm & 0100) ? 0755 : 0644;
        entry.size = payload.size();
        entry.sha256 = declared_hashes.contains(clean_path)
            ? declared_hashes.at(clean_path) : std::string{};

        std::string redirect_leaf;
        std::string leaf = std::filesystem::path(rel_path).filename().string();
        record_entry(entry, rel_path, leaf, &redirect_leaf);
        const auto write_leaf = redirect_leaf.empty() ? leaf : redirect_leaf;

        auto write_task = [root = root_fd.get(), rel_path, write_leaf, mode = entry.mode,
                              mtime = info->mtime, payload = std::move(payload)]()
                -> std::expected<void, std::string> {
            return detail::write_regular_anchored(root, rel_path, write_leaf, mode, mtime, payload);
        };
        pool.submit(std::move(write_task));
    }

    auto drained = pool.drain();
    if (!drained) return std::unexpected(drained.error());
    if (!manifest_parsed) {
        return std::unexpected(std::string("Package archive is missing .METADATA/manifest.toml"));
    }

    result.manifest.files = result.extracted_files;
    if (durability == ExtractionDurability::Immediate) {
        if (auto sync_res = detail::sync_extracted_root(target_root); !sync_res) {
            return std::unexpected(sync_res.error());
        }
    }
    return result;
}


// ============================================================================
// Package creation -- inventory + hash pass, then one streaming tar+zstd write
// ============================================================================

namespace detail {

// Simple atomic-index parallel_for over [0, n).
template <typename Fn>
inline void parallel_for(size_t n, Fn&& fn) {
    const unsigned lanes = std::min<size_t>(
        std::max(1u, std::thread::hardware_concurrency()), std::max<size_t>(n, 1));
    if (lanes <= 1 || n <= 1) {
        for (size_t i = 0; i < n; ++i) fn(i);
        return;
    }
    // Plain size_t + __atomic_fetch_add: GCC's modules implementation loses
    // the always_inline body of std::atomic's fetch_add across a module
    // import, which fails instantiation of this template in test binaries.
    size_t cursor{0};
    std::vector<std::jthread> lanes_threads;
    for (unsigned lane = 0; lane < lanes; ++lane) {
        lanes_threads.emplace_back([&] {
            while (true) {
                const auto index = __atomic_fetch_add(
                    &cursor, static_cast<size_t>(1), __ATOMIC_RELAXED);
                if (index >= n) return;
                fn(index);
            }
        });
    }
}

} // namespace detail

inline std::expected<void, std::string> create_package(
    const package::PackageManifest& manifest,
    const std::filesystem::path& data_dir,
    const std::filesystem::path& output_path)
{
    namespace libarchive = vendor::libarchive;

    if (auto parent = output_path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    // 1. Deterministic payload inventory: sorted relative paths, normalized
    // modes, hardware-accelerated SHA-256 computed in parallel.
    package::PackageManifest final_manifest = manifest;
    final_manifest.files.clear();
    uint64_t total_size = 0;

    struct PayloadItem {
        std::string tar_name;
        std::filesystem::path disk_path;
        package::FileEntry entry;
    };
    std::vector<PayloadItem> payload;

    if (std::filesystem::exists(data_dir)) {
        for (const auto& item : std::filesystem::recursive_directory_iterator(data_dir)) {
            package::FileEntry fe;
            fe.path = item.path().lexically_relative(data_dir).generic_string();
            std::error_code ec;
            if (item.is_symlink(ec)) {
                fe.type = package::FileType::Symlink;
                fe.mode = 0777;
                fe.link_target = std::filesystem::read_symlink(item.path(), ec).generic_string();
            } else if (item.is_directory(ec)) {
                fe.type = package::FileType::Directory;
                fe.mode = 0755;
            } else if (item.is_regular_file(ec)) {
                fe.type = package::FileType::Regular;
                auto perms = item.status(ec).permissions();
                fe.mode = ((perms & std::filesystem::perms::owner_exec)
                        != std::filesystem::perms::none) ? 0755 : 0644;
                fe.size = item.file_size(ec);
            } else {
                continue;
            }
            payload.push_back(PayloadItem{
                .tar_name = "data/" + fe.path,
                .disk_path = item.path(),
                .entry = std::move(fe),
            });
        }
        std::ranges::sort(payload, {}, [](const PayloadItem& item) { return item.tar_name; });

        // Parallel hashing: the expensive part of packaging, fully parallel.
        std::vector<std::string> hashes(payload.size());
        detail::parallel_for(payload.size(), [&](size_t index) {
            const auto& item = payload[index];
            if (item.entry.type == package::FileType::Regular && item.entry.size > 0) {
                if (auto hash = util::compute_file_sha256(item.disk_path)) {
                    hashes[index] = std::move(*hash);
                }
            }
        });

        std::string manifest_toml;
        for (size_t index = 0; index < payload.size(); ++index) {
            payload[index].entry.sha256 = std::move(hashes[index]);
            total_size += payload[index].entry.size;
            final_manifest.files.push_back(std::move(payload[index].entry));
        }
    }
    final_manifest.installed_size = total_size;

    auto writer_res = libarchive::ArchiveWriter::create(output_path);
    if (!writer_res) return std::unexpected(writer_res.error());

    auto add_file_entry = [&](std::string_view name,
                              std::span<const uint8_t> bytes) -> std::expected<void, std::string> {
        if (auto res = writer_res->start_entry(libarchive::WriteEntry{
                .pathname = name, .size = bytes.size(),
                .perm = 0644, .filetype = libarchive::type_regular}); !res) return res;
        if (auto res = writer_res->write_data(bytes); !res) return res;
        return writer_res->finish_entry();
    };

    // 2. Metadata section must lead the archive: install-time inspection stops
    // after reading exactly these members.
    final_manifest.installed_size = total_size;
    std::string manifest_toml = final_manifest.serialize_toml();
    if (auto res = add_file_entry(".METADATA/manifest.toml",
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(manifest_toml.data()),
                manifest_toml.size())); !res) return res;

    std::string files_idx = serialize_files_idx(final_manifest.files);
    if (auto res = add_file_entry(".METADATA/files.idx",
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(files_idx.data()),
                files_idx.size())); !res) return res;

    if (!final_manifest.triggers.empty()) {
        std::string triggers_toml = package::serialize_triggers_toml(final_manifest.triggers);
        if (auto res = add_file_entry(".METADATA/triggers.toml",
                std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(triggers_toml.data()),
                    triggers_toml.size())); !res) return res;
    }
    if (!manifest.service_toml.empty()) {
        if (auto res = add_file_entry(".METADATA/service.toml",
                std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(manifest.service_toml.data()),
                    manifest.service_toml.size())); !res) return res;
    }

    // 3. Payload members. The files list was moved out of `payload` entries, so
    // stream from disk using the recorded metadata.
    for (size_t index = 0; index < payload.size(); ++index) {
        const auto& fe = final_manifest.files[index];
        const auto& item = payload[index];
        if (fe.type == package::FileType::Symlink) {
            if (auto res = writer_res->start_entry(libarchive::WriteEntry{
                    .pathname = item.tar_name, .symlink = fe.link_target,
                    .perm = 0777, .filetype = libarchive::type_symlink}); !res) return res;
            if (auto res = writer_res->finish_entry(); !res) return res;
        } else if (fe.type == package::FileType::Directory) {
            if (auto res = writer_res->start_entry(libarchive::WriteEntry{
                    .pathname = item.tar_name, .perm = 0755,
                    .filetype = libarchive::type_directory}); !res) return res;
            if (auto res = writer_res->finish_entry(); !res) return res;
        } else {
            if (auto res = writer_res->start_entry(libarchive::WriteEntry{
                    .pathname = item.tar_name, .size = fe.size,
                    .perm = fe.mode & 0100 ? 0755 : 0644,
                    .filetype = libarchive::type_regular}); !res) return res;
            std::ifstream input(item.disk_path, std::ios::binary);
            if (!input.is_open()) {
                return std::unexpected("Cannot read payload file: " + item.disk_path.string());
            }
            std::array<uint8_t, 256 * 1024> buffer{};
            while (input.read(reinterpret_cast<char*>(buffer.data()), buffer.size())
                   || input.gcount() > 0) {
                if (auto res = writer_res->write_data(
                        std::span<const uint8_t>(buffer.data(),
                            static_cast<size_t>(input.gcount()))); !res) return res;
                if (input.eof()) break;
            }
            if (auto res = writer_res->finish_entry(); !res) return res;
        }
    }

    return writer_res->finish();
}

// ============================================================================
// Local repository index generator (index.toml)
// ============================================================================

inline std::expected<void, std::string> generate_repo_index(
    const std::filesystem::path& repo_dir,
    std::string_view channel_name = "core")
{
    if (!std::filesystem::exists(repo_dir)) {
        return std::unexpected("Repository directory does not exist: " + repo_dir.string());
    }

    std::ostringstream ss;
    const auto quote = [](std::string_view value) {
        return package::escape_toml_basic_string(value);
    };
    ss << "schema_version = 1\n\n";
    ss << "[channel]\n";
    ss << "name = \"" << quote(channel_name) << "\"\n";
    ss << "updated_at = \"" << "2026-08-20T00:00:00Z" << "\"\n\n";

    std::vector<std::pair<std::filesystem::path, std::string>> packages;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             repo_dir, std::filesystem::directory_options::skip_permission_denied)) {
        if (entry.is_regular_file() && entry.path().string().ends_with(".pkg.tar.zst")) {
            packages.emplace_back(
                entry.path(), entry.path().lexically_relative(repo_dir).generic_string());
        }
    }
    std::ranges::sort(packages, {}, [](const auto& package) -> const std::string& {
        return package.second;
    });

    for (const auto& [abs_path, rel_path] : packages) {
        auto inspect_res = inspect_package(abs_path);
        if (!inspect_res) {
            return std::unexpected(std::format(
                "Cannot index package '{}': {}", rel_path, inspect_res.error()));
        }
        const auto& m = inspect_res->manifest;
        ss << "[[packages]]\n";
        ss << "name = \"" << quote(m.name) << "\"\n";
        ss << "version = \"" << quote(m.version.ver) << "\"\n";
        ss << "release = \"" << quote(m.version.rel) << "\"\n";
        if (m.version.epoch > 0) ss << "epoch = " << m.version.epoch << "\n";
        ss << "description = \"" << quote(m.description) << "\"\n";
        ss << "license = \"" << quote(m.license) << "\"\n";
        ss << "channel = \"" << quote(m.channel) << "\"\n";
        ss << "arch = \"" << quote(m.arch) << "\"\n";
        ss << "installed_size = " << m.installed_size << "\n";
        ss << "file = \"" << quote(rel_path) << "\"\n";
        if (!m.build_compiler.empty()) ss << "build_compiler = \"" << quote(m.build_compiler) << "\"\n";
        if (!m.build_compiler_version.empty()) ss << "build_compiler_version = \"" << quote(m.build_compiler_version) << "\"\n";
        if (!m.build_cflags.empty()) ss << "build_cflags = \"" << quote(m.build_cflags) << "\"\n";
        if (!m.build_cxxflags.empty()) ss << "build_cxxflags = \"" << quote(m.build_cxxflags) << "\"\n";
        if (!m.build_ldflags.empty()) ss << "build_ldflags = \"" << quote(m.build_ldflags) << "\"\n";
        if (!m.build_rustflags.empty()) ss << "build_rustflags = \"" << quote(m.build_rustflags) << "\"\n";
        ss << "dependencies = [\n";
        for (const auto& d : m.dependencies) ss << "    \"" << quote(d.to_string()) << "\",\n";
        ss << "]\n";
        ss << "provides = [\n";
        for (const auto& prov : m.provides) ss << "    \"" << quote(prov) << "\",\n";
        ss << "]\n";
        if (!m.conffiles.empty()) {
            ss << "conffiles = [\n";
            for (const auto& c : m.conffiles) ss << "    \"" << quote(c) << "\",\n";
            ss << "]\n";
        }
        ss << "\n";
    }

    std::ofstream out(repo_dir / "index.toml");
    if (!out.is_open()) {
        return std::unexpected("Cannot write " + (repo_dir / "index.toml").string());
    }
    out << ss.str();
    return {};
}

} // namespace sage::archive
