#pragma once

// The memory flow controller (MFC) of one SPU thread: DMA between the local store (LS) and guest memory,
// the atomic commands and the tag-group status.
//
// Every command runs to completion inside command(), on the SPU's own host thread, so a transfer is
// finished before the SPU can look at its tag status. Barrier and fence variants therefore need no
// ordering logic beyond a host fence. The channel layer owns one SpuMfc per thread and forwards the MFC
// channels to it:
//
//   16 MFC_LSA     writeLsa          21 MFC_Cmd        command (site, value)      count: commandSlots
//   17 MFC_EAH     writeEah          22 WrTagMask      writeTagMask
//   18 MFC_EAL     writeEal          12 RdTagMask      tagMask
//   19 MFC_Size    writeSize         23 WrTagUpdate    writeTagUpdate
//   20 MFC_TagID   writeTagId        24 RdTagStat      takeTagStatus              count: tagStatusAvailable
//                                    27 RdAtomicStat   takeAtomicStatus           count: atomicStatusAvailable
//
// Everything the MFC cannot execute is a fatal stop on the thread (spuStop), never a silent skip.

#include <atomic>
#include <cstdint>

#include "core/reservation.h"
#include "spu/spu_thread.h"

namespace tox2 {

// MFC command opcodes (low 8 bits of the value written to MFC_Cmd; bits 16..31 hold the cache class ids
// and do not matter here).
namespace mfc {
inline constexpr std::uint32_t kPut = 0x20;
inline constexpr std::uint32_t kPutB = 0x21;
inline constexpr std::uint32_t kPutF = 0x22;
inline constexpr std::uint32_t kPutL = 0x24;
inline constexpr std::uint32_t kPutLB = 0x25;
inline constexpr std::uint32_t kPutLF = 0x26;
inline constexpr std::uint32_t kGet = 0x40;
inline constexpr std::uint32_t kGetB = 0x41;
inline constexpr std::uint32_t kGetF = 0x42;
inline constexpr std::uint32_t kGetL = 0x44;
inline constexpr std::uint32_t kGetLB = 0x45;
inline constexpr std::uint32_t kGetLF = 0x46;
inline constexpr std::uint32_t kPutLLUC = 0xb0;
inline constexpr std::uint32_t kPutLLC = 0xb4;
inline constexpr std::uint32_t kPutQLLUC = 0xb8;
inline constexpr std::uint32_t kBarrier = 0xc0;
inline constexpr std::uint32_t kMfcEieio = 0xc8;
inline constexpr std::uint32_t kMfcSync = 0xcc;
inline constexpr std::uint32_t kGetLLAR = 0xd0;

// RdAtomicStat values.
inline constexpr std::uint32_t kPutLLCSuccess = 0;
inline constexpr std::uint32_t kPutLLCFailed = 1;
inline constexpr std::uint32_t kPutLLUCDone = 2;
inline constexpr std::uint32_t kGetLLARDone = 4;

inline constexpr std::uint32_t kMaxTransfer = 0x4000;
inline constexpr std::uint32_t kQueueSlots = 16;
inline constexpr std::uint32_t kTagCount = 32;
}  // namespace mfc

// Counters for the census. Written by the owning host thread, read by anyone.
struct SpuMfcCounters {
    std::atomic<std::uint64_t> get{0};
    std::atomic<std::uint64_t> put{0};
    std::atomic<std::uint64_t> get_list{0};
    std::atomic<std::uint64_t> put_list{0};
    std::atomic<std::uint64_t> getllar{0};
    std::atomic<std::uint64_t> putllc_ok{0};
    std::atomic<std::uint64_t> putllc_failed{0};
    std::atomic<std::uint64_t> putlluc{0};
    std::atomic<std::uint64_t> fences{0};
};

class SpuMfc {
public:
    // `guest_base` is the host address of guest address 0 (null until setGuestBase()).
    explicit SpuMfc(SpuThread& thread, std::uint8_t* guest_base = nullptr) : thread_(thread), base_(guest_base) {}

    void setGuestBase(std::uint8_t* guest_base) { base_ = guest_base; }

    // Command parameter channels. The tag id is checked when the command runs.
    void writeLsa(std::uint32_t value) { lsa_ = value; }
    void writeEah(std::uint32_t value) { eah_ = value; }
    void writeEal(std::uint32_t value) { eal_ = value; }
    void writeSize(std::uint32_t value) { size_ = value; }
    void writeTagId(std::uint32_t value) { tag_ = value; }

    // MFC_Cmd: `site` is the LS address of the writing instruction (for stop lines), `value` the channel
    // value. Executes the command synchronously. Transfers move the bytes, tag the LS for image
    // tracking (GET) and take the reservation stripes (PUT, atomic commands).
    void command(std::uint32_t site, std::uint32_t value);
    // rchcnt MFC_Cmd: queue slots free. Commands complete at once, so the queue is always empty.
    std::uint32_t commandSlots() const { return mfc::kQueueSlots; }

    // Tag groups. Every transfer is complete when the SPU gets to ask, so the completed set is every tag.
    void writeTagMask(std::uint32_t mask) { tag_mask_ = mask; }
    std::uint32_t tagMask() const { return tag_mask_; }
    // WrTagUpdate: 0 immediate, 1 any, 2 all. All three are satisfied at once; the status is the mask.
    // Any other value is a fatal stop.
    void writeTagUpdate(std::uint32_t site, std::uint32_t mode);
    // rchcnt RdTagStat; also the level of the tag-group status event (0x1).
    bool tagStatusAvailable() const { return tag_status_pending_; }
    // RdTagStat: false when no update was requested (the channel layer decides how to block). A true
    // return consumes the status.
    bool takeTagStatus(std::uint32_t* value);

    // Status of the last atomic command (GETLLAR 4, PUTLLC 0 or 1, PUTLLUC 2). PUTQLLUC sets none.
    bool atomicStatusAvailable() const { return atomic_status_pending_; }
    bool takeAtomicStatus(std::uint32_t* value);

    // The reservation made by the last GETLLAR, for the lock-line-lost event (0x400): true when the line
    // changed since. It stays true until the next atomic command clears or replaces the reservation.
    // Waiters follow the order in core/reservation.h: storeEpoch(), then this, then waitStoreEpoch().
    bool hasReservation() const { return reservation_.valid; }
    bool reservationLost() const { return reservations().lost(base_, reservation_); }
    const Reservation& reservation() const { return reservation_; }

    const SpuMfcCounters& counters() const { return counters_; }

private:
    struct Params {
        std::uint32_t lsa, ea, size, tag;
    };

    [[noreturn]] void stop(std::uint32_t site, std::uint32_t opcode, const Params& p, const char* why) const;
    // Size, alignment and tag rules for one transfer (the plain command or one list element).
    void checkTransfer(std::uint32_t site, std::uint32_t opcode, const Params& p) const;
    void transfer(bool to_ls, std::uint32_t site, std::uint32_t opcode, const Params& p);
    void list(bool to_ls, std::uint32_t site, std::uint32_t opcode);
    void atomic(std::uint32_t site, std::uint32_t opcode);

    SpuThread& thread_;
    std::uint8_t* base_;
    std::uint32_t lsa_ = 0, eah_ = 0, eal_ = 0, size_ = 0, tag_ = 0;
    std::uint32_t tag_mask_ = 0;
    std::uint32_t tag_status_ = 0;
    bool tag_status_pending_ = false;
    std::uint32_t atomic_status_ = 0;
    bool atomic_status_pending_ = false;
    Reservation reservation_;
    SpuMfcCounters counters_;
};

}  // namespace tox2
