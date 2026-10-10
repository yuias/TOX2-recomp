#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "crypto.h"

namespace tox2::decrypt {

enum class KeyType : std::uint32_t { Lv0 = 1, Lv1, Lv2, App, Iso, Ldr, Unk7, Npdrm };

struct SelfKey {
    KeyType type;
    std::uint64_t version_start;
    std::uint64_t version_end;
    std::uint16_t revision;
    Key256 erk;
    Block16 riv;
};

struct KeyVault {
    std::array<std::uint8_t, 64> pup_key{};
    Key256 scepkg_erk{};
    Block16 scepkg_riv{};
    std::vector<SelfKey> self_keys;  // declaration order of the emplace_back calls
};

// header_text = key_vault.h, source_text = key_vault.cpp. Error messages name constants and counts, never
// key bytes.
bool parseKeyVault(std::string_view header_text, std::string_view source_text, KeyVault& keys, std::string& error);

// Reads dir/key_vault.h and dir/key_vault.cpp, then parseKeyVault.
bool loadKeyVault(const std::filesystem::path& dir, KeyVault& keys, std::string& error);

// Picks the first entry of the program type's table that matches; nullptr when none does.
// `revision` is the whole 16-bit SCE flags field.
const SelfKey* findSelfKey(const KeyVault& keys, std::uint32_t program_type, std::uint16_t revision,
                           std::uint64_t version);

}  // namespace tox2::decrypt
