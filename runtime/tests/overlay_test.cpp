// Self-checking tests for the debug overlay library. The model and panel tests need no GPU or window; the
// overlay tests draw into an offscreen image on the first Vulkan device. The only window they create is a
// hidden one, to give the Win32 backend a handle.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "overlay/debug_overlay.h"
#include "overlay/overlay_model.h"
#include "overlay/overlay_panels.h"
#include "overlay/overlay_sources.h"
#include "rsx/rsx_unimplemented.h"

namespace {

int g_passed;
int g_failed;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            g_passed++;                                                            \
        } else {                                                                   \
            g_failed++;                                                            \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
        }                                                                          \
    } while (0)

using Clock = tox2::FrameStatsHistory::Clock;

Clock::time_point at(double seconds) {
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds)) +
                             std::chrono::seconds(100));
}

void testSteadyFlips() {
    tox2::FrameStatsHistory history;
    tox2::RendererStats stats;
    for (int i = 0; i <= 60; ++i) {
        stats.flips = static_cast<std::uint64_t>(i + 1);
        history.onFlip(stats, at(i / 60.0));
    }
    const Clock::time_point last = at(1.0);
    CHECK(history.flip() == 61);
    CHECK(std::fabs(history.flipsPerSecond(last) - 60.0) < 0.01);
    CHECK(history.flipsPerSecond(last + std::chrono::seconds(2)) == 0.0);
    CHECK(history.frameTimeCount() == 60);
    CHECK(history.frameTimeOffset() == 0);
    bool steady = true;
    for (std::size_t i = 0; i < history.frameTimeCount(); ++i) {
        steady = steady && std::fabs(history.frameTimesMs()[i] - 16.667f) < 0.01f;
    }
    CHECK(steady);
    CHECK(std::fabs(history.maxFrameTimeMs() - 16.667f) < 0.01f);
}

void testRingWraps() {
    tox2::FrameStatsHistory history;
    tox2::RendererStats stats;
    for (int i = 0; i < 300; ++i) {
        stats.flips = static_cast<std::uint64_t>(i + 1);
        // The last interval is the longest, so the maximum has to come from the newest slot.
        history.onFlip(stats, at(i * 0.01 + (i == 299 ? 0.05 : 0.0)));
    }
    CHECK(history.frameTimeCount() == tox2::FrameStatsHistory::kSamples);
    // 299 intervals were written; the next slot to overwrite (the oldest) is 299 % 240.
    CHECK(history.frameTimeOffset() == 299 % tox2::FrameStatsHistory::kSamples);
    CHECK(std::fabs(history.frameTimesMs()[(history.frameTimeOffset() + 239) % 240] - 60.0f) < 0.01f);
    CHECK(std::fabs(history.maxFrameTimeMs() - 60.0f) < 0.01f);
    CHECK(history.flip() == 300);
}

void testDrawDeltas() {
    tox2::FrameStatsHistory history;
    tox2::RendererStats stats;
    stats.flips = 5;
    stats.draws = 40;
    stats.draws_skipped = 3;
    history.onFlip(stats, at(0.0));
    // The first call reports everything so far.
    CHECK(history.drawsLastFlip() == 40);
    CHECK(history.skippedLastFlip() == 3);
    CHECK(history.skippedTotal() == 3);

    stats.flips = 6;
    stats.draws = 52;
    stats.draws_skipped = 4;
    history.onFlip(stats, at(0.016));
    CHECK(history.drawsLastFlip() == 12);
    CHECK(history.skippedLastFlip() == 1);
    CHECK(history.skippedTotal() == 4);
    CHECK(history.flip() == 6);
}

void testRefresh() {
    int audio_calls = 0;
    int unimplemented_calls = 0;
    int limit_calls = 0;
    std::uint64_t skipped = 0;
    std::uint64_t underruns = 0;

    tox2::OverlaySources sources;
    sources.audio = [&] {
        ++audio_calls;
        tox2::OverlayAudioStats stats;
        stats.output = "fake";
        stats.skipped_blocks = skipped;
        stats.underrun_frames = underruns;
        return stats;
    };
    sources.unimplemented = [&] {
        ++unimplemented_calls;
        return std::vector<std::pair<std::string, std::uint64_t>>{{"state: x", 2}};
    };
    sources.unimplemented_dropped = [] { return std::uint64_t{7}; };
    sources.flip_limit = [&] {
        ++limit_calls;
        return false;
    };
    sources.log_level = [] { return tox2::LogLevel::Warn; };
    sources.capture_status = [] {
        tox2::RsxCaptureStatus status;
        status.available = true;
        status.last_frame = 9;
        return status;
    };

    tox2::OverlayState state;
    CHECK(state.flip_limit);
    CHECK(state.log_level == tox2::LogLevel::Info);

    state.refresh(sources, at(0.0));
    CHECK(audio_calls == 1);
    CHECK(unimplemented_calls == 1);
    CHECK(state.audio.output == "fake");
    CHECK(state.skipped_per_s == 0.0);
    CHECK(state.unimplemented.size() == 1 && state.unimplemented[0].second == 2);
    CHECK(state.unimplemented_dropped == 7);
    CHECK(!state.flip_limit);
    CHECK(state.log_level == tox2::LogLevel::Warn);
    CHECK(state.capture.available && state.capture.last_frame == 9);

    // Inside the audio interval: the cheap providers still run, the audio one does not.
    skipped = 10;
    state.refresh(sources, at(0.1));
    CHECK(audio_calls == 1);
    CHECK(unimplemented_calls == 1);
    CHECK(limit_calls == 2);

    skipped = 5;
    underruns = 480;
    state.refresh(sources, at(0.25));
    CHECK(audio_calls == 2);
    CHECK(unimplemented_calls == 1);
    CHECK(std::fabs(state.skipped_per_s - 20.0) < 1e-6);
    CHECK(std::fabs(state.underrun_frames_per_s - 1920.0) < 1e-6);

    state.refresh(sources, at(0.5));
    CHECK(audio_calls == 3);
    CHECK(unimplemented_calls == 2);
    CHECK(state.skipped_per_s == 0.0);

    // Empty sources leave the cached values alone.
    state.refresh(tox2::OverlaySources{}, at(5.0));
    CHECK(state.audio.output == "fake");
    CHECK(state.unimplemented_dropped == 7);
}

tox2::OverlaySources makeFakeSources() {
    tox2::OverlaySources sources;
    sources.audio = [] {
        tox2::OverlayAudioStats stats;
        stats.output = "fake";
        stats.max_producer_ms = 1.5;
        stats.mean_fill_frames = 1500.0;
        return stats;
    };
    sources.unimplemented = [] { return std::vector<std::pair<std::string, std::uint64_t>>{{"state: x", 2}}; };
    sources.unimplemented_dropped = [] { return std::uint64_t{1}; };
    sources.flip_limit = [] { return true; };
    sources.set_flip_limit = [](bool) {};
    sources.log_level = [] { return tox2::LogLevel::Info; };
    sources.set_log_level = [](tox2::LogLevel) {};
    sources.request_capture = [] { return true; };
    sources.capture_status = [] {
        tox2::RsxCaptureStatus status;
        status.available = true;
        status.last_frame = 4;
        status.last_path = "captures/frame_4.tox2cap";
        return status;
    };
    return sources;
}

// Runs three frames of the overlay window in a private ImGui context and returns whether the last one
// produced geometry. The Vulkan backend is not involved.
bool drawFrames(tox2::OverlayState& state, const tox2::OverlaySources& sources) {
    ImGuiContext* context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.Fonts->Build();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);

    bool drew = false;
    for (int i = 0; i < 3; ++i) {
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        tox2::drawOverlayWindow(state, sources);
        ImGui::Render();
        drew = ImGui::GetDrawData() != nullptr && ImGui::GetDrawData()->TotalVtxCount > 0;
    }
    CHECK(ImGui::FindWindowByName("tox2") != nullptr);
    ImGui::DestroyContext(context);
    return drew;
}

void testPanels() {
    tox2::OverlayState state;
    tox2::RendererStats stats;
    for (int i = 0; i < 5; ++i) {
        stats.flips = static_cast<std::uint64_t>(i + 1);
        stats.draws = static_cast<std::uint64_t>(10 * (i + 1));
        stats.draws_skipped = static_cast<std::uint64_t>(i);
        state.frames.onFlip(stats, at(i / 60.0));
    }
    const tox2::OverlaySources sources = makeFakeSources();
    state.refresh(sources, at(1.0));
    state.dumps.written = 2;
    state.last_dump = "frames/frame_000002.png";
    CHECK(drawFrames(state, sources));

    // The last frame ran through every section with data; clicks are not simulated, so no request is set.
    CHECK(!state.dump_requested);

    // No providers at all: every section falls back to its placeholder.
    tox2::OverlayState empty_state;
    CHECK(drawFrames(empty_state, tox2::OverlaySources{}));
}

// Drives the real Settings panel in a private ImGui context with simulated mouse input. An item is found by
// moving the pointer down the window until ImGui reports it hovered and enabled.
class SettingsProbe {
public:
    SettingsProbe(tox2::OverlayState& state, const tox2::OverlaySources& sources) : state_(state), sources_(sources) {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.Fonts->Build();
        io.DisplaySize = ImVec2(1280.0f, 720.0f);
        frame();
        frame();
        window_ = ImGui::FindWindowByName("tox2");
        CHECK(window_ != nullptr);
        if (window_ == nullptr) return;
        const std::optional<ImVec2> header = findHovered(window_->GetID("Settings"), 60.0f, 10.0f, 720.0f);
        CHECK(header.has_value());
        if (header) click(*header);
        // The window sized itself to the closed sections; give the opened one room.
        ImGui::SetWindowSize(window_, ImVec2(440.0f, 700.0f));
        frame();
        frame();
    }

    ~SettingsProbe() { ImGui::DestroyContext(context_); }

    SettingsProbe(const SettingsProbe&) = delete;
    SettingsProbe& operator=(const SettingsProbe&) = delete;

    void frame() {
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        tox2::drawOverlayWindow(state_, sources_);
        ImGui::Render();
    }

    // The pointer position over the item with this id, or nothing.
    std::optional<ImVec2> findHovered(ImGuiID id, float x, float y_begin, float y_end) {
        for (float y = y_begin; y < y_end; y += 2.0f) {
            ImGui::GetIO().AddMousePosEvent(x, y);
            frame();
            frame();
            const ImGuiContext& g = *ImGui::GetCurrentContext();
            if (g.HoveredId == id && !g.HoveredIdIsDisabled) return ImVec2(x, y);
        }
        return std::nullopt;
    }

    void click(ImVec2 at) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(at.x, at.y);
        frame();
        io.AddMouseButtonEvent(0, true);
        frame();
        frame();
        io.AddMouseButtonEvent(0, false);
        frame();
        frame();
    }

    ImGuiWindow* window() const { return window_; }

private:
    tox2::OverlayState& state_;
    const tox2::OverlaySources& sources_;
    ImGuiContext* context_ = nullptr;
    ImGuiWindow* window_ = nullptr;
};

void testResolutionScalePanel() {
    const tox2::OverlaySources sources = makeFakeSources();
    constexpr float kProbeX = 60.0f;
    constexpr float kWindowBottom = 720.0f;

    {
        // No scale reported: the combo is disabled and nothing can be requested.
        tox2::OverlayState state;
        SettingsProbe probe(state, sources);
        if (probe.window() != nullptr) {
            const ImGuiID id = probe.window()->GetID("Internal resolution scale");
            CHECK(!probe.findHovered(id, kProbeX, 10.0f, kWindowBottom));
        }
        CHECK(state.resolution_scale_request == 0);
    }

    {
        // The combo is enabled, so it sits outside the group of unimplemented settings.
        tox2::OverlayState state;
        state.resolution_scale = 2;
        SettingsProbe probe(state, sources);
        if (probe.window() == nullptr) return;
        const ImGuiID id = probe.window()->GetID("Internal resolution scale");
        const std::optional<ImVec2> combo = probe.findHovered(id, kProbeX, 10.0f, kWindowBottom);
        CHECK(combo.has_value());
        if (!combo) return;
        CHECK(state.resolution_scale_request == 0);

        probe.click(*combo);
        ImGuiContext& g = *ImGui::GetCurrentContext();
        CHECK(g.OpenPopupStack.Size == 1);
        if (g.OpenPopupStack.Size != 1 || g.OpenPopupStack[0].Window == nullptr) return;
        ImGuiWindow* popup = g.OpenPopupStack[0].Window;
        const ImGuiID entry = popup->GetID("3x");
        const std::optional<ImVec2> three =
                probe.findHovered(entry, popup->Pos.x + 10.0f, popup->Pos.y, popup->Pos.y + popup->Size.y);
        CHECK(three.has_value());
        if (!three) return;
        probe.click(*three);
        CHECK(state.resolution_scale_request == 3);
        // The request is taken once; the renderer reports the scale it applied on a later flip.
        const tox2::PresentOverlayRequests requests = tox2::takePresentRequests(state);
        CHECK(requests.resolution_scale == 3);
        CHECK(state.resolution_scale_request == 0);
    }
}

void testPresentRequests() {
    tox2::OverlayState state;
    state.dump_requested = true;
    state.resolution_scale_request = 3;
    tox2::PresentOverlayRequests requests = tox2::takePresentRequests(state);
    CHECK(requests.dump_display);
    CHECK(requests.resolution_scale == 3);
    CHECK(!state.dump_requested);
    CHECK(state.resolution_scale_request == 0);

    requests = tox2::takePresentRequests(state);
    CHECK(!requests.dump_display);
    CHECK(requests.resolution_scale == 0);

    // The reported scale is a status, not a request, so taking requests leaves it alone.
    state.resolution_scale = 2;
    requests = tox2::takePresentRequests(state);
    CHECK(requests.resolution_scale == 0);
    CHECK(state.resolution_scale == 2);
}

void testRuntimeSources() {
    const tox2::OverlaySources sources = tox2::makeRuntimeOverlaySources();
    CHECK(sources.audio && sources.unimplemented && sources.unimplemented_dropped && sources.flip_limit &&
          sources.set_flip_limit && sources.log_level && sources.set_log_level && sources.request_capture &&
          sources.capture_status);
    if (!(sources.audio && sources.unimplemented && sources.unimplemented_dropped && sources.flip_limit &&
          sources.set_flip_limit && sources.log_level && sources.set_log_level && sources.request_capture &&
          sources.capture_status)) {
        return;
    }

    const bool limit = sources.flip_limit();
    sources.set_flip_limit(!limit);
    CHECK(sources.flip_limit() == !limit);
    sources.set_flip_limit(limit);
    CHECK(sources.flip_limit() == limit);

    tox2::rsxUnimplementedReset();
    tox2::rsxNoteUnimplemented("state", "overlay test");
    const auto counts = sources.unimplemented();
    CHECK(counts.size() == 1 && counts[0].first == "state overlay test" && counts[0].second == 1);
    CHECK(sources.unimplemented_dropped() == 0);
    tox2::rsxUnimplementedReset();

    // No audio engine and no RSX thread run in this process.
    CHECK(sources.audio().output.empty());
    CHECK(!sources.capture_status().available);
    CHECK(!sources.request_capture());

    const tox2::LogLevel level = sources.log_level();
    sources.set_log_level(tox2::LogLevel::Warn);
    CHECK(sources.log_level() == tox2::LogLevel::Warn);
    sources.set_log_level(tox2::LogLevel::Debug);
    CHECK(sources.log_level() == tox2::LogLevel::Debug);
    sources.set_log_level(level);
    CHECK(sources.log_level() == level);
}

// Raw Win32 key message values, so the test needs no windows.h.
constexpr std::uint32_t kWmKeyDown = 0x0100;
constexpr std::uint32_t kWmKeyUp = 0x0101;
constexpr std::int64_t kRepeatBit = std::int64_t{1} << 30;

tox2::WindowMessage keyMessage(std::uint32_t message, std::uint32_t scan_code, bool repeat = false) {
    tox2::WindowMessage result;
    result.message = message;
    result.wparam = 0x70;  // the virtual key is only a fallback when the scan code is zero
    result.lparam = static_cast<std::int64_t>(scan_code) << 16 | (repeat ? kRepeatBit : 0);
    return result;
}

// The overlay needs a VkContext only to be constructed; the key path touches neither Vulkan nor ImGui.
void testToggleKey(tox2::VkContext& ctx) {
    tox2::DebugOverlay overlay(ctx, tox2::OverlaySources{});
    const std::uint32_t f1 = tox2::kOverlayToggleScanCode;
    CHECK(!overlay.visible() && !overlay.wantsDraw());

    tox2::MessageRoute route = overlay.onWindowMessage(keyMessage(kWmKeyDown, f1));
    CHECK(overlay.visible() && overlay.wantsDraw() && route.withhold_key && !route.handled);

    // Auto-repeat neither toggles nor reaches the pad.
    route = overlay.onWindowMessage(keyMessage(kWmKeyDown, f1, true));
    CHECK(overlay.visible() && route.withhold_key);
    route = overlay.onWindowMessage(keyMessage(kWmKeyUp, f1));
    CHECK(overlay.visible() && route.withhold_key);

    route = overlay.onWindowMessage(keyMessage(kWmKeyDown, f1));
    CHECK(!overlay.visible() && route.withhold_key);
    overlay.onWindowMessage(keyMessage(kWmKeyUp, f1));

    // Other keys are never withheld here: the overlay is hidden or has no window input yet.
    constexpr std::uint32_t kScanZ = 0x2C;
    CHECK(!overlay.onWindowMessage(keyMessage(kWmKeyDown, kScanZ)).withhold_key);
    CHECK(!overlay.onWindowMessage(keyMessage(kWmKeyUp, kScanZ)).withhold_key);
    overlay.setVisible(true);
    CHECK(!overlay.onWindowMessage(keyMessage(kWmKeyDown, kScanZ)).withhold_key);
    CHECK(!overlay.onWindowMessage(keyMessage(kWmKeyUp, kScanZ)).withhold_key);
}

constexpr std::uint32_t kTargetWidth = 1280;
constexpr std::uint32_t kTargetHeight = 720;

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                  VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

// An offscreen stand-in for a swapchain image, with a host buffer to read it back.
class OffscreenTarget {
public:
    explicit OffscreenTarget(tox2::VkContext& ctx) : ctx_(ctx) {
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_B8G8R8A8_UNORM;
        image_info.extent = {kTargetWidth, kTargetHeight, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage =
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo image_alloc_info{};
        image_alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        ok_ = vmaCreateImage(ctx.allocator(), &image_info, &image_alloc_info, &image_, &image_alloc_, nullptr) ==
              VK_SUCCESS;

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = image_;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_B8G8R8A8_UNORM;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ok_ = ok_ && vkCreateImageView(ctx.device(), &view_info, nullptr, &view_) == VK_SUCCESS;

        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = static_cast<VkDeviceSize>(kTargetWidth) * kTargetHeight * 4;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo buffer_alloc_info{};
        buffer_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        buffer_alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo mapped{};
        ok_ = ok_ && vmaCreateBuffer(ctx.allocator(), &buffer_info, &buffer_alloc_info, &buffer_, &buffer_alloc_,
                                     &mapped) == VK_SUCCESS;
        pixels_ = static_cast<const std::uint8_t*>(mapped.pMappedData);

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = ctx.graphicsQueueFamily();
        ok_ = ok_ && vkCreateCommandPool(ctx.device(), &pool_info, nullptr, &pool_) == VK_SUCCESS;
        VkCommandBufferAllocateInfo cmd_info{};
        cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_info.commandPool = pool_;
        cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_info.commandBufferCount = 1;
        ok_ = ok_ && vkAllocateCommandBuffers(ctx.device(), &cmd_info, &cmd_) == VK_SUCCESS;
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        ok_ = ok_ && vkCreateFence(ctx.device(), &fence_info, nullptr, &fence_) == VK_SUCCESS;
        ok_ = ok_ && pixels_ != nullptr;
    }

    ~OffscreenTarget() {
        vkDeviceWaitIdle(ctx_.device());
        if (fence_ != VK_NULL_HANDLE) vkDestroyFence(ctx_.device(), fence_, nullptr);
        if (pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(ctx_.device(), pool_, nullptr);
        if (buffer_ != VK_NULL_HANDLE) vmaDestroyBuffer(ctx_.allocator(), buffer_, buffer_alloc_);
        if (view_ != VK_NULL_HANDLE) vkDestroyImageView(ctx_.device(), view_, nullptr);
        if (image_ != VK_NULL_HANDLE) vmaDestroyImage(ctx_.allocator(), image_, image_alloc_);
    }

    OffscreenTarget(const OffscreenTarget&) = delete;
    OffscreenTarget& operator=(const OffscreenTarget&) = delete;

    bool ok() const { return ok_; }

    // One submit that follows the renderer's present path: clear to black (the blit), the barriers around
    // the overlay, then a copy into the host buffer instead of the present.
    bool drawOnce(tox2::DebugOverlay& overlay, const tox2::RendererStats& stats) {
        tox2::Presenter::Target target;
        target.image = image_;
        target.view = view_;
        target.format = VK_FORMAT_B8G8R8A8_UNORM;
        target.width = kTargetWidth;
        target.height = kTargetHeight;

        vkResetCommandBuffer(cmd_, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd_, &begin);

        imageBarrier(cmd_, image_, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue black{};
        const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd_, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);

        tox2::PresentFrameInfo info;
        info.stats = &stats;
        overlay.onFlip(info);
        imageBarrier(cmd_, image_, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        overlay.record(cmd_, target);
        imageBarrier(cmd_, image_, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kTargetWidth, kTargetHeight, 1};
        vkCmdCopyImageToBuffer(cmd_, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer_, 1, &region);
        VkMemoryBarrier2 host_barrier{};
        host_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        host_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        host_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        host_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        host_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &host_barrier;
        vkCmdPipelineBarrier2(cmd_, &dependency);
        if (vkEndCommandBuffer(cmd_) != VK_SUCCESS) return false;

        VkCommandBufferSubmitInfo cmd_submit{};
        cmd_submit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_submit.commandBuffer = cmd_;
        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmd_submit;
        if (vkQueueSubmit2(ctx_.graphicsQueue(), 1, &submit, fence_) != VK_SUCCESS) return false;
        const bool done = vkWaitForFences(ctx_.device(), 1, &fence_, VK_TRUE, 10'000'000'000ull) == VK_SUCCESS;
        vkResetFences(ctx_.device(), 1, &fence_);
        vmaInvalidateAllocation(ctx_.allocator(), buffer_alloc_, 0, VK_WHOLE_SIZE);
        return done;
    }

    // True when any pixel of the rectangle (inclusive) has a non-zero colour channel.
    bool anyLit(std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1) const {
        for (std::uint32_t y = y0; y <= y1; ++y) {
            for (std::uint32_t x = x0; x <= x1; ++x) {
                const std::uint8_t* p = pixels_ + (static_cast<std::size_t>(y) * kTargetWidth + x) * 4;
                if (p[0] != 0 || p[1] != 0 || p[2] != 0) return true;
            }
        }
        return false;
    }

private:
    tox2::VkContext& ctx_;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation image_alloc_ = nullptr;
    VkImageView view_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation buffer_alloc_ = nullptr;
    const std::uint8_t* pixels_ = nullptr;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    bool ok_ = false;
};

// Records the overlay over two consecutive submits, then checks that the window's title bar is lit and
// the opposite corner untouched. The probes follow the panel's window position (10, 10) and width (440).
void drawTwice(tox2::VkContext& ctx, OffscreenTarget& target) {
    tox2::DebugOverlay overlay(ctx, makeFakeSources());
    overlay.attachWindow(nullptr);
    overlay.setVisible(true);
    CHECK(overlay.wantsDraw());

    tox2::RendererStats stats;
    stats.flips = 1;
    stats.draws = 10;
    CHECK(target.drawOnce(overlay, stats));
    CHECK(!overlay.failed());
    // ImGui keeps a new window hidden for its first frame, so the title bar shows from the second one.
    CHECK(!target.anyLit(1270, 710, 1270, 710));

    stats.flips = 2;
    CHECK(target.drawOnce(overlay, stats));
    CHECK(!overlay.failed());
    CHECK(target.anyLit(10, 10, 200, 40));
    CHECK(!target.anyLit(1270, 710, 1270, 710));
}

tox2::DebugOverlay* g_forward_overlay = nullptr;
WNDPROC g_forward_previous = nullptr;
int g_forwarded_capture_changes = 0;

// Routes messages to the overlay first, as the game window's procedure does.
LRESULT CALLBACK forwardingProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (g_forward_overlay != nullptr) {
        if (message == WM_CAPTURECHANGED) g_forwarded_capture_changes++;
        const tox2::MessageRoute route = g_forward_overlay->onWindowMessage(
                {hwnd, message, static_cast<std::uint64_t>(wparam), static_cast<std::int64_t>(lparam)});
        if (route.handled) return static_cast<LRESULT>(route.result);
    }
    return CallWindowProcW(g_forward_previous, hwnd, message, wparam, lparam);
}

// Feeds the overlay Win32 input through a hidden window: the message hook has to reach the ImGui context
// that record() created, with the real backend initialised for that window.
void testWin32Input(tox2::VkContext& ctx, OffscreenTarget& target) {
    HWND hwnd =
            CreateWindowExW(0, L"STATIC", L"overlay test", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    CHECK(hwnd != nullptr);
    if (hwnd == nullptr) return;
    {
        tox2::DebugOverlay overlay(ctx, makeFakeSources());
        overlay.attachWindow(hwnd);
        overlay.setVisible(true);
        tox2::RendererStats stats;
        stats.flips = 1;
        CHECK(target.drawOnce(overlay, stats));
        CHECK(!overlay.failed());

        tox2::WindowMessage move;
        move.hwnd = hwnd;
        move.message = WM_MOUSEMOVE;
        move.lparam = (15 << 16) | 20;
        overlay.onWindowMessage(move);
        // WantCaptureMouse comes from the window hovered in the previous frame, so it lags by one frame.
        stats.flips = 2;
        CHECK(target.drawOnce(overlay, stats));
        stats.flips = 3;
        CHECK(target.drawOnce(overlay, stats));
        CHECK(ImGui::GetIO().WantCaptureMouse);

        // Hiding and showing again must leave ImGui focused, or it would drop all input.
        overlay.onWindowMessage(keyMessage(kWmKeyDown, tox2::kOverlayToggleScanCode));
        overlay.onWindowMessage(keyMessage(kWmKeyUp, tox2::kOverlayToggleScanCode));
        CHECK(!overlay.visible());
        overlay.onWindowMessage(keyMessage(kWmKeyDown, tox2::kOverlayToggleScanCode));
        overlay.onWindowMessage(keyMessage(kWmKeyUp, tox2::kOverlayToggleScanCode));
        CHECK(overlay.visible());
        overlay.onWindowMessage(move);
        stats.flips = 4;
        CHECK(target.drawOnce(overlay, stats));
        CHECK(ImGui::GetIO().WantCaptureMouse);

        // A click goes through the window procedure like in the game. The button-up makes the handler call
        // ReleaseCapture, which sends WM_CAPTURECHANGED to the same window procedure before the handler returns.
        g_forward_overlay = &overlay;
        const LONG_PTR previous = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&forwardingProc));
        g_forward_previous = reinterpret_cast<WNDPROC>(previous);
        SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, (15 << 16) | 20);
        CHECK(GetCapture() == hwnd);
        SendMessageW(hwnd, WM_LBUTTONUP, 0, (15 << 16) | 20);
        CHECK(GetCapture() == nullptr);
        CHECK(g_forwarded_capture_changes > 0);
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, previous);
        g_forward_overlay = nullptr;
        stats.flips = 5;
        CHECK(target.drawOnce(overlay, stats));
    }
    DestroyWindow(hwnd);
}

void testOverlayRecording(tox2::VkContext& ctx) {
    OffscreenTarget target(ctx);
    CHECK(target.ok());
    if (!target.ok()) return;
    const std::uint32_t errors_before = ctx.validationErrorCount();

    drawTwice(ctx, target);
    // The first overlay is gone; a second one starts from scratch on the same device.
    drawTwice(ctx, target);
    testWin32Input(ctx, target);

    const std::uint32_t errors = ctx.validationErrorCount() - errors_before;
    std::printf("overlay tests: validation %s, %u error(s)\n", ctx.validationEnabled() ? "active" : "inactive", errors);
    CHECK(errors == 0);
}

}  // namespace

int main() {
    testSteadyFlips();
    testRingWraps();
    testDrawDeltas();
    testRefresh();
    testPanels();
    testPresentRequests();
    testResolutionScalePanel();
    testRuntimeSources();

    tox2::VkContextOptions options;
    options.validation = true;
    std::string error;
    std::unique_ptr<tox2::VkContext> ctx = tox2::createVkContext(options, error);
    if (ctx) {
        testToggleKey(*ctx);
        testOverlayRecording(*ctx);
    } else {
        std::printf("overlay tests: vulkan: skipped (%s)\n", error.c_str());
    }
    std::printf("overlay tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}
