#pragma once

// Camera motion vectors for frame generation, and a diagnostic that checks them against the images. Both are plain
// fragment passes over the scene image; nothing here calls Streamline, so they run in every build.

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
};

// Sums over the pixels whose motion-compensated position lies inside the image. Means are over the mean of the three
// channels of |difference|, in 0..255 units.
struct FgWarpStats {
    double warp_mean = 0.0;       // |cur(p) - prev(p + mv)|, prev sampled bilinearly
    double no_motion_mean = 0.0;  // |cur(p) - prev(p)| over the same pixels
    std::uint32_t pixels = 0;
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

    // The R32G32_UINT object image of the scene's size (x = the two half floats of the vector, y = the bits of the
    // depth the fragment wrote, or kObjectMotionNone) and its view for the scene pass to attach; empty before
    // prepare().
    VkImage objectImage() const;
    VkImageView objectView() const;
    // Before the first candidate instance of `frame`: clears the object image once per frame to (0, kObjectMotionNone),
    // then makes it a colour attachment. Called again in the same frame (a scene written again after the tap) it keeps
    // the contents. Outside rendering; needs prepare(). The image is in COLOR_ATTACHMENT_OPTIMAL afterwards; a caller
    // that moves it elsewhere moves it back before the next call.
    void beginObjectWrites(VkCommandBuffer cmd, std::uint64_t frame);

    // Records the motion vector pass: for each pixel p (centre), the previous position of the surface it shows minus
    // p, in pixels with y down. All of the image is written. Outside rendering; the bound pipeline, descriptors and
    // push constants are invalid afterwards.
    bool recordMotion(VkCommandBuffer cmd, const FgMotionInputs& inputs, std::string& error);

    // Records the warp check of the current colour against the previous call's colour, moved by `motion`
    // (SHADER_READ_ONLY_OPTIMAL, the size given to prepare()), then keeps a copy of `color` as the next call's
    // previous colour. `color` is in `color_layout` and 8-bit RGBA-like (the copy is R8G8B8A8_UNORM). The first call
    // after prepare() only seeds the copy. One check may be pending: read it with collectWarpStats() once the
    // commands finished, before recording the next one. Recording while a check is still pending fails with `error`,
    // since both write the same read-back buffer.
    bool recordWarpCheck(VkCommandBuffer cmd, VkImageView color, VkImageLayout color_layout, VkImageView motion,
                         std::string& error);
    // False when the last recordWarpCheck() had no previous colour, or none was recorded since the last collect.
    bool collectWarpStats(FgWarpStats& out);

private:
    struct Resources;
    bool createObjects(std::string& error);
    bool createPipeline(bool warp, std::string& error);
    bool createWarpResources(std::string& error);

    VkContext& context_;
    VkSampler nearest_ = VK_NULL_HANDLE, linear_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkShaderModule vertex_ = VK_NULL_HANDLE, motion_fragment_ = VK_NULL_HANDLE, warp_fragment_ = VK_NULL_HANDLE;
    VkPipeline motion_pipeline_ = VK_NULL_HANDLE, warp_pipeline_ = VK_NULL_HANDLE;
    std::shared_ptr<Resources> resources_;
};

}  // namespace tox2
