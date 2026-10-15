// config.json support: reading the file into raw option values and rewriting it with changed settings.
// JSON types stay in config_file.cpp.

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tox2 {

enum class OptionKind : std::uint8_t {
    Path,        // JSON string; relative to the config file's directory
    String,      // JSON string, or a non-negative integer taken as its decimal text ("frame_limit": 60)
    Unsigned,    // JSON non-negative integer
    Flag,        // JSON true/false; false is the same as leaving the key out
    Switch,      // on|off options: JSON true/false or "on"/"off"
    FrameList,   // JSON string "300,600" or a non-empty array of positive integers
    StringList,  // JSON array of strings, one per command-line occurrence (dump-guest)
};

// What a persisted option needs to have an effect. A persisted key read from the file is dropped,
// without a message, when the run lacks it; on the command line the usual usage error stays.
enum class OptionNeeds : std::uint8_t { Nothing, Renderer, Window };

struct OptionSpec {
    std::string_view name;  // command-line name without "--", e.g. "resolution-scale"
    OptionKind kind;
    bool persisted = false;  // the runtime writes the key when the user changes the setting in the window
    OptionNeeds needs = OptionNeeds::Nothing;
};

// "resolution-scale" -> "resolution_scale".
std::string configKeyFor(std::string_view option_name);

struct ConfigValue {
    std::string option;              // "resolution-scale"
    std::string key;                 // "resolution_scale"
    std::string text;                // String, Unsigned (decimal), Switch ("on"/"off")
    bool flag = false;               // Flag
    std::filesystem::path path;      // Path: base_dir / value, lexically normalised; UTF-8 input
    std::vector<std::string> items;  // FrameList: one comma-joined entry; StringList: one per element
};

struct ConfigFileContents {
    std::vector<ConfigValue> values;    // document order
    std::vector<std::string> warnings;  // unknown key "x" ignored
};

// Errors are one line without the file name; the caller prefixes it.
std::expected<ConfigFileContents, std::string> parseConfigFile(std::string_view text,
                                                               const std::filesystem::path& base_dir,
                                                               std::span<const OptionSpec> specs);

struct ConfigChange {
    std::string key;    // a persisted key, e.g. "aa_mode"
    std::string value;  // command-line form, e.g. "msaa", "2", "on"
};

// The text of `original` with each change applied: an existing member keeps its position and gets the
// new value, a new member is appended. Every other member keeps its key, order and value. `original`
// empty or only whitespace counts as "{}". The output is yyjson's two-space pretty form with a final
// newline, UTF-8 without BOM. Value encoding by kind: Unsigned -> JSON integer, String -> JSON string,
// Switch -> true/false. Fails (and nothing must be written) when `original` is not valid JSON, its top
// level is not an object, it has a duplicate key, or a change names a key that is not persisted or has
// a value its kind cannot hold.
std::expected<std::string, std::string> updateConfigText(std::string_view original,
                                                         std::span<const ConfigChange> changes,
                                                         std::span<const OptionSpec> specs);

// Writes `<path>.tmp` and renames it over `path`, so a reader sees either the old or the new file.
// On failure the temp file is removed and `path` is untouched; the error names the step and the system
// message.
std::expected<void, std::string> writeFileAtomic(const std::filesystem::path& path, std::string_view text);

}  // namespace tox2
