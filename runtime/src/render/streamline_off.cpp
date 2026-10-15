// Stand-in for streamline.cpp in builds without the Streamline SDK: DLAA is never available.

#include "render/streamline.h"

namespace tox2 {

struct Streamline::Impl {};

Streamline::Streamline() = default;
Streamline::~Streamline() = default;

std::unique_ptr<Streamline> Streamline::load(std::string& reason) {
    reason = "built without the Streamline SDK";
    return nullptr;
}

std::string_view Streamline::version() const { return {}; }

bool Streamline::requirements(StreamlineRequirements& /*out*/, std::string& reason) {
    reason = "built without the Streamline SDK";
    return false;
}

bool Streamline::dlssSupported(VkPhysicalDevice /*device*/, std::string& reason) {
    reason = "built without the Streamline SDK";
    return false;
}

bool Streamline::setDevice(const StreamlineDevice& /*device*/, std::string& reason) {
    reason = "built without the Streamline SDK";
    return false;
}

PFN_vkVoidFunction Streamline::instanceProxy(const char* /*name*/) const { return nullptr; }

}  // namespace tox2
