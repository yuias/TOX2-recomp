#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "render/streamline.h"
#include "render/vk_context.h"

namespace tox2 {

// The images and the DLSS viewport around one DLAA evaluation. The renderer records every command that is not the
// evaluation itself (layout transitions, the motion vector clear, the widening blit) because it tracks the layouts of
// the images it records against; this class only creates the images and calls Streamline. Renderer thread.
class DlaaPass {
public:
    // An image the pass owns. `layout` is the layout at the end of the commands recorded so far.
    struct Image {
        VkContext* context = nullptr;
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;  // the whole image, identity mapping
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        std::uint32_t width = 0, height = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageUsageFlags usage = 0;
        std::map<std::uint32_t, VkImageView> sampled_views;  // by RSX remap word (low 16 bits)

        Image() = default;
        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;
        ~Image();
    };

    // Everything that depends on the input size. Replaced as a whole when the size changes.
    struct Resources {
        Image output;    // DLSS output, input size
        Image motion;    // motion vectors, input size; all zero until per-object vectors exist
        Image expanded;  // the output widened to twice its width, which is what the game's resolve samples
        bool motion_cleared = false;
    };

    DlaaPass(VkContext& context, Streamline& streamline);
    ~DlaaPass();  // slFreeResources; the caller has waited for the device to be idle
    DlaaPass(const DlaaPass&) = delete;
    DlaaPass& operator=(const DlaaPass&) = delete;

    // Makes the images for an input of this size. When the size changed, `recreated` is true and `retired` holds the
    // previous images (null at the first call), which recorded commands may still use. False with `error` on failure.
    bool prepare(std::uint32_t width, std::uint32_t height, std::shared_ptr<void>& retired, bool& recreated,
                 std::string& error);
    // Valid after a successful prepare().
    Resources& resources() { return *resources_; }

    // Records one evaluation of `color_in` and `depth` (both SHADER_READ_ONLY_OPTIMAL, the input size) into
    // resources().output (GENERAL) with resources().motion (SHADER_READ_ONLY_OPTIMAL). `jitter_px` is the sub-pixel
    // offset the scene was drawn with, in input pixels (x right, y down), as Streamline wants it reported. Identity
    // matrices. All bound command buffer state is invalid afterwards.
    bool evaluate(VkCommandBuffer cmd, const SlImage& color_in, const SlImage& depth, const float (&jitter_px)[2],
                  bool reset, bool alpha, std::string& error);

private:
    VkContext& context_;
    Streamline& streamline_;
    std::uint32_t viewport_ = 0;
    std::shared_ptr<Resources> resources_;
};

}  // namespace tox2
