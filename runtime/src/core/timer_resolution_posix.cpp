#include "core/timer_resolution.h"

namespace tox2 {

// POSIX has no coarse system tick to raise: timed waits already have the precision requested, so
// there is nothing to hold and the 1 ms request counts as granted. App Nap and timer coalescing
// are handled by the event loop.
TimerResolution::TimerResolution() { period_ms_ = 1; }

TimerResolution::~TimerResolution() = default;

std::optional<std::uint32_t> currentTimerPeriodUs() { return std::nullopt; }

}  // namespace tox2
