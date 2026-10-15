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
#include "render/render_scale.h"
#include "rsx/rsx_thread.h"

namespace tox2 {

struct GameOptions {
    std::filesystem::path game_dir, elf, libsre, hdd_dir, sdata_cache, vfs_config, png_dump, rsx_census, log_file,
            pad_script, audio_dump, dump_frames, shader_cache;
    std::optional<LogLevel> log_level;  // applied by applyGameOptions
    std::string dialog_answer;          // validated; applied by applyGameOptions
    std::string pad;                    // settled: none|neutral|keyboard|gamepad|keyboard+gamepad
    std::string audio;                  // settled: none|wasapi
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
    std::vector<std::uint32_t> rsx_capture_frames;
    std::vector<GuestDumpSpec> dumps;
};

struct OptionsError {
    int exit_code = 2;        // ExitCode::Usage
    std::string message;      // complete, one or more lines, each ending in '\n'
    bool print_usage = true;  // command-line mistakes
};

// The persisted and typed description of every option, in the order value errors are reported.
std::span<const OptionSpec> gameOptionSpecs();
std::string_view gameOptionsUsage();

// `args` excludes argv[0] and is in the ANSI code page, as argv is. Reads the files named by --pad-script and
// --audio-dump (the latter is created and truncated, to turn a bad path into a usage error); nothing else is
// touched, so the process state only changes in applyGameOptions.
std::expected<GameOptions, OptionsError> parseGameOptions(std::span<const std::string> args);

// Log level first, then the dialog answer: the settings that take effect in the process at once.
void applyGameOptions(const GameOptions& options);

}  // namespace tox2
