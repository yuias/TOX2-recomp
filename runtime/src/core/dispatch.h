#pragma once

#include <cstdint>

#include "tox2/ppu_context.h"

namespace tox2 {

// Adds the functions of a module as its own span next to the ones tox2RegisterFuncs set up. Same checks
// as tox2RegisterFuncs (aligned, sorted, non-null); a span that overlaps an existing one is a fatal
// error. Call before any guest thread starts.
void appendFuncs(const Tox2FuncMapping* mappings, std::uint32_t count);

}  // namespace tox2
