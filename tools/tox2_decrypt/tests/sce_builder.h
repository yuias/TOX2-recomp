#pragma once

// Builds synthetic SCE containers by running the decryption procedure backwards with the library's own
// primitives. Every key and payload is made up.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <zlib.h>

#include "crypto.h"
#include "key_vault_text.h"
#include "keys.h"

namespace tox2::decrypt::test {

constexpr std::uint32_t kNoIndex = 0xFFFFFFFF;

struct SceBuildSection {
    std::uint32_t type = 2;
    std::uint32_t program_index = 0;
    Bytes plain;
    bool compress = false;           // zlib (RFC 1950) before encryption
    std::size_t cut_compressed = 0;  // bytes removed from the end of the compressed stream
    bool encrypt = true;             // AES-128-CTR with key_slots[key_index] and IV key_slots[iv_index]
    std::uint32_t key_index = 0;
    std::uint32_t iv_index = 1;
};

struct SceBuildOptions {
    std::uint16_t flags = 0;  // key revision; 0x8000 stores the metadata info unencrypted
    std::uint16_t type = 3;
    Bytes prefix;  // bytes between the 0x20-byte header and the metadata info; sets the metadata offset
    Key256 erk{};
    Block16 riv{};
    std::vector<SceBuildSection> sections;
    std::uint32_t key_count = 20;
    std::uint32_t optional_size = 0x30;
    // Values written into the metadata header instead of the real counts, to test bounds checks.
    std::uint32_t declared_section_count = kNoIndex;
    std::uint32_t declared_key_count = kNoIndex;
};

struct BuiltSce {
    Bytes file;
    std::vector<std::uint64_t> data_offsets;  // per section, in the file
    std::vector<std::uint64_t> data_sizes;    // stored sizes
    std::vector<Block16> key_slots;
};

namespace detail {

inline void putBe16(Bytes& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

inline void putBe32(Bytes& out, std::uint32_t value) {
    putBe16(out, static_cast<std::uint16_t>(value >> 16));
    putBe16(out, static_cast<std::uint16_t>(value));
}

inline void putBe64(Bytes& out, std::uint64_t value) {
    putBe32(out, static_cast<std::uint32_t>(value >> 32));
    putBe32(out, static_cast<std::uint32_t>(value));
}

inline Bytes zlibCompress(const Bytes& data) {
    uLongf size = compressBound(static_cast<uLong>(data.size()));
    Bytes out(size);
    compress2(out.data(), &size, data.data(), static_cast<uLong>(data.size()), Z_BEST_COMPRESSION);
    out.resize(size);
    return out;
}

}  // namespace detail

inline BuiltSce buildSce(const SceBuildOptions& options) {
    BuiltSce built;
    for (std::uint32_t i = 0; i < options.key_count; i++) {
        built.key_slots.push_back(fakeBytes<Block16>(5000 + i));
    }

    // Stored section bytes. A section whose slot index is out of range cannot be encrypted; its
    // plain bytes are stored and the reader must not use them.
    std::vector<Bytes> stored;
    for (const SceBuildSection& section : options.sections) {
        Bytes data = section.compress ? detail::zlibCompress(section.plain) : section.plain;
        data.resize(data.size() - std::min(data.size(), section.cut_compressed));
        if (section.encrypt && section.key_index < options.key_count && section.iv_index < options.key_count) {
            aes128Ctr(built.key_slots[section.key_index], built.key_slots[section.iv_index], data, data.data());
        }
        stored.push_back(std::move(data));
    }

    const std::uint32_t section_count = static_cast<std::uint32_t>(options.sections.size());
    const std::size_t region_size = 0x20 + section_count * 0x30u + options.key_count * 16u + options.optional_size;
    const std::uint64_t header_length = 0x20 + options.prefix.size() + 0x40 + region_size;

    // Data offsets are known now; build the metadata region.
    Bytes region;
    detail::putBe64(region, 0x500);
    detail::putBe32(region, 1);
    detail::putBe32(region,
                    options.declared_section_count != kNoIndex ? options.declared_section_count : section_count);
    detail::putBe32(region, options.declared_key_count != kNoIndex ? options.declared_key_count : options.key_count);
    detail::putBe32(region, options.optional_size);
    detail::putBe32(region, 0);
    detail::putBe32(region, 0);
    std::uint64_t data_offset = header_length;
    std::uint64_t data_total = 0;
    for (std::size_t i = 0; i < options.sections.size(); i++) {
        const SceBuildSection& section = options.sections[i];
        built.data_offsets.push_back(data_offset);
        built.data_sizes.push_back(stored[i].size());
        detail::putBe64(region, data_offset);
        detail::putBe64(region, stored[i].size());
        detail::putBe32(region, section.type);
        detail::putBe32(region, section.program_index);
        detail::putBe32(region, 2);  // hashed
        detail::putBe32(region, 0);  // SHA-1 slot
        detail::putBe32(region, section.encrypt ? 3 : 1);
        detail::putBe32(region, section.encrypt ? section.key_index : kNoIndex);
        detail::putBe32(region, section.encrypt ? section.iv_index : kNoIndex);
        detail::putBe32(region, section.compress ? 2 : 1);
        data_offset += stored[i].size();
        data_total += stored[i].size();
    }
    for (const Block16& slot : built.key_slots) {
        region.insert(region.end(), slot.begin(), slot.end());
    }
    region.resize(region.size() + options.optional_size, 0);

    const Block16 meta_key = fakeBytes<Block16>(6001);
    const Block16 meta_iv = fakeBytes<Block16>(6002);
    Bytes info(0x40, 0);
    std::memcpy(info.data(), meta_key.data(), 16);
    std::memcpy(info.data() + 0x20, meta_iv.data(), 16);
    if ((options.flags & 0x8000) == 0) {
        aes256CbcEncrypt(options.erk, options.riv, info, info.data());
    }
    aes128Ctr(meta_key, meta_iv, region, region.data());

    Bytes& file = built.file;
    file = {'S', 'C', 'E', 0};
    detail::putBe32(file, 2);
    detail::putBe16(file, options.flags);
    detail::putBe16(file, options.type);
    detail::putBe32(file, static_cast<std::uint32_t>(options.prefix.size()));
    detail::putBe64(file, header_length);
    detail::putBe64(file, data_total);
    file.insert(file.end(), options.prefix.begin(), options.prefix.end());
    file.insert(file.end(), info.begin(), info.end());
    file.insert(file.end(), region.begin(), region.end());
    for (const Bytes& data : stored) {
        file.insert(file.end(), data.begin(), data.end());
    }
    return built;
}

// The layout of a firmware dev_flash_* package: two plain 0x40-byte info sections, then the inner TAR
// as an encrypted zlib section (key slot 18, IV slot 19 of 20).
inline SceBuildOptions scePackageOptions(const KeyVault& keys, const Bytes& info0, const Bytes& info1,
                                         const Bytes& payload) {
    SceBuildOptions options;
    options.erk = keys.scepkg_erk;
    options.riv = keys.scepkg_riv;
    SceBuildSection first;
    first.type = 1;
    first.plain = info0;
    first.encrypt = false;
    SceBuildSection second;
    second.type = 2;
    second.plain = info1;
    second.encrypt = false;
    SceBuildSection tar;
    tar.type = 3;
    tar.plain = payload;
    tar.compress = true;
    tar.key_index = 18;
    tar.iv_index = 19;
    options.sections = {first, second, tar};
    return options;
}

inline BuiltSce buildScePackage(const KeyVault& keys, const Bytes& info0, const Bytes& info1, const Bytes& payload) {
    return buildSce(scePackageOptions(keys, info0, info1, payload));
}

}  // namespace tox2::decrypt::test
