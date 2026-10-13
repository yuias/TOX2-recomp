#include "rsx/rsx_thread.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "core/guest_memory.h"
#include "core/log.h"
#include "core/sleeper.h"
#include "core/threads.h"
#include "tox2/ppu_context.h"
#include "rsx/gcm_interrupts.h"
#include "rsx/rsx_decode.h"
#include "rsx/rsx_state.h"
#include "rsx/rsx_unimplemented.h"

namespace tox2 {
namespace {

// CellGcmControl field offsets.
constexpr std::uint32_t kControlPut = 0x00;
constexpr std::uint32_t kControlGet = 0x04;
constexpr std::uint32_t kControlRef = 0x08;

// Context DMA object names (RPCS3 `Emu/RSX/gcm_enums.h`) and their values after a context reset.
constexpr std::uint32_t kDmaSemaphoreRw = 0x66606660;
constexpr std::uint32_t kDmaSemaphoreR = 0x66616661;
constexpr std::uint32_t kDmaDeviceRw = 0x56616660;
constexpr std::uint32_t kDmaDeviceR = 0x56616661;
constexpr std::uint32_t kDmaReportLocal = 0x66626660;
constexpr std::uint32_t kDmaReportMain = 0xBAD68000;

constexpr std::uint32_t kSemaphoreAreaSize = 0x1000;  // RsxReports::semaphore[1024]
constexpr std::uint32_t kDeviceAreaSize = 0x100000;
constexpr std::uint32_t kReportAreaSize = 2048 * 16;
constexpr std::uint32_t kReportMainIoBase = 0x0E000000;  // the main-memory report window in IO space
constexpr std::uint32_t kReportMainSize = 0x1000000;
constexpr std::uint32_t kReportSize = 16;
constexpr std::uint32_t kFlipSemaphoreOffset = 0x10;  // the label the flip command clears
constexpr std::uint32_t kDeviceReadyOffset = 0x30;
constexpr std::uint64_t kFlipTimeOffsetUs = 1000000;

// NV3062 colour formats the inline blit copies word for word (RPCS3 `blit_engine::transfer_destination_format`).
constexpr std::uint32_t kBlitFormatA8R8G8B8 = 10;
constexpr std::uint32_t kBlitFormatY32 = 11;
constexpr std::uint32_t kBlitColorRegisters = 0x700;  // NV308A_COLOR spans this many registers

constexpr std::uint32_t kMethodCount = 0x4000;  // 16-bit register offsets, 4 bytes apart
constexpr std::uint32_t kPumpSliceWords = 128;  // words per pump() call inside one lock hold
// Checked after each slice, so one slice can exceed it.
constexpr std::chrono::microseconds kMaxGuestLockHold{300};

// Names of the registers the consumer interprets. The census prints hex for the others.
struct MethodName {
    std::uint32_t method;
    const char* name;
};
constexpr MethodName kMethodNames[] = {
        {kMethodNv406eSetReference, "NV406E_SET_REFERENCE"},
        {kMethodNv406eSetContextDmaSemaphore, "NV406E_SET_CONTEXT_DMA_SEMAPHORE"},
        {kMethodNv406eSemaphoreOffset, "NV406E_SEMAPHORE_OFFSET"},
        {kMethodNv406eSemaphoreAcquire, "NV406E_SEMAPHORE_ACQUIRE"},
        {kMethodNv406eSemaphoreRelease, "NV406E_SEMAPHORE_RELEASE"},
        {kMethodNv4097SetContextDmaSemaphore, "NV4097_SET_CONTEXT_DMA_SEMAPHORE"},
        {kMethodNv4097SetContextDmaReport, "NV4097_SET_CONTEXT_DMA_REPORT"},
        {kMethodNv4097GetReport, "NV4097_GET_REPORT"},
        {kMethodNv4097SetSemaphoreOffset, "NV4097_SET_SEMAPHORE_OFFSET"},
        {kMethodNv4097BackEndWriteSemaphoreRelease, "NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE"},
        {kMethodNv4097TextureReadSemaphoreRelease, "NV4097_TEXTURE_READ_SEMAPHORE_RELEASE"},
        {kMethodGcmFlipHead, "GCM_FLIP_HEAD"},
        {kMethodGcmFlipHead + 4, "GCM_FLIP_HEAD"},
        {kMethodGcmSetUserCommand, "GCM_SET_USER_COMMAND"},
        {kMethodGcmSetUserCommand + 4, "GCM_SET_USER_COMMAND"},
        {kMethodGcmFlipCommand, "GCM_FLIP_COMMAND"},
};

std::string methodName(std::uint32_t method) {
    for (const MethodName& entry : kMethodNames) {
        if (entry.method == method) return entry.name;
    }
    return {};
}

std::atomic<void (*)(RsxInterrupt, std::uint32_t)> g_hook{nullptr};

// Flips completed by any consumer; one process has one RSX thread, so this is the game's flip number.
std::atomic<std::uint64_t> g_flip_count{0};

// Kept in nanoseconds: a pass has several phase boundaries, and truncating each interval to microseconds would
// bias the sums by about half a microsecond per boundary.
struct ThreadTimesNs {
    std::atomic<std::uint64_t> idle{0}, lock_wait{0}, pump{0}, build{0}, record{0};
};
ThreadTimesNs g_thread_times;

void signalInterrupt(RsxInterrupt kind, std::uint32_t arg) {
    if (auto hook = g_hook.load()) hook(kind, arg);
}

}  // namespace

void setRsxInterruptHook(void (*hook)(RsxInterrupt kind, std::uint32_t arg)) { g_hook.store(hook); }

std::uint64_t rsxFlipCount() { return g_flip_count.load(std::memory_order_acquire); }

void waitRsxFlipCount(std::uint64_t n) {
    for (std::uint64_t count = rsxFlipCount(); count < n; count = rsxFlipCount()) g_flip_count.wait(count);
}

RsxThreadTimes rsxThreadTimes() {
    const auto us = [](const std::atomic<std::uint64_t>& ns) { return ns.load(std::memory_order_relaxed) / 1000; };
    return {us(g_thread_times.idle), us(g_thread_times.lock_wait), us(g_thread_times.pump), us(g_thread_times.build),
            us(g_thread_times.record)};
}

std::string formatRsxBusy(const RsxThreadTimes& delta, std::uint64_t flips) {
    if (flips == 0) return {};
    const double per_flip_ms = 1.0 / (1000.0 * static_cast<double>(flips));
    const auto ms = [&](std::uint64_t us) { return static_cast<double>(us) * per_flip_ms; };
    const std::uint64_t busy_us = delta.lock_wait_us + delta.pump_us + delta.build_us + delta.record_us;
    return std::format("rsx busy {:.2f} ms/flip: lock {:.2f} pump {:.2f} build {:.2f} record {:.2f}, idle {:.2f}",
                       ms(busy_us), ms(delta.lock_wait_us), ms(delta.pump_us), ms(delta.build_us), ms(delta.record_us),
                       ms(delta.idle_us));
}

RsxConsumer::RsxConsumer(std::uint8_t* base)
    : base_(base), method_counts_(std::make_unique<std::atomic<std::uint64_t>[]>(kMethodCount)) {
    resetDmaRegisters();
}

void RsxConsumer::resetDmaRegisters() {
    dma_406e_ = kDmaSemaphoreR;
    dma_semaphore_ = kDmaSemaphoreRw;
    dma_report_ = kDmaReportLocal;
    // The NV3062 destination is set once per boot by the game and falls back to local memory with every
    // context reset (the register table in rsx_registers.cpp).
    blit_dma_ = kRsxContextDmaLocal;
}

bool RsxConsumer::readIo(std::uint32_t io, std::uint32_t& word) {
    std::uint32_t ea = 0;
    if (!rsxIoToEa(io, ea)) return false;
    word = tox2Read32(base_, ea);
    return true;
}

bool RsxConsumer::write(std::uint32_t method, std::uint32_t value) {
    if (!execute(method, value)) return false;
    // Counted on completion, so a write retried while waiting is counted once.
    method_counts_[(method >> 2) & (kMethodCount - 1)].fetch_add(1, std::memory_order_relaxed);
    if (arg_census_ != nullptr) arg_census_->onWrite(method, value);
    if (builder_ != nullptr) {
        regs_->apply(method, value);
        builder_->onWrite(*regs_, method, value);
        // After the builder, so the memory it read is recorded ahead of the write that caused the read. A
        // draw end is recorded by finishPendingDraw(), after the copies.
        if (builder_->drawPending()) {
            pending_end_ = {method, value};
        } else if (capture_ != nullptr) {
            capture_->onWrite(method, value);
        }
        // Only a completed flip command gets here. The next frame starts from the state this flip left,
        // which includes its context reset and this write.
        if (method == kMethodGcmFlipCommand && capture_ == nullptr) {
            const auto next = static_cast<std::uint32_t>(flips_reached_ + 1);
            const bool requested = capture_requested_.load(std::memory_order_acquire);
            if (requested || captureWanted(next)) startCapture(next, requested);
        }
    }
    return true;
}

bool RsxConsumer::stopAfterWrite() { return builder_ != nullptr && builder_->drawPending(); }

void RsxConsumer::finishPendingDraw() {
    if (builder_ == nullptr || !builder_->drawPending()) return;
    builder_->finishDraw(*regs_);
    if (capture_ != nullptr) capture_->onWrite(pending_end_.first, pending_end_.second);
}

void RsxConsumer::enableFrameBuilder() {
    if (builder_ != nullptr) return;
    regs_ = std::make_unique<RsxRegisters>();
    regs_->reset();
    memory_view_ = std::make_unique<GuestRsxMemoryView>(base_);
    recording_view_ = std::make_unique<RsxRecordingMemoryView>(*memory_view_);
    builder_ = std::make_unique<RsxFrameBuilder>(*recording_view_);
    builder_->setDeferDrawEnd(true);
    builder_live_.store(true, std::memory_order_release);
}

void RsxConsumer::disableFrameBuilder() {
    if (builder_ == nullptr || capture_ != nullptr || !capture_frames_.empty() || flips_reached_ != 0) return;
    builder_live_.store(false, std::memory_order_release);
    capture_requested_.store(false, std::memory_order_release);
    builder_.reset();
    recording_view_.reset();
    memory_view_.reset();
    regs_.reset();
}

void RsxConsumer::enableCapture(std::vector<std::uint32_t> frames) {
    enableFrameBuilder();
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    capture_frames_ = std::move(frames);
    if (capture_ == nullptr && captureWanted(1)) startCapture(1);
}

bool RsxConsumer::captureWanted(std::uint32_t frame) const {
    return std::binary_search(capture_frames_.begin(), capture_frames_.end(), frame);
}

void RsxConsumer::startCapture(std::uint32_t frame, bool requested) {
    capture_frame_ = frame;
    capture_is_requested_ = requested;
    capture_ = std::make_unique<RsxCaptureWriter>(frame - 1, *regs_, builder_->knownSurfaces());
    recording_view_->setWriter(capture_.get());
    if (requested) {
        // Recording first, then the request cleared, so an observer never sees neither. Only this thread
        // clears the request; one made meanwhile is served by this capture too.
        requested_recording_.store(frame, std::memory_order_release);
        capture_requested_.store(false, std::memory_order_release);
    }
}

bool RsxConsumer::requestCapture() {
    if (!builder_live_.load(std::memory_order_acquire)) return false;
    capture_requested_.store(true, std::memory_order_release);
    return true;
}

void RsxConsumer::finishRequestedRecording(std::uint32_t frame) {
    // Compare first: a newer requested capture may have started while this one's file was written.
    std::uint32_t expected = frame;
    requested_recording_.compare_exchange_strong(expected, 0, std::memory_order_release, std::memory_order_relaxed);
}

void RsxConsumer::finishCapture(std::uint32_t buffer) {
    capture_->onFlip(buffer);
    const RsxState& state = rsxState();
    FinishedCapture done;
    done.frame = capture_frame_;
    done.writes = capture_->writeCount();
    done.ranges = capture_->rangeCount();
    done.requested = capture_is_requested_;
    done.file = capture_->finish(state.display, state.display_count);
    finished_captures_.push_back(std::move(done));
    recording_view_->setWriter(nullptr);
    capture_.reset();
}

std::vector<RsxConsumer::FinishedCapture> RsxConsumer::takeCaptures() { return std::exchange(finished_captures_, {}); }

std::vector<RsxCommand> RsxConsumer::takeCommands() {
    if (builder_ == nullptr) return {};
    finishPendingDraw();
    return builder_->takeCommands();
}

void RsxConsumer::countCommands(const std::vector<RsxCommand>& commands) {
    std::uint64_t clears = 0, draws = 0, flips = 0;
    for (const RsxCommand& command : commands) {
        clears += std::holds_alternative<RsxClear>(command);
        draws += std::holds_alternative<RsxDraw>(command);
        flips += std::holds_alternative<RsxFlip>(command);
    }
    clears_.fetch_add(clears, std::memory_order_relaxed);
    draws_.fetch_add(draws, std::memory_order_relaxed);
    built_flips_.fetch_add(flips, std::memory_order_relaxed);
}

RsxConsumer::CommandCounts RsxConsumer::commandCounts() const {
    return {clears_.load(std::memory_order_relaxed), draws_.load(std::memory_order_relaxed),
            built_flips_.load(std::memory_order_relaxed)};
}

void RsxConsumer::enableArgCensus(std::filesystem::path json_path, std::filesystem::path dump_dir) {
    arg_census_ = std::make_unique<RsxArgCensus>(
            std::move(json_path), std::move(dump_dir),
            [this](std::uint32_t location, std::uint32_t offset, std::uint32_t& word) {
                return readCensusWord(location, offset, word);
            },
            systemTimeUs());
}

bool RsxConsumer::readCensusWord(std::uint32_t location, std::uint32_t offset, std::uint32_t& word) const {
    std::uint32_t ea = 0;
    if (location == 0) {
        if (offset >= kRsxLocalSize || (offset & 3) != 0) return false;
        ea = kRsxLocalBase + offset;
    } else if (!rsxIoToEa(offset, ea)) {
        return false;
    }
    if (!process().memory.isCommitted(ea, 4)) return false;
    word = tox2Read32(base_, ea);
    return true;
}

bool RsxConsumer::execute(std::uint32_t method, std::uint32_t value) {
    switch (method) {
        case kMethodNv406eSetReference: tox2Write32(base_, kRsxControlAddr + kControlRef, value); return true;
        case kMethodNv406eSetContextDmaSemaphore: dma_406e_ = value; return true;
        case kMethodNv406eSemaphoreOffset: offset_406e_ = value; return true;
        case kMethodNv406eSemaphoreAcquire: return acquire(value);
        case kMethodNv406eSemaphoreRelease: releaseNv406e(value); return true;
        case kMethodNv4097SetContextDmaSemaphore: dma_semaphore_ = value; return true;
        case kMethodNv4097SetContextDmaReport: dma_report_ = value; return true;
        case kMethodNv4097SetSemaphoreOffset: offset_4097_ = value; return true;
        case kMethodNv4097BackEndWriteSemaphoreRelease: releaseNv4097(value, true); return true;
        case kMethodNv4097TextureReadSemaphoreRelease: releaseNv4097(value, false); return true;
        case kMethodNv4097GetReport: getReport(value); return true;
        case kMethodNv3062SetContextDmaImageDestin: blit_dma_ = value; return true;
        case kMethodNv3062SetColorFormat: blit_format_ = value; return true;
        case kMethodNv3062SetPitch: blit_pitch_ = value; return true;
        case kMethodNv3062SetOffsetDestin: blit_offset_ = value; return true;
        case kMethodNv308aPoint: blit_point_ = value; return true;
        case kMethodNv308aSizeOut: blit_size_out_ = value; return true;
        case kMethodGcmSetUserCommand:
        case kMethodGcmSetUserCommand + 4:  // two registers share the handler
            userCommand(value);
            return true;
        case kMethodGcmFlipCommand: {
            bool vblank_paced = false;
            if (!flipDue(vblank_paced)) return false;
            flip(value);
            // A flip that waited for a vblank has no wall-clock deadline to move on.
            if (!vblank_paced) advanceFlipDeadline();
            return true;
        }
        default:
            if (method >= kMethodNv308aColor && method < kMethodNv308aColor + kBlitColorRegisters * 4) {
                inlineBlitWord(method, value);
            }
            return true;
    }
}

void RsxConsumer::inlineBlitWord(std::uint32_t method, std::uint32_t value) {
    const std::uint32_t index = (method - kMethodNv308aColor) >> 2;
    const std::uint32_t width = blit_size_out_ & 0xFFFF;
    if (index >= width) return;
    // Only the plain word copies are done; the other formats need a conversion that is not implemented.
    if (blit_format_ != kBlitFormatA8R8G8B8 && blit_format_ != kBlitFormatY32) {
        rsxNoteUnimplemented("method", std::format("NV308A_COLOR format {}", blit_format_));
        return;
    }
    // Source pitch is the low half and destination pitch the high half of NV3062_SET_PITCH.
    const std::uint64_t offset =
            rsxInlineBlitOffset(blit_offset_, blit_pitch_ >> 16, blit_point_ & 0xFFFF, blit_point_ >> 16, index);
    std::uint32_t ea = 0;
    bool ok = false;
    if (offset <= 0xFFFFFFFFull && (offset & 3) == 0) {
        const std::uint32_t off = static_cast<std::uint32_t>(offset);
        if (blit_dma_ == kRsxContextDmaLocal) {
            ok = off < kRsxLocalSize && kRsxLocalSize - off >= 4 &&
                 process().memory.isCommitted(kRsxLocalBase + off, 4);
            ea = kRsxLocalBase + off;
        } else if (blit_dma_ == kRsxContextDmaMain) {
            ok = rsxIoToEa(off, ea) && process().memory.isCommitted(ea, 4);
        }
    }
    if (!ok) {
        rsxNoteUnimplemented("method", "NV308A_COLOR destination out of range");
        logOnce("rsx-blit-bad-address", "tox2: rsx: NV308A inline blit to offset {:#x} in DMA {:#010x} ignored", offset,
                blit_dma_);
        return;
    }
    tox2Write32(base_, ea, value);
}

bool RsxConsumer::resolve(std::uint32_t offset, std::uint32_t location, std::uint32_t size, std::uint32_t& ea) const {
    switch (location) {
        case kDmaSemaphoreRw:
        case kDmaSemaphoreR:
            if (offset >= kSemaphoreAreaSize || size > kSemaphoreAreaSize - offset) return false;
            ea = kRsxLabelAddr + offset;
            return true;
        case kDmaDeviceRw:
        case kDmaDeviceR:
            if (offset >= kDeviceAreaSize || size > kDeviceAreaSize - offset) return false;
            ea = kRsxDeviceAddr + offset;
            return true;
        case kDmaReportLocal:
            if (offset >= kReportAreaSize || size > kReportAreaSize - offset) return false;
            ea = kRsxLabelAddr + kRsxReportsReportOffset + offset;
            return true;
        case kDmaReportMain: {
            std::uint32_t mapped = 0;
            if (offset >= kReportMainSize || !rsxIoToEa(kReportMainIoBase + offset, mapped)) return false;
            if (!process().memory.isCommitted(mapped, size)) return false;
            ea = mapped;
            return true;
        }
        default: return false;
    }
}

bool RsxConsumer::acquire(std::uint32_t value) {
    std::uint32_t ea = 0;
    if (!resolve(offset_406e_, dma_406e_, 4, ea)) {
        // RPCS3 throws here; waiting forever on an address that cannot hold a label helps nobody.
        logOnce("rsx-acquire-bad-address",
                "tox2: rsx: semaphore acquire at offset {:#x} in DMA {:#010x} is not addressable, ignored",
                offset_406e_, dma_406e_);
        return true;
    }
    return tox2Read32(base_, ea) == value;
}

void RsxConsumer::releaseNv406e(std::uint32_t value) {
    if (offset_406e_ % 4 != 0) {
        logOnce("rsx-release-unaligned", "tox2: rsx: NV406E semaphore release at unaligned offset {:#x} ignored",
                offset_406e_);
        return;
    }
    std::uint32_t ea = 0;
    if (!resolve(offset_406e_, dma_406e_, 4, ea) || (ea >> 28) != (kRsxLabelAddr >> 28)) {
        logOnce("rsx-release-bad-address",
                "tox2: rsx: NV406E semaphore release at offset {:#x} in DMA {:#010x} ignored", offset_406e_, dma_406e_);
        return;
    }
    // The flip handshake word reads back 1 even though no display queue command set it.
    if (ea == kRsxDeviceAddr + kDeviceReadyOffset && value == 0) value = 1;
    tox2Write32(base_, ea, value);
}

void RsxConsumer::releaseNv4097(std::uint32_t value, bool swizzle) {
    if (offset_4097_ % 16 != 0) {
        logOnce("rsx-label-unaligned", "tox2: rsx: NV4097 semaphore release at unaligned offset {:#x} ignored",
                offset_4097_);
        return;
    }
    std::uint32_t ea = 0;
    if (!resolve(offset_4097_, dma_semaphore_, 4, ea) || (ea >> 28) != (kRsxLabelAddr >> 28)) {
        logOnce("rsx-label-bad-address", "tox2: rsx: NV4097 semaphore release at offset {:#x} in DMA {:#010x} ignored",
                offset_4097_, dma_semaphore_);
        return;
    }
    // The back-end release swaps the red and blue bytes of the value.
    if (swizzle) value = (value & 0xFF00FF00) | ((value & 0xFF) << 16) | ((value >> 16) & 0xFF);
    tox2Write32(base_, ea, value);
}

void RsxConsumer::getReport(std::uint32_t value) {
    const std::uint32_t offset = value & 0xFFFFFF;
    std::uint32_t ea = 0;
    if (!resolve(offset, dma_report_, kReportSize, ea)) {
        logOnce("rsx-report-bad-address", "tox2: rsx: NV4097_GET_REPORT {:#010x} in DMA {:#010x} ignored", value,
                dma_report_);
        return;
    }
    // No zpass or zcull statistics are gathered; only the timer is real. The value 0 is what RPCS3 writes
    // when no counting is active (`thread::get_zcull_stats` with disable_zcull_queries, and
    // `ZCULL_control::read_report` with no query ever started); this title never enables pixel counting.
    rsxNoteUnimplemented("state", "zpass report constant");
    const std::uint64_t now_ns = systemTimeUs() * 1000;
    last_timestamp_ns_ = std::max(now_ns, last_timestamp_ns_ + 1);
    tox2Write64(base_, ea, last_timestamp_ns_);
    tox2Write32(base_, ea + 8, 0);
    tox2Write32(base_, ea + 12, 0);
}

bool RsxConsumer::flipDue(bool& vblank_paced) {
    const RsxFlipLimit limit = flip_limit_.load(std::memory_order_relaxed);
    // The mode is re-read on every attempt: a switch of the limit or of the game's flip mode ends a hold at once.
    vblank_paced = limit == RsxFlipLimit::Vsync &&
                   std::atomic_ref<bool>(rsxState().requested_vsync).load(std::memory_order_relaxed);
    if (vblank_paced) return vblankFlipDue();
    vblank_at_flip_.reset();
    if (limit == RsxFlipLimit::Off || !flip_deadline_ || Clock::now() >= *flip_deadline_) {
        flip_waiting_ = false;
        return true;
    }
    flip_waiting_ = true;
    return false;
}

// RPCS3's "PS3 Native" frame limit (`handle_emu_flip`): the flip records the vblank count where its command first
// executes and completes once the count has moved on. Unlike RPCS3, which defers the flip and lets the FIFO run on,
// the parser stays at the command, so a second flip cannot complete in the same vblank interval.
bool RsxConsumer::vblankFlipDue() {
    const std::uint64_t vblank_count = rsxState().vblank_count;
    if (!vblank_at_flip_) {
        vblank_at_flip_ = vblank_count;
        flip_waiting_ = true;
        return false;
    }
    if (*vblank_at_flip_ == vblank_count) return false;
    vblank_at_flip_.reset();
    flip_waiting_ = false;
    return true;
}

// RPCS3's default "Auto" frame limit (`handle_emu_flip`): the RSX thread sleeps until a target time that moves on
// by one period per flip, measured from the previous target. It does not catch up after a late flip.
void RsxConsumer::advanceFlipDeadline() {
    if (flip_limit_.load(std::memory_order_relaxed) == RsxFlipLimit::Off) return;
    const Clock::time_point now = Clock::now();
    Clock::time_point next = flip_deadline_ ? *flip_deadline_ + kRsxFlipPeriod : now + kRsxFlipPeriod;
    if (next <= now) next = now + kRsxFlipPeriod;
    flip_deadline_ = next;
}

void RsxConsumer::flip(std::uint32_t buffer) {
    RsxState& state = rsxState();
    // The command waits in flipDue() until the flip limit allows it, and then completes where it executes
    // (RPCS3 `flip_command` -> `request_emu_flip` -> `handle_emu_flip`). In the Vsync limit a flip in the game's
    // VSYNC flip mode waits for a vblank tick (RPCS3's "PS3 Native" frame limit); otherwise only the wall-clock
    // limiter applies.
    if (builder_ != nullptr) {
        static constexpr RsxDisplayBuffer kNoBuffer{};
        builder_->onFlip(*regs_, buffer, buffer < kRsxDisplayBufferCount ? state.display[buffer] : kNoBuffer);
        if (capture_ != nullptr) finishCapture(buffer);
        flips_reached_++;
        // RPCS3 resets the method registers where the command executes (`flip_command` -> `thread::reset`).
        regs_->contextReset();
    }

    if (flips_.fetch_add(1, std::memory_order_relaxed) == 0) {
        logInfo("tox2: rsx: first flip of buffer {}: put={:#x} get={:#x} parser={:#x}", buffer,
                tox2Read32(base_, kRsxControlAddr + kControlPut), tox2Read32(base_, kRsxControlAddr + kControlGet),
                get_);
    }
    state.flip_status = kRsxFlipStatusDone;
    const std::uint64_t now = systemTimeUs();
    state.last_flip_time = now > kFlipTimeOffsetUs ? now - kFlipTimeOffsetUs : 0;
    if (arg_census_ != nullptr) arg_census_->onFlip(now);
    std::memset(base_ + kRsxLabelAddr + kFlipSemaphoreOffset, 0, 16);
    // RPCS3 resets the method registers on every flip; the context DMA objects are the ones that matter.
    resetDmaRegisters();
    signalInterrupt(RsxInterrupt::Flip, 1);
    // Last, so a waiter that wakes up sees the flip fully done. The count after the Nth flip is N, the same
    // number that names frame_<N>.png (the renderer's flip count) and selects --rsx-capture N (flips_reached_).
    g_flip_count.fetch_add(1, std::memory_order_release);
    g_flip_count.notify_all();
}

void RsxConsumer::userCommand(std::uint32_t cause) {
    user_commands_.fetch_add(1, std::memory_order_relaxed);
    if (g_hook.load() == nullptr) {
        logOnce(std::format("rsx-user-command-{}", cause), "tox2: rsx: pending user command cause {}", cause);
        return;
    }
    signalInterrupt(RsxInterrupt::User, cause);
}

void RsxConsumer::vblankTick() {
    rsxState().vblank_count++;
    if (arg_census_ != nullptr) arg_census_->onVBlank(systemTimeUs());
    signalInterrupt(RsxInterrupt::VBlank, 1);
}

void RsxConsumer::publishGet(std::uint32_t get) {
    if (get_known_ && get == published_get_) return;
    published_get_ = get;
    get_known_ = true;
    tox2Write32(base_, kRsxControlAddr + kControlGet, get);
    rsxGetAdvanced().notify_all();
}

FifoStatus RsxConsumer::pump(std::uint32_t max_words) {
    // The guest may already see `get` past a pending draw (when the draw end was the last argument of its
    // command), as on the hardware, where get is the fetch position. It reuses draw memory only after a later label,
    // report or flip, which the writes after the draw produce; so the draw is copied before any of them runs. Normally
    // takeCommands() already did this outside the lock.
    finishPendingDraw();
    if (faulted_) return FifoStatus::Fault;
    if (!get_known_) {
        get_ = tox2Read32(base_, kRsxControlAddr + kControlGet);
        published_get_ = get_;
        get_known_ = true;
    }
    const std::uint32_t put = tox2Read32(base_, kRsxControlAddr + kControlPut);
    const FifoStatus status = parser_.run(get_, put, max_words, *this);
    // The guest sees `get` at the start of an unfinished command (a semaphore wait, a flip waiting
    // for VBlank), so "get == put" means the work is really done. The wrap callback only needs get
    // to leave the fragment it is about to refill, which this also guarantees.
    publishGet(parser_.commandGet(get_));
    headers_.store(parser_.headersParsed(), std::memory_order_relaxed);
    last_status_ = status;
    if (status == FifoStatus::Idle) {
        idle_put_ = put & ~3u;
        if (flips_.load(std::memory_order_relaxed) != 0) {
            logOnce("rsx-idle-after-flip",
                    "tox2: rsx: command buffer drained after the first flip: put={:#x} get={:#x}", put, published_get_);
        }
    }
    if (status == FifoStatus::Fault) {
        faulted_ = true;
        logError("tox2: rsx: command fifo fault, no more commands are consumed: {} (get {:#x}, put {:#x})",
                 parser_.lastFault(), get_, put);
    }
    return status;
}

bool RsxConsumer::hasWork(std::uint32_t put) const {
    if (faulted_) return false;
    return last_status_ != FifoStatus::Idle || (put & ~3u) != idle_put_;
}

RsxCensus RsxConsumer::census() const {
    RsxCensus result;
    result.headers = headers_.load(std::memory_order_relaxed);
    result.flips = flips_.load(std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < kMethodCount; i++) {
        const std::uint64_t count = method_counts_[i].load(std::memory_order_relaxed);
        if (count != 0) result.methods.push_back({i << 2, count, methodName(i << 2)});
    }
    return result;
}

namespace {

RsxConsumer* g_consumer = nullptr;
std::string g_arg_census_file;
RsxExecutorFactory g_executor_factory;
// Published by the RSX thread once the executor exists; the thread never ends, so it is never cleared.
std::atomic<RsxExecutor*> g_executor{nullptr};

RsxCensus consumerCensus() {
    RsxCensus result = g_consumer != nullptr ? g_consumer->census() : RsxCensus{};
    const GcmInterruptCounts calls = gcmInterruptCounts();
    result.vblank_handler_runs = calls.vblank;
    result.flip_handler_runs = calls.flip;
    result.user_handler_runs = calls.user;
    result.vblank_coalesced = calls.vblank_coalesced;
    if (const RsxExecutor* executor = g_executor.load(std::memory_order_acquire)) {
        const RsxRenderCounts counts = executor->counts();
        result.rendering = true;
        result.draws_rendered = counts.draws;
        result.draws_skipped = counts.draws_skipped;
    }
    return result;
}

RsxUnimplementedCensus unimplementedCensus() { return {rsxUnimplementedSnapshot(), rsxUnimplementedDropped()}; }

RsxCommandSink g_command_sink;
bool g_frame_builder_enabled = false;
// Read by any thread through rsxFlipLimit(); the live consumer is told as well, see startRsxThread().
std::atomic<RsxFlipLimit> g_flip_limit{RsxFlipLimit::Vsync};
// Set once by startRsxThread, never cleared (the consumer is leaked). Sequentially consistent on purpose: it
// pairs with g_flip_limit so that a limiter change racing the thread start is seen by at least one side.
std::atomic<RsxConsumer*> g_live_consumer{nullptr};
std::vector<std::uint32_t> g_capture_frames;
std::filesystem::path g_capture_dir = std::filesystem::path("generated") / "rsx" / "captures";
std::atomic<std::uint32_t> g_captures_written{0};
// The file result of the last requested capture; the UI thread reads it while the RSX thread writes it.
std::mutex g_capture_status_mutex;
RsxCaptureStatus g_capture_last;

// Runs once on the stop path, after the text census.
void onStop() {
    if (g_consumer == nullptr) return;
    if (g_consumer->frameBuilderEnabled()) {
        const RsxConsumer::CommandCounts counts = g_consumer->commandCounts();
        logInfo("tox2: rsx: frame builder: {} clears, {} draws, {} flips", counts.clears, counts.draws, counts.flips);
    }
    if (RsxExecutor* executor = g_executor.load(std::memory_order_acquire)) {
        // The process ends without destructors, so the dump worker's queue is written here. The bound keeps a
        // stuck disk from delaying the exit; frames still pending after it are lost and reported below.
        constexpr auto kFlushTimeout = std::chrono::milliseconds(2000);
        const std::uint64_t unflushed = executor->flushOutput(kFlushTimeout);
        const RsxRenderCounts r = executor->counts();
        logInfo("tox2: render: {} clears, {} draws rendered, {} skipped, {} flips, {} frames dumped, "
                "{} dumps skipped (queue full), {} dumps failed, validation {}{}",
                r.clears, r.draws, r.draws_skipped, r.flips, r.frames_dumped, r.dumps_skipped, r.dumps_failed,
                r.validation ? std::format("{} errors", r.validation_errors) : std::string("off"),
                r.failed ? ", renderer stopped after an error" : "");
        logInfo("tox2: render: {} pipelines created in {} ms, {} bytes of pipeline cache reused", r.pipelines_created,
                r.pipeline_create_us / 1000, r.pipeline_cache_loaded);
        if (unflushed != 0) {
            logWarn("tox2: render: frame dump flush timed out, {} queued frame(s) were not written", unflushed);
        }
    }
    const std::uint32_t written = g_captures_written.load();
    if (written < g_capture_frames.size()) {
        logWarn("tox2: rsx: {} of {} requested captures written", written, g_capture_frames.size());
    }
    if (g_consumer->argCensus() != nullptr) g_consumer->argCensus()->writeFiles();
}

// Counts the commands built so far and hands them to the sink. Same thread as the pump, so nothing
// else touches the builder in between.
void dispatchCommands(RsxConsumer* consumer, RsxExecutor* executor, std::vector<RsxCommand> commands) {
    if (commands.empty()) return;
    consumer->countCommands(commands);
    if (executor != nullptr) executor->execute(commands);
    if (g_command_sink) g_command_sink(std::move(commands));
}

// File I/O stays out of the guest lock; a failed write is reported and the run goes on.
void writeCaptures(RsxConsumer* consumer, const std::vector<RsxConsumer::FinishedCapture>& captures) {
    for (const RsxConsumer::FinishedCapture& capture : captures) {
        const std::filesystem::path path = g_capture_dir / std::format("frame_{}.tox2cap", capture.frame);
        std::string error;
        const bool written = writeCaptureFile(path, capture.file, error);
        if (!written) {
            logWarn("tox2: rsx: capture of frame {} failed: {}", capture.frame, error);
        } else {
            // The stop line compares this with the --rsx-capture list, so a requested capture only counts when
            // it is also listed.
            if (std::binary_search(g_capture_frames.begin(), g_capture_frames.end(), capture.frame)) {
                g_captures_written.fetch_add(1);
            }
            logInfo("tox2: rsx: captured frame {} -> {} ({} bytes, {} writes, {} ranges)", capture.frame,
                    path.generic_string(), capture.file.size(), capture.writes, capture.ranges);
        }
        if (!capture.requested) continue;
        {
            std::lock_guard<std::mutex> lock(g_capture_status_mutex);
            g_capture_last.last_frame = capture.frame;
            g_capture_last.last_path = written ? path.generic_string() : std::string();
            g_capture_last.last_error = written ? std::string() : error;
        }
        consumer->finishRequestedRecording(capture.frame);
    }
}

void consumerLoop(RsxConsumer* consumer, std::uint8_t* base) {
    using Clock = std::chrono::steady_clock;
    constexpr auto kPeriod = std::chrono::microseconds(kRsxVBlankPeriodUs);
    constexpr auto kPollInterval = std::chrono::milliseconds(1);
    Sleeper sleeper;
    setGuestLockClass(GuestLockClass::Rsx);

    // The device and renderer belong to this thread. They are created before the first command is
    // consumed, so a failure can still drop the frame builder.
    std::unique_ptr<RsxExecutor> executor;
    if (g_executor_factory) {
        std::string error;
        executor = g_executor_factory(error);
        if (executor) {
            g_executor.store(executor.get(), std::memory_order_release);
        } else {
            logWarn("tox2: render: {}; continuing without rendering", error.empty() ? "initialisation failed" : error);
            consumer->disableFrameBuilder();
        }
    }

    // The put word is the one guest word read without the lock: aligned, and a stale value only
    // delays work until the next pass.
    std::atomic_ref<std::uint32_t> put_word(*reinterpret_cast<std::uint32_t*>(base + kRsxControlAddr + kControlPut));

    auto last_mark = Clock::now();
    auto next_tick = last_mark + kPeriod;
    // Charges the time since the previous boundary to `counter`. The boundaries chain, so every moment of the loop
    // is charged to exactly one activity; a pass that ends in `continue` is all idle.
    const auto charge = [&](std::atomic<std::uint64_t>& counter, Clock::time_point now) {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_mark).count();
        counter.fetch_add(static_cast<std::uint64_t>(std::max<std::int64_t>(0, ns)), std::memory_order_relaxed);
        last_mark = now;
    };
    // True when a vblank is due at `now`; moves the deadline on, without a burst after a long pause.
    const auto tickDue = [&](Clock::time_point now) {
        if (now < next_tick) return false;
        next_tick += kPeriod;
        if (next_tick <= now) next_tick = now + kPeriod;
        return true;
    };
    for (;;) {
        // A flip held by the limit ends the pump with Waiting and the loop comes back here, outside the guest
        // lock. A flip held by the Hz60 deadline ends the sleep at that deadline, so it completes on time. A flip
        // held for a vblank sleeps up to the next tick instead of polling: nothing can advance before it. Either
        // way the vblank ticks still arrive on time.
        const auto before = Clock::now();
        const std::optional<Clock::time_point> flip_deadline = consumer->flipHoldDeadline();
        const bool flip_vblank_held = consumer->flipHeldForVblank();
        Clock::duration wait = next_tick - before;
        if (!flip_vblank_held) wait = std::min<Clock::duration>(wait, kPollInterval);
        if (flip_deadline) wait = std::min(wait, *flip_deadline - before);
        sleeper.sleep(std::chrono::duration_cast<std::chrono::microseconds>(wait));

        // A due tick is never skipped for a held flip. Interrupt order: the tick queues the vblank handler (in
        // vblankTick(), which also advances the count the held flip waits for) before the pump that completes the
        // flip queues the flip handler. The game's frame-interval countdown in the vblank handler is released by
        // the flip handler; reversed, it would see the flip one vblank early and the 30 fps mode would drop to 60.
        const auto woke = Clock::now();
        charge(g_thread_times.idle, woke);
        const bool tick = tickDue(woke);
        const std::uint32_t put = std::byteswap(put_word.load(std::memory_order_relaxed));
        // Nothing can advance before the deadline or the tick whatever the guest puts, so the lock is not taken
        // for it. flipHeldForVblank() compares with the live count: a tick delivered after the last pump (by the
        // slice loop below) already ended the hold, and the flip must be pumped now, not one tick later.
        const bool flip_held = (flip_deadline && woke < *flip_deadline) || consumer->flipHeldForVblank();
        if (!tick && (flip_held || !consumer->hasWork(put))) continue;

        bool pending_tick = tick;
        FifoStatus status;
        do {
            std::vector<RsxCommand> commands;
            std::vector<RsxConsumer::FinishedCapture> captures;
            {
                // The guest lock is a FIFO ticket lock shared with the audio engine and every guest thread.
                // Pumping in small slices and leaving once the hold budget is spent lets the waiters in between;
                // the loop re-queues for the rest. A pump also stops after each draw end, whose vertex and
                // texture bytes takeCommands() then copies without the lock.
                std::lock_guard<GuestLock> lock(guestLock());
                const auto hold_start = Clock::now();
                charge(g_thread_times.lock_wait, hold_start);
                if (pending_tick) {
                    consumer->vblankTick();
                    pending_tick = false;
                }
                do {
                    status = consumer->pump(kPumpSliceWords);
                    // A backlog keeps this loop running for as long as the guest queues work, so the vblank is
                    // delivered here too; otherwise the guest sees none while the consumer is behind.
                    if (tickDue(Clock::now())) consumer->vblankTick();
                } while (status == FifoStatus::Budget && !consumer->stopAfterWrite() &&
                         Clock::now() - hold_start < kMaxGuestLockHold);
            }
            charge(g_thread_times.pump, Clock::now());
            commands = consumer->takeCommands();
            charge(g_thread_times.build, Clock::now());
            captures = consumer->takeCaptures();
            dispatchCommands(consumer, executor.get(), std::move(commands));
            if (!captures.empty()) writeCaptures(consumer, captures);
            charge(g_thread_times.record, Clock::now());
        } while (status == FifoStatus::Budget);
    }
}

}  // namespace

void setRsxArgCensusFile(std::string json_path) { g_arg_census_file = std::move(json_path); }

void setRsxCommandSink(RsxCommandSink sink) { g_command_sink = std::move(sink); }

void setRsxFrameBuilderEnabled(bool enabled) { g_frame_builder_enabled = enabled; }

void setRsxFlipLimit(RsxFlipLimit limit) {
    g_flip_limit.store(limit);
    if (RsxConsumer* consumer = g_live_consumer.load()) consumer->setFlipLimit(limit);
}

RsxFlipLimit rsxFlipLimit() { return g_flip_limit.load(); }

bool requestRsxCapture() {
    RsxConsumer* consumer = g_live_consumer.load();
    return consumer != nullptr && consumer->requestCapture();
}

RsxCaptureStatus rsxCaptureStatus() {
    RsxCaptureStatus status;
    if (const RsxConsumer* consumer = g_live_consumer.load()) {
        // In this order: the request is cleared only after the recording frame is published.
        status.available = consumer->frameBuilderLive();
        status.pending = status.available && consumer->captureRequestPending();
        status.recording = consumer->requestedRecording();
    }
    std::lock_guard<std::mutex> lock(g_capture_status_mutex);
    status.last_frame = g_capture_last.last_frame;
    status.last_path = g_capture_last.last_path;
    status.last_error = g_capture_last.last_error;
    return status;
}

void setRsxExecutorFactory(RsxExecutorFactory factory) {
    g_executor_factory = std::move(factory);
    if (g_executor_factory) g_frame_builder_enabled = true;
}

void setRsxCaptureFrames(std::vector<std::uint32_t> frames, std::filesystem::path dir) {
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    g_capture_frames = std::move(frames);
    g_capture_dir = std::move(dir);
    if (!g_capture_frames.empty()) g_frame_builder_enabled = true;
}

void startRsxThread(std::uint8_t* base) {
    if (g_consumer != nullptr) return;
    // Leaked: the thread runs until the process ends.
    g_consumer = new RsxConsumer(base);
    // Published before the limiter is read, so a setRsxFlipLimit() racing this is usually seen by one side.
    // Both running at the same instant can still leave the consumer one switch behind; the overlay, the only
    // run-time caller, starts after the first flip.
    g_live_consumer.store(g_consumer);
    g_consumer->setFlipLimit(g_flip_limit.load());
    setRsxCensusProvider(&consumerCensus);
    setRsxUnimplementedProvider(&unimplementedCensus, &rsxUnimplementedReset);
    if (!g_arg_census_file.empty()) {
        g_consumer->enableArgCensus(g_arg_census_file, std::filesystem::path("generated") / "rsx");
    }
    if (g_frame_builder_enabled) g_consumer->enableFrameBuilder();
    if (!g_capture_frames.empty()) g_consumer->enableCapture(g_capture_frames);
    if (!g_arg_census_file.empty() || g_frame_builder_enabled) setRsxStopWriter(&onStop);
    std::thread(consumerLoop, g_consumer, base).detach();
}

}  // namespace tox2
