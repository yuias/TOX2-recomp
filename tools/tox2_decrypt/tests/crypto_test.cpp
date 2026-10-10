// Known-answer tests for the crypto primitives. All vectors are public (FIPS 197, SP 800-38A,
// RFC 2202, FIPS 180), none of them is a product key.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include <zlib.h>

#include "crypto.h"
#include "test_util.h"

namespace tox2::decrypt {
namespace {

Bytes fromHex(std::string_view text) {
    Bytes bytes;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        bytes.push_back(static_cast<std::uint8_t>(std::stoi(std::string(text.substr(i, 2)), nullptr, 16)));
    }
    return bytes;
}

template <typename Array>
Array arrayFromHex(std::string_view text) {
    const Bytes bytes = fromHex(text);
    Array array{};
    std::memcpy(array.data(), bytes.data(), std::min(bytes.size(), array.size()));
    return array;
}

Bytes asBytes(std::string_view text) { return Bytes(text.begin(), text.end()); }

Bytes patternBytes(std::size_t size) {
    Bytes bytes(size);
    std::uint32_t state = 0x12345678;
    for (std::uint8_t& byte : bytes) {
        state = state * 1664525 + 1013904223;
        byte = static_cast<std::uint8_t>(state >> 24);
    }
    return bytes;
}

// Compresses with the given window bits: 15 zlib, -15 raw deflate, 31 gzip.
Bytes deflateWith(std::span<const std::uint8_t> data, int window_bits) {
    z_stream stream{};
    deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY);
    Bytes out(deflateBound(&stream, static_cast<uLong>(data.size())) + 64);
    stream.next_in = const_cast<Bytef*>(data.data());
    stream.avail_in = static_cast<uInt>(data.size());
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    deflate(&stream, Z_FINISH);
    out.resize(stream.total_out);
    deflateEnd(&stream);
    return out;
}

}  // namespace

TOX2_TEST(aesEcbFips197) {
    const Block16 key = arrayFromHex<Block16>("000102030405060708090a0b0c0d0e0f");
    const Bytes plain = fromHex("00112233445566778899aabbccddeeff");
    Bytes out(16);
    CHECK(aes128EcbEncrypt(key, plain, out.data()));
    CHECK(toHex(out) == "69c4e0d86a7b0430d8cdb78070b4c55a");

    Bytes in_place = plain;
    CHECK(aes128EcbEncrypt(key, in_place, in_place.data()));
    CHECK(in_place == out);

    // Not a whole number of blocks.
    CHECK(!aes128EcbEncrypt(key, std::span<const std::uint8_t>(plain).first(15), out.data()));
}

TOX2_TEST(aes256CbcFips197) {
    const Key256 key = arrayFromHex<Key256>("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    const Block16 iv{};
    const Bytes plain = fromHex("00112233445566778899aabbccddeeff");
    Bytes cipher(16);
    CHECK(aes256CbcEncrypt(key, iv, plain, cipher.data()));
    CHECK(toHex(cipher) == "8ea2b7ca516745bfeafc49904b496089");
    Bytes back(16);
    CHECK(aes256CbcDecrypt(key, iv, cipher, back.data()));
    CHECK(back == plain);

    // Chaining across several blocks: each plaintext block is XORed with the previous ciphertext block.
    const Bytes two = patternBytes(48);
    Bytes two_cipher(48);
    Bytes two_back(48);
    CHECK(aes256CbcEncrypt(key, iv, two, two_cipher.data()));
    CHECK(aes256CbcDecrypt(key, iv, two_cipher, two_back.data()));
    CHECK(two_back == two);
    Bytes second_alone(16);
    CHECK(aes256CbcDecrypt(key, arrayFromHex<Block16>(toHex(std::span(two_cipher).subspan(0, 16))),
                           std::span(two_cipher).subspan(16, 16), second_alone.data()));
    CHECK(std::equal(second_alone.begin(), second_alone.end(), two.begin() + 16));
}

TOX2_TEST(aesCtrSp80038a) {
    const Block16 key = arrayFromHex<Block16>("2b7e151628aed2a6abf7158809cf4f3c");
    const Block16 counter = arrayFromHex<Block16>("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
    const Bytes plain = fromHex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
    const std::string expected = "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff";

    Bytes out(plain.size());
    CHECK(aes128Ctr(key, counter, plain, out.data()));
    CHECK(toHex(out) == expected);

    // A partial last block uses the leading keystream bytes.
    Bytes partial(20);
    CHECK(aes128Ctr(key, counter, std::span(plain).first(20), partial.data()));
    CHECK(toHex(partial) == expected.substr(0, 40));

    Bytes in_place = plain;
    CHECK(aes128Ctr(key, counter, in_place, in_place.data()));
    CHECK(toHex(in_place) == expected);

    Bytes empty_out;
    CHECK(aes128Ctr(key, counter, {}, empty_out.data()));
}

TOX2_TEST(aesCtrCarry) {
    const Block16 key = arrayFromHex<Block16>("000102030405060708090a0b0c0d0e0f");
    Block16 iv;
    iv.fill(0xff);

    // Block 0 uses all-FF, block 1 wraps to all-zero.
    const Bytes zeros(32);
    Bytes stream(32);
    CHECK(aes128Ctr(key, iv, zeros, stream.data()));
    Bytes counters(32, 0);
    std::memset(counters.data(), 0xff, 16);
    Bytes expected(32);
    CHECK(aes128EcbEncrypt(key, counters, expected.data()));
    CHECK(stream == expected);

    // 70000 blocks cross the 4096-block batches. Counter j is the big-endian value j-1 for j >= 1.
    constexpr std::size_t kBlocks = 70000;
    const Bytes plain = patternBytes(kBlocks * 16);
    Bytes cipher(plain.size());
    CHECK(aes128Ctr(key, iv, plain, cipher.data()));

    Bytes all_counters(plain.size(), 0);
    std::memset(all_counters.data(), 0xff, 16);
    for (std::size_t j = 1; j < kBlocks; j++) {
        const std::uint64_t value = j - 1;
        for (int b = 0; b < 8; b++) {
            all_counters[j * 16 + 15 - b] = static_cast<std::uint8_t>(value >> (8 * b));
        }
    }
    Bytes keystream(plain.size());
    CHECK(aes128EcbEncrypt(key, all_counters, keystream.data()));
    bool same = true;
    for (std::size_t i = 0; i < plain.size(); i++) {
        same = same && cipher[i] == static_cast<std::uint8_t>(plain[i] ^ keystream[i]);
    }
    CHECK(same);

    // A carry that runs through several bytes: ...00ff + 1 = ...0100.
    const Block16 low_ff = arrayFromHex<Block16>("000000000000000000000000000000ff");
    Bytes two_blocks(32);
    CHECK(aes128Ctr(key, low_ff, Bytes(32), two_blocks.data()));
    Bytes second_counter(16, 0);
    second_counter[14] = 1;
    Bytes second_stream(16);
    CHECK(aes128EcbEncrypt(key, second_counter, second_stream.data()));
    CHECK(std::equal(second_stream.begin(), second_stream.end(), two_blocks.begin() + 16));
}

TOX2_TEST(hmacSha1Rfc2202) {
    Sha1Digest digest{};
    const Bytes key1(20, 0x0b);
    CHECK(hmacSha1(key1, asBytes("Hi There"), digest));
    CHECK(toHex(digest) == "b617318655057264e28bc0b6fb378c8ef146be00");

    CHECK(hmacSha1(asBytes("Jefe"), asBytes("what do ya want for nothing?"), digest));
    CHECK(toHex(digest) == "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");

    // A key longer than the block size (80 bytes of 0xaa), RFC 2202 case 6.
    const Bytes key6(80, 0xaa);
    CHECK(hmacSha1(key6, asBytes("Test Using Larger Than Block-Size Key - Hash Key First"), digest));
    CHECK(toHex(digest) == "aa4ae5e15272d00e95705637ce8a3b55ed402112");
}

TOX2_TEST(sha256Abc) {
    Sha256Digest digest{};
    CHECK(sha256(asBytes("abc"), digest));
    CHECK(toHex(digest) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    CHECK(sha256({}, digest));
    CHECK(toHex(digest) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TOX2_TEST(zlibInflate) {
    Bytes plain = patternBytes(5000);
    // Compressible tail so that the stream is shorter than the output.
    plain.insert(plain.end(), 60000, 0x41);

    Bytes packed(compressBound(static_cast<uLong>(plain.size())));
    uLongf packed_size = static_cast<uLongf>(packed.size());
    CHECK(compress2(packed.data(), &packed_size, plain.data(), static_cast<uLong>(plain.size()), Z_BEST_COMPRESSION) ==
          Z_OK);
    packed.resize(packed_size);

    // Trailing junk after the stream end is ignored.
    Bytes with_junk = packed;
    with_junk.insert(with_junk.end(), {0xde, 0xad, 0xbe, 0xef});
    Bytes out;
    std::string error;
    CHECK(zlibInflate(with_junk, plain.size(), out, error));
    CHECK(out == plain);

    // max_out exactly equal to the output size succeeds, also for a stream that is empty after inflation.
    out.clear();
    CHECK(zlibInflate(packed, plain.size(), out, error));
    CHECK(out == plain);
    CHECK(zlibInflate(packed, plain.size() + 12345, out, error));
    CHECK(out == plain);

    // One byte less fails and leaves `out` alone.
    out = {1, 2, 3};
    error.clear();
    CHECK(!zlibInflate(packed, plain.size() - 1, out, error));
    CHECK(!error.empty());
    CHECK(out == (Bytes{1, 2, 3}));

    // Truncated stream.
    error.clear();
    CHECK(!zlibInflate(std::span(packed).first(packed.size() / 2), plain.size(), out, error));
    CHECK(!error.empty());
    CHECK(!zlibInflate(std::span(packed).first(packed.size() - 1), plain.size(), out, error));
    CHECK(!zlibInflate({}, plain.size(), out, error));

    // Raw deflate and gzip are not RFC 1950.
    error.clear();
    CHECK(!zlibInflate(deflateWith(plain, -15), plain.size(), out, error));
    CHECK(!error.empty());
    CHECK(!zlibInflate(deflateWith(plain, 31), plain.size(), out, error));
    CHECK(zlibInflate(deflateWith(plain, 15), plain.size(), out, error));

    // A zero-length payload with max_out 0.
    const Bytes empty_packed = deflateWith({}, 15);
    CHECK(zlibInflate(empty_packed, 0, out, error));
    CHECK(out.empty());
}

TOX2_TEST(hexAndBigEndian) {
    CHECK(toHex(fromHex("00ff10ab")) == "00ff10ab");
    CHECK(toHex({}) == "");
    const Bytes data = fromHex("0102030405060708");
    CHECK(readBe16(data.data()) == 0x0102);
    CHECK(readBe32(data.data()) == 0x01020304);
    CHECK(readBe64(data.data()) == 0x0102030405060708ull);
}

}  // namespace tox2::decrypt
