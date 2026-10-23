#include "input/gamecontroller_backend.h"

#import <Cocoa/Cocoa.h>
#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.h"

namespace tox2 {

namespace {

// The latest state of one controller. The controller's handler queue stores it and the gamepad thread
// loads it, so it is the only thing the two share.
class SharedReading {
public:
    void store(const GamepadReading& reading) {
        std::lock_guard<std::mutex> lock(mutex_);
        reading_ = reading;
    }
    GamepadReading load() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return reading_;
    }

private:
    mutable std::mutex mutex_;
    GamepadReading reading_;
};

GamepadReading readState(GCExtendedGamepad* pad) {
    GamepadReading reading;
    const auto press = [&reading](GamepadButton button, GCControllerButtonInput* input) {
        if (input.pressed) reading.buttons |= gamepadButtonBit(button);
    };
    press(GamepadButton::A, pad.buttonA);
    press(GamepadButton::B, pad.buttonB);
    press(GamepadButton::X, pad.buttonX);
    press(GamepadButton::Y, pad.buttonY);
    press(GamepadButton::LeftShoulder, pad.leftShoulder);
    press(GamepadButton::RightShoulder, pad.rightShoulder);
    press(GamepadButton::Menu, pad.buttonMenu);
    press(GamepadButton::View, pad.buttonOptions);  // nil on pads without one, which reads as not pressed
    press(GamepadButton::LeftThumbstick, pad.leftThumbstickButton);
    press(GamepadButton::RightThumbstick, pad.rightThumbstickButton);
    press(GamepadButton::DPadUp, pad.dpad.up);
    press(GamepadButton::DPadDown, pad.dpad.down);
    press(GamepadButton::DPadLeft, pad.dpad.left);
    press(GamepadButton::DPadRight, pad.dpad.right);
    // The framework's sticks already have +y up and its triggers run 0 to 1.
    reading.left_x = pad.leftThumbstick.xAxis.value;
    reading.left_y = pad.leftThumbstick.yAxis.value;
    reading.right_x = pad.rightThumbstick.xAxis.value;
    reading.right_y = pad.rightThumbstick.yAxis.value;
    reading.left_trigger = pad.leftTrigger.value;
    reading.right_trigger = pad.rightTrigger.value;
    return reading;
}

// One motor of a controller: a haptic engine on one handle (or on the whole controller), playing a
// long continuous event whose strength is set with a dynamic intensity parameter. The player loops so
// the event does not end while the game keeps the motor on. Used by the gamepad thread only; the
// engine's own callbacks on other queues just raise `stale_`.
class Motor {
public:
    Motor(GCController* controller, GCHapticsLocality locality, float sharpness)
        : controller_(controller), locality_(locality), sharpness_(sharpness) {}

    ~Motor() { teardown(); }
    Motor(const Motor&) = delete;
    Motor& operator=(const Motor&) = delete;

    void set(float intensity) {
        if (intensity <= 0.0f) {
            if (playing_) {
                NSError* error = nil;
                if (![player_ stopAtTime:CHHapticTimeImmediate error:&error]) report("stop", error);
                playing_ = false;
            }
            return;
        }
        // The engine stopped or was reset by the system (for example after the app was suspended).
        if (stale_->load()) teardown();
        if (engine_ == nil && !build()) return;
        NSError* error = nil;
        if (!playing_) {
            if (![player_ startAtTime:CHHapticTimeImmediate error:&error]) return report("start", error);
            playing_ = true;
        }
        CHHapticDynamicParameter* parameter =
                [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
                                                                value:intensity
                                                         relativeTime:0];
        if (![player_ sendParameters:@[ parameter ] atTime:CHHapticTimeImmediate error:&error]) {
            report("set intensity", error);
        }
    }

private:
    bool build() {
        NSError* error = nil;
        CHHapticEngine* engine = [controller_.haptics createEngineWithLocality:locality_];
        if (engine == nil) {
            report("create engine", nil);
            return false;
        }
        engine.playsHapticsOnly = YES;
        // A flag per engine: the stop of a replaced engine must not mark its successor stale.
        const auto stale = std::make_shared<std::atomic<bool>>(false);
        engine.stoppedHandler = ^(CHHapticEngineStoppedReason) {
          stale->store(true);
        };
        engine.resetHandler = ^{
          stale->store(true);
        };
        if (![engine startAndReturnError:&error]) {
            report("start engine", error);
            return false;
        }
        // 30 s is the longest continuous event Core Haptics accepts.
        CHHapticEvent* event = [[CHHapticEvent alloc]
                initWithEventType:CHHapticEventTypeHapticContinuous
                       parameters:@[
                           [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity
                                                                         value:1.0f],
                           [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness
                                                                         value:sharpness_],
                       ]
                     relativeTime:0
                         duration:30.0];
        CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[ event ] parameters:@[] error:&error];
        id<CHHapticAdvancedPatternPlayer> player =
                pattern != nil ? [engine createAdvancedPlayerWithPattern:pattern error:&error] : nil;
        if (player == nil) {
            [engine stopWithCompletionHandler:nil];
            report("create player", error);
            return false;
        }
        player.loopEnabled = YES;
        engine_ = engine;
        player_ = player;
        stale_ = stale;
        playing_ = false;
        return true;
    }

    void teardown() {
        if (engine_ == nil) return;
        [engine_ stopWithCompletionHandler:nil];
        engine_ = nil;
        player_ = nil;
        playing_ = false;
    }

    // One warning per motor: a failing engine would otherwise repeat it on every change of strength.
    void report(const char* what, NSError* error) {
        if (reported_) return;
        reported_ = true;
        logWarn("tox2: pad: haptics: {} failed: {}", what,
                error != nil ? error.localizedDescription.UTF8String : "no engine for this locality");
    }

    GCController* controller_;
    GCHapticsLocality locality_;
    float sharpness_;
    CHHapticEngine* engine_ = nil;
    id<CHHapticAdvancedPatternPlayer> player_ = nil;
    std::shared_ptr<std::atomic<bool>> stale_ = std::make_shared<std::atomic<bool>>(false);
    bool playing_ = false;
    bool reported_ = false;
};

// The two body motors of a controller. Engines are created on the first nonzero strength, so a
// controller that is never rumbled never claims its haptics hardware.
class Haptics {
public:
    explicit Haptics(GCController* controller) {
        NSSet<GCHapticsLocality>* supported = controller.haptics.supportedLocalities;
        if ([supported containsObject:GCHapticsLocalityLeftHandle] &&
            [supported containsObject:GCHapticsLocalityRightHandle]) {
            low_.emplace(controller, GCHapticsLocalityLeftHandle, 0.5f);
            high_.emplace(controller, GCHapticsLocalityRightHandle, 0.5f);
        } else {
            // One actuator group: the two engines differ in sharpness only.
            low_.emplace(controller, GCHapticsLocalityDefault, 0.0f);
            high_.emplace(controller, GCHapticsLocalityDefault, 1.0f);
        }
    }

    void set(const GamepadRumble& rumble) {
        low_->set(std::clamp(rumble.low_frequency, 0.0f, 1.0f));
        high_->set(std::clamp(rumble.high_frequency, 0.0f, 1.0f));
    }

private:
    std::optional<Motor> low_, high_;
};

struct Device {
    GamepadDeviceInfo info;
    GCController* controller = nil;
    bool readable = false;  // has an extended gamepad profile
    std::shared_ptr<SharedReading> reading;
    std::shared_ptr<Haptics> haptics;  // null without haptics support
};

class Backend : public GamepadBackend, public std::enable_shared_from_this<Backend> {
public:
    ~Backend() override {
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
        if (connect_observer_ != nil) [center removeObserver:connect_observer_];
        if (disconnect_observer_ != nil) [center removeObserver:disconnect_observer_];
        for (Device& device : devices_) detach(device);
    }

    // Separate from the constructor: the observers need a shared_ptr to this object.
    void start() {
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
        const std::weak_ptr<Backend> weak = weak_from_this();
        // On the main queue, as the framework documents; the blocks only take the backend's mutex.
        connect_observer_ = [center addObserverForName:GCControllerDidConnectNotification
                                                object:nil
                                                 queue:NSOperationQueue.mainQueue
                                            usingBlock:^(NSNotification* note) {
                                              if (const auto backend = weak.lock()) backend->add(note.object);
                                            }];
        disconnect_observer_ = [center addObserverForName:GCControllerDidDisconnectNotification
                                                   object:nil
                                                    queue:NSOperationQueue.mainQueue
                                               usingBlock:^(NSNotification* note) {
                                                 if (const auto backend = weak.lock()) backend->remove(note.object);
                                               }];
        for (GCController* controller in GCController.controllers) add(controller);
    }

    std::string description() const override { return "GameController.framework"; }

    std::uint64_t deviceGeneration() const override { return generation_.load(); }

    std::vector<GamepadDeviceInfo> devices() override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<GamepadDeviceInfo> result;
        result.reserve(devices_.size());
        for (const Device& device : devices_) result.push_back(device.info);
        return result;
    }

    std::optional<GamepadReading> read(std::uint64_t id) override {
        std::shared_ptr<SharedReading> reading;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const Device& device : devices_) {
                if (device.info.id == id && device.readable) reading = device.reading;
            }
        }
        if (!reading) return std::nullopt;
        return reading->load();
    }

    void setRumble(std::uint64_t id, const GamepadRumble& rumble) override {
        std::shared_ptr<Haptics> haptics;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const Device& device : devices_) {
                if (device.info.id == id) haptics = device.haptics;
            }
        }
        if (haptics) haptics->set(rumble);
    }

private:
    void add(id object) {
        if (![object isKindOfClass:GCController.class]) return;
        GCController* controller = object;
        std::lock_guard<std::mutex> lock(mutex_);
        // The initial scan and a connect notification can both report the same controller.
        for (const Device& device : devices_) {
            if (device.controller == controller) return;
        }

        Device device;
        device.controller = controller;
        device.info.id = next_id_++;
        GCExtendedGamepad* extended = controller.extendedGamepad;
        device.readable = extended != nil;
        device.info.kinds = device.readable ? kGamepadKindGamepad : kGamepadKindController;
        if (NSString* name = controller.vendorName) device.info.display_name = name.UTF8String;
        device.reading = std::make_shared<SharedReading>();
        if (controller.haptics != nil) {
            device.info.rumble = true;
            device.haptics = std::make_shared<Haptics>(controller);
        }
        if (extended != nil) {
            // A private serial queue per controller; the handler writes only this device's reading.
            controller.handlerQueue = dispatch_queue_create(
                    std::format("tox2.gamecontroller.{}", device.info.id).c_str(), DISPATCH_QUEUE_SERIAL);
            const std::shared_ptr<SharedReading> reading = device.reading;
            extended.valueChangedHandler = ^(GCExtendedGamepad* gamepad, GCControllerElement*) {
              reading->store(readState(gamepad));
            };
            // The handler only runs on changes; start from what is held down now.
            reading->store(readState(extended));
        }
        devices_.push_back(std::move(device));
        generation_.fetch_add(1);
    }

    void remove(id object) {
        if (![object isKindOfClass:GCController.class]) return;
        GCController* controller = object;
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = devices_.begin(); it != devices_.end(); ++it) {
            if (it->controller != controller) continue;
            detach(*it);
            devices_.erase(it);
            generation_.fetch_add(1);
            return;
        }
    }

    static void detach(Device& device) { device.controller.extendedGamepad.valueChangedHandler = nil; }

    std::mutex mutex_;  // devices_ and next_id_
    std::vector<Device> devices_;
    std::uint64_t next_id_ = 1;
    std::atomic<std::uint64_t> generation_{0};
    id connect_observer_ = nil;
    id disconnect_observer_ = nil;
};

}  // namespace

std::expected<std::shared_ptr<GamepadBackend>, std::string> makeGameControllerBackend() {
    if (NSClassFromString(@"GCController") == nil) return std::unexpected("GameController.framework is not available");
    auto backend = std::make_shared<Backend>();
    backend->start();
    return backend;
}

SteamContext detectSteamContext() {
    SteamContext context;
    context.process_id = static_cast<std::uint32_t>(getpid());
    for (const std::string_view name : kSteamEnvVars) {
        const char* value = std::getenv(std::string(name).c_str());
        // A set but empty variable reads as unset.
        if (value == nullptr || *value == '\0') continue;
        context.env.emplace(std::string(name), value);
    }
    return context;
}

bool processOwnsForegroundWindow() {
    @autoreleasepool {
        return NSRunningApplication.currentApplication.active;
    }
}

}  // namespace tox2
