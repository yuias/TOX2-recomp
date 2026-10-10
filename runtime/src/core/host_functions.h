#pragma once

#include <cstdint>

#include "lv2/syscalls.h"

namespace dal {

// Entry keys at the top of the address space: never committed, never in the code span.
inline constexpr std::uint32_t kHostFunctionBase = 0xFFFF0000;
inline constexpr std::uint32_t kMaxHostFunctions = 0x2000;

// Registers a host function that guest code can call or start a thread on through a function
// descriptor. Returns the guest address of an OPD {kHostFunctionBase + 4 * i, 0}; dalLookup resolves
// that key to `handler`. The OPDs live in one 64 KiB block taken from process().main at the first
// registration, so call this after the ELF image is loaded, under the guest lock or before boot.
// A host function behaves like a guest leaf function: arguments r3..r10, result r3, returning acts
// as blr.
std::uint32_t registerHostFunction(const char* name, GuestHandler handler);

// Forgets every registered host function and its OPD block. For tests that re-reserve guest memory.
void resetHostFunctions();

}  // namespace dal
