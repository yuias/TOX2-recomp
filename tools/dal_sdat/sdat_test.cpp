// Self-checking tests for the SDAT library. The keys below are made-up patterns, not real ones;
// containers are built with the library's own primitives, so they check the layout handling and
// the integrity checks rather than the real key set.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <string>

#include "sdat.h"

namespace {

using dal::sdat::Block16;
using dal::sdat::Bytes;

int g_passed = 0;
int g_failed = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            g_passed++;                                                            \
        } else {                                                                   \
            g_failed++;                                                            \
            std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                          \
    } while (0)

Block16 patterned(std::uint8_t seed) {
    Block16 b;
    for (std::size_t i = 0; i < b.size(); i++) {
        b[i] = static_cast<std::uint8_t>(seed + i * 7);
    }
    return b;
}

dal::sdat::Keys fakeKeys() {
    dal::sdat::Keys keys;
    keys.sdat_key = patterned(0x11);
    keys.edat_key_0 = patterned(0x22);
    keys.edat_key_1 = patterned(0x33);
    keys.edat_iv = patterned(0x44);
    return keys;
}

void put32(Bytes& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; i++) b[at + i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
}

void put64(Bytes& b, std::size_t at, std::uint64_t v) {
    put32(b, at, static_cast<std::uint32_t>(v >> 32));
    put32(b, at + 4, static_cast<std::uint32_t>(v));
}

// Builds an SDAT image for `plain` with blocks of `block_size`; `compressed` stores every block in
// the stored form of the block compression.
Bytes buildSdat(const dal::sdat::Keys& keys, const Bytes& plain, std::uint32_t block_size, bool compressed) {
    const std::uint32_t blocks = static_cast<std::uint32_t>((plain.size() + block_size - 1) / block_size);
    const std::size_t entry = compressed ? 0x20 : 0x10;
    Bytes file(0x100 + blocks * entry, 0);
    std::memcpy(file.data(), "NPD\0", 4);
    put32(file, 4, 3);
    const Block16 digest = patterned(0x55);
    const Block16 dev_hash = patterned(0x66);
    std::memcpy(file.data() + 0x40, digest.data(), 16);
    std::memcpy(file.data() + 0x60, dev_hash.data(), 16);
    put32(file, 0x80, 0x0100000C | (compressed ? 1 : 0));
    put32(file, 0x84, block_size);
    put64(file, 0x88, plain.size());

    Block16 data_key;
    for (std::size_t i = 0; i < 16; i++) data_key[i] = dev_hash[i] ^ keys.sdat_key[i];

    for (std::uint32_t block = 0; block < blocks; block++) {
        const std::size_t begin = std::size_t{block} * block_size;
        const std::size_t length = std::min<std::size_t>(block_size, plain.size() - begin);
        Bytes content(plain.begin() + begin, plain.begin() + begin + length);
        if (compressed) {
            Bytes stored(5, 0);
            stored[0] = 0xFF;
            put32(stored, 1, static_cast<std::uint32_t>(length));
            stored.insert(stored.end(), content.begin(), content.end());
            content = stored;
        }
        const std::size_t stored_length = content.size();
        content.resize((content.size() + 15) & ~std::size_t{15}, 0);

        Block16 block_id{};
        std::memcpy(block_id.data(), dev_hash.data(), 12);
        for (int i = 0; i < 4; i++) block_id[12 + i] = static_cast<std::uint8_t>(block >> (24 - 8 * i));
        Block16 block_key;
        Block16 content_key;
        CHECK(dal::sdat::aesEcbEncrypt(data_key, block_id, block_key));
        CHECK(dal::sdat::aesCbcDecrypt(keys.edat_key_0, keys.edat_iv, block_key, content_key.data()));
        Bytes cipher(content.size());
        CHECK(dal::sdat::aesCbcEncrypt(content_key, digest, content, cipher.data()));
        Block16 hash;
        CHECK(dal::sdat::aesCmac(content_key, cipher, hash));

        const std::size_t offset = compressed ? file.size() : 0x100 + std::size_t{block} * block_size + blocks * 0x10;
        if (file.size() < offset + cipher.size()) file.resize(offset + cipher.size(), 0);
        std::memcpy(file.data() + offset, cipher.data(), cipher.size());
        std::uint8_t* meta = file.data() + 0x100 + block * entry;
        std::memcpy(meta, hash.data(), 16);
        if (compressed) {
            std::uint8_t mixed[16] = {};
            for (int i = 0; i < 8; i++) mixed[i] = static_cast<std::uint8_t>(std::uint64_t{offset} >> (56 - 8 * i));
            for (int i = 0; i < 4; i++) {
                mixed[8 + i] = static_cast<std::uint8_t>(stored_length >> (24 - 8 * i));
                mixed[12 + i] = static_cast<std::uint8_t>(block_size >> (24 - 8 * i));  // compression_end != 0
            }
            constexpr int kLeft[4] = {0xC, 0x4, 0xC, 0x4};
            constexpr int kRight[4] = {0x8, 0x8, 0x0, 0x0};
            for (int g = 0; g < 4; g++)
                for (int j = 0; j < 4; j++)
                    meta[0x10 + g * 4 + j] = mixed[g * 4 + j] ^ meta[kLeft[g] + j] ^ meta[kRight[g] + j];
        }
    }
    return file;
}

void testCmac() {
    // RFC 4493 example vectors.
    const Block16 key = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                         0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
    const Bytes msg = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93,
                       0x17, 0x2a, 0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac,
                       0x45, 0xaf, 0x8e, 0x51, 0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11};
    const Block16 want16 = {0x07, 0x0a, 0x16, 0xb4, 0x6b, 0x4d, 0x41, 0x44,
                            0xf7, 0x9b, 0xdd, 0x9d, 0xd0, 0x4a, 0x28, 0x7c};
    const Block16 want40 = {0xdf, 0xa6, 0x67, 0x47, 0xde, 0x9a, 0xe6, 0x30,
                            0x30, 0xca, 0x32, 0x61, 0x14, 0x97, 0xc8, 0x27};
    Block16 got;
    CHECK(dal::sdat::aesCmac(key, std::span(msg).first(16), got));
    CHECK(got == want16);
    CHECK(dal::sdat::aesCmac(key, std::span(msg).first(40), got));
    CHECK(got == want40);
}

void testKeyParsing() {
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() /
            std::format("dal_sdat_test_keys_{}",
                        std::chrono::steady_clock::now().time_since_epoch().count());  // unique under parallel runs
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "key_vault.h");
        f << "constexpr u8 NPDRM_SDAT_KEY[0x10] = { 0xFF };\n"
             "constexpr u8 SDAT_KEY[0x10] = {\n 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,\n"
             " 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10\n};\n"
             "constexpr u8 EDAT_KEY_0[0x10] = { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01 };\n"
             "constexpr u8 EDAT_KEY_1[0x10] = { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02 };\n"
             "constexpr u8 EDAT_IV[0x10] = { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03 };\n";
    }
    dal::sdat::Keys keys;
    std::string error;
    CHECK(dal::sdat::loadKeys(dir, keys, error));
    CHECK(keys.sdat_key[0] == 0x01 && keys.sdat_key[15] == 0x10);
    CHECK(keys.edat_key_1[15] == 0x02 && keys.edat_iv[15] == 0x03);
    CHECK(dal::sdat::loadKeys(dir / "key_vault.cpp", keys, error));  // falls back to the sibling .h
    CHECK(!dal::sdat::loadKeys(dir / "nowhere" / "key_vault.h", keys, error));
    {
        std::ofstream f(dir / "key_vault.h", std::ios::trunc);
        f << "constexpr u8 SDAT_KEY[0x10] = { 0x01, 0x02 };\n";
    }
    CHECK(!dal::sdat::loadKeys(dir, keys, error));
    std::filesystem::remove_all(dir);
}

void testContainers() {
    const dal::sdat::Keys keys = fakeKeys();
    Bytes plain(100);
    for (std::size_t i = 0; i < plain.size(); i++) plain[i] = static_cast<std::uint8_t>(i * 3 + 1);

    for (const bool compressed : {false, true}) {
        const Bytes image = buildSdat(keys, plain, 32, compressed);
        Bytes out;
        dal::sdat::SdatInfo info;
        std::string error;
        CHECK(dal::sdat::decryptSdat(image, keys, out, info, error));
        CHECK(out == plain);
        CHECK(info.blocks == 4);
        CHECK(info.compressed_blocks == (compressed ? 4u : 0u));

        // A damaged ciphertext byte fails the block hash.
        Bytes damaged = image;
        damaged[damaged.size() - 20] ^= 0x40;
        CHECK(!dal::sdat::decryptSdat(damaged, keys, out, info, error));
        // A different key fails as well.
        dal::sdat::Keys other = keys;
        other.sdat_key[0] ^= 1;
        CHECK(!dal::sdat::decryptSdat(image, other, out, info, error));
    }

    // Header rejections.
    Bytes image = buildSdat(keys, plain, 32, false);
    Bytes bad = image;
    bad[0] = 'X';
    Bytes out;
    dal::sdat::SdatInfo info;
    std::string error;
    CHECK(!dal::sdat::decryptSdat(bad, keys, out, info, error));
    bad = image;
    put32(bad, 0x80, 0x0100001C);  // flag 0x10 is not supported
    CHECK(!dal::sdat::decryptSdat(bad, keys, out, info, error));
    bad = image;
    put32(bad, 0x80, 0x0000000C);  // not an SDAT
    CHECK(!dal::sdat::decryptSdat(bad, keys, out, info, error));
    bad = image;
    put32(bad, 0x84, 0);
    CHECK(!dal::sdat::decryptSdat(bad, keys, out, info, error));
    bad.resize(0x100 + 10);
    CHECK(!dal::sdat::decryptSdat(bad, keys, out, info, error));  // metadata/data cut off
}

void testLz() {
    Bytes out(8);
    const Bytes stored = {0xFF, 0, 0, 0, 3, 'a', 'b', 'c'};
    CHECK(dal::sdat::lzDecompress(stored, out) == 3 && out[0] == 'a' && out[2] == 'c');
    const Bytes too_long = {0xFF, 0, 0, 0, 9, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK(dal::sdat::lzDecompress(too_long, out) == -1);
    const Bytes truncated = {0xFF, 0, 0, 0, 4, 'a'};
    CHECK(dal::sdat::lzDecompress(truncated, out) == -1);
    CHECK(dal::sdat::lzDecompress(Bytes{0x01, 0x02}, out) == -1);
    CHECK(dal::sdat::lzDecompress(Bytes{}, out) == -1);
    CHECK(dal::sdat::lzDecompress(Bytes{0xFF, 0, 0, 0}, out) == -1);

    // Stored blocks: empty, exactly at capacity, and padded input (extra bytes are ignored).
    const Bytes stored_empty = {0xFF, 0, 0, 0, 0};
    CHECK(dal::sdat::lzDecompress(stored_empty, out) == 0);
    const Bytes stored_full = {0x81, 0, 0, 0, 8, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0};
    CHECK(dal::sdat::lzDecompress(stored_full, out) == 8 && out[0] == 1 && out[7] == 8);
    const Bytes stored_over = {0xFF, 0, 0, 0, 9, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK(dal::sdat::lzDecompress(stored_over, std::span<std::uint8_t>(out).first(8)) == -1);
    const Bytes stored_huge = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 1, 2, 3};
    CHECK(dal::sdat::lzDecompress(stored_huge, out) == -1);

    // Mode 0x80 is still a compressed block. With a zero code value every decoded bit is 1, so the
    // first match carries the end-of-stream length and nothing is produced.
    const Bytes zeros(64, 0);
    CHECK(dal::sdat::lzDecompress(zeros, out) == 0);
    Bytes zeros_top = zeros;
    zeros_top[0] = 0x80;
    CHECK(dal::sdat::lzDecompress(zeros_top, out) == 0);
    // A short all-zero stream reads missing bytes as zero and ends the same way.
    CHECK(dal::sdat::lzDecompress(Bytes(5, 0), out) == 0);

    // A maximal code value makes the first flag 0 (literal); with capacity 0 that ends the block.
    const Bytes literal_first = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0};
    CHECK(dal::sdat::lzDecompress(literal_first, std::span<std::uint8_t>()) == 0);
    // Same stream with room for output decodes a literal byte and then keeps going; it must stay in bounds.
    const std::int64_t n = dal::sdat::lzDecompress(literal_first, out);
    CHECK(n >= -1 && n <= 8);
}

}  // namespace

int main() {
    testCmac();
    testKeyParsing();
    testContainers();
    testLz();
    std::printf("sdat tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
