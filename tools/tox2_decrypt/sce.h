#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "crypto.h"
#include "keys.h"

namespace tox2::decrypt {

struct SceHeader {
    std::uint32_t version;
    std::uint16_t flags;  // key revision
    std::uint16_t type;
    std::uint32_t metadata_offset;  // relative to the end of the 0x20-byte header
    std::uint64_t header_length;
    std::uint64_t data_length;
};

struct SceSection {
    std::uint64_t offset;
    std::uint64_t size;
    std::uint32_t type;
    std::uint32_t program_index;
    std::uint32_t encrypted;  // 3 = AES-128-CTR
    std::uint32_t key_index;
    std::uint32_t iv_index;
    std::uint32_t compressed;  // 2 = zlib
};

struct SceMetadata {
    std::vector<SceSection> sections;
    std::vector<Block16> key_slots;
};

// Magic "SCE\0" and 0x20 bytes present.
bool readSceHeader(std::span<const std::uint8_t> file, SceHeader& header, std::string& error);

// Checks the header against the file, decrypts the metadata info with `erk`/`riv` (not when flags has
// 0x8000), rejects a wrong key, decrypts the metadata region and parses its sections and key slots.
bool readSceMetadata(std::span<const std::uint8_t> file, const SceHeader& header, const Key256& erk, const Block16& riv,
                     SceMetadata& metadata, std::string& error);

// Decrypts an SCE package (a firmware dev_flash_* file) with the package keys. One output per section,
// in metadata order.
bool decryptScePackage(std::span<const std::uint8_t> file, const KeyVault& keys, std::vector<Bytes>& outputs,
                       std::string& error);

}  // namespace tox2::decrypt
