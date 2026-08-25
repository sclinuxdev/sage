export module sage.archive:pack;

// Package creation (inventory + hash pass, then one streaming tar+zstd write)
// and local repository index generation.
import std;
import sage.vendor.libarchive;
import sage.package;
import sage.util;
import :core;
import :idx;
import :inspect;

export namespace sage::archive {

using std::size_t;
using std::uint8_t;
using std::uint32_t;
using std::uint64_t;

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
    // files.idx is the canonical, compact payload inventory used by ownership
    // preflight. Duplicating tens of thousands of entries as [[files]] TOML
    // made every install parse the same inventory twice before extraction.
    // Readers still accept legacy full manifests; newly packed archives keep
    // manifest.toml metadata-only and reconstruct installed files from the
    // verified extraction result.
    std::string manifest_toml = final_manifest.serialize_summary_toml();
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
                    .perm = static_cast<uint32_t>(fe.mode & 0100 ? 0755 : 0644),
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
        for (const auto& tool : m.managed_build_tools) {
            ss << "[[packages.managed_build_tools]]\n";
            ss << "role = \"" << quote(tool.role) << "\"\n";
            ss << "executable = \"" << quote(tool.executable) << "\"\n";
            ss << "family = \"" << quote(tool.family) << "\"\n";
            ss << "version = \"" << quote(tool.version) << "\"\n";
            ss << "version_argument = \"--version\"\n";
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
