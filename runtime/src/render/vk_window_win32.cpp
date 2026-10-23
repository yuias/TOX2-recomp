// Win32 window on a UI thread. Presenter::Window forwards to Impl, which owns the HWND and the message loop.

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

namespace tox2 {
namespace {

constexpr wchar_t kWindowClass[] = L"Tox2GameWindow";
constexpr wchar_t kWindowTitle[] = L"Tales of Xillia 2";
constexpr DWORD kWindowedStyle = WS_OVERLAPPEDWINDOW;

}  // namespace

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

class Presenter::Window::Impl {
public:
    static std::unique_ptr<Impl> create(const PresenterOptions& options, std::string& error) {
        std::unique_ptr<Impl> window(new Impl(options));
        std::future<std::string> ready = window->ready_.get_future();
        window->thread_ = std::thread([w = window.get()] { w->run(); });
        error = ready.get();
        if (!error.empty()) {
            window->thread_.join();
            return nullptr;
        }
        return window;
    }

    ~Impl() {
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
    WindowStatus status() const {
        WindowStatus status;
        status.preset = static_cast<WindowSizePreset>(preset_.load(std::memory_order_relaxed));
        status.client = {width_.load(std::memory_order_relaxed), height_.load(std::memory_order_relaxed)};
        status.available = {available_width_.load(std::memory_order_relaxed),
                            available_height_.load(std::memory_order_relaxed)};
        status.fullscreen = fullscreen_.load(std::memory_order_relaxed);
        status.refresh_hz = refresh_hz_.load(std::memory_order_relaxed);
        return status;
    }
    // Only PostMessageW: window functions belong to the UI thread.
    void requestSize(WindowSizePreset preset) {
        if (hwnd_ != nullptr) PostMessageW(hwnd_, kSetSizeMessage, static_cast<WPARAM>(preset), 0);
    }

private:
    static constexpr UINT kDestroyMessage = WM_APP;
    static constexpr UINT kSetSizeMessage = WM_APP + 1;

    // What sizing a window needs to know about the monitor it is on.
    struct FrameMetrics {
        ScreenRect work;       // rcWork of the window's monitor
        ClientSize available;  // client pixels that fit `work` with the windowed frame at the window's DPI
    };

    explicit Impl(const PresenterOptions& options)
        : on_close_(options.on_close),
          on_message_(options.on_message),
          on_key_(options.on_key),
          on_focus_lost_(options.on_focus_lost),
          on_fullscreen_changed_(options.on_fullscreen_changed),
          start_fullscreen_(options.fullscreen),
          preset_(static_cast<std::uint8_t>(options.window_size)) {}

    void run() {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.lpfnWndProc = &Impl::wndProc;
        cls.hInstance = instance;
        cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW, whose macro is the ANSI form here
        cls.lpszClassName = kWindowClass;
        if (RegisterClassExW(&cls) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            ready_.set_value(std::format("RegisterClassExW failed ({})", GetLastError()));
            return;
        }
        // The monitor is not known before the window exists, so it is created with the preset's frame at the
        // system DPI at the default position; applyPreset below fits and moves it before it is shown.
        const ClientSize wanted = presetClientSize(static_cast<WindowSizePreset>(preset_.load()));
        RECT frame = {0, 0, static_cast<LONG>(wanted.width), static_cast<LONG>(wanted.height)};
        AdjustWindowRectExForDpi(&frame, kWindowedStyle, FALSE, 0, GetDpiForSystem());
        hwnd_ = CreateWindowExW(0, kWindowClass, kWindowTitle, kWindowedStyle, CW_USEDEFAULT, CW_USEDEFAULT,
                                frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr, instance, this);
        if (hwnd_ == nullptr) {
            ready_.set_value(std::format("CreateWindowExW failed ({})", GetLastError()));
            UnregisterClassW(kWindowClass, instance);
            return;
        }
        // No IME for the game window: an open Japanese IME would otherwise show a composition window over
        // the game and turn keys into composition input. Keys still arrive as plain key messages.
        ImmAssociateContext(hwnd_, nullptr);
        applyPreset();
        // Fullscreen is entered while the window is hidden, after the preset, so the windowed placement it
        // remembers is the preset and no windowed frame is ever shown.
        if (start_fullscreen_ && enterFullscreen()) {
            // A window that was never shown reports SW_HIDE; leaving fullscreen must show the window.
            windowed_placement_.showCmd = SW_SHOWNORMAL;
        }
        refreshAvailable(hwnd_);
        refreshRefreshRate(hwnd_, true);
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
        Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self == nullptr) return DefWindowProcW(hwnd, message, wparam, lparam);
        if (isFullscreenToggle(message, wparam, lparam)) {
            self->toggleFullscreen();
            return 0;
        }
        // The WM_SYSCHAR that TranslateMessage makes from Alt+Enter would make DefWindowProc beep.
        if (message == WM_SYSCHAR && wparam == VK_RETURN) return 0;
        MessageRoute route;
        if (self->on_message_) {
            route = self->on_message_(WindowMessage{hwnd, message, static_cast<std::uint64_t>(wparam),
                                                    static_cast<std::int64_t>(lparam)});
            if (route.handled) return static_cast<LRESULT>(route.result);
        }
        switch (message) {
            case WM_SIZE:
                // A minimised window reports a 0x0 client area.
                self->width_.store(wparam == SIZE_MINIMIZED ? 0 : LOWORD(lparam));
                self->height_.store(wparam == SIZE_MINIMIZED ? 0 : HIWORD(lparam));
                self->refreshAvailable(hwnd);
                // A preset chosen while minimised applies once the window is back. Fullscreen waits for its own exit
                // (the WM_SIZE of leaving it arrives while fullscreen_ is still set).
                if (self->preset_pending_ && wparam != SIZE_MINIMIZED && !self->fullscreen_.load()) self->applyPreset();
                return 0;
            // The work area or the frame width can change with the window's monitor, its DPI, the taskbar or the
            // display layout. The window keeps its rectangle (a size the user dragged stays).
            case WM_MOVE:
            case WM_DPICHANGED:
            case WM_DISPLAYCHANGE:
            case WM_SETTINGCHANGE:
                self->refreshAvailable(hwnd);
                // A mode change keeps the monitor handle, so only these two messages force the query.
                if (message != WM_SETTINGCHANGE) {
                    self->refreshRefreshRate(hwnd, message == WM_DISPLAYCHANGE || message == WM_DPICHANGED);
                }
                break;
            case kSetSizeMessage: self->setPreset(wparam); return 0;
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
                if (self->on_key_ && !route.withhold_key) {
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

    // Alt+Enter or F11, first press only (auto-repeat would toggle back and forth). Bit 29 of lparam is
    // the context code: Alt is down.
    static bool isFullscreenToggle(UINT message, WPARAM wparam, LPARAM lparam) {
        if (message != WM_KEYDOWN && message != WM_SYSKEYDOWN) return false;
        if ((lparam & (1 << 30)) != 0) return false;
        if (wparam == VK_F11) return true;
        return wparam == VK_RETURN && message == WM_SYSKEYDOWN && (lparam & (1 << 29)) != 0;
    }

    // Borderless fullscreen: a popup window covering the monitor it is on. The swapchain follows the
    // client size through WM_SIZE, so nothing on the Vulkan side changes here.
    bool enterFullscreen() {
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        windowed_placement_.length = sizeof(windowed_placement_);
        if (!GetWindowPlacement(hwnd_, &windowed_placement_) ||
            !GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) {
            return false;
        }
        windowed_style_ = GetWindowLongW(hwnd_, GWL_STYLE);
        SetWindowLongW(hwnd_, GWL_STYLE, (windowed_style_ & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        const RECT& r = monitor.rcMonitor;
        SetWindowPos(hwnd_, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        fullscreen_.store(true);
        return true;
    }

    void leaveFullscreen() {
        SetWindowLongW(hwnd_, GWL_STYLE, windowed_style_);
        // The placement restores a maximised window as maximised.
        SetWindowPlacement(hwnd_, &windowed_placement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_.store(false);
        // A preset chosen while fullscreen replaces the remembered placement (and leaves a maximised one).
        if (preset_pending_) applyPreset();
    }

    void toggleFullscreen() {
        if (fullscreen_.load()) {
            leaveFullscreen();
        } else if (!enterFullscreen()) {
            return;
        }
        if (on_fullscreen_changed_) on_fullscreen_changed_(fullscreen_.load());
    }

    // `hwnd` is a parameter because messages arrive inside CreateWindowExW, before hwnd_ is set.
    static bool queryFrameMetrics(HWND hwnd, FrameMetrics& metrics) {
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
        UINT dpi = GetDpiForWindow(hwnd);
        if (dpi == 0) dpi = GetDpiForSystem();
        // An empty client rectangle grows to just the frame (the invisible resize borders count as frame).
        RECT frame{};
        AdjustWindowRectExForDpi(&frame, kWindowedStyle, FALSE, 0, dpi);
        const RECT& work = monitor.rcWork;
        metrics.work = {work.left, work.top, work.right, work.bottom};
        const auto room = [](LONG work_size, LONG frame_size) {
            return static_cast<std::uint32_t>(std::max<LONG>(0, work_size - frame_size));
        };
        metrics.available = {room(work.right - work.left, frame.right - frame.left),
                             room(work.bottom - work.top, frame.bottom - frame.top)};
        return true;
    }

    void refreshAvailable(HWND hwnd) {
        // A minimised window sits at (-32000, -32000), which says nothing about its monitor.
        if (IsIconic(hwnd)) return;
        FrameMetrics metrics;
        if (!queryFrameMetrics(hwnd, metrics)) return;
        available_width_.store(metrics.available.width);
        available_height_.store(metrics.available.height);
    }

    // Refresh rate of the monitor the window is on, from the monitor's current display mode (not the GPU's
    // video-controller properties, which report wrong values on multi-monitor setups). The mode is only queried when
    // the monitor changed or `force` is set, because WM_MOVE arrives continuously while dragging.
    void refreshRefreshRate(HWND hwnd, bool force) {
        if (IsIconic(hwnd)) return;
        const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (!force && monitor == refresh_monitor_) return;
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) return;
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        std::uint32_t hz = 0;
        // 0 and 1 mean "hardware default" rather than a rate.
        if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1) {
            hz = mode.dmDisplayFrequency;
        }
        refresh_monitor_ = monitor;
        refresh_hz_.store(hz);
    }

    void setPreset(WPARAM value) {
        if (value >= kWindowSizePresets.size()) return;
        preset_.store(static_cast<std::uint8_t>(value));
        applyPreset();
    }

    // Windowed only. Sizes the frame for the current preset, fitted to the work area of the window's monitor, and
    // keeps its top-left corner unless the window would stick out of the work area.
    void applyPreset() {
        // Neither state can be sized now; the next WM_SIZE that is not a minimise, or leaving fullscreen, retries.
        if (fullscreen_.load() || IsIconic(hwnd_)) {
            preset_pending_ = true;
            return;
        }
        preset_pending_ = false;
        // Resizing a maximised window only changes the rectangle it returns to, so restore it first.
        if (IsZoomed(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        FrameMetrics metrics;
        if (!queryFrameMetrics(hwnd_, metrics)) return;
        available_width_.store(metrics.available.width);
        available_height_.store(metrics.available.height);

        const ClientSize client =
                fitClientSize(presetClientSize(static_cast<WindowSizePreset>(preset_.load())), metrics.available);
        UINT dpi = GetDpiForWindow(hwnd_);
        if (dpi == 0) dpi = GetDpiForSystem();
        RECT frame = {0, 0, static_cast<LONG>(client.width), static_cast<LONG>(client.height)};
        AdjustWindowRectExForDpi(&frame, kWindowedStyle, FALSE, 0, dpi);
        RECT current{};
        if (!GetWindowRect(hwnd_, &current)) return;
        const LONG width = frame.right - frame.left;
        const LONG height = frame.bottom - frame.top;
        const ScreenRect placed =
                placeInWorkArea({current.left, current.top, current.left + width, current.top + height}, metrics.work);
        SetWindowPos(hwnd_, nullptr, placed.left, placed.top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    std::function<void()> on_close_;
    std::function<MessageRoute(const WindowMessage&)> on_message_;
    std::function<void(const KeyMessage&)> on_key_;
    std::function<void()> on_focus_lost_;
    std::function<void(bool)> on_fullscreen_changed_;
    bool start_fullscreen_;
    HWND hwnd_ = nullptr;
    std::thread thread_;
    std::promise<std::string> ready_;  // empty string: the window exists
    std::atomic<bool> closing_{false};
    std::atomic<std::uint32_t> width_{0};
    std::atomic<std::uint32_t> height_{0};
    // Written on the UI thread, read by Presenter::windowStatus.
    std::atomic<std::uint8_t> preset_;
    std::atomic<std::uint32_t> available_width_{0};
    std::atomic<std::uint32_t> available_height_{0};
    std::atomic<bool> fullscreen_{false};
    std::atomic<std::uint32_t> refresh_hz_{0};
    // UI thread only.
    HMONITOR refresh_monitor_ = nullptr;
    bool preset_pending_ = false;  // a preset was chosen while fullscreen
    LONG windowed_style_ = 0;
    WINDOWPLACEMENT windowed_placement_{};
};

Presenter::Window::Window(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Presenter::Window::~Window() = default;

std::unique_ptr<Presenter::Window> Presenter::Window::create(const PresenterOptions& options, std::string& error) {
    // Process-wide and before any window exists, so the surface queries on the calling thread report
    // physical pixels like the UI thread's window. Fails harmlessly when the awareness is already set.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::unique_ptr<Impl> impl = Impl::create(options, error);
    if (!impl) return nullptr;
    return std::unique_ptr<Window>(new Window(std::move(impl)));
}

void* Presenter::Window::nativeHandle() const { return impl_->hwnd(); }

bool Presenter::Window::minimized() const { return impl_->minimized(); }

VkExtent2D Presenter::Window::clientSize() const { return impl_->clientSize(); }

WindowStatus Presenter::Window::status() const { return impl_->status(); }

void Presenter::Window::requestSize(WindowSizePreset preset) { impl_->requestSize(preset); }

VkResult Presenter::Window::createSurface(const VkContext& context, VkSurfaceKHR* surface, std::string& error) const {
    const auto create_surface =
            reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(context.instanceProc("vkCreateWin32SurfaceKHR"));
    if (create_surface == nullptr) {
        error = "vkCreateWin32SurfaceKHR is not available";
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    VkWin32SurfaceCreateInfoKHR surface_info{};
    surface_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surface_info.hinstance = GetModuleHandleW(nullptr);
    surface_info.hwnd = impl_->hwnd();
    const VkResult result = create_surface(context.instance(), &surface_info, nullptr, surface);
    if (result != VK_SUCCESS) error = std::format("vkCreateWin32SurfaceKHR failed ({})", static_cast<int>(result));
    return result;
}

// The window has its own thread, so the calling thread needs no event loop.
void runWithHostEventLoop(std::function<void()> body, bool /*user_interface*/) { body(); }

void pumpHostEvents() {}

}  // namespace tox2
