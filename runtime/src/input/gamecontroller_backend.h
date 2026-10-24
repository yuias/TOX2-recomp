// GameController.framework as a GamepadBackend (macOS). The header holds no Apple types so that
// game_main.cpp and tests can include it from plain C++; the implementation is Objective-C++.
//
// The framework reports controllers through notifications on the main queue, so it needs the main run
// loop to be served (runWithHostEventLoop in tox2_game, CFRunLoopRunInMode in the test). Readings are
// copied by each controller's own handler queue into the backend's mutex-protected state; no guest
// memory is touched from there.

#pragma once

#include <expected>
#include <memory>
#include <string>

#include "input/gamepad_backend.h"

namespace tox2 {

// Registers for connect and disconnect notifications and takes the controllers already connected.
std::expected<std::shared_ptr<GamepadBackend>, std::string> makeGameControllerBackend();

// The Steam environment variables and the process id; the overlay is never reported as loaded.
SteamContext detectSteamContext();

// Whether this process is the active application. The hub releases the gamepad's buttons while it is
// not, whatever the framework reports for a background process.
bool processOwnsForegroundWindow();

}  // namespace tox2
