#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

// volk replaces the Vulkan prototypes (VK_NO_PROTOTYPES): every vk* name is a function pointer
// loaded by createVkContext. Device-level pointers are global, so one context is live at a time.
#include <volk.h>
#include <vk_mem_alloc.h>

namespace tox2 {

struct VkContextOptions {
    bool validation = false;  // enable VK_LAYER_KHRONOS_validation when it is installed
    bool window = false;      // also enable the surface and swapchain extensions
};

// Instance, physical device, logical device with one graphics queue, and the VMA allocator.
// Requires Vulkan 1.3 (dynamic rendering, synchronization2, shader demote to helper invocation,
// extended dynamic state) and VK_KHR_push_descriptor.
class VkContext {
public:
    ~VkContext();
    VkContext(const VkContext&) = delete;
    VkContext& operator=(const VkContext&) = delete;

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physical_device_; }
    VkDevice device() const { return device_; }
    VkQueue graphicsQueue() const { return graphics_queue_; }
    std::uint32_t graphicsQueueFamily() const { return graphics_family_; }
    VmaAllocator allocator() const { return allocator_; }
    const VkPhysicalDeviceProperties& deviceProperties() const { return properties_; }
    std::string_view deviceName() const { return properties_.deviceName; }

    bool hasD24S8() const { return has_d24s8_; }
    // True when the device was created with textureCompressionBC (BC1/BC2/BC3 sampled images).
    bool hasTextureBC() const { return has_texture_bc_; }
    // True when the device was created with shaderClipDistance (gl_ClipDistance in the vertex shader).
    bool hasClipDistance() const { return has_clip_distance_; }
    // True when the device was created with largePoints (gl_PointSize above 1).
    bool hasLargePoints() const { return has_large_points_; }
    // D24_UNORM_S8_UINT when usable as an attachment, else D32_SFLOAT_S8_UINT (one of the two is
    // mandatory for Vulkan implementations).
    VkFormat depthStencilFormat() const {
        return has_d24s8_ ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
    }

    // Empty when 2-sample colour and depth/stencil images can be rendered to and sampled; otherwise the first missing
    // capability (for example "framebufferStencilSampleCounts lacks 2"). 2 samples is not a guaranteed count.
    const std::string& twoSampleSupportGap() const { return two_sample_gap_; }

    // True when the validation layer was requested, installed and its messenger is active.
    bool validationEnabled() const { return messenger_ != VK_NULL_HANDLE; }
    std::uint32_t validationErrorCount() const { return validation_errors_.load(std::memory_order_relaxed); }

private:
    friend std::unique_ptr<VkContext> createVkContext(const VkContextOptions& options, std::string& error);
    VkContext() = default;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;
    VkPhysicalDeviceProperties properties_{};
    std::uint32_t graphics_family_ = 0;
    bool has_d24s8_ = false;
    bool has_texture_bc_ = false;
    bool has_clip_distance_ = false;
    bool has_large_points_ = false;
    std::string two_sample_gap_;
    std::atomic<std::uint32_t> validation_errors_{0};
};

// Returns null and sets `error` when Vulkan cannot be used (no loader, no suitable device, a
// missing required feature). Never fatal.
std::unique_ptr<VkContext> createVkContext(const VkContextOptions& options, std::string& error);

// True when VK_LAYER_KHRONOS_validation is installed. Needs the loader; false without it.
bool vkValidationLayerAvailable();

}  // namespace tox2
