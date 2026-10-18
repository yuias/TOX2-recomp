#pragma once

// Several overlays in the renderer's one PresentOverlay slot, drawn in insertion order (later = on top).

#include <vector>

#include "render/present_overlay.h"

namespace tox2 {

class OverlayStack final : public PresentOverlay {
public:
    // Not owned; add every overlay before the renderer uses the stack.
    void add(PresentOverlay* overlay) {
        if (overlay != nullptr) overlays_.push_back(overlay);
    }

    // Every child sees every flip. A field set by a later child wins, so the top overlay decides conflicts.
    // A field added to PresentOverlayRequests must be added to the merge below, or it is silently dropped.
    PresentOverlayRequests onFlip(const PresentFrameInfo& info) override {
        PresentOverlayRequests merged;
        for (PresentOverlay* overlay : overlays_) {
            const PresentOverlayRequests requests = overlay->onFlip(info);
            merged.dump_display = merged.dump_display || requests.dump_display;
            if (requests.resolution_scale != 0) merged.resolution_scale = requests.resolution_scale;
            if (requests.aa_mode) merged.aa_mode = requests.aa_mode;
            if (requests.anisotropy != 0) merged.anisotropy = requests.anisotropy;
            if (requests.window_size) merged.window_size = requests.window_size;
            if (requests.frame_gen) merged.frame_gen = requests.frame_gen;
        }
        return merged;
    }

    bool wantsDraw() const override {
        for (const PresentOverlay* overlay : overlays_) {
            if (overlay->wantsDraw()) return true;
        }
        return false;
    }

    void record(VkCommandBuffer cmd, const Presenter::Target& target) override {
        for (PresentOverlay* overlay : overlays_) {
            if (overlay->wantsDraw()) overlay->record(cmd, target);
        }
    }

private:
    std::vector<PresentOverlay*> overlays_;
};

}  // namespace tox2
