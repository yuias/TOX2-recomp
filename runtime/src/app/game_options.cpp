#include "app/game_options.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <format>
#include <system_error>
#include <utility>

#include "audio/audio_output.h"
#include "audio/wav_file.h"
#include "core/fatal.h"
#include "hle/hle.h"

namespace tox2 {
namespace {

constexpr std::string_view kUsage =
        "usage: tox2_game [--game-dir DIR] [--vfs-config FILE] (at least one) [--elf PATH]\n"
        "                [--libsre PATH]\n"
        "                [--seconds N] [--strict] [--log FILE] [--hdd-dir DIR] [--sdata-cache DIR]\n"
        "                [--log-level trace|debug|info|warn|error]\n"
        "                [--quiet-hle] [--load-only] [--png-dump DIR]\n"
        "                [--dialog-answer yes|no|ok] [--pad none|neutral|keyboard|gamepad|keyboard+gamepad]\n"
        "                [--pad-script FILE] [--pad-probe] [--audio none|wasapi] [--audio-dump FILE]\n"
        "                [--render none|vulkan] [--rsx-capture N[,M...]] [--rsx-census FILE]\n"
        "                [--dump-frames DIR] [--dump-frames-every K] [--shader-cache DIR]\n"
        "                [--resolution-scale N] [--aa-mode supersample|msaa] [--vk-validation] [--no-window]\n"
        "                [--frame-limit vsync|60|off] [--host-priorities on|off] [--yield-backoff on|off]\n"
        "                [--ppu-threads legacy|1|2] [--lwmutex-fast-path on|off] [--lwmutex-watch]\n"
        "                [--dump-guest ADDR:SIZE:FILE]...\n";

constexpr std::string_view kRsxCaptureError = "error: --rsx-capture needs frame numbers (1-based), e.g. 300,600\n";

// What the validators settle besides the GameOptions members: facts about which options were given, and the
// text of the values whose messages quote it.
struct ParseState {
    GameOptions options;
    std::optional<bool> render_flag;  // --render as given
    bool dump_every_given = false;
    bool resolution_scale_given = false;
    bool aa_mode_given = false;
    bool pad_given = false;
    bool audio_given = false;
    std::string pad_script_text;
    std::string audio_dump_text;
};

// Checks one command-line occurrence of an option and stores it. On failure sets `error` to the complete message.
using ValidateFn = bool (*)(ParseState& state, std::string_view option, std::string_view text, std::string& error);

struct OptionEntry {
    OptionSpec spec;
    ValidateFn validate;
};

std::string optionLabel(std::string_view option) { return std::format("--{}", option); }

bool fail(std::string& error, std::string message) {
    error = std::move(message);
    return false;
}

template <typename T>
bool parseUnsigned(std::string_view text, T& out) {
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} && end == text.data() + text.size();
}

bool parseFrameList(std::string_view list, std::vector<std::uint32_t>& frames) {
    while (!list.empty()) {
        const std::size_t comma = list.find(',');
        const std::string_view item = list.substr(0, comma);
        std::uint32_t frame = 0;
        const auto [end, ec] = std::from_chars(item.data(), item.data() + item.size(), frame);
        if (item.empty() || ec != std::errc{} || end != item.data() + item.size() || frame == 0) return false;
        frames.push_back(frame);
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
        if (list.empty()) return false;  // trailing comma
    }
    return !frames.empty();
}

std::optional<LogLevel> parseLogLevel(std::string_view name) {
    if (name == "trace") return LogLevel::Trace;
    if (name == "debug") return LogLevel::Debug;
    if (name == "info") return LogLevel::Info;
    if (name == "warn") return LogLevel::Warn;
    if (name == "error") return LogLevel::Error;
    return std::nullopt;
}

bool padUsesKeyboard(std::string_view pad) { return pad == "keyboard" || pad == "keyboard+gamepad"; }
bool padUsesGamepad(std::string_view pad) { return pad == "gamepad" || pad == "keyboard+gamepad"; }

// Validators. Each one's message is the text the option has always printed.

template <std::filesystem::path GameOptions::* Member>
bool setPath(ParseState& state, std::string_view, std::string_view text, std::string&) {
    state.options.*Member = std::filesystem::path(text);
    return true;
}

template <bool GameOptions::* Member>
bool setFlag(ParseState& state, std::string_view, std::string_view, std::string&) {
    state.options.*Member = true;
    return true;
}

template <bool GameOptions::* Member>
bool setSwitch(ParseState& state, std::string_view option, std::string_view text, std::string& error) {
    if (text != "on" && text != "off")
        return fail(error, std::format("error: {} needs on or off\n", optionLabel(option)));
    state.options.*Member = text == "on";
    return true;
}

bool setPadScript(ParseState& state, std::string_view option, std::string_view text, std::string& error) {
    state.pad_script_text = text;
    state.options.pad_script = std::filesystem::path(text);
    auto script = loadPadScript(state.options.pad_script);
    if (!script) {
        return fail(error,
                    std::format("error: {} {}: {}\n", optionLabel(option), state.pad_script_text, script.error()));
    }
    state.options.pad_script_data = std::move(*script);
    return true;
}

bool setAudioDump(ParseState& state, std::string_view, std::string_view text, std::string&) {
    state.audio_dump_text = text;
    state.options.audio_dump = std::filesystem::path(text);
    return true;
}

bool setLogLevelOption(ParseState& state, std::string_view option, std::string_view text, std::string& error) {
    const std::optional<LogLevel> level = parseLogLevel(text);
    if (!level) {
        return fail(error, std::format("error: {} needs trace, debug, info, warn or error\n", optionLabel(option)));
    }
    state.options.log_level = level;
    return true;
}

bool setDialogAnswerOption(ParseState& state, std::string_view option, std::string_view text, std::string& error) {
    if (text != "yes" && text != "no" && text != "ok") {
        return fail(error, std::format("error: {} needs yes, no or ok\n", optionLabel(option)));
    }
    state.options.dialog_answer = text;
    return true;
}

bool setPad(ParseState& state, std::string_view option, std::string_view text, std::string& error) {
    if (text != "none" && text != "neutral" && text != "keyboard" && text != "gamepad" && text != "keyboard+gamepad") {
        return fail(error, std::format("error: {} needs none, neutral, keyboard, gamepad or keyboard+gamepad\n",
                                       optionLabel(option)));
    }
    state.options.pad = text;
    state.pad_given = true;
    return true;
}

bool setAudio(ParseState& state, std::string_view option, std::string_view text, std::string& error) {
    if (text != "none" && text != "wasapi") {
        return fail(error, std::format("error: {} needs none or wasapi\n", optionLabel(option)));
    }
    state.options.audio = text;
    state.audio_given = true;
    return true;
}

bool setRender(ParseState& state, std::string_view option, std::string_view text, std::string& error) {
    if (text != "none" && text != "vulkan") {
        return fail(error, std::format("error: {} needs none or vulkan\n", optionLabel(option)));
    }
    state.render_flag = text == "vulkan";
    return true;
}

bool setPpuThreads(ParseState& state, std::string_view option, std::string_view text, std::string& error) {
    if (text == "legacy") {
        state.options.ppu_threads = PpuThreadsMode::Legacy;
    } else if (text == "1") {
        state.options.ppu_threads = PpuThreadsMode::Split1;
    } else if (text == "2") {
        state.options.ppu_threads = PpuThreadsMode::Split2;
    } else {
        return fail(error, std::format("error: {} needs legacy, 1 or 2\n", optionLabel(option)));
    }
    return true;
}

bool setSeconds(ParseState& state, std::string_view option, std::string_view text, std::string& error) {
    if (!parseUnsigned(text, state.options.seconds)) {
        return fail(error, std::format("error: {} needs a non-negative integer\n", optionLabel(option)));
    }
    return true;
}

bool setDumpFramesEvery(ParseState& state, std::string_view option, std::string_view text, std::string& error) {
    if (!parseUnsigned(text, state.options.dump_frames_every) || state.options.dump_frames_every == 0) {
        return fail(error, std::format("error: {} needs a positive integer\n", optionLabel(option)));
    }
    state.dump_every_given = true;
    return true;
}

bool setRsxCapture(ParseState& state, std::string_view, std::string_view text, std::string& error) {
    // The frames of every occurrence accumulate, and a later list only has to be valid, not non-empty.
    if (!parseFrameList(text, state.options.rsx_capture_frames)) return fail(error, std::string(kRsxCaptureError));
    return true;
}

bool addDumpGuest(ParseState& state, std::string_view option, std::string_view text, std::string& error) {
    std::string parse_error;
    auto spec = parseGuestDumpSpec(text, parse_error);
    if (!spec) return fail(error, std::format("error: {} {}: {}\n", optionLabel(option), text, parse_error));
    state.options.dumps.push_back(std::move(*spec));
    return true;
}

bool setFrameLimit(ParseState& state, std::string_view option, std::string_view text, std::string& error) {
    if (text == "vsync") {
        state.options.frame_limit = RsxFlipLimit::Vsync;
    } else if (text == "60") {
        state.options.frame_limit = RsxFlipLimit::Hz60;
    } else if (text == "off") {
        state.options.frame_limit = RsxFlipLimit::Off;
    } else {
        return fail(error, std::format("error: {} needs vsync, 60 or off\n", optionLabel(option)));
    }
    return true;
}

bool setResolutionScale(ParseState& state, std::string_view option, std::string_view text, std::string& error) {
    if (!parseUnsigned(text, state.options.resolution_scale) || state.options.resolution_scale < 1 ||
        state.options.resolution_scale > kMaxResolutionScale) {
        return fail(error, std::format("error: {} needs a whole number from 1 to {}\n", optionLabel(option),
                                       kMaxResolutionScale));
    }
    state.resolution_scale_given = true;
    return true;
}

bool setAaMode(ParseState& state, std::string_view option, std::string_view text, std::string& error) {
    const std::optional<AaMode> mode = parseAaMode(text);
    if (!mode) return fail(error, std::format("error: {} needs supersample|msaa\n", optionLabel(option)));
    state.options.aa_mode = *mode;
    state.aa_mode_given = true;
    return true;
}

// Value errors are reported in this order. An option that takes a value is any kind but Flag.
constexpr auto kEntries = std::to_array<OptionEntry>({
        {{"game-dir", OptionKind::Path}, setPath<&GameOptions::game_dir>},
        {{"vfs-config", OptionKind::Path}, setPath<&GameOptions::vfs_config>},
        {{"elf", OptionKind::Path}, setPath<&GameOptions::elf>},
        {{"libsre", OptionKind::Path}, setPath<&GameOptions::libsre>},
        {{"hdd-dir", OptionKind::Path}, setPath<&GameOptions::hdd_dir>},
        {{"sdata-cache", OptionKind::Path}, setPath<&GameOptions::sdata_cache>},
        {{"log", OptionKind::Path}, setPath<&GameOptions::log_file>},
        {{"png-dump", OptionKind::Path}, setPath<&GameOptions::png_dump>},
        {{"pad-script", OptionKind::Path}, setPadScript},
        {{"audio-dump", OptionKind::Path}, setAudioDump},
        {{"rsx-census", OptionKind::Path}, setPath<&GameOptions::rsx_census>},
        {{"dump-frames", OptionKind::Path}, setPath<&GameOptions::dump_frames>},
        {{"shader-cache", OptionKind::Path}, setPath<&GameOptions::shader_cache>},
        {{"log-level", OptionKind::String}, setLogLevelOption},
        {{"dialog-answer", OptionKind::String}, setDialogAnswerOption},
        {{"pad", OptionKind::String}, setPad},
        {{"audio", OptionKind::String}, setAudio},
        {{"render", OptionKind::String}, setRender},
        {{"ppu-threads", OptionKind::String}, setPpuThreads},
        {{"seconds", OptionKind::Unsigned}, setSeconds},
        {{"dump-frames-every", OptionKind::Unsigned}, setDumpFramesEvery},
        {{"strict", OptionKind::Flag}, setFlag<&GameOptions::strict>},
        {{"quiet-hle", OptionKind::Flag}, setFlag<&GameOptions::quiet_hle>},
        {{"load-only", OptionKind::Flag}, setFlag<&GameOptions::load_only>},
        {{"pad-probe", OptionKind::Flag}, setFlag<&GameOptions::pad_probe>},
        {{"vk-validation", OptionKind::Flag}, setFlag<&GameOptions::vk_validation>},
        {{"no-window", OptionKind::Flag}, setFlag<&GameOptions::no_window>},
        {{"lwmutex-watch", OptionKind::Flag}, setFlag<&GameOptions::lwmutex_watch>},
        {{"host-priorities", OptionKind::Switch}, setSwitch<&GameOptions::host_priorities>},
        {{"yield-backoff", OptionKind::Switch}, setSwitch<&GameOptions::yield_backoff>},
        {{"lwmutex-fast-path", OptionKind::Switch}, setSwitch<&GameOptions::lwmutex_fast_path>},
        {{"rsx-capture", OptionKind::FrameList}, setRsxCapture},
        {{"dump-guest", OptionKind::StringList}, addDumpGuest},
        {{"frame-limit", OptionKind::String, true, OptionNeeds::Nothing}, setFrameLimit},
        {{"resolution-scale", OptionKind::Unsigned, true, OptionNeeds::Renderer}, setResolutionScale},
        {{"aa-mode", OptionKind::String, true, OptionNeeds::Renderer}, setAaMode},
});

constexpr auto kSpecs = [] {
    std::array<OptionSpec, kEntries.size()> specs{};
    for (std::size_t i = 0; i < kEntries.size(); ++i) specs[i] = kEntries[i].spec;
    return specs;
}();

// The command-line text of every occurrence of each option, indexed like kEntries.
using RawValues = std::array<std::vector<std::string>, kEntries.size()>;

// Phase 1: finds the options and their values in argument order.
std::optional<OptionsError> collectCommandLine(std::span<const std::string> args, RawValues& raw) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        std::size_t index = kEntries.size();
        if (arg.starts_with("--")) {
            for (std::size_t e = 0; e < kEntries.size(); ++e) {
                if (arg.substr(2) == kEntries[e].spec.name) index = e;
            }
        }
        if (index == kEntries.size()) return OptionsError{.message = std::format("error: unknown argument {}\n", arg)};
        const OptionSpec& spec = kEntries[index].spec;
        if (spec.kind == OptionKind::Flag) {
            raw[index].emplace_back();
            continue;
        }
        if (i + 1 >= args.size()) {
            std::string message = std::format("error: {} needs a value\n", arg);
            // The frame list has always reported its format after the missing value as well.
            if (spec.kind == OptionKind::FrameList) message += kRsxCaptureError;
            return OptionsError{.message = std::move(message)};
        }
        raw[index].push_back(args[++i]);
    }
    return std::nullopt;
}

OptionsError usageError(std::string message) {
    return OptionsError{.exit_code = static_cast<int>(ExitCode::Usage), .message = std::move(message)};
}

// Checks that depend on several options, in the order they have always run.
std::optional<OptionsError> settle(ParseState& state) {
    GameOptions& opt = state.options;
    if (opt.game_dir.empty() && opt.vfs_config.empty()) {
        return usageError("error: --game-dir or --vfs-config is required\n");
    }
    // Playing is the default, so the window is on unless the run asks for none: --no-window for
    // unattended runs, or an explicit "--render none", which leaves nothing to show.
    opt.window = !opt.no_window && state.render_flag.value_or(true);
    // --dump-frames and the window imply vulkan; an explicit "--render none" (anywhere on the line)
    // overrides that and then conflicts with it. The other options have no effect without a renderer.
    opt.render_vulkan = state.render_flag.value_or(!opt.dump_frames.empty() || opt.window);
    const bool render_option = !opt.dump_frames.empty() || state.dump_every_given || !opt.shader_cache.empty() ||
                               opt.vk_validation || state.resolution_scale_given || state.aa_mode_given;
    if (render_option && !opt.render_vulkan) {
        return usageError(
                "error: --dump-frames, --dump-frames-every, --shader-cache, --resolution-scale, --aa-mode and "
                "--vk-validation need --render vulkan\n");
    }
    // GameInput delivers readings to the foreground window only, so unattended runs have nothing to probe.
    if (opt.pad_probe && !opt.window) return usageError("error: --pad-probe needs the window\n");
    // A window or a script is what makes a keyboard pad useful, so either one selects it by default.
    const bool pad_input = opt.window || !opt.pad_script.empty();
    if (!state.pad_given) opt.pad = opt.window ? "keyboard+gamepad" : pad_input ? "keyboard" : "neutral";
    if (opt.pad == "keyboard" && !pad_input) {
        return usageError("error: --pad keyboard needs the window or --pad-script\n");
    }
    // GameInput delivers readings to the foreground window only.
    if (padUsesGamepad(opt.pad) && !opt.window) {
        return usageError(std::format("error: --pad {} needs the window\n", opt.pad));
    }
    if (!opt.pad_script.empty() && !padUsesKeyboard(opt.pad)) {
        return usageError("error: --pad-script needs --pad keyboard or keyboard+gamepad\n");
    }
    // A window means somebody is listening; unattended runs must not need a sound device.
    if (!state.audio_given) opt.audio = opt.window ? "wasapi" : "none";
    if (!opt.audio_dump.empty()) {
        // The engine creates the file again on its own thread; creating it here makes a bad path a
        // usage error instead of a warning in the middle of a run.
        const auto probe = WavWriter::create(opt.audio_dump, kAudioSampleRate, kAudioOutputChannels);
        if (!probe) {
            return usageError(std::format("error: --audio-dump {}: {}\n", state.audio_dump_text, probe.error()));
        }
    }
    return std::nullopt;
}

}  // namespace

std::span<const OptionSpec> gameOptionSpecs() { return kSpecs; }

std::string_view gameOptionsUsage() { return kUsage; }

std::expected<GameOptions, OptionsError> parseGameOptions(std::span<const std::string> args) {
    RawValues raw;
    if (std::optional<OptionsError> error = collectCommandLine(args, raw)) {
        error->exit_code = static_cast<int>(ExitCode::Usage);
        return std::unexpected(std::move(*error));
    }

    // Phase 2: every given occurrence is checked, so a bad value is reported even when a later one replaces it.
    ParseState state;
    for (std::size_t e = 0; e < kEntries.size(); ++e) {
        for (const std::string& text : raw[e]) {
            std::string message;
            if (!kEntries[e].validate(state, kEntries[e].spec.name, text, message)) {
                return std::unexpected(usageError(std::move(message)));
            }
        }
    }
    if (std::optional<OptionsError> error = settle(state)) return std::unexpected(std::move(*error));
    return std::move(state.options);
}

void applyGameOptions(const GameOptions& options) {
    // Before anything else logs, so that nothing later in startup logs at the wrong level.
    if (options.log_level) setLogLevel(*options.log_level);
    if (!options.dialog_answer.empty()) setDialogAnswer(options.dialog_answer);
}

}  // namespace tox2
