#pragma once

// The save data dialog drawn over the presented image. It owns an ImGui context of its own (used only under
// ImGuiContextScope, imgui_context.h) and draws the latest snapshot of a SaveDialogBoard with the foreground draw
// list: a 1280x720 canvas mapped onto the game's 16:9 image area. It takes no window input; the dialog is driven by
// the pad through the interactive driver.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <vector>

#include "hle/save_dialog.h"
#include "imgui.h"
#include "overlay/ui_font.h"
#include "render/present_overlay.h"
#include "render/vk_context.h"
#include "render/vk_window.h"

struct ImGuiContext;

namespace tox2 {

struct SaveDialogOverlayOptions {
    std::vector<std::filesystem::path> font_candidates;  // uiFontCandidates(...)
};

class SaveDialogOverlay final : public PresentOverlay {
public:
    // Sets board.drawer() to Ready; touches no Vulkan or ImGui state until the first record().
    SaveDialogOverlay(VkContext& context, SaveDialogBoard& board, SaveDialogOverlayOptions options);
    // Waits for the device, shuts the backend down and sets board.drawer() to None, so a dialog that is still
    // waiting is answered as cancelled instead of waiting for a drawer that is gone.
    ~SaveDialogOverlay() override;
    SaveDialogOverlay(const SaveDialogOverlay&) = delete;
    SaveDialogOverlay& operator=(const SaveDialogOverlay&) = delete;

    PresentOverlayRequests onFlip(const PresentFrameInfo&) override { return {}; }
    bool wantsDraw() const override;         // the board holds a dialog and init has not failed
    bool wantsIdlePresent() const override;  // same as wantsDraw()
    void record(VkCommandBuffer cmd, const Presenter::Target& target) override;

    // Initialisation failed; the overlay stays off for the rest of the run.
    bool failed() const { return failed_.load(std::memory_order_relaxed); }
    // Number of font atlas rebuilds so far. Renderer's thread; for tests.
    int atlasBuildCount() const { return atlas_build_count_; }

private:
    bool initialize(const Presenter::Target& target);
    const char* setUpContext();
    void shutdown();
    void fail(const char* what);
    // Renderer's thread, under the context scope. False after a failure (the overlay is then shut down).
    bool ensureAtlas(const SaveDialogContent& content, std::uint32_t scene_height);
    bool rebuildAtlas(const std::set<ImWchar>& code_points, std::uint32_t scene_height);

    VkContext& context_;
    SaveDialogBoard& board_;
    SaveDialogOverlayOptions options_;

    std::atomic<bool> failed_{false};

    // Renderer's thread only (the backend holds pointers into these).
    ImGuiContext* imgui_ = nullptr;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkFormat color_format_ = VK_FORMAT_UNDEFINED;
    bool initialized_ = false;
    bool vulkan_backend_ = false;
    std::chrono::steady_clock::time_point last_frame_time_{};

    // The atlas bakes a fixed glyph set per pixel size, so it is rebuilt when a dialog needs glyphs it lacks or the
    // image area changes size.
    std::optional<UiFontFile> font_;  // must outlive the atlas: the atlas does not own the bytes
    ImVector<ImWchar> glyph_ranges_;  // must stay valid while the atlas holds the fonts
    std::set<ImWchar> baked_;
    std::uint32_t baked_scene_height_ = 0;  // 0: no atlas built yet
    std::uint64_t checked_content_id_ = 0;
    int atlas_build_count_ = 0;
};

}  // namespace tox2
