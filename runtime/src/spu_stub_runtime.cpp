// Minimal runtime for the contract hooks in tox2/spu_context.h. It lets generated SPU code be linked
// and exercised without the real runtime: dispatch works over the selected image's table, everything
// that would need a thread (channels, stops, halts, unknown targets, unhandled words) reports and exits.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "tox2/spu_context.h"
#include "tox2/spu_stub.h"

namespace {

const Tox2SpuImageInfo* g_image = nullptr;
const char* g_label = nullptr;

// std::abort raises a modal "Debug Error!" dialog in debug CRT builds, which stalls unattended test
// runs; exit with a status code instead.
[[noreturn]] void fatalExit() {
    if (g_label != nullptr) std::fprintf(stderr, "tox2: while running %s\n", g_label);
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(3);
}

uint32_t loadWord(const uint8_t* ls, uint32_t addr) {
    const uint8_t* p = ls + (addr & (kTox2SpuLsSize - 1u));
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

}  // namespace

extern "C" {

void tox2SpuStubSelectImage(const Tox2SpuImageInfo* image) { g_image = image; }

void tox2SpuStubSetLabel(const char* label) { g_label = label; }

// The musttail keeps dynamic transfers from adding a host frame per hop.
void tox2SpuDispatch(Tox2SpuContext* __restrict ctx, uint8_t* __restrict ls) {
    const uint32_t target = ctx->jump_target & (kTox2SpuLsSize - 4u);
    if (g_image == nullptr) {
        std::fprintf(stderr, "tox2: spu dispatch to ls 0x%05x with no image selected\n", target);
        fatalExit();
    }
    const Tox2SpuEntry* first = g_image->entries;
    const Tox2SpuEntry* last = first + g_image->entry_count;
    const Tox2SpuEntry* it =
            std::lower_bound(first, last, target, [](const Tox2SpuEntry& e, uint32_t a) { return e.ls < a; });
    if (it == last || it->ls != target) {
        std::fprintf(stderr, "tox2: spu unknown target ls 0x%05x from ls 0x%05x (%s)\n", target, ctx->jump_site,
                     g_image->name);
        fatalExit();
    }
    for (uint32_t i = 0; i < it->check_count; i++) {
        if (loadWord(ls, target + 4 * i) != it->check[i]) {
            std::fprintf(stderr, "tox2: spu code mismatch at ls 0x%05x (%s)\n", target, g_image->name);
            fatalExit();
        }
    }
    [[clang::musttail]] return it->host(ctx, ls);
}

bool tox2SpuReadChannel(Tox2SpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t ch, uint32_t* value) {
    (void)ctx;
    (void)ls;
    (void)value;
    std::fprintf(stderr, "tox2: spu rdch %u at ls 0x%05x\n", ch, site);
    fatalExit();
}

bool tox2SpuWriteChannel(Tox2SpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t ch, uint32_t value) {
    (void)ctx;
    (void)ls;
    std::fprintf(stderr, "tox2: spu wrch %u (0x%08x) at ls 0x%05x\n", ch, value, site);
    fatalExit();
}

uint32_t tox2SpuChannelCount(Tox2SpuContext* ctx, uint8_t* ls, uint32_t ch) {
    (void)ctx;
    (void)ls;
    std::fprintf(stderr, "tox2: spu rchcnt %u\n", ch);
    fatalExit();
}

bool tox2SpuStop(Tox2SpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t code) {
    (void)ctx;
    (void)ls;
    std::fprintf(stderr, "tox2: spu stop 0x%x at ls 0x%05x\n", code, site);
    fatalExit();
}

void tox2SpuHalt(Tox2SpuContext* ctx, uint8_t* ls, uint32_t site) {
    (void)ctx;
    (void)ls;
    std::fprintf(stderr, "tox2: spu halt at ls 0x%05x\n", site);
    fatalExit();
}

void tox2SpuUnhandled(Tox2SpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t word) {
    (void)ctx;
    (void)ls;
    std::fprintf(stderr, "tox2: spu unhandled word 0x%08x at ls 0x%05x\n", word, site);
    fatalExit();
}

}  // extern "C"
