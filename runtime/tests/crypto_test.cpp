// Tests of the host crypto library against published vectors: FIPS-197 (AES), NIST SP 800-38A (CBC),
// RFC 4493 (AES-CMAC), RFC 2202 (HMAC-SHA1) and FIPS 180-4 (SHA-256).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "crypto/host_crypto.h"

namespace {

int g_passed;
int g_failed;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            g_passed++;                                                            \
        } else {                                                                   \
            g_failed++;                                                            \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
        }                                                                          \
    } while (0)

using tox2::crypto::Block16;
using tox2::crypto::Sha1Digest;
using tox2::crypto::Sha256Digest;
using Bytes = std::vector<std::uint8_t>;

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    return c - 'a' + 10;
}

Bytes fromHex(std::string_view text) {
    Bytes bytes;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        bytes.push_back(static_cast<std::uint8_t>(hexValue(text[i]) * 16 + hexValue(text[i + 1])));
    }
    return bytes;
}

std::string toHex(const std::uint8_t* data, std::size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    for (std::size_t i = 0; i < size; i++) {
        text += kDigits[data[i] >> 4];
        text += kDigits[data[i] & 15];
    }
    return text;
}

std::string toHex(const Bytes& bytes) { return toHex(bytes.data(), bytes.size()); }

Block16 block(std::string_view hex) {
    const Bytes bytes = fromHex(hex);
    Block16 result{};
    std::copy(bytes.begin(), bytes.end(), result.begin());
    return result;
}

Bytes bytesOf(std::string_view text) { return Bytes(text.begin(), text.end()); }

// Runs `crypt` and returns the hex of its output, or "failed".
template <typename Fn>
std::string run(std::size_t size, Fn&& crypt) {
    Bytes out(size);
    return crypt(out.data()) ? toHex(out) : "failed";
}

// ECB single-block answers of FIPS-197 appendix C.
void testAesEcb() {
    const Bytes plain = fromHex("00112233445566778899aabbccddeeff");
    const Bytes key128 = fromHex("000102030405060708090a0b0c0d0e0f");
    const Bytes key256 = fromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");

    CHECK(run(16, [&](std::uint8_t* out) { return tox2::crypto::aesEcbEncrypt(key128, plain, out); }) ==
          "69c4e0d86a7b0430d8cdb78070b4c55a");
    CHECK(run(16, [&](std::uint8_t* out) { return tox2::crypto::aesEcbEncrypt(key256, plain, out); }) ==
          "8ea2b7ca516745bfeafc49904b496089");

    // Several blocks are independent in ECB, and the output may be the input buffer.
    Bytes three;
    for (int i = 0; i < 3; i++) {
        three.insert(three.end(), plain.begin(), plain.end());
    }
    CHECK(tox2::crypto::aesEcbEncrypt(key128, three, three.data()));
    CHECK(toHex(three) == std::string("69c4e0d86a7b0430d8cdb78070b4c55a") + "69c4e0d86a7b0430d8cdb78070b4c55a" +
                                  "69c4e0d86a7b0430d8cdb78070b4c55a");

    // Rejected inputs.
    Bytes out(32);
    CHECK(!tox2::crypto::aesEcbEncrypt(key128, std::span<const std::uint8_t>(plain).first(15), out.data()));
    CHECK(!tox2::crypto::aesEcbEncrypt(std::span<const std::uint8_t>(key128).first(15), plain, out.data()));
    CHECK(!tox2::crypto::aesEcbEncrypt(std::span<const std::uint8_t>(key256).first(24), plain, out.data()));
    CHECK(!tox2::crypto::aesEcbEncrypt({}, plain, out.data()));
    CHECK(tox2::crypto::aesEcbEncrypt(key128, {}, out.data()));
}

// NIST SP 800-38A, F.2.1/F.2.2 (CBC-AES128) and F.2.5/F.2.6 (CBC-AES256).
void testAesCbc() {
    const Block16 iv = block("000102030405060708090a0b0c0d0e0f");
    const Bytes plain =
            fromHex("6bc1bee22e409f96e93d7e117393172a"
                    "ae2d8a571e03ac9c9eb76fac45af8e51"
                    "30c81c46a35ce411e5fbc1191a0a52ef"
                    "f69f2445df4f9b17ad2b417be66c3710");
    struct Vector {
        const char* key;
        const char* cipher;
    };
    const Vector vectors[] = {
            {"2b7e151628aed2a6abf7158809cf4f3c",
             "7649abac8119b246cee98e9b12e9197d"
             "5086cb9b507219ee95db113a917678b2"
             "73bed6b8e3c1743b7116e69e22229516"
             "3ff1caa1681fac09120eca307586e1a7"},
            {"603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4",
             "f58c4c04d6e5f1ba779eabfb5f7bfbd6"
             "9cfc4e967edb808d679f777bc6702c7d"
             "39f23369a9d9bacfa530e26304231461"
             "b2eb05e2c39be9fcda6c19078c6a9d1b"},
    };
    for (const Vector& vector : vectors) {
        const Bytes key = fromHex(vector.key);
        const Bytes cipher = fromHex(vector.cipher);
        CHECK(run(plain.size(), [&](std::uint8_t* out) { return tox2::crypto::aesCbcEncrypt(key, iv, plain, out); }) ==
              vector.cipher);
        CHECK(run(cipher.size(), [&](std::uint8_t* out) { return tox2::crypto::aesCbcDecrypt(key, iv, cipher, out); }) ==
              toHex(plain));

        // In place, as the tools call it.
        Bytes buffer = plain;
        CHECK(tox2::crypto::aesCbcEncrypt(key, iv, buffer, buffer.data()));
        CHECK(toHex(buffer) == vector.cipher);
        CHECK(tox2::crypto::aesCbcDecrypt(key, iv, buffer, buffer.data()));
        CHECK(buffer == plain);

        // The IV is an input only; a second call with the same IV gives the same result.
        CHECK(run(plain.size(), [&](std::uint8_t* out) { return tox2::crypto::aesCbcEncrypt(key, iv, plain, out); }) ==
              vector.cipher);

        Bytes out(plain.size());
        CHECK(!tox2::crypto::aesCbcEncrypt(key, iv, std::span<const std::uint8_t>(plain).first(17), out.data()));
        CHECK(!tox2::crypto::aesCbcDecrypt(key, iv, std::span<const std::uint8_t>(cipher).first(33), out.data()));
    }
}

// RFC 4493, section 4: the 64-byte message is cut to 0, 16, 40 and 64 bytes.
void testAesCmac() {
    const Bytes key = fromHex("2b7e151628aed2a6abf7158809cf4f3c");
    const Bytes message =
            fromHex("6bc1bee22e409f96e93d7e117393172a"
                    "ae2d8a571e03ac9c9eb76fac45af8e51"
                    "30c81c46a35ce411e5fbc1191a0a52ef"
                    "f69f2445df4f9b17ad2b417be66c3710");
    struct Vector {
        std::size_t size;
        const char* mac;
    };
    const Vector vectors[] = {
            {0, "bb1d6929e95937287fa37d129b756746"},
            {16, "070a16b46b4d4144f79bdd9dd04a287c"},
            {40, "dfa66747de9ae63030ca32611497c827"},
            {64, "51f0bebf7e3b9d92fc49741779363cfe"},
    };
    for (const Vector& vector : vectors) {
        Block16 mac{};
        CHECK(tox2::crypto::aesCmac(key, std::span<const std::uint8_t>(message).first(vector.size), mac));
        CHECK(toHex(mac.data(), mac.size()) == vector.mac);
    }

    // Inputs longer than any internal chunk of a provider, with and without a partial last block.
    // Expected values from OpenSSL (`openssl mac -cipher AES-128-CBC CMAC`).
    const Bytes long_message(200001, 0x5a);
    Block16 mac{};
    CHECK(tox2::crypto::aesCmac(key, std::span<const std::uint8_t>(long_message).first(200000), mac));
    CHECK(toHex(mac.data(), mac.size()) == "c5137ef76fae8ae2a4f107a9644ce6d7");
    CHECK(tox2::crypto::aesCmac(key, long_message, mac));
    CHECK(toHex(mac.data(), mac.size()) == "6a2bf517a29eedc94d28aec5590e05f6");

    CHECK(!tox2::crypto::aesCmac(std::span<const std::uint8_t>(key).first(15), message, mac));
    CHECK(!tox2::crypto::aesCmac({}, message, mac));
}

// RFC 2202, test cases 1 to 3.
void testHmacSha1() {
    struct Vector {
        Bytes key;
        Bytes data;
        const char* mac;
    };
    const Vector vectors[] = {
            {Bytes(20, 0x0b), bytesOf("Hi There"), "b617318655057264e28bc0b6fb378c8ef146be00"},
            {bytesOf("Jefe"), bytesOf("what do ya want for nothing?"), "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"},
            {Bytes(20, 0xaa), Bytes(50, 0xdd), "125d7342b9ac11cd91a39af48aa17b4f63f175d3"},
    };
    for (const Vector& vector : vectors) {
        Sha1Digest mac{};
        CHECK(tox2::crypto::hmacSha1(vector.key, vector.data, mac));
        CHECK(toHex(mac.data(), mac.size()) == vector.mac);
    }
}

void testSha256() {
    Sha256Digest digest{};
    const Bytes abc = bytesOf("abc");
    CHECK(tox2::crypto::sha256(abc, digest));
    CHECK(toHex(digest.data(), digest.size()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    CHECK(tox2::crypto::sha256({}, digest));
    CHECK(toHex(digest.data(), digest.size()) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    const Bytes zeros(std::size_t{1} << 20, 0);
    CHECK(tox2::crypto::sha256(zeros, digest));
    CHECK(toHex(digest.data(), digest.size()) == "30e14955ebf1352266dc2ff8067e68104607e750abb9d3b36582b8af909fcb58");
}

}  // namespace

int main() {
    testAesEcb();
    testAesCbc();
    testAesCmac();
    testHmacSha1();
    testSha256();
    std::printf("crypto tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
