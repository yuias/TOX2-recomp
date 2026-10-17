#pragma once

// The fake RSX command consumer: a host thread that feeds the FIFO parser, executes the
// synchronisation methods (labels, reports, reference, flips, user commands) and counts every
// register write for the census. Nothing is drawn; with the frame builder enabled the writes also turn
// into render commands (rsx/rsx_frame_builder.h). Method semantics follow RPCS3
// `Emu/RSX/NV47/HW/nv406e.cpp`, `nv4097.cpp`, `nv47_sync.hpp`, `rsx_methods.cpp` (`flip_command`,
// `user_command`) and `RSXThread.cpp` (`get_address`, `handle_emu_flip`, `post_vblank_event`).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/census.h"
#include "rsx/fifo_parser.h"
#include "rsx/rsx_capture.h"
#include "rsx/rsx_census.h"
#include "rsx/rsx_frame.h"
#include "rsx/rsx_frame_builder.h"
#include "rsx/rsx_registers.h"
#include "rsx/rsx_state.h"

namespace tox2 {

enum class RsxInterrupt { VBlank, Flip, User };

// Byte offset inside the destination DMA object of word `index` of an NV308A inline blit. 64-bit so a
// hostile offset cannot wrap into a valid range.
constexpr std::uint64_t rsxInlineBlitOffset(std::uint32_t offset, std::uint32_t pitch, std::uint32_t point_x,
                                            std::uint32_t point_y, std::uint32_t index) {
    return static_cast<std::uint64_t>(offset) + (static_cast<std::uint64_t>(point_x) + index) * 4 +
           static_cast<std::uint64_t>(pitch) * point_y;
}

// Called by the RSX thread with the kernel lock held. `arg` is the head (1) for VBlank and Flip and
// the cause for User. No-op while unset.
void setRsxInterruptHook(void (*hook)(RsxInterrupt kind, std::uint32_t arg));

// One VBlank every 1/60 s (RPCS3's default VBlank rate).
inline constexpr std::uint32_t kRsxVBlankPeriodUs = 16667;

// Flips completed so far by the RSX thread. After the Nth flip it is N, which is the number in
// `frame_<N>.png` of --dump-frames and the N of --rsx-capture. Any thread.
std::uint64_t rsxFlipCount();
// Blocks until rsxFlipCount() >= n; returns at once when it already is. Any thread.
void waitRsxFlipCount(std::uint64_t n);

// Wall time of the RSX consumer thread by activity since start, in microseconds. Written by the RSX thread with
// relaxed atomics; read from any thread.
struct RsxThreadTimes {
    std::uint64_t idle_us = 0;       // sleeping between passes
    std::uint64_t lock_wait_us = 0;  // waiting for the kernel lock
    std::uint64_t pump_us = 0;       // holding it: pump slices and vblank ticks
    std::uint64_t build_us = 0;      // takeCommands(): the pending draw's copies, outside the lock
    std::uint64_t record_us = 0;     // dispatchCommands() and capture writes: renderer recording, submit
};
RsxThreadTimes rsxThreadTimes();
// "rsx busy 12.80 ms/flip: lock 2.09 pump 0.79 build 4.83 record 5.09, idle 3.89" from the difference of two
// snapshots and the flips between them (busy = lock + pump + build + record); empty when flips is 0.
std::string formatRsxBusy(const RsxThreadTimes& delta, std::uint64_t flips);

// Interval between completed flips in the Hz60 flip limit: RPCS3's default "Auto" frame limit (60.000 fps).
inline constexpr std::chrono::nanoseconds kRsxFlipPeriod{1'000'000'000 / 60};

// How completing flip commands are paced.
enum class RsxFlipLimit {
    // RPCS3's "PS3 Native" frame limit: a flip in the game's VSYNC flip mode completes at the first vblank tick after
    // its command executes, at most one per tick, with no wall-clock deadline. A flip in the HSYNC flip mode uses the
    // Hz60 limiter.
    Vsync,
    // RPCS3's default "Auto" frame limit: at most 60 flips per second, whatever the flip mode.
    Hz60,
    // Every flip completes where its command executes.
    Off,
};

// The command-line form of the limit (--frame-limit, config key frame_limit).
constexpr std::string_view rsxFlipLimitName(RsxFlipLimit limit) {
    switch (limit) {
        case RsxFlipLimit::Vsync: return "vsync";
        case RsxFlipLimit::Hz60: return "60";
        case RsxFlipLimit::Off: return "off";
    }
    return "";
}

// Register state and effects behind the parser. All members are used under the kernel lock except
// census(), which only touches atomics, and takeCommands()/takeCaptures(), which only touch state the
// RSX thread owns.
class RsxConsumer final : public RsxSink {
public:
    explicit RsxConsumer(std::uint8_t* base);

    bool readIo(std::uint32_t io, std::uint32_t& word) override;
    const std::uint8_t* ioSpan(std::uint32_t io, std::uint32_t& bytes) override;
    bool write(std::uint32_t method, std::uint32_t value) override;
    // True after the write that ends a draw clause: the run stops there so that takeCommands() can copy
    // the draw's guest bytes outside the kernel lock with the registers that draw used. write() sets the sink's
    // stop flag from the same condition for the parser.
    bool stopAfterWrite() const;

    // Parses what the guest has put since the last call (at most `max_words` words), then publishes
    // `get` to the control register. After Fault nothing is consumed any more. A draw still pending from
    // the last run is built first.
    FifoStatus pump(std::uint32_t max_words);

    // False when a pump would find nothing new for this raw (big-endian already decoded) put value.
    // Needs no lock: used by the host thread to decide whether to take it.
    bool hasWork(std::uint32_t put) const;

    // Counts a VBlank and reports it.
    void vblankTick();

    using Clock = std::chrono::steady_clock;
    // How flip commands are paced; a flip command that is not due is refused (the parser keeps it and retries).
    // Hz60 holds a flip until a deadline that advances by kRsxFlipPeriod per completed flip; the first flip is
    // never delayed. Vsync holds a flip in VSYNC flip mode (rsxState().requested_vsync) until a vblank tick has
    // passed since its command first executed, the first flip included; a flip in HSYNC mode follows Hz60. Off by
    // default. Any thread: the mode is read on every attempt, so switching to Off releases a held flip on the next
    // pump, and switching to Hz60 or Vsync holds the flips after the next one.
    void setFlipLimit(RsxFlipLimit limit) { flip_limit_.store(limit, std::memory_order_relaxed); }
    // The time the held flip becomes due, while the parser is stopped at one held by the Hz60 deadline. The host
    // thread uses it to sleep without the kernel lock until then, because nothing can advance before.
    std::optional<Clock::time_point> flipHoldDeadline() const {
        return flip_waiting_ && !vblank_at_flip_ && flip_limit_.load(std::memory_order_relaxed) != RsxFlipLimit::Off
                       ? flip_deadline_
                       : std::nullopt;
    }
    // True while the parser is stopped at a flip that waits for a vblank tick and none has passed yet. Compares
    // against the live tick count, so a tick delivered after the last pump ends the hold at once: the host thread
    // must pump again rather than sleep to the next tick. Needs no lock: only vblankTick(), on the same thread,
    // changes the count. requested_vsync is written by guest threads, so a stale read there delays the end of the
    // hold by at most one sleep pass (one tick) after the game switches to HSYNC.
    bool flipHeldForVblank() const {
        return flip_waiting_ && vblank_at_flip_ && *vblank_at_flip_ == rsxState().vblank_count &&
               flip_limit_.load(std::memory_order_relaxed) == RsxFlipLimit::Vsync &&
               std::atomic_ref<bool>(rsxState().requested_vsync).load(std::memory_order_relaxed);
    }

    RsxCensus census() const;

    // Collects the argument census (rsx/rsx_census.h) from now on. Call before the first write.
    void enableArgCensus(std::filesystem::path json_path, std::filesystem::path dump_dir);
    RsxArgCensus* argCensus() { return arg_census_.get(); }

    // Keeps a register file and builds render commands from every completed write. Call before the first
    // write.
    void enableFrameBuilder();
    bool frameBuilderEnabled() const { return builder_ != nullptr; }
    // True while a frame builder exists, so a capture can be requested. Any thread.
    bool frameBuilderLive() const { return builder_live_.load(std::memory_order_acquire); }
    // Drops the frame builder again. Only while nothing was written yet and no capture is enabled.
    void disableFrameBuilder();
    // Commands built since the last call, after building a pending draw. Needs no kernel lock (only the
    // pump's thread may call it); the draw's copies then run without it.
    std::vector<RsxCommand> takeCommands();
    // Gives back commands that were executed and are not kept, so their buffers serve the next draws. Same thread
    // as takeCommands().
    void recycleCommands(std::vector<RsxCommand>&& commands);
    // Counts the commands by kind for the stop line; any thread.
    void countCommands(const std::vector<RsxCommand>& commands);
    struct CommandCounts {
        std::uint64_t clears = 0, draws = 0, flips = 0;
    };
    CommandCounts commandCounts() const;

    // Records the listed frames (1 = the writes up to and including the first flip command, N = those
    // after flip N-1 up to flip N) for rsx/rsx_capture.h. Enables the frame builder; call before the
    // first write.
    void enableCapture(std::vector<std::uint32_t> frames);
    struct FinishedCapture {
        std::uint32_t frame = 0;
        std::uint64_t writes = 0, ranges = 0;
        bool requested = false;  // started by requestCapture()
        RsxCaptureFile file;
    };
    // Captures completed since the last call; same thread rules as takeCommands().
    std::vector<FinishedCapture> takeCaptures();

    // Records the first whole frame that starts after this call, independent of the listed frames: the
    // consumer is usually past the flip command of the frame being presented, so that is typically the
    // frame after the next one. False, with nothing queued, without a frame builder. A request made while
    // another capture is recording starts at the next flip command. Any thread.
    bool requestCapture();
    // A request that has not started recording yet. Any thread.
    bool captureRequestPending() const { return capture_requested_.load(std::memory_order_acquire); }
    // The requested frame being recorded or waiting to be written, 0 when none. Read it after
    // captureRequestPending() so that a request is never seen as neither. Any thread.
    std::uint32_t requestedRecording() const { return requested_recording_.load(std::memory_order_acquire); }
    // Ends the requestedRecording() state of `frame` once its file result is published. The thread that
    // takes the captures calls it, so the state does not vanish while the file is still being written.
    void finishRequestedRecording(std::uint32_t frame);

    bool faulted() const { return faulted_; }
    std::uint32_t parserGet() const { return get_; }

private:
    bool execute(std::uint32_t method, std::uint32_t value);
    bool acquire(std::uint32_t value);
    void releaseNv406e(std::uint32_t value);
    void releaseNv4097(std::uint32_t value, bool swizzle);
    void getReport(std::uint32_t value);
    // One NV308A_COLOR register write: the word goes to the NV3062 destination surface.
    void inlineBlitWord(std::uint32_t method, std::uint32_t value);
    // True when a flip command may complete now; otherwise the flip is held. `vblank_paced` tells whether the
    // vblank rule decided it, in which case the wall-clock deadline does not move on.
    bool flipDue(bool& vblank_paced);
    // The Vsync limit's part of flipDue(): true once a vblank tick has passed since the first attempt.
    bool vblankFlipDue();
    void flip(std::uint32_t buffer);
    void advanceFlipDeadline();
    void userCommand(std::uint32_t cause);
    void publishGet(std::uint32_t get);
    // Builds the draw the last SET_BEGIN_END(0) left pending and records that write in the capture.
    void finishPendingDraw();
    // Effective address of `offset` in the context DMA `location`; false when out of range or not
    // one of the areas the consumer serves.
    bool resolve(std::uint32_t offset, std::uint32_t location, std::uint32_t size, std::uint32_t& ea) const;
    void resetDmaRegisters();
    // Word of RSX-visible memory for the argument census: location 0 = local, 1 = main (IO map).
    bool readCensusWord(std::uint32_t location, std::uint32_t offset, std::uint32_t& word) const;

    std::uint8_t* base_;
    FifoParser parser_;
    std::uint32_t get_ = 0;  // IO offset of the next unread word
    std::uint32_t published_get_ = 0;
    bool get_known_ = false;
    bool faulted_ = false;
    FifoStatus last_status_ = FifoStatus::Budget;
    std::uint32_t idle_put_ = 0xFFFFFFFF;  // masked put at the last Idle, never a valid one initially

    // Latched registers: semaphore offsets and context DMA objects.
    std::uint32_t offset_406e_ = 0x30, offset_4097_ = 0;
    std::uint32_t dma_406e_ = 0, dma_semaphore_ = 0, dma_report_ = 0;

    // NV3062 destination surface and NV308A point/size, latched for the inline blit. The DMA object is
    // set by resetDmaRegisters().
    std::uint32_t blit_dma_ = 0, blit_offset_ = 0, blit_pitch_ = 0, blit_format_ = 0;
    std::uint32_t blit_point_ = 0, blit_size_out_ = 0;

    std::uint64_t last_timestamp_ns_ = 0;

    // Written from any thread, read by the pump and the host loop. Relaxed: it guards no other data, and a
    // stale read only moves the switch by one loop pass.
    std::atomic<RsxFlipLimit> flip_limit_{RsxFlipLimit::Off};
    bool flip_waiting_ = false;  // a flip command was refused and is retried
    std::optional<Clock::time_point> flip_deadline_;
    // The vblank count when the held flip command first executed; set only while a Vsync hold is in progress.
    std::optional<std::uint64_t> vblank_at_flip_;

    std::atomic<std::uint64_t> headers_{0}, flips_{0}, user_commands_{0};
    std::unique_ptr<std::atomic<std::uint64_t>[]> method_counts_;  // by register offset >> 2
    std::unique_ptr<RsxArgCensus> arg_census_;                     // null unless --rsx-census

    // Null unless enableFrameBuilder() was called.
    std::unique_ptr<RsxRegisters> regs_;
    std::unique_ptr<GuestRsxMemoryView> memory_view_;
    std::unique_ptr<RsxFrameBuilder> builder_;
    std::atomic<std::uint64_t> clears_{0}, draws_{0}, built_flips_{0};

    void startCapture(std::uint32_t frame, bool requested = false);
    void finishCapture(std::uint32_t buffer);
    bool captureWanted(std::uint32_t frame) const;
    std::unique_ptr<RsxRecordingMemoryView> recording_view_;  // between the builder and memory_view_
    std::vector<std::uint32_t> capture_frames_;
    std::unique_ptr<RsxCaptureWriter> capture_;  // the frame being recorded
    std::uint32_t capture_frame_ = 0;
    bool capture_is_requested_ = false;
    // Release/acquire: the request flag is cleared after the recording frame is published, so a reader that sees
    // no pending request also sees the recording.
    std::atomic<bool> capture_requested_{false};
    std::atomic<std::uint32_t> requested_recording_{0};
    std::atomic<bool> builder_live_{false};
    std::uint64_t flips_reached_ = 0;  // flip commands the parser has reached (the builder's flip index)
    std::vector<FinishedCapture> finished_captures_;
    // The write that left the draw pending (method, value): the capture records it after the draw's memory reads.
    std::pair<std::uint32_t, std::uint32_t> pending_end_{0, 0};
};

// Receives the commands the frame builder produced, in order, on the RSX thread without the kernel lock.
// Without a sink they are counted and dropped. Call before startRsxThread.
using RsxCommandSink = std::function<void(std::vector<RsxCommand>&&)>;
void setRsxCommandSink(RsxCommandSink sink);

// What a renderer reports for the census and the stop line; read from any thread.
struct RsxRenderCounts {
    std::uint64_t draws = 0;          // draws rendered
    std::uint64_t draws_skipped = 0;  // draws seen and not rendered
    std::uint64_t clears = 0;
    std::uint64_t flips = 0;
    std::uint64_t frames_dumped = 0;
    std::uint64_t dumps_skipped = 0;  // dropped because the dump queue was full
    std::uint64_t dumps_failed = 0;
    std::uint32_t validation_errors = 0;
    std::uint64_t pipelines_created = 0;
    std::uint64_t pipeline_create_us = 0;     // time spent creating them
    std::uint64_t pipeline_cache_loaded = 0;  // bytes of pipeline cache data reused from an earlier run
    bool validation = false;                  // the validation layer is active
    bool failed = false;                      // the renderer stopped after an error
};

// A renderer owned by the RSX thread: created, used and destroyed there, so a device-bound object never
// changes thread.
class RsxExecutor {
public:
    virtual ~RsxExecutor() = default;
    // Runs on the RSX thread without the kernel lock; the commands carry every byte they need.
    virtual void execute(std::span<RsxCommand> commands) = 0;
    // Any thread.
    virtual RsxRenderCounts counts() const = 0;
    // Waits up to `timeout` for background output (dumped frames, the pipeline cache) to reach disk; returns
    // how many dumped frames are still pending (0 when all are done). The stop path calls it because the process ends
    // without running destructors. Any thread.
    virtual std::uint64_t flushOutput(std::chrono::milliseconds timeout) {
        (void)timeout;
        return 0;
    }
};

// Called once on the RSX thread before the first command is consumed. A null result (with the reason in
// `error`) leaves the run without rendering and drops the frame builder unless a capture needs it. Enables
// the frame builder. Call before startRsxThread.
using RsxExecutorFactory = std::function<std::unique_ptr<RsxExecutor>(std::string& error)>;
void setRsxExecutorFactory(RsxExecutorFactory factory);

// Turns the frame builder on for the consumer startRsxThread creates. Call before startRsxThread.
void setRsxFrameBuilderEnabled(bool enabled);

// Selects how flips are paced (see RsxFlipLimit): for the consumer startRsxThread creates, and at run time for the
// one already running. Vsync by default. Any thread.
void setRsxFlipLimit(RsxFlipLimit limit);
RsxFlipLimit rsxFlipLimit();

// Captures the listed frames (see RsxConsumer::enableCapture) into `<dir>/frame_<N>.tox2cap`, written on
// the RSX thread after the kernel lock is released. Implies the frame builder. Call before startRsxThread.
// The directory defaults to generated/rsx/captures, which also receives the captures requested at run time.
void setRsxCaptureFrames(std::vector<std::uint32_t> frames, std::filesystem::path dir);

// Where a capture requested at run time stands; see requestRsxCapture().
struct RsxCaptureStatus {
    bool available = false;        // a frame builder runs, so a capture can be requested
    bool pending = false;          // requested; the frame has not started yet
    std::uint32_t recording = 0;   // requested frame being recorded or written, 0 when none
    std::uint32_t last_frame = 0;  // last requested capture written (or failed)
    std::string last_path;         // generic_string() of its file; empty on failure
    std::string last_error;        // why the last requested capture failed; empty on success
};

// Records the first whole frame that starts after the call into `<capture dir>/frame_<N>.tox2cap`, like a
// listed frame. False when no frame builder runs (before startRsxThread, or after the renderer failed to
// start). Any thread.
bool requestRsxCapture();
RsxCaptureStatus rsxCaptureStatus();  // any thread

// Turns the argument census on for the consumer startRsxThread creates; written to `json_path` (the
// program dumps go to generated/rsx) when the process stops. Call before startRsxThread.
void setRsxArgCensusFile(std::string json_path);

// Starts the consumer thread over the guest memory at `base` and registers its census section.
// Call once after the RSX state is initialised; later calls do nothing.
void startRsxThread(std::uint8_t* base);

}  // namespace tox2
