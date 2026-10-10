#pragma once

// Builds synthetic ustar archives and PUPs. Every key and payload is made up.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "crypto.h"
#include "keys.h"
#include "sce_builder.h"

namespace tox2::decrypt::test {

struct TarBuildEntry {
    std::string name;
    std::string prefix;
    Bytes data;
    char type_flag = '0';
    char size_terminator = '\0';  // after the 11 octal digits
    std::string size_field;       // non-empty: written as is (up to 12 bytes) instead of the real size
    std::string magic = "ustar ";
    Bytes raw;  // non-empty: appended as is instead of a header and data
};

inline void putOctal(Bytes& block, std::size_t offset, std::uint64_t value, char terminator) {
    for (int i = 10; i >= 0; i--) {
        block[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>('0' + (value & 7));
        value >>= 3;
    }
    block[offset + 11] = static_cast<std::uint8_t>(terminator);
}

inline void putText(Bytes& block, std::size_t offset, const std::string& text) {
    std::memcpy(block.data() + offset, text.data(), text.size());
}

// The archive ends with two zero blocks, as tar writes it.
inline Bytes buildTar(const std::vector<TarBuildEntry>& entries) {
    Bytes tar;
    for (const TarBuildEntry& entry : entries) {
        if (!entry.raw.empty()) {
            tar.insert(tar.end(), entry.raw.begin(), entry.raw.end());
            continue;
        }
        Bytes block(512, 0);
        putText(block, 0, entry.name);
        putText(block, 0x159, entry.prefix);
        if (entry.size_field.empty()) {
            putOctal(block, 0x7C, entry.data.size(), entry.size_terminator);
        } else {
            putText(block, 0x7C, entry.size_field);
        }
        block[0x9C] = static_cast<std::uint8_t>(entry.type_flag);
        putText(block, 0x101, entry.magic);
        tar.insert(tar.end(), block.begin(), block.end());
        tar.insert(tar.end(), entry.data.begin(), entry.data.end());
        tar.resize((tar.size() + 511) / 512 * 512, 0);
    }
    tar.resize(tar.size() + 1024, 0);
    return tar;
}

struct PupBuildEntry {
    std::uint64_t id;
    Bytes data;
};

// Header, file table, hash table (HMAC-SHA1 with the vault's PUP_KEY), 0x20 bytes of trailer, then the
// entries back to back.
inline Bytes buildPup(const KeyVault& keys, const std::vector<PupBuildEntry>& entries) {
    const std::uint64_t count = entries.size();
    const std::uint64_t header_length = 0x30 + count * 0x40 + 0x20;
    std::uint64_t data_length = 0;
    for (const PupBuildEntry& entry : entries) {
        data_length += entry.data.size();
    }

    Bytes pup = {'S', 'C', 'E', 'U', 'F', 0, 0, 0};
    detail::putBe64(pup, 1);
    detail::putBe64(pup, 0x10B94);
    detail::putBe64(pup, count);
    detail::putBe64(pup, header_length);
    detail::putBe64(pup, data_length);

    std::uint64_t offset = header_length;
    for (const PupBuildEntry& entry : entries) {
        detail::putBe64(pup, entry.id);
        detail::putBe64(pup, offset);
        detail::putBe64(pup, entry.data.size());
        detail::putBe64(pup, 0);
        offset += entry.data.size();
    }
    for (std::size_t i = 0; i < entries.size(); i++) {
        Sha1Digest digest;
        hmacSha1(keys.pup_key, entries[i].data, digest);
        detail::putBe64(pup, i);
        pup.insert(pup.end(), digest.begin(), digest.end());
        detail::putBe32(pup, 0);
    }
    pup.resize(header_length, 0);
    for (const PupBuildEntry& entry : entries) {
        pup.insert(pup.end(), entry.data.begin(), entry.data.end());
    }
    return pup;
}

}  // namespace tox2::decrypt::test
