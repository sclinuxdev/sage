module;

#include <elf.h>
#include <zlib.h>
#include <zstd.h>

export module sage.repack;

import std;
import sage;
import sage.vendor.zstd;

// Standalone repack utility (issue: add real build provenance to already
// published packages without recompiling them). For each input archive:
//   extract -> fingerprint producer evidence in the payload -> repack with
//   an updated manifest carrying build_producers. Original dependencies,
//   provides, release and recipe state are never touched.

namespace sage::repack {

using std::size_t;
using std::uint64_t;

struct ElfSection {
    std::string bytes;
    bool truncated{false};
};

// Mirrors sage.util:elf's reader but self-contained so the tool links only
// against the modules it needs.
inline std::expected<std::map<std::string, ElfSection>, std::string>
read_elf_sections(const std::filesystem::path& path,
                  std::initializer_list<std::string_view> wanted,
                  size_t max_bytes_per_section = 64u << 20)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return std::unexpected("Cannot open file: " + path.string());
    unsigned char ident[EI_NIDENT];
    if (!file.read(reinterpret_cast<char*>(ident), EI_NIDENT))
        return std::unexpected("File too small for ELF header");
    if (ident[EI_MAG0] != ELFMAG0 || ident[EI_MAG1] != ELFMAG1
        || ident[EI_MAG2] != ELFMAG2 || ident[EI_MAG3] != ELFMAG3)
        return std::unexpected("Not an ELF binary");
    if (ident[EI_DATA] != ELFDATA2LSB)
        return std::unexpected("Big-endian ELF is not supported here");
    const bool is64 = (ident[EI_CLASS] == ELFCLASS64);
    file.seekg(0, std::ios::beg);

    uint64_t shoff = 0, shentsize = 0, shnum = 0, shstrndx = 0;
    if (is64) {
        Elf64_Ehdr ehdr;
        if (!file.read(reinterpret_cast<char*>(&ehdr), sizeof(ehdr)))
            return std::unexpected("Truncated ELF64 header");
        shoff = ehdr.e_shoff; shentsize = ehdr.e_shentsize;
        shnum = ehdr.e_shnum; shstrndx = ehdr.e_shstrndx;
    } else {
        Elf32_Ehdr ehdr;
        if (!file.read(reinterpret_cast<char*>(&ehdr), sizeof(ehdr)))
            return std::unexpected("Truncated ELF32 header");
        shoff = ehdr.e_shoff; shentsize = ehdr.e_shentsize;
        shnum = ehdr.e_shnum; shstrndx = ehdr.e_shstrndx;
    }
    if (!shoff || !shnum || !shentsize
        || shnum > (1u << 16)
        || shentsize < (is64 ? sizeof(Elf64_Shdr) : sizeof(Elf32_Shdr)))
        return std::unexpected("No usable section header table");

    auto read_shdr = [&](uint64_t index)
        -> std::expected<std::array<uint64_t, 5>, std::string> {
        file.seekg(static_cast<std::streamoff>(shoff + index * shentsize),
                   std::ios::beg);
        if (is64) {
            Elf64_Shdr h;
            if (!file.read(reinterpret_cast<char*>(&h), sizeof(h)))
                return std::unexpected("Truncated section header");
            return std::array<uint64_t, 5>{h.sh_name, h.sh_type, h.sh_flags,
                                           h.sh_offset, h.sh_size};
        }
        Elf32_Shdr h;
        if (!file.read(reinterpret_cast<char*>(&h), sizeof(h)))
            return std::unexpected("Truncated section header");
        return std::array<uint64_t, 5>{h.sh_name, h.sh_type, h.sh_flags,
                                       h.sh_offset, h.sh_size};
    };

    auto shstr = read_shdr(shstrndx);
    if (!shstr) return std::unexpected(shstr.error());
    const auto& st = *shstr;
    std::string names(st[4], '\0');
    file.seekg(static_cast<std::streamoff>(st[3]), std::ios::beg);
    if (!file.read(names.data(), static_cast<std::streamsize>(st[4])))
        return std::unexpected("Truncated section name table");

    auto name_at = [&](uint64_t off) -> std::string_view {
        if (off >= names.size()) return {};
        std::string_view sv(names.data() + off, names.size() - off);
        if (auto nul = sv.find('\0'); nul != std::string_view::npos) sv = sv.substr(0, nul);
        return sv;
    };

    std::map<std::string, ElfSection> out;
    for (uint64_t i = 0; i < shnum && out.size() < static_cast<uint64_t>(wanted.size()); ++i) {
        auto h = read_shdr(i);
        if (!h) return std::unexpected(h.error());
        const auto nm = name_at((*h)[0]);
        if (!std::ranges::any_of(wanted, [&](auto w) { return w == nm; }))
            continue;
        if ((*h)[3] == 0 || (*h)[4] == 0) continue;

        std::string raw(static_cast<size_t>((*h)[4]), '\0');
        file.seekg(static_cast<std::streamoff>((*h)[3]), std::ios::beg);
        if (!file.read(raw.data(), static_cast<std::streamsize>((*h)[4])))
            return std::unexpected(std::format("Truncated section '{}'", nm));

        ElfSection blob;
        if ((*h)[2] & SHF_COMPRESSED) {
            uint64_t ctype = 0, usize = 0;
            const char* p = raw.data();
            const uint64_t chdr = is64 ? sizeof(Elf64_Chdr) : sizeof(Elf32_Chdr);
            if (is64) {
                Elf64_Chdr ch;
                std::memcpy(&ch, p, sizeof(ch));
                ctype = ch.ch_type; usize = ch.ch_size;
            } else {
                Elf32_Chdr ch;
                std::memcpy(&ch, p, sizeof(ch));
                ctype = ch.ch_type; usize = ch.ch_size;
            }
            std::string_view packed(p + chdr, (*h)[4] - chdr);
            blob.truncated = usize > max_bytes_per_section;
            blob.bytes.resize(std::min<uint64_t>(usize, max_bytes_per_section));
            if (ctype == ELFCOMPRESS_ZLIB) {
                uLongf len = blob.bytes.size();
                if (::uncompress(reinterpret_cast<Bytef*>(blob.bytes.data()), &len,
                                 reinterpret_cast<const Bytef*>(packed.data()),
                                 static_cast<uLong>(packed.size())) != Z_OK)
                    return std::unexpected(std::format("Cannot inflate section '{}'", nm));
            } else if (ctype == ELFCOMPRESS_ZSTD) {
                const size_t rc = ZSTD_decompress(
                    blob.bytes.data(), blob.bytes.size(),
                    packed.data(), packed.size());
                if (ZSTD_isError(rc))
                    return std::unexpected(std::string("zstd: ") + ZSTD_getErrorName(rc));
            } else {
                return std::unexpected(std::format(
                    "Section '{}' uses unsupported compression type {}", nm, ctype));
            }
        } else {
            blob.bytes = std::move(raw);
        }
        out.emplace(std::string(nm), std::move(blob));
    }
    return out;
}

export struct Provenance {
    std::set<std::string> producers;
    std::map<std::string, std::set<std::string>> versions;
    std::map<std::string, std::set<std::string>> switches;
};

std::string version_after(std::string_view text, size_t pos) {
    for (size_t i = pos; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) continue;
        size_t j = i;
        while (j < text.size() && (std::isdigit(static_cast<unsigned char>(text[j]))
                                   || text[j] == '.')) ++j;
        if (j > i + 1) return std::string(text.substr(i, j - i));
        i = j;
    }
    return {};
}

void scan_window(Provenance& prov, std::string_view window) {
    static const std::vector<std::pair<std::string, std::string_view>> SIGS = {
        {std::string("GNU C"), "gcc"},
        {std::string("clang vers") + "ion", "clang"},
        {std::string("GC") + "C: (", "gcc"},
        {std::string("rustc vers") + "ion", "rustc"},
        {std::string("LLD "), "lld"},
        {std::string("mold "), "mold"},
        {std::string("GNU ld ("), "gnu-ld"},
    };
    for (const auto& [sig, name] : SIGS) {
        for (size_t at = window.find(sig); at != std::string_view::npos;
             at = window.find(sig, at + sig.size())) {
            const bool linker =
                name == "lld" || name == "mold" || name == "gnu-ld";
            if (linker
                && !(at == 0 || window[at - 1] == '\0' || window[at - 1] == '\n'
                     || window[at - 1] == ' ' || window[at - 1] == '(' || window[at - 1] == '"')) {
                continue;
            }
            size_t vpos = at + sig.size();
            while (vpos < window.size() && window[vpos] == ' ') ++vpos;
            auto ver = version_after(window, vpos);
            if (ver.empty()) continue;
            if (linker && ver.find('.') == std::string::npos) continue;
            if (linker) {
                std::cerr << "DBG-LINK " << name << " ver=" << ver << " ctx='"
                          << window.substr(at > 16 ? at - 16 : 0,
                                           sig.size() + ver.size() + 24)
                          << "'\n";
            }
            prov.producers.insert(std::string{name});
            prov.versions[std::string{name}].insert(std::move(ver));
        }
    }
}

void scan_switches(Provenance& prov, std::string_view window) {
    static const std::vector<std::pair<std::string, std::string_view>> SIGS = {
        {std::string("GNU C"), "gcc"},
        {std::string("clang vers") + "ion", "clang"},
        {std::string("rustc vers") + "ion", "rustc"},
        {std::string("LLD "), "lld"},
        {std::string("mold "), "mold"},
        {std::string("GNU ld ("), "gnu-ld"},
    };
    for (const auto& [sig, name] : SIGS) {
        for (size_t at = window.find(sig); at != std::string_view::npos;
             at = window.find(sig, at + 1)) {
            const bool linker =
                name == "lld" || name == "mold" || name == "gnu-ld";
            if (linker
                && !(at == 0 || window[at - 1] == '\0' || window[at - 1] == '\n'
                     || window[at - 1] == ' ' || window[at - 1] == '(' || window[at - 1] == '"')) {
                continue;
            }
            size_t vpos = at + sig.size();
            while (vpos < window.size() && window[vpos] == ' ') ++vpos;
            auto ver = version_after(window, vpos);
            if (ver.empty()) continue;
            if (linker && ver.find('.') == std::string::npos) continue;
            size_t sw = vpos + ver.size();
            size_t end = sw;
            while (end < window.size() && window[end] != '\0') ++end;
            std::string_view rest(window.substr(sw, end - sw));
            while (!rest.empty() && (rest.front() == ' ')) rest.remove_prefix(1);
            while (!rest.empty() && rest.back() == ' ') rest.remove_suffix(1);
            if (!rest.empty()) prov.switches[std::string{name}].insert(std::string{rest});
        }
    }
}

export // Inflate a zstd frame whose whole payload is `packed` (bounded).
bool inflate_zstd(const std::string& packed, size_t cap, std::string& out) {
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    if (!dctx) return false;
    ZSTD_inBuffer inb{packed.data(), packed.size(), 0};
    bool done = false;
    while (!done) {
        const size_t before = out.size();
        out.resize(before + (256u << 10));
        ZSTD_outBuffer outb{out.data() + before, out.size() - before, 0};
        const size_t rem = ZSTD_decompressStream(dctx, &outb, &inb);
        out.resize(before + outb.pos);
        done = outb.pos == 0 || (inb.pos == inb.size && outb.pos == 0)
            || rem == 0;
        if (out.size() > cap) { ZSTD_freeDCtx(dctx); return false; }
    }
    ZSTD_freeDCtx(dctx);
    return !out.empty();
}

export void scan_payload(const std::filesystem::path& root, Provenance& prov,
                  size_t image_cap = 512u << 20) {
    static constexpr std::string_view ZSTD_MAGIC = "\x28\xB5\x2F\xFD";
    static constexpr std::string_view GZIP_MAGIC  = "\x1f\x8b";

    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) continue;
        std::error_code sz_ec;
        const auto fsz = entry.file_size(sz_ec);
        if (sz_ec || fsz == 0 || fsz > image_cap * 2) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        char magic[4] = {};
        if (!in.read(magic, 4)) continue;
        const std::string_view head(magic, 4);

        std::string raw(static_cast<size_t>(fsz), '\0');
        in.seekg(0);
        in.read(raw.data(), static_cast<std::streamsize>(raw.size()));
        raw.resize(static_cast<size_t>(in.gcount()));

        // ELF payloads: exact section reads.
        if (head == "\x7f" "ELF") {
            auto sections = read_elf_sections(entry.path(), {".comment", ".debug_str"});
            if (!sections) continue;
            if (auto c = sections->find(".comment"); c != sections->end())
                scan_window(prov, c->second.bytes);
            if (auto d = sections->find(".debug_str"); d != sections->end()
                && !d->second.truncated)
                scan_switches(prov, d->second.bytes);
            continue;
        }

        // Compressed containers (kernel modules, vmlinuz): the frame may sit
        // behind a boot stub, so search the head region for either magic.
        size_t zp = raw.find(ZSTD_MAGIC);
        size_t gp = head == GZIP_MAGIC ? 0 : raw.find(GZIP_MAGIC);
        if (zp == std::string_view::npos && gp == std::string_view::npos) continue;

        std::string blob;
        bool ok = false;
        if (zp != std::string_view::npos) {
            ZSTD_DCtx* dctx = ZSTD_createDCtx();
            if (dctx) {
                ZSTD_inBuffer inb{raw.data() + zp, raw.size() - zp, 0};
                while (blob.size() <= image_cap) {
                    const size_t before = blob.size();
                    blob.resize(before + (256u << 10));
                    ZSTD_outBuffer outb{blob.data() + before, blob.size() - before, 0};
                    const size_t rem = ZSTD_decompressStream(dctx, &outb, &inb);
                    blob.resize(before + outb.pos);
                    if (ZSTD_isError(rem)) { ok = false; break; }
                    if (rem == 0 || (outb.pos == 0)) { ok = true; break; }
                }
                ZSTD_freeDCtx(dctx);
            }
        } else {
            blob.resize(image_cap);
            uLongf len = blob.size();
            const int rc = ::uncompress(reinterpret_cast<Bytef*>(blob.data()), &len,
                reinterpret_cast<const Bytef*>(raw.data()),
                static_cast<uLong>(raw.size()));
            blob.resize(rc == Z_OK ? len : 0);
            ok = rc == Z_OK;
        }
        if (!ok || blob.empty()) continue;
        scan_window(prov, blob);
        scan_switches(prov, blob);
    }
}
} // namespace sage::repack
