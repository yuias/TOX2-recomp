// tox2_game start-up options: the option table, the command-line parser and the validated result.
// Kept out of game_main.cpp so that it builds and is tested without the generated tree.

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "app/config_file.h"
#include "core/guest_dump.h"
#include "core/log.h"
#include "core/threads.h"
#include "input/pad_script.h"
#include "render/frame_gen.h"
#include "render/render_scale.h"
#include "render/vk_device_choice.h"
#include "render/window_size.h"
#include "rsx/rsx_thread.h"
#include "spu/spu_capture.h"

namespace tox2 {

// The device output --audio can select besides "none": WASAPI on Windows, CoreAudio on macOS.
#if defined(_WIN32)
inline constexpr std::string_view kHostAudioOutput = "wasapi";
#elif defined(__APPLE__)
inline constexpr std::string_view kHostAudioOutput = "coreaudio";
#else
#error "no host audio output for this platform"
#endif

// The present mode requested while frame generation is loaded. Kept here, apart from VkPresentModeKHR, so that option
// parsing does not depend on Vulkan; game_main maps it.
enum class FgPresentMode : std::uint8_t { Fifo, Mailbox, Immediate };

// How a save data dialog is answered: Auto and Cancel never show anything, Window is the interactive dialog.
enum class SaveDialogMode : std::uint8_t { Auto, Cancel, Window };

struct GameOptions {
    std::filesystem::path game_dir, elf, libsre, hdd_dir, sdata_cache, vfs_config, png_dump, rsx_census, log_file,
            pad_script, audio_dump, dump_frames, shader_cache, ui_font;
    std::optional<LogLevel> log_level;  // applied by applyGameOptions
    std::string dialog_answer;          // validated; applied by applyGameOptions
    std::string pad;                    // settled: none|neutral|keyboard|gamepad|keyboard+gamepad
    std::string audio;                  // settled: none|kHostAudioOutput
    // --save-dialog as given; unset when absent, because the default depends on the window and the pad, which
    // game_main resolves (a requested Window may still fall back to Auto there).
    std::optional<SaveDialogMode> save_dialog;
    PadScript pad_script_data;
    unsigned seconds = 0;
    bool strict = false, quiet_hle = false, load_only = false, pad_probe = false, vk_validation = false,
         no_window = false, lwmutex_watch = false;
    bool render_vulkan = false;  // settled
    bool window = false;         // settled
    RsxFlipLimit frame_limit = RsxFlipLimit::Vsync;
    bool host_priorities = true, yield_backoff = true, lwmutex_fast_path = true;
    PpuThreadsMode ppu_threads = PpuThreadsMode::Split2;
    std::uint32_t dump_frames_every = 1;
    std::uint32_t resolution_scale = 1;
    AaMode aa_mode = AaMode::Msaa;
    std::uint32_t anisotropy = 1;  // 1 = off, else 2, 4, 8, 16
    VkDriverChoice vk_driver = VkDriverChoice::Auto;
    WindowSizePreset window_size = WindowSizePreset::Size720p;
    bool fullscreen = false;
    FrameGenTarget frame_gen = FrameGenTarget::Off;
    // Frame generation diagnostics; each needs frame_gen other than Off. Mailbox is the default because DLSS-G requires
    // vsync off; Fifo is only a diagnostic.
    FgPresentMode fg_present_mode = FgPresentMode::Mailbox;
    bool fg_reflex = true;
    bool fg_only_generated = false;
    std::uint32_t fg_cycle_seconds = 0;  // 0 = off, else 1..600
    std::vector<std::uint32_t> rsx_capture_frames;
    std::vector<GuestDumpSpec> dumps;
    // SPU span capture (debug): the spans to capture and where, when and how many files to write. Settled: the
    // selectors come with a directory.
    std::vector<SpuCaptureSelector> spu_captures;
    SpuCaptureSettings spu_capture;

    bool no_config = false;               // --no-config was given
    std::filesystem::path config_file;    // absolute; the file that was read, empty when none
    std::filesystem::path settings_file;  // absolute; where changes are saved, empty when they are not
    std::vector<std::string> config_warnings;
    // Keys of the file that were left out because the run lacks the renderer or the window they need. Logged by
    // applyGameOptions, because the log level is not applied while parsing.
    std::vector<std::string> config_ignored;
};

// Where config.json is looked for when neither --config nor --no-config is given.
struct ConfigSearch {
    std::filesystem::path working_dir;     // checked first
    std::filesystem::path executable_dir;  // checked second, and where a new file is created; may be empty
};

struct OptionsError {
    int exit_code = 2;        // ExitCode::Usage
    std::string message;      // complete, one or more lines, each ending in '\n'
    bool print_usage = true;  // command-line mistakes; false for config file problems
};

// The persisted and typed description of every option, in the order value errors are reported.
std::span<const OptionSpec> gameOptionSpecs();
std::string_view gameOptionsUsage();

bool padUsesKeyboard(std::string_view pad);
bool padUsesGamepad(std::string_view pad);

// `args` excludes argv[0] and is in the ANSI code page, as argv is; the config file is UTF-8. Reads the config
// file and the files named by --pad-script and --audio-dump (the latter is created and truncated, to turn a bad
// path into a usage error); nothing else is touched, so the process state only changes in applyGameOptions.
// A relative --config is resolved against `search.working_dir`.
std::expected<GameOptions, OptionsError> parseGameOptions(std::span<const std::string> args,
                                                          const ConfigSearch& search);

// Log level first, then the dialog answer, then the config lines.
void applyGameOptions(const GameOptions& options);

}  // namespace tox2
