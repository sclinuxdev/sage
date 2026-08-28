module;

#include <archive.h>
#include <archive_entry.h>
#include <sys/stat.h>
#include <clocale>

export module sage.vendor.libarchive;

import std;

export namespace sage::vendor::libarchive {

using std::size_t;
using std::uint32_t;
using std::uint64_t;

// Plain-value description of one tar member. Libarchive types never cross
// this boundary; the business layer reasons about this struct only.
struct EntryInfo {
    std::string pathname{};
    std::string symlink{};
    std::string hardlink{};
    uint64_t size{0};
    uint32_t perm{0644};
    unsigned int filetype{0};
    uint64_t mtime{0};
};

struct WriteEntry {
    std::string_view pathname{};
    std::string_view symlink{};
    std::string_view hardlink{};
    uint64_t size{0};
    uint32_t perm{0644};
    uint32_t uid{0};
    uint32_t gid{0};
    unsigned int filetype{0};
    uint64_t mtime{1700000000};
};

inline constexpr unsigned int type_regular = AE_IFREG;
inline constexpr unsigned int type_directory = AE_IFDIR;
inline constexpr unsigned int type_symlink = AE_IFLNK;

namespace detail {
    inline std::string last_error(::archive* handle) {
        const char* message = ::archive_error_string(handle);
        return message ? std::string(message) : "unknown libarchive error";
    }
}

// Streaming reader over a raw file descriptor. Move-only RAII handle.
class ArchiveReader {
public:
    ArchiveReader() noexcept = default;
    ArchiveReader(const ArchiveReader&) = delete;
    ArchiveReader& operator=(const ArchiveReader&) = delete;

    ArchiveReader(ArchiveReader&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

    ArchiveReader& operator=(ArchiveReader&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    ~ArchiveReader() noexcept { reset(); }

    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

    static std::expected<ArchiveReader, std::string> open_fd(int fd, size_t block_size = 64 * 1024) {
        std::setlocale(LC_ALL, "");
        ArchiveReader reader;
        reader.handle_ = ::archive_read_new();
        if (!reader.handle_) return std::unexpected(std::string{"Cannot allocate libarchive reader"});
        ::archive_read_support_filter_all(reader.handle_);
        ::archive_read_support_format_all(reader.handle_);
        if (::archive_read_open_fd(reader.handle_, fd, block_size) != ARCHIVE_OK) {
            auto error = detail::last_error(reader.handle_);
            reader.reset();
            return std::unexpected(std::move(error));
        }
        return reader;
    }

    // Reads the next member header; nullopt marks a clean end of archive.
    std::expected<std::optional<EntryInfo>, std::string> next_header() {
        ::archive_entry* entry = nullptr;
        int status = ::archive_read_next_header(handle_, &entry);
        if (status == ARCHIVE_EOF) return std::nullopt;
        if (status != ARCHIVE_OK) {
            return std::unexpected(detail::last_error(handle_));
        }
        EntryInfo info;
        const char* name = ::archive_entry_pathname(entry);
        info.pathname = name ? name : "";
        const char* target = ::archive_entry_symlink(entry);
        info.symlink = target ? target : "";
        const char* hlink = ::archive_entry_hardlink(entry);
        info.hardlink = hlink ? hlink : "";
        info.size = static_cast<uint64_t>(::archive_entry_size(entry));
        info.perm = static_cast<uint32_t>(::archive_entry_perm(entry));
        info.filetype = ::archive_entry_filetype(entry);
        info.mtime = static_cast<uint64_t>(::archive_entry_mtime(entry));
        return std::optional{std::move(info)};
    }

    // Reads the current member's payload into `buffer`; returns bytes read.
    std::expected<size_t, std::string> read_data(std::span<uint8_t> buffer) {
        la_ssize_t count = ::archive_read_data(handle_, buffer.data(), buffer.size());
        if (count < 0) return std::unexpected(detail::last_error(handle_));
        return static_cast<size_t>(count);
    }

private:
    void reset() noexcept {
        if (handle_) {
            ::archive_read_close(handle_);
            ::archive_read_free(handle_);
            handle_ = nullptr;
        }
    }
    ::archive* handle_{nullptr};
};

// tar+zstd stream writer producing byte-reproducible package archives.
class ArchiveWriter {
public:
    ArchiveWriter() noexcept = default;
    ArchiveWriter(const ArchiveWriter&) = delete;
    ArchiveWriter& operator=(const ArchiveWriter&) = delete;

    ArchiveWriter(ArchiveWriter&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)),
          header_pending_(std::exchange(other.header_pending_, false)) {}

    ArchiveWriter& operator=(ArchiveWriter&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
            header_pending_ = std::exchange(other.header_pending_, false);
        }
        return *this;
    }

    ~ArchiveWriter() noexcept { reset(); }

    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

    static std::expected<ArchiveWriter, std::string> create(
        const std::filesystem::path& output_path, int compression_level = 9) {
        ArchiveWriter writer;
        writer.handle_ = ::archive_write_new();
        if (!writer.handle_) return std::unexpected(std::string{"Cannot allocate libarchive writer"});
        if (::archive_write_set_format_pax_restricted(writer.handle_) != ARCHIVE_OK
            || ::archive_write_add_filter_zstd(writer.handle_) != ARCHIVE_OK
            || ::archive_write_set_options(
                   writer.handle_, std::format("zstd:compression-level={}", compression_level).c_str())
                != ARCHIVE_OK
            || ::archive_write_open_filename(
                   writer.handle_, output_path.c_str())
                != ARCHIVE_OK) {
            auto error = detail::last_error(writer.handle_);
            writer.reset();
            return std::unexpected(std::move(error));
        }
        return writer;
    }

    std::expected<void, std::string> start_entry(const WriteEntry& meta) {
        if (header_pending_) {
            if (auto res = finish_entry(); !res) return res;
        }
        struct EntryDeleter {
            void operator()(::archive_entry* e) const noexcept { ::archive_entry_free(e); }
        };
        std::unique_ptr<::archive_entry, EntryDeleter> entry(::archive_entry_new());
        if (!entry) return std::unexpected(std::string{"Cannot allocate libarchive entry"});
        auto* raw = entry.get();
        ::archive_entry_set_pathname(raw, std::string(meta.pathname).c_str());
        ::archive_entry_set_perm(raw, static_cast<mode_t>(meta.perm));
        ::archive_entry_set_filetype(raw, meta.filetype);
        ::archive_entry_set_size(raw, static_cast<la_int64_t>(meta.size));
        ::archive_entry_set_mtime(raw, static_cast<la_int64_t>(meta.mtime), 0);
        ::archive_entry_set_uid(raw, static_cast<la_int64_t>(meta.uid));
        ::archive_entry_set_gid(raw, static_cast<la_int64_t>(meta.gid));
        if (!meta.symlink.empty()) {
            ::archive_entry_set_symlink(raw, std::string(meta.symlink).c_str());
        }
        if (!meta.hardlink.empty()) {
            ::archive_entry_set_hardlink(raw, std::string(meta.hardlink).c_str());
        }
        if (::archive_write_header(handle_, entry.get()) != ARCHIVE_OK) {
            return std::unexpected(detail::last_error(handle_));
        }
        header_pending_ = true;
        return {};
    }

    std::expected<void, std::string> write_data(std::span<const uint8_t> bytes) {
        if (bytes.empty()) return {};
        auto written = ::archive_write_data(handle_, bytes.data(), bytes.size());
        if (written < 0 || static_cast<size_t>(written) != bytes.size()) {
            return std::unexpected(detail::last_error(handle_));
        }
        return {};
    }

    std::expected<void, std::string> finish_entry() {
        if (!header_pending_) return {};
        if (::archive_write_finish_entry(handle_) != ARCHIVE_OK) {
            header_pending_ = false;
            return std::unexpected(detail::last_error(handle_));
        }
        header_pending_ = false;
        return {};
    }

    std::expected<void, std::string> finish() {
        if (auto res = finish_entry(); !res) return res;
        if (::archive_write_close(handle_) != ARCHIVE_OK) {
            return std::unexpected(detail::last_error(handle_));
        }
        return {};
    }

private:
    void reset() noexcept {
        if (handle_) {
            (void)::archive_write_close(handle_);
            ::archive_write_free(handle_);
            handle_ = nullptr;
        }
    }
    ::archive* handle_{nullptr};
    bool header_pending_{false};
};

} // namespace sage::vendor::libarchive
