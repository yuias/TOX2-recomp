#include "core/guest_memory.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <format>

#include "core/fatal.h"
#include "core/log.h"
#include "dal/ppu_context.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace dal {
namespace {

constexpr std::uint64_t kGuestSpace = 1ull << 32;
constexpr std::uint64_t kPageSize = 0x1000;
constexpr std::uint64_t kFaultStackBytes = 0x20000;

// The reservation the fault handler recognises, set by GuestMemory::reserve().
std::atomic<std::uintptr_t> g_reserve_begin{0};
std::atomic<std::uintptr_t> g_reserve_end{0};

std::uint64_t alignUp(std::uint64_t value, std::uint64_t align) { return (value + align - 1) / align * align; }

LONG CALLBACK faultHandler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD& rec = *info->ExceptionRecord;
    if (rec.ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        fatal(FatalKind::GuestFault, "host stack overflow");
    }
    if (rec.ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec.NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const std::uintptr_t address = rec.ExceptionInformation[1];
    const std::uintptr_t begin = g_reserve_begin.load();
    if (begin == 0 || address < begin || address >= g_reserve_end.load()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const ULONG_PTR access = rec.ExceptionInformation[0];
    const char* what = access == 1 ? "write to" : access == 8 ? "execute at" : "read from";
    fatal(FatalKind::GuestFault,
          std::format("{} guest address {:#010x}", what, static_cast<std::uint64_t>(address - begin)));
}

}  // namespace

GuestMemory::~GuestMemory() {
    if (base_ == nullptr) {
        return;
    }
    std::uintptr_t expected = reinterpret_cast<std::uintptr_t>(base_);
    if (g_reserve_begin.compare_exchange_strong(expected, 0)) {
        g_reserve_end.store(0);
    }
    VirtualFree(base_, 0, MEM_RELEASE);
}

bool GuestMemory::reserve() {
    if (base_ != nullptr) {
        return true;
    }
    const std::size_t total = static_cast<std::size_t>(kGuestSpace) + kDalGuestGuardSize;
    void* p = VirtualAlloc(nullptr, total, MEM_RESERVE, PAGE_NOACCESS);
    if (p == nullptr) {
        logError("cannot reserve {:#x} bytes of guest address space (error {})", total, GetLastError());
        return false;
    }
    base_ = static_cast<std::uint8_t*>(p);
    committed_pages_ = std::make_unique<std::atomic<std::uint64_t>[]>(kGuestSpace / kPageSize / 64);  // zeroed
    g_reserve_end.store(reinterpret_cast<std::uintptr_t>(base_) + total);
    g_reserve_begin.store(reinterpret_cast<std::uintptr_t>(base_));
    return true;
}

bool GuestMemory::commit(std::uint32_t addr, std::uint32_t size) {
    if (base_ == nullptr) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    const std::uint64_t begin = addr & ~(kPageSize - 1);
    const std::uint64_t end = alignUp(static_cast<std::uint64_t>(addr) + size, kPageSize);
    if (end > kGuestSpace) {
        return false;
    }
    if (VirtualAlloc(base_ + begin, static_cast<std::size_t>(end - begin), MEM_COMMIT, PAGE_READWRITE) == nullptr) {
        return false;
    }
    markPages(begin / kPageSize, end / kPageSize, true);
    return true;
}

void GuestMemory::decommit(std::uint32_t addr, std::uint32_t size) {
    if (base_ == nullptr || size == 0) {
        return;
    }
    const std::uint64_t begin = addr & ~(kPageSize - 1);
    const std::uint64_t end = std::min(alignUp(static_cast<std::uint64_t>(addr) + size, kPageSize), kGuestSpace);
    // Cleared first, so a concurrent isCommitted never reports a page that is already being released.
    markPages(begin / kPageSize, end / kPageSize, false);
    VirtualFree(base_ + begin, static_cast<std::size_t>(end - begin), MEM_DECOMMIT);
}

void GuestMemory::markPages(std::uint64_t first_page, std::uint64_t end_page, bool committed) {
    for (std::uint64_t page = first_page; page < end_page;) {
        const std::uint64_t word = page / 64;
        const std::uint64_t first_bit = page % 64;
        const std::uint64_t bits = std::min<std::uint64_t>(64 - first_bit, end_page - page);
        const std::uint64_t mask = (bits == 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << bits) - 1)) << first_bit;
        if (committed) {
            committed_pages_[word].fetch_or(mask, std::memory_order_release);
        } else {
            committed_pages_[word].fetch_and(~mask, std::memory_order_release);
        }
        page += bits;
    }
}

bool GuestMemory::isCommitted(std::uint32_t addr, std::uint32_t size) const {
    if (base_ == nullptr) {
        return false;
    }
    const std::uint64_t end = static_cast<std::uint64_t>(addr) + std::max<std::uint32_t>(size, 1);
    if (end > kGuestSpace) {
        return false;
    }
    const std::uint64_t end_page = alignUp(end, kPageSize) / kPageSize;
    for (std::uint64_t page = addr / kPageSize; page < end_page;) {
        const std::uint64_t first_bit = page % 64;
        const std::uint64_t bits = std::min<std::uint64_t>(64 - first_bit, end_page - page);
        const std::uint64_t mask = (bits == 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << bits) - 1)) << first_bit;
        if ((committed_pages_[page / 64].load(std::memory_order_acquire) & mask) != mask) {
            return false;
        }
        page += bits;
    }
    return true;
}

bool GuestMemory::containsHost(const void* host) const {
    if (base_ == nullptr) {
        return false;
    }
    const auto p = reinterpret_cast<std::uintptr_t>(host);
    const auto b = reinterpret_cast<std::uintptr_t>(base_);
    return p >= b && p < b + kGuestSpace + kDalGuestGuardSize;
}

const char* guestRangeName(std::uint32_t addr) {
    for (const GuestRegion* region : {&kMainRegion, &kUser1mRegion, &kUser64kRegion, &kStackRegion}) {
        if (addr >= region->begin && addr < region->end) {
            return region->name;
        }
    }
    if (addr >= kRsxContextBegin && addr < kRsxContextEnd) {
        return "rsx-context";
    }
    if (addr >= kRsxLocalBegin && addr < kRsxLocalEnd) {
        return "rsx-local";
    }
    return nullptr;
}

RegionAllocator::RegionAllocator(GuestMemory& memory, GuestRegion region) : memory_(memory), region_(region) {}

void RegionAllocator::markUsed(std::uint32_t addr, std::uint32_t size) {
    if (size == 0) {
        return;
    }
    const std::uint64_t end = static_cast<std::uint64_t>(addr) + size;
    if (addr < region_.begin || end > region_.end) {
        logError("markUsed {:#010x}+{:#x} is outside region {}", addr, size, region_.name);
        return;
    }
    for (const auto& [start, block] : blocks_) {
        if (addr < block.ext_end && end > block.ext_begin) {
            logError("markUsed {:#010x}+{:#x} overlaps {:#010x} in region {}", addr, size, start, region_.name);
            return;
        }
    }
    blocks_.emplace(addr, Block{size, addr, end});
}

std::uint32_t RegionAllocator::allocate(std::uint32_t size, std::uint32_t align) {
    if (size == 0 || (align != 0 && !std::has_single_bit(align))) {
        return 0;
    }
    const std::uint64_t granularity = region_.granularity;
    const std::uint64_t guard = region_.guard;
    const std::uint64_t alignment = std::max<std::uint64_t>(align, granularity);
    const std::uint64_t rounded = alignUp(size, granularity);

    // Lowest gap, scanning upward, that fits the guards, the alignment and the size.
    auto fit = [&](std::uint64_t gap_begin, std::uint64_t gap_end) -> std::uint64_t {
        const std::uint64_t start = alignUp(gap_begin + guard, alignment);
        return start + rounded + guard <= gap_end ? start : 0;
    };

    std::uint64_t found = 0;
    std::uint64_t cursor = region_.begin;
    for (const auto& [start, block] : blocks_) {
        found = fit(cursor, block.ext_begin);
        if (found != 0) {
            break;
        }
        cursor = std::max(cursor, block.ext_end);
    }
    if (found == 0) {
        found = fit(cursor, region_.end);
    }
    if (found == 0) {
        return 0;
    }

    const auto addr = static_cast<std::uint32_t>(found);
    const auto bytes = static_cast<std::uint32_t>(rounded);
    if (!memory_.commit(addr, bytes)) {
        return 0;
    }
    blocks_.emplace(addr, Block{bytes, found - guard, found + rounded + guard});
    return addr;
}

std::uint32_t RegionAllocator::free(std::uint32_t addr) {
    const auto it = blocks_.find(addr);
    if (it == blocks_.end()) {
        return 0;
    }
    const std::uint32_t size = it->second.size;
    blocks_.erase(it);
    memory_.decommit(addr, size);
    return size;
}

Process::Process()
    : main(memory, kMainRegion),
      user1m(memory, kUser1mRegion),
      user64k(memory, kUser64kRegion),
      stack(memory, kStackRegion) {}

Process& process() {
    // Leaked on purpose: guest threads may still touch it while the process exits.
    static Process* p = new Process;
    return *p;
}

void installFaultHandler() {
    static std::atomic<bool> installed{false};
    if (!installed.exchange(true)) {
        AddVectoredExceptionHandler(1, &faultHandler);
    }
    reserveFaultStack();
}

void reserveFaultStack() {
    ULONG bytes = static_cast<ULONG>(kFaultStackBytes);
    SetThreadStackGuarantee(&bytes);
}

}  // namespace dal
