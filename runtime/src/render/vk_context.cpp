// Vulkan bootstrap: loader, instance, device, allocator.

#define VMA_IMPLEMENTATION
#include "render/vk_context.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <format>
#include <iterator>
#include <string>
#include <vector>

#include "core/log.h"

namespace tox2 {
namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr std::uint32_t kRequiredApiVersion = VK_API_VERSION_1_3;

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) == 0) return VK_FALSE;
    static_cast<std::atomic<std::uint32_t>*>(user)->fetch_add(1, std::memory_order_relaxed);
    logError("tox2: render: validation: {}",
             data != nullptr && data->pMessage != nullptr ? data->pMessage : "(no message)");
    return VK_FALSE;
}

bool hasName(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const VkExtensionProperties& ext : list) {
        if (std::strcmp(ext.extensionName, name) == 0) return true;
    }
    return false;
}

std::vector<VkExtensionProperties> instanceExtensions() {
    std::uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> list(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, list.data());
    list.resize(count);
    return list;
}

std::vector<VkExtensionProperties> deviceExtensions(VkPhysicalDevice device) {
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> list(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, list.data());
    list.resize(count);
    return list;
}

bool layerInstalled(const char* name) {
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    layers.resize(count);
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, name) == 0) return true;
    }
    return false;
}

struct Candidate {
    VkPhysicalDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    std::uint32_t graphics_family = 0;
};

// Returns an empty string when `device` is usable, else the reason it is not.
std::string checkDevice(VkPhysicalDevice device, const VkContextOptions& options, Candidate& out) {
    vkGetPhysicalDeviceProperties(device, &out.properties);
    out.device = device;
    if (out.properties.apiVersion < kRequiredApiVersion) return "Vulkan 1.3 is not supported";

    std::uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families.data());
    bool found = false;
    for (std::uint32_t i = 0; i < family_count; i++) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && families[i].queueCount > 0) {
            out.graphics_family = i;
            found = true;
            break;
        }
    }
    if (!found) return "no graphics queue";

    const std::vector<VkExtensionProperties> extensions = deviceExtensions(device);
    if (!hasName(extensions, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
        return "VK_KHR_push_descriptor is missing";
    }
    if (options.window && !hasName(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        return "VK_KHR_swapchain is missing";
    }

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &features13;
    vkGetPhysicalDeviceFeatures2(device, &features);
    // Extended dynamic state is core functionality in 1.3 and has no separate feature bit there.
    if (features13.dynamicRendering == VK_FALSE) return "dynamicRendering is not supported";
    if (features13.synchronization2 == VK_FALSE) return "synchronization2 is not supported";
    // glslang compiles `discard` for Vulkan 1.3 as a demote to a helper invocation.
    if (features13.shaderDemoteToHelperInvocation == VK_FALSE) return "shaderDemoteToHelperInvocation is not supported";
    return {};
}

bool formatUsableAsDepthStencil(VkPhysicalDevice device, VkFormat format) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(device, format, &props);
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
}

// Returns an empty string when images of `format` and `usage` can be created with 2 samples, else why not.
std::string checkTwoSampleImage(VkPhysicalDevice device, VkFormat format, VkImageUsageFlags usage, const char* what) {
    VkImageFormatProperties props{};
    const VkResult result = vkGetPhysicalDeviceImageFormatProperties(device, format, VK_IMAGE_TYPE_2D,
                                                                     VK_IMAGE_TILING_OPTIMAL, usage, 0, &props);
    if (result != VK_SUCCESS) return std::format("{} images are not supported ({})", what, static_cast<int>(result));
    if ((props.sampleCounts & VK_SAMPLE_COUNT_2_BIT) == 0) return std::format("{} images lack 2 samples", what);
    return {};
}

// Returns an empty string when colour and depth/stencil images can be multisampled with 2 samples and sampled as
// such. 2 samples is not a guaranteed count, so a missing one is a reason to fall back, not a device error.
std::string twoSampleGap(VkPhysicalDevice device, const VkPhysicalDeviceLimits& limits, VkFormat depth_stencil_format) {
    struct Limit {
        const char* name;
        VkSampleCountFlags counts;
    };
    const Limit limit_list[] = {
            {"framebufferColorSampleCounts", limits.framebufferColorSampleCounts},
            {"framebufferDepthSampleCounts", limits.framebufferDepthSampleCounts},
            {"framebufferStencilSampleCounts", limits.framebufferStencilSampleCounts},
            {"sampledImageColorSampleCounts", limits.sampledImageColorSampleCounts},
            {"sampledImageDepthSampleCounts", limits.sampledImageDepthSampleCounts},
            {"sampledImageStencilSampleCounts", limits.sampledImageStencilSampleCounts},
    };
    for (const Limit& limit : limit_list) {
        if ((limit.counts & VK_SAMPLE_COUNT_2_BIT) == 0) return std::format("{} lacks 2", limit.name);
    }

    constexpr VkImageUsageFlags kColorUsage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    constexpr VkImageUsageFlags kDepthUsage =
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    struct Format {
        VkFormat format;
        VkImageUsageFlags usage;
        const char* name;
    };
    const Format format_list[] = {
            {VK_FORMAT_B8G8R8A8_UNORM, kColorUsage, "B8G8R8A8_UNORM"},
            {VK_FORMAT_R32_SFLOAT, kColorUsage, "R32_SFLOAT"},
            {depth_stencil_format, kDepthUsage, "depth/stencil"},
            {VK_FORMAT_D16_UNORM, kDepthUsage, "D16_UNORM"},
    };
    for (const Format& format : format_list) {
        std::string gap = checkTwoSampleImage(device, format.format, format.usage, format.name);
        if (!gap.empty()) return gap;
    }
    return {};
}

// Member names of the Vulkan 1.2 and 1.3 feature structs, in declaration order, to name what Streamline asks for.
constexpr const char* kFeatures12Names[] = {
        "samplerMirrorClampToEdge",
        "drawIndirectCount",
        "storageBuffer8BitAccess",
        "uniformAndStorageBuffer8BitAccess",
        "storagePushConstant8",
        "shaderBufferInt64Atomics",
        "shaderSharedInt64Atomics",
        "shaderFloat16",
        "shaderInt8",
        "descriptorIndexing",
        "shaderInputAttachmentArrayDynamicIndexing",
        "shaderUniformTexelBufferArrayDynamicIndexing",
        "shaderStorageTexelBufferArrayDynamicIndexing",
        "shaderUniformBufferArrayNonUniformIndexing",
        "shaderSampledImageArrayNonUniformIndexing",
        "shaderStorageBufferArrayNonUniformIndexing",
        "shaderStorageImageArrayNonUniformIndexing",
        "shaderInputAttachmentArrayNonUniformIndexing",
        "shaderUniformTexelBufferArrayNonUniformIndexing",
        "shaderStorageTexelBufferArrayNonUniformIndexing",
        "descriptorBindingUniformBufferUpdateAfterBind",
        "descriptorBindingSampledImageUpdateAfterBind",
        "descriptorBindingStorageImageUpdateAfterBind",
        "descriptorBindingStorageBufferUpdateAfterBind",
        "descriptorBindingUniformTexelBufferUpdateAfterBind",
        "descriptorBindingStorageTexelBufferUpdateAfterBind",
        "descriptorBindingUpdateUnusedWhilePending",
        "descriptorBindingPartiallyBound",
        "descriptorBindingVariableDescriptorCount",
        "runtimeDescriptorArray",
        "samplerFilterMinmax",
        "scalarBlockLayout",
        "imagelessFramebuffer",
        "uniformBufferStandardLayout",
        "shaderSubgroupExtendedTypes",
        "separateDepthStencilLayouts",
        "hostQueryReset",
        "timelineSemaphore",
        "bufferDeviceAddress",
        "bufferDeviceAddressCaptureReplay",
        "bufferDeviceAddressMultiDevice",
        "vulkanMemoryModel",
        "vulkanMemoryModelDeviceScope",
        "vulkanMemoryModelAvailabilityVisibilityChains",
        "shaderOutputViewportIndex",
        "shaderOutputLayer",
        "subgroupBroadcastDynamicId",
};
constexpr const char* kFeatures13Names[] = {
        "robustImageAccess",
        "inlineUniformBlock",
        "descriptorBindingInlineUniformBlockUpdateAfterBind",
        "pipelineCreationCacheControl",
        "privateData",
        "shaderDemoteToHelperInvocation",
        "shaderTerminateInvocation",
        "subgroupSizeControl",
        "computeFullSubgroups",
        "synchronization2",
        "textureCompressionASTC_HDR",
        "shaderZeroInitializeWorkgroupMemory",
        "dynamicRendering",
        "shaderIntegerDotProduct",
        "maintenance4",
};

template <typename Features>
VkBool32* featureFlags(Features& features) {
    return reinterpret_cast<VkBool32*>(&features.pNext + 1);
}

// Every member of these structs after sType and pNext is a VkBool32; the name lists must cover all of them (the
// struct may be padded to pointer alignment after the last flag).
template <typename Features>
constexpr bool namesCoverFeatures(std::size_t name_count) {
    const std::size_t flag_bytes = name_count * sizeof(VkBool32);
    const std::size_t rest = sizeof(Features) - offsetof(Features, pNext) - sizeof(void*);
    return rest >= flag_bytes && rest - flag_bytes < sizeof(void*);
}

static_assert(namesCoverFeatures<VkPhysicalDeviceVulkan12Features>(std::size(kFeatures12Names)));
static_assert(namesCoverFeatures<VkPhysicalDeviceVulkan13Features>(std::size(kFeatures13Names)));

// Returns the first member enabled in `wanted` and not in `supported`, or an empty string.
template <typename Features, std::size_t N>
std::string firstUnsupportedFeature(Features wanted, Features supported, const char* const (&names)[N]) {
    const VkBool32* wanted_flags = featureFlags(wanted);
    const VkBool32* supported_flags = featureFlags(supported);
    for (std::size_t i = 0; i < N; i++) {
        if (wanted_flags[i] != VK_FALSE && supported_flags[i] == VK_FALSE) return names[i];
    }
    return {};
}

// What createVkContext adds to the device for Streamline.
struct StreamlineDevicePlan {
    std::vector<std::string> extensions;
    VkPhysicalDeviceVulkan12Features features12{};
    VkPhysicalDeviceVulkan13Features features13{};
    std::uint32_t graphics_queue_count = 1;  // queue 0 is the renderer's, the rest are Streamline's
    // Streamline's own device proxy reports index 0 when no extra queue is requested, so this mirrors that
    // instead of pointing past the renderer's only queue.
    std::uint32_t graphics_first_queue = 0;
    std::uint32_t compute_family = 0;
    std::uint32_t compute_queue_count = 0;  // queues created in compute_family when it is not the graphics family
    std::uint32_t compute_first_queue = 0;
    // DLSS-G's optical flow on a queue of its own (set by planNativeOpticalFlow); otherwise it uses CUDA interop.
    bool native_optical_flow = false;
    std::uint32_t optical_flow_family = 0;
};

// The union of two requirement sets for one device: names de-duplicated, features ORed, queue counts summed, the way
// Streamline's interposer combines the plugins' lists when it creates the device itself.
StreamlineRequirements mergeRequirements(const StreamlineRequirements& first, const StreamlineRequirements& second) {
    StreamlineRequirements out = first;
    for (const std::string& name : second.instance_extensions) {
        if (std::find(out.instance_extensions.begin(), out.instance_extensions.end(), name) ==
            out.instance_extensions.end()) {
            out.instance_extensions.push_back(name);
        }
    }
    for (const std::string& name : second.device_extensions) {
        if (std::find(out.device_extensions.begin(), out.device_extensions.end(), name) ==
            out.device_extensions.end()) {
            out.device_extensions.push_back(name);
        }
    }
    const auto orFlags = [](auto& into, auto from, const auto& names) {
        VkBool32* into_flags = featureFlags(into);
        const VkBool32* from_flags = featureFlags(from);
        for (std::size_t i = 0; i < std::size(names); i++) {
            if (from_flags[i] != VK_FALSE) into_flags[i] = VK_TRUE;
        }
    };
    orFlags(out.features12, second.features12, kFeatures12Names);
    orFlags(out.features13, second.features13, kFeatures13Names);
    out.graphics_queues += second.graphics_queues;
    out.compute_queues += second.compute_queues;
    out.optical_flow_queues += second.optical_flow_queues;
    out.vsync_off_required = out.vsync_off_required || second.vsync_off_required;
    out.hws_required = out.hws_required || second.hws_required;
    return out;
}

// VK_NV_low_latency2 requires a present id extension on the device, which Streamline's list leaves out (the
// validation layer rejects vkCreateDevice without one). The extension is only enabled, not used.
// VK_KHR_present_id2 depends on the instance extension VK_KHR_get_surface_capabilities2, which the instance has
// already been created with or without; `surface_capabilities2_enabled` says which. Returns why the dependency cannot
// be met (frame generation is then dropped), else an empty string.
std::string addPresentIdDependency(VkPhysicalDevice device, bool surface_capabilities2_enabled,
                                   StreamlineRequirements& requirements) {
    std::vector<std::string>& names = requirements.device_extensions;
    const auto listed = [&names](const char* name) {
        return std::find(names.begin(), names.end(), name) != names.end();
    };
    if (!listed(VK_NV_LOW_LATENCY_2_EXTENSION_NAME) || listed(VK_KHR_PRESENT_ID_EXTENSION_NAME) ||
        listed(VK_KHR_PRESENT_ID_2_EXTENSION_NAME)) {
        return {};
    }
    // A missing extension is reported by the device plan under the first name.
    const std::vector<VkExtensionProperties> offered = deviceExtensions(device);
    const bool use_second =
            !hasName(offered, VK_KHR_PRESENT_ID_EXTENSION_NAME) && hasName(offered, VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
    if (use_second && !surface_capabilities2_enabled) {
        return std::format("{} needs the instance extension {}, which the instance does not have",
                           VK_KHR_PRESENT_ID_2_EXTENSION_NAME, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    }
    const char* added = use_second ? VK_KHR_PRESENT_ID_2_EXTENSION_NAME : VK_KHR_PRESENT_ID_EXTENSION_NAME;
    names.emplace_back(added);
    logDebug(
            "tox2: render: frame generation requirements: added device extension {}, a dependency of {} that "
            "Streamline's list omits",
            added, VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
    return {};
}

std::string joinNames(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        if (!out.empty()) out += ", ";
        out += name;
    }
    return out.empty() ? "(none)" : out;
}

template <typename Features, std::size_t N>
std::string enabledFeatureNames(Features features, const char* const (&names)[N]) {
    const VkBool32* flags = featureFlags(features);
    std::vector<std::string> list;
    for (std::size_t i = 0; i < N; i++) {
        if (flags[i] != VK_FALSE) list.emplace_back(names[i]);
    }
    return joinNames(list);
}

void logRequirements(const char* label, const StreamlineRequirements& requirements) {
    logDebug("tox2: render: {} requirements: instance extensions: {}", label,
             joinNames(requirements.instance_extensions));
    logDebug("tox2: render: {} requirements: device extensions: {}", label, joinNames(requirements.device_extensions));
    logDebug("tox2: render: {} requirements: Vulkan 1.2 features: {}; 1.3 features: {}", label,
             enabledFeatureNames(requirements.features12, kFeatures12Names),
             enabledFeatureNames(requirements.features13, kFeatures13Names));
    logDebug(
            "tox2: render: {} requirements: extra queues: graphics {}, compute {}, optical flow {}; vsync off {}, "
            "hardware scheduling {}",
            label, requirements.graphics_queues, requirements.compute_queues, requirements.optical_flow_queues,
            requirements.vsync_off_required ? "required" : "not required",
            requirements.hws_required ? "required" : "not required");
    logDebug("tox2: render: {} requirements: OS {} (needs {}), driver {} (needs {})", label, requirements.os_detected,
             requirements.os_required, requirements.driver_detected, requirements.driver_required);
}

// Decides whether DLSS-G can run its optical flow natively. Returns an empty string when it can (fills the plan's
// optical flow members and device extension), else why it falls back to CUDA interop.
std::string planNativeOpticalFlow(VkPhysicalDevice device, std::uint32_t graphics_family, StreamlineDevicePlan& plan) {
    const std::vector<VkExtensionProperties> offered = deviceExtensions(device);
    if (!hasName(offered, VK_NV_OPTICAL_FLOW_EXTENSION_NAME)) return "VK_NV_optical_flow is missing";

    VkPhysicalDeviceOpticalFlowFeaturesNV optical_flow{};
    optical_flow.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &optical_flow;
    vkGetPhysicalDeviceFeatures2(device, &features);
    if (optical_flow.opticalFlow == VK_FALSE) return "the opticalFlow feature is not supported";

    std::uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families.data());
    families.resize(family_count);
    // The rule of Streamline's own helper (sl.chi/vulkan.cpp, getOpticalFlowQueueInfo): the very first
    // optical-flow-capable family, queue 0, and not a family the client's own queues are created in. The family may
    // also carry transfer: the helper does not look at the other bits.
    for (std::uint32_t i = 0; i < family_count; i++) {
        if ((families[i].queueFlags & VK_QUEUE_OPTICAL_FLOW_BIT_NV) == 0) continue;
        if (families[i].queueCount == 0) return "the optical flow queue family has no queue";
        if (i == graphics_family || (plan.compute_queue_count > 0 && i == plan.compute_family)) {
            return "the optical flow queue family is also used for graphics or compute";
        }
        plan.native_optical_flow = true;
        plan.optical_flow_family = i;
        if (std::find(plan.extensions.begin(), plan.extensions.end(), VK_NV_OPTICAL_FLOW_EXTENSION_NAME) ==
            plan.extensions.end()) {
            plan.extensions.emplace_back(VK_NV_OPTICAL_FLOW_EXTENSION_NAME);
        }
        return {};
    }
    return "no queue family has the optical flow bit";
}

// Checks that `device` offers everything DLSS asked for and fills `plan`; returns the reason when it does not.
std::string planStreamlineDevice(VkPhysicalDevice device, std::uint32_t graphics_family,
                                 const StreamlineRequirements& requirements, StreamlineDevicePlan& plan) {
    const std::vector<VkExtensionProperties> offered = deviceExtensions(device);
    for (const std::string& name : requirements.device_extensions) {
        if (!hasName(offered, name.c_str())) return std::format("device extension {} is missing", name);
    }

    VkPhysicalDeviceVulkan12Features supported12{};
    supported12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceVulkan13Features supported13{};
    supported13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    supported12.pNext = &supported13;
    VkPhysicalDeviceFeatures2 supported{};
    supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supported.pNext = &supported12;
    vkGetPhysicalDeviceFeatures2(device, &supported);
    std::string missing = firstUnsupportedFeature(requirements.features12, supported12, kFeatures12Names);
    if (!missing.empty()) return std::format("Vulkan 1.2 feature {} is not supported", missing);
    // Streamline attaches per-object data with vkCreatePrivateDataSlot on a 1.3 device without listing the feature.
    VkPhysicalDeviceVulkan13Features wanted13 = requirements.features13;
    wanted13.privateData = VK_TRUE;
    missing = firstUnsupportedFeature(wanted13, supported13, kFeatures13Names);
    if (!missing.empty()) return std::format("Vulkan 1.3 feature {} is not supported", missing);

    std::uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families.data());
    families.resize(family_count);

    StreamlineDevicePlan result;
    result.graphics_queue_count = 1 + requirements.graphics_queues;
    result.graphics_first_queue = requirements.graphics_queues > 0 ? 1 : 0;
    if (result.graphics_queue_count > families[graphics_family].queueCount) {
        return std::format("the graphics queue family has {} queue(s), DLSS needs {}",
                           families[graphics_family].queueCount, result.graphics_queue_count);
    }
    if (requirements.compute_queues > 0) {
        bool found = false;
        for (std::uint32_t i = 0; i < family_count && !found; i++) {
            const VkQueueFlags flags = families[i].queueFlags;
            if ((flags & VK_QUEUE_COMPUTE_BIT) != 0 && (flags & VK_QUEUE_GRAPHICS_BIT) == 0 &&
                families[i].queueCount >= requirements.compute_queues) {
                result.compute_family = i;
                result.compute_queue_count = requirements.compute_queues;
                result.compute_first_queue = 0;
                found = true;
            }
        }
        if (!found) {
            // Share the graphics family, behind the graphics queues.
            const std::uint32_t needed = result.graphics_queue_count + requirements.compute_queues;
            if (needed > families[graphics_family].queueCount) {
                return std::format("no queue family has {} compute queue(s) for DLSS", requirements.compute_queues);
            }
            result.compute_family = graphics_family;
            result.compute_first_queue = result.graphics_queue_count;
            result.graphics_queue_count = needed;
        }
    } else {
        // Nothing requested: point at the last queue created in the graphics family so the indices stay valid.
        result.compute_family = graphics_family;
        result.compute_first_queue = result.graphics_queue_count - 1;
    }

    result.extensions = requirements.device_extensions;
    result.features12 = requirements.features12;
    result.features13 = wanted13;
    if (result.features12.bufferDeviceAddress != VK_FALSE) {
        // The feature comes from the core struct; the vkCreateDevice rules forbid the EXT extension next to it and
        // next to the KHR one that Streamline also lists.
        std::erase(result.extensions, std::string(VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME));
    }
    plan = std::move(result);
    return {};
}

}  // namespace

PFN_vkVoidFunction VkContext::instanceProc(const char* name) const {
    if (streamline_) {
        if (const PFN_vkVoidFunction proxy = streamline_->instanceProxy(name)) return proxy;
    }
    return vkGetInstanceProcAddr(instance_, name);
}

VkContext::~VkContext() {
    if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
    // Streamline's proxies replace volk's device pointers, so it goes before anything uses them again.
    streamline_.reset();
    if (allocator_ != nullptr) vmaDestroyAllocator(allocator_);
    if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
    if (messenger_ != VK_NULL_HANDLE) vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
}

bool vkValidationLayerAvailable() {
    if (volkInitialize() != VK_SUCCESS) return false;
    return layerInstalled(kValidationLayer);
}

std::unique_ptr<VkContext> createVkContext(const VkContextOptions& options, std::string& error) {
    if (volkInitialize() != VK_SUCCESS) {
        error = "the Vulkan loader was not found";
        return nullptr;
    }
    if (volkGetInstanceVersion() < kRequiredApiVersion) {
        error = "the Vulkan loader is older than 1.3";
        return nullptr;
    }

    std::unique_ptr<VkContext> ctx(new VkContext());

    // Frame generation is wanted while no step has dropped it; it never needs more than DLAA does, so a failure
    // here leaves DLAA alone, while any Streamline failure takes both away.
    bool frame_gen_wanted = options.frame_generation;
    StreamlineRequirements frame_gen_requirements;
    const auto dropFrameGen = [&ctx, &frame_gen_wanted](const std::string& reason) {
        if (!frame_gen_wanted) return;
        frame_gen_wanted = false;
        ctx->frame_generation_status_ = reason;
    };
    // Any Streamline failure only turns DLAA off: the reason is kept and creation goes on without it.
    const auto dropStreamline = [&ctx, &dropFrameGen](const std::string& reason) {
        ctx->streamline_.reset();
        ctx->streamline_status_ = reason;
        dropFrameGen(reason);
    };
    StreamlineRequirements streamline_requirements;
    if (options.streamline) {
        std::string reason;
        ctx->streamline_ = Streamline::load({.frame_generation = options.frame_generation}, reason);
        if (!ctx->streamline_) {
            ctx->streamline_status_ = reason;
            dropFrameGen(reason);
        } else if (!ctx->streamline_->requirements(StreamlineFeature::Dlss, streamline_requirements, reason)) {
            dropStreamline(reason);
        } else if (frame_gen_wanted) {
            // Gathered before the plugins are unloaded at the end of creation.
            if (!ctx->streamline_->requirements(StreamlineFeature::FrameGeneration, frame_gen_requirements, reason)) {
                dropFrameGen(reason);
            } else {
                logRequirements("frame generation", frame_gen_requirements);
            }
        }
    } else {
        dropFrameGen("Streamline is not requested");
    }

    const std::vector<VkExtensionProperties> available = instanceExtensions();
    std::vector<const char*> instance_exts;
    std::vector<const char*> layers;
    bool want_messenger = false;
    if (options.validation && layerInstalled(kValidationLayer) &&
        hasName(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        layers.push_back(kValidationLayer);
        instance_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        want_messenger = true;
    }
    if (options.window) {
        // Literal names: the platform header that defines the win32 macro is not included here.
        for (const char* name : {VK_KHR_SURFACE_EXTENSION_NAME, "VK_KHR_win32_surface"}) {
            if (!hasName(available, name)) {
                error = std::format("instance extension {} is missing", name);
                return nullptr;
            }
            instance_exts.push_back(name);
        }
    }
    if (ctx->streamline_) {
        std::vector<const char*> wanted;
        for (const std::string& name : streamline_requirements.instance_extensions) {
            if (!hasName(available, name.c_str())) {
                dropStreamline(std::format("instance extension {} is missing", name));
                break;
            }
            wanted.push_back(name.c_str());
        }
        if (ctx->streamline_) {
            // Streamline names its objects and marks command ranges through VK_EXT_debug_utils and warns at start-up
            // when the instance lacks it, validation or not. No messenger is needed for that.
            if (hasName(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
                wanted.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            for (const char* name : wanted) {
                const bool listed = std::any_of(instance_exts.begin(), instance_exts.end(),
                                                [name](const char* have) { return std::strcmp(have, name) == 0; });
                if (!listed) instance_exts.push_back(name);
            }
        }
    }
    if (ctx->streamline_ && frame_gen_wanted) {
        // All or nothing: a missing extension drops frame generation, not DLAA.
        std::vector<const char*> wanted;
        for (const std::string& name : frame_gen_requirements.instance_extensions) {
            if (!hasName(available, name.c_str())) {
                dropFrameGen(std::format("instance extension {} is missing", name));
                break;
            }
            wanted.push_back(name.c_str());
        }
        if (frame_gen_wanted) {
            // VK_KHR_present_id2 (the present id extension the device may offer) depends on this one, and Streamline's
            // list does not name it. It is optional: without it the device falls back to VK_KHR_present_id.
            if (hasName(available, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) {
                wanted.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
            }
            for (const char* name : wanted) {
                const bool listed = std::any_of(instance_exts.begin(), instance_exts.end(),
                                                [name](const char* have) { return std::strcmp(have, name) == 0; });
                if (!listed) instance_exts.push_back(name);
            }
        }
    }
    const bool surface_capabilities2_enabled = std::any_of(
            instance_exts.begin(), instance_exts.end(),
            [](const char* have) { return std::strcmp(have, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) == 0; });

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "tox2";
    app.apiVersion = kRequiredApiVersion;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    instance_info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    instance_info.ppEnabledLayerNames = layers.data();
    instance_info.enabledExtensionCount = static_cast<std::uint32_t>(instance_exts.size());
    instance_info.ppEnabledExtensionNames = instance_exts.data();
    VkResult result = vkCreateInstance(&instance_info, nullptr, &ctx->instance_);
    if (result != VK_SUCCESS) {
        ctx->instance_ = VK_NULL_HANDLE;
        error = std::format("vkCreateInstance failed ({})", static_cast<int>(result));
        return nullptr;
    }
    volkLoadInstance(ctx->instance_);

    if (want_messenger) {
        VkDebugUtilsMessengerCreateInfoEXT messenger_info{};
        messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger_info.pfnUserCallback = &debugCallback;
        messenger_info.pUserData = &ctx->validation_errors_;
        if (vkCreateDebugUtilsMessengerEXT(ctx->instance_, &messenger_info, nullptr, &ctx->messenger_) != VK_SUCCESS) {
            ctx->messenger_ = VK_NULL_HANDLE;
        }
    }

    std::uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(ctx->instance_, &device_count, nullptr);
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(ctx->instance_, &device_count, devices.data());
    devices.resize(device_count);
    if (devices.empty()) {
        error = "no Vulkan physical device";
        return nullptr;
    }

    // The first discrete GPU wins; otherwise the first usable device.
    Candidate chosen;
    bool have_choice = false;
    std::string rejected;
    for (VkPhysicalDevice device : devices) {
        Candidate candidate;
        const std::string reason = checkDevice(device, options, candidate);
        if (!reason.empty()) {
            if (!rejected.empty()) rejected += "; ";
            rejected += std::format("{}: {}", candidate.properties.deviceName, reason);
            continue;
        }
        const bool discrete = candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        if (!have_choice) {
            chosen = candidate;
            have_choice = true;
        }
        if (discrete) {
            chosen = candidate;
            break;
        }
    }
    if (!have_choice) {
        error = "no suitable Vulkan device (" + rejected + ")";
        return nullptr;
    }
    ctx->physical_device_ = chosen.device;
    ctx->properties_ = chosen.properties;
    ctx->graphics_family_ = chosen.graphics_family;
    ctx->has_d24s8_ = formatUsableAsDepthStencil(chosen.device, VK_FORMAT_D24_UNORM_S8_UINT);
    if (!ctx->has_d24s8_ && !formatUsableAsDepthStencil(chosen.device, VK_FORMAT_D32_SFLOAT_S8_UINT)) {
        error = "the device supports neither D24S8 nor D32S8 attachments";
        return nullptr;
    }
    ctx->two_sample_gap_ = twoSampleGap(chosen.device, ctx->properties_.limits, ctx->depthStencilFormat());

    StreamlineDevicePlan streamline_plan;
    if (ctx->streamline_) {
        std::string reason;
        if (!ctx->streamline_->featureSupported(StreamlineFeature::Dlss, chosen.device, reason)) {
            dropStreamline(reason);
        } else {
            bool planned = false;
            if (frame_gen_wanted) {
                if (!ctx->streamline_->featureSupported(StreamlineFeature::FrameGeneration, chosen.device, reason)) {
                    dropFrameGen(reason);
                } else {
                    // The device must carry DLSS's and frame generation's requirements together.
                    StreamlineRequirements merged = mergeRequirements(streamline_requirements, frame_gen_requirements);
                    reason = addPresentIdDependency(chosen.device, surface_capabilities2_enabled, merged);
                    if (reason.empty()) {
                        reason = planStreamlineDevice(chosen.device, chosen.graphics_family, merged, streamline_plan);
                    }
                    if (!reason.empty()) {
                        dropFrameGen(reason);
                    } else {
                        planned = true;
                        // Without a native queue DLSS-G runs its optical flow through CUDA interop.
                        reason = frame_gen_requirements.optical_flow_queues > 0
                                         ? planNativeOpticalFlow(chosen.device, chosen.graphics_family, streamline_plan)
                                         : "DLSS-G asks for no optical flow queue";
                        if (!reason.empty()) logDebug("tox2: render: optical flow interop: {}", reason);
                    }
                }
            }
            if (!planned) {
                streamline_plan = {};
                reason = planStreamlineDevice(chosen.device, chosen.graphics_family, streamline_requirements,
                                              streamline_plan);
                if (!reason.empty()) dropStreamline(reason);
            }
        }
    }

    // Runs twice at most: when the device with frame generation's requirements is refused, it is built again with
    // DLAA's alone, so a frame generation failure never takes DLAA away.
    bool native_optical_flow = false;
    for (;;) {
        std::vector<const char*> device_exts = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
        if (options.window) device_exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;
        features13.shaderDemoteToHelperInvocation = VK_TRUE;
        VkPhysicalDeviceVulkan12Features features12 = streamline_plan.features12;
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        VkPhysicalDeviceOpticalFlowFeaturesNV optical_flow_features{};
        optical_flow_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV;
        optical_flow_features.opticalFlow = VK_TRUE;
        if (ctx->streamline_) {
            VkBool32* flags = featureFlags(features13);
            const VkBool32* wanted = featureFlags(streamline_plan.features13);
            for (std::size_t i = 0; i < std::size(kFeatures13Names); i++) {
                if (wanted[i] != VK_FALSE) flags[i] = VK_TRUE;
            }
            for (const std::string& name : streamline_plan.extensions) {
                const bool listed = std::any_of(device_exts.begin(), device_exts.end(),
                                                [&name](const char* have) { return name == have; });
                if (!listed) device_exts.push_back(name.c_str());
            }
            features13.pNext = &features12;
            if (streamline_plan.native_optical_flow) features12.pNext = &optical_flow_features;
        }

        // Optional: DXT guest textures fall back to the placeholder without it. shaderClipDistance is
        // optional too: without it user clip planes are not applied (the translator emits no clip outputs).
        VkPhysicalDeviceFeatures supported_features{};
        vkGetPhysicalDeviceFeatures(chosen.device, &supported_features);
        ctx->has_texture_bc_ = supported_features.textureCompressionBC == VK_TRUE;
        VkPhysicalDeviceFeatures enabled_features{};
        enabled_features.textureCompressionBC = ctx->has_texture_bc_ ? VK_TRUE : VK_FALSE;
        ctx->has_clip_distance_ = supported_features.shaderClipDistance == VK_TRUE;
        enabled_features.shaderClipDistance = ctx->has_clip_distance_ ? VK_TRUE : VK_FALSE;
        // Without largePoints gl_PointSize is clamped to 1, so points shrink as the resolution scale grows.
        ctx->has_large_points_ = supported_features.largePoints == VK_TRUE;
        enabled_features.largePoints = ctx->has_large_points_ ? VK_TRUE : VK_FALSE;
        // Optional: forced anisotropic filtering (an overlay/command-line setting) is unavailable without it.
        ctx->has_sampler_anisotropy_ = supported_features.samplerAnisotropy == VK_TRUE;
        enabled_features.samplerAnisotropy = ctx->has_sampler_anisotropy_ ? VK_TRUE : VK_FALSE;

        const std::uint32_t graphics_queue_count = ctx->streamline_ ? streamline_plan.graphics_queue_count : 1;
        const std::vector<float> priorities(std::max(graphics_queue_count, streamline_plan.compute_queue_count), 1.0f);
        VkDeviceQueueCreateInfo queue_infos[3]{};
        queue_infos[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_infos[0].queueFamilyIndex = chosen.graphics_family;
        queue_infos[0].queueCount = graphics_queue_count;
        queue_infos[0].pQueuePriorities = priorities.data();
        std::uint32_t queue_info_count = 1;
        if (ctx->streamline_ && streamline_plan.compute_queue_count > 0) {
            queue_infos[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue_infos[1].queueFamilyIndex = streamline_plan.compute_family;
            queue_infos[1].queueCount = streamline_plan.compute_queue_count;
            queue_infos[1].pQueuePriorities = priorities.data();
            queue_info_count = 2;
        }
        native_optical_flow = ctx->streamline_ && streamline_plan.native_optical_flow;
        if (native_optical_flow) {
            // Queue 0 of its own family: Streamline's optical flow queue rules (manual hooking guide, 5.2.1).
            queue_infos[queue_info_count].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue_infos[queue_info_count].queueFamilyIndex = streamline_plan.optical_flow_family;
            queue_infos[queue_info_count].queueCount = 1;
            queue_infos[queue_info_count].pQueuePriorities = priorities.data();
            queue_info_count++;
        }

        VkDeviceCreateInfo device_info{};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.pNext = &features13;
        device_info.pEnabledFeatures = &enabled_features;
        device_info.queueCreateInfoCount = queue_info_count;
        device_info.pQueueCreateInfos = queue_infos;
        device_info.enabledExtensionCount = static_cast<std::uint32_t>(device_exts.size());
        device_info.ppEnabledExtensionNames = device_exts.data();
        result = vkCreateDevice(chosen.device, &device_info, nullptr, &ctx->device_);
        if (result == VK_SUCCESS) break;
        ctx->device_ = VK_NULL_HANDLE;
        if (ctx->streamline_ && frame_gen_wanted) {
            dropFrameGen(std::format("vkCreateDevice failed ({}) with the frame generation requirements",
                                     static_cast<int>(result)));
            streamline_plan = {};
            const std::string reason = planStreamlineDevice(chosen.device, chosen.graphics_family,
                                                            streamline_requirements, streamline_plan);
            if (!reason.empty()) dropStreamline(reason);
            continue;
        }
        error = std::format("vkCreateDevice failed ({})", static_cast<int>(result));
        return nullptr;
    }
    volkLoadDevice(ctx->device_);
    vkGetDeviceQueue(ctx->device_, chosen.graphics_family, 0, &ctx->graphics_queue_);

    if (ctx->streamline_) {
        StreamlineDevice sl_device;
        sl_device.instance = ctx->instance_;
        sl_device.physical_device = chosen.device;
        sl_device.device = ctx->device_;
        sl_device.graphics_family = chosen.graphics_family;
        sl_device.graphics_first_queue = streamline_plan.graphics_first_queue;
        sl_device.compute_family = streamline_plan.compute_family;
        sl_device.compute_first_queue = streamline_plan.compute_first_queue;
        sl_device.presents = options.window;
        sl_device.native_optical_flow = native_optical_flow;
        sl_device.optical_flow_family = streamline_plan.optical_flow_family;
        sl_device.optical_flow_queue = 0;
        std::string reason;
        if (ctx->streamline_->setDevice(sl_device, reason)) {
            ctx->streamline_status_ = std::format("Streamline {}, DLSS supported", ctx->streamline_->version());
        } else {
            dropStreamline(reason);
        }
    }

    // The plugins were loaded at slInit so that their requirements could be read. Unloading them now keeps their
    // hooks off the swapchain until a target is set (the off path stays as it is without frame generation).
    if (ctx->streamline_ && options.frame_generation) {
        std::string reason;
        if (frame_gen_wanted) {
            FrameGenState state;
            if (!ctx->streamline_->frameGenState(state, nullptr, reason)) {
                // No swapchain yet: the maximum is read at the first load instead and stays unknown until then.
                logDebug("tox2: render: DLSS-G state is not available before the first swapchain: {}", reason);
            } else if (state.generated_max == 0) {
                dropFrameGen("multi frame generation reports no generated frames");
            } else {
                ctx->frame_generation_max_multiplier_ = 1 + state.generated_max;
            }
        }
        if (!ctx->streamline_->setFrameGenerationLoaded(false, reason)) {
            // Only worth a warning when frame generation was still on offer; otherwise the plugins may never have
            // loaded and the reason is already recorded.
            if (frame_gen_wanted) {
                logWarn("tox2: render: frame generation plugins could not be unloaded: {}", reason);
            } else {
                logDebug("tox2: render: frame generation plugins could not be unloaded: {}", reason);
            }
            dropFrameGen("the frame generation plugins could not be unloaded");
        }
    }
    if (options.frame_generation) {
        if (frame_gen_wanted) {
            ctx->frame_generation_supported_ = true;
            ctx->native_optical_flow_ = native_optical_flow;
            ctx->optical_flow_family_ = streamline_plan.optical_flow_family;
            const char* optical_flow = native_optical_flow ? "native" : "interop";
            ctx->frame_generation_status_ = ctx->frame_generation_max_multiplier_ > 0
                                                    ? std::format("DLSS-G up to {}x, optical flow {}",
                                                                  ctx->frame_generation_max_multiplier_, optical_flow)
                                                    : std::format("DLSS-G, optical flow {}", optical_flow);
        } else {
            ctx->frame_generation_max_multiplier_ = 0;
        }
    }

    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo allocator_info{};
    allocator_info.physicalDevice = chosen.device;
    allocator_info.device = ctx->device_;
    allocator_info.instance = ctx->instance_;
    allocator_info.vulkanApiVersion = kRequiredApiVersion;
    allocator_info.pVulkanFunctions = &functions;
    result = vmaCreateAllocator(&allocator_info, &ctx->allocator_);
    if (result != VK_SUCCESS) {
        ctx->allocator_ = nullptr;
        error = std::format("vmaCreateAllocator failed ({})", static_cast<int>(result));
        return nullptr;
    }
    return ctx;
}

}  // namespace tox2
