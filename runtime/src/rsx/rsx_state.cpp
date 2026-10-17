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

bool rsxInitialized() { return std::atomic_ref(g_state.initialized).load(std::memory_order_relaxed); }

std::uint32_t rsxLocalSize() { return std::atomic_ref(g_state.local_size).load(std::memory_order_relaxed); }

void clearRsxState() {
    // Not `g_state = RsxState{}`: the whole-struct store would be a plain write to the fields that kernel-free handlers
    // and the RSX thread read atomically without the kernel lock.
    const RsxState fresh;
    RsxState& s = g_state;
    constexpr auto kRelaxed = std::memory_order_relaxed;
    std::atomic_ref(s.initialized).store(fresh.initialized, kRelaxed);
    std::atomic_ref(s.local_size).store(fresh.local_size, kRelaxed);
    for (std::size_t i = 0; i < s.io_of_ea.size(); i++)
        std::atomic_ref(s.io_of_ea[i]).store(fresh.io_of_ea[i], kRelaxed);
    for (std::size_t i = 0; i < s.ea_of_io.size(); i++)
        std::atomic_ref(s.ea_of_io[i]).store(fresh.ea_of_io[i], kRelaxed);
    std::atomic_ref(s.requested_vsync).store(fresh.requested_vsync, kRelaxed);
    std::atomic_ref(s.flip_status).store(fresh.flip_status, kRelaxed);

    s.io_address = fresh.io_address;
    s.io_size = fresh.io_size;
    s.context_pp = fresh.context_pp;
    s.io_map_size = fresh.io_map_size;
    s.command_begin = fresh.command_begin;
    s.fragment_count = fresh.fragment_count;
    s.command_size = fresh.command_size;
    s.segment_size = fresh.segment_size;
    s.wrap_callback = fresh.wrap_callback;
    s.display = fresh.display;
    s.display_count = fresh.display_count;
    s.tiles = fresh.tiles;
    s.zculls = fresh.zculls;
    s.last_flip_time = fresh.last_flip_time;
    s.vblank_count = fresh.vblank_count;
    s.flip_handler = fresh.flip_handler;
    s.vblank_handler = fresh.vblank_handler;
    s.user_handler = fresh.user_handler;
}

void resetRsxState() {
    if (rsxInitialized()) {
        process().memory.decommit(kRsxDeviceAddr, kRsxContextCommitSize);
        process().memory.decommit(kRsxLocalBase, rsxLocalSize());
    }
    clearRsxState();
}

std::condition_variable_any& rsxGetAdvanced() { return g_get_advanced; }

bool rsxIoToEa(std::uint32_t io, std::uint32_t& ea) {
    if (io >= kRsxIoSpaceSize) return false;
    // The RSX thread copies draw data without the kernel lock, so this table is read and written atomically.
    const std::uint32_t page = std::atomic_ref(g_state.ea_of_io[io >> kPageShift]).load(std::memory_order_relaxed);
    if (page >= kEaPageLimit) return false;
    ea = (page << kPageShift) | (io & kPageMask);
    return true;
}

bool rsxIoToEaSpan(std::uint32_t io, std::uint32_t& ea, std::uint32_t& bytes) {
    if (!rsxIoToEa(io, ea)) return false;
    bytes = (kPageMask + 1) - (io & kPageMask);
    return true;
}

bool rsxEaToOffset(std::uint32_t ea, std::uint32_t& offset) {
    // cellGcmAddressToOffset calls this without the kernel lock.
    if (const std::uint32_t local = ea - kRsxLocalBase; local < rsxLocalSize()) {
        offset = local;
        return true;
    }
    // RPCS3 indexes its table with any address; the host table covers the EAs below local memory.
    if ((ea >> kPageShift) >= kEaPageLimit) return false;
    const std::uint32_t io_page = std::atomic_ref(g_state.io_of_ea[ea >> kPageShift]).load(std::memory_order_relaxed);
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
        std::atomic_ref(g_state.io_of_ea[ea_page + i])
                .store(static_cast<std::uint16_t>(io_page + i), std::memory_order_relaxed);
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
        run = std::atomic_ref(g_state.ea_of_io[io]).load(std::memory_order_relaxed) >= kEaPageLimit ? run + 1 : 0;
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

    const std::uint32_t io_page = std::atomic_ref(g_state.io_of_ea[ea_page]).load(std::memory_order_relaxed);
    if ((io_page << kPageShift) >= kRsxIoSpaceSize) return kGcmErrorFailure;

    // The mapping is recorded under the EA page that the IO page currently points back to.
    const std::uint32_t start_page = std::atomic_ref(g_state.ea_of_io[io_page]).load(std::memory_order_relaxed);
    if (start_page >= kEaPageLimit) return kGcmErrorFailure;
    const std::uint32_t pages = g_state.io_map_size[start_page];
    if (pages == 0 || io_page + pages > kRsxIoPages) return kGcmErrorFailure;

    for (std::uint32_t i = 0; i < pages; i++) {
        std::atomic_ref(g_state.io_of_ea[start_page + i]).store(kRsxUnmapped, std::memory_order_relaxed);
        std::atomic_ref(g_state.ea_of_io[io_page + i]).store(kRsxUnmapped, std::memory_order_relaxed);
    }
    g_state.io_map_size[start_page] = 0;
    return 0;
}

}  // namespace tox2
