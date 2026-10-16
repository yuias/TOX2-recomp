#include "core/event_history.h"

#include <array>
#include <atomic>
#include <chrono>
#include <format>

#include "core/log.h"

namespace tox2 {
namespace {

// Every field is an atomic so a reader racing a writer sees a mixed entry, not undefined behaviour. `seq` is
// written last (release); a reader accepts the entry only when it still holds the expected value afterwards.
struct Slot {
    std::atomic<std::uint64_t> seq{0};  // 0 = empty or being written, otherwise entry index + 1
    std::atomic<std::uint64_t> micros{0};
    std::atomic<std::uint64_t> meta{0};  // thread id | kind << 32
    std::atomic<std::uint64_t> a{0};
    std::atomic<std::uint64_t> b{0};
    std::atomic<std::uint64_t> c{0};
    std::atomic<std::uint64_t> lrs{0};  // lr1 | lr2 << 32
};

struct Entry {
    std::uint64_t micros;
    std::uint32_t thread;
    HistoryKind kind;
    std::uint64_t a, b, c;
    std::uint32_t lr1, lr2;
};

std::array<Slot, kHistoryCapacity> g_slots;
std::atomic<std::uint64_t> g_next{0};
const std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();

std::uint64_t elapsedMicros() {
    return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_start).count());
}

bool readEntry(std::uint64_t index, Entry& out) {
    const Slot& slot = g_slots[index % kHistoryCapacity];
    if (slot.seq.load(std::memory_order_acquire) != index + 1) return false;
    const std::uint64_t meta = slot.meta.load(std::memory_order_relaxed);
    out.micros = slot.micros.load(std::memory_order_relaxed);
    out.thread = static_cast<std::uint32_t>(meta);
    out.kind = static_cast<HistoryKind>(meta >> 32);
    out.a = slot.a.load(std::memory_order_relaxed);
    out.b = slot.b.load(std::memory_order_relaxed);
    out.c = slot.c.load(std::memory_order_relaxed);
    const std::uint64_t lrs = slot.lrs.load(std::memory_order_relaxed);
    out.lr1 = static_cast<std::uint32_t>(lrs);
    out.lr2 = static_cast<std::uint32_t>(lrs >> 32);
    std::atomic_thread_fence(std::memory_order_acquire);
    return slot.seq.load(std::memory_order_relaxed) == index + 1;
}

std::string formatEntry(const Entry& e) {
    const std::string head = std::format("history +{}.{:03} ms t {:08x} ", e.micros / 1000, e.micros % 1000, e.thread);
    switch (e.kind) {
        case HistoryKind::SpuSend:
            return head + std::format("spu-send queue {:#x} code {:#x} data3 {:#x}", e.a, e.b, e.c);
        case HistoryKind::EventReceive:
            return head + std::format("eq-recv queue {:#x} code {:#x} data3 {:#x}", e.a, e.b, e.c);
        case HistoryKind::MemoryFree:
            return head + std::format("mem-free addr {:#x} size {:#x} lr {:#x} caller lrs {:#x} {:#x}", e.a, e.b, e.c,
                                      e.lr1, e.lr2);
        case HistoryKind::JoinEnter: return head + std::format("join-enter target {:#x}", e.a);
        case HistoryKind::JoinReturn: return head + std::format("join-return target {:#x} result {:#x}", e.a, e.b);
        case HistoryKind::MutexDestroy: return head + std::format("mutex-destroy id {:#x} result {:#x}", e.a, e.b);
    }
    return head + "unknown";
}

}  // namespace

void recordHistory(HistoryKind kind, std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint32_t lr1,
                   std::uint32_t lr2) {
    const std::uint64_t index = g_next.fetch_add(1, std::memory_order_relaxed);
    Slot& slot = g_slots[index % kHistoryCapacity];
    // Invalidate first so a reader never pairs the old seq with new fields.
    slot.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    slot.micros.store(elapsedMicros(), std::memory_order_relaxed);
    slot.meta.store(
            std::uint64_t{currentGuestThreadId().value_or(0)} | (std::uint64_t{static_cast<std::uint8_t>(kind)} << 32),
            std::memory_order_relaxed);
    slot.a.store(a, std::memory_order_relaxed);
    slot.b.store(b, std::memory_order_relaxed);
    slot.c.store(c, std::memory_order_relaxed);
    slot.lrs.store(std::uint64_t{lr1} | (std::uint64_t{lr2} << 32), std::memory_order_relaxed);
    slot.seq.store(index + 1, std::memory_order_release);
}

std::vector<std::string> historyLines(std::optional<std::uint32_t> fault_addr) {
    const std::uint64_t end = g_next.load(std::memory_order_relaxed);
    const std::uint64_t begin = end > kHistoryCapacity ? end - kHistoryCapacity : 0;
    std::vector<std::string> lines;
    std::optional<Entry> covering;  // newest free that contains the fault address
    for (std::uint64_t index = begin; index < end; index++) {
        Entry entry;
        if (!readEntry(index, entry)) continue;
        lines.push_back(formatEntry(entry));
        if (fault_addr && entry.kind == HistoryKind::MemoryFree && *fault_addr >= entry.a &&
            *fault_addr - entry.a < entry.b) {
            covering = entry;
        }
    }
    if (fault_addr) {
        if (covering) {
            lines.push_back(std::format("history: fault address {:#x} lies in the block freed by: {}", *fault_addr,
                                        formatEntry(*covering)));
        } else {
            lines.push_back(std::format("history: no recorded free covers {:#x}", *fault_addr));
        }
    }
    return lines;
}

void resetHistory() {
    for (Slot& slot : g_slots) slot.seq.store(0, std::memory_order_relaxed);
    g_next.store(0, std::memory_order_relaxed);
}

}  // namespace tox2
