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

#include <algorithm>
#include <atomic>
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
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
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

// Read by Streamline's logging threads; set by setDevice(). True until then so that the message below is never
// hidden by accident.
std::atomic<bool> g_context_presents{true};

void logCallback(sl::LogType type, const char* message) {
    std::string_view text = message != nullptr ? message : "";
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
    // Streamline reports once, after 60 evaluations, that presentCommon() was never observed and so its garbage
    // collection will not run. A windowless context never presents, and its tags are eValidUntilEvaluate so nothing
    // waits for that collection. With a window the line is the only sign that presents bypass Streamline's proxy.
    if (type != sl::LogType::eInfo && !g_context_presents.load(std::memory_order_relaxed) &&
        text.find("presentCommon() was not observed") != std::string_view::npos) {
        type = sl::LogType::eInfo;
    }
    // Streamline warns once per process when the calling thread's id is above 65536 and switches to a slower lookup.
    // Thread ids are the OS's choice, so the line comes and goes between runs and says nothing about our use.
    // sl.common's plugin config asks for CmdBindPipeline, CmdBindDescriptorSets and BeginCommandBuffer hooks that the
    // plugin manager of this SDK release has no hook ids for, so it warns at start-up whatever the integration mode.
    // Streamline therefore never restores command-buffer state; the renderer rebinds everything after an evaluation.
    if (type == sl::LogType::eWarn && (text.find("Thread id over 65536") != std::string_view::npos ||
                                       text.find("Hook sl.common:Vulkan:") != std::string_view::npos)) {
        type = sl::LogType::eInfo;
    }
    // sl.common tries to load the Reflex DLL at start-up. It is not shipped (its licence does not list it as
    // distributable) and DLAA does not use it, so the failure is expected.
    if (type != sl::LogType::eInfo && text.find("NvLowLatencyVk") != std::string_view::npos) type = sl::LogType::eInfo;
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

// Counts the Vulkan errors DLSS-G reports from vkQueuePresentKHR and vkAcquireNextImageKHR; one context at a time.
std::atomic<std::uint32_t> g_frame_gen_api_errors{0};

void onFrameGenApiError(const sl::APIError& /*error*/) {
    g_frame_gen_api_errors.fetch_add(1, std::memory_order_relaxed);
}

sl::PCLMarker toPclMarker(LatencyMarker marker) {
    switch (marker) {
        case LatencyMarker::SimulationStart: return sl::PCLMarker::eSimulationStart;
        case LatencyMarker::SimulationEnd: return sl::PCLMarker::eSimulationEnd;
        case LatencyMarker::RenderSubmitStart: return sl::PCLMarker::eRenderSubmitStart;
        case LatencyMarker::RenderSubmitEnd: return sl::PCLMarker::eRenderSubmitEnd;
        case LatencyMarker::PresentStart: return sl::PCLMarker::ePresentStart;
        case LatencyMarker::PresentEnd: return sl::PCLMarker::ePresentEnd;
    }
    return sl::PCLMarker::eSimulationStart;
}

void appendUnique(std::vector<std::string>& into, const std::vector<std::string>& names) {
    for (const std::string& name : names) {
        if (std::find(into.begin(), into.end(), name) == into.end()) into.push_back(name);
    }
}

// The members of one feature's requirements that are merged across features.
struct GatheredRequirements {
    std::vector<std::string> instance_extensions, device_extensions, features12, features13;
    std::uint32_t graphics_queues = 0, compute_queues = 0, optical_flow_queues = 0;
    bool vsync_off_required = false, hws_required = false;
    std::string os_detected, os_required, driver_detected, driver_required;
};

sl::DLSSGOptions makeDlssgOptions(const FrameGenOptions& options, bool estimate) {
    sl::DLSSGOptions out;
    out.mode = options.on || estimate ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    out.numFramesToGenerate = std::max(options.generated, 1u);
    // Retained resources avoid reallocation stutter whenever generation is switched off between frames.
    out.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    if (options.only_generated) out.flags |= sl::DLSSGFlags::eShowOnlyInterpolatedFrame;
    if (estimate) out.flags |= sl::DLSSGFlags::eRequestVRAMEstimate;
    out.numBackBuffers = options.back_buffers;
    out.mvecDepthWidth = options.input_width;
    out.mvecDepthHeight = options.input_height;
    out.colorWidth = options.color_width;
    out.colorHeight = options.color_height;
    out.colorBufferFormat = static_cast<std::uint32_t>(options.color_format);
    out.mvecBufferFormat = static_cast<std::uint32_t>(options.motion_format);
    out.depthBufferFormat = static_cast<std::uint32_t>(options.depth_format);
    out.onErrorCallback = &onFrameGenApiError;
    return out;
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
        PFun_slIsFeatureLoaded* is_feature_loaded = nullptr;
        PFun_slSetFeatureLoaded* set_feature_loaded = nullptr;
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

    // Frame generation. The plugins are loaded by default when slInit was asked for them; the functions are resolved
    // while they are loaded and dropped when they are unloaded.
    bool frame_gen_requested = false;
    bool frame_gen_loaded = false;   // sl.dlss_g and sl.reflex are both loaded
    bool frame_gen_partial = false;  // only one of them is, after a failed (un)load
    bool pcl_tried = false;
    PFun_slDLSSGSetOptions* dlssg_set_options = nullptr;
    PFun_slDLSSGGetState* dlssg_get_state = nullptr;
    PFun_slReflexSetOptions* reflex_set_options = nullptr;
    PFun_slPCLSetMarker* pcl_set_marker = nullptr;
    // The Reflex sleep runs on another thread than the rest.
    std::atomic<PFun_slReflexSleep*> reflex_sleep{nullptr};
    sl::FrameToken* frame_token = nullptr;
    std::uint32_t frame_index = 0;

    template <typename Function>
    bool featureFunction(sl::Feature feature, const char* name, Function*& out, std::string& error) {
        if (out != nullptr) return true;
        void* function = nullptr;
        const sl::Result result = api.get_feature_function(feature, name, function);
        if (result != sl::Result::eOk || function == nullptr) {
            error = std::format("{} is unavailable: {}", name, resultName(result));
            return false;
        }
        out = reinterpret_cast<Function*>(function);
        return true;
    }

    // The frame generation calls need the plugins; the check comes before any function lookup.
    bool frameGenReady(std::string& error) const {
        if (!device_set) {
            error = "the Streamline device is not set";
            return false;
        }
        if (!frame_gen_loaded) {
            error = "frame generation is not loaded";
            return false;
        }
        return true;
    }

    // Reads the plugins' load state back from Streamline; a failed query counts as not loaded.
    void refreshFrameGenState() {
        bool dlssg = false, reflex = false;
        if (api.is_feature_loaded(sl::kFeatureDLSS_G, dlssg) != sl::Result::eOk) dlssg = false;
        if (api.is_feature_loaded(sl::kFeatureReflex, reflex) != sl::Result::eOk) reflex = false;
        frame_gen_loaded = dlssg && reflex;
        frame_gen_partial = dlssg != reflex;
    }

    void dropFrameGenFunctions() {
        reflex_sleep.store(nullptr, std::memory_order_release);
        dlssg_set_options = nullptr;
        dlssg_get_state = nullptr;
        reflex_set_options = nullptr;
        pcl_set_marker = nullptr;
        pcl_tried = false;
    }

    bool gather(sl::Feature feature, const char* name, bool check_vulkan, GatheredRequirements& out,
                std::string& reason) {
        sl::FeatureRequirements requirements;
        const sl::Result result = api.get_feature_requirements(feature, requirements);
        if (result != sl::Result::eOk) {
            reason = std::format("{} is unavailable: {}", name, resultName(result));
            return false;
        }
        if (check_vulkan && !(requirements.flags & sl::FeatureRequirementFlags::eVulkanSupported)) {
            reason = std::format("{} does not support Vulkan", name);
            return false;
        }
        out.instance_extensions = copyNames(requirements.vkNumInstanceExtensions, requirements.vkInstanceExtensions);
        out.device_extensions = copyNames(requirements.vkNumDeviceExtensions, requirements.vkDeviceExtensions);
        out.features12 = copyNames(requirements.vkNumFeatures12, requirements.vkFeatures12);
        out.features13 = copyNames(requirements.vkNumFeatures13, requirements.vkFeatures13);
        out.graphics_queues = requirements.vkNumGraphicsQueuesRequired;
        out.compute_queues = requirements.vkNumComputeQueuesRequired;
        out.optical_flow_queues = requirements.vkNumOpticalFlowQueuesRequired;
        out.vsync_off_required = requirements.flags & sl::FeatureRequirementFlags::eVSyncOffRequired;
        out.hws_required = requirements.flags & sl::FeatureRequirementFlags::eHardwareSchedulingRequired;
        out.os_detected = requirements.osVersionDetected.toStr();
        out.os_required = requirements.osVersionRequired.toStr();
        out.driver_detected = requirements.driverVersionDetected.toStr();
        out.driver_required = requirements.driverVersionRequired.toStr();
        return true;
    }

    ~Impl() {
        // Streamline's proxies go away with the interposer, so the volk globals must not outlive it.
        for (const Hook& hook : hooks) *hook.global = hook.saved;
        if (initialized) api.shutdown();
        if (module != nullptr) FreeLibrary(module);
    }
};

Streamline::Streamline() : impl_(std::make_unique<Impl>()) {}
Streamline::~Streamline() = default;

std::unique_ptr<Streamline> Streamline::load(const StreamlineLoadOptions& options, std::string& reason) {
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
    api.is_feature_loaded = resolve<PFun_slIsFeatureLoaded*>(impl.module, "slIsFeatureLoaded");
    api.set_feature_loaded = resolve<PFun_slSetFeatureLoaded*>(impl.module, "slSetFeatureLoaded");
    api.get_instance_proc_addr = resolve<PFN_vkGetInstanceProcAddr>(impl.module, "vkGetInstanceProcAddr");
    api.get_device_proc_addr = resolve<PFN_vkGetDeviceProcAddr>(impl.module, "vkGetDeviceProcAddr");
    if (api.init == nullptr || api.shutdown == nullptr || api.is_feature_supported == nullptr ||
        api.get_feature_requirements == nullptr || api.set_vulkan_info == nullptr ||
        api.get_feature_function == nullptr || api.get_new_frame_token == nullptr || api.set_constants == nullptr ||
        api.set_tag_for_frame == nullptr || api.evaluate_feature == nullptr || api.free_resources == nullptr ||
        api.is_feature_loaded == nullptr || api.set_feature_loaded == nullptr ||
        api.get_instance_proc_addr == nullptr || api.get_device_proc_addr == nullptr) {
        reason = std::string(kInterposerName) + " lacks a required export";
        return nullptr;
    }

    impl.plugin_dir = directory.wstring();
    impl.plugin_paths[0] = impl.plugin_dir.c_str();
    // sl.pcl (the latency markers) is loaded by default and needs no entry.
    std::vector<sl::Feature> features = {sl::kFeatureDLSS};
    if (options.frame_generation) {
        features.push_back(sl::kFeatureDLSS_G);
        features.push_back(sl::kFeatureReflex);
    }

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
    preferences.featuresToLoad = features.data();
    preferences.numFeaturesToLoad = static_cast<std::uint32_t>(features.size());
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
    impl.frame_gen_requested = options.frame_generation;
    impl.frame_gen_loaded = options.frame_generation;
    g_frame_gen_api_errors.store(0, std::memory_order_relaxed);
    impl.version = std::format("{}.{}.{}", SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH);
    return streamline;
}

std::string_view Streamline::version() const { return impl_->version; }

bool Streamline::requirements(StreamlineFeature feature, StreamlineRequirements& out, std::string& reason) {
    Impl& impl = *impl_;
    std::vector<GatheredRequirements> parts;
    if (feature == StreamlineFeature::Dlss) {
        parts.resize(1);
        if (!impl.gather(sl::kFeatureDLSS, "DLSS", true, parts[0], reason)) return false;
    } else {
        if (!impl.frame_gen_loaded) {
            reason = "frame generation is not loaded";
            return false;
        }
        parts.resize(3);
        // DLSS-G and Reflex must report Vulkan support; PCL is only the marker plugin and does not.
        if (!impl.gather(sl::kFeatureDLSS_G, "DLSS-G", true, parts[0], reason) ||
            !impl.gather(sl::kFeatureReflex, "Reflex", true, parts[1], reason) ||
            !impl.gather(sl::kFeaturePCL, "PCL", false, parts[2], reason)) {
            return false;
        }
    }

    // The union of the features: names de-duplicated, queue counts summed, flags ORed. The OS and driver come from
    // the first part (DLSS or DLSS-G), which is the one the user can act on.
    GatheredRequirements merged;
    for (const GatheredRequirements& part : parts) {
        appendUnique(merged.instance_extensions, part.instance_extensions);
        appendUnique(merged.device_extensions, part.device_extensions);
        appendUnique(merged.features12, part.features12);
        appendUnique(merged.features13, part.features13);
        merged.graphics_queues += part.graphics_queues;
        merged.compute_queues += part.compute_queues;
        merged.optical_flow_queues += part.optical_flow_queues;
        merged.vsync_off_required = merged.vsync_off_required || part.vsync_off_required;
        merged.hws_required = merged.hws_required || part.hws_required;
    }
    // Streamline does not always list the extension that Reflex needs on Vulkan; without it on the device frame
    // generation is unavailable, which the caller decides from the device's extension list.
    if (feature == StreamlineFeature::FrameGeneration) {
        appendUnique(merged.device_extensions, {VK_NV_LOW_LATENCY_2_EXTENSION_NAME});
    }

    const auto pointers = [](const std::vector<std::string>& names) {
        std::vector<const char*> out;
        for (const std::string& name : names) out.push_back(name.c_str());
        return out;
    };
    const std::vector<const char*> names12 = pointers(merged.features12);
    const std::vector<const char*> names13 = pointers(merged.features13);
    out.instance_extensions = merged.instance_extensions;
    out.device_extensions = merged.device_extensions;
    out.features12 = sl::getVkPhysicalDeviceVulkan12Features(static_cast<std::uint32_t>(names12.size()),
                                                             const_cast<const char**>(names12.data()));
    out.features13 = sl::getVkPhysicalDeviceVulkan13Features(static_cast<std::uint32_t>(names13.size()),
                                                             const_cast<const char**>(names13.data()));
    out.graphics_queues = merged.graphics_queues;
    out.compute_queues = merged.compute_queues;
    out.optical_flow_queues = merged.optical_flow_queues;
    out.vsync_off_required = merged.vsync_off_required;
    out.hws_required = merged.hws_required;
    out.os_detected = parts[0].os_detected;
    out.os_required = parts[0].os_required;
    out.driver_detected = parts[0].driver_detected;
    out.driver_required = parts[0].driver_required;
    return true;
}

bool Streamline::featureSupported(StreamlineFeature feature, VkPhysicalDevice device, std::string& reason) {
    Impl& impl = *impl_;
    if (feature == StreamlineFeature::FrameGeneration && !impl.frame_gen_loaded) {
        reason = "frame generation is not loaded";
        return false;
    }

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
    if (feature == StreamlineFeature::Dlss) {
        const sl::Result result = impl.api.is_feature_supported(sl::kFeatureDLSS, adapter);
        if (result != sl::Result::eOk) {
            reason = "DLSS is not supported on this GPU: " + resultName(result);
            return false;
        }
        return true;
    }

    struct Needed {
        sl::Feature feature;
        const char* name;
    };
    for (const Needed& needed : {Needed{sl::kFeatureDLSS_G, "DLSS-G"}, Needed{sl::kFeatureReflex, "Reflex"}}) {
        const sl::Result result = impl.api.is_feature_supported(needed.feature, adapter);
        if (result == sl::Result::eErrorOSDisabledHWS) {
            reason = "hardware-accelerated GPU scheduling is off";
            return false;
        }
        if (result != sl::Result::eOk) {
            reason = std::format("{} is not supported on this GPU: {}", needed.name, resultName(result));
            return false;
        }
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
    info.opticalFlowQueueIndex = device.optical_flow_queue;
    info.opticalFlowQueueFamily = device.optical_flow_family;
    info.useNativeOpticalFlowMode = device.native_optical_flow;
    const sl::Result result = impl.api.set_vulkan_info(info);
    if (result != sl::Result::eOk) {
        reason = "slSetVulkanInfo failed: " + resultName(result);
        return false;
    }
    impl.device_set = true;
    g_context_presents.store(device.presents, std::memory_order_relaxed);

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

bool Streamline::frameGenerationLoaded() const { return impl_->frame_gen_loaded; }

bool Streamline::setFrameGenerationLoaded(bool loaded, std::string& reason) {
    Impl& impl = *impl_;
    if (!impl.frame_gen_requested) {
        reason = "frame generation is not loaded: Streamline was initialised without it";
        return false;
    }
    if (!impl.device_set) {
        reason = "the Streamline device is not set";
        return false;
    }
    // A half-loaded state left by an earlier failure is not "already done": unloading is retried.
    if (impl.frame_gen_loaded == loaded && !impl.frame_gen_partial) return true;

    // Dropped before any plugin changes, so no exit path leaves a pointer into an unloaded plugin; the caller has
    // stopped the thread that uses reflex_sleep.
    impl.dropFrameGenFunctions();

    const auto setLoaded = [&](sl::Feature feature, bool value) {
        const sl::Result result = impl.api.set_feature_loaded(feature, value);
        if (result == sl::Result::eOk) return true;
        reason = std::format("slSetFeatureLoaded({}) failed: {}", value ? "true" : "false", resultName(result));
        return false;
    };

    // Reflex goes first when loading and last when unloading, the order of their dependency. Unloading stops at the
    // first failure: Reflex stays while DLSS-G is still loaded.
    bool ok = loaded ? setLoaded(sl::kFeatureReflex, true) && setLoaded(sl::kFeatureDLSS_G, true)
                     : setLoaded(sl::kFeatureDLSS_G, false) && setLoaded(sl::kFeatureReflex, false);
    // Streamline answers eOk without always changing anything; ask for the state it ended up in.
    impl.refreshFrameGenState();
    if (ok && (impl.frame_gen_loaded != loaded || impl.frame_gen_partial)) {
        reason = std::format("sl.dlss_g and sl.reflex did not become {}", loaded ? "loaded" : "unloaded");
        ok = false;
    }
    if (!ok && loaded) {
        // Loading is all or nothing: undo what was loaded (errors ignored; the state is queried again).
        impl.api.set_feature_loaded(sl::kFeatureDLSS_G, false);
        impl.api.set_feature_loaded(sl::kFeatureReflex, false);
        impl.refreshFrameGenState();
    }
    return ok;
}

void Streamline::beginFrame(std::uint32_t index) {
    Impl& impl = *impl_;
    impl.frame_token = nullptr;
    impl.frame_index = 0;
    if (!impl.device_set) return;
    sl::FrameToken* token = nullptr;
    const sl::Result result = impl.api.get_new_frame_token(token, &index);
    if (result != sl::Result::eOk || token == nullptr) {
        logWarn("tox2: streamline: slGetNewFrameToken failed: {}", resultName(result));
        return;
    }
    impl.frame_token = token;
    impl.frame_index = index;
}

std::uint32_t Streamline::frameIndex() const { return impl_->frame_index; }

const void* Streamline::frameHandle() const { return impl_->frame_token; }

void Streamline::latencyMarker(LatencyMarker marker) {
    Impl& impl = *impl_;
    if (!impl.device_set || !impl.frame_gen_loaded || impl.frame_token == nullptr) return;
    if (impl.pcl_set_marker == nullptr) {
        // One attempt per load: a missing sl.pcl would otherwise repeat the lookup and its log line every frame.
        if (impl.pcl_tried) return;
        impl.pcl_tried = true;
        std::string error;
        if (!impl.featureFunction(sl::kFeaturePCL, "slPCLSetMarker", impl.pcl_set_marker, error)) {
            logWarn("tox2: streamline: {}", error);
            return;
        }
    }
    const sl::Result result = impl.pcl_set_marker(toPclMarker(marker), *impl.frame_token);
    if (result != sl::Result::eOk) logDebug("tox2: streamline: slPCLSetMarker failed: {}", resultName(result));
}

bool Streamline::setReflex(const ReflexSettings& settings, std::string& error) {
    Impl& impl = *impl_;
    if (!impl.frameGenReady(error)) return false;
    // The sleep function is resolved here, on the renderer thread, before the sleeper thread can be handed a token.
    PFun_slReflexSleep* sleep = nullptr;
    if (!impl.featureFunction(sl::kFeatureReflex, "slReflexSetOptions", impl.reflex_set_options, error) ||
        !impl.featureFunction(sl::kFeatureReflex, "slReflexSleep", sleep, error)) {
        return false;
    }
    impl.reflex_sleep.store(sleep, std::memory_order_release);

    sl::ReflexOptions options;
    options.mode = settings.low_latency ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff;
    options.frameLimitUs = settings.frame_limit_us;
    const sl::Result result = impl.reflex_set_options(options);
    if (result != sl::Result::eOk) {
        error = "slReflexSetOptions failed: " + resultName(result);
        return false;
    }
    return true;
}

void Streamline::reflexSleep(const void* handle) {
    PFun_slReflexSleep* sleep = impl_->reflex_sleep.load(std::memory_order_acquire);
    if (sleep == nullptr || handle == nullptr) return;
    const sl::Result result = sleep(*static_cast<const sl::FrameToken*>(handle));
    if (result != sl::Result::eOk) logDebug("tox2: streamline: slReflexSleep failed: {}", resultName(result));
}

bool Streamline::setFrameGenOptions(const FrameGenOptions& options, std::string& error) {
    Impl& impl = *impl_;
    if (!impl.frameGenReady(error)) return false;
    if (!impl.featureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", impl.dlssg_set_options, error)) return false;
    const sl::Result result =
            impl.dlssg_set_options(sl::ViewportHandle(kFrameGenViewport), makeDlssgOptions(options, false));
    if (result != sl::Result::eOk) {
        error = "slDLSSGSetOptions failed: " + resultName(result);
        return false;
    }
    return true;
}

bool Streamline::frameGenState(FrameGenState& out, const FrameGenOptions* estimate_for, std::string& error) {
    Impl& impl = *impl_;
    if (!impl.frameGenReady(error)) return false;
    if (!impl.featureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", impl.dlssg_get_state, error)) return false;
    sl::DLSSGState state;
    sl::DLSSGOptions estimate;
    if (estimate_for != nullptr) estimate = makeDlssgOptions(*estimate_for, true);
    const sl::Result result = impl.dlssg_get_state(sl::ViewportHandle(kFrameGenViewport), state,
                                                   estimate_for != nullptr ? &estimate : nullptr);
    if (result != sl::Result::eOk) {
        error = "slDLSSGGetState failed: " + resultName(result);
        return false;
    }
    out.status = static_cast<std::uint32_t>(state.status);
    out.ok = state.status == sl::DLSSGStatus::eOk;
    out.generated_max = state.numFramesToGenerateMax;
    out.presented = state.numFramesActuallyPresented;
    out.min_size = state.minWidthOrHeight;
    out.vram_bytes = state.estimatedVRAMUsageInBytes;
    out.vsync_supported = state.bIsVsyncSupportAvailable == sl::Boolean::eTrue;
    return true;
}

bool Streamline::tagFrameGen(VkCommandBuffer cmd, const FrameGenFrame& frame, std::string& error) {
    Impl& impl = *impl_;
    if (!impl.frameGenReady(error)) return false;
    if (cmd == VK_NULL_HANDLE) {
        error = "no command buffer";
        return false;
    }
    if (impl.frame_token == nullptr) {
        error = "no frame token (beginFrame was not called)";
        return false;
    }
    const sl::ViewportHandle viewport(kFrameGenViewport);

    sl::Constants constants;
    constants.cameraViewToClip = toFloat4x4(frame.view_to_clip);
    constants.clipToCameraView = toFloat4x4(frame.clip_to_view);
    constants.clipToPrevClip = toFloat4x4(frame.clip_to_prev_clip);
    constants.prevClipToClip = toFloat4x4(frame.prev_clip_to_clip);
    constants.jitterOffset = sl::float2(frame.jitter_px[0], frame.jitter_px[1]);
    constants.mvecScale = sl::float2(frame.mvec_scale[0], frame.mvec_scale[1]);
    constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    constants.cameraPos = sl::float3(frame.camera.pos[0], frame.camera.pos[1], frame.camera.pos[2]);
    constants.cameraUp = sl::float3(frame.camera.up[0], frame.camera.up[1], frame.camera.up[2]);
    constants.cameraRight = sl::float3(frame.camera.right[0], frame.camera.right[1], frame.camera.right[2]);
    constants.cameraFwd = sl::float3(frame.camera.fwd[0], frame.camera.fwd[1], frame.camera.fwd[2]);
    constants.cameraNear = frame.camera.near_plane;
    constants.cameraFar = frame.camera.far_plane;
    constants.cameraFOV = frame.camera.fov_y;
    constants.cameraAspectRatio = frame.camera.aspect;
    constants.depthInverted = sl::Boolean::eFalse;
    // Our own vectors include the camera motion, so DLSS-G need not derive it from the matrices; the invalid-value
    // member is not read in that case.
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = frame.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.orthographicProjection = sl::Boolean::eFalse;
    constants.motionVectorsDilated = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    sl::Result result = impl.api.set_constants(constants, *impl.frame_token, viewport);
    if (result != sl::Result::eOk) {
        error = "slSetConstants failed: " + resultName(result);
        return false;
    }

    // eOnlyValidNow: Streamline copies the images while recording into `cmd`, so later passes cannot change what
    // DLSS-G reads at present time.
    sl::Resource depth = toResource(frame.depth);
    sl::Resource motion = toResource(frame.motion);
    const sl::ResourceTag tags[] = {
            sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow),
            sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow),
    };
    result = impl.api.set_tag_for_frame(*impl.frame_token, viewport, tags, static_cast<std::uint32_t>(std::size(tags)),
                                        cmd);
    if (result != sl::Result::eOk) {
        error = "slSetTagForFrame failed: " + resultName(result);
        return false;
    }
    return true;
}

bool Streamline::tagBackbufferRect(VkCommandBuffer cmd, std::int32_t x, std::int32_t y, std::uint32_t width,
                                   std::uint32_t height, std::string& error) {
    Impl& impl = *impl_;
    if (!impl.frameGenReady(error)) return false;
    if (impl.frame_token == nullptr) {
        error = "no frame token (beginFrame was not called)";
        return false;
    }
    // A null resource with an extent marks the part of the backbuffer that holds the scene; the rest is copied as is.
    sl::Extent extent;
    extent.left = static_cast<std::uint32_t>(std::max(x, 0));
    extent.top = static_cast<std::uint32_t>(std::max(y, 0));
    extent.width = width;
    extent.height = height;
    const sl::ResourceTag tag(nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &extent);
    const sl::Result result =
            impl.api.set_tag_for_frame(*impl.frame_token, sl::ViewportHandle(kFrameGenViewport), &tag, 1, cmd);
    if (result != sl::Result::eOk) {
        error = "slSetTagForFrame (backbuffer) failed: " + resultName(result);
        return false;
    }
    return true;
}

void Streamline::freeFrameGen() {
    Impl& impl = *impl_;
    if (!impl.device_set || !impl.frame_gen_loaded) return;
    const sl::Result result = impl.api.free_resources(sl::kFeatureDLSS_G, sl::ViewportHandle(kFrameGenViewport));
    if (result != sl::Result::eOk) logWarn("tox2: streamline: slFreeResources (DLSS-G) failed: {}", resultName(result));
}

std::uint32_t Streamline::frameGenApiErrors() const { return g_frame_gen_api_errors.load(std::memory_order_relaxed); }

}  // namespace tox2
