#pragma once

// Counts what the RSX state decoders, the frame builder and the renderer meet but cannot handle yet
// (formats, opcodes, primitives, state), so later scenes turn into a work list. Categories in use:
// "surface format", "depth format", "texture format", "texture layout", "vertex type", "primitive",
// "fp opcode", "vp opcode", "vp sca opcode", "blend", "state", "method", "capture".

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tox2 {

// Thread-safe. Counts one occurrence of "<category> <key>".
void rsxNoteUnimplemented(std::string_view category, std::string_view key);

// "<category> <key>" with its count, sorted by count (descending) and then by name. The number of
// distinct kinds is capped; occurrences of further new kinds are only summed in rsxUnimplementedDropped().
std::vector<std::pair<std::string, std::uint64_t>> rsxUnimplementedSnapshot();
std::uint64_t rsxUnimplementedDropped();

// Forgets all counts. Intended for tests.
void rsxUnimplementedReset();

}  // namespace tox2
