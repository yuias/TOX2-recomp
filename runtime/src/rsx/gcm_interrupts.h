#pragma once

// The GCM interrupt thread: a dedicated guest thread that runs the handlers the game registered
// with cellGcmSetVBlankHandler / SetFlipHandler / SetUserHandler, as RPCS3 does with its
// `_gcm_intr_thread`. The RSX host thread has no guest context, stack or TLS, so it only queues
// work here; the thread runs the handlers one at a time. It takes the kernel lock only for its queue (in legacy mode
// the whole thread runs under the guest lock).

#include <cstdint>

#include "rsx/rsx_thread.h"

namespace tox2 {

struct GcmInterruptCounts {
    std::uint64_t vblank = 0, flip = 0, user = 0;  // handler calls made so far
    std::uint64_t vblank_coalesced = 0;            // vblank ticks dropped because one was still queued
};

// Creates `_gcm_intr_thread` (a service thread) and installs the RSX interrupt hook that feeds it.
// Call with the kernel lock held after the guest TLS area exists, before the RSX thread starts. A
// later call returns the existing thread id. Failure to create the thread is fatal.
std::uint32_t startGcmInterruptThread();

// What the installed hook does; exposed for tests. Queues a call of the handler stored in
// rsxState() for `kind` (a missing handler drops the event). A VBlank is not queued while an earlier
// one is still waiting, so a slow guest sees fewer ticks; flips and user commands are never dropped.
// Needs the kernel lock.
void postGcmInterrupt(RsxInterrupt kind, std::uint32_t arg);

// Readable from any thread.
GcmInterruptCounts gcmInterruptCounts();

// Ends the interrupt thread, removes the hook and forgets the queue and counters. Needs the kernel
// lock, which is released while waiting for the thread. For tests.
void stopGcmInterruptThread();

}  // namespace tox2
