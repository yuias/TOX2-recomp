#include "spu/spu_census.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>

#include "core/census.h"
#include "core/reservation.h"
#include "core/threads.h"
#include "spu/spu_channels.h"
#include "spu/spu_thread.h"

namespace tox2 {
namespace {

struct Totals {
    std::uint64_t dispatches = 0;
    std::uint64_t unknown_targets = 0;
    std::array<std::uint64_t, kMaxSpuImages> image_dispatches{};
    std::uint64_t mfc_get = 0, mfc_put = 0, mfc_getllar = 0, mfc_putllc_ok = 0, mfc_putllc_failed = 0;
    std::uint64_t mfc_get_list = 0, mfc_put_list = 0, mfc_putlluc = 0, mfc_fences = 0;
    std::array<std::uint64_t, chan::kCount> reads{};
    std::map<std::uint32_t, std::uint64_t> stops;

    void add(SpuThread& thread) {
        dispatches += thread.dispatchCount();
        unknown_targets += thread.unknownTargetCount();
        for (std::uint32_t i = 0; i < kMaxSpuImages; i++) image_dispatches[i] += thread.imageDispatchCount(i);
        const SpuMfcCounters& c = thread.channels().mfc().counters();
        mfc_get += c.get.load(std::memory_order_relaxed);
        mfc_put += c.put.load(std::memory_order_relaxed);
        mfc_getllar += c.getllar.load(std::memory_order_relaxed);
        mfc_putllc_ok += c.putllc_ok.load(std::memory_order_relaxed);
        mfc_putllc_failed += c.putllc_failed.load(std::memory_order_relaxed);
        mfc_get_list += c.get_list.load(std::memory_order_relaxed);
        mfc_put_list += c.put_list.load(std::memory_order_relaxed);
        mfc_putlluc += c.putlluc.load(std::memory_order_relaxed);
        mfc_fences += c.fences.load(std::memory_order_relaxed);
        for (std::uint32_t ch = 0; ch < chan::kCount; ch++) reads[ch] += thread.channels().readCount(ch);
        for (const auto& [code, count] : thread.stopCounts()) stops[code] += count;
    }
};

// What the census prints about one thread.
struct ThreadRow {
    std::string name;
    std::string state;
    std::string image;
    std::uint32_t ls = 0;
};

struct Live {
    SpuThread* thread;
    std::uint64_t key;
};

// Receive timing of one thread key across its runs.
struct ReceiveTotals {
    std::string name;
    LatencySnapshot busy, wait;

    void add(const SpuThread& thread) {
        name = thread.name();
        busy.merge(thread.receiveBusy());
        wait.merge(thread.receiveWait());
    }
};

struct Registry {
    std::mutex mutex;
    std::vector<Live> live;
    // Rows of threads whose objects are gone, by key; a live thread with the same key replaces its row.
    std::map<std::uint64_t, ThreadRow> gone;
    // Unlike `gone`, a live thread adds to these instead of replacing them, so a restarted group keeps its history.
    std::map<std::uint64_t, ReceiveTotals> receive_gone;
    Totals totals;
    bool any_started = false;
    std::uint32_t next_anonymous = 0;
};

// Leaked on purpose: a fatal stop prints the census while other host threads may still run.
Registry& registry() {
    static Registry* r = new Registry;
    return *r;
}

// A wait that ends by timeout normally lasts the 1 ms poll; twice that flags a delayed wake-up.
constexpr std::uint64_t kReservationWaitOverUs = 2000;

struct ReservationWaitCounters {
    std::atomic<std::uint64_t> epoch_before_sleep{0}, epoch_woken{0}, timeout{0}, halt{0};
    LatencyStats lost_wait{kReservationWaitOverUs};
};

// Leaked like the registry: SPU threads may still wait while the process shuts down.
ReservationWaitCounters& reservationWaits() {
    static ReservationWaitCounters* c = new ReservationWaitCounters;
    return *c;
}

// Threads that lv2 numbered keep one row across restarts; threads outside lv2 (id 0) each have their own.
std::uint64_t keyFor(const SpuThread& thread, std::uint32_t anonymous) {
    return thread.id() != 0 ? thread.id() : (std::uint64_t{1} << 32) + anonymous;
}

std::optional<ThreadRow> describeRow(const SpuThread& thread) {
    ThreadRow row;
    row.name = thread.name();
    switch (thread.state()) {
        case SpuState::Created: return std::nullopt;
        case SpuState::Running: row.state = "running"; break;
        case SpuState::Blocked: {
            const char* what = thread.blockedIn();
            row.state = std::string("blocked in ") + (what != nullptr ? what : "?");
            break;
        }
        case SpuState::Finished: row.state = "finished"; break;
    }
    const std::optional<std::uint32_t> image = thread.lastDispatchImage();
    row.image = image && *image < spuImageCount() ? spuImage(*image).name : "untracked";
    row.ls = thread.lastDispatchTarget();
    return row;
}

void resetSpuCensus() {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    r.gone.clear();
    r.receive_gone.clear();
    r.totals = Totals{};
    r.any_started = false;
}

void collectSpuHealth(std::vector<ThreadHealth>& out) {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    for (const Live& l : r.live) {
        switch (l.thread->state()) {
            case SpuState::Created:
            case SpuState::Finished: break;
            case SpuState::Running: out.push_back({ThreadState::Running, false, 0, false}); break;
            case SpuState::Blocked:
                out.push_back({ThreadState::Blocked, l.thread->indefiniteWait(), l.thread->blockedSinceMs(), false});
                break;
        }
    }
}

}  // namespace

void spuCensusAdd(SpuThread& thread) {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    r.live.push_back({&thread, keyFor(thread, r.next_anonymous++)});
}

void spuCensusRemove(SpuThread& thread) {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    const auto it = std::ranges::find(r.live, &thread, &Live::thread);
    if (it == r.live.end()) return;
    r.totals.add(thread);
    if (const std::optional<ThreadRow> row = describeRow(thread)) r.gone[it->key] = *row;
    if (thread.receiveWait().count != 0) r.receive_gone[it->key].add(thread);
    r.live.erase(it);
}

void spuCountReservationWait(StoreWait reason, bool slept) {
    ReservationWaitCounters& c = reservationWaits();
    switch (reason) {
        // One counter per return, as before the split: the wait path adds no atomic operation.
        case StoreWait::Epoch:
            (slept ? c.epoch_woken : c.epoch_before_sleep).fetch_add(1, std::memory_order_relaxed);
            break;
        case StoreWait::Timeout: c.timeout.fetch_add(1, std::memory_order_relaxed); break;
        case StoreWait::Halt: c.halt.fetch_add(1, std::memory_order_relaxed); break;
    }
}

void spuCountLostAfterTimeout(std::chrono::microseconds waited) {
    reservationWaits().lost_wait.record(static_cast<std::uint64_t>(std::max<std::int64_t>(waited.count(), 0)));
}

SpuReservationWaits spuReservationWaitSnapshot() {
    const ReservationWaitCounters& c = reservationWaits();
    SpuReservationWaits out;
    out.epoch_before_sleep = c.epoch_before_sleep.load(std::memory_order_relaxed);
    out.epoch_woken = c.epoch_woken.load(std::memory_order_relaxed);
    out.epoch = out.epoch_before_sleep + out.epoch_woken;
    out.timeout = c.timeout.load(std::memory_order_relaxed);
    out.halt = c.halt.load(std::memory_order_relaxed);
    out.lost_wait = c.lost_wait.snapshot();
    return out;
}

std::string spuReservationWaitSegment(const SpuReservationWaits& now, const SpuReservationWaits& before) {
    const std::uint64_t lost = now.lost_wait.count - before.lost_wait.count;
    if (now.epoch == before.epoch && now.timeout == before.timeout && lost == 0) return {};
    return std::format("rsv-wait epoch {} timeout {} lost-after-timeout {} pre-sleep {} woken {}",
                       now.epoch - before.epoch, now.timeout - before.timeout, lost,
                       now.epoch_before_sleep - before.epoch_before_sleep, now.epoch_woken - before.epoch_woken);
}

std::vector<SpuReceiveRow> spuReceiveSnapshot() {
    Registry& r = registry();
    std::map<std::uint64_t, ReceiveTotals> by_key;
    {
        std::lock_guard lock(r.mutex);
        by_key = r.receive_gone;
        for (const Live& l : r.live) {
            if (l.thread->receiveWait().count != 0) by_key[l.key].add(*l.thread);
        }
    }
    std::vector<SpuReceiveRow> rows;
    for (auto& [key, totals] : by_key) {
        rows.push_back({static_cast<std::uint32_t>(key), std::move(totals.name), totals.busy, totals.wait});
    }
    return rows;
}

void spuCensusStarted() {
    Registry& r = registry();
    {
        std::lock_guard lock(r.mutex);
        r.any_started = true;
    }
    setSpuCensusProvider(&spuCensusLines, &resetSpuCensus);
    setSpuHealthProvider(&collectSpuHealth);
}

std::vector<std::string> spuCensusLines() {
    Registry& r = registry();
    Totals sum;
    std::map<std::uint64_t, ThreadRow> rows;
    {
        std::lock_guard lock(r.mutex);
        if (!r.any_started) return {};
        sum = r.totals;
        rows = r.gone;
        for (const Live& l : r.live) {
            sum.add(*l.thread);
            if (const std::optional<ThreadRow> row = describeRow(*l.thread)) rows[l.key] = *row;
        }
    }

    std::vector<std::string> out;
    out.push_back(std::format("census: spu {} threads, {} dispatches, {} unknown targets", rows.size(), sum.dispatches,
                              sum.unknown_targets));
    for (const auto& [key, row] : rows) {
        const auto id = static_cast<std::uint32_t>(key);
        out.push_back(
                std::format("  thread {:#010x} {}: {} {} ls {:#07x}", id, row.name, row.state, row.image, row.ls));
    }
    for (std::uint32_t i = 0; i < spuImageCount(); i++) {
        if (sum.image_dispatches[i] == 0) continue;
        out.push_back(std::format("  image {}: {} dispatches, {} distinct entries", spuImage(i).name,
                                  sum.image_dispatches[i], spuDistinctEntries(i)));
    }
    // List elements are counted in get/put already; the list commands themselves get the extra line below.
    out.push_back(std::format("  mfc get: {}, put: {}, getllar: {}, putllc: {} ok {} failed", sum.mfc_get, sum.mfc_put,
                              sum.mfc_getllar, sum.mfc_putllc_ok, sum.mfc_putllc_failed));
    if (sum.mfc_get_list + sum.mfc_put_list + sum.mfc_putlluc + sum.mfc_fences != 0) {
        out.push_back(std::format("  mfc get lists: {}, put lists: {}, putlluc: {}, fences: {}", sum.mfc_get_list,
                                  sum.mfc_put_list, sum.mfc_putlluc, sum.mfc_fences));
    }
    for (std::uint32_t ch = 0; ch < chan::kCount; ch++) {
        if (sum.reads[ch] != 0) {
            out.push_back(std::format("  channel rdch {} {}: {}", ch, spuChannelName(ch), sum.reads[ch]));
        }
    }
    for (const auto& [code, count] : sum.stops) {
        out.push_back(std::format("  stop {:#x}: {}", code, count));
    }
    const ReservationStats reservation = reservations().stats();
    out.push_back(
            std::format("  reservation table: {} publishes, {} notifies", reservation.publishes, reservation.notifies));
    const SpuReservationWaits waits = spuReservationWaitSnapshot();
    out.push_back(std::format("  reservation waits: epoch {}, timeout {} (lost after timeout {}, wait {}), halt {}",
                              waits.epoch, waits.timeout, waits.lost_wait.count,
                              formatLatency(waits.lost_wait, kReservationWaitOverUs), waits.halt));
    const double seconds = std::max(static_cast<double>(systemTimeUs()) / 1e6, 1e-3);
    for (const SpuReceiveRow& row : spuReceiveSnapshot()) {
        out.push_back(std::format("  thread {:#010x} {} receive {:.1f}/s: busy {}; wait {}", row.id, row.name,
                                  static_cast<double>(row.wait.count) / seconds,
                                  formatLatency(row.busy, kSpuReceiveOverUs),
                                  formatLatency(row.wait, kSpuReceiveOverUs)));
    }
    return out;
}

}  // namespace tox2
