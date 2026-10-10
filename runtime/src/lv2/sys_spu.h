#pragma once

#include <cstdint>

namespace tox2 {

// SPU thread groups are bookkeeping only: groups, threads, their images and event connections are
// recorded and validated, but no SPU thread ever runs. Starting a group ends the run with
// ExitCode::Unsupported after logging what would have started. Ids: groups 0x04000100 + slot * 0x100
// (255 slots), threads (n << 24) | (group id & 0xFFFFFF). Handlers run under the guest lock.

// Forgets every group and thread and restarts the thread counter. Intended for tests.
void resetSpuThreadGroups();

// An SPU ELF imported with spuImageImport. `segs` is the guest address of the segment array that the
// sys_spu_image points to; it identifies the record.
struct SpuImageRecord {
    std::uint32_t source = 0;       // guest address of the ELF32 that was imported
    std::uint32_t import_type = 0;  // 0 PROTECT, 1 DIRECT
    std::uint32_t entry = 0;
    std::uint32_t segs = 0;
    std::int32_t nsegs = 0;
};

// sys_spu_image_import: reads the SPU ELF32 at `source` and fills the 16-byte sys_spu_image at `image`
// with {type 0, entry point, segment array, segment count}. The array of 16-byte sys_spu_segment
// entries {type, ls_start, size, addr_or_value} (1 copy, 2 fill) comes from the main region.
// Returns a cell error code.
std::uint32_t spuImageImport(std::uint8_t* base, std::uint32_t image, std::uint32_t source, std::uint32_t type);

// sys_spu_image_close: frees the segment array of the image and forgets its record.
std::uint32_t spuImageClose(std::uint8_t* base, std::uint32_t image);

// The record of the image whose segment array is at `segs`, or null.
const SpuImageRecord* findSpuImage(std::uint32_t segs);

// Frees every segment array and forgets every record. Intended for tests.
void resetSpuImages();

}  // namespace tox2
