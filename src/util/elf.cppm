module;

#include <elf.h>

export module sage.util:elf;

import std;

export namespace sage::util {

using std::uint64_t;
using std::size_t;


// Native zero-copy ELF SONAME / DT_NEEDED scanner.
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

} // namespace sage::util
