#pragma once

// PNG output of rendered frames and the image comparison used by `tox2_rsxreplay --compare`.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace tox2 {

// Tightly packed 8-bit RGB rows, top row first.
struct RgbImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgb;
};

// `bgra` holds width * height * 4 bytes (B, G, R, A per pixel); alpha is dropped.
RgbImage rgbFromBgra(std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height);

// Writes an RGB8 PNG (zlib level 1) and creates the parent directory. False with a reason in `error`.
bool writePngRgb(const std::filesystem::path& path, const RgbImage& image, std::string& error);

// Reads any PNG as RGB8 (palette, gray, 16-bit and alpha are converted or dropped).
bool readPngRgb(const std::filesystem::path& path, RgbImage& image, std::string& error);

struct ImageDiff {
    std::uint32_t max = 0;           // largest absolute per-channel difference
    double mean = 0.0;               // mean absolute per-channel difference
    double differing_percent = 0.0;  // share of pixels with any channel differing by more than 2
};

// False with a reason in `error` when the sizes differ.
bool diffImages(const RgbImage& a, const RgbImage& b, ImageDiff& diff, std::string& error);

// Averages each factor_x x factor_y block of `in` (rounded to nearest) into `out`. False with a reason when a
// factor is 0 or the size is not a multiple of the factors.
bool downsampleBox(const RgbImage& in, std::uint32_t factor_x, std::uint32_t factor_y, RgbImage& out,
                   std::string& error);
// The same on readDisplay's BGRA8 rows, all four channels: the render tests compare alpha too.
bool downsampleBoxBgra(std::span<const std::uint8_t> in, std::uint32_t width, std::uint32_t height,
                       std::uint32_t factor_x, std::uint32_t factor_y, std::vector<std::uint8_t>& out,
                       std::string& error);

// Copies the width x height rectangle at (x, y) of `in` into `out`. False with a reason when the rectangle is empty or
// not inside `in`.
bool cropRgb(const RgbImage& in, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height,
             RgbImage& out, std::string& error);

// One display readback waiting to become `path`.
struct DumpFrame {
    std::filesystem::path path;
    std::vector<std::uint8_t> bgra;  // width * height * 4 bytes, see rgbFromBgra
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct DumpCounts {
    std::uint64_t written = 0;
    std::uint64_t failed = 0;
    std::uint64_t skipped = 0;  // dropped because the queue was full
};

// Converts, encodes and writes dumped frames on one worker thread, so the producer (the RSX thread) only
// pays for the readback. The queue is bounded and never blocks the producer: a frame that does not fit is
// dropped and counted. Single producer; counts(), flush() and the destructor are safe from any thread.
// Each PNG is written to `<name>.png.tmp` and renamed, so a process exit during a write can leave a stray
// `frame_*.png.tmp`, never a truncated `.png`.
class FrameDumper {
public:
    // `capacity` is the number of frames that may wait; the one being encoded is not counted.
    explicit FrameDumper(std::size_t capacity = 4);
    // Writes what is queued, then stops the worker.
    ~FrameDumper();
    FrameDumper(const FrameDumper&) = delete;
    FrameDumper& operator=(const FrameDumper&) = delete;

    // A buffer for DumpFrame::bgra; recycled from finished frames, so the steady state allocates nothing.
    std::vector<std::uint8_t> takeBuffer();
    // Hands back a buffer from takeBuffer() that was not submitted.
    void recycleBuffer(std::vector<std::uint8_t>&& buffer);
    // Lets the producer skip the readback when submit() would drop the frame anyway.
    bool hasRoom() const;
    void countSkipped() { skipped_.fetch_add(1, std::memory_order_relaxed); }
    // Takes the frame; false (counted as skipped, `frame` untouched) when the queue is full.
    bool submit(DumpFrame&& frame);

    // Waits until every frame submitted before the call is written or has failed; frames submitted later
    // do not extend the wait. Returns how many of those frames were still pending when `timeout` passed
    // (0 means all done).
    std::uint64_t flush(std::chrono::milliseconds timeout);
    DumpCounts counts() const;

private:
    void run();

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable idle_cv_;
    std::deque<DumpFrame> queue_;
    std::vector<std::vector<std::uint8_t>> free_buffers_;
    std::uint64_t submitted_ = 0;  // frames accepted by submit()
    std::uint64_t completed_ = 0;  // frames the worker finished, written or failed; frames finish in order
    bool stop_ = false;
    std::thread worker_;  // started by the first submit()
    std::atomic<std::uint64_t> written_{0};
    std::atomic<std::uint64_t> failed_{0};
    std::atomic<std::uint64_t> skipped_{0};
};

}  // namespace tox2
