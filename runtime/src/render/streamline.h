#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <volk.h>

#include "render/dlaa_math.h"

namespace tox2 {

// What the DLSS feature asks of the Vulkan instance and device. The extension names are copies; the feature
// structs have their sType set and only the requested members enabled.
struct StreamlineRequirements {
    std::vector<std::string> instance_extensions;
    std::vector<std::string> device_extensions;
    VkPhysicalDeviceVulkan12Features features12{};
    VkPhysicalDeviceVulkan13Features features13{};
    std::uint32_t graphics_queues = 0;  // extra queues in the graphics family
    std::uint32_t compute_queues = 0;   // extra queues in a compute family

    StreamlineRequirements() {
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    }
};

struct StreamlineDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    std::uint32_t graphics_family = 0, graphics_first_queue = 0;  // first queue index Streamline may use
    std::uint32_t compute_family = 0, compute_first_queue = 0;
};

// One image as Streamline must be told about it under manual hooking: it cannot look the handles up itself.
struct SlImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;            // the allocation's device memory (allocations are dedicated)
    VkImageView view = VK_NULL_HANDLE;                 // depth: the depth-aspect view
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;  // the layout when the evaluation executes
    std::uint32_t width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage = 0;  // the image's real usage flags
};

// Everything one DLAA evaluation consumes.
struct DlaaFrame {
    std::uint32_t viewport = 0;  // from Streamline::newViewport()
    SlImage color_in, depth, motion, color_out;
    float jitter_px[2] = {0.0f, 0.0f};   // Constants::jitterOffset
    float mvec_scale[2] = {1.0f, 1.0f};  // 1 / input width, 1 / input height (pixel-space vectors)
    Mat4 view_to_clip = mat4Identity(), clip_to_view = mat4Identity();
    Mat4 clip_to_prev_clip = mat4Identity(), prev_clip_to_clip = mat4Identity();
    // Constants::motionVectorsInvalidValue. Left unset (the SDK default), Streamline warns on every evaluation;
    // unset is kept only for A/B runs.
    std::optional<float> mvec_invalid_value = 0.0f;
    bool reset = false;  // the previous frame has no connection to this one
    bool alpha = false;  // DLSSOptions::alphaUpscalingEnabled
};

// NVIDIA Streamline (DLSS) for the Vulkan context and the DLAA pass. The SDK is loaded at run time from
// sl.interposer.dll next to the executable after its signature is checked; a build without the SDK compiles
// streamline_off.cpp instead, whose load() always fails.
class Streamline {
public:
    // Before vkCreateInstance. Null with `reason` (one line, no trailing period) on any failure.
    static std::unique_ptr<Streamline> load(std::string& reason);
    ~Streamline();  // restores the hooked volk pointers, then slShutdown, then FreeLibrary
    Streamline(const Streamline&) = delete;
    Streamline& operator=(const Streamline&) = delete;

    std::string_view version() const;                                     // SDK headers built against, e.g. "2.14.1"
    bool requirements(StreamlineRequirements& out, std::string& reason);  // slGetFeatureRequirements(kFeatureDLSS)
    // slIsFeatureSupported for DLSS on `device`; needs an instance, so it runs after the device choice.
    bool dlssSupported(VkPhysicalDevice device, std::string& reason);
    // slSetVulkanInfo, then replaces the volk globals of the mandatory hooks with Streamline's proxies.
    bool setDevice(const StreamlineDevice& device, std::string& reason);
    // Streamline's proxy for an instance-level function it hooks (vkCreateWin32SurfaceKHR); null otherwise.
    PFN_vkVoidFunction instanceProxy(const char* name) const;

    // Frame API, for the thread that records commands. Valid after a successful setDevice().

    // A new DLSS viewport id; each viewport keeps its own temporal history.
    std::uint32_t newViewport();
    // Records one DLAA evaluation (colour in, depth, motion vectors -> colour out, all the same size) into `cmd`.
    // The images must already be in the layouts named by their SlImage and must not change until `cmd` has
    // executed. Streamline transitions them internally and returns them to those layouts.
    //
    // Under manual hooking Streamline does not track command-buffer state, so the caller must treat ALL bound state
    // (pipeline, descriptor sets, push descriptors, push constants, dynamic state) as invalid afterwards and bind
    // what the next command needs again. On false, `error` says why and nothing usable has been recorded.
    bool evaluateDlaa(VkCommandBuffer cmd, const DlaaFrame& frame, std::string& error);
    // slFreeResources for the viewport; the caller has waited for the device to be idle.
    void freeViewport(std::uint32_t viewport);

private:
    Streamline();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tox2
