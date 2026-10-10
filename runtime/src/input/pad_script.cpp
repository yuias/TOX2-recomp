#include "input/pad_script.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <thread>
#include <utility>

#include "core/log.h"

namespace tox2 {
namespace {

// Bounds keep a typo such as `every 0.2 until 99999999` from allocating without limit.
constexpr std::int64_t kMaxSeconds = 1'000'000;
constexpr std::size_t kMaxEvents = 100'000;

// Decimal seconds ("40", "40.5") to milliseconds without going through floating point; digits
// beyond the third decimal round half up.
std::optional<std::chrono::milliseconds> parseSeconds(std::string_view text) {
    const std::size_t dot = text.find('.');
    const std::string_view whole = text.substr(0, dot);
    const std::string_view fraction = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    const auto all_digits = [](std::string_view s) {
        return std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
    };
    if (whole.empty() || !all_digits(whole) || !all_digits(fraction)) return std::nullopt;
    if (dot != std::string_view::npos && fraction.empty()) return std::nullopt;

    std::int64_t seconds = 0;
    for (const char c : whole) {
        seconds = seconds * 10 + (c - '0');
        if (seconds > kMaxSeconds) return std::nullopt;
    }
    std::int64_t millis = 0;
    for (std::size_t i = 0; i < 3; i++) millis = millis * 10 + (i < fraction.size() ? fraction[i] - '0' : 0);
    if (fraction.size() > 3 && fraction[3] >= '5') millis++;
    return std::chrono::milliseconds(seconds * 1000 + millis);
}

std::vector<std::string_view> splitTokens(std::string_view line) {
    std::vector<std::string_view> tokens;
    std::size_t pos = 0;
    while (pos < line.size()) {
        const std::size_t begin = line.find_first_not_of(" \t", pos);
        if (begin == std::string_view::npos) break;
        const std::size_t end = std::min(line.find_first_of(" \t", begin), line.size());
        tokens.push_back(line.substr(begin, end - begin));
        pos = end;
    }
    return tokens;
}

std::expected<PadButtons, std::string> parseButtons(std::string_view text) {
    PadButtons buttons = 0;
    std::size_t pos = 0;
    while (true) {
        const std::size_t plus = text.find('+', pos);
        const std::string_view name = text.substr(pos, plus == std::string_view::npos ? plus : plus - pos);
        const std::optional<PadButton> button = padButtonFromName(name);
        if (!button) return std::unexpected(std::format("unknown button \"{}\"", name));
        buttons |= padButtonBit(*button);
        if (plus == std::string_view::npos) return buttons;
        pos = plus + 1;
    }
}

// Parses one non-empty line into events; returns the error text without the line number.
std::optional<std::string> parseLine(const std::vector<std::string_view>& tokens, std::vector<PadScriptEvent>& events) {
    const std::optional<std::chrono::milliseconds> at = parseSeconds(tokens[0]);
    if (!at) return std::format("bad time \"{}\"", tokens[0]);
    if (tokens.size() < 2) return "missing command";
    const std::string_view command = tokens[1];
    if (command != "press" && command != "release" && command != "tap") {
        return std::format("unknown command \"{}\"", command);
    }
    if (tokens.size() < 3) return std::format("\"{}\" needs buttons", command);
    const std::expected<PadButtons, std::string> buttons = parseButtons(tokens[2]);
    if (!buttons) return buttons.error();

    if (command != "tap") {
        if (tokens.size() > 3) return std::format("unexpected \"{}\"", tokens[3]);
        events.push_back({*at, *buttons, command == "press"});
        return std::nullopt;
    }

    std::optional<std::chrono::milliseconds> interval;
    std::chrono::milliseconds until = *at;
    if (tokens.size() > 3) {
        if (tokens[3] != "every") return std::format("unexpected \"{}\"", tokens[3]);
        if (tokens.size() < 5) return "\"every\" needs an interval";
        interval = parseSeconds(tokens[4]);
        if (!interval) return std::format("bad interval \"{}\"", tokens[4]);
        if (*interval <= kPadScriptTapHold) {
            return std::format("interval must be longer than the {} ms tap hold", kPadScriptTapHold.count());
        }
        if (tokens.size() < 6 || tokens[5] != "until") return "\"every\" needs \"until <time>\"";
        if (tokens.size() < 7) return "\"until\" needs a time";
        const std::optional<std::chrono::milliseconds> end = parseSeconds(tokens[6]);
        if (!end) return std::format("bad time \"{}\"", tokens[6]);
        if (*end < *at) return "\"until\" is before the first tap";
        if (tokens.size() > 7) return std::format("unexpected \"{}\"", tokens[7]);
        until = *end;
    }

    // Without `every` the step only has to move past `until` (== `at`) to end the loop.
    const std::chrono::milliseconds step = interval.value_or(std::chrono::milliseconds(1));
    for (std::chrono::milliseconds t = *at; t <= until; t += step) {
        if (events.size() + 2 > kMaxEvents) return std::format("more than {} events", kMaxEvents);
        events.push_back({t, *buttons, true});
        events.push_back({t + kPadScriptTapHold, *buttons, false});
    }
    return std::nullopt;
}

std::string joinButtonNames(PadButtons buttons) {
    std::string names;
    for (std::size_t i = 0; i < kPadButtonTotal; i++) {
        const auto button = static_cast<PadButton>(i);
        if ((buttons & padButtonBit(button)) == 0) continue;
        if (!names.empty()) names += '+';
        names += padButtonName(button);
    }
    return names;
}

}  // namespace

std::expected<std::vector<PadScriptEvent>, std::string> parsePadScript(std::string_view text) {
    std::vector<PadScriptEvent> events;
    std::size_t line_number = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t newline = text.find('\n', pos);
        std::string_view line = text.substr(pos, newline == std::string_view::npos ? newline : newline - pos);
        pos = newline == std::string_view::npos ? text.size() + 1 : newline + 1;
        line_number++;

        line = line.substr(0, line.find('#'));
        const std::vector<std::string_view> tokens = splitTokens(line);
        if (tokens.empty()) continue;
        if (const std::optional<std::string> error = parseLine(tokens, events)) {
            return std::unexpected(std::format("line {}: {}", line_number, *error));
        }
    }
    // Releases first at equal times, so a release and a press of the same button at one instant
    // read as a re-press rather than a no-op.
    std::ranges::stable_sort(events, [](const PadScriptEvent& a, const PadScriptEvent& b) {
        if (a.at != b.at) return a.at < b.at;
        return !a.down && b.down;
    });
    return events;
}

std::expected<std::vector<PadScriptEvent>, std::string> loadPadScript(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::unexpected(std::format("cannot open {}", file.string()));
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return std::unexpected(std::format("cannot read {}", file.string()));
    return parsePadScript(text);
}

void playPadScript(std::span<const PadScriptEvent> events, KeyboardPadState& state,
                   const std::function<void(std::chrono::milliseconds)>& wait_until) {
    for (const PadScriptEvent& event : events) {
        wait_until(event.at);
        state.setButtons(InputSource::Script, event.buttons, event.down, KeyboardPadState::Clock::now());
        const std::int64_t ms = event.at.count();
        logInfo("tox2: pad: script {}.{:03} s: {} {}", ms / 1000, ms % 1000, event.down ? "press" : "release",
                joinButtonNames(event.buttons));
    }
}

void startPadScript(std::vector<PadScriptEvent> events, std::shared_ptr<KeyboardPadState> state) {
    if (events.empty() || !state) return;
    const KeyboardPadState::Clock::time_point origin = KeyboardPadState::Clock::now();
    std::thread([origin, events = std::move(events), state = std::move(state)] {
        playPadScript(events, *state,
                      [origin](std::chrono::milliseconds at) { std::this_thread::sleep_until(origin + at); });
    }).detach();
}

}  // namespace tox2
