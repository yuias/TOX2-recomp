#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "crypto.h"

namespace tox2::decrypt {

struct RelocCounts {
    std::uint32_t addr32 = 0, addr16_lo = 0, addr16_hi = 0, addr16_ha = 0;
    std::uint32_t total() const { return addr32 + addr16_lo + addr16_hi + addr16_ha; }
};

// Rebases a pre-linked PRX (ELF64, e_type 0xFFA4, one SCE_PPURELA header) to `base`: the LOAD segments are
// placed contiguously at `base`, the relocation records are applied, and the SCE_PPURELA header becomes
// PT_NULL so no loader applies them a second time. `base` must be non-zero and a multiple of 0x10000, which
// is what lets ADDR16_LO stay as it is and ADDR16_HA change by exactly `base >> 16` for any pre-linked value.
// `out` is replaced and has the size of `prx`. `image_end` receives `base` + the highest LOAD end after
// the overlap between segments is trimmed. On failure `error` names the cause and `out` is left empty.
bool relocatePrx(std::span<const std::uint8_t> prx, std::uint32_t base, Bytes& out, RelocCounts& counts,
                 std::uint32_t& image_end, std::string& error);

}  // namespace tox2::decrypt
