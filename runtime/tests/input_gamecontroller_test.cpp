// Tests for the GameController.framework backend. The Steam context and the unknown-id behaviour need
// no controller. The rest needs one: the program waits up to three seconds for the framework to report
// one and, when none arrives, prints so and exits 77 (CTest "skipped") if everything else passed.

#include <CoreFoundation/CoreFoundation.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "input/gamecontroller_backend.h"

namespace {

int g_passed = 0;
int g_failed = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            g_passed++;                                                            \
        } else {                                                                   \
            g_failed++;                                                            \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
        }                                                                          \
    } while (0)

constexpr int kExitSkip = 77;

// The framework delivers connect notifications on the main queue, which only this thread serves.
void spinMainRunLoop(std::chrono::milliseconds duration) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, std::chrono::duration<double>(duration).count(), false);
}

void testSteamContext() {
    setenv("SteamGameId", "12345", 1);
    setenv("SteamAppId", "", 1);  // set but empty reads as unset
    unsetenv("SteamOverlayGameId");
    const tox2::SteamContext context = tox2::detectSteamContext();
    CHECK(!context.overlay_loaded);
    CHECK(context.process_id == static_cast<std::uint32_t>(getpid()));
    CHECK(context.env.contains("SteamGameId"));
    CHECK(context.env.at("SteamGameId") == "12345");
    CHECK(!context.env.contains("SteamAppId"));
    CHECK(!context.env.contains("SteamOverlayGameId"));
    // Only the variables of kSteamEnvVars are reported.
    setenv("TOX2_TEST_UNRELATED", "1", 1);
    CHECK(!tox2::detectSteamContext().env.contains("TOX2_TEST_UNRELATED"));

    // Callable from the main thread and from another one, as the gamepad thread does.
    (void)tox2::processOwnsForegroundWindow();
    std::thread([] { (void)tox2::processOwnsForegroundWindow(); }).join();
}

void testBackend(tox2::GamepadBackend& backend) {
    CHECK(!backend.description().empty());

    // Reading or rumbling an id that does not exist is harmless.
    CHECK(!backend.read(0).has_value());
    CHECK(!backend.read(~std::uint64_t{0}).has_value());
    backend.setRumble(~std::uint64_t{0}, {.low_frequency = 1.0f, .high_frequency = 1.0f});

    // Controllers are reported through the main run loop, possibly a little after the call returned.
    constexpr auto kLimit = std::chrono::seconds(3);
    const auto start = std::chrono::steady_clock::now();
    while (backend.devices().empty() && std::chrono::steady_clock::now() - start < kLimit) {
        spinMainRunLoop(std::chrono::milliseconds(50));
    }
    spinMainRunLoop(std::chrono::milliseconds(250));  // let the rest of a multi-controller arrival in
}

// Returns whether a controller was present.
bool testControllers(tox2::GamepadBackend& backend) {
    const std::uint64_t generation = backend.deviceGeneration();
    const std::vector<tox2::GamepadDeviceInfo> devices = backend.devices();
    std::printf("input gamecontroller: %s, %zu devices\n", backend.description().c_str(), devices.size());
    if (devices.empty()) return false;

    std::set<std::uint64_t> ids;
    for (const tox2::GamepadDeviceInfo& info : devices) {
        ids.insert(info.id);
        CHECK(info.id != 0);
        CHECK(info.kinds != 0);
        std::printf("input gamecontroller: id %llu kinds %u rumble %d '%s'\n", static_cast<unsigned long long>(info.id),
                    info.kinds, info.rumble ? 1 : 0, info.display_name.c_str());
    }
    CHECK(ids.size() == devices.size());
    CHECK(std::any_of(devices.begin(), devices.end(), [](const tox2::GamepadDeviceInfo& info) {
        return (info.kinds & tox2::kGamepadKindGamepad) != 0;
    }));

    for (const tox2::GamepadDeviceInfo& info : devices) {
        const std::optional<tox2::GamepadReading> reading = backend.read(info.id);
        // Readable exactly when the device has the gamepad kind.
        CHECK(reading.has_value() == ((info.kinds & tox2::kGamepadKindGamepad) != 0));
        if (!reading) continue;
        const auto stick = [](float v) { return v >= -1.0f && v <= 1.0f; };
        const auto trigger = [](float v) { return v >= 0.0f && v <= 1.0f; };
        CHECK(stick(reading->left_x) && stick(reading->left_y) && stick(reading->right_x) && stick(reading->right_y));
        CHECK(trigger(reading->left_trigger) && trigger(reading->right_trigger));
    }

    // Nothing is plugged in or out while the test runs.
    spinMainRunLoop(std::chrono::milliseconds(300));
    CHECK(backend.deviceGeneration() == generation);
    CHECK(backend.devices().size() == devices.size());
    return true;
}

}  // namespace

int main() {
    testSteamContext();

    auto backend = tox2::makeGameControllerBackend();
    if (!backend) {
        std::printf("input gamecontroller: unavailable: %s\n", backend.error().c_str());
        return 1;
    }
    testBackend(**backend);
    const bool have_controller = testControllers(**backend);

    if (!have_controller) {
        std::printf("input gamecontroller: skipped, no controller connected (%d passed, %d failed)\n", g_passed,
                    g_failed);
        return g_failed == 0 ? kExitSkip : 1;
    }
    std::printf("input gamecontroller tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
