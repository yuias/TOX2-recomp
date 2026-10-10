#pragma once

#include <cstdint>
#include <span>

namespace dal::sdat {

// Decodes one compressed SDAT data block into `out` (capacity `out.size()`). Returns the number of
// bytes produced, or -1 when the block is malformed. A block whose first byte is above 0x80 is
// stored with a 4-byte length and copied as is.
std::int64_t lzDecompress(std::span<const std::uint8_t> in, std::span<std::uint8_t> out);

}  // namespace dal::sdat
