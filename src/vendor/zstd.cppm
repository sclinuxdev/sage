module;

#include <zstd.h>

export module sage.vendor.zstd;

import std;

export namespace sage::vendor::zstd {

// One-shot decompression of a whole frame whose uncompressed size is known
// up front (ELF SHF_COMPRESSED sections carry it in their Chdr). Output is
// capped: oversized frames come back truncated with the flag set so callers
// stay honest about completeness.
struct BufferDecompress {
    std::string bytes;
    bool truncated{false};
};

inline std::expected<BufferDecompress, std::string>
decompress_buffer(std::string_view src, size_t uncompressed_size, size_t cap) {
    BufferDecompress out;
    out.truncated = uncompressed_size > cap;
    const auto take = out.truncated ? cap : uncompressed_size;
    out.bytes.resize(take);
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    if (!dctx) return std::unexpected(std::string{"zstd: cannot create DCtx"});
    const size_t rc = ZSTD_decompressDCtx(
        dctx, out.bytes.data(), take, src.data(), src.size());
    ZSTD_freeDCtx(dctx);
    if (ZSTD_isError(rc)) {
        return std::unexpected(std::string("zstd: ") + ZSTD_getErrorName(rc));
    }
    out.bytes.resize(rc);
    return out;
}

class ZstdDecompressStream {
public:
    ZstdDecompressStream() noexcept : dctx_(ZSTD_createDCtx()) {}
    ~ZstdDecompressStream() noexcept {
        if (dctx_) {
            ZSTD_freeDCtx(dctx_);
            dctx_ = nullptr;
        }
    }

    ZstdDecompressStream(const ZstdDecompressStream&) = delete;
    ZstdDecompressStream& operator=(const ZstdDecompressStream&) = delete;

    ZstdDecompressStream(ZstdDecompressStream&& other) noexcept 
        : dctx_(std::exchange(other.dctx_, nullptr)) {}

    ZstdDecompressStream& operator=(ZstdDecompressStream&& other) noexcept {
        if (this != &other) {
            if (dctx_) ZSTD_freeDCtx(dctx_);
            dctx_ = std::exchange(other.dctx_, nullptr);
        }
        return *this;
    }

    void reset() noexcept {
        if (dctx_) {
            ZSTD_DCtx_reset(dctx_, ZSTD_reset_session_only);
        }
    }

    // Decompress streaming chunk
    // Returns remaining bytes to read in frame, or 0 when frame finished
    std::expected<size_t, std::string> decompress_stream(
        ZSTD_inBuffer& in, 
        ZSTD_outBuffer& out) noexcept 
    {
        if (!dctx_) return std::unexpected(std::string{"Uninitialized ZSTD DCtx"});
        size_t ret = ZSTD_decompressStream(dctx_, &out, &in);
        if (ZSTD_isError(ret)) {
            return std::unexpected(std::string{ZSTD_getErrorName(ret)});
        }
        return ret;
    }

    // Decompressed leading slice of the framed stream read from `in`,
    // capped at `max_bytes` for consumers inspecting framed members.
    // Malformed input is an error; the zstd buffer structs stay inside
    // this module instead of leaking past the vendor boundary.
    std::expected<std::string, std::string> decompress_lead(
        std::istream& in, size_t max_bytes) {
        if (!dctx_) return std::unexpected(std::string{"Uninitialized ZSTD DCtx"});
        std::string out(max_bytes, '\0');
        ZSTD_outBuffer out_buf{out.data(), out.size(), 0};
        std::vector<char> chunk(64 << 10);
        bool frame_done = false;
        while (out_buf.pos < out_buf.size && !frame_done) {
            in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            if (in.gcount() <= 0) break;
            ZSTD_inBuffer in_buf{chunk.data(), static_cast<size_t>(in.gcount()), 0};
            while (in_buf.pos < in_buf.size && out_buf.pos < out_buf.size && !frame_done) {
                const size_t ret = ZSTD_decompressStream(dctx_, &out_buf, &in_buf);
                if (ZSTD_isError(ret))
                    return std::unexpected(ZSTD_getErrorName(ret));
                frame_done = ret == 0;
            }
        }
        out.resize(out_buf.pos);
        return out;
    }

    [[nodiscard]] explicit operator bool() const noexcept { return dctx_ != nullptr; }

private:
    ZSTD_DCtx* dctx_{nullptr};
};

class ZstdCompressStream {
public:
    explicit ZstdCompressStream(int level = 3) noexcept 
        : cctx_(ZSTD_createCCtx()), level_(level) 
    {
        if (cctx_) {
            ZSTD_CCtx_setParameter(cctx_, ZSTD_c_compressionLevel, level_);
        }
    }

    ~ZstdCompressStream() noexcept {
        if (cctx_) {
            ZSTD_freeCCtx(cctx_);
            cctx_ = nullptr;
        }
    }

    ZstdCompressStream(const ZstdCompressStream&) = delete;
    ZstdCompressStream& operator=(const ZstdCompressStream&) = delete;

    ZstdCompressStream(ZstdCompressStream&& other) noexcept 
        : cctx_(std::exchange(other.cctx_, nullptr)),
          level_(other.level_) {}

    ZstdCompressStream& operator=(ZstdCompressStream&& other) noexcept {
        if (this != &other) {
            if (cctx_) ZSTD_freeCCtx(cctx_);
            cctx_ = std::exchange(other.cctx_, nullptr);
            level_ = other.level_;
        }
        return *this;
    }

    std::expected<size_t, std::string> compress_stream(
        ZSTD_inBuffer& in, 
        ZSTD_outBuffer& out, 
        ZSTD_EndDirective end_op = ZSTD_e_continue) noexcept 
    {
        if (!cctx_) return std::unexpected(std::string{"Uninitialized ZSTD CCtx"});
        size_t ret = ZSTD_compressStream2(cctx_, &out, &in, end_op);
        if (ZSTD_isError(ret)) {
            return std::unexpected(std::string{ZSTD_getErrorName(ret)});
        }
        return ret;
    }

    [[nodiscard]] explicit operator bool() const noexcept { return cctx_ != nullptr; }

private:
    ZSTD_CCtx* cctx_{nullptr};
    int level_{3};
};

inline std::expected<std::vector<std::uint8_t>, std::string> compress_block(
    std::span<const std::uint8_t> src, 
    int level = 3) 
{
    std::size_t max_dest = ZSTD_compressBound(src.size());
    std::vector<std::uint8_t> dest(max_dest);
    std::size_t written = ZSTD_compress(dest.data(), max_dest, src.data(), src.size(), level);
    if (ZSTD_isError(written)) {
        return std::unexpected(ZSTD_getErrorName(written));
    }
    dest.resize(written);
    return dest;
}

inline std::expected<std::vector<std::uint8_t>, std::string> decompress_block(
    std::span<const std::uint8_t> src) 
{
    unsigned long long const r_size = ZSTD_getFrameContentSize(src.data(), src.size());
    if (r_size == ZSTD_CONTENTSIZE_ERROR || r_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        return std::unexpected(std::string{"Invalid or unknown ZSTD content size"});
    }
    std::vector<std::uint8_t> dest(static_cast<std::size_t>(r_size));
    std::size_t actual = ZSTD_decompress(dest.data(), dest.size(), src.data(), src.size());
    if (ZSTD_isError(actual)) {
        return std::unexpected(ZSTD_getErrorName(actual));
    }
    return dest;
}

} // namespace sage::vendor::zstd
