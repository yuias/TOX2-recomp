#pragma once

// Guest addresses of the relocated firmware SPURS module (libsre.reloc.elf) that the runtime uses
// directly. Each is the module's link offset plus kBase. The module is identified by the SHA-256 pin
// of the whole file; checkLibsreLayout is a second, structural check that fails loudly if an offset
// below no longer matches the image that was loaded.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "tox2/ppu_context.h"

namespace tox2::libsre {

inline constexpr std::uint32_t kBase = 0x011D0000;  // segment 0 (link address 0)

inline constexpr std::uint32_t kCodeEnd = kBase + 0x1D718;    // end of code = first import thunk
inline constexpr std::uint32_t kThunksEnd = kBase + 0x1DB18;  // end of the 32 thunks
inline constexpr std::uint32_t kThunkBytes = 32;              // every thunk has the same size
inline constexpr std::uint32_t kExportsBegin = kBase + 0x1DB24;
inline constexpr std::uint32_t kExportsEnd = kBase + 0x1DBE8;
inline constexpr std::uint32_t kImportsBegin = kBase + 0x1DBF0;
inline constexpr std::uint32_t kImportsEnd = kBase + 0x1DC48;
inline constexpr std::uint32_t kModuleInfo = kBase + 0x1DC4C;

// SPU images embedded in the module. The two kernels are SPU ELF files; the others are raw code.
inline constexpr std::uint32_t kSpuKernel1 = kBase + 0x20380;  // entry point 0x818
inline constexpr std::uint32_t kSpuKernel2 = kBase + 0x20C00;  // entry point 0x848
inline constexpr std::uint32_t kSpuSystemService = kBase + 0x21480;
inline constexpr std::uint32_t kSpuTasksetPm = kBase + 0x23680;
inline constexpr std::uint32_t kSpuTaskset2ExitHandler = kBase + 0x25500;
inline constexpr std::uint32_t kSpuExitCodePlugin = kBase + 0x25B80;
inline constexpr std::uint32_t kSpuJobChainPm1 = kBase + 0x26300;
inline constexpr std::uint32_t kSpuJobChainPm2 = kBase + 0x2A180;

inline constexpr std::uint32_t kDataSegment = kBase + 0x2E000;  // segment 1
inline constexpr std::uint32_t kImportSlots = kDataSegment;     // 32 words, one per thunk
inline constexpr std::uint32_t kImportSlotCount = 32;
inline constexpr std::uint32_t kOpdBegin = kBase + 0x30FAC;  // function descriptors {entry, toc}
inline constexpr std::uint32_t kOpdEnd = kBase + 0x31AAC;    // 352 of them
inline constexpr std::uint32_t kToc = kBase + 0x39AB0;

inline constexpr std::uint32_t kImageEnd = 0x01210000;  // 64 KiB-rounded end of the image

// Checks the loaded image (guest memory at `base`, which must be committed over the module) against the
// constants above: module info name, TOC and table ranges; the TOC word and an in-code, 4-aligned entry
// of every function descriptor; the import slots and the generated import stubs inside the thunk area at
// a multiple of the thunk size; the generated export descriptors inside the descriptor area; ELF
// identification and entry point of both SPU kernels; the `ila $2` opcode field of the first word of each
// raw SPU image. Returns one message per failed check and an empty vector when all pass.
std::vector<std::string> checkLibsreLayout(const std::uint8_t* base, std::span<const Tox2ImportInfo> imports,
                                           std::span<const Tox2ExportInfo> exports);

}  // namespace tox2::libsre
