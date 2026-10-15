#pragma once

// The {owner, waiter} word pair of a guest `sys_lwmutex_t`. Every access to the pair goes through LwmutexWord, so the
// protocol in sys_mutex_cond.cpp sees one 8-byte unit. RPCS3 treats the pair the same way (`lv2/sys_lwmutex.h`).

#include <atomic>
#include <bit>
#include <cstdint>

#include "core/fatal.h"
#include "core/threads.h"
#include "tox2/ppu_context.h"

namespace tox2 {

static_assert(std::endian::native == std::endian::little, "the pair packing assumes a little-endian host");

struct LwmutexPair {
    std::uint32_t owner = 0;
    std::uint32_t waiter = 0;
    friend bool operator==(const LwmutexPair&, const LwmutexPair&) = default;
};

// The pair as the host u64 whose memory bytes are the big-endian owner followed by the big-endian waiter. The only
// place that knows the byte order.
inline std::uint64_t packLwmutexPair(LwmutexPair pair) {
    return static_cast<std::uint64_t>(__builtin_bswap32(pair.owner)) |
           (static_cast<std::uint64_t>(__builtin_bswap32(pair.waiter)) << 32);
}

inline LwmutexPair unpackLwmutexPair(std::uint64_t raw) {
    return {__builtin_bswap32(static_cast<std::uint32_t>(raw)),
            __builtin_bswap32(static_cast<std::uint32_t>(raw >> 32))};
}

// The {owner, waiter} pair of the sys_lwmutex_t at guest address `lwmutex`. When the address is 8-byte aligned every
// operation is an atomic_ref<std::uint64_t> operation (seq_cst RMWs, acquire loads), safe without the kernel lock. When
// it is not, the operations are plain tox2Read32/tox2Write32 accesses and the caller must hold the kernel lock (a fatal
// internal error in the split modes otherwise; legacy mode holds it anyway).
class LwmutexWord {
public:
    LwmutexWord(std::uint8_t* base, std::uint32_t lwmutex) : base_(base), lwmutex_(lwmutex) {}

    bool aligned() const { return (lwmutex_ & 7) == 0; }

    LwmutexPair load() const {
        if (!aligned()) {
            requireKernelLock();
            return {tox2Read32(base_, lwmutex_), tox2Read32(base_, lwmutex_ + 4)};
        }
        return unpackLwmutexPair(ref().load(std::memory_order_acquire));
    }

    // compare_exchange_strong; on failure `expected` receives the current pair.
    bool compareExchange(LwmutexPair& expected, LwmutexPair desired) {
        if (!aligned()) {
            requireKernelLock();
            const LwmutexPair current{tox2Read32(base_, lwmutex_), tox2Read32(base_, lwmutex_ + 4)};
            if (!(current == expected)) {
                expected = current;
                return false;
            }
            tox2Write32(base_, lwmutex_, desired.owner);
            tox2Write32(base_, lwmutex_ + 4, desired.waiter);
            return true;
        }
        std::uint64_t raw = packLwmutexPair(expected);
        if (ref().compare_exchange_strong(raw, packLwmutexPair(desired), std::memory_order_seq_cst)) return true;
        expected = unpackLwmutexPair(raw);
        return false;
    }

    // CAS loop: desired = fn(current); writes only when desired != current. Returns the pair it replaced (or saw).
    template <class Fn>
    LwmutexPair update(Fn fn) {
        LwmutexPair current = load();
        for (;;) {
            const LwmutexPair desired = fn(current);
            if (desired == current) return current;
            LwmutexPair expected = current;
            if (compareExchange(expected, desired)) return current;
            current = expected;
        }
    }

private:
    std::atomic_ref<std::uint64_t> ref() const {
        return std::atomic_ref<std::uint64_t>(*reinterpret_cast<std::uint64_t*>(base_ + lwmutex_));
    }

    // A misaligned pair cannot be accessed atomically, so the kernel lock stands in for it.
    static void requireKernelLock() {
        if (splitPpuMode() && !guestLock().heldByCurrentThread()) {
            fatal(FatalKind::Internal, "lwmutex: a misaligned lwmutex was accessed without the kernel lock");
        }
    }

    std::uint8_t* base_;
    std::uint32_t lwmutex_;
};

}  // namespace tox2
