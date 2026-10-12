#pragma once

// Hook for drawing over the presented swapchain image (the debug overlay). The renderer owns the present
// barriers and calls this only between them, so an implementation never sees frame dumps or captures.

#include <cstdint>
#include <filesystem>
#include <optional>

#include "render/frame_dump.h"
#include "render/render_scale.h"
#include "render/vk_window.h"

namespace tox2 {

struct RendererStats;  // vk_renderer.h

struct PresentFrameInfo {
    const RendererStats* stats = nullptr;       // stats->flips already counts this flip
    DumpCounts dumps;                           // the dump worker's counts (--dump-frames and requested dumps)
    std::filesystem::path last_requested_dump;  // empty until a requested dump was queued
    std::uint32_t resolution_scale = 0;         // the renderer's current internal resolution scale; 0 = unknown
    std::optional<AaMode> aa_mode;              // the renderer's effective anti-aliasing mode; unset = unknown
    bool msaa_available = false;                // the device can draw 2-sample surfaces (AaMode::Msaa is selectable)
};

struct PresentOverlayRequests {
    bool dump_display = false;  // read back this flip and write it to RenderOptions::requested_dump_dir
    // New internal resolution scale; 0 = no change. The renderer applies it after the flip is submitted, so
    // a frame is never recorded at two scales.
    std::uint32_t resolution_scale = 0;
    // New anti-aliasing mode; unset = no change. Applied after the flip is submitted, like the scale; the next pass
    // over an anti-aliased surface uses it.
    std::optional<AaMode> aa_mode;
};

// Drawn over the presented swapchain image only. Never called without a presenter, so it cannot reach
// frame dumps, readDisplay or captures. All calls come from the renderer's thread.
class PresentOverlay {
public:
    virtual ~PresentOverlay() = default;
    // Once per flip with a presenter, before the flip's readback decision (also when no image is acquired).
    virtual PresentOverlayRequests onFlip(const PresentFrameInfo& info) = 0;
    // After the blit into an acquired image; false skips record() and its barriers.
    virtual bool wantsDraw() const = 0;
    // `target.image` is in COLOR_ATTACHMENT_OPTIMAL and must be left so; target.view/format are valid.
    virtual void record(VkCommandBuffer cmd, const Presenter::Target& target) = 0;
};

}  // namespace tox2
