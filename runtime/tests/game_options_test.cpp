// Self-checking tests for the config file reader and writer. No game data and no player config file are read.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "app/config_file.h"

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
    testUpdateFailures();
    testWriteFileAtomic();
    std::printf("game options tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}
