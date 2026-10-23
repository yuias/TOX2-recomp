#include "input/gamecontroller_backend.h"

#import <Cocoa/Cocoa.h>
#import <GameController/GameController.h>

#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

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

struct Device {
    GamepadDeviceInfo info;
    GCController* controller = nil;
    bool readable = false;  // has an extended gamepad profile
    std::shared_ptr<SharedReading> reading;
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

    void setRumble(std::uint64_t, const GamepadRumble&) override {}

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
