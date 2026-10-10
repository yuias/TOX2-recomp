#pragma once

#include <cstdint>
#include <span>

#include "dal/ppu_context.h"

namespace dal {

// Arguments arrive in r3..r10 (syscalls) or r3..r8 (HLE calls); the result goes to r3.
using GuestHandler = void (*)(DalPpuContext* ctx, uint8_t* base);

struct SyscallEntry {
    uint32_t number;
    const char* name;      // null when the number has no known name
    GuestHandler handler;  // null: the default handler runs
};

// Every syscall the game can issue plus every implemented one, sorted by number.
std::span<const SyscallEntry> syscallTable();

// Returns nullptr for numbers that are not in the table.
const SyscallEntry* findSyscall(uint32_t number);

}  // namespace dal
