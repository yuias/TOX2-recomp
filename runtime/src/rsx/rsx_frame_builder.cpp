#include "rsx/rsx_frame_builder.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "core/guest_memory.h"
#include "dal/ppu_context.h"
#include "rsx/fifo_parser.h"
#include "rsx/rsx_census.h"
#include "rsx/rsx_decode.h"
#include "rsx/rsx_unimplemented.h"

namespace dal {
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
        default:
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
}

std::vector<RsxCommand> RsxFrameBuilder::takeCommands() { return std::exchange(commands_, {}); }

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

std::int32_t RsxFrameBuilder::noteSurface(const RsxSurface& surface) {
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
    if (!c.surfaces.has_color) mask &= ~kColorBits;
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
// single_vertex blocks).
bool RsxFrameBuilder::fillVertices(const RsxRegisters& regs, RsxDraw& draw, std::uint32_t vertex_base,
                                   std::uint32_t vertex_count) {
    const std::uint32_t base_offset = regs.reg(kMethodNv4097SetVertexDataBaseOffset);
    draw.attribs = decodeVertexAttribs(regs);
    for (RsxVertexAttrib& attrib : draw.attribs) {
        const std::uint32_t element = elementBytes(attrib.type, attrib.size);
        if (element == 0) continue;
        const std::uint64_t first = attrib.stride == 0 ? 0 : static_cast<std::uint64_t>(vertex_base) * attrib.stride;
        const std::uint64_t length =
                attrib.stride == 0 ? element : static_cast<std::uint64_t>(vertex_count - 1) * attrib.stride + element;
        const std::uint64_t address = ((base_offset + attrib.offset) & 0xFFFFFFFull) + first;
        if (draw.vertex_data.size() + length + 3 > kRsxMaxVertexBytes) {
            noteState("vertex data too large");
            return false;
        }
        if (address + length > 0xFFFFFFFFull) {
            noteState("unreadable vertex memory");
            return false;
        }
        draw.vertex_data.resize((draw.vertex_data.size() + 3) & ~std::size_t{3});  // 4-byte aligned slots
        attrib.data_offset = static_cast<std::uint32_t>(draw.vertex_data.size());
        draw.vertex_data.resize(draw.vertex_data.size() + length);
        const std::span<std::uint8_t> out(draw.vertex_data.data() + attrib.data_offset, length);
        if (!memory_.read(attrib.location, static_cast<std::uint32_t>(address), out)) {
            noteState("unreadable vertex memory");
            return false;
        }
        attrib.has_data = true;
    }
    draw.default_attribs = decodeDefaultAttribs(regs);
    draw.default_written = regs.vertex_default_written;
    return true;
}

// Texture bytes are copied for the layouts the renderer supports: 2D, one mip level, A8R8G8B8 linear or
// DXT1/DXT23/DXT45 in any layout (block rows, see below). A texture at a known render target is an alias and carries no
// bytes. RPCS3 takes the pitch from the row size when the register is 0 (`texture_cache.h` upload path).
void RsxFrameBuilder::fillTextures(const RsxRegisters& regs, RsxDraw& draw) {
    for (std::uint32_t unit = 0; unit < kRsxTextureUnitCount; unit++) {
        if (!rsxTextureEnabled(regs, unit)) continue;
        RsxTextureDesc t = decodeTexture(regs, unit);

        t.rt_alias = findSurface(t.location, t.offset);
        if (t.rt_alias >= 0) {
            draw.textures.push_back(t);
            continue;
        }

        // Block-compressed rows are always contiguous, whatever the layout bit says.
        const bool compressed = rsxTextureBlockBytes(t.format) != 0;
        if (!t.linear_layout && !compressed) rsxNoteUnimplemented("texture layout", "swizzled");
        if (t.cubemap) rsxNoteUnimplemented("texture layout", "cubemap");
        if (t.mips > 1) rsxNoteUnimplemented("texture layout", std::format("mips {}", t.mips));
        if (t.dimension != 2) rsxNoteUnimplemented("texture layout", std::format("dimension {}", t.dimension));

        const bool supported_format = rsxTextureFormatSampleable(t.format);
        const bool supported_layout = (t.linear_layout || compressed) && !t.cubemap && t.mips <= 1 && t.dimension == 2;
        if (supported_format && supported_layout && t.width != 0 && t.height != 0) {
            std::uint64_t row, size;
            if (compressed) {
                // The pitch register is not used for the block rows (the game's value is a block row or 0).
                row = rsxCompressedRowBytes(t.format, t.width);
                size = rsxCompressedSize(t.format, t.width, t.height);
                t.pitch = static_cast<std::uint32_t>(row);
            } else {
                row = static_cast<std::uint64_t>(t.width) * 4;
                if (t.pitch == 0) t.pitch = static_cast<std::uint32_t>(row);
                // The last row only needs its pixels, not the padding up to the pitch.
                size = static_cast<std::uint64_t>(t.pitch) * (t.height - 1) + row;
            }
            if (t.pitch < row) {
                rsxNoteUnimplemented("texture layout", "pitch below row size");
            } else if (draw.texture_data.size() + size > kRsxMaxTextureBytes) {
                rsxNoteUnimplemented("texture layout", "too large");
            } else {
                draw.texture_data.resize((draw.texture_data.size() + 3) & ~std::size_t{3});
                t.data_offset = static_cast<std::uint32_t>(draw.texture_data.size());
                t.data_size = static_cast<std::uint32_t>(size);
                draw.texture_data.resize(draw.texture_data.size() + size);
                if (!memory_.read(t.location, t.offset,
                                  std::span<std::uint8_t>(draw.texture_data.data() + t.data_offset, size))) {
                    draw.texture_data.resize(t.data_offset);
                    t.data_offset = t.data_size = 0;
                    noteState("unreadable texture memory");
                }
            }
        }
        draw.textures.push_back(t);
    }
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
    draw.fp_ucode.reserve(scan.words.size());
    for (std::size_t p = 0; p + 4 <= scan.words.size();) {
        bool constant = false;
        for (std::size_t w = 1; w < 4; w++) constant = constant || (std::rotl(scan.words[p + w], 16) & 3) == 2;
        draw.fp_ucode.insert(draw.fp_ucode.end(), scan.words.begin() + p, scan.words.begin() + p + 4);
        p += 4;
        if (constant && p + 4 <= scan.words.size()) {
            draw.fp_constants.insert(draw.fp_constants.end(), scan.words.begin() + p, scan.words.begin() + p + 4);
            draw.fp_ucode.insert(draw.fp_ucode.end(), 4, 0);
            p += 4;
        }
    }
    draw.fp_hash = hashWords(draw.fp_ucode);
}

void RsxFrameBuilder::fillVertexProgram(const RsxRegisters& regs, RsxDraw& draw) {
    draw.vp_input_mask = regs.reg(kMethodNv4097SetVertexAttribInputMask);
    draw.vp_output_mask = regs.reg(kMethodNv4097SetVertexAttribOutputMask);
    draw.vp_constants.assign(regs.vp_constants.begin(), regs.vp_constants.end());
    const std::uint32_t start = regs.vp_start;
    if (start >= kRsxVpInstructionCount) {
        noteState("vertex program start out of range");
        return;
    }
    const RsxVpScan scan = scanVertexProgram(regs.vp_ucode.data(), start);
    if (!scan.terminated) noteState("unterminated vertex program");
    draw.vp_ucode.assign(regs.vp_ucode.begin() + start * 4, regs.vp_ucode.begin() + (start + scan.instructions) * 4);
    draw.vp_hash = hashWords(draw.vp_ucode);
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
        for (std::uint32_t i = 0; i < range_.count; i++) {
            const std::uint8_t* p = draw.index_data.data() + static_cast<std::size_t>(i) * type_size;
            const std::uint32_t index = type_size == 2 ? readBe16(p) : readBe32(p);
            if (restart && index == restart_index) {
                draw.restart_used = true;
                continue;
            }
            min_index = std::min(min_index, index);
            max_index = std::max(max_index, index);
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
    // drawn to is seen as an alias.
    if (draw.surfaces.has_color && draw.surfaces.color.format == RsxColorFormat::A8R8G8B8 &&
        draw.raster.color_mask != 0) {
        noteSurface(draw.surfaces.color);
    }
    fillTextures(regs, draw);
    fillFragmentProgram(regs, draw);
    fillVertexProgram(regs, draw);
    commands_.emplace_back(std::move(draw));
}

}  // namespace dal
