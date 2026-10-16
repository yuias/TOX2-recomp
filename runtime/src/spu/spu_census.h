#pragma once

// The SPU threads as the supervisor and the census see them. Every SpuThread registers itself here when it
// is constructed and hands its counters over when it is destroyed, so the totals survive the fresh objects
// that lv2 builds at every group start.

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

// How the SPU waits on the reservation table in SpuChannels::readEventStat ended, process-wide. A reservation lost
// through a plain PPU store does not move the store epoch, so it shows up as a Timeout that is followed by a lost
// reservation: that is what `lost_wait` counts (and times, from the start of the wait that timed out). Monotonic;
// not cleared by the census reset, so take deltas.
struct SpuReservationWaits {
    std::uint64_t epoch = 0;               // epoch_before_sleep + epoch_woken
    std::uint64_t epoch_before_sleep = 0;  // Epoch from the global-epoch check before the first sleep
    std::uint64_t epoch_woken = 0;         // Epoch after sleeping: the waiter's wake word changed
    std::uint64_t timeout = 0, halt = 0;
    LatencySnapshot lost_wait;  // count = Timeout returns after which the reservation was found lost
};
// `slept` is what waitStoreEpoch() reported; it only splits the Epoch returns.
void spuCountReservationWait(StoreWait reason, bool slept);
void spuCountLostAfterTimeout(std::chrono::microseconds waited);
SpuReservationWaits spuReservationWaitSnapshot();
// The perf-line segment for the waits between two snapshots; empty when epoch, timeout and lost_wait.count are all
// unchanged.
std::string spuReservationWaitSegment(const SpuReservationWaits& now, const SpuReservationWaits& before);

// The blocking-receive timing of one thread (see SpuThread::receiveBusy), summed over its runs.
struct SpuReceiveRow {
    std::uint32_t id = 0;
    std::string name;
    LatencySnapshot busy, wait;
};
// Threads with at least one receive, live and gone, ordered by thread key; any thread may call it.
std::vector<SpuReceiveRow> spuReceiveSnapshot();

}  // namespace tox2
