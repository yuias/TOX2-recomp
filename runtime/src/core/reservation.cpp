#include "core/reservation.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <immintrin.h>

#include "core/log.h"

namespace tox2 {

namespace {

constexpr std::uint32_t kChunkCount = kReservationLineSize / 8;

// Guest bytes may be read by one thread while another stores to them, so every access to shared
// memory is an atomic access of its natural width. Host pointers into a line are 8-byte aligned.
std::uint64_t loadChunk(const std::uint8_t* p) {
    return __atomic_load_n(reinterpret_cast<const std::uint64_t*>(p), __ATOMIC_RELAXED);
}

void storeChunk(std::uint8_t* p, std::uint64_t value) {
    __atomic_store_n(reinterpret_cast<std::uint64_t*>(p), value, __ATOMIC_RELEASE);
}

// Unaligned read of a caller buffer (LS data, snapshot): no atomicity needed, the thread owns it.
std::uint64_t readBuffer(const std::uint8_t* p) {
    std::uint64_t value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

// A stripe is held for a few hundred nanoseconds, so a short spin is the right wait. A holder that was
// preempted is another matter: yield() only gives way to threads that are ready on this processor, and a
// raised SPU thread (host_priority.h) could keep the holder from running, so a long wait ends in a sleep.
void spinOnce(std::uint32_t& spins) {
    if (++spins < 64) {
        _mm_pause();
    } else if (spins < 64 + 64) {
        std::this_thread::yield();
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool haltRequested(const std::uint32_t* halt) {
    return halt != nullptr && __atomic_load_n(halt, __ATOMIC_ACQUIRE) != 0;
}

// PUT and PUTLLUC reach this path too, so they also skip equal-valued pieces. That narrows the window in
// which they undo a concurrent plain store; the stripe version and the store epoch still bump.
template <typename T>
void storeSkipEqual(std::uint8_t* dst, const std::uint8_t* src) {
    T value;
    std::memcpy(&value, src, sizeof(T));
    T* target = reinterpret_cast<T*>(dst);
    if (__atomic_load_n(target, __ATOMIC_RELAXED) != value) __atomic_store_n(target, value, __ATOMIC_RELEASE);
}

// Writes `size` bytes in the widest naturally aligned atomic pieces. A piece whose bytes already equal
// the new ones is skipped, so a concurrent plain store to it is not undone by an equal-valued write.
void storeBytes(std::uint8_t* dst, const std::uint8_t* src, std::uint32_t size) {
    while (size != 0) {
        std::uint32_t width = 8;
        while (width > size || (reinterpret_cast<std::uintptr_t>(dst) & (width - 1)) != 0) width >>= 1;
        switch (width) {
            case 8: storeSkipEqual<std::uint64_t>(dst, src); break;
            case 4: storeSkipEqual<std::uint32_t>(dst, src); break;
            case 2: storeSkipEqual<std::uint16_t>(dst, src); break;
            default: storeSkipEqual<std::uint8_t>(dst, src); break;
        }
        dst += width;
        src += width;
        size -= width;
    }
}

std::uint32_t stripeIndex(std::uint32_t line) { return (line >> 7) & (kReservationStripeCount - 1); }

// Registers a waiter for the duration of a waitStoreEpoch() call: in a watch slot when it names a line and one is
// free, as a wildcard otherwise. Both stores are seq_cst, see publish().
class WatchRegistration {
public:
    WatchRegistration(std::array<std::atomic<std::uint32_t>, kWatchSlots>& slots, std::atomic<std::uint32_t>& wildcards,
                      std::uint32_t watch_line)
        : slots_(slots), wildcards_(wildcards) {
        if (watch_line != kWatchAnyLine) {
            const std::uint32_t key = stripeIndex(watch_line) + 1;
            for (std::uint32_t i = 0; i < kWatchSlots; i++) {
                std::uint32_t expected = 0;
                if (slots_[i].compare_exchange_strong(expected, key, std::memory_order_seq_cst)) {
                    slot_ = static_cast<int>(i);
                    return;
                }
            }
        }
        wildcards_.fetch_add(1, std::memory_order_seq_cst);
    }
    ~WatchRegistration() {
        if (slot_ >= 0) {
            slots_[slot_].store(0, std::memory_order_seq_cst);
        } else {
            wildcards_.fetch_sub(1, std::memory_order_seq_cst);
        }
    }
    WatchRegistration(const WatchRegistration&) = delete;
    WatchRegistration& operator=(const WatchRegistration&) = delete;

private:
    std::array<std::atomic<std::uint32_t>, kWatchSlots>& slots_;
    std::atomic<std::uint32_t>& wildcards_;
    int slot_ = -1;
};

}  // namespace

ReservationTable::ReservationTable() : stripes_(new std::atomic<std::uint64_t>[kReservationStripeCount]) {
    for (std::uint32_t i = 0; i < kReservationStripeCount; i++) stripes_[i].store(0, std::memory_order_relaxed);
}

std::uint64_t ReservationTable::lock(std::atomic<std::uint64_t>& s) {
    std::uint32_t spins = 0;
    for (;;) {
        std::uint64_t v = s.load(std::memory_order_relaxed);
        if ((v & 1) == 0 && s.compare_exchange_weak(v, v + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
            return v;
        }
        spinOnce(spins);
    }
}

void ReservationTable::unlockVersion(std::atomic<std::uint64_t>& s) { s.fetch_add(1, std::memory_order_seq_cst); }

void ReservationTable::unlockWithWrite(std::atomic<std::uint64_t>& s, std::uint32_t line) {
    unlockVersion(s);
    // The version moves before the epoch, so a waiter that read the old epoch and then found the
    // reservation intact is still woken by this bump.
    publish(line, line);
}

void ReservationTable::unlockClean(std::atomic<std::uint64_t>& s) { s.fetch_sub(1, std::memory_order_release); }

// No wake-up is lost to the filter: a waiter stores its watch slot (seq_cst) before it reads the epoch inside
// waitStoreEpoch(), and a writer bumps the epoch (seq_cst) before it reads the slots. In the seq_cst order
// either the writer's slot read comes after the waiter's store, so it sees the slot and notifies (the mutex
// orders that notify after the waiter's predicate check, which is made with the mutex held), or it comes
// before, so the bump precedes the waiter's epoch read and the waiter does not sleep. The waiter watches the
// stripe of its line and every written line's version moved before this call, so whatever makes lost() true
// reaches the waiter through a stripe it watches.
void ReservationTable::publish(std::uint32_t first_line, std::uint32_t last_line) {
    publishes_.fetch_add(1, std::memory_order_relaxed);
    epoch_.fetch_add(1, std::memory_order_seq_cst);
    if (hasWatcher(first_line, last_line)) notifyWaiters();
}

void ReservationTable::wake() {
    publishes_.fetch_add(1, std::memory_order_relaxed);
    epoch_.fetch_add(1, std::memory_order_seq_cst);
    notifyWaiters();
}

void ReservationTable::notifyWaiters() {
    // Taking the mutex orders this notify after a waiter that has checked the epoch but not yet started to wait.
    {
        std::lock_guard<std::mutex> guard(wait_mutex_);
    }
    notifies_.fetch_add(1, std::memory_order_relaxed);
    wait_cv_.notify_all();
}

bool ReservationTable::hasWatcher(std::uint32_t first_line, std::uint32_t last_line) const {
    if (wildcard_waiters_.load(std::memory_order_seq_cst) != 0) return true;
    // Stripes of the written lines are first, first + 1, ... (mod the stripe count); a range of that many lines
    // or more covers every stripe.
    const std::uint32_t lines = ((last_line - first_line) >> 7) + 1;
    const std::uint32_t first = stripeIndex(first_line);
    for (const auto& slot : watch_) {
        const std::uint32_t key = slot.load(std::memory_order_seq_cst);
        if (key == 0) continue;
        if (lines >= kReservationStripeCount || ((key - 1 - first) & (kReservationStripeCount - 1)) < lines) {
            return true;
        }
    }
    return false;
}

void ReservationTable::getllar(const std::uint8_t* base, std::uint32_t ea, std::uint8_t* dst, Reservation& res) {
    const std::uint32_t line = ea & ~(kReservationLineSize - 1);
    if (lwmutexWatchEnabled()) lwmutexWatchCheck(LwmutexWatchKind::SpuGetllar, line, kReservationLineSize, 0);
    const std::uint8_t* host = base + line;
    const std::atomic<std::uint64_t>& s = stripe(line);
    std::uint32_t spins = 0;
    for (;;) {
        const std::uint64_t v = s.load(std::memory_order_acquire);
        if (v & 1) {
            spinOnce(spins);
            continue;
        }
        for (std::uint32_t i = 0; i < kChunkCount; i++) {
            const std::uint64_t chunk = loadChunk(host + 8 * i);
            std::memcpy(res.snapshot + 8 * i, &chunk, sizeof(chunk));
        }
        // Keeps the chunk loads above the re-read; without it the copy could pass a writer unnoticed.
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.load(std::memory_order_relaxed) == v) {
            res.valid = true;
            res.line = line;
            res.version = v;
            break;
        }
    }
    std::memcpy(dst, res.snapshot, kReservationLineSize);
}

bool ReservationTable::putllc(std::uint8_t* base, std::uint32_t ea, const std::uint8_t* src, Reservation& res) {
    const std::uint32_t line = ea & ~(kReservationLineSize - 1);
    const bool usable = res.valid && res.line == line;
    res.valid = false;
    if (!usable) return false;

    // No spinning: a locked or moved stripe means another writer got there first.
    std::atomic<std::uint64_t>& s = stripe(line);
    std::uint64_t expected = res.version;
    if (!s.compare_exchange_strong(expected, expected + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
        return false;
    }

    std::uint8_t* host = base + line;
    for (std::uint32_t i = 0; i < kChunkCount; i++) {
        if (loadChunk(host + 8 * i) != readBuffer(res.snapshot + 8 * i)) {
            unlockClean(s);
            return false;
        }
    }
    if (compare_hook_ != nullptr) compare_hook_(compare_hook_arg_);
    // Only the chunks written are checked: a line can hold an lwmutex next to an SPU atomic target, and an
    // update of the neighbour never touches the lwmutex words. One hit per PUTLLC, however many chunks it writes.
    bool watch_hit = !lwmutexWatchEnabled();
    for (std::uint32_t i = 0; i < kChunkCount; i++) {
        const std::uint64_t value = readBuffer(src + 8 * i);
        if (value == readBuffer(res.snapshot + 8 * i)) continue;
        if (!watch_hit) watch_hit = lwmutexWatchCheck(LwmutexWatchKind::SpuPutllc, line + 8 * i, 8, 0);
        storeChunk(host + 8 * i, value);
    }
    unlockWithWrite(s, line);
    return true;
}

void ReservationTable::putlluc(std::uint8_t* base, std::uint32_t ea, const std::uint8_t* src) {
    const std::uint32_t line = ea & ~(kReservationLineSize - 1);
    if (lwmutexWatchEnabled()) lwmutexWatchCheck(LwmutexWatchKind::SpuPutlluc, line, kReservationLineSize, 0);
    std::atomic<std::uint64_t>& s = stripe(line);
    lock(s);
    storeBytes(base + line, src, kReservationLineSize);
    unlockWithWrite(s, line);
}

void ReservationTable::put(std::uint8_t* base, std::uint32_t ea, const std::uint8_t* src, std::uint32_t size) {
    if (size == 0) return;
    if (lwmutexWatchEnabled()) lwmutexWatchCheck(LwmutexWatchKind::SpuPut, ea, size, 0);
    // Last line touched, without overflowing at the top of the address space.
    const std::uint32_t first_line = ea & ~(kReservationLineSize - 1);
    const std::uint32_t last_line =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(std::uint64_t{ea} + size - 1, 0xFFFFFFFFull) &
                                       ~std::uint64_t{kReservationLineSize - 1});
    while (size != 0) {
        const std::uint32_t line_end = (ea | (kReservationLineSize - 1)) + 1;  // 0 at the top of the space
        const std::uint32_t piece = line_end == 0 ? size : std::min(size, line_end - ea);
        std::atomic<std::uint64_t>& s = stripe(ea);
        lock(s);
        storeBytes(base + ea, src, piece);
        unlockVersion(s);
        ea += piece;
        src += piece;
        size -= piece;
    }
    // Every line's version has moved by now, so this one epoch bump still follows each of them.
    publish(first_line, last_line);
}

void ReservationTable::loadReserved(const std::uint8_t* base, std::uint32_t ea, std::uint32_t size,
                                    std::uint64_t& value, std::uint64_t& version) {
    const std::atomic<std::uint64_t>& s = stripe(ea);
    std::uint32_t spins = 0;
    for (;;) {
        // The version is read before the value: a write that lands in between must not become the baseline.
        const std::uint64_t v = s.load(std::memory_order_acquire);
        if (v & 1) {
            spinOnce(spins);
            continue;
        }
        std::uint64_t loaded;
        if (size == 4) {
            loaded =
                    std::byteswap(__atomic_load_n(reinterpret_cast<const std::uint32_t*>(base + ea), __ATOMIC_RELAXED));
        } else {
            loaded =
                    std::byteswap(__atomic_load_n(reinterpret_cast<const std::uint64_t*>(base + ea), __ATOMIC_RELAXED));
        }
        // Keeps the word load above the re-read; without it a writer could pass unnoticed.
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.load(std::memory_order_relaxed) == v) {
            value = loaded;
            version = v;
            return;
        }
    }
}

bool ReservationTable::storeConditional(std::uint8_t* base, std::uint32_t ea, std::uint32_t size,
                                        std::uint64_t expected, std::uint64_t expected_version, std::uint64_t value) {
    if (size != 4 && size != 8) return false;
    std::atomic<std::uint64_t>& s = stripe(ea);
    // lock() returns the version the stripe had; a bump since loadReserved() means a reservation-aware
    // write touched the line (or a line sharing the stripe) after the reservation was taken.
    const bool version_ok = lock(s) == expected_version;
    bool ok = false;
    if (version_ok) {
        // The word compare still matters: plain stores to the word do not bump the version.
        if (size == 4) {
            auto* target = reinterpret_cast<std::uint32_t*>(base + ea);
            ok = std::byteswap(__atomic_load_n(target, __ATOMIC_RELAXED)) == static_cast<std::uint32_t>(expected);
            if (ok) __atomic_store_n(target, std::byteswap(static_cast<std::uint32_t>(value)), __ATOMIC_RELEASE);
        } else {
            auto* target = reinterpret_cast<std::uint64_t*>(base + ea);
            ok = std::byteswap(__atomic_load_n(target, __ATOMIC_RELAXED)) == expected;
            if (ok) __atomic_store_n(target, std::byteswap(value), __ATOMIC_RELEASE);
        }
    }
    if (ok) {
        unlockWithWrite(s, ea & ~(kReservationLineSize - 1));
    } else {
        unlockClean(s);
    }
    return ok;
}

bool ReservationTable::lost(const std::uint8_t* base, const Reservation& res) const {
    if (!res.valid) return false;
    // Ignore the lock bit: a writer that holds the stripe briefly and restores it (failed store-conditional,
    // failed PUTLLC compare, stripe collision) leaves the version unchanged. A writer that did write is
    // caught by the version bump once it unlocks, or by the content compare below while it still holds it.
    if ((stripe(res.line).load(std::memory_order_acquire) & ~1ull) != res.version) return true;
    const std::uint8_t* host = base + res.line;
    for (std::uint32_t i = 0; i < kChunkCount; i++) {
        if (loadChunk(host + 8 * i) != readBuffer(res.snapshot + 8 * i)) return true;
    }
    return false;
}

StoreWait ReservationTable::waitStoreEpoch(std::uint64_t seen, const std::uint32_t* halt,
                                           std::chrono::microseconds timeout, std::uint32_t watch_line) {
    // Registered before the epoch is read for the first time, see publish().
    const WatchRegistration registration(watch_, wildcard_waiters_, watch_line);
    if (haltRequested(halt)) return StoreWait::Halt;
    if (epoch_.load(std::memory_order_seq_cst) != seen) return StoreWait::Epoch;

    std::unique_lock<std::mutex> guard(wait_mutex_);
    const bool woken = wait_cv_.wait_for(
            guard, timeout, [&] { return epoch_.load(std::memory_order_seq_cst) != seen || haltRequested(halt); });
    if (!woken) return StoreWait::Timeout;
    return haltRequested(halt) ? StoreWait::Halt : StoreWait::Epoch;
}

ReservationStats ReservationTable::stats() const {
    return {publishes_.load(std::memory_order_relaxed), notifies_.load(std::memory_order_relaxed)};
}

ReservationTable& reservations() {
    static ReservationTable table;
    return table;
}

namespace {

struct LwmutexWatchState {
    std::mutex mutex;
    std::vector<std::uint32_t> live;  // sorted struct addresses
    std::uint64_t max_live = 0;
    std::array<std::atomic<std::uint64_t>, kLwmutexWatchKindCount> hits{};
    // Per kind so frequent harmless getllar hits cannot use up the log lines a write kind needs.
    std::array<std::atomic<std::uint32_t>, kLwmutexWatchKindCount> logged{};
};

LwmutexWatchState& watchState() {
    static LwmutexWatchState state;
    return state;
}

constexpr std::uint32_t kWatchLogLimit = 16;  // per kind

const char* watchKindName(LwmutexWatchKind kind) {
    switch (kind) {
        case LwmutexWatchKind::Lwarx: return "lwarx";
        case LwmutexWatchKind::Stwcx: return "stwcx";
        case LwmutexWatchKind::Dcbz: return "dcbz";
        case LwmutexWatchKind::SpuGetllar: return "getllar";
        case LwmutexWatchKind::SpuPutllc: return "putllc";
        case LwmutexWatchKind::SpuPutlluc: return "putlluc";
        case LwmutexWatchKind::SpuPut: return "put";
        case LwmutexWatchKind::HleCopy: return "hle-copy";
        case LwmutexWatchKind::OwnerValue: return "owner";
        case LwmutexWatchKind::WaiterValue: return "waiter";
    }
    return "?";
}

// The live struct that overlaps [ea, ea + size), if any. Structs do not overlap each other, so the one with the
// largest start at or below the last byte is the only candidate. Caller holds the mutex.
bool findOverlap(const LwmutexWatchState& state, std::uint32_t ea, std::uint32_t size, std::uint32_t& lwmutex) {
    if (size == 0 || state.live.empty()) return false;
    const std::uint64_t last = std::uint64_t{ea} + size - 1;
    auto it = std::upper_bound(state.live.begin(), state.live.end(), last);
    if (it == state.live.begin()) return false;
    --it;
    if (std::uint64_t{*it} + kLwmutexWatchSize <= ea) return false;
    lwmutex = *it;
    return true;
}

void recordWatchHit(LwmutexWatchKind kind, std::uint32_t ea, std::uint32_t size, std::uint64_t lr,
                    std::optional<std::uint32_t> lwmutex) {
    LwmutexWatchState& state = watchState();
    state.hits[static_cast<std::size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
    if (state.logged[static_cast<std::size_t>(kind)].fetch_add(1, std::memory_order_relaxed) >= kWatchLogLimit) {
        return;
    }
    std::string where = "no live struct";
    if (lwmutex) where = std::format("lwmutex {:#010x} offset {}", *lwmutex, static_cast<std::int64_t>(ea) - *lwmutex);
    const std::string lr_text = lr != 0 ? std::format("{:#x}", lr) : "unknown";
    logWarn("tox2: lwmutex watch: {} at {:#010x} size {}, {}, lr {}", watchKindName(kind), ea, size, where, lr_text);
}

}  // namespace

void setLwmutexWatchEnabled(bool enabled) { detail::g_lwmutex_watch.store(enabled, std::memory_order_relaxed); }

void lwmutexWatchAdd(std::uint32_t lwmutex) {
    LwmutexWatchState& state = watchState();
    std::lock_guard guard(state.mutex);
    const auto it = std::lower_bound(state.live.begin(), state.live.end(), lwmutex);
    if (it != state.live.end() && *it == lwmutex) return;
    state.live.insert(it, lwmutex);
    state.max_live = std::max<std::uint64_t>(state.max_live, state.live.size());
}

void lwmutexWatchRemove(std::uint32_t lwmutex) {
    LwmutexWatchState& state = watchState();
    std::lock_guard guard(state.mutex);
    const auto it = std::lower_bound(state.live.begin(), state.live.end(), lwmutex);
    if (it != state.live.end() && *it == lwmutex) state.live.erase(it);
}

void lwmutexWatchHit(LwmutexWatchKind kind, std::uint32_t ea, std::uint32_t size, std::uint64_t lr) {
    std::optional<std::uint32_t> lwmutex;
    {
        LwmutexWatchState& state = watchState();
        std::lock_guard guard(state.mutex);
        std::uint32_t found;
        if (findOverlap(state, ea, size, found)) lwmutex = found;
    }
    recordWatchHit(kind, ea, size, lr, lwmutex);
}

bool lwmutexWatchCheck(LwmutexWatchKind kind, std::uint32_t ea, std::uint32_t size, std::uint64_t lr) {
    std::uint32_t found;
    {
        LwmutexWatchState& state = watchState();
        std::lock_guard guard(state.mutex);
        if (!findOverlap(state, ea, size, found)) return false;
    }
    recordWatchHit(kind, ea, size, lr, found);
    return true;
}

LwmutexWatchStats lwmutexWatchStats() {
    LwmutexWatchState& state = watchState();
    LwmutexWatchStats stats;
    {
        std::lock_guard guard(state.mutex);
        stats.live = state.live.size();
        stats.max_live = state.max_live;
    }
    for (std::size_t i = 0; i < kLwmutexWatchKindCount; i++) {
        stats.hits[i] = state.hits[i].load(std::memory_order_relaxed);
    }
    return stats;
}

}  // namespace tox2
