// Whole-program SPU tests: assembled SPU programs, emitted to C by tools/tests/spu_programs/gen.py through the
// real recovery and emitter, run on host threads through the lv2 SPU thread group calls. The images sit at the
// fixed guest addresses of the generated image table; everything else (arrays, counters, mailboxes, events) goes
// through the syscalls and the guest memory, as it does for the game.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/guest_memory.h"
#include "core/log.h"
#include "core/threads.h"
#include "core/timer_resolution.h"
#include "lv2/cell_error.h"
#include "lv2/sys_spu.h"
#include "lv2/sys_sync.h"
#include "spu/spu_thread.h"
#include "spu_programs_data.h"
#include "tox2/ppu_context.h"
#include "tox2/spu_context.h"

namespace {

int g_passed;
int g_failed;
int g_check_failures;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            g_check_failures++;                                                    \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
        }                                                                          \
    } while (0)

using Clock = std::chrono::steady_clock;

template <class Pred>
bool waitUntil(Pred pred, int timeout_ms = 5000) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!pred()) {
        if (Clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

// A PPU thread as the syscall handlers see it: a register context, every call made under the guest lock (which a
// waiting call gives up while it waits).
struct PpuCaller {
    std::unique_ptr<Tox2PpuContext> ctx = std::make_unique<Tox2PpuContext>();

    std::uint32_t call(std::uint32_t number, std::uint64_t r3 = 0, std::uint64_t r4 = 0, std::uint64_t r5 = 0,
                       std::uint64_t r6 = 0, std::uint64_t r7 = 0, std::uint64_t r8 = 0) {
        std::lock_guard<tox2::GuestLock> lock(tox2::guestLock());
        ctx->gpr[11] = number;
        ctx->gpr[3] = r3;
        ctx->gpr[4] = r4;
        ctx->gpr[5] = r5;
        ctx->gpr[6] = r6;
        ctx->gpr[7] = r7;
        ctx->gpr[8] = r8;
        tox2Syscall(ctx.get(), tox2::process().memory.base(), 0x10000);
        return static_cast<std::uint32_t>(ctx->gpr[3]);
    }
};

// Offsets in the scratch block of guest memory.
constexpr std::uint32_t kIdOut = 0x000;
constexpr std::uint32_t kGroupAttr = 0x010;
constexpr std::uint32_t kThreadOut = 0x030;
constexpr std::uint32_t kThreadAttr = 0x050;
constexpr std::uint32_t kArgs = 0x070;
constexpr std::uint32_t kImageStruct = 0x0a0;
constexpr std::uint32_t kQueueAttr = 0x100;
constexpr std::uint32_t kQueueOut = 0x120;
constexpr std::uint32_t kCauseOut = 0x140;
constexpr std::uint32_t kStatusOut = 0x144;
constexpr std::uint32_t kValueOut = 0x148;
constexpr std::uint32_t kNames = 0x200;
constexpr std::uint32_t kSegments = 0x400;  // up to 16 sys_spu_segment records
constexpr std::uint32_t kData = 0x1000;     // arrays and counters of the programs

constexpr std::uint32_t kSegmentCopy = 1;
constexpr std::uint32_t kImageSlot = 0x10000;  // guest range reserved per image (IMAGE_STRIDE of the generator)

struct Fixture {
    std::uint8_t* base;
    std::uint32_t scratch;
    PpuCaller ppu;
    std::vector<std::uint32_t> groups;

    Fixture() {
        tox2::resetSyncObjects();
        tox2::resetSpuThreadGroups();
        tox2::resetSpuImages();
        tox2::setLogSink([](std::string_view) {});
        tox2::process().memory.reserve();
        base = tox2::process().memory.base();
        scratch = tox2::process().main.allocate(0x10000, 0x10000);
        for (std::uint32_t i = 0; i < kTox2SpuImageCount; i++) {
            const Tox2SpuImageInfo& image = kTox2SpuImages[i];
            tox2::process().main.markUsed(image.ea, kImageSlot);
            CHECK(tox2::process().memory.commit(image.ea, image.size));
            std::memcpy(base + image.ea, kSpuProgramBytes[i], image.size);
        }
        tox2::registerSpuImages(kTox2SpuImages, kTox2SpuImageCount);
        // The SHA-256 of every image in the table against the bytes just placed.
        CHECK(tox2::checkSpuImages(base));
    }

    ~Fixture() {
        // A group that is still running must be ended first: resetting waits for host threads that may need
        // the guest lock.
        for (const std::uint32_t group : groups) ppu.call(177, group, 0);
        tox2::resetSpuThreadGroups();
        tox2::resetSpuImages();
        tox2::resetSyncObjects();
        for (std::uint32_t i = 0; i < kTox2SpuImageCount; i++) tox2::process().main.free(kTox2SpuImages[i].ea);
        tox2::process().main.free(scratch);
        tox2::setLogSink({});
    }

    void putText(std::uint32_t at, std::string_view text) {
        for (std::size_t i = 0; i < text.size(); i++) tox2Write8(base, at + static_cast<std::uint32_t>(i), text[i]);
        tox2Write8(base, at + static_cast<std::uint32_t>(text.size()), 0);
    }

    std::uint32_t makeGroup(std::uint32_t num) {
        putText(scratch + kNames, "program group");
        tox2Write32(base, scratch + kGroupAttr, 8);
        tox2Write32(base, scratch + kGroupAttr + 4, scratch + kNames);
        tox2Write32(base, scratch + kGroupAttr + 8, 0x18);
        CHECK(ppu.call(170, scratch + kIdOut, num, 250, scratch + kGroupAttr) == 0);
        groups.push_back(tox2Read32(base, scratch + kIdOut));
        return groups.back();
    }

    // Initializes thread `index` of `group` with image `image_index` of the generated table: one COPY segment per
    // image segment, the entry point of the image, and the two 64-bit arguments in $3 and $4.
    std::uint32_t makeThread(std::uint32_t group, std::uint32_t index, std::uint32_t image_index, std::uint64_t arg0,
                             std::uint64_t arg1 = 0) {
        const Tox2SpuImageInfo& image = kTox2SpuImages[image_index];
        const std::uint32_t segs = scratch + kSegments;
        for (std::uint32_t i = 0; i < image.segment_count; i++) {
            const Tox2SpuSegment& s = image.segments[i];
            tox2Write32(base, segs + 16 * i, kSegmentCopy);
            tox2Write32(base, segs + 16 * i + 4, s.ls);
            tox2Write32(base, segs + 16 * i + 8, s.size);
            tox2Write32(base, segs + 16 * i + 12, s.ea);
        }
        tox2Write32(base, scratch + kImageStruct, 0);
        tox2Write32(base, scratch + kImageStruct + 4, image.entry_point);
        tox2Write32(base, scratch + kImageStruct + 8, segs);
        tox2Write32(base, scratch + kImageStruct + 12, image.segment_count);
        putText(scratch + kNames + 0x80, image.name);
        tox2Write32(base, scratch + kThreadAttr, scratch + kNames + 0x80);
        tox2Write32(base, scratch + kThreadAttr + 4, static_cast<std::uint32_t>(std::strlen(image.name)) + 1);
        tox2Write32(base, scratch + kThreadAttr + 8, 2);
        tox2Write64(base, scratch + kArgs, arg0);
        tox2Write64(base, scratch + kArgs + 8, arg1);
        tox2Write64(base, scratch + kArgs + 16, 0);
        tox2Write64(base, scratch + kArgs + 24, 0);
        CHECK(ppu.call(172, scratch + kThreadOut + 4 * index, group, index, scratch + kImageStruct,
                       scratch + kThreadAttr, scratch + kArgs) == 0);
        return tox2Read32(base, scratch + kThreadOut + 4 * index);
    }

    std::uint32_t makeQueue(std::uint32_t size = 8) {
        tox2Write32(base, scratch + kQueueAttr, 1);
        tox2Write32(base, scratch + kQueueAttr + 4, 1);
        CHECK(ppu.call(128, scratch + kQueueOut, scratch + kQueueAttr, 0, size) == 0);
        return tox2Read32(base, scratch + kQueueOut);
    }

    // Starts the group and joins it; the cause and status are left in the scratch outputs.
    void runGroup(std::uint32_t group) {
        CHECK(ppu.call(173, group) == 0);
        CHECK(join(group) == 0);
    }
    std::uint32_t join(std::uint32_t group) { return ppu.call(178, group, scratch + kCauseOut, scratch + kStatusOut); }
    std::uint32_t cause() const { return tox2Read32(base, scratch + kCauseOut); }
    std::uint32_t status() const { return tox2Read32(base, scratch + kStatusOut); }

    // sys_spu_thread_read_ls: one 32-bit local store word of a thread that has run.
    bool readLsWord(std::uint32_t thread_id, std::uint32_t lsa, std::uint32_t* value) {
        const std::uint32_t code = ppu.call(182, thread_id, lsa, scratch + kValueOut, 4);
        *value = static_cast<std::uint32_t>(tox2Read64(base, scratch + kValueOut));
        return code == 0;
    }

    bool waitUntilBlocked(std::uint32_t thread_id, std::string_view what) {
        return waitUntil([&] {
            std::lock_guard<tox2::GuestLock> lock(tox2::guestLock());
            const tox2::SpuThread* thread = tox2::spuThreadById(thread_id);
            return thread != nullptr && thread->state() == tox2::SpuState::Blocked && thread->blockedIn() != nullptr &&
                   std::string_view(thread->blockedIn()).contains(what);
        });
    }

    std::uint64_t dispatches(std::uint32_t thread_id) {
        std::lock_guard<tox2::GuestLock> lock(tox2::guestLock());
        const tox2::SpuThread* thread = tox2::spuThreadById(thread_id);
        return thread == nullptr ? 0 : thread->dispatchCount();
    }
};

// 1. A loop that GETs a guest array in chunks and sums it; the GET and the sum are functions called with brsl
// and left with bi $0.
void testSumArray() {
    Fixture f;
    constexpr std::uint32_t kWords = 1024;
    const std::uint32_t array = f.scratch + kData;
    std::uint32_t expected = 0;
    for (std::uint32_t i = 0; i < kWords; i++) {
        tox2Write32(f.base, array + 4 * i, 3 * i + 1);
        expected += 3 * i + 1;
    }
    CHECK(kWords * 4 % kSpuProgChunkBytes == 0);

    const std::uint32_t group = f.makeGroup(1);
    const std::uint32_t thread = f.makeThread(group, 0, kSpuProg_sum_array, array, kWords * 4);
    f.runGroup(group);
    CHECK(f.cause() == 2);
    CHECK(f.status() == expected);
    // The entry plus two returns per chunk went through the dispatcher.
    CHECK(f.dispatches(thread) == 1 + 2 * (kWords * 4 / kSpuProgChunkBytes));
}

// 2. Two threads of one group increment one guest counter with GETLLAR/PUTLLC retry loops.
void testAtomicCounter() {
    Fixture f;
    // The counter shares a reservation line with words that must come out unchanged.
    const std::uint32_t line = f.scratch + 0x4000;
    constexpr std::uint32_t kCounterOffset = 0x20;
    for (std::uint32_t i = 0; i < 32; i++) tox2Write32(f.base, line + 4 * i, 0xa5a50000 + i);
    tox2Write32(f.base, line + kCounterOffset, 0);

    const std::uint32_t group = f.makeGroup(2);
    f.makeThread(group, 0, kSpuProg_atomic_counter, line + kCounterOffset);
    f.makeThread(group, 1, kSpuProg_atomic_counter, line + kCounterOffset);
    f.runGroup(group);
    CHECK(f.cause() == 2);
    CHECK(tox2Read32(f.base, line + kCounterOffset) == 2 * kSpuProgCounterRounds);
    for (std::uint32_t i = 0; i < 32; i++) {
        if (4 * i != kCounterOffset) CHECK(tox2Read32(f.base, line + 4 * i) == 0xa5a50000 + i);
    }
}

// 3. A jump table: one thread per selector, each leaves its result in local store.
void testJumpTable() {
    Fixture f;
    const std::uint32_t selectors[] = {0, 1, 2, 3, 4, 0xffffffffu};
    const std::uint32_t expected[] = {kSpuProgTableResult0, kSpuProgTableResult1, kSpuProgTableResult2,
                                      kSpuProgTableResult3, kSpuProgTableDefault, kSpuProgTableDefault};
    constexpr std::uint32_t kCount = sizeof selectors / sizeof selectors[0];

    const std::uint32_t group = f.makeGroup(kCount);
    std::uint32_t threads[kCount];
    for (std::uint32_t i = 0; i < kCount; i++) {
        threads[i] = f.makeThread(group, i, kSpuProg_jump_table, selectors[i]);
    }
    f.runGroup(group);
    CHECK(f.cause() == 2);
    for (std::uint32_t i = 0; i < kCount; i++) {
        std::uint32_t result = 0;
        CHECK(f.readLsWord(threads[i], kSpuProgBufLs, &result));
        CHECK(result == expected[i]);
        if (result != expected[i])
            std::fprintf(stderr, "selector %#x: got %#x, expected %#x\n", selectors[i], result, expected[i]);
    }
}

// 4. A thread throws an event, then blocks on its inbound mailbox until the PPU side answers.
void testThrowEvent() {
    Fixture f;
    const std::uint32_t group = f.makeGroup(1);
    const std::uint32_t thread = f.makeThread(group, 0, kSpuProg_throw_event, 0);
    const std::uint32_t queue = f.makeQueue();
    CHECK(f.ppu.call(191, thread, queue, 1, kSpuProgEventPort) == 0);
    CHECK(f.ppu.call(173, group) == 0);

    CHECK(f.ppu.call(130, queue, 0, 5000000) == 0);
    const Tox2PpuContext& ctx = *f.ppu.ctx;
    CHECK(ctx.gpr[4] == 0xFFFFFFFF53505501ull);
    CHECK(ctx.gpr[5] == thread);
    CHECK(ctx.gpr[6] == ((std::uint64_t{kSpuProgEventPort} << 32) | kSpuProgEventValue));
    CHECK(ctx.gpr[7] == kSpuProgEventDataValue);

    // The thread waits for the reply and has not exited.
    CHECK(f.waitUntilBlocked(thread, "RdInMbox"));
    constexpr std::uint32_t kReply = 0x1000;
    CHECK(f.ppu.call(190, thread, kReply) == 0);
    CHECK(f.join(group) == 0);
    CHECK(f.cause() == 2);
    CHECK(f.status() == kReply + 1);
}

void runProgram(const char* name, void (*test)()) {
    const int before = g_check_failures;
    test();
    const bool ok = g_check_failures == before;
    std::printf("spu program %s: %s\n", name, ok ? "ok" : "FAILED");
    (ok ? g_passed : g_failed)++;
}

}  // namespace

int main() {
    // Timed waits round up to the system tick otherwise.
    const tox2::TimerResolution timer_resolution;

    runProgram("sum_array", testSumArray);
    runProgram("atomic_counter", testAtomicCounter);
    runProgram("jump_table", testJumpTable);
    runProgram("throw_event", testThrowEvent);

    std::printf("spu programs: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
