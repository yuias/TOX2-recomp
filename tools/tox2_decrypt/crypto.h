#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tox2::decrypt {

using Bytes = std::vector<std::uint8_t>;
using Block16 = std::array<std::uint8_t, 16>;  // AES-128 key, IV, counter, key slot
using Key256 = std::array<std::uint8_t, 32>;   // AES-256 key (erk, SCEPKG_ERK)
using Sha1Digest = std::array<std::uint8_t, 20>;
using Sha256Digest = std::array<std::uint8_t, 32>;

std::uint16_t readBe16(const std::uint8_t* p);
std::uint32_t readBe32(const std::uint8_t* p);
std::uint64_t readBe64(const std::uint8_t* p);

// `in.size()` must be a multiple of 16; `out` may equal `in`. All AES functions return false on a
// CNG failure or a bad size.
bool aes128EcbEncrypt(const Block16& key, std::span<const std::uint8_t> in, std::uint8_t* out);
bool aes256CbcDecrypt(const Key256& key, const Block16& iv, std::span<const std::uint8_t> in, std::uint8_t* out);
bool aes256CbcEncrypt(const Key256& key, const Block16& iv, std::span<const std::uint8_t> in, std::uint8_t* out);

// Big-endian 128-bit counter starting at `iv`; any length (the last partial block uses the leading
// keystream bytes). CNG has no CTR mode, so the keystream is produced with ECB. `out` may equal `in`.
bool aes128Ctr(const Block16& key, const Block16& iv, std::span<const std::uint8_t> in, std::uint8_t* out);

bool hmacSha1(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data, Sha1Digest& out);
bool sha256(std::span<const std::uint8_t> data, Sha256Digest& out);
std::string toHex(std::span<const std::uint8_t> bytes);  // lowercase

// Inflates one RFC 1950 (zlib-wrapped) stream. Succeeds only if the stream ends inside `in` and
// produces at most `max_out` bytes; `out` is replaced. Trailing input after the stream end is ignored.
bool zlibInflate(std::span<const std::uint8_t> in, std::size_t max_out, Bytes& out, std::string& error);

}  // namespace tox2::decrypt
