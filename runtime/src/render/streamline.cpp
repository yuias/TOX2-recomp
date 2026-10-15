// NVIDIA Streamline loader. This is the only translation unit that sees the Streamline headers and <windows.h>.
// sl.interposer.dll is loaded at run time instead of linking its import library: that library exports the Vulkan
// entry-point names, which volk already defines as global function pointers.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "render/streamline.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// The Streamline helpers use the Vulkan types without including them.
#include <volk.h>

#include <sl.h>
#include <sl_helpers.h>
#include <sl_helpers_vk.h>
#include <sl_dlss.h>
// Defines non-inline functions and globals: include in this translation unit only.
#include <sl_security.h>

#include "core/log.h"

namespace tox2 {
namespace {

// Fixed for this project; Streamline uses it to identify the application.
constexpr const char* kStreamlineProjectId = "b3d6f6c2-7a4e-4f1b-9c55-2e8d0a6f4c17";
constexpr const char* kInterposerName = "sl.interposer.dll";

std::string resultName(sl::Result result) {
    std::string_view name = sl::getResultAsStr(result);
    constexpr std::string_view kPrefix = "Result::";
    if (name.starts_with(kPrefix)) name.remove_prefix(kPrefix.size());
    return std::string(name);
}

void logCallback(sl::LogType type, const char* message) {
    std::string_view text = message != nullptr ? message : "";
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
    // Streamline warns once per process when the calling thread's id is above 65536 and switches to a slower lookup.
    // Thread ids are the OS's choice, so the line comes and goes between runs and says nothing about our use.
    if (type == sl::LogType::eWarn && text.find("Thread id over 65536") != std::string_view::npos) {
        type = sl::LogType::eInfo;
    }
    switch (type) {
        case sl::LogType::eInfo: logDebug("tox2: streamline: {}", text); break;
        case sl::LogType::eWarn: logWarn("tox2: streamline: {}", text); break;
        default: logError("tox2: streamline: {}", text); break;
    }
}

std::filesystem::path executablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

template <typename Pointer>
Pointer resolve(HMODULE module, const char* name) {
    return reinterpret_cast<Pointer>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

std::vector<std::string> copyNames(std::uint32_t count, const char** names) {
    std::vector<std::string> out;
    for (std::uint32_t i = 0; names != nullptr && i < count; i++) {
        if (names[i] != nullptr) out.emplace_back(names[i]);
    }
    return out;
}

sl::float4x4 toFloat4x4(const Mat4& m) {
    sl::float4x4 out;
    for (std::uint32_t row = 0; row < 4; row++) {
        out.setRow(row, sl::float4(m[row * 4 + 0], m[row * 4 + 1], m[row * 4 + 2], m[row * 4 + 3]));
    }
    return out;
}

sl::Resource toResource(const SlImage& image) {
    sl::Resource resource(sl::ResourceType::eTex2d, static_cast<void*>(image.image), static_cast<void*>(image.memory),
                          static_cast<void*>(image.view), static_cast<std::uint32_t>(image.layout));
    resource.width = image.width;
    resource.height = image.height;
    resource.nativeFormat = static_cast<std::uint32_t>(image.format);
    resource.mipLevels = 1;
    resource.arrayLayers = 1;
    resource.flags = 0;
    resource.usage = static_cast<std::uint32_t>(image.usage);
    return resource;
}

}  // namespace

struct Streamline::Impl {
    // The interposer's exports; kept out of the global namespace so they never meet the SDK's declarations.
    struct Api {
        PFun_slInit* init = nullptr;
        PFun_slShutdown* shutdown = nullptr;
        PFun_slIsFeatureSupported* is_feature_supported = nullptr;
        PFun_slGetFeatureRequirements* get_feature_requirements = nullptr;
        PFun_slSetVulkanInfo* set_vulkan_info = nullptr;
        PFun_slGetFeatureFunction* get_feature_function = nullptr;
        PFun_slGetNewFrameToken* get_new_frame_token = nullptr;
        PFun_slSetConstants* set_constants = nullptr;
        PFun_slSetTagForFrame* set_tag_for_frame = nullptr;
        PFun_slEvaluateFeature* evaluate_feature = nullptr;
        PFun_slFreeResources* free_resources = nullptr;
        PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
        PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
    };

    // A volk global replaced by Streamline's proxy; `saved` is put back on destruction.
    struct Hook {
        PFN_vkVoidFunction* global;
        PFN_vkVoidFunction saved;
    };

    HMODULE module = nullptr;
    bool initialized = false;
    bool device_set = false;
    Api api;
    std::wstring plugin_dir;
    const wchar_t* plugin_paths[1] = {nullptr};
    std::string version;
    std::vector<Hook> hooks;
    PFN_vkVoidFunction create_surface_proxy = nullptr;

    // The DLSS options last sent for a viewport; slDLSSSetOptions is called only when they change.
    struct ViewportOptions {
        std::uint32_t width = 0, height = 0;
        bool alpha = false;
    };
    std::unordered_map<std::uint32_t, ViewportOptions> viewport_options;
    std::uint32_t next_viewport = 0;
    PFun_slDLSSSetOptions* dlss_set_options = nullptr;

    ~Impl() {
        // Streamline's proxies go away with the interposer, so the volk globals must not outlive it.
        for (const Hook& hook : hooks) *hook.global = hook.saved;
        if (initialized) api.shutdown();
        if (module != nullptr) FreeLibrary(module);
    }
};

Streamline::Streamline() : impl_(std::make_unique<Impl>()) {}
Streamline::~Streamline() = default;

std::unique_ptr<Streamline> Streamline::load(std::string& reason) {
    const std::filesystem::path exe = executablePath();
    if (exe.empty()) {
        reason = "the executable path is unavailable";
        return nullptr;
    }
    const std::filesystem::path directory = exe.parent_path();
    const std::filesystem::path interposer = directory / kInterposerName;
    std::error_code ec;
    if (!std::filesystem::exists(interposer, ec)) {
        reason = std::string(kInterposerName) + " not found next to the executable";
        return nullptr;
    }
    // The SDK requires the full path of an NVIDIA-signed module before it is loaded.
    if (!sl::security::verifyEmbeddedSignature(interposer.c_str())) {
        reason = std::string(kInterposerName) + " is not signed by NVIDIA";
        return nullptr;
    }

    std::unique_ptr<Streamline> streamline(new Streamline());
    Impl& impl = *streamline->impl_;
    impl.module = LoadLibraryExW(interposer.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (impl.module == nullptr) {
        reason = std::format("{} could not be loaded (error {})", kInterposerName, GetLastError());
        return nullptr;
    }

    Impl::Api& api = impl.api;
    api.init = resolve<PFun_slInit*>(impl.module, "slInit");
    api.shutdown = resolve<PFun_slShutdown*>(impl.module, "slShutdown");
    api.is_feature_supported = resolve<PFun_slIsFeatureSupported*>(impl.module, "slIsFeatureSupported");
    api.get_feature_requirements = resolve<PFun_slGetFeatureRequirements*>(impl.module, "slGetFeatureRequirements");
    api.set_vulkan_info = resolve<PFun_slSetVulkanInfo*>(impl.module, "slSetVulkanInfo");
    api.get_feature_function = resolve<PFun_slGetFeatureFunction*>(impl.module, "slGetFeatureFunction");
    api.get_new_frame_token = resolve<PFun_slGetNewFrameToken*>(impl.module, "slGetNewFrameToken");
    api.set_constants = resolve<PFun_slSetConstants*>(impl.module, "slSetConstants");
    api.set_tag_for_frame = resolve<PFun_slSetTagForFrame*>(impl.module, "slSetTagForFrame");
    api.evaluate_feature = resolve<PFun_slEvaluateFeature*>(impl.module, "slEvaluateFeature");
    api.free_resources = resolve<PFun_slFreeResources*>(impl.module, "slFreeResources");
    api.get_instance_proc_addr = resolve<PFN_vkGetInstanceProcAddr>(impl.module, "vkGetInstanceProcAddr");
    api.get_device_proc_addr = resolve<PFN_vkGetDeviceProcAddr>(impl.module, "vkGetDeviceProcAddr");
    if (api.init == nullptr || api.shutdown == nullptr || api.is_feature_supported == nullptr ||
        api.get_feature_requirements == nullptr || api.set_vulkan_info == nullptr ||
        api.get_feature_function == nullptr || api.get_new_frame_token == nullptr || api.set_constants == nullptr ||
        api.set_tag_for_frame == nullptr || api.evaluate_feature == nullptr || api.free_resources == nullptr ||
        api.get_instance_proc_addr == nullptr || api.get_device_proc_addr == nullptr) {
        reason = std::string(kInterposerName) + " lacks a required export";
        return nullptr;
    }

    impl.plugin_dir = directory.wstring();
    impl.plugin_paths[0] = impl.plugin_dir.c_str();
    const sl::Feature features[] = {sl::kFeatureDLSS};

    sl::Preferences preferences;
    preferences.showConsole = false;
    preferences.logLevel = logEnabled(LogLevel::Debug) ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    preferences.pathsToPlugins = impl.plugin_paths;
    preferences.numPathsToPlugins = 1;
    preferences.pathToLogsAndData = nullptr;
    preferences.logMessageCallback = &logCallback;
    // Manual hooking: the host creates the instance and device. No OTA updates and no downloaded plugins, so only
    // the DLLs shipped next to the executable are used.
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                        sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = 1;
    preferences.applicationId = 0;
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "tox2";
    preferences.projectId = kStreamlineProjectId;
    preferences.renderAPI = sl::RenderAPI::eVulkan;

    const sl::Result result = api.init(preferences, sl::kSDKVersion);
    if (result != sl::Result::eOk) {
        reason = "slInit failed: " + resultName(result);
        return nullptr;
    }
    impl.initialized = true;
    impl.version = std::format("{}.{}.{}", SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH);
    return streamline;
}

std::string_view Streamline::version() const { return impl_->version; }

bool Streamline::requirements(StreamlineRequirements& out, std::string& reason) {
    sl::FeatureRequirements requirements;
    const sl::Result result = impl_->api.get_feature_requirements(sl::kFeatureDLSS, requirements);
    if (result != sl::Result::eOk) {
        reason = "DLSS is unavailable: " + resultName(result);
        return false;
    }
    if (!(requirements.flags & sl::FeatureRequirementFlags::eVulkanSupported)) {
        reason = "DLSS does not support Vulkan";
        return false;
    }
    out.instance_extensions = copyNames(requirements.vkNumInstanceExtensions, requirements.vkInstanceExtensions);
    out.device_extensions = copyNames(requirements.vkNumDeviceExtensions, requirements.vkDeviceExtensions);
    out.features12 = sl::getVkPhysicalDeviceVulkan12Features(requirements.vkNumFeatures12, requirements.vkFeatures12);
    out.features13 = sl::getVkPhysicalDeviceVulkan13Features(requirements.vkNumFeatures13, requirements.vkFeatures13);
    out.graphics_queues = requirements.vkNumGraphicsQueuesRequired;
    out.compute_queues = requirements.vkNumComputeQueuesRequired;
    return true;
}

bool Streamline::dlssSupported(VkPhysicalDevice device, std::string& reason) {
    VkPhysicalDeviceIDProperties id{};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &id;
    vkGetPhysicalDeviceProperties2(device, &properties);

    // The LUID names the adapter for the driver check; without one Streamline takes the physical device.
    sl::AdapterInfo adapter;
    if (id.deviceLUIDValid == VK_TRUE) {
        adapter.deviceLUID = id.deviceLUID;
        adapter.deviceLUIDSizeInBytes = VK_LUID_SIZE;
    } else {
        adapter.vkPhysicalDevice = device;
    }
    const sl::Result result = impl_->api.is_feature_supported(sl::kFeatureDLSS, adapter);
    if (result != sl::Result::eOk) {
        reason = "DLSS is not supported on this GPU: " + resultName(result);
        return false;
    }
    return true;
}

bool Streamline::setDevice(const StreamlineDevice& device, std::string& reason) {
    Impl& impl = *impl_;
    if (impl.device_set) {
        reason = "the device is already set";
        return false;
    }

    sl::VulkanInfo info;
    info.device = device.device;
    info.instance = device.instance;
    info.physicalDevice = device.physical_device;
    info.computeQueueIndex = device.compute_first_queue;
    info.computeQueueFamily = device.compute_family;
    info.graphicsQueueIndex = device.graphics_first_queue;
    info.graphicsQueueFamily = device.graphics_family;
    const sl::Result result = impl.api.set_vulkan_info(info);
    if (result != sl::Result::eOk) {
        reason = "slSetVulkanInfo failed: " + resultName(result);
        return false;
    }
    impl.device_set = true;

    // Every proxy is looked up before any volk global is replaced, so a failure leaves them untouched. A missing
    // proxy is tolerated only where volk has no pointer either (the extension is not enabled on this device).
    struct Candidate {
        const char* name;
        bool device_level;
        PFN_vkVoidFunction* global;  // null for functions volk does not declare
    };
    const Candidate candidates[] = {
            {"vkQueuePresentKHR", true, reinterpret_cast<PFN_vkVoidFunction*>(&vkQueuePresentKHR)},
            {"vkCreateSwapchainKHR", true, reinterpret_cast<PFN_vkVoidFunction*>(&vkCreateSwapchainKHR)},
            {"vkDestroySwapchainKHR", true, reinterpret_cast<PFN_vkVoidFunction*>(&vkDestroySwapchainKHR)},
            {"vkGetSwapchainImagesKHR", true, reinterpret_cast<PFN_vkVoidFunction*>(&vkGetSwapchainImagesKHR)},
            {"vkAcquireNextImageKHR", true, reinterpret_cast<PFN_vkVoidFunction*>(&vkAcquireNextImageKHR)},
            {"vkDeviceWaitIdle", true, reinterpret_cast<PFN_vkVoidFunction*>(&vkDeviceWaitIdle)},
            {"vkDestroySurfaceKHR", false, reinterpret_cast<PFN_vkVoidFunction*>(&vkDestroySurfaceKHR)},
            {"vkCreateWin32SurfaceKHR", false, nullptr},
    };
    std::vector<Impl::Hook> hooks;
    std::vector<PFN_vkVoidFunction> proxies;
    PFN_vkVoidFunction surface_proxy = nullptr;
    for (const Candidate& candidate : candidates) {
        const PFN_vkVoidFunction proxy = candidate.device_level
                                                 ? impl.api.get_device_proc_addr(device.device, candidate.name)
                                                 : impl.api.get_instance_proc_addr(device.instance, candidate.name);
        if (proxy == nullptr && (candidate.global == nullptr || *candidate.global != nullptr)) {
            reason = std::string("Streamline provides no proxy for ") + candidate.name;
            return false;
        }
        if (candidate.global == nullptr) {
            surface_proxy = proxy;
            continue;
        }
        // The extension is not enabled on this device, so there is nothing to hook.
        if (proxy == nullptr) continue;
        hooks.push_back({candidate.global, *candidate.global});
        proxies.push_back(proxy);
    }
    for (std::size_t i = 0; i < hooks.size(); i++) *hooks[i].global = proxies[i];
    impl.hooks = std::move(hooks);
    impl.create_surface_proxy = surface_proxy;
    return true;
}

PFN_vkVoidFunction Streamline::instanceProxy(const char* name) const {
    if (name != nullptr && std::strcmp(name, "vkCreateWin32SurfaceKHR") == 0) return impl_->create_surface_proxy;
    return nullptr;
}

std::uint32_t Streamline::newViewport() { return impl_->next_viewport++; }

bool Streamline::evaluateDlaa(VkCommandBuffer cmd, const DlaaFrame& frame, std::string& error) {
    Impl& impl = *impl_;
    if (!impl.device_set) {
        error = "the Streamline device is not set";
        return false;
    }
    if (cmd == VK_NULL_HANDLE) {
        error = "no command buffer";
        return false;
    }
    const std::uint32_t width = frame.color_in.width;
    const std::uint32_t height = frame.color_in.height;
    if (width == 0 || height == 0) {
        error = "the input image has no size";
        return false;
    }

    if (impl.dlss_set_options == nullptr) {
        void* function = nullptr;
        const sl::Result result = impl.api.get_feature_function(sl::kFeatureDLSS, "slDLSSSetOptions", function);
        if (result != sl::Result::eOk || function == nullptr) {
            error = "slDLSSSetOptions is unavailable: " + resultName(result);
            return false;
        }
        impl.dlss_set_options = reinterpret_cast<PFun_slDLSSSetOptions*>(function);
    }

    const sl::ViewportHandle viewport(frame.viewport);
    const Impl::ViewportOptions wanted{width, height, frame.alpha};
    const auto known = impl.viewport_options.find(frame.viewport);
    if (known == impl.viewport_options.end() || known->second.width != wanted.width ||
        known->second.height != wanted.height || known->second.alpha != wanted.alpha) {
        // DLAA keeps the input size: the output is the same size, so nothing is upscaled.
        sl::DLSSOptions options;
        options.mode = sl::DLSSMode::eDLAA;
        options.outputWidth = width;
        options.outputHeight = height;
        options.preExposure = 1.0f;
        options.exposureScale = 1.0f;
        options.colorBuffersHDR = sl::Boolean::eFalse;
        options.useAutoExposure = sl::Boolean::eTrue;
        options.alphaUpscalingEnabled = frame.alpha ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        const sl::Result result = impl.dlss_set_options(viewport, options);
        if (result != sl::Result::eOk) {
            error = "slDLSSSetOptions failed: " + resultName(result);
            return false;
        }
        impl.viewport_options[frame.viewport] = wanted;
    }

    sl::FrameToken* token = nullptr;
    sl::Result result = impl.api.get_new_frame_token(token, nullptr);
    if (result != sl::Result::eOk || token == nullptr) {
        error = "slGetNewFrameToken failed: " + resultName(result);
        return false;
    }

    sl::Constants constants;
    constants.cameraViewToClip = toFloat4x4(frame.view_to_clip);
    constants.clipToCameraView = toFloat4x4(frame.clip_to_view);
    constants.clipToPrevClip = toFloat4x4(frame.clip_to_prev_clip);
    constants.prevClipToClip = toFloat4x4(frame.prev_clip_to_clip);
    constants.jitterOffset = sl::float2(frame.jitter_px[0], frame.jitter_px[1]);
    constants.mvecScale = sl::float2(frame.mvec_scale[0], frame.mvec_scale[1]);
    constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    // DLAA reads none of the camera description, but the struct asks for all of it.
    constants.cameraPos = sl::float3(0.0f, 0.0f, 0.0f);
    constants.cameraUp = sl::float3(0.0f, 1.0f, 0.0f);
    constants.cameraRight = sl::float3(1.0f, 0.0f, 0.0f);
    constants.cameraFwd = sl::float3(0.0f, 0.0f, 1.0f);
    constants.cameraNear = 1.0f;
    constants.cameraFar = 1000.0f;
    constants.cameraFOV = 1.0f;
    constants.cameraAspectRatio = static_cast<float>(width) / static_cast<float>(height);
    if (frame.mvec_invalid_value) constants.motionVectorsInvalidValue = *frame.mvec_invalid_value;
    constants.depthInverted = sl::Boolean::eFalse;
    constants.cameraMotionIncluded = sl::Boolean::eFalse;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = frame.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.orthographicProjection = sl::Boolean::eFalse;
    constants.motionVectorsDilated = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    result = impl.api.set_constants(constants, *token, viewport);
    if (result != sl::Result::eOk) {
        error = "slSetConstants failed: " + resultName(result);
        return false;
    }

    sl::Resource color_in = toResource(frame.color_in);
    sl::Resource depth = toResource(frame.depth);
    sl::Resource motion = toResource(frame.motion);
    sl::Resource color_out = toResource(frame.color_out);
    // The images stay untouched until the evaluation has been recorded; whole resources, no sub-rectangle.
    const sl::ResourceTag tags[] = {
            sl::ResourceTag(&color_in, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate),
            sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate),
            sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate),
            sl::ResourceTag(&color_out, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate),
    };
    result = impl.api.set_tag_for_frame(*token, viewport, tags, static_cast<std::uint32_t>(std::size(tags)), cmd);
    if (result != sl::Result::eOk) {
        error = "slSetTagForFrame failed: " + resultName(result);
        return false;
    }

    const sl::BaseStructure* inputs[] = {&viewport};
    result = impl.api.evaluate_feature(sl::kFeatureDLSS, *token, inputs, 1, cmd);
    if (result != sl::Result::eOk) {
        error = "slEvaluateFeature failed: " + resultName(result);
        return false;
    }
    return true;
}

void Streamline::freeViewport(std::uint32_t viewport) {
    Impl& impl = *impl_;
    if (!impl.device_set) return;
    impl.viewport_options.erase(viewport);
    const sl::Result result = impl.api.free_resources(sl::kFeatureDLSS, sl::ViewportHandle(viewport));
    if (result != sl::Result::eOk) logWarn("tox2: streamline: slFreeResources failed: {}", resultName(result));
}

}  // namespace tox2
