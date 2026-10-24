#pragma once

#include <chrono>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace tox2 {

#if defined(_WIN32)

// A high-resolution timer sleeps in about a millisecond steps; Sleep() would round to the 15.6 ms
// system tick and make periodic host clocks (VBlank, audio blocks) irregular. One instance per
// thread: the timer handle is not shared.
class Sleeper {
public:
    Sleeper() {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    ~Sleeper() {
        if (timer_ != nullptr) CloseHandle(timer_);
    }
    Sleeper(const Sleeper&) = delete;
    Sleeper& operator=(const Sleeper&) = delete;

    void sleep(std::chrono::microseconds duration) {
        if (duration.count() <= 0) return;
        if (timer_ == nullptr) return std::this_thread::sleep_for(duration);
        LARGE_INTEGER due;
        due.QuadPart = -10 * duration.count();  // relative, in 100 ns units
        if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer_, INFINITE);
        } else {
            std::this_thread::sleep_for(duration);
        }
    }

private:
    HANDLE timer_ = nullptr;
};
#else
// POSIX timed sleeps already have sub-millisecond precision, so there is nothing to set up.
class Sleeper {
public:
    Sleeper() = default;
    Sleeper(const Sleeper&) = delete;
    Sleeper& operator=(const Sleeper&) = delete;

    void sleep(std::chrono::microseconds duration) {
        if (duration.count() <= 0) return;
        std::this_thread::sleep_for(duration);
    }
};
#endif

}  // namespace tox2
