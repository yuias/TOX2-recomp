#pragma once

// RSX command executor on Vulkan: render target cache, clears, draws with textures and flips. A sampled
// unit without a usable texture reads a 1x1 magenta placeholder.

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "render/fp_translator.h"
#include "render/frame_dump.h"
#include "render/image_pool.h"
#include "render/present_overlay.h"
#include "render/render_scale.h"
#include "render/vk_context.h"
#include "render/vk_pipelines.h"
#include "render/vk_textures.h"
#include "render/vk_window.h"
#include "rsx/rsx_frame.h"

namespace tox2 {

struct RenderOptions {
    // Every dump_every-th flip (counted from 1) is written to `<dump_dir>/frame_<flip number>.png`. An
    // empty directory disables dumping.
    std::filesystem::path dump_dir;
    std::uint32_t dump_every = 1;
    // Where requestDisplayDump() writes `frame_<flip number>.png`, unless dump_dir already dumps that flip.
    std::filesystem::path requested_dump_dir = "generated/frames";
    // Also holds the driver's pipeline cache (pipelines.bin) when set.
    std::filesystem::path shader_cache;
    // Copy the display buffer back at every flip so readDisplay works after each one (replay, tests). The
    // copy is 8 MiB at 1080p and grows with the square of the resolution scale, so a live run leaves this off
    // and only reads back the flips that are dumped.
    bool readback_all = false;
    // Image pixels per guest pixel of every colour and depth target, 1..kMaxResolutionScale. Anything else
    // makes the renderer invalid. A target too large for the device at this scale is created at the largest
    // scale that fits (and counted).
    std::uint32_t resolution_scale = 1;
    // How anti-aliased surfaces are drawn. Msaa falls back to Supersample (and is counted) on a device without
    // 2-sample support; aaMode() reports the effective mode.
    AaMode aa_mode = AaMode::Msaa;
    // Forced anisotropic filtering level: 1 (off), 2, 4, 8 or 16; anything else makes the renderer invalid. A device
    // without the samplerAnisotropy feature keeps 1; a lower device maximum clamps the level (see anisotropy()).
    std::uint32_t anisotropy = 1;
};

// What a created GPU image or upload block was for (see RendererStats::images_created).
enum class RenderImageKind : std::uint8_t {
    Target,       // colour or depth target
    Sidecar,      // 2-sample image of an anti-aliased target
    DepthArgb,    // A8R8G8B8 rewrite of a depth surface
    Texture,      // texture cache image (not the DepthArgb ones)
    UploadBlock,  // host-visible draw upload buffer
};
inline constexpr std::size_t kRenderImageKindCount = 5;

struct RendererStats {
    std::uint64_t clears = 0;         // clears recorded (a clear whose mask is not modelled still counts)
    std::uint64_t draws = 0;          // draws handled: recorded into a command buffer, or dropped with an empty scissor
    std::uint64_t draws_skipped = 0;  // draws seen but not rendered (unimplemented state, program or primitive)
    std::uint64_t flips = 0;
    std::uint64_t texture_uploads = 0;    // texture images written from guest bytes (when the content id changed)
    std::uint64_t feedback_copies = 0;    // render target copies made for draws that sample their own target
    std::uint64_t depth_conversions = 0;  // depth surfaces rewritten as A8R8G8B8 texels for a draw that reads them
    std::uint64_t surface_inherits = 0;   // copies of an old target's contents into a resized one
    std::uint64_t layout_copies = 0;      // multisampled targets copied into their memory layout for a read
    // GPU images and upload blocks created, by RenderImageKind, with their size in bytes (width x height x samples x
    // texel size). The Texture entries follow the texture cache as of the end of the last execute() or flip.
    std::array<std::uint64_t, kRenderImageKindCount> images_created{};
    std::array<std::uint64_t, kRenderImageKindCount> image_bytes_created{};
    // Retired images reused instead of created, by RenderImageKind (Target, Sidecar and DepthArgb are pooled).
    std::array<std::uint64_t, kRenderImageKindCount> image_pool_hits{};
    // Creations of pooled kinds, by ImagePoolMiss. Their sum equals images_created[Target] + [Sidecar] + [DepthArgb].
    std::array<std::uint64_t, kImagePoolMissCount> image_pool_misses{};
    std::uint64_t image_pool_bytes = 0;     // held by the pool at the end of the last flip
    ImagePoolDiscards image_pool_discards;  // the pool's discards() at the end of the last flip
    // Recording cost counters, read by the replay tool's timing report.
    std::uint64_t slot_wait_ns = 0;                 // in vkWaitForFences for a frame slot (the GPU behind the CPU)
    std::uint64_t barriers = 0;                     // image barriers recorded by transitionImage
    std::uint64_t barriers_skipped = 0;             // transitions that needed no barrier
    std::uint64_t rendering_instances = 0;          // vkCmdBeginRendering calls, all paths
    std::uint64_t state_calls = 0;                  // vkCmdSet* and vkCmdBindPipeline calls recorded by recordDraw
    std::uint64_t state_calls_skipped = 0;          // ... that were not recorded because the value was already set
    std::uint64_t vp_constant_uploads_reused = 0;   // vertex constant uploads shared by draws with equal constants
    VkMemoryPropertyFlags upload_memory_flags = 0;  // of the first upload block (vmaGetAllocationMemoryProperties)
};

// Single-threaded: execute, readDisplay and stats are called from one thread. The renderer never
// touches guest memory; every command carries the bytes it needs.
class RsxRenderer {
public:
    RsxRenderer(VkContext& context, RenderOptions options);
    ~RsxRenderer();
    RsxRenderer(const RsxRenderer&) = delete;
    RsxRenderer& operator=(const RsxRenderer&) = delete;

    // False when construction or a later Vulkan call failed; `error()` says why. Commands are
    // ignored afterwards.
    bool valid() const { return error_.empty(); }
    const std::string& error() const { return error_; }

    // Records into the frame's command buffer; a flip submits it.
    void execute(std::span<RsxCommand> commands);

    // BGRA8 rows of the display buffer of the last flip (waits for the GPU), at image size: the guest size times
    // the resolution scale the target was created at. A flip whose buffer is not a known render target reads
    // back as opaque black at the flip's size times the resolution scale. False (outputs untouched)
    // before any flip, and when the last flip was gated off: without `readback_all` the pixels are only
    // copied for flips that are dumped, and a dump due at that flip reads them itself.
    bool readDisplay(std::vector<std::uint8_t>& bgra, std::uint32_t& width, std::uint32_t& height);

    const RendererStats& stats() const { return stats_; }

    std::uint32_t resolutionScale() const { return resolution_scale_; }

    // The effective anti-aliasing mode: the requested one, except Supersample when the device lacks 2 samples.
    AaMode aaMode() const { return aa_mode_; }
    // True when the device supports 2-sample colour and depth/stencil surfaces.
    bool msaaAvailable() const { return context_.twoSampleSupportGap().empty(); }

    // Changes the scale of the targets created or bound from now on; false and no change outside
    // 1..kMaxResolutionScale. A target keeps its image until a draw or clear binds it again, then it is replaced by a
    // copy at the new scale (see acquire), so the picture changes at the next flip that draws. Renderer thread.
    bool setResolutionScale(std::uint32_t scale);

    // Changes how the next pass over an anti-aliased surface is drawn; false and no change for Msaa when the device
    // lacks 2-sample support. No target is touched: a read brings the memory layout up to date from the 2-sample image
    // (syncLayout), and a pass that finds the other representation newer and does not replace all of it is counted
    // (see beginSidecarPass). Renderer thread, between commands.
    bool setAaMode(AaMode mode);

    // The effective forced anisotropy level: 1 when off or unsupported, else the requested level clamped to the
    // device maximum.
    std::uint32_t anisotropy() const { return anisotropy_; }
    // The device's maximum sampler anisotropy; 0 without the samplerAnisotropy feature.
    float maxAnisotropy() const { return context_.maxSamplerAnisotropy(); }

    // Changes the level of the samplers created from now on; existing ones stay cached because frames in flight may
    // use them. False and no change for a level not in kAnisotropyLevels. Renderer thread, between commands.
    bool setAnisotropy(std::uint32_t level);

    // Frames written, failed or dropped by the dump worker so far (dump_dir and requested dumps). Any thread.
    DumpCounts dumpCounts() const;
    // Waits until the dumps queued so far are written, at most `timeout`; returns how many are still
    // pending (0 when all are done). Any thread.
    std::uint64_t flushDumps(std::chrono::milliseconds timeout);
    // Writes the pipeline cache to the shader cache directory; false without one or on failure. Any thread.
    bool savePipelineCache();
    // Any thread.
    PipelineCacheStats pipelineCacheStats() const;

    // The next flip is read back and written as `frame_<flip number>.png`: into dump_dir when that flip is
    // dumped anyway (one file, no duplicate), else into requested_dump_dir. Renderer thread.
    void requestDisplayDump() { dump_requested_ = true; }

    // Drawn over the presented image after the blit; only used while a presenter is set. Not owned; call
    // before the first flip.
    void setPresentOverlay(PresentOverlay* overlay) { overlay_ = overlay; }

    // Every flip is also blitted to the presenter's next swapchain image (letterboxed 16:9) and presented.
    // Not owned; null (the default) presents nothing. Call before the first flip.
    void setPresenter(Presenter* presenter) {
        if (presenter != nullptr && presenter->frameSlots() < kFramesInFlight) {
            fail("the presenter has fewer frame slots than the renderer");
            return;
        }
        presenter_ = presenter;
    }

    // Frames recorded before a slot is reused; a presenter needs one acquire semaphore per slot.
    static constexpr std::size_t kFramesInFlight = 2;

private:
    struct RenderTarget;
    struct FrameSlot;
    // A region of a slot's upload block, already mapped for writing.
    struct UploadRef {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        std::uint8_t* data = nullptr;
    };
    using TargetKey = std::pair<std::uint32_t, std::uint32_t>;  // location, offset
    using TargetMap = std::map<TargetKey, std::shared_ptr<RenderTarget>>;

    void fail(std::string message);
    void noteImageCreated(RenderImageKind kind, std::uint64_t bytes);
    // Takes an image with the parameters of `info` from the pool, or creates it (counting the creation). `key` is
    // filled either way; false when the allocation fails.
    bool obtainImage(RenderImageKind kind, const VkImageCreateInfo& info, std::uint64_t bytes, PooledImageKey& key,
                     VkImage& image, VmaAllocation& allocation);
    void syncTextureStats();
    void logImageCreations();
    bool beginFrame();
    bool waitSlot(FrameSlot& slot);
    bool submitFrame();
    // The slot's command buffer, for every command recorded outside a draw's rendering instance (the only way to
    // reach it besides beginFrame and the in-instance part of recordDraw). Ends the draw instance left open by the
    // last draw, if any.
    VkCommandBuffer outsideCmd();

    std::shared_ptr<RenderTarget> acquireColor(const RsxSurface& surface, bool overwrites_all);
    std::shared_ptr<RenderTarget> acquireDepth(const RsxSurfaceState& surfaces, bool overwrites_all);
    std::shared_ptr<RenderTarget> acquire(TargetMap& map, bool depth, const RsxSurface& surface, VkFormat format,
                                          bool overwrites_all);
    std::shared_ptr<RenderTarget> createTarget(bool depth, std::uint32_t layout_width, std::uint32_t layout_height,
                                               std::uint32_t pitch, VkFormat format);
    std::uint32_t targetScale(std::uint32_t layout_width, std::uint32_t layout_height) const;
    std::shared_ptr<RenderTarget> rescaleTarget(const std::shared_ptr<RenderTarget>& previous);
    void inheritContents(const std::shared_ptr<RenderTarget>& previous, RenderTarget& target);
    void touch(const std::shared_ptr<RenderTarget>& target);
    void transition(RenderTarget& target, VkImageLayout layout);
    void transitionImage(VkImage image, VkImageAspectFlags aspect, VkImageLayout& current, VkImageLayout layout);
    void transitionSidecar(RenderTarget& target, VkImageLayout layout);

    // Anti-aliased surfaces in Msaa mode (see the multisampled sidecar of RenderTarget).
    bool multisampledPass(const RsxSurface& surface) const;
    bool ensureMultisample(RenderTarget& target);
    bool syncLayout(RenderTarget& target);
    bool beginLayoutPass(RenderTarget& target, bool writes, bool overwrites_all);
    void beginSidecarPass(RenderTarget& target, bool writes, bool overwrites_all);

    // Host-visible memory of the current frame slot for vertex, index and uniform data, valid until the
    // slot's fence has signaled. False (and fail()) when the allocation fails.
    bool allocUpload(std::size_t size, UploadRef& out);
    bool ensurePlaceholder();
    bool prepareTextures(const RsxDraw& draw, const DrawShaders& shaders, RenderTarget& color, bool multisampled,
                         VkDescriptorImageInfo (&infos)[kFpTextureUnitCount], float (&scales)[kFpTextureUnitCount][4]);
    bool vertexFormatSupported(VkFormat format);
    bool blendSupported(VkFormat format);
    std::shared_ptr<TextureImage> depthAsArgb(RenderTarget& depth);

    void recordClear(const RsxClear& clear);
    void recordMaskedColorClear(const RsxClear& clear, RenderTarget& color, std::uint32_t color_bits,
                                bool multisampled);
    void recordDraw(const RsxDraw& draw);
    void recordFlip(const RsxFlip& flip);
    void recordPresentBlit(RenderTarget* source);
    bool ensureReadback(FrameSlot& slot, VkDeviceSize size);
    bool dumpDue() const;
    bool dumpDisplay(const std::filesystem::path& path);

    VkContext& context_;
    RenderOptions options_;
    std::string error_;
    RendererStats stats_;
    RendererStats logged_stats_;  // stats_ at the last image-creation log line
    std::uint32_t resolution_scale_ = 1;
    AaMode aa_mode_ = AaMode::Supersample;
    std::uint32_t anisotropy_ = 1;
    bool anisotropy_unavailable_logged_ = false;
    // Always created: its worker thread only starts with the first submitted frame.
    std::unique_ptr<FrameDumper> dumper_;
    bool dump_requested_ = false;  // requestDisplayDump() was called and no flip has taken it yet
    bool dump_this_flip_ = false;  // the flip being recorded was requested
    std::filesystem::path last_requested_dump_;

    // Color and depth targets live in separate maps: a surface of each kind may share an offset.
    TargetMap color_targets_;
    TargetMap depth_targets_;

    // Shared with the targets and sidecars it serves, so a target that outlives the renderer can still give its image
    // back.
    std::shared_ptr<ImagePool> image_pool_;

    std::unique_ptr<DrawPipelines> pipelines_;
    std::unique_ptr<TextureCache> textures_;
    std::map<VkFormat, bool> vertex_format_support_;
    std::map<VkFormat, bool> blend_support_;

    // Bound to a sampled texture unit that has no usable texture. The view type has to match the sampler type
    // the fragment program declares for the unit, so there is one image per type (kFpTextureType*: 2D, 3D, cube).
    struct Placeholder {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkImageView view = VK_NULL_HANDLE;
    };
    std::array<Placeholder, 3> placeholders_;
    VkSampler placeholder_sampler_ = VK_NULL_HANDLE;
    bool placeholder_ready_ = false;

    std::array<std::unique_ptr<FrameSlot>, kFramesInFlight> slots_;
    std::size_t current_slot_ = 0;
    bool recording_ = false;
    // The vertex constant upload of the last draw in this frame, for the next draw with the same constants id.
    struct VpConstantUpload {
        std::uint64_t id = 0;
        UploadRef ref;
    };
    VpConstantUpload vp_constant_upload_;

    // The rendering instance the last draw left open on the slot's command buffer. The next draw with the same
    // attachments records into it. Nothing else records while it is open: every other site gets the command buffer
    // from outsideCmd(), which ends it, so an open instance also means the attachments are still in their attachment
    // layouts.
    struct OpenDrawPass {
        VkImageView color_view = VK_NULL_HANDLE;
        VkImageView depth_view = VK_NULL_HANDLE;
        bool stencil = false;
        std::uint32_t width = 0, height = 0;  // render area
        bool operator==(const OpenDrawPass&) const = default;
    };
    std::optional<OpenDrawPass> open_pass_;

    // The dynamic state and pipeline recordDraw last recorded on the slot's command buffer. Dynamic state is command
    // buffer state: it survives the end of a rendering instance, so a draw records only what differs. Every draw
    // pipeline declares the same dynamic states (DrawPipelines::pipeline), so binding another one keeps them.
    struct DrawStateShadow {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkViewport viewport{};
        VkRect2D scissor{};
        VkCullModeFlags cull = 0;
        VkFrontFace front_face{};
        VkPrimitiveTopology topology{};
        VkBool32 restart = 0, depth_test = 0, depth_write = 0, stencil_test = 0;
        VkCompareOp depth_compare{};
        struct Face {
            VkStencilOp fail{}, pass{}, depth_fail{};
            VkCompareOp compare{};
            std::uint32_t compare_mask = 0, write_mask = 0, reference = 0;
        } faces[2];
        float blend_constants[4] = {};
    };
    // Empty: the command buffer's state is unknown and the next draw records all of it. Cleared by beginFrame and by
    // every other site that binds a pipeline or sets state, since a pipeline that does not declare a state as dynamic
    // overwrites it.
    std::optional<DrawStateShadow> draw_state_;
    void invalidateDrawState() { draw_state_.reset(); }

    Presenter* presenter_ = nullptr;
    Presenter::Target present_target_;  // acquired for the frame being recorded
    bool present_pending_ = false;      // present_target_ is valid and its submit has not been queued yet
    PresentOverlay* overlay_ = nullptr;

    bool has_flip_ = false;
    bool display_black_ = false;
    bool display_copied_ = false;  // the last flip's pixels are in its slot's readback buffer
    std::size_t display_slot_ = 0;
    std::uint32_t display_width_ = 0;
    std::uint32_t display_height_ = 0;
};

}  // namespace tox2
