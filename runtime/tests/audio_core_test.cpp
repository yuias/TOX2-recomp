// Self-checking tests for the audio core: the output interface, the null output, the block ring, the
// WAV writer, reader and level statistics, and the engine with its block clock. No sound device is
// involved.
//
// With a directory argument the WAV files the tests write are kept there (for tox2_audiotool
// wavstat); without one they go to a temporary directory that is removed at the end.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "audio/audio_engine.h"
#include "audio/audio_output.h"
#include "audio/wav_file.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "core/threads.h"
#include "tox2/ppu_context.h"
#include "hle/cell_atrac_stream.h"
#include "hle/cell_audio_ports.h"
#include "hle/hle.h"
#include "lv2/sys_sync.h"
#include "media/at3_file.h"
#include "media/atrac_decoder.h"

namespace {

namespace fs = std::filesystem;

int g_passed;
int g_failed;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            g_passed++;                                                            \
        } else {                                                                   \
            g_failed++;                                                            \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
        }                                                                          \
    } while (0)

constexpr double kInf = std::numeric_limits<double>::infinity();

bool isNear(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

fs::path g_dir;

// Frame n of a ramp: left n, right -n. Exact in float up to 2^24 frames.
void fillRamp(std::span<float> block, std::uint64_t first_frame) {
    for (std::size_t i = 0; i < block.size() / 2; ++i) {
        const float value = static_cast<float>(first_frame + i);
        block[2 * i] = value;
        block[2 * i + 1] = -value;
    }
}

bool isRamp(std::span<const float> frames, std::uint64_t first_frame) {
    for (std::size_t i = 0; i < frames.size() / 2; ++i) {
        const float value = static_cast<float>(first_frame + i);
        if (frames[2 * i] != value || frames[2 * i + 1] != -value) return false;
    }
    return true;
}

// 1 kHz sine at 48 kHz (a whole number of periods per second). The phase offset keeps every sample
// away from zero, so all frames count as non-silent.
float toneSample(std::uint64_t frame, float amplitude) {
    return amplitude *
           static_cast<float>(std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(frame) / 48000.0 + 0.3));
}

bool pushRamp(tox2::AudioBlockRing& ring, std::uint64_t first_frame) {
    tox2::AudioBlock block;
    fillRamp(block, first_frame);
    return ring.push(block);
}

void testRingPushPop() {
    tox2::AudioBlockRing ring(4);
    CHECK(ring.queuedFrames() == 0);

    std::vector<float> out(2 * 1024);
    CHECK(ring.pop(out, 100) == 0);

    for (std::uint64_t b = 0; b < 4; ++b) CHECK(pushRamp(ring, b * tox2::kAudioBlockFrames));
    CHECK(ring.queuedFrames() == 4 * tox2::kAudioBlockFrames);
    // Full: the block is refused and nothing changes.
    CHECK(!pushRamp(ring, 4 * tox2::kAudioBlockFrames));
    CHECK(ring.queuedFrames() == 4 * tox2::kAudioBlockFrames);

    // Partial reads cross block boundaries and continue where the last one stopped.
    std::uint64_t next = 0;
    const std::size_t chunks[] = {100, 1, 155, 256, 300, 212};
    for (const std::size_t chunk : chunks) {
        CHECK(ring.pop(out, chunk) == chunk);
        CHECK(isRamp(std::span<const float>(out).first(chunk * 2), next));
        next += chunk;
    }
    CHECK(next == 1024);
    CHECK(ring.queuedFrames() == 0);
    CHECK(ring.pop(out, 10) == 0);

    // A wrong-sized block is refused.
    std::vector<float> small(tox2::kAudioBlockSamples - 2);
    CHECK(!ring.push(small));
    CHECK(ring.queuedFrames() == 0);

    // The destination bounds the read as well as the frame count.
    CHECK(pushRamp(ring, 1024));
    CHECK(ring.pop(std::span<float>(out).first(20), 256) == 10);
    CHECK(ring.queuedFrames() == tox2::kAudioBlockFrames - 10);
    CHECK(ring.pop(out, 0) == 0);

    // A capacity of zero still holds one block.
    tox2::AudioBlockRing tiny(0);
    CHECK(pushRamp(tiny, 0));
    CHECK(!pushRamp(tiny, 256));
}

void testRingWrap() {
    tox2::AudioBlockRing ring(3);
    std::vector<float> out(2 * 1024);
    std::uint64_t pushed = 0;
    std::uint64_t popped = 0;
    bool ok = true;

    // Keep the ring between one and three blocks full while reads and writes drift against each other
    // across many wraps.
    for (int round = 0; round < 200; ++round) {
        while (pushRamp(ring, pushed)) pushed += tox2::kAudioBlockFrames;
        const std::size_t want = 37 + static_cast<std::size_t>(round * 11 % 400);
        const std::size_t got = ring.pop(out, want);
        ok = ok && got == std::min<std::size_t>(want, pushed - popped);
        ok = ok && isRamp(std::span<const float>(out).first(got * 2), popped);
        popped += got;
        ok = ok && ring.queuedFrames() == pushed - popped;
    }
    CHECK(ok);
    CHECK(pushed > 20 * 3 * tox2::kAudioBlockFrames);
}

void testRingDrop() {
    tox2::AudioBlockRing ring(4);
    std::vector<float> out(2 * 1024);

    // Dropping from an empty ring does nothing.
    ring.dropOldestBlock();
    CHECK(ring.queuedFrames() == 0);
    CHECK(ring.pop(out, 100) == 0);
    CHECK(pushRamp(ring, 0));
    CHECK(ring.queuedFrames() == tox2::kAudioBlockFrames);

    // The drop discards the oldest block; the count reflects it before the consumer runs.
    CHECK(pushRamp(ring, 256));
    CHECK(pushRamp(ring, 512));
    ring.dropOldestBlock();
    CHECK(ring.queuedFrames() == 2 * tox2::kAudioBlockFrames);
    CHECK(ring.pop(out, 256) == 256);
    CHECK(isRamp(std::span<const float>(out).first(512), 256));
    CHECK(ring.queuedFrames() == tox2::kAudioBlockFrames);

    // A drop with less than a block queued discards what is there; with a block half read it removes
    // the rest of the data it covers.
    CHECK(ring.pop(out, 100) == 100);
    CHECK(isRamp(std::span<const float>(out).first(200), 512));
    ring.dropOldestBlock();
    CHECK(ring.queuedFrames() == 0);
    CHECK(ring.pop(out, 100) == 0);

    // A drop requested on a full ring frees space once the consumer has run.
    std::uint64_t next = 768;
    for (int i = 0; i < 4; ++i, next += tox2::kAudioBlockFrames) CHECK(pushRamp(ring, next));
    CHECK(!pushRamp(ring, next));
    ring.dropOldestBlock();
    ring.dropOldestBlock();
    CHECK(ring.queuedFrames() == 2 * tox2::kAudioBlockFrames);
    CHECK(ring.pop(out, 1) == 1);
    CHECK(isRamp(std::span<const float>(out).first(2), 768 + 512));
    CHECK(pushRamp(ring, next));
    CHECK(ring.queuedFrames() == 2 * tox2::kAudioBlockFrames - 1 + tox2::kAudioBlockFrames);
}

void testRingThreaded() {
    constexpr std::uint64_t kBlocks = 3000;
    constexpr std::uint64_t kFrames = kBlocks * tox2::kAudioBlockFrames;
    tox2::AudioBlockRing ring(8);

    std::thread producer([&ring] {
        tox2::AudioBlock block;
        for (std::uint64_t b = 0; b < kBlocks; ++b) {
            fillRamp(block, b * tox2::kAudioBlockFrames);
            while (!ring.push(block)) std::this_thread::yield();
        }
    });

    bool ok = true;
    std::uint64_t consumed = 0;
    std::vector<float> out(2 * 300);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (consumed < kFrames && std::chrono::steady_clock::now() < deadline) {
        const std::size_t want = 1 + static_cast<std::size_t>(consumed * 7 % 300);
        const std::size_t got = ring.pop(out, want);
        ok = ok && isRamp(std::span<const float>(out).first(got * 2), consumed);
        consumed += got;
        if (got == 0) std::this_thread::yield();
    }
    producer.join();
    CHECK(ok);
    CHECK(consumed == kFrames);
    CHECK(ring.queuedFrames() == 0);
}

void testNullOutput() {
    const auto output = tox2::makeNullAudioOutput();
    CHECK(output->name() == "none");
    CHECK(!output->description().empty());

    std::string error;
    CHECK(output->open(error));
    CHECK(error.empty());

    tox2::AudioOutputStats stats = output->stats();
    CHECK(stats.blocks_submitted == 0 && stats.frames_played == 0);

    tox2::AudioBlock block{};
    for (int i = 0; i < 5; ++i) output->submit(block);
    stats = output->stats();
    CHECK(stats.blocks_submitted == 5);
    CHECK(stats.frames_played == 5 * tox2::kAudioBlockFrames);
    CHECK(stats.blocks_dropped == 0 && stats.underrun_frames == 0 && stats.device_errors == 0);

    // The block period is the block length at the sample rate, to the nanosecond.
    CHECK(tox2::kAudioBlockPeriod.count() == 5'333'333);
    CHECK(tox2::kAudioBlockSamples == 512);
}

std::vector<std::uint8_t> makePcm16Wav(std::uint16_t channels, std::uint32_t rate,
                                       std::span<const std::int16_t> samples, bool long_fmt, bool extra_chunk,
                                       std::uint32_t data_size_override = 0) {
    std::vector<std::uint8_t> bytes;
    auto put = [&bytes](const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), p, p + size);
    };
    auto put32 = [&](std::uint32_t v) { put(&v, 4); };
    auto put16 = [&](std::uint16_t v) { put(&v, 2); };

    put("RIFF", 4);
    put32(0);  // patched below; the reader ignores it
    put("WAVE", 4);
    if (extra_chunk) {
        // Odd size: the chunk is followed by a pad byte the reader must skip.
        put("LIST", 4);
        put32(5);
        put("abcde", 5);
        put("\0", 1);
    }
    put("fmt ", 4);
    put32(long_fmt ? 18 : 16);
    put16(1);
    put16(channels);
    put32(rate);
    put32(rate * channels * 2);
    put16(static_cast<std::uint16_t>(channels * 2));
    put16(16);
    if (long_fmt) put16(0);
    put("data", 4);
    put32(data_size_override != 0 ? data_size_override
                                  : static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t)));
    put(samples.data(), samples.size() * sizeof(std::int16_t));
    const std::uint32_t riff = static_cast<std::uint32_t>(bytes.size() - 8);
    std::memcpy(bytes.data() + 4, &riff, 4);
    return bytes;
}

std::vector<std::uint8_t> readFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void testWavFloatRoundTrip() {
    const fs::path path = g_dir / "roundtrip.wav";
    auto created = tox2::WavWriter::create(path, 48000, 2);
    CHECK(created.has_value());
    if (!created) return;
    auto& writer = *created;

    // 30000 frames in uneven pieces, including a piece that is not a multiple of the block size.
    std::vector<float> all(2 * 30000);
    for (std::size_t i = 0; i < all.size(); ++i) {
        all[i] = std::sin(static_cast<float>(i) * 0.01f) * 0.8f;
    }
    const std::size_t pieces[] = {256, 1, 4999, 12345, 30000 - 256 - 1 - 4999 - 12345};
    std::size_t offset = 0;
    bool wrote = true;
    for (const std::size_t frames : pieces) {
        wrote = writer->write(std::span<const float>(all).subspan(offset * 2, frames * 2)) && wrote;
        offset += frames;
    }
    CHECK(wrote);
    CHECK(writer->framesWritten() == 30000);
    CHECK(writer->finish());

    const auto wav = tox2::readWav(path);
    CHECK(wav.has_value());
    if (wav) {
        CHECK(wav->sample_rate == 48000 && wav->channels == 2);
        CHECK(wav->frames() == 30000);
        CHECK(wav->samples == all);
    }
    CHECK(readFile(path).size() == 58 + 30000 * 8);

    // finish() is idempotent; writes after it are refused and leave the file alone.
    CHECK(writer->finish());
    CHECK(!writer->write(std::span<const float>(all).first(512)));
    CHECK(writer->framesWritten() == 30000);
    CHECK(readFile(path).size() == 58 + 30000 * 8);

    // The writer's header follows the layout other tools expect for float WAV.
    const auto bytes = readFile(path);
    CHECK(std::memcmp(bytes.data(), "RIFF", 4) == 0 && std::memcmp(bytes.data() + 8, "WAVE", 4) == 0);
    std::uint16_t format = 0;
    std::uint32_t riff_size = 0;
    std::uint32_t data_size = 0;
    std::memcpy(&riff_size, bytes.data() + 4, 4);
    std::memcpy(&format, bytes.data() + 20, 2);
    std::memcpy(&data_size, bytes.data() + 54, 4);
    CHECK(format == 3);
    CHECK(riff_size == bytes.size() - 8);
    CHECK(data_size == 30000 * 8);
}

void testWavHeaderValidWithoutFinish() {
    const fs::path path = g_dir / "unfinished.wav";
    auto created = tox2::WavWriter::create(path, 48000, 2);
    CHECK(created.has_value());
    if (!created) return;
    auto& writer = *created;

    // A file that was never written to is a valid empty WAV.
    auto empty = tox2::readWav(path);
    CHECK(empty.has_value() && empty->frames() == 0 && empty->channels == 2 && empty->sample_rate == 48000);

    std::uint64_t frame = 0;
    auto writeFrames = [&](std::uint64_t count) {
        std::vector<float> block(2 * count);
        fillRamp(block, frame);
        frame += count;
        return writer->write(block);
    };

    // Before the first patch the header still describes an empty file.
    CHECK(writeFrames(40000));
    auto early = tox2::readWav(path);
    CHECK(early.has_value() && early->frames() == 0);

    // After the patch at 48000 frames the file holds exactly that many, whatever was written since
    // sits only in the stream's buffer (what a std::_Exit leaves behind).
    bool ok = true;
    for (int i = 0; i < 100 && ok; ++i) ok = writeFrames(256);
    CHECK(ok);
    CHECK(writer->framesWritten() == 40000 + 100 * 256);
    auto cut = tox2::readWav(path);
    CHECK(cut.has_value());
    if (cut) {
        CHECK(cut->frames() == 48000);
        CHECK(isRamp(cut->samples, 0));
    }

    CHECK(writeFrames(48000));
    cut = tox2::readWav(path);
    CHECK(cut.has_value() && cut->frames() == 96000 && isRamp(cut->samples, 0));

    CHECK(writer->finish());
    const auto done = tox2::readWav(path);
    CHECK(done.has_value() && done->frames() == 40000 + 100 * 256 + 48000 && isRamp(done->samples, 0));
}

void testWavWriterErrors() {
    CHECK(!tox2::WavWriter::create(g_dir / "no_such_dir" / "x.wav", 48000, 2).has_value());
    CHECK(!tox2::WavWriter::create(g_dir / "bad_format.wav", 0, 2).has_value());
    CHECK(!tox2::WavWriter::create(g_dir / "bad_format.wav", 48000, 0).has_value());

    // Samples that are not whole frames are refused without being written.
    auto created = tox2::WavWriter::create(g_dir / "odd.wav", 48000, 2);
    CHECK(created.has_value());
    if (!created) return;
    const float three[3] = {0.1f, 0.2f, 0.3f};
    CHECK(!(*created)->write(three));
    CHECK((*created)->framesWritten() == 0);
    CHECK((*created)->write(std::span<const float>(three).first(2)));
    CHECK((*created)->framesWritten() == 1);

    // The destructor finishes the file.
    created->reset();
    const auto wav = tox2::readWav(g_dir / "odd.wav");
    CHECK(wav.has_value() && wav->frames() == 1 && wav->samples[0] == 0.1f && wav->samples[1] == 0.2f);

    // A mono writer counts frames, not samples.
    auto mono = tox2::WavWriter::create(g_dir / "mono.wav", 44100, 1);
    CHECK(mono.has_value());
    if (!mono) return;
    CHECK((*mono)->write(three));
    CHECK((*mono)->framesWritten() == 3);
    CHECK((*mono)->finish());
    const auto mono_wav = tox2::readWav(g_dir / "mono.wav");
    CHECK(mono_wav.has_value() && mono_wav->channels == 1 && mono_wav->sample_rate == 44100 && mono_wav->frames() == 3);
}

void testWavReadPcm16() {
    const std::int16_t stereo[] = {0, 16384, -16384, -32768, 32767, 1};

    for (const bool long_fmt : {false, true}) {
        for (const bool extra : {false, true}) {
            const auto bytes = makePcm16Wav(2, 44100, stereo, long_fmt, extra);
            const auto wav = tox2::parseWav(bytes);
            CHECK(wav.has_value());
            if (!wav) continue;
            CHECK(wav->sample_rate == 44100 && wav->channels == 2 && wav->frames() == 3);
            CHECK(wav->samples.size() == 6);
            CHECK(wav->samples[0] == 0.0f && wav->samples[1] == 0.5f && wav->samples[2] == -0.5f);
            CHECK(wav->samples[3] == -1.0f && wav->samples[5] == 1.0f / 32768.0f);
            CHECK(isNear(wav->samples[4], 32767.0 / 32768.0, 1e-9));
        }
    }

    const auto mono = tox2::parseWav(makePcm16Wav(1, 22050, stereo, false, false));
    CHECK(mono.has_value() && mono->channels == 1 && mono->frames() == 6 && mono->sample_rate == 22050);

    // A data size past the end of the buffer (a file cut off mid-write) reads what is present; a
    // trailing partial frame is dropped.
    const auto cut = tox2::parseWav(makePcm16Wav(2, 48000, stereo, false, false, 1000));
    CHECK(cut.has_value() && cut->frames() == 3);
    const std::int16_t odd[] = {1, 2, 3, 4, 5};
    const auto partial = tox2::parseWav(makePcm16Wav(2, 48000, odd, false, false));
    CHECK(partial.has_value() && partial->frames() == 2);
}

void testWavReadRejects() {
    const std::int16_t samples[] = {1, 2, 3, 4};
    CHECK(!tox2::parseWav({}).has_value());
    CHECK(!tox2::readWav(g_dir / "does_not_exist.wav").has_value());

    auto bytes = makePcm16Wav(2, 48000, samples, false, false);
    auto damaged = bytes;
    damaged[0] = 'X';
    CHECK(!tox2::parseWav(damaged).has_value());
    damaged = bytes;
    damaged[8] = 'X';
    CHECK(!tox2::parseWav(damaged).has_value());

    // fmt cut off, and no data chunk.
    CHECK(!tox2::parseWav(std::span<const std::uint8_t>(bytes).first(20)).has_value());
    CHECK(!tox2::parseWav(std::span<const std::uint8_t>(bytes).first(36)).has_value());

    // Unsupported sample formats and channel counts: 8-bit PCM, 24-bit PCM, float64, 3 channels.
    // The fmt payload starts at offset 20; bits per sample sits 14 bytes into it.
    auto with = [&](std::size_t offset, std::uint16_t value) {
        auto copy = bytes;
        std::memcpy(copy.data() + offset, &value, 2);
        return copy;
    };
    CHECK(!tox2::parseWav(with(20 + 14, 8)).has_value());
    CHECK(!tox2::parseWav(with(20 + 14, 24)).has_value());
    CHECK(!tox2::parseWav(with(20, 3)).has_value());  // float tag with 16-bit samples
    CHECK(!tox2::parseWav(with(20, 0xFFFE)).has_value());
    CHECK(!tox2::parseWav(with(20 + 2, 3)).has_value());
    CHECK(!tox2::parseWav(with(20 + 2, 0)).has_value());
}

void testWavStatsTone() {
    // One second of a 0.5-amplitude sine: RMS is 0.5 / sqrt(2), the peak is just under 0.5.
    std::vector<float> tone(2 * 48000);
    for (std::uint64_t i = 0; i < 48000; ++i) {
        tone[2 * i] = tone[2 * i + 1] = toneSample(i, 0.5f);
    }
    auto stats = tox2::wavStats(tone, 48000, 2, 1.0);
    CHECK(stats.size() == 1);
    if (stats.size() == 1) {
        CHECK(stats[0].start_frame == 0 && stats[0].end_frame == 48000);
        CHECK(isNear(stats[0].rms_db, 20 * std::log10(0.5 / std::numbers::sqrt2), 0.01));
        CHECK(isNear(stats[0].peak_db, 20 * std::log10(0.5), 0.05));
        CHECK(stats[0].non_silent_frames == 48000);
    }

    // The same signal on the left channel only: RMS is taken over both channels, the peak is unchanged.
    std::vector<float> left_only = tone;
    for (std::size_t i = 1; i < left_only.size(); i += 2) left_only[i] = 0;
    stats = tox2::wavStats(left_only, 48000, 2, 1.0);
    CHECK(stats.size() == 1);
    if (stats.size() == 1) {
        CHECK(isNear(stats[0].rms_db, 20 * std::log10(0.25), 0.01));
        CHECK(isNear(stats[0].peak_db, 20 * std::log10(0.5), 0.05));
        CHECK(stats[0].non_silent_frames == 48000);
    }

    // Windows of 0.25 s: four equal windows.
    stats = tox2::wavStats(tone, 48000, 2, 0.25);
    CHECK(stats.size() == 4);
    for (std::size_t i = 0; i < stats.size(); ++i) {
        CHECK(stats[i].start_frame == i * 12000 && stats[i].end_frame == (i + 1) * 12000);
        CHECK(isNear(stats[i].rms_db, 20 * std::log10(0.5 / std::numbers::sqrt2), 0.01));
    }

    // A mono signal is one channel per frame.
    std::vector<float> mono(48000);
    for (std::uint64_t i = 0; i < mono.size(); ++i) mono[i] = toneSample(i, 1.0f);
    stats = tox2::wavStats(mono, 48000, 1, 1.0);
    CHECK(stats.size() == 1 && stats[0].non_silent_frames == 48000 &&
          isNear(stats[0].rms_db, 20 * std::log10(1.0 / std::numbers::sqrt2), 0.01));
}

void testWavStatsSilence() {
    std::vector<float> silence(2 * 48000 + 2 * 1000);
    auto stats = tox2::wavStats(silence, 48000, 2, 1.0);
    CHECK(stats.size() == 2);
    if (stats.size() == 2) {
        // The last window covers the remainder.
        CHECK(stats[1].start_frame == 48000 && stats[1].end_frame == 49000);
        for (const auto& w : stats) {
            CHECK(w.rms_db == -kInf && w.peak_db == -kInf && w.non_silent_frames == 0);
        }
    }

    // The threshold is 1e-4 on any channel: at it is silent, above it is not.
    std::vector<float> faint(2 * 100);
    for (std::size_t i = 0; i < 10; ++i) faint[2 * i] = 1e-4f;       // at the threshold
    for (std::size_t i = 10; i < 30; ++i) faint[2 * i + 1] = 2e-4f;  // right channel only
    for (std::size_t i = 30; i < 35; ++i) faint[2 * i] = -3e-4f;     // negative counts too
    stats = tox2::wavStats(faint, 48000, 2, 1.0);
    CHECK(stats.size() == 1 && stats[0].non_silent_frames == 25);
    CHECK(stats.size() == 1 && isNear(stats[0].peak_db, 20 * std::log10(3e-4), 0.01));

    // Degenerate input produces no windows.
    CHECK(tox2::wavStats({}, 48000, 2, 1.0).empty());
    CHECK(tox2::wavStats(silence, 0, 2, 1.0).empty());
    CHECK(tox2::wavStats(silence, 48000, 0, 1.0).empty());
    CHECK(tox2::wavStats(silence, 48000, 2, 0.0).empty());
    CHECK(tox2::wavStats(silence, 48000, 2, -1.0).empty());
    CHECK(tox2::wavStats(silence, 48000, 2, 1e-9).empty());
    CHECK(tox2::wavStats(silence, 48000, 2, 1e30).size() == 1);
}

// The file tox2_audiotool wavstat is checked against by hand (and by anyone reading the docs):
//   0-1 s    stereo tone, amplitude 0.5
//   1-2 s    stereo tone, amplitude 0.05
//   2-3 s    silence
//   3-3.5 s  tone, amplitude 0.5, left channel only
void testWavStatsFile() {
    const fs::path path = g_dir / "wavstat_sample.wav";
    auto created = tox2::WavWriter::create(path, 48000, 2);
    CHECK(created.has_value());
    if (!created) return;

    std::vector<float> all(2 * 168000);
    for (std::uint64_t i = 0; i < 168000; ++i) {
        float left = 0;
        float right = 0;
        if (i < 48000) {
            left = right = toneSample(i, 0.5f);
        } else if (i < 96000) {
            left = right = toneSample(i, 0.05f);
        } else if (i >= 144000) {
            left = toneSample(i, 0.5f);
        }
        all[2 * i] = left;
        all[2 * i + 1] = right;
    }
    // Written in cellAudio-sized blocks, like the engine will.
    bool wrote = true;
    for (std::size_t frame = 0; frame < 168000; frame += tox2::kAudioBlockFrames) {
        const std::size_t frames = std::min<std::size_t>(tox2::kAudioBlockFrames, 168000 - frame);
        wrote = (*created)->write(std::span<const float>(all).subspan(frame * 2, frames * 2)) && wrote;
    }
    CHECK(wrote);
    CHECK((*created)->finish());

    const auto wav = tox2::readWav(path);
    CHECK(wav.has_value() && wav->frames() == 168000 && wav->samples == all);
    if (!wav) return;
    const auto stats = tox2::wavStats(wav->samples, wav->sample_rate, wav->channels, 1.0);
    CHECK(stats.size() == 4);
    if (stats.size() != 4) return;
    CHECK(isNear(stats[0].rms_db, -9.03, 0.01) && isNear(stats[0].peak_db, -6.02, 0.05) &&
          stats[0].non_silent_frames == 48000);
    CHECK(isNear(stats[1].rms_db, -29.03, 0.01) && isNear(stats[1].peak_db, -26.02, 0.05) &&
          stats[1].non_silent_frames == 48000);
    CHECK(stats[2].rms_db == -kInf && stats[2].peak_db == -kInf && stats[2].non_silent_frames == 0);
    CHECK(stats[3].end_frame - stats[3].start_frame == 24000);
    CHECK(isNear(stats[3].rms_db, -12.04, 0.01) && isNear(stats[3].peak_db, -6.02, 0.05) &&
          stats[3].non_silent_frames == 24000);
}

void testBlockClock() {
    using Clock = tox2::BlockClock::Clock;
    using namespace std::chrono_literals;
    const Clock::time_point start{};
    const auto period = tox2::kAudioBlockPeriod;
    CHECK(tox2::kMaxCatchUpBlocks == 0);

    tox2::BlockClock clock(start);
    CHECK(clock.nextDue() == start + period);

    // Before the first period is due nothing happens and the schedule stays.
    auto plan = clock.due(start + period - 1ns);
    CHECK(plan.produce == 0 && plan.skipped == 0 && clock.nextDue() == start + period);

    // On time, then a little late: one block each, no skips.
    plan = clock.due(start + period);
    CHECK(plan.produce == 1 && plan.skipped == 0 && clock.nextDue() == start + 2 * period);
    plan = clock.due(start + 2 * period + 3ms);
    CHECK(plan.produce == 1 && plan.skipped == 0 && clock.nextDue() == start + 3 * period);

    // Woken with three periods due (3p, 4p, 5p): one block, two skipped, and the schedule moves to the
    // first period after now.
    Clock::time_point now = start + 5 * period + 1ms;
    plan = clock.due(now);
    CHECK(plan.produce == 1 && plan.skipped == 2);
    CHECK(clock.nextDue() == start + 6 * period && clock.nextDue() > now);

    // Ten due.
    now = clock.nextDue() + 9 * period + 100us;
    const Clock::time_point before = clock.nextDue();
    plan = clock.due(now);
    CHECK(plan.produce == 1 && plan.skipped == 9);
    CHECK(clock.nextDue() == before + 10 * period && clock.nextDue() > now && clock.nextDue() - now <= period);

    // Just short of the next period: nothing due.
    plan = clock.due(clock.nextDue() - 1ns);
    CHECK(plan.produce == 0 && plan.skipped == 0);

    // Exactly on time for a long run: one block per period, nothing skipped.
    tox2::BlockClock steady(start);
    std::uint64_t produced = 0;
    std::uint64_t skipped = 0;
    for (int i = 1; i <= 1000; ++i) {
        plan = steady.due(start + i * period);
        produced += plan.produce;
        skipped += plan.skipped;
    }
    CHECK(produced == 1000 && skipped == 0 && steady.nextDue() == start + 1001 * period);

    // A pause of an hour is one block and a large count of skipped periods.
    tox2::BlockClock paused(start);
    now = start + 1h;
    plan = paused.due(now);
    const std::uint64_t due = 1 + (now - (start + period)) / period;
    CHECK(plan.produce == 1 && plan.skipped == due - 1);
    CHECK(paused.nextDue() > now && paused.nextDue() - now <= period);
}

void testBlockClockCorrection() {
    using Clock = tox2::BlockClock::Clock;
    using Ns = std::chrono::nanoseconds;
    const Clock::time_point start{};
    const auto nominal = tox2::kAudioBlockPeriod;

    tox2::BlockClock clock(start);
    CHECK(clock.period() == nominal);
    clock.setCorrectionPpm(0.0);
    CHECK(clock.period() == nominal);

    // Faster means a shorter period; 300 ppm of 5.333 ms is 1.6 us.
    clock.setCorrectionPpm(300.0);
    CHECK(nominal - clock.period() >= Ns(1590) && nominal - clock.period() <= Ns(1610));
    clock.setCorrectionPpm(-300.0);
    CHECK(clock.period() - nominal >= Ns(1590) && clock.period() - nominal <= Ns(1610));

    // Out-of-range requests are held at the bound: +-2.7 us.
    clock.setCorrectionPpm(1e6);
    CHECK(nominal - clock.period() > Ns(2600) && nominal - clock.period() <= Ns(2700));
    clock.setCorrectionPpm(-1e6);
    CHECK(clock.period() - nominal > Ns(2600) && clock.period() - nominal <= Ns(2700));

    // The schedule is absolute: a change moves only the steps after it.
    tox2::BlockClock stepped(start);
    stepped.due(start + nominal);
    const Clock::time_point due_before = stepped.nextDue();
    stepped.setCorrectionPpm(-500.0);
    CHECK(stepped.nextDue() == due_before);
    const tox2::BlockClockPlan plan = stepped.due(due_before);
    CHECK(plan.produce == 1 && plan.skipped == 0 && stepped.nextDue() == due_before + stepped.period());
}

// The engine thread's loop against a modelled device, on made-up time: the BlockClock ticks (a little
// late, now and then three periods late), every tick adds the periods that elapsed to the frames
// submitted, and the device has consumed `(1 + device_ppm) * 48000` frames per second. The reported
// fill goes to the controller once per tick, as the engine does.
struct DriftRun {
    double max_error = 0.0;  // largest |true fill - target| over the run
    double min_fill = 1e18;
    double max_fill = 0.0;
    double max_abs_ppm = 0.0;
    double final_ppm = 0.0;
    double final_error = 0.0;
    std::uint64_t updates = 0;
};

DriftRun simulateDrift(double device_ppm, std::chrono::seconds length, double initial_error, bool device_steps) {
    using Clock = tox2::BlockClock::Clock;
    using Ns = std::chrono::nanoseconds;
    constexpr double kDevicePeriodFrames = 480.0;  // the device takes its data in 10 ms steps
    const Clock::time_point start{};
    const double target = static_cast<double>(tox2::kFillTargetFrames);
    const double initial_fill = target + initial_error;

    tox2::BlockClock clock(start);
    tox2::BlockRateController rate;
    DriftRun run;
    std::uint64_t submitted = 0;
    std::uint64_t wakes = 0;
    std::uint32_t lcg = 12345;
    const Clock::time_point end = start + length;
    while (clock.nextDue() < end) {
        lcg = lcg * 1664525u + 1013904223u;
        ++wakes;
        Clock::time_point now = clock.nextDue() + Ns((lcg >> 16 & 0x7fff) * 45);  // up to 1.5 ms late
        if (wakes % 997 == 0) now += 2 * clock.period();
        const tox2::BlockClockPlan plan = clock.due(now);
        submitted += std::uint64_t{plan.produce + plan.skipped} * tox2::kAudioBlockFrames;

        const double consumed =
                std::chrono::duration<double>(now - start).count() * tox2::kAudioSampleRate * (1.0 + device_ppm * 1e-6);
        const double fill = initial_fill + static_cast<double>(submitted) - consumed;
        // A real device reports its padding at the top of the step; the output subtracts half a period.
        const double reported = device_steps
                                        ? initial_fill + static_cast<double>(submitted) -
                                                  std::floor(consumed / kDevicePeriodFrames) * kDevicePeriodFrames -
                                                  kDevicePeriodFrames / 2
                                        : fill;
        run.max_error = std::max(run.max_error, std::fabs(fill - target));
        run.min_fill = std::min(run.min_fill, fill);
        run.max_fill = std::max(run.max_fill, fill);
        if (rate.observe(now, static_cast<std::uint64_t>(std::max(0.0, std::round(reported))))) {
            clock.setCorrectionPpm(rate.ppm());
            run.max_abs_ppm = std::max(run.max_abs_ppm, std::fabs(rate.ppm()));
            ++run.updates;
        }
        run.final_error = fill - target;
    }
    run.final_ppm = rate.ppm();
    return run;
}

void testRateControllerFollowsDeviceDrift() {
    using namespace std::chrono_literals;
    using Clock = tox2::BlockClock::Clock;
    constexpr double kBlock = tox2::kAudioBlockFrames;
    for (const double device_ppm : {300.0, -300.0, 50.0, 0.0}) {
        const DriftRun run = simulateDrift(device_ppm, 30min, 0.0, false);
        // Within one block of the target for the whole 30 minutes, nothing near empty or near the ring's
        // high-water mark, and the correction is the drift (device faster: the engine speeds up).
        CHECK(run.max_error <= kBlock);
        CHECK(run.min_fill > 0.0 && run.max_fill < 12 * kBlock);
        CHECK(run.max_abs_ppm <= tox2::kClockMaxPpm);
        CHECK(run.updates >= 1700);
        CHECK(std::fabs(run.final_ppm - device_ppm) < 15.0);
        CHECK(std::fabs(run.final_error) <= kBlock);
    }

    // The same with a device that reports its consumption in 10 ms steps, as the output corrects it
    // (half a period off the top of the step): the mean of the reading follows the true fill.
    for (const double device_ppm : {300.0, -300.0}) {
        const DriftRun run = simulateDrift(device_ppm, 30min, 0.0, true);
        CHECK(run.max_error <= kBlock);
        CHECK(std::fabs(run.final_error) <= kBlock);
        CHECK(run.min_fill > 0.0 && run.max_fill < 12 * kBlock);
        CHECK(run.max_abs_ppm <= tox2::kClockMaxPpm);
        CHECK(std::fabs(run.final_ppm - device_ppm) < 30.0);
    }

    // A fill that starts off the target is walked back at the bounded rate, without running dry.
    for (const double initial_error : {-512.0, 512.0}) {
        const DriftRun run = simulateDrift(0.0, 10min, initial_error, false);
        CHECK(run.min_fill > 0.0);
        CHECK(run.max_abs_ppm <= tox2::kClockMaxPpm);
        CHECK(std::fabs(run.final_error) < 32.0);
        CHECK(std::fabs(run.final_ppm) < 15.0);
    }

    // Readings of "no fill" leave the correction alone and restart the interval.
    tox2::BlockRateController rate;
    const Clock::time_point t0{};
    CHECK(!rate.observe(t0, tox2::kFillTargetFrames + 1000));
    CHECK(rate.observe(t0 + 1s, tox2::kFillTargetFrames + 1000));
    const double before = rate.ppm();
    CHECK(before < 0.0);
    CHECK(!rate.observe(t0 + 1500ms, std::nullopt));
    CHECK(!rate.observe(t0 + 3s, tox2::kFillTargetFrames + 1000));  // a new interval starts here
    CHECK(rate.ppm() == before);

    // No fill level, no correction.
    tox2::BlockRateController idle;
    for (int i = 0; i < 1000; ++i) CHECK(!idle.observe(t0 + i * 10ms, std::nullopt));
    CHECK(idle.ppm() == 0.0);
}

// Everything a RecordingOutput receives, readable by the test after the engine has stopped.
struct Recording {
    std::mutex mutex;
    std::vector<float> samples;  // every submitted block back to back
    int opens = 0;
    std::chrono::milliseconds open_delay{0};  // how long open() takes, to model a slow device
    std::atomic<std::int64_t> fill{-1};       // what fillFrames() reports; negative: none

    std::vector<float> take() {
        std::lock_guard<std::mutex> lock(mutex);
        return samples;
    }
};

class RecordingOutput final : public tox2::AudioOutput {
public:
    RecordingOutput(std::shared_ptr<Recording> recording, bool open_ok)
        : recording_(std::move(recording)), open_ok_(open_ok) {}

    std::string_view name() const override { return "fake"; }
    bool open(std::string& error) override {
        if (recording_->open_delay.count() > 0) std::this_thread::sleep_for(recording_->open_delay);
        std::lock_guard<std::mutex> lock(recording_->mutex);
        ++recording_->opens;
        if (!open_ok_) error = "device gone";
        return open_ok_;
    }
    void submit(std::span<const float> block) override {
        std::lock_guard<std::mutex> lock(recording_->mutex);
        recording_->samples.insert(recording_->samples.end(), block.begin(), block.end());
        submitted_.fetch_add(1);
    }
    std::string description() const override { return "recording"; }
    std::optional<std::uint64_t> fillFrames() const override {
        const std::int64_t fill = recording_->fill.load();
        if (fill < 0) return std::nullopt;
        return static_cast<std::uint64_t>(fill);
    }
    tox2::AudioOutputStats stats() const override {
        tox2::AudioOutputStats stats;
        stats.blocks_submitted = submitted_.load();
        return stats;
    }

private:
    std::shared_ptr<Recording> recording_;
    bool open_ok_;
    std::atomic<std::uint64_t> submitted_{0};
};

tox2::AudioEngineConfig engineConfig(const std::shared_ptr<Recording>& recording, bool open_ok, fs::path dump) {
    tox2::AudioEngineConfig config;
    config.make_output = [recording, open_ok] { return std::make_unique<RecordingOutput>(recording, open_ok); };
    config.dump = std::move(dump);
    return config;
}

// A producer that numbers its calls: call c fills block frames c * 256 + 1 ... as a ramp, so every
// produced block is non-silent and says which call made it. Optionally every fourth call leaves the
// block silent, and one call can stall to make the engine wake late.
struct ProducerState {
    std::atomic<std::uint64_t> calls{0};
    bool silent_every_fourth = false;
    std::uint64_t stall_call = ~std::uint64_t{0};
    std::chrono::milliseconds stall{0};
};

tox2::AudioBlockProducer makeProducer(const std::shared_ptr<ProducerState>& state) {
    return [state](std::span<float> block) {
        const std::uint64_t call = state->calls.fetch_add(1);
        if (call == state->stall_call) std::this_thread::sleep_for(state->stall);
        if (state->silent_every_fourth && call % 4 == 3) return;
        fillRamp(block, call * tox2::kAudioBlockFrames + 1);
    };
}

template <typename Predicate>
bool waitFor(Predicate done, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

struct RecordedBlocks {
    std::size_t total = 0;
    std::size_t silent = 0;
    std::vector<std::uint64_t> tags;  // producer call of each non-silent block, in order
    bool ramps_intact = true;
};

RecordedBlocks analyse(std::span<const float> samples) {
    RecordedBlocks result;
    result.total = samples.size() / tox2::kAudioBlockSamples;
    for (std::size_t b = 0; b < result.total; ++b) {
        const auto block = samples.subspan(b * tox2::kAudioBlockSamples, tox2::kAudioBlockSamples);
        if (std::all_of(block.begin(), block.end(), [](float s) { return s == 0.0f; })) {
            ++result.silent;
            continue;
        }
        const auto tag = static_cast<std::uint64_t>((static_cast<double>(block[0]) - 1.0) / tox2::kAudioBlockFrames);
        result.tags.push_back(tag);
        result.ramps_intact = result.ramps_intact && isRamp(block, tag * tox2::kAudioBlockFrames + 1);
    }
    return result;
}

double periodsToMs(std::uint64_t periods) {
    return static_cast<double>(periods) * static_cast<double>(tox2::kAudioBlockPeriod.count()) / 1e6;
}

void testEngineBlocksAndDump() {
    using Clock = std::chrono::steady_clock;
    const fs::path dump_path = g_dir / "engine_dump.wav";
    const auto recording = std::make_shared<Recording>();
    const auto state = std::make_shared<ProducerState>();
    state->silent_every_fourth = true;
    const auto other = std::make_shared<ProducerState>();

    tox2::configureAudioEngine(engineConfig(recording, true, dump_path));
    const Clock::time_point t0 = Clock::now();
    tox2::startAudioEngine(makeProducer(state));
    tox2::startAudioEngine(makeProducer(other));  // already running: ignored
    CHECK(waitFor([&] { return state->calls.load() >= 40; }));
    const Clock::time_point t_stop = Clock::now();

    const tox2::AudioEngineStats running = tox2::audioEngineStats();
    CHECK(running.output == "fake");

    tox2::AudioEngineStats stats;
    CHECK(tox2::stopAudioEngineForTest(&stats));
    const Clock::time_point t1 = Clock::now();
    CHECK(other->calls.load() == 0);

    const std::uint64_t calls = state->calls.load();
    CHECK(stats.output == "fake");
    CHECK(stats.blocks == calls);
    const std::vector<float> samples = recording->take();
    const RecordedBlocks recorded = analyse(samples);
    CHECK(recorded.total == stats.blocks + stats.skipped_blocks);
    CHECK(stats.output_stats.blocks_submitted == recorded.total);
    CHECK(recording->opens == 1);

    // Every fourth produced block is silent; the rest arrive in call order, unchanged.
    std::vector<std::uint64_t> expected_tags;
    for (std::uint64_t c = 0; c < calls; ++c) {
        if (c % 4 != 3) expected_tags.push_back(c);
    }
    CHECK(recorded.tags == expected_tags);
    CHECK(recorded.ramps_intact);
    CHECK(stats.non_silent_blocks == expected_tags.size());
    CHECK(recorded.silent == calls / 4 + stats.skipped_blocks);

    // The dump holds exactly what the output received.
    const auto wav = tox2::readWav(dump_path);
    CHECK(wav.has_value());
    if (wav) {
        CHECK(wav->sample_rate == tox2::kAudioSampleRate && wav->channels == tox2::kAudioOutputChannels);
        CHECK(wav->samples == samples);
    }

    // The periods that elapsed are all accounted for as blocks or skips.
    const double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double until_stop_ms = std::chrono::duration<double, std::milli>(t_stop - t0).count();
    CHECK(periodsToMs(recorded.total) <= elapsed_ms);
    CHECK(periodsToMs(recorded.total) >= until_stop_ms - 80.0);

    // The engine is gone: the next test starts from a clean slate.
    CHECK(tox2::audioEngineStats().blocks == 0 && tox2::audioEngineStats().output.empty());
}

void testEngineSkipsLatePeriods() {
    const fs::path dump_path = g_dir / "engine_skip.wav";
    const auto recording = std::make_shared<Recording>();
    const auto state = std::make_shared<ProducerState>();
    state->stall_call = 5;
    state->stall = std::chrono::milliseconds(25);  // 4.7 periods

    tox2::configureAudioEngine(engineConfig(recording, true, dump_path));
    tox2::startAudioEngine(makeProducer(state));
    CHECK(waitFor([&] { return state->calls.load() >= 20; }));
    tox2::AudioEngineStats stats;
    CHECK(tox2::stopAudioEngineForTest(&stats));

    const std::uint64_t calls = state->calls.load();
    const std::vector<float> samples = recording->take();
    const RecordedBlocks recorded = analyse(samples);

    // The producer ran once per produced block, never for a skipped period; every skipped period is a
    // silent block in the output and the dump.
    CHECK(stats.blocks == calls);
    CHECK(stats.skipped_blocks >= 3);
    CHECK(stats.max_producer_time >= std::chrono::milliseconds(20));
    CHECK(recorded.total == stats.blocks + stats.skipped_blocks);
    CHECK(recorded.silent == stats.skipped_blocks);
    CHECK(stats.non_silent_blocks == calls);
    CHECK(recorded.ramps_intact);
    bool contiguous = recorded.tags.size() == calls;
    for (std::size_t i = 0; i < recorded.tags.size(); ++i) contiguous = contiguous && recorded.tags[i] == i;
    CHECK(contiguous);

    // The silence for the missed periods sits between the stalled block (call 5) and the next one.
    std::size_t run = 0;
    std::size_t index_of_five = 0;
    for (std::size_t b = 0; b < recorded.total; ++b) {
        if (samples[b * tox2::kAudioBlockSamples] == 5.0f * tox2::kAudioBlockFrames + 1.0f) index_of_five = b;
    }
    for (std::size_t b = index_of_five + 1; b < recorded.total; ++b) {
        const float first = samples[b * tox2::kAudioBlockSamples];
        if (first != 0.0f) break;
        ++run;
    }
    CHECK(run >= 3);

    const auto wav = tox2::readWav(dump_path);
    CHECK(wav.has_value() && wav->samples == samples);
    CHECK(wav.has_value() && wav->frames() == recorded.total * tox2::kAudioBlockFrames);
}

void testEngineStopDoesNotWait() {
    using Clock = std::chrono::steady_clock;
    const auto recording = std::make_shared<Recording>();
    // Shared with the producer, which can outlive this function if the engine fails to stop.
    struct Gate {
        std::mutex mutex;  // stands in for the guest lock
        std::atomic<int> calls{0};
    };
    const auto gate_state = std::make_shared<Gate>();
    gate_state->mutex.lock();
    std::mutex& gate = gate_state->mutex;
    std::atomic<int>& calls = gate_state->calls;

    tox2::configureAudioEngine(engineConfig(recording, true, {}));
    tox2::startAudioEngine([gate_state](std::span<float>) {
        gate_state->calls.fetch_add(1);
        std::lock_guard<std::mutex> hold(gate_state->mutex);
    });
    CHECK(waitFor([&] { return calls.load() >= 1; }));

    // The producer is stuck on the gate; neither stopping nor reading the statistics waits for it.
    const Clock::time_point before = Clock::now();
    tox2::requestAudioEngineStop();
    const tox2::AudioEngineStats blocked = tox2::audioEngineStats();
    const auto took = Clock::now() - before;
    CHECK(took < std::chrono::milliseconds(100));
    CHECK(blocked.blocks == 0);

    gate.unlock();
    tox2::AudioEngineStats stats;
    CHECK(tox2::stopAudioEngineForTest(&stats));
    // The thread ended at its next tick without calling the producer again.
    CHECK(calls.load() == 1);
    CHECK(stats.blocks == 1);
}

void testEngineOutputOpenFails() {
    std::mutex lines_mutex;
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) {
        std::lock_guard<std::mutex> lock(lines_mutex);
        lines.emplace_back(line);
    });

    const auto recording = std::make_shared<Recording>();
    const auto state = std::make_shared<ProducerState>();
    // The dump cannot be created either: the engine reports it once and carries on without it.
    tox2::configureAudioEngine(engineConfig(recording, false, g_dir / "no_such_dir" / "x.wav"));
    tox2::startAudioEngine(makeProducer(state));
    CHECK(waitFor([&] { return state->calls.load() >= 5; }));
    tox2::AudioEngineStats stats;
    CHECK(tox2::stopAudioEngineForTest(&stats));
    tox2::setLogSink({});

    CHECK(stats.output == "none (fake failed)");
    CHECK(stats.blocks >= 5);
    CHECK(stats.output_stats.blocks_submitted == stats.blocks + stats.skipped_blocks);
    CHECK(recording->opens == 1 && recording->take().empty());

    auto count = [&](std::string_view needle) {
        std::lock_guard<std::mutex> lock(lines_mutex);
        return std::count_if(lines.begin(), lines.end(),
                             [&](const std::string& line) { return line.find(needle) != std::string::npos; });
    };
    CHECK(count("tox2: audio: fake: device gone; continuing without sound") == 1);
    CHECK(count("continuing without sound") == 1);
    CHECK(count("tox2: audio: output none") == 1);
    CHECK(count("continuing without a dump") == 1);
}

void testEngineFinishDumpDuringOpen() {
    const fs::path dump_path = g_dir / "engine_early_finish.wav";
    std::error_code ec;
    fs::remove(dump_path, ec);

    const auto recording = std::make_shared<Recording>();
    recording->open_delay = std::chrono::milliseconds(200);
    const auto state = std::make_shared<ProducerState>();
    tox2::configureAudioEngine(engineConfig(recording, true, dump_path));
    tox2::startAudioEngine(makeProducer(state));

    // The engine thread is still inside the slow open(): there is no dump to finish yet. The finish
    // request must not be lost; the engine then creates no dump file.
    tox2::finishAudioDump();
    CHECK(waitFor([&] { return state->calls.load() >= 3; }));
    tox2::AudioEngineStats stats;
    CHECK(tox2::stopAudioEngineForTest(&stats));
    CHECK(stats.blocks >= 3);
    CHECK(!recording->take().empty());
    CHECK(!fs::exists(dump_path));
}

void testEngineResumesAfterStop() {
    const fs::path dump_path = g_dir / "engine_resume.wav";
    for (const bool wait_for_exit : {false, true}) {
        const auto recording = std::make_shared<Recording>();
        const auto state = std::make_shared<ProducerState>();
        tox2::configureAudioEngine(engineConfig(recording, true, dump_path));
        tox2::startAudioEngine(makeProducer(state));
        CHECK(waitFor([&] { return state->calls.load() >= 5; }));

        // A start right after a stop request cancels it; one after the thread has ended starts a new
        // thread. Either way production goes on with the same output and dump.
        tox2::requestAudioEngineStop();
        if (wait_for_exit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const std::uint64_t settled = state->calls.load();
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            CHECK(state->calls.load() == settled);
        }
        const std::uint64_t before = state->calls.load();
        tox2::startAudioEngine(makeProducer(std::make_shared<ProducerState>()));
        CHECK(waitFor([&] { return state->calls.load() >= before + 10; }));

        tox2::AudioEngineStats stats;
        CHECK(tox2::stopAudioEngineForTest(&stats));
        const std::vector<float> samples = recording->take();
        const RecordedBlocks recorded = analyse(samples);
        CHECK(recording->opens == 1);
        CHECK(stats.blocks == state->calls.load());
        CHECK(recorded.total == stats.blocks + stats.skipped_blocks);
        const auto wav = tox2::readWav(dump_path);
        CHECK(wav.has_value() && wav->samples == samples);
    }
}

void testEngineFollowsReportedFill() {
    using namespace std::chrono_literals;
    // An output that reports far more than the target in flight: the engine slows down, bounded.
    {
        const auto recording = std::make_shared<Recording>();
        recording->fill = 3000;
        tox2::configureAudioEngine(engineConfig(recording, true, {}));
        tox2::startAudioEngine(makeProducer(std::make_shared<ProducerState>()));
        CHECK(waitFor([&] { return tox2::audioEngineStats().clock_ppm != 0.0; }, 5s));
        const double ppm = tox2::audioEngineStats().clock_ppm;
        CHECK(ppm < 0.0 && ppm >= -tox2::kClockMaxPpm);
        CHECK(tox2::audioEngineStats().mean_fill_frames == 3000.0);
        CHECK(tox2::stopAudioEngineForTest());
    }
    // An output without a fill level keeps the plain clock.
    {
        const auto recording = std::make_shared<Recording>();
        tox2::configureAudioEngine(engineConfig(recording, true, {}));
        const auto state = std::make_shared<ProducerState>();
        tox2::startAudioEngine(makeProducer(state));
        CHECK(waitFor([&] { return state->calls.load() >= 250; }));
        CHECK(tox2::audioEngineStats().clock_ppm == 0.0);
        CHECK(tox2::audioEngineStats().mean_fill_frames == 0.0);
        CHECK(tox2::stopAudioEngineForTest());
    }
}

// ---- cellAudio on synthetic guest memory ----

constexpr std::uint32_t kAudioNotInit = 0x80310703;
constexpr std::uint32_t kAudioAlreadyInit = 0x80310701;
constexpr std::uint32_t kAudioParam = 0x80310704;
constexpr std::uint32_t kAudioPortFull = 0x80310705;
constexpr std::uint32_t kAudioAlreadyRun = 0x80310706;
constexpr std::uint32_t kAudioPortNotOpen = 0x80310707;
constexpr std::uint32_t kAudioPortNotRun = 0x80310708;
constexpr std::uint32_t kAudioTransEvent = 0x80310709;
constexpr std::uint32_t kAudioTagNotFound = 0x8031070f;
constexpr std::uint64_t kNotifyKey = 0x8000cafe02460300;  // the game mixer's first notify queue key

// One simulated guest thread that holds the guest lock, with guest memory reserved and the
// cellAudio state, the event objects and the audio engine reset. Release the lock before the engine
// is stopped: its producer may be waiting for it.
class AudioGuest {
public:
    AudioGuest() : caller_(0x01000000, tox2::GuestThreadParams{}, 0, 0) {
        tox2::setLogSink([](std::string_view) {});
        tox2::resetSyncObjects();
        tox2::resetCellAudio();
        tox2::configureAudioEngine({});
        CHECK(tox2::process().memory.reserve());
        base_ = tox2::process().memory.base();
        scratch_ = tox2::process().main.allocate(0x10000, 0x10000);
        CHECK(scratch_ != 0);
        tox2::guestLock().lock();
    }
    ~AudioGuest() {
        tox2::guestLock().unlock();
        CHECK(tox2::stopAudioEngineForTest());
        tox2::resetCellAudio();
        tox2::setLogSink({});
    }

    std::uint8_t* base() const { return base_; }
    // Guest address of scratch byte `offset`.
    std::uint32_t at(std::uint32_t offset) const { return scratch_ + offset; }

    // Calls the handler of a cellAudio import and returns its result as the guest sees it in r3.
    std::uint32_t call(std::string_view name, std::uint64_t r3 = 0, std::uint64_t r4 = 0, std::uint64_t r5 = 0) {
        for (const tox2::HleEntry& entry : tox2::cellAudioHandlers()) {
            if (name != entry.name) continue;
            Tox2PpuContext* ctx = &caller_.context();
            ctx->gpr[3] = r3;
            ctx->gpr[4] = r4;
            ctx->gpr[5] = r5;
            entry.handler(ctx, base_);
            return static_cast<std::uint32_t>(ctx->gpr[3]);
        }
        CHECK(false);
        return 0;
    }

    // cellAudioInit, then stops the engine it started, so that only runPeriod drives periods: a real
    // producer call that sneaks in while a timed receive gives up the guest lock would add a notify.
    // A plain stop request is not enough, because one queued producer call still completes.
    std::uint32_t initWithoutEngine() {
        const std::uint32_t result = call("cellAudioInit");
        tox2::guestLock().unlock();
        CHECK(tox2::stopAudioEngineForTest());
        tox2::guestLock().lock();
        return result;
    }

    // Writes a CellAudioPortParam at scratch `offset` and opens a port with it.
    std::uint32_t open(std::uint32_t offset, std::uint64_t channels, std::uint64_t blocks, std::uint64_t attr,
                       float level, std::uint32_t port_out) {
        tox2Write64(base_, at(offset), channels);
        tox2Write64(base_, at(offset + 8), blocks);
        tox2Write64(base_, at(offset + 0x10), attr);
        tox2Write32(base_, at(offset + 0x18), std::bit_cast<std::uint32_t>(level));
        return call("cellAudioPortOpen", at(offset), port_out);
    }

    // Creates an event queue with `key` (PPU queue, FIFO, depth 2) through the syscall; returns its id.
    std::uint32_t createQueue(std::uint64_t key) {
        tox2Write32(base_, at(0x400), 1);
        tox2Write32(base_, at(0x404), 1);
        const std::uint64_t result = syscall(128, at(0x410), at(0x400), key, 2);
        CHECK(result == 0);
        return tox2Read32(base_, at(0x410));
    }

    // The result of `sys_event_queue_receive(id, buffer, 1 ms)`; the event words come back in r4..r7.
    std::uint64_t receive(std::uint32_t id, std::array<std::uint64_t, 4>& event) {
        const std::uint64_t result = syscall(130, id, at(0x440), 1000);
        Tox2PpuContext* ctx = &caller_.context();
        event = {ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7]};
        return result;
    }

    // cellAudioSetPortLevel: the level travels in f1, not in a general register.
    std::uint32_t setPortLevel(std::uint32_t port, double level) {
        caller_.context().fpr[1].f64 = level;
        return call("cellAudioSetPortLevel", port);
    }

    std::uint64_t syscall(std::uint32_t number, std::uint64_t r3, std::uint64_t r4 = 0, std::uint64_t r5 = 0,
                          std::uint64_t r6 = 0) {
        Tox2PpuContext* ctx = &caller_.context();
        ctx->gpr[11] = number;
        ctx->gpr[3] = r3;
        ctx->gpr[4] = r4;
        ctx->gpr[5] = r5;
        ctx->gpr[6] = r6;
        tox2Syscall(ctx, base_, 0x10000);
        return ctx->gpr[3];
    }

    // The 32-bit result of the last syscall as a signed lv2 error.
    static std::uint64_t err(std::uint32_t code) {
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(code)));
    }

private:
    tox2::GuestThread caller_;
    std::uint8_t* base_ = nullptr;
    std::uint32_t scratch_ = 0;
};

void writeFloat(std::uint8_t* base, std::uint32_t addr, float value) {
    tox2Write32(base, addr, std::bit_cast<std::uint32_t>(value));
}

// Runs one period and returns the stereo block.
tox2::AudioBlock runPeriod(AudioGuest& guest, std::uint64_t now_us) {
    tox2::AudioBlock out{};
    tox2::cellAudioPorts().producePeriod(guest.base(), now_us, out);
    return out;
}

void testCellAudioStateErrors() {
    AudioGuest guest;
    // Everything but Init needs Init first.
    for (const char* name :
         {"cellAudioQuit", "cellAudioPortOpen", "cellAudioGetPortConfig", "cellAudioPortStart", "cellAudioPortStop",
          "cellAudioPortClose", "cellAudioGetPortBlockTag", "cellAudioGetPortTimestamp", "cellAudioSetPortLevel",
          "cellAudioSetNotifyEventQueue", "cellAudioRemoveNotifyEventQueue"}) {
        CHECK(guest.call(name, guest.at(0), guest.at(0x40), guest.at(0x80)) == kAudioNotInit);
    }
    CHECK(guest.initWithoutEngine() == 0);
    CHECK(guest.call("cellAudioInit") == kAudioAlreadyInit);
    CHECK(guest.call("cellAudioQuit") == 0);
    CHECK(guest.call("cellAudioQuit") == kAudioNotInit);
    CHECK(tox2::cellAudioHandlers().size() == 12);
}

void testCellAudioPortOpenAndConfig() {
    AudioGuest guest;
    std::uint8_t* base = guest.base();
    CHECK(guest.initWithoutEngine() == 0);

    // The game's parameters.
    CHECK(guest.open(0x00, 8, 8, 0, 1.0f, guest.at(0x60)) == 0);
    CHECK(tox2Read32(base, guest.at(0x60)) == 0);
    CHECK(guest.call("cellAudioGetPortConfig", 0, guest.at(0x80)) == 0);
    const std::uint32_t cell = tox2Read32(base, guest.at(0x80));
    const std::uint32_t memory = tox2Read32(base, guest.at(0x80 + 0x1c));
    CHECK(cell != 0 && cell % 16 == 0 && tox2::process().memory.isCommitted(cell, 8));
    CHECK(tox2Read64(base, cell) == 0);
    CHECK(memory != 0 && memory % 0x10000 == 0 && tox2::process().memory.isCommitted(memory, 0x10000));
    CHECK(tox2Read32(base, guest.at(0x80 + 4)) == 1);  // opened
    CHECK(tox2Read64(base, guest.at(0x80 + 8)) == 8);
    CHECK(tox2Read64(base, guest.at(0x80 + 0x10)) == 8);
    CHECK(tox2Read32(base, guest.at(0x80 + 0x18)) == 0x10000);

    // Start zeroes the port memory and marks the port running.
    tox2Write32(base, memory + 0x100, 0x3f800000);
    CHECK(guest.call("cellAudioPortStart", 0) == 0);
    CHECK(tox2Read32(base, memory + 0x100) == 0);
    CHECK(guest.call("cellAudioGetPortConfig", 0, guest.at(0x80)) == 0);
    CHECK(tox2Read32(base, guest.at(0x80 + 4)) == 2);
    CHECK(guest.call("cellAudioPortStart", 0) == kAudioAlreadyRun);

    // A second port gets the next number and its own cell and memory.
    CHECK(guest.open(0x00, 2, 4, 0, 1.0f, guest.at(0x60)) == 0);
    CHECK(tox2Read32(base, guest.at(0x60)) == 1);
    CHECK(guest.call("cellAudioGetPortConfig", 1, guest.at(0xc0)) == 0);
    CHECK(tox2Read32(base, guest.at(0xc0)) == cell + 16);
    CHECK(tox2Read32(base, guest.at(0xc0 + 0x1c)) != memory);
    CHECK(tox2Read32(base, guest.at(0xc0 + 0x18)) == 2 * 4 * 1024);

    // Stop, start again; closing works from any open state; the number is reused.
    CHECK(guest.call("cellAudioPortStop", 1) == kAudioPortNotRun);
    CHECK(guest.call("cellAudioPortStop", 0) == 0);
    CHECK(guest.call("cellAudioPortStop", 0) == kAudioPortNotRun);
    CHECK(guest.call("cellAudioPortStart", 0) == 0);
    CHECK(guest.call("cellAudioPortClose", 0) == 0);
    CHECK(guest.call("cellAudioPortClose", 0) == kAudioPortNotOpen);
    CHECK(guest.call("cellAudioPortStart", 0) == kAudioPortNotOpen);
    CHECK(guest.call("cellAudioGetPortConfig", 0, guest.at(0x80)) == 0);
    CHECK(tox2Read32(base, guest.at(0x80 + 4)) == 0x1010);
    CHECK(guest.open(0x00, 8, 8, 0, 1.0f, guest.at(0x60)) == 0);
    CHECK(tox2Read32(base, guest.at(0x60)) == 0);

    // Argument checks.
    CHECK(guest.call("cellAudioGetPortConfig", 8, guest.at(0x80)) == kAudioParam);
    CHECK(guest.call("cellAudioGetPortConfig", 0, 0) == kAudioParam);
    CHECK(guest.call("cellAudioPortStart", 8) == kAudioParam);
    CHECK(guest.call("cellAudioPortStop", 8) == kAudioParam);
    CHECK(guest.call("cellAudioPortClose", 8) == kAudioParam);
    CHECK(guest.call("cellAudioPortOpen", 0, guest.at(0x60)) == kAudioParam);
    CHECK(guest.call("cellAudioPortOpen", guest.at(0), 0) == kAudioParam);
    CHECK(guest.open(0x00, 4, 8, 0, 1.0f, guest.at(0x60)) == kAudioParam);
    CHECK(guest.open(0x00, 2, 3, 0, 1.0f, guest.at(0x60)) == kAudioParam);
    CHECK(guest.open(0x00, 2, 64, 0, 1.0f, guest.at(0x60)) == kAudioParam);

    // Eight ports exist; a ninth open fails.
    for (int i = 0; i < 6; ++i) CHECK(guest.open(0x00, 2, 2, 0, 1.0f, guest.at(0x60)) == 0);
    CHECK(guest.open(0x00, 2, 2, 0, 1.0f, guest.at(0x60)) == kAudioPortFull);

    CHECK(guest.call("cellAudioQuit") == 0);
}

void testCellAudioTagsAndTimestamps() {
    AudioGuest guest;
    std::uint8_t* base = guest.base();
    CHECK(guest.initWithoutEngine() == 0);

    // Periods before any port is open still count, so tags do not start at 0.
    constexpr std::uint64_t kStart = 1'000'000;
    for (std::uint64_t i = 0; i < 5; ++i) runPeriod(guest, kStart + i * 5333);
    CHECK(tox2::cellAudioPorts().stats().periods == 5);

    CHECK(guest.open(0x00, 8, 8, 0, 1.0f, guest.at(0x60)) == 0);
    CHECK(guest.call("cellAudioGetPortConfig", 0, guest.at(0x80)) == 0);
    const std::uint32_t cell = tox2Read32(base, guest.at(0x80));
    CHECK(guest.call("cellAudioPortStart", 0) == 0);

    const std::uint64_t t5 = kStart + 5 * 5333;
    const std::uint32_t tag_out = guest.at(0x100);
    const std::uint32_t stamp_out = guest.at(0x108);
    CHECK(guest.call("cellAudioGetPortBlockTag", 0, 0, tag_out) == 0);
    CHECK(tox2Read64(base, tag_out) == 5);

    // Period k publishes the next block with tag k; the cell holds the block the library mixes next.
    for (std::uint64_t i = 0; i < 12; ++i) {
        runPeriod(guest, t5 + i * 5333);
        const std::uint64_t index = (i + 1) % 8;
        CHECK(tox2Read64(base, cell) == index);
        CHECK(guest.call("cellAudioGetPortBlockTag", 0, index, tag_out) == 0);
        CHECK(tox2Read64(base, tag_out) == 5 + i);
        CHECK(guest.call("cellAudioGetPortBlockTag", 0, (index + 1) % 8, tag_out) == 0);
        if ((index + 1) % 8 > index) CHECK(tox2Read64(base, tag_out) == 5 + i + 1);
    }
    // 12 periods since open: the latest period is number 16 at t5 + 11 periods.
    const std::uint64_t latest_time = t5 + 11 * 5333;
    tox2Write64(base, stamp_out, 0xdeadbeef);
    CHECK(guest.call("cellAudioGetPortTimestamp", 0, 16, stamp_out) == 0);
    CHECK(tox2Read64(base, stamp_out) == latest_time);
    CHECK(guest.call("cellAudioGetPortTimestamp", 0, 13, stamp_out) == 0);
    CHECK(tox2Read64(base, stamp_out) == latest_time - 3 * 5333);
    // No history: tag 0 answers too, extrapolated back at one period per tag.
    CHECK(guest.call("cellAudioGetPortTimestamp", 0, 0, stamp_out) == 0);
    CHECK(tox2Read64(base, stamp_out) == latest_time - 16 * 5333);
    CHECK(guest.call("cellAudioGetPortTimestamp", 0, 15, 0) == 0);  // null output: nothing written

    // A tag that was never issued: error, output untouched.
    tox2Write64(base, stamp_out, 0xdeadbeef);
    CHECK(guest.call("cellAudioGetPortTimestamp", 0, 17, stamp_out) == kAudioTagNotFound);
    CHECK(tox2Read64(base, stamp_out) == 0xdeadbeef);

    // Stopping freezes the counters but they still answer.
    CHECK(guest.call("cellAudioPortStop", 0) == 0);
    runPeriod(guest, t5 + 20 * 5333);
    CHECK(guest.call("cellAudioGetPortTimestamp", 0, 16, stamp_out) == 0);
    CHECK(tox2Read64(base, stamp_out) == latest_time);
    CHECK(guest.call("cellAudioGetPortTimestamp", 0, 17, stamp_out) == kAudioTagNotFound);

    // Argument checks: block number, port number, closed port, unwritten output.
    CHECK(guest.call("cellAudioGetPortBlockTag", 0, 8, tag_out) == kAudioParam);
    CHECK(guest.call("cellAudioGetPortBlockTag", 8, 0, tag_out) == kAudioParam);
    CHECK(guest.call("cellAudioGetPortBlockTag", 1, 0, tag_out) == kAudioPortNotOpen);
    CHECK(guest.call("cellAudioGetPortTimestamp", 1, 0, stamp_out) == kAudioPortNotOpen);
    CHECK(guest.call("cellAudioGetPortTimestamp", 8, 0, stamp_out) == kAudioParam);
    CHECK(guest.call("cellAudioGetPortBlockTag", 0, 0, 0) == 0);
}

void testCellAudioMixing() {
    AudioGuest guest;
    std::uint8_t* base = guest.base();
    CHECK(guest.initWithoutEngine() == 0);

    // 8 channels: a ramp on all of them. Only the front pair reaches the output; the rest is counted.
    CHECK(guest.open(0x00, 8, 8, 0, 0.25f, guest.at(0x60)) == 0);  // attr 0: the level argument is ignored
    CHECK(guest.call("cellAudioGetPortConfig", 0, guest.at(0x80)) == 0);
    const std::uint32_t cell = tox2Read32(base, guest.at(0x80));
    const std::uint32_t memory = tox2Read32(base, guest.at(0x80 + 0x1c));

    // Opened but not started: contributes nothing, the cell stays.
    writeFloat(base, memory, 0.5f);
    CHECK(std::ranges::all_of(runPeriod(guest, 1000), [](float s) { return s == 0.0f; }));
    CHECK(tox2Read64(base, cell) == 0);

    CHECK(guest.call("cellAudioPortStart", 0) == 0);
    for (std::uint32_t frame = 0; frame < 256; ++frame) {
        for (std::uint32_t channel = 0; channel < 8; ++channel) {
            const float value = channel == 0   ? static_cast<float>(frame) / 512.0f
                                : channel == 1 ? -static_cast<float>(frame) / 1024.0f
                                               : 0.25f * static_cast<float>(channel);
            writeFloat(base, memory + (frame * 8 + channel) * 4, value);
        }
    }
    const tox2::AudioBlock out = runPeriod(guest, 2000);
    bool front_intact = true;
    for (std::uint32_t frame = 0; frame < 256; ++frame) {
        front_intact = front_intact && out[2 * frame] == static_cast<float>(frame) / 512.0f &&
                       out[2 * frame + 1] == -static_cast<float>(frame) / 1024.0f;
    }
    CHECK(front_intact);

    // The consumed block is zeroed and the index advanced as a big-endian u64.
    bool zeroed = true;
    for (std::uint32_t offset = 0; offset < 0x2000; offset += 4)
        zeroed = zeroed && tox2Read32(base, memory + offset) == 0;
    CHECK(zeroed);
    CHECK(base[cell + 7] == 1 && tox2Read64(base, cell) == 1);

    const tox2::CellAudioStats stats = tox2::cellAudioPorts().stats();
    CHECK(stats.nonzero_samples[0] == 255 && stats.nonzero_samples[1] == 255);
    for (std::size_t channel = 2; channel < 8; ++channel) CHECK(stats.nonzero_samples[channel] == 256);

    // Block 1 was never written: the next period is silent and the index moves on.
    CHECK(std::ranges::all_of(runPeriod(guest, 3000), [](float s) { return s == 0.0f; }));
    CHECK(tox2Read64(base, cell) == 2);
    CHECK(guest.call("cellAudioPortClose", 0) == 0);

    // 2 channels: scaled by the initial level only when the attribute asks for it; ports add up.
    CHECK(guest.open(0x00, 2, 2, 0x1000, 0.5f, guest.at(0x60)) == 0);
    const std::uint32_t scaled = tox2Read32(base, guest.at(0x60));
    CHECK(guest.open(0x00, 2, 4, 0, 0.25f, guest.at(0x60)) == 0);
    const std::uint32_t plain = tox2Read32(base, guest.at(0x60));
    CHECK(guest.call("cellAudioGetPortConfig", scaled, guest.at(0x80)) == 0);
    const std::uint32_t scaled_memory = tox2Read32(base, guest.at(0x80 + 0x1c));
    CHECK(guest.call("cellAudioGetPortConfig", plain, guest.at(0xc0)) == 0);
    const std::uint32_t plain_memory = tox2Read32(base, guest.at(0xc0 + 0x1c));
    CHECK(guest.call("cellAudioPortStart", scaled) == 0);
    CHECK(guest.call("cellAudioPortStart", plain) == 0);
    for (std::uint32_t frame = 0; frame < 256; ++frame) {
        writeFloat(base, scaled_memory + frame * 8, 0.25f);
        writeFloat(base, scaled_memory + frame * 8 + 4, 0.5f);
        writeFloat(base, plain_memory + frame * 8, 0.125f);
        writeFloat(base, plain_memory + frame * 8 + 4, 0.0625f);
    }
    // Frame 255 overdrives: 3.0 * 0.5 + 0.125 clamps to 1.
    writeFloat(base, scaled_memory + 255 * 8, 3.0f);
    writeFloat(base, scaled_memory + 255 * 8 + 4, -3.0f);
    const tox2::AudioBlock mixed = runPeriod(guest, 4000);
    CHECK(mixed[0] == 0.25f && mixed[1] == 0.3125f);
    CHECK(mixed[2 * 255] == 1.0f && mixed[2 * 255 + 1] == -1.0f);
    CHECK(tox2Read64(base, tox2Read32(base, guest.at(0x80))) == 1);
    CHECK(tox2Read64(base, tox2Read32(base, guest.at(0xc0))) == 1);
}

void testCellAudioPortLevel() {
    AudioGuest guest;
    std::uint8_t* base = guest.base();
    CHECK(guest.initWithoutEngine() == 0);

    CHECK(guest.open(0x00, 2, 2, 0, 1.0f, guest.at(0x60)) == 0);
    CHECK(guest.call("cellAudioGetPortConfig", 0, guest.at(0x80)) == 0);
    const std::uint32_t memory = tox2Read32(base, guest.at(0x80 + 0x1c));
    CHECK(guest.call("cellAudioPortStart", 0) == 0);

    // The level applies to the next period, also on a started port.
    CHECK(guest.setPortLevel(0, 0.5) == 0);
    writeFloat(base, memory, 0.5f);
    writeFloat(base, memory + 4, -0.25f);
    tox2::AudioBlock out = runPeriod(guest, 1000);
    CHECK(out[0] == 0.25f && out[1] == -0.125f);
    CHECK(guest.setPortLevel(0, 0.0) == 0);
    writeFloat(base, memory + 2 * 256 * 4, 0.5f);
    CHECK(std::ranges::all_of(runPeriod(guest, 2000), [](float s) { return s == 0.0f; }));

    // Errors leave the level alone; a port that was closed and opened again starts at 1.
    CHECK(guest.setPortLevel(0, -0.5) == kAudioParam);
    CHECK(guest.setPortLevel(0, std::numeric_limits<double>::quiet_NaN()) == kAudioParam);
    CHECK(guest.setPortLevel(8, 1.0) == kAudioParam);
    CHECK(guest.setPortLevel(1, 1.0) == kAudioPortNotOpen);
    CHECK(guest.call("cellAudioPortClose", 0) == 0);
    CHECK(guest.setPortLevel(0, 1.0) == kAudioPortNotOpen);
    CHECK(guest.open(0x00, 2, 2, 0, 1.0f, guest.at(0x60)) == 0);
    CHECK(guest.call("cellAudioGetPortConfig", 0, guest.at(0x80)) == 0);
    CHECK(guest.call("cellAudioPortStart", 0) == 0);  // start zeroes the port memory
    writeFloat(base, tox2Read32(base, guest.at(0x80 + 0x1c)), 0.5f);
    CHECK(runPeriod(guest, 3000)[0] == 0.5f);
    CHECK(guest.call("cellAudioQuit") == 0);
}

void testCellAudioNotify() {
    AudioGuest guest;
    std::uint8_t* base = guest.base();
    (void)base;
    CHECK(guest.initWithoutEngine() == 0);
    const std::uint32_t queue = guest.createQueue(kNotifyKey);
    CHECK(queue != 0);

    // Unknown keys, the local key and a second registration of the same queue fail.
    CHECK(guest.call("cellAudioSetNotifyEventQueue", kNotifyKey + 1) == kAudioTransEvent);
    CHECK(guest.call("cellAudioSetNotifyEventQueue", 0) == kAudioTransEvent);
    CHECK(guest.call("cellAudioRemoveNotifyEventQueue", kNotifyKey) == kAudioTransEvent);
    CHECK(guest.call("cellAudioSetNotifyEventQueue", kNotifyKey) == 0);
    CHECK(guest.call("cellAudioSetNotifyEventQueue", kNotifyKey) == kAudioTransEvent);

    // One event per period, even with no port started: source, then three zeros.
    std::array<std::uint64_t, 4> event{};
    runPeriod(guest, 1000);
    CHECK(guest.receive(queue, event) == 0);
    CHECK(event[0] == 0x1'0e00'0000 && event[1] == 0 && event[2] == 0 && event[3] == 0);

    // The queue holds two events; the third is refused and only counted.
    runPeriod(guest, 2000);
    runPeriod(guest, 3000);
    runPeriod(guest, 4000);
    const tox2::CellAudioStats stats = tox2::cellAudioPorts().stats();
    CHECK(stats.notifies_sent == 3 && stats.notifies_failed == 1);
    CHECK(guest.receive(queue, event) == 0);
    CHECK(guest.receive(queue, event) == 0);
    CHECK(guest.receive(queue, event) == AudioGuest::err(0x8001000B));  // the fourth event never existed

    // After Remove nothing more arrives.
    CHECK(guest.call("cellAudioRemoveNotifyEventQueue", kNotifyKey) == 0);
    CHECK(guest.call("cellAudioRemoveNotifyEventQueue", kNotifyKey) == kAudioTransEvent);
    runPeriod(guest, 5000);
    CHECK(guest.receive(queue, event) == AudioGuest::err(0x8001000B));
    CHECK(tox2::cellAudioPorts().stats().notifies_sent == 3);

    // A destroyed queue only costs failed notifies, and its registration goes at the next Set.
    CHECK(guest.call("cellAudioSetNotifyEventQueue", kNotifyKey) == 0);
    CHECK(guest.syscall(129, queue, 0) == 0);
    runPeriod(guest, 6000);
    CHECK(tox2::cellAudioPorts().stats().notifies_failed == 2);
    const std::uint32_t second = guest.createQueue(kNotifyKey + 1);
    CHECK(guest.call("cellAudioSetNotifyEventQueue", kNotifyKey + 1) == 0);
    runPeriod(guest, 7000);
    CHECK(tox2::cellAudioPorts().stats().notifies_failed == 2);
    CHECK(tox2::cellAudioPorts().stats().notifies_sent == 4);
    CHECK(guest.receive(second, event) == 0);
    // The third successful registration since reset has its own source.
    CHECK(event[0] == 0x1'0e00'0000 + 2 * 0x100);
}

void testCellAudioEngine() {
    AudioGuest guest;
    CHECK(guest.call("cellAudioInit") == 0);
    const auto periods = [] { return tox2::cellAudioPorts().stats().periods; };

    // Init starts the engine, which calls the producer once the guest lock is free.
    tox2::guestLock().unlock();
    CHECK(waitFor([&] { return periods() >= 5; }));
    tox2::guestLock().lock();

    // Quit returns while the engine may be waiting for the lock we hold, and the engine stops
    // calling the producer (at most the call that was already under way completes).
    CHECK(guest.call("cellAudioQuit") == 0);
    tox2::guestLock().unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const std::uint64_t settled_calls = tox2::audioEngineStats().blocks;
    const std::uint64_t settled = periods();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(tox2::audioEngineStats().blocks == settled_calls);
    CHECK(periods() == settled);
    tox2::guestLock().lock();

    // A second Init resumes the same engine.
    CHECK(guest.call("cellAudioInit") == 0);
    tox2::guestLock().unlock();
    CHECK(waitFor([&] { return periods() >= settled + 5; }));
    tox2::guestLock().lock();
}

// Synthetic ATRAC3plus RIFF entries.
using Bytes = std::vector<std::uint8_t>;

void put16(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put32(Bytes& out, std::uint32_t v) {
    put16(out, v);
    put16(out, v >> 16);
}

void putTag(Bytes& out, const char* tag) { out.insert(out.end(), tag, tag + 4); }

constexpr std::uint8_t kAt3Guid[16] = {0xbf, 0xaa, 0x23, 0xe9, 0x58, 0xcb, 0x71, 0x44,
                                       0xa1, 0x19, 0xff, 0xfa, 0x01, 0xe4, 0xce, 0x62};

// Chunk with its pad byte when the size is odd.
Bytes chunk(const char* tag, const Bytes& body) {
    Bytes out;
    putTag(out, tag);
    put32(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    if (body.size() & 1) out.push_back(0);
    return out;
}

Bytes fmtBody(std::uint32_t channels, const std::uint8_t* guid = kAt3Guid, std::uint32_t block_align = 512) {
    Bytes b;
    put16(b, 0xfffe);
    put16(b, channels);
    put32(b, 48000);
    put32(b, 12000);
    put16(b, block_align);
    put16(b, 0);
    put16(b, 34);                     // cbSize
    put16(b, 2048);                   // valid bits
    put32(b, channels == 1 ? 1 : 3);  // channel mask
    b.insert(b.end(), guid, guid + 16);
    for (int i = 0; i < 12; ++i) b.push_back(i == 0 ? 1 : 0);
    return b;
}

Bytes factBody(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    Bytes out;
    put32(out, a);
    put32(out, b);
    put32(out, c);
    return out;
}

Bytes smplBody(std::uint32_t loops, std::uint32_t start, std::uint32_t end) {
    Bytes b;
    for (int i = 0; i < 7; ++i) put32(b, 0);
    put32(b, loops);
    put32(b, 0);  // sampler data size
    for (std::uint32_t i = 0; i < loops; ++i) {
        put32(b, i);          // id
        put32(b, 0);          // type
        put32(b, start + i);  // start
        put32(b, end + i);    // end
        put32(b, 0);          // fraction
        put32(b, 0);          // play count
    }
    return b;
}

// RIFF header plus the given chunks and a data chunk header declaring `data_size` bytes. The payload
// is not appended: the parser must not need it.
Bytes riffEntry(const std::vector<Bytes>& chunks, std::uint32_t data_size) {
    Bytes body;
    putTag(body, "WAVE");
    for (const Bytes& c : chunks) body.insert(body.end(), c.begin(), c.end());
    putTag(body, "data");
    put32(body, data_size);
    Bytes out;
    putTag(out, "RIFF");
    put32(out, static_cast<std::uint32_t>(body.size()) + data_size);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

void testAt3ParseStereoWithLoop() {
    const Bytes file = riffEntry({chunk("fmt ", fmtBody(2)), chunk("fact", factBody(5097599, 3601, 3785)),
                                  chunk("smpl", smplBody(1, 3785, 5101383))},
                                 1275392);
    const auto info = tox2::parseAt3Header(file);
    CHECK(info.has_value());
    if (!info) return;
    CHECK(info->channels == 2);
    CHECK(info->sample_rate == 48000);
    CHECK(info->block_align == 512);
    CHECK(info->bytes_per_second == 12000);
    CHECK(info->total_samples == 5097599);
    CHECK(info->delay_samples == 3785);
    CHECK(info->loop.has_value());
    if (info->loop) {
        CHECK(info->loop->start_sample == 3785);
        CHECK(info->loop->end_sample == 5101383);
    }
    CHECK(info->data_size == 1275392);
    CHECK(info->data_offset == file.size());
    CHECK(info->frameCount() == 2491);
    CHECK(tox2::kAtrac3plusFrameSamples == 2048);
}

void testAt3ParseMonoWithoutLoop() {
    // An unknown chunk of odd size: its pad byte must be skipped. Chunk order differs from the game's.
    const Bytes file = riffEntry(
            {chunk("fmt ", fmtBody(1)), chunk("LIST", Bytes{1, 2, 3}), chunk("fact", factBody(1000, 2048, 2232))},
            512 * 3);
    const auto info = tox2::parseAt3Header(file);
    CHECK(info.has_value());
    if (!info) return;
    CHECK(info->channels == 1);
    CHECK(info->total_samples == 1000);
    CHECK(info->delay_samples == 2232);
    CHECK(!info->loop.has_value());
    CHECK(info->frameCount() == 3);
    CHECK(info->data_offset == file.size());
}

void testAt3ParseOptionalChunks() {
    // No fact: nothing known about length or delay. A one-dword fact has a length but no delay.
    auto info = tox2::parseAt3Header(riffEntry({chunk("fmt ", fmtBody(2))}, 1024));
    CHECK(info.has_value());
    if (info) {
        CHECK(info->total_samples == 0);
        CHECK(info->delay_samples == 0);
    }
    Bytes one_dword;
    put32(one_dword, 777);
    info = tox2::parseAt3Header(riffEntry({chunk("fmt ", fmtBody(2)), chunk("fact", one_dword)}, 1024));
    CHECK(info.has_value());
    if (info) {
        CHECK(info->total_samples == 777);
        CHECK(info->delay_samples == 0);
    }
    // smpl with no loops, and with two loops (the first one is reported).
    info = tox2::parseAt3Header(riffEntry({chunk("fmt ", fmtBody(2)), chunk("smpl", smplBody(0, 0, 0))}, 512));
    CHECK(info.has_value() && !info->loop.has_value());
    info = tox2::parseAt3Header(riffEntry({chunk("fmt ", fmtBody(2)), chunk("smpl", smplBody(2, 10, 20))}, 512));
    CHECK(info.has_value() && info->loop.has_value());
    if (info && info->loop) {
        CHECK(info->loop->start_sample == 10);
        CHECK(info->loop->end_sample == 20);
    }
    // A data size that is not a multiple of block_align is reported as it is; frameCount() rounds down.
    info = tox2::parseAt3Header(riffEntry({chunk("fmt ", fmtBody(2))}, 1300));
    CHECK(info.has_value() && info->data_size == 1300 && info->frameCount() == 2);
}

void testAt3ParseRejects() {
    // A plain WAVE_FORMAT_ATRAC3 (0x0270) header.
    Bytes plain;
    put16(plain, 0x0270);
    put16(plain, 2);
    put32(plain, 44100);
    put32(plain, 16537);
    put16(plain, 384);
    put16(plain, 0);
    put16(plain, 14);
    for (int i = 0; i < 14; ++i) plain.push_back(0);
    auto info = tox2::parseAt3Header(riffEntry({chunk("fmt ", plain)}, 384));
    CHECK(!info.has_value());
    if (!info) CHECK(info.error().find("0x0270") != std::string::npos);

    // Extensible with another sub-format GUID (the PCM one).
    constexpr std::uint8_t kPcmGuid[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                           0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
    info = tox2::parseAt3Header(riffEntry({chunk("fmt ", fmtBody(2, kPcmGuid))}, 512));
    CHECK(!info.has_value());

    CHECK(!tox2::parseAt3Header(riffEntry({chunk("fmt ", fmtBody(2, kAt3Guid, 0))}, 512)).has_value());
    CHECK(!tox2::parseAt3Header(riffEntry({chunk("fmt ", fmtBody(0))}, 512)).has_value());
    CHECK(!tox2::parseAt3Header(riffEntry({chunk("fmt ", Bytes(10, 0))}, 512)).has_value());
    // data before fmt, and no fmt at all.
    CHECK(!tox2::parseAt3Header(riffEntry({chunk("fact", factBody(1, 2, 3))}, 512)).has_value());
    // smpl that claims a loop it does not hold.
    Bytes short_smpl = smplBody(1, 0, 0);
    short_smpl.resize(40);
    CHECK(!tox2::parseAt3Header(riffEntry({chunk("fmt ", fmtBody(2)), chunk("smpl", short_smpl)}, 512)).has_value());
    // Not RIFF/WAVE.
    Bytes bad_magic = riffEntry({chunk("fmt ", fmtBody(2))}, 512);
    bad_magic[8] = 'X';
    CHECK(!tox2::parseAt3Header(bad_magic).has_value());
    CHECK(!tox2::parseAt3Header(Bytes{}).has_value());
}

void testAt3ParseTruncated() {
    const Bytes file = riffEntry({chunk("fmt ", fmtBody(2)), chunk("fact", factBody(5, 6, 7))}, 512);
    CHECK(tox2::parseAt3Header(file).has_value());
    // Every prefix that stops before the end of the data chunk header is an error, and none may crash.
    bool all_rejected = true;
    for (std::size_t cut = 0; cut < file.size(); ++cut) {
        if (tox2::parseAt3Header(std::span<const std::uint8_t>(file).first(cut)).has_value()) all_rejected = false;
    }
    CHECK(all_rejected);
}

void testScanRiffEntries() {
    const Bytes first = riffEntry({chunk("fmt ", fmtBody(2))}, 8);
    Bytes second = riffEntry({chunk("fmt ", fmtBody(1)), chunk("fact", factBody(1, 2, 3))}, 8);

    Bytes archive(5, 0xaa);
    const std::size_t first_at = archive.size();
    archive.insert(archive.end(), first.begin(), first.end());
    // The payload of the first entry holds a RIFF/WAVE magic: it must not be reported.
    Bytes payload;
    putTag(payload, "RIFF");
    put32(payload, 4);
    putTag(payload, "WAVE");
    archive.insert(archive.end(), payload.begin(), payload.begin() + 8);
    const std::size_t second_at = archive.size();
    archive.insert(archive.end(), second.begin(), second.end());
    archive.insert(archive.end(), 7, 0x55);

    auto entries = tox2::scanRiffEntries(archive);
    CHECK(entries.size() == 2);
    if (entries.size() == 2) {
        CHECK(entries[0].first == first_at);
        CHECK(entries[0].second == first.size());  // header bytes after the size field plus the 8-byte payload
        CHECK(entries[1].first == second_at);
    }

    CHECK(tox2::scanRiffEntries(Bytes{}).empty());
    CHECK(tox2::scanRiffEntries(Bytes(100, 'R')).empty());
    // A declared size that runs past the archive still reports the entry.
    Bytes cut_entry(second.begin(), second.begin() + 20);
    entries = tox2::scanRiffEntries(cut_entry);
    CHECK(entries.size() == 1);
    // "RIFF" without "WAVE" is not an entry.
    Bytes avi = first;
    avi[8] = 'A';
    CHECK(tox2::scanRiffEntries(avi).empty());
}

class FakeAtracFactory : public tox2::AtracDecoderFactory {
public:
    std::unique_ptr<tox2::AtracFrameDecoder> create(const tox2::At3Info&, std::string& error) override {
        error = "fake";
        return nullptr;
    }
};

void testAtracFactorySlot() {
    CHECK(tox2::atracDecoderFactory() == nullptr);
    auto factory = std::make_unique<FakeAtracFactory>();
    tox2::AtracDecoderFactory* raw = factory.get();
    tox2::setAtracDecoderFactory(std::move(factory));
    CHECK(tox2::atracDecoderFactory() == raw);
    std::string error;
    CHECK(tox2::atracDecoderFactory()->create(tox2::At3Info{}, error) == nullptr && error == "fake");
    tox2::setAtracDecoderFactory(nullptr);
    CHECK(tox2::atracDecoderFactory() == nullptr);
}

// ---- cellAtrac stream model and handlers ----

constexpr std::uint32_t kAtracUnknownFormat = 0x80610312;
constexpr std::uint32_t kAtracReadsizeOverBuffer = 0x80610311;
constexpr std::uint32_t kAtracNoDecoder = 0x80610321;
constexpr std::uint32_t kAtracUnsetData = 0x80610322;
constexpr std::uint32_t kAtracDecoderWasCreated = 0x80610323;
constexpr std::uint32_t kAtracNodataInBuffer = 0x80610332;
constexpr std::uint32_t kAtracAllDataIsOnMemory = 0x80610341;
constexpr std::uint32_t kAtracAddDataIsTooBig = 0x80610342;
constexpr std::int32_t kRemainOnMemory = -1;
constexpr std::int32_t kRemainStreamEnd = -2;
constexpr std::uint32_t kAt3Block = 512;

// A synthetic ATRAC3plus file: `frames` frames of 512 bytes whose first four bytes hold the frame
// index (big-endian) and whose other bytes follow a pattern of the index, so a wrong or torn frame
// is recognisable. `fact` holds (total, 2048, delay).
Bytes makeAt3File(std::uint32_t channels, std::uint32_t frames, std::uint32_t total, std::uint32_t delay,
                  std::optional<std::pair<std::uint32_t, std::uint32_t>> loop = std::nullopt) {
    std::vector<Bytes> chunks = {chunk("fmt ", fmtBody(channels)), chunk("fact", factBody(total, 2048, delay))};
    if (loop) chunks.push_back(chunk("smpl", smplBody(1, loop->first, loop->second)));
    Bytes file = riffEntry(chunks, frames * kAt3Block);
    for (std::uint32_t f = 0; f < frames; ++f) {
        for (std::uint32_t i = 0; i < kAt3Block; ++i) {
            file.push_back(i < 4 ? static_cast<std::uint8_t>(f >> (24 - 8 * i)) : static_cast<std::uint8_t>(f * 7 + i));
        }
    }
    return file;
}

std::uint32_t at3DataOffset(const Bytes& file) {
    const auto info = tox2::parseAt3Header(file);
    CHECK(info.has_value());
    return info ? info->data_offset : 0;
}

struct FakeDecoderStats {
    std::vector<std::int64_t> events;  // frame index per decoded frame, -1 per reset()
    std::uint64_t bad_frames = 0;
    std::uint32_t resets() const { return static_cast<std::uint32_t>(std::count(events.begin(), events.end(), -1)); }
};

// Output sample j of frame f is f * 2048 + j on the left channel and its negative on the right, so
// the output shows which decoder-output position every sample came from.
class FakeFrameDecoder : public tox2::AtracFrameDecoder {
public:
    FakeFrameDecoder(std::uint32_t channels, FakeDecoderStats* stats) : channels_(channels), stats_(stats) {}
    bool decode(std::span<const std::uint8_t> frame, std::span<float> out, std::string&) override {
        std::uint32_t index = 0;
        for (int i = 0; i < 4 && frame.size() >= 4; ++i) index = (index << 8) | frame[i];
        bool good = frame.size() == kAt3Block && out.size() >= std::size_t{2048} * channels_;
        for (std::uint32_t i = 4; good && i < frame.size(); ++i)
            good = frame[i] == static_cast<std::uint8_t>(index * 7 + i);
        if (!good) ++stats_->bad_frames;
        stats_->events.push_back(index);
        for (std::uint32_t j = 0; j < 2048; ++j) {
            const float value = static_cast<float>(index * 2048 + j);
            out[j * channels_] = value;
            if (channels_ == 2) out[j * channels_ + 1] = -value;
        }
        return true;
    }
    void reset() override { stats_->events.push_back(-1); }

private:
    std::uint32_t channels_;
    FakeDecoderStats* stats_;
};

// A model under test with its guest buffer as a host vector (the model never sees guest memory).
struct ModelRig {
    static constexpr std::uint32_t kBufferAddr = 0x30000000;
    Bytes file;
    Bytes ring;
    FakeDecoderStats stats;
    std::optional<tox2::AtracStream> stream;
    std::uint32_t data_offset = 0;
    std::uint32_t threshold = 0;

    // read_bytes == buffer_bytes == file size is memory mode.
    bool open(Bytes file_bytes, std::uint32_t read_bytes, std::uint32_t buffer_bytes, bool with_decoder = true) {
        file = std::move(file_bytes);
        data_offset = at3DataOffset(file);
        ring.assign(buffer_bytes, 0);
        std::copy_n(file.begin(), std::min<std::size_t>(read_bytes, file.size()), ring.begin());
        std::string error;
        auto opened = tox2::AtracStream::open(std::span<const std::uint8_t>(ring).first(read_bytes), kBufferAddr,
                                              buffer_bytes, error);
        CHECK(opened.has_value());
        if (!opened) return false;
        stream.emplace(std::move(*opened));
        std::unique_ptr<tox2::AtracFrameDecoder> decoder;
        if (with_decoder) decoder = std::make_unique<FakeFrameDecoder>(stream->info().channels, &stats);
        CHECK(stream->createDecoder(std::move(decoder)) == 0);
        const std::int32_t initial = stream->remain();
        threshold = static_cast<std::uint32_t>(initial * 3 / 4);  // as the game computes it
        if (initial < 0) threshold = 0;
        return true;
    }
};

struct Collected {
    std::vector<float> left;
    std::vector<std::uint32_t> call_samples;
    std::vector<std::int32_t> loops_after_call;
    std::vector<std::uint32_t> read_positions;  // every GetStreamDataInfo that led to a read
    std::vector<std::uint32_t> chunks;          // bytes of every committed read
    bool finished = false;
    std::int32_t last_remain = 0;
    bool all_ok = true;
};

// Decode calls with the game's refill rule: a request when 0 <= remain < threshold, at most 0x4000
// bytes, one in flight. The read completes (writes the ring, then AddStreamData) after the next
// Decode call, as the file thread would.
Collected collect(ModelRig& rig, std::uint32_t max_calls, std::function<bool(const Collected&)> stop = {}) {
    Collected got;
    tox2::AtracStream& stream = *rig.stream;
    const std::uint32_t channels = stream.info().channels;
    struct Pending {
        std::uint32_t read_pos, ring_offset, bytes;
    };
    std::optional<Pending> pending;
    for (std::uint32_t call = 0; call < max_calls && !got.finished; ++call) {
        std::vector<float> out(2048 * channels + 16, -12345.0f);
        const auto result = stream.decode(rig.ring, std::span<float>(out).first(2048 * channels));
        if (!result) {
            got.all_ok = false;
            break;
        }
        got.all_ok = got.all_ok && result->samples <= 2048;
        for (std::uint32_t i = 0; i < result->samples; ++i) got.left.push_back(out[i * channels]);
        // Nothing beyond the reported frames may be touched.
        for (std::size_t i = std::size_t{result->samples} * channels; i < out.size(); ++i) {
            got.all_ok = got.all_ok && out[i] == -12345.0f;
        }
        got.call_samples.push_back(result->samples);
        got.loops_after_call.push_back(stream.loopNum());
        got.finished = result->finish;
        got.last_remain = result->remain;

        if (pending) {
            for (std::uint32_t i = 0; i < pending->bytes; ++i) {
                rig.ring[(pending->ring_offset + i) % rig.ring.size()] = rig.file[pending->read_pos + i];
            }
            got.all_ok = got.all_ok && stream.addStreamData(pending->bytes) == 0;
            got.chunks.push_back(pending->bytes);
            pending.reset();
        } else if (!stream.memoryMode() && result->remain >= 0 &&
                   result->remain < static_cast<std::int32_t>(rig.threshold)) {
            const auto info = stream.streamDataInfo();
            got.all_ok = got.all_ok && info.has_value();
            if (info) {
                const std::uint32_t bytes = std::min<std::uint32_t>(info->writable, 0x4000);
                got.all_ok = got.all_ok && info->write_addr >= ModelRig::kBufferAddr &&
                             info->write_addr + info->writable <= ModelRig::kBufferAddr + rig.ring.size();
                if (bytes > 0) {
                    got.read_positions.push_back(info->read_pos);
                    pending = Pending{info->read_pos, info->write_addr - ModelRig::kBufferAddr,
                                      std::min<std::uint32_t>(
                                              bytes, static_cast<std::uint32_t>(rig.file.size()) - info->read_pos)};
                }
            }
        }
        if (stop && stop(got)) break;
    }
    return got;
}

// Positions [first, last] as the floats the fake decoder produces.
void appendPositions(std::vector<float>& expected, std::uint32_t first, std::uint32_t last) {
    for (std::uint32_t p = first; p <= last; ++p) expected.push_back(static_cast<float>(p));
}

void testAtracModelMemoryStereo() {
    // 4 frames, delay 2232, 5000 valid samples: frame 0 is cut entirely, frame 1 starts at the delay and
    // frame 3 ends at the total.
    ModelRig rig;
    const Bytes file = makeAt3File(2, 4, 5000, 2232);
    CHECK(rig.open(file, static_cast<std::uint32_t>(file.size()), static_cast<std::uint32_t>(file.size())));
    CHECK(rig.stream->memoryMode());
    CHECK(rig.stream->frames() == 4);
    CHECK(rig.stream->remain() == kRemainOnMemory);
    CHECK(rig.stream->loopStatus() == 0);
    CHECK(rig.stream->soundInfo().end_sample == 4999);
    CHECK(rig.stream->soundInfo().loop_start == 0 && rig.stream->soundInfo().loop_end == 0);
    CHECK(rig.stream->createDecoder(nullptr) == kAtracDecoderWasCreated);
    const auto info = rig.stream->streamDataInfo();
    CHECK(!info.has_value() && info.error() == kAtracAllDataIsOnMemory);
    CHECK(rig.stream->addStreamData(1) == kAtracAllDataIsOnMemory);

    const Collected got = collect(rig, 20);
    CHECK(got.all_ok && got.finished);
    CHECK((got.call_samples == std::vector<std::uint32_t>{1864, 2048, 1088}));
    std::vector<float> expected;
    appendPositions(expected, 2232, 2232 + 4999);
    CHECK(got.left == expected);
    CHECK(got.last_remain == kRemainOnMemory);
    CHECK(rig.stats.bad_frames == 0 && rig.stats.resets() == 0);
    CHECK((rig.stats.events == std::vector<std::int64_t>{0, 1, 2, 3}));

    // After the end every call repeats "finished" without an error.
    std::vector<float> out(4096);
    const auto again = rig.stream->decode(rig.ring, out);
    CHECK(again.has_value() && again->samples == 0 && again->finish);
}

void testAtracModelMemoryMono() {
    ModelRig rig;
    const Bytes file = makeAt3File(1, 3, 3000, 100);
    CHECK(rig.open(file, static_cast<std::uint32_t>(file.size()), static_cast<std::uint32_t>(file.size())));
    const Collected got = collect(rig, 20);
    CHECK(got.all_ok && got.finished);
    CHECK((got.call_samples == std::vector<std::uint32_t>{1948, 1052}));
    std::vector<float> expected;
    appendPositions(expected, 100, 3099);
    CHECK(got.left == expected);
}

void testAtracModelSilentWithoutDecoder() {
    ModelRig rig;
    const Bytes file = makeAt3File(2, 4, 5000, 2232);
    CHECK(rig.open(file, static_cast<std::uint32_t>(file.size()), static_cast<std::uint32_t>(file.size()), false));
    const Collected got = collect(rig, 20);
    CHECK(got.all_ok && got.finished);
    CHECK((got.call_samples == std::vector<std::uint32_t>{1864, 2048, 1088}));
    CHECK(std::all_of(got.left.begin(), got.left.end(), [](float v) { return v == 0.0f; }));
}

void testAtracModelStreaming() {
    // The game's sizes: 0x10000 bytes read first, a 0x20000 byte ring, refills of at most 0x4000 bytes.
    constexpr std::uint32_t kFrames = 300;
    ModelRig rig;
    const Bytes file = makeAt3File(2, kFrames, kFrames * 2048 - 2232 - 1, 2232);
    CHECK(rig.open(file, 0x10000, 0x20000));
    CHECK(!rig.stream->memoryMode());
    const std::int32_t initial = rig.stream->remain();
    CHECK(initial == static_cast<std::int32_t>((0x10000 - rig.data_offset) / kAt3Block));
    CHECK(rig.threshold == static_cast<std::uint32_t>(initial * 3 / 4));

    const Collected got = collect(rig, 2000);
    CHECK(got.all_ok && got.finished);
    CHECK(got.left.size() == kFrames * 2048 - 2232 - 1);
    std::vector<float> expected;
    appendPositions(expected, 2232, kFrames * 2048 - 2);
    CHECK(got.left == expected);
    CHECK(got.last_remain == kRemainStreamEnd);
    CHECK(rig.stats.bad_frames == 0);
    CHECK(rig.stats.events.size() == kFrames);

    // The refills: file order, never above 0x4000 bytes, the last one short, the whole file read.
    CHECK(!got.read_positions.empty());
    CHECK(std::is_sorted(got.read_positions.begin(), got.read_positions.end()));
    CHECK(got.read_positions.front() == 0x10000);
    std::uint64_t total = 0x10000;
    for (std::uint32_t chunk_bytes : got.chunks) {
        CHECK(chunk_bytes > 0 && chunk_bytes <= 0x4000);
        total += chunk_bytes;
    }
    CHECK(total == file.size());
    CHECK(got.chunks.back() < 0x4000);
    CHECK(file.size() > 0x20000);  // the ring wrapped

    // At least one frame straddled the end of the ring and still decoded intact.
    std::uint32_t straddling = 0;
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        const std::uint32_t at = (rig.data_offset + f * kAt3Block) % 0x20000;
        if (at + kAt3Block > 0x20000) ++straddling;
    }
    CHECK(straddling >= 1);
}

void testAtracModelStarvation() {
    ModelRig rig;
    const Bytes file = makeAt3File(2, 10, 10 * 2048, 0);
    // Two and a half frames are buffered.
    const std::uint32_t read_bytes = at3DataOffset(file) + 2 * kAt3Block + kAt3Block / 2;
    CHECK(rig.open(file, read_bytes, 0x20000));
    CHECK(rig.stream->remain() == 2);

    std::vector<float> out(4096);
    for (int i = 0; i < 2; ++i) {
        const auto result = rig.stream->decode(rig.ring, out);
        CHECK(result.has_value() && result->samples == 2048 && !result->finish && !result->starved);
    }
    for (int i = 0; i < 3; ++i) {
        const auto starved = rig.stream->decode(rig.ring, out);
        CHECK(starved.has_value());
        if (starved) CHECK(starved->samples == 0 && !starved->finish && starved->starved && starved->remain == 0);
    }

    // The rest of frame 2 arrives: decoding resumes where it stopped.
    const auto info = rig.stream->streamDataInfo();
    CHECK(info.has_value() && info->read_pos == read_bytes);
    if (!info) return;
    CHECK(info->writable > 0 && info->write_addr == ModelRig::kBufferAddr + read_bytes);
    for (std::uint32_t i = 0; i < kAt3Block / 2; ++i) rig.ring[read_bytes + i] = file[read_bytes + i];
    CHECK(rig.stream->addStreamData(kAt3Block / 2) == 0);
    const auto resumed = rig.stream->decode(rig.ring, out);
    CHECK(resumed.has_value() && resumed->samples == 2048);
    CHECK(rig.stream->addStreamData(0x20000) == kAtracAddDataIsTooBig);
    CHECK(rig.stats.events.size() == 3 && rig.stats.bad_frames == 0);
}

void testAtracModelLoops() {
    // 6 frames, delay 100, 12000 valid samples, loop [5000, 8999] (start is not the delay).
    const Bytes file = makeAt3File(2, 6, 12000, 100, std::pair{5000u, 8999u});
    const auto size = static_cast<std::uint32_t>(file.size());
    {
        ModelRig rig;
        CHECK(rig.open(file, size, size));
        CHECK(rig.stream->loopStatus() == 1);
        CHECK(rig.stream->soundInfo().loop_start == 4900 && rig.stream->soundInfo().loop_end == 8899);
        CHECK(rig.stream->soundInfo().end_sample == 11999);
        rig.stream->setLoopNum(3);
        const Collected got = collect(rig, 100);
        CHECK(got.all_ok && got.finished);
        std::vector<float> expected;
        appendPositions(expected, 100, 8999);
        appendPositions(expected, 5000, 8999);
        appendPositions(expected, 5000, 12099);
        CHECK(got.left == expected);
        // The count reads 3, 2, 1 over the three passes; a seek goes back one frame before the loop start.
        std::vector<std::int32_t> distinct;
        for (std::int32_t count : got.loops_after_call) {
            if (distinct.empty() || distinct.back() != count) distinct.push_back(count);
        }
        CHECK((distinct == std::vector<std::int32_t>{3, 2, 1}));
        CHECK((rig.stats.events == std::vector<std::int64_t>{0, 1, 2, 3, 4, -1, 1, 2, 3, 4, -1, 1, 2, 3, 4, 5}));
        CHECK(rig.stats.bad_frames == 0);
    }
    for (std::int32_t count : {0, 1}) {
        ModelRig rig;
        CHECK(rig.open(file, size, size));
        rig.stream->setLoopNum(count);
        const Collected got = collect(rig, 100);
        std::vector<float> expected;
        appendPositions(expected, 100, 12099);
        CHECK(got.all_ok && got.finished && got.left == expected);
        CHECK(rig.stats.resets() == 0);
    }
    {
        ModelRig rig;
        CHECK(rig.open(file, size, size));
        rig.stream->setLoopNum(-1);
        const Collected got = collect(rig, 200, [&](const Collected&) { return rig.stats.resets() >= 4; });
        CHECK(got.all_ok && !got.finished);
        CHECK(rig.stats.resets() >= 4);
        CHECK(std::all_of(got.loops_after_call.begin(), got.loops_after_call.end(),
                          [](std::int32_t n) { return n == -1; }));
    }
    {
        // No smpl chunk: no loop status, and a loop count has no effect.
        ModelRig rig;
        const Bytes plain = makeAt3File(2, 6, 12000, 100);
        CHECK(rig.open(plain, static_cast<std::uint32_t>(plain.size()), static_cast<std::uint32_t>(plain.size())));
        CHECK(rig.stream->loopStatus() == 0);
        rig.stream->setLoopNum(5);
        const Collected got = collect(rig, 100);
        CHECK(got.finished && got.left.size() == 12000 && rig.stats.resets() == 0);
    }
}

void testAtracModelStreamingLoop() {
    constexpr std::uint32_t kFrames = 300;
    constexpr std::uint32_t kEnd = kFrames * 2048 - 1;  // exclusive end of the valid audio
    constexpr std::uint32_t kStart = 100 * 2048 + 500;
    ModelRig rig;
    const Bytes file = makeAt3File(2, kFrames, kEnd - 2232, 2232, std::pair{kStart, kEnd - 1});
    CHECK(rig.open(file, 0x10000, 0x20000));
    rig.stream->setLoopNum(2);
    std::vector<std::uint32_t> seek_read_pos;
    std::uint32_t seen_resets = 0;
    // After the seek the game's next request must start at the frame before the loop start.
    const Collected got = collect(rig, 3000, [&](const Collected&) {
        if (rig.stats.resets() > seen_resets) {
            seen_resets = rig.stats.resets();
            const auto info = rig.stream->streamDataInfo();
            CHECK(info.has_value());
            if (info) {
                seek_read_pos.push_back(info->read_pos);
                CHECK(info->write_addr == ModelRig::kBufferAddr + info->read_pos % 0x20000);
                CHECK(info->writable > 0);
            }
        }
        return false;
    });
    CHECK(got.all_ok && got.finished);
    CHECK(seek_read_pos.size() == 1);
    if (!seek_read_pos.empty()) CHECK(seek_read_pos[0] == rig.data_offset + 99 * kAt3Block);
    std::vector<float> expected;
    appendPositions(expected, 2232, kEnd - 1);
    appendPositions(expected, kStart, kEnd - 1);
    CHECK(got.left == expected);
    CHECK(rig.stats.bad_frames == 0);
}

void testAtracModelStaleRead() {
    // A read requested before a loop seek completes after it: its bytes belong to the old position and
    // must not be committed. The file is long enough that data past the loop end is still unread.
    constexpr std::uint32_t kFrames = 600;
    constexpr std::uint32_t kLoopEnd = 200 * 2048 + 100;
    ModelRig rig;
    const Bytes file = makeAt3File(2, kFrames, kFrames * 2048 - 2232 - 1, 2232, std::pair{2048u * 10, kLoopEnd});
    CHECK(rig.open(file, 0x10000, 0x20000));
    rig.stream->setLoopNum(2);
    std::vector<float> out(4096);

    // Play up to frame 199 with every refill completing at once.
    while (rig.stats.events.size() < 200) {
        const auto result = rig.stream->decode(rig.ring, out);
        CHECK(result.has_value());
        if (!result) return;
        const auto info = rig.stream->streamDataInfo();
        if (info && info->writable > 0 && result->remain >= 0 &&
            result->remain < static_cast<std::int32_t>(rig.threshold)) {
            const std::uint32_t bytes = std::min<std::uint32_t>(info->writable, 0x4000);
            for (std::uint32_t i = 0; i < bytes; ++i) {
                rig.ring[(info->write_addr - ModelRig::kBufferAddr + i) % rig.ring.size()] = file[info->read_pos + i];
            }
            CHECK(rig.stream->addStreamData(bytes) == 0);
        }
    }

    // A read is requested and stays in flight while the decoder reaches the loop end and seeks.
    const auto request = rig.stream->streamDataInfo();
    CHECK(request.has_value() && request->writable > 0);
    if (!request) return;
    const std::uint32_t stale_bytes = std::min<std::uint32_t>(request->writable, 0x4000);
    for (int guard = 0; guard < 10 && rig.stats.resets() == 0; ++guard) {
        CHECK(rig.stream->decode(rig.ring, out).has_value());
    }
    CHECK(rig.stats.resets() == 1);

    // The late completion of the old request succeeds but neither moves the read position nor adds
    // frames; the game asks for the position again only afterwards.
    CHECK(rig.stream->addStreamData(stale_bytes) == 0);
    CHECK(rig.stream->remain() == 0);
    const auto fresh = rig.stream->streamDataInfo();
    CHECK(fresh.has_value() && fresh->read_pos == rig.data_offset + 9 * kAt3Block);
}

void testAtracModelOpenErrors() {
    std::string error;
    const Bytes file = makeAt3File(2, 4, 5000, 2232);
    const auto size = static_cast<std::uint32_t>(file.size());
    const auto over = tox2::AtracStream::open(file, 0x30000000, size - 1, error);
    CHECK(!over.has_value() && over.error() == kAtracReadsizeOverBuffer);

    Bytes junk = file;
    junk[0] = 'X';
    const auto bad = tox2::AtracStream::open(junk, 0x30000000, size, error);
    CHECK(!bad.has_value() && bad.error() == kAtracUnknownFormat);
    CHECK(!error.empty());

    // The header ends before the data chunk header: the parser cannot find the data.
    const auto truncated =
            tox2::AtracStream::open(std::span<const std::uint8_t>(file).first(40), 0x30000000, 0x20000, error);
    CHECK(!truncated.has_value() && truncated.error() == kAtracUnknownFormat);

    // No frame in the data chunk.
    const Bytes empty = makeAt3File(2, 0, 0, 0);
    const auto none = tox2::AtracStream::open(empty, 0x30000000, static_cast<std::uint32_t>(empty.size()), error);
    CHECK(!none.has_value() && none.error() == 0x80610315);

    // Decode before CreateDecoder.
    auto opened = tox2::AtracStream::open(file, 0x30000000, size, error);
    CHECK(opened.has_value());
    if (!opened) return;
    std::vector<float> out(4096);
    const auto early = opened->decode(file, out);
    CHECK(!early.has_value() && early.error() == kAtracNoDecoder);

    // No `fact` chunk: the whole decoder output counts.
    Bytes no_fact = riffEntry({chunk("fmt ", fmtBody(2))}, 2 * kAt3Block);
    no_fact.resize(no_fact.size() + 2 * kAt3Block);
    auto plain = tox2::AtracStream::open(no_fact, 0x30000000, static_cast<std::uint32_t>(no_fact.size()), error);
    CHECK(plain.has_value());
    if (plain) CHECK(plain->soundInfo().end_sample == 2 * 2048 - 1);
}

// The handlers on synthetic guest memory.
class AtracGuest {
public:
    static constexpr std::uint32_t kHandle = 0x0;
    static constexpr std::uint32_t kCells = 0x400;
    static constexpr std::uint32_t kBuffer = 0x10000;
    static constexpr std::uint32_t kOut = 0x40000;

    AtracGuest() : caller_(0x01000000, tox2::GuestThreadParams{}, 0, 0) {
        tox2::setLogSink([](std::string_view) {});
        tox2::resetCellAtrac();
        CHECK(tox2::process().memory.reserve());
        base_ = tox2::process().memory.base();
        scratch_ = tox2::process().main.allocate(0x80000, 0x10000);
        CHECK(scratch_ != 0);
    }
    ~AtracGuest() {
        tox2::resetCellAtrac();
        tox2::setLogSink({});
    }

    std::uint8_t* base() const { return base_; }
    std::uint32_t at(std::uint32_t offset) const { return scratch_ + offset; }
    std::uint32_t handle() const { return at(kHandle); }
    std::uint32_t cell(std::uint32_t n) const { return at(kCells + 4 * n); }
    std::uint32_t read(std::uint32_t n) const { return tox2Read32(base_, cell(n)); }

    void put(std::uint32_t addr, const Bytes& bytes) { std::memcpy(base_ + addr, bytes.data(), bytes.size()); }

    std::uint32_t call(std::string_view name, std::uint64_t r3 = 0, std::uint64_t r4 = 0, std::uint64_t r5 = 0,
                       std::uint64_t r6 = 0, std::uint64_t r7 = 0) {
        for (const tox2::HleEntry& entry : tox2::cellAtracHandlers()) {
            if (name != entry.name) continue;
            Tox2PpuContext* ctx = &caller_.context();
            ctx->gpr[3] = r3;
            ctx->gpr[4] = r4;
            ctx->gpr[5] = r5;
            ctx->gpr[6] = r6;
            ctx->gpr[7] = r7;
            entry.handler(ctx, base_);
            return static_cast<std::uint32_t>(ctx->gpr[3]);
        }
        CHECK(false);
        return 0;
    }

    std::uint32_t setData(const Bytes& file, std::uint32_t read_bytes, std::uint32_t buffer_bytes) {
        put(at(kBuffer), file);
        return call("cellAtracSetDataAndGetMemSize", handle(), at(kBuffer), read_bytes, buffer_bytes, cell(0));
    }
    std::uint32_t createDecoder() { return call("cellAtracCreateDecoder", handle(), at(0x800), 0x64, 0x64); }
    // The last argument is a CellAtracExtRes pointer, which the model must not need.
    std::uint32_t createDecoderExt(std::uint32_t ext_res = 0) {
        return call("cellAtracCreateDecoderExt", handle(), at(0x800), 0x64, ext_res);
    }

private:
    tox2::GuestThread caller_;
    std::uint8_t* base_ = nullptr;
    std::uint32_t scratch_ = 0;
};

constexpr std::uint8_t kSentinel = 0x5a;

// Decode until `finish`, checking each call against the silent-output contract.
struct HandlerDecode {
    std::uint32_t total = 0;
    std::uint32_t calls = 0;
    std::uint32_t starved = 0;  // calls answered with NODATA_IN_BUFFER
    bool finished = false;
    std::int32_t last_remain = 0;
    bool ok = true;
};

HandlerDecode decodeAll(AtracGuest& guest, std::uint32_t channels,
                        const std::function<void(std::uint32_t)>& after_call = {}) {
    HandlerDecode got;
    const std::uint32_t out = guest.at(AtracGuest::kOut);
    for (std::uint32_t call = 0; call < 4000 && !got.finished; ++call) {
        std::memset(guest.base() + out, kSentinel, 2048 * channels * 4 + 64);
        const std::uint32_t result =
                guest.call("cellAtracDecode", guest.handle(), out, guest.cell(1), guest.cell(2), guest.cell(3));
        if (result == kAtracNodataInBuffer) {
            // Nothing is written on starvation; the caller waits and retries.
            ++got.starved;
            ++got.calls;
            for (std::uint32_t i = 0; i < 2048 * channels * 4 + 64; ++i) {
                got.ok = got.ok && guest.base()[out + i] == kSentinel;
            }
            if (after_call) after_call(call);
            continue;
        }
        got.ok = got.ok && result == 0;
        const std::uint32_t samples = guest.read(1);
        got.ok = got.ok && samples <= 2048;
        // Silence for the reported frames, nothing written past them.
        for (std::uint32_t i = 0; i < samples * channels * 4; ++i) got.ok = got.ok && guest.base()[out + i] == 0;
        for (std::uint32_t i = samples * channels * 4; i < 2048 * channels * 4 + 64; ++i) {
            got.ok = got.ok && guest.base()[out + i] == kSentinel;
        }
        got.total += samples;
        ++got.calls;
        got.finished = guest.read(2) == 1;
        got.last_remain = static_cast<std::int32_t>(guest.read(3));
        if (after_call) after_call(call);
    }
    return got;
}

void testAtracHandlersMemory() {
    CHECK(tox2::cellAtracHandlers().size() == 18);
    AtracGuest guest;
    std::uint8_t* base = guest.base();
    std::memset(base + guest.handle(), 0xaa, 0x200);

    // Nothing works before SetDataAndGetMemSize.
    CHECK(guest.call("cellAtracGetChannel", guest.handle(), guest.cell(0)) == kAtracUnsetData);
    CHECK(guest.call("cellAtracDecode", guest.handle(), guest.at(AtracGuest::kOut), guest.cell(1), guest.cell(2),
                     guest.cell(3)) == kAtracUnsetData);
    CHECK(guest.call("cellAtracDeleteDecoder", guest.handle()) == kAtracUnsetData);
    CHECK(guest.call("cellAtracIsSecondBufferNeeded", guest.handle()) == kAtracUnsetData);
    CHECK(guest.call("cellAtracGetVacantSize", guest.handle(), guest.cell(0)) == kAtracUnsetData);
    CHECK(guest.call("cellAtracGetInternalErrorInfo", guest.handle(), guest.cell(0)) == kAtracUnsetData);
    CHECK(guest.createDecoderExt() == kAtracUnsetData);

    const Bytes file = makeAt3File(2, 4, 5000, 2232, std::pair{3000u, 7231u});
    const auto size = static_cast<std::uint32_t>(file.size());
    tox2Write32(base, guest.cell(0), 0xdeadbeef);
    CHECK(guest.setData(file, size, size) == 0);
    CHECK(guest.read(0) == 0x1000);

    CHECK(guest.call("cellAtracGetChannel", guest.handle(), guest.cell(1)) == 0 && guest.read(1) == 2);
    CHECK(guest.call("cellAtracGetMaxSample", guest.handle(), guest.cell(1)) == 0 && guest.read(1) == 2048);
    CHECK(guest.call("cellAtracGetLoopInfo", guest.handle(), guest.cell(1), guest.cell(2)) == 0);
    CHECK(guest.read(1) == 0 && guest.read(2) == 1);
    CHECK(guest.call("cellAtracGetRemainFrame", guest.handle(), guest.cell(1)) == 0 &&
          static_cast<std::int32_t>(guest.read(1)) == kRemainOnMemory);
    CHECK(guest.call("cellAtracGetSoundInfo", guest.handle(), guest.cell(1), guest.cell(2), guest.cell(3)) == 0);
    CHECK(guest.read(1) == 4999 && guest.read(2) == 768 && guest.read(3) == 4999);
    CHECK(guest.call("cellAtracIsSecondBufferNeeded", guest.handle()) == 0);
    tox2Write32(base, guest.cell(1), 7);
    tox2Write32(base, guest.cell(2), 7);
    CHECK(guest.call("cellAtracGetSecondBufferInfo", guest.handle(), guest.cell(1), guest.cell(2)) == 0);
    CHECK(guest.read(1) == 0 && guest.read(2) == 0);
    CHECK(guest.call("cellAtracSetSecondBuffer", guest.handle(), guest.at(0x900), 0x100) == 0);

    // Decode needs the decoder; the stream calls do not exist in memory mode.
    CHECK(guest.call("cellAtracDecode", guest.handle(), guest.at(AtracGuest::kOut), guest.cell(1), guest.cell(2),
                     guest.cell(3)) == kAtracNoDecoder);
    CHECK(guest.call("cellAtracGetStreamDataInfo", guest.handle(), guest.cell(1), guest.cell(2), guest.cell(3)) ==
          kAtracAllDataIsOnMemory);
    CHECK(guest.call("cellAtracAddStreamData", guest.handle(), 0x100) == kAtracAllDataIsOnMemory);
    CHECK(guest.createDecoder() == 0);
    CHECK(guest.createDecoder() == kAtracDecoderWasCreated);
    CHECK(guest.createDecoderExt() == kAtracDecoderWasCreated);

    // Nothing can be added in memory mode: the vacant size is 0 and written regardless of the code.
    tox2Write32(base, guest.cell(1), 0xdeadbeef);
    CHECK(guest.call("cellAtracGetVacantSize", guest.handle(), guest.cell(1)) == kAtracAllDataIsOnMemory);
    CHECK(guest.read(1) == 0);
    CHECK(guest.call("cellAtracGetVacantSize", guest.handle(), 0) == 0x8001000d);
    tox2Write32(base, guest.cell(1), 0xdeadbeef);
    CHECK(guest.call("cellAtracGetInternalErrorInfo", guest.handle(), guest.cell(1)) == 0 && guest.read(1) == 0);
    CHECK(guest.call("cellAtracGetInternalErrorInfo", guest.handle(), 0) == 0x8001000d);

    // The loop count: 3, then endless (-1 as a 32-bit register value).
    CHECK(guest.call("cellAtracSetLoopNum", guest.handle(), 3) == 0);
    CHECK(guest.call("cellAtracGetLoopInfo", guest.handle(), guest.cell(1), guest.cell(2)) == 0);
    CHECK(guest.read(1) == 3 && guest.read(2) == 1);
    CHECK(guest.call("cellAtracSetLoopNum", guest.handle(), 0xffffffffu) == 0);
    CHECK(guest.call("cellAtracGetLoopInfo", guest.handle(), guest.cell(1), guest.cell(2)) == 0);
    CHECK(guest.read(1) == 0xffffffffu && guest.read(2) == 1);
    CHECK(guest.call("cellAtracSetLoopNum", guest.handle(), 0) == 0);

    // Decode to the end: 5000 frames of silence in 1864 + 2048 + 1088.
    const HandlerDecode got = decodeAll(guest, 2);
    CHECK(got.ok && got.finished && got.total == 5000 && got.calls == 3);
    CHECK(got.last_remain == kRemainOnMemory);
    CHECK(guest.call("cellAtracDecode", guest.handle(), guest.at(AtracGuest::kOut), guest.cell(1), guest.cell(2),
                     guest.cell(3)) == 0);
    CHECK(guest.read(1) == 0 && guest.read(2) == 1);

    CHECK(guest.call("cellAtracDeleteDecoder", guest.handle()) == 0);
    CHECK(guest.call("cellAtracDeleteDecoder", guest.handle()) == kAtracUnsetData);
    CHECK(guest.call("cellAtracGetChannel", guest.handle(), guest.cell(1)) == kAtracUnsetData);

    // Nothing was ever written into the handle.
    bool untouched = true;
    for (std::uint32_t i = 0; i < 0x200; ++i) untouched = untouched && base[guest.handle() + i] == 0xaa;
    CHECK(untouched);
}

void testAtracHandlersMono() {
    AtracGuest guest;
    const Bytes file = makeAt3File(1, 3, 3000, 100);
    const auto size = static_cast<std::uint32_t>(file.size());
    CHECK(guest.setData(file, size, size) == 0);
    CHECK(guest.call("cellAtracGetChannel", guest.handle(), guest.cell(1)) == 0 && guest.read(1) == 1);
    CHECK(guest.createDecoder() == 0);
    const HandlerDecode got = decodeAll(guest, 1);
    CHECK(got.ok && got.finished && got.total == 3000 && got.calls == 2);
}

void testAtracHandlersStreaming() {
    constexpr std::uint32_t kFrames = 300;
    AtracGuest guest;
    std::uint8_t* base = guest.base();
    const Bytes file = makeAt3File(2, kFrames, kFrames * 2048 - 2232, 2232);
    guest.put(guest.at(AtracGuest::kBuffer), Bytes(file.begin(), file.begin() + 0x10000));
    CHECK(guest.call("cellAtracSetDataAndGetMemSize", guest.handle(), guest.at(AtracGuest::kBuffer), 0x10000, 0x20000,
                     guest.cell(0)) == 0);
    CHECK(guest.createDecoder() == 0);
    CHECK(guest.call("cellAtracGetRemainFrame", guest.handle(), guest.cell(4)) == 0);
    const auto initial = static_cast<std::int32_t>(guest.read(4));
    const std::int32_t threshold = initial * 3 / 4;
    CHECK(initial > 100 && initial < 128);

    // The ring holds 0x10000 - data_offset live bytes, the rest of the 0x20000 is free (the file is larger).
    const std::uint32_t data_offset = at3DataOffset(file);
    CHECK(guest.call("cellAtracGetVacantSize", guest.handle(), guest.cell(8)) == 0);
    CHECK(guest.read(8) == 0x20000 - (0x10000 - data_offset));

    struct Pending {
        std::uint32_t write_ptr, bytes, read_pos;
    };
    std::optional<Pending> pending;
    std::vector<std::uint32_t> read_positions;
    std::uint32_t total_read = 0x10000;
    bool cells_ok = true;
    const std::uint32_t ring = guest.at(AtracGuest::kBuffer);
    const HandlerDecode got = decodeAll(guest, 2, [&](std::uint32_t) {
        const auto remain = static_cast<std::int32_t>(guest.read(3));
        if (pending) {
            std::memcpy(base + pending->write_ptr, file.data() + pending->read_pos, pending->bytes);
            cells_ok = cells_ok && guest.call("cellAtracAddStreamData", guest.handle(), pending->bytes) == 0;
            total_read += pending->bytes;
            pending.reset();
        } else if (remain >= 0 && remain < threshold) {
            cells_ok = cells_ok && guest.call("cellAtracGetStreamDataInfo", guest.handle(), guest.cell(5),
                                              guest.cell(6), guest.cell(7)) == 0;
            const std::uint32_t write_ptr = guest.read(5), writable = guest.read(6), read_pos = guest.read(7);
            cells_ok = cells_ok && write_ptr >= ring && write_ptr + writable <= ring + 0x20000;
            // The contiguous run never exceeds the total free space.
            cells_ok = cells_ok && guest.call("cellAtracGetVacantSize", guest.handle(), guest.cell(8)) == 0 &&
                       writable <= guest.read(8) && guest.read(8) <= 0x20000;
            const std::uint32_t bytes =
                    std::min({writable, 0x4000u, static_cast<std::uint32_t>(file.size()) - read_pos});
            if (bytes > 0) {
                read_positions.push_back(read_pos);
                pending = Pending{write_ptr, bytes, read_pos};
            }
        }
    });
    CHECK(got.ok && got.finished && cells_ok);
    CHECK(got.total == kFrames * 2048 - 2232);
    CHECK(got.last_remain == kRemainStreamEnd);
    CHECK(total_read == file.size());
    CHECK(!read_positions.empty() && std::is_sorted(read_positions.begin(), read_positions.end()));

    // The whole file is buffered: nothing more can be committed.
    CHECK(guest.call("cellAtracGetStreamDataInfo", guest.handle(), guest.cell(5), guest.cell(6), guest.cell(7)) == 0);
    CHECK(guest.read(6) == 0 && guest.read(7) == file.size());
    CHECK(guest.call("cellAtracAddStreamData", guest.handle(), 1) == kAtracAddDataIsTooBig);
    tox2Write32(base, guest.cell(8), 0xdeadbeef);
    CHECK(guest.call("cellAtracGetVacantSize", guest.handle(), guest.cell(8)) == 0 && guest.read(8) == 0);
}

// Starvation, as the game's decode thread sees it: NODATA_IN_BUFFER with nothing written, then success
// once the missing bytes are added. The ring (0x800) is smaller than the file, so the free space is
// limited by the ring.
void testAtracHandlersStarvation() {
    AtracGuest guest;
    std::uint8_t* base = guest.base();
    const Bytes file = makeAt3File(2, 10, 10 * 2048, 0);
    const std::uint32_t data_offset = at3DataOffset(file);
    constexpr std::uint32_t kRing = 0x800;
    const std::uint32_t read_bytes = data_offset + 2 * kAt3Block + kAt3Block / 2;  // two and a half frames
    CHECK(read_bytes < kRing);
    guest.put(guest.at(AtracGuest::kBuffer), Bytes(file.begin(), file.begin() + read_bytes));
    CHECK(guest.call("cellAtracSetDataAndGetMemSize", guest.handle(), guest.at(AtracGuest::kBuffer), read_bytes, kRing,
                     guest.cell(0)) == 0);
    CHECK(guest.createDecoderExt() == 0);

    const std::uint32_t out = guest.at(AtracGuest::kOut);
    auto decodeOnce = [&] {
        std::memset(base + out, kSentinel, 2048 * 2 * 4 + 64);
        for (std::uint32_t n = 1; n <= 3; ++n) tox2Write32(base, guest.cell(n), 0xdeadbeef);
        return guest.call("cellAtracDecode", guest.handle(), out, guest.cell(1), guest.cell(2), guest.cell(3));
    };
    for (int i = 0; i < 2; ++i) CHECK(decodeOnce() == 0 && guest.read(1) == 2048 && guest.read(2) == 0);

    // Half a frame is left in the ring: every further call fails the same way and writes nothing.
    for (int i = 0; i < 3; ++i) {
        CHECK(decodeOnce() == kAtracNodataInBuffer);
        bool untouched = guest.read(1) == 0xdeadbeef && guest.read(2) == 0xdeadbeef && guest.read(3) == 0xdeadbeef;
        for (std::uint32_t b = 0; b < 2048 * 2 * 4 + 64; ++b) untouched = untouched && base[out + b] == kSentinel;
        CHECK(untouched);
    }
    CHECK(tox2::cellAtracStats().starved == 3);

    CHECK(guest.call("cellAtracGetVacantSize", guest.handle(), guest.cell(4)) == 0);
    CHECK(guest.read(4) == kRing - kAt3Block / 2);  // the ring is the limit
    CHECK(guest.call("cellAtracGetInternalErrorInfo", guest.handle(), guest.cell(5)) == 0 && guest.read(5) == 0);

    // The rest of frame 2, possibly in two runs when the ring wraps.
    std::uint32_t pos = read_bytes;
    while (pos < data_offset + 3 * kAt3Block) {
        CHECK(guest.call("cellAtracGetStreamDataInfo", guest.handle(), guest.cell(5), guest.cell(6), guest.cell(7)) ==
              0);
        CHECK(guest.read(7) == pos);
        const std::uint32_t n = std::min(guest.read(6), data_offset + 3 * kAt3Block - pos);
        CHECK(n > 0);
        if (n == 0) break;
        std::memcpy(base + guest.read(5), file.data() + pos, n);
        CHECK(guest.call("cellAtracAddStreamData", guest.handle(), n) == 0);
        pos += n;
    }
    CHECK(decodeOnce() == 0 && guest.read(1) == 2048 && guest.read(2) == 0);

    // All three frames are consumed: the ring is empty again and the file has bytes left.
    CHECK(guest.call("cellAtracGetVacantSize", guest.handle(), guest.cell(4)) == 0 && guest.read(4) == kRing);
    CHECK(decodeOnce() == kAtracNodataInBuffer);
}

void testAtracHandlersReuseAndErrors() {
    AtracGuest guest;
    const Bytes stereo = makeAt3File(2, 4, 5000, 2232, std::pair{3000u, 7231u});
    const Bytes mono = makeAt3File(1, 3, 3000, 100);
    CHECK(guest.setData(stereo, static_cast<std::uint32_t>(stereo.size()), static_cast<std::uint32_t>(stereo.size())) ==
          0);
    CHECK(guest.createDecoder() == 0);
    CHECK(guest.call("cellAtracGetLoopInfo", guest.handle(), guest.cell(1), guest.cell(2)) == 0 && guest.read(2) == 1);

    // The same address with another file, with and without DeleteDecoder in between: a fresh state.
    for (bool deleted : {true, false}) {
        if (deleted) CHECK(guest.call("cellAtracDeleteDecoder", guest.handle()) == 0);
        CHECK(guest.setData(mono, static_cast<std::uint32_t>(mono.size()), static_cast<std::uint32_t>(mono.size())) ==
              0);
        CHECK(guest.call("cellAtracGetChannel", guest.handle(), guest.cell(1)) == 0 && guest.read(1) == 1);
        CHECK(guest.call("cellAtracGetLoopInfo", guest.handle(), guest.cell(1), guest.cell(2)) == 0);
        CHECK(guest.read(1) == 0 && guest.read(2) == 0);
        CHECK(guest.call("cellAtracGetSoundInfo", guest.handle(), guest.cell(1), guest.cell(2), guest.cell(3)) == 0);
        CHECK(guest.read(1) == 2999);
        CHECK(guest.createDecoder() == 0);  // the decoder of the old state is gone too
        const HandlerDecode got = decodeAll(guest, 1);
        CHECK(got.ok && got.finished && got.total == 3000);
        if (!deleted) CHECK(guest.call("cellAtracDeleteDecoder", guest.handle()) == 0);
    }

    // A bad header leaves the handle without data; the outputs are not written.
    Bytes junk = stereo;
    junk[1] = 'X';
    tox2Write32(guest.base(), guest.cell(0), 0x1234);
    CHECK(guest.setData(junk, static_cast<std::uint32_t>(junk.size()), static_cast<std::uint32_t>(junk.size())) ==
          kAtracUnknownFormat);
    CHECK(guest.read(0) == 0x1234);
    CHECK(guest.call("cellAtracGetChannel", guest.handle(), guest.cell(1)) == kAtracUnsetData);
    CHECK(guest.setData(stereo, static_cast<std::uint32_t>(stereo.size()),
                        static_cast<std::uint32_t>(stereo.size()) - 1) == kAtracReadsizeOverBuffer);
    CHECK(guest.call("cellAtracGetChannel", guest.handle(), guest.cell(1)) == kAtracUnsetData);

    // An unmapped output pointer is a fault code, not a crash.
    CHECK(guest.setData(stereo, static_cast<std::uint32_t>(stereo.size()), static_cast<std::uint32_t>(stereo.size())) ==
          0);
    CHECK(guest.call("cellAtracGetChannel", guest.handle(), 0) == 0x8001000d);
    CHECK(guest.call("cellAtracSetDataAndGetMemSize", 0, guest.at(AtracGuest::kBuffer), 100, 100, guest.cell(0)) ==
          0x8001000d);
}

// A decoder that fills every sample with one value and rejects the frame whose index is `bad_frame`.
class ConstantDecoder : public tox2::AtracFrameDecoder {
public:
    ConstantDecoder(float value, std::uint32_t bad_frame) : value_(value), bad_frame_(bad_frame) {}
    bool decode(std::span<const std::uint8_t> frame, std::span<float> out, std::string& error) override {
        const std::uint32_t index = frame.size() >= 4 ? (std::uint32_t{frame[2]} << 8) | frame[3] : 0;
        if (index == bad_frame_) {
            error = "bad frame";
            return false;
        }
        std::fill(out.begin(), out.end(), value_);
        return true;
    }
    void reset() override {}

private:
    float value_;
    std::uint32_t bad_frame_;
};

class ConstantFactory : public tox2::AtracDecoderFactory {
public:
    ConstantFactory(float value, std::uint32_t bad_frame) : value_(value), bad_frame_(bad_frame) {}
    std::unique_ptr<tox2::AtracFrameDecoder> create(const tox2::At3Info&, std::string&) override {
        return std::make_unique<ConstantDecoder>(value_, bad_frame_);
    }

private:
    float value_;
    std::uint32_t bad_frame_;
};

// Decode to the end and check that every reported sample is the big-endian bytes of `value`, or zero
// for the frame the decoder rejects (which is 2048 samples long and not cut by the delay).
void testAtracHandlersDecodeThroughFactory() {
    // 0.5f is 0x3f000000, so a byte-order mistake shows as a different first byte.
    constexpr std::uint8_t kHalf[4] = {0x3f, 0x00, 0x00, 0x00};
    AtracGuest guest;
    tox2::setAtracDecoderFactory(std::make_unique<ConstantFactory>(0.5f, 2));
    const Bytes file = makeAt3File(2, 4, 5000, 2232);
    const auto size = static_cast<std::uint32_t>(file.size());
    CHECK(guest.setData(file, size, size) == 0);
    CHECK(guest.createDecoder() == 0);

    const std::uint32_t out = guest.at(AtracGuest::kOut);
    const std::uint32_t expected_samples[] = {1864, 2048, 1088};
    bool bytes_ok = true;
    for (int call = 0; call < 3; ++call) {
        std::memset(guest.base() + out, kSentinel, 2048 * 2 * 4 + 64);
        CHECK(guest.call("cellAtracDecode", guest.handle(), out, guest.cell(1), guest.cell(2), guest.cell(3)) == 0);
        const std::uint32_t samples = guest.read(1);
        CHECK(samples == expected_samples[call]);
        for (std::uint32_t i = 0; i < samples * 2; ++i) {
            const std::uint8_t* word = guest.base() + out + 4 * i;
            // The second call is the rejected frame: silence.
            const bool match = call == 1 ? (word[0] == 0 && word[1] == 0 && word[2] == 0 && word[3] == 0)
                                         : std::memcmp(word, kHalf, 4) == 0;
            bytes_ok = bytes_ok && match;
        }
        for (std::uint32_t i = samples * 2 * 4; i < 2048 * 2 * 4 + 64; ++i) {
            bytes_ok = bytes_ok && guest.base()[out + i] == kSentinel;
        }
        CHECK(guest.read(2) == (call == 2 ? 1u : 0u));
    }
    CHECK(bytes_ok);

    // Frames 0, 1 and 3 decode (the cut first frame included), frame 2 is rejected.
    tox2::CellAtracStats stats = tox2::cellAtracStats();
    CHECK(stats.decoders == 1 && stats.frames == 3 && stats.non_silent == 3 && stats.errors == 1);
    CHECK(stats.without_decoder == 0 && stats.starved == 0);

    // A factory that cannot make a decoder leaves the handle usable: silence, counted as such.
    tox2::setAtracDecoderFactory(std::make_unique<FakeAtracFactory>());
    CHECK(guest.call("cellAtracDeleteDecoder", guest.handle()) == 0);
    CHECK(guest.setData(file, size, size) == 0);
    CHECK(guest.createDecoder() == 0);
    const HandlerDecode silent = decodeAll(guest, 2);
    CHECK(silent.ok && silent.finished && silent.total == 5000);
    stats = tox2::cellAtracStats();
    CHECK(stats.decoders == 2 && stats.frames == 3 && stats.without_decoder == 4);
    tox2::setAtracDecoderFactory(nullptr);
}

// CreateDecoderExt attaches a working decoder without looking at its SPURS argument.
void testAtracHandlersCreateDecoderExt() {
    constexpr std::uint8_t kHalf[4] = {0x3f, 0x00, 0x00, 0x00};
    AtracGuest guest;
    tox2::setAtracDecoderFactory(std::make_unique<ConstantFactory>(0.5f, 999));
    const Bytes file = makeAt3File(2, 2, 4096, 0);
    const auto size = static_cast<std::uint32_t>(file.size());
    CHECK(guest.setData(file, size, size) == 0);
    // An unmapped CellAtracExtRes pointer must not matter.
    CHECK(guest.call("cellAtracCreateDecoderExt", guest.handle(), 0, 0, 0xfffffff0) == 0);
    CHECK(guest.createDecoderExt(guest.at(0x900)) == kAtracDecoderWasCreated);

    const std::uint32_t out = guest.at(AtracGuest::kOut);
    CHECK(guest.call("cellAtracDecode", guest.handle(), out, guest.cell(1), guest.cell(2), guest.cell(3)) == 0);
    CHECK(guest.read(1) == 2048 && std::memcmp(guest.base() + out, kHalf, 4) == 0);
    const tox2::CellAtracStats stats = tox2::cellAtracStats();
    CHECK(stats.decoders == 1 && stats.frames == 1 && stats.non_silent == 1);
    tox2::setAtracDecoderFactory(nullptr);
}

// A loop that ends before the valid audio starts is ignored, so it must not be advertised either.
void testAtracLoopOutsideValidAudio() {
    ModelRig rig;
    const Bytes file = makeAt3File(2, 4, 5000, 2232, std::pair{0u, 100u});
    CHECK(rig.open(file, static_cast<std::uint32_t>(file.size()), static_cast<std::uint32_t>(file.size())));
    CHECK(rig.stream->loopStatus() == 0);
    rig.stream->setLoopNum(3);
    const Collected got = collect(rig, 20);
    CHECK(got.all_ok && got.finished && got.left.size() == 5000 && rig.stats.resets() == 0);

    ModelRig inside;
    const Bytes usable = makeAt3File(2, 4, 5000, 2232, std::pair{3000u, 7231u});
    CHECK(inside.open(usable, static_cast<std::uint32_t>(usable.size()), static_cast<std::uint32_t>(usable.size())));
    CHECK(inside.stream->loopStatus() == 1);
}

}  // namespace

int main(int argc, char** argv) {
    const bool keep = argc > 1;
    std::error_code ec;
    if (keep) {
        g_dir = argv[1];
    } else {
        g_dir = fs::temp_directory_path() /
                std::format("tox2_audio_core_{}", std::chrono::steady_clock::now().time_since_epoch().count());
    }
    fs::create_directories(g_dir, ec);
    if (ec) {
        std::fprintf(stderr, "cannot create %s\n", g_dir.string().c_str());
        return 1;
    }

    testRingPushPop();
    testRingWrap();
    testRingDrop();
    testRingThreaded();
    testNullOutput();
    testWavFloatRoundTrip();
    testWavHeaderValidWithoutFinish();
    testWavWriterErrors();
    testWavReadPcm16();
    testWavReadRejects();
    testWavStatsTone();
    testWavStatsSilence();
    testWavStatsFile();
    testBlockClock();
    testBlockClockCorrection();
    testRateControllerFollowsDeviceDrift();
    testEngineBlocksAndDump();
    testEngineSkipsLatePeriods();
    testEngineStopDoesNotWait();
    testEngineOutputOpenFails();
    testEngineFinishDumpDuringOpen();
    testEngineResumesAfterStop();
    testEngineFollowsReportedFill();
    testCellAudioStateErrors();
    testCellAudioPortOpenAndConfig();
    testCellAudioTagsAndTimestamps();
    testCellAudioMixing();
    testCellAudioPortLevel();
    testCellAudioNotify();
    testCellAudioEngine();
    testAt3ParseStereoWithLoop();
    testAt3ParseMonoWithoutLoop();
    testAt3ParseOptionalChunks();
    testAt3ParseRejects();
    testAt3ParseTruncated();
    testScanRiffEntries();
    testAtracFactorySlot();
    testAtracModelMemoryStereo();
    testAtracModelMemoryMono();
    testAtracModelSilentWithoutDecoder();
    testAtracModelStreaming();
    testAtracModelStarvation();
    testAtracModelLoops();
    testAtracModelStreamingLoop();
    testAtracModelStaleRead();
    testAtracModelOpenErrors();
    testAtracHandlersMemory();
    testAtracHandlersMono();
    testAtracHandlersStreaming();
    testAtracHandlersStarvation();
    testAtracHandlersReuseAndErrors();
    testAtracHandlersDecodeThroughFactory();
    testAtracHandlersCreateDecoderExt();
    testAtracLoopOutsideValidAudio();

    if (!keep) fs::remove_all(g_dir, ec);
    std::printf("audio core tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}
