// Self-checking tests for the config file reader and writer and for the command-line parser. No game data and no
// player config file are read.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "app/config_file.h"
#include "app/game_options.h"
#include "app/settings_store.h"
#include "core/log.h"

namespace {

int g_passed;
int g_failed;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            g_passed++;                                                            \
        } else {                                                                   \
            g_failed++;                                                            \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
        }                                                                          \
    } while (0)

namespace stdfs = std::filesystem;
using tox2::OptionKind;
using tox2::OptionNeeds;
using tox2::OptionSpec;

// The reader gets its table from the caller, so the tests use their own.
constexpr std::array<OptionSpec, 10> kSpecs = {{
        {"elf", OptionKind::Path},
        {"log-level", OptionKind::String},
        {"seconds", OptionKind::Unsigned},
        {"strict", OptionKind::Flag},
        {"yield-backoff", OptionKind::Switch},
        {"rsx-capture", OptionKind::FrameList},
        {"dump-guest", OptionKind::StringList},
        {"resolution-scale", OptionKind::Unsigned, true, OptionNeeds::Renderer},
        {"frame-limit", OptionKind::String, true, OptionNeeds::Nothing},
        {"fullscreen", OptionKind::Switch, true, OptionNeeds::Window},
}};

const stdfs::path kBase = "base_dir";

std::expected<tox2::ConfigFileContents, std::string> parse(std::string_view text) {
    return tox2::parseConfigFile(text, kBase, kSpecs);
}

// True when the text is rejected with an error that contains `needle`.
bool rejectedWith(std::string_view text, std::string_view needle) {
    const auto result = parse(text);
    return !result && result.error().find(needle) != std::string::npos;
}

// The JSON text of an absolute path with forward slashes, which JSON needs no escapes for.
std::string absoluteJsonPath(const stdfs::path& name) {
    const std::u8string text = stdfs::absolute(name).generic_u8string();
    return std::string(text.begin(), text.end());
}

void testKeyNames() {
    CHECK(tox2::configKeyFor("resolution-scale") == "resolution_scale");
    CHECK(tox2::configKeyFor("seconds") == "seconds");
    CHECK(tox2::configKeyFor("dump-frames-every") == "dump_frames_every");
    CHECK(tox2::configKeyFor("").empty());
}

void testEmptyObject() {
    const auto result = parse("{}");
    CHECK(result && result->values.empty() && result->warnings.empty());
    const auto spaced = parse(" \n{ }\n");
    CHECK(spaced && spaced->values.empty());
}

void testPaths() {
    const auto relative = parse(R"({"elf": "game/EBOOT.ELF"})");
    CHECK(relative && relative->values.size() == 1);
    if (relative && relative->values.size() == 1) {
        const tox2::ConfigValue& value = relative->values[0];
        CHECK(value.option == "elf" && value.key == "elf");
        CHECK(value.path == (kBase / "game" / "EBOOT.ELF").lexically_normal());
    }

    // Dot components are folded, and the result is relative to the file's directory.
    const auto dotted = parse(R"({"elf": "./a/../b.elf"})");
    CHECK(dotted && dotted->values.size() == 1 && dotted->values[0].path == (kBase / "b.elf").lexically_normal());

    const std::string absolute = absoluteJsonPath("abs_dir/x.elf");
    const auto abs_result = parse(R"({"elf": ")" + absolute + R"("})");
    CHECK(abs_result && abs_result->values.size() == 1);
    if (abs_result && abs_result->values.size() == 1) {
        CHECK(abs_result->values[0].path == stdfs::absolute("abs_dir/x.elf").lexically_normal());
    }

    // "caf\xC3\xA9" is UTF-8 for "cafe" with an acute accent.
    const auto utf8 = parse("{\"elf\": \"caf\xC3\xA9/\xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0.elf\"}");
    CHECK(utf8 && utf8->values.size() == 1);
    if (utf8 && utf8->values.size() == 1) {
        CHECK(utf8->values[0].path == (kBase / stdfs::path(u8"café") / stdfs::path(u8"ゲーム.elf")).lexically_normal());
    }
    // The same text spelled as JSON escapes gives the same path.
    const auto escaped = parse(R"({"elf": "café/ゲーム.elf"})");
    CHECK(escaped && utf8 && escaped->values.size() == 1 && utf8->values.size() == 1 &&
          escaped->values[0].path == utf8->values[0].path);

    CHECK(rejectedWith(R"({"elf": 3})", "\"elf\": expected a path string, got number"));
    CHECK(rejectedWith(R"({"elf": ["a"]})", "\"elf\": expected a path string, got array"));
    CHECK(rejectedWith(R"({"elf": true})", "\"elf\": expected a path string, got boolean"));
    CHECK(rejectedWith(R"({"elf": ""})", "\"elf\": the path is empty"));
    CHECK(rejectedWith(R"({"elf": "a\u0000b"})", "NUL"));
}

void testStrings() {
    const auto text = parse(R"({"log-level": "x"})");
    CHECK(text && text->values.empty() && text->warnings.size() == 1);  // the key is log_level

    const auto result = parse(R"({"log_level": "debug", "frame_limit": 60})");
    CHECK(result && result->values.size() == 2);
    if (result && result->values.size() == 2) {
        CHECK(result->values[0].text == "debug");
        CHECK(result->values[1].option == "frame-limit" && result->values[1].key == "frame_limit" &&
              result->values[1].text == "60");
    }
    const auto zero = parse(R"({"frame_limit": 0})");
    CHECK(zero && zero->values.size() == 1 && zero->values[0].text == "0");
    const auto empty = parse(R"({"log_level": ""})");
    CHECK(empty && empty->values.size() == 1 && empty->values[0].text.empty());

    CHECK(rejectedWith(R"({"frame_limit": -1})", "\"frame_limit\": expected a string or a non-negative whole number"));
    CHECK(rejectedWith(R"({"frame_limit": 1.5})", "\"frame_limit\": expected a string or a non-negative whole number"));
    CHECK(rejectedWith(R"({"frame_limit": true})", "got boolean"));
    CHECK(rejectedWith(R"({"frame_limit": ["60"]})", "got array"));
    CHECK(rejectedWith(R"({"frame_limit": {}})", "got object"));
}

void testUnsigned() {
    const auto result = parse(R"({"seconds": 12, "resolution_scale": 0})");
    CHECK(result && result->values.size() == 2);
    if (result && result->values.size() == 2) {
        CHECK(result->values[0].text == "12" && result->values[1].text == "0");
    }
    const auto big = parse(R"({"seconds": 18446744073709551615})");
    CHECK(big && big->values.size() == 1 && big->values[0].text == "18446744073709551615");

    CHECK(rejectedWith(R"({"seconds": -3})", "\"seconds\": expected a non-negative whole number, got number"));
    CHECK(rejectedWith(R"({"seconds": 2.5})", "\"seconds\": expected a non-negative whole number, got number"));
    CHECK(rejectedWith(R"({"seconds": 2.0})", "\"seconds\": expected a non-negative whole number, got number"));
    CHECK(rejectedWith(R"({"seconds": 1e3})", "\"seconds\": expected a non-negative whole number, got number"));
    CHECK(rejectedWith(R"({"seconds": 18446744073709551616})", "\"seconds\": expected a non-negative whole number"));
    CHECK(rejectedWith(R"({"seconds": "12"})", "\"seconds\": expected a non-negative whole number, got string"));
    CHECK(rejectedWith(R"({"seconds": true})", "got boolean"));
}

void testFlags() {
    const auto on = parse(R"({"strict": true})");
    CHECK(on && on->values.size() == 1 && on->values[0].flag);
    // false is reported as such; leaving the key out has the same effect for the caller.
    const auto off = parse(R"({"strict": false})");
    CHECK(off && off->values.size() == 1 && !off->values[0].flag);

    CHECK(rejectedWith(R"({"strict": "true"})", "\"strict\": expected true or false, got string"));
    CHECK(rejectedWith(R"({"strict": 1})", "\"strict\": expected true or false, got number"));
}

void testSwitches() {
    const auto result = parse(R"({"yield_backoff": true})");
    CHECK(result && result->values.size() == 1 && result->values[0].text == "on");
    const auto off = parse(R"({"yield_backoff": false})");
    CHECK(off && off->values.size() == 1 && off->values[0].text == "off");
    const auto word_on = parse(R"({"yield_backoff": "on"})");
    CHECK(word_on && word_on->values.size() == 1 && word_on->values[0].text == "on");
    const auto word_off = parse(R"({"yield_backoff": "off"})");
    CHECK(word_off && word_off->values.size() == 1 && word_off->values[0].text == "off");

    CHECK(rejectedWith(R"({"yield_backoff": "maybe"})", "\"yield_backoff\": expected true, false, \"on\" or \"off\""));
    CHECK(rejectedWith(R"({"yield_backoff": "ON"})", "\"yield_backoff\": expected true, false"));
    CHECK(rejectedWith(R"({"yield_backoff": 1})", "got number"));
}

void testFrameLists() {
    const auto text = parse(R"({"rsx_capture": "300,600"})");
    CHECK(text && text->values.size() == 1 && text->values[0].items == std::vector<std::string>{"300,600"});
    const auto array = parse(R"({"rsx_capture": [300, 600, 7]})");
    CHECK(array && array->values.size() == 1 && array->values[0].items == std::vector<std::string>{"300,600,7"});
    const auto single = parse(R"({"rsx_capture": [5]})");
    CHECK(single && single->values.size() == 1 && single->values[0].items == std::vector<std::string>{"5"});

    CHECK(rejectedWith(R"({"rsx_capture": []})", "\"rsx_capture\": expected a non-empty array"));
    CHECK(rejectedWith(R"({"rsx_capture": [0]})", "\"rsx_capture\"[0]: expected a positive whole number, got number"));
    CHECK(rejectedWith(R"({"rsx_capture": [1, -2]})", "\"rsx_capture\"[1]: expected a positive whole number"));
    CHECK(rejectedWith(R"({"rsx_capture": [1, 2.5]})", "\"rsx_capture\"[1]: expected a positive whole number"));
    CHECK(rejectedWith(R"({"rsx_capture": ["3"]})",
                       "\"rsx_capture\"[0]: expected a positive whole number, got string"));
    CHECK(rejectedWith(R"({"rsx_capture": 300})", "\"rsx_capture\": expected a string or a non-empty array"));
    CHECK(rejectedWith(R"({"rsx_capture": true})", "got boolean"));
}

void testStringLists() {
    const auto result = parse(R"({"dump_guest": ["a=1", "b=2"]})");
    CHECK(result && result->values.size() == 1);
    if (result && result->values.size() == 1) {
        CHECK(result->values[0].option == "dump-guest" &&
              result->values[0].items == std::vector<std::string>({"a=1", "b=2"}));
    }
    const auto empty = parse(R"({"dump_guest": []})");
    CHECK(empty && empty->values.size() == 1 && empty->values[0].items.empty());

    CHECK(rejectedWith(R"({"dump_guest": "a=1"})", "\"dump_guest\": expected an array of strings, got string"));
    CHECK(rejectedWith(R"({"dump_guest": ["a", 2]})", "\"dump_guest\"[1]: expected a string, got number"));
    CHECK(rejectedWith(R"({"dump_guest": [["a"]]})", "\"dump_guest\"[0]: expected a string, got array"));
}

void testNull() {
    // null is an error for every kind, with the same hint.
    for (const char* key :
         {"elf", "log_level", "seconds", "strict", "yield_backoff", "rsx_capture", "dump_guest", "frame_limit"}) {
        const std::string text = std::string("{\"") + key + "\": null}";
        const std::string expected =
                std::string("\"") + key + "\": null is not a value; remove the key to use the default";
        CHECK(parse(text) == std::unexpected(expected));
    }
}

void testDocumentShape() {
    // Values keep the document order, not the table order.
    const auto result = parse(R"({"seconds": 1, "elf": "x", "strict": true})");
    CHECK(result && result->values.size() == 3);
    if (result && result->values.size() == 3) {
        CHECK(result->values[0].key == "seconds" && result->values[1].key == "elf" &&
              result->values[2].key == "strict");
    }

    CHECK(rejectedWith("[]", "the top level must be an object"));
    CHECK(rejectedWith("3", "the top level must be an object"));
    CHECK(rejectedWith("\"x\"", "the top level must be an object"));
    CHECK(rejectedWith("null", "the top level must be an object"));

    // The first bad value in document order is reported.
    CHECK(rejectedWith(R"({"seconds": "x", "strict": 1})", "\"seconds\""));
    CHECK(rejectedWith(R"({"strict": 1, "seconds": "x"})", "\"strict\""));
}

void testUnknownKeys() {
    const auto result = parse(R"({"my_note": "keep", "seconds": 2, "elf-path": 1, "Seconds": 3})");
    CHECK(result && result->values.size() == 1 && result->values[0].key == "seconds");
    if (result) {
        CHECK(result->warnings.size() == 3);
        if (result->warnings.size() == 3) {
            CHECK(result->warnings[0] == "unknown key \"my_note\" ignored");
            CHECK(result->warnings[1] == "unknown key \"elf-path\" ignored");  // the key uses '_', not '-'
            CHECK(result->warnings[2] == "unknown key \"Seconds\" ignored");   // keys are case-sensitive
        }
    }
    // The type of an unknown key's value does not matter.
    const auto odd = parse(R"({"x": null, "y": {"a": [1, 2]}})");
    CHECK(odd && odd->values.empty() && odd->warnings.size() == 2);
}

void testJsonSyntax() {
    // A UTF-8 byte order mark is skipped, as Windows editors write one.
    const auto bom = parse("\xEF\xBB\xBF{\"seconds\": 4}");
    CHECK(bom && bom->values.size() == 1 && bom->values[0].text == "4");

    // Comments and trailing commas are not JSON.
    CHECK(rejectedWith("{\n  // note\n  \"seconds\": 4\n}", "invalid JSON"));
    CHECK(rejectedWith("{\"seconds\": 4 /* note */}", "invalid JSON"));
    CHECK(rejectedWith("{\"seconds\": 4,}", "invalid JSON"));
    CHECK(rejectedWith("[1,]", "invalid JSON"));
    CHECK(rejectedWith("", "invalid JSON"));
    CHECK(rejectedWith("   \n", "invalid JSON"));
    CHECK(rejectedWith("{\"seconds\": 4} {}", "invalid JSON"));
    CHECK(rejectedWith("{\"seconds\": NaN}", "invalid JSON"));
    // Invalid UTF-8 inside a string is a JSON error already.
    CHECK(rejectedWith("{\"elf\": \"\xC3\x28\"}", "invalid JSON"));

    // The position is 1-based and counts from the last line break.
    CHECK(rejectedWith("{\n  \"seconds\": 4,\n  oops\n}", "at line 3, column 3"));
    CHECK(rejectedWith("{\"seconds\": ?}", "at line 1, column 13"));
    CHECK(rejectedWith("{\r\n\"seconds\": ?}", "at line 2, column 12"));
}

void testDuplicateKeys() {
    CHECK(rejectedWith(R"({"seconds": 1, "seconds": 2})", "duplicate key \"seconds\" in an object"));
    // Also for unknown keys and inside nested values.
    CHECK(rejectedWith(R"({"x": 1, "x": 2})", "duplicate key \"x\""));
    CHECK(rejectedWith(R"({"x": {"a": 1, "a": 2}})", "duplicate key \"a\""));
    CHECK(rejectedWith(R"({"x": [{"a": 1, "a": 2}]})", "duplicate key \"a\""));
    // The same key in different objects is fine.
    CHECK(parse(R"({"x": {"a": 1}, "y": {"a": 2}})").has_value());
}

// The result of applying one change to `original`, or the error text.
std::expected<std::string, std::string> update(std::string_view original, std::string_view key,
                                               std::string_view value) {
    const std::array<tox2::ConfigChange, 1> changes = {{{std::string(key), std::string(value)}}};
    return tox2::updateConfigText(original, changes, kSpecs);
}

bool updateFailsWith(std::string_view original, std::string_view key, std::string_view value, std::string_view needle) {
    const auto result = update(original, key, value);
    return !result && result.error().find(needle) != std::string::npos;
}

void testUpdateNewFile() {
    CHECK(update("", "resolution_scale", "2") == "{\n  \"resolution_scale\": 2\n}\n");
    CHECK(update(" \r\n\t", "resolution_scale", "2") == "{\n  \"resolution_scale\": 2\n}\n");
    CHECK(update("\xEF\xBB\xBF", "resolution_scale", "2") == "{\n  \"resolution_scale\": 2\n}\n");
    CHECK(update("{}", "resolution_scale", "2") == "{\n  \"resolution_scale\": 2\n}\n");
    CHECK(update("{\"resolution_scale\":2}", "frame_limit", "off") ==
          "{\n  \"resolution_scale\": 2,\n  \"frame_limit\": \"off\"\n}\n");
}

void testUpdateKinds() {
    CHECK(update("{}", "fullscreen", "on") == "{\n  \"fullscreen\": true\n}\n");
    CHECK(update("{}", "fullscreen", "off") == "{\n  \"fullscreen\": false\n}\n");
    // frame_limit is a string option, so "60" stays a string even though it looks like a number.
    CHECK(update("{}", "frame_limit", "60") == "{\n  \"frame_limit\": \"60\"\n}\n");
    CHECK(update("{}", "resolution_scale", "18446744073709551615") ==
          "{\n  \"resolution_scale\": 18446744073709551615\n}\n");
}

void testUpdateKeepsOtherMembers() {
    const std::string original =
            "\xEF\xBB\xBF{\"my_note\": \"keep \\\"this\\\"\", \"resolution_scale\": 4, \"elf\": \"a/b.elf\", "
            "\"big\": 1.50, \"nested\": {\"a\": [1, 2.0, null]}, \"jp\": \"\xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0\"}";
    const auto result = update(original, "resolution_scale", "3");
    CHECK(result.has_value());
    if (result) {
        // The BOM is dropped; the changed member stays where it was; the rest is untouched (numbers verbatim).
        CHECK(*result ==
              "{\n  \"my_note\": \"keep \\\"this\\\"\",\n  \"resolution_scale\": 3,\n  \"elf\": \"a/b.elf\",\n"
              "  \"big\": 1.50,\n  \"nested\": {\n    \"a\": [\n      1,\n      2.0,\n      null\n    ]\n  },\n"
              "  \"jp\": \"\xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0\"\n}\n");
        CHECK(result->compare(0, 3, "\xEF\xBB\xBF") != 0);
        // The output is valid input for the reader and shows the change.
        const auto reread = parse(*result);
        CHECK(reread && reread->values.size() == 2 && reread->warnings.size() == 4);
    }

    // A second change to the same key replaces the first; different keys are applied in order.
    const std::array<tox2::ConfigChange, 3> changes = {
            {{"resolution_scale", "2"}, {"fullscreen", "on"}, {"resolution_scale", "3"}}};
    CHECK(tox2::updateConfigText("{\"x\": 1}", changes, kSpecs) ==
          "{\n  \"x\": 1,\n  \"resolution_scale\": 3,\n  \"fullscreen\": true\n}\n");
    // No change gives the normalised text.
    CHECK(tox2::updateConfigText("{\"x\":1}", {}, kSpecs) == "{\n  \"x\": 1\n}\n");
}

void testCheckConfigChange() {
    CHECK(tox2::checkConfigChange({"resolution_scale", "2"}, kSpecs).has_value());
    CHECK(tox2::checkConfigChange({"frame_limit", "anything"}, kSpecs).has_value());
    CHECK(tox2::checkConfigChange({"fullscreen", "off"}, kSpecs).has_value());
    const auto bad_kind = tox2::checkConfigChange({"fullscreen", "maybe"}, kSpecs);
    CHECK(!bad_kind && bad_kind.error() == "\"fullscreen\": \"maybe\" is not on or off");
    const auto bad_number = tox2::checkConfigChange({"resolution_scale", "-1"}, kSpecs);
    CHECK(!bad_number && bad_number.error().find("not a non-negative whole number") != std::string::npos);
    CHECK(!tox2::checkConfigChange({"elf", "x"}, kSpecs).has_value());
}

void testUpdateFailures() {
    CHECK(updateFailsWith("{\"a\": ", "resolution_scale", "2", "invalid JSON"));
    CHECK(updateFailsWith("{\"a\": 1,}", "resolution_scale", "2", "invalid JSON"));
    CHECK(updateFailsWith("{\n  oops\n}", "resolution_scale", "2", "at line 2, column 3"));
    CHECK(updateFailsWith("{\"a\": 1, \"a\": 2}", "resolution_scale", "2", "duplicate key \"a\""));
    CHECK(updateFailsWith("[]", "resolution_scale", "2", "the top level must be an object"));
    // elf is a known option but not persisted; "nothing" is not an option at all.
    CHECK(updateFailsWith("{}", "elf", "x", "\"elf\" is not a setting that is saved"));
    CHECK(updateFailsWith("{}", "nothing", "x", "\"nothing\" is not a setting that is saved"));
    CHECK(updateFailsWith("{}", "resolution_scale", "x",
                          "\"resolution_scale\": \"x\" is not a non-negative whole number"));
    CHECK(updateFailsWith("{}", "resolution_scale", "-1", "not a non-negative whole number"));
    CHECK(updateFailsWith("{}", "resolution_scale", "", "not a non-negative whole number"));
    CHECK(updateFailsWith("{}", "resolution_scale", "2x", "not a non-negative whole number"));
    CHECK(updateFailsWith("{}", "resolution_scale", "18446744073709551616", "not a non-negative whole number"));
    CHECK(updateFailsWith("{}", "fullscreen", "maybe", "\"fullscreen\": \"maybe\" is not on or off"));
    // One bad change fails the whole update.
    const std::array<tox2::ConfigChange, 2> changes = {{{"resolution_scale", "2"}, {"fullscreen", "x"}}};
    CHECK(!tox2::updateConfigText("{}", changes, kSpecs).has_value());
}

std::string readFile(const stdfs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void writeFile(const stdfs::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
}

void testWriteFileAtomic() {
    // One directory per process, so parallel test runs do not share files.
    const stdfs::path dir =
            stdfs::temp_directory_path() / ("tox2_game_options_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    stdfs::remove_all(dir, ec);
    stdfs::create_directories(dir);
    const stdfs::path target = dir / "config.json";
    const stdfs::path temp = dir / "config.json.tmp";

    CHECK(tox2::writeFileAtomic(target, "first\n").has_value());
    CHECK(readFile(target) == "first\n");
    CHECK(!stdfs::exists(temp));

    CHECK(tox2::writeFileAtomic(target, "second \xE3\x82\xB2\r\n").has_value());
    CHECK(readFile(target) == "second \xE3\x82\xB2\r\n");  // bytes are written as they are
    CHECK(!stdfs::exists(temp));

    // A read-only target cannot be replaced: the old content stays and no temp file is left behind.
    SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_READONLY);
    const auto blocked = tox2::writeFileAtomic(target, "third\n");
    CHECK(!blocked.has_value());
    if (!blocked) {
        CHECK(blocked.error().find("cannot replace") != std::string::npos);
    }
    CHECK(readFile(target) == "second \xE3\x82\xB2\r\n");
    CHECK(!stdfs::exists(temp));
    SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);

    // A directory that does not exist fails at the first step.
    const auto missing = tox2::writeFileAtomic(dir / "no_such_dir" / "config.json", "x");
    CHECK(!missing.has_value());
    if (!missing) {
        CHECK(missing.error().find("cannot create") != std::string::npos);
    }

    // The writer output feeds the next update: the full round trip a saved setting takes.
    writeFile(target, "{\"my_note\": \"keep\"}");
    const auto text = update(readFile(target), "resolution_scale", "2");
    CHECK(text.has_value() && tox2::writeFileAtomic(target, *text).has_value());
    CHECK(readFile(target) == "{\n  \"my_note\": \"keep\",\n  \"resolution_scale\": 2\n}\n");

    stdfs::remove_all(dir, ec);
}

// Directories that do not exist, so that a line without --config never reads the player's config.json.
tox2::ConfigSearch noConfigSearch() {
    static const stdfs::path root =
            stdfs::temp_directory_path() / ("tox2_game_options_absent_" + std::to_string(GetCurrentProcessId()));
    return {root / "work", root / "exe"};
}

std::expected<tox2::GameOptions, tox2::OptionsError> parseLine(const std::vector<std::string>& args) {
    return tox2::parseGameOptions(args, noConfigSearch());
}

// Parses `args` and returns the error, or an error with an empty message when the line is accepted.
tox2::OptionsError parseError(const std::vector<std::string>& args) {
    const auto result = parseLine(args);
    return result ? tox2::OptionsError{.message = {}} : result.error();
}

// True when the line is rejected with exactly `message`, exit code 2 and the usage text requested.
bool rejectedLine(const std::vector<std::string>& args, std::string_view message) {
    const tox2::OptionsError error = parseError(args);
    return error.message == message && error.exit_code == 2 && error.print_usage;
}

void testOptionTable() {
    const std::span<const OptionSpec> specs = tox2::gameOptionSpecs();
    CHECK(!specs.empty());
    for (std::size_t i = 0; i < specs.size(); ++i) {
        for (std::size_t j = i + 1; j < specs.size(); ++j) CHECK(specs[i].name != specs[j].name);
    }
    // Only the settings a player changes in the window are persisted.
    std::vector<std::string_view> persisted;
    for (const OptionSpec& spec : specs) {
        if (spec.persisted) persisted.push_back(spec.name);
    }
    CHECK((persisted == std::vector<std::string_view>{"frame-limit", "resolution-scale", "aa-mode",
                                                      "anisotropic-filter", "window-size", "fullscreen"}));
    for (const OptionSpec& spec : specs) {
        if (spec.name == "frame-limit") CHECK(spec.kind == OptionKind::String && spec.needs == OptionNeeds::Nothing);
        if (spec.name == "resolution-scale") {
            CHECK(spec.kind == OptionKind::Unsigned && spec.needs == OptionNeeds::Renderer);
        }
        if (spec.name == "aa-mode") CHECK(spec.kind == OptionKind::String && spec.needs == OptionNeeds::Renderer);
        if (spec.name == "anisotropic-filter") {
            CHECK(spec.kind == OptionKind::String && spec.needs == OptionNeeds::Renderer);
        }
        if (spec.name == "window-size") CHECK(spec.kind == OptionKind::String && spec.needs == OptionNeeds::Window);
        if (spec.name == "fullscreen") CHECK(spec.kind == OptionKind::Switch && spec.needs == OptionNeeds::Window);
        if (spec.name == "rsx-capture") CHECK(spec.kind == OptionKind::FrameList);
        if (spec.name == "dump-guest") CHECK(spec.kind == OptionKind::StringList);
    }
    // Options a file value can not use without a renderer or a window are left out of such runs.
    for (const OptionSpec& spec : specs) {
        const bool renderer = spec.name == "dump-frames" || spec.name == "dump-frames-every" ||
                              spec.name == "shader-cache" || spec.name == "vk-validation" ||
                              spec.name == "resolution-scale" || spec.name == "aa-mode" ||
                              spec.name == "anisotropic-filter";
        const bool window = spec.name == "pad-probe" || spec.name == "window-size" || spec.name == "fullscreen";
        const OptionNeeds expected = renderer ? OptionNeeds::Renderer
                                     : window ? OptionNeeds::Window
                                              : OptionNeeds::Nothing;
        CHECK(spec.needs == expected);
    }
    CHECK(tox2::gameOptionsUsage().starts_with("usage: tox2_game "));
    CHECK(tox2::gameOptionsUsage().find("--config FILE | --no-config") != std::string_view::npos);
    CHECK(tox2::gameOptionsUsage().find("[--anisotropic-filter off|2|4|8|16]") != std::string_view::npos);
    CHECK(tox2::gameOptionsUsage().find("[--window-size 720p|1080p|1440p|2160p]") != std::string_view::npos);
    CHECK(tox2::gameOptionsUsage().find("[--fullscreen on|off]") != std::string_view::npos);
}

void testDefaultLines() {
    const auto windowed = parseLine(std::vector<std::string>{"--game-dir", "g"});
    CHECK(windowed.has_value());
    if (windowed) {
        CHECK(windowed->game_dir == stdfs::path("g"));
        CHECK(windowed->window && windowed->render_vulkan && !windowed->no_window);
        CHECK(windowed->pad == "keyboard+gamepad" && windowed->audio == "wasapi");
        CHECK(windowed->frame_limit == tox2::RsxFlipLimit::Vsync && windowed->resolution_scale == 1);
        CHECK(windowed->aa_mode == tox2::AaMode::Msaa && windowed->ppu_threads == tox2::PpuThreadsMode::Split2);
        CHECK(windowed->host_priorities && windowed->yield_backoff && windowed->lwmutex_fast_path);
        CHECK(windowed->dump_frames_every == 1 && windowed->seconds == 0 && !windowed->log_level);
        CHECK(windowed->dialog_answer.empty() && windowed->rsx_capture_frames.empty() && windowed->dumps.empty());
        CHECK(windowed->anisotropy == 1 && windowed->window_size == tox2::WindowSizePreset::Size720p &&
              !windowed->fullscreen);
    }

    const auto headless = parseLine(std::vector<std::string>{"--vfs-config", "v.json", "--no-window"});
    CHECK(headless.has_value());
    if (headless) {
        CHECK(headless->vfs_config == stdfs::path("v.json") && headless->game_dir.empty());
        CHECK(!headless->window && !headless->render_vulkan && headless->no_window);
        CHECK(headless->pad == "neutral" && headless->audio == "none");
    }

    // A frame dump implies the renderer without a window.
    const auto dumping = parseLine(std::vector<std::string>{"--game-dir", "g", "--no-window", "--dump-frames", "d"});
    CHECK(dumping && !dumping->window && dumping->render_vulkan && dumping->dump_frames == stdfs::path("d"));
    // An explicit pad and audio are kept.
    const auto explicit_pad =
            parseLine(std::vector<std::string>{"--game-dir", "g", "--no-window", "--pad", "none", "--audio", "wasapi"});
    CHECK(explicit_pad && explicit_pad->pad == "none" && explicit_pad->audio == "wasapi");
}

void testValues() {
    const auto all = parseLine(std::vector<std::string>{"--game-dir",
                                                        "g",
                                                        "--elf",
                                                        "e.elf",
                                                        "--libsre",
                                                        "l.elf",
                                                        "--hdd-dir",
                                                        "h",
                                                        "--sdata-cache",
                                                        "s",
                                                        "--png-dump",
                                                        "p",
                                                        "--rsx-census",
                                                        "c.json",
                                                        "--log",
                                                        "l.log",
                                                        "--seconds",
                                                        "30",
                                                        "--strict",
                                                        "--quiet-hle",
                                                        "--load-only",
                                                        "--lwmutex-watch",
                                                        "--frame-limit",
                                                        "60",
                                                        "--host-priorities",
                                                        "off",
                                                        "--yield-backoff",
                                                        "off",
                                                        "--lwmutex-fast-path",
                                                        "off",
                                                        "--ppu-threads",
                                                        "legacy",
                                                        "--resolution-scale",
                                                        "3",
                                                        "--aa-mode",
                                                        "supersample",
                                                        "--dump-frames-every",
                                                        "4",
                                                        "--dialog-answer",
                                                        "no"});
    CHECK(all.has_value());
    if (all) {
        CHECK(all->elf == stdfs::path("e.elf") && all->libsre == stdfs::path("l.elf") && all->hdd_dir == "h");
        CHECK(all->sdata_cache == "s" && all->png_dump == "p" && all->rsx_census == "c.json" &&
              all->log_file == "l.log");
        CHECK(all->seconds == 30 && all->strict && all->quiet_hle && all->load_only && all->lwmutex_watch);
        CHECK(all->frame_limit == tox2::RsxFlipLimit::Hz60 && !all->host_priorities && !all->yield_backoff);
        CHECK(!all->lwmutex_fast_path && all->ppu_threads == tox2::PpuThreadsMode::Legacy);
        CHECK(all->resolution_scale == 3 && all->aa_mode == tox2::AaMode::Supersample && all->dump_frames_every == 4);
        CHECK(all->dialog_answer == "no");
    }
    const auto one = parseLine(std::vector<std::string>{"--game-dir", "g", "--ppu-threads", "1"});
    CHECK(one && one->ppu_threads == tox2::PpuThreadsMode::Split1);
    const auto two = parseLine(std::vector<std::string>{"--game-dir", "g", "--ppu-threads", "2"});
    CHECK(two && two->ppu_threads == tox2::PpuThreadsMode::Split2);
    const auto off = parseLine(std::vector<std::string>{"--game-dir", "g", "--frame-limit", "off"});
    CHECK(off && off->frame_limit == tox2::RsxFlipLimit::Off);
    const auto none = parseLine(std::vector<std::string>{"--game-dir", "g", "--render", "none"});
    CHECK(none && !none->window && !none->render_vulkan && none->pad == "neutral" && none->audio == "none");

    // The last value of a repeated option wins; the list options accumulate.
    const auto repeated = parseLine(std::vector<std::string>{"--game-dir", "g", "--seconds", "5", "--seconds", "10"});
    CHECK(repeated && repeated->seconds == 10);
    const auto frames =
            parseLine(std::vector<std::string>{"--game-dir", "g", "--rsx-capture", "1,2", "--rsx-capture", "3"});
    CHECK(frames && frames->rsx_capture_frames == (std::vector<std::uint32_t>{1, 2, 3}));
    const auto dumps = parseLine(std::vector<std::string>{"--game-dir", "g", "--dump-guest", "0x1000:16:a.bin",
                                                          "--dump-guest", "0x2000:8:b.bin"});
    CHECK(dumps && dumps->dumps.size() == 2);
    if (dumps && dumps->dumps.size() == 2) {
        CHECK(dumps->dumps[0].addr == 0x1000 && dumps->dumps[0].size == 16 && dumps->dumps[0].file == "a.bin");
        CHECK(dumps->dumps[1].addr == 0x2000 && dumps->dumps[1].file == "b.bin");
    }
}

void testErrorMessages() {
    CHECK(rejectedLine({"--game-dir", "g", "--resolution-scale", "5"},
                       "error: --resolution-scale needs a whole number from 1 to 4\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--resolution-scale", "x"},
                       "error: --resolution-scale needs a whole number from 1 to 4\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--frame-limit", "30"}, "error: --frame-limit needs vsync, 60 or off\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--bogus"}, "error: unknown argument --bogus\n"));
    CHECK(rejectedLine({"--game-dir", "g", "stray"}, "error: unknown argument stray\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--lwmutex-watch", "extra"}, "error: unknown argument extra\n"));
    CHECK(rejectedLine({"--game-dir"}, "error: --game-dir needs a value\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--seconds"}, "error: --seconds needs a value\n"));
    CHECK(rejectedLine({}, "error: --game-dir or --vfs-config is required\n"));
    CHECK(rejectedLine({"--hdd-dir", "h"}, "error: --game-dir or --vfs-config is required\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--render", "none", "--aa-mode", "msaa"},
                       "error: --dump-frames, --dump-frames-every, --shader-cache, --resolution-scale, --aa-mode, "
                       "--anisotropic-filter and --vk-validation need --render vulkan\n"));
    // The frame list reports its format after the missing value, as it always has.
    CHECK(rejectedLine({"--game-dir", "g", "--rsx-capture"},
                       "error: --rsx-capture needs a value\n"
                       "error: --rsx-capture needs frame numbers (1-based), e.g. 300,600\n"));
    for (const char* list : {"0", "1,", ",1", "x", "", "1,2,x"}) {
        CHECK(rejectedLine({"--game-dir", "g", "--rsx-capture", list},
                           "error: --rsx-capture needs frame numbers (1-based), e.g. 300,600\n"));
    }
    CHECK(rejectedLine({"--game-dir", "g", "--log-level", "loud"},
                       "error: --log-level needs trace, debug, info, warn or error\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--dialog-answer", "maybe"},
                       "error: --dialog-answer needs yes, no or ok\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--pad", "joystick"},
                       "error: --pad needs none, neutral, keyboard, gamepad or keyboard+gamepad\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--audio", "alsa"}, "error: --audio needs none or wasapi\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--render", "opengl"}, "error: --render needs none or vulkan\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--aa-mode", "none"}, "error: --aa-mode needs supersample|msaa\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--host-priorities", "maybe"},
                       "error: --host-priorities needs on or off\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--yield-backoff", "1"}, "error: --yield-backoff needs on or off\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--lwmutex-fast-path", "true"},
                       "error: --lwmutex-fast-path needs on or off\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--ppu-threads", "3"}, "error: --ppu-threads needs legacy, 1 or 2\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--seconds", "-1"}, "error: --seconds needs a non-negative integer\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--dump-frames-every", "0"},
                       "error: --dump-frames-every needs a positive integer\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--dump-frames-every", "4294967296"},
                       "error: --dump-frames-every needs a positive integer\n"));
    CHECK(parseError({"--game-dir", "g", "--dump-guest", "bad"}).message.starts_with("error: --dump-guest bad: "));
    CHECK(rejectedLine({"--game-dir", "g", "--no-window", "--pad", "keyboard"},
                       "error: --pad keyboard needs the window or --pad-script\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--no-window", "--pad", "gamepad"},
                       "error: --pad gamepad needs the window\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--no-window", "--pad-probe"}, "error: --pad-probe needs the window\n"));
}

void testCheckOrder() {
    // The first combined check in the fixed order wins.
    CHECK(rejectedLine({"--game-dir", "g", "--render", "none", "--aa-mode", "msaa", "--pad", "keyboard"},
                       "error: --dump-frames, --dump-frames-every, --shader-cache, --resolution-scale, --aa-mode, "
                       "--anisotropic-filter and --vk-validation need --render vulkan\n"));
    // A bad option value is found before a combined check.
    CHECK(rejectedLine({"--game-dir", "g", "--render", "none", "--aa-mode", "bogus"},
                       "error: --aa-mode needs supersample|msaa\n"));
    // An unknown argument is found while the line is collected, before any value is checked.
    CHECK(rejectedLine({"--game-dir", "g", "--log-level", "loud", "--bogus"}, "error: unknown argument --bogus\n"));
    // Every given occurrence is checked, so an invalid value is not hidden by a later valid one.
    CHECK(rejectedLine({"--game-dir", "g", "--resolution-scale", "5", "--resolution-scale", "2"},
                       "error: --resolution-scale needs a whole number from 1 to 4\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--seconds", "x", "--seconds", "5"},
                       "error: --seconds needs a non-negative integer\n"));
}

void testPadScriptAndAudioDump() {
    std::error_code ec;
    const stdfs::path dir =
            stdfs::temp_directory_path() / ("tox2_game_options_args_" + std::to_string(GetCurrentProcessId()));
    stdfs::remove_all(dir, ec);
    stdfs::create_directories(dir);
    const stdfs::path good = dir / "ok.pad";
    const stdfs::path bad = dir / "bad.pad";
    // Binary, because the script reader does not strip a carriage return.
    writeFile(good, "0.5 tap cross\n");
    writeFile(bad, "0.5 tapp cross\n");
    const std::string good_text = good.string();
    const std::string bad_text = bad.string();

    // A script selects the keyboard pad for an unattended run.
    const auto scripted =
            parseLine(std::vector<std::string>{"--game-dir", "g", "--no-window", "--pad-script", good_text});
    CHECK(scripted && scripted->pad == "keyboard" && scripted->pad_script == good &&
          scripted->pad_script_data.events.size() == 2);
    const tox2::OptionsError broken = parseError({"--game-dir", "g", "--pad-script", bad_text});
    CHECK(broken.message == "error: --pad-script " + bad_text + ": line 1: unknown command \"tapp\"\n" &&
          broken.print_usage);
    const tox2::OptionsError missing = parseError({"--game-dir", "g", "--pad-script", (dir / "missing.pad").string()});
    CHECK(missing.message.starts_with("error: --pad-script ") && missing.print_usage);
    CHECK(rejectedLine({"--game-dir", "g", "--no-window", "--pad", "neutral", "--pad-script", good_text},
                       "error: --pad-script needs --pad keyboard or keyboard+gamepad\n"));

    // The audio dump probe creates the file; a path in a missing directory is a usage error.
    const stdfs::path wav = dir / "dump.wav";
    const auto dumping = parseLine(std::vector<std::string>{"--game-dir", "g", "--audio-dump", wav.string()});
    CHECK(dumping && dumping->audio_dump == wav && stdfs::exists(wav));
    const std::string no_dir_text = (dir / "nodir" / "x.wav").string();
    const tox2::OptionsError no_dir = parseError({"--game-dir", "g", "--audio-dump", no_dir_text});
    CHECK(no_dir.message.starts_with("error: --audio-dump " + no_dir_text + ": ") && no_dir.print_usage);

    stdfs::remove_all(dir, ec);
}

void testApplyOptions() {
    // Parsing only records the level and the answer; applyGameOptions is what changes the process.
    const bool debug_before = tox2::logEnabled(tox2::LogLevel::Debug);
    const auto parsed =
            parseLine(std::vector<std::string>{"--game-dir", "g", "--log-level", "debug", "--dialog-answer", "no"});
    CHECK(parsed && parsed->log_level == tox2::LogLevel::Debug && parsed->dialog_answer == "no");
    CHECK(tox2::logEnabled(tox2::LogLevel::Debug) == debug_before);
    if (parsed) {
        tox2::applyGameOptions(*parsed);
        CHECK(tox2::logEnabled(tox2::LogLevel::Debug));
        tox2::setLogLevel(tox2::LogLevel::Info);
    }
}

// A working and an executable directory under a per-process temporary directory.
struct ConfigDirs {
    stdfs::path root, work, exe;

    explicit ConfigDirs(const char* name) {
        root = stdfs::temp_directory_path() / (std::string(name) + std::to_string(GetCurrentProcessId()));
        work = root / "work";
        exe = root / "exe";
        std::error_code ec;
        stdfs::remove_all(root, ec);
        stdfs::create_directories(work);
        stdfs::create_directories(exe);
    }
    ~ConfigDirs() {
        std::error_code ec;
        stdfs::remove_all(root, ec);
    }
    ConfigDirs(const ConfigDirs&) = delete;
    ConfigDirs& operator=(const ConfigDirs&) = delete;

    std::expected<tox2::GameOptions, tox2::OptionsError> parse(const std::vector<std::string>& args) const {
        return tox2::parseGameOptions(args, tox2::ConfigSearch{work, exe});
    }
    // The error of a line, or an error with an empty message when it is accepted.
    tox2::OptionsError error(const std::vector<std::string>& args) const {
        const auto result = parse(args);
        return result ? tox2::OptionsError{.message = {}} : result.error();
    }
};

bool samePath(const stdfs::path& a, const stdfs::path& b) { return a.lexically_normal() == b.lexically_normal(); }

void testConfigFileValues() {
    ConfigDirs dirs("tox2_game_options_cfg_values_");
    const stdfs::path file = dirs.work / "config.json";
    writeFile(file,
              R"({"game_dir": "g", "elf": "bin/e.elf", "resolution_scale": 2, "frame_limit": 60, "strict": true,
                  "seconds": 7, "rsx_capture": "1,2", "dump_guest": ["0x1000:16:a.bin"], "my_note": "keep"})");

    const auto opt = dirs.parse({});
    CHECK(opt.has_value());
    if (opt) {
        // Paths of the file resolve against its directory, so a file used from any working directory works.
        CHECK(samePath(opt->game_dir, dirs.work / "g") && samePath(opt->elf, dirs.work / "bin" / "e.elf"));
        CHECK(opt->resolution_scale == 2 && opt->frame_limit == tox2::RsxFlipLimit::Hz60 && opt->strict);
        CHECK(opt->seconds == 7 && opt->rsx_capture_frames == (std::vector<std::uint32_t>{1, 2}));
        CHECK(opt->dumps.size() == 1 && opt->dumps[0].file == "a.bin");
        CHECK(opt->config_file == file && opt->settings_file == file && !opt->no_config);
        CHECK(opt->config_warnings == std::vector<std::string>{"unknown key \"my_note\" ignored"});
        CHECK(opt->window && opt->render_vulkan && opt->config_ignored.empty());
    }

    // The command line wins, option by option; the rest of the file still applies.
    const auto over = dirs.parse(
            {"--resolution-scale", "3", "--game-dir", "cli", "--rsx-capture", "9", "--dump-guest", "0x2000:8:b.bin"});
    CHECK(over.has_value());
    if (over) {
        CHECK(over->resolution_scale == 3 && over->game_dir == stdfs::path("cli"));
        CHECK(over->frame_limit == tox2::RsxFlipLimit::Hz60 && over->seconds == 7);
        // A list on the command line replaces the file's whole list.
        CHECK(over->rsx_capture_frames == std::vector<std::uint32_t>{9});
        CHECK(over->dumps.size() == 1 && over->dumps[0].file == "b.bin");
    }

    // A file value that the command line replaces has no effect, so it is not checked.
    writeFile(file, R"({"game_dir": "g", "resolution_scale": 9})");
    CHECK(dirs.parse({"--resolution-scale", "2"}).has_value());

    // A flag set to false is the same as no key; "no_window" and "render" from the file shape the run.
    writeFile(file, R"({"game_dir": "g", "strict": false, "no_window": true})");
    const auto flags = dirs.parse({});
    CHECK(flags && !flags->strict && flags->no_window && !flags->window && flags->pad == "neutral");
    writeFile(file, R"({"game_dir": "g", "render": "none", "resolution_scale": 2})");
    const auto none = dirs.parse({});
    CHECK(none && !none->render_vulkan && !none->window && none->resolution_scale == 1);

    // A non-ASCII path keeps its characters.
    writeFile(file, "{\"game_dir\": \"\xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0\"}");
    const auto utf8 = dirs.parse({});
    CHECK(utf8 && samePath(utf8->game_dir, dirs.work / stdfs::path(u8"ゲーム")));
}

void testConfigFileLookup() {
    ConfigDirs dirs("tox2_game_options_cfg_lookup_");
    const stdfs::path work_file = dirs.work / "config.json";
    const stdfs::path exe_file = dirs.exe / "config.json";

    // Neither directory has a file: nothing is read, and the executable directory is where one will be created.
    const auto none = dirs.parse({"--game-dir", "g"});
    CHECK(none && none->config_file.empty() && none->settings_file == exe_file && !none->no_config);
    // Without an executable directory there is nowhere to save.
    const auto no_exe =
            tox2::parseGameOptions(std::vector<std::string>{"--game-dir", "g"}, tox2::ConfigSearch{dirs.work, {}});
    CHECK(no_exe && no_exe->config_file.empty() && no_exe->settings_file.empty());

    writeFile(exe_file, R"({"game_dir": "g", "seconds": 9})");
    const auto exe_only = dirs.parse({});
    CHECK(exe_only && exe_only->seconds == 9 && exe_only->config_file == exe_file &&
          exe_only->settings_file == exe_file);

    // The working directory comes first, and the two files are never merged.
    writeFile(work_file, R"({"game_dir": "g", "strict": true, "seconds": 7})");
    const auto both = dirs.parse({});
    CHECK(both && both->seconds == 7 && both->strict && both->config_file == work_file);
    writeFile(exe_file, R"({"game_dir": "g", "load_only": true})");
    const auto unmerged = dirs.parse({});
    CHECK(unmerged && !unmerged->load_only && unmerged->config_file == work_file);

    // --config names the file read and saved; a relative name is relative to the working directory.
    writeFile(dirs.work / "other.json", R"({"game_dir": "g", "seconds": 5})");
    const auto named = dirs.parse({"--config", "other.json"});
    CHECK(named && named->seconds == 5 && !named->strict && named->config_file == dirs.work / "other.json" &&
          named->settings_file == named->config_file);
    const auto last = dirs.parse({"--config", "missing.json", "--config", "other.json"});
    CHECK(last && last->seconds == 5);
    const auto absolute = dirs.parse({"--config", (dirs.work / "other.json").string()});
    CHECK(absolute && absolute->seconds == 5);

    const tox2::OptionsError missing = dirs.error({"--config", "missing.json"});
    CHECK(missing.message == "error: missing.json: cannot open the file\n" && missing.exit_code == 2 &&
          !missing.print_usage);
    CHECK(dirs.error({"--config", "."}).message == "error: .: cannot open the file\n");
    const tox2::OptionsError no_value = dirs.error({"--game-dir", "g", "--config"});
    CHECK(no_value.message == "error: --config needs a value\n" && no_value.print_usage);

    // --no-config reads and saves nothing, and does not look at a broken file.
    writeFile(work_file, "{");
    const auto off = dirs.parse({"--game-dir", "g", "--no-config"});
    CHECK(off && off->config_file.empty() && off->settings_file.empty() && off->no_config);
    const tox2::OptionsError excluded = dirs.error({"--no-config", "--config", "other.json"});
    CHECK(excluded.message == "error: --no-config excludes --config\n" && excluded.exit_code == 2 &&
          excluded.print_usage);
    CHECK(dirs.error({"--config", "other.json", "--no-config"}).message == excluded.message);
}

void testConfigFileErrors() {
    ConfigDirs dirs("tox2_game_options_cfg_errors_");
    const stdfs::path file = dirs.work / "config.json";
    const std::string name = file.string();
    const std::string note = "note: settings were also read from " + name + "\n";

    // A file that cannot be parsed stops the start and names the file; the usage text is for the command line.
    writeFile(file, "{");
    const tox2::OptionsError broken = dirs.error({"--game-dir", "g"});
    CHECK(broken.message.starts_with("error: " + name + ": ") && broken.exit_code == 2 && !broken.print_usage);
    writeFile(file, R"({"game_dir": null})");
    CHECK(dirs.error({}).message.starts_with("error: " + name + ": "));

    // A value that fails validation names the key.
    writeFile(file, R"({"game_dir": "g", "resolution_scale": 9})");
    const tox2::OptionsError scale = dirs.error({});
    CHECK(scale.message == "error: " + name + ": resolution_scale needs a whole number from 1 to 4\n" &&
          scale.exit_code == 2 && !scale.print_usage);
    writeFile(file, R"({"game_dir": "g", "pad": "joystick", "frame_limit": 30})");
    CHECK(dirs.error({}).message ==
          "error: " + name + ": pad needs none, neutral, keyboard, gamepad or keyboard+gamepad\n");
    writeFile(file, R"({"game_dir": "g", "rsx_capture": "0"})");
    CHECK(dirs.error({}).message == "error: " + name + ": rsx_capture needs frame numbers (1-based), e.g. 300,600\n");
    // The same value on the command line keeps its message and the usage text.
    CHECK(rejectedLine({"--game-dir", "g", "--no-config", "--resolution-scale", "9"},
                       "error: --resolution-scale needs a whole number from 1 to 4\n"));

    // A combined check keeps its text and adds where settings were also read from.
    writeFile(file, R"({"game_dir": "g", "pad": "gamepad"})");
    const tox2::OptionsError pad = dirs.error({"--no-window"});
    CHECK(pad.message == "error: --pad gamepad needs the window\n" + note && pad.print_usage);
    writeFile(file, R"({"seconds": 1})");
    CHECK(dirs.error({}).message == "error: --game-dir or --vfs-config is required\n" + note);
    // The note is about the file that was read, not about where the problem is.
    writeFile(file, R"({"game_dir": "g"})");
    CHECK(dirs.error({"--no-window", "--resolution-scale", "2"}).message ==
          "error: --dump-frames, --dump-frames-every, --shader-cache, --resolution-scale, --aa-mode, "
          "--anisotropic-filter and --vk-validation need --render vulkan\n" +
                  note);
    // A check on a single option gets no note.
    writeFile(file, R"({"seconds": 1})");
    const std::string no_dir_text = (dirs.work / "nodir" / "x.wav").string();
    const tox2::OptionsError no_dir = dirs.error({"--game-dir", "g", "--audio-dump", no_dir_text});
    CHECK(no_dir.message.starts_with("error: --audio-dump " + no_dir_text + ": ") &&
          no_dir.message.find("note:") == std::string::npos);
    // Command-line mistakes found while collecting stay as they are.
    CHECK(dirs.error({"--bogus"}).message == "error: unknown argument --bogus\n");
}

void testConfigFileDroppedKeys() {
    ConfigDirs dirs("tox2_game_options_cfg_dropped_");
    const stdfs::path file = dirs.work / "config.json";

    // A saved setting must not break an unattended run, so file values the run cannot use are left out.
    writeFile(file, R"({"game_dir": "g", "resolution_scale": 2, "aa_mode": "supersample"})");
    const auto headless = dirs.parse({"--no-window"});
    CHECK(headless.has_value());
    if (headless) {
        CHECK(headless->resolution_scale == 1 && headless->aa_mode == tox2::AaMode::Msaa);
        CHECK((headless->config_ignored == std::vector<std::string>{"resolution_scale ignored: the run has no renderer",
                                                                    "aa_mode ignored: the run has no renderer"}));
    }
    // With a renderer they apply again.
    const auto frames = dirs.parse({"--no-window", "--dump-frames", "d"});
    CHECK(frames && frames->resolution_scale == 2 && frames->aa_mode == tox2::AaMode::Supersample &&
          frames->config_ignored.empty());
    // The same option on the command line keeps the usage error.
    CHECK(dirs.error({"--no-window", "--resolution-scale", "2"})
                  .message.starts_with("error: --dump-frames, --dump-frames-every,"));
    CHECK(dirs.error({"--render", "none", "--aa-mode", "msaa"}).print_usage);

    // The rule covers every renderer-only and window-only key, hand-written ones included.
    writeFile(file, R"({"game_dir": "g", "shader_cache": "sc", "vk_validation": true, "dump_frames_every": 2,
                        "pad_probe": true})");
    const auto unattended = dirs.parse({"--no-window"});
    CHECK(unattended && unattended->shader_cache.empty() && !unattended->vk_validation && !unattended->pad_probe &&
          unattended->dump_frames_every == 1 && unattended->config_ignored.size() == 4);
    const auto windowed = dirs.parse({});
    CHECK(windowed && windowed->vk_validation && windowed->pad_probe && windowed->dump_frames_every == 2 &&
          samePath(windowed->shader_cache, dirs.work / "sc") && windowed->config_ignored.empty());

    // A frame dump from the file is a renderer option too: it implies the renderer unless the run says none.
    writeFile(file, R"({"game_dir": "g", "dump_frames": "d"})");
    const auto dumping = dirs.parse({"--no-window"});
    CHECK(dumping && dumping->render_vulkan && !dumping->window && samePath(dumping->dump_frames, dirs.work / "d"));
    const auto render_none = dirs.parse({"--render", "none"});
    CHECK(render_none && !render_none->render_vulkan && render_none->dump_frames.empty() &&
          render_none->config_ignored.size() == 1);
    // A bad value that is left out is not reported.
    writeFile(file, R"({"game_dir": "g", "dump_frames_every": 0})");
    CHECK(dirs.parse({"--no-window"}).has_value());
    CHECK(dirs.error({}).message == "error: " + file.string() + ": dump_frames_every needs a positive integer\n");
}

void testWindowAndFilterOptions() {
    for (const auto& [text, level] :
         {std::pair<const char*, std::uint32_t>{"off", 1}, {"2", 2}, {"4", 4}, {"8", 8}, {"16", 16}}) {
        const auto parsed = parseLine({"--game-dir", "g", "--anisotropic-filter", text});
        CHECK(parsed && parsed->anisotropy == level);
    }
    for (const auto& [text, preset] :
         {std::pair<const char*, tox2::WindowSizePreset>{"720p", tox2::WindowSizePreset::Size720p},
          {"1080p", tox2::WindowSizePreset::Size1080p},
          {"1440p", tox2::WindowSizePreset::Size1440p},
          {"2160p", tox2::WindowSizePreset::Size2160p}}) {
        const auto parsed = parseLine({"--game-dir", "g", "--window-size", text});
        CHECK(parsed && parsed->window_size == preset);
    }
    const auto on = parseLine({"--game-dir", "g", "--fullscreen", "on"});
    CHECK(on && on->fullscreen);
    const auto off = parseLine({"--game-dir", "g", "--fullscreen", "off"});
    CHECK(off && !off->fullscreen);
    // The last occurrence wins, like the other single-value options.
    const auto repeated = parseLine({"--game-dir", "g", "--window-size", "1080p", "--window-size", "1440p"});
    CHECK(repeated && repeated->window_size == tox2::WindowSizePreset::Size1440p);

    CHECK(rejectedLine({"--game-dir", "g", "--anisotropic-filter", "3"},
                       "error: --anisotropic-filter needs off, 2, 4, 8 or 16\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--anisotropic-filter", "1"},
                       "error: --anisotropic-filter needs off, 2, 4, 8 or 16\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--window-size", "1080P"},
                       "error: --window-size needs 720p, 1080p, 1440p or 2160p\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--window-size", "4k"},
                       "error: --window-size needs 720p, 1080p, 1440p or 2160p\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--fullscreen", "yes"}, "error: --fullscreen needs on or off\n"));
    CHECK(rejectedLine({"--game-dir", "g", "--fullscreen"}, "error: --fullscreen needs a value\n"));

    // The window options need the window, even when they only restate the defaults.
    const char* const need_window = "error: --window-size and --fullscreen need the window\n";
    CHECK(rejectedLine({"--game-dir", "g", "--fullscreen", "on", "--no-window"}, need_window));
    CHECK(rejectedLine({"--game-dir", "g", "--fullscreen", "off", "--no-window"}, need_window));
    CHECK(rejectedLine({"--game-dir", "g", "--window-size", "1080p", "--no-window"}, need_window));
    CHECK(rejectedLine({"--game-dir", "g", "--window-size", "1080p", "--render", "none"}, need_window));
    // The filter is a renderer option.
    CHECK(rejectedLine({"--game-dir", "g", "--anisotropic-filter", "16", "--no-window"},
                       "error: --dump-frames, --dump-frames-every, --shader-cache, --resolution-scale, --aa-mode, "
                       "--anisotropic-filter and --vk-validation need --render vulkan\n"));
    const auto dumping =
            parseLine({"--game-dir", "g", "--no-window", "--dump-frames", "d", "--anisotropic-filter", "4"});
    CHECK(dumping && dumping->anisotropy == 4);

    // A saved setting is dropped, not rejected, by a run that cannot use it.
    ConfigDirs dirs("tox2_game_options_cfg_new_options_");
    const stdfs::path file = dirs.work / "config.json";
    writeFile(file, R"({"game_dir": "g", "anisotropic_filter": 16, "window_size": "1440p", "fullscreen": true})");
    const auto windowed = dirs.parse({});
    CHECK(windowed && windowed->anisotropy == 16 && windowed->window_size == tox2::WindowSizePreset::Size1440p &&
          windowed->fullscreen && windowed->config_ignored.empty());
    const auto headless = dirs.parse({"--no-window"});
    CHECK(headless && headless->anisotropy == 1 && headless->window_size == tox2::WindowSizePreset::Size720p &&
          !headless->fullscreen);
    if (headless) {
        CHECK((headless->config_ignored ==
               std::vector<std::string>{"anisotropic_filter ignored: the run has no renderer",
                                        "window_size ignored: the run has no window",
                                        "fullscreen ignored: the run has no window"}));
    }
    // A renderer without a window keeps the filter and drops the window keys.
    const auto frames = dirs.parse({"--no-window", "--dump-frames", "d"});
    CHECK(frames && frames->anisotropy == 16 && !frames->fullscreen && frames->config_ignored.size() == 2);
    // The same options on the command line keep their errors, and the file's values are checked like any other.
    CHECK(dirs.error({"--no-window", "--fullscreen", "on"}).print_usage);
    writeFile(file, R"({"game_dir": "g", "anisotropic_filter": 3})");
    CHECK(dirs.error({}).message == "error: " + file.string() + ": anisotropic_filter needs off, 2, 4, 8 or 16\n");
    writeFile(file, R"({"game_dir": "g", "anisotropic_filter": "off", "window_size": "huge"})");
    const tox2::OptionsError bad_size = dirs.error({});
    CHECK(bad_size.message == "error: " + file.string() + ": window_size needs 720p, 1080p, 1440p or 2160p\n" &&
          !bad_size.print_usage);
    writeFile(file, R"({"game_dir": "g", "fullscreen": "maybe"})");
    CHECK(dirs.error({}).message.starts_with("error: " + file.string() + ": "));
    // A key given on the command line replaces the file's value.
    writeFile(file, R"({"game_dir": "g", "window_size": "2160p", "fullscreen": true})");
    const auto over = dirs.parse({"--window-size", "1080p", "--fullscreen", "off"});
    CHECK(over && over->window_size == tox2::WindowSizePreset::Size1080p && !over->fullscreen);
}

void testConfigApply() {
    // Only the process state changes here; the lines themselves go to the log.
    ConfigDirs dirs("tox2_game_options_cfg_apply_");
    writeFile(dirs.work / "config.json", R"({"game_dir": "g", "unknown": 1, "resolution_scale": 2})");
    const auto read = dirs.parse({"--no-window"});
    CHECK(read && read->config_warnings.size() == 1 && read->config_ignored.size() == 1);
    if (read) tox2::applyGameOptions(*read);
    const auto none = tox2::parseGameOptions(std::vector<std::string>{"--game-dir", "g", "--no-config"},
                                             tox2::ConfigSearch{dirs.work, dirs.exe});
    CHECK(none.has_value());
    if (none) tox2::applyGameOptions(*none);
}

// A per-process directory for one store test; the file inside may be read-only when the test ends.
struct StoreDir {
    stdfs::path dir;
    stdfs::path file;

    explicit StoreDir(const std::string& name)
        : dir(stdfs::temp_directory_path() / (name + std::to_string(GetCurrentProcessId()))),
          file(dir / "config.json") {
        std::error_code ec;
        stdfs::remove_all(dir, ec);
        stdfs::create_directories(dir);
    }
    ~StoreDir() {
        std::error_code ec;
        SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);
        stdfs::remove_all(dir, ec);
    }
};

// Collects the log lines of the writer thread.
class LogCapture {
public:
    LogCapture() {
        tox2::resetLogOnce();
        tox2::setLogSink([this](std::string_view line) {
            std::lock_guard lock(mutex_);
            lines_.emplace_back(line);
        });
    }
    ~LogCapture() { tox2::setLogSink({}); }

    std::size_t count(std::string_view needle) {
        std::lock_guard lock(mutex_);
        std::size_t found = 0;
        for (const std::string& line : lines_) found += line.find(needle) != std::string::npos ? 1 : 0;
        return found;
    }

private:
    std::mutex mutex_;
    std::vector<std::string> lines_;
};

// Polls for at most two seconds.
template <typename Pred>
bool waitFor(Pred pred) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > limit) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

constexpr tox2::SettingsStore::Timing kFastTiming{std::chrono::milliseconds(20), std::chrono::milliseconds(100)};
// The writer never fires inside a test, so flush() is the only writer.
constexpr tox2::SettingsStore::Timing kSlowTiming{std::chrono::seconds(5), std::chrono::seconds(10)};

void testStoreWrites() {
    LogCapture log;
    StoreDir dir("tox2_settings_store_write_");
    {
        // No file yet: the first change creates it.
        tox2::SettingsStore store(dir.file, kSpecs, kFastTiming);
        CHECK(store.file() == dir.file);
        CHECK(!stdfs::exists(dir.file));
        store.set("resolution_scale", "2");
        CHECK(waitFor([&] { return store.writes() >= 1; }));
        CHECK(readFile(dir.file) == "{\n  \"resolution_scale\": 2\n}\n");
        CHECK(log.count("saved resolution_scale to ") == 1);
    }

    // Ten quick changes of one key are one write with the last value, and unknown members survive. The quiet
    // time is longer than kFastTiming's so that a stalled test thread cannot split the burst.
    writeFile(dir.file, "{\"my_note\": \"keep\", \"nested\": {\"a\": [1, 2]}}");
    {
        tox2::SettingsStore store(dir.file, kSpecs,
                                  tox2::SettingsStore::Timing{std::chrono::milliseconds(100), std::chrono::seconds(1)});
        const std::array<const char*, 4> values = {"1", "2", "4", "3"};
        for (std::size_t i = 0; i < 10; ++i) store.set("resolution_scale", values[i % 4]);
        CHECK(waitFor([&] { return store.writes() >= 1; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(store.writes() == 1);
        CHECK(readFile(dir.file) ==
              "{\n  \"my_note\": \"keep\",\n  \"nested\": {\n    \"a\": [\n      1,\n      2\n    ]\n  },\n"
              "  \"resolution_scale\": 2\n}\n");
    }

    // A change that keeps coming is still written after max_delay.
    writeFile(dir.file, "{}");
    {
        tox2::SettingsStore store(
                dir.file, kSpecs,
                tox2::SettingsStore::Timing{std::chrono::milliseconds(200), std::chrono::milliseconds(300)});
        for (int i = 0; i < 60; ++i) {
            store.set("resolution_scale", i % 2 != 0 ? "2" : "3");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(store.writes() >= 1);
    }
}

void testStoreFailures() {
    LogCapture log;
    StoreDir dir("tox2_settings_store_fail_");
    const std::string broken = "{ oops";
    writeFile(dir.file, broken);
    tox2::SettingsStore store(dir.file, kSpecs, kFastTiming);
    store.set("resolution_scale", "2");
    CHECK(waitFor([&] { return log.count("cannot save settings to ") == 1; }));
    CHECK(readFile(dir.file) == broken);
    CHECK(store.writes() == 0);

    // The change stays pending and the same failure is not logged again.
    store.set("frame_limit", "off");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(log.count("cannot save settings to ") == 1);
    CHECK(readFile(dir.file) == broken);
    CHECK(store.writes() == 0);

    // Once the file is repaired the next change writes all of them.
    writeFile(dir.file, "{\"my_note\": \"keep\"}");
    store.set("fullscreen", "on");
    CHECK(waitFor([&] { return store.writes() >= 1; }));
    CHECK(readFile(dir.file) ==
          "{\n  \"my_note\": \"keep\",\n  \"resolution_scale\": 2,\n  \"frame_limit\": \"off\",\n"
          "  \"fullscreen\": true\n}\n");
    CHECK(store.flush(std::chrono::milliseconds(500)));
    CHECK(store.writes() == 1);
}

void testStoreReadOnlyFile() {
    LogCapture log;
    StoreDir dir("tox2_settings_store_ro_");
    const std::string original = "{\"my_note\": \"keep\"}";
    writeFile(dir.file, original);
    tox2::SettingsStore store(dir.file, kSpecs, kSlowTiming);
    store.set("resolution_scale", "2");

    SetFileAttributesW(dir.file.c_str(), FILE_ATTRIBUTE_READONLY);
    CHECK(!store.flush(std::chrono::seconds(1)));
    CHECK(readFile(dir.file) == original);
    CHECK(!stdfs::exists(dir.file.string() + ".tmp"));
    CHECK(store.writes() == 0);
    CHECK(log.count("cannot save settings to ") == 1);
    // A second attempt fails the same way and stays quiet.
    CHECK(!store.flush(std::chrono::seconds(1)));
    CHECK(log.count("cannot save settings to ") == 1);

    // The change was kept.
    SetFileAttributesW(dir.file.c_str(), FILE_ATTRIBUTE_NORMAL);
    CHECK(store.flush(std::chrono::seconds(1)));
    CHECK(store.writes() == 1);
    CHECK(readFile(dir.file) == "{\n  \"my_note\": \"keep\",\n  \"resolution_scale\": 2\n}\n");
}

void testStoreFlush() {
    LogCapture log;
    StoreDir dir("tox2_settings_store_flush_");
    {
        tox2::SettingsStore store(dir.file, kSpecs, kSlowTiming);
        CHECK(store.flush(std::chrono::seconds(1)));  // nothing pending
        CHECK(store.writes() == 0 && !stdfs::exists(dir.file));

        // A flush writes at once instead of waiting for the quiet time.
        store.set("frame_limit", "60");
        CHECK(store.flush(std::chrono::seconds(1)));
        CHECK(store.writes() == 1);
        CHECK(readFile(dir.file) == "{\n  \"frame_limit\": \"60\"\n}\n");
        CHECK(store.flush(std::chrono::seconds(1)));
        CHECK(store.writes() == 1);

        // Keys that are not saved are ignored, and reported once.
        store.set("elf", "x");
        store.set("elf", "y");
        store.set("nothing", "z");
        CHECK(store.flush(std::chrono::seconds(1)));
        CHECK(store.writes() == 1);
        CHECK(readFile(dir.file) == "{\n  \"frame_limit\": \"60\"\n}\n");
        CHECK(log.count("\"elf\" is not a setting that is saved") == 1);
        CHECK(log.count("\"nothing\" is not a setting that is saved") == 1);

        store.set("fullscreen", "on");
    }
    // The destructor flushes what is still pending.
    CHECK(readFile(dir.file) == "{\n  \"frame_limit\": \"60\",\n  \"fullscreen\": true\n}\n");
}

void testStoreRejectsBadValue() {
    LogCapture log;
    StoreDir dir("tox2_settings_store_bad_");
    tox2::SettingsStore store(dir.file, kSpecs, kSlowTiming);
    // A bad value is dropped when set, so it cannot block the changes after it.
    store.set("fullscreen", "maybe");
    store.set("frame_limit", "off");
    CHECK(store.flush(std::chrono::seconds(1)));
    CHECK(store.writes() == 1);
    CHECK(readFile(dir.file) == "{\n  \"frame_limit\": \"off\"\n}\n");
    CHECK(log.count("cannot save fullscreen: ") == 1);

    // An earlier valid value of the same key stays pending.
    store.set("resolution_scale", "2");
    store.set("resolution_scale", "x");
    CHECK(store.flush(std::chrono::seconds(1)));
    CHECK(readFile(dir.file) == "{\n  \"frame_limit\": \"off\",\n  \"resolution_scale\": 2\n}\n");
}

void testSettingsStore() {
    testStoreWrites();
    testStoreFailures();
    testStoreReadOnlyFile();
    testStoreFlush();
    testStoreRejectsBadValue();
}

}  // namespace

int main() {
    testKeyNames();
    testEmptyObject();
    testPaths();
    testStrings();
    testUnsigned();
    testFlags();
    testSwitches();
    testFrameLists();
    testStringLists();
    testNull();
    testDocumentShape();
    testUnknownKeys();
    testJsonSyntax();
    testDuplicateKeys();
    testUpdateNewFile();
    testUpdateKinds();
    testUpdateKeepsOtherMembers();
    testCheckConfigChange();
    testUpdateFailures();
    testWriteFileAtomic();
    testOptionTable();
    testDefaultLines();
    testValues();
    testErrorMessages();
    testCheckOrder();
    testPadScriptAndAudioDump();
    testApplyOptions();
    testConfigFileValues();
    testConfigFileLookup();
    testConfigFileErrors();
    testConfigFileDroppedKeys();
    testWindowAndFilterOptions();
    testConfigApply();
    testSettingsStore();
    std::printf("game options tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}
