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

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

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

// What a lost reservation looks like now, for diagnostics. Chunks are raw 8-byte loads (guest byte order).
struct ReservationLoss {
    bool version_moved = false;        // a reservation-aware write bumped the stripe since GETLLAR
    std::uint16_t changed_chunks = 0;  // bit i: chunk i (bytes 8i..8i+7) differs from the snapshot now
    std::array<std::uint64_t, kReservationLineSize / 8> now{};  // the line's chunks as loaded for the compare
};

// Why waitStoreEpoch() returned.
enum class StoreWait { Epoch, Timeout, Halt };

// Passed as `watch_line` by a waiter that wants every store epoch change. Line addresses are 128-aligned, so
// this value never names a line.
inline constexpr std::uint32_t kWatchAnyLine = 0xFFFFFFFF;
// Waiters that name a line take one of these slots; the rest count as wildcards.
inline constexpr std::uint32_t kWatchSlots = 16;

// What one line-watching waiter sleeps on. A slot is claimed by at most one waiter at a time; each slot sits on its own
// cache line so that a writer bumping one waiter's word does not disturb the others.
struct alignas(64) WatchSlot {
    std::atomic<std::uint32_t> key{0};   // stripe + 1 of the watched line; 0 is a free slot
    std::atomic<std::uint32_t> wake{0};  // generation: a writer that hits the stripe bumps it and wakes the address
};
static_assert(sizeof(std::atomic<std::uint32_t>) == 4 && std::atomic<std::uint32_t>::is_always_lock_free,
              "the wake words are waited on by address");

// What bumped the epoch; notifies are counted per kind.
enum class WriterKind : std::uint8_t { Putllc, Put, Putlluc, PpuStoreConditional, Wake };
inline constexpr std::size_t kWriterKindCount = 5;

// Why a PUTLLC failed, by the first check that rejected it.
enum class PutllcFail : std::uint8_t {
    Lost,    // no valid reservation, or one on another line
    Locked,  // the stripe was held by another writer
    Moved,   // the stripe version changed since GETLLAR
    Data     // the line differs from the snapshot
};
inline constexpr std::size_t kPutllcFailCount = 4;

// Cost counters: epoch bumps, and the bumps that found a waiter and notified (`notifies` is the sum of the kinds).
struct ReservationStats {
    std::uint64_t publishes = 0;
    std::uint64_t notifies = 0;
    std::array<std::uint64_t, kWriterKindCount> notifies_by_kind{};
    std::array<std::uint64_t, kPutllcFailCount> putllc_fails{};
};

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
    // invalidates the reservations on it. The range is not atomic as a whole. The store epoch moves once, after
    // the last line, not once per line: every version has moved before the bump, which is all the waiter
    // protocol needs. This is also the reservation-aware replacement for a plain store of up to 8 bytes:
    // byte-swap the value into `src`.
    void put(std::uint8_t* base, std::uint32_t ea, const std::uint8_t* src, std::uint32_t size);

    // PPU lwarx/ldarx: loads the `size` (4 or 8) bytes at `ea` as a host-order number and returns the
    // version of the stripe that covers the line. The version is read before the word and re-checked
    // after it, so the pair is consistent and no write that completed before the call can be missed.
    void loadReserved(const std::uint8_t* base, std::uint32_t ea, std::uint32_t size, std::uint64_t& value,
                      std::uint64_t& version);

    // PPU stwcx./stdcx.: writes `value` when the stripe still has `expected_version` (from loadReserved())
    // and the `size` (4 or 8) bytes at `ea` still equal `expected`, under the stripe lock. Any
    // reservation-aware write to the line since the load bumped the version, so the store fails like it
    // would on hardware; a line that shares the stripe fails it too. The word compare catches plain
    // stores to the word, which do not bump versions. Values are host-order numbers; memory is big-endian.
    // Returns whether the store happened. Success bumps the stripe version and the store epoch.
    bool storeConditional(std::uint8_t* base, std::uint32_t ea, std::uint32_t size, std::uint64_t expected,
                          std::uint64_t expected_version, std::uint64_t value);

    // True when the reservation is gone: the stripe version changed or the line differs from the
    // snapshot. The content compare is what sees plain stores. A reservation that is not valid is
    // never lost. Equals versionMoved() || contentDiffers(); both are false for an invalid reservation.
    bool lost(const std::uint8_t* base, const Reservation& res) const;
    bool versionMoved(const Reservation& res) const;
    bool contentDiffers(const std::uint8_t* base, const Reservation& res) const;

    // Diagnostic version of lost() for the rare timeout path. It first waits, read-only, while the stripe is locked,
    // so that a reservation-aware write in progress is seen as a version move and not as a plain store. May block
    // as long as a writer holds the stripe. Not for the hot wait path.
    ReservationLoss describeLoss(const std::uint8_t* base, const Reservation& res) const;

    // Count of reservation-aware write operations (one PUT counts once, however many lines it spans) and
    // wake() calls so far. Must stay an acquire load: a writer bumps every stripe version it touches before the epoch,
    // so a waiter that reads the epoch first and lost() second can never miss a write that the later waitStoreEpoch()
    // would otherwise have reported.
    std::uint64_t storeEpoch() const { return epoch_.load(std::memory_order_acquire); }

    // Blocks until a write to a watched stripe (any write for a wildcard), wake(), `timeout`, or a non-zero `*halt`
    // (may be null). Plain stores do not change the epoch, so a caller that waits for a plain store gets
    // StoreWait::Timeout and must re-check lost() on every return, whatever the reason.
    //
    // A call that returns without sleeping reports Epoch when the epoch already differs from `seen`. After sleeping,
    // it returns Epoch only when a write to a stripe the waiter watches (any write for a wildcard) or wake() came
    // in; a write elsewhere moves the epoch but does not end the sleep, and at the deadline the result is Timeout
    // even if the epoch moved. Halt is reported whenever the flag is set when the wait ends.
    //
    // Waiter protocol, in this order: `seen = storeEpoch()`, then `lost()`, then `waitStoreEpoch(seen, ...)`.
    // Reading the epoch after lost() can sleep through a write that landed in between.
    //
    // Watch line: a waiter whose only interest is the reservation on one line passes that line's address as
    // `watch_line`. A writer then wakes it only when it wrote a line of the same stripe (the granularity of
    // lost()). Writes elsewhere still move the epoch but leave it asleep, so anything else that has to end its
    // wait (mailbox, halt) must go through wake(). kWatchAnyLine, or a table with all kWatchSlots taken, makes
    // the waiter a wildcard that every epoch change wakes.
    //
    // Halt: the setter must store the flag atomically (the waiter reads it with an acquire load) and only
    // then call wake(), or the waiter notices it only at its timeout.
    //
    // `slept` (may be null) is set to false on entry and to true just before the first sleep. It tells a result of
    // the checks before sleeping (Halt, or Epoch because any write moved the epoch since `seen`; Timeout for a zero
    // timeout) from one after sleeping.
    StoreWait waitStoreEpoch(std::uint64_t seen, const std::uint32_t* halt,
                             std::chrono::microseconds timeout = std::chrono::milliseconds(1),
                             std::uint32_t watch_line = kWatchAnyLine, bool* slept = nullptr);

    // Bumps the epoch and wakes every waiter without a write, whatever line it watches.
    void wake();

    ReservationStats stats() const;

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
    // Version bump only; the caller publishes once its writes are all done.
    void unlockVersion(std::atomic<std::uint64_t>& s);
    void unlockWithWrite(std::atomic<std::uint64_t>& s, std::uint32_t line, WriterKind kind);
    void unlockClean(std::atomic<std::uint64_t>& s);
    // Bumps the epoch and wakes the waiters that watch a stripe of the inclusive line range (the versions are
    // already updated, see waitStoreEpoch()).
    void publish(std::uint32_t first_line, std::uint32_t last_line, WriterKind kind);
    void countPutllcFail(PutllcFail why) {
        diag_.putllc_fails[static_cast<std::size_t>(why)].fetch_add(1, std::memory_order_relaxed);
    }

    std::unique_ptr<std::atomic<std::uint64_t>[]> stripes_;  // bit 0 lock, bits 1..63 version
    std::atomic<std::uint64_t> epoch_{0};
    std::array<WatchSlot, kWatchSlots> watch_;
    std::atomic<std::uint32_t> wildcard_waiters_{0};
    // Bumped by every publish while a wildcard waits, and by wake(); the wildcards sleep on it.
    std::atomic<std::uint32_t> wildcard_wake_{0};
    std::atomic<std::uint64_t> publishes_{0};
    // Own cache lines: the failure counters are bumped from several SPU threads and must not slow the epoch.
    struct alignas(64) Diagnostics {
        std::array<std::atomic<std::uint64_t>, kWriterKindCount> notifies{};
        std::array<std::atomic<std::uint64_t>, kPutllcFailCount> putllc_fails{};
    } diag_;
    void (*compare_hook_)(void*) = nullptr;
    void* compare_hook_arg_ = nullptr;
};

// The process-wide table that PPU and SPU threads share.
ReservationTable& reservations();

// Totals over all host threads that ran PPU reservation instructions. `lwarx` counts ldarx too, `stwcx` counts stdcx.
// too; `stwcx_failed` includes store-conditionals that fail without reaching the table.
struct PpuReservationStats {
    std::uint64_t lwarx = 0, stwcx = 0, stwcx_failed = 0;
};

PpuReservationStats ppuReservationStats();

// Diagnostic that finds guest code touching a live sys_lwmutex_t other than through the lwmutex calls. The
// lwmutex fast path updates the owner/waiter pair with host compare-and-swap, which is only sound while no
// recompiled PPU or SPU code reads or writes those words itself (the reservation hooks do not see the pair).
// Off by default; when off, each hook costs one relaxed load.
enum class LwmutexWatchKind : std::uint8_t {
    Lwarx,  // lwarx and ldarx
    Stwcx,  // stwcx. and stdcx.
    Dcbz,
    SpuGetllar,  // read-only, reported but not a write
    SpuPutllc,
    SpuPutlluc,
    SpuPut,
    HleCopy,
    OwnerValue,
    WaiterValue
};
inline constexpr std::size_t kLwmutexWatchKindCount = 10;
// Size of sys_lwmutex_t. Kept here so core/ does not depend on lv2/.
inline constexpr std::uint32_t kLwmutexWatchSize = 24;

namespace detail {
inline std::atomic<bool> g_lwmutex_watch{false};
}  // namespace detail

void setLwmutexWatchEnabled(bool enabled);  // before boot
inline bool lwmutexWatchEnabled() { return detail::g_lwmutex_watch.load(std::memory_order_relaxed); }

// The live set is the structs between create and destroy; a re-add of a live address is ignored.
void lwmutexWatchAdd(std::uint32_t lwmutex);
void lwmutexWatchRemove(std::uint32_t lwmutex);

// Counts one hit of `kind` unconditionally and logs the first 16 (kind, address, struct, offset, ctx lr when
// known; the log prefix names the calling thread). The value checks use it, since they have already decided.
void lwmutexWatchHit(LwmutexWatchKind kind, std::uint32_t ea, std::uint32_t size, std::uint64_t lr);
// lwmutexWatchHit() when [ea, ea + size) overlaps a live struct. Returns whether it did. Only call it when
// lwmutexWatchEnabled(); it takes a mutex.
bool lwmutexWatchCheck(LwmutexWatchKind kind, std::uint32_t ea, std::uint32_t size, std::uint64_t lr);

struct LwmutexWatchStats {
    std::uint64_t live = 0;
    std::uint64_t max_live = 0;
    std::array<std::uint64_t, kLwmutexWatchKindCount> hits{};
};

LwmutexWatchStats lwmutexWatchStats();

}  // namespace tox2
