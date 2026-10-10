#include "spu/spu_channels.h"

#include <algorithm>
#include <format>
#include <optional>

#include "core/census.h"
#include "core/reservation.h"

namespace tox2 {
namespace {

using std::chrono::microseconds;
using std::chrono::milliseconds;

// How long a wait on the reservation table goes without looking again. Plain PPU stores do not move the
// store epoch, so this bounds how late they are noticed.
constexpr milliseconds kPlainStorePoll{1};

const char* channelName(std::uint32_t ch) {
    switch (ch) {
        case chan::kRdEventStat: return "SPU_RdEventStat";
        case chan::kWrEventMask: return "SPU_WrEventMask";
        case chan::kWrEventAck: return "SPU_WrEventAck";
        case chan::kRdSigNotify1: return "SPU_RdSigNotify1";
        case chan::kRdSigNotify2: return "SPU_RdSigNotify2";
        case chan::kWrDec: return "SPU_WrDec";
        case chan::kRdDec: return "SPU_RdDec";
        case chan::kWrMsSyncReq: return "MFC_WrMSSyncReq";
        case chan::kRdEventMask: return "SPU_RdEventMask";
        case chan::kRdTagMask: return "MFC_RdTagMask";
        case chan::kRdMachStat: return "SPU_RdMachStat";
        case chan::kWrSrr0: return "SPU_WrSRR0";
        case chan::kRdSrr0: return "SPU_RdSRR0";
        case chan::kMfcLsa: return "MFC_LSA";
        case chan::kMfcEah: return "MFC_EAH";
        case chan::kMfcEal: return "MFC_EAL";
        case chan::kMfcSize: return "MFC_Size";
        case chan::kMfcTagId: return "MFC_TagID";
        case chan::kMfcCmd: return "MFC_Cmd";
        case chan::kWrTagMask: return "MFC_WrTagMask";
        case chan::kWrTagUpdate: return "MFC_WrTagUpdate";
        case chan::kRdTagStat: return "MFC_RdTagStat";
        case chan::kRdListStallStat: return "MFC_RdListStallStat";
        case chan::kWrListStallAck: return "MFC_WrListStallAck";
        case chan::kRdAtomicStat: return "MFC_RdAtomicStat";
        case chan::kWrOutMbox: return "SPU_WrOutMbox";
        case chan::kRdInMbox: return "SPU_RdInMbox";
        case chan::kWrOutIntrMbox: return "SPU_WrOutIntrMbox";
        default: return "?";
    }
}

// Shows a wait to the supervisor for as long as the scope lives.
class BlockedScope {
public:
    BlockedScope(SpuThread& thread, const char* what, bool indefinite) : thread_(thread) {
        thread_.setBlocked(what, indefinite);
    }
    ~BlockedScope() { thread_.setRunning(); }
    BlockedScope(const BlockedScope&) = delete;
    BlockedScope& operator=(const BlockedScope&) = delete;

private:
    SpuThread& thread_;
};

// Timebase ticks in `ns` nanoseconds, split so the product stays in 64 bits.
std::uint64_t ticksFromNs(std::uint64_t ns) {
    constexpr std::uint64_t kBillion = 1000000000ull;
    return (ns / kBillion) * kTimebaseFrequency + (ns % kBillion) * kTimebaseFrequency / kBillion;
}

std::uint64_t nsFromTicks(std::uint64_t ticks) {
    constexpr std::uint64_t kBillion = 1000000000ull;
    return (ticks / kTimebaseFrequency) * kBillion + (ticks % kTimebaseFrequency) * kBillion / kTimebaseFrequency;
}

}  // namespace

SpuChannels::SpuChannels(SpuThread& thread) : thread_(thread), mfc_(thread) {}

void SpuChannels::channelStop(std::uint32_t site, const char* op, std::uint32_t ch, const char* why) const {
    spuStop(thread_, std::format("tox2: stop: spu: {} {} ({}) at ls {:#07x} on thread {:#010x}: {}", op, ch,
                                 channelName(ch), site, thread_.id(), why));
}

// ---------------------------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------------------------

void SpuChannels::sampleReservation() {
    // The MFC forgets a loss once the next atomic command clears the reservation, so the loss is kept
    // here until WrEventAck. It is recorded once per reservation: an acknowledged loss must not come
    // back when the failing PUTLLC looks again.
    if (lost_reported_ || !mfc_.hasReservation()) return;
    if (mfc_.reservationLost()) {
        latched_ |= spu_event::kLockLineLost;
        lost_reported_ = true;
    }
}

std::uint64_t SpuChannels::decrementerTicks() const {
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - dec_set_).count();
    return ticksFromNs(static_cast<std::uint64_t>(std::max<std::int64_t>(elapsed, 0)));
}

void SpuChannels::sampleDecrementer() {
    if (!dec_running_) return;
    // The event is raised each time the counter passes zero, that is 2^32 ticks apart after the first.
    const std::uint64_t elapsed = decrementerTicks();
    if (elapsed <= dec_value_) return;
    const std::uint64_t crossings = ((elapsed - dec_value_ - 1) >> 32) + 1;
    if (crossings > dec_crossings_) {
        dec_crossings_ = crossings;
        latched_ |= spu_event::kDecrementer;
    }
}

bool SpuChannels::decrementerDeadline(Clock::time_point* when) const {
    if (!dec_running_) return false;
    const std::uint64_t next = std::uint64_t{dec_value_} + 1 + (dec_crossings_ << 32);
    // One microsecond of slack so the wake-up lands after the crossing.
    *when = dec_set_ + std::chrono::nanoseconds(nsFromTicks(next) + 1000);
    return true;
}

std::uint32_t SpuChannels::decrementer() const {
    if (!dec_running_) return 0;
    return static_cast<std::uint32_t>(dec_value_ - decrementerTicks());
}

std::uint32_t SpuChannels::eventStatus() {
    sampleReservation();
    sampleDecrementer();
    std::uint32_t status = latched_ | spu_event::kQueueVacancy;
    if (mfc_.tagStatusAvailable()) status |= spu_event::kTagStatusUpdate;
    if (inMboxCount() != 0) status |= spu_event::kInMboxAvailable;
    return status;
}

bool SpuChannels::readEventStat(std::uint32_t site, std::uint32_t* value) {
    const std::uint32_t mask = event_mask_;
    if ((mask & spu_event::kImplemented) == 0) {
        channelStop(site, "rdch", chan::kRdEventStat,
                    std::format("event mask {:#x} has no implemented source (implemented: {:#x})", mask,
                                spu_event::kImplemented)
                            .c_str());
    }
    const bool wants_reservation = (mask & spu_event::kLockLineLost) != 0;
    std::optional<BlockedScope> blocked;
    for (;;) {
        // A mailbox push that lands while this thread waits on the table must wake it. The flag goes up
        // before anything is sampled; the fences pair with the pusher's (a missed wake costs 1 ms).
        const bool on_table = wants_reservation && mfc_.hasReservation();
        table_wait_.store(on_table, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        // Order matters (core/reservation.h): epoch first, then the sampling, then the wait.
        const std::uint64_t seen = reservations().storeEpoch();
        const std::uint32_t status = eventStatus() & mask;
        if (status != 0) {
            table_wait_.store(false, std::memory_order_relaxed);
            *value = status;
            return true;
        }
        if (thread_.haltRequested()) {
            table_wait_.store(false, std::memory_order_relaxed);
            return false;
        }
        if (!blocked) blocked.emplace(thread_, "rdch 0 SPU_RdEventStat", true);

        Clock::time_point dec_when;
        const bool dec_wait = (mask & spu_event::kDecrementer) != 0 && decrementerDeadline(&dec_when);
        if (on_table) {
            std::chrono::microseconds timeout = std::chrono::duration_cast<microseconds>(kPlainStorePoll);
            if (dec_wait) {
                const auto left = std::chrono::duration_cast<microseconds>(dec_when - Clock::now());
                timeout = std::clamp(left, microseconds(0), timeout);
            }
            reservations().waitStoreEpoch(seen, &thread_.context().halt, timeout);
            continue;
        }
        std::unique_lock lock(mutex_);
        const auto ready = [&] {
            return thread_.haltRequested() || ((mask & spu_event::kInMboxAvailable) != 0 && inMboxCount() != 0);
        };
        if (dec_wait) {
            cv_.wait_until(lock, dec_when, ready);
        } else {
            cv_.wait(lock, ready);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Mailboxes
// ---------------------------------------------------------------------------------------------

bool SpuChannels::readInMbox(std::uint32_t* value) {
    std::unique_lock lock(mutex_);
    if (in_count_.load(std::memory_order_relaxed) == 0) {
        BlockedScope blocked(thread_, "rdch 29 SPU_RdInMbox", true);
        cv_.wait(lock, [&] { return in_count_.load(std::memory_order_relaxed) != 0 || thread_.haltRequested(); });
    }
    const std::uint32_t n = in_count_.load(std::memory_order_relaxed);
    if (n == 0) return false;
    *value = in_fifo_[0];
    for (std::uint32_t i = 1; i < n; i++) in_fifo_[i - 1] = in_fifo_[i];
    in_count_.store(n - 1, std::memory_order_release);
    return true;
}

bool SpuChannels::pushInMbox(std::uint32_t value, bool overwrite_newest) {
    bool queued = true;
    {
        std::lock_guard lock(mutex_);
        const std::uint32_t n = in_count_.load(std::memory_order_relaxed);
        if (n < kInMboxDepth) {
            in_fifo_[n] = value;
            in_count_.store(n + 1, std::memory_order_release);
        } else {
            queued = false;
            if (overwrite_newest) in_fifo_[kInMboxDepth - 1] = value;
        }
    }
    cv_.notify_all();
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (table_wait_.load(std::memory_order_seq_cst)) reservations().wake();
    return queued;
}

bool SpuChannels::writeOutMbox(std::uint32_t value) {
    std::unique_lock lock(mutex_);
    if (out_full_) {
        BlockedScope blocked(thread_, "wrch 28 SPU_WrOutMbox", true);
        cv_.wait(lock, [&] { return !out_full_ || thread_.haltRequested(); });
        if (out_full_) return false;
    }
    out_value_ = value;
    out_full_ = true;
    lock.unlock();
    cv_.notify_all();
    return true;
}

bool SpuChannels::takeOutMbox(std::uint32_t* value) {
    {
        std::lock_guard lock(mutex_);
        if (!out_full_) return false;
        *value = out_value_;
        out_full_ = false;
    }
    cv_.notify_all();
    return true;
}

bool SpuChannels::outMboxFull() const {
    std::lock_guard lock(mutex_);
    return out_full_;
}

void SpuChannels::wake() {
    // Taking the mutex orders the wake after a waiter's predicate check, so a waiter that has just seen
    // the halt flag clear is already inside wait() when the notification arrives.
    {
        std::lock_guard lock(mutex_);
    }
    cv_.notify_all();
}

bool SpuChannels::pause(std::chrono::microseconds timeout) {
    BlockedScope blocked(thread_, "owner wait", false);
    std::unique_lock lock(mutex_);
    cv_.wait_for(lock, timeout, [&] { return thread_.haltRequested(); });
    return !thread_.haltRequested();
}

// ---------------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------------

void SpuChannels::writeCommand(std::uint32_t site, std::uint32_t value) {
    // A loss that happens before this command (a failing PUTLLC, or a GETLLAR that replaces the
    // reservation) would otherwise be forgotten.
    sampleReservation();
    mfc_.command(site, value);
    if (!mfc_.hasReservation() || (value & 0xff) == mfc::kGetLLAR) lost_reported_ = false;
}

bool SpuChannels::read(std::uint32_t site, std::uint32_t ch, std::uint32_t* value) {
    if (ch < chan::kCount) reads_[ch].fetch_add(1, std::memory_order_relaxed);
    switch (ch) {
        case chan::kRdEventStat: return readEventStat(site, value);
        case chan::kRdDec: *value = decrementer(); return true;
        case chan::kRdEventMask: *value = event_mask_; return true;
        case chan::kRdTagMask: *value = mfc_.tagMask(); return true;
        case chan::kRdMachStat:
            // Bit 0 is the interrupt-enable state, bit 1 (isolation) is never set.
            *value = thread_.context().int_enabled != 0 ? 1u : 0u;
            return true;
        case chan::kRdSrr0: *value = thread_.context().srr0; return true;
        case chan::kRdTagStat:
            if (!mfc_.takeTagStatus(value)) {
                // Only this thread can request a tag update, so nothing could ever end the wait.
                channelStop(site, "rdch", ch, "no tag update was requested, the read would block forever");
            }
            return true;
        case chan::kRdAtomicStat:
            if (!mfc_.takeAtomicStatus(value)) {
                channelStop(site, "rdch", ch, "no atomic command is pending, the read would block forever");
            }
            return true;
        case chan::kRdInMbox: return readInMbox(value);
        default: channelStop(site, "rdch", ch, "channel is not readable or not implemented");
    }
}

bool SpuChannels::write(std::uint32_t site, std::uint32_t ch, std::uint32_t value) {
    switch (ch) {
        case chan::kWrEventMask: event_mask_ = value; return true;
        case chan::kWrEventAck:
            // Events that happened before the acknowledgement are cleared with it.
            sampleReservation();
            sampleDecrementer();
            latched_ &= ~value;
            return true;
        case chan::kWrDec:
            dec_value_ = value;
            dec_set_ = Clock::now();
            dec_crossings_ = 0;
            dec_running_ = true;
            return true;
        case chan::kWrSrr0: thread_.context().srr0 = value; return true;
        case chan::kMfcLsa: mfc_.writeLsa(value); return true;
        case chan::kMfcEah: mfc_.writeEah(value); return true;
        case chan::kMfcEal: mfc_.writeEal(value); return true;
        case chan::kMfcSize: mfc_.writeSize(value); return true;
        case chan::kMfcTagId: mfc_.writeTagId(value); return true;
        case chan::kMfcCmd: writeCommand(site, value); return true;
        case chan::kWrTagMask: mfc_.writeTagMask(value); return true;
        case chan::kWrTagUpdate: mfc_.writeTagUpdate(site, value); return true;
        case chan::kWrOutMbox: return writeOutMbox(value);
        case chan::kWrOutIntrMbox: {
            SpuOwner* owner = thread_.owner();
            if (owner == nullptr) channelStop(site, "wrch", ch, "the thread has no owner to receive it");
            // No channel lock is held: the owner takes lv2 locks and reads the outbound mailbox.
            owner->onInterruptMailbox(thread_, value);
            return !thread_.haltRequested();
        }
        default: channelStop(site, "wrch", ch, "channel is not writable or not implemented");
    }
}

std::uint32_t SpuChannels::count(std::uint32_t ch) {
    switch (ch) {
        case chan::kRdEventStat: return (eventStatus() & event_mask_) != 0 ? 1u : 0u;
        case chan::kWrEventMask:
        case chan::kWrEventAck:
        case chan::kWrDec:
        case chan::kRdDec:
        case chan::kRdEventMask:
        case chan::kRdTagMask:
        case chan::kRdMachStat:
        case chan::kWrSrr0:
        case chan::kRdSrr0:
        case chan::kMfcLsa:
        case chan::kMfcEah:
        case chan::kMfcEal:
        case chan::kMfcSize:
        case chan::kMfcTagId:
        case chan::kWrTagMask:
        case chan::kWrTagUpdate: return 1;
        case chan::kMfcCmd: return mfc_.commandSlots();
        case chan::kRdTagStat: return mfc_.tagStatusAvailable() ? 1u : 0u;
        case chan::kRdAtomicStat: return mfc_.atomicStatusAvailable() ? 1u : 0u;
        case chan::kWrOutMbox: return outMboxFull() ? 0u : 1u;
        case chan::kRdInMbox: return inMboxCount();
        // The owner takes the value inside the write, so the mailbox is empty again when the SPU looks.
        case chan::kWrOutIntrMbox: return 1;
        default: channelStop(thread_.context().jump_site, "rchcnt", ch, "channel is not implemented");
    }
}

bool SpuChannels::stop(std::uint32_t site, std::uint32_t code) {
    SpuOwner* owner = thread_.owner();
    if (owner == nullptr) {
        spuStop(thread_, std::format("tox2: stop: spu: stop {:#x} at ls {:#07x} on thread {:#010x}: not implemented",
                                     code, site, thread_.id()));
    }
    return owner->onStop(thread_, site, code);
}

}  // namespace tox2

extern "C" {

bool tox2SpuReadChannel(Tox2SpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t ch, uint32_t* value) {
    (void)ls;
    return tox2::spuThreadOf(ctx)->channels().read(site, ch, value);
}

bool tox2SpuWriteChannel(Tox2SpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t ch, uint32_t value) {
    (void)ls;
    return tox2::spuThreadOf(ctx)->channels().write(site, ch, value);
}

uint32_t tox2SpuChannelCount(Tox2SpuContext* ctx, uint8_t* ls, uint32_t ch) {
    (void)ls;
    return tox2::spuThreadOf(ctx)->channels().count(ch);
}

bool tox2SpuStop(Tox2SpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t code) {
    (void)ls;
    return tox2::spuThreadOf(ctx)->channels().stop(site, code);
}

}  // extern "C"
