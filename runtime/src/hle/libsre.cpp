#include "hle/libsre.h"

#include <array>
#include <cstring>
#include <format>

namespace tox2::libsre {
namespace {

constexpr char kModuleName[] = "cellSre_Library";

constexpr std::uint32_t kModuleInfoName = 4;
constexpr std::uint32_t kModuleInfoToc = 0x20;
constexpr std::uint32_t kModuleInfoExportsStart = 0x24;
constexpr std::uint32_t kModuleInfoExportsEnd = 0x28;
constexpr std::uint32_t kModuleInfoImportsStart = 0x2C;
constexpr std::uint32_t kModuleInfoImportsEnd = 0x30;

constexpr std::uint32_t kSpuMachine = 23;
constexpr std::uint32_t kSpuKernel1Entry = 0x818;
constexpr std::uint32_t kSpuKernel2Entry = 0x848;

// `ila $2, imm`: the 7-bit opcode field of the RI18 format is 0x21 (the top bits of the word) and RT is
// the low seven bits.
constexpr std::uint32_t kIlaOpcode = 0x21;
constexpr std::uint32_t kIlaTarget = 2;

struct RawImage {
    std::uint32_t address;
    const char* name;
};

constexpr std::array kRawImages{
        RawImage{kSpuSystemService, "system service"},
        RawImage{kSpuTasksetPm, "taskset policy module"},
        RawImage{kSpuTaskset2ExitHandler, "taskset2 exit handler"},
        RawImage{kSpuExitCodePlugin, "exit code plugin"},
        RawImage{kSpuJobChainPm1, "job chain policy module 1"},
        RawImage{kSpuJobChainPm2, "job chain policy module 2"},
};

bool isThunk(std::uint32_t address) {
    return address >= kCodeEnd && address < kThunksEnd && (address - kCodeEnd) % kThunkBytes == 0;
}

void checkSpuKernel(const std::uint8_t* base, std::uint32_t address, const char* name, std::uint32_t entry,
                    std::vector<std::string>& problems) {
    if (std::memcmp(base + address,
                    "\x7f"
                    "ELF",
                    4) != 0) {
        problems.push_back(std::format("{} at {:#010x}: no ELF magic", name, address));
    }
    if (tox2Read8(base, address + 4) != 1) {
        problems.push_back(std::format("{} at {:#010x}: ELF class is {}, expected 1", name, address,
                                       tox2Read8(base, address + 4)));
    }
    if (tox2Read8(base, address + 5) != 2) {
        problems.push_back(std::format("{} at {:#010x}: ELF data encoding is {}, expected 2", name, address,
                                       tox2Read8(base, address + 5)));
    }
    if (tox2Read16(base, address + 0x12) != kSpuMachine) {
        problems.push_back(std::format("{} at {:#010x}: e_machine is {}, expected {}", name, address,
                                       tox2Read16(base, address + 0x12), kSpuMachine));
    }
    if (tox2Read32(base, address + 0x18) != entry) {
        problems.push_back(std::format("{} at {:#010x}: e_entry is {:#x}, expected {:#x}", name, address,
                                       tox2Read32(base, address + 0x18), entry));
    }
}

}  // namespace

std::vector<std::string> checkLibsreLayout(const std::uint8_t* base, std::span<const Tox2ImportInfo> imports,
                                           std::span<const Tox2ExportInfo> exports) {
    std::vector<std::string> problems;

    if (std::memcmp(base + kModuleInfo + kModuleInfoName, kModuleName, sizeof kModuleName) != 0) {
        problems.push_back(std::format("module info at {:#010x}: name is not {}", kModuleInfo, kModuleName));
    }
    const auto check_field = [&](std::uint32_t offset, const char* field, std::uint32_t expected) {
        const std::uint32_t value = tox2Read32(base, kModuleInfo + offset);
        if (value != expected) {
            problems.push_back(std::format("module info at {:#010x}: {} is {:#010x}, expected {:#010x}", kModuleInfo,
                                           field, value, expected));
        }
    };
    check_field(kModuleInfoToc, "TOC", kToc);
    check_field(kModuleInfoExportsStart, "exports start", kExportsBegin);
    check_field(kModuleInfoExportsEnd, "exports end", kExportsEnd);
    check_field(kModuleInfoImportsStart, "imports start", kImportsBegin);
    check_field(kModuleInfoImportsEnd, "imports end", kImportsEnd);

    for (std::uint32_t opd = kOpdBegin; opd < kOpdEnd; opd += 8) {
        const std::uint32_t entry = tox2Read32(base, opd);
        const std::uint32_t toc = tox2Read32(base, opd + 4);
        if (toc != kToc) {
            problems.push_back(
                    std::format("descriptor {:#010x}: TOC word is {:#010x}, expected {:#010x}", opd, toc, kToc));
        }
        if (entry < kBase || entry >= kCodeEnd || entry % 4 != 0) {
            problems.push_back(std::format("descriptor {:#010x}: entry {:#010x} is not a 4-aligned address in the code",
                                           opd, entry));
        }
    }

    for (std::uint32_t i = 0; i < kImportSlotCount; i++) {
        const std::uint32_t slot = tox2Read32(base, kImportSlots + 4 * i);
        if (!isThunk(slot)) {
            problems.push_back(std::format("import slot {} at {:#010x}: {:#010x} is not an import thunk", i,
                                           kImportSlots + 4 * i, slot));
        }
    }
    for (const Tox2ImportInfo& import : imports) {
        if (!isThunk(import.stub)) {
            problems.push_back(std::format("generated import stub {:#010x} (fnid {:#010x}) is not an import thunk",
                                           import.stub, import.fnid));
        }
    }
    for (const Tox2ExportInfo& export_info : exports) {
        if (export_info.opd < kOpdBegin || export_info.opd >= kOpdEnd) {
            problems.push_back(
                    std::format("generated export descriptor {:#010x} (fnid {:#010x}) is outside the "
                                "descriptor area",
                                export_info.opd, export_info.fnid));
        }
    }

    checkSpuKernel(base, kSpuKernel1, "SPU kernel 1", kSpuKernel1Entry, problems);
    checkSpuKernel(base, kSpuKernel2, "SPU kernel 2", kSpuKernel2Entry, problems);
    for (const RawImage& image : kRawImages) {
        const std::uint32_t word = tox2Read32(base, image.address);
        if ((word >> 25) != kIlaOpcode || (word & 0x7F) != kIlaTarget) {
            problems.push_back(std::format("SPU {} at {:#010x}: first word is not an `ila $2` instruction", image.name,
                                           image.address));
        }
    }
    return problems;
}

}  // namespace tox2::libsre
