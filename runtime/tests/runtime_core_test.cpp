// Self-checking unit tests for the runtime core library. No generated code is involved.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <csetjmp>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/boot.h"
#include "core/census.h"
#include "core/dispatch.h"
#include "core/elf_loader.h"
#include "core/event_history.h"
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/host_functions.h"
#include "core/host_priority.h"
#include "core/latency_stats.h"
#include "core/log.h"
#include "core/param_sfo.h"
#include "core/perf_log.h"
#include "core/reservation.h"
#include "core/thread_cpu.h"
#include "core/threads.h"
#include "core/timer_resolution.h"
#include "tox2/ppu_context.h"
#include "hle/guest_printf.h"
#include "hle/hle.h"
#include "hle/libsre.h"
#include "hle/save_dialog.h"
#include "hle/save_dialog_text.h"
#include "hle/sail_service.h"
#include "input/keyboard_pad.h"
#include "input/pad_buttons.h"
#include "input/pad_script.h"
#include "png.h"
#include "lv2/cell_error.h"
#include "lv2/lwmutex_word.h"
#include "lv2/sys_memory.h"
#include "lv2/sys_ppu_thread.h"
#include "lv2/sys_fs.h"
#include "lv2/sys_spu.h"
#include "lv2/syscalls.h"
#include "lv2/vfs.h"
#include "lv2/vfs_config.h"
#include "lv2/sys_sync.h"
#include "rsx/fifo_parser.h"
#include "rsx/gcm_interrupts.h"
#include "rsx/rsx_capture.h"
#include "rsx/rsx_census.h"
#include "rsx/rsx_decode.h"
#include "rsx/rsx_frame.h"
#include "rsx/rsx_frame_builder.h"
#include "rsx/rsx_hash_stream.h"
#include "rsx/rsx_registers.h"
#include "rsx/rsx_state.h"
#include "rsx/rsx_texture_layout.h"
#include "rsx/rsx_thread.h"
#include "rsx/rsx_unimplemented.h"
#include "rsx_synthetic.h"
#include "zlib.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

int g_passed;
int g_failed;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            g_passed++;                                                            \
        } else {                                                                   \
            g_failed++;                                                            \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
        }                                                                          \
    } while (0)

std::optional<std::uint32_t> g_thread_id;

std::optional<std::uint32_t> fakeThreadId() { return g_thread_id; }

void testLogLevelsAndPrefix() {
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::setGuestThreadIdProvider(&fakeThreadId);

    g_thread_id = std::nullopt;
    tox2::logInfo("value {}", 7);
    g_thread_id = 0x01000000u;
    tox2::logWarn("w {:#x}", 255u);
    tox2::logError("e");

    tox2::setLogSink({});
    tox2::setGuestThreadIdProvider(nullptr);

    CHECK(lines.size() == 3);
    if (lines.size() == 3) {
        CHECK(lines[0] == "[host] value 7");
        CHECK(lines[1] == "[t 01000000] warn: w 0xff");
        CHECK(lines[2] == "[t 01000000] error: e");
    }
}

void testEventHistoryRing() {
    using tox2::HistoryKind;
    tox2::setGuestThreadIdProvider(&fakeThreadId);
    g_thread_id = 0x0100000bu;
    tox2::resetHistory();

    CHECK(tox2::historyLines(std::nullopt).empty());
    {
        const auto lines = tox2::historyLines(0x1000);
        CHECK(lines.size() == 1 && lines[0] == "history: no recorded free covers 0x1000");
    }

    // 700 entries overflow the 512-entry ring. The free of 0x40000 is the first entry, so it is evicted; the
    // first free of 0x30000 is still in the ring but older than the second one.
    tox2::recordHistory(HistoryKind::MemoryFree, 0x40000, 0x100, 0x444, 1, 2);
    for (std::uint32_t i = 0; i < 696; i++) {
        if (i == 300) tox2::recordHistory(HistoryKind::MemoryFree, 0x30000, 0x100, 0x111, 1, 2);
        switch (i % 5) {
            case 0: tox2::recordHistory(HistoryKind::SpuSend, 0x77, i, 0x55); break;
            case 1: tox2::recordHistory(HistoryKind::EventReceive, 0x77, i, 0x55); break;
            case 2: tox2::recordHistory(HistoryKind::JoinEnter, i, 0, 0); break;
            case 3: tox2::recordHistory(HistoryKind::JoinReturn, i, 0, 0); break;
            default: tox2::recordHistory(HistoryKind::MutexDestroy, i, 0, 0); break;
        }
    }
    tox2::recordHistory(HistoryKind::MemoryFree, 0x30000, 0x100, 0x222, 0x3333, 0x4444);
    tox2::recordHistory(HistoryKind::MemoryFree, 0x50000, 0x100, 0x333);

    const auto plain = tox2::historyLines(std::nullopt);
    CHECK(plain.size() == 512);
    // The last two are the newest frees; the evicted first free must not be listed.
    CHECK(plain.size() == 512 && plain[0].contains("t 0100000b "));
    bool evicted = true;
    for (const std::string& line : plain) evicted = evicted && !line.contains("addr 0x40000");
    CHECK(evicted);
    CHECK(plain.size() == 512 && plain[510].contains("mem-free addr 0x30000 size 0x100 lr 0x222"));
    CHECK(plain.size() == 512 && plain[510].contains("caller lrs 0x3333 0x4444"));
    CHECK(plain.size() == 512 && plain[511].contains("mem-free addr 0x50000"));

    // Timestamps never decrease: "+<ms>.<us>" is the second token.
    bool ordered = true;
    double previous = 0;
    for (const std::string& line : plain) {
        const double ms = std::stod(line.substr(line.find('+') + 1));
        ordered = ordered && ms >= previous;
        previous = ms;
    }
    CHECK(ordered);

    // The sequence of "spu-send" codes is increasing, so the ring was read oldest to newest.
    std::uint64_t last_code = 0;
    bool increasing = true;
    for (const std::string& line : plain) {
        if (!line.contains("spu-send")) continue;
        const std::size_t at = line.find("code 0x");
        const std::uint64_t code = std::stoull(line.substr(at + 7), nullptr, 16);
        increasing = increasing && code >= last_code;
        last_code = code;
    }
    CHECK(increasing && last_code != 0);

    const auto covered = tox2::historyLines(0x30080);
    CHECK(covered.size() == 513);
    CHECK(covered.size() == 513 && covered.back().contains("lr 0x222") && !covered.back().contains("lr 0x111"));
    const auto uncovered = tox2::historyLines(0x40000);
    CHECK(uncovered.size() == 513 && uncovered.back() == "history: no recorded free covers 0x40000");

    tox2::resetHistory();
    CHECK(tox2::historyLines(std::nullopt).empty());
    tox2::setGuestThreadIdProvider(nullptr);
    g_thread_id = std::nullopt;
}

void testLogVisibleLevel() {
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::setGuestThreadIdProvider(nullptr);
    tox2::resetLogOnce();

    CHECK(!tox2::logEnabled(tox2::LogLevel::Debug));
    CHECK(tox2::logEnabled(tox2::LogLevel::Info));
    tox2::logTrace("t0");
    tox2::logDebug("d0");
    tox2::logInfo("i0");
    const std::size_t default_count = lines.size();

    // A suppressed logOnce does not use up its key.
    tox2::logOnce(tox2::LogLevel::Debug, "level-key", "once");

    tox2::setLogLevel(tox2::LogLevel::Debug);
    tox2::logTrace("t1");
    tox2::logDebug("d1 {}", 5);
    tox2::logOnce(tox2::LogLevel::Debug, "level-key", "once");

    tox2::setLogLevel(tox2::LogLevel::Info);
    tox2::setLogSink({});

    CHECK(default_count == 1);
    CHECK(lines.size() == 3);
    if (lines.size() == 3) {
        CHECK(lines[0] == "[host] i0");
        CHECK(lines[1] == "[host] debug: d1 5");
        CHECK(lines[2] == "[host] debug: once");
    }
    CHECK(!tox2::logEnabled(tox2::LogLevel::Debug));
}

void testLogOnce() {
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetLogOnce();

    for (int i = 0; i < 3; i++) {
        tox2::logOnce("key-a", "a {}", i);
    }
    tox2::logOnce("key-b", "b");
    tox2::logOnce(tox2::LogLevel::Warn, "key-b", "b again");
    tox2::logOnce(tox2::LogLevel::Warn, "key-c", "c");

    tox2::resetLogOnce();
    tox2::logOnce("key-a", "a after reset");

    tox2::setLogSink({});

    CHECK(lines.size() == 4);
    if (lines.size() == 4) {
        CHECK(lines[0] == "[host] a 0");
        CHECK(lines[1] == "[host] b");
        CHECK(lines[2] == "[host] warn: c");
        CHECK(lines[3] == "[host] a after reset");
    }
}

constexpr std::uint64_t kGuestSpace = 1ull << 32;

tox2::GuestMemory* newMemory() {
    auto* memory = new tox2::GuestMemory;
    if (!memory->reserve()) {
        std::fprintf(stderr, "cannot reserve guest memory\n");
        std::exit(1);
    }
    return memory;
}

DWORD queryState(const tox2::GuestMemory& memory, std::uint64_t guest_addr) {
    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(memory.base() + guest_addr, &info, sizeof info);
    return info.State;
}

void testGuestMemoryRoundTrip() {
    std::unique_ptr<tox2::GuestMemory> memory(newMemory());
    std::uint8_t* base = memory->base();

    CHECK(base != nullptr);
    CHECK(queryState(*memory, 0x10000) == MEM_RESERVE);
    CHECK(memory->commit(0x10001, 0x10));  // rounds out to the whole page
    CHECK(queryState(*memory, 0x10000) == MEM_COMMIT);
    CHECK(queryState(*memory, 0x11000) == MEM_RESERVE);
    CHECK(tox2Read32(base, 0x10100) == 0);  // zero-filled

    tox2Write32(base, 0x10100, 0x11223344u);
    CHECK(base[0x10100] == 0x11 && base[0x10101] == 0x22 && base[0x10102] == 0x33 && base[0x10103] == 0x44);
    CHECK(tox2Read32(base, 0x10100) == 0x11223344u);
    CHECK(tox2Read16(base, 0x10101) == 0x2233);

    // Commit keeps contents, decommit discards them.
    CHECK(memory->commit(0x10000, 0x1000));
    CHECK(tox2Read32(base, 0x10100) == 0x11223344u);
    memory->decommit(0x10000, 0x1000);
    CHECK(queryState(*memory, 0x10000) == MEM_RESERVE);
    CHECK(memory->commit(0x10000, 0x1000));
    CHECK(tox2Read32(base, 0x10100) == 0);

    CHECK(memory->containsHost(base));
    CHECK(memory->containsHost(base + kGuestSpace + kTox2GuestGuardSize - 1));
    CHECK(!memory->containsHost(base + kGuestSpace + kTox2GuestGuardSize));
    CHECK(!memory->containsHost(base - 1));
}

void testIsCommittedAndRangeNames() {
    std::unique_ptr<tox2::GuestMemory> memory(newMemory());
    CHECK(!memory->isCommitted(0x10000, 4));
    CHECK(memory->commit(0x10000, 0x2000));
    CHECK(memory->isCommitted(0x10000, 4));
    CHECK(memory->isCommitted(0x11ffc, 4));
    CHECK(memory->isCommitted(0x10ff0, 0x20));  // spans two pages
    CHECK(!memory->isCommitted(0x11ffe, 4));    // runs into the uncommitted page after
    CHECK(!memory->isCommitted(0x12000, 4));
    CHECK(!memory->isCommitted(0xFFFFFFFEu, 4));  // past the 32-bit space
    tox2::GuestMemory unreserved;
    CHECK(!unreserved.isCommitted(0x10000, 4));

    // The page bitmap holds 64 pages per word: ranges across word boundaries, partial decommit, top page.
    CHECK(memory->commit(0x100000, 0x200000));  // 512 pages, whole words and partial ones
    CHECK(memory->isCommitted(0x100000, 0x200000));
    CHECK(!memory->isCommitted(0x100000, 0x200001));
    CHECK(!memory->isCommitted(0xFFFFF, 2));
    memory->decommit(0x13F000, 0x2000);  // pages 0x13F and 0x140 straddle a word boundary
    CHECK(memory->isCommitted(0x100000, 0x3F000));
    CHECK(!memory->isCommitted(0x13F000, 4));
    CHECK(!memory->isCommitted(0x140FFC, 4));
    CHECK(!memory->isCommitted(0x13EFFC, 8));
    CHECK(memory->isCommitted(0x141000, 0x1BF000));
    CHECK(memory->commit(0xFFFFF000u, 0x1000));
    CHECK(memory->isCommitted(0xFFFFFFFCu, 4));
    CHECK(!memory->isCommitted(0xFFFFFFFCu, 5));

    CHECK(std::string_view(tox2::guestRangeName(0x10000)) == "main");
    CHECK(std::string_view(tox2::guestRangeName(0x0FFFFFFF)) == "main");
    CHECK(std::string_view(tox2::guestRangeName(0x10000000)) == "rsx-context");
    CHECK(std::string_view(tox2::guestRangeName(0x20000000)) == "user1m");
    CHECK(std::string_view(tox2::guestRangeName(0x30000000)) == "user64k");
    CHECK(std::string_view(tox2::guestRangeName(0xC0000000)) == "rsx-local");
    CHECK(std::string_view(tox2::guestRangeName(0xD0000000)) == "stack");
    CHECK(tox2::guestRangeName(0) == nullptr);
    CHECK(tox2::guestRangeName(0x40000000) == nullptr);
    CHECK(tox2::guestRangeName(0xE0000000) == nullptr);
}

// The RSX texture validation asks which pages of a range were written since its last query.
void testGuestWriteCollection() {
    tox2::GuestMemory& memory = tox2::process().memory;
    CHECK(memory.reserve());
    constexpr std::uint32_t kPage = 0x1000;
    constexpr std::uint32_t kTestBase = tox2::kRsxLocalBegin + 0x08000000;
    CHECK(memory.commit(kTestBase, 16 * kPage));
#if TOX2_RSX_WRITE_WATCH
    std::uint8_t* base = memory.base();
    const auto page = [&](std::uint32_t n) { return kTestBase + n * kPage; };
    using Pages = std::vector<std::uint32_t>;
    std::vector<std::uint32_t> got;
    const auto collect = [&](std::uint32_t first, std::uint32_t count) {
        const bool ok = memory.collectWrites(page(first), count * kPage, got);
        return ok ? got : Pages{0xFFFFFFFFu};
    };

    // Committing the pages marked them; the first collection drains that.
    CHECK(collect(0, 16) == Pages({page(0), page(1), page(2), page(3), page(4), page(5), page(6), page(7), page(8),
                                   page(9), page(10), page(11), page(12), page(13), page(14), page(15)}));
    CHECK(collect(0, 16).empty());

    tox2Write32(base, page(3) + 0x40, 0x11223344u);
    CHECK(collect(0, 16) == Pages({page(3)}));
    CHECK(collect(0, 16).empty());

    // A collection resets only the pages it covers.
    tox2Write32(base, page(5), 1);
    tox2Write32(base, page(12) + 0xFFC, 2);
    CHECK(collect(0, 8) == Pages({page(5)}));
    CHECK(collect(8, 8) == Pages({page(12)}));
    CHECK(collect(0, 16).empty());

    // A range that is not page-aligned is rounded out.
    tox2Write32(base, page(2), 3);
    CHECK(memory.collectWrites(page(1) + 0x800, kPage, got) && got == Pages({page(2)}));

    // markWritten stands in for writers the OS does not report, and is consumed by the first collection covering it.
    memory.markWritten(page(7) + 0x123, 1);
    CHECK(collect(8, 8).empty());
    CHECK(collect(0, 16) == Pages({page(7)}));
    CHECK(collect(0, 16).empty());
    memory.markWritten(page(14) + 0xFFF, 2);  // straddles pages 14 and 15
    CHECK(collect(0, 16) == Pages({page(14), page(15)}));

    // Outside the RSX local range nothing is recorded.
    constexpr std::uint32_t kMainTest = 0x0F000000;
    CHECK(memory.commit(kMainTest, kPage));
    CHECK(memory.collectWrites(kMainTest, kPage, got));
    memory.markWritten(kMainTest, kPage);
    CHECK(memory.collectWrites(kMainTest, kPage, got) && got.empty());
    memory.decommit(kMainTest, kPage);

    // The OS does not report decommit and recommit; they are marked. An uncommitted range is refused.
    memory.decommit(page(9), kPage);
    CHECK(!memory.collectWrites(page(0), 16 * kPage, got) && got.empty());
    CHECK(!memory.collectWrites(page(9) + 8, 4, got));
    CHECK(memory.collectWrites(page(0), 8 * kPage, got) && got.empty());
    CHECK(memory.commit(page(9), kPage));
    CHECK(collect(0, 16) == Pages({page(9)}));
    CHECK(!memory.collectWrites(page(10), 0xFFFFFFFFu - page(10) + 2, got));  // runs past the 32-bit space
    CHECK(memory.collectWrites(page(10), 0, got) && got.empty());

    // Marks from another thread are never lost while the main thread collects.
    constexpr std::uint32_t kThreadPages = 1024;
    const std::uint32_t thread_base = kTestBase + 0x00100000;
    CHECK(memory.commit(thread_base, kThreadPages * kPage));
    CHECK(memory.collectWrites(thread_base, kThreadPages * kPage, got));  // drains the commit marks
    std::atomic<bool> done{false};
    std::thread marker([&] {
        for (std::uint32_t i = 0; i < 1000; i++) {
            memory.markWritten(thread_base + i * kPage + (i % 7), 1);
        }
        done.store(true, std::memory_order_release);
    });
    std::vector<bool> seen(kThreadPages, false);
    const auto absorb = [&] {
        CHECK(memory.collectWrites(thread_base, kThreadPages * kPage, got));
        for (const std::uint32_t p : got) seen[(p - thread_base) / kPage] = true;
    };
    while (!done.load(std::memory_order_acquire)) absorb();
    marker.join();
    absorb();
    CHECK(std::count(seen.begin(), seen.end(), true) == 1000);
    CHECK(std::count(seen.begin(), seen.begin() + 1000, true) == 1000);

    memory.decommit(thread_base, kThreadPages * kPage);
#else
    // Without the macro the reservation is not watched, so the query is refused.
    void* addresses[4];
    ULONG_PTR count = 4;
    ULONG granularity = 0;
    CHECK(GetWriteWatch(0, memory.base() + kTestBase, kPage, addresses, &count, &granularity) != 0);
#endif
    memory.decommit(kTestBase, 16 * kPage);
}

void testTopOfAddressSpace() {
    std::unique_ptr<tox2::GuestMemory> memory(newMemory());
    std::uint8_t* base = memory->base();

    CHECK(memory->commit(0xFFFFF000u, 0x1000));
    CHECK(!memory->commit(0xFFFFFFFCu, 8));  // would run past the 32-bit space
    tox2Write64(base, 0xFFFFFFF8u, 0x0102030405060708ull);
    CHECK(tox2Read64(base, 0xFFFFFFF8u) == 0x0102030405060708ull);
    tox2Write32(base, 0xFFFFFFFCu, 0xA1B2C3D4u);
    CHECK(tox2Read32(base, 0xFFFFFFFCu) == 0xA1B2C3D4u);

    // An 8-byte access at 0xFFFFFFFC spills into the guard tail. The tail belongs to the same
    // reservation and stays inaccessible, so it faults instead of touching foreign memory.
    MEMORY_BASIC_INFORMATION tail{};
    VirtualQuery(base + kGuestSpace, &tail, sizeof tail);
    CHECK(tail.AllocationBase == base);
    CHECK(tail.State == MEM_RESERVE);
    CHECK(tail.RegionSize >= static_cast<SIZE_T>(kTox2GuestGuardSize));
}

void testAllocatorFirstFit() {
    std::unique_ptr<tox2::GuestMemory> memory(newMemory());
    tox2::RegionAllocator alloc(*memory, tox2::kMainRegion);

    const std::uint32_t a = alloc.allocate(0x1000, 0);
    const std::uint32_t b = alloc.allocate(0x10001, 0);
    const std::uint32_t c = alloc.allocate(0x10000, 0x80000);
    CHECK(a == 0x10000);
    CHECK(b == 0x20000);  // 0x10001 rounds up to two 64 KiB granules
    CHECK(c == 0x80000);  // alignment leaves 0x40000..0x80000 free
    CHECK(queryState(*memory, a) == MEM_COMMIT);

    // First fit takes the hole before the end of the region.
    const std::uint32_t d = alloc.allocate(0x10000, 0);
    CHECK(d == 0x40000);

    CHECK(alloc.free(b) == 0x20000);
    CHECK(alloc.free(b) == 0);        // already freed
    CHECK(alloc.free(0x10010) == 0);  // not an allocation start
    CHECK(queryState(*memory, b) == MEM_RESERVE);
    CHECK(alloc.allocate(0x20000, 0) == b);  // reuse of the freed hole
    CHECK(tox2Read32(memory->base(), b) == 0);

    CHECK(alloc.allocate(0, 0) == 0);
    CHECK(alloc.allocate(0x1000, 0x30000) == 0);  // not a power of two

    // Memory returned by a reuse is zero again.
    tox2Write32(memory->base(), a, 0xFFFFFFFFu);
    CHECK(alloc.free(a) == 0x10000);
    CHECK(alloc.allocate(0x10000, 0) == a);
    CHECK(tox2Read32(memory->base(), a) == 0);
}

void testAllocatorMarkUsedAndExhaustion() {
    std::unique_ptr<tox2::GuestMemory> memory(newMemory());
    const tox2::GuestRegion tiny{0x10000, 0x50000, 0x10000, 0, "tiny"};
    tox2::RegionAllocator alloc(*memory, tiny);

    alloc.markUsed(0x20000, 0x10000);
    CHECK(alloc.allocate(0x10000, 0) == 0x10000);
    CHECK(alloc.allocate(0x10000, 0) == 0x30000);
    CHECK(alloc.allocate(0x10000, 0) == 0x40000);
    CHECK(alloc.allocate(0x10000, 0) == 0);  // exhausted
    CHECK(alloc.allocate(0x40000, 0) == 0);  // larger than the region
    CHECK(alloc.allocate(0xFFFFFFFFu, 0) == 0);
    CHECK(alloc.free(0x20000) == 0x10000);  // marked ranges are freeable
    CHECK(alloc.allocate(0x10000, 0) == 0x20000);

    // Marking outside the region or over an existing range is rejected.
    tox2::setLogSink([](std::string_view) {});
    alloc.markUsed(0x60000, 0x1000);
    alloc.markUsed(0x30000, 0x1000);
    tox2::setLogSink({});
    CHECK(alloc.free(0x60000) == 0);
    CHECK(alloc.free(0x30000) == 0x10000);
}

void testStackGuards() {
    std::unique_ptr<tox2::GuestMemory> memory(newMemory());
    tox2::RegionAllocator stack(*memory, tox2::kStackRegion);

    const std::uint32_t a = stack.allocate(0x10000, 0x1000);
    CHECK(a == 0xD0001000);
    CHECK(queryState(*memory, a - 0x1000) == MEM_RESERVE);  // guard below
    CHECK(queryState(*memory, a) == MEM_COMMIT);
    CHECK(queryState(*memory, a + 0xF000) == MEM_COMMIT);
    CHECK(queryState(*memory, a + 0x10000) == MEM_RESERVE);  // guard above

    const std::uint32_t b = stack.allocate(0x1000, 0x1000);
    CHECK(b == 0xD0013000);  // the upper guard of a, then the lower guard of b
    CHECK(queryState(*memory, b - 0x1000) == MEM_RESERVE);
    CHECK(stack.allocate(0xE0000000u - 0xD0000000u, 0x1000) == 0);
    CHECK(stack.free(a) == 0x10000);
    CHECK(stack.allocate(0x2000, 0x1000) == a);
}

void testProcessSingleton() {
    tox2::Process& p = tox2::process();
    CHECK(&p == &tox2::process());
    CHECK(p.main.region().begin == tox2::kMainRegion.begin);
    CHECK(p.stack.region().guard == 0x1000);
    CHECK(p.user_memory_total == 0xD500000);
    CHECK(!p.strict);
}

// Builds a minimal big-endian ELF64 in memory: two PT_LOAD (the second with bss), PT_TLS and a
// process parameter block inside the first segment.
std::vector<std::uint8_t> syntheticElf(std::uint16_t machine) {
    std::vector<std::uint8_t> elf(0x320, 0);
    auto put = [&](std::size_t at, std::uint64_t value, int size) {
        for (int i = 0; i < size; i++) {
            elf[at + i] = static_cast<std::uint8_t>(value >> (8 * (size - 1 - i)));
        }
    };
    std::memcpy(elf.data(),
                "\x7f"
                "ELF",
                4);
    elf[4] = 2;
    elf[5] = 2;
    put(18, machine, 2);
    put(24, 0x10080, 8);  // e_entry: the function descriptor
    put(32, 64, 8);
    put(54, 56, 2);
    put(56, 4, 2);

    auto phdr = [&](int index, std::uint32_t type, std::uint32_t flags, std::uint64_t offset, std::uint64_t vaddr,
                    std::uint64_t filesz, std::uint64_t memsz) {
        const std::size_t at = 64 + 56 * index;
        put(at, type, 4);
        put(at + 4, flags, 4);
        put(at + 8, offset, 8);
        put(at + 16, vaddr, 8);
        put(at + 24, vaddr, 8);
        put(at + 32, filesz, 8);
        put(at + 40, memsz, 8);
    };
    phdr(0, 1, 0x400005, 0x200, 0x10000, 0x100, 0x100);
    phdr(1, 1, 0x600006, 0x300, 0x30000, 0x20, 0x1000);
    phdr(2, 7, 4, 0x300, 0x30000, 4, 0x10);
    phdr(3, 0x60000001, 0, 0x2c0, 0x100c0, 0x20, 0x20);

    put(0x280, 0x10010, 4);  // descriptor at 0x10080: code address, then TOC
    put(0x284, 0x18000, 4);
    put(0x2c0, 0x20, 4);  // process parameter block
    put(0x2c4, 0x13bcc5f6, 4);
    put(0x2cc, 0x00430001, 4);
    put(0x2d0, 1001, 4);
    put(0x2d4, 0x8000, 4);
    put(0x2d8, 0x100000, 4);
    put(0x2dc, 7, 4);
    put(0x300, 0xcafef00d, 4);  // first word of the data segment
    return elf;
}

std::filesystem::path writeTempFile(const char* name, const std::vector<std::uint8_t>& bytes) {
    const std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            (std::string("tox2_runtime_test_") + std::to_string(GetCurrentProcessId()) + name);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
}

void testElfLoader() {
    tox2::setLogSink([](std::string_view) {});
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());

    const auto bad_path = writeTempFile("_bad.elf", syntheticElf(62));
    const auto bad = tox2::loadExecutable(bad_path);
    std::filesystem::remove(bad_path);
    CHECK(!bad.has_value());
    if (!bad) {
        CHECK(bad.error().find("e_machine") != std::string::npos);
    }

    const auto path = writeTempFile("_ok.elf", syntheticElf(21));
    const auto loaded = tox2::loadExecutable(path);
    std::filesystem::remove(path);
    tox2::setLogSink({});

    CHECK(loaded.has_value());
    if (!loaded) {
        std::fprintf(stderr, "load error: %s\n", loaded.error().c_str());
        return;
    }
    const tox2::LoadedExecutable& exe = *loaded;
    CHECK(exe.entry_opd == 0x10080);
    CHECK(exe.entry_func == 0x10010);
    CHECK(exe.entry_toc == 0x18000);
    CHECK(exe.tls_vaddr == 0x30000 && exe.tls_filesz == 4 && exe.tls_memsz == 0x10);
    CHECK(exe.sdk_version == 0x00430001);
    CHECK(exe.primary_prio == 1001);
    CHECK(exe.primary_stacksize == 0x8000);
    CHECK(exe.malloc_pagesize == 0x100000);
    CHECK(exe.ppc_seg == 7);
    CHECK(exe.image_end == 0x40000);
    CHECK(exe.sha256.size() == 64);

    const std::uint8_t* base = p.memory.base();
    CHECK(tox2Read32(base, 0x30000) == 0xcafef00d);
    CHECK(tox2Read32(base, 0x30020) == 0 && tox2Read32(base, 0x30ffc) == 0);  // bss
    // The image range is taken, so the first free block starts after it.
    CHECK(p.main.allocate(0x10000, 0x10000) == 0x40000);
}

void putBigEndian(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint64_t value, int size) {
    for (int i = 0; i < size; i++) {
        bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * (size - 1 - i)));
    }
}

// A minimal relocated PRX: segment 0 at `base` (memsz trimmed to the distance to segment 1), segment 1
// directly after it with bss, and a third header that tests turn into PT_SCE_PPURELA. The module info
// sits at file offset 0x2C0, which is address base + 0xC0.
std::vector<std::uint8_t> syntheticPrx(std::uint32_t base) {
    std::vector<std::uint8_t> prx(0x400, 0);
    std::memcpy(prx.data(),
                "\x7f"
                "ELF",
                4);
    prx[4] = 2;
    prx[5] = 2;
    putBigEndian(prx, 16, 0xFFA4, 2);
    putBigEndian(prx, 18, 21, 2);
    putBigEndian(prx, 32, 64, 8);
    putBigEndian(prx, 54, 56, 2);
    putBigEndian(prx, 56, 3, 2);

    auto phdr = [&](int index, std::uint32_t type, std::uint32_t flags, std::uint64_t offset, std::uint64_t vaddr,
                    std::uint64_t paddr, std::uint64_t filesz, std::uint64_t memsz) {
        const std::size_t at = 64 + 56 * index;
        putBigEndian(prx, at, type, 4);
        putBigEndian(prx, at + 4, flags, 4);
        putBigEndian(prx, at + 8, offset, 8);
        putBigEndian(prx, at + 16, vaddr, 8);
        putBigEndian(prx, at + 24, paddr, 8);
        putBigEndian(prx, at + 32, filesz, 8);
        putBigEndian(prx, at + 40, memsz, 8);
    };
    phdr(0, 1, 0x400005, 0x200, base, 0x2C0, 0x100, 0x10000);
    phdr(1, 1, 0x600006, 0x300, base + 0x10000, 0x300, 0x20, 0x1000);
    phdr(2, 0, 0, 0, 0, 0, 0, 0);

    putBigEndian(prx, 0x200, 0xfeedbead, 4);
    putBigEndian(prx, 0x240, 0x51000, 4);  // a function descriptor: code address, then TOC
    putBigEndian(prx, 0x244, base + 0x10800, 4);
    putBigEndian(prx, 0x2C2, 1, 1);  // module info: version 1.1, name, TOC
    putBigEndian(prx, 0x2C3, 1, 1);
    std::memcpy(prx.data() + 0x2C4, "testPrx", 8);
    putBigEndian(prx, 0x2E0, base + 0x10800, 4);
    putBigEndian(prx, 0x300, 0xcafef00d, 4);
    return prx;
}

std::atomic<tox2::GuestThread*> g_boot_thread{nullptr};

TOX2_PPU_FUNC(testBootEntry) {
    (void)base;
    g_boot_thread = tox2::currentThread();
    ctx->gpr[3] = 0;
}

void waitForBootThread() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        tox2::GuestThread* thread = g_boot_thread.load();
        if (thread != nullptr && thread->state() == tox2::ThreadState::Exited) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void testLoadModule() {
    tox2::setLogSink([](std::string_view) {});
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());

    // testElfLoader left its image (0x10000..0x40000) and one block above it in the main region.
    constexpr std::uint32_t kBase = 0x50000;
    const std::uint32_t used_before = p.user_memory_used;

    // Every refusal names its cause, and none of them touches guest memory.
    auto refusal = [&](const char* name, std::uint32_t expected_base, std::uint32_t lowest_free,
                       const std::function<void(std::vector<std::uint8_t>&)>& mutate, std::string_view needle) {
        std::vector<std::uint8_t> prx = syntheticPrx(kBase);
        mutate(prx);
        const auto path = writeTempFile(name, prx);
        const auto result = tox2::loadModule(path, expected_base, lowest_free);
        std::filesystem::remove(path);
        CHECK(!result.has_value());
        if (result) return;
        CHECK(result.error().find(needle) != std::string::npos);
        if (result.error().find(needle) == std::string::npos) {
            std::fprintf(stderr, "refusal %s: unexpected message: %s\n", name, result.error().c_str());
        }
    };
    const auto none = [](std::vector<std::uint8_t>&) {};
    CHECK(!tox2::loadModule(std::filesystem::temp_directory_path() / "tox2_no_such_module.elf", kBase, kBase));
    refusal("_not_elf.prx", kBase, kBase, [](auto& b) { b[0] = 'X'; }, "not an ELF");
    refusal("_little.prx", kBase, kBase, [](auto& b) { b[5] = 1; }, "big-endian");
    refusal("_machine.prx", kBase, kBase, [](auto& b) { putBigEndian(b, 18, 62, 2); }, "e_machine");
    refusal("_etype.prx", kBase, kBase, [](auto& b) { putBigEndian(b, 16, 2, 2); }, "e_type");
    refusal("_phdrs.prx", kBase, kBase, [](auto& b) { putBigEndian(b, 56, 0x1000, 2); }, "program header table");
    refusal("_filesz.prx", kBase, kBase, [](auto& b) { putBigEndian(b, 64 + 32, 0x1000, 8); }, "past the end");
    refusal(
            "_relocs.prx", kBase, kBase, [](auto& b) { putBigEndian(b, 64 + 112, 0x700000A4, 4); },
            "relocations not applied");
    refusal(
            "_noload.prx", kBase, kBase,
            [](auto& b) {
                putBigEndian(b, 64, 0, 4);
                putBigEndian(b, 64 + 56, 0, 4);
            },
            "no PT_LOAD");
    refusal("_phdr0.prx", kBase, kBase, [](auto& b) { putBigEndian(b, 64, 4, 4); }, "program header 0");
    refusal("_overlap.prx", kBase, kBase, [](auto& b) { putBigEndian(b, 64 + 56 + 16, kBase + 0x80, 8); }, "overlap");
    refusal("_below.prx", kBase, kBase, [](auto& b) { putBigEndian(b, 64 + 56 + 16, 0x8000, 8); }, "main region");
    refusal("_above.prx", kBase, kBase, [](auto& b) { putBigEndian(b, 64 + 56 + 16, 0x0FFFF800, 8); }, "main region");
    refusal("_filesz_memsz.prx", kBase, kBase, [](auto& b) { putBigEndian(b, 64 + 56 + 40, 0x10, 8); }, "main region");
    refusal("_base.prx", kBase + 0x10000, kBase, none, "expected 0x00060000");
    refusal("_free.prx", kBase, kBase + 0x10000, none, "first free address");
    refusal("_info_high.prx", kBase, kBase, [](auto& b) { putBigEndian(b, 64 + 24, 0x2FC, 8); }, "module info");
    refusal("_info_low.prx", kBase, kBase, [](auto& b) { putBigEndian(b, 64 + 24, 0x1F0, 8); }, "module info");
    CHECK(!p.memory.isCommitted(kBase, 4));

    const auto path = writeTempFile("_ok.prx", syntheticPrx(kBase));
    const auto loaded = tox2::loadModule(path, kBase, kBase);
    std::filesystem::remove(path);
    CHECK(loaded.has_value());
    if (!loaded) {
        std::fprintf(stderr, "module load error: %s\n", loaded.error().c_str());
        tox2::setLogSink({});
        return;
    }
    const tox2::LoadedModule& module = *loaded;
    CHECK(module.name == "testPrx");
    CHECK(module.version_major == 1 && module.version_minor == 1);
    CHECK(module.base == kBase);
    CHECK(module.image_end == kBase + 0x20000);
    CHECK(module.toc == kBase + 0x10800);
    CHECK(module.module_info == kBase + 0xC0);
    CHECK(module.sha256.size() == 64);

    std::uint8_t* base = p.memory.base();
    CHECK(tox2Read32(base, kBase) == 0xfeedbead);
    CHECK(tox2Read32(base, kBase + 0x10000) == 0xcafef00d);
    CHECK(tox2Read32(base, kBase + 0xC0 + 0x20) == module.toc);  // the module info is in guest memory
    CHECK(tox2Read32(base, kBase + 0x2000) == 0);                // trimmed gap between the segments
    CHECK(tox2Read32(base, kBase + 0x10024) == 0 && tox2Read32(base, kBase + 0x10ffc) == 0);  // bss
    // The image range is taken, so the next block starts after it.
    CHECK(p.main.allocate(0x10000, 0x10000) == kBase + 0x20000);

    // The boot charge adds the module bytes to the stack and the executable image. The thread runs
    // after bootMainThread returns and leaves at once.
    tox2::LoadedExecutable exe;
    exe.entry_opd = kBase + 0x40;
    exe.entry_func = 0x51000;
    exe.entry_toc = module.toc;
    exe.tls_vaddr = kBase + 0x10000;
    exe.tls_filesz = 4;
    exe.tls_memsz = 0x10;
    exe.primary_prio = 1001;
    exe.primary_stacksize = 0x10000;
    exe.image_end = kBase;
    const Tox2FuncMapping mappings[] = {{0x51000, testBootEntry}};
    tox2RegisterFuncs(mappings, 1);
    const std::uint32_t exe_bytes = exe.image_end - tox2::kMainRegion.begin;

    g_boot_thread = nullptr;
    tox2::bootMainThread(exe, "boot_test", 0x20000);
    CHECK(p.user_memory_used == 0x10000 + exe_bytes + 0x20000);
    waitForBootThread();
    CHECK(g_boot_thread.load() != nullptr);

    g_boot_thread = nullptr;
    tox2::bootMainThread(exe, "boot_test");
    CHECK(p.user_memory_used == 0x10000 + exe_bytes);
    waitForBootThread();

    tox2RegisterFuncs(nullptr, 0);
    p.user_memory_used = used_before;
    CHECK(p.main.free(kBase + 0x20000) == 0x10000);
    CHECK(p.main.free(kBase) == 0x20000);
    CHECK(!p.memory.isCommitted(kBase, 4));
    tox2::setLogSink({});
}

void testLibsreLayout() {
    namespace libsre = tox2::libsre;
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    CHECK(p.memory.commit(libsre::kBase, libsre::kImageEnd - libsre::kBase));
    std::uint8_t* base = p.memory.base();

    // An image that satisfies every structural check, built from the layout constants only.
    std::memcpy(base + libsre::kModuleInfo + 4, "cellSre_Library", 16);
    tox2Write32(base, libsre::kModuleInfo + 0x20, libsre::kToc);
    tox2Write32(base, libsre::kModuleInfo + 0x24, libsre::kExportsBegin);
    tox2Write32(base, libsre::kModuleInfo + 0x28, libsre::kExportsEnd);
    tox2Write32(base, libsre::kModuleInfo + 0x2C, libsre::kImportsBegin);
    tox2Write32(base, libsre::kModuleInfo + 0x30, libsre::kImportsEnd);
    for (std::uint32_t i = 0; i < 352; i++) {
        tox2Write32(base, libsre::kOpdBegin + 8 * i, libsre::kBase + 0x100 + 4 * i);
        tox2Write32(base, libsre::kOpdBegin + 8 * i + 4, libsre::kToc);
    }
    for (std::uint32_t i = 0; i < 32; i++) {
        tox2Write32(base, libsre::kImportSlots + 4 * i, libsre::kCodeEnd + 32 * i);
    }
    for (const auto& [address, entry] :
         {std::pair{libsre::kSpuKernel1, 0x818u}, std::pair{libsre::kSpuKernel2, 0x848u}}) {
        std::memcpy(base + address,
                    "\x7f"
                    "ELF",
                    4);
        base[address + 4] = 1;
        base[address + 5] = 2;
        tox2Write16(base, address + 0x12, 23);
        tox2Write32(base, address + 0x18, entry);
    }
    const std::uint32_t blobs[] = {libsre::kSpuSystemService,  libsre::kSpuTasksetPm,   libsre::kSpuTaskset2ExitHandler,
                                   libsre::kSpuExitCodePlugin, libsre::kSpuJobChainPm1, libsre::kSpuJobChainPm2};
    for (const std::uint32_t blob : blobs) {
        tox2Write32(base, blob, (0x21u << 25) | (0x1234u << 7) | 2);
    }

    std::vector<Tox2ImportInfo> imports;
    for (std::uint32_t i = 0; i < 32; i++) {
        imports.push_back({libsre::kCodeEnd + 32 * i, 0x1000 + i, "module", nullptr});
    }
    std::vector<Tox2ExportInfo> exports = {{libsre::kOpdBegin, 1, "module"},
                                           {libsre::kOpdBegin + 8 * 351, 2, "module"},
                                           {libsre::kOpdBegin + 8 * 3, 3, "other"}};

    CHECK(libsre::checkLibsreLayout(base, imports, exports).empty());

    // Every check, broken on its own, yields exactly one message that names it.
    auto expectOne = [&](const char* what, const std::vector<std::string>& problems, std::string_view needle) {
        CHECK(problems.size() == 1);
        if (problems.size() != 1 || problems[0].find(needle) == std::string::npos) {
            std::fprintf(stderr, "layout check %s: %zu messages:\n", what, problems.size());
            for (const std::string& line : problems) std::fprintf(stderr, "  %s\n", line.c_str());
            g_failed++;
        }
    };
    auto poke32 = [&](const char* what, std::uint32_t at, std::uint32_t value, std::string_view needle) {
        const std::uint32_t old = tox2Read32(base, at);
        tox2Write32(base, at, value);
        expectOne(what, libsre::checkLibsreLayout(base, imports, exports), needle);
        tox2Write32(base, at, old);
    };
    auto poke8 = [&](const char* what, std::uint32_t at, std::uint8_t value, std::string_view needle) {
        const std::uint8_t old = base[at];
        base[at] = value;
        expectOne(what, libsre::checkLibsreLayout(base, imports, exports), needle);
        base[at] = old;
    };

    poke8("module name", libsre::kModuleInfo + 4, 'X', "name is not");
    poke8("module name terminator", libsre::kModuleInfo + 4 + 15, 'x', "name is not");
    poke32("module toc", libsre::kModuleInfo + 0x20, libsre::kToc + 8, "TOC is");
    poke32("exports start", libsre::kModuleInfo + 0x24, 0, "exports start");
    poke32("exports end", libsre::kModuleInfo + 0x28, 0, "exports end");
    poke32("imports start", libsre::kModuleInfo + 0x2C, 0, "imports start");
    poke32("imports end", libsre::kModuleInfo + 0x30, 0, "imports end");

    const std::uint32_t opd = libsre::kOpdBegin + 8 * 100;
    poke32("descriptor toc", opd + 4, libsre::kToc + 8, "TOC word");
    poke32("descriptor entry misaligned", opd, libsre::kBase + 0x103, "descriptor 0x012012cc: entry");
    poke32("descriptor entry at code end", opd, libsre::kCodeEnd, "4-aligned address in the code");
    poke32("descriptor entry below base", opd, libsre::kBase - 4, "4-aligned address in the code");
    poke32("first descriptor", libsre::kOpdBegin + 4, 0, "TOC word");
    poke32("last descriptor", libsre::kOpdEnd - 8, 0, "4-aligned address in the code");

    poke32("slot misaligned", libsre::kImportSlots + 4 * 3, libsre::kCodeEnd + 1, "import slot 3");
    poke32("slot below thunks", libsre::kImportSlots + 4 * 4, libsre::kCodeEnd - 32, "import slot 4");
    poke32("slot past thunks", libsre::kImportSlots + 4 * 31, libsre::kThunksEnd, "import slot 31");

    imports[5].stub = libsre::kThunksEnd;
    expectOne("stub past thunks", libsre::checkLibsreLayout(base, imports, exports), "import stub");
    imports[5].stub = libsre::kCodeEnd + 32 * 5 + 4;
    expectOne("stub misaligned", libsre::checkLibsreLayout(base, imports, exports), "import stub");
    imports[5].stub = libsre::kCodeEnd + 32 * 5;

    exports[1].opd = libsre::kOpdEnd;
    expectOne("export past descriptors", libsre::checkLibsreLayout(base, imports, exports), "export descriptor");
    exports[1].opd = libsre::kOpdBegin - 8;
    expectOne("export below descriptors", libsre::checkLibsreLayout(base, imports, exports), "export descriptor");
    exports[1].opd = libsre::kOpdBegin + 8 * 351;

    struct Kernel {
        std::uint32_t address;
        std::string name;
        std::uint32_t entry;
    };
    for (const Kernel& kernel : {Kernel{libsre::kSpuKernel1, "SPU kernel 1 at", 0x818},
                                 Kernel{libsre::kSpuKernel2, "SPU kernel 2 at", 0x848}}) {
        poke8("kernel magic", kernel.address + 1, 'X', kernel.name);
        poke8("kernel class", kernel.address + 4, 2, kernel.name);
        poke8("kernel data", kernel.address + 5, 1, kernel.name);
        poke32("kernel machine", kernel.address + 0x10, 0x00020015, kernel.name);
        poke32("kernel entry", kernel.address + 0x18, kernel.entry + 4, kernel.name);
    }

    const char* const blob_names[] = {"system service",   "taskset policy module",     "taskset2 exit handler",
                                      "exit code plugin", "job chain policy module 1", "job chain policy module 2"};
    for (std::size_t i = 0; i < 6; i++) {
        poke32("blob zero", blobs[i], 0, blob_names[i]);
        poke32("blob wrong opcode", blobs[i], (0x20u << 25) | (0x1234u << 7) | 2, blob_names[i]);
        poke32("blob wrong target", blobs[i], (0x21u << 25) | (0x1234u << 7) | 3, blob_names[i]);
    }

    // The immediate of the `ila` is not checked.
    tox2Write32(base, libsre::kSpuJobChainPm2, (0x21u << 25) | (0x3FFFFu << 7) | 2);
    CHECK(libsre::checkLibsreLayout(base, imports, exports).empty());

    p.memory.decommit(libsre::kBase, libsre::kImageEnd - libsre::kBase);
}

TOX2_PPU_FUNC(testFuncA) {
    (void)base;
    ctx->gpr[3] = 1;
}

TOX2_PPU_FUNC(testFuncB) {
    (void)base;
    ctx->gpr[3] = 2;
}

std::size_t countContaining(const std::vector<std::string>& lines, std::string_view needle) {
    std::size_t n = 0;
    for (const std::string& line : lines) {
        n += line.find(needle) != std::string::npos ? 1 : 0;
    }
    return n;
}

void testDispatch() {
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetCensus();
    tox2::resetLogOnce();

    const Tox2FuncMapping mappings[] = {{0x10200, testFuncA}, {0x10210, testFuncB}, {0x10400, testFuncA}};
    tox2RegisterFuncs(mappings, 3);
    CHECK(countContaining(lines, "tox2: dispatch: 3 entries, span 0x00010200..0x00010404") == 1);

    CHECK(tox2Lookup(0x10200) == testFuncA);
    CHECK(tox2Lookup(0x10210) == testFuncB);
    CHECK(tox2Lookup(0x10400) == testFuncA);
    CHECK(tox2Lookup(0x10204) == nullptr);  // inside the span, no function
    CHECK(tox2Lookup(0x10201) == nullptr);  // misaligned
    CHECK(tox2Lookup(0x10211) == nullptr);
    CHECK(tox2Lookup(0x101fc) == nullptr);  // below the span
    CHECK(tox2Lookup(0x10404) == nullptr);  // above the span
    CHECK(tox2Lookup(0) == nullptr);
    CHECK(tox2Lookup(0xFFFFFFFCu) == nullptr);

    auto ctx = std::make_unique<Tox2PpuContext>();
    ctx->jump_target = 0x10210;
    tox2Dispatch(ctx.get(), nullptr);
    CHECK(ctx->gpr[3] == 2);
    ctx->jump_target = 0x10200;
    tox2Dispatch(ctx.get(), nullptr);
    CHECK(ctx->gpr[3] == 1);

    // Not strict: the unknown transfer returns, leaves the registers alone, and logs once per pair.
    ctx->gpr[3] = 77;
    ctx->trace_pos = 2;
    ctx->trace[0] = 0x10200;
    ctx->trace[1] = 0x10210;
    lines.clear();
    for (int i = 0; i < 3; i++) {
        ctx->jump_target = 0x10204;
        ctx->jump_site = 0x10300;
        tox2Dispatch(ctx.get(), nullptr);
    }
    CHECK(ctx->gpr[3] == 77);
    CHECK(countContaining(lines, "unknown target 0x00010204 from 0x00010300: inside a code span") == 1);
    CHECK(countContaining(lines, "trace (oldest -> newest): 00010200 00010210") == 1);

    ctx->jump_target = 0x20000;
    tox2Dispatch(ctx.get(), nullptr);
    CHECK(countContaining(lines, "unknown target 0x00020000 from 0x00010300: outside the code spans") == 1);

    const std::string census = tox2::formatCensus();
    CHECK(census.find("census: unknown targets 2 distinct\n  0x00010204 from 0x00010300: 3\n"
                      "  0x00020000 from 0x00010300: 1") != std::string::npos);

    tox2RegisterFuncs(nullptr, 0);
    CHECK(tox2Lookup(0x10200) == nullptr);
    tox2::setLogSink({});
    tox2::resetCensus();
}

void testDispatchSpans() {
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetCensus();
    tox2::resetLogOnce();

    const Tox2FuncMapping first[] = {{0x10200, testFuncA}, {0x10210, testFuncB}};
    const Tox2FuncMapping second[] = {{0x20000, testFuncB}, {0x20040, testFuncA}};
    tox2RegisterFuncs(first, 2);
    tox2::appendFuncs(second, 2);
    CHECK(countContaining(lines, "tox2: dispatch: 2 entries, span 0x00010200..0x00010214") == 1);
    CHECK(countContaining(lines, "tox2: dispatch: 2 entries, span 0x00020000..0x00020044") == 1);

    CHECK(tox2Lookup(0x10200) == testFuncA);
    CHECK(tox2Lookup(0x10210) == testFuncB);
    CHECK(tox2Lookup(0x20000) == testFuncB);
    CHECK(tox2Lookup(0x20040) == testFuncA);
    CHECK(tox2Lookup(0x20004) == nullptr);  // inside the second span, no function
    CHECK(tox2Lookup(0x15000) == nullptr);  // between the spans
    CHECK(tox2Lookup(0x20044) == nullptr);

    // The walk back stays inside the span that holds the address.
    CHECK(tox2::findOwningFunction(0x10210) == 0x10210);
    CHECK(tox2::findOwningFunction(0x20008) == 0x20000);
    CHECK(tox2::findOwningFunction(0x20040) == 0x20040);
    CHECK(tox2::findOwningFunction(0x15000) == 0);
    CHECK(tox2::findOwningFunction(0x1ffff) == 0);

    // A span added below the first one is found too, and the order of registration does not matter.
    const Tox2FuncMapping third[] = {{0x8000, testFuncA}};
    tox2::appendFuncs(third, 1);
    CHECK(tox2Lookup(0x8000) == testFuncA);
    CHECK(tox2::findOwningFunction(0x8000) == 0x8000);

    auto ctx = std::make_unique<Tox2PpuContext>();
    ctx->jump_target = 0x20000;
    tox2Dispatch(ctx.get(), nullptr);
    CHECK(ctx->gpr[3] == 2);

    lines.clear();
    ctx->jump_target = 0x20004;
    ctx->jump_site = 0x10300;
    tox2Dispatch(ctx.get(), nullptr);
    CHECK(countContaining(lines, "unknown target 0x00020004 from 0x00010300: inside a code span") == 1);
    ctx->jump_target = 0x15000;
    tox2Dispatch(ctx.get(), nullptr);
    CHECK(countContaining(lines, "unknown target 0x00015000 from 0x00010300: outside the code spans") == 1);

    // Registering again replaces every span, appended ones included.
    tox2RegisterFuncs(first, 2);
    CHECK(tox2Lookup(0x10200) == testFuncA);
    CHECK(tox2Lookup(0x20000) == nullptr);
    CHECK(tox2::findOwningFunction(0x20008) == 0);
    CHECK(tox2Lookup(0x8000) == nullptr);

    // Spans that touch without overlapping are fine.
    const Tox2FuncMapping adjacent[] = {{0x10214, testFuncB}};
    tox2::appendFuncs(adjacent, 1);
    CHECK(tox2Lookup(0x10214) == testFuncB);
    CHECK(tox2::findOwningFunction(0x10214) == 0x10214);

    tox2RegisterFuncs(nullptr, 0);
    tox2::appendFuncs(nullptr, 0);
    CHECK(tox2Lookup(0x10200) == nullptr);
    tox2::setLogSink({});
    tox2::resetCensus();
}

std::vector<tox2::ThreadCensusEntry> fakeThreads() {
    using State = tox2::ThreadCensusEntry::State;
    return {{0x01000000, "main_thread", State::Blocked, "sys_event_queue_receive"},
            {0x01000001, "worker", State::Exited, ""},
            {0x01000002, "idle", State::Running, ""}};
}

void testCensus() {
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetCensus();

    tox2::censusSyscall(141, "sys_timer_usleep", true);
    tox2::censusSyscall(988, nullptr, false);
    for (int i = 0; i < 119; i++) tox2::censusSyscall(141, "sys_timer_usleep", true);
    tox2::censusSyscall(988, nullptr, false);
    tox2::censusSyscall(988, nullptr, false);
    tox2::censusHle("sysPrxForUser", "sys_lwmutex_lock", true);
    tox2::censusHle("cellSysutil", "cellSysutilRegisterCallback", false);
    tox2::censusHle("sysPrxForUser", "sys_lwmutex_lock", true);
    tox2::setThreadCensusProvider(&fakeThreads);

    CHECK(lines.size() == 4);
    CHECK(countContaining(lines, "tox2: first sc 141 sys_timer_usleep") == 1);
    CHECK(countContaining(lines, "tox2: first sc 988 ?") == 1);
    CHECK(countContaining(lines, "tox2: first hle sysPrxForUser::sys_lwmutex_lock") == 1);
    CHECK(countContaining(lines, "tox2: first hle cellSysutil::cellSysutilRegisterCallback") == 1);

    const std::string expected =
            "census: syscalls 123 calls, 2 distinct, 1 default-handled\n"
            "  sc 141 sys_timer_usleep: 120 [impl]\n"
            "  sc 988 ?: 3 [default]\n"
            "census: hle 3 calls, 2 distinct, 1 default-handled\n"
            "  sysPrxForUser::sys_lwmutex_lock: 2 [impl]\n"
            "  cellSysutil::cellSysutilRegisterCallback: 1 [default]\n"
            "census: unknown targets 0 distinct\n"
            "census: threads 3 created, 2 live\n"
            "  0x01000000 main_thread: blocked in sys_event_queue_receive\n"
            "  0x01000001 worker: exited\n"
            "  0x01000002 idle: running";
    const std::string actual = tox2::formatCensus();
    CHECK(actual == expected);
    if (actual != expected) std::fprintf(stderr, "census was:\n%s\n", actual.c_str());

    lines.clear();
    tox2::printCensus();
    CHECK(lines.size() == 11);
    CHECK(!lines.empty() && lines[0].find("census: syscalls 123 calls") != std::string::npos);

    tox2::setLogSink({});
    tox2::resetCensus();
}

bool g_fake_rendering = false;

tox2::RsxCensus fakeRenderingCensus() {
    tox2::RsxCensus census;
    census.headers = 10;
    census.flips = 2;
    census.rendering = g_fake_rendering;
    census.draws_rendered = 7;
    census.draws_skipped = 3;
    return census;
}

// The rsx census line ends with the draw counts only while a renderer runs.
void testCensusRenderSuffix() {
    tox2::resetCensus();
    tox2::setRsxCensusProvider(&fakeRenderingCensus);

    g_fake_rendering = false;
    std::string census = tox2::formatCensus();
    CHECK(census.find("census: rsx 10 headers, 0 distinct methods, 2 flips") != std::string::npos);
    CHECK(census.find("rendered") == std::string::npos);

    g_fake_rendering = true;
    census = tox2::formatCensus();
    CHECK(census.find("census: rsx 10 headers, 0 distinct methods, 2 flips, rendered 7 draws, skipped 3\n") !=
          std::string::npos);
    tox2::resetCensus();
}

void testLatencyStats() {
    // Bucket layout: 0 alone, four per octave from 1 us, one bucket for 2^27 us and above.
    CHECK(tox2::latencyBucketIndex(0) == 0);
    CHECK(tox2::latencyBucketIndex(1) == 1);
    CHECK(tox2::latencyBucketIndex(2) == 5);
    CHECK(tox2::latencyBucketIndex(1000) == 40 && tox2::latencyBucketUpperUs(40) == 1024);
    CHECK(tox2::latencyBucketIndex(1023) == 40 && tox2::latencyBucketIndex(1024) == 41);
    CHECK(tox2::latencyBucketUpperUs(0) == 0 && tox2::latencyBucketUpperUs(1) == 2);
    CHECK(tox2::latencyBucketIndex(70'000'000) == 105);  // last octave, 2^26 <= v < 2^27
    CHECK(tox2::latencyBucketIndex((1ull << 27) - 1) == tox2::kLatencyBuckets - 2);
    CHECK(tox2::latencyBucketIndex(1ull << 27) == tox2::kLatencyBuckets - 1);
    CHECK(tox2::latencyBucketIndex(~0ull) == tox2::kLatencyBuckets - 1);
    // From 4 us up every bucket holds whole values: the one below its upper bound is in it, the bound is in the next.
    bool monotonic = true;
    for (std::size_t i = 9; i + 1 < tox2::kLatencyBuckets; i++) {
        const std::uint64_t upper = tox2::latencyBucketUpperUs(i);
        monotonic = monotonic && upper > tox2::latencyBucketUpperUs(i - 1);
        monotonic = monotonic && tox2::latencyBucketIndex(upper - 1) == i && tox2::latencyBucketIndex(upper) == i + 1;
    }
    CHECK(monotonic);

    tox2::LatencyStats stats(1000);
    CHECK(stats.overUs() == 1000);
    tox2::LatencySnapshot empty = stats.snapshot();
    CHECK(empty.count == 0 && empty.meanUs() == 0.0 && empty.percentileUs(0.99) == 0);
    for (int i = 0; i < 99; i++) stats.record(100);
    stats.record(10'000);
    tox2::LatencySnapshot s = stats.snapshot();
    CHECK(s.count == 100 && s.sum_us == 19'900 && s.max_us == 10'000 && s.over == 1);
    CHECK(s.meanUs() == 199.0);
    CHECK(s.percentileUs(0.99) >= 100 && s.percentileUs(0.99) <= 125);
    CHECK(s.percentileUs(1.0) == 10'000);  // the top rank sits in the 10 ms bucket and is capped at max
    CHECK(s.buckets[tox2::latencyBucketIndex(100)] == 99 && s.buckets[tox2::latencyBucketIndex(10'000)] == 1);

    // A record equal to the threshold is not over; zero and the top bucket count.
    tox2::LatencyStats edge(5);
    edge.record(0);
    edge.record(5);
    edge.record(6);
    edge.record(70'000'000);
    edge.record(1ull << 28);
    const tox2::LatencySnapshot e = edge.snapshot();
    CHECK(e.count == 5 && e.over == 3 && e.max_us == (1ull << 28));
    CHECK(e.buckets[0] == 1 && e.buckets[tox2::kLatencyBuckets - 1] == 1 && e.buckets[105] == 1);
    CHECK(e.percentileUs(1.0) == (1ull << 28));  // the top bucket reports max_us

    // since(): the difference, with the maximum bounded by the highest bucket of the interval.
    tox2::LatencyStats ivl(1000);
    ivl.record(50);
    ivl.record(5'000);
    const tox2::LatencySnapshot first = ivl.snapshot();
    ivl.record(200);
    ivl.record(210);
    ivl.record(2'000);
    const tox2::LatencySnapshot second = ivl.snapshot();
    const tox2::LatencySnapshot d = second.since(first);
    CHECK(d.count == 3 && d.sum_us == 2'410 && d.over == 1);
    CHECK(second.max_us == 5'000);
    CHECK(d.max_us == tox2::latencyBucketUpperUs(tox2::latencyBucketIndex(2'000)) && d.max_us >= 2'000);
    CHECK(d.max_us < 5'000);
    CHECK(d.buckets[tox2::latencyBucketIndex(50)] == 0 && d.buckets[tox2::latencyBucketIndex(200)] == 2);
    const tox2::LatencySnapshot none = second.since(second);
    CHECK(none.count == 0 && none.max_us == 0 && none.percentileUs(0.5) == 0);

    // merge(): sums, and the larger maximum.
    tox2::LatencySnapshot merged = first;
    merged.merge(d);
    CHECK(merged.count == 5 && merged.sum_us == 7'460 && merged.over == 2 && merged.max_us == 5'000);
    CHECK(merged.buckets[tox2::latencyBucketIndex(200)] == 2 && merged.buckets[tox2::latencyBucketIndex(50)] == 1);
    tox2::LatencySnapshot low;
    low.merge(d);
    CHECK(low.max_us == d.max_us);

    // Format: milliseconds with two decimals and the threshold in the label.
    tox2::LatencyStats fixed(1000);
    for (int i = 0; i < 1222; i++) fixed.record(750);
    for (int i = 0; i < 12; i++) fixed.record(3100);
    const std::string line = tox2::formatLatency(fixed.snapshot(), fixed.overUs());
    CHECK(line == "n 1234 avg 0.77 p99 0.77 max 3.10 ms >1.00ms 12");
    if (line != "n 1234 avg 0.77 p99 0.77 max 3.10 ms >1.00ms 12")
        std::fprintf(stderr, "latency was: %s\n", line.c_str());
    CHECK(tox2::formatLatency({}, 5333) == "n 0 avg 0.00 p99 0.00 max 0.00 ms >5.33ms 0");

    // elapsedUs never goes negative.
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(tox2::elapsedUs(t0, t0 + std::chrono::microseconds(1500)) == 1500);
    CHECK(tox2::elapsedUs(t0 + std::chrono::microseconds(5), t0) == 0);

    // Concurrent recording loses nothing.
    tox2::LatencyStats shared(100);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&shared, t] {
            for (std::uint64_t i = 0; i < 100'000; i++) shared.record(i % 200 + static_cast<std::uint64_t>(t));
        });
    }
    for (auto& th : threads) th.join();
    const tox2::LatencySnapshot c = shared.snapshot();
    std::uint64_t bucket_total = 0;
    for (std::uint64_t n : c.buckets) bucket_total += n;
    CHECK(c.count == 400'000 && bucket_total == 400'000);
    CHECK(c.max_us == 199 + 3);
    CHECK(c.sum_us == 4ull * 500 * (199 * 200 / 2) + 100'000ull * (0 + 1 + 2 + 3));
    CHECK(c.over > 0);
}

void testDisableFrameBuilder() {
    tox2::RsxConsumer plain(nullptr);  // the base is only dereferenced when commands are parsed
    CHECK(!plain.frameBuilderEnabled());
    plain.enableFrameBuilder();
    CHECK(plain.frameBuilderEnabled());
    plain.disableFrameBuilder();
    CHECK(!plain.frameBuilderEnabled());
    CHECK(plain.takeCommands().empty());

    tox2::RsxConsumer capturing(nullptr);
    capturing.enableCapture({2});
    CHECK(capturing.frameBuilderEnabled());
    capturing.disableFrameBuilder();  // a capture needs the builder
    CHECK(capturing.frameBuilderEnabled());
}

void testSyscalls() {
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetCensus();
    tox2::resetLogOnce();

    const auto table = tox2::syscallTable();
    CHECK(table.size() == 100);  // every number the game issues plus the rest of the families that are implemented
    bool sorted = true;
    for (std::size_t i = 1; i < table.size(); i++) sorted = sorted && table[i - 1].number < table[i].number;
    CHECK(sorted);
    const tox2::SyscallEntry* usleep_entry = tox2::findSyscall(141);
    CHECK(usleep_entry != nullptr && std::string_view(usleep_entry->name) == "sys_timer_usleep");
    CHECK(usleep_entry != nullptr && usleep_entry->handler != nullptr);
    const tox2::SyscallEntry* unnamed = tox2::findSyscall(988);
    CHECK(unnamed != nullptr && unnamed->name == nullptr && unnamed->handler != nullptr);
    const tox2::SyscallEntry* detach_entry = tox2::findSyscall(45);
    CHECK(detach_entry != nullptr && std::string_view(detach_entry->name) == "sys_ppu_thread_detach");
    CHECK(detach_entry != nullptr && detach_entry->handler != nullptr);
    const tox2::SyscallEntry* container_entry = tox2::findSyscall(343);
    CHECK(container_entry != nullptr && std::string_view(container_entry->name) == "sys_memory_container_get_size");
    CHECK(container_entry != nullptr && container_entry->handler != nullptr);
    CHECK(tox2::findSyscall(999) == nullptr);

    auto ctx = std::make_unique<Tox2PpuContext>();
    ctx->gpr[11] = 300;  // sys_vm_memory_map has no handler
    ctx->gpr[3] = 0x1234;
    tox2Syscall(ctx.get(), nullptr, 0x10000);
    CHECK(ctx->gpr[3] == tox2::kCellOk);
    tox2Syscall(ctx.get(), nullptr, 0x10000);
    ctx->gpr[11] = 999;
    ctx->gpr[3] = 0x1234;
    tox2Syscall(ctx.get(), nullptr, 0x10004);
    CHECK(ctx->gpr[3] == tox2::kCellOk);

    CHECK(countContaining(lines, "tox2: first sc 300 sys_vm_memory_map") == 1);
    CHECK(countContaining(lines, "tox2: first sc 999 ?") == 1);
    CHECK(countContaining(lines, "tox2: default sc 300 sys_vm_memory_map(0x1234") == 1);
    CHECK(countContaining(lines, "tox2: default sc 999 ?(0x1234") == 1);

    const std::string census = tox2::formatCensus();
    CHECK(census.find("census: syscalls 3 calls, 2 distinct, 2 default-handled") != std::string::npos);
    CHECK(census.find("  sc 300 sys_vm_memory_map: 2 [default]") != std::string::npos);
    CHECK(census.find("  sc 999 ?: 1 [default]") != std::string::npos);

    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

TOX2_PPU_FUNC(opdTarget) {
    (void)base;
    ctx->gpr[3] = ctx->gpr[2] + 1;  // proves r2 holds the descriptor's TOC
    ctx->gpr[4] = ctx->lr;
    ctx->gpr[2] = 0;
    ctx->lr = 0x7777;
}

void testHle() {
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetCensus();
    tox2::resetLogOnce();

    const Tox2ImportInfo imports[] = {
            {0x00800000, 0xdeadbeef, "cellTestModule", "cellTestFirst"},
            {0x00800010, 0xcafef00d, "cellTestModule", "cellTestSecond"},
    };
    tox2::installImports(imports, 2);
    CHECK(countContaining(lines, "tox2: hle: 2 imports, 0 with handlers") == 1);

    auto ctx = std::make_unique<Tox2PpuContext>();
    for (int i = 0; i < 10; i++) {
        ctx->gpr[3] = 0x55;
        ctx->gpr[4] = 0x66;
        tox2HleCall(ctx.get(), nullptr, 0);
        CHECK(ctx->gpr[3] == 0);
    }
    CHECK(countContaining(lines, "tox2: default hle cellTestModule::cellTestFirst(0x55, 0x66,") == 8);
    CHECK(countContaining(lines, "tox2: default hle cellTestModule::cellTestFirst(") == 8);
    CHECK(countContaining(lines, "tox2: first hle cellTestModule::cellTestFirst") == 1);

    tox2::setQuietHle(true);
    ctx->gpr[3] = 0x55;
    tox2HleCall(ctx.get(), nullptr, 1);
    tox2::setQuietHle(false);
    CHECK(ctx->gpr[3] == 0);
    CHECK(countContaining(lines, "cellTestSecond(") == 0);
    CHECK(countContaining(lines, "tox2: first hle cellTestModule::cellTestSecond") == 1);

    const std::string census = tox2::formatCensus();
    CHECK(census.find("census: hle 11 calls, 2 distinct, 2 default-handled") != std::string::npos);
    CHECK(census.find("  cellTestModule::cellTestFirst: 10 [default]") != std::string::npos);

    // callGuestOpd: the descriptor holds the entry address and the TOC.
    std::vector<std::uint8_t> memory(0x20000);
    std::uint8_t* base = memory.data();
    const Tox2FuncMapping mappings[] = {{0x10200, opdTarget}};
    tox2RegisterFuncs(mappings, 1);
    tox2Write32(base, 0x18000, 0x10200);
    tox2Write32(base, 0x18004, 0xd0000);
    ctx->gpr[2] = 0x1111;
    ctx->lr = 0x2222;
    tox2::callGuestOpd(ctx.get(), base, 0x18000);
    CHECK(ctx->gpr[3] == 0xd0001);
    CHECK(ctx->gpr[4] == 0);
    CHECK(ctx->gpr[2] == 0x1111);
    CHECK(ctx->lr == 0x2222);

    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

void testHleGuestExports() {
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetCensus();
    tox2::resetLogOnce();
    CHECK(tox2::process().memory.reserve());
    CHECK(tox2::process().memory.commit(0x500000, 0x2000));
    std::uint8_t* base = tox2::process().memory.base();

    const Tox2FuncMapping mappings[] = {{0x10200, opdTarget}};
    tox2RegisterFuncs(mappings, 1);
    tox2Write32(base, 0x500000, 0x10200);
    tox2Write32(base, 0x500004, 0xd0000);

    const Tox2ExportInfo exports[] = {{0x500000, 0xaaaa0001, "cellTestSpurs"}};
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0xaaaa0001, "cellTestSpurs", "cellTestBoundCall"},
            {0x00800010, 0xbbbb0002, "cellTestSpurs", "cellTestOther"},
    };
    const tox2::GuestExports guest[] = {{"img", exports, 1}};
    CHECK(tox2::guestExportConflicts(guest).empty());
    tox2::installImports(imports, 2, guest);
    CHECK(countContaining(lines, "tox2: hle: 2 imports, 0 with handlers, 1 bound to img") == 1);

    auto ctx = std::make_unique<Tox2PpuContext>();
    ctx->gpr[2] = 0x1111;
    ctx->lr = 0x2222;
    tox2HleCall(ctx.get(), base, 0);
    CHECK(ctx->gpr[3] == 0xd0001);  // r2 held the descriptor's TOC
    CHECK(ctx->gpr[4] == 0x2222);   // lr was left as the caller's
    CHECK(ctx->lr == 0x7777);       // and nothing restored what the callee changed
    CHECK(ctx->jump_site == 0x00800000);
    CHECK(ctx->jump_target == 0x10200);
    tox2HleCall(ctx.get(), base, 0);
    CHECK(countContaining(lines, "tox2: first hle cellTestSpurs::cellTestBoundCall") == 1);
    CHECK(countContaining(lines, "tox2: default hle cellTestSpurs::cellTestBoundCall") == 0);

    ctx->gpr[3] = 0x55;
    tox2HleCall(ctx.get(), base, 1);  // not exported by the guest: default handler
    CHECK(ctx->gpr[3] == 0);
    CHECK(countContaining(lines, "tox2: default hle cellTestSpurs::cellTestOther(0x55") == 1);

    const std::string census = tox2::formatCensus();
    CHECK(census.find("census: hle 3 calls, 2 distinct, 1 default-handled") != std::string::npos);
    CHECK(census.find("  cellTestSpurs::cellTestBoundCall: 2 [img]\n") != std::string::npos);
    CHECK(census.find("  cellTestSpurs::cellTestOther: 1 [default]\n") != std::string::npos);

    // Installing again without the export set binds nothing.
    tox2::installImports(imports, 2);
    ctx->gpr[3] = 0x55;
    tox2HleCall(ctx.get(), base, 0);
    CHECK(ctx->gpr[3] == 0);
    CHECK(countContaining(lines, "tox2: default hle cellTestSpurs::cellTestBoundCall(0x55") == 1);

    tox2::process().memory.decommit(0x500000, 0x2000);
    tox2RegisterFuncs(nullptr, 0);
    tox2::installImports(nullptr, 0);
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

void testModuleImports() {
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetCensus();
    tox2::resetLogOnce();

    constexpr const char* kModule = "sysPrxForUser";
    const Tox2ImportInfo main_imports[] = {{0x00800000, 0x45fe2fce, kModule, "_sys_spu_printf_initialize"}};
    // Index 0 of this table is a different import than index 0 of the main table. The third entry has no
    // generated name; its handler's name is used instead.
    const Tox2ImportInfo module_imports[] = {
            {0x001e0000, 0x8461e528, kModule, "sys_time_get_system_time"},
            {0x001e0020, 0x12345678, "cellTestLib", "cellTestUnknown"},
            {0x001e0040, 0xdd3b27ac, kModule, nullptr},
    };
    const Tox2ImportInfo other_imports[] = {{0x001f0000, 0xdd3b27ac, kModule, "_sys_spu_printf_finalize"}};
    tox2::installImports(main_imports, 1);
    tox2::installModuleImports("img", module_imports, 3);
    tox2::installModuleImports("other", other_imports, 1);
    CHECK(countContaining(lines, "tox2: hle: img: 3 imports, 2 with handlers") == 1);
    CHECK(countContaining(lines, "tox2: hle: other: 1 imports, 1 with handlers") == 1);

    auto ctx = std::make_unique<Tox2PpuContext>();
    ctx->gpr[3] = 0x55;
    tox2HleCall(ctx.get(), nullptr, 0);
    CHECK(ctx->gpr[3] == 0);  // returnOk
    tox2HleCallImport(ctx.get(), nullptr, &module_imports[0]);
    tox2HleCallImport(ctx.get(), nullptr, &other_imports[0]);
    tox2HleCallImport(ctx.get(), nullptr, &module_imports[2]);

    ctx->gpr[3] = 0x55;
    tox2HleCallImport(ctx.get(), nullptr, &module_imports[1]);
    CHECK(ctx->gpr[3] == 0);
    CHECK(countContaining(lines, "tox2: default hle cellTestLib::cellTestUnknown(0x55") == 1);
    CHECK(countContaining(lines, "tox2: first hle sysPrxForUser::_sys_spu_printf_finalize") == 1);

    const std::string census = tox2::formatCensus();
    CHECK(census.find("census: hle 5 calls, 4 distinct, 1 default-handled") != std::string::npos);
    CHECK(census.find("  sysPrxForUser::_sys_spu_printf_initialize: 1 [impl]\n") != std::string::npos);
    CHECK(census.find("  sysPrxForUser::sys_time_get_system_time: 1 [impl]\n") != std::string::npos);
    CHECK(census.find("  sysPrxForUser::_sys_spu_printf_finalize: 2 [impl]\n") != std::string::npos);
    CHECK(census.find("  cellTestLib::cellTestUnknown: 1 [default]\n") != std::string::npos);

    // Installing the same image again installs the new, shorter table; only its log line is checked here.
    lines.clear();
    tox2::installModuleImports("img", module_imports, 1);
    CHECK(countContaining(lines, "tox2: hle: img: 1 imports, 1 with handlers") == 1);

    // Installing the main table again must still work after module tables were installed.
    tox2::installImports(nullptr, 0);
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

void testGuestExportConflicts() {
    // sys_time_get_system_time and sys_lwmutex_create are registered handlers of sysPrxForUser.
    const Tox2ExportInfo first[] = {
            {0x1000, 0x8461e528, "sysPrxForUser"},
            {0x1008, 0x8461e528, "cellOther"},      // same FNID, another module
            {0x1010, 0x12345678, "sysPrxForUser"},  // no handler registered
    };
    const Tox2ExportInfo second[] = {
            {0x2000, 0x2f85c0ef, "sysPrxForUser"},
            {0x2008, 0x8461e528, "sysPrxForUser"},  // exported by both images: reported once
    };
    const tox2::GuestExports both[] = {{"a", first, 3}, {"b", second, 2}};
    const std::vector<std::string> conflicts = tox2::guestExportConflicts(both);
    CHECK(conflicts.size() == 2);
    CHECK(conflicts.size() == 2 && conflicts[0] == "sysPrxForUser::sys_lwmutex_create (fnid 0x2f85c0ef)");
    CHECK(conflicts.size() == 2 && conflicts[1] == "sysPrxForUser::sys_time_get_system_time (fnid 0x8461e528)");

    const tox2::GuestExports disjoint[] = {{"a", first + 1, 2}};
    CHECK(tox2::guestExportConflicts(disjoint).empty());
    CHECK(tox2::guestExportConflicts({}).empty());
}

void testHleOutPointerHints() {
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetCensus();
    tox2::resetLogOnce();
    CHECK(tox2::process().memory.reserve());

    // 0x500000 and 0x501000 are committed data, 0x700000 is not, 0x10200 is committed code.
    CHECK(tox2::process().memory.commit(0x500000, 0x2000));
    CHECK(tox2::process().memory.commit(0x10000, 0x1000));
    const Tox2FuncMapping mappings[] = {{0x10200, opdTarget}};
    tox2RegisterFuncs(mappings, 1);

    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x11111111, "cellHintModule", "cellHintFirst"},
            {0x00800010, 0x22222222, "cellHintModule", "cellHintSecond"},
            {0x00800020, 0x33333333, "cellHintModule", "cellHintThird"},
    };
    tox2::installImports(imports, 3);

    auto ctx = std::make_unique<Tox2PpuContext>();
    ctx->gpr[3] = 0;                          // null
    ctx->gpr[4] = 0x500000;                   // candidate
    ctx->gpr[5] = 0x100000000ull | 0x500000;  // upper bits set
    ctx->gpr[6] = 0x700000;                   // uncommitted
    ctx->gpr[7] = 0x10200;                    // code address
    ctx->gpr[8] = 0x501000;                   // candidate
    ctx->gpr[9] = 0x20;                       // small integer, not committed
    tox2::setQuietHle(true);                  // the hint is printed anyway
    tox2HleCall(ctx.get(), nullptr, 0);
    tox2HleCall(ctx.get(), nullptr, 0);  // only the first default call scans
    tox2::setQuietHle(false);
    CHECK(ctx->gpr[3] == tox2::kCellOk);
    CHECK(countContaining(lines, "possible out-pointer") == 2);
    CHECK(countContaining(lines,
                          "tox2: default hle cellHintModule::cellHintFirst: possible out-pointer "
                          "r4=0x00500000 (main)") == 1);
    CHECK(countContaining(lines, "r8=0x00501000 (main)") == 1);

    ctx->gpr[3] = 0x500000;
    for (int reg = 4; reg <= 10; reg++) ctx->gpr[reg] = 0;
    tox2HleCall(ctx.get(), nullptr, 1);
    tox2HleCall(ctx.get(), nullptr, 2);  // no candidates: no note
    CHECK(countContaining(lines, "cellHintSecond: possible out-pointer r3=0x00500000 (main)") == 1);

    const std::string census = tox2::formatCensus();
    CHECK(census.find("  cellHintModule::cellHintFirst: 2 [default] ptr r4,r8\n") != std::string::npos);
    CHECK(census.find("  cellHintModule::cellHintSecond: 1 [default] ptr r3\n") != std::string::npos);
    CHECK(census.find("  cellHintModule::cellHintThird: 1 [default]\n") != std::string::npos);

    tox2::process().memory.decommit(0x500000, 0x2000);
    tox2::process().memory.decommit(0x10000, 0x1000);
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

TOX2_PPU_FUNC(hostFunctionA) {
    (void)base;
    ctx->gpr[3] = ctx->gpr[3] + 100;
}

TOX2_PPU_FUNC(hostFunctionB) {
    (void)base;
    ctx->gpr[3] = 0xB0B;
}

struct CallSnapshot {
    std::uint64_t r1 = 0, r2 = 0, lr = 0, back_chain = 0;
};
CallSnapshot g_call_snapshot;

TOX2_PPU_FUNC(sumTarget) {
    g_call_snapshot = {ctx->gpr[1], ctx->gpr[2], ctx->lr, tox2Read64(base, static_cast<std::uint32_t>(ctx->gpr[1]))};
    ctx->gpr[3] = ctx->gpr[3] + ctx->gpr[4] + ctx->gpr[5];
    ctx->gpr[1] = 0;  // callee-side clobbers must not leak to the caller
    ctx->gpr[2] = 0;
    ctx->lr = 0x7777;
}

void testHostFunctionsAndCallGuestFunction() {
    tox2::resetHostFunctions();
    CHECK(tox2::process().memory.reserve());
    std::uint8_t* base = tox2::process().memory.base();
    CHECK(tox2::process().memory.commit(0x500000, 0x2000));
    CHECK(tox2::process().memory.commit(0x10000, 0x10000));
    const Tox2FuncMapping mappings[] = {{0x10200, sumTarget}};
    tox2RegisterFuncs(mappings, 1);

    CHECK(tox2Lookup(tox2::kHostFunctionBase) == nullptr);  // nothing registered yet
    const std::uint32_t opd_a = tox2::registerHostFunction("hostFunctionA", hostFunctionA);
    const std::uint32_t opd_b = tox2::registerHostFunction("hostFunctionB", hostFunctionB);
    CHECK(opd_a != 0 && opd_b == opd_a + 8);
    CHECK(tox2::process().memory.isCommitted(opd_a, 16));
    CHECK(tox2Read32(base, opd_a) == tox2::kHostFunctionBase);
    CHECK(tox2Read32(base, opd_a + 4) == 0);
    CHECK(tox2Read32(base, opd_b) == tox2::kHostFunctionBase + 4);

    CHECK(tox2Lookup(tox2::kHostFunctionBase) == hostFunctionA);
    CHECK(tox2Lookup(tox2::kHostFunctionBase + 4) == hostFunctionB);
    CHECK(tox2Lookup(tox2::kHostFunctionBase + 8) == nullptr);  // not registered
    CHECK(tox2Lookup(tox2::kHostFunctionBase + 2) == nullptr);  // misaligned
    CHECK(tox2Lookup(tox2::kHostFunctionBase - 4) == nullptr);
    CHECK(tox2Lookup(0xFFFFFFFCu) == nullptr);
    CHECK(tox2Lookup(0x10200) == sumTarget);  // the code table still resolves

    // Guest code reaches a host function through the OPD's entry word.
    auto ctx = std::make_unique<Tox2PpuContext>();
    ctx->gpr[3] = 5;
    ctx->jump_target = tox2Read32(base, opd_a);
    tox2Dispatch(ctx.get(), base);
    CHECK(ctx->gpr[3] == 105);
    ctx->jump_target = tox2Read32(base, opd_b);
    tox2Dispatch(ctx.get(), base);
    CHECK(ctx->gpr[3] == 0xB0B);

    // callGuestFunction: arguments in r3..r5, a lowered and aligned r1 with a back chain, and
    // r1/r2/lr restored afterwards.
    tox2Write32(base, 0x18000, 0x10200);
    tox2Write32(base, 0x18004, 0xd0000);
    ctx->gpr[1] = 0x500808;  // deliberately not 16-aligned
    ctx->gpr[2] = 0x1111;
    ctx->lr = 0x2222;
    const std::uint64_t result = tox2::callGuestFunction(ctx.get(), base, 0x18000, {10, 20, 30});
    CHECK(result == 60);
    CHECK(g_call_snapshot.r1 == 0x500800 - 0x70);
    CHECK(g_call_snapshot.r2 == 0xd0000);
    CHECK(g_call_snapshot.lr == 0);
    CHECK(g_call_snapshot.back_chain == 0x500808);
    CHECK(ctx->gpr[1] == 0x500808 && ctx->gpr[2] == 0x1111 && ctx->lr == 0x2222);

    CHECK(tox2::callGuestFunction(ctx.get(), base, opd_a, {7}) == 107);  // host function OPD
    CHECK(ctx->gpr[1] == 0x500808);

    CHECK(tox2::argU32(ctx.get(), 0) == 107);
    ctx->gpr[5] = 0x1ffffffffull;
    CHECK(tox2::argU32(ctx.get(), 2) == 0xffffffffu);

    std::memcpy(base + 0x500100, "hello\0world", 12);
    CHECK(tox2::readGuestString(base, 0x500100, 64) == "hello");
    CHECK(tox2::readGuestString(base, 0x500100, 3) == "hel");
    CHECK(tox2::readGuestString(base, 0x500100, 0).empty());
    // A string that reaches the end of the committed range stops there instead of reading on.
    std::memcpy(base + 0x501ff8, "abcdefgh", 8);
    CHECK(!tox2::process().memory.isCommitted(0x502000, 1));
    CHECK(tox2::readGuestString(base, 0x501ff8, 64) == "abcdefgh");
    CHECK(tox2::readGuestString(base, 0x502000, 64).empty());

    tox2RegisterFuncs(nullptr, 0);
    tox2::resetHostFunctions();
    tox2::process().memory.decommit(0x500000, 0x2000);
    tox2::process().memory.decommit(0x10000, 0x10000);
}

// Issues a syscall through the dispatcher and returns r3.
std::uint64_t syscall(std::uint32_t number, std::uint64_t r3 = 0, std::uint64_t r4 = 0, std::uint64_t r5 = 0,
                      std::uint64_t r6 = 0) {
    auto ctx = std::make_unique<Tox2PpuContext>();
    ctx->gpr[11] = number;
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    ctx->gpr[5] = r5;
    ctx->gpr[6] = r6;
    tox2Syscall(ctx.get(), tox2::process().memory.base(), 0x10000);
    return ctx->gpr[3];
}

// lv2 errors reach the guest as sign-extended s32 values.
constexpr std::uint64_t err(std::uint32_t code) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(code)));
}

void testStartupSyscalls() {
    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);

    // User memory size bookkeeping.
    const std::uint32_t used_before = p.user_memory_used;
    CHECK(syscall(352, scratch) == 0);
    CHECK(tox2Read32(base, scratch) == p.user_memory_total);
    CHECK(tox2Read32(base, scratch + 4) == p.user_memory_total - used_before);

    // Allocation: 1 MiB pages by default and for 0x400, 64 KiB pages for 0x200.
    CHECK(syscall(348, 0x100000, 0x400, scratch) == 0);
    const std::uint32_t a = tox2Read32(base, scratch);
    CHECK(a >= tox2::kUser1mRegion.begin && a < tox2::kUser1mRegion.end && a % 0x100000 == 0);
    CHECK(p.user_memory_used == used_before + 0x100000);
    CHECK(syscall(352, scratch) == 0);
    CHECK(tox2Read32(base, scratch + 4) == p.user_memory_total - used_before - 0x100000);
    CHECK(syscall(348, 0x200000, 0, scratch) == 0);
    const std::uint32_t b = tox2Read32(base, scratch);
    CHECK(b >= tox2::kUser1mRegion.begin && b != a);
    CHECK(syscall(348, 0x30000, 0x200, scratch) == 0);
    const std::uint32_t c = tox2Read32(base, scratch);
    CHECK(c >= tox2::kUser64kRegion.begin && c < tox2::kUser64kRegion.end && c % 0x10000 == 0);
    tox2Write32(base, c, 0xdeadbeef);  // committed and writable

    // Error cases leave the bookkeeping alone.
    const std::uint32_t used = p.user_memory_used;
    CHECK(syscall(348, 0, 0x400, scratch) == err(tox2::kCellEalign));
    CHECK(syscall(348, 0x100000, 0x123, scratch) == err(tox2::kCellEinval));
    CHECK(syscall(348, 0x10000, 0x400, scratch) == err(tox2::kCellEalign));
    CHECK(syscall(348, 0x180000, 0, scratch) == err(tox2::kCellEalign));
    CHECK(syscall(348, 0xF000000, 0x400, scratch) == err(tox2::kCellEnomem));  // above the total
    CHECK(syscall(348, 0x100000, 0x400, 0) == err(tox2::kCellEfault));
    CHECK(p.user_memory_used == used);

    // Free: only allocation starts are accepted, once.
    CHECK(syscall(349, a + 0x10000) == err(tox2::kCellEinval));
    CHECK(syscall(349, 0x1234) == err(tox2::kCellEinval));
    CHECK(syscall(349, a) == 0);
    CHECK(syscall(349, a) == err(tox2::kCellEinval));
    CHECK(syscall(349, b) == 0);
    CHECK(syscall(349, c) == 0);
    CHECK(p.user_memory_used == used_before);
    CHECK(syscall(348, 0x100000, 0x400, scratch) == 0 && tox2Read32(base, scratch) == a);  // reuse
    CHECK(syscall(349, a) == 0);

    // tty: writes to channels outside 0..15 are dropped but still reported; reads fail.
    CHECK(syscall(403, 16, scratch + 0x100, 5, scratch) == 0);
    CHECK(tox2Read32(base, scratch) == 5);
    CHECK(syscall(403, 0, scratch + 0x100, 0, scratch) == 0);
    CHECK(tox2Read32(base, scratch) == 0);
    CHECK(syscall(403, 0, scratch + 0x100, 5, 0) == err(tox2::kCellEfault));
    CHECK(syscall(402, 0, scratch + 0x100, 4, scratch) == err(tox2::kCellEio));

    // Time base and sleeps (the sleeps wait on the guest lock, which the caller holds).
    CHECK(syscall(147) == tox2::kTimebaseFrequency);
    {
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        const auto start = std::chrono::steady_clock::now();
        CHECK(syscall(141, 20000) == 0);
        CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(15));
        CHECK(syscall(141, 0) == 0);  // yield
        CHECK(syscall(142, 0) == 0);
    }

    p.main.free(scratch);
    tox2::setLogSink({});
    tox2::resetCensus();
}

void testProcessAndPageSyscalls() {
    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::resetSyncObjects();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);

    // sys_process_getpid: the id is a plain value.
    CHECK(syscall(1) == 1);
    CHECK(tox2::kProcessId == 1);

    // sys_process_get_sdk_version: the version that was set, written as a big-endian u32.
    tox2::setProcessSdkVersion(0x00330000);
    tox2Write32(base, scratch, 0xeeeeeeee);
    CHECK(syscall(25, 1, scratch) == 0 && tox2Read32(base, scratch) == 0x00330000);
    CHECK(syscall(25, 0x1234, scratch) == 0);  // the pid is not looked at
    CHECK(syscall(25, 1, 0) == err(tox2::kCellEfault));
    CHECK(syscall(25, 1, 0x0f000000) == err(tox2::kCellEfault));  // not committed
    tox2::setProcessSdkVersion(0x00475001);
    CHECK(syscall(25, 1, scratch) == 0 && tox2Read32(base, scratch) == 0x00475001);

    // Memory from the three process ranges qualifies for a lock line reservation, nothing else does.
    CHECK(syscall(348, 0x100000, 0x400, scratch + 0x100) == 0);
    const std::uint32_t user1m = tox2Read32(base, scratch + 0x100);
    CHECK(syscall(348, 0x10000, 0x200, scratch + 0x100) == 0);
    const std::uint32_t user64k = tox2Read32(base, scratch + 0x100);
    const std::uint32_t stack = p.stack.allocate(0x1000, 0x1000);
    CHECK(stack != 0);
    CHECK(syscall(14, scratch, 2) == 0);
    CHECK(syscall(14, user1m, 0) == 0);
    CHECK(syscall(14, user64k, 7) == 0);
    CHECK(syscall(14, stack, 2) == err(tox2::kCellEperm));
    CHECK(syscall(14, 0, 2) == err(tox2::kCellEperm));
    CHECK(syscall(14, 0xC0000000, 2) == err(tox2::kCellEperm));  // RSX local memory

    // sys_memory_get_page_attribute: committed memory only, page size by range.
    const std::uint32_t attr = scratch + 0x200;
    auto pageAttr = [&](std::uint32_t addr) {
        for (std::uint32_t i = 0; i < 24; i++) tox2Write8(base, attr + i, 0xee);
        return syscall(351, addr, attr);
    };
    CHECK(pageAttr(user1m + 0x1234) == 0);
    CHECK(tox2Read64(base, attr) == 0x40000 && tox2Read64(base, attr + 8) == 0xF);
    CHECK(tox2Read32(base, attr + 16) == 0x100000 && tox2Read32(base, attr + 20) == 0);
    CHECK(pageAttr(user64k) == 0 && tox2Read32(base, attr + 16) == 0x10000);
    CHECK(pageAttr(scratch) == 0 && tox2Read32(base, attr + 16) == 0x10000);
    CHECK(pageAttr(stack + 8) == 0 && tox2Read32(base, attr + 16) == 0x1000);
    CHECK(pageAttr(0x0f000000) == err(tox2::kCellEinval));  // not committed
    CHECK(tox2Read8(base, attr) == 0xee);
    CHECK(syscall(351, scratch, 0) == err(tox2::kCellEfault));
    CHECK(syscall(351, user1m, 0x0f000000) == err(tox2::kCellEfault));

    // sys_event_queue_tryreceive: oldest first, never waits, only as many as asked for.
    const std::uint32_t queue_attr = scratch + 0x300, id_out = scratch + 0x320, events = scratch + 0x400,
                        number = scratch + 0x380;
    tox2Write32(base, queue_attr, 1);
    tox2Write32(base, queue_attr + 4, 1);
    CHECK(syscall(128, id_out, queue_attr, 0, 8) == 0);
    const std::uint32_t queue = tox2Read32(base, id_out);
    auto tryReceive = [&](std::uint32_t id, std::uint32_t array, std::int32_t size, std::uint32_t count_out) {
        tox2Write32(base, number, 0xeeeeeeee);
        return syscall(131, id, array, static_cast<std::uint32_t>(size), count_out);
    };
    CHECK(tryReceive(queue, events, 4, number) == 0 && tox2Read32(base, number) == 0);  // empty queue
    for (std::uint64_t i = 1; i <= 3; i++) {
        CHECK(tox2::sendEventToQueue(queue, 0x100 + i, 0x200 + i, 0x300 + i, 0x400 + i) == 0);
    }
    for (std::uint32_t i = 0; i < 0x80; i++) tox2Write8(base, events + i, 0xee);
    CHECK(tryReceive(queue, events, 0, number) == 0 && tox2Read32(base, number) == 0);  // size 0 takes nothing
    CHECK(tox2Read8(base, events) == 0xee);
    CHECK(tryReceive(queue, events, 1, number) == 0 && tox2Read32(base, number) == 1);
    CHECK(tox2Read64(base, events) == 0x101 && tox2Read64(base, events + 8) == 0x201 &&
          tox2Read64(base, events + 16) == 0x301 && tox2Read64(base, events + 24) == 0x401);
    CHECK(tox2Read8(base, events + 32) == 0xee);                                        // only one slot was written
    CHECK(tryReceive(queue, events, 8, number) == 0 && tox2Read32(base, number) == 2);  // fewer stored than asked
    CHECK(tox2Read64(base, events) == 0x102 && tox2Read64(base, events + 32) == 0x103 &&
          tox2Read64(base, events + 56) == 0x403);
    CHECK(tryReceive(queue, events, 8, number) == 0 && tox2Read32(base, number) == 0);

    // More stored events than the caller asks for stay in the queue; the queue holds at most its size.
    for (std::uint64_t i = 1; i <= 9; i++) {
        CHECK(tox2::sendEventToQueue(queue, i, 0, 0, 0) == (i <= 8 ? 0u : tox2::kCellEbusy));
    }
    CHECK(tryReceive(queue, events, 5, number) == 0 && tox2Read32(base, number) == 5);
    CHECK(tryReceive(queue, events, -1, number) == 0 && tox2Read32(base, number) == 0);
    CHECK(tryReceive(queue, events, 5, number) == 0 && tox2Read32(base, number) == 3);
    CHECK(tox2Read64(base, events) == 6 && tox2Read64(base, events + 64) == 8);

    // Errors: no such queue, no count pointer, no array while events are taken.
    CHECK(tryReceive(0x8d000fff, events, 1, number) == err(tox2::kCellEsrch));
    CHECK(tox2::sendEventToQueue(queue, 1, 2, 3, 4) == 0);
    CHECK(syscall(131, queue, events, 1, 0) == err(tox2::kCellEfault));
    CHECK(tryReceive(queue, 0, 1, number) == err(tox2::kCellEfault));
    CHECK(tryReceive(queue, 0, 0, number) == 0);  // nothing to copy, so no array is needed
    CHECK(tryReceive(queue, events, 1, number) == 0 && tox2Read32(base, number) == 1);

    const std::string census = tox2::formatCensus();
    CHECK(census.find("sc 1 sys_process_getpid: 1 [impl]") != std::string::npos);
    CHECK(census.find("sc 131 sys_event_queue_tryreceive: ") != std::string::npos);

    CHECK(syscall(349, user1m) == 0 && syscall(349, user64k) == 0);
    p.stack.free(stack);
    p.main.free(scratch);
    tox2::setProcessSdkVersion(0);
    tox2::resetSyncObjects();
    tox2::setLogSink({});
    tox2::resetCensus();
}

void testMemoryContainers() {
    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::resetMemoryContainers();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t cid_out = scratch, info = scratch + 0x10, addr_out = scratch + 0x20;
    constexpr std::uint32_t kInvalidId = 0xFFFFFFFF;
    const std::uint32_t used0 = p.user_memory_used;

    // The size is rounded down to 1 MiB and charged to the default container.
    CHECK(syscall(324, cid_out, 0x5600000 + 0x1234) == 0);
    const std::uint32_t cid = tox2Read32(base, cid_out);
    CHECK(cid == 0x3F000000);
    CHECK(p.user_memory_used == used0 + 0x5600000);
    CHECK(syscall(352, info) == 0);
    CHECK(tox2Read32(base, info + 4) == p.user_memory_total - used0 - 0x5600000);
    CHECK(syscall(343, info, cid) == 0);
    CHECK(tox2Read32(base, info) == 0x5600000 && tox2Read32(base, info + 4) == 0x5600000);

    // Unknown ids, among them 0, which the game passed when no id had been written.
    CHECK(syscall(343, info, 0) == err(tox2::kCellEsrch));
    CHECK(syscall(343, info, kInvalidId) == err(tox2::kCellEsrch));
    CHECK(syscall(343, 0, cid) == err(tox2::kCellEfault));

    // Create errors leave the bookkeeping alone.
    const std::uint32_t used = p.user_memory_used;
    CHECK(syscall(324, cid_out, 0xFFFFF) == err(tox2::kCellEnomem));  // rounds down to zero
    CHECK(syscall(324, cid_out, 0x100000000ull) == err(tox2::kCellEnomem));
    CHECK(syscall(324, cid_out, p.user_memory_total - used + 0x100000) == err(tox2::kCellEnomem));
    CHECK(syscall(324, 0, 0x100000) == err(tox2::kCellEfault));
    CHECK(p.user_memory_used == used);

    // Allocation from the container is charged to it, not to the default container a second time.
    CHECK(syscall(350, 0x200000, cid, 0x400, addr_out) == 0);
    const std::uint32_t a = tox2Read32(base, addr_out);
    CHECK(a >= tox2::kUser1mRegion.begin && a < tox2::kUser1mRegion.end && a % 0x100000 == 0);
    CHECK(syscall(350, 0x30000, cid, 0x200, addr_out) == 0);
    const std::uint32_t b = tox2Read32(base, addr_out);
    CHECK(b >= tox2::kUser64kRegion.begin && b < tox2::kUser64kRegion.end && b % 0x10000 == 0);
    tox2Write32(base, b, 0xdeadbeef);  // committed and writable
    CHECK(p.user_memory_used == used);
    CHECK(syscall(343, info, cid) == 0);
    CHECK(tox2Read32(base, info + 4) == 0x5600000 - 0x200000 - 0x30000);

    // Allocation errors: the checks of sys_memory_allocate first, then the container.
    CHECK(syscall(350, 0, cid, 0x400, addr_out) == err(tox2::kCellEalign));
    CHECK(syscall(350, 0x100000, cid, 0x123, addr_out) == err(tox2::kCellEinval));
    CHECK(syscall(350, 0x10000, cid, 0x400, addr_out) == err(tox2::kCellEalign));
    CHECK(syscall(350, 0x100000, 0, 0x400, addr_out) == err(tox2::kCellEsrch));
    CHECK(syscall(350, 0x100000, kInvalidId, 0x400, addr_out) == err(tox2::kCellEsrch));
    CHECK(syscall(350, 0x5600000, cid, 0x400, addr_out) == err(tox2::kCellEnomem));  // only the rest is free
    CHECK(syscall(350, 0x100000, cid, 0x400, 0) == err(tox2::kCellEfault));
    CHECK(syscall(343, info, cid) == 0);
    CHECK(tox2Read32(base, info + 4) == 0x5600000 - 0x200000 - 0x30000);

    // A container with memory in use cannot be destroyed; freeing a block credits the container.
    CHECK(syscall(325, cid) == err(tox2::kCellEbusy));
    CHECK(syscall(349, a) == 0);
    CHECK(syscall(343, info, cid) == 0);
    CHECK(tox2Read32(base, info + 4) == 0x5600000 - 0x30000);
    CHECK(syscall(349, b) == 0);
    CHECK(syscall(349, b) == err(tox2::kCellEinval));
    CHECK(syscall(343, info, cid) == 0);
    CHECK(tox2Read32(base, info + 4) == 0x5600000);
    CHECK(p.user_memory_used == used);

    // Destroy gives the memory back to the default container; 341 and 342 are the older numbers.
    CHECK(syscall(342, cid) == 0);
    CHECK(p.user_memory_used == used0);
    CHECK(syscall(343, info, cid) == err(tox2::kCellEsrch));
    CHECK(syscall(325, cid) == err(tox2::kCellEsrch));
    CHECK(syscall(341, cid_out, 0x100000) == 0);
    CHECK(tox2Read32(base, cid_out) == cid + 1);  // ids are not reused
    CHECK(syscall(325, tox2Read32(base, cid_out)) == 0);

    // At most 16 containers exist at a time.
    std::uint32_t ids[16];
    for (std::uint32_t& id : ids) {
        CHECK(syscall(324, cid_out, 0x100000) == 0);
        id = tox2Read32(base, cid_out);
    }
    CHECK(syscall(324, cid_out, 0x100000) == err(tox2::kCellEagain));
    CHECK(p.user_memory_used == used0 + 16 * 0x100000);
    for (const std::uint32_t id : ids) CHECK(syscall(325, id) == 0);
    CHECK(p.user_memory_used == used0);

    p.main.free(scratch);
    tox2::resetMemoryContainers();
    tox2::setLogSink({});
    tox2::resetCensus();
}

void testFileSystem() {
    namespace stdfs = std::filesystem;
    tox2::setLogSink([](std::string_view) {});
    tox2::resetLogOnce();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t path_a = scratch;
    const std::uint32_t buf = scratch + 0x2000;  // data buffer
    const std::uint32_t out = scratch + 0x3000;  // fd / nread / pos
    const std::uint32_t sb = scratch + 0x3100;   // CellFsStat
    const std::uint32_t ent = scratch + 0x3200;  // CellFsDirent
    const std::uint32_t big = scratch + 0x4000;

    auto put = [&](std::uint32_t addr, std::string_view text) {
        std::memcpy(base + addr, text.data(), text.size());
        base[addr + text.size()] = 0;
    };
    auto open = [&](std::string_view path, std::uint64_t flags) {
        put(path_a, path);
        return syscall(801, path_a, flags, out);
    };
    auto fdOut = [&] { return tox2Read32(base, out); };
    auto hostFile = [](const stdfs::path& path, std::string_view data) {
        stdfs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << data;
    };
    auto hostRead = [](const stdfs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };

    const stdfs::path root = stdfs::temp_directory_path() / ("tox2_vfs_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    stdfs::remove_all(root, ec);
    const stdfs::path game = root / "game";
    hostFile(game / "b.txt", "0123456789");
    hostFile(game / "PS3_GAME" / "USRDIR" / "a.bin", "usrdir");
    stdfs::create_directories(game / "sub");

    // Resolution rules.
    tox2::configureVfs(game, std::nullopt);
    bool read_only = false;
    const auto resolved = tox2::resolveGuestPath("/dev_bdvd/sub/../b.txt", read_only);
    CHECK(resolved && read_only && stdfs::equivalent(*resolved, game / "b.txt"));
    CHECK(!tox2::resolveGuestPath("/dev_bdvd/../b.txt", read_only));
    CHECK(!tox2::resolveGuestPath("/dev_bdvd/sub/../../x", read_only));
    CHECK(!tox2::resolveGuestPath("/dev_bdvd/a:b", read_only));
    CHECK(!tox2::resolveGuestPath("/dev_bdvd/NUL.txt", read_only));
    CHECK(!tox2::resolveGuestPath("/dev_hdd0/x", read_only));  // no hdd directory configured
    CHECK(!tox2::resolveGuestPath("/dev_usb000/x", read_only));
    CHECK(!tox2::resolveGuestPath("relative", read_only));

    // open/read/lseek/close.
    CHECK(open("/dev_bdvd/b.txt", 0) == 0);
    const std::uint32_t fd = fdOut();
    CHECK(fd == 3);
    CHECK(syscall(802, fd, buf, 4, out) == 0 && tox2Read64(base, out) == 4);
    CHECK(std::memcmp(base + buf, "0123", 4) == 0);
    CHECK(syscall(818, fd, 2, 0, out) == 0 && tox2Read64(base, out) == 2);
    CHECK(syscall(802, fd, buf, 3, out) == 0 && tox2Read64(base, out) == 3);
    CHECK(std::memcmp(base + buf, "234", 3) == 0);
    CHECK(syscall(818, fd, static_cast<std::uint64_t>(-2), 2, out) == 0 && tox2Read64(base, out) == 8);
    CHECK(syscall(802, fd, buf, 100, out) == 0 && tox2Read64(base, out) == 2);  // short read at the end
    CHECK(syscall(802, fd, buf, 100, out) == 0 && tox2Read64(base, out) == 0);
    CHECK(syscall(818, fd, 0, 3, out) == err(tox2::kCellEinval));
    CHECK(syscall(818, fd, static_cast<std::uint64_t>(-100), 0, out) == err(tox2::kCellEinval));
    CHECK(syscall(802, fd, 0, 1, out) == err(tox2::kCellEfault));
    CHECK(syscall(802, fd, buf, 1, 0) == err(tox2::kCellEfault));
    CHECK(syscall(803, fd, buf, 1, out) == err(tox2::kCellEbadf));  // opened read-only
    // fstat reports a read-only regular file on the disc.
    CHECK(syscall(809, fd, sb) == 0);
    CHECK(tox2Read32(base, sb) == 0100444 && tox2Read32(base, sb + 4) == 0xFFFFFFFFu);
    CHECK(tox2Read64(base, sb + 36) == 10 && tox2Read64(base, sb + 44) == 2048);
    CHECK(syscall(804, fd) == 0);
    CHECK(syscall(804, fd) == err(tox2::kCellEbadf));
    CHECK(syscall(802, fd, buf, 1, out) == err(tox2::kCellEbadf) && tox2Read64(base, out) == 0);

    // Read-only enforcement on the disc.
    CHECK(open("/dev_bdvd/b.txt", 1) == err(tox2::kCellEperm));
    CHECK(open("/dev_bdvd/b.txt", 02) == err(tox2::kCellEperm));
    CHECK(open("/dev_bdvd/new.bin", 0101) == err(tox2::kCellEperm));
    CHECK(open("/dev_bdvd/new.bin", 0) == err(tox2::kCellEnoent));
    CHECK(open("/dev_bdvd/b.txt", 03) == err(tox2::kCellEinval));
    put(path_a, "/dev_bdvd/x");
    CHECK(syscall(811, path_a, 0777) == err(tox2::kCellErofs));
    put(path_a, "/dev_bdvd/b.txt");
    CHECK(syscall(814, path_a) == err(tox2::kCellErofs));
    CHECK(stdfs::exists(game / "b.txt"));

    // Path errors.
    CHECK(open("/dev_bdvd/../b.txt", 0) == err(tox2::kCellEnoent));
    CHECK(open("b.txt", 0) == err(tox2::kCellEnoent));
    CHECK(open("/dev_usb000/b.txt", 0) == err(tox2::kCellEnotmounted));
    CHECK(open("/dev_bdvd", 0) == err(tox2::kCellEisdir));
    CHECK(open("/", 0) == err(tox2::kCellEisdir));
    CHECK(open("/dev_bdvd/b.txt/x", 0) == err(tox2::kCellEnotdir));
    CHECK(syscall(801, 0, 0, out) == err(tox2::kCellEfault));
    CHECK(!p.memory.isCommitted(0x0D000000, 1));
    CHECK(syscall(801, 0x0D000000, 0, out) == err(tox2::kCellEfault));
    std::memset(base + big, 'a', 1100);
    base[big] = '/';
    base[big + 1100] = 0;
    CHECK(syscall(801, big, 0, out) == err(tox2::kCellEnametoolong));
    CHECK(open("/dev_hdd0/x", 0) == err(tox2::kCellEnoent));  // not configured
    CHECK(open("/app_home/a.bin", 0) == 0);
    const std::uint32_t app_fd = fdOut();
    CHECK(syscall(802, app_fd, buf, 6, out) == 0 && std::memcmp(base + buf, "usrdir", 6) == 0);
    CHECK(syscall(804, app_fd) == 0);

    // stat.
    put(path_a, "/dev_bdvd/b.txt");
    CHECK(syscall(808, path_a, sb) == 0 && tox2Read32(base, sb) == 0100444);
    CHECK(tox2Read64(base, sb + 36) == 10);
    put(path_a, "/dev_bdvd/sub");
    CHECK(syscall(808, path_a, sb) == 0 && tox2Read32(base, sb) == 040555);
    CHECK(tox2Read64(base, sb + 36) == 2048);  // a directory reports its block size
    put(path_a, "/dev_bdvd/b.txt/");
    CHECK(syscall(808, path_a, sb) == err(tox2::kCellEnotdir));
    put(path_a, "/dev_bdvd/missing");
    CHECK(syscall(808, path_a, sb) == err(tox2::kCellEnoent));
    put(path_a, "/");
    CHECK(syscall(808, path_a, sb) == 0 && tox2Read32(base, sb) == 040711);
    put(path_a, "/dev_usb000");
    CHECK(syscall(808, path_a, sb) == err(tox2::kCellEnotmounted));

    // opendir/readdir: ".", "..", then names in byte order; at the end nread is 0 and the entry is
    // zeroed.
    put(path_a, "/dev_bdvd");
    CHECK(syscall(805, path_a, out) == 0);
    const std::uint32_t dir = fdOut();
    std::vector<std::string> names;
    std::vector<int> types;
    for (int i = 0; i < 10; i++) {
        std::memset(base + ent, 0xEE, 258);
        CHECK(syscall(806, dir, ent, out + 8) == 0);
        if (tox2Read64(base, out + 8) == 0) {
            break;
        }
        CHECK(tox2Read64(base, out + 8) == 258);
        names.emplace_back(reinterpret_cast<const char*>(base + ent + 2), base[ent + 1]);
        types.push_back(base[ent]);
    }
    CHECK((names == std::vector<std::string>{".", "..", "PS3_GAME", "b.txt", "sub"}));
    CHECK((types == std::vector<int>{1, 1, 1, 2, 1}));
    bool zeroed = true;
    for (int i = 0; i < 258; i++) {
        zeroed = zeroed && base[ent + i] == 0;
    }
    CHECK(zeroed);
    CHECK(syscall(806, dir, ent, out + 8) == 0 && tox2Read64(base, out + 8) == 0);  // stays at the end
    CHECK(syscall(804, dir) == err(tox2::kCellEbadf));                              // a directory is not a file
    CHECK(syscall(807, dir) == 0);
    CHECK(syscall(807, dir) == err(tox2::kCellEbadf));
    CHECK(syscall(806, dir, ent, out + 8) == err(tox2::kCellEbadf));
    put(path_a, "/dev_bdvd/b.txt");
    CHECK(syscall(805, path_a, out) == err(tox2::kCellEnotdir));
    put(path_a, "/dev_bdvd/missing");
    CHECK(syscall(805, path_a, out) == err(tox2::kCellEnoent));
    put(path_a, "/");
    CHECK(syscall(805, path_a, out) == 0);  // the virtual root lists the mounted devices
    CHECK(syscall(807, fdOut()) == 0);

    // Writable device.
    const stdfs::path hdd = root / "hdd";
    tox2::configureVfs(game, hdd);
    CHECK(stdfs::is_directory(hdd / "dev_hdd0") && stdfs::is_directory(hdd / "dev_hdd1"));
    CHECK(open("/dev_hdd0/out.bin", 0101) == err(tox2::kCellEacces));  // direct child of /dev_hdd0
    CHECK(stdfs::is_directory(hdd / "dev_hdd0" / "game") && stdfs::is_directory(hdd / "dev_hdd0" / "tmp"));
    put(path_a, "/dev_hdd0/game");
    CHECK(syscall(811, path_a, 0777) == err(tox2::kCellEexist));
    put(path_a, "/dev_hdd0/newdir");
    CHECK(syscall(811, path_a, 0777) == err(tox2::kCellEacces));  // games cannot add to /dev_hdd0
    put(path_a, "/dev_hdd0/game/NEWGAME");
    CHECK(syscall(811, path_a, 0777) == err(tox2::kCellEacces));
    put(path_a, "/dev_hdd0/tmp/data");
    CHECK(syscall(811, path_a, 0777) == 0);
    CHECK(syscall(811, path_a, 0777) == err(tox2::kCellEexist));
    put(path_a, "/dev_hdd0/tmp/nope/x");
    CHECK(syscall(811, path_a, 0777) == err(tox2::kCellEnoent));

    CHECK(open("/dev_hdd0/tmp/data/out.bin", 01101) == 0);  // WRONLY | CREAT | TRUNC
    const std::uint32_t wfd = fdOut();
    put(buf, "hello");
    CHECK(syscall(803, wfd, buf, 5, out) == 0 && tox2Read64(base, out) == 5);
    CHECK(syscall(802, wfd, buf, 1, out) == err(tox2::kCellEbadf));  // write-only
    CHECK(syscall(804, wfd) == 0);
    CHECK(hostRead(hdd / "dev_hdd0" / "tmp" / "data" / "out.bin") == "hello");
    CHECK(open("/dev_hdd0/tmp/data/out.bin", 0201 | 0100) == err(tox2::kCellEexist));  // CREAT | EXCL
    CHECK(open("/dev_hdd0/tmp/data/out.bin", 02001) == 0);                             // WRONLY | APPEND
    const std::uint32_t afd = fdOut();
    put(buf, "!!");
    CHECK(syscall(803, afd, buf, 2, out) == 0 && tox2Read64(base, out) == 2);
    CHECK(syscall(804, afd) == 0);
    CHECK(hostRead(hdd / "dev_hdd0" / "tmp" / "data" / "out.bin") == "hello!!");
    CHECK(open("/dev_hdd0/tmp/data/out.bin", 02) == 0);  // RDWR keeps the contents
    const std::uint32_t rfd = fdOut();
    CHECK(syscall(818, rfd, 1, 0, out) == 0);
    put(buf, "E");
    CHECK(syscall(803, rfd, buf, 1, out) == 0);
    CHECK(syscall(818, rfd, 0, 0, out) == 0 && syscall(802, rfd, buf + 0x100, 16, out) == 0);
    CHECK(tox2Read64(base, out) == 7 && std::memcmp(base + buf + 0x100, "hEllo!!", 7) == 0);
    CHECK(syscall(809, rfd, sb) == 0 && tox2Read32(base, sb) == 0100666 && tox2Read64(base, sb + 36) == 7);
    CHECK(syscall(804, rfd) == 0);
    CHECK(open("/dev_hdd0/tmp/data/out.bin", 01000) == 0);  // RDONLY | TRUNC empties it
    CHECK(syscall(804, fdOut()) == 0);
    CHECK(hostRead(hdd / "dev_hdd0" / "tmp" / "data" / "out.bin").empty());
    put(path_a, "/dev_hdd0/tmp/data");
    CHECK(syscall(814, path_a) == err(tox2::kCellEisdir));
    put(path_a, "/dev_hdd0/tmp/data/out.bin");
    CHECK(syscall(814, path_a) == 0);
    CHECK(syscall(814, path_a) == err(tox2::kCellEnoent));
    CHECK(!stdfs::exists(hdd / "dev_hdd0" / "tmp" / "data" / "out.bin"));
    put(path_a, "/dev_hdd1/y");
    CHECK(syscall(811, path_a, 0777) == 0);
    put(path_a, "/dev_hdd1");
    CHECK(syscall(805, path_a, out) == 0);
    const std::uint32_t d1 = fdOut();
    for (int i = 0; i < 3; i++) {
        CHECK(syscall(806, d1, ent, out + 8) == 0 && tox2Read64(base, out + 8) == 258);
    }
    // hdd1 repeats its last entry at the end instead of zeroing it.
    CHECK(syscall(806, d1, ent, out + 8) == 0 && tox2Read64(base, out + 8) == 0);
    CHECK(base[ent + 1] == 1 && base[ent + 2] == 'y');
    CHECK(syscall(807, d1) == 0);

    // The id space: 252 objects starting at 3, then EMFILE.
    std::vector<std::uint32_t> fds;
    while (open("/dev_bdvd/b.txt", 0) == 0) {
        fds.push_back(fdOut());
    }
    CHECK(fds.size() == 252 && fds.front() == 3 && fds.back() == 254);
    CHECK(open("/dev_bdvd/b.txt", 0) == err(tox2::kCellEmfile));
    put(path_a, "/dev_bdvd");
    CHECK(syscall(805, path_a, out) == err(tox2::kCellEmfile));
    CHECK(syscall(804, fds[1]) == 0);
    CHECK(open("/dev_bdvd/b.txt", 0) == 0 && fdOut() == fds[1]);  // the lowest free id is reused
    for (const std::uint32_t id : fds) {
        syscall(804, id);
    }

    p.main.free(scratch);
    stdfs::remove_all(root, ec);
    tox2::configureVfs({}, std::nullopt);
    tox2::setLogSink({});
}

void testSysPrxForUser() {
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetCensus();
    tox2::resetLogOnce();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();

    constexpr const char* kModule = "sysPrxForUser";
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0xb2fcf2c8, kModule, "_sys_heap_create_heap"},
            {0x00800010, 0xaede4b03, kModule, "_sys_heap_delete_heap"},
            {0x00800020, 0x35168520, kModule, "_sys_heap_malloc"},
            {0x00800030, 0x44265c08, kModule, "_sys_heap_memalign"},
            {0x00800040, 0x8a561d92, kModule, "_sys_heap_free"},
            {0x00800050, 0x744680a2, kModule, "sys_initialize_tls"},
            {0x00800060, 0x8461e528, kModule, "sys_time_get_system_time"},
            {0x00800070, 0x45fe2fce, kModule, "_sys_spu_printf_initialize"},
            {0x00800080, 0x12345678, kModule, "sys_unknown"},
    };
    tox2::installImports(imports, 9);
    CHECK(countContaining(lines, "tox2: hle: 9 imports, 8 with handlers") == 1);
    CHECK(countContaining(lines, "is registered as") == 0);

    auto call = [&](std::uint32_t index, std::uint64_t r3 = 0, std::uint64_t r4 = 0, std::uint64_t r5 = 0) {
        auto ctx = std::make_unique<Tox2PpuContext>();
        ctx->gpr[3] = r3;
        ctx->gpr[4] = r4;
        ctx->gpr[5] = r5;
        tox2HleCall(ctx.get(), base, index);
        return ctx->gpr[3];
    };

    // Heap: ids start at 1; blocks are 64 KiB aligned and come from the main region.
    CHECK(call(0, 0x1000) == 1);
    CHECK(call(0, 0x1000) == 2);
    const std::uint64_t block = call(2, 1, 0x100);
    CHECK(block != 0 && block % 0x10000 == 0);
    CHECK(block >= tox2::kMainRegion.begin && block < tox2::kMainRegion.end);
    const std::uint64_t aligned = call(3, 1, 0x40000, 0x10);
    CHECK(aligned != 0 && aligned % 0x40000 == 0);
    const std::uint64_t small_align = call(3, 1, 0x10, 0x10);
    CHECK(small_align != 0 && small_align % 0x10000 == 0);
    CHECK(call(4, 1, block) == 0);
    CHECK(call(2, 1, 0x100) == block);  // the freed block is reused
    CHECK(call(4, 1, block) == 0);
    CHECK(call(4, 1, aligned) == 0);
    CHECK(call(4, 1, small_align) == 0);
    CHECK(call(1, 2) == 0);
    CHECK(call(0, 0x1000) == 2);  // the deleted id is free again

    // sys_initialize_tls is a no-op once r13 is set (the boot did it natively).
    {
        auto ctx = std::make_unique<Tox2PpuContext>();
        ctx->gpr[13] = 0x00123456;
        ctx->gpr[3] = 0x01000000;
        ctx->gpr[4] = 0x0010245c;
        ctx->gpr[5] = 4;
        ctx->gpr[6] = 0x110;
        tox2HleCall(ctx.get(), base, 5);
        CHECK(ctx->gpr[13] == 0x00123456);
        CHECK(ctx->gpr[3] == 0x01000000);
        CHECK(countContaining(lines, "tox2: tls:") == 0);
    }

    const std::uint64_t t0 = call(6);
    CHECK(t0 != 0 && call(6) >= t0);
    CHECK(call(7, 0x55) == 0);
    CHECK(call(8, 0x55) == 0);
    CHECK(tox2::sysPrxForUserHandlers().size() == 33);

    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

// A guest thread object that is not registered or started: it only gives a syscall caller an id.
std::unique_ptr<tox2::GuestThread> makeCaller(std::uint32_t id) {
    return std::make_unique<tox2::GuestThread>(id, tox2::GuestThreadParams{}, 0, 0);
}

std::uint64_t syscallAs(tox2::GuestThread& caller, std::uint32_t number, std::uint64_t r3 = 0, std::uint64_t r4 = 0,
                        std::uint64_t r5 = 0, std::uint64_t r6 = 0, std::uint64_t r7 = 0, std::uint64_t r8 = 0) {
    Tox2PpuContext* ctx = &caller.context();
    ctx->gpr[11] = number;
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    ctx->gpr[5] = r5;
    ctx->gpr[6] = r6;
    ctx->gpr[7] = r7;
    ctx->gpr[8] = r8;
    tox2Syscall(ctx, tox2::process().memory.base(), 0x10000);
    return ctx->gpr[3];
}

void testMutexAndCond() {
    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::resetSyncObjects();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t id_out = scratch, attr = scratch + 0x40, cond_out = scratch + 8;

    auto a = makeCaller(0x01000000);
    auto b = makeCaller(0x01000001);
    auto setMutexAttr = [&](std::uint32_t protocol, std::uint32_t recursive) {
        tox2Write32(base, attr, protocol);
        tox2Write32(base, attr + 4, recursive);
    };

    // Recursive mutex: the count survives nested locks, only the owner may unlock.
    setMutexAttr(2, 0x10);
    CHECK(syscallAs(*a, 100, id_out, attr) == 0);
    const std::uint32_t m = tox2Read32(base, id_out);
    CHECK(m == 0x85000000);
    CHECK(syscallAs(*a, 102, m, 0) == 0);
    CHECK(syscallAs(*a, 102, m, 0) == 0);
    CHECK(syscallAs(*a, 103, m) == 0);
    CHECK(syscallAs(*b, 103, m) == err(tox2::kCellEbusy));
    CHECK(syscallAs(*b, 104, m) == err(tox2::kCellEperm));
    CHECK(syscallAs(*a, 101, m) == err(tox2::kCellEbusy));
    CHECK(syscallAs(*a, 104, m) == 0);
    CHECK(syscallAs(*a, 104, m) == 0);
    CHECK(syscallAs(*b, 104, m) == err(tox2::kCellEperm));  // still owned
    CHECK(syscallAs(*a, 104, m) == 0);
    CHECK(syscallAs(*a, 104, m) == err(tox2::kCellEperm));  // now free
    CHECK(syscallAs(*a, 101, m) == 0);
    CHECK(syscallAs(*a, 101, m) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 102, m, 0) == err(tox2::kCellEsrch));

    // A non-recursive mutex reports a relock as a deadlock.
    setMutexAttr(1, 0x20);
    CHECK(syscallAs(*a, 100, id_out, attr) == 0);
    const std::uint32_t plain = tox2Read32(base, id_out);
    CHECK(plain == 0x85000100);
    CHECK(syscallAs(*a, 102, plain, 0) == 0);
    CHECK(syscallAs(*a, 102, plain, 0) == err(tox2::kCellEdeadlk));
    CHECK(syscallAs(*a, 103, plain) == err(tox2::kCellEdeadlk));
    CHECK(syscallAs(*a, 104, plain) == 0);

    setMutexAttr(9, 0x20);
    CHECK(syscallAs(*a, 100, id_out, attr) == err(tox2::kCellEinval));
    setMutexAttr(1, 0x21);
    CHECK(syscallAs(*a, 100, id_out, attr) == err(tox2::kCellEinval));
    CHECK(syscallAs(*a, 100, 0, attr) == err(tox2::kCellEfault));

    // Condition variable: the mutex must be held, and a timed-out wait still hands it back.
    CHECK(syscallAs(*a, 105, cond_out, 0x85000fff, attr) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 105, cond_out, plain, attr) == 0);
    const std::uint32_t c = tox2Read32(base, cond_out);
    CHECK(c == 0x86000000);
    CHECK(syscallAs(*a, 107, c, 0) == err(tox2::kCellEperm));   // mutex not held
    CHECK(syscallAs(*a, 101, plain) == err(tox2::kCellEperm));  // bound to a condition variable
    CHECK(syscallAs(*a, 108, c) == 0);                          // no waiters: dropped
    CHECK(syscallAs(*a, 109, c) == 0);
    {
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        CHECK(syscallAs(*a, 102, plain, 0) == 0);
        CHECK(syscallAs(*a, 107, c, 1000) == err(tox2::kCellEtimedout));
        CHECK(syscallAs(*b, 103, plain) == err(tox2::kCellEbusy));  // reacquired by the waiter
        CHECK(syscallAs(*a, 104, plain) == 0);
    }
    setMutexAttr(2, 0x10);
    CHECK(syscallAs(*a, 100, id_out, attr) == 0);
    const std::uint32_t rec = tox2Read32(base, id_out);
    CHECK(syscallAs(*a, 105, cond_out, rec, attr) == 0);
    const std::uint32_t rc = tox2Read32(base, cond_out);
    {
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        CHECK(syscallAs(*a, 102, rec, 0) == 0);
        CHECK(syscallAs(*a, 102, rec, 0) == 0);
        CHECK(syscallAs(*a, 107, rc, 1000) == err(tox2::kCellEtimedout));
        CHECK(syscallAs(*a, 104, rec) == 0);  // the recursion count came back
        CHECK(syscallAs(*a, 104, rec) == 0);
        CHECK(syscallAs(*a, 104, rec) == err(tox2::kCellEperm));
    }
    CHECK(syscallAs(*a, 106, rc) == 0);
    CHECK(syscallAs(*a, 106, rc) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 101, rec) == 0);

    // Contention: a second host thread blocks on a held mutex and gets it after the unlock. The
    // worker holds the guest lock from the flag write until its blocking wait releases it, so seeing
    // the flag while holding the lock means the worker is waiting.
    CHECK(syscallAs(*a, 100, id_out, attr) == 0);
    const std::uint32_t contended = tox2Read32(base, id_out);
    std::atomic<bool> b_ready{false};
    std::uint64_t b_result = ~0ull;
    {
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        CHECK(syscallAs(*a, 102, contended, 0) == 0);
        std::thread worker([&] {
            std::lock_guard<tox2::GuestLock> worker_guard(tox2::guestLock());
            b_ready = true;
            b_result = syscallAs(*b, 102, contended, 0);
        });
        tox2::guestLock().unlock();
        while (!b_ready) std::this_thread::yield();
        tox2::guestLock().lock();
        CHECK(syscallAs(*a, 104, contended) == 0);
        tox2::guestLock().unlock();
        worker.join();
        tox2::guestLock().lock();
    }
    CHECK(b_result == 0);
    CHECK(syscallAs(*a, 104, contended) == err(tox2::kCellEperm));
    CHECK(syscallAs(*b, 104, contended) == 0);

    // A waiter is woken by a signal and returns holding the mutex again.
    CHECK(syscallAs(*a, 105, cond_out, contended, attr) == 0);
    const std::uint32_t cv = tox2Read32(base, cond_out);
    b_ready = false;
    b_result = ~0ull;
    {
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        std::thread worker([&] {
            std::lock_guard<tox2::GuestLock> worker_guard(tox2::guestLock());
            syscallAs(*b, 102, contended, 0);
            b_ready = true;
            b_result = syscallAs(*b, 107, cv, 0);
        });
        tox2::guestLock().unlock();
        while (!b_ready) std::this_thread::yield();
        tox2::guestLock().lock();
        CHECK(syscallAs(*a, 102, contended, 0) == 0);  // free: the waiter released it
        CHECK(syscallAs(*a, 108, cv) == 0);
        CHECK(syscallAs(*a, 104, contended) == 0);
        tox2::guestLock().unlock();
        worker.join();
        tox2::guestLock().lock();
    }
    CHECK(b_result == 0);
    CHECK(syscallAs(*b, 104, contended) == 0);  // held by the waiter after the wait

    p.main.free(scratch);
    tox2::resetSyncObjects();
    tox2::setLogSink({});
    tox2::resetCensus();
}

void testSemaphores() {
    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::resetSyncObjects();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t id_out = scratch, attr = scratch + 0x40, value_out = scratch + 0x80;

    auto a = makeCaller(0x01000000);
    auto b = makeCaller(0x01000001);
    tox2Write32(base, attr, 1);          // SYS_SYNC_FIFO
    tox2Write32(base, attr + 4, 0x200);  // not shared between processes

    // Create checks its arguments.
    CHECK(syscallAs(*a, 90, 0, attr, 0, 1) == err(tox2::kCellEfault));
    CHECK(syscallAs(*a, 90, id_out, 0, 0, 1) == err(tox2::kCellEfault));
    CHECK(syscallAs(*a, 90, id_out, attr, 0, 0) == err(tox2::kCellEinval));  // the maximum must be positive
    CHECK(syscallAs(*a, 90, id_out, attr, 2, 1) == err(tox2::kCellEinval));  // initial above the maximum
    CHECK(syscallAs(*a, 90, id_out, attr, static_cast<std::uint64_t>(-1), 1) == err(tox2::kCellEinval));
    tox2Write32(base, attr, 3);  // priority inheritance is a mutex protocol
    CHECK(syscallAs(*a, 90, id_out, attr, 0, 1) == err(tox2::kCellEinval));
    tox2Write32(base, attr, 2);
    CHECK(syscallAs(*a, 90, id_out, attr, 1, 2) == 0);
    const std::uint32_t sem = tox2Read32(base, id_out);
    CHECK(sem == 0x96000000);

    // Counting: trywait takes one; a post that would pass the maximum fails and changes nothing.
    CHECK(syscallAs(*a, 114, sem, value_out) == 0 && tox2Read32(base, value_out) == 1);
    CHECK(syscallAs(*a, 93, sem) == 0);
    CHECK(syscallAs(*a, 93, sem) == err(tox2::kCellEbusy));
    CHECK(syscallAs(*a, 94, sem, 2) == 0);
    CHECK(syscallAs(*a, 94, sem, 1) == err(tox2::kCellEbusy));
    CHECK(syscallAs(*a, 94, sem, static_cast<std::uint64_t>(-1)) == err(tox2::kCellEinval));
    CHECK(syscallAs(*a, 114, sem, value_out) == 0 && tox2Read32(base, value_out) == 2);
    CHECK(syscallAs(*a, 114, sem, 0) == err(tox2::kCellEfault));
    {
        // A wait gives the guest lock up, so the caller holds it as a guest thread does.
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        CHECK(syscallAs(*a, 92, sem, 0) == 0);
        CHECK(syscallAs(*a, 92, sem, 1000) == 0);
        CHECK(syscallAs(*a, 92, sem, 1000) == err(tox2::kCellEtimedout));
    }
    CHECK(syscallAs(*a, 114, sem, value_out) == 0 && tox2Read32(base, value_out) == 0);

    // Unknown ids.
    CHECK(syscallAs(*a, 92, 0x96000fff, 1) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 93, 0x96000fff) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 94, 0x96000fff, 1) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 91, 0x96000fff) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 114, 0x96000fff, value_out) == err(tox2::kCellEsrch));

    // A second host thread blocks in sys_semaphore_wait. The worker holds the guest lock from the
    // flag write until its blocking wait releases it, so seeing the flag while holding the lock
    // means the worker is waiting: the semaphore cannot be destroyed, and a post wakes the worker.
    std::atomic<bool> b_ready{false};
    std::uint64_t b_result = ~0ull;
    {
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        std::thread worker([&] {
            std::lock_guard<tox2::GuestLock> worker_guard(tox2::guestLock());
            b_ready = true;
            b_result = syscallAs(*b, 92, sem, 0);
        });
        tox2::guestLock().unlock();
        while (!b_ready) std::this_thread::yield();
        tox2::guestLock().lock();
        CHECK(syscallAs(*a, 91, sem) == err(tox2::kCellEbusy));
        CHECK(syscallAs(*a, 94, sem, 1) == 0);
        tox2::guestLock().unlock();
        worker.join();
        tox2::guestLock().lock();
    }
    CHECK(b_result == 0);
    CHECK(syscallAs(*a, 114, sem, value_out) == 0 && tox2Read32(base, value_out) == 0);  // the worker took it

    CHECK(syscallAs(*a, 91, sem) == 0);
    CHECK(syscallAs(*a, 91, sem) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 93, sem) == err(tox2::kCellEsrch));

    p.main.free(scratch);
    tox2::resetSyncObjects();
    tox2::setLogSink({});
    tox2::resetCensus();
}

void testEventObjects() {
    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::resetSyncObjects();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t id_out = scratch, attr = scratch + 0x40, result = scratch + 0x80, num = scratch + 0x90;

    auto a = makeCaller(0x01000000);
    auto b = makeCaller(0x01000001);
    // Waiting calls need the guest lock; the helpers below hand it over to the worker threads.
    std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());

    // Event flag: attribute checks, then OR / AND waits with the clear modes.
    tox2Write32(base, attr, 1);             // protocol FIFO
    tox2Write32(base, attr + 20, 0x20000);  // multiple waiters
    CHECK(syscallAs(*a, 82, 0, attr, 0) == err(tox2::kCellEfault));
    tox2Write32(base, attr + 20, 0x30000);
    CHECK(syscallAs(*a, 82, id_out, attr, 0) == err(tox2::kCellEinval));
    tox2Write32(base, attr + 20, 0x20000);
    CHECK(syscallAs(*a, 82, id_out, attr, 0x5) == 0);
    const std::uint32_t flag = tox2Read32(base, id_out);
    CHECK(flag == 0x98000000);
    CHECK(syscallAs(*a, 139, flag, result) == 0);
    CHECK(tox2Read64(base, result) == 0x5);

    CHECK(syscallAs(*a, 85, flag, 0x4, tox2::kWaitOr, result, 0) == 0);  // already set
    CHECK(tox2Read64(base, result) == 0x5);
    CHECK(syscallAs(*a, 85, flag, 0x4, tox2::kWaitOr | tox2::kWaitClear, result, 1000) == 0);
    CHECK(tox2Read64(base, result) == 0x5);  // the value before the clear
    CHECK(syscallAs(*a, 139, flag, result) == 0);
    CHECK(tox2Read64(base, result) == 0x1);
    CHECK(syscallAs(*a, 87, flag, 0x6) == 0);
    CHECK(syscallAs(*a, 139, flag, result) == 0);
    CHECK(tox2Read64(base, result) == 0x7);
    CHECK(syscallAs(*a, 85, flag, 0x3, tox2::kWaitAnd | tox2::kWaitClearAll, result, 1000) == 0);
    CHECK(tox2Read64(base, result) == 0x7);
    CHECK(syscallAs(*a, 139, flag, result) == 0);
    CHECK(tox2Read64(base, result) == 0);
    CHECK(syscallAs(*a, 87, flag, 0x3) == 0);
    CHECK(syscallAs(*a, 85, flag, 0x7, tox2::kWaitAnd, result, 1000) == err(tox2::kCellEtimedout));
    CHECK(tox2Read64(base, result) == 0x3);  // the pattern at the time of the timeout
    CHECK(syscallAs(*a, 118, flag, 0x2) == 0);
    CHECK(syscallAs(*a, 139, flag, result) == 0);
    CHECK(tox2Read64(base, result) == 0x2);
    CHECK(syscallAs(*a, 85, flag, 0x1, 0x3, result, 1000) == err(tox2::kCellEinval));  // AND|OR
    CHECK(syscallAs(*a, 85, flag, 0x1, 0x4, result, 1000) == err(tox2::kCellEinval));
    CHECK(syscallAs(*a, 85, 0x98000fff, 0x1, tox2::kWaitOr, result, 1000) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 139, 0x98000fff, result) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 87, 0x98000fff, 1) == err(tox2::kCellEsrch));

    // A waiter on another host thread is woken by a set and cancelled by a cancel. The worker holds
    // the guest lock from the flag write until its blocking wait releases it.
    std::atomic<bool> b_ready{false};
    std::uint64_t b_result = ~0ull;
    auto runWaiter = [&](std::uint32_t id, std::uint64_t pattern, std::uint32_t mode) {
        b_ready = false;
        b_result = ~0ull;
        std::thread worker([&, id, pattern, mode] {
            std::lock_guard<tox2::GuestLock> worker_guard(tox2::guestLock());
            b_ready = true;
            b_result = syscallAs(*b, 85, id, pattern, mode, result + 0x20, 0);
        });
        tox2::guestLock().unlock();
        while (!b_ready) std::this_thread::yield();
        tox2::guestLock().lock();
        return worker;
    };
    const auto finish = [&](std::thread& worker) {
        tox2::guestLock().unlock();
        worker.join();
        tox2::guestLock().lock();
    };
    {
        CHECK(syscallAs(*a, 118, flag, 0) == 0);
        std::thread worker = runWaiter(flag, 0x8, tox2::kWaitOr | tox2::kWaitClear);
        CHECK(syscallAs(*a, 83, flag) == err(tox2::kCellEbusy));
        CHECK(syscallAs(*a, 87, flag, 0x1) == 0);  // does not match: still waiting
        CHECK(b_result == ~0ull);
        CHECK(syscallAs(*a, 87, flag, 0x8) == 0);
        finish(worker);
        CHECK(b_result == 0);
        CHECK(tox2Read64(base, result + 0x20) == 0x9);
        CHECK(syscallAs(*a, 139, flag, result) == 0);
        CHECK(tox2Read64(base, result) == 0x1);  // the waiter cleared its bit only

        std::thread canceled = runWaiter(flag, 0x10, tox2::kWaitAnd);
        CHECK(syscallAs(*a, 132, flag, num) == 0);
        CHECK(tox2Read32(base, num) == 1);
        finish(canceled);
        CHECK(b_result == err(tox2::kCellEcanceled));
        CHECK(tox2Read64(base, result + 0x20) == 0x1);
        CHECK(syscallAs(*a, 132, flag, num) == 0);
        CHECK(tox2Read32(base, num) == 0);
        CHECK(syscallAs(*a, 132, 0x98000fff, num) == err(tox2::kCellEsrch));
    }
    CHECK(syscallAs(*a, 83, flag) == 0);
    CHECK(syscallAs(*a, 83, flag) == err(tox2::kCellEsrch));

    // A single-waiter flag refuses a second waiter.
    tox2Write32(base, attr + 20, 0x10000);
    CHECK(syscallAs(*a, 82, id_out, attr, 0) == 0);
    const std::uint32_t single = tox2Read32(base, id_out);
    CHECK(single == 0x98000100);
    {
        std::thread worker = runWaiter(single, 0x1, tox2::kWaitOr);
        CHECK(syscallAs(*a, 85, single, 0x2, tox2::kWaitOr, result, 0) == err(tox2::kCellEperm));
        CHECK(syscallAs(*a, 87, single, 0x1) == 0);
        finish(worker);
        CHECK(b_result == 0);
    }
    CHECK(syscallAs(*a, 83, single) == 0);

    // Event queue and port: send then receive delivers source/data1..3 in r4..r7.
    const std::uint32_t queue_id = scratch + 0xa0, port_id = scratch + 0xb0, sys_event = scratch + 0xc0;
    tox2Write32(base, attr, 2);                                                   // protocol PRIORITY
    tox2Write32(base, attr + 4, 1);                                               // PPU queue
    CHECK(syscallAs(*a, 128, queue_id, attr, 0, 0) == err(tox2::kCellEinval));    // size 0
    CHECK(syscallAs(*a, 128, queue_id, attr, 0, 128) == err(tox2::kCellEinval));  // size 128
    tox2Write32(base, attr + 4, 3);
    CHECK(syscallAs(*a, 128, queue_id, attr, 0, 2) == err(tox2::kCellEinval));  // type
    tox2Write32(base, attr + 4, 1);
    CHECK(syscallAs(*a, 128, queue_id, attr, 0, 2) == 0);
    const std::uint32_t queue = tox2Read32(base, queue_id);
    CHECK(queue == 0x8d000000);
    CHECK(syscallAs(*a, 134, port_id, 2, 0) == err(tox2::kCellEinval));
    CHECK(syscallAs(*a, 134, port_id, 1, 0) == 0);
    const std::uint32_t port = tox2Read32(base, port_id);
    CHECK(port == 0x0e000000);
    CHECK(syscallAs(*a, 138, port, 1, 2, 3) == err(tox2::kCellEnotconn));
    CHECK(syscallAs(*a, 136, port, 0x8d000fff) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 136, port, queue) == 0);
    CHECK(syscallAs(*a, 136, port, queue) == err(tox2::kCellEisconn));
    CHECK(syscallAs(*a, 135, port) == err(tox2::kCellEisconn));

    CHECK(syscallAs(*a, 138, port, 0x11, 0x22, 0x33) == 0);
    CHECK(syscallAs(*a, 138, port, 0x44, 0x55, 0x66) == 0);
    CHECK(syscallAs(*a, 138, port, 0x77, 0x88, 0x99) == err(tox2::kCellEbusy));  // queue full
    CHECK(syscallAs(*a, 130, queue, sys_event, 1000) == 0);
    Tox2PpuContext* actx = &a->context();
    CHECK(actx->gpr[4] == ((std::uint64_t{1} << 32) | port));  // unnamed port: pid << 32 | id
    CHECK(actx->gpr[5] == 0x11 && actx->gpr[6] == 0x22 && actx->gpr[7] == 0x33);
    CHECK(tox2Read64(base, sys_event) == 0);  // the event pointer is never written
    CHECK(syscallAs(*a, 133, queue) == 0);    // drain
    CHECK(syscallAs(*a, 130, queue, sys_event, 1000) == err(tox2::kCellEtimedout));

    // A named port reports its name; a blocked receiver gets the event directly.
    CHECK(syscallAs(*a, 137, port) == 0);
    CHECK(syscallAs(*a, 137, port) == err(tox2::kCellEnotconn));
    CHECK(syscallAs(*a, 135, port) == 0);
    CHECK(syscallAs(*a, 134, port_id, 1, 0xabcd) == 0);
    const std::uint32_t named = tox2Read32(base, port_id);
    CHECK(syscallAs(*a, 136, named, queue) == 0);
    {
        std::atomic<bool> receiving{false};
        std::thread worker([&] {
            std::lock_guard<tox2::GuestLock> worker_guard(tox2::guestLock());
            receiving = true;
            b_result = syscallAs(*b, 130, queue, sys_event, 0);
        });
        tox2::guestLock().unlock();
        while (!receiving) std::this_thread::yield();
        tox2::guestLock().lock();
        CHECK(syscallAs(*a, 129, queue, 0) == err(tox2::kCellEbusy));  // a receiver waits
        CHECK(syscallAs(*a, 138, named, 7, 8, 9) == 0);
        tox2::guestLock().unlock();
        worker.join();
        tox2::guestLock().lock();
    }
    CHECK(b_result == 0);
    Tox2PpuContext* bctx = &b->context();
    CHECK(bctx->gpr[4] == 0xabcd && bctx->gpr[5] == 7 && bctx->gpr[6] == 8 && bctx->gpr[7] == 9);

    // Forced destroy cancels receivers; the port is then disconnected.
    {
        std::atomic<bool> receiving{false};
        std::thread worker([&] {
            std::lock_guard<tox2::GuestLock> worker_guard(tox2::guestLock());
            receiving = true;
            b_result = syscallAs(*b, 130, queue, sys_event, 0);
        });
        tox2::guestLock().unlock();
        while (!receiving) std::this_thread::yield();
        tox2::guestLock().lock();
        CHECK(syscallAs(*a, 129, queue, 2) == err(tox2::kCellEinval));
        CHECK(syscallAs(*a, 129, queue, 1) == 0);
        tox2::guestLock().unlock();
        worker.join();
        tox2::guestLock().lock();
    }
    CHECK(b_result == err(tox2::kCellEcanceled));
    CHECK(syscallAs(*a, 129, queue, 0) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 130, queue, sys_event, 1000) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 138, named, 1, 2, 3) == err(tox2::kCellEnotconn));
    CHECK(syscallAs(*a, 135, named) == 0);
    CHECK(syscallAs(*a, 135, named) == err(tox2::kCellEsrch));

    p.main.free(scratch);
    tox2::resetSyncObjects();
    tox2::setLogSink({});
    tox2::resetCensus();
}

void testEventQueueIpcKeys() {
    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::resetSyncObjects();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t id_out = scratch, attr = scratch + 0x40, port_out = scratch + 0x80, sys_event = scratch + 0xc0;

    auto a = makeCaller(0x01000000);
    auto b = makeCaller(0x01000001);
    std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());

    constexpr std::uint64_t kKey = 0x8000cafe02460300;  // the game mixer's first notify queue key
    tox2Write32(base, attr, 1);                         // protocol FIFO
    tox2Write32(base, attr + 4, 1);                     // PPU queue

    // A keyed queue is found by its key.
    CHECK(tox2::eventQueueIdByKey(kKey) == 0);
    CHECK(syscallAs(*a, 128, id_out, attr, kKey, 2) == 0);
    const std::uint32_t first = tox2Read32(base, id_out);
    CHECK(first == 0x8d000000);
    CHECK(tox2::eventQueueIdByKey(kKey) == first);
    CHECK(tox2::eventQueueIdByKey(kKey + 1) == 0);
    CHECK(tox2::eventQueueIdByKey(0) == 0);

    // A duplicate key fails with EEXIST and creates nothing: `*id` stays and the id counter does not
    // move. The game's retry with key + 1 then succeeds.
    tox2Write32(base, id_out, 0xdeadbeef);
    CHECK(syscallAs(*a, 128, id_out, attr, kKey, 2) == err(tox2::kCellEexist));
    CHECK(tox2Read32(base, id_out) == 0xdeadbeef);
    CHECK(tox2::eventQueueIdByKey(kKey) == first);
    CHECK(syscallAs(*a, 128, id_out, attr, kKey + 1, 2) == 0);
    const std::uint32_t second = tox2Read32(base, id_out);
    CHECK(second == first + 0x100);
    CHECK(tox2::eventQueueIdByKey(kKey + 1) == second);
    CHECK(tox2::eventQueueIdByKey(kKey) == first);

    // Argument checks come before the key rules.
    tox2Write32(base, attr + 4, 3);
    CHECK(syscallAs(*a, 128, id_out, attr, kKey, 2) == err(tox2::kCellEinval));
    tox2Write32(base, attr + 4, 1);
    CHECK(syscallAs(*a, 128, id_out, attr, kKey, 0) == err(tox2::kCellEinval));

    // Key 0 is local: any number of queues, none findable by key.
    CHECK(syscallAs(*a, 128, id_out, attr, 0, 2) == 0);
    const std::uint32_t local_a = tox2Read32(base, id_out);
    CHECK(syscallAs(*a, 128, id_out, attr, 0, 2) == 0);
    const std::uint32_t local_b = tox2Read32(base, id_out);
    CHECK(local_a != local_b && local_a != 0 && local_b != 0);
    CHECK(tox2::eventQueueIdByKey(0) == 0);

    // Destroying a queue forgets its key; the key can then be used again. A refused destroy (a
    // receiver waits) keeps it.
    {
        std::atomic<bool> receiving{false};
        std::uint64_t b_result = ~0ull;
        std::thread worker([&] {
            std::lock_guard<tox2::GuestLock> worker_guard(tox2::guestLock());
            receiving = true;
            b_result = syscallAs(*b, 130, second, sys_event, 0);
        });
        tox2::guestLock().unlock();
        while (!receiving) std::this_thread::yield();
        tox2::guestLock().lock();
        CHECK(syscallAs(*a, 129, second, 0) == err(tox2::kCellEbusy));
        CHECK(tox2::eventQueueIdByKey(kKey + 1) == second);

        // sendEventToQueue wakes the blocked receiver; the event arrives in r4..r7.
        CHECK(tox2::sendEventToQueue(second, 0x1111, 1, 2, 3) == 0);
        tox2::guestLock().unlock();
        worker.join();
        tox2::guestLock().lock();
        CHECK(b_result == 0);
        Tox2PpuContext* bctx = &b->context();
        CHECK(bctx->gpr[4] == 0x1111 && bctx->gpr[5] == 1 && bctx->gpr[6] == 2 && bctx->gpr[7] == 3);
    }
    CHECK(syscallAs(*a, 129, second, 0) == 0);
    CHECK(tox2::eventQueueIdByKey(kKey + 1) == 0);
    CHECK(tox2::eventQueueIdByKey(kKey) == first);
    CHECK(syscallAs(*a, 128, id_out, attr, kKey + 1, 2) == 0);
    const std::uint32_t reused = tox2Read32(base, id_out);
    CHECK(reused != second);  // ids are never handed out twice
    CHECK(tox2::eventQueueIdByKey(kKey + 1) == reused);

    // With nobody waiting the event is stored; a size-2 queue refuses the third one.
    CHECK(tox2::sendEventToQueue(first, 0xa1, 10, 11, 12) == 0);
    CHECK(tox2::sendEventToQueue(first, 0xa2, 20, 21, 22) == 0);
    CHECK(tox2::sendEventToQueue(first, 0xa3, 30, 31, 32) == tox2::kCellEbusy);
    CHECK(tox2::sendEventToQueue(0x8d000fff, 1, 2, 3, 4) == tox2::kCellEsrch);
    Tox2PpuContext* actx = &a->context();
    CHECK(syscallAs(*a, 130, first, sys_event, 1000) == 0);
    CHECK(actx->gpr[4] == 0xa1 && actx->gpr[5] == 10 && actx->gpr[6] == 11 && actx->gpr[7] == 12);
    CHECK(syscallAs(*a, 130, first, sys_event, 1000) == 0);
    CHECK(actx->gpr[4] == 0xa2 && actx->gpr[5] == 20 && actx->gpr[6] == 21 && actx->gpr[7] == 22);
    CHECK(syscallAs(*a, 130, first, sys_event, 1000) == err(tox2::kCellEtimedout));  // the third was lost

    // A local port connected by queue id reaches a keyed queue (the game's shutdown path).
    CHECK(syscallAs(*a, 134, port_out, 1, 0) == 0);
    const std::uint32_t port = tox2Read32(base, port_out);
    CHECK(syscallAs(*a, 136, port, first) == 0);
    CHECK(syscallAs(*a, 138, port, 0x51, 0x52, 0x53) == 0);
    CHECK(syscallAs(*a, 130, first, sys_event, 1000) == 0);
    CHECK(actx->gpr[4] == ((std::uint64_t{1} << 32) | port));
    CHECK(actx->gpr[5] == 0x51 && actx->gpr[6] == 0x52 && actx->gpr[7] == 0x53);

    // Destroying the keyed queue frees its key even though a port still points at it.
    CHECK(syscallAs(*a, 129, first, 0) == 0);
    CHECK(tox2::eventQueueIdByKey(kKey) == 0);
    CHECK(tox2::sendEventToQueue(first, 1, 2, 3, 4) == tox2::kCellEsrch);
    CHECK(syscallAs(*a, 138, port, 1, 2, 3) == err(tox2::kCellEnotconn));
    CHECK(syscallAs(*a, 128, id_out, attr, kKey, 2) == 0);

    p.main.free(scratch);
    tox2::resetSyncObjects();
    CHECK(tox2::eventQueueIdByKey(kKey) == 0);  // a reset forgets the keys too
    tox2::setLogSink({});
    tox2::resetCensus();
}

// Layout of the scratch block that the SPU thread group tests and children share.
constexpr std::uint32_t kSpuIdOut = 0x000;
constexpr std::uint32_t kSpuGroupAttr = 0x010;
constexpr std::uint32_t kSpuThreadOut = 0x030;
constexpr std::uint32_t kSpuThreadAttr = 0x050;
constexpr std::uint32_t kSpuArgs = 0x070;
constexpr std::uint32_t kSpuImage = 0x0A0;
constexpr std::uint32_t kSpuSegments = 0x0C0;
constexpr std::uint32_t kSpuGroupName = 0x200;
constexpr std::uint32_t kSpuThreadName = 0x280;
constexpr std::uint32_t kSpuElf = 0x400;
constexpr std::string_view kSpuGroupNameText = "SpursTLGraphicsCellSpursKernelGroup";  // 35 characters
constexpr std::string_view kSpuThreadNameText = "SpursTLGraphicsCellSpursKernel0";     // 31 characters

void putText(std::uint8_t* base, std::uint32_t at, std::string_view text) {
    for (std::size_t i = 0; i < text.size(); i++) tox2Write8(base, at + static_cast<std::uint32_t>(i), text[i]);
    tox2Write8(base, at + static_cast<std::uint32_t>(text.size()), 0);
}

// An SPU ELF32 with one PT_LOAD (0x40 bytes in the file, 0x60 in memory, at local store 0x800), entry 0x818.
void writeSpuElf(std::uint8_t* base, std::uint32_t elf) {
    for (std::uint32_t i = 0; i < 0x200; i++) tox2Write8(base, elf + i, 0);
    tox2Write32(base, elf, 0x7F454C46);
    tox2Write8(base, elf + 4, 1);
    tox2Write8(base, elf + 5, 2);
    tox2Write16(base, elf + 0x12, 23);
    tox2Write32(base, elf + 0x18, 0x818);
    tox2Write32(base, elf + 0x1C, 0x34);
    tox2Write16(base, elf + 0x2A, 0x20);
    tox2Write16(base, elf + 0x2C, 1);
    tox2Write32(base, elf + 0x34, 1);
    tox2Write32(base, elf + 0x38, 0x100);
    tox2Write32(base, elf + 0x3C, 0x800);
    tox2Write32(base, elf + 0x44, 0x40);
    tox2Write32(base, elf + 0x48, 0x60);
}

// Creates a group of `num` threads the way libsre does (type 0x18, priority 250), imports the ELF and
// initializes every thread from it. Returns the group id.
std::uint32_t buildSpuGroup(tox2::GuestThread& caller, std::uint8_t* base, std::uint32_t scratch, std::uint32_t num) {
    putText(base, scratch + kSpuGroupName, kSpuGroupNameText);
    tox2Write32(base, scratch + kSpuGroupAttr, static_cast<std::uint32_t>(kSpuGroupNameText.size() + 1));
    tox2Write32(base, scratch + kSpuGroupAttr + 4, scratch + kSpuGroupName);
    tox2Write32(base, scratch + kSpuGroupAttr + 8, 0x18);
    tox2Write32(base, scratch + kSpuGroupAttr + 12, 0xdeadbeef);  // the container word is left uninitialised
    CHECK(syscallAs(caller, 170, scratch + kSpuIdOut, num, 250, scratch + kSpuGroupAttr) == 0);
    const std::uint32_t group = tox2Read32(base, scratch + kSpuIdOut);

    writeSpuElf(base, scratch + kSpuElf);
    CHECK(tox2::spuImageImport(base, scratch + kSpuImage, scratch + kSpuElf, 1) == 0);
    putText(base, scratch + kSpuThreadName, kSpuThreadNameText);
    tox2Write32(base, scratch + kSpuThreadAttr, scratch + kSpuThreadName);
    tox2Write32(base, scratch + kSpuThreadAttr + 4, 32);
    tox2Write32(base, scratch + kSpuThreadAttr + 8, 2);
    for (std::uint32_t i = 0; i < num; i++) {
        tox2Write64(base, scratch + kSpuArgs, std::uint64_t{i} << 32);
        tox2Write64(base, scratch + kSpuArgs + 8, 0x01127c80);
        tox2Write64(base, scratch + kSpuArgs + 16, 0);
        tox2Write64(base, scratch + kSpuArgs + 24, 0);
        CHECK(syscallAs(caller, 172, scratch + kSpuThreadOut + 4 * i, group, i, scratch + kSpuImage,
                        scratch + kSpuThreadAttr, scratch + kSpuArgs) == 0);
    }
    return group;
}

void testSpuThreadGroups() {
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetCensus();
    tox2::resetLogOnce();
    tox2::resetSyncObjects();
    tox2::resetSpuThreadGroups();
    tox2::resetSpuImages();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t id_out = scratch + kSpuIdOut, attr = scratch + kSpuGroupAttr;
    const std::uint32_t thread_out = scratch + kSpuThreadOut, thread_attr = scratch + kSpuThreadAttr;
    const std::uint32_t args = scratch + kSpuArgs, image = scratch + kSpuImage, segs = scratch + kSpuSegments;

    auto a = makeCaller(0x01000000);
    std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());

    putText(base, scratch + kSpuGroupName, "group");
    auto setAttr = [&](std::uint32_t nsize, std::uint32_t type, std::uint32_t container = 0) {
        tox2Write32(base, attr, nsize);
        tox2Write32(base, attr + 4, scratch + kSpuGroupName);
        tox2Write32(base, attr + 8, type);
        tox2Write32(base, attr + 12, container);
    };
    auto create = [&](std::uint64_t num, std::int32_t priority, std::uint32_t type, std::uint32_t container = 0) {
        setAttr(6, type, container);
        return syscallAs(*a, 170, id_out, num, static_cast<std::uint32_t>(priority), attr);
    };

    // Group creation: argument errors leave no group behind.
    setAttr(6, 0);
    CHECK(syscallAs(*a, 170, 0, 1, 100, attr) == err(tox2::kCellEfault));
    CHECK(syscallAs(*a, 170, id_out, 1, 100, 0) == err(tox2::kCellEfault));
    CHECK(create(0, 100, 0) == err(tox2::kCellEinval));
    CHECK(create(7, 100, 0) == err(tox2::kCellEinval));
    CHECK(create(1, 100, 1) == err(tox2::kCellEinval));
    CHECK(create(1, 15, 0) == err(tox2::kCellEinval));
    CHECK(create(1, 256, 0) == err(tox2::kCellEinval));
    CHECK(create(1, 1, 0x18, 0xdeadbeef) == 0);  // type 0x18: any priority, any container word
    tox2::resetSpuThreadGroups();
    setAttr(0x81, 0);
    CHECK(syscallAs(*a, 170, id_out, 1, 100, attr) == err(tox2::kCellEinval));
    CHECK(create(1, 100, 4, 0x3F000000) == err(tox2::kCellEsrch));  // type 4 needs a memory container
    std::uint32_t container = 0;
    CHECK(syscallAs(*a, 324, id_out, 0x100000) == 0);
    container = tox2Read32(base, id_out);
    CHECK(create(2, 100, 4, container) == 0);
    CHECK(syscallAs(*a, 325, container) == 0);
    tox2::resetSpuThreadGroups();

    // Ids start at 0x04000100 and advance by 0x100; the group name and parameters are logged.
    lines.clear();
    CHECK(create(2, 100, 0) == 0 && tox2Read32(base, id_out) == 0x04000100);
    CHECK(countContaining(lines, "tox2: spu: group 0x04000100 \"group\" created: 2 threads, priority 100, type 0x0") ==
          1);
    CHECK(create(1, 250, 0x20) == 0 && tox2Read32(base, id_out) == 0x04000200);
    CHECK(syscallAs(*a, 171, 0x04000200) == 0);
    CHECK(syscallAs(*a, 171, 0x04000200) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 171, 0x04000101) == err(tox2::kCellEsrch));
    CHECK(create(1, 100, 0) == 0 && tox2Read32(base, id_out) == 0x04000300);  // 0x04000200 is not reused at once
    const std::uint32_t group = 0x04000100;

    // Thread initialization. The image is one that the import call produced.
    writeSpuElf(base, scratch + kSpuElf);
    CHECK(tox2::spuImageImport(base, image, scratch + kSpuElf, 1) == 0);
    const std::uint32_t array = tox2Read32(base, image + 8);
    putText(base, scratch + kSpuThreadName, "thread");
    tox2Write32(base, thread_attr, scratch + kSpuThreadName);
    tox2Write32(base, thread_attr + 4, 6);
    tox2Write32(base, thread_attr + 8, 2);
    tox2Write64(base, args, 0x1111);
    tox2Write64(base, args + 8, 0x2222);
    tox2Write64(base, args + 16, 0x3333);
    tox2Write64(base, args + 24, 0x4444);
    auto init = [&](std::uint32_t group_id, std::uint32_t index) {
        return syscallAs(*a, 172, thread_out, group_id, index, image, thread_attr, args);
    };
    CHECK(syscallAs(*a, 172, 0, group, 0, image, thread_attr, args) == err(tox2::kCellEfault));
    CHECK(syscallAs(*a, 172, thread_out, group, 0, 0, thread_attr, args) == err(tox2::kCellEfault));
    CHECK(syscallAs(*a, 172, thread_out, group, 0, image, 0, args) == err(tox2::kCellEfault));
    CHECK(syscallAs(*a, 172, thread_out, group, 0, image, thread_attr, 0) == err(tox2::kCellEfault));
    CHECK(init(0x04000900, 0) == err(tox2::kCellEsrch));
    CHECK(init(group, 256) == err(tox2::kCellEinval));  // the SPU number indexes a table of 256
    tox2Write32(base, thread_attr + 4, 0x81);
    CHECK(init(group, 0) == err(tox2::kCellEinval));
    tox2Write32(base, thread_attr + 4, 6);
    tox2Write32(base, image + 4, 0x40000);
    CHECK(init(group, 0) == err(tox2::kCellEinval));  // entry point beyond the local store
    tox2Write32(base, image + 4, 0x818);
    tox2Write32(base, image + 12, 0);
    CHECK(init(group, 0) == err(tox2::kCellEinval));
    tox2Write32(base, image + 12, 0x21);
    CHECK(init(group, 0) == err(tox2::kCellEinval));
    tox2Write32(base, image + 12, 2);

    // A copy of the image whose segment list is wrong in one way at a time.
    tox2Write32(base, image + 8, segs);
    for (std::uint32_t i = 0; i < 8; i++) tox2Write32(base, segs + 4 * i, tox2Read32(base, array + 4 * i));
    CHECK(init(group, 0) == 0);  // the unchanged list is fine
    tox2::resetSpuThreadGroups();
    CHECK(create(2, 100, 0) == 0);
    tox2Write32(base, segs, 3);
    CHECK(init(group, 0) == err(tox2::kCellEinval));  // unknown segment type
    tox2Write32(base, segs, 1);
    tox2Write32(base, segs + 4, 0x3FFF0);
    tox2Write32(base, segs + 8, 0x20);
    CHECK(init(group, 0) == err(tox2::kCellEinval));  // ends beyond the local store
    tox2Write32(base, segs + 4, 0x800);
    tox2Write32(base, segs + 8, 0xFFFFFFF0);
    CHECK(init(group, 0) == err(tox2::kCellEinval));  // negative size
    tox2Write32(base, segs + 8, 0x40);
    tox2Write32(base, image + 8, array);
    CHECK(create(1, 100, 0) == 0 && tox2Read32(base, id_out) == 0x04000200);  // never initialized

    // Index 1 first: thread ids count in initialization order, not by index.
    lines.clear();
    CHECK(init(group, 1) == 0 && tox2Read32(base, thread_out) == 0x01000100);
    CHECK(countContaining(lines,
                          std::format("\"thread\" initialized: group 0x04000100 index 1, image {:#010x} entry 0x818, "
                                      "args 0x1111 0x2222 0x3333 0x4444",
                                      scratch + kSpuElf)) == 1);
    CHECK(init(group, 1) == err(tox2::kCellEbusy));
    CHECK(syscallAs(*a, 173, group) == err(tox2::kCellEstat));  // not every thread exists yet
    CHECK(syscallAs(*a, 178, group) == err(tox2::kCellEstat));
    CHECK(syscallAs(*a, 177, group, 0) == err(tox2::kCellEstat));
    CHECK(syscallAs(*a, 173, 0x04000900) == err(tox2::kCellEsrch));
    CHECK(init(group, 0) == 0 && tox2Read32(base, thread_out) == 0x02000100);
    CHECK(init(group, 0) == err(tox2::kCellEbusy));  // the group is initialized now
    CHECK(syscallAs(*a, 177, group, 0) == err(tox2::kCellEstat));
    CHECK(syscallAs(*a, 177, 0x04000900, 0) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 178, 0x04000900) == err(tox2::kCellEsrch));
    const std::uint32_t thread0 = 0x02000100, thread1 = 0x01000100;

    // Event queues for the connections.
    const std::uint32_t queue_attr = scratch + 0x180;
    tox2Write32(base, queue_attr, 1);
    tox2Write32(base, queue_attr + 4, 2);
    CHECK(syscallAs(*a, 128, id_out, queue_attr, 0, 8) == 0);
    const std::uint32_t queue = tox2Read32(base, id_out);
    CHECK(syscallAs(*a, 128, id_out, queue_attr, 0, 8) == 0);
    const std::uint32_t queue2 = tox2Read32(base, id_out);

    // Group events: types 1, 2 and 4, each connected once.
    CHECK(syscallAs(*a, 185, group, queue, 3) == err(tox2::kCellEinval));
    CHECK(syscallAs(*a, 185, 0x04000900, queue, 1) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 185, group, 0x8d000fff, 1) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 185, group, queue, 1) == 0);
    CHECK(syscallAs(*a, 185, group, queue2, 1) == err(tox2::kCellEbusy));
    CHECK(syscallAs(*a, 185, group, queue2, 4) == 0);
    CHECK(syscallAs(*a, 186, group, 3) == err(tox2::kCellEinval));
    CHECK(syscallAs(*a, 186, 0x04000900, 1) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 186, group, 1) == 0);
    CHECK(syscallAs(*a, 186, group, 1) == err(tox2::kCellEnotconn));
    CHECK(syscallAs(*a, 186, group, 2) == err(tox2::kCellEnotconn));
    CHECK(syscallAs(*a, 185, group, queue, 1) == 0);  // free again

    // Thread events: user type 1, ports 0..63.
    CHECK(syscallAs(*a, 191, thread0, queue, 2, 3) == err(tox2::kCellEinval));
    CHECK(syscallAs(*a, 191, thread0, queue, 1, 64) == err(tox2::kCellEinval));
    CHECK(syscallAs(*a, 191, 0x03000100, queue, 1, 3) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 191, thread0, 0x8d000fff, 1, 3) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 191, thread0, queue, 1, 3) == 0);
    CHECK(syscallAs(*a, 191, thread0, queue2, 1, 3) == err(tox2::kCellEisconn));
    CHECK(syscallAs(*a, 192, thread0, 2, 3) == err(tox2::kCellEinval));
    CHECK(syscallAs(*a, 192, thread0, 1, 64) == err(tox2::kCellEinval));
    CHECK(syscallAs(*a, 192, 0x03000100, 1, 3) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 192, thread1, 1, 3) == err(tox2::kCellEnotconn));
    CHECK(syscallAs(*a, 191, thread0, queue, 1, 4) == 0);
    CHECK(syscallAs(*a, 192, thread0, 1, 4) == 0);
    CHECK(syscallAs(*a, 192, thread0, 1, 4) == err(tox2::kCellEnotconn));

    // Queue bindings: only SPU queues; one binding per key and per queue and thread; 32 slots a thread.
    tox2Write32(base, queue_attr + 4, 1);
    CHECK(syscallAs(*a, 128, id_out, queue_attr, 0, 8) == 0);
    const std::uint32_t ppu_queue = tox2Read32(base, id_out);
    tox2Write32(base, queue_attr + 4, 2);
    CHECK(syscallAs(*a, 193, 0x03000100, queue, 5) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 193, thread0, 0x8d000fff, 5) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 193, 0x03000100, ppu_queue, 5) == err(tox2::kCellEsrch));  // the lookups come first
    CHECK(syscallAs(*a, 193, thread0, ppu_queue, 5) == err(tox2::kCellEinval));
    CHECK(syscallAs(*a, 193, thread0, queue, 0) == 0);                       // any key is valid, also 0
    CHECK(syscallAs(*a, 193, thread0, queue2, 0) == err(tox2::kCellEbusy));  // the key is taken
    CHECK(syscallAs(*a, 193, thread0, queue, 1) == err(tox2::kCellEbusy));   // the queue is bound already
    CHECK(syscallAs(*a, 193, thread1, queue, 0) == 0);                       // the table belongs to the thread
    CHECK(syscallAs(*a, 194, 0x03000100, 0) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 194, thread0, 7) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 194, thread0, 0) == 0);
    CHECK(syscallAs(*a, 194, thread0, 0) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 193, thread0, queue, 1) == 0);  // the slot and the queue are free again
    std::vector<std::uint32_t> more;
    for (std::uint32_t i = 0; i < 32; i++) {
        CHECK(syscallAs(*a, 128, id_out, queue_attr, 0, 8) == 0);
        more.push_back(tox2Read32(base, id_out));
    }
    for (std::uint32_t i = 0; i < 31; i++) CHECK(syscallAs(*a, 193, thread0, more[i], 100 + i) == 0);
    CHECK(syscallAs(*a, 193, thread0, more[31], 200) == err(tox2::kCellEagain));  // 32 slots are in use
    CHECK(syscallAs(*a, 194, thread0, 100) == 0);
    CHECK(syscallAs(*a, 193, thread0, more[31], 200) == 0);
    // A destroyed queue keeps its slot: the key stays taken and can be unbound.
    CHECK(syscallAs(*a, 129, more[31], 0) == 0);
    CHECK(syscallAs(*a, 193, thread0, queue2, 200) == err(tox2::kCellEbusy));
    CHECK(syscallAs(*a, 194, thread0, 200) == 0);
    // The queue calls that take events refuse an SPU queue.
    CHECK(syscallAs(*a, 130, queue, 0, 1000) == err(tox2::kCellEinval));
    CHECK(syscallAs(*a, 131, queue, scratch + 0x800, 1, scratch + 0x7f0) == err(tox2::kCellEinval));
    CHECK(syscallAs(*a, 194, thread1, 0) == 0);

    // Event connection on all threads. Port 3 is taken on thread 0, so it is skipped for the group.
    const std::uint32_t spup = scratch + 0x1C0;
    tox2Write8(base, spup, 0xee);
    CHECK(syscallAs(*a, 251, group, queue, 0, spup) == err(tox2::kCellEinval));
    CHECK(syscallAs(*a, 251, 0x04000900, queue, 4, spup) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 251, group, queue, 4, 0) == err(tox2::kCellEfault));
    CHECK(syscallAs(*a, 251, group, queue, 0x8, spup) == err(tox2::kCellEisconn));
    CHECK(tox2Read8(base, spup) == 0xee);
    CHECK(syscallAs(*a, 251, group, queue2, 0x18, spup) == 0 && tox2Read8(base, spup) == 4);
    CHECK(syscallAs(*a, 191, thread1, queue, 1, 4) == err(tox2::kCellEisconn));  // bound on thread 1 too
    CHECK(syscallAs(*a, 252, group, 64) == err(tox2::kCellEinval));
    CHECK(syscallAs(*a, 252, 0x04000900, 4) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 252, group, 5) == err(tox2::kCellEnotconn));
    CHECK(syscallAs(*a, 252, group, 4) == 0);
    CHECK(syscallAs(*a, 191, thread1, queue, 1, 4) == 0);

    // A group that is not initialized refuses the all-threads connection.
    CHECK(syscallAs(*a, 251, 0x04000200, queue, 4, spup) == err(tox2::kCellEstat));

    // Cooperative victims are only recorded.
    CHECK(syscallAs(*a, 250, group, 0x3) == 0);
    CHECK(syscallAs(*a, 250, 0x04000900, 0x3) == err(tox2::kCellEsrch));

    // Destroy takes the threads along.
    CHECK(syscallAs(*a, 171, group) == 0);
    CHECK(syscallAs(*a, 191, thread0, queue, 1, 9) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 173, group) == err(tox2::kCellEsrch));

    // Every slot can be used once; the 256th group has no slot.
    tox2::resetSpuThreadGroups();
    std::uint32_t created = 0;
    for (; created < 300; created++) {
        if (create(1, 100, 0) != 0) break;
    }
    CHECK(created == 255);
    CHECK(create(1, 100, 0) == err(tox2::kCellEagain));
    CHECK(syscallAs(*a, 171, 0x04000100) == 0);
    CHECK(create(1, 100, 0) == 0 && tox2Read32(base, id_out) == 0x04000100);

    const std::string census = tox2::formatCensus();
    CHECK(census.find("sc 170 sys_spu_thread_group_create: ") != std::string::npos);
    CHECK(census.find("sc 173 sys_spu_thread_group_start: 3 [impl]") != std::string::npos);

    tox2::resetSpuThreadGroups();
    tox2::resetSpuImages();
    tox2::resetSyncObjects();
    p.main.free(scratch);
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

void testSpuAndSysmodule() {
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetCensus();
    tox2::resetLogOnce();
    tox2::resetSyncObjects();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t queue_id_out = scratch, attr = scratch + 0x40, spup = scratch + 0x80;

    auto a = makeCaller(0x01000000);
    std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());

    // sys_spu_initialize: more than 5 raw SPUs is invalid.
    CHECK(syscallAs(*a, 169, 6, 5) == 0);
    CHECK(syscallAs(*a, 169, 6, 6) == err(tox2::kCellEinval));
    CHECK(syscallAs(*a, 169, 0, 0) == 0);

    tox2Write32(base, attr, 1);      // protocol FIFO
    tox2Write32(base, attr + 4, 2);  // SPU queue
    CHECK(syscallAs(*a, 128, queue_id_out, attr, 0, 2) == 0);
    const std::uint32_t queue = tox2Read32(base, queue_id_out);

    // sc 251: req 0 is invalid, an unknown group or queue is ESRCH, the port is the lowest
    // requested bit.
    tox2::resetSpuThreadGroups();
    tox2::resetSpuImages();
    const std::uint32_t spu_scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(spu_scratch != 0);
    CHECK(syscallAs(*a, 251, 0x04000100, queue, 4, spup) == err(tox2::kCellEsrch));
    CHECK(buildSpuGroup(*a, base, spu_scratch, 1) == 0x04000100);
    tox2Write8(base, spup, 0xee);
    CHECK(syscallAs(*a, 251, 0x04000100, queue, 0, spup) == err(tox2::kCellEinval));
    CHECK(syscallAs(*a, 251, 0x04000100, 0x8d000fff, 4, spup) == err(tox2::kCellEsrch));
    CHECK(tox2Read8(base, spup) == 0xee);
    CHECK(syscallAs(*a, 251, 0x04000100, queue, 4, spup) == 0);
    CHECK(tox2Read8(base, spup) == 2);
    CHECK(syscallAs(*a, 251, 0x04000100, queue, 0x28, spup) == 0);
    CHECK(tox2Read8(base, spup) == 3);
    CHECK(syscallAs(*a, 251, 0x04000100, queue, std::uint64_t{1} << 63, spup) == 0);
    CHECK(tox2Read8(base, spup) == 63);
    CHECK(syscallAs(*a, 251, 0x04000100, queue, 4, 0) == err(tox2::kCellEfault));

    const std::string census = tox2::formatCensus();
    CHECK(census.find("sc 169 sys_spu_initialize: 3 [impl]") != std::string::npos);
    CHECK(census.find("sc 251 sys_spu_thread_group_connect_event_all_threads: 7 [impl]") != std::string::npos);

    // cellSysmodule: any id succeeds; the first load and unload of each id is logged once.
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x32267a31, "cellSysmodule", "cellSysmoduleLoadModule"},
            {0x00800010, 0x112a5ee9, "cellSysmodule", "cellSysmoduleUnloadModule"},
    };
    tox2::installImports(imports, 2);
    Tox2PpuContext* ctx = &a->context();
    for (int i = 0; i < 2; i++) {
        ctx->gpr[3] = 0xf;
        tox2HleCall(ctx, base, 0);
        CHECK(ctx->gpr[3] == 0);
    }
    ctx->gpr[3] = 0x10001;  // only the low 16 bits are the id
    tox2HleCall(ctx, base, 0);
    CHECK(ctx->gpr[3] == 0);
    ctx->gpr[3] = 0xf;
    tox2HleCall(ctx, base, 1);
    CHECK(ctx->gpr[3] == 0);
    CHECK(countContaining(lines, "tox2: sysmodule: load 0x000f") == 1);
    CHECK(countContaining(lines, "tox2: sysmodule: load 0x0001") == 1);
    CHECK(countContaining(lines, "tox2: sysmodule: unload 0x000f") == 1);
    CHECK(countContaining(lines, "tox2: default hle") == 0);
    CHECK(tox2::formatCensus().find("cellSysmodule::cellSysmoduleLoadModule: 3 [impl]") != std::string::npos);

    tox2::resetSpuThreadGroups();
    tox2::resetSpuImages();
    p.main.free(spu_scratch);
    p.main.free(scratch);
    tox2::resetSyncObjects();
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

// In-memory command buffer: words are keyed by IO offset, unlisted offsets are unmapped.
class FakeSink : public tox2::RsxSink {
public:
    bool readIo(std::uint32_t io, std::uint32_t& word) override {
        read_io_calls++;
        const auto it = words.find(io);
        if (it == words.end()) return false;
        word = it->second;
        return true;
    }
    bool write(std::uint32_t method, std::uint32_t value) override {
        ++attempts;
        if (refuse > 0) {
            --refuse;
            return false;
        }
        writes.emplace_back(method, value);
        stop_after_write_ = writes.back() == stop_on;
        return true;
    }
    const std::uint8_t* ioSpan(std::uint32_t io, std::uint32_t& bytes) override {
        if (!use_spans) return nullptr;
        const auto it = words.find(io);
        if (it == words.end()) return nullptr;
        // Contiguous words from `io`, each as big-endian bytes in a buffer rebuilt per call.
        span_buffer.clear();
        std::uint32_t at = io;
        for (auto word = it; word != words.end() && word->first == at && span_buffer.size() < span_limit * 4;
             ++word, at += 4) {
            for (int shift = 24; shift >= 0; shift -= 8) {
                span_buffer.push_back(static_cast<std::uint8_t>(word->second >> shift));
            }
        }
        bytes = static_cast<std::uint32_t>(span_buffer.size());
        return span_buffer.data();
    }
    void put(std::uint32_t at, std::initializer_list<std::uint32_t> list) {
        for (const std::uint32_t word : list) {
            words[at] = word;
            at += 4;
        }
    }

    std::map<std::uint32_t, std::uint32_t> words;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> writes;
    int refuse = 0;  // the next writes return false
    int attempts = 0;
    std::pair<std::uint32_t, std::uint32_t> stop_on{0xFFFFFFFF, 0};  // the run stops after this write
    bool use_spans = false;                                          // offer ioSpan() instead of readIo() only
    int read_io_calls = 0;                                           // readIo() calls, to show the span path ran
    std::size_t span_limit = 1000;                                   // words per span, to cross span ends
    std::vector<std::uint8_t> span_buffer;
};

using Writes = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

void testParamSfo() {
    tox2::ParamSfo sfo;
    sfo.ints["PARENTAL_LEVEL"] = 5;
    sfo.ints["RESOLUTION"] = 0x1d;
    sfo.strings["CATEGORY"] = "DG";
    sfo.strings["TITLE_ID"] = "BLJS10188";
    sfo.strings["TITLE"] = "Sample title";
    const std::vector<std::uint8_t> bytes = tox2::serializeParamSfo(sfo);
    CHECK(bytes.size() > 20 && bytes[0] == 0 && bytes[1] == 'P' && bytes[2] == 'S' && bytes[3] == 'F');
    const auto parsed = tox2::parseParamSfo(bytes);
    CHECK(parsed && parsed->ints == sfo.ints && parsed->strings == sfo.strings);
    CHECK(parsed && parsed->ints.find("SOUND_FORMAT") == parsed->ints.end());  // missing key

    std::vector<std::uint8_t> bad = bytes;
    bad[1] = 'X';
    CHECK(!tox2::parseParamSfo(bad));
    CHECK(!tox2::parseParamSfo(std::span<const std::uint8_t>(bytes.data(), 10)));
    CHECK(!tox2::parseParamSfo(std::span<const std::uint8_t>(bytes.data(), bytes.size() - 8)));

    // A raw-format (0x0004) string that fills its length without a terminator.
    const std::vector<std::uint8_t> raw = {0, 'P', 'S', 'F', 1, 1, 0,   0, 36, 0, 0,   0,   40,  0,  0,
                                           0, 1,   0,   0,   0, 0, 0,   4, 0,  4, 0,   0,   0,   4,  0,
                                           0, 0,   0,   0,   0, 0, 'K', 0, 0,  0, 'a', 'b', 'c', 'd'};
    const auto raw_parsed = tox2::parseParamSfo(raw);
    CHECK(raw_parsed && raw_parsed->strings.at("K") == "abcd");
}

void testCellGame() {
    namespace stdfs = std::filesystem;
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetCensus();
    tox2::resetLogOnce();
    tox2::resetCellGame();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t name = scratch, size = scratch + 0x100, info = scratch + 0x200, usrdir = scratch + 0x300,
                        init = scratch + 0x400;

    const stdfs::path root = stdfs::temp_directory_path() / ("tox2_game_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    stdfs::remove_all(root, ec);
    const stdfs::path game = root / "game";
    stdfs::create_directories(game / "PS3_GAME" / "USRDIR");
    tox2::ParamSfo disc;
    disc.strings["CATEGORY"] = "DG";
    disc.strings["TITLE_ID"] = "BLJS10188";
    {
        const auto bytes = tox2::serializeParamSfo(disc);
        std::ofstream(game / "PS3_GAME" / "PARAM.SFO", std::ios::binary)
                .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    tox2::configureVfs(game, root / "hdd");

    enum Import : std::uint32_t { Create, Permit, ErrorDialog, DataCheck, BootCheck, ImportCount };
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x42a2e133, "cellGame", "cellGameCreateGameData"},
            {0x00800010, 0x70acec67, "cellGame", "cellGameContentPermit"},
            {0x00800020, 0xb0a1f8c6, "cellGame", "cellGameContentErrorDialog"},
            {0x00800030, 0xdb9819f3, "cellGame", "cellGameDataCheck"},
            {0x00800040, 0xf52639ea, "cellGame", "cellGameBootCheck"},
    };
    tox2::installImports(imports, ImportCount);
    auto ctx = std::make_unique<Tox2PpuContext>();
    auto call = [&](Import which, std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d = 0) {
        ctx->gpr[3] = a;
        ctx->gpr[4] = b;
        ctx->gpr[5] = c;
        ctx->gpr[6] = d;
        tox2HleCall(ctx.get(), base, which);
        return static_cast<std::uint32_t>(ctx->gpr[3]);
    };
    auto put = [&](std::uint32_t addr, std::string_view text) {
        std::memcpy(base + addr, text.data(), text.size());
        base[addr + text.size()] = 0;
    };
    auto get = [&](std::uint32_t addr) { return std::string(reinterpret_cast<const char*>(base + addr)); };
    constexpr std::uint32_t kParam = 0x8002cb07, kFailure = 0x8002cb22, kBusy = 0x8002cb23, kBroken = 0x8002cb05,
                            kExist = 0x8002cb26;

    // Disc: the content directory is the disc itself.
    CHECK(call(DataCheck, 1, 0, size) == 0);
    CHECK(tox2Read32(base, size) == 40 * 1024 * 1024 - 256);
    CHECK(static_cast<std::int32_t>(tox2Read32(base, size + 4)) == -1 && tox2Read32(base, size + 8) == 0);
    CHECK(call(Permit, info, usrdir, 0) == 0);
    CHECK(get(info) == "/dev_bdvd/PS3_GAME" && get(usrdir) == "/dev_bdvd/PS3_GAME/USRDIR");
    CHECK(call(Permit, info, usrdir, 0) == kFailure);  // no outstanding check

    // Disc boot check: type, attributes, size and the empty directory name, then the permit returns the
    // disc directories. The out-parameters are optional.
    const std::uint32_t type = scratch + 0x500, attrs = scratch + 0x510, dir_name = scratch + 0x520;
    tox2Write32(base, type, 0xDEADBEEF);
    tox2Write32(base, attrs, 0xDEADBEEF);
    put(dir_name, "junk");
    CHECK(call(BootCheck, type, attrs, size, dir_name) == 0);
    CHECK(tox2Read32(base, type) == 1 && tox2Read32(base, attrs) == 0 && get(dir_name).empty());
    CHECK(tox2Read32(base, size) == 40 * 1024 * 1024 - 256);
    CHECK(static_cast<std::int32_t>(tox2Read32(base, size + 4)) == -1 && tox2Read32(base, size + 8) == 0);
    CHECK(call(BootCheck, type, attrs, size, dir_name) == kBusy);  // the permit is still outstanding
    CHECK(call(Permit, info, usrdir, 0) == 0);
    CHECK(get(info) == "/dev_bdvd/PS3_GAME" && get(usrdir) == "/dev_bdvd/PS3_GAME/USRDIR");
    CHECK(call(BootCheck, 0, 0, 0, 0) == 0);
    CHECK(call(Permit, info, usrdir, 0) == 0 && get(usrdir) == "/dev_bdvd/PS3_GAME/USRDIR");
    CHECK(call(Permit, info, usrdir, 0) == kFailure);

    // Argument validation and the busy state.
    CHECK(call(DataCheck, 0, name, 0) == kParam);
    CHECK(call(DataCheck, 4, name, 0) == kParam);
    CHECK(call(DataCheck, 3, 0, 0) == kParam);
    CHECK(call(DataCheck, 1, 0, 0) == 0);
    CHECK(call(DataCheck, 1, 0, 0) == kBusy);
    CHECK(call(Permit, 0, usrdir, 0) == kParam);
    CHECK(call(Permit, info, usrdir, 0) == 0);

    // Game data that does not exist yet: check, create, permit moves it into place.
    put(name, "TESTDATA");
    CHECK(call(DataCheck, 3, name, size) == 2);
    CHECK(tox2Read32(base, size + 4) == 0);
    put(init + 0, "My title");
    put(init + 128, "TESTID001");
    put(init + 140, "01.00");
    CHECK(call(Create, init, info, usrdir) == 0);
    const std::string temp_dir = get(info);
    CHECK(temp_dir.starts_with("/dev_hdd0/game/_GDATA_") && get(usrdir) == temp_dir + "/USRDIR");
    const stdfs::path hdd_game = root / "hdd" / "dev_hdd0" / "game";
    CHECK(stdfs::is_directory(hdd_game / temp_dir.substr(15) / "USRDIR"));
    CHECK(call(Permit, info, usrdir, 0) == 0);
    CHECK(get(info) == "/dev_hdd0/game/TESTDATA" && get(usrdir) == "/dev_hdd0/game/TESTDATA/USRDIR");
    CHECK(stdfs::is_directory(hdd_game / "TESTDATA" / "USRDIR"));
    CHECK(!stdfs::exists(hdd_game / temp_dir.substr(15)));
    const auto written = tox2::readParamSfoFile(hdd_game / "TESTDATA" / "PARAM.SFO");
    CHECK(written && written->strings.at("CATEGORY") == "GD" && written->strings.at("TITLE") == "My title" &&
          written->strings.at("TITLE_ID") == "TESTID001" && written->strings.at("VERSION") == "01.00");

    // The data now exists: no creation, a category mismatch is reported as broken.
    CHECK(call(DataCheck, 3, name, size) == 0);
    CHECK(static_cast<std::int32_t>(tox2Read32(base, size + 4)) == -1);
    CHECK(call(Create, init, info, usrdir) == kExist);
    CHECK(call(Permit, info, usrdir, 0) == 0);
    CHECK(get(usrdir) == "/dev_hdd0/game/TESTDATA/USRDIR");
    CHECK(call(DataCheck, 2, name, 0) == kBroken);
    CHECK(call(DataCheck, 3, name, 0) == 0);  // the broken check left no outstanding state
    CHECK(call(Permit, info, usrdir, 0) == 0);

    // CreateGameData needs an outstanding game data check.
    CHECK(call(Create, init, info, usrdir) == kFailure);
    CHECK(call(Create, 0, info, usrdir) == kParam);

    // Permit for a missing directory returns empty paths.
    put(name, "NOSUCHDIR");
    CHECK(call(DataCheck, 2, name, 0) == 2);
    CHECK(call(Permit, info, usrdir, 0) == 0);
    CHECK(get(info).empty() && get(usrdir).empty());

    // Error dialog: only the continuing types return.
    CHECK(call(ErrorDialog, 0, 0, 0) == 0);
    CHECK(call(ErrorDialog, 2, 1024, name) == 0);
    CHECK(call(ErrorDialog, 7, 0, 0) == kParam);

    tox2::resetCellGame();
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
    stdfs::remove_all(root, ec);
}

void testCellRtc() {
    tox2::resetCensus();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t tick = scratch, local = scratch + 0x10, date = scratch + 0x20;

    enum Import : std::uint32_t { SetTick, GetCurrentTick, GetDayOfWeek, ConvertUtcToLocalTime, ImportCount };
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x99b13034, "cellRtc", "cellRtcSetTick"},
            {0x00800010, 0x9dafc0d9, "cellRtc", "cellRtcGetCurrentTick"},
            {0x00800020, 0xc2d8cf95, "cellRtc", "cellRtcGetDayOfWeek"},
            {0x00800030, 0xc48d5002, "cellRtc", "cellRtcConvertUtcToLocalTime"},
    };
    tox2::installImports(imports, ImportCount);
    auto ctx = std::make_unique<Tox2PpuContext>();
    auto call = [&](Import which, std::uint64_t a, std::uint64_t b = 0, std::uint64_t c = 0) {
        ctx->gpr[3] = a;
        ctx->gpr[4] = b;
        ctx->gpr[5] = c;
        tox2HleCall(ctx.get(), base, which);
        return static_cast<std::uint32_t>(ctx->gpr[3]);
    };
    constexpr std::uint32_t kInvalidPointer = 0x80010602;
    constexpr std::uint64_t kUnixEpoch = 62135596800000000ull;
    const std::uint32_t unmapped = 0x70000000;
    CHECK(!p.memory.isCommitted(unmapped, 16));

    // GetCurrentTick: host UTC time on the 0001-01-01 epoch.
    const auto host_now = [] {
        const auto since = std::chrono::system_clock::now().time_since_epoch();
        return kUnixEpoch +
               static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(since).count());
    };
    const std::uint64_t before = host_now();
    CHECK(call(GetCurrentTick, tick) == 0);
    const std::uint64_t after = host_now();
    CHECK(tox2Read64(base, tick) >= before && tox2Read64(base, tick) <= after + 1000000);
    CHECK(call(GetCurrentTick, 0) == kInvalidPointer);
    CHECK(call(GetCurrentTick, unmapped) == kInvalidPointer);

    // ConvertUtcToLocalTime adds the offset in minutes, in either direction.
    const std::uint64_t utc = 63000000000000000ull;
    tox2Write64(base, tick, utc);
    tox2::setRtcLocalOffsetMinutes(540);  // UTC+9
    CHECK(call(ConvertUtcToLocalTime, tick, local) == 0);
    CHECK(tox2Read64(base, local) == utc + 540ull * 60 * 1000000);
    tox2::setRtcLocalOffsetMinutes(-330);
    CHECK(call(ConvertUtcToLocalTime, tick, tick) == 0);  // in place
    CHECK(tox2Read64(base, tick) == utc - 330ull * 60 * 1000000);
    tox2::setRtcLocalOffsetMinutes(std::nullopt);  // the host time zone: whole minutes, within a day
    tox2Write64(base, tick, utc);
    CHECK(call(ConvertUtcToLocalTime, tick, local) == 0);
    const auto host_delta = static_cast<std::int64_t>(tox2Read64(base, local) - utc);
    CHECK(host_delta % (60 * 1000000) == 0 && host_delta > -24ll * 3600 * 1000000 &&
          host_delta < 24ll * 3600 * 1000000);
    tox2Write64(base, local, 0x1122334455667788ull);
    CHECK(call(ConvertUtcToLocalTime, 0, local) == kInvalidPointer);
    CHECK(call(ConvertUtcToLocalTime, tick, 0) == kInvalidPointer);
    CHECK(call(ConvertUtcToLocalTime, unmapped, local) == kInvalidPointer);
    CHECK(call(ConvertUtcToLocalTime, tick, unmapped) == kInvalidPointer);
    CHECK(tox2Read64(base, local) == 0x1122334455667788ull);  // untouched on error

    // SetTick: 2026-10-12 12:34:56.789 UTC is 1791808496 s after 1970-01-01.
    const std::uint64_t known = (kUnixEpoch / 1000000 + 1791808496ull) * 1000000 + 789000;
    tox2Write64(base, tick, known);
    CHECK(call(SetTick, date, tick) == 0);
    CHECK(tox2Read16(base, date) == 2026 && tox2Read16(base, date + 2) == 10 && tox2Read16(base, date + 4) == 12);
    CHECK(tox2Read16(base, date + 6) == 12 && tox2Read16(base, date + 8) == 34 && tox2Read16(base, date + 10) == 56);
    CHECK(tox2Read32(base, date + 12) == 789000);
    CHECK(call(GetDayOfWeek, 2026, 10, 12) == 1);  // a Monday

    // Edges: the epoch, the Unix epoch, a leap day and the last representable second of 9999.
    struct Case {
        std::uint64_t tick;
        std::uint16_t year, month, day, hour, minute, second;
        std::uint32_t micro;
    };
    const Case cases[] = {
            {0, 1, 1, 1, 0, 0, 0, 0},
            {kUnixEpoch, 1970, 1, 1, 0, 0, 0, 0},
            {63082281600000000ull, 2000, 1, 1, 0, 0, 0, 0},
            {63082281600000000ull + (31 + 28) * 86400ull * 1000000, 2000, 2, 29, 0, 0, 0, 0},
            {63082281600000000ull + 366 * 86400ull * 1000000 - 1, 2000, 12, 31, 23, 59, 59, 999999},
            {66238041599999999ull, 2099, 12, 31, 23, 59, 59, 999999},
            {kUnixEpoch + 951782400ull * 1000000, 2000, 2, 29, 0, 0, 0, 0},
            {kUnixEpoch + 4107542399ull * 1000000, 2100, 2, 28, 23, 59, 59, 0},  // 2100 is not a leap year
            {315537897599999999ull, 9999, 12, 31, 23, 59, 59, 999999},
    };
    for (const Case& c : cases) {
        tox2Write64(base, tick, c.tick);
        CHECK(call(SetTick, date, tick) == 0);
        CHECK(tox2Read16(base, date) == c.year && tox2Read16(base, date + 2) == c.month &&
              tox2Read16(base, date + 4) == c.day && tox2Read16(base, date + 6) == c.hour &&
              tox2Read16(base, date + 8) == c.minute && tox2Read16(base, date + 10) == c.second &&
              tox2Read32(base, date + 12) == c.micro);
    }
    CHECK(call(SetTick, 0, tick) == kInvalidPointer);
    CHECK(call(SetTick, date, 0) == kInvalidPointer);
    CHECK(call(SetTick, unmapped, tick) == kInvalidPointer);
    CHECK(call(SetTick, date, unmapped) == kInvalidPointer);

    // GetDayOfWeek: 0 = Sunday; January and February use the previous year.
    CHECK(call(GetDayOfWeek, 1, 1, 1) == 1);       // 0001-01-01 is a Monday
    CHECK(call(GetDayOfWeek, 1970, 1, 1) == 4);    // Thursday
    CHECK(call(GetDayOfWeek, 2000, 1, 1) == 6);    // Saturday
    CHECK(call(GetDayOfWeek, 2000, 2, 29) == 2);   // Tuesday
    CHECK(call(GetDayOfWeek, 2000, 3, 1) == 3);    // Wednesday
    CHECK(call(GetDayOfWeek, 2024, 2, 29) == 4);   // Thursday
    CHECK(call(GetDayOfWeek, 2023, 12, 31) == 0);  // Sunday
    CHECK(call(GetDayOfWeek, 2100, 3, 1) == 1);    // Monday
    CHECK(call(GetDayOfWeek, 9999, 12, 31) == 5);  // Friday

    tox2::installImports(nullptr, 0);
    p.main.free(scratch);
}

void testCellFs() {
    namespace stdfs = std::filesystem;
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetCensus();
    tox2::resetLogOnce();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t path_a = scratch, buf = scratch + 0x2000, out = scratch + 0x3000, sb = scratch + 0x3100,
                        arg = scratch + 0x3200;

    const stdfs::path root =
            stdfs::temp_directory_path() / ("tox2_cellfs_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    stdfs::remove_all(root, ec);
    const stdfs::path game = root / "game";
    stdfs::create_directories(game / "sub");
    std::ofstream(game / "b.txt", std::ios::binary) << "0123456789";
    tox2::configureVfs(game, root / "hdd");

    enum Import : std::uint32_t {
        Read,
        Close,
        Open,
        Stat,
        Unlink,
        Write,
        SdataOpen,
        Mkdir,
        Fstat,
        SeqRead,
        Lseek,
        SeqWrite,
        ImportCount
    };
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x0d5b4a14, "sys_fs", "cellFsReadWithOffset"},
            {0x00800010, 0x2cb51f0d, "sys_fs", "cellFsClose"},
            {0x00800020, 0x718bf5f8, "sys_fs", "cellFsOpen"},
            {0x00800030, 0x7de6dced, "sys_fs", "cellFsStat"},
            {0x00800040, 0x7f4677a8, "sys_fs", "cellFsUnlink"},
            {0x00800050, 0x8cb722d5, "sys_fs", "cellFsWriteWithOffset"},
            {0x00800060, 0xb1840b53, "sys_fs", "cellFsSdataOpen"},
            {0x00800070, 0xba901fe6, "sys_fs", "cellFsMkdir"},
            {0x00800080, 0xef3efa34, "sys_fs", "cellFsFstat"},
            {0x00800090, 0x4d5ff8e2, "sys_fs", "cellFsRead"},
            {0x008000a0, 0xa397d042, "sys_fs", "cellFsLseek"},
            {0x008000b0, 0xecdcf2ab, "sys_fs", "cellFsWrite"},
    };
    tox2::installImports(imports, ImportCount);
    auto ctx = std::make_unique<Tox2PpuContext>();
    auto call = [&](Import which, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0, std::uint64_t d = 0,
                    std::uint64_t e = 0) {
        ctx->gpr[3] = a;
        ctx->gpr[4] = b;
        ctx->gpr[5] = c;
        ctx->gpr[6] = d;
        ctx->gpr[7] = e;
        tox2HleCall(ctx.get(), base, which);
        return ctx->gpr[3];
    };
    auto put = [&](std::uint32_t addr, std::string_view text) {
        std::memcpy(base + addr, text.data(), text.size());
        base[addr + text.size()] = 0;
    };
    auto hostRead = [](const stdfs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };

    // Open, positional read, close. The file position is not moved by ReadWithOffset.
    put(path_a, "/dev_bdvd/b.txt");
    CHECK(call(Open, path_a, 0, out, 0, 0) == 0);
    const std::uint32_t fd = tox2Read32(base, out);
    CHECK(fd == 3);
    CHECK(call(Open, path_a, 0, 0, 0, 0) == err(tox2::kCellEfault));
    CHECK(call(Read, fd, 6, buf, 3, out) == 0 && tox2Read64(base, out) == 3);
    CHECK(std::memcmp(base + buf, "678", 3) == 0);
    CHECK(call(Read, fd, 8, buf, 100, out) == 0 && tox2Read64(base, out) == 2);  // short at the end
    CHECK(call(Read, fd, 50, buf, 4, out) == 0 && tox2Read64(base, out) == 0);   // past the end
    CHECK(call(Read, fd, 0, buf, 2, 0) == 0);                                    // nread is optional
    CHECK(call(Read, fd, 0, 0, 2, out) == err(tox2::kCellEfault) && tox2Read64(base, out) == 0);
    // sys_fs_read continues from the start: the position never moved.
    CHECK(syscall(802, fd, buf, 4, out) == 0 && std::memcmp(base + buf, "0123", 4) == 0);
    CHECK(call(Read, fd, 0, buf, 1, out) == 0 && syscall(802, fd, buf + 8, 2, out) == 0);
    CHECK(std::memcmp(base + buf + 8, "45", 2) == 0);
    CHECK(call(Read, 99, 0, buf, 1, out) == err(tox2::kCellEbadf) && tox2Read64(base, out) == 0);
    CHECK(call(Read, 0, 0, buf, 1, out) == err(tox2::kCellEbadf));
    CHECK(call(Write, fd, 0, buf, 1, out) == err(tox2::kCellEbadf));  // opened read-only

    // Sequential cellFsRead and cellFsLseek move the file position and agree with the syscalls.
    CHECK(call(Lseek, fd, 0, 0, out) == 0 && tox2Read64(base, out) == 0);  // earlier syscalls moved it to 6
    CHECK(call(SeqRead, fd, buf, 4, out) == 0 && tox2Read64(base, out) == 4 && std::memcmp(base + buf, "0123", 4) == 0);
    CHECK(call(Lseek, fd, 0, 1, out) == 0 && tox2Read64(base, out) == 4);                               // SEEK_CUR
    CHECK(call(Lseek, fd, static_cast<std::uint64_t>(-3), 2, out) == 0 && tox2Read64(base, out) == 7);  // SEEK_END
    CHECK(call(SeqRead, fd, buf, 100, out) == 0 && tox2Read64(base, out) == 3 &&
          std::memcmp(base + buf, "789", 3) == 0);
    CHECK(call(SeqRead, fd, buf, 4, out) == 0 && tox2Read64(base, out) == 0);  // at the end
    CHECK(call(Lseek, fd, 2, 0, out) == 0 && tox2Read64(base, out) == 2);
    CHECK(syscall(802, fd, buf, 2, out) == 0 && std::memcmp(base + buf, "23", 2) == 0);
    // Offsets and positions above 2 GiB and 4 GiB are kept in full; a read there finds nothing.
    CHECK(call(Lseek, fd, 0xC0000005ull, 0, out) == 0 && tox2Read64(base, out) == 0xC0000005ull);
    CHECK(call(Lseek, fd, 0x1'0000'0000ull + 7, 0, out) == 0 && tox2Read64(base, out) == 0x1'0000'0007ull);
    CHECK(call(SeqRead, fd, buf, 4, out) == 0 && tox2Read64(base, out) == 0);
    CHECK(call(Lseek, fd, static_cast<std::uint64_t>(-1), 0, out) != 0);  // before the start
    CHECK(call(Lseek, fd, 0, 3, out) == err(tox2::kCellEinval));
    CHECK(call(Lseek, fd, 0, 0, 0) == err(tox2::kCellEfault));
    CHECK(call(Lseek, 99, 0, 0, out) == err(tox2::kCellEbadf));
    CHECK(call(SeqRead, fd, 0, 1, out) == err(tox2::kCellEfault) && tox2Read64(base, out) == 0);
    CHECK(call(SeqRead, fd, buf, 1, 0) == err(tox2::kCellEfault));
    CHECK(call(SeqRead, 99, buf, 1, out) == err(tox2::kCellEbadf));
    CHECK(call(SeqWrite, fd, buf, 1, out) == err(tox2::kCellEbadf));  // opened read-only

    // Stat of a file and of a directory, fstat of the open file.
    CHECK(call(Stat, path_a, sb) == 0 && tox2Read32(base, sb) == 0100444 && tox2Read64(base, sb + 36) == 10);
    CHECK(call(Stat, path_a, 0) == err(tox2::kCellEfault));
    put(path_a, "/dev_bdvd/sub");
    CHECK(call(Stat, path_a, sb) == 0 && tox2Read32(base, sb) == 040555);
    put(path_a, "/dev_bdvd/none");
    CHECK(call(Stat, path_a, sb) == err(tox2::kCellEnoent));
    CHECK(call(Fstat, fd, sb) == 0 && tox2Read32(base, sb) == 0100444 && tox2Read64(base, sb + 36) == 10);
    CHECK(call(Fstat, 99, sb) == err(tox2::kCellEbadf));
    CHECK(call(Close, fd) == 0);
    CHECK(call(Close, fd) == err(tox2::kCellEbadf));

    // Mkdir, positional write and Unlink under the hdd directory.
    put(path_a, "/dev_hdd0/tmp/d");
    CHECK(call(Mkdir, path_a, 0777) == 0);
    CHECK(call(Mkdir, path_a, 0777) == err(tox2::kCellEexist));
    CHECK(stdfs::is_directory(root / "hdd" / "dev_hdd0" / "tmp" / "d"));
    put(path_a, "/dev_hdd0/tmp/d/f.bin");
    CHECK(call(Open, path_a, 0102, out, 0, 0) == 0);  // RDWR | CREAT
    const std::uint32_t wfd = tox2Read32(base, out);
    put(buf, "abcdef");
    CHECK(call(Write, wfd, 0, buf, 6, out) == 0 && tox2Read64(base, out) == 6);
    put(buf + 0x10, "XY");
    CHECK(call(Write, wfd, 2, buf + 0x10, 2, out) == 0 && tox2Read64(base, out) == 2);
    CHECK(call(Write, wfd, 0, 0, 2, out) == err(tox2::kCellEfault) && tox2Read64(base, out) == 0);
    // The file position stayed at 0, so a plain write lands at the start.
    put(buf + 0x20, "Z");
    CHECK(syscall(803, wfd, buf + 0x20, 1, out) == 0);
    CHECK(call(Fstat, wfd, sb) == 0 && tox2Read64(base, sb + 36) == 6);
    // Sequential cellFsWrite writes at the current position, which cellFsLseek moves.
    CHECK(call(Lseek, wfd, 0, 2, out) == 0 && tox2Read64(base, out) == 6);
    put(buf + 0x30, "!?");
    CHECK(call(SeqWrite, wfd, buf + 0x30, 2, out) == 0 && tox2Read64(base, out) == 2);
    CHECK(call(Lseek, wfd, 1, 0, out) == 0 && tox2Read64(base, out) == 1);
    CHECK(call(SeqWrite, wfd, buf + 0x30, 1, out) == 0 && tox2Read64(base, out) == 1);
    CHECK(call(SeqWrite, wfd, 0, 1, out) == err(tox2::kCellEfault) && tox2Read64(base, out) == 0);
    CHECK(call(SeqWrite, wfd, buf, 1, 0) == err(tox2::kCellEfault));
    CHECK(call(SeqWrite, 99, buf, 1, out) == err(tox2::kCellEbadf));
    CHECK(call(Close, wfd) == 0);
    CHECK(call(Lseek, wfd, 0, 0, out) == err(tox2::kCellEbadf));
    CHECK(hostRead(root / "hdd" / "dev_hdd0" / "tmp" / "d" / "f.bin") == "Z!XYef!?");
    CHECK(call(Unlink, path_a) == 0);
    CHECK(call(Unlink, path_a) == err(tox2::kCellEnoent));
    CHECK(!stdfs::exists(root / "hdd" / "dev_hdd0" / "tmp" / "d" / "f.bin"));
    put(path_a, "/dev_bdvd/b.txt");
    CHECK(call(Unlink, path_a) == err(tox2::kCellErofs));
    CHECK(call(Write, 3, 0, buf, 1, out) == err(tox2::kCellEbadf));

    // cellFsSdataOpen: argument validation, then ENOENT with nothing written to *fd.
    tox2Write32(base, out, 0xDEADBEEF);
    CHECK(call(SdataOpen, path_a, 1, out) == err(tox2::kCellEinval));
    CHECK(call(SdataOpen, path_a, 0, 0) == err(tox2::kCellEfault));
    CHECK(call(SdataOpen, 0, 0, out) == err(tox2::kCellEfault));
    CHECK(call(SdataOpen, path_a, 0, out) == err(tox2::kCellEnoent));
    CHECK(call(SdataOpen, path_a, 0, out) == err(tox2::kCellEnoent));
    CHECK(tox2Read32(base, out) == 0xDEADBEEF);
    int sdata_lines = 0;
    for (const std::string& line : lines) {
        sdata_lines += line.find("sdata open not supported: /dev_bdvd/b.txt") != std::string::npos;
    }
    CHECK(sdata_lines == 1);  // logged once per path
    // A plain open through the sdata argument (0x180, 0x10) takes the same route.
    tox2Write32(base, arg, 0x180);
    tox2Write32(base, arg + 4, 0x10);
    CHECK(call(Open, path_a, 0, out, arg, 8) == err(tox2::kCellEnoent));
    CHECK(tox2Read32(base, out) == 0xDEADBEEF);

    // With a cache directory the decrypted copy at the same relative path is served read-only.
    stdfs::create_directories(root / "cache");
    std::ofstream(root / "cache" / "b.txt", std::ios::binary) << "decrypted!";
    tox2::configureSdataCache(root / "cache");
    CHECK(call(SdataOpen, path_a, 0, out) == 0);
    const std::uint32_t sfd = tox2Read32(base, out);
    CHECK(sfd != 0xDEADBEEF);
    CHECK(call(Fstat, sfd, sb) == 0 && tox2Read64(base, sb + 36) == 10 && tox2Read32(base, sb) == 0100444);
    CHECK(call(Read, sfd, 2, buf, 4, out) == 0 && tox2Read64(base, out) == 4 &&
          std::memcmp(base + buf, "cryp", 4) == 0);
    CHECK(call(Write, sfd, 0, buf, 1, out) == err(tox2::kCellEbadf));
    CHECK(call(Close, sfd) == 0);
    CHECK(call(Open, path_a, 0, out, arg, 8) == 0);  // the sdata marker takes the same route
    CHECK(call(Close, tox2Read32(base, out)) == 0);
    put(path_a, "/dev_bdvd/sub/nothing.sdat");
    tox2Write32(base, out, 0xDEADBEEF);
    CHECK(call(SdataOpen, path_a, 0, out) == err(tox2::kCellEnoent) && tox2Read32(base, out) == 0xDEADBEEF);
    put(path_a, "/dev_hdd0/tmp/x.sdat");  // only the game directory is mapped
    CHECK(call(SdataOpen, path_a, 0, out) == err(tox2::kCellEnoent));
    tox2::configureSdataCache(std::nullopt);

    p.main.free(scratch);
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
    stdfs::remove_all(root, ec);
    tox2::configureVfs({}, std::nullopt);
}

// Parsing, composition and installation of the VFS configuration file, and file overrides through
// the guest-facing open calls.
void testVfsConfig() {
    namespace stdfs = std::filesystem;
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetCensus();
    tox2::resetLogOnce();

    const stdfs::path root =
            stdfs::temp_directory_path() / ("tox2_vfs_config_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    stdfs::remove_all(root, ec);
    auto hostFile = [](const stdfs::path& path, std::string_view data) {
        stdfs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << data;
    };
    const stdfs::path cfg = root / "cfg";
    hostFile(cfg / "game" / "PS3_GAME" / "USRDIR" / "Exi.sdat", "encrypted");
    hostFile(cfg / "game" / "b.txt", "0123456789");
    hostFile(cfg / "plain" / "Exi.bin", "plain-bytes");
    hostFile(cfg / "plain" / "other.txt", "other");
    hostFile(root / "cache" / "PS3_GAME" / "USRDIR" / "Exi.sdat", "from-cache");

    // A full file: relative host paths resolve against the base directory.
    const auto full = tox2::parseVfsConfig(R"({
    "version": 1,
    "game_dir": "game",
    "hdd_dir": "hdd",
    "sdata_cache": "../cache",
    "mounts": {"/dev_usb000": {"host": "usb", "block_size": 4096},
               "/dev_hdd2": {"host": "hdd2", "read_only": true}},
    "files": {"/dev_bdvd/PS3_GAME/USRDIR/Exi.sdat": "plain/Exi.bin",
              "/dev_hdd0/tmp/w.bin": {"host": "plain/w.bin", "read_only": false},
              "/dev_bdvd/other.txt": {"host": "plain/other.txt"}}
  })",
                                           cfg);
    CHECK(full.has_value());
    if (full) {
        CHECK(full->warnings.empty());
        CHECK(full->mounts.size() == 6);
        auto find = [&](std::string_view name) -> const tox2::VfsMount* {
            for (const tox2::VfsMount& m : full->mounts) {
                if (m.name == name) return &m;
            }
            return nullptr;
        };
        const tox2::VfsMount* bdvd = find("dev_bdvd");
        const tox2::VfsMount* app = find("app_home");
        const tox2::VfsMount* hdd0 = find("dev_hdd0");
        const tox2::VfsMount* usb = find("dev_usb000");
        const tox2::VfsMount* hdd2 = find("dev_hdd2");
        CHECK(bdvd && bdvd->read_only && bdvd->host == cfg / "game");
        CHECK(app && app->read_only && app->host == cfg / "game" / "PS3_GAME" / "USRDIR");
        CHECK(hdd0 && !hdd0->read_only && hdd0->host == cfg / "hdd" / "dev_hdd0");
        CHECK(usb && !usb->read_only && usb->block_size == 4096u && usb->host == cfg / "usb");
        CHECK(hdd2 && hdd2->read_only && !hdd2->block_size);
        CHECK(full->sdata_cache == root / "cache");
        CHECK(full->files.size() == 3);
        for (const tox2::VfsFileOverride& f : full->files) {
            if (f.guest == "/dev_bdvd/PS3_GAME/USRDIR/Exi.sdat") {
                CHECK(f.read_only && f.host == cfg / "plain" / "Exi.bin");
            } else if (f.guest == "/dev_hdd0/tmp/w.bin") {
                CHECK(!f.read_only);
            }
        }
    }

    // Unknown keys warn and do not fail.
    const auto unknown = tox2::parseVfsConfig(R"({"version": 1, "colour": 1,
      "mounts": {"/dev_usb000": {"host": "usb", "mode": "x"}},
      "files": {"/dev_bdvd/a": {"host": "a", "extra": 1}}})",
                                              cfg);
    CHECK(unknown.has_value() && unknown->warnings.size() == 3);
    if (unknown) {
        CHECK(unknown->warnings[0].find("colour") != std::string::npos);
    }
    CHECK(tox2::parseVfsConfig(R"({"version": 1})", cfg).has_value());

    // Errors, each with a message that names the problem.
    auto expectError = [&](std::string_view text, std::string_view needle) {
        const auto parsed = tox2::parseVfsConfig(text, cfg);
        CHECK(!parsed.has_value());
        if (parsed) {
            std::printf("  expected an error containing \"%.*s\"\n", static_cast<int>(needle.size()), needle.data());
        } else {
            const bool found = parsed.error().find(needle) != std::string::npos;
            CHECK(found);
            if (!found) {
                std::printf("  error was: %s\n", parsed.error().c_str());
            }
        }
    };
    expectError("{", "invalid JSON");
    expectError("[]", "top level");
    expectError(R"({})", "\"version\" is missing");
    expectError(R"({"version": 2})", "unsupported");
    expectError(R"({"version": "1"})", "unsupported");
    expectError(R"({"version": 1, "mounts": []})", "\"mounts\" must be an object");
    expectError(R"({"version": 1, "mounts": {"dev_usb000": {"host": "x"}}})", "not absolute");
    expectError(R"({"version": 1, "mounts": {"/dev_usb000/": {"host": "x"}}})", "not normalised");
    expectError(R"({"version": 1, "mounts": {"/dev_usb000/../x": {"host": "x"}}})", "not normalised");
    expectError(R"({"version": 1, "mounts": {"/dev_hdd0/game": {"host": "x"}}})", "overlap");
    expectError(R"({"version": 1, "mounts": {"/dev_usb000": {}}})", "\"host\" is missing");
    expectError(R"({"version": 1, "mounts": {"/dev_usb000": {"host": ""}}})", "empty");
    expectError(R"({"version": 1, "mounts": {"/dev_usb000": {"host": "x", "read_only": 1}}})", "read_only");
    expectError(R"({"version": 1, "mounts": {"/dev_usb000": {"host": "x", "block_size": 1000}}})", "block_size");
    expectError(R"({"version": 1, "mounts": {"/dev_usb000": {"host": "x"}, "/dev_usb000": {"host": "y"}}})",
                "duplicate key");
    expectError(R"({"version": 1, "game_dir": "g", "mounts": {"/dev_bdvd": {"host": "x"}}})", "mounted twice");
    expectError(R"({"version": 1, "hdd_dir": "g", "mounts": {"/dev_hdd1": {"host": "x"}}})", "mounted twice");
    expectError(R"({"version": 1, "files": {"/dev_bdvd": "x"}})", "at least 2");
    expectError(R"({"version": 1, "files": {"/dev_bdvd/a/./b": "x"}})", "not normalised");
    expectError(R"({"version": 1, "files": {"dev_bdvd/a": "x"}})", "not absolute");
    expectError(R"({"version": 1, "files": {"/dev_bdvd/a": 3}})", "host path string");
    expectError(R"({"version": 1, "files": {"/dev_bdvd/a": "x", "/dev_bdvd/a/b": "y"}})", "below");
    expectError(R"({"version": 1, "sdata_cache": 4})", "path string");

    // Composition: command-line options win over the file, which wins over the defaults.
    const auto parsed_base = tox2::parseVfsConfig(R"({"version": 1, "game_dir": "game", "hdd_dir": "hdd",
      "sdata_cache": "../cache", "mounts": {"/dev_hdd2": {"host": "hdd2"}}})",
                                                  cfg);
    CHECK(parsed_base.has_value());
    const tox2::VfsConfigFile* file = parsed_base ? &*parsed_base : nullptr;
    auto hostOf = [](const tox2::VfsLayout& layout, std::string_view name) {
        for (const tox2::VfsMount& m : layout.mounts) {
            if (m.name == name) return m.host;
        }
        return stdfs::path();
    };
    const auto from_file = tox2::composeVfsLayout(file, {});
    CHECK(from_file.has_value());
    if (from_file) {
        CHECK(hostOf(*from_file, "dev_bdvd") == cfg / "game");
        CHECK(hostOf(*from_file, "dev_hdd1") == cfg / "hdd" / "dev_hdd1");
        CHECK(from_file->sdata_cache == root / "cache");
    }
    tox2::VfsCliOptions cli;
    cli.game_dir = cfg / "plain";
    cli.hdd_dir = root / "cli_hdd";
    cli.sdata_cache = root / "cli_cache";
    const auto merged = tox2::composeVfsLayout(file, cli);
    CHECK(merged.has_value());
    if (merged) {
        CHECK(merged->mounts.size() == 5);  // the options replaced four mounts, the extra one stays
        CHECK(hostOf(*merged, "dev_bdvd") == *cli.game_dir);
        CHECK(hostOf(*merged, "app_home") == *cli.game_dir / "PS3_GAME" / "USRDIR");
        CHECK(hostOf(*merged, "dev_hdd0") == *cli.hdd_dir / "dev_hdd0");
        CHECK(hostOf(*merged, "dev_hdd2") == cfg / "hdd2");
        CHECK(merged->sdata_cache == cli.sdata_cache);
    }
    // Without a file the options alone give the default layout.
    tox2::VfsCliOptions game_only;
    game_only.game_dir = cfg / "game";
    const auto only_cli = tox2::composeVfsLayout(nullptr, game_only);
    CHECK(only_cli.has_value() && only_cli->mounts.size() == 2 && !only_cli->sdata_cache);
    CHECK(tox2::composeVfsLayout(nullptr, {}).has_value());  // no mounts at all is valid here
    // A game directory without PS3_GAME/USRDIR still composes: the derived /app_home may be absent.
    CHECK(!stdfs::exists(cfg / "plain" / "PS3_GAME"));
    tox2::VfsCliOptions bare_game;
    bare_game.game_dir = cfg / "plain";
    CHECK(tox2::composeVfsLayout(nullptr, bare_game).has_value());

    // Host checks: a read-only mount and an override file must exist; writable mounts may not overlap.
    auto composeError = [&](std::string_view text, std::string_view needle) {
        const auto parsed = tox2::parseVfsConfig(text, cfg);
        CHECK(parsed.has_value());
        if (!parsed) return;
        const auto composed = tox2::composeVfsLayout(&*parsed, {});
        CHECK(!composed.has_value());
        CHECK(!composed && composed.error().find(needle) != std::string::npos);
    };
    composeError(R"({"version": 1, "game_dir": "missing"})", "does not exist");
    composeError(R"({"version": 1, "mounts": {"/app_home": {"host": "plain/none"}}})", "does not exist");
    composeError(R"({"version": 1, "files": {"/dev_bdvd/a": "plain/missing"}})", "does not exist");
    composeError(R"({"version": 1, "files": {"/dev_bdvd/a": "plain"}})", "not a regular file");
    composeError(R"({"version": 1, "files": {"/dev_bdvd/a": {"host": "nodir/f", "read_only": false}}})",
                 "does not exist");
    composeError(R"({"version": 1, "mounts": {"/dev_usb000": {"host": "usb"}, "/dev_usb001": {"host": "usb/inner"}}})",
                 "overlap");
    composeError(R"({"version": 1, "mounts": {"/dev_hdd0": {"host": "plain"}, "/dev_bdvd": {"host": "plain"}}})",
                 "overlap");
    // Two read-only mounts may share a directory.
    const auto shared = tox2::parseVfsConfig(
            R"({"version": 1, "mounts": {"/dev_bdvd": {"host": "game"}, "/app_home": {"host": "game/PS3_GAME/USRDIR"}}})",
            cfg);
    CHECK(shared && tox2::composeVfsLayout(&*shared, {}).has_value());

    // loadVfsConfig reads a file and resolves against its directory; errors name the file.
    hostFile(cfg / "vfs.json", R"({"version": 1, "game_dir": "game"})");
    const auto loaded = tox2::loadVfsConfig(cfg / "vfs.json");
    CHECK(loaded && loaded->mounts.size() == 2 && loaded->mounts[0].host == cfg / "game");
    const auto no_file = tox2::loadVfsConfig(cfg / "none.json");
    CHECK(!no_file && no_file.error().find("none.json") != std::string::npos);
    hostFile(cfg / "bad.json", R"({"version": 9})");
    const auto bad_file = tox2::loadVfsConfig(cfg / "bad.json");
    CHECK(!bad_file && bad_file.error().find("bad.json") != std::string::npos);

    // Install a file's layout and use it through the guest-facing calls.
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t path_a = scratch, buf = scratch + 0x2000, out = scratch + 0x3000, sb = scratch + 0x3100,
                        arg = scratch + 0x3200;
    enum Import : std::uint32_t { Read, Close, Open, Stat, Write, SdataOpen, Fstat, ImportCount };
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x0d5b4a14, "sys_fs", "cellFsReadWithOffset"},
            {0x00800010, 0x2cb51f0d, "sys_fs", "cellFsClose"},
            {0x00800020, 0x718bf5f8, "sys_fs", "cellFsOpen"},
            {0x00800030, 0x7de6dced, "sys_fs", "cellFsStat"},
            {0x00800040, 0x8cb722d5, "sys_fs", "cellFsWriteWithOffset"},
            {0x00800050, 0xb1840b53, "sys_fs", "cellFsSdataOpen"},
            {0x00800060, 0xef3efa34, "sys_fs", "cellFsFstat"},
    };
    tox2::installImports(imports, ImportCount);
    auto ctx = std::make_unique<Tox2PpuContext>();
    auto call = [&](Import which, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0, std::uint64_t d = 0,
                    std::uint64_t e = 0) {
        ctx->gpr[3] = a;
        ctx->gpr[4] = b;
        ctx->gpr[5] = c;
        ctx->gpr[6] = d;
        ctx->gpr[7] = e;
        tox2HleCall(ctx.get(), base, which);
        return ctx->gpr[3];
    };
    auto put = [&](std::uint32_t addr, std::string_view text) {
        std::memcpy(base + addr, text.data(), text.size());
        base[addr + text.size()] = 0;
    };
    // Reads up to 64 bytes from the start of the open file `fd`.
    auto readAll = [&](std::uint32_t fd) {
        CHECK(call(Read, fd, 0, buf, 64, out + 0x40) == 0);  // `out` keeps the fd
        return std::string(reinterpret_cast<const char*>(base + buf), tox2Read64(base, out + 0x40));
    };

    hostFile(cfg / "layout.json", R"({
    "version": 1, "game_dir": "game", "hdd_dir": "hdd", "sdata_cache": "../cache",
    "mounts": {"/dev_usb000": {"host": "usb", "block_size": 1024}},
    "files": {"/dev_bdvd/PS3_GAME/USRDIR/Exi.sdat": "plain/Exi.bin",
              "/dev_bdvd/b.txt": "plain/other.txt",
              "/dev_hdd0/tmp/w.bin": {"host": "plain/w.bin", "read_only": false}}
  })");
    const auto layout_file = tox2::loadVfsConfig(cfg / "layout.json");
    CHECK(layout_file.has_value());
    const auto layout = layout_file ? tox2::composeVfsLayout(&*layout_file, {})
                                    : std::expected<tox2::VfsLayout, std::string>(std::unexpected("no file"));
    CHECK(layout.has_value());
    if (layout) {
        tox2::configureVfs(*layout);
        CHECK(stdfs::is_directory(cfg / "hdd" / "dev_hdd0" / "game") && stdfs::is_directory(cfg / "usb"));

        // The sdata open serves the override, not the cache copy at the same path.
        tox2Write32(base, out, 0xDEADBEEF);
        put(path_a, "/dev_bdvd/PS3_GAME/USRDIR/Exi.sdat");
        CHECK(call(SdataOpen, path_a, 0, out) == 0);
        const std::uint32_t sfd = tox2Read32(base, out);
        CHECK(readAll(sfd) == "plain-bytes");
        CHECK(call(Fstat, sfd, sb) == 0 && tox2Read64(base, sb + 36) == 11 && tox2Read32(base, sb) == 0100444);
        CHECK(call(Close, sfd) == 0);
        // So does a plain open of the same path and the sdata marker of cellFsOpen.
        CHECK(call(Open, path_a, 0, out, 0, 0) == 0 && readAll(tox2Read32(base, out)) == "plain-bytes");
        CHECK(call(Close, tox2Read32(base, out)) == 0);
        tox2Write32(base, arg, 0x180);
        tox2Write32(base, arg + 4, 0x10);
        CHECK(call(Open, path_a, 0, out, arg, 8) == 0 && readAll(tox2Read32(base, out)) == "plain-bytes");
        CHECK(call(Close, tox2Read32(base, out)) == 0);
        CHECK(call(Stat, path_a, sb) == 0 && tox2Read64(base, sb + 36) == 11);
        // A spelling that normalises to the same guest path matches too.
        put(path_a, "/dev_bdvd/PS3_GAME//USRDIR/./Exi.sdat");
        CHECK(call(SdataOpen, path_a, 0, out) == 0 && readAll(tox2Read32(base, out)) == "plain-bytes");
        CHECK(call(Close, tox2Read32(base, out)) == 0);
        // /app_home is another guest path: no override there, so the cache copy is served.
        put(path_a, "/app_home/Exi.sdat");
        CHECK(call(SdataOpen, path_a, 0, out) == 0 && readAll(tox2Read32(base, out)) == "from-cache");
        CHECK(call(Close, tox2Read32(base, out)) == 0);

        // An override on a path the game directory also has wins over it, read-only.
        put(path_a, "/dev_bdvd/b.txt");
        CHECK(call(Open, path_a, 0, out, 0, 0) == 0);
        const std::uint32_t over_fd = tox2Read32(base, out);
        CHECK(readAll(over_fd) == "other");
        CHECK(call(Write, over_fd, 0, buf, 1, out) == err(tox2::kCellEbadf));
        CHECK(call(Close, over_fd) == 0);
        CHECK(call(Open, path_a, 1, out, 0, 0) == err(tox2::kCellEperm));  // write-only on a read-only file

        // A writable override is created and written at the host path it names.
        put(path_a, "/dev_hdd0/tmp/w.bin");
        CHECK(call(Open, path_a, 0102, out, 0, 0) == 0);  // RDWR | CREAT
        const std::uint32_t w_fd = tox2Read32(base, out);
        put(buf, "written");
        CHECK(call(Write, w_fd, 0, buf, 7, out) == 0 && tox2Read64(base, out) == 7);
        CHECK(call(Close, w_fd) == 0);
        {
            std::ifstream in(cfg / "plain" / "w.bin", std::ios::binary);
            CHECK(std::string(std::istreambuf_iterator<char>(in), {}) == "written");
        }
        CHECK(!stdfs::exists(cfg / "hdd" / "dev_hdd0" / "tmp" / "w.bin"));

        // Mounts from the file: an extra device with its block size; overrides report their mount's.
        hostFile(cfg / "usb" / "f.bin", "usb");
        put(path_a, "/dev_usb000/f.bin");
        CHECK(call(Stat, path_a, sb) == 0 && tox2Read64(base, sb + 44) == 1024);
        put(path_a, "/dev_bdvd/b.txt");
        CHECK(call(Stat, path_a, sb) == 0 && tox2Read64(base, sb + 44) == 2048);
        const tox2::VfsResolution marked = tox2::resolveVfs("/dev_bdvd/b.txt/");
        CHECK(marked.file_override && marked.dir_only);
        CHECK(!tox2::resolveVfs("/dev_usb000/f.bin").file_override);
        CHECK(tox2::resolveVfs("/dev_usb001/x").status == tox2::VfsStatus::NotMounted);
        const std::vector<std::string> names = tox2::vfsMountNames();
        CHECK(std::ranges::find(names, "dev_usb000") != names.end() && names.size() == 5);

        // Dropping the overrides brings back the cache copy for the same path.
        tox2::VfsLayout without_files = *layout;
        without_files.files.clear();
        tox2::configureVfs(std::move(without_files));
        put(path_a, "/dev_bdvd/PS3_GAME/USRDIR/Exi.sdat");
        CHECK(call(SdataOpen, path_a, 0, out) == 0 && readAll(tox2Read32(base, out)) == "from-cache");
        CHECK(call(Close, tox2Read32(base, out)) == 0);
    }

    p.main.free(scratch);
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
    stdfs::remove_all(root, ec);
    tox2::configureVfs({}, std::nullopt);
}

struct TrophyCallbackCall {
    std::uint32_t context, status;
    std::int32_t completed, total;
    std::uint32_t arg;
};
std::vector<TrophyCallbackCall> g_trophy_callbacks;
std::uint64_t g_trophy_callback_result = 0;

TOX2_PPU_FUNC(trophyStatusCallback) {
    (void)base;
    g_trophy_callbacks.push_back({static_cast<std::uint32_t>(ctx->gpr[3]), static_cast<std::uint32_t>(ctx->gpr[4]),
                                  static_cast<std::int32_t>(ctx->gpr[5]), static_cast<std::int32_t>(ctx->gpr[6]),
                                  static_cast<std::uint32_t>(ctx->gpr[7])});
    ctx->gpr[3] = g_trophy_callback_result;
}

void testSceNpAndTrophy() {
    namespace stdfs = std::filesystem;
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetCensus();
    tox2::resetLogOnce();
    tox2::resetHostFunctions();
    tox2::resetSceNp();
    tox2::resetSceNpTrophy();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t out = scratch, comm_id = scratch + 0x100, comm_sign = scratch + 0x200,
                        path_addr = scratch + 0x300, flags = scratch + 0x400;

    // A trophy pack with one entry, the trophy config: a platinum trophy (id 0) and three others
    // that link to it.
    const std::string config =
            "<trophyconf version=\"1.1\">\n"
            " <trophy id=\"000\" hidden=\"no\" ttype=\"P\" pid=\"-1\"/>\n"
            " <trophy id=\"001\" hidden=\"no\" ttype=\"B\" pid=\"000\"/>\n"
            " <trophy id=\"002\" hidden=\"no\" ttype=\"S\" pid=\"000\">\n  <name>x</name>\n </trophy>\n"
            " <trophy id=\"003\" hidden=\"no\" ttype=\"G\" pid=\"000\"/>\n"
            "</trophyconf>\n";
    std::vector<std::uint8_t> trp(128, 0);
    const auto put32 = [&](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; i++) trp[at + i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
    };
    put32(0, 0xDCA24D00);
    put32(4, 1);
    put32(12, static_cast<std::uint32_t>(128 + config.size()));  // file size (low word)
    put32(16, 1);
    put32(20, 64);
    std::memcpy(&trp[64], "TROPCONF.SFM", 12);
    put32(64 + 36, 128);                                        // entry offset (low word)
    put32(64 + 44, static_cast<std::uint32_t>(config.size()));  // entry size (low word)
    trp.insert(trp.end(), config.begin(), config.end());

    const stdfs::path root =
            stdfs::temp_directory_path() / ("tox2_trophy_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    stdfs::remove_all(root, ec);
    const stdfs::path game = root / "game";
    stdfs::create_directories(game / "PS3_GAME" / "TROPDIR" / "NPWR00001_00");
    std::ofstream(game / "PS3_GAME" / "TROPDIR" / "NPWR00001_00" / "TROPHY.TRP", std::ios::binary)
            .write(reinterpret_cast<const char*>(trp.data()), static_cast<std::streamsize>(trp.size()));
    std::ofstream(game / "drm.bin", std::ios::binary) << "data";
    tox2::configureVfs(game, root / "hdd");

    enum Import : std::uint32_t {
        Register,
        CreateHandle,
        DiskSpace,
        DestroyContext,
        Init,
        DestroyHandle,
        Unlock,
        Term,
        UnlockState,
        CreateContext,
        NpInit,
        NpTerm,
        NpDrm,
        ImportCount
    };
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x1197b52c, "sceNpTrophy", "sceNpTrophyRegisterContext"},
            {0x00800010, 0x1c25470d, "sceNpTrophy", "sceNpTrophyCreateHandle"},
            {0x00800020, 0x370136fe, "sceNpTrophy", "sceNpTrophyGetRequiredDiskSpace"},
            {0x00800030, 0x3741ecc7, "sceNpTrophy", "sceNpTrophyDestroyContext"},
            {0x00800040, 0x39567781, "sceNpTrophy", "sceNpTrophyInit"},
            {0x00800050, 0x623cd2dc, "sceNpTrophy", "sceNpTrophyDestroyHandle"},
            {0x00800060, 0x8ceedd21, "sceNpTrophy", "sceNpTrophyUnlockTrophy"},
            {0x00800070, 0xa7fabf4d, "sceNpTrophy", "sceNpTrophyTerm"},
            {0x00800080, 0xb3ac3478, "sceNpTrophy", "sceNpTrophyGetTrophyUnlockState"},
            {0x00800090, 0xe3bf9a28, "sceNpTrophy", "sceNpTrophyCreateContext"},
            {0x008000a0, 0xbd28fdbf, "sceNp", "sceNpInit"},
            {0x008000b0, 0x4885aa18, "sceNp", "sceNpTerm"},
            {0x008000c0, 0xad218faf, "sceNp", "sceNpDrmIsAvailable"},
    };
    tox2::installImports(imports, ImportCount);
    auto ctx = std::make_unique<Tox2PpuContext>();
    ctx->gpr[1] = 0x500000;
    CHECK(p.memory.commit(0x4f0000, 0x10000));
    auto call = [&](Import which, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0, std::uint64_t d = 0,
                    std::uint64_t e = 0) {
        ctx->gpr[3] = a;
        ctx->gpr[4] = b;
        ctx->gpr[5] = c;
        ctx->gpr[6] = d;
        ctx->gpr[7] = e;
        tox2HleCall(ctx.get(), base, which);
        return static_cast<std::uint32_t>(ctx->gpr[3]);
    };
    constexpr std::uint32_t kNotInit = 0x80022902, kNotSupported = 0x80022903, kInvalidArg = 0x80022906,
                            kNotRegistered = 0x80022904, kUnknownContext = 0x8002290a, kInvalidId = 0x80022920,
                            kPlatinum = 0x80022914, kAlready = 0x80022915, kAlreadyInit = 0x80022901,
                            kBadCommId = 0x80022918;

    // sceNp.
    CHECK(call(NpTerm) == 0x8002aa01);
    CHECK(call(NpInit, 0, 0x1000) == 0x8002aa03);
    CHECK(call(NpInit, 0x1000, 0) == 0x8002aa03);
    CHECK(call(NpInit, 0x1ffff, 0x1000) == 0x8002aa0f);
    CHECK(call(NpInit, 0x20000, 0x1000) == 0);
    CHECK(call(NpInit, 0x20000, 0x1000) == 0x8002aa02);
    CHECK(call(NpTerm) == 0);
    CHECK(call(NpDrm, 0, 0) == 0x80029502);
    std::memcpy(base + path_addr, "/dev_bdvd/drm.bin", 18);
    CHECK(call(NpDrm, 0, path_addr) == 0);
    std::memcpy(base + path_addr, "/dev_bdvd/none.bin", 19);
    CHECK(call(NpDrm, 0, path_addr) == tox2::kCellEnoent);

    // Init and the tables.
    CHECK(call(CreateHandle, out) == kNotInit);
    CHECK(call(Init, 0, 0, 0xffffffff, 1) == kNotSupported);
    CHECK(call(Init, 0, 0, 0xffffffff, 0) == 0);
    CHECK(call(Init, 0, 0, 0xffffffff, 0) == kAlreadyInit);
    CHECK(call(CreateHandle, 0) == kInvalidArg);
    CHECK(call(CreateHandle, out) == 0 && tox2Read32(base, out) == 1);
    CHECK(call(CreateHandle, out) == 0 && tox2Read32(base, out) == 2);
    CHECK(call(DestroyHandle, 2) == 0 && call(DestroyHandle, 2) == 0x80022911);
    CHECK(call(DestroyHandle, 9) == kInvalidArg);

    std::memcpy(base + comm_id, "NPWR00001\0", 10);
    base[comm_id + 10] = 0;
    tox2Write32(base, comm_sign, 0xB9DDE13B);
    tox2Write16(base, comm_sign + 4, 0x100);
    CHECK(call(CreateContext, out, comm_id, 0) == kInvalidArg);
    CHECK(call(CreateContext, out, comm_id, comm_sign, 2) == kNotSupported);
    base[comm_sign + 8] = 1;
    CHECK(call(CreateContext, out, comm_id, comm_sign) == kBadCommId);
    base[comm_sign + 8] = 0;
    CHECK(call(CreateContext, out, comm_id, comm_sign) == 0 && tox2Read32(base, out) == 1);

    // Required space is the pack minus its header and entry table; bad ids are rejected.
    CHECK(call(DiskSpace, 1, 1, 0) == kInvalidArg);
    CHECK(call(DiskSpace, 0, 0, out) == kInvalidArg);
    CHECK(call(DiskSpace, 1, 3, out) == 0x80022911);
    CHECK(call(DiskSpace, 1, 1, out) == 0 && tox2Read64(base, out) == config.size());

    // Unlocking needs a registered context; read-only state without an install is empty.
    CHECK(call(Unlock, 1, 1, 1, 0) == kNotRegistered);
    CHECK(call(UnlockState, 1, 1, flags, out) == kNotRegistered);

    // Registration: the install question, then the progress events in order.
    const std::uint32_t cb = tox2::registerHostFunction("trophyStatusCallback", trophyStatusCallback);
    g_trophy_callbacks.clear();
    g_trophy_callback_result = static_cast<std::uint64_t>(-1);  // refuse the install
    CHECK(call(Register, 1, 1, 0, 0, 0) == kInvalidArg);
    CHECK(call(Register, 1, 1, cb, 0x1234, 2) == kNotSupported);
    CHECK(call(Register, 1, 1, cb, 0x1234, 0) == 0x8002290f);
    CHECK(g_trophy_callbacks.size() == 1);
    CHECK(g_trophy_callbacks[0].context == 1 && g_trophy_callbacks[0].status == 1 &&
          g_trophy_callbacks[0].completed == 0 && g_trophy_callbacks[0].total == 0 &&
          g_trophy_callbacks[0].arg == 0x1234);
    CHECK(!stdfs::exists(root / "hdd" / "dev_hdd0" / "home" / "00000001" / "trophy" / "NPWR00001_00"));

    g_trophy_callbacks.clear();
    g_trophy_callback_result = 0;
    CHECK(call(Register, 1, 1, cb, 0x1234, 0) == 0);
    std::vector<TrophyCallbackCall> expected = {{1, 1, 0, 0, 0x1234}};
    const auto expect_run = [&](std::uint32_t status, std::int32_t total) {
        for (std::int32_t i = 0; i <= total; i++) expected.push_back({1, status, i, total, 0x1234});
    };
    expect_run(5, 3);  // 4 trophies: progress count 3, finalize max(3, 9) - 5 = 4
    expect_run(6, 3);
    expect_run(7, 4);
    expect_run(8, 0);
    CHECK(g_trophy_callbacks.size() == expected.size());
    bool same = g_trophy_callbacks.size() == expected.size();
    for (std::size_t i = 0; same && i < expected.size(); i++) {
        const auto& a = g_trophy_callbacks[i];
        const auto& b = expected[i];
        same = a.context == b.context && a.status == b.status && a.completed == b.completed && a.total == b.total &&
               a.arg == b.arg;
    }
    CHECK(same);
    const stdfs::path trophy_dir = root / "hdd" / "dev_hdd0" / "home" / "00000001" / "trophy" / "NPWR00001_00";
    CHECK(stdfs::exists(trophy_dir / "TROPCONF.SFM"));

    // Installed: no disk space needed, and the next registration starts from the installed status.
    CHECK(call(DiskSpace, 1, 1, out) == 0 && tox2Read64(base, out) == 0);
    g_trophy_callbacks.clear();
    CHECK(call(Register, 1, 1, cb, 0, 0) == 0 && g_trophy_callbacks[0].status == 3);

    // Unlock state and the platinum link.
    CHECK(call(UnlockState, 1, 1, flags, out) == 0 && tox2Read32(base, out) == 4 && tox2Read32(base, flags) == 0);
    tox2Write32(base, out, 0x55555555);
    CHECK(call(Unlock, 1, 1, 4, out) == kInvalidId);
    CHECK(call(Unlock, 1, 1, 0xffffffff, out) == kInvalidId);
    CHECK(call(Unlock, 1, 1, 0, out) == kPlatinum);
    CHECK(call(Unlock, 1, 1, 1, out) == 0 && tox2Read32(base, out) == 0xffffffff);
    CHECK(call(Unlock, 1, 1, 1, out) == kAlready);
    CHECK(call(Unlock, 1, 1, 2, out) == 0 && tox2Read32(base, out) == 0xffffffff);
    CHECK(call(Unlock, 1, 1, 3, out) == 0 && tox2Read32(base, out) == 0);  // completes the platinum
    CHECK(call(UnlockState, 1, 1, flags, out) == 0 && tox2Read32(base, flags) == 0xf);
    CHECK(stdfs::exists(trophy_dir / "UNLOCKED.TXT"));

    // The unlock state survives a restart (Term, Init, new context, register).
    CHECK(call(Term) == 0 && call(Term) == kNotInit);
    CHECK(call(UnlockState, 1, 1, flags, out) == kNotInit);
    CHECK(call(Init, 0, 0, 0xffffffff, 0) == 0);
    CHECK(call(UnlockState, 1, 1, flags, out) == kUnknownContext);
    CHECK(call(CreateHandle, out) == 0 && call(CreateContext, out, comm_id, comm_sign) == 0);
    CHECK(call(Register, 1, 1, cb, 0, 0) == 0);
    CHECK(call(UnlockState, 1, 1, flags, out) == 0 && tox2Read32(base, flags) == 0xf);
    CHECK(call(DestroyContext, 1) == 0 && call(DestroyContext, 1) == kUnknownContext);

    tox2::resetSceNpTrophy();
    tox2::resetSceNp();
    tox2::resetHostFunctions();
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
    stdfs::remove_all(root, ec);
}

struct SysutilCallbackCall {
    std::uint64_t status, param;
    std::uint32_t userdata;
};
std::vector<SysutilCallbackCall> g_sysutil_callbacks;
std::function<void()> g_sysutil_callback_hook;

TOX2_PPU_FUNC(sysutilCallback) {
    (void)base;
    g_sysutil_callbacks.push_back({ctx->gpr[3], ctx->gpr[4], static_cast<std::uint32_t>(ctx->gpr[5])});
    if (g_sysutil_callback_hook) g_sysutil_callback_hook();
}

void testCellSysutil() {
    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::resetLogOnce();
    tox2::resetHostFunctions();
    tox2::resetCellSysutil();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);

    enum Import : std::uint32_t { Register, Unregister, Check, EnableBgm, BgmStatus, ImportCount };
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x9d98afa0, "cellSysutil", "cellSysutilRegisterCallback"},
            {0x00800010, 0x02ff3c1b, "cellSysutil", "cellSysutilUnregisterCallback"},
            {0x00800020, 0x189a74da, "cellSysutil", "cellSysutilCheckCallback"},
            {0x00800030, 0x220894e3, "cellSysutil", "cellSysutilEnableBgmPlayback"},
            {0x00800040, 0xa11552f6, "cellSysutil", "cellSysutilGetBgmPlaybackStatus"},
    };
    tox2::installImports(imports, ImportCount);
    auto ctx = std::make_unique<Tox2PpuContext>();
    ctx->gpr[1] = 0x500000;
    CHECK(p.memory.commit(0x4f0000, 0x10000));
    auto call = [&](Import which, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0) {
        ctx->gpr[3] = a;
        ctx->gpr[4] = b;
        ctx->gpr[5] = c;
        tox2HleCall(ctx.get(), base, which);
        return static_cast<std::uint32_t>(ctx->gpr[3]);
    };
    constexpr std::uint32_t kErrorValue = 0x8002b102, kErrorBusy = 0x8002b105, kBgmErrorParam = 0x8002b101;

    const std::uint32_t cb = tox2::registerHostFunction("sysutilCallback", sysutilCallback);
    g_sysutil_callbacks.clear();
    g_sysutil_callback_hook = nullptr;

    // Only slots 0..3 exist.
    CHECK(call(Register, 4, cb, 1) == kErrorValue);
    CHECK(call(Unregister, 4) == kErrorValue);
    CHECK(call(Unregister, 0xffffffff) == kErrorValue);

    // No registered callback: nothing is queued and a check does nothing.
    CHECK(tox2::queueSysutilEvent(tox2::kSysutilRequestExitGame, 1) == 0);
    CHECK(call(Check) == 0 && g_sysutil_callbacks.empty());

    // An event is delivered by the next check, once, with (status, param, userdata).
    CHECK(call(Register, 0, cb, 0x1234) == 0);
    CHECK(call(Register, 2, cb, 0x5678) == 0);
    CHECK(tox2::queueSysutilEvent(tox2::kSysutilRequestExitGame, 7) == 2);
    CHECK(g_sysutil_callbacks.empty());
    CHECK(call(Check) == 0);
    CHECK(g_sysutil_callbacks.size() == 2);
    if (g_sysutil_callbacks.size() == 2) {
        CHECK(g_sysutil_callbacks[0].status == 0x101 && g_sysutil_callbacks[0].param == 7 &&
              g_sysutil_callbacks[0].userdata == 0x1234);
        CHECK(g_sysutil_callbacks[1].status == 0x101 && g_sysutil_callbacks[1].param == 7 &&
              g_sysutil_callbacks[1].userdata == 0x5678);
    }
    CHECK(call(Check) == 0 && g_sysutil_callbacks.size() == 2);

    // An unregistered slot gets nothing; an event keeps the registrations of its enqueue time.
    g_sysutil_callbacks.clear();
    CHECK(call(Unregister, 2) == 0);
    CHECK(tox2::queueSysutilEvent(tox2::kSysutilBgmPlaybackPlay, 0) == 1);
    CHECK(call(Register, 3, cb, 9) == 0);
    CHECK(call(Check) == 0 && g_sysutil_callbacks.size() == 1 && g_sysutil_callbacks[0].userdata == 0x1234);
    CHECK(call(Unregister, 3) == 0);

    // A plain closure runs on the checking thread with its context; one queued from inside a
    // callback waits for the next check.
    g_sysutil_callbacks.clear();
    int ran = 0;
    g_sysutil_callback_hook = [&] { tox2::queueSysutilCall([&](Tox2PpuContext*, std::uint8_t*) { ran += 10; }); };
    tox2::queueSysutilCall([&](Tox2PpuContext* c, std::uint8_t*) {
        CHECK(c == ctx.get());
        ran++;
    });
    CHECK(tox2::queueSysutilEvent(tox2::kSysutilSystemMenuOpen, 0) == 1);
    CHECK(call(Check) == 0 && ran == 1 && g_sysutil_callbacks.size() == 1);
    g_sysutil_callback_hook = nullptr;
    CHECK(call(Check) == 0 && ran == 11 && g_sysutil_callbacks.size() == 1);

    // The callback is called through callGuestFunction: r1 is restored.
    CHECK(ctx->gpr[1] == 0x500000);

    // DRAWING_BEGIN/END pair up.
    CHECK(tox2::queueSysutilEvent(tox2::kSysutilDrawingEnd, 0) == -1);
    CHECK(tox2::queueSysutilEvent(tox2::kSysutilDrawingBegin, 0) == 1);
    CHECK(tox2::queueSysutilEvent(tox2::kSysutilDrawingBegin, 0) == static_cast<std::int32_t>(kErrorBusy));
    CHECK(tox2::queueSysutilEvent(tox2::kSysutilDrawingEnd, 0) == 1);
    CHECK(tox2::queueSysutilEvent(tox2::kSysutilDrawingEnd, 0) == -1);
    CHECK(call(Check) == 0);

    // BGM status: stopped and disabled until enabled.
    const std::uint32_t status = scratch;
    std::memset(base + status, 0xaa, 40);
    CHECK(call(BgmStatus, 0) == kBgmErrorParam);
    CHECK(call(BgmStatus, status) == 0);
    CHECK(base[status] == 1 && base[status + 1] == 1);
    bool zero_rest = true;
    for (std::uint32_t i = 2; i < 32; i++) zero_rest &= base[status + i] == 0;
    CHECK(zero_rest && base[status + 32] == 0xaa);
    CHECK(call(EnableBgm) == 0);
    CHECK(call(BgmStatus, status) == 0 && base[status] == 1 && base[status + 1] == 0);

    tox2::resetCellSysutil();
    g_sysutil_callbacks.clear();
    tox2::resetHostFunctions();
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

void testVideoAndAudioOut() {
    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::resetLogOnce();
    tox2::resetCellSysutil();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);

    enum Import : std::uint32_t {
        GetState,
        Availability,
        Configure,
        AudioState,
        GetResolution,
        AudioConfiguration,
        SoundAvailability,
        AudioConfigure,
        SetGamma,
        DisableBgm,
        EnableBgm,
        BgmStatus,
        ImportCount
    };
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x887572d5, "cellSysutil", "cellVideoOutGetState"},
            {0x00800010, 0xa322db75, "cellSysutil", "cellVideoOutGetResolutionAvailability"},
            {0x00800020, 0x0bae8772, "cellSysutil", "cellVideoOutConfigure"},
            {0x00800030, 0xf4e3caa0, "cellSysutil", "cellAudioOutGetState"},
            {0x00800040, 0xe558748d, "cellSysutil", "cellVideoOutGetResolution"},
            {0x00800050, 0xed5d96af, "cellSysutil", "cellAudioOutGetConfiguration"},
            {0x00800060, 0xc01b4e7c, "cellSysutil", "cellAudioOutGetSoundAvailability"},
            {0x00800070, 0x4692ab35, "cellSysutil", "cellAudioOutConfigure"},
            {0x00800080, 0xc7020f62, "cellSysutilAvconfExt", "cellVideoOutSetGamma"},
            {0x00800090, 0xcfdd8e87, "cellSysutil", "cellSysutilDisableBgmPlayback"},
            {0x008000a0, 0x220894e3, "cellSysutil", "cellSysutilEnableBgmPlayback"},
            {0x008000b0, 0xa11552f6, "cellSysutil", "cellSysutilGetBgmPlaybackStatus"},
    };
    tox2::installImports(imports, ImportCount);
    auto ctx = std::make_unique<Tox2PpuContext>();
    ctx->gpr[1] = 0x500000;
    auto call = [&](Import which, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0, std::uint64_t d = 0) {
        ctx->gpr[3] = a;
        ctx->gpr[4] = b;
        ctx->gpr[5] = c;
        ctx->gpr[6] = d;
        tox2HleCall(ctx.get(), base, which);
        return static_cast<std::uint32_t>(ctx->gpr[3]);
    };
    constexpr std::uint32_t kIllegalParameter = 0x8002b222, kOutOfRange = 0x8002b223, kDeviceNotFound = 0x8002b224,
                            kUnsupportedMode = 0x8002b226, kIllegalConfiguration = 0x8002b221,
                            kAudioIllegal = 0x8002b242, kAudioRange = 0x8002b243;

    // RESOLUTION 0x1d: 480, 720, 1080 and 480 16:9.
    tox2::ParamSfo sfo;
    sfo.ints["RESOLUTION"] = 0x1d;
    sfo.ints["SOUND_FORMAT"] = 0x1;
    tox2::setGameParamSfo(sfo);
    CHECK(call(Availability, 0, 2, 0, 0) == 1);  // 720
    CHECK(call(Availability, 0, 1, 0, 0) == 1);  // 1080
    CHECK(call(Availability, 0, 4, 0, 0) == 1);  // 480
    CHECK(call(Availability, 0, 5, 0, 0) == 0);  // 576
    CHECK(call(Availability, 0, 2, 1, 0) == 0);  // 720 is 16:9 only
    CHECK(call(Availability, 0, 4, 2, 0) == 1);  // 480 16:9
    CHECK(call(Availability, 0, 5, 2, 0) == 0);
    CHECK(call(Availability, 1, 2, 0, 0) == 0);  // secondary output
    CHECK(call(Availability, 2, 2, 0, 0) == 0x8002b225);

    // Default state: 1280x720 progressive 16:9 RGB, 59.94 Hz.
    const std::uint32_t state = scratch;
    std::memset(base + state, 0xaa, 24);
    CHECK(call(GetState, 0, 0, 0) == kIllegalParameter);
    CHECK(call(GetState, 0, 1, state) == kDeviceNotFound);
    CHECK(call(GetState, 1, 0, state) == kDeviceNotFound);
    CHECK(call(GetState, 0, 0, state) == 0);
    const std::uint8_t expected_default[16] = {0, 1, 0, 0, 0, 0, 0, 0, 2, 1, 0, 2, 0, 0, 0, 1};
    CHECK(std::memcmp(base + state, expected_default, 16) == 0 && base[state + 16] == 0xaa);

    // Without 720p the default falls back to 480.
    tox2::ParamSfo sd;
    sd.ints["RESOLUTION"] = 0x1;
    tox2::setGameParamSfo(sd);
    CHECK(call(GetState, 0, 0, state) == 0 && base[state + 8] == 4 && base[state + 11] == 1);
    CHECK(call(Availability, 0, 2, 0, 0) == 0 && call(Availability, 0, 4, 0, 0) == 1);
    tox2::setGameParamSfo(sfo);

    // Configure validates, then GetState reports the stored mode.
    const std::uint32_t config = scratch + 0x100;
    auto setConfig = [&](std::uint8_t res, std::uint8_t format, std::uint8_t aspect, std::uint32_t pitch) {
        std::memset(base + config, 0, 16);
        base[config] = res;
        base[config + 1] = format;
        base[config + 2] = aspect;
        tox2Write32(base, config + 12, pitch);
    };
    CHECK(call(Configure, 0, 0, 0, 0) == kIllegalParameter);
    setConfig(2, 0, 0, 0);
    CHECK(call(Configure, 0, config, 0, 0) == kOutOfRange);
    setConfig(0x92, 0, 0, 0x1400);
    CHECK(call(Configure, 0, config, 0, 0) == kIllegalConfiguration);
    setConfig(0x3, 0, 0, 0x1400);
    CHECK(call(Configure, 0, config, 0, 0) == kUnsupportedMode);
    setConfig(0x81, 0, 0, 0x1400);
    CHECK(call(Configure, 0, config, 0, 0) == kUnsupportedMode);
    CHECK(call(GetState, 0, 0, state) == 0 && base[state + 8] == 2);  // nothing was stored
    setConfig(1, 0, 0, 0x1e00);
    CHECK(call(Configure, 0, config, 0, 0) == 0);
    CHECK(call(GetState, 0, 0, state) == 0 && base[state + 8] == 1 && base[state + 9] == 1 && base[state + 11] == 2);
    setConfig(4, 0, 1, 0xb40);
    CHECK(call(Configure, 0, config, 0, 0) == 0);
    CHECK(call(GetState, 0, 0, state) == 0 && base[state + 8] == 4 && base[state + 11] == 1);

    // Audio out: LPCM 2 ch 48 kHz, layout 2CH.
    std::memset(base + state, 0xaa, 24);
    CHECK(call(AudioState, 0, 0, 0) == kAudioIllegal);
    CHECK(call(AudioState, 2, 0, state) == kAudioIllegal);
    CHECK(call(AudioState, 0, 1, state) == kAudioRange);
    CHECK(call(AudioState, 0, 0, state) == 0);
    const std::uint8_t expected_audio[20] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 4, 0, 0, 0, 0, 1};
    CHECK(std::memcmp(base + state, expected_audio, 20) == 0 && base[state + 20] == 0xaa);
    CHECK(call(AudioState, 1, 0, state) == 0 && base[state] == 0x10 && tox2Read32(base, state + 16) == 0xd00c1680);

    // cellVideoOutGetResolution describes the id, whatever the configured mode and the SFO flags are.
    const std::uint32_t resolution = scratch + 0x200;
    std::uint32_t width = 0, height = 0;
    const auto sizeOf = [&](std::uint32_t id) {
        std::memset(base + resolution, 0xaa, 8);
        const std::uint32_t result = call(GetResolution, id, resolution);
        width = tox2Read16(base, resolution);
        height = tox2Read16(base, resolution + 2);
        return result;
    };
    CHECK(sizeOf(1) == 0 && width == 1920 && height == 1080);
    CHECK(sizeOf(2) == 0 && width == 1280 && height == 720);
    CHECK(sizeOf(4) == 0 && width == 720 && height == 480);
    CHECK(sizeOf(5) == 0 && width == 720 && height == 576);
    CHECK(sizeOf(0x0a) == 0 && width == 1600 && height == 1080);
    CHECK(sizeOf(0x0d) == 0 && width == 960 && height == 1080);
    CHECK(sizeOf(0) == kIllegalParameter && width == 0xaaaa && height == 0xaaaa);
    CHECK(sizeOf(3) == kIllegalParameter && sizeOf(0x81) == kIllegalParameter);
    CHECK(sizeOf(0x102) == kIllegalParameter);  // the id is not truncated to a byte
    CHECK(call(GetResolution, 2, 0) == kIllegalParameter);
    // cellVideoOutConfigure accepts exactly the ids that have a size.
    for (std::uint32_t id = 0; id < 0x100; id++) {
        setConfig(static_cast<std::uint8_t>(id), 0, 0, 0x1400);
        CHECK((call(Configure, 0, config, 0, 0) == 0) == (sizeOf(id) == 0));
    }

    // Audio out: the configuration, the availability and the state share one setting.
    const std::uint32_t audio_config = scratch + 0x300;
    std::memset(base + audio_config, 0xaa, 20);
    CHECK(call(AudioConfiguration, 0, 0, 0) == kAudioIllegal);
    CHECK(call(AudioConfiguration, 2, audio_config, 0) == kAudioIllegal);
    CHECK(call(AudioConfiguration, 0, audio_config, 0) == 0);
    const std::uint8_t expected_config[16] = {2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(std::memcmp(base + audio_config, expected_config, 16) == 0 && base[audio_config + 16] == 0xaa);
    CHECK(call(AudioConfiguration, 1, audio_config, 0) == 0);  // the secondary output is empty
    CHECK(std::all_of(base + audio_config, base + audio_config + 16, [](std::uint8_t byte) { return byte == 0; }));

    // LPCM (0) at 48 kHz (4) offers 2 channels; other types and rates offer none.
    CHECK(call(SoundAvailability, 0, 0, 4, 0) == 2);
    CHECK(call(SoundAvailability, 0, 1, 4, 0) == 0 && call(SoundAvailability, 0, 0, 1, 0) == 0);
    CHECK(call(SoundAvailability, 1, 0, 4, 0) == 0);
    CHECK(call(SoundAvailability, 2, 0, 4, 0) == 0x8002b245);

    const auto setAudioConfig = [&](std::uint8_t channel, std::uint8_t encoder, std::uint32_t down_mixer) {
        std::memset(base + audio_config, 0, 16);
        base[audio_config] = channel;
        base[audio_config + 1] = encoder;
        tox2Write32(base, audio_config + 12, down_mixer);
    };
    constexpr std::uint32_t kAudioUnsupportedMode = 0x8002b246;
    CHECK(call(AudioConfigure, 0, 0, 0, 0) == kAudioIllegal);
    CHECK(call(AudioConfigure, 2, audio_config, 0, 0) == kAudioIllegal);
    setAudioConfig(8, 0, 0);
    CHECK(call(AudioConfigure, 0, audio_config, 0, 0) == kAudioUnsupportedMode);
    setAudioConfig(2, 1, 0);
    CHECK(call(AudioConfigure, 0, audio_config, 0, 0) == kAudioUnsupportedMode);
    setAudioConfig(2, 0, 3);
    CHECK(call(AudioConfigure, 0, audio_config, 0, 0) == kAudioIllegal);
    CHECK(call(AudioState, 0, 0, state) == 0 && tox2Read32(base, state + 8) == 0);  // nothing was stored
    setAudioConfig(0, 0, 2);                                  // channel 0 keeps the current channel count
    CHECK(call(AudioConfigure, 1, audio_config, 0, 0) == 0);  // the secondary output ignores it
    CHECK(call(AudioState, 0, 0, state) == 0 && tox2Read32(base, state + 8) == 0);
    CHECK(call(AudioConfigure, 0, audio_config, 0, 0) == 0);
    CHECK(call(AudioState, 0, 0, state) == 0 && tox2Read32(base, state + 8) == 2 && base[state + 13] == 2);
    CHECK(call(AudioConfiguration, 0, audio_config, 0) == 0 && base[audio_config] == 2 &&
          tox2Read32(base, audio_config + 12) == 2);

    // cellVideoOutSetGamma accepts the SDK range 0.8 to 1.8; the value is in f1.
    const auto setGamma = [&](double gamma) {
        ctx->fpr[1].f64 = gamma;
        return call(SetGamma, 0);
    };
    CHECK(setGamma(1.0) == 0 && setGamma(0.8) == 0 && setGamma(1.8) == 0);
    CHECK(setGamma(0.5) == kIllegalParameter && setGamma(2.5) == kIllegalParameter);
    CHECK(setGamma(std::numeric_limits<double>::quiet_NaN()) == kIllegalParameter);
    ctx->fpr[1].f64 = 1.0;
    CHECK(call(SetGamma, 1) == 0x8002b225);  // only the primary output has a gamma

    // DisableBgmPlayback undoes EnableBgmPlayback.
    const std::uint32_t bgm = scratch + 0x400;
    CHECK(call(EnableBgm) == 0 && call(BgmStatus, bgm) == 0 && base[bgm + 1] == 0);
    CHECK(call(DisableBgm) == 0 && call(BgmStatus, bgm) == 0 && base[bgm + 1] == 1);

    // A reset forgets the audio setting.
    tox2::resetCellSysutil();
    CHECK(call(AudioState, 0, 0, state) == 0 && tox2Read32(base, state + 8) == 0);

    tox2::setGameParamSfo({});
    tox2::resetCellSysutil();
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

std::vector<std::pair<std::int64_t, std::uint32_t>> g_dialog_results;
std::vector<std::uint64_t> g_dialog_sysutil_events;

TOX2_PPU_FUNC(msgDialogCallback) {
    (void)base;
    g_dialog_results.emplace_back(static_cast<std::int32_t>(ctx->gpr[3]), static_cast<std::uint32_t>(ctx->gpr[4]));
}

TOX2_PPU_FUNC(msgDialogSysutilCallback) {
    (void)base;
    g_dialog_sysutil_events.push_back(ctx->gpr[3]);
}

// A scripted pad for the cellPad tests: the state a host backend would report.
struct ScriptedPad final : public tox2::PadProvider {
    tox2::PadPortState state = tox2::neutralPadState();
    std::uint32_t vibration_port = 99;
    int small_motor = -1, large_motor = -1;

    tox2::PadPortState poll(std::uint32_t port) override { return port == 0 ? state : tox2::PadPortState{}; }
    void setVibration(std::uint32_t port, std::uint8_t small, std::uint8_t large) override {
        vibration_port = port;
        small_motor = small;
        large_motor = large;
    }
};

void testCellPad() {
    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::resetLogOnce();
    tox2::resetCellPad();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t info = scratch, data = scratch + 0x200, act = scratch + 0x400;

    enum Import : std::uint32_t { Init, End, SetPortSetting, GetData, GetInfo2, SetActDirect, ImportCount };
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x1cf98800, "sys_io", "cellPadInit"},
            {0x00800010, 0x4d9b75d5, "sys_io", "cellPadEnd"},
            {0x00800020, 0x578e3c98, "sys_io", "cellPadSetPortSetting"},
            {0x00800030, 0x8b72cda1, "sys_io", "cellPadGetData"},
            {0x00800040, 0xa703a51d, "sys_io", "cellPadGetInfo2"},
            {0x00800050, 0xf65544ee, "sys_io", "cellPadSetActDirect"},
    };
    tox2::installImports(imports, ImportCount);
    auto ctx = std::make_unique<Tox2PpuContext>();
    auto call = [&](Import which, std::uint64_t a = 0, std::uint64_t b = 0) {
        ctx->gpr[3] = a;
        ctx->gpr[4] = b;
        tox2HleCall(ctx.get(), base, which);
        return static_cast<std::uint32_t>(ctx->gpr[3]);
    };
    constexpr std::uint32_t kInvalid = 0x80121102, kAlready = 0x80121103, kUninit = 0x80121104, kNoDevice = 0x80121107,
                            kUnsupported = 0x80121108;
    const auto len = [&] { return static_cast<std::int32_t>(tox2Read32(base, data)); };
    const auto word = [&](std::uint32_t i) { return tox2Read16(base, data + 4 + 2 * i); };
    const auto fill = [&](std::uint32_t addr, std::uint32_t size) { std::memset(base + addr, 0xAA, size); };

    // Everything before Init.
    CHECK(call(End) == kUninit);
    CHECK(call(GetInfo2, info) == kUninit);
    CHECK(call(GetData, 0, data) == kUninit);
    CHECK(call(SetPortSetting, 0, 2) == kUninit);
    CHECK(call(SetActDirect, 0, act) == kUninit);

    // Init validation and state.
    CHECK(call(Init, 0) == kInvalid);
    CHECK(call(Init, 128) == kInvalid);
    CHECK(call(Init, 7) == 0);
    CHECK(call(Init, 7) == kAlready);
    CHECK(call(Init, 0) == kAlready);  // the initialised check comes first
    CHECK(call(End) == 0 && call(End) == kUninit);

    // No pad connected: zeroed info, NO_DEVICE everywhere, the data block untouched.
    CHECK(tox2::setPadBackend("none") && !tox2::setPadBackend("keyboard"));
    CHECK(call(Init, 7) == 0);
    fill(info, 0x100);
    CHECK(call(GetInfo2, 0) == kInvalid);
    CHECK(call(GetInfo2, info) == 0);
    bool zero = true;
    for (std::uint32_t i = 0; i < 124; i++) zero = zero && base[info + i] == (i == 3 ? 7 : 0);
    CHECK(zero);  // max_connect 7 in the last byte of the first word
    CHECK(base[info + 124] == 0xAA);
    fill(data, 0x20);
    CHECK(call(GetData, 0, data) == kNoDevice && base[data] == 0xAA);
    CHECK(call(GetData, 6, data) == kNoDevice);
    CHECK(call(GetData, 127, data) == kInvalid && call(GetData, 0, 0) == kInvalid);
    std::memset(base + act, 0, 8);
    CHECK(call(SetActDirect, 0, act) == kNoDevice);
    CHECK(call(SetPortSetting, 0, 6) == 0 && call(SetPortSetting, 127, 0) == kInvalid);
    CHECK(call(SetPortSetting, 50, 6) == 0);  // beyond the 7 ports: accepted, no state
    CHECK(call(End) == 0);

    // Neutral pad: GetInfo2 layout in big-endian words.
    CHECK(tox2::setPadBackend("neutral"));
    CHECK(call(Init, 4) == 0);
    fill(info, 0x100);
    CHECK(call(GetInfo2, info) == 0);
    CHECK(tox2Read32(base, info + 0) == 4 && tox2Read32(base, info + 4) == 1 && tox2Read32(base, info + 8) == 0);
    CHECK(tox2Read32(base, info + 12) == 3);  // connected + assign changed, reported once
    CHECK(tox2Read32(base, info + 16) == 0 && tox2Read32(base, info + 36) == 0);
    CHECK(tox2Read32(base, info + 40) == 0);  // port_setting
    CHECK(tox2Read32(base, info + 68) == tox2::kPadCapabilityStandard && tox2Read32(base, info + 96) == 0);
    CHECK(tox2Read32(base, info + 72) == 0);                   // port 1: nothing connected
    CHECK(tox2Read32(base, info + 12 + 4 * 4) == 0);           // port 4 is beyond max_connect 4
    CHECK(base[info + 123] == 0 && base[info + 124] == 0xAA);  // exactly 124 bytes written
    CHECK(call(GetInfo2, info) == 0 && tox2Read32(base, info + 12) == 1 && tox2Read32(base, info + 4) == 1);

    // The first GetData reports everything, later ones report no change.
    fill(data, 0x100);
    CHECK(call(GetData, 0, data) == 0 && len() == 8);
    CHECK(word(0) == 0 && word(1) == 0x74 && word(2) == 0 && word(3) == 0);
    CHECK(word(4) == 0x80 && word(5) == 0x80 && word(6) == 0x80 && word(7) == 0x80);
    CHECK(word(8) == 0 && word(19) == 0);  // pressure area cleared
    CHECK(call(GetData, 0, data) == 0 && len() == 0);
    CHECK(call(GetData, 1, data) == kNoDevice);
    CHECK(call(GetData, 4, data) == kNoDevice);

    // Port setting keeps only the two defined bits.
    CHECK(call(SetPortSetting, 0, 0xff) == 0);
    CHECK(call(GetInfo2, info) == 0 && tox2Read32(base, info + 40) == 6);
    CHECK(call(SetPortSetting, 0, 2) == 0);
    CHECK(call(GetInfo2, info) == 0 && tox2Read32(base, info + 40) == 2);

    // A host backend plugs in; pressure data appears only when asked for.
    auto host = std::make_unique<ScriptedPad>();
    ScriptedPad* pad = host.get();
    tox2::setPadProvider(std::move(host));
    CHECK(call(End) == 0 && call(Init, 4) == 0);
    CHECK(call(GetInfo2, info) == 0 && tox2Read32(base, info + 12) == 3 && tox2Read32(base, info + 40) == 0);
    CHECK(call(SetPortSetting, 0, 2) == 0);
    fill(data, 0x100);
    pad->state.button[tox2::kPadButtonDigital2] = 0x40;  // cross
    pad->state.button[tox2::kPadButtonPressureFirst + 9] = 0xff;
    CHECK(call(GetData, 0, data) == 0 && len() == 20 && word(1) == 0x7a);
    CHECK(word(3) == 0x40 && word(17) == 0xff && word(20) == 0xAAAA);  // sensors not copied
    CHECK(call(GetData, 0, data) == 0 && len() == 0);
    pad->state.button[tox2::kPadButtonDigital1] = 0x10;  // d-pad up
    CHECK(call(GetData, 0, data) == 0 && len() == 20 && word(2) == 0x10);
    pad->state.button[tox2::kPadButtonSensorFirst] = 600;  // ignored while sensors are off
    CHECK(call(GetData, 0, data) == 0 && len() == 0);
    CHECK(call(SetPortSetting, 0, 6) == 0);
    CHECK(call(GetData, 0, data) == 0 && len() == 24 && word(1) == 0x7c && word(20) == 600 && word(21) == 399);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    CHECK(call(GetData, 0, data) == 0 && len() == 24);  // sensors on: repeated every 10 ms

    // Unplugging and replugging is reported through the assign-changed flag.
    CHECK(call(SetPortSetting, 0, 0) == 0);
    pad->state.connected = false;
    CHECK(call(GetData, 0, data) == kNoDevice);
    CHECK(call(GetInfo2, info) == 0 && tox2Read32(base, info + 12) == 2 && tox2Read32(base, info + 4) == 0 &&
          tox2Read32(base, info + 68) == 0);
    CHECK(call(GetInfo2, info) == 0 && tox2Read32(base, info + 12) == 0);
    pad->state.connected = true;
    CHECK(call(GetInfo2, info) == 0 && tox2Read32(base, info + 12) == 3);
    CHECK(call(GetData, 0, data) == 0 && len() == 8);  // a replugged pad reports everything again

    // SetActDirect: validation order, capability check, motor routing.
    std::memset(base + act, 0, 8);
    base[act] = 1;
    base[act + 1] = 200;
    CHECK(call(SetActDirect, 0, 0) == kInvalid && call(SetActDirect, 127, act) == kInvalid);
    base[act + 5] = 1;  // reserved byte
    CHECK(call(SetActDirect, 0, act) == kInvalid);
    base[act + 5] = 0;
    CHECK(call(SetActDirect, 5, act) == kNoDevice && call(SetActDirect, 1, act) == kNoDevice);
    CHECK(call(SetActDirect, 0, act) == 0 && pad->vibration_port == 0 && pad->small_motor == 1 &&
          pad->large_motor == 200);
    pad->state.device_capability = tox2::kPadCapabilityStandard & ~0x10u;
    CHECK(call(SetActDirect, 0, act) == kUnsupported);

    tox2::resetCellPad();
    p.main.free(scratch);
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

void testCellPadIntercept() {
    using tox2::PadButton;
    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::resetLogOnce();
    tox2::resetCellPad();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t info = scratch, data = scratch + 0x200;

    enum Import : std::uint32_t { Init, End, SetPortSetting, GetData, GetInfo2, SetActDirect, ImportCount };
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x1cf98800, "sys_io", "cellPadInit"},
            {0x00800010, 0x4d9b75d5, "sys_io", "cellPadEnd"},
            {0x00800020, 0x578e3c98, "sys_io", "cellPadSetPortSetting"},
            {0x00800030, 0x8b72cda1, "sys_io", "cellPadGetData"},
            {0x00800040, 0xa703a51d, "sys_io", "cellPadGetInfo2"},
            {0x00800050, 0xf65544ee, "sys_io", "cellPadSetActDirect"},
    };
    tox2::installImports(imports, ImportCount);
    auto ctx = std::make_unique<Tox2PpuContext>();
    auto call = [&](Import which, std::uint64_t a = 0, std::uint64_t b = 0) {
        ctx->gpr[3] = a;
        ctx->gpr[4] = b;
        tox2HleCall(ctx.get(), base, which);
        return static_cast<std::uint32_t>(ctx->gpr[3]);
    };
    const auto len = [&] { return static_cast<std::int32_t>(tox2Read32(base, data)); };
    const auto word = [&](std::uint32_t i) { return tox2Read16(base, data + 4 + 2 * i); };
    const auto fill = [&] { std::memset(base + data, 0xAA, 0x100); };
    const auto set = [](ScriptedPad& pad, tox2::PadButtons buttons) { pad.state = tox2::padStateFromButtons(buttons); };
    const auto circle = tox2::padButtonBit(PadButton::Circle);
    const auto cross = tox2::padButtonBit(PadButton::Cross);
    const auto up = tox2::padButtonBit(PadButton::Up);

    // Enter/back mapping.
    CHECK(tox2::enterButtonAssign() == tox2::EnterButtonAssign::Circle);
    CHECK(tox2::dialogButtons().enter == PadButton::Circle && tox2::dialogButtons().back == PadButton::Cross);
    tox2::setEnterButtonAssign(tox2::EnterButtonAssign::Cross);
    CHECK(tox2::dialogButtons().enter == PadButton::Cross && tox2::dialogButtons().back == PadButton::Circle);
    tox2::setEnterButtonAssign(tox2::EnterButtonAssign::Circle);
    CHECK(tox2::dialogButtons().enter == PadButton::Circle && tox2::dialogButtons().back == PadButton::Cross);

    auto host = std::make_unique<ScriptedPad>();
    ScriptedPad* pad = host.get();
    tox2::setPadProvider(std::move(host));
    CHECK(call(Init, 4) == 0);
    CHECK(!tox2::padIntercepted());
    CHECK(call(GetInfo2, info) == 0 && tox2Read32(base, info + 8) == 0);
    set(*pad, circle);
    CHECK(call(GetData, 0, data) == 0 && len() == 8 && word(3) == 0x20);

    // Intercepted: no change is reported even when a button changes; the game's view stays as it was.
    tox2::setPadIntercepted(true);
    CHECK(tox2::padIntercepted());
    CHECK(call(GetInfo2, info) == 0 && tox2Read32(base, info + 8) == 1);
    set(*pad, circle | cross);
    fill();
    CHECK(call(GetData, 0, data) == 0 && len() == 0);
    CHECK(call(GetData, 0, data) == 0 && len() == 0);
    CHECK(call(GetData, 1, data) == 0x80121107);  // the connection checks still come first

    // pollHostPad reads the provider and leaves what the game was told alone.
    const tox2::PadPortState host_state = tox2::pollHostPad(0);
    CHECK(host_state.connected && host_state.button[tox2::kPadButtonDigital2] == 0x60);
    CHECK(!tox2::pollHostPad(1).connected);

    // Released with circle still held: one change to neutral, then nothing.
    tox2::setPadIntercepted(false);
    CHECK(!tox2::padIntercepted());
    CHECK(call(GetInfo2, info) == 0 && tox2Read32(base, info + 8) == 0);
    set(*pad, circle);
    fill();
    CHECK(call(GetData, 0, data) == 0 && len() == 8 && word(2) == 0 && word(3) == 0);
    CHECK(word(4) == 0x80 && word(7) == 0x80);
    CHECK(call(GetData, 0, data) == 0 && len() == 0);
    CHECK(call(GetData, 0, data) == 0 && len() == 0);

    // Once the watched buttons are up the real state is reported. A d-pad press is not watched.
    set(*pad, up);
    fill();
    CHECK(call(GetData, 0, data) == 0 && len() == 8 && word(2) == 0x10 && word(3) == 0);
    CHECK(call(GetData, 0, data) == 0 && len() == 0);
    set(*pad, circle);
    CHECK(call(GetData, 0, data) == 0 && len() == 8 && word(3) == 0x20);  // the phase has ended

    // pollHostPad does not change what the next GetData reports.
    set(*pad, cross);
    (void)tox2::pollHostPad(0);
    CHECK(call(GetData, 0, data) == 0 && len() == 8 && word(3) == 0x40);

    // Intercepting twice and a full cycle without a held button ends the phase on the first call.
    tox2::setPadIntercepted(true);
    tox2::setPadIntercepted(true);
    set(*pad, 0);
    tox2::setPadIntercepted(false);
    CHECK(call(GetData, 0, data) == 0 && len() == 8 && word(3) == 0);  // buffer cleared: reported again
    CHECK(call(GetData, 0, data) == 0 && len() == 0);

    // resetCellPad clears the interception.
    tox2::setPadIntercepted(true);
    tox2::resetCellPad();
    CHECK(!tox2::padIntercepted());

    p.main.free(scratch);
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

bool samePadState(const tox2::PadPortState& a, const tox2::PadPortState& b) {
    return a.connected == b.connected && a.device_capability == b.device_capability && a.device_type == b.device_type &&
           a.button == b.button;
}

void testPadButtons() {
    using tox2::PadButton;
    CHECK(samePadState(tox2::padStateFromButtons(0), tox2::neutralPadState()));

    const tox2::PadPortState cross = tox2::padStateFromButtons(tox2::padButtonBit(PadButton::Cross));
    CHECK(cross.button[tox2::kPadButtonDigital1] == 0 && cross.button[tox2::kPadButtonDigital2] == 0x40);
    CHECK(cross.button[14] == 0xFF && cross.connected);

    const tox2::PadPortState up_start =
            tox2::padStateFromButtons(tox2::padButtonBit(PadButton::Up) | tox2::padButtonBit(PadButton::Start));
    CHECK(up_start.button[tox2::kPadButtonDigital1] == 0x18 && up_start.button[tox2::kPadButtonDigital2] == 0);
    CHECK(up_start.button[10] == 0xFF);
    // Start has no pressure word: Up's is the only one set.
    std::size_t pressure_set = 0;
    for (std::size_t i = tox2::kPadButtonPressureFirst; i < tox2::kPadButtonSensorFirst; i++) {
        pressure_set += up_start.button[i] != 0;
    }
    CHECK(pressure_set == 1);

    // Every button on its own: one digital bit, its pressure word (if any), nothing else changed.
    constexpr std::size_t kPressureWords[16] = {0, 0, 0, 0, 10, 8, 11, 9, 18, 19, 16, 17, 12, 13, 14, 15};
    const tox2::PadPortState neutral = tox2::neutralPadState();
    bool layout_ok = true;
    for (std::size_t i = 0; i < 16; i++) {
        const auto button = static_cast<PadButton>(i);
        CHECK(tox2::padPressureWord(button) == kPressureWords[i]);
        tox2::PadPortState expected = neutral;
        expected.button[i < 8 ? tox2::kPadButtonDigital1 : tox2::kPadButtonDigital2] =
                static_cast<std::uint16_t>(1u << (i % 8));
        if (kPressureWords[i] != 0) expected.button[kPressureWords[i]] = 0xFF;
        layout_ok = layout_ok && samePadState(tox2::padStateFromButtons(tox2::padButtonBit(button)), expected);
    }
    CHECK(layout_ok);
    CHECK(tox2::padPressureWord(PadButton::Count) == 0);

    // Names round-trip for all 16 buttons; anything else is rejected.
    bool names_ok = true;
    for (std::size_t i = 0; i < 16; i++) {
        const auto button = static_cast<PadButton>(i);
        const std::string_view name = tox2::padButtonName(button);
        names_ok = names_ok && !name.empty() && tox2::padButtonFromName(name) == button;
    }
    CHECK(names_ok);
    CHECK(tox2::padButtonName(PadButton::Cross) == "cross" && tox2::padButtonName(PadButton::L3) == "l3");
    CHECK(tox2::padButtonName(PadButton::Count).empty());
    CHECK(!tox2::padButtonFromName("crosss") && !tox2::padButtonFromName("") && !tox2::padButtonFromName("Cross"));
    CHECK(!tox2::padButtonFromName("ps") && !tox2::padButtonFromName("count"));
}

void testKeyboardPadScanCodes() {
    using tox2::PadButton;
    struct Key {
        std::uint32_t scan_code;
        PadButton button;
    };
    constexpr Key kKeys[] = {
            {0xE048, PadButton::Up},  {0xE050, PadButton::Down}, {0xE04B, PadButton::Left}, {0xE04D, PadButton::Right},
            {0x2C, PadButton::Cross}, {0x2D, PadButton::Circle}, {0x1F, PadButton::Square}, {0x20, PadButton::Triangle},
            {0x11, PadButton::L1},    {0x12, PadButton::L2},     {0x13, PadButton::R1},     {0x16, PadButton::R2},
            {0x02, PadButton::L3},    {0x04, PadButton::R3},     {0x2F, PadButton::Start},  {0x2E, PadButton::Select},
    };
    tox2::PadButtons covered = 0;
    for (const Key& key : kKeys) {
        CHECK(tox2::padButtonForScanCode(key.scan_code) == key.button);
        covered |= tox2::padButtonBit(key.button);
    }
    CHECK(covered == 0xFFFF);

    // No other scan code maps, so each button has exactly one key.
    std::size_t mapped = 0;
    for (std::uint32_t code = 0; code < 0x10000; code++) mapped += tox2::padButtonForScanCode(code).has_value();
    CHECK(mapped == 16);
    CHECK(!tox2::padButtonForScanCode(0x48));  // numpad 8: same code as Up without the extended bit
    CHECK(!tox2::padButtonForScanCode(0x50) && !tox2::padButtonForScanCode(0x4B) && !tox2::padButtonForScanCode(0x4D));
    CHECK(!tox2::padButtonForScanCode(0));
}

void testKeyboardPadState() {
    using State = tox2::KeyboardPadState;
    using std::chrono::milliseconds;
    using tox2::PadButton;
    constexpr tox2::PadButtons kCross = tox2::padButtonBit(PadButton::Cross);
    constexpr tox2::PadButtons kStart = tox2::padButtonBit(PadButton::Start);
    constexpr tox2::PadButtons kUp = tox2::padButtonBit(PadButton::Up);
    constexpr tox2::PadButtons kDown = tox2::padButtonBit(PadButton::Down);
    constexpr tox2::PadButtons kLeft = tox2::padButtonBit(PadButton::Left);
    constexpr tox2::PadButtons kRight = tox2::padButtonBit(PadButton::Right);
    constexpr std::uint32_t kZ = 0x2C;
    const State::Clock::time_point t0 = State::Clock::time_point{} + std::chrono::seconds(100);

    State state;
    CHECK(state.held() == 0 && state.reported(t0) == 0);
    CHECK(!state.keyEvent(0x48, true, false, t0) && state.held() == 0);  // unmapped
    CHECK(state.keyEvent(kZ, true, false, t0) && state.held() == kCross);

    // A tap shorter than the polling interval is still reported for the minimum visible time. The
    // key-up message of Win32 always carries the repeat flag; it must still release.
    CHECK(state.keyEvent(kZ, false, true, t0 + milliseconds(10)));
    CHECK(state.held() == 0);
    CHECK(state.reported(t0 + milliseconds(10)) == kCross);
    CHECK(state.reported(t0 + milliseconds(50)) == kCross);
    CHECK(state.reported(t0 + milliseconds(59)) == kCross);
    CHECK(state.reported(t0 + milliseconds(70)) == 0);

    // A repeat of a key that is not held (focus regained) is ignored; a repeat of a held key does
    // not refresh the press time.
    CHECK(state.keyEvent(kZ, true, true, t0 + milliseconds(100)) && state.held() == 0);
    CHECK(state.reported(t0 + milliseconds(110)) == 0);
    const State::Clock::time_point t1 = t0 + std::chrono::seconds(1);
    CHECK(state.keyEvent(kZ, true, false, t1));
    CHECK(state.keyEvent(kZ, true, true, t1 + milliseconds(40)) && state.held() == kCross);
    CHECK(state.keyEvent(kZ, true, false, t1 + milliseconds(45)));  // already down: also ignored
    CHECK(state.keyEvent(kZ, false, true, t1 + milliseconds(50)));
    CHECK(state.reported(t1 + milliseconds(59)) == kCross);
    CHECK(state.reported(t1 + milliseconds(61)) == 0);

    // A long press ends with the key.
    const State::Clock::time_point t2 = t1 + std::chrono::seconds(1);
    CHECK(state.keyEvent(kZ, true, false, t2));
    CHECK(state.reported(t2 + std::chrono::seconds(5)) == kCross);
    CHECK(state.keyEvent(kZ, false, true, t2 + std::chrono::seconds(5)));
    CHECK(state.reported(t2 + std::chrono::seconds(5) + milliseconds(70)) == 0);

    // Opposite d-pad directions held together cancel; the other axis is unaffected.
    const State::Clock::time_point t3 = t2 + std::chrono::seconds(10);
    State dpad;
    dpad.setButtons(tox2::InputSource::Keyboard, kUp, true, t3);
    CHECK(dpad.reported(t3 + std::chrono::seconds(1)) == kUp);
    dpad.setButtons(tox2::InputSource::Script, kDown, true, t3);
    CHECK(dpad.held() == (kUp | kDown));
    CHECK(dpad.reported(t3 + std::chrono::seconds(1)) == 0);
    dpad.setButtons(tox2::InputSource::Keyboard, kLeft | kRight | kCross, true, t3);
    CHECK(dpad.reported(t3 + std::chrono::seconds(1)) == kCross);
    dpad.setButtons(tox2::InputSource::Script, kDown, false, t3 + std::chrono::seconds(1));
    CHECK(dpad.reported(t3 + std::chrono::seconds(2)) == (kUp | kCross));

    // releaseAll drops one source at once and leaves the other alone.
    const State::Clock::time_point t4 = t3 + std::chrono::seconds(10);
    State sources;
    sources.setButtons(tox2::InputSource::Keyboard, kCross, true, t4);
    sources.setButtons(tox2::InputSource::Script, kStart, true, t4);
    CHECK(sources.held() == (kCross | kStart));
    sources.releaseAll(tox2::InputSource::Keyboard);
    CHECK(sources.held() == kStart);
    CHECK(sources.reported(t4 + milliseconds(5)) == kStart);  // Cross is not kept for 60 ms
    sources.releaseAll(tox2::InputSource::Keyboard);          // nothing left to release
    CHECK(sources.held() == kStart);
    // A script tap keeps its minimum visible time; a button held by two sources stays until both let go.
    sources.setButtons(tox2::InputSource::Script, kStart, false, t4 + milliseconds(10));
    CHECK(sources.held() == 0 && sources.reported(t4 + milliseconds(50)) == kStart);
    CHECK(sources.reported(t4 + milliseconds(70)) == 0);
    sources.setButtons(tox2::InputSource::Keyboard, kCross, true, t4 + std::chrono::seconds(1));
    sources.setButtons(tox2::InputSource::Script, kCross, true, t4 + std::chrono::seconds(1));
    sources.setButtons(tox2::InputSource::Script, kCross, false, t4 + std::chrono::seconds(2));
    CHECK(sources.held() == kCross);

    // The tail of a released key never swallows a newly pressed opposite key.
    const State::Clock::time_point t5 = t4 + std::chrono::seconds(10);
    State menu;
    CHECK(menu.keyEvent(0xE048, true, false, t5));                     // Up down
    CHECK(menu.keyEvent(0xE048, false, true, t5 + milliseconds(10)));  // Up up
    CHECK(menu.keyEvent(0xE050, true, false, t5 + milliseconds(20)));  // Down down
    CHECK(menu.reported(t5 + milliseconds(20)) == kDown);
    CHECK(menu.reported(t5 + milliseconds(40)) == kDown);
    CHECK(menu.keyEvent(0xE050, false, true, t5 + milliseconds(30)));
    // Both only tails: the more recent press remains.
    CHECK(menu.reported(t5 + milliseconds(35)) == kDown);
    CHECK(menu.reported(t5 + milliseconds(75)) == kDown);
    CHECK(menu.reported(t5 + milliseconds(85)) == 0);

    // releaseAll of one source keeps the tail of a tap made by the other.
    const State::Clock::time_point t6 = t5 + std::chrono::seconds(10);
    State tails;
    tails.setButtons(tox2::InputSource::Script, kStart, true, t6);
    tails.setButtons(tox2::InputSource::Script, kStart, false, t6 + milliseconds(10));
    tails.setButtons(tox2::InputSource::Keyboard, kStart, true, t6 + milliseconds(20));
    tails.releaseAll(tox2::InputSource::Keyboard);
    CHECK(tails.held() == 0 && tails.reported(t6 + milliseconds(30)) == kStart);
    CHECK(tails.reported(t6 + milliseconds(70)) == 0);
}

void testKeyboardPadProvider() {
    using State = tox2::KeyboardPadState;
    using tox2::PadButton;
    constexpr std::uint32_t kZ = 0x2C;

    // The provider reads the state through the injected clock.
    auto state = std::make_shared<State>();
    State::Clock::time_point now = State::Clock::time_point{} + std::chrono::seconds(50);
    std::unique_ptr<tox2::PadProvider> provider = tox2::makeKeyboardPadProvider(state, [&] { return now; });
    CHECK(samePadState(provider->poll(0), tox2::neutralPadState()));
    CHECK(provider->poll(0).connected && !provider->poll(1).connected && !provider->poll(6).connected);
    CHECK(state->keyEvent(kZ, true, false, now));
    CHECK(samePadState(provider->poll(0), tox2::padStateFromButtons(tox2::padButtonBit(PadButton::Cross))));
    CHECK(!provider->poll(1).connected);
    now += std::chrono::seconds(2);
    CHECK(state->keyEvent(kZ, false, false, now));
    now += std::chrono::seconds(1);
    CHECK(samePadState(provider->poll(0), tox2::neutralPadState()));

    // The default clock is the real one: a key held now shows up.
    auto live_state = std::make_shared<State>();
    std::unique_ptr<tox2::PadProvider> live = tox2::makeKeyboardPadProvider(live_state);
    live_state->keyEvent(kZ, true, false, State::Clock::now());
    CHECK(live->poll(0).button[tox2::kPadButtonDigital2] == 0x40);

    // Through the cellPad handlers, as a game polls it.
    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::resetLogOnce();
    tox2::resetCellPad();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t data = scratch;

    enum Import : std::uint32_t { Init, End, SetPortSetting, GetData, GetInfo2, SetActDirect, ImportCount };
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x1cf98800, "sys_io", "cellPadInit"},
            {0x00800010, 0x4d9b75d5, "sys_io", "cellPadEnd"},
            {0x00800020, 0x578e3c98, "sys_io", "cellPadSetPortSetting"},
            {0x00800030, 0x8b72cda1, "sys_io", "cellPadGetData"},
            {0x00800040, 0xa703a51d, "sys_io", "cellPadGetInfo2"},
            {0x00800050, 0xf65544ee, "sys_io", "cellPadSetActDirect"},
    };
    tox2::installImports(imports, ImportCount);
    auto ctx = std::make_unique<Tox2PpuContext>();
    auto call = [&](Import which, std::uint64_t a = 0, std::uint64_t b = 0) {
        ctx->gpr[3] = a;
        ctx->gpr[4] = b;
        tox2HleCall(ctx.get(), base, which);
        return static_cast<std::uint32_t>(ctx->gpr[3]);
    };
    const auto len = [&] { return static_cast<std::int32_t>(tox2Read32(base, data)); };
    const auto word = [&](std::uint32_t i) { return tox2Read16(base, data + 4 + 2 * i); };

    auto pad_state = std::make_shared<State>();
    State::Clock::time_point pad_now = State::Clock::time_point{} + std::chrono::seconds(10);
    tox2::setPadProvider(tox2::makeKeyboardPadProvider(pad_state, [&] { return pad_now; }));
    CHECK(call(Init, 4) == 0);
    CHECK(call(GetData, 0, data) == 0 && len() == 8 && word(2) == 0 && word(3) == 0);
    CHECK(word(4) == 0x80 && word(7) == 0x80);
    CHECK(call(GetData, 0, data) == 0 && len() == 0);
    CHECK(call(GetData, 1, data) == 0x80121107);  // port 1: no device

    CHECK(pad_state->keyEvent(kZ, true, false, pad_now));
    CHECK(call(GetData, 0, data) == 0 && len() == 8 && word(3) == 0x40 && word(2) == 0);
    CHECK(call(GetData, 0, data) == 0 && len() == 0);
    CHECK(pad_state->keyEvent(0xE048, true, false, pad_now));
    CHECK(call(GetData, 0, data) == 0 && len() == 8 && word(2) == 0x10 && word(3) == 0x40);

    // With press mode on the pressed buttons report 0xFF in their pressure words.
    CHECK(call(SetPortSetting, 0, 2) == 0);
    CHECK(call(GetData, 0, data) == 0);  // a changed setting alone changes nothing the game compared
    CHECK(pad_state->keyEvent(kZ, false, false, pad_now));
    pad_now += std::chrono::seconds(1);
    CHECK(call(GetData, 0, data) == 0 && len() == 20 && word(3) == 0 && word(2) == 0x10);
    CHECK(word(10) == 0xFF && word(14) == 0);

    tox2::resetCellPad();
    p.main.free(scratch);
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

void testKeyboardPadThreads() {
    using State = tox2::KeyboardPadState;
    constexpr std::uint32_t kZ = 0x2C;
    constexpr int kToggles = 10000;

    auto state = std::make_shared<State>();
    std::unique_ptr<tox2::PadProvider> provider = tox2::makeKeyboardPadProvider(state);
    std::atomic<bool> done{false};
    std::atomic<bool> started{false};
    // Two downs then an up: repeated downs of a held key are ignored, and the last toggle is a down.
    bool last_down = false;
    std::thread writer([&] {
        // Starts only after the first poll, so writes and polls really overlap.
        while (!started.load()) std::this_thread::yield();
        for (int i = 0; i < kToggles; i++) {
            last_down = i % 3 != 2;
            state->keyEvent(kZ, last_down, false, State::Clock::now());
        }
        done.store(true);
    });
    std::uint64_t polls = 0;
    while (!done.load()) {
        const tox2::PadPortState polled = provider->poll(0);
        CHECK(polled.connected);
        polls++;
        started.store(true);
    }
    writer.join();
    const tox2::PadButtons expected = last_down ? tox2::padButtonBit(tox2::PadButton::Cross) : 0;
    CHECK(last_down);
    CHECK(state->held() == expected);
    CHECK(provider->poll(0).button[tox2::kPadButtonDigital2] == 0x40);
    CHECK(polls > 0);
}

void testPadStateFromInput() {
    using tox2::PadButton;
    constexpr tox2::PadButtons kL2 = tox2::padButtonBit(PadButton::L2);
    constexpr tox2::PadButtons kR2 = tox2::padButtonBit(PadButton::R2);
    constexpr tox2::PadButtons kCross = tox2::padButtonBit(PadButton::Cross);

    // An idle pad is the neutral state word for word, so GetData keeps reporting no change.
    CHECK(samePadState(tox2::padStateFromInput(0, {}, 0), tox2::neutralPadState()));
    CHECK(samePadState(tox2::padStateFromInput(kCross, {}, 0), tox2::padStateFromButtons(kCross)));

    // Words 4..7 are right x, right y, left x, left y (hle.h); nothing else changes.
    tox2::PadAnalog sticks;
    sticks.left_x = 0x11;
    sticks.left_y = 0x22;
    sticks.right_x = 0x33;
    sticks.right_y = 0x44;
    tox2::PadPortState expected = tox2::neutralPadState();
    expected.button[4] = 0x33;
    expected.button[5] = 0x44;
    expected.button[6] = 0x11;
    expected.button[7] = 0x22;
    CHECK(samePadState(tox2::padStateFromInput(0, sticks, 0), expected));
    // Trigger values alone do nothing without the button.
    tox2::PadAnalog triggers;
    triggers.l2 = 0x40;
    triggers.r2 = 0x90;
    CHECK(samePadState(tox2::padStateFromInput(0, triggers, kL2 | kR2), tox2::neutralPadState()));

    // The L2/R2 pressure words (18, 19) take the analog value only for buttons in the mask.
    const std::size_t l2_word = tox2::padPressureWord(PadButton::L2);
    const std::size_t r2_word = tox2::padPressureWord(PadButton::R2);
    CHECK(l2_word == 18 && r2_word == 19);
    tox2::PadPortState both = tox2::padStateFromInput(kL2 | kR2 | kCross, triggers, kL2 | kR2);
    CHECK(both.button[l2_word] == 0x40 && both.button[r2_word] == 0x90 && both.button[14] == 0xFF);
    tox2::PadPortState one = tox2::padStateFromInput(kL2 | kR2, triggers, kL2);
    CHECK(one.button[l2_word] == 0x40 && one.button[r2_word] == 0xFF);
    tox2::PadPortState none = tox2::padStateFromInput(kL2 | kR2, triggers, 0);
    CHECK(none.button[l2_word] == 0xFF && none.button[r2_word] == 0xFF);
    CHECK(one.button[tox2::kPadButtonDigital2] == 0x03);
}

void testGamepadSourceState() {
    using State = tox2::KeyboardPadState;
    using std::chrono::milliseconds;
    using tox2::PadButton;
    constexpr tox2::PadButtons kCross = tox2::padButtonBit(PadButton::Cross);
    constexpr tox2::PadButtons kStart = tox2::padButtonBit(PadButton::Start);
    constexpr tox2::PadButtons kUp = tox2::padButtonBit(PadButton::Up);
    constexpr tox2::PadButtons kDown = tox2::padButtonBit(PadButton::Down);
    const State::Clock::time_point t0 = State::Clock::time_point{} + std::chrono::seconds(100);

    // setHeld stamps only newly set bits: Cross keeps the time of its first report.
    State state;
    state.setHeld(tox2::InputSource::Gamepad, kCross, t0);
    CHECK(state.held(tox2::InputSource::Gamepad) == kCross && state.held(tox2::InputSource::Keyboard) == 0 &&
          state.held() == kCross);
    const State::Clock::time_point t1 = t0 + std::chrono::seconds(1);
    state.setHeld(tox2::InputSource::Gamepad, kCross | kStart, t1);
    state.setHeld(tox2::InputSource::Gamepad, kStart, t1 + milliseconds(10));
    CHECK(state.held() == kStart);
    CHECK(state.reported(t1 + milliseconds(30)) == kStart);  // Cross was pressed 1 s ago: no tail left
    // A released bit keeps its tail from the press time, then goes.
    state.setHeld(tox2::InputSource::Gamepad, 0, t1 + milliseconds(20));
    CHECK(state.held() == 0);
    CHECK(state.reported(t1 + milliseconds(30)) == kStart);
    CHECK(state.reported(t1 + milliseconds(59)) == kStart);
    CHECK(state.reported(t1 + milliseconds(70)) == 0);

    // A tap shorter than one poll is still seen, and a held bit is not re-stamped by the next report.
    const State::Clock::time_point t2 = t1 + std::chrono::seconds(10);
    state.setHeld(tox2::InputSource::Gamepad, kCross, t2);
    state.setHeld(tox2::InputSource::Gamepad, kCross, t2 + milliseconds(50));
    state.setHeld(tox2::InputSource::Gamepad, 0, t2 + milliseconds(55));
    CHECK(state.reported(t2 + milliseconds(59)) == kCross);
    CHECK(state.reported(t2 + milliseconds(61)) == 0);

    // The d-pad rule applies to the gamepad source: Up on the pad with Down on the keyboard cancel.
    const State::Clock::time_point t3 = t2 + std::chrono::seconds(10);
    State dpad;
    dpad.setHeld(tox2::InputSource::Gamepad, kUp, t3);
    CHECK(dpad.reported(t3 + std::chrono::seconds(1)) == kUp);
    dpad.setButtons(tox2::InputSource::Keyboard, kDown, true, t3);
    CHECK(dpad.held() == (kUp | kDown));
    CHECK(dpad.reported(t3 + std::chrono::seconds(1)) == 0);
    dpad.setHeld(tox2::InputSource::Gamepad, 0, t3 + std::chrono::seconds(2));
    CHECK(dpad.reported(t3 + std::chrono::seconds(3)) == kDown);

    // releaseAll(Gamepad) drops the gamepad's buttons at once and leaves the keyboard alone.
    const State::Clock::time_point t4 = t3 + std::chrono::seconds(10);
    State released;
    released.setHeld(tox2::InputSource::Gamepad, kCross, t4);
    released.setButtons(tox2::InputSource::Keyboard, kStart, true, t4);
    released.releaseAll(tox2::InputSource::Gamepad);
    CHECK(released.held() == kStart && released.reported(t4 + milliseconds(5)) == kStart);

    // Analog snapshot, connection flag and vibration start neutral and round-trip exactly.
    State values;
    CHECK(values.analog() == tox2::PadAnalog{} && !values.gamepadConnected());
    const std::pair<std::uint8_t, std::uint8_t> no_vibration{0, 0};
    CHECK(values.vibration() == no_vibration);
    tox2::PadAnalog analog;
    analog.left_x = 1;
    analog.left_y = 2;
    analog.right_x = 3;
    analog.right_y = 4;
    analog.l2 = 5;
    analog.r2 = 255;
    values.setAnalog(analog);
    CHECK(values.analog() == analog);
    values.setAnalog({});
    CHECK(values.analog() == tox2::PadAnalog{});
    values.setGamepadConnected(true);
    CHECK(values.gamepadConnected());
    values.setGamepadConnected(false);
    CHECK(!values.gamepadConnected());
    const std::pair<std::uint8_t, std::uint8_t> motors{7, 200};
    values.setVibration(7, 200);
    CHECK(values.vibration() == motors);

    // Script sticks merge with the gamepad's per axis, the value farther from 0x80 winning (a tie
    // keeps the gamepad's); centred, they leave the gamepad snapshot untouched.
    State merged;
    tox2::PadAnalog pad;
    pad.left_x = 0xC0;
    pad.left_y = 0x70;
    pad.right_x = 0x40;
    pad.l2 = 0x55;
    merged.setAnalog(pad);
    CHECK(merged.analog() == pad);
    merged.setScriptStick(tox2::PadStick::Left, 0x10, 0x80);
    CHECK(merged.analog().left_x == 0x10 && merged.analog().left_y == 0x70);
    merged.setScriptStick(tox2::PadStick::Left, 0x80, 0x00);
    CHECK(merged.analog().left_x == 0xC0 && merged.analog().left_y == 0x00);
    merged.setScriptStick(tox2::PadStick::Right, 0xC0, 0x80);  // tie with the gamepad's 0x40 distance
    CHECK(merged.analog().right_x == 0x40 && merged.analog().right_y == 0x80);
    CHECK(merged.analog().l2 == 0x55 && merged.analog().r2 == 0);
    merged.setScriptStick(tox2::PadStick::Left, 0x80, 0x80);
    merged.setScriptStick(tox2::PadStick::Right, 0x80, 0x80);
    CHECK(merged.analog() == pad);
    State script_only;
    script_only.setScriptStick(tox2::PadStick::Right, 0xFF, 0x00);
    CHECK(script_only.analog().right_x == 0xFF && script_only.analog().right_y == 0x00);
    CHECK(script_only.analog().left_x == 0x80 && script_only.analog().left_y == 0x80);
}

void testGamepadPadProvider() {
    using State = tox2::KeyboardPadState;
    using std::chrono::milliseconds;
    using tox2::PadButton;
    constexpr tox2::PadButtons kL2 = tox2::padButtonBit(PadButton::L2);
    const std::size_t l2_word = tox2::padPressureWord(PadButton::L2);
    const std::size_t r2_word = tox2::padPressureWord(PadButton::R2);

    auto state = std::make_shared<State>();
    State::Clock::time_point now = State::Clock::time_point{} + std::chrono::seconds(50);
    std::unique_ptr<tox2::PadProvider> provider = tox2::makeKeyboardPadProvider(state, [&] { return now; });
    CHECK(samePadState(provider->poll(0), tox2::neutralPadState()));

    // A trigger alone reports its analog value; sticks come from the snapshot.
    tox2::PadAnalog analog;
    analog.l2 = 0x55;
    analog.right_x = 0x10;
    analog.left_y = 0xF0;
    state->setHeld(tox2::InputSource::Gamepad, kL2, now);
    state->setAnalog(analog);
    tox2::PadPortState polled = provider->poll(0);
    CHECK(polled.button[tox2::kPadButtonDigital2] == 0x01);
    CHECK(polled.button[l2_word] == 0x55 && polled.button[r2_word] == 0);
    CHECK(polled.button[4] == 0x10 && polled.button[5] == 0x80 && polled.button[6] == 0x80 && polled.button[7] == 0xF0);

    // The keyboard or the script holding L2 as well makes it fully pressed, even with a half trigger.
    state->setButtons(tox2::InputSource::Keyboard, kL2, true, now);
    CHECK(provider->poll(0).button[l2_word] == 0xFF);
    state->setButtons(tox2::InputSource::Keyboard, kL2, false, now);
    CHECK(provider->poll(0).button[l2_word] == 0x55);
    state->setButtons(tox2::InputSource::Script, kL2, true, now);
    CHECK(provider->poll(0).button[l2_word] == 0xFF);
    state->setButtons(tox2::InputSource::Script, kL2, false, now);

    // A trigger released within the minimum visible time keeps the bit, now fully pressed.
    now += std::chrono::seconds(1);
    state->setHeld(tox2::InputSource::Gamepad, 0, now);
    state->setHeld(tox2::InputSource::Gamepad, kL2, now);
    state->setHeld(tox2::InputSource::Gamepad, 0, now + milliseconds(10));
    state->setAnalog({});
    now += milliseconds(20);
    polled = provider->poll(0);
    CHECK(polled.button[tox2::kPadButtonDigital2] == 0x01 && polled.button[l2_word] == 0xFF);
    now += milliseconds(100);
    CHECK(samePadState(provider->poll(0), tox2::neutralPadState()));

    // Vibration reaches the shared state for port 0 only.
    const std::pair<std::uint8_t, std::uint8_t> motors{3, 4};
    provider->setVibration(0, 3, 4);
    provider->setVibration(1, 9, 9);
    CHECK(state->vibration() == motors);

    // always_connected = false: port 0 follows the gamepad connection and is a plain empty port otherwise.
    auto pad_only = std::make_shared<State>();
    std::unique_ptr<tox2::PadProvider> gamepad_provider =
            tox2::makeKeyboardPadProvider(pad_only, [&] { return now; }, {.always_connected = false});
    CHECK(!gamepad_provider->poll(0).connected);
    pad_only->setGamepadConnected(true);
    CHECK(samePadState(gamepad_provider->poll(0), tox2::neutralPadState()));
    CHECK(!gamepad_provider->poll(1).connected);
    pad_only->setGamepadConnected(false);
    CHECK(!gamepad_provider->poll(0).connected);
    CHECK(provider->poll(0).connected);  // the default provider ignores the flag
}

void testGamepadCellPad() {
    using State = tox2::KeyboardPadState;
    using tox2::PadButton;
    constexpr tox2::PadButtons kCross = tox2::padButtonBit(PadButton::Cross);
    constexpr tox2::PadButtons kL2 = tox2::padButtonBit(PadButton::L2);

    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::resetLogOnce();
    tox2::resetCellPad();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t info = scratch, data = scratch + 0x200, act = scratch + 0x400;

    enum Import : std::uint32_t { Init, End, SetPortSetting, GetData, GetInfo2, SetActDirect, ImportCount };
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x1cf98800, "sys_io", "cellPadInit"},
            {0x00800010, 0x4d9b75d5, "sys_io", "cellPadEnd"},
            {0x00800020, 0x578e3c98, "sys_io", "cellPadSetPortSetting"},
            {0x00800030, 0x8b72cda1, "sys_io", "cellPadGetData"},
            {0x00800040, 0xa703a51d, "sys_io", "cellPadGetInfo2"},
            {0x00800050, 0xf65544ee, "sys_io", "cellPadSetActDirect"},
    };
    tox2::installImports(imports, ImportCount);
    auto ctx = std::make_unique<Tox2PpuContext>();
    auto call = [&](Import which, std::uint64_t a = 0, std::uint64_t b = 0) {
        ctx->gpr[3] = a;
        ctx->gpr[4] = b;
        tox2HleCall(ctx.get(), base, which);
        return static_cast<std::uint32_t>(ctx->gpr[3]);
    };
    constexpr std::uint32_t kNoDevice = 0x80121107;
    const auto len = [&] { return static_cast<std::int32_t>(tox2Read32(base, data)); };
    const auto word = [&](std::uint32_t i) { return tox2Read16(base, data + 4 + 2 * i); };

    auto state = std::make_shared<State>();
    State::Clock::time_point now = State::Clock::time_point{} + std::chrono::seconds(10);
    tox2::setPadProvider(tox2::makeKeyboardPadProvider(state, [&] { return now; }, {.always_connected = false}));
    CHECK(call(Init, 4) == 0);
    CHECK(call(SetPortSetting, 0, 2) == 0);

    // No gamepad yet: nothing connected, and the game is told nothing.
    CHECK(call(GetInfo2, info) == 0 && tox2Read32(base, info + 4) == 0 && tox2Read32(base, info + 12) == 0);
    CHECK(call(GetData, 0, data) == kNoDevice);
    std::memset(base + act, 0, 8);
    base[act] = 1;
    base[act + 1] = 200;
    CHECK(call(SetActDirect, 0, act) == kNoDevice);

    // Plugging in shows the assign-changed flag once; the first report then carries everything.
    state->setGamepadConnected(true);
    CHECK(call(GetInfo2, info) == 0 && tox2Read32(base, info + 4) == 1 && tox2Read32(base, info + 12) == 3);
    CHECK(call(GetInfo2, info) == 0 && tox2Read32(base, info + 12) == 1);
    CHECK(call(GetData, 0, data) == 0 && len() == 20 && word(2) == 0 && word(3) == 0);
    CHECK(word(4) == 0x80 && word(7) == 0x80);

    // An idle gamepad source (empty mask, default analog) reports no change, however often it is set.
    state->setHeld(tox2::InputSource::Gamepad, 0, now);
    state->setAnalog({});
    CHECK(call(GetData, 0, data) == 0 && len() == 0);
    now += std::chrono::seconds(1);
    state->setHeld(tox2::InputSource::Gamepad, 0, now);
    CHECK(call(GetData, 0, data) == 0 && len() == 0);

    // A trigger pull and a stick move are reported with their analog values.
    tox2::PadAnalog analog;
    analog.l2 = 0x77;
    analog.left_x = 0xC0;
    state->setHeld(tox2::InputSource::Gamepad, kL2 | kCross, now);
    state->setAnalog(analog);
    CHECK(call(GetData, 0, data) == 0 && len() == 20);
    CHECK(word(3) == 0x41 && word(6) == 0xC0 && word(18) == 0x77 && word(14) == 0xFF);
    state->setHeld(tox2::InputSource::Gamepad, 0, now);
    state->setAnalog({});
    now += std::chrono::seconds(1);
    CHECK(call(GetData, 0, data) == 0 && len() == 20 && word(3) == 0 && word(6) == 0x80);
    CHECK(call(GetData, 0, data) == 0 && len() == 0);

    // cellPadSetActDirect stores (small, large) for the gamepad thread.
    const std::pair<std::uint8_t, std::uint8_t> motors{1, 200};
    CHECK(call(SetActDirect, 0, act) == 0 && state->vibration() == motors);

    // Unplugging shows the flag once as well.
    state->setGamepadConnected(false);
    CHECK(call(GetInfo2, info) == 0 && tox2Read32(base, info + 4) == 0 && tox2Read32(base, info + 12) == 2);
    CHECK(call(GetInfo2, info) == 0 && tox2Read32(base, info + 12) == 0);
    CHECK(call(GetData, 0, data) == kNoDevice);

    tox2::resetCellPad();
    p.main.free(scratch);
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

struct ScriptEventSpec {
    int at;  // milliseconds or flips, as the script's clock says
    tox2::PadButtons buttons;
    bool down;
};

bool scriptEventsAre(const std::string& text, std::initializer_list<ScriptEventSpec> expected,
                     tox2::PadScriptClock clock = tox2::PadScriptClock::Seconds) {
    const auto parsed = tox2::parsePadScript(text);
    if (!parsed) {
        std::fprintf(stderr, "parse failed: %s\n", parsed.error().c_str());
        return false;
    }
    if (parsed->clock != clock || parsed->events.size() != expected.size()) return false;
    std::size_t i = 0;
    for (const ScriptEventSpec& want : expected) {
        const tox2::PadScriptEvent& got = parsed->events[i++];
        if (got.at != want.at || got.buttons != want.buttons || got.down != want.down) return false;
    }
    return true;
}

std::string scriptError(const std::string& text) {
    const auto parsed = tox2::parsePadScript(text);
    return parsed ? std::string{} : parsed.error();
}

void testPadScriptParse() {
    using tox2::PadButton;
    constexpr tox2::PadButtons kStart = tox2::padButtonBit(PadButton::Start);
    constexpr tox2::PadButtons kCross = tox2::padButtonBit(PadButton::Cross);
    constexpr tox2::PadButtons kR1 = tox2::padButtonBit(PadButton::R1);
    constexpr tox2::PadButtons kCircle = tox2::padButtonBit(PadButton::Circle);
    constexpr int kHold = static_cast<int>(tox2::kPadScriptTapHold.count());
    CHECK(tox2::kPadScriptTapHold > tox2::KeyboardPadState::kMinPressVisible);

    // Comments, blank lines, tabs, CRLF and a missing final newline.
    CHECK(scriptEventsAre("", {}));
    CHECK(scriptEventsAre("# only a comment\n\n   \t\n", {}));
    CHECK(scriptEventsAre("1 press start # trailing comment\r\n\t2.5\tpress\t cross",
                          {{1000, kStart, true}, {2500, kCross, true}}));

    // press / release, decimal times (also rounding past the third decimal), several buttons.
    CHECK(
            scriptEventsAre("40 press start\n40.5 release start\n0.25 press cross+r1\n0.0004 release r1\n"
                            "0.0005 release r1",
                            {{0, kR1, false},
                             {1, kR1, false},
                             {250, kCross | kR1, true},
                             {40000, kStart, true},
                             {40500, kStart, false}}));

    // tap = press and release one hold later; the sort is by time.
    CHECK(scriptEventsAre("3 tap start\n1 tap cross+r1\n", {{1000, kCross | kR1, true},
                                                            {1000 + kHold, kCross | kR1, false},
                                                            {3000, kStart, true},
                                                            {3000 + kHold, kStart, false}}));

    // Repeated taps at time + k * interval while <= until: 1, 3, 5.
    CHECK(scriptEventsAre("1 tap start every 2 until 6", {{1000, kStart, true},
                                                          {1000 + kHold, kStart, false},
                                                          {3000, kStart, true},
                                                          {3000 + kHold, kStart, false},
                                                          {5000, kStart, true},
                                                          {5000 + kHold, kStart, false}}));
    // `until` is inclusive (5 here); a lone tap when no repeat fits before it.
    CHECK(scriptEventsAre("1 tap start every 2 until 5", {{1000, kStart, true},
                                                          {1000 + kHold, kStart, false},
                                                          {3000, kStart, true},
                                                          {3000 + kHold, kStart, false},
                                                          {5000, kStart, true},
                                                          {5000 + kHold, kStart, false}}));
    CHECK(scriptEventsAre("1 tap start every 0.151 until 1", {{1000, kStart, true}, {1000 + kHold, kStart, false}}));

    // Equal times: releases before presses, otherwise file order (stable).
    CHECK(scriptEventsAre("2 press start\n2 release cross\n2 press cross\n2 release start\n",
                          {{2000, kCross, false}, {2000, kStart, false}, {2000, kStart, true}, {2000, kCross, true}}));
    // A release of one tap and the press of the next at the same instant also order release first.
    CHECK(scriptEventsAre("1 tap start\n1.15 tap start",
                          {{1000, kStart, true}, {1150, kStart, false}, {1150, kStart, true}, {1300, kStart, false}}));

    // The flip clock: `f<N>` times, a 9 flip tap hold, repeats in flips, and the same sort rules.
    using tox2::PadScriptClock;
    constexpr int kHoldFlips = static_cast<int>(tox2::kPadScriptTapHoldFlips);
    CHECK(kHoldFlips == 9);
    CHECK(scriptEventsAre("f1200 press circle\nf0 press start\nf1300 release circle",
                          {{0, kStart, true}, {1200, kCircle, true}, {1300, kCircle, false}}, PadScriptClock::Flips));
    CHECK(scriptEventsAre("f100 tap cross\nf50 tap start",
                          {{50, kStart, true},
                           {50 + kHoldFlips, kStart, false},
                           {100, kCross, true},
                           {100 + kHoldFlips, kCross, false}},
                          PadScriptClock::Flips));
    CHECK(scriptEventsAre("f10 tap start every f20 until f50",
                          {{10, kStart, true},
                           {10 + kHoldFlips, kStart, false},
                           {30, kStart, true},
                           {30 + kHoldFlips, kStart, false},
                           {50, kStart, true},
                           {50 + kHoldFlips, kStart, false}},
                          PadScriptClock::Flips));
    CHECK(scriptEventsAre("f10 tap start every f10 until f10", {{10, kStart, true}, {10 + kHoldFlips, kStart, false}},
                          PadScriptClock::Flips));
    // An interval of 10 flips is the shortest that clears the hold; releases still come before presses.
    CHECK(scriptEventsAre(
            "f1 tap start every f10 until f11",
            {{1, kStart, true}, {1 + kHoldFlips, kStart, false}, {11, kStart, true}, {11 + kHoldFlips, kStart, false}},
            PadScriptClock::Flips));
    CHECK(scriptEventsAre("f5 press start\nf5 release cross", {{5, kCross, false}, {5, kStart, true}},
                          PadScriptClock::Flips));
    CHECK(scriptEventsAre("f100000000 press start", {{100000000, kStart, true}}, PadScriptClock::Flips));
    // Comments and blank lines do not decide the clock; the first timed line does.
    CHECK(scriptEventsAre("# f1 is not a time here\n\nf7 press start", {{7, kStart, true}}, PadScriptClock::Flips));
    CHECK(scriptEventsAre("7 press start # f8", {{7000, kStart, true}}));

    // Stick events: both clocks, rounding, a sign, and the sort rule (only button releases move ahead).
    const auto left_up = tox2::parsePadScript("75 stick left 0 1");
    CHECK(left_up && left_up->clock == PadScriptClock::Seconds && left_up->events.size() == 1);
    if (left_up && left_up->events.size() == 1) {
        const tox2::PadScriptEvent& e = left_up->events[0];
        CHECK(e.at == 75000 && e.kind == tox2::PadScriptEventKind::Stick && e.stick == tox2::PadStick::Left);
        CHECK(e.x_milli == 0 && e.y_milli == 1000);
    }
    const auto right_flip = tox2::parsePadScript("f100 stick right -1 0.5");
    CHECK(right_flip && right_flip->clock == PadScriptClock::Flips && right_flip->events.size() == 1);
    if (right_flip && right_flip->events.size() == 1) {
        const tox2::PadScriptEvent& e = right_flip->events[0];
        CHECK(e.at == 100 && e.kind == tox2::PadScriptEventKind::Stick && e.stick == tox2::PadStick::Right);
        CHECK(e.x_milli == -1000 && e.y_milli == 500);
    }
    const auto rounded = tox2::parsePadScript("1 stick left 0.12345 -0.9995\n2 stick right -0.0004 1.0004\n");
    CHECK(rounded && rounded->events.size() == 2);
    if (rounded && rounded->events.size() == 2) {
        CHECK(rounded->events[0].x_milli == 123 && rounded->events[0].y_milli == -1000);
        CHECK(rounded->events[1].x_milli == 0 && rounded->events[1].y_milli == 1000);
    }
    // At one time: the release first, then the press and the stick in file order.
    const auto mixed_order =
            tox2::parsePadScript("2 stick left 1 0\n2 press start\n2 release cross\n2 stick right 0 1\n");
    CHECK(mixed_order && mixed_order->events.size() == 4);
    if (mixed_order && mixed_order->events.size() == 4) {
        const auto& ev = mixed_order->events;
        CHECK(ev[0].kind == tox2::PadScriptEventKind::Buttons && !ev[0].down && ev[0].buttons == kCross);
        CHECK(ev[1].kind == tox2::PadScriptEventKind::Stick && ev[1].stick == tox2::PadStick::Left);
        CHECK(ev[2].kind == tox2::PadScriptEventKind::Buttons && ev[2].down && ev[2].buttons == kStart);
        CHECK(ev[3].kind == tox2::PadScriptEventKind::Stick && ev[3].stick == tox2::PadStick::Right);
    }
    // A stick event does not count as a release against an earlier time either.
    const auto timed_order = tox2::parsePadScript("3 stick left 0 0\n1 stick left 0 1\n2 release cross\n");
    CHECK(timed_order && timed_order->events.size() == 3 && timed_order->events[0].at == 1000 &&
          timed_order->events[1].at == 2000 && timed_order->events[2].at == 3000);
}

void testPadScriptErrors() {
    CHECK(scriptError("1 press start\n\n3 press crosss\n") == "line 3: unknown button \"crosss\"");
    CHECK(scriptError("# header\n1 jump start") == "line 2: unknown command \"jump\"");
    CHECK(scriptError("soon press start") == "line 1: bad time \"soon\"");
    CHECK(scriptError("-1 press start").starts_with("line 1: bad time"));
    CHECK(scriptError("1.2.3 press start").starts_with("line 1: bad time"));
    CHECK(scriptError("1e3 press start").starts_with("line 1: bad time"));
    CHECK(scriptError("5. press start").starts_with("line 1: bad time"));
    CHECK(scriptError("99999999 press start").starts_with("line 1: bad time"));
    CHECK(scriptError("1").starts_with("line 1: missing command"));
    CHECK(scriptError("1 press").starts_with("line 1: \"press\" needs buttons"));
    CHECK(scriptError("1 press start+").starts_with("line 1: unknown button \"\""));
    CHECK(scriptError("1 press cross+cross+nope") == "line 1: unknown button \"nope\"");
    CHECK(scriptError("1 press start now").starts_with("line 1: unexpected \"now\""));

    // Interval no longer than the hold, missing or malformed `every` clauses.
    CHECK(scriptError("1 tap start every 0.15 until 5").starts_with("line 1: interval must be longer"));
    CHECK(scriptError("1 tap start every 0 until 5").starts_with("line 1: interval must be longer"));
    CHECK(scriptError("1 tap start every 2").starts_with("line 1: \"every\" needs \"until <time>\""));
    CHECK(scriptError("1 tap start every 2 till 5").starts_with("line 1: \"every\" needs \"until <time>\""));
    CHECK(scriptError("1 tap start every").starts_with("line 1: \"every\" needs an interval"));
    CHECK(scriptError("1 tap start every x until 5") == "line 1: bad interval \"x\"");
    CHECK(scriptError("1 tap start every 2 until").starts_with("line 1: \"until\" needs a time"));
    CHECK(scriptError("1 tap start every 2 until x") == "line 1: bad time \"x\"");
    CHECK(scriptError("4 tap start every 2 until 3").starts_with("line 1: \"until\" is before"));
    CHECK(scriptError("1 tap start often").starts_with("line 1: unexpected \"often\""));
    CHECK(scriptError("1 tap start every 2 until 5 extra").starts_with("line 1: unexpected \"extra\""));
    CHECK(scriptError("1 tap start every 0.2 until 999999").starts_with("line 1: more than"));

    // Stick commands: unknown stick, missing or extra tokens, non-numbers and out-of-range values.
    CHECK(scriptError("1 press start\n\n5 stick left 1.5 0") == "line 3: stick value \"1.5\" out of range -1..1");
    CHECK(scriptError("5 stick left 0 -1.001") == "line 1: stick value \"-1.001\" out of range -1..1");
    CHECK(scriptError("5 stick left 0 1.0005") == "line 1: stick value \"1.0005\" out of range -1..1");
    CHECK(scriptError("5 stick left 99999999999999999999 0").starts_with("line 1: stick value"));
    CHECK(scriptError("5 stick middle 0 0") == "line 1: unknown stick \"middle\"");
    CHECK(scriptError("5 stick left 0").starts_with("line 1: \"stick\" needs"));
    CHECK(scriptError("5 stick left").starts_with("line 1: \"stick\" needs"));
    CHECK(scriptError("5 stick").starts_with("line 1: \"stick\" needs"));
    CHECK(scriptError("5 stick left 0 0 0") == "line 1: unexpected \"0\"");
    CHECK(scriptError("5 stick left x 0") == "line 1: bad stick value \"x\"");
    CHECK(scriptError("5 stick left 0 1e0") == "line 1: bad stick value \"1e0\"");
    CHECK(scriptError("5 stick left 0 --1") == "line 1: bad stick value \"--1\"");
    CHECK(scriptError("5 stick left 0 .5") == "line 1: bad stick value \".5\"");
    CHECK(scriptError("5 stick left 1. 0") == "line 1: bad stick value \"1.\"");
    CHECK(scriptError("5 stick left 0 -").starts_with("line 1: bad stick value"));
    CHECK(scriptError("f5 stick left 0 0\n6 stick left 0 0") == "line 2: flip times and second times cannot be mixed");
    CHECK(scriptError("5 stick left 0 0 # comment\n6 stick right 1 -1\n").empty());

    // Flip times: malformed tokens, bounds, and no mixing with seconds.
    CHECK(scriptError("f press start") == "line 1: bad time \"f\"");
    CHECK(scriptError("f1.5 press start") == "line 1: bad time \"f1.5\"");
    CHECK(scriptError("f-1 press start").starts_with("line 1: bad time"));
    CHECK(scriptError("fx press start").starts_with("line 1: bad time"));
    CHECK(scriptError("F1 press start").starts_with("line 1: bad time"));
    CHECK(scriptError("f100000001 press start").starts_with("line 1: bad time"));
    CHECK(scriptError("f99999999999999999999 press start").starts_with("line 1: bad time"));
    CHECK(scriptError("f1 tap start every f9 until f50")
                  .starts_with("line 1: interval must be longer than the 9 flip"));
    CHECK(scriptError("f1 tap start every f0 until f50").starts_with("line 1: interval must be longer"));
    CHECK(scriptError("f1 tap start every f until f50") == "line 1: bad interval \"f\"");
    CHECK(scriptError("f1 tap start every f1.5 until f50") == "line 1: bad interval \"f1.5\"");
    CHECK(scriptError("f1 tap start every f10 until f1.5") == "line 1: bad time \"f1.5\"");
    CHECK(scriptError("f9 tap start every f10 until f1").starts_with("line 1: \"until\" is before"));
    CHECK(scriptError("f1 tap start every f10").starts_with("line 1: \"every\" needs \"until <time>\""));
    CHECK(scriptError("f1 tap start every f10 until f100000000").starts_with("line 1: more than"));

    constexpr const char* kMixed = "flip times and second times cannot be mixed";
    CHECK(scriptError("f1 press start\n\n# c\n2 press start\n") == std::string("line 4: ") + kMixed);
    CHECK(scriptError("1 press start\n2 press start\nf3 release start") == std::string("line 3: ") + kMixed);
    CHECK(scriptError("f1 tap start every 2 until f50") == std::string("line 1: ") + kMixed);
    CHECK(scriptError("1 tap start every f20 until 50") == std::string("line 1: ") + kMixed);
    CHECK(scriptError("f1 tap start every f20 until 50") == std::string("line 1: ") + kMixed);
    CHECK(scriptError("1 tap start every 2 until f50") == std::string("line 1: ") + kMixed);
    // A comment never decides the clock.
    CHECK(scriptError("# 1 press start\nf1 press start\nf2 release start").empty());

    const auto missing = tox2::loadPadScript(std::filesystem::path("no_such_dir") / "no_such_script.txt");
    CHECK(!missing && missing.error().starts_with("cannot open"));

    const std::filesystem::path file =
            std::filesystem::temp_directory_path() /
            std::format("tox2_pad_script_test_{}.txt",
                        std::chrono::steady_clock::now().time_since_epoch().count());  // unique under ctest -j
    {
        std::ofstream out(file, std::ios::binary);
        out << "# test\n2 tap start\n";
    }
    const auto loaded = tox2::loadPadScript(file);
    CHECK(loaded && loaded->events.size() == 2 && loaded->events[0].at == 2000);
    {
        std::ofstream out(file, std::ios::binary);
        out << "1 press start\n2 press bogus\n";
    }
    const auto bad = tox2::loadPadScript(file);
    CHECK(!bad && bad.error() == "line 2: unknown button \"bogus\"");
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
}

void testPadScriptPlay() {
    using tox2::PadButton;
    using std::chrono::milliseconds;
    constexpr tox2::PadButtons kStart = tox2::padButtonBit(PadButton::Start);
    constexpr tox2::PadButtons kCross = tox2::padButtonBit(PadButton::Cross);
    constexpr tox2::PadButtons kR1 = tox2::padButtonBit(PadButton::R1);

    const auto parsed = tox2::parsePadScript("1 press start\n2 press cross+r1\n3 release start\n4 release cross+r1\n");
    CHECK(parsed && parsed->events.size() == 4 && parsed->clock == tox2::PadScriptClock::Seconds);
    if (!parsed || parsed->events.size() != 4) return;

    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::setGuestThreadIdProvider(nullptr);
    tox2::KeyboardPadState state;
    std::vector<std::int64_t> waits;
    std::vector<tox2::PadButtons> held_before_wait;
    // No flip runs in this test, so the count stays what earlier tests left.
    const std::uint64_t flip_count = tox2::rsxFlipCount();
    tox2::playPadScript(*parsed, state, [&](std::int64_t at) {
        waits.push_back(at);
        held_before_wait.push_back(state.held());
    });
    tox2::setLogSink({});

    // wait_until runs before each event, so it sees the state left by the previous one.
    CHECK((waits == std::vector<std::int64_t>{1000, 2000, 3000, 4000}));
    CHECK((held_before_wait == std::vector<tox2::PadButtons>{0, kStart, kStart | kCross | kR1, kCross | kR1}));
    CHECK(state.held() == 0);

    CHECK(lines.size() == 4);
    if (lines.size() == 4) {
        CHECK(lines[0] == "[host] tox2: pad: script 1.000 s: press start (flip " + std::to_string(flip_count) + ")");
        CHECK(lines[1] == "[host] tox2: pad: script 2.000 s: press r1+cross (flip " + std::to_string(flip_count) + ")");
        CHECK(lines[2] == "[host] tox2: pad: script 3.000 s: release start (flip " + std::to_string(flip_count) + ")");
        CHECK(lines[3] ==
              "[host] tox2: pad: script 4.000 s: release r1+cross (flip " + std::to_string(flip_count) + ")");
    }

    // A flip script hands the flip numbers to the waiter in order and logs them as flips.
    const auto flips = tox2::parsePadScript("f1200 press cross+r1\nf1000 tap start\nf1300 release cross+r1\n");
    CHECK(flips && flips->clock == tox2::PadScriptClock::Flips && flips->events.size() == 4);
    if (flips && flips->events.size() == 4) {
        lines.clear();
        tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
        waits.clear();
        tox2::KeyboardPadState flip_state;
        tox2::playPadScript(*flips, flip_state, [&](std::int64_t at) { waits.push_back(at); });
        tox2::setLogSink({});
        CHECK((waits == std::vector<std::int64_t>{1000, 1009, 1200, 1300}));
        CHECK(flip_state.held() == 0);
        CHECK(lines.size() == 4);
        if (lines.size() == 4) {
            CHECK(lines[0] == "[host] tox2: pad: script f1000: press start");
            CHECK(lines[1] == "[host] tox2: pad: script f1009: release start");
            CHECK(lines[2] == "[host] tox2: pad: script f1200: press r1+cross");
            CHECK(lines[3] == "[host] tox2: pad: script f1300: release r1+cross");
        }
    }

    // Stick events move the script sticks and log like button events; the stick stays until moved again.
    const auto stick_script = tox2::parsePadScript("1 stick left 0 1\n2.5 stick right -0.25 0\n3 stick left 0 0\n");
    CHECK(stick_script && stick_script->events.size() == 3);
    if (stick_script && stick_script->events.size() == 3) {
        lines.clear();
        tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
        tox2::KeyboardPadState stick_state;
        std::vector<tox2::PadAnalog> analog_before_wait;
        tox2::playPadScript(*stick_script, stick_state,
                            [&](std::int64_t) { analog_before_wait.push_back(stick_state.analog()); });
        tox2::setLogSink({});
        CHECK(analog_before_wait.size() == 3);
        if (analog_before_wait.size() == 3) {
            CHECK(analog_before_wait[0] == tox2::PadAnalog{});
            // After the first event: up is CellPadData 0, x stays centred.
            CHECK(analog_before_wait[1].left_x == 0x80 && analog_before_wait[1].left_y == 0x00);
            CHECK(analog_before_wait[1].right_x == 0x80 && analog_before_wait[1].right_y == 0x80);
            CHECK(analog_before_wait[2].left_y == 0x00 && analog_before_wait[2].right_x == 0x60 &&
                  analog_before_wait[2].right_y == 0x80);
        }
        // The left stick is centred again; the right one stays where the script left it.
        CHECK(stick_state.analog().left_x == 0x80 && stick_state.analog().left_y == 0x80);
        CHECK(stick_state.analog().right_x == 0x60 && stick_state.analog().right_y == 0x80);
        CHECK(stick_state.held() == 0);
        CHECK(lines.size() == 3);
        if (lines.size() == 3) {
            CHECK(lines[0] ==
                  "[host] tox2: pad: script 1.000 s: stick left 0.000 1.000 (flip " + std::to_string(flip_count) + ")");
            CHECK(lines[1] == "[host] tox2: pad: script 2.500 s: stick right -0.250 0.000 (flip " +
                                      std::to_string(flip_count) + ")");
            CHECK(lines[2] ==
                  "[host] tox2: pad: script 3.000 s: stick left 0.000 0.000 (flip " + std::to_string(flip_count) + ")");
        }

        // Full deflection and the flip clock's log format.
        const auto corner = tox2::parsePadScript("f7 stick right 1 -1\n");
        CHECK(corner && corner->events.size() == 1);
        if (corner && corner->events.size() == 1) {
            lines.clear();
            tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
            tox2::KeyboardPadState corner_state;
            tox2::playPadScript(*corner, corner_state, [](std::int64_t) {});
            tox2::setLogSink({});
            CHECK(corner_state.analog().right_x == 0xFF && corner_state.analog().right_y == 0xFF);
            CHECK(lines.size() == 1 && lines[0] == "[host] tox2: pad: script f7: stick right 1.000 -1.000");
        }
    }

    // Script buttons are held in their own source: a keyboard focus loss does not drop them.
    tox2::KeyboardPadState mixed;
    const auto now = tox2::KeyboardPadState::Clock::now();
    mixed.setButtons(tox2::InputSource::Script, kStart, true, now);
    mixed.keyEvent(0x2C, true, false, now);
    mixed.releaseAll(tox2::InputSource::Keyboard);
    CHECK(mixed.held() == kStart);

    // The detached player thread applies the events on its own clock and keeps the state alive.
    auto shared = std::make_shared<tox2::KeyboardPadState>();
    tox2::startPadScript({}, shared);  // nothing to play: no thread, no change
    CHECK(shared->held() == 0);
    tox2::PadScript timed;
    timed.events = {{0, kCross, true}, {20, kStart, true}};
    tox2::startPadScript(timed, shared);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (shared->held() != (kCross | kStart) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(1));
    }
    CHECK(shared->held() == (kCross | kStart));

    // A flip script waits on the RSX flip count; a time that count has already reached applies at once.
    auto flip_shared = std::make_shared<tox2::KeyboardPadState>();
    tox2::PadScript flip_script;
    flip_script.clock = tox2::PadScriptClock::Flips;
    flip_script.events = {{0, kCross, true}};
    tox2::startPadScript(std::move(flip_script), flip_shared);
    const auto flip_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (flip_shared->held() != kCross && std::chrono::steady_clock::now() < flip_deadline) {
        std::this_thread::sleep_for(milliseconds(1));
    }
    CHECK(flip_shared->held() == kCross);
}

void testCellMsgDialog() {
    std::vector<std::string> log_lines;
    tox2::setLogSink([&](std::string_view line) { log_lines.emplace_back(line); });
    tox2::resetCensus();
    tox2::resetLogOnce();
    tox2::resetHostFunctions();
    tox2::resetCellSysutil();
    tox2::resetCellMsgDialog();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);

    enum Import : std::uint32_t { Open2, Close, BarReset, BarInc, Register, Check, ImportCount };
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x7603d3db, "cellSysutil", "cellMsgDialogOpen2"},
            {0x00800010, 0x20543730, "cellSysutil", "cellMsgDialogClose"},
            {0x00800020, 0x7bc2c8a8, "cellSysutil", "cellMsgDialogProgressBarReset"},
            {0x00800030, 0x94862702, "cellSysutil", "cellMsgDialogProgressBarInc"},
            {0x00800040, 0x9d98afa0, "cellSysutil", "cellSysutilRegisterCallback"},
            {0x00800050, 0x189a74da, "cellSysutil", "cellSysutilCheckCallback"},
    };
    tox2::installImports(imports, ImportCount);
    auto ctx = std::make_unique<Tox2PpuContext>();
    ctx->gpr[1] = 0x500000;
    CHECK(p.memory.commit(0x4f0000, 0x10000));
    auto call = [&](Import which, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0, std::uint64_t d = 0) {
        ctx->gpr[3] = a;
        ctx->gpr[4] = b;
        ctx->gpr[5] = c;
        ctx->gpr[6] = d;
        tox2HleCall(ctx.get(), base, which);
        return static_cast<std::uint32_t>(ctx->gpr[3]);
    };
    constexpr std::uint32_t kErrorParam = 0x8002b301, kErrorNotOpened = 0x8002b302, kErrorBusy = 0x8002b105;
    constexpr std::uint32_t kOk = 2u << 4, kYesNo = 1u << 4;
    const std::uint32_t text = scratch;
    auto setText = [&](const char* s) { std::memcpy(base + text, s, std::strlen(s) + 1); };
    auto logged = [&](std::string_view needle) {
        for (const std::string& line : log_lines) {
            if (line.find(needle) != std::string::npos) return true;
        }
        return false;
    };

    const std::uint32_t dialog_cb = tox2::registerHostFunction("msgDialogCallback", msgDialogCallback);
    const std::uint32_t sysutil_cb = tox2::registerHostFunction("msgDialogSysutilCallback", msgDialogSysutilCallback);
    g_dialog_results.clear();
    g_dialog_sysutil_events.clear();
    tox2::setDialogAnswerDelay(std::chrono::milliseconds(0));
    CHECK(call(Register, 0, sysutil_cb, 0) == 0);

    // Nothing is open.
    CHECK(call(Close) == kErrorNotOpened);
    CHECK(call(BarReset, 0) == kErrorNotOpened);
    CHECK(call(BarInc, 0, 1) == kErrorNotOpened);

    // Parameter checks: no text, unknown type bits, a cursor or bars where they do not belong.
    setText("hello");
    CHECK(call(Open2, kOk, 0, 0, 0) == kErrorParam);
    CHECK(call(Open2, 0x8, text, 0, 0) == kErrorParam);  // bit 3 is not a type bit
    CHECK(call(Open2, kOk | (1u << 8), text, 0, 0) == kErrorParam);
    CHECK(call(Open2, kYesNo | (3u << 8), text, 0, 0) == kErrorParam);
    CHECK(call(Open2, kYesNo | (1u << 12), text, 0, 0) == kErrorParam);
    CHECK(call(Open2, 3u << 12, text, 0, 0) == kErrorParam);  // more than two bars
    CHECK(call(Open2, 3u << 4, text, 0, 0) == kErrorParam);   // unknown button type
    std::memset(base + text, 'a', 512);
    base[text + 512] = 0;
    CHECK(call(Open2, kOk, text, 0, 0) == kErrorParam);  // text of CELL_MSGDIALOG_STRING_SIZE
    CHECK(g_dialog_sysutil_events.empty());

    // An OK dialog answers itself with OK; DRAWING_BEGIN/END surround it and the callback gets the
    // button and the user data. A second dialog is refused while the first one is open.
    setText("\xe3\x82\xbb\xe3\x83\xbc\xe3\x83\x96");  // UTF-8 text reaches the log verbatim
    CHECK(call(Open2, kOk, text, dialog_cb, 0x77) == 0);
    CHECK(logged("open type 0x20") && logged("\xe3\x82\xbb\xe3\x83\xbc\xe3\x83\x96"));
    CHECK(call(Open2, kOk, text, 0, 0) == kErrorBusy);
    CHECK(call(Check) == 0);
    CHECK(g_dialog_sysutil_events.size() == 1 && g_dialog_sysutil_events[0] == tox2::kSysutilDrawingBegin);
    CHECK(g_dialog_results.empty());
    CHECK(call(Check) == 0);
    CHECK(g_dialog_sysutil_events.size() == 2 && g_dialog_sysutil_events[1] == tox2::kSysutilDrawingEnd);
    CHECK(g_dialog_results.size() == 1 && g_dialog_results[0].first == 1 && g_dialog_results[0].second == 0x77);
    CHECK(ctx->gpr[1] == 0x500000);
    CHECK(call(Close) == kErrorNotOpened);

    // A Yes/No dialog answers YES by default and NO when asked to.
    g_dialog_results.clear();
    CHECK(call(Open2, kYesNo | (1u << 8), text, dialog_cb, 1) == 0);
    CHECK(call(Check) == 0 && call(Check) == 0);
    CHECK(g_dialog_results.size() == 1 && g_dialog_results[0].first == 1);
    CHECK(!tox2::setDialogAnswer("maybe"));
    CHECK(tox2::setDialogAnswer("no"));
    CHECK(call(Open2, kYesNo, text, dialog_cb, 2) == 0);
    CHECK(call(Check) == 0 && call(Check) == 0);
    CHECK(g_dialog_results.size() == 2 && g_dialog_results[1].first == 2 && g_dialog_results[1].second == 2);
    CHECK(tox2::setDialogAnswer("ok"));

    // A dialog without buttons waits for cellMsgDialogClose; Close(0) ends it with button NONE (-1).
    // Type 0x85 has no progress bar; 0x1000 has one.
    g_dialog_results.clear();
    CHECK(call(Open2, 0x85, text, dialog_cb, 3) == 0);
    for (int i = 0; i < 5; i++) CHECK(call(Check) == 0);
    CHECK(g_dialog_results.empty());
    CHECK(call(BarInc, 0, 1) == kErrorParam && call(BarReset, 0) == kErrorParam);
    ctx->fpr[1].f64 = 0.0;
    CHECK(call(Close) == 0);
    CHECK(call(Check) == 0 && call(Check) == 0);
    CHECK(g_dialog_results.size() == 1 && g_dialog_results[0].first == -1 && g_dialog_results[0].second == 3);

    g_dialog_results.clear();
    CHECK(call(Open2, 0x1000, text, dialog_cb, 4) == 0);
    CHECK(call(BarInc, 0, 5) == 0 && call(BarReset, 0) == 0);
    CHECK(call(BarInc, 1, 5) == kErrorParam);
    ctx->fpr[1].f64 = 1.0e9;  // a far-away deadline keeps the dialog open
    CHECK(call(Close) == 0);
    for (int i = 0; i < 3; i++) CHECK(call(Check) == 0);
    CHECK(g_dialog_results.empty() && call(Open2, 0x1000, text, 0, 0) == kErrorBusy);
    ctx->fpr[1].f64 = -5.0;  // a negative delay counts as 0; a later Close replaces the deadline
    CHECK(call(Close) == 0);
    CHECK(call(Check) == 0 && call(Check) == 0);
    CHECK(g_dialog_results.size() == 1 && g_dialog_results[0].first == -1);

    // After a close a new dialog opens again, and the events stayed paired.
    CHECK(call(Open2, 0x4, text, 0, 0) == 0);
    ctx->fpr[1].f64 = 0.0;
    CHECK(call(Close) == 0);
    CHECK(call(Check) == 0 && call(Check) == 0);
    CHECK(tox2::queueSysutilEvent(tox2::kSysutilDrawingEnd, 0) == -1);

    tox2::resetCellMsgDialog();
    tox2::resetCellSysutil();
    g_dialog_results.clear();
    g_dialog_sysutil_events.clear();
    tox2::resetHostFunctions();
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

// Script and observations shared by the save data callbacks below.
struct SaveFileOp {
    std::uint32_t operation = 0, type = 1, offset = 0, size = 0, buf_size = 0, buf = 0;
    std::string name;
};

struct SaveScript {
    // funcStat
    std::int32_t stat_result = 0;
    std::uint32_t set_param = 0, recreate = 0;
    std::uint32_t stat_calls = 0, stat_is_new = 0, stat_file_num = 0, stat_file_list_num = 0;
    std::int32_t stat_size_kb = 0, stat_hdd_free = 0;
    std::string stat_title, stat_dir;
    // funcFile
    std::vector<SaveFileOp> file_ops;
    std::size_t next_op = 0;
    std::int32_t file_final = 1;
    std::vector<std::uint32_t> file_exc;
    // funcList
    std::int32_t list_result = 0;
    std::uint32_t list_calls = 0, list_dir_num = 0, list_dir_list_num = 0, focus = 1, fixed_num = 0, fixed_list = 0,
                  new_data = 0, focus_dir = 0;
    // funcFixed
    std::int32_t fixed_result = 0;
    std::uint32_t fixed_name = 0, fixed_option = 0;
    std::uint32_t fixed_calls = 0, fixed_dir_num = 0, fixed_dir_list_num = 0, fixed_userdata = 0;
    std::vector<std::string> fixed_listed;
};
SaveScript g_save;

TOX2_PPU_FUNC(saveStatCallback) {
    const auto result = static_cast<std::uint32_t>(ctx->gpr[3]);
    const auto get = static_cast<std::uint32_t>(ctx->gpr[4]);
    const auto set = static_cast<std::uint32_t>(ctx->gpr[5]);
    g_save.stat_calls++;
    g_save.stat_hdd_free = static_cast<std::int32_t>(tox2Read32(base, get));
    g_save.stat_is_new = tox2Read32(base, get + 4);
    g_save.stat_dir = reinterpret_cast<const char*>(base + get + 32);
    g_save.stat_title = reinterpret_cast<const char*>(base + get + 64);
    g_save.stat_size_kb = static_cast<std::int32_t>(tox2Read32(base, get + 1620));
    g_save.stat_file_num = tox2Read32(base, get + 1628);
    g_save.stat_file_list_num = tox2Read32(base, get + 1632);
    tox2Write32(base, result, static_cast<std::uint32_t>(g_save.stat_result));
    tox2Write32(base, set, g_save.set_param);
    tox2Write32(base, set + 4, g_save.recreate);
}

TOX2_PPU_FUNC(saveFileCallback) {
    const auto result = static_cast<std::uint32_t>(ctx->gpr[3]);
    const auto get = static_cast<std::uint32_t>(ctx->gpr[4]);
    const auto set = static_cast<std::uint32_t>(ctx->gpr[5]);
    g_save.file_exc.push_back(tox2Read32(base, get));
    if (g_save.next_op >= g_save.file_ops.size()) {
        tox2Write32(base, result, static_cast<std::uint32_t>(g_save.file_final));
        return;
    }
    const SaveFileOp& op = g_save.file_ops[g_save.next_op++];
    tox2Write32(base, set, op.operation);
    tox2Write32(base, set + 8, op.type);
    // The file name goes into a spare area of the callback scratch block (after the structures).
    const std::uint32_t name_addr = result + 0x900;
    std::memcpy(base + name_addr, op.name.c_str(), op.name.size() + 1);
    tox2Write32(base, set + 28, op.name.empty() ? 0 : name_addr);
    tox2Write32(base, set + 32, op.offset);
    tox2Write32(base, set + 36, op.size);
    tox2Write32(base, set + 40, op.buf_size);
    tox2Write32(base, set + 44, op.buf);
    tox2Write32(base, result, 0);
}

TOX2_PPU_FUNC(saveListCallback) {
    const auto result = static_cast<std::uint32_t>(ctx->gpr[3]);
    const auto get = static_cast<std::uint32_t>(ctx->gpr[4]);
    const auto set = static_cast<std::uint32_t>(ctx->gpr[5]);
    g_save.list_calls++;
    g_save.list_dir_num = tox2Read32(base, get);
    g_save.list_dir_list_num = tox2Read32(base, get + 4);
    tox2Write32(base, result, static_cast<std::uint32_t>(g_save.list_result));
    tox2Write32(base, set, g_save.focus);
    tox2Write32(base, set + 4, g_save.focus_dir);
    tox2Write32(base, set + 8, g_save.fixed_num);
    tox2Write32(base, set + 12, g_save.fixed_list);
    tox2Write32(base, set + 16, g_save.new_data);
}

TOX2_PPU_FUNC(saveFixedCallback) {
    const auto result = static_cast<std::uint32_t>(ctx->gpr[3]);
    const auto get = static_cast<std::uint32_t>(ctx->gpr[4]);
    const auto set = static_cast<std::uint32_t>(ctx->gpr[5]);
    g_save.fixed_calls++;
    g_save.fixed_dir_num = tox2Read32(base, get);
    g_save.fixed_dir_list_num = tox2Read32(base, get + 4);
    g_save.fixed_userdata = tox2Read32(base, result + 16);
    const std::uint32_t list = tox2Read32(base, get + 8);
    g_save.fixed_listed.clear();
    for (std::uint32_t i = 0; i < g_save.fixed_dir_list_num; i++) {
        g_save.fixed_listed.emplace_back(reinterpret_cast<const char*>(base + list + 48 * i));
    }
    tox2Write32(base, result, static_cast<std::uint32_t>(g_save.fixed_result));
    tox2Write32(base, set, g_save.fixed_name);
    tox2Write32(base, set + 4, 0);
    tox2Write32(base, set + 8, g_save.fixed_option);
}

// A save dialog driver that records every request and returns queued answers. With nothing queued a list
// answers back and a message answers No.
struct ScriptedSaveDialog final : tox2::SaveDialogDriver {
    struct ListCall {
        tox2::SaveListKind kind = tox2::SaveListKind::Save;
        std::vector<std::string> titles, infos;
        std::vector<bool> has_icon, is_new;
        std::int32_t initial_row = 0;
    };
    std::vector<ListCall> lists;
    std::vector<std::string> messages;
    std::vector<tox2::DialogKind> message_kinds;
    std::deque<std::optional<std::int32_t>> list_answers;
    std::deque<tox2::SaveMessageResult> message_answers;

    void reset() {
        lists.clear();
        messages.clear();
        message_kinds.clear();
        list_answers.clear();
        message_answers.clear();
    }
    bool is_interactive = false;
    bool interactive() const override { return is_interactive; }
    std::optional<std::int32_t> showList(const tox2::SaveListRequest& request) override {
        ListCall call;
        call.kind = request.content->list_kind;
        call.initial_row = request.initial_row;
        for (const tox2::SaveDialogRow& row : request.content->rows) {
            call.titles.push_back(row.title);
            call.infos.push_back(row.info);
            call.has_icon.push_back(row.icon_png != nullptr);
            call.is_new.push_back(row.new_data);
        }
        lists.push_back(std::move(call));
        if (list_answers.empty()) return std::nullopt;
        const auto answer = list_answers.front();
        list_answers.pop_front();
        return answer;
    }
    tox2::SaveMessageResult showMessage(std::shared_ptr<tox2::SaveDialogContent> content) override {
        messages.push_back(content->message);
        message_kinds.push_back(content->kind);
        if (message_answers.empty()) return tox2::SaveMessageResult::No;
        const auto answer = message_answers.front();
        message_answers.pop_front();
        return answer;
    }
};

void testCellSaveData() {
    namespace stdfs = std::filesystem;
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetCensus();
    tox2::resetLogOnce();
    tox2::resetHostFunctions();
    tox2::resetCellSaveData();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t dir_addr = scratch, set_buf = scratch + 0x100, buf = scratch + 0x1000,
                        set_param = scratch + 0x4000, data = scratch + 0x6000, set_list = scratch + 0x7100,
                        prefix = scratch + 0x7200, new_data = scratch + 0x7300, new_dir = scratch + 0x7340,
                        fixed_list = scratch + 0x7400, focus_dir = scratch + 0x7500;

    const stdfs::path root =
            stdfs::temp_directory_path() / ("tox2_savedata_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    stdfs::remove_all(root, ec);
    stdfs::create_directories(root / "game", ec);
    tox2::configureVfs(root / "game", root / "hdd");
    const stdfs::path saves = root / "hdd" / "dev_hdd0" / "home" / "00000001" / "savedata";

    enum Import : std::uint32_t {
        AutoLoad,
        AutoSave,
        ListLoad,
        ListSave,
        Delete,
        ListAutoLoad,
        FixedLoad,
        FixedSave,
        ImportCount
    };
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0xfbd5c856, "cellSysutil", "cellSaveDataAutoLoad2"},
            {0x00800010, 0x8b7ed64b, "cellSysutil", "cellSaveDataAutoSave2"},
            {0x00800020, 0x1dfbfdd6, "cellSysutil", "cellSaveDataListLoad2"},
            {0x00800030, 0x2de0d663, "cellSysutil", "cellSaveDataListSave2"},
            {0x00800040, 0xedadd797, "cellSysutil", "cellSaveDataDelete2"},
            {0x00800050, 0x21425307, "cellSysutil", "cellSaveDataListAutoLoad"},
            {0x00800060, 0x2a8eada2, "cellSysutil", "cellSaveDataFixedLoad2"},
            {0x00800070, 0x2aae9ef5, "cellSysutil", "cellSaveDataFixedSave2"},
    };
    tox2::installImports(imports, ImportCount);
    auto ctx = std::make_unique<Tox2PpuContext>();
    ctx->gpr[1] = 0x500000;
    CHECK(p.memory.commit(0x4f0000, 0x11000));  // the stack and the caller's parameter save area above r1
    auto call = [&](Import which, std::initializer_list<std::uint64_t> args) {
        int reg = 3;
        for (const std::uint64_t arg : args) ctx->gpr[reg++] = arg;
        tox2HleCall(ctx.get(), base, which);
        return static_cast<std::uint32_t>(ctx->gpr[3]);
    };
    auto put = [&](std::uint32_t addr, std::string_view text) {
        std::memcpy(base + addr, text.data(), text.size());
        base[addr + text.size()] = 0;
    };
    auto logged = [&](std::string_view needle) {
        for (const std::string& line : lines) {
            if (line.find(needle) != std::string::npos) return true;
        }
        return false;
    };
    auto text = [&](const stdfs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };

    const std::uint32_t stat_cb = tox2::registerHostFunction("saveStatCallback", saveStatCallback);
    const std::uint32_t file_cb = tox2::registerHostFunction("saveFileCallback", saveFileCallback);
    const std::uint32_t list_cb = tox2::registerHostFunction("saveListCallback", saveListCallback);
    const std::uint32_t fixed_cb = tox2::registerHostFunction("saveFixedCallback", saveFixedCallback);
    constexpr std::uint32_t kErrCbResult = 0x8002b401, kErrParam = 0x8002b404, kErrFailure = 0x8002b407;

    // setBuf: dirListMax 4, fileListMax 8, bufSize 0x2000, buf.
    tox2Write32(base, set_buf, 4);
    tox2Write32(base, set_buf + 4, 8);
    tox2Write32(base, set_buf + 32, 0x2000);
    tox2Write32(base, set_buf + 36, buf);
    put(dir_addr, "SAVE0001");
    auto autoLoad = [&](std::uint32_t err_dialog = 0) {
        return call(AutoLoad, {0, dir_addr, err_dialog, set_buf, stat_cb, file_cb, 0, 0x1234});
    };
    auto autoSave = [&](std::uint32_t err_dialog = 0) {
        return call(AutoSave, {0, dir_addr, err_dialog, set_buf, stat_cb, file_cb, 0, 0x1234});
    };

    // Argument checks: lowercase name, version above 1, missing funcStat, bad errDialog, no setBuf.
    put(dir_addr, "save0001");
    CHECK(autoLoad() == kErrParam);
    put(dir_addr, "SAVE0001");
    CHECK(call(AutoLoad, {2, dir_addr, 0, set_buf, stat_cb, file_cb, 0, 0}) == kErrParam);
    CHECK(call(AutoLoad, {0, dir_addr, 0, set_buf, 0, file_cb, 0, 0}) == kErrParam);
    CHECK(autoLoad(3) == kErrParam);
    CHECK(call(AutoLoad, {0, dir_addr, 0, 0, stat_cb, file_cb, 0, 0}) == kErrParam);
    CHECK(call(AutoLoad, {0, 0, 0, set_buf, stat_cb, file_cb, 0, 0}) == kErrParam);
    CHECK(g_save.stat_calls == 0);

    // First boot: no save. funcStat sees isNewData and no files, and reports NODATA.
    g_save = SaveScript{};
    g_save.stat_result = -4;
    CHECK(autoLoad(0) == kErrCbResult);
    CHECK(g_save.stat_calls == 1 && g_save.stat_is_new == 1 && g_save.stat_file_num == 0 && g_save.stat_size_kb == 0 &&
          g_save.stat_dir == "SAVE0001" && g_save.stat_hdd_free == 40 * 1024 * 1024 - 256);
    CHECK(logged("there is no save data (no dialog requested)"));
    lines.clear();
    CHECK(autoLoad(1) == kErrCbResult);
    CHECK(logged("error dialog (headless, not shown): there is no save data"));
    lines.clear();
    CHECK(autoLoad(2) == kErrCbResult && logged("error dialog"));  // NOREPEAT: the first one shows
    lines.clear();
    CHECK(autoLoad(2) == kErrCbResult);  // the same error again raises no dialog
    CHECK(!logged("error dialog"));
    g_save.stat_result = 1;  // OK_LAST ends the call successfully
    CHECK(autoLoad(1) == 0);
    g_save.stat_result = 0;  // OK_NEXT for a new save without setParam is a parameter error
    CHECK(autoLoad(1) == kErrParam);
    g_save.stat_result = 9;  // out of range
    CHECK(autoLoad() == kErrParam);
    CHECK(!stdfs::exists(saves / "SAVE0001"));

    // Save: setParam, a secure file, a normal file and the icon.
    g_save = SaveScript{};
    put(set_param + 0, "My title");
    put(set_param + 128, "My subtitle");
    put(set_param + 256, "My detail");
    put(set_param + 1288, "LP-1");
    g_save.set_param = set_param;
    put(data, "hello");
    put(data + 0x100, "xyz");
    put(data + 0x200, "PNG!");
    g_save.file_ops = {{1, 0, 0, 5, 5, data, "DATA.BIN"},
                       {1, 1, 0, 3, 3, data + 0x100, "B.DAT"},
                       {1, 2, 0, 4, 4, data + 0x200, ""}};
    CHECK(autoSave() == 0);
    CHECK(g_save.stat_is_new == 1 && g_save.next_op == 3);
    CHECK(text(saves / "SAVE0001" / "DATA.BIN") == "hello" && text(saves / "SAVE0001" / "B.DAT") == "xyz" &&
          text(saves / "SAVE0001" / "ICON0.PNG") == "PNG!");
    CHECK(!stdfs::exists(saves / ".working_SAVE0001") && !stdfs::exists(saves / ".backup_SAVE0001"));
    {
        const auto sfo = tox2::readParamSfoFile(saves / "SAVE0001" / "PARAM.SFO");
        CHECK(sfo.has_value());
        if (sfo) {
            CHECK(sfo->strings.at("CATEGORY") == "SD" && sfo->strings.at("TITLE") == "My title" &&
                  sfo->strings.at("SUB_TITLE") == "My subtitle" && sfo->strings.at("DETAIL") == "My detail" &&
                  sfo->strings.at("SAVEDATA_DIRECTORY") == "SAVE0001" &&
                  sfo->strings.at("SAVEDATA_LIST_PARAM") == "LP-1");
            CHECK(sfo->ints.at("*DATA.BIN") == 1 && sfo->ints.at("*B.DAT") == 0 && sfo->ints.at("ATTRIBUTE") == 0);
        }
    }
    CHECK(g_save.file_exc.size() == 4 && g_save.file_exc[1] == 5 && g_save.file_exc[2] == 3);

    // Load: the stat step lists the files (PARAM.SFO excluded) and the READ returns the bytes.
    g_save = SaveScript{};
    std::memset(base + data + 0x400, 0, 16);
    g_save.file_ops = {{0, 0, 0, 5, 16, data + 0x400, "DATA.BIN"}};
    CHECK(autoLoad() == 0);
    CHECK(g_save.stat_is_new == 0 && g_save.stat_title == "My title" && g_save.stat_file_num == 3 &&
          g_save.stat_file_list_num == 3 && g_save.stat_size_kb == 3 + 35);
    CHECK(std::memcmp(base + data + 0x400, "hello", 6) == 0);
    CHECK(g_save.file_exc.size() == 2 && g_save.file_exc[1] == 5);
    // Sorted by name: B.DAT (normal), DATA.BIN (secure), ICON0.PNG (icon).
    CHECK(tox2Read32(base, buf + 0) == 1 && tox2Read32(base, buf + 56) == 0 && tox2Read32(base, buf + 112) == 2);
    CHECK(tox2Read64(base, buf + 8) == 3 && tox2Read64(base, buf + 64) == 5);
    CHECK(std::strcmp(reinterpret_cast<const char*>(base + buf + 40), "B.DAT") == 0);
    CHECK(std::strcmp(reinterpret_cast<const char*>(base + buf + 96), "DATA.BIN") == 0);
    CHECK(text(saves / "SAVE0001" / "DATA.BIN") == "hello");  // a load commits nothing

    // Reads of a missing file and past the end fail; callback result codes map to the CBRESULT error.
    g_save = SaveScript{};
    g_save.file_ops = {{0, 1, 0, 4, 4, data + 0x400, "NONE.DAT"}};
    CHECK(autoLoad() == kErrFailure);
    g_save = SaveScript{};
    g_save.file_ops = {{0, 0, 5, 4, 4, data + 0x400, "DATA.BIN"}};
    CHECK(autoLoad() == kErrFailure);
    g_save = SaveScript{};
    g_save.file_final = -3;
    CHECK(autoLoad() == kErrCbResult);
    g_save = SaveScript{};
    g_save.file_final = 7;
    CHECK(autoLoad() == kErrParam);
    g_save = SaveScript{};
    g_save.file_ops = {{0, 0, 0, 8, 4, data + 0x400, "DATA.BIN"}};  // buffer smaller than the size
    CHECK(autoLoad() == kErrParam);
    g_save = SaveScript{};
    g_save.file_ops = {{0, 0, 0, 1, 1, data + 0x400, "PARAM.SFO"}};  // system files are refused
    CHECK(autoLoad() == kErrParam);

    // Write without truncation, a delete and a missing delete.
    g_save = SaveScript{};
    put(data + 0x300, "EY");
    g_save.file_ops = {{3, 0, 1, 2, 2, data + 0x300, "DATA.BIN"}, {2, 1, 0, 0, 0, 0, "B.DAT"}};
    CHECK(autoSave() == 0);
    CHECK(text(saves / "SAVE0001" / "DATA.BIN") == "hEYlo" && !stdfs::exists(saves / "SAVE0001" / "B.DAT"));
    {
        const auto sfo = tox2::readParamSfoFile(saves / "SAVE0001" / "PARAM.SFO");
        CHECK(sfo && !sfo->ints.contains("*B.DAT") && sfo->ints.at("*DATA.BIN") == 1);
    }
    g_save = SaveScript{};
    g_save.file_ops = {{2, 1, 0, 0, 0, 0, "B.DAT"}};
    CHECK(autoSave() == kErrFailure);
    g_save = SaveScript{};  // a plain WRITE truncates
    g_save.file_ops = {{1, 0, 0, 2, 2, data + 0x300, "DATA.BIN"}};
    CHECK(autoSave() == 0 && text(saves / "SAVE0001" / "DATA.BIN") == "EY");

    // reCreateMode YES needs setParam and drops the old files.
    g_save = SaveScript{};
    g_save.recreate = 2;
    g_save.file_ops = {{1, 1, 0, 3, 3, data + 0x100, "NEW.DAT"}};
    CHECK(autoSave() == kErrParam);
    g_save.set_param = set_param;
    g_save.next_op = 0;
    CHECK(autoSave() == 0);
    CHECK(!stdfs::exists(saves / "SAVE0001" / "DATA.BIN") && text(saves / "SAVE0001" / "NEW.DAT") == "xyz");
    {
        const auto sfo = tox2::readParamSfoFile(saves / "SAVE0001" / "PARAM.SFO");
        CHECK(sfo && !sfo->ints.contains("*DATA.BIN") && sfo->ints.contains("*NEW.DAT"));
    }

    // List operations: ListSave2 creates the game's new-data slot; ListLoad2 picks by focus.
    put(prefix, "SAVE");
    tox2Write32(base, set_list, 0);
    tox2Write32(base, set_list + 4, 1);
    tox2Write32(base, set_list + 8, prefix);
    put(new_dir, "SAVE0002");
    tox2Write32(base, new_data, 0);
    tox2Write32(base, new_data + 4, new_dir);
    g_save = SaveScript{};
    g_save.focus = 5;  // NEWDATA
    g_save.new_data = new_data;
    g_save.set_param = set_param;
    g_save.file_ops = {{1, 1, 0, 3, 3, data + 0x100, "B.DAT"}};
    CHECK(call(ListSave, {0, set_list, set_buf, list_cb, stat_cb, file_cb, 0, 0}) == 0);
    CHECK(g_save.list_calls == 1 && g_save.list_dir_num == 1 && g_save.stat_dir == "SAVE0002" &&
          g_save.stat_is_new == 1);
    CHECK(text(saves / "SAVE0002" / "B.DAT") == "xyz");

    g_save = SaveScript{};
    put(fixed_list, "SAVE0001");
    put(fixed_list + 48, "SAVE0002");
    put(focus_dir, "SAVE0001");
    g_save.focus = 0;  // DIRNAME
    g_save.focus_dir = focus_dir;
    g_save.fixed_num = 2;
    g_save.fixed_list = fixed_list;
    g_save.file_ops = {{0, 1, 0, 3, 3, data + 0x400, "NEW.DAT"}};
    CHECK(call(ListLoad, {0, set_list, set_buf, list_cb, stat_cb, file_cb, 0, 0}) == 0);
    CHECK(g_save.list_dir_num == 2 && g_save.list_dir_list_num == 2 && g_save.stat_dir == "SAVE0001" &&
          g_save.stat_is_new == 0);
    CHECK(std::memcmp(base + data + 0x400, "xyz", 3) == 0);

    g_save.focus = 1;  // LISTHEAD
    g_save.stat_calls = 0;
    CHECK(call(ListLoad, {0, set_list, set_buf, list_cb, stat_cb, file_cb, 0, 0}) == 0);
    CHECK(g_save.stat_calls == 1);

    g_save = SaveScript{};  // nothing matches the prefix and there is no new-data slot: cancel
    put(prefix, "NOPE");
    CHECK(call(ListLoad, {0, set_list, set_buf, list_cb, stat_cb, file_cb, 0, 0}) == 1);
    CHECK(g_save.stat_calls == 0);
    g_save.list_result = 1;  // funcList OK_LAST ends the call
    CHECK(call(ListLoad, {0, set_list, set_buf, list_cb, stat_cb, file_cb, 0, 0}) == 0);
    g_save.list_result = -4;
    CHECK(call(ListLoad, {0, set_list, set_buf, list_cb, stat_cb, file_cb, 0, 0}) == kErrCbResult);
    put(prefix, "");  // an empty prefix is a parameter error
    CHECK(call(ListLoad, {0, set_list, set_buf, list_cb, stat_cb, file_cb, 0, 0}) == kErrParam);
    CHECK(call(ListLoad, {0, 0, set_buf, list_cb, stat_cb, file_cb, 0, 0}) == kErrParam);
    CHECK(call(ListLoad, {0, set_list, set_buf, 0, stat_cb, file_cb, 0, 0}) == kErrParam);

    // Fixed operations (ListAutoLoad, FixedSave2, FixedLoad2): the list goes to funcFixed, which names the
    // save. ListAutoLoad takes its userdata from the caller's stack (the ninth argument).
    const std::uint32_t fixed_name = scratch + 0x7600, other_name = scratch + 0x7700;
    tox2Write64(base, 0x500070, 0x4321);
    auto listAutoLoad = [&](std::uint32_t err_dialog = 0) {
        return call(ListAutoLoad, {0, err_dialog, set_list, set_buf, fixed_cb, stat_cb, file_cb, 0xffffffff});
    };
    auto fixedLoad = [&] { return call(FixedLoad, {0, set_list, set_buf, fixed_cb, stat_cb, file_cb, 0, 0x55}); };
    auto fixedSave = [&] { return call(FixedSave, {0, set_list, set_buf, fixed_cb, stat_cb, file_cb, 0, 0x55}); };
    auto fixedListed = [&] {
        std::string joined;
        for (const std::string& name : g_save.fixed_listed) joined += (joined.empty() ? "" : ",") + name;
        return joined;
    };

    // The game's boot call: no matching save, funcFixed answers ERR_NODATA, errDialog NONE. The result is
    // CBRESULT without a dialog, and neither funcStat nor funcFile run.
    put(prefix, "BOOT_SD");
    tox2Write32(base, set_list, 1);  // SUBTITLE, ASCENT
    g_save = SaveScript{};
    g_save.fixed_result = -4;
    lines.clear();
    CHECK(listAutoLoad(0) == kErrCbResult);
    CHECK(g_save.fixed_calls == 1 && g_save.fixed_dir_num == 0 && g_save.fixed_dir_list_num == 0 &&
          g_save.fixed_userdata == 0x4321 && g_save.stat_calls == 0 && g_save.file_exc.empty());
    CHECK(logged("prefix 'BOOT_SD'") && logged("there is no save data (no dialog requested)") &&
          !logged("error dialog"));
    CHECK(logged("ListAutoLoad returns 0x8002b401"));
    lines.clear();
    CHECK(listAutoLoad(1) == kErrCbResult && logged("error dialog (headless, not shown): there is no save data"));
    g_save.fixed_result = 1;  // OK_LAST ends the call successfully
    CHECK(listAutoLoad() == 0 && g_save.stat_calls == 0);
    g_save.fixed_result = 2;  // OK_LAST_NOCONFIRM is no valid funcFixed result
    CHECK(listAutoLoad() == kErrParam);
    g_save.fixed_result = 9;
    CHECK(listAutoLoad() == kErrParam);

    // Argument checks: no funcFixed, no setList, a bad version or errDialog.
    CHECK(call(ListAutoLoad, {0, 0, set_list, set_buf, 0, stat_cb, file_cb, 0, 0}) == kErrParam);
    CHECK(call(ListAutoLoad, {0, 0, 0, set_buf, fixed_cb, stat_cb, file_cb, 0, 0}) == kErrParam);
    CHECK(call(ListAutoLoad, {2, 0, set_list, set_buf, fixed_cb, stat_cb, file_cb, 0, 0}) == kErrParam);
    CHECK(call(ListAutoLoad, {0, 3, set_list, set_buf, fixed_cb, stat_cb, file_cb, 0, 0}) == kErrParam);
    CHECK(call(FixedLoad, {0, set_list, set_buf, 0, stat_cb, file_cb, 0, 0}) == kErrParam);
    CHECK(call(FixedSave, {0, 0, set_buf, fixed_cb, stat_cb, file_cb, 0, 0}) == kErrParam);

    // FixedSave2 creates a save the game names (the list is empty, so the save is new data).
    put(prefix, "FX_");
    put(fixed_name, "FX_SAVE01");
    put(set_param + 128, "M-sub");
    put(data, "fixed");
    g_save = SaveScript{};
    g_save.fixed_name = fixed_name;
    g_save.set_param = set_param;
    g_save.file_ops = {{1, 1, 0, 5, 5, data, "NEWF.DAT"}};
    lines.clear();
    CHECK(fixedSave() == 0);
    CHECK(g_save.fixed_calls == 1 && g_save.fixed_dir_num == 0 && g_save.fixed_userdata == 0x55 &&
          g_save.stat_calls == 1 && g_save.stat_is_new == 1 && g_save.stat_dir == "FX_SAVE01" &&
          g_save.stat_file_num == 0);
    CHECK(logged("confirmation for 'FX_SAVE01' counted as YES"));
    CHECK(text(saves / "FX_SAVE01" / "NEWF.DAT") == "fixed");
    {
        const auto sfo = tox2::readParamSfoFile(saves / "FX_SAVE01" / "PARAM.SFO");
        CHECK(sfo && sfo->strings.at("TITLE") == "My title" && sfo->strings.at("SAVEDATA_DIRECTORY") == "FX_SAVE01");
    }

    // FixedLoad2 and ListAutoLoad pick the save back: funcStat sees its PARAM.SFO and file list, and a READ
    // returns the bytes. The list reaches funcFixed with the save in it.
    g_save = SaveScript{};
    g_save.fixed_name = fixed_name;
    g_save.fixed_option = 1;  // NOCONFIRM
    std::memset(base + data + 0x400, 0, 16);
    g_save.file_ops = {{0, 1, 0, 5, 16, data + 0x400, "NEWF.DAT"}};
    lines.clear();
    CHECK(fixedLoad() == 0);
    CHECK(fixedListed() == "FX_SAVE01" && g_save.fixed_dir_num == 1 && g_save.fixed_dir_list_num == 1);
    CHECK(g_save.stat_is_new == 0 && g_save.stat_title == "My title" && g_save.stat_dir == "FX_SAVE01" &&
          g_save.stat_file_num == 1 && g_save.stat_file_list_num == 1);
    CHECK(tox2Read32(base, buf) == 1 && tox2Read64(base, buf + 8) == 5);  // the file list: NORMALFILE, 5 bytes
    CHECK(std::strcmp(reinterpret_cast<const char*>(base + buf + 40), "NEWF.DAT") == 0);
    CHECK(std::memcmp(base + data + 0x400, "fixed", 6) == 0);
    CHECK(g_save.file_exc.size() == 2 && g_save.file_exc[1] == 5);
    CHECK(!logged("counted as YES"));  // NOCONFIRM asks nothing
    CHECK(text(saves / "FX_SAVE01" / "NEWF.DAT") == "fixed");

    g_save.next_op = 0;
    g_save.file_exc.clear();
    std::memset(base + data + 0x400, 0, 16);
    CHECK(listAutoLoad(2) == 0);
    CHECK(g_save.fixed_userdata == 0x4321 && g_save.stat_is_new == 0 && g_save.stat_file_num == 1);
    CHECK(std::memcmp(base + data + 0x400, "fixed", 6) == 0);

    // A name that is not saved is new data: without setParam the call is a parameter error, and OK_LAST in
    // funcStat ends it without creating anything.
    put(other_name, "FX_NONE001");
    g_save = SaveScript{};
    g_save.fixed_name = other_name;
    CHECK(fixedLoad() == kErrParam && g_save.stat_is_new == 1);
    g_save.stat_result = 1;
    CHECK(fixedLoad() == 0 && !stdfs::exists(saves / "FX_NONE001"));
    g_save.stat_result = 0;
    g_save.stat_calls = 0;

    // Errors in funcFixed's answer: no name, a bad name, an empty name, an unknown option.
    g_save = SaveScript{};
    CHECK(fixedLoad() == kErrParam && g_save.stat_calls == 0);  // fixedSet->dirName is null
    put(other_name, "fx_lower");
    g_save.fixed_name = other_name;
    CHECK(fixedLoad() == kErrParam && g_save.stat_calls == 0);
    put(other_name, "");
    CHECK(fixedLoad() == kErrParam && g_save.stat_calls == 0);
    g_save.fixed_name = fixed_name;
    g_save.fixed_option = 5;
    CHECK(fixedLoad() == kErrParam && g_save.stat_calls == 0);
    g_save.fixed_option = 0;
    CHECK(call(FixedLoad, {0, set_list, set_buf, fixed_cb, 0, file_cb, 0, 0}) == kErrParam);  // no funcStat

    // The directory list: prefix filtering, several prefixes and sorting by subtitle.
    auto saveWith = [&](const char* name, const char* subtitle) {
        put(other_name, name);
        put(set_param + 128, subtitle);
        g_save = SaveScript{};
        g_save.fixed_name = other_name;
        g_save.set_param = set_param;
        g_save.file_ops = {{1, 1, 0, 5, 5, data, "NEWF.DAT"}};
        return fixedSave();
    };
    CHECK(saveWith("FX_SAVE02", "B-sub") == 0);
    CHECK(saveWith("FX_SAVE03", "A-sub") == 0);
    CHECK(saveWith("OTHER_01", "Z-sub") == 0);
    g_save = SaveScript{};
    g_save.fixed_result = 1;  // only the list matters
    CHECK(fixedLoad() == 0);
    CHECK(g_save.fixed_dir_num == 3 && fixedListed() == "FX_SAVE03,FX_SAVE02,FX_SAVE01");  // ASCENT
    tox2Write32(base, set_list + 4, 0);                                                    // DESCENT
    CHECK(fixedLoad() == 0 && fixedListed() == "FX_SAVE01,FX_SAVE02,FX_SAVE03");
    tox2Write32(base, set_list + 4, 1);
    put(prefix, "FX_SAVE01|OTHER");
    CHECK(fixedLoad() == 0 && g_save.fixed_dir_num == 2 && fixedListed() == "FX_SAVE01,OTHER_01");
    put(prefix, "FX_");
    tox2Write32(base, set_buf, 2);  // dirListMax 2: all three match, two are listed (the first two by name)
    CHECK(fixedLoad() == 0 && g_save.fixed_dir_num == 3 && g_save.fixed_dir_list_num == 2 &&
          fixedListed() == "FX_SAVE02,FX_SAVE01");
    tox2Write32(base, set_buf, 4);
    put(prefix, "NOTHERE");
    CHECK(fixedLoad() == 0 && g_save.fixed_dir_num == 0 && g_save.fixed_listed.empty());
    put(prefix, "");  // an empty prefix is a parameter error
    CHECK(fixedLoad() == kErrParam);
    CHECK(ctx->gpr[1] == 0x500000);

    // The save list dialog. Three saves match "FX_" (sorted by subtitle: FX_SAVE03 "A-sub", FX_SAVE02 "B-sub",
    // FX_SAVE01 "M-sub"); the driver is scripted.
    {
        using tox2::DialogKind;
        using tox2::SaveListKind;
        using tox2::SaveMessageResult;
        constexpr std::uint32_t kErrInternal = 0x8002b403;
        const std::uint32_t icon_struct = scratch + 0x7800, icon_title = scratch + 0x7840,
                            icon_bytes = scratch + 0x7900, missing_dir = scratch + 0x7a00;
        {
            std::ofstream icon(saves / "FX_SAVE02" / "ICON0.PNG", std::ios::binary);
            icon << "PNGBYTES";
        }
        put(prefix, "FX_");
        tox2Write32(base, set_list, 1);
        tox2Write32(base, set_list + 4, 1);
        put(fixed_list, "FX_SAVE01");
        put(fixed_list + 48, "FX_SAVE02");
        put(fixed_list + 96, "FX_SAVE03");
        put(new_dir, "FX_NEW001");
        put(missing_dir, "FX_MISSING");
        put(icon_title, "Fresh start");
        put(icon_bytes, "NEWICON");
        tox2Write32(base, icon_struct, icon_title);
        tox2Write32(base, icon_struct + 4, 7);
        tox2Write32(base, icon_struct + 8, icon_bytes);
        auto script = std::make_unique<ScriptedSaveDialog>();
        ScriptedSaveDialog& dlg = *script;
        tox2::setSaveDialogDriver(std::move(script));
        // `new_pos` 2 means no newData structure. OK_LAST from funcStat ends a call after the choice without
        // writing anything.
        auto setup = [&](std::uint32_t focus, std::uint32_t new_pos, std::uint32_t icon) {
            g_save = SaveScript{};
            g_save.focus = focus;
            g_save.focus_dir = focus_dir;
            g_save.fixed_num = 3;
            g_save.fixed_list = fixed_list;
            g_save.new_data = new_pos == 2 ? 0 : new_data;
            g_save.stat_result = 1;
            tox2Write32(base, new_data, new_pos);
            tox2Write32(base, new_data + 8, icon);
            dlg.reset();
        };
        auto run = [&](Import which) { return call(which, {0, set_list, set_buf, list_cb, stat_cb, file_cb, 0, 0}); };

        // TAIL, NEWDATA focus: the new-data row comes last and the list opens on it.
        setup(5, 1, 0);
        dlg.list_answers = {3};
        dlg.message_answers = {SaveMessageResult::Yes};
        CHECK(run(ListSave) == 0);
        CHECK(dlg.lists.size() == 1 && dlg.lists[0].kind == SaveListKind::Save && dlg.lists[0].initial_row == 3);
        CHECK((dlg.lists[0].titles ==
               std::vector<std::string>{"A-sub", "B-sub", "M-sub", std::string(tox2::kNewDataTitle)}));
        CHECK((dlg.lists[0].has_icon == std::vector<bool>{false, true, false, false}));
        CHECK((dlg.lists[0].is_new == std::vector<bool>{false, false, false, true}));
        CHECK(dlg.lists[0].infos[3] == tox2::kNewDataLine && dlg.lists[0].infos[0].find("KB") != std::string::npos);
        CHECK(dlg.messages.size() == 1 && dlg.messages[0] == tox2::kConfirmSaveNew &&
              dlg.message_kinds[0] == DialogKind::YesNo);
        CHECK(g_save.list_calls == 1 && g_save.stat_dir == "FX_NEW001" && g_save.stat_is_new == 1);

        // HEAD: the new-data row comes first; entry focus 0 opens on row 1, NEWDATA on row 0.
        setup(1, 0, 0);  // LISTHEAD
        CHECK(run(ListSave) == 1);
        CHECK(dlg.lists.size() == 1 && dlg.lists[0].initial_row == 1 && dlg.lists[0].titles.size() == 4 &&
              dlg.lists[0].titles[0] == tox2::kNewDataTitle && dlg.lists[0].titles[1] == "A-sub");
        CHECK(g_save.stat_calls == 0 && dlg.messages.empty());
        setup(5, 0, 0);
        CHECK(run(ListSave) == 1 && dlg.lists[0].initial_row == 0);
        setup(2, 0, 0);  // LISTTAIL
        CHECK(run(ListSave) == 1 && dlg.lists[0].initial_row == 3);

        // The game's newData title and icon buffer; a null title keeps the default.
        setup(5, 0, icon_struct);
        CHECK(run(ListSave) == 1 && dlg.lists[0].titles[0] == "Fresh start" && dlg.lists[0].has_icon[0]);
        tox2Write32(base, icon_struct, 0);
        setup(5, 0, icon_struct);
        CHECK(run(ListSave) == 1 && dlg.lists[0].titles[0] == tox2::kNewDataTitle && dlg.lists[0].has_icon[0]);
        tox2Write32(base, icon_struct + 4, 225281);  // beyond the icon buffer limit: no icon
        setup(5, 0, icon_struct);
        CHECK(run(ListSave) == 1 && !dlg.lists[0].has_icon[0]);
        tox2Write32(base, icon_struct, icon_title);
        tox2Write32(base, icon_struct + 4, 7);

        // List -> row 2, No, list again on row 2 -> row 1, Yes: row 1's save is used and funcList ran once.
        setup(1, 2, 0);
        dlg.list_answers = {2, 1};
        dlg.message_answers = {SaveMessageResult::No, SaveMessageResult::Yes};
        CHECK(run(ListLoad) == 0);
        CHECK(dlg.lists.size() == 2 && dlg.lists[0].kind == SaveListKind::Load && dlg.lists[0].initial_row == 0 &&
              dlg.lists[1].initial_row == 2);
        CHECK(dlg.messages.size() == 2 && dlg.messages[0].starts_with(tox2::kConfirmLoad) &&
              dlg.messages[0].find("M-sub") != std::string::npos && dlg.messages[1].find("B-sub") != std::string::npos);
        CHECK(g_save.list_calls == 1 && g_save.stat_dir == "FX_SAVE02" && g_save.stat_is_new == 0);

        // ListSave2 asks the overwrite question for an entry.
        setup(1, 2, 0);
        dlg.list_answers = {0};
        dlg.message_answers = {SaveMessageResult::Yes};
        CHECK(run(ListSave) == 0 && dlg.messages[0].starts_with(tox2::kConfirmOverwrite) &&
              g_save.stat_dir == "FX_SAVE03");

        // A confirmation that cannot be shown is INTERNAL; back from the list is CELL_CANCEL.
        setup(1, 2, 0);
        dlg.list_answers = {1};
        dlg.message_answers = {SaveMessageResult::Failed};
        CHECK(run(ListSave) == kErrInternal && g_save.stat_calls == 0);
        setup(1, 2, 0);
        CHECK(run(ListSave) == 1 && g_save.stat_calls == 0);

        // ListLoad2 shows the "create new data" question on its new-data row.
        setup(5, 0, 0);
        dlg.list_answers = {0};
        dlg.message_answers = {SaveMessageResult::Yes};
        CHECK(run(ListLoad) == 0 && dlg.messages.size() == 1 && dlg.messages[0] == tox2::kConfirmLoadNew &&
              g_save.stat_dir == "FX_NEW001");

        // DIRNAME focus on a name that is not listed opens on row 0 (no newest-entry choice).
        setup(0, 2, 0);
        g_save.focus_dir = missing_dir;
        CHECK(run(ListLoad) == 1 && dlg.lists.size() == 1 && dlg.lists[0].initial_row == 0 &&
              dlg.lists[0].titles.size() == 3 && g_save.stat_calls == 0);

        // FixedSave2 / FixedLoad2 with option NONE: the confirmation depends on whether the name is listed.
        // OK_LAST from funcStat ends a call after the confirmation without writing anything.
        auto fixedSetup = [&](std::uint32_t name) {
            g_save = SaveScript{};
            g_save.fixed_name = name;
            g_save.stat_result = 1;
            dlg.reset();
        };
        put(fixed_name, "FX_SAVE01");
        fixedSetup(fixed_name);
        dlg.message_answers = {SaveMessageResult::No};
        lines.clear();
        CHECK(fixedSave() == 1);
        CHECK(dlg.lists.empty() && dlg.messages.size() == 1 && dlg.message_kinds[0] == DialogKind::YesNo);
        CHECK(dlg.messages[0].starts_with(tox2::kConfirmOverwrite) &&
              dlg.messages[0].find("My title") != std::string::npos &&
              dlg.messages[0].find("M-sub") != std::string::npos && dlg.messages[0].find("KB") != std::string::npos);
        CHECK(g_save.fixed_calls == 1 && g_save.stat_calls == 0);  // No: funcFixed is not asked again
        CHECK(logged("confirmation for 'FX_SAVE01' counted as NO"));
        fixedSetup(fixed_name);
        dlg.message_answers = {SaveMessageResult::Yes};
        CHECK(fixedLoad() == 0 && dlg.messages.size() == 1 && dlg.messages[0].starts_with(tox2::kConfirmLoad) &&
              g_save.stat_calls == 1 && g_save.stat_dir == "FX_SAVE01" && g_save.stat_is_new == 0);

        put(other_name, "FX_NONE001");
        fixedSetup(other_name);
        dlg.message_answers = {SaveMessageResult::Yes};
        CHECK(fixedLoad() == 0 && dlg.messages.size() == 1 && dlg.messages[0] == tox2::kConfirmLoadNew &&
              g_save.stat_calls == 1 && g_save.stat_dir == "FX_NONE001" && g_save.stat_is_new == 1);
        fixedSetup(other_name);
        dlg.message_answers = {SaveMessageResult::Yes};
        CHECK(fixedSave() == 0 && dlg.messages.size() == 1 && dlg.messages[0] == tox2::kConfirmSaveNew);
        fixedSetup(other_name);
        dlg.message_answers = {SaveMessageResult::No};
        CHECK(fixedLoad() == 1 && g_save.stat_calls == 0);

        fixedSetup(fixed_name);
        dlg.message_answers = {SaveMessageResult::Failed};
        CHECK(fixedSave() == kErrInternal && g_save.stat_calls == 0);

        // NOCONFIRM and ListAutoLoad ask nothing.
        fixedSetup(fixed_name);
        g_save.fixed_option = 1;
        CHECK(fixedLoad() == 0 && dlg.messages.empty() && g_save.stat_calls == 1);
        fixedSetup(fixed_name);
        CHECK(listAutoLoad(0) == 0 && dlg.messages.empty() && g_save.stat_calls == 1);

        // Error dialogs: one OK message per error with the Japanese text; the result is ignored.
        auto errorSetup = [&](std::int32_t stat_result) {
            g_save = SaveScript{};
            g_save.stat_result = stat_result;
            dlg.reset();
            lines.clear();
        };
        put(dir_addr, "SAVE0001");
        tox2::resetCellSaveData();
        errorSetup(-4);
        dlg.message_answers = {SaveMessageResult::Failed};
        CHECK(autoLoad(1) == kErrCbResult);
        CHECK(dlg.messages.size() == 1 && dlg.messages[0] == tox2::kErrorNoData &&
              dlg.message_kinds[0] == DialogKind::Ok);
        CHECK(logged("error dialog (headless, not shown): there is no save data"));
        dlg.is_interactive = true;
        errorSetup(-4);
        CHECK(autoLoad(1) == kErrCbResult && dlg.messages.size() == 1 &&
              logged("error dialog: there is no save data") && !logged("headless"));
        dlg.is_interactive = false;
        errorSetup(-2);
        CHECK(autoLoad(1) == kErrCbResult && dlg.messages.size() == 1 && dlg.messages[0] == tox2::kErrorFailure);
        errorSetup(-3);
        CHECK(autoLoad(1) == kErrCbResult && dlg.messages.size() == 1 && dlg.messages[0] == tox2::kErrorBroken);
        errorSetup(-4);
        CHECK(autoLoad(0) == kErrCbResult && dlg.messages.empty() && logged("no dialog requested"));
        errorSetup(9);  // a result without a dialog is a parameter error
        CHECK(autoLoad(1) == kErrParam && dlg.messages.empty());
        errorSetup(-5);  // the game's message pointer is null: the dialog opens with an empty text
        CHECK(autoLoad(1) == kErrCbResult && dlg.messages.size() == 1 && dlg.messages[0].empty());
        errorSetup(-1);
        CHECK(autoLoad(1) == kErrCbResult && dlg.messages.size() == 1 &&
              dlg.messages[0].starts_with(tox2::kErrorNoSpace) && dlg.messages[0].find("KB") != std::string::npos);

        // ListSave2 and the Fixed functions always ask for the error dialog.
        errorSetup(0);
        g_save.list_result = -4;
        put(prefix, "FX_");
        CHECK(run(ListSave) == kErrCbResult && dlg.messages.size() == 1 && dlg.messages[0] == tox2::kErrorNoData);
        errorSetup(0);
        g_save.fixed_result = -3;
        CHECK(fixedLoad() == kErrCbResult && dlg.messages.size() == 1 && dlg.messages[0] == tox2::kErrorBroken);

        // NOREPEAT shows the same error once.
        tox2::resetCellSaveData();
        errorSetup(-4);
        CHECK(autoLoad(2) == kErrCbResult && autoLoad(2) == kErrCbResult && dlg.messages.size() == 1);
        errorSetup(-3);  // a different error shows again
        CHECK(autoLoad(2) == kErrCbResult && dlg.messages.size() == 1 && dlg.messages[0] == tox2::kErrorBroken);

        tox2::resetSaveDialog();
        tox2Write32(base, new_data + 8, 0);
    }

    // Delete2 has only a selection UI in RPCS3.
    CHECK(call(Delete, {0}) == 1);
    CHECK(ctx->gpr[1] == 0x500000);

    stdfs::remove_all(root, ec);
    tox2::resetCellSaveData();
    tox2::resetHostFunctions();
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

// Guest allocation callbacks of the cellPngDec test: a bump allocator over a guest block.
struct PngTestHeap {
    std::uint32_t next = 0;
    std::uint32_t end = 0;
    std::vector<std::uint32_t> freed;
    std::uint32_t malloc_calls = 0;
};
PngTestHeap g_png_heap;

TOX2_PPU_FUNC(pngTestMalloc) {
    (void)base;
    const auto size = static_cast<std::uint32_t>(ctx->gpr[3]);
    g_png_heap.malloc_calls++;
    const std::uint32_t addr = g_png_heap.next;
    if (addr + size > g_png_heap.end) {
        ctx->gpr[3] = 0;
        return;
    }
    g_png_heap.next = (addr + size + 15) & ~15u;
    ctx->gpr[3] = addr;
}

TOX2_PPU_FUNC(pngTestFree) {
    (void)base;
    g_png_heap.freed.push_back(static_cast<std::uint32_t>(ctx->gpr[3]));
    ctx->gpr[3] = 0;
}

// cbCtrlStrm: hands out the rest of a source in pieces of `piece` bytes through a staging buffer.
struct PngTestFeed {
    std::vector<std::uint8_t> rest;
    std::size_t pos = 0;
    std::size_t piece = 16;
    std::uint32_t staging = 0;
    std::uint32_t calls = 0;
    std::uint32_t last_decoded = 0;
    std::uint32_t arg_seen = 0;
};
PngTestFeed g_png_feed;

TOX2_PPU_FUNC(pngTestStream) {
    const auto info = static_cast<std::uint32_t>(ctx->gpr[3]);
    const auto param = static_cast<std::uint32_t>(ctx->gpr[4]);
    g_png_feed.arg_seen = static_cast<std::uint32_t>(ctx->gpr[5]);
    g_png_feed.last_decoded = tox2Read32(base, info);
    g_png_feed.calls++;
    const std::size_t count = std::min(g_png_feed.piece, g_png_feed.rest.size() - g_png_feed.pos);
    std::memcpy(base + g_png_feed.staging, g_png_feed.rest.data() + g_png_feed.pos, count);
    g_png_feed.pos += count;
    tox2Write32(base, param, g_png_feed.staging);
    tox2Write32(base, param + 4, static_cast<std::uint32_t>(count));
    ctx->gpr[3] = 0;
}

struct PngTestImage {
    unsigned width = 0;
    unsigned height = 0;
    std::vector<std::uint8_t> file;
};

struct PngPaletteEntry {
    std::uint8_t r, g, b, a;
};

// Input of the libpng writer. Like the decoder, the writer keeps its setjmp in a function with
// plain locals only; the caller owns every container.
struct PngWriteJob {
    unsigned width = 0;
    unsigned height = 0;
    int color_type = 0;
    int depth = 8;
    unsigned interlace = 0;
    png_bytep* rows = nullptr;
    const png_color* palette = nullptr;
    const std::uint8_t* trans = nullptr;
    int palette_size = 0;
    std::vector<std::uint8_t>* out = nullptr;
};

void pngWriteCallback(png_structp png, png_bytep data, size_t count) {
    auto* job = static_cast<PngWriteJob*>(png_get_io_ptr(png));
    job->out->insert(job->out->end(), data, data + count);
}

void pngFlushCallback(png_structp) {}

void pngWriteErrorCallback(png_structp png, png_const_charp) { png_longjmp(png, 1); }

bool writePng(PngWriteJob* job) {
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, job, pngWriteErrorCallback, nullptr);
    if (png == nullptr) return false;
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_write_struct(&png, nullptr);
        return false;
    }
    if (setjmp(png_jmpbuf(png)) != 0) {
        png_destroy_write_struct(&png, &info);
        return false;
    }
    png_set_write_fn(png, job, pngWriteCallback, pngFlushCallback);
    png_set_IHDR(png, info, job->width, job->height, job->depth, job->color_type,
                 job->interlace != 0 ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    if (job->palette != nullptr) {
        png_set_PLTE(png, info, job->palette, job->palette_size);
        if (job->trans != nullptr) png_set_tRNS(png, info, job->trans, job->palette_size, nullptr);
    }
    png_write_info(png, info);
    if (job->depth < 8) png_set_packing(png);  // the rows carry one index per byte
    png_set_interlace_handling(png);
    png_write_image(png, job->rows);
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    return true;
}

// Encodes `raw` into a PNG of the given type and depth. Rows are tightly packed, 16-bit samples
// are big-endian, and a depth below 8 takes one index (or gray level) per byte.
PngTestImage encodePng(unsigned width, unsigned height, const std::vector<std::uint8_t>& raw, int color_type,
                       int png_depth, unsigned interlace = 0, const std::vector<PngPaletteEntry>& palette = {}) {
    const std::size_t channels = color_type == PNG_COLOR_TYPE_RGB_ALPHA    ? 4
                                 : color_type == PNG_COLOR_TYPE_RGB        ? 3
                                 : color_type == PNG_COLOR_TYPE_GRAY_ALPHA ? 2
                                                                           : 1;
    const std::size_t row_bytes = std::size_t{width} * channels * (png_depth == 16 ? 2 : 1);
    CHECK(raw.size() == row_bytes * height);
    std::vector<png_bytep> rows(height);
    std::vector<std::uint8_t> pixels = raw;  // libpng wants non-const rows
    for (unsigned row = 0; row < height; row++) rows[row] = pixels.data() + row * row_bytes;
    std::vector<png_color> colors;
    std::vector<std::uint8_t> alphas;
    bool translucent = false;
    for (const PngPaletteEntry& entry : palette) {
        colors.push_back({entry.r, entry.g, entry.b});
        alphas.push_back(entry.a);
        translucent = translucent || entry.a != 255;
    }
    PngTestImage image{width, height, {}};
    PngWriteJob job;
    job.width = width;
    job.height = height;
    job.color_type = color_type;
    job.depth = png_depth;
    job.interlace = interlace;
    job.rows = rows.data();
    job.palette = colors.empty() ? nullptr : colors.data();
    job.trans = translucent ? alphas.data() : nullptr;
    job.palette_size = static_cast<int>(colors.size());
    job.out = &image.file;
    CHECK(writePng(&job));
    return image;
}

// Inserts a chunk so that it starts at byte `at` (33 is the first position after IHDR; the last
// 12 bytes of a file are IEND).
void insertChunk(std::vector<std::uint8_t>& png, std::size_t at, const char* type,
                 const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> chunk;
    const auto length = static_cast<std::uint32_t>(payload.size());
    for (int shift = 24; shift >= 0; shift -= 8) chunk.push_back(static_cast<std::uint8_t>(length >> shift));
    chunk.insert(chunk.end(), type, type + 4);
    chunk.insert(chunk.end(), payload.begin(), payload.end());
    const auto crc = static_cast<std::uint32_t>(crc32(0, chunk.data() + 4, static_cast<uInt>(chunk.size() - 4)));
    for (int shift = 24; shift >= 0; shift -= 8) chunk.push_back(static_cast<std::uint8_t>(crc >> shift));
    png.insert(png.begin() + static_cast<std::ptrdiff_t>(at), chunk.begin(), chunk.end());
}

void testCellPngDec() {
    namespace stdfs = std::filesystem;
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetCensus();
    tox2::resetLogOnce();
    tox2::resetHostFunctions();
    tox2::resetCellPngDec();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x100000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t heap = scratch, params = scratch + 0x10000, src_buf = scratch + 0x20000,
                        out_buf = scratch + 0x40000, staging = scratch + 0x80000;
    g_png_heap = {heap, heap + 0x10000, {}, 0};
    g_png_feed = {};
    g_png_feed.staging = staging;

    const stdfs::path root =
            stdfs::temp_directory_path() / ("tox2_pngdec_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    stdfs::remove_all(root, ec);
    stdfs::create_directories(root / "game", ec);
    stdfs::create_directories(root / "dump", ec);
    tox2::configureVfs(root / "game", std::nullopt);
    tox2::setPngDumpDirectory(root / "dump");

    enum Import : std::uint32_t {
        Create,
        ExtOpen,
        ExtReadHeader,
        ExtSetParameter,
        ExtDecodeData,
        Close,
        Destroy,
        ImportCount
    };
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x157d30c5, "cellPngDec", "cellPngDecCreate"},
            {0x00800010, 0x0c515302, "cellPngDec", "cellPngDecExtOpen"},
            {0x00800020, 0x8b33f863, "cellPngDec", "cellPngDecExtReadHeader"},
            {0x00800030, 0x9e9d7d42, "cellPngDec", "cellPngDecExtSetParameter"},
            {0x00800040, 0x726fc1d0, "cellPngDec", "cellPngDecExtDecodeData"},
            {0x00800050, 0x5b3d1ff1, "cellPngDec", "cellPngDecClose"},
            {0x00800060, 0x820dae1a, "cellPngDec", "cellPngDecDestroy"},
    };
    tox2::installImports(imports, ImportCount);
    auto ctx = std::make_unique<Tox2PpuContext>();
    ctx->gpr[1] = 0x500000;
    CHECK(p.memory.commit(0x4f0000, 0x10000));
    auto call = [&](Import which, std::initializer_list<std::uint64_t> args) {
        int reg = 3;
        for (const std::uint64_t arg : args) ctx->gpr[reg++] = arg;
        tox2HleCall(ctx.get(), base, which);
        return static_cast<std::uint32_t>(ctx->gpr[3]);
    };

    constexpr std::uint32_t kOk = 0, kErrHeader = 0x80611201, kErrFormat = 0x80611202, kErrArg = 0x80611203,
                            kErrSeq = 0x80611204, kErrOpenFile = 0x80611207;
    constexpr std::uint32_t kSpaceRgb = 2, kSpacePal = 4, kSpaceGa = 9, kSpaceRgba = 10, kSpaceArgb = 20;

    // Guest scratch addresses inside the params block.
    const std::uint32_t handle_pp = params, thread_in = params + 0x10, thread_out = params + 0x40,
                        stream_pp = params + 0x50, src = params + 0x60, open_info = params + 0x90,
                        cb_strm = params + 0xa0, info = params + 0xb0, ext_info = params + 0xd0,
                        in_param = params + 0xe0, out_param = params + 0x110, ext_in = params + 0x140,
                        ext_out = params + 0x150, ctrl_param = params + 0x160, out_info = params + 0x170,
                        file_name = params + 0x200;
    const std::uint32_t malloc_opd = tox2::registerHostFunction("pngTestMalloc", pngTestMalloc);
    const std::uint32_t free_opd = tox2::registerHostFunction("pngTestFree", pngTestFree);
    const std::uint32_t stream_opd = tox2::registerHostFunction("pngTestStream", pngTestStream);

    // Create: the handle block comes from the guest allocator; the codec version is reported.
    tox2Write32(base, thread_in + 12, malloc_opd);
    tox2Write32(base, thread_in + 16, 0x1111);
    tox2Write32(base, thread_in + 20, free_opd);
    tox2Write32(base, thread_in + 24, 0x2222);
    CHECK(call(Create, {0, thread_in, thread_out}) == kErrArg);
    CHECK(call(Create, {handle_pp, thread_in, thread_out}) == kOk);
    const std::uint32_t handle = tox2Read32(base, handle_pp);
    CHECK(handle >= heap && handle < heap + 0x10000);
    CHECK(tox2Read32(base, thread_out) == 0x00420000);
    CHECK(g_png_heap.malloc_calls == 1);
    tox2Write32(base, thread_in + 12, 0);
    CHECK(call(Create, {handle_pp, thread_in, thread_out}) == kErrArg);
    tox2Write32(base, thread_in + 12, malloc_opd);

    auto putSource = [&](const std::vector<std::uint8_t>& bytes) {
        std::memcpy(base + src_buf, bytes.data(), bytes.size());
        tox2Write32(base, src, 1);  // CELL_PNGDEC_BUFFER
        tox2Write32(base, src + 20, src_buf);
        tox2Write32(base, src + 24, static_cast<std::uint32_t>(bytes.size()));
    };
    auto setParam = [&](std::uint32_t mode, std::uint32_t space, std::uint32_t depth, std::uint32_t alpha_select = 0,
                        std::uint32_t alpha = 0) {
        tox2Write32(base, in_param, 0);
        tox2Write32(base, in_param + 4, mode);
        tox2Write32(base, in_param + 8, space);
        tox2Write32(base, in_param + 12, depth);
        tox2Write32(base, in_param + 16, 1);
        tox2Write32(base, in_param + 20, alpha_select);
        tox2Write32(base, in_param + 24, alpha);
    };
    auto bytesAt = [&](std::uint32_t addr, std::size_t count) {
        return std::vector<std::uint8_t>(base + addr, base + addr + count);
    };

    // Opens `bytes` as a buffer source and reads its header; returns the stream, 0 on failure.
    auto openAndRead = [&](const std::vector<std::uint8_t>& bytes) -> std::uint32_t {
        putSource(bytes);
        tox2Write32(base, stream_pp, 0);
        if (call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) != kOk) return 0;
        const std::uint32_t stream = tox2Read32(base, stream_pp);
        if (call(ExtReadHeader, {handle, stream, info, 0}) != kOk) return 0;
        return stream;
    };
    struct Decoded {
        std::uint32_t result;
        std::vector<std::uint8_t> out;
    };
    // DecodeData into a 0xEE-filled buffer.
    auto decode = [&](std::uint32_t stream, std::uint64_t bytes_per_line, std::size_t out_size) {
        std::memset(base + out_buf, 0xEE, out_size);
        tox2Write64(base, ctrl_param, bytes_per_line);
        Decoded decoded{call(ExtDecodeData, {handle, stream, out_buf, ctrl_param, out_info, 0, 0}), {}};
        decoded.out = bytesAt(out_buf, out_size);
        return decoded;
    };
    // SetParameter (extIn null), then DecodeData.
    auto setAndDecode = [&](std::uint32_t stream, std::uint32_t mode, std::uint32_t space, std::uint32_t depth,
                            std::uint32_t alpha_select, std::uint32_t alpha, std::uint64_t bytes_per_line,
                            std::size_t out_size) {
        setParam(mode, space, depth, alpha_select, alpha);
        CHECK(call(ExtSetParameter, {handle, stream, in_param, out_param, 0, 0}) == kOk);
        return decode(stream, bytes_per_line, out_size);
    };
    auto closeStream = [&](std::uint32_t stream) { CHECK(call(Close, {handle, stream}) == kOk); };

    // 3x2 RGBA8, every sample distinct.
    const std::vector<std::uint8_t> rgba = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
                                            13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};
    const PngTestImage rgba_png = encodePng(3, 2, rgba, PNG_COLOR_TYPE_RGB_ALPHA, 8);

    // Header values; SetParameter and DecodeData before ReadHeader are sequence errors.
    {
        putSource(rgba_png.file);
        CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) == kOk);
        const std::uint32_t stream = tox2Read32(base, stream_pp);
        CHECK(tox2Read32(base, open_info) != 0);
        CHECK(stream != 0 && stream != handle);
        setParam(0, kSpaceRgba, 8);
        CHECK(call(ExtSetParameter, {handle, stream, in_param, out_param, 0, 0}) == kErrSeq);
        tox2Write64(base, ctrl_param, 12);
        CHECK(call(ExtDecodeData, {handle, stream, out_buf, ctrl_param, out_info, 0, 0}) == kErrSeq);
        CHECK(call(ExtReadHeader, {handle, stream, info, ext_info}) == kOk);
        CHECK(tox2Read32(base, info) == 3 && tox2Read32(base, info + 4) == 2);
        CHECK(tox2Read32(base, info + 8) == 4 && tox2Read32(base, info + 12) == kSpaceRgba);
        CHECK(tox2Read32(base, info + 16) == 8 && tox2Read32(base, info + 20) == 0);
        CHECK(tox2Read32(base, info + 24) == 0);
        CHECK(tox2Read64(base, ext_info) == 0);
        CHECK(call(ExtReadHeader, {0, stream, info, 0}) == kErrArg);
        CHECK(call(ExtReadHeader, {handle, stream + 1, info, 0}) == kErrArg);
        CHECK(call(ExtReadHeader, {handle, stream, 0, 0}) == kErrArg);
        closeStream(stream);
        CHECK(call(ExtReadHeader, {handle, stream, info, 0}) == kErrArg);  // closed
    }

    // RGBA8 -> RGBA8 with 5 padding bytes per row; the pad is not written.
    {
        const std::uint32_t stream = openAndRead(rgba_png.file);
        CHECK(stream != 0);
        setParam(0, kSpaceRgba, 8);
        CHECK(call(ExtSetParameter, {handle, stream, in_param, out_param, 0, 0}) == kOk);
        CHECK(tox2Read64(base, out_param) == 12);
        CHECK(tox2Read32(base, out_param + 8) == 3 && tox2Read32(base, out_param + 12) == 2);
        CHECK(tox2Read32(base, out_param + 16) == 4 && tox2Read32(base, out_param + 20) == 8);
        CHECK(tox2Read32(base, out_param + 24) == 0 && tox2Read32(base, out_param + 28) == kSpaceRgba);
        CHECK(tox2Read32(base, out_param + 32) == 0);
        const Decoded decoded = decode(stream, 17, 29);
        CHECK(decoded.result == kOk);
        std::vector<std::uint8_t> expected(29, 0xEE);
        std::copy(rgba.begin(), rgba.begin() + 12, expected.begin());
        std::copy(rgba.begin() + 12, rgba.end(), expected.begin() + 17);
        CHECK(decoded.out == expected);
        CHECK(tox2Read32(base, out_info) == (1u << 15) && tox2Read32(base, out_info + 4) == 0);
        CHECK(tox2Read32(base, out_info + 8) == 0 && tox2Read32(base, out_info + 12) == 0);
        // A row pitch below outputWidthByte is refused and leaves the status at STOP.
        const Decoded refused = decode(stream, 11, 29);
        CHECK(refused.result == kErrArg);
        CHECK(tox2Read32(base, out_info + 12) == 1);
        closeStream(stream);
    }

    // RGBA8 -> ARGB8, and the same bottom to top.
    {
        const std::uint32_t stream = openAndRead(rgba_png.file);
        const Decoded top = setAndDecode(stream, 0, kSpaceArgb, 8, 0, 0, 12, 24);
        CHECK(top.result == kOk);
        const std::vector<std::uint8_t> argb = {4,  1,  2,  3,  8,  5,  6,  7,  12, 9,  10, 11,
                                                16, 13, 14, 15, 20, 17, 18, 19, 24, 21, 22, 23};
        CHECK(top.out == argb);
        CHECK(tox2Read32(base, out_param + 28) == kSpaceArgb);
        const Decoded flipped = setAndDecode(stream, 1, kSpaceArgb, 8, 0, 0, 12, 24);
        CHECK(flipped.result == kOk);
        std::vector<std::uint8_t> argb_flipped(argb.begin() + 12, argb.end());
        argb_flipped.insert(argb_flipped.end(), argb.begin(), argb.begin() + 12);
        CHECK(flipped.out == argb_flipped);
        CHECK(tox2Read32(base, out_param + 24) == 1);
        closeStream(stream);
    }

    // RGBA8 -> RGB8 drops the alpha; colour to gray-alpha is refused.
    {
        const std::uint32_t stream = openAndRead(rgba_png.file);
        const Decoded rgb = setAndDecode(stream, 0, kSpaceRgb, 8, 0, 0, 9, 18);
        CHECK(rgb.result == kOk);
        const std::vector<std::uint8_t> expected = {1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15, 17, 18, 19, 21, 22, 23};
        CHECK(rgb.out == expected);
        CHECK(tox2Read32(base, out_param + 16) == 3);
        setParam(0, kSpaceGa, 8);
        CHECK(call(ExtSetParameter, {handle, stream, in_param, out_param, 0, 0}) == kErrArg);
        closeStream(stream);
    }

    // Palette input: 4x2, 8-bit indices, a palette with one translucent entry.
    {
        const std::vector<PngPaletteEntry> pal = {{255, 0, 0, 255}, {0, 255, 0, 128}, {0, 0, 255, 255}};
        const std::vector<std::uint8_t> indices = {0, 1, 2, 1, 2, 2, 0, 0};
        const PngTestImage image = encodePng(4, 2, indices, PNG_COLOR_TYPE_PALETTE, 8, 0, pal);
        const std::uint32_t stream = openAndRead(image.file);
        CHECK(stream != 0);
        CHECK(tox2Read32(base, info + 8) == 1 && tox2Read32(base, info + 12) == kSpacePal);
        CHECK(tox2Read32(base, info + 16) == 8);
        CHECK((tox2Read32(base, info + 24) & (1u << 3)) != 0);  // PLTE
        const Decoded decoded = setAndDecode(stream, 0, kSpaceRgba, 8, 0, 0, 16, 32);
        CHECK(decoded.result == kOk);
        const std::uint8_t red[] = {255, 0, 0, 255}, green[] = {0, 255, 0, 128}, blue[] = {0, 0, 255, 255};
        std::vector<std::uint8_t> expected;
        for (const std::uint8_t index : indices) {
            const std::uint8_t* color = index == 0 ? red : index == 1 ? green : blue;
            expected.insert(expected.end(), color, color + 4);
        }
        CHECK(decoded.out == expected);
        // Fixed alpha applies to a palette input, whatever its palette alpha.
        const Decoded fixed = setAndDecode(stream, 0, kSpaceArgb, 8, 1, 0x40, 16, 32);
        CHECK(fixed.result == kOk);
        for (std::size_t pixel = 0; pixel < 8; pixel++) {
            const std::uint8_t* color = indices[pixel] == 0 ? red : indices[pixel] == 1 ? green : blue;
            CHECK(fixed.out[pixel * 4] == 0x40 && fixed.out[pixel * 4 + 1] == color[0] &&
                  fixed.out[pixel * 4 + 3] == color[2]);
        }
        // Palette output at the input's own depth hands the indices through.
        const Decoded raw = setAndDecode(stream, 0, kSpacePal, 8, 0, 0, 4, 8);
        CHECK(raw.result == kOk);
        CHECK(raw.out == indices);
        CHECK(tox2Read64(base, out_param) == 4);
        closeStream(stream);
    }

    // 2-bit palette, width 5 (the file pads its rows to bytes), expanded to RGBA.
    {
        std::vector<PngPaletteEntry> pal;
        for (const std::uint8_t gray : {10, 90, 170, 250}) pal.push_back({gray, gray, gray, 255});
        const std::vector<std::uint8_t> indices = {0, 1, 2, 3, 0, 3, 2, 1, 0, 3};
        const PngTestImage image = encodePng(5, 2, indices, PNG_COLOR_TYPE_PALETTE, 2, 0, pal);
        const std::uint32_t stream = openAndRead(image.file);
        CHECK(stream != 0);
        CHECK(tox2Read32(base, info + 16) == 2);
        const Decoded decoded = setAndDecode(stream, 0, kSpaceRgba, 8, 0, 0, 20, 40);
        CHECK(decoded.result == kOk);
        const std::uint8_t levels[] = {10, 90, 170, 250};
        bool match = decoded.result == kOk;
        for (std::size_t pixel = 0; pixel < indices.size() && match; pixel++) {
            match = decoded.out[pixel * 4] == levels[indices[pixel]] && decoded.out[pixel * 4 + 3] == 255;
        }
        CHECK(match);
        closeStream(stream);
    }

    // RGB8: stream alpha is opaque, fixed alpha is the requested value, RGB output is the data.
    {
        const std::vector<std::uint8_t> rgb = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
        const PngTestImage image = encodePng(2, 2, rgb, PNG_COLOR_TYPE_RGB, 8);
        const std::uint32_t stream = openAndRead(image.file);
        CHECK(tox2Read32(base, info + 8) == 3 && tox2Read32(base, info + 12) == kSpaceRgb);
        const Decoded opaque = setAndDecode(stream, 0, kSpaceRgba, 8, 0, 0, 8, 16);
        CHECK(opaque.result == kOk);
        CHECK(opaque.out ==
              (std::vector<std::uint8_t>{10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 100, 110, 120, 255}));
        const Decoded fixed = setAndDecode(stream, 0, kSpaceRgba, 8, 1, 0x7f, 8, 16);
        CHECK(fixed.out[3] == 0x7f && fixed.out[7] == 0x7f && fixed.out[15] == 0x7f && fixed.out[0] == 10);
        const Decoded same = setAndDecode(stream, 0, kSpaceRgb, 8, 0, 0, 6, 12);
        CHECK(same.out == rgb);
        closeStream(stream);
    }

    // 16-bit RGBA: stripped to the high byte for 8-bit output, kept big-endian for 16-bit output.
    {
        const std::vector<std::uint8_t> rgba16 = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
                                                  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
        const PngTestImage image = encodePng(2, 1, rgba16, PNG_COLOR_TYPE_RGB_ALPHA, 16);
        const std::uint32_t stream = openAndRead(image.file);
        CHECK(tox2Read32(base, info + 16) == 16);
        const Decoded eight = setAndDecode(stream, 0, kSpaceRgba, 8, 0, 0, 8, 8);
        CHECK(eight.result == kOk);
        CHECK(eight.out == (std::vector<std::uint8_t>{0x12, 0x56, 0x9a, 0xde, 0x01, 0x03, 0x05, 0x07}));
        const Decoded sixteen = setAndDecode(stream, 0, kSpaceRgba, 16, 0, 0, 16, 16);
        CHECK(sixteen.result == kOk && sixteen.out == rgba16);
        CHECK(tox2Read64(base, out_param) == 16 && tox2Read32(base, out_param + 20) == 16);
        const Decoded argb16 = setAndDecode(stream, 0, kSpaceArgb, 16, 0, 0, 16, 16);
        CHECK(argb16.out[0] == 0xde && argb16.out[1] == 0xf0 && argb16.out[2] == 0x12 && argb16.out[3] == 0x34 &&
              argb16.out[6] == 0x9a && argb16.out[7] == 0xbc);
        closeStream(stream);
    }

    // Adam7: the guest gets the same image as from a non-interlaced file.
    {
        std::vector<std::uint8_t> grid;
        for (unsigned i = 0; i < 5 * 5 * 4; i++) grid.push_back(static_cast<std::uint8_t>(i * 7 + 3));
        const PngTestImage image = encodePng(5, 5, grid, PNG_COLOR_TYPE_RGB_ALPHA, 8, 1);
        const std::uint32_t stream = openAndRead(image.file);
        CHECK(stream != 0 && tox2Read32(base, info + 20) == 1);
        const Decoded decoded = setAndDecode(stream, 0, kSpaceRgba, 8, 0, 0, 20, 100);
        CHECK(decoded.result == kOk && decoded.out == grid);
        closeStream(stream);
    }

    // Chunk bits: ReadHeader sees what precedes IDAT, DecodeData everything plus IDAT.
    {
        PngTestImage image = rgba_png;
        insertChunk(image.file, image.file.size() - 12, "tIME", {0x07, 0xe8, 1, 1, 0, 0, 0});
        insertChunk(image.file, 33, "prVt", {1, 2, 3});
        insertChunk(image.file, 33, "tEXt", {'k', 0, 'v'});
        insertChunk(image.file, 33, "gAMA", {0, 1, 0x86, 0xa0});
        const std::uint32_t stream = openAndRead(image.file);
        CHECK(stream != 0);
        CHECK(tox2Read32(base, info + 24) == 1u);  // gAMA only
        const Decoded decoded = setAndDecode(stream, 0, kSpaceRgba, 8, 0, 0, 12, 24);
        CHECK(decoded.result == kOk);
        CHECK(tox2Read32(base, out_info) == ((1u << 0) | (1u << 9) | (1u << 15)));
        CHECK(tox2Read32(base, out_info + 4) == 1 && tox2Read32(base, out_info + 8) == 1);
        closeStream(stream);
    }

    // Header errors and argument checks of Open.
    {
        putSource(std::vector<std::uint8_t>(64, 0x42));
        CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) == kErrHeader);
        putSource(std::vector<std::uint8_t>(4, 0x89));
        CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) == kErrHeader);
        putSource(rgba_png.file);
        CHECK(call(ExtOpen, {0, stream_pp, src, open_info, 0, 0}) == kErrArg);
        CHECK(call(ExtOpen, {handle, 0, src, open_info, 0, 0}) == kErrArg);
        tox2Write32(base, src, 7);
        CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) == kErrArg);
        // A valid signature followed by junk opens, but there is no IHDR to report.
        std::vector<std::uint8_t> junk = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
        junk.insert(junk.end(), 56, 0x42);
        putSource(junk);
        CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) == kOk);
        const std::uint32_t junk_stream = tox2Read32(base, stream_pp);
        CHECK(call(ExtReadHeader, {handle, junk_stream, info, 0}) == kErrHeader);
        closeStream(junk_stream);
        // A truncated image opens (the signature is fine) and fails when decoded.
        std::vector<std::uint8_t> broken = rgba_png.file;
        broken.resize(40);
        putSource(broken);
        CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) == kOk);
        const std::uint32_t stream = tox2Read32(base, stream_pp);
        CHECK(call(ExtReadHeader, {handle, stream, info, 0}) == kOk);
        const Decoded decoded = setAndDecode(stream, 0, kSpaceRgba, 8, 0, 0, 12, 24);
        CHECK(decoded.result == kErrFormat);
        CHECK(tox2Read32(base, out_info + 12) == 1);
        closeStream(stream);
    }

    // ExtSetParameter's extra parameters: LINE_MODE only, and the extra output.
    {
        const std::uint32_t stream = openAndRead(rgba_png.file);
        setParam(0, kSpaceRgba, 8);
        tox2Write32(base, ext_in, 0);
        tox2Write32(base, ext_in + 4, 1);
        tox2Write32(base, ext_in + 8, 0);
        CHECK(call(ExtSetParameter, {handle, stream, in_param, out_param, ext_in, ext_out}) == kErrArg);
        tox2Write32(base, ext_in, 1);
        CHECK(call(ExtSetParameter, {handle, stream, in_param, out_param, ext_in, ext_out}) == kOk);
        CHECK(tox2Read64(base, ext_out) == 12 && tox2Read32(base, ext_out + 8) == 1);
        tox2Write32(base, ext_in + 4, 0);
        CHECK(call(ExtSetParameter, {handle, stream, in_param, out_param, ext_in, ext_out}) == kOk);
        CHECK(tox2Read32(base, ext_out + 8) == 2);
        tox2Write32(base, ext_in + 4, 9);
        CHECK(call(ExtSetParameter, {handle, stream, in_param, out_param, ext_in, ext_out}) == kOk);
        CHECK(tox2Read32(base, ext_out + 8) == 2);
        // Partial output through cbCtrlDisp is not implemented: refused, not silently ignored.
        tox2Write32(base, ext_in + 4, 1);
        CHECK(call(ExtSetParameter, {handle, stream, in_param, out_param, ext_in, ext_out}) == kOk);
        tox2Write64(base, ctrl_param, 12);
        CHECK(call(ExtDecodeData, {handle, stream, out_buf, ctrl_param, out_info, cb_strm, 0}) == kErrArg);
        // Invalid output parameters.
        setParam(2, kSpaceRgba, 8);
        CHECK(call(ExtSetParameter, {handle, stream, in_param, out_param, 0, 0}) == kErrArg);
        setParam(0, 3, 8);
        CHECK(call(ExtSetParameter, {handle, stream, in_param, out_param, 0, 0}) == kErrArg);
        closeStream(stream);
    }

    // File source through the VFS, with an offset into the file.
    {
        const std::string name = "/dev_bdvd/test.png";
        std::vector<std::uint8_t> padded = {1, 2, 3, 4, 5};
        padded.insert(padded.end(), rgba_png.file.begin(), rgba_png.file.end());
        {
            std::ofstream out(root / "game" / "test.png", std::ios::binary);
            out.write(reinterpret_cast<const char*>(padded.data()), static_cast<std::streamsize>(padded.size()));
        }
        std::memcpy(base + file_name, name.c_str(), name.size() + 1);
        tox2Write32(base, src, 0);  // CELL_PNGDEC_FILE
        tox2Write32(base, src + 4, file_name);
        tox2Write64(base, src + 8, 5);
        tox2Write32(base, src + 16, 0);
        CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) == kOk);
        const std::uint32_t stream = tox2Read32(base, stream_pp);
        CHECK(call(ExtReadHeader, {handle, stream, info, 0}) == kOk);
        const Decoded decoded = setAndDecode(stream, 0, kSpaceRgba, 8, 0, 0, 12, 24);
        CHECK(decoded.result == kOk && decoded.out == rgba);
        closeStream(stream);

        // The size limits what is read: a file size short of the image cannot decode.
        tox2Write32(base, src + 16, 40);
        CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) == kOk);
        const std::uint32_t cut = tox2Read32(base, stream_pp);
        CHECK(call(ExtReadHeader, {handle, cut, info, 0}) == kOk);
        CHECK(setAndDecode(cut, 0, kSpaceRgba, 8, 0, 0, 12, 24).result == kErrFormat);
        closeStream(cut);

        tox2Write64(base, src + 8, 0);
        tox2Write32(base, src + 16, 0);
        CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) == kErrHeader);  // junk first
        const std::string missing = "/dev_bdvd/missing.png";
        std::memcpy(base + file_name, missing.c_str(), missing.size() + 1);
        CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) == kErrOpenFile);
        // Partial decoding is for buffers only.
        tox2Write32(base, cb_strm, stream_opd);
        tox2Write32(base, cb_strm + 4, 0x3333);
        CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, cb_strm, 0}) == kErrFormat);
    }

    // Streamed source: the first buffer holds only the start, the callback supplies the rest.
    {
        const std::size_t head = 40;
        const std::vector<std::uint8_t> first(rgba_png.file.begin(), rgba_png.file.begin() + head);
        g_png_feed = {};
        g_png_feed.staging = staging;
        g_png_feed.rest.assign(rgba_png.file.begin() + head, rgba_png.file.end());
        putSource(first);
        tox2Write32(base, cb_strm, stream_opd);
        tox2Write32(base, cb_strm + 4, 0x3333);
        CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, cb_strm, 0}) == kOk);
        const std::uint32_t stream = tox2Read32(base, stream_pp);
        CHECK(call(ExtReadHeader, {handle, stream, info, 0}) == kOk);
        CHECK(g_png_feed.calls == 0);  // the header was already there
        const std::size_t frees_before = g_png_heap.freed.size();
        const Decoded decoded = setAndDecode(stream, 0, kSpaceRgba, 8, 0, 0, 12, 24);
        CHECK(decoded.result == kOk && decoded.out == rgba);
        CHECK(g_png_feed.calls >= 2 && g_png_feed.arg_seen == 0x3333);
        CHECK(g_png_feed.pos == g_png_feed.rest.size());
        CHECK(g_png_feed.last_decoded > head);
        CHECK(g_png_heap.freed.size() == frees_before + 1);  // the callback's scratch block
        closeStream(stream);

        // A callback that runs dry before the image ends fails the decode.
        g_png_feed = {};
        g_png_feed.staging = staging;
        g_png_feed.rest.assign(rgba_png.file.begin() + head, rgba_png.file.begin() + head + 20);
        putSource(first);
        CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, cb_strm, 0}) == kOk);
        const std::uint32_t dry = tox2Read32(base, stream_pp);
        CHECK(call(ExtReadHeader, {handle, dry, info, 0}) == kOk);
        CHECK(setAndDecode(dry, 0, kSpaceRgba, 8, 0, 0, 12, 24).result == kErrFormat);
        closeStream(dry);
    }

    // The dump holds the first decoded image.
    {
        const stdfs::path dumped = root / "dump" / "png_0.png";
        CHECK(stdfs::exists(dumped));
        png_image image{};
        image.version = PNG_IMAGE_VERSION;
        CHECK(png_image_begin_read_from_file(&image, dumped.string().c_str()) != 0);
        image.format = PNG_FORMAT_RGBA;
        std::vector<std::uint8_t> pixels(PNG_IMAGE_SIZE(image));
        CHECK(png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr) != 0);
        CHECK(image.width == 3 && image.height == 2 && pixels.size() == 24 &&
              std::memcmp(pixels.data(), rgba.data(), 24) == 0);
        png_image_free(&image);
    }

    // Destroy frees the handle block; the handle is then unknown.
    const std::size_t frees_before_destroy = g_png_heap.freed.size();
    CHECK(call(Destroy, {handle}) == kOk);
    CHECK(g_png_heap.freed.size() == frees_before_destroy + 1 && g_png_heap.freed.back() == handle);
    CHECK(call(Destroy, {handle}) == kErrArg);
    CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) == kErrArg);
    CHECK(ctx->gpr[1] == 0x500000);

    stdfs::remove_all(root, ec);
    p.main.free(scratch);
    tox2::resetCellPngDec();
    tox2::resetHostFunctions();
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

void testFifoParser() {
    using tox2::FifoParser;
    using tox2::FifoStatus;
    constexpr std::uint32_t kBase = 0x1000;

    // Increment method: registers advance by 4 per argument. 0x4feac and 0x80064 are the headers
    // the cellGcmSys flip functions write.
    {
        FakeSink sink;
        FifoParser parser;
        sink.put(kBase, {0x1d6c | (3u << 18), 10, 11, 12, 0x4feac, 2, 0x80064, 0x50, 0x77});
        std::uint32_t get = kBase;
        CHECK(parser.run(get, kBase + 36, 100, sink) == FifoStatus::Idle);
        CHECK(get == kBase + 36);
        CHECK((sink.writes ==
               Writes{{0x1d6c, 10}, {0x1d70, 11}, {0x1d74, 12}, {0xfeac, 2}, {0x64, 0x50}, {0x68, 0x77}}));
        CHECK(parser.headersParsed() == 3);
        CHECK(parser.run(get, kBase + 36, 100, sink) == FifoStatus::Idle && sink.writes.size() == 6);
    }

    // Non-increment method: every argument goes to the same register. The low bits of put are ignored.
    {
        FakeSink sink;
        FifoParser parser;
        sink.put(kBase, {0x40000000u | 0x1800 | (3u << 18), 1, 2, 3, 0x1800 | (1u << 18), 9});
        std::uint32_t get = kBase;
        CHECK(parser.run(get, kBase + 24 + 3, 100, sink) == FifoStatus::Idle && get == kBase + 24);
        CHECK((sink.writes == Writes{{0x1800, 1}, {0x1800, 2}, {0x1800, 3}, {0x1800, 9}}));
    }

    // A sink that asks to stop ends the run right after that write, inside the command; the next run
    // continues with the following argument.
    {
        FakeSink sink;
        FifoParser parser;
        sink.stop_on = {0x1808, 0};
        sink.put(kBase, {0x40000000u | 0x1808 | (3u << 18), 5, 0, 7});
        std::uint32_t get = kBase;
        CHECK(parser.run(get, kBase + 16, 100, sink) == FifoStatus::Budget);
        CHECK(get == kBase + 12 && parser.commandGet(get) == kBase);
        CHECK((sink.writes == Writes{{0x1808, 5}, {0x1808, 0}}));
        CHECK(parser.run(get, kBase + 16, 100, sink) == FifoStatus::Idle && get == kBase + 16);
        CHECK((sink.writes == Writes{{0x1808, 5}, {0x1808, 0}, {0x1808, 7}}));
    }

    // The method register wraps at 16 bits.
    {
        FakeSink sink;
        FifoParser parser;
        sink.put(kBase, {0xfffcu | (2u << 18), 5, 6});
        std::uint32_t get = kBase;
        CHECK(parser.run(get, kBase + 12, 100, sink) == FifoStatus::Idle);
        CHECK((sink.writes == Writes{{0xfffc, 5}, {0x0000, 6}}));
    }

    // NOPs: an all-zero word and a zero-count header of either kind write nothing.
    {
        FakeSink sink;
        FifoParser parser;
        sink.put(kBase, {0, 0x40000000u | 0x100, 0x100, 0x1800 | (1u << 18), 4});
        std::uint32_t get = kBase;
        CHECK(parser.run(get, kBase + 20, 100, sink) == FifoStatus::Idle);
        CHECK((sink.writes == Writes{{0x1800, 4}}) && parser.headersParsed() == 1);
    }

    // Old and new jumps move get; put is only compared against get.
    {
        FakeSink sink;
        FifoParser parser;
        sink.put(kBase, {0x20000000u | 0x2000});
        sink.put(0x2000, {0x00003000u | 1});
        sink.put(0x3000, {0x1800 | (1u << 18), 7});
        std::uint32_t get = kBase;
        CHECK(parser.run(get, 0x3008, 100, sink) == FifoStatus::Idle && get == 0x3008);
        CHECK((sink.writes == Writes{{0x1800, 7}}));
    }

    // A jump target is only validated when its word is read.
    {
        FakeSink sink;
        FifoParser parser;
        sink.put(kBase, {0x20000000u | 0x1ffffffc});
        std::uint32_t get = kBase;
        CHECK(parser.run(get, 0x40, 100, sink) == FifoStatus::Fault);  // 0x1ffffffc is unmapped
        CHECK(get == 0x1ffffffc);
    }

    // Call and return: the return lands after the call word; the same call word can be reused.
    {
        FakeSink sink;
        FifoParser parser;
        sink.put(kBase, {0x2000u | 2, 0x1800 | (1u << 18), 100, 0x2000u | 2});
        sink.put(0x2000, {0x1800 | (1u << 18), 200, 0x00020000});
        std::uint32_t get = kBase;
        CHECK(parser.run(get, kBase + 16, 100, sink) == FifoStatus::Idle && get == kBase + 16);
        CHECK((sink.writes == Writes{{0x1800, 200}, {0x1800, 100}, {0x1800, 200}}));
    }

    // A call inside a subroutine and a return without a call are faults that leave get on the word.
    {
        FakeSink sink;
        FifoParser parser;
        sink.put(kBase, {0x2000u | 2});
        sink.put(0x2000, {0x3000u | 2});
        std::uint32_t get = kBase;
        CHECK(parser.run(get, 0x4000, 100, sink) == FifoStatus::Fault);
        CHECK(get == 0x2000 && parser.lastFault().find("inside a subroutine") != std::string_view::npos);
        CHECK(parser.run(get, 0x4000, 100, sink) == FifoStatus::Fault && get == 0x2000);  // sticky
        parser.reset();
        CHECK(parser.lastFault().empty());
        get = kBase;
        CHECK(parser.run(get, 0x4000, 100, sink) == FifoStatus::Fault && get == 0x2000);  // the nested call again

        FakeSink sink2;
        FifoParser parser2;
        sink2.put(kBase, {0x00020000});
        get = kBase;
        CHECK(parser2.run(get, kBase + 4, 100, sink2) == FifoStatus::Fault && get == kBase);
        CHECK(parser2.lastFault().find("without a call") != std::string_view::npos);
    }

    // Headers that are neither a method nor flow control: no write, get stays on the header.
    for (const std::uint32_t bad :
         {0x80000000u, 0xa0001800u | (1u << 18), 0x00010000u, 0x00030000u, 0x00060000u, 0x60000000u | 0x1800}) {
        FakeSink sink;
        FifoParser parser;
        sink.put(kBase, {0x1800 | (1u << 18), 1, bad});
        std::uint32_t get = kBase;
        CHECK(parser.run(get, kBase + 12, 100, sink) == FifoStatus::Fault);
        CHECK(get == kBase + 8 && sink.writes.size() == 1);
        CHECK(parser.lastFault().find("invalid header") != std::string_view::npos);
    }

    // Unaligned get.
    {
        FakeSink sink;
        FifoParser parser;
        std::uint32_t get = kBase + 2;
        CHECK(parser.run(get, kBase + 16, 100, sink) == FifoStatus::Fault && get == kBase + 2);
    }

    // A refused write stops with Waiting and the next run retries the same write before reading on.
    {
        FakeSink sink;
        FifoParser parser;
        sink.put(kBase, {0x68 | (2u << 18), 0x11, 0x22, 0x1800 | (1u << 18), 3});
        sink.refuse = 2;
        std::uint32_t get = kBase;
        CHECK(parser.run(get, kBase + 20, 100, sink) == FifoStatus::Waiting);
        CHECK(get == kBase + 8 && sink.writes.empty());  // the argument is consumed, the write is kept
        CHECK(parser.run(get, kBase + 20, 100, sink) == FifoStatus::Waiting && sink.attempts == 2);
        CHECK(parser.run(get, kBase + 20, 100, sink) == FifoStatus::Idle && get == kBase + 20);
        CHECK((sink.writes == Writes{{0x68, 0x11}, {0x6c, 0x22}, {0x1800, 3}}));
        CHECK(sink.attempts == 5);
    }

    // Budget counts words read; the method register keeps advancing across runs.
    {
        FakeSink sink;
        FifoParser parser;
        sink.put(kBase, {0x1d6c | (5u << 18), 1, 2, 3, 4, 5, 0x1800 | (1u << 18), 6});
        std::uint32_t get = kBase;
        CHECK(parser.run(get, kBase + 32, 3, sink) == FifoStatus::Budget && get == kBase + 12);
        CHECK((sink.writes == Writes{{0x1d6c, 1}, {0x1d70, 2}}));
        CHECK(parser.run(get, kBase + 32, 100, sink) == FifoStatus::Idle && get == kBase + 32);
        CHECK((sink.writes == Writes{{0x1d6c, 1}, {0x1d70, 2}, {0x1d74, 3}, {0x1d78, 4}, {0x1d7c, 5}, {0x1800, 6}}));
        CHECK(parser.run(get, kBase + 32, 0, sink) == FifoStatus::Idle);  // nothing left: Idle wins over Budget
        sink.put(kBase + 32, {0x1800 | (1u << 18), 8});
        CHECK(parser.run(get, kBase + 40, 0, sink) == FifoStatus::Budget && get == kBase + 32);
    }

    // put in the middle of a method: Idle with the arguments still owed, then it resumes.
    {
        FakeSink sink;
        FifoParser parser;
        sink.put(kBase, {0x1d6c | (3u << 18), 1, 2, 3});
        std::uint32_t get = kBase;
        CHECK(parser.run(get, kBase + 8, 100, sink) == FifoStatus::Idle && get == kBase + 8);
        CHECK((sink.writes == Writes{{0x1d6c, 1}}));
        CHECK(parser.run(get, kBase + 16, 100, sink) == FifoStatus::Idle && get == kBase + 16);
        CHECK((sink.writes == Writes{{0x1d6c, 1}, {0x1d70, 2}, {0x1d74, 3}}));
    }

    // Unmapped reads: get stays on the unreadable word, the fault is sticky.
    {
        FakeSink sink;
        FifoParser parser;
        std::uint32_t get = 0x5000;
        CHECK(parser.run(get, 0x5010, 100, sink) == FifoStatus::Fault && get == 0x5000);
        CHECK(parser.lastFault().find("unmapped") != std::string_view::npos);

        FakeSink sink2;
        FifoParser parser2;
        sink2.put(kBase, {0x1800 | (2u << 18), 1});  // the second argument is unmapped
        get = kBase;
        CHECK(parser2.run(get, kBase + 12, 100, sink2) == FifoStatus::Fault && get == kBase + 8);
        CHECK((sink2.writes == Writes{{0x1800, 1}}));
    }

    // Jumping to itself parks the puller as Idle without moving get; rewriting the word resumes.
    {
        FakeSink sink;
        FifoParser parser;
        sink.put(kBase, {0, 0x20000000u | (kBase + 4)});
        std::uint32_t get = kBase;
        CHECK(parser.run(get, 0x9000, 100, sink) == FifoStatus::Idle && get == kBase + 4);
        CHECK(parser.run(get, 0x9000, 100, sink) == FifoStatus::Idle && get == kBase + 4);
        sink.put(kBase + 4, {0x1800 | (1u << 18), 5, 0x00000001u | 0x4000});
        sink.put(0x4000, {0x1800 | (1u << 18), 6});
        CHECK(parser.run(get, 0x4008, 100, sink) == FifoStatus::Idle && get == 0x4008);
        CHECK((sink.writes == Writes{{0x1800, 5}, {0x1800, 6}}));
    }

    // Reading through spans gives the same writes, get, status and fault as word reads, also when spans end
    // mid-command, when jumps and calls leave the span, and with budget and stop points inside it.
    for (const std::size_t span_limit : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{1000}}) {
        const auto fill = [](FakeSink& sink) {
            sink.put(kBase,
                     {0x1d6c | (5u << 18), 1, 2, 3, 4, 5, 0x1800 | (1u << 18), 6, 0x3000u | 2, 0x20000000u | 0x2000});
            sink.put(0x2000, {0x40000000u | 0x1808 | (3u << 18), 7, 0, 9, 0x1800 | (1u << 18), 8});
            sink.put(0x3000, {0x1800 | (2u << 18), 10, 11, 0x00020000});
        };
        for (const std::uint32_t budget : {2u, 3u, 7u, 100u}) {
            FakeSink words_sink, span_sink;
            fill(words_sink);
            fill(span_sink);
            span_sink.use_spans = true;
            span_sink.span_limit = span_limit;
            words_sink.stop_on = span_sink.stop_on = {0x1808, 0};
            FifoParser words_parser, span_parser;
            std::uint32_t words_get = kBase, span_get = kBase;
            for (int run = 0; run < 40; run++) {
                const FifoStatus a = words_parser.run(words_get, 0x2018, budget, words_sink);
                const FifoStatus b = span_parser.run(span_get, 0x2018, budget, span_sink);
                CHECK(a == b && words_get == span_get);
                CHECK(words_parser.commandGet(words_get) == span_parser.commandGet(span_get));
                if (a == FifoStatus::Idle || a == FifoStatus::Fault) break;
            }
            CHECK(words_sink.writes == span_sink.writes && !span_sink.writes.empty());
            CHECK(words_get == span_get && words_parser.headersParsed() == span_parser.headersParsed());
            CHECK(span_sink.read_io_calls == 0 && words_sink.read_io_calls > 0);
        }
    }

    // An unmapped word is a fault with spans too, and a span that stops before the unmapped word is not trusted
    // beyond its end.
    {
        FakeSink sink;
        sink.use_spans = true;
        FifoParser parser;
        sink.put(kBase, {0x1800 | (3u << 18), 1, 2});  // the third argument is unmapped
        std::uint32_t get = kBase;
        CHECK(parser.run(get, kBase + 20, 100, sink) == FifoStatus::Fault && get == kBase + 12);
        CHECK((sink.writes == Writes{{0x1800, 1}, {0x1804, 2}}));
        CHECK(parser.lastFault().find("unmapped") != std::string_view::npos);
        CHECK(sink.read_io_calls == 1);  // only the unmapped word falls back to readIo
    }
}

std::vector<std::pair<tox2::RsxInterrupt, std::uint32_t>> g_interrupts;
void recordInterrupt(tox2::RsxInterrupt kind, std::uint32_t arg) { g_interrupts.emplace_back(kind, arg); }

tox2::RsxCensus fakeRsxCensus() {
    return {120, 3, {{0x1d6c, 5, "NV4097_SET_SEMAPHORE_OFFSET"}, {0x1808, 9, ""}, {0x1800, 5, ""}}};
}

tox2::RsxCensus fakeRsxCensusWithHandlerCalls() {
    tox2::RsxCensus census = fakeRsxCensus();
    census.vblank_handler_runs = 600;
    census.user_handler_runs = 2;
    return census;
}

// Words of a fragment program as stored in guest memory: the 16-bit halves of each word swapped.
std::uint32_t fpStoredWord(std::uint32_t decoded) { return std::rotl(decoded, 16); }

void testRsxArgCensus() {
    CHECK(tox2::rsxMethodName(0x1A04 + 2 * 0x20) == "NV4097_SET_TEXTURE_FORMAT[2]");
    CHECK(tox2::rsxMethodName(0x0208) == "NV4097_SET_SURFACE_FORMAT");
    CHECK(tox2::rsxMethodName(0x0004) == "0x0004");
    CHECK(tox2::rsxDecodeArgument(0x0208, 8 | (2u << 5) | (2u << 8) | (10u << 16) | (9u << 24)) ==
          "color=A8R8G8B8 depth=Z24S8 type=swizzle aa=center_1 size=1024x512");
    CHECK(tox2::rsxDecodeArgument(0x1A04 + 0x20, 2 | (2u << 4) | (0xA5u << 8) | (1u << 16)) ==
          "location=main cubemap=0 border=0 dim=2D format=A8R8G8B8_LN_NR mipmaps=1");
    CHECK(tox2::rsxTextureFormatName(0x86 | 0x40) == "DXT1_SZ_UN");
    CHECK(tox2::rsxDecodeArgument(0x1814, 5 | (3u << 24)) == "first=5 count=4");
    CHECK(tox2::rsxDecodeArgument(0x1808, 5) == "TRIANGLES");
    CHECK(tox2::rsxDecodeArgument(0x08E4, 0x1000 | 2) == "location=main offset=0x1000");
    CHECK(tox2::rsxDecodeArgument(0x1740, 2 | (3u << 4) | (12u << 8)) == "type=f size=3 stride=12 frequency=0");
    CHECK(tox2::rsxDecodeArgument(0x1680, 0x80000040) == "location=main offset=0x40");
    CHECK(tox2::rsxDecodeArgument(0x0324, (1u << 16) | 1u) == "R-B-");
    CHECK(tox2::rsxFpOpcodeName(0x42) == "IFE" && tox2::rsxFpOpcodeName(0x30) == "OP_0x30");
    CHECK(tox2::rsxVpVecOpcodeName(7) == "DP4" && tox2::rsxVpScaOpcodeName(2) == "RCP");

    // Fragment program: MOV, then MUL with an inline constant and the end flag.
    const std::vector<std::uint32_t> fp = {
            fpStoredWord(1u << 24),
            0,
            0,
            0,  // MOV
            fpStoredWord((2u << 24) | 1u),
            0,
            fpStoredWord(2u),
            0,  // MUL, end, src1 is a constant
            0x3F800000,
            0x3F800000,
            0,
            0,  // the constant
    };
    const auto fpReader = [&](std::uint32_t index, std::uint32_t& word) {
        if (index >= fp.size()) return false;
        word = fp[index];
        return true;
    };
    const tox2::RsxFpScan scan = tox2::scanFragmentProgram(fpReader);
    CHECK(scan.terminated && scan.instructions == 2 && scan.constant_words == 1 && scan.words.size() == 12);
    CHECK((scan.opcodes == std::map<std::uint32_t, std::uint32_t>{{1, 1}, {2, 1}}));
    // The opcode's high bit lives in the second source word.
    const std::vector<std::uint32_t> hi = {fpStoredWord((2u << 24) | 1u), 0, fpStoredWord(0x80000000u), 0};
    const tox2::RsxFpScan hi_scan = tox2::scanFragmentProgram([&](std::uint32_t index, std::uint32_t& word) {
        if (index >= hi.size()) return false;
        word = hi[index];
        return true;
    });
    CHECK(hi_scan.terminated && hi_scan.opcodes.count(0x42) == 1);
    // A program that runs off readable memory is not reported as terminated.
    const tox2::RsxFpScan cut = tox2::scanFragmentProgram([&](std::uint32_t index, std::uint32_t& word) {
        if (index >= 8) return false;
        word = fp[index];
        return true;
    });
    CHECK(!cut.terminated && cut.instructions == 2);

    std::vector<std::uint32_t> ucode(tox2::kRsxVpInstructionCount * 4, 0);
    ucode[3 * 4 + 1] = (1u << 22) | (2u << 27);  // MOV / RCP
    ucode[4 * 4 + 3] = 1;                        // end
    const tox2::RsxVpScan vp = tox2::scanVertexProgram(ucode.data(), 3);
    CHECK(vp.terminated && vp.instructions == 2);
    CHECK((vp.vec_opcodes == std::map<std::uint32_t, std::uint32_t>{{0, 1}, {1, 1}}));
    CHECK((vp.sca_opcodes == std::map<std::uint32_t, std::uint32_t>{{0, 1}, {2, 1}}));
    CHECK(!tox2::scanVertexProgram(std::vector<std::uint32_t>(tox2::kRsxVpInstructionCount * 4, 0).data(), 540)
                   .terminated);

    // Flow analysis: the program extends past its first end bit when a branch leads there, and the
    // instruction no path reaches stays out.
    constexpr std::uint32_t kBrb = 0x11, kCal = 0x0a, kRet = 0x0c, kBri = 0x09;
    const auto setFlow = [](std::vector<std::uint32_t>& store, std::uint32_t index, std::uint32_t sca_opcode,
                            std::uint32_t target, bool end) {
        std::uint32_t* instruction = &store[index * 4];
        instruction[0] = (7u << 10) | (((target >> 9) & 1u) << 23);
        instruction[1] = sca_opcode << 27;
        instruction[2] = (target >> 3) & 0x3F;
        instruction[3] = (end ? 1u : 0u) | ((target & 7u) << 29);
    };
    std::vector<std::uint32_t> flow_store(tox2::kRsxVpInstructionCount * 4, 0);
    setFlow(flow_store, 2, kBrb, 6, false);
    setFlow(flow_store, 3, 0, 0, true);
    setFlow(flow_store, 4, kBrb, 900, false);  // stale, never reached
    setFlow(flow_store, 6, 0, 0, true);
    tox2::RsxVpFlow flow = tox2::analyzeVertexProgramFlow(flow_store.data(), tox2::kRsxVpInstructionCount, 2);
    CHECK(flow.terminated && !flow.bad_target && flow.has_flow_control && flow.last == 6);
    CHECK(flow.reached[2] == 1 && flow.reached[3] == 1 && flow.reached[4] == 0 && flow.reached[5] == 0 &&
          flow.reached[6] == 1);
    CHECK(tox2::rsxVpBranchTarget(&flow_store[2 * 4]) == 6 && tox2::rsxVpBranchTarget(&flow_store[4 * 4]) == 900);
    tox2::rsxVpSetBranchTarget(&flow_store[2 * 4], 0x2A5);
    CHECK(tox2::rsxVpBranchTarget(&flow_store[2 * 4]) == 0x2A5 && flow_store[2 * 4 + 1] == kBrb << 27);
    tox2::rsxVpSetBranchTarget(&flow_store[2 * 4], 6);

    // A target before the entry or past the store is reported.
    setFlow(flow_store, 2, kBrb, 1, false);
    CHECK(tox2::analyzeVertexProgramFlow(flow_store.data(), tox2::kRsxVpInstructionCount, 2).bad_target);
    setFlow(flow_store, 2, kBrb, 600, false);
    CHECK(tox2::analyzeVertexProgramFlow(flow_store.data(), tox2::kRsxVpInstructionCount, 2).bad_target);

    // A call runs its callee to the return and continues after the call; a jump that is always taken
    // leaves the instructions it skips unreached; a loop is walked once.
    std::fill(flow_store.begin(), flow_store.end(), 0);
    setFlow(flow_store, 0, kCal, 5, false);
    setFlow(flow_store, 1, kBri, 8, false);
    setFlow(flow_store, 2, 0, 0, true);  // skipped by the jump
    setFlow(flow_store, 5, kRet, 0, false);
    setFlow(flow_store, 8, kBri, 0, false);  // back to the start
    flow = tox2::analyzeVertexProgramFlow(flow_store.data(), tox2::kRsxVpInstructionCount, 0);
    CHECK(!flow.terminated && !flow.bad_target && flow.last == 8);
    CHECK(flow.reached[0] == 1 && flow.reached[1] == 1 && flow.reached[2] == 0 && flow.reached[5] == 1 &&
          flow.reached[8] == 1);

    // Collector: feed the register writes of one draw.
    namespace stdfs = std::filesystem;
    const stdfs::path root =
            stdfs::temp_directory_path() / ("tox2_rsx_census_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    stdfs::remove_all(root, ec);
    tox2::RsxArgCensus census(
            root / "census.json", root / "dump",
            [&](std::uint32_t location, std::uint32_t offset, std::uint32_t& word) {
                return location == 1 && offset >= 0x2000 && offset % 4 == 0 && fpReader((offset - 0x2000) / 4, word);
            },
            1000000);
    census.onWrite(0x0208, 8 | (2u << 5) | (2u << 8) | (10u << 16) | (9u << 24));
    census.onWrite(0x1740, 2 | (3u << 4) | (12u << 8));
    census.onWrite(0x1A0C, 1u << 31);
    census.onWrite(0x1A04, 2 | (2u << 4) | (0xA5u << 8));
    census.onWrite(0x08E4, 0x2000 | 2);
    census.onWrite(0x1E9C, 0);
    census.onWrite(0x1EA0, 0);
    for (std::uint32_t i = 0; i < 4; i++) census.onWrite(0x0B80 + i * 4, i == 3 ? 1u : (i == 1 ? (1u << 22) : 0u));
    census.onWrite(0x1808, 5);
    census.onWrite(0x1814, 0 | (5u << 24));
    census.onWrite(0x1808, 0);
    census.onFlip(1500000);
    census.onFlip(2500000);
    CHECK(census.writesOf(0x0208) == 1 && census.distinctValuesOf(0x0208) == 1);
    CHECK(census.writesOf(0x0B80) == 1 && census.draws() == 1);
    CHECK(census.fragmentProgramCount() == 1 && census.vertexProgramCount() == 1);
    const std::string json = census.formatJson();
    CHECK(json.find("\"name\": \"NV4097_SET_SURFACE_FORMAT\"") != std::string::npos);
    CHECK(json.find("A8R8G8B8_LN_NR") != std::string::npos);
    CHECK(json.find("\"flips_per_second\"") != std::string::npos);
    CHECK(json.find("\"instructions\": 2") != std::string::npos);
    CHECK(census.writeFiles());
    std::size_t dumps = 0;
    for (const auto& entry : stdfs::directory_iterator(root / "dump")) {
        const std::string name = entry.path().filename().string();
        dumps += (name.starts_with("fp_") || name.starts_with("vp_")) && name.ends_with(".bin") ? 1 : 0;
        if (name.starts_with("fp_")) CHECK(stdfs::file_size(entry.path()) == 48);
        if (name.starts_with("vp_")) CHECK(stdfs::file_size(entry.path()) == 16);
    }
    CHECK(dumps == 2 && stdfs::exists(root / "census.json"));
    stdfs::remove_all(root, ec);
}

void testRsxRegisters() {
    auto regs = std::make_unique<tox2::RsxRegisters>();
    regs->reset();

    // Plain writes land in the register file; the offset is masked to the register window.
    regs->apply(0x0A6C, 0x207);
    CHECK(regs->reg(0x0A6C) == 0x207 && regs->regs[0x0A6C >> 2] == 0x207);
    regs->apply(0x10000 + 0x0A6C, 0x201);
    CHECK(regs->reg(0x0A6C) == 0x201);

    // Eight program words at load slot 3 fill instructions 3 and 4 and move the load pointer by two.
    regs->apply(tox2::kMethodNv4097SetTransformProgramLoad, 3);
    CHECK(regs->vp_load == 3);
    for (std::uint32_t i = 0; i < 8; i++) regs->apply(tox2::kMethodNv4097SetTransformProgram + i * 4, 0x100 + i);
    for (std::uint32_t i = 0; i < 8; i++) CHECK(regs->vp_ucode[12 + i] == 0x100 + i);
    CHECK(regs->vp_ucode[11] == 0 && regs->vp_ucode[20] == 0);
    CHECK(regs->vp_load == 5 && regs->reg(tox2::kMethodNv4097SetTransformProgramLoad) == 5);
    // A block that starts mid-instruction (word 2 onwards) completes it, then starts the next one.
    regs->apply(tox2::kMethodNv4097SetTransformProgramLoad, 0);
    regs->apply(tox2::kMethodNv4097SetTransformProgram + 2 * 4, 0x20);
    regs->apply(tox2::kMethodNv4097SetTransformProgram + 3 * 4, 0x30);
    CHECK(regs->vp_ucode[2] == 0x20 && regs->vp_ucode[3] == 0x30 && regs->vp_load == 1);
    // Writes past the last instruction are dropped.
    regs->apply(tox2::kMethodNv4097SetTransformProgramLoad, tox2::kRsxVpInstructionCount);
    regs->apply(tox2::kMethodNv4097SetTransformProgram, 0xDEAD);
    CHECK(regs->vp_load == tox2::kRsxVpInstructionCount);
    regs->apply(tox2::kMethodNv4097SetTransformProgramLoad, tox2::kRsxVpInstructionCount - 1);
    for (std::uint32_t i = 0; i < 8; i++) regs->apply(tox2::kMethodNv4097SetTransformProgram + i * 4, 0x500 + i);
    CHECK(regs->vp_ucode[(tox2::kRsxVpInstructionCount - 1) * 4 + 3] == 0x503 && regs->vp_ucode[0] == 0);
    CHECK(regs->vp_load == tox2::kRsxVpInstructionCount);
    // Further writes at the end of the store neither land nor move the pointer.
    for (std::uint32_t i = 0; i < 4; i++) regs->apply(tox2::kMethodNv4097SetTransformProgram + i * 4, 0x600 + i);
    CHECK(regs->vp_load == tox2::kRsxVpInstructionCount && regs->vp_ucode[0] == 0);
    CHECK(regs->reg(tox2::kMethodNv4097SetTransformProgramLoad) == tox2::kRsxVpInstructionCount);
    regs->apply(tox2::kMethodNv4097SetTransformProgramStart, 7);
    CHECK(regs->vp_start == 7);

    // Constants are addressed from the load slot and the load slot itself stays put.
    regs->apply(tox2::kMethodNv4097SetTransformConstantLoad, 3);
    CHECK(regs->constant_load == 3);
    for (std::uint32_t i = 0; i < 8; i++) regs->apply(tox2::kMethodNv4097SetTransformConstant + i * 4, 0x200 + i);
    for (std::uint32_t i = 0; i < 8; i++) CHECK(regs->vp_constants[12 + i] == 0x200 + i);
    CHECK(regs->vp_constants[11] == 0 && regs->vp_constants[20] == 0);
    CHECK(regs->constant_load == 3 && regs->reg(tox2::kMethodNv4097SetTransformConstantLoad) == 3);
    // The last slot is 467; slot 468 and beyond are dropped.
    regs->apply(tox2::kMethodNv4097SetTransformConstantLoad, tox2::kRsxVpConstantCount - 1);
    for (std::uint32_t i = 0; i < 8; i++) regs->apply(tox2::kMethodNv4097SetTransformConstant + i * 4, 0x300 + i);
    CHECK(regs->vp_constants[(tox2::kRsxVpConstantCount - 1) * 4 + 3] == 0x303 && regs->vp_constants[0] == 0);

    // Reset restores the defaults and clears both stores.
    regs->reset();
    CHECK(regs->vp_load == 0 && regs->vp_start == 0 && regs->constant_load == 0);
    CHECK(regs->vp_ucode[12] == 0 && regs->vp_constants[12] == 0 && regs->reg(0x0A6C) == 0x201);
    CHECK(regs->reg(0x0208) == 0x121 && regs->reg(0x020C) == 0x40);             // surface format, pitch A
    CHECK(regs->reg(0x0A30) == 0x45000000 && regs->reg(0x0A38) == 0x3F000000);  // viewport scale x, z
    CHECK(regs->reg(0x0A20) == 0x45000000 && regs->reg(0x0A3C) == 0);           // viewport offset x, scale w
    CHECK(regs->reg(0x0A00) == 0x10000000 && regs->reg(0x08C0) == 0x10000000);  // viewport, scissor
    CHECK(regs->reg(0x0314) == 0x10001 && regs->reg(0x0318) == 0);              // blend sfactor, dfactor
    CHECK(regs->reg(0x0320) == 0x80068006);                                     // blend equation
    CHECK(regs->reg(0x0324) == 0x1010101);                                      // color mask
    CHECK(regs->reg(0x0308) == 0x207 && regs->reg(0x030C) == 0);                // alpha func, ref
    CHECK(regs->reg(0x0A6C) == 0x201 && regs->reg(0x0A70) == 1);                // depth func, mask
    CHECK(regs->reg(0x1830) == 0x405 && regs->reg(0x1834) == 0x901);            // cull face, front face
    CHECK(regs->reg(0x0368) == 0x1D01);                                         // shade mode
    CHECK(regs->reg(0x08E4) == 1 && regs->reg(0x1D8C) == 0xFFFFFF00);           // shader program, zs clear
    CHECK(regs->reg(0x1A04) == 0x18429 && regs->reg(0x1A04 + 15 * 0x20) == 0x18429);
    CHECK(regs->reg(0x1A14) == 0x2052000 && regs->reg(0x1A08 + 3 * 0x20) == 0x30101);
    CHECK(regs->reg(0x1740) == 2 && regs->reg(0x177C) == 2 && regs->reg(0x1680) == 0);
    CHECK(regs->reg(0x0904 + 3 * 0x20) == 0x1BC21 && regs->reg(0x0904 + 4 * 0x20) == 0);
    CHECK(regs->reg(0x1DB0) == 0xFFFFFFFF && regs->reg(0x1D7C) == 0xFFFF0000);

    // contextReset (RPCS3 `rsx_state::reset`, run on every flip): assigned registers go back, including
    // the explicit zeros; registers it does not assign, the vertex program store, the constants and the
    // load pointers stay.
    regs->apply(tox2::kMethodNv4097SetTransformProgramLoad, 4);
    regs->apply(tox2::kMethodNv4097SetTransformProgramStart, 5);
    regs->apply(tox2::kMethodNv4097SetTransformConstantLoad, 6);
    regs->apply(tox2::kMethodNv4097SetTransformConstant, 0x88);
    regs->vp_ucode[0] = 0x77;
    regs->markVpStoresChanged();
    regs->apply(0x0200, 0x05000500);         // SURFACE_CLIP_HORIZONTAL: reset writes an explicit zero
    regs->apply(0x1738, 9);                  // VERTEX_DATA_BASE_OFFSET: explicit zero
    regs->apply(0x0064, 0x30);               // SEMAPHORE_OFFSET: 0x10 here, 0x30 at init
    regs->apply(0x1A0C, 0x80000000);         // TEXTURE_CONTROL0 unit 0
    regs->apply(0x1A08, 0x5555);             // TEXTURE_ADDRESS unit 0
    regs->apply(0x1A08 + 2 * 0x20, 0x7777);  // unit 2: reset never assigns it
    regs->apply(0x1A04, 0x1234);             // TEXTURE_FORMAT: init only
    regs->apply(0x08E4, 0x2002);             // SHADER_PROGRAM: init only
    regs->contextReset();
    CHECK(regs->reg(0x0200) == 0 && regs->reg(0x1738) == 0 && regs->reg(0x0064) == 0x10);
    CHECK(regs->reg(0x1A0C) == 0x60000 && regs->reg(0x1A08) == 0x30101);
    CHECK(regs->reg(0x1A08 + 2 * 0x20) == 0x7777 && regs->reg(0x1A08 + 3 * 0x20) == 0x30101);
    CHECK(regs->reg(0x1A04) == 0x1234 && regs->reg(0x08E4) == 0x2002);
    CHECK(regs->vp_ucode[0] == 0x77 && regs->vp_constants[24] == 0x88);
    CHECK(regs->vp_load == 4 && regs->vp_start == 5 && regs->constant_load == 6);
    CHECK(regs->reg(tox2::kMethodNv4097SetTransformProgramLoad) == 4 && regs->reg(0x0A00) == 0x10000000);

    // The command types compose into the variant and start out zeroed.
    tox2::RsxCommand command = tox2::RsxDraw{};
    CHECK(std::holds_alternative<tox2::RsxDraw>(command));
    command = tox2::RsxFlip{};
    CHECK(std::get<tox2::RsxFlip>(command).flip_index == 0);
    command = tox2::RsxClear{};
    CHECK(std::get<tox2::RsxClear>(command).surfaces.color.pitch == 0);
    CHECK(static_cast<int>(tox2::RsxPrimitive::Points) == 1 && static_cast<int>(tox2::RsxPrimitive::Polygon) == 10);
}

void testRsxRegisterGenerations() {
    auto regs = std::make_unique<tox2::RsxRegisters>();
    regs->reset();

    // Names are non-zero, distinct per store, and stable while nothing is written.
    const std::uint64_t ucode = regs->vpUcodeGeneration(), constants = regs->vpConstantsGeneration();
    CHECK(ucode != 0 && constants != 0 && ucode != constants);
    CHECK(regs->vpUcodeGeneration() == ucode && regs->vpConstantsGeneration() == constants);
    regs->apply(0x0A6C, 0x207);  // plain register: neither store
    regs->apply(tox2::kMethodNv4097SetTransformProgramLoad, 2);
    regs->apply(tox2::kMethodNv4097SetTransformProgramStart, 1);
    regs->apply(tox2::kMethodNv4097SetTransformConstantLoad, 3);
    CHECK(regs->vpUcodeGeneration() == ucode && regs->vpConstantsGeneration() == constants);

    // Each store is renamed only by its own write.
    regs->apply(tox2::kMethodNv4097SetTransformConstant, 0x200);
    const std::uint64_t constants2 = regs->vpConstantsGeneration();
    CHECK(constants2 != constants && constants2 != 0 && regs->vpUcodeGeneration() == ucode);
    regs->apply(tox2::kMethodNv4097SetTransformProgram, 0x100);
    const std::uint64_t ucode2 = regs->vpUcodeGeneration();
    CHECK(ucode2 != ucode && ucode2 != 0 && regs->vpConstantsGeneration() == constants2);

    // Writes past the end of a store are dropped and change nothing.
    regs->apply(tox2::kMethodNv4097SetTransformProgramLoad, tox2::kRsxVpInstructionCount);
    regs->apply(tox2::kMethodNv4097SetTransformProgram, 0xDEAD);
    regs->apply(tox2::kMethodNv4097SetTransformConstantLoad, tox2::kRsxVpConstantCount);
    regs->apply(tox2::kMethodNv4097SetTransformConstant, 0xBEEF);
    CHECK(regs->vpUcodeGeneration() == ucode2 && regs->vpConstantsGeneration() == constants2);

    // A copy holds the same contents under the same names; a write to the copy renames only the copy.
    regs->apply(tox2::kMethodNv4097SetTransformConstantLoad, 0);
    regs->apply(tox2::kMethodNv4097SetTransformProgramLoad, 0);
    const std::uint64_t ucode3 = regs->vpUcodeGeneration(), constants3 = regs->vpConstantsGeneration();
    auto copy = std::make_unique<tox2::RsxRegisters>(*regs);
    CHECK(copy->vpUcodeGeneration() == ucode3 && copy->vpConstantsGeneration() == constants3);
    copy->apply(tox2::kMethodNv4097SetTransformConstant, 0x300);
    CHECK(copy->vpConstantsGeneration() != constants3 && copy->vpConstantsGeneration() != 0);
    CHECK(regs->vpConstantsGeneration() == constants3 && copy->vpUcodeGeneration() == ucode3);
    // A copy taken before the original was ever named gets names of its own, never equal to the original's.
    auto fresh = std::make_unique<tox2::RsxRegisters>();
    auto fresh_copy = std::make_unique<tox2::RsxRegisters>(*fresh);
    CHECK(fresh->vpUcodeGeneration() != fresh_copy->vpUcodeGeneration());

    // reset() and markVpStoresChanged() give both stores new names.
    regs->reset();
    const std::uint64_t ucode4 = regs->vpUcodeGeneration(), constants4 = regs->vpConstantsGeneration();
    CHECK(ucode4 != ucode3 && constants4 != constants3 && ucode4 != 0 && constants4 != 0);
    regs->vp_ucode[0] = 1;  // a direct write is announced by hand
    regs->markVpStoresChanged();
    CHECK(regs->vpUcodeGeneration() != ucode4 && regs->vpConstantsGeneration() != constants4);
}

std::uint64_t unimplementedCount(const std::string& name) {
    for (const auto& [key, count] : tox2::rsxUnimplementedSnapshot()) {
        if (key == name) return count;
    }
    return 0;
}

void testRsxDecode() {
    auto regs = std::make_unique<tox2::RsxRegisters>();
    regs->reset();
    tox2::rsxUnimplementedReset();

    // Surface: A8R8G8B8 1280x720 pitch 0x1400 with Z24S8 on the first target in local memory.
    regs->apply(0x0200, 1280u << 16);
    regs->apply(0x0204, 720u << 16);
    regs->apply(0x0208, 8u | (2u << 5) | (1u << 8) | (11u << 16) | (10u << 24));
    regs->apply(0x020C, 0x1400);
    regs->apply(0x0210, 0x100000);
    regs->apply(0x0214, 0x900000);
    regs->apply(0x022C, 0x1400);
    regs->apply(0x0220, 1);
    const tox2::RsxSurfaceState surface = tox2::decodeSurfaceState(*regs);
    CHECK(surface.has_color && surface.color.format == tox2::RsxColorFormat::A8R8G8B8);
    CHECK(surface.color.width == 1280 && surface.color.height == 720 && surface.color.pitch == 0x1400);
    CHECK(surface.color.offset == 0x100000 && surface.color.location == 0);
    CHECK(surface.depth_format == tox2::RsxDepthFormat::Z24S8 && surface.depth.offset == 0x900000);
    CHECK(surface.depth.pitch == 0x1400 && surface.depth.width == 1280 && surface.depth.height == 720);
    CHECK(surface.clip_x == 0 && surface.clip_y == 0 && surface.clip_w == 1280 && surface.clip_h == 720);
    CHECK(tox2::rsxUnimplementedSnapshot().empty());

    // Target B, main memory, an unsupported color format and no depth.
    regs->apply(0x0218, 0x2000);
    regs->apply(0x021C, 0x800);
    regs->apply(0x018C, tox2::kRsxContextDmaMain);
    regs->apply(0x0220, 2);
    regs->apply(0x0208, 3u | (1u << 8));
    const tox2::RsxSurfaceState other = tox2::decodeSurfaceState(*regs);
    CHECK(other.has_color && other.color.offset == 0x2000 && other.color.pitch == 0x800 && other.color.location == 1);
    CHECK(other.color.format == tox2::RsxColorFormat::Unsupported && other.depth_format == tox2::RsxDepthFormat::None);
    CHECK(unimplementedCount("surface format R5G6B5") == 1);
    regs->apply(0x0220, 0);
    CHECK(!tox2::decodeSurfaceState(*regs).has_color);
    regs->apply(0x0220, 0x13);
    CHECK(tox2::decodeSurfaceState(*regs).has_color && unimplementedCount("state multiple render targets 0x13") == 1);
    regs->apply(0x0208, 8u | (5u << 5) | (1u << 8));
    CHECK(tox2::decodeSurfaceState(*regs).depth_format == tox2::RsxDepthFormat::Unsupported);
    CHECK(unimplementedCount("depth format ?0x5") == 1);

    // Vertex arrays: slots 0, 2, 3 and 8 are float3, float3, ubyte4 and float2 with stride 36 in main memory.
    tox2::rsxUnimplementedReset();
    const auto format = [](std::uint32_t type, std::uint32_t size) { return type | (size << 4) | (36u << 8); };
    regs->apply(0x1740, format(2, 3));
    regs->apply(0x1740 + 2 * 4, format(2, 3));
    regs->apply(0x1740 + 3 * 4, format(4, 4));
    regs->apply(0x1740 + 8 * 4, format(2, 2));
    regs->apply(0x1680, 0x80001000);
    regs->apply(0x1680 + 2 * 4, 0x8000100C);
    regs->apply(0x1680 + 3 * 4, 0x80001018);
    regs->apply(0x1680 + 8 * 4, 0x8000101C);
    std::vector<tox2::RsxVertexAttrib> attribs;
    tox2::decodeVertexAttribs(*regs, attribs);
    CHECK(attribs.size() == 4);
    if (attribs.size() == 4) {
        const std::uint8_t slots[] = {0, 2, 3, 8}, types[] = {2, 2, 4, 2}, sizes[] = {3, 3, 4, 2};
        const std::uint32_t offsets[] = {0x1000, 0x100C, 0x1018, 0x101C};
        for (std::size_t i = 0; i < 4; i++) {
            CHECK(attribs[i].slot == slots[i] && attribs[i].type == types[i] && attribs[i].size == sizes[i]);
            CHECK(attribs[i].stride == 36 && attribs[i].location == 1 && attribs[i].offset == offsets[i]);
            CHECK(attribs[i].data_offset == 0);
        }
    }
    CHECK(tox2::rsxUnimplementedSnapshot().empty());
    regs->apply(0x1740 + 5 * 4, 6u | (1u << 4));  // CMP32
    tox2::decodeVertexAttribs(*regs, attribs);    // also checks that the vector is replaced, not appended to
    CHECK(attribs.size() == 5 && unimplementedCount("vertex type cmp") == 1);
    regs->apply(0x1740 + 5 * 4, 2);

    // Default attributes: slot 2 of SET_VERTEX_DATA4F_M.
    const float constants[] = {1.0f, 2.5f, -3.0f, 4.0f};
    for (std::uint32_t i = 0; i < 4; i++)
        regs->apply(0x1C00 + 2 * 16 + i * 4, std::bit_cast<std::uint32_t>(constants[i]));
    const auto defaults = tox2::decodeDefaultAttribs(*regs);
    CHECK(defaults[8] == 1.0f && defaults[9] == 2.5f && defaults[10] == -3.0f && defaults[11] == 4.0f);
    CHECK(defaults[0] == 0.0f && defaults[12] == 0.0f);
    CHECK((regs->vertex_default_written & (1u << 2)) != 0 && (regs->vertex_default_written & (1u << 5)) == 0);

    // The other float forms land in the same block, the components they do not carry read (0, 0, 0, 1),
    // and an explicit zero write still counts as written.
    regs->apply(0x1880 + 5 * 8, std::bit_cast<std::uint32_t>(7.0f));
    regs->apply(0x1880 + 5 * 8 + 4, std::bit_cast<std::uint32_t>(8.0f));
    regs->apply(0x1E40 + 7 * 4, std::bit_cast<std::uint32_t>(9.0f));
    for (std::uint32_t i = 0; i < 4; i++) regs->apply(0x1C00 + 9 * 16 + i * 4, 0);
    const auto more = tox2::decodeDefaultAttribs(*regs);
    CHECK(more[20] == 7.0f && more[21] == 8.0f && more[22] == 0.0f && more[23] == 1.0f);
    CHECK(more[28] == 9.0f && more[29] == 0.0f && more[30] == 0.0f && more[31] == 1.0f);
    CHECK(more[36] == 0.0f && more[39] == 0.0f);
    CHECK((regs->vertex_default_written & ((1u << 5) | (1u << 7) | (1u << 9))) == ((1u << 5) | (1u << 7) | (1u << 9)));
    CHECK((regs->vertex_default_written & (1u << 6)) == 0);
    // The written mask survives a context reset (it lives in RPCS3's init-only register_vertex_info) and
    // reset() clears it.
    auto scratch = std::make_unique<tox2::RsxRegisters>();
    scratch->reset();
    scratch->apply(0x1C00, 0);
    scratch->contextReset();
    CHECK(scratch->vertex_default_written == 1);
    scratch->reset();
    CHECK(scratch->vertex_default_written == 0);

    // Texture unit 0: A8R8G8B8 linear, normalized, one mip, 1024x512 in main memory, wrap and linear filters.
    tox2::rsxUnimplementedReset();
    CHECK(!tox2::rsxTextureEnabled(*regs, 0));
    regs->apply(0x1A0C, 0x80060000);
    CHECK(tox2::rsxTextureEnabled(*regs, 0) && !tox2::rsxTextureEnabled(*regs, 1));
    regs->apply(0x1A00, 0x40000);
    regs->apply(0x1A04, 2u | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16));
    regs->apply(0x1A14, 0x02022000);
    regs->apply(0x1A18, (1024u << 16) | 512u);
    regs->apply(0x1840, 0x1000);
    const tox2::RsxTextureDesc texture = tox2::decodeTexture(*regs, 0);
    CHECK(texture.unit == 0 && texture.format == 0xA5 && texture.mips == 1 && texture.dimension == 2);
    CHECK(texture.linear_layout && texture.normalized && !texture.cubemap);
    CHECK(texture.location == 1 && texture.offset == 0x40000 && texture.pitch == 0x1000);
    CHECK(texture.width == 1024 && texture.height == 512 && texture.rt_alias == -1);
    CHECK(tox2::rsxTextureWrapS(texture.address) == 1 && tox2::rsxTextureWrapT(texture.address) == 1);
    CHECK(tox2::rsxTextureMinFilter(texture.filter) == 2 && tox2::rsxTextureMagFilter(texture.filter) == 2);
    CHECK(texture.remap == 0xAAE4);
    CHECK(tox2::rsxUnimplementedSnapshot().empty());
    // The 20-bit pitch field, a compressed format and unnormalized coordinates.
    regs->apply(0x1840 + 4, 0x12345);
    CHECK(tox2::decodeTexture(*regs, 1).pitch == 0x12345);
    regs->apply(0x1A04 + 0x20, 1u | (2u << 4) | (0x86u << 8) | (1u << 16));
    CHECK(!tox2::decodeTexture(*regs, 1).linear_layout);
    CHECK(unimplementedCount("texture format DXT1_SZ_NR") == 0);  // block-compressed formats are sampled
    regs->apply(0x1A04 + 0x20, 1u | (2u << 4) | (0x84u << 8) | (1u << 16));
    tox2::rsxUnimplementedReset();
    const tox2::RsxTextureDesc r5g6b5 = tox2::decodeTexture(*regs, 1);
    CHECK(r5g6b5.format == 0x84);
    CHECK(unimplementedCount("texture format R5G6B5_SZ_NR") == 1);

    // Block-compressed sizes: 4x4 blocks of 8 (DXT1) or 16 (DXT23, DXT45) bytes, partial blocks rounded up.
    CHECK(tox2::rsxTextureBlockBytes(0x86) == 8 && tox2::rsxTextureBlockBytes(0x87 | 0x20) == 16);
    CHECK(tox2::rsxTextureBlockBytes(0x88 | 0x40) == 16 && tox2::rsxTextureBlockBytes(0x85) == 0);
    CHECK(tox2::rsxCompressedRowBytes(0x86, 16) == 32 && tox2::rsxCompressedRowBytes(0x88, 17) == 80);
    CHECK(tox2::rsxCompressedSize(0x86, 16, 8) == 64 && tox2::rsxCompressedSize(0x88, 17, 5) == 160);
    CHECK(tox2::rsxCompressedSize(0x87, 1, 1) == 16 && tox2::rsxCompressedSize(0x85, 16, 16) == 0);
    CHECK(tox2::rsxTextureFormatSampleable(0x85 | 0x20) && tox2::rsxTextureFormatSampleable(0x88));
    CHECK(!tox2::rsxTextureFormatSampleable(0x84));
    CHECK(tox2::rsxTextureFormatSampleable(0x81 | 0x20) && tox2::rsxTextureFormatSampleable(0x81));
    CHECK(tox2::rsxTextureBlockBytes(0x81) == 0);
    regs->apply(0x1A04, 1u | (2u << 4) | ((0x85u | 0x20u | 0x40u) << 8) | (1u << 16));
    CHECK(!tox2::decodeTexture(*regs, 0).normalized);
    CHECK(unimplementedCount("texture format A8R8G8B8_LN_UN") == 0);  // the renderer scales the coordinates

    // Dimension, cube bit and depth: the depth is the top 12 bits of the pitch register and counts for a 3D
    // texture only; a 3D texture is 3D whatever the cube bit says; a zero depth is one slice.
    regs->apply(0x1840, (7u << 20) | 0x40);
    regs->apply(0x1A04, 1u | (3u << 4) | (0x86u << 8) | (1u << 16));
    tox2::RsxTextureDesc volume = tox2::decodeTexture(*regs, 0);
    CHECK(volume.dimension == 3 && volume.depth == 7 && volume.pitch == 0x40);
    CHECK(tox2::rsxTextureKind(volume) == tox2::RsxTextureKind::Tex3D);
    regs->apply(0x1A04, 1u | (1u << 2) | (3u << 4) | (0x86u << 8) | (1u << 16));
    CHECK(tox2::rsxTextureKind(tox2::decodeTexture(*regs, 0)) == tox2::RsxTextureKind::Tex3D);
    regs->apply(0x1840, 0x40);
    CHECK(tox2::decodeTexture(*regs, 0).depth == 1);
    regs->apply(0x1840, (7u << 20) | 0x40);
    regs->apply(0x1A04, 1u | (1u << 2) | (2u << 4) | (0x86u << 8) | (1u << 16));
    volume = tox2::decodeTexture(*regs, 0);
    CHECK(volume.depth == 1 && tox2::rsxTextureKind(volume) == tox2::RsxTextureKind::Cube);
    regs->apply(0x1A04, 1u | (2u << 4) | (0x86u << 8) | (1u << 16));
    CHECK(tox2::rsxTextureKind(tox2::decodeTexture(*regs, 0)) == tox2::RsxTextureKind::Tex2D);
    regs->apply(0x1A04, 1u | (1u << 4) | (0x86u << 8) | (1u << 16));
    CHECK(tox2::rsxTextureKind(tox2::decodeTexture(*regs, 0)) == tox2::RsxTextureKind::Unsupported);

    // Fixed-function state: blend SRC_ALPHA / ONE_MINUS_SRC_ALPHA with ADD for color and MAX for alpha,
    // alpha test NOTEQUAL 0, back-face culling with counter-clockwise front faces.
    tox2::rsxUnimplementedReset();
    regs->apply(0x0310, 1);
    regs->apply(0x0314, 0x302u | (0x302u << 16));
    regs->apply(0x0318, 0x303u | (0x303u << 16));
    regs->apply(0x0320, 0x8006u | (0x8008u << 16));
    regs->apply(0x0304, 1);
    regs->apply(0x0308, 0x205);
    regs->apply(0x030C, 0);
    regs->apply(0x183C, 1);
    regs->apply(0x0A20 + 4, std::bit_cast<std::uint32_t>(360.0f));
    regs->apply(0x0A30 + 4, std::bit_cast<std::uint32_t>(-360.0f));
    regs->apply(0x08C0, 1280u << 16);
    regs->apply(0x08C4, (720u << 16) | 16u);
    const tox2::RsxRasterState raster = tox2::decodeRasterState(*regs);
    CHECK(raster.blend && raster.alpha_test && !raster.depth_test && raster.depth_write && raster.cull);
    CHECK(raster.blend_sfactor_rgb == 0x302 && raster.blend_sfactor_a == 0x302);
    CHECK(raster.blend_dfactor_rgb == 0x303 && raster.blend_dfactor_a == 0x303);
    CHECK(raster.blend_eq_rgb == 0x8006 && raster.blend_eq_a == 0x8008);
    CHECK(raster.alpha_func == 0x205 && raster.alpha_ref == 0);
    CHECK(raster.cull_face == 0x405 && raster.front_face == 0x901 && raster.shade_model == 0x1D01);
    CHECK(raster.depth_func == 0x201 && raster.color_mask == 0x1010101);
    CHECK(raster.viewport_offset[1] == 360.0f && raster.viewport_scale[1] == -360.0f);
    CHECK(raster.viewport_scale[0] == 2048.0f && raster.viewport_scale[2] == 0.5f);
    CHECK(raster.scissor_x == 0 && raster.scissor_w == 1280 && raster.scissor_y == 16 && raster.scissor_h == 720);
    CHECK(raster.window_origin == 1 && raster.pixel_center == 0);
    CHECK(tox2::rsxUnimplementedSnapshot().empty());
    regs->apply(0x030C, 0x12345680);
    CHECK(tox2::decodeRasterState(*regs).alpha_ref == 0x80);
    regs->apply(0x0320, 0xF005u | (0x800Bu << 16));
    regs->apply(0x0328, 1);
    const tox2::RsxRasterState signed_equation = tox2::decodeRasterState(*regs);
    CHECK(signed_equation.blend_eq_rgb == 0xF005 && signed_equation.blend_eq_a == 0x800B);
    CHECK(unimplementedCount("blend equation REVERSE_SUBTRACT_SIGNED") == 1);
    CHECK(unimplementedCount("state stencil test") == 0);  // modelled by the renderer, no longer a census kind

    // Index array, draw range, primitive and clear.
    regs->apply(0x181C, 0x1000);
    regs->apply(0x1820, 1u | (1u << 4));
    const tox2::RsxIndexArray index = tox2::decodeIndexArray(*regs);
    CHECK(index.location == 1 && index.offset == 0x1000 && index.type == tox2::kRsxIndexTypeU16);
    const tox2::RsxDrawRange range = tox2::decodeDrawRange(0x03000010);
    CHECK(range.first == 0x10 && range.count == 4);
    CHECK(tox2::decodeDrawRange(0xFF000000).count == 256);
    tox2::RsxPrimitive primitive = tox2::RsxPrimitive::Points;
    CHECK(tox2::decodePrimitive(5, primitive) && primitive == tox2::RsxPrimitive::Triangles);
    CHECK(tox2::decodePrimitive(10, primitive) && primitive == tox2::RsxPrimitive::Polygon);
    CHECK(!tox2::decodePrimitive(0, primitive) && primitive == tox2::RsxPrimitive::Polygon);
    CHECK(unimplementedCount("primitive ?0xb") == 0);
    CHECK(!tox2::decodePrimitive(11, primitive) && unimplementedCount("primitive ?0xb") == 1);

    regs->apply(0x1D90, 0xFF102030);
    regs->apply(0x1D8C, 0xFFFFFF00);
    const tox2::RsxClear clear = tox2::decodeClear(*regs, 0xF3);
    CHECK(clear.mask == 0xF3 && clear.color_argb == 0xFF102030 && clear.depth_stencil == 0xFFFFFF00);
    CHECK(clear.scissor[0] == 0 && clear.scissor[1] == 16 && clear.scissor[2] == 1280 && clear.scissor[3] == 720);
    CHECK(clear.surfaces.clip_w == 1280);

    // Written methods with render semantics that the builder ignores.
    CHECK(std::string_view(tox2::rsxIgnoredMethodName(0x1940)) == "NV4097_SET_VERTEX_DATA4UB_M");
    CHECK(tox2::rsxIgnoredMethodName(0x197C) != nullptr && tox2::rsxIgnoredMethodName(0x1900) != nullptr);
    CHECK(tox2::rsxIgnoredMethodName(0x1500) == nullptr);  // the float forms are decoded into the defaults
    CHECK(tox2::rsxIgnoredMethodName(0x1C00) == nullptr && tox2::rsxIgnoredMethodName(0x1814) == nullptr);
    CHECK(tox2::rsxIgnoredMethodName(0x1818) != nullptr && tox2::rsxIgnoredMethodName(0xC40C) != nullptr);

    // The lookup is a bitmap built from the ranges; it must agree with the plain range scan for every offset,
    // unaligned ones and the whole 16-bit method space and beyond included.
    {
        struct Range {
            std::uint32_t first, count;
            const char* name;
        };
        const Range ranges[] = {
                {tox2::kMethodNv4097SetVertexDataScaled4sM, 32, "NV4097_SET_VERTEX_DATA_SCALED4S_M"},
                {tox2::kMethodNv4097ArrayElement16, 1, "NV4097_ARRAY_ELEMENT16"},
                {tox2::kMethodNv4097ArrayElement32, 1, "NV4097_ARRAY_ELEMENT32"},
                {tox2::kMethodNv4097InlineArray, 1, "NV4097_INLINE_ARRAY"},
                {tox2::kMethodNv4097SetVertexData2sM, 16, "NV4097_SET_VERTEX_DATA2S_M"},
                {tox2::kMethodNv4097SetVertexData4ubM, 16, "NV4097_SET_VERTEX_DATA4UB_M"},
                {tox2::kMethodNv4097SetVertexData4sM, 32, "NV4097_SET_VERTEX_DATA4S_M"},
                {tox2::kMethodNv3089ImageIn, 1, "NV3089_IMAGE_IN"},
        };
        std::size_t ignored = 0;
        for (std::uint32_t method = 0; method < 0x20000; method++) {
            const char* expected = nullptr;
            for (const Range& range : ranges) {
                if (method >= range.first && method < range.first + range.count * 4) {
                    expected = range.name;
                    break;
                }
            }
            const char* actual = tox2::rsxIgnoredMethodName(method);
            CHECK((actual == nullptr) == (expected == nullptr));
            if (actual != nullptr && expected != nullptr) CHECK(std::string_view(actual) == expected);
            ignored += actual != nullptr;
        }
        CHECK(ignored == (32 + 1 + 1 + 1 + 16 + 16 + 32 + 1) * 4);
        CHECK(tox2::rsxIgnoredMethodName(0xFFFFFFFC) == nullptr);
    }

    // The census prints the section once something was noted, most frequent first.
    tox2::rsxUnimplementedReset();
    tox2::setRsxUnimplementedProvider(
            [] {
                return tox2::RsxUnimplementedCensus{tox2::rsxUnimplementedSnapshot(), tox2::rsxUnimplementedDropped()};
            },
            &tox2::rsxUnimplementedReset);
    CHECK(tox2::formatCensus().find("rsx unimplemented") == std::string::npos);
    tox2::rsxNoteUnimplemented("texture format", "DXT1");
    tox2::rsxNoteUnimplemented("fp opcode", "DDX");
    tox2::rsxNoteUnimplemented("texture format", "DXT1");
    const std::string census = tox2::formatCensus();
    const std::size_t header =
            census.find("census: rsx unimplemented 2 kinds\n  texture format DXT1: 2\n  fp opcode DDX: 1");
    CHECK(header != std::string::npos);

    // Past the cap of distinct kinds, new kinds are only summed and the header counts real kinds.
    tox2::rsxUnimplementedReset();
    for (int i = 0; i < 1030; i++) tox2::rsxNoteUnimplemented("state", std::to_string(i));
    CHECK(tox2::rsxUnimplementedSnapshot().size() == 1024 && tox2::rsxUnimplementedDropped() == 6);
    const std::string capped = tox2::formatCensus();
    CHECK(capped.find("census: rsx unimplemented 1024 kinds\n") != std::string::npos);
    CHECK(capped.find("\n  other kinds (limit reached): 6") != std::string::npos);

    // resetCensus() clears the counts through the provider and forgets it.
    tox2::resetCensus();
    CHECK(tox2::rsxUnimplementedSnapshot().empty() && tox2::rsxUnimplementedDropped() == 0);
    tox2::rsxNoteUnimplemented("state", "after reset");
    CHECK(tox2::formatCensus().find("rsx unimplemented") == std::string::npos);
    tox2::rsxUnimplementedReset();
}

// RSX-visible memory backed by two byte arrays: reads outside them fail.
class FakeRsxMemory final : public tox2::RsxMemoryView {
public:
    FakeRsxMemory() : local(0x10000), main(0x10000) {}
    bool read(std::uint32_t location, std::uint32_t offset, std::span<std::uint8_t> out) override {
        reads[{location, offset}]++;
        const std::vector<std::uint8_t>& bytes = location == 0 ? local : main;
        if (offset > bytes.size() || out.size() > bytes.size() - offset) return false;
        std::memcpy(out.data(), bytes.data() + offset, out.size());
        return true;
    }
    // Hashes in place like a view over plain memory; counted apart from the copies in `reads`.
    bool hashRange(std::uint32_t location, std::uint32_t offset, std::size_t size, std::uint64_t& out) override {
        hashes[{location, offset}]++;
        const std::vector<std::uint8_t>& bytes = location == 0 ? local : main;
        if (offset > bytes.size() || size > bytes.size() - offset) return false;
        out = tox2::rsxHashBytes(std::span<const std::uint8_t>(bytes).subspan(offset, size));
        return true;
    }
    void putWord(std::uint32_t location, std::uint32_t offset, std::uint32_t word) {
        std::vector<std::uint8_t>& bytes = location == 0 ? local : main;
        for (int i = 0; i < 4; i++) bytes[offset + i] = static_cast<std::uint8_t>(word >> (24 - 8 * i));
    }
    // Reads that started at (location, offset).
    int readsAt(std::uint32_t location, std::uint32_t offset) const {
        const auto it = reads.find({location, offset});
        return it == reads.end() ? 0 : it->second;
    }
    std::vector<std::uint8_t> local, main;
    // Hashes in place that started at (location, offset).
    int hashesAt(std::uint32_t location, std::uint32_t offset) const {
        const auto it = hashes.find({location, offset});
        return it == hashes.end() ? 0 : it->second;
    }
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> reads, hashes;
};

// Guest layout of mip chains and de-swizzling, checked against the rules of RPCS3's `TextureUtils.cpp`.
void testRsxTextureLayout() {
    // Morton order of a square image: x and y bits interleaved, x in the even positions.
    CHECK(tox2::rsxSwizzleIndex(0, 0, 2, 2) == 0 && tox2::rsxSwizzleIndex(1, 0, 2, 2) == 1);
    CHECK(tox2::rsxSwizzleIndex(0, 1, 2, 2) == 2 && tox2::rsxSwizzleIndex(1, 1, 2, 2) == 3);
    CHECK(tox2::rsxSwizzleIndex(2, 0, 2, 2) == 4 && tox2::rsxSwizzleIndex(3, 1, 2, 2) == 7);
    CHECK(tox2::rsxSwizzleIndex(0, 2, 2, 2) == 8 && tox2::rsxSwizzleIndex(3, 3, 2, 2) == 15);
    // A wide image interleaves up to the shorter side (one bit here) and counts the rest of x above.
    CHECK(tox2::rsxSwizzleIndex(0, 1, 3, 1) == 2 && tox2::rsxSwizzleIndex(3, 1, 3, 1) == 7);
    CHECK(tox2::rsxSwizzleIndex(7, 0, 3, 1) == 13 && tox2::rsxSwizzleIndex(7, 1, 3, 1) == 15);
    // A tall image counts the rest of y above.
    CHECK(tox2::rsxSwizzleIndex(1, 5, 1, 3) == 11 && tox2::rsxSwizzleIndex(0, 7, 1, 3) == 14);
    // A one-texel-high image is a plain row.
    for (std::uint32_t x = 0; x < 8; x++) CHECK(tox2::rsxSwizzleIndex(x, 0, 3, 0) == x);

    // Every texel position is hit exactly once.
    for (const auto& [log2_w, log2_h] : {std::pair{2u, 2u}, std::pair{3u, 1u}, std::pair{1u, 4u}, std::pair{5u, 0u}}) {
        std::vector<int> seen(std::size_t{1} << (log2_w + log2_h), 0);
        for (std::uint32_t y = 0; y < (1u << log2_h); y++) {
            for (std::uint32_t x = 0; x < (1u << log2_w); x++) seen[tox2::rsxSwizzleIndex(x, y, log2_w, log2_h)]++;
        }
        CHECK(std::all_of(seen.begin(), seen.end(), [](int n) { return n == 1; }));
    }

    // De-swizzle 4x4, 8x2 and 2x8 A8R8G8B8: guest texel k holds the bytes {k, 0, 0, 0}.
    for (const auto& [width, height] : {std::pair{4u, 4u}, std::pair{8u, 2u}, std::pair{2u, 8u}}) {
        tox2::RsxTextureLayout layout;
        CHECK(tox2::rsxTextureLayout(0x85, false, width, height, 1, 0, 1, layout));
        CHECK(layout.levels.size() == 1 && layout.block_bytes == 4 && layout.levels[0].swizzled);
        CHECK(layout.guest_bytes == width * height * 4 && layout.packed_bytes == width * height * 4);
        std::vector<std::uint8_t> guest(layout.guest_bytes, 0), packed(layout.packed_bytes, 0xEE);
        for (std::uint32_t k = 0; k < width * height; k++) guest[k * 4] = static_cast<std::uint8_t>(k);
        CHECK(tox2::rsxExtractTextureLevel(layout.levels[0], 4, guest, packed));
        const std::uint32_t log2_w = std::countr_zero(width), log2_h = std::countr_zero(height);
        bool all = true;
        for (std::uint32_t y = 0; y < height; y++) {
            for (std::uint32_t x = 0; x < width; x++) {
                all = all && packed[(y * width + x) * 4] == tox2::rsxSwizzleIndex(x, y, log2_w, log2_h) &&
                      packed[(y * width + x) * 4 + 1] == 0;
            }
        }
        CHECK(all);
    }
    // The 4x4 case in full: row 0 reads guest texels 0, 1, 4, 5 and row 3 reads 10, 11, 14, 15.
    {
        tox2::RsxTextureLayout layout;
        CHECK(tox2::rsxTextureLayout(0x85, false, 4, 4, 1, 0, 1, layout));
        std::vector<std::uint8_t> guest(64, 0), packed(64, 0);
        for (std::uint32_t k = 0; k < 16; k++) guest[k * 4] = static_cast<std::uint8_t>(k);
        CHECK(tox2::rsxExtractTextureLevel(layout.levels[0], 4, guest, packed));
        const std::uint8_t expected[16] = {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15};
        bool same = true;
        for (std::uint32_t i = 0; i < 16; i++) same = same && packed[i * 4] == expected[i];
        CHECK(same);
        CHECK(!tox2::rsxExtractTextureLevel(layout.levels[0], 4, std::span<const std::uint8_t>(guest).first(63),
                                            packed));
    }

    // Level counts: the register value is clamped to the chain that ends at one texel (one block for DXT).
    CHECK(tox2::rsxTextureMipCount(0x85, 256, 16, 0) == 1 && tox2::rsxTextureMipCount(0x85, 256, 16, 1) == 1);
    CHECK(tox2::rsxTextureMipCount(0x85, 256, 16, 3) == 3 && tox2::rsxTextureMipCount(0x85, 256, 16, 100) == 9);
    CHECK(tox2::rsxTextureMipCount(0x85, 1, 1, 5) == 1 && tox2::rsxTextureMipCount(0xA5, 1012, 132, 3) == 3);
    CHECK(tox2::rsxTextureMipCount(0x86, 16, 16, 5) == 3 && tox2::rsxTextureMipCount(0x86, 64, 16, 9) == 3);
    CHECK(tox2::rsxTextureMipCount(0x88, 4, 4, 5) == 1 && tox2::rsxTextureMipCount(0x88, 2, 2, 5) == 1);

    // A linear chain keeps the pitch of level 0: 8x4 texels with a 64-byte pitch, levels 8x4, 4x2, 2x1, 1x1.
    {
        tox2::RsxTextureLayout layout;
        CHECK(tox2::rsxTextureLayout(0xA5, true, 8, 4, 9, 64, 1, layout));
        CHECK(layout.mip_count == 4 && layout.levels.size() == 4);
        const std::uint64_t guest_offset[4] = {0, 256, 384, 448}, packed_offset[4] = {0, 128, 160, 168};
        const std::uint32_t widths[4] = {8, 4, 2, 1}, heights[4] = {4, 2, 1, 1};
        for (std::size_t i = 0; i < 4; i++) {
            CHECK(layout.levels[i].guest_offset == guest_offset[i] && layout.levels[i].guest_pitch == 64);
            CHECK(layout.levels[i].packed_offset == packed_offset[i] && !layout.levels[i].swizzled);
            CHECK(layout.levels[i].width == widths[i] && layout.levels[i].height == heights[i]);
        }
        CHECK(layout.levels[0].guest_bytes == 64 * 3 + 32 && layout.levels[3].row_bytes == 4);
        CHECK(layout.guest_bytes == 448 + 4 && layout.packed_bytes == 172);
        // Extraction drops the padding; guest byte i holds i modulo 256.
        std::vector<std::uint8_t> guest(layout.guest_bytes), packed(layout.packed_bytes);
        for (std::size_t i = 0; i < guest.size(); i++) guest[i] = static_cast<std::uint8_t>(i);
        for (const tox2::RsxTextureLevel& level : layout.levels) {
            CHECK(tox2::rsxExtractTextureLevel(
                    level, 4, guest, std::span<std::uint8_t>(packed).subspan(level.packed_offset, level.packed_bytes)));
        }
        CHECK(packed[0] == 0 && packed[31] == 31 && packed[32] == 64 && packed[63] == 95 && packed[96] == 192);
        CHECK(packed[128] == 0 && packed[143] == 15 && packed[144] == 64);
        CHECK(packed[160] == 128 && packed[168] == 192);
        // A pitch of 0 stands for the row size.
        CHECK(tox2::rsxTextureLayout(0xA5, true, 8, 4, 1, 0, 1, layout) && layout.levels[0].guest_pitch == 32);
        // A pitch below one row, or a size of 0, has no layout.
        CHECK(!tox2::rsxTextureLayout(0xA5, true, 8, 4, 1, 28, 1, layout));
        CHECK(!tox2::rsxTextureLayout(0xA5, true, 0, 4, 1, 32, 1, layout));
    }
    // Odd sizes halve with truncation: 5x3 gives 5x3, 2x1, 1x1.
    {
        tox2::RsxTextureLayout layout;
        CHECK(tox2::rsxTextureLayout(0xA5, true, 5, 3, 3, 20, 1, layout));
        CHECK(layout.levels.size() == 3 && layout.levels[1].width == 2 && layout.levels[1].height == 1);
        CHECK(layout.levels[2].width == 1 && layout.levels[1].guest_offset == 60);
    }
    // A swizzled chain has no padding: 8x4 gives 128, 32, 8 and 4 bytes back to back.
    {
        tox2::RsxTextureLayout layout;
        CHECK(tox2::rsxTextureLayout(0x85, false, 8, 4, 4, 9999, 1, layout));  // the pitch register is ignored
        CHECK(layout.levels.size() == 4 && layout.guest_bytes == 172 && layout.packed_bytes == 172);
        const std::uint64_t offset[4] = {0, 128, 160, 168};
        for (std::size_t i = 0; i < 4; i++) {
            CHECK(layout.levels[i].guest_offset == offset[i] && layout.levels[i].packed_offset == offset[i]);
            CHECK(layout.levels[i].swizzled && layout.levels[i].guest_pitch == layout.levels[i].row_bytes);
        }
        // Not a power of two: no swizzled layout. The same size is fine when linear.
        CHECK(!tox2::rsxTextureLayout(0x85, false, 12, 4, 1, 0, 1, layout));
        CHECK(tox2::rsxTextureLayout(0xA5, true, 12, 4, 1, 0, 1, layout));
    }
    // DXT1 with the swizzle flag: contiguous block rows per level, 16x16 gives 128, 32 and 8 bytes.
    {
        tox2::RsxTextureLayout layout;
        CHECK(tox2::rsxTextureLayout(0x86, false, 16, 16, 5, 128, 1, layout));
        CHECK(layout.block_bytes == 8 && layout.levels.size() == 3);
        CHECK(layout.levels[0].packed_bytes == 128 && layout.levels[1].packed_bytes == 32 &&
              layout.levels[2].packed_bytes == 8);
        CHECK(layout.levels[1].guest_offset == 128 && layout.levels[2].guest_offset == 160 &&
              layout.guest_bytes == 168 && !layout.levels[0].swizzled);
        // 12x12 is not a power of two, which DXT does not care about: 3x3 blocks, then 6x6 texels in 2x2.
        CHECK(tox2::rsxTextureLayout(0x87, false, 12, 12, 2, 0, 1, layout));
        CHECK(layout.levels[0].packed_bytes == 9 * 16 && layout.levels[1].blocks_w == 2 && layout.levels[1].width == 6);
        // 64x16 stops after three levels, whatever the register says.
        CHECK(tox2::rsxTextureLayout(0x86, false, 64, 16, 9, 128, 1, layout) && layout.levels.size() == 3);
        CHECK(layout.levels[2].width == 16 && layout.levels[2].height == 4 && layout.levels[2].packed_bytes == 32);
    }
    // DXT45 with the linear flag uses the pitch register for every level: 8x8 with a 64-byte pitch.
    {
        tox2::RsxTextureLayout layout;
        CHECK(tox2::rsxTextureLayout(0xA8, true, 8, 8, 2, 64, 1, layout));
        CHECK(layout.levels.size() == 2 && layout.block_bytes == 16);
        CHECK(layout.levels[0].guest_pitch == 64 && layout.levels[0].guest_bytes == 64 + 32 &&
              layout.levels[0].row_bytes == 32);
        CHECK(layout.levels[1].guest_offset == 128 && layout.levels[1].guest_pitch == 64 &&
              layout.levels[1].row_bytes == 16 && layout.levels[1].packed_offset == 64);
        CHECK(!tox2::rsxTextureLayout(0xA8, true, 8, 8, 2, 16, 1, layout));
    }
    // Faces of a cube map: a texture without the linear flag aligns each to 128 bytes; a linear one does not.
    {
        tox2::RsxTextureLayout layout;
        CHECK(tox2::rsxTextureLayout(0x85, false, 4, 4, 1, 0, 6, layout) && layout.levels.size() == 6);
        CHECK(layout.levels[1].guest_offset == 128 && layout.levels[5].guest_offset == 640 &&
              layout.levels[1].packed_offset == 64);
        CHECK(tox2::rsxTextureLayout(0xA5, true, 4, 4, 1, 0, 6, layout) && layout.levels[1].guest_offset == 64);
    }
    // B8 linear: one byte per texel, the pitch register may exceed the row (6x3 texels in a 16-byte pitch).
    {
        tox2::RsxTextureLayout layout;
        CHECK(tox2::rsxTextureLayout(0x81 | 0x20, true, 6, 3, 1, 16, 1, layout));
        CHECK(layout.block_bytes == 1 && layout.levels.size() == 1 && !layout.levels[0].swizzled);
        CHECK(layout.levels[0].row_bytes == 6 && layout.levels[0].guest_pitch == 16);
        CHECK(layout.levels[0].guest_bytes == 16 * 2 + 6 && layout.guest_bytes == 38 && layout.packed_bytes == 18);
        std::vector<std::uint8_t> guest(layout.guest_bytes), packed(layout.packed_bytes, 0xEE);
        for (std::size_t i = 0; i < guest.size(); i++) guest[i] = static_cast<std::uint8_t>(i);
        CHECK(tox2::rsxExtractTextureLevel(layout.levels[0], layout.block_bytes, guest, packed));
        // Rows start at guest bytes 0, 16 and 32; the padding is dropped.
        const std::uint8_t expected[18] = {0, 1, 2, 3, 4, 5, 16, 17, 18, 19, 20, 21, 32, 33, 34, 35, 36, 37};
        CHECK(std::equal(packed.begin(), packed.end(), expected));
        // A pitch of 0 stands for the row size; a pitch below one row has no layout.
        CHECK(tox2::rsxTextureLayout(0xA1, true, 6, 3, 1, 0, 1, layout) && layout.levels[0].guest_pitch == 6);
        CHECK(!tox2::rsxTextureLayout(0xA1, true, 6, 3, 1, 5, 1, layout));
        // Mip levels keep the pitch of level 0: 8x4 in a 12-byte pitch gives levels at 0, 48 and 72.
        CHECK(tox2::rsxTextureLayout(0xA1, true, 8, 4, 3, 12, 1, layout) && layout.levels.size() == 3);
        CHECK(layout.levels[1].guest_offset == 48 && layout.levels[2].guest_offset == 72 &&
              layout.levels[1].packed_offset == 32 && layout.levels[2].packed_offset == 40);
    }
    // B8 swizzled: the Morton order of 4-byte texels with one byte per texel, 8x4 and a 4x4 cube map.
    {
        tox2::RsxTextureLayout layout;
        CHECK(tox2::rsxTextureLayout(0x81, false, 8, 4, 1, 0, 1, layout));
        CHECK(layout.block_bytes == 1 && layout.levels[0].swizzled && layout.guest_bytes == 32);
        std::vector<std::uint8_t> guest(32), packed(32, 0xEE);
        for (std::uint32_t k = 0; k < 32; k++) guest[k] = static_cast<std::uint8_t>(k);
        CHECK(tox2::rsxExtractTextureLevel(layout.levels[0], layout.block_bytes, guest, packed));
        // Row y of 8 texels: x and y bits interleave up to the shorter side (2 bits), x2 counts above them.
        const std::uint8_t row0[8] = {0, 1, 4, 5, 16, 17, 20, 21};
        const std::uint8_t row3[8] = {10, 11, 14, 15, 26, 27, 30, 31};
        CHECK(std::equal(packed.begin(), packed.begin() + 8, row0) &&
              std::equal(packed.begin() + 24, packed.end(), row3));
        CHECK(tox2::rsxTextureLayout(0x81, false, 4, 4, 1, 0, 6, layout) && layout.levels.size() == 6);
        CHECK(layout.levels[1].guest_offset == 128 && layout.levels[5].guest_offset == 640 &&
              layout.levels[1].packed_offset == 16 && layout.packed_bytes == 96);
    }
    // Formats other than A8R8G8B8, B8 and DXT have no layout.
    {
        tox2::RsxTextureLayout layout;
        CHECK(!tox2::rsxTextureLayout(0x84, true, 4, 4, 1, 0, 1, layout));
    }

    // A cube map with a mip chain: every face holds its levels back to back (64 + 16 + 4 bytes) and the next
    // face starts at the next multiple of 128.
    {
        tox2::RsxTextureLayout layout;
        CHECK(tox2::rsxTextureLayout(0x85, false, 4, 4, 3, 0, 6, layout) && layout.levels.size() == 18);
        CHECK(layout.levels[3].layer == 1 && layout.levels[3].level == 0 && layout.levels[3].guest_offset == 128);
        CHECK(layout.levels[4].guest_offset == 128 + 64 && layout.levels[5].guest_offset == 128 + 80);
        CHECK(layout.levels[15].layer == 5 && layout.levels[15].guest_offset == 640);
        CHECK(layout.levels[3].packed_offset == 84 && layout.packed_bytes == 6 * 84);
        CHECK(layout.guest_bytes == 640 + 84);
    }

    // Morton order of a 3D image: one bit of x, y and z per round, a coordinate leaving the rounds once its
    // bits are used up. With no depth bits it is the 2D order.
    CHECK(tox2::rsxSwizzleIndex3d(1, 0, 0, 1, 1, 1) == 1 && tox2::rsxSwizzleIndex3d(0, 1, 0, 1, 1, 1) == 2);
    CHECK(tox2::rsxSwizzleIndex3d(0, 0, 1, 1, 1, 1) == 4 && tox2::rsxSwizzleIndex3d(1, 1, 1, 1, 1, 1) == 7);
    // 4x2x2: x0, y0, z0, then x1 above them.
    CHECK(tox2::rsxSwizzleIndex3d(2, 0, 0, 2, 1, 1) == 8 && tox2::rsxSwizzleIndex3d(3, 1, 1, 2, 1, 1) == 15);
    CHECK(tox2::rsxSwizzleIndex3d(2, 1, 0, 2, 1, 1) == 10);
    // 2x2x4: z1 follows z0.
    CHECK(tox2::rsxSwizzleIndex3d(0, 0, 2, 1, 1, 2) == 8 && tox2::rsxSwizzleIndex3d(1, 1, 3, 1, 1, 2) == 15);
    for (const auto& [log2_w, log2_h, log2_d] :
         {std::tuple{2u, 2u, 0u}, std::tuple{3u, 1u, 0u}, std::tuple{1u, 2u, 3u}, std::tuple{0u, 3u, 2u}}) {
        std::vector<int> seen(std::size_t{1} << (log2_w + log2_h + log2_d), 0);
        bool same_as_2d = true;
        for (std::uint32_t z = 0; z < (1u << log2_d); z++) {
            for (std::uint32_t y = 0; y < (1u << log2_h); y++) {
                for (std::uint32_t x = 0; x < (1u << log2_w); x++) {
                    seen[tox2::rsxSwizzleIndex3d(x, y, z, log2_w, log2_h, log2_d)]++;
                    same_as_2d = same_as_2d && (log2_d != 0 || tox2::rsxSwizzleIndex3d(x, y, z, log2_w, log2_h, 0) ==
                                                                       tox2::rsxSwizzleIndex(x, y, log2_w, log2_h));
                }
            }
        }
        CHECK(std::all_of(seen.begin(), seen.end(), [](int n) { return n == 1; }) && same_as_2d);
    }

    // A linear 3D chain: 4x2 texels, 3 slices, 24-byte pitch. Level 0 is 24 * 2 * 3 bytes of guest memory (the
    // last row without padding), level 1 is 2x1x1 and follows it.
    {
        tox2::RsxTextureLayout layout;
        CHECK(tox2::rsxTextureLayout(0xA5, true, 4, 2, 2, 24, 1, layout, 3) && layout.levels.size() == 2);
        CHECK(layout.levels[0].depth == 3 && layout.levels[0].guest_bytes == 24 * 5 + 16);
        CHECK(layout.levels[0].packed_bytes == 16 * 2 * 3 && !layout.levels[0].vtc);
        CHECK(layout.levels[1].depth == 1 && layout.levels[1].width == 2 && layout.levels[1].guest_offset == 144);
        CHECK(layout.levels[1].packed_offset == 96 && layout.guest_bytes == 144 + 8 && layout.packed_bytes == 104);
        std::vector<std::uint8_t> guest(layout.guest_bytes), packed(layout.packed_bytes);
        for (std::size_t i = 0; i < guest.size(); i++) guest[i] = static_cast<std::uint8_t>(i);
        for (const tox2::RsxTextureLevel& level : layout.levels) {
            CHECK(tox2::rsxExtractTextureLevel(
                    level, 4, guest, std::span<std::uint8_t>(packed).subspan(level.packed_offset, level.packed_bytes)));
        }
        // Packed row r (slice r / 2, row r % 2) comes from guest row r: planes follow each other with the pitch.
        CHECK(packed[0] == 0 && packed[15] == 15 && packed[16] == 24 && packed[32] == 48 && packed[80] == 120);
        CHECK(packed[96] == 144);
        // The depth halves like the sides: 4x4x4 gives 4, 2, 1 slices.
        CHECK(tox2::rsxTextureLayout(0xA5, true, 4, 4, 3, 0, 1, layout, 4));
        CHECK(layout.levels[0].depth == 4 && layout.levels[1].depth == 2 && layout.levels[2].depth == 1);
        // Every level keeps the 16-byte pitch of level 0: 2x2x2 rows of 16 bytes follow the first 256 bytes.
        CHECK(layout.levels[1].guest_offset == 4 * 4 * 4 * 4 && layout.levels[2].guest_offset == 256 + 16 * 2 * 2);
    }

    // A swizzled 3D image: guest texel k holds the bytes {k, 0, 0, 0}.
    {
        tox2::RsxTextureLayout layout;
        CHECK(tox2::rsxTextureLayout(0x85, false, 4, 2, 1, 0, 1, layout, 2) && layout.levels[0].swizzled);
        CHECK(layout.guest_bytes == 4 * 2 * 2 * 4 && layout.packed_bytes == layout.guest_bytes);
        std::vector<std::uint8_t> guest(layout.guest_bytes, 0), packed(layout.packed_bytes, 0xEE);
        for (std::uint32_t k = 0; k < 16; k++) guest[k * 4] = static_cast<std::uint8_t>(k);
        CHECK(tox2::rsxExtractTextureLevel(layout.levels[0], 4, guest, packed));
        const auto texel = [&](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
            return packed[((z * 2 + y) * 4 + x) * 4];
        };
        CHECK(texel(0, 0, 0) == 0 && texel(1, 0, 0) == 1 && texel(0, 1, 0) == 2 && texel(0, 0, 1) == 4);
        CHECK(texel(2, 0, 0) == 8 && texel(2, 1, 0) == 10 && texel(3, 1, 1) == 15);
        // The depth has to be a power of two as well; a linear layout takes any depth.
        CHECK(!tox2::rsxTextureLayout(0x85, false, 4, 4, 1, 0, 1, layout, 3));
        CHECK(tox2::rsxTextureLayout(0xA5, true, 4, 4, 1, 0, 1, layout, 3));
        CHECK(!tox2::rsxTextureLayout(0x85, false, 4, 4, 1, 0, 1, layout, 0));
    }

    // DXT1 3D, power-of-two sides, depth above 1: the VTC order. 8x8 texels are 4 blocks per slice. Slices 0..3
    // are interleaved block by block (guest block 4 * b + z holds block b of slice z); the last group of two
    // slices (depth 6) interleaves the same way with a stride of 2. Guest block i holds the byte i.
    {
        tox2::RsxTextureLayout layout;
        CHECK(tox2::rsxTextureLayout(0x86, false, 8, 8, 1, 0, 1, layout, 6) && layout.levels[0].vtc);
        CHECK(layout.guest_bytes == 24 * 8 && layout.packed_bytes == 24 * 8 && !layout.levels[0].swizzled);
        std::vector<std::uint8_t> guest(layout.guest_bytes), packed(layout.packed_bytes);
        for (std::size_t i = 0; i < guest.size(); i++) guest[i] = static_cast<std::uint8_t>(i / 8);
        CHECK(tox2::rsxExtractTextureLevel(layout.levels[0], 8, guest, packed));
        bool vtc = true;
        for (std::uint32_t z = 0; z < 6; z++) {
            for (std::uint32_t b = 0; b < 4; b++) {
                const std::uint32_t from = z < 4 ? b * 4 + z : 16 + b * 2 + (z - 4);
                vtc = vtc && packed[(z * 4 + b) * 8] == from && packed[(z * 4 + b) * 8 + 7] == from;
            }
        }
        CHECK(vtc);
        // One slice has no VTC order; neither does a side that is not a power of two (12x8 gives 3x2 blocks).
        CHECK(tox2::rsxTextureLayout(0x86, false, 8, 8, 1, 0, 1, layout, 1) && !layout.levels[0].vtc);
        CHECK(tox2::rsxTextureLayout(0x86, false, 12, 8, 1, 0, 1, layout, 2) && !layout.levels[0].vtc);
        CHECK(layout.packed_bytes == 6 * 8 * 2 && layout.guest_bytes == layout.packed_bytes);
        // A mip chain: the second level has its own depth and VTC order (4x4x3 is one block per slice).
        CHECK(tox2::rsxTextureLayout(0x87, false, 8, 8, 2, 0, 1, layout, 6) && layout.levels.size() == 2);
        CHECK(layout.levels[1].depth == 3 && layout.levels[1].vtc && layout.levels[1].packed_bytes == 3 * 16);
        CHECK(layout.levels[1].guest_offset == 24 * 16 && layout.levels[1].packed_offset == 24 * 16);
    }
}

// Bytes of a texture's converted blob; empty when the builder copied none.
std::size_t texSize(const tox2::RsxTextureDesc& texture) { return texture.data ? texture.data->size() : 0; }

std::uint8_t texByte(const tox2::RsxTextureDesc& texture, std::size_t index) {
    return texture.data && index < texture.data->size() ? (*texture.data)[index] : 0xEE;
}

void testRsxFrameBuilder() {
    auto regs = std::make_unique<tox2::RsxRegisters>();
    regs->reset();
    tox2::rsxUnimplementedReset();
    FakeRsxMemory memory;
    tox2::RsxFrameBuilder builder(memory);
    const auto write = [&](std::uint32_t method, std::uint32_t value) {
        regs->apply(method, value);
        builder.onWrite(*regs, method, value);
    };

    // Surface: A8R8G8B8 1280x720 at local 0x100000 with Z24S8, scissor covering all of it.
    write(0x0200, 1280u << 16);
    write(0x0204, 720u << 16);
    write(0x0208, 8u | (2u << 5) | (1u << 8));
    write(0x020C, 0x1400);
    write(0x0210, 0x100000);
    write(0x0214, 0x900000);
    write(0x022C, 0x1400);
    write(0x0220, 1);
    write(0x08C0, 1280u << 16);
    write(0x08C4, 720u << 16);

    // CLEAR_SURFACE 0xF3 gives a clear that keeps all bits and registers the color target and the depth surface.
    write(0x1D90, 0xFF102030);
    write(0x1D8C, 0xFFFFFF00);
    write(0x1D94, 0xF3);
    std::vector<tox2::RsxCommand> commands = builder.takeCommands();
    CHECK(commands.size() == 1 && std::holds_alternative<tox2::RsxClear>(commands[0]));
    const tox2::RsxClear& clear = std::get<tox2::RsxClear>(commands[0]);
    CHECK(clear.mask == 0xF3 && clear.color_argb == 0xFF102030 && clear.depth_stencil == 0xFFFFFF00);
    CHECK(clear.scissor[0] == 0 && clear.scissor[1] == 0 && clear.scissor[2] == 1280 && clear.scissor[3] == 720);
    CHECK(builder.knownSurfaces().size() == 2 && builder.knownSurfaces()[0].offset == 0x100000 &&
          builder.knownSurfaces()[0].depth_format == tox2::RsxDepthFormat::None);
    CHECK(builder.knownSurfaces()[1].offset == 0x900000 &&
          builder.knownSurfaces()[1].depth_format == tox2::RsxDepthFormat::Z24S8);
    CHECK(builder.takeCommands().empty());

    // A zero stencil write mask drops the stencil bit, a clear without color, depth or stencil bits does
    // nothing, and the rectangle is clipped to the framebuffer.
    write(0x032C, 0);
    write(0x1D94, 0xF3);
    write(0x1D94, 0x02);
    write(0x1D94, 0x0C);
    write(0x1D94, 0x00);
    write(0x032C, 0xFF);
    write(0x08C0, 1000u | (1000u << 16));
    write(0x08C4, 700u | (100u << 16));
    write(0x1D94, 0x10);
    commands = builder.takeCommands();
    CHECK(commands.size() == 2);
    CHECK(std::get<tox2::RsxClear>(commands[0]).mask == 0xF1);
    const tox2::RsxClear& clipped = std::get<tox2::RsxClear>(commands[1]);
    CHECK(clipped.mask == 0x10 && clipped.scissor[0] == 1000 && clipped.scissor[2] == 280);
    CHECK(clipped.scissor[1] == 700 && clipped.scissor[3] == 20);
    write(0x08C0, 1280u << 16);
    write(0x08C4, 720u << 16);

    // Draw state: slot 0 is three floats with stride 12 in main memory at 0x1000, eight vertices whose
    // bytes count up. The fragment program is one instruction with the end flag, in local memory.
    for (std::uint32_t i = 0; i < 96; i++) memory.main[0x1000 + i] = static_cast<std::uint8_t>(i);
    write(0x1740, 2u | (3u << 4) | (12u << 8));
    write(0x1680, 0x80001000);
    write(0x08E4, 0x2000 | 1);
    memory.putWord(0, 0x2000, 0x00010000);
    write(0x1D60, 0x40);
    write(0x0B40 + 4, 1);

    // A DRAW_INDEX_ARRAY of 4 u16 indices gives one draw with the index bytes and the vertices rebased
    // to the smallest index.
    const std::uint16_t indices[] = {1, 2, 3, 2};
    for (int i = 0; i < 4; i++) {
        memory.main[0x2000 + i * 2] = static_cast<std::uint8_t>(indices[i] >> 8);
        memory.main[0x2000 + i * 2 + 1] = static_cast<std::uint8_t>(indices[i]);
    }
    write(0x181C, 0x2000);
    write(0x1820, 1u | (1u << 4));
    write(0x1808, 5);
    write(0x1824, 3u << 24);
    write(0x1808, 0);
    commands = builder.takeCommands();
    CHECK(commands.size() == 1 && std::holds_alternative<tox2::RsxDraw>(commands[0]));
    const tox2::RsxDraw& indexed = std::get<tox2::RsxDraw>(commands[0]);
    CHECK(indexed.indexed && indexed.index_type == tox2::kRsxIndexTypeU16 &&
          indexed.primitive == tox2::RsxPrimitive::Triangles);
    CHECK(indexed.first == 0 && indexed.count == 4 && indexed.min_index == 1);
    CHECK((indexed.index_data == std::vector<std::uint8_t>{0, 1, 0, 2, 0, 3, 0, 2}));
    CHECK(indexed.attribs.size() == 1 && indexed.attribs[0].slot == 0 && indexed.attribs[0].location == 1);
    CHECK(indexed.attribs[0].data_offset == 0 && indexed.vertex_data.size() == 36);
    CHECK(indexed.vertex_data[0] == 12 && indexed.vertex_data[35] == 47);
    CHECK(indexed.surfaces.color.offset == 0x100000 && indexed.raster.cull == false);
    CHECK(indexed.fp_ucode.size() == 4);
    CHECK(indexed.fp_ucode[0] == 0x00010000 && indexed.fp_hash != 0 && indexed.fp_constants.empty());
    CHECK(indexed.fp_control == 0x40 && indexed.texcoord_control == 2);
    CHECK(indexed.vp_constants && indexed.vp_constants->size() == tox2::kRsxVpConstantCount * 4);
    CHECK(unimplementedCount("state unterminated vertex program") == 1);
    CHECK(indexed.attribs[0].has_data);

    // SET_VERTEX_DATA_BASE_INDEX rebases the vertex start of an indexed draw: (min_index + 2) * stride. A
    // slot of a type the builder cannot copy stays in the list without data.
    write(0x173C, 2);
    write(0x1744, 0u | (4u << 4) | (4u << 8));
    write(0x1684, 0x80001000);
    write(0x1808, 5);
    write(0x1824, 3u << 24);
    write(0x1808, 0);
    commands = builder.takeCommands();
    CHECK(commands.size() == 1);
    const tox2::RsxDraw& rebased = std::get<tox2::RsxDraw>(commands[0]);
    CHECK(rebased.min_index == 1 && rebased.vertex_data.size() == 36 && rebased.vertex_data[0] == (1 + 2) * 12);
    CHECK(rebased.attribs.size() == 2 && rebased.attribs[0].has_data && !rebased.attribs[1].has_data);
    write(0x173C, 0);
    write(0x1744, 0);

    // An array draw with a vertex base offset, merged contiguous ranges and a different primitive.
    write(0x1738, 0x10);
    write(0x1808, 6);
    write(0x1814, 2u | (1u << 24));
    write(0x1814, 4u | (1u << 24));
    write(0x1808, 0);
    commands = builder.takeCommands();
    CHECK(commands.size() == 1);
    const tox2::RsxDraw& array = std::get<tox2::RsxDraw>(commands[0]);
    CHECK(!array.indexed && array.primitive == tox2::RsxPrimitive::TriangleStrip);
    CHECK(array.first == 2 && array.count == 4 && array.min_index == 2 && array.index_data.empty());
    CHECK(array.vertex_data.size() == 3 * 12 + 12 && array.vertex_data[0] == 0x10 + 24);
    CHECK(unimplementedCount("state multiple ranges") == 0);

    // Ranges that do not touch keep the first one; an empty clause emits nothing; an index set of one
    // element draws nothing.
    write(0x1808, 5);
    write(0x1814, 0);
    write(0x1814, 5u | (2u << 24));
    write(0x1808, 0);
    write(0x1808, 5);
    write(0x1808, 0);
    commands = builder.takeCommands();
    CHECK(commands.size() == 1 && std::get<tox2::RsxDraw>(commands[0]).count == 1);
    CHECK(unimplementedCount("state multiple ranges") == 1);
    write(0x1824, 0);
    write(0x1808, 5);
    write(0x1824, 0);
    write(0x1808, 0);
    CHECK(builder.takeCommands().empty());

    // Texture unit 0 sits at the cleared surface and is an alias; unit 1 is a linear 4x4 texture whose
    // bytes are copied without the row padding; unit 2 is a swizzled 16x4 texture that is de-swizzled into
    // tightly packed rows. Disabled units are not decoded.
    for (std::uint32_t i = 0; i < 1024; i++) memory.local[0x3000 + i] = static_cast<std::uint8_t>(i ^ 0x5A);
    const std::uint32_t format = 1u | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16);
    write(0x1A00, 0x100000);
    write(0x1A04, format);
    write(0x1A0C, 0x80000000);
    write(0x1A18, (16u << 16) | 4u);
    write(0x1840, 64);
    write(0x1A00 + 0x20, 0x3000);
    write(0x1A04 + 0x20, format);
    write(0x1A0C + 0x20, 0x80000000);
    write(0x1A18 + 0x20, (16u << 16) | 4u);
    write(0x1840 + 4, 64);
    write(0x1A00 + 0x40, 0x3000);
    write(0x1A04 + 0x40, 1u | (2u << 4) | (0x85u << 8) | (1u << 16));
    write(0x1A0C + 0x40, 0x80000000);
    write(0x1A18 + 0x40, (16u << 16) | 4u);
    write(0x1A04 + 0x80, 0);
    write(0x1808, 5);
    write(0x1814, 2u << 24);
    write(0x1808, 0);
    commands = builder.takeCommands();
    CHECK(commands.size() == 1);
    const tox2::RsxDraw& textured = std::get<tox2::RsxDraw>(commands[0]);
    CHECK(textured.textures.size() == 3);
    CHECK(textured.textures[0].unit == 0 && textured.textures[0].rt_alias == 0 && texSize(textured.textures[0]) == 0);
    CHECK(textured.textures[1].unit == 1 && textured.textures[1].rt_alias == -1 &&
          texSize(textured.textures[1]) == 64 * 3 + 64);
    CHECK(texSize(textured.textures[2]) == 256 && texByte(textured.textures[1], 0) == (0 ^ 0x5A) &&
          texByte(textured.textures[1], 255) == (255 ^ 0x5A));
    // Texel (x, y) of the swizzled image comes from guest texel (x & 3 interleaved with y & 3) | (x >> 2) << 4:
    // (5, 1) is guest texel 19, (15, 3) is guest texel 63.
    const tox2::RsxTextureDesc& swizzled = textured.textures[2];
    CHECK(swizzled.unit == 2 && texSize(swizzled) == 256 && swizzled.pitch == 64);
    CHECK(texByte(swizzled, 0) == (0 ^ 0x5A));
    CHECK(texByte(swizzled, (1 * 16 + 5) * 4 + 2) == ((19 * 4 + 2) ^ 0x5A));
    CHECK(texByte(swizzled, (3 * 16 + 15) * 4 + 3) == ((63 * 4 + 3) ^ 0x5A));
    CHECK(unimplementedCount("texture layout swizzled") == 0 && unimplementedCount("texture layout mips 3") == 0);
    CHECK(unimplementedCount("state texture location 0x0") == 0);

    // Block-compressed textures copy whole block rows: unit 3 is a swizzled-flag DXT45 8x5 (2x2 blocks of 16
    // bytes, the pitch register is ignored), unit 4 a linear DXT1 6x4 with a padded pitch register (2x1 blocks
    // of 8 bytes, the padding is not copied), unit 5 a DXT1 8x8 with two mip levels (32 + 8 bytes).
    write(0x1A0C, 0);
    write(0x1A0C + 0x20, 0);
    write(0x1A0C + 0x40, 0);
    const auto dxtUnit = [&](std::uint32_t unit, std::uint32_t fmt, std::uint32_t mips, std::uint32_t width,
                             std::uint32_t height, std::uint32_t pitch) {
        write(0x1A00 + unit * 0x20, 0x3000);
        write(0x1A04 + unit * 0x20, 1u | (2u << 4) | (fmt << 8) | (mips << 16));
        write(0x1A0C + unit * 0x20, 0x80000000);
        write(0x1A18 + unit * 0x20, (width << 16) | height);
        write(0x1840 + unit * 4, pitch);
    };
    dxtUnit(3, 0x88, 1, 8, 5, 0);
    dxtUnit(4, 0x86 | 0x20, 1, 6, 4, 0x1000);
    dxtUnit(5, 0x86, 2, 8, 8, 0);
    write(0x1808, 5);
    write(0x1814, 2u << 24);
    write(0x1808, 0);
    commands = builder.takeCommands();
    CHECK(commands.size() == 1);
    const tox2::RsxDraw& compressed = std::get<tox2::RsxDraw>(commands[0]);
    CHECK(compressed.textures.size() == 3);
    CHECK(compressed.textures[0].unit == 3 && texSize(compressed.textures[0]) == 64 &&
          compressed.textures[0].pitch == 32);
    CHECK(compressed.textures[1].unit == 4 && texSize(compressed.textures[1]) == 16 &&
          compressed.textures[1].pitch == 16);
    CHECK(compressed.textures[2].unit == 5 && texSize(compressed.textures[2]) == 40);
    CHECK(texByte(compressed.textures[0], 0) == (0 ^ 0x5A) && texByte(compressed.textures[0], 63) == (63 ^ 0x5A));
    CHECK(texByte(compressed.textures[1], 0) == (0 ^ 0x5A) && texByte(compressed.textures[1], 15) == (15 ^ 0x5A));
    CHECK(texByte(compressed.textures[2], 39) == (39 ^ 0x5A));
    CHECK(unimplementedCount("texture layout swizzled") == 0 && unimplementedCount("texture layout mips 2") == 0);
    CHECK(unimplementedCount("texture format DXT45_SZ_NR") == 0);

    // Mip chains: unit 6 is a swizzled A8R8G8B8 8x4 with three levels (128 + 32 + 8 bytes, back to back, each
    // de-swizzled on its own); unit 7 a linear 8x4 with a padded 64-byte pitch and two levels, whose second
    // level keeps that pitch (guest rows at 256 and 320) and is copied without the padding. Unit 8 is a
    // swizzled 12x4 texture, which has no layout; unit 9 a linear texture whose pitch is below a row.
    tox2::rsxUnimplementedReset();
    const auto colorUnit = [&](std::uint32_t unit, std::uint32_t fmt, std::uint32_t mips, std::uint32_t width,
                               std::uint32_t height, std::uint32_t pitch) {
        write(0x1A00 + unit * 0x20, 0x3000);
        write(0x1A04 + unit * 0x20, 1u | (2u << 4) | (fmt << 8) | (mips << 16));
        write(0x1A0C + unit * 0x20, 0x80000000);
        write(0x1A18 + unit * 0x20, (width << 16) | height);
        write(0x1840 + unit * 4, pitch);
    };
    colorUnit(3, 0x85, 1, 4, 4, 0);  // reuse the units of the previous draw
    colorUnit(4, 0x85, 1, 4, 4, 0);
    colorUnit(5, 0x85, 1, 4, 4, 0);
    colorUnit(6, 0x85, 3, 8, 4, 0);
    colorUnit(7, 0xA5, 2, 8, 4, 64);
    colorUnit(8, 0x85, 1, 12, 4, 0);
    colorUnit(9, 0xA5, 1, 8, 4, 28);
    write(0x1808, 5);
    write(0x1814, 2u << 24);
    write(0x1808, 0);
    commands = builder.takeCommands();
    CHECK(commands.size() == 1);
    const tox2::RsxDraw& chains = std::get<tox2::RsxDraw>(commands[0]);
    const auto unitOf = [&](std::uint32_t unit) -> const tox2::RsxTextureDesc& {
        for (const tox2::RsxTextureDesc& texture : chains.textures) {
            if (texture.unit == unit) return texture;
        }
        return chains.textures[0];
    };
    const tox2::RsxTextureDesc& swizzled_chain = unitOf(6);
    CHECK(texSize(swizzled_chain) == 168 && swizzled_chain.pitch == 32);
    const auto guestByte = [](std::uint32_t i) { return static_cast<std::uint8_t>(i ^ 0x5A); };
    // Level 0 texel (3, 2) is guest texel 13 (x bits 0 and 1 interleave with y bits 0 and 1).
    CHECK(tox2::rsxSwizzleIndex(3, 2, 3, 2) == 13);
    CHECK(texByte(swizzled_chain, (2 * 8 + 3) * 4 + 1) == guestByte(13 * 4 + 1));
    // Level 1 is 4x2 from guest byte 128: texel (3, 1) is guest texel 7 of that level.
    CHECK(tox2::rsxSwizzleIndex(3, 1, 2, 1) == 7);
    CHECK(texByte(swizzled_chain, 128 + (1 * 4 + 3) * 4) == guestByte(128 + 7 * 4));
    // Level 2 is 2x1 from guest byte 160, a plain row.
    CHECK(texByte(swizzled_chain, 160 + 4) == guestByte(160 + 4));
    const tox2::RsxTextureDesc& linear_chain = unitOf(7);
    CHECK(texSize(linear_chain) == 128 + 32 && linear_chain.pitch == 32);
    CHECK(texByte(linear_chain, 32) == guestByte(64));         // row 1 of level 0
    CHECK(texByte(linear_chain, 128) == guestByte(256));       // row 0 of level 1
    CHECK(texByte(linear_chain, 128 + 16) == guestByte(320));  // row 1 of level 1
    CHECK(texSize(unitOf(8)) == 0 && texSize(unitOf(9)) == 0);
    CHECK(unimplementedCount("texture layout swizzled non-power-of-two") == 1);
    CHECK(unimplementedCount("texture layout pitch below row size") == 1);
    CHECK(unimplementedCount("texture layout swizzled") == 0 && unimplementedCount("texture layout mips 3") == 0);

    // 3D and cube textures: unit 3 is a linear A8R8G8B8 3D texture of 4x2 texels and 3 slices (24-byte
    // pitch, depth in the top bits of the pitch register); unit 4 a swizzled A8R8G8B8 cube map of 4x4 texels
    // (6 faces of 64 bytes, 128 bytes apart in guest memory); unit 5 a DXT1 3D texture of 8x8 texels and 5
    // slices in the VTC order. Unit 6 is 1D, which has no copy. Unit 7 is a swizzled 3D texture with 3 slices,
    // which has no layout.
    tox2::rsxUnimplementedReset();
    for (std::uint32_t unit = 8; unit < 10; unit++) write(0x1A0C + unit * 0x20, 0);
    const auto shapeUnit = [&](std::uint32_t unit, std::uint32_t fmt, std::uint32_t dimension, std::uint32_t cube,
                               std::uint32_t width, std::uint32_t height, std::uint32_t depth, std::uint32_t pitch) {
        write(0x1A00 + unit * 0x20, 0x3000);
        write(0x1A04 + unit * 0x20, 1u | (cube << 2) | (dimension << 4) | (fmt << 8) | (1u << 16));
        write(0x1A0C + unit * 0x20, 0x80000000);
        write(0x1A18 + unit * 0x20, (width << 16) | height);
        write(0x1840 + unit * 4, (depth << 20) | pitch);
    };
    shapeUnit(3, 0xA5, 3, 0, 4, 2, 3, 24);
    shapeUnit(4, 0x85, 2, 1, 4, 4, 0, 0);
    shapeUnit(5, 0x86, 3, 0, 8, 8, 5, 0);
    shapeUnit(6, 0x85, 1, 0, 4, 4, 0, 0);
    shapeUnit(7, 0x85, 3, 0, 4, 4, 3, 0);
    write(0x1808, 5);
    write(0x1814, 2u << 24);
    write(0x1808, 0);
    commands = builder.takeCommands();
    CHECK(commands.size() == 1);
    const tox2::RsxDraw& shapes = std::get<tox2::RsxDraw>(commands[0]);
    const auto shapeOf = [&](std::uint32_t unit) -> const tox2::RsxTextureDesc& {
        for (const tox2::RsxTextureDesc& texture : shapes.textures) {
            if (texture.unit == unit) return texture;
        }
        return shapes.textures[0];
    };
    const tox2::RsxTextureDesc& volume_texture = shapeOf(3);
    CHECK(volume_texture.depth == 3 && texSize(volume_texture) == 96 && volume_texture.pitch == 16);
    CHECK(texByte(volume_texture, 16) == guestByte(24));   // slice 0, row 1
    CHECK(texByte(volume_texture, 32) == guestByte(48));   // slice 1, row 0
    CHECK(texByte(volume_texture, 80) == guestByte(120));  // slice 2, row 1
    const tox2::RsxTextureDesc& cube_texture = shapeOf(4);
    CHECK(cube_texture.cubemap && texSize(cube_texture) == 6 * 64);
    CHECK(texByte(cube_texture, 64) == guestByte(128));                  // face 1, texel (0, 0)
    CHECK(texByte(cube_texture, 2 * 64 + 4) == guestByte(256 + 1 * 4));  // face 2, texel (1, 0)
    const tox2::RsxTextureDesc& vtc_texture = shapeOf(5);
    CHECK(vtc_texture.depth == 5 && texSize(vtc_texture) == 5 * 4 * 8);
    // Slice 1 block 2 is guest block 2 * 4 + 1; slice 4, the last group of one slice, block 2 is guest block 16 + 2.
    CHECK(texByte(vtc_texture, (1 * 4 + 2) * 8) == guestByte((2 * 4 + 1) * 8));
    CHECK(texByte(vtc_texture, (4 * 4 + 2) * 8) == guestByte((16 + 2) * 8));
    CHECK(texSize(shapeOf(6)) == 0 && texSize(shapeOf(7)) == 0);
    CHECK(unimplementedCount("texture layout dimension 1") == 1);
    CHECK(unimplementedCount("texture layout swizzled non-power-of-two") == 1);
    CHECK(unimplementedCount("texture layout cubemap") == 0 && unimplementedCount("texture layout dimension 3") == 0);

    // The fragment program with an inline constant keeps its place in the ucode but zeroed; programs
    // that differ only in the constant hash equal.
    const auto fragmentProgram = [&](std::uint32_t constant) {
        memory.putWord(0, 0x2000, 0);
        memory.putWord(0, 0x2004, 0x00020000);  // instruction 0 reads a constant (source type 2)
        memory.putWord(0, 0x2008, 0);
        memory.putWord(0, 0x200C, 0);
        memory.putWord(0, 0x2010, constant);
        memory.putWord(0, 0x2020, 0x00010000);  // instruction 1, end flag
        write(0x1808, 5);
        write(0x1814, 2u << 24);
        write(0x1808, 0);
        return std::get<tox2::RsxDraw>(builder.takeCommands().at(0));
    };
    const tox2::RsxDraw first_program = fragmentProgram(0x3F800000);
    const tox2::RsxDraw second_program = fragmentProgram(0x40000000);
    CHECK(first_program.fp_ucode.size() == 12 && first_program.fp_constants.size() == 4);
    CHECK(first_program.fp_constants[0] == 0x3F800000 && first_program.fp_ucode[4] == 0 &&
          first_program.fp_ucode[8] == 0x00010000);
    CHECK(second_program.fp_constants[0] == 0x40000000 && first_program.fp_hash == second_program.fp_hash);

    // Flips carry the display buffer; ignored methods are counted.
    write(0x1818, 0x12345678);
    tox2::RsxDisplayBuffer display;
    display.offset = 0x400000;
    display.pitch = 0x1400;
    display.width = 1280;
    display.height = 720;
    builder.onFlip(*regs, 1, display);
    builder.onFlip(*regs, 2, display);
    commands = builder.takeCommands();
    CHECK(commands.size() == 2 && std::get<tox2::RsxFlip>(commands[0]).flip_index == 0);
    CHECK(std::get<tox2::RsxFlip>(commands[1]).flip_index == 1 && std::get<tox2::RsxFlip>(commands[1]).buffer == 2);
    CHECK(std::get<tox2::RsxFlip>(commands[1]).offset == 0x400000 &&
          std::get<tox2::RsxFlip>(commands[1]).height == 720);
    CHECK(unimplementedCount("method NV4097_INLINE_ARRAY") == 1);
    CHECK(std::get<tox2::RsxFlip>(commands[0]).completed_us == 0);  // no time passed: replays leave 0

    // The completion time given by the RSX thread rides on the flip command.
    builder.onFlip(*regs, 1, display, 1000);
    builder.onFlip(*regs, 2, display, 17667);
    commands = builder.takeCommands();
    CHECK(commands.size() == 2 && std::get<tox2::RsxFlip>(commands[0]).completed_us == 1000 &&
          std::get<tox2::RsxFlip>(commands[1]).completed_us == 17667);

    // Unreadable vertex memory drops the draw and counts it.
    write(0x1680, 0x80000000 | 0x20000);
    write(0x1808, 5);
    write(0x1814, 2u << 24);
    write(0x1808, 0);
    CHECK(builder.takeCommands().empty() && unimplementedCount("state unreadable vertex memory") == 1);
    tox2::rsxUnimplementedReset();
}

// Registers and builder with a render target, one vertex attribute and a fragment program pointer, enough to build
// a draw from whatever the shared memory holds at the program address.
struct FragmentRig {
    explicit FragmentRig(tox2::RsxMemoryView& memory) : regs(std::make_unique<tox2::RsxRegisters>()), builder(memory) {
        regs->reset();
        write(0x0200, 1280u << 16);
        write(0x0204, 720u << 16);
        write(0x0208, 8u | (2u << 5) | (1u << 8));
        write(0x020C, 0x1400);
        write(0x0210, 0x100000);
        write(0x0214, 0x900000);
        write(0x022C, 0x1400);
        write(0x0220, 1);
        write(0x08C0, 1280u << 16);
        write(0x08C4, 720u << 16);
        write(0x1740, 2u | (3u << 4) | (12u << 8));
        write(0x1680, 0x80001000);
    }
    void write(std::uint32_t method, std::uint32_t value) {
        regs->apply(method, value);
        builder.onWrite(*regs, method, value);
    }
    tox2::RsxDraw draw(std::uint32_t program_offset) {
        write(0x08E4, program_offset | 1);
        write(0x1808, 5);
        write(0x1814, 2u << 24);
        write(0x1808, 0);
        return std::get<tox2::RsxDraw>(builder.takeCommands().at(0));
    }
    std::unique_ptr<tox2::RsxRegisters> regs;
    tox2::RsxFrameBuilder builder;
};

// Reads of the fragment program area in local memory (vertex reads are not counted).
std::size_t programReads(const FakeRsxMemory& memory) {
    std::size_t total = 0;
    for (const auto& [where, count] : memory.reads) {
        if (where.first == 0 && where.second >= 0x2000 && where.second < 0x3000)
            total += static_cast<std::size_t>(count);
    }
    return total;
}

std::uint64_t hashBigEndian(const std::vector<std::uint32_t>& words) {
    std::vector<std::uint8_t> bytes;
    for (const std::uint32_t word : words) {
        for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<std::uint8_t>(word >> shift));
    }
    return tox2::rsxHash64(bytes.data(), bytes.size());
}

void testFragmentProgramCache() {
    tox2::rsxUnimplementedReset();
    FakeRsxMemory memory;
    for (std::uint32_t i = 0; i < 96; i++) memory.main[0x1000 + i] = static_cast<std::uint8_t>(i);
    FragmentRig rig(memory);

    // Instruction 0 reads an inline constant (source type 2), instruction 1 has the end flag.
    const auto putProgram = [&](std::uint32_t constant) {
        memory.putWord(0, 0x2000, 0);
        memory.putWord(0, 0x2004, 0x00020000);
        memory.putWord(0, 0x2008, 0);
        memory.putWord(0, 0x200C, 0);
        for (std::uint32_t w = 0; w < 4; w++) memory.putWord(0, 0x2010 + w * 4, constant + w);
        memory.putWord(0, 0x2020, 0x00010000);
        memory.putWord(0, 0x2024, 0);
        memory.putWord(0, 0x2028, 0);
        memory.putWord(0, 0x202C, 0);
    };
    putProgram(0x3F800000);
    const tox2::RsxDraw first = rig.draw(0x2000);
    CHECK(first.fp_ucode.size() == 12 && first.fp_constants.size() == 4);
    CHECK(rig.builder.stats().fp_cache_hits == 0 && rig.builder.stats().fp_cache_misses == 1);
    CHECK(first.fp_hash == hashBigEndian(first.fp_ucode));

    // The same program with another constant: one read for the whole program, a hit, the new constants.
    putProgram(0x40000000);
    const std::size_t reads_before = programReads(memory);
    const tox2::RsxDraw second = rig.draw(0x2000);
    CHECK(programReads(memory) - reads_before == 1 && memory.readsAt(0, 0x2000) == 2);
    CHECK(rig.builder.stats().fp_cache_hits == 1 && rig.builder.stats().fp_cache_misses == 1);
    CHECK(second.fp_hash == first.fp_hash && second.fp_ucode == first.fp_ucode);
    CHECK(second.fp_constants == std::vector<std::uint32_t>({0x40000000, 0x40000001, 0x40000002, 0x40000003}));
    CHECK(second.fp_hash == hashBigEndian(second.fp_ucode));

    // An instruction word rewritten at the same address misses and gives what a fresh builder gives.
    const auto sameAsFresh = [&](const tox2::RsxDraw& got) {
        FragmentRig fresh(memory);
        const tox2::RsxDraw expected = fresh.draw(0x2000);
        return got.fp_ucode == expected.fp_ucode && got.fp_constants == expected.fp_constants &&
               got.fp_hash == expected.fp_hash;
    };
    memory.putWord(0, 0x2008, 0x1234);
    const tox2::RsxDraw rewritten = rig.draw(0x2000);
    CHECK(rig.builder.stats().fp_cache_hits == 1 && rig.builder.stats().fp_cache_misses == 2);
    CHECK(rewritten.fp_ucode.size() == 12 && rewritten.fp_ucode[2] == 0x1234 && rewritten.fp_hash != first.fp_hash);
    CHECK(sameAsFresh(rewritten));
    CHECK(rewritten.fp_hash == hashBigEndian(rewritten.fp_ucode));

    // A shorter program at the same address (end flag in instruction 0) misses; so does the longer one again.
    memory.putWord(0, 0x2000, 0x00010000);
    memory.putWord(0, 0x2004, 0);
    const tox2::RsxDraw shorter = rig.draw(0x2000);
    CHECK(rig.builder.stats().fp_cache_misses == 3);
    CHECK(shorter.fp_ucode.size() == 4 && shorter.fp_constants.empty() && sameAsFresh(shorter));
    memory.putWord(0, 0x2000, 0);
    memory.putWord(0, 0x2004, 0x00020000);
    const tox2::RsxDraw longer = rig.draw(0x2000);
    CHECK(rig.builder.stats().fp_cache_misses == 4);
    CHECK(longer.fp_ucode == rewritten.fp_ucode && sameAsFresh(longer));
    CHECK(rig.builder.stats().fp_cache_hits == 1);

    // Many distinct programs flush the cache without changing results; the newest entry is still cached.
    for (std::uint32_t i = 0; i < 1100; i++) {
        const std::uint32_t at = 0x3000 + i * 16;
        memory.putWord(0, at, 0x00010000 + (i << 1));
        const tox2::RsxDraw many = rig.draw(at);
        CHECK(many.fp_ucode.size() == 4 && many.fp_ucode[0] == 0x00010000 + (i << 1));
    }
    const std::uint64_t hits = rig.builder.stats().fp_cache_hits;
    rig.draw(0x3000 + 1099 * 16);
    CHECK(rig.builder.stats().fp_cache_hits == hits + 1);

    // The replay view's speculative read fails quietly; read() still counts the miss.
    tox2::rsxUnimplementedReset();
    tox2::ReplayRsxMemoryView view;
    std::uint8_t probe[8] = {};
    CHECK(!view.tryRead(0, 0x4000, probe) && unimplementedCount("capture missing range") == 0);
    CHECK(!view.read(0, 0x4000, probe) && unimplementedCount("capture missing range") == 1);
    const std::uint8_t stored[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    view.add(0, 0x4000, stored);
    CHECK(view.tryRead(0, 0x4000, probe) && std::memcmp(probe, stored, 8) == 0);
    CHECK(!view.tryRead(0, 0x4004, probe) && !view.tryRead(1, 0x4000, probe) && !view.tryRead(2, 0, probe));
    CHECK(unimplementedCount("capture missing range") == 1);
    tox2::rsxUnimplementedReset();
}

// Loads `words` (4 per instruction) at instruction `at` and starts the vertex program there.
void uploadVertexProgram(FragmentRig& rig, std::uint32_t at, const std::vector<std::uint32_t>& words) {
    rig.write(tox2::kMethodNv4097SetTransformProgramLoad, at);
    for (std::size_t w = 0; w < words.size(); w++) {
        rig.write(tox2::kMethodNv4097SetTransformProgram + static_cast<std::uint32_t>(w % 4) * 4, words[w]);
    }
    rig.write(tox2::kMethodNv4097SetTransformProgramStart, at);
}

void testVertexProgramCache() {
    tox2::rsxUnimplementedReset();
    FakeRsxMemory memory;
    for (std::uint32_t i = 0; i < 96; i++) memory.main[0x1000 + i] = static_cast<std::uint8_t>(i);
    memory.putWord(0, 0x2000, 0x00010000);
    FragmentRig rig(memory);

    // What a builder with no history makes of the same program.
    const auto fresh = [&](std::uint32_t at, const std::vector<std::uint32_t>& words) {
        FragmentRig other(memory);
        uploadVertexProgram(other, at, words);
        return other.draw(0x2000);
    };
    const auto sameProgram = [](const tox2::RsxDraw& a, const tox2::RsxDraw& b) {
        return a.vp_ucode == b.vp_ucode && a.vp_hash == b.vp_hash;
    };
    const auto& stats = rig.builder.stats();

    // Two instructions, the second with the end bit. Words 2 are payload that does not steer the flow.
    std::vector<std::uint32_t> program = {0, 0, 0x4000, 0, 0, 0, 0x8000, 1};
    uploadVertexProgram(rig, 4, program);
    const tox2::RsxDraw first = rig.draw(0x2000);
    CHECK(first.vp_ucode == program && first.vp_hash != 0);
    CHECK(stats.vp_cache_hits == 0 && stats.vp_cache_misses == 1);
    CHECK(sameProgram(first, fresh(4, program)));

    // The same store again: a hit under the same name.
    const tox2::RsxDraw second = rig.draw(0x2000);
    CHECK(stats.vp_cache_hits == 1 && stats.vp_cache_misses == 1 && sameProgram(second, first));

    // The same words uploaded again carry a new name and hit through the comparison.
    const std::uint64_t name = rig.regs->vpUcodeGeneration();
    uploadVertexProgram(rig, 4, program);
    CHECK(rig.regs->vpUcodeGeneration() != name);
    const tox2::RsxDraw reuploaded = rig.draw(0x2000);
    CHECK(stats.vp_cache_hits == 2 && stats.vp_cache_misses == 1 && sameProgram(reuploaded, first));

    // A word past the last reached instruction is not part of the program: still a hit.
    uploadVertexProgram(rig, 6, {0xDEAD0001, 0, 0x4000, 0});
    rig.write(tox2::kMethodNv4097SetTransformProgramStart, 4);
    CHECK(sameProgram(rig.draw(0x2000), first) && stats.vp_cache_hits == 3 && stats.vp_cache_misses == 1);

    // A changed reached word misses and gives what a fresh builder gives.
    program[2] = 0x4001;
    uploadVertexProgram(rig, 4, program);
    const tox2::RsxDraw changed = rig.draw(0x2000);
    CHECK(stats.vp_cache_hits == 3 && stats.vp_cache_misses == 2);
    CHECK(changed.vp_ucode == program && changed.vp_hash != first.vp_hash && sameProgram(changed, fresh(4, program)));

    // The same words at another start are another entry with the same hash, and both stay cached.
    uploadVertexProgram(rig, 12, program);
    const tox2::RsxDraw moved = rig.draw(0x2000);
    CHECK(stats.vp_cache_misses == 3 && moved.vp_hash == changed.vp_hash && moved.vp_ucode == changed.vp_ucode);
    rig.write(tox2::kMethodNv4097SetTransformProgramStart, 4);
    CHECK(sameProgram(rig.draw(0x2000), changed) && stats.vp_cache_hits == 4);
    rig.write(tox2::kMethodNv4097SetTransformProgramStart, 12);
    CHECK(sameProgram(rig.draw(0x2000), moved) && stats.vp_cache_hits == 5 && stats.vp_cache_misses == 3);

    // A branchy program (as in the synthetic capture test): a hit gives the rebased words again.
    const std::vector<std::uint32_t> branchy = {
            (7u << 10), 0x11u << 27, 2, (1u << 23) | (7u << 29),  // BRB, bit 1, taken when clear, target 23 = start + 3
            0,          0,           0, 1,                        // end
            0xDEAD0001, 0x0F000000,  0, 0,                        // stale, never reached
            0,          0,           0, 1,                        // end
    };
    uploadVertexProgram(rig, 20, branchy);
    const tox2::RsxDraw branchy_first = rig.draw(0x2000);
    const tox2::RsxDraw branchy_second = rig.draw(0x2000);
    CHECK(branchy_first.vp_ucode.size() == 16 && branchy_first.vp_ucode[3] == ((1u << 23) | (3u << 29)));
    CHECK(branchy_first.vp_ucode[8] == 0 && branchy_first.vp_ucode[9] == 0 && branchy_first.vp_ucode[15] == 1);
    CHECK(stats.vp_cache_misses == 4 && stats.vp_cache_hits == 6 && sameProgram(branchy_second, branchy_first));
    CHECK(sameProgram(branchy_first, fresh(20, branchy)));

    // Programs that run off the end of the store, or branch out of themselves, repeat their notes on a hit.
    tox2::rsxUnimplementedReset();
    const std::vector<std::uint32_t> open_ended = {0, 0, 0x4000, 0};
    uploadVertexProgram(rig, tox2::kRsxVpInstructionCount - 1, open_ended);
    const std::uint64_t misses = stats.vp_cache_misses, hits = stats.vp_cache_hits;
    const tox2::RsxDraw unterminated = rig.draw(0x2000);
    CHECK(unimplementedCount("state unterminated vertex program") == 1 && stats.vp_cache_misses == misses + 1);
    const tox2::RsxDraw unterminated_again = rig.draw(0x2000);
    CHECK(unimplementedCount("state unterminated vertex program") == 2 && stats.vp_cache_hits == hits + 1);
    CHECK(unterminated.vp_ucode == open_ended && sameProgram(unterminated_again, unterminated));
    const std::vector<std::uint32_t> outside = {0, 0x11u << 27, 0, 1};  // BRB with target 0, before the entry
    uploadVertexProgram(rig, 30, outside);
    const tox2::RsxDraw bad = rig.draw(0x2000);
    CHECK(bad.vp_ucode.empty() && bad.vp_hash == 0);
    CHECK(unimplementedCount("state vertex program branch outside the program") == 1);
    const tox2::RsxDraw bad_again = rig.draw(0x2000);
    CHECK(bad_again.vp_ucode.empty() && bad_again.vp_hash == 0 && stats.vp_cache_hits == hits + 2);
    CHECK(unimplementedCount("state vertex program branch outside the program") == 2);
    CHECK(unimplementedCount("state unterminated vertex program") == 2);

    // More programs than the cache holds: results stay right and the newest is cached.
    for (std::uint32_t i = 0; i < 40; i++) {
        const std::vector<std::uint32_t> word = {0, 0, 0x4000 + i * 64, 1};
        uploadVertexProgram(rig, 100 + i, word);
        const tox2::RsxDraw many = rig.draw(0x2000);
        CHECK(many.vp_ucode == word);
    }
    const std::uint64_t before = stats.vp_cache_hits;
    rig.draw(0x2000);
    CHECK(stats.vp_cache_hits == before + 1);
    tox2::rsxUnimplementedReset();
}

void testSharedVertexConstants() {
    tox2::rsxUnimplementedReset();
    FakeRsxMemory memory;
    memory.putWord(0, 0x2000, 0x00010000);
    FragmentRig rig(memory);
    const auto writeConstant = [&](std::uint32_t slot, std::uint32_t value) {
        rig.write(tox2::kMethodNv4097SetTransformConstantLoad, slot);
        rig.write(tox2::kMethodNv4097SetTransformConstant, value);
    };
    constexpr std::size_t kWords = tox2::kRsxVpConstantCount * 4;

    // No constant write between two draws: one snapshot, one id.
    writeConstant(2, 0x111);
    tox2::RsxDraw first = rig.draw(0x2000);
    tox2::RsxDraw second = rig.draw(0x2000);
    CHECK(first.vp_constants && first.vp_constants->size() == kWords && first.vp_constants_id != 0);
    CHECK(first.vp_constants == second.vp_constants && first.vp_constants_id == second.vp_constants_id);
    CHECK((*first.vp_constants)[8] == 0x111);

    // A write gives the next draw a new id and snapshot; the earlier snapshots keep their values.
    writeConstant(2, 0x222);
    tox2::RsxDraw third = rig.draw(0x2000);
    CHECK(third.vp_constants_id != first.vp_constants_id && third.vp_constants != first.vp_constants);
    CHECK((*third.vp_constants)[8] == 0x222 && (*first.vp_constants)[8] == 0x111 && (*second.vp_constants)[8] == 0x111);

    // Equal ids mean equal contents even for another builder fed the same registers.
    FragmentRig other(memory);
    other.regs->vp_constants = rig.regs->vp_constants;
    other.regs->vp_constants_generation = rig.regs->vpConstantsGeneration();
    CHECK(other.draw(0x2000).vp_constants_id == third.vp_constants_id);

    // A snapshot no draw holds any more is rewritten in place; one a draw still holds is left alone.
    const auto* held = third.vp_constants.get();
    third = tox2::RsxDraw{};
    writeConstant(2, 0x333);
    tox2::RsxDraw fourth = rig.draw(0x2000);
    CHECK(fourth.vp_constants.get() == held && (*fourth.vp_constants)[8] == 0x333);
    writeConstant(2, 0x444);
    tox2::RsxDraw fifth = rig.draw(0x2000);
    CHECK(fifth.vp_constants != fourth.vp_constants && (*fifth.vp_constants)[8] == 0x444);
    CHECK((*fourth.vp_constants)[8] == 0x333);

    // Writes past the end of the store change neither the name nor the snapshot.
    rig.write(tox2::kMethodNv4097SetTransformConstantLoad, tox2::kRsxVpConstantCount);
    rig.write(tox2::kMethodNv4097SetTransformConstant, 0xBEEF);
    tox2::RsxDraw sixth = rig.draw(0x2000);
    CHECK(sixth.vp_constants == fifth.vp_constants && sixth.vp_constants_id == fifth.vp_constants_id);
    tox2::rsxUnimplementedReset();
}

// The snapshot a draw carries always equals the register store, whatever mix of constant writes, draws that still hold
// their snapshot (a new vector) or released it (rewritten in place, only the changed words), direct stores and resets
// came before it.
void testVertexConstantSnapshotMatchesRegisters() {
    tox2::rsxUnimplementedReset();
    FakeRsxMemory memory;
    memory.putWord(0, 0x2000, 0x00010000);
    FragmentRig rig(memory);
    const auto configure = [&] {
        rig.write(0x0200, 1280u << 16);
        rig.write(0x0204, 720u << 16);
        rig.write(0x0208, 8u | (2u << 5) | (1u << 8));
        rig.write(0x020C, 0x1400);
        rig.write(0x0210, 0x100000);
        rig.write(0x0214, 0x900000);
        rig.write(0x022C, 0x1400);
        rig.write(0x0220, 1);
        rig.write(0x08C0, 1280u << 16);
        rig.write(0x08C4, 720u << 16);
        rig.write(0x1740, 2u | (3u << 4) | (12u << 8));
        rig.write(0x1680, 0x80001000);
    };
    std::uint32_t state = 0x9E3779B9u;  // xorshift32, fixed seed
    const auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    const auto matches = [&](const tox2::RsxDraw& draw) {
        return draw.vp_constants && draw.vp_constants->size() == rig.regs->vp_constants.size() &&
               std::equal(draw.vp_constants->begin(), draw.vp_constants->end(), rig.regs->vp_constants.begin());
    };
    const auto writeConstants = [&](std::uint32_t load, std::uint32_t first, std::uint32_t count) {
        rig.write(tox2::kMethodNv4097SetTransformConstantLoad, load);
        for (std::uint32_t i = 0; i < count; i++) {
            rig.write(tox2::kMethodNv4097SetTransformConstant + (first + i) * 4, next());
        }
    };

    // Writes before the first draw, spread over the store and past its end.
    writeConstants(0, 0, 1);
    writeConstants(tox2::kRsxVpConstantCount - 2, 0, 12);
    writeConstants(100, 3, 5);
    CHECK(matches(rig.draw(0x2000)));

    struct Held {
        tox2::RsxDraw draw;
        std::vector<std::uint32_t> expected;
    };
    std::vector<Held> held;
    const auto releaseHeld = [&] {
        for (const Held& entry : held) CHECK(*entry.draw.vp_constants == entry.expected);
        held.clear();
    };
    for (std::uint32_t step = 0; step < 400; step++) {
        const std::uint32_t action = next() % 16;
        if (action < 9) {
            writeConstants(next() % (tox2::kRsxVpConstantCount + 3), next() % 24, 1 + next() % 8);
        } else if (action < 13) {
            tox2::RsxDraw draw = rig.draw(0x2000);
            CHECK(matches(draw));
            // The next change must not touch a snapshot a draw still holds.
            if (next() % 3 == 0)
                held.push_back({std::move(draw), {rig.regs->vp_constants.begin(), rig.regs->vp_constants.end()}});
        } else if (action == 13) {
            // A direct store followed by markVpStoresChanged, as the capture loader does.
            for (std::uint32_t& word : rig.regs->vp_constants) word = next();
            rig.regs->markVpStoresChanged();
        } else if (action == 14) {
            rig.regs->reset();
            configure();
            writeConstants(next() % tox2::kRsxVpConstantCount, 0, 4);
        } else {
            releaseHeld();
        }
    }
    releaseHeld();
    tox2::rsxUnimplementedReset();
}

// fillVertices sizes the vertex vector once: slot placement, the bytes copied, the notes for each failure order and
// the copied and union byte counters.
void testFillVerticesOnePass() {
    tox2::rsxUnimplementedReset();
    FakeRsxMemory memory;
    memory.putWord(0, 0x2000, 0x00010000);
    for (std::uint32_t i = 0; i < 64; i++) {
        memory.main[0x1000 + i] = static_cast<std::uint8_t>(i + 1);
        memory.main[0x2000 + i] = static_cast<std::uint8_t>(0x80 + i);
        memory.local[0x4000 + i] = static_cast<std::uint8_t>(0xC0 + i);
    }
    const auto setAttrib = [](FragmentRig& rig, std::uint32_t slot, std::uint32_t type, std::uint32_t size,
                              std::uint32_t stride, std::uint32_t location_offset) {
        rig.write(0x1740 + slot * 4, type | (size << 4) | (stride << 8));
        rig.write(0x1680 + slot * 4, location_offset);
    };

    // Three vertices, three attributes: 36 bytes (f x3, stride 12), 10 bytes (ub x2, stride 4, from the other
    // location) and 3 bytes (ub x1, stride 1, local memory). Slots are 4-byte aligned, so the third starts at 48.
    {
        FragmentRig rig(memory);
        setAttrib(rig, 1, 4, 2, 4, 0x80002000);
        setAttrib(rig, 2, 4, 1, 1, 0x00004000);
        const tox2::RsxDraw draw = rig.draw(0x2000);
        const std::vector<std::uint8_t> expected = {
                0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11,
                0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22,
                0x23, 0x24, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x00, 0x00, 0xC0, 0xC1, 0xC2};
        CHECK(draw.vertex_data == expected);
        CHECK(draw.attribs.size() == 3 && draw.attribs[0].data_offset == 0 && draw.attribs[1].data_offset == 36 &&
              draw.attribs[2].data_offset == 48);
        CHECK(draw.attribs[0].has_data && draw.attribs[1].has_data && draw.attribs[2].has_data);

        // The union is counted only while timing; the copied figure always.
        const auto& stats = rig.builder.stats();
        CHECK(stats.vertex_bytes_copied == 49 && stats.vertex_bytes_union == 0);
        rig.builder.setTimingEnabled(true);
        rig.draw(0x2000);
        CHECK(stats.vertex_bytes_copied == 98 && stats.vertex_bytes_union == 49);
    }

    // Two attributes of one interleaved array (stride 16, colour at +12) read overlapping ranges of 44 and 36 bytes;
    // the 48 distinct bytes are copied once and the colour points 12 bytes into them.
    {
        FragmentRig rig(memory);
        rig.builder.setTimingEnabled(true);
        setAttrib(rig, 0, 2, 3, 16, 0x80001000);
        setAttrib(rig, 1, 4, 4, 16, 0x8000100C);
        const tox2::RsxDraw draw = rig.draw(0x2000);
        CHECK(draw.vertex_data.size() == 48 && draw.attribs[1].data_offset == 12);
        CHECK(draw.vertex_data[0] == 0x01 && draw.vertex_data[12] == 0x0D && draw.vertex_data[47] == 0x30);
        CHECK(rig.builder.stats().vertex_bytes_copied == 48 && rig.builder.stats().vertex_bytes_union == 48);
    }

    // Failure orders. Two indices (0 and 0x7FFFF) make 512 K vertices, so a stride-255 array is 133 MB: too large.
    // Slot 0 has stride 0 and shares one element, so it is small whatever the range.
    const auto indexedDraw = [&](FragmentRig& rig) {
        memory.putWord(0, 0x5000, 0);
        memory.putWord(0, 0x5004, 0x7FFFF);
        rig.write(0x08E4, 0x2000 | 1);
        rig.write(0x1808, 5);
        rig.write(0x181C, 0x5000);
        rig.write(0x1820, 0);
        rig.write(0x1824, 1u << 24);
        rig.write(0x1808, 0);
        return rig.builder.takeCommands();
    };
    const auto notes = [](const char* name) { return unimplementedCount(name); };
    {
        // The first attribute is unreadable and the second too large: the unreadable note, once.
        tox2::rsxUnimplementedReset();
        FragmentRig rig(memory);
        setAttrib(rig, 0, 2, 3, 0, 0x80020000);
        setAttrib(rig, 1, 4, 1, 255, 0x80001000);
        CHECK(indexedDraw(rig).empty());
        CHECK(notes("state unreadable vertex memory") == 1 && notes("state vertex data too large") == 0);
    }
    {
        // The first attribute is too large and the second unreadable: the size note, once.
        tox2::rsxUnimplementedReset();
        FragmentRig rig(memory);
        setAttrib(rig, 0, 4, 1, 255, 0x80001000);
        setAttrib(rig, 1, 2, 3, 0, 0x80020000);
        CHECK(indexedDraw(rig).empty());
        CHECK(notes("state vertex data too large") == 1 && notes("state unreadable vertex memory") == 0);
    }
    {
        // A readable first attribute and a too large second: the size note, and the first was read.
        tox2::rsxUnimplementedReset();
        FragmentRig rig(memory);
        rig.builder.setTimingEnabled(true);
        setAttrib(rig, 0, 2, 3, 0, 0x80001000);
        setAttrib(rig, 1, 4, 1, 255, 0x80001000);
        CHECK(indexedDraw(rig).empty());
        CHECK(notes("state vertex data too large") == 1 && notes("state unreadable vertex memory") == 0);
        CHECK(rig.builder.stats().vertex_bytes_copied == 12 && rig.builder.stats().vertex_bytes_union == 12);
    }
    tox2::rsxUnimplementedReset();
}

// Attributes of one location whose ranges overlap are copied once: slot sharing, offsets into the shared range, the
// bytes each attribute's elements read, the cases that stay separate and the size limit on the unshared total.
void testSharedVertexRanges() {
    tox2::rsxUnimplementedReset();
    FakeRsxMemory memory;
    memory.main.resize(40u << 20);
    for (std::uint32_t i = 0; i < 0x400; i++) {
        memory.main[0x1000 + i] = static_cast<std::uint8_t>(i * 7 + 1);
        memory.local[0x1000 + i] = static_cast<std::uint8_t>(0x55 + i * 3);
    }
    const auto setAttrib = [](FragmentRig& rig, std::uint32_t slot, std::uint32_t type, std::uint32_t size,
                              std::uint32_t stride, std::uint32_t location_offset) {
        rig.write(0x1740 + slot * 4, type | (size << 4) | (stride << 8));
        rig.write(0x1680 + slot * 4, location_offset);
    };
    // Every element of the attribute reads the guest bytes it would have read from a copy of its own.
    const auto readsGuestBytes = [&](const tox2::RsxDraw& draw, std::size_t index, std::uint32_t location,
                                     std::uint32_t address, std::uint32_t element) {
        const tox2::RsxVertexAttrib& attrib = draw.attribs[index];
        const std::vector<std::uint8_t>& guest = location == 0 ? memory.local : memory.main;
        for (std::uint32_t k = 0; k < 3; k++) {
            for (std::uint32_t b = 0; b < element; b++) {
                const std::size_t at = attrib.data_offset + k * attrib.stride + b;
                if (at >= draw.vertex_data.size() || draw.vertex_data[at] != guest[address + k * attrib.stride + b])
                    return false;
            }
        }
        return attrib.has_data;
    };

    // Position at +0 and colour at +12 of a 16-byte vertex: one interval, read once.
    {
        FragmentRig rig(memory);
        setAttrib(rig, 0, 2, 3, 16, 0x80001000);
        setAttrib(rig, 1, 4, 4, 16, 0x8000100C);
        const tox2::RsxDraw draw = rig.draw(0x2000);
        CHECK(draw.attribs.size() == 2 && draw.vertex_data.size() == 48);
        CHECK(draw.attribs[0].data_offset == 0 && draw.attribs[1].data_offset == 12);
        CHECK(readsGuestBytes(draw, 0, 1, 0x1000, 12) && readsGuestBytes(draw, 1, 1, 0x100C, 4));
        CHECK(memory.readsAt(1, 0x1000) >= 1 && memory.readsAt(1, 0x100C) == 0);
    }

    // A narrower stride inside the range, and a partial overlap past its end: one interval of 72 bytes.
    {
        FragmentRig rig(memory);
        setAttrib(rig, 0, 2, 3, 16, 0x80001000);
        setAttrib(rig, 1, 4, 4, 8, 0x80001004);
        setAttrib(rig, 2, 4, 4, 16, 0x80001024);
        const tox2::RsxDraw draw = rig.draw(0x2000);
        CHECK(draw.vertex_data.size() == 72);
        CHECK(draw.attribs[0].data_offset == 0 && draw.attribs[1].data_offset == 4 &&
              draw.attribs[2].data_offset == 36);
        CHECK(readsGuestBytes(draw, 0, 1, 0x1000, 12) && readsGuestBytes(draw, 1, 1, 0x1004, 4) &&
              readsGuestBytes(draw, 2, 1, 0x1024, 4));
    }

    // The first attribute may sit after the second in memory; the interval still starts at the lower address.
    {
        FragmentRig rig(memory);
        setAttrib(rig, 0, 4, 4, 16, 0x8000100C);
        setAttrib(rig, 1, 2, 3, 16, 0x80001000);
        const tox2::RsxDraw draw = rig.draw(0x2000);
        CHECK(draw.vertex_data.size() == 48 && draw.attribs[0].data_offset == 12 && draw.attribs[1].data_offset == 0);
        CHECK(readsGuestBytes(draw, 0, 1, 0x100C, 4) && readsGuestBytes(draw, 1, 1, 0x1000, 12));
    }

    // Ranges that do not overlap, and the same address in the other location, keep their own 4-byte aligned slots.
    {
        FragmentRig rig(memory);
        setAttrib(rig, 0, 2, 3, 16, 0x80001000);
        setAttrib(rig, 1, 4, 4, 16, 0x80001100);
        setAttrib(rig, 2, 4, 4, 16, 0x00001000);
        const tox2::RsxDraw draw = rig.draw(0x2000);
        CHECK(draw.vertex_data.size() == 116);
        CHECK(draw.attribs[0].data_offset == 0 && draw.attribs[1].data_offset == 44 &&
              draw.attribs[2].data_offset == 80);
        CHECK(readsGuestBytes(draw, 0, 1, 0x1000, 12) && readsGuestBytes(draw, 1, 1, 0x1100, 4) &&
              readsGuestBytes(draw, 2, 0, 0x1000, 4));
    }

    // 512 K vertices at stride 64: each attribute is 32 MB. Two sharing one array are still drawn (merged size);
    // a third makes the unshared total pass 64 MB and the draw is dropped, as before ranges were shared.
    const auto indexedDraw = [&](FragmentRig& rig) {
        memory.putWord(0, 0x5000, 0);
        memory.putWord(0, 0x5004, 0x7FFFF);
        rig.write(0x08E4, 0x2000 | 1);
        rig.write(0x1808, 5);
        rig.write(0x181C, 0x5000);
        rig.write(0x1820, 0);
        rig.write(0x1824, 1u << 24);
        rig.write(0x1808, 0);
        return rig.builder.takeCommands();
    };
    {
        FragmentRig rig(memory);
        setAttrib(rig, 0, 4, 4, 64, 0x80000000);
        setAttrib(rig, 1, 4, 4, 64, 0x80000004);
        const auto commands = indexedDraw(rig);
        CHECK(commands.size() == 1);
        if (commands.size() == 1) {
            const auto& draw = std::get<tox2::RsxDraw>(commands[0]);
            CHECK(draw.vertex_data.size() == 524287u * 64 + 8 && draw.attribs[1].data_offset == 4);
        }
    }
    {
        tox2::rsxUnimplementedReset();
        FragmentRig rig(memory);
        setAttrib(rig, 0, 4, 4, 64, 0x80000000);
        setAttrib(rig, 1, 4, 4, 64, 0x80000004);
        setAttrib(rig, 2, 4, 4, 64, 0x80000008);
        CHECK(indexedDraw(rig).empty());
        CHECK(unimplementedCount("state vertex data too large") == 1);
    }
    tox2::rsxUnimplementedReset();
}

bool sameSurface(const tox2::RsxSurface& a, const tox2::RsxSurface& b) {
    return a.location == b.location && a.offset == b.offset && a.pitch == b.pitch && a.width == b.width &&
           a.height == b.height && a.format == b.format && a.depth_format == b.depth_format;
}

bool sameSurfaceState(const tox2::RsxSurfaceState& a, const tox2::RsxSurfaceState& b) {
    return sameSurface(a.color, b.color) && a.has_color == b.has_color && sameSurface(a.depth, b.depth) &&
           a.depth_format == b.depth_format && a.clip_x == b.clip_x && a.clip_y == b.clip_y && a.clip_w == b.clip_w &&
           a.clip_h == b.clip_h;
}

// Compares the snapshots' contents; a null snapshot reads as all zeros, so only null against null is equal there.
bool sameConstants(const tox2::RsxDraw& a, const tox2::RsxDraw& b) {
    if (!a.vp_constants || !b.vp_constants) return !a.vp_constants && !b.vp_constants;
    return *a.vp_constants == *b.vp_constants;
}

bool sameCommand(const tox2::RsxCommand& left, const tox2::RsxCommand& right) {
    if (left.index() != right.index()) return false;
    if (const auto* a = std::get_if<tox2::RsxClear>(&left)) {
        const auto& b = std::get<tox2::RsxClear>(right);
        return sameSurfaceState(a->surfaces, b.surfaces) && a->mask == b.mask && a->color_argb == b.color_argb &&
               a->depth_stencil == b.depth_stencil && std::equal(a->scissor, a->scissor + 4, b.scissor);
    }
    if (const auto* a = std::get_if<tox2::RsxFlip>(&left)) {
        const auto& b = std::get<tox2::RsxFlip>(right);
        return a->buffer == b.buffer && a->offset == b.offset && a->pitch == b.pitch && a->width == b.width &&
               a->height == b.height && a->flip_index == b.flip_index;
    }
    const auto& a = std::get<tox2::RsxDraw>(left);
    const auto& b = std::get<tox2::RsxDraw>(right);
    if (!sameSurfaceState(a.surfaces, b.surfaces) || a.primitive != b.primitive || a.indexed != b.indexed ||
        a.index_type != b.index_type || a.first != b.first || a.count != b.count || a.min_index != b.min_index ||
        a.attribs.size() != b.attribs.size() || a.textures.size() != b.textures.size() ||
        a.vertex_data != b.vertex_data || a.index_data != b.index_data || a.vp_hash != b.vp_hash ||
        a.vp_ucode != b.vp_ucode || !sameConstants(a, b) || a.fp_hash != b.fp_hash || a.fp_ucode != b.fp_ucode ||
        a.fp_constants != b.fp_constants || a.fp_control != b.fp_control || a.texcoord_control != b.texcoord_control ||
        a.raster.cull != b.raster.cull || a.default_attribs != b.default_attribs) {
        return false;
    }
    for (std::size_t i = 0; i < a.textures.size(); i++) {
        const auto& x = a.textures[i];
        const auto& y = b.textures[i];
        if (x.unit != y.unit || x.rt_alias != y.rt_alias || x.offset != y.offset || x.pitch != y.pitch ||
            x.width != y.width || x.height != y.height || (x.data == nullptr) != (y.data == nullptr) ||
            (x.data != nullptr && *x.data != *y.data)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.attribs.size(); i++) {
        const auto& x = a.attribs[i];
        const auto& y = b.attribs[i];
        if (x.slot != y.slot || x.type != y.type || x.size != y.size || x.stride != y.stride ||
            x.location != y.location || x.offset != y.offset || x.data_offset != y.data_offset ||
            x.has_data != y.has_data) {
            return false;
        }
    }
    return true;
}

// Draw vectors handed back with recycle() serve the next draws: same buffers, same contents as a fresh builder's
// draw, texture blobs released at once, a size cap on the kept capacity and a cap on the number of kept sets.
void testRecycledDrawBuffers() {
    tox2::rsxUnimplementedReset();
    FakeRsxMemory memory;
    memory.main.resize(2u << 20);
    for (std::size_t i = 0; i < memory.main.size(); i++) memory.main[i] = static_cast<std::uint8_t>(i * 7 + (i >> 8));
    for (std::uint32_t i = 0; i < 1024; i++) memory.local[0x3000 + i] = static_cast<std::uint8_t>(i ^ 0x5A);
    memory.putWord(0, 0x2000, 0x00010000);

    // A rig whose draws sample a linear 4x4 texture (one shared blob in the builder's texture cache).
    const auto makeRig = [&] {
        auto rig = std::make_unique<FragmentRig>(memory);
        rig->write(0x08E4, 0x2000 | 1);
        rig->write(0x1A00 + 0x20, 0x3000);
        rig->write(0x1A04 + 0x20, 1u | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16));
        rig->write(0x1A0C + 0x20, 0x80000000);
        rig->write(0x1A18 + 0x20, (16u << 16) | 4u);
        rig->write(0x1840 + 4, 64);
        return rig;
    };
    // A draw of `vertices` vertices (a multiple of 256: one DRAW_ARRAYS write covers 256).
    const auto drawArrays = [](FragmentRig& rig, std::uint32_t vertices) {
        rig.write(0x1808, 5);
        for (std::uint32_t first = 0; first < vertices; first += 256) rig.write(0x1814, first | (255u << 24));
        rig.write(0x1808, 0);
    };

    auto rig = makeRig();
    auto fresh = makeRig();
    drawArrays(*rig, 1024);
    drawArrays(*fresh, 1024);
    std::vector<tox2::RsxCommand> commands = rig->builder.takeCommands();
    std::vector<tox2::RsxCommand> expected = fresh->builder.takeCommands();
    CHECK(commands.size() == 1 && expected.size() == 1);
    const tox2::RsxDraw& first = std::get<tox2::RsxDraw>(commands[0]);
    CHECK(first.vertex_data.size() == 1024 * 12 && first.textures.size() == 1 && first.textures[0].data);
    const std::weak_ptr<const std::vector<std::uint8_t>> blob = first.textures[0].data;
    CHECK(blob.use_count() == 3);  // the texture cache, the unit's remembered descriptor and the draw
    const std::uint8_t* const vertex_buffer = first.vertex_data.data();
    const void* const command_storage = commands.data();
    CHECK(rig->builder.spareDraws() == 0);

    rig->builder.recycle(std::move(commands));
    CHECK(rig->builder.spareDraws() == 1 && blob.use_count() == 2);  // the draw's reference is gone

    // The next draw is built in the recycled buffer, in the recycled command vector, and equals a fresh builder's.
    drawArrays(*rig, 1024);
    drawArrays(*fresh, 1024);
    commands = rig->builder.takeCommands();
    expected = fresh->builder.takeCommands();
    CHECK(commands.size() == 1 && expected.size() == 1 && commands.data() == command_storage);
    const tox2::RsxDraw& second = std::get<tox2::RsxDraw>(commands[0]);
    const tox2::RsxDraw& reference = std::get<tox2::RsxDraw>(expected[0]);
    CHECK(rig->builder.spareDraws() == 0 && second.vertex_data.data() == vertex_buffer);
    CHECK(second.vertex_data == reference.vertex_data && second.index_data == reference.index_data);
    CHECK(second.attribs.size() == reference.attribs.size() && second.attribs[0].data_offset == 0);
    CHECK(second.fp_ucode == reference.fp_ucode && second.fp_constants == reference.fp_constants &&
          second.fp_hash == reference.fp_hash);
    CHECK(second.vp_ucode == reference.vp_ucode && second.vp_hash == reference.vp_hash);
    CHECK(second.textures.size() == 1 && second.textures[0].unit == 1 && *second.textures[0].data == *blob.lock());
    CHECK(blob.use_count() == 3);
    rig->builder.recycle(std::move(commands));
    CHECK(rig->builder.spareDraws() == 1 && blob.use_count() == 2);

    // A draw with more than 1 MiB of vertex data is built and recycled without changing the number of spares, and
    // its buffer is not kept: the next small draw gets a vector sized for itself.
    drawArrays(*rig, 256 * 392);
    commands = rig->builder.takeCommands();
    CHECK(commands.size() == 1 && std::get<tox2::RsxDraw>(commands[0]).vertex_data.size() > (1u << 20));
    rig->builder.recycle(std::move(commands));
    CHECK(rig->builder.spareDraws() == 1);
    drawArrays(*rig, 1024);
    commands = rig->builder.takeCommands();
    CHECK(commands.size() == 1 && std::get<tox2::RsxDraw>(commands[0]).vertex_data.capacity() <= (1u << 20));
    rig->builder.recycle(std::move(commands));

    // No more than 64 sets are kept, however many draws come back at once.
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < 70; i++) drawArrays(*rig, 256);
        commands = rig->builder.takeCommands();
        CHECK(commands.size() == 70);
        rig->builder.recycle(std::move(commands));
        CHECK(rig->builder.spareDraws() == 64);
    }
    // Clears and flips come back too; they keep nothing.
    rig->builder.recycle(std::vector<tox2::RsxCommand>{tox2::RsxFlip{}});
    CHECK(rig->builder.spareDraws() == 64);
    tox2::rsxUnimplementedReset();
}

// F_X32 color surfaces and depth surfaces as render target aliases: decoding, registration in the builder
// (which draws and clears make a surface known, what a clear of a float surface does), the alias flags of a
// texture, and the capture round trip of both kinds.
// The frame builder's texture cache: one blob and one guest read per epoch for an unchanged texture, a new
// content id when the guest bytes change, and the events that end the trust in a cached blob.
void testRsxTextureCache() {
    // Hash: any single changed byte, and a changed length of zero bytes, give another value.
    {
        std::vector<std::uint8_t> bytes(300);
        for (std::size_t i = 0; i < bytes.size(); i++) bytes[i] = static_cast<std::uint8_t>(i * 7 + 1);
        bool distinct = true;
        for (std::size_t length : {0, 1, 7, 8, 15, 16, 17, 63, 64, 65, 127, 128, 129, 300}) {
            const std::uint64_t base = tox2::rsxHashBytes(std::span(bytes).first(length));
            for (std::size_t i = 0; i < length; i++) {
                bytes[i] ^= 0x10;
                distinct = distinct && tox2::rsxHashBytes(std::span(bytes).first(length)) != base;
                bytes[i] ^= 0x10;
            }
            distinct = distinct && base == tox2::rsxHashBytes(std::span(bytes).first(length));
        }
        CHECK(distinct);
        const std::vector<std::uint8_t> zeros(64);
        CHECK(tox2::rsxHashBytes(std::span(zeros).first(8)) != tox2::rsxHashBytes(std::span(zeros).first(9)));
    }

    // Eviction: past the byte budget the least recently used entry goes; an entry unused for many flips
    // goes at the next sweep.
    {
        tox2::RsxTextureCache cache(250);
        tox2::RsxTextureLayout layout;
        layout.guest_bytes = 100;
        layout.levels.resize(1);
        const auto blob = [] { return std::make_shared<const std::vector<std::uint8_t>>(100); };
        const auto key = [](std::uint32_t offset) {
            tox2::RsxTextureCache::Key k;
            k.offset = offset;
            return k;
        };
        cache.store(key(0), layout, 1, blob());
        cache.store(key(100), layout, 2, blob());
        CHECK(cache.find(key(0)) != nullptr);  // key(100) is now the least recently used
        const std::uint64_t first_id = cache.find(key(0))->content_id;
        cache.store(key(200), layout, 3, blob());
        CHECK(cache.entryCount() == 2 && cache.byteCount() == 200);
        CHECK(cache.find(key(100)) == nullptr && cache.find(key(0)) != nullptr && cache.find(key(200)) != nullptr);
        CHECK(cache.find(key(0))->content_id == first_id);
        cache.onFlip(32);
        CHECK(cache.entryCount() == 2);
        cache.onFlip((tox2::RsxTextureCache::kIdleFlips / 32 + 2) * 32);  // a sweep flip, past the idle limit
        CHECK(cache.entryCount() == 0 && cache.byteCount() == 0);
    }

    tox2::rsxUnimplementedReset();
    auto regs = std::make_unique<tox2::RsxRegisters>();
    regs->reset();
    FakeRsxMemory memory;
    tox2::RsxFrameBuilder builder(memory);
    const auto write = [&](std::uint32_t method, std::uint32_t value) {
        regs->apply(method, value);
        builder.onWrite(*regs, method, value);
    };
    const auto flip = [&] {
        builder.onFlip(*regs, 0, {});
        regs->contextReset();
    };
    const auto setSurface = [&](std::uint32_t offset, std::uint32_t pitch, std::uint32_t height) {
        write(0x0200, 16u << 16);
        write(0x0204, height << 16);
        write(0x0208, 8u | (2u << 5) | (1u << 8));
        write(0x020C, pitch);
        write(0x0210, offset);
        write(0x0214, 0x900000);
        write(0x022C, pitch);
        write(0x0220, 1);
        write(0x08C0, 16u << 16);
        write(0x08C4, height << 16);
    };
    // Unit `unit` samples a texture at local `offset`: format byte `fmt` (0x85 swizzled, 0xA5 linear) and
    // `mips` levels, width x height texels, pitch register `pitch`.
    const auto setTexture = [&](std::uint32_t unit, std::uint32_t offset, std::uint32_t fmt, std::uint32_t mips,
                                std::uint32_t width, std::uint32_t height, std::uint32_t pitch) {
        write(0x1A00 + unit * 0x20, offset);
        write(0x1A04 + unit * 0x20, 1u | (2u << 4) | (fmt << 8) | (mips << 16));
        write(0x1A0C + unit * 0x20, 0x80000000);
        write(0x1A18 + unit * 0x20, (width << 16) | height);
        write(0x1840 + unit * 4, pitch);
    };
    // A fragment program that ends at its first word, so that the builder does not scan guest memory for the
    // end flag (and read the texture bytes on the way).
    memory.putWord(0, 0x2000, 0x00010000);
    const auto draw = [&]() {
        write(0x08E4, 0x2000 | 1);
        write(0x1808, 5);
        write(0x1814, 2u << 24);
        write(0x1808, 0);
        // The commands since the last call: possibly a flip, then the draw.
        std::vector<tox2::RsxCommand> commands = builder.takeCommands();
        tox2::RsxDraw result;
        int draws = 0;
        for (const tox2::RsxCommand& command : commands) {
            if (const auto* found = std::get_if<tox2::RsxDraw>(&command)) {
                result = *found;
                draws++;
            }
        }
        CHECK(draws == 1);
        return result;
    };
    const auto fill = [&](std::uint32_t offset, std::uint32_t size, std::uint8_t seed) {
        for (std::uint32_t i = 0; i < size; i++) memory.local[offset + i] = static_cast<std::uint8_t>(i + seed);
    };
    constexpr std::uint32_t kTex = 0x3000;
    setSurface(0x100000, 64, 4);
    fill(kTex, 64, 1);
    setTexture(0, kTex, 0xA5, 1, 4, 4, 16);

    // Draws in one epoch share the blob and read guest memory once; a guest write nobody announced is not
    // seen yet. A new texture is copied without a hash pass first.
    const tox2::RsxDraw first = draw();
    CHECK(first.textures.size() == 1 && first.textures[0].data != nullptr && first.textures[0].content_id != 0);
    CHECK(texSize(first.textures[0]) == 64 && texByte(first.textures[0], 0) == 1);
    fill(kTex, 64, 100);
    const tox2::RsxDraw second = draw();
    CHECK(second.textures[0].data == first.textures[0].data &&
          second.textures[0].content_id == first.textures[0].content_id);
    CHECK(memory.readsAt(0, kTex) == 1 && memory.hashesAt(0, kTex) == 0);

    // A flip ends the epoch: the changed bytes are hashed in place, then copied for a new blob and id, and the
    // earlier draws keep theirs.
    flip();
    setTexture(0, kTex, 0xA5, 1, 4, 4, 16);
    const tox2::RsxDraw third = draw();
    CHECK(memory.readsAt(0, kTex) == 2 && memory.hashesAt(0, kTex) == 1);
    CHECK(third.textures[0].content_id != first.textures[0].content_id &&
          third.textures[0].data != first.textures[0].data);
    CHECK(texByte(third.textures[0], 0) == 100 && texByte(first.textures[0], 0) == 1);

    // After a flip with the bytes unchanged the guest memory is hashed in place and not copied, and the blob and id
    // stay.
    flip();
    setTexture(0, kTex, 0xA5, 1, 4, 4, 16);
    const tox2::RsxDraw fourth = draw();
    const tox2::RsxDraw fifth = draw();
    CHECK(memory.readsAt(0, kTex) == 2 && memory.hashesAt(0, kTex) == 2);
    CHECK(builder.stats().texture_validations == 2 && builder.stats().texture_bytes_hashed == 128 &&
          builder.stats().texture_bytes_read == 128);
    CHECK(fourth.textures[0].content_id == third.textures[0].content_id &&
          fourth.textures[0].data == third.textures[0].data);
    CHECK(fifth.textures[0].data == third.textures[0].data);

    // A semaphore acquire (and a release) ends the epoch too.
    setSurface(0x100000, 64, 4);
    write(tox2::kMethodNv4097SetColorMask, 0x01010101);
    fill(kTex, 64, 7);
    write(tox2::kMethodNv406eSemaphoreAcquire, 0);
    const tox2::RsxDraw after_acquire = draw();
    CHECK(memory.readsAt(0, kTex) == 3 && memory.hashesAt(0, kTex) == 3 &&
          after_acquire.textures[0].content_id != third.textures[0].content_id);
    fill(kTex, 64, 9);
    write(tox2::kMethodNv406eSemaphoreRelease, 1);
    const tox2::RsxDraw after_release = draw();
    CHECK(memory.readsAt(0, kTex) == 4 && memory.hashesAt(0, kTex) == 4 &&
          after_release.textures[0].content_id != after_acquire.textures[0].content_id);

    // A target the RSX draws or clears over the texture's range ends the trust at once, inside the epoch; a
    // target elsewhere does not.
    fill(kTex, 64, 11);
    write(0x1D90, 0xFF102030);
    write(0x1D8C, 0xFFFFFF00);
    write(0x1D94, 0xF3);  // clears the surface at 0x100000
    CHECK(builder.takeCommands().size() == 1);
    const tox2::RsxDraw elsewhere = draw();
    CHECK(memory.readsAt(0, kTex) == 4 && memory.hashesAt(0, kTex) == 4 &&
          elsewhere.textures[0].content_id == after_release.textures[0].content_id);
    setSurface(kTex - 0x40, 64, 4);  // covers [kTex - 0x40, kTex + 0xC0)
    write(0x1D94, 0xF3);
    CHECK(builder.takeCommands().size() == 1);
    const tox2::RsxDraw overlapped = draw();
    CHECK(memory.readsAt(0, kTex) == 5 && memory.hashesAt(0, kTex) == 5 &&
          overlapped.textures[0].content_id != after_release.textures[0].content_id);
    CHECK(overlapped.textures[0].rt_alias == -1 && texByte(overlapped.textures[0], 0) == 11);
    CHECK(overlapped.surfaces.color.offset == kTex - 0x40);
    const tox2::RsxDraw again = draw();  // the draw above wrote the target again, so the texture is read again
    CHECK(memory.readsAt(0, kTex) == 5 && memory.hashesAt(0, kTex) == 6 &&
          again.textures[0].content_id == overlapped.textures[0].content_id);

    // A texture at the address of a known render target is an alias and carries no bytes, cached blob or not.
    setSurface(kTex, 64, 4);
    write(0x1D94, 0xF3);
    CHECK(builder.takeCommands().size() == 1);
    const tox2::RsxDraw alias = draw();
    CHECK(alias.textures[0].rt_alias >= 0 && alias.textures[0].data == nullptr && alias.textures[0].content_id == 0);

    // Descriptors that differ in anything that changes the packed bytes get their own entries.
    flip();
    setSurface(0x100000, 64, 4);
    fill(kTex + 0x100, 256, 3);
    const std::size_t entries_before = builder.textureCache().entryCount();
    setTexture(0, kTex + 0x100, 0xA5, 1, 4, 4, 16);  // linear 4x4
    setTexture(1, kTex + 0x100, 0x85, 1, 4, 4, 0);   // the same bytes swizzled
    setTexture(2, kTex + 0x100, 0xA5, 1, 4, 4, 32);  // padded pitch
    setTexture(3, kTex + 0x100, 0xA5, 2, 4, 4, 16);  // two levels
    setTexture(4, kTex + 0x100, 0xA5, 1, 8, 2, 32);  // another shape
    setTexture(5, kTex + 0x100, 0xA5, 1, 4, 4, 16);  // same as unit 0
    const tox2::RsxDraw variants = draw();
    CHECK(variants.textures.size() == 6);
    CHECK(builder.textureCache().entryCount() == entries_before + 5);
    for (std::size_t a = 0; a < 5; a++) {
        for (std::size_t b = a + 1; b < 5; b++) {
            CHECK(variants.textures[a].content_id != variants.textures[b].content_id);
        }
    }
    CHECK(variants.textures[5].content_id == variants.textures[0].content_id &&
          variants.textures[5].data == variants.textures[0].data);
    CHECK(texSize(variants.textures[1]) == 64 && variants.textures[1].data != variants.textures[0].data);
    CHECK(*variants.textures[1].data != *variants.textures[0].data);
    CHECK(unimplementedCount("state unreadable texture memory") == 0);

    // An unreadable range is counted on every draw and leaves no entry.
    for (std::uint32_t unit = 0; unit < 6; unit++) write(0x1A0C + unit * 0x20, 0);
    setTexture(6, 0xFFF0, 0xA5, 1, 16, 16, 64);
    const tox2::RsxDraw unreadable = draw();
    CHECK(texSize(unreadable.textures.back()) == 0 && unimplementedCount("state unreadable texture memory") == 1);
    CHECK(builder.textureCache().entryCount() == entries_before + 5);

    // A cached texture whose memory is no longer readable is counted and dropped, and nothing is copied for it.
    const std::uint64_t reads_before = builder.stats().texture_bytes_read;
    flip();
    setSurface(0x100000, 64, 4);
    write(0x1A0C + 6 * 0x20, 0);
    setTexture(0, kTex + 0x100, 0xA5, 1, 4, 4, 16);
    memory.local.resize(kTex + 0x100 + 32);
    const tox2::RsxDraw gone = draw();
    CHECK(texSize(gone.textures[0]) == 0 && unimplementedCount("state unreadable texture memory") == 2);
    CHECK(builder.textureCache().entryCount() == entries_before + 4);
    CHECK(builder.stats().texture_bytes_read == reads_before);
}

// A frame builder over fake memory with the register helpers the texture unit cache tests share.
struct RsxTextureUnitHarness {
    RsxTextureUnitHarness() : regs(std::make_unique<tox2::RsxRegisters>()), builder(memory) {
        regs->reset();
        // A fragment program that ends at its first word, so that the builder does not scan guest memory for the
        // end flag (and read the texture bytes on the way).
        memory.putWord(0, 0x2000, 0x00010000);
    }
    void write(std::uint32_t method, std::uint32_t value) {
        regs->apply(method, value);
        builder.onWrite(*regs, method, value);
    }
    void flip() {
        builder.onFlip(*regs, 0, {});
        regs->contextReset();
    }
    void setSurface(std::uint32_t offset, std::uint32_t pitch, std::uint32_t height) {
        write(0x0200, 16u << 16);
        write(0x0204, height << 16);
        write(0x0208, 8u | (2u << 5) | (1u << 8));
        write(0x020C, pitch);
        write(0x0210, offset);
        write(0x0214, 0x900000);
        write(0x022C, pitch);
        write(0x0220, 1);
        write(0x08C0, 16u << 16);
        write(0x08C4, height << 16);
    }
    void setTexture(std::uint32_t unit, std::uint32_t offset, std::uint32_t fmt, std::uint32_t mips,
                    std::uint32_t width, std::uint32_t height, std::uint32_t pitch) {
        write(0x1A00 + unit * 0x20, offset);
        write(0x1A04 + unit * 0x20, 1u | (2u << 4) | (fmt << 8) | (mips << 16));
        write(0x1A0C + unit * 0x20, 0x80000000);
        write(0x1A18 + unit * 0x20, (width << 16) | height);
        write(0x1840 + unit * 4, pitch);
    }
    void clear(std::uint32_t mask) {
        write(0x1D90, 0xFF102030);
        write(0x1D8C, 0xFFFFFF00);
        write(0x1D94, mask);
    }
    void fill(std::uint32_t offset, std::uint32_t size, std::uint8_t seed) {
        for (std::uint32_t i = 0; i < size; i++) memory.local[offset + i] = static_cast<std::uint8_t>(i + seed);
    }
    // One draw; the commands since the last call are taken (a flip or a clear may precede it).
    tox2::RsxDraw draw() {
        write(0x08E4, 0x2000 | 1);
        write(0x1808, 5);
        write(0x1814, 2u << 24);
        write(0x1808, 0);
        tox2::RsxDraw result;
        int draws = 0;
        for (const tox2::RsxCommand& command : builder.takeCommands()) {
            if (const auto* found = std::get_if<tox2::RsxDraw>(&command)) {
                result = *found;
                draws++;
            }
        }
        CHECK(draws == 1);
        return result;
    }
    std::uint64_t hits() const { return builder.stats().texture_unit_hits; }
    std::uint64_t misses() const { return builder.stats().texture_unit_misses; }
    // Whether the unit cache counted exactly `more_hits` hits and `more_misses` misses since the previous call.
    bool since(std::uint64_t more_hits, std::uint64_t more_misses) {
        const bool ok = hits() == mark_hits + more_hits && misses() == mark_misses + more_misses;
        mark_hits = hits();
        mark_misses = misses();
        return ok;
    }

    std::unique_ptr<tox2::RsxRegisters> regs;
    FakeRsxMemory memory;
    tox2::RsxFrameBuilder builder;
    std::uint64_t mark_hits = 0, mark_misses = 0;
};

// What the equivalence run records per draw: the descriptors and the census after it.
struct RsxUnitCacheStep {
    std::vector<tox2::RsxTextureDesc> textures;
    std::vector<std::pair<std::string, std::uint64_t>> census;
};

struct RsxUnitCacheRun {
    std::vector<RsxUnitCacheStep> steps;
    std::uint64_t validations = 0, hashed = 0, read = 0, hits = 0, misses = 0;
};

// A scripted sequence with repeats, register changes, a unit with location field 3 (noted every draw), an unreadable
// range, render targets written over cached ranges, an alias, a semaphore acquire and flips.
RsxUnitCacheRun runRsxUnitCacheScript(bool unit_cache) {
    tox2::rsxUnimplementedReset();
    RsxTextureUnitHarness h;
    h.builder.setTextureUnitCache(unit_cache);
    RsxUnitCacheRun run;
    const auto step = [&] {
        RsxUnitCacheStep s;
        s.textures = h.draw().textures;
        s.census = tox2::rsxUnimplementedSnapshot();
        run.steps.push_back(std::move(s));
    };
    constexpr std::uint32_t kA = 0x3000, kB = 0x3100, kC = 0x3200;
    h.setSurface(0x100000, 64, 4);
    h.fill(kA, 64, 1);
    h.fill(kB, 64, 2);
    h.fill(kC, 64, 3);
    h.setTexture(0, kA, 0xA5, 1, 4, 4, 16);
    h.setTexture(1, kB, 0x85, 1, 4, 4, 0);
    step();                       // 0: first use
    step();                       // 1: repeat
    h.write(0x1A10, 0x0000AAE4);  // 2: unit 0 remap
    step();
    h.write(0x1A34, 0x00002000);  // 3: unit 1 filter
    step();
    h.write(0x1A00, kC);  // 4: unit 0 at another texture
    step();
    step();  // 5: repeat
    h.setTexture(2, kA, 0xA5, 1, 4, 4, 16);
    h.write(0x1A44, 3u | (2u << 4) | (0xA5u << 8) | (1u << 16));  // unit 2: location field 3, noted every draw
    step();                                                       // 6
    step();                                                       // 7
    h.write(0x1A4C, 0);                                           // unit 2 off
    h.setTexture(3, 0xFFF0, 0xA5, 1, 16, 16, 64);                 // 8: unreadable range, noted every draw
    step();
    step();                          // 9
    h.write(0x1A6C, 0);              // unit 3 off
    h.setSurface(kC - 0x40, 64, 4);  // a target over a cached range
    h.clear(0xF3);
    step();              // 10: re-read
    h.fill(kC, 64, 77);  // unannounced, seen because the draw wrote the target again
    step();              // 11
    h.setSurface(0x100000, 64, 4);
    step();                       // 12
    step();                       // 13
    h.write(0x1A10, 0x0000AAE4);  // same value: still a repeat
    step();                       // 14
    h.write(0x1A10, 0x0000AAE4 + 1);
    step();  // 15
    h.write(tox2::kMethodNv406eSemaphoreAcquire, 0);
    h.fill(kA, 64, 9);
    step();  // 16: new epoch, changed bytes
    step();  // 17
    h.flip();
    h.setTexture(0, kA, 0xA5, 1, 4, 4, 16);
    h.setTexture(1, kB, 0x85, 1, 4, 4, 0);
    step();                   // 18: new epoch, unchanged bytes
    step();                   // 19
    h.setSurface(kB, 64, 4);  // an alias for unit 1
    h.clear(0xF3);
    step();  // 20
    step();  // 21
    h.setSurface(0x100000, 64, 4);
    step();               // 22
    h.write(0x1A00, kC);  // unit 0 back to the texture of step 4
    step();               // 23
    step();               // 24
    run.validations = h.builder.stats().texture_validations;
    run.hashed = h.builder.stats().texture_bytes_hashed;
    run.read = h.builder.stats().texture_bytes_read;
    run.hits = h.hits();
    run.misses = h.misses();
    return run;
}

// A unit reuses its descriptor while its registers, the known surfaces and the texture cache state are unchanged.
void testRsxTextureUnitCache() {
    constexpr std::uint32_t kTex = 0x3000;

    // (a) The same registers in one epoch: a hit per enabled unit, the same blob, no guest access. A unit whose
    // entry was made after another unit's in the same draw sees the entry set change once, so the first repeat of
    // two new units hits only one of them.
    {
        RsxTextureUnitHarness h;
        h.setSurface(0x100000, 64, 4);
        h.fill(kTex, 64, 1);
        h.fill(kTex + 0x100, 64, 2);
        h.setTexture(0, kTex, 0xA5, 1, 4, 4, 16);
        h.setTexture(1, kTex + 0x100, 0xA5, 1, 4, 4, 16);
        const tox2::RsxDraw first = h.draw();
        CHECK(h.since(0, 2));
        h.draw();
        CHECK(h.since(1, 1));
        const tox2::RsxDraw third = h.draw();
        CHECK(h.since(2, 0));
        CHECK(first.textures.size() == 2 && third.textures.size() == 2);
        for (std::size_t i = 0; i < 2; i++) {
            CHECK(third.textures[i].data != nullptr && third.textures[i].data == first.textures[i].data);
            CHECK(third.textures[i].content_id == first.textures[i].content_id && first.textures[i].content_id != 0);
            CHECK(third.textures[i].pitch == first.textures[i].pitch && third.textures[i].unit == i);
        }
        CHECK(first.textures[0].content_id != first.textures[1].content_id);
        CHECK(h.memory.readsAt(0, kTex) == 1 && h.memory.hashesAt(0, kTex) == 0);
        CHECK(h.builder.stats().texture_validations == 0);

        // A new epoch starts with a miss (the entry is validated against the guest bytes), then hits again; the
        // unchanged bytes keep the entries, so nothing moves the entry set.
        h.write(tox2::kMethodNv406eSemaphoreAcquire, 0);
        const tox2::RsxDraw fourth = h.draw();
        CHECK(h.since(0, 2) && h.memory.hashesAt(0, kTex) == 1);
        const tox2::RsxDraw fifth = h.draw();
        CHECK(h.since(2, 0) && h.memory.hashesAt(0, kTex) == 1);
        CHECK(fourth.textures[0].content_id == first.textures[0].content_id &&
              fifth.textures[0].content_id == first.textures[0].content_id);

        // A flip clears the registers: a unit that is no longer enabled is not counted.
        h.flip();
        h.setTexture(0, kTex, 0xA5, 1, 4, 4, 16);
        const tox2::RsxDraw sixth = h.draw();
        CHECK(sixth.textures.size() == 1 && h.since(0, 1));
        h.draw();
        CHECK(h.since(1, 0));
    }

    // (b) One register changes: a miss with the new value. A change of the remap or the filter keeps the blob, a
    // change of the offset gives another one.
    {
        RsxTextureUnitHarness h;
        h.setSurface(0x100000, 64, 4);
        h.fill(kTex, 64, 1);
        h.fill(kTex + 0x100, 64, 50);
        h.setTexture(0, kTex, 0xA5, 1, 4, 4, 16);
        const tox2::RsxDraw base = h.draw();
        h.draw();
        CHECK(h.since(1, 1));

        h.write(0x1A10, base.textures[0].remap ^ 0x100);
        const tox2::RsxDraw remapped = h.draw();
        CHECK(h.since(0, 1));
        CHECK(remapped.textures[0].remap == (base.textures[0].remap ^ 0x100));
        CHECK(remapped.textures[0].content_id == base.textures[0].content_id &&
              remapped.textures[0].data == base.textures[0].data);
        h.draw();
        CHECK(h.since(1, 0));

        h.write(0x1A14, base.textures[0].filter ^ 0x2000);
        const tox2::RsxDraw filtered = h.draw();
        CHECK(h.since(0, 1));
        CHECK(filtered.textures[0].filter == (base.textures[0].filter ^ 0x2000) &&
              filtered.textures[0].remap == remapped.textures[0].remap);
        CHECK(filtered.textures[0].content_id == base.textures[0].content_id);

        h.write(0x1A00, kTex + 0x100);
        const tox2::RsxDraw moved = h.draw();
        CHECK(h.since(0, 1));
        CHECK(moved.textures[0].offset == kTex + 0x100 && moved.textures[0].content_id != base.textures[0].content_id);
        CHECK(texByte(moved.textures[0], 0) == 50 && texByte(filtered.textures[0], 0) == 1);
        h.draw();
        CHECK(h.since(1, 0));

        // The other registers the descriptor is made from.
        for (const std::uint32_t method : {0x1A08u, 0x1A0Cu, 0x1A18u, 0x1A1Cu, 0x1840u}) {
            const std::uint32_t old = h.regs->reg(method);
            const std::uint32_t next = method == 0x1A18u ? ((4u << 16) | 2u) : (method == 0x1840u ? 32u : old ^ 0x10u);
            h.write(method, next);
            h.draw();
            CHECK(h.since(0, 1));
            h.write(method, old);
            h.draw();
            CHECK(h.since(0, 1));
            h.draw();
            CHECK(h.since(1, 0));
        }
    }

    // (c) A render target written over the cached range between two draws of one epoch: a miss and one more hash,
    // and the draws after it validate again for as long as the target is the draw's own.
    {
        RsxTextureUnitHarness h;
        h.setSurface(kTex - 0x40, 64, 4);  // covers [kTex - 0x40, kTex + 0xC0); known before the texture is cached
        h.clear(0xF3);
        h.setSurface(0x100000, 64, 4);
        h.fill(kTex, 64, 1);
        h.setTexture(0, kTex, 0xA5, 1, 4, 4, 16);
        const tox2::RsxDraw first = h.draw();
        h.draw();
        CHECK(h.since(1, 1) && h.memory.hashesAt(0, kTex) == 0);
        h.setSurface(kTex - 0x40, 64, 4);
        h.clear(0xF3);
        const tox2::RsxDraw overlapped = h.draw();
        CHECK(h.since(0, 1) && h.memory.hashesAt(0, kTex) == 1);
        CHECK(overlapped.textures[0].content_id == first.textures[0].content_id);
        h.draw();  // the draw above wrote the target again
        CHECK(h.since(0, 1) && h.memory.hashesAt(0, kTex) == 2);
        h.setSurface(0x100000, 64, 4);
        h.draw();
        CHECK(h.since(1, 0) && h.memory.hashesAt(0, kTex) == 2);
        // A write elsewhere does not end the reuse.
        h.clear(0xF3);
        h.draw();
        CHECK(h.since(1, 0) && h.memory.hashesAt(0, kTex) == 2);
    }

    // (d) A surface registered at the texture's offset turns the unit into an alias; a known surface whose depth
    // format changes moves rt_depth.
    {
        RsxTextureUnitHarness h;
        h.setSurface(0x100000, 64, 4);
        h.fill(kTex, 64, 1);
        h.setTexture(0, kTex, 0xA5, 1, 4, 4, 16);
        const tox2::RsxDraw plain = h.draw();
        h.draw();
        CHECK(h.since(1, 1) && plain.textures[0].rt_alias == -1 && plain.textures[0].data != nullptr);

        h.setSurface(kTex, 64, 4);
        h.clear(0xF3);  // registers a color surface at kTex and a depth surface at 0x900000
        h.setSurface(0x100000, 64, 4);
        const tox2::RsxDraw alias = h.draw();
        CHECK(h.since(0, 1));
        CHECK(alias.textures[0].rt_alias >= 0 && !alias.textures[0].rt_depth && alias.textures[0].data == nullptr &&
              alias.textures[0].content_id == 0);
        const tox2::RsxDraw alias_again = h.draw();
        CHECK(h.since(1, 0));
        CHECK(alias_again.textures[0].rt_alias == alias.textures[0].rt_alias && !alias_again.textures[0].rt_depth);

        // A depth surface at the same address replaces the entry (the later one wins).
        h.write(0x0214, kTex);
        h.clear(0x03);
        h.write(0x0214, 0x900000);
        const tox2::RsxDraw depth = h.draw();
        CHECK(h.since(0, 1));
        CHECK(depth.textures[0].rt_alias == alias.textures[0].rt_alias && depth.textures[0].rt_depth);
        const tox2::RsxDraw depth_again = h.draw();
        CHECK(h.since(1, 0) && depth_again.textures[0].rt_depth);
    }

    // (d2) Known surfaces set from outside end the reuse.
    {
        RsxTextureUnitHarness h;
        h.setSurface(0x100000, 64, 4);
        h.fill(kTex, 64, 1);
        h.setTexture(0, kTex, 0xA5, 1, 4, 4, 16);
        h.draw();
        h.draw();
        CHECK(h.since(1, 1));
        tox2::RsxSurface surface;
        surface.offset = kTex;
        surface.pitch = 64;
        surface.width = 4;
        surface.height = 4;
        surface.format = tox2::RsxColorFormat::A8R8G8B8;
        h.builder.setKnownSurfaces({surface});
        const tox2::RsxDraw alias = h.draw();
        CHECK(h.since(0, 1) && alias.textures[0].rt_alias == 0 && alias.textures[0].data == nullptr);
    }

    // The off switch takes the full path for every unit.
    {
        RsxTextureUnitHarness h;
        h.builder.setTextureUnitCache(false);
        h.setSurface(0x100000, 64, 4);
        h.fill(kTex, 64, 1);
        h.setTexture(0, kTex, 0xA5, 1, 4, 4, 16);
        h.draw();
        h.draw();
        h.draw();
        CHECK(h.hits() == 0 && h.misses() == 3);
    }

    // (e) Equivalence: the same script with and without the unit cache gives the same descriptors and census after
    // every draw, the same sharing of content ids and the same guest access.
    {
        const RsxUnitCacheRun off = runRsxUnitCacheScript(false);
        const RsxUnitCacheRun on = runRsxUnitCacheScript(true);
        CHECK(off.steps.size() >= 12 && off.steps.size() == on.steps.size());
        CHECK(off.hits == 0 && on.hits > 0 && off.misses == on.hits + on.misses);
        CHECK(off.validations == on.validations && off.hashed == on.hashed && off.read == on.read);
        CHECK(off.hashed > 0 && off.read > 0);

        bool same = true;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ids;  // (off, on) content id per descriptor
        std::vector<const tox2::RsxTextureDesc*> off_descs, on_descs;
        for (std::size_t i = 0; i < off.steps.size() && i < on.steps.size(); i++) {
            same = same && off.steps[i].census == on.steps[i].census;
            const auto& a = off.steps[i].textures;
            const auto& b = on.steps[i].textures;
            same = same && a.size() == b.size();
            for (std::size_t u = 0; u < a.size() && u < b.size(); u++) {
                same = same && a[u].unit == b[u].unit && a[u].format == b[u].format && a[u].mips == b[u].mips &&
                       a[u].dimension == b[u].dimension && a[u].linear_layout == b[u].linear_layout &&
                       a[u].normalized == b[u].normalized && a[u].cubemap == b[u].cubemap && a[u].depth == b[u].depth &&
                       a[u].location == b[u].location && a[u].offset == b[u].offset && a[u].pitch == b[u].pitch &&
                       a[u].width == b[u].width && a[u].height == b[u].height && a[u].control0 == b[u].control0 &&
                       a[u].remap == b[u].remap && a[u].filter == b[u].filter && a[u].address == b[u].address &&
                       a[u].border == b[u].border && a[u].rt_alias == b[u].rt_alias && a[u].rt_depth == b[u].rt_depth &&
                       (a[u].data == nullptr) == (b[u].data == nullptr) &&
                       (a[u].content_id == 0) == (b[u].content_id == 0);
                if (a[u].data != nullptr && b[u].data != nullptr) same = same && *a[u].data == *b[u].data;
                off_descs.push_back(&a[u]);
                on_descs.push_back(&b[u]);
            }
        }
        CHECK(same);
        // Two descriptors share a content id in one run exactly when they share it in the other.
        bool ids_match = off_descs.size() == on_descs.size();
        for (std::size_t i = 0; ids_match && i < off_descs.size(); i++) {
            for (std::size_t j = i + 1; j < off_descs.size(); j++) {
                if ((off_descs[i]->content_id == off_descs[j]->content_id) !=
                    (on_descs[i]->content_id == on_descs[j]->content_id)) {
                    ids_match = false;
                    break;
                }
            }
        }
        CHECK(ids_match);
    }
}

// The calling thread's note count moves by one per note, ignores other threads and survives a census reset.
void testRsxUnimplementedThreadNotes() {
    const std::uint64_t start = tox2::rsxUnimplementedThreadNotes();
    tox2::rsxNoteUnimplemented("state", "thread notes a");
    CHECK(tox2::rsxUnimplementedThreadNotes() == start + 1);
    tox2::rsxNoteUnimplemented("state", "thread notes a");  // a repeated kind counts too
    tox2::rsxNoteUnimplemented("state", "thread notes b");
    CHECK(tox2::rsxUnimplementedThreadNotes() == start + 3);

    std::uint64_t other_notes = 0;
    std::thread other([&] {
        for (int i = 0; i < 5; i++) tox2::rsxNoteUnimplemented("state", "thread notes other");
        other_notes = tox2::rsxUnimplementedThreadNotes();
    });
    other.join();
    CHECK(other_notes == 5);
    CHECK(tox2::rsxUnimplementedThreadNotes() == start + 3);

    tox2::rsxUnimplementedReset();
    CHECK(tox2::rsxUnimplementedThreadNotes() == start + 3);
}

// The validity interface the builder uses to decide that a descriptor it took earlier is still current: epoch,
// write serial, overlapping writes since a serial, and the entry-set generation.
void testRsxTextureCacheValidity() {
    tox2::RsxTextureLayout layout;
    layout.guest_bytes = 100;
    layout.levels.resize(1);
    const auto blob = [] { return std::make_shared<const std::vector<std::uint8_t>>(100); };
    const auto key = [](std::uint32_t offset) {
        tox2::RsxTextureCache::Key k;
        k.offset = offset;
        return k;
    };

    // Epoch and write serial.
    {
        tox2::RsxTextureCache cache;
        const std::uint64_t epoch = cache.epoch();
        cache.beginEpoch();
        CHECK(cache.epoch() == epoch + 1);
        cache.onFlip(1);
        CHECK(cache.epoch() == epoch + 2);

        const std::uint64_t serial = cache.writeSerial();
        cache.noteWrite(0, 100, 200);
        CHECK(cache.writeSerial() == serial + 1);
        cache.noteWrite(0, 100, 200);  // the same range again still moves the serial
        CHECK(cache.writeSerial() == serial + 2);
        cache.noteWrite(0, 300, 300);  // an empty range is no write
        cache.noteWrite(0, 400, 300);
        CHECK(cache.writeSerial() == serial + 2);
        cache.noteWrite(1, 0, 10);
        CHECK(cache.writeSerial() == serial + 3);
    }

    // writtenSince.
    {
        tox2::RsxTextureCache cache;
        cache.noteWrite(0, 100, 200);
        const std::uint64_t first = cache.writeSerial();
        CHECK(!cache.writtenSince(0, 100, 200, first));  // a write at the serial is not after it
        CHECK(cache.writtenSince(0, 100, 200, first - 1));
        CHECK(cache.writtenSince(0, 0, 101, first - 1));  // partial overlaps count
        CHECK(cache.writtenSince(0, 199, 500, first - 1));
        CHECK(!cache.writtenSince(0, 0, 100, first - 1));  // ranges that only touch do not overlap
        CHECK(!cache.writtenSince(0, 200, 300, first - 1));
        CHECK(!cache.writtenSince(1, 100, 200, first - 1));  // another location
        cache.noteWrite(0, 150, 160);
        CHECK(cache.writtenSince(0, 100, 200, first));
        CHECK(!cache.writtenSince(0, 100, 150, first));
        CHECK(!cache.writtenSince(0, 100, 200, cache.writeSerial()));
        cache.beginEpoch();
        CHECK(!cache.writtenSince(0, 100, 200, 0));
    }

    // fresh() agrees with the pieces it is made of.
    {
        tox2::RsxTextureCache cache;
        const tox2::RsxTextureCache::Entry& entry = cache.store(key(100), layout, 1, blob());
        CHECK(cache.fresh(entry));
        cache.noteWrite(0, 0, 100);
        CHECK(cache.fresh(entry));
        cache.noteWrite(0, 150, 160);
        CHECK(!cache.fresh(entry));
        cache.beginEpoch();
        CHECK(!cache.fresh(entry));
    }

    // generation: created, replaced or dropped entries move it; lookups, freshness and writes do not.
    {
        tox2::RsxTextureCache cache(250);
        std::uint64_t generation = cache.generation();
        cache.store(key(0), layout, 1, blob());
        CHECK(cache.generation() > generation);
        generation = cache.generation();
        cache.store(key(0), layout, 2, blob());  // replaced in place
        CHECK(cache.generation() > generation);
        generation = cache.generation();

        tox2::RsxTextureCache::Entry* found = cache.find(key(0));
        CHECK(found != nullptr);
        cache.markFresh(*found);
        cache.noteWrite(0, 0, 10);
        cache.beginEpoch();
        CHECK(cache.find(key(500)) == nullptr);  // a missing key
        CHECK(cache.generation() == generation);

        cache.erase(key(500));  // nothing to erase
        CHECK(cache.generation() == generation);
        cache.erase(key(0));
        CHECK(cache.generation() > generation);
        CHECK(cache.entryCount() == 0);

        // A store that goes over the budget drops the least recently used entry.
        cache.store(key(0), layout, 1, blob());
        cache.store(key(100), layout, 2, blob());
        generation = cache.generation();
        cache.store(key(200), layout, 3, blob());
        CHECK(cache.entryCount() == 2 && cache.generation() > generation);
        generation = cache.generation();

        // A flip that is no sweep leaves the entries alone; the idle sweep drops both.
        cache.onFlip(1);
        CHECK(cache.entryCount() == 2 && cache.generation() == generation);
        cache.onFlip((tox2::RsxTextureCache::kIdleFlips / 32 + 2) * 32);
        CHECK(cache.entryCount() == 0 && cache.generation() > generation);
    }

    // A sweep flip that only trims moves the generation. The store keeps the entry it just made even over the
    // budget, so the next sweep is the one that drops it.
    {
        tox2::RsxTextureCache cache(50);
        cache.store(key(0), layout, 1, blob());
        CHECK(cache.entryCount() == 1);
        const std::uint64_t generation = cache.generation();
        cache.onFlip(1);
        CHECK(cache.entryCount() == 1 && cache.generation() == generation);
        cache.onFlip(32);
        CHECK(cache.entryCount() == 0 && cache.generation() > generation);
    }
}

// rsxHashBytes without a copy: the streaming hash and the memory views' hashRange give the hash of the
// concatenated bytes however the range is split into chunks or pages.
void testRsxHashRange() {
    std::vector<std::uint8_t> bytes(0x6000);
    std::uint32_t x = 12345;
    for (std::uint8_t& b : bytes) {
        x = x * 1664525u + 1013904223u;
        b = static_cast<std::uint8_t>(x >> 24);
    }
    const auto hashOf = [&](std::size_t begin, std::size_t size) {
        return tox2::rsxHashBytes(std::span<const std::uint8_t>(bytes).subspan(begin, size));
    };

    // The stream is invariant under the split points, including empty pieces and pieces of one byte.
    {
        bool same = true;
        for (std::size_t length : {0, 1, 63, 64, 65, 240, 241, 256, 257, 1000, 4096, 5000}) {
            for (std::size_t first : {0, 1, 17, 64, 255, 256, 257, 3000}) {
                const std::size_t cut = std::min(first, length);
                tox2::RsxHashStream stream;
                stream.update(std::span<const std::uint8_t>(bytes).first(cut));
                stream.update({});
                stream.update(std::span<const std::uint8_t>(bytes).subspan(cut, length - cut));
                same = same && stream.digest() == hashOf(0, length);
            }
        }
        tox2::RsxHashStream bytewise;
        for (std::size_t i = 0; i < 700; i++) bytewise.update(std::span<const std::uint8_t>(bytes).subspan(i, 1));
        CHECK(same && bytewise.digest() == hashOf(0, 700));
        const tox2::RsxHashStream empty;
        CHECK(empty.digest() == tox2::rsxHashBytes({}));
    }

    // The replay view walks 4 KiB pages: a range over four pages with partial ends, and the failures.
    {
        tox2::rsxUnimplementedReset();
        tox2::ReplayRsxMemoryView view;
        view.add(0, 0x2F00, std::span<const std::uint8_t>(bytes).subspan(0, 0x3200));
        std::uint64_t hash = 0;
        CHECK(view.hashRange(0, 0x2F10, 0x3000, hash) && hash == hashOf(0x10, 0x3000));
        CHECK(view.hashRange(0, 0x3000, 0x1000, hash) && hash == hashOf(0x100, 0x1000));  // one whole page
        CHECK(view.hashRange(0, 0x2F00, 0x3200, hash) && hash == hashOf(0, 0x3200));
        std::vector<std::uint8_t> copy(0x3000);
        CHECK(view.read(0, 0x2F10, copy) && tox2::rsxHashBytes(copy) == hashOf(0x10, 0x3000));
        CHECK(view.hashRange(0, 0x9000, 0, hash) && hash == tox2::rsxHashBytes({}));
        CHECK(unimplementedCount("capture missing range") == 0);
        // Past the recorded end (inside a page that is only partly recorded), in an unrecorded page, in the other
        // location and beyond the 32-bit space: each fails and is counted once, like read().
        CHECK(!view.hashRange(0, 0x2F10, 0x3200, hash) && unimplementedCount("capture missing range") == 1);
        CHECK(!view.hashRange(0, 0x1000, 16, hash) && unimplementedCount("capture missing range") == 2);
        CHECK(!view.hashRange(1, 0x3000, 16, hash) && unimplementedCount("capture missing range") == 3);
        CHECK(!view.hashRange(0, 0xFFFFFFF0u, 0x20, hash) && unimplementedCount("capture missing range") == 4);
        tox2::rsxUnimplementedReset();
    }

    // A view that only implements read() gets the copying default.
    {
        class PlainMemory final : public tox2::RsxMemoryView {
        public:
            explicit PlainMemory(const std::vector<std::uint8_t>& data) : data_(data) {}
            bool read(std::uint32_t, std::uint32_t offset, std::span<std::uint8_t> out) override {
                if (offset > data_.size() || out.size() > data_.size() - offset) return false;
                std::memcpy(out.data(), data_.data() + offset, out.size());
                return true;
            }

        private:
            const std::vector<std::uint8_t>& data_;
        };
        PlainMemory plain(bytes);
        std::uint64_t hash = 0;
        CHECK(plain.hashRange(0, 0x123, 0x2000, hash) && hash == hashOf(0x123, 0x2000));
        CHECK(!plain.hashRange(0, 0x5F00, 0x200, hash));
    }

    // The recording view hashes in place while no capture is active; with a writer it must see the bytes, so the
    // writer receives them.
    {
        FakeRsxMemory memory;
        std::copy(bytes.begin(), bytes.begin() + 0x1000, memory.local.begin() + 0x100);
        tox2::RsxRecordingMemoryView recording(memory);
        std::uint64_t hash = 0;
        CHECK(recording.hashRange(0, 0x100, 0x1000, hash) && hash == hashOf(0, 0x1000));
        CHECK(memory.hashesAt(0, 0x100) == 1 && memory.readsAt(0, 0x100) == 0);

        auto regs = std::make_unique<tox2::RsxRegisters>();
        regs->reset();
        tox2::RsxCaptureWriter writer(1, *regs, {});
        recording.setWriter(&writer);
        hash = 0;
        CHECK(recording.hashRange(0, 0x100, 0x1000, hash) && hash == hashOf(0, 0x1000));
        CHECK(memory.hashesAt(0, 0x100) == 1 && memory.readsAt(0, 0x100) == 1 && writer.rangeCount() == 1);
        CHECK(!recording.hashRange(0, 0xFFF0, 0x40, hash) && writer.rangeCount() == 1);
    }

    // The live view: local memory is one span; IO memory is made of 1 MiB pages that need not be contiguous in
    // guest memory, and a hash that crosses them equals the hash of what read() copies.
    {
        tox2::resetRsxState();
        tox2::Process& process = tox2::process();
        CHECK(process.memory.reserve());
        std::uint8_t* base = process.memory.base();
        constexpr std::uint32_t kPage = 0x100000;
        constexpr std::uint32_t kEaA = 0x30000000, kEaB = 0x30500000, kEaC = 0x30200000;
        constexpr std::uint32_t kLocal = tox2::kRsxLocalBase;
        CHECK(process.memory.commit(kEaA, kPage) && process.memory.commit(kEaB, kPage) &&
              process.memory.commit(kEaC, kPage) && process.memory.commit(kLocal + 0x1000, 0x3000));
        // IO pages 0, 1, 2 map to three scattered pages; the fourth is not mapped.
        CHECK(tox2::rsxMapEaIo(kEaA, 0, kPage) == 0 && tox2::rsxMapEaIo(kEaB, kPage, kPage) == 0 &&
              tox2::rsxMapEaIo(kEaC, 2 * kPage, kPage) == 0);
        std::vector<std::uint8_t> pattern(3 * kPage);
        for (std::size_t i = 0; i < pattern.size(); i++) pattern[i] = static_cast<std::uint8_t>((i * 31) ^ (i >> 12));
        std::memcpy(base + kEaA, pattern.data(), kPage);
        std::memcpy(base + kEaB, pattern.data() + kPage, kPage);
        std::memcpy(base + kEaC, pattern.data() + 2 * kPage, kPage);
        for (std::uint32_t i = 0; i < 0x3000; i++) base[kLocal + 0x1000 + i] = static_cast<std::uint8_t>(i * 13 + 5);

        tox2::GuestRsxMemoryView view(base);
        std::uint64_t hash = 0;
        std::vector<std::uint8_t> copy;
        for (const auto& [offset, size] : {std::pair<std::uint32_t, std::size_t>{0, 100},
                                           {kPage - 50, 100},
                                           {kPage - 1, 2},
                                           {123, 3 * kPage - 123},
                                           {kPage, kPage},
                                           {kPage + 7, 2 * kPage - 7}}) {
            copy.assign(size, 0);
            CHECK(view.read(1, offset, copy) && view.hashRange(1, offset, size, hash) &&
                  hash == tox2::rsxHashBytes(copy));
        }
        CHECK(view.hashRange(1, 5, 0, hash) && hash == tox2::rsxHashBytes({}));
        // Past the mapped pages, and over an io page that is not mapped.
        CHECK(!view.hashRange(1, 3 * kPage - 10, 20, hash) && !view.hashRange(1, 4 * kPage, 16, hash));
        CHECK(!view.hashRange(1, 0xFFFFFFF0u, 0x20, hash));

        copy.assign(0x3000, 0);
        CHECK(view.read(0, 0x1000, copy) && view.hashRange(0, 0x1000, 0x3000, hash) &&
              hash == tox2::rsxHashBytes(copy));
        CHECK(view.hashRange(0, 0x1800, 0x100, hash) &&
              hash == tox2::rsxHashBytes(std::span<const std::uint8_t>(copy).subspan(0x800, 0x100)));
        // Not committed, and beyond the end of local memory.
        CHECK(!view.hashRange(0, 0x800, 0x1000, hash) && !view.hashRange(0, 0x3800, 0x1000, hash));
        CHECK(!view.hashRange(0, tox2::kRsxLocalSize - 8, 16, hash) &&
              !view.hashRange(0, tox2::kRsxLocalSize, 1, hash));

        tox2::resetRsxState();
        process.memory.decommit(kEaA, kPage);
        process.memory.decommit(kEaB, kPage);
        process.memory.decommit(kEaC, kPage);
        process.memory.decommit(kLocal + 0x1000, 0x3000);
    }
}

// RsxMemoryView::checkWrites: each view answers "was anything in this range written since the check that returned
// `since`" without reading the bytes.
void testRsxWriteCheck() {
#if TOX2_RSX_WRITE_WATCH
    constexpr std::uint32_t kPage = 0x1000;

    // Live view over committed local pages.
    {
        tox2::resetRsxState();
        tox2::Process& process = tox2::process();
        CHECK(process.memory.reserve());
        std::uint8_t* base = process.memory.base();
        constexpr std::uint32_t kOffset = 0x09000000;
        constexpr std::uint32_t kEa = tox2::kRsxLocalBase + kOffset;
        CHECK(process.memory.commit(kEa, 8 * kPage));

        tox2::GuestRsxMemoryView view(base);
        const auto first = view.checkWrites(0, kOffset, 4 * kPage, 0);
        CHECK(first.tracked && !first.clean && first.stamp != 0);
        // Nothing was written since, whichever stamp-0 check asks again.
        CHECK(!view.checkWrites(0, kOffset, 4 * kPage, 0).clean);
        const auto second = view.checkWrites(0, kOffset, 4 * kPage, first.stamp);
        CHECK(second.tracked && second.clean && second.stamp > first.stamp);

        tox2Write32(base, kEa + 2 * kPage + 0x10, 0x01020304u);
        const auto after_write = view.checkWrites(0, kOffset, 4 * kPage, second.stamp);
        CHECK(after_write.tracked && !after_write.clean);
        // The report is consumed, so a check made with the stamp just returned is clean again.
        CHECK(view.checkWrites(0, kOffset, 4 * kPage, after_write.stamp).clean);

        // A write outside the range does not dirty it; the neighbour page does see its own write.
        const auto head = view.checkWrites(0, kOffset, kPage, 0);
        tox2Write32(base, kEa + kPage, 5);
        CHECK(view.checkWrites(0, kOffset, kPage, head.stamp).clean);
        CHECK(!view.checkWrites(0, kOffset + kPage, kPage, head.stamp).clean);

        // A mark the OS does not report.
        const auto before_mark = view.checkWrites(0, kOffset, 8 * kPage, 0);
        process.memory.markWritten(kEa + 5 * kPage + 7, 1);
        CHECK(!view.checkWrites(0, kOffset, 8 * kPage, before_mark.stamp).clean);

        // A page written during one texture's check is still seen by another texture on the same page.
        const auto texture_a = view.checkWrites(0, kOffset, 2 * kPage, 0);
        const auto texture_b = view.checkWrites(0, kOffset + kPage, 2 * kPage, 0);
        tox2Write32(base, kEa + kPage + 8, 6);
        CHECK(!view.checkWrites(0, kOffset, 2 * kPage, texture_a.stamp).clean);
        CHECK(!view.checkWrites(0, kOffset + kPage, 2 * kPage, texture_b.stamp).clean);

        // Another location is not tracked, and a range that is out of local memory or not committed is tracked but
        // never clean.
        CHECK(!view.checkWrites(1, 0, kPage, 1).tracked);
        const auto beyond = view.checkWrites(0, tox2::kRsxLocalSize - 8, 16, 1);
        CHECK(beyond.tracked && !beyond.clean);
        const auto uncommitted = view.checkWrites(0, kOffset + 6 * kPage, 4 * kPage, 1);
        CHECK(uncommitted.tracked && !uncommitted.clean);
        CHECK(!view.checkWrites(0, kOffset + 6 * kPage, 4 * kPage, uncommitted.stamp).clean);

        // Two views: the second one consumes the OS report, the first still sees the write.
        tox2::GuestRsxMemoryView view_a(base), view_b(base);
        const auto a = view_a.checkWrites(0, kOffset, 4 * kPage, 0);
        CHECK(view_a.checkWrites(0, kOffset, 4 * kPage, a.stamp).clean);
        tox2Write32(base, kEa + 3 * kPage, 7);
        const auto b = view_b.checkWrites(0, kOffset, 4 * kPage, 0);
        CHECK(!b.clean);
        CHECK(!view_a.checkWrites(0, kOffset, 4 * kPage, a.stamp).clean);

        // The recording view forwards while no capture is active and tracks nothing while one is.
        tox2::RsxRecordingMemoryView recording(view);
        const auto r0 = recording.checkWrites(0, kOffset, 4 * kPage, 0);
        CHECK(r0.tracked && !r0.clean);
        CHECK(recording.checkWrites(0, kOffset, 4 * kPage, r0.stamp).clean);
        auto regs = std::make_unique<tox2::RsxRegisters>();
        regs->reset();
        tox2::RsxCaptureWriter writer(1, *regs, {});
        recording.setWriter(&writer);
        CHECK(!recording.checkWrites(0, kOffset, 4 * kPage, r0.stamp).tracked);
        recording.setWriter(nullptr);
        CHECK(recording.checkWrites(0, kOffset, 4 * kPage, r0.stamp).tracked);

        tox2::resetRsxState();
        process.memory.decommit(kEa, 8 * kPage);
    }

    // Replay view: the bytes added are the writes.
    {
        std::vector<std::uint8_t> bytes(0x4000);
        std::uint32_t x = 99;
        for (std::uint8_t& b : bytes) {
            x = x * 1664525u + 1013904223u;
            b = static_cast<std::uint8_t>(x >> 24);
        }
        const auto add = [&](tox2::ReplayRsxMemoryView& view, std::uint32_t location, std::uint32_t offset,
                             std::size_t size) {
            view.add(location, offset, std::span<const std::uint8_t>(bytes).first(size));
        };

        tox2::ReplayRsxMemoryView view;
        add(view, 0, 0x1000, 0x2000);  // pages 1 and 2
        const auto first = view.checkWrites(0, 0x1000, 0x2000, 0);
        CHECK(first.tracked && !first.clean && first.stamp != 0);

        // The same bytes again change nothing.
        add(view, 0, 0x1000, 0x2000);
        const auto same = view.checkWrites(0, 0x1000, 0x2000, first.stamp);
        CHECK(same.tracked && same.clean && same.stamp == first.stamp);

        // One changed byte in page 2 dirties the range that holds it and not the range on page 1 only.
        bytes[0x1010] ^= 0x5A;
        view.add(0, 0x2000, std::span<const std::uint8_t>(bytes).subspan(0x1000, 0x1000));
        bytes[0x1010] ^= 0x5A;
        CHECK(!view.checkWrites(0, 0x1000, 0x2000, first.stamp).clean);
        CHECK(view.checkWrites(0, 0x1000, 0x1000, first.stamp).clean);
        const auto changed = view.checkWrites(0, 0x1000, 0x2000, first.stamp);
        CHECK(view.checkWrites(0, 0x1000, 0x2000, changed.stamp).clean);

        // New bytes in an unrecorded part of a recorded page.
        add(view, 0, 0x4000, 0x800);
        const auto half = view.checkWrites(0, 0x4000, 0x800, 0);
        CHECK(!half.clean && !view.checkWrites(0, 0x4000, 0x1000, half.stamp).clean);
        view.add(0, 0x4800, std::span<const std::uint8_t>(bytes).subspan(0x800, 0x800));
        CHECK(!view.checkWrites(0, 0x4000, 0x800, half.stamp).clean);
        const auto whole = view.checkWrites(0, 0x4000, 0x1000, 0);
        CHECK(view.checkWrites(0, 0x4000, 0x1000, whole.stamp).clean);

        // A missing page, or a range that runs into one, is tracked and never clean.
        const auto missing = view.checkWrites(0, 0x9000, 16, first.stamp);
        CHECK(missing.tracked && !missing.clean);
        CHECK(!view.checkWrites(0, 0x2000, 0x2000, changed.stamp).clean);
        CHECK(!view.checkWrites(0, 0xFFFFFFF0u, 0x20, changed.stamp).clean);

        // Location 1 is tracked, location 2 is not.
        add(view, 1, 0x10000, 0x1000);
        const auto io = view.checkWrites(1, 0x10000, 0x1000, 0);
        CHECK(io.tracked && !io.clean);
        CHECK(view.checkWrites(1, 0x10000, 0x1000, io.stamp).clean);
        CHECK(!view.checkWrites(2, 0, 16, 1).tracked);
    }
#endif
}

// A frame builder over any memory view, with the register helpers the hash skip tests share. The fragment program
// ends at its first word, at `fp_offset`; every texture is a linear A8R8G8B8 4x4 image (64 guest bytes).
struct RsxSkipHarness {
    static constexpr std::uint32_t kTextureBytes = 64;
    RsxSkipHarness(tox2::RsxMemoryView& view, std::uint32_t fp_offset)
        : regs(std::make_unique<tox2::RsxRegisters>()), fp(fp_offset), builder(view) {
        regs->reset();
    }
    void write(std::uint32_t method, std::uint32_t value) {
        regs->apply(method, value);
        builder.onWrite(*regs, method, value);
    }
    void flip() {
        builder.onFlip(*regs, 0, {});
        regs->contextReset();
    }
    void setSurface(std::uint32_t offset, std::uint32_t pitch, std::uint32_t height) {
        write(0x0200, 16u << 16);
        write(0x0204, height << 16);
        write(0x0208, 8u | (2u << 5) | (1u << 8));
        write(0x020C, pitch);
        write(0x0210, offset);
        write(0x0214, 0x900000);
        write(0x022C, pitch);
        write(0x0220, 1);
        write(0x08C0, 16u << 16);
        write(0x08C4, height << 16);
    }
    void setTexture(std::uint32_t unit, std::uint32_t offset) {
        write(0x1A00 + unit * 0x20, offset);
        write(0x1A04 + unit * 0x20, 1u | (2u << 4) | (0xA5u << 8) | (1u << 16));
        write(0x1A0C + unit * 0x20, 0x80000000);
        write(0x1A18 + unit * 0x20, (4u << 16) | 4u);
        write(0x1840 + unit * 4, 16);
    }
    void clear(std::uint32_t mask) {
        write(0x1D90, 0xFF102030);
        write(0x1D8C, 0xFFFFFF00);
        write(0x1D94, mask);
    }
    // Textures at `offsets` in units 0, 1, ...
    void bind(std::initializer_list<std::uint32_t> offsets) {
        setSurface(0x100000, 64, 4);
        std::uint32_t unit = 0;
        for (const std::uint32_t offset : offsets) setTexture(unit++, offset);
    }
    // A flip (a new epoch and a safety-net tick), then the registers it cleared.
    void newFrame(std::initializer_list<std::uint32_t> offsets) {
        flip();
        bind(offsets);
    }
    void newEpoch() { write(tox2::kMethodNv406eSemaphoreAcquire, 0); }
    tox2::RsxDraw draw() {
        write(0x08E4, fp | 1);
        write(0x1808, 5);
        write(0x1814, 2u << 24);
        write(0x1808, 0);
        tox2::RsxDraw result;
        int draws = 0;
        for (const tox2::RsxCommand& command : builder.takeCommands()) {
            if (const auto* found = std::get_if<tox2::RsxDraw>(&command)) {
                result = *found;
                draws++;
            }
        }
        CHECK(draws == 1);
        return result;
    }

    std::unique_ptr<tox2::RsxRegisters> regs;
    std::uint32_t fp;
    tox2::RsxFrameBuilder builder;
};

// The bytes of a replay view kept in a plain array, so a test can change some and publish them like a capture does.
// 0x2000 holds the fragment program word; the textures sit in [0x3000, 0x6000).
struct RsxReplayImage {
    RsxReplayImage() : bytes(0x8000) {
        bytes[0x2001] = 0x01;  // the word 0x00010000, big-endian
        publish(0x2000, 0x4000);
    }
    void fill(std::uint32_t offset, std::uint8_t seed) {
        for (std::uint32_t i = 0; i < RsxSkipHarness::kTextureBytes; i++) {
            bytes[offset + i] = static_cast<std::uint8_t>(i + seed);
        }
    }
    void publish(std::uint32_t offset, std::size_t size = RsxSkipHarness::kTextureBytes) {
        view.add(0, offset, std::span<const std::uint8_t>(bytes).subspan(offset, size));
    }
    void set(std::uint32_t offset, std::uint8_t seed) {
        fill(offset, seed);
        publish(offset);
    }
    tox2::ReplayRsxMemoryView view;
    std::vector<std::uint8_t> bytes;
};

#if TOX2_RSX_WRITE_WATCH
// Forwards to another view and reports what the test wants about writes.
class RsxForwardingView final : public tox2::RsxMemoryView {
public:
    enum class Mode { Untracked, AlwaysClean };
    RsxForwardingView(tox2::RsxMemoryView& inner, Mode mode) : inner_(inner), mode_(mode) {}
    bool read(std::uint32_t location, std::uint32_t offset, std::span<std::uint8_t> out) override {
        return inner_.read(location, offset, out);
    }
    bool tryRead(std::uint32_t location, std::uint32_t offset, std::span<std::uint8_t> out) override {
        return inner_.tryRead(location, offset, out);
    }
    bool hashRange(std::uint32_t location, std::uint32_t offset, std::size_t size, std::uint64_t& out) override {
        return inner_.hashRange(location, offset, size, out);
    }
    // AlwaysClean claims no write ever happened after a first check: the writer a view misses.
    tox2::RsxWriteCheck checkWrites(std::uint32_t, std::uint32_t, std::size_t, std::uint64_t since) override {
        if (mode_ == Mode::Untracked) return {};
        return {true, since != 0, ++serial_};
    }

private:
    tox2::RsxMemoryView& inner_;
    Mode mode_;
    std::uint64_t serial_ = 0;
};

bool sameDescriptorExceptId(const tox2::RsxTextureDesc& a, const tox2::RsxTextureDesc& b) {
    return a.unit == b.unit && a.format == b.format && a.mips == b.mips && a.dimension == b.dimension &&
           a.linear_layout == b.linear_layout && a.normalized == b.normalized && a.cubemap == b.cubemap &&
           a.depth == b.depth && a.location == b.location && a.offset == b.offset && a.pitch == b.pitch &&
           a.width == b.width && a.height == b.height && a.control0 == b.control0 && a.remap == b.remap &&
           a.filter == b.filter && a.address == b.address && a.border == b.border && a.rt_alias == b.rt_alias &&
           a.rt_depth == b.rt_depth && (a.data == nullptr) == (b.data == nullptr) &&
           (a.content_id == 0) == (b.content_id == 0) && (a.data == nullptr || *a.data == *b.data);
}

struct RsxSkipRun {
    std::vector<tox2::RsxTextureDesc> descs;  // every descriptor of every draw, in order
    std::size_t draws = 0;
    std::uint64_t validations = 0, skips = 0, safety_hashes = 0, unreported = 0;
};

// One scripted sequence of draws: repeats, changed and rewritten textures between epochs, a change inside an epoch
// (which no validation sees), a render target over a cached range and several flips (so that the safety net comes
// round). `tracked` picks the replay view itself or a view that reports nothing.
RsxSkipRun runRsxSkipScript(bool tracked) {
    constexpr std::uint32_t kA = 0x3000, kB = 0x4000, kC = 0x5000;
    tox2::rsxUnimplementedReset();
    RsxReplayImage image;
    image.set(kA, 1);
    image.set(kB, 2);
    image.set(kC, 3);
    RsxForwardingView untracked(image.view, RsxForwardingView::Mode::Untracked);
    tox2::RsxMemoryView& view = tracked ? static_cast<tox2::RsxMemoryView&>(image.view) : untracked;
    RsxSkipHarness h(view, 0x2000);
    h.builder.setHashCheckFlips(4);
    RsxSkipRun run;
    const auto step = [&] {
        const tox2::RsxDraw draw = h.draw();
        run.draws++;
        for (const tox2::RsxTextureDesc& texture : draw.textures) run.descs.push_back(texture);
    };
    h.bind({kA, kB});
    step();  // 1: first use
    step();  // 2: same epoch
    h.newEpoch();
    step();  // 3: unchanged
    image.set(kA, 9);
    h.newEpoch();
    step();  // 4: A changed
    h.newFrame({kA, kB});
    step();  // 5
    image.set(kB, 7);
    step();  // 6: B changed inside the epoch, nothing looks yet
    h.newEpoch();
    step();             // 7: B new
    image.publish(kA);  // the same bytes again
    h.newEpoch();
    step();  // 8
    h.newFrame({kA, kB, kC});
    step();            // 9: C joins
    image.set(kA, 1);  // back to the first bytes: a new blob, not the old id
    h.newFrame({kA, kC});
    step();  // 10
    h.setSurface(kC - 0x40, 64, 4);
    h.clear(0xF3);
    step();  // 11: a target over C
    for (int frame = 0; frame < 4; frame++) {
        h.newFrame({kA, kB, kC});
        step();  // 12-15
    }
    const tox2::RsxBuilderStats& stats = h.builder.stats();
    run.validations = stats.texture_validations;
    run.skips = stats.texture_hash_skips;
    run.safety_hashes = stats.texture_safety_hashes;
    run.unreported = stats.texture_unreported_changes;
    return run;
}
#endif

// RsxFrameBuilder::copyTexture with write tracking: a range the memory view reports unwritten keeps its entry without
// being hashed. With the switch off nothing is skipped.
void testRsxTextureHashSkip() {
    constexpr std::uint32_t kA = 0x3000, kBytes = RsxSkipHarness::kTextureBytes;

    // (a) The same bytes added again between two frames. With tracking the second draw validates by a write check
    // alone and keeps the blob and the id; without it the entry is hashed in place.
    {
        tox2::rsxUnimplementedReset();
        RsxReplayImage image;
        image.set(kA, 1);
        RsxSkipHarness h(image.view, 0x2000);
        h.bind({kA});
        const tox2::RsxDraw first = h.draw();
        CHECK(first.textures.size() == 1 && first.textures[0].data != nullptr && first.textures[0].content_id != 0);
        h.newFrame({kA});
        image.publish(kA);
        const tox2::RsxBuilderStats before = h.builder.stats();
        const tox2::RsxDraw second = h.draw();
        const tox2::RsxBuilderStats& after = h.builder.stats();
        CHECK(second.textures.size() == 1 && second.textures[0].content_id == first.textures[0].content_id);
        CHECK(second.textures[0].data == first.textures[0].data);
        CHECK(after.texture_validations == before.texture_validations + 1);
        CHECK(after.texture_bytes_read == before.texture_bytes_read);
#if TOX2_RSX_WRITE_WATCH
        CHECK(after.texture_hash_skips == before.texture_hash_skips + 1);
        CHECK(after.texture_bytes_hashed == before.texture_bytes_hashed);
        CHECK(after.texture_safety_hashes == 0 && after.texture_unreported_changes == 0);
#else
        // (h) Off: every validation hashes.
        CHECK(after.texture_hash_skips == 0 && after.texture_safety_hashes == 0 &&
              after.texture_unreported_changes == 0);
        CHECK(after.texture_bytes_hashed == before.texture_bytes_hashed + kBytes);
#endif
    }

#if TOX2_RSX_WRITE_WATCH
    // (b) Changed bytes added between frames, with no write the RSX could see: the memory view still reports them.
    {
        tox2::rsxUnimplementedReset();
        RsxReplayImage image;
        image.set(kA, 1);
        RsxSkipHarness h(image.view, 0x2000);
        h.bind({kA});
        const tox2::RsxDraw first = h.draw();
        h.newFrame({kA});
        image.set(kA, 40);
        const tox2::RsxDraw second = h.draw();
        CHECK(second.textures[0].content_id != first.textures[0].content_id);
        CHECK(texByte(second.textures[0], 0) == 40 && texByte(first.textures[0], 0) == 1);
        const tox2::RsxBuilderStats& stats = h.builder.stats();
        CHECK(stats.texture_hash_skips == 0 && stats.texture_unreported_changes == 0);
        CHECK(stats.texture_bytes_hashed == kBytes && stats.texture_validations == 1);
        // The new bytes are the baseline from now on.
        h.newFrame({kA});
        const tox2::RsxDraw third = h.draw();
        CHECK(third.textures[0].content_id == second.textures[0].content_id && stats.texture_hash_skips == 1);
    }

    // (c) The game view: a store between frames is seen, a texture nobody wrote is skipped.
    {
        tox2::rsxUnimplementedReset();
        tox2::resetRsxState();
        tox2::Process& process = tox2::process();
        CHECK(process.memory.reserve());
        std::uint8_t* base = process.memory.base();
        constexpr std::uint32_t kPage = 0x1000;
        constexpr std::uint32_t kOffset = 0x0A000000;
        constexpr std::uint32_t kEa = tox2::kRsxLocalBase + kOffset;
        constexpr std::uint32_t kTexA = kOffset + 2 * kPage, kTexB = kOffset + 4 * kPage;
        CHECK(process.memory.commit(kEa, 8 * kPage));
        tox2Write32(base, kEa, 0x00010000u);
        for (std::uint32_t i = 0; i < kBytes; i += 4) {
            tox2Write32(base, tox2::kRsxLocalBase + kTexA + i, 0x01010101u * (i / 4 + 1));
            tox2Write32(base, tox2::kRsxLocalBase + kTexB + i, 0x01010101u * (i / 4 + 20));
        }

        tox2::GuestRsxMemoryView view(base);
        RsxSkipHarness h(view, kOffset);
        h.bind({kTexA, kTexB});
        const tox2::RsxDraw first = h.draw();
        CHECK(first.textures.size() == 2 && first.textures[0].data != nullptr && first.textures[1].data != nullptr);
        h.newFrame({kTexA, kTexB});
        const tox2::RsxDraw second = h.draw();
        const tox2::RsxBuilderStats& stats = h.builder.stats();
        CHECK(stats.texture_hash_skips == 2 && stats.texture_bytes_hashed == 0 && stats.texture_validations == 2);
        CHECK(second.textures[0].content_id == first.textures[0].content_id &&
              second.textures[1].content_id == first.textures[1].content_id);

        tox2Write32(base, tox2::kRsxLocalBase + kTexA + 8, 0xFFEEDDCCu);
        h.newFrame({kTexA, kTexB});
        const tox2::RsxDraw third = h.draw();
        CHECK(third.textures[0].content_id != second.textures[0].content_id);
        CHECK(third.textures[1].content_id == second.textures[1].content_id);
        CHECK(texByte(third.textures[0], 8) == 0xFF && texByte(third.textures[0], 11) == 0xCC);
        CHECK(stats.texture_hash_skips == 3 && stats.texture_bytes_hashed == kBytes);
        CHECK(stats.texture_unreported_changes == 0);

        tox2::resetRsxState();
        process.memory.decommit(kEa, 8 * kPage);
    }

    // (d) A view that never reports a write. The safety net hashes the entry on one flip in every N, so a change
    // shows within N flips; with the net off the stale blob stays.
    {
        tox2::rsxUnimplementedReset();
        FakeRsxMemory memory;
        memory.putWord(0, 0x2000, 0x00010000);
        for (std::uint32_t i = 0; i < kBytes; i++) memory.local[kA + i] = static_cast<std::uint8_t>(i + 1);
        RsxForwardingView view(memory, RsxForwardingView::Mode::AlwaysClean);
        RsxSkipHarness h(view, 0x2000);
        h.builder.setHashCheckFlips(2);
        h.bind({kA});
        const tox2::RsxDraw first = h.draw();
        for (std::uint32_t i = 0; i < kBytes; i++) memory.local[kA + i] = static_cast<std::uint8_t>(i + 40);
        tox2::RsxDraw latest = first;
        int frames = 0;
        while (frames < 2 && latest.textures[0].content_id == first.textures[0].content_id) {
            h.newFrame({kA});
            latest = h.draw();
            frames++;
        }
        const tox2::RsxBuilderStats& stats = h.builder.stats();
        CHECK(frames == 2 && latest.textures[0].content_id != first.textures[0].content_id);
        CHECK(texByte(latest.textures[0], 0) == 40);
        CHECK(stats.texture_unreported_changes == 1 && stats.texture_safety_hashes == 1 &&
              stats.texture_hash_skips == 1);
    }
    {
        tox2::rsxUnimplementedReset();
        FakeRsxMemory memory;
        memory.putWord(0, 0x2000, 0x00010000);
        for (std::uint32_t i = 0; i < kBytes; i++) memory.local[kA + i] = static_cast<std::uint8_t>(i + 1);
        RsxForwardingView view(memory, RsxForwardingView::Mode::AlwaysClean);
        RsxSkipHarness h(view, 0x2000);
        h.builder.setHashCheckFlips(0);
        h.bind({kA});
        const tox2::RsxDraw first = h.draw();
        for (std::uint32_t i = 0; i < kBytes; i++) memory.local[kA + i] = static_cast<std::uint8_t>(i + 40);
        for (int frame = 0; frame < 40; frame++) {
            h.newFrame({kA});
            const tox2::RsxDraw stale = h.draw();
            CHECK(stale.textures[0].content_id == first.textures[0].content_id && texByte(stale.textures[0], 0) == 1);
        }
        const tox2::RsxBuilderStats& stats = h.builder.stats();
        CHECK(stats.texture_hash_skips == 40 && stats.texture_safety_hashes == 0 && stats.texture_bytes_hashed == 0);
        CHECK(stats.texture_unreported_changes == 0);
    }

    // (e) A render target drawn over a cached range ends the trust in the entry, but the pages are clean, so the next
    // validation keeps it, as an equal hash does without tracking.
    {
        tox2::rsxUnimplementedReset();
        RsxReplayImage image;
        image.set(kA, 1);
        RsxSkipHarness h(image.view, 0x2000);
        h.bind({kA});
        const tox2::RsxDraw first = h.draw();
        h.newFrame({kA});
        const tox2::RsxDraw second = h.draw();
        const tox2::RsxBuilderStats& stats = h.builder.stats();
        CHECK(stats.texture_hash_skips == 1 && second.textures[0].content_id == first.textures[0].content_id);
        h.setSurface(kA - 0x40, 64, 4);
        h.clear(0xF3);
        const tox2::RsxDraw third = h.draw();
        CHECK(stats.texture_validations == 2 && stats.texture_hash_skips == 2 && stats.texture_bytes_hashed == 0);
        CHECK(third.textures[0].content_id == first.textures[0].content_id &&
              third.textures[0].data == first.textures[0].data);
    }

    // (f) While a capture records, the bytes must be read so that they are recorded: nothing is skipped and the
    // capture holds the texture range.
    {
        tox2::rsxUnimplementedReset();
        RsxReplayImage image;
        image.set(kA, 1);
        tox2::RsxRecordingMemoryView recording(image.view);
        RsxSkipHarness h(recording, 0x2000);
        h.bind({kA});
        h.draw();
        h.newFrame({kA});
        h.draw();
        const tox2::RsxBuilderStats& stats = h.builder.stats();
        CHECK(stats.texture_hash_skips == 1);  // no capture yet: tracked

        auto regs = std::make_unique<tox2::RsxRegisters>();
        regs->reset();
        tox2::RsxCaptureWriter writer(1, *regs, {});
        recording.setWriter(&writer);
        h.newFrame({kA});
        h.draw();
        CHECK(stats.texture_hash_skips == 1 && stats.texture_validations == 2 && stats.texture_bytes_hashed == kBytes);
        recording.setWriter(nullptr);
        tox2::RsxCaptureReader reader;
        std::string error;
        CHECK(reader.parse(writer.finish({}, 0).flatten(), error));
        bool has_texture = false;
        for (const tox2::RsxCaptureReader::Event& event : reader.events()) {
            has_texture = has_texture || (event.kind == tox2::RsxCaptureReader::EventKind::Memory && event.a == 0 &&
                                          event.b == kA && event.size == kBytes);
        }
        CHECK(has_texture);

        // The untracked validation left the entry without a baseline: one more hash, then skips again.
        h.newFrame({kA});
        h.draw();
        CHECK(stats.texture_hash_skips == 1 && stats.texture_bytes_hashed == 2 * kBytes);
        h.newFrame({kA});
        h.draw();
        CHECK(stats.texture_hash_skips == 2 && stats.texture_bytes_hashed == 2 * kBytes);
    }

    // (g) Equivalence: the same script over a view that tracks nothing and over the replay view.
    {
        const RsxSkipRun plain = runRsxSkipScript(false);
        const RsxSkipRun tracked = runRsxSkipScript(true);
        CHECK(plain.draws >= 12 && plain.draws == tracked.draws && plain.descs.size() == tracked.descs.size());
        CHECK(plain.validations == tracked.validations && plain.validations > 0);
        CHECK(plain.skips == 0 && tracked.skips > 0);
        CHECK(tracked.safety_hashes > 0 && plain.unreported == 0 && tracked.unreported == 0);
        bool same = true;
        for (std::size_t i = 0; i < plain.descs.size() && i < tracked.descs.size(); i++) {
            same = same && sameDescriptorExceptId(plain.descs[i], tracked.descs[i]);
        }
        CHECK(same);
        bool classes = true;
        for (std::size_t i = 0; classes && i < plain.descs.size() && i < tracked.descs.size(); i++) {
            for (std::size_t j = i + 1; j < plain.descs.size() && j < tracked.descs.size(); j++) {
                if ((plain.descs[i].content_id == plain.descs[j].content_id) !=
                    (tracked.descs[i].content_id == tracked.descs[j].content_id)) {
                    classes = false;
                    break;
                }
            }
        }
        CHECK(classes);
    }
#endif
}

void testRsxFloatAndDepthSurfaces() {
    auto regs = std::make_unique<tox2::RsxRegisters>();
    regs->reset();
    tox2::rsxUnimplementedReset();
    FakeRsxMemory memory;
    tox2::RsxFrameBuilder builder(memory);
    const auto write = [&](std::uint32_t method, std::uint32_t value) {
        regs->apply(method, value);
        builder.onWrite(*regs, method, value);
    };
    constexpr std::uint32_t kColorOffset = 0x100000, kDepthOffset = 0x900000, kOtherDepthOffset = 0xA00000;

    // SET_SURFACE_FORMAT code 13 is F_X32; the depth surface keeps its own format.
    write(0x0200, 1280u << 16);
    write(0x0204, 720u << 16);
    write(0x0208, 13u | (2u << 5) | (1u << 8));
    write(0x020C, 0x1400);
    write(0x0210, kColorOffset);
    write(0x0214, kDepthOffset);
    write(0x022C, 0x1400);
    write(0x0220, 1);
    write(0x08C0, 1280u << 16);
    write(0x08C4, 720u << 16);
    const tox2::RsxSurfaceState state = tox2::decodeSurfaceState(*regs);
    CHECK(state.has_color && state.color.format == tox2::RsxColorFormat::F_X32 &&
          state.color.depth_format == tox2::RsxDepthFormat::None);
    CHECK(state.depth_format == tox2::RsxDepthFormat::Z24S8 && state.depth.depth_format == tox2::RsxDepthFormat::Z24S8);
    CHECK(unimplementedCount("surface format F_X32") == 0);

    // A color clear of a float surface does nothing (RPCS3 `clear_surface`); the depth and stencil bits of the
    // same clear still apply and register the depth surface, and no color surface is registered.
    write(0x1D90, 0xFF102030);
    write(0x1D8C, 0xFFFFFF00);
    write(0x1D94, 0xF0);
    CHECK(builder.takeCommands().empty() && builder.knownSurfaces().empty());
    write(0x1D94, 0xF3);
    std::vector<tox2::RsxCommand> commands = builder.takeCommands();
    CHECK(commands.size() == 1 && std::get<tox2::RsxClear>(commands[0]).mask == 0x03);
    CHECK(builder.knownSurfaces().size() == 1 && builder.knownSurfaces()[0].offset == kDepthOffset &&
          builder.knownSurfaces()[0].depth_format == tox2::RsxDepthFormat::Z24S8);

    // A draw into the float surface registers it. The depth binding of that draw is not a surface while the
    // draw does not write depth.
    for (std::uint32_t i = 0; i < 96; i++) memory.main[0x1000 + i] = static_cast<std::uint8_t>(i);
    write(0x1740, 2u | (3u << 4) | (12u << 8));
    write(0x1680, 0x80001000);
    write(0x08E4, 0x2000 | 1);
    memory.putWord(0, 0x2000, 0x00010000);
    write(tox2::kMethodNv4097SetColorMask, 0x01010101);
    write(0x0214, kOtherDepthOffset);
    const auto draw = [&] {
        write(0x1808, 5);
        write(0x1814, 2u << 24);
        write(0x1808, 0);
        return builder.takeCommands();
    };
    commands = draw();
    CHECK(commands.size() == 1 && std::holds_alternative<tox2::RsxDraw>(commands[0]));
    CHECK(builder.knownSurfaces().size() == 2 && builder.knownSurfaces()[1].offset == kColorOffset &&
          builder.knownSurfaces()[1].format == tox2::RsxColorFormat::F_X32 &&
          builder.knownSurfaces()[1].depth_format == tox2::RsxDepthFormat::None);

    // Depth test with depth write makes the draw's depth surface known.
    write(tox2::kMethodNv4097SetDepthTestEnable, 1);
    write(tox2::kMethodNv4097SetDepthMask, 1);
    commands = draw();
    CHECK(builder.knownSurfaces().size() == 3 && builder.knownSurfaces()[2].offset == kOtherDepthOffset &&
          builder.knownSurfaces()[2].depth_format == tox2::RsxDepthFormat::Z24S8);

    // Unit 0 reads the float surface as X32_FLOAT (no note, an alias of a color surface), unit 1 reads the
    // depth surface as A8R8G8B8 (an alias of a depth surface), unit 2 is X32_FLOAT at an address that is no
    // surface (counted, nothing to read it from).
    const auto unit = [&](std::uint32_t index, std::uint32_t offset, std::uint32_t format, std::uint32_t width) {
        write(0x1A00 + index * 0x20, offset);
        write(0x1A04 + index * 0x20, 1u | (2u << 4) | (format << 8) | (1u << 16));
        write(0x1A0C + index * 0x20, 0x80000000);
        write(0x1A18 + index * 0x20, (width << 16) | 720u);
        write(0x1840 + index * 4, width * 4);
    };
    unit(0, kColorOffset, 0x9C | 0x20, 1280);
    unit(1, kDepthOffset, 0x85 | 0x20, 2560);
    unit(2, 0x3000, 0x9C | 0x20, 4);
    commands = draw();
    CHECK(commands.size() == 1);
    const tox2::RsxDraw& textured = std::get<tox2::RsxDraw>(commands[0]);
    CHECK(textured.textures.size() == 3);
    CHECK(textured.textures[0].rt_alias == 1 && !textured.textures[0].rt_depth);
    CHECK(textured.textures[1].rt_alias == 0 && textured.textures[1].rt_depth && texSize(textured.textures[1]) == 0);
    CHECK(textured.textures[2].rt_alias == -1 && !textured.textures[2].rt_depth && texSize(textured.textures[2]) == 0);
    CHECK(unimplementedCount("texture format X32_FLOAT_LN_NR") == 1);
    CHECK(tox2::rsxTextureFormatRenderTargetOnly(0x9C | 0x20) && !tox2::rsxTextureFormatRenderTargetOnly(0x85) &&
          !tox2::rsxTextureFormatSampleable(0x9C));

    // Both kinds survive a capture.
    const std::vector<tox2::RsxSurface> known(builder.knownSurfaces().begin(), builder.knownSurfaces().end());
    tox2::RsxCaptureWriter writer(3, *regs, known);
    tox2::RsxCaptureReader reader;
    std::string error;
    CHECK(reader.parse(writer.finish({}, 0).flatten(), error));
    CHECK(reader.surfaces().size() == known.size());
    bool same = reader.surfaces().size() == known.size();
    for (std::size_t i = 0; same && i < known.size(); i++) same = sameSurface(reader.surfaces()[i], known[i]);
    CHECK(same);
    tox2::rsxUnimplementedReset();
}

void testRsxCapture() {
    tox2::rsxUnimplementedReset();
    auto regs = std::make_unique<tox2::RsxRegisters>();
    regs->reset();
    const auto initial = std::make_unique<tox2::RsxRegisters>(*regs);
    FakeRsxMemory memory;
    tox2::RsxRecordingMemoryView recording(memory);
    tox2::RsxFrameBuilder builder(recording);
    tox2::RsxCaptureWriter writer(7, *regs, builder.knownSurfaces());
    builder.setFlipIndex(7);  // the capture header says this frame ends with flip 7
    recording.setWriter(&writer);
    std::vector<tox2::RsxCommand> live;
    const auto write = [&](std::uint32_t method, std::uint32_t value) {
        regs->apply(method, value);
        builder.onWrite(*regs, method, value);
        writer.onWrite(method, value);
    };
    const auto collect = [&]() {
        for (tox2::RsxCommand& command : builder.takeCommands()) live.push_back(std::move(command));
    };

    // A frame with a clear, an indexed draw, a draw sampling the cleared surface and a draw with a
    // texture copied from memory.
    write(0x0200, 1280u << 16);
    write(0x0204, 720u << 16);
    write(0x0208, 8u | (2u << 5) | (1u << 8));
    write(0x020C, 0x1400);
    write(0x0210, 0x100000);
    write(0x0214, 0x900000);
    write(0x022C, 0x1400);
    write(0x0220, 1);
    write(0x08C0, 1280u << 16);
    write(0x08C4, 720u << 16);
    write(0x1D90, 0xFF102030);
    write(0x1D8C, 0xFFFFFF00);
    write(0x1D94, 0xF3);

    for (std::uint32_t i = 0; i < 96; i++) memory.main[0x1000 + i] = static_cast<std::uint8_t>(i * 3);
    write(0x1740, 2u | (3u << 4) | (12u << 8));
    write(0x1680, 0x80001000);
    write(0x08E4, 0x2000 | 1);
    memory.putWord(0, 0x2000, 0x00010000);
    write(0x1D60, 0x40);
    write(0x0B40 + 4, 1);
    const std::uint16_t indices[] = {1, 2, 3, 2};
    for (int i = 0; i < 4; i++) {
        memory.main[0x2000 + i * 2] = static_cast<std::uint8_t>(indices[i] >> 8);
        memory.main[0x2000 + i * 2 + 1] = static_cast<std::uint8_t>(indices[i]);
    }
    write(0x181C, 0x2000);
    write(0x1820, 1u | (1u << 4));
    write(0x1808, 5);
    write(0x1824, 3u << 24);
    write(0x1808, 0);

    for (std::uint32_t i = 0; i < 256; i++) memory.local[0x3000 + i] = static_cast<std::uint8_t>(i ^ 0x5A);
    const std::uint32_t format = 1u | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16);
    write(0x1A00, 0x100000);  // unit 0: alias of the cleared surface
    write(0x1A04, format);
    write(0x1A0C, 0x80000000);
    write(0x1A18, (16u << 16) | 4u);
    write(0x1840, 64);
    write(0x1A00 + 0x20, 0x3000);  // unit 1: bytes
    write(0x1A04 + 0x20, format);
    write(0x1A0C + 0x20, 0x80000000);
    write(0x1A18 + 0x20, (16u << 16) | 4u);
    write(0x1840 + 4, 64);
    write(0x1808, 5);
    write(0x1814, 2u << 24);
    write(0x1808, 0);
    collect();
    CHECK(live.size() == 3 && std::holds_alternative<tox2::RsxClear>(live[0]) &&
          std::holds_alternative<tox2::RsxDraw>(live[1]));
    CHECK(std::get<tox2::RsxDraw>(live[2]).textures.size() == 2 &&
          std::get<tox2::RsxDraw>(live[2]).textures[0].rt_alias == 0);
    CHECK(texSize(std::get<tox2::RsxDraw>(live[2]).textures[1]) != 0);

    // The same draw again: the texture is cached, so the builder reads no guest memory for it, yet the capture
    // has its bytes once and the replay, which starts with an empty cache, rebuilds both draws.
    const int texture_reads = memory.readsAt(0, 0x3000);
    write(0x1808, 5);
    write(0x1814, 2u << 24);
    write(0x1808, 0);
    collect();
    CHECK(live.size() == 4 && memory.readsAt(0, 0x3000) == texture_reads);
    CHECK(std::get<tox2::RsxDraw>(live[3]).textures[1].data == std::get<tox2::RsxDraw>(live[2]).textures[1].data);

    // The flip, as the consumer runs it: the builder and the writer see it, the context registers are
    // reset, and only then does the flip method write itself land.
    tox2::RsxDisplayBuffer display;
    display.offset = 0x400000;
    display.pitch = 0x1400;
    display.width = 1280;
    display.height = 720;
    builder.onFlip(*regs, 1, display);
    writer.onFlip(1);
    regs->contextReset();
    write(tox2::kMethodGcmFlipCommand, 1);

    // The reset zeroed the surface offset and clip. A clear that rewrites the clip and pitch but not the
    // offset must therefore land on offset 0, in the live run and in the replay.
    write(0x0200, 1280u << 16);
    write(0x0204, 720u << 16);
    write(0x0208, 8u | (2u << 5) | (1u << 8));
    write(0x020C, 0x1400);
    write(0x0220, 1);
    write(0x1D94, 0xF3);
    collect();
    CHECK(live.size() == 6 && std::holds_alternative<tox2::RsxFlip>(live[4]) &&
          std::holds_alternative<tox2::RsxClear>(live[5]));
    CHECK(std::get<tox2::RsxClear>(live[5]).surfaces.color.offset == 0);
    CHECK(std::get<tox2::RsxClear>(live[0]).surfaces.color.offset == 0x100000);

    std::array<tox2::RsxDisplayBuffer, tox2::kRsxDisplayBufferCount> displays{};
    displays[1] = display;
    recording.setWriter(nullptr);
    const std::uint64_t live_writes = writer.writeCount();
    const std::uint64_t live_ranges = writer.rangeCount();
    const tox2::RsxCaptureFile capture_file = writer.finish(displays, 2);
    const std::vector<std::uint8_t> file = capture_file.flatten();
    CHECK(capture_file.size() == file.size() && capture_file.tail.size() == 8);

    tox2::RsxCaptureReader reader;
    std::string error;
    CHECK(reader.parse(file, error));
    CHECK(reader.flipIndex() == 7 && reader.displayCount() == 2 && reader.display()[1].offset == 0x400000);
    CHECK(reader.writeCount() == live_writes && reader.rangeCount() == live_ranges && live_ranges > 0);
    CHECK(reader.surfaces().empty());
    CHECK(reader.startRegisters().regs == initial->regs && reader.startRegisters().vp_ucode == initial->vp_ucode);
    const auto replayCommands = [](const tox2::RsxCaptureReader& capture) {
        tox2::ReplayRsxMemoryView view;
        tox2::RsxFrameBuilder replay_builder(view);
        tox2::replayCapture(capture, view, replay_builder);
        return std::pair{replay_builder.takeCommands(),
                         std::vector<tox2::RsxSurface>(replay_builder.knownSurfaces().begin(),
                                                       replay_builder.knownSurfaces().end())};
    };
    const auto [replayed, replay_surfaces] = replayCommands(reader);
    CHECK(replayed.size() == live.size() && std::get<tox2::RsxFlip>(live[4]).flip_index == 7);
    bool same = replayed.size() == live.size();
    for (std::size_t i = 0; same && i < live.size(); i++) same = sameCommand(live[i], replayed[i]);
    CHECK(same);
    CHECK(replay_surfaces.size() == builder.knownSurfaces().size() &&
          std::equal(replay_surfaces.begin(), replay_surfaces.end(), builder.knownSurfaces().begin(), sameSurface));
    CHECK(unimplementedCount("capture missing range") == 0);

    // The file survives a trip through the disk.
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / ("tox2_rsx_capture_test_" + std::to_string(GetCurrentProcessId()));
    const std::filesystem::path path = dir / "nested" / "frame.tox2cap";
    CHECK(tox2::writeCaptureFile(path, capture_file, error));
    tox2::RsxCaptureReader from_disk;
    CHECK(from_disk.load(path, error) && from_disk.events().size() == reader.events().size());
    CHECK(!from_disk.load(dir / "missing.tox2cap", error) && !error.empty());
    std::error_code remove_ec;
    std::filesystem::remove_all(dir, remove_ec);

    // A frame that starts mid-run starts from non-default registers and known surfaces.
    regs->vp_ucode[5] = 0x1234;
    regs->vp_constants[9] = 0xABCD;
    regs->markVpStoresChanged();
    regs->vp_load = 3;
    regs->vp_start = 4;
    regs->constant_load = 6;
    regs->apply(0x1C00 + 2 * 16, 0x3F800000);  // default attribute slot 2 written
    regs->vertex_default_written |= 1u << 9;   // slot 9 written with zeros: only the v2 mask can say so
    const tox2::RsxSurface known{1, 0x2000, 0x800, 64, 32, tox2::RsxColorFormat::A8R8G8B8};
    tox2::RsxCaptureWriter second(11, *regs, std::span<const tox2::RsxSurface>(&known, 1));
    tox2::RsxCaptureReader second_reader;
    const std::vector<std::uint8_t> second_file = second.finish({}, 0).flatten();
    CHECK(second_reader.parse(second_file, error));
    CHECK(second_file[8] == 2);
    CHECK(second_reader.startRegisters().vertex_default_written == regs->vertex_default_written &&
          (regs->vertex_default_written & (1u << 9)) != 0);
    CHECK(second_reader.flipIndex() == 11 && second_reader.events().empty());
    CHECK(second_reader.startRegisters().regs == regs->regs &&
          second_reader.startRegisters().vp_ucode == regs->vp_ucode);
    CHECK(second_reader.startRegisters().vp_constants == regs->vp_constants);
    CHECK(second_reader.startRegisters().vp_load == 3 && second_reader.startRegisters().vp_start == 4 &&
          second_reader.startRegisters().constant_load == 6);
    CHECK(second_reader.surfaces().size() == 1 && sameSurface(second_reader.surfaces()[0], known));

    // A version 1 file has no written mask in its REGS chunk: the same bytes without the mask word and with
    // the version and chunk size of v1 load, and the mask is inferred from the default attribute registers
    // (slot 2 is nonzero; slot 9, written with zeros, cannot be told apart).
    constexpr std::size_t kRegsChunkOffset = 20;  // header, then the REGS chunk: tag, size, data
    constexpr std::size_t kRegsBytesV1 =
            (0x4000 + tox2::kRsxVpInstructionCount * 4 + tox2::kRsxVpConstantCount * 4 + 3) * 4;
    std::vector<std::uint8_t> v1_file = second_file;
    v1_file[8] = 1;
    v1_file.erase(v1_file.begin() + kRegsChunkOffset + 8 + kRegsBytesV1,
                  v1_file.begin() + kRegsChunkOffset + 8 + kRegsBytesV1 + 4);
    v1_file[kRegsChunkOffset + 4] = static_cast<std::uint8_t>(kRegsBytesV1);
    v1_file[kRegsChunkOffset + 5] = static_cast<std::uint8_t>(kRegsBytesV1 >> 8);
    v1_file[kRegsChunkOffset + 6] = static_cast<std::uint8_t>(kRegsBytesV1 >> 16);
    v1_file[kRegsChunkOffset + 7] = static_cast<std::uint8_t>(kRegsBytesV1 >> 24);
    tox2::RsxCaptureReader v1_reader;
    CHECK(v1_reader.parse(v1_file, error));
    CHECK(v1_reader.startRegisters().vertex_default_written == (1u << 2));
    CHECK(v1_reader.startRegisters().regs == regs->regs && v1_reader.flipIndex() == 11);

    // Malformed files are refused, including a version that does not match the REGS chunk size.
    std::vector<std::uint8_t> bad = file;
    bad[0] = 'X';
    CHECK(!reader.parse(bad, error));
    bad = file;
    bad[8] = 3;  // unknown version
    CHECK(!reader.parse(bad, error));
    bad = file;
    bad[8] = 0;
    CHECK(!reader.parse(bad, error));
    bad = file;
    bad[8] = 1;  // v2 chunk size under v1
    CHECK(!reader.parse(bad, error));
    bad = v1_file;
    bad[8] = 2;  // v1 chunk size under v2
    CHECK(!reader.parse(bad, error));
    bad = file;
    bad.resize(bad.size() - 3);
    CHECK(!reader.parse(bad, error));
    bad = file;
    bad.push_back(0);
    CHECK(!reader.parse(bad, error));
    bad = file;
    bad[20] = 'Q';  // first chunk tag
    CHECK(!reader.parse(bad, error));
    bad.assign(file.begin(), file.begin() + 20);
    CHECK(!reader.parse(bad, error));
    CHECK(reader.parse(file, error));

    // The replay view serves only what was recorded, across page borders, and counts the rest.
    tox2::ReplayRsxMemoryView view;
    const std::uint8_t recorded[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    view.add(0, 4092, recorded);
    std::uint8_t out[9] = {};
    CHECK(view.read(0, 4092, std::span<std::uint8_t>(out, 8)) && std::equal(recorded, recorded + 8, out));
    CHECK(view.read(0, 4094, std::span<std::uint8_t>(out, 4)) && out[0] == 3 && out[3] == 6);
    CHECK(unimplementedCount("capture missing range") == 0);
    CHECK(!view.read(0, 4092, std::span<std::uint8_t>(out, 9)));
    CHECK(!view.read(1, 4092, std::span<std::uint8_t>(out, 8)));
    CHECK(!view.read(0, 0xFFFFFFFCu, std::span<std::uint8_t>(out, 8)));
    CHECK(unimplementedCount("capture missing range") == 3);
    tox2::rsxUnimplementedReset();
}

// The in-memory captures of rsx_synthetic.h replay into the commands they describe.
void testRsxSyntheticCaptures() {
    tox2::rsxUnimplementedReset();
    const auto replay = [](const std::vector<std::uint8_t>& file) {
        tox2::RsxCaptureReader reader;
        std::string error;
        CHECK(reader.parse(file, error));
        tox2::ReplayRsxMemoryView view;
        tox2::RsxFrameBuilder builder(view);
        tox2::replayCapture(reader, view, builder);
        return builder.takeCommands();
    };

    std::vector<tox2::RsxCommand> commands = replay(tox2::rsx_synthetic::makeClearCapture(0xFF405060));
    CHECK(commands.size() == 2 && std::holds_alternative<tox2::RsxClear>(commands[0]));
    CHECK(std::get<tox2::RsxClear>(commands[0]).color_argb == 0xFF405060 &&
          std::get<tox2::RsxClear>(commands[0]).mask == 0xF3);
    CHECK(std::get<tox2::RsxClear>(commands[0]).surfaces.color.width == tox2::rsx_synthetic::kSurfaceSize);
    CHECK(std::holds_alternative<tox2::RsxFlip>(commands[1]) && std::get<tox2::RsxFlip>(commands[1]).buffer == 0);

    commands = replay(tox2::rsx_synthetic::makeQuadCapture());
    CHECK(commands.size() == 3 && std::holds_alternative<tox2::RsxDraw>(commands[1]));
    const tox2::RsxDraw& quad = std::get<tox2::RsxDraw>(commands[1]);
    CHECK(quad.primitive == tox2::RsxPrimitive::TriangleStrip && quad.count == 4 && quad.attribs.size() == 2);
    CHECK(quad.attribs[0].slot == 0 && quad.attribs[0].type == 2 && quad.attribs[1].slot == 3 &&
          quad.attribs[1].type == 4);
    CHECK(quad.vertex_data.size() == 4 * 12 + 4 * 4 && quad.textures.empty());
    CHECK(quad.vp_ucode.size() == 4 && quad.fp_ucode.size() == 4);
    CHECK(unimplementedCount("state unterminated vertex program") == 0);
    // -0.5f, big-endian, is the first position component.
    CHECK(quad.vertex_data[0] == 0xBF && quad.vertex_data[1] == 0x00 && quad.vertex_data[2] == 0x00);

    commands = replay(tox2::rsx_synthetic::makeTexturedQuadCapture());
    CHECK(commands.size() == 3 && std::holds_alternative<tox2::RsxDraw>(commands[1]));
    const tox2::RsxDraw& textured = std::get<tox2::RsxDraw>(commands[1]);
    CHECK(textured.textures.size() == 1 && textured.textures[0].rt_alias == -1 && textured.textures[0].width == 2);
    CHECK(texSize(textured.textures[0]) == 16);
    CHECK(texByte(textured.textures[0], 0) == 0xFF && texByte(textured.textures[0], 1) == 0xFF &&
          texByte(textured.textures[0], 2) == 0x00);
    CHECK(unimplementedCount("capture missing range") == 0);

    // A draw carries the branch bits register, and its vertex program runs on past the first end bit
    // when a branch leads there: 0: BRB b1 -> 3, 1: end, 2: stale words, 3: end.
    {
        using namespace tox2::rsx_synthetic;
        SceneBuilder scene;
        scene.setSurface();
        scene.putQuadPositions();
        scene.setFloatAttrib(0, 3, kPositionsOffset);
        Programs programs;
        programs.vertex = {
                (7u << 10), 0x11u << 27, 0, (1u << 23) | (3u << 29),  // BRB, bit 1, taken when clear, target 3
                0,          0,           0, 1,                        // end
                0xDEAD0001, 0x0F000000,  0, 0,                        // stale, never reached
                0,          0,           0, 1,                        // end
        };
        scene.setPrograms(programs);
        scene.write(tox2::kMethodNv4097SetTransformBranchBits, 0xA5);
        scene.drawStrip(4);
        commands = replay(scene.finish());
        CHECK(commands.size() == 2 && std::holds_alternative<tox2::RsxDraw>(commands[0]));
        const tox2::RsxDraw& branchy = std::get<tox2::RsxDraw>(commands[0]);
        CHECK(branchy.vp_branch_bits == 0xA5);
        CHECK(branchy.vp_ucode.size() == 16 && branchy.vp_ucode[3] == ((1u << 23) | (3u << 29)));
        CHECK(branchy.vp_ucode[8] == 0 && branchy.vp_ucode[9] == 0 && branchy.vp_ucode[15] == 1);
        CHECK(unimplementedCount("state unterminated vertex program") == 0);
        CHECK(unimplementedCount("state vertex program branch outside the program") == 0);
    }
    tox2::rsxUnimplementedReset();
}

void testRsxConsumer() {
    using tox2::FifoStatus;
    using Interrupts = std::vector<std::pair<tox2::RsxInterrupt, std::uint32_t>>;
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetCensus();
    tox2::resetLogOnce();
    tox2::resetRsxState();
    tox2::setRsxInterruptHook(nullptr);
    g_interrupts.clear();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    const std::uint32_t area = p.user1m.allocate(0x400000, 0x100000);
    CHECK(scratch != 0 && area != 0);
    CHECK(tox2::gcmInitBody(base, scratch, 0x10000, 0x100000, area, 0x200000) == 0);

    constexpr std::uint32_t kControl = 0x10100040, kLabels = 0x10300000, kDevice = 0x10000000;
    constexpr std::uint32_t kStart = 0x1000;
    tox2::RsxConsumer consumer(base);
    consumer.enableFrameBuilder();
    std::uint32_t at = kStart;
    const auto emit = [&](std::uint32_t method, std::initializer_list<std::uint32_t> args) {
        tox2Write32(base, area + at, method | (static_cast<std::uint32_t>(args.size()) << 18));
        at += 4;
        for (const std::uint32_t arg : args) {
            tox2Write32(base, area + at, arg);
            at += 4;
        }
    };
    const auto run = [&] {
        tox2Write32(base, kControl, at);
        return consumer.pump(100);
    };
    const auto label = [&](std::uint32_t offset) { return tox2Read32(base, kLabels + offset); };

    // Back-end release swaps the red and blue bytes, texture-read release stores the value as is;
    // an unaligned offset is ignored.
    emit(tox2::kMethodNv4097SetSemaphoreOffset, {0x20});
    emit(tox2::kMethodNv4097BackEndWriteSemaphoreRelease, {0x11223344});
    CHECK(run() == FifoStatus::Idle);
    CHECK(label(0x20) == 0x11443322);
    CHECK(tox2Read32(base, kControl + 4) == at);  // get follows put
    emit(tox2::kMethodNv4097SetSemaphoreOffset, {0x40});
    emit(tox2::kMethodNv4097TextureReadSemaphoreRelease, {0xAABBCCDD});
    emit(tox2::kMethodNv4097SetSemaphoreOffset, {0x14});
    emit(tox2::kMethodNv4097TextureReadSemaphoreRelease, {0x99});
    CHECK(run() == FifoStatus::Idle);
    CHECK(label(0x40) == 0xAABBCCDD && label(0x14) == 0x1337BABE);
    CHECK(countContaining(lines, "unaligned offset 0x14") == 1);
    CHECK(!consumer.hasWork(at) && consumer.hasWork(at + 4));

    // The reference value goes to the control register.
    emit(tox2::kMethodNv406eSetReference, {0x1234});
    CHECK(run() == FifoStatus::Idle && tox2Read32(base, kControl + 8) == 0x1234);

    // An acquire that is not satisfied keeps get at its header; it completes once the label matches.
    const std::uint32_t acquire_at = at;
    emit(tox2::kMethodNv406eSemaphoreOffset, {0x80, 0x5555});
    CHECK(run() == FifoStatus::Waiting);
    CHECK(tox2Read32(base, kControl + 4) == acquire_at && consumer.hasWork(at));
    CHECK(run() == FifoStatus::Waiting);
    tox2Write32(base, kLabels + 0x80, 0x5555);
    CHECK(run() == FifoStatus::Idle && tox2Read32(base, kControl + 4) == at);

    // NV406E release writes through the semaphore DMA; a zero into the device ready word becomes 1.
    emit(tox2::kMethodNv406eSemaphoreOffset, {0x84});
    emit(tox2::kMethodNv406eSemaphoreRelease, {0x777});
    tox2Write32(base, kDevice + 0x30, 5);
    emit(tox2::kMethodNv406eSetContextDmaSemaphore, {0x56616661});
    emit(tox2::kMethodNv406eSemaphoreOffset, {0x30});
    emit(tox2::kMethodNv406eSemaphoreRelease, {0});
    CHECK(run() == FifoStatus::Idle);
    CHECK(label(0x84) == 0x777 && tox2Read32(base, kDevice + 0x30) == 1);

    // Reports carry an increasing timer in nanoseconds and a zero value.
    emit(tox2::kMethodNv4097GetReport, {0x00000010});
    CHECK(run() == FifoStatus::Idle);
    const std::uint32_t report = kLabels + 0x1400;
    const std::uint64_t first = tox2Read64(base, report + 0x10);
    CHECK(first != 0 && tox2Read32(base, report + 0x18) == 0 && tox2Read32(base, report + 0x1c) == 0);
    CHECK(tox2Read64(base, report + 0x20) == 0xFFFFFFFFFFFFFFFF);
    emit(tox2::kMethodNv4097GetReport, {0x00000020});
    CHECK(run() == FifoStatus::Idle && tox2Read64(base, report + 0x20) > first);

    // With the flip limit off (a new consumer's default) a flip completes where its command executes, in VSYNC flip
    // mode too: it clears the flip label, notifies the hook, and the writes behind it are consumed before the next
    // VBlank.
    tox2::RsxState& state = tox2::rsxState();
    state.requested_vsync = true;
    state.flip_status = tox2::kRsxFlipStatusWaiting;
    tox2Write32(base, kLabels + 0x10, 0x1111);
    tox2::setRsxInterruptHook(&recordInterrupt);
    // The process-wide flip count moves with the flip, and a thread waiting for it wakes up.
    const std::uint64_t flips_before = tox2::rsxFlipCount();
    tox2::waitRsxFlipCount(flips_before);  // already reached: returns at once
    std::atomic<bool> flip_seen{false};
    std::thread flip_waiter([&] {
        tox2::waitRsxFlipCount(flips_before + 1);
        flip_seen = true;
    });
    emit(tox2::kMethodGcmFlipCommand, {1});
    emit(tox2::kMethodNv4097SetSemaphoreOffset, {0x50});
    emit(tox2::kMethodNv4097TextureReadSemaphoreRelease, {0x55667788});
    CHECK(!flip_seen);  // nothing has run the flip yet
    CHECK(run() == FifoStatus::Idle && consumer.census().flips == 1);
    flip_waiter.join();
    CHECK(flip_seen && tox2::rsxFlipCount() == flips_before + 1);
    CHECK(state.vblank_count == 0 && label(0x50) == 0x55667788);
    CHECK(state.flip_status == tox2::kRsxFlipStatusDone && label(0x10) == 0);
    CHECK(tox2Read32(base, kControl + 4) == at);  // get follows put
    CHECK((g_interrupts == Interrupts{{tox2::RsxInterrupt::Flip, 1}}));
    CHECK(countContaining(lines, "tox2: rsx: first flip") == 1);
    // The frame builder ends the frame at the command, once.
    std::vector<tox2::RsxCommand> built = consumer.takeCommands();
    CHECK(built.size() == 1 && std::holds_alternative<tox2::RsxFlip>(built[0]));
    CHECK(std::get<tox2::RsxFlip>(built[0]).buffer == 1 && std::get<tox2::RsxFlip>(built[0]).flip_index == 0);
    CHECK(run() == FifoStatus::Idle && consumer.takeCommands().empty());
    // A VBlank neither completes nor repeats a flip.
    consumer.vblankTick();
    CHECK(state.vblank_count == 1 && consumer.census().flips == 1);
    CHECK(consumer.pump(100) == FifoStatus::Idle && consumer.takeCommands().empty());
    CHECK((g_interrupts == Interrupts{{tox2::RsxInterrupt::Flip, 1}, {tox2::RsxInterrupt::VBlank, 1}}));
    // Two flips in a row (HSYNC flip mode) complete in one run as well.
    state.requested_vsync = false;
    emit(tox2::kMethodGcmFlipCommand, {2});
    emit(tox2::kMethodGcmFlipCommand, {2});
    CHECK(run() == FifoStatus::Idle && consumer.census().flips == 3);
    built = consumer.takeCommands();
    CHECK(built.size() == 2 && std::get<tox2::RsxFlip>(built[0]).buffer == 2 &&
          std::get<tox2::RsxFlip>(built[0]).flip_index == 1);
    CHECK(std::get<tox2::RsxFlip>(built[1]).flip_index == 2);

    // User commands reach the hook; without one they are only logged.
    g_interrupts.clear();
    emit(tox2::kMethodGcmSetUserCommand, {7});
    emit(tox2::kMethodGcmSetUserCommand + 4, {9});
    CHECK(run() == FifoStatus::Idle);
    CHECK((g_interrupts == Interrupts{{tox2::RsxInterrupt::User, 7}, {tox2::RsxInterrupt::User, 9}}));
    tox2::setRsxInterruptHook(nullptr);
    emit(tox2::kMethodGcmSetUserCommand, {3});
    CHECK(run() == FifoStatus::Idle);
    CHECK(countContaining(lines, "pending user command cause 3") == 1);

    // NV308A inline blit: the color words land at offset + (x + i) * 4 + pitch * y, big-endian, in local
    // memory or through the IO map. The pitch is the high half of NV3062_SET_PITCH.
    CHECK(tox2::rsxInlineBlitOffset(0x01882180, 0x1000, 4, 0, 0) == 0x01882190);
    CHECK(tox2::rsxInlineBlitOffset(0x01882180, 0x1000, 4, 2, 3) == 0x01882190 + 12 + 0x2000);
    CHECK(tox2::rsxInlineBlitOffset(0xFFFFFFFC, 0xFFFF, 0xFFFF, 0xFFFF, 0) > 0xFFFFFFFFull);  // no wrap
    {
        tox2::rsxUnimplementedReset();
        constexpr std::uint32_t kLocalOffset = 0x1000, kIoOffset = 0x8000;
        const std::uint32_t local_ea = 0xC0000000 + kLocalOffset;
        const std::uint32_t io_ea = area + kIoOffset;
        for (std::uint32_t i = 0; i < 8; i++) {
            tox2Write32(base, local_ea + i * 4, 0xAAAAAAAA);
            tox2Write32(base, io_ea + i * 4, 0xAAAAAAAA);
        }
        const auto blit = [&](std::uint32_t dma, std::uint32_t format, std::uint32_t offset, std::uint32_t point,
                              std::uint32_t width, std::initializer_list<std::uint32_t> words) {
            emit(tox2::kMethodNv3062SetColorFormat, {format, 0x10001000});  // format, pitch
            if (dma != 0) emit(tox2::kMethodNv3062SetContextDmaImageDestin, {dma});
            emit(tox2::kMethodNv3062SetOffsetDestin, {offset});
            emit(tox2::kMethodNv308aPoint, {point, width | (1u << 16), width | (1u << 16)});
            emit(tox2::kMethodNv308aColor, words);
            CHECK(run() == FifoStatus::Idle);
        };
        // Point x = 4 (as in the game), 4 words, Y32; the destination DMA stays at its post-flip default (local).
#if TOX2_RSX_WRITE_WATCH
        // The blit is a user-mode store, so the game view's next check of the range sees it.
        tox2::GuestRsxMemoryView watch_view(base);
        const auto blit_before = watch_view.checkWrites(0, kLocalOffset, 16, 0);
        CHECK(blit_before.tracked && !blit_before.clean);
        const auto blit_control = watch_view.checkWrites(0, kLocalOffset, 16, blit_before.stamp);
        CHECK(blit_control.clean);
#endif
        blit(0, 11, kLocalOffset - 16, 0x00000004, 4, {0x00000000, 0x3F80, 0x01020304, 0x0A0B0C0D});
#if TOX2_RSX_WRITE_WATCH
        CHECK(!watch_view.checkWrites(0, kLocalOffset, 16, blit_control.stamp).clean);
#endif
        CHECK(tox2Read32(base, local_ea) == 0 && tox2Read32(base, local_ea + 4) == 0x3F80);
        CHECK(tox2Read32(base, local_ea + 8) == 0x01020304 && tox2Read32(base, local_ea + 12) == 0x0A0B0C0D);
        CHECK(base[local_ea + 8] == 0x01 && base[local_ea + 11] == 0x04);  // big-endian in guest memory
        CHECK(tox2Read32(base, local_ea + 16) == 0xAAAAAAAA);
        // A8R8G8B8 through the IO map, second row: y = 1 adds one pitch (0x1000).
        blit(tox2::kRsxContextDmaMain, 10, kIoOffset - 0x1000, 0x00010000, 2, {0x11111111, 0x22222222});
        CHECK(tox2Read32(base, io_ea) == 0x11111111 && tox2Read32(base, io_ea + 4) == 0x22222222);
        CHECK(tox2Read32(base, io_ea + 8) == 0xAAAAAAAA);
        // Words beyond the width are dropped.
        blit(tox2::kRsxContextDmaLocal, 11, kLocalOffset + 32, 0, 1, {7, 8, 9});
        CHECK(tox2Read32(base, local_ea + 32) == 7 && tox2Read32(base, local_ea + 36) != 8);
        CHECK(unimplementedCount("method NV308A_COLOR destination out of range") == 0);
        // Formats that need a conversion write nothing and are counted.
        tox2Write32(base, local_ea + 64, 0xAAAAAAAA);
        blit(tox2::kRsxContextDmaLocal, 4, kLocalOffset + 64, 0, 1, {0x55555555});
        CHECK(tox2Read32(base, local_ea + 64) == 0xAAAAAAAA && unimplementedCount("method NV308A_COLOR format 4") == 1);
        // Out-of-range destinations are skipped and reported once.
        blit(tox2::kRsxContextDmaLocal, 11, 0x0F900000, 0, 2, {1, 2});
        blit(tox2::kRsxContextDmaLocal, 11, 0xFFFFFFF0, 0x00FF0000, 1, {3});
        blit(tox2::kRsxContextDmaMain, 11, 0x3F000000, 0, 1, {4});
        blit(0x12345678, 11, 0, 0, 1, {5});
        blit(tox2::kRsxContextDmaLocal, 11, kLocalOffset + 2, 0, 1, {6});  // unaligned
        CHECK(unimplementedCount("method NV308A_COLOR destination out of range") == 6);
        CHECK(countContaining(lines, "NV308A inline blit") == 1 && !consumer.faulted());
        tox2::rsxUnimplementedReset();
    }

    // NV0039 buffer copy: main and local windows are seeded, every copy is replayed on a byte model, and the
    // whole windows are compared, so a byte written outside the destination shows up.
    {
        tox2::rsxUnimplementedReset();
        // A second IO page that is not contiguous in guest memory with the first.
        CHECK(tox2::rsxMapEaIo(area + 0x200000, 0x100000, 0x100000) == 0);
        constexpr std::uint32_t kLocalWin = 0x4000, kLocalLen = 0x400;
        constexpr std::uint32_t kMainWin = 0xFFF00, kMainLen = 0x300;  // crosses the IO page boundary
        const std::uint32_t local_ea = 0xC0000000 + kLocalWin;
        const auto mainEa = [&](std::uint32_t io) {
            return io < 0x100000 ? area + io : area + 0x200000 + io - 0x100000;
        };
        std::vector<std::uint8_t> local_model(kLocalLen), main_model(kMainLen);
        const auto seed = [&] {
            for (std::uint32_t i = 0; i < kLocalLen; i++) {
                local_model[i] = static_cast<std::uint8_t>(i * 37 + 11);
                base[local_ea + i] = local_model[i];
            }
            for (std::uint32_t i = 0; i < kMainLen; i++) {
                main_model[i] = static_cast<std::uint8_t>(i * 53 + 5);
                base[mainEa(kMainWin + i)] = main_model[i];
            }
        };
        const auto matchesModel = [&] {
            bool same = std::memcmp(base + local_ea, local_model.data(), kLocalLen) == 0;
            for (std::uint32_t i = 0; i < kMainLen; i++) same = same && base[mainEa(kMainWin + i)] == main_model[i];
            return same;
        };
        const auto modelByte = [&](std::uint32_t dma, std::int64_t offset) -> std::uint8_t& {
            return dma == tox2::kRsxContextDmaLocal ? local_model[offset - kLocalWin] : main_model[offset - kMainWin];
        };
        const auto copy = [&](std::uint32_t dma_in, std::uint32_t offset_in, std::uint32_t dma_out,
                              std::uint32_t offset_out, std::int32_t pitch_in, std::int32_t pitch_out,
                              std::uint32_t length, std::uint32_t line_count, std::uint32_t format = 0x101) {
            emit(tox2::kMethodNv0039SetContextDmaBufferIn, {dma_in, dma_out});
            emit(tox2::kMethodNv0039OffsetIn, {offset_in, offset_out, static_cast<std::uint32_t>(pitch_in),
                                               static_cast<std::uint32_t>(pitch_out), length, line_count, format, 0});
            CHECK(run() == FifoStatus::Idle);
        };
        // The same copy on the model, line by line through a temporary so that overlap cannot matter.
        const auto replay = [&](std::uint32_t dma_in, std::uint32_t offset_in, std::uint32_t dma_out,
                                std::uint32_t offset_out, std::int32_t pitch_in, std::int32_t pitch_out,
                                std::uint32_t length, std::uint32_t line_count) {
            for (std::uint32_t line = 0; line < line_count; line++) {
                std::vector<std::uint8_t> bytes(length);
                for (std::uint32_t i = 0; i < length; i++) {
                    bytes[i] = modelByte(dma_in, std::int64_t{offset_in} + std::int64_t{line} * pitch_in + i);
                }
                for (std::uint32_t i = 0; i < length; i++) {
                    modelByte(dma_out, std::int64_t{offset_out} + std::int64_t{line} * pitch_out + i) = bytes[i];
                }
            }
        };
        constexpr std::uint32_t kLocal = tox2::kRsxContextDmaLocal, kMain = tox2::kRsxContextDmaMain;

        // Main to local, across the IO page boundary (the source is split in guest memory): one block.
        seed();
        copy(kMain, kMainWin + 0x80, kLocal, kLocalWin + 0x40, 0x200, 0x200, 0x200, 1);
        replay(kMain, kMainWin + 0x80, kLocal, kLocalWin + 0x40, 0x200, 0x200, 0x200, 1);
        CHECK(matchesModel() && base[local_ea + 0x40] == static_cast<std::uint8_t>(0x80 * 53 + 5));
        CHECK(base[local_ea + 0x3F] == static_cast<std::uint8_t>(0x3F * 37 + 11));
        CHECK(base[local_ea + 0x240] == static_cast<std::uint8_t>(0x240 * 37 + 11));
        // The block form: pitches equal to the line length, several lines, still one range.
        seed();
        copy(kMain, kMainWin, kLocal, kLocalWin + 0x100, 0x40, 0x40, 0x40, 8);
        replay(kMain, kMainWin, kLocal, kLocalWin + 0x100, 0x40, 0x40, 0x40, 8);
        CHECK(matchesModel() && base[local_ea + 0x100 + 0x1FF] == static_cast<std::uint8_t>(0x1FF * 53 + 5));
        // Local to main, the destination crossing the page boundary.
        seed();
        copy(kLocal, kLocalWin + 0x10, kMain, kMainWin + 0xF0, 0x100, 0x100, 0x100, 1);
        replay(kLocal, kLocalWin + 0x10, kMain, kMainWin + 0xF0, 0x100, 0x100, 0x100, 1);
        CHECK(matchesModel() && base[mainEa(kMainWin + 0xF0)] == static_cast<std::uint8_t>(0x10 * 37 + 11));
        CHECK(base[mainEa(0x100000)] == static_cast<std::uint8_t>((0x10 + 0x10) * 37 + 11));
        // Local to local.
        seed();
        copy(kLocal, kLocalWin, kLocal, kLocalWin + 0x300, 0x40, 0x40, 0x40, 1);
        replay(kLocal, kLocalWin, kLocal, kLocalWin + 0x300, 0x40, 0x40, 0x40, 1);
        CHECK(matchesModel() && base[local_ea + 0x300] == 11 &&
              base[local_ea + 0x33F] == static_cast<std::uint8_t>(0x3F * 37 + 11));
        // Pitched, pitch_in != pitch_out, four lines of 0x10 bytes; the gaps keep their bytes.
        seed();
        copy(kLocal, kLocalWin + 0x200, kLocal, kLocalWin + 0x100, 0x20, 0x30, 0x10, 4);
        replay(kLocal, kLocalWin + 0x200, kLocal, kLocalWin + 0x100, 0x20, 0x30, 0x10, 4);
        CHECK(matchesModel() && base[local_ea + 0x130] == static_cast<std::uint8_t>(0x220 * 37 + 11));
        CHECK(base[local_ea + 0x110] == static_cast<std::uint8_t>(0x110 * 37 + 11));  // gap between lines
        // Pitched between main and local, and a negative destination pitch (lines go downwards in memory).
        seed();
        copy(kMain, kMainWin + 0x100, kLocal, kLocalWin + 0x380, 0x10, -0x20, 0x10, 3);
        replay(kMain, kMainWin + 0x100, kLocal, kLocalWin + 0x380, 0x10, -0x20, 0x10, 3);
        CHECK(matchesModel() && base[local_ea + 0x340] == static_cast<std::uint8_t>((0x100 + 0x20) * 53 + 5));
        // Overlapping local to local, destination above and below the source: memmove.
        seed();
        copy(kLocal, kLocalWin + 0x10, kLocal, kLocalWin + 0x30, 0x80, 0x80, 0x80, 1);
        replay(kLocal, kLocalWin + 0x10, kLocal, kLocalWin + 0x30, 0x80, 0x80, 0x80, 1);
        CHECK(matchesModel() && base[local_ea + 0x30] == static_cast<std::uint8_t>(0x10 * 37 + 11));
        CHECK(base[local_ea + 0xAF] == static_cast<std::uint8_t>(0x8F * 37 + 11));
        seed();
        copy(kLocal, kLocalWin + 0x30, kLocal, kLocalWin + 0x10, 0x80, 0x80, 0x80, 1);
        replay(kLocal, kLocalWin + 0x30, kLocal, kLocalWin + 0x10, 0x80, 0x80, 0x80, 1);
        CHECK(matchesModel() && base[local_ea + 0x10] == static_cast<std::uint8_t>(0x30 * 37 + 11));
        // Overlap inside main memory across the page boundary (the staged path).
        seed();
        copy(kMain, kMainWin + 0x40, kMain, kMainWin + 0x50, 0x200, 0x200, 0x200, 1);
        replay(kMain, kMainWin + 0x40, kMain, kMainWin + 0x50, 0x200, 0x200, 0x200, 1);
        CHECK(matchesModel());

        // Nothing is copied for an empty line or an empty count.
        seed();
        copy(kLocal, kLocalWin, kLocal, kLocalWin + 0x100, 0x40, 0x40, 0x40, 0);
        copy(kLocal, kLocalWin, kLocal, kLocalWin + 0x100, 0x40, 0x40, 0, 4);
        CHECK(matchesModel() && unimplementedCount("method NV0039 copy out of range") == 0);
        // Other formats are counted and write nothing.
        copy(kLocal, kLocalWin, kLocal, kLocalWin + 0x100, 0x40, 0x40, 0x40, 1, 0x202);
        CHECK(matchesModel() && unimplementedCount("method NV0039 format 0x202") == 1);
        // Out-of-range addresses write nothing and are reported once: destination past local memory, a source
        // that is not mapped, a negative offset, an unknown DMA object, and a later line that leaves the buffer.
        copy(kLocal, kLocalWin, kLocal, 0x0F900000 - 8, 0x10, 0x10, 0x10, 1);
        CHECK(matchesModel() && unimplementedCount("method NV0039 copy out of range") == 1);
        copy(kMain, 0x3F000000, kLocal, kLocalWin, 0x10, 0x10, 0x10, 1);
        copy(kLocal, kLocalWin, kMain, 0x3F000000, 0x10, 0x10, 0x10, 1);
        copy(kLocal, kLocalWin, kLocal, kLocalWin + 0x10, 0x10, -0x10000000, 0x10, 2);
        copy(0x12345678, 0, kLocal, kLocalWin, 0x10, 0x10, 0x10, 1);
        copy(kLocal, kLocalWin, 0x12345678, 0, 0x10, 0x10, 0x10, 1);
        // A destination that runs from the mapped IO page 1 into page 2, which nothing maps, is rejected as a whole,
        // before the first store; so is a huge length, before anything is allocated.
        copy(kMain, 0x80000, kMain, kMainWin, 0x180000, 0x180000, 0x180000, 1);
        copy(kMain, kMainWin, kMain, kMainWin + 0x1000, 0x70000000, 0x70000000, 0x70000000, 1);
        copy(kMain, kMainWin, kMain, kMainWin + 0x1000, 0, 0, 0xFFFFFFFF, 1);
        CHECK(unimplementedCount("method NV0039 copy out of range") == 9);
        // The last case copies its first line before the second leaves the buffer; the rest is untouched.
        replay(kLocal, kLocalWin, kLocal, kLocalWin + 0x10, 0x10, -0x10000000, 0x10, 1);
        CHECK(matchesModel());
        CHECK(countContaining(lines, "NV0039 copy") == 1 && !consumer.faulted());
        tox2::rsxUnimplementedReset();
        tox2::rsxUnmapEa(area + 0x200000);
    }

    // The census counts completed writes by register: a flip counts once.
    const tox2::RsxCensus census = consumer.census();
    const auto countOf = [&](std::uint32_t method) -> std::uint64_t {
        for (const auto& m : census.methods) {
            if (m.method == method) return m.count;
        }
        return 0;
    };
    CHECK(countOf(tox2::kMethodGcmFlipCommand) == 3 && countOf(tox2::kMethodGcmSetUserCommand) == 2);
    CHECK(countOf(tox2::kMethodNv4097SetSemaphoreOffset) == 4 && countOf(tox2::kMethodNv4097GetReport) == 2);
    CHECK(countOf(0x1234) == 0 && census.headers > 10);

    // A jump into unmapped IO space faults once; after that nothing is consumed.
    tox2Write32(base, area + at, 0x20000000 | 0x0F000000);
    at += 4;
    CHECK(run() == FifoStatus::Fault && consumer.faulted());
    CHECK(countContaining(lines, "command fifo fault") == 1);
    CHECK(run() == FifoStatus::Fault && countContaining(lines, "command fifo fault") == 1);
    CHECK(!consumer.hasWork(at + 4));

    // Capturing through a consumer: frame 2 starts from what flip 1 left behind (its context reset
    // applied, the surface cleared in frame 1 known) and replays to the commands built after that flip.
    // The first consumer is done, so the second one starts at the current put.
    {
        state.display[1] = {0x400000, 0x1400, 1280, 720};
        state.display_count = 2;
        tox2Write32(base, kControl + 4, at);
        tox2::RsxConsumer capturing(base);
        capturing.enableCapture({2});
        emit(0x0200, {1280u << 16});
        emit(0x0204, {720u << 16});
        emit(0x0208, {8u | (2u << 5) | (1u << 8)});
        emit(0x020C, {0x1400});
        emit(0x0210, {0x100000});
        emit(0x0214, {0x900000});
        emit(0x022C, {0x1400});
        emit(0x0220, {1});
        emit(0x08C0, {1280u << 16});
        emit(0x08C4, {720u << 16});
        emit(0x1D90, {0xFF102030});
        emit(0x1D8C, {0xFFFFFF00});
        emit(0x1D94, {0xF3});
        emit(tox2::kMethodGcmFlipCommand, {1});
        // Frame 2 leaves the surface offset unset, which the context reset made 0. Its draw reads three
        // vertices of three floats from main memory at IO offset 0x80000.
        for (std::uint32_t i = 0; i < 36; i++) base[area + 0x80000 + i] = static_cast<std::uint8_t>(i + 1);
        emit(0x0200, {1280u << 16});
        emit(0x0204, {720u << 16});
        emit(0x0208, {8u | (2u << 5) | (1u << 8)});
        emit(0x020C, {0x1400});
        emit(0x0220, {1});
        emit(0x1D94, {0xF3});
        emit(tox2::kMethodNv4097SetVertexDataArrayFormat, {2u | (3u << 4) | (12u << 8)});
        emit(tox2::kMethodNv4097SetVertexDataArrayOffset, {0x80080000});
        emit(tox2::kMethodNv4097SetBeginEnd, {5});
        emit(tox2::kMethodNv4097DrawArrays, {2u << 24});
        emit(tox2::kMethodNv4097SetBeginEnd, {0});
        const std::uint32_t after_draw = at;
        emit(tox2::kMethodGcmFlipCommand, {2});
        tox2Write32(base, kControl, at);

        // The pump stops right after the draw end (a command of its own here, so get is published past it);
        // the draw is built when the commands are taken, and the rest of the frame follows on the next pump.
        CHECK(capturing.pump(200) == FifoStatus::Budget && capturing.stopAfterWrite());
        CHECK(tox2Read32(base, kControl + 4) == after_draw);
        std::vector<tox2::RsxCommand> live = capturing.takeCommands();
        CHECK(!capturing.stopAfterWrite());
        CHECK(capturing.pump(200) == FifoStatus::Idle);
        for (tox2::RsxCommand& command : capturing.takeCommands()) live.push_back(std::move(command));
        CHECK(live.size() == 5 && std::holds_alternative<tox2::RsxClear>(live[2]) &&
              std::holds_alternative<tox2::RsxDraw>(live[3]) && std::holds_alternative<tox2::RsxFlip>(live[4]));
        CHECK(std::get<tox2::RsxClear>(live[0]).surfaces.color.offset == 0x100000);
        CHECK(std::get<tox2::RsxClear>(live[2]).surfaces.color.offset == 0);
        CHECK(std::get<tox2::RsxFlip>(live[4]).flip_index == 1);
        const tox2::RsxDraw& draw = std::get<tox2::RsxDraw>(live[3]);
        CHECK(draw.count == 3 && draw.vertex_data.size() == 36 && draw.vertex_data[0] == 1 &&
              draw.vertex_data[35] == 36);

        std::vector<tox2::RsxConsumer::FinishedCapture> captures = capturing.takeCaptures();
        CHECK(captures.size() == 1 && captures[0].frame == 2);
        tox2::RsxCaptureReader reader;
        std::string error;
        CHECK(reader.parse(captures[0].file.flatten(), error));
        CHECK(reader.flipIndex() == 1);
        CHECK(reader.startRegisters().reg(0x0210) == 0 && reader.startRegisters().reg(0x0200) == 0);
        CHECK(reader.startRegisters().reg(tox2::kMethodGcmFlipCommand) == 1);  // the flip write itself landed
        CHECK(reader.surfaces().size() == 2 && reader.surfaces()[0].offset == 0x100000 &&
              reader.surfaces()[0].width == 1280);
        CHECK(reader.surfaces()[1].offset == 0x900000 &&
              reader.surfaces()[1].depth_format == tox2::RsxDepthFormat::Z24S8);
        CHECK(reader.displayCount() == 2 && reader.display()[1].offset == 0x400000);
        CHECK(captures[0].writes == reader.writeCount() && captures[0].writes == 11);

        // The draw's memory reads were recorded ahead of its draw-end write, so the replay builds it too.
        tox2::ReplayRsxMemoryView view;
        tox2::RsxFrameBuilder replay_builder(view);
        tox2::replayCapture(reader, view, replay_builder);
        const std::vector<tox2::RsxCommand> replayed = replay_builder.takeCommands();
        CHECK(replayed.size() == 3 && sameCommand(live[2], replayed[0]) && sameCommand(live[3], replayed[1]) &&
              sameCommand(live[4], replayed[2]));
        CHECK(capturing.takeCaptures().empty());
    }

    // The Hz60 flip limit: the first flip is immediate, every later one waits for a deadline that moves on by one
    // period per flip, and the parser stays at the held flip meanwhile. The game's flip mode does not matter.
    {
        state.requested_vsync = true;
        const auto emitFlips = [&](int count) {
            for (int i = 0; i < count; i++) emit(tox2::kMethodGcmFlipCommand, {1});
        };
        tox2Write32(base, kControl + 4, at);
        tox2::RsxConsumer limited(base);
        limited.setFlipLimit(tox2::RsxFlipLimit::Hz60);
        CHECK(!limited.flipHoldDeadline() && !limited.flipHeldForVblank());
        emitFlips(4);
        tox2Write32(base, kControl, at);
        const auto start = std::chrono::steady_clock::now();
        CHECK(limited.pump(100) == FifoStatus::Waiting && limited.census().flips == 1);
        CHECK(limited.flipHoldDeadline().has_value() && !limited.flipHeldForVblank() && limited.hasWork(at));
        const std::uint32_t held_get = tox2Read32(base, kControl + 4);
        CHECK(held_get < at);  // get stays at the held flip
        CHECK(!limited.write(tox2::kMethodGcmFlipCommand, 1) && limited.census().flips == 1);
        while (limited.pump(100) != FifoStatus::Idle) {
            CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        CHECK(limited.census().flips == 4 && !limited.flipHoldDeadline());
        CHECK(elapsed >= 3 * tox2::kRsxFlipPeriod && elapsed < 3 * tox2::kRsxFlipPeriod + std::chrono::seconds(1));
        CHECK(tox2Read32(base, kControl + 4) == at);

        // Off, the same four flips complete in a single pump.
        tox2::RsxConsumer unlimited(base);
        emitFlips(4);
        tox2Write32(base, kControl, at);
        CHECK(unlimited.pump(100) == FifoStatus::Idle && unlimited.census().flips == 4);

        // Turning the limiter off while a flip is held releases it: no hold deadline is left, and the next pump
        // completes everything that was queued without waiting.
        tox2::RsxConsumer switched(base);
        switched.setFlipLimit(tox2::RsxFlipLimit::Hz60);
        emitFlips(4);
        tox2Write32(base, kControl, at);
        CHECK(switched.pump(100) == FifoStatus::Waiting && switched.census().flips == 1);
        CHECK(switched.flipHoldDeadline().has_value());
        switched.setFlipLimit(tox2::RsxFlipLimit::Off);
        CHECK(!switched.flipHoldDeadline() && switched.hasWork(at));
        CHECK(switched.pump(100) == FifoStatus::Idle && switched.census().flips == 4);

        // Hz60 with the HSYNC flip mode paces the same way, and Off completes VSYNC flips at once.
        state.requested_vsync = false;
        tox2::RsxConsumer hsync_limited(base);
        hsync_limited.setFlipLimit(tox2::RsxFlipLimit::Hz60);
        emitFlips(2);
        tox2Write32(base, kControl, at);
        CHECK(hsync_limited.pump(100) == FifoStatus::Waiting && hsync_limited.census().flips == 1);
        CHECK(hsync_limited.flipHoldDeadline().has_value() && !hsync_limited.flipHeldForVblank());
        state.requested_vsync = true;
        tox2Write32(base, kControl + 4, at);  // hsync_limited left get at its held flip
        tox2::RsxConsumer off_vsync(base);
        off_vsync.setFlipLimit(tox2::RsxFlipLimit::Off);
        emitFlips(2);
        tox2Write32(base, kControl, at);
        CHECK(off_vsync.pump(100) == FifoStatus::Idle && off_vsync.census().flips == 2);
        CHECK(!off_vsync.flipHeldForVblank());
    }

    // The Vsync flip limit (RPCS3's "PS3 Native" frame limit): a flip in VSYNC flip mode completes at the first
    // vblank tick after its command executes, the first flip included, and at most one flip completes per tick.
    // The tick queues the vblank interrupt before the flip it releases queues the flip interrupt: the game's
    // vblank handler counts down the frame interval and the flip handler releases the gate it waits on.
    {
        const auto emitFlips = [&](int count) {
            for (int i = 0; i < count; i++) emit(tox2::kMethodGcmFlipCommand, {1});
        };
        state.requested_vsync = true;
        g_interrupts.clear();
        tox2::setRsxInterruptHook(&recordInterrupt);
        tox2Write32(base, kControl + 4, at);
        tox2::RsxConsumer paced(base);
        paced.setFlipLimit(tox2::RsxFlipLimit::Vsync);
        CHECK(!paced.flipHeldForVblank());
        emitFlips(2);
        tox2Write32(base, kControl, at);
        CHECK(paced.pump(100) == FifoStatus::Waiting && paced.census().flips == 0);
        CHECK(paced.flipHeldForVblank() && !paced.flipHoldDeadline() && paced.hasWork(at));
        CHECK(tox2Read32(base, kControl + 4) < at);  // get stays at the held flip
        CHECK(paced.pump(100) == FifoStatus::Waiting && paced.census().flips == 0 && g_interrupts.empty());
        paced.vblankTick();
        CHECK(!paced.flipHeldForVblank() && paced.hasWork(at));  // the tick ended the hold: pump now
        CHECK(paced.pump(100) == FifoStatus::Waiting && paced.census().flips == 1);
        CHECK(paced.flipHeldForVblank());  // the second flip needs a tick of its own
        CHECK(paced.pump(100) == FifoStatus::Waiting && paced.census().flips == 1);
        paced.vblankTick();
        CHECK(paced.pump(100) == FifoStatus::Idle && paced.census().flips == 2 && !paced.flipHeldForVblank());
        CHECK(!paced.flipHoldDeadline() && tox2Read32(base, kControl + 4) == at);
        CHECK((g_interrupts == Interrupts{{tox2::RsxInterrupt::VBlank, 1},
                                          {tox2::RsxInterrupt::Flip, 1},
                                          {tox2::RsxInterrupt::VBlank, 1},
                                          {tox2::RsxInterrupt::Flip, 1}}));

        // HSYNC flip mode uses the Hz60 limiter: the first flip is immediate, the second waits for a deadline.
        state.requested_vsync = false;
        emitFlips(2);
        tox2Write32(base, kControl, at);
        CHECK(paced.pump(100) == FifoStatus::Waiting && paced.census().flips == 3);
        CHECK(paced.flipHoldDeadline().has_value() && !paced.flipHeldForVblank());

        // A game that switches to HSYNC while a flip waits for a tick releases it to the limiter at once, and the
        // limit switched to Off releases it without a tick.
        state.requested_vsync = true;
        tox2::RsxConsumer switched(base);
        switched.setFlipLimit(tox2::RsxFlipLimit::Vsync);
        tox2Write32(base, kControl + 4, at - 4 * 2);
        CHECK(switched.pump(100) == FifoStatus::Waiting && switched.census().flips == 0);
        CHECK(switched.flipHeldForVblank());
        state.requested_vsync = false;
        CHECK(!switched.flipHeldForVblank() && switched.hasWork(at));
        CHECK(switched.pump(100) == FifoStatus::Idle && switched.census().flips == 1);
        state.requested_vsync = true;
        tox2::RsxConsumer released(base);
        released.setFlipLimit(tox2::RsxFlipLimit::Vsync);
        tox2Write32(base, kControl + 4, at - 4 * 2);
        CHECK(released.pump(100) == FifoStatus::Waiting && released.flipHeldForVblank());
        released.setFlipLimit(tox2::RsxFlipLimit::Off);
        CHECK(!released.flipHeldForVblank() && released.hasWork(at));
        CHECK(released.pump(100) == FifoStatus::Idle && released.census().flips == 1);
        tox2::setRsxInterruptHook(nullptr);
    }

    // A capture requested at run time records the first whole frame that starts after the request, with no
    // frame listed. The request before flip command 1 therefore yields frame 2 (flip 1 is already reached when
    // the frame builder sees the write).
    {
        tox2Write32(base, kControl + 4, at);
        tox2::RsxConsumer plain(base);
        CHECK(!plain.requestCapture() && !plain.captureRequestPending() && !plain.frameBuilderLive());

        tox2::RsxConsumer requesting(base);
        requesting.enableFrameBuilder();
        CHECK(requesting.frameBuilderLive() && !requesting.captureRequestPending());
        CHECK(requesting.requestCapture() && requesting.captureRequestPending());
        emit(tox2::kMethodGcmFlipCommand, {1});
        emit(tox2::kMethodGcmFlipCommand, {1});
        tox2Write32(base, kControl, at);
        CHECK(requesting.pump(100) == FifoStatus::Idle && requesting.census().flips == 2);
        std::vector<tox2::RsxConsumer::FinishedCapture> done = requesting.takeCaptures();
        CHECK(done.size() == 1 && done[0].frame == 2 && done[0].requested);
        CHECK(!requesting.captureRequestPending() && requesting.requestedRecording() == 2);
        requesting.finishRequestedRecording(7);  // another frame's: ignored
        CHECK(requesting.requestedRecording() == 2);
        requesting.finishRequestedRecording(2);
        CHECK(requesting.requestedRecording() == 0);

        // Nothing more is recorded for later flips.
        emit(tox2::kMethodGcmFlipCommand, {1});
        tox2Write32(base, kControl, at);
        CHECK(requesting.pump(100) == FifoStatus::Idle && requesting.census().flips == 3);
        CHECK(requesting.takeCaptures().empty() && !requesting.captureRequestPending());
    }

    // The process-wide flip limit is readable and writable without a running RSX thread; it starts as Vsync.
    {
        CHECK(tox2::rsxFlipLimit() == tox2::RsxFlipLimit::Vsync);
        tox2::setRsxFlipLimit(tox2::RsxFlipLimit::Off);
        CHECK(tox2::rsxFlipLimit() == tox2::RsxFlipLimit::Off);
        tox2::setRsxFlipLimit(tox2::RsxFlipLimit::Hz60);
        CHECK(tox2::rsxFlipLimit() == tox2::RsxFlipLimit::Hz60);
        tox2::setRsxFlipLimit(tox2::RsxFlipLimit::Vsync);
        CHECK(tox2::rsxFlipLimit() == tox2::RsxFlipLimit::Vsync);
        CHECK(!tox2::requestRsxCapture() && !tox2::rsxCaptureStatus().available);
    }

    // The census section is printed only while a provider is registered.
    CHECK(tox2::formatCensus().find("census: rsx") == std::string::npos);
    tox2::setRsxCensusProvider(&fakeRsxCensus);
    const std::string text = tox2::formatCensus();
    CHECK(text.find("census: rsx 120 headers, 3 distinct methods, 3 flips\n"
                    "  method 0x1808: 9\n"
                    "  method 0x1800: 5\n"
                    "  method 0x1d6c NV4097_SET_SEMAPHORE_OFFSET: 5\n") != std::string::npos);
    CHECK(text.find("interrupt handler calls") == std::string::npos);
    tox2::setRsxCensusProvider(&fakeRsxCensusWithHandlerCalls);
    CHECK(tox2::formatCensus().find("census: rsx 120 headers, 3 distinct methods, 3 flips\n"
                                    "  interrupt handler calls: vblank 600, flip 0, user 2\n"
                                    "  method 0x1808: 9\n") != std::string::npos);
    tox2::setRsxCensusProvider([]() {
        tox2::RsxCensus census = fakeRsxCensusWithHandlerCalls();
        census.vblank_coalesced = 4;
        return census;
    });
    CHECK(tox2::formatCensus().find("  interrupt handler calls: vblank 600, flip 0, user 2, coalesced vblanks 4\n") !=
          std::string::npos);

    tox2::setRsxInterruptHook(nullptr);
    tox2::resetRsxState();
    p.user1m.free(area);
    p.main.free(scratch);
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

void testCellGcmSys() {
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetCensus();
    tox2::resetLogOnce();
    tox2::resetRsxState();
    tox2::resetHostFunctions();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    const std::uint32_t area = p.user1m.allocate(0x400000, 0x100000);
    CHECK(scratch != 0 && area != 0 && (area & 0xfffff) == 0);
    const std::uint32_t out = scratch, config = scratch + 0x100, stack = scratch + 0x9000;
    const std::uint32_t io_ea = area, io_size = 0x100000, local_size = 0x200000;
    constexpr std::uint32_t kFailure = 0x802100ff, kNoIoPageTable = 0x80210001, kInval = 0x80010002;

    enum Import : std::uint32_t {
        TiledPitch,
        InitBody,
        AddressToOffset,
        Func15,
        TimeStamp,
        CommandWords,
        SegmentWords,
        FifoSize,
        MapMain,
        ControlRegister,
        GetConfig,
        UnmapEa,
        SetUserHandler,
        SetFlipCommand,
        BindTile,
        SetFlipMode,
        GetLastFlipTime,
        BindZcull,
        SetFlipHandler,
        SetDisplayBuffer,
        UnbindZcull,
        SetVBlankHandler,
        ResetFlipStatus,
        SetTileInfo,
        FlipWithLabel,
        UnbindTile,
        GetFlipStatus,
        GetLabelAddress,
        ImportCount
    };
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x055bd74d, "cellGcmSys", "cellGcmGetTiledPitchSize"},
            {0x00800010, 0x15bae46b, "cellGcmSys", "_cellGcmInitBody"},
            {0x00800020, 0x21ac3697, "cellGcmSys", "cellGcmAddressToOffset"},
            {0x00800030, 0x3a33c1fd, "cellGcmSys", "_cellGcmFunc15"},
            {0x00800040, 0x5a41c10f, "cellGcmSys", "cellGcmGetTimeStamp"},
            {0x00800050, 0x5e2ee0f0, "cellGcmSys", "cellGcmGetDefaultCommandWordSize"},
            {0x00800060, 0x8cdf8c70, "cellGcmSys", "cellGcmGetDefaultSegmentWordSize"},
            {0x00800070, 0x9ba451e4, "cellGcmSys", "cellGcmSetDefaultFifoSize"},
            {0x00800080, 0xa114ec67, "cellGcmSys", "cellGcmMapMainMemory"},
            {0x00800090, 0xa547adde, "cellGcmSys", "cellGcmGetControlRegister"},
            {0x008000a0, 0xe315a0b2, "cellGcmSys", "cellGcmGetConfiguration"},
            {0x008000b0, 0xefd00f54, "cellGcmSys", "cellGcmUnmapEaIoAddress"},
            {0x008000c0, 0x06edea9e, "cellGcmSys", "cellGcmSetUserHandler"},
            {0x008000d0, 0x21397818, "cellGcmSys", "_cellGcmSetFlipCommand"},
            {0x008000e0, 0x4524cccd, "cellGcmSys", "cellGcmBindTile"},
            {0x008000f0, 0x4ae8d215, "cellGcmSys", "cellGcmSetFlipMode"},
            {0x00800100, 0x63387071, "cellGcmSys", "cellGcmGetLastFlipTime"},
            {0x00800110, 0x9dc04436, "cellGcmSys", "cellGcmBindZcull"},
            {0x00800120, 0xa41ef7e8, "cellGcmSys", "cellGcmSetFlipHandler"},
            {0x00800130, 0xa53d12ae, "cellGcmSys", "cellGcmSetDisplayBuffer"},
            {0x00800140, 0xa75640e8, "cellGcmSys", "cellGcmUnbindZcull"},
            {0x00800150, 0xa91b0402, "cellGcmSys", "cellGcmSetVBlankHandler"},
            {0x00800160, 0xb2e761d4, "cellGcmSys", "cellGcmResetFlipStatus"},
            {0x00800170, 0xbd100dbc, "cellGcmSys", "cellGcmSetTileInfo"},
            {0x00800180, 0xd8f88e1a, "cellGcmSys", "_cellGcmSetFlipCommandWithWaitLabel"},
            {0x00800190, 0xd9b7653e, "cellGcmSys", "cellGcmUnbindTile"},
            {0x008001a0, 0x72a577ce, "cellGcmSys", "cellGcmGetFlipStatus"},
            {0x008001b0, 0xf80196c1, "cellGcmSys", "cellGcmGetLabelAddress"},
    };
    tox2::installImports(imports, ImportCount);
    auto a = makeCaller(0x01000000);
    std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
    Tox2PpuContext* ctx = &a->context();
    ctx->gpr[1] = stack;
    const auto call = [&](std::uint32_t index, std::initializer_list<std::uint64_t> args) {
        int reg = 3;
        for (const std::uint64_t value : args) ctx->gpr[reg++] = value;
        tox2HleCall(ctx, base, index);
        return static_cast<std::uint32_t>(ctx->gpr[3]);
    };
    const auto offsetOf = [&](std::uint32_t address, std::uint32_t& offset) {
        tox2Write32(base, out, 0xdeadbeef);
        const std::uint32_t result = call(AddressToOffset, {address, out});
        offset = tox2Read32(base, out);
        return result;
    };

    // Before initialisation the configuration is zero and nothing is mappable.
    std::memset(base + config, 0xaa, 0x20);
    call(GetConfig, {config});
    CHECK(tox2Read32(base, config) == 0 && tox2Read32(base, config + 8) == 0 && tox2Read32(base, config + 0x14) == 0);
    std::uint32_t offset = 0;
    CHECK(offsetOf(0xc0000000, offset) == kFailure && offset == 0xdeadbeef);
    CHECK(call(TiledPitch, {0x1400}) == 0x1400);
    CHECK(call(GetLabelAddress, {0xc0}) == 0);  // no label area before initialisation
    CHECK(call(GetFlipStatus, {}) == tox2::kRsxFlipStatusDone);

    // Handlers are ignored and the zcull request is refused before initialisation.
    call(SetFlipHandler, {0x1234});
    CHECK(tox2::rsxState().flip_handler == 0);
    CHECK(call(BindZcull, {0, 0x1000, 0x500, 0x300, 0, 2, 0, 0}) == kInval);

    // A failing init releases what it committed and can be retried.
    CHECK(tox2::gcmInitBody(base, out, 0x10000, io_size, io_ea + 0x1000, local_size) == kFailure);
    CHECK(!tox2::rsxState().initialized && !p.memory.isCommitted(tox2::kRsxDeviceAddr, 4));
    CHECK(tox2::gcmInitBody(base, out, 0x10000, io_size, 0x2f000000, local_size) == kFailure);  // uncommitted

    CHECK(tox2::gcmInitBody(base, out, 0x10000, io_size, io_ea, local_size) == 0);
    CHECK(tox2::gcmInitBody(base, out, 0x10000, io_size, io_ea, local_size) == kFailure);  // second init
    const tox2::RsxState& state = tox2::rsxState();
    const std::uint32_t callback = state.wrap_callback;
    CHECK(callback != 0);
    CHECK(tox2Read32(base, out) == 0x10000000);
    CHECK(tox2Read32(base, 0x10000000) == io_ea + 0x1000);
    CHECK(tox2Read32(base, 0x10000004) == io_ea + 0x8000 - 4);
    CHECK(tox2Read32(base, 0x10000008) == io_ea + 0x1000);
    CHECK(tox2Read32(base, 0x1000000c) == callback);
    CHECK(tox2Read32(base, 0x10000030) == 1);
    CHECK(tox2Read32(base, 0x10100040) == 0x1000 && tox2Read32(base, 0x10100044) == 0x1000);
    CHECK(tox2Read32(base, 0x10100048) == 0xffffffff);
    CHECK(tox2Read32(base, 0x10200008) == local_size && tox2Read32(base, 0x10200010) == 500000000);
    CHECK(tox2Read32(base, 0x1020002c) == 0x1000 && tox2Read32(base, 0x10200034) == 0x1400);
    CHECK(tox2Read32(base, 0x10300000) == 0x1337c0d3 && tox2Read32(base, 0x1030000c) == 0x1337f001);
    CHECK(tox2Read32(base, 0x10300ffc) == 0x1337f001);
    CHECK(tox2Read64(base, 0x10301000) == ~std::uint64_t{0} && tox2Read64(base, 0x10301008) == 0);
    CHECK(tox2Read64(base, 0x10301400) == ~std::uint64_t{0} && tox2Read32(base, 0x10301408) == 0);
    CHECK(tox2Read32(base, 0x1030140c) == 0xffffffff);
    CHECK(p.memory.isCommitted(0xc0000000, local_size) && !p.memory.isCommitted(0xc0000000 + local_size, 0x1000));

    std::memset(base + config, 0xaa, 0x20);
    call(GetConfig, {config});
    CHECK(tox2Read32(base, config + 0x00) == 0xc0000000 && tox2Read32(base, config + 0x04) == io_ea);
    CHECK(tox2Read32(base, config + 0x08) == local_size && tox2Read32(base, config + 0x0c) == io_size);
    CHECK(tox2Read32(base, config + 0x10) == 650000000 && tox2Read32(base, config + 0x14) == 500000000);
    CHECK(tox2Read8(base, config + 0x18) == 0xaa);

    CHECK(call(ControlRegister, {}) == 0x10100040);
    CHECK(call(CommandWords, {}) == 0x400 && call(SegmentWords, {}) == 0x100);
    CHECK(call(FifoSize, {0x20000, 0x1000}) == 0);
    ctx->gpr[3] = 0x1234;
    call(Func15, {0x10000000});
    CHECK(ctx->gpr[3] == 0x10000000);  // void: r3 is not written
    CHECK(call(TimeStamp, {0}) == 0xffffffff && ctx->gpr[3] == ~std::uint64_t{0});
    CHECK(call(TimeStamp, {2047}) == 0xffffffff);
    CHECK(call(TimeStamp, {2048}) == 0 && ctx->gpr[3] == 0);

    // Labels are 16 bytes apart in the semaphore area, which is where the consumer's semaphore methods and the
    // flip-with-label wait offsets (index * 16) point; the index is a u8.
    CHECK(call(GetLabelAddress, {0}) == 0x10300000 && call(GetLabelAddress, {1}) == 0x10300010);
    CHECK(call(GetLabelAddress, {0xc0}) == 0x10300c00 && call(GetLabelAddress, {255}) == 0x10300ff0);
    CHECK(call(GetLabelAddress, {0x1c0}) == 0x10300c00);
    CHECK(p.memory.isCommitted(call(GetLabelAddress, {255}), 16));

    // cellGcmGetTiledPitchSize
    CHECK(call(TiledPitch, {0}) == 0 && call(TiledPitch, {1}) == 0x200);
    CHECK(call(TiledPitch, {0x200}) == 0x200 && call(TiledPitch, {0x201}) == 0x300);
    CHECK(call(TiledPitch, {0x1400}) == 0x1400 && call(TiledPitch, {0x10000}) == 0x10000);
    CHECK(call(TiledPitch, {0x10001}) == 0);

    // The IO area mapped at init: io page 0. cellGcmMapMainMemory takes the first free pages.
    CHECK(offsetOf(io_ea + 8, offset) == 0 && offset == 8);
    const std::uint32_t ea1 = io_ea + 0x100000, ea2 = io_ea + 0x200000;
    tox2Write32(base, out, 0);
    CHECK(call(MapMain, {ea1, 0x100000, out}) == 0 && tox2Read32(base, out) == 0x100000);
    CHECK(call(MapMain, {ea2, 0x200000, out}) == 0 && tox2Read32(base, out) == 0x200000);
    CHECK(offsetOf(ea1 + 0x1234, offset) == 0 && offset == 0x101234);
    CHECK(offsetOf(ea2 + 0x1fffff, offset) == 0 && offset == 0x3fffff);
    std::uint32_t ea = 0;
    CHECK(tox2::rsxIoToEa(0x101234, ea) && ea == ea1 + 0x1234);
    CHECK(!tox2::rsxIoToEa(0x400000, ea) && !tox2::rsxIoToEa(0x10000000, ea));

    // Rejected mappings write nothing.
    tox2Write32(base, out, 0xdeadbeef);
    CHECK(call(MapMain, {ea1, 0, out}) == kFailure);
    CHECK(call(MapMain, {ea1 + 0x1000, 0x100000, out}) == kFailure);
    CHECK(call(MapMain, {ea1, 0x180000, out}) == kFailure);
    CHECK(call(MapMain, {0x2f000000, 0x100000, out}) == kInval);  // not committed guest memory
    CHECK(call(MapMain, {ea1, 0x10000000, out}) == kNoIoPageTable);
    CHECK(tox2Read32(base, out) == 0xdeadbeef);

    // Local memory, unmapped addresses and the end of the local range.
    CHECK(offsetOf(0xc0000000, offset) == 0 && offset == 0);
    CHECK(offsetOf(0xc0000000 + local_size - 4, offset) == 0 && offset == local_size - 4);
    CHECK(offsetOf(0xc0000000 + local_size, offset) == kFailure && offset == 0xdeadbeef);
    CHECK(offsetOf(0xcff00000, offset) == kFailure);
    CHECK(offsetOf(0x00010000, offset) == kFailure);

    // cellGcmUnmapEaIoAddress frees the whole mapping that starts at the page of `ea` (low bits are
    // ignored); an address inside a larger mapping is not its start.
    CHECK(call(UnmapEa, {ea2 + 0x100000}) == kFailure);
    CHECK(call(UnmapEa, {ea2 + 0x1234}) == 0);
    CHECK(offsetOf(ea2, offset) == kFailure && offsetOf(ea2 + 0x100000, offset) == kFailure);
    CHECK(call(UnmapEa, {ea2}) == kFailure);
    CHECK(call(UnmapEa, {0xc0000000}) == kFailure);
    CHECK(call(UnmapEa, {ea1}) == 0);
    CHECK(call(MapMain, {ea2 + 0x100000, 0x100000, out}) == 0 && tox2Read32(base, out) == 0x100000);
    CHECK(offsetOf(ea2 + 0x100000, offset) == 0 && offset == 0x100000);

    // The wrap callback flushes put, jumps to the next fragment and wraps around after the last.
    const std::uint32_t context = tox2::kRsxDeviceAddr;
    const auto wrap = [&] { return tox2::callGuestFunction(ctx, base, callback, {context, 0}); };
    tox2Write32(base, context + 8, io_ea + 0x1010);
    tox2Write32(base, 0x10100044, 0x1000);
    CHECK(wrap() == 0);
    CHECK(tox2Read32(base, 0x10100040) == 0x1010);
    CHECK(tox2Read32(base, io_ea + 0x1010) == (0x20000000 | 0x8000));
    CHECK(tox2Read32(base, context) == io_ea + 0x8000 && tox2Read32(base, context + 8) == io_ea + 0x8000);
    CHECK(tox2Read32(base, context + 4) == io_ea + 0x10000 - 4);
    CHECK(ctx->gpr[1] == stack);

    tox2Write32(base, 0x10100044, 0x9000);  // the consumer is past the new fragment
    tox2Write32(base, context + 8, io_ea + 0x8040);
    CHECK(wrap() == 0);
    CHECK(tox2Read32(base, 0x10100040) == 0x8040);
    CHECK(tox2Read32(base, io_ea + 0x8040) == (0x20000000 | 0x1000));
    CHECK(tox2Read32(base, context) == io_ea + 0x1000 && tox2Read32(base, context + 8) == io_ea + 0x1000);
    CHECK(tox2Read32(base, context + 4) == io_ea + 0x8000 - 4);

    // With get inside the new fragment the callback waits until the consumer moves it.
    tox2Write32(base, 0x10100044, 0x8010);
    tox2Write32(base, context + 8, io_ea + 0x1020);
    std::thread consumer([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::lock_guard<tox2::GuestLock> lock(tox2::guestLock());
        tox2Write32(tox2::process().memory.base(), 0x10100044, 0x1020);
        tox2::rsxGetAdvanced().notify_all();
    });
    const auto started = std::chrono::steady_clock::now();
    CHECK(wrap() == 0);
    CHECK(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(80));
    CHECK(tox2Read32(base, context) == io_ea + 0x8000);
    consumer.join();

    // Display buffers, flip mode and status, handlers.
    CHECK(call(SetDisplayBuffer, {1, 0x100, 0x2000, 1280, 720}) == 0);
    CHECK(state.display_count == 2 && state.display[1].offset == 0x100 && state.display[1].pitch == 0x2000);
    CHECK(state.display[1].width == 1280 && state.display[1].height == 720);
    CHECK(call(SetDisplayBuffer, {8, 1, 2, 3, 4}) == kFailure && state.display_count == 2);
    CHECK(call(SetDisplayBuffer, {0x101, 1, 2, 3, 4}) == 0 && state.display[1].offset == 1);  // 8-bit id
    call(SetFlipMode, {1});
    CHECK(!state.requested_vsync);
    call(SetFlipMode, {2});
    CHECK(state.requested_vsync);
    CHECK(state.flip_status == tox2::kRsxFlipStatusDone);
    CHECK(call(GetFlipStatus, {}) == 0);
    call(ResetFlipStatus, {});
    CHECK(state.flip_status == tox2::kRsxFlipStatusWaiting);
    CHECK(call(GetFlipStatus, {}) == 1);
    call(GetLastFlipTime, {});
    CHECK(ctx->gpr[3] == state.last_flip_time);
    call(SetFlipHandler, {0x1111});
    call(SetVBlankHandler, {0x2222});
    call(SetUserHandler, {0x3333});
    CHECK(state.flip_handler == 0x1111 && state.vblank_handler == 0x2222 && state.user_handler == 0x3333);

    // Old-API flip commands: header = method | count << 18 at `current`, no put update.
    const std::uint32_t cmd = io_ea + 0x2000;
    tox2Write32(base, context + 8, cmd);
    tox2Write32(base, 0x10100040, 0x4444);
    CHECK(call(SetFlipCommand, {context, 3}) == context);  // void: r3 is not written
    CHECK(tox2Read32(base, cmd) == 0x4feac && tox2Read32(base, cmd + 4) == 3);
    CHECK(tox2Read32(base, context + 8) == cmd + 8 && tox2Read32(base, 0x10100040) == 0x4444);
    tox2Write32(base, cmd + 8, 0xdeadbeef);
    call(SetFlipCommand, {context, 8});
    CHECK(tox2Read32(base, cmd + 8) == 0xdeadbeef && tox2Read32(base, context + 8) == cmd + 8);
    call(FlipWithLabel, {context, 2, 5, 0x77});
    CHECK(tox2Read32(base, cmd + 8) == 0x80064 && tox2Read32(base, cmd + 12) == 0x50 &&
          tox2Read32(base, cmd + 16) == 0x77);
    CHECK(tox2Read32(base, cmd + 20) == 0x4feac && tox2Read32(base, cmd + 24) == 2);
    CHECK(tox2Read32(base, context + 8) == cmd + 28);
    CHECK(call(GetLabelAddress, {5}) == tox2::kRsxLabelAddr + tox2Read32(base, cmd + 12));  // the label it waits on
    call(FlipWithLabel, {context, 9, 1, 2});  // the label wait is written, the flip is refused
    CHECK(tox2Read32(base, cmd + 28) == 0x80064 && tox2Read32(base, context + 8) == cmd + 40);
    CHECK(countContaining(lines, "display buffer id 8 is out of range") == 1);
    CHECK(countContaining(lines, "display buffer id 9 is out of range") == 1);

    // Tiles: validation order and error codes of RPCS3.
    constexpr std::uint32_t kInvalidEnum = 0x80210002, kInvalidValue = 0x80210003, kInvalidAlign = 0x80210004;
    CHECK(call(SetTileInfo, {5, 0, 0, 0x3a0000, 0x1400, 7, 0, 0}) == 0);
    CHECK(state.tiles[5].size == 0x3a0000 && state.tiles[5].pitch == 0x1400 && state.tiles[5].comp == 7);
    CHECK(!state.tiles[5].bound);
    CHECK(call(BindTile, {5}) == 0 && state.tiles[5].bound);
    CHECK(call(UnbindTile, {5}) == 0 && !state.tiles[5].bound);
    CHECK(call(BindTile, {15}) == kInvalidValue && call(UnbindTile, {15}) == kInvalidValue);
    CHECK(call(SetTileInfo, {15, 0, 0, 0x10000, 0x100, 0, 0, 0}) == kInvalidValue);
    CHECK(call(SetTileInfo, {1, 0, 0, 0x10000, 0x100, 0, 2048, 0}) == kInvalidValue);
    CHECK(call(SetTileInfo, {1, 0, 0, 0x10000, 0x100, 0, 0, 4}) == kInvalidValue);
    CHECK(call(SetTileInfo, {1, 0, 0x10, 0x10000, 0x100, 0, 0, 0}) == kInvalidAlign);
    CHECK(call(SetTileInfo, {1, 0, 0, 0x10001, 0x100, 0, 0, 0}) == kInvalidAlign);
    CHECK(call(SetTileInfo, {1, 0, 0, 0x10000, 0x80, 0, 0, 0}) == kInvalidAlign);
    CHECK(call(SetTileInfo, {1, 2, 0, 0x10000, 0x100, 0, 0, 0}) == kInvalidEnum);
    CHECK(call(SetTileInfo, {1, 0, 0, 0x10000, 0x100, 6, 0, 0}) == kInvalidEnum);
    CHECK(call(SetTileInfo, {1, 0, 0, 0x10000, 0x100, 13, 0, 0}) == kInvalidEnum);
    CHECK(call(SetTileInfo, {1, 1, 0, 0x10000, 0x100, 12, 2047, 3}) == 0 && state.tiles[1].bank == 3);
    CHECK(call(SetTileInfo, {0x101, 0, 0, 0x10000, 0x100, 0, 0, 0}) == 0);  // 8-bit index: tile 1

    // Zcull: the last four arguments (zCullFormat, sFunc, sRef, sMask) come from the stack.
    const auto bindZcull = [&](std::uint32_t index, std::uint32_t offset, std::uint32_t width, std::uint32_t height,
                               std::uint32_t cull_start) {
        tox2Write64(base, stack + 0x70, 1);
        tox2Write64(base, stack + 0x78, 3);
        tox2Write64(base, stack + 0x80, 0x41);
        tox2Write64(base, stack + 0x88, 0xff);
        return call(BindZcull, {index, offset, width, height, cull_start, 2, 0, 0});
    };
    CHECK(bindZcull(0, 0x100000, 0x500, 0x300, 0x1000) == 0);
    const tox2::RsxZcull& zcull = state.zculls[0];
    CHECK(zcull.bound && zcull.offset == 0x100000 && zcull.width == 0x500 && zcull.height == 0x300);
    CHECK(zcull.cull_start == 0x1000 && zcull.z_format == 2 && zcull.aa_format == 0 && zcull.zcull_dir == 0);
    CHECK(zcull.zcull_format == 1 && zcull.s_func == 3 && zcull.s_ref == 0x41 && zcull.s_mask == 0xff);
    CHECK(bindZcull(1, 0x100000, 0x500, 0x300, 0x1234) == 0 && state.zculls[1].cull_start == 0x1000);
    CHECK(bindZcull(8, 0x100000, 0x500, 0x300, 0) == 0xffffffef);
    CHECK(bindZcull(2, 0x100000, 0, 0x300, 0) == kInval);             // empty region
    CHECK(bindZcull(2, 0x100000, 0x500, 0x300, 0x300000) == kInval);  // beyond the 3 MiB zcull RAM
    CHECK(bindZcull(2, local_size, 0x500, 0x300, 0) == kInval);       // offset outside local memory
    CHECK(!state.zculls[2].bound);
    CHECK(call(UnbindZcull, {0}) == 0 && !zcull.bound);
    CHECK(call(UnbindZcull, {8}) == kInvalidValue);

    CHECK(countContaining(lines, "tox2: default hle") == 0);
    CHECK(tox2::formatCensus().find("cellGcmSys::cellGcmMapMainMemory: 8 [impl]") != std::string::npos);

    tox2::resetRsxState();
    CHECK(!p.memory.isCommitted(0xc0000000, 4) && !tox2::rsxState().initialized);
    tox2::resetHostFunctions();
    p.user1m.free(area);
    p.main.free(scratch);
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

void testLwmutex() {
    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::resetSyncObjects();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t lw = scratch, lw2 = scratch + 0x20, attr = scratch + 0x40;

    constexpr const char* kModule = "sysPrxForUser";
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x2f85c0ef, kModule, "sys_lwmutex_create"},
            {0x00800010, 0xc3476d0c, kModule, "sys_lwmutex_destroy"},
            {0x00800020, 0x1573dc3f, kModule, "sys_lwmutex_lock"},
            {0x00800030, 0xaeb78725, kModule, "sys_lwmutex_trylock"},
            {0x00800040, 0x1bc200f4, kModule, "sys_lwmutex_unlock"},
    };
    tox2::installImports(imports, 5);
    enum { kCreate, kDestroy, kLock, kTryLock, kUnlock };
    auto call = [&](tox2::GuestThread& caller, std::uint32_t index, std::uint64_t r3, std::uint64_t r4 = 0) {
        Tox2PpuContext* ctx = &caller.context();
        ctx->gpr[3] = r3;
        ctx->gpr[4] = r4;
        tox2HleCall(ctx, base, index);
        return ctx->gpr[3];
    };
    auto a = makeCaller(0x01000000);
    auto b = makeCaller(0x01000001);

    tox2Write32(base, attr, 2);
    tox2Write32(base, attr + 4, 0x10);
    CHECK(call(*a, kCreate, lw, attr) == 0);
    CHECK(tox2Read32(base, lw) == tox2::kLwmutexFree);
    CHECK(tox2Read32(base, lw + 8) == 0x12);
    CHECK(tox2Read32(base, lw + 16) == 0x95000000);

    CHECK(call(*a, kLock, lw, 0) == 0);
    CHECK(tox2Read32(base, lw) == 0x01000000);
    CHECK(call(*a, kLock, lw, 0) == 0);  // recursive
    CHECK(tox2Read32(base, lw + 12) == 1);
    CHECK(call(*a, kTryLock, lw) == 0);
    CHECK(tox2Read32(base, lw + 12) == 2);
    CHECK(call(*b, kTryLock, lw) == err(tox2::kCellEbusy));
    CHECK(call(*b, kUnlock, lw) == err(tox2::kCellEperm));
    CHECK(call(*a, kDestroy, lw) == err(tox2::kCellEbusy));
    CHECK(call(*b, kDestroy, lw) == err(tox2::kCellEbusy));
    CHECK(call(*a, kUnlock, lw) == 0);
    CHECK(call(*a, kUnlock, lw) == 0);
    CHECK(tox2Read32(base, lw + 12) == 0);
    CHECK(tox2Read32(base, lw) == 0x01000000);
    CHECK(call(*a, kUnlock, lw) == 0);
    CHECK(tox2Read32(base, lw) == tox2::kLwmutexFree);

    // Contention: the second thread blocks until the owner unlocks, and the waiter count shows in
    // the guest struct meanwhile.
    CHECK(call(*a, kLock, lw, 0) == 0);
    std::atomic<bool> b_ready{false};
    std::uint64_t b_result = ~0ull;
    {
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        std::thread worker([&] {
            std::lock_guard<tox2::GuestLock> worker_guard(tox2::guestLock());
            b_ready = true;
            b_result = call(*b, kLock, lw, 0);
        });
        tox2::guestLock().unlock();
        while (!b_ready) std::this_thread::yield();
        tox2::guestLock().lock();
        CHECK(tox2Read32(base, lw + 4) == 1);
        CHECK(call(*a, kUnlock, lw) == 0);
        tox2::guestLock().unlock();
        worker.join();
        tox2::guestLock().lock();
    }
    CHECK(b_result == 0);
    CHECK(tox2Read32(base, lw) == 0x01000001);
    CHECK(tox2Read32(base, lw + 4) == 0);
    CHECK(call(*b, kUnlock, lw) == 0);

    // A timed lock on a held lwmutex gives up with ETIMEDOUT.
    CHECK(call(*a, kLock, lw, 0) == 0);
    {
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        CHECK(call(*b, kLock, lw, 1000) == err(tox2::kCellEtimedout));
    }
    CHECK(call(*a, kUnlock, lw) == 0);

    // Non-recursive: relock is EDEADLK; a destroyed lwmutex is dead.
    tox2Write32(base, attr, 1);
    tox2Write32(base, attr + 4, 0x20);
    CHECK(call(*a, kCreate, lw2, attr) == 0);
    CHECK(call(*a, kLock, lw2, 0) == 0);
    CHECK(call(*a, kLock, lw2, 0) == err(tox2::kCellEdeadlk));
    CHECK(call(*a, kUnlock, lw2) == 0);
    CHECK(call(*a, kDestroy, lw2) == 0);
    CHECK(tox2Read32(base, lw2) == tox2::kLwmutexDead);
    CHECK(call(*a, kLock, lw2, 0) == err(tox2::kCellEinval));
    CHECK(call(*a, kTryLock, lw2) == err(tox2::kCellEinval));

    tox2Write32(base, attr + 4, 0x30);
    CHECK(call(*a, kCreate, lw2, attr) == err(tox2::kCellEinval));
    tox2Write32(base, attr, 3);
    tox2Write32(base, attr + 4, 0x10);
    CHECK(call(*a, kCreate, lw2, attr) == err(tox2::kCellEinval));

    // The process-internal objects take the first ids of their kinds.
    tox2::resetSyncObjects();
    tox2::createProcessSyncObjects(base);
    const tox2::ProcessSyncObjects& objects = tox2::processSyncObjects();
    CHECK(tox2Read32(base, objects.atexit_lwmutex + 16) == 0x95000000);
    CHECK(tox2Read32(base, objects.prx_lwmutex + 16) == 0x95000100);
    CHECK(objects.once_mutex == 0x85000000 && objects.exit_mutex == 0x85000100);

    p.main.free(scratch);
    tox2::resetSyncObjects();
    tox2::setLogSink({});
    tox2::resetCensus();
}

void testLwcond() {
    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::resetSyncObjects();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t lw = scratch, lc = scratch + 0x40, lc2 = scratch + 0x50, attr = scratch + 0x80,
                        cattr = scratch + 0xa0;

    constexpr const char* kModule = "sysPrxForUser";
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x2f85c0ef, kModule, "sys_lwmutex_create"},
            {0x00800010, 0x1573dc3f, kModule, "sys_lwmutex_lock"},
            {0x00800020, 0x1bc200f4, kModule, "sys_lwmutex_unlock"},
            {0x00800030, 0xda0eb71a, kModule, "sys_lwcond_create"},
            {0x00800040, 0x1c9a942c, kModule, "sys_lwcond_destroy"},
            {0x00800050, 0x2a6d9d51, kModule, "sys_lwcond_wait"},
            {0x00800060, 0xef87a695, kModule, "sys_lwcond_signal"},
    };
    tox2::installImports(imports, 7);
    enum { kMutexCreate, kLock, kUnlock, kCondCreate, kCondDestroy, kWait, kSignal };
    auto call = [&](tox2::GuestThread& caller, std::uint32_t index, std::uint64_t r3, std::uint64_t r4 = 0,
                    std::uint64_t r5 = 0) {
        Tox2PpuContext* ctx = &caller.context();
        ctx->gpr[3] = r3;
        ctx->gpr[4] = r4;
        ctx->gpr[5] = r5;
        tox2HleCall(ctx, base, index);
        return ctx->gpr[3];
    };
    auto a = makeCaller(0x01000000);
    auto b = makeCaller(0x01000001);

    tox2Write32(base, attr, 2);
    tox2Write32(base, attr + 4, 0x10);
    CHECK(call(*a, kMutexCreate, lw, attr) == 0);

    // Create stores the lwmutex address and the object id; ids are never reused.
    CHECK(call(*a, kCondCreate, 0, lw, cattr) == err(tox2::kCellEfault));
    CHECK(call(*a, kCondCreate, lc, 0, cattr) == err(tox2::kCellEfault));
    CHECK(call(*a, kCondCreate, lc, lw, cattr) == 0);
    CHECK(tox2Read32(base, lc) == lw && tox2Read32(base, lc + 4) == 0x97000000);
    CHECK(call(*a, kCondCreate, lc2, lw, cattr) == 0);
    CHECK(tox2Read32(base, lc2 + 4) == 0x97000100);

    // The caller must own the lwmutex; an unknown id is ESRCH.
    CHECK(call(*a, kWait, lc, 0) == err(tox2::kCellEperm));
    CHECK(call(*a, kLock, lw, 0) == 0);
    CHECK(call(*b, kWait, lc, 0) == err(tox2::kCellEperm));
    CHECK(call(*a, kUnlock, lw) == 0);
    tox2Write32(base, lc2 + 4, 0x97000fff);
    CHECK(call(*a, kWait, lc2, 0) == err(tox2::kCellEsrch));
    CHECK(call(*a, kSignal, lc2) == err(tox2::kCellEsrch));
    CHECK(call(*a, kCondDestroy, lc2) == err(tox2::kCellEsrch));
    tox2Write32(base, lc2 + 4, 0x97000100);

    // A signal without a waiter is dropped; a timed wait gives the lwmutex back with its recursion
    // count restored.
    CHECK(call(*a, kSignal, lc) == 0);
    CHECK(call(*a, kLock, lw, 0) == 0);
    CHECK(call(*a, kLock, lw, 0) == 0);
    {
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        CHECK(call(*a, kWait, lc, 1000) == err(tox2::kCellEtimedout));
    }
    CHECK(tox2Read32(base, lw) == 0x01000000 && tox2Read32(base, lw + 12) == 1);
    CHECK(call(*a, kUnlock, lw) == 0);
    CHECK(call(*a, kUnlock, lw) == 0);
    CHECK(tox2Read32(base, lw) == tox2::kLwmutexFree);

    // Hand-over: the waiter releases the lwmutex completely, a signal sent while another thread owns
    // the lwmutex wakes it, and it returns only once it owns the lwmutex again.
    std::atomic<bool> b_ready{false};
    std::uint64_t b_result = ~0ull;
    {
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        std::thread worker([&] {
            std::lock_guard<tox2::GuestLock> worker_guard(tox2::guestLock());
            call(*b, kLock, lw, 0);
            call(*b, kLock, lw, 0);
            b_ready = true;
            b_result = call(*b, kWait, lc, 0);
        });
        tox2::guestLock().unlock();
        while (!b_ready) std::this_thread::yield();
        tox2::guestLock().lock();
        CHECK(tox2Read32(base, lw) == tox2::kLwmutexFree && tox2Read32(base, lw + 12) == 0);
        CHECK(call(*a, kCondDestroy, lc) == err(tox2::kCellEbusy));
        CHECK(call(*a, kLock, lw, 0) == 0);
        CHECK(call(*a, kSignal, lc) == 0);
        tox2::guestLock().unlock();
        // The woken waiter blocks on the lwmutex that `a` still owns.
        for (;;) {
            tox2::guestLock().lock();
            if (tox2Read32(base, lw + 4) == 1) break;
            tox2::guestLock().unlock();
            std::this_thread::yield();
        }
        CHECK(b_result == ~0ull);
        CHECK(call(*a, kUnlock, lw) == 0);
        tox2::guestLock().unlock();
        worker.join();
        tox2::guestLock().lock();
    }
    CHECK(b_result == 0);
    CHECK(tox2Read32(base, lw) == 0x01000001 && tox2Read32(base, lw + 12) == 1);
    CHECK(call(*b, kUnlock, lw) == 0);
    CHECK(call(*b, kUnlock, lw) == 0);
    CHECK(call(*b, kUnlock, lw) == err(tox2::kCellEperm));

    // Destroy forgets the object.
    CHECK(call(*a, kCondDestroy, lc) == 0);
    CHECK(call(*a, kCondDestroy, lc) == err(tox2::kCellEsrch));
    CHECK(call(*a, kSignal, lc) == err(tox2::kCellEsrch));
    CHECK(call(*a, kCondDestroy, lc2) == 0);

    p.main.free(scratch);
    tox2::resetSyncObjects();
    tox2::setLogSink({});
    tox2::resetCensus();
}

// Polls `done` with the guest lock held; false after 5 s. Seeing a worker's flag this way means the worker, which sets
// it while holding the lock, has since given the lock up in a blocking wait.
template <class Pred>
bool waitUnderGuestLock(Pred done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
            if (done()) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// An unlock gives the lwmutex to the waiter its protocol chooses and leaves the owner word reserved, so the unlocking
// thread cannot take it back; a grant that races with the waiter's timeout still makes it the owner.
void testLwmutexHandoff() {
    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::resetSyncObjects();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t lw = scratch, lc = scratch + 0x40, attr = scratch + 0x80, cattr = scratch + 0xa0;

    constexpr const char* kModule = "sysPrxForUser";
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x2f85c0ef, kModule, "sys_lwmutex_create"},
            {0x00800010, 0x1573dc3f, kModule, "sys_lwmutex_lock"},
            {0x00800020, 0xaeb78725, kModule, "sys_lwmutex_trylock"},
            {0x00800030, 0x1bc200f4, kModule, "sys_lwmutex_unlock"},
            {0x00800040, 0xda0eb71a, kModule, "sys_lwcond_create"},
            {0x00800050, 0x2a6d9d51, kModule, "sys_lwcond_wait"},
            {0x00800060, 0xef87a695, kModule, "sys_lwcond_signal"},
    };
    tox2::installImports(imports, 7);
    enum { kCreate, kLock, kTryLock, kUnlock, kCondCreate, kWait, kSignal };
    auto call = [&](tox2::GuestThread& caller, std::uint32_t index, std::uint64_t r3, std::uint64_t r4 = 0,
                    std::uint64_t r5 = 0) {
        Tox2PpuContext* ctx = &caller.context();
        ctx->gpr[3] = r3;
        ctx->gpr[4] = r4;
        ctx->gpr[5] = r5;
        tox2HleCall(ctx, base, index);
        return ctx->gpr[3];
    };
    auto a = makeCaller(0x01000000);
    auto b = makeCaller(0x01000001);
    auto c = makeCaller(0x01000002);
    auto d = makeCaller(0x01000003);
    auto e = makeCaller(0x01000004);
    a->setPriority(1001);
    b->setPriority(1001);
    c->setPriority(500);
    d->setPriority(1001);
    e->setPriority(1001);
    const tox2::SyncHandoffStats stats_before = tox2::syncHandoffStats();

    // `a` owns the lwmutex and b, c, d queue in that order. After a's unlock the lwmutex is reserved for the chosen
    // waiter and each owner unlocks in turn, so the serving order is the protocol's.
    auto serveOrder = [&](std::uint32_t protocol) {
        tox2Write32(base, attr, protocol);
        tox2Write32(base, attr + 4, 0x10);
        CHECK(call(*a, kCreate, lw, attr) == 0);
        CHECK(call(*a, kLock, lw, 0) == 0);
        std::vector<std::string> served;
        std::vector<std::thread> workers;
        tox2::GuestThread* const callers[] = {b.get(), c.get(), d.get()};
        const char* const names[] = {"b", "c", "d"};
        for (std::uint32_t i = 0; i < 3; i++) {
            workers.emplace_back([&, i] {
                std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
                const bool locked = call(*callers[i], kLock, lw, 0) == 0;
                served.push_back(locked ? names[i] : "failed");
                call(*callers[i], kUnlock, lw);
            });
            CHECK(waitUnderGuestLock([&] { return tox2Read32(base, lw + 4) == i + 1; }));
        }
        {
            // The chosen waiter cannot run before the guest lock is given up.
            std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
            CHECK(call(*a, kUnlock, lw) == 0);
            CHECK(tox2Read32(base, lw) == tox2::kLwmutexReserved);
            CHECK(call(*a, kTryLock, lw) == err(tox2::kCellEbusy));
            CHECK(call(*a, kUnlock, lw) == err(tox2::kCellEperm));
        }
        for (std::thread& worker : workers) worker.join();
        CHECK(tox2Read32(base, lw) == tox2::kLwmutexFree && tox2Read32(base, lw + 4) == 0);
        return served;
    };
    const std::vector<std::string> by_priority = {"c", "b", "d"};
    CHECK(serveOrder(2) == by_priority);
    const tox2::SyncHandoffStats stats_after = tox2::syncHandoffStats();
    CHECK(stats_after.lwmutex_waits - stats_before.lwmutex_waits == 3);
    CHECK(stats_after.lwmutex_handoffs - stats_before.lwmutex_handoffs == 3);
    CHECK(stats_after.mutex_waits == stats_before.mutex_waits);
    const std::vector<std::string> by_arrival = {"b", "c", "d"};
    CHECK(serveOrder(1) == by_arrival);
    CHECK(tox2::syncHandoffStats().lwmutex_handoffs - stats_before.lwmutex_handoffs == 6);

    // The unlock grants the waiter after its timeout expired but before it got the guest lock back: it owns the
    // lwmutex and reports success.
    tox2Write32(base, attr, 2);
    CHECK(call(*a, kCreate, lw, attr) == 0);
    CHECK(call(*a, kLock, lw, 0) == 0);
    std::uint64_t e_result = ~0ull;
    std::thread timed([&] {
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        e_result = call(*e, kLock, lw, 20'000);
    });
    CHECK(waitUnderGuestLock([&] { return tox2Read32(base, lw + 4) == 1; }));
    {
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(call(*a, kUnlock, lw) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    timed.join();
    CHECK(e_result == 0);
    CHECK(tox2Read32(base, lw) == 0x01000004 && tox2Read32(base, lw + 4) == 0);
    CHECK(call(*e, kUnlock, lw) == 0);
    CHECK(tox2Read32(base, lw) == tox2::kLwmutexFree);

    // The release inside lwcond_wait hands the lwmutex over too: b queued before `a` waits on the condition, so
    // right after the wait began the lwmutex is reserved for b. The observer queues for the guest lock before the wait
    // wakes b, so it runs first.
    CHECK(call(*a, kCondCreate, lc, lw, cattr) == 0);
    CHECK(call(*a, kLock, lw, 0) == 0);
    std::uint64_t b_result = ~0ull, a_result = ~0ull;
    std::thread locker([&] {
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        b_result = call(*b, kLock, lw, 0);
        call(*b, kUnlock, lw);
    });
    CHECK(waitUnderGuestLock([&] { return tox2Read32(base, lw + 4) == 1; }));
    std::thread observer;
    std::uint32_t owner_word = 0;
    std::uint64_t try_result = ~0ull, signal_result = ~0ull;
    {
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        observer = std::thread([&] {
            std::lock_guard<tox2::GuestLock> observer_guard(tox2::guestLock());
            owner_word = tox2Read32(base, lw);
            try_result = call(*c, kTryLock, lw);
            signal_result = call(*c, kSignal, lc);
        });
        while (tox2::guestLock().waiterCount() == 0) std::this_thread::yield();
        a_result = call(*a, kWait, lc, 0);
    }
    observer.join();
    locker.join();
    CHECK(owner_word == tox2::kLwmutexReserved);
    CHECK(try_result == err(tox2::kCellEbusy) && signal_result == 0);
    CHECK(b_result == 0 && a_result == 0);
    CHECK(tox2Read32(base, lw) == 0x01000000);  // the waiter took the lwmutex back after b
    CHECK(call(*a, kUnlock, lw) == 0);
    CHECK(tox2Read32(base, lw) == tox2::kLwmutexFree);

    p.main.free(scratch);
    tox2::resetSyncObjects();
    tox2::setLogSink({});
    tox2::resetCensus();
}

// The same for sys_mutex: an unlock, also the one inside sys_cond_wait, makes the chosen waiter the owner at once.
void testMutexHandoff() {
    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::resetSyncObjects();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t id_out = scratch, cond_out = scratch + 8, attr = scratch + 0x40;

    auto a = makeCaller(0x01000000);
    auto b = makeCaller(0x01000001);
    auto c = makeCaller(0x01000002);
    auto d = makeCaller(0x01000003);
    a->setPriority(1001);
    b->setPriority(1001);
    c->setPriority(500);
    d->setPriority(1001);
    const tox2::SyncHandoffStats stats_before = tox2::syncHandoffStats();

    auto serveOrder = [&](std::uint32_t protocol) {
        tox2Write32(base, attr, protocol);
        tox2Write32(base, attr + 4, 0x20);
        CHECK(syscallAs(*a, 100, id_out, attr) == 0);
        const std::uint32_t m = tox2Read32(base, id_out);
        CHECK(syscallAs(*a, 102, m, 0) == 0);
        std::vector<std::string> served;
        std::vector<std::thread> workers;
        std::atomic<std::uint32_t> queued{0};
        tox2::GuestThread* const callers[] = {b.get(), c.get(), d.get()};
        const char* const names[] = {"b", "c", "d"};
        for (std::uint32_t i = 0; i < 3; i++) {
            workers.emplace_back([&, i, m] {
                std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
                queued++;
                const bool locked = syscallAs(*callers[i], 102, m, 0) == 0;
                served.push_back(locked ? names[i] : "failed");
                syscallAs(*callers[i], 104, m);
            });
            CHECK(waitUnderGuestLock([&] { return queued.load() == i + 1; }));
        }
        {
            std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
            CHECK(syscallAs(*a, 104, m) == 0);
            CHECK(syscallAs(*a, 103, m) == err(tox2::kCellEbusy));
            CHECK(syscallAs(*a, 104, m) == err(tox2::kCellEperm));
            CHECK(syscallAs(*a, 101, m) == err(tox2::kCellEbusy));
        }
        for (std::thread& worker : workers) worker.join();
        CHECK(syscallAs(*a, 101, m) == 0);
        return served;
    };
    const std::vector<std::string> by_priority = {"c", "b", "d"};
    CHECK(serveOrder(2) == by_priority);
    const tox2::SyncHandoffStats stats_after = tox2::syncHandoffStats();
    CHECK(stats_after.mutex_waits - stats_before.mutex_waits == 3);
    CHECK(stats_after.mutex_handoffs - stats_before.mutex_handoffs == 3);
    CHECK(stats_after.lwmutex_waits == stats_before.lwmutex_waits);
    const std::vector<std::string> by_arrival = {"b", "c", "d"};
    CHECK(serveOrder(1) == by_arrival);

    // Waiting on the condition variable hands the mutex to the queued waiter, who owns it before it runs. The observer
    // queues for the guest lock before the wait wakes b, so it runs first.
    tox2Write32(base, attr, 2);
    tox2Write32(base, attr + 4, 0x20);
    CHECK(syscallAs(*a, 100, id_out, attr) == 0);
    const std::uint32_t m = tox2Read32(base, id_out);
    CHECK(syscallAs(*a, 105, cond_out, m, attr) == 0);
    const std::uint32_t cv = tox2Read32(base, cond_out);
    CHECK(syscallAs(*a, 102, m, 0) == 0);
    std::uint64_t b_result = ~0ull, a_result = ~0ull;
    std::atomic<bool> b_queued{false};
    std::thread locker([&] {
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        b_queued = true;
        b_result = syscallAs(*b, 102, m, 0);
        syscallAs(*b, 104, m);
    });
    CHECK(waitUnderGuestLock([&] { return b_queued.load(); }));
    std::thread observer;
    std::uint64_t relock_result = ~0ull, try_result = ~0ull, unlock_result = ~0ull, signal_result = ~0ull;
    {
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        observer = std::thread([&] {
            std::lock_guard<tox2::GuestLock> observer_guard(tox2::guestLock());
            // b has not run yet, but the non-recursive relock shows it owns the mutex.
            relock_result = syscallAs(*b, 102, m, 0);
            try_result = syscallAs(*c, 103, m);
            unlock_result = syscallAs(*a, 104, m);
            signal_result = syscallAs(*c, 108, cv);
        });
        while (tox2::guestLock().waiterCount() == 0) std::this_thread::yield();
        a_result = syscallAs(*a, 107, cv, 0);
    }
    observer.join();
    locker.join();
    CHECK(relock_result == err(tox2::kCellEdeadlk) && try_result == err(tox2::kCellEbusy));
    CHECK(unlock_result == err(tox2::kCellEperm) && signal_result == 0);
    CHECK(b_result == 0 && a_result == 0);
    CHECK(syscallAs(*a, 104, m) == 0);  // the waiter owns the mutex again
    CHECK(syscallAs(*a, 104, m) == err(tox2::kCellEperm));

    p.main.free(scratch);
    tox2::resetSyncObjects();
    tox2::setLogSink({});
    tox2::resetCensus();
}

void testGuestPrintf() {
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t fmt_addr = scratch, str_a = scratch + 0x400, str_b = scratch + 0x440, va = scratch + 0x800,
                        stack = scratch + 0x2000;
    auto put = [&](std::uint32_t address, std::string_view text) {
        for (std::size_t i = 0; i < text.size(); i++) tox2Write8(base, address + i, static_cast<std::uint8_t>(text[i]));
        tox2Write8(base, address + text.size(), 0);
    };
    put(str_a, "abcdef");
    put(str_b, "xy");

    auto ctx = std::make_unique<Tox2PpuContext>();
    ctx->gpr[1] = stack;
    std::string error;
    // Slot 0 is the format; `args` fill slots 1.. from registers, then from the parameter save area.
    auto run = [&](std::string_view format, std::initializer_list<std::uint64_t> args) {
        put(fmt_addr, format);
        ctx->gpr[3] = fmt_addr;
        std::uint32_t slot = 1;
        for (const std::uint64_t value : args) {
            if (slot < 8) {
                ctx->gpr[3 + slot] = value;
            } else {
                tox2Write64(base, stack + 0x30 + 8 * slot, value);
            }
            slot++;
        }
        tox2::GuestVarArgs va_args = tox2::GuestVarArgs::fromRegisters(ctx.get(), 1);
        return tox2::formatGuestPrintf(base, fmt_addr, va_args, error);
    };
    auto bitsOf = [](double value) { return std::bit_cast<std::uint64_t>(value); };

    // Integers and flags.
    CHECK(run("%d|%5d|%-5d|%05d|%+d|% d", {static_cast<std::uint64_t>(-5), 42, 42, 42, 42, 42}) ==
          "-5|   42|42   |00042|+42| 42");
    CHECK(error.empty());
    CHECK(run("%d %i", {0xfffffffe, 7}) == "-2 7");  // the low 32 bits, sign-extended
    CHECK(run("%u %lu", {0xffffffffffffffffull, 0xffffffffffffffffull}) == "4294967295 18446744073709551615");
    CHECK(run("%lld %ld", {0x8000000000000000ull, 0x8000000000000000ull}) ==
          "-9223372036854775808 -9223372036854775808");
    CHECK(run("%x %#x %X %o", {0xbeef, 0xbeef, 0xbeef, 8}) == "beef 0xbeef BEEF 10");
    CHECK(run("%02x|%05x|%08x|%2u|% 2u|%02u", {0xa, 0xab, 0xab, 5, 5, 5}) == "0a|000ab|000000ab| 5| 5|05");
    CHECK(run("%lx %llx %08llx %016llx", {0x123456789aull, 0x123456789aull, 0xabcdull, 0xabcdull}) ==
          "123456789a 123456789a 0000abcd 000000000000abcd");
    CHECK(run("%hhd %hd %hhu %hu", {0x1ff, 0x18000, 0x1ff, 0x12345}) == "-1 -32768 255 9029");
    CHECK(run("%zd %jd %td %qd", {1, 2, 3, 4}) == "1 2 3 4");
    CHECK(run("%.3d|%8.3d|%-8.3d|", {5, 5, 5}) == "005|     005|005     |");

    // Characters, strings, pointers.
    CHECK(run("%c%c", {0x141, 'z'}) == "Az");
    {
        const std::string with_nul = run("a%cb", {0});
        CHECK(with_nul.size() == 3 && with_nul[1] == '\0');
    }
    CHECK(run("%s|%.3s|%-6s|%6s|%s", {str_a, str_a, str_b, str_b, 0}) == "abcdef|abc|xy    |    xy|(null)");
    CHECK(run("%-.15s|%.32s|%.0s|", {str_a, str_a, str_a}) == "abcdef|abcdef||");
    CHECK(run("%s", {0x100000000ull + str_b}) == "xy");  // only the low 32 bits are an address
    CHECK(run("%p %p", {0x1234, 0x100000010ull}) == "0x1234 0x10");
    CHECK(run("%s%s%u", {str_b, str_b, 9}) == "xyxy9");

    // Star width and precision, percent.
    CHECK(run("%*d|%*d|%.*s|%*.*s|", {6, 42, static_cast<std::uint64_t>(-6), 42, 2, str_a, 5, 1, str_a}) ==
          "    42|42    |ab|    a|");
    CHECK(run("%.*s", {static_cast<std::uint64_t>(-1), str_b}) == "xy");  // a negative precision is none
    CHECK(run("100%% %d%%", {5}) == "100% 5%");

    // Floating point arrives as the double's bits in the slot.
    CHECK(run("%f|%.2f|%e|%g|%5.1f|%G", {bitsOf(3.5), bitsOf(2.0 / 3.0), bitsOf(1500.0), bitsOf(1e10), bitsOf(2.25),
                                         bitsOf(1e-10)}) == "3.500000|0.67|1.500000e+03|1e+10|  2.2|1E-10");
    CHECK(run("%Lf", {bitsOf(0.5)}) == "0.500000");

    // Slots 0-7 come from r3..r10, slot 8 and up from the parameter save area at r1 + 0x30 + 8k.
    CHECK(run("%d %d %d %d %d %d %d %d %d", {1, 2, 3, 4, 5, 6, 7, 8, 9}) == "1 2 3 4 5 6 7 8 9");
    CHECK(tox2Read64(base, stack + 0x30 + 8 * 8) == 8 && tox2Read64(base, stack + 0x30 + 8 * 9) == 9);
    {
        // snprintf-style: the variable arguments start at slot 3.
        put(fmt_addr, "%d-%d");
        ctx->gpr[6] = 11;
        ctx->gpr[7] = 22;
        tox2::GuestVarArgs from_third = tox2::GuestVarArgs::fromRegisters(ctx.get(), 3);
        CHECK(tox2::formatGuestPrintf(base, fmt_addr, from_third, error) == "11-22" && error.empty());
        CHECK(from_third.slot == 5);
    }

    // A va list: 8-byte slots at the pointer.
    {
        put(fmt_addr, "%s=%d");
        tox2Write64(base, va, str_a);
        tox2Write64(base, va + 8, static_cast<std::uint64_t>(-3));
        tox2::GuestVarArgs list = tox2::GuestVarArgs::fromVaList(va);
        CHECK(tox2::formatGuestPrintf(base, fmt_addr, list, error) == "abcdef=-3" && error.empty());
        CHECK(list.address == va + 16);
    }

    // Unsupported conversions stop the output and name themselves.
    CHECK(run("abc%n def", {0}) == "abc");
    CHECK(error.find("%n") != std::string::npos);
    CHECK(run("x%y", {0}) == "x" && !error.empty());
    CHECK(run("done%", {}) == "done" && !error.empty());
    CHECK(run("%99999999d", {1}) == "" && !error.empty());
    CHECK(run("ok", {}) == "ok" && error.empty());  // the error is cleared by the next call

    p.main.free(scratch);
}

void testSysPrxForUserLibc() {
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetCensus();
    tox2::resetLogOnce();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t a = scratch, b = scratch + 0x100, c = scratch + 0x200, fmt = scratch + 0x300,
                        va = scratch + 0x400, stack = scratch + 0x1000;

    constexpr const char* kModule = "sysPrxForUser";
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x2d36462b, kModule, "_sys_strlen"},    {0x00800010, 0x99c88692, kModule, "_sys_strcpy"},
            {0x00800020, 0x052d29a6, kModule, "_sys_strcat"},    {0x00800030, 0xd3039d4d, kModule, "_sys_strncpy"},
            {0x00800040, 0x996f7cf8, kModule, "_sys_strncat"},   {0x00800050, 0x04e83d2c, kModule, "_sys_strncmp"},
            {0x00800060, 0xfb5db080, kModule, "_sys_memcmp"},    {0x00800070, 0x68b9b011, kModule, "_sys_memset"},
            {0x00800080, 0x6bf66ea7, kModule, "_sys_memcpy"},    {0x00800090, 0x9f04f7af, kModule, "_sys_printf"},
            {0x008000a0, 0xfa7f693d, kModule, "_sys_vprintf"},   {0x008000b0, 0x06574237, kModule, "_sys_snprintf"},
            {0x008000c0, 0x0618936b, kModule, "_sys_vsnprintf"},
    };
    tox2::installImports(imports, 13);
    enum {
        kStrlen,
        kStrcpy,
        kStrcat,
        kStrncpy,
        kStrncat,
        kStrncmp,
        kMemcmp,
        kMemset,
        kMemcpy,
        kPrintf,
        kVprintf,
        kSnprintf,
        kVsnprintf
    };
    auto ctx = std::make_unique<Tox2PpuContext>();
    ctx->gpr[1] = stack;
    auto call = [&](std::uint32_t index, std::uint64_t r3 = 0, std::uint64_t r4 = 0, std::uint64_t r5 = 0,
                    std::uint64_t r6 = 0, std::uint64_t r7 = 0, std::uint64_t r8 = 0) {
        ctx->gpr[3] = r3;
        ctx->gpr[4] = r4;
        ctx->gpr[5] = r5;
        ctx->gpr[6] = r6;
        ctx->gpr[7] = r7;
        ctx->gpr[8] = r8;
        tox2HleCall(ctx.get(), base, index);
        return ctx->gpr[3];
    };
    auto put = [&](std::uint32_t address, std::string_view text) {
        for (std::size_t i = 0; i < text.size(); i++) tox2Write8(base, address + i, static_cast<std::uint8_t>(text[i]));
        tox2Write8(base, address + text.size(), 0);
    };
    auto get = [&](std::uint32_t address) { return tox2::readGuestString(base, address, 0x100); };
    auto fill = [&](std::uint32_t address, std::uint8_t value, std::uint32_t count) {
        for (std::uint32_t i = 0; i < count; i++) tox2Write8(base, address + i, value);
    };

    // strlen, strcpy, strcat: the result of the last two is the destination.
    put(a, "hello");
    CHECK(call(kStrlen, a) == 5);
    CHECK(call(kStrlen, c) == 0);
    fill(b, 0xaa, 32);
    CHECK(call(kStrcpy, b, a) == b && get(b) == "hello" && tox2Read8(base, b + 6) == 0xaa);
    put(c, " world");
    CHECK(call(kStrcat, b, c) == b && get(b) == "hello world");

    // strncpy pads with NULs up to n and leaves the string unterminated when the source is longer.
    fill(b, 0xaa, 32);
    CHECK(call(kStrncpy, b, a, 8) == b);
    CHECK(get(b) == "hello" && tox2Read8(base, b + 5) == 0 && tox2Read8(base, b + 7) == 0 &&
          tox2Read8(base, b + 8) == 0xaa);
    fill(b, 0xaa, 32);
    call(kStrncpy, b, a, 3);
    CHECK(tox2Read8(base, b + 2) == 'l' && tox2Read8(base, b + 3) == 0xaa);

    // strncat appends at most n characters and then a NUL.
    put(b, "ab");
    put(c, "cdefgh");
    CHECK(call(kStrncat, b, c, 3) == b && get(b) == "abcde" && tox2Read8(base, b + 5) == 0);
    put(b, "ab");
    call(kStrncat, b, c, 100);
    CHECK(get(b) == "abcdefgh");

    // strncmp stops at NUL and at n; memcmp does neither. The result is the unsigned byte difference.
    put(a, "abc");
    put(b, "abd");
    CHECK(call(kStrncmp, a, b, 3) == err(static_cast<std::uint32_t>(-1)));
    CHECK(call(kStrncmp, b, a, 3) == 1);
    CHECK(call(kStrncmp, a, b, 2) == 0);
    CHECK(call(kStrncmp, a, b, 0) == 0);
    put(a, "ab");
    tox2Write8(base, a + 3, 'x');
    put(b, "ab");
    tox2Write8(base, b + 3, 'y');
    CHECK(call(kStrncmp, a, b, 4) == 0);
    CHECK(call(kMemcmp, a, b, 4) == err(static_cast<std::uint32_t>(-1)));
    tox2Write8(base, a, 0xff);
    CHECK(call(kMemcmp, a, b, 1) == 0xff - 'a');
    CHECK(call(kMemcmp, a, b, 0) == 0);

    // memset stores the low byte; memcpy copes with overlap in both directions.
    fill(a, 0, 16);
    CHECK(call(kMemset, a, 0x1234, 4) == a);
    CHECK(tox2Read8(base, a) == 0x34 && tox2Read8(base, a + 3) == 0x34 && tox2Read8(base, a + 4) == 0);
    put(a, "0123456789");
    CHECK(call(kMemcpy, a + 2, a, 6) == a + 2);
    CHECK(get(a) == "0101234589");
    put(a, "0123456789");
    CHECK(call(kMemcpy, a, a + 2, 6) == a);
    CHECK(get(a) == "2345676789");

    // printf returns the length; its arguments start at slot 1.
    put(fmt, "%s=%d\n");
    put(c, "key");
    CHECK(call(kPrintf, fmt, c, 42) == 7);
    CHECK(countContaining(lines, "tox2: printf:") == 0);

    // vprintf reads 8-byte slots at the pointer.
    tox2Write64(base, va, c);
    tox2Write64(base, va + 8, static_cast<std::uint64_t>(-1));
    put(fmt, "%s:%d\n");
    CHECK(call(kVprintf, fmt, va) == 7);

    // snprintf returns the full length and cuts the output to size - 1 bytes plus a NUL. Its arguments
    // start at slot 3.
    put(fmt, "%s%s%u");
    put(a, "ab");
    put(b, "cd");
    fill(c, 0xaa, 16);
    CHECK(call(kSnprintf, c, 16, fmt, a, b, 123) == 7 && get(c) == "abcd123");
    fill(c, 0xaa, 16);
    CHECK(call(kSnprintf, c, 5, fmt, a, b, 123) == 7 && get(c) == "abcd" && tox2Read8(base, c + 5) == 0xaa);
    CHECK(call(kSnprintf, c, 0, fmt, a, b, 123) == 7 && get(c) == "abcd");
    CHECK(call(kSnprintf, c, 1, fmt, a, b, 123) == 7 && tox2Read8(base, c) == 0 && tox2Read8(base, c + 1) == 'b');

    // vsnprintf takes its arguments from the va list.
    put(fmt, "%s-%x");
    put(c, "id");
    tox2Write64(base, va, c);
    tox2Write64(base, va + 8, 0xbeef);
    fill(a, 0xaa, 16);
    CHECK(call(kVsnprintf, a, 16, fmt, va) == 7 && get(a) == "id-beef");

    // An unsupported conversion is logged once per format address; what came before it is printed.
    put(fmt, "ab%n");
    lines.clear();
    CHECK(call(kPrintf, fmt) == 2);
    CHECK(call(kPrintf, fmt) == 2);
    CHECK(countContaining(lines, "tox2: printf:") == 1);

    p.main.free(scratch);
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

void testSpuImageImport() {
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetLogOnce();
    tox2::resetSpuImages();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t image = scratch, elf = scratch + 0x100;

    // An SPU ELF32 with one PT_LOAD (0x40 bytes in the file, 0x60 in memory) and a PT_NOTE.
    auto writeElf = [&](std::uint16_t machine, std::uint32_t load_type) {
        for (std::uint32_t i = 0; i < 0x200; i++) tox2Write8(base, elf + i, 0);
        tox2Write32(base, elf, 0x7F454C46);
        tox2Write8(base, elf + 4, 1);
        tox2Write8(base, elf + 5, 2);
        tox2Write16(base, elf + 0x12, machine);
        tox2Write32(base, elf + 0x18, 0x818);
        tox2Write32(base, elf + 0x1C, 0x34);
        tox2Write16(base, elf + 0x2A, 0x20);
        tox2Write16(base, elf + 0x2C, 2);
        tox2Write32(base, elf + 0x34, load_type);
        tox2Write32(base, elf + 0x38, 0x100);
        tox2Write32(base, elf + 0x3C, 0x800);
        tox2Write32(base, elf + 0x44, 0x40);
        tox2Write32(base, elf + 0x48, 0x60);
        tox2Write32(base, elf + 0x54, 4);
    };
    writeElf(23, 1);

    // Copy and fill segments, the struct words, and the record found by the segment array.
    CHECK(tox2::spuImageImport(base, image, elf, 1) == 0);
    const std::uint32_t segs = tox2Read32(base, image + 8);
    CHECK(tox2Read32(base, image) == 0 && tox2Read32(base, image + 4) == 0x818 && segs != 0 &&
          tox2Read32(base, image + 12) == 2);
    CHECK(tox2Read32(base, segs) == 1 && tox2Read32(base, segs + 4) == 0x800 && tox2Read32(base, segs + 8) == 0x40 &&
          tox2Read32(base, segs + 12) == elf + 0x100);
    CHECK(tox2Read32(base, segs + 16) == 2 && tox2Read32(base, segs + 20) == 0x840 &&
          tox2Read32(base, segs + 24) == 0x20 && tox2Read32(base, segs + 28) == 0);
    const tox2::SpuImageRecord* record = tox2::findSpuImage(segs);
    CHECK(record != nullptr);
    if (record != nullptr) {
        CHECK(record->source == elf && record->import_type == 1 && record->entry == 0x818 && record->segs == segs &&
              record->nsegs == 2);
    }
    CHECK(tox2::findSpuImage(segs + 16) == nullptr && tox2::findSpuImage(0) == nullptr);
    CHECK(countContaining(lines, "tox2: spu: image import 0x") == 1 && countContaining(lines, "2 segments") == 1);

    // Close frees the array and forgets the record; the image struct still names it, so a second close
    // finds nothing.
    CHECK(tox2::spuImageClose(base, image) == 0);
    CHECK(tox2::findSpuImage(segs) == nullptr);
    CHECK(!p.memory.isCommitted(segs, 16));
    CHECK(tox2::spuImageClose(base, image) == tox2::kCellEsrch);
    CHECK(tox2::spuImageClose(base, 0) == tox2::kCellEfault);

    // Argument errors; PROTECT is accepted and noted once.
    CHECK(tox2::spuImageImport(base, image, elf, 2) == tox2::kCellEinval);
    CHECK(tox2::spuImageImport(base, 0, elf, 1) == tox2::kCellEfault);
    CHECK(tox2::spuImageImport(base, image, 0, 1) == tox2::kCellEfault);
    lines.clear();
    CHECK(tox2::spuImageImport(base, image, elf, 0) == 0);
    CHECK(tox2::findSpuImage(tox2Read32(base, image + 8)) != nullptr);
    CHECK(tox2::findSpuImage(tox2Read32(base, image + 8))->import_type == 0);
    CHECK(tox2::spuImageClose(base, image) == 0);
    CHECK(tox2::spuImageImport(base, image, elf, 0) == 0);
    CHECK(tox2::spuImageClose(base, image) == 0);
    CHECK(countContaining(lines, "SYS_SPU_IMAGE_PROTECT") == 1);

    // Not an SPU ELF, or no PT_LOAD at all.
    tox2Write32(base, elf, 0x7F454C47);
    CHECK(tox2::spuImageImport(base, image, elf, 1) == tox2::kCellEnoexec);
    writeElf(20, 1);
    CHECK(tox2::spuImageImport(base, image, elf, 1) == tox2::kCellEnoexec);
    writeElf(23, 4);
    CHECK(tox2::spuImageImport(base, image, elf, 1) == tox2::kCellEnoexec);
    CHECK(tox2::kCellEnoexec == 0x80010007);

    // The import handler goes through the same code.
    writeElf(23, 1);
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0xebe5f72f, "sysPrxForUser", "sys_spu_image_import"},
            {0x00800010, 0xe0da8efd, "sysPrxForUser", "sys_spu_image_close"},
    };
    tox2::installImports(imports, 2);
    auto ctx = std::make_unique<Tox2PpuContext>();
    ctx->gpr[3] = image;
    ctx->gpr[4] = elf;
    ctx->gpr[5] = 1;
    tox2HleCall(ctx.get(), base, 0);
    CHECK(ctx->gpr[3] == 0 && tox2Read32(base, image + 12) == 2);
    ctx->gpr[3] = image;
    tox2HleCall(ctx.get(), base, 1);
    CHECK(ctx->gpr[3] == 0);
    ctx->gpr[3] = image;
    tox2HleCall(ctx.get(), base, 1);
    CHECK(ctx->gpr[3] == err(tox2::kCellEsrch));

    p.main.free(scratch);
    tox2::resetSpuImages();
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

std::uint64_t g_printf_callback_group, g_printf_callback_runs;

TOX2_PPU_FUNC(spuPrintfAttachCallback) {
    (void)base;
    g_printf_callback_runs++;
    g_printf_callback_group = ctx->gpr[3];
    ctx->gpr[3] = 0;
}

TOX2_PPU_FUNC(spuPrintfDetachCallback) {
    (void)base;
    g_printf_callback_runs++;
    g_printf_callback_group = ctx->gpr[3];
    ctx->gpr[3] = 0x80010002;
}

void testSpuPrintfCallbacks() {
    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::resetHostFunctions();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x45fe2fce, "sysPrxForUser", "_sys_spu_printf_initialize"},
            {0x00800010, 0xdd3b27ac, "sysPrxForUser", "_sys_spu_printf_finalize"},
            {0x00800020, 0xdd0c1e09, "sysPrxForUser", "_sys_spu_printf_attach_group"},
            {0x00800030, 0x5fdfb2fe, "sysPrxForUser", "_sys_spu_printf_detach_group"},
    };
    tox2::installImports(imports, 4);
    enum { kInitialize, kFinalize, kAttach, kDetach };
    auto ctx = std::make_unique<Tox2PpuContext>();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    ctx->gpr[1] = scratch + 0x8000;
    auto call = [&](std::uint32_t index, std::uint64_t r3 = 0, std::uint64_t r4 = 0, std::uint64_t r5 = 0,
                    std::uint64_t r6 = 0) {
        ctx->gpr[3] = r3;
        ctx->gpr[4] = r4;
        ctx->gpr[5] = r5;
        ctx->gpr[6] = r6;
        tox2HleCall(ctx.get(), base, index);
        return ctx->gpr[3];
    };

    // No server registered yet: both calls report a wrong state. An earlier test may have left callbacks
    // behind, so start from finalize.
    CHECK(call(kFinalize) == 0);
    CHECK(call(kAttach, 0x04000100) == err(tox2::kCellEstat));
    CHECK(call(kDetach, 0x04000100) == err(tox2::kCellEstat));

    const std::uint32_t attach = tox2::registerHostFunction("spuPrintfAttachCallback", spuPrintfAttachCallback);
    const std::uint32_t detach = tox2::registerHostFunction("spuPrintfDetachCallback", spuPrintfDetachCallback);
    CHECK(attach != 0 && detach != 0);
    CHECK(call(kInitialize, attach, detach, 0x1234, 0x5678) == 0);

    // The group id reaches the callback and the callback's result comes back.
    g_printf_callback_runs = 0;
    CHECK(call(kAttach, 0x04000100) == 0);
    CHECK(g_printf_callback_runs == 1 && g_printf_callback_group == 0x04000100);
    CHECK(call(kDetach, 0x04000200) == err(tox2::kCellEinval));
    CHECK(g_printf_callback_runs == 2 && g_printf_callback_group == 0x04000200);

    // Finalize forgets the callbacks.
    CHECK(call(kFinalize) == 0);
    CHECK(call(kAttach, 0x04000100) == err(tox2::kCellEstat));
    CHECK(g_printf_callback_runs == 2);

    // A server that registers no group callback is the same as none.
    CHECK(call(kInitialize, 0, 0, 0, 0) == 0);
    CHECK(call(kAttach, 0x04000100) == err(tox2::kCellEstat));
    CHECK(call(kFinalize) == 0);

    p.main.free(scratch);
    tox2::resetHostFunctions();
    tox2::setLogSink({});
    tox2::resetCensus();
}

void testPrxModuleId() {
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetCensus();
    tox2::resetLogOnce();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const Tox2ImportInfo imports[] = {{0x00800000, 0xe0998dbf, "sysPrxForUser", "sys_prx_get_module_id_by_name"}};
    tox2::installImports(imports, 1);
    auto ctx = std::make_unique<Tox2PpuContext>();
    auto lookup = [&](std::string_view name) {
        for (std::size_t i = 0; i < name.size(); i++) {
            tox2Write8(base, scratch + i, static_cast<std::uint8_t>(name[i]));
        }
        tox2Write8(base, scratch + name.size(), 0);
        ctx->gpr[3] = scratch;
        ctx->gpr[4] = 0;
        ctx->gpr[5] = 0;
        tox2HleCall(ctx.get(), base, 0);
        return ctx->gpr[3];
    };

    // Every name is unknown; each is logged once.
    lines.clear();
    CHECK(lookup("cellLibprof") == err(0x8001112E));
    CHECK(lookup("cellLibprof") == err(0x8001112E));
    CHECK(lookup("cellSpurs") == err(0x8001112E));
    CHECK(countContaining(lines, "tox2: prx: module \"cellLibprof\" is not available") == 1);
    CHECK(countContaining(lines, "tox2: prx: module \"cellSpurs\" is not available") == 1);

    p.main.free(scratch);
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

void testProcessParamsfo() {
    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const Tox2ImportInfo imports[] = {{0x00800000, 0xe75c40f2, "sysPrxForUser", "sys_process_get_paramsfo"}};
    tox2::installImports(imports, 1);
    auto ctx = std::make_unique<Tox2PpuContext>();
    auto call = [&](std::uint32_t buffer) {
        ctx->gpr[3] = buffer;
        tox2HleCall(ctx.get(), base, 0);
        return ctx->gpr[3];
    };

    const tox2::ParamSfo saved = tox2::gameParamSfo();
    tox2::ParamSfo sfo;
    sfo.strings["TITLE_ID"] = "BLJS10188";
    tox2::setGameParamSfo(sfo);

    // The 0x40-byte buffer is zeroed and holds the title id from byte 1; nothing past it is touched.
    for (std::uint32_t i = 0; i < 0x50; i++) tox2Write8(base, scratch + i, 0xaa);
    CHECK(call(scratch) == 0);
    CHECK(tox2Read8(base, scratch) == 0);
    CHECK(std::memcmp(base + scratch + 1, "BLJS10188", 9) == 0);
    bool padded = true;
    for (std::uint32_t i = 10; i < 0x40; i++) padded = padded && tox2Read8(base, scratch + i) == 0;
    CHECK(padded);
    CHECK(tox2Read8(base, scratch + 0x40) == 0xaa);
    CHECK(call(0) == err(tox2::kCellEfault));

    // A longer title id is cut to nine characters; a game without one leaves the id zero.
    sfo.strings["TITLE_ID"] = "ABCDEFGHIJKL";
    tox2::setGameParamSfo(sfo);
    CHECK(call(scratch) == 0);
    CHECK(std::memcmp(base + scratch + 1, "ABCDEFGHI", 9) == 0 && tox2Read8(base, scratch + 10) == 0);
    sfo.strings.clear();
    tox2::setGameParamSfo(sfo);
    CHECK(call(scratch) == 0);
    CHECK(tox2Read8(base, scratch + 1) == 0);

    tox2::setGameParamSfo(saved);
    p.main.free(scratch);
    tox2::setLogSink({});
    tox2::resetCensus();
}

// What the entry function of the guest threads below saw; written under the guest lock.
std::uint64_t g_entry_arg, g_entry_r2, g_entry_r4, g_entry_r13;
std::uint32_t g_entry_runs, g_once_runs;

TOX2_PPU_FUNC(ppuThreadEntry) {
    g_entry_runs++;
    g_entry_arg = ctx->gpr[3];
    g_entry_r2 = ctx->gpr[2];
    g_entry_r4 = ctx->gpr[4];
    g_entry_r13 = ctx->gpr[13];
    if (ctx->gpr[3] == 5) {
        ctx->gpr[3] = 0x77;
        tox2HleCall(ctx, base, 3);  // sys_ppu_thread_exit: does not return
        return;
    }
    ctx->gpr[3] += 100;  // a C return ends the thread with r3 as the value
}

TOX2_PPU_FUNC(ppuOnceInit) {
    (void)ctx;
    (void)base;
    g_once_runs++;
}

void testPpuThreads() {
    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::resetSyncObjects();
    tox2::resetPpuThreadState();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t id_out = scratch, opd_entry = scratch + 0x10, opd_init = scratch + 0x20;
    const std::uint32_t once_ctrl = scratch + 0x30, name = scratch + 0x40, join_out = scratch + 0x80;
    const std::uint32_t tls_image = scratch + 0x100;

    const Tox2FuncMapping mappings[] = {{0x10200, ppuThreadEntry}, {0x10210, ppuOnceInit}};
    tox2RegisterFuncs(mappings, 2);
    tox2Write32(base, opd_entry, 0x10200);
    tox2Write32(base, opd_entry + 4, 0x1234);
    tox2Write32(base, opd_init, 0x10210);
    tox2Write32(base, opd_init + 4, 0x4321);

    constexpr const char* kModule = "sysPrxForUser";
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x24a1ea07, kModule, "sys_ppu_thread_create"},
            {0x00800010, 0x350d454e, kModule, "sys_ppu_thread_get_id"},
            {0x00800020, 0xa3e3be68, kModule, "sys_ppu_thread_once"},
            {0x00800030, 0xaff080a4, kModule, "sys_ppu_thread_exit"},
    };
    tox2::installImports(imports, 4);
    enum { kCreate, kGetId, kOnce };
    auto call = [&](tox2::GuestThread& caller, std::uint32_t index, std::uint64_t r3 = 0, std::uint64_t r4 = 0,
                    std::uint64_t r5 = 0, std::uint64_t r6 = 0, std::uint64_t r7 = 0, std::uint64_t r8 = 0,
                    std::uint64_t r9 = 0) {
        Tox2PpuContext* ctx = &caller.context();
        ctx->gpr[3] = r3;
        ctx->gpr[4] = r4;
        ctx->gpr[5] = r5;
        ctx->gpr[6] = r6;
        ctx->gpr[7] = r7;
        ctx->gpr[8] = r8;
        ctx->gpr[9] = r9;
        tox2HleCall(ctx, base, index);
        return ctx->gpr[3];
    };

    // The guest lock is held throughout: blocking calls release it while they wait.
    std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
    auto caller = makeCaller(0x0e000000);

    tox2Write32(base, tls_image, 0xCAFEF00D);
    auto boot_ctx = std::make_unique<Tox2PpuContext>();
    CHECK(tox2::initializeTls(boot_ctx.get(), base, 0x01000000, tls_image, 4, 0x20) != 0);
    const std::uint32_t probe = tox2::allocateTls();
    tox2::freeTls(probe);
    CHECK(probe != 0);

    // Creation errors give the TLS block back.
    CHECK(call(*caller, kCreate, id_out, 0, 1, 500, 0x10000, 1, 0) == err(tox2::kCellEfault));
    CHECK(call(*caller, kCreate, id_out, opd_entry, 1, 3072, 0x10000, 1, 0) == err(tox2::kCellEinval));
    CHECK(call(*caller, kCreate, id_out, opd_entry, 1, static_cast<std::uint64_t>(-1), 0x10000, 1, 0) ==
          err(tox2::kCellEinval));
    CHECK(call(*caller, kCreate, id_out, opd_entry, 1, 500, 0x10000, 3, 0) == err(tox2::kCellEperm));
    CHECK(call(*caller, kCreate, 0, opd_entry, 1, 500, 0x10000, 1, 0) == err(tox2::kCellEnomem));
    const std::uint32_t probe2 = tox2::allocateTls();
    tox2::freeTls(probe2);
    CHECK(probe2 == probe);
    CHECK(g_entry_runs == 0);

    // A joinable thread starts at the descriptor with r3 = arg, r4 = 0, r2 = TOC and its own r13.
    const char long_name[] = "a-thread-name-that-is-far-longer-than-27-characters";
    std::memcpy(base + name, long_name, sizeof(long_name));
    CHECK(call(*caller, kCreate, id_out, opd_entry, 7, 500, 0x10000, 1, name) == 0);
    const auto id = static_cast<std::uint32_t>(tox2Read64(base, id_out));
    tox2::GuestThread* thread = tox2::findThread(id);
    CHECK(thread != nullptr && thread->id() == id);
    if (thread != nullptr) {
        CHECK(thread->name() == std::string(long_name, 27));
        CHECK(thread->joinable() && thread->priority() == 500 && thread->stackSize() == 0x10000);
        CHECK(thread->tlsR13() == probe);
        CHECK(thread->context().gpr[13] == probe && thread->context().gpr[2] == 0x1234);
        // No guard pattern inside the stack: the guest sees zero, as on RPCS3. The top page may already be in
        // use by the running thread, so the lowest page is checked.
        const std::uint8_t* low = base + thread->stackAddr();
        CHECK(std::all_of(low, low + 0x1000, [](std::uint8_t b) { return b == 0; }));
    }
    CHECK(syscallAs(*caller, 44, id, join_out) == 0);
    CHECK(tox2Read64(base, join_out) == 107);
    CHECK(g_entry_runs == 1 && g_entry_arg == 7 && g_entry_r4 == 0 && g_entry_r2 == 0x1234);
    CHECK(g_entry_r13 == probe);
    CHECK(syscallAs(*caller, 44, id, join_out) == err(tox2::kCellEsrch));  // already joined
    CHECK(syscallAs(*caller, 48, id, join_out) == err(tox2::kCellEsrch));

    // Leaving through sys_ppu_thread_exit returns the TLS block and reports the exit value.
    CHECK(call(*caller, kCreate, id_out, opd_entry, 5, 500, 0x4000, 1, 0) == 0);
    const auto id2 = static_cast<std::uint32_t>(tox2Read64(base, id_out));
    CHECK(id2 == id + 1);
    CHECK(syscallAs(*caller, 44, id2, join_out) == 0);
    CHECK(tox2Read64(base, join_out) == 0x77);
    const std::uint32_t probe3 = tox2::allocateTls();
    tox2::freeTls(probe3);
    CHECK(probe3 == probe);
    CHECK(syscallAs(*caller, 44, id2, 0) == err(tox2::kCellEsrch));

    // A detached thread cannot be joined; a missing result pointer is reported after the join.
    CHECK(call(*caller, kCreate, id_out, opd_entry, 1, 500, 0x4000, 0, 0) == 0);
    const auto id3 = static_cast<std::uint32_t>(tox2Read64(base, id_out));
    CHECK(syscallAs(*caller, 44, id3, join_out) == err(tox2::kCellEinval));
    CHECK(call(*caller, kCreate, id_out, opd_entry, 2, 500, 0x4000, 1, 0) == 0);
    const auto id4 = static_cast<std::uint32_t>(tox2Read64(base, id_out));
    CHECK(syscallAs(*caller, 44, id4, 0) == err(tox2::kCellEfault));
    while (tox2::findThread(id3)->state() != tox2::ThreadState::Exited) {
        syscallAs(*caller, 141, 1000);  // sleeping releases the guest lock
    }
    CHECK(syscallAs(*caller, 47, id3, 100) == err(tox2::kCellEsrch));  // detached and gone
    CHECK(g_entry_runs == 4);

    // The interrupt flag creates the thread without starting it.
    CHECK(call(*caller, kCreate, id_out, opd_entry, 1, 500, 0x4000, 2, 0) == 0);
    const auto id5 = static_cast<std::uint32_t>(tox2Read64(base, id_out));
    CHECK(tox2::findThread(id5) != nullptr && tox2::findThread(id5)->state() == tox2::ThreadState::Created);
    CHECK(g_entry_runs == 4);

    // join: self and unknown ids.
    CHECK(syscallAs(*caller, 44, 0x0e000000, join_out) == err(tox2::kCellEdeadlk));
    CHECK(syscallAs(*caller, 44, 0x0e0000f0, join_out) == err(tox2::kCellEsrch));

    // Priority is stored; the caller is not registered, so it is found through its context.
    CHECK(syscallAs(*caller, 47, 0x0e000000, 1234) == 0);
    CHECK(syscallAs(*caller, 48, 0x0e000000, join_out) == 0);
    CHECK(tox2Read32(base, join_out) == 1234);
    CHECK(syscallAs(*caller, 47, 0x0e000000, 3072) == err(tox2::kCellEinval));
    CHECK(syscallAs(*caller, 47, 0x0e000000, static_cast<std::uint64_t>(-1)) == err(tox2::kCellEinval));
    CHECK(syscallAs(*caller, 47, 0x0e0000f0, 100) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*caller, 48, 0x0e0000f0, join_out) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*caller, 47, id5, 77) == 0);
    CHECK(tox2::findThread(id5)->priority() == 77);

    // detach: a joinable thread that has not run yet becomes unjoinable, a second detach is
    // refused, and the thread is gone for lv2 once it has exited.
    CHECK(call(*caller, kCreate, id_out, opd_entry, 1, 500, 0x4000, 1, 0) == 0);
    const auto id6 = static_cast<std::uint32_t>(tox2Read64(base, id_out));
    CHECK(syscallAs(*caller, 45, id6) == 0);
    CHECK(syscallAs(*caller, 45, id6) == err(tox2::kCellEinval));
    CHECK(syscallAs(*caller, 44, id6, join_out) == err(tox2::kCellEinval));
    while (tox2::findThread(id6)->state() != tox2::ThreadState::Exited) {
        syscallAs(*caller, 141, 1000);
    }
    CHECK(syscallAs(*caller, 45, id6) == err(tox2::kCellEsrch));

    // A thread that exited without being joined is dropped by the detach.
    CHECK(call(*caller, kCreate, id_out, opd_entry, 1, 500, 0x4000, 1, 0) == 0);
    const auto id7 = static_cast<std::uint32_t>(tox2Read64(base, id_out));
    while (tox2::findThread(id7)->state() != tox2::ThreadState::Exited) {
        syscallAs(*caller, 141, 1000);
    }
    CHECK(syscallAs(*caller, 45, id7) == 0);
    CHECK(syscallAs(*caller, 44, id7, join_out) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*caller, 45, id7) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*caller, 45, 0x0e0000f0) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*caller, 45, 0x0e000000) == err(tox2::kCellEinval));  // the caller is not joinable
    CHECK(syscallAs(*caller, 45, id5) == err(tox2::kCellEinval));         // created without the joinable flag

    // yield: CELL_CANCEL when no other thread waits for the lock.
    CHECK(syscallAs(*caller, 43) == 1);

    // sys_ppu_thread_get_id writes the id of the calling thread as a BE u64.
    tox2Write64(base, id_out, ~0ull);
    CHECK(call(*caller, kGetId, id_out) == 0);
    CHECK(tox2Read64(base, id_out) == 0x0e000000);

    // sys_ppu_thread_once runs the initialiser once and leaves the mutex unlocked.
    tox2Write32(base, once_ctrl, tox2::kOnceInit);
    call(*caller, kOnce, once_ctrl, opd_init);
    call(*caller, kOnce, once_ctrl, opd_init);
    CHECK(g_once_runs == 1 && tox2Read32(base, once_ctrl) == tox2::kDoneInit);
    CHECK(tox2::mutexTryLock(&caller->context(), tox2::processSyncObjects().once_mutex) == 0);
    CHECK(tox2::mutexUnlock(&caller->context(), tox2::processSyncObjects().once_mutex) == 0);

    p.main.free(scratch);
    tox2RegisterFuncs(nullptr, 0);
    tox2::resetSyncObjects();
    tox2::resetPpuThreadState();
    tox2::setLogSink({});
    tox2::resetCensus();
}

// Inputs and results of the priority creation test; written under the guest lock.
struct PrioScenario {
    std::uint32_t word_addr = 0, id_out = 0, opd_child = 0;
    std::int64_t set_priority_to = -1;  // the creator lowers itself first when >= 0
    std::int32_t child_prio = 0;
    std::uint64_t flags = 0;
    std::uint32_t seen_after_create = 0, create_result = ~0u;
    bool done = false;
};
PrioScenario g_prio;
std::condition_variable_any g_prio_cv;

TOX2_PPU_FUNC(prioChildEntry) {
    (void)ctx;
    tox2Write32(base, g_prio.word_addr, 1);
}

TOX2_PPU_FUNC(prioCreatorEntry) {
    // As runImport does around a handler in the split modes; a no-op in Legacy, where the thread holds the guest lock.
    tox2::KernelSection section;
    if (g_prio.set_priority_to >= 0) {
        ctx->gpr[11] = 47;  // sys_ppu_thread_set_priority(self, prio)
        ctx->gpr[3] = static_cast<tox2::GuestThread*>(ctx->runtime)->id();
        ctx->gpr[4] = static_cast<std::uint64_t>(g_prio.set_priority_to);
        tox2Syscall(ctx, base, 0x10000);
    }
    ctx->gpr[3] = g_prio.id_out;
    ctx->gpr[4] = g_prio.opd_child;
    ctx->gpr[5] = 0;
    ctx->gpr[6] = static_cast<std::uint64_t>(g_prio.child_prio);
    ctx->gpr[7] = 0x4000;
    ctx->gpr[8] = g_prio.flags;
    ctx->gpr[9] = 0;
    tox2HleCall(ctx, base, 0);  // sys_ppu_thread_create, no blocking call after it
    g_prio.create_result = static_cast<std::uint32_t>(ctx->gpr[3]);
    g_prio.seen_after_create = tox2Read32(base, g_prio.word_addr);
    g_prio.done = true;
    g_prio_cv.notify_all();
    ctx->gpr[3] = 0;
}

// `parallel_slots` is true when the creator and a new thread of equal or lower priority can run at the same time (two
// PPU slots): then the word a new thread writes may or may not be visible right after the create returns.
void ppuThreadCreatePriorityScenario(bool parallel_slots) {
    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::resetSyncObjects();
    tox2::resetPpuThreadState();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t opd_child = scratch + 0x10, opd_creator = scratch + 0x20;
    const std::uint32_t tls_image = scratch + 0x100;

    const Tox2FuncMapping mappings[] = {{0x10300, prioChildEntry}, {0x10310, prioCreatorEntry}};
    tox2RegisterFuncs(mappings, 2);
    tox2Write32(base, opd_child, 0x10300);
    tox2Write32(base, opd_child + 4, 0x1111);
    tox2Write32(base, opd_creator, 0x10310);
    tox2Write32(base, opd_creator + 4, 0x2222);
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x24a1ea07, "sysPrxForUser", "sys_ppu_thread_create"},
    };
    tox2::installImports(imports, 1);

    std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
    tox2Write32(base, tls_image, 0xCAFEF00D);
    auto boot_ctx = std::make_unique<Tox2PpuContext>();
    CHECK(tox2::initializeTls(boot_ctx.get(), base, 0x01000000, tls_image, 4, 0x20) != 0);

    struct Outcome {
        std::uint32_t seen_after_create;
        std::uint32_t seen_at_end;
        tox2::ThreadState child_state;
        bool ok;
    };
    // Runs a guest creator thread at `creator_prio`; it creates a thread with `child_prio` and
    // reports the guest word right after the create returned.
    auto run = [&](std::int32_t creator_prio, std::int64_t set_priority_to, std::int32_t child_prio,
                   std::uint64_t flags) {
        g_prio = PrioScenario{};
        g_prio.word_addr = scratch + 0x200;
        g_prio.id_out = scratch + 0x208;
        g_prio.opd_child = opd_child;
        g_prio.set_priority_to = set_priority_to;
        g_prio.child_prio = child_prio;
        g_prio.flags = flags;
        tox2Write32(base, g_prio.word_addr, 0);

        tox2::GuestThreadParams params;
        params.entry_opd = opd_creator;
        params.priority = creator_prio;
        params.stack_size = 0x10000;
        params.tls_r13 = tox2::allocateTls();
        params.name = "creator";
        const std::uint32_t creator_id = tox2::createGuestThread(params);
        Outcome out{0, 0, tox2::ThreadState::Created, false};
        if (creator_id == 0) return out;
        out.ok = tox2::blockingWait(g_prio_cv, [] { return g_prio.done; }, "test wait", std::chrono::seconds(5));
        out.seen_after_create = g_prio.seen_after_create;
        const tox2::GuestThread* child = tox2::findThread(creator_id + 1);
        for (int i = 0; child != nullptr && flags == 0 && i < 2500 && child->state() != tox2::ThreadState::Exited;
             i++) {
            std::condition_variable_any pause;
            tox2::blockingWait(pause, [] { return false; }, "test pause", std::chrono::milliseconds(2));
        }
        if (child != nullptr) out.child_state = child->state();
        out.seen_at_end = tox2Read32(base, g_prio.word_addr);
        return out;
    };

    // A lower number than the creator's: the new thread has run when the create returns.
    Outcome first = run(1005, -1, 90, 0);
    CHECK(first.ok && g_prio.create_result == 0);
    CHECK(first.seen_after_create == 1 && first.seen_at_end == 1);
    CHECK(first.child_state == tox2::ThreadState::Exited);

    // Equal and higher numbers: the creator keeps the lock; the thread runs after it returned.
    Outcome equal = run(1005, -1, 1005, 0);
    CHECK(equal.ok && (parallel_slots || equal.seen_after_create == 0) && equal.seen_at_end == 1);
    Outcome lower = run(1005, -1, 1100, 0);
    CHECK(lower.ok && (parallel_slots || lower.seen_after_create == 0) && lower.seen_at_end == 1);

    // The stored priority counts: a creator that moved itself from 1100 to 1005 yields to a thread
    // at 1002.
    Outcome lowered = run(1100, 1005, 1002, 0);
    CHECK(lowered.ok && lowered.seen_after_create == 1);
    // And the other way: a creator that raised itself to 1000 does not yield to a thread at 1002.
    Outcome raised = run(1005, 1000, 1002, 0);
    CHECK(raised.ok && (parallel_slots || raised.seen_after_create == 0) && raised.seen_at_end == 1);

    // An interrupt thread is created but not started, whatever its priority.
    Outcome interrupt = run(1005, -1, 90, tox2::kCreateInterrupt);
    CHECK(interrupt.ok && interrupt.seen_after_create == 0 && interrupt.seen_at_end == 0);
    CHECK(interrupt.child_state == tox2::ThreadState::Created);

    p.main.free(scratch);
    tox2RegisterFuncs(nullptr, 0);
    tox2::resetSyncObjects();
    tox2::resetPpuThreadState();
    tox2::setLogSink({});
    tox2::resetCensus();
}

void testPpuThreadCreatePriority() { ppuThreadCreatePriorityScenario(false); }

struct HandlerCall {
    char tag;
    std::uint64_t r3;
    bool on_service_thread;
};
std::vector<HandlerCall> g_handler_calls;  // written under the guest lock
std::condition_variable_any g_handler_cv;

void recordHandlerCall(char tag, Tox2PpuContext* ctx) {
    const tox2::GuestThread* thread = tox2::currentThread();
    g_handler_calls.push_back({tag, ctx->gpr[3],
                               thread != nullptr && thread->service() && thread->name() == "_gcm_intr_thread" &&
                                       ctx->gpr[13] == thread->tlsR13() && ctx->gpr[13] != 0});
    g_handler_cv.notify_all();
}

TOX2_PPU_FUNC(gcmVblankHandler) {
    (void)base;
    recordHandlerCall('v', ctx);
}

TOX2_PPU_FUNC(gcmUserHandler) {
    (void)base;
    recordHandlerCall('u', ctx);
}

// Waits with the guest lock released until `done` holds; false after five seconds.
template <class Pred>
bool waitUntil(Pred done) {
    return tox2::blockingWait(g_handler_cv, done, "test wait", std::chrono::seconds(5));
}

// Lets other guest threads run for a moment.
void pauseUnlocked() {
    std::condition_variable_any cv;
    tox2::blockingWait(cv, [] { return false; }, "test pause", std::chrono::milliseconds(2));
}

// Runs in whatever PPU threads mode the caller selected (testGcmInterruptThread).
void gcmInterruptThreadCase() {
    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::resetLogOnce();
    tox2::resetSyncObjects();
    tox2::resetRsxState();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t opd_vblank = scratch, opd_user = scratch + 0x10, tls_image = scratch + 0x100;
    const Tox2FuncMapping mappings[] = {{0x10400, gcmVblankHandler}, {0x10410, gcmUserHandler}};
    tox2RegisterFuncs(mappings, 2);
    tox2Write32(base, opd_vblank, 0x10400);
    tox2Write32(base, opd_vblank + 4, 0x5555);
    tox2Write32(base, opd_user, 0x10410);
    tox2Write32(base, opd_user + 4, 0x6666);
    g_handler_calls.clear();

    std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
    auto boot_ctx = std::make_unique<Tox2PpuContext>();
    tox2Write32(base, tls_image, 0xCAFEF00D);
    CHECK(tox2::initializeTls(boot_ctx.get(), base, 0x01000000, tls_image, 4, 0x20) != 0);

    tox2::RsxState& state = tox2::rsxState();
    state.vblank_handler = opd_vblank;
    state.user_handler = opd_user;  // the flip handler stays unset, like the game's

    const std::uint32_t id = tox2::startGcmInterruptThread();
    CHECK(id != 0 && tox2::startGcmInterruptThread() == id);
    const tox2::GuestThread* thread = tox2::findThread(id);
    CHECK(thread != nullptr && thread->service() && thread->name() == "_gcm_intr_thread");
    CHECK(thread != nullptr && thread->priority() == 1 && thread->stackSize() == 0x4000);

    // The thread cannot run while this test holds the guest lock, so everything below queues up:
    // user commands and flips are all delivered in order, vblanks coalesce, a missing handler drops.
    tox2::postGcmInterrupt(tox2::RsxInterrupt::User, 7);
    tox2::postGcmInterrupt(tox2::RsxInterrupt::VBlank, 1);
    tox2::postGcmInterrupt(tox2::RsxInterrupt::VBlank, 1);
    tox2::postGcmInterrupt(tox2::RsxInterrupt::Flip, 1);  // no flip handler
    tox2::postGcmInterrupt(tox2::RsxInterrupt::User, 8);
    tox2::postGcmInterrupt(tox2::RsxInterrupt::VBlank, 1);
    CHECK(waitUntil([] { return g_handler_calls.size() >= 3; }));
    CHECK(g_handler_calls.size() == 3);
    if (g_handler_calls.size() == 3) {
        CHECK(g_handler_calls[0].tag == 'u' && g_handler_calls[0].r3 == 7);
        CHECK(g_handler_calls[1].tag == 'v' && g_handler_calls[1].r3 == 1);
        CHECK(g_handler_calls[2].tag == 'u' && g_handler_calls[2].r3 == 8);
        CHECK(g_handler_calls[0].on_service_thread && g_handler_calls[1].on_service_thread);
    }

    // Once the queue is empty a new tick is accepted again, and the thread idles as a service thread.
    tox2::postGcmInterrupt(tox2::RsxInterrupt::VBlank, 1);
    CHECK(waitUntil([] { return g_handler_calls.size() >= 4; }));
    for (int i = 0; i < 2500 && thread->state() != tox2::ThreadState::Blocked; i++) pauseUnlocked();
    CHECK(thread->state() == tox2::ThreadState::Blocked);
    const tox2::GcmInterruptCounts counts = tox2::gcmInterruptCounts();
    CHECK(counts.vblank == 2 && counts.flip == 0 && counts.user == 2 && counts.vblank_coalesced == 2);
    CHECK(tox2::formatCensus().find("_gcm_intr_thread: blocked in gcm interrupt wait [service]") != std::string::npos);
    CHECK(tox2::formatCensus().find("census: guest lock") != std::string::npos);

    tox2::stopGcmInterruptThread();
    CHECK(thread->state() == tox2::ThreadState::Exited);
    CHECK(tox2::gcmInterruptCounts().vblank == 0 && tox2::gcmInterruptCounts().vblank_coalesced == 0);

    tox2RegisterFuncs(nullptr, 0);
    tox2::resetHostFunctions();
    tox2::resetRsxState();
    tox2::resetSyncObjects();
    p.main.free(scratch);
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

void testJudgeThreads() {
    using tox2::ThreadHealth;
    using tox2::ThreadState;
    using tox2::ThreadVerdict;
    const auto stalled = [](bool service) { return ThreadHealth{ThreadState::Blocked, true, 0, service}; };
    const ThreadHealth running{ThreadState::Running, false, 0, false};
    const ThreadHealth exited{ThreadState::Exited, false, 0, false};
    std::size_t count = 0;

    // Blocked for under two seconds is not a stall.
    CHECK(tox2::judgeThreads({ThreadHealth{ThreadState::Blocked, true, 9000, false}}, 10000) == ThreadVerdict::Ok);
    CHECK(tox2::judgeThreads({stalled(false)}, 10000, &count) == ThreadVerdict::Stall && count == 1);
    // A timed wait never counts.
    CHECK(tox2::judgeThreads({ThreadHealth{ThreadState::Blocked, false, 0, false}}, 10000) == ThreadVerdict::Ok);
    // A service thread, however it waits, neither keeps the process alive nor hides a stall.
    const ThreadHealth running_service{ThreadState::Running, false, 0, true};
    CHECK(tox2::judgeThreads({stalled(false), running_service}, 10000) == ThreadVerdict::Stall);
    CHECK(tox2::judgeThreads({stalled(false), stalled(true)}, 10000, &count) == ThreadVerdict::Stall && count == 1);
    CHECK(tox2::judgeThreads({stalled(false), running}, 10000) == ThreadVerdict::Ok);
    CHECK(tox2::judgeThreads({exited, stalled(true)}, 10000) == ThreadVerdict::NoThreads);
    CHECK(tox2::judgeThreads({}, 10000) == ThreadVerdict::NoThreads);
}

// Waiters get the lock in the order they queued, whatever the host scheduler does.
void testGuestLockFifo() {
    tox2::GuestLock lock;
    std::vector<int> order;  // guarded by `lock`

    lock.lock();
    CHECK(!lock.hasWaiters());

    std::vector<std::thread> threads;
    for (int id = 1; id <= 3; id++) {
        threads.emplace_back([&lock, &order, id] {
            lock.lock();
            order.push_back(id);
            lock.unlock();
        });
        // The next thread starts only after this one has its place in the queue.
        while (lock.waiterCount() < static_cast<std::size_t>(id)) {
            std::this_thread::yield();
        }
    }
    CHECK(lock.hasWaiters());
    lock.unlock();
    for (std::thread& t : threads) t.join();

    CHECK(order == (std::vector<int>{1, 2, 3}));
    CHECK(!lock.hasWaiters());
}

// startPerfLog is once per process, so this is the only test that starts it.
void testRsxBusyFormat() {
    CHECK(tox2::formatRsxBusy({3890, 2090, 790, 4830, 5090}, 1) ==
          "rsx busy 12.80 ms/flip: lock 2.09 pump 0.79 build 4.83 record 5.09, idle 3.89");
    // The figures are per flip.
    CHECK(tox2::formatRsxBusy({7780, 4180, 1580, 9660, 10180}, 2) ==
          "rsx busy 12.80 ms/flip: lock 2.09 pump 0.79 build 4.83 record 5.09, idle 3.89");
    CHECK(tox2::formatRsxBusy({3890, 2090, 790, 4830, 5090}, 0).empty());
}

void testHostThreadCpu() {
    using tox2::HostThreadCpu;
    const std::vector<HostThreadCpu> before = {{"rsx", 0}, {"ppu.01000006", 0}};
    const std::vector<HostThreadCpu> after = {{"rsx", 262100}, {"ppu.01000006", 150200}, {"spu.06000200", 4000}};
    CHECK(tox2::formatHostThreadCpu(before, after, 0, 812300) ==
          "cpu total 812.3 ms: rsx=262.1 ppu.01000006=150.2 other=400.0");
    CHECK(tox2::formatHostThreadCpu(before, after, 500, 500).empty());

    // A thread missing from `before` counts from 0; the listed entries never push `other` below 0.
    CHECK(tox2::formatHostThreadCpu({}, after, 1000, 101000) ==
          "cpu total 100.0 ms: rsx=262.1 ppu.01000006=150.2 other=0.0");

    // 13 threads of 10 ms: ties keep registration order and the line lists 12.
    std::vector<HostThreadCpu> zero, ten;
    for (int i = 0; i < 13; i++) {
        const std::string label = std::format("t{:02}", i);
        zero.push_back({label, 0});
        ten.push_back({label, 10000});
    }
    const std::string many = tox2::formatHostThreadCpu(zero, ten, 0, 200000);
    CHECK(many.starts_with("cpu total 200.0 ms: t00=10.0 t01=10.0"));
    CHECK(many.find(" t11=10.0") != std::string::npos);
    CHECK(many.find("t12") == std::string::npos);
    CHECK(many.ends_with(" other=80.0"));

    // A thread that registers itself keeps its CPU time after it exits.
    std::mutex mutex;
    std::condition_variable cv;
    bool spun = false, release = false;
    std::thread helper([&] {
        tox2::registerHostThreadCpu("test.cpu.helper");
        tox2::registerHostThreadCpu("test.cpu.helper.again");
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100)) {
        }
        std::unique_lock lock(mutex);
        spun = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
    });
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return spun; });
    }
    const auto findHelper = [](const std::vector<HostThreadCpu>& snapshot) {
        std::size_t count = 0;
        std::uint64_t cpu_us = 0;
        for (const HostThreadCpu& entry : snapshot) {
            if (entry.label.starts_with("test.cpu.helper")) {
                count++;
                cpu_us = entry.cpu_us;
            }
        }
        return std::pair{count, cpu_us};
    };
    const auto [live_count, live_us] = findHelper(tox2::hostThreadCpuSnapshot());
    CHECK(live_count == 1);
    CHECK(live_us >= 80000 && live_us <= 200000);
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    cv.notify_all();
    helper.join();
    const auto [exited_count, exited_us] = findHelper(tox2::hostThreadCpuSnapshot());
    const auto [exited_count2, exited_us2] = findHelper(tox2::hostThreadCpuSnapshot());
    CHECK(exited_count == 1 && exited_count2 == 1);
    // The thread still runs a little between the first snapshot and its exit.
    CHECK(exited_us >= live_us && exited_us - live_us < 5000);
    CHECK(exited_us2 == exited_us);

    CHECK(tox2::hostCyclesPerMs() > 0.0);
    CHECK(tox2::processCpuUs() > 0);
}

void testCycleStats() {
    const auto summarize = [](std::initializer_list<std::uint64_t> values) {
        const std::vector<std::uint64_t> samples(values);
        return tox2::summarizeCycles(samples);
    };
    const tox2::CycleStats three = summarize({5, 1, 3});
    CHECK(three.count == 3 && three.min == 1 && three.median == 3 && three.max == 5);
    // Even counts take the upper median.
    const tox2::CycleStats four = summarize({4, 1, 3, 2});
    CHECK(four.count == 4 && four.min == 1 && four.median == 3 && four.max == 4);
    const tox2::CycleStats one = summarize({7});
    CHECK(one.count == 1 && one.min == 7 && one.median == 7 && one.max == 7);
    const tox2::CycleStats empty = summarize({});
    CHECK(empty.count == 0 && empty.min == 0 && empty.median == 0 && empty.max == 0);

    // Spinning advances the thread's cycles at about the calibrated rate; sleeping hardly advances them.
    const double expected = 50.0 * tox2::hostCyclesPerMs();
    std::uint64_t spun = 0, slept = 0;
    std::thread helper([&] {
        const std::uint64_t spin_start = tox2::currentThreadCycles();
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(50)) {
        }
        spun = tox2::currentThreadCycles() - spin_start;
        const std::uint64_t sleep_start = tox2::currentThreadCycles();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        slept = tox2::currentThreadCycles() - sleep_start;
    });
    helper.join();
    CHECK(static_cast<double>(spun) >= 0.5 * expected && static_cast<double>(spun) <= 3.0 * expected);
    CHECK(static_cast<double>(slept) < 0.1 * expected);
}

void testPerfLine() {
    CHECK(tox2::formatPerfLine(212.04, "x") == "tox2: perf: t 212.0 s: x");

    // The perf thread outlives this function, so the counters are shared with it.
    const auto calls = std::make_shared<std::atomic<int>>(0);
    const auto empty_calls = std::make_shared<std::atomic<int>>(0);
    const auto ignored = std::make_shared<std::atomic<int>>(0);
    tox2::startPerfLog({[calls] {
                            ++*calls;
                            return std::string("body");
                        },
                        [empty_calls] {
                            ++*empty_calls;
                            return std::string();
                        }},
                       std::chrono::milliseconds(200));
    // A second start must not add a thread or change the interval.
    tox2::startPerfLog({[ignored] {
                           ++*ignored;
                           return std::string("late");
                       }},
                       std::chrono::milliseconds(10));
    std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    CHECK(calls->load() >= 3);
    CHECK(calls->load() <= 18);
    CHECK(ignored->load() == 0);
}

// Waits and holds are attributed to the class of the thread that waited or held.
void testGuestLockStats() {
    constexpr auto kRsx = tox2::GuestLockClass::Rsx;
    constexpr auto kAudio = tox2::GuestLockClass::Audio;
    constexpr auto kOther = tox2::GuestLockClass::Other;
    const auto idx = [](tox2::GuestLockClass cls) { return static_cast<std::size_t>(cls); };
    CHECK(tox2::guestLockClassName(kRsx) == "rsx" && tox2::guestLockClassName(kAudio) == "audio");
    CHECK(tox2::guestLockClassName(tox2::GuestLockClass::PpuHigh) == "ppu-high");

    // An Audio thread queues behind an Rsx holder that keeps the lock for 20 ms.
    {
        tox2::GuestLock lock;
        std::atomic<bool> held{false};
        std::thread holder([&] {
            tox2::setGuestLockClass(kRsx);
            lock.lock();
            held = true;
            while (!lock.hasWaiters()) std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            lock.unlock();
        });
        std::thread waiter([&] {
            tox2::setGuestLockClass(kAudio);
            while (!held) std::this_thread::yield();
            lock.lock();
            lock.unlock();
        });
        holder.join();
        waiter.join();

        const tox2::GuestLockStats s = lock.stats();
        const tox2::GuestLockClassStats& audio = s.classes[idx(kAudio)];
        const tox2::GuestLockClassStats& rsx = s.classes[idx(kRsx)];
        CHECK(audio.wait.count == 1 && audio.wait.max_us >= 15'000 && audio.wait.max_us <= 60'000);
        CHECK(audio.wait.over >= 1);
        CHECK(audio.contended == 1 && audio.ahead_sum == 1 && audio.ahead_max == 1);
        CHECK(rsx.wait.count == 1 && rsx.wait.max_us == 0 && rsx.contended == 0);
        CHECK(rsx.hold.count == 1 && rsx.hold.max_us >= 20'000);
        CHECK(audio.waited_behind_us[idx(kRsx)] >= 15'000);
        CHECK(audio.waited_behind_us[idx(kAudio)] == 0);
        CHECK(s.handoff.count == 1);
        CHECK(s.wakeups >= 1 && s.spurious_wakeups < s.wakeups);
        CHECK(s.classes[idx(kOther)].wait.count == 0);
    }

    // A thread that never named itself counts as Other; an uncontended acquisition waits 0.
    {
        tox2::GuestLock lock;
        std::thread t([&] {
            CHECK(tox2::currentGuestLockClass() == kOther);
            for (int i = 0; i < 1000; i++) {
                lock.lock();
                lock.unlock();
            }
        });
        t.join();
        const tox2::GuestLockStats s = lock.stats();
        const tox2::GuestLockClassStats& other = s.classes[idx(kOther)];
        CHECK(other.wait.count == 1000 && other.wait.sum_us == 0 && other.wait.max_us == 0);
        CHECK(other.wait.buckets[0] == 1000 && other.hold.count == 1000 && other.contended == 0);
        CHECK(s.handoff.count == 0 && s.wakeups == 0);
        CHECK(s.classes[idx(kAudio)].wait.count == 0);
    }

    // Cost of an uncontended lock + unlock pair, with the statistics on.
    {
        tox2::GuestLock lock;
        constexpr int kRuns = 5;
        constexpr int kPairs = 1'000'000;
        std::array<double, kRuns> ns_per_pair{};
        for (double& ns : ns_per_pair) {
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < kPairs; i++) {
                lock.lock();
                lock.unlock();
            }
            const auto elapsed = std::chrono::steady_clock::now() - start;
            ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) / kPairs;
        }
        std::ranges::sort(ns_per_pair);
        std::printf("guest lock uncontended pair: %.0f ns\n", ns_per_pair[kRuns / 2]);
        CHECK(lock.stats().classes[idx(kOther)].wait.count == static_cast<std::uint64_t>(kRuns) * kPairs);
    }
}

std::vector<tox2::GuestLock::LongHoldReport> g_long_hold_reports;

void collectLongHold(const tox2::GuestLock::LongHoldReport& report) { g_long_hold_reports.push_back(report); }

// A hold of at least kGuestLockLongHoldUs is counted, also when it ends in yieldToHostWaiters; a shorter one is not.
void testGuestLockLongHold() {
    constexpr auto kSleep = std::chrono::microseconds(tox2::kGuestLockLongHoldUs + 3000);
    tox2::GuestLock lock;
    for (int i = 0; i < 10; i++) {
        lock.lock();
        lock.unlock();
    }
    CHECK(lock.stats().long_holds == 0);

    lock.lock();
    std::this_thread::sleep_for(kSleep);
    lock.unlock();
    CHECK(lock.stats().long_holds == 1);

    // The label restores on scope exit, and a new hold starts without the previous hold's last label.
    CHECK(tox2::t_activity.current == nullptr);
    lock.lock();
    CHECK(tox2::t_activity.hold_last == nullptr);
    {
        const tox2::ActivityScope outer("outer");
        {
            const tox2::ActivityScope inner("inner");
            CHECK(std::string_view(tox2::t_activity.current) == "inner");
        }
        CHECK(std::string_view(tox2::t_activity.current) == "outer");
    }
    CHECK(tox2::t_activity.current == nullptr && std::string_view(tox2::t_activity.hold_last) == "inner");
    CHECK(std::string_view(tox2::t_activity.hold_prev) == "outer");
    {
        // The same label again changes nothing, so hold_prev keeps the label before it.
        const tox2::ActivityScope again("inner");
        CHECK(std::string_view(tox2::t_activity.hold_prev) == "outer");
    }
    lock.unlock();
    lock.lock();
    CHECK(tox2::t_activity.hold_last == nullptr && tox2::t_activity.hold_prev == nullptr);
    lock.unlock();

    // A host waiter makes yieldToHostWaiters release; the hold that ends there counts too.
    std::thread waiter([&] {
        lock.lock();
        lock.unlock();
    });
    lock.lock();
    while (!lock.hasHostWaiters()) std::this_thread::yield();
    std::this_thread::sleep_for(kSleep);
    CHECK(lock.yieldToHostWaiters());
    lock.unlock();
    waiter.join();
    CHECK(lock.stats().long_holds == 2);

    // The report separates a holder that ran from one the host did not schedule, and names the last two labels.
    {
        constexpr auto kHold = std::chrono::milliseconds(tox2::kGuestLockLongHoldUs / 1000 + 4);
        tox2::setGuestLockLongHoldObserverForTesting(&collectLongHold);
        tox2::setGuestLockPpuPriorityForTesting(1001);  // the cycle count is taken for PPU-class holders only
        g_long_hold_reports.clear();

        lock.lock();
        {
            const tox2::ActivityScope first("first");
        }
        {
            const tox2::ActivityScope second("second");
        }
        for (const auto start = std::chrono::steady_clock::now(); std::chrono::steady_clock::now() - start < kHold;) {
        }
        lock.unlock();

        lock.lock();
        std::this_thread::sleep_for(kHold);
        lock.unlock();
        tox2::setGuestLockLongHoldObserverForTesting(nullptr);
        tox2::setGuestLockPpuPriorityForTesting(std::nullopt);

        CHECK(g_long_hold_reports.size() == 2);
        if (g_long_hold_reports.size() == 2) {
            const tox2::GuestLock::LongHoldReport& busy = g_long_hold_reports[0];
            const tox2::GuestLock::LongHoldReport& idle = g_long_hold_reports[1];
            const double busy_cpu_ms = tox2::GuestLock::cyclesToMs(busy.cpu_cycles);
            const double idle_cpu_ms = tox2::GuestLock::cyclesToMs(idle.cpu_cycles);
            const double busy_ms = static_cast<double>(busy.hold_us) / 1000.0;
            const double idle_ms = static_cast<double>(idle.hold_us) / 1000.0;
            std::printf("long hold cpu: busy %.2f of %.2f ms, sleep %.2f of %.2f ms\n", busy_cpu_ms, busy_ms,
                        idle_cpu_ms, idle_ms);
            CHECK(busy.has_cpu && idle.has_cpu && busy.cpu_cycles > 0 && busy_cpu_ms >= 0.5 * busy_ms);
            CHECK(idle_cpu_ms <= 0.5 * idle_ms);
            CHECK(std::string_view(busy.last_in_hold) == "second" &&
                  std::string_view(busy.previous_in_hold) == "first");
            CHECK(busy.at_release == nullptr && idle.last_in_hold == nullptr && idle.previous_in_hold == nullptr);
        }
    }
}

// A release wakes only the waiter it serves, not every waiter.
void testGuestLockSingleWake() {
    tox2::GuestLock lock;
    std::vector<int> order;  // guarded by `lock`

    lock.lock();
    std::vector<std::thread> threads;
    for (int id = 1; id <= 3; id++) {
        threads.emplace_back([&lock, &order, id] {
            lock.lock();
            order.push_back(id);
            lock.unlock();
        });
        while (lock.waiterCount() < static_cast<std::size_t>(id)) {
            std::this_thread::yield();
        }
    }
    lock.unlock();
    for (std::thread& t : threads) t.join();

    CHECK(order == (std::vector<int>{1, 2, 3}));
    const tox2::GuestLockStats s = lock.stats();
    CHECK(s.wakeups <= 4);
    CHECK(s.spurious_wakeups <= 1);
    CHECK(!lock.hasWaiters());
}

// The overlap is the part of a PPU hold during which another PPU-class thread was queued; a host-class waiter does
// not count.
void testGuestLockPpuOverlap() {
    const auto run = [](tox2::GuestLockClass waiter_class, std::optional<std::int32_t> waiter_priority) {
        tox2::GuestLock lock;
        std::atomic<bool> held{false};
        std::thread holder([&] {
            tox2::setGuestLockPpuPriorityForTesting(1001);
            lock.lock();
            held = true;
            while (!lock.hasWaiters()) std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            lock.unlock();
            tox2::setGuestLockPpuPriorityForTesting(std::nullopt);
        });
        std::thread waiter([&] {
            if (waiter_priority.has_value()) {
                tox2::setGuestLockPpuPriorityForTesting(waiter_priority);
            } else {
                tox2::setGuestLockClass(waiter_class);
            }
            while (!held) std::this_thread::yield();
            lock.lock();
            lock.unlock();
            tox2::setGuestLockPpuPriorityForTesting(std::nullopt);
        });
        holder.join();
        waiter.join();
        return lock.stats();
    };

    const tox2::GuestLockStats ppu = run(tox2::GuestLockClass::Other, 1005);
    CHECK(ppu.ppu_hold_overlap_us >= 15'000);
    CHECK(ppu.ppu_hold_us >= ppu.ppu_hold_overlap_us);

    const tox2::GuestLockStats rsx = run(tox2::GuestLockClass::Rsx, std::nullopt);
    CHECK(rsx.ppu_hold_us >= 20'000);
    CHECK(rsx.ppu_hold_overlap_us == 0);
}

// PPU waiters are served by lv2 priority, host-class waiters keep their arrival position, and the thread that ran last
// lets the second-best PPU waiter go first. Each case uses its own lock.
void testGuestLockPpuPriority() {
    using tox2::GuestLockClass;
    using Priority = std::optional<std::int32_t>;

    // Threads queue one at a time; each appends its name under the lock it is served by.
    struct Case {
        tox2::GuestLock lock;
        std::vector<std::string> order;  // guarded by `lock`
        std::vector<std::thread> threads;

        // `while_held` runs while the thread holds the lock, after its name is appended.
        void add(std::string name, Priority priority, GuestLockClass cls, std::function<void()> while_held = {}) {
            const std::size_t queued = lock.waiterCount();
            threads.emplace_back([this, name = std::move(name), priority, cls, while_held = std::move(while_held)] {
                if (priority.has_value()) {
                    tox2::setGuestLockPpuPriorityForTesting(priority);
                } else {
                    tox2::setGuestLockClass(cls);
                }
                lock.lock();
                order.push_back(name);
                if (while_held) while_held();
                lock.unlock();
                tox2::setGuestLockPpuPriorityForTesting(std::nullopt);
            });
            while (lock.waiterCount() <= queued) std::this_thread::yield();
        }
        void join() {
            for (std::thread& t : threads) t.join();
        }
    };
    const auto names = [](std::initializer_list<const char*> list) {
        return std::vector<std::string>(list.begin(), list.end());
    };

    // (a) PPU waiters by priority, then queue order.
    {
        Case c;
        c.lock.lock();
        c.add("1001a", 1001, GuestLockClass::Other);
        c.add("100", 100, GuestLockClass::Other);
        c.add("1001b", 1001, GuestLockClass::Other);
        c.add("0", 0, GuestLockClass::Other);
        c.lock.unlock();
        c.join();
        CHECK(c.order == names({"0", "100", "1001a", "1001b"}));
    }

    // (b) Host waiters keep their position among the PPU slots.
    {
        Case c;
        c.lock.lock();
        c.add("Rsx", std::nullopt, GuestLockClass::Rsx);
        c.add("1001", 1001, GuestLockClass::Other);
        c.add("Audio", std::nullopt, GuestLockClass::Audio);
        c.add("100", 100, GuestLockClass::Other);
        c.lock.unlock();
        c.join();
        CHECK(c.order == names({"Rsx", "100", "Audio", "1001"}));
    }

    // Holder A (1001) gives up the lock and queues again, as a budget expiry does; Rsx is the first waiter and holds
    // the lock until A has queued. C (1005) is queued last, with B (1001) before it when `with_b`.
    const auto alternation = [&](bool with_b) {
        Case c;
        std::atomic<bool> a_held{false}, a_go{false};
        std::thread a([&] {
            tox2::setGuestLockPpuPriorityForTesting(1001);
            c.lock.lock();
            a_held = true;
            while (!a_go) std::this_thread::yield();
            c.lock.unlock();
            c.lock.lock();
            c.order.push_back("A");
            c.lock.unlock();
            tox2::setGuestLockPpuPriorityForTesting(std::nullopt);
        });
        while (!a_held) std::this_thread::yield();
        const std::size_t expected = with_b ? 3 : 2;  // A has queued again when Rsx sees this
        const auto wait_for_a = [&c, expected] {
            while (c.lock.waiterCount() < expected) std::this_thread::yield();
        };
        c.add("Rsx", std::nullopt, GuestLockClass::Rsx, wait_for_a);
        if (with_b) c.add("B", 1001, GuestLockClass::Other);
        c.add("C", 1005, GuestLockClass::Other);
        a_go = true;
        a.join();
        c.join();
        return c.order;
    };
    // (c) Rsx, then C, then A: A just held the lock, so the second-best waiter takes the PPU slot.
    CHECK(alternation(false) == names({"Rsx", "C", "A"}));
    // (d) With B ready as well, C is ranked third and runs last.
    CHECK(alternation(true) == names({"Rsx", "B", "A", "C"}));

    // (e) A cancelled yield serves the host waiter and gets the lock back before a lower-ranked PPU thread.
    {
        Case c;
        std::atomic<bool> a_held{false}, a_go{false}, yielded{false};
        std::thread a([&] {
            tox2::setGuestLockPpuPriorityForTesting(1001);
            c.lock.lock();
            a_held = true;
            while (!a_go) std::this_thread::yield();
            yielded = c.lock.yieldToHostWaiters();
            c.order.push_back("A");
            c.lock.unlock();
            tox2::setGuestLockPpuPriorityForTesting(std::nullopt);
        });
        while (!a_held) std::this_thread::yield();
        c.add("C", 1005, GuestLockClass::Other);
        c.add("Rsx", std::nullopt, GuestLockClass::Rsx);
        a_go = true;
        a.join();
        c.join();
        CHECK(yielded);
        CHECK(c.order == names({"Rsx", "A", "C"}));
    }
    // Without a host waiter nothing happens.
    {
        Case c;
        std::atomic<bool> a_held{false}, a_go{false}, yielded{true};
        std::uint64_t releases_before = 0, releases_after = 0;
        std::thread a([&] {
            tox2::setGuestLockPpuPriorityForTesting(1001);
            c.lock.lock();
            a_held = true;
            while (!a_go) std::this_thread::yield();
            releases_before = c.lock.releaseCount();
            yielded = c.lock.yieldToHostWaiters();
            releases_after = c.lock.releaseCount();
            c.order.push_back("A");
            c.lock.unlock();
            tox2::setGuestLockPpuPriorityForTesting(std::nullopt);
        });
        while (!a_held) std::this_thread::yield();
        c.add("C", 1005, GuestLockClass::Other);
        a_go = true;
        a.join();
        c.join();
        CHECK(!yielded);
        CHECK(releases_after == releases_before);
        CHECK(c.order == names({"A", "C"}));
    }

    // (f) The holder-side queries.
    {
        Case c;
        c.lock.lock();
        CHECK(!c.lock.hasPpuWaiterAtOrAbove(3071) && !c.lock.hasHostWaiters());
        c.add("1001", 1001, GuestLockClass::Other);
        c.add("Rsx", std::nullopt, GuestLockClass::Rsx);
        CHECK(c.lock.hasPpuWaiterAtOrAbove(1001));
        CHECK(!c.lock.hasPpuWaiterAtOrAbove(1000));
        CHECK(c.lock.hasHostWaiters());
        c.lock.unlock();
        c.join();
        CHECK(c.order == names({"1001", "Rsx"}));
        CHECK(!c.lock.hasPpuWaiterAtOrAbove(3071) && !c.lock.hasHostWaiters());
    }
}

// Polls until `done` holds; false after five seconds, so a broken wait fails a check instead of hanging the test.
template <class Pred>
bool pollUntil(Pred done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

// A thread that queues for slots as a PPU thread of this lv2 priority; the override is reset on every path.
std::thread slotThread(std::optional<std::int32_t> priority, std::function<void()> body) {
    return std::thread([priority, body = std::move(body)] {
        tox2::setGuestLockPpuPriorityForTesting(priority);
        body();
        tox2::setGuestLockPpuPriorityForTesting(std::nullopt);
    });
}

// Starts a thread and returns once `slots` shows it queued.
void queueSlotThread(tox2::PpuSlots& slots, std::vector<std::thread>& threads, std::int32_t priority,
                     std::function<void()> body) {
    const std::size_t queued = slots.waiterCount();
    threads.push_back(slotThread(priority, std::move(body)));
    CHECK(pollUntil([&] { return slots.waiterCount() > queued; }));
}

void joinAll(std::vector<std::thread>& threads) {
    for (std::thread& t : threads) t.join();
    threads.clear();
}

// Capacity 2 lets two threads hold at once; a third waits for a release. Capacity 1 admits one.
void testPpuSlotsTwoHolders() {
    for (const unsigned capacity : {2u, 1u}) {
        tox2::PpuSlots slots(capacity);
        CHECK(slots.capacity() == capacity);
        std::atomic<int> holding{0};
        std::atomic<bool> go{false}, extra_held{false};
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < capacity; i++) {
            threads.push_back(slotThread(1001, [&] {
                slots.acquire();
                holding++;  // all holders reach this while holding
                pollUntil([&] { return go.load(); });
                slots.release();
            }));
        }
        CHECK(pollUntil([&] { return holding.load() == static_cast<int>(capacity); }));
        threads.push_back(slotThread(1001, [&] {
            slots.acquire();
            extra_held = true;
            slots.release();
        }));
        CHECK(pollUntil([&] { return slots.waiterCount() == 1; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(!extra_held);
        go = true;
        joinAll(threads);
        CHECK(extra_held);
        CHECK(slots.waiterCount() == 0 && slots.releaseCount() == capacity + 1);
        const tox2::PpuSlotStats s = slots.stats();
        CHECK(s.acquisitions == capacity + 1 && s.contended == 1);
        CHECK(slots.arrivals() == capacity + 1);
    }
}

// Waiters are served by lv2 priority, then queue order, at every capacity.
void testPpuSlotsPriority() {
    tox2::PpuSlots slots(2);
    std::mutex order_mutex;
    std::vector<std::string> order;
    std::atomic<bool> second_held{false}, second_go{false};
    std::atomic<int> permits{0};
    const auto order_size = [&] {
        std::lock_guard lock(order_mutex);
        return order.size();
    };
    const auto waiter = [&](std::string name) {
        return [&, name = std::move(name)] {
            slots.acquire();
            {
                std::lock_guard lock(order_mutex);
                order.push_back(name);
            }
            // Holds until the test lets one holder go.
            pollUntil([&] {
                int p = permits.load();
                return p > 0 && permits.compare_exchange_strong(p, p - 1);
            });
            slots.release();
        };
    };

    // The test thread and a second thread fill both slots.
    slots.acquire();
    std::vector<std::thread> threads;
    threads.push_back(slotThread(1001, [&] {
        slots.acquire();
        second_held = true;
        pollUntil([&] { return second_go.load(); });
        slots.release();
    }));
    CHECK(pollUntil([&] { return second_held.load(); }));
    queueSlotThread(slots, threads, 1001, waiter("1001a"));
    queueSlotThread(slots, threads, 100, waiter("100"));
    queueSlotThread(slots, threads, 1001, waiter("1001b"));
    queueSlotThread(slots, threads, 0, waiter("0"));
    CHECK(slots.hasWaiterAtOrAbove(0) && slots.waiterCount() == 4);

    slots.release();
    CHECK(pollUntil([&] { return order_size() == 1; }));
    second_go = true;
    CHECK(pollUntil([&] { return order_size() == 2; }));
    permits++;
    CHECK(pollUntil([&] { return order_size() == 3; }));
    permits++;
    CHECK(pollUntil([&] { return order_size() == 4; }));
    permits += 2;
    joinAll(threads);
    const std::vector<std::string> expected{"0", "100", "1001a", "1001b"};
    CHECK(order == expected);
    CHECK(slots.waiterCount() == 0 && !slots.hasWaiterAtOrAbove(3071));
}

// A yielding holder queues behind its equal-priority peers and ahead of lower-priority waiters.
void testPpuSlotsRotation() {
    for (const unsigned capacity : {1u, 2u}) {
        tox2::PpuSlots slots(capacity);
        std::mutex order_mutex;
        std::vector<std::string> order;
        std::atomic<bool> a_held{false}, a_go{false}, x_held{false}, x_go{false}, yielded{false};
        const auto turn = [&](std::string name) {
            return [&, name = std::move(name)] {
                slots.acquire();
                {
                    std::lock_guard lock(order_mutex);
                    order.push_back(name);
                }
                slots.release();
            };
        };
        std::vector<std::thread> threads;
        threads.push_back(slotThread(1001, [&] {
            slots.acquire();
            a_held = true;
            pollUntil([&] { return a_go.load(); });
            yielded = slots.yield();
            {
                std::lock_guard lock(order_mutex);
                order.push_back("A");
            }
            slots.release();
        }));
        CHECK(pollUntil([&] { return a_held.load(); }));
        std::thread x;
        if (capacity == 2) {
            // Another holder keeps the second slot busy, so the yield is the only thing that frees one.
            x = slotThread(1001, [&] {
                slots.acquire();
                x_held = true;
                pollUntil([&] { return x_go.load(); });
                slots.release();
            });
            CHECK(pollUntil([&] { return x_held.load(); }));
        }
        queueSlotThread(slots, threads, 1001, turn("B"));
        queueSlotThread(slots, threads, 1005, turn("C"));
        a_go = true;
        joinAll(threads);
        x_go = true;
        if (x.joinable()) x.join();
        const std::vector<std::string> expected{"B", "A", "C"};
        CHECK(order == expected);
        CHECK(yielded);
        // The yield, then the three releases that follow it.
        CHECK(slots.releaseCount() == 4 + (capacity == 2 ? 1u : 0u));
    }
}

void testPpuSlotsYield() {
    tox2::PpuSlots slots(1);
    slots.acquire();
    CHECK(slots.heldByCurrentThread());
    const std::uint64_t releases = slots.releaseCount();
    CHECK(!slots.yield());
    CHECK(slots.releaseCount() == releases && slots.heldByCurrentThread());
    CHECK(slots.arrivals() == 1);

    // With a waiter queued the yield hands the slot over and comes back after it.
    std::atomic<bool> waiter_ran{false};
    std::vector<std::thread> threads;
    queueSlotThread(slots, threads, 1001, [&] {
        slots.acquire();
        waiter_ran = true;
        slots.release();
    });
    CHECK(slots.hasWaiterAtOrAbove(1001) && !slots.hasWaiterAtOrAbove(1000));
    CHECK(slots.yield());
    CHECK(waiter_ran && slots.heldByCurrentThread());
    CHECK(slots.releaseCount() == releases + 2);
    joinAll(threads);
    slots.release();
    CHECK(!slots.heldByCurrentThread());
    CHECK(slots.releaseCount() == releases + 3);
}

void testPpuSlotsStats() {
    // Two holders overlap for 20 ms.
    {
        tox2::PpuSlots slots(2);
        std::atomic<int> holding{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 2; i++) {
            threads.push_back(slotThread(1001, [&] {
                slots.acquire();
                holding++;
                pollUntil([&] { return holding.load() == 2; });
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                slots.release();
            }));
        }
        joinAll(threads);
        const tox2::PpuSlotStats s = slots.stats();
        CHECK(s.parallel_us >= 15'000);
        CHECK(s.held_us >= 30'000);
        CHECK(s.full_waiting_us == 0);
        CHECK(s.wait[0].count == 2 && s.wait[0].max_us == 0 && s.wait[1].count == 0);
    }

    // One holder keeps the only slot for 20 ms with a waiter queued the whole time.
    {
        tox2::PpuSlots slots(1);
        slots.acquire();
        std::vector<std::thread> threads;
        queueSlotThread(slots, threads, 100, [&] {
            slots.acquire();
            slots.release();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        slots.release();
        joinAll(threads);
        const tox2::PpuSlotStats s = slots.stats();
        CHECK(s.full_waiting_us >= 15'000);
        CHECK(s.parallel_us == 0);
        CHECK(s.contended == 1 && s.acquisitions == 2);
        // The waiter has priority 100, which is not "ppu-high" (below 100): it counts in the ppu bucket.
        CHECK(s.wait[0].count == 2 && s.wait[0].max_us >= 15'000 && s.wait[1].count == 0);
    }

    // The same hold without a waiter adds nothing to full_waiting_us, and a high-priority waiter lands in [1].
    {
        tox2::PpuSlots slots(1);
        slots.acquire();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        slots.release();
        const tox2::PpuSlotStats s = slots.stats();
        CHECK(s.full_waiting_us == 0 && s.parallel_us == 0 && s.held_us >= 15'000);

        slots.acquire();
        std::vector<std::thread> threads;
        queueSlotThread(slots, threads, 0, [&] {
            slots.acquire();
            slots.release();
        });
        slots.release();
        joinAll(threads);
        CHECK(slots.stats().wait[1].count == 1);
    }
}

// An uncontended acquire + release pair costs at most twice an uncontended GuestLock pair.
void testPpuSlotsUncontendedCost() {
    constexpr int kRuns = 5;
    constexpr int kPairs = 1'000'000;
    tox2::PpuSlots slots(1);
    tox2::GuestLock lock;
    const auto measure = [](auto& primitive, auto&& take, auto&& give) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kPairs; i++) {
            take(primitive);
            give(primitive);
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) / kPairs;
    };
    std::array<double, kRuns> slot_ns{}, lock_ns{};
    for (int run = 0; run < kRuns; run++) {
        slot_ns[run] = measure(slots, [](tox2::PpuSlots& s) { s.acquire(); }, [](tox2::PpuSlots& s) { s.release(); });
        lock_ns[run] = measure(lock, [](tox2::GuestLock& l) { l.lock(); }, [](tox2::GuestLock& l) { l.unlock(); });
    }
    std::ranges::sort(slot_ns);
    std::ranges::sort(lock_ns);
    std::printf("ppu slots uncontended pair: %.0f ns, guest lock uncontended pair: %.0f ns\n", slot_ns[kRuns / 2],
                lock_ns[kRuns / 2]);
    CHECK(slot_ns[kRuns / 2] <= 2.0 * lock_ns[kRuns / 2]);
}

// ---- Split-mode thread lifecycle and kernel helpers ----

// Ends the process when a split-mode test hangs, so a broken lock order fails the run instead of blocking it.
class HangGuard {
public:
    explicit HangGuard(const char* name)
        : thread_([this, name] {
              std::unique_lock lock(mutex_);
              if (!cv_.wait_for(lock, std::chrono::seconds(60), [this] { return done_; })) {
                  std::fprintf(stderr, "test hang in %s\n", name);
                  std::fflush(stderr);
                  std::_Exit(1);
              }
          }) {}
    ~HangGuard() {
        {
            std::lock_guard lock(mutex_);
            done_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }
    HangGuard(const HangGuard&) = delete;
    HangGuard& operator=(const HangGuard&) = delete;

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    std::thread thread_;
};

// Polls for a lock or slot that a thread under test should have released. A thread blocked on it cannot be joined, so
// a timeout ends the process.
template <class Pred>
void requireWithin(std::chrono::milliseconds limit, Pred done, const char* what) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            g_failed++;
            std::fprintf(stderr, "check failed: %s\n", what);
            std::fflush(stderr);
            std::_Exit(1);
        }
        std::this_thread::yield();
    }
}

// True when the global slots have no holder and no waiter.
bool ppuSlotsIdle() {
    tox2::PpuSlots& slots = tox2::ppuSlots();
    return slots.waiterCount() == 0 && slots.stats().acquisitions == slots.releaseCount();
}

// Selects a split mode for a test and restores Legacy at the end, once the guest threads of the test have given up
// their slots (a thread marks itself Exited before it releases its slot).
class SplitModeScope {
public:
    explicit SplitModeScope(tox2::PpuThreadsMode mode) { tox2::setPpuThreadsMode(mode); }
    ~SplitModeScope() {
        CHECK(pollUntil([] { return ppuSlotsIdle(); }));
        tox2::setPpuThreadsMode(tox2::PpuThreadsMode::Legacy);
    }
    SplitModeScope(const SplitModeScope&) = delete;
    SplitModeScope& operator=(const SplitModeScope&) = delete;
};

// Guest memory, registered entry functions (one OPD each) and, optionally, the TLS area, for tests that run real guest
// threads.
class GuestThreadEnv {
public:
    explicit GuestThreadEnv(std::initializer_list<Tox2PpuFunc*> funcs, bool with_tls = false) {
        tox2::setLogSink([](std::string_view) {});
        tox2::resetCensus();
        tox2::resetSyncObjects();
        tox2::resetPpuThreadState();
        tox2::Process& p = tox2::process();
        CHECK(p.memory.reserve());
        base_ = p.memory.base();
        scratch_ = p.main.allocate(0x10000, 0x10000);
        CHECK(scratch_ != 0);
        std::uint32_t index = 0;
        for (Tox2PpuFunc* func : funcs) {
            const std::uint32_t code = 0x10300 + 0x10 * index;
            const std::uint32_t opd = scratch_ + 0x10 + 0x10 * index;
            mappings_.push_back({code, func});
            tox2Write32(base_, opd, code);
            tox2Write32(base_, opd + 4, 0x1111);
            opds_.push_back(opd);
            index++;
        }
        tox2RegisterFuncs(mappings_.data(), static_cast<std::uint32_t>(mappings_.size()));
        if (with_tls) {
            const std::uint32_t tls_image = scratch_ + 0x100;
            tox2Write32(base_, tls_image, 0xCAFEF00D);
            auto boot_ctx = std::make_unique<Tox2PpuContext>();
            CHECK(tox2::initializeTls(boot_ctx.get(), base_, 0x01000000, tls_image, 4, 0x20) != 0);
        }
    }
    ~GuestThreadEnv() {
        tox2::process().main.free(scratch_);
        tox2RegisterFuncs(nullptr, 0);
        tox2::resetSyncObjects();
        tox2::resetPpuThreadState();
        tox2::setLogSink({});
        tox2::resetCensus();
    }
    GuestThreadEnv(const GuestThreadEnv&) = delete;
    GuestThreadEnv& operator=(const GuestThreadEnv&) = delete;

    std::uint8_t* base() const { return base_; }
    std::uint32_t opd(std::size_t index) const { return opds_[index]; }

private:
    std::uint8_t* base_ = nullptr;
    std::uint32_t scratch_ = 0;
    std::vector<Tox2FuncMapping> mappings_;
    std::vector<std::uint32_t> opds_;
};

// A: slot, then kernel lock, then a blocking wait. B takes both while A waits, so the wait released both. A, once
// woken, takes the slot before the kernel lock: B holds the slot and then takes the kernel lock itself, which would
// deadlock if A sat on the kernel lock while it waited for the slot.
void testSplitBlockingWaitOrder() {
    HangGuard guard("testSplitBlockingWaitOrder");
    SplitModeScope mode(tox2::PpuThreadsMode::Split1);
    tox2::PpuSlots& slots = tox2::ppuSlots();
    tox2::GuestLock& kernel = tox2::guestLock();
    std::condition_variable_any cv;
    bool woken = false;  // under the kernel lock
    std::atomic<bool> a_ready{false}, a_returned{false}, a_wait_ok{false}, a_returned_early{true}, b_done{false};
    std::vector<std::thread> threads;
    threads.push_back(slotThread(1001, [&] {
        slots.acquire();
        kernel.lock();
        a_ready = true;
        a_wait_ok = tox2::blockingWait(cv, [&] { return woken; }, "test wait", std::chrono::seconds(10));
        a_returned = true;
        CHECK(kernel.heldByCurrentThread() && slots.heldByCurrentThread());
        kernel.unlock();
        slots.release();
    }));
    CHECK(pollUntil([&] { return a_ready.load(); }));
    threads.push_back(slotThread(1001, [&] {
        slots.acquire();  // possible only once A's wait gave up the slot
        kernel.lock();    // and the kernel lock
        woken = true;
        cv.notify_all();
        kernel.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        a_returned_early = a_returned.load();
        kernel.lock();  // slot held: A must not hold the kernel lock while it waits for that slot
        kernel.unlock();
        b_done = true;
        slots.release();
    }));
    CHECK(pollUntil([&] { return b_done.load(); }));
    joinAll(threads);
    CHECK(!a_returned_early && a_wait_ok);
    CHECK(slots.waiterCount() == 0 && !kernel.hasWaiters());
}

// Two slot holders run at once, and a third thread that takes only the kernel lock runs next to both.
void testSplitTwoRunners() {
    HangGuard guard("testSplitTwoRunners");
    SplitModeScope mode(tox2::PpuThreadsMode::Split2);
    tox2::PpuSlots& slots = tox2::ppuSlots();
    tox2::GuestLock& kernel = tox2::guestLock();
    CHECK(slots.capacity() == 2);
    std::atomic<int> holding{0};
    std::atomic<bool> host_done{false};
    int counter = 0;  // under the kernel lock
    std::vector<std::thread> threads;
    for (int i = 0; i < 2; i++) {
        threads.push_back(slotThread(1001, [&] {
            slots.acquire();
            holding++;
            for (int n = 0; n < 100; n++) {
                tox2::KernelSection section;
                counter++;
            }
            pollUntil([&] { return host_done.load(); });
            slots.release();
        }));
    }
    int held_when_host_done = 0;
    std::thread host([&] {
        pollUntil([&] { return holding.load() == 2; });
        for (int n = 0; n < 200; n++) {
            std::lock_guard<tox2::GuestLock> lock(kernel);
            counter++;
        }
        held_when_host_done = holding.load();
        host_done = true;
    });
    host.join();
    joinAll(threads);
    CHECK(held_when_host_done == 2);
    CHECK(counter == 400);
    CHECK(slots.stats().parallel_us > 0);
}

// sys_ppu_thread_yield in Split2: eligible exactly when a waiter of the same or a higher priority is queued.
void testSplitYield() {
    HangGuard guard("testSplitYield");
    SplitModeScope mode(tox2::PpuThreadsMode::Split2);
    tox2::PpuSlots& slots = tox2::ppuSlots();
    std::atomic<int> stage{0};  // the test lets A proceed: 1 yield to C, 2 yield ten times
    std::atomic<int> a_progress{0};
    std::atomic<bool> a_held{false}, b_held{false}, b_go{false}, c_ran{false}, d_ran{false};
    bool first_result = false, c_ran_at_return = false, d_ran_during = true;
    int later_switches = 0;
    tox2::PpuYieldStats before{}, after{};
    std::vector<std::thread> threads;
    threads.push_back(slotThread(1001, [&] {  // A
        slots.acquire();
        a_held = true;
        pollUntil([&] { return stage.load() >= 1; });
        before = tox2::ppuYieldStats();
        first_result = tox2::yieldPpuThread();
        c_ran_at_return = c_ran.load();
        a_progress = 1;
        pollUntil([&] { return stage.load() >= 2; });
        for (int i = 0; i < 10; i++) {
            if (tox2::yieldPpuThread()) later_switches++;
        }
        d_ran_during = d_ran.load();
        after = tox2::ppuYieldStats();
        a_progress = 2;
        slots.release();
    }));
    CHECK(pollUntil([&] { return a_held.load(); }));
    threads.push_back(slotThread(1001, [&] {  // B holds the second slot
        slots.acquire();
        b_held = true;
        pollUntil([&] { return b_go.load(); });
        slots.release();
    }));
    CHECK(pollUntil([&] { return b_held.load(); }));
    queueSlotThread(slots, threads, 1001, [&] {  // C
        slots.acquire();
        c_ran = true;
        slots.release();
    });
    stage = 1;
    CHECK(pollUntil([&] { return a_progress.load() >= 1; }));
    queueSlotThread(slots, threads, 1005, [&] {  // D ranks below A
        slots.acquire();
        d_ran = true;
        slots.release();
    });
    stage = 2;
    CHECK(pollUntil([&] { return a_progress.load() >= 2; }));
    b_go = true;
    joinAll(threads);
    CHECK(first_result && c_ran_at_return);
    CHECK(later_switches == 0 && !d_ran_during);
    CHECK(after.kept - before.kept == 10);
    CHECK(after.switched - before.switched == 1);
    CHECK(after.to_host == before.to_host);
    CHECK(d_ran);  // A's release gave D the slot
}

void testSplitKernelHelpers() {
    HangGuard guard("testSplitKernelHelpers");
    tox2::GuestLock& kernel = tox2::guestLock();
    const auto heldByOther = [&] {
        bool held = true;
        std::thread([&] { held = kernel.heldByCurrentThread(); }).join();
        return held;
    };
    const auto otherCanLock = [&] {
        bool got = false;
        std::thread([&] {
            kernel.lock();
            got = true;
            kernel.unlock();
        }).join();
        return got;
    };

    // Legacy: the helpers leave the one guest lock as it is.
    CHECK(!tox2::splitPpuMode() && tox2::ppuThreadsMode() == tox2::PpuThreadsMode::Legacy);
    CHECK(tox2::blockingWaitAllowed());
    {
        tox2::KernelSection section;
        CHECK(!kernel.heldByCurrentThread());
    }
    {
        std::lock_guard<tox2::GuestLock> lock(kernel);
        CHECK(kernel.heldByCurrentThread() && !heldByOther());
        {
            tox2::KernelSection section;
            tox2::KernelExit leave;
            CHECK(kernel.heldByCurrentThread());
        }
        CHECK(kernel.heldByCurrentThread());
    }
    CHECK(!kernel.heldByCurrentThread());

    for (const tox2::PpuThreadsMode split : {tox2::PpuThreadsMode::Split1, tox2::PpuThreadsMode::Split2}) {
        SplitModeScope mode(split);
        CHECK(tox2::splitPpuMode() && tox2::ppuThreadsMode() == split);
        CHECK(tox2::ppuSlots().capacity() == (split == tox2::PpuThreadsMode::Split2 ? 2u : 1u));
        CHECK(!tox2::blockingWaitAllowed());  // blockingWait without the kernel lock stops with a fatal error
        {
            tox2::KernelSection outer;
            CHECK(kernel.heldByCurrentThread() && tox2::blockingWaitAllowed());
            CHECK(!heldByOther());
            {
                tox2::KernelSection inner;  // nested: free
                CHECK(kernel.heldByCurrentThread());
            }
            CHECK(kernel.heldByCurrentThread());
            {
                tox2::KernelExit leave;
                CHECK(!kernel.heldByCurrentThread());
                CHECK(otherCanLock());
                {
                    tox2::KernelExit again;  // nothing left to release
                    CHECK(!kernel.heldByCurrentThread());
                }
                CHECK(!kernel.heldByCurrentThread());
            }
            CHECK(kernel.heldByCurrentThread());
            // A timed wait with the lock held returns holding it again.
            std::condition_variable_any cv;
            CHECK(!tox2::blockingWait(cv, [] { return false; }, "test wait", std::chrono::milliseconds(1)));
            CHECK(kernel.heldByCurrentThread());
        }
        CHECK(!kernel.heldByCurrentThread() && otherCanLock());
        {
            tox2::KernelExit leave;  // not held: nothing to release or take back
            CHECK(!kernel.heldByCurrentThread());
        }
        CHECK(!kernel.heldByCurrentThread());
    }
    CHECK(!tox2::splitPpuMode() && tox2::ppuSlots().capacity() == 1);
}

// A host thread holds the kernel lock for 20 ms while a PPU-class thread, inside a call labelled `label` (nullptr:
// none), waits for it. Returns the change of the by-call statistics. In a split mode the waiter takes a slot and enters
// a KernelSection; in Legacy it takes the lock itself, as a legacy guest thread does.
tox2::KernelWaitByCall kernelWaitByCallCase(const char* label) {
    tox2::GuestLock& kernel = tox2::guestLock();
    const tox2::KernelWaitByCall before = kernel.stats().ppu_wait_by_call;
    kernel.lock();
    std::thread waiter = slotThread(1001, [&] {
        const bool split = tox2::splitPpuMode();
        if (split) tox2::ppuSlots().acquire();
        {
            std::optional<tox2::ActivityScope> scope;
            if (label != nullptr) scope.emplace(label);
            if (split) {
                tox2::KernelSection section;
            } else {
                kernel.lock();
                kernel.unlock();
            }
        }
        if (split) tox2::ppuSlots().release();
    });
    CHECK(pollUntil([&] { return kernel.hasWaiters(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    kernel.unlock();
    waiter.join();
    tox2::KernelWaitByCall delta = kernel.stats().ppu_wait_by_call;
    for (std::size_t i = 0; i < tox2::kKernelWaitBucketCount; i++) {
        delta.wait_us[i] -= before.wait_us[i];
        delta.count[i] -= before.count[i];
    }
    return delta;
}

void testKernelWaitByCall() {
    HangGuard guard("testKernelWaitByCall");
    using tox2::KernelWaitBucket;
    struct Case {
        const char* label;
        KernelWaitBucket bucket;
    };
    const Case cases[] = {
            {"sys_lwmutex_lock", KernelWaitBucket::Lwmutex},   {"sys_timer_usleep", KernelWaitBucket::TimerSleep},
            {"cellGcmAddressToOffset", KernelWaitBucket::Gcm}, {"_cellGcmFunc2", KernelWaitBucket::Gcm},
            {"sys_mutex_lock", KernelWaitBucket::Other},       {nullptr, KernelWaitBucket::None},
    };
    {
        SplitModeScope mode(tox2::PpuThreadsMode::Split1);
        for (const Case& c : cases) {
            const tox2::KernelWaitByCall delta = kernelWaitByCallCase(c.label);
            for (std::size_t i = 0; i < tox2::kKernelWaitBucketCount; i++) {
                if (i == static_cast<std::size_t>(c.bucket)) {
                    CHECK(delta.wait_us[i] >= 15'000 && delta.count[i] == 1);
                } else {
                    CHECK(delta.wait_us[i] == 0 && delta.count[i] == 0);
                }
            }
        }
    }
    // Legacy: the same sequence changes no bucket.
    CHECK(!tox2::splitPpuMode());
    for (const Case& c : cases) {
        const tox2::KernelWaitByCall delta = kernelWaitByCallCase(c.label);
        for (std::size_t i = 0; i < tox2::kKernelWaitBucketCount; i++)
            CHECK(delta.wait_us[i] == 0 && delta.count[i] == 0);
    }
}

// A guest read waiting on the host gives up the kernel lock and its slot, and has both again when the call returns. The
// pipe makes the host read block until the test writes, so the order is fixed without any timing: the second thread can
// only get the lock (and, with one slot, the slot) while the first one is inside ReadFile, and the read cannot return
// before the second thread has written.
void testFsReadReleasesKernelLock() {
    HangGuard guard("testFsReadReleasesKernelLock");
    tox2::setLogSink([](std::string_view) {});
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t buf = scratch, nread = scratch + 0x100;

    HANDLE read_end = nullptr, write_end = nullptr;
    CHECK(CreatePipe(&read_end, &write_end, nullptr, 0));
    const std::uint32_t fd = tox2::fsAdoptHostHandleForTesting(read_end, 0);
    CHECK(fd != 0);
    const char payload[] = "pipe!";
    const auto writePayload = [&] {
        DWORD written = 0;
        CHECK(WriteFile(write_end, payload, 5, &written, nullptr) && written == 5);
    };

    for (const tox2::PpuThreadsMode split : {tox2::PpuThreadsMode::Split1, tox2::PpuThreadsMode::Split2}) {
        SplitModeScope mode(split);
        tox2::PpuSlots& slots = tox2::ppuSlots();
        tox2::GuestLock& kernel = tox2::guestLock();
        std::memset(base + buf, 0, 8);
        tox2Write64(base, nread, 0);
        std::atomic<bool> reader_holds{false}, other_got_lock{false}, holds_after{false};
        std::atomic<std::uint64_t> result{~0ull};
        std::thread reader = slotThread(1001, [&] {
            slots.acquire();
            kernel.lock();
            reader_holds = true;
            auto ctx = std::make_unique<Tox2PpuContext>();
            ctx->gpr[3] = fd;
            ctx->gpr[4] = buf;
            ctx->gpr[5] = 5;
            ctx->gpr[6] = nread;
            tox2::sysFsRead(ctx.get(), base);
            result = ctx->gpr[3];
            holds_after = kernel.heldByCurrentThread() && slots.heldByCurrentThread();
            kernel.unlock();
            slots.release();
        });
        CHECK(pollUntil([&] { return reader_holds.load(); }));
        std::thread other = slotThread(1001, [&] {
            slots.acquire();  // with one slot this needs the reader's slot, which it gave up for the host read
            kernel.lock();
            other_got_lock = true;
            kernel.unlock();
            writePayload();
            slots.release();
        });
        if (!pollUntil([&] { return other_got_lock.load(); })) {
            writePayload();  // the check below fails; this only lets the threads end
        }
        reader.join();
        other.join();
        CHECK(other_got_lock);
        CHECK(holds_after);
        CHECK(result == 0 && tox2Read64(base, nread) == 5 && std::memcmp(base + buf, payload, 5) == 0);
        CHECK(slots.waiterCount() == 0 && !kernel.hasWaiters() && !kernel.heldByCurrentThread());
    }

    CHECK(tox2::fsCloseFd(fd) == 0);
    CloseHandle(write_end);
    p.main.free(scratch);
    tox2::setLogSink({});
}

// Every way guest bytes reach RSX local memory is seen by the game view's next check: the user-mode stores through the
// OS write watch, the file read through the explicit mark. A control pair without a write in between stays clean.
void testRsxWriteCheckWriters() {
#if TOX2_RSX_WRITE_WATCH
    HangGuard guard("testRsxWriteCheckWriters");
    tox2::setLogSink([](std::string_view) {});
    constexpr std::uint32_t kPage = 0x1000;
    constexpr std::uint32_t kOffset = 0x0A000000;
    constexpr std::uint32_t kEa = tox2::kRsxLocalBase + kOffset;
    constexpr std::size_t kRangeSize = 4 * kPage;

    tox2::resetRsxState();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    CHECK(p.memory.commit(kEa, kRangeSize));
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t src = scratch, nread = scratch + 0x100;
    std::memcpy(base + src, "write-watch\0", 12);

    tox2::GuestRsxMemoryView view(base);
    const auto check = [&](std::uint64_t since) { return view.checkWrites(0, kOffset, kRangeSize, since); };
    // Runs `write` between two checks, after a control pair that has no write between its checks.
    const auto expectSeen = [&](const auto& write) {
        const auto first = check(0);
        CHECK(first.tracked && !first.clean);
        const auto control = check(first.stamp);
        CHECK(control.tracked && control.clean);
        write();
        const auto second = check(control.stamp);
        CHECK(second.tracked && !second.clean);
    };

    const auto handler = [](std::string_view name) {
        for (const tox2::HleEntry& entry : tox2::sysPrxForUserLibcHandlers()) {
            if (name == entry.name) return entry.handler;
        }
        CHECK(false);
        return tox2::GuestHandler{};
    };
    const auto call = [&](std::string_view name, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2) {
        auto ctx = std::make_unique<Tox2PpuContext>();
        ctx->gpr[3] = a0;
        ctx->gpr[4] = a1;
        ctx->gpr[5] = a2;
        handler(name)(ctx.get(), base);
        CHECK(ctx->gpr[3] == a0);
    };

    expectSeen([&] { tox2Write32(base, kEa + kPage + 0x10, 0x01020304u); });
    expectSeen([&] { call("_sys_memcpy", kEa + 2 * kPage, src, 12); });
    expectSeen([&] { call("_sys_memset", kEa + kPage, 0x41, 8); });
    expectSeen([&] { call("_sys_strcpy", kEa + 3 * kPage, src, 0); });
    expectSeen([&] {
        std::array<std::uint8_t, 16> bytes{};
        bytes[0] = 0x5A;
        tox2::reservations().put(base, kEa + 2 * kPage + 0x40, bytes.data(), 16);
    });

    // A file read into the buffer: the pipe holds the bytes already, so the read does not wait.
    HANDLE read_end = nullptr, write_end = nullptr;
    CHECK(CreatePipe(&read_end, &write_end, nullptr, 0));
    const std::uint32_t fd = tox2::fsAdoptHostHandleForTesting(read_end, 0);
    CHECK(fd != 0);
    const char payload[] = "pipe!";
    DWORD written = 0;
    CHECK(WriteFile(write_end, payload, 5, &written, nullptr) && written == 5);
    {
        SplitModeScope mode(tox2::PpuThreadsMode::Split1);
        expectSeen([&] {
            std::thread reader = slotThread(1001, [&] {
                tox2::PpuSlots& slots = tox2::ppuSlots();
                tox2::GuestLock& kernel = tox2::guestLock();
                slots.acquire();
                kernel.lock();
                auto ctx = std::make_unique<Tox2PpuContext>();
                ctx->gpr[3] = fd;
                ctx->gpr[4] = kEa + 3 * kPage + 0x20;
                ctx->gpr[5] = 5;
                ctx->gpr[6] = nread;
                tox2::sysFsRead(ctx.get(), base);
                CHECK(ctx->gpr[3] == 0 && tox2Read64(base, nread) == 5);
                kernel.unlock();
                slots.release();
            });
            reader.join();
            CHECK(std::memcmp(base + kEa + 3 * kPage + 0x20, payload, 5) == 0);
        });
    }

    CHECK(tox2::fsCloseFd(fd) == 0);
    CloseHandle(write_end);
    p.main.free(scratch);
    tox2::resetRsxState();
    p.memory.decommit(kEa, kRangeSize);
    tox2::setLogSink({});
#endif
}

// Threads using one fd at the same time: sequential reads, positional reads, lseek queries and append writes keep the
// file position semantics of a single caller.
void testFsConcurrentFdUse() {
    namespace stdfs = std::filesystem;
    HangGuard guard("testFsConcurrentFdUse");
    tox2::setLogSink([](std::string_view) {});
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t path_addr = scratch, fd_addr = scratch + 0x200;
    const auto threadBuf = [&](std::uint32_t index) { return scratch + 0x1000 * (index + 1); };

    const stdfs::path root =
            stdfs::temp_directory_path() / ("tox2_fs_io_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    stdfs::remove_all(root, ec);
    stdfs::create_directories(root / "game");
    stdfs::create_directories(root / "hdd" / "dev_hdd0" / "tmp" / "d");
    constexpr std::uint32_t kRecords = 2048;
    const auto record = [](std::uint32_t i) {
        return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(i >> 8), static_cast<std::uint8_t>(i), 0xA5, 0x5A};
    };
    {
        std::ofstream out(root / "game" / "rec.bin", std::ios::binary);
        for (std::uint32_t i = 0; i < kRecords; i++) {
            const auto r = record(i);
            out.write(reinterpret_cast<const char*>(r.data()), r.size());
        }
    }
    tox2::configureVfs(root / "game", root / "hdd");

    const auto putPath = [&](std::string_view text) {
        std::memcpy(base + path_addr, text.data(), text.size());
        base[path_addr + text.size()] = 0;
    };
    putPath("/dev_bdvd/rec.bin");
    CHECK(tox2::fsOpenGuest(base, path_addr, 0, fd_addr, 0, 0) == 0);
    const std::uint32_t fd = tox2Read32(base, fd_addr);
    putPath("/dev_hdd0/tmp/d/app.bin");
    CHECK(tox2::fsOpenGuest(base, path_addr, 02101, fd_addr, 0, 0) == 0);  // WRONLY | CREAT | APPEND
    const std::uint32_t append_fd = tox2Read32(base, fd_addr);

    constexpr int kAppendsPerThread = 300;
    constexpr std::size_t kAppendSize = 8;
    for (const bool split : {false, true}) {
        std::optional<SplitModeScope> mode;
        if (split) mode.emplace(tox2::PpuThreadsMode::Split2);
        // Each thread acts as a guest thread would: slot and kernel lock held around the call.
        const auto asGuest = [&](auto&& call) {
            if (split) {
                tox2::ppuSlots().acquire();
                tox2::guestLock().lock();
            }
            call();
            if (split) {
                tox2::guestLock().unlock();
                tox2::ppuSlots().release();
            }
        };
        const auto lseek = [&](std::uint32_t buf, std::uint64_t offset, std::uint32_t whence) {
            auto ctx = std::make_unique<Tox2PpuContext>();
            ctx->gpr[3] = fd;
            ctx->gpr[4] = offset;
            ctx->gpr[5] = whence;
            ctx->gpr[6] = buf;
            asGuest([&] { tox2::sysFsLseek(ctx.get(), base); });
            return ctx->gpr[3] == 0 ? tox2Read64(base, buf) : ~0ull;
        };
        CHECK(lseek(threadBuf(0), 0, 0) == 0);

        std::vector<std::vector<std::uint32_t>> seen(2);
        std::vector<std::thread> threads;
        for (std::uint32_t t = 0; t < 2; t++) {
            threads.push_back(slotThread(1001, [&, t] {
                const std::uint32_t buf = threadBuf(t), nread = buf + 0x800;
                auto ctx = std::make_unique<Tox2PpuContext>();
                for (std::uint32_t n = 0; n < kRecords + 8; n++) {
                    ctx->gpr[3] = fd;
                    ctx->gpr[4] = buf;
                    ctx->gpr[5] = 4;
                    ctx->gpr[6] = nread;
                    asGuest([&] { tox2::sysFsRead(ctx.get(), base); });
                    if (ctx->gpr[3] != 0 || tox2Read64(base, nread) == 0) break;
                    const std::uint32_t index = (static_cast<std::uint32_t>(base[buf]) << 8) | base[buf + 1];
                    CHECK(tox2Read64(base, nread) == 4 && index < kRecords);
                    CHECK(std::memcmp(base + buf, record(index).data(), 4) == 0);
                    seen[t].push_back(index);
                }
            }));
        }
        threads.push_back(slotThread(1001, [&] {  // positional reads that must not move the position
            const std::uint32_t buf = threadBuf(2), nread = buf + 0x800;
            for (std::uint32_t n = 0; n < 2 * kRecords; n++) {
                const std::uint32_t index = (n * 37) % kRecords;
                std::uint32_t code = ~0u;
                asGuest([&] { code = tox2::fsReadWithOffset(base, fd, index * 4ull, buf, 4, nread); });
                CHECK(code == 0 && tox2Read64(base, nread) == 4);
                CHECK(std::memcmp(base + buf, record(index).data(), 4) == 0);
            }
        }));
        threads.push_back(slotThread(1001, [&] {  // position queries
            for (std::uint32_t n = 0; n < kRecords; n++) {
                const std::uint64_t pos = lseek(threadBuf(3), 0, 1);
                CHECK(pos % 4 == 0 && pos <= 4ull * kRecords);
            }
        }));
        for (std::uint32_t t = 0; t < 2; t++) {
            threads.push_back(slotThread(1001, [&, t] {  // appends of whole records
                const std::uint32_t buf = threadBuf(4 + t), nwrite = buf + 0x800;
                std::memset(base + buf, t == 0 ? 'A' : 'B', kAppendSize - 1);
                base[buf + kAppendSize - 1] = '\n';
                auto ctx = std::make_unique<Tox2PpuContext>();
                for (int n = 0; n < kAppendsPerThread; n++) {
                    ctx->gpr[3] = append_fd;
                    ctx->gpr[4] = buf;
                    ctx->gpr[5] = kAppendSize;
                    ctx->gpr[6] = nwrite;
                    asGuest([&] { tox2::sysFsWrite(ctx.get(), base); });
                    CHECK(ctx->gpr[3] == 0 && tox2Read64(base, nwrite) == kAppendSize);
                }
            }));
        }
        joinAll(threads);

        std::vector<std::uint32_t> all = seen[0];
        all.insert(all.end(), seen[1].begin(), seen[1].end());
        std::ranges::sort(all);
        bool exact = all.size() == kRecords;
        for (std::uint32_t i = 0; exact && i < kRecords; i++) exact = all[i] == i;
        CHECK(exact);  // every record read once: no repeat, no skip
        CHECK(lseek(threadBuf(0), 0, 1) == 4ull * kRecords);
    }

    std::ifstream in(root / "hdd" / "dev_hdd0" / "tmp" / "d" / "app.bin", std::ios::binary);
    const std::string appended(std::istreambuf_iterator<char>(in), {});
    CHECK(appended.size() == 2 * 2 * kAppendsPerThread * kAppendSize);
    bool whole = appended.size() % kAppendSize == 0;
    for (std::size_t off = 0; whole && off < appended.size(); off += kAppendSize) {
        const std::string_view line(appended.data() + off, kAppendSize);
        whole = (line == "AAAAAAA\n" || line == "BBBBBBB\n");
    }
    CHECK(whole);  // no append overwrote another

    CHECK(tox2::fsCloseFd(fd) == 0);
    CHECK(tox2::fsCloseFd(append_fd) == 0);
    p.main.free(scratch);
    tox2::setLogSink({});
    stdfs::remove_all(root, ec);
}

constexpr int kStepBudget = kTox2PreemptBudget;
std::atomic<int> g_step_count{0};

// Steps through three budgets without any syscall.
TOX2_PPU_FUNC(stepLoopEntry) {
    for (int step = 1; step <= 3 * kStepBudget; step++) {
        g_step_count.store(step, std::memory_order_relaxed);
        tox2Loop(ctx, base);
    }
}

// The creation rule must not depend on the new thread blocking: a thread that only computes still has to run first,
// until its budget expires.
void splitCreateStepLoopCase(tox2::PpuThreadsMode mode_to_test) {
    GuestThreadEnv env({stepLoopEntry});
    SplitModeScope mode(mode_to_test);
    g_step_count = 0;
    std::atomic<int> returned_step{-1};
    std::atomic<std::uint32_t> thread_id{0};
    std::atomic<bool> create_ok{false}, holds_after{false}, creator_done{false};
    std::thread creator = slotThread(1001, [&] {
        tox2::PpuSlots& slots = tox2::ppuSlots();
        tox2::GuestLock& kernel = tox2::guestLock();
        slots.acquire();
        kernel.lock();
        tox2::GuestThreadParams params;
        params.entry_opd = env.opd(0);
        params.priority = 500;
        params.stack_size = 0x10000;
        params.name = "stepper";
        tox2::GuestThread* thread = tox2::prepareGuestThread(params);
        if (thread != nullptr) {
            thread_id = thread->id();
            create_ok = tox2::startGuestThreadFirst(thread);
            returned_step = g_step_count.load();
        }
        holds_after = kernel.heldByCurrentThread() && slots.heldByCurrentThread();
        kernel.unlock();
        slots.release();
        creator_done = true;
    });
    CHECK(pollUntil([&] { return creator_done.load(); }));
    creator.join();
    CHECK(create_ok && holds_after);
    CHECK(returned_step >= kStepBudget);  // not before the first budget expiry of the new thread
    const tox2::GuestThread* thread = tox2::findThread(thread_id);
    CHECK(thread != nullptr && pollUntil([&] { return thread->state() == tox2::ThreadState::Exited; }));
    CHECK(g_step_count == 3 * kStepBudget);
}

void testSplitCreatePriority() {
    HangGuard guard("testSplitCreatePriority");
    {
        SplitModeScope mode(tox2::PpuThreadsMode::Split1);
        ppuThreadCreatePriorityScenario(false);
    }
    {
        // The second slot is free, so a new thread of equal or lower priority runs next to its creator.
        SplitModeScope mode(tox2::PpuThreadsMode::Split2);
        ppuThreadCreatePriorityScenario(true);
    }
    splitCreateStepLoopCase(tox2::PpuThreadsMode::Split1);
    splitCreateStepLoopCase(tox2::PpuThreadsMode::Split2);
}

std::atomic<bool> g_exit_section_held{false};

// What HLE sys_ppu_thread_exit does inside runImport: the handler runs in a kernel section and ends the thread.
TOX2_PPU_FUNC(exitInSectionEntry) {
    (void)base;
    tox2::KernelSection section;
    g_exit_section_held = tox2::guestLock().heldByCurrentThread();
    tox2::exitCurrentThread(ctx->gpr[3]);
}

void testSplitExitInKernelSection() {
    HangGuard guard("testSplitExitInKernelSection");
    for (const tox2::PpuThreadsMode split : {tox2::PpuThreadsMode::Split1, tox2::PpuThreadsMode::Split2}) {
        GuestThreadEnv env({exitInSectionEntry});
        SplitModeScope mode(split);
        g_exit_section_held = false;
        tox2::GuestThreadParams params;
        params.entry_opd = env.opd(0);
        params.arg0 = 0x1234;
        params.priority = 1001;
        params.stack_size = 0x10000;
        params.name = "exiter";
        const std::uint32_t id = tox2::createGuestThread(params);
        CHECK(id != 0);
        const tox2::GuestThread* thread = tox2::findThread(id);
        CHECK(thread != nullptr);
        if (thread == nullptr) continue;
        CHECK(pollUntil([&] { return thread->state() == tox2::ThreadState::Exited; }));
        CHECK(g_exit_section_held && thread->exitValue() == 0x1234);

        // The section's destructor never ran: exit has to release the kernel lock and the slot itself.
        std::atomic<bool> kernel_ok{false}, slot_ok{false};
        std::thread host([&] {
            std::lock_guard<tox2::GuestLock> lock(tox2::guestLock());
            kernel_ok = true;
        });
        requireWithin(std::chrono::seconds(1), [&] { return kernel_ok.load(); }, "kernel lock not released at exit");
        host.join();
        std::thread guest([&] {
            tox2::ppuSlots().acquire();
            slot_ok = true;
            tox2::ppuSlots().release();
        });
        requireWithin(std::chrono::seconds(1), [&] { return slot_ok.load(); }, "slot not released at exit");
        guest.join();
    }
}

TOX2_PPU_FUNC(returnPathEntry) {
    (void)ctx;
    (void)base;
}

std::atomic<int> g_free_tls_warnings{0};

// Threads that return from their entry function free their TLS block on the way out; that free is lv2 state and
// must be ordered with the allocations a slot holder makes in kernel sections.
void testSplitReturnPathTls() {
    HangGuard guard("testSplitReturnPathTls");
    GuestThreadEnv env({returnPathEntry}, true);
    g_free_tls_warnings = 0;
    tox2::setLogSink([](std::string_view line) {
        if (line.find("freeTls(") != std::string_view::npos) g_free_tls_warnings++;
    });
    SplitModeScope mode(tox2::PpuThreadsMode::Split2);
    tox2::PpuSlots& slots = tox2::ppuSlots();

    // The guest threads that are alive (at most two) and their r13. A thread frees its block and becomes Exited without
    // leaving the kernel lock, so inside a kernel section a thread that is not Exited still owns its block.
    std::array<std::atomic<std::uint32_t>, 2> live_r13{}, live_id{};
    std::atomic<bool> stop{false};
    std::atomic<int> duplicates{0}, churn_rounds{0};
    const auto clashesWithLive = [&](std::uint32_t r13) {  // in a kernel section
        for (std::size_t c = 0; c < live_id.size(); c++) {
            const std::uint32_t id = live_id[c].load();
            if (id != 0 && r13 != 0 && live_r13[c].load() == r13 &&
                tox2::findThread(id)->state() != tox2::ThreadState::Exited) {
                return true;
            }
        }
        return false;
    };
    std::vector<std::thread> threads;
    threads.push_back(slotThread(1001, [&] {
        slots.acquire();
        while (!stop.load()) {
            tox2::KernelSection section;
            const std::uint32_t r13 = tox2::allocateTls();
            if (clashesWithLive(r13)) duplicates++;
            tox2::freeTls(r13);
            churn_rounds++;
        }
        slots.release();
    }));

    constexpr int kThreadCount = 50;
    const auto reapExited = [&] {
        for (std::size_t c = 0; c < live_id.size(); c++) {
            const std::uint32_t id = live_id[c].load();
            if (id != 0 && tox2::findThread(id)->state() == tox2::ThreadState::Exited) live_id[c] = 0;
        }
    };
    for (int i = 0; i < kThreadCount; i++) {
        std::size_t cell = live_id.size();
        CHECK(pollUntil([&] {
            reapExited();
            for (std::size_t c = 0; c < live_id.size(); c++) {
                if (live_id[c].load() == 0) {
                    cell = c;
                    return true;
                }
            }
            return false;
        }));
        if (cell == live_id.size()) break;
        tox2::GuestThreadParams params;
        params.entry_opd = env.opd(0);
        params.priority = 1001;
        params.stack_size = 0x4000;
        params.name = "returner";
        std::uint32_t id = 0;
        {
            tox2::KernelSection section;
            params.tls_r13 = tox2::allocateTls();
            if (clashesWithLive(params.tls_r13)) duplicates++;
            live_r13[cell] = params.tls_r13;
            id = tox2::createGuestThread(params);
            live_id[cell] = id;
        }
        CHECK(id != 0);
    }
    CHECK(pollUntil([&] {
        reapExited();
        return live_id[0].load() == 0 && live_id[1].load() == 0;
    }));
    stop = true;
    joinAll(threads);
    CHECK(duplicates == 0);
    CHECK(churn_rounds > 0);
    CHECK(g_free_tls_warnings == 0);
    tox2::setLogSink([](std::string_view) {});

    // No slot was lost on the way.
    std::atomic<int> holding{0};
    std::atomic<bool> go{false};
    for (int i = 0; i < 2; i++) {
        threads.push_back(slotThread(1001, [&] {
            slots.acquire();
            holding++;
            pollUntil([&] { return go.load(); });
            slots.release();
        }));
    }
    CHECK(pollUntil([&] { return holding.load() == 2; }));
    go = true;
    joinAll(threads);
}

// Budget expiry inside a handler's kernel section (as in the GCM interrupt and sail service bodies): the kernel lock
// goes first, so the thread that takes the slot can take the kernel lock, and comes back last.
void testSplitYieldInsideKernel() {
    HangGuard guard("testSplitYieldInsideKernel");
    SplitModeScope mode(tox2::PpuThreadsMode::Split1);
    tox2::PpuSlots& slots = tox2::ppuSlots();
    std::atomic<bool> a_ready{false}, a_go{false}, b_turn{false}, a_done{false};
    bool a_holds_after = false;
    std::vector<std::thread> threads;
    threads.push_back(slotThread(1001, [&] {  // A
        slots.acquire();
        {
            tox2::KernelSection section;
            a_ready = true;
            pollUntil([&] { return a_go.load(); });
            auto ctx = std::make_unique<Tox2PpuContext>();
            tox2Yield(ctx.get(), nullptr);
            a_holds_after = tox2::guestLock().heldByCurrentThread() && slots.heldByCurrentThread();
        }
        a_done = true;
        slots.release();
    }));
    CHECK(pollUntil([&] { return a_ready.load(); }));
    queueSlotThread(slots, threads, 1001, [&] {  // B has the same priority
        slots.acquire();
        {
            std::lock_guard<tox2::GuestLock> lock(tox2::guestLock());
            b_turn = true;
        }
        slots.release();
    });
    a_go = true;
    CHECK(pollUntil([&] { return a_done.load(); }));
    joinAll(threads);
    CHECK(b_turn && a_holds_after);
}

// Two PPU threads increment one word with lwarx/stwcx. while an SPU-style GETLLAR/PUTLLC loop increments another word
// of the same line: neither count loses an update.
void testSplitReservationCounter() {
    HangGuard guard("testSplitReservationCounter");
    SplitModeScope mode(tox2::PpuThreadsMode::Split2);
    std::unique_ptr<tox2::GuestMemory> memory(newMemory());
    std::uint8_t* base = memory->base();
    CHECK(memory->commit(0x20000, 0x1000));
    constexpr std::uint32_t kPpuWord = 0x20400, kSpuWord = 0x20408;  // the same 128-byte line, separate 8-byte chunks
    constexpr int kPpuIncrements = 200'000, kSpuIncrements = 50'000;
    tox2::PpuSlots& slots = tox2::ppuSlots();
    std::vector<std::thread> threads;
    for (int i = 0; i < 2; i++) {
        threads.push_back(slotThread(1001, [&] {
            slots.acquire();
            auto ctx = std::make_unique<Tox2PpuContext>();
            for (int n = 0; n < kPpuIncrements; n++) {
                for (;;) {
                    const std::uint32_t value = tox2Lwarx(ctx.get(), base, kPpuWord);
                    if (tox2Stwcx(ctx.get(), base, kPpuWord, value + 1)) break;
                }
            }
            slots.release();
        }));
    }
    threads.push_back(std::thread([&] {
        tox2::Reservation reservation;
        std::uint8_t line[tox2::kReservationLineSize];
        for (int n = 0; n < kSpuIncrements; n++) {
            for (;;) {
                tox2::reservations().getllar(base, kSpuWord, line, reservation);
                const std::uint32_t offset = kSpuWord % tox2::kReservationLineSize;
                const std::uint32_t value = (std::uint32_t{line[offset]} << 24) |
                                            (std::uint32_t{line[offset + 1]} << 16) |
                                            (std::uint32_t{line[offset + 2]} << 8) | line[offset + 3];
                const std::uint32_t next = value + 1;
                line[offset] = static_cast<std::uint8_t>(next >> 24);
                line[offset + 1] = static_cast<std::uint8_t>(next >> 16);
                line[offset + 2] = static_cast<std::uint8_t>(next >> 8);
                line[offset + 3] = static_cast<std::uint8_t>(next);
                if (tox2::reservations().putllc(base, kSpuWord, line, reservation)) break;
            }
        }
    }));
    joinAll(threads);
    CHECK(tox2Read32(base, kPpuWord) == 2 * kPpuIncrements);
    CHECK(tox2Read32(base, kSpuWord) == kSpuIncrements);
}

// ---- Kernel sections at the syscall and HLE boundary ----

TOX2_PPU_FUNC(boundaryDummyEntry) {
    (void)ctx;
    (void)base;
}

// Guest memory for a boundary test, with a stack area for the threads that call back into guest code.
class BoundaryEnv {
public:
    BoundaryEnv() : env_({boundaryDummyEntry}) {
        tox2::resetHostFunctions();
        tox2::resetCellSysutil();
        tox2::Process& p = tox2::process();
        data_ = p.main.allocate(0x10000, 0x10000);
        stack_ = p.main.allocate(0x10000, 0x10000);
        CHECK(data_ != 0 && stack_ != 0);
    }
    ~BoundaryEnv() {
        tox2::installImports(nullptr, 0);
        tox2::resetCellSysutil();
        tox2::resetHostFunctions();
        tox2::process().main.free(stack_);
        tox2::process().main.free(data_);
    }
    BoundaryEnv(const BoundaryEnv&) = delete;
    BoundaryEnv& operator=(const BoundaryEnv&) = delete;

    std::uint8_t* base() const { return env_.base(); }
    std::uint32_t data() const { return data_; }
    // A caller object whose r1 points into its own part of the stack area.
    std::unique_ptr<tox2::GuestThread> makeCallerWithStack(std::uint32_t id, unsigned index) const {
        auto caller = makeCaller(id);
        caller->context().gpr[1] = stack_ + 0x4000 + 0x1000 * index;
        return caller;
    }

private:
    GuestThreadEnv env_;
    std::uint32_t data_ = 0, stack_ = 0;
};

// Four slot-holding threads (priorities 1001, 1001, 500, 100) run `iteration(caller)` 20,000 times each while a host
// thread takes the kernel lock in 300 us slices. Returns the wall time.
std::chrono::milliseconds runKernelStress(const std::function<void(tox2::GuestThread&)>& iteration) {
    constexpr std::array<std::int32_t, 4> kPriorities{1001, 1001, 500, 100};
    constexpr int kIterations = 20'000;
    tox2::PpuSlots& slots = tox2::ppuSlots();
    std::vector<std::unique_ptr<tox2::GuestThread>> callers;
    for (std::size_t i = 0; i < kPriorities.size(); i++) {
        callers.push_back(makeCaller(0x01000000 + static_cast<std::uint32_t>(i)));
    }
    std::atomic<bool> stop{false};
    std::thread host([&] {
        while (!stop.load()) {
            {
                std::lock_guard<tox2::GuestLock> lock(tox2::guestLock());
                const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(300);
                while (std::chrono::steady_clock::now() < until) {
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < kPriorities.size(); i++) {
        threads.push_back(slotThread(kPriorities[i], [&, i] {
            slots.acquire();
            for (int n = 0; n < kIterations; n++) {
                iteration(*callers[i]);
                if (n % 100 == 99) syscallAs(*callers[i], 43);
            }
            slots.release();
        }));
    }
    joinAll(threads);
    const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    stop = true;
    host.join();
    return elapsed;
}

const char* splitName(tox2::PpuThreadsMode mode) { return mode == tox2::PpuThreadsMode::Split1 ? "split1" : "split2"; }

// The guest word is incremented with plain accesses under an lv2 mutex: only the kernel sections around the handlers
// (and the lock handoff through them) keep the count exact.
void splitSyscallStressCase(tox2::PpuThreadsMode split) {
    BoundaryEnv env;
    std::uint8_t* base = env.base();
    const std::uint32_t id_out = env.data(), attr = env.data() + 0x40, word = env.data() + 0x80;
    auto setup = makeCaller(0x01000100);
    tox2Write32(base, attr, tox2::kSyncFifo);
    tox2Write32(base, attr + 4, tox2::kSyncNotRecursive);
    CHECK(syscallAs(*setup, 100, id_out, attr) == 0);
    const std::uint32_t mutex = tox2Read32(base, id_out);
    tox2Write32(base, word, 0);

    SplitModeScope mode(split);
    std::atomic<int> failures{0};
    const auto elapsed = runKernelStress([&](tox2::GuestThread& caller) {
        if (syscallAs(caller, 102, mutex, 0) != 0) failures++;
        tox2Write32(base, word, tox2Read32(base, word) + 1);
        if (syscallAs(caller, 104, mutex) != 0) failures++;
    });
    CHECK(failures == 0);
    CHECK(tox2Read32(base, word) == 80'000);
    CHECK(elapsed < std::chrono::seconds(30));
    std::printf("split syscall stress (%s): %lld ms\n", splitName(split), static_cast<long long>(elapsed.count()));
}

// The same through tox2HleCall with an lwmutex from an installed import table.
void splitHleStressCase(tox2::PpuThreadsMode split) {
    BoundaryEnv env;
    std::uint8_t* base = env.base();
    constexpr const char* kModule = "sysPrxForUser";
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x2f85c0ef, kModule, "sys_lwmutex_create"},
            {0x00800020, 0x1573dc3f, kModule, "sys_lwmutex_lock"},
            {0x00800040, 0x1bc200f4, kModule, "sys_lwmutex_unlock"},
    };
    tox2::installImports(imports, 3);
    enum { kCreate, kLock, kUnlock };
    const auto call = [&](tox2::GuestThread& caller, std::uint32_t index, std::uint64_t r3, std::uint64_t r4) {
        Tox2PpuContext* ctx = &caller.context();
        ctx->gpr[3] = r3;
        ctx->gpr[4] = r4;
        tox2HleCall(ctx, base, index);
        return ctx->gpr[3];
    };
    const std::uint32_t lwmutex = env.data(), attr = env.data() + 0x40, word = env.data() + 0x80;
    auto setup = makeCaller(0x01000100);
    tox2Write32(base, attr, tox2::kSyncFifo);
    tox2Write32(base, attr + 4, tox2::kSyncNotRecursive);
    CHECK(call(*setup, kCreate, lwmutex, attr) == 0);
    tox2Write32(base, word, 0);

    SplitModeScope mode(split);
    std::atomic<int> failures{0};
    const auto elapsed = runKernelStress([&](tox2::GuestThread& caller) {
        if (call(caller, kLock, lwmutex, 0) != 0) failures++;
        tox2Write32(base, word, tox2Read32(base, word) + 1);
        if (call(caller, kUnlock, lwmutex, 0) != 0) failures++;
    });
    CHECK(failures == 0);
    CHECK(tox2Read32(base, word) == 80'000);
    CHECK(elapsed < std::chrono::seconds(30));
    std::printf("split hle stress (%s): %lld ms\n", splitName(split), static_cast<long long>(elapsed.count()));
}

void testSplitSyscallStress() {
    HangGuard guard("testSplitSyscallStress");
    for (const tox2::PpuThreadsMode split : {tox2::PpuThreadsMode::Split1, tox2::PpuThreadsMode::Split2}) {
        splitSyscallStressCase(split);
        splitHleStressCase(split);
    }
}

// The operations of LwmutexWord on the pair at `addr`; the same results are expected for an aligned address (atomic
// 64-bit accesses) and a misaligned one (plain accesses, which need the kernel lock in the split modes).
void checkLwmutexWordOps(std::uint8_t* base, std::uint32_t addr) {
    using tox2::LwmutexPair;
    tox2::LwmutexWord word(base, addr);
    CHECK(word.aligned() == ((addr & 7) == 0));
    const auto stored = [&] { return LwmutexPair{tox2Read32(base, addr), tox2Read32(base, addr + 4)}; };

    tox2Write32(base, addr, 0x01000001);
    tox2Write32(base, addr + 4, 7);
    CHECK(word.load() == (LwmutexPair{0x01000001, 7}));

    // A failed compare-and-swap reports the current pair and writes nothing.
    LwmutexPair expected{0x01000001, 8};
    CHECK(!word.compareExchange(expected, {1, 2}));
    CHECK(expected == (LwmutexPair{0x01000001, 7}));
    CHECK(stored() == (LwmutexPair{0x01000001, 7}));
    CHECK(word.compareExchange(expected, {tox2::kLwmutexFree, 0}));
    CHECK(stored() == (LwmutexPair{tox2::kLwmutexFree, 0}));

    // update() leaves the word alone when the function returns the pair it was given: the function writes a different
    // waiter value behind its back, which a store of the old pair would overwrite.
    int calls = 0;
    LwmutexPair seen = word.update([&](LwmutexPair pair) {
        calls++;
        tox2Write32(base, addr + 4, 5);
        return pair;
    });
    CHECK(calls == 1 && seen == (LwmutexPair{tox2::kLwmutexFree, 0}));
    CHECK(stored() == (LwmutexPair{tox2::kLwmutexFree, 5}));

    // A change between the load and the swap makes update() run the function again on the new pair.
    calls = 0;
    bool raced = false;
    seen = word.update([&](LwmutexPair pair) {
        calls++;
        if (!raced) {
            raced = true;
            tox2Write32(base, addr + 4, 9);
        }
        return LwmutexPair{pair.owner, pair.waiter + 1};
    });
    CHECK(calls == 2 && seen == (LwmutexPair{tox2::kLwmutexFree, 9}));
    CHECK(stored() == (LwmutexPair{tox2::kLwmutexFree, 10}));
}

void testLwmutexWord() {
    using tox2::LwmutexPair;
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);

    // The packed value is the host u64 whose bytes in memory are the big-endian owner, then the big-endian waiter.
    const LwmutexPair pair{0x01000001, 0x00000007};
    tox2Write32(base, scratch, pair.owner);
    tox2Write32(base, scratch + 4, pair.waiter);
    std::uint64_t raw = 0;
    std::memcpy(&raw, base + scratch, sizeof raw);
    CHECK(tox2::packLwmutexPair(pair) == raw);
    CHECK(tox2::unpackLwmutexPair(raw) == pair);
    CHECK(tox2::unpackLwmutexPair(tox2::packLwmutexPair({tox2::kLwmutexReserved, 0xfffffffe})) ==
          (LwmutexPair{tox2::kLwmutexReserved, 0xfffffffe}));

    checkLwmutexWordOps(base, scratch + 0x40);
    checkLwmutexWordOps(base, scratch + 0x84);  // 4 mod 8
    {
        HangGuard guard("testLwmutexWord");
        SplitModeScope mode(tox2::PpuThreadsMode::Split2);
        checkLwmutexWordOps(base, scratch + 0x40);  // the atomic variant needs no kernel lock
        std::lock_guard<tox2::GuestLock> kernel(tox2::guestLock());
        checkLwmutexWordOps(base, scratch + 0x84);
        checkLwmutexWordOps(base, scratch + 0x40);
    }
    p.main.free(scratch);
}

// The lwmutex protocol through the native entry points, one case per ppu-threads mode. Every call opens a kernel
// section, which is a no-op in legacy mode, so the same sequences run in both.
void lwmutexProtocolCase(tox2::PpuThreadsMode mode) {
    using tox2::LwmutexPair;
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::resetCensus();
    tox2::resetSyncObjects();
    tox2::resetLogOnce();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t lw = scratch, lw_odd = scratch + 0x104, attr = scratch + 0x80;
    auto a = makeCaller(0x01000000);
    auto b = makeCaller(0x01000001);
    auto e = makeCaller(0x01000004);

    SplitModeScope scope(mode);
    const auto ctxOf = [](tox2::GuestThread& caller) { return &caller.context(); };
    const auto create = [&](std::uint32_t addr, std::uint32_t protocol, std::uint32_t recursive) {
        tox2::KernelSection section;
        tox2Write32(base, attr, protocol);
        tox2Write32(base, attr + 4, recursive);
        return tox2::lwmutexCreate(base, addr, attr);
    };
    const auto lock = [&](tox2::GuestThread& who, std::uint32_t addr, std::uint64_t timeout_us = 0) {
        tox2::KernelSection section;
        return tox2::lwmutexLock(ctxOf(who), base, addr, timeout_us);
    };
    const auto tryLock = [&](tox2::GuestThread& who, std::uint32_t addr) {
        tox2::KernelSection section;
        return tox2::lwmutexTryLock(ctxOf(who), base, addr);
    };
    const auto unlock = [&](tox2::GuestThread& who, std::uint32_t addr) {
        tox2::KernelSection section;
        return tox2::lwmutexUnlock(ctxOf(who), base, addr);
    };
    const auto destroy = [&](tox2::GuestThread& who, std::uint32_t addr) {
        tox2::KernelSection section;
        return tox2::lwmutexDestroy(ctxOf(who), base, addr);
    };
    const auto pairAt = [&](std::uint32_t addr) {
        return LwmutexPair{tox2Read32(base, addr), tox2Read32(base, addr + 4)};
    };
    const auto idle = LwmutexPair{tox2::kLwmutexFree, 0};
    const tox2::SyncHandoffStats before = tox2::syncHandoffStats();

    // Uncontended use never queues: the pair goes {Free,0} -> {tid,0} -> {Free,0} and the slow counters stay put.
    CHECK(create(lw, tox2::kSyncRetry, tox2::kSyncRecursive) == 0);
    CHECK(pairAt(lw) == idle);
    CHECK(lock(*a, lw) == 0 && pairAt(lw) == (LwmutexPair{0x01000000, 0}));
    CHECK(lock(*a, lw) == 0 && tryLock(*a, lw) == 0 && tox2Read32(base, lw + 12) == 2);
    CHECK(tryLock(*b, lw) == tox2::kCellEbusy);
    CHECK(unlock(*b, lw) == tox2::kCellEperm);
    CHECK(destroy(*a, lw) == tox2::kCellEbusy && destroy(*b, lw) == tox2::kCellEbusy);
    CHECK(unlock(*a, lw) == 0 && unlock(*a, lw) == 0 && pairAt(lw) == (LwmutexPair{0x01000000, 0}));
    CHECK(unlock(*a, lw) == 0 && pairAt(lw) == idle);
    CHECK(unlock(*a, lw) == tox2::kCellEperm);
    CHECK(tox2::syncHandoffStats().lwmutex_slow_locks == before.lwmutex_slow_locks);
    CHECK(tox2::syncHandoffStats().lwmutex_slow_unlocks == before.lwmutex_slow_unlocks);

    // A RETRY lwmutex still hands off: with a blocked waiter the unlock leaves the owner word reserved for it.
    CHECK(lock(*a, lw) == 0);
    std::uint32_t b_result = ~0u;
    std::thread waiter([&] {
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        b_result = lock(*b, lw);
        unlock(*b, lw);
    });
    CHECK(waitUnderGuestLock([&] { return pairAt(lw) == (LwmutexPair{0x01000000, 1}); }));
    {
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        CHECK(unlock(*a, lw) == 0);
        CHECK(pairAt(lw) == (LwmutexPair{tox2::kLwmutexReserved, 1}));
        CHECK(tryLock(*a, lw) == tox2::kCellEbusy);
    }
    waiter.join();
    CHECK(b_result == 0 && pairAt(lw) == idle);
    CHECK(tox2::syncHandoffStats().lwmutex_handoffs - before.lwmutex_handoffs == 1);

    // A grant that races with the waiter's timeout still makes it the owner and it reports success.
    CHECK(lock(*a, lw) == 0);
    std::uint32_t e_result = ~0u;
    std::thread timed([&] {
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        e_result = lock(*e, lw, 20'000);
    });
    CHECK(waitUnderGuestLock([&] { return pairAt(lw) == (LwmutexPair{0x01000000, 1}); }));
    {
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(unlock(*a, lw) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    timed.join();
    CHECK(e_result == 0 && pairAt(lw) == (LwmutexPair{0x01000004, 0}));
    CHECK(unlock(*e, lw) == 0 && pairAt(lw) == idle);

    // A timeout without a grant takes the waiter out of the count again and leaves the owner alone.
    CHECK(lock(*a, lw) == 0);
    {
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        CHECK(lock(*b, lw, 1000) == tox2::kCellEtimedout);
    }
    CHECK(pairAt(lw) == (LwmutexPair{0x01000000, 0}));
    CHECK(unlock(*a, lw) == 0 && pairAt(lw) == idle);

    // Only the contended calls took the slow paths: three locks that queued, two unlocks that found a waiter.
    const tox2::SyncHandoffStats contended = tox2::syncHandoffStats();
    CHECK(contended.lwmutex_slow_locks - before.lwmutex_slow_locks == 3);
    CHECK(contended.lwmutex_slow_unlocks - before.lwmutex_slow_unlocks == 2);
    CHECK(contended.lwmutex_misaligned == before.lwmutex_misaligned);

    // Destroy on a free lwmutex writes Dead, and a dead lwmutex refuses everything.
    CHECK(destroy(*a, lw) == 0 && pairAt(lw) == (LwmutexPair{tox2::kLwmutexDead, 0}));
    CHECK(lock(*a, lw) == tox2::kCellEinval && tryLock(*a, lw) == tox2::kCellEinval);
    CHECK(destroy(*a, lw) == tox2::kCellEinval);
    CHECK(pairAt(lw) == (LwmutexPair{tox2::kLwmutexDead, 0}));

    // A lwmutex that is not 8-byte aligned works through the kernel lock, is counted per call and is reported once.
    CHECK(create(lw_odd, tox2::kSyncFifo, tox2::kSyncRecursive) == 0);
    CHECK(lock(*a, lw_odd) == 0 && pairAt(lw_odd) == (LwmutexPair{0x01000000, 0}));
    CHECK(lock(*a, lw_odd) == 0 && tox2Read32(base, lw_odd + 12) == 1);
    CHECK(tryLock(*b, lw_odd) == tox2::kCellEbusy);
    CHECK(unlock(*a, lw_odd) == 0 && unlock(*a, lw_odd) == 0 && pairAt(lw_odd) == idle);
    CHECK(unlock(*a, lw_odd) == tox2::kCellEperm);
    CHECK(tox2::syncHandoffStats().lwmutex_misaligned - contended.lwmutex_misaligned == 6);
    CHECK(countContaining(lines, "not 8-byte aligned") == 1);
    CHECK(destroy(*a, lw_odd) == 0 && pairAt(lw_odd) == (LwmutexPair{tox2::kLwmutexDead, 0}));

    p.main.free(scratch);
    tox2::resetSyncObjects();
    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

void testLwmutexProtocol() {
    HangGuard guard("testLwmutexProtocol");
    lwmutexProtocolCase(tox2::PpuThreadsMode::Legacy);
    lwmutexProtocolCase(tox2::PpuThreadsMode::Split2);
}

// The lwmutex imports of sysPrxForUser, called through tox2HleCall like generated code does.
class LwmutexHleEnv {
public:
    enum Import : std::uint32_t { Create, Lock, TryLock, Unlock, Destroy, ImportCount };

    LwmutexHleEnv() {
        static const Tox2ImportInfo kImports[] = {
                {0x00800000, 0x2f85c0ef, "sysPrxForUser", "sys_lwmutex_create"},
                {0x00800010, 0x1573dc3f, "sysPrxForUser", "sys_lwmutex_lock"},
                {0x00800020, 0xaeb78725, "sysPrxForUser", "sys_lwmutex_trylock"},
                {0x00800030, 0x1bc200f4, "sysPrxForUser", "sys_lwmutex_unlock"},
                {0x00800040, 0xc3476d0c, "sysPrxForUser", "sys_lwmutex_destroy"},
        };
        tox2::installImports(kImports, ImportCount);
    }
    LwmutexHleEnv(const LwmutexHleEnv&) = delete;
    LwmutexHleEnv& operator=(const LwmutexHleEnv&) = delete;

    std::uint8_t* base() const { return env_.base(); }
    // Guest address of the lwmutex with this index; the structs are 8-byte aligned and do not overlap.
    std::uint32_t lwmutex(std::uint32_t index) const { return env_.data() + 0x100 * index; }
    // Plain 32-bit counters that the tests increment inside a critical section.
    std::uint32_t counter(std::uint32_t index) const { return env_.data() + 0x4000 + 4 * index; }

    // The result of the call as the 32-bit error code the guest sees.
    std::uint32_t call(tox2::GuestThread& caller, Import index, std::uint64_t r3 = 0, std::uint64_t r4 = 0) const {
        Tox2PpuContext* ctx = &caller.context();
        ctx->gpr[3] = r3;
        ctx->gpr[4] = r4;
        tox2HleCall(ctx, base(), index);
        return static_cast<std::uint32_t>(ctx->gpr[3]);
    }
    bool create(tox2::GuestThread& caller, std::uint32_t index, std::uint32_t protocol, std::uint32_t recursive) const {
        const std::uint32_t attr = env_.data() + 0x3000;
        tox2Write32(base(), attr, protocol);
        tox2Write32(base(), attr + 4, recursive);
        return call(caller, Create, lwmutex(index), attr) == 0;
    }
    bool idle(std::uint32_t index) const {
        return tox2Read32(base(), lwmutex(index)) == tox2::kLwmutexFree && tox2Read32(base(), lwmutex(index) + 4) == 0;
    }

private:
    BoundaryEnv env_;
};

// In Split2, a slot holder's uncontended lwmutex calls return while a host thread holds the kernel lock; with the fast
// path off the same calls wait for it.
void testLwmutexKernelFree() {
    HangGuard guard("testLwmutexKernelFree");
    LwmutexHleEnv env;
    auto caller = makeCaller(0x01000000);
    CHECK(env.create(*caller, 0, tox2::kSyncFifo, tox2::kSyncRecursive));
    const std::uint32_t lw = env.lwmutex(0);

    const auto phase = [&](bool fast_path) {
        tox2::setLwmutexFastPathEnabled(fast_path);
        SplitModeScope mode(tox2::PpuThreadsMode::Split2);
        tox2::PpuSlots& slots = tox2::ppuSlots();
        enum Step { kLock, kTryLock, kUnlock, kUnlockLast, kStepCount };
        std::array<std::uint32_t, kStepCount> results{};
        std::array<std::chrono::steady_clock::time_point, kStepCount> returned{};
        std::atomic<bool> go{false};
        std::thread holder = slotThread(1001, [&] {
            slots.acquire();
            CHECK(pollUntil([&] { return go.load(); }));
            const LwmutexHleEnv::Import calls[kStepCount] = {LwmutexHleEnv::Lock, LwmutexHleEnv::TryLock,
                                                             LwmutexHleEnv::Unlock, LwmutexHleEnv::Unlock};
            for (int i = 0; i < kStepCount; i++) {
                results[i] = env.call(*caller, calls[i], lw, 0);
                returned[i] = std::chrono::steady_clock::now();
            }
            slots.release();
        });
        std::chrono::steady_clock::time_point hold_end;
        {
            std::lock_guard<tox2::GuestLock> host(tox2::guestLock());
            go = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            hold_end = std::chrono::steady_clock::now();
        }
        holder.join();
        for (int i = 0; i < kStepCount; i++) {
            CHECK(results[i] == 0);
            // The calls run in order: with the lock held by the host the first one waits, and the rest follow it.
            if (fast_path) {
                CHECK(returned[i] < hold_end);
            } else {
                CHECK(returned[i] >= hold_end);
            }
        }
        CHECK(env.idle(0));
    };
    phase(true);
    phase(false);
    tox2::setLwmutexFastPathEnabled(true);
}

// Runs `body(caller, index, preemption_point)` on one thread per priority and returns the wall time. Each thread holds
// the guest lock for its whole run in legacy mode and a PPU slot in the split modes, as guest code does; a host thread
// takes the kernel lock in 300 us slices throughout. The preemption point is where a legacy or one-slot thread would
// lose the CPU in the middle of a critical section: it gives up the guest lock (legacy) or yields (Split1), and it is
// empty in Split2, where the other slot runs in parallel anyway.
using LwmutexThreadBody =
        std::function<void(tox2::GuestThread&, std::size_t, const std::function<void()>& preemption_point)>;
std::chrono::milliseconds runLwmutexThreads(tox2::PpuThreadsMode mode, std::span<const std::int32_t> priorities,
                                            const LwmutexThreadBody& body) {
    const bool legacy = mode == tox2::PpuThreadsMode::Legacy;
    SplitModeScope scope(mode);
    tox2::PpuSlots& slots = tox2::ppuSlots();
    std::vector<std::unique_ptr<tox2::GuestThread>> callers;
    for (std::size_t i = 0; i < priorities.size(); i++) {
        callers.push_back(makeCaller(0x01000000 + static_cast<std::uint32_t>(i)));
    }
    std::atomic<bool> stop{false};
    std::thread host([&] {
        while (!stop.load()) {
            {
                std::lock_guard<tox2::GuestLock> lock(tox2::guestLock());
                const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(300);
                while (std::chrono::steady_clock::now() < until) {
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < priorities.size(); i++) {
        threads.push_back(slotThread(priorities[i], [&, i] {
            std::unique_lock<tox2::GuestLock> guest_lock(tox2::guestLock(), std::defer_lock);
            if (legacy) {
                guest_lock.lock();
            } else {
                slots.acquire();
            }
            const std::function<void()> preemption_point = [&] {
                if (legacy) {
                    guest_lock.unlock();
                    std::this_thread::yield();
                    guest_lock.lock();
                } else if (mode == tox2::PpuThreadsMode::Split1) {
                    syscallAs(*callers[i], 43);
                }
            };
            body(*callers[i], i, preemption_point);
            if (legacy) {
                guest_lock.unlock();
            } else {
                slots.release();
            }
        }));
    }
    joinAll(threads);
    const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    stop = true;
    host.join();
    return elapsed;
}

const char* modeName(tox2::PpuThreadsMode mode) {
    return mode == tox2::PpuThreadsMode::Legacy ? "legacy" : splitName(mode);
}

// Every case below counts with plain loads and stores inside the critical section: only the lwmutex (its compare-and-
// swap on the owner/waiter pair and the hand-off through the kernel lock) keeps the counts exact.
void lwmutexContentionCase(tox2::PpuThreadsMode mode) {
    constexpr int kIterations = 50'000;
    constexpr std::chrono::seconds kLimit{30};
    LwmutexHleEnv env;
    using Env = LwmutexHleEnv;
    std::uint8_t* base = env.base();
    auto setup = makeCaller(0x01000100);
    std::atomic<int> failures{0};
    const auto ok = [&](std::uint32_t code) {
        if (code != 0) failures++;
    };
    const auto bump = [&](std::uint32_t counter) { tox2Write32(base, counter, tox2Read32(base, counter) + 1); };
    // Every lwmutex ends free with no waiter and destroys cleanly.
    const auto finish = [&](std::uint32_t count) {
        for (std::uint32_t i = 0; i < count; i++) {
            CHECK(env.idle(i));
            CHECK(env.call(*setup, Env::Destroy, env.lwmutex(i)) == 0);
        }
    };
    const auto begin = [&](std::uint32_t count, std::uint32_t protocol, std::uint32_t recursive) {
        failures = 0;
        for (std::uint32_t i = 0; i < count; i++) {
            CHECK(env.create(*setup, i, protocol, recursive));
            tox2Write32(base, env.counter(i), 0);
        }
        return tox2::syncHandoffStats();
    };
    const auto report = [&](const char* name, std::chrono::milliseconds elapsed, const tox2::SyncHandoffStats& before) {
        const tox2::SyncHandoffStats after = tox2::syncHandoffStats();
        CHECK(failures == 0);
        CHECK(elapsed < kLimit);
        // Slow locks that did not wait took the lwmutex, or found it dead, in the kernel section: the owner had
        // unlocked between the failed compare-and-swap and the queueing.
        const std::uint64_t slow_locks = after.lwmutex_slow_locks - before.lwmutex_slow_locks;
        const std::uint64_t waits = after.lwmutex_waits - before.lwmutex_waits;
        std::printf(
                "lwmutex contention %s (%s): %lld ms, slow locks %llu unlocks %llu, waits %llu hand-offs %llu, "
                "slow locks - waits %llu\n",
                name, modeName(mode), static_cast<long long>(elapsed.count()),
                static_cast<unsigned long long>(slow_locks),
                static_cast<unsigned long long>(after.lwmutex_slow_unlocks - before.lwmutex_slow_unlocks),
                static_cast<unsigned long long>(waits),
                static_cast<unsigned long long>(after.lwmutex_handoffs - before.lwmutex_handoffs),
                static_cast<unsigned long long>(slow_locks - waits));
    };
    constexpr std::array<std::int32_t, 4> kFour{1001, 1001, 500, 100};
    constexpr std::array<std::int32_t, 4> kEqualFour{1001, 1001, 1001, 1001};
    constexpr std::array<std::int32_t, 2> kTwo{1001, 1001};

    {  // (a) four threads on one FIFO lwmutex
        const auto before = begin(1, tox2::kSyncFifo, tox2::kSyncNotRecursive);
        const auto elapsed = runLwmutexThreads(mode, kFour, [&](tox2::GuestThread& caller, std::size_t, auto& preempt) {
            for (int n = 0; n < kIterations; n++) {
                ok(env.call(caller, Env::Lock, env.lwmutex(0), 0));
                bump(env.counter(0));
                if (n % 64 == 63) preempt();
                ok(env.call(caller, Env::Unlock, env.lwmutex(0)));
            }
        });
        report("fifo", elapsed, before);
        CHECK(tox2Read32(base, env.counter(0)) == 4 * kIterations);
        finish(1);
    }
    {  // (b) the same on one recursive PRIORITY lwmutex, every 8th iteration locking twice
        const auto before = begin(1, tox2::kSyncPriority, tox2::kSyncRecursive);
        const auto elapsed = runLwmutexThreads(mode, kFour, [&](tox2::GuestThread& caller, std::size_t, auto& preempt) {
            for (int n = 0; n < kIterations; n++) {
                const bool twice = n % 8 == 0;
                ok(env.call(caller, Env::Lock, env.lwmutex(0), 0));
                if (twice) ok(env.call(caller, Env::Lock, env.lwmutex(0), 0));
                bump(env.counter(0));
                if (n % 64 == 63) preempt();
                if (twice) ok(env.call(caller, Env::Unlock, env.lwmutex(0)));
                ok(env.call(caller, Env::Unlock, env.lwmutex(0)));
            }
        });
        report("priority recursive", elapsed, before);
        CHECK(tox2Read32(base, env.counter(0)) == 4 * kIterations);
        finish(1);
    }
    {  // (c) two threads over 8 lwmutexes in round-robin order
        const auto before = begin(8, tox2::kSyncFifo, tox2::kSyncNotRecursive);
        const auto elapsed = runLwmutexThreads(mode, kTwo, [&](tox2::GuestThread& caller, std::size_t, auto& preempt) {
            for (int n = 0; n < kIterations; n++) {
                const std::uint32_t k = static_cast<std::uint32_t>(n % 8);
                ok(env.call(caller, Env::Lock, env.lwmutex(k), 0));
                bump(env.counter(k));
                if (n % 64 == 63) preempt();
                ok(env.call(caller, Env::Unlock, env.lwmutex(k)));
            }
        });
        report("round-robin", elapsed, before);
        for (std::uint32_t k = 0; k < 8; k++) CHECK(tox2Read32(base, env.counter(k)) == 2 * kIterations / 8);
        finish(8);
    }
    {  // (d) two threads spinning on try-lock against two locking with a 100 us timeout
        const auto before = begin(1, tox2::kSyncFifo, tox2::kSyncNotRecursive);
        const auto elapsed =
                runLwmutexThreads(mode, kEqualFour, [&](tox2::GuestThread& caller, std::size_t index, auto& preempt) {
                    const bool try_lock = index < 2;
                    for (int done = 0; done < kIterations;) {
                        const std::uint32_t result = try_lock ? env.call(caller, Env::TryLock, env.lwmutex(0))
                                                              : env.call(caller, Env::Lock, env.lwmutex(0), 100);
                        if (result == 0) {
                            bump(env.counter(0));
                            // Only the timed lockers lose the CPU while they own the lwmutex; the try-lock spinners
                            // yield on EBUSY instead, so a preempted owner always gets to run again.
                            if (!try_lock && done % 64 == 63) preempt();
                            ok(env.call(caller, Env::Unlock, env.lwmutex(0)));
                            done++;
                        } else if (try_lock && result == tox2::kCellEbusy) {
                            // The owner may be waiting for a slot that this spinner holds.
                            syscallAs(caller, 43);
                        } else if (!try_lock && result == tox2::kCellEtimedout) {
                            continue;
                        } else {
                            failures++;
                            break;
                        }
                    }
                });
        report("try-lock against timed lock", elapsed, before);
        CHECK(tox2Read32(base, env.counter(0)) == 4 * kIterations);
        finish(1);
    }
}

void testLwmutexContentionStress() {
    HangGuard guard("testLwmutexContentionStress");
    for (const tox2::PpuThreadsMode mode :
         {tox2::PpuThreadsMode::Legacy, tox2::PpuThreadsMode::Split1, tox2::PpuThreadsMode::Split2}) {
        lwmutexContentionCase(mode);
    }
}

// Busy-waits for about `ns` nanoseconds without a system call, so the caller stays on its CPU.
void spinFor(std::int64_t ns) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(ns);
    while (std::chrono::steady_clock::now() < until) {
    }
}

// Destroy against a fast lock (Split2): the holder locks and unlocks in a loop while the main thread destroys the
// lwmutex. Destroy is EBUSY while the holder owns it or wins the compare-and-swap race, and once it succeeds the next
// lock must see the lwmutex dead. The compare-and-swap in destroy is what keeps a lock from slipping in after it.
void testLwmutexDestroyRace() {
    HangGuard guard("testLwmutexDestroyRace");
    LwmutexHleEnv env;
    using Env = LwmutexHleEnv;
    std::uint8_t* base = env.base();
    auto setup = makeCaller(0x01000100);
    auto holder_caller = makeCaller(0x01000000);
    CHECK(env.create(*setup, 0, tox2::kSyncFifo, tox2::kSyncNotRecursive));
    const std::uint32_t lw = env.lwmutex(0), counter = env.counter(0);
    tox2Write32(base, counter, 0);

    SplitModeScope mode(tox2::PpuThreadsMode::Split2);
    tox2::PpuSlots& slots = tox2::ppuSlots();
    std::atomic<std::uint32_t> locks{0};
    std::atomic<int> failures{0};
    std::atomic<bool> stop{false};
    std::uint32_t last_result = 0;
    std::thread holder = slotThread(1001, [&] {
        slots.acquire();
        while (!stop.load()) {
            last_result = env.call(*holder_caller, Env::Lock, lw, 0);
            if (last_result != 0) break;
            tox2Write32(base, counter, tox2Read32(base, counter) + 1);
            locks++;
            if (env.call(*holder_caller, Env::Unlock, lw) != 0) failures++;
        }
        slots.release();
    });
    CHECK(pollUntil([&] { return locks.load() >= 1000; }));

    std::uint64_t busy = 0;
    bool destroyed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!destroyed && std::chrono::steady_clock::now() < deadline) {
        const std::uint32_t result = env.call(*setup, Env::Destroy, lw);
        if (result == 0) {
            destroyed = true;
        } else if (result == tox2::kCellEbusy) {
            busy++;
        } else {
            failures++;
            break;
        }
    }
    stop = true;
    holder.join();
    CHECK(destroyed);
    CHECK(failures == 0);
    CHECK(last_result == tox2::kCellEinval);
    CHECK(tox2Read32(base, lw) == tox2::kLwmutexDead && tox2Read32(base, lw + 4) == 0);
    CHECK(tox2Read32(base, counter) == locks.load());
    std::printf("lwmutex destroy against fast lock (split2): %llu locks, destroy EBUSY %llu times\n",
                static_cast<unsigned long long>(locks.load()), static_cast<unsigned long long>(busy));
}

// Fast unlocks against slow-path publishing (Split2): two slot holders lock and unlock one lwmutex with a short,
// varying pause outside the critical section, so an unlock lands at every point of the other thread's failed
// compare-and-swap, queueing and wait. A host thread tries the lock now and then. No unlock may miss a waiter.
void testLwmutexPublishRace() {
    HangGuard guard("testLwmutexPublishRace");
    constexpr int kIterations = 20'000;
    LwmutexHleEnv env;
    using Env = LwmutexHleEnv;
    std::uint8_t* base = env.base();
    auto setup = makeCaller(0x01000100);
    auto try_caller = makeCaller(0x01000010);
    std::array<std::unique_ptr<tox2::GuestThread>, 2> callers{makeCaller(0x01000000), makeCaller(0x01000001)};
    CHECK(env.create(*setup, 0, tox2::kSyncFifo, tox2::kSyncNotRecursive));
    const std::uint32_t lw = env.lwmutex(0), counter = env.counter(0);
    tox2Write32(base, counter, 0);
    const tox2::SyncHandoffStats before = tox2::syncHandoffStats();

    SplitModeScope mode(tox2::PpuThreadsMode::Split2);
    tox2::PpuSlots& slots = tox2::ppuSlots();
    std::atomic<int> failures{0};
    std::atomic<bool> stop{false};
    std::array<std::uint32_t, 2> bumps{};
    std::uint32_t try_bumps = 0;
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < 2; i++) {
        threads.push_back(slotThread(1001, [&, i] {
            slots.acquire();
            for (int n = 0; n < kIterations; n++) {
                if (env.call(*callers[i], Env::Lock, lw, 0) != 0) failures++;
                // No critical-section work on half of the iterations, one increment on the rest.
                if ((n + i) % 2 == 0) {
                    tox2Write32(base, counter, tox2Read32(base, counter) + 1);
                    bumps[i]++;
                }
                if (env.call(*callers[i], Env::Unlock, lw) != 0) failures++;
                spinFor((n % 7) * 20);
            }
            slots.release();
        }));
    }
    std::thread try_locker([&] {
        while (!stop.load()) {
            const std::uint32_t result = env.call(*try_caller, Env::TryLock, lw);
            if (result == 0) {
                tox2Write32(base, counter, tox2Read32(base, counter) + 1);
                try_bumps++;
                if (env.call(*try_caller, Env::Unlock, lw) != 0) failures++;
            } else if (result != tox2::kCellEbusy) {
                failures++;
            }
            spinFor(20'000);
        }
    });
    joinAll(threads);
    stop = true;
    try_locker.join();
    const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    CHECK(failures == 0);
    CHECK(elapsed < std::chrono::seconds(10));
    CHECK(tox2Read32(base, counter) == bumps[0] + bumps[1] + try_bumps);
    CHECK(env.idle(0));
    CHECK(env.call(*setup, Env::Destroy, lw) == 0);
    const tox2::SyncHandoffStats after = tox2::syncHandoffStats();
    const std::uint64_t slow_locks = after.lwmutex_slow_locks - before.lwmutex_slow_locks;
    const std::uint64_t waits = after.lwmutex_waits - before.lwmutex_waits;
    std::printf(
            "lwmutex publish race (split2): %lld ms, slow locks %llu unlocks %llu, waits %llu, "
            "slow locks - waits %llu, try-lock successes %u\n",
            static_cast<long long>(elapsed.count()), static_cast<unsigned long long>(slow_locks),
            static_cast<unsigned long long>(after.lwmutex_slow_unlocks - before.lwmutex_slow_unlocks),
            static_cast<unsigned long long>(waits), static_cast<unsigned long long>(slow_locks - waits), try_bumps);
}

// Nanoseconds per uncontended lock + unlock pair through tox2HleCall with `holders` slot holders in parallel, each on
// its own lwmutex. Includes the census mutex that every import call takes, so it is a trend figure, not a bound.
double lwmutexPairNs(LwmutexHleEnv& env, std::uint32_t holders, bool fast_path) {
    constexpr int kPairs = 200'000;
    tox2::setLwmutexFastPathEnabled(fast_path);
    SplitModeScope mode(tox2::PpuThreadsMode::Split2);
    tox2::PpuSlots& slots = tox2::ppuSlots();
    std::vector<std::unique_ptr<tox2::GuestThread>> callers;
    for (std::uint32_t i = 0; i < holders; i++) callers.push_back(makeCaller(0x01000000 + i));
    std::atomic<std::uint32_t> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (std::uint32_t i = 0; i < holders; i++) {
        threads.push_back(slotThread(1001, [&, i] {
            slots.acquire();
            ready++;
            pollUntil([&] { return go.load(); });
            for (int n = 0; n < kPairs; n++) {
                env.call(*callers[i], LwmutexHleEnv::Lock, env.lwmutex(i), 0);
                env.call(*callers[i], LwmutexHleEnv::Unlock, env.lwmutex(i));
            }
            slots.release();
        }));
    }
    CHECK(pollUntil([&] { return ready.load() == holders; }));
    const auto start = std::chrono::steady_clock::now();
    go = true;
    joinAll(threads);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    tox2::setLwmutexFastPathEnabled(true);
    return std::chrono::duration<double, std::nano>(elapsed).count() / kPairs;
}

// Report only: how the cost of an uncontended pair changes when a second slot holder works on another lwmutex.
void testLwmutexPairBenchmark() {
    HangGuard guard("testLwmutexPairBenchmark");
    constexpr int kRuns = 5;
    LwmutexHleEnv env;
    auto setup = makeCaller(0x01000100);
    for (std::uint32_t i = 0; i < 2; i++) CHECK(env.create(*setup, i, tox2::kSyncFifo, tox2::kSyncNotRecursive));
    const auto median = [&](std::uint32_t holders, bool fast_path) {
        std::array<double, kRuns> samples{};
        lwmutexPairNs(env, holders, fast_path);  // warm-up
        for (double& sample : samples) sample = lwmutexPairNs(env, holders, fast_path);
        std::ranges::sort(samples);
        return samples[kRuns / 2];
    };
    const double one_on = median(1, true), two_on = median(2, true);
    const double one_off = median(1, false), two_off = median(2, false);
    CHECK(env.idle(0) && env.idle(1));
    std::printf(
            "lwmutex lock+unlock pair, ns per pair (split2): fast path on: 1 holder %.0f, 2 holders %.0f; "
            "off: 1 holder %.0f, 2 holders %.0f\n",
            one_on, two_on, one_off, two_off);
    std::printf("lwmutex pair ratio two holders / one holder: fast path on %.2f, off %.2f\n", two_on / one_on,
                two_off / one_off);
}

// An initialised gcm state (no consumer thread) with the imports the kernel-free tests call through tox2HleCall:
// cellGcmSys and the libc, sysPrxForUser, sysutil and rtc handlers marked kernel_free. The IO area is mapped at IO
// offset 0, so `area() + n` has the offset n.
class KernelFreeEnv {
public:
    enum Import : std::uint32_t {
        AddressToOffset,
        GetFlipStatus,
        GetLabelAddress,
        SetFlipCommand,
        GetTimeStamp,
        GetControlRegister,
        GetTiledPitchSize,
        SetFlipMode,
        MapMain,
        UnmapEa,
        Strlen,
        Strcpy,
        Strcat,
        Strncpy,
        Strncat,
        Strncmp,
        Memcmp,
        Memset,
        Memcpy,
        Snprintf,
        Vsnprintf,
        AtExitSpawn,
        AtExitSpawnUpper,
        GetThreadId,
        GetSystemTime,
        GetBgmStatus,
        VideoOutGetResolution,
        VideoOutSetGamma,
        RtcGetCurrentTick,
        RtcConvertUtcToLocalTime,
        ImportCount
    };

    KernelFreeEnv() {
        tox2::resetRsxState();
        area_ = tox2::process().user1m.allocate(0x400000, 0x100000);
        CHECK(area_ != 0 && (area_ & 0xfffff) == 0);
        static const Tox2ImportInfo kImports[] = {
                {0x00800000, 0x21ac3697, "cellGcmSys", "cellGcmAddressToOffset"},
                {0x00800010, 0x72a577ce, "cellGcmSys", "cellGcmGetFlipStatus"},
                {0x00800020, 0xf80196c1, "cellGcmSys", "cellGcmGetLabelAddress"},
                {0x00800030, 0x21397818, "cellGcmSys", "_cellGcmSetFlipCommand"},
                {0x00800040, 0x5a41c10f, "cellGcmSys", "cellGcmGetTimeStamp"},
                {0x00800050, 0xa547adde, "cellGcmSys", "cellGcmGetControlRegister"},
                {0x00800060, 0x055bd74d, "cellGcmSys", "cellGcmGetTiledPitchSize"},
                {0x00800070, 0x4ae8d215, "cellGcmSys", "cellGcmSetFlipMode"},
                {0x00800080, 0xa114ec67, "cellGcmSys", "cellGcmMapMainMemory"},
                {0x00800090, 0xefd00f54, "cellGcmSys", "cellGcmUnmapEaIoAddress"},
                {0x008000a0, 0x2d36462b, "sysPrxForUser", "_sys_strlen"},
                {0x008000b0, 0x99c88692, "sysPrxForUser", "_sys_strcpy"},
                {0x008000c0, 0x052d29a6, "sysPrxForUser", "_sys_strcat"},
                {0x008000d0, 0xd3039d4d, "sysPrxForUser", "_sys_strncpy"},
                {0x008000e0, 0x996f7cf8, "sysPrxForUser", "_sys_strncat"},
                {0x008000f0, 0x04e83d2c, "sysPrxForUser", "_sys_strncmp"},
                {0x00800100, 0xfb5db080, "sysPrxForUser", "_sys_memcmp"},
                {0x00800110, 0x68b9b011, "sysPrxForUser", "_sys_memset"},
                {0x00800120, 0x6bf66ea7, "sysPrxForUser", "_sys_memcpy"},
                {0x00800130, 0x06574237, "sysPrxForUser", "_sys_snprintf"},
                {0x00800140, 0x0618936b, "sysPrxForUser", "_sys_vsnprintf"},
                {0x00800150, 0x2c847572, "sysPrxForUser", "_sys_process_atexitspawn"},
                {0x00800160, 0x96328741, "sysPrxForUser", "_sys_process_at_Exitspawn"},
                {0x00800170, 0x350d454e, "sysPrxForUser", "sys_ppu_thread_get_id"},
                {0x00800180, 0x8461e528, "sysPrxForUser", "sys_time_get_system_time"},
                {0x00800190, 0xa11552f6, "cellSysutil", "cellSysutilGetBgmPlaybackStatus"},
                {0x008001a0, 0xe558748d, "cellSysutil", "cellVideoOutGetResolution"},
                {0x008001b0, 0xc7020f62, "cellSysutilAvconfExt", "cellVideoOutSetGamma"},
                {0x008001c0, 0x9dafc0d9, "cellRtc", "cellRtcGetCurrentTick"},
                {0x008001d0, 0xc48d5002, "cellRtc", "cellRtcConvertUtcToLocalTime"},
        };
        tox2::installImports(kImports, ImportCount);
        std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
        CHECK(tox2::gcmInitBody(env_.base(), env_.data(), 0x10000, kIoSize, area_, kLocalSize) == 0);
    }
    ~KernelFreeEnv() {
        tox2::resetRsxState();
        tox2::process().user1m.free(area_);
    }
    KernelFreeEnv(const KernelFreeEnv&) = delete;
    KernelFreeEnv& operator=(const KernelFreeEnv&) = delete;

    static constexpr std::uint32_t kIoSize = 0x100000, kLocalSize = 0x200000;

    std::uint8_t* base() const { return env_.base(); }
    std::uint32_t data() const { return env_.data(); }
    std::uint32_t area() const { return area_; }

    std::uint64_t call(tox2::GuestThread& caller, Import index, std::initializer_list<std::uint64_t> args) const {
        Tox2PpuContext* ctx = &caller.context();
        int reg = 3;
        for (const std::uint64_t value : args) ctx->gpr[reg++] = value;
        tox2HleCall(ctx, base(), index);
        return ctx->gpr[3];
    }

private:
    BoundaryEnv env_;
    std::uint32_t area_ = 0;
};

// In Split2, a slot holder's call of each kernel-free handler returns while a host thread holds the kernel lock;
// a handler that took the lock would return only after the hold ended.
void testKernelFreeHandlers() {
    HangGuard guard("testKernelFreeHandlers");
    KernelFreeEnv gcm;
    tox2::resetCellSysutil();  // the BGM status check expects the default (enabled) state
    auto caller = makeCaller(0x01000000);
    caller->context().fpr[1].f64 = 1.0;  // the gamma argument of cellVideoOutSetGamma
    constexpr std::uint32_t kOffsetMinutes = 540;
    tox2::setRtcLocalOffsetMinutes(kOffsetMinutes);

    // Scratch layout inside the data area, one slot per handler that writes.
    const std::uint32_t out = gcm.data();
    const std::uint32_t str_abc = out + 0x100, str_abd = out + 0x110, fmt = out + 0x120, va_list = out + 0x130;
    const std::uint32_t cat_buf = out + 0x200, ncat_buf = out + 0x220, set_buf = out + 0x240, cpy_buf = out + 0x260;
    const std::uint32_t snp_buf = out + 0x280, vsn_buf = out + 0x2a0, tid_out = out + 0x2c0;
    const std::uint32_t bgm_out = out + 0x300, res_out = out + 0x340, utc_tick = out + 0x360, local_tick = out + 0x368;
    std::memcpy(gcm.base() + str_abc, "abc", 4);
    std::memcpy(gcm.base() + str_abd, "abd", 4);
    std::memcpy(gcm.base() + fmt, "x%d", 4);
    tox2Write64(gcm.base(), va_list, 7);
    constexpr std::uint64_t kMinusOne = ~std::uint64_t{0};  // s32 -1 sign-extended

    struct HandlerCall {
        const char* name;
        KernelFreeEnv::Import import;
        std::array<std::uint64_t, 4> args;
        std::optional<std::uint64_t> expected;  // nullopt: the result is void or not deterministic
    };
    using Env = KernelFreeEnv;
    const std::array<HandlerCall, 28> calls{{
            {"cellGcmAddressToOffset", KernelFreeEnv::AddressToOffset, {gcm.area() + 8, out}, 0},
            {"cellGcmGetFlipStatus", KernelFreeEnv::GetFlipStatus, {}, tox2::kRsxFlipStatusDone},
            {"cellGcmGetLabelAddress",
             KernelFreeEnv::GetLabelAddress,
             {1, 0},
             tox2::kRsxLabelAddr + tox2::kRsxLabelStride},
            {"_cellGcmSetFlipCommand", KernelFreeEnv::SetFlipCommand, {tox2::kRsxDeviceAddr, 0}, std::nullopt},
            {"cellGcmGetTimeStamp", KernelFreeEnv::GetTimeStamp, {0, 0}, ~std::uint64_t{0}},
            {"cellGcmGetControlRegister", KernelFreeEnv::GetControlRegister, {}, tox2::kRsxControlAddr},
            {"cellGcmGetTiledPitchSize", KernelFreeEnv::GetTiledPitchSize, {0x1400, 0}, 0x1400},
            {"cellGcmSetFlipMode", KernelFreeEnv::SetFlipMode, {tox2::kRsxDisplayVsync, 0}, std::nullopt},
            {"_sys_strlen", Env::Strlen, {str_abc}, 3},
            {"_sys_strcpy", Env::Strcpy, {cat_buf, str_abc}, cat_buf},
            {"_sys_strcat", Env::Strcat, {cat_buf, str_abc}, cat_buf},
            {"_sys_strncpy", Env::Strncpy, {ncat_buf, str_abc, 4}, ncat_buf},
            {"_sys_strncat", Env::Strncat, {ncat_buf, str_abd, 2}, ncat_buf},
            {"_sys_strncmp", Env::Strncmp, {str_abc, str_abd, 3}, kMinusOne},
            {"_sys_memcmp", Env::Memcmp, {str_abc, str_abd, 3}, kMinusOne},
            {"_sys_memset", Env::Memset, {set_buf, 0x41, 4}, set_buf},
            {"_sys_memcpy", Env::Memcpy, {cpy_buf, str_abc, 4}, cpy_buf},
            {"_sys_snprintf", Env::Snprintf, {snp_buf, 16, fmt, 7}, 2},
            {"_sys_vsnprintf", Env::Vsnprintf, {vsn_buf, 16, fmt, va_list}, 2},
            {"_sys_process_atexitspawn", Env::AtExitSpawn, {}, 0},
            {"_sys_process_at_Exitspawn", Env::AtExitSpawnUpper, {}, 0},
            {"sys_ppu_thread_get_id", Env::GetThreadId, {tid_out}, 0},
            {"sys_time_get_system_time", Env::GetSystemTime, {}, std::nullopt},
            {"cellSysutilGetBgmPlaybackStatus", Env::GetBgmStatus, {bgm_out}, 0},
            {"cellVideoOutGetResolution", Env::VideoOutGetResolution, {2, res_out}, 0},
            {"cellVideoOutSetGamma", Env::VideoOutSetGamma, {0}, 0},
            {"cellRtcGetCurrentTick", Env::RtcGetCurrentTick, {utc_tick}, 0},
            {"cellRtcConvertUtcToLocalTime", Env::RtcConvertUtcToLocalTime, {utc_tick, local_tick}, 0},
    }};

    SplitModeScope mode(tox2::PpuThreadsMode::Split2);
    tox2::PpuSlots& slots = tox2::ppuSlots();
    std::atomic<bool> go{false};
    std::array<std::chrono::steady_clock::time_point, calls.size()> returned{};
    std::array<std::uint64_t, calls.size()> results{};
    std::thread holder = slotThread(1001, [&] {
        slots.acquire();
        CHECK(pollUntil([&] { return go.load(); }));
        for (std::size_t i = 0; i < calls.size(); i++) {
            const auto& a = calls[i].args;
            results[i] = gcm.call(*caller, calls[i].import, {a[0], a[1], a[2], a[3]});
            returned[i] = std::chrono::steady_clock::now();
        }
        slots.release();
    });
    std::chrono::steady_clock::time_point hold_end;
    {
        std::lock_guard<tox2::GuestLock> host(tox2::guestLock());
        go = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        hold_end = std::chrono::steady_clock::now();
    }
    holder.join();
    for (std::size_t i = 0; i < calls.size(); i++) {
        if (returned[i] >= hold_end) {
            // The calls run in order, so every call after the first one that waited returns late too.
            std::fprintf(stderr, "%s waited for the kernel lock\n", calls[i].name);
            CHECK(false);
            break;
        }
        if (calls[i].expected) CHECK(results[i] == *calls[i].expected);
    }
    tox2::setRtcLocalOffsetMinutes(std::nullopt);

    // What the handlers wrote.
    const auto text = [&](std::uint32_t addr) { return std::string(reinterpret_cast<const char*>(gcm.base() + addr)); };
    CHECK(tox2Read32(gcm.base(), out) == 8);
    CHECK(text(cat_buf) == "abcabc");
    CHECK(text(ncat_buf) == "abcab");
    CHECK(text(set_buf).substr(0, 4) == "AAAA");
    CHECK(text(cpy_buf) == "abc");
    CHECK(text(snp_buf) == "x7");
    CHECK(text(vsn_buf) == "x7");
    CHECK(tox2Read64(gcm.base(), tid_out) == 0x01000000);
    CHECK(tox2Read8(gcm.base(), bgm_out + 1) == 1);  // BGM disabled
    CHECK(tox2Read16(gcm.base(), res_out) == 1280 && tox2Read16(gcm.base(), res_out + 2) == 720);
    CHECK(tox2Read64(gcm.base(), local_tick) - tox2Read64(gcm.base(), utc_tick) ==
          std::uint64_t{kOffsetMinutes} * 60'000'000);
}

// _sys_memcpy copies exactly its range while another slot holder keeps rewriting the words on both sides of the
// destination; a copy that read and rewrote neighbouring bytes would lose a write or leak source bytes into them.
void testMemcpyNeighbourWrites() {
    HangGuard guard("testMemcpyNeighbourWrites");
    KernelFreeEnv env;
    auto copier = makeCaller(0x01000000);
    constexpr std::uint32_t kLength = 61;  // odd, so the copy has a head and a tail
    const std::uint32_t src = env.data() + 0x100, dst = env.data() + 0x200;
    const std::uint32_t before = dst - 4, after = dst + kLength;
    constexpr int kCopies = 200'000;

    SplitModeScope mode(tox2::PpuThreadsMode::Split2);
    tox2::PpuSlots& slots = tox2::ppuSlots();
    std::atomic<bool> copier_done{false}, writer_running{false};
    std::atomic<int> bad_copies{0}, bad_neighbours{0};
    std::vector<std::thread> threads;
    threads.push_back(slotThread(1001, [&] {
        slots.acquire();
        CHECK(pollUntil([&] { return writer_running.load(); }));
        for (int n = 0; n < kCopies; n++) {
            for (std::uint32_t i = 0; i < kLength; i++) env.base()[src + i] = static_cast<std::uint8_t>(n + i);
            if (env.call(*copier, KernelFreeEnv::Memcpy, {dst, src, kLength}) != dst) bad_copies++;
            if (std::memcmp(env.base() + dst, env.base() + src, kLength) != 0) bad_copies++;
        }
        copier_done = true;
        slots.release();
    }));
    threads.push_back(slotThread(1001, [&] {
        slots.acquire();
        writer_running = true;
        std::uint32_t value = 0;
        while (!copier_done.load()) {
            value = value * 1664525 + 1013904223;
            tox2Write32(env.base(), before, value);
            tox2Write32(env.base(), after, ~value);
            // Read back at once: a copy that stored stale bytes over a neighbour would show here.
            if (tox2Read32(env.base(), before) != value || tox2Read32(env.base(), after) != ~value) bad_neighbours++;
        }
        slots.release();
    }));
    joinAll(threads);
    CHECK(bad_copies == 0);
    CHECK(bad_neighbours == 0);
}

// cellGcmAddressToOffset on a mapping that stays in place keeps returning its offset while another slot holder maps
// and unmaps a second range, which rewrites the lock-free tables the lookup reads.
void testGcmAddressToOffsetConcurrent() {
    HangGuard guard("testGcmAddressToOffsetConcurrent");
    KernelFreeEnv gcm;
    auto reader = makeCaller(0x01000000);
    auto mapper = makeCaller(0x01000001);
    const std::uint32_t reader_out = gcm.data() + 0x100, mapper_out = gcm.data() + 0x200;
    const std::uint32_t second = gcm.area() + 0x100000;
    constexpr int kLookups = 1'000'000;

    SplitModeScope mode(tox2::PpuThreadsMode::Split2);
    tox2::PpuSlots& slots = tox2::ppuSlots();
    std::atomic<bool> reader_done{false};
    std::atomic<int> mapper_rounds{0}, failures{0};
    std::vector<std::thread> threads;
    threads.push_back(slotThread(1001, [&] {
        slots.acquire();
        for (int n = 0; n < kLookups; n++) {
            const std::uint64_t result =
                    gcm.call(*reader, KernelFreeEnv::AddressToOffset, {gcm.area() + 8, reader_out});
            if (result != 0 || tox2Read32(gcm.base(), reader_out) != 8) failures++;
            if (n == 0) CHECK(pollUntil([&] { return mapper_rounds.load() > 0; }));
        }
        reader_done = true;
        slots.release();
    }));
    threads.push_back(slotThread(1001, [&] {
        slots.acquire();
        while (!reader_done.load()) {
            if (gcm.call(*mapper, KernelFreeEnv::MapMain, {second, 0x100000, mapper_out}) != 0 ||
                tox2Read32(gcm.base(), mapper_out) != 0x100000) {
                failures++;
            }
            if (gcm.call(*mapper, KernelFreeEnv::UnmapEa, {second}) != 0) failures++;
            mapper_rounds++;
        }
        slots.release();
    }));
    joinAll(threads);
    CHECK(failures == 0);
    CHECK(mapper_rounds > 0);
}

std::atomic<std::uint64_t> g_sleep_us{0};
std::atomic<bool> g_sleep_hold_after{false};

// sys_timer_usleep(g_sleep_us) issued the way generated code does it, outside any kernel section. With
// g_sleep_hold_after set the thread then keeps computing without a yield or a syscall, so the sleep is its only release
// point before it ends.
TOX2_PPU_FUNC(sleepThenHoldEntry) {
    ctx->gpr[11] = 141;
    ctx->gpr[3] = g_sleep_us.load();
    tox2Syscall(ctx, base, 0x10000);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (g_sleep_hold_after.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
}

// sys_timer_usleep and sys_timer_sleep run without the kernel lock and give up the PPU slot while they sleep.
void testKernelFreeSleep() {
    HangGuard guard("testKernelFreeSleep");
    GuestThreadEnv env({sleepThenHoldEntry});
    auto caller = makeCaller(0x01000000);
    const tox2::SyscallEntry* usleep_entry = tox2::findSyscall(141);
    const tox2::SyscallEntry* sleep_entry = tox2::findSyscall(142);
    CHECK(usleep_entry != nullptr && usleep_entry->kernel_free);
    CHECK(sleep_entry != nullptr && sleep_entry->kernel_free);
    const auto now = [] { return std::chrono::steady_clock::now(); };
    const auto microsSince = [&](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::microseconds>(now() - t).count();
    };

    // A slot holder's sleep returns while a host thread holds the kernel lock; a handler that took it would return only
    // after the hold ended.
    {
        SplitModeScope mode(tox2::PpuThreadsMode::Split2);
        tox2::PpuSlots& slots = tox2::ppuSlots();
        std::atomic<bool> go{false};
        std::chrono::steady_clock::time_point started, returned, hold_end;
        std::thread holder = slotThread(1001, [&] {
            slots.acquire();
            CHECK(pollUntil([&] { return go.load(); }));
            started = now();
            CHECK(syscallAs(*caller, 141, 5000) == 0);
            returned = now();
            CHECK(slots.heldByCurrentThread());  // the slot came back
            slots.release();
        });
        {
            std::lock_guard<tox2::GuestLock> host(tox2::guestLock());
            go = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            hold_end = now();
        }
        holder.join();
        CHECK(returned < hold_end);
        CHECK(returned - started >= std::chrono::milliseconds(5));
    }

    // While a guest thread sleeps, a third thread gets its slot at once; the sleeper reads Blocked until it holds a
    // slot again, and it queues for that by its priority like any other waiter.
    {
        constexpr std::uint64_t kSleepUs = 300'000;
        SplitModeScope mode(tox2::PpuThreadsMode::Split2);
        tox2::PpuSlots& slots = tox2::ppuSlots();
        std::atomic<bool> a_held{false}, a_release{false}, c_go{false}, c_release{false};
        std::atomic<std::int64_t> c_wait_us{-1};
        std::atomic<int> d_saw_state{-1};
        std::vector<std::thread> threads;
        threads.push_back(slotThread(1001, [&] {  // A holds one slot throughout
            slots.acquire();
            a_held = true;
            pollUntil([&] { return a_release.load(); });
            slots.release();
        }));
        CHECK(pollUntil([&] { return a_held.load(); }));
        threads.push_back(slotThread(1001, [&] {  // C is the third thread
            pollUntil([&] { return c_go.load(); });
            const auto start = now();
            slots.acquire();
            c_wait_us = microsSince(start);
            pollUntil([&] { return c_release.load(); });
            slots.release();
        }));

        g_sleep_us = kSleepUs;
        g_sleep_hold_after = false;
        tox2::GuestThreadParams params;
        params.entry_opd = env.opd(0);
        params.priority = 1001;
        params.stack_size = 0x10000;
        params.name = "sleeper";
        const std::uint32_t sleeper_id = tox2::createGuestThread(params);
        const tox2::GuestThread* sleeper = tox2::findThread(sleeper_id);
        CHECK(sleeper != nullptr);
        if (sleeper == nullptr) {
            a_release = c_go = c_release = true;
            joinAll(threads);
            return;
        }
        CHECK(pollUntil([&] { return sleeper->state() == tox2::ThreadState::Blocked; }));
        CHECK(std::string_view(sleeper->blockedIn() != nullptr ? sleeper->blockedIn() : "") == "sys_timer_usleep");
        CHECK(!sleeper->indefiniteWait());  // a timed sleep is no stall

        c_go = true;
        CHECK(pollUntil([&] { return c_wait_us.load() >= 0; }));
        CHECK(c_wait_us.load() < 5000);
        CHECK(sleeper->state() == tox2::ThreadState::Blocked);  // still asleep: C did not wait for the wake-up

        // The sleeper wakes while A and C hold both slots and queues; a better thread queued after it is served first.
        CHECK(pollUntil([&] { return slots.waiterCount() == 1; }));
        queueSlotThread(slots, threads, 500, [&] {
            slots.acquire();
            d_saw_state = static_cast<int>(sleeper->state());
            slots.release();
        });
        a_release = true;
        CHECK(pollUntil([&] { return d_saw_state.load() >= 0; }));
        CHECK(d_saw_state.load() == static_cast<int>(tox2::ThreadState::Blocked));
        CHECK(pollUntil([&] { return sleeper->state() == tox2::ThreadState::Exited; }));
        c_release = true;
        joinAll(threads);
    }

    // The creator of a higher-priority thread waits for that thread's first release point. The new thread computes
    // after its sleep until released, so only the sleep can end the wait.
    {
        SplitModeScope mode(tox2::PpuThreadsMode::Split2);
        tox2::PpuSlots& slots = tox2::ppuSlots();
        tox2::GuestLock& kernel = tox2::guestLock();
        g_sleep_us = 1000;
        g_sleep_hold_after = true;
        std::atomic<std::uint32_t> thread_id{0};
        std::atomic<bool> create_ok{false}, creator_returned{false};
        std::thread creator = slotThread(1001, [&] {
            slots.acquire();
            kernel.lock();
            tox2::GuestThreadParams params;
            params.entry_opd = env.opd(0);
            params.priority = 500;
            params.stack_size = 0x10000;
            params.name = "sleeping child";
            tox2::GuestThread* thread = tox2::prepareGuestThread(params);
            if (thread != nullptr) {
                thread_id = thread->id();
                create_ok = tox2::startGuestThreadFirst(thread);
                creator_returned = true;
            }
            kernel.unlock();
            slots.release();
        });
        const auto deadline = now() + std::chrono::seconds(1);
        while (!creator_returned.load() && now() < deadline) std::this_thread::yield();
        const bool in_time = creator_returned.load();
        g_sleep_hold_after = false;  // lets the child end even when the creator did not return
        CHECK(in_time);
        creator.join();
        CHECK(create_ok);
        const tox2::GuestThread* child = tox2::findThread(thread_id);
        CHECK(child != nullptr && pollUntil([&] { return child->state() == tox2::ThreadState::Exited; }));
    }

    // Legacy: the sleep takes its time and lets other holders of the guest lock in meanwhile.
    {
        CHECK(!tox2::splitPpuMode());
        std::lock_guard<tox2::GuestLock> hold(tox2::guestLock());
        std::atomic<bool> host_got_lock{false};
        std::thread host([&] {
            std::lock_guard<tox2::GuestLock> lock(tox2::guestLock());
            host_got_lock = true;
        });
        CHECK(pollUntil([&] { return tox2::guestLock().waiterCount() > 0; }));
        const auto start = now();
        CHECK(syscallAs(*caller, 141, 5000) == 0);
        CHECK(microsSince(start) >= 5000);
        CHECK(host_got_lock.load());
        host.join();
    }
}

std::atomic<int> g_callee_kernel_held{-1};

TOX2_PPU_FUNC(kernelProbeCallee) {
    (void)ctx;
    (void)base;
    g_callee_kernel_held = tox2::guestLock().heldByCurrentThread() ? 1 : 0;
}

// A callback from a handler runs without the kernel lock and returns to the handler holding it again.
void testSplitGuestCallbackReleasesKernel() {
    HangGuard guard("testSplitGuestCallbackReleasesKernel");
    for (const tox2::PpuThreadsMode split : {tox2::PpuThreadsMode::Split1, tox2::PpuThreadsMode::Split2}) {
        BoundaryEnv env;
        std::uint8_t* base = env.base();
        const std::uint32_t cb = tox2::registerHostFunction("kernelProbeCallee", kernelProbeCallee);
        CHECK(cb != 0);
        constexpr const char* kModule = "cellSysutil";
        const Tox2ImportInfo imports[] = {
                {0x00800000, 0x9d98afa0, kModule, "cellSysutilRegisterCallback"},
                {0x00800010, 0x189a74da, kModule, "cellSysutilCheckCallback"},
        };
        tox2::installImports(imports, 2);
        auto caller = env.makeCallerWithStack(0x01000000, 0);
        Tox2PpuContext* ctx = &caller->context();

        SplitModeScope mode(split);
        bool direct_after = false, hle_after = true;
        int direct_inside = -1, hle_inside = -1;
        std::thread thread = slotThread(1001, [&] {
            tox2::ppuSlots().acquire();
            {
                // What a handler does: it runs in a kernel section (runImport opens it) and calls back.
                tox2::KernelSection section;
                g_callee_kernel_held = -1;
                tox2::callGuestFunction(ctx, base, cb, {});
                direct_inside = g_callee_kernel_held;
                direct_after = tox2::guestLock().heldByCurrentThread();
            }
            // The same through a real handler: cellSysutilCheckCallback runs the queued closure, which calls back.
            ctx->gpr[3] = 0;
            ctx->gpr[4] = cb;
            ctx->gpr[5] = 0;
            tox2HleCall(ctx, base, 0);
            {
                tox2::KernelSection section;
                CHECK(tox2::queueSysutilEvent(tox2::kSysutilRequestExitGame, 1) == 1);
            }
            g_callee_kernel_held = -1;
            tox2HleCall(ctx, base, 1);
            hle_inside = g_callee_kernel_held;
            hle_after = tox2::guestLock().heldByCurrentThread();
            tox2::ppuSlots().release();
        });
        thread.join();
        CHECK(direct_inside == 0 && direct_after);
        CHECK(hle_inside == 0 && !hle_after);
    }
}

std::atomic<std::int64_t> g_sysutil_stress_calls{0};

TOX2_PPU_FUNC(sysutilStressCallback) {
    (void)ctx;
    (void)base;
    g_sysutil_stress_calls++;
}

// Thread A runs the queued closures (each calls back into guest code, which releases the kernel lock) while thread B
// registers callbacks and queues events: every closure runs exactly once.
void testSplitSysutilCallbacks() {
    HangGuard guard("testSplitSysutilCallbacks");
    BoundaryEnv env;
    std::uint8_t* base = env.base();
    const std::uint32_t cb = tox2::registerHostFunction("sysutilStressCallback", sysutilStressCallback);
    CHECK(cb != 0);
    constexpr const char* kModule = "cellSysutil";
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x9d98afa0, kModule, "cellSysutilRegisterCallback"},
            {0x00800010, 0x189a74da, kModule, "cellSysutilCheckCallback"},
    };
    tox2::installImports(imports, 2);
    auto a = env.makeCallerWithStack(0x01000000, 0);
    auto b = env.makeCallerWithStack(0x01000001, 1);
    g_sysutil_stress_calls = 0;

    SplitModeScope mode(tox2::PpuThreadsMode::Split2);
    constexpr int kRounds = 3000;
    std::atomic<std::int64_t> queued{0};
    std::atomic<bool> b_done{false};
    const auto start = std::chrono::steady_clock::now();
    std::thread thread_a = slotThread(1001, [&] {
        tox2::ppuSlots().acquire();
        Tox2PpuContext* ctx = &a->context();
        // The check after B finished drains what B queued last.
        for (bool last = false; !last;) {
            last = b_done.load();
            tox2HleCall(ctx, base, 1);
        }
        tox2::ppuSlots().release();
    });
    std::thread thread_b = slotThread(1001, [&] {
        tox2::ppuSlots().acquire();
        Tox2PpuContext* ctx = &b->context();
        for (int i = 0; i < kRounds; i++) {
            ctx->gpr[3] = static_cast<std::uint32_t>(i) % 4;
            ctx->gpr[4] = cb;
            ctx->gpr[5] = static_cast<std::uint32_t>(i);
            tox2HleCall(ctx, base, 0);
            tox2::KernelSection section;  // queueSysutilEvent is called from handlers, which run in a section
            const std::int32_t count =
                    tox2::queueSysutilEvent(tox2::kSysutilRequestExitGame, static_cast<std::uint64_t>(i));
            if (count > 0) queued += count;
        }
        b_done = true;
        tox2::ppuSlots().release();
    });
    thread_b.join();
    thread_a.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(queued > 0);
    CHECK(g_sysutil_stress_calls == queued.load());
    CHECK(elapsed < std::chrono::seconds(10));
}

// Two slot holders call the same default-handled import: the shared call counter and the log limit stay exact.
void testSplitDefaultPathCount() {
    HangGuard guard("testSplitDefaultPathCount");
    BoundaryEnv env;
    std::uint8_t* base = env.base();
    std::atomic<int> default_lines{0};
    tox2::setLogSink([&](std::string_view line) {
        if (line.find("tox2: default hle cellTestModule::cellTestFirst(") != std::string_view::npos) default_lines++;
    });
    tox2::resetLogOnce();
    tox2::setQuietHle(false);
    const Tox2ImportInfo imports[] = {{0x00800000, 0xdeadbeef, "cellTestModule", "cellTestFirst"}};
    tox2::installImports(imports, 1);
    auto a = makeCaller(0x01000000);
    auto b = makeCaller(0x01000001);

    SplitModeScope mode(tox2::PpuThreadsMode::Split2);
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (tox2::GuestThread* caller : {a.get(), b.get()}) {
        threads.push_back(slotThread(1001, [&, caller] {
            tox2::ppuSlots().acquire();
            Tox2PpuContext* ctx = &caller->context();
            for (int n = 0; n < 10'000; n++) {
                ctx->gpr[3] = 0x55;
                ctx->gpr[4] = 0x66;
                tox2HleCall(ctx, base, 0);
                if (ctx->gpr[3] != 0) bad++;
            }
            tox2::ppuSlots().release();
        }));
    }
    joinAll(threads);
    CHECK(bad == 0);
    CHECK(default_lines == 8);
    CHECK(tox2::formatCensus().find("  cellTestModule::cellTestFirst: 20000 [default]") != std::string::npos);
    tox2::setLogSink({});
}

// Two slot holders call one implemented import and one syscall in parallel without the census mutex: both counts stay
// exact, and a reset makes the cached counters start again from zero.
void testCensusConcurrentCount() {
    HangGuard guard("testCensusConcurrentCount");
    constexpr int kCalls = 100'000;
    BoundaryEnv env;
    std::uint8_t* base = env.base();
    tox2::resetCensus();
    const Tox2ImportInfo imports[] = {{0x00800000, 0x8461e528, "sysPrxForUser", "sys_time_get_system_time"}};
    tox2::installImports(imports, 1);
    auto a = makeCaller(0x01000000);
    auto b = makeCaller(0x01000001);

    const auto count_line = [](std::string_view prefix, std::uint64_t count) {
        return tox2::formatCensus().find(std::format("{}: {} [impl]", prefix, count)) != std::string::npos;
    };
    SplitModeScope mode(tox2::PpuThreadsMode::Split2);
    std::vector<std::thread> threads;
    for (tox2::GuestThread* caller : {a.get(), b.get()}) {
        threads.push_back(slotThread(1001, [&, caller] {
            tox2::ppuSlots().acquire();
            Tox2PpuContext* ctx = &caller->context();
            for (int n = 0; n < kCalls; n++) {
                tox2HleCall(ctx, base, 0);
                ctx->gpr[11] = 147;
                tox2Syscall(ctx, base, 0x10000);
            }
            tox2::ppuSlots().release();
        }));
    }
    joinAll(threads);
    CHECK(count_line("sysPrxForUser::sys_time_get_system_time", 2 * kCalls));
    CHECK(count_line("sc 147 sys_time_get_timebase_frequency", 2 * kCalls));

    tox2::resetCensus();
    Tox2PpuContext* ctx = &a->context();
    for (int n = 0; n < 5; n++) {
        tox2HleCall(ctx, base, 0);
        ctx->gpr[11] = 147;
        tox2Syscall(ctx, base, 0x10000);
    }
    CHECK(count_line("sysPrxForUser::sys_time_get_system_time", 5));
    CHECK(count_line("sc 147 sys_time_get_timebase_frequency", 5));
    tox2::resetCensus();
}

TOX2_PPU_FUNC(hostRegistryProbe) {
    (void)ctx;
    (void)base;
}

// One thread registers host functions while another resolves the newest published key: the lock-free lookup never
// misses an entry whose OPD was handed out.
void testHostFunctionConcurrentLookup() {
    HangGuard guard("testHostFunctionConcurrentLookup");
    BoundaryEnv env;
    std::uint8_t* base = env.base();
    constexpr std::uint32_t kCount = 1000;

    SplitModeScope mode(tox2::PpuThreadsMode::Split2);
    std::atomic<std::uint32_t> latest_opd{0};
    std::atomic<bool> done{false};
    std::atomic<std::int64_t> lookups{0}, misses{0};
    std::thread registrar = slotThread(1001, [&] {
        for (std::uint32_t i = 0; i < kCount; i++) {
            latest_opd.store(tox2::registerHostFunction("hostRegistryProbe", hostRegistryProbe));
        }
        done = true;
    });
    std::thread reader = slotThread(1001, [&] {
        for (bool last = false; !last;) {
            last = done.load();
            const std::uint32_t opd = latest_opd.load();
            if (opd == 0) continue;
            const std::uint32_t key = tox2Read32(base, opd);
            lookups++;
            if (tox2Lookup(key) != hostRegistryProbe) misses++;
        }
    });
    registrar.join();
    reader.join();
    CHECK(lookups > 0);
    CHECK(misses == 0);
    for (std::uint32_t i = 0; i < kCount; i++) {
        CHECK(tox2Lookup(tox2::kHostFunctionBase + 4 * i) == hostRegistryProbe);
    }
    CHECK(tox2Lookup(tox2::kHostFunctionBase + 4 * kCount) == nullptr);
}

// The interrupt thread takes the kernel lock itself, so the same scenario must hold in every mode.
void testGcmInterruptThread() {
    HangGuard guard("testGcmInterruptThread");
    gcmInterruptThreadCase();
    for (const tox2::PpuThreadsMode split : {tox2::PpuThreadsMode::Split1, tox2::PpuThreadsMode::Split2}) {
        SplitModeScope mode(split);
        gcmInterruptThreadCase();
    }
}

std::atomic<std::int64_t> g_start_call_ns{0};  // first handler / service call, steady clock
std::atomic<int> g_start_calls{0};
std::atomic<std::uint32_t> g_start_thread_id{0};

std::int64_t steadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

void recordStartCall() {
    std::int64_t expected = 0;
    g_start_call_ns.compare_exchange_strong(expected, steadyNs());
    g_start_calls++;
}

TOX2_PPU_FUNC(startUserHandler) {
    (void)ctx;
    (void)base;
    recordStartCall();
}

// Split2 with the second slot free: the new thread runs at once but waits for the starter's kernel lock, so its first
// read of the state follows the starter's writes and the handler is not called before the lock is released.
void testSplitGcmInterruptStart() {
    HangGuard guard("testSplitGcmInterruptStart");
    GuestThreadEnv env({startUserHandler}, true);
    tox2::resetRsxState();
    SplitModeScope mode(tox2::PpuThreadsMode::Split2);
    g_start_call_ns = 0;
    g_start_calls = 0;
    tox2::rsxState().user_handler = env.opd(0);

    std::int64_t released_ns = 0;
    std::uint32_t id = 0;
    bool alive_while_held = false, alive_after = false;
    std::uint64_t user_calls = 0;
    std::thread starter = slotThread(1001, [&] {
        tox2::PpuSlots& slots = tox2::ppuSlots();
        tox2::GuestLock& kernel = tox2::guestLock();
        slots.acquire();
        kernel.lock();
        id = tox2::startGcmInterruptThread();
        tox2::postGcmInterrupt(tox2::RsxInterrupt::User, 7);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const tox2::GuestThread* thread = tox2::findThread(id);
        alive_while_held = thread != nullptr && thread->state() != tox2::ThreadState::Exited;
        released_ns = steadyNs();
        kernel.unlock();
        CHECK(pollUntil([] { return g_start_calls.load() >= 1; }));
        alive_after = thread != nullptr && thread->state() != tox2::ThreadState::Exited;
        user_calls = tox2::gcmInterruptCounts().user;  // stopGcmInterruptThread zeroes the counters
        kernel.lock();
        tox2::stopGcmInterruptThread();
        kernel.unlock();
        slots.release();
    });
    starter.join();
    CHECK(id != 0);
    CHECK(alive_while_held && alive_after);
    CHECK(user_calls == 1 && g_start_calls == 1);
    CHECK(g_start_call_ns.load() >= released_ns);
    tox2RegisterFuncs(nullptr, 0);
    tox2::resetHostFunctions();
    tox2::resetRsxState();
}

// The same for the sail service: the queued call runs exactly once, after the starter released the kernel lock.
void testSplitSailServiceStart() {
    HangGuard guard("testSplitSailServiceStart");
    GuestThreadEnv env({startUserHandler}, true);
    SplitModeScope mode(tox2::PpuThreadsMode::Split2);
    g_start_call_ns = 0;
    g_start_calls = 0;
    g_start_thread_id = 0;

    std::int64_t released_ns = 0;
    bool queued = false, alive_after = false;
    std::thread starter = slotThread(1001, [&] {
        tox2::PpuSlots& slots = tox2::ppuSlots();
        tox2::GuestLock& kernel = tox2::guestLock();
        slots.acquire();
        kernel.lock();
        queued = tox2::queueSailCall([](Tox2PpuContext*, std::uint8_t*) {
            const tox2::GuestThread* thread = tox2::currentThread();
            g_start_thread_id = thread != nullptr ? thread->id() : 0;
            recordStartCall();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        released_ns = steadyNs();
        kernel.unlock();
        CHECK(pollUntil([] { return g_start_calls.load() >= 1; }));
        const tox2::GuestThread* thread = tox2::findThread(g_start_thread_id.load());
        alive_after = thread != nullptr && thread->state() != tox2::ThreadState::Exited;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));  // a second run would show up here
        kernel.lock();
        tox2::stopSailService();
        kernel.unlock();
        slots.release();
    });
    starter.join();
    CHECK(queued && alive_after);
    CHECK(g_start_calls == 1);
    CHECK(g_start_call_ns.load() >= released_ns);
    tox2::resetHostFunctions();
}

// Uncontended cost of one dispatcher call: legacy against Split1, for a kernel entry and a kernel-free one.
void testSplitBoundaryCost() {
    constexpr int kRuns = 5;
    constexpr int kCalls = 300'000;
    GuestThreadEnv env({boundaryDummyEntry});
    auto caller = makeCaller(0x01000000);
    const auto measure = [&](std::uint32_t number) {
        std::array<double, kRuns> ns{};
        for (double& run : ns) {
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < kCalls; i++) syscallAs(*caller, number);
            const auto elapsed = std::chrono::steady_clock::now() - start;
            run = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) / kCalls;
        }
        std::ranges::sort(ns);
        return ns[kRuns / 2];
    };
    // sys_process_getpid takes the kernel lock in the split modes; sys_time_get_timebase_frequency does not.
    const double legacy_kernel = measure(1), legacy_free = measure(147);
    double split_kernel = 0, split_free = 0;
    {
        SplitModeScope mode(tox2::PpuThreadsMode::Split1);
        tox2::ppuSlots().acquire();
        split_kernel = measure(1);
        split_free = measure(147);
        tox2::ppuSlots().release();
    }
    std::printf(
            "syscall dispatch uncontended: kernel entry legacy %.0f ns, split %.0f ns (+%.0f); kernel-free legacy "
            "%.0f ns, split %.0f ns (+%.0f)\n",
            legacy_kernel, split_kernel, split_kernel - legacy_kernel, legacy_free, split_free,
            split_free - legacy_free);
}

// Rapid yields reach the backoff and let another host thread take the lock; spaced yields never do.
void testYieldBackoff() {
    tox2::setYieldBackoffEnabled(true);
    tox2::GuestLock& lock = tox2::guestLock();

    // The other thread queues only once the first backoff sleep is counted, so a yield that handed the lock to it
    // without the sleep would be a failure of the test.
    {
        const std::uint64_t before = tox2::yieldBackoffCount();
        std::atomic<bool> other_ran{false};
        std::thread spinner([&] {
            lock.lock();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!other_ran && std::chrono::steady_clock::now() < deadline) tox2::yieldPpuThread();
            lock.unlock();
        });
        std::thread other([&] {
            while (tox2::yieldBackoffCount() == before) std::this_thread::yield();
            lock.lock();
            other_ran = true;
            lock.unlock();
        });
        spinner.join();
        other.join();
        CHECK(other_ran);
        CHECK(tox2::yieldBackoffCount() > before);
    }

    // Fewer yields than the threshold in a burst never sleep.
    {
        const std::uint64_t before = tox2::yieldBackoffCount();
        std::thread t([&] {
            lock.lock();
            for (unsigned i = 0; i < tox2::kYieldSpinThreshold - 1; i++) tox2::yieldPpuThread();
            lock.unlock();
        });
        t.join();
        CHECK(tox2::yieldBackoffCount() == before);
    }

    // Yields spaced further apart than kYieldSpinGap reset the count.
    {
        const std::uint64_t before = tox2::yieldBackoffCount();
        std::thread t([&] {
            lock.lock();
            for (unsigned i = 0; i < 3 * tox2::kYieldSpinThreshold; i++) {
                const auto until = std::chrono::steady_clock::now() + 2 * tox2::kYieldSpinGap;
                while (std::chrono::steady_clock::now() < until) {
                }
                CHECK(!tox2::yieldPpuThread());  // nobody waits
            }
            lock.unlock();
        });
        t.join();
        CHECK(tox2::yieldBackoffCount() == before);
    }

    // Disabled: any number of rapid yields is the plain yield.
    {
        tox2::setYieldBackoffEnabled(false);
        const std::uint64_t before = tox2::yieldBackoffCount();
        std::thread t([&] {
            lock.lock();
            for (unsigned i = 0; i < 4 * tox2::kYieldSpinThreshold; i++) tox2::yieldPpuThread();
            lock.unlock();
        });
        t.join();
        CHECK(tox2::yieldBackoffCount() == before);
        tox2::setYieldBackoffEnabled(true);
    }
}

// The yield outcome counters: a yield with nobody queued is kept, one that let a queued PPU thread run is switched.
void testPpuYieldStats() {
    tox2::setYieldBackoffEnabled(true);
    tox2::GuestLock& lock = tox2::guestLock();
    const tox2::PpuYieldStats before = tox2::ppuYieldStats();

    std::thread holder([&] {
        lock.lock();
        for (int i = 0; i < 3; i++) {
            // Spaced beyond kYieldSpinGap, so none of them counts toward the backoff.
            const auto until = std::chrono::steady_clock::now() + 2 * tox2::kYieldSpinGap;
            while (std::chrono::steady_clock::now() < until) {
            }
            CHECK(!tox2::yieldPpuThread());
        }
        lock.unlock();
    });
    holder.join();
    const tox2::PpuYieldStats kept = tox2::ppuYieldStats();
    CHECK(kept.calls - before.calls == 3);
    CHECK(kept.kept - before.kept == 3);
    CHECK(kept.switched - before.switched == 0);
    CHECK(kept.to_host - before.to_host == 0);
    CHECK(kept.backoffs - before.backoffs == 0);

    std::atomic<bool> waiter_ran{false};
    std::thread waiter;
    std::thread yielder([&] {
        lock.lock();
        waiter = std::thread([&] {
            // A PPU waiter: only a PPU thread makes the yield a switch. This host-class yielder counts every one as
            // eligible.
            tox2::setGuestLockPpuPriorityForTesting(1001);
            lock.lock();
            waiter_ran = true;
            lock.unlock();
            tox2::setGuestLockPpuPriorityForTesting(std::nullopt);
        });
        while (lock.waiterCount() == 0) std::this_thread::yield();
        CHECK(tox2::yieldPpuThread());
        CHECK(waiter_ran);
        lock.unlock();
    });
    yielder.join();
    waiter.join();
    const tox2::PpuYieldStats switched = tox2::ppuYieldStats();
    CHECK(switched.calls - kept.calls == 1);
    CHECK(switched.switched - kept.switched == 1);
    CHECK(switched.kept - kept.kept == 0);
}

// sys_ppu_thread_yield returns CELL_OK only when a PPU thread of the same or a higher priority was queued; queued
// host-class waiters are served either way, lower-priority PPU threads are not.
void testPpuYieldSemantics() {
    using tox2::GuestLockClass;
    using Priority = std::optional<std::int32_t>;
    tox2::setYieldBackoffEnabled(true);
    tox2::GuestLock& lock = tox2::guestLock();

    struct Spec {
        Priority priority;  // nullopt: a host-class waiter of class `cls`
        GuestLockClass cls;
    };
    struct Outcome {
        std::vector<bool> results;
        std::vector<unsigned> ran_masks;  // bit i: waiter i had run when the yield returned
        unsigned ran_final = 0;
        std::uint64_t releases_before = 0, releases_after = 0;
    };

    // A PPU yielder (test priority 1001) holds the global lock while the waiters queue one at a time, then yields
    // `yields` times, each spaced by 2 x kYieldSpinGap so the backoff never triggers.
    const auto run = [&](const std::vector<Spec>& specs, int yields, bool via_syscall) {
        Outcome out;
        std::array<std::atomic<bool>, 4> ran{};
        auto caller = makeCaller(0x01000100);
        std::atomic<bool> held{false}, go{false};
        std::thread yielder([&] {
            tox2::setGuestLockPpuPriorityForTesting(1001);
            lock.lock();
            held = true;
            while (!go) std::this_thread::yield();
            out.releases_before = lock.releaseCount();
            for (int i = 0; i < yields; i++) {
                const auto until = std::chrono::steady_clock::now() + 2 * tox2::kYieldSpinGap;
                while (std::chrono::steady_clock::now() < until) {
                }
                const bool switched = via_syscall ? syscallAs(*caller, 43) == tox2::kCellOk : tox2::yieldPpuThread();
                out.results.push_back(switched);
                unsigned mask = 0;
                for (std::size_t w = 0; w < specs.size(); w++) mask |= ran[w] ? 1u << w : 0u;
                out.ran_masks.push_back(mask);
            }
            out.releases_after = lock.releaseCount();
            lock.unlock();
            tox2::setGuestLockPpuPriorityForTesting(std::nullopt);
        });
        while (!held) std::this_thread::yield();
        std::vector<std::thread> waiters;
        for (std::size_t w = 0; w < specs.size(); w++) {
            const std::size_t queued = lock.waiterCount();
            waiters.emplace_back([&, w] {
                if (specs[w].priority.has_value()) {
                    tox2::setGuestLockPpuPriorityForTesting(specs[w].priority);
                } else {
                    tox2::setGuestLockClass(specs[w].cls);
                }
                lock.lock();
                ran[w] = true;
                lock.unlock();
                tox2::setGuestLockPpuPriorityForTesting(std::nullopt);
            });
            while (lock.waiterCount() <= queued) std::this_thread::yield();
        }
        go = true;
        yielder.join();
        for (std::thread& t : waiters) t.join();
        for (std::size_t w = 0; w < specs.size(); w++) out.ran_final |= ran[w] ? 1u << w : 0u;
        return out;
    };

    const tox2::PpuYieldStats before = tox2::ppuYieldStats();

    // (a) An equal-priority PPU thread: the yield switches, and it ran before the yield returned.
    {
        const Outcome o = run({{1001, GuestLockClass::Other}}, 1, false);
        CHECK(o.results == std::vector<bool>({true}));
        CHECK(o.ran_masks == std::vector<unsigned>({1u}));
    }
    // (b) A higher-priority one.
    {
        const Outcome o = run({{500, GuestLockClass::Other}}, 1, false);
        CHECK(o.results == std::vector<bool>({true}));
        CHECK(o.ran_masks == std::vector<unsigned>({1u}));
    }
    // (c) Only a lower-priority PPU thread is queued: every yield keeps the lock and does not release it.
    {
        const Outcome o = run({{1005, GuestLockClass::Other}}, 10, false);
        CHECK(o.results == std::vector<bool>(10, false));
        CHECK(o.ran_masks == std::vector<unsigned>(10, 0u));
        CHECK(o.releases_after == o.releases_before);
        CHECK(o.ran_final == 1u);  // it runs once the yielder unlocks
    }
    // (d) A lower-priority PPU thread and then a host-class waiter: the host waiter runs, the PPU thread does not.
    {
        const Outcome o = run({{1005, GuestLockClass::Other}, {std::nullopt, GuestLockClass::Rsx}}, 1, false);
        CHECK(o.results == std::vector<bool>({false}));
        CHECK(o.ran_masks == std::vector<unsigned>({2u}));
        CHECK(o.ran_final == 3u);
    }
    // (f) The counters.
    const tox2::PpuYieldStats after = tox2::ppuYieldStats();
    CHECK(after.calls - before.calls == 13);
    CHECK(after.switched - before.switched == 2);
    CHECK(after.kept - before.kept == 10);
    CHECK(after.to_host - before.to_host == 1);
    CHECK(after.backoffs - before.backoffs == 0);

    // (e) The syscall returns the same result: CELL_CANCEL (1) in case (c), CELL_OK (0) in case (a).
    {
        const Outcome o = run({{1005, GuestLockClass::Other}}, 1, true);
        CHECK(o.results == std::vector<bool>({false}));
    }
    {
        const Outcome o = run({{1001, GuestLockClass::Other}}, 1, true);
        CHECK(o.results == std::vector<bool>({true}));
    }
    CHECK(lock.waiterCount() == 0);
}

void testTimebase() {
    auto ctx = std::make_unique<Tox2PpuContext>();
    LARGE_INTEGER frequency, t0, t1;
    QueryPerformanceFrequency(&frequency);

    const std::uint64_t first = tox2Mftb(ctx.get());
    QueryPerformanceCounter(&t0);
    const std::uint64_t tb0 = tox2Mftb(ctx.get());
    Sleep(100);
    const std::uint64_t tb1 = tox2Mftb(ctx.get());
    QueryPerformanceCounter(&t1);

    CHECK(tb0 >= first);
    CHECK(tb1 > tb0);
    // The rate is measured against the host clock over the same interval, so Sleep jitter cancels.
    const double seconds = static_cast<double>(t1.QuadPart - t0.QuadPart) / frequency.QuadPart;
    const double rate = static_cast<double>(tb1 - tb0) / seconds;
    const bool rate_ok = rate > 79800000.0 * 0.95 && rate < 79800000.0 * 1.05;
    CHECK(rate_ok);
    if (!rate_ok) std::fprintf(stderr, "timebase rate %.0f Hz\n", rate);
    CHECK(tox2::kTimebaseFrequency == 79800000);

    const std::uint64_t us0 = tox2::systemTimeUs();
    Sleep(20);
    CHECK(tox2::systemTimeUs() >= us0 + 15000);
}

void testTimerResolution() {
    const tox2::TimerResolution resolution;
    CHECK(resolution.active());
    CHECK(resolution.periodMs() >= 1);
    // Reported in whole microseconds; an unreadable period only skips the check.
    const auto period_us = tox2::currentTimerPeriodUs();
    if (period_us) {
        CHECK(*period_us <= 1000 * resolution.periodMs());
    }

    // The median over a few waits tolerates a single descheduled sample.
    std::vector<double> ms;
    for (int i = 0; i < 21; ++i) {
        const auto start = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(ms.begin(), ms.end());
    const bool fast = ms[ms.size() / 2] < 5.0;
    CHECK(fast);
    if (!fast) std::fprintf(stderr, "1 ms sleep median %.2f ms\n", ms[ms.size() / 2]);
}

// A fresh thread per call: the priority belongs to the thread, and the test thread must stay at normal.
int appliedPriority(tox2::HostThreadRole role) {
    int priority = -100;
    std::thread([&] { priority = tox2::applyHostThreadPriority(role); }).join();
    return priority;
}

void testHostPriority() {
    CHECK(tox2::hostPrioritiesEnabled());
    CHECK(appliedPriority(tox2::HostThreadRole::AudioEngine) == THREAD_PRIORITY_TIME_CRITICAL);
    CHECK(appliedPriority(tox2::HostThreadRole::HighPrioritySpu) == THREAD_PRIORITY_HIGHEST);

    tox2::setHostPrioritiesEnabled(false);
    CHECK(!tox2::hostPrioritiesEnabled());
    CHECK(appliedPriority(tox2::HostThreadRole::AudioEngine) == THREAD_PRIORITY_NORMAL);
    CHECK(appliedPriority(tox2::HostThreadRole::HighPrioritySpu) == THREAD_PRIORITY_NORMAL);
    tox2::setHostPrioritiesEnabled(true);
    CHECK(GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_NORMAL);
}

void testReservationAndDcbz() {
    std::unique_ptr<tox2::GuestMemory> memory(newMemory());
    std::uint8_t* base = memory->base();
    CHECK(memory->commit(0x20000, 0x1000));
    auto ctx = std::make_unique<Tox2PpuContext>();

    tox2Write32(base, 0x20010, 5);
    CHECK(tox2Lwarx(ctx.get(), base, 0x20010) == 5);
    const std::uint64_t epoch = tox2::reservations().storeEpoch();
    CHECK(tox2Stwcx(ctx.get(), base, 0x20010, 6));
    CHECK(tox2Read32(base, 0x20010) == 6);
    CHECK(tox2::reservations().storeEpoch() != epoch);  // goes through the table that SPU atomics use
    CHECK(!tox2Stwcx(ctx.get(), base, 0x20010, 7));     // the reservation is gone
    CHECK(tox2Read32(base, 0x20010) == 6);

    tox2Lwarx(ctx.get(), base, 0x20010);
    tox2Write32(base, 0x20010, 9);  // another thread changed the value
    CHECK(!tox2Stwcx(ctx.get(), base, 0x20010, 7));
    CHECK(tox2Read32(base, 0x20010) == 9);

    tox2Lwarx(ctx.get(), base, 0x20010);
    CHECK(!tox2Stwcx(ctx.get(), base, 0x20014, 7));  // different address
    CHECK(!tox2Stwcx(ctx.get(), base, 0x20010, 7));  // the failure also cleared it

    tox2Write64(base, 0x20020, 0x1122334455667788ull);
    CHECK(tox2Ldarx(ctx.get(), base, 0x20020) == 0x1122334455667788ull);
    CHECK(tox2Stdcx(ctx.get(), base, 0x20020, 3));
    CHECK(tox2Read64(base, 0x20020) == 3);
    CHECK(!tox2Stdcx(ctx.get(), base, 0x20020, 4));

    std::memset(base + 0x20000, 0xAB, 0x300);
    tox2Dcbz(ctx.get(), base, 0x20085);  // any address inside the line
    CHECK(base[0x2007f] == 0xAB);
    CHECK(base[0x20100] == 0xAB);
    bool line_zero = true;
    for (int i = 0x80; i < 0x100; i++) line_zero = line_zero && base[0x20000 + i] == 0;
    CHECK(line_zero);

    ctx->fpscr = 0;
    tox2Mtfsf(ctx.get(), 0x80, 0xF0000000u);
    CHECK(tox2Mffs(ctx.get()) == 0xF0000000u);
    tox2Mtfsb(ctx.get(), 31, 1);
    CHECK(tox2Mffs(ctx.get()) == 0xF0000001u);
    tox2Mtfsb(ctx.get(), 0, 0);
    CHECK(tox2Mffs(ctx.get()) == 0x70000001u);
}

// The watch counts guest-side accesses to a live sys_lwmutex_t. Every kind is checked as a delta, because the
// counters are process-wide.
void testLwmutexWatchHooks() {
    using Kind = tox2::LwmutexWatchKind;
    std::unique_ptr<tox2::GuestMemory> memory(newMemory());
    std::uint8_t* base = memory->base();
    CHECK(memory->commit(0x20000, 0x1000));
    auto ctx = std::make_unique<Tox2PpuContext>();
    auto table = std::make_unique<tox2::ReservationTable>();
    constexpr std::uint32_t kLwmutex = 0x20100;  // [0x20100, 0x20118)
    constexpr std::uint32_t kOther = 0x20400;    // no struct

    std::vector<std::string> log_lines;
    tox2::setLogSink([&](std::string_view line) { log_lines.emplace_back(line); });

    auto snapshot = [] { return tox2::lwmutexWatchStats(); };
    tox2::LwmutexWatchStats last = snapshot();
    // Returns the per-kind hit growth since the previous call.
    auto growth = [&] {
        const tox2::LwmutexWatchStats now = snapshot();
        std::array<std::uint64_t, tox2::kLwmutexWatchKindCount> delta{};
        for (std::size_t i = 0; i < delta.size(); i++) delta[i] = now.hits[i] - last.hits[i];
        last = now;
        return delta;
    };
    auto only = [](const std::array<std::uint64_t, tox2::kLwmutexWatchKindCount>& delta, Kind kind) {
        for (std::size_t i = 0; i < delta.size(); i++) {
            if (delta[i] != (i == static_cast<std::size_t>(kind) ? 1u : 0u)) return false;
        }
        return true;
    };
    auto none = [](const std::array<std::uint64_t, tox2::kLwmutexWatchKindCount>& delta) {
        for (const std::uint64_t value : delta) {
            if (value != 0) return false;
        }
        return true;
    };

    // Runs the seven PPU and SPU accesses on the line at `line` (the struct, when registered, sits at its start).
    // `hits` receives the delta of each call in this order.
    auto exercise = [&](std::uint32_t line, std::array<std::uint64_t, tox2::kLwmutexWatchKindCount>(&hits)[8]) {
        alignas(16) std::uint8_t data[tox2::kReservationLineSize];
        tox2::Reservation res;
        tox2Lwarx(ctx.get(), base, line);
        hits[0] = growth();
        tox2Stwcx(ctx.get(), base, line, 1);
        hits[1] = growth();
        tox2Dcbz(ctx.get(), base, line + 4);
        hits[2] = growth();
        table->getllar(base, line + 0x40, data, res);
        hits[3] = growth();
        data[0] ^= 1;  // chunk 0 overlaps the struct
        table->putllc(base, line, data, res);
        hits[4] = growth();
        table->putlluc(base, line, data);
        hits[5] = growth();
        table->put(base, line + 4, data, 4);
        hits[6] = growth();
        // The docs/spu.md case: an SPU atomic on a chunk of the line that is outside the struct.
        table->getllar(base, line, data, res);
        growth();
        data[0x40] ^= 1;
        table->putllc(base, line, data, res);
        hits[7] = growth();
    };

    std::array<std::uint64_t, tox2::kLwmutexWatchKindCount> hits[8];

    tox2::setLwmutexWatchEnabled(true);
    tox2::lwmutexWatchAdd(kLwmutex);
    tox2::lwmutexWatchAdd(kLwmutex);  // a second add of a live address is ignored
    CHECK(tox2::lwmutexWatchStats().live >= 1);
    const std::uint64_t live_with = tox2::lwmutexWatchStats().live;
    growth();

    exercise(kLwmutex, hits);
    CHECK(only(hits[0], Kind::Lwarx));
    CHECK(only(hits[1], Kind::Stwcx));
    CHECK(only(hits[2], Kind::Dcbz));
    CHECK(only(hits[3], Kind::SpuGetllar));
    CHECK(only(hits[4], Kind::SpuPutllc));  // the line changed, so the PUTLLC reached the chunk store
    CHECK(only(hits[5], Kind::SpuPutlluc));
    CHECK(only(hits[6], Kind::SpuPut));
    CHECK(none(hits[7]));  // the changed chunk is outside the struct
    CHECK(!log_lines.empty());

    // A neighbouring word is not a hit: the struct ends at +24.
    tox2Lwarx(ctx.get(), base, kLwmutex + 24);
    CHECK(none(growth()));
    tox2Lwarx(ctx.get(), base, kLwmutex + 20);  // the last word of the struct
    CHECK(only(growth(), Kind::Lwarx));

    // Direct use of the value-check entry points.
    tox2::lwmutexWatchHit(Kind::OwnerValue, kLwmutex, 4, 0);
    CHECK(only(growth(), Kind::OwnerValue));
    CHECK(!tox2::lwmutexWatchCheck(Kind::HleCopy, kOther, 16, 0));
    CHECK(tox2::lwmutexWatchCheck(Kind::HleCopy, kLwmutex + 16, 16, 0));
    CHECK(only(growth(), Kind::HleCopy));

    exercise(kOther, hits);
    for (const auto& delta : hits) CHECK(none(delta));

    tox2::lwmutexWatchRemove(kLwmutex);
    CHECK(tox2::lwmutexWatchStats().live == live_with - 1);
    CHECK(tox2::lwmutexWatchStats().max_live >= live_with);
    exercise(kLwmutex, hits);
    for (const auto& delta : hits) CHECK(none(delta));

    tox2::lwmutexWatchAdd(kLwmutex);
    tox2::setLwmutexWatchEnabled(false);
    exercise(kLwmutex, hits);
    for (const auto& delta : hits) CHECK(none(delta));
    tox2::lwmutexWatchRemove(kLwmutex);

    tox2::setLogSink({});
}

// With the watch on, the lwmutex calls register their struct, count an owner word that is no lwmutex state or thread
// id, and the HLE copies count a destination that overlaps a live struct.
void testLwmutexWatchRegistration() {
    using Kind = tox2::LwmutexWatchKind;
    tox2::setLogSink([](std::string_view) {});
    tox2::resetCensus();
    tox2::resetSyncObjects();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t lw = scratch, attr = scratch + 0x40;

    constexpr const char* kModule = "sysPrxForUser";
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0x2f85c0ef, kModule, "sys_lwmutex_create"},
            {0x00800010, 0xc3476d0c, kModule, "sys_lwmutex_destroy"},
            {0x00800030, 0xaeb78725, kModule, "sys_lwmutex_trylock"},
            {0x00800040, 0x1bc200f4, kModule, "sys_lwmutex_unlock"},
            {0x00800070, 0x68b9b011, kModule, "_sys_memset"},
    };
    tox2::installImports(imports, 5);
    enum { kCreate, kDestroy, kTryLock, kUnlock, kMemset };
    auto caller = makeCaller(0x01000000);
    auto call = [&](std::uint32_t index, std::uint64_t r3, std::uint64_t r4 = 0, std::uint64_t r5 = 0) {
        Tox2PpuContext* ctx = &caller->context();
        ctx->gpr[3] = r3;
        ctx->gpr[4] = r4;
        ctx->gpr[5] = r5;
        tox2HleCall(ctx, base, index);
        return ctx->gpr[3];
    };
    auto hits = [](Kind kind) { return tox2::lwmutexWatchStats().hits[static_cast<std::size_t>(kind)]; };

    // Off: nothing is registered and nothing counted.
    tox2Write32(base, attr, 2);
    tox2Write32(base, attr + 4, 0x10);
    const std::uint64_t live_before = tox2::lwmutexWatchStats().live;
    CHECK(call(kCreate, lw, attr) == 0);
    CHECK(tox2::lwmutexWatchStats().live == live_before);
    CHECK(call(kDestroy, lw) == 0);

    tox2::setLwmutexWatchEnabled(true);
    CHECK(call(kCreate, lw, attr) == 0);
    CHECK(tox2::lwmutexWatchStats().live == live_before + 1);

    // Writing the pad word through the HLE copy is a hit; the lwmutex calls themselves are not.
    const std::uint64_t copy_before = hits(Kind::HleCopy);
    CHECK(call(kMemset, lw + 20, 0, 4) == lw + 20);
    CHECK(hits(Kind::HleCopy) == copy_before + 1);
    CHECK(call(kMemset, lw + 24, 0, 4) == lw + 24);  // the word after the struct
    CHECK(hits(Kind::HleCopy) == copy_before + 1);

    // Lwmutex states and thread ids are valid owner words; anything else is a hit.
    const std::uint64_t owner_before = hits(Kind::OwnerValue);
    CHECK(call(kTryLock, lw) == 0);
    CHECK(call(kTryLock, lw) == 0);  // recursion step on the thread's own id
    CHECK(call(kUnlock, lw) == 0);
    CHECK(call(kUnlock, lw) == 0);
    CHECK(tox2Read32(base, lw) == tox2::kLwmutexFree);
    CHECK(hits(Kind::OwnerValue) == owner_before);
    tox2Write32(base, lw, 0x12345678);
    CHECK(call(kTryLock, lw) == err(tox2::kCellEbusy));
    CHECK(hits(Kind::OwnerValue) == owner_before + 1);
    tox2Write32(base, lw, tox2::kLwmutexFree);

    // A waiter word that differs from the (empty) wait list is a hit when the unlock enters the release path.
    const std::uint64_t waiter_before = hits(Kind::WaiterValue);
    CHECK(call(kTryLock, lw) == 0);
    CHECK(call(kUnlock, lw) == 0);
    CHECK(hits(Kind::WaiterValue) == waiter_before);
    CHECK(call(kTryLock, lw) == 0);
    tox2Write32(base, lw + 4, 3);
    CHECK(call(kUnlock, lw) == 0);
    CHECK(hits(Kind::WaiterValue) == waiter_before + 1);
    tox2Write32(base, lw + 4, 0);

    CHECK(tox2Read32(base, lw) == tox2::kLwmutexFree);
    CHECK(call(kDestroy, lw) == 0);
    CHECK(tox2::lwmutexWatchStats().live == live_before);
    CHECK(tox2::lwmutexWatchStats().max_live >= live_before + 1);

    // The destroyed struct no longer counts.
    const std::uint64_t copy_after_destroy = hits(Kind::HleCopy);
    CHECK(call(kMemset, lw + 20, 0, 4) == lw + 20);
    CHECK(hits(Kind::HleCopy) == copy_after_destroy);

    tox2::setLwmutexWatchEnabled(false);
    p.main.free(scratch);
    tox2::resetSyncObjects();
    tox2::setLogSink({});
    tox2::resetCensus();
}

// The counters are process-wide, so the test compares against totals taken before it.
void testPpuReservationStats() {
    std::unique_ptr<tox2::GuestMemory> memory(newMemory());
    std::uint8_t* base = memory->base();
    CHECK(memory->commit(0x20000, 0x1000));
    auto ctx = std::make_unique<Tox2PpuContext>();
    const tox2::PpuReservationStats before = tox2::ppuReservationStats();

    for (int i = 0; i < 2; i++) {
        const std::uint32_t value = tox2Lwarx(ctx.get(), base, 0x20200);
        CHECK(tox2Stwcx(ctx.get(), base, 0x20200, value + 1));
    }
    tox2Lwarx(ctx.get(), base, 0x20200);
    const std::uint8_t other_word[4] = {1, 2, 3, 4};
    tox2::reservations().put(base, 0x20208, other_word, 4);  // another word of the reserved line
    CHECK(!tox2Stwcx(ctx.get(), base, 0x20200, 99));

    const tox2::PpuReservationStats after_first = tox2::ppuReservationStats();
    CHECK(after_first.lwarx - before.lwarx == 3);
    CHECK(after_first.stwcx - before.stwcx == 3);
    CHECK(after_first.stwcx_failed - before.stwcx_failed == 1);

    // A second host thread counts into its own block, which stays in the totals after the thread exits.
    std::thread([&] {
        auto other = std::make_unique<Tox2PpuContext>();
        const std::uint32_t value = tox2Lwarx(other.get(), base, 0x20300);
        CHECK(tox2Stwcx(other.get(), base, 0x20300, value + 1));
    }).join();
    const tox2::PpuReservationStats after_second = tox2::ppuReservationStats();
    CHECK(after_second.lwarx - before.lwarx == 4);
    CHECK(after_second.stwcx - before.stwcx == 4);
    CHECK(after_second.stwcx_failed - before.stwcx_failed == 1);
}

struct ChildResult {
    DWORD exit_code = 0xFFFFFFFF;
    std::string output;
};

// Runs this executable again with `arg`, capturing stdout and stderr together.
ChildResult runChild(const wchar_t* arg) {
    ChildResult result;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring command = std::wstring(L"\"") + exe + L"\" " + arg;

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    CreatePipe(&read_end, &write_end, &sa, 0);
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = INVALID_HANDLE_VALUE;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(read_end);
        CloseHandle(write_end);
        return result;
    }
    CloseHandle(write_end);

    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(read_end, buffer, sizeof buffer, &got, nullptr) && got != 0) {
        result.output.append(buffer, got);
    }
    WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, &result.exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(read_end);
    return result;
}

void echoChild(const char* name, const ChildResult& result) {
    std::printf("--- %s: exit code %lu\n", name, static_cast<unsigned long>(result.exit_code));
    std::printf("%s", result.output.c_str());
    std::printf("--- end %s\n", name);
    std::fflush(stdout);
}

bool contains(const ChildResult& result, std::string_view needle) {
    return result.output.find(needle) != std::string::npos;
}

void testFaultChild() {
    const ChildResult r = runChild(L"--fault-child");
    echoChild("fault child", r);
    CHECK(r.exit_code == 3);
    CHECK(contains(r, "tox2: stop: fatal guest fault: write to guest address 0x00200000"));
    CHECK(contains(r,
                   "r0  0000000000000000  r1  0000000000000000  r2  0000000000000000  "
                   "r3  00000000deadbeef"));
    CHECK(contains(r, "trace (oldest -> newest, 3 entries):"));
    CHECK(contains(r, " 00010000 00010100 00010200"));
    CHECK(contains(r, "census: test printer ran"));
}

void testOverflowChild() {
    const ChildResult r = runChild(L"--overflow-child");
    echoChild("overflow child", r);
    CHECK(r.exit_code == 3);
    CHECK(contains(r, "tox2: stop: fatal guest fault: host stack overflow"));
}

void testStopChild() {
    const ChildResult r = runChild(L"--stop-child");
    echoChild("stop child", r);
    CHECK(r.exit_code == 5);
    CHECK(contains(r, "tox2: stop: watchdog 5 s"));
    CHECK(!contains(r, "registers:"));
    CHECK(contains(r, "census: test printer ran"));
}

void testStrictDispatchChild() {
    const ChildResult r = runChild(L"--strict-dispatch-child");
    echoChild("strict dispatch child", r);
    CHECK(r.exit_code == 3);
    CHECK(contains(r, "tox2: stop: fatal unknown target: target 0x00010204 from 0x00010300"));
    CHECK(contains(r, "census: unknown targets 1 distinct"));
}

void testUnsortedTableChild() {
    const ChildResult r = runChild(L"--unsorted-table-child");
    echoChild("unsorted table child", r);
    CHECK(r.exit_code == 3);
    CHECK(contains(r,
                   "tox2: stop: fatal internal error: function table: 0x00010200 follows "
                   "0x00010210 (unsorted or duplicate)"));
}

void testOverlappingSpanChild() {
    const ChildResult r = runChild(L"--overlapping-span-child");
    echoChild("overlapping span child", r);
    CHECK(r.exit_code == 3);
    CHECK(contains(r,
                   "tox2: stop: fatal internal error: function table: span 0x00010210..0x00010218 overlaps "
                   "span 0x00010200..0x00010214"));
}

void testAbortChild() {
    const ChildResult r = runChild(L"--abort-child");
    echoChild("abort child", r);
    CHECK(r.exit_code == 3);
    CHECK(contains(r, "The SPURS is aborted."));  // the console's text mode adds a CR to the line break
    CHECK(contains(r, "tox2: stop: abort from 0x011d1234 (sysPrxForUser 0x9fb6228e)"));
}

void testCallWithoutSlotChild() {
    const ChildResult r = runChild(L"--call-without-slot-child");
    echoChild("call without slot child", r);
    CHECK(r.exit_code == 3);
    CHECK(contains(r, "tox2: stop: fatal internal error: callGuestOpd without a PPU slot"));
}

void testSpuStartChild() {
    const ChildResult r = runChild(L"--spu-start-child");
    echoChild("spu start child", r);
    CHECK(r.exit_code == 3);
    // The child names the address it imported the ELF from.
    const std::string_view marker = "spu child: elf at ";
    const std::size_t at = r.output.find(marker);
    CHECK(at != std::string::npos);
    const std::string elf = at != std::string::npos ? r.output.substr(at + marker.size(), 10) : std::string();
    const std::string group = "\"SpursTLGraphicsCellSpursKernelGroup\"";
    CHECK(contains(r, "tox2: spu: start group 0x04000100 " + group + " type 0x18 priority 250: 2 threads"));
    CHECK(contains(r, "tox2: spu:   thread 0 0x01000100 \"SpursTLGraphicsCellSpursKernel0\" image " + elf +
                              " entry 0x818 segments 2 args 0x0 0x1127c80 0x0 0x0"));
    CHECK(contains(r, "tox2: spu:   thread 1 0x02000100 \"SpursTLGraphicsCellSpursKernel0\" image " + elf +
                              " entry 0x818 segments 2 args 0x100000000 0x1127c80 0x0 0x0"));
    CHECK(contains(r, "tox2: spu:     segment fill ls 0x840 size 0x20 value 0x0"));
    CHECK(contains(r, "tox2: spu:     segment copy ls 0x800 size 0x40 ea 0x"));
    CHECK(contains(r, "tox2: spu:   event queue run 0x8d000000"));
    CHECK(contains(r, "tox2: spu:   thread 0 port 2 -> queue 0x8d000000"));
    CHECK(contains(r, "tox2: spu:   thread 1 port 2 -> queue 0x8d000000"));
    // This binary registers no SPU images, so the entry at 0x818 is untracked and the first dispatch stops.
    // Which of the two threads gets there first is not fixed.
    CHECK(contains(r,
                   "tox2: spu: thread 0x01000100 \"SpursTLGraphicsCellSpursKernel0\" started at untracked ls 0x00818"));
    CHECK(contains(r,
                   "tox2: spu: thread 0x02000100 \"SpursTLGraphicsCellSpursKernel0\" started at untracked ls 0x00818"));
    CHECK(contains(r, "tox2: stop: spu: unknown target ls 0x00818"));
    CHECK(contains(r, "(untracked)"));
}

// A join on a group that is not fully initialized does not wait.
void testSpuJoinNotInitialized() {
    tox2::setLogSink([](std::string_view) {});
    tox2::resetSyncObjects();
    tox2::resetSpuThreadGroups();
    tox2::resetSpuImages();
    tox2::Process& p = tox2::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    auto a = makeCaller(0x01000000);
    std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());

    putText(base, scratch + kSpuGroupName, "group");
    tox2Write32(base, scratch + kSpuGroupAttr, 6);
    tox2Write32(base, scratch + kSpuGroupAttr + 4, scratch + kSpuGroupName);
    tox2Write32(base, scratch + kSpuGroupAttr + 8, 0);
    CHECK(syscallAs(*a, 170, scratch + kSpuIdOut, 2, 100, scratch + kSpuGroupAttr) == 0);
    const std::uint32_t group = tox2Read32(base, scratch + kSpuIdOut);
    CHECK(syscallAs(*a, 178, group, scratch + 0x1C0, scratch + 0x1C4) == err(tox2::kCellEstat));
    CHECK(syscallAs(*a, 178, 0x04000900, scratch + 0x1C0, scratch + 0x1C4) == err(tox2::kCellEsrch));
    CHECK(syscallAs(*a, 173, group) == err(tox2::kCellEstat));
    CHECK(syscallAs(*a, 177, group, 0) == err(tox2::kCellEstat));

    tox2::resetSpuThreadGroups();
    tox2::resetSpuImages();
    tox2::resetSyncObjects();
    p.main.free(scratch);
    tox2::setLogSink({});
}

void testGuestStackWalk() {
    std::unique_ptr<tox2::GuestMemory> memory(newMemory());
    tox2::RegionAllocator stack(*memory, tox2::kStackRegion);
    const std::uint32_t lo = stack.allocate(0x1000, 0x1000);
    std::uint8_t* base = memory->base();

    const Tox2FuncMapping mappings[] = {{0x10200, testFuncA}, {0x10210, testFuncB}, {0x10300, testFuncA}};
    tox2RegisterFuncs(mappings, 3);
    CHECK(tox2::findOwningFunction(0x10200) == 0x10200);
    CHECK(tox2::findOwningFunction(0x1020c) == 0x10200);
    CHECK(tox2::findOwningFunction(0x10214) == 0x10210);
    CHECK(tox2::findOwningFunction(0x101fc) == 0);
    CHECK(tox2::findOwningFunction(0x10304) == 0);  // past the last entry's slot

    // Chain sp0 -> sp1 -> sp2 -> 0. The LR for the frame at spN is saved at (back chain + 16).
    const std::uint32_t sp0 = lo + 0x100, sp1 = lo + 0x200, sp2 = lo + 0x300;
    tox2Write64(base, sp0, sp1);
    tox2Write64(base, sp1, sp2);
    tox2Write64(base, sp1 + 16, 0x10214);  // return into the middle of fn 0x10210
    tox2Write64(base, sp2, 0);
    tox2Write64(base, sp2 + 16, 0x10208);

    Tox2PpuContext ctx{};
    ctx.gpr[1] = sp0;
    tox2::GuestFrame frames[tox2::kMaxGuestFrames];
    std::size_t n = tox2::walkGuestStack(ctx, base, frames, tox2::kMaxGuestFrames);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(frames[0].lr == 0x10214 && frames[0].function == 0x10210);
        CHECK(frames[1].lr == 0x10208 && frames[1].function == 0x10200);
    }

    CHECK(tox2::walkGuestStack(ctx, base, frames, 1) == 1);

    // A cycle, a link that does not ascend, a link outside the stack region, a link into an
    // uncommitted page and a misaligned link all end the walk without touching bad memory.
    tox2Write64(base, sp2, sp0);
    CHECK(tox2::walkGuestStack(ctx, base, frames, tox2::kMaxGuestFrames) == 2);
    // The first frame is still good; only the link behind it is bad.
    tox2Write64(base, sp1, 0x00010000);
    CHECK(tox2::walkGuestStack(ctx, base, frames, tox2::kMaxGuestFrames) == 1);
    tox2Write64(base, sp1, lo + 0x1000);  // the guard page above the allocation
    CHECK(tox2::walkGuestStack(ctx, base, frames, tox2::kMaxGuestFrames) == 1);
    tox2Write64(base, sp1, sp1 + 4);
    CHECK(tox2::walkGuestStack(ctx, base, frames, tox2::kMaxGuestFrames) == 1);
    tox2Write64(base, sp0, 0x00010000);
    CHECK(tox2::walkGuestStack(ctx, base, frames, tox2::kMaxGuestFrames) == 0);
    ctx.gpr[1] = 0;
    CHECK(tox2::walkGuestStack(ctx, base, frames, tox2::kMaxGuestFrames) == 0);
    ctx.gpr[1] = 0x1'0000'0000ull + sp0;
    CHECK(tox2::walkGuestStack(ctx, base, frames, tox2::kMaxGuestFrames) == 0);
}

// Children: each ends the process through the stop path under test.

int strictDispatchChild() {
    tox2::installProcessSettings();
    tox2::installCensus();
    tox2::process().strict = true;
    const Tox2FuncMapping mappings[] = {{0x10200, testFuncA}};
    tox2RegisterFuncs(mappings, 1);
    auto ctx = std::make_unique<Tox2PpuContext>();
    ctx->jump_target = 0x10204;
    ctx->jump_site = 0x10300;
    tox2Dispatch(ctx.get(), nullptr);
    return 99;
}

int unsortedTableChild() {
    tox2::installProcessSettings();
    const Tox2FuncMapping mappings[] = {{0x10210, testFuncA}, {0x10200, testFuncB}};
    tox2RegisterFuncs(mappings, 2);
    return 99;
}

int overlappingSpanChild() {
    tox2::installProcessSettings();
    const Tox2FuncMapping first[] = {{0x10200, testFuncA}, {0x10210, testFuncB}};
    const Tox2FuncMapping second[] = {{0x10210, testFuncA}, {0x10214, testFuncB}};
    tox2RegisterFuncs(first, 2);
    tox2::appendFuncs(second, 2);
    return 99;
}

int abortChild() {
    tox2::installProcessSettings();
    tox2::process().memory.reserve();
    std::uint8_t* base = tox2::process().memory.base();
    const std::uint32_t text = tox2::process().main.allocate(0x1000, 0x1000);
    const std::string_view message = "The SPURS is aborted.\n";
    for (std::size_t i = 0; i < message.size(); i++) tox2Write8(base, text + i, static_cast<std::uint8_t>(message[i]));
    const Tox2ImportInfo imports[] = {{0x00800000, 0x9fb6228e, "sysPrxForUser", "abort"}};
    tox2::installImports(imports, 1);
    auto ctx = std::make_unique<Tox2PpuContext>();
    ctx->gpr[3] = text;
    ctx->lr = 0x011d1234;
    tox2HleCall(ctx.get(), base, 0);
    return 99;
}

// Builds the two-thread group of the SPU children and wires an event queue to it.
std::uint32_t spuChildGroup(tox2::GuestThread& caller, std::uint8_t* base, std::uint32_t scratch) {
    const std::uint32_t group = buildSpuGroup(caller, base, scratch, 2);
    const std::uint32_t queue_attr = scratch + 0x180, queue_out = scratch + 0x190, spup = scratch + 0x1C0;
    tox2Write32(base, queue_attr, 1);
    tox2Write32(base, queue_attr + 4, 2);
    CHECK(syscallAs(caller, 128, queue_out, queue_attr, 0, 8) == 0);
    const std::uint32_t queue = tox2Read32(base, queue_out);
    CHECK(syscallAs(caller, 185, group, queue, 1) == 0);
    CHECK(syscallAs(caller, 251, group, queue, 4, spup) == 0);
    return group;
}

int spuStartChild() {
    tox2::installProcessSettings();
    tox2::process().memory.reserve();
    std::uint8_t* base = tox2::process().memory.base();
    const std::uint32_t scratch = tox2::process().main.allocate(0x10000, 0x10000);
    auto caller = makeCaller(0x01000000);
    std::lock_guard<tox2::GuestLock> guard(tox2::guestLock());
    const std::uint32_t group = spuChildGroup(*caller, base, scratch);
    std::printf("spu child: elf at %#010x\n", scratch + kSpuElf);
    std::fflush(stdout);
    syscallAs(*caller, 173, group);
    // The SPU threads stop the process; the join keeps this thread from returning first.
    syscallAs(*caller, 178, group, 0, 0);
    return 99;
}

int callWithoutSlotChild() {
    tox2::installProcessSettings();
    tox2::process().memory.reserve();
    tox2::process().memory.commit(0x500000, 0x1000);
    tox2::setPpuThreadsMode(tox2::PpuThreadsMode::Split1);
    auto ctx = std::make_unique<Tox2PpuContext>();
    ctx->gpr[1] = 0x500800;
    // This thread holds no PPU slot.
    tox2::callGuestFunction(ctx.get(), tox2::process().memory.base(), 0x500000, {});
    return 99;
}

void childCensus() { std::fprintf(stderr, "census: test printer ran\n"); }

Tox2PpuContext g_child_ctx;

int faultChild() {
    tox2::installProcessSettings();
    tox2::installFaultHandler();
    tox2::process().memory.reserve();
    tox2::setCensusPrinter(&childCensus);
    g_child_ctx.gpr[3] = 0xdeadbeefull;
    g_child_ctx.trace_pos = 3;
    g_child_ctx.trace[0] = 0x00010000;
    g_child_ctx.trace[1] = 0x00010100;
    g_child_ctx.trace[2] = 0x00010200;
    tox2::setFatalContext(&g_child_ctx);

    // The page was never committed.
    *reinterpret_cast<volatile std::uint8_t*>(tox2::process().memory.base() + 0x00200000) = 1;
    return 99;
}

// The volatile limit keeps the compiler from proving the recursion infinite and removing it.
volatile int g_recursion_limit = 0x7FFFFFFF;

TOX2_NOINLINE int recurse(int depth) {
    volatile char pad[512];
    pad[0] = static_cast<char>(depth);
    return depth < g_recursion_limit ? recurse(depth + 1) + pad[0] : 0;
}

int overflowChild() {
    tox2::installProcessSettings();
    tox2::installFaultHandler();
    tox2::process().memory.reserve();
    return recurse(0);
}

int stopChild() {
    tox2::installProcessSettings();
    tox2::setCensusPrinter(&childCensus);
    tox2::stopProcess(tox2::ExitCode::Watchdog, "tox2: stop: watchdog 5 s");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        const std::string_view mode = argv[1];
        if (mode == "--fault-child") return faultChild();
        if (mode == "--overflow-child") return overflowChild();
        if (mode == "--stop-child") return stopChild();
        if (mode == "--strict-dispatch-child") return strictDispatchChild();
        if (mode == "--unsorted-table-child") return unsortedTableChild();
        if (mode == "--overlapping-span-child") return overlappingSpanChild();
        if (mode == "--abort-child") return abortChild();
        if (mode == "--spu-start-child") return spuStartChild();
        if (mode == "--call-without-slot-child") return callWithoutSlotChild();
    }

    tox2::installProcessSettings();
    testLogLevelsAndPrefix();
    testEventHistoryRing();
    testLogVisibleLevel();
    testLogOnce();
    testGuestMemoryRoundTrip();
    testIsCommittedAndRangeNames();
    testGuestWriteCollection();
    testTopOfAddressSpace();
    testAllocatorFirstFit();
    testAllocatorMarkUsedAndExhaustion();
    testStackGuards();
    testProcessSingleton();
    testElfLoader();
    testLoadModule();
    testLibsreLayout();
    testDispatch();
    testDispatchSpans();
    testCensus();
    testCensusRenderSuffix();
    testLatencyStats();
    testDisableFrameBuilder();
    testSyscalls();
    testHle();
    testHleGuestExports();
    testModuleImports();
    testGuestExportConflicts();
    testHleOutPointerHints();
    testHostFunctionsAndCallGuestFunction();
    testStartupSyscalls();
    testProcessAndPageSyscalls();
    testMemoryContainers();
    testSemaphores();
    testFileSystem();
    testSysPrxForUser();
    testMutexAndCond();
    testEventObjects();
    testEventQueueIpcKeys();
    testSpuAndSysmodule();
    testSpuThreadGroups();
    testCellGcmSys();
    testParamSfo();
    testCellGame();
    testCellRtc();
    testCellFs();
    testVfsConfig();
    testSceNpAndTrophy();
    testCellSysutil();
    testVideoAndAudioOut();
    testCellPad();
    testCellPadIntercept();
    testPadButtons();
    testKeyboardPadScanCodes();
    testKeyboardPadState();
    testKeyboardPadProvider();
    testKeyboardPadThreads();
    testPadStateFromInput();
    testGamepadSourceState();
    testGamepadPadProvider();
    testGamepadCellPad();
    testPadScriptParse();
    testPadScriptErrors();
    testPadScriptPlay();
    testCellMsgDialog();
    testCellSaveData();
    testCellPngDec();
    testFifoParser();
    testRsxArgCensus();
    testRsxRegisters();
    testRsxRegisterGenerations();
    testRsxDecode();
    testRsxTextureLayout();
    testRsxFrameBuilder();
    testFragmentProgramCache();
    testVertexProgramCache();
    testSharedVertexConstants();
    testVertexConstantSnapshotMatchesRegisters();
    testFillVerticesOnePass();
    testSharedVertexRanges();
    testRecycledDrawBuffers();
    testRsxTextureCache();
    testRsxTextureUnitCache();
    testRsxTextureCacheValidity();
    testRsxUnimplementedThreadNotes();
    testRsxHashRange();
    testRsxWriteCheck();
    testRsxTextureHashSkip();
    testRsxFloatAndDepthSurfaces();
    testRsxCapture();
    testRsxSyntheticCaptures();
    testRsxConsumer();
    testLwmutex();
    testLwcond();
    testLwmutexHandoff();
    testMutexHandoff();
    testGuestPrintf();
    testSysPrxForUserLibc();
    testSpuImageImport();
    testSpuPrintfCallbacks();
    testPrxModuleId();
    testProcessParamsfo();
    testPpuThreads();
    testPpuThreadCreatePriority();
    testGcmInterruptThread();
    testJudgeThreads();
    testTimebase();
    testTimerResolution();
    testHostPriority();
    testGuestLockFifo();
    testGuestLockStats();
    testGuestLockLongHold();
    testGuestLockSingleWake();
    testGuestLockPpuOverlap();
    testGuestLockPpuPriority();
    testPpuSlotsTwoHolders();
    testPpuSlotsPriority();
    testPpuSlotsRotation();
    testPpuSlotsYield();
    testPpuSlotsStats();
    testPpuSlotsUncontendedCost();
    testSplitBlockingWaitOrder();
    testSplitTwoRunners();
    testSplitYield();
    testSplitKernelHelpers();
    testKernelWaitByCall();
    testFsReadReleasesKernelLock();
    testRsxWriteCheckWriters();
    testFsConcurrentFdUse();
    testSplitCreatePriority();
    testSplitExitInKernelSection();
    testSplitReturnPathTls();
    testSplitYieldInsideKernel();
    testSplitReservationCounter();
    testSplitSyscallStress();
    testLwmutexWord();
    testLwmutexProtocol();
    testLwmutexKernelFree();
    testLwmutexContentionStress();
    testLwmutexDestroyRace();
    testLwmutexPublishRace();
    testLwmutexPairBenchmark();
    testKernelFreeHandlers();
    testMemcpyNeighbourWrites();
    testGcmAddressToOffsetConcurrent();
    testKernelFreeSleep();
    testSplitGuestCallbackReleasesKernel();
    testSplitSysutilCallbacks();
    testSplitDefaultPathCount();
    testCensusConcurrentCount();
    testHostFunctionConcurrentLookup();
    testSplitGcmInterruptStart();
    testSplitSailServiceStart();
    testSplitBoundaryCost();
    testYieldBackoff();
    testPpuYieldStats();
    testPpuYieldSemantics();
    testPerfLine();
    testHostThreadCpu();
    testCycleStats();
    testRsxBusyFormat();
    testReservationAndDcbz();
    testPpuReservationStats();
    testLwmutexWatchHooks();
    testLwmutexWatchRegistration();
    testGuestStackWalk();
    testFaultChild();
    testOverflowChild();
    testStopChild();
    testStrictDispatchChild();
    testUnsortedTableChild();
    testOverlappingSpanChild();
    testAbortChild();
    testCallWithoutSlotChild();
    testSpuStartChild();
    testSpuJoinNotInitialized();
    std::printf("runtime core tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}
