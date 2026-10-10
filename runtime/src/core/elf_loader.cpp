#include "core/elf_loader.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <vector>

#include "core/guest_memory.h"
#include "core/log.h"
#include "tox2/ppu_context.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

namespace tox2 {
namespace {

constexpr std::uint16_t kMachinePpc64 = 21;
constexpr std::uint32_t kPtLoad = 1;
constexpr std::uint32_t kPtTls = 7;
constexpr std::uint32_t kPtProcessParam = 0x60000001;
constexpr std::uint32_t kProcessParamMagic = 0x13bcc5f6;
constexpr std::uint32_t kProcessParamBytes = 0x20;
constexpr std::uint64_t kElfHeaderBytes = 64;
constexpr std::uint64_t kPhdrBytes = 56;
constexpr std::uint64_t kSegmentAlign = 0x10000;
constexpr std::uint16_t kTypePrx = 0xFFA4;
constexpr std::uint32_t kPtSceRelocations = 0x700000A4;
// Module info (u16 attributes, u8 major, u8 minor, char name[28], u32 toc, ...) up to its last field.
constexpr std::uint64_t kModuleInfoBytes = 0x34;
constexpr std::uint64_t kModuleNameBytes = 28;

// Bounds-checked big-endian reads from the file image.
struct ByteView {
    const std::vector<std::uint8_t>& bytes;

    std::uint64_t read(std::uint64_t offset, int size) const {
        std::uint64_t value = 0;
        for (int i = 0; i < size; i++) {
            value = (value << 8) | bytes[offset + i];
        }
        return value;
    }
    std::uint16_t u16(std::uint64_t offset) const { return static_cast<std::uint16_t>(read(offset, 2)); }
    std::uint32_t u32(std::uint64_t offset) const { return static_cast<std::uint32_t>(read(offset, 4)); }
    std::uint64_t u64(std::uint64_t offset) const { return read(offset, 8); }
};

struct ProgramHeader {
    std::uint32_t type, flags;
    std::uint64_t offset, vaddr, filesz, memsz;
    std::uint64_t paddr = 0;
};

std::uint64_t alignUp(std::uint64_t value, std::uint64_t align) { return (value + align - 1) / align * align; }

std::string sha256Hex(const std::vector<std::uint8_t>& data) {
    std::array<std::uint8_t, 32> digest{};
    const NTSTATUS status =
            BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data.data()),
                       static_cast<ULONG>(data.size()), digest.data(), static_cast<ULONG>(digest.size()));
    if (status < 0) {
        return {};
    }
    std::string hex;
    for (const std::uint8_t byte : digest) {
        hex += std::format("{:02x}", byte);
    }
    return hex;
}

}  // namespace

std::expected<LoadedExecutable, std::string> loadExecutable(const std::filesystem::path& elf) {
    using Error = std::unexpected<std::string>;

    std::ifstream in(elf, std::ios::binary);
    if (!in) {
        return Error(std::format("cannot open {}", elf.string()));
    }
    const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const ByteView v{file};

    if (file.size() < kElfHeaderBytes || std::memcmp(file.data(),
                                                     "\x7f"
                                                     "ELF",
                                                     4) != 0) {
        return Error("not an ELF file");
    }
    if (file[4] != 2 || file[5] != 2) {
        return Error("not a 64-bit big-endian ELF");
    }
    if (v.u16(18) != kMachinePpc64) {
        return Error(std::format("unexpected e_machine {} (expected {})", v.u16(18), kMachinePpc64));
    }

    const std::uint64_t entry = v.u64(24);
    const std::uint64_t phoff = v.u64(32);
    const std::uint64_t phentsize = v.u16(54);
    const std::uint64_t phnum = v.u16(56);
    if (phentsize < kPhdrBytes || phoff > file.size() || phnum * phentsize > file.size() - phoff) {
        return Error("program header table is outside the file");
    }

    LoadedExecutable exe;
    exe.sdk_version = 0xffffffffu;
    exe.primary_prio = 1001;
    exe.primary_stacksize = 0x100000;
    exe.malloc_pagesize = 0x100000;
    exe.entry_opd = static_cast<std::uint32_t>(entry);

    std::vector<ProgramHeader> loads;
    std::uint64_t image_begin = ~0ull;
    std::uint64_t image_end = 0;

    for (std::uint64_t i = 0; i < phnum; i++) {
        const std::uint64_t at = phoff + i * phentsize;
        const ProgramHeader ph{v.u32(at), v.u32(at + 4), v.u64(at + 8), v.u64(at + 16), v.u64(at + 32), v.u64(at + 40)};
        if (ph.offset > file.size() || ph.filesz > file.size() - ph.offset) {
            return Error(std::format("program header {} reaches past the end of the file", i));
        }

        // Same predicate as RPCS3 `ppu_load_exec`: raw flags equal to their low three bits mark a
        // very old executable with a private area below 0x10000000.
        const bool low_flags = (ph.flags & 7) == ph.flags;
        logInfo("tox2: load: phdr {} type {:#x} flags {:#010x} vaddr {:#010x} filesz {:#x} memsz {:#x} "
                "(flags & 7) == flags: {}",
                i, ph.type, ph.flags, ph.vaddr, ph.filesz, ph.memsz, low_flags ? "yes" : "no");

        if (ph.type == kPtLoad && ph.filesz != 0 && low_flags && ph.vaddr < kMainRegion.end) {
            return Error("executable with a private PPU area is not supported");
        }

        if (ph.type == kPtLoad && ph.memsz != 0) {
            if (ph.filesz > ph.memsz || ph.vaddr < kMainRegion.begin || ph.vaddr + ph.memsz > kMainRegion.end) {
                return Error(std::format("segment {:#x}+{:#x} is outside the main region", ph.vaddr, ph.memsz));
            }
            loads.push_back(ph);
            image_begin = std::min(image_begin, ph.vaddr & ~(kSegmentAlign - 1));
            image_end = std::max(image_end, alignUp(ph.vaddr + ph.memsz, kSegmentAlign));
        } else if (ph.type == kPtTls) {
            exe.tls_vaddr = static_cast<std::uint32_t>(ph.vaddr);
            exe.tls_filesz = static_cast<std::uint32_t>(ph.filesz);
            exe.tls_memsz = static_cast<std::uint32_t>(ph.memsz);
        } else if (ph.type == kPtProcessParam && ph.filesz != 0) {
            if (ph.filesz < kProcessParamBytes || v.u32(ph.offset + 4) != kProcessParamMagic) {
                return Error("PT 0x60000001 is not a process parameter block");
            }
            exe.sdk_version = v.u32(ph.offset + 12);
            exe.primary_prio = static_cast<std::int32_t>(v.u32(ph.offset + 16));
            exe.primary_stacksize = v.u32(ph.offset + 20);
            exe.malloc_pagesize = v.u32(ph.offset + 24);
            exe.ppc_seg = v.u32(ph.offset + 28);
        }
    }
    if (loads.empty()) {
        return Error("no PT_LOAD segment");
    }
    exe.image_end = static_cast<std::uint32_t>(image_end);

    Process& p = process();
    std::uint8_t* base = p.memory.base();
    if (base == nullptr) {
        return Error("guest memory is not reserved");
    }
    for (const ProgramHeader& ph : loads) {
        const std::uint64_t begin = ph.vaddr & ~(kSegmentAlign - 1);
        const std::uint64_t end = alignUp(ph.vaddr + ph.memsz, kSegmentAlign);
        if (!p.memory.commit(static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end - begin))) {
            return Error(std::format("cannot commit {:#x}..{:#x}", begin, end));
        }
        std::memcpy(base + ph.vaddr, file.data() + ph.offset, ph.filesz);
    }
    p.main.markUsed(static_cast<std::uint32_t>(image_begin), static_cast<std::uint32_t>(image_end - image_begin));

    // e_entry is the address of the entry's function descriptor: code address, then TOC.
    if (entry < kMainRegion.begin || entry + 8 > image_end) {
        return Error(std::format("entry descriptor {:#x} is outside the image", entry));
    }
    exe.entry_func = tox2Read32(base, exe.entry_opd);
    exe.entry_toc = tox2Read32(base, exe.entry_opd + 4);

    exe.sha256 = sha256Hex(file);
    if (exe.sha256.empty()) {
        return Error("SHA-256 computation failed");
    }
    return exe;
}

std::expected<LoadedModule, std::string> loadModule(const std::filesystem::path& path, std::uint32_t expected_base,
                                                    std::uint32_t lowest_free) {
    using Error = std::unexpected<std::string>;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return Error(std::format("cannot open {}", path.string()));
    }
    const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const ByteView v{file};

    if (file.size() < kElfHeaderBytes || std::memcmp(file.data(),
                                                     "\x7f"
                                                     "ELF",
                                                     4) != 0) {
        return Error("not an ELF file");
    }
    if (file[4] != 2 || file[5] != 2) {
        return Error("not a 64-bit big-endian ELF");
    }
    if (v.u16(18) != kMachinePpc64) {
        return Error(std::format("unexpected e_machine {} (expected {})", v.u16(18), kMachinePpc64));
    }
    if (v.u16(16) != kTypePrx) {
        return Error(std::format("unexpected e_type {:#x} (expected {:#x}, a PRX)", v.u16(16), kTypePrx));
    }

    const std::uint64_t phoff = v.u64(32);
    const std::uint64_t phentsize = v.u16(54);
    const std::uint64_t phnum = v.u16(56);
    if (phentsize < kPhdrBytes || phoff > file.size() || phnum * phentsize > file.size() - phoff) {
        return Error("program header table is outside the file");
    }

    const std::string file_name = path.filename().string();
    std::vector<ProgramHeader> headers;
    for (std::uint64_t i = 0; i < phnum; i++) {
        const std::uint64_t at = phoff + i * phentsize;
        const ProgramHeader ph{v.u32(at),      v.u32(at + 4),  v.u64(at + 8), v.u64(at + 16),
                               v.u64(at + 32), v.u64(at + 40), v.u64(at + 24)};
        if (ph.offset > file.size() || ph.filesz > file.size() - ph.offset) {
            return Error(std::format("program header {} reaches past the end of the file", i));
        }
        logInfo("tox2: load: {} phdr {} type {:#x} flags {:#010x} vaddr {:#010x} paddr {:#010x} filesz {:#x} "
                "memsz {:#x}",
                file_name, i, ph.type, ph.flags, ph.vaddr, ph.paddr, ph.filesz, ph.memsz);
        headers.push_back(ph);
    }

    for (const ProgramHeader& ph : headers) {
        if (ph.type == kPtSceRelocations) {
            return Error("relocations not applied; pass libsre.reloc.elf written by tox2_decrypt reloc");
        }
    }
    std::vector<ProgramHeader> loads;
    for (const ProgramHeader& ph : headers) {
        if (ph.type == kPtLoad && ph.memsz != 0) {
            loads.push_back(ph);
        }
    }
    if (loads.empty()) {
        return Error("no PT_LOAD segment");
    }
    if (headers.empty() || headers[0].type != kPtLoad) {
        return Error("program header 0 is not PT_LOAD");
    }

    for (const ProgramHeader& ph : loads) {
        if (ph.filesz > ph.memsz || ph.vaddr < kMainRegion.begin || ph.vaddr + ph.memsz > kMainRegion.end) {
            return Error(std::format("segment {:#x}+{:#x} is outside the main region or has more file data than memory",
                                     ph.vaddr, ph.memsz));
        }
    }
    std::ranges::sort(loads, {}, &ProgramHeader::vaddr);
    for (std::size_t i = 1; i < loads.size(); i++) {
        if (loads[i - 1].vaddr + loads[i - 1].memsz > loads[i].vaddr) {
            return Error(std::format("segments {:#x}+{:#x} and {:#x} overlap", loads[i - 1].vaddr, loads[i - 1].memsz,
                                     loads[i].vaddr));
        }
    }

    const std::uint64_t lowest = loads.front().vaddr;
    std::uint64_t image_end = 0;
    for (const ProgramHeader& ph : loads) {
        image_end = std::max(image_end, alignUp(ph.vaddr + ph.memsz, kSegmentAlign));
    }
    if (lowest != expected_base) {
        return Error(std::format("lowest segment is at {:#010x}, expected {:#010x}", lowest, expected_base));
    }
    if (expected_base < lowest_free) {
        return Error(std::format("base {:#010x} is below the first free address {:#010x}", expected_base, lowest_free));
    }

    // The module info is addressed by the file offset in p_paddr of program header 0.
    const ProgramHeader& first = headers[0];
    if (first.paddr < first.offset || first.paddr - first.offset > first.filesz ||
        first.filesz - (first.paddr - first.offset) < kModuleInfoBytes) {
        return Error(std::format("module info at file offset {:#x} is outside segment 0's file data", first.paddr));
    }
    const std::uint64_t info_offset = first.paddr;
    const std::uint64_t info_address = first.vaddr + (first.paddr - first.offset);

    LoadedModule module;
    module.version_major = file[info_offset + 2];
    module.version_minor = file[info_offset + 3];
    for (std::uint64_t i = 0; i < kModuleNameBytes && file[info_offset + 4 + i] != 0; i++) {
        module.name.push_back(static_cast<char>(file[info_offset + 4 + i]));
    }
    module.toc = v.u32(info_offset + 4 + kModuleNameBytes);
    module.base = static_cast<std::uint32_t>(lowest);
    module.image_end = static_cast<std::uint32_t>(image_end);
    module.module_info = static_cast<std::uint32_t>(info_address);
    module.sha256 = sha256Hex(file);
    if (module.sha256.empty()) {
        return Error("SHA-256 computation failed");
    }

    Process& p = process();
    std::uint8_t* base = p.memory.base();
    if (base == nullptr) {
        return Error("guest memory is not reserved");
    }
    for (const ProgramHeader& ph : loads) {
        const std::uint64_t begin = ph.vaddr & ~(kSegmentAlign - 1);
        const std::uint64_t end = alignUp(ph.vaddr + ph.memsz, kSegmentAlign);
        if (!p.memory.commit(static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end - begin))) {
            return Error(std::format("cannot commit {:#x}..{:#x}", begin, end));
        }
        std::memcpy(base + ph.vaddr, file.data() + ph.offset, ph.filesz);
    }
    p.main.markUsed(module.base, module.image_end - module.base);
    return module;
}

}  // namespace tox2
