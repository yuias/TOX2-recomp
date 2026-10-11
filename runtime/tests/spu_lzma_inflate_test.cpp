// Runs the recompiled edgeLzmaInflateRawData of the game's LZMA SPURS task (image eboot_edgelzma) on real
// data: a TLZC item of TLFILE.TLDAT is inflated chunk by chunk the way the task's main loop does it, and the
// concatenated output is compared with the SHA-256 of the item as inflated by a reference decoder. The time
// spent in the recompiled code is printed as a throughput figure; it is not checked.
//
// Usage: tox2_spu_lzma_inflate_test <game-dir> <decrypted EBOOT.elf>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/elf_loader.h"
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "spu/spu_thread.h"
#include "tox2/spu_context.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

namespace {

constexpr std::uint64_t kItemOffset = 0x336bb50f;
constexpr std::uint32_t kItemUsize = 0x3073c;
constexpr std::string_view kExpectedSha256 = "f4ad1f7125d9b31abf228e8043587aebc8f1501f0f4debed6d9814d06a12c751";

constexpr std::uint32_t kInflateEntry = 0x3628;
constexpr std::uint32_t kChunkSize = 0x10000;
constexpr std::uint32_t kHeaderSize = 0x18;
constexpr std::uint32_t kPropsSize = 5;

// Local store layout of the task around its call to edgeLzmaInflateRawData: the output buffer is where the
// task's main loop places it, the input buffer follows the task's data segment.
constexpr std::uint32_t kLsOutput = 0x17980;
constexpr std::uint32_t kLsInput = 0x7900;
constexpr std::uint32_t kLsStack = 0x3ff00;
constexpr std::uint32_t kLsPropsOffset = 32;
// The task's crt sets the stack pointer's second word to the stack space left below the bss end.
constexpr std::uint32_t kLsBssEnd = 0x37c80;
// No code or data of the task lives here, so a return through $0 is recognised by the dispatcher alone.
constexpr std::uint32_t kExitLs = 0x3fff0;

struct Owner : tox2::SpuOwner {
    std::atomic<int> stops{0};
    void onInterruptMailbox(tox2::SpuThread&, std::uint32_t) override {}
    bool onStop(tox2::SpuThread&, std::uint32_t, std::uint32_t) override {
        stops++;
        return false;
    }
    void onFinished(tox2::SpuThread&, tox2::SpuLeave) override {}
};

std::string sha256Hex(const std::uint8_t* data, std::size_t size) {
    std::array<std::uint8_t, 32> digest{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data), static_cast<ULONG>(size),
                   digest.data(), static_cast<ULONG>(digest.size())) < 0) {
        return {};
    }
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string hex;
    for (const std::uint8_t b : digest) {
        hex.push_back(kDigits[b >> 4]);
        hex.push_back(kDigits[b & 15]);
    }
    return hex;
}

std::uint32_t le32(const std::uint8_t* p) {
    return p[0] | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24);
}

// The TLZC item: header, 5 properties bytes, one little-endian u16 compressed size per 64 KiB chunk, chunk
// data. A size of 0 marks a stored chunk.
struct Item {
    std::uint32_t usize = 0;
    std::uint8_t props[kPropsSize] = {};
    std::vector<std::uint32_t> chunk_sizes;
    std::vector<std::uint64_t> chunk_offsets;  // file offsets
    std::vector<std::uint8_t> payload;         // everything after the header
};

bool readItem(const std::filesystem::path& tldat, Item* item) {
    std::ifstream in(tldat, std::ios::binary);
    std::uint8_t header[kHeaderSize];
    in.seekg(static_cast<std::streamoff>(kItemOffset));
    if (!in.read(reinterpret_cast<char*>(header), sizeof header)) {
        std::fprintf(stderr, "cannot read the TLZC header from %s\n", tldat.string().c_str());
        return false;
    }
    const std::uint32_t csize = le32(header + 8);
    item->usize = le32(header + 12);
    if (std::memcmp(header, "TLZC", 4) != 0 || header[5] != 4 || csize < kHeaderSize + kPropsSize ||
        item->usize != kItemUsize) {
        std::fprintf(stderr, "not the expected LZMA TLZC item (csize %#x usize %#x)\n", csize, item->usize);
        return false;
    }
    item->payload.resize(csize - kHeaderSize);
    if (!in.read(reinterpret_cast<char*>(item->payload.data()), static_cast<std::streamsize>(item->payload.size()))) {
        std::fprintf(stderr, "cannot read the TLZC payload\n");
        return false;
    }
    std::memcpy(item->props, item->payload.data(), kPropsSize);
    const std::uint32_t chunks = (item->usize + kChunkSize - 1) / kChunkSize;
    std::uint32_t pos = kPropsSize + 2 * chunks;
    for (std::uint32_t i = 0; i < chunks; i++) {
        const std::uint8_t* p = item->payload.data() + kPropsSize + 2 * i;
        const std::uint32_t size = p[0] | (std::uint32_t{p[1]} << 8);
        item->chunk_sizes.push_back(size);
        item->chunk_offsets.push_back(kItemOffset + kHeaderSize + pos);
        pos += size == 0 ? std::min(kChunkSize, item->usize - i * kChunkSize) : size;
    }
    if (pos != item->payload.size()) {
        std::fprintf(stderr, "chunk sizes add up to %#x, the payload has %#zx bytes\n", pos, item->payload.size());
        return false;
    }
    return true;
}

void setWord(Tox2SpuContext& ctx, unsigned reg, std::uint32_t value) {
    ctx.gpr[reg] = {};
    TOX2_VR_W(ctx.gpr[reg], 0) = value;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <game-dir> <EBOOT.elf>\n", argv[0]);
        return 2;
    }
    const std::filesystem::path tldat = std::filesystem::path(argv[1]) / "PS3_GAME" / "USRDIR" / "TLFILE.TLDAT";

    tox2::installProcessSettings();
    Item item;
    if (!readItem(tldat, &item)) return 1;

    // Lines go to stderr as they come: a stop during setup ends the process before they could be flushed.
    tox2::setLogSink(
            [](std::string_view line) { std::fprintf(stderr, "%.*s\n", static_cast<int>(line.size()), line.data()); });
    if (!tox2::process().memory.reserve()) {
        std::fprintf(stderr, "cannot reserve the guest address space\n");
        return 1;
    }
    const auto loaded = tox2::loadExecutable(argv[2]);
    if (!loaded) {
        std::fprintf(stderr, "load failed: %s\n", loaded.error().c_str());
        return 1;
    }
    tox2::installFaultHandler();
    tox2::reserveFaultStack();
    const std::uint8_t* base = tox2::process().memory.base();

    tox2::registerSpuImages(kTox2SpuImages, kTox2SpuImageCount);

    const Tox2SpuImageInfo* image = nullptr;
    for (std::uint32_t i = 0; i < kTox2SpuImageCount; i++) {
        if (std::string_view(kTox2SpuImages[i].name) == "eboot_edgelzma") image = &kTox2SpuImages[i];
    }
    if (image == nullptr) {
        std::fprintf(stderr, "image eboot_edgelzma is not in the generated table\n");
        return 1;
    }
    // Only this image is checked: the libsre images live in a module that the test does not load.
    if (sha256Hex(base + image->ea, image->size) != image->sha256) {
        std::fprintf(stderr, "image eboot_edgelzma does not match the generated code\n");
        return 1;
    }

    Owner owner;
    tox2::SpuThread thread(0x01000100, "lzma_inflate", &owner);
    for (std::uint32_t i = 0; i < image->segment_count; i++) {
        const Tox2SpuSegment& s = image->segments[i];
        tox2::copyToLs(thread, base, s.ea, s.ls, s.size);
    }
    Tox2SpuContext& ctx = thread.context();
    ctx.exit_ls = kExitLs;

    std::chrono::steady_clock::duration spu_time{};
    std::vector<std::uint8_t> output;
    output.reserve(item.usize);
    for (std::size_t chunk = 0; chunk < item.chunk_sizes.size(); chunk++) {
        const std::uint32_t usize = std::min(kChunkSize, item.usize - static_cast<std::uint32_t>(chunk) * kChunkSize);
        const std::uint32_t csize = item.chunk_sizes[chunk];
        const std::uint8_t* src = item.payload.data() + (item.chunk_offsets[chunk] - kItemOffset - kHeaderSize);
        if (csize == 0) {
            output.insert(output.end(), src, src + usize);
            continue;
        }

        // The task's DMA keeps the source's offset within a 128-byte line.
        const std::uint32_t in_ls = kLsInput + static_cast<std::uint32_t>(item.chunk_offsets[chunk] & 127);
        std::memcpy(thread.ls() + in_ls, src, csize);
        tox2::untagLs(thread, in_ls, csize);

        // The properties sit in the caller's frame: [byte 0 of the item, 0, 0, 1, 0] (dictionary size 0x10000).
        const std::uint32_t props_ls = kLsStack + kLsPropsOffset;
        const std::uint8_t props[kPropsSize] = {item.props[0], 0, 0, 1, 0};
        std::memcpy(thread.ls() + props_ls, props, kPropsSize);
        tox2::untagLs(thread, props_ls, kPropsSize);

        setWord(ctx, 0, kExitLs);
        setWord(ctx, 1, kLsStack);
        TOX2_VR_W(ctx.gpr[1], 1) = kLsStack - kLsBssEnd;
        setWord(ctx, 3, kLsOutput);
        setWord(ctx, 4, usize);
        setWord(ctx, 5, props_ls);
        setWord(ctx, 6, kPropsSize);
        setWord(ctx, 7, in_ls);
        setWord(ctx, 8, csize);

        const auto started = std::chrono::steady_clock::now();
        const tox2::SpuLeave how = tox2::runSpu(thread, kInflateEntry);
        spu_time += std::chrono::steady_clock::now() - started;
        const std::uint32_t result = TOX2_VR_W(ctx.gpr[3], 0);
        if (how != tox2::SpuLeave::Exit || result != 0 || owner.stops != 0) {
            std::fprintf(stderr, "chunk %zu: leave %d, result %u, %d stops\n", chunk, static_cast<int>(how), result,
                         owner.stops.load());
            return 1;
        }
        output.insert(output.end(), thread.ls() + kLsOutput, thread.ls() + kLsOutput + usize);
    }

    const std::string sha256 = sha256Hex(output.data(), output.size());
    std::printf("spu lzma inflate: %#zx bytes sha256 %s\n", output.size(), sha256.c_str());
    const double spu_us = std::chrono::duration<double, std::micro>(spu_time).count();
    std::printf("spu lzma inflate: %.0f us in the recompiled code, %.1f MB/s\n", spu_us, output.size() / spu_us);
    if (output.size() != item.usize || sha256 != kExpectedSha256) {
        std::fprintf(stderr, "expected %#x bytes sha256 %s\n", item.usize, std::string(kExpectedSha256).c_str());
        return 1;
    }
    return 0;
}
