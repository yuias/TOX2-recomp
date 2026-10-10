#pragma once

// SPU thread core: one SpuThread per SPU, with its local store (LS), context and image tracking.
// Nothing here knows about SPURS or lv2 thread groups; whoever runs the thread supplies a SpuOwner.
//
// An SPU thread runs outside the guest lock, in parallel with the PPU threads. It must never take the
// guest lock except around calls into lv2 state, and never while it waits on a channel.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "tox2/spu_context.h"

namespace tox2 {

class SpuThread;

// Why runSpu() returned.
enum class SpuLeave {
    Exit,      // a dispatch reached exit_ls
    Halt,      // a halt was requested
    Returned,  // generated code returned: a channel or stop hook asked the thread to leave
};

// What the thread's runner reports to. A raw SPU or a test harness can implement it as well as an lv2
// SPU thread group. Callbacks run on the SPU's host thread, without the guest lock.
class SpuOwner {
public:
    virtual ~SpuOwner() = default;
    // WrOutIntrMbox wrote `value`.
    virtual void onInterruptMailbox(SpuThread& thread, std::uint32_t value) = 0;
    // A `stop` instruction at `site`. True: continue at site + 4; false: the thread leaves.
    virtual bool onStop(SpuThread& thread, std::uint32_t site, std::uint32_t code) = 0;
    // The host thread is about to end. The thread object must not be destroyed from this callback.
    virtual void onFinished(SpuThread& thread, SpuLeave how) = 0;
};

enum class SpuState { Created, Running, Blocked, Finished };

// Image tracking (see copyToLs): the LS map has one 16-bit tag per 16 bytes. 0 is untracked, otherwise
// the tag minus one indexes the thread's load records.
inline constexpr std::uint32_t kSpuTagUnit = 16;
inline constexpr std::uint32_t kSpuTagCount = kTox2SpuLsSize / kSpuTagUnit;
inline constexpr std::uint32_t kSpuMaxLoadRecords = 255;
inline constexpr std::uint32_t kMaxSpuImages = 64;

// "Image `image` (an index into the registered images) occupies this LS range through its code
// segment `segment`, loaded `delta` bytes away from the segment's link address".
struct SpuLoadRecord {
    std::uint32_t image = 0;
    std::uint32_t segment = 0;
    std::int32_t delta = 0;
};

class SpuThread {
public:
    // `id` is the lv2 SPU thread id (0 outside lv2); it appears in stop lines. `owner` may be null.
    SpuThread(std::uint32_t id, std::string name, SpuOwner* owner);
    // Requests a halt and joins the host thread if it is still running.
    ~SpuThread();
    SpuThread(const SpuThread&) = delete;
    SpuThread& operator=(const SpuThread&) = delete;

    std::uint32_t id() const { return id_; }
    const std::string& name() const { return name_; }
    SpuOwner* owner() const { return owner_; }
    Tox2SpuContext& context() { return ctx_; }
    const Tox2SpuContext& context() const { return ctx_; }
    std::uint8_t* ls() { return ls_.get(); }
    const std::uint8_t* ls() const { return ls_.get(); }

    // Starts the host thread at `entry_ls`. False when it was started already.
    bool start(std::uint32_t entry_ls);
    // Waits for the host thread (a no-op when none runs).
    void join();

    // Asks the thread to leave at its next check. Any thread may call it. The flag is stored atomically
    // and the reservation waiters are woken, so a thread that waits for a reservation event notices at
    // once. Waits on the thread's own channels are woken by the channel layer.
    void requestHalt();
    bool haltRequested() const { return tox2SpuHaltRequested(&ctx_); }

    // Health record for the supervisor.
    SpuState state() const { return state_.load(); }
    const char* blockedIn() const { return blocked_in_.load(); }
    bool indefiniteWait() const { return indefinite_.load(); }
    // `what` must outlive the wait (a string literal).
    void setBlocked(const char* what, bool indefinite) {
        blocked_in_.store(what);
        indefinite_.store(indefinite);
        state_.store(SpuState::Blocked);
    }
    void setRunning() { state_.store(SpuState::Running); }

    // Census counters; written by the owning host thread only.
    std::uint64_t dispatchCount() const { return dispatches_.load(std::memory_order_relaxed); }
    std::uint64_t imageDispatchCount(std::uint32_t image) const {
        return image < kMaxSpuImages ? image_dispatches_[image].load(std::memory_order_relaxed) : 0;
    }
    void countDispatch(std::uint32_t image) {
        dispatches_.store(dispatches_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        std::atomic<std::uint64_t>& n = image_dispatches_[image];
        n.store(n.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    // The load record that covers the 16-byte unit of `lsa`, or null when it is untracked.
    const SpuLoadRecord* recordAt(std::uint32_t lsa) const {
        const std::uint16_t tag = tags_[(lsa & (kTox2SpuLsSize - 1u)) / kSpuTagUnit].load(std::memory_order_acquire);
        return tag == 0 ? nullptr : &records_[tag - 1u];
    }

    // Tracking internals for copyToLs and untagLs; other code goes through those two.
    // The tag (record index + 1) for this triple; adds the record when it is new.
    std::uint16_t recordTag(std::uint32_t image, std::uint32_t segment, std::int32_t delta);
    // Tag units [first_unit, last_unit) with `tag` (0 untags).
    void setTags(std::uint32_t first_unit, std::uint32_t last_unit, std::uint16_t tag);

private:
    struct LsDeleter {
        void operator()(std::uint8_t* p) const;
    };

    void threadMain(std::uint32_t entry_ls);

    std::uint32_t id_;
    std::string name_;
    SpuOwner* owner_;
    alignas(16) Tox2SpuContext ctx_{};
    std::unique_ptr<std::uint8_t[], LsDeleter> ls_;

    // Tags may be changed by another thread (write_ls from the PPU) while this one dispatches. Records are
    // only ever appended, so a tag that is visible always names a complete record.
    std::array<std::atomic<std::uint16_t>, kSpuTagCount> tags_{};
    std::array<SpuLoadRecord, kSpuMaxLoadRecords> records_{};
    std::uint32_t record_count_ = 0;  // guarded by record_mutex_
    std::mutex record_mutex_;

    std::atomic<SpuState> state_{SpuState::Created};
    std::atomic<const char*> blocked_in_{nullptr};
    std::atomic<bool> indefinite_{false};
    std::atomic<std::uint64_t> dispatches_{0};
    std::array<std::atomic<std::uint64_t>, kMaxSpuImages> image_dispatches_{};

    std::thread thread_;
};

// The SPU thread running on the calling host thread; null on other threads.
SpuThread* currentSpuThread();

// The thread behind a generated function's context.
inline SpuThread* spuThreadOf(Tox2SpuContext* ctx) { return static_cast<SpuThread*>(ctx->runtime); }

// Runs the thread on the calling host thread from `entry_ls` until generated code returns: a leave, a
// halt request or a dispatch to exit_ls. SpuThread::start() calls it on the thread's own host thread.
SpuLeave runSpu(SpuThread& thread, std::uint32_t entry_ls);

// Copies guest bytes [ea, ea + size) to LS at `lsa` (wrapping at the LS size) and updates the image
// tracking: the 16-byte units that lie wholly inside the intersection of the copy with a code segment of
// a registered image are tagged with that image; every other unit the copy touches is untagged. The MFC
// GET and the lv2 segment loads copy through here.
void copyToLs(SpuThread& thread, const std::uint8_t* base, std::uint32_t ea, std::uint32_t lsa, std::uint32_t size);

// Untags every 16-byte unit that [lsa, lsa + size) touches. Any LS write that does not come from a
// tracked copy (GET from other memory, GETLLAR, FILL segments, write_ls) calls it.
void untagLs(SpuThread& thread, std::uint32_t lsa, std::uint32_t size);

// What the tracking says about `lsa`: the image and the record, or nullopt when untracked.
struct SpuTracked {
    const Tox2SpuImageInfo* image;
    std::uint32_t image_index;
    std::uint32_t segment;
    std::int32_t delta;
};
std::optional<SpuTracked> trackedImageAt(const SpuThread& thread, std::uint32_t lsa);

// Ends the process with `line` as the stop line (exit code Fatal), followed by `detail` when non-empty,
// the thread's registers, trace ring and LS map. For fatal conditions on an SPU thread.
[[noreturn]] void spuStop(const SpuThread& thread, std::string_view line, std::string_view detail = {});

// The text that spuStop prints after the stop line.
std::string describeSpuThread(const SpuThread& thread);

// Image registry. The tables must stay valid for the rest of the process. Registering replaces the
// previous list; do it before any SPU thread exists. A malformed table ends the process with
// ExitCode::LoadError.
void registerSpuImages(const Tox2SpuImageInfo* images, std::uint32_t count);
std::uint32_t spuImageCount();
const Tox2SpuImageInfo& spuImage(std::uint32_t index);
// The dispatchable entry of image `index` at `ls`, or null.
const Tox2SpuEntry* spuFindEntry(std::uint32_t index, std::uint32_t ls);
// True the first time it is called for the image since registration (for the "first image" log line).
bool spuMarkImageSeen(std::uint32_t index);

// Compares the SHA-256 of every registered image with the bytes in guest memory and logs the result.
// False on a mismatch (the caller ends the run with ExitCode::LoadError).
bool checkSpuImages(const std::uint8_t* base);

}  // namespace tox2
