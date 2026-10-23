// WASAPI audio output (shared mode, default render device) and the device-facing seam it is built on.
// The header holds no Windows types, so tools and tests can include it freely.

#pragma once

#include <chrono>
#include <memory>

#include "audio/audio_output.h"
#include "audio/stream_output.h"

namespace tox2 {

// Opens the default render device (shared mode, float32 48 kHz stereo, 40 ms buffer). Must run on a
// thread whose COM apartment is initialised (the output's device thread is). An error that starts
// with "no render device:" means the machine has no usable render endpoint. The stream's status codes
// are HRESULT values.
RenderStreamFactory makeWasapiStreamFactory();

struct WasapiOutputOptions {
    std::chrono::milliseconds open_timeout{2000};     // how long open() waits for the device thread
    std::chrono::milliseconds reopen_interval{2000};  // between reopen attempts after a device loss
};

std::unique_ptr<AudioOutput> makeWasapiOutput();
// Same output over any stream factory; for tests.
std::unique_ptr<AudioOutput> makeWasapiOutputForTest(RenderStreamFactory factory, WasapiOutputOptions options = {});

}  // namespace tox2
