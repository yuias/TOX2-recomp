// VFS configuration file: parsing, validation and composition with the command-line options.

#include "lv2/vfs_config.h"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
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

void warnUnknownKeys(Json object, std::initializer_list<std::string_view> known, std::string_view where,
                     std::vector<std::string>& warnings) {
    forEachMember(object, [&](std::string_view key, Json) {
        if (std::ranges::find(known, key) == known.end()) {
            warnings.push_back(std::format("{}: unknown key \"{}\" ignored", where, key));
        }
        return true;
    });
}

// The guest path must be absolute and already normalised: no empty, "." or ".." component, no
// trailing slash, no control character. `min_parts` counts components.
std::expected<size_t, std::string> checkGuestPath(std::string_view path, size_t min_parts) {
    if (!path.starts_with('/')) {
        return fail("guest path \"{}\" is not absolute", path);
    }
    size_t parts = 0;
    for (size_t pos = 1; pos <= path.size();) {
        const size_t end = std::min(path.find('/', pos), path.size());
        const std::string_view name = path.substr(pos, end - pos);
        if (name.empty() || name == "." || name == "..") {
            return fail(
                    "guest path \"{}\" is not normalised (empty, \".\" or \"..\" component, or a "
                    "trailing '/')",
                    path);
        }
        if (std::ranges::any_of(name, [](char c) { return static_cast<unsigned char>(c) < 0x20; })) {
            return fail("guest path \"{}\" contains a control character", path);
        }
        parts++;
        pos = end + 1;
    }
    if (parts < min_parts) {
        return fail("guest path \"{}\" needs at least {} component(s)", path, min_parts);
    }
    return parts;
}

std::expected<stdfs::path, std::string> hostPath(Json value, std::string_view where, const stdfs::path& base_dir) {
    if (!yyjson_is_str(value)) {
        return fail("{}: expected a path string, got {}", where, typeName(value));
    }
    const std::string_view text = stringOf(value);
    if (text.empty()) {
        return fail("{}: the path is empty", where);
    }
    stdfs::path path;
    try {
        const std::u8string utf8(reinterpret_cast<const char8_t*>(text.data()), text.size());
        path = stdfs::path(utf8);
    } catch (const std::exception&) {
        return fail("{}: the path is not valid UTF-8", where);
    }
    if (path.is_relative()) {
        path = base_dir / path;
    }
    return path.lexically_normal();
}

bool defaultReadOnly(std::string_view name) { return name == "dev_bdvd" || name == "app_home"; }

std::expected<bool, std::string> readOnlyFlag(Json object, std::string_view where, bool fallback) {
    const Json flag = yyjson_obj_get(object, "read_only");
    if (flag == nullptr) {
        return fallback;
    }
    if (!yyjson_is_bool(flag)) {
        return fail("{}: read_only must be true or false", where);
    }
    return yyjson_get_bool(flag);
}

std::expected<VfsMount, std::string> parseMount(std::string_view key, Json value, const stdfs::path& base_dir,
                                                std::vector<std::string>& warnings) {
    const std::string where = std::format("mounts[\"{}\"]", key);
    const auto parts = checkGuestPath(key, 1);
    if (!parts) {
        return fail("{}: {}", where, parts.error());
    }
    if (*parts != 1) {
        return fail(
                "{}: nested mount points overlap their device; mount a device root such as "
                "\"/dev_hdd0\"",
                where);
    }
    if (!yyjson_is_obj(value)) {
        return fail("{}: expected an object with \"host\"", where);
    }
    warnUnknownKeys(value, {"host", "read_only", "block_size"}, where, warnings);

    VfsMount mount;
    mount.name = std::string(key.substr(1));
    const Json host = yyjson_obj_get(value, "host");
    if (host == nullptr) {
        return fail("{}: \"host\" is missing", where);
    }
    auto path = hostPath(host, where + ".host", base_dir);
    if (!path) {
        return std::unexpected(path.error());
    }
    mount.host = std::move(*path);
    const auto read_only = readOnlyFlag(value, where, defaultReadOnly(mount.name));
    if (!read_only) {
        return std::unexpected(read_only.error());
    }
    mount.read_only = *read_only;
    if (const Json block = yyjson_obj_get(value, "block_size"); block != nullptr) {
        if (!yyjson_is_uint(block)) {
            return fail("{}: block_size must be a positive integer", where);
        }
        const std::uint64_t size = yyjson_get_uint(block);
        if (size < 512 || size > (1u << 20) || (size & (size - 1)) != 0) {
            return fail("{}: block_size must be a power of two from 512 to 1048576", where);
        }
        mount.block_size = static_cast<std::uint32_t>(size);
    }
    return mount;
}

std::expected<VfsFileOverride, std::string> parseFileOverride(std::string_view key, Json value,
                                                              const stdfs::path& base_dir,
                                                              std::vector<std::string>& warnings) {
    const std::string where = std::format("files[\"{}\"]", key);
    const auto parts = checkGuestPath(key, 2);
    if (!parts) {
        return fail("{}: {}", where, parts.error());
    }
    VfsFileOverride file;
    file.guest = std::string(key);
    if (yyjson_is_str(value)) {
        auto path = hostPath(value, where, base_dir);
        if (!path) {
            return std::unexpected(path.error());
        }
        file.host = std::move(*path);
        return file;
    }
    if (!yyjson_is_obj(value)) {
        return fail("{}: expected a host path string or an object with \"host\"", where);
    }
    warnUnknownKeys(value, {"host", "read_only"}, where, warnings);
    const Json host = yyjson_obj_get(value, "host");
    if (host == nullptr) {
        return fail("{}: \"host\" is missing", where);
    }
    auto path = hostPath(host, where + ".host", base_dir);
    if (!path) {
        return std::unexpected(path.error());
    }
    file.host = std::move(*path);
    const auto read_only = readOnlyFlag(value, where, true);
    if (!read_only) {
        return std::unexpected(read_only.error());
    }
    file.read_only = *read_only;
    return file;
}

// yyjson keeps duplicate object keys; a key that appears twice in one object is an error. Walks
// every object in the document with an explicit stack, so nesting depth cannot overflow.
std::optional<std::string> findDuplicateKey(Json root) {
    std::vector<Json> pending{root};
    std::vector<std::string_view> keys;
    while (!pending.empty()) {
        const Json node = pending.back();
        pending.pop_back();
        const size_t first_child = pending.size();
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
        // The stack pops from the back: reverse the children so they are visited in document order.
        std::reverse(pending.begin() + static_cast<std::ptrdiff_t>(first_child), pending.end());
    }
    return std::nullopt;
}

void replaceMounts(std::vector<VfsMount>& mounts, std::vector<VfsMount> replacement) {
    std::erase_if(mounts, [&](const VfsMount& m) {
        return std::ranges::any_of(replacement, [&](const VfsMount& r) { return r.name == m.name; });
    });
    for (VfsMount& mount : replacement) {
        mounts.push_back(std::move(mount));
    }
}

stdfs::path canonicalOrNormal(const stdfs::path& path) {
    std::error_code ec;
    stdfs::path canonical = stdfs::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : canonical;
}

// True when `inner` is `outer` or below it.
bool isInside(const stdfs::path& inner, const stdfs::path& outer) {
    const stdfs::path relative = inner.lexically_relative(outer);
    return !relative.empty() && *relative.begin() != "..";
}

}  // namespace

std::expected<VfsConfigFile, std::string> parseVfsConfig(std::string_view json_text, const stdfs::path& base_dir) {
    // The input is not modified without YYJSON_READ_INSITU; the API just has no const overload.
    // A UTF-8 BOM is skipped, as Windows editors write one.
    yyjson_read_err read_err;
    const std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc(
            yyjson_read_opts(const_cast<char*>(json_text.data()), json_text.size(), YYJSON_READ_ALLOW_BOM, nullptr,
                             &read_err),
            &yyjson_doc_free);
    if (!doc) {
        const std::string_view before = json_text.substr(0, std::min(read_err.pos, json_text.size()));
        const auto line = 1 + std::ranges::count(before, '\n');
        const size_t column = before.size() - (before.rfind('\n') + 1);  // npos + 1 wraps to 0
        return fail("invalid JSON: {} at line {}, column {} (byte {})", read_err.msg, line, column + 1, read_err.pos);
    }
    const Json root = yyjson_doc_get_root(doc.get());
    if (const auto duplicate = findDuplicateKey(root)) {
        return fail("duplicate key \"{}\" in an object", *duplicate);
    }
    if (!yyjson_is_obj(root)) {
        return fail("the top level must be an object");
    }

    VfsConfigFile config;
    warnUnknownKeys(root, {"version", "game_dir", "hdd_dir", "sdata_cache", "mounts", "files"}, "top level",
                    config.warnings);

    const Json version = yyjson_obj_get(root, "version");
    if (version == nullptr) {
        return fail("\"version\" is missing (current version: {})", kVfsConfigVersion);
    }
    const bool version_ok = yyjson_is_uint(version)
                                    ? yyjson_get_uint(version) == kVfsConfigVersion
                                    : yyjson_is_sint(version) && yyjson_get_sint(version) == kVfsConfigVersion;
    if (!version_ok) {
        const std::unique_ptr<char, decltype(&std::free)> dumped(
                yyjson_val_write(version, YYJSON_WRITE_NOFLAG, nullptr), &std::free);
        return fail("unsupported \"version\" {} (this build reads version {})",
                    dumped ? dumped.get() : typeName(version), kVfsConfigVersion);
    }

    // Shorthands expand to the mounts the matching command-line options create.
    std::vector<std::pair<std::string, VfsMount>> origin;  // mount and where it came from
    auto add = [&](std::vector<VfsMount> mounts, std::string_view source) {
        for (VfsMount& mount : mounts) {
            origin.emplace_back(std::string(source), std::move(mount));
        }
    };
    if (const Json it = yyjson_obj_get(root, "game_dir")) {
        const auto path = hostPath(it, "game_dir", base_dir);
        if (!path) {
            return std::unexpected(path.error());
        }
        add(gameDirMounts(*path), "game_dir");
    }
    if (const Json it = yyjson_obj_get(root, "hdd_dir")) {
        const auto path = hostPath(it, "hdd_dir", base_dir);
        if (!path) {
            return std::unexpected(path.error());
        }
        add(hddDirMounts(*path), "hdd_dir");
    }
    if (const Json it = yyjson_obj_get(root, "sdata_cache")) {
        auto path = hostPath(it, "sdata_cache", base_dir);
        if (!path) {
            return std::unexpected(path.error());
        }
        config.sdata_cache = std::move(*path);
    }

    if (const Json it = yyjson_obj_get(root, "mounts")) {
        if (!yyjson_is_obj(it)) {
            return fail("\"mounts\" must be an object");
        }
        std::optional<std::string> error;
        forEachMember(it, [&](std::string_view key, Json value) {
            auto mount = parseMount(key, value, base_dir, config.warnings);
            if (!mount) {
                error = std::move(mount.error());
                return false;
            }
            origin.emplace_back("mounts", std::move(*mount));
            return true;
        });
        if (error) {
            return std::unexpected(std::move(*error));
        }
    }
    for (size_t i = 0; i < origin.size(); i++) {
        for (size_t j = i + 1; j < origin.size(); j++) {
            if (origin[i].second.name == origin[j].second.name) {
                return fail("/{} is mounted twice (by {} and {})", origin[i].second.name, origin[i].first,
                            origin[j].first);
            }
        }
    }
    for (auto& [source, mount] : origin) {
        config.mounts.push_back(std::move(mount));
    }

    if (const Json it = yyjson_obj_get(root, "files")) {
        if (!yyjson_is_obj(it)) {
            return fail("\"files\" must be an object");
        }
        std::set<std::string> keys;
        std::optional<std::string> error;
        forEachMember(it, [&](std::string_view key, Json value) {
            auto file = parseFileOverride(key, value, base_dir, config.warnings);
            if (!file) {
                error = std::move(file.error());
                return false;
            }
            keys.insert(file->guest);
            config.files.push_back(std::move(*file));
            return true;
        });
        if (error) {
            return std::unexpected(std::move(*error));
        }
        // A file cannot also be a directory that holds another override.
        for (const std::string& key : keys) {
            for (size_t slash = key.find('/', 1); slash != std::string::npos; slash = key.find('/', slash + 1)) {
                if (keys.contains(key.substr(0, slash))) {
                    return fail("files[\"{}\"] lies below files[\"{}\"], which is a file override", key,
                                key.substr(0, slash));
                }
            }
        }
    }
    return config;
}

std::expected<VfsConfigFile, std::string> loadVfsConfig(const stdfs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return fail("{}: cannot open the file", file.string());
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return fail("{}: cannot read the file", file.string());
    }
    std::error_code ec;
    stdfs::path absolute = stdfs::absolute(file, ec);
    if (ec) {
        absolute = file;
    }
    auto config = parseVfsConfig(text, absolute.parent_path());
    if (!config) {
        return fail("{}: {}", file.string(), config.error());
    }
    return config;
}

std::expected<VfsLayout, std::string> composeVfsLayout(const VfsConfigFile* config, const VfsCliOptions& cli) {
    VfsLayout layout;
    if (config != nullptr) {
        layout.mounts = config->mounts;
        layout.files = config->files;
        layout.sdata_cache = config->sdata_cache;
    }
    if (cli.game_dir) {
        replaceMounts(layout.mounts, gameDirMounts(*cli.game_dir));
    }
    if (cli.hdd_dir) {
        replaceMounts(layout.mounts, hddDirMounts(*cli.hdd_dir));
    }
    if (cli.sdata_cache) {
        layout.sdata_cache = *cli.sdata_cache;
    }

    std::error_code ec;
    // The /app_home that --game-dir or "game_dir" derives may be absent (a game directory without
    // PS3_GAME/USRDIR still starts); one spelled out in "mounts" must exist.
    const auto bdvd = std::ranges::find(layout.mounts, "dev_bdvd", &VfsMount::name);
    for (const VfsMount& mount : layout.mounts) {
        const stdfs::file_status status = stdfs::status(mount.host, ec);
        const bool derived_app_home = mount.name == "app_home" && bdvd != layout.mounts.end() &&
                                      mount.host == bdvd->host / "PS3_GAME" / "USRDIR";
        if (stdfs::exists(status) ? !stdfs::is_directory(status) : mount.read_only && !derived_app_home) {
            return fail("/{}: host directory {} {}", mount.name, mount.host.string(),
                        stdfs::exists(status) ? "is not a directory" : "does not exist");
        }
    }
    for (size_t i = 0; i < layout.mounts.size(); i++) {
        for (size_t j = i + 1; j < layout.mounts.size(); j++) {
            const VfsMount& a = layout.mounts[i];
            const VfsMount& b = layout.mounts[j];
            if (a.read_only && b.read_only) {
                continue;
            }
            const stdfs::path host_a = canonicalOrNormal(a.host);
            const stdfs::path host_b = canonicalOrNormal(b.host);
            if (isInside(host_a, host_b) || isInside(host_b, host_a)) {
                return fail(
                        "/{} and /{} overlap on the host ({} and {}); a writable mount may not share "
                        "or contain another mount's directory",
                        a.name, b.name, a.host.string(), b.host.string());
            }
        }
    }
    for (const VfsFileOverride& file : layout.files) {
        const stdfs::file_status status = stdfs::status(file.host, ec);
        if (stdfs::exists(status)) {
            if (!stdfs::is_regular_file(status)) {
                return fail("files[\"{}\"]: host path {} is not a regular file", file.guest, file.host.string());
            }
        } else if (file.read_only) {
            return fail("files[\"{}\"]: host file {} does not exist", file.guest, file.host.string());
        } else if (!stdfs::is_directory(stdfs::status(file.host.parent_path(), ec))) {
            return fail("files[\"{}\"]: the directory of the writable host file {} does not exist", file.guest,
                        file.host.string());
        }
    }
    return layout;
}

}  // namespace tox2
