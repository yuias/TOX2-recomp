// tox2_game: loads the decrypted EBOOT.elf and runs it through the generated PPU code.

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <string_view>
#include <vector>

#include "app/game_options.h"
#include "app/settings_store.h"
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
#include "core/host_priority.h"
#include "core/log.h"
#include "core/param_sfo.h"
#include "core/perf_log.h"
#include "core/reservation.h"
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
#include "lv2/sys_fs.h"
#include "lv2/sys_sync.h"
#include "lv2/syscalls.h"
#include "lv2/vfs.h"
#include "lv2/vfs_config.h"
#include "media/atrac_decoder.h"
#include "media/ffmpeg_codecs.h"
#include "media/movie_decoder.h"
#include "overlay/debug_overlay.h"
#include "overlay/overlay_sources.h"
#include "render/render_scale.h"
#include "render/vk_context.h"
#include "render/vk_renderer.h"
#include "render/vk_window.h"
#include "rsx/rsx_thread.h"
#include "spu/spu_census.h"
#include "spu/spu_thread.h"

namespace {

// Guest addresses in the BLJS10188 EBOOT, read only for the per-second perf line.
// Time block: f32 dt at +0, u64 render hand-off job counters at +0x120/+0x128/+0x130.
constexpr std::uint32_t kGameTimeBlock = 0x116abd0;
constexpr std::uint32_t kGameTimeBlockBytes = 0x138;
constexpr std::uint32_t kGameJobCounters = 0x120;
// Frame block: u32 frame interval at +0 (1 = 60 fps, 2 = 30 fps), u64 frame counter at +8.
constexpr std::uint32_t kGameFrameBlock = 0x10cd670;
constexpr std::uint32_t kGameFrameBlockBytes = 0x10;

// Range of the game's dt over the flips of the last perf interval. Written by the RSX thread, drained by the perf
// thread; a flip racing the drain lands in either interval.
class GameDtWindow {
public:
    void add(float dt) {
        float cur = min_.load(std::memory_order_relaxed);
        while (dt < cur && !min_.compare_exchange_weak(cur, dt, std::memory_order_relaxed)) {
        }
        cur = max_.load(std::memory_order_relaxed);
        while (dt > cur && !max_.compare_exchange_weak(cur, dt, std::memory_order_relaxed)) {
        }
    }

    // Returns false when no flip was sampled.
    bool take(float& min, float& max) {
        min = min_.exchange(std::numeric_limits<float>::infinity(), std::memory_order_relaxed);
        max = max_.exchange(-std::numeric_limits<float>::infinity(), std::memory_order_relaxed);
        return min <= max;
    }

private:
    std::atomic<float> min_{std::numeric_limits<float>::infinity()};
    std::atomic<float> max_{-std::numeric_limits<float>::infinity()};
};

GameDtWindow& gameDtWindow() {
    static GameDtWindow window;
    return window;
}

// False until the EBOOT is loaded (or when the images differ).
bool gameTimeMapped() {
    const tox2::GuestMemory& memory = tox2::process().memory;
    return memory.isCommitted(kGameTimeBlock, kGameTimeBlockBytes) &&
           memory.isCommitted(kGameFrameBlock, kGameFrameBlockBytes);
}

float readGameDt() { return std::bit_cast<float>(tox2Read32(tox2::process().memory.base(), kGameTimeBlock)); }

// The Vulkan device and the renderer for the RSX thread. Built and destroyed on that thread; the
// counters are published for the census, which reads them from another one.
class VulkanExecutor final : public tox2::RsxExecutor {
public:
    // `presenter` and `overlay` may be null (no window).
    VulkanExecutor(std::unique_ptr<tox2::VkContext> context, std::unique_ptr<tox2::Presenter> presenter,
                   std::unique_ptr<tox2::DebugOverlay> overlay, const tox2::RenderOptions& options)
        : context_(std::move(context)),
          presenter_(std::move(presenter)),
          overlay_(std::move(overlay)),
          renderer_(std::make_unique<tox2::RsxRenderer>(*context_, options)) {
        renderer_->setPresenter(presenter_.get());
        renderer_->setPresentOverlay(overlay_.get());
    }

    ~VulkanExecutor() override {
        // The renderer and the swapchain go before the device they were created from. The presenter's
        // destructor joins the window thread, which calls into the overlay, so the overlay outlives it.
        renderer_.reset();
        presenter_.reset();
        overlay_.reset();
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
        if (stats.flips != flips_.load(std::memory_order_relaxed) && gameTimeMapped()) {
            gameDtWindow().add(readGameDt());
        }
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
    std::unique_ptr<tox2::DebugOverlay> overlay_;
    std::unique_ptr<tox2::RsxRenderer> renderer_;
    std::atomic<std::uint64_t> draws_{0}, skipped_{0}, clears_{0}, flips_{0};
    std::atomic<bool> failed_{false};
};

// The process ends without running destructors on the stop paths, so the pending settings are written from a
// stop hook. The store is created only when changes can be saved (not with --no-config). It is never destroyed:
// a stop can arrive after main has returned and static destruction has begun, and the hook must not reach a
// freed object.
tox2::SettingsStore* g_settings_store = nullptr;

// Config keys of the overlay's saveable settings. Only changes the user makes in the overlay get here, so a value
// given on the command line is never written unless the user changes that setting.
void saveOverlaySetting(tox2::OverlaySetting setting, std::string_view value) {
    if (g_settings_store == nullptr) return;
    switch (setting) {
        case tox2::OverlaySetting::FrameLimit: g_settings_store->set("frame_limit", value); break;
        case tox2::OverlaySetting::ResolutionScale: g_settings_store->set("resolution_scale", value); break;
        case tox2::OverlaySetting::AaMode: g_settings_store->set("aa_mode", value); break;
        case tox2::OverlaySetting::Anisotropy: g_settings_store->set("anisotropic_filter", value); break;
        case tox2::OverlaySetting::WindowSize: g_settings_store->set("window_size", value); break;
    }
}

// Runs on the RSX thread; a null result makes the consumer continue without rendering.
std::unique_ptr<tox2::RsxExecutor> createVulkanExecutor(const tox2::GameOptions& opt,
                                                        const std::shared_ptr<tox2::KeyboardPadState>& keyboard,
                                                        std::string& error) {
    const auto start = std::chrono::steady_clock::now();
    tox2::VkContextOptions context_options;
    context_options.validation = opt.vk_validation;
    context_options.window = opt.window;
    // Always loaded: availability is fixed at context creation and the overlay can switch to DLAA later.
    context_options.streamline = true;
    // Frame generation is a property of presentation: without a window nothing is presented.
    context_options.frame_generation = opt.window;
    std::unique_ptr<tox2::VkContext> context = tox2::createVkContext(context_options, error);
    if (!context) return nullptr;
    const std::uint32_t api = context->deviceProperties().apiVersion;
    tox2::logInfo("tox2: render: vulkan device {} (api {}.{}.{}, d24s8 {})", context->deviceName(),
                  VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api),
                  context->hasD24S8() ? "yes" : "no");
    if (context->streamline() != nullptr) {
        tox2::logInfo("tox2: render: DLAA available (Streamline {})", context->streamline()->version());
    } else {
        tox2::logInfo("tox2: render: DLAA unavailable: {}", context->streamlineStatus());
    }
    if (!opt.window) {
        tox2::logInfo("tox2: render: frame generation unavailable: no window");
    } else if (context->frameGenerationSupported()) {
        tox2::logInfo("tox2: render: frame generation available ({})", context->frameGenerationStatus());
    } else {
        tox2::logInfo("tox2: render: frame generation unavailable: {}", context->frameGenerationStatus());
    }
    if (opt.vk_validation && !context->validationEnabled()) {
        tox2::logWarn("tox2: render: validation layer requested but not installed");
    }

    tox2::RenderOptions render_options;
    render_options.dump_dir = opt.dump_frames;
    render_options.dump_every = opt.dump_frames_every;
    render_options.resolution_scale = opt.resolution_scale;
    render_options.aa_mode = opt.aa_mode;
    render_options.anisotropy = opt.anisotropy;
    render_options.shader_cache =
            opt.shader_cache.empty() ? std::filesystem::path("generated") / "shader_cache" : opt.shader_cache;
    // Requested PNG dumps follow --dump-frames so they land next to the periodic ones.
    if (!opt.dump_frames.empty()) render_options.requested_dump_dir = opt.dump_frames;
    // Declared before the presenter so that on every return path the presenter (and its window thread, which
    // calls into the overlay) is destroyed first. Only created with a window.
    std::unique_ptr<tox2::DebugOverlay> overlay;
    std::unique_ptr<tox2::Presenter> presenter;
    if (opt.window) {
        tox2::OverlaySources overlay_sources = tox2::makeRuntimeOverlaySources();
        overlay_sources.setting_changed = saveOverlaySetting;
        overlay = std::make_unique<tox2::DebugOverlay>(*context, std::move(overlay_sources));
        tox2::PresenterOptions presenter_options;
        presenter_options.on_message = [overlay = overlay.get()](const tox2::WindowMessage& message) {
            return overlay->onWindowMessage(message);
        };
        presenter_options.frame_slots = tox2::RsxRenderer::kFramesInFlight;
        presenter_options.window_size = opt.window_size;
        presenter_options.fullscreen = opt.fullscreen;
        presenter_options.on_fullscreen_changed = [](bool fullscreen) {
            if (g_settings_store != nullptr) g_settings_store->set("fullscreen", fullscreen ? "on" : "off");
        };
        presenter_options.on_close = [] {
            tox2::stopProcess(tox2::ExitCode::ProcessExit, "tox2: stop: window closed");
        };
        if (keyboard && tox2::padUsesKeyboard(opt.pad)) {
            // These run on the window thread; the state is lock-free, so the guest never waits on it.
            // Focus loss releases the keyboard source only: the gamepad thread is the sole writer of its own.
            presenter_options.on_key = [keyboard](const tox2::KeyMessage& key) {
                keyboard->keyEvent(key.scan_code, key.down, key.repeat, tox2::KeyboardPadState::Clock::now());
                if (key.repeat) return;
                if (const std::optional<tox2::PadButton> button = tox2::padButtonForScanCode(key.scan_code)) {
                    tox2::logDebug("tox2: pad: key {:#x} {} -> {}", key.scan_code, key.down ? "down" : "up",
                                   tox2::padButtonName(*button));
                }
            };
            presenter_options.on_focus_lost = [keyboard] { keyboard->releaseAll(tox2::InputSource::Keyboard); };
        }
        presenter = tox2::Presenter::create(*context, std::move(presenter_options), error);
        if (!presenter) {
            error = "window: " + error;
            return nullptr;
        }
        overlay->attachWindow(presenter->nativeWindowHandle());
    }
    auto executor = std::make_unique<VulkanExecutor>(std::move(context), std::move(presenter), std::move(overlay),
                                                     render_options);
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
        lines.push_back(std::format(
                "audio input: {} periods, non-zero samples per channel {}, unwritten blocks {} (runs {}, longest {})",
                ports.periods, channels, ports.unwritten_blocks, ports.unwritten_runs, ports.longest_unwritten_run));
    }

    const tox2::CellAtracStats atrac = tox2::cellAtracStats();
    if (atrac.decoders != 0) {
        lines.push_back(
                std::format("atrac {} decoders, {} frames decoded ({} non-silent), {} errors, {} without decoder",
                            atrac.decoders, atrac.frames, atrac.non_silent, atrac.errors, atrac.without_decoder));
    }
    return lines;
}

// Interval deltas for the once-per-second perf line; each source keeps its previous snapshot because the perf thread
// is the only caller.
tox2::PerfSource audioPerfSource() {
    return [prev = tox2::CellAudioStats{}, prev_flips = std::uint64_t{0}]() mutable -> std::string {
        if (tox2::audioEngineStats().output.empty()) return {};  // cellAudioInit has not run
        const tox2::CellAudioStats now = tox2::cellAudioPorts().stats();
        const std::uint64_t flips = tox2::rsxCensusSnapshot().flips;
        std::string body = std::format("audio flips {} periods {} unwritten {} (runs {})", flips - prev_flips,
                                       now.periods - prev.periods, now.unwritten_blocks - prev.unwritten_blocks,
                                       now.unwritten_runs - prev.unwritten_runs);
        prev = now;
        prev_flips = flips;
        return body;
    };
}

// The game's own timing state, for judging whether dt and the render hand-off keep up with the frame interval.
// Snapshot reads of live guest memory without the kernel lock; the values are diagnostic only.
tox2::PerfSource gamePerfSource() {
    return []() -> std::string {
        if (!gameTimeMapped()) return {};
        const std::uint8_t* base = tox2::process().memory.base();
        std::string body = std::format("game dt {:.3f} interval {} frame {} jobs {}/{}/{}", readGameDt(),
                                       tox2Read32(base, kGameFrameBlock), tox2Read64(base, kGameFrameBlock + 8),
                                       tox2Read64(base, kGameTimeBlock + kGameJobCounters),
                                       tox2Read64(base, kGameTimeBlock + kGameJobCounters + 8),
                                       tox2Read64(base, kGameTimeBlock + kGameJobCounters + 0x10));
        float min = 0.0f, max = 0.0f;
        if (gameDtWindow().take(min, max)) body += std::format(" dt-at-flips min {:.3f} max {:.3f}", min, max);
        return body;
    };
}

// Where the RSX thread's time goes, per completed flip. Empty in a second without a flip.
tox2::PerfSource rsxPerfSource() {
    // The first interval starts when the source is created, not at thread start.
    return [prev = tox2::rsxThreadTimes(), prev_flips = tox2::rsxFlipCount()]() mutable -> std::string {
        const tox2::RsxThreadTimes now = tox2::rsxThreadTimes();
        const std::uint64_t flips = tox2::rsxFlipCount();
        const tox2::RsxThreadTimes delta{now.idle_us - prev.idle_us, now.lock_wait_us - prev.lock_wait_us,
                                         now.pump_us - prev.pump_us, now.build_us - prev.build_us,
                                         now.record_us - prev.record_us};
        const std::string body = tox2::formatRsxBusy(delta, flips - prev_flips);
        prev = now;
        prev_flips = flips;
        return body;
    };
}

bool isAudioPathClass(tox2::GuestLockClass cls) {
    return cls == tox2::GuestLockClass::Audio || cls == tox2::GuestLockClass::Spu ||
           cls == tox2::GuestLockClass::PpuHigh;
}

tox2::PerfSource lockPerfSource() {
    return [prev = tox2::GuestLockStats{}, prev_yields = tox2::PpuYieldStats{},
            prev_handoffs = tox2::SyncHandoffStats{}, prev_reservations = tox2::PpuReservationStats{},
            prev_slots = tox2::PpuSlotStats{}, prev_slow = tox2::SyncHandoffStats{}]() mutable -> std::string {
        const tox2::GuestLockStats now = tox2::guestLock().stats();
        std::string body;
        for (std::size_t i = 0; i < tox2::kGuestLockClassCount; i++) {
            const auto cls = static_cast<tox2::GuestLockClass>(i);
            const tox2::GuestLockClassStats& a = now.classes[i];
            const tox2::GuestLockClassStats& b = prev.classes[i];
            const tox2::LatencySnapshot wait = a.wait.since(b.wait);
            if (wait.count == 0) continue;
            const tox2::LatencySnapshot hold = a.hold.since(b.hold);
            body += std::format("{}{} {} hold p99 {:.2f} max {:.2f}", body.empty() ? "lock " : " | ",
                                tox2::guestLockClassName(cls), tox2::formatLatency(wait, tox2::kGuestLockWaitOverUs),
                                static_cast<double>(hold.percentileUs(0.99)) / 1000.0,
                                static_cast<double>(hold.max_us) / 1000.0);
            if (!isAudioPathClass(cls)) continue;
            // Mean queue length over the contended acquisitions only, the same denominator as the census.
            const std::uint64_t contended = a.contended - b.contended;
            const double ahead =
                    contended != 0 ? static_cast<double>(a.ahead_sum - b.ahead_sum) / static_cast<double>(contended)
                                   : 0.0;
            body += std::format(" ahead avg {:.1f}", ahead);
            bool first = true;
            for (std::size_t holder = 0; holder < tox2::kGuestLockClassCount; holder++) {
                const std::uint64_t us = a.waited_behind_us[holder] - b.waited_behind_us[holder];
                if (us == 0) continue;
                body += std::format("{} {} {:.2f}", first ? " behind" : "",
                                    tox2::guestLockClassName(static_cast<tox2::GuestLockClass>(holder)),
                                    static_cast<double>(us) / 1000.0);
                first = false;
            }
            if (!first) body += " ms";
        }
        const tox2::PpuYieldStats yields = tox2::ppuYieldStats();
        if (yields.calls != prev_yields.calls) {
            body += std::format("{}yield calls {} ok {} host {} kept {} backoff {}", body.empty() ? "lock " : " | ",
                                yields.calls - prev_yields.calls, yields.switched - prev_yields.switched,
                                yields.to_host - prev_yields.to_host, yields.kept - prev_yields.kept,
                                yields.backoffs - prev_yields.backoffs);
            prev_yields = yields;
        }
        const tox2::SyncHandoffStats handoffs = tox2::syncHandoffStats();
        if (handoffs.mutex_waits != prev_handoffs.mutex_waits ||
            handoffs.mutex_handoffs != prev_handoffs.mutex_handoffs ||
            handoffs.lwmutex_waits != prev_handoffs.lwmutex_waits ||
            handoffs.lwmutex_handoffs != prev_handoffs.lwmutex_handoffs) {
            body += std::format("{}lwmutex wait {} handoff {} mutex wait {} handoff {}", body.empty() ? "lock " : " | ",
                                handoffs.lwmutex_waits - prev_handoffs.lwmutex_waits,
                                handoffs.lwmutex_handoffs - prev_handoffs.lwmutex_handoffs,
                                handoffs.mutex_waits - prev_handoffs.mutex_waits,
                                handoffs.mutex_handoffs - prev_handoffs.mutex_handoffs);
            prev_handoffs = handoffs;
        }
        // Appended after the fields above, whose text scripts parse.
        if (now.long_holds != prev.long_holds) {
            body += std::format("{}long holds {}", body.empty() ? "lock " : " | ", now.long_holds - prev.long_holds);
        }
        const tox2::FsTransferStats fs = tox2::takeFsTransferStats();
        if (fs.reads != 0 || fs.writes != 0) {
            body += std::format("{}fs read {} {} bytes max {:.2f} ms write {} {} bytes max {:.2f} ms",
                                body.empty() ? "lock " : " | ", fs.reads, fs.read_bytes,
                                static_cast<double>(fs.read_max_us) / 1000.0, fs.writes, fs.write_bytes,
                                static_cast<double>(fs.write_max_us) / 1000.0);
        }
        // In the split modes the hold time is kernel sections, which this name would misreport; the slots field below
        // replaces it.
        if (!tox2::splitPpuMode() && now.ppu_hold_us != prev.ppu_hold_us) {
            body += std::format("{}ppu overlap {:.2f} of {:.2f} ms", body.empty() ? "lock " : " | ",
                                static_cast<double>(now.ppu_hold_overlap_us - prev.ppu_hold_overlap_us) / 1000.0,
                                static_cast<double>(now.ppu_hold_us - prev.ppu_hold_us) / 1000.0);
        }
        const tox2::PpuReservationStats reservations = tox2::ppuReservationStats();
        if (reservations.stwcx != prev_reservations.stwcx) {
            body += std::format("{}stwcx {} fail {}", body.empty() ? "lock " : " | ",
                                reservations.stwcx - prev_reservations.stwcx,
                                reservations.stwcx_failed - prev_reservations.stwcx_failed);
            prev_reservations = reservations;
        }
        if (tox2::splitPpuMode()) {
            const tox2::PpuSlotStats slots = tox2::ppuSlots().stats();
            body += std::format("{}slots par {:.2f} ms full {:.2f} ms", body.empty() ? "lock " : " | ",
                                static_cast<double>(slots.parallel_us - prev_slots.parallel_us) / 1000.0,
                                static_cast<double>(slots.full_waiting_us - prev_slots.full_waiting_us) / 1000.0);
            prev_slots = slots;
        }
        // Last, so the fields above keep their positions for the scripts that parse them. The uncontended calls count
        // nothing; these are the calls that left the compare-and-swap path.
        const std::uint64_t slow_locks = handoffs.lwmutex_slow_locks - prev_slow.lwmutex_slow_locks;
        const std::uint64_t slow_unlocks = handoffs.lwmutex_slow_unlocks - prev_slow.lwmutex_slow_unlocks;
        const std::uint64_t misaligned = handoffs.lwmutex_misaligned - prev_slow.lwmutex_misaligned;
        if (slow_locks != 0 || slow_unlocks != 0 || misaligned != 0) {
            body += std::format("{}lwmutex slow lock {} unlock {}", body.empty() ? "lock " : " | ", slow_locks,
                                slow_unlocks);
            if (misaligned != 0) body += std::format(" misaligned {}", misaligned);
            prev_slow = handoffs;
        }
        prev = now;
        return body;
    };
}

tox2::PerfSource spuPerfSource() {
    return [prev = std::map<std::uint32_t, tox2::SpuReceiveRow>{},
            prev_waits = tox2::SpuReservationWaits{}]() mutable -> std::string {
        std::string body;
        for (const tox2::SpuReceiveRow& row : tox2::spuReceiveSnapshot()) {
            const tox2::SpuReceiveRow& before = prev[row.id];
            const tox2::LatencySnapshot busy = row.busy.since(before.busy);
            const tox2::LatencySnapshot wait = row.wait.since(before.wait);
            if (busy.count != 0 || wait.count != 0) {
                body += std::format("{}spu {:#010x} {} busy {}; wait p99 {:.2f} max {:.2f}", body.empty() ? "" : " | ",
                                    row.id, row.name, tox2::formatLatency(busy, tox2::kSpuReceiveOverUs),
                                    static_cast<double>(wait.percentileUs(0.99)) / 1000.0,
                                    static_cast<double>(wait.max_us) / 1000.0);
            }
            prev[row.id] = row;
        }
        const tox2::SpuReservationWaits waits = tox2::spuReservationWaitSnapshot();
        const std::uint64_t lost = waits.lost_wait.count - prev_waits.lost_wait.count;
        if (waits.epoch != prev_waits.epoch || waits.timeout != prev_waits.timeout || lost != 0) {
            body += std::format("{}rsv-wait epoch {} timeout {} lost-after-timeout {}", body.empty() ? "" : " | ",
                                waits.epoch - prev_waits.epoch, waits.timeout - prev_waits.timeout, lost);
        }
        prev_waits = waits;
        return body;
    };
}

void configureAudio(const tox2::GameOptions& opt) {
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

void flushSettingsStore() {
    if (g_settings_store != nullptr) g_settings_store->flush(std::chrono::milliseconds(250));
}

}  // namespace

int main(int argc, char** argv) {
    tox2::installProcessSettings();
    tox2::installCensus();

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    std::error_code cwd_ec;
    const tox2::ConfigSearch search{std::filesystem::current_path(cwd_ec), tox2::executableDirectory()};
    auto parsed = tox2::parseGameOptions(args, search);
    if (!parsed) {
        const tox2::OptionsError& error = parsed.error();
        std::fputs(error.message.c_str(), stderr);
        if (error.print_usage) {
            const std::string_view usage = tox2::gameOptionsUsage();
            std::fwrite(usage.data(), 1, usage.size(), stderr);
        }
        return error.exit_code;
    }
    tox2::GameOptions opt = std::move(*parsed);
    tox2::applyGameOptions(opt);
    if (cwd_ec) {
        // Without a working directory the lookup silently falls back to the executable's directory.
        tox2::logWarn("tox2: config: no working directory ({}); only the executable's directory is searched",
                      cwd_ec.message());
    }

    std::error_code ec;
    tox2::VfsCliOptions cli;
    if (!opt.game_dir.empty()) {
        if (!std::filesystem::is_directory(opt.game_dir, ec)) {
            std::fprintf(stderr, "error: game directory %s does not exist\n", opt.game_dir.string().c_str());
            return static_cast<int>(tox2::ExitCode::Usage);
        }
        cli.game_dir = opt.game_dir;
    }
    if (!opt.hdd_dir.empty()) {
        cli.hdd_dir = opt.hdd_dir;
    }
    if (!opt.sdata_cache.empty()) {
        cli.sdata_cache = opt.sdata_cache;
    }

    if (!opt.log_file.empty() && !tox2::setLogFile(opt.log_file.string())) {
        std::fprintf(stderr, "error: cannot open log file %s\n", opt.log_file.string().c_str());
        return static_cast<int>(tox2::ExitCode::Usage);
    }

    if (!opt.settings_file.empty()) {
        g_settings_store = new tox2::SettingsStore(opt.settings_file, tox2::gameOptionSpecs());
        if (!tox2::addStopHook(&flushSettingsStore)) {
            tox2::logWarn("tox2: config: no free stop hook; settings changed just before the end may be lost");
        }
    }

    tox2::startPerfLog({audioPerfSource(), gamePerfSource(), rsxPerfSource(), lockPerfSource(), spuPerfSource()});

    // Precedence: command-line options, then config.json, then the --vfs-config file, then the defaults. Keys in
    // config.json act exactly like command-line options, so its game_dir overrides the /dev_bdvd mount of the
    // VFS file.
    std::optional<tox2::VfsConfigFile> vfs_file;
    if (!opt.vfs_config.empty()) {
        auto loaded = tox2::loadVfsConfig(opt.vfs_config);
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

    std::filesystem::path elf_path = opt.elf;
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
        tox2::setPngDumpDirectory(opt.png_dump);
    }

    loadGameParamSfo();
    tox2::setRsxFlipLimit(opt.frame_limit);
    tox2::setHostPrioritiesEnabled(opt.host_priorities);
    tox2::setYieldBackoffEnabled(opt.yield_backoff);
    tox2::setLwmutexFastPathEnabled(opt.lwmutex_fast_path);
    // Before boot creates the process lwmutexes, which the watch has to register.
    tox2::setLwmutexWatchEnabled(opt.lwmutex_watch);
    // Before any guest thread exists: the mode fixes the slot capacity and the lock rules those threads start with.
    tox2::setPpuThreadsMode(opt.ppu_threads);
    tox2::logInfo("tox2: ppu threads: {}", opt.ppu_threads == tox2::PpuThreadsMode::Legacy   ? "legacy"
                                           : opt.ppu_threads == tox2::PpuThreadsMode::Split1 ? "1"
                                                                                             : "2");
    if (!opt.rsx_census.empty()) tox2::setRsxArgCensusFile(opt.rsx_census.string());
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
            opt.libsre.empty() ? elf_path.parent_path() / "libsre.reloc.elf" : opt.libsre;
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

    // The host pad state, set for every --pad that reads host input (keyboard, gamepad, both). The window
    // thread, the script thread and the gamepad thread each write their own source of it. The executor
    // factory below copies it, so it is created first.
    std::shared_ptr<tox2::KeyboardPadState> keyboard;
    if (tox2::padUsesKeyboard(opt.pad) || tox2::padUsesGamepad(opt.pad)) {
        keyboard = std::make_shared<tox2::KeyboardPadState>();
        // With the keyboard in the pad, port 0 is always there; a gamepad-only pad follows the hardware.
        tox2::setPadProvider(tox2::makeKeyboardPadProvider(keyboard, {}, {.always_connected = opt.pad != "gamepad"}));
        std::string where;
        if (opt.window) where += " (window)";
        if (!opt.pad_script.empty()) {
            where += std::format(" (script {}, {} events)", opt.pad_script.filename().string(),
                                 opt.pad_script_data.events.size());
        }
        tox2::logInfo("tox2: pad: {}{}", opt.pad, where);
    } else {
        tox2::setPadBackend(opt.pad);
    }

    configureAudio(opt);

    // The device is created on the RSX thread when it starts, so it belongs to that thread.
    if (opt.render_vulkan) {
        tox2::setRsxExecutorFactory(
                [opt, keyboard](std::string& error) { return createVulkanExecutor(opt, keyboard, error); });
    }

    // RPCS3 passes this VFS path as argv[0] (`Emu/System.cpp` `Emulator::Load`, "Fake arg").
    // A seconds script starts here so that it matches --seconds within a few milliseconds; a flip script
    // follows the process-wide flip count.
    if (keyboard && !opt.pad_script.empty()) tox2::startPadScript(opt.pad_script_data, keyboard);
    // GameInput takes about a second to initialise, so the gamepad thread creates the backend itself.
    // The probe alone (no gamepad in the pad) runs the thread without a state to write.
    if (tox2::padUsesGamepad(opt.pad) || opt.pad_probe) {
        tox2::startGamepadHub(
                []() -> std::expected<std::shared_ptr<tox2::GamepadBackend>, std::string> {
                    return tox2::makeGameInputBackend({});
                },
                tox2::padUsesGamepad(opt.pad) ? keyboard : nullptr,
                {.is_foreground = tox2::processOwnsForegroundWindow,
                 .detect_steam = tox2::detectSteamContext,
                 .probe = opt.pad_probe});
    }
    tox2::bootMainThread(exe, "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN", module.image_end - module.base);
    tox2::runSupervisor(opt.seconds);
}
