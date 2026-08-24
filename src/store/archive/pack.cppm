module;

#include <zstd.h>
#include <cstring>

// Package creation (*.pkg.tar.zst) and local repository indexing.
export module sage.archive:pack;

import std;
import sage.vendor.zstd;
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

// ============================================================================
// Streaming Tar + Zstd Package Builder (`*.pkg.tar.zst`)
// ============================================================================

inline std::expected<void, std::string> create_package(
    const package::PackageManifest& manifest,
    const std::filesystem::path& data_dir,
    const std::filesystem::path& output_path)
{
    if (auto parent = output_path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    std::ofstream out_file(output_path, std::ios::binary);
    if (!out_file.is_open()) {
        return std::unexpected("Cannot create output package: " + output_path.string());
    }

    vendor::zstd::ZstdCompressStream zstd_stream(9); // Level 9 compression
    if (!zstd_stream) {
        return std::unexpected(std::string("Failed to initialize ZSTD compressor"));
    }

    constexpr size_t out_chunk_size = 128 * 1024;
    std::vector<uint8_t> out_chunk(out_chunk_size);

    auto write_compressed_chunk = [&](std::span<const uint8_t> raw, bool is_end = false) -> std::expected<void, std::string> {
        ZSTD_inBuffer in = { raw.data(), raw.size(), 0 };
        while (in.pos < in.size || is_end) {
            ZSTD_outBuffer out = { out_chunk.data(), out_chunk.size(), 0 };
            auto comp_res = zstd_stream.compress_stream(in, out, is_end ? ZSTD_e_end : ZSTD_e_continue);
            if (!comp_res) return std::unexpected(comp_res.error());

            if (out.pos > 0) {
                out_file.write(reinterpret_cast<const char*>(out_chunk.data()), static_cast<std::streamsize>(out.pos));
            }
            if (is_end && *comp_res == 0) break;
        }
        return {};
    };

    auto append_tar_entry = [&](std::string_view name, std::span<const uint8_t> content, uint32_t mode = 0644, char typeflag = '0', std::string_view linkname = {}) -> std::expected<void, std::string> {
        TarHeader hdr{};
        std::memset(&hdr, 0, sizeof(hdr));

        auto path_parts = split_ustar_path(name);
        if (!path_parts) return std::unexpected(path_parts.error());
        if (!path_parts->prefix.empty()) {
            std::memcpy(hdr.prefix, path_parts->prefix.data(), path_parts->prefix.size());
        }
        if (!path_parts->name.empty()) {
            std::memcpy(hdr.name, path_parts->name.data(), path_parts->name.size());
        }

        write_octal(hdr.mode, sizeof(hdr.mode), mode);
        write_octal(hdr.uid, sizeof(hdr.uid), 0);
        write_octal(hdr.gid, sizeof(hdr.gid), 0);
        write_octal(hdr.size, sizeof(hdr.size), content.size());
        write_octal(hdr.mtime, sizeof(hdr.mtime), 1700000000);
        hdr.typeflag = typeflag;

        if (linkname.size() > sizeof(hdr.linkname)) {
            return std::unexpected("Link target cannot be represented in a POSIX USTAR header: " + std::string(linkname));
        }
        if (!linkname.empty()) {
            std::memcpy(hdr.linkname, linkname.data(), linkname.size());
        }

        std::memcpy(hdr.magic, "ustar", 5);
        std::memcpy(hdr.version, "00", 2);
        std::memcpy(hdr.uname, "root", 4);
        std::memcpy(hdr.gname, "root", 4);

        uint32_t chk = compute_tar_checksum(hdr);
        write_tar_checksum(hdr.chksum, chk);

        // Write header
        auto res = write_compressed_chunk(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&hdr), 512));
        if (!res) return res;

        // Write payload
        if (!content.empty()) {
            res = write_compressed_chunk(content);
            if (!res) return res;

            // Pad to 512-byte boundary
            size_t pad = (512 - (content.size() % 512)) % 512;
            if (pad > 0) {
                static const uint8_t zeros[512] = {0};
                res = write_compressed_chunk(std::span<const uint8_t>(zeros, pad));
                if (!res) return res;
            }
        }
        return {};
    };

    // 1. Inventory the payload before writing anything.
    //
    // The metadata blocks lead the tar stream, and files.idx plus
    // installed_size can only be filled in once every payload file has been
    // walked and hashed -- hence a pass over data/ first. It costs one extra
    // traversal; the file contents are read once either way.
    package::PackageManifest final_manifest = manifest;
    final_manifest.files.clear();
    uint64_t total_size = 0;

    if (std::filesystem::exists(data_dir)) {
        std::vector<std::filesystem::path> payload;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(data_dir, std::filesystem::directory_options::none)) {
            payload.push_back(entry.path());
        }
        // Deterministic ordering: two builds of the same tree must produce the
        // same files.idx, otherwise nothing downstream can compare them.
        std::ranges::sort(payload);

        for (const auto& path : payload) {
            package::FileEntry fe;
            fe.path = path.lexically_relative(data_dir).generic_string();

            if (std::filesystem::is_symlink(path)) {
                fe.type = package::FileType::Symlink;
                fe.mode = 0777;
                std::error_code ec;
                fe.link_target = std::filesystem::read_symlink(path, ec).generic_string();
            } else if (std::filesystem::is_directory(path)) {
                fe.type = package::FileType::Directory;
                fe.mode = 0755;
            } else if (std::filesystem::is_regular_file(path)) {
                fe.type = package::FileType::Regular;
                auto perms = std::filesystem::status(path).permissions();
                fe.mode = ((perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none) ? 0755 : 0644;
                std::error_code ec;
                fe.size = std::filesystem::file_size(path, ec);
                if (ec) fe.size = 0;
                total_size += fe.size;
                if (auto h = util::compute_file_sha256(path)) {
                    fe.sha256 = *h;
                }
            } else {
                continue;
            }
            final_manifest.files.push_back(std::move(fe));
        }
    }
    final_manifest.installed_size = total_size;

    // 2. Append .METADATA/manifest.toml
    std::string manifest_toml = final_manifest.serialize_toml();
    auto m_res = append_tar_entry(".METADATA/manifest.toml", std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(manifest_toml.data()), manifest_toml.size()), 0644);
    if (!m_res) return m_res;

    // 3. Append .METADATA/files.idx
    std::string files_idx = serialize_files_idx(final_manifest.files);
    auto fi_res = append_tar_entry(".METADATA/files.idx", std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(files_idx.data()), files_idx.size()), 0644);
    if (!fi_res) return fi_res;

    // 4. Append .METADATA/triggers.toml if the package declares any
    if (!final_manifest.triggers.empty()) {
        std::string trig_toml = package::serialize_triggers_toml(final_manifest.triggers);
        auto t_res = append_tar_entry(".METADATA/triggers.toml", std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(trig_toml.data()), trig_toml.size()), 0644);
        if (!t_res) return t_res;
    }

    // 5. Append .METADATA/service.toml verbatim when the manifest carries a
    // universal service definition (the manifest itself is the LMDB record;
    // this copy keeps the archive self-describing).
    if (!manifest.service_toml.empty()) {
        const std::string& svc_toml = manifest.service_toml;
        auto s_res = append_tar_entry(".METADATA/service.toml", std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(svc_toml.data()), svc_toml.size()), 0644);
        if (!s_res) return s_res;
    }

    // 3. Append data/... filesystem payload
    if (std::filesystem::exists(data_dir)) {
        std::map<std::string, std::filesystem::directory_entry> entries;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(data_dir, std::filesystem::directory_options::none)) {
            auto rel = entry.path().lexically_relative(data_dir).generic_string();
            entries.emplace(rel, entry);
        }

        for (const auto& [rel, entry] : entries) {
            std::string tar_name = "data/" + rel;

            if (entry.is_symlink()) {
                auto target = std::filesystem::read_symlink(entry.path()).generic_string();
                auto r = append_tar_entry(tar_name, {}, 0777, '2', target);
                if (!r) return r;
            } else if (entry.is_directory()) {
                auto directory_name = tar_name;
                auto path_parts = split_ustar_path(directory_name);
                std::error_code empty_ec;
                if (!path_parts && !std::filesystem::is_empty(entry.path(), empty_ec) && !empty_ec) {
                    continue;
                }
                auto r = append_tar_entry(directory_name, {}, 0755, '5');
                if (!r) return r;
            } else if (entry.is_regular_file()) {
                std::ifstream f(entry.path(), std::ios::binary);
                std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                auto perms = entry.status().permissions();
                uint32_t mode = 0644;
                if ((perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none) {
                    mode = 0755;
                }
                auto r = append_tar_entry(tar_name, data, mode, '0');
                if (!r) return r;
            }
        }
    }

    // 7. Two 512-byte zero blocks marking end of Tar archive
    static const uint8_t end_blocks[1024] = {0};
    auto end_res = write_compressed_chunk(std::span<const uint8_t>(end_blocks, sizeof(end_blocks)));
    if (!end_res) return end_res;

    // 8. Flush and finish ZSTD stream
    return write_compressed_chunk({}, true);
}
// ============================================================================
// Local Repository Index Generator (index.toml)
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

    // Collect all .pkg.tar.zst files recursively, building relative paths
    std::vector<std::pair<std::filesystem::path, std::string>> packages;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             repo_dir,
             std::filesystem::directory_options::skip_permission_denied)) {
        if (entry.is_regular_file() && entry.path().string().ends_with(".pkg.tar.zst")) {
            auto rel_path = entry.path().lexically_relative(repo_dir).generic_string();
            packages.emplace_back(entry.path(), rel_path);
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
        // Build provenance, same omit-if-empty rule as the manifest.
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
        for (const auto& p : m.provides) ss << "    \"" << quote(p) << "\",\n";
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
    out.close();

    return {};
}
} // namespace sage::archive
