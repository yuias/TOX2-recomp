#pragma once

// Builds RSX captures in memory for tests that replay them without the game: a clear, one quad with
// big-endian float positions and ub4 colours, and one quad textured with a 2x2 texture. The registers
// are written the way the SDK writes them and recorded through the real capture writer, so a replay
// gives the same commands a live consumer would.
//
// The programs are placeholders that only carry the end flag, enough for the frame builder. A test that
// renders the scene supplies real vertex and fragment programs through `Programs`.

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include "rsx/fifo_parser.h"
#include "rsx/rsx_capture.h"
#include "rsx/rsx_frame_builder.h"
#include "rsx/rsx_registers.h"
#include "rsx/rsx_state.h"

namespace dal::rsx_synthetic {

inline constexpr std::uint32_t kSurfaceSize = 64;  // the color target and display buffer are 64x64
inline constexpr std::uint32_t kSurfacePitch = kSurfaceSize * 4;
inline constexpr std::uint32_t kSurfaceOffset = 0x20000;   // local memory; never read, only addressed
inline constexpr std::uint32_t kPositionsOffset = 0x1000;  // main memory
inline constexpr std::uint32_t kColorsOffset = 0x1100;
inline constexpr std::uint32_t kTexCoordsOffset = 0x1200;
inline constexpr std::uint32_t kFragmentProgramOffset = 0x2000;  // local memory, the fragment program
inline constexpr std::uint32_t kTextureOffset = 0x3000;          // local memory, 2x2 A8R8G8B8 linear
inline constexpr std::uint32_t kTextureSize = 2;
inline constexpr std::uint32_t kClearColor = 0xFF102030;  // ARGB

struct Programs {
    std::vector<std::uint32_t> vertex{0, 0, 0, 1};  // decoded words, the last instruction flagged
    std::vector<std::uint32_t> fragment{0x00010000, 0, 0, 0};
};

// The 2x2 texture, row by row: red, green / blue, half-transparent white (ARGB).
inline constexpr std::array<std::uint32_t, 4> kTexturePixels = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0x80FFFFFF};

class SyntheticMemory final : public RsxMemoryView {
public:
    SyntheticMemory() : local_(0x10000), main_(0x10000) {}
    bool read(std::uint32_t location, std::uint32_t offset, std::span<std::uint8_t> out) override {
        const std::vector<std::uint8_t>& bytes = location == 0 ? local_ : main_;
        if (offset > bytes.size() || out.size() > bytes.size() - offset) return false;
        std::memcpy(out.data(), bytes.data() + offset, out.size());
        return true;
    }
    void putWord(std::uint32_t location, std::uint32_t offset, std::uint32_t word) {
        std::vector<std::uint8_t>& bytes = location == 0 ? local_ : main_;
        for (std::uint32_t i = 0; i < 4; i++) bytes[offset + i] = static_cast<std::uint8_t>(word >> (24 - 8 * i));
    }
    void putFloat(std::uint32_t location, std::uint32_t offset, float value) {
        putWord(location, offset, std::bit_cast<std::uint32_t>(value));
    }

private:
    std::vector<std::uint8_t> local_, main_;
};

// Applies writes to the registers, the frame builder and the capture writer in the order the consumer
// does, and ends the frame with a flip of buffer 0.
class SceneBuilder {
public:
    SceneBuilder() : regs_(std::make_unique<RsxRegisters>()), recording_(memory_), builder_(recording_) {
        regs_->reset();
        writer_ = std::make_unique<RsxCaptureWriter>(0, *regs_, builder_.knownSurfaces());
        recording_.setWriter(writer_.get());
    }

    SyntheticMemory& memory() { return memory_; }

    void write(std::uint32_t method, std::uint32_t value) {
        regs_->apply(method, value);
        builder_.onWrite(*regs_, method, value);
        writer_->onWrite(method, value);
    }

    // A8R8G8B8 color target with Z24S8, clip and scissor covering all of it.
    void setSurface() {
        write(kMethodNv4097SetSurfaceClipHorizontal, kSurfaceSize << 16);
        write(kMethodNv4097SetSurfaceClipVertical, kSurfaceSize << 16);
        write(kMethodNv4097SetSurfaceFormat, 8u | (2u << 5) | (1u << 8));
        write(kMethodNv4097SetSurfacePitchA, kSurfacePitch);
        write(kMethodNv4097SetSurfaceColorAOffset, kSurfaceOffset);
        write(kMethodNv4097SetSurfaceZetaOffset, kSurfaceOffset + 0x10000);
        write(kMethodNv4097SetSurfacePitchZ, kSurfacePitch);
        write(kMethodNv4097SetSurfaceColorTarget, 1);
        write(kMethodNv4097SetScissorHorizontal, kSurfaceSize << 16);
        write(kMethodNv4097SetScissorVertical, kSurfaceSize << 16);
    }

    void clear(std::uint32_t color_argb) {
        write(kMethodNv4097SetColorClearValue, color_argb);
        write(kMethodNv4097SetZstencilClearValue, 0xFFFFFF00);
        write(kMethodNv4097ClearSurface, 0xF3);
    }

    void setPrograms(const Programs& programs) {
        write(kMethodNv4097SetTransformProgramLoad, 0);
        write(kMethodNv4097SetTransformProgramStart, 0);
        for (std::size_t i = 0; i < programs.vertex.size(); i++) {
            write(kMethodNv4097SetTransformProgram + static_cast<std::uint32_t>(i % 4) * 4, programs.vertex[i]);
        }
        for (std::size_t i = 0; i < programs.fragment.size(); i++) {
            memory_.putWord(0, kFragmentProgramOffset + static_cast<std::uint32_t>(i) * 4, programs.fragment[i]);
        }
        write(kMethodNv4097SetShaderProgram, kFragmentProgramOffset | 1);  // low bits: 1 = local memory
    }

    // Slot `slot` reads `size` floats per vertex from main memory.
    void setFloatAttrib(std::uint32_t slot, std::uint32_t size, std::uint32_t offset) {
        write(kMethodNv4097SetVertexDataArrayFormat + slot * 4, 2u | (size << 4) | ((size * 4) << 8));
        write(kMethodNv4097SetVertexDataArrayOffset + slot * 4, 0x80000000u | offset);
    }
    void setColorAttrib(std::uint32_t slot, std::uint32_t offset) {
        write(kMethodNv4097SetVertexDataArrayFormat + slot * 4, 4u | (4u << 4) | (4u << 8));
        write(kMethodNv4097SetVertexDataArrayOffset + slot * 4, 0x80000000u | offset);
    }

    // A triangle strip of `count` vertices from the enabled arrays.
    void drawStrip(std::uint32_t count) {
        write(kMethodNv4097SetBeginEnd, static_cast<std::uint32_t>(RsxPrimitive::TriangleStrip));
        write(kMethodNv4097DrawArrays, (count - 1) << 24);
        write(kMethodNv4097SetBeginEnd, 0);
    }

    // The four corners of the quad as a strip: bottom-left, bottom-right, top-left, top-right.
    void putQuadPositions() {
        constexpr float kCorners[4][3] = {{-0.5f, -0.5f, 0}, {0.5f, -0.5f, 0}, {-0.5f, 0.5f, 0}, {0.5f, 0.5f, 0}};
        for (std::uint32_t v = 0; v < 4; v++) {
            for (std::uint32_t c = 0; c < 3; c++)
                memory_.putFloat(1, kPositionsOffset + v * 12 + c * 4, kCorners[v][c]);
        }
    }

    void putQuadColors() {
        constexpr std::uint32_t kColors[4] = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF,
                                              0xFFFFFFFF};  // bytes A R G B in memory
        for (std::uint32_t v = 0; v < 4; v++) memory_.putWord(1, kColorsOffset + v * 4, kColors[v]);
    }

    void putQuadTexCoords() {
        constexpr float kCoords[4][2] = {{0, 1}, {1, 1}, {0, 0}, {1, 0}};
        for (std::uint32_t v = 0; v < 4; v++) {
            for (std::uint32_t c = 0; c < 2; c++) memory_.putFloat(1, kTexCoordsOffset + v * 8 + c * 4, kCoords[v][c]);
        }
    }

    // Unit 0 samples the 2x2 texture in local memory, linear layout, no mipmaps.
    void setTexture() {
        for (std::uint32_t i = 0; i < kTexturePixels.size(); i++) {
            memory_.putWord(0, kTextureOffset + (i / kTextureSize) * (kTextureSize * 4) + (i % kTextureSize) * 4,
                            kTexturePixels[i]);
        }
        bindTexture(kTextureOffset, kTextureSize, kTextureSize);
    }

    // Unit 0 reads an A8R8G8B8 linear texture of `width` x `height` at `offset` in local memory; with an
    // offset of a render target it aliases the target. Identity remap, NEAREST filters, WRAP addressing.
    // A pitch of 0 means width * 4; `format_flags` adds bits to the format field (0x40 = unnormalized).
    void bindTexture(std::uint32_t offset, std::uint32_t width, std::uint32_t height, std::uint32_t pitch = 0,
                     std::uint32_t format_flags = 0) {
        constexpr std::uint32_t kFormatA8R8G8B8Linear = 0x85 | 0x20;
        write(kMethodNv4097SetTextureOffset, offset);
        write(kMethodNv4097SetTextureFormat,
              1u | (2u << 4) | ((kFormatA8R8G8B8Linear | format_flags) << 8) | (1u << 16));
        write(kMethodNv4097SetTextureControl0, 0x80000000);
        write(kMethodNv4097SetTextureControl1, 0xAAE4);
        write(kMethodNv4097SetTextureFilter, (1u << 16) | (1u << 24));
        write(kMethodNv4097SetTextureAddress, 1u | (1u << 8) | (1u << 16));
        write(kMethodNv4097SetTextureImageRect, (width << 16) | height);
        write(kMethodNv4097SetTextureControl3, pitch != 0 ? pitch : width * 4);
    }

    // Ends the frame with a flip of buffer 0 and returns the file image.
    std::vector<std::uint8_t> finish() {
        RsxDisplayBuffer display;
        display.offset = kSurfaceOffset;
        display.pitch = kSurfacePitch;
        display.width = kSurfaceSize;
        display.height = kSurfaceSize;
        builder_.onFlip(*regs_, 0, display);
        writer_->onFlip(0);
        std::array<RsxDisplayBuffer, kRsxDisplayBufferCount> displays{};
        displays[0] = display;
        recording_.setWriter(nullptr);
        return writer_->finish(displays, 1).flatten();
    }

private:
    std::unique_ptr<RsxRegisters> regs_;
    SyntheticMemory memory_;
    RsxRecordingMemoryView recording_;
    RsxFrameBuilder builder_;
    std::unique_ptr<RsxCaptureWriter> writer_;
};

// Clear to `color_argb`, flip.
inline std::vector<std::uint8_t> makeClearCapture(std::uint32_t color_argb = kClearColor) {
    SceneBuilder scene;
    scene.setSurface();
    scene.clear(color_argb);
    return scene.finish();
}

// Clear to black, then a quad whose corners carry red, green, blue and white (slot 0 positions, slot 3
// colours), flip.
inline std::vector<std::uint8_t> makeQuadCapture(const Programs& programs = {}) {
    SceneBuilder scene;
    scene.setSurface();
    scene.clear(0xFF000000);
    scene.putQuadPositions();
    scene.putQuadColors();
    scene.setFloatAttrib(0, 3, kPositionsOffset);
    scene.setColorAttrib(3, kColorsOffset);
    scene.setPrograms(programs);
    scene.drawStrip(4);
    return scene.finish();
}

// Clear to black, then the quad textured with the 2x2 texture (slot 0 positions, slot 8 texture
// coordinates, unit 0), flip.
inline std::vector<std::uint8_t> makeTexturedQuadCapture(const Programs& programs = {}) {
    SceneBuilder scene;
    scene.setSurface();
    scene.clear(0xFF000000);
    scene.putQuadPositions();
    scene.putQuadTexCoords();
    scene.setFloatAttrib(0, 3, kPositionsOffset);
    scene.setFloatAttrib(8, 2, kTexCoordsOffset);
    scene.setTexture();
    scene.setPrograms(programs);
    scene.drawStrip(4);
    return scene.finish();
}

}  // namespace dal::rsx_synthetic
