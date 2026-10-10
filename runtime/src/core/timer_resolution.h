#pragma once

#include <cstdint>
#include <optional>

namespace dal {

// Windows rounds timed waits up to the system timer tick (~15.6 ms by default), so a guest
// sys_timer_usleep(1000) would take 15 ms. Holding a 1 ms request for the object's lifetime keeps
// guest sleeps and timed lock waits close to what the guest asked for.
//
// The request is per process on current Windows. The process normally ends with std::_Exit, which
// runs no destructors; the OS drops the request then.
class TimerResolution {
public:
    // Requests the finest period the system supports, but no coarser than 1 ms.
    TimerResolution();
    ~TimerResolution();
    TimerResolution(const TimerResolution&) = delete;
    TimerResolution& operator=(const TimerResolution&) = delete;

    // False when the system refused the request; waits then keep the default tick.
    bool active() const { return period_ms_ != 0; }
    std::uint32_t periodMs() const { return period_ms_; }

private:
    std::uint32_t period_ms_ = 0;
};

// The system timer period in effect now, in microseconds, or nullopt when the system does not
// report it. It is the finest request of any process, so it can be below what this process asked for.
std::optional<std::uint32_t> currentTimerPeriodUs();

}  // namespace dal
