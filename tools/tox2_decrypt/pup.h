#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "crypto.h"
#include "keys.h"

namespace tox2::decrypt {

// Checks the PUP magic, sizes and the bounds of every entry, finds the first entry with `id` and verifies
// its HMAC-SHA1 (key PUP_KEY) against the paired hash table digest. `data` points into `pup`.
bool readPupEntry(std::span<const std::uint8_t> pup, const KeyVault& keys, std::uint64_t id,
                  std::span<const std::uint8_t>& data, std::string& error);

// Entry 0x100 up to the first '\n' (exclusive), with one trailing '\r' removed.
bool readPupFirmware(std::span<const std::uint8_t> pup, const KeyVault& keys, std::string& version, std::string& error);

struct TarEntry {
    std::string path;      // prefix (to its first NUL) + name (to its first NUL), no separator
    char type_flag;        // '0', '\0' or '5'; other values are kept as read
    std::uint64_t offset;  // of the data, in the archive
    std::uint64_t size;
};

// Scans a ustar archive with these rules: blocks without the `ustar` magic or with an
// unusable size are skipped, a later duplicate path replaces an earlier one. Sorted by path.
std::vector<TarEntry> scanTar(std::span<const std::uint8_t> archive);

struct PupExtract {
    std::string package;  // outer TAR entry that held the file
    Bytes data;           // raw TAR payload (still encrypted .sprx)
};

// Searches the dev_flash_* packages of the PUP (entry 0x300 is the outer TAR) in name order for the first
// regular file whose path equals `path`.
bool extractFromPup(std::span<const std::uint8_t> pup, const KeyVault& keys, std::string_view path, PupExtract& result,
                    std::string& error);

}  // namespace tox2::decrypt
