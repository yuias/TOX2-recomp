#pragma once

// AES, AES-CMAC, HMAC-SHA1 and SHA-256 on the host's own crypto library: CNG (BCrypt) on Windows,
// CommonCrypto on macOS. Which provider is built is decided by the build files.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tox2::crypto {

using Block16 = std::array<std::uint8_t, 16>;
using Sha1Digest = std::array<std::uint8_t, 20>;
using Sha256Digest = std::array<std::uint8_t, 32>;

// AES keys are 16 or 32 bytes. ECB/CBC need in.size() % 16 == 0; `out` holds in.size() bytes and may
// equal in.data(). Any size_t length is accepted (providers with 32-bit sizes chunk internally).
// All functions return false on a bad size or a provider failure.
bool aesEcbEncrypt(std::span<const std::uint8_t> key, std::span<const std::uint8_t> in, std::uint8_t* out);
bool aesCbcEncrypt(std::span<const std::uint8_t> key, const Block16& iv, std::span<const std::uint8_t> in,
                   std::uint8_t* out);
bool aesCbcDecrypt(std::span<const std::uint8_t> key, const Block16& iv, std::span<const std::uint8_t> in,
                   std::uint8_t* out);

// AES-128 CMAC (RFC 4493); `key` must be 16 bytes. `data` may be empty.
bool aesCmac(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data, Block16& out);

bool hmacSha1(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data, Sha1Digest& out);
bool sha256(std::span<const std::uint8_t> data, Sha256Digest& out);

}  // namespace tox2::crypto
