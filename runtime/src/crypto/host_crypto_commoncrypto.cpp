#include "crypto/host_crypto.h"

#include <algorithm>
#include <vector>

#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>

namespace tox2::crypto {
namespace {

// CC_LONG is 32 bits; larger inputs are fed to the hash in pieces.
constexpr std::size_t kMaxHashChunk = std::size_t{1} << 30;
// Plaintext handed to one CBC call while chaining the AES-CMAC prefix.
constexpr std::size_t kCmacChunk = 64 * 1024;

bool validAesKeySize(std::size_t size) { return size == 16 || size == 32; }

bool aesCrypt(CCOperation op, CCOptions options, std::span<const std::uint8_t> key, const Block16* iv,
              std::span<const std::uint8_t> in, std::uint8_t* out) {
    if (!validAesKeySize(key.size()) || in.size() % 16 != 0) {
        return false;
    }
    if (in.empty()) {
        return true;
    }
    std::size_t moved = 0;
    const CCCryptorStatus status =
            CCCrypt(op, kCCAlgorithmAES, options, key.data(), key.size(), iv != nullptr ? iv->data() : nullptr,
                    in.data(), in.size(), out, in.size(), &moved);
    return status == kCCSuccess && moved == in.size();
}

// Doubling in GF(2^128) as defined for the CMAC subkeys (RFC 4493, section 2.3).
Block16 cmacDouble(const Block16& value) {
    Block16 result{};
    for (std::size_t i = 0; i < result.size(); i++) {
        result[i] = static_cast<std::uint8_t>(value[i] << 1);
        if (i + 1 < result.size() && (value[i + 1] & 0x80) != 0) {
            result[i] |= 1;
        }
    }
    if ((value[0] & 0x80) != 0) {
        result[15] ^= 0x87;
    }
    return result;
}

}  // namespace

bool aesEcbEncrypt(std::span<const std::uint8_t> key, std::span<const std::uint8_t> in, std::uint8_t* out) {
    return aesCrypt(kCCEncrypt, kCCOptionECBMode, key, nullptr, in, out);
}

bool aesCbcEncrypt(std::span<const std::uint8_t> key, const Block16& iv, std::span<const std::uint8_t> in,
                   std::uint8_t* out) {
    return aesCrypt(kCCEncrypt, 0, key, &iv, in, out);
}

bool aesCbcDecrypt(std::span<const std::uint8_t> key, const Block16& iv, std::span<const std::uint8_t> in,
                   std::uint8_t* out) {
    return aesCrypt(kCCDecrypt, 0, key, &iv, in, out);
}

// CommonCrypto has no CMAC, so it is built on AES as in RFC 4493: CBC-MAC with a zero IV, where the
// last block is XORed with a subkey (and padded with 0x80 00.. when it is not a full block).
bool aesCmac(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data, Block16& out) {
    if (key.size() != 16) {
        return false;
    }
    const Block16 zero{};
    Block16 l{};
    if (!aesEcbEncrypt(key, zero, l.data())) {
        return false;
    }
    const Block16 k1 = cmacDouble(l);
    const Block16 k2 = cmacDouble(k1);

    const bool complete = !data.empty() && data.size() % 16 == 0;
    const std::size_t blocks = std::max<std::size_t>((data.size() + 15) / 16, 1);
    const std::size_t prefix = (blocks - 1) * 16;

    // The CBC chain over every block but the last; its final ciphertext block is the running MAC.
    Block16 mac{};
    std::vector<std::uint8_t> scratch(std::min(prefix, kCmacChunk));
    for (std::size_t at = 0; at < prefix;) {
        const std::size_t chunk = std::min(prefix - at, scratch.size());
        if (!aesCbcEncrypt(key, mac, data.subspan(at, chunk), scratch.data())) {
            return false;
        }
        std::copy(scratch.begin() + static_cast<std::ptrdiff_t>(chunk - 16),
                  scratch.begin() + static_cast<std::ptrdiff_t>(chunk), mac.begin());
        at += chunk;
    }

    Block16 last{};
    const std::size_t tail = data.size() - prefix;
    std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(prefix), tail, last.begin());
    if (!complete) {
        last[tail] = 0x80;
    }
    const Block16& subkey = complete ? k1 : k2;
    for (std::size_t i = 0; i < last.size(); i++) {
        last[i] ^= mac[i] ^ subkey[i];
    }
    return aesEcbEncrypt(key, last, out.data());
}

bool hmacSha1(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data, Sha1Digest& out) {
    CCHmac(kCCHmacAlgSHA1, key.data(), key.size(), data.data(), data.size(), out.data());
    return true;
}

bool sha256(std::span<const std::uint8_t> data, Sha256Digest& out) {
    CC_SHA256_CTX context;
    if (CC_SHA256_Init(&context) != 1) {
        return false;
    }
    for (std::size_t at = 0; at < data.size();) {
        const std::size_t chunk = std::min(data.size() - at, kMaxHashChunk);
        if (CC_SHA256_Update(&context, data.data() + at, static_cast<CC_LONG>(chunk)) != 1) {
            return false;
        }
        at += chunk;
    }
    return CC_SHA256_Final(out.data(), &context) == 1;
}

}  // namespace tox2::crypto
