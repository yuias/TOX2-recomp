#pragma once

// The cellSail service thread: a guest thread named `_libsail-control` (the name of libsail's
// control thread in the reference run) that runs the work the cellSail HLE hands it, such as the
// player's notify callback and the graphics adapter's frame allocation callbacks. HLE handlers
// queue work and return, so the game sees events asynchronously as it does on hardware. It is a
// service thread: it never counts toward stall detection.

#include <cstdint>
#include <functional>

#include "dal/ppu_context.h"

namespace dal {

// Runs on the service thread with the guest lock held, with that thread's context and the guest
// memory base; it may call guest functions through callGuestFunction.
using SailCall = std::function<void(DalPpuContext*, std::uint8_t*)>;

// Appends `call` to a FIFO and wakes the thread; calls run one at a time, in order. Starts the
// thread on first use. Needs the guest lock and an initialised guest TLS area (so it works from any
// guest thread inside an HLE handler). Returns false, dropping `call`, when the thread cannot be
// created; nothing queued will ever run then.
bool queueSailCall(SailCall call);

// Ends the thread, drops queued calls and forgets the thread so a later queueSailCall starts a new
// one. Needs the guest lock, which is released while waiting for the thread. For tests.
void stopSailService();

}  // namespace dal
