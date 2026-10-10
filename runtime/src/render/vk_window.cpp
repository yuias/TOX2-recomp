// Win32 window on a UI thread and the swapchain behind it.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// WIN32_LEAN_AND_MEAN leaves the IME header out.
#include <imm.h>

#include "render/vk_window.h"

// Not through volk's platform switch: volk is built without VK_USE_PLATFORM_WIN32_KHR, so the surface
// entry point is fetched by name below. The header only adds types here (VK_NO_PROTOTYPES).
#include <vulkan/vulkan_win32.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <future>
#include <thread>

#include "core/log.h"

namespace dal {
namespace {

constexpr wchar_t kWindowClass[] = L"DalGameWindow";
constexpr wchar_t kWindowTitle[] = L"デート・ア・ライブ 凜祢ユートピア";
constexpr std::uint64_t kAcquireTimeoutNs = 1'000'000'000ull;
constexpr std::chrono::seconds kReportInterval{10};
constexpr int kMaxSwapchainFailures = 3;

double toMs(std::chrono::nanoseconds duration) { return std::chrono::duration<double, std::milli>(duration).count(); }

}  // namespace

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

std::optional<KeyMessage> keyMessageFromWin32(std::uint32_t message, std::uint64_t wparam, std::uint64_t lparam) {
    bool down = false;
    switch (message) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: down = true; break;
        case WM_KEYUP:
        case WM_SYSKEYUP: break;
        default: return std::nullopt;
    }
    std::uint32_t scan_code = static_cast<std::uint32_t>((lparam >> 16) & 0xFF);
    if (scan_code == 0) {
        // Not every extended key comes back with 0xE0 (the arrows map to the numpad codes), so the
        // message's own extended bit decides below.
        scan_code = MapVirtualKeyW(static_cast<UINT>(wparam), MAPVK_VK_TO_VSC_EX) & 0xFF;
    }
    if (scan_code != 0 && (lparam & (1u << 24)) != 0) scan_code |= 0xE000u;
    return KeyMessage{scan_code, down, down && (lparam & (1u << 30)) != 0};
}

class Presenter::Window {
public:
    static std::unique_ptr<Window> create(const PresenterOptions& options, std::string& error) {
        std::unique_ptr<Window> window(new Window(options));
        std::future<std::string> ready = window->ready_.get_future();
        window->thread_ = std::thread([w = window.get()] { w->run(); });
        error = ready.get();
        if (!error.empty()) {
            window->thread_.join();
            return nullptr;
        }
        return window;
    }

    ~Window() {
        if (thread_.joinable()) {
            closing_.store(true);
            // DestroyWindow only works on the owning thread.
            if (hwnd_ != nullptr) PostMessageW(hwnd_, kDestroyMessage, 0, 0);
            thread_.join();
        }
    }

    HWND hwnd() const { return hwnd_; }
    bool minimized() const {
        return width_.load(std::memory_order_relaxed) == 0 || height_.load(std::memory_order_relaxed) == 0;
    }
    VkExtent2D clientSize() const {
        return {width_.load(std::memory_order_relaxed), height_.load(std::memory_order_relaxed)};
    }

private:
    static constexpr UINT kDestroyMessage = WM_APP;

    explicit Window(const PresenterOptions& options)
        : on_close_(options.on_close),
          on_key_(options.on_key),
          on_focus_lost_(options.on_focus_lost),
          client_width_(options.client_width),
          client_height_(options.client_height) {}

    void run() {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.lpfnWndProc = &Window::wndProc;
        cls.hInstance = instance;
        cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW, whose macro is the ANSI form here
        cls.lpszClassName = kWindowClass;
        if (RegisterClassExW(&cls) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            ready_.set_value(std::format("RegisterClassExW failed ({})", GetLastError()));
            return;
        }
        const DWORD style = WS_OVERLAPPEDWINDOW;
        // The frame is sized for the DPI of the monitor the window opens on, so the client area is
        // client_width_ x client_height_ physical pixels.
        const auto frameSize = [&](UINT dpi) {
            RECT frame = {0, 0, static_cast<LONG>(client_width_), static_cast<LONG>(client_height_)};
            AdjustWindowRectExForDpi(&frame, style, FALSE, 0, dpi);
            return SIZE{frame.right - frame.left, frame.bottom - frame.top};
        };
        const UINT system_dpi = GetDpiForSystem();
        const SIZE initial = frameSize(system_dpi);
        hwnd_ = CreateWindowExW(0, kWindowClass, kWindowTitle, style, CW_USEDEFAULT, CW_USEDEFAULT, initial.cx,
                                initial.cy, nullptr, nullptr, instance, this);
        if (hwnd_ == nullptr) {
            ready_.set_value(std::format("CreateWindowExW failed ({})", GetLastError()));
            UnregisterClassW(kWindowClass, instance);
            return;
        }
        // No IME for the game window: an open Japanese IME would otherwise show a composition window over
        // the game and turn keys into composition input. Keys still arrive as plain key messages.
        ImmAssociateContext(hwnd_, nullptr);
        if (const UINT window_dpi = GetDpiForWindow(hwnd_); window_dpi != 0 && window_dpi != system_dpi) {
            const SIZE adjusted = frameSize(window_dpi);
            SetWindowPos(hwnd_, nullptr, 0, 0, adjusted.cx, adjusted.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        RECT client{};
        GetClientRect(hwnd_, &client);
        width_.store(static_cast<std::uint32_t>(client.right - client.left));
        height_.store(static_cast<std::uint32_t>(client.bottom - client.top));
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        ready_.set_value({});

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        UnregisterClassW(kWindowClass, instance);
    }

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        Window* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self == nullptr) return DefWindowProcW(hwnd, message, wparam, lparam);
        switch (message) {
            case WM_SIZE:
                // A minimised window reports a 0x0 client area.
                self->width_.store(wparam == SIZE_MINIMIZED ? 0 : LOWORD(lparam));
                self->height_.store(wparam == SIZE_MINIMIZED ? 0 : HIWORD(lparam));
                return 0;
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
                if (self->on_key_) {
                    if (const auto key = keyMessageFromWin32(message, wparam, static_cast<std::uint64_t>(lparam)))
                        self->on_key_(*key);
                }
                // Alt combinations (Alt+F4, Alt+Space) must still reach DefWindowProc.
                if (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) break;
                return 0;
            case WM_KILLFOCUS:
                if (self->on_focus_lost_) self->on_focus_lost_();
                break;
            case WM_CLOSE:
                if (self->on_close_ && !self->closing_.load()) {
                    self->on_close_();  // ends the process
                    return 0;
                }
                DestroyWindow(hwnd);
                return 0;
            case kDestroyMessage: DestroyWindow(hwnd); return 0;
            case WM_DESTROY: PostQuitMessage(0); return 0;
            case WM_ERASEBKGND: return 1;
            case WM_PAINT:
                // The swapchain draws the client area.
                ValidateRect(hwnd, nullptr);
                return 0;
            default: break;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    std::function<void()> on_close_;
    std::function<void(const KeyMessage&)> on_key_;
    std::function<void()> on_focus_lost_;
    std::uint32_t client_width_;
    std::uint32_t client_height_;
    HWND hwnd_ = nullptr;
    std::thread thread_;
    std::promise<std::string> ready_;  // empty string: the window exists
    std::atomic<bool> closing_{false};
    std::atomic<std::uint32_t> width_{0};
    std::atomic<std::uint32_t> height_{0};
};

Presenter::Presenter(VkContext& context) : context_(context) {}

std::unique_ptr<Presenter> Presenter::create(VkContext& context, PresenterOptions options, std::string& error) {
    const std::size_t frame_slots = std::max<std::size_t>(options.frame_slots, 1);
    // Process-wide and before any window exists, so the surface queries on the calling thread report
    // physical pixels like the UI thread's window. Fails harmlessly when the awareness is already set.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::unique_ptr<Presenter> presenter(new Presenter(context));
    presenter->window_ = Window::create(options, error);
    if (!presenter->window_) return nullptr;

    const auto create_surface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
            vkGetInstanceProcAddr(context.instance(), "vkCreateWin32SurfaceKHR"));
    if (create_surface == nullptr) {
        error = "vkCreateWin32SurfaceKHR is not available";
        return nullptr;
    }
    VkWin32SurfaceCreateInfoKHR surface_info{};
    surface_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surface_info.hinstance = GetModuleHandleW(nullptr);
    surface_info.hwnd = presenter->window_->hwnd();
    VkResult result = create_surface(context.instance(), &surface_info, nullptr, &presenter->surface_);
    if (result != VK_SUCCESS) {
        presenter->surface_ = VK_NULL_HANDLE;
        error = std::format("vkCreateWin32SurfaceKHR failed ({})", static_cast<int>(result));
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

void Presenter::destroySwapchainResources() {
    const VkDevice device = context_.device();
    for (VkSemaphore semaphore : render_semaphores_) vkDestroySemaphore(device, semaphore, nullptr);
    render_semaphores_.clear();
    images_.clear();
    if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

bool Presenter::ensureSwapchain() {
    if (broken_ || window_->minimized()) return false;
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context_.physicalDevice(), surface_, &caps) != VK_SUCCESS) {
        logOnce(LogLevel::Warn, "window-surface-caps", "dal: render: window: cannot query the surface capabilities");
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

bool Presenter::createSwapchain(const VkSurfaceCapabilitiesKHR& caps, VkExtent2D extent) {
    const VkDevice device = context_.device();
    if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
        logOnce(LogLevel::Warn, "window-no-transfer-dst",
                "dal: render: window: swapchain images cannot be a transfer destination");
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
                    "dal: render: window: no B8G8R8A8/R8G8B8A8 UNORM surface format with blit support");
            broken_ = true;
            return false;
        }
        format_ = chosen.format;
        color_space_ = chosen.colorSpace;
    }

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
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // vsync; always available
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain_;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    const VkResult result = vkCreateSwapchainKHR(device, &info, nullptr, &created);
    // The old swapchain is retired whatever the outcome.
    for (VkSemaphore semaphore : render_semaphores_) vkDestroySemaphore(device, semaphore, nullptr);
    render_semaphores_.clear();
    images_.clear();
    if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    if (result != VK_SUCCESS) {
        logOnce(LogLevel::Warn, "window-swapchain", "dal: render: window: vkCreateSwapchainKHR failed ({})",
                static_cast<int>(result));
        needs_recreate_ = true;  // try again on a later frame
        if (++swapchain_failures_ >= kMaxSwapchainFailures) {
            logOnce(LogLevel::Warn, "window-swapchain-giving-up",
                    "dal: render: window: giving up after {} failed swapchain creations", swapchain_failures_);
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
    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    render_semaphores_.assign(images_.size(), VK_NULL_HANDLE);
    for (VkSemaphore& semaphore : render_semaphores_) {
        if (vkCreateSemaphore(device, &semaphore_info, nullptr, &semaphore) != VK_SUCCESS) {
            semaphore = VK_NULL_HANDLE;
            logOnce(LogLevel::Warn, "window-semaphore", "dal: render: window: vkCreateSemaphore failed");
            broken_ = true;
            destroySwapchainResources();
            return false;
        }
    }
    extent_ = extent;
    needs_recreate_ = false;
    stats_.recreations++;
    return true;
}

bool Presenter::acquire(std::size_t frame_slot, Target& target) {
    if (frame_slot >= acquire_semaphores_.size()) {
        logOnce(LogLevel::Warn, "window-frame-slot",
                "dal: render: window: frame slot {} but the presenter has {} acquire semaphores", frame_slot,
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
            logOnce(LogLevel::Warn, "window-acquire", "dal: render: window: vkAcquireNextImageKHR failed ({})",
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
            logOnce(LogLevel::Warn, "window-present", "dal: render: window: vkQueuePresentKHR failed ({})",
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
            "dal: render: window: {} presents in {:.1f} s ({:.1f}/s), acquire avg {:.2f} max {:.2f} ms, present avg {:.2f} max {:.2f} ms, "
            "{} skipped, {} swapchain creations",
            stats_.presents, seconds, static_cast<double>(stats_.presents) / seconds,
            toMs(stats_.acquire_total) / acquires, toMs(stats_.acquire_max), toMs(stats_.present_total) / presents,
            toMs(stats_.present_max), stats_.skipped, stats_.recreations);
    const std::uint64_t recreations = stats_.recreations;
    stats_ = Stats{};
    stats_.recreations = recreations;
    report_start_ = now;
}

}  // namespace dal
