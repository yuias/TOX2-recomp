#include "rsx/rsx_state.h"

#include <atomic>

#include "core/guest_memory.h"

namespace tox2 {
namespace {

RsxState g_state;
std::condition_variable_any g_get_advanced;

constexpr std::uint32_t kPageShift = 20;
constexpr std::uint32_t kPageMask = 0xFFFFF;
// First EA MiB that is not ordinary memory (the local memory starts here).
constexpr std::uint32_t kEaPageLimit = kRsxLocalBase >> kPageShift;

}  // namespace

RsxState& rsxState() { return g_state; }

void resetRsxState() {
    if (g_state.initialized) {
        process().memory.decommit(kRsxDeviceAddr, kRsxContextCommitSize);
        process().memory.decommit(kRsxLocalBase, g_state.local_size);
    }
    g_state = RsxState{};
}

std::condition_variable_any& rsxGetAdvanced() { return g_get_advanced; }

bool rsxIoToEa(std::uint32_t io, std::uint32_t& ea) {
    if (io >= kRsxIoSpaceSize) return false;
    // The RSX thread copies draw data without the guest lock, so this table is read and written atomically.
    const std::uint32_t page = std::atomic_ref(g_state.ea_of_io[io >> kPageShift]).load(std::memory_order_relaxed);
    if (page >= kEaPageLimit) return false;
    ea = (page << kPageShift) | (io & kPageMask);
    return true;
}

bool rsxEaToOffset(std::uint32_t ea, std::uint32_t& offset) {
    if (const std::uint32_t local = ea - kRsxLocalBase; local < g_state.local_size) {
        offset = local;
        return true;
    }
    // RPCS3 indexes its table with any address; the host table covers the EAs below local memory.
    if ((ea >> kPageShift) >= kEaPageLimit) return false;
    const std::uint32_t io_page = g_state.io_of_ea[ea >> kPageShift];
    const std::uint32_t io_base = io_page << kPageShift;  // wraps to >= the IO space when unmapped
    if (io_base >= kRsxIoSpaceSize) return false;
    offset = io_base | (ea & kPageMask);
    return true;
}

std::uint32_t rsxMapEaIo(std::uint32_t ea, std::uint32_t io, std::uint32_t size) {
    if (size == 0 || ((ea | io | size) & kPageMask) != 0) return kGcmErrorFailure;

    // 64-bit sums: the ranges may touch the end of the 32-bit space.
    const std::uint64_t ea_end = std::uint64_t{ea} + size;
    const std::uint64_t io_end = std::uint64_t{io} + size;
    if (ea_end > kRsxLocalBase || io_end > kRsxIoSpaceSize) return kGcmErrorIoMapInvalid;
    // Every mapped page must be readable so the consumer cannot fault on it.
    if (!process().memory.isCommitted(ea, size)) return kGcmErrorIoMapInvalid;

    const std::uint32_t ea_page = ea >> kPageShift, io_page = io >> kPageShift;
    const std::uint32_t pages = size >> kPageShift;
    for (std::uint32_t i = 0; i < pages; i++) {
        g_state.io_of_ea[ea_page + i] = static_cast<std::uint16_t>(io_page + i);
        std::atomic_ref(g_state.ea_of_io[io_page + i])
                .store(static_cast<std::uint16_t>(ea_page + i), std::memory_order_relaxed);
    }
    g_state.io_map_size[ea_page] = static_cast<std::uint16_t>(pages);
    return 0;
}

std::uint32_t rsxMapMainMemory(std::uint32_t ea, std::uint32_t size, std::uint32_t& io_offset) {
    if (size == 0 || ((ea | size) & kPageMask) != 0) return kGcmErrorFailure;

    // First run of `pages` free IO pages (free = the entry is not a valid EA page).
    const std::uint32_t pages = size >> kPageShift;
    const std::uint32_t total = kRsxIoSpaceSize >> kPageShift;
    std::uint32_t run = 0;
    for (std::uint32_t io = 0; io < total; io++) {
        run = g_state.ea_of_io[io] >= kEaPageLimit ? run + 1 : 0;
        if (run < pages) continue;

        const std::uint32_t start = (io + 1 - pages) << kPageShift;
        if (const std::uint32_t error = rsxMapEaIo(ea, start, size); error != 0) return error;
        io_offset = start;
        return 0;
    }
    return kGcmErrorNoIoPageTable;
}

std::uint32_t rsxUnmapEa(std::uint32_t ea) {
    const std::uint32_t ea_page = ea >> kPageShift;
    if (ea_page >= kEaPageLimit) return kGcmErrorFailure;

    const std::uint32_t io_page = g_state.io_of_ea[ea_page];
    if ((io_page << kPageShift) >= kRsxIoSpaceSize) return kGcmErrorFailure;

    // The mapping is recorded under the EA page that the IO page currently points back to.
    const std::uint32_t start_page = g_state.ea_of_io[io_page];
    if (start_page >= kEaPageLimit) return kGcmErrorFailure;
    const std::uint32_t pages = g_state.io_map_size[start_page];
    if (pages == 0 || io_page + pages > kRsxIoPages) return kGcmErrorFailure;

    for (std::uint32_t i = 0; i < pages; i++) {
        g_state.io_of_ea[start_page + i] = kRsxUnmapped;
        std::atomic_ref(g_state.ea_of_io[io_page + i]).store(kRsxUnmapped, std::memory_order_relaxed);
    }
    g_state.io_map_size[start_page] = 0;
    return 0;
}

}  // namespace tox2
