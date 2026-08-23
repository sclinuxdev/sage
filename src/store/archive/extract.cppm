module;

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <cerrno>

// Mutations on the target root: anchored cleanup and streaming extraction.
export module sage.archive:extract;

import std;
import sage.package;
import sage.service;
import sage.util;

import :detail;
import :tape;

export namespace sage::archive {

using std::size_t;
using std::uint8_t;
using std::uint32_t;
using std::uint64_t;

// Bootstrap-era registry claims may reference compatibility paths that only
// exist as base-files merge symlinks on a collapsed layout. Rewriting the
// claim onto its canonical target lets stale cleanup delete at the real
// location without ever opening through a link -- anything else that turns
// out to be a symlink mid-walk stays a hard error.
inline std::expected<std::string, bool> canonicalize_merge_claim(std::string_view path)
{
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 6> aliases{{
        {"usr/sbin", "usr/bin"},
        {"usr/lib64", "usr/lib"},
        {"sbin", "usr/bin"},
        {"lib64", "usr/lib"},
        {"bin", "usr/bin"},
        {"lib", "usr/lib"},
    }};
    for (const auto& [from, to] : aliases) {
        // A claim naming exactly a merge point maps to nothing deletable.
        if (path == from) return std::unexpected(true);
        const auto prefix = std::format("{}/", from);
        if (path.starts_with(prefix))
            return std::format("{}/{}", to, path.substr(prefix.size()));
    }
    return std::string{path};
}

inline std::expected<void, std::string> remove_path_anchored(
    const std::filesystem::path& target_root,
    std::string_view raw_path,
    bool ignore_nonempty_directory = false)
{
    auto normalized = normalize_data_path(raw_path);
    if (!normalized) return std::unexpected(normalized.error());

    // A claim naming exactly a merge point cannot be deleted through its
    // symlink; release it untouched.
    auto canonical = canonicalize_merge_claim(*normalized);
    if (!canonical.has_value())
        return {};
    if (*canonical != *normalized)
        sage::util::log_info(
            "  ~ claim '{}' predates the usr merge; cleaning as '{}'",
            raw_path, *canonical);

    int root_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    root_flags |= O_CLOEXEC;
#endif
    int root_fd = ::open(target_root.c_str(), root_flags);
    if (root_fd < 0) {
        return std::unexpected(
            "Cannot securely open target root: " + std::string(std::strerror(errno)));
    }
    UniqueFd current(root_fd);
    const auto relative = std::filesystem::path(*canonical);
    for (const auto& component : relative.parent_path()) {
        if (component == ".") continue;
        const auto name = component.string();
        int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        int next = ::openat(current.get(), name.c_str(), flags);
        if (next < 0 && errno == ENOENT) return {};
        if (next < 0) {
            return std::unexpected(std::format(
                "Cannot securely open parent directory '{}': {}",
                name, std::strerror(errno)));
        }
        current = UniqueFd(next);
    }

    const auto leaf = relative.filename().string();
    struct stat status {};
    if (::fstatat(current.get(), leaf.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return {};
        return std::unexpected(std::strerror(errno));
    }
    const bool is_directory = S_ISDIR(status.st_mode);
    if (::unlinkat(current.get(), leaf.c_str(), is_directory ? AT_REMOVEDIR : 0) != 0) {
        if (ignore_nonempty_directory && is_directory
            && (errno == ENOTEMPTY || errno == EEXIST)) {
            return {};
        }
        return std::unexpected(std::strerror(errno));
    }
    if (::fsync(current.get()) != 0) {
        return std::unexpected(
            "Cannot sync parent after removing path: "
            + std::string(std::strerror(errno)));
    }
    return {};
}
// ============================================================================
// Streaming Tar + Zstd Extractor (Extracts directly to target root)
// ============================================================================

enum class ExtractionDurability {
    Immediate,
    Batch,
};

// A declared conffile whose on-disk contents no longer match what the
// previous package generation recorded must not be overwritten: the admin
// edited it. True when `rel_path` exists under `target_root`, is declared by
// `conffiles`, and differs from the previous record's hash -- or when the file
// is on disk but was never recorded (foreign content wins, we stand aside).
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

    const auto disk_path = target_root / key;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(disk_path, ec)) return false;
    auto disk_hash = util::compute_file_sha256(disk_path);
    if (!disk_hash) return true; // unreadable -> assume local changes

    for (const auto& f : previous->files) {
        if (util::clean_rel_path(f.path) != key) continue;
        return f.sha256 != *disk_hash;
    }
    return true;
}

inline std::expected<ExtractedPackage, std::string> extract_package(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& target_root,
    const package::PackageManifest* expected_manifest = nullptr,
    const InspectedPackage* expected_inspection = nullptr,
    const package::PackageManifest* previous_manifest = nullptr,
    ExtractionDurability durability = ExtractionDurability::Immediate)
{
    const auto expected_source = expected_inspection
        ? std::optional{std::array<uint64_t, 5>{
              expected_inspection->source_device,
              expected_inspection->source_inode,
              expected_inspection->source_size,
              expected_inspection->source_mtime_ns,
              expected_inspection->source_ctime_ns,
          }}
        : std::nullopt;
    auto snapshot_res = PrivateArchiveSnapshot::create(
        archive_path, target_root / "var/lib/sage/tmp", expected_source);
    if (!snapshot_res) return std::unexpected(snapshot_res.error());
    auto snapshot = std::move(*snapshot_res);

    std::ifstream archive_file(snapshot.path(), std::ios::binary);
    if (!archive_file.is_open()) {
        return std::unexpected(
            "Cannot open private archive snapshot for: " + archive_path.string());
    }
    const auto archive_name = archive_path.string();
    ExtractedPackage result;
    if (expected_inspection) {
        result.manifest = expected_inspection->manifest;
        result.declared_files = expected_inspection->declared_files;
    } else {
        auto inspect_res = inspect_package_stream(archive_file, archive_name, &target_root);
        if (!inspect_res) return std::unexpected(inspect_res.error());
        result.manifest = std::move(inspect_res->manifest);
        result.declared_files = std::move(inspect_res->declared_files);
    }
    if (expected_manifest
        && package::package_identity(result.manifest)
            != package::package_identity(*expected_manifest)) {
        return std::unexpected(std::format(
            "Package archive identity mismatch: selected {} {} [{}; {}], archive contains {} {} [{}; {}]",
            expected_manifest->name, expected_manifest->version.to_string(),
            expected_manifest->arch, expected_manifest->channel,
            result.manifest.name, result.manifest.version.to_string(),
            result.manifest.arch, result.manifest.channel));
    }

    std::error_code root_ec;
    std::filesystem::create_directories(target_root, root_ec);
    if (root_ec) {
        return std::unexpected(
            "Cannot create target root: " + root_ec.message());
    }
    int root_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    root_flags |= O_CLOEXEC;
#endif
    int root_fd = ::open(target_root.c_str(), root_flags);
    if (root_fd < 0) {
        return std::unexpected(
            "Cannot securely open target root: " + std::string(std::strerror(errno)));
    }
    UniqueFd extraction_root(root_fd);

    if (!expected_inspection) {
        archive_file.clear();
        archive_file.seekg(0);
        if (!archive_file) {
            return std::unexpected("Cannot rewind package archive: " + archive_name);
        }
    }
    const bool durable = durability == ExtractionDurability::Immediate;
    util::Sha256 archive_hasher;
    auto extract_res = walk_archive_entries(archive_file, archive_name, [&](const ArchiveEntryView& archive_entry)
        -> std::expected<void, std::string> {
        if (!archive_entry.name.starts_with("data/")) return {};

        if (archive_entry.name.size() == 5) return {};
        auto path_res = normalize_data_path(std::string_view(archive_entry.name).substr(5));
        if (!path_res) return std::unexpected(path_res.error());
        std::string rel_path = std::move(*path_res);
        const auto clean_path = util::clean_rel_path(rel_path);
        if (!durable
            && (clean_path == "usr/share/info/dir" || clean_path.ends_with("/info/dir"))) {
            return {};
        }
        auto destination = open_anchored_parent(
            extraction_root.get(), std::filesystem::path(rel_path), durable);
        if (!destination) {
            return std::unexpected(std::format(
                "Cannot open parent directory for '{}': {}", rel_path, destination.error()));
        }

        package::FileEntry entry;
        entry.path = rel_path;
        entry.mode = archive_entry.mode;
        entry.size = archive_entry.size;

        if (archive_entry.typeflag == '5') {
            entry.type = package::FileType::Directory;
            auto directory_res = ensure_anchored_directory(
                destination->directory.get(), destination->leaf, durable);
            if (!directory_res) {
                return std::unexpected(std::format(
                    "Cannot create directory '{}': {}", rel_path, directory_res.error()));
            }
        } else if (archive_entry.typeflag == '2') {
            entry.type = package::FileType::Symlink;
            entry.link_target = archive_entry.linkname;
            auto remove_res = remove_anchored_leaf(
                destination->directory.get(), destination->leaf, durable);
            if (!remove_res) {
                return std::unexpected(std::format(
                    "Cannot replace '{}' with symlink: {}", rel_path, remove_res.error()));
            }
            const auto link_target = std::string(archive_entry.linkname);
            if (::symlinkat(
                    link_target.c_str(), destination->directory.get(), destination->leaf.c_str()) != 0) {
                return std::unexpected(std::format(
                    "Cannot create symlink '{}' -> '{}': {}",
                    rel_path, archive_entry.linkname, std::strerror(errno)));
            }
            if (durable && ::fsync(destination->directory.get()) != 0) {
                return std::unexpected(std::format(
                    "Cannot sync parent after creating symlink '{}': {}",
                    rel_path, std::strerror(errno)));
            }
        } else {
            entry.type = package::FileType::Regular;
            bool redirected_config = false;
            // Conffile protection compares against the archive's own manifest
            // declaration, not any solver-side copy: the archive is ground
            // truth for what this payload considers configuration.
            if (previous_manifest && !result.manifest.conffiles.empty()
                && conffile_modified(target_root, rel_path, result.manifest.conffiles, previous_manifest)) {
                redirected_config = true;
                destination->leaf += ".new";
                util::log_warn("Protecting modified config '{}': new version saved as '{}'",
                    rel_path, destination->leaf);
            }
            const auto destination_name = destination->leaf;
            auto temp_res = UniqueTempFile::create(std::move(destination->directory));
            if (!temp_res) {
                return std::unexpected(std::format(
                    "Cannot create temporary file for '{}': {}", rel_path, temp_res.error()));
            }
            auto write_res = temp_res->write_all(archive_entry.payload);
            if (!write_res) {
                return std::unexpected(std::format(
                    "Cannot write '{}': {}", rel_path, write_res.error()));
            }
            auto install_res = temp_res->install(
                destination_name, archive_entry.mode, durable);
            if (!install_res) {
                return std::unexpected(std::format(
                    "Cannot install '{}': {}", rel_path, install_res.error()));
            }

            util::Sha256 hasher;
            if (!archive_entry.payload.empty()) {
                hasher.update(archive_entry.payload.data(), archive_entry.payload.size());
            }
            entry.sha256 = hasher.finalize();

            // A protected conffile was redirected: ownership records must name
            // the path we actually wrote, so removal and verify stay honest.
            if (redirected_config) entry.path = rel_path + ".new";
        }

        result.extracted_files.push_back(std::move(entry));
        return {};
    }, expected_inspection ? &archive_hasher : nullptr);
    if (!extract_res) return std::unexpected(extract_res.error());
    if (expected_inspection
        && archive_hasher.finalize() != expected_inspection->archive_sha256) {
        return std::unexpected("Package archive changed after ownership preflight");
    }

    result.manifest.files = result.extracted_files;

    return result;
}

inline std::expected<void, std::string> sync_extracted_root(
    const std::filesystem::path& target_root)
{
    int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    UniqueFd root(::open(target_root.c_str(), flags));
    if (root.get() < 0) {
        return std::unexpected(
            "Cannot open target root for durability sync: "
            + std::string(std::strerror(errno)));
    }
#ifdef __linux__
    if (::syncfs(root.get()) != 0) {
        return std::unexpected(
            "Cannot sync installed package batch: "
            + std::string(std::strerror(errno)));
    }
#else
    if (::fsync(root.get()) != 0) {
        return std::unexpected(
            "Cannot sync installed package batch: "
            + std::string(std::strerror(errno)));
    }
#endif
    return {};
}
} // namespace sage::archive
