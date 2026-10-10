#pragma once

// SDAT container decryption (the NPD/EDAT layout of PS3 "SDAT" data files). The key constants are
// never part of this repository: loadKeys() reads them from a local RPCS3 source checkout at run
// time. The container format follows RPCS3 `Crypto/unedat.cpp` (behaviour only; the code here is
// independent). Block decompression lives in sdat_lz.h.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

#include "sdat_lz.h"
#include <string>
#include <vector>

namespace dal::sdat {

using Bytes = std::vector<std::uint8_t>;
using Block16 = std::array<std::uint8_t, 16>;

struct Keys {
    Block16 sdat_key{};
    Block16 edat_key_0{};
    Block16 edat_key_1{};
    Block16 edat_iv{};
};

// `path` is a key_vault.h / key_vault.cpp file or the directory holding them. The constants are
// found by name, as byte lists in the source text. Returns false and sets `error` when one is
// missing or has the wrong size.
bool loadKeys(const std::filesystem::path& path, Keys& keys, std::string& error);

// AES-128 primitives over Windows CNG. Lengths of the CBC calls must be a multiple of 16.
bool aesEcbEncrypt(const Block16& key, const Block16& in, Block16& out);
bool aesCbcEncrypt(const Block16& key, const Block16& iv, std::span<const std::uint8_t> in, std::uint8_t* out);
bool aesCbcDecrypt(const Block16& key, const Block16& iv, std::span<const std::uint8_t> in, std::uint8_t* out);
bool aesCmac(const Block16& key, std::span<const std::uint8_t> data, Block16& out);

struct SdatInfo {
    std::int32_t version = 0;
    std::int32_t license = 0;
    std::uint32_t flags = 0;
    std::uint32_t block_size = 0;
    std::uint64_t file_size = 0;
    std::uint32_t blocks = 0;
    std::uint32_t compressed_blocks = 0;
};

// Decrypts a whole SDAT file image. Every block's AES-CMAC is verified, so a wrong key fails.
// On failure `error` says why; `info` is filled as far as the header was read.
bool decryptSdat(std::span<const std::uint8_t> file, const Keys& keys, Bytes& out, SdatInfo& info, std::string& error);

}  // namespace dal::sdat
