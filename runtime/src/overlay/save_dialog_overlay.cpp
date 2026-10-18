#include "overlay/save_dialog_overlay.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <string_view>
#include <utility>

#include "core/log.h"
#include "hle/save_dialog_text.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "imgui_internal.h"
#include "input/pad_buttons.h"
#include "overlay/imgui_context.h"
#include "render/vk_renderer.h"

namespace tox2 {

namespace {

// Descriptor sets: the font atlas plus the icon textures of the list (kMaxIconTextures).
constexpr std::uint32_t kMaxIconTextures = 32;
constexpr std::uint32_t kDescriptorCount = 1 + kMaxIconTextures;

// The canvas is 1280x720 and scales with the image area's height; every layout number below is in canvas pixels.
constexpr float kCanvasHeight = 720.0f;

// Font sizes in canvas pixels; the atlas holds one font per entry.
constexpr std::array<float, 3> kFontSizes = {32.0f, 22.0f, 17.0f};
constexpr int kFontBody = 1;

// A glyph set above this restarts from the current dialog's glyphs, which keeps the atlas texture bounded.
constexpr std::size_t kMaxBakedCodePoints = 8000;

constexpr float kMinDeltaTime = 1e-4f;
constexpr float kMaxDeltaTime = 0.1f;
constexpr float kFirstDeltaTime = 1.0f / 60.0f;

// The backend reports failures through a plain function pointer without user data.
std::atomic<VkResult> g_first_vk_error{VK_SUCCESS};

void recordVkResult(VkResult result) {
    if (result >= VK_SUCCESS) return;
    VkResult expected = VK_SUCCESS;
    g_first_vk_error.compare_exchange_strong(expected, result);
}

// Maps canvas coordinates onto the game's image area and applies the dialog's fade to every color.
struct Canvas {
    ImVec2 origin;
    float scale = 1.0f;
    float opacity = 1.0f;

    ImVec2 pt(float x, float y) const { return ImVec2(origin.x + x * scale, origin.y + y * scale); }
    float px(float v) const { return v * scale; }
    // A line is never thinner than one device pixel, or it would vanish at small window sizes.
    float line(float v) const { return std::max(1.0f, v * scale); }
    ImU32 color(float r, float g, float b, float a) const {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a * opacity));
    }
    ImU32 white(float a) const { return color(1.0f, 1.0f, 1.0f, a); }
};

// A horizontal rule covering exactly the pixel row(s) at canvas y, so it is not spread over two rows.
void drawRule(ImDrawList* list, const Canvas& canvas, float x0, float x1, float y, ImU32 color) {
    const ImVec2 a = canvas.pt(x0, y);
    list->AddRectFilled(a, ImVec2(canvas.pt(x1, y).x, a.y + canvas.line(1.0f)), color);
}

// The enter/back symbols are draw-list primitives, not font glyphs, so no font needs them.
void drawSymbol(ImDrawList* list, const Canvas& canvas, PadButton button, ImVec2 center) {
    const float half = canvas.px(8.0f);
    if (button == PadButton::Circle) {
        list->AddCircle(center, half, canvas.color(0.95f, 0.45f, 0.35f, 1.0f), 0, canvas.line(2.0f));
        return;
    }
    const ImU32 color = canvas.color(0.45f, 0.65f, 0.95f, 1.0f);
    const float thickness = canvas.line(2.0f);
    list->AddLine(ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half), color, thickness);
    list->AddLine(ImVec2(center.x - half, center.y + half), ImVec2(center.x + half, center.y - half), color, thickness);
}

float textWidth(ImFont* font, const char* begin, const char* end) {
    return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, begin, end).x;
}

// A symbol and its label, centred on canvas x as one item; `top` is the canvas y of the text line.
void drawButtonItem(ImDrawList* list, const Canvas& canvas, ImFont* font, PadButton button, std::string_view label,
                    float center_x, float top) {
    const float size = font->FontSize;
    const char* begin = label.data();
    const char* end = begin + label.size();
    const float symbol = canvas.px(16.0f);
    const float gap = canvas.px(8.0f);
    const float total = symbol + gap + textWidth(font, begin, end);
    const ImVec2 origin = canvas.pt(center_x, top);
    const float x0 = origin.x - total * 0.5f;
    drawSymbol(list, canvas, button, ImVec2(x0 + symbol * 0.5f, origin.y + size * 0.5f));
    list->AddText(font, size, ImVec2(x0 + symbol + gap, origin.y), canvas.white(1.0f), begin, end);
}

struct TextLine {
    const char* begin;
    const char* end;
};

// Breaks `text` at newlines and at `wrap_px`. ImGui's wrapper cuts a run without blanks (Japanese) at the first
// character that overflows, so the text needs no spaces to wrap.
std::vector<TextLine> wrapLines(ImFont* font, std::string_view text, float wrap_px) {
    std::vector<TextLine> lines;
    const char* p = text.data();
    const char* const text_end = p + text.size();
    while (true) {
        const char* paragraph_end = p;
        while (paragraph_end < text_end && *paragraph_end != '\n') ++paragraph_end;
        if (p == paragraph_end) lines.push_back({p, p});  // a blank line keeps its height
        while (p < paragraph_end) {
            const char* eol = font->CalcWordWrapPositionA(1.0f, p, paragraph_end, wrap_px);
            if (eol <= p + 1) {
                // Too narrow for anything: take one whole character so the loop advances.
                unsigned int c = 0;
                const int used = ImTextCharFromUtf8(&c, p, paragraph_end);
                eol = std::min(paragraph_end, p + std::max(1, used));
            }
            const char* line_end = eol;
            while (line_end > p && line_end[-1] == ' ') --line_end;
            lines.push_back({p, line_end});
            p = eol;
            while (p < paragraph_end && *p == ' ') ++p;
        }
        if (paragraph_end >= text_end) break;
        p = paragraph_end + 1;
    }
    return lines;
}

// Centred message lines growing upward from y 380, a rule below them and the answer buttons under the rule.
void drawMessage(ImDrawList* list, const Canvas& canvas, ImFont* font, const SaveDialogContent& content) {
    const float size = font->FontSize;
    const float line_height = size * 1.3f;
    const std::vector<TextLine> lines = wrapLines(font, content.message, canvas.px(1000.0f));
    float y = canvas.pt(0.0f, 380.0f).y - line_height * static_cast<float>(lines.size());
    const float center_x = canvas.pt(640.0f, 0.0f).x;
    for (const TextLine& line : lines) {
        const float x = center_x - textWidth(font, line.begin, line.end) * 0.5f;
        if (line.end > line.begin) list->AddText(font, size, ImVec2(x, y), canvas.white(1.0f), line.begin, line.end);
        y += line_height;
    }

    drawRule(list, canvas, 140.0f, 1140.0f, 400.0f, canvas.white(0.6f));

    const DialogButtons buttons = dialogButtons();
    if (content.kind == DialogKind::YesNo) {
        drawButtonItem(list, canvas, font, buttons.enter, kButtonYes, 560.0f, 420.0f);
        drawButtonItem(list, canvas, font, buttons.back, kButtonNo, 720.0f, 420.0f);
    } else {
        drawButtonItem(list, canvas, font, buttons.enter, kButtonOk, 640.0f, 420.0f);
    }
}

}  // namespace

SaveDialogOverlay::SaveDialogOverlay(VkContext& context, SaveDialogBoard& board, SaveDialogOverlayOptions options)
    : context_(context), board_(board), options_(std::move(options)) {
    board_.setDrawer(SaveDialogDrawer::Ready);
}

SaveDialogOverlay::~SaveDialogOverlay() {
    if (imgui_ != nullptr || descriptor_pool_ != VK_NULL_HANDLE) vkDeviceWaitIdle(context_.device());
    shutdown();
    board_.setDrawer(SaveDialogDrawer::None);
}

bool SaveDialogOverlay::wantsDraw() const { return !failed() && board_.read().content != nullptr; }

bool SaveDialogOverlay::wantsIdlePresent() const { return wantsDraw(); }

void SaveDialogOverlay::fail(const char* what) {
    failed_.store(true, std::memory_order_relaxed);
    board_.setDrawer(SaveDialogDrawer::Failed);
    logWarn("tox2: savedata dialog: overlay {} failed, the dialog is answered without being shown", what);
    shutdown();
}

void SaveDialogOverlay::shutdown() {
    std::lock_guard lock(imguiMutex());
    initialized_ = false;
    if (imgui_ != nullptr) {
        {
            // The backend's shutdown acts on the current context.
            ImGuiContextScope scope(imgui_);
            if (vulkan_backend_) ImGui_ImplVulkan_Shutdown();
        }
        vulkan_backend_ = false;
        // Outside the scope: DestroyContext clears the current context itself when it is the destroyed one, and
        // a scope would restore that freed pointer afterwards.
        ImGui::DestroyContext(imgui_);
        imgui_ = nullptr;
    }
    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(context_.device(), descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
    }
    baked_.clear();
    baked_scene_height_ = 0;
}

bool SaveDialogOverlay::initialize(const Presenter::Target& target) {
    color_format_ = target.format;
    g_first_vk_error.store(VK_SUCCESS);
    // A missing font is not a failure: the dialog then falls back to ImGui's default font.
    font_ = loadUiFont(options_.font_candidates);
    if (const char* failed_step = setUpContext()) {
        // Outside the scope of setUpContext, so shutdown() does not restore a context it has just destroyed.
        fail(failed_step);
        return false;
    }
    return true;
}

const char* SaveDialogOverlay::setUpContext() {
    std::lock_guard lock(imguiMutex());
    IMGUI_CHECKVERSION();
    // CreateContext makes the new context current, which would switch another overlay's context under it.
    ImGuiContext* previous = ImGui::GetCurrentContext();
    imgui_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(previous);
    ImGuiContextScope scope(imgui_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = kDescriptorCount;
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = kDescriptorCount;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(context_.device(), &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        descriptor_pool_ = VK_NULL_HANDLE;
        return "descriptor pool";
    }

    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = VK_API_VERSION_1_3;
    init.Instance = context_.instance();
    init.PhysicalDevice = context_.physicalDevice();
    init.Device = context_.device();
    init.QueueFamily = context_.graphicsQueueFamily();
    init.Queue = context_.graphicsQueue();
    init.DescriptorPool = descriptor_pool_;
    init.MinImageCount = 2;
    // The backend reuses a vertex buffer every ImageCount draws. The renderer has waited a slot's fence
    // after kFramesInFlight frames, so one more is margin.
    init.ImageCount = static_cast<std::uint32_t>(RsxRenderer::kFramesInFlight + 1);
    init.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.UseDynamicRendering = true;
    init.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    init.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format_;
    init.CheckVkResultFn = &recordVkResult;
    if (!ImGui_ImplVulkan_Init(&init)) return "Vulkan backend";
    vulkan_backend_ = true;

    // The atlas is built by the first record(), when the dialog's glyphs are known.
    initialized_ = true;
    logInfo("tox2: savedata dialog: overlay ready");
    return nullptr;
}

bool SaveDialogOverlay::ensureAtlas(const SaveDialogContent& content, std::uint32_t scene_height) {
    bool due = scene_height != baked_scene_height_;  // also true while no atlas is built
    std::set<ImWchar> needed;
    bool computed = false;
    if (!due && content.id != checked_content_id_) {
        needed = dialogCodePoints(content);
        computed = true;
        due = !std::includes(baked_.begin(), baked_.end(), needed.begin(), needed.end());
    }
    checked_content_id_ = content.id;
    if (!due) return true;
    if (!computed) needed = dialogCodePoints(content);

    std::set<ImWchar> merged = baked_;
    merged.insert(needed.begin(), needed.end());
    if (merged.size() > kMaxBakedCodePoints) merged = std::move(needed);
    if (rebuildAtlas(merged, scene_height)) return true;
    fail("font atlas");
    return false;
}

bool SaveDialogOverlay::rebuildAtlas(const std::set<ImWchar>& code_points, std::uint32_t scene_height) {
    ImGuiIO& io = ImGui::GetIO();
    g_first_vk_error.store(VK_SUCCESS);
    if (baked_scene_height_ != 0) {
        // Frames in flight may still sample the old texture.
        vkDeviceWaitIdle(context_.device());
        ImGui_ImplVulkan_DestroyFontsTexture();
    }
    io.Fonts->Clear();
    io.FontDefault = nullptr;
    glyph_ranges_ = glyphRanges(code_points);

    const float scale = static_cast<float>(scene_height) / kCanvasHeight;
    for (const float size : kFontSizes) {
        const float pixels = std::max(1.0f, std::round(size * scale));
        ImFontConfig config;
        config.SizePixels = pixels;
        if (font_) {
            config.FontDataOwnedByAtlas = false;
            // Large glyphs do not need sub-pixel positioning, and the atlas texture stays smaller.
            config.OversampleH = 1;
            config.OversampleV = 1;
            config.PixelSnapH = true;
            config.GlyphRanges = glyph_ranges_.Data;
            ImFormatString(config.Name, IM_ARRAYSIZE(config.Name), "ui %.0fpx", pixels);
            if (io.Fonts->AddFontFromMemoryTTF(font_->bytes.data(), static_cast<int>(font_->bytes.size()), pixels,
                                               &config) == nullptr) {
                return false;
            }
        } else if (io.Fonts->AddFontDefault(&config) == nullptr) {
            return false;
        }
    }

    // One queue submit and a wait: the hitch when a dialog with new glyphs opens.
    if (!ImGui_ImplVulkan_CreateFontsTexture() || g_first_vk_error.load() != VK_SUCCESS) return false;
    baked_ = code_points;
    baked_scene_height_ = scene_height;
    ++atlas_build_count_;
    return true;
}

void SaveDialogOverlay::record(VkCommandBuffer cmd, const Presenter::Target& target) {
    if (failed() || target.view == VK_NULL_HANDLE) return;
    // The board is read before the context scope opens (lock order, see imgui_context.h).
    const SaveDialogSnapshot snapshot = board_.read();
    if (snapshot.content == nullptr) return;
    const PixelRect scene = letterboxRect(target.width, target.height, 16, 9);
    if (scene.height == 0) return;

    if (!initialized_) {
        if (!initialize(target)) return;
    } else if (target.format != color_format_) {
        logOnce("save-dialog-format", "tox2: savedata dialog: surface format changed, dialog skipped for that image");
        return;
    }

    ImGuiContextScope scope(imgui_);
    // Built before the opacity check, so the hitch falls into the fade-in instead of the first visible frame.
    if (!ensureAtlas(*snapshot.content, scene.height)) return;
    if (snapshot.opacity <= 0.0f) return;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(target.width), static_cast<float>(target.height));
    const auto now = std::chrono::steady_clock::now();
    io.DeltaTime = last_frame_time_.time_since_epoch().count() == 0
                           ? kFirstDeltaTime
                           : std::clamp(std::chrono::duration<float>(now - last_frame_time_).count(), kMinDeltaTime,
                                        kMaxDeltaTime);
    last_frame_time_ = now;

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    {
        Canvas canvas;
        canvas.origin = ImVec2(static_cast<float>(scene.x), static_cast<float>(scene.y));
        canvas.scale = static_cast<float>(scene.height) / kCanvasHeight;
        canvas.opacity = std::min(snapshot.opacity, 1.0f);
        ImDrawList* list = ImGui::GetForegroundDrawList();
        const SaveDialogContent& content = *snapshot.content;
        const bool message = content.kind != DialogKind::List;

        // The dim covers the whole target, including the bars beside the 16:9 image.
        const ImVec2 target_size(static_cast<float>(target.width), static_cast<float>(target.height));
        list->AddRectFilled(ImVec2(0.0f, 0.0f), target_size, canvas.color(0.0f, 0.0f, 0.0f, message ? 0.85f : 0.6f));
        if (message && io.Fonts->Fonts.Size > kFontBody) drawMessage(list, canvas, io.Fonts->Fonts[kFontBody], content);
    }
    ImGui::Render();

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = target.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {target.width, target.height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRendering(cmd);
}

}  // namespace tox2
