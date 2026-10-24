#include "core/host_priority.h"

#include <atomic>

#include "core/log.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace tox2 {
namespace {

std::atomic<bool> g_enabled{true};
// Per role: the first use logs, so a run shows what the audio threads got without one line per SPU thread.
std::atomic<bool> g_logged[2];

const char* nameOf(HostThreadRole role) {
    return role == HostThreadRole::AudioEngine ? "audio engine" : "high-priority SPU";
}

// Sets the calling thread's priority; false with `error` set when the system refuses.
bool setCurrentPriority(int priority, long& error) {
#if defined(_WIN32)
    if (SetThreadPriority(GetCurrentThread(), priority)) return true;
    error = static_cast<long>(GetLastError());
    return false;
#else
    const int result = pthread_set_qos_class_self_np(static_cast<qos_class_t>(priority), 0);
    error = result;
    return result == 0;
#endif
}

}  // namespace

void setHostPrioritiesEnabled(bool enabled) { g_enabled.store(enabled, std::memory_order_relaxed); }

bool hostPrioritiesEnabled() { return g_enabled.load(std::memory_order_relaxed); }

int hostThreadPriorityOf(HostThreadRole role) {
#if defined(_WIN32)
    return role == HostThreadRole::AudioEngine ? THREAD_PRIORITY_TIME_CRITICAL : THREAD_PRIORITY_HIGHEST;
#else
    return role == HostThreadRole::AudioEngine ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED;
#endif
}

int currentHostThreadPriority() {
#if defined(_WIN32)
    return GetThreadPriority(GetCurrentThread());
#else
    return static_cast<int>(qos_class_self());
#endif
}

int applyHostThreadPriority(HostThreadRole role) {
    if (!hostPrioritiesEnabled()) return currentHostThreadPriority();

    const int wanted = hostThreadPriorityOf(role);
    const bool first = !g_logged[static_cast<int>(role)].exchange(true);
    long error = 0;
    if (!setCurrentPriority(wanted, error)) {
        if (first) logWarn("tox2: host: cannot raise the {} thread priority (error {})", nameOf(role), error);
    } else if (first) {
        logDebug("tox2: host: {} thread priority {}", nameOf(role), wanted);
    }
    return currentHostThreadPriority();
}

}  // namespace tox2
