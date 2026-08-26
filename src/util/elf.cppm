module;

#include <elf.h>
#include <zlib.h>
#include <zstd.h>

export module sage.util:elf;

import std;

export namespace sage::util {

using std::uint64_t;
using std::size_t;


// Native zero-copy ELF SONAME / DT_NEEDED and GNU symbol versioning scanner.
struct ElfMetadata {
    std::string soname;
    std::vector<std::string> needed;
    bool is_shared{false};
    bool is_executable{false};
    std::vector<std::string> rpaths;
    std::vector<std::string> runpaths;
    std::vector<std::string> verdef_versions;
    std::vector<std::pair<std::string, std::string>> verneed_entries;
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
        uint64_t verdef_vaddr = 0;
        uint64_t verdef_num = 0;
        uint64_t verneed_vaddr = 0;
        uint64_t verneed_num = 0;
        std::vector<uint64_t> needed_offsets;
        std::optional<uint64_t> soname_offset;
        std::vector<uint64_t> rpath_offsets;
        std::vector<uint64_t> runpath_offsets;

        for (const auto& d : dyns) {
            if (d.d_tag == DT_STRTAB) strtab_vaddr = d.d_un.d_ptr;
            else if (d.d_tag == DT_STRSZ) strsz = d.d_un.d_val;
            else if (d.d_tag == DT_NEEDED) needed_offsets.push_back(d.d_un.d_val);
            else if (d.d_tag == DT_SONAME) soname_offset = d.d_un.d_val;
            else if (d.d_tag == DT_RPATH) rpath_offsets.push_back(d.d_un.d_val);
            else if (d.d_tag == DT_RUNPATH) runpath_offsets.push_back(d.d_un.d_val);
            else if (d.d_tag == DT_VERDEF) verdef_vaddr = d.d_un.d_ptr;
            else if (d.d_tag == DT_VERDEFNUM) verdef_num = d.d_un.d_val;
            else if (d.d_tag == DT_VERNEED) verneed_vaddr = d.d_un.d_ptr;
            else if (d.d_tag == DT_VERNEEDNUM) verneed_num = d.d_un.d_val;
            else if (d.d_tag == DT_NULL) break;
        }

        if (strtab_vaddr == 0 || strsz == 0) return meta;

        auto vaddr_to_offset = [&](uint64_t vaddr, uint32_t shtype) -> uint64_t {
            if (vaddr != 0) {
                for (const auto& ph : phdrs) {
                    if (ph.p_type == PT_LOAD && vaddr >= ph.p_vaddr && vaddr < (ph.p_vaddr + ph.p_memsz)) {
                        return ph.p_offset + (vaddr - ph.p_vaddr);
                    }
                }
            }
            if (ehdr.e_shoff != 0 && ehdr.e_shnum > 0) {
                file.seekg(static_cast<std::streamoff>(ehdr.e_shoff), std::ios::beg);
                std::vector<Elf64_Shdr> shdrs(ehdr.e_shnum);
                if (file.read(reinterpret_cast<char*>(shdrs.data()), ehdr.e_shnum * sizeof(Elf64_Shdr))) {
                    for (const auto& sh : shdrs) {
                        if ((shtype != 0 && sh.sh_type == shtype) || (vaddr != 0 && sh.sh_addr == vaddr)) {
                            return sh.sh_offset;
                        }
                    }
                }
            }
            return 0;
        };

        uint64_t strtab_offset = vaddr_to_offset(strtab_vaddr, SHT_STRTAB);
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

        for (uint64_t off : rpath_offsets) {
            std::string rp = get_string(off);
            if (!rp.empty()) {
                meta.rpaths.push_back(std::move(rp));
            }
        }

        for (uint64_t off : runpath_offsets) {
            std::string rp = get_string(off);
            if (!rp.empty()) {
                meta.runpaths.push_back(std::move(rp));
            }
        }

        // Parse GNU Symbol Version Definitions (DT_VERDEF)
        uint64_t verdef_offset = vaddr_to_offset(verdef_vaddr, SHT_GNU_verdef);
        if (verdef_offset != 0) {
            uint64_t cur = verdef_offset;
            size_t count = (verdef_num > 0) ? verdef_num : 1024;
            for (size_t i = 0; i < count; ++i) {
                file.seekg(static_cast<std::streamoff>(cur), std::ios::beg);
                Elf64_Verdef vd;
                if (!file.read(reinterpret_cast<char*>(&vd), sizeof(vd))) break;
                if (vd.vd_cnt > 0 && !(vd.vd_flags & 1 /* VER_FLG_BASE */)) {
                    uint64_t aux_cur = cur + vd.vd_aux;
                    file.seekg(static_cast<std::streamoff>(aux_cur), std::ios::beg);
                    Elf64_Verdaux vda;
                    if (file.read(reinterpret_cast<char*>(&vda), sizeof(vda))) {
                        std::string vname = get_string(vda.vda_name);
                        if (!vname.empty()) {
                            meta.verdef_versions.push_back(std::move(vname));
                        }
                    }
                }
                if (vd.vd_next == 0) break;
                cur += vd.vd_next;
            }
        }

        // Parse GNU Symbol Version Requirements (DT_VERNEED)
        uint64_t verneed_offset = vaddr_to_offset(verneed_vaddr, SHT_GNU_verneed);
        if (verneed_offset != 0) {
            uint64_t cur = verneed_offset;
            size_t count = (verneed_num > 0) ? verneed_num : 1024;
            for (size_t i = 0; i < count; ++i) {
                file.seekg(static_cast<std::streamoff>(cur), std::ios::beg);
                Elf64_Verneed vn;
                if (!file.read(reinterpret_cast<char*>(&vn), sizeof(vn))) break;
                std::string fname = get_string(vn.vn_file);
                if (vn.vn_cnt > 0) {
                    uint64_t aux_cur = cur + vn.vn_aux;
                    for (size_t j = 0; j < vn.vn_cnt; ++j) {
                        file.seekg(static_cast<std::streamoff>(aux_cur), std::ios::beg);
                        Elf64_Vernaux vna;
                        if (!file.read(reinterpret_cast<char*>(&vna), sizeof(vna))) break;
                        std::string vname = get_string(vna.vna_name);
                        if (!fname.empty() && !vname.empty()) {
                            meta.verneed_entries.push_back({fname, vname});
                        }
                        if (vna.vna_next == 0) break;
                        aux_cur += vna.vna_next;
                    }
                }
                if (vn.vn_next == 0) break;
                cur += vn.vn_next;
            }
        }
    } else {
        Elf32_Ehdr ehdr;
        if (!file.read(reinterpret_cast<char*>(&ehdr), sizeof(ehdr))) {
            return std::unexpected("Failed to read ELF32 header");
        }

        meta.is_shared = (ehdr.e_type == ET_DYN);
        meta.is_executable = (ehdr.e_type == ET_EXEC || ehdr.e_type == ET_DYN);

        std::vector<Elf32_Phdr> phdrs(ehdr.e_phnum);
        file.seekg(static_cast<std::streamoff>(ehdr.e_phoff), std::ios::beg);
        if (!file.read(reinterpret_cast<char*>(phdrs.data()), ehdr.e_phnum * sizeof(Elf32_Phdr))) {
            return meta;
        }

        const Elf32_Phdr* dyn_phdr = nullptr;
        for (const auto& ph : phdrs) {
            if (ph.p_type == PT_DYNAMIC) {
                dyn_phdr = &ph;
                break;
            }
        }

        if (!dyn_phdr) return meta;

        size_t num_dyn = dyn_phdr->p_filesz / sizeof(Elf32_Dyn);
        std::vector<Elf32_Dyn> dyns(num_dyn);
        file.seekg(static_cast<std::streamoff>(dyn_phdr->p_offset), std::ios::beg);
        if (!file.read(reinterpret_cast<char*>(dyns.data()), dyn_phdr->p_filesz)) {
            return meta;
        }

        uint64_t strtab_vaddr = 0;
        uint64_t strsz = 0;
        uint64_t verdef_vaddr = 0;
        uint64_t verdef_num = 0;
        uint64_t verneed_vaddr = 0;
        uint64_t verneed_num = 0;
        std::vector<uint64_t> needed_offsets;
        std::optional<uint64_t> soname_offset;
        std::vector<uint64_t> rpath_offsets;
        std::vector<uint64_t> runpath_offsets;

        for (const auto& d : dyns) {
            if (d.d_tag == DT_STRTAB) strtab_vaddr = d.d_un.d_ptr;
            else if (d.d_tag == DT_STRSZ) strsz = d.d_un.d_val;
            else if (d.d_tag == DT_NEEDED) needed_offsets.push_back(d.d_un.d_val);
            else if (d.d_tag == DT_SONAME) soname_offset = d.d_un.d_val;
            else if (d.d_tag == DT_RPATH) rpath_offsets.push_back(d.d_un.d_val);
            else if (d.d_tag == DT_RUNPATH) runpath_offsets.push_back(d.d_un.d_val);
            else if (d.d_tag == DT_VERDEF) verdef_vaddr = d.d_un.d_ptr;
            else if (d.d_tag == DT_VERDEFNUM) verdef_num = d.d_un.d_val;
            else if (d.d_tag == DT_VERNEED) verneed_vaddr = d.d_un.d_ptr;
            else if (d.d_tag == DT_VERNEEDNUM) verneed_num = d.d_un.d_val;
            else if (d.d_tag == DT_NULL) break;
        }

        if (strtab_vaddr == 0 || strsz == 0) return meta;

        auto vaddr_to_offset = [&](uint64_t vaddr, uint32_t shtype) -> uint64_t {
            if (vaddr != 0) {
                for (const auto& ph : phdrs) {
                    if (ph.p_type == PT_LOAD && strtab_vaddr >= ph.p_vaddr && strtab_vaddr < (ph.p_vaddr + ph.p_memsz)) {
                        return ph.p_offset + (vaddr - ph.p_vaddr);
                    }
                }
            }
            if (ehdr.e_shoff != 0 && ehdr.e_shnum > 0) {
                file.seekg(static_cast<std::streamoff>(ehdr.e_shoff), std::ios::beg);
                std::vector<Elf32_Shdr> shdrs(ehdr.e_shnum);
                if (file.read(reinterpret_cast<char*>(shdrs.data()), ehdr.e_shnum * sizeof(Elf32_Shdr))) {
                    for (const auto& sh : shdrs) {
                        if ((shtype != 0 && sh.sh_type == shtype) || (vaddr != 0 && sh.sh_addr == vaddr)) {
                            return sh.sh_offset;
                        }
                    }
                }
            }
            return 0;
        };

        uint64_t strtab_offset = vaddr_to_offset(strtab_vaddr, SHT_STRTAB);
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

        for (uint64_t off : rpath_offsets) {
            std::string rp = get_string(off);
            if (!rp.empty()) {
                meta.rpaths.push_back(std::move(rp));
            }
        }

        for (uint64_t off : runpath_offsets) {
            std::string rp = get_string(off);
            if (!rp.empty()) {
                meta.runpaths.push_back(std::move(rp));
            }
        }

        // Parse GNU Symbol Version Definitions (DT_VERDEF)
        uint64_t verdef_offset = vaddr_to_offset(verdef_vaddr, SHT_GNU_verdef);
        if (verdef_offset != 0) {
            uint64_t cur = verdef_offset;
            size_t count = (verdef_num > 0) ? verdef_num : 1024;
            for (size_t i = 0; i < count; ++i) {
                file.seekg(static_cast<std::streamoff>(cur), std::ios::beg);
                Elf32_Verdef vd;
                if (!file.read(reinterpret_cast<char*>(&vd), sizeof(vd))) break;
                if (vd.vd_cnt > 0 && !(vd.vd_flags & 1 /* VER_FLG_BASE */)) {
                    uint64_t aux_cur = cur + vd.vd_aux;
                    file.seekg(static_cast<std::streamoff>(aux_cur), std::ios::beg);
                    Elf32_Verdaux vda;
                    if (file.read(reinterpret_cast<char*>(&vda), sizeof(vda))) {
                        std::string vname = get_string(vda.vda_name);
                        if (!vname.empty()) {
                            meta.verdef_versions.push_back(std::move(vname));
                        }
                    }
                }
                if (vd.vd_next == 0) break;
                cur += vd.vd_next;
            }
        }

        // Parse GNU Symbol Version Requirements (DT_VERNEED)
        uint64_t verneed_offset = vaddr_to_offset(verneed_vaddr, SHT_GNU_verneed);
        if (verneed_offset != 0) {
            uint64_t cur = verneed_offset;
            size_t count = (verneed_num > 0) ? verneed_num : 1024;
            for (size_t i = 0; i < count; ++i) {
                file.seekg(static_cast<std::streamoff>(cur), std::ios::beg);
                Elf32_Verneed vn;
                if (!file.read(reinterpret_cast<char*>(&vn), sizeof(vn))) break;
                std::string fname = get_string(vn.vn_file);
                if (vn.vn_cnt > 0) {
                    uint64_t aux_cur = cur + vn.vn_aux;
                    for (size_t j = 0; j < vn.vn_cnt; ++j) {
                        file.seekg(static_cast<std::streamoff>(aux_cur), std::ios::beg);
                        Elf32_Vernaux vna;
                        if (!file.read(reinterpret_cast<char*>(&vna), sizeof(vna))) break;
                        std::string vname = get_string(vna.vna_name);
                        if (!fname.empty() && !vname.empty()) {
                            meta.verneed_entries.push_back({fname, vname});
                        }
                        if (vna.vna_next == 0) break;
                        aux_cur += vna.vna_next;
                    }
                }
                if (vn.vn_next == 0) break;
                cur += vn.vn_next;
            }
        }
    }

    return meta;
}
// Section-name-addressable bounded reads for ELF inspection tools.
struct ElfSection {
    std::string bytes;
    bool truncated{false};
};

// Blob reads are bounded: a section larger than the cap comes back truncated
// with the flag set, so callers stay honest about what they did verify.
inline std::expected<std::map<std::string, ElfSection>, std::string>
read_elf_sections(
    const std::filesystem::path& path,
    std::initializer_list<std::string_view> wanted,
    size_t max_bytes_per_section = 64u << 20)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::unexpected("Cannot open file: " + path.string());
    }
    unsigned char ident[EI_NIDENT];
    if (!file.read(reinterpret_cast<char*>(ident), EI_NIDENT)) {
        return std::unexpected("File too small for ELF header");
    }
    if (ident[EI_MAG0] != ELFMAG0 || ident[EI_MAG1] != ELFMAG1
        || ident[EI_MAG2] != ELFMAG2 || ident[EI_MAG3] != ELFMAG3) {
        return std::unexpected("Not an ELF binary");
    }
    if (ident[EI_DATA] != ELFDATA2LSB) {
        return std::unexpected("Big-endian ELF is not supported here");
    }
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
        || shentsize < (is64 ? sizeof(Elf64_Shdr) : sizeof(Elf32_Shdr))) {
        return std::unexpected("No usable section header table");
    }

    auto read_shdr = [&](uint64_t index)
        -> std::expected<std::array<uint64_t, 5>, std::string> {
        // name, type, offset, size  (width-normalized; link unused today)
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
    for (uint64_t i = 0; i < shnum && out.size() < wanted.size(); ++i) {
        auto h = read_shdr(i);
        if (!h) return std::unexpected(h.error());
        // h: [0]=name [1]=type [2]=flags [3]=offset [4]=size
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
            // SHF_COMPRESSED: ElfNN_Chdr (type/size/align) + compressed frame.
            uint64_t ctype = 0, usize = 0;
            const char* p = raw.data();
            if (is64) {
                Elf64_Chdr ch;
                std::memcpy(&ch, p, sizeof(ch));
                ctype = ch.ch_type; usize = ch.ch_size;
            } else {
                Elf32_Chdr ch;
                std::memcpy(&ch, p, sizeof(ch));
                ctype = ch.ch_type; usize = ch.ch_size;
            }
            const uint64_t chdr = is64 ? sizeof(Elf64_Chdr) : sizeof(Elf32_Chdr);
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


} // namespace sage::util
