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

}  // namespace tox2
