#pragma once

// The SPU threads as the supervisor and the census see them. Every SpuThread registers itself here when it
// is constructed and hands its counters over when it is destroyed, so the totals survive the fresh objects
// that lv2 builds at every group start.

#include <string>
#include <vector>

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

}  // namespace tox2
