// Self-checking tests for the hand-written replacements of recompiled guest functions. The overrides are linked
// against fakes of the guest functions they call, so no generated tree is needed. Each ATRAC case runs in Legacy and
// in Split2, where every lv2 call the override makes must open its own kernel section; the save DLC check makes no lv2
// call and runs once.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
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
constexpr std::uint32_t kObjParent = 0x4;
constexpr std::uint32_t kObjDecoder = 0x208;
constexpr std::uint32_t kObjStop = 0x218;
constexpr std::uint32_t kObjMutex = 0x21c;
constexpr std::uint32_t kObjCond = 0x220;
constexpr std::uint32_t kObjRing = 0x224;
constexpr std::uint32_t kObjBufferSize = 0x228;
constexpr std::uint32_t kObjStreaming = 0x230;
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

    // Guest address of the stream object, for the fakes that look into it.
    std::uint32_t obj = 0;
    // fn_00955eb8 runs on the loader thread right before the loader takes the mutex.
    std::function<void()> free_space_hook;

    // imp_00c900e8 (SetData): its arguments, and the object as the call saw it.
    int set_data_calls = 0;
    std::uint64_t set_data_args[5] = {};  // r3..r7
    std::uint32_t set_data_flag = 0xffffffff;
    std::uint32_t set_data_ring_fill = 0xffffffff;
    std::uint32_t set_data_trylock = 0xffffffff;
    std::uint64_t set_data_result = 0;
    // Runs inside SetData, after the observations above.
    std::function<void()> set_data_hook;

    int release_decoder_calls = 0;  // imp_00c90108
    int teardown_end_calls = 0;     // imp_00c908e8, the last import of the function
    int unexpected_import_calls = 0;
};
FakeState g_fake;

// What the fakes called by the save DLC check (fn_00523fb8) saw and answer.
struct DlcFakeState {
    std::uint32_t items_begin = 0;  // guest address of the item id array the save's DLC set expands to
    std::uint32_t items_end = 0;
    std::uint32_t licensed_count = 2;  // fn_001da10c: number of installed DLC
    std::uint32_t unlicensed_id = 0;   // fn_00522b44 answers "not found" (the count) for this id
    int lookups = 0;
    int frees = 0;
    std::uint32_t freed = 0;
};
DlcFakeState g_dlc;

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
    if (g_fake.free_space_hook) g_fake.free_space_hook();
    ctx->gpr[3] = 0x7fffffffu;
}

// fn_00954a08: frees the staging buffer.
TOX2_PPU_FUNC(fn_00954a08) {
    (void)base;
    g_fake.free_staging_calls++;
    ctx->gpr[3] = 0;
}

// The import trampolines store the TOC pointer at r1+0x28, where the body reloads it from after the call.
#define SAVE_TOC() tox2Write64(base, static_cast<std::uint32_t>(ctx->gpr[1] + 0x28), ctx->gpr[2])

// imp_00c900e8: SetData. Records its arguments and the object, and tries the stream mutex from the calling context.
TOX2_PPU_FUNC(imp_00c900e8) {
    SAVE_TOC();
    g_fake.set_data_calls++;
    for (int i = 0; i < 5; i++) g_fake.set_data_args[i] = ctx->gpr[3 + i];
    g_fake.set_data_flag = tox2Read32(base, g_fake.obj + kObjFlag);
    g_fake.set_data_ring_fill = tox2Read32(base, g_fake.obj + kObjRingFill);
    g_fake.set_data_trylock = guestSyscall(ctx, base, 103, g_fake.mutex_id);
    if (g_fake.set_data_trylock == kCellOk) guestSyscall(ctx, base, 104, g_fake.mutex_id);
    if (g_fake.set_data_hook) g_fake.set_data_hook();
    ctx->gpr[3] = g_fake.set_data_result;
}

TOX2_PPU_FUNC(imp_00c90108) {
    SAVE_TOC();
    g_fake.release_decoder_calls++;
    ctx->gpr[3] = 0;
}

TOX2_PPU_FUNC(imp_00c908e8) {
    SAVE_TOC();
    g_fake.teardown_end_calls++;
    ctx->gpr[3] = 0;
}

// The decoder setup links against these imports too, but the cases stop before any of them is reached.
#define UNEXPECTED_IMPORT(name)           \
    TOX2_PPU_FUNC(name) {                 \
        SAVE_TOC();                       \
        g_fake.unexpected_import_calls++; \
        ctx->gpr[3] = 0;                  \
    }
UNEXPECTED_IMPORT(imp_00c90028)
UNEXPECTED_IMPORT(imp_00c90048)
UNEXPECTED_IMPORT(imp_00c90068)
UNEXPECTED_IMPORT(imp_00c90088)
UNEXPECTED_IMPORT(imp_00c900a8)
UNEXPECTED_IMPORT(imp_00c900c8)
UNEXPECTED_IMPORT(imp_00c90128)
UNEXPECTED_IMPORT(imp_00c90148)
UNEXPECTED_IMPORT(imp_00c90168)
UNEXPECTED_IMPORT(imp_00c90188)
UNEXPECTED_IMPORT(imp_00c901a8)
UNEXPECTED_IMPORT(imp_00c901c8)
UNEXPECTED_IMPORT(imp_00c901e8)

TOX2_PPU_FUNC(fn_00956a00);
TOX2_PPU_FUNC(fn_00955f50);

// Fakes of the guest functions the save DLC check calls.

// fn_005333d8: the item manager.
TOX2_PPU_FUNC(fn_005333d8) {
    (void)base;
    ctx->gpr[3] = 0x10;
}

// fn_0053c81c(mgr, vector, save): fills the vector (begin at +4, end at +8) with the ids of the save's DLC items.
TOX2_PPU_FUNC(fn_0053c81c) {
    const auto vec = static_cast<std::uint32_t>(ctx->gpr[4]);
    tox2Write32(base, vec + 4, g_dlc.items_begin);
    tox2Write32(base, vec + 8, g_dlc.items_end);
}

// fn_00522b44(out, id): the index of the installed DLC that owns the item, or the DLC count when none does.
TOX2_PPU_FUNC(fn_00522b44) {
    const auto out = static_cast<std::uint32_t>(ctx->gpr[3]);
    const auto id = static_cast<std::uint32_t>(ctx->gpr[4]);
    g_dlc.lookups++;
    tox2Write32(base, out, id == g_dlc.unlicensed_id ? g_dlc.licensed_count : 0);
}

// fn_001da10c(out): the number of installed DLC.
TOX2_PPU_FUNC(fn_001da10c) { tox2Write32(base, static_cast<std::uint32_t>(ctx->gpr[3]), g_dlc.licensed_count); }

// fn_009b4088(ptr): frees the vector's storage.
TOX2_PPU_FUNC(fn_009b4088) {
    (void)base;
    g_dlc.frees++;
    g_dlc.freed = static_cast<std::uint32_t>(ctx->gpr[3]);
}

TOX2_PPU_FUNC(fn_00523fb8);

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
    static constexpr std::uint32_t kTeardown = 0x5000;  // obj[4]: the structure the teardown walks
    static constexpr std::uint32_t kLoaderStackTop = 0xe000;
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
        g_fake.obj = at(kObj);

        write32(kObj + kObjRing, at(kRing));
        for (std::uint32_t i = 0; i < 0x100; i++) base_[scratch_ + kSrc + i] = static_cast<std::uint8_t>(0x80 + i);
        // Block align 0x2b0, little endian, in the header the decoder setup reads from the ring.
        base_[scratch_ + kRing + 0x20] = 0xb0;
        base_[scratch_ + kRing + 0x21] = 0x02;
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
    void setByteField(std::uint32_t member, std::uint8_t value) const { tox2Write8(base_, at(kObj + member), value); }

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

    // Calls fn_00955f50(obj) on a guest thread in `mode`. With `loader_n` != 0, the SetData fake starts the loader
    // override on a second guest thread with its own stack; in Split2 it first waits for that thread to reach the free
    // space call and then 20 ms more, so the loader is blocked on the mutex while SetData runs. Legacy cannot overlap
    // them, and the loader runs once the decoder releases the guest lock. The runner gives up its lock or slot before
    // joining the loader, and checks afterwards that the stream mutex is free.
    void runDecoder(tox2::PpuThreadsMode mode, std::uint32_t loader_n = 0) {
        ModeScope scope(mode);
        const bool split = tox2::splitPpuMode();
        tox2::GuestThread decoder(1002, tox2::GuestThreadParams{}, 0, 0);
        tox2::GuestThread other(1003, tox2::GuestThreadParams{}, 0, 0);
        tox2::GuestThread loader(1004, tox2::GuestThreadParams{}, 0, 0);
        std::thread loader_thread;
        std::atomic<bool> loader_at_free_space{false};

        // The stop byte ends the retry loop, so the function returns after the first SetData; the teardown completes
        // on the structure at obj[4].
        setByteField(kObjStop, 1);
        setByteField(kObjStreaming, 1);
        setField(kObjParent, at(kTeardown));
        setField(kObjDecoder, 5);

        auto acquire = [split] {
            if (split) {
                tox2::ppuSlots().acquire();
            } else {
                tox2::guestLock().lock();
            }
        };
        auto release = [split] {
            if (split) {
                tox2::ppuSlots().release();
            } else {
                tox2::guestLock().unlock();
            }
        };

        if (loader_n != 0) {
            g_fake.free_space_hook = [&] { loader_at_free_space = true; };
            g_fake.set_data_hook = [&] {
                loader_thread = std::thread([&] {
                    tox2::setGuestLockPpuPriorityForTesting(1001);
                    acquire();
                    Tox2PpuContext* ctx = &loader.context();
                    ctx->gpr[1] = at(kLoaderStackTop);
                    ctx->gpr[3] = at(kObj);
                    ctx->gpr[4] = at(kSrc);
                    ctx->gpr[5] = loader_n;
                    ctx->lr = 0;
                    fn_00956a00(ctx, base_);
                    release();
                    tox2::setGuestLockPpuPriorityForTesting(std::nullopt);
                });
                if (!split) return;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (!loader_at_free_space && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                CHECK(loader_at_free_space);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            };
        }

        std::thread runner([&] {
            tox2::setGuestLockPpuPriorityForTesting(1001);
            acquire();
            Tox2PpuContext* ctx = &decoder.context();
            ctx->gpr[1] = at(kStackTop);
            ctx->gpr[2] = 0x1234;
            ctx->gpr[3] = at(kObj);
            ctx->lr = 0;
            fn_00955f50(ctx, base_);
            release();
            if (loader_thread.joinable()) loader_thread.join();
            acquire();
            const std::uint32_t mutex_id = field(kObjMutex);
            const bool free_after = guestSyscall(&other.context(), base_, 103, mutex_id) == kCellOk;
            CHECK(free_after);
            if (free_after) guestSyscall(&other.context(), base_, 104, mutex_id);
            release();
            tox2::setGuestLockPpuPriorityForTesting(std::nullopt);
        });
        runner.join();
        g_fake.free_space_hook = nullptr;
        g_fake.set_data_hook = nullptr;
    }

    // Block align in the ATRAC header at the start of the ring, which the decoder setup copies into the flag.
    std::uint32_t ringBlockAlign() const {
        const std::uint8_t* header = base_ + at(kRing);
        return header[0x20] | (header[0x21] << 8);
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

// The decoder setup runs with a ring that already holds data.
constexpr std::uint32_t kRingFill = 0x100;
constexpr std::uint32_t kBufferSize = 0x8000;

void prepareDecoder(const LoaderEnv& env) {
    env.setField(kObjFlag, 0);
    env.setField(kObjRingFill, kRingFill);
    env.setField(kObjBufferSize, kBufferSize);
}

// SetData runs under the mutex, sees the flag still clear and the byte count it was given, and the flag is raised
// once it succeeded.
void decoderSetDataUnderLock() {
    HangGuard guard("decoderSetDataUnderLock");
    for (const tox2::PpuThreadsMode mode : kModes) {
        LoaderEnv env;
        prepareDecoder(env);
        g_fake.set_data_result = 0;

        env.runDecoder(mode);

        CHECK(g_fake.set_data_calls == 1);
        // The holder's own trylock on a non-recursive mutex fails; a success means SetData ran unlocked.
        CHECK(g_fake.set_data_trylock != kCellOk);
        CHECK(g_fake.set_data_flag == 0);
        CHECK(g_fake.set_data_ring_fill == kRingFill);
        CHECK(g_fake.set_data_args[0] == env.at(LoaderEnv::kObj) + 8);
        CHECK(g_fake.set_data_args[1] == env.at(LoaderEnv::kRing));
        CHECK(g_fake.set_data_args[2] == g_fake.set_data_ring_fill);
        CHECK(g_fake.set_data_args[3] == kBufferSize);
        CHECK(env.ringBlockAlign() == 0x2b0);
        CHECK(env.field(kObjFlag) == env.ringBlockAlign());
        CHECK(env.field(kObjRingFill) == kRingFill);
        CHECK(g_fake.release_decoder_calls == 1);
        CHECK(g_fake.teardown_end_calls == 1);
        CHECK(g_fake.unexpected_import_calls == 0);
    }
}

// A push that arrives while SetData runs waits for the mutex and then finds the flag set, so it goes to staging; the
// ring keeps the bytes SetData was given.
void decoderPushDuringSetData() {
    HangGuard guard("decoderPushDuringSetData");
    for (const tox2::PpuThreadsMode mode : kModes) {
        LoaderEnv env;
        constexpr std::uint32_t kN = 0x40;
        prepareDecoder(env);
        env.setField(kObjStaging, env.at(LoaderEnv::kStaging));
        env.setField(kObjStagingCap, 0x100);
        env.setField(kObjStagingFill, 7);
        g_fake.set_data_result = 0;

        env.runDecoder(mode, kN);

        CHECK(g_fake.set_data_calls == 1);
        CHECK(g_fake.set_data_trylock != kCellOk);
        CHECK(g_fake.copy_calls == 1);
        CHECK(g_fake.copy_dest == env.at(LoaderEnv::kStaging));
        CHECK(g_fake.copy_n == kN);
        CHECK(env.field(kObjStagingFill) == kN);
        CHECK(env.field(kObjRingFill) == kRingFill);
        CHECK(g_fake.set_data_args[2] == env.field(kObjRingFill));
        CHECK(env.field(kObjFlag) == env.ringBlockAlign());
        // The ring holds only what it held before the push.
        bool ring_untouched = true;
        for (std::uint32_t i = kRingFill; i < kRingFill + kN; i++) {
            ring_untouched = ring_untouched && env.base()[env.at(LoaderEnv::kRing) + i] == 0;
        }
        CHECK(ring_untouched);
        CHECK(std::memcmp(env.base() + env.at(LoaderEnv::kStaging), env.base() + env.at(LoaderEnv::kSrc), kN) == 0);
        CHECK(g_fake.teardown_end_calls == 1);
        CHECK(g_fake.unexpected_import_calls == 0);
    }
}

// A failed SetData ends at the stop byte without raising the flag and without a second attempt.
void decoderSetDataFailureNoFlag() {
    HangGuard guard("decoderSetDataFailureNoFlag");
    for (const tox2::PpuThreadsMode mode : kModes) {
        LoaderEnv env;
        prepareDecoder(env);
        g_fake.set_data_result = 0x80610001;

        env.runDecoder(mode);

        CHECK(g_fake.set_data_calls == 1);
        CHECK(g_fake.set_data_trylock != kCellOk);
        CHECK(env.field(kObjFlag) == 0);
        CHECK(g_fake.teardown_end_calls == 1);
        CHECK(g_fake.unexpected_import_calls == 0);
    }
}

// Runs the save DLC check with `count` DLC item ids, the one at `unlicensed` (if below `count`) owned by no installed
// DLC, and checks that it passes and frees the item vector.
void dlcCheckCase(std::uint32_t count, std::uint32_t unlicensed) {
    tox2::setLogSink([](std::string_view) {});
    CHECK(tox2::process().memory.reserve());
    std::uint8_t* base = tox2::process().memory.base();
    const std::uint32_t scratch = tox2::process().main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    tox2::GuestThread thread(1001, tox2::GuestThreadParams{}, 0, 0);
    Tox2PpuContext* ctx = &thread.context();

    g_dlc = DlcFakeState{};
    const std::uint32_t items = scratch + 0x100;
    for (std::uint32_t i = 0; i < count; i++) tox2Write32(base, items + 4 * i, 0x06000000u + i);
    g_dlc.items_begin = count != 0 ? items : 0;
    g_dlc.items_end = count != 0 ? items + 4 * count : 0;
    g_dlc.unlicensed_id = unlicensed < count ? 0x06000000u + unlicensed : 0;

    ctx->gpr[1] = scratch + 0xf000;
    ctx->gpr[3] = scratch + 0x8000;  // the save buffer; the fakes do not read it
    ctx->lr = 0;
    fn_00523fb8(ctx, base);

    CHECK(ctx->gpr[3] == 1);
    CHECK(ctx->gpr[1] == scratch + 0xf000);
    CHECK(g_dlc.frees == (count != 0 ? 1 : 0));
    if (count != 0) CHECK(g_dlc.freed == items);
    // The original stops at the first unlicensed item.
    CHECK(g_dlc.lookups == static_cast<int>(unlicensed < count ? unlicensed + 1 : count));

    tox2::process().main.free(scratch);
    tox2::setLogSink({});
}

// The save DLC check passes for an empty set, a fully licensed set and a set with an item of DLC that is not
// installed (the original returns 0 there), for counts that take each of the unrolled loop's paths.
void dlcCheckAlwaysPasses() {
    dlcCheckCase(0, 0);
    for (std::uint32_t count : {1u, 2u, 3u, 4u, 5u, 9u}) {
        dlcCheckCase(count, count);      // all licensed
        dlcCheckCase(count, 0);          // the first is not
        dlcCheckCase(count, count - 1);  // the last is not
    }
}

}  // namespace

int main() {
    loaderDirectUnderLock();
    loaderStagingPath();
    decoderSetDataUnderLock();
    decoderPushDuringSetData();
    decoderSetDataFailureNoFlag();
    dlcCheckAlwaysPasses();
    std::printf("ppu overrides tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}
