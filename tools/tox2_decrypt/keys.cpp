#include "keys.h"

#include <cstring>
#include <fstream>
#include <iterator>

namespace tox2::decrypt {
namespace {

bool isIdentChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::size_t skipSpace(std::string_view text, std::size_t pos) {
    while (pos < text.size() && isSpace(text[pos])) {
        pos++;
    }
    return pos;
}

// Removes // and /* */ comments outside double-quoted strings. A block comment becomes one space so
// that it still separates tokens; line breaks are kept.
std::string stripComments(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '"') {
            out.push_back(c);
            i++;
            while (i < text.size()) {
                const char s = text[i++];
                out.push_back(s);
                if (s == '\\' && i < text.size()) {
                    out.push_back(text[i++]);
                } else if (s == '"') {
                    break;
                }
            }
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') {
                i++;
            }
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const std::size_t end = text.find("*/", i + 2);
            i = end == std::string_view::npos ? text.size() : end + 2;
            out.push_back(' ');
        } else {
            out.push_back(c);
            i++;
        }
    }
    return out;
}

// Finds `name[...] = { 0x.., ... }` and collects the two-digit 0x tokens between the braces. Tokens
// of any other shape are ignored, so a wrong count shows up as a size error.
bool findByteArray(std::string_view text, std::string_view name, std::size_t expected, std::uint8_t* out,
                   std::string& error) {
    std::size_t pos = 0;
    while ((pos = text.find(name, pos)) != std::string_view::npos) {
        const std::size_t begin = pos;
        const std::size_t end = pos + name.size();
        pos = end;
        const bool whole =
                (begin == 0 || !isIdentChar(text[begin - 1])) && (end >= text.size() || !isIdentChar(text[end]));
        const std::size_t bracket = skipSpace(text, end);
        if (!whole || bracket >= text.size() || text[bracket] != '[') {
            continue;
        }
        const std::size_t open = text.find('{', bracket);
        const std::size_t semicolon = text.find(';', bracket);
        // A use such as `NAME[i];` is not the initializer.
        if (open == std::string_view::npos || semicolon < open) {
            continue;
        }
        const std::size_t close = text.find('}', open);
        if (close == std::string_view::npos) {
            error = std::string(name) + ": initializer is not closed";
            return false;
        }
        std::size_t count = 0;
        std::size_t i = open + 1;
        while (i < close) {
            if (!isIdentChar(text[i])) {
                i++;
                continue;
            }
            std::size_t run_end = i;
            while (run_end < close && isIdentChar(text[run_end])) {
                run_end++;
            }
            const std::string_view token = text.substr(i, run_end - i);
            if (token.size() == 4 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X') &&
                hexValue(token[2]) >= 0 && hexValue(token[3]) >= 0) {
                if (count < expected) {
                    out[count] = static_cast<std::uint8_t>(hexValue(token[2]) * 16 + hexValue(token[3]));
                }
                count++;
            }
            i = run_end;
        }
        if (count != expected) {
            error = std::string(name) + ": found " + std::to_string(count) + " bytes, expected " +
                    std::to_string(expected);
            return false;
        }
        return true;
    }
    error = std::string(name) + ": not found in key_vault.h";
    return false;
}

struct Token {
    enum class Kind { Word, String, End };
    Kind kind = Kind::End;
    std::string_view text;  // string contents without the quotes
};

// Skips whitespace, commas and punctuation that cannot start an argument.
Token nextToken(std::string_view args, std::size_t& pos) {
    while (pos < args.size() && !isIdentChar(args[pos]) && args[pos] != '"') {
        pos++;
    }
    if (pos >= args.size()) {
        return {};
    }
    if (args[pos] == '"') {
        const std::size_t begin = pos + 1;
        const std::size_t end = args.find('"', begin);
        if (end == std::string_view::npos) {
            pos = args.size();
            return {};
        }
        pos = end + 1;
        return {Token::Kind::String, args.substr(begin, end - begin)};
    }
    const std::size_t begin = pos;
    while (pos < args.size() && isIdentChar(args[pos])) {
        pos++;
    }
    return {Token::Kind::Word, args.substr(begin, pos - begin)};
}

bool parseHexNumber(std::string_view word, std::size_t max_digits, std::uint64_t& value) {
    if (word.size() < 3 || word[0] != '0' || (word[1] != 'x' && word[1] != 'X') || word.size() - 2 > max_digits) {
        return false;
    }
    value = 0;
    for (const char c : word.substr(2)) {
        const int digit = hexValue(c);
        if (digit < 0) {
            return false;
        }
        value = (value << 4) | static_cast<std::uint64_t>(digit);
    }
    return true;
}

// `digits` must hold exactly 2 * size hex digits.
bool parseHexString(std::string_view digits, std::size_t size, std::uint8_t* out) {
    if (digits.size() != size * 2) {
        return false;
    }
    for (std::size_t i = 0; i < size; i++) {
        const int hi = hexValue(digits[i * 2]);
        const int lo = hexValue(digits[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    return true;
}

bool parseKeyType(std::string_view word, KeyType& type) {
    static constexpr struct {
        std::string_view name;
        KeyType type;
    } kTypes[] = {{"KEY_LV0", KeyType::Lv0},   {"KEY_LV1", KeyType::Lv1},    {"KEY_LV2", KeyType::Lv2},
                  {"KEY_APP", KeyType::App},   {"KEY_ISO", KeyType::Iso},    {"KEY_LDR", KeyType::Ldr},
                  {"KEY_UNK7", KeyType::Unk7}, {"KEY_NPDRM", KeyType::Npdrm}};
    for (const auto& entry : kTypes) {
        if (entry.name == word) {
            type = entry.type;
            return true;
        }
    }
    return false;
}

// Arguments in order: version_start, version_end, revision, KEY_<TYPE>, erk, riv. Later arguments are
// not read. Returns an empty string on success, else the reason.
const char* parseSelfKeyArgs(std::string_view args, SelfKey& key) {
    std::size_t pos = 0;
    std::uint64_t value = 0;

    Token token = nextToken(args, pos);
    if (token.kind != Token::Kind::Word || !parseHexNumber(token.text, 16, key.version_start)) {
        return "version_start is not a 0x number of up to 16 digits";
    }
    token = nextToken(args, pos);
    if (token.kind != Token::Kind::Word || !parseHexNumber(token.text, 16, key.version_end)) {
        return "version_end is not a 0x number of up to 16 digits";
    }
    token = nextToken(args, pos);
    if (token.kind != Token::Kind::Word || !parseHexNumber(token.text, 4, value)) {
        return "revision is not a 0x number of up to 4 digits";
    }
    key.revision = static_cast<std::uint16_t>(value);
    token = nextToken(args, pos);
    if (token.kind != Token::Kind::Word || !parseKeyType(token.text, key.type)) {
        return "key type is not KEY_LV0 .. KEY_NPDRM";
    }
    token = nextToken(args, pos);
    if (token.kind != Token::Kind::String || !parseHexString(token.text, key.erk.size(), key.erk.data())) {
        return "erk is not a string of exactly 64 hex digits";
    }
    token = nextToken(args, pos);
    if (token.kind != Token::Kind::String || !parseHexString(token.text, key.riv.size(), key.riv.data())) {
        return "riv is not a string of exactly 32 hex digits";
    }
    return "";
}

// Returns the text between the parenthesis at `open` and its match, or npos if there is none.
std::size_t findClosingParen(std::string_view text, std::size_t open) {
    int depth = 0;
    for (std::size_t i = open; i < text.size(); i++) {
        if (text[i] == '"') {
            i = text.find('"', i + 1);
            if (i == std::string_view::npos) {
                return i;
            }
        } else if (text[i] == '(') {
            depth++;
        } else if (text[i] == ')' && --depth == 0) {
            return i;
        }
    }
    return std::string_view::npos;
}

// True when `pos` (the start of "emplace_back") is preceded by `sk_<NAME>_arr.` and followed by `(`;
// `open` receives the position of that parenthesis.
bool isSelfKeyCall(std::string_view text, std::size_t pos, std::size_t& open) {
    constexpr std::string_view kCall = "emplace_back";
    const std::size_t after = pos + kCall.size();
    if (after < text.size() && isIdentChar(text[after])) {
        return false;
    }
    open = skipSpace(text, after);
    if (open >= text.size() || text[open] != '(') {
        return false;
    }
    std::size_t p = pos;
    while (p > 0 && isSpace(text[p - 1])) {
        p--;
    }
    if (p == 0 || text[p - 1] != '.') {
        return false;
    }
    p--;
    while (p > 0 && isSpace(text[p - 1])) {
        p--;
    }
    const std::size_t name_end = p;
    while (p > 0 && isIdentChar(text[p - 1])) {
        p--;
    }
    const std::string_view name = text.substr(p, name_end - p);
    return name.size() > 7 && name.starts_with("sk_") && name.ends_with("_arr");
}

bool parseSelfKeys(std::string_view text, std::vector<SelfKey>& entries, std::string& error) {
    constexpr std::string_view kCall = "emplace_back";
    std::size_t pos = 0;
    while ((pos = text.find(kCall, pos)) != std::string_view::npos) {
        const std::size_t call = pos;
        pos += kCall.size();
        std::size_t open = 0;
        if (!isSelfKeyCall(text, call, open)) {
            continue;
        }
        const std::string prefix = "self key entry #" + std::to_string(entries.size() + 1) + ": ";
        const std::size_t close = findClosingParen(text, open);
        if (close == std::string_view::npos) {
            error = prefix + "call is not closed";
            return false;
        }
        SelfKey key{};
        const char* reason = parseSelfKeyArgs(text.substr(open + 1, close - open - 1), key);
        if (*reason != '\0') {
            error = prefix + reason;
            return false;
        }
        entries.push_back(key);
        pos = close;
    }
    if (entries.empty()) {
        error = "key_vault.cpp: no sk_*_arr.emplace_back entries found";
        return false;
    }
    return true;
}

bool readFile(const std::filesystem::path& path, std::string& text, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot read " + path.string();
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

}  // namespace

bool parseKeyVault(std::string_view header_text, std::string_view source_text, KeyVault& keys, std::string& error) {
    const std::string header = stripComments(header_text);
    const std::string source = stripComments(source_text);

    KeyVault parsed;
    if (!findByteArray(header, "PUP_KEY", parsed.pup_key.size(), parsed.pup_key.data(), error) ||
        !findByteArray(header, "SCEPKG_ERK", parsed.scepkg_erk.size(), parsed.scepkg_erk.data(), error) ||
        !findByteArray(header, "SCEPKG_RIV", parsed.scepkg_riv.size(), parsed.scepkg_riv.data(), error) ||
        !parseSelfKeys(source, parsed.self_keys, error)) {
        return false;
    }
    keys = std::move(parsed);
    return true;
}

bool loadKeyVault(const std::filesystem::path& dir, KeyVault& keys, std::string& error) {
    std::string header;
    std::string source;
    if (!readFile(dir / "key_vault.h", header, error) || !readFile(dir / "key_vault.cpp", source, error)) {
        return false;
    }
    return parseKeyVault(header, source, keys, error);
}

const SelfKey* findSelfKey(const KeyVault& keys, std::uint32_t program_type, std::uint16_t revision,
                           std::uint64_t version) {
    for (const SelfKey& key : keys.self_keys) {
        if (static_cast<std::uint32_t>(key.type) != program_type) {
            continue;
        }
        const bool in_range = key.version_start <= version && version <= key.version_end;
        bool match = false;
        switch (key.type) {
            case KeyType::Lv0:
            case KeyType::Ldr: match = true; break;
            case KeyType::Lv1:
            case KeyType::Lv2:
            case KeyType::Unk7: match = in_range; break;
            case KeyType::App:
            case KeyType::Npdrm: match = key.revision == revision; break;
            case KeyType::Iso: match = in_range && key.revision == revision; break;
        }
        if (match) {
            return &key;
        }
    }
    return nullptr;
}

}  // namespace tox2::decrypt
