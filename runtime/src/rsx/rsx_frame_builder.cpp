#include "rsx/rsx_frame_builder.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "core/guest_memory.h"
#include "tox2/ppu_context.h"
#include "rsx/fifo_parser.h"
#include "rsx/rsx_census.h"
#include "rsx/rsx_decode.h"
#include "rsx/rsx_texture_layout.h"
#include "rsx/rsx_unimplemented.h"

namespace tox2 {
namespace {

constexpr std::uint32_t kIoPageSize = 0x100000;
constexpr std::uint32_t kIoPageMask = kIoPageSize - 1;

// Registers that RPCS3 defers with an execution barrier when they change inside an open draw clause.
bool isDrawParameter(std::uint32_t method) {
    return method == kMethodNv4097SetVertexDataBaseOffset || method == kMethodNv4097SetVertexDataBaseIndex ||
           (method >= kMethodNv4097SetVertexDataArrayOffset &&
            method < kMethodNv4097SetVertexDataArrayOffset + 16 * 4) ||
           method == kMethodNv4097SetTransformConstantLoad ||
           (method >= kMethodNv4097SetTransformConstant &&
            method < kMethodNv4097SetTransformConstant + kRsxTransformWindow * 4);
}

void noteState(std::string_view key) { rsxNoteUnimplemented("state", key); }

// Guest bytes per vertex element: `size` components of the component width of the vertex type; the
// packed CMP type is one 32-bit word. 0 for the types the renderer cannot take (already counted by
// decodeVertexAttribs). RPCS3 `RSXThread.cpp` get_vertex_type_size_on_host pads 3-component vectors
// to four on the host; the guest data is not padded.
std::uint32_t elementBytes(std::uint32_t type, std::uint32_t size) {
    switch (type) {
        case 1:  // s1
        case 3:  // sf
        case 5:  // s32k
            return 2 * size;
        case 2:  // f
            return 4 * size;
        case 4:  // ub
        case 7:  // ub256
            return size;
        case 6:  // cmp
            return 4;
        default: return 0;
    }
}

std::uint32_t readBe32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

std::uint32_t readBe16(const std::uint8_t* p) { return (static_cast<std::uint32_t>(p[0]) << 8) | p[1]; }

// Smallest and largest index outside the restart value, and whether the restart value occurred. The restart test is
// hoisted out of the loops: most draws have no restart enabled, and a u16 index can never equal a wider value.
template <std::uint32_t kSize>
void scanIndices(const std::uint8_t* data, std::uint32_t count, bool restart, std::uint32_t restart_index,
                 std::uint32_t& min_index, std::uint32_t& max_index, bool& restart_used) {
    const auto read = [](const std::uint8_t* p) { return kSize == 2 ? readBe16(p) : readBe32(p); };
    std::uint32_t lo = 0xFFFFFFFF, hi = 0;
    if (kSize == 2 && restart_index > 0xFFFF) restart = false;
    if (!restart) {
        for (std::uint32_t i = 0; i < count; i++) {
            const std::uint32_t index = read(data + static_cast<std::size_t>(i) * kSize);
            lo = std::min(lo, index);
            hi = std::max(hi, index);
        }
    } else {
        for (std::uint32_t i = 0; i < count; i++) {
            const std::uint32_t index = read(data + static_cast<std::size_t>(i) * kSize);
            if (index == restart_index) {
                restart_used = true;
                continue;
            }
            lo = std::min(lo, index);
            hi = std::max(hi, index);
        }
    }
    min_index = lo;
    max_index = hi;
}

// Big-endian bytes of `words`, the form the census hashes programs in.
std::uint64_t hashWords(const std::vector<std::uint32_t>& words) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(words.size() * 4);
    for (const std::uint32_t word : words) {
        for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<std::uint8_t>(word >> shift));
    }
    return rsxHash64(bytes.data(), bytes.size());
}

// Fits [pos, pos + length) in [0, parent) the way RPCS3 `rsx::clip_region` does with attempt_fit set
// (`Utils/rsx_utils.h`): a region that starts inside the parent is shortened; one that starts outside
// moves to 0 and only keeps its length when that is below the parent size.
void clipAxis(std::uint32_t parent, std::uint32_t& pos, std::uint32_t& length) {
    if (pos + length <= parent) return;
    if (pos >= parent) {
        if (length >= parent) length = parent;
        pos = 0;
    } else {
        length = parent - pos;
    }
}

}  // namespace

bool GuestRsxMemoryView::read(std::uint32_t location, std::uint32_t offset, std::span<std::uint8_t> out) {
    if (out.empty()) return true;
    const GuestMemory& memory = process().memory;
    if (location == 0) {
        if (offset >= kRsxLocalSize || out.size() > kRsxLocalSize - offset) return false;
        const std::uint32_t ea = kRsxLocalBase + offset;
        if (!memory.isCommitted(ea, static_cast<std::uint32_t>(out.size()))) return false;
        std::memcpy(out.data(), base_ + ea, out.size());
        return true;
    }
    // The IO map is made of 1 MiB pages that need not be contiguous in guest memory.
    std::size_t done = 0;
    while (done < out.size()) {
        const std::uint64_t io = static_cast<std::uint64_t>(offset) + done;
        if (io > 0xFFFFFFFFull) return false;
        const std::uint32_t chunk = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                out.size() - done, kIoPageSize - (static_cast<std::uint32_t>(io) & kIoPageMask)));
        std::uint32_t ea = 0;
        if (!rsxIoToEa(static_cast<std::uint32_t>(io), ea) || !memory.isCommitted(ea, chunk)) return false;
        std::memcpy(out.data() + done, base_ + ea, chunk);
        done += chunk;
    }
    return true;
}

void RsxFrameBuilder::onWrite(const RsxRegisters& regs, std::uint32_t method, std::uint32_t value) {
    method &= 0xFFFC;
    if (const char* name = rsxIgnoredMethodName(method)) rsxNoteUnimplemented("method", name);
    switch (method) {
        case kMethodNv4097SetBeginEnd:
            // RPCS3 ignores the upper bits (nv4097.cpp set_begin_end).
            if ((value & 0xFF) != 0) {
                beginDraw(value & 0xFF);
            } else {
                endDraw(regs);
            }
            break;
        case kMethodNv4097DrawArrays: addRange(false, value); break;
        case kMethodNv4097DrawIndexArray: addRange(true, value); break;
        case kMethodNv4097ClearSurface: clear(regs, value); break;
        // Points where the guest learns how far the RSX has got, or the RSX changes memory outside a draw: the
        // guest may rewrite texture memory after them (see rsx_texture_cache.h).
        case kMethodNv406eSemaphoreAcquire:
        case kMethodNv406eSemaphoreRelease:
        case kMethodNv4097BackEndWriteSemaphoreRelease:
        case kMethodNv4097TextureReadSemaphoreRelease:
        case kMethodNv0039BufferNotify:
        case kMethodNv3089ImageIn: textures_.beginEpoch(); break;
        default:
            if (method >= kMethodNv308aColor && method < kMethodNv308aColor + kMethodNv308aColorCount * 4) {
                textures_.beginEpoch();
                break;
            }
            if (in_draw_ && have_range_ && isDrawParameter(method)) noteState("draw parameter change inside a clause");
            break;
    }
}

void RsxFrameBuilder::onFlip(const RsxRegisters&, std::uint32_t buffer, const RsxDisplayBuffer& display) {
    RsxFlip flip;
    flip.buffer = buffer;
    flip.offset = display.offset;
    flip.pitch = display.pitch;
    flip.width = display.width;
    flip.height = display.height;
    flip.flip_index = flips_++;
    commands_.emplace_back(flip);
    textures_.onFlip(flips_);
}

std::vector<RsxCommand> RsxFrameBuilder::takeCommands() { return std::exchange(commands_, {}); }

// Nothing is kept yet: the commands are released here.
void RsxFrameBuilder::recycle(std::vector<RsxCommand>&& commands) { commands.clear(); }

void RsxFrameBuilder::beginDraw(std::uint32_t primitive) {
    in_draw_ = true;
    have_range_ = false;
    indexed_ = false;
    // An invalid primitive still opens a clause in RPCS3 (it leaves the previous draw state in place and
    // sets in_begin_end); nothing sensible can be drawn from it.
    skip_draw_ = !decodePrimitive(primitive, primitive_);
}

// RPCS3 `draw_clause::append`: a range that continues the previous one extends it. Other cases (sorted
// insertion, duplicates, execution barriers) are not modelled.
void RsxFrameBuilder::addRange(bool indexed, std::uint32_t value) {
    if (!in_draw_) {
        noteState("draw range outside a clause");
        return;
    }
    const RsxDrawRange range = decodeDrawRange(value);
    if (!have_range_) {
        have_range_ = true;
        indexed_ = indexed;
        range_ = {range.first, range.count};
        return;
    }
    if (indexed != indexed_) {
        noteState("array and indexed ranges in one clause");
    } else if (range_.first + range_.count == range.first) {
        range_.count += range.count;
    } else {
        noteState("multiple ranges");
    }
}

std::int32_t RsxFrameBuilder::findSurface(std::uint32_t location, std::uint32_t offset) const {
    for (std::size_t i = 0; i < known_surfaces_.size(); i++) {
        if (known_surfaces_[i].location == location && known_surfaces_[i].offset == offset)
            return static_cast<std::int32_t>(i);
    }
    return -1;
}

// A color surface needs a color format the renderer can draw to; a depth surface carries its depth format
// and no color format. Anything else (no depth bound, an unsupported format) is not registered.
std::int32_t RsxFrameBuilder::noteSurface(const RsxSurface& surface) {
    const bool is_depth = surface.depth_format != RsxDepthFormat::None;
    if (is_depth ? surface.depth_format == RsxDepthFormat::Unsupported
                 : surface.format == RsxColorFormat::Unsupported) {
        return -1;
    }
    // The renderer keeps the surface contents on the GPU and guest memory stays as it was, but a texture
    // that overlaps the surface is then read again, as the RSX would see the pixels written to it.
    // The pitch covers the expanded row of an anti-aliased layout, and the surface holds samples_y rows per pixel row.
    if (surface.pitch == 0 || surface.height == 0) {
        textures_.beginEpoch();
    } else {
        textures_.noteWrite(surface.location, surface.offset,
                            static_cast<std::uint64_t>(surface.offset) +
                                    static_cast<std::uint64_t>(surface.pitch) * surface.height * surface.samples_y);
    }
    const std::int32_t index = findSurface(surface.location, surface.offset);
    if (index >= 0) {
        known_surfaces_[index] = surface;  // size or pitch may have changed
        return index;
    }
    if (known_surfaces_.size() >= kRsxMaxKnownSurfaces) {
        noteState("too many render targets");
        return -1;
    }
    known_surfaces_.push_back(surface);
    return static_cast<std::int32_t>(known_surfaces_.size() - 1);
}

// RPCS3 `VKGSRender::clear_surface`: the stencil bit is dropped while the stencil write mask is 0, a
// clear with no color/depth/stencil bit does nothing, stencil needs a D24S8 target and color needs a
// bound color target. The rectangle is the scissor clipped to the framebuffer (`rsx::clip_region`);
// an empty scissor makes RPCS3 skip the clear (`thread::get_scissor`). RPCS3 also intersects the
// scissor with the viewport for some clears; that is not modelled.
void RsxFrameBuilder::clear(const RsxRegisters& regs, std::uint32_t mask) {
    constexpr std::uint32_t kColorBits = kRsxClearRed | kRsxClearGreen | kRsxClearBlue | kRsxClearAlpha;
    RsxClear c = decodeClear(regs, mask);
    if ((regs.reg(kMethodNv4097SetStencilMask) & 0xFF) == 0) mask &= ~kRsxClearStencil;
    // RPCS3 `clear_surface` turns a color clear of a 32-bit float surface into a no-op.
    if (!c.surfaces.has_color || c.surfaces.color.format == RsxColorFormat::F_X32) mask &= ~kColorBits;
    if (c.surfaces.depth_format == RsxDepthFormat::None || c.surfaces.depth_format == RsxDepthFormat::Unsupported) {
        mask &= ~(kRsxClearDepth | kRsxClearStencil);
    } else if (c.surfaces.depth_format != RsxDepthFormat::Z24S8) {
        mask &= ~kRsxClearStencil;
    }
    if ((mask & (kColorBits | kRsxClearDepth | kRsxClearStencil)) == 0) return;
    c.mask = mask;

    if (c.scissor[2] == 0 || c.scissor[3] == 0) return;
    clipAxis(c.surfaces.clip_w, c.scissor[0], c.scissor[2]);
    clipAxis(c.surfaces.clip_h, c.scissor[1], c.scissor[3]);
    if (c.scissor[2] == 0 || c.scissor[3] == 0) return;

    if ((mask & kColorBits) != 0 && c.surfaces.color.format == RsxColorFormat::A8R8G8B8) noteSurface(c.surfaces.color);
    if ((mask & (kRsxClearDepth | kRsxClearStencil)) != 0) noteSurface(c.surfaces.depth);
    // RPCS3 clears a partially masked stencil with a shader pass; RsxClear carries no stencil mask, so
    // the renderer clears the whole byte.
    if ((mask & kRsxClearStencil) != 0 && (regs.reg(kMethodNv4097SetStencilMask) & 0xFF) != 0xFF) {
        rsxNoteUnimplemented("state", "partial stencil clear mask");
    }
    commands_.emplace_back(std::move(c));
}

// Per attribute, the guest bytes of vertices [vertex_base, vertex_base + vertex_count): address =
// (SET_VERTEX_DATA_BASE_OFFSET + array offset) & 0xFFFFFFF (`rsx::get_vertex_offset_from_base`), element
// k at k * stride. A stride of 0 means one element that every vertex shares (`RSXDrawCommands.cpp`
// single_vertex blocks). Attributes of one location whose ranges overlap or touch (the fields of an interleaved
// array) share one copy of the merged range; each attribute's data_offset points into it.
bool RsxFrameBuilder::fillVertices(const RsxRegisters& regs, RsxDraw& draw, std::uint32_t vertex_base,
                                   std::uint32_t vertex_count) {
    const std::uint32_t base_offset = regs.reg(kMethodNv4097SetVertexDataBaseOffset);
    draw.attribs = decodeVertexAttribs(regs);

    // Pass 1 finds every attribute's range. The first attribute that fails a check ends the plan; pass 2 still reads
    // the ones before it, so an unreadable earlier attribute is the note that wins, as when each attribute was sized
    // and read in turn. The size limit counts the ranges as if they were not shared, so the same draws are dropped.
    struct Source {
        std::uint64_t address = 0, length = 0;  // length 0: not copied
        std::size_t interval = 0;
    };
    std::array<Source, kRsxVertexSlotCount> sources;
    std::uint64_t unshared = draw.vertex_data.size();
    const char* failure = nullptr;
    std::size_t planned = draw.attribs.size();
    for (std::size_t i = 0; i < draw.attribs.size(); i++) {
        const RsxVertexAttrib& attrib = draw.attribs[i];
        const std::uint32_t element = elementBytes(attrib.type, attrib.size);
        if (element == 0) continue;
        const std::uint64_t first = attrib.stride == 0 ? 0 : static_cast<std::uint64_t>(vertex_base) * attrib.stride;
        const std::uint64_t length =
                attrib.stride == 0 ? element : static_cast<std::uint64_t>(vertex_count - 1) * attrib.stride + element;
        const std::uint64_t address = ((base_offset + attrib.offset) & 0xFFFFFFFull) + first;
        if (unshared + length + 3 > kRsxMaxVertexBytes) {
            failure = "vertex data too large";
        } else if (address + length > 0xFFFFFFFFull) {
            failure = "unreadable vertex memory";
        }
        if (failure != nullptr) {
            planned = i;
            break;
        }
        unshared = ((unshared + 3) & ~std::uint64_t{3}) + length;
        sources[i] = {address, length, 0};
    }

    // Merge the ranges per location, sweeping them in address order.
    struct Interval {
        std::uint32_t location = 0;
        std::uint64_t begin = 0, end = 0, offset = 0;
        bool placed = false, read = false;
    };
    std::array<Interval, kRsxVertexSlotCount> intervals;
    std::size_t interval_count = 0;
    std::array<std::uint8_t, kRsxVertexSlotCount> by_address;
    std::size_t sorted_count = 0;
    {
        for (std::size_t i = 0; i < planned; i++) {
            if (sources[i].length != 0) by_address[sorted_count++] = static_cast<std::uint8_t>(i);
        }
        std::sort(by_address.begin(), by_address.begin() + sorted_count, [&](std::uint8_t a, std::uint8_t b) {
            const std::uint32_t location_a = draw.attribs[a].location, location_b = draw.attribs[b].location;
            return location_a != location_b ? location_a < location_b : sources[a].address < sources[b].address;
        });
        for (std::size_t k = 0; k < sorted_count; k++) {
            Source& source = sources[by_address[k]];
            const std::uint32_t location = draw.attribs[by_address[k]].location;
            if (interval_count != 0 && intervals[interval_count - 1].location == location &&
                source.address <= intervals[interval_count - 1].end) {
                intervals[interval_count - 1].end =
                        std::max(intervals[interval_count - 1].end, source.address + source.length);
            } else {
                intervals[interval_count++] = {location, source.address, source.address + source.length,
                                               0,        false,          false};
            }
            source.interval = interval_count - 1;
        }
    }

    // Slots are 4-byte aligned and laid out in the order the attributes first use them, so draws without overlap keep
    // today's layout.
    std::uint64_t total = draw.vertex_data.size();
    std::array<std::size_t, kRsxVertexSlotCount> layout;
    std::size_t layout_count = 0;
    for (std::size_t i = 0; i < planned; i++) {
        if (sources[i].length == 0) continue;
        Interval& interval = intervals[sources[i].interval];
        if (!interval.placed) {
            total = (total + 3) & ~std::uint64_t{3};
            interval.offset = total;
            interval.placed = true;
            total += interval.end - interval.begin;
            layout[layout_count++] = sources[i].interval;
        }
        draw.attribs[i].data_offset =
                static_cast<std::uint32_t>(interval.offset + (sources[i].address - interval.begin));
    }

    // While timing, the distinct bytes of the attributes whose interval was read, merged again from the attribute
    // ranges themselves: equal to the bytes copied unless the merge above is wrong.
    const auto countUnion = [&] {
        if (!timing_enabled_) return;
        bool open = false;
        std::uint32_t location = 0;
        std::uint64_t begin = 0, end = 0;
        for (std::size_t k = 0; k < sorted_count; k++) {
            const std::size_t i = by_address[k];
            if (!intervals[sources[i].interval].read) continue;
            const std::uint64_t source_end = sources[i].address + sources[i].length;
            if (open && draw.attribs[i].location == location && sources[i].address <= end) {
                end = std::max(end, source_end);
                continue;
            }
            if (open) stats_.vertex_bytes_union += end - begin;
            open = true;
            location = draw.attribs[i].location;
            begin = sources[i].address;
            end = source_end;
        }
        if (open) stats_.vertex_bytes_union += end - begin;
    };

    draw.vertex_data.resize(static_cast<std::size_t>(total));
    for (std::size_t k = 0; k < layout_count; k++) {
        Interval& interval = intervals[layout[k]];
        const std::span<std::uint8_t> out(draw.vertex_data.data() + interval.offset, interval.end - interval.begin);
        if (!memory_.read(interval.location, static_cast<std::uint32_t>(interval.begin), out)) {
            countUnion();
            noteState("unreadable vertex memory");
            return false;
        }
        interval.read = true;
        stats_.vertex_bytes_copied += out.size();
    }
    countUnion();
    for (std::size_t i = 0; i < planned; i++) {
        if (sources[i].length != 0) draw.attribs[i].has_data = true;
    }
    if (failure != nullptr) {
        noteState(failure);
        return false;
    }
    draw.default_attribs = decodeDefaultAttribs(regs);
    draw.default_written = regs.vertex_default_written;
    return true;
}

// Texture bytes are copied for 2D, 3D and cube map A8R8G8B8 and DXT1/DXT23/DXT45 textures, linear or
// swizzled, with their whole mip chain. The copy is converted to what the renderer uploads: every level in
// tightly packed rows and slices (padding removed), de-swizzled, and a cube map as six faces one after the
// other; see rsx_texture_layout.h for the guest layout rules. The converted bytes are a shared blob kept in
// the texture cache (rsx_texture_cache.h), so draws that sample an unchanged texture share one blob and one
// content id. A texture at a known render target is an alias and carries no bytes; that test comes first, so
// a cached blob is never used for an address that is a render target now. RPCS3 takes the pitch from the row
// size when the register is 0 (`texture_cache.h` upload path).
void RsxFrameBuilder::fillTextures(const RsxRegisters& regs, RsxDraw& draw) {
    for (std::uint32_t unit = 0; unit < kRsxTextureUnitCount; unit++) {
        if (!rsxTextureEnabled(regs, unit)) continue;
        RsxTextureDesc t = decodeTexture(regs, unit);

        t.rt_alias = findSurface(t.location, t.offset);
        if (t.rt_alias >= 0) {
            t.rt_depth = known_surfaces_[t.rt_alias].depth_format != RsxDepthFormat::None;
            draw.textures.push_back(std::move(t));
            continue;
        }
        if (rsxTextureFormatRenderTargetOnly(t.format))
            rsxNoteUnimplemented("texture format", rsxTextureFormatName(t.format));

        const RsxTextureKind kind = rsxTextureKind(t);
        if (kind == RsxTextureKind::Unsupported) {
            rsxNoteUnimplemented("texture layout", std::format("dimension {}", t.dimension));
        }

        const bool supported_format = rsxTextureFormatSampleable(t.format);
        if (supported_format && kind != RsxTextureKind::Unsupported && t.width != 0 && t.height != 0) {
            copyTexture(t);
        }
        draw.textures.push_back(std::move(t));
    }
}

void RsxFrameBuilder::copyTexture(RsxTextureDesc& t) {
    const std::uint32_t layers = rsxTextureKind(t) == RsxTextureKind::Cube ? 6 : 1;
    const RsxTextureCache::Key key{t.location,     t.offset, t.width,
                                   t.height,       t.pitch,  t.depth,
                                   t.format,       t.mips,   static_cast<std::uint8_t>(layers),
                                   t.linear_layout};
    RsxTextureCache::Entry* entry = textures_.find(key);
    if (entry != nullptr && textures_.fresh(*entry)) {
        t.pitch = entry->pitch;
        t.data = entry->data;
        t.content_id = entry->content_id;
        return;
    }

    RsxTextureLayout fresh_layout;
    if (entry == nullptr) {
        if (!rsxTextureLayout(t.format, t.linear_layout, t.width, t.height, t.mips, t.pitch, layers, fresh_layout,
                              t.depth)) {
            // The swizzle needs a power-of-two image; otherwise the pitch register is smaller than a row.
            const bool compressed = rsxTextureBlockBytes(t.format) != 0;
            const bool power_of_two = std::has_single_bit(t.width) && std::has_single_bit(t.height) &&
                                      std::has_single_bit<std::uint32_t>(t.depth);
            if (!t.linear_layout && !compressed && !power_of_two) {
                rsxNoteUnimplemented("texture layout", "swizzled non-power-of-two");
            } else {
                rsxNoteUnimplemented("texture layout", "pitch below row size");
            }
            return;
        }
        if (fresh_layout.guest_bytes > kRsxMaxTextureBytes || fresh_layout.packed_bytes > kRsxMaxTextureBytes) {
            rsxNoteUnimplemented("texture layout", "too large");
            return;
        }
    }
    const RsxTextureLayout& layout = entry != nullptr ? entry->layout : fresh_layout;

    // First use in this epoch, or after a render target write over the range: the guest bytes are read and
    // compared with the ones the blob was made from. The scratch buffer is reused, so a texture that did not
    // change costs one read and one hash.
    texture_scratch_.resize(layout.guest_bytes);
    if (!memory_.read(t.location, t.offset, texture_scratch_)) {
        textures_.erase(key);
        noteState("unreadable texture memory");
        return;
    }
    const std::uint64_t hash = rsxHashBytes(texture_scratch_);
    if (entry != nullptr && entry->hash == hash) {
        textures_.markFresh(*entry);
    } else {
        // Bytes that need no conversion are taken as they are.
        bool in_place = layout.guest_bytes == layout.packed_bytes;
        for (const RsxTextureLevel& level : layout.levels) {
            in_place = in_place && !level.swizzled && !level.vtc && level.guest_offset == level.packed_offset &&
                       level.guest_pitch == level.row_bytes;
        }
        auto blob = std::make_shared<std::vector<std::uint8_t>>();
        if (in_place) {
            blob->assign(texture_scratch_.begin(), texture_scratch_.end());
        } else {
            blob->resize(layout.packed_bytes);
            bool ok = true;
            for (std::size_t i = 0; ok && i < layout.levels.size(); i++) {
                const RsxTextureLevel& level = layout.levels[i];
                ok = rsxExtractTextureLevel(
                        level, layout.block_bytes, texture_scratch_,
                        std::span<std::uint8_t>(blob->data() + level.packed_offset, level.packed_bytes));
            }
            if (!ok) {
                textures_.erase(key);
                noteState("unreadable texture memory");
                return;
            }
        }
        entry = &textures_.store(key, layout, hash, std::move(blob));
    }
    t.pitch = entry->pitch;
    t.data = entry->data;
    t.content_id = entry->content_id;
}

// SET_SHADER_PROGRAM: location (value & 3) - 1, offset value & ~3 (RPCS3 `rsx_decode.h`). The program
// is read up to its end flag. Words are kept as stored (big-endian read, 16-bit halves still swapped);
// the 4-word block after an instruction that has a constant operand is moved into `fp_constants` and
// zeroed in `fp_ucode`, so programs that differ only in constants share a hash.
void RsxFrameBuilder::fillFragmentProgram(const RsxRegisters& regs, RsxDraw& draw) {
    const std::uint32_t program = regs.reg(kMethodNv4097SetShaderProgram);
    const std::uint32_t location = (program & 3) - 1, offset = program & ~3u;
    draw.fp_control = regs.reg(kMethodNv4097SetShaderControl);
    // RPCS3 `rsx_methods.h` texcoord_control_mask: bit i = register i & 1, ten coordinates. The point
    // sprite override in the high half (`RSXThread.cpp`) is not modelled.
    for (std::uint32_t i = 0; i < 10; i++) {
        draw.texcoord_control |= (regs.reg(kMethodNv4097SetTexCoordControl + i * 4) & 1) << i;
    }
    if (location > 1) {
        noteState("fragment program unreadable");
        return;
    }
    if (matchFragmentProgram(program, location, offset, draw)) {
        stats_.fp_cache_hits++;
        return;
    }
    stats_.fp_cache_misses++;
    draw.fp_constants.clear();
    const RsxFpScan scan = scanFragmentProgram([&](std::uint32_t index, std::uint32_t& word) {
        std::uint8_t bytes[4];
        if (!memory_.read(location, offset + index * 4, bytes)) return false;
        word = readBe32(bytes);
        return true;
    });
    if (!scan.terminated) {
        noteState("fragment program unreadable");
        return;
    }
    FpCacheEntry entry;
    draw.fp_ucode.reserve(scan.words.size());
    for (std::size_t p = 0; p + 4 <= scan.words.size();) {
        bool constant = false;
        for (std::size_t w = 1; w < 4; w++) constant = constant || (std::rotl(scan.words[p + w], 16) & 3) == 2;
        draw.fp_ucode.insert(draw.fp_ucode.end(), scan.words.begin() + p, scan.words.begin() + p + 4);
        p += 4;
        if (constant && p + 4 <= scan.words.size()) {
            draw.fp_constants.insert(draw.fp_constants.end(), scan.words.begin() + p, scan.words.begin() + p + 4);
            entry.constant_blocks.push_back(static_cast<std::uint32_t>(draw.fp_ucode.size()));
            draw.fp_ucode.insert(draw.fp_ucode.end(), 4, 0);
            p += 4;
        }
    }
    entry.bytes.reserve(draw.fp_ucode.size() * 4);
    for (const std::uint32_t word : draw.fp_ucode) {
        for (int shift = 24; shift >= 0; shift -= 8) entry.bytes.push_back(static_cast<std::uint8_t>(word >> shift));
    }
    draw.fp_hash = rsxHash64(entry.bytes.data(), entry.bytes.size());
    entry.ucode = draw.fp_ucode;
    entry.hash = draw.fp_hash;
    if (fp_cache_.size() >= kFpCacheEntries) fp_cache_.clear();
    fp_cache_.insert_or_assign(program, std::move(entry));
}

// A program is the same when its bytes are, apart from the inline constants: the scan's shape (which blocks are
// constants, where the end flag is) depends only on the instruction words, and those are the bytes compared. The
// probe reads only what the scan would read for an unchanged program; a changed one falls back to the scan.
bool RsxFrameBuilder::matchFragmentProgram(std::uint32_t program, std::uint32_t location, std::uint32_t offset,
                                           RsxDraw& draw) {
    const auto it = fp_cache_.find(program);
    if (it == fp_cache_.end()) return false;
    const FpCacheEntry& entry = it->second;
    fp_probe_.resize(entry.bytes.size());
    if (!memory_.tryRead(location, offset, fp_probe_)) return false;
    draw.fp_constants.reserve(entry.constant_blocks.size() * 4);
    for (const std::uint32_t block : entry.constant_blocks) {
        std::uint8_t* bytes = fp_probe_.data() + static_cast<std::size_t>(block) * 4;
        for (std::size_t w = 0; w < 4; w++) draw.fp_constants.push_back(readBe32(bytes + w * 4));
        std::memset(bytes, 0, 16);
    }
    if (std::memcmp(fp_probe_.data(), entry.bytes.data(), entry.bytes.size()) != 0) return false;
    draw.fp_ucode = entry.ucode;
    draw.fp_hash = entry.hash;
    return true;
}

void RsxFrameBuilder::fillVertexProgram(const RsxRegisters& regs, RsxDraw& draw) {
    draw.vp_input_mask = regs.reg(kMethodNv4097SetVertexAttribInputMask);
    draw.vp_output_mask = regs.reg(kMethodNv4097SetVertexAttribOutputMask);
    draw.vp_branch_bits = regs.reg(kMethodNv4097SetTransformBranchBits);
    // Draws between two constant writes share one snapshot. A snapshot no draw holds any more is rewritten in place
    // instead of allocating a new 7.5 KB vector.
    const std::uint64_t constants_generation = regs.vpConstantsGeneration();
    if (!constants_snapshot_ || constants_generation_ != constants_generation) {
        if (constants_snapshot_ && constants_snapshot_.use_count() == 1) {
            constants_snapshot_->assign(regs.vp_constants.begin(), regs.vp_constants.end());
        } else {
            constants_snapshot_ =
                    std::make_shared<std::vector<std::uint32_t>>(regs.vp_constants.begin(), regs.vp_constants.end());
        }
        constants_generation_ = constants_generation;
    }
    draw.vp_constants = constants_snapshot_;
    draw.vp_constants_id = constants_generation;
    const std::uint32_t start = regs.vp_start;
    if (start >= kRsxVpInstructionCount) {
        noteState("vertex program start out of range");
        return;
    }
    // The flow walk reads only reached instructions, all within [start, flow.last], so a program seen before is the
    // same while those words are. The store's name decides that without reading it; when the name is new (the
    // program was uploaded again), the words are compared.
    const std::uint64_t generation = regs.vpUcodeGeneration();
    for (std::size_t i = 0; i < vp_cache_.size(); i++) {
        VpCacheEntry& entry = vp_cache_[i];
        if (entry.start != start) continue;
        if (entry.ucode_generation != generation &&
            std::memcmp(regs.vp_ucode.data() + start * 4, entry.source.data(), entry.source.size() * 4) != 0) {
            continue;
        }
        entry.ucode_generation = generation;
        std::rotate(vp_cache_.begin(), vp_cache_.begin() + i, vp_cache_.begin() + i + 1);
        stats_.vp_cache_hits++;
        // Same notes as the walk below, so the census counts per draw do not depend on the cache.
        const VpCacheEntry& hit = vp_cache_.front();
        if (!hit.terminated) noteState("unterminated vertex program");
        if (hit.bad_target) {
            noteState("vertex program branch outside the program");
            return;
        }
        draw.vp_ucode = hit.ucode;
        draw.vp_hash = hit.hash;
        return;
    }
    stats_.vp_cache_misses++;

    // The program may extend past its first end bit when branches or calls lead there, and the store
    // can hold stale words in between. Only reached instructions are kept (the rest are zeroed, which
    // decodes as a NOP) so that the hash does not depend on leftovers, and branch targets are rebased
    // to the first instruction so that the same program hashes the same wherever it was loaded.
    const RsxVpFlow flow = analyzeVertexProgramFlow(regs.vp_ucode.data(), kRsxVpInstructionCount, start);
    VpCacheEntry entry;
    entry.start = start;
    entry.ucode_generation = generation;
    entry.source.assign(regs.vp_ucode.begin() + start * 4, regs.vp_ucode.begin() + (flow.last + 1) * 4);
    entry.terminated = flow.terminated;
    entry.bad_target = flow.bad_target;
    if (!flow.terminated) noteState("unterminated vertex program");
    if (flow.bad_target) {
        noteState("vertex program branch outside the program");
    } else {
        draw.vp_ucode.assign((flow.last - start + 1) * 4, 0);
        for (std::uint32_t i = start; i <= flow.last; i++) {
            if (flow.reached[i] == 0) continue;
            std::uint32_t* out = &draw.vp_ucode[(i - start) * 4];
            std::copy_n(&regs.vp_ucode[i * 4], 4, out);
            if (rsxVpHasBranchTarget(rsxVpScaOpcode(out))) rsxVpSetBranchTarget(out, rsxVpBranchTarget(out) - start);
        }
        draw.vp_hash = hashWords(draw.vp_ucode);
        entry.ucode = draw.vp_ucode;
        entry.hash = draw.vp_hash;
    }
    if (vp_cache_.size() >= kVpCacheEntries) vp_cache_.pop_back();
    vp_cache_.insert(vp_cache_.begin(), std::move(entry));
}

void RsxFrameBuilder::endDraw(const RsxRegisters& regs) {
    if (!in_draw_) return;
    in_draw_ = false;
    if (skip_draw_ || !have_range_) return;
    if (defer_draw_end_) {
        draw_pending_ = true;
        return;
    }
    buildDraw(regs);
}

void RsxFrameBuilder::finishDraw(const RsxRegisters& regs) {
    if (!draw_pending_) return;
    draw_pending_ = false;
    buildDraw(regs);
}

void RsxFrameBuilder::buildDraw(const RsxRegisters& regs) {
    // Counts the early returns for dropped draws too.
    struct Timer {
        RsxBuilderStats& stats;
        bool enabled;
        std::chrono::steady_clock::time_point begin;
        Timer(RsxBuilderStats& stats, bool enabled) : stats(stats), enabled(enabled) {
            if (enabled) begin = std::chrono::steady_clock::now();
        }
        ~Timer() {
            if (!enabled) return;
            stats.build_draw_ns += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin)
                            .count());
        }
    } timer(stats_, timing_enabled_);
    stats_.build_draw_calls++;

    RsxDraw draw;
    draw.primitive = primitive_;
    draw.indexed = indexed_;
    draw.first = range_.first;
    draw.count = range_.count;

    std::uint32_t vertex_base = range_.first, vertex_count = range_.count;
    if (indexed_) {
        // RPCS3 `get_raw_index_array`: the address is aligned down to the index size, the range starts
        // `first` indices in.
        const RsxIndexArray array = decodeIndexArray(regs);
        draw.index_type = array.type;
        const std::uint32_t type_size = array.type == kRsxIndexTypeU16 ? 2 : 4;
        if (range_.count > kRsxMaxIndexCount) {
            noteState("index data too large");
            return;
        }
        draw.index_data.resize(static_cast<std::size_t>(range_.count) * type_size);
        const std::uint64_t address =
                (array.offset & ~(type_size - 1)) + static_cast<std::uint64_t>(range_.first) * type_size;
        if (address > 0xFFFFFFFFull ||
            !memory_.read(array.location, static_cast<std::uint32_t>(address), draw.index_data)) {
            noteState("unreadable index memory");
            return;
        }

        const bool restart = regs.reg(kMethodNv4097SetRestartIndexEnable) != 0;
        const std::uint32_t restart_index = regs.reg(kMethodNv4097SetRestartIndex);
        std::uint32_t min_index = 0xFFFFFFFF, max_index = 0;
        if (type_size == 2) {
            scanIndices<2>(draw.index_data.data(), range_.count, restart, restart_index, min_index, max_index,
                           draw.restart_used);
        } else {
            scanIndices<4>(draw.index_data.data(), range_.count, restart, restart_index, min_index, max_index,
                           draw.restart_used);
        }
        draw.restart_index = restart_index;
        draw.max_index = max_index;
        // What the renderer can do with a restart index is its own business; only an unsupported one is counted.
        if (draw.restart_used && !rsxPrimitiveRestartSupported(primitive_, draw.index_type, restart_index)) {
            noteState("primitive restart");
        }
        // RPCS3 `VKVertexBuffers.cpp`: an empty index set, or a single index outside point lists, draws
        // nothing.
        if (min_index > max_index || (min_index == max_index && primitive_ != RsxPrimitive::Points)) return;
        // Indices 0 and 0xFFFFFFFF together span 2^32 vertices, which no count can hold.
        if (static_cast<std::uint64_t>(max_index) - min_index + 1 > 0xFFFFFFFFull) {
            noteState("index range too large");
            return;
        }
        draw.min_index = min_index;
        // The vertex start of an indexed draw is rebased by SET_VERTEX_DATA_BASE_INDEX
        // (`rsx::get_index_from_base`); array draws are not.
        vertex_base = (min_index + regs.reg(kMethodNv4097SetVertexDataBaseIndex)) & 0xFFFFF;
        vertex_count = max_index - min_index + 1;
    } else {
        draw.min_index = range_.first;
        draw.max_index = range_.first + range_.count - 1;
    }

    draw.surfaces = decodeSurfaceState(regs);
    draw.raster = decodeRasterState(regs);

    if (!fillVertices(regs, draw, vertex_base, vertex_count)) return;
    // Registered only for a draw that is kept, and before the textures so that sampling the target being
    // drawn to is seen as an alias. A depth surface counts once the draw can write it, by depth or by
    // stencil (rsxZetaWriteEnabled), so the unused depth binding of a pass that only needs a color target
    // is not taken for a surface.
    if (draw.surfaces.has_color &&
        (draw.surfaces.color.format == RsxColorFormat::A8R8G8B8 ||
         draw.surfaces.color.format == RsxColorFormat::F_X32) &&
        draw.raster.color_mask != 0) {
        noteSurface(draw.surfaces.color);
    }
    if (rsxZetaWriteEnabled(draw.raster, draw.surfaces.depth_format)) noteSurface(draw.surfaces.depth);
    fillTextures(regs, draw);
    fillFragmentProgram(regs, draw);
    fillVertexProgram(regs, draw);
    commands_.emplace_back(std::move(draw));
}

}  // namespace tox2
