#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "render/dlaa_math.h"
#include "render/streamline.h"
#include "render/vk_context.h"

namespace tox2 {

// The matrices of Streamline's Constants for one evaluation (row-major, row vectors; see dlaa_math.h).
struct DlaaMatrices {
    Mat4 view_to_clip = mat4Identity(), clip_to_view = mat4Identity();
    Mat4 clip_to_prev_clip = mat4Identity(), prev_clip_to_clip = mat4Identity();
};

// Describes the camera motion between two frames to DLAA without motion vectors: the perspective scene draws of a frame
// report their projection (record), the one used by most of them is the frame's camera, and the clip-to-clip transform
// between the cameras of two consecutive frames is what Streamline reprojects the history with. CPU only, so it
// exists before the first evaluation, which creates DlaaPass.
class DlaaCameraTracker {
public:
    // At most this many distinct cameras are counted per frame; draws with further ones are ignored.
    static constexpr std::size_t kMaxKeys = 16;

    // The result for the frame being drawn: the matrices (identity unless both frames have a camera), and whether
    // the history must be dropped because the camera appeared, disappeared or jumped, or a matrix cannot be inverted.
    struct Result {
        DlaaMatrices matrices;
        bool reset = true;
    };

    void record(const DlaaCameraKey& key);
    // The result for the draws recorded so far, which are the ones the evaluation sees. The camera used (or its
    // absence) is remembered as the next frame's previous camera. `flip_clip_y` mirrors the clip-to-clip transforms
    // in y (the sign of the clip space relative to Streamline's).
    Result resolve(bool flip_clip_y);
    // The frame is over: its camera becomes the previous one and the table starts again. That camera is the one
    // resolve() used; a frame without a resolve() contributes its own dominant key.
    void endFrame();

private:
    struct Entry {
        DlaaCameraKey key;
        std::uint32_t draws = 0;
    };
    const Entry* dominant() const;

    std::vector<Entry> entries_;
    std::optional<DlaaCameraKey> previous_;
    // Set by resolve(); the key it used, if it found one.
    bool resolved_ = false;
    std::optional<DlaaCameraKey> resolved_key_;
};

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

    struct Inputs {
        SlImage color_in, depth;        // both SHADER_READ_ONLY_OPTIMAL, the input size
        std::uint32_t frame_index = 0;  // the renderer's index of this guest frame (>= 1), see DlaaFrame
        // The sub-pixel offset the scene was drawn with, in input pixels (x right, y down), as Streamline wants it
        // reported.
        float jitter_px[2] = {0.0f, 0.0f};
        DlaaMatrices matrices;
        std::optional<float> mvec_invalid_value;
        bool reset = false;
        bool alpha = false;
    };
    // Records one evaluation into resources().output (GENERAL) with resources().motion (SHADER_READ_ONLY_OPTIMAL). All
    // bound command buffer state is invalid afterwards.
    bool evaluate(VkCommandBuffer cmd, const Inputs& inputs, std::string& error);

private:
    VkContext& context_;
    Streamline& streamline_;
    std::uint32_t viewport_ = 0;
    std::shared_ptr<Resources> resources_;
};

}  // namespace tox2
