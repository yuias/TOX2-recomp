#pragma once

// PARAM.SFO ("PSF") reader and writer. Little-endian: header {magic "\0PSF", version, key table
// offset, data table offset, entry count}, then 16-byte index entries {key offset u16, format u16,
// length u32, max length u32, data offset u32}. Format 0x0404 is a u32, 0x0204 and 0x0004 are
// UTF-8 text (0x0204 is NUL-terminated).

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace dal {

struct ParamSfo {
    std::map<std::string, std::uint32_t> ints;
    std::map<std::string, std::string> strings;

    bool empty() const { return ints.empty() && strings.empty(); }
};

// Entries with an unknown format are skipped. Errors: bad magic, truncated table, or an entry
// whose key or data lies outside the file.
std::expected<ParamSfo, std::string> parseParamSfo(std::span<const std::uint8_t> bytes);

// Reads and parses `path`; the error text starts with "cannot read" when the file is unavailable.
std::expected<ParamSfo, std::string> readParamSfoFile(const std::filesystem::path& path);

// Serialises with version 0x101, entries ordered by key. Text entries use the 0x0204 format and a
// capacity of at least the console's usual size for that key (TITLE 128, TITLE_ID 16, VERSION 8,
// CATEGORY 4), rounded up to a multiple of 4.
std::vector<std::uint8_t> serializeParamSfo(const ParamSfo& sfo);

// The running game's PARAM.SFO. Set once before boot and read-only afterwards. Until it is set (or
// when the game has none) it holds RESOLUTION 0x1 (480 only) and SOUND_FORMAT 0x1 (LPCM 2 ch).
const ParamSfo& gameParamSfo();
void setGameParamSfo(ParamSfo sfo);

}  // namespace dal
