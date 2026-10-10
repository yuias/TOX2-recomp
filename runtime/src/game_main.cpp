// dal_game: loads the decrypted EBOOT.elf and runs it through the generated PPU code.

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <string_view>
#include <vector>

#include "audio/audio_engine.h"
#include "audio/audio_output.h"
#include "audio/wasapi_output.h"
#include "audio/wav_file.h"
#include "core/boot.h"
#include "core/census.h"
#include "core/elf_loader.h"
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "core/param_sfo.h"
#include "core/threads.h"
#include "core/timer_resolution.h"
#include "dal/ppu_context.h"
#include "hle/cell_audio_ports.h"
#include "hle/hle.h"
#include "input/gameinput_backend.h"
#include "input/gamepad_hub.h"
#include "input/keyboard_pad.h"
#include "input/pad_script.h"
#include "lv2/vfs.h"
#include "lv2/vfs_config.h"
#include "media/atrac_decoder.h"
#include "media/ffmpeg_codecs.h"
#include "media/movie_decoder.h"
#include "render/vk_context.h"
#include "render/vk_renderer.h"
#include "render/vk_window.h"
#include "rsx/rsx_thread.h"

namespace {

struct Options {
    std::string game_dir;
    std::string elf;
    std::string hdd_dir;
    std::string sdata_cache;
    std::string vfs_config;
    std::string png_dump;
    std::string rsx_census;
    std::string log_file;
    std::string dialog_answer;
    std::string pad;  // --pad as given; empty until the defaults are settled after all arguments
    std::string pad_script;
    std::string audio;  // --audio as given; empty until the defaults are settled after all arguments
    std::string audio_dump;
    std::vector<dal::PadScriptEvent> pad_script_events;
    // The host pad state, set for every --pad that reads host input (keyboard, gamepad, both). The
    // window thread, the script thread and the gamepad thread each write their own source of it.
    std::shared_ptr<dal::KeyboardPadState> keyboard;
    unsigned seconds = 0;
    bool strict = false;
    bool quiet_hle = false;
    bool load_only = false;
    bool render_vulkan = false;
    std::optional<bool> render_flag;  // --render as given; the final mode is settled after all arguments
    bool dump_every_given = false;
    bool vk_validation = false;
    bool no_window = false;
    bool window = false;  // settled after all arguments
    bool pad_probe = false;
    std::string dump_frames;
    std::uint32_t dump_frames_every = 1;
    std::string shader_cache;
    std::vector<std::uint32_t> rsx_capture_frames;
};

bool padUsesKeyboard(std::string_view pad) { return pad == "keyboard" || pad == "keyboard+gamepad"; }
bool padUsesGamepad(std::string_view pad) { return pad == "gamepad" || pad == "keyboard+gamepad"; }

// The Vulkan device and the renderer for the RSX thread. Built and destroyed on that thread; the
// counters are published for the census, which reads them from another one.
class VulkanExecutor final : public dal::RsxExecutor {
public:
    // `presenter` may be null (no window).
    VulkanExecutor(std::unique_ptr<dal::VkContext> context, std::unique_ptr<dal::Presenter> presenter,
                   const dal::RenderOptions& options)
        : context_(std::move(context)),
          presenter_(std::move(presenter)),
          renderer_(std::make_unique<dal::RsxRenderer>(*context_, options)) {
        renderer_->setPresenter(presenter_.get());
    }

    ~VulkanExecutor() override {
        // The renderer and the swapchain go before the device they were created from.
        renderer_.reset();
        presenter_.reset();
        context_.reset();
    }

    bool valid() const { return renderer_->valid(); }
    const std::string& error() const { return renderer_->error(); }
    const dal::VkContext& context() const { return *context_; }

    void execute(std::span<dal::RsxCommand> commands) override {
        if (!renderer_->valid()) return;
        renderer_->execute(commands);
        const dal::RendererStats& stats = renderer_->stats();
        draws_.store(stats.draws, std::memory_order_relaxed);
        skipped_.store(stats.draws_skipped, std::memory_order_relaxed);
        clears_.store(stats.clears, std::memory_order_relaxed);
        flips_.store(stats.flips, std::memory_order_relaxed);
        if (!renderer_->valid()) {
            failed_.store(true, std::memory_order_relaxed);
            dal::logError("dal: render: renderer stopped: {}", renderer_->error());
        }
    }

    dal::RsxRenderCounts counts() const override {
        dal::RsxRenderCounts counts;
        counts.draws = draws_.load(std::memory_order_relaxed);
        counts.draws_skipped = skipped_.load(std::memory_order_relaxed);
        counts.clears = clears_.load(std::memory_order_relaxed);
        counts.flips = flips_.load(std::memory_order_relaxed);
        // The dump worker keeps its own atomic counters, so these are current without the RSX thread's help.
        const dal::DumpCounts dumps = renderer_->dumpCounts();
        counts.frames_dumped = dumps.written;
        counts.dumps_skipped = dumps.skipped;
        counts.dumps_failed = dumps.failed;
        counts.validation = context_->validationEnabled();
        counts.validation_errors = context_->validationErrorCount();
        counts.failed = failed_.load(std::memory_order_relaxed);
        return counts;
    }

    std::uint64_t flushOutput(std::chrono::milliseconds timeout) override { return renderer_->flushDumps(timeout); }

private:
    std::unique_ptr<dal::VkContext> context_;
    std::unique_ptr<dal::Presenter> presenter_;
    std::unique_ptr<dal::RsxRenderer> renderer_;
    std::atomic<std::uint64_t> draws_{0}, skipped_{0}, clears_{0}, flips_{0};
    std::atomic<bool> failed_{false};
};

// Runs on the RSX thread; a null result makes the consumer continue without rendering.
std::unique_ptr<dal::RsxExecutor> createVulkanExecutor(const Options& opt, std::string& error) {
    const auto start = std::chrono::steady_clock::now();
    dal::VkContextOptions context_options;
    context_options.validation = opt.vk_validation;
    context_options.window = opt.window;
    std::unique_ptr<dal::VkContext> context = dal::createVkContext(context_options, error);
    if (!context) return nullptr;
    const std::uint32_t api = context->deviceProperties().apiVersion;
    dal::logInfo("dal: render: vulkan device {} (api {}.{}.{}, d24s8 {})", context->deviceName(),
                 VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api),
                 context->hasD24S8() ? "yes" : "no");
    if (opt.vk_validation && !context->validationEnabled()) {
        dal::logWarn("dal: render: validation layer requested but not installed");
    }

    dal::RenderOptions render_options;
    render_options.dump_dir = opt.dump_frames;
    render_options.dump_every = opt.dump_frames_every;
    render_options.shader_cache = opt.shader_cache.empty() ? std::filesystem::path("generated") / "shader_cache"
                                                           : std::filesystem::path(opt.shader_cache);
    std::unique_ptr<dal::Presenter> presenter;
    if (opt.window) {
        dal::PresenterOptions presenter_options;
        presenter_options.frame_slots = dal::RsxRenderer::kFramesInFlight;
        presenter_options.on_close = [] { dal::stopProcess(dal::ExitCode::ProcessExit, "dal: stop: window closed"); };
        if (opt.keyboard && padUsesKeyboard(opt.pad)) {
            // These run on the window thread; the state is lock-free, so the guest never waits on it.
            // Focus loss releases the keyboard source only: the gamepad thread is the sole writer of its own.
            presenter_options.on_key = [keyboard = opt.keyboard](const dal::KeyMessage& key) {
                keyboard->keyEvent(key.scan_code, key.down, key.repeat, dal::KeyboardPadState::Clock::now());
                if (key.repeat) return;
                if (const std::optional<dal::PadButton> button = dal::padButtonForScanCode(key.scan_code)) {
                    dal::logDebug("dal: pad: key {:#x} {} -> {}", key.scan_code, key.down ? "down" : "up",
                                  dal::padButtonName(*button));
                }
            };
            presenter_options.on_focus_lost = [keyboard = opt.keyboard] {
                keyboard->releaseAll(dal::InputSource::Keyboard);
            };
        }
        presenter = dal::Presenter::create(*context, std::move(presenter_options), error);
        if (!presenter) {
            error = "window: " + error;
            return nullptr;
        }
    }
    auto executor = std::make_unique<VulkanExecutor>(std::move(context), std::move(presenter), render_options);
    if (!executor->valid()) {
        error = executor->error();
        return nullptr;
    }
    const auto init_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    dal::logInfo("dal: render: device and renderer ready in {} ms", init_ms);
    if (!render_options.dump_dir.empty()) {
        dal::logInfo("dal: render: dumping every {} flip(s) to {}", render_options.dump_every,
                     render_options.dump_dir.generic_string());
    }
    return executor;
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

void printUsage() {
    std::fputs(
            "usage: dal_game [--game-dir DIR] [--vfs-config FILE] (at least one) [--elf PATH]\n"
            "                [--seconds N] [--strict] [--log FILE] [--hdd-dir DIR] [--sdata-cache DIR]\n"
            "                [--log-level trace|debug|info|warn|error]\n"
            "                [--quiet-hle] [--load-only] [--png-dump DIR]\n"
            "                [--dialog-answer yes|no|ok] [--pad none|neutral|keyboard|gamepad|keyboard+gamepad]\n"
            "                [--pad-script FILE] [--pad-probe] [--audio none|wasapi] [--audio-dump FILE]\n"
            "                [--render none|vulkan] [--rsx-capture N[,M...]] [--rsx-census FILE]\n"
            "                [--dump-frames DIR] [--dump-frames-every K] [--shader-cache DIR]\n"
            "                [--vk-validation] [--no-window]\n",
            stderr);
}

std::optional<dal::LogLevel> parseLogLevel(std::string_view name) {
    if (name == "trace") return dal::LogLevel::Trace;
    if (name == "debug") return dal::LogLevel::Debug;
    if (name == "info") return dal::LogLevel::Info;
    if (name == "warn") return dal::LogLevel::Warn;
    if (name == "error") return dal::LogLevel::Error;
    return std::nullopt;
}

bool parseArgs(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; i++) {
        const std::string_view arg = argv[i];
        auto value = [&](std::string& out) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a value\n", argv[i]);
                return false;
            }
            out = argv[++i];
            return true;
        };
        if (arg == "--game-dir") {
            if (!value(opt.game_dir)) return false;
        } else if (arg == "--elf") {
            if (!value(opt.elf)) return false;
        } else if (arg == "--hdd-dir") {
            if (!value(opt.hdd_dir)) return false;
        } else if (arg == "--sdata-cache") {
            if (!value(opt.sdata_cache)) return false;
        } else if (arg == "--vfs-config") {
            if (!value(opt.vfs_config)) return false;
        } else if (arg == "--png-dump") {
            if (!value(opt.png_dump)) return false;
        } else if (arg == "--rsx-census") {
            if (!value(opt.rsx_census)) return false;
        } else if (arg == "--rsx-capture") {
            std::string list;
            if (!value(list) || !parseFrameList(list, opt.rsx_capture_frames)) {
                std::fprintf(stderr, "error: --rsx-capture needs frame numbers (1-based), e.g. 300,600\n");
                return false;
            }
        } else if (arg == "--log") {
            if (!value(opt.log_file)) return false;
        } else if (arg == "--log-level") {
            std::string name;
            if (!value(name)) return false;
            const std::optional<dal::LogLevel> level = parseLogLevel(name);
            if (!level) {
                std::fprintf(stderr, "error: --log-level needs trace, debug, info, warn or error\n");
                return false;
            }
            // Applied here so that nothing later in startup logs at the wrong level.
            dal::setLogLevel(*level);
        } else if (arg == "--dialog-answer") {
            if (!value(opt.dialog_answer)) return false;
            if (!dal::setDialogAnswer(opt.dialog_answer)) {
                std::fprintf(stderr, "error: --dialog-answer needs yes, no or ok\n");
                return false;
            }
        } else if (arg == "--pad") {
            std::string name;
            if (!value(name)) return false;
            if (name != "none" && name != "neutral" && name != "keyboard" && name != "gamepad" &&
                name != "keyboard+gamepad") {
                std::fprintf(stderr, "error: --pad needs none, neutral, keyboard, gamepad or keyboard+gamepad\n");
                return false;
            }
            opt.pad = name;
        } else if (arg == "--pad-script") {
            if (!value(opt.pad_script)) return false;
            auto events = dal::loadPadScript(std::filesystem::path(opt.pad_script));
            if (!events) {
                std::fprintf(stderr, "error: --pad-script %s: %s\n", opt.pad_script.c_str(), events.error().c_str());
                return false;
            }
            opt.pad_script_events = std::move(*events);
        } else if (arg == "--pad-probe") {
            opt.pad_probe = true;
        } else if (arg == "--audio") {
            std::string name;
            if (!value(name)) return false;
            if (name != "none" && name != "wasapi") {
                std::fprintf(stderr, "error: --audio needs none or wasapi\n");
                return false;
            }
            opt.audio = name;
        } else if (arg == "--audio-dump") {
            if (!value(opt.audio_dump)) return false;
        } else if (arg == "--render") {
            std::string name;
            if (!value(name)) return false;
            if (name != "none" && name != "vulkan") {
                std::fprintf(stderr, "error: --render needs none or vulkan\n");
                return false;
            }
            opt.render_flag = name == "vulkan";
        } else if (arg == "--dump-frames") {
            if (!value(opt.dump_frames)) return false;
        } else if (arg == "--dump-frames-every") {
            std::string text;
            if (!value(text)) return false;
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), opt.dump_frames_every);
            if (ec != std::errc{} || end != text.data() + text.size() || opt.dump_frames_every == 0) {
                std::fprintf(stderr, "error: --dump-frames-every needs a positive integer\n");
                return false;
            }
            opt.dump_every_given = true;
        } else if (arg == "--shader-cache") {
            if (!value(opt.shader_cache)) return false;
        } else if (arg == "--vk-validation") {
            opt.vk_validation = true;
        } else if (arg == "--no-window") {
            opt.no_window = true;
        } else if (arg == "--seconds") {
            std::string text;
            if (!value(text)) return false;
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), opt.seconds);
            if (ec != std::errc{} || end != text.data() + text.size()) {
                std::fprintf(stderr, "error: --seconds needs a non-negative integer\n");
                return false;
            }
        } else if (arg == "--strict") {
            opt.strict = true;
        } else if (arg == "--quiet-hle") {
            opt.quiet_hle = true;
        } else if (arg == "--load-only") {
            opt.load_only = true;
        } else {
            std::fprintf(stderr, "error: unknown argument %s\n", argv[i]);
            return false;
        }
    }
    if (opt.game_dir.empty() && opt.vfs_config.empty()) {
        std::fputs("error: --game-dir or --vfs-config is required\n", stderr);
        return false;
    }
    // Playing is the default, so the window is on unless the run asks for none: --no-window for
    // unattended runs, or an explicit "--render none", which leaves nothing to show.
    opt.window = !opt.no_window && opt.render_flag.value_or(true);
    // --dump-frames and the window imply vulkan; an explicit "--render none" (anywhere on the line)
    // overrides that and then conflicts with it. The other options have no effect without a renderer.
    opt.render_vulkan = opt.render_flag.value_or(!opt.dump_frames.empty() || opt.window);
    const bool render_option =
            !opt.dump_frames.empty() || opt.dump_every_given || !opt.shader_cache.empty() || opt.vk_validation;
    if (render_option && !opt.render_vulkan) {
        std::fputs(
                "error: --dump-frames, --dump-frames-every, --shader-cache and --vk-validation need --render vulkan\n",
                stderr);
        return false;
    }
    // GameInput delivers readings to the foreground window only, so unattended runs have nothing to probe.
    if (opt.pad_probe && !opt.window) {
        std::fputs("error: --pad-probe needs the window\n", stderr);
        return false;
    }
    // A window or a script is what makes a keyboard pad useful, so either one selects it by default.
    const bool pad_input = opt.window || !opt.pad_script.empty();
    if (opt.pad.empty()) opt.pad = opt.window ? "keyboard+gamepad" : pad_input ? "keyboard" : "neutral";
    if (opt.pad == "keyboard" && !pad_input) {
        std::fputs("error: --pad keyboard needs the window or --pad-script\n", stderr);
        return false;
    }
    // GameInput delivers readings to the foreground window only.
    if (padUsesGamepad(opt.pad) && !opt.window) {
        std::fprintf(stderr, "error: --pad %s needs the window\n", opt.pad.c_str());
        return false;
    }
    if (!opt.pad_script.empty() && !padUsesKeyboard(opt.pad)) {
        std::fputs("error: --pad-script needs --pad keyboard or keyboard+gamepad\n", stderr);
        return false;
    }
    // A window means somebody is listening; unattended runs must not need a sound device.
    if (opt.audio.empty()) opt.audio = opt.window ? "wasapi" : "none";
    if (!opt.audio_dump.empty()) {
        // The engine creates the file again on its own thread; creating it here makes a bad path a
        // usage error instead of a warning in the middle of a run.
        const auto probe = dal::WavWriter::create(opt.audio_dump, dal::kAudioSampleRate, dal::kAudioOutputChannels);
        if (!probe) {
            std::fprintf(stderr, "error: --audio-dump %s: %s\n", opt.audio_dump.c_str(), probe.error().c_str());
            return false;
        }
    }
    return true;
}

// The movies decode through FFmpeg DLLs loaded at run time. Without them the factory stays unset
// and every movie plays as black frames and silence for its duration.
void installMovieCodecs() {
    std::string error;
    std::unique_ptr<dal::MediaCodecFactory> factory = dal::createFfmpegCodecFactory(error);
    if (!factory) {
        dal::logWarn("dal: sail: codecs unavailable: {}", error);
        return;
    }
    dal::setMediaCodecFactory(std::move(factory));
}

// cellAtrac sounds decode through the same DLLs. Without them every sound plays as silence of its
// own length and the stream protocol is unchanged.
void installAtracDecoder() {
    std::string error;
    std::unique_ptr<dal::AtracDecoderFactory> factory = dal::createFfmpegAtracDecoderFactory(error);
    if (!factory) {
        dal::logWarn("dal: atrac: no decoder: {}", error);
        return;
    }
    dal::setAtracDecoderFactory(std::move(factory));
}

// Video and audio out report what the game's PARAM.SFO allows. A missing or broken file keeps the
// defaults of gameParamSfo().
void loadGameParamSfo() {
    bool read_only = false;
    const auto sfo_path = dal::resolveGuestPath("/dev_bdvd/PS3_GAME/PARAM.SFO", read_only);
    if (!sfo_path) {
        dal::logWarn("dal: sfo: /dev_bdvd is not mounted; using the defaults");
        return;
    }
    auto sfo = dal::readParamSfoFile(*sfo_path);
    if (!sfo) {
        dal::logWarn("dal: sfo: {}; using the defaults", sfo.error());
        return;
    }
    const auto title_id = sfo->strings.find("TITLE_ID");
    const auto resolution = sfo->ints.find("RESOLUTION");
    const auto sound_format = sfo->ints.find("SOUND_FORMAT");
    dal::logInfo("dal: sfo: TITLE_ID {} RESOLUTION {:#x} SOUND_FORMAT {:#x}",
                 title_id == sfo->strings.end() ? "?" : title_id->second,
                 resolution == sfo->ints.end() ? 0x1u : resolution->second,
                 sound_format == sfo->ints.end() ? 0x1u : sound_format->second);
    dal::setGameParamSfo(std::move(*sfo));
}

// The census is printed on every stop path and the process then ends without running destructors,
// so this is also where an --audio-dump file gets its final header.
std::vector<std::string> audioCensusLines() {
    const dal::AudioEngineStats engine = dal::audioEngineStats();
    dal::finishAudioDump();

    std::vector<std::string> lines;
    if (!engine.output.empty()) {  // otherwise cellAudioInit never ran
        const dal::CellAudioStats ports = dal::cellAudioPorts().stats();
        lines.push_back(std::format(
                "audio {}: {} blocks ({} non-silent), skipped {}, dropped {}, underrun {} frames, notify {} sent {} failed, "
                "max producer {:.2f} ms, clock {:+.0f} ppm",
                engine.output, engine.blocks, engine.non_silent_blocks, engine.skipped_blocks,
                engine.output_stats.blocks_dropped, engine.output_stats.underrun_frames, ports.notifies_sent,
                ports.notifies_failed, static_cast<double>(engine.max_producer_time.count()) / 1000.0,
                engine.clock_ppm));
        std::string channels;
        for (std::size_t c = 0; c < ports.nonzero_samples.size(); ++c) {
            channels += std::format("{}{}:{}", c == 0 ? "" : " ", c, ports.nonzero_samples[c]);
        }
        lines.push_back(
                std::format("audio input: {} periods, non-zero samples per channel {}", ports.periods, channels));
    }

    const dal::CellAtracStats atrac = dal::cellAtracStats();
    if (atrac.decoders != 0) {
        lines.push_back(
                std::format("atrac {} decoders, {} frames decoded ({} non-silent), {} errors, {} without decoder",
                            atrac.decoders, atrac.frames, atrac.non_silent, atrac.errors, atrac.without_decoder));
    }
    return lines;
}

void configureAudio(const Options& opt) {
    dal::AudioEngineConfig config;
    // Only the wasapi choice builds a device output, so a run without it never touches COM.
    if (opt.audio == "wasapi") config.make_output = [] { return dal::makeWasapiOutput(); };
    config.dump = opt.audio_dump;
    dal::configureAudioEngine(std::move(config));
    dal::setAudioCensusProvider(&audioCensusLines);
}

// dal_game lives in build/<preset>/bin, and the build writes the .sdat cache next to that
// directory, so a plain run finds it without --sdata-cache. The hdd directory defaults to the same
// place so that game data and saves persist between runs without --hdd-dir. Both apply only when
// neither the command line nor the VFS configuration chose one.
void applyBuildDirDefaults(dal::VfsLayout& layout) {
    const std::filesystem::path exe_dir = dal::executableDirectory();
    if (exe_dir.empty()) return;
    const std::filesystem::path build_dir = exe_dir.parent_path();
    std::error_code ec;
    if (!layout.sdata_cache) {
        const std::filesystem::path cache = build_dir / "sdata_cache";
        if (std::filesystem::is_directory(cache, ec)) {
            dal::logInfo("dal: vfs: sdata cache {} (default)", cache.generic_string());
            layout.sdata_cache = cache;
        }
    }
    if (std::ranges::find(layout.mounts, "dev_hdd0", &dal::VfsMount::name) == layout.mounts.end()) {
        const std::filesystem::path hdd = build_dir / "hdd";
        dal::logInfo("dal: vfs: hdd directory {} (default)", hdd.generic_string());
        for (dal::VfsMount& mount : dal::hddDirMounts(hdd)) {
            std::erase_if(layout.mounts, [&](const dal::VfsMount& m) { return m.name == mount.name; });
            layout.mounts.push_back(std::move(mount));
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    dal::installProcessSettings();
    dal::installCensus();

    Options opt;
    if (!parseArgs(argc, argv, opt)) {
        printUsage();
        return static_cast<int>(dal::ExitCode::Usage);
    }

    std::error_code ec;
    dal::VfsCliOptions cli;
    if (!opt.game_dir.empty()) {
        const std::filesystem::path game_dir(opt.game_dir);
        if (!std::filesystem::is_directory(game_dir, ec)) {
            std::fprintf(stderr, "error: game directory %s does not exist\n", opt.game_dir.c_str());
            return static_cast<int>(dal::ExitCode::Usage);
        }
        cli.game_dir = game_dir;
    }
    if (!opt.hdd_dir.empty()) {
        cli.hdd_dir = std::filesystem::path(opt.hdd_dir);
    }
    if (!opt.sdata_cache.empty()) {
        cli.sdata_cache = std::filesystem::path(opt.sdata_cache);
    }

    if (!opt.log_file.empty() && !dal::setLogFile(opt.log_file)) {
        std::fprintf(stderr, "error: cannot open log file %s\n", opt.log_file.c_str());
        return static_cast<int>(dal::ExitCode::Usage);
    }

    // Precedence: command-line options, then the configuration file, then the defaults.
    std::optional<dal::VfsConfigFile> vfs_file;
    if (!opt.vfs_config.empty()) {
        auto loaded = dal::loadVfsConfig(std::filesystem::path(opt.vfs_config));
        if (!loaded) {
            dal::logError("vfs config: {}", loaded.error());
            return static_cast<int>(dal::ExitCode::LoadError);
        }
        for (const std::string& warning : loaded->warnings) {
            dal::logWarn("vfs config: {}", warning);
        }
        vfs_file = std::move(*loaded);
    }
    auto layout = dal::composeVfsLayout(vfs_file ? &*vfs_file : nullptr, cli);
    if (!layout) {
        dal::logError("vfs: {}", layout.error());
        return static_cast<int>(dal::ExitCode::LoadError);
    }
    applyBuildDirDefaults(*layout);
    dal::configureVfs(std::move(*layout));

    std::filesystem::path elf_path(opt.elf);
    if (opt.elf.empty()) {
        bool read_only = false;
        const auto default_elf = dal::resolveGuestPath("/dev_bdvd/PS3_GAME/USRDIR/EBOOT.elf", read_only);
        if (!default_elf) {
            dal::logError("no executable: pass --elf or --game-dir, or mount /dev_bdvd in the VFS config");
            return static_cast<int>(dal::ExitCode::LoadError);
        }
        elf_path = *default_elf;
    }

    if (!opt.png_dump.empty()) {
        std::filesystem::create_directories(opt.png_dump, ec);
        dal::setPngDumpDirectory(std::filesystem::path(opt.png_dump));
    }

    loadGameParamSfo();
    if (!opt.rsx_census.empty()) dal::setRsxArgCensusFile(opt.rsx_census);
    if (!opt.rsx_capture_frames.empty()) {
        dal::setRsxCaptureFrames(opt.rsx_capture_frames, std::filesystem::path("generated") / "rsx" / "captures");
    }

    dal::installFaultHandler();
    dal::Process& process = dal::process();
    process.strict = opt.strict;
    if (!process.memory.reserve()) {
        dal::logError("cannot reserve the guest address space");
        return static_cast<int>(dal::ExitCode::Fatal);
    }

    const auto loaded = dal::loadExecutable(elf_path);
    if (!loaded) {
        dal::logError("load failed: {}", loaded.error());
        return static_cast<int>(dal::ExitCode::LoadError);
    }
    const dal::LoadedExecutable& exe = *loaded;

    const std::string_view expected_sha256 = kDalPpuExecutableSha256;
    if (exe.sha256 != expected_sha256) {
        dal::logError("executable hash {} does not match the generated code ({})", exe.sha256, expected_sha256);
        return static_cast<int>(dal::ExitCode::LoadError);
    }

    dalRegisterFuncs(kDalPpuFuncMappings, kDalPpuFuncMappingCount);
    dal::setQuietHle(opt.quiet_hle);
    dal::installImports(kDalPpuImports, kDalPpuImportCount);

    dal::logInfo(
            "dal: loaded {}: entry {:#010x} toc {:#010x} opd {:#010x} tls {:#010x}/{}/{:#x} sdk {:#010x} "
            "prio {} stacksize {:#x} malloc {:#x} ppc_seg {:#x} image end {:#010x}",
            elf_path.filename().string(), exe.entry_func, exe.entry_toc, exe.entry_opd, exe.tls_vaddr, exe.tls_filesz,
            exe.tls_memsz, exe.sdk_version, exe.primary_prio, exe.primary_stacksize, exe.malloc_pagesize, exe.ppc_seg,
            exe.image_end);
    dal::logInfo("dal: sha256 {}", exe.sha256);

    if (opt.load_only) {
        return static_cast<int>(dal::ExitCode::ProcessExit);
    }

    installMovieCodecs();
    installAtracDecoder();

    // Held until the process ends (runSupervisor does not return); the decode loops of the game poll
    // with 1 ms sleeps, which the default tick would stretch to 15 ms.
    const dal::TimerResolution timer_resolution;
    if (timer_resolution.active()) {
        const auto period_us = dal::currentTimerPeriodUs();
        dal::logDebug("dal: timer: requested {} ms, system period {}", timer_resolution.periodMs(),
                      period_us ? std::format("{:.3f} ms", *period_us / 1000.0) : std::string("unknown"));
    } else {
        dal::logWarn("dal: timer: the 1 ms resolution request was refused; timed waits use the default tick");
    }

    // The window, script and gamepad threads share `opt.keyboard`; the executor factory below copies
    // the options, so it is created first.
    if (padUsesKeyboard(opt.pad) || padUsesGamepad(opt.pad)) {
        opt.keyboard = std::make_shared<dal::KeyboardPadState>();
        // With the keyboard in the pad, port 0 is always there; a gamepad-only pad follows the hardware.
        dal::setPadProvider(dal::makeKeyboardPadProvider(opt.keyboard, {}, {.always_connected = opt.pad != "gamepad"}));
        std::string where;
        if (opt.window) where += " (window)";
        if (!opt.pad_script.empty()) {
            where += std::format(" (script {}, {} events)", std::filesystem::path(opt.pad_script).filename().string(),
                                 opt.pad_script_events.size());
        }
        dal::logInfo("dal: pad: {}{}", opt.pad, where);
    } else {
        dal::setPadBackend(opt.pad);
    }

    configureAudio(opt);

    // The device is created on the RSX thread when it starts, so it belongs to that thread.
    if (opt.render_vulkan) {
        dal::setRsxExecutorFactory([opt](std::string& error) { return createVulkanExecutor(opt, error); });
    }

    // RPCS3 passes this VFS path as argv[0] (`Emu/System.cpp` `Emulator::Load`, "Fake arg").
    // The script clock starts here so that it matches --seconds within a few milliseconds.
    if (opt.keyboard && !opt.pad_script.empty()) dal::startPadScript(opt.pad_script_events, opt.keyboard);
    // GameInput takes about a second to initialise, so the gamepad thread creates the backend itself.
    // The probe alone (no gamepad in the pad) runs the thread without a state to write.
    if (padUsesGamepad(opt.pad) || opt.pad_probe) {
        dal::startGamepadHub(
                []() -> std::expected<std::shared_ptr<dal::GamepadBackend>, std::string> {
                    return dal::makeGameInputBackend({});
                },
                padUsesGamepad(opt.pad) ? opt.keyboard : nullptr,
                {.is_foreground = dal::processOwnsForegroundWindow,
                 .detect_steam = dal::detectSteamContext,
                 .probe = opt.pad_probe});
    }
    dal::bootMainThread(exe, "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN");
    dal::runSupervisor(opt.seconds);
}
