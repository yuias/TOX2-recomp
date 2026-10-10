// Self-checking unit tests for the gamepad code in the runtime core. A fake backend stands in for
// GameInput, so nothing here needs a pad, a Windows input runtime or generated code.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/log.h"
#include "input/gamepad_backend.h"
#include "input/gamepad_hub.h"
#include "input/gamepad_map.h"
#include "input/gamepad_probe.h"
#include "input/gamepad_select.h"
#include "input/keyboard_pad.h"
#include "input/pad_buttons.h"

namespace {

int g_passed;
int g_failed;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            g_passed++;                                                            \
        } else {                                                                   \
            g_failed++;                                                            \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
        }                                                                          \
    } while (0)

using dal::GamepadButton;
using dal::gamepadButtonBit;

// A scriptable GamepadBackend: tests set the device list and the readings, and read the counters.
class FakeBackend final : public dal::GamepadBackend {
public:
    std::string description() const override { return "fake"; }

    std::uint64_t deviceGeneration() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation_;
    }

    std::vector<dal::GamepadDeviceInfo> devices() override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<dal::GamepadDeviceInfo> list;
        for (const Entry& entry : entries_) list.push_back(entry.info);
        return list;
    }

    std::optional<dal::GamepadReading> read(std::uint64_t id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (Entry& entry : entries_) {
            if (entry.info.id != id) continue;
            ++entry.reads;
            return entry.reading;
        }
        return std::nullopt;
    }

    void setRumble(std::uint64_t id, const dal::GamepadRumble& rumble) override {
        std::lock_guard<std::mutex> lock(mutex_);
        rumble_calls_.emplace_back(id, rumble);
    }

    // Plugs in a device and returns its id.
    std::uint64_t add(std::uint16_t vendor_id, std::uint16_t product_id,
                      std::uint32_t kinds = dal::kGamepadKindGamepad) {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry entry;
        entry.info.id = next_id_++;
        entry.info.vendor_id = vendor_id;
        entry.info.product_id = product_id;
        entry.info.kinds = kinds;
        entries_.push_back(entry);
        ++generation_;
        return entry.info.id;
    }

    // Plugs in a described device (its id is replaced) and returns its id.
    std::uint64_t add(dal::GamepadDeviceInfo info) {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry entry;
        entry.info = std::move(info);
        entry.info.id = next_id_++;
        entries_.push_back(entry);
        ++generation_;
        return entry.info.id;
    }

    int reads(std::uint64_t id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const Entry& entry : entries_) {
            if (entry.info.id == id) return entry.reads;
        }
        return 0;
    }

    std::vector<std::pair<std::uint64_t, dal::GamepadRumble>> rumbleCalls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return rumble_calls_;
    }

    void remove(std::uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::erase_if(entries_, [&](const Entry& e) { return e.info.id == id; });
        ++generation_;
    }

    // nullopt: the device has no reading.
    void setReading(std::uint64_t id, std::optional<dal::GamepadReading> reading) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (Entry& entry : entries_) {
            if (entry.info.id == id) entry.reading = reading;
        }
    }

private:
    struct Entry {
        dal::GamepadDeviceInfo info;
        std::optional<dal::GamepadReading> reading = dal::GamepadReading{};
        int reads = 0;
    };

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::uint64_t next_id_ = 1;
    std::uint64_t generation_ = 0;
    std::vector<std::pair<std::uint64_t, dal::GamepadRumble>> rumble_calls_;
};

// Collects log lines on any thread while it is alive.
class LogCapture {
public:
    LogCapture() {
        dal::setLogSink([this](std::string_view line) {
            std::lock_guard<std::mutex> lock(mutex_);
            lines_.emplace_back(line);
        });
    }
    ~LogCapture() { dal::setLogSink({}); }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    std::vector<std::string> lines() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.clear();
    }
    // Number of lines that contain `text`.
    int count(std::string_view text) const {
        int n = 0;
        for (const std::string& line : lines()) n += line.find(text) != std::string::npos ? 1 : 0;
        return n;
    }
    // Polls until a line containing `text` exists; the sink is global, so waits are bounded.
    bool waitFor(std::string_view text, std::chrono::milliseconds limit = std::chrono::seconds(5)) const {
        const auto end = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < end) {
            if (count(text) > 0) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

void testFormatValue() {
    CHECK(dal::formatGamepadValue(0.0f) == "0.00");
    CHECK(dal::formatGamepadValue(-0.0f) == "0.00");
    // A tiny negative value rounds to zero without keeping its sign.
    CHECK(dal::formatGamepadValue(-0.01f) == "0.00");
    CHECK(dal::formatGamepadValue(0.01f) == "0.00");
    CHECK(dal::formatGamepadValue(0.024f) == "0.00");
    // 0.049 is inside the step that rounds to 0.05; 0.026 is just over the boundary.
    CHECK(dal::formatGamepadValue(0.049f) == "0.05");
    CHECK(dal::formatGamepadValue(0.026f) == "0.05");
    CHECK(dal::formatGamepadValue(0.074f) == "0.05");
    CHECK(dal::formatGamepadValue(0.076f) == "0.10");
    CHECK(dal::formatGamepadValue(0.5f) == "0.50");
    CHECK(dal::formatGamepadValue(1.0f) == "1.00");
    CHECK(dal::formatGamepadValue(-1.0f) == "-1.00");
    CHECK(dal::formatGamepadValue(-0.049f) == "-0.05");
}

void testFormatReading() {
    CHECK(dal::formatGamepadReading({}) == "buttons none lt 0.00 rt 0.00 ls (0.00,0.00) rs (0.00,0.00)");

    dal::GamepadReading reading;
    reading.buttons = gamepadButtonBit(GamepadButton::A) | gamepadButtonBit(GamepadButton::RightShoulder);
    reading.left_trigger = 0.5f;
    reading.right_trigger = 1.0f;
    reading.left_x = -0.25f;
    reading.left_y = 0.999f;
    reading.right_x = -0.0f;
    reading.right_y = -1.0f;
    CHECK(dal::formatGamepadReading(reading) ==
          "buttons A+RightShoulder lt 0.50 rt 1.00 ls (-0.25,1.00) rs (0.00,-1.00)");

    // Every button has its own name, in the order of the enum.
    const char* const names[] = {"Menu",
                                 "View",
                                 "A",
                                 "B",
                                 "X",
                                 "Y",
                                 "DPadUp",
                                 "DPadDown",
                                 "DPadLeft",
                                 "DPadRight",
                                 "LeftShoulder",
                                 "RightShoulder",
                                 "LeftThumbstick",
                                 "RightThumbstick"};
    bool names_match = true;
    for (unsigned i = 0; i < static_cast<unsigned>(GamepadButton::Count); ++i) {
        dal::GamepadReading single;
        single.buttons = gamepadButtonBit(static_cast<GamepadButton>(i));
        const std::string expected =
                std::string("buttons ") + names[i] + " lt 0.00 rt 0.00 ls (0.00,0.00) rs (0.00,0.00)";
        names_match = names_match && dal::formatGamepadReading(single) == expected;
    }
    CHECK(names_match);
}

void testFormatDeviceParts() {
    CHECK(dal::formatGamepadKinds(dal::kGamepadKindGamepad) == "gamepad");
    CHECK(dal::formatGamepadKinds(dal::kGamepadKindController) == "controller");
    CHECK(dal::formatGamepadKinds(dal::kGamepadKindGamepad | dal::kGamepadKindController) == "gamepad+controller");
    CHECK(dal::formatGamepadKinds(0) == "none");

    dal::GamepadDeviceInfo info;
    info.vendor_id = 0x045e;
    info.product_id = 0x028e;
    CHECK(dal::formatGamepadVidPid(info) == "045e:028e");
    info.vendor_id = 0x54c;
    info.product_id = 0x9;
    CHECK(dal::formatGamepadVidPid(info) == "054c:0009");

    dal::SteamContext steam;
    CHECK(dal::formatSteamContext(steam) == "overlay not loaded");
    steam.overlay_loaded = true;
    CHECK(dal::formatSteamContext(steam) == "overlay loaded");
    steam.env["SteamGameId"] = "123";
    steam.env["SteamVirtualGamepadInfo"] = "a-file-path";
    steam.env["EnableConfiguratorSupport"] = "15";
    // std::map order: names sort alphabetically; the file path never appears.
    CHECK(dal::formatSteamContext(steam) ==
          "overlay loaded; EnableConfiguratorSupport=15 SteamGameId=123 SteamVirtualGamepadInfo=set");
}

void testProbeLogsChangesOnly() {
    LogCapture log;
    auto backend = std::make_shared<FakeBackend>();
    const std::uint64_t id = backend->add(0x045e, 0x028e);
    dal::GamepadProbe probe(backend);

    probe.step();
    CHECK(log.lines().size() == 1);
    CHECK(log.count("dal: pad: probe 1 045e:028e buttons none lt 0.00 rt 0.00 ls (0.00,0.00) rs (0.00,0.00)") == 1);

    // The same reading, and a change below the 0.05 rounding step, print nothing.
    probe.step();
    dal::GamepadReading reading;
    reading.left_x = 0.01f;
    backend->setReading(id, reading);
    probe.step();
    CHECK(log.lines().size() == 1);

    reading.buttons = gamepadButtonBit(GamepadButton::A);
    reading.left_x = 0.5f;
    backend->setReading(id, reading);
    probe.step();
    CHECK(log.lines().size() == 2);
    CHECK(log.count("dal: pad: probe 1 045e:028e buttons A lt 0.00 rt 0.00 ls (0.50,0.00) rs (0.00,0.00)") == 1);
    probe.step();
    CHECK(log.lines().size() == 2);

    // Going back to the first text prints again: the comparison is with the last printed one.
    backend->setReading(id, dal::GamepadReading{});
    probe.step();
    CHECK(log.lines().size() == 3);
}

void testProbeReportsMissingReadingOnce() {
    LogCapture log;
    auto backend = std::make_shared<FakeBackend>();
    const std::uint64_t pad = backend->add(0x045e, 0x028e);
    const std::uint64_t other = backend->add(0x054c, 0x05c4, dal::kGamepadKindController);
    backend->setReading(pad, std::nullopt);
    backend->setReading(other, std::nullopt);
    dal::GamepadProbe probe(backend);

    probe.step();
    probe.step();
    probe.step();
    CHECK(log.lines().size() == 2);
    CHECK(log.count("dal: pad: probe 1 045e:028e no gamepad reading") == 1);
    CHECK(log.count("dal: pad: probe 2 054c:05c4 no gamepad reading") == 1);

    // A reading that arrives later is an ordinary change.
    backend->setReading(pad, dal::GamepadReading{});
    probe.step();
    CHECK(log.lines().size() == 3);
    CHECK(log.count("dal: pad: probe 1 045e:028e buttons none") == 1);
}

void testProbeFollowsDevices() {
    LogCapture log;
    auto backend = std::make_shared<FakeBackend>();
    dal::GamepadProbe probe(backend);

    probe.step();
    CHECK(log.lines().empty());  // no device, no line

    const std::uint64_t first = backend->add(0x045e, 0x028e);
    const std::uint64_t second = backend->add(0x054c, 0x09cc);
    probe.step();
    CHECK(log.lines().size() == 2);

    // Each device keeps its own last text.
    dal::GamepadReading reading;
    reading.buttons = gamepadButtonBit(GamepadButton::B);
    backend->setReading(second, reading);
    probe.step();
    CHECK(log.lines().size() == 3);
    CHECK(log.count("dal: pad: probe 2 054c:09cc buttons B") == 1);

    // A removed device is forgotten; a new one (new id) prints its first reading again.
    backend->remove(first);
    const std::uint64_t third = backend->add(0x045e, 0x028e);
    CHECK(third != first);
    probe.step();
    CHECK(log.lines().size() == 4);
    CHECK(log.count("dal: pad: probe 3 045e:028e buttons none") == 1);
}

void testSteamVirtualGamepadInfoLog() {
    {
        LogCapture log;
        dal::logSteamVirtualGamepadInfo({});
        CHECK(log.lines().size() == 1);
        CHECK(log.count("dal: pad: probe SteamVirtualGamepadInfo unset") == 1);
    }

    const std::filesystem::path file =
            std::filesystem::temp_directory_path() /
            std::format("dal_input_core_test_vgp_info_{}.txt",
                        std::chrono::steady_clock::now().time_since_epoch().count());  // unique under ctest -j
    {
        std::ofstream out(file, std::ios::binary);
        out << "[slot 0]\r\nvid=0x045e\n\npid=0x028e";
    }
    {
        LogCapture log;
        dal::SteamContext steam;
        steam.env["SteamVirtualGamepadInfo"] = file.string();
        dal::logSteamVirtualGamepadInfo(steam);
        // Lines are split at \n, a trailing \r is dropped, and the last line needs no newline.
        CHECK(log.lines().size() == 4);
        CHECK(log.count("dal: pad: probe SteamVirtualGamepadInfo: [slot 0]") == 1);
        CHECK(log.count("dal: pad: probe SteamVirtualGamepadInfo: vid=0x045e") == 1);
        CHECK(log.count("dal: pad: probe SteamVirtualGamepadInfo: pid=0x028e") == 1);
    }
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << std::string(10000, 'x');
    }
    {
        LogCapture log;
        dal::SteamContext steam;
        steam.env["SteamVirtualGamepadInfo"] = file.string();
        dal::logSteamVirtualGamepadInfo(steam);
        // Only the first 4 KiB are read.
        CHECK(log.lines().size() == 1);
        CHECK(log.lines().front().size() < 4096 + 100);
    }
    {
        LogCapture log;
        dal::SteamContext steam;
        steam.env["SteamVirtualGamepadInfo"] = (file.parent_path() / "dal_input_core_test_missing.txt").string();
        dal::logSteamVirtualGamepadInfo(steam);
        CHECK(log.count("dal: pad: probe SteamVirtualGamepadInfo unreadable") == 1);
    }
    std::filesystem::remove(file);
}

dal::GamepadReading readingWithButton(GamepadButton button) {
    dal::GamepadReading reading;
    reading.buttons = gamepadButtonBit(button);
    return reading;
}

void testMapButtonTable() {
    struct Row {
        GamepadButton from;
        dal::PadButton to;
    };
    const Row rows[] = {
            {GamepadButton::A, dal::PadButton::Cross},           {GamepadButton::B, dal::PadButton::Circle},
            {GamepadButton::X, dal::PadButton::Square},          {GamepadButton::Y, dal::PadButton::Triangle},
            {GamepadButton::DPadUp, dal::PadButton::Up},         {GamepadButton::DPadDown, dal::PadButton::Down},
            {GamepadButton::DPadLeft, dal::PadButton::Left},     {GamepadButton::DPadRight, dal::PadButton::Right},
            {GamepadButton::LeftShoulder, dal::PadButton::L1},   {GamepadButton::RightShoulder, dal::PadButton::R1},
            {GamepadButton::View, dal::PadButton::Select},       {GamepadButton::Menu, dal::PadButton::Start},
            {GamepadButton::LeftThumbstick, dal::PadButton::L3}, {GamepadButton::RightThumbstick, dal::PadButton::R3},
    };
    static_assert(std::size(rows) == static_cast<std::size_t>(GamepadButton::Count));
    for (const Row& row : rows) {
        const dal::MappedPad pad = dal::mapGamepadReading(readingWithButton(row.from));
        CHECK(pad.buttons == dal::padButtonBit(row.to));
        CHECK(pad.analog == dal::PadAnalog{});
    }

    dal::GamepadReading all;
    all.buttons = gamepadButtonBit(GamepadButton::A) | gamepadButtonBit(GamepadButton::B) |
                  gamepadButtonBit(GamepadButton::DPadUp);
    CHECK(dal::mapGamepadReading(all).buttons ==
          (dal::padButtonBit(dal::PadButton::Cross) | dal::padButtonBit(dal::PadButton::Circle) |
           dal::padButtonBit(dal::PadButton::Up)));
}

void testMapIdleReading() {
    CHECK(dal::mapGamepadReading(dal::GamepadReading{}) == dal::MappedPad{});

    // Noise inside the deadzones and below the trigger threshold is still idle.
    dal::GamepadReading noisy;
    noisy.left_x = 0.1f;
    noisy.left_y = -0.15f;
    noisy.right_x = -0.15f;
    noisy.right_y = 0.15f;
    noisy.left_trigger = 29.0f / 255.0f;
    noisy.right_trigger = 0.05f;
    CHECK(dal::mapGamepadReading(noisy) == dal::MappedPad{});
}

void testStickByte() {
    CHECK(dal::stickByte(0.0f) == 0x80);
    CHECK(dal::stickByte(-0.0f) == 0x80);
    CHECK(dal::stickByte(1.0f) == 0xFF);
    CHECK(dal::stickByte(-1.0f) == 0x00);
    // Out-of-range input is clamped.
    CHECK(dal::stickByte(1.5f) == 0xFF);
    CHECK(dal::stickByte(-1.5f) == 0x00);
    CHECK(dal::stickByte(0.5f) == 191);  // lround(1.5 * 127.5) = 191
    CHECK(dal::stickByte(-0.5f) == 64);  // lround(0.5 * 127.5) = 64
}

void testMapSticks() {
    dal::GamepadReading reading;
    reading.left_x = 1.0f;
    CHECK(dal::mapGamepadReading(reading).analog.left_x == 0xFF);
    reading.left_x = -1.0f;
    CHECK(dal::mapGamepadReading(reading).analog.left_x == 0x00);

    // GameInput +y is up and CellPadData 0 is up.
    reading = {};
    reading.left_y = 1.0f;
    reading.right_y = -1.0f;
    dal::MappedPad pad = dal::mapGamepadReading(reading);
    CHECK(pad.analog.left_y == 0x00);
    CHECK(pad.analog.right_y == 0xFF);
    CHECK(pad.analog.left_x == 0x80 && pad.analog.right_x == 0x80);

    // Each stick has its own deadzone.
    reading = {};
    reading.left_x = 0.23f;   // below 0.2395
    reading.right_x = 0.25f;  // below 0.2652
    CHECK(dal::mapGamepadReading(reading) == dal::MappedPad{});
    reading.left_x = 0.25f;   // above the left deadzone
    reading.right_x = 0.28f;  // above the right deadzone
    pad = dal::mapGamepadReading(reading);
    CHECK(pad.analog.left_x != 0x80);
    CHECK(pad.analog.right_x != 0x80);

    // Just outside the deadzone the output is near the centre: the rescaling avoids a jump.
    reading = {};
    reading.left_x = dal::kLeftStickDeadzone + 0.001f;
    reading.right_y = -(dal::kRightStickDeadzone + 0.001f);
    pad = dal::mapGamepadReading(reading);
    CHECK(std::abs(static_cast<int>(pad.analog.left_x) - 0x80) <= 2);
    CHECK(pad.analog.left_x != 0x00);
    CHECK(std::abs(static_cast<int>(pad.analog.right_y) - 0x80) <= 2);

    // The deadzone is radial: each axis alone is inside it, but the diagonal (radius 0.28) is outside,
    // while a diagonal of radius 0.21 is idle.
    const auto [zx, zy] = dal::applyRadialDeadzone(0.15f, 0.15f, dal::kLeftStickDeadzone);
    CHECK(zx == 0.0f && zy == 0.0f);
    const auto [ox, oy] = dal::applyRadialDeadzone(0.2f, 0.2f, dal::kLeftStickDeadzone);
    CHECK(ox > 0.0f && oy > 0.0f);
    const auto [ex, ey] = dal::applyRadialDeadzone(0.0f, 0.0f, dal::kLeftStickDeadzone);
    CHECK(ex == 0.0f && ey == 0.0f);

    // Direction is kept and a full deflection stays 1.
    const auto [fx, fy] = dal::applyRadialDeadzone(0.0f, -1.0f, dal::kRightStickDeadzone);
    CHECK(fx == 0.0f && std::abs(fy + 1.0f) < 1e-6f);
}

void testMapDiagonalClamped() {
    // (1, 1) has radius sqrt(2); it is clamped to radius 1, not to the square's corner.
    const auto [x, y] = dal::applyRadialDeadzone(1.0f, 1.0f, dal::kLeftStickDeadzone);
    CHECK(std::abs(x - y) < 1e-6f);
    CHECK(std::abs(x - std::sqrt(0.5f)) < 1e-5f);
    const int expected = dal::stickByte(std::sqrt(0.5f));
    CHECK(dal::stickByte(x) == dal::stickByte(y));
    CHECK(std::abs(static_cast<int>(dal::stickByte(x)) - expected) <= 1);

    dal::GamepadReading reading;
    reading.left_x = 1.0f;
    reading.left_y = 1.0f;
    const dal::MappedPad pad = dal::mapGamepadReading(reading);
    CHECK(std::abs(static_cast<int>(pad.analog.left_x) - expected) <= 1);
    // The y byte mirrors the x byte around the centre because y is inverted.
    CHECK(std::abs(static_cast<int>(pad.analog.left_x) + static_cast<int>(pad.analog.left_y) - 255) <= 1);
}

void testMapTriggers() {
    dal::GamepadReading reading;
    reading.left_trigger = 29.0f / 255.0f;
    reading.right_trigger = 29.0f / 255.0f;
    dal::MappedPad pad = dal::mapGamepadReading(reading);
    CHECK(pad.buttons == 0);
    CHECK(pad.analog.l2 == 0 && pad.analog.r2 == 0);

    reading.left_trigger = 30.0f / 255.0f;
    pad = dal::mapGamepadReading(reading);
    CHECK(pad.buttons == dal::padButtonBit(dal::PadButton::L2));
    CHECK(pad.analog.l2 == 30);
    CHECK(pad.analog.r2 == 0);

    reading.left_trigger = 1.0f;
    reading.right_trigger = 1.0f;
    pad = dal::mapGamepadReading(reading);
    CHECK(pad.buttons == (dal::padButtonBit(dal::PadButton::L2) | dal::padButtonBit(dal::PadButton::R2)));
    CHECK(pad.analog.l2 == 255 && pad.analog.r2 == 255);

    // The right trigger is R2 on its own.
    reading = {};
    reading.right_trigger = 0.5f;
    pad = dal::mapGamepadReading(reading);
    CHECK(pad.buttons == dal::padButtonBit(dal::PadButton::R2));
    CHECK(pad.analog.r2 == 128);  // lround(127.5)
}

void testMergeMappedPads() {
    CHECK(dal::mergeMappedPads({}) == dal::MappedPad{});

    dal::MappedPad first;
    first.buttons = dal::padButtonBit(dal::PadButton::Cross);
    first.analog.left_x = 0x90;  // 16 right of centre
    first.analog.left_y = 0x10;  // 112 up
    first.analog.right_x = 0x80;
    first.analog.l2 = 40;
    dal::MappedPad second;
    second.buttons = dal::padButtonBit(dal::PadButton::Circle);
    second.analog.left_x = 0x10;  // 112 left: farther than the first
    second.analog.left_y = 0xA0;  // 32 down: nearer than the first
    second.analog.right_x = 0xF0;
    second.analog.l2 = 200;
    second.analog.r2 = 7;

    const dal::MappedPad pads[] = {first, second};
    const dal::MappedPad merged = dal::mergeMappedPads(pads);
    CHECK(merged.buttons == (dal::padButtonBit(dal::PadButton::Cross) | dal::padButtonBit(dal::PadButton::Circle)));
    CHECK(merged.analog.left_x == 0x10);
    CHECK(merged.analog.left_y == 0x10);
    CHECK(merged.analog.right_x == 0xF0);
    CHECK(merged.analog.right_y == 0x80);
    CHECK(merged.analog.l2 == 200);
    CHECK(merged.analog.r2 == 7);

    // One pad merges to itself.
    const dal::MappedPad one[] = {second};
    CHECK(dal::mergeMappedPads(one) == second);

    // Equal distance on opposite sides: the first pad wins.
    dal::MappedPad left, right;
    left.analog.left_x = 0x40;   // 64 left
    right.analog.left_x = 0xC0;  // 64 right
    const dal::MappedPad tie[] = {left, right};
    CHECK(dal::mergeMappedPads(tie).analog.left_x == 0x40);
    const dal::MappedPad tie_swapped[] = {right, left};
    CHECK(dal::mergeMappedPads(tie_swapped).analog.left_x == 0xC0);
}

void testRumbleFromCellPad() {
    CHECK(dal::rumbleFromCellPad(0, 0) == dal::GamepadRumble{});
    const dal::GamepadRumble small = dal::rumbleFromCellPad(1, 0);
    CHECK(small.high_frequency == 1.0f && small.low_frequency == 0.0f);
    const dal::GamepadRumble full_small = dal::rumbleFromCellPad(255, 0);
    CHECK(full_small.high_frequency == 1.0f);
    const dal::GamepadRumble large = dal::rumbleFromCellPad(0, 255);
    CHECK(large.low_frequency == 1.0f && large.high_frequency == 0.0f);
    const dal::GamepadRumble half = dal::rumbleFromCellPad(0, 128);
    CHECK(half.low_frequency == 128.0f / 255.0f && half.high_frequency == 0.0f);
    const dal::GamepadRumble both = dal::rumbleFromCellPad(3, 255);
    CHECK(both.low_frequency == 1.0f && both.high_frequency == 1.0f);
}

// Device paths measured with the Xbox Wireless Controller over Bluetooth LE; the Bluetooth address
// is replaced and the tail after the interface is synthetic.
constexpr const char* kXboxPhysicalPath =
        R"(\\?\HID#{00001812-0000-1000-8000-00805f9b34fb}&Dev&VID_045e&PID_0b13&REV_0509&0123456789ab&IG_00#c&1a2b3c4d&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030})";
constexpr const char* kXboxName = "Bluetooth LE XINPUT compatible input device";

// Values of the recorded Steam shortcut runs (with and without per-game Steam Input). The variable
// holds 55 entries; these are representative ones of the recorded vendors, none of them 0x045e.
constexpr const char* kRecordedIgnoreDevices =
        "0x28de/0x1102,0x28de/0x1142,0x28de/0x11ff,0x2dc8/0x6101,0x0f0d/0x0092,0x0000/0x11fb";

dal::GamepadDeviceInfo xboxPad(std::uint64_t id) {
    dal::GamepadDeviceInfo info;
    info.id = id;
    info.vendor_id = 0x045e;
    info.product_id = 0x0b13;
    info.kinds = dal::kGamepadKindGamepad | dal::kGamepadKindController;
    info.rumble = true;
    info.pnp_path = kXboxPhysicalPath;
    info.display_name = kXboxName;
    return info;
}

// The keyboard's second HID interface, which GameInput lists as a generic controller.
dal::GamepadDeviceInfo keyboardInterface(std::uint64_t id) {
    dal::GamepadDeviceInfo info;
    info.id = id;
    info.vendor_id = 0x3434;
    info.product_id = 0xd030;
    info.kinds = dal::kGamepadKindController;
    return info;
}

dal::GamepadDeviceInfo steamVirtualPad(std::uint64_t id, std::string path) {
    dal::GamepadDeviceInfo info;
    info.id = id;
    info.vendor_id = 0x045e;
    info.product_id = 0x028e;
    info.kinds = dal::kGamepadKindGamepad | dal::kGamepadKindController;
    info.rumble = true;
    info.pnp_path = std::move(path);
    return info;
}

void testSteamVirtualPath() {
    const std::string path = R"(\\.\pipe\HID#VID_045E&PID_028E&IG_00#45e&b13&1f4a#2#4321)";
    const auto pad = dal::parseSteamVirtualPath(path);
    CHECK(pad.has_value());
    if (pad) {
        CHECK(pad->real_vendor_id == 0x045e);
        CHECK(pad->real_product_id == 0x0b13);
        CHECK(pad->slot == 2);
        CHECK(pad->process_id == 4321);
    }

    // Four-digit hex ids, lower case, and no process id.
    const auto lower = dal::parseSteamVirtualPath(R"(\\.\pipe\hid#vid_045e&pid_028e&ig_00#054c&05c4&abcdef#0)");
    CHECK(lower.has_value());
    if (lower) {
        CHECK(lower->real_vendor_id == 0x054c);
        CHECK(lower->real_product_id == 0x05c4);
        CHECK(lower->slot == 0);
        CHECK(lower->process_id == 0);
    }

    // Physical paths are not virtual pads, whatever their VID/PID.
    CHECK(!dal::parseSteamVirtualPath(kXboxPhysicalPath));
    CHECK(!dal::parseSteamVirtualPath(
            R"(\\?\HID#VID_045E&PID_028E&IG_00#7&2a3b4c5d&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030})"));
    CHECK(!dal::parseSteamVirtualPath(R"(\\.\pipe\HID#VID_054C&PID_05C4#45e&b13&1f4a#2#4321)"));
    CHECK(!dal::parseSteamVirtualPath(""));

    // Truncated or malformed.
    CHECK(!dal::parseSteamVirtualPath(R"(\\.\pipe\HID#VID_045E&PID_028E&IG_00)"));
    CHECK(!dal::parseSteamVirtualPath(R"(\\.\pipe\HID#VID_045E&PID_028E&IG_00#)"));
    CHECK(!dal::parseSteamVirtualPath(R"(\\.\pipe\HID#VID_045E&PID_028E&IG_00#45e&b13&1f4a)"));
    CHECK(!dal::parseSteamVirtualPath(R"(\\.\pipe\HID#VID_045E&PID_028E&IG_00#45e&b13&1f4a#)"));
    CHECK(!dal::parseSteamVirtualPath(R"(\\.\pipe\HID#VID_045E&PID_028E&IG_00#45e&b13#2#4321)"));
    CHECK(!dal::parseSteamVirtualPath(R"(\\.\pipe\HID#VID_045E&PID_028E&IG_00#45e&b13&1f4a#two#4321)"));
    CHECK(!dal::parseSteamVirtualPath(R"(\\.\pipe\HID#VID_045E&PID_028E&IG_00#45e&b13&1f4a#2#pid)"));
    CHECK(!dal::parseSteamVirtualPath(R"(\\.\pipe\HID#VID_045E&PID_028E&IG_00#12345&b13&1f4a#2#4321)"));
    CHECK(!dal::parseSteamVirtualPath(R"(\\.\pipe\HID#VID_045E&PID_028E&IG_00#xyz&b13&1f4a#2#4321)"));
}

void testParseIgnoreDevices() {
    using Id = std::pair<std::uint16_t, std::uint16_t>;
    using Ids = std::vector<Id>;
    const Ids recorded = dal::parseIgnoreDevices(kRecordedIgnoreDevices);
    CHECK(recorded.size() == 6);
    CHECK(recorded.front() == Id(0x28de, 0x1102));
    CHECK(recorded.back() == Id(0x0000, 0x11fb));
    CHECK(std::ranges::none_of(recorded, [](const auto& id) { return id.first == 0x045e; }));

    // Malformed entries are skipped; the rest is kept, spaces and capitals are tolerated.
    const Ids mixed =
            dal::parseIgnoreDevices("0x28DE/0x1102, 0x045e/*,junk,0x12345/0x1,0x1/,/0x1,0x054c/0x05c4,,0xzz/0x1");
    CHECK(mixed.size() == 2);
    if (mixed.size() == 2) {
        CHECK(mixed[0] == Id(0x28de, 0x1102));
        CHECK(mixed[1] == Id(0x054c, 0x05c4));
    }
    CHECK(dal::parseIgnoreDevices("").empty());
    CHECK(dal::parseIgnoreDevices("not a list").empty());
    CHECK(dal::parseIgnoreDevices("28de/1102").empty());
}

// Environment of the recorded Steam shortcut runs.
dal::SteamContext recordedSteamContext() {
    dal::SteamContext steam;
    steam.overlay_loaded = true;
    steam.process_id = 4321;
    steam.env["EnableConfiguratorSupport"] = "4096";
    steam.env["SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD"] = "1";
    steam.env["SDL_GAMECONTROLLER_IGNORE_DEVICES"] = kRecordedIgnoreDevices;
    steam.env["SteamAppId"] = "2510826688";
    steam.env["SteamGameId"] = "10783918510917550080";
    steam.env["SteamOverlayGameId"] = "10783918510917550080";
    steam.env["SteamVirtualGamepadInfo"] = "";
    return steam;
}

// Recorded runs with the Xbox pad only (plain, and as a Steam shortcut with Steam Input on and off):
// each gives one used pad and the keyboard interface as NotGamepad, in either arrival order (the
// Bluetooth pad settles late).
void testSelectRecordedRuns() {
    struct Run {
        const char* name;
        dal::SteamContext steam;
    };
    const Run runs[] = {
            {"plain, no Steam", {}},
            {"Steam shortcut, per-game Steam Input on", recordedSteamContext()},
            {"Steam shortcut, per-game Steam Input off", recordedSteamContext()},
    };
    for (const Run& run : runs) {
        for (const bool pad_first : {true, false}) {
            std::vector<dal::GamepadDeviceInfo> devices;
            if (pad_first)
                devices = {xboxPad(1), keyboardInterface(2)};
            else
                devices = {keyboardInterface(1), xboxPad(2)};

            const auto selection = dal::selectGamepads(devices, run.steam);
            CHECK(selection.size() == 2);
            if (selection.size() != 2) continue;
            const std::size_t pad = pad_first ? 0 : 1;
            const std::size_t keyboard = 1 - pad;
            CHECK(selection[pad].id == devices[pad].id);
            CHECK(selection[pad].use == dal::GamepadUse::Used);
            CHECK(!selection[pad].steam);
            CHECK(selection[keyboard].id == devices[keyboard].id);
            CHECK(selection[keyboard].use == dal::GamepadUse::NotGamepad);
            const auto used = std::ranges::count(selection, dal::GamepadUse::Used, &dal::GamepadSelection::use);
            CHECK(used == 1);
            if (used != 1) std::fprintf(stderr, "run: %s\n", run.name);
        }
    }
}

void testSelectSteamVirtualPad() {
    const std::string own_path = R"(\\.\pipe\HID#VID_045E&PID_028E&IG_00#45e&b13&1f4a#1#4321)";
    const std::string other_path = R"(\\.\pipe\HID#VID_045E&PID_028E&IG_00#45e&b13&1f4a#1#999)";
    const std::string no_pid_path = R"(\\.\pipe\HID#VID_045E&PID_028E&IG_00#45e&b13&1f4a#3)";
    dal::SteamContext steam;
    steam.overlay_loaded = true;
    steam.process_id = 4321;

    // The virtual pad of this process is used and its physical pad is dropped.
    {
        const std::vector<dal::GamepadDeviceInfo> devices = {xboxPad(1), steamVirtualPad(2, own_path),
                                                             keyboardInterface(3)};
        const auto selection = dal::selectGamepads(devices, steam);
        CHECK(selection.size() == 3);
        if (selection.size() == 3) {
            CHECK(selection[0].use == dal::GamepadUse::HandledBySteam);
            CHECK(selection[0].steam && selection[0].steam->slot == 1);
            CHECK(selection[1].use == dal::GamepadUse::Used);
            CHECK(selection[1].steam && selection[1].steam->process_id == 4321);
            CHECK(selection[2].use == dal::GamepadUse::NotGamepad);
        }
    }
    // A virtual pad of another process is not read; a path without a process id is.
    {
        const std::vector<dal::GamepadDeviceInfo> devices = {steamVirtualPad(1, other_path),
                                                             steamVirtualPad(2, no_pid_path)};
        const auto selection = dal::selectGamepads(devices, steam);
        CHECK(selection.size() == 2);
        if (selection.size() == 2) {
            CHECK(selection[0].use == dal::GamepadUse::OtherProcess);
            CHECK(selection[1].use == dal::GamepadUse::Used);
            CHECK(selection[1].steam && selection[1].steam->slot == 3);
        }
    }
    // The virtual pad stands for a physical pad of another model than the Xbox pad: that one stays used.
    {
        dal::GamepadDeviceInfo ds4 = xboxPad(1);
        ds4.vendor_id = 0x054c;
        ds4.product_id = 0x05c4;
        const std::vector<dal::GamepadDeviceInfo> devices = {ds4, xboxPad(2), steamVirtualPad(3, own_path)};
        const auto selection = dal::selectGamepads(devices, steam);
        CHECK(selection.size() == 3);
        if (selection.size() == 3) {
            CHECK(selection[0].use == dal::GamepadUse::Used);
            CHECK(selection[1].use == dal::GamepadUse::HandledBySteam);
            CHECK(selection[2].use == dal::GamepadUse::Used);
        }
    }
}

void testSelectIgnoreList() {
    dal::GamepadDeviceInfo ds4 = xboxPad(1);
    ds4.vendor_id = 0x054c;
    ds4.product_id = 0x05c4;
    const std::vector<dal::GamepadDeviceInfo> devices = {ds4, xboxPad(2)};

    // Steam lists the DS4 as handled: with the overlay loaded the physical DS4 is dropped.
    dal::SteamContext steam = recordedSteamContext();
    steam.env["SDL_GAMECONTROLLER_IGNORE_DEVICES"] = std::string(kRecordedIgnoreDevices) + ",0x054c/0x05c4";
    auto selection = dal::selectGamepads(devices, steam);
    CHECK(selection.size() == 2);
    if (selection.size() == 2) {
        CHECK(selection[0].use == dal::GamepadUse::HandledBySteam);
        CHECK(!selection[0].steam);
        CHECK(selection[1].use == dal::GamepadUse::Used);
    }

    // The variable alone, without the overlay, is not a Steam marker.
    steam.overlay_loaded = false;
    selection = dal::selectGamepads(devices, steam);
    CHECK(selection.size() == 2);
    if (selection.size() == 2) {
        CHECK(selection[0].use == dal::GamepadUse::Used);
        CHECK(selection[1].use == dal::GamepadUse::Used);
    }
}

void testSelectNoSteam() {
    dal::GamepadDeviceInfo ds4 = xboxPad(2);
    ds4.vendor_id = 0x054c;
    ds4.product_id = 0x05c4;
    const std::vector<dal::GamepadDeviceInfo> devices = {xboxPad(1), ds4, keyboardInterface(3), xboxPad(4)};
    const auto selection = dal::selectGamepads(devices, dal::SteamContext{});
    CHECK(selection.size() == 4);
    if (selection.size() == 4) {
        CHECK(selection[0].use == dal::GamepadUse::Used);
        CHECK(selection[1].use == dal::GamepadUse::Used);
        CHECK(selection[2].use == dal::GamepadUse::NotGamepad);
        CHECK(selection[3].use == dal::GamepadUse::Used);
        CHECK(selection[3].id == 4);
    }
    CHECK(dal::selectGamepads(std::span<const dal::GamepadDeviceInfo>{}, dal::SteamContext{}).empty());
}

void testGamepadUseName() {
    CHECK(dal::gamepadUseName(dal::GamepadUse::Used) == "used");
    CHECK(dal::gamepadUseName(dal::GamepadUse::HandledBySteam) == "handled by steam");
    CHECK(dal::gamepadUseName(dal::GamepadUse::OtherProcess) == "other process");
    CHECK(dal::gamepadUseName(dal::GamepadUse::NotGamepad) == "not a gamepad");
}

using Clock = dal::KeyboardPadState::Clock;
using std::chrono::milliseconds;

constexpr dal::PadButtons kCross = dal::padButtonBit(dal::PadButton::Cross);
constexpr dal::PadButtons kCircle = dal::padButtonBit(dal::PadButton::Circle);

// Debug lines (focus changes) are dropped at the default level.
class DebugLogLevel {
public:
    DebugLogLevel() { dal::setLogLevel(dal::LogLevel::Debug); }
    ~DebugLogLevel() { dal::setLogLevel(dal::LogLevel::Info); }
    DebugLogLevel(const DebugLogLevel&) = delete;
    DebugLogLevel& operator=(const DebugLogLevel&) = delete;
};

dal::GamepadReading readingWith(dal::GamepadButton button) {
    dal::GamepadReading reading;
    reading.buttons = gamepadButtonBit(button);
    return reading;
}

void testHubFirstStepUsesPad() {
    LogCapture log;
    auto backend = std::make_shared<FakeBackend>();
    const std::uint64_t id = backend->add(xboxPad(0));
    auto state = std::make_shared<dal::KeyboardPadState>();
    dal::GamepadHub hub(backend, state, {});
    const Clock::time_point t0 = Clock::now();

    CHECK(!state->gamepadConnected());
    hub.step(t0);
    CHECK(hub.usedCount() == 1);
    CHECK(state->gamepadConnected());
    CHECK(log.count("dal: pad: gamepad 045e:0b13 used") == 1);
    CHECK(state->reported(t0) == 0);

    backend->setReading(id, readingWith(GamepadButton::A));
    hub.step(t0 + milliseconds(10));
    CHECK(state->held(dal::InputSource::Gamepad) == kCross);
    CHECK(state->reported(t0 + milliseconds(10)) == kCross);

    // Sticks and triggers reach the analog snapshot; a trigger past the threshold presses L2.
    dal::GamepadReading reading;
    reading.left_x = 1.0f;
    reading.right_y = -1.0f;
    reading.left_trigger = 1.0f;
    backend->setReading(id, reading);
    hub.step(t0 + milliseconds(20));
    CHECK(state->analog().left_x == 0xFF);
    CHECK(state->analog().right_y == 0xFF);  // GameInput +y is up, CellPadData 0xFF is down
    CHECK(state->analog().l2 == 0xFF);
    CHECK(state->held(dal::InputSource::Gamepad) == dal::padButtonBit(dal::PadButton::L2));

    // Steps without a change publish nothing and log nothing.
    hub.step(t0 + milliseconds(30));
    CHECK(log.lines().size() == 1);
    // A keyboard-free state: the other sources are untouched.
    CHECK(state->held(dal::InputSource::Keyboard) == 0 && state->held(dal::InputSource::Script) == 0);
}

void testHubPadRemoved() {
    auto backend = std::make_shared<FakeBackend>();
    const std::uint64_t id = backend->add(xboxPad(0));
    auto state = std::make_shared<dal::KeyboardPadState>();
    dal::GamepadHub hub(backend, state, {});
    const Clock::time_point t0 = Clock::now();

    dal::GamepadReading reading = readingWith(GamepadButton::A);
    reading.left_x = 1.0f;
    backend->setReading(id, reading);
    hub.step(t0);
    CHECK(state->reported(t0) == kCross);

    backend->remove(id);
    hub.step(t0 + milliseconds(20));
    CHECK(hub.usedCount() == 0);
    CHECK(!state->gamepadConnected());
    CHECK(state->held(dal::InputSource::Gamepad) == 0);
    CHECK(state->analog() == dal::PadAnalog{});
    // Released like a button let go: the 60 ms minimum visible time still applies.
    CHECK(state->reported(t0 + milliseconds(20)) == kCross);
    CHECK(state->reported(t0 + milliseconds(59)) == kCross);
    CHECK(state->reported(t0 + milliseconds(61)) == 0);
}

void testHubMergesTwoPads() {
    auto backend = std::make_shared<FakeBackend>();
    const std::uint64_t first = backend->add(xboxPad(0));
    const std::uint64_t second = backend->add(xboxPad(0));
    auto state = std::make_shared<dal::KeyboardPadState>();
    dal::GamepadHub hub(backend, state, {});

    dal::GamepadReading a = readingWith(GamepadButton::A);
    a.left_x = 0.5f;
    a.right_trigger = 0.5f;
    dal::GamepadReading b = readingWith(GamepadButton::B);
    b.left_x = -1.0f;
    b.right_trigger = 1.0f;
    backend->setReading(first, a);
    backend->setReading(second, b);
    hub.step(Clock::now());

    CHECK(hub.usedCount() == 2);
    CHECK((state->held(dal::InputSource::Gamepad) & (kCross | kCircle)) == (kCross | kCircle));
    CHECK(state->analog().left_x == 0x00);  // the value farthest from centre
    CHECK(state->analog().r2 == 0xFF);      // the larger trigger
}

void testHubSteamVirtualPadOnly() {
    LogCapture log;
    const std::string path = R"(\\.\pipe\HID#VID_045E&PID_028E&IG_00#45e&b13&1f4a#1#4321)";
    auto backend = std::make_shared<FakeBackend>();
    const std::uint64_t physical = backend->add(xboxPad(0));
    const std::uint64_t virtual_pad = backend->add(steamVirtualPad(0, path));
    const std::uint64_t keyboard = backend->add(keyboardInterface(0));
    dal::SteamContext steam;
    steam.overlay_loaded = true;
    steam.process_id = 4321;
    auto state = std::make_shared<dal::KeyboardPadState>();
    dal::GamepadHub hub(backend, state, {.steam = steam});

    backend->setReading(physical, readingWith(GamepadButton::B));
    backend->setReading(virtual_pad, readingWith(GamepadButton::A));
    backend->setReading(keyboard, readingWith(GamepadButton::X));
    hub.step(Clock::now());
    hub.step(Clock::now());

    CHECK(hub.usedCount() == 1);
    CHECK(backend->reads(physical) == 0);
    CHECK(backend->reads(keyboard) == 0);
    CHECK(backend->reads(virtual_pad) == 2);
    CHECK(state->held(dal::InputSource::Gamepad) == kCross);
    CHECK(log.count("dal: pad: gamepad 045e:0b13 handled by steam (steam slot 1)") == 1);
    CHECK(log.count("dal: pad: gamepad 045e:028e used (steam slot 1)") == 1);
    CHECK(log.count("dal: pad: gamepad 3434:d030 not a gamepad") == 1);
}

void testHubUseChangeIsLoggedOnce() {
    LogCapture log;
    auto backend = std::make_shared<FakeBackend>();
    backend->add(xboxPad(0));
    auto state = std::make_shared<dal::KeyboardPadState>();
    dal::GamepadHub hub(backend, state, {});

    hub.step(Clock::now());
    backend->add(keyboardInterface(0));
    hub.step(Clock::now());
    hub.step(Clock::now());
    // The first pad is not logged again when only another device arrives.
    CHECK(log.count("dal: pad: gamepad 045e:0b13 used") == 1);
    CHECK(log.count("dal: pad: gamepad 3434:d030 not a gamepad") == 1);
    CHECK(log.lines().size() == 2);
    CHECK(hub.usedCount() == 1);
    CHECK(state->gamepadConnected());
}

void testHubFocusGate() {
    DebugLogLevel debug;
    LogCapture log;
    auto backend = std::make_shared<FakeBackend>();
    const std::uint64_t id = backend->add(xboxPad(0));
    auto state = std::make_shared<dal::KeyboardPadState>();
    bool foreground = true;
    dal::GamepadHub hub(backend, state, {.is_foreground = [&] { return foreground; }});
    const Clock::time_point t0 = Clock::now();

    dal::GamepadReading reading = readingWith(GamepadButton::A);
    reading.left_x = 1.0f;
    backend->setReading(id, reading);
    state->setVibration(0, 200);
    hub.step(t0);
    CHECK(state->reported(t0) == kCross);
    CHECK(state->analog().left_x == 0xFF);
    CHECK(backend->rumbleCalls().size() == 1);
    CHECK(log.count("paused") == 0);

    // Background: released at once (no minimum visible time), analog neutral, rumble stopped once, and
    // no reads while it lasts.
    foreground = false;
    hub.step(t0 + milliseconds(10));
    CHECK(state->held(dal::InputSource::Gamepad) == 0);
    CHECK(state->reported(t0 + milliseconds(10)) == 0);
    CHECK(state->analog() == dal::PadAnalog{});
    auto calls = backend->rumbleCalls();
    CHECK(calls.size() == 2);
    if (calls.size() == 2) CHECK(calls[1].second == dal::GamepadRumble{});
    const int reads_before = backend->reads(id);
    hub.step(t0 + milliseconds(20));
    hub.step(t0 + milliseconds(30));
    CHECK(backend->reads(id) == reads_before);
    CHECK(backend->rumbleCalls().size() == 2);
    CHECK(log.count("dal: pad: gamepad input paused (background)") == 1);
    // The port stays connected while the process is in the background.
    CHECK(state->gamepadConnected());

    // Foreground again: reads resume and the game's current vibration is sent again.
    foreground = true;
    hub.step(t0 + milliseconds(40));
    CHECK(backend->reads(id) == reads_before + 1);
    CHECK(state->held(dal::InputSource::Gamepad) == kCross);
    CHECK(state->analog().left_x == 0xFF);
    calls = backend->rumbleCalls();
    CHECK(calls.size() == 3);
    if (calls.size() == 3) CHECK(calls[2].second.low_frequency == 200.0f / 255.0f);
    CHECK(log.count("dal: pad: gamepad input resumed") == 1);
}

void testHubFirstStepInBackground() {
    DebugLogLevel debug;
    LogCapture log;
    auto backend = std::make_shared<FakeBackend>();
    const std::uint64_t id = backend->add(xboxPad(0));
    backend->setReading(id, readingWith(GamepadButton::A));
    auto state = std::make_shared<dal::KeyboardPadState>();
    bool foreground = false;
    dal::GamepadHub hub(backend, state, {.is_foreground = [&] { return foreground; }});
    const Clock::time_point t0 = Clock::now();

    // The first step only records the focus state: no pause line, no reads, but the pad counts as present.
    hub.step(t0);
    hub.step(t0 + milliseconds(10));
    CHECK(state->gamepadConnected());
    CHECK(backend->reads(id) == 0);
    CHECK(state->held(dal::InputSource::Gamepad) == 0);
    CHECK(log.count("paused") == 0 && log.count("resumed") == 0);

    foreground = true;
    hub.step(t0 + milliseconds(20));
    CHECK(log.count("dal: pad: gamepad input resumed") == 1);
    CHECK(backend->reads(id) == 1);
    CHECK(state->held(dal::InputSource::Gamepad) == kCross);
}

void testHubVibration() {
    auto backend = std::make_shared<FakeBackend>();
    const std::uint64_t pad = backend->add(xboxPad(0));
    dal::GamepadDeviceInfo plain = xboxPad(0);
    plain.rumble = false;
    backend->add(plain);
    auto state = std::make_shared<dal::KeyboardPadState>();
    dal::GamepadHub hub(backend, state, {});
    const Clock::time_point t0 = Clock::now();

    // Nothing requested: nothing sent.
    hub.step(t0);
    CHECK(backend->rumbleCalls().empty());

    state->setVibration(0, 200);
    hub.step(t0 + milliseconds(4));
    auto calls = backend->rumbleCalls();
    CHECK(calls.size() == 1);
    if (calls.size() == 1) {
        CHECK(calls[0].first == pad);
        CHECK(calls[0].second.low_frequency == 200.0f / 255.0f && calls[0].second.high_frequency == 0.0f);
    }
    // Unchanged vibration: no further call, however many steps.
    hub.step(t0 + milliseconds(8));
    hub.step(t0 + milliseconds(12));
    CHECK(backend->rumbleCalls().size() == 1);

    // A pad that arrives later gets the current vibration, and the first pad is not told again.
    const std::uint64_t late = backend->add(xboxPad(0));
    hub.step(t0 + milliseconds(16));
    calls = backend->rumbleCalls();
    CHECK(calls.size() == 2);
    if (calls.size() == 2) {
        CHECK(calls[1].first == late);
        CHECK(calls[1].second.low_frequency == 200.0f / 255.0f);
    }

    // A new request reaches every pad with motors once.
    state->setVibration(5, 0);
    hub.step(t0 + milliseconds(20));
    hub.step(t0 + milliseconds(24));
    calls = backend->rumbleCalls();
    CHECK(calls.size() == 4);
    if (calls.size() == 4) {
        CHECK(calls[2].second.high_frequency == 1.0f && calls[2].second.low_frequency == 0.0f);
        CHECK(calls[3].second == calls[2].second);
    }

    // Stopping is sent too.
    state->setVibration(0, 0);
    hub.step(t0 + milliseconds(28));
    CHECK(backend->rumbleCalls().size() == 6);
}

// A pad that stays connected but stops being used (Steam's virtual pad arrives) is told to stop.
void testHubRumbleStopsWhenPadIsNoLongerUsed() {
    const std::string path = R"(\\.\pipe\HID#VID_045E&PID_028E&IG_00#45e&b13&1f4a#1#4321)";
    auto backend = std::make_shared<FakeBackend>();
    const std::uint64_t physical = backend->add(xboxPad(0));
    dal::SteamContext steam;
    steam.overlay_loaded = true;
    steam.process_id = 4321;
    auto state = std::make_shared<dal::KeyboardPadState>();
    dal::GamepadHub hub(backend, state, {.steam = steam});
    const Clock::time_point t0 = Clock::now();

    state->setVibration(0, 200);
    hub.step(t0);
    auto calls = backend->rumbleCalls();
    CHECK(calls.size() == 1);
    if (calls.size() == 1) {
        CHECK(calls[0].first == physical);
        CHECK(calls[0].second.low_frequency == 200.0f / 255.0f);
    }

    // The physical pad becomes HandledBySteam: it is stopped once, the virtual pad gets the vibration.
    const std::uint64_t virtual_pad = backend->add(steamVirtualPad(0, path));
    hub.step(t0 + milliseconds(4));
    calls = backend->rumbleCalls();
    CHECK(calls.size() == 3);
    if (calls.size() == 3) {
        CHECK(calls[1].first == physical && calls[1].second == dal::GamepadRumble{});
        CHECK(calls[2].first == virtual_pad && calls[2].second.low_frequency == 200.0f / 255.0f);
    }

    // The virtual pad disappears while nothing vibrates: the physical pad, already at rest, is not
    // told again.
    state->setVibration(0, 0);
    hub.step(t0 + milliseconds(8));
    backend->remove(virtual_pad);
    hub.step(t0 + milliseconds(12));
    calls = backend->rumbleCalls();
    CHECK(calls.size() == 4);  // only the virtual pad's stop
    if (calls.size() == 4) CHECK(calls[3].first == virtual_pad && calls[3].second == dal::GamepadRumble{});
    CHECK(hub.usedCount() == 1);
}

void testHubProviderFollowsPlugging() {
    auto backend = std::make_shared<FakeBackend>();
    auto state = std::make_shared<dal::KeyboardPadState>();
    Clock::time_point now = Clock::now();
    const auto provider = dal::makeKeyboardPadProvider(state, [&] { return now; }, {.always_connected = false});
    dal::GamepadHub hub(backend, state, {});

    hub.step(now);
    CHECK(!provider->poll(0).connected);

    const std::uint64_t id = backend->add(xboxPad(0));
    now += milliseconds(10);
    hub.step(now);
    CHECK(provider->poll(0).connected);

    backend->setReading(id, readingWith(GamepadButton::A));
    now += milliseconds(10);
    hub.step(now);
    CHECK(provider->poll(0).connected);
    CHECK((provider->poll(0).button[3] & (1u << 6)) != 0);  // Cross is bit 6 of word 3

    backend->remove(id);
    now += milliseconds(10);
    hub.step(now);
    CHECK(!provider->poll(0).connected);
}

// A backend that cannot be created is one warning, and the pad stays disconnected.
void testHubThreadUnavailable() {
    LogCapture log;
    auto state = std::make_shared<dal::KeyboardPadState>();
    dal::startGamepadHub(
            []() -> std::expected<std::shared_ptr<dal::GamepadBackend>, std::string> {
                return std::unexpected("no runtime");
            },
            state, {});
    CHECK(log.waitFor("dal: pad: gameinput unavailable: no runtime; gamepads disabled"));
    CHECK(log.lines().size() == 1);
    CHECK(!state->gamepadConnected());
}

// Neither a state nor the probe: nothing to do, and the factory is never called.
void testHubThreadNothingToDo() {
    bool called = false;
    dal::startGamepadHub(
            [&]() -> std::expected<std::shared_ptr<dal::GamepadBackend>, std::string> {
                called = true;
                return std::unexpected("unused");
            },
            nullptr, {});
    CHECK(!called);
}

// The detached threads never end. They are silent once the device list is stable, which later tests
// rely on: they check exact log line counts.
void testHubThreadFeedsPad() {
    LogCapture log;
    auto backend = std::make_shared<FakeBackend>();
    const std::uint64_t id = backend->add(xboxPad(0));
    backend->setReading(id, readingWith(GamepadButton::A));
    auto state = std::make_shared<dal::KeyboardPadState>();
    const auto provider = dal::makeKeyboardPadProvider(state, {}, {.always_connected = false});
    dal::startGamepadHub(
            [backend]() -> std::expected<std::shared_ptr<dal::GamepadBackend>, std::string> { return backend; }, state,
            {});

    const auto wait_for = [&](auto condition) {
        const auto end = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < end && !condition()) std::this_thread::sleep_for(milliseconds(2));
    };
    wait_for([&] { return state->gamepadConnected(); });

    // A guest thread polls under the guest lock while the hub thread keeps writing (the reading flips
    // between pressed and idle); neither may block the other.
    std::atomic<bool> stop{false};
    std::thread guest([&] {
        for (int i = 0; i < 10000 || !stop.load(); ++i) provider->poll(0);
    });
    for (int i = 0; i < 25; ++i) {
        backend->setReading(id, i % 2 == 0 ? dal::GamepadReading{} : readingWith(GamepadButton::A));
        std::this_thread::sleep_for(milliseconds(2));
    }
    stop = true;
    guest.join();

    backend->setReading(id, readingWith(GamepadButton::A));
    wait_for([&] { return state->held(dal::InputSource::Gamepad) == kCross; });
    CHECK(state->gamepadConnected());
    CHECK(state->held(dal::InputSource::Gamepad) == kCross);
    CHECK(provider->poll(0).connected);
}

void testHubThreadStartLinesWithProbe() {
    LogCapture log;
    auto backend = std::make_shared<FakeBackend>();
    backend->add(0x045e, 0x028e);
    auto state = std::make_shared<dal::KeyboardPadState>();
    dal::startGamepadHub(
            [backend]() -> std::expected<std::shared_ptr<dal::GamepadBackend>, std::string> { return backend; }, state,
            {.detect_steam =
                     [] {
                         dal::SteamContext steam;
                         steam.overlay_loaded = true;
                         steam.env["SteamGameId"] = "42";
                         return steam;
                     },
             .probe = true});
    CHECK(log.waitFor("dal: pad: probe 1 045e:028e buttons none"));
    const std::vector<std::string> lines = log.lines();
    CHECK(lines.size() == 5);
    if (lines.size() == 5) {
        CHECK(lines[0].ends_with("dal: pad: gameinput fake"));
        CHECK(lines[1].ends_with("dal: pad: steam overlay loaded; SteamGameId=42"));
        CHECK(lines[2].ends_with("dal: pad: probe SteamVirtualGamepadInfo unset"));
        CHECK(lines[3].ends_with("dal: pad: gamepad 045e:028e used"));
        CHECK(lines[4].ends_with(
                "dal: pad: probe 1 045e:028e buttons none lt 0.00 rt 0.00 ls (0.00,0.00) rs (0.00,0.00)"));
    }
    CHECK(state->gamepadConnected());
}

// Probe only (no state): the same thread logs the probe lines and has no pad to feed.
void testHubThreadProbeOnly() {
    LogCapture log;
    auto backend = std::make_shared<FakeBackend>();
    backend->add(0x045e, 0x028e);
    dal::startGamepadHub(
            [backend]() -> std::expected<std::shared_ptr<dal::GamepadBackend>, std::string> { return backend; },
            nullptr, {.probe = true});
    CHECK(log.waitFor("dal: pad: probe 1 045e:028e buttons none"));
    CHECK(log.lines().size() == 4);
    CHECK(log.count("dal: pad: gamepad ") == 0);
}

}  // namespace

int main() {
    testFormatValue();
    testFormatReading();
    testFormatDeviceParts();
    testProbeLogsChangesOnly();
    testProbeReportsMissingReadingOnce();
    testProbeFollowsDevices();
    testSteamVirtualGamepadInfoLog();
    testMapButtonTable();
    testMapIdleReading();
    testStickByte();
    testMapSticks();
    testMapDiagonalClamped();
    testMapTriggers();
    testMergeMappedPads();
    testRumbleFromCellPad();
    testSteamVirtualPath();
    testParseIgnoreDevices();
    testSelectRecordedRuns();
    testSelectSteamVirtualPad();
    testSelectIgnoreList();
    testSelectNoSteam();
    testGamepadUseName();
    testHubFirstStepUsesPad();
    testHubPadRemoved();
    testHubMergesTwoPads();
    testHubSteamVirtualPadOnly();
    testHubUseChangeIsLoggedOnce();
    testHubFocusGate();
    testHubFirstStepInBackground();
    testHubVibration();
    testHubRumbleStopsWhenPadIsNoLongerUsed();
    testHubProviderFollowsPlugging();
    testHubThreadUnavailable();
    testHubThreadNothingToDo();
    testHubThreadFeedsPad();
    testHubThreadStartLinesWithProbe();
    testHubThreadProbeOnly();

    std::printf("input core tests: %d passed, %d failed\n", g_passed, g_failed);
    std::fflush(stdout);
    // Detached probe threads still run; leave without static destruction, as the game does.
    std::_Exit(g_failed == 0 ? 0 : 1);
}
