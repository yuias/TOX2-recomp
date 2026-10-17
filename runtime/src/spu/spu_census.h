#pragma once

// The SPU threads as the supervisor and the census see them. Every SpuThread registers itself here when it
// is constructed and hands its counters over when it is destroyed, so the totals survive the fresh objects
// that lv2 builds at every group start.

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "core/latency_stats.h"
#include "core/reservation.h"

namespace tox2 {

class SpuThread;

// Registration of a thread object; called by the SpuThread constructor and destructor.
void spuCensusAdd(SpuThread& thread);
// Folds the counters and the last state into the totals and forgets the object.
void spuCensusRemove(SpuThread& thread);
// A thread is about to start: the census section appears from now on, and the census and supervisor
// providers are installed again (resetCensus() forgets the census one).
void spuCensusStarted();

// The lines of the "census: spu" section; empty when no SPU thread started since the last reset.
std::vector<std::string> spuCensusLines();

// Upper bounds in microseconds of the buckets of SpuReservationWaits::woken_us; the last bucket is everything above.
inline constexpr std::array<std::uint64_t, 8> kWokenBucketBoundsUs = {2, 5, 10, 20, 50, 100, 500, 1000};
inline constexpr std::size_t kWokenBuckets = kWokenBucketBoundsUs.size() + 1;

// How the SPU waits on the reservation table in SpuChannels::readEventStat ended, process-wide. A reservation lost
// through a plain PPU store does not move the store epoch, so it shows up as a Timeout that is followed by a lost
// reservation: that is what `lost_wait` counts (and times, from the start of the wait that timed out). `lost_table`
// and `lost_plain` split that count: a write through the table that landed after the timeout moved the stripe version,
// anything else changed the line's content without it (a plain PPU store or a host write). Monotonic; not cleared by
// the census reset, so take deltas.
struct SpuReservationWaits {
    std::uint64_t epoch = 0;               // epoch_before_sleep + epoch_woken
    std::uint64_t epoch_before_sleep = 0;  // Epoch from the global-epoch check before the first sleep
    std::uint64_t epoch_woken = 0;         // Epoch after sleeping: the waiter's wake word changed
    std::uint64_t timeout = 0, halt = 0;
    LatencySnapshot lost_wait;                     // count = Timeout returns after which the reservation was found lost
    std::uint64_t lost_table = 0, lost_plain = 0;  // lost_table + lost_plain == lost_wait.count
    // Time from the start of the wait to the return, for the Epoch returns after a sleep (epoch_woken).
    std::array<std::uint64_t, kWokenBuckets> woken_us{};
    // Table notifies by writer kind (index = WriterKind), taken from the reservation table.
    std::array<std::uint64_t, kWriterKindCount> notifies{};
};
// `slept` is what waitStoreEpoch() reported; it only splits the Epoch returns.
void spuCountReservationWait(StoreWait reason, bool slept);
// Classifies the loss as table (`loss.version_moved`) or plain, and logs the first 16 events of each kind (line
// address, thread id, the LS address of the waiting rdch and the chunks that differ from `res`'s snapshot).
void spuCountLostAfterTimeout(std::chrono::microseconds waited, std::uint32_t line, std::uint32_t thread_id,
                              std::uint32_t site, const Reservation& res, const ReservationLoss& loss);
// `slept` is how long an Epoch-after-sleep wait took.
void spuCountWokenSleep(std::chrono::microseconds slept);
SpuReservationWaits spuReservationWaitSnapshot();
// The perf-line segment for the waits between two snapshots; empty when epoch, timeout and lost_wait.count are all
// unchanged.
// It ends with the woken-us histogram, the notify counts and the table/plain split of the lost-after-timeout count.
std::string spuReservationWaitSegment(const SpuReservationWaits& now, const SpuReservationWaits& before);

// Failed PUTLLCs by reason, process-wide and monotonic (from the reservation table).
using SpuPutllcFails = std::array<std::uint64_t, kPutllcFailCount>;
SpuPutllcFails spuPutllcFailSnapshot();
// "putllc-fail lost x locked y moved z data w" for the failures between two snapshots; empty when there are none.
std::string spuPutllcFailSegment(const SpuPutllcFails& now, const SpuPutllcFails& before);

// The blocking-receive timing of one thread (see SpuThread::receiveBusy), summed over its runs.
struct SpuReceiveRow {
    std::uint32_t id = 0;
    std::string name;
    LatencySnapshot busy, wait;
};
// Threads with at least one receive, live and gone, ordered by thread key; any thread may call it.
std::vector<SpuReceiveRow> spuReceiveSnapshot();

}  // namespace tox2
