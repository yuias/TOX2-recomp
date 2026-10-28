#include "app/game_options.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include "audio/audio_output.h"
#include "audio/wav_file.h"
#include "core/fatal.h"
#include "hle/hle.h"

namespace tox2 {
namespace {

// A string built once because the --audio choices name the host's output.
const std::string kUsage = std::format(
        "usage: tox2_game [--game-dir DIR] [--vfs-config FILE] (at least one) [--elf PATH]\n"
        "                [--libsre PATH]\n"
        "                [--seconds N] [--strict] [--log FILE] [--hdd-dir DIR] [--sdata-cache DIR]\n"
        "                [--log-level trace|debug|info|warn|error]\n"
        "                [--quiet-hle] [--load-only] [--png-dump DIR]\n"
        "                [--dialog-answer yes|no|ok] [--save-dialog auto|cancel|window] [--ui-font PATH]\n"
        "                [--pad none|neutral|keyboard|gamepad|keyboard+gamepad]\n"
        "                [--pad-script FILE] [--pad-probe] [--audio none|{}] [--audio-dump FILE]\n"
        "                [--render none|vulkan] [--rsx-capture N[,M...]] [--rsx-census FILE]\n"
        "                [--dump-frames DIR] [--dump-frames-every K] [--shader-cache DIR]\n"
        "                [--resolution-scale N] [--aa-mode supersample|msaa|dlaa] [--vk-validation] [--no-window]\n"
        "                [--anisotropic-filter off|2|4|8|16] [--vk-driver auto|moltenvk|kosmickrisp]\n"
        "                [--window-size 720p|1080p|1440p|2160p]\n"
        "                [--object-motion on|off] [--fullscreen on|off] [--frame-gen off|60|90|120|180|unlimited]\n"
        "                [--fg-present-mode mailbox|immediate|fifo] [--fg-reflex on|off] [--fg-only-generated]\n"
        "                [--fg-cycle SECONDS] (frame generation defaults: mailbox, reflex on)\n"
        "                [--frame-limit vsync|60|off] [--host-priorities on|off] [--yield-backoff on|off]\n"
        "                [--ppu-threads legacy|1|2] [--lwmutex-fast-path on|off] [--lwmutex-watch]\n"
        "                [--dump-guest ADDR:SIZE:FILE]...\n"
        "                [--spu-capture IMAGE:S[+S...]:E[:dispatches=N]]... [--spu-capture-dir DIR]\n"
        "                [--spu-capture-after SECONDS] [--spu-capture-count N] [--spu-capture-every K]\n"
        "                [--config FILE | --no-config]\n",
        kHostAudioOutput);

std::string rsxCaptureError(std::string_view label) {
    return std::format("error: {} needs frame numbers (1-based), e.g. 300,600\n", label);
}

// One occurrence of an option, from the command line or from the config file.
struct RawItem {
    std::string text;  // what the messages quote
    bool from_file = false;
    std::filesystem::path path;  // file Path values; their UTF-8 text must not go through `text`
};

std::filesystem::path itemPath(const RawItem& item) {
    return item.from_file ? item.path : std::filesystem::path(item.text);
}

// What the validators settle besides the GameOptions members: facts about which options were given, and the
// text of the values whose messages quote it.
struct ParseState {
    GameOptions options;
    std::optional<bool> render_flag;  // --render as given
    bool dump_every_given = false;
    bool resolution_scale_given = false;
    bool aa_mode_given = false;
    bool anisotropy_given = false;
    bool vk_driver_given = false;
    bool window_option_given = false;  // --window-size or --fullscreen
    bool frame_gen_given = false;
    bool fg_option_given = false;  // any of the --fg-* diagnostics
    bool pad_given = false;
    bool audio_given = false;
    std::string pad_script_text;
    std::string audio_dump_text;
};

// Checks one occurrence of an option and stores it. `label` names the option in messages: "--name" for the
// command line, "<file>: key" for the config file. On failure sets `error` to the complete message.
using ValidateFn = bool (*)(ParseState& state, std::string_view label, const RawItem& item, std::string& error);

struct OptionEntry {
    OptionSpec spec;
    ValidateFn validate;
};

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

// The window and renderer a run gets. Playing is the default, so the window is on unless the run asks for none:
// --no-window for unattended runs, or an explicit "--render none", which leaves nothing to show. --dump-frames
// and the window imply vulkan; an explicit "--render none" (anywhere on the line) overrides that.
struct RunShape {
    bool window = false;
    bool render_vulkan = false;
};

RunShape runShape(bool no_window, std::optional<bool> render_flag, bool dump_frames) {
    RunShape shape;
    shape.window = !no_window && render_flag.value_or(true);
    shape.render_vulkan = render_flag.value_or(dump_frames || shape.window);
    return shape;
}

// Validators. Each one's message is the text the option has always printed.

template <std::filesystem::path GameOptions::* Member>
bool setPath(ParseState& state, std::string_view, const RawItem& item, std::string&) {
    state.options.*Member = itemPath(item);
    return true;
}

template <bool GameOptions::* Member>
bool setFlag(ParseState& state, std::string_view, const RawItem&, std::string&) {
    state.options.*Member = true;
    return true;
}

template <bool GameOptions::* Member>
bool setSwitch(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    if (item.text != "on" && item.text != "off") return fail(error, std::format("error: {} needs on or off\n", label));
    state.options.*Member = item.text == "on";
    return true;
}

bool setPadScript(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    state.pad_script_text = item.text;
    state.options.pad_script = itemPath(item);
    auto script = loadPadScript(state.options.pad_script);
    if (!script) {
        return fail(error, std::format("error: {} {}: {}\n", label, state.pad_script_text, script.error()));
    }
    state.options.pad_script_data = std::move(*script);
    return true;
}

bool setAudioDump(ParseState& state, std::string_view, const RawItem& item, std::string&) {
    state.audio_dump_text = item.text;
    state.options.audio_dump = itemPath(item);
    return true;
}

bool setLogLevelOption(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    const std::optional<LogLevel> level = parseLogLevel(item.text);
    if (!level) return fail(error, std::format("error: {} needs trace, debug, info, warn or error\n", label));
    state.options.log_level = level;
    return true;
}

bool setDialogAnswerOption(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    if (item.text != "yes" && item.text != "no" && item.text != "ok") {
        return fail(error, std::format("error: {} needs yes, no or ok\n", label));
    }
    state.options.dialog_answer = item.text;
    return true;
}

bool setSaveDialog(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    if (item.text == "auto") {
        state.options.save_dialog = SaveDialogMode::Auto;
    } else if (item.text == "cancel") {
        state.options.save_dialog = SaveDialogMode::Cancel;
    } else if (item.text == "window") {
        state.options.save_dialog = SaveDialogMode::Window;
    } else {
        return fail(error, std::format("error: {} needs auto, cancel or window\n", label));
    }
    return true;
}

bool setPad(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    const std::string& text = item.text;
    if (text != "none" && text != "neutral" && text != "keyboard" && text != "gamepad" && text != "keyboard+gamepad") {
        return fail(error,
                    std::format("error: {} needs none, neutral, keyboard, gamepad or keyboard+gamepad\n", label));
    }
    state.options.pad = text;
    state.pad_given = true;
    return true;
}

bool setAudio(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    if (item.text != "none" && item.text != kHostAudioOutput) {
        return fail(error, std::format("error: {} needs none or {}\n", label, kHostAudioOutput));
    }
    state.options.audio = item.text;
    state.audio_given = true;
    return true;
}

bool setRender(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    if (item.text != "none" && item.text != "vulkan") {
        return fail(error, std::format("error: {} needs none or vulkan\n", label));
    }
    state.render_flag = item.text == "vulkan";
    return true;
}

bool setPpuThreads(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    if (item.text == "legacy") {
        state.options.ppu_threads = PpuThreadsMode::Legacy;
    } else if (item.text == "1") {
        state.options.ppu_threads = PpuThreadsMode::Split1;
    } else if (item.text == "2") {
        state.options.ppu_threads = PpuThreadsMode::Split2;
    } else {
        return fail(error, std::format("error: {} needs legacy, 1 or 2\n", label));
    }
    return true;
}

bool setSeconds(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    if (!parseUnsigned(item.text, state.options.seconds)) {
        return fail(error, std::format("error: {} needs a non-negative integer\n", label));
    }
    return true;
}

bool setDumpFramesEvery(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    if (!parseUnsigned(item.text, state.options.dump_frames_every) || state.options.dump_frames_every == 0) {
        return fail(error, std::format("error: {} needs a positive integer\n", label));
    }
    state.dump_every_given = true;
    return true;
}

bool setRsxCapture(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    // The frames of every occurrence accumulate, and a later list only has to be valid, not non-empty.
    if (!parseFrameList(item.text, state.options.rsx_capture_frames)) return fail(error, rsxCaptureError(label));
    return true;
}

bool addDumpGuest(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    std::string parse_error;
    auto spec = parseGuestDumpSpec(item.text, parse_error);
    if (!spec) return fail(error, std::format("error: {} {}: {}\n", label, item.text, parse_error));
    state.options.dumps.push_back(std::move(*spec));
    return true;
}

bool addSpuCapture(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    std::string parse_error;
    auto selector = parseSpuCaptureSelector(item.text, parse_error);
    if (!selector) return fail(error, std::format("error: {} {}: {}\n", label, item.text, parse_error));
    state.options.spu_captures.push_back(std::move(*selector));
    return true;
}

bool setSpuCaptureDir(ParseState& state, std::string_view, const RawItem& item, std::string&) {
    state.options.spu_capture.dir = itemPath(item).string();
    return true;
}

bool setSpuCaptureAfter(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    std::uint32_t seconds = 0;
    if (!parseUnsigned(item.text, seconds)) {
        return fail(error, std::format("error: {} needs a non-negative integer\n", label));
    }
    state.options.spu_capture.after_seconds = seconds;
    return true;
}

template <std::uint32_t SpuCaptureSettings::* Member>
bool setSpuCapturePositive(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    std::uint32_t value = 0;
    if (!parseUnsigned(item.text, value) || value == 0) {
        return fail(error, std::format("error: {} needs a positive integer\n", label));
    }
    state.options.spu_capture.*Member = value;
    return true;
}

bool setFrameLimit(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    if (item.text == "vsync") {
        state.options.frame_limit = RsxFlipLimit::Vsync;
    } else if (item.text == "60") {
        state.options.frame_limit = RsxFlipLimit::Hz60;
    } else if (item.text == "off") {
        state.options.frame_limit = RsxFlipLimit::Off;
    } else {
        return fail(error, std::format("error: {} needs vsync, 60 or off\n", label));
    }
    return true;
}

bool setResolutionScale(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    if (!parseUnsigned(item.text, state.options.resolution_scale) || state.options.resolution_scale < 1 ||
        state.options.resolution_scale > kMaxResolutionScale) {
        return fail(error, std::format("error: {} needs a whole number from 1 to {}\n", label, kMaxResolutionScale));
    }
    state.resolution_scale_given = true;
    return true;
}

bool setAaMode(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    const std::optional<AaMode> mode = parseAaMode(item.text);
    if (!mode) return fail(error, std::format("error: {} needs supersample|msaa|dlaa\n", label));
    state.options.aa_mode = *mode;
    state.aa_mode_given = true;
    return true;
}

bool setAnisotropicFilter(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    const std::optional<std::uint32_t> level = parseAnisotropy(item.text);
    if (!level) return fail(error, std::format("error: {} needs off, 2, 4, 8 or 16\n", label));
    state.options.anisotropy = *level;
    state.anisotropy_given = true;
    return true;
}

bool setVkDriver(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    const std::optional<VkDriverChoice> choice = parseVkDriverChoice(item.text);
    if (!choice) return fail(error, std::format("error: {} needs auto, moltenvk or kosmickrisp\n", label));
    state.options.vk_driver = *choice;
    state.vk_driver_given = true;
    return true;
}

bool setWindowSize(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    const std::optional<WindowSizePreset> preset = parseWindowSize(item.text);
    if (!preset) return fail(error, std::format("error: {} needs 720p, 1080p, 1440p or 2160p\n", label));
    state.options.window_size = *preset;
    state.window_option_given = true;
    return true;
}

bool setFullscreen(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    if (!setSwitch<&GameOptions::fullscreen>(state, label, item, error)) return false;
    state.window_option_given = true;
    return true;
}

bool setFrameGen(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    const std::optional<FrameGenTarget> target = parseFrameGenTarget(item.text);
    if (!target) return fail(error, std::format("error: {} needs off|60|90|120|180|unlimited\n", label));
    state.options.frame_gen = *target;
    state.frame_gen_given = true;
    return true;
}

bool setFgPresentMode(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    if (item.text == "fifo") {
        state.options.fg_present_mode = FgPresentMode::Fifo;
    } else if (item.text == "mailbox") {
        state.options.fg_present_mode = FgPresentMode::Mailbox;
    } else if (item.text == "immediate") {
        state.options.fg_present_mode = FgPresentMode::Immediate;
    } else {
        return fail(error, std::format("error: {} needs fifo|mailbox|immediate\n", label));
    }
    state.fg_option_given = true;
    return true;
}

bool setFgReflex(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    if (!setSwitch<&GameOptions::fg_reflex>(state, label, item, error)) return false;
    state.fg_option_given = true;
    return true;
}

bool setFgOnlyGenerated(ParseState& state, std::string_view, const RawItem&, std::string&) {
    state.options.fg_only_generated = true;
    state.fg_option_given = true;
    return true;
}

bool setFgCycle(ParseState& state, std::string_view label, const RawItem& item, std::string& error) {
    constexpr std::uint32_t kMaxCycleSeconds = 600;
    if (!parseUnsigned(item.text, state.options.fg_cycle_seconds) || state.options.fg_cycle_seconds < 1 ||
        state.options.fg_cycle_seconds > kMaxCycleSeconds) {
        return fail(error, std::format("error: {} needs a whole number from 1 to {}\n", label, kMaxCycleSeconds));
    }
    state.fg_option_given = true;
    return true;
}

// Value errors are reported in this order. An option that takes a value is any kind but Flag. `needs` is also
// set on options that are not persisted: a key of the config file that the run cannot use is left out.
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
        {{"ui-font", OptionKind::Path}, setPath<&GameOptions::ui_font>},
        {{"dump-frames", OptionKind::Path, false, OptionNeeds::Renderer}, setPath<&GameOptions::dump_frames>},
        {{"shader-cache", OptionKind::Path, false, OptionNeeds::Renderer}, setPath<&GameOptions::shader_cache>},
        {{"log-level", OptionKind::String}, setLogLevelOption},
        {{"dialog-answer", OptionKind::String}, setDialogAnswerOption},
        {{"save-dialog", OptionKind::String}, setSaveDialog},
        {{"pad", OptionKind::String}, setPad},
        {{"audio", OptionKind::String}, setAudio},
        {{"render", OptionKind::String}, setRender},
        {{"ppu-threads", OptionKind::String}, setPpuThreads},
        {{"seconds", OptionKind::Unsigned}, setSeconds},
        {{"dump-frames-every", OptionKind::Unsigned, false, OptionNeeds::Renderer}, setDumpFramesEvery},
        {{"strict", OptionKind::Flag}, setFlag<&GameOptions::strict>},
        {{"quiet-hle", OptionKind::Flag}, setFlag<&GameOptions::quiet_hle>},
        {{"load-only", OptionKind::Flag}, setFlag<&GameOptions::load_only>},
        {{"pad-probe", OptionKind::Flag, false, OptionNeeds::Window}, setFlag<&GameOptions::pad_probe>},
        {{"vk-validation", OptionKind::Flag, false, OptionNeeds::Renderer}, setFlag<&GameOptions::vk_validation>},
        {{"no-window", OptionKind::Flag}, setFlag<&GameOptions::no_window>},
        {{"lwmutex-watch", OptionKind::Flag}, setFlag<&GameOptions::lwmutex_watch>},
        {{"host-priorities", OptionKind::Switch}, setSwitch<&GameOptions::host_priorities>},
        {{"yield-backoff", OptionKind::Switch}, setSwitch<&GameOptions::yield_backoff>},
        {{"lwmutex-fast-path", OptionKind::Switch}, setSwitch<&GameOptions::lwmutex_fast_path>},
        {{"rsx-capture", OptionKind::FrameList}, setRsxCapture},
        {{"dump-guest", OptionKind::StringList}, addDumpGuest},
        {{"spu-capture", OptionKind::StringList}, addSpuCapture},
        {{"spu-capture-dir", OptionKind::Path}, setSpuCaptureDir},
        {{"spu-capture-after", OptionKind::Unsigned}, setSpuCaptureAfter},
        {{"spu-capture-count", OptionKind::Unsigned}, setSpuCapturePositive<&SpuCaptureSettings::count>},
        {{"spu-capture-every", OptionKind::Unsigned}, setSpuCapturePositive<&SpuCaptureSettings::every>},
        {{"frame-limit", OptionKind::String, true, OptionNeeds::Nothing}, setFrameLimit},
        {{"resolution-scale", OptionKind::Unsigned, true, OptionNeeds::Renderer}, setResolutionScale},
        {{"aa-mode", OptionKind::String, true, OptionNeeds::Renderer}, setAaMode},
        {{"object-motion", OptionKind::Switch, true, OptionNeeds::Renderer}, setSwitch<&GameOptions::object_motion>},
        {{"anisotropic-filter", OptionKind::String, true, OptionNeeds::Renderer}, setAnisotropicFilter},
        {{"vk-driver", OptionKind::String, false, OptionNeeds::Renderer}, setVkDriver},
        {{"window-size", OptionKind::String, true, OptionNeeds::Window}, setWindowSize},
        {{"fullscreen", OptionKind::Switch, true, OptionNeeds::Window}, setFullscreen},
        {{"frame-gen", OptionKind::String, true, OptionNeeds::Window}, setFrameGen},
        {{"fg-present-mode", OptionKind::String, false, OptionNeeds::Window}, setFgPresentMode},
        {{"fg-reflex", OptionKind::Switch, false, OptionNeeds::Window}, setFgReflex},
        {{"fg-only-generated", OptionKind::Flag, false, OptionNeeds::Window}, setFgOnlyGenerated},
        {{"fg-cycle", OptionKind::Unsigned, false, OptionNeeds::Window}, setFgCycle},
});

constexpr auto kSpecs = [] {
    std::array<OptionSpec, kEntries.size()> specs{};
    for (std::size_t i = 0; i < kEntries.size(); ++i) specs[i] = kEntries[i].spec;
    return specs;
}();

constexpr std::size_t entryIndex(std::string_view name) {
    for (std::size_t e = 0; e < kEntries.size(); ++e) {
        if (kEntries[e].spec.name == name) return e;
    }
    return kEntries.size();
}

constexpr std::size_t kDumpFramesEntry = entryIndex("dump-frames");
constexpr std::size_t kRenderEntry = entryIndex("render");
constexpr std::size_t kNoWindowEntry = entryIndex("no-window");
static_assert(kDumpFramesEntry < kEntries.size() && kRenderEntry < kEntries.size() && kNoWindowEntry < kEntries.size());

// What phase 1 finds on the command line. `raw` holds the text of every occurrence of each table option, indexed
// like kEntries; --config and --no-config are not table options, so a "config" key in the file is just unknown.
struct CommandLine {
    std::array<std::vector<std::string>, kEntries.size()> raw;
    std::optional<std::string> config;
    bool no_config = false;
};

// Phase 1: finds the options and their values in argument order.
std::optional<OptionsError> collectCommandLine(std::span<const std::string> args, CommandLine& line) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--no-config") {
            line.no_config = true;
            continue;
        }
        if (arg == "--config") {
            if (i + 1 >= args.size()) return OptionsError{.message = "error: --config needs a value\n"};
            line.config = args[++i];  // the last one wins
            continue;
        }
        std::size_t index = kEntries.size();
        if (arg.starts_with("--")) index = entryIndex(arg.substr(2));
        if (index == kEntries.size()) return OptionsError{.message = std::format("error: unknown argument {}\n", arg)};
        const OptionSpec& spec = kEntries[index].spec;
        if (spec.kind == OptionKind::Flag) {
            line.raw[index].emplace_back();
            continue;
        }
        if (i + 1 >= args.size()) {
            std::string message = std::format("error: {} needs a value\n", arg);
            // The frame list has always reported its format after the missing value as well.
            if (spec.kind == OptionKind::FrameList) message += rsxCaptureError("--rsx-capture");
            return OptionsError{.message = std::move(message)};
        }
        line.raw[index].push_back(args[++i]);
    }
    return std::nullopt;
}

OptionsError usageError(std::string message) {
    return OptionsError{.exit_code = static_cast<int>(ExitCode::Usage), .message = std::move(message)};
}

// A problem with the config file itself: no usage text, the command line is not at fault.
OptionsError fileError(std::string_view file, std::string_view what) {
    return OptionsError{.exit_code = static_cast<int>(ExitCode::Usage),
                        .message = std::format("error: {}: {}\n", file, what),
                        .print_usage = false};
}

struct ConfigLocation {
    std::filesystem::path file;      // empty when none is read
    std::filesystem::path settings;  // empty when changes are not saved
};

bool isRegularFile(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

std::filesystem::path absolutePath(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path result = std::filesystem::absolute(path, ec);
    return ec ? path : result;
}

std::expected<ConfigLocation, OptionsError> locateConfig(const CommandLine& line, const ConfigSearch& search) {
    ConfigLocation location;
    if (line.no_config) return location;
    constexpr std::string_view kFileName = "config.json";
    if (line.config) {
        std::filesystem::path file(*line.config);
        // Relative to the directory the caller treats as the working directory, so that the lookup does not
        // depend on the process state.
        if (file.is_relative() && !search.working_dir.empty()) file = search.working_dir / file;
        file = absolutePath(file);
        if (!isRegularFile(file)) return std::unexpected(fileError(*line.config, "cannot open the file"));
        location.file = file;
        location.settings = file;
        return location;
    }
    for (const std::filesystem::path& dir : {search.working_dir, search.executable_dir}) {
        if (dir.empty()) continue;
        const std::filesystem::path candidate = absolutePath(dir / kFileName);
        if (isRegularFile(candidate)) {
            location.file = candidate;
            location.settings = candidate;
            return location;
        }
    }
    // A new file is never created in the working directory, where it would end up in the repository.
    if (!search.executable_dir.empty()) location.settings = absolutePath(search.executable_dir / kFileName);
    return location;
}

std::optional<std::string> readBinaryFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (stream.bad()) return std::nullopt;
    return text;
}

// The occurrences of each option that take effect: those of the command line when it has any (they replace the
// file's value as a whole), else the file's value. A file value that a command-line occurrence replaces is
// not checked, because it has no effect on this run.
struct Chosen {
    std::vector<RawItem> items;
    bool from_file = false;
};
using ChosenValues = std::array<Chosen, kEntries.size()>;

void addFileItems(const OptionSpec& spec, const ConfigValue& value, Chosen& chosen) {
    chosen.from_file = true;
    const auto text_item = [](std::string text) {
        return RawItem{.text = std::move(text), .from_file = true, .path = {}};
    };
    switch (spec.kind) {
        case OptionKind::Path:
            chosen.items.push_back({.text = value.path.string(), .from_file = true, .path = value.path});
            break;
        case OptionKind::String:
        case OptionKind::Unsigned:
        case OptionKind::Switch: chosen.items.push_back(text_item(value.text)); break;
        case OptionKind::Flag:
            // false is the same as leaving the key out.
            if (value.flag) chosen.items.push_back(text_item({}));
            break;
        case OptionKind::FrameList:
        case OptionKind::StringList:
            for (const std::string& item : value.items) chosen.items.push_back(text_item(item));
            break;
    }
}

ChosenValues chooseValues(const CommandLine& line, const ConfigFileContents& file) {
    ChosenValues chosen;
    for (std::size_t e = 0; e < kEntries.size(); ++e) {
        for (const std::string& text : line.raw[e])
            chosen[e].items.push_back({.text = text, .from_file = false, .path = {}});
    }
    for (const ConfigValue& value : file.values) {
        const std::size_t e = entryIndex(value.option);
        if (e == kEntries.size() || !line.raw[e].empty()) continue;
        addFileItems(kEntries[e].spec, value, chosen[e]);
    }
    return chosen;
}

// The window and renderer of the run, read off the chosen values before any of them is checked: whether a
// file value that needs them survives depends on it, and a bad value is still reported in table order.
RunShape peekRunShape(const ChosenValues& chosen) {
    std::optional<bool> render_flag;
    if (!chosen[kRenderEntry].items.empty()) {
        const std::string& text = chosen[kRenderEntry].items.back().text;
        if (text == "vulkan") render_flag = true;
        if (text == "none") render_flag = false;
    }
    const bool dump_frames =
            !chosen[kDumpFramesEntry].items.empty() && !chosen[kDumpFramesEntry].items.back().text.empty();
    return runShape(!chosen[kNoWindowEntry].items.empty(), render_flag, dump_frames);
}

// Leaves out the file values whose renderer or window the run lacks, so a saved setting never breaks an
// unattended run. The same option on the command line keeps its usage error.
void dropUnusableFileValues(ChosenValues& chosen, const RunShape& shape, std::vector<std::string>& ignored) {
    for (std::size_t e = 0; e < kEntries.size(); ++e) {
        const OptionSpec& spec = kEntries[e].spec;
        if (!chosen[e].from_file || chosen[e].items.empty()) continue;
        const char* reason = nullptr;
        if (spec.needs == OptionNeeds::Renderer && !shape.render_vulkan) reason = "the run has no renderer";
        if (spec.needs == OptionNeeds::Window && !shape.window) reason = "the run has no window";
        if (!reason) continue;
        ignored.push_back(std::format("{} ignored: {}", configKeyFor(spec.name), reason));
        chosen[e].items.clear();
    }
}

// Checks that depend on several options, in the order they have always run.
// `file_note` is appended to the combined checks, whose cause may be in the config file.
std::optional<OptionsError> settle(ParseState& state, const std::string& file_note) {
    GameOptions& opt = state.options;
    const auto combined = [&file_note](std::string message) { return usageError(std::move(message) + file_note); };
    if (opt.game_dir.empty() && opt.vfs_config.empty()) {
        return combined("error: --game-dir or --vfs-config is required\n");
    }
    const RunShape shape = runShape(opt.no_window, state.render_flag, !opt.dump_frames.empty());
    opt.window = shape.window;
    opt.render_vulkan = shape.render_vulkan;
    const bool render_option = !opt.dump_frames.empty() || state.dump_every_given || !opt.shader_cache.empty() ||
                               opt.vk_validation || state.resolution_scale_given || state.aa_mode_given ||
                               state.anisotropy_given || state.vk_driver_given;
    if (render_option && !opt.render_vulkan) {
        return combined(
                "error: --dump-frames, --dump-frames-every, --shader-cache, --resolution-scale, --aa-mode, "
                "--anisotropic-filter, --vk-driver and --vk-validation need --render vulkan\n");
    }
    if (!opt.spu_captures.empty() && opt.spu_capture.dir.empty()) {
        return combined("error: --spu-capture needs --spu-capture-dir\n");
    }
    // GameInput delivers readings to the foreground window only, so unattended runs have nothing to probe.
    if (opt.pad_probe && !opt.window) return combined("error: --pad-probe needs the window\n");
    if (state.window_option_given && !opt.window) {
        return combined("error: --window-size and --fullscreen need the window\n");
    }
    if (state.frame_gen_given && !opt.window) return combined("error: --frame-gen needs the window\n");
    if (state.fg_option_given && opt.frame_gen == FrameGenTarget::Off) {
        return combined(
                "error: --fg-present-mode, --fg-reflex, --fg-only-generated and --fg-cycle need --frame-gen other "
                "than off\n");
    }
    // A window or a script is what makes a keyboard pad useful, so either one selects it by default.
    const bool pad_input = opt.window || !opt.pad_script.empty();
    if (!state.pad_given) opt.pad = opt.window ? "keyboard+gamepad" : pad_input ? "keyboard" : "neutral";
    if (opt.pad == "keyboard" && !pad_input) {
        return combined("error: --pad keyboard needs the window or --pad-script\n");
    }
    // GameInput delivers readings to the foreground window only.
    if (padUsesGamepad(opt.pad) && !opt.window) {
        return combined(std::format("error: --pad {} needs the window\n", opt.pad));
    }
    if (!opt.pad_script.empty() && !padUsesKeyboard(opt.pad)) {
        return combined("error: --pad-script needs --pad keyboard or keyboard+gamepad\n");
    }
    // A window means somebody is listening; unattended runs must not need a sound device.
    if (!state.audio_given) opt.audio = opt.window ? std::string(kHostAudioOutput) : "none";
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

bool padUsesKeyboard(std::string_view pad) { return pad == "keyboard" || pad == "keyboard+gamepad"; }
bool padUsesGamepad(std::string_view pad) { return pad == "gamepad" || pad == "keyboard+gamepad"; }

std::span<const OptionSpec> gameOptionSpecs() { return kSpecs; }

std::string_view gameOptionsUsage() { return kUsage; }

std::expected<GameOptions, OptionsError> parseGameOptions(std::span<const std::string> args,
                                                          const ConfigSearch& search) {
    CommandLine line;
    if (std::optional<OptionsError> error = collectCommandLine(args, line)) {
        error->exit_code = static_cast<int>(ExitCode::Usage);
        return std::unexpected(std::move(*error));
    }
    if (line.no_config && line.config) return std::unexpected(usageError("error: --no-config excludes --config\n"));

    auto location = locateConfig(line, search);
    if (!location) return std::unexpected(std::move(location.error()));
    const std::string file_name = location->file.string();
    ConfigFileContents file;
    if (!location->file.empty()) {
        const std::optional<std::string> text = readBinaryFile(location->file);
        if (!text) return std::unexpected(fileError(file_name, "cannot open the file"));
        auto parsed = parseConfigFile(*text, location->file.parent_path(), kSpecs);
        if (!parsed) return std::unexpected(fileError(file_name, parsed.error()));
        file = std::move(*parsed);
    }

    ChosenValues chosen = chooseValues(line, file);
    ParseState state;
    dropUnusableFileValues(chosen, peekRunShape(chosen), state.options.config_ignored);

    // Phase 2: every occurrence that takes effect is checked, so a bad value is reported even when a later one
    // on the command line replaces it.
    for (std::size_t e = 0; e < kEntries.size(); ++e) {
        const std::string_view name = kEntries[e].spec.name;
        for (const RawItem& item : chosen[e].items) {
            const std::string label =
                    item.from_file ? std::format("{}: {}", file_name, configKeyFor(name)) : std::format("--{}", name);
            std::string message;
            if (!kEntries[e].validate(state, label, item, message)) {
                OptionsError error = usageError(std::move(message));
                // A bad value in the file is a file problem; the usage text describes the command line.
                error.print_usage = !item.from_file;
                return std::unexpected(std::move(error));
            }
        }
    }
    const std::string file_note =
            location->file.empty() ? std::string() : std::format("note: settings were also read from {}\n", file_name);
    if (std::optional<OptionsError> error = settle(state, file_note)) return std::unexpected(std::move(*error));
    GameOptions& options = state.options;
    options.no_config = line.no_config;
    options.config_file = std::move(location->file);
    options.settings_file = std::move(location->settings);
    options.config_warnings = std::move(file.warnings);
    return std::move(options);
}

void applyGameOptions(const GameOptions& options) {
    // Before anything else logs, so that nothing later in startup logs at the wrong level.
    if (options.log_level) setLogLevel(*options.log_level);
    if (!options.dialog_answer.empty()) setDialogAnswer(options.dialog_answer);

    if (!options.config_file.empty()) {
        const std::string file = options.config_file.string();
        logInfo("tox2: config: {}", file);
        for (const std::string& warning : options.config_warnings) logWarn("tox2: config: {}: {}", file, warning);
        for (const std::string& ignored : options.config_ignored) logDebug("tox2: config: {}: {}", file, ignored);
    } else if (options.no_config) {
        logInfo("tox2: config: off (--no-config)");
    } else if (!options.settings_file.empty()) {
        logInfo("tox2: config: none found; changes are saved to {}", options.settings_file.string());
    } else {
        logInfo("tox2: config: none found");
    }
}

}  // namespace tox2
