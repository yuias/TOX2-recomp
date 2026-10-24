// The swapchain behind the OS window and the letterbox geometry. The window itself is per platform.

#include "render/vk_window.h"

#include <algorithm>
#include <format>

#include "core/log.h"

namespace tox2 {
namespace {

constexpr std::uint64_t kAcquireTimeoutNs = 1'000'000'000ull;
constexpr std::chrono::seconds kReportInterval{10};
constexpr int kMaxSwapchainFailures = 3;

double toMs(std::chrono::nanoseconds duration) { return std::chrono::duration<double, std::milli>(duration).count(); }

std::string presentModeName(VkPresentModeKHR mode) {
    switch (mode) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
        case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
        case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo_relaxed";
        default: return std::format("mode {}", static_cast<int>(mode));
    }
}

}  // namespace

VkPresentModeKHR choosePresentMode(VkPresentModeKHR requested, std::span<const VkPresentModeKHR> offered) {
    const auto has = [&offered](VkPresentModeKHR mode) { return std::ranges::find(offered, mode) != offered.end(); };
    if (requested == VK_PRESENT_MODE_FIFO_KHR || has(requested)) return requested;
    for (const VkPresentModeKHR mode : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
        if (!has(mode)) continue;
        logInfo("tox2: render: window: present mode {} is not offered; using {}", presentModeName(requested),
                presentModeName(mode));
        return mode;
    }
    logWarn("tox2: render: frame generation: no present mode without vsync; using fifo (DLSS-G requires vsync off)");
    return VK_PRESENT_MODE_FIFO_KHR;
}

PixelRect letterboxRect(std::uint32_t area_width, std::uint32_t area_height, std::uint32_t aspect_width,
                        std::uint32_t aspect_height) {
    if (area_width == 0 || area_height == 0 || aspect_width == 0 || aspect_height == 0) return {};
    PixelRect rect;
    // Compare the ratios by cross-multiplying: the area is wider than the aspect when
    // area_width / area_height > aspect_width / aspect_height.
    if (static_cast<std::uint64_t>(area_width) * aspect_height <=
        static_cast<std::uint64_t>(area_height) * aspect_width) {
        rect.width = area_width;
        rect.height = static_cast<std::uint32_t>(static_cast<std::uint64_t>(area_width) * aspect_height / aspect_width);
    } else {
        rect.height = area_height;
        rect.width = static_cast<std::uint32_t>(static_cast<std::uint64_t>(area_height) * aspect_width / aspect_height);
    }
    if (rect.width == 0 || rect.height == 0) return {};
    rect.x = static_cast<std::int32_t>((area_width - rect.width) / 2);
    rect.y = static_cast<std::int32_t>((area_height - rect.height) / 2);
    return rect;
}

Presenter::Presenter(VkContext& context) : context_(context) {}

std::unique_ptr<Presenter> Presenter::create(VkContext& context, PresenterOptions options, std::string& error) {
    const std::size_t frame_slots = std::max<std::size_t>(options.frame_slots, 1);
    std::unique_ptr<Presenter> presenter(new Presenter(context));
    presenter->window_ = Window::create(options, error);
    if (!presenter->window_) return nullptr;

    const VkResult result = presenter->window_->createSurface(context, &presenter->surface_, error);
    if (result != VK_SUCCESS) {
        presenter->surface_ = VK_NULL_HANDLE;
        return nullptr;
    }
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(context.physicalDevice(), context.graphicsQueueFamily(), presenter->surface_,
                                         &supported);
    if (supported == VK_FALSE) {
        error = "the graphics queue family cannot present to the window";
        return nullptr;
    }

    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    presenter->acquire_semaphores_.assign(frame_slots, VK_NULL_HANDLE);
    for (VkSemaphore& semaphore : presenter->acquire_semaphores_) {
        if (vkCreateSemaphore(context.device(), &semaphore_info, nullptr, &semaphore) != VK_SUCCESS) {
            semaphore = VK_NULL_HANDLE;
            error = "vkCreateSemaphore failed";
            return nullptr;
        }
    }
    return presenter;
}

Presenter::~Presenter() {
    const VkDevice device = context_.device();
    if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
    destroySwapchainResources();
    for (VkSemaphore semaphore : acquire_semaphores_) {
        if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device, semaphore, nullptr);
    }
    if (surface_ != VK_NULL_HANDLE) vkDestroySurfaceKHR(context_.instance(), surface_, nullptr);
    // The window goes last: the surface refers to it.
}

void* Presenter::nativeWindowHandle() const { return window_ ? window_->nativeHandle() : nullptr; }

WindowStatus Presenter::windowStatus() const { return window_ ? window_->status() : WindowStatus{}; }

void Presenter::requestWindowSize(WindowSizePreset preset) {
    if (window_) window_->requestSize(preset);
}

void Presenter::destroySwapchainResources() {
    const VkDevice device = context_.device();
    for (VkSemaphore semaphore : render_semaphores_) vkDestroySemaphore(device, semaphore, nullptr);
    render_semaphores_.clear();
    for (VkImageView view : image_views_) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
    }
    image_views_.clear();
    images_.clear();
    if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

bool Presenter::ensureSwapchain() {
    if (broken_ || window_->minimized()) return false;
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context_.physicalDevice(), surface_, &caps) != VK_SUCCESS) {
        logOnce(LogLevel::Warn, "window-surface-caps", "tox2: render: window: cannot query the surface capabilities");
        return false;
    }
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        const VkExtent2D client = window_->clientSize();
        extent.width = std::clamp(client.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(client.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) return false;
    if (swapchain_ != VK_NULL_HANDLE && !needs_recreate_ && extent.width == extent_.width &&
        extent.height == extent_.height) {
        return true;
    }
    return createSwapchain(caps, extent);
}

void Presenter::notifyBeforeRecreate() {
    if (swapchain_ != VK_NULL_HANDLE && before_recreate_) before_recreate_();
}

void Presenter::releaseSwapchain() {
    notifyBeforeRecreate();
    const VkDevice device = context_.device();
    if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
    destroySwapchainResources();
    // The next ensureSwapchain would otherwise take the old extent for a swapchain that still matches.
    extent_ = {};
    acquire_stalled_ = false;
}

bool Presenter::createSwapchain(const VkSurfaceCapabilitiesKHR& caps, VkExtent2D extent) {
    const VkDevice device = context_.device();
    if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
        logOnce(LogLevel::Warn, "window-no-transfer-dst",
                "tox2: render: window: swapchain images cannot be a transfer destination");
        broken_ = true;
        return false;
    }
    if (format_ == VK_FORMAT_UNDEFINED) {
        std::uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(context_.physicalDevice(), surface_, &count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(context_.physicalDevice(), surface_, &count, formats.data());
        formats.resize(count);
        // The display buffer holds display-encoded values, so a plain UNORM format shows them unchanged
        // (no sRGB conversion, no HDR); the blit needs BLIT_DST with optimal tiling.
        bool found = false;
        VkSurfaceFormatKHR chosen{};
        for (VkFormat wanted : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
            for (const VkSurfaceFormatKHR& candidate : formats) {
                if (candidate.format != wanted || candidate.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
                VkFormatProperties props{};
                vkGetPhysicalDeviceFormatProperties(context_.physicalDevice(), wanted, &props);
                if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) == 0) continue;
                chosen = candidate;
                found = true;
                break;
            }
            if (found) break;
        }
        if (!found) {
            logOnce(LogLevel::Warn, "window-no-format",
                    "tox2: render: window: no B8G8R8A8/R8G8B8A8 UNORM surface format with blit support");
            broken_ = true;
            return false;
        }
        format_ = chosen.format;
        color_space_ = chosen.colorSpace;
    }

    // Runs before the wait so that a client can stop work that uses the old swapchain first.
    notifyBeforeRecreate();
    // Nothing may still use the old images or semaphores.
    vkDeviceWaitIdle(device);

    std::uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount != 0) image_count = std::min(image_count, caps.maxImageCount);
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((caps.supportedCompositeAlpha & alpha) == 0) {
        for (VkCompositeAlphaFlagBitsKHR bit :
             {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
            if ((caps.supportedCompositeAlpha & bit) != 0) alpha = bit;
        }
    }

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_;
    info.minImageCount = image_count;
    info.imageFormat = format_;
    info.imageColorSpace = color_space_;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    // The Vulkan spec requires colour-attachment support on surfaces; the overlay renders into the image.
    // Without it the views stay null and the overlay is skipped.
    const bool color_attachment = (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (color_attachment ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = alpha;
    std::uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(context_.physicalDevice(), surface_, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> offered(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(context_.physicalDevice(), surface_, &mode_count, offered.data());
    offered.resize(mode_count);
    // FIFO (vsync) is the default request and is always available, so it never falls back.
    const VkPresentModeKHR present_mode = choosePresentMode(requested_present_mode_, offered);
    info.presentMode = present_mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain_;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    const VkResult result = vkCreateSwapchainKHR(device, &info, nullptr, &created);
    // The old swapchain is retired whatever the outcome.
    for (VkSemaphore semaphore : render_semaphores_) vkDestroySemaphore(device, semaphore, nullptr);
    render_semaphores_.clear();
    for (VkImageView view : image_views_) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
    }
    image_views_.clear();
    images_.clear();
    if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    if (result != VK_SUCCESS) {
        logOnce(LogLevel::Warn, "window-swapchain", "tox2: render: window: vkCreateSwapchainKHR failed ({})",
                static_cast<int>(result));
        needs_recreate_ = true;  // try again on a later frame
        if (++swapchain_failures_ >= kMaxSwapchainFailures) {
            logOnce(LogLevel::Warn, "window-swapchain-giving-up",
                    "tox2: render: window: giving up after {} failed swapchain creations", swapchain_failures_);
            broken_ = true;
        }
        return false;
    }
    swapchain_failures_ = 0;
    swapchain_ = created;

    std::uint32_t count = 0;
    vkGetSwapchainImagesKHR(device, swapchain_, &count, nullptr);
    images_.resize(count);
    vkGetSwapchainImagesKHR(device, swapchain_, &count, images_.data());
    images_.resize(count);
    if (color_attachment) {
        image_views_.assign(images_.size(), VK_NULL_HANDLE);
        for (std::size_t i = 0; i < images_.size(); i++) {
            VkImageViewCreateInfo view_info{};
            view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image = images_[i];
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = format_;
            view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(device, &view_info, nullptr, &image_views_[i]) != VK_SUCCESS) {
                image_views_[i] = VK_NULL_HANDLE;  // that image is presented without the overlay
                logOnce(LogLevel::Warn, "window-image-view", "tox2: render: window: vkCreateImageView failed");
            }
        }
    }
    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    render_semaphores_.assign(images_.size(), VK_NULL_HANDLE);
    for (VkSemaphore& semaphore : render_semaphores_) {
        if (vkCreateSemaphore(device, &semaphore_info, nullptr, &semaphore) != VK_SUCCESS) {
            semaphore = VK_NULL_HANDLE;
            logOnce(LogLevel::Warn, "window-semaphore", "tox2: render: window: vkCreateSemaphore failed");
            broken_ = true;
            destroySwapchainResources();
            return false;
        }
    }
    extent_ = extent;
    needs_recreate_ = false;
    present_mode_ = present_mode;
    stats_.recreations++;
    logDebug("tox2: render: window: swapchain {}x{}, {} images, present mode {}, display {} Hz", extent.width,
             extent.height, images_.size(), presentModeName(present_mode), window_->status().refresh_hz);
    return true;
}

bool Presenter::acquire(std::size_t frame_slot, Target& target) {
    if (frame_slot >= acquire_semaphores_.size()) {
        logOnce(LogLevel::Warn, "window-frame-slot",
                "tox2: render: window: frame slot {} but the presenter has {} acquire semaphores", frame_slot,
                acquire_semaphores_.size());
        return false;
    }
    if (report_start_ == std::chrono::steady_clock::time_point{}) report_start_ = std::chrono::steady_clock::now();
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!ensureSwapchain()) {
            stats_.skipped++;
            return false;
        }
        std::uint32_t index = 0;
        const auto start = std::chrono::steady_clock::now();
        // After a timeout the presentation engine is stalled (locked session, occluded window): poll without
        // waiting until an image comes back, so a stall does not cost a full timeout per flip.
        const VkResult result =
                vkAcquireNextImageKHR(context_.device(), swapchain_, acquire_stalled_ ? 0 : kAcquireTimeoutNs,
                                      acquire_semaphores_[frame_slot], VK_NULL_HANDLE, &index);
        const auto waited = std::chrono::steady_clock::now() - start;
        stats_.acquires++;
        stats_.acquire_total += waited;
        stats_.acquire_max = std::max<std::chrono::nanoseconds>(stats_.acquire_max, waited);
        if (result == VK_TIMEOUT) acquire_stalled_ = true;
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            acquire_stalled_ = false;
            if (result == VK_SUBOPTIMAL_KHR) needs_recreate_ = true;  // the image is still usable this time
            target.image = images_[index];
            target.view = index < image_views_.size() ? image_views_[index] : VK_NULL_HANDLE;
            target.format = format_;
            target.acquire_semaphore = acquire_semaphores_[frame_slot];
            target.render_semaphore = render_semaphores_[index];
            target.index = index;
            target.width = extent_.width;
            target.height = extent_.height;
            return true;
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            needs_recreate_ = true;
            continue;
        }
        stats_.skipped++;
        if (result != VK_TIMEOUT && result != VK_NOT_READY) {
            logOnce(LogLevel::Warn, "window-acquire", "tox2: render: window: vkAcquireNextImageKHR failed ({})",
                    static_cast<int>(result));
            needs_recreate_ = true;
        }
        return false;
    }
    stats_.skipped++;
    return false;
}

void Presenter::present(const Target& target) {
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &target.render_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain_;
    info.pImageIndices = &target.index;
    const auto start = std::chrono::steady_clock::now();
    const VkResult result = vkQueuePresentKHR(context_.graphicsQueue(), &info);
    const auto waited = std::chrono::steady_clock::now() - start;
    stats_.presents++;
    stats_.present_total += waited;
    stats_.present_max = std::max<std::chrono::nanoseconds>(stats_.present_max, waited);
    if (result != VK_SUCCESS) {
        // A swapchain that no longer matches the window is rebuilt before the next acquire.
        needs_recreate_ = true;
        if (result != VK_SUBOPTIMAL_KHR && result != VK_ERROR_OUT_OF_DATE_KHR) {
            logOnce(LogLevel::Warn, "window-present", "tox2: render: window: vkQueuePresentKHR failed ({})",
                    static_cast<int>(result));
        }
    }
    reportStats();
}

// Blocking times of the calling thread, to see whether FIFO presentation holds back the RSX thread.
void Presenter::reportStats() {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = now - report_start_;
    if (elapsed < kReportInterval) return;
    const double seconds = std::chrono::duration<double>(elapsed).count();
    const double acquires = static_cast<double>(std::max<std::uint64_t>(stats_.acquires, 1));
    const double presents = static_cast<double>(std::max<std::uint64_t>(stats_.presents, 1));
    logDebug(
            "tox2: render: window: {} presents in {:.1f} s ({:.1f}/s), acquire avg {:.2f} max {:.2f} ms, present avg {:.2f} max {:.2f} ms, "
            "{} skipped, {} swapchain creations",
            stats_.presents, seconds, static_cast<double>(stats_.presents) / seconds,
            toMs(stats_.acquire_total) / acquires, toMs(stats_.acquire_max), toMs(stats_.present_total) / presents,
            toMs(stats_.present_max), stats_.skipped, stats_.recreations);
    const std::uint64_t recreations = stats_.recreations;
    stats_ = Stats{};
    stats_.recreations = recreations;
    report_start_ = now;
}

}  // namespace tox2
