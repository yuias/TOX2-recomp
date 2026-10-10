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
#include "dal/ppu_context.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

namespace dal {
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
        logInfo("dal: load: phdr {} type {:#x} flags {:#010x} vaddr {:#010x} filesz {:#x} memsz {:#x} "
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
    exe.entry_func = dalRead32(base, exe.entry_opd);
    exe.entry_toc = dalRead32(base, exe.entry_opd + 4);

    exe.sha256 = sha256Hex(file);
    if (exe.sha256.empty()) {
        return Error("SHA-256 computation failed");
    }
    return exe;
}

}  // namespace dal
