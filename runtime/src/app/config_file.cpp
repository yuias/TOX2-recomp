// config.json support: reading the file into raw option values.

#include "app/config_file.h"

#include <algorithm>
#include <exception>
#include <format>
#include <memory>
#include <optional>
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
    // The input is not modified without YYJSON_READ_INSITU; the API just has no const overload.
    // A UTF-8 BOM is skipped, as Windows editors write one. Comments and trailing commas stay errors.
    yyjson_read_err read_err;
    const std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc(
            yyjson_read_opts(const_cast<char*>(text.data()), text.size(), YYJSON_READ_ALLOW_BOM, nullptr, &read_err),
            &yyjson_doc_free);
    if (!doc) {
        const std::string_view before = text.substr(0, std::min(read_err.pos, text.size()));
        const auto line = 1 + std::ranges::count(before, '\n');
        const std::size_t column = before.size() - (before.rfind('\n') + 1);  // npos + 1 wraps to 0
        return fail("invalid JSON: {} at line {}, column {} (byte {})", read_err.msg, line, column + 1, read_err.pos);
    }
    const Json root = yyjson_doc_get_root(doc.get());
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

}  // namespace tox2
