#include "core/host_priority.h"

#include <atomic>

#include "core/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace tox2 {
namespace {

std::atomic<bool> g_enabled{true};
// Per role: the first use logs, so a run shows what the audio threads got without one line per SPU thread.
std::atomic<bool> g_logged[2];

int priorityOf(HostThreadRole role) {
    return role == HostThreadRole::AudioEngine ? THREAD_PRIORITY_TIME_CRITICAL : THREAD_PRIORITY_HIGHEST;
}

const char* nameOf(HostThreadRole role) {
    return role == HostThreadRole::AudioEngine ? "audio engine" : "high-priority SPU";
}

}  // namespace

void setHostPrioritiesEnabled(bool enabled) { g_enabled.store(enabled, std::memory_order_relaxed); }

bool hostPrioritiesEnabled() { return g_enabled.load(std::memory_order_relaxed); }

int applyHostThreadPriority(HostThreadRole role) {
    const HANDLE thread = GetCurrentThread();
    if (!hostPrioritiesEnabled()) return GetThreadPriority(thread);

    const int wanted = priorityOf(role);
    const bool first = !g_logged[static_cast<int>(role)].exchange(true);
    if (!SetThreadPriority(thread, wanted)) {
        if (first) logWarn("tox2: host: cannot raise the {} thread priority (error {})", nameOf(role), GetLastError());
    } else if (first) {
        logDebug("tox2: host: {} thread priority {}", nameOf(role), wanted);
    }
    return GetThreadPriority(thread);
}

}  // namespace tox2
