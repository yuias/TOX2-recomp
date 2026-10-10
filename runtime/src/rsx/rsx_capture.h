#pragma once

// Capture of one RSX frame as inputs (register writes, the guest bytes the frame builder read, and the
// state the frame started from), so that a decoder or builder fix can be tested on an old capture.
// The file is little-endian with the `.tox2cap` extension:
//   header  "TX2RSXC1", u32 version (2), u64 flip index of the flip command that ends the frame
//   chunks  {u32 tag, u32 size, bytes}: REGS, DISP, SURF, then in stream order WRIT / MEMR / FLIP, END
// A capture holds the writes after the previous flip command up to and including the next one. The
// start registers are those the previous flip left behind: after its context reset and after the flip
// method write itself.

#include <array>
#include <bitset>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "rsx/rsx_frame.h"
#include "rsx/rsx_frame_builder.h"
#include "rsx/rsx_registers.h"
#include "rsx/rsx_state.h"

namespace tox2 {

// The REGS chunk size follows kRsxVpInstructionCount and kRsxVpConstantCount: changing either needs a
// version bump, or old captures would be rejected as malformed. The writer writes this version; the
// reader also accepts version 1, whose REGS chunk lacks the vertex default written mask.
inline constexpr std::uint32_t kRsxCaptureVersion = 2;

// The file image in three parts, so the bulk of it (the recorded stream) is moved rather than copied
// while the guest lock is held.
struct RsxCaptureFile {
    std::vector<std::uint8_t> head;    // header, REGS, DISP, SURF
    std::vector<std::uint8_t> stream;  // WRIT / MEMR / FLIP chunks
    std::vector<std::uint8_t> tail;    // END
    std::size_t size() const { return head.size() + stream.size() + tail.size(); }
    std::vector<std::uint8_t> flatten() const;
};

class RsxCaptureWriter {
public:
    // `flip_index` is the zero-based index of the flip command that ends the frame (the builder's
    // RsxFlip::flip_index); replay seeds the builder with it. `regs` and `surfaces` are the state at the
    // start of the frame.
    RsxCaptureWriter(std::uint64_t flip_index, const RsxRegisters& regs, std::span<const RsxSurface> surfaces);

    // A completed write; call after the frame builder handled it, so the memory it read comes first.
    void onWrite(std::uint32_t method, std::uint32_t value);
    // Guest bytes the frame builder read, in the order it read them.
    void onMemory(std::uint32_t location, std::uint32_t offset, std::span<const std::uint8_t> bytes);
    void onFlip(std::uint32_t buffer);

    // Ends the stream and returns the file image. The display buffers are those the flip command used.
    RsxCaptureFile finish(const std::array<RsxDisplayBuffer, kRsxDisplayBufferCount>& display,
                          std::uint32_t display_count);

    std::uint64_t writeCount() const { return writes_; }
    std::uint64_t rangeCount() const { return ranges_; }

private:
    void flushWrites();

    std::vector<std::uint8_t> head_;      // header, REGS
    std::vector<std::uint8_t> surfaces_;  // the SURF chunk
    std::vector<std::uint8_t> stream_;    // WRIT / MEMR / FLIP chunks so far
    std::vector<std::uint8_t> pending_;   // method/value pairs not yet flushed into a WRIT chunk
    std::uint64_t writes_ = 0, ranges_ = 0;
};

// Passes reads through and reports the bytes of successful ones to the active writer.
class RsxRecordingMemoryView final : public RsxMemoryView {
public:
    explicit RsxRecordingMemoryView(RsxMemoryView& inner) : inner_(inner) {}
    void setWriter(RsxCaptureWriter* writer) { writer_ = writer; }
    bool read(std::uint32_t location, std::uint32_t offset, std::span<std::uint8_t> out) override;

private:
    RsxMemoryView& inner_;
    RsxCaptureWriter* writer_ = nullptr;
};

class RsxCaptureReader {
public:
    enum class EventKind : std::uint8_t { Write, Memory, Flip };
    struct Event {
        EventKind kind = EventKind::Write;
        // Write: method/value. Memory: location/offset and `size` bytes at `data_pos`. Flip: buffer in `a`.
        std::uint32_t a = 0, b = 0, size = 0;
        std::size_t data_pos = 0;
    };

    // Validates tags and sizes; false with a reason in `error` for anything malformed or truncated.
    bool parse(std::vector<std::uint8_t> bytes, std::string& error);
    bool load(const std::filesystem::path& path, std::string& error);

    std::uint64_t flipIndex() const { return flip_index_; }
    const RsxRegisters& startRegisters() const { return *regs_; }
    const std::array<RsxDisplayBuffer, kRsxDisplayBufferCount>& display() const { return display_; }
    std::uint32_t displayCount() const { return display_count_; }
    const std::vector<RsxSurface>& surfaces() const { return surfaces_; }
    const std::vector<Event>& events() const { return events_; }
    std::span<const std::uint8_t> memoryBytes(const Event& event) const {
        return std::span<const std::uint8_t>(data_).subspan(event.data_pos, event.size);
    }
    std::uint64_t writeCount() const { return writes_; }
    std::uint64_t rangeCount() const { return ranges_; }

private:
    std::vector<std::uint8_t> data_;
    std::uint64_t flip_index_ = 0;
    std::unique_ptr<RsxRegisters> regs_;
    std::array<RsxDisplayBuffer, kRsxDisplayBufferCount> display_{};
    std::uint32_t display_count_ = 0;
    std::vector<RsxSurface> surfaces_;
    std::vector<Event> events_;
    std::uint64_t writes_ = 0, ranges_ = 0;
};

// Sparse image of the bytes the capture recorded. A read of anything else fails and is counted as
// `capture: missing range`.
class ReplayRsxMemoryView final : public RsxMemoryView {
public:
    // Later bytes replace earlier ones, like the live memory they were read from.
    void add(std::uint32_t location, std::uint32_t offset, std::span<const std::uint8_t> bytes);
    bool read(std::uint32_t location, std::uint32_t offset, std::span<std::uint8_t> out) override;

private:
    static constexpr std::uint32_t kPageSize = 4096;
    struct Page {
        std::array<std::uint8_t, kPageSize> data{};
        std::bitset<kPageSize> valid;
        std::uint32_t valid_count = 0;
    };
    std::array<std::unordered_map<std::uint32_t, std::unique_ptr<Page>>, 2> pages_;
};

// Feeds the capture to `builder` (which must read through `memory`) in the order the live consumer
// used: each write is applied to the registers and then handed to the builder; a flip is handed to the
// builder and then resets the context registers. The builder's flip index is seeded from the capture so
// replayed flips carry the live index. The commands stay in the builder.
void replayCapture(const RsxCaptureReader& reader, ReplayRsxMemoryView& memory, RsxFrameBuilder& builder);

// Creates the parent directory and writes the file; false with a reason in `error`.
bool writeCaptureFile(const std::filesystem::path& path, const RsxCaptureFile& file, std::string& error);

}  // namespace tox2
