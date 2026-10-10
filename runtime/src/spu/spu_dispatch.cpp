// The image registry, the dispatcher that generated SPU code calls for every computed transfer, and
// the hooks that end the thread: halt and unhandled words.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <string_view>

#include "core/fatal.h"
#include "core/log.h"
#include "spu/spu_thread.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

namespace tox2 {
namespace {

constexpr std::uint16_t kNoEntry = 0xffff;
constexpr std::uint32_t kLookupSize = kTox2SpuLsSize / 4;

struct ImageTable {
    // LS address / 4 -> index into the image's entries.
    std::array<std::uint16_t, kLookupSize> entry_of;
};

const Tox2SpuImageInfo* g_images = nullptr;
std::uint32_t g_image_count = 0;
// Built by registerSpuImages, read by every SPU thread without a lock: registration happens before
// any thread exists.
std::array<std::unique_ptr<ImageTable>, kMaxSpuImages> g_tables;
std::array<std::atomic<bool>, kMaxSpuImages> g_seen;

[[noreturn]] void badTable(const Tox2SpuImageInfo& image, std::string_view why) {
    stopProcess(ExitCode::LoadError, std::format("tox2: spu: image {}: {}", image.name, why));
}

std::string sha256Hex(const std::uint8_t* data, std::uint32_t size) {
    std::array<std::uint8_t, 32> digest{};
    const NTSTATUS status = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data),
                                       static_cast<ULONG>(size), digest.data(), static_cast<ULONG>(digest.size()));
    if (status < 0) {
        return {};
    }
    std::string hex;
    for (const std::uint8_t byte : digest) {
        hex += std::format("{:02x}", byte);
    }
    return hex;
}

std::uint32_t loadWord(const std::uint8_t* ls, std::uint32_t addr) {
    std::uint32_t v;
    std::memcpy(&v, ls + (addr & (kTox2SpuLsSize - 4u)), sizeof v);
    return std::byteswap(v);
}

// The stop paths are out of line so the dispatcher keeps no locals that would block its tail call.
[[noreturn]] TOX2_NOINLINE void stopUnknownTarget(const Tox2SpuContext* ctx, std::uint32_t target,
                                                  const SpuThread& thread, const Tox2SpuImageInfo* image) {
    spuStop(thread, std::format("tox2: stop: spu: unknown target ls {:#07x} from ls {:#07x} on thread {:#010x} ({})",
                                target, ctx->jump_site, thread.id(), image != nullptr ? image->name : "untracked"));
}

[[noreturn]] TOX2_NOINLINE void stopNonLink(const Tox2SpuContext* ctx, std::uint32_t target, const SpuThread& thread,
                                            const Tox2SpuImageInfo& image, std::int32_t delta) {
    spuStop(thread,
            std::format("tox2: stop: spu: target ls {:#07x} from ls {:#07x} on thread {:#010x} ({}) loaded at a "
                        "non-link address (delta {})",
                        target, ctx->jump_site, thread.id(), image.name, delta));
}

[[noreturn]] TOX2_NOINLINE void stopMismatch(std::uint32_t target, const SpuThread& thread,
                                             const Tox2SpuImageInfo& image, const Tox2SpuEntry& entry,
                                             std::uint32_t word_index, std::uint32_t found) {
    spuStop(thread,
            std::format("tox2: stop: spu: code mismatch at ls {:#07x} ({}) on thread {:#010x}", target, image.name,
                        thread.id()),
            std::format("word {} at ls {:#07x}: LS has {:#010x}, the image has {:#010x}", word_index,
                        target + 4 * word_index, found, entry.check[word_index]));
}

}  // namespace

void registerSpuImages(const Tox2SpuImageInfo* images, std::uint32_t count) {
    if (count > kMaxSpuImages) {
        stopProcess(ExitCode::LoadError,
                    std::format("tox2: spu: {} images registered, at most {}", count, kMaxSpuImages));
    }
    g_images = nullptr;
    g_image_count = 0;
    for (std::uint32_t i = 0; i < kMaxSpuImages; i++) {
        g_tables[i].reset();
        g_seen[i].store(false);
    }
    for (std::uint32_t i = 0; i < count; i++) {
        const Tox2SpuImageInfo& image = images[i];
        if (image.entry_count >= kNoEntry) badTable(image, "too many entries");
        auto table = std::make_unique<ImageTable>();
        table->entry_of.fill(kNoEntry);
        for (std::uint32_t e = 0; e < image.entry_count; e++) {
            const Tox2SpuEntry& entry = image.entries[e];
            if (entry.ls % 4 != 0 || entry.ls >= kTox2SpuLsSize) {
                badTable(image, std::format("entry ls {:#x} is not a word in the LS", entry.ls));
            }
            if (entry.check_count < 1 || entry.check_count > 4 || entry.host == nullptr) {
                badTable(image, std::format("entry ls {:#x} has no check words or no function", entry.ls));
            }
            if (e > 0 && image.entries[e - 1].ls >= entry.ls) {
                badTable(image, std::format("entries are not sorted at ls {:#x}", entry.ls));
            }
            table->entry_of[entry.ls / 4] = static_cast<std::uint16_t>(e);
        }
        g_tables[i] = std::move(table);
    }
    g_images = images;
    g_image_count = count;
}

std::uint32_t spuImageCount() { return g_image_count; }

const Tox2SpuImageInfo& spuImage(std::uint32_t index) { return g_images[index]; }

const Tox2SpuEntry* spuFindEntry(std::uint32_t index, std::uint32_t ls) {
    const std::uint16_t e = g_tables[index]->entry_of[(ls & (kTox2SpuLsSize - 1u)) / 4];
    return e == kNoEntry ? nullptr : &g_images[index].entries[e];
}

bool spuMarkImageSeen(std::uint32_t index) { return !g_seen[index].exchange(true); }

bool checkSpuImages(const std::uint8_t* base) {
    bool ok = true;
    for (std::uint32_t i = 0; i < g_image_count; i++) {
        const Tox2SpuImageInfo& image = g_images[i];
        const std::string got = sha256Hex(base + image.ea, image.size);
        if (got != image.sha256) {
            logError("tox2: spu: image {} at {:#010x}: hash {} does not match the generated code ({})", image.name,
                     image.ea, got.empty() ? "(unavailable)" : got, image.sha256);
            ok = false;
        }
    }
    if (ok) {
        logInfo("tox2: spu: {} images ok", g_image_count);
    }
    return ok;
}

}  // namespace tox2

using tox2::SpuLoadRecord;
using tox2::SpuThread;

extern "C" {

void tox2SpuDispatch(Tox2SpuContext* __restrict ctx, uint8_t* __restrict ls) {
    const uint32_t target = ctx->jump_target & (kTox2SpuLsSize - 4u);
    if (target == ctx->exit_ls || tox2SpuHaltRequested(ctx)) {
        return;
    }
    SpuThread* thread = tox2::spuThreadOf(ctx);
    const SpuLoadRecord* record = thread->recordAt(target);
    // Resolved by the tracked identity of the target, not by the image of the branching code: the
    // kernel returns into policy modules and policy modules start tasks.
    if (__builtin_expect(record == nullptr, 0)) {
        tox2::stopUnknownTarget(ctx, target, *thread, nullptr);
    }
    const Tox2SpuImageInfo& image = tox2::spuImage(record->image);
    if (__builtin_expect(record->delta != 0, 0)) {
        tox2::stopNonLink(ctx, target, *thread, image, record->delta);
    }
    const Tox2SpuEntry* entry = tox2::spuFindEntry(record->image, target);
    if (__builtin_expect(entry == nullptr, 0)) {
        tox2::stopUnknownTarget(ctx, target, *thread, &image);
    }
    for (uint32_t i = 0; i < entry->check_count; i++) {
        const uint32_t word = tox2::loadWord(ls, target + 4 * i);
        if (__builtin_expect(word != entry->check[i], 0)) {
            tox2::stopMismatch(target, *thread, image, *entry, i, word);
        }
    }
    thread->countDispatch(record->image);
    TOX2_MUSTTAIL return entry->host(ctx, ls);
}

void tox2SpuHalt(Tox2SpuContext* ctx, uint8_t* ls, uint32_t site) {
    (void)ls;
    const SpuThread& thread = *tox2::spuThreadOf(ctx);
    tox2::spuStop(thread, std::format("tox2: stop: spu: halt at ls {:#07x} on thread {:#010x}", site, thread.id()));
}

void tox2SpuUnhandled(Tox2SpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t word) {
    (void)ls;
    const SpuThread& thread = *tox2::spuThreadOf(ctx);
    tox2::spuStop(thread, std::format("tox2: stop: spu: unhandled word {:#010x} at ls {:#07x} on thread {:#010x}", word,
                                      site, thread.id()));
}

}  // extern "C"
