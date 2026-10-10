#pragma once

// A C printf for guest format strings. The PPC64 ELFv1 ABI gives every variadic argument one 8-byte
// slot (doubles included), so a format needs no per-type bookkeeping beyond the conversion itself.

#include <cstdint>
#include <string>

#include "tox2/ppu_context.h"

namespace tox2 {

// Where the variable arguments come from. Slot k is the k-th 8-byte parameter word counted from the
// first parameter of the call (the format string's own slot included).
struct GuestVarArgs {
    // r3 + k for k < 8, else the caller's parameter save area at r1 + 0x30 + 8k. `first_slot` is the slot of
    // the first variable argument (1 for printf, 3 for snprintf). The context is read at each next(), so it
    // must outlive the formatting.
    static GuestVarArgs fromRegisters(const Tox2PpuContext* ctx, std::uint32_t first_slot);
    // 8-byte slots at va, va + 8, ... (a PPC64 ELFv1 va_list is a pointer to the next slot).
    static GuestVarArgs fromVaList(std::uint32_t va);

    std::uint64_t next(const std::uint8_t* base);

    const Tox2PpuContext* ctx = nullptr;  // null: read from guest memory at `address`
    std::uint32_t slot = 0;               // next register-passed slot
    std::uint32_t address = 0;            // next va list slot
};

// Formats `fmt` (a guest string) like C printf. Returns the text; on an unsupported conversion sets
// `error` (empty otherwise) and returns what was formatted before it.
std::string formatGuestPrintf(const std::uint8_t* base, std::uint32_t fmt, GuestVarArgs& args, std::string& error);

}  // namespace tox2
