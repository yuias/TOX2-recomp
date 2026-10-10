#pragma once

// Atomic reservations for SPU GETLLAR/PUTLLC and the PPU store-conditionals, plus the line-granular
// writes (PUT, PUTLLUC) that have to invalidate them.
//
// Guest memory is addressed as `base + ea` and holds big-endian bytes. Line and chunk logic works on raw
// bytes; only storeConditional() takes host-order numbers and swaps them.
//
// Every reservation-aware writer takes the stripe lock of each 128-byte line it writes. PPU plain stores
// and host plain writes are not instrumented: they change neither the stripe version nor the store
// epoch. The consequences are documented at putllc() and waitStoreEpoch().

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace tox2 {

inline constexpr std::uint32_t kReservationLineSize = 128;
inline constexpr std::uint32_t kReservationStripeCount = 65536;

// What an SPU thread keeps between GETLLAR and the next atomic command.
struct Reservation {
    bool valid = false;
    std::uint32_t line = 0;     // guest address of the 128-byte line
    std::uint64_t version = 0;  // stripe version seen by GETLLAR (always even)
    alignas(16) std::uint8_t snapshot[kReservationLineSize] = {};
};

// Why waitStoreEpoch() returned.
enum class StoreWait { Epoch, Timeout, Halt };

class ReservationTable {
public:
    ReservationTable();
    ReservationTable(const ReservationTable&) = delete;
    ReservationTable& operator=(const ReservationTable&) = delete;

    // Reads the 128-byte line that contains `ea` into `dst` (any alignment) and records the reservation.
    // The copy is consistent: no writer held the stripe while it was taken.
    void getllar(const std::uint8_t* base, std::uint32_t ea, std::uint8_t* dst, Reservation& res);

    // PUTLLC of the line that contains `ea`; `src` holds the 128 new bytes. Fails at once when the
    // reservation is gone, names another line, or the stripe version moved; fails when the line differs
    // from the snapshot. On success every aligned 8-byte chunk whose new value differs from the snapshot
    // is written with one atomic store; chunks it leaves unchanged are never touched. The reservation is
    // cleared in every case.
    //
    // Known race window: a plain store that lands after the snapshot compare and before the chunk store
    // is overwritten when it hits a chunk this PUTLLC changes. The window is a few dozen instructions.
    // Plain stores before the compare make the PUTLLC fail; plain stores to other chunks are kept.
    bool putllc(std::uint8_t* base, std::uint32_t ea, const std::uint8_t* src, Reservation& res);

    // PUTLLUC: unconditional write of the 128-byte line that contains `ea`. Always invalidates the
    // reservations on that line, even when the bytes are identical.
    void putlluc(std::uint8_t* base, std::uint32_t ea, const std::uint8_t* src);

    // PUT of `size` bytes (1, 2, 4, 8 or a multiple of 16; natural alignment of `base + ea` for the
    // small sizes). Each 128-byte line the range touches is written under its own stripe lock and
    // invalidates the reservations on it. The range is not atomic as a whole. This is also the
    // reservation-aware replacement for a plain store of up to 8 bytes: byte-swap the value into `src`.
    void put(std::uint8_t* base, std::uint32_t ea, const std::uint8_t* src, std::uint32_t size);

    // PPU stwcx./stdcx.: writes `value` when the `size` (4 or 8) bytes at `ea` still equal `expected`,
    // under the stripe lock. Both values are host-order numbers; memory is big-endian. Returns whether
    // the store happened. Success bumps the stripe version and the store epoch.
    bool storeConditional(std::uint8_t* base, std::uint32_t ea, std::uint32_t size, std::uint64_t expected,
                          std::uint64_t value);

    // True when the reservation is gone: the stripe version changed or the line differs from the
    // snapshot. The content compare is what sees plain stores. A reservation that is not valid is
    // never lost.
    bool lost(const std::uint8_t* base, const Reservation& res) const;

    // Count of version-bumping writes (and wake() calls) so far. Must stay an acquire load: a writer bumps
    // the stripe version before the epoch, so a waiter that reads the epoch first and lost() second can
    // never miss a write that the later waitStoreEpoch() would otherwise have reported.
    std::uint64_t storeEpoch() const { return epoch_.load(std::memory_order_acquire); }

    // Blocks until the store epoch differs from `seen`, `timeout` passes, or `*halt` (may be null) is
    // non-zero. Plain stores do not change the epoch, so a caller that waits for a plain store gets
    // StoreWait::Timeout and must re-check lost() on every return, whatever the reason.
    //
    // Waiter protocol, in this order: `seen = storeEpoch()`, then `lost()`, then `waitStoreEpoch(seen, ...)`.
    // Reading the epoch after lost() can sleep through a write that landed in between.
    //
    // Halt: the setter must store the flag atomically (the waiter reads it with an acquire load) and only
    // then call wake(), or the waiter notices it only at its timeout.
    StoreWait waitStoreEpoch(std::uint64_t seen, const std::uint32_t* halt,
                             std::chrono::microseconds timeout = std::chrono::milliseconds(1));

    // Bumps the epoch and wakes every waiter without a write.
    void wake() { publish(); }

    // Test seam: `fn(arg)` runs inside putllc() after the snapshot compare and before the chunk stores,
    // which makes the race window deterministic. Set it before any other thread uses the table.
    void setCompareHook(void (*fn)(void*), void* arg) {
        compare_hook_ = fn;
        compare_hook_arg_ = arg;
    }

private:
    std::atomic<std::uint64_t>& stripe(std::uint32_t ea) { return stripes_[(ea >> 7) & (kReservationStripeCount - 1)]; }
    const std::atomic<std::uint64_t>& stripe(std::uint32_t ea) const {
        return stripes_[(ea >> 7) & (kReservationStripeCount - 1)];
    }

    // Spins while the stripe is locked, then locks it; returns the even version it had.
    std::uint64_t lock(std::atomic<std::uint64_t>& s);
    void unlockWithWrite(std::atomic<std::uint64_t>& s);
    void unlockClean(std::atomic<std::uint64_t>& s);
    // Bumps the epoch and wakes waiters (the version is already updated, see waitStoreEpoch()).
    void publish();

    std::unique_ptr<std::atomic<std::uint64_t>[]> stripes_;  // bit 0 lock, bits 1..63 version
    std::atomic<std::uint64_t> epoch_{0};
    std::atomic<std::uint32_t> waiters_{0};
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    void (*compare_hook_)(void*) = nullptr;
    void* compare_hook_arg_ = nullptr;
};

// The process-wide table that PPU and SPU threads share.
ReservationTable& reservations();

}  // namespace tox2
