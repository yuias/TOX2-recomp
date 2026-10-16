#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <volk.h>

#include "render/dlaa_math.h"
#include "render/frame_gen.h"

namespace tox2 {

// The Streamline features whose requirements and support are queried separately.
enum class StreamlineFeature : std::uint8_t {
    Dlss,             // sl.dlss (DLAA)
    FrameGeneration,  // sl.dlss_g with sl.reflex (sl.pcl is loaded by default)
};

struct StreamlineLoadOptions {
    bool frame_generation = false;  // also load sl.dlss_g and sl.reflex at slInit
};

// What a Streamline feature asks of the Vulkan instance and device. The extension names are copies; the feature
// structs have their sType set and only the requested members enabled.
struct StreamlineRequirements {
    std::vector<std::string> instance_extensions;
    std::vector<std::string> device_extensions;
    VkPhysicalDeviceVulkan12Features features12{};
    VkPhysicalDeviceVulkan13Features features13{};
    std::uint32_t graphics_queues = 0;      // extra queues in the graphics family
    std::uint32_t compute_queues = 0;       // extra queues in a compute family
    std::uint32_t optical_flow_queues = 0;  // FeatureRequirements::vkNumOpticalFlowQueuesRequired (struct version 2)
    bool vsync_off_required = false;        // eVSyncOffRequired
    bool hws_required = false;              // eHardwareSchedulingRequired
    std::string os_detected, os_required, driver_detected, driver_required;  // "a.b.c"

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
    // The context has a window and presents. Streamline reports a "presentCommon() was not observed" error when no
    // present passes its proxy; that is expected only when nothing presents.
    bool presents = false;
    bool native_optical_flow = false;  // DLSS-G uses a dedicated optical flow queue instead of CUDA interop
    std::uint32_t optical_flow_family = 0, optical_flow_queue = 0;
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
    // The renderer's index of the guest frame (>= 1); the same value for every call of one frame, so they all share
    // one Streamline frame token.
    std::uint32_t frame_index = 0;
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

// The frame generation viewport. DLAA viewports are numbered from 1 so the two never share constants.
inline constexpr std::uint32_t kFrameGenViewport = 0;

enum class LatencyMarker : std::uint8_t {
    SimulationStart,
    SimulationEnd,
    RenderSubmitStart,
    RenderSubmitEnd,
    PresentStart,
    PresentEnd
};

struct ReflexSettings {
    bool low_latency = true;           // ReflexMode::eLowLatency, else eOff
    std::uint32_t frame_limit_us = 0;  // always 0 here: a limiter would pace the game
};

struct FrameGenOptions {
    bool on = false;              // DLSSGMode::eOn, else eOff; eRetainResourcesWhenOff is always set
    std::uint32_t generated = 1;  // numFramesToGenerate = multiplier - 1
    bool only_generated = false;  // eShowOnlyInterpolatedFrame (diagnostic)
    std::uint32_t color_width = 0, color_height = 0, back_buffers = 0;
    VkFormat color_format = VK_FORMAT_UNDEFINED;
    std::uint32_t input_width = 0, input_height = 0;  // depth and motion vectors
    VkFormat depth_format = VK_FORMAT_UNDEFINED, motion_format = VK_FORMAT_UNDEFINED;
    bool operator==(const FrameGenOptions&) const = default;
};

struct FrameGenState {
    bool ok = false;                  // status == eOk
    std::uint32_t status = 0;         // DLSSGStatus bits
    std::uint32_t generated_max = 0;  // numFramesToGenerateMax
    std::uint32_t presented = 0;      // numFramesActuallyPresented since the previous query
    std::uint32_t min_size = 0;       // minWidthOrHeight
    std::uint64_t vram_bytes = 0;     // only with an estimate request
    bool vsync_supported = false;
};

// Inputs and constants of one frame for frame generation, recorded at the DLAA tap.
struct FrameGenFrame {
    SlImage depth, motion;               // SHADER_READ_ONLY_OPTIMAL; valid until present, so unchanged until then
    float jitter_px[2] = {0.0f, 0.0f};   // as reported to DLAA for this frame
    float mvec_scale[2] = {1.0f, 1.0f};  // 1 / input size: pixel-space vectors
    Mat4 view_to_clip = mat4Identity(), clip_to_view = mat4Identity();
    Mat4 clip_to_prev_clip = mat4Identity(), prev_clip_to_clip = mat4Identity();  // Streamline's convention (y up)
    FgCamera camera;
    bool reset = false;
};

// NVIDIA Streamline (DLSS) for the Vulkan context and the DLAA pass. The SDK is loaded at run time from
// sl.interposer.dll next to the executable after its signature is checked; a build without the SDK compiles
// streamline_off.cpp instead, whose load() always fails.
class Streamline {
public:
    // Before vkCreateInstance. Null with `reason` (one line, no trailing period) on any failure.
    static std::unique_ptr<Streamline> load(const StreamlineLoadOptions& options, std::string& reason);
    ~Streamline();  // restores the hooked volk pointers, then slShutdown, then FreeLibrary
    Streamline(const Streamline&) = delete;
    Streamline& operator=(const Streamline&) = delete;

    std::string_view version() const;  // SDK headers built against, e.g. "2.14.1"
    // slGetFeatureRequirements. FrameGeneration is the union of DLSS-G, Reflex and PCL plus VK_NV_low_latency2; it
    // fails with "not loaded" when load() was not asked for frame generation and after
    // setFrameGenerationLoaded(false).
    bool requirements(StreamlineFeature feature, StreamlineRequirements& out, std::string& reason);
    // slIsFeatureSupported on `device`; needs an instance, so it runs after the device choice. FrameGeneration asks
    // for both DLSS-G and Reflex.
    bool featureSupported(StreamlineFeature feature, VkPhysicalDevice device, std::string& reason);
    // slSetVulkanInfo, then replaces the volk globals of the mandatory hooks with Streamline's proxies.
    bool setDevice(const StreamlineDevice& device, std::string& reason);
    // Streamline's proxy for an instance-level function it hooks (vkCreateWin32SurfaceKHR); null otherwise.
    PFN_vkVoidFunction instanceProxy(const char* name) const;

    // Frame API, for the thread that records commands. Valid after a successful setDevice().

    // A new DLSS viewport id, counted from 1 (kFrameGenViewport is reserved); each viewport keeps its own temporal
    // history.
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

    // Frame generation. Needs the plugins to be loaded (see frameGenerationLoaded()); the calls below fail or do
    // nothing otherwise.

    bool frameGenerationLoaded() const;
    // slSetFeatureLoaded for sl.dlss_g and sl.reflex. The caller has destroyed the swapchain first (guide 18.0),
    // waited for the device to be idle, stopped or joined the thread that calls reflexSleep (slSetFeatureLoaded is
    // not thread safe), and called freeFrameGen() (it does nothing once unloaded). Fails with "not loaded" when
    // load() was not asked for frame generation. Loading is all or nothing; a failed unload may leave one plugin
    // loaded, in which case frameGenerationLoaded() is false and a later unload is retried. The resolved plugin
    // functions are dropped and looked up again after the next load.
    bool setFrameGenerationLoaded(bool loaded, std::string& reason);

    // Frame identity: the renderer's index (>= 1) for one guest frame. Only the renderer thread calls these.
    void beginFrame(std::uint32_t index);      // slGetNewFrameToken(&index), cached for the frame
    std::uint32_t frameIndex() const;          // 0 before the first beginFrame
    const void* frameHandle() const;           // the cached token, for reflexSleep on another thread
    void latencyMarker(LatencyMarker marker);  // slPCLSetMarker with the current token; no-op unless loaded
    bool setReflex(const ReflexSettings& settings, std::string& error);
    // Thread safe against the renderer thread, but not against setFrameGenerationLoaded(): the caller stops or joins
    // the calling thread first. `handle` from frameHandle(); valid for the next 5 beginFrame calls (ring of 6). Does
    // nothing until setReflex() has resolved the sleep function.
    void reflexSleep(const void* handle);

    bool setFrameGenOptions(const FrameGenOptions& options, std::string& error);  // kFrameGenViewport
    // slDLSSGGetState. With `estimate_for`, the query also asks for a VRAM estimate for those options.
    bool frameGenState(FrameGenState& out, const FrameGenOptions* estimate_for, std::string& error);
    // slSetConstants + slSetTagForFrame(depth, motion) for kFrameGenViewport, current token. All bound command
    // buffer state is invalid afterwards.
    bool tagFrameGen(VkCommandBuffer cmd, const FrameGenFrame& frame, std::string& error);
    // kBufferTypeBackbuffer, null resource, the letterboxed scene rectangle of the swapchain image.
    bool tagBackbufferRect(VkCommandBuffer cmd, std::int32_t x, std::int32_t y, std::uint32_t width,
                           std::uint32_t height, std::string& error);
    void freeFrameGen();                      // slFreeResources(kFeatureDLSS_G, kFrameGenViewport)
    std::uint32_t frameGenApiErrors() const;  // Vulkan errors DLSS-G reported through its error callback

private:
    Streamline();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tox2
