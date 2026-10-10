#include "crypto.h"

#include <algorithm>
#include <cstring>

#include <zlib.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

namespace tox2::decrypt {
namespace {

// BCrypt sizes are ULONG; larger buffers are processed in pieces. A multiple of 16 so that block
// ciphers split on block boundaries.
constexpr std::size_t kMaxCngChunk = 0x7FFFFFF0;
// Counter blocks encrypted per ECB call in aes128Ctr.
constexpr std::size_t kCtrBatchBlocks = 4096;
// zlib sizes are 32-bit; larger buffers are fed in pieces.
constexpr std::size_t kMaxZlibChunk = 1u << 30;

struct AlgHandle {
    AlgHandle(const wchar_t* id, const wchar_t* chain_mode, ULONG flags = 0) {
        if (BCryptOpenAlgorithmProvider(&handle, id, nullptr, flags) != 0) {
            handle = nullptr;
            return;
        }
        if (chain_mode != nullptr &&
            BCryptSetProperty(handle, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(chain_mode)),
                              static_cast<ULONG>((wcslen(chain_mode) + 1) * sizeof(wchar_t)), 0) != 0) {
            BCryptCloseAlgorithmProvider(handle, 0);
            handle = nullptr;
        }
    }
    ~AlgHandle() {
        if (handle != nullptr) {
            BCryptCloseAlgorithmProvider(handle, 0);
        }
    }
    AlgHandle(const AlgHandle&) = delete;
    AlgHandle& operator=(const AlgHandle&) = delete;

    BCRYPT_ALG_HANDLE handle = nullptr;
};

const AlgHandle& ecbAlg() {
    static const AlgHandle alg(BCRYPT_AES_ALGORITHM, BCRYPT_CHAIN_MODE_ECB);
    return alg;
}

const AlgHandle& cbcAlg() {
    static const AlgHandle alg(BCRYPT_AES_ALGORITHM, BCRYPT_CHAIN_MODE_CBC);
    return alg;
}

const AlgHandle& sha256Alg() {
    static const AlgHandle alg(BCRYPT_SHA256_ALGORITHM, nullptr);
    return alg;
}

const AlgHandle& hmacSha1Alg() {
    static const AlgHandle alg(BCRYPT_SHA1_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    return alg;
}

// The key length (16 or 32 bytes) selects AES-128 or AES-256.
class AesKey {
public:
    AesKey(const AlgHandle& alg, std::span<const std::uint8_t> key) {
        if (alg.handle == nullptr ||
            BCryptGenerateSymmetricKey(alg.handle, &key_, nullptr, 0, const_cast<PUCHAR>(key.data()),
                                       static_cast<ULONG>(key.size()), 0) != 0) {
            key_ = nullptr;
        }
    }
    ~AesKey() {
        if (key_ != nullptr) {
            BCryptDestroyKey(key_);
        }
    }
    AesKey(const AesKey&) = delete;
    AesKey& operator=(const AesKey&) = delete;

    // `iv` is null for ECB. CNG updates the IV buffer with the last ciphertext block, so a copy that
    // lives across the chunks chains them correctly.
    bool crypt(bool encrypt, const Block16* iv, const std::uint8_t* in, std::uint8_t* out, std::size_t size) const {
        if (key_ == nullptr || size % 16 != 0) {
            return false;
        }
        Block16 iv_copy{};
        if (iv != nullptr) {
            iv_copy = *iv;
        }
        PUCHAR iv_ptr = iv != nullptr ? iv_copy.data() : nullptr;
        const ULONG iv_size = iv != nullptr ? static_cast<ULONG>(iv_copy.size()) : 0;
        for (std::size_t done_total = 0; done_total < size;) {
            const ULONG chunk = static_cast<ULONG>(std::min(size - done_total, kMaxCngChunk));
            ULONG done = 0;
            const NTSTATUS status = encrypt ? BCryptEncrypt(key_, const_cast<PUCHAR>(in + done_total), chunk, nullptr,
                                                            iv_ptr, iv_size, out + done_total, chunk, &done, 0)
                                            : BCryptDecrypt(key_, const_cast<PUCHAR>(in + done_total), chunk, nullptr,
                                                            iv_ptr, iv_size, out + done_total, chunk, &done, 0);
            if (status != 0 || done != chunk) {
                return false;
            }
            done_total += chunk;
        }
        return true;
    }

private:
    BCRYPT_KEY_HANDLE key_ = nullptr;
};

// One hash over `data`, fed in pieces. `key` is the HMAC key and must be empty for a plain hash.
bool hashData(const AlgHandle& alg, std::span<const std::uint8_t> key, std::span<const std::uint8_t> data,
              std::uint8_t* out, ULONG out_size) {
    if (alg.handle == nullptr) {
        return false;
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg.handle, &hash, nullptr, 0, key.empty() ? nullptr : const_cast<PUCHAR>(key.data()),
                         static_cast<ULONG>(key.size()), 0) != 0) {
        return false;
    }
    bool ok = true;
    for (std::size_t at = 0; ok && at < data.size();) {
        const ULONG chunk = static_cast<ULONG>(std::min(data.size() - at, kMaxCngChunk));
        ok = BCryptHashData(hash, const_cast<PUCHAR>(data.data() + at), chunk, 0) == 0;
        at += chunk;
    }
    ok = ok && BCryptFinishHash(hash, out, out_size, 0) == 0;
    BCryptDestroyHash(hash);
    return ok;
}

}  // namespace

std::uint16_t readBe16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }

std::uint32_t readBe32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}

std::uint64_t readBe64(const std::uint8_t* p) { return (std::uint64_t{readBe32(p)} << 32) | readBe32(p + 4); }

bool aes128EcbEncrypt(const Block16& key, std::span<const std::uint8_t> in, std::uint8_t* out) {
    const AesKey aes(ecbAlg(), key);
    return aes.crypt(true, nullptr, in.data(), out, in.size());
}

bool aes256CbcDecrypt(const Key256& key, const Block16& iv, std::span<const std::uint8_t> in, std::uint8_t* out) {
    const AesKey aes(cbcAlg(), key);
    return aes.crypt(false, &iv, in.data(), out, in.size());
}

bool aes256CbcEncrypt(const Key256& key, const Block16& iv, std::span<const std::uint8_t> in, std::uint8_t* out) {
    const AesKey aes(cbcAlg(), key);
    return aes.crypt(true, &iv, in.data(), out, in.size());
}

bool aes128Ctr(const Block16& key, const Block16& iv, std::span<const std::uint8_t> in, std::uint8_t* out) {
    const AesKey aes(ecbAlg(), key);
    Block16 counter = iv;
    Bytes keystream(kCtrBatchBlocks * 16);
    for (std::size_t at = 0; at < in.size();) {
        const std::size_t bytes = std::min(in.size() - at, keystream.size());
        const std::size_t blocks = (bytes + 15) / 16;
        for (std::size_t i = 0; i < blocks; i++) {
            std::memcpy(keystream.data() + i * 16, counter.data(), 16);
            // Increment as a 128-bit big-endian integer; the carry stops at the first byte that does not wrap.
            for (int j = 15; j >= 0 && ++counter[j] == 0; j--) {
            }
        }
        if (!aes.crypt(true, nullptr, keystream.data(), keystream.data(), blocks * 16)) {
            return false;
        }
        for (std::size_t i = 0; i < bytes; i++) {
            out[at + i] = in[at + i] ^ keystream[i];
        }
        at += bytes;
    }
    return true;
}

bool hmacSha1(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data, Sha1Digest& out) {
    return hashData(hmacSha1Alg(), key, data, out.data(), static_cast<ULONG>(out.size()));
}

bool sha256(std::span<const std::uint8_t> data, Sha256Digest& out) {
    return hashData(sha256Alg(), {}, data, out.data(), static_cast<ULONG>(out.size()));
}

std::string toHex(std::span<const std::uint8_t> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        text += kDigits[byte >> 4];
        text += kDigits[byte & 15];
    }
    return text;
}

bool zlibInflate(std::span<const std::uint8_t> in, std::size_t max_out, Bytes& out, std::string& error) {
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) {
        error = "zlib: inflateInit failed";
        return false;
    }
    Bytes result;
    std::size_t produced = 0;
    std::size_t consumed = 0;
    bool ok = false;
    for (;;) {
        if (stream.avail_in == 0 && consumed < in.size()) {
            const std::size_t chunk = std::min(in.size() - consumed, kMaxZlibChunk);
            stream.next_in = const_cast<Bytef*>(in.data() + consumed);
            stream.avail_in = static_cast<uInt>(chunk);
            consumed += chunk;
        }
        // Once the buffer holds max_out bytes, a one-byte probe tells "exactly full" from "too long".
        std::uint8_t spare = 0;
        bool probe = false;
        if (produced == result.size()) {
            if (result.size() < max_out) {
                result.resize(std::min(max_out, std::max<std::size_t>(result.size() * 2, 65536)));
            } else {
                probe = true;
            }
        }
        uInt avail_out = 0;
        if (probe) {
            stream.next_out = &spare;
            avail_out = 1;
        } else {
            stream.next_out = result.data() + produced;
            avail_out = static_cast<uInt>(std::min(result.size() - produced, kMaxZlibChunk));
        }
        stream.avail_out = avail_out;
        const int status = inflate(&stream, Z_NO_FLUSH);
        const std::size_t written = avail_out - stream.avail_out;
        if (probe && written != 0) {
            error = "zlib: output exceeds the size limit";
            break;
        }
        produced += probe ? 0 : written;
        if (status == Z_STREAM_END) {
            ok = true;
            break;
        }
        if (status == Z_BUF_ERROR) {
            error = "zlib: stream is truncated";
            break;
        }
        if (status != Z_OK) {
            error = std::string("zlib: ") + (stream.msg != nullptr ? stream.msg : "inflate failed");
            break;
        }
    }
    inflateEnd(&stream);
    if (!ok) {
        return false;
    }
    result.resize(produced);
    out = std::move(result);
    return true;
}

}  // namespace tox2::decrypt
