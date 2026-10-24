#include "crypto/host_crypto.h"

#include <algorithm>
#include <cwchar>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

namespace tox2::crypto {
namespace {

// BCrypt sizes are ULONG; larger buffers are processed in pieces. A multiple of 16 so that block
// ciphers split on block boundaries.
constexpr std::size_t kMaxCngChunk = 0x7FFFFFF0;

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

const AlgHandle& cmacAlg() {
    static const AlgHandle alg(L"AES-CMAC", nullptr);
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
        if (alg.handle == nullptr || (key.size() != 16 && key.size() != 32) ||
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

// One hash over `data`, fed in pieces. `key` is the HMAC or CMAC key and must be empty for a plain hash.
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

bool aesEcbEncrypt(std::span<const std::uint8_t> key, std::span<const std::uint8_t> in, std::uint8_t* out) {
    const AesKey aes(ecbAlg(), key);
    return aes.crypt(true, nullptr, in.data(), out, in.size());
}

bool aesCbcEncrypt(std::span<const std::uint8_t> key, const Block16& iv, std::span<const std::uint8_t> in,
                   std::uint8_t* out) {
    const AesKey aes(cbcAlg(), key);
    return aes.crypt(true, &iv, in.data(), out, in.size());
}

bool aesCbcDecrypt(std::span<const std::uint8_t> key, const Block16& iv, std::span<const std::uint8_t> in,
                   std::uint8_t* out) {
    const AesKey aes(cbcAlg(), key);
    return aes.crypt(false, &iv, in.data(), out, in.size());
}

bool aesCmac(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data, Block16& out) {
    if (key.size() != 16) {
        return false;
    }
    return hashData(cmacAlg(), key, data, out.data(), static_cast<ULONG>(out.size()));
}

bool hmacSha1(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data, Sha1Digest& out) {
    return hashData(hmacSha1Alg(), key, data, out.data(), static_cast<ULONG>(out.size()));
}

bool sha256(std::span<const std::uint8_t> data, Sha256Digest& out) {
    return hashData(sha256Alg(), {}, data, out.data(), static_cast<ULONG>(out.size()));
}

}  // namespace tox2::crypto
