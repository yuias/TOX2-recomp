// dal_padtool: gamepad utilities for the runtime. Subcommands:
//   devices                         the GameInput source, the Steam context and every game controller
//   watch [--seconds N] [--raw]     print a line per change of every device's reading (default 10 s)
//   rumble [--seconds N]            run each rumble motor of every pad for N seconds each (default 1)
// Reads in the background too (the console window belongs to another process).
// Exits 0 on success, 1 when GameInput is unavailable, 2 on a usage error.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/sleeper.h"
#include "input/gameinput_backend.h"
#include "input/gamepad_probe.h"

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::chrono::milliseconds kWatchInterval{10};
constexpr double kDefaultWatchSeconds = 10.0;
constexpr double kDefaultRumbleSeconds = 1.0;

struct Subcommand {
    std::string_view name;
    int (*run)(std::span<const std::string_view> args);
};

void printUsage() {
    std::fputs(
            "usage: dal_padtool devices\n"
            "       dal_padtool watch [--seconds N] [--raw]\n"
            "       dal_padtool rumble [--seconds N]\n"
            "  devices   the GameInput source, the Steam context and one line per game controller:\n"
            "            id vid:pid kinds rumble yes|no \"name\" path\n"
            "  watch     poll every device every 10 ms and print a line per change (default 10 s);\n"
            "            sticks and triggers are rounded to 0.05; --raw also prints the generic\n"
            "            controller layout (button indices, axes, switches) of controller-kind devices\n"
            "  rumble    for each pad with rumble: low frequency motor for N s, high frequency motor for N s\n"
            "            (default 1 s each), then stop\n",
            stderr);
}

void printLine(const std::string& line) {
    std::fputs(line.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

bool parsePositive(std::string_view text, double& value) {
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    return ec == std::errc{} && ptr == end && value > 0 && std::isfinite(value);
}

// Shared by watch and rumble. Returns false after printing the usage error.
bool parseSeconds(std::span<const std::string_view> args, double default_seconds, bool allow_raw, double& seconds,
                  bool& raw) {
    seconds = default_seconds;
    raw = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--seconds" && i + 1 < args.size() && parsePositive(args[i + 1], seconds)) {
            ++i;
        } else if (allow_raw && args[i] == "--raw") {
            raw = true;
        } else {
            std::fprintf(stderr, "dal_padtool: bad argument '%.*s'\n", static_cast<int>(args[i].size()),
                         args[i].data());
            printUsage();
            return false;
        }
    }
    return true;
}

std::string idText(const dal::GamepadDeviceInfo& info) {
    return std::format("{} {}", info.id, dal::formatGamepadVidPid(info));
}

std::string deviceLine(const dal::GamepadDeviceInfo& info) {
    return std::format("{} {} rumble {} \"{}\" {}", idText(info), dal::formatGamepadKinds(info.kinds),
                       info.rumble ? "yes" : "no", info.display_name, info.pnp_path);
}

// The runtime reports some devices a few tens of milliseconds after its blocking enumeration has
// returned, so one-shot commands wait until the device list has been quiet for a moment.
void waitForDevices(dal::GamepadBackend& backend) {
    constexpr auto kQuiet = 250ms;
    constexpr auto kLimit = 2s;
    dal::Sleeper sleeper;
    const Clock::time_point start = Clock::now();
    std::uint64_t generation = backend.deviceGeneration();
    Clock::time_point changed = start;
    while (Clock::now() - changed < kQuiet && Clock::now() - start < kLimit) {
        sleeper.sleep(10ms);
        if (const std::uint64_t now = backend.deviceGeneration(); now != generation) {
            generation = now;
            changed = Clock::now();
        }
    }
}

// Prints the reason on stderr and returns null when GameInput is unavailable (the callers exit 1).
std::shared_ptr<dal::GameInputBackend> openBackend(bool settle) {
    auto backend = dal::makeGameInputBackend({.background_input = true});
    if (!backend) {
        std::fprintf(stderr, "dal_padtool: GameInput unavailable: %s\n", backend.error().c_str());
        return nullptr;
    }
    if (settle) waitForDevices(**backend);
    return *backend;
}

std::string rawText(const dal::GameInputRawReading& r) {
    std::string buttons;
    for (std::size_t i = 0; i < r.buttons.size(); ++i) {
        if (r.buttons[i] != 0) buttons += std::format("{}{}", buttons.empty() ? "" : ",", i);
    }
    std::string axes;
    for (std::size_t i = 0; i < r.axes.size(); ++i) {
        axes += std::format("{}{}:{}", axes.empty() ? "" : " ", i, dal::formatGamepadValue(r.axes[i]));
    }
    std::string switches;
    for (std::size_t i = 0; i < r.switches.size(); ++i) {
        switches += std::format("{}{}:{}", switches.empty() ? "" : " ", i, r.switches[i]);
    }
    return std::format("raw buttons [{}] axes [{}] switches [{}]", buttons, axes, switches);
}

double secondsSince(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

int runDevices(std::span<const std::string_view> args) {
    if (!args.empty()) {
        printUsage();
        return 2;
    }
    const auto backend = openBackend(true);
    if (!backend) return 1;
    printLine(std::format("gameinput {}", backend->description()));
    const dal::SteamContext steam = dal::detectSteamContext();
    printLine(std::format("steam overlay {}", steam.overlay_loaded ? "loaded" : "not loaded"));
    for (const auto& [name, value] : steam.env) printLine(std::format("steam {}={}", name, value));
    const std::vector<dal::GamepadDeviceInfo> devices = backend->devices();
    for (const dal::GamepadDeviceInfo& info : devices) printLine(deviceLine(info));
    printLine(std::format("{} devices", devices.size()));
    return 0;
}

int runWatch(std::span<const std::string_view> args) {
    double seconds = 0;
    bool raw = false;
    if (!parseSeconds(args, kDefaultWatchSeconds, true, seconds, raw)) return 2;
    const auto backend = openBackend(false);
    if (!backend) return 1;

    const Clock::time_point start = Clock::now();
    const auto stamp = [&] { return std::format("{:.3f} s", secondsSince(start)); };
    std::map<std::uint64_t, std::string> last_gamepad;  // last printed text per device
    std::map<std::uint64_t, std::string> last_raw;
    std::map<std::uint64_t, dal::GamepadDeviceInfo> known;
    std::optional<std::uint64_t> generation;
    dal::Sleeper sleeper;
    std::vector<dal::GamepadDeviceInfo> devices;

    while (secondsSince(start) < seconds) {
        if (!generation || backend->deviceGeneration() != *generation) {
            generation = backend->deviceGeneration();
            devices = backend->devices();
            std::map<std::uint64_t, dal::GamepadDeviceInfo> now_known;
            for (const dal::GamepadDeviceInfo& info : devices) {
                now_known.emplace(info.id, info);
                if (!known.contains(info.id)) printLine(std::format("{} device + {}", stamp(), deviceLine(info)));
            }
            for (const auto& [id, info] : known) {
                if (now_known.contains(id)) continue;
                printLine(std::format("{} device - {}", stamp(), idText(info)));
                last_gamepad.erase(id);
                last_raw.erase(id);
            }
            known = std::move(now_known);
        }

        for (const dal::GamepadDeviceInfo& info : devices) {
            if ((info.kinds & dal::kGamepadKindGamepad) != 0) {
                if (const auto reading = backend->read(info.id)) {
                    const std::string text = dal::formatGamepadReading(*reading);
                    std::string& last = last_gamepad[info.id];
                    if (text != last) {
                        last = text;
                        printLine(std::format("{} {} {}", stamp(), idText(info), text));
                    }
                }
            }
            if (raw && (info.kinds & dal::kGamepadKindController) != 0) {
                if (const auto reading = backend->readRaw(info.id)) {
                    const std::string text = rawText(*reading);
                    std::string& last = last_raw[info.id];
                    if (text != last) {
                        last = text;
                        printLine(std::format("{} {} {}", stamp(), idText(info), text));
                    }
                }
            }
        }
        sleeper.sleep(kWatchInterval);
    }
    return 0;
}

int runRumble(std::span<const std::string_view> args) {
    double seconds = 0;
    bool raw = false;
    if (!parseSeconds(args, kDefaultRumbleSeconds, false, seconds, raw)) return 2;
    const auto backend = openBackend(true);
    if (!backend) return 1;

    dal::Sleeper sleeper;
    const auto step = [&](const dal::GamepadDeviceInfo& info, const char* what, const dal::GamepadRumble& rumble) {
        backend->setRumble(info.id, rumble);
        printLine(std::format("{} {}", idText(info), what));
        sleeper.sleep(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::duration<double>(seconds)));
    };

    int count = 0;
    for (const dal::GamepadDeviceInfo& info : backend->devices()) {
        if (!info.rumble) continue;
        ++count;
        step(info, "low frequency motor", {.low_frequency = 1.0f});
        step(info, "high frequency motor", {.high_frequency = 1.0f});
        backend->setRumble(info.id, {});
        printLine(std::format("{} stopped", idText(info)));
    }
    printLine(std::format("{} devices with rumble", count));
    return 0;
}

constexpr Subcommand kSubcommands[] = {
        {"devices", runDevices},
        {"watch", runWatch},
        {"rumble", runRumble},
};

}  // namespace

int main(int argc, char** argv) {
    // The backend's device lines are for the game's log; the tool prints its own device lines.
    dal::setLogLevel(dal::LogLevel::Warn);
    std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.empty()) {
        printUsage();
        return 2;
    }
    for (const Subcommand& command : kSubcommands) {
        if (args[0] == command.name) return command.run(std::span<const std::string_view>(args).subspan(1));
    }
    std::fprintf(stderr, "dal_padtool: unknown subcommand '%.*s'\n", static_cast<int>(args[0].size()), args[0].data());
    printUsage();
    return 2;
}
