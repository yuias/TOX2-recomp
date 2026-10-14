// Self-checking tests for the hand-written replacements of recompiled guest functions. The overrides are linked
// against fakes of the guest functions they call, so no generated tree is needed. Each case runs in Legacy and in
// Split2, where every lv2 call the override makes must open its own kernel section.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>

#include "core/guest_memory.h"
#include "core/log.h"
#include "core/threads.h"
#include "lv2/sys_sync.h"
#include "tox2/ppu_context.h"

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

constexpr std::uint32_t kCellOk = 0;
constexpr std::uint32_t kSyncPriority = 2;
constexpr std::uint32_t kSyncNotRecursive = 0x20;

// obj layout of the ATRAC stream object (guest offsets).
constexpr std::uint32_t kObjMutex = 0x21c;
constexpr std::uint32_t kObjCond = 0x220;
constexpr std::uint32_t kObjRing = 0x224;
constexpr std::uint32_t kObjRingFill = 0x234;
constexpr std::uint32_t kObjFlag = 0x244;
constexpr std::uint32_t kObjStaging = 0x24c;
constexpr std::uint32_t kObjStagingCap = 0x250;
constexpr std::uint32_t kObjStagingFill = 0x254;

// What the fakes of the guest functions saw. Only the one guest thread of a case runs at a time.
struct FakeState {
    std::uint32_t mutex_id = 0;
    int copy_calls = 0;
    std::uint32_t copy_dest = 0;
    std::uint32_t copy_n = 0;
    // Result of sys_mutex_trylock on the stream mutex from inside the copy.
    std::uint32_t copy_trylock = 0xffffffff;
    int free_staging_calls = 0;
};
FakeState g_fake;

// Runs a lv2 call as the guest would: number in r11, arguments in r3..r5.
std::uint32_t guestSyscall(Tox2PpuContext* ctx, std::uint8_t* base, std::uint32_t number, std::uint64_t r3,
                           std::uint64_t r4 = 0, std::uint64_t r5 = 0) {
    ctx->gpr[11] = number;
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    ctx->gpr[5] = r5;
    tox2Syscall(ctx, base, 0x10000);
    return static_cast<std::uint32_t>(ctx->gpr[3]);
}

}  // namespace

// Fakes of the guest functions that the loader override calls. They keep the callee-saved registers.
extern "C" {

// fn_00985a08(dest, src, n): a memcpy in guest memory that records where the bytes went and whether the stream mutex
// was free, as seen by the copying thread itself. A trylock that succeeds leaves no hold behind.
TOX2_PPU_FUNC(fn_00985a08) {
    const auto dest = static_cast<std::uint32_t>(ctx->gpr[3]);
    const auto src = static_cast<std::uint32_t>(ctx->gpr[4]);
    const auto n = static_cast<std::uint32_t>(ctx->gpr[5]);
    g_fake.copy_calls++;
    g_fake.copy_dest = dest;
    g_fake.copy_n = n;
    std::memcpy(base + dest, base + src, n);
    g_fake.copy_trylock = guestSyscall(ctx, base, 103, g_fake.mutex_id);
    if (g_fake.copy_trylock == kCellOk) guestSyscall(ctx, base, 104, g_fake.mutex_id);
    ctx->gpr[3] = dest;
}

// fn_00955eb8: free space in the ring; always enough.
TOX2_PPU_FUNC(fn_00955eb8) {
    (void)base;
    ctx->gpr[3] = 0x7fffffffu;
}

// fn_00954a08: frees the staging buffer.
TOX2_PPU_FUNC(fn_00954a08) {
    (void)base;
    g_fake.free_staging_calls++;
    ctx->gpr[3] = 0;
}

TOX2_PPU_FUNC(fn_00956a00);

}  // extern "C"

namespace {

// Ends the process when a case hangs, so a broken lock order fails the run instead of blocking it.
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

// Selects a PPU thread mode for a case and restores Legacy at the end.
class ModeScope {
public:
    explicit ModeScope(tox2::PpuThreadsMode mode) { tox2::setPpuThreadsMode(mode); }
    ~ModeScope() { tox2::setPpuThreadsMode(tox2::PpuThreadsMode::Legacy); }
    ModeScope(const ModeScope&) = delete;
    ModeScope& operator=(const ModeScope&) = delete;
};

// Guest memory for one case: the ATRAC stream object with its mutex (created through sc 100 with the attributes the
// game uses) and condition variable (sc 105), a ring, a source buffer, a staging buffer and a stack.
class LoaderEnv {
public:
    static constexpr std::uint32_t kObj = 0x1000;
    static constexpr std::uint32_t kRing = 0x2000;
    static constexpr std::uint32_t kSrc = 0x3000;
    static constexpr std::uint32_t kStaging = 0x4000;
    static constexpr std::uint32_t kStackTop = 0xf000;

    LoaderEnv() : setup_(1001, tox2::GuestThreadParams{}, 0, 0) {
        tox2::setLogSink([](std::string_view) {});
        tox2::resetSyncObjects();
        CHECK(tox2::process().memory.reserve());
        base_ = tox2::process().memory.base();
        scratch_ = tox2::process().main.allocate(0x10000, 0x10000);
        CHECK(scratch_ != 0);
        g_fake = FakeState{};

        // Legacy: the caller holds the one lock for every guest call.
        tox2::guestLock().lock();
        Tox2PpuContext* ctx = &setup_.context();
        write32(0x100, kSyncPriority);
        write32(0x104, kSyncNotRecursive);
        CHECK(guestSyscall(ctx, base_, 100, at(0x110), at(0x100)) == kCellOk);
        write32(kObj + kObjMutex, read32(0x110));
        CHECK(guestSyscall(ctx, base_, 105, at(0x114), read32(0x110), at(0x100)) == kCellOk);
        write32(kObj + kObjCond, read32(0x114));
        tox2::guestLock().unlock();
        g_fake.mutex_id = read32(kObj + kObjMutex);

        write32(kObj + kObjRing, at(kRing));
        for (std::uint32_t i = 0; i < 0x100; i++) base_[scratch_ + kSrc + i] = static_cast<std::uint8_t>(0x80 + i);
    }
    ~LoaderEnv() {
        tox2::process().main.free(scratch_);
        tox2::resetSyncObjects();
        tox2::setLogSink({});
    }
    LoaderEnv(const LoaderEnv&) = delete;
    LoaderEnv& operator=(const LoaderEnv&) = delete;

    std::uint8_t* base() const { return base_; }
    std::uint32_t at(std::uint32_t offset) const { return scratch_ + offset; }
    std::uint32_t read32(std::uint32_t offset) const { return tox2Read32(base_, at(offset)); }
    void write32(std::uint32_t offset, std::uint32_t value) const { tox2Write32(base_, at(offset), value); }
    // Field of the stream object.
    std::uint32_t field(std::uint32_t member) const { return read32(kObj + member); }
    void setField(std::uint32_t member, std::uint32_t value) const { write32(kObj + member, value); }

    // Calls fn_00956a00(obj, src, n) on a guest thread in `mode` and returns its r3. Checks that the stream mutex is
    // free afterwards, as seen by a second guest thread.
    std::uint32_t runLoader(tox2::PpuThreadsMode mode, std::uint32_t n) {
        ModeScope scope(mode);
        const bool split = tox2::splitPpuMode();
        tox2::GuestThread loader(1002, tox2::GuestThreadParams{}, 0, 0);
        tox2::GuestThread other(1003, tox2::GuestThreadParams{}, 0, 0);
        std::uint32_t result = 0;
        std::thread runner([&] {
            tox2::setGuestLockPpuPriorityForTesting(1001);
            if (split) {
                tox2::ppuSlots().acquire();
            } else {
                tox2::guestLock().lock();
            }
            Tox2PpuContext* ctx = &loader.context();
            ctx->gpr[1] = at(kStackTop);
            ctx->gpr[3] = at(kObj);
            ctx->gpr[4] = at(kSrc);
            ctx->gpr[5] = n;
            ctx->lr = 0;
            fn_00956a00(ctx, base_);
            result = static_cast<std::uint32_t>(ctx->gpr[3]);
            const std::uint32_t mutex_id = field(kObjMutex);
            const bool free_after = guestSyscall(&other.context(), base_, 103, mutex_id) == kCellOk;
            CHECK(free_after);
            if (free_after) guestSyscall(&other.context(), base_, 104, mutex_id);
            if (split) {
                tox2::ppuSlots().release();
            } else {
                tox2::guestLock().unlock();
            }
            tox2::setGuestLockPpuPriorityForTesting(std::nullopt);
        });
        runner.join();
        return result;
    }

private:
    std::uint8_t* base_ = nullptr;
    std::uint32_t scratch_ = 0;
    tox2::GuestThread setup_;
};

constexpr tox2::PpuThreadsMode kModes[] = {tox2::PpuThreadsMode::Legacy, tox2::PpuThreadsMode::Split2};

// Flag 0: the bytes go straight into the ring at the current fill, under the mutex, and the count grows by n.
void loaderDirectUnderLock() {
    HangGuard guard("loaderDirectUnderLock");
    for (const tox2::PpuThreadsMode mode : kModes) {
        LoaderEnv env;
        constexpr std::uint32_t kOldFill = 0x30;
        constexpr std::uint32_t kN = 0x40;
        env.setField(kObjFlag, 0);
        env.setField(kObjRingFill, kOldFill);

        const std::uint32_t result = env.runLoader(mode, kN);

        CHECK(g_fake.copy_calls == 1);
        CHECK(g_fake.copy_dest == env.at(LoaderEnv::kRing) + kOldFill);
        CHECK(g_fake.copy_n == kN);
        // The holder's own trylock on a non-recursive mutex fails; a success means the copy ran unlocked.
        CHECK(g_fake.copy_trylock != kCellOk);
        CHECK(env.field(kObjRingFill) == kOldFill + kN);
        CHECK(env.field(kObjFlag) == 0);
        CHECK(result == kN);
        CHECK(std::memcmp(env.base() + env.at(LoaderEnv::kRing) + kOldFill, env.base() + env.at(LoaderEnv::kSrc), kN) ==
              0);
        CHECK(g_fake.free_staging_calls == 0);
    }
}

// Flag != 0 and a staging buffer that is large enough: the bytes go to staging while the mutex is free, the staged
// count is set to n and the ring count stays.
void loaderStagingPath() {
    HangGuard guard("loaderStagingPath");
    for (const tox2::PpuThreadsMode mode : kModes) {
        LoaderEnv env;
        constexpr std::uint32_t kRingFill = 0x30;
        constexpr std::uint32_t kN = 0x40;
        env.setField(kObjFlag, 1);
        env.setField(kObjRingFill, kRingFill);
        env.setField(kObjStaging, env.at(LoaderEnv::kStaging));
        env.setField(kObjStagingCap, 0x100);
        env.setField(kObjStagingFill, 7);

        const std::uint32_t result = env.runLoader(mode, kN);

        CHECK(g_fake.copy_calls == 1);
        CHECK(g_fake.copy_dest == env.at(LoaderEnv::kStaging));
        CHECK(g_fake.copy_n == kN);
        // The staging copy takes its own short lock afterwards; during the copy the mutex is free.
        CHECK(g_fake.copy_trylock == kCellOk);
        CHECK(env.field(kObjStagingCap) == kN);
        CHECK(env.field(kObjStagingFill) == kN);
        CHECK(env.field(kObjRingFill) == kRingFill);
        CHECK(result == kN);
        CHECK(std::memcmp(env.base() + env.at(LoaderEnv::kStaging), env.base() + env.at(LoaderEnv::kSrc), kN) == 0);
        CHECK(g_fake.free_staging_calls == 0);
    }
}

}  // namespace

int main() {
    loaderDirectUnderLock();
    loaderStagingPath();
    std::printf("ppu overrides tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}
