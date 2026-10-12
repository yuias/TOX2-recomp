#pragma once

namespace tox2 {

// Threads that the audio path depends on and that sleep or block while they wait (they never spin; see the
// wait loops in spu_channels.cpp and audio_engine.cpp) are raised above the renderer and the PPU threads.
enum class HostThreadRole {
    AudioEngine,      // THREAD_PRIORITY_TIME_CRITICAL
    HighPrioritySpu,  // THREAD_PRIORITY_HIGHEST
};

// --host-priorities on|off; on by default. Set it before the threads that apply a role are started.
void setHostPrioritiesEnabled(bool enabled);
bool hostPrioritiesEnabled();

// When enabled, raises the calling thread to the role's priority. Logs one debug line per role on first use; a
// failure logs one warning and is ignored. Returns the calling thread's priority afterwards (GetThreadPriority).
int applyHostThreadPriority(HostThreadRole role);

}  // namespace tox2
