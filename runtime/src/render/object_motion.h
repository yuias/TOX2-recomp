#pragma once

// Candidates, signatures and previous-frame matching for per-object motion vectors. No Vulkan, so it is testable
// without a device.
//
// A candidate draw of this frame is paired with the same draw of the previous frame: the renderer evaluates the vertex
// program a second time with the previous frame's inputs and the difference of the two screen positions is the
// object's motion.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "render/dlaa_math.h"
#include "rsx/rsx_decode.h"
#include "rsx/rsx_frame.h"

namespace tox2 {

// A draw that writes object motion: a perspective draw (dlaaPerspective on c0..c3) into an anti-aliased colour surface
// (samples_x * samples_y > 1) that writes depth (rsxZetaWriteEnabled). The renderer adds the DLAA-mode conditions
// (one-sample scene sidecar, dlaa_evaluate).
inline bool objectMotionCandidate(const RsxDraw& draw) {
    const RsxSurface& color = draw.surfaces.color;
    return draw.vp_constants && dlaaPerspective(*draw.vp_constants) && color.samples_x * color.samples_y > 1 &&
           rsxZetaWriteEnabled(draw.raster, draw.surfaces.depth_format);
}

struct MotionDrawSignature {
    // Address-free identity: vp_hash, fp_hash, primitive, indexed, index_type, count, the vertex count
    // (max_index - min_index + 1), and per attribute slot 0..15 in slot order (slot, type, size, stride, has_data),
    // and per texture (unit, location, offset, texture format). Equal for the same mesh drawn the same way.
    std::uint64_t layout = 0;
    // Where the data sits: the attribute arrays' guest addresses (location, offset per slot with data), `first` and
    // `min_index`. Differs between frames when a mesh's vertices or indices are written to alternating buffers or
    // streamed through a ring, which must not break the address-free match.
    std::uint64_t address = 0;

    bool operator==(const MotionDrawSignature&) const = default;
};

namespace detail {

// Order-sensitive 64-bit mix (splitmix64 finaliser over a running state).
inline void motionMix(std::uint64_t& state, std::uint64_t value) {
    state ^= value + 0x9E3779B97F4A7C15ull + (state << 6) + (state >> 2);
    state *= 0xBF58476D1CE4E5B9ull;
    state ^= state >> 31;
}

}  // namespace detail

inline MotionDrawSignature motionDrawSignature(const RsxDraw& draw) {
    using detail::motionMix;
    constexpr std::size_t kSlots = 16;
    // Attributes are listed by slot but the order is not part of the draw's meaning, so look each slot up.
    std::array<const RsxVertexAttrib*, kSlots> by_slot{};
    for (const RsxVertexAttrib& attrib : draw.attribs) {
        by_slot[attrib.slot] = &attrib;
    }

    std::uint64_t layout = 0x243F6A8885A308D3ull;
    motionMix(layout, draw.vp_hash);
    motionMix(layout, draw.fp_hash);
    motionMix(layout, static_cast<std::uint64_t>(draw.primitive));
    motionMix(layout, draw.indexed ? 1 : 0);
    motionMix(layout, draw.index_type);
    motionMix(layout, draw.count);
    motionMix(layout, std::uint64_t{draw.max_index} - draw.min_index + 1);

    std::uint64_t address = 0x13198A2E03707344ull;
    for (std::size_t slot = 0; slot < kSlots; ++slot) {
        const RsxVertexAttrib* attrib = by_slot[slot];
        if (!attrib) continue;
        motionMix(layout, slot);
        motionMix(layout, attrib->type);
        motionMix(layout, attrib->size);
        motionMix(layout, attrib->stride);
        motionMix(layout, attrib->has_data ? 1 : 0);
        if (attrib->has_data) {
            motionMix(address, slot);
            motionMix(address, attrib->location);
            motionMix(address, attrib->offset);
        }
    }
    for (const RsxTextureDesc& texture : draw.textures) {
        motionMix(layout, texture.unit);
        motionMix(layout, texture.location);
        motionMix(layout, texture.offset);
        motionMix(layout, texture.format);
    }
    motionMix(address, draw.first);
    motionMix(address, draw.min_index);
    return {layout, address};
}

enum class MotionMatchRule : std::uint8_t {
    Ordered,             // the n-th draw of a layout in this frame matches the n-th of the previous frame
    AddressThenOrdered,  // an unused previous draw with equal layout and address, else the next unused one in order
    AddressOnly,         // equal layout and address only (ties in order)
};
inline constexpr MotionMatchRule kDefaultMotionMatchRule = MotionMatchRule::AddressThenOrdered;  // T4 may change it

class MotionDrawMatcher {
public:
    // `previous`: the previous frame's candidate signatures in draw order (empty: nothing matches).
    void beginFrame(std::span<const MotionDrawSignature> previous, MotionMatchRule rule) {
        rule_ = rule;
        previous_.assign(previous.begin(), previous.end());
        used_.assign(previous_.size(), 0);
        // Sorting by (layout, index) groups the repeats of a layout in draw order without per-node allocations.
        by_layout_.resize(previous_.size());
        for (std::uint32_t i = 0; i < previous_.size(); ++i) by_layout_[i] = {previous_[i].layout, i};
        std::sort(by_layout_.begin(), by_layout_.end());
    }

    // Called once per candidate of this frame, in draw order. Index into `previous`; each is matched at most once.
    std::optional<std::uint32_t> match(const MotionDrawSignature& current) {
        auto it = std::lower_bound(by_layout_.begin(), by_layout_.end(), Entry{current.layout, 0});
        std::optional<std::uint32_t> first_unused;
        for (; it != by_layout_.end() && it->layout == current.layout; ++it) {
            if (used_[it->index]) continue;
            if (rule_ != MotionMatchRule::AddressOnly && !first_unused) first_unused = it->index;
            if (rule_ == MotionMatchRule::Ordered) break;
            if (previous_[it->index].address == current.address) {
                used_[it->index] = 1;
                return it->index;
            }
        }
        if (rule_ == MotionMatchRule::Ordered || rule_ == MotionMatchRule::AddressThenOrdered) {
            if (first_unused) used_[*first_unused] = 1;
            return first_unused;
        }
        return std::nullopt;
    }

private:
    struct Entry {
        std::uint64_t layout = 0;
        std::uint32_t index = 0;
        auto operator<=>(const Entry&) const = default;
    };

    MotionMatchRule rule_ = kDefaultMotionMatchRule;
    std::vector<MotionDrawSignature> previous_;
    std::vector<Entry> by_layout_;
    std::vector<std::uint8_t> used_;
};

}  // namespace tox2
