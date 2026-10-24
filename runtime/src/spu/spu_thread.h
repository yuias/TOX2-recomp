#pragma once

// SPU thread core: one SpuThread per SPU, with its local store (LS), context and image tracking.
// Nothing here knows about SPURS or lv2 thread groups; whoever runs the thread supplies a SpuOwner.
//
// An SPU thread runs outside the guest lock, in parallel with the PPU threads. It must never take the
// kernel lock except around calls into lv2 state, and never while it waits on a channel.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/latency_stats.h"
#include "tox2/spu_context.h"

namespace tox2 {

// Threshold of the receive statistics: one cellAudio period.
inline constexpr std::uint64_t kSpuReceiveOverUs = 5333;

class SpuChannels;
class SpuThread;

// Why runSpu() returned.
enum class SpuLeave {
    Exit,      // a dispatch reached exit_ls
    Halt,      // a halt was requested
    Returned,  // generated code returned: a channel or stop hook asked the thread to leave
};

// What the thread's runner reports to. A raw SPU or a test harness can implement it as well as an lv2
// SPU thread group. Callbacks run on the SPU's host thread, without the kernel lock.
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
// The summary of the tag map: one bit per kSpuTagBlock bytes of LS (see SpuThread::taggedBlocks).
inline constexpr std::uint32_t kSpuTagBlock = 1024;
inline constexpr std::uint32_t kSpuTagBlockUnits = kSpuTagBlock / kSpuTagUnit;
inline constexpr std::uint32_t kSpuTagBlockCount = kTox2SpuLsSize / kSpuTagBlock;
inline constexpr std::uint32_t kSpuTagBlockWords = kSpuTagBlockCount / 64;
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
    // Mailboxes, events, decrementer and the MFC (spu_channels.h).
    SpuChannels& channels() { return *channels_; }

    // Raises the host thread to HighPrioritySpu (core/host_priority.h) when it starts; call before start().
    void setRaiseHostPriority(bool raise) { raise_host_priority_ = raise; }
    // The host thread's priority as it settled at the top of threadMain, in currentHostThreadPriority's terms (a
    // thread that was not raised keeps the one it started with); 0 until then.
    int hostPriority() const { return host_priority_.load(std::memory_order_acquire); }

    // Starts the host thread at `entry_ls`. False when it was started already.
    bool start(std::uint32_t entry_ls);
    // Waits for the host thread (a no-op when none runs).
    void join();

    // Asks the thread to leave at its next check. Any thread may call it. The flag is stored atomically
    // and both the reservation waiters and the thread's channel waits are woken, so a waiting thread
    // notices at once.
    void requestHalt();
    bool haltRequested() const { return tox2SpuHaltRequested(&ctx_); }

    // Health record for the supervisor.
    SpuState state() const { return state_.load(); }
    const char* blockedIn() const { return blocked_in_.load(); }
    bool indefiniteWait() const { return indefinite_.load(); }
    // Milliseconds on the steady clock at which the current wait (or the current kind of wait) began; the
    // same clock the PPU threads' health uses.
    std::int64_t blockedSinceMs() const { return blocked_since_ms_.load(); }
    // `what` must outlive the wait (a string literal). Calling it again for the same wait keeps the start
    // time; changing `indefinite` starts a new one.
    void setBlocked(const char* what, bool indefinite) {
        if (state_.load() != SpuState::Blocked || indefinite_.load() != indefinite) {
            blocked_since_ms_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count());
        }
        blocked_in_.store(what);
        indefinite_.store(indefinite);
        state_.store(SpuState::Blocked);
    }
    void setRunning() { state_.store(SpuState::Running); }

    // Census counters; written by the owning host thread only (relaxed).
    std::uint64_t dispatchCount() const { return dispatches_.load(std::memory_order_relaxed); }
    std::uint64_t imageDispatchCount(std::uint32_t image) const {
        return image < kMaxSpuImages ? image_dispatches_[image].load(std::memory_order_relaxed) : 0;
    }
    // One dispatch of `image` (an index into the registered images) to LS address `target`.
    void countDispatch(std::uint32_t image, std::uint32_t target) {
        dispatches_.store(dispatches_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        std::atomic<std::uint64_t>& n = image_dispatches_[image];
        n.store(n.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        last_image_.store(image + 1, std::memory_order_relaxed);
        last_target_.store(target, std::memory_order_relaxed);
    }
    // The image and LS address of the latest dispatch; the image is nullopt before the first one.
    std::optional<std::uint32_t> lastDispatchImage() const {
        const std::uint32_t v = last_image_.load(std::memory_order_relaxed);
        return v == 0 ? std::nullopt : std::optional<std::uint32_t>(v - 1);
    }
    std::uint32_t lastDispatchTarget() const { return last_target_.load(std::memory_order_relaxed); }
    // Dispatches that stopped at an unknown target; counted just before the stop.
    void countUnknownTarget() { unknown_targets_.fetch_add(1, std::memory_order_relaxed); }
    std::uint64_t unknownTargetCount() const { return unknown_targets_.load(std::memory_order_relaxed); }
    // `stop` instructions by code, counted before the owner decides what they mean.
    void countStop(std::uint32_t code);
    std::map<std::uint32_t, std::uint64_t> stopCounts() const;

    // Timing of the blocking receive (stop 0x110), recorded by this thread's own host thread. busy: from the reply
    // of one receive to the arrival of the next, the SPU's wall time per event (compare with kSpuReceiveOverUs);
    // wait: from arrival to reply, the idle time for the event plus the kernel-lock wait for the reply.
    LatencySnapshot receiveBusy() const { return receive_busy_.snapshot(); }
    LatencySnapshot receiveWait() const { return receive_wait_.snapshot(); }
    void noteReceiveArrival(std::chrono::steady_clock::time_point arrival) {
        if (last_receive_return_) receive_busy_.record(elapsedUs(*last_receive_return_, arrival));
    }
    void noteReceiveReply(std::chrono::steady_clock::time_point arrival, std::chrono::steady_clock::time_point reply) {
        receive_wait_.record(elapsedUs(arrival, reply));
        last_receive_return_ = reply;
    }
    // The receive ended without a reply (teardown): the next arrival has no previous reply to measure from.
    void noteReceiveAbort() { last_receive_return_.reset(); }

    // The load record that covers the 16-byte unit of `lsa`, or null when it is untracked.
    const SpuLoadRecord* recordAt(std::uint32_t lsa) const {
        const std::uint16_t tag = tags_[(lsa & (kTox2SpuLsSize - 1u)) / kSpuTagUnit].load(std::memory_order_acquire);
        return tag == 0 ? nullptr : &records_[tag - 1u];
    }

    // Whether tag unit `unit` (LS offset / kSpuTagUnit, below kSpuTagCount) is tagged; no record lookup.
    bool unitTagged(std::uint32_t unit) const { return tags_[unit].load(std::memory_order_relaxed) != 0; }

    // Tracking internals for copyToLs and untagLs; other code goes through those two.
    // The tag (record index + 1) for this triple; adds the record when it is new.
    std::uint16_t recordTag(std::uint32_t image, std::uint32_t segment, std::int32_t delta);
    // Tag units [first_unit, last_unit) with `tag` (0 untags). Keeps the block summary up to date.
    void setTags(std::uint32_t first_unit, std::uint32_t last_unit, std::uint16_t tag);
    // False when no unit of LS block `block` (kSpuTagBlock bytes) is tagged, so a walk over it can be skipped.
    bool blockMayBeTagged(std::uint32_t block) const {
        const std::uint64_t word = tagged_blocks_[block / 64].load(std::memory_order_acquire);
        return ((word >> (block % 64)) & 1u) != 0;
    }
    // Clears the summary bit of `block`; the caller has seen every unit of the block untagged.
    void clearBlockMayBeTagged(std::uint32_t block) {
        tagged_blocks_[block / 64].fetch_and(~(std::uint64_t{1} << (block % 64)), std::memory_order_release);
    }
    // The summary words, bit b of word b / 64 for block b (test accessor).
    std::array<std::uint64_t, kSpuTagBlockWords> taggedBlocks() const;

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
    std::unique_ptr<SpuChannels> channels_;

    // Tags may be changed by another thread (write_ls from the PPU) while this one dispatches. Records are
    // only ever appended, so a tag that is visible always names a complete record.
    std::array<std::atomic<std::uint16_t>, kSpuTagCount> tags_{};
    // Bit b of word b / 64 is set when a unit of LS block b may be tagged. It is set before the tags are stored and
    // cleared only when every unit of the block is known to be 0, so a set bit with no tagged unit costs a walk and
    // never a wrong answer. Non-zero tags come from this thread's own DMA and from image loads while the thread does
    // not run; other threads only untag (write_ls, at most 8 bytes), so they never cover a whole block and never
    // clear a bit, and an untag racing with a walk can only leave a bit set. A whole-block untag from another thread
    // racing with this thread's tagging could leave a tag with its bit clear.
    std::array<std::atomic<std::uint64_t>, kSpuTagBlockWords> tagged_blocks_{};
    std::array<SpuLoadRecord, kSpuMaxLoadRecords> records_{};
    std::uint32_t record_count_ = 0;  // guarded by record_mutex_
    std::mutex record_mutex_;

    bool raise_host_priority_ = false;  // set before start(), read by the host thread
    std::atomic<int> host_priority_{0};
    std::atomic<SpuState> state_{SpuState::Created};
    std::atomic<const char*> blocked_in_{nullptr};
    std::atomic<bool> indefinite_{false};
    std::atomic<std::int64_t> blocked_since_ms_{0};
    std::atomic<std::uint64_t> dispatches_{0};
    std::array<std::atomic<std::uint64_t>, kMaxSpuImages> image_dispatches_{};
    std::atomic<std::uint32_t> last_image_{0};  // image index + 1, 0 before the first dispatch
    std::atomic<std::uint32_t> last_target_{0};
    std::atomic<std::uint64_t> unknown_targets_{0};
    mutable std::mutex stop_mutex_;
    std::map<std::uint32_t, std::uint64_t> stops_;  // guarded by stop_mutex_

    LatencyStats receive_busy_{kSpuReceiveOverUs};
    LatencyStats receive_wait_{kSpuReceiveOverUs};
    std::optional<std::chrono::steady_clock::time_point> last_receive_return_;  // own host thread only

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
// a registered image are tagged with that image. Any other tagged unit the copy touches is untagged,
// unless the copy writes the bytes it already holds (for a partial edge unit, only the bytes written are
// compared). The MFC GET and the lv2 segment loads copy through here.
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
// True when guest bytes [ea, ea + size) intersect the span from the lowest code segment start to the highest code
// segment end of the registered images. False means a copy from there tags nothing.
bool spuCodeRangeOverlaps(std::uint32_t ea, std::uint32_t size);
const Tox2SpuImageInfo& spuImage(std::uint32_t index);
// The dispatchable entry of image `index` at `ls`, or null.
const Tox2SpuEntry* spuFindEntry(std::uint32_t index, std::uint32_t ls);
// True the first time it is called for the image since registration (for the "first image" log line).
bool spuMarkImageSeen(std::uint32_t index);
// Census: records a dispatch to entry `entry_index` of image `index`; the bitmap is shared by all threads, so
// restarts and replaced threads keep their entries. Counts each entry once.
void spuMarkEntryDispatched(std::uint32_t index, std::uint32_t entry_index);
std::uint32_t spuDistinctEntries(std::uint32_t index);

// Compares the SHA-256 of every registered image with the bytes in guest memory and logs the result.
// False on a mismatch (the caller ends the run with ExitCode::LoadError).
bool checkSpuImages(const std::uint8_t* base);

}  // namespace tox2
