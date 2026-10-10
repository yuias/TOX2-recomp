#include "sdat.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

namespace dal::sdat {
namespace {

namespace stdfs = std::filesystem;

constexpr std::uint32_t kSdatFlag = 0x01000000;
constexpr std::uint32_t kCompressedFlag = 0x00000001;
constexpr std::uint32_t kEncryptedKeyFlag = 0x00000008;
// Set in the SDAT files seen so far; unedat.cpp tolerates it without acting on it.
constexpr std::uint32_t kIgnoredFlag = 0x00000004;
constexpr std::uint32_t kSupportedFlags = kSdatFlag | kCompressedFlag | kEncryptedKeyFlag | kIgnoredFlag;
constexpr std::uint64_t kMetadataOffset = 0x100;
constexpr std::uint32_t kMaxBlockSize = 1u << 24;
constexpr std::uint64_t kMaxFileSize = 1ull << 32;

std::uint32_t readBe32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}

std::uint64_t readBe64(const std::uint8_t* p) { return (std::uint64_t{readBe32(p)} << 32) | readBe32(p + 4); }

// ---------------------------------------------------------------------------------------------
// CNG

struct AlgHandle {
    explicit AlgHandle(const wchar_t* id, const wchar_t* chain_mode) {
        if (BCryptOpenAlgorithmProvider(&handle, id, nullptr, 0) != 0) {
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

class AesKey {
public:
    AesKey(const AlgHandle& alg, const Block16& key) {
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

    // `iv` is null for ECB. The chaining state is kept in a copy, so the same IV can be reused.
    bool crypt(bool encrypt, const Block16* iv, const std::uint8_t* in, std::uint8_t* out, std::size_t size) const {
        if (key_ == nullptr || size % 16 != 0 || size > 0x7FFFFFFF) {
            return false;
        }
        Block16 iv_copy{};
        if (iv != nullptr) {
            iv_copy = *iv;
        }
        ULONG done = 0;
        PUCHAR iv_ptr = iv != nullptr ? iv_copy.data() : nullptr;
        const ULONG iv_size = iv != nullptr ? static_cast<ULONG>(iv_copy.size()) : 0;
        const NTSTATUS status = encrypt ? BCryptEncrypt(key_, const_cast<PUCHAR>(in), static_cast<ULONG>(size), nullptr,
                                                        iv_ptr, iv_size, out, static_cast<ULONG>(size), &done, 0)
                                        : BCryptDecrypt(key_, const_cast<PUCHAR>(in), static_cast<ULONG>(size), nullptr,
                                                        iv_ptr, iv_size, out, static_cast<ULONG>(size), &done, 0);
        return status == 0 && done == size;
    }

private:
    BCRYPT_KEY_HANDLE key_ = nullptr;
};

// ---------------------------------------------------------------------------------------------
// Key file parsing

bool readTextFile(const stdfs::path& path, std::string& text) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

bool isIdentChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Finds `NAME[...] = { 0x.., ... }` and collects the bytes. Returns false when the name is absent.
bool findByteArray(std::string_view text, std::string_view name, Bytes& bytes) {
    for (std::size_t at = text.find(name); at != std::string_view::npos; at = text.find(name, at + 1)) {
        if ((at > 0 && isIdentChar(text[at - 1])) || at + name.size() >= text.size() || text[at + name.size()] != '[') {
            continue;
        }
        const std::size_t open = text.find('{', at);
        const std::size_t close = open == std::string_view::npos ? open : text.find('}', open);
        if (close == std::string_view::npos) {
            continue;
        }
        bytes.clear();
        for (std::size_t i = open; i + 3 < close; i++) {
            if (text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X') && hexValue(text[i + 2]) >= 0 &&
                hexValue(text[i + 3]) >= 0) {
                bytes.push_back(static_cast<std::uint8_t>(hexValue(text[i + 2]) * 16 + hexValue(text[i + 3])));
                i += 3;
            }
        }
        return true;
    }
    return false;
}

}  // namespace

bool loadKeys(const stdfs::path& path, Keys& keys, std::string& error) {
    std::vector<stdfs::path> candidates;
    std::error_code ec;
    if (stdfs::is_directory(path, ec)) {
        candidates = {path / "key_vault.h", path / "key_vault.cpp"};
    } else {
        candidates = {path, path.parent_path() / "key_vault.h", path.parent_path() / "key_vault.cpp"};
    }
    std::string text;
    for (const stdfs::path& candidate : candidates) {
        std::string part;
        if (readTextFile(candidate, part)) {
            text += part;
            text += '\n';
        }
    }
    if (text.empty()) {
        error = "cannot read the key source (expected key_vault.h / key_vault.cpp)";
        return false;
    }

    struct Entry {
        std::string_view name;
        Block16* target;
    };
    const Entry entries[] = {{"SDAT_KEY", &keys.sdat_key},
                             {"EDAT_KEY_0", &keys.edat_key_0},
                             {"EDAT_KEY_1", &keys.edat_key_1},
                             {"EDAT_IV", &keys.edat_iv}};
    for (const Entry& entry : entries) {
        Bytes bytes;
        if (!findByteArray(text, entry.name, bytes)) {
            error = std::format("{} not found in the key source", entry.name);
            return false;
        }
        if (bytes.size() != entry.target->size()) {
            error = std::format("{} has {} bytes, expected 16", entry.name, bytes.size());
            return false;
        }
        std::copy(bytes.begin(), bytes.end(), entry.target->begin());
    }
    return true;
}

bool aesEcbEncrypt(const Block16& key, const Block16& in, Block16& out) {
    return AesKey(ecbAlg(), key).crypt(true, nullptr, in.data(), out.data(), in.size());
}

bool aesCbcEncrypt(const Block16& key, const Block16& iv, std::span<const std::uint8_t> in, std::uint8_t* out) {
    return AesKey(cbcAlg(), key).crypt(true, &iv, in.data(), out, in.size());
}

bool aesCbcDecrypt(const Block16& key, const Block16& iv, std::span<const std::uint8_t> in, std::uint8_t* out) {
    return AesKey(cbcAlg(), key).crypt(false, &iv, in.data(), out, in.size());
}

bool aesCmac(const Block16& key, std::span<const std::uint8_t> data, Block16& out) {
    const AlgHandle& alg = cmacAlg();
    if (alg.handle == nullptr) {
        return false;
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg.handle, &hash, nullptr, 0, const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()),
                         0) != 0) {
        return false;
    }
    bool ok = data.size() <= 0x7FFFFFFF &&
              (data.empty() ||
               BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) == 0) &&
              BCryptFinishHash(hash, out.data(), static_cast<ULONG>(out.size()), 0) == 0;
    BCryptDestroyHash(hash);
    return ok;
}

bool decryptSdat(std::span<const std::uint8_t> file, const Keys& keys, Bytes& out, SdatInfo& info, std::string& error) {
    if (file.size() < kMetadataOffset || std::memcmp(file.data(), "NPD\0", 4) != 0) {
        error = "not an NPD file";
        return false;
    }
    const std::uint8_t* digest = file.data() + 0x40;
    const std::uint8_t* dev_hash = file.data() + 0x60;
    info.version = static_cast<std::int32_t>(readBe32(file.data() + 4));
    info.license = static_cast<std::int32_t>(readBe32(file.data() + 8));
    info.flags = readBe32(file.data() + 0x80);
    info.block_size = readBe32(file.data() + 0x84);
    info.file_size = readBe64(file.data() + 0x88);

    if ((info.flags & kSdatFlag) == 0) {
        error = "not an SDAT file (an EDAT needs a per-title key)";
        return false;
    }
    if ((info.flags & ~kSupportedFlags) != 0) {
        error = std::format("unsupported EDAT flags {:#010x}", info.flags);
        return false;
    }
    if (info.version < 2 || info.version > 4) {
        error = std::format("unsupported NPD version {}", info.version);
        return false;
    }
    if (info.block_size == 0 || info.block_size > kMaxBlockSize || info.file_size > kMaxFileSize) {
        error = std::format("implausible block size {:#x} / file size {:#x}", info.block_size, info.file_size);
        return false;
    }

    const bool compressed = (info.flags & kCompressedFlag) != 0;
    const std::uint64_t metadata_entry = compressed ? 0x20 : 0x10;
    info.blocks = static_cast<std::uint32_t>((info.file_size + info.block_size - 1) / info.block_size);
    if (kMetadataOffset + info.blocks * metadata_entry > file.size()) {
        error = "the metadata table runs past the end of the file";
        return false;
    }

    // The data key is derived from the header; version 4 selects the second constant set.
    Block16 data_key;
    for (std::size_t i = 0; i < data_key.size(); i++) {
        data_key[i] = dev_hash[i] ^ keys.sdat_key[i];
    }
    const Block16& wrap_key = info.version == 4 ? keys.edat_key_1 : keys.edat_key_0;
    Block16 digest_iv;
    std::memcpy(digest_iv.data(), digest, digest_iv.size());

    const AesKey block_key_cipher(ecbAlg(), data_key);
    out.clear();
    out.reserve(info.file_size);
    Bytes encrypted;
    Bytes decrypted;
    Bytes expanded(info.block_size);

    for (std::uint32_t block = 0; block < info.blocks; block++) {
        const std::uint8_t* meta = file.data() + kMetadataOffset + block * metadata_entry;
        std::uint64_t offset;
        std::uint64_t length;
        std::int32_t compression_end = 0;
        Block16 expected_hash;
        std::memcpy(expected_hash.data(), meta, expected_hash.size());
        if (compressed) {
            // The 0x20-byte entry hides offset/length/compression_end behind an XOR mix of its words.
            std::uint8_t mixed[16];
            constexpr int kLeft[4] = {0xC, 0x4, 0xC, 0x4};
            constexpr int kRight[4] = {0x8, 0x8, 0x0, 0x0};
            for (int group = 0; group < 4; group++) {
                for (int j = 0; j < 4; j++) {
                    mixed[group * 4 + j] =
                            meta[kLeft[group] + j] ^ meta[kRight[group] + j] ^ meta[0x10 + group * 4 + j];
                }
            }
            offset = readBe64(mixed);
            length = readBe32(mixed + 8);
            compression_end = static_cast<std::int32_t>(readBe32(mixed + 12));
        } else {
            offset = kMetadataOffset + std::uint64_t{block} * info.block_size + info.blocks * metadata_entry;
            length = info.block_size;
            if (block == info.blocks - 1 && info.file_size % info.block_size != 0) {
                length = info.file_size % info.block_size;
            }
        }
        const std::uint64_t padded = (length + 15) & ~std::uint64_t{15};
        if (length == 0 || length > std::uint64_t{info.block_size} + 32 || offset > file.size() ||
            padded > file.size() - offset) {
            error = std::format("block {} has an invalid location (offset {:#x}, length {:#x})", block, offset, length);
            return false;
        }
        const std::span<const std::uint8_t> block_in = file.subspan(offset, padded);

        // Per-block key: the header hash prefix plus the block number, encrypted with the data key.
        Block16 block_id{};
        std::memcpy(block_id.data(), dev_hash, 12);
        block_id[12] = static_cast<std::uint8_t>(block >> 24);
        block_id[13] = static_cast<std::uint8_t>(block >> 16);
        block_id[14] = static_cast<std::uint8_t>(block >> 8);
        block_id[15] = static_cast<std::uint8_t>(block);
        Block16 block_key;
        if (!block_key_cipher.crypt(true, nullptr, block_id.data(), block_key.data(), block_key.size())) {
            error = "AES failure";
            return false;
        }
        // With the encrypted-key flag the block key goes through one more AES-CBC step; the same value
        // is the AES-CMAC key (the hash input equals the key input for the flag combinations accepted).
        Block16 content_key = block_key;
        if ((info.flags & kEncryptedKeyFlag) != 0) {
            if (!aesCbcDecrypt(wrap_key, keys.edat_iv, block_key, content_key.data())) {
                error = "AES failure";
                return false;
            }
        }

        Block16 actual_hash;
        if (!aesCmac(content_key, block_in, actual_hash)) {
            error = "AES-CMAC failure";
            return false;
        }
        if (actual_hash != expected_hash) {
            error = std::format("block {} fails its hash check (wrong key or damaged file)", block);
            return false;
        }
        decrypted.resize(padded);
        if (!aesCbcDecrypt(content_key, digest_iv, block_in, decrypted.data())) {
            error = "AES failure";
            return false;
        }

        const std::uint64_t remaining = info.file_size - out.size();
        if (compressed && compression_end != 0) {
            info.compressed_blocks++;
            const std::int64_t produced = lzDecompress(decrypted, expanded);
            if (produced < 0) {
                error = std::format("block {} failed to decompress", block);
                return false;
            }
            const std::uint64_t take = std::min<std::uint64_t>(static_cast<std::uint64_t>(produced), remaining);
            out.insert(out.end(), expanded.begin(), expanded.begin() + static_cast<std::ptrdiff_t>(take));
        } else {
            const std::uint64_t take = std::min<std::uint64_t>(length, remaining);
            out.insert(out.end(), decrypted.begin(), decrypted.begin() + static_cast<std::ptrdiff_t>(take));
        }
    }
    if (out.size() != info.file_size) {
        error = std::format("decrypted {} bytes but the header says {}", out.size(), info.file_size);
        return false;
    }
    return true;
}

}  // namespace dal::sdat
