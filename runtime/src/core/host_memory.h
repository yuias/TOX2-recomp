#pragma once

// Virtual memory and access-fault reporting of the host OS, without OS types: VirtualAlloc and a
// vectored exception handler on Windows, mmap and sigaction elsewhere. Which implementation is built
// is decided by the build files.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace tox2::host {

// Host page size: 4 KiB on Windows x64, 16 KiB on Apple Silicon. All addresses and sizes passed to
// commit() and decommit() are multiples of it.
std::size_t pageSize();

// Reserves `bytes` of inaccessible address space. Returns nullptr on failure.
void* reserve(std::size_t bytes);

// Makes [address, address + bytes) readable and writable. Pages that are already committed keep
// their contents. Returns false on failure.
bool commit(void* address, std::size_t bytes);

// Makes [address, address + bytes) inaccessible and discards the contents, so the pages are
// zero-filled when committed again. The address space stays reserved.
void decommit(void* address, std::size_t bytes);

#if TOX2_RSX_WRITE_WATCH
// The OS write watch, which only Windows has (MEM_WRITE_WATCH); the build turns TOX2_RSX_WRITE_WATCH
// off elsewhere. Like reserve(), with the watch on for the whole reservation until it is released.
void* reserveWriteWatched(std::size_t bytes);

// The pages of [address, address + bytes) written since the previous call that covered them, with
// their state reset in the same step. `pages` must have room for every page of the range; the first
// `count` entries receive the pages found. `granularity` receives the page size the OS reports in.
// Returns false on failure.
bool collectWrittenPages(void* address, std::size_t bytes, std::span<void*> pages, std::size_t& count,
                         std::size_t& granularity);
#endif

// Gives back a whole reservation. `bytes` is the size passed to reserve().
void release(void* address, std::size_t bytes);

enum class PageState { Free, Reserved, Committed };

// State of the page holding `address`, for tests and diagnostics.
PageState queryPage(const void* address);

// True when every byte of [address, address + bytes) is committed and readable (bytes == 0 checks
// the byte at `address`). Used by the fatal-stop stack walk.
bool readable(const void* address, std::size_t bytes);

// Description of the last failed call on this thread, for log lines.
std::string lastErrorText();

enum class FaultAccess { Read, Write, Execute };

struct FaultInfo {
    bool stack_overflow;     // the host stack of the faulting thread ran out
    std::uintptr_t address;  // faulting address; meaningless when stack_overflow
    FaultAccess access;
};

// Called for access faults on any thread. Returning declines the fault (it is passed on as if no
// handler existed); a handler that takes the fault does not return (it ends the process). On POSIX,
// declining puts the previous signal action back for the whole process, so later faults of that
// signal no longer reach the handler.
using FaultHandler = void (*)(const FaultInfo&);

// Installs `handler` for the process. Call it once.
void installFaultHandler(FaultHandler handler);

// Reserves extra stack for the calling thread so the handler still has room to run after a stack
// overflow. Call it on every thread that runs guest code.
void reserveFaultStack();

}  // namespace tox2::host
