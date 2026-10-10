#include "core/param_sfo.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>

namespace dal {
namespace {

constexpr std::uint32_t kMagic = 0x46535000;  // "\0PSF" read as a little-endian u32 is 0x46535000
constexpr std::size_t kHeaderSize = 20;
constexpr std::size_t kIndexSize = 16;
constexpr std::uint16_t kFormatUtf8Raw = 0x0004;
constexpr std::uint16_t kFormatUtf8 = 0x0204;
constexpr std::uint16_t kFormatU32 = 0x0404;

std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void putLe16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void putLe32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; i++) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::uint32_t alignUp4(std::uint32_t v) { return (v + 3) & ~3u; }

std::uint32_t textCapacity(const std::string& key, const std::string& value) {
    std::uint32_t min_capacity = 0;
    if (key == "TITLE")
        min_capacity = 128;
    else if (key == "TITLE_ID")
        min_capacity = 16;
    else if (key == "VERSION")
        min_capacity = 8;
    else if (key == "CATEGORY")
        min_capacity = 4;
    return alignUp4(std::max<std::uint32_t>(min_capacity, static_cast<std::uint32_t>(value.size()) + 1));
}

}  // namespace

std::expected<ParamSfo, std::string> parseParamSfo(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kHeaderSize || le32(bytes.data()) != kMagic) {
        return std::unexpected("not a PARAM.SFO (bad magic)");
    }
    const std::uint32_t key_table = le32(bytes.data() + 8);
    const std::uint32_t data_table = le32(bytes.data() + 12);
    const std::uint32_t count = le32(bytes.data() + 16);
    if (count > (bytes.size() - kHeaderSize) / kIndexSize || key_table > bytes.size() || data_table > bytes.size()) {
        return std::unexpected("PARAM.SFO is truncated");
    }

    ParamSfo sfo;
    for (std::uint32_t i = 0; i < count; i++) {
        const std::uint8_t* entry = bytes.data() + kHeaderSize + static_cast<std::size_t>(i) * kIndexSize;
        const std::uint32_t key_off = key_table + le16(entry);
        const std::uint16_t format = le16(entry + 2);
        const std::uint32_t length = le32(entry + 4);
        const std::uint64_t data_off = static_cast<std::uint64_t>(data_table) + le32(entry + 12);
        if (key_off >= bytes.size()) {
            return std::unexpected(std::format("PARAM.SFO entry {} has its key outside the file", i));
        }
        const char* key_begin = reinterpret_cast<const char*>(bytes.data()) + key_off;
        const std::size_t key_avail = bytes.size() - key_off;
        const std::size_t key_len = strnlen(key_begin, key_avail);
        if (key_len == key_avail) {
            return std::unexpected(std::format("PARAM.SFO entry {} has an unterminated key", i));
        }
        const std::string key(key_begin, key_len);

        if (format != kFormatU32 && format != kFormatUtf8 && format != kFormatUtf8Raw) continue;
        const std::uint32_t used = format == kFormatU32 ? 4 : length;
        if (data_off + used > bytes.size() || (format == kFormatU32 && length < 4)) {
            return std::unexpected(std::format("PARAM.SFO entry '{}' has its data outside the file", key));
        }
        const std::uint8_t* data = bytes.data() + data_off;
        if (format == kFormatU32) {
            sfo.ints[key] = le32(data);
        } else {
            const std::size_t text_len = strnlen(reinterpret_cast<const char*>(data), length);
            sfo.strings[key] = std::string(reinterpret_cast<const char*>(data), text_len);
        }
    }
    return sfo;
}

std::expected<ParamSfo, std::string> readParamSfoFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected("cannot read " + path.string());
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parseParamSfo(bytes);
}

std::vector<std::uint8_t> serializeParamSfo(const ParamSfo& sfo) {
    struct Entry {
        std::uint16_t format;
        std::uint32_t length;
        std::uint32_t capacity;
        const std::string* text;  // null for integers
        std::uint32_t value;
    };
    std::map<std::string, Entry> entries;
    for (const auto& [key, value] : sfo.ints) {
        entries[key] = Entry{kFormatU32, 4, 4, nullptr, value};
    }
    for (const auto& [key, text] : sfo.strings) {
        entries[key] =
                Entry{kFormatUtf8, static_cast<std::uint32_t>(text.size() + 1), textCapacity(key, text), &text, 0};
    }

    std::uint32_t key_bytes = 0;
    for (const auto& [key, entry] : entries) key_bytes += static_cast<std::uint32_t>(key.size() + 1);
    const std::uint32_t key_table = static_cast<std::uint32_t>(kHeaderSize + kIndexSize * entries.size());
    const std::uint32_t data_table = key_table + alignUp4(key_bytes);

    std::vector<std::uint8_t> out;
    putLe32(out, kMagic);
    putLe32(out, 0x101);
    putLe32(out, key_table);
    putLe32(out, data_table);
    putLe32(out, static_cast<std::uint32_t>(entries.size()));

    std::uint32_t key_off = 0, data_off = 0;
    for (const auto& [key, entry] : entries) {
        putLe16(out, static_cast<std::uint16_t>(key_off));
        putLe16(out, entry.format);
        putLe32(out, entry.length);
        putLe32(out, entry.capacity);
        putLe32(out, data_off);
        key_off += static_cast<std::uint32_t>(key.size() + 1);
        data_off += entry.capacity;
    }
    for (const auto& [key, entry] : entries) {
        out.insert(out.end(), key.begin(), key.end());
        out.push_back(0);
    }
    out.resize(data_table, 0);
    for (const auto& [key, entry] : entries) {
        const std::size_t start = out.size();
        if (entry.text == nullptr) {
            putLe32(out, entry.value);
        } else {
            out.insert(out.end(), entry.text->begin(), entry.text->end());
        }
        out.resize(start + entry.capacity, 0);
    }
    return out;
}

namespace {

ParamSfo& gameSfoStorage() {
    static ParamSfo sfo = [] {
        ParamSfo defaults;
        defaults.ints["RESOLUTION"] = 0x1;
        defaults.ints["SOUND_FORMAT"] = 0x1;
        return defaults;
    }();
    return sfo;
}

}  // namespace

const ParamSfo& gameParamSfo() { return gameSfoStorage(); }

void setGameParamSfo(ParamSfo sfo) { gameSfoStorage() = std::move(sfo); }

}  // namespace dal
