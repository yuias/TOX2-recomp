#include "crypto.h"

#include <algorithm>
#include <cstring>

#include <zlib.h>

#include "crypto/host_crypto.h"

namespace tox2::decrypt {
namespace {

// Counter blocks encrypted per ECB call in aes128Ctr.
constexpr std::size_t kCtrBatchBlocks = 4096;
// zlib sizes are 32-bit; larger buffers are fed in pieces.
constexpr std::size_t kMaxZlibChunk = 1u << 30;

}  // namespace

std::uint16_t readBe16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }

std::uint32_t readBe32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}

std::uint64_t readBe64(const std::uint8_t* p) { return (std::uint64_t{readBe32(p)} << 32) | readBe32(p + 4); }

bool aes128EcbEncrypt(const Block16& key, std::span<const std::uint8_t> in, std::uint8_t* out) {
    return crypto::aesEcbEncrypt(key, in, out);
}

bool aes256CbcDecrypt(const Key256& key, const Block16& iv, std::span<const std::uint8_t> in, std::uint8_t* out) {
    return crypto::aesCbcDecrypt(key, iv, in, out);
}

bool aes256CbcEncrypt(const Key256& key, const Block16& iv, std::span<const std::uint8_t> in, std::uint8_t* out) {
    return crypto::aesCbcEncrypt(key, iv, in, out);
}

bool aes128Ctr(const Block16& key, const Block16& iv, std::span<const std::uint8_t> in, std::uint8_t* out) {
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
        if (!crypto::aesEcbEncrypt(key, std::span(keystream.data(), blocks * 16), keystream.data())) {
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
    return crypto::hmacSha1(key, data, out);
}

bool sha256(std::span<const std::uint8_t> data, Sha256Digest& out) { return crypto::sha256(data, out); }

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
