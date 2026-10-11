#pragma once

// The channels of one SPU thread: mailboxes, the event facility, the decrementer, the machine-status and
// SRR0 channels, and the forwarding of the MFC channels to SpuMfc. Generated code reaches it through
// tox2SpuReadChannel, tox2SpuWriteChannel, tox2SpuChannelCount and tox2SpuStop.
//
// Every blocking wait uses the thread's own mutex and condition variable (or, for a wait on the
// lock-line-lost event, the reservation table's store epoch), never the guest lock, and ends when a halt
// is requested: the hook then returns false and the generated code leaves.
//
// Threads: the SPU's own host thread calls read/write/count/stop. Any thread may call pushInMbox,
// takeOutMbox, outMboxFull, inMboxCount and wake.

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "spu/spu_mfc.h"
#include "spu/spu_thread.h"

namespace tox2 {

// Channel numbers (CBEA).
namespace chan {
inline constexpr std::uint32_t kRdEventStat = 0;
inline constexpr std::uint32_t kWrEventMask = 1;
inline constexpr std::uint32_t kWrEventAck = 2;
inline constexpr std::uint32_t kRdSigNotify1 = 3;
inline constexpr std::uint32_t kRdSigNotify2 = 4;
inline constexpr std::uint32_t kWrDec = 7;
inline constexpr std::uint32_t kRdDec = 8;
inline constexpr std::uint32_t kWrMsSyncReq = 9;
inline constexpr std::uint32_t kRdEventMask = 11;
inline constexpr std::uint32_t kRdTagMask = 12;
inline constexpr std::uint32_t kRdMachStat = 13;
inline constexpr std::uint32_t kWrSrr0 = 14;
inline constexpr std::uint32_t kRdSrr0 = 15;
inline constexpr std::uint32_t kMfcLsa = 16;
inline constexpr std::uint32_t kMfcEah = 17;
inline constexpr std::uint32_t kMfcEal = 18;
inline constexpr std::uint32_t kMfcSize = 19;
inline constexpr std::uint32_t kMfcTagId = 20;
inline constexpr std::uint32_t kMfcCmd = 21;
inline constexpr std::uint32_t kWrTagMask = 22;
inline constexpr std::uint32_t kWrTagUpdate = 23;
inline constexpr std::uint32_t kRdTagStat = 24;
inline constexpr std::uint32_t kRdListStallStat = 25;
inline constexpr std::uint32_t kWrListStallAck = 26;
inline constexpr std::uint32_t kRdAtomicStat = 27;
inline constexpr std::uint32_t kWrOutMbox = 28;
inline constexpr std::uint32_t kRdInMbox = 29;
inline constexpr std::uint32_t kWrOutIntrMbox = 30;
inline constexpr std::uint32_t kCount = 32;
}  // namespace chan

// Event bits of RdEventStat / WrEventMask / WrEventAck.
namespace spu_event {
inline constexpr std::uint32_t kTagStatusUpdate = 0x1;
inline constexpr std::uint32_t kQueueVacancy = 0x8;
inline constexpr std::uint32_t kInMboxAvailable = 0x10;
inline constexpr std::uint32_t kDecrementer = 0x20;
inline constexpr std::uint32_t kLockLineLost = 0x400;
// Sources that exist here; a wait on a mask without any of them could never end.
inline constexpr std::uint32_t kImplemented =
        kTagStatusUpdate | kQueueVacancy | kInMboxAvailable | kDecrementer | kLockLineLost;
}  // namespace spu_event

inline constexpr std::uint32_t kInMboxDepth = 4;

class SpuChannels {
public:
    explicit SpuChannels(SpuThread& thread);

    // The thread's MFC. Whoever starts the thread supplies the guest base: mfc().setGuestBase(base).
    SpuMfc& mfc() { return mfc_; }
    const SpuMfc& mfc() const { return mfc_; }

    // The hooks' implementations. read/write return false when the thread must leave (a halt ended a
    // wait); *value is then not written.
    bool read(std::uint32_t site, std::uint32_t ch, std::uint32_t* value);
    bool write(std::uint32_t site, std::uint32_t ch, std::uint32_t value);
    std::uint32_t count(std::uint32_t ch);
    // A `stop` instruction: the owner decides. False: the thread leaves.
    bool stop(std::uint32_t site, std::uint32_t code);

    // Inbound mailbox, filled by the PPU or by lv2 replies. True when the value was queued; false when
    // the mailbox was full: the value is dropped, or replaces the newest entry when `overwrite_newest`
    // is set (what the hardware does for a PPU write to a full SPU inbound mailbox).
    bool pushInMbox(std::uint32_t value, bool overwrite_newest = false);
    std::uint32_t inMboxCount() const { return in_count_.load(std::memory_order_acquire); }
    // Makes `value` the only entry of the inbound mailbox (the lv2 busy reply to a send_event).
    void replaceInMbox(std::uint32_t value);

    // Outbound mailbox (one entry). takeOutMbox consumes it and lets a blocked WrOutMbox go on.
    bool takeOutMbox(std::uint32_t* value);
    bool outMboxFull() const;

    // Wakes the SPU thread's channel waits; called after a halt flag was stored.
    void wake();
    // For the owner's callbacks: sleeps up to `timeout`, shown to the supervisor as a wait. False when a
    // halt was requested.
    bool pause(std::chrono::microseconds timeout);

    // State the SPU thread reads and writes; for its own thread and for tests.
    std::uint32_t eventMask() const { return event_mask_; }
    // Pending events, before the mask: sticky bits (lock-line-lost, decrementer) and levels.
    std::uint32_t eventStatus();
    std::uint32_t decrementer() const;

    // Census: reads of each channel by the SPU thread (relaxed).
    std::uint64_t readCount(std::uint32_t ch) const {
        return ch < chan::kCount ? reads_[ch].load(std::memory_order_relaxed) : 0;
    }

private:
    using Clock = std::chrono::steady_clock;

    [[noreturn]] void channelStop(std::uint32_t site, const char* op, std::uint32_t ch, const char* why) const;
    bool readEventStat(std::uint32_t site, std::uint32_t* value);
    bool readInMbox(std::uint32_t* value);
    bool writeOutMbox(std::uint32_t value);
    void writeCommand(std::uint32_t site, std::uint32_t value);
    // Set a sticky bit the first time the reservation is seen lost.
    void sampleReservation();
    void sampleDecrementer();
    std::uint64_t decrementerTicks() const;
    // When the decrementer next passes zero; false when it is not running.
    bool decrementerDeadline(Clock::time_point* when) const;

    SpuThread& thread_;
    SpuMfc mfc_;

    // Written by the SPU's own thread only.
    std::uint32_t event_mask_ = 0;
    std::uint32_t latched_ = 0;
    bool lost_reported_ = false;
    bool dec_running_ = false;
    std::uint32_t dec_value_ = 0;
    std::uint64_t dec_crossings_ = 0;
    Clock::time_point dec_set_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::array<std::uint32_t, kInMboxDepth> in_fifo_{};
    std::atomic<std::uint32_t> in_count_{0};  // changed under mutex_
    std::uint32_t out_value_ = 0;
    bool out_full_ = false;
    // True while the SPU thread waits for the lock-line-lost event on the reservation table; a mailbox
    // push then wakes the table as well.
    std::atomic<bool> table_wait_{false};

    std::array<std::atomic<std::uint64_t>, chan::kCount> reads_{};
};

}  // namespace tox2
