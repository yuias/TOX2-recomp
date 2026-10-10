#pragma once

// Synthetic key_vault.h / key_vault.cpp text in the layout the parser accepts. Every key is made up.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "keys.h"

namespace tox2::decrypt::test {

struct FakeHeaderOptions {
    bool crlf = false;
    bool omit_riv = false;
    std::size_t erk_bytes = 32;  // number of bytes written for SCEPKG_ERK
};

struct FakeSourceOptions {
    bool crlf = false;
    std::size_t short_erk_entry = static_cast<std::size_t>(-1);  // entry whose erk gets 63 digits
};

inline std::string fakeToCrlf(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '\n') {
            out += '\r';
        }
        out += c;
    }
    return out;
}

// Deterministic made-up bytes; `seed` makes different keys differ.
template <typename Array>
inline Array fakeBytes(std::uint32_t seed) {
    Array bytes{};
    std::uint32_t state = seed * 2654435761u + 12345u;
    for (std::uint8_t& byte : bytes) {
        state = state * 1664525u + 1013904223u;
        byte = static_cast<std::uint8_t>(state >> 24);
    }
    return bytes;
}

inline SelfKey fakeSelfKey(KeyType type, std::uint64_t version_start, std::uint64_t version_end, std::uint16_t revision,
                           std::uint32_t seed) {
    return SelfKey{type,     version_start,           version_end,
                   revision, fakeBytes<Key256>(seed), fakeBytes<Block16>(seed + 1000)};
}

inline KeyVault fakeKeyVault(std::vector<SelfKey> self_keys) {
    KeyVault keys;
    keys.pup_key = fakeBytes<std::array<std::uint8_t, 64>>(7001);
    keys.scepkg_erk = fakeBytes<Key256>(7002);
    keys.scepkg_riv = fakeBytes<Block16>(7003);
    keys.self_keys = std::move(self_keys);
    return keys;
}

namespace detail {

inline std::string hexByte(std::uint8_t value) {
    static const char kDigits[] = "0123456789abcdef";
    return {'0', 'x', kDigits[value >> 4], kDigits[value & 15]};
}

// Writes `bytes` as `0x..` items, `per_line` per line, with a made-up comment inside the braces that
// has a 0x token in it: a parser that does not strip comments would count it as a byte.
inline std::string byteArrayBody(const std::uint8_t* bytes, std::size_t size, std::size_t per_line, bool comment) {
    std::string out;
    for (std::size_t i = 0; i < size; i++) {
        out += (i % per_line == 0) ? "\n    " : " ";
        out += hexByte(bytes[i]);
        out += i + 1 < size ? "," : "";
        if (comment && i == 2) {
            out += "  // 0xEE is not part of the key\n   ";
        }
    }
    return out;
}

inline std::string hexString(const std::uint8_t* bytes, std::size_t size, bool upper) {
    static const char kLower[] = "0123456789abcdef";
    static const char kUpper[] = "0123456789ABCDEF";
    const char* digits = upper ? kUpper : kLower;
    std::string out;
    for (std::size_t i = 0; i < size; i++) {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 15];
    }
    return out;
}

inline std::string hex64(std::uint64_t value) {
    std::uint8_t bytes[8];
    for (int i = 0; i < 8; i++) {
        bytes[i] = static_cast<std::uint8_t>(value >> (56 - 8 * i));
    }
    return "0x" + hexString(bytes, 8, true);
}

inline std::string hex16(std::uint16_t value) {
    const std::uint8_t bytes[2] = {static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
    return "0x" + hexString(bytes, 2, false);
}

inline const char* typeName(KeyType type) {
    switch (type) {
        case KeyType::Lv0: return "LV0";
        case KeyType::Lv1: return "LV1";
        case KeyType::Lv2: return "LV2";
        case KeyType::App: return "APP";
        case KeyType::Iso: return "ISO";
        case KeyType::Ldr: return "LDR";
        case KeyType::Unk7: return "UNK7";
        case KeyType::Npdrm: return "NPDRM";
    }
    return "LV0";
}

}  // namespace detail

// `PUP_KEY` and `SCEPKG_ERK` span several lines; an unrelated array carries a trailing comment with
// bare hex, and a longer identifier (`PUP_KEY_OLD`) must not be mistaken for `PUP_KEY`.
inline std::string fakeKeyVaultHeader(const KeyVault& keys, const FakeHeaderOptions& options = {}) {
    std::string text;
    text += "#pragma once\n\n";
    text += "// Made-up key vault. PUP_KEY[0x40] = { 0x01 } would be wrong here.\n";
    text += "constexpr u8 NP_KLIC_FREE[0x10] = { 0xAA, 0xBB, 0xCC, 0xDD };  // deadbeef 0x99 cafebabe\n";
    text += "constexpr u8 PUP_KEY_OLD[0x04] = { 0x11, 0x22, 0x33, 0x44 };\n\n";
    text += "constexpr u8 PUP_KEY[0x40] = {" +
            detail::byteArrayBody(keys.pup_key.data(), keys.pup_key.size(), 16, true) + "\n};\n\n";
    text += "/* SCEPKG_RIV[0x10] = { 0x00 } is only a comment\n   0xEE */\n";
    text += "constexpr u8 SCEPKG_ERK[0x20] = {" +
            detail::byteArrayBody(keys.scepkg_erk.data(), options.erk_bytes, 8, false) + "\n};\n\n";
    if (!options.omit_riv) {
        text += "constexpr u8 SCEPKG_RIV[0x10] = {" +
                detail::byteArrayBody(keys.scepkg_riv.data(), keys.scepkg_riv.size(), 16, false) + " };\n";
    }
    text += "const char* name = \"PUP_KEY[1] = { 0x00 }\";\n";
    return options.crlf ? fakeToCrlf(text) : text;
}

// One `sk_<TYPE>_arr.emplace_back(...)` call per key, in vector order. Even entries have the arguments
// on separate lines with upper-case hex; odd entries are compact with lower-case hex. Each call has
// the public key, an empty or a 42-digit private key and a curve argument after riv.
inline std::string fakeKeyVaultSource(const std::vector<SelfKey>& self_keys, const FakeSourceOptions& options = {}) {
    std::string text;
    text += "// Made-up SELF key tables.\n";
    text += "void KeyVault::LoadSelfKeys()\n{\n";
    text += "\t// sk_APP_arr.emplace_back(0x0, 0x0, 0x0001, KEY_APP, \"00\", \"00\");\n";
    for (std::size_t i = 0; i < self_keys.size(); i++) {
        const SelfKey& key = self_keys[i];
        const bool upper = (i % 2) == 0;
        std::string erk = detail::hexString(key.erk.data(), key.erk.size(), upper);
        if (i == options.short_erk_entry) {
            erk.pop_back();
        }
        const std::string riv = detail::hexString(key.riv.data(), key.riv.size(), upper);
        const std::string pub = detail::hexString(
                fakeBytes<std::array<std::uint8_t, 40>>(static_cast<std::uint32_t>(i)).data(), 40, !upper);
        const std::string priv =
                (i % 3 == 0) ? ""
                             : detail::hexString(
                                       fakeBytes<std::array<std::uint8_t, 21>>(static_cast<std::uint32_t>(i)).data(),
                                       21, upper);
        const std::string array = std::string("sk_") + detail::typeName(key.type) + "_arr";
        const std::string type = std::string("KEY_") + detail::typeName(key.type);
        if (upper) {
            text += "\t" + array + ".emplace_back(\n";
            text += "\t\t" + detail::hex64(key.version_start) + ",\n";
            text += "\t\t" + detail::hex64(key.version_end) + ",\n";
            text += "\t\t" + detail::hex16(key.revision) + ",\n";
            text += "\t\t" + type + ",\n";
            text += "\t\t\"" + erk + "\",\n";
            text += "\t\t\"" + riv + "\",\n";
            text += "\t\t\"" + pub + "\",\n";
            text += "\t\t\"" + priv + "\",\n";
            text += "\t\t0x02\n\t);\n";
        } else {
            text += "\t" + array + ".emplace_back(" + detail::hex64(key.version_start) + ", " +
                    detail::hex64(key.version_end) + ", " + detail::hex16(key.revision) + ", " + type + ", \"" + erk +
                    "\", \"" + riv + "\", \"" + pub + "\", \"" + priv + "\", 0x02);\n";
        }
    }
    text += "}\n";
    return options.crlf ? fakeToCrlf(text) : text;
}

}  // namespace tox2::decrypt::test
