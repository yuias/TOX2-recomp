#pragma once

#include <cstdint>
#include <string_view>

#include "core/elf_loader.h"
#include "tox2/ppu_context.h"

namespace tox2 {

// Process-wide thread-local storage setup of RPCS3 `sys_initialize_tls`: does nothing when r13 is
// already set; otherwise reserves the TLS area, gives the calling thread its block and stores the
// thread pointer in ctx->gpr[13]. Returns r13. Call with the kernel lock held (or before any guest
// thread runs). The internal lwmutexes and mutexes the HLE version also creates are not made here.
std::uint32_t initializeTls(Tox2PpuContext* ctx, std::uint8_t* base, std::uint32_t thread_id, std::uint32_t vaddr,
                            std::uint32_t filesz, std::uint32_t memsz);

// Takes a TLS block for a new thread and returns its r13, or 0 when initializeTls has not run or
// memory is exhausted.
std::uint32_t allocateTls();

// Gives back the block of a thread whose r13 is `r13` (RPCS3 `ppu_free_tls`). Call with the kernel
// lock held.
void freeTls(std::uint32_t r13);

// Creates the main thread exactly as RPCS3 `ppu_load_exec` does (stack, argument block, TLS,
// entry registers), logs the resulting registers and starts it. Legacy mode: the thread runs
// under the guest lock, so it begins once this function has returned. Split modes: it may run guest code at once, but
// its first kernel section waits for this function to return. `module_bytes` (the loaded PRX images) is
// charged to user memory together with the stack and the executable image.
void bootMainThread(const LoadedExecutable& exe, std::string_view argv0, std::uint32_t module_bytes = 0);

}  // namespace tox2
