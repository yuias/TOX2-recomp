#pragma once

// The VFS configuration file (tox2_game --vfs-config) and its composition with the command-line
// options. Parsing is separate from installing the layout (configureVfs) so both are testable
// without touching the global VFS state.

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lv2/vfs.h"

namespace tox2 {

inline constexpr int kVfsConfigVersion = 1;

// A parsed configuration file. Host paths are absolute or relative to the working directory:
// relative ones in the file were resolved against the file's directory.
struct VfsConfigFile {
    // Explicit "mounts" plus the "game_dir" and "hdd_dir" shorthands, expanded.
    std::vector<VfsMount> mounts;
    std::vector<VfsFileOverride> files;
    std::optional<std::filesystem::path> sdata_cache;
    // Keys the parser did not recognise; the caller logs them.
    std::vector<std::string> warnings;
};

// The command-line options that take part in composition. They win over the configuration.
struct VfsCliOptions {
    std::optional<std::filesystem::path> game_dir;
    std::optional<std::filesystem::path> hdd_dir;
    std::optional<std::filesystem::path> sdata_cache;
};

// Parses and validates the JSON text of a configuration file; relative host paths resolve
// against `base_dir`. Checks syntax only, not whether the host paths exist. The error is a
// one-line message.
std::expected<VfsConfigFile, std::string> parseVfsConfig(std::string_view text, const std::filesystem::path& base_dir);

// Reads `file` and parses it with its directory as the base. Errors name the file.
std::expected<VfsConfigFile, std::string> loadVfsConfig(const std::filesystem::path& file);

// Merges the configuration (may be null) with the command-line options (--game-dir replaces the
// /dev_bdvd and /app_home mounts, --hdd-dir /dev_hdd0 and /dev_hdd1, --sdata-cache the cache) and
// checks the result against the host: read-only mounts and override files must exist, and a
// writable mount may not share or contain another mount's host directory.
std::expected<VfsLayout, std::string> composeVfsLayout(const VfsConfigFile* config, const VfsCliOptions& cli);

}  // namespace tox2
