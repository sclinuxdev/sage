module;
#include <fcntl.h>
#include <unistd.h>
export module sage.archive:inspect;

import std;
import sage.vendor.libarchive;
import sage.package;
import sage.service;
import :core;
import :detail;
import :idx;

export namespace sage::archive {

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
} // namespace sage::archive
