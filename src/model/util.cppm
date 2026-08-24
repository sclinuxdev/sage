module;

#include <elf.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

export module sage.util;

import std;
export import :lock;



export namespace sage::util {

using std::uint8_t;
using std::uint32_t;
using std::uint64_t;
using std::size_t;

// ============================================================================
// ANSI Styling & Terminal Output
// ============================================================================

namespace color {
    inline constexpr std::string_view reset   = "\033[0m";
    inline constexpr std::string_view bold    = "\033[1m";
    inline constexpr std::string_view dim     = "\033[2m";
    inline constexpr std::string_view red     = "\033[31m";
    inline constexpr std::string_view green   = "\033[32m";
    inline constexpr std::string_view yellow  = "\033[33m";
    inline constexpr std::string_view blue    = "\033[34m";
    inline constexpr std::string_view magenta = "\033[35m";
    inline constexpr std::string_view cyan    = "\033[36m";
    inline constexpr std::string_view white   = "\033[37m";
}

template <typename... Args>
inline void log_info(std::format_string<Args...> fmt, Args&&... args) {
    std::string msg = std::format(fmt, std::forward<Args>(args)...);
    std::println("{}{}::{}{}", color::cyan, color::bold, color::reset, msg);
}

template <typename... Args>
inline void log_success(std::format_string<Args...> fmt, Args&&... args) {
    std::string msg = std::format(fmt, std::forward<Args>(args)...);
    std::println("{}{}✓{}{}", color::green, color::bold, color::reset, msg);
}

template <typename... Args>
inline void log_warn(std::format_string<Args...> fmt, Args&&... args) {
    std::string msg = std::format(fmt, std::forward<Args>(args)...);
    std::println(std::cerr, "{}{}warning:{}{}", color::yellow, color::bold, color::reset, msg);
}

template <typename... Args>
inline void log_error(std::format_string<Args...> fmt, Args&&... args) {
    std::string msg = std::format(fmt, std::forward<Args>(args)...);
    std::println(std::cerr, "{}{}error:{}{}", color::red, color::bold, color::reset, msg);
}

// ============================================================================
// String Utilities
// ============================================================================

inline std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

inline std::vector<std::string_view> split(std::string_view s, char delim) {
    std::vector<std::string_view> result;
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find(delim, start);
        if (end == std::string_view::npos) {
            result.push_back(s.substr(start));
            break;
        }
        result.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    return result;
}

inline std::string join(const std::vector<std::string>& list, std::string_view delimiter) {
    if (list.empty()) return {};
    std::string res = list[0];
    for (size_t i = 1; i < list.size(); ++i) {
        res += delimiter;
        res += list[i];
    }
    return res;
}

// ============================================================================
// Path Utilities
// ============================================================================

// Shell-style glob: '*' any run, '?' one character, '[abc]' / '[!abc]' a set.
// Iterative with a single backtrack point, so a pattern full of stars cannot
// turn a package listing into an exponential walk.
inline bool glob_match(std::string_view pattern, std::string_view text) noexcept {
    std::size_t p = 0, t = 0;
    std::size_t star = std::string_view::npos, star_t = 0;

    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '[') {
            std::size_t close = pattern.find(']', p + 2);
            if (close == std::string_view::npos) return false;  // unterminated set: no match
            std::string_view set = pattern.substr(p + 1, close - p - 1);
            bool negate = !set.empty() && (set[0] == '!' || set[0] == '^');
            if (negate) set.remove_prefix(1);

            bool hit = false;
            for (std::size_t i = 0; i < set.size(); ++i) {
                if (i + 2 < set.size() && set[i + 1] == '-') {
                    if (text[t] >= set[i] && text[t] <= set[i + 2]) hit = true;
                    i += 2;
                } else if (set[i] == text[t]) {
                    hit = true;
                }
            }
            if (hit != negate) { p = close + 1; ++t; continue; }
        } else if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p; ++t;
            continue;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            star_t = t;
            continue;
        }

        if (star == std::string_view::npos) return false;
        p = star + 1;
        t = ++star_t;
    }

    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

// Human-readable byte count. Binary units, because that is what a filesystem
// reports back when someone goes to check.
inline std::string format_size(std::uintmax_t bytes) {
    constexpr std::string_view units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    auto value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0 ? std::format("{} {}", bytes, units[0])
                     : std::format("{:.1f} {}", value, units[unit]);
}

inline std::filesystem::path normalize_path(const std::filesystem::path& p) {
    return p.lexically_normal();
}

inline std::string clean_rel_path(std::string_view path) {
    while (path.starts_with('/') || path.starts_with('.')) {
        if (path.starts_with('/')) path.remove_prefix(1);
        else if (path.starts_with("./")) path.remove_prefix(2);
        else break;
    }
    return std::string(path);
}

// Number of components in a relative package path; file removal walks deepest
// first so parent directories empty out and can be pruned.
inline size_t path_depth(std::string_view path) {
    const auto relative = std::filesystem::path(clean_rel_path(path));
    return static_cast<size_t>(std::distance(relative.begin(), relative.end()));
}

// Process environment. The CLI layer stays pure `import sage;`, so the one
// POSIX call it needs (setenv, absent from std) lives here.
inline bool set_env(std::string_view key, std::string_view value) {
    return ::setenv(std::string(key).c_str(), std::string(value).c_str(), 1) == 0;
}

inline const char* get_env(std::string_view key) {
    return std::getenv(std::string(key).c_str());
}

// Same rationale as set_env above.
inline long current_pid() {
    return static_cast<long>(::getpid());
}

struct FileMetadataSnapshot {
    std::uintmax_t size{0};
    std::int64_t mtime_nanoseconds{0};
    std::int64_t ctime_nanoseconds{0};
    std::uint32_t owner_uid{0};
    std::uint32_t owner_gid{0};
    std::uint32_t mode{0};
    bool operator==(const FileMetadataSnapshot&) const = default;
};

inline std::expected<FileMetadataSnapshot, std::string> snapshot_file_metadata(
    const std::filesystem::path& path)
{
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) {
        const int stat_errno = errno;
        return std::unexpected(std::format(
            "cannot stat '{}': {}", path.string(), std::strerror(stat_errno)));
    }
    return FileMetadataSnapshot{
        .size = static_cast<std::uintmax_t>(info.st_size),
        .mtime_nanoseconds = static_cast<std::int64_t>(info.st_mtim.tv_sec) * 1'000'000'000
            + info.st_mtim.tv_nsec,
        .ctime_nanoseconds = static_cast<std::int64_t>(info.st_ctim.tv_sec) * 1'000'000'000
            + info.st_ctim.tv_nsec,
        .owner_uid = static_cast<std::uint32_t>(info.st_uid),
        .owner_gid = static_cast<std::uint32_t>(info.st_gid),
        .mode = static_cast<std::uint32_t>(info.st_mode & 07777),
    };
}


// ============================================================================
// Native Zero-Copy ELF SONAME / DT_NEEDED Scanner
// ============================================================================

struct ElfMetadata {
    std::string soname;
    std::vector<std::string> needed;
    bool is_shared{false};
    bool is_executable{false};
};

inline std::expected<ElfMetadata, std::string> scan_elf(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::unexpected("Cannot open file: " + path.string());
    }

    unsigned char e_ident[EI_NIDENT];
    if (!file.read(reinterpret_cast<char*>(e_ident), EI_NIDENT)) {
        return std::unexpected("File too small for ELF header");
    }

    if (e_ident[EI_MAG0] != ELFMAG0 || e_ident[EI_MAG1] != ELFMAG1 ||
        e_ident[EI_MAG2] != ELFMAG2 || e_ident[EI_MAG3] != ELFMAG3) {
        return std::unexpected("Not an ELF binary");
    }

    bool is_64 = (e_ident[EI_CLASS] == ELFCLASS64);
    file.seekg(0, std::ios::beg);

    ElfMetadata meta;

    if (is_64) {
        Elf64_Ehdr ehdr;
        if (!file.read(reinterpret_cast<char*>(&ehdr), sizeof(ehdr))) {
            return std::unexpected("Failed to read ELF64 header");
        }

        meta.is_shared = (ehdr.e_type == ET_DYN);
        meta.is_executable = (ehdr.e_type == ET_EXEC || ehdr.e_type == ET_DYN);

        // Read Program Headers to find PT_DYNAMIC
        std::vector<Elf64_Phdr> phdrs(ehdr.e_phnum);
        file.seekg(static_cast<std::streamoff>(ehdr.e_phoff), std::ios::beg);
        if (!file.read(reinterpret_cast<char*>(phdrs.data()), ehdr.e_phnum * sizeof(Elf64_Phdr))) {
            return meta; // No program headers or static
        }

        const Elf64_Phdr* dyn_phdr = nullptr;
        for (const auto& ph : phdrs) {
            if (ph.p_type == PT_DYNAMIC) {
                dyn_phdr = &ph;
                break;
            }
        }

        if (!dyn_phdr) return meta; // Statically linked

        size_t num_dyn = dyn_phdr->p_filesz / sizeof(Elf64_Dyn);
        std::vector<Elf64_Dyn> dyns(num_dyn);
        file.seekg(static_cast<std::streamoff>(dyn_phdr->p_offset), std::ios::beg);
        if (!file.read(reinterpret_cast<char*>(dyns.data()), dyn_phdr->p_filesz)) {
            return meta;
        }

        uint64_t strtab_vaddr = 0;
        uint64_t strsz = 0;
        std::vector<uint64_t> needed_offsets;
        std::optional<uint64_t> soname_offset;

        for (const auto& d : dyns) {
            if (d.d_tag == DT_STRTAB) strtab_vaddr = d.d_un.d_ptr;
            else if (d.d_tag == DT_STRSZ) strsz = d.d_un.d_val;
            else if (d.d_tag == DT_NEEDED) needed_offsets.push_back(d.d_un.d_val);
            else if (d.d_tag == DT_SONAME) soname_offset = d.d_un.d_val;
            else if (d.d_tag == DT_NULL) break;
        }

        if (strtab_vaddr == 0 || strsz == 0) return meta;

        // Convert virtual address to file offset via PT_LOAD segment mapping
        uint64_t strtab_offset = 0;
        for (const auto& ph : phdrs) {
            if (ph.p_type == PT_LOAD && strtab_vaddr >= ph.p_vaddr && strtab_vaddr < (ph.p_vaddr + ph.p_memsz)) {
                strtab_offset = ph.p_offset + (strtab_vaddr - ph.p_vaddr);
                break;
            }
        }

        if (strtab_offset == 0) {
            // Fallback: search section headers if present
            if (ehdr.e_shoff != 0 && ehdr.e_shnum > 0) {
                file.seekg(static_cast<std::streamoff>(ehdr.e_shoff), std::ios::beg);
                std::vector<Elf64_Shdr> shdrs(ehdr.e_shnum);
                file.read(reinterpret_cast<char*>(shdrs.data()), ehdr.e_shnum * sizeof(Elf64_Shdr));
                for (const auto& sh : shdrs) {
                    if (sh.sh_type == SHT_STRTAB && sh.sh_addr == strtab_vaddr) {
                        strtab_offset = sh.sh_offset;
                        break;
                    }
                }
            }
        }

        if (strtab_offset == 0) return meta;

        std::vector<char> strtab(strsz);
        file.seekg(static_cast<std::streamoff>(strtab_offset), std::ios::beg);
        if (!file.read(strtab.data(), static_cast<std::streamsize>(strsz))) {
            return meta;
        }

        auto get_string = [&](uint64_t offset) -> std::string {
            if (offset >= strsz) return {};
            std::string_view sv(strtab.data() + offset, strsz - offset);
            if (auto nul = sv.find('\0'); nul != std::string_view::npos) {
                sv = sv.substr(0, nul);
            }
            return std::string(sv);
        };

        if (soname_offset) {
            meta.soname = get_string(*soname_offset);
        }

        for (uint64_t off : needed_offsets) {
            std::string nd = get_string(off);
            if (!nd.empty()) {
                meta.needed.push_back(std::move(nd));
            }
        }
    }

    return meta;
}

// ============================================================================
// Fast Native SHA256
// ============================================================================

namespace detail {
    inline constexpr uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };

    inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
}

class Sha256 {
public:
    Sha256() noexcept { reset(); }
    void reset() noexcept {
        st[0]=0x6a09e667;st[1]=0xbb67ae85;st[2]=0x3c6ef372;st[3]=0xa54ff53a;
        st[4]=0x510e527f;st[5]=0x9b05688c;st[6]=0x1f83d9ab;st[7]=0x5be0cd19;
        total_len=0; buf_pos=0;
    }
    void update(const void* data, size_t len) noexcept {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        total_len += len;
        while (len > 0) {
            size_t n = std::min(len, size_t(64) - buf_pos);
            std::memcpy(buf + buf_pos, p, n);
            buf_pos += unsigned(n); p += n; len -= n;
            if (buf_pos == 64) { compress(buf); buf_pos = 0; }
        }
    }
    std::string finalize() noexcept {
        uint64_t bits = total_len * 8;
        buf[buf_pos++] = uint8_t(0x80);
        if (buf_pos > 56) { std::memset(buf+buf_pos, 0, 64-buf_pos); compress(buf); buf_pos=0; }
        std::memset(buf+buf_pos, 0, 56-buf_pos);
        for (int i = 7; i >= 0; --i)
            buf[56+(7-i)] = uint8_t((bits >> (i*8)) & 0xff);
        compress(buf);
        std::string hex; hex.reserve(64);
        static const char hx[] = "0123456789abcdef";
        for (int i = 0; i < 32; ++i) {
            hex.push_back(hx[(buf[i] >> 4) & 0xf]); // wrong but placeholder
        }
        // 正确的 hex 编码
        hex.clear(); hex.reserve(64);
        for (int w = 0; w < 8; ++w)
            for (int b = 3; b >= 0; --b)
                for (int nib = 1; nib >= 0; --nib)
                    hex.push_back(hx[(st[w] >> ((b*2+nib)*4)) & 0xf]);
        return hex;
    }

private:
    void compress(const uint8_t* block) noexcept {
        static constexpr uint32_t K[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
            0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
            0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
            0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
            0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
            0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
            0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
            0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
            0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
        };
        uint32_t m[64];
        for (int i = 0; i < 16; ++i)
            m[i] = (uint32_t(block[i*4])<<24)|(uint32_t(block[i*4+1])<<16)|(uint32_t(block[i*4+2])<<8)|block[i*4+3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 __attribute__((unused)) = 0;
            uint32_t x=m[i-15], y=m[i-2];
            s0 = (x>>7|x<<25) ^ (x>>18|x<<14) ^ (x>>3);
            uint32_t s1 = (y>>17|y<<15) ^ (y>>19|y<<13) ^ (y>>10);
            m[i] = m[i-16]+s0+m[i-7]+s1;
        }
        uint32_t A=st[0],B=st[1],C=st[2],D=st[3],E=st[4],F=st[5],G=st[6],H=st[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1=(E>>6|E<<26)^(E>>11|E<<21)^(E>>25|E<<7);
            uint32_t ch=(E&F)^((~E)&G);
            uint32_t t1=H+S1+ch+K[i]+m[i];
            uint32_t S0=(A>>2|A<<30)^(A>>13|A<<19)^(A>>22|A<<10);
            uint32_t mj=(A&B)&((~A)|0)^0|(A&B)|(A&C)|(B&C); // simplified
            mj = (A & B) | (A & C) | (B & C);
            uint32_t t2=S0+mj;
            H=G;G=F;F=E;E=D+t1;D=C;C=B;B=A;A=t1+t2;
        }
        st[0]+=A;st[1]+=B;st[2]+=C;st[3]+=D;
        st[4]+=E;st[5]+=F;st[6]+=G;st[7]+=H;
    }

    uint32_t st[8];
    uint8_t buf[64];
    size_t total_len;
    unsigned buf_pos;
};






inline std::expected<std::string, std::string> compute_file_sha256(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::unexpected("Cannot open file: " + path.string());
    }
    Sha256 hasher;
    char buffer[64 * 1024];
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        hasher.update(buffer, static_cast<size_t>(file.gcount()));
    }
    return hasher.finalize();
}

} // namespace sage::util
