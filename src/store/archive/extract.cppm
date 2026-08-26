module;
#include <fcntl.h>
#include <sys/stat.h>
#include <cerrno>
#include <unistd.h>
export module sage.archive:extract;


import std;
import sage.package;
import sage.util;
import :core;
import :detail;
import :idx;
import :inspect;

export namespace sage::archive {

using std::size_t;
using std::uint8_t;
using std::uint32_t;
using std::uint64_t;

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

    explicit WritePool(unsigned workers, size_t capacity);

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
// Out-of-line on purpose: GCC 15's module writer segfaults (depset ICE) when
// an exported partition class constructs std::jthread from a lambda inside an
// inline constructor body. Same code, defined after the class; both compilers
// accept it. Revisit when the compiler bug is fixed.
inline WritePool::WritePool(unsigned workers, size_t capacity)
    : capacity_(std::max<size_t>(capacity, workers)) {
    for (unsigned i = 0; i < workers; ++i) {
        workers_.emplace_back([this](std::stop_token stop) { loop(stop); });
    }
}

inline std::expected<void, std::string> write_regular_at(
    int parent_fd, std::string_view rel_path, std::string_view leaf,
    uint32_t mode, uint64_t mtime, std::span<const uint8_t> payload)
{
    const auto leaf_bytes = std::string(leaf);
    int open_flags = O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW;
#ifdef O_CLOEXEC
    open_flags |= O_CLOEXEC;
#endif
    int fd = ::openat(parent_fd, leaf_bytes.c_str(), open_flags, mode & 07777);
    if (fd < 0) {
        // A foreign leaf symlink must never be followed: replace it, once.
        if ((errno == ELOOP || errno == EEXIST)
            && ::unlinkat(parent_fd, leaf_bytes.c_str(), 0) == 0) {
            fd = ::openat(parent_fd, leaf_bytes.c_str(), open_flags, mode & 07777);
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
        // 8 concurrent anchored writers: small-file installs are create/close
        // bound, and NVMe queue depth absorbs far more than CPU count. The
        // decode side stays single-threaded (libarchive single stream).
        std::min<unsigned>(8u, std::max(1u, std::thread::hardware_concurrency())),
        64};
    std::mutex state_mutex;
    std::unordered_map<std::string, char> seen_paths;
    // Staging is private and immutable until pool.drain(). Resolve each parent
    // once on the decode thread and let all worker openat() calls share that
    // descriptor. Header-heavy packages otherwise walk the same directory
    // chain tens of thousands of times.
    std::unordered_map<std::string, detail::UniqueFd> write_parents;
    auto write_parent = [&](std::string_view rel_path)
        -> std::expected<int, std::string> {
        const auto parent_path = std::filesystem::path(rel_path).parent_path();
        const std::string key = parent_path.generic_string();
        if (auto found = write_parents.find(key); found != write_parents.end()) {
            return found->second.get();
        }
        std::vector<std::string> components;
        for (const auto& component : parent_path) {
            if (component != ".") components.push_back(component.string());
        }
        auto opened = detail::open_anchored_dir(root_fd.get(), components);
        if (!opened) return std::unexpected(opened.error());
        const int fd = *opened;
        write_parents.emplace(key, detail::UniqueFd{fd});
        return fd;
    };
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
        const bool is_hardlink = !info->hardlink.empty();
        const char typeflag = is_dir ? '5' : is_symlink ? '2' : is_hardlink ? '1' : info->filetype == vendor::libarchive::type_regular ? '0' : '?';

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
            auto safe_target = normalize_link_target(rel_path, info->symlink);
            if (!safe_target) {
                failed = true;
                pool.drain();
                return std::unexpected(safe_target.error());
            }
            package::FileEntry entry;
            entry.path = rel_path;
            entry.type = package::FileType::Symlink;
            entry.mode = 0777;
            entry.link_target = *safe_target;
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
            if (::symlinkat(safe_target->c_str(), *parent, leaf.c_str()) != 0) {
                failed = true;
                pool.drain();
                return std::unexpected(std::format(
                    "Cannot create symlink '{}' -> '{}': {}",
                    rel_path, info->symlink, std::strerror(errno)));
            }
            detail::close_fd(*parent);
            continue;
        }

        if (is_hardlink) {
            std::string_view htarget = info->hardlink;
            if (htarget.starts_with("data/")) htarget.remove_prefix(5);
            auto safe_target = normalize_data_path(htarget);
            if (!safe_target) {
                failed = true;
                pool.drain();
                return std::unexpected(safe_target.error());
            }
            package::FileEntry entry;
            entry.path = rel_path;
            entry.type = package::FileType::Hardlink;
            entry.mode = (info->perm & 0100) ? 0755 : 0644;
            entry.link_target = *safe_target;
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
            if (::linkat(root_fd.get(), safe_target->c_str(), *parent, leaf.c_str(), 0) != 0) {
                failed = true;
                pool.drain();
                detail::close_fd(*parent);
                return std::unexpected(std::format(
                    "Cannot create hardlink '{}' -> '{}': {}",
                    rel_path, *safe_target, std::strerror(errno)));
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
        auto parent_fd = write_parent(rel_path);
        if (!parent_fd) {
            failed = true;
            pool.drain();
            return std::unexpected(parent_fd.error());
        }

        auto write_task = [parent = *parent_fd, rel_path, write_leaf, mode = entry.mode,
                              mtime = info->mtime, payload = std::move(payload)]()
                -> std::expected<void, std::string> {
            return detail::write_regular_at(
                parent, rel_path, write_leaf, mode, mtime, payload);
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

} // namespace sage::archive
