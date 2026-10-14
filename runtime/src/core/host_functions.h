#pragma once

#include <cstdint>

#include "lv2/syscalls.h"

namespace tox2 {

// Entry keys at the top of the address space: never committed, never in the code span.
inline constexpr std::uint32_t kHostFunctionBase = 0xFFFF0000;
inline constexpr std::uint32_t kMaxHostFunctions = 0x2000;

// Registers a host function that guest code can call or start a thread on through a function
// descriptor. Returns the guest address of an OPD {kHostFunctionBase + 4 * i, 0}; tox2Lookup resolves
// that key to `handler`. The OPDs live in one 64 KiB block taken from process().main at the first
// registration, so call this after the ELF image is loaded. It opens a KernelSection itself, so it can be called
// from guest threads in the split modes, and tox2Lookup resolves the keys without a lock.
// A host function behaves like a guest leaf function: arguments r3..r10, result r3, returning acts
// as blr. It runs as guest code, so in the split modes it holds no lock on entry and must open a KernelSection
// before it touches lv2 or HLE state.
std::uint32_t registerHostFunction(const char* name, GuestHandler handler);

// Forgets every registered host function and its OPD block. For tests that re-reserve guest memory.
void resetHostFunctions();

}  // namespace tox2
