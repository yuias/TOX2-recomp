// config.json support: reading the file into raw option values, and rewriting it with changed settings.

#include "app/config_file.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <exception>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>

#include <yyjson.h>

namespace tox2 {
namespace {

using Json = const yyjson_val*;
namespace stdfs = std::filesystem;

template <typename... Args>
std::unexpected<std::string> fail(std::format_string<Args...> fmt, Args&&... args) {
    return std::unexpected(std::format(fmt, std::forward<Args>(args)...));
}

std::string_view stringOf(Json value) { return {yyjson_get_str(value), yyjson_get_len(value)}; }

const char* typeName(Json value) {
    if (yyjson_is_null(value)) return "null";
    if (yyjson_is_obj(value)) return "object";
    if (yyjson_is_arr(value)) return "array";
    if (yyjson_is_str(value)) return "string";
    if (yyjson_is_bool(value)) return "boolean";
    return "number";
}

// Calls fn(key, value) for each member in document order; stops when fn returns false.
template <typename Fn>
void forEachMember(Json object, Fn&& fn) {
    yyjson_obj_iter it = yyjson_obj_iter_with(object);
    while (yyjson_val* key = yyjson_obj_iter_next(&it)) {
        if (!fn(stringOf(key), static_cast<Json>(yyjson_obj_iter_get_val(key)))) {
            return;
        }
    }
}

// yyjson keeps duplicate object keys, and a silent "last one wins" would hide a typo. Walks every object
// in the document with an explicit stack, so nesting depth cannot overflow.
std::optional<std::string> findDuplicateKey(Json root) {
    std::vector<Json> pending{root};
    std::vector<std::string_view> keys;
    while (!pending.empty()) {
        const Json node = pending.back();
        pending.pop_back();
        if (yyjson_is_obj(node)) {
            keys.clear();
            forEachMember(node, [&](std::string_view key, Json value) {
                keys.push_back(key);
                if (yyjson_is_obj(value) || yyjson_is_arr(value)) pending.push_back(value);
                return true;
            });
            std::ranges::sort(keys);
            const auto dup = std::ranges::adjacent_find(keys);
            if (dup != keys.end()) {
                return std::string(*dup);
            }
        } else if (yyjson_is_arr(node)) {
            yyjson_arr_iter it = yyjson_arr_iter_with(node);
            while (yyjson_val* value = yyjson_arr_iter_next(&it)) {
                if (yyjson_is_obj(value) || yyjson_is_arr(value)) pending.push_back(value);
            }
        }
    }
    return std::nullopt;
}

using DocPtr = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>;

// A UTF-8 BOM is skipped, as Windows editors write one. Comments and trailing commas stay errors.
std::expected<DocPtr, std::string> readDocument(std::string_view text, yyjson_read_flag flags) {
    // The input is not modified without YYJSON_READ_INSITU; the API just has no const overload.
    yyjson_read_err read_err;
    DocPtr doc(yyjson_read_opts(const_cast<char*>(text.data()), text.size(), flags, nullptr, &read_err),
               &yyjson_doc_free);
    if (!doc) {
        const std::string_view before = text.substr(0, std::min(read_err.pos, text.size()));
        const auto line = 1 + std::ranges::count(before, '\n');
        const std::size_t column = before.size() - (before.rfind('\n') + 1);  // npos + 1 wraps to 0
        return fail("invalid JSON: {} at line {}, column {} (byte {})", read_err.msg, line, column + 1, read_err.pos);
    }
    return doc;
}

std::expected<void, std::string> readPath(Json value, const stdfs::path& base_dir, ConfigValue& out) {
    if (!yyjson_is_str(value)) {
        return fail("\"{}\": expected a path string, got {}", out.key, typeName(value));
    }
    const std::string_view text = stringOf(value);
    if (text.empty()) {
        return fail("\"{}\": the path is empty", out.key);
    }
    // "\u0000" is valid JSON but cannot be part of a file name.
    if (text.find('\0') != std::string_view::npos) {
        return fail("\"{}\": the path contains a NUL character", out.key);
    }
    stdfs::path path;
    try {
        path = stdfs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
    } catch (const std::exception&) {
        return fail("\"{}\": the path is not valid UTF-8", out.key);
    }
    if (path.is_relative()) {
        path = base_dir / path;
    }
    out.path = path.lexically_normal();
    return {};
}

std::expected<void, std::string> readString(Json value, ConfigValue& out) {
    if (yyjson_is_str(value)) {
        out.text = std::string(stringOf(value));
    } else if (yyjson_is_uint(value)) {
        out.text = std::to_string(yyjson_get_uint(value));
    } else {
        return fail("\"{}\": expected a string or a non-negative whole number, got {}", out.key, typeName(value));
    }
    return {};
}

std::expected<void, std::string> readUnsigned(Json value, ConfigValue& out) {
    // Negative numbers, fractions and values beyond 64 bits are not "uint" in yyjson.
    if (!yyjson_is_uint(value)) {
        return fail("\"{}\": expected a non-negative whole number, got {}", out.key, typeName(value));
    }
    out.text = std::to_string(yyjson_get_uint(value));
    return {};
}

std::expected<void, std::string> readFlag(Json value, ConfigValue& out) {
    if (!yyjson_is_bool(value)) {
        return fail("\"{}\": expected true or false, got {}", out.key, typeName(value));
    }
    out.flag = yyjson_get_bool(value);
    return {};
}

std::expected<void, std::string> readSwitch(Json value, ConfigValue& out) {
    if (yyjson_is_bool(value)) {
        out.text = yyjson_get_bool(value) ? "on" : "off";
        return {};
    }
    if (yyjson_is_str(value)) {
        const std::string_view text = stringOf(value);
        if (text == "on" || text == "off") {
            out.text = std::string(text);
            return {};
        }
    }
    return fail("\"{}\": expected true, false, \"on\" or \"off\", got {}", out.key, typeName(value));
}

std::expected<void, std::string> readFrameList(Json value, ConfigValue& out) {
    if (yyjson_is_str(value)) {
        out.items.emplace_back(stringOf(value));
        return {};
    }
    if (!yyjson_is_arr(value)) {
        return fail("\"{}\": expected a string or a non-empty array of positive whole numbers, got {}", out.key,
                    typeName(value));
    }
    if (yyjson_arr_size(value) == 0) {
        return fail("\"{}\": expected a non-empty array of positive whole numbers, got an empty array", out.key);
    }
    std::string joined;
    std::size_t index = 0;
    yyjson_arr_iter it = yyjson_arr_iter_with(const_cast<yyjson_val*>(value));
    while (yyjson_val* element = yyjson_arr_iter_next(&it)) {
        if (!yyjson_is_uint(element) || yyjson_get_uint(element) == 0) {
            return fail("\"{}\"[{}]: expected a positive whole number, got {}", out.key, index, typeName(element));
        }
        if (index != 0) joined += ',';
        joined += std::to_string(yyjson_get_uint(element));
        index++;
    }
    out.items.push_back(std::move(joined));
    return {};
}

std::expected<void, std::string> readStringList(Json value, ConfigValue& out) {
    if (!yyjson_is_arr(value)) {
        return fail("\"{}\": expected an array of strings, got {}", out.key, typeName(value));
    }
    std::size_t index = 0;
    yyjson_arr_iter it = yyjson_arr_iter_with(const_cast<yyjson_val*>(value));
    while (yyjson_val* element = yyjson_arr_iter_next(&it)) {
        if (!yyjson_is_str(element)) {
            return fail("\"{}\"[{}]: expected a string, got {}", out.key, index, typeName(element));
        }
        out.items.emplace_back(stringOf(element));
        index++;
    }
    return {};
}

std::expected<ConfigValue, std::string> readValue(const OptionSpec& spec, std::string_view key, Json value,
                                                  const stdfs::path& base_dir) {
    ConfigValue out;
    out.option = std::string(spec.name);
    out.key = std::string(key);
    if (yyjson_is_null(value)) {
        return fail("\"{}\": null is not a value; remove the key to use the default", key);
    }
    std::expected<void, std::string> result;
    switch (spec.kind) {
        case OptionKind::Path: result = readPath(value, base_dir, out); break;
        case OptionKind::String: result = readString(value, out); break;
        case OptionKind::Unsigned: result = readUnsigned(value, out); break;
        case OptionKind::Flag: result = readFlag(value, out); break;
        case OptionKind::Switch: result = readSwitch(value, out); break;
        case OptionKind::FrameList: result = readFrameList(value, out); break;
        case OptionKind::StringList: result = readStringList(value, out); break;
    }
    if (!result) {
        return std::unexpected(std::move(result.error()));
    }
    return out;
}

}  // namespace

std::string configKeyFor(std::string_view option_name) {
    std::string key(option_name);
    std::ranges::replace(key, '-', '_');
    return key;
}

std::expected<ConfigFileContents, std::string> parseConfigFile(std::string_view text, const stdfs::path& base_dir,
                                                               std::span<const OptionSpec> specs) {
    auto doc = readDocument(text, YYJSON_READ_ALLOW_BOM);
    if (!doc) {
        return std::unexpected(std::move(doc.error()));
    }
    const Json root = yyjson_doc_get_root(doc->get());
    if (const auto duplicate = findDuplicateKey(root)) {
        return fail("duplicate key \"{}\" in an object", *duplicate);
    }
    if (!yyjson_is_obj(root)) {
        return fail("the top level must be an object");
    }

    ConfigFileContents contents;
    std::optional<std::string> error;
    forEachMember(root, [&](std::string_view key, Json value) {
        const auto spec = std::ranges::find_if(specs, [&](const OptionSpec& s) { return configKeyFor(s.name) == key; });
        if (spec == specs.end()) {
            contents.warnings.push_back(std::format("unknown key \"{}\" ignored", key));
            return true;
        }
        auto parsed = readValue(*spec, key, value, base_dir);
        if (!parsed) {
            error = std::move(parsed.error());
            return false;
        }
        contents.values.push_back(std::move(*parsed));
        return true;
    });
    if (error) {
        return std::unexpected(std::move(*error));
    }
    return contents;
}

namespace {

using MutDocPtr = std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)>;

const OptionSpec* findPersistedSpec(std::string_view key, std::span<const OptionSpec> specs) {
    const auto spec = std::ranges::find_if(
            specs, [&](const OptionSpec& s) { return s.persisted && configKeyFor(s.name) == key; });
    return spec == specs.end() ? nullptr : &*spec;
}

// The reason the kind of `spec` cannot hold the text of `change`; empty when it can.
std::string checkValue(const OptionSpec& spec, const ConfigChange& change) {
    switch (spec.kind) {
        case OptionKind::Unsigned: {
            std::uint64_t number = 0;
            const char* end = change.value.data() + change.value.size();
            const auto [ptr, ec] = std::from_chars(change.value.data(), end, number);
            if (ec != std::errc() || ptr != end) {
                return std::format("\"{}\": \"{}\" is not a non-negative whole number", change.key, change.value);
            }
            return {};
        }
        case OptionKind::String: return {};
        case OptionKind::Switch:
            if (change.value != "on" && change.value != "off") {
                return std::format("\"{}\": \"{}\" is not on or off", change.key, change.value);
            }
            return {};
        case OptionKind::Path:
        case OptionKind::Flag:
        case OptionKind::FrameList:
        case OptionKind::StringList: break;
    }
    return std::format("\"{}\": this kind of setting cannot be saved", change.key);
}

// Builds the JSON value for `change` in `doc`; null with `error` set when the kind cannot hold the text.
yyjson_mut_val* makeValue(yyjson_mut_doc* doc, const OptionSpec& spec, const ConfigChange& change, std::string& error) {
    error = checkValue(spec, change);
    if (!error.empty()) return nullptr;
    switch (spec.kind) {
        case OptionKind::Unsigned: return yyjson_mut_uint(doc, std::stoull(change.value));
        case OptionKind::String: return yyjson_mut_strncpy(doc, change.value.data(), change.value.size());
        default: return yyjson_mut_bool(doc, change.value == "on");
    }
}

bool isBlank(std::string_view text) {
    return std::ranges::all_of(text, [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; });
}

}  // namespace

std::expected<void, std::string> checkConfigChange(const ConfigChange& change, std::span<const OptionSpec> specs) {
    const OptionSpec* spec = findPersistedSpec(change.key, specs);
    if (spec == nullptr) {
        return fail("\"{}\" is not a setting that is saved", change.key);
    }
    const std::string error = checkValue(*spec, change);
    if (!error.empty()) return std::unexpected(error);
    return {};
}

std::expected<std::string, std::string> updateConfigText(std::string_view original,
                                                         std::span<const ConfigChange> changes,
                                                         std::span<const OptionSpec> specs) {
    // A file that is empty (or holds only a BOM) is a fresh file, not a syntax error.
    constexpr std::string_view kBom = "\xEF\xBB\xBF";
    const std::string_view without_bom = original.starts_with(kBom) ? original.substr(kBom.size()) : original;
    const std::string_view source = isBlank(without_bom) ? std::string_view("{}") : original;

    // Numbers are kept as their source text so that rewriting does not reformat values it does not own.
    auto doc = readDocument(source, YYJSON_READ_ALLOW_BOM | YYJSON_READ_NUMBER_AS_RAW);
    if (!doc) {
        return std::unexpected(std::move(doc.error()));
    }
    const Json read_root = yyjson_doc_get_root(doc->get());
    if (const auto duplicate = findDuplicateKey(read_root)) {
        return fail("duplicate key \"{}\" in an object", *duplicate);
    }
    if (!yyjson_is_obj(read_root)) {
        return fail("the top level must be an object");
    }

    MutDocPtr out(yyjson_doc_mut_copy(doc->get(), nullptr), &yyjson_mut_doc_free);
    if (!out) {
        return fail("out of memory");
    }
    yyjson_mut_val* root = yyjson_mut_doc_get_root(out.get());
    for (const ConfigChange& change : changes) {
        const OptionSpec* spec = findPersistedSpec(change.key, specs);
        if (spec == nullptr) {
            return fail("\"{}\" is not a setting that is saved", change.key);
        }
        std::string error;
        yyjson_mut_val* value = makeValue(out.get(), *spec, change, error);
        if (value == nullptr) {
            return std::unexpected(std::move(error));
        }
        yyjson_mut_val* key = yyjson_mut_strncpy(out.get(), change.key.data(), change.key.size());
        // Replacing in place keeps the member's position; a new member goes to the end.
        const bool done = yyjson_mut_obj_getn(root, change.key.data(), change.key.size()) != nullptr
                                  ? yyjson_mut_obj_replace(root, key, value)
                                  : yyjson_mut_obj_add(root, key, value);
        if (!done) {
            return fail("out of memory");
        }
    }

    size_t length = 0;
    const std::unique_ptr<char, decltype(&std::free)> text(
            yyjson_mut_write(out.get(), YYJSON_WRITE_PRETTY_TWO_SPACES | YYJSON_WRITE_NEWLINE_AT_END, &length),
            &std::free);
    if (!text) {
        return fail("cannot encode the JSON text");
    }
    return std::string(text.get(), length);
}

std::expected<void, std::string> writeFileAtomic(const stdfs::path& path, std::string_view text) {
    stdfs::path temp = path;
    temp += ".tmp";
    std::error_code ec;
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            return fail("cannot create {}: {}", temp.string(), std::generic_category().message(errno));
        }
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.flush();
        if (!file) {
            const std::string reason = std::generic_category().message(errno);
            file.close();
            stdfs::remove(temp, ec);
            return fail("cannot write {}: {}", temp.string(), reason);
        }
    }
    stdfs::rename(temp, path, ec);
    if (ec) {
        const std::string reason = ec.message();
        stdfs::remove(temp, ec);
        return fail("cannot replace {}: {}", path.string(), reason);
    }
    return {};
}

}  // namespace tox2
