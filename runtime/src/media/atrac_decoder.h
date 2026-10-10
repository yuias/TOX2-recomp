#pragma once

// ATRAC3plus frame decoder interface. No codec types here: the FFmpeg implementation lives in
// dal_media and registers itself through setAtracDecoderFactory().

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "media/at3_file.h"

namespace dal {

class AtracFrameDecoder {
public:
    virtual ~AtracFrameDecoder() = default;
    // One frame of block_align bytes -> kAtrac3plusFrameSamples x channels interleaved host floats.
    // False with the reason on a bad frame; the caller then uses silence for that frame.
    virtual bool decode(std::span<const std::uint8_t> frame, std::span<float> out, std::string& error) = 0;
    virtual void reset() = 0;  // after a seek (loop); the next frame starts a new overlap
};

class AtracDecoderFactory {
public:
    virtual ~AtracDecoderFactory() = default;
    // Null with the reason in `error` when the file's parameters cannot be decoded.
    virtual std::unique_ptr<AtracFrameDecoder> create(const At3Info& info, std::string& error) = 0;
};

// The factory is installed once at start-up, before any guest code runs. Null means silence.
void setAtracDecoderFactory(std::unique_ptr<AtracDecoderFactory> factory);
AtracDecoderFactory* atracDecoderFactory();

}  // namespace dal
