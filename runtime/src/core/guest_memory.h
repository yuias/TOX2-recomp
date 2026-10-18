#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "core/build_config.h"

namespace tox2 {

// One address range of the guest address space with its own allocator. `guard` bytes below and
// above every allocation stay uncommitted so that a stray access faults instead of corrupting a
// neighbour.
struct GuestRegion {
    std::uint32_t begin, end, granularity, guard;
    const char* name;
};

inline constexpr GuestRegion kMainRegion{0x00010000, 0x10000000, 0x10000, 0, "main"};
inline constexpr GuestRegion kUser1mRegion{0x20000000, 0x30000000, 0x100000, 0, "user1m"};
inline constexpr GuestRegion kUser64kRegion{0x30000000, 0x40000000, 0x10000, 0, "user64k"};
inline constexpr GuestRegion kStackRegion{0xD0000000, 0xE0000000, 0x1000, 0x1000, "stack"};

// RSX ranges are fixed by the console memory map, not handed out by an allocator.
inline constexpr std::uint32_t kRsxContextBegin = 0x10000000;
inline constexpr std::uint32_t kRsxContextEnd = 0x20000000;
inline constexpr std::uint32_t kRsxLocalBegin = 0xC0000000;
inline constexpr std::uint32_t kRsxLocalEnd = 0xD0000000;

// Name of the guest range holding `addr`: "main", "user1m", "user64k", "stack", "rsx-context",
// "rsx-local", or nullptr when it is in none of them.
const char* guestRangeName(std::uint32_t addr);

// The flat guest address space: 4 GiB plus a guard tail reserved up front, with pages committed on
// demand. Memory is zero-filled when it is first committed or committed again after a decommit.
class GuestMemory {
public:
    GuestMemory() = default;
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    // Reserves 4 GiB + kTox2GuestGuardSize as PAGE_NOACCESS. The newest reservation is the one the
    // fault handler recognises.
    bool reserve();
    std::uint8_t* base() const { return base_; }

    // Page-rounded (outward) read/write commit of [addr, addr + size). Committing pages that are
    // already committed keeps their contents.
    bool commit(std::uint32_t addr, std::uint32_t size);
    void decommit(std::uint32_t addr, std::uint32_t size);

    // True when every page of [addr, addr + size) is committed. False before reserve() and when the
    // range runs past the 32-bit space. Answered from a page bitmap kept by commit()/decommit();
    // VirtualQuery costs hundreds of microseconds inside a large committed block.
    bool isCommitted(std::uint32_t addr, std::uint32_t size) const;

    // True when `host` points into the reservation, including the guard tail.
    bool containsHost(const void* host) const;

#if TOX2_RSX_WRITE_WATCH
    // The pages of [addr, addr + size) written since the previous collection that covered them: by any user-mode store
    // in the process (the OS write watch the reservation is made with) or reported through markWritten. Their state is
    // reset in the same step (WRITE_WATCH_FLAG_RESET), so a write made after this call is reported by the next one.
    // `pages` receives the page-aligned guest addresses, ascending, without duplicates. False when any page of the
    // range is not committed or the OS call fails; the caller then treats the whole range as written. One calling
    // thread at a time; markWritten may run on any thread meanwhile.
    bool collectWrites(std::uint32_t addr, std::uint32_t size, std::vector<std::uint32_t>& pages);
    // Records a write the OS write watch may not report (a file read into guest memory, commit, decommit). Call after
    // the write is complete. Only the RSX local range [kRsxLocalBegin, kRsxLocalEnd), the only range anything collects,
    // is recorded; the rest of the range is ignored. Thread-safe.
    void markWritten(std::uint32_t addr, std::uint32_t size);
#endif

private:
    // Sets or clears the bitmap bits of the pages [first_page, end_page).
    void markPages(std::uint64_t first_page, std::uint64_t end_page, bool committed);

    std::uint8_t* base_ = nullptr;
    // One bit per 4 KiB page; a set bit means the page was committed by commit() and not decommitted
    // since. Guest pages are never re-protected, so this matches what VirtualQuery would report.
    std::unique_ptr<std::atomic<std::uint64_t>[]> committed_pages_;
#if TOX2_RSX_WRITE_WATCH
    // One bit per page of the RSX local range: a write reported through markWritten and not yet collected.
    std::unique_ptr<std::atomic<std::uint64_t>[]> marked_local_;
#endif
};

// First-fit allocator over a sorted map of used ranges. Callers hold the kernel lock, so there is no
// mutex of its own.
class RegionAllocator {
public:
    RegionAllocator(GuestMemory& memory, GuestRegion region);

    // Reserves a range that someone else committed (e.g. the ELF image in the main region).
    void markUsed(std::uint32_t addr, std::uint32_t size);

    // Returns 0 on failure. `size` is rounded up to the granularity and `align` is raised to it.
    // The memory is committed and zeroed; `guard` bytes on both sides stay uncommitted.
    std::uint32_t allocate(std::uint32_t size, std::uint32_t align);

    // Decommits an allocation and returns its size, or 0 when `addr` is not the start of one.
    std::uint32_t free(std::uint32_t addr);

    const GuestRegion& region() const { return region_; }

private:
    struct Block {
        std::uint32_t size;
        std::uint64_t ext_begin, ext_end;  // the range including guards
    };

    GuestMemory& memory_;
    GuestRegion region_;
    std::map<std::uint32_t, Block> blocks_;  // by start address; extended ranges never overlap
};

struct Process {
    Process();

    GuestMemory memory;
    RegionAllocator main, user1m, user64k, stack;
    std::uint32_t user_memory_total = 0xD500000, user_memory_used = 0;
    bool strict = false;
};

Process& process();

// Adds a vectored handler that turns an access violation inside the guest reservation, or a host
// stack overflow, into a fatal stop with the guest address. Everything else is passed on.
void installFaultHandler();

// Reserves extra stack for the calling host thread so the fault handler still has room to run after
// a stack overflow. Call it on every thread that runs guest code.
void reserveFaultStack();

}  // namespace tox2
