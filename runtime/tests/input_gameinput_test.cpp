// Tests for the GameInput backend. They need a GameInput runtime that provides API version 3; without
// one the program prints the reason and exits 77 (CTest "skipped"). No pad has to be connected.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "input/gameinput_backend.h"

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

// The runtime reports some devices a few tens of milliseconds after its blocking enumeration has
// returned; wait until the list has been quiet so the stability checks below are not racing it.
void waitForDevices(dal::GamepadBackend& backend) {
    constexpr auto kQuiet = std::chrono::milliseconds(250);
    constexpr auto kLimit = std::chrono::seconds(2);
    const auto start = std::chrono::steady_clock::now();
    auto changed = start;
    std::uint64_t generation = backend.deviceGeneration();
    while (std::chrono::steady_clock::now() - changed < kQuiet && std::chrono::steady_clock::now() - start < kLimit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (const std::uint64_t now = backend.deviceGeneration(); now != generation) {
            generation = now;
            changed = std::chrono::steady_clock::now();
        }
    }
}

void testBackend(dal::GameInputBackend& backend) {
    CHECK(!backend.description().empty());
    waitForDevices(backend);

    const std::uint64_t generation = backend.deviceGeneration();
    const std::vector<dal::GamepadDeviceInfo> devices = backend.devices();
    std::printf("input gameinput: %s, %zu devices\n", backend.description().c_str(), devices.size());

    std::set<std::uint64_t> ids;
    bool all_valid = true;
    for (const dal::GamepadDeviceInfo& info : devices) {
        ids.insert(info.id);
        // The device callback is registered for these kinds only.
        all_valid = all_valid && info.id != 0 && info.kinds != 0;
    }
    CHECK(all_valid);
    CHECK(ids.size() == devices.size());

    // Nothing is plugged in or out while the test runs.
    CHECK(backend.deviceGeneration() == generation);
    CHECK(backend.deviceGeneration() == backend.deviceGeneration());
    CHECK(backend.devices().size() == devices.size());

    // Reading or rumbling an id that does not exist is harmless.
    const std::uint64_t unknown = ids.empty() ? 1 : *ids.rbegin() + 1000;
    CHECK(!backend.read(unknown).has_value());
    CHECK(!backend.readRaw(unknown).has_value());
    backend.setRumble(unknown, {.low_frequency = 1.0f, .high_frequency = 1.0f});
    CHECK(!backend.read(0).has_value());
    CHECK(backend.deviceGeneration() == generation);

    // A present pad answers read() without crashing; whether it has a reading is up to the hardware.
    for (const dal::GamepadDeviceInfo& info : devices) {
        if ((info.kinds & dal::kGamepadKindGamepad) == 0) {
            CHECK(!backend.read(info.id).has_value());
            continue;
        }
        if (const auto reading = backend.read(info.id)) {
            CHECK(reading->left_trigger >= 0.0f && reading->left_trigger <= 1.0f);
            CHECK(reading->right_trigger >= 0.0f && reading->right_trigger <= 1.0f);
        }
    }
}

void testSteamContext() {
    const dal::SteamContext steam = dal::detectSteamContext();
    CHECK(steam.process_id != 0);
    for (const auto& [name, value] : steam.env) {
        CHECK(std::ranges::find(dal::kSteamEnvVars, name) != dal::kSteamEnvVars.end());
        CHECK(!value.empty());
    }
}

}  // namespace

int main() {
    testSteamContext();

    auto backend = dal::makeGameInputBackend({.background_input = true});
    if (!backend) {
        std::printf("input gameinput: skipped, GameInput unavailable: %s\n", backend.error().c_str());
        return g_failed == 0 ? kExitSkip : 1;
    }
    testBackend(**backend);

    std::printf("input gameinput tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
