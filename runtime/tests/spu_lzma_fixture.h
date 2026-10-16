// Shared fixture of the recompiled LZMA inflate: a TLZC item of TLFILE.TLDAT is inflated chunk by chunk through the
// recompiled edgeLzmaInflateRawData of the game's LZMA SPURS task (image eboot_edgelzma) the way the task's main
// loop does it. Used by spu_lzma_inflate_test (correctness) and tox2_bench (cycles), so both run the same code.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/elf_loader.h"
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "core/thread_cpu.h"
#include "spu/spu_thread.h"
#include "tox2/spu_context.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

namespace tox2::lzma_fixture {

inline constexpr std::uint64_t kItemOffset = 0x336bb50f;
inline constexpr std::uint32_t kItemUsize = 0x3073c;
inline constexpr std::string_view kExpectedSha256 = "f4ad1f7125d9b31abf228e8043587aebc8f1501f0f4debed6d9814d06a12c751";

inline constexpr std::uint32_t kInflateEntry = 0x3628;
inline constexpr std::uint32_t kChunkSize = 0x10000;
inline constexpr std::uint32_t kHeaderSize = 0x18;
inline constexpr std::uint32_t kPropsSize = 5;

// Local store layout of the task around its call to edgeLzmaInflateRawData: the output buffer is where the
// task's main loop places it, the input buffer follows the task's data segment.
inline constexpr std::uint32_t kLsOutput = 0x17980;
inline constexpr std::uint32_t kLsInput = 0x7900;
inline constexpr std::uint32_t kLsStack = 0x3ff00;
inline constexpr std::uint32_t kLsPropsOffset = 32;
// The task's crt sets the stack pointer's second word to the stack space left below the bss end.
inline constexpr std::uint32_t kLsBssEnd = 0x37c80;
// No code or data of the task lives here, so a return through $0 is recognised by the dispatcher alone.
inline constexpr std::uint32_t kExitLs = 0x3fff0;

struct Owner : SpuOwner {
    std::atomic<int> stops{0};
    void onInterruptMailbox(SpuThread&, std::uint32_t) override {}
    bool onStop(SpuThread&, std::uint32_t, std::uint32_t) override {
        stops++;
        return false;
    }
    void onFinished(SpuThread&, SpuLeave) override {}
};

inline std::string sha256Hex(const std::uint8_t* data, std::size_t size) {
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

inline std::uint32_t le32(const std::uint8_t* p) {
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

inline bool readItem(const std::filesystem::path& tldat, Item* item) {
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

inline void setWord(Tox2SpuContext& ctx, unsigned reg, std::uint32_t value) {
    ctx.gpr[reg] = {};
    TOX2_VR_W(ctx.gpr[reg], 0) = value;
}

// What one inflate of the whole item produced and what the recompiled code cost.
struct Inflate {
    std::vector<std::uint8_t> output;
    std::chrono::steady_clock::duration time{};  // wall time inside the runSpu calls
    std::uint64_t cycles = 0;                    // thread cycles inside the runSpu calls (calling thread)
};

class Fixture {
public:
    // Reads the item, loads the executable into the process, registers the SPU images and checks the task image.
    // Process-wide state is touched, so one Fixture per process.
    bool init(const std::filesystem::path& game_dir, const std::filesystem::path& eboot_elf) {
        const std::filesystem::path tldat = game_dir / "PS3_GAME" / "USRDIR" / "TLFILE.TLDAT";

        installProcessSettings();
        if (!readItem(tldat, &item_)) return false;

        // Lines go to stderr as they come: a stop during setup ends the process before they could be flushed.
        setLogSink([](std::string_view line) {
            std::fprintf(stderr, "%.*s\n", static_cast<int>(line.size()), line.data());
        });
        if (!process().memory.reserve()) {
            std::fprintf(stderr, "cannot reserve the guest address space\n");
            return false;
        }
        const auto loaded = loadExecutable(eboot_elf);
        if (!loaded) {
            std::fprintf(stderr, "load failed: %s\n", loaded.error().c_str());
            return false;
        }
        installFaultHandler();
        reserveFaultStack();
        base_ = process().memory.base();

        registerSpuImages(kTox2SpuImages, kTox2SpuImageCount);

        for (std::uint32_t i = 0; i < kTox2SpuImageCount; i++) {
            if (std::string_view(kTox2SpuImages[i].name) == "eboot_edgelzma") image_ = &kTox2SpuImages[i];
        }
        if (image_ == nullptr) {
            std::fprintf(stderr, "image eboot_edgelzma is not in the generated table\n");
            return false;
        }
        // Only this image is checked: the libsre images live in a module that the test does not load.
        if (sha256Hex(base_ + image_->ea, image_->size) != image_->sha256) {
            std::fprintf(stderr, "image eboot_edgelzma does not match the generated code\n");
            return false;
        }
        return true;
    }

    // Inflates the whole item on a fresh SPU thread (zeroed local store, so every call starts from the same state).
    // The per-chunk setup is outside the timed span; only the runSpu calls are timed.
    bool inflate(Inflate* out) const {
        Owner owner;
        SpuThread thread(0x01000100, "lzma_inflate", &owner);
        for (std::uint32_t i = 0; i < image_->segment_count; i++) {
            const Tox2SpuSegment& s = image_->segments[i];
            copyToLs(thread, base_, s.ea, s.ls, s.size);
        }
        Tox2SpuContext& ctx = thread.context();
        ctx.exit_ls = kExitLs;

        out->time = {};
        out->cycles = 0;
        out->output.clear();
        out->output.reserve(item_.usize);
        for (std::size_t chunk = 0; chunk < item_.chunk_sizes.size(); chunk++) {
            const std::uint32_t usize =
                    std::min(kChunkSize, item_.usize - static_cast<std::uint32_t>(chunk) * kChunkSize);
            const std::uint32_t csize = item_.chunk_sizes[chunk];
            const std::uint8_t* src = item_.payload.data() + (item_.chunk_offsets[chunk] - kItemOffset - kHeaderSize);
            if (csize == 0) {
                out->output.insert(out->output.end(), src, src + usize);
                continue;
            }

            // The task's DMA keeps the source's offset within a 128-byte line.
            const std::uint32_t in_ls = kLsInput + static_cast<std::uint32_t>(item_.chunk_offsets[chunk] & 127);
            std::memcpy(thread.ls() + in_ls, src, csize);
            untagLs(thread, in_ls, csize);

            // The properties sit in the caller's frame: [byte 0 of the item, 0, 0, 1, 0] (dictionary size 0x10000).
            const std::uint32_t props_ls = kLsStack + kLsPropsOffset;
            const std::uint8_t props[kPropsSize] = {item_.props[0], 0, 0, 1, 0};
            std::memcpy(thread.ls() + props_ls, props, kPropsSize);
            untagLs(thread, props_ls, kPropsSize);

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
            const std::uint64_t cycles_before = currentThreadCycles();
            const SpuLeave how = runSpu(thread, kInflateEntry);
            out->cycles += currentThreadCycles() - cycles_before;
            out->time += std::chrono::steady_clock::now() - started;
            const std::uint32_t result = TOX2_VR_W(ctx.gpr[3], 0);
            if (how != SpuLeave::Exit || result != 0 || owner.stops != 0) {
                std::fprintf(stderr, "chunk %zu: leave %d, result %u, %d stops\n", chunk, static_cast<int>(how), result,
                             owner.stops.load());
                return false;
            }
            out->output.insert(out->output.end(), thread.ls() + kLsOutput, thread.ls() + kLsOutput + usize);
        }
        return true;
    }

    std::uint32_t itemUsize() const { return item_.usize; }

private:
    Item item_;
    const std::uint8_t* base_ = nullptr;
    const Tox2SpuImageInfo* image_ = nullptr;
};

}  // namespace tox2::lzma_fixture
