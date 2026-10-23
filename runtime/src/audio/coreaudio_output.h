// CoreAudio audio output (default output device, followed through the default-output AudioUnit). The
// header holds no Apple types, so tools and tests can include it freely.

#pragma once

#include <memory>

#include "audio/audio_output.h"
#include "audio/stream_output.h"

namespace tox2 {

// Opens the default output device through an AudioUnit (float32 48 kHz stereo; CoreAudio converts it
// to the device's format). The stream owns a 40 ms FIFO that the unit's render callback drains; the
// callback touches nothing else. An error that starts with "no render device:" means the machine has
// no default output device. The stream's status codes are OSStatus values, a positive error code
// negated.
RenderStreamFactory makeCoreAudioStreamFactory();

std::unique_ptr<AudioOutput> makeCoreAudioOutput();

}  // namespace tox2
