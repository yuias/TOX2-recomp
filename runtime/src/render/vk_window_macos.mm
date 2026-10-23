// Cocoa window with a CAMetalLayer for MoltenVK, and the host event loop. AppKit objects may only be
// touched on the process main thread, so every Cocoa call below runs there; the size and the minimized
// state are published in atomics for the thread that owns the renderer.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include "render/vk_window.h"

// Not through volk's platform switch: volk is built without VK_USE_PLATFORM_METAL_EXT, so the surface
// entry point is fetched by name below. The header only adds types here (VK_NO_PROTOTYPES).
#include <vulkan/vulkan_metal.h>

#include <dispatch/dispatch.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>

#include "core/log.h"
#include "core/threads.h"
#include "input/mac_key_codes.h"

namespace {

constexpr char kWindowTitle[] = "Tales of Xillia 2";

// What the Objective-C classes below call into; implemented by Presenter::Window::Impl. All calls happen
// on the main thread.
struct WindowEvents {
    virtual ~WindowEvents() = default;
    // Returns true when the event must not reach AppKit's own dispatch.
    virtual bool onEvent(NSEvent* event) = 0;
    // The user asked to close the window. Returns true when the client took over (the window stays open).
    virtual bool requestClose() = 0;
    virtual void onFocus(bool gained) = 0;
    // The frame, the backing scale, the screen or the minimized state changed.
    virtual void onLayoutChanged() = 0;
    // The window entered or left fullscreen.
    virtual void onFullscreenChanged(bool fullscreen) = 0;
};

}  // namespace

// AppKit does not deliver the key-up of a key that went down with Command held to the key window (it
// ends the key equivalent search), so a key pressed together with Command would stay down for the pad.
// Handing those key-ups to the window makes up for it.
@interface Tox2Application : NSApplication
@end

@implementation Tox2Application
- (void)sendEvent:(NSEvent*)event {
    if (event.type == NSEventTypeKeyUp && (event.modifierFlags & NSEventModifierFlagCommand) != 0) {
        [self.keyWindow sendEvent:event];
        return;
    }
    [super sendEvent:event];
}
@end

@interface Tox2Window : NSWindow
@property(nonatomic, assign) WindowEvents* events;
@end

@implementation Tox2Window
// The single routing point for window input: every event sent to this window reaches the client first.
// Key equivalents (Cmd+Q and other menu shortcuts) are handled by NSApplication before they get here, so
// the client never sees those key-downs.
- (void)sendEvent:(NSEvent*)event {
    if (self.events != nullptr && self.events->onEvent(event)) return;
    [super sendEvent:event];
}
@end

@interface Tox2MetalView : NSView
@property(nonatomic, assign) WindowEvents* events;
@end

@implementation Tox2MetalView
- (CALayer*)makeBackingLayer {
    return [CAMetalLayer layer];
}
- (BOOL)wantsUpdateLayer {
    return YES;
}
- (BOOL)isOpaque {
    return YES;
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    return YES;
}
// Keys are not text input here: the view is no NSTextInputClient and never calls interpretKeyEvents:, so
// no input method opens. The empty overrides keep the keys that still reach the responder chain (unmapped
// keys, Command combinations) from beeping.
- (void)keyDown:(NSEvent*)event {
}
- (void)keyUp:(NSEvent*)event {
}
- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    if (self.events != nullptr) self.events->onLayoutChanged();
}
- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.events != nullptr) self.events->onLayoutChanged();
}
- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    if (self.events != nullptr) self.events->onLayoutChanged();
}
@end

@interface Tox2WindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) WindowEvents* events;
@end

@implementation Tox2WindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    return self.events == nullptr || !self.events->requestClose();
}
- (void)windowDidBecomeKey:(NSNotification*)notification {
    if (self.events != nullptr) self.events->onFocus(true);
}
- (void)windowDidResignKey:(NSNotification*)notification {
    if (self.events != nullptr) self.events->onFocus(false);
}
- (void)windowDidMiniaturize:(NSNotification*)notification {
    if (self.events != nullptr) self.events->onLayoutChanged();
}
- (void)windowDidDeminiaturize:(NSNotification*)notification {
    if (self.events != nullptr) self.events->onLayoutChanged();
}
- (void)windowDidChangeBackingProperties:(NSNotification*)notification {
    if (self.events != nullptr) self.events->onLayoutChanged();
}
- (void)windowDidChangeScreen:(NSNotification*)notification {
    if (self.events != nullptr) self.events->onLayoutChanged();
}
- (void)windowDidEnterFullScreen:(NSNotification*)notification {
    if (self.events != nullptr) self.events->onFullscreenChanged(true);
}
- (void)windowDidExitFullScreen:(NSNotification*)notification {
    if (self.events != nullptr) self.events->onFullscreenChanged(false);
}
@end

namespace {

// The window that is open, for Cmd+Q. Main thread only.
WindowEvents* g_open_window = nullptr;

}  // namespace

@interface Tox2AppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation Tox2AppDelegate
// Cmd+Q and Quit end the run through the client's close handling, like the close button, and never
// terminate the process from here: AppKit would call exit() while the emulation thread still runs. Before a
// window is open (or while it closes) Quit does nothing.
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    if (g_open_window != nullptr) g_open_window->requestClose();
    return NSTerminateCancel;
}
@end

namespace tox2 {
namespace {

// Set once the main thread runs (or is about to run) [NSApp run], which finishes the launch itself.
bool g_loop_started = false;
bool g_launched = false;
Tox2AppDelegate* g_app_delegate = nil;

void runOnMain(const std::function<void()>& fn) {
    if (pthread_main_np() != 0) {
        fn();
        return;
    }
    const std::function<void()>* target = &fn;
    dispatch_sync(dispatch_get_main_queue(), ^{
      (*target)();
    });
}

// Creates NSApp with a minimal menu. Main thread only; the first successful call does the work.
bool ensureApplication(std::string& error) {
    if (g_app_delegate == nil) {
        // Without a window server connection NSApplication aborts the process instead of failing.
        CFDictionaryRef session = CGSessionCopyCurrentDictionary();
        if (session == nullptr) {
            error = "no GUI session (the window server is not reachable)";
            return false;
        }
        CFRelease(session);

        [Tox2Application sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        g_app_delegate = [[Tox2AppDelegate alloc] init];
        [NSApp setDelegate:g_app_delegate];

        NSMenu* menu_bar = [[NSMenu alloc] init];
        NSMenuItem* app_item = [[NSMenuItem alloc] init];
        [menu_bar addItem:app_item];
        NSMenu* app_menu = [[NSMenu alloc] init];
        NSString* quit_title = [@"Quit " stringByAppendingString:[[NSProcessInfo processInfo] processName]];
        [app_menu addItem:[[NSMenuItem alloc] initWithTitle:quit_title
                                                     action:@selector(terminate:)
                                              keyEquivalent:@"q"]];
        [app_item setSubmenu:app_menu];
        // Ctrl+Cmd+F, the system's shortcut for fullscreen; it reaches the key window through the responder chain.
        NSMenuItem* view_item = [[NSMenuItem alloc] init];
        [menu_bar addItem:view_item];
        NSMenu* view_menu = [[NSMenu alloc] initWithTitle:@"View"];
        NSMenuItem* fullscreen_item = [[NSMenuItem alloc] initWithTitle:@"Toggle Full Screen"
                                                                 action:@selector(toggleFullScreen:)
                                                          keyEquivalent:@"f"];
        [fullscreen_item setKeyEquivalentModifierMask:NSEventModifierFlagControl | NSEventModifierFlagCommand];
        [view_menu addItem:fullscreen_item];
        [view_item setSubmenu:view_menu];
        [NSApp setMainMenu:menu_bar];
    }
    if (!g_launched && !g_loop_started) {
        [NSApp finishLaunching];
        g_launched = true;
    }
    return true;
}

}  // namespace

class Presenter::Window::Impl final : public WindowEvents {
public:
    // Opens the window on the main thread; from another thread the main thread must be running its event
    // loop (runWithHostEventLoop).
    static std::unique_ptr<Impl> create(const PresenterOptions& options, std::string& error) {
        std::unique_ptr<Impl> window(new Impl(options));
        bool opened = false;
        runOnMain([&] { opened = window->open(error); });
        if (!opened) return nullptr;
        return window;
    }

    ~Impl() override {
        // Callbacks that arrive from now on find the window closing and do not reach the client.
        closing_.store(true);
        runOnMain([this] { close(); });
    }

    void* view() const { return (__bridge void*)view_; }
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
    // Asynchronous: the caller may be the renderer thread, which must not wait for the main thread. The block
    // reaches the window through `alive_`, which close() clears, so a request still queued at close does nothing.
    void requestSize(WindowSizePreset preset) {
        preset_.store(static_cast<std::uint8_t>(preset));
        const std::shared_ptr<Impl*> alive = alive_;
        dispatch_async(dispatch_get_main_queue(), ^{
          if (*alive != nullptr) (*alive)->applyPreset();
        });
    }

    VkResult createSurface(const VkContext& context, VkSurfaceKHR* surface, std::string& error) const {
        const VkInstance instance = context.instance();
        const auto create_surface =
                reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(context.instanceProc("vkCreateMetalSurfaceEXT"));
        if (create_surface == nullptr) {
            error = "vkCreateMetalSurfaceEXT is not available";
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        VkMetalSurfaceCreateInfoEXT surface_info{};
        surface_info.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
        surface_info.pLayer = layer_;
        const VkResult result = create_surface(instance, &surface_info, nullptr, surface);
        if (result != VK_SUCCESS) error = std::format("vkCreateMetalSurfaceEXT failed ({})", static_cast<int>(result));
        return result;
    }

private:
    explicit Impl(const PresenterOptions& options)
        : on_close_(options.on_close),
          on_message_(options.on_message),
          on_key_(options.on_key),
          on_focus_lost_(options.on_focus_lost),
          on_fullscreen_changed_(options.on_fullscreen_changed),
          start_fullscreen_(options.fullscreen),
          preset_(static_cast<std::uint8_t>(options.window_size)) {}

    // Main thread.
    bool open(std::string& error) {
        @autoreleasepool {
            if (!ensureApplication(error)) return false;
            // The preset is in pixels; applyPreset below converts it with the screen's backing scale once the
            // window knows its screen.
            const ClientSize wanted = presetClientSize(static_cast<WindowSizePreset>(preset_.load()));
            const NSRect content = NSMakeRect(0, 0, wanted.width, wanted.height);
            const NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
            window_ = [[Tox2Window alloc] initWithContentRect:content
                                                    styleMask:style
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
            if (window_ == nil) {
                error = "NSWindow creation failed";
                return false;
            }
            // ARC owns the window; closing it must not release it a second time.
            [window_ setReleasedWhenClosed:NO];
            [window_ setTitle:[NSString stringWithUTF8String:kWindowTitle]];
            [window_ setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];

            view_ = [[Tox2MetalView alloc] initWithFrame:content];
            [view_ setWantsLayer:YES];
            layer_ = [view_.layer isKindOfClass:[CAMetalLayer class]] ? (CAMetalLayer*)view_.layer : nil;
            if (layer_ == nil) {
                error = "the content view has no CAMetalLayer";
                window_ = nil;
                view_ = nil;
                return false;
            }
            [layer_ setOpaque:YES];

            delegate_ = [[Tox2WindowDelegate alloc] init];
            window_.events = this;
            view_.events = this;
            delegate_.events = this;
            [window_ setDelegate:delegate_];
            [window_ setContentView:view_];
            [window_ makeFirstResponder:view_];
            applyPreset();
            [window_ center];
            updateSurface();
            g_open_window = this;
            [window_ makeKeyAndOrderFront:nil];
            [NSApp activate];
            // The system's fullscreen animates into its own space once the window is on screen.
            if (start_fullscreen_) {
                starting_fullscreen_ = true;
                [window_ toggleFullScreen:nil];
            }
        }
        return true;
    }

    // Main thread.
    void close() {
        @autoreleasepool {
            *alive_ = nullptr;
            if (g_open_window == this) g_open_window = nullptr;
            if (window_ == nil) return;
            window_.events = nullptr;
            view_.events = nullptr;
            delegate_.events = nullptr;
            [window_ setDelegate:nil];
            [window_ close];
            // Released on the next turn of the main queue: close() may run from one of this window's own
            // callbacks, and ARC does not keep the window alive while it is still inside sendEvent:.
            Tox2Window* closed = window_;
            dispatch_async(dispatch_get_main_queue(), ^{
              (void)closed;
            });
            window_ = nil;
            view_ = nil;
            layer_ = nil;
            delegate_ = nil;
        }
    }

    // Main thread. Sizes the content for the current preset, fitted to the visible frame of the window's screen
    // (minus the title bar). A miniaturized or fullscreen window applies it when it comes back.
    void applyPreset() {
        if (window_ == nil) return;
        if (fullscreen_.load() || window_.miniaturized) {
            preset_pending_ = true;
            return;
        }
        preset_pending_ = false;
        NSScreen* screen = window_.screen != nil ? window_.screen : [NSScreen mainScreen];
        if (screen == nil) return;
        const CGFloat scale = screen.backingScaleFactor;
        const NSRect visible = screen.visibleFrame;
        const ClientSize available = availablePixels(screen);
        available_width_.store(available.width);
        available_height_.store(available.height);
        const ClientSize client =
                fitClientSize(presetClientSize(static_cast<WindowSizePreset>(preset_.load())), available);
        [window_ setContentSize:NSMakeSize(client.width / scale, client.height / scale)];
        // Keeps the top-left corner unless the window would leave the visible frame.
        NSRect frame = window_.frame;
        frame.origin.x = std::clamp(frame.origin.x, visible.origin.x,
                                    std::max(visible.origin.x, NSMaxX(visible) - frame.size.width));
        frame.origin.y = std::clamp(frame.origin.y, visible.origin.y,
                                    std::max(visible.origin.y, NSMaxY(visible) - frame.size.height));
        [window_ setFrameOrigin:frame.origin];
    }

    // Main thread. Client pixels that fit the visible frame of `screen` (without the menu bar and the Dock) with
    // the window's title bar.
    ClientSize availablePixels(NSScreen* screen) const {
        const CGFloat scale = screen.backingScaleFactor;
        const NSRect visible = screen.visibleFrame;
        const NSRect frame = [window_ frameRectForContentRect:NSMakeRect(0, 0, 0, 0)];
        const auto room = [scale](CGFloat visible_size, CGFloat frame_size) {
            return static_cast<std::uint32_t>(std::max<CGFloat>(0, (visible_size - frame_size) * scale));
        };
        return {room(visible.size.width, frame.size.width), room(visible.size.height, frame.size.height)};
    }

    // Main thread. The available client pixels and the refresh rate of the window's screen.
    void refreshScreen() {
        NSScreen* screen = window_.screen;
        if (screen == nil) return;
        // Fullscreen has no title bar and no Dock, so the windowed room is kept from before.
        if (!fullscreen_.load()) {
            const ClientSize available = availablePixels(screen);
            available_width_.store(available.width);
            available_height_.store(available.height);
        }
        // 0 for a screen that does not report a rate (ProMotion screens report their maximum).
        refresh_hz_.store(static_cast<std::uint32_t>(std::max<NSInteger>(0, screen.maximumFramesPerSecond)));
    }

    // Publishes the pixel size and keeps the layer's drawable the same size.
    void updateSurface() {
        if (window_ == nil || layer_ == nil) return;
        refreshScreen();
        const CGFloat scale = window_.backingScaleFactor;
        const NSSize points = view_.bounds.size;
        const double pixels_w = std::round(points.width * scale);
        const double pixels_h = std::round(points.height * scale);
        const bool empty = pixels_w < 1 || pixels_h < 1;
        if (!empty) {
            layer_.contentsScale = scale;
            layer_.drawableSize = CGSizeMake(pixels_w, pixels_h);
        }
        const bool hidden = empty || window_.miniaturized;
        width_.store(hidden ? 0 : static_cast<std::uint32_t>(pixels_w));
        height_.store(hidden ? 0 : static_cast<std::uint32_t>(pixels_h));
    }

    // The key message of a key event, or nullopt for any other event and for keys without a scan code.
    static std::optional<KeyMessage> keyMessageFromEvent(NSEvent* event) {
        const NSEventType type = event.type;
        if (type != NSEventTypeKeyDown && type != NSEventTypeKeyUp && type != NSEventTypeFlagsChanged) {
            return std::nullopt;
        }
        const std::optional<std::uint32_t> scan_code = scanCodeFromMacKeyCode(event.keyCode);
        if (!scan_code) return std::nullopt;
        if (type == NSEventTypeFlagsChanged) {
            // A modifier has no key-down or key-up event, only the new flag state. Its own device-dependent
            // bit tells which key changed, even while the other key of the pair is held.
            const std::optional<std::uint64_t> mask = macModifierDownMask(event.keyCode);
            if (!mask) return std::nullopt;
            return KeyMessage{*scan_code, (event.modifierFlags & *mask) != 0, false};
        }
        // isARepeat raises an exception for events that are not key-down or key-up.
        const bool down = type == NSEventTypeKeyDown;
        return KeyMessage{*scan_code, down, down && event.isARepeat};
    }

    bool onEvent(NSEvent* event) override {
        if (closing_.load()) return false;
        const std::optional<KeyMessage> key = keyMessageFromEvent(event);
        bool withhold_key = false;
        if (on_message_) {
            WindowMessage message;
            message.view = view();
            message.event = (__bridge void*)event;
            if (key) {
                message.kind = WindowEventKind::Key;
                message.key = key;
            }
            const MessageRoute route = on_message_(message);
            if (route.handled) return true;
            withhold_key = route.withhold_key;
        }
        if (!key) return false;
        if (on_key_ && !withhold_key) on_key_(*key);
        // The client has the key. Command combinations still go on to AppKit for the system shortcuts that
        // are not menu items; other keys end here, so nothing beeps.
        return (event.modifierFlags & NSEventModifierFlagCommand) == 0 || event.type == NSEventTypeFlagsChanged;
    }

    bool requestClose() override {
        if (!on_close_ || closing_.load()) return false;
        on_close_();  // ends the process
        return true;
    }

    void onFocus(bool gained) override {
        if (closing_.load()) return;
        if (on_message_) {
            WindowMessage message;
            message.view = view();
            message.kind = gained ? WindowEventKind::FocusGained : WindowEventKind::FocusLost;
            on_message_(message);
        }
        if (!gained && on_focus_lost_) on_focus_lost_();
    }

    void onLayoutChanged() override {
        updateSurface();
        if (preset_pending_ && !window_.miniaturized && !fullscreen_.load()) applyPreset();
    }

    void onFullscreenChanged(bool fullscreen) override {
        fullscreen_.store(fullscreen);
        updateSurface();
        // A preset chosen while fullscreen applies to the window it returns to.
        if (!fullscreen && preset_pending_) applyPreset();
        // The start-up transition is not a user toggle.
        if (starting_fullscreen_) {
            starting_fullscreen_ = false;
            return;
        }
        if (on_fullscreen_changed_ && !closing_.load()) on_fullscreen_changed_(fullscreen);
    }

    std::function<void()> on_close_;
    std::function<MessageRoute(const WindowMessage&)> on_message_;
    std::function<void(const KeyMessage&)> on_key_;
    std::function<void()> on_focus_lost_;
    std::function<void(bool)> on_fullscreen_changed_;
    bool start_fullscreen_;
    Tox2Window* window_ = nil;
    Tox2MetalView* view_ = nil;
    CAMetalLayer* layer_ = nil;
    Tox2WindowDelegate* delegate_ = nil;
    std::atomic<bool> closing_{false};
    std::atomic<std::uint32_t> width_{0};
    std::atomic<std::uint32_t> height_{0};
    // Written on the main thread, read by Presenter::windowStatus.
    std::atomic<std::uint8_t> preset_;
    std::atomic<std::uint32_t> available_width_{0};
    std::atomic<std::uint32_t> available_height_{0};
    std::atomic<bool> fullscreen_{false};
    std::atomic<std::uint32_t> refresh_hz_{0};
    const std::shared_ptr<Impl*> alive_ = std::make_shared<Impl*>(this);
    // Main thread only.
    bool preset_pending_ = false;       // a preset was chosen while fullscreen or miniaturized
    bool starting_fullscreen_ = false;  // the start-up transition into fullscreen is running
};

Presenter::Window::Window(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Presenter::Window::~Window() = default;

std::unique_ptr<Presenter::Window> Presenter::Window::create(const PresenterOptions& options, std::string& error) {
    std::unique_ptr<Impl> impl = Impl::create(options, error);
    if (!impl) return nullptr;
    return std::unique_ptr<Window>(new Window(std::move(impl)));
}

void* Presenter::Window::nativeHandle() const { return impl_->view(); }

bool Presenter::Window::minimized() const { return impl_->minimized(); }

VkExtent2D Presenter::Window::clientSize() const { return impl_->clientSize(); }

WindowStatus Presenter::Window::status() const { return impl_->status(); }

void Presenter::Window::requestSize(WindowSizePreset preset) { impl_->requestSize(preset); }

VkResult Presenter::Window::createSurface(const VkContext& context, VkSurfaceKHR* surface, std::string& error) const {
    return impl_->createSurface(context, surface, error);
}

void runWithHostEventLoop(std::function<void()> body, bool user_interface) {
    if (pthread_main_np() == 0) throw std::logic_error("runWithHostEventLoop must be called on the main thread");

    // Keeps App Nap and timer coalescing from slowing the emulation down while the window is hidden or
    // unfocused.
    id activity =
            [[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
                                                           reason:@"Running the emulated game"];

    bool ui = user_interface;
    if (ui) {
        g_loop_started = true;  // [NSApp run] finishes the launch itself
        std::string error;
        if (!ensureApplication(error)) {
            // Presenter::create reports the same problem when a window is requested.
            logWarn("tox2: window: {}; running without a user interface", error);
            g_loop_started = false;
            ui = false;
        }
    }

    struct Shared {
        std::atomic<bool> done{false};
        std::exception_ptr error;
    };
    const auto shared = std::make_shared<Shared>();
    // The body replaces what ran on the process main thread, whose stack is 8 MiB.
    startHostThread(
            [shared, ui, body = std::move(body)]() mutable {
                try {
                    // Destroyed here, before the main thread is told: once it returns, the caller may be gone.
                    auto run = std::move(body);
                    run();
                } catch (...) {
                    shared->error = std::current_exception();
                }
                dispatch_async(dispatch_get_main_queue(), ^{
                  shared->done.store(true);
                  if (ui) {
                      [NSApp stop:nil];
                      // stop: only takes effect once the loop handles another event.
                      [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                                          location:NSZeroPoint
                                                     modifierFlags:0
                                                         timestamp:0
                                                      windowNumber:0
                                                           context:nil
                                                           subtype:0
                                                             data1:0
                                                             data2:0]
                               atStart:YES];
                  } else {
                      CFRunLoopStop(CFRunLoopGetMain());
                  }
                });
            },
            8u << 20);

    while (!shared->done.load()) {
        if (ui) {
            [NSApp run];
        } else {
            CFRunLoopRun();
        }
    }
    [[NSProcessInfo processInfo] endActivity:activity];
    if (shared->error) std::rethrow_exception(shared->error);
}

void pumpHostEvents() {
    if (pthread_main_np() == 0 || NSApp == nil) return;
    @autoreleasepool {
        std::string error;
        ensureApplication(error);
        for (;;) {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:nil
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (event == nil) break;
            [NSApp sendEvent:event];
        }
        [NSApp updateWindows];
    }
}

}  // namespace tox2
