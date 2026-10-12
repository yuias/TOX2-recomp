// tox2_game: loads the decrypted EBOOT.elf and runs it through the generated PPU code.

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
#include "core/dispatch.h"
#include "core/elf_loader.h"
#include "core/fatal.h"
#include "core/guest_dump.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "core/param_sfo.h"
#include "core/threads.h"
#include "core/timer_resolution.h"
#include "tox2/ppu_context.h"
#include "hle/cell_audio_ports.h"
#include "hle/hle.h"
#include "hle/libsre.h"
#include "input/gameinput_backend.h"
#include "input/gamepad_hub.h"
#include "input/keyboard_pad.h"
#include "input/pad_script.h"
#include "libsre_ppu_tables.h"
#include "lv2/syscalls.h"
#include "lv2/vfs.h"
#include "lv2/vfs_config.h"
#include "media/atrac_decoder.h"
#include "media/ffmpeg_codecs.h"
#include "media/movie_decoder.h"
#include "render/vk_context.h"
#include "render/vk_renderer.h"
#include "render/vk_window.h"
#include "rsx/rsx_thread.h"
#include "spu/spu_thread.h"

namespace {

struct Options {
    std::string game_dir;
    std::string elf;
    std::string libsre;
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
    std::vector<tox2::PadScriptEvent> pad_script_events;
    // The host pad state, set for every --pad that reads host input (keyboard, gamepad, both). The
    // window thread, the script thread and the gamepad thread each write their own source of it.
    std::shared_ptr<tox2::KeyboardPadState> keyboard;
    unsigned seconds = 0;
    bool strict = false;
    bool quiet_hle = false;
    bool load_only = false;
    bool render_vulkan = false;
    std::optional<bool> render_flag;  // --render as given; the final mode is settled after all arguments
    bool dump_every_given = false;
    bool vk_validation = false;
    bool no_window = false;
    bool frame_limit = true;  // --frame-limit 60 (default) or off
    bool window = false;      // settled after all arguments
    bool pad_probe = false;
    std::string dump_frames;
    std::uint32_t dump_frames_every = 1;
    std::string shader_cache;
    std::vector<std::uint32_t> rsx_capture_frames;
    std::vector<tox2::GuestDumpSpec> dumps;  // --dump-guest
};

bool padUsesKeyboard(std::string_view pad) { return pad == "keyboard" || pad == "keyboard+gamepad"; }
bool padUsesGamepad(std::string_view pad) { return pad == "gamepad" || pad == "keyboard+gamepad"; }

// The Vulkan device and the renderer for the RSX thread. Built and destroyed on that thread; the
// counters are published for the census, which reads them from another one.
class VulkanExecutor final : public tox2::RsxExecutor {
public:
    // `presenter` may be null (no window).
    VulkanExecutor(std::unique_ptr<tox2::VkContext> context, std::unique_ptr<tox2::Presenter> presenter,
                   const tox2::RenderOptions& options)
        : context_(std::move(context)),
          presenter_(std::move(presenter)),
          renderer_(std::make_unique<tox2::RsxRenderer>(*context_, options)) {
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
    const tox2::VkContext& context() const { return *context_; }

    void execute(std::span<tox2::RsxCommand> commands) override {
        if (!renderer_->valid()) return;
        renderer_->execute(commands);
        const tox2::RendererStats& stats = renderer_->stats();
        draws_.store(stats.draws, std::memory_order_relaxed);
        skipped_.store(stats.draws_skipped, std::memory_order_relaxed);
        clears_.store(stats.clears, std::memory_order_relaxed);
        flips_.store(stats.flips, std::memory_order_relaxed);
        if (!renderer_->valid()) {
            failed_.store(true, std::memory_order_relaxed);
            tox2::logError("tox2: render: renderer stopped: {}", renderer_->error());
        }
    }

    tox2::RsxRenderCounts counts() const override {
        tox2::RsxRenderCounts counts;
        counts.draws = draws_.load(std::memory_order_relaxed);
        counts.draws_skipped = skipped_.load(std::memory_order_relaxed);
        counts.clears = clears_.load(std::memory_order_relaxed);
        counts.flips = flips_.load(std::memory_order_relaxed);
        // The dump worker keeps its own atomic counters, so these are current without the RSX thread's help.
        const tox2::DumpCounts dumps = renderer_->dumpCounts();
        counts.frames_dumped = dumps.written;
        counts.dumps_skipped = dumps.skipped;
        counts.dumps_failed = dumps.failed;
        counts.validation = context_->validationEnabled();
        counts.validation_errors = context_->validationErrorCount();
        counts.failed = failed_.load(std::memory_order_relaxed);
        const tox2::PipelineCacheStats pipelines = renderer_->pipelineCacheStats();
        counts.pipelines_created = pipelines.created;
        counts.pipeline_create_us = pipelines.create_us;
        counts.pipeline_cache_loaded = pipelines.loaded_bytes;
        return counts;
    }

    std::uint64_t flushOutput(std::chrono::milliseconds timeout) override {
        if (!renderer_->savePipelineCache()) tox2::logWarn("tox2: render: pipeline cache not saved");
        return renderer_->flushDumps(timeout);
    }

private:
    std::unique_ptr<tox2::VkContext> context_;
    std::unique_ptr<tox2::Presenter> presenter_;
    std::unique_ptr<tox2::RsxRenderer> renderer_;
    std::atomic<std::uint64_t> draws_{0}, skipped_{0}, clears_{0}, flips_{0};
    std::atomic<bool> failed_{false};
};

// Runs on the RSX thread; a null result makes the consumer continue without rendering.
std::unique_ptr<tox2::RsxExecutor> createVulkanExecutor(const Options& opt, std::string& error) {
    const auto start = std::chrono::steady_clock::now();
    tox2::VkContextOptions context_options;
    context_options.validation = opt.vk_validation;
    context_options.window = opt.window;
    std::unique_ptr<tox2::VkContext> context = tox2::createVkContext(context_options, error);
    if (!context) return nullptr;
    const std::uint32_t api = context->deviceProperties().apiVersion;
    tox2::logInfo("tox2: render: vulkan device {} (api {}.{}.{}, d24s8 {})", context->deviceName(),
                  VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api),
                  context->hasD24S8() ? "yes" : "no");
    if (opt.vk_validation && !context->validationEnabled()) {
        tox2::logWarn("tox2: render: validation layer requested but not installed");
    }

    tox2::RenderOptions render_options;
    render_options.dump_dir = opt.dump_frames;
    render_options.dump_every = opt.dump_frames_every;
    render_options.shader_cache = opt.shader_cache.empty() ? std::filesystem::path("generated") / "shader_cache"
                                                           : std::filesystem::path(opt.shader_cache);
    std::unique_ptr<tox2::Presenter> presenter;
    if (opt.window) {
        tox2::PresenterOptions presenter_options;
        presenter_options.frame_slots = tox2::RsxRenderer::kFramesInFlight;
        presenter_options.on_close = [] {
            tox2::stopProcess(tox2::ExitCode::ProcessExit, "tox2: stop: window closed");
        };
        if (opt.keyboard && padUsesKeyboard(opt.pad)) {
            // These run on the window thread; the state is lock-free, so the guest never waits on it.
            // Focus loss releases the keyboard source only: the gamepad thread is the sole writer of its own.
            presenter_options.on_key = [keyboard = opt.keyboard](const tox2::KeyMessage& key) {
                keyboard->keyEvent(key.scan_code, key.down, key.repeat, tox2::KeyboardPadState::Clock::now());
                if (key.repeat) return;
                if (const std::optional<tox2::PadButton> button = tox2::padButtonForScanCode(key.scan_code)) {
                    tox2::logDebug("tox2: pad: key {:#x} {} -> {}", key.scan_code, key.down ? "down" : "up",
                                   tox2::padButtonName(*button));
                }
            };
            presenter_options.on_focus_lost = [keyboard = opt.keyboard] {
                keyboard->releaseAll(tox2::InputSource::Keyboard);
            };
        }
        presenter = tox2::Presenter::create(*context, std::move(presenter_options), error);
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
    tox2::logInfo("tox2: render: device and renderer ready in {} ms", init_ms);
    if (!render_options.dump_dir.empty()) {
        tox2::logInfo("tox2: render: dumping every {} flip(s) to {}", render_options.dump_every,
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
            "usage: tox2_game [--game-dir DIR] [--vfs-config FILE] (at least one) [--elf PATH]\n"
            "                [--libsre PATH]\n"
            "                [--seconds N] [--strict] [--log FILE] [--hdd-dir DIR] [--sdata-cache DIR]\n"
            "                [--log-level trace|debug|info|warn|error]\n"
            "                [--quiet-hle] [--load-only] [--png-dump DIR]\n"
            "                [--dialog-answer yes|no|ok] [--pad none|neutral|keyboard|gamepad|keyboard+gamepad]\n"
            "                [--pad-script FILE] [--pad-probe] [--audio none|wasapi] [--audio-dump FILE]\n"
            "                [--render none|vulkan] [--rsx-capture N[,M...]] [--rsx-census FILE]\n"
            "                [--dump-frames DIR] [--dump-frames-every K] [--shader-cache DIR]\n"
            "                [--vk-validation] [--no-window] [--frame-limit 60|off]\n"
            "                [--dump-guest ADDR:SIZE:FILE]...\n",
            stderr);
}

std::optional<tox2::LogLevel> parseLogLevel(std::string_view name) {
    if (name == "trace") return tox2::LogLevel::Trace;
    if (name == "debug") return tox2::LogLevel::Debug;
    if (name == "info") return tox2::LogLevel::Info;
    if (name == "warn") return tox2::LogLevel::Warn;
    if (name == "error") return tox2::LogLevel::Error;
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
        } else if (arg == "--libsre") {
            if (!value(opt.libsre)) return false;
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
        } else if (arg == "--dump-guest") {
            std::string text;
            if (!value(text)) return false;
            std::string error;
            auto spec = tox2::parseGuestDumpSpec(text, error);
            if (!spec) {
                std::fprintf(stderr, "error: --dump-guest %s: %s\n", text.c_str(), error.c_str());
                return false;
            }
            opt.dumps.push_back(std::move(*spec));
        } else if (arg == "--log") {
            if (!value(opt.log_file)) return false;
        } else if (arg == "--log-level") {
            std::string name;
            if (!value(name)) return false;
            const std::optional<tox2::LogLevel> level = parseLogLevel(name);
            if (!level) {
                std::fprintf(stderr, "error: --log-level needs trace, debug, info, warn or error\n");
                return false;
            }
            // Applied here so that nothing later in startup logs at the wrong level.
            tox2::setLogLevel(*level);
        } else if (arg == "--dialog-answer") {
            if (!value(opt.dialog_answer)) return false;
            if (!tox2::setDialogAnswer(opt.dialog_answer)) {
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
            auto events = tox2::loadPadScript(std::filesystem::path(opt.pad_script));
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
        } else if (arg == "--frame-limit") {
            std::string name;
            if (!value(name)) return false;
            if (name != "60" && name != "off") {
                std::fprintf(stderr, "error: --frame-limit needs 60 or off\n");
                return false;
            }
            opt.frame_limit = name == "60";
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
        const auto probe = tox2::WavWriter::create(opt.audio_dump, tox2::kAudioSampleRate, tox2::kAudioOutputChannels);
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
    std::unique_ptr<tox2::MediaCodecFactory> factory = tox2::createFfmpegCodecFactory(error);
    if (!factory) {
        tox2::logWarn("tox2: sail: codecs unavailable: {}", error);
        return;
    }
    tox2::setMediaCodecFactory(std::move(factory));
}

// cellAtrac sounds decode through the same DLLs. Without them every sound plays as silence of its
// own length and the stream protocol is unchanged.
void installAtracDecoder() {
    std::string error;
    std::unique_ptr<tox2::AtracDecoderFactory> factory = tox2::createFfmpegAtracDecoderFactory(error);
    if (!factory) {
        tox2::logWarn("tox2: atrac: no decoder: {}", error);
        return;
    }
    tox2::setAtracDecoderFactory(std::move(factory));
}

// Video and audio out report what the game's PARAM.SFO allows. A missing or broken file keeps the
// defaults of gameParamSfo().
void loadGameParamSfo() {
    bool read_only = false;
    const auto sfo_path = tox2::resolveGuestPath("/dev_bdvd/PS3_GAME/PARAM.SFO", read_only);
    if (!sfo_path) {
        tox2::logWarn("tox2: sfo: /dev_bdvd is not mounted; using the defaults");
        return;
    }
    auto sfo = tox2::readParamSfoFile(*sfo_path);
    if (!sfo) {
        tox2::logWarn("tox2: sfo: {}; using the defaults", sfo.error());
        return;
    }
    const auto title_id = sfo->strings.find("TITLE_ID");
    const auto resolution = sfo->ints.find("RESOLUTION");
    const auto sound_format = sfo->ints.find("SOUND_FORMAT");
    tox2::logInfo("tox2: sfo: TITLE_ID {} RESOLUTION {:#x} SOUND_FORMAT {:#x}",
                  title_id == sfo->strings.end() ? "?" : title_id->second,
                  resolution == sfo->ints.end() ? 0x1u : resolution->second,
                  sound_format == sfo->ints.end() ? 0x1u : sound_format->second);
    tox2::setGameParamSfo(std::move(*sfo));
}

// The census is printed on every stop path and the process then ends without running destructors,
// so this is also where an --audio-dump file gets its final header.
std::vector<std::string> audioCensusLines() {
    const tox2::AudioEngineStats engine = tox2::audioEngineStats();
    tox2::finishAudioDump();

    std::vector<std::string> lines;
    if (!engine.output.empty()) {  // otherwise cellAudioInit never ran
        const tox2::CellAudioStats ports = tox2::cellAudioPorts().stats();
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

    const tox2::CellAtracStats atrac = tox2::cellAtracStats();
    if (atrac.decoders != 0) {
        lines.push_back(
                std::format("atrac {} decoders, {} frames decoded ({} non-silent), {} errors, {} without decoder",
                            atrac.decoders, atrac.frames, atrac.non_silent, atrac.errors, atrac.without_decoder));
    }
    return lines;
}

void configureAudio(const Options& opt) {
    tox2::AudioEngineConfig config;
    // Only the wasapi choice builds a device output, so a run without it never touches COM.
    if (opt.audio == "wasapi") config.make_output = [] { return tox2::makeWasapiOutput(); };
    config.dump = opt.audio_dump;
    tox2::configureAudioEngine(std::move(config));
    tox2::setAudioCensusProvider(&audioCensusLines);
}

// tox2_game lives in build/<preset>/bin; a decrypted sdata cache placed next to that directory is
// found without --sdata-cache. The hdd directory defaults to the same place so that game data and
// saves persist between runs without --hdd-dir. Both apply only when neither the command line nor
// the VFS configuration chose one.
void applyBuildDirDefaults(tox2::VfsLayout& layout) {
    const std::filesystem::path exe_dir = tox2::executableDirectory();
    if (exe_dir.empty()) return;
    const std::filesystem::path build_dir = exe_dir.parent_path();
    std::error_code ec;
    if (!layout.sdata_cache) {
        const std::filesystem::path cache = build_dir / "sdata_cache";
        if (std::filesystem::is_directory(cache, ec)) {
            tox2::logInfo("tox2: vfs: sdata cache {} (default)", cache.generic_string());
            layout.sdata_cache = cache;
        }
    }
    if (std::ranges::find(layout.mounts, "dev_hdd0", &tox2::VfsMount::name) == layout.mounts.end()) {
        const std::filesystem::path hdd = build_dir / "hdd";
        tox2::logInfo("tox2: vfs: hdd directory {} (default)", hdd.generic_string());
        for (tox2::VfsMount& mount : tox2::hddDirMounts(hdd)) {
            std::erase_if(layout.mounts, [&](const tox2::VfsMount& m) { return m.name == mount.name; });
            layout.mounts.push_back(std::move(mount));
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    tox2::installProcessSettings();
    tox2::installCensus();

    Options opt;
    if (!parseArgs(argc, argv, opt)) {
        printUsage();
        return static_cast<int>(tox2::ExitCode::Usage);
    }

    std::error_code ec;
    tox2::VfsCliOptions cli;
    if (!opt.game_dir.empty()) {
        const std::filesystem::path game_dir(opt.game_dir);
        if (!std::filesystem::is_directory(game_dir, ec)) {
            std::fprintf(stderr, "error: game directory %s does not exist\n", opt.game_dir.c_str());
            return static_cast<int>(tox2::ExitCode::Usage);
        }
        cli.game_dir = game_dir;
    }
    if (!opt.hdd_dir.empty()) {
        cli.hdd_dir = std::filesystem::path(opt.hdd_dir);
    }
    if (!opt.sdata_cache.empty()) {
        cli.sdata_cache = std::filesystem::path(opt.sdata_cache);
    }

    if (!opt.log_file.empty() && !tox2::setLogFile(opt.log_file)) {
        std::fprintf(stderr, "error: cannot open log file %s\n", opt.log_file.c_str());
        return static_cast<int>(tox2::ExitCode::Usage);
    }

    // Precedence: command-line options, then the configuration file, then the defaults.
    std::optional<tox2::VfsConfigFile> vfs_file;
    if (!opt.vfs_config.empty()) {
        auto loaded = tox2::loadVfsConfig(std::filesystem::path(opt.vfs_config));
        if (!loaded) {
            tox2::logError("vfs config: {}", loaded.error());
            return static_cast<int>(tox2::ExitCode::LoadError);
        }
        for (const std::string& warning : loaded->warnings) {
            tox2::logWarn("vfs config: {}", warning);
        }
        vfs_file = std::move(*loaded);
    }
    auto layout = tox2::composeVfsLayout(vfs_file ? &*vfs_file : nullptr, cli);
    if (!layout) {
        tox2::logError("vfs: {}", layout.error());
        return static_cast<int>(tox2::ExitCode::LoadError);
    }
    applyBuildDirDefaults(*layout);
    tox2::configureVfs(std::move(*layout));

    std::filesystem::path elf_path(opt.elf);
    if (opt.elf.empty()) {
        bool read_only = false;
        const auto default_elf = tox2::resolveGuestPath("/dev_bdvd/PS3_GAME/USRDIR/EBOOT.elf", read_only);
        if (!default_elf) {
            tox2::logError("no executable: pass --elf or --game-dir, or mount /dev_bdvd in the VFS config");
            return static_cast<int>(tox2::ExitCode::LoadError);
        }
        elf_path = *default_elf;
    }

    if (!opt.png_dump.empty()) {
        std::filesystem::create_directories(opt.png_dump, ec);
        tox2::setPngDumpDirectory(std::filesystem::path(opt.png_dump));
    }

    loadGameParamSfo();
    tox2::setRsxFlipLimit(opt.frame_limit);
    if (!opt.rsx_census.empty()) tox2::setRsxArgCensusFile(opt.rsx_census);
    if (!opt.rsx_capture_frames.empty()) {
        tox2::setRsxCaptureFrames(opt.rsx_capture_frames, std::filesystem::path("generated") / "rsx" / "captures");
    }

    for (tox2::GuestDumpSpec& dump : opt.dumps) {
        if (!tox2::addGuestDump(std::move(dump))) {
            std::fputs("error: too many --dump-guest options\n", stderr);
            return static_cast<int>(tox2::ExitCode::Usage);
        }
    }

    tox2::installFaultHandler();
    tox2::Process& process = tox2::process();
    process.strict = opt.strict;
    if (!process.memory.reserve()) {
        tox2::logError("cannot reserve the guest address space");
        return static_cast<int>(tox2::ExitCode::Fatal);
    }

    const auto loaded = tox2::loadExecutable(elf_path);
    if (!loaded) {
        tox2::logError("load failed: {}", loaded.error());
        return static_cast<int>(tox2::ExitCode::LoadError);
    }
    const tox2::LoadedExecutable& exe = *loaded;

    const std::string_view expected_sha256 = kTox2PpuExecutableSha256;
    if (exe.sha256 != expected_sha256) {
        tox2::logError("executable hash {} does not match the generated code ({})", exe.sha256, expected_sha256);
        return static_cast<int>(tox2::ExitCode::LoadError);
    }

    // libsre sits directly above the executable, before anything else allocates from the main region.
    // By default it is the file the build wrote next to the executable.
    const std::filesystem::path libsre_path =
            opt.libsre.empty() ? elf_path.parent_path() / "libsre.reloc.elf" : std::filesystem::path(opt.libsre);
    if (!std::filesystem::is_regular_file(libsre_path, ec)) {
        tox2::logError(
                "libsre: cannot open {} (build with -Pup to produce generated/inputs/libsre.reloc.elf, or pass --libsre)",
                libsre_path.generic_string());
        return static_cast<int>(tox2::ExitCode::LoadError);
    }
    const auto loaded_module = tox2::loadModule(libsre_path, tox2::libsre::kBase, exe.image_end);
    if (!loaded_module) {
        tox2::logError("libsre: load failed: {}", loaded_module.error());
        return static_cast<int>(tox2::ExitCode::LoadError);
    }
    const tox2::LoadedModule& module = *loaded_module;
    const std::string_view libsre_expected_sha256 = kTox2LibsrePpuExecutableSha256;
    if (module.sha256 != libsre_expected_sha256) {
        tox2::logError("libsre hash {} does not match the generated code ({})", module.sha256, libsre_expected_sha256);
        return static_cast<int>(tox2::ExitCode::LoadError);
    }
    // The whole file is pinned by its hash; these checks catch an offset in the layout header that
    // drifted from the image. A long list has one root cause, so only the first few are logged.
    const std::vector<std::string> layout_errors = tox2::libsre::checkLibsreLayout(
            process.memory.base(), std::span(kTox2LibsrePpuImports, kTox2LibsrePpuImportCount),
            std::span(kTox2LibsrePpuExports, kTox2LibsrePpuExportCount));
    constexpr std::size_t kMaxLayoutMessages = 5;
    for (std::size_t i = 0; i < layout_errors.size() && i < kMaxLayoutMessages; ++i) {
        tox2::logError("libsre: layout: {}", layout_errors[i]);
    }
    if (layout_errors.size() > kMaxLayoutMessages) {
        tox2::logError("libsre: layout: {} more failed checks", layout_errors.size() - kMaxLayoutMessages);
    }
    bool libsre_ok = layout_errors.empty();
    if (module.base != tox2::libsre::kBase || module.toc != tox2::libsre::kToc ||
        module.image_end != tox2::libsre::kImageEnd) {
        tox2::logError("libsre: module base {:#010x} toc {:#010x} image end {:#010x} differ from the layout header",
                       module.base, module.toc, module.image_end);
        libsre_ok = false;
    }
    if (!libsre_ok) return static_cast<int>(tox2::ExitCode::LoadError);

    // The SPU tables hold hashes of the blobs inside the two images; a mismatch means they were emitted from
    // other inputs, so this stops before an SPU thread can run code the tables do not describe.
    tox2::registerSpuImages(kTox2SpuImages, kTox2SpuImageCount);
    if (!tox2::checkSpuImages(process.memory.base())) return static_cast<int>(tox2::ExitCode::LoadError);

    tox2RegisterFuncs(kTox2PpuFuncMappings, kTox2PpuFuncMappingCount);
    tox2::appendFuncs(kTox2LibsrePpuFuncMappings, kTox2LibsrePpuFuncMappingCount);
    tox2::setQuietHle(opt.quiet_hle);
    const tox2::GuestExports guest_exports[] = {{"libsre", kTox2LibsrePpuExports, kTox2LibsrePpuExportCount}};
    tox2::installImports(kTox2PpuImports, kTox2PpuImportCount, guest_exports);
    tox2::installModuleImports("libsre", kTox2LibsrePpuImports, kTox2LibsrePpuImportCount);
    tox2::setProcessSdkVersion(exe.sdk_version);

    tox2::logInfo(
            "tox2: loaded {}: entry {:#010x} toc {:#010x} opd {:#010x} tls {:#010x}/{}/{:#x} sdk {:#010x} "
            "prio {} stacksize {:#x} malloc {:#x} ppc_seg {:#x} image end {:#010x}",
            elf_path.filename().string(), exe.entry_func, exe.entry_toc, exe.entry_opd, exe.tls_vaddr, exe.tls_filesz,
            exe.tls_memsz, exe.sdk_version, exe.primary_prio, exe.primary_stacksize, exe.malloc_pagesize, exe.ppc_seg,
            exe.image_end);
    tox2::logInfo("tox2: sha256 {}", exe.sha256);
    tox2::logInfo("tox2: loaded {}: module {} {}.{} base {:#010x} toc {:#010x} image end {:#010x}",
                  libsre_path.filename().string(), module.name, unsigned{module.version_major},
                  unsigned{module.version_minor}, module.base, module.toc, module.image_end);
    tox2::logInfo("tox2: libsre: sha256 {}", module.sha256);
    tox2::logInfo("tox2: libsre: layout ok");

    if (opt.load_only) {
        return static_cast<int>(tox2::ExitCode::ProcessExit);
    }

    installMovieCodecs();
    installAtracDecoder();

    // Held until the process ends (runSupervisor does not return); the decode loops of the game poll
    // with 1 ms sleeps, which the default tick would stretch to 15 ms.
    const tox2::TimerResolution timer_resolution;
    if (timer_resolution.active()) {
        const auto period_us = tox2::currentTimerPeriodUs();
        tox2::logDebug("tox2: timer: requested {} ms, system period {}", timer_resolution.periodMs(),
                       period_us ? std::format("{:.3f} ms", *period_us / 1000.0) : std::string("unknown"));
    } else {
        tox2::logWarn("tox2: timer: the 1 ms resolution request was refused; timed waits use the default tick");
    }

    // The window, script and gamepad threads share `opt.keyboard`; the executor factory below copies
    // the options, so it is created first.
    if (padUsesKeyboard(opt.pad) || padUsesGamepad(opt.pad)) {
        opt.keyboard = std::make_shared<tox2::KeyboardPadState>();
        // With the keyboard in the pad, port 0 is always there; a gamepad-only pad follows the hardware.
        tox2::setPadProvider(
                tox2::makeKeyboardPadProvider(opt.keyboard, {}, {.always_connected = opt.pad != "gamepad"}));
        std::string where;
        if (opt.window) where += " (window)";
        if (!opt.pad_script.empty()) {
            where += std::format(" (script {}, {} events)", std::filesystem::path(opt.pad_script).filename().string(),
                                 opt.pad_script_events.size());
        }
        tox2::logInfo("tox2: pad: {}{}", opt.pad, where);
    } else {
        tox2::setPadBackend(opt.pad);
    }

    configureAudio(opt);

    // The device is created on the RSX thread when it starts, so it belongs to that thread.
    if (opt.render_vulkan) {
        tox2::setRsxExecutorFactory([opt](std::string& error) { return createVulkanExecutor(opt, error); });
    }

    // RPCS3 passes this VFS path as argv[0] (`Emu/System.cpp` `Emulator::Load`, "Fake arg").
    // The script clock starts here so that it matches --seconds within a few milliseconds.
    if (opt.keyboard && !opt.pad_script.empty()) tox2::startPadScript(opt.pad_script_events, opt.keyboard);
    // GameInput takes about a second to initialise, so the gamepad thread creates the backend itself.
    // The probe alone (no gamepad in the pad) runs the thread without a state to write.
    if (padUsesGamepad(opt.pad) || opt.pad_probe) {
        tox2::startGamepadHub(
                []() -> std::expected<std::shared_ptr<tox2::GamepadBackend>, std::string> {
                    return tox2::makeGameInputBackend({});
                },
                padUsesGamepad(opt.pad) ? opt.keyboard : nullptr,
                {.is_foreground = tox2::processOwnsForegroundWindow,
                 .detect_steam = tox2::detectSteamContext,
                 .probe = opt.pad_probe});
    }
    tox2::bootMainThread(exe, "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN", module.image_end - module.base);
    tox2::runSupervisor(opt.seconds);
}
