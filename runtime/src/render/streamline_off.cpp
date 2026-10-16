// Stand-in for streamline.cpp in builds without the Streamline SDK: DLAA and frame generation are never available.

#include "render/streamline.h"

namespace tox2 {

namespace {
constexpr const char* kNoSdk = "built without the Streamline SDK";
}  // namespace

struct Streamline::Impl {};

Streamline::Streamline() = default;
Streamline::~Streamline() = default;

std::unique_ptr<Streamline> Streamline::load(const StreamlineLoadOptions& /*options*/, std::string& reason) {
    reason = kNoSdk;
    return nullptr;
}

std::string_view Streamline::version() const { return {}; }

bool Streamline::requirements(StreamlineFeature /*feature*/, StreamlineRequirements& /*out*/, std::string& reason) {
    reason = kNoSdk;
    return false;
}

bool Streamline::featureSupported(StreamlineFeature /*feature*/, VkPhysicalDevice /*device*/, std::string& reason) {
    reason = kNoSdk;
    return false;
}

bool Streamline::setDevice(const StreamlineDevice& /*device*/, std::string& reason) {
    reason = kNoSdk;
    return false;
}

PFN_vkVoidFunction Streamline::instanceProxy(const char* /*name*/) const { return nullptr; }

std::uint32_t Streamline::newViewport() { return 0; }

bool Streamline::evaluateDlaa(VkCommandBuffer /*cmd*/, const DlaaFrame& /*frame*/, std::string& error) {
    error = kNoSdk;
    return false;
}

void Streamline::freeViewport(std::uint32_t /*viewport*/) {}

bool Streamline::frameGenerationLoaded() const { return false; }

bool Streamline::setFrameGenerationLoaded(bool /*loaded*/, std::string& reason) {
    reason = kNoSdk;
    return false;
}

void Streamline::beginFrame(std::uint32_t /*index*/) {}

std::uint32_t Streamline::frameIndex() const { return 0; }

const void* Streamline::frameHandle() const { return nullptr; }

void Streamline::latencyMarker(LatencyMarker /*marker*/) {}

bool Streamline::setReflex(const ReflexSettings& /*settings*/, std::string& error) {
    error = kNoSdk;
    return false;
}

void Streamline::reflexSleep(const void* /*handle*/) {}

bool Streamline::setFrameGenOptions(const FrameGenOptions& /*options*/, std::string& error) {
    error = kNoSdk;
    return false;
}

bool Streamline::frameGenState(FrameGenState& /*out*/, const FrameGenOptions* /*estimate_for*/, std::string& error) {
    error = kNoSdk;
    return false;
}

bool Streamline::tagFrameGen(VkCommandBuffer /*cmd*/, const FrameGenFrame& /*frame*/, std::string& error) {
    error = kNoSdk;
    return false;
}

bool Streamline::tagBackbufferRect(VkCommandBuffer /*cmd*/, std::int32_t /*x*/, std::int32_t /*y*/,
                                   std::uint32_t /*width*/, std::uint32_t /*height*/, std::string& error) {
    error = kNoSdk;
    return false;
}

void Streamline::freeFrameGen() {}

std::uint32_t Streamline::frameGenApiErrors() const { return 0; }

}  // namespace tox2
