#pragma once

// Native entry points of the PPU thread family shared by the syscall and HLE handlers. Everything
// here runs under the guest lock.

#include <cstdint>

namespace tox2 {

// RPCS3 `Modules/sys_ppu_thread_.cpp` `sys_ppu_thread_create`: takes a TLS block, creates the
// thread (`lv2/sys_ppu_thread.cpp` `_sys_ppu_thread_create`) and starts it unless
// kCreateInterrupt is set. `thread_id` is the guest address that receives the id as a BE u64,
// `entry` the guest address of the entry function descriptor, `name` a guest C string or 0.
// Returns a cell error code.
std::uint32_t ppuThreadCreate(std::uint8_t* base, std::uint32_t thread_id, std::uint32_t entry, std::uint64_t arg,
                              std::int32_t priority, std::uint32_t stack_size, std::uint64_t flags, std::uint32_t name);

// Forgets which threads were joined. Intended for tests.
void resetPpuThreadState();

}  // namespace tox2
