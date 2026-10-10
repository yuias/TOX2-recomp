// Self-checking unit tests for the runtime core library. No generated code is involved.

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <csetjmp>
#include <cstdlib>
#include <condition_variable>
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
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/host_functions.h"
#include "core/log.h"
#include "core/param_sfo.h"
#include "core/threads.h"
#include "core/timer_resolution.h"
#include "tox2/ppu_context.h"
#include "hle/hle.h"
#include "input/keyboard_pad.h"
#include "input/pad_buttons.h"
#include "input/pad_script.h"
#include "png.h"
#include "lv2/cell_error.h"
#include "lv2/sys_memory.h"
#include "lv2/sys_ppu_thread.h"
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
#include "rsx/rsx_registers.h"
#include "rsx/rsx_state.h"
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
    CHECK(table.size() == 91);  // every number the game issues plus the rest of the families that are implemented
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
    ctx->gpr[11] = 181;  // sys_spu_thread_write_ls has no handler
    ctx->gpr[3] = 0x1234;
    tox2Syscall(ctx.get(), nullptr, 0x10000);
    CHECK(ctx->gpr[3] == tox2::kCellOk);
    tox2Syscall(ctx.get(), nullptr, 0x10000);
    ctx->gpr[11] = 999;
    ctx->gpr[3] = 0x1234;
    tox2Syscall(ctx.get(), nullptr, 0x10004);
    CHECK(ctx->gpr[3] == tox2::kCellOk);

    CHECK(countContaining(lines, "tox2: first sc 181 sys_spu_thread_write_ls") == 1);
    CHECK(countContaining(lines, "tox2: first sc 999 ?") == 1);
    CHECK(countContaining(lines, "tox2: default sc 181 sys_spu_thread_write_ls(0x1234") == 1);
    CHECK(countContaining(lines, "tox2: default sc 999 ?(0x1234") == 1);

    const std::string census = tox2::formatCensus();
    CHECK(census.find("census: syscalls 3 calls, 2 distinct, 2 default-handled") != std::string::npos);
    CHECK(census.find("  sc 181 sys_spu_thread_write_ls: 2 [default]") != std::string::npos);
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
    CHECK(tox2::sysPrxForUserHandlers().size() == 23);

    tox2::setLogSink({});
    tox2::resetCensus();
    tox2::resetLogOnce();
}

// A guest thread object that is not registered or started: it only gives a syscall caller an id.
std::unique_ptr<tox2::GuestThread> makeCaller(std::uint32_t id) {
    return std::make_unique<tox2::GuestThread>(id, tox2::GuestThreadParams{}, 0, 0);
}

std::uint64_t syscallAs(tox2::GuestThread& caller, std::uint32_t number, std::uint64_t r3 = 0, std::uint64_t r4 = 0,
                        std::uint64_t r5 = 0, std::uint64_t r6 = 0, std::uint64_t r7 = 0) {
    Tox2PpuContext* ctx = &caller.context();
    ctx->gpr[11] = number;
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    ctx->gpr[5] = r5;
    ctx->gpr[6] = r6;
    ctx->gpr[7] = r7;
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
    CHECK(syscallAs(*a, 251, 0x04000100, queue, 4, spup) == err(tox2::kCellEsrch));
    CHECK(tox2::createSpuThreadGroup() == 0x04000100);
    CHECK(!tox2::spuThreadGroupExists(0x04000101));
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
        return true;
    }
    bool stopAfterWrite() override { return !writes.empty() && writes.back() == stop_on; }
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
    int at_ms;
    tox2::PadButtons buttons;
    bool down;
};

bool scriptEventsAre(const std::string& text, std::initializer_list<ScriptEventSpec> expected) {
    const auto parsed = tox2::parsePadScript(text);
    if (!parsed) {
        std::fprintf(stderr, "parse failed: %s\n", parsed.error().c_str());
        return false;
    }
    if (parsed->size() != expected.size()) return false;
    std::size_t i = 0;
    for (const ScriptEventSpec& want : expected) {
        const tox2::PadScriptEvent& got = (*parsed)[i++];
        if (got.at.count() != want.at_ms || got.buttons != want.buttons || got.down != want.down) return false;
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
    CHECK(loaded && loaded->size() == 2 && (*loaded)[0].at.count() == 2000);
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
    CHECK(parsed && parsed->size() == 4);
    if (!parsed || parsed->size() != 4) return;

    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::setGuestThreadIdProvider(nullptr);
    tox2::KeyboardPadState state;
    std::vector<std::int64_t> waits;
    std::vector<tox2::PadButtons> held_before_wait;
    tox2::playPadScript(*parsed, state, [&](milliseconds at) {
        waits.push_back(at.count());
        held_before_wait.push_back(state.held());
    });
    tox2::setLogSink({});

    // wait_until runs before each event, so it sees the state left by the previous one.
    CHECK((waits == std::vector<std::int64_t>{1000, 2000, 3000, 4000}));
    CHECK((held_before_wait == std::vector<tox2::PadButtons>{0, kStart, kStart | kCross | kR1, kCross | kR1}));
    CHECK(state.held() == 0);

    CHECK(lines.size() == 4);
    if (lines.size() == 4) {
        CHECK(lines[0] == "[host] tox2: pad: script 1.000 s: press start");
        CHECK(lines[1] == "[host] tox2: pad: script 2.000 s: press r1+cross");
        CHECK(lines[2] == "[host] tox2: pad: script 3.000 s: release start");
        CHECK(lines[3] == "[host] tox2: pad: script 4.000 s: release r1+cross");
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
    tox2::startPadScript({{milliseconds(0), kCross, true}, {milliseconds(20), kStart, true}}, shared);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (shared->held() != (kCross | kStart) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(1));
    }
    CHECK(shared->held() == (kCross | kStart));
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

    enum Import : std::uint32_t { AutoLoad, AutoSave, ListLoad, ListSave, Delete, ImportCount };
    const Tox2ImportInfo imports[] = {
            {0x00800000, 0xfbd5c856, "cellSysutil", "cellSaveDataAutoLoad2"},
            {0x00800010, 0x8b7ed64b, "cellSysutil", "cellSaveDataAutoSave2"},
            {0x00800020, 0x1dfbfdd6, "cellSysutil", "cellSaveDataListLoad2"},
            {0x00800030, 0x2de0d663, "cellSysutil", "cellSaveDataListSave2"},
            {0x00800040, 0xedadd797, "cellSysutil", "cellSaveDataDelete2"},
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
    const std::vector<tox2::RsxVertexAttrib> attribs = tox2::decodeVertexAttribs(*regs);
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
    CHECK(tox2::decodeVertexAttribs(*regs).size() == 5 && unimplementedCount("vertex type cmp") == 1);
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
    regs->apply(0x1A04, 1u | (2u << 4) | ((0x85u | 0x20u | 0x40u) << 8) | (1u << 16));
    CHECK(!tox2::decodeTexture(*regs, 0).normalized);
    CHECK(unimplementedCount("texture format A8R8G8B8_LN_UN") == 0);  // the renderer scales the coordinates

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
    CHECK(unimplementedCount("state stencil test") == 1);

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
        const std::vector<std::uint8_t>& bytes = location == 0 ? local : main;
        if (offset > bytes.size() || out.size() > bytes.size() - offset) return false;
        std::memcpy(out.data(), bytes.data() + offset, out.size());
        return true;
    }
    void putWord(std::uint32_t location, std::uint32_t offset, std::uint32_t word) {
        std::vector<std::uint8_t>& bytes = location == 0 ? local : main;
        for (int i = 0; i < 4; i++) bytes[offset + i] = static_cast<std::uint8_t>(word >> (24 - 8 * i));
    }
    std::vector<std::uint8_t> local, main;
};

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

    // CLEAR_SURFACE 0xF3 gives a clear that keeps all bits and registers the color target.
    write(0x1D90, 0xFF102030);
    write(0x1D8C, 0xFFFFFF00);
    write(0x1D94, 0xF3);
    std::vector<tox2::RsxCommand> commands = builder.takeCommands();
    CHECK(commands.size() == 1 && std::holds_alternative<tox2::RsxClear>(commands[0]));
    const tox2::RsxClear& clear = std::get<tox2::RsxClear>(commands[0]);
    CHECK(clear.mask == 0xF3 && clear.color_argb == 0xFF102030 && clear.depth_stencil == 0xFFFFFF00);
    CHECK(clear.scissor[0] == 0 && clear.scissor[1] == 0 && clear.scissor[2] == 1280 && clear.scissor[3] == 720);
    CHECK(builder.knownSurfaces().size() == 1 && builder.knownSurfaces()[0].offset == 0x100000);
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
    CHECK(indexed.vp_constants.size() == tox2::kRsxVpConstantCount * 4);
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
    // bytes are copied without the row padding; unit 2 is swizzled and only counted. Disabled units are
    // not decoded.
    for (std::uint32_t i = 0; i < 256; i++) memory.local[0x3000 + i] = static_cast<std::uint8_t>(i ^ 0x5A);
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
    CHECK(textured.textures[0].unit == 0 && textured.textures[0].rt_alias == 0 && textured.textures[0].data_size == 0);
    CHECK(textured.textures[1].unit == 1 && textured.textures[1].rt_alias == -1 &&
          textured.textures[1].data_size == 64 * 3 + 64);
    CHECK(textured.texture_data.size() == 256 && textured.texture_data[0] == (0 ^ 0x5A) &&
          textured.texture_data[255] == (255 ^ 0x5A));
    CHECK(textured.textures[2].unit == 2 && textured.textures[2].data_size == 0);
    CHECK(unimplementedCount("texture layout swizzled") == 1);
    CHECK(unimplementedCount("state texture location 0x0") == 0);

    // Block-compressed textures copy whole block rows whatever the layout bit and the pitch register say:
    // unit 3 is a swizzled-flag DXT45 8x5 (2x2 blocks of 16 bytes), unit 4 a linear DXT1 6x4 with a
    // padded pitch register (2x1 blocks of 8 bytes), unit 5 a DXT1 with two mip levels (counted, no bytes).
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
    CHECK(compressed.textures[0].unit == 3 && compressed.textures[0].data_size == 64 &&
          compressed.textures[0].pitch == 32);
    CHECK(compressed.textures[1].unit == 4 && compressed.textures[1].data_size == 16 &&
          compressed.textures[1].pitch == 16);
    CHECK(compressed.textures[2].unit == 5 && compressed.textures[2].data_size == 0);
    CHECK(compressed.texture_data.size() == 64 + 16);
    CHECK(compressed.texture_data[0] == (0 ^ 0x5A) && compressed.texture_data[63] == (63 ^ 0x5A));
    CHECK(compressed.texture_data[compressed.textures[1].data_offset] == (0 ^ 0x5A) &&
          compressed.texture_data[compressed.textures[1].data_offset + 15] == (15 ^ 0x5A));
    CHECK(unimplementedCount("texture layout swizzled") == 1);  // compressed formats are never counted as swizzled
    CHECK(unimplementedCount("texture layout mips 2") == 1);
    CHECK(unimplementedCount("texture format DXT45_SZ_NR") == 0);

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

    // Unreadable vertex memory drops the draw and counts it.
    write(0x1680, 0x80000000 | 0x20000);
    write(0x1808, 5);
    write(0x1814, 2u << 24);
    write(0x1808, 0);
    CHECK(builder.takeCommands().empty() && unimplementedCount("state unreadable vertex memory") == 1);
    tox2::rsxUnimplementedReset();
}

bool sameSurface(const tox2::RsxSurface& a, const tox2::RsxSurface& b) {
    return a.location == b.location && a.offset == b.offset && a.pitch == b.pitch && a.width == b.width &&
           a.height == b.height && a.format == b.format;
}

bool sameSurfaceState(const tox2::RsxSurfaceState& a, const tox2::RsxSurfaceState& b) {
    return sameSurface(a.color, b.color) && a.has_color == b.has_color && sameSurface(a.depth, b.depth) &&
           a.depth_format == b.depth_format && a.clip_x == b.clip_x && a.clip_y == b.clip_y && a.clip_w == b.clip_w &&
           a.clip_h == b.clip_h;
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
        a.vertex_data != b.vertex_data || a.index_data != b.index_data || a.texture_data != b.texture_data ||
        a.vp_hash != b.vp_hash || a.vp_ucode != b.vp_ucode || a.vp_constants != b.vp_constants ||
        a.fp_hash != b.fp_hash || a.fp_ucode != b.fp_ucode || a.fp_constants != b.fp_constants ||
        a.fp_control != b.fp_control || a.texcoord_control != b.texcoord_control || a.raster.cull != b.raster.cull ||
        a.default_attribs != b.default_attribs) {
        return false;
    }
    for (std::size_t i = 0; i < a.textures.size(); i++) {
        const auto& x = a.textures[i];
        const auto& y = b.textures[i];
        if (x.unit != y.unit || x.rt_alias != y.rt_alias || x.offset != y.offset || x.pitch != y.pitch ||
            x.width != y.width || x.height != y.height || x.data_offset != y.data_offset ||
            x.data_size != y.data_size) {
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
    CHECK(!std::get<tox2::RsxDraw>(live[2]).texture_data.empty());

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
    CHECK(live.size() == 5 && std::holds_alternative<tox2::RsxFlip>(live[3]) &&
          std::holds_alternative<tox2::RsxClear>(live[4]));
    CHECK(std::get<tox2::RsxClear>(live[4]).surfaces.color.offset == 0);
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
    CHECK(replayed.size() == live.size() && std::get<tox2::RsxFlip>(live[3]).flip_index == 7);
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
    CHECK(textured.textures[0].data_size == 16 && textured.texture_data.size() == 16);
    CHECK(textured.texture_data[0] == 0xFF && textured.texture_data[1] == 0xFF && textured.texture_data[2] == 0x00);
    CHECK(unimplementedCount("capture missing range") == 0);
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

    // A flip completes where its command executes, with vsync too: it clears the flip label, notifies the
    // hook, and the writes behind it are consumed before the next VBlank.
    tox2::RsxState& state = tox2::rsxState();
    state.requested_vsync = true;
    state.flip_status = tox2::kRsxFlipStatusWaiting;
    tox2Write32(base, kLabels + 0x10, 0x1111);
    tox2::setRsxInterruptHook(&recordInterrupt);
    emit(tox2::kMethodGcmFlipCommand, {1});
    emit(tox2::kMethodNv4097SetSemaphoreOffset, {0x50});
    emit(tox2::kMethodNv4097TextureReadSemaphoreRelease, {0x55667788});
    CHECK(run() == FifoStatus::Idle && consumer.census().flips == 1);
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
    // Two flips in a row (hsync) complete in one run as well.
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
        blit(0, 11, kLocalOffset - 16, 0x00000004, 4, {0x00000000, 0x3F80, 0x01020304, 0x0A0B0C0D});
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
        CHECK(reader.surfaces().size() == 1 && reader.surfaces()[0].offset == 0x100000 &&
              reader.surfaces()[0].width == 1280);
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
        CHECK(std::memcmp(base + thread->stackAddr(), "STACKGRD", 8) == 0);
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

void testPpuThreadCreatePriority() {
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
    CHECK(equal.ok && equal.seen_after_create == 0 && equal.seen_at_end == 1);
    Outcome lower = run(1005, -1, 1100, 0);
    CHECK(lower.ok && lower.seen_after_create == 0 && lower.seen_at_end == 1);

    // The stored priority counts: a creator that moved itself from 1100 to 1005 yields to a thread
    // at 1002.
    Outcome lowered = run(1100, 1005, 1002, 0);
    CHECK(lowered.ok && lowered.seen_after_create == 1);
    // And the other way: a creator that raised itself to 1000 does not yield to a thread at 1002.
    Outcome raised = run(1005, 1000, 1002, 0);
    CHECK(raised.ok && raised.seen_after_create == 0 && raised.seen_at_end == 1);

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

void testGcmInterruptThread() {
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
    CHECK(counts.vblank == 2 && counts.flip == 0 && counts.user == 2);
    CHECK(tox2::formatCensus().find("_gcm_intr_thread: blocked in gcm interrupt wait [service]") != std::string::npos);

    tox2::stopGcmInterruptThread();
    CHECK(thread->state() == tox2::ThreadState::Exited);
    CHECK(tox2::gcmInterruptCounts().vblank == 0);

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

void testReservationAndDcbz() {
    std::unique_ptr<tox2::GuestMemory> memory(newMemory());
    std::uint8_t* base = memory->base();
    CHECK(memory->commit(0x20000, 0x1000));
    auto ctx = std::make_unique<Tox2PpuContext>();

    tox2Write32(base, 0x20010, 5);
    CHECK(tox2Lwarx(ctx.get(), base, 0x20010) == 5);
    CHECK(tox2Stwcx(ctx.get(), base, 0x20010, 6));
    CHECK(tox2Read32(base, 0x20010) == 6);
    CHECK(!tox2Stwcx(ctx.get(), base, 0x20010, 7));  // the reservation is gone
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

    tox2Sync(ctx.get(), kTox2SyncHeavy);
    ctx->fpscr = 0;
    tox2Mtfsf(ctx.get(), 0x80, 0xF0000000u);
    CHECK(tox2Mffs(ctx.get()) == 0xF0000000u);
    tox2Mtfsb(ctx.get(), 31, 1);
    CHECK(tox2Mffs(ctx.get()) == 0xF0000001u);
    tox2Mtfsb(ctx.get(), 0, 0);
    CHECK(tox2Mffs(ctx.get()) == 0x70000001u);
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
    }

    tox2::installProcessSettings();
    testLogLevelsAndPrefix();
    testLogVisibleLevel();
    testLogOnce();
    testGuestMemoryRoundTrip();
    testIsCommittedAndRangeNames();
    testTopOfAddressSpace();
    testAllocatorFirstFit();
    testAllocatorMarkUsedAndExhaustion();
    testStackGuards();
    testProcessSingleton();
    testElfLoader();
    testDispatch();
    testDispatchSpans();
    testCensus();
    testCensusRenderSuffix();
    testDisableFrameBuilder();
    testSyscalls();
    testHle();
    testHleGuestExports();
    testModuleImports();
    testGuestExportConflicts();
    testHleOutPointerHints();
    testHostFunctionsAndCallGuestFunction();
    testStartupSyscalls();
    testMemoryContainers();
    testSemaphores();
    testFileSystem();
    testSysPrxForUser();
    testMutexAndCond();
    testEventObjects();
    testEventQueueIpcKeys();
    testSpuAndSysmodule();
    testCellGcmSys();
    testParamSfo();
    testCellGame();
    testCellFs();
    testVfsConfig();
    testSceNpAndTrophy();
    testCellSysutil();
    testVideoAndAudioOut();
    testCellPad();
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
    testRsxDecode();
    testRsxFrameBuilder();
    testRsxCapture();
    testRsxSyntheticCaptures();
    testRsxConsumer();
    testLwmutex();
    testPpuThreads();
    testPpuThreadCreatePriority();
    testGcmInterruptThread();
    testJudgeThreads();
    testTimebase();
    testTimerResolution();
    testGuestLockFifo();
    testReservationAndDcbz();
    testGuestStackWalk();
    testFaultChild();
    testOverflowChild();
    testStopChild();
    testStrictDispatchChild();
    testUnsortedTableChild();
    testOverlappingSpanChild();
    std::printf("runtime core tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}
