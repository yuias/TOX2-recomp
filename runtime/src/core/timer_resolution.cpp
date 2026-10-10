#include "core/timer_resolution.h"

#include <algorithm>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <timeapi.h>

namespace tox2 {
namespace {

// ntdll exports it without a header; the values are in 100 ns units.
using NtQueryTimerResolutionFn = LONG(NTAPI*)(PULONG minimum, PULONG maximum, PULONG current);

}  // namespace

TimerResolution::TimerResolution() {
    TIMECAPS caps;
    if (timeGetDevCaps(&caps, sizeof caps) != TIMERR_NOERROR) return;
    const UINT period = std::clamp<UINT>(1, caps.wPeriodMin, caps.wPeriodMax);
    if (timeBeginPeriod(period) == TIMERR_NOERROR) period_ms_ = period;
}

TimerResolution::~TimerResolution() {
    if (period_ms_ != 0) timeEndPeriod(period_ms_);
}

std::optional<std::uint32_t> currentTimerPeriodUs() {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) return std::nullopt;
    const auto query = reinterpret_cast<NtQueryTimerResolutionFn>(
            reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryTimerResolution")));
    if (query == nullptr) return std::nullopt;
    ULONG minimum = 0, maximum = 0, current = 0;
    if (query(&minimum, &maximum, &current) < 0) return std::nullopt;
    return current / 10;
}

}  // namespace tox2
