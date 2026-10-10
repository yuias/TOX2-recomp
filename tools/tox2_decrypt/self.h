#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "crypto.h"
#include "keys.h"

namespace tox2::decrypt {

struct SelfInfo {
    std::uint16_t key_revision = 0;
    std::uint32_t program_type = 0;
    std::uint64_t program_version = 0;
    std::uint32_t section_count = 0;
};

// Turns an ELF64 SELF (retail, no NPDRM layer) into the plain ELF. `info` is filled as far as the file
// was parsed, also on failure. Refuses what it does not implement (debug and NPDRM SELFs, ELF32) and
// layouts that would need RPCS3's shared-buffer cursor quirk, with a message in `error`.
bool decryptSelf(std::span<const std::uint8_t> file, const KeyVault& keys, Bytes& elf, SelfInfo& info,
                 std::string& error);

}  // namespace tox2::decrypt
