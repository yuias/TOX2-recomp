#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace tox2 {

struct LoadedExecutable {
    std::uint32_t entry_opd = 0, entry_func = 0, entry_toc = 0;
    std::uint32_t tls_vaddr = 0, tls_filesz = 0, tls_memsz = 0;
    std::uint32_t sdk_version = 0;
    std::int32_t primary_prio = 0;
    std::uint32_t primary_stacksize = 0, malloc_pagesize = 0, ppc_seg = 0;
    std::uint32_t image_end = 0;  // highest PT_LOAD end, 64 KiB rounded
    std::string sha256;           // lower-case hex of the whole file
};

// Loads a big-endian PPC64 ELF executable into process().memory (which must be reserved) and
// marks its range used in process().main. Logs one line per program header. Errors are reported
// before any guest memory is touched where possible.
std::expected<LoadedExecutable, std::string> loadExecutable(const std::filesystem::path& elf);

struct LoadedModule {
    std::string name;  // module info name, e.g. "cellSre_Library"
    std::uint8_t version_major = 0, version_minor = 0;
    std::uint32_t base = 0;         // lowest PT_LOAD address
    std::uint32_t image_end = 0;    // highest PT_LOAD end, 64 KiB rounded
    std::uint32_t toc = 0;          // module info TOC
    std::uint32_t module_info = 0;  // guest address of the module info
    std::string sha256;             // lower-case hex of the whole file
};

// Loads a relocated PRX at its segment addresses into process().memory and marks [base, image_end)
// used in process().main. Refuses: not ELF64 big-endian PPC64; e_type other than 0xFFA4; a 0x700000A4
// program header ("relocations not applied; pass libsre.reloc.elf written by tox2_decrypt reloc"); no
// PT_LOAD; program header 0 not PT_LOAD; overlapping or out-of-main-region segments; lowest PT_LOAD
// address != expected_base; expected_base < lowest_free; module info outside segment 0's file data.
// Logs one "tox2: load: <file> phdr ..." line per program header. Does not apply the executable-only
// rules of loadExecutable (private-area predicate, entry descriptor, process parameters).
std::expected<LoadedModule, std::string> loadModule(const std::filesystem::path& path, std::uint32_t expected_base,
                                                    std::uint32_t lowest_free);

}  // namespace tox2
