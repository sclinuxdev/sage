module;
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>

export module sage.cli.build:source;

import std;
import sage;
import sage.vendor.libarchive;

namespace sage::cli {

// Source archives are untrusted build inputs.  Use the same libarchive reader
// boundary as package archives instead of shelling out to tar, reject path and
// symlink escapes, and choose a top-level strip only when every member agrees
// on the same directory.  The second pass is intentional: archive readers are
// streaming, so no payload is retained in memory while we inspect the names.
struct SourceArchiveFd {
    int value{-1};
    explicit SourceArchiveFd(int fd) : value(fd) {}
    ~SourceArchiveFd() { if (value >= 0) ::close(value); }
    SourceArchiveFd(const SourceArchiveFd&) = delete;
    SourceArchiveFd& operator=(const SourceArchiveFd&) = delete;
    SourceArchiveFd(SourceArchiveFd&& other) noexcept : value(std::exchange(other.value, -1)) {}
    SourceArchiveFd& operator=(SourceArchiveFd&& other) noexcept {
        if (this != &other) {
            if (value >= 0) ::close(value);
            value = std::exchange(other.value, -1);
        }
        return *this;
    }
};

inline std::expected<void, std::string> extract_source_archive(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination)
{
    const auto open_reader = [&]() -> std::expected<
        std::pair<SourceArchiveFd, sage::vendor::libarchive::ArchiveReader>, std::string> {
        SourceArchiveFd fd{::open(archive_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
        if (fd.value < 0) return std::unexpected(
            "Cannot open source archive: " + archive_path.string());
        auto reader = sage::vendor::libarchive::ArchiveReader::open_fd(fd.value);
        if (!reader) return std::unexpected(reader.error());
        return std::pair<SourceArchiveFd, sage::vendor::libarchive::ArchiveReader>{
            std::move(fd), std::move(*reader)};
    };
    const auto clean_name = [](std::string_view raw)
        -> std::expected<std::filesystem::path, std::string> {
        if (raw.empty()) return std::unexpected("Source archive contains an empty path");
        const std::filesystem::path path{std::string(raw)};
        if (path.is_absolute() || path.has_root_path()
            || std::ranges::any_of(path, [](const auto& part) { return part == ".."; }))
            return std::unexpected("Source archive contains an unsafe path: "
                                   + std::string(raw));
        const auto normalized = path.lexically_normal();
        if (normalized.empty() || normalized == ".")
            return std::unexpected("Source archive contains an empty normalized path");
        return normalized;
    };
    const auto drain = [](sage::vendor::libarchive::ArchiveReader& reader)
        -> std::expected<void, std::string> {
        std::array<std::uint8_t, 64 * 1024> buffer{};
        for (;;) {
            auto count = reader.read_data(buffer);
            if (!count) return std::unexpected(count.error());
            if (*count == 0) break;
        }
        return {};
    };

    auto first = open_reader();
    if (!first) return std::unexpected(first.error());
    std::set<std::string> raw_names;
    std::string common_root;
    bool all_nested = true;
    bool saw_member = false;
    for (;;) {
        auto header = first->second.next_header();
        if (!header) return std::unexpected(header.error());
        if (!*header) break;
        auto name = clean_name((*header)->pathname);
        if (!name) return std::unexpected(name.error());
        const auto text = name->generic_string();
        if (!raw_names.insert(text).second)
            return std::unexpected("Source archive contains duplicate path: " + text);
        saw_member = true;
        const auto slash = text.find('/');
        // Tar writers normally include an explicit top-level directory entry
        // (`project-1.0/`) before its children.  That entry is not evidence
        // of a flat archive; only a root-level non-directory payload disables
        // the common-root strip.
        if (slash == std::string::npos
            && (*header)->filetype != sage::vendor::libarchive::type_directory)
            all_nested = false;
        else {
            const auto root = text.substr(0, slash);
            if (common_root.empty()) common_root = root;
            else if (common_root != root) all_nested = false;
        }
        if ((*header)->filetype == sage::vendor::libarchive::type_regular) {
            if (auto result = drain(first->second); !result)
                return std::unexpected(result.error());
        }
    }
    const bool strip_root = saw_member && all_nested && !common_root.empty();

    auto second = open_reader();
    if (!second) return std::unexpected(second.error());
    std::set<std::string> extracted;
    for (;;) {
        auto header = second->second.next_header();
        if (!header) return std::unexpected(header.error());
        if (!*header) break;
        auto name = clean_name((*header)->pathname);
        if (!name) return std::unexpected(name.error());
        auto relative = name->generic_string();
        if (strip_root) {
            const auto prefix = common_root + "/";
            if (relative == common_root) relative.clear();
            else if (relative.starts_with(prefix)) relative.erase(0, prefix.size());
            else return std::unexpected(
                "Source archive has inconsistent top-level paths");
        }
        if (relative.empty()) {
            if ((*header)->filetype == sage::vendor::libarchive::type_regular) {
                auto result = drain(second->second);
                if (!result) return std::unexpected(
                    "Cannot discard source archive root payload");
            }
            continue;
        }
        const std::filesystem::path out_rel(relative);
        if (out_rel.is_absolute() || out_rel.has_root_path()
            || std::ranges::any_of(out_rel,
                [](const auto& part) { return part == ".."; }))
            return std::unexpected("Source archive path escapes destination: " + relative);
        if (!extracted.insert(relative).second)
            return std::unexpected("Source archive contains duplicate extracted path: " + relative);
        const auto out = destination / out_rel;
        std::error_code ec;
        if ((*header)->filetype == sage::vendor::libarchive::type_directory) {
            std::filesystem::create_directories(out, ec);
            if (ec) return std::unexpected("Cannot create source directory '"
                                           + out.string() + "': " + ec.message());
            std::filesystem::permissions(out,
                static_cast<std::filesystem::perms>((*header)->perm),
                std::filesystem::perm_options::replace, ec);
            if (ec) return std::unexpected("Cannot set source directory mode: "
                                           + ec.message());
        } else if ((*header)->filetype == sage::vendor::libarchive::type_symlink) {
            const std::filesystem::path target((*header)->symlink);
            const auto resolved = out_rel.parent_path() / target;
            if (target.empty() || target.is_absolute() || target.has_root_path()
                || std::ranges::any_of(resolved.lexically_normal(),
                    [](const auto& part) { return part == ".."; }))
                return std::unexpected("Source archive symlink escapes destination: "
                                       + relative);
            std::filesystem::create_directories(out.parent_path(), ec);
            if (ec) return std::unexpected("Cannot create source symlink directory: "
                                           + ec.message());
            std::filesystem::create_symlink((*header)->symlink, out, ec);
            if (ec) return std::unexpected("Cannot create source symlink '"
                                           + out.string() + "': " + ec.message());
        } else if ((*header)->filetype == sage::vendor::libarchive::type_regular) {
            std::filesystem::create_directories(out.parent_path(), ec);
            if (ec) return std::unexpected("Cannot create source file directory: "
                                           + ec.message());
            std::ofstream file(out, std::ios::binary | std::ios::trunc);
            if (!file) return std::unexpected("Cannot create source file: " + out.string());
            std::array<std::uint8_t, 64 * 1024> buffer{};
            std::uint64_t written = 0;
            for (;;) {
                auto count = second->second.read_data(buffer);
                if (!count) return std::unexpected(count.error());
                if (*count == 0) break;
                file.write(reinterpret_cast<const char*>(buffer.data()),
                           static_cast<std::streamsize>(*count));
                if (!file) return std::unexpected("Cannot write source file: " + out.string());
                written += *count;
            }
            if (written != (*header)->size) return std::unexpected(
                "Source archive file size changed while extracting: " + relative);
            file.close();
            std::filesystem::permissions(out,
                static_cast<std::filesystem::perms>((*header)->perm),
                std::filesystem::perm_options::replace, ec);
            if (ec) return std::unexpected("Cannot set source file mode: " + ec.message());
        } else {
            return std::unexpected("Source archive contains unsupported special file: "
                                   + relative);
        }
    }
    return {};
}

} // namespace sage::cli
