#include "core/guest_memory.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <format>

#include "core/fatal.h"
#include "core/host_memory.h"
#include "core/log.h"
#include "tox2/ppu_context.h"

namespace tox2 {
namespace {

constexpr std::uint64_t kGuestSpace = 1ull << 32;
constexpr std::uint64_t kPageSize = 0x1000;
constexpr std::size_t kReserveBytes = static_cast<std::size_t>(kGuestSpace) + kTox2GuestGuardSize;

// The reservation the fault handler recognises, set by GuestMemory::reserve().
std::atomic<std::uintptr_t> g_reserve_begin{0};
std::atomic<std::uintptr_t> g_reserve_end{0};

std::uint64_t alignUp(std::uint64_t value, std::uint64_t align) { return (value + align - 1) / align * align; }

std::uint64_t hostPage() { return host::pageSize(); }

// True when any logical page of the host page holding logical page `page` has its bit set.
bool hostPageLive(const std::atomic<std::uint64_t>* bitmap, std::uint64_t page) {
    const std::uint64_t per_host = hostPage() / kPageSize;
    const std::uint64_t first = page / per_host * per_host;
    // A host page is a power-of-two multiple of kPageSize and aligned, so its bits sit in one word.
    const std::uint64_t mask = (per_host >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << per_host) - 1))
                               << (first % 64);
    return (bitmap[first / 64].load(std::memory_order_acquire) & mask) != 0;
}

// Zero-fills the logical pages of [first_page, end_page) that are not committed yet but share a live
// host page, before their bits are set. Such a host page stayed committed because a neighbouring
// logical page is live, so its other logical pages may still hold the data of an earlier allocation.
void zeroStaleSiblings(std::uint8_t* base, const std::atomic<std::uint64_t>* bitmap, std::uint64_t first_page,
                       std::uint64_t end_page) {
    const std::uint64_t per_host = hostPage() / kPageSize;
    if (per_host == 1) {
        return;
    }
    for (std::uint64_t host_first = first_page / per_host * per_host; host_first < end_page; host_first += per_host) {
        if (!hostPageLive(bitmap, host_first)) {
            continue;
        }
        for (std::uint64_t page = std::max(host_first, first_page); page < std::min(host_first + per_host, end_page);
             page++) {
            if (((bitmap[page / 64].load(std::memory_order_relaxed) >> (page % 64)) & 1) == 0) {
                std::memset(base + page * kPageSize, 0, kPageSize);
            }
        }
    }
}

#if TOX2_RSX_WRITE_WATCH
constexpr std::uint64_t kLocalPages = (kRsxLocalEnd - kRsxLocalBegin) / kPageSize;

// The bits of one mark word that cover the local pages [page, end_page) (page numbers relative to kRsxLocalBegin).
std::uint64_t markMask(std::uint64_t page, std::uint64_t end_page, std::uint64_t& bits) {
    const std::uint64_t first_bit = page % 64;
    bits = std::min<std::uint64_t>(64 - first_bit, end_page - page);
    return (bits == 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << bits) - 1)) << first_bit;
}

// Sets the mark bits for the part of the page-aligned guest range [begin, end) that lies in RSX local memory.
void markLocalPages(std::atomic<std::uint64_t>* words, std::uint64_t begin, std::uint64_t end) {
    begin = std::max<std::uint64_t>(begin, kRsxLocalBegin);
    end = std::min<std::uint64_t>(end, kRsxLocalEnd);
    if (begin >= end) {
        return;
    }
    const std::uint64_t end_page = (end - kRsxLocalBegin) / kPageSize;
    for (std::uint64_t page = (begin - kRsxLocalBegin) / kPageSize; page < end_page;) {
        std::uint64_t bits = 0;
        const std::uint64_t mask = markMask(page, end_page, bits);
        words[page / 64].fetch_or(mask, std::memory_order_release);
        page += bits;
    }
}
#endif

void faultHandler(const host::FaultInfo& fault) {
    if (fault.stack_overflow) {
        fatal(FatalKind::GuestFault, "host stack overflow");
    }
    const std::uintptr_t begin = g_reserve_begin.load();
    if (begin == 0 || fault.address < begin || fault.address >= g_reserve_end.load()) {
        return;
    }
    const char* what = fault.access == host::FaultAccess::Write     ? "write to"
                       : fault.access == host::FaultAccess::Execute ? "execute at"
                                                                    : "read from";
    fatal(FatalKind::GuestFault,
          std::format("{} guest address {:#010x}", what, static_cast<std::uint64_t>(fault.address - begin)),
          static_cast<std::uint32_t>(fault.address - begin));
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
    host::release(base_, kReserveBytes);
}

bool GuestMemory::reserve() {
    if (base_ != nullptr) {
        return true;
    }
    const std::uint64_t host_page = hostPage();
    if (host_page < kPageSize || host_page > kPageSize * 64 || !std::has_single_bit(host_page)) {
        logError("unsupported host page size {:#x}", host_page);
        return false;
    }
#if TOX2_RSX_WRITE_WATCH
    // The watch covers the whole reservation and stays on until it is released.
    void* p = host::reserveWriteWatched(kReserveBytes);
#else
    void* p = host::reserve(kReserveBytes);
#endif
    if (p == nullptr) {
        logError("cannot reserve {:#x} bytes of guest address space ({})", kReserveBytes, host::lastErrorText());
        return false;
    }
    base_ = static_cast<std::uint8_t*>(p);
    committed_pages_ = std::make_unique<std::atomic<std::uint64_t>[]>(kGuestSpace / kPageSize / 64);  // zeroed
#if TOX2_RSX_WRITE_WATCH
    marked_local_ = std::make_unique<std::atomic<std::uint64_t>[]>(kLocalPages / 64);  // zeroed
#endif
    g_reserve_end.store(reinterpret_cast<std::uintptr_t>(base_) + kReserveBytes);
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
    const std::uint64_t host_page = hostPage();
    const std::uint64_t host_begin = begin / host_page * host_page;
    const std::uint64_t host_end = alignUp(end, host_page);
    if (!host::commit(base_ + host_begin, static_cast<std::size_t>(host_end - host_begin))) {
        return false;
    }
    zeroStaleSiblings(base_, committed_pages_.get(), begin / kPageSize, end / kPageSize);
    markPages(begin / kPageSize, end / kPageSize, true);
#if TOX2_RSX_WRITE_WATCH
    // Recommitting zero-fills the pages, which the OS write watch does not report.
    markLocalPages(marked_local_.get(), begin, end);
#endif
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

    // Only host pages with no committed logical page left are given back. Pages strictly between the
    // first and the last host page lie wholly inside the range, so only those two can be shared.
    const std::uint64_t host_page = hostPage();
    std::uint64_t host_begin = begin / host_page * host_page;
    std::uint64_t host_end = alignUp(end, host_page);
    if (host_begin < host_end && hostPageLive(committed_pages_.get(), host_begin / kPageSize)) {
        host_begin += host_page;
    }
    if (host_end > host_begin && hostPageLive(committed_pages_.get(), (host_end - host_page) / kPageSize)) {
        host_end -= host_page;
    }
    if (host_begin < host_end) {
        host::decommit(base_ + host_begin, static_cast<std::size_t>(host_end - host_begin));
    }
#if TOX2_RSX_WRITE_WATCH
    markLocalPages(marked_local_.get(), begin, end);
#endif
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
    return p >= b && p < b + kGuestSpace + kTox2GuestGuardSize;
}

#if TOX2_RSX_WRITE_WATCH
bool GuestMemory::collectWrites(std::uint32_t addr, std::uint32_t size, std::vector<std::uint32_t>& pages) {
    pages.clear();
    if (base_ == nullptr) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    const std::uint64_t begin = addr & ~(kPageSize - 1);
    const std::uint64_t end = alignUp(static_cast<std::uint64_t>(addr) + size, kPageSize);
    if (end > kGuestSpace || end - begin > 0xFFFFFFFFull ||
        !isCommitted(static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end - begin))) {
        return false;
    }

    // Room for every page of the range, so the result is never cut short (a short buffer would leave the pages past
    // it reported, but the caller could not tell which).
    thread_local std::vector<void*> scratch;
    const std::size_t capacity = static_cast<std::size_t>((end - begin) / kPageSize);
    if (scratch.size() < capacity) {
        scratch.resize(capacity);
    }
    std::size_t count = 0;
    std::size_t granularity = 0;
    if (!host::collectWrittenPages(base_ + begin, static_cast<std::size_t>(end - begin),
                                   std::span<void*>(scratch.data(), capacity), count, granularity)) {
        return false;
    }
    if (granularity != kPageSize) {
        // Windows on x64 always reports 4 KiB pages, so this is not expected to run. A port to a host with 16 KiB
        // pages (macOS on Apple silicon, recent Android) would need its own tracking, since kPageSize is the guest's
        // 4 KiB. The reset already cleared the OS state, so keep the range as marks for the next collect.
        markLocalPages(marked_local_.get(), begin, end);
        return false;
    }
    pages.reserve(count);
    for (std::size_t i = 0; i < count; i++) {
        pages.push_back(static_cast<std::uint32_t>(static_cast<std::uint8_t*>(scratch[i]) - base_));
    }
    std::sort(pages.begin(), pages.end());

    // Marks are taken after the OS reset: a mark set meanwhile is kept for the next collection or caught here, never
    // lost.
    const std::uint64_t local_begin = std::max<std::uint64_t>(begin, kRsxLocalBegin);
    const std::uint64_t local_end = std::min<std::uint64_t>(end, kRsxLocalEnd);
    if (local_begin < local_end) {
        const std::size_t os_pages = pages.size();
        const std::uint64_t first_page = (local_begin - kRsxLocalBegin) / kPageSize;
        const std::uint64_t end_page = (local_end - kRsxLocalBegin) / kPageSize;
        for (std::uint64_t page = first_page; page < end_page;) {
            std::uint64_t bits = 0;
            const std::uint64_t mask = markMask(page, end_page, bits);
            std::uint64_t taken = marked_local_[page / 64].fetch_and(~mask, std::memory_order_acq_rel) & mask;
            while (taken != 0) {
                const std::uint64_t bit = static_cast<std::uint64_t>(std::countr_zero(taken));
                taken &= taken - 1;
                pages.push_back(static_cast<std::uint32_t>(kRsxLocalBegin + ((page / 64) * 64 + bit) * kPageSize));
            }
            page += bits;
        }
        if (pages.size() != os_pages) {
            std::sort(pages.begin(), pages.end());
            pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
        }
    }
    return true;
}

void GuestMemory::markWritten(std::uint32_t addr, std::uint32_t size) {
    if (base_ == nullptr || size == 0) {
        return;
    }
    markLocalPages(marked_local_.get(), addr & ~(kPageSize - 1),
                   alignUp(static_cast<std::uint64_t>(addr) + size, kPageSize));
}
#endif

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
    host::installFaultHandler(&faultHandler);
    reserveFaultStack();
}

void reserveFaultStack() { host::reserveFaultStack(); }

}  // namespace tox2
