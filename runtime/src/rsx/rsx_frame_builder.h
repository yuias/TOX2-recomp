#pragma once

// Turns the stream of completed RSX register writes into plain commands (clear, draw, flip) that
// carry every guest byte they need, so a renderer never touches guest memory. Draw boundaries,
// vertex and index addressing follow RPCS3 `Emu/RSX/NV47/HW/nv4097.cpp` (`set_begin_end`, `draw_arrays`,
// `draw_index_array`), `NV47/FW/draw_call.hpp` (`draw_clause::append`), `Core/RSXDrawCommands.cpp`,
// `Utils/rsx_utils.h` and `VK/VKVertexBuffers.cpp`; clears follow `VK/VKGSRender.cpp` `clear_surface`.

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rsx/rsx_frame.h"
#include "rsx/rsx_registers.h"
#include "rsx/rsx_state.h"
#include "rsx/rsx_texture_cache.h"

namespace tox2 {

// RSX-visible memory; location 0 = local (offset from kRsxLocalBase), 1 = main memory through the IO map.
class RsxMemoryView {
public:
    virtual ~RsxMemoryView() = default;
    // Copies raw (big-endian) guest bytes; false when any part of the range is not readable.
    virtual bool read(std::uint32_t location, std::uint32_t offset, std::span<std::uint8_t> out) = 0;
    // Like read(), for a read whose failure the caller handles (it falls back to a careful path): a view that counts
    // failed reads, such as the replay view's `capture: missing range`, does not count this one.
    virtual bool tryRead(std::uint32_t location, std::uint32_t offset, std::span<std::uint8_t> out) {
        return read(location, offset, out);
    }
};

// Live view over guest memory. The RSX thread reads through it without the kernel lock: like the
// hardware, it relies on the game not freeing or unmapping memory a submitted draw still reads.
class GuestRsxMemoryView final : public RsxMemoryView {
public:
    explicit GuestRsxMemoryView(const std::uint8_t* base) : base_(base) {}
    bool read(std::uint32_t location, std::uint32_t offset, std::span<std::uint8_t> out) override;

private:
    const std::uint8_t* base_;
};

// Upper bounds for what one command may copy; a draw or texture beyond them is dropped and counted.
inline constexpr std::uint32_t kRsxMaxVertexBytes = 64u << 20;
inline constexpr std::uint32_t kRsxMaxIndexCount = 1u << 20;
inline constexpr std::uint32_t kRsxMaxTextureBytes = 64u << 20;
inline constexpr std::size_t kRsxMaxKnownSurfaces = 64;

// Counters for measuring the builder. Times and the union figure are taken only while timing is enabled.
struct RsxBuilderStats {
    std::uint64_t build_draw_calls = 0;  // buildDraw runs (including draws it drops)
    std::uint64_t build_draw_ns = 0;     // wall time inside buildDraw
    std::uint64_t fp_cache_hits = 0, fp_cache_misses = 0;
    std::uint64_t vp_cache_hits = 0, vp_cache_misses = 0;
    std::uint64_t vertex_bytes_copied = 0;  // guest vertex bytes fillVertices copied
    std::uint64_t vertex_bytes_union = 0;   // bytes of the union of those ranges per location
};

class RsxFrameBuilder {
public:
    explicit RsxFrameBuilder(RsxMemoryView& memory) : memory_(memory) {}

    // Called after RsxRegisters::apply for every completed write.
    void onWrite(const RsxRegisters& regs, std::uint32_t method, std::uint32_t value);
    void onFlip(const RsxRegisters& regs, std::uint32_t buffer, const RsxDisplayBuffer& display);

    // Moves out what was built since the last call.
    std::vector<RsxCommand> takeCommands();

    // Gives back commands the caller has finished with (after execution), so the builder may reuse their buffers:
    // the vectors of up to kRecycledDraws draws are kept (emptied), and so is the command vector itself.
    void recycle(std::vector<RsxCommand>&& commands);

    // Draw buffer sets waiting to be reused by the next draws.
    std::size_t spareDraws() const { return spare_draws_.size(); }

    // While enabled, buildDraw is timed into stats().build_draw_ns; the clock is not read otherwise.
    void setTimingEnabled(bool enabled) { timing_enabled_ = enabled; }
    const RsxBuilderStats& stats() const { return stats_; }

    // Deferred mode: SET_BEGIN_END(0) only marks the draw as pending, and finishDraw() copies its guest
    // bytes and builds the command. The caller must call finishDraw() before the next write, with the
    // registers as that write left them. Off by default: the draw is built inside onWrite().
    void setDeferDrawEnd(bool defer) { defer_draw_end_ = defer; }
    bool drawPending() const { return draw_pending_; }
    void finishDraw(const RsxRegisters& regs);

    // Color and depth surfaces cleared or drawn to so far, indexed by RsxTextureDesc::rt_alias. A color
    // surface and a depth surface at one address share an entry (the later one wins). Entries are only
    // appended or replaced in place, so an index stays valid for the life of the builder.
    std::span<const RsxSurface> knownSurfaces() const { return known_surfaces_; }
    void setKnownSurfaces(std::vector<RsxSurface> surfaces) { known_surfaces_ = std::move(surfaces); }

    // Index the next flip command gets; a replay continues the numbering of the run it was captured from.
    void setFlipIndex(std::uint64_t index) { flips_ = index; }

    // Converted textures kept across draws (see rsx_texture_cache.h).
    const RsxTextureCache& textureCache() const { return textures_; }

private:
    struct Range {
        std::uint32_t first = 0, count = 0;
    };

    void beginDraw(std::uint32_t value);
    void addRange(bool indexed, std::uint32_t value);
    void endDraw(const RsxRegisters& regs);
    void buildDraw(const RsxRegisters& regs);
    void clear(const RsxRegisters& regs, std::uint32_t mask);
    bool fillVertices(const RsxRegisters& regs, RsxDraw& draw, std::uint32_t vertex_base, std::uint32_t vertex_count);
    void fillTextures(const RsxRegisters& regs, RsxDraw& draw);
    void copyTexture(RsxTextureDesc& texture);
    void fillFragmentProgram(const RsxRegisters& regs, RsxDraw& draw);
    bool matchFragmentProgram(std::uint32_t program, std::uint32_t location, std::uint32_t offset, RsxDraw& draw);
    void fillVertexProgram(const RsxRegisters& regs, RsxDraw& draw);
    std::int32_t noteSurface(const RsxSurface& surface);
    std::int32_t findSurface(std::uint32_t location, std::uint32_t offset) const;

    RsxMemoryView& memory_;
    std::vector<RsxCommand> commands_;
    std::vector<RsxSurface> known_surfaces_;
    std::uint64_t flips_ = 0;
    RsxTextureCache textures_;
    std::vector<std::uint8_t> texture_scratch_;  // guest bytes of the texture being checked or converted
    RsxBuilderStats stats_;
    bool timing_enabled_ = false;

    // The vectors of a finished draw, emptied but with their capacity, so that a steady stream of draws stops
    // allocating (and page-faulting) the same megabytes every frame. Capacity above the caps is freed so that one huge
    // draw does not pin its memory.
    struct DrawBuffers {
        std::vector<RsxVertexAttrib> attribs;
        std::vector<std::uint8_t> vertex_data, index_data;
        std::vector<RsxTextureDesc> textures;
        std::vector<std::uint32_t> vp_ucode, fp_ucode, fp_constants;
    };
    static constexpr std::size_t kRecycledDraws = 64;
    static constexpr std::size_t kRecycledVertexBytes = 1u << 20;
    static constexpr std::size_t kRecycledIndexBytes = 256u << 10;
    std::vector<DrawBuffers> spare_draws_;

    // A fragment program seen before, keyed by the SET_SHADER_PROGRAM value. A draw whose program bytes still match
    // `bytes` (apart from the inline constants) reuses `ucode` and `hash` and skips the word-by-word scan.
    struct FpCacheEntry {
        std::vector<std::uint8_t> bytes;             // big-endian image of RsxDraw::fp_ucode (constant blocks zeroed)
        std::vector<std::uint32_t> constant_blocks;  // word index of each inline constant block, program order
        std::vector<std::uint32_t> ucode;            // RsxDraw::fp_ucode
        std::uint64_t hash = 0;                      // RsxDraw::fp_hash = rsxHash64(bytes)
    };
    // Above the ~650 distinct program addresses of a heavy frame; the map is cleared when full.
    static constexpr std::size_t kFpCacheEntries = 1024;
    std::unordered_map<std::uint32_t, FpCacheEntry> fp_cache_;
    std::vector<std::uint8_t> fp_probe_;

    // A vertex program seen before, keyed by its start address. `source` is the store's words that the flow walk
    // read, [start * 4, (last + 1) * 4); `ucode_generation` is the store's name when the entry last matched.
    struct VpCacheEntry {
        std::uint32_t start = 0;
        std::uint64_t ucode_generation = 0;
        std::vector<std::uint32_t> source;
        bool terminated = false, bad_target = false;
        std::vector<std::uint32_t> ucode;  // RsxDraw::vp_ucode (empty when bad_target)
        std::uint64_t hash = 0;
    };
    static constexpr std::size_t kVpCacheEntries = 32;
    std::vector<VpCacheEntry> vp_cache_;  // most recently used first; the last is dropped when full

    // The constant snapshot the latest draws share, and the registers' name of the constants it holds.
    std::shared_ptr<std::vector<std::uint32_t>> constants_snapshot_;
    std::uint64_t constants_generation_ = 0;

    // The open draw clause between SET_BEGIN_END(prim) and SET_BEGIN_END(0).
    bool in_draw_ = false;
    bool skip_draw_ = false;  // BEGIN with a primitive RPCS3 rejects: the clause is ignored up to END
    bool have_range_ = false, indexed_ = false;
    RsxPrimitive primitive_ = RsxPrimitive::Triangles;
    Range range_;
    bool defer_draw_end_ = false;
    bool draw_pending_ = false;  // the clause ended and buildDraw() has not run yet
};

}  // namespace tox2
