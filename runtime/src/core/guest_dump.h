#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tox2 {

// A guest range written to a host file when the process stops.
struct GuestDumpSpec {
    std::uint32_t addr;
    std::uint32_t size;
    std::string file;
};

// Parses "ADDR:SIZE:FILE". ADDR and SIZE are decimal or 0x-prefixed hex, SIZE is at least 1 and the range
// must lie inside the 32-bit guest space. FILE is everything after the second colon, so it may hold a
// drive letter. On failure returns nullopt and sets `error`.
std::optional<GuestDumpSpec> parseGuestDumpSpec(std::string_view text, std::string& error);

// Adds a dump. The first call installs the stop hook that writes every dump, in the order added, before the
// census. A range that is not fully committed at the stop is logged as such and writes no file.
bool addGuestDump(GuestDumpSpec spec);

}  // namespace tox2
