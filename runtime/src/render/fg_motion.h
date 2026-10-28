#pragma once

// Camera motion vectors for frame generation, and a diagnostic that checks them against the images. Both are plain
// fragment passes over the scene image; nothing here calls Streamline, so they run in every build.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "render/dlaa_math.h"
#include "render/streamline.h"
#include "render/vk_context.h"

namespace tox2 {

struct FgMotionInputs {
    // Depth-aspect view of the frame's depth, SHADER_READ_ONLY_OPTIMAL, the size given to prepare().
    VkImageView depth = VK_NULL_HANDLE;
    // The unflipped clip-to-clip transform (row vectors, the renderer's clip space); identity gives zero vectors.
    Mat4 clip_to_prev_clip = mat4Identity();
    // Diagnostic sign of each component, applied to the finished vector.
    float flip[2] = {1.0f, 1.0f};
    // Takes the object vector where the object image holds one for the pixel (its depth equals the depth image's), the
    // camera vector elsewhere, and keeps the camera-only vectors in their own image. Needs hasObjectWrites() for the
    // frame; the object image is sampled and stays in SHADER_READ_ONLY_OPTIMAL afterwards (objectLayout()).
    bool object = false;
};

// Sums over the pixels whose motion-compensated position lies inside the image, for the vectors and for the camera-only
// vectors both. Means are over the mean of the three channels of |difference|, in 0..255 units.
struct FgWarpStats {
    double warp_mean = 0.0;         // |cur(p) - prev(p + mv)|, prev sampled bilinearly
    double no_motion_mean = 0.0;    // |cur(p) - prev(p)| over the same pixels
    double camera_warp_mean = 0.0;  // the same pixels warped with the camera-only vectors
    std::uint32_t pixels = 0;
    std::uint32_t object_pixels = 0;  // pixels whose vector differs from the camera's by more than 0.5 px
    double object_warp_mean = 0.0, object_camera_warp_mean = 0.0;  // over those pixels
};

// Renderer thread. The caller keeps the device idle (or the commands finished) before destroying the pass or
// replacing its images.
class FgMotionPass {
public:
    explicit FgMotionPass(VkContext& context);
    ~FgMotionPass();
    FgMotionPass(const FgMotionPass&) = delete;
    FgMotionPass& operator=(const FgMotionPass&) = delete;

    // Makes the images for a scene of this size. When the size changed, `recreated` is true and `retired` holds the
    // previous images (null at the first call), which recorded commands may still use. This also drops the
    // previous-colour history of the warp check. False with `error` on failure.
    bool prepare(std::uint32_t width, std::uint32_t height, std::shared_ptr<void>& retired, bool& recreated,
                 std::string& error);

    // The R16G16_SFLOAT motion vector image, dedicated memory, in the shape Streamline is told about it. Its layout
    // is SHADER_READ_ONLY_OPTIMAL once recordMotion() has run; empty before prepare().
    SlImage motionImage() const;
    VkImageView motionView() const;
    // The camera-only vectors the combine pass keeps beside motionImage(); written only by recordMotion() with
    // `object`, SHADER_READ_ONLY_OPTIMAL afterwards.
    VkImageView cameraView() const;

    // The R32G32_UINT object image of the scene's size (x = the two half floats of the vector, y = the bits of the
    // depth the fragment wrote, or kObjectMotionNone) and its view for the scene pass to attach; empty before
    // prepare().
    VkImage objectImage() const;
    VkImageView objectView() const;
    // The layout the object image is in; UNDEFINED until the first beginObjectWrites().
    VkImageLayout objectLayout() const;
    // Before the first candidate instance of `frame`: clears the object image once per frame to (0, kObjectMotionNone),
    // then makes it a colour attachment. Called again in the same frame (a scene written again after the tap) it keeps
    // the contents. Outside rendering; needs prepare(). The image is in COLOR_ATTACHMENT_OPTIMAL afterwards. Only
    // recordMotion() with `object` and the tracked objectLayout() move it elsewhere; a caller that does so too (a
    // read-back) leaves it in the layout objectLayout() reports.
    void beginObjectWrites(VkCommandBuffer cmd, std::uint64_t frame);
    // True when beginObjectWrites() ran for `frame`, so the object image holds that frame's draws (or nothing yet).
    bool hasObjectWrites(std::uint64_t frame) const;

    // Records the motion vector pass: for each pixel p (centre), the previous position of the surface it shows minus
    // p, in pixels with y down. All of the image is written. With `inputs.object` the combine pass writes the final
    // vectors and the camera-only ones. Outside rendering; the bound pipeline, descriptors and
    // push constants are invalid afterwards.
    bool recordMotion(VkCommandBuffer cmd, const FgMotionInputs& inputs, std::string& error);

    // Records the warp check of the current colour against the previous call's colour, moved by `motion`
    // (SHADER_READ_ONLY_OPTIMAL, the size given to prepare()), then keeps a copy of `color` as the next call's
    // previous colour. `color` is in `color_layout` and 8-bit RGBA-like (the copy is R8G8B8A8_UNORM). The first call
    // after prepare() only seeds the copy. One check may be pending: read it with collectWarpStats() once the
    // commands finished, before recording the next one. Recording while a check is still pending fails with `error`,
    // since both write the same read-back buffer. `camera_motion` (SHADER_READ_ONLY_OPTIMAL) is the camera-only
    // vectors for the camera figures; null uses `motion`, which makes both figures the same.
    bool recordWarpCheck(VkCommandBuffer cmd, VkImageView color, VkImageLayout color_layout, VkImageView motion,
                         std::string& error, VkImageView camera_motion = VK_NULL_HANDLE);
    // False when the last recordWarpCheck() had no previous colour, or none was recorded since the last collect.
    bool collectWarpStats(FgWarpStats& out);

private:
    struct Resources;
    bool createObjects(std::string& error);
    enum class Pass : std::uint8_t { Motion, Combine, Warp };
    bool createPipeline(Pass pass, std::string& error);
    bool createWarpResources(std::string& error);

    VkContext& context_;
    VkSampler nearest_ = VK_NULL_HANDLE, linear_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkShaderModule vertex_ = VK_NULL_HANDLE;
    // Indexed by Pass.
    std::array<VkShaderModule, 3> fragments_{};
    std::array<VkPipeline, 3> pipelines_{};
    std::shared_ptr<Resources> resources_;
};

}  // namespace tox2
