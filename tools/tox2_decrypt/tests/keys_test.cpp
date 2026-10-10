// Tests for the key file parser and key selection. Every key is made up (see key_vault_text.h).

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "key_vault_text.h"
#include "keys.h"
#include "test_util.h"

namespace tox2::decrypt {
namespace {

using test::fakeKeyVault;
using test::fakeKeyVaultHeader;
using test::fakeKeyVaultSource;
using test::fakeSelfKey;
using test::FakeHeaderOptions;
using test::FakeSourceOptions;

constexpr std::uint64_t kOpenEnd = ~0ull;

std::vector<SelfKey> sampleEntries() {
    return {
            fakeSelfKey(KeyType::Lv0, 0, kOpenEnd, 0x0000, 1),
            fakeSelfKey(KeyType::Lv2, 0x0001000000000000, 0x0003005500000000, 0x0000, 2),
            fakeSelfKey(KeyType::App, 0x0004004600000000, 0x0004004600000000, 0x001C, 3),
            fakeSelfKey(KeyType::App, 0x0000000000000000, 0x00000000FFFFFFFF, 0x0001, 4),
            fakeSelfKey(KeyType::Iso, 0x0001000000000000, 0x0002000000000000, 0x0005, 5),
            fakeSelfKey(KeyType::Ldr, 0, kOpenEnd, 0x0000, 6),
            fakeSelfKey(KeyType::Unk7, 0x0000000100000000, 0x0000000200000000, 0x0000, 7),
            fakeSelfKey(KeyType::Npdrm, 0, kOpenEnd, 0x00AB, 8),
            fakeSelfKey(KeyType::Lv1, 0, kOpenEnd, 0x0000, 9),
    };
}

bool sameKey(const SelfKey& a, const SelfKey& b) {
    return a.type == b.type && a.version_start == b.version_start && a.version_end == b.version_end &&
           a.revision == b.revision && a.erk == b.erk && a.riv == b.riv;
}

bool sameVault(const KeyVault& a, const KeyVault& b) {
    if (a.pup_key != b.pup_key || a.scepkg_erk != b.scepkg_erk || a.scepkg_riv != b.scepkg_riv ||
        a.self_keys.size() != b.self_keys.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.self_keys.size(); i++) {
        if (!sameKey(a.self_keys[i], b.self_keys[i])) {
            return false;
        }
    }
    return true;
}

// True when the text holds the first 4 bytes of any key of the vault (upper or lower case), which
// would mean an error message leaked key material.
bool leaksKeyBytes(std::string text, const KeyVault& keys) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    auto leaks = [&](const std::uint8_t* bytes) { return text.find(toHex(std::span(bytes, 4))) != std::string::npos; };
    if (leaks(keys.pup_key.data()) || leaks(keys.scepkg_erk.data()) || leaks(keys.scepkg_riv.data())) {
        return true;
    }
    return std::any_of(keys.self_keys.begin(), keys.self_keys.end(),
                       [&](const SelfKey& key) { return leaks(key.erk.data()) || leaks(key.riv.data()); });
}

}  // namespace

TOX2_TEST(parseKeyVaultValid) {
    const KeyVault expected = fakeKeyVault(sampleEntries());
    for (const bool crlf : {false, true}) {
        FakeHeaderOptions header_options;
        header_options.crlf = crlf;
        FakeSourceOptions source_options;
        source_options.crlf = crlf;
        KeyVault keys;
        std::string error;
        CHECK(parseKeyVault(fakeKeyVaultHeader(expected, header_options),
                            fakeKeyVaultSource(expected.self_keys, source_options), keys, error));
        CHECK(error.empty());
        CHECK(sameVault(keys, expected));
    }
}

TOX2_TEST(parseKeyVaultCommentedOut) {
    const KeyVault expected = fakeKeyVault(sampleEntries());

    // Decoys come first, so a parser that read them would return their values.
    std::string header =
            "/* constexpr u8 SCEPKG_ERK[0x20] = { 0x01, 0x02 };\n   constexpr u8 SCEPKG_RIV[0x10] = { 0x03 }; */\n";
    header += "// constexpr u8 PUP_KEY[0x40] = { 0x01, 0x02 };\n";
    header += fakeKeyVaultHeader(expected);

    std::string source = "// sk_APP_arr.emplace_back(0x1, 0x2, 0x7777, KEY_APP, \"" + std::string(64, 'a') + "\", \"" +
                         std::string(32, 'b') + "\");\n";
    source += "/* sk_LV0_arr.emplace_back(0x1, 0x2, 0x0000, KEY_LV0, \"" + std::string(64, 'c') + "\", \"" +
              std::string(32, 'd') +
              "\");\n   sk_LV0_arr.emplace_back(0x1, 0x2, 0x0000, KEY_LV0, \"bad\", \"bad\"); */\n";
    // A "//" inside a string is not a comment.
    source += "const char* url = \"http://example.invalid/\"; sk_LDR_arr.emplace_back(0x0, 0x0, 0x0000, KEY_LDR, \"" +
              std::string(64, '1') + "\", \"" + std::string(32, '2') + "\");\n";
    source += fakeKeyVaultSource(expected.self_keys);

    KeyVault keys;
    std::string error;
    CHECK(parseKeyVault(header, source, keys, error));
    CHECK(keys.pup_key == expected.pup_key);
    CHECK(keys.scepkg_erk == expected.scepkg_erk);
    CHECK(keys.scepkg_riv == expected.scepkg_riv);
    // Only the real-looking Ldr call before the fake source is extra.
    CHECK(keys.self_keys.size() == expected.self_keys.size() + 1);
    if (keys.self_keys.size() == expected.self_keys.size() + 1) {
        CHECK(keys.self_keys[0].type == KeyType::Ldr);
        for (std::size_t i = 0; i < expected.self_keys.size(); i++) {
            CHECK(sameKey(keys.self_keys[i + 1], expected.self_keys[i]));
        }
    }
}

TOX2_TEST(parseKeyVaultErrors) {
    const KeyVault expected = fakeKeyVault(sampleEntries());
    const std::string good_header = fakeKeyVaultHeader(expected);
    const std::string good_source = fakeKeyVaultSource(expected.self_keys);
    KeyVault keys;
    std::string error;

    FakeHeaderOptions no_riv;
    no_riv.omit_riv = true;
    CHECK(!parseKeyVault(fakeKeyVaultHeader(expected, no_riv), good_source, keys, error));
    CHECK(error.find("SCEPKG_RIV") != std::string::npos);
    CHECK(!leaksKeyBytes(error, expected));

    FakeHeaderOptions short_erk;
    short_erk.erk_bytes = 31;
    CHECK(!parseKeyVault(fakeKeyVaultHeader(expected, short_erk), good_source, keys, error));
    CHECK(error.find("SCEPKG_ERK") != std::string::npos);
    CHECK(error.find("31") != std::string::npos);
    CHECK(!leaksKeyBytes(error, expected));

    FakeSourceOptions bad_erk;
    bad_erk.short_erk_entry = 1;
    CHECK(!parseKeyVault(good_header, fakeKeyVaultSource(expected.self_keys, bad_erk), keys, error));
    CHECK(error.find("#2") != std::string::npos);
    CHECK(error.find("erk") != std::string::npos);
    CHECK(!leaksKeyBytes(error, expected));

    CHECK(!parseKeyVault(good_header, "void KeyVault::LoadSelfKeys() {}\n", keys, error));
    CHECK(!error.empty());

    // A failed parse leaves the output untouched.
    KeyVault untouched = expected;
    CHECK(!parseKeyVault(good_header, "", untouched, error));
    CHECK(sameVault(untouched, expected));
}

TOX2_TEST(findSelfKeyRules) {
    KeyVault keys;
    keys.self_keys = {
            fakeSelfKey(KeyType::Lv0, 0, kOpenEnd, 0x0000, 1),                             // 0
            fakeSelfKey(KeyType::Lv0, 0, kOpenEnd, 0x0000, 2),                             // 1
            fakeSelfKey(KeyType::Lv2, 0x10, 0x20, 0x0000, 3),                              // 2
            fakeSelfKey(KeyType::Lv2, 0x15, 0x30, 0x0000, 4),                              // 3 overlaps 2
            fakeSelfKey(KeyType::Lv2, 0x40, kOpenEnd, 0x0000, 5),                          // 4 open-ended
            fakeSelfKey(KeyType::App, 0x0004004600000000, 0x0004004600000000, 0x001C, 6),  // 5
            fakeSelfKey(KeyType::App, 0, 0, 0x001C, 7),                                    // 6 same revision
            fakeSelfKey(KeyType::App, 0, 0, 0x0001, 8),                                    // 7
            fakeSelfKey(KeyType::Iso, 0x100, 0x200, 0x0003, 9),                            // 8
            fakeSelfKey(KeyType::Ldr, 0, kOpenEnd, 0x0000, 10),                            // 9
            fakeSelfKey(KeyType::Ldr, 0, kOpenEnd, 0x0000, 11),                            // 10
            fakeSelfKey(KeyType::Lv1, 0x5, 0x6, 0x0000, 12),                               // 11
            fakeSelfKey(KeyType::Unk7, 0x5, 0x6, 0x0000, 13),                              // 12
            fakeSelfKey(KeyType::Npdrm, 0, kOpenEnd, 0x0009, 14),                          // 13
    };
    auto index = [&](const SelfKey* key) {
        return key == nullptr ? -1 : static_cast<int>(key - keys.self_keys.data());
    };

    // LV0 and LDR: first entry, whatever revision and version.
    CHECK(index(findSelfKey(keys, 1, 0x1234, 0x99)) == 0);
    CHECK(index(findSelfKey(keys, 6, 0, 0)) == 9);

    // LV2: version range, first hit in declaration order, open-ended end.
    CHECK(index(findSelfKey(keys, 3, 0, 0x10)) == 2);
    CHECK(index(findSelfKey(keys, 3, 0, 0x18)) == 2);
    CHECK(index(findSelfKey(keys, 3, 0, 0x25)) == 3);
    CHECK(index(findSelfKey(keys, 3, 0, 0x30)) == 3);
    CHECK(index(findSelfKey(keys, 3, 0, 0x31)) == -1);
    CHECK(index(findSelfKey(keys, 3, 0, 0x40)) == 4);
    CHECK(index(findSelfKey(keys, 3, 0, 0xFFFFFFFFFFFFFFFFull)) == 4);
    CHECK(index(findSelfKey(keys, 3, 0, 0x0F)) == -1);

    // APP: revision only, version ignored, first hit wins.
    CHECK(index(findSelfKey(keys, 4, 0x001C, 0x0004009300000000)) == 5);
    CHECK(index(findSelfKey(keys, 4, 0x001C, 0)) == 5);
    CHECK(index(findSelfKey(keys, 4, 0x0001, 0x77)) == 7);
    CHECK(index(findSelfKey(keys, 4, 0x8001, 0x77)) == -1);
    CHECK(index(findSelfKey(keys, 4, 0x0002, 0x77)) == -1);

    // ISO: version and revision.
    CHECK(index(findSelfKey(keys, 5, 0x0003, 0x150)) == 8);
    CHECK(index(findSelfKey(keys, 5, 0x0004, 0x150)) == -1);
    CHECK(index(findSelfKey(keys, 5, 0x0003, 0x250)) == -1);

    // LV1 and UNK7: version only; NPDRM: revision only.
    CHECK(index(findSelfKey(keys, 2, 0x1234, 0x5)) == 11);
    CHECK(index(findSelfKey(keys, 2, 0x1234, 0x7)) == -1);
    CHECK(index(findSelfKey(keys, 7, 0x1234, 0x6)) == 12);
    CHECK(index(findSelfKey(keys, 8, 0x0009, 0x9999)) == 13);
    CHECK(index(findSelfKey(keys, 8, 0x000A, 0)) == -1);

    // Types outside 1..8.
    CHECK(findSelfKey(keys, 0, 0, 0) == nullptr);
    CHECK(findSelfKey(keys, 9, 0x001C, 0) == nullptr);

    // A type with no entries.
    KeyVault empty;
    CHECK(findSelfKey(empty, 4, 0x001C, 0) == nullptr);
}

TOX2_TEST(loadKeyVaultDir) {
    const KeyVault expected = fakeKeyVault(sampleEntries());
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() /
            ("tox2_decrypt_keys_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);

    auto write = [&](const char* name, const std::string& text) {
        std::ofstream(dir / name, std::ios::binary) << text;
    };

    KeyVault keys;
    std::string error;
    write("key_vault.h", fakeKeyVaultHeader(expected));
    // Header only: key_vault.cpp is missing.
    CHECK(!loadKeyVault(dir, keys, error));
    CHECK(error.find("key_vault.cpp") != std::string::npos);

    write("key_vault.cpp", fakeKeyVaultSource(expected.self_keys));
    CHECK(loadKeyVault(dir, keys, error));
    CHECK(sameVault(keys, expected));

    std::filesystem::remove(dir / "key_vault.h");
    CHECK(!loadKeyVault(dir, keys, error));
    CHECK(error.find("key_vault.h") != std::string::npos);

    CHECK(!loadKeyVault(dir / "missing", keys, error));

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

}  // namespace tox2::decrypt
