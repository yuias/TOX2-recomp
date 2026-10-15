#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <volk.h>

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

private:
    Streamline();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tox2
