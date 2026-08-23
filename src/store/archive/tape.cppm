module;

#include <zstd.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>

// USTAR wire format, the streaming archive walker, files.idx codec and
// package inspection.
export module sage.archive:tape;

import std;
import sage.vendor.zstd;
import sage.package;
import sage.service;
import sage.util;

import :detail;

export namespace sage::archive {

using std::size_t;
using std::uint8_t;
using std::uint32_t;
using std::uint64_t;

// ============================================================================
// POSIX USTAR Tar Header (512 bytes)
// ============================================================================

#pragma pack(push, 1)
struct TarHeader {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
};
#pragma pack(pop)

static_assert(sizeof(TarHeader) == 512, "TarHeader must be exactly 512 bytes");

inline uint64_t parse_octal(const char* str, size_t len) noexcept {
    uint64_t val = 0;
    size_t i = 0;
    while (i < len && (str[i] == ' ' || str[i] == '\0')) ++i;
    while (i < len && str[i] >= '0' && str[i] <= '7') {
        val = (val << 3) | (str[i] - '0');
        ++i;
    }
    return val;
}

inline void write_octal(char* dest, size_t len, uint64_t val) noexcept {
    if (len == 0) return;
    dest[len - 1] = '\0';
    size_t i = len - 1;
    if (val == 0) {
        if (i > 0) dest[--i] = '0';
    } else {
        while (i > 0 && val > 0) {
            dest[--i] = static_cast<char>('0' + (val & 7));
            val >>= 3;
        }
    }
    while (i > 0) {
        dest[--i] = '0';
    }
}

inline void write_tar_checksum(char* dest, uint32_t val) noexcept {
    for (size_t i = 6; i > 0; --i) {
        dest[i - 1] = static_cast<char>('0' + (val & 7));
        val >>= 3;
    }
    dest[6] = '\0';
    dest[7] = ' ';
}

struct UstarPathParts {
    std::string_view prefix;
    std::string_view name;
};

inline std::expected<UstarPathParts, std::string> split_ustar_path(std::string_view path) {
    if (path.size() <= 100) {
        return UstarPathParts{{}, path};
    }

    size_t slash = path.rfind('/', std::min(path.size() - 1, size_t{155}));
    while (slash != std::string_view::npos) {
        size_t name_size = path.size() - slash - 1;
        if (slash > 0 && name_size > 0 && name_size <= 100) {
            return UstarPathParts{path.substr(0, slash), path.substr(slash + 1)};
        }
        if (slash == 0) break;
        slash = path.rfind('/', slash - 1);
    }

    return std::unexpected("Path cannot be represented in a POSIX USTAR header: " + std::string(path));
}

inline uint32_t compute_tar_checksum(const TarHeader& hdr) noexcept {
    const auto* p = reinterpret_cast<const uint8_t*>(&hdr);
    uint32_t sum = 0;
    for (size_t i = 0; i < 512; ++i) {
        // Tar chksum field (bytes 148..155) treated as ASCII spaces during calculation
        if (i >= 148 && i < 156) {
            sum += ' ';
        } else {
            sum += p[i];
        }
    }
    return sum;
}

// ============================================================================
// .METADATA/files.idx -- per-file integrity index
// ============================================================================
//
// Tab-separated, path last but one so a path containing whitespace still
// parses: type, mode (octal), size, sha256 ("-" when not applicable), path,
// symlink target ("-" when not a symlink).

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
            fields[idx] = line.substr(fpos, tab - fpos);
            fpos = tab + 1;
        }
        if (idx + 1 != fields.size()) continue; // malformed line

        package::FileEntry fe;
        fe.type = package::parse_file_type(fields[0]);
        fe.mode = static_cast<uint32_t>(parse_octal(fields[1].data(), fields[1].size()));
        fe.size = 0;
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
// Extracted Package Result
// ============================================================================

struct ExtractedPackage {
    package::PackageManifest manifest;
    std::vector<package::FileEntry> extracted_files;
    // What .METADATA/files.idx claimed, when the archive shipped one.
    std::vector<package::FileEntry> declared_files;
};

struct InspectedPackage {
    package::PackageManifest manifest;
    std::vector<package::FileEntry> data_files;
    std::vector<package::FileEntry> declared_files;
    std::string archive_sha256;
    uint64_t source_device{0};
    uint64_t source_inode{0};
    uint64_t source_size{0};
    uint64_t source_mtime_ns{0};
    uint64_t source_ctime_ns{0};
};
struct ArchiveEntryView {
    std::string name;
    uint64_t size{0};
    uint32_t mode{0};
    char typeflag{'0'};
    std::string linkname;
    std::span<const uint8_t> payload;
};

template <typename Handler>
inline std::expected<void, std::string> walk_archive_entries(
    std::istream& file,
    std::string_view archive_name,
    Handler&& handler,
    util::Sha256* archive_hasher = nullptr)
{
    vendor::zstd::ZstdDecompressStream zstd_stream;
    if (!zstd_stream) {
        return std::unexpected(std::string("Failed to initialize ZSTD decompressor"));
    }

    constexpr size_t in_buf_size = 64 * 1024;
    constexpr size_t out_buf_size = 128 * 1024;

    std::vector<uint8_t> in_buffer(in_buf_size);
    std::vector<uint8_t> out_buffer(out_buf_size);

    std::vector<uint8_t> ring;
    ring.reserve(256 * 1024);
    size_t end_zero_blocks = 0;
    bool frame_finished = false;

    auto process_decompressed_bytes = [&](std::span<const uint8_t> chunk) -> std::expected<void, std::string> {
        ring.insert(ring.end(), chunk.begin(), chunk.end());

        while (ring.size() >= 512) {
            const auto* hdr = reinterpret_cast<const TarHeader*>(ring.data());

            // End of archive marker (all zero block)
            bool all_zero = true;
            for (size_t i = 0; i < 512; ++i) {
                if (ring[i] != 0) { all_zero = false; break; }
            }
            if (all_zero) {
                ++end_zero_blocks;
                ring.erase(ring.begin(), ring.begin() + 512);
                continue;
            }
            if (end_zero_blocks > 0) {
                return std::unexpected(std::string("Tar archive contains data after its end marker"));
            }

            // Verify checksum
            uint32_t expected_chk = static_cast<uint32_t>(parse_octal(hdr->chksum, sizeof(hdr->chksum)));
            uint32_t actual_chk = compute_tar_checksum(*hdr);
            if (expected_chk != actual_chk) {
                return std::unexpected(std::string("Tar header checksum mismatch"));
            }

            std::string full_name;
            if (hdr->prefix[0] != '\0') {
                full_name = std::string(hdr->prefix, strnlen(hdr->prefix, sizeof(hdr->prefix))) + "/";
            }
            full_name += std::string(hdr->name, strnlen(hdr->name, sizeof(hdr->name)));

            uint64_t file_size = parse_octal(hdr->size, sizeof(hdr->size));
            uint32_t mode = static_cast<uint32_t>(parse_octal(hdr->mode, sizeof(hdr->mode)));
            char typeflag = hdr->typeflag ? hdr->typeflag : '0';
            std::string linkname(hdr->linkname, strnlen(hdr->linkname, sizeof(hdr->linkname)));

            size_t total_entry_size = 512 + ((file_size + 511) / 512) * 512;
            if (ring.size() < total_entry_size) {
                // Wait for more decompressed stream data
                return {};
            }

            ArchiveEntryView entry{
                .name = std::move(full_name),
                .size = file_size,
                .mode = mode,
                .typeflag = typeflag,
                .linkname = std::move(linkname),
                .payload = std::span<const uint8_t>(ring.data() + 512, file_size),
            };
            auto handle_res = handler(entry);
            if (!handle_res) return handle_res;

            ring.erase(ring.begin(), ring.begin() + total_entry_size);
        }
        return {};
    };

    while (file.read(reinterpret_cast<char*>(in_buffer.data()), in_buf_size) || file.gcount() > 0) {
        size_t read_bytes = static_cast<size_t>(file.gcount());
        if (archive_hasher && read_bytes > 0) {
            archive_hasher->update(in_buffer.data(), read_bytes);
        }
        ZSTD_inBuffer in = { in_buffer.data(), read_bytes, 0 };

        while (in.pos < in.size) {
            ZSTD_outBuffer out = { out_buffer.data(), out_buffer.size(), 0 };
            auto dec_res = zstd_stream.decompress_stream(in, out);
            if (!dec_res) return std::unexpected(dec_res.error());
            frame_finished = *dec_res == 0;

            if (out.pos > 0) {
                auto proc_res = process_decompressed_bytes(std::span<const uint8_t>(out_buffer.data(), out.pos));
                if (!proc_res) return std::unexpected(proc_res.error());
            }
        }
    }

    if (file.bad()) {
        return std::unexpected("Failed while reading package archive: " + std::string(archive_name));
    }
    if (!frame_finished) {
        return std::unexpected("Truncated ZSTD package archive: " + std::string(archive_name));
    }
    if (!ring.empty()) {
        return std::unexpected("Truncated tar block in package archive: " + std::string(archive_name));
    }
    if (end_zero_blocks < 2) {
        return std::unexpected(std::string("Tar archive is missing its end marker"));
    }

    return {};
}

template <typename Handler>
inline std::expected<void, std::string> walk_archive_entries(
    const std::filesystem::path& archive_path,
    Handler&& handler)
{
    std::ifstream file(archive_path, std::ios::binary);
    if (!file.is_open()) {
        return std::unexpected("Cannot open package archive: " + archive_path.string());
    }
    return walk_archive_entries(file, archive_path.string(), std::forward<Handler>(handler));
}
inline std::expected<InspectedPackage, std::string> inspect_package_stream(
    std::istream& archive_file,
    std::string_view archive_name,
    const std::filesystem::path* target_root)
{
    std::string manifest_content;
    std::string service_content;
    std::string triggers_content;
    std::string files_idx_content;
    bool manifest_seen = false;
    bool service_seen = false;
    bool triggers_seen = false;
    bool files_idx_seen = false;
    std::vector<PackageDataEntry> data_entries;
    std::unordered_set<std::string> data_paths;
    std::unordered_map<std::string, char> archive_entry_types;
    util::Sha256 archive_hasher;

    auto walk_res = walk_archive_entries(archive_file, archive_name, [&](const ArchiveEntryView& entry)
        -> std::expected<void, std::string> {
        if (entry.name == ".METADATA/manifest.toml") {
            if (manifest_seen) return std::unexpected(std::string("Package archive contains multiple manifests"));
            if (entry.typeflag != '0') return std::unexpected(std::string("Package manifest must be a regular file"));
            manifest_seen = true;
            manifest_content.assign(
                reinterpret_cast<const char*>(entry.payload.data()), entry.payload.size());
            return {};
        }
        if (entry.name == ".METADATA/service.toml") {
            if (service_seen) return std::unexpected(std::string("Package archive contains multiple service manifests"));
            if (entry.typeflag != '0') return std::unexpected(std::string("Package service manifest must be a regular file"));
            service_seen = true;
            service_content.assign(
                reinterpret_cast<const char*>(entry.payload.data()), entry.payload.size());
            return {};
        }
        if (entry.name == ".METADATA/triggers.toml") {
            if (triggers_seen) return std::unexpected(std::string("Package archive contains multiple trigger manifests"));
            if (entry.typeflag != '0') return std::unexpected(std::string("Package trigger manifest must be a regular file"));
            triggers_seen = true;
            triggers_content.assign(
                reinterpret_cast<const char*>(entry.payload.data()), entry.payload.size());
            return {};
        }
        if (entry.name == ".METADATA/files.idx") {
            if (files_idx_seen) return std::unexpected(std::string("Package archive contains multiple file indexes"));
            if (entry.typeflag != '0') return std::unexpected(std::string("Package file index must be a regular file"));
            files_idx_seen = true;
            files_idx_content.assign(
                reinterpret_cast<const char*>(entry.payload.data()), entry.payload.size());
            return {};
        }
        if (!entry.name.starts_with("data/")) return {};
        if (entry.name.size() == 5) return {};
        if (entry.typeflag != '0' && entry.typeflag != '5' && entry.typeflag != '2') {
            return std::unexpected(std::format(
                "Unsupported tar entry type '{}' for '{}'", entry.typeflag, entry.name));
        }
        auto path_res = normalize_data_path(std::string_view(entry.name).substr(5));
        if (!path_res) return std::unexpected(path_res.error());
        for (const auto& component : std::filesystem::path(*path_res)) {
            if (component.string().starts_with(temp_file_prefix)) {
                return std::unexpected(
                    "Package data path uses reserved temporary-file namespace: " + *path_res);
            }
        }
        if (!data_paths.insert(*path_res).second) {
            return std::unexpected("Package archive contains duplicate data path: " + *path_res);
        }
        std::string sha256;
        if (entry.typeflag == '0') {
            util::Sha256 hasher;
            if (!entry.payload.empty()) {
                hasher.update(entry.payload.data(), entry.payload.size());
            }
            sha256 = hasher.finalize();
        }
        data_entries.push_back(PackageDataEntry{
            .path = *path_res,
            .size = entry.size,
            .mode = entry.mode,
            .typeflag = entry.typeflag,
            .link_target = entry.typeflag == '2' ? std::string(entry.linkname) : std::string{},
            .sha256 = std::move(sha256),
        });
        archive_entry_types.emplace(*path_res, entry.typeflag);
        return {};
    }, &archive_hasher);
    if (!walk_res) return std::unexpected(walk_res.error());

    if (!manifest_seen) {
        return std::unexpected(std::string("Package archive is missing .METADATA/manifest.toml"));
    }
    auto manifest_res = package::PackageManifest::parse_toml(manifest_content);
    if (!manifest_res) {
        return std::unexpected("Failed to parse manifest.toml: " + manifest_res.error());
    }

    InspectedPackage result;
    result.manifest = std::move(*manifest_res);
    result.archive_sha256 = archive_hasher.finalize();

    // A standalone trigger manifest is the package-authoring source of truth
    // and overrides trigger tables embedded in manifest.toml.
    if (triggers_seen) {
        auto trigger_res = package::parse_triggers_toml(triggers_content);
        if (!trigger_res) {
            return std::unexpected("Failed to parse triggers.toml: " + trigger_res.error());
        }
        result.manifest.triggers = std::move(*trigger_res);
    }

    constexpr std::array<std::pair<std::string_view, std::string_view>, 6> usr_merge_aliases{
        std::pair{"bin", "usr/bin"},
        std::pair{"sbin", "usr/bin"},
        std::pair{"lib", "usr/lib"},
        std::pair{"lib64", "usr/lib"},
        std::pair{"usr/sbin", "bin"},
        std::pair{"usr/lib64", "lib"},
    };
    for (const auto& entry : data_entries) {
        if (entry.path.starts_with("usr/sbin/")
            || entry.path.starts_with("usr/lib64/")) {
            return std::unexpected(std::format(
                "Package '{}' must not install payload below compatibility path '{}'",
                result.manifest.name, entry.path));
        }

        auto alias = std::ranges::find_if(
            usr_merge_aliases, [&](const auto& candidate) {
                return entry.path == candidate.first
                    || entry.path.starts_with(std::string(candidate.first) + "/");
            });
        if (alias == usr_merge_aliases.end()) continue;

        const bool is_base_merge_link = result.manifest.name == "base-files"
            && entry.path == alias->first
            && entry.typeflag == '2'
            && entry.link_target == alias->second;
        if (!is_base_merge_link) {
            return std::unexpected(std::format(
                "Package '{}' must use canonical usr/ paths instead of '{}'",
                result.manifest.name, entry.path));
        }
    }

    if (service_seen) {
        auto service_res = service::ServiceSpec::parse_toml(service_content);
        if (!service_res) {
            return std::unexpected("Failed to parse service.toml: " + service_res.error());
        }
        // The manifest is the single carrier: the raw document rides into the
        // LMDB record so `sage rebuild` can regenerate scripts for any init.
        result.manifest.service_toml = std::move(service_content);
    }

    for (const auto& entry : data_entries) {
        package::FileEntry file;
        file.path = entry.path;
        file.size = entry.size;
        file.mode = entry.mode;
        file.sha256 = entry.sha256;
        if (entry.typeflag == '5') {
            file.type = package::FileType::Directory;
        } else if (entry.typeflag == '2') {
            file.type = package::FileType::Symlink;
            file.link_target = entry.link_target;
        }
        result.data_files.push_back(std::move(file));
    }

    // Validate the archive payload against its integrity index before any
    // target-root mutation occurs.
    if (files_idx_seen) {
        result.declared_files = parse_files_idx(files_idx_content);
        std::unordered_map<std::string, const package::FileEntry*> actual;
        for (const auto& file : result.data_files) actual.emplace(file.path, &file);
        for (const auto& declared : result.declared_files) {
            if (declared.type != package::FileType::Regular || declared.sha256.empty()) continue;
            auto actual_it = actual.find(declared.path);
            if (actual_it == actual.end()) {
                return std::unexpected(std::format(
                    "Package integrity failure: files.idx lists '{}' but the archive does not contain it",
                    declared.path));
            }
            if (actual_it->second->sha256 != declared.sha256) {
                return std::unexpected(std::format(
                    "Package integrity failure for '{}': files.idx {}, archive {}",
                    declared.path, declared.sha256, actual_it->second->sha256));
            }
        }
    }

    for (const auto& entry : data_entries) {
        for (auto parent = std::filesystem::path(entry.path).parent_path();
             !parent.empty() && parent != ".";
             parent = parent.parent_path()) {
            auto parent_entry = archive_entry_types.find(parent.generic_string());
            if (parent_entry != archive_entry_types.end() && parent_entry->second != '5') {
                return std::unexpected(std::format(
                    "Package data path '{}' traverses non-directory archive entry '{}'",
                    entry.path, parent.generic_string()));
            }
        }
    }

    if (target_root) {
        std::error_code root_ec;
        auto absolute_root = std::filesystem::absolute(*target_root, root_ec).lexically_normal();
        if (root_ec) {
            return std::unexpected("Cannot resolve target root: " + root_ec.message());
        }
        auto resolved_root = std::filesystem::weakly_canonical(absolute_root, root_ec);
        if (root_ec) {
            return std::unexpected("Cannot resolve target root: " + root_ec.message());
        }
        for (const auto& entry : data_entries) {
            auto path_res = validate_target_path(entry, *target_root, resolved_root);
            if (!path_res) return std::unexpected(path_res.error());
        }
    }

    return result;
}

inline std::expected<InspectedPackage, std::string> inspect_package_impl(
    const std::filesystem::path& archive_path,
    const std::filesystem::path* target_root)
{
    int flags = O_RDONLY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    UniqueFd source(::open(archive_path.c_str(), flags));
    if (source.get() < 0) {
        return std::unexpected("Cannot open package archive: " + archive_path.string());
    }
    struct stat status {};
    if (::fstat(source.get(), &status) != 0 || !S_ISREG(status.st_mode)) {
        return std::unexpected("Cannot inspect package archive identity");
    }
    std::ifstream archive_file(
        std::format("/proc/self/fd/{}", source.get()), std::ios::binary);
    if (!archive_file.is_open()) {
        return std::unexpected("Cannot stream package archive: " + archive_path.string());
    }
    auto inspected = inspect_package_stream(
        archive_file, archive_path.string(), target_root);
    if (!inspected) return inspected;
    inspected->source_device = static_cast<uint64_t>(status.st_dev);
    inspected->source_inode = static_cast<uint64_t>(status.st_ino);
    inspected->source_size = static_cast<uint64_t>(status.st_size);
    inspected->source_mtime_ns = static_cast<uint64_t>(status.st_mtim.tv_sec) * 1'000'000'000
        + static_cast<uint64_t>(status.st_mtim.tv_nsec);
    inspected->source_ctime_ns = static_cast<uint64_t>(status.st_ctim.tv_sec) * 1'000'000'000
        + static_cast<uint64_t>(status.st_ctim.tv_nsec);
    return inspected;
}

inline std::expected<InspectedPackage, std::string> inspect_package(
    const std::filesystem::path& archive_path)
{
    return inspect_package_impl(archive_path, nullptr);
}

inline std::expected<InspectedPackage, std::string> inspect_package(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& target_root)
{
    return inspect_package_impl(archive_path, &target_root);
}
} // namespace sage::archive
