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

#include "render/streamline.h"

namespace tox2 {

struct VkContextOptions {
    bool validation = false;  // enable VK_LAYER_KHRONOS_validation when it is installed
    bool window = false;      // also enable the surface and swapchain extensions
    // Load Streamline and create the instance and device with what DLSS asks for. Failing is not an error: the
    // context is created without it and streamlineStatus() says why.
    bool streamline = false;
    // Also load the frame generation plugins and create the instance and device with what DLSS-G and Reflex ask for.
    // Needs `streamline` and a window to be useful. Failing is not an error and never turns DLAA off:
    // frameGenerationStatus() says why.
    bool frame_generation = false;
};

// Instance, physical device, logical device whose graphics queue 0 is the renderer's (Streamline may add more),
// and the VMA allocator.
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

    VkDriverId driverId() const { return driver_id_; }
    std::string_view driverName() const { return driver_name_; }
    std::string_view driverInfo() const { return driver_info_; }

    // False when triangle fans must be drawn as lists: a portability-subset device without them, or MoltenVK,
    // whose fans break with a dynamic topology.
    bool triangleFans() const { return triangle_fans_; }

    bool hasD24S8() const { return has_d24s8_; }
    // True when the device was created with textureCompressionBC (BC1/BC2/BC3 sampled images).
    bool hasTextureBC() const { return has_texture_bc_; }
    // True when the device was created with shaderClipDistance (gl_ClipDistance in the vertex shader).
    bool hasClipDistance() const { return has_clip_distance_; }
    // True when the device was created with largePoints (gl_PointSize above 1).
    bool hasLargePoints() const { return has_large_points_; }
    // True when the device was created with samplerAnisotropy.
    bool hasSamplerAnisotropy() const { return has_sampler_anisotropy_; }
    // 0 without the feature, else limits.maxSamplerAnisotropy.
    float maxSamplerAnisotropy() const {
        return has_sampler_anisotropy_ ? properties_.limits.maxSamplerAnisotropy : 0.0f;
    }
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

    // Null when DLAA is unavailable: not requested, or Streamline or DLSS cannot be used (see streamlineStatus()).
    Streamline* streamline() const { return streamline_.get(); }
    // "Streamline 2.14.1, DLSS supported", or the reason Streamline is off; empty when it was not requested.
    const std::string& streamlineStatus() const { return streamline_status_; }
    // The instance-level function `name`: Streamline's proxy when it hooks it, else the loader's.
    PFN_vkVoidFunction instanceProc(const char* name) const;

    // True when the device was created with the frame generation requirements and DLSS-G reported generated frames.
    // The plugins are unloaded again at creation; the renderer loads them when a target is set.
    bool frameGenerationSupported() const { return frame_generation_supported_; }
    // "DLSS-G up to 6x, optical flow native|interop", or the reason it is unavailable; empty when not requested.
    const std::string& frameGenerationStatus() const { return frame_generation_status_; }
    // 1 + numFramesToGenerateMax; 0 when unsupported or not known yet.
    std::uint32_t frameGenerationMaxMultiplier() const { return frame_generation_max_multiplier_; }
    // The queue family of the native optical flow queue; valid when frameGenerationNativeOpticalFlow() is true.
    std::uint32_t opticalFlowQueueFamily() const { return optical_flow_family_; }
    bool frameGenerationNativeOpticalFlow() const { return native_optical_flow_; }

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
    VkDriverId driver_id_ = static_cast<VkDriverId>(0);
    std::string driver_name_;
    std::string driver_info_;
    std::uint32_t graphics_family_ = 0;
    bool triangle_fans_ = true;
    bool has_d24s8_ = false;
    bool has_texture_bc_ = false;
    bool has_clip_distance_ = false;
    bool has_large_points_ = false;
    bool has_sampler_anisotropy_ = false;
    std::string two_sample_gap_;
    std::unique_ptr<Streamline> streamline_;
    std::string streamline_status_;
    bool frame_generation_supported_ = false;
    bool native_optical_flow_ = false;
    std::uint32_t frame_generation_max_multiplier_ = 0;
    std::uint32_t optical_flow_family_ = 0;
    std::string frame_generation_status_;
    std::atomic<std::uint32_t> validation_errors_{0};
};

// Returns null and sets `error` when Vulkan cannot be used (no loader, no suitable device, a
// missing required feature). Never fatal.
std::unique_ptr<VkContext> createVkContext(const VkContextOptions& options, std::string& error);

// True when VK_LAYER_KHRONOS_validation is installed. Needs the loader; false without it.
bool vkValidationLayerAvailable();

}  // namespace tox2
