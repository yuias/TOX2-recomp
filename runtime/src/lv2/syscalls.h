#pragma once

#include <cstdint>
#include <span>

#include "tox2/ppu_context.h"

namespace tox2 {

// The process id that sys_process_getpid reports and that unnamed event ports use in their source.
inline constexpr uint32_t kProcessId = 1;

// The SDK version that sys_process_get_sdk_version reports (taken from the executable's process
// parameters). Zero until set.
void setProcessSdkVersion(uint32_t version);

// Arguments arrive in r3..r10 (syscalls) or r3..r8 (HLE calls); the result goes to r3.
using GuestHandler = void (*)(Tox2PpuContext* ctx, uint8_t* base);

struct SyscallEntry {
    uint32_t number;
    const char* name;      // null when the number has no known name
    GuestHandler handler;  // null: the default handler runs
    // Split PPU modes: the handler runs without the kernel lock. Only for a handler that touches nothing but its
    // context, thread-locals, guest memory and atomics (it must not block, allocate guest memory or touch lv2/HLE
    // state).
    bool kernel_free = false;
};

// Every syscall the game can issue plus every implemented one, sorted by number.
std::span<const SyscallEntry> syscallTable();

// Returns nullptr for numbers that are not in the table.
const SyscallEntry* findSyscall(uint32_t number);

}  // namespace tox2
