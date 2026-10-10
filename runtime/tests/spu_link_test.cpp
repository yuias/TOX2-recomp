// Links the generated SPU tree against the stub runtime and checks its tables. No SPU code runs: every
// entry is dispatched with a halt request set, so the generated function returns at its block entry.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "tox2/spu_context.h"
#include "tox2/spu_stub.h"

namespace {

int g_failures;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
            g_failures++;                                                          \
        }                                                                          \
    } while (0)

alignas(16) Tox2SpuContext g_ctx;
alignas(16) uint8_t g_ls[kTox2SpuLsSize];

bool isHex64(const char* s) {
    if (s == nullptr || std::strlen(s) != 64) return false;
    for (const char* p = s; *p != '\0'; p++) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return false;
    }
    return true;
}

// The code segment that holds [ls, ls + size), or nullptr.
const Tox2SpuSegment* codeSegmentOf(const Tox2SpuImageInfo& image, uint32_t ls, uint32_t size) {
    for (uint32_t i = 0; i < image.segment_count; i++) {
        const Tox2SpuSegment& s = image.segments[i];
        if (s.code != 0 && ls >= s.ls && ls + size <= s.ls + s.size) return &s;
    }
    return nullptr;
}

void storeWord(uint8_t* ls, uint32_t addr, uint32_t word) {
    for (uint32_t i = 0; i < 4; i++) ls[addr + i] = (uint8_t)(word >> (24 - 8 * i));
}

// Table invariants of one image. Returns the number of dispatched entries.
uint32_t checkImage(const Tox2SpuImageInfo& image) {
    CHECK(image.name != nullptr && image.name[0] != '\0');
    CHECK(isHex64(image.sha256));
    CHECK(image.size > 0);
    CHECK(image.segment_count > 0 && image.segments != nullptr);
    CHECK(image.entry_count > 0 && image.entries != nullptr);

    bool has_code = false;
    for (uint32_t i = 0; i < image.segment_count; i++) {
        const Tox2SpuSegment& s = image.segments[i];
        CHECK(s.size > 0);
        CHECK(s.ls + s.size <= (uint32_t)kTox2SpuLsSize);
        has_code = has_code || s.code != 0;
    }
    CHECK(has_code);

    // The check words of overlapping entries describe the same local-store bytes, so they are laid out
    // into one image and must agree; the dispatcher then verifies each entry against that image.
    std::memset(g_ls, 0, sizeof g_ls);
    std::vector<uint8_t> known(kTox2SpuLsSize / 4, 0);
    bool entry_point_listed = false;
    for (uint32_t i = 0; i < image.entry_count; i++) {
        const Tox2SpuEntry& e = image.entries[i];
        CHECK(i == 0 || image.entries[i - 1].ls < e.ls);
        CHECK(e.ls % 4 == 0);
        CHECK(e.check_count >= 1 && e.check_count <= 4);
        CHECK(e.host != nullptr);
        if (e.check_count < 1 || e.check_count > 4 || e.ls % 4 != 0 || e.ls + 4 * e.check_count > kTox2SpuLsSize) {
            continue;
        }
        CHECK(codeSegmentOf(image, e.ls, 4 * e.check_count) != nullptr);
        entry_point_listed = entry_point_listed || e.ls == image.entry_point;
        for (uint32_t w = 0; w < e.check_count; w++) {
            const uint32_t addr = e.ls + 4 * w;
            if (known[addr / 4] != 0) {
                uint32_t have = (uint32_t)g_ls[addr] << 24 | (uint32_t)g_ls[addr + 1] << 16 |
                                (uint32_t)g_ls[addr + 2] << 8 | g_ls[addr + 3];
                CHECK(have == e.check[w]);
            }
            storeWord(g_ls, addr, e.check[w]);
            known[addr / 4] = 1;
        }
    }
    CHECK(entry_point_listed);

    tox2SpuStubSelectImage(&image);
    uint32_t dispatched = 0;
    for (uint32_t i = 0; i < image.entry_count; i++) {
        const Tox2SpuEntry& e = image.entries[i];
        if (e.check_count < 1 || e.check_count > 4 || e.ls % 4 != 0 || e.ls + 4 * e.check_count > kTox2SpuLsSize) {
            continue;
        }
        std::memset(&g_ctx, 0, sizeof g_ctx);
        g_ctx.exit_ls = kTox2SpuNoExit;
        g_ctx.halt = 1;
        g_ctx.jump_target = e.ls;
        tox2SpuDispatch(&g_ctx, g_ls);
        // The block entry records its address in the trace ring and returns on the halt request.
        CHECK(g_ctx.trace_pos == 1);
        CHECK(g_ctx.trace[0] == e.ls);
        dispatched++;
    }
    tox2SpuStubSelectImage(nullptr);
    return dispatched;
}

}  // namespace

int main() {
    CHECK(kTox2SpuImageCount > 0);

    uint32_t entries = 0;
    uint32_t dispatched = 0;
    for (uint32_t i = 0; i < kTox2SpuImageCount; i++) {
        const Tox2SpuImageInfo& image = kTox2SpuImages[i];
        tox2SpuStubSetLabel(image.name);
        for (uint32_t j = 0; j < i; j++) {
            CHECK(std::strcmp(kTox2SpuImages[j].name, image.name) != 0);
        }
        entries += image.entry_count;
        dispatched += checkImage(image);
    }
    tox2SpuStubSetLabel(nullptr);

    if (g_failures != 0) {
        std::fprintf(stderr, "spu link: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("spu link ok: %u images, %u entries, %u dispatched\n", kTox2SpuImageCount, entries, dispatched);
    return 0;
}
