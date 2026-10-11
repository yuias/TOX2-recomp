// Links the generated tree against the stub runtime and checks its tables. No game code runs.

#include <cstdint>
#include <cstdio>

#include "libsre_ppu_tables.h"
#include "tox2/ppu_context.h"
#include "tox2/spu_context.h"

namespace {

int g_failures;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
            g_failures++;                                                          \
        }                                                                          \
    } while (0)

// Every mapping is sorted and resolves to its own host function, and every import stub is a registered
// function (the trampoline).
void checkImage(const Tox2FuncMapping* mappings, uint32_t count, const Tox2ImportInfo* imports, uint32_t import_count) {
    for (uint32_t i = 0; i < count; i++) {
        const Tox2FuncMapping& m = mappings[i];
        CHECK(m.host != nullptr);
        CHECK(i == 0 || mappings[i - 1].guest < m.guest);
        CHECK(tox2Lookup(m.guest) == m.host);
    }

    for (uint32_t i = 0; i < import_count; i++) {
        const Tox2ImportInfo& info = imports[i];
        CHECK(info.module != nullptr);
        Tox2PpuFunc* host = tox2Lookup(info.stub);
        CHECK(host != nullptr);
    }
}

}  // namespace

int main() {
    const uint32_t count = kTox2PpuFuncMappingCount;
    tox2RegisterFuncs(kTox2PpuFuncMappings, count);
    checkImage(kTox2PpuFuncMappings, count, kTox2PpuImports, kTox2PpuImportCount);

    // The stub runtime keeps one merged table, so a second call adds the libsre mappings to it.
    tox2RegisterFuncs(kTox2LibsrePpuFuncMappings, kTox2LibsrePpuFuncMappingCount);
    checkImage(kTox2LibsrePpuFuncMappings, kTox2LibsrePpuFuncMappingCount, kTox2LibsrePpuImports,
               kTox2LibsrePpuImportCount);
    // The first image must stay reachable after the second span was added.
    checkImage(kTox2PpuFuncMappings, count, kTox2PpuImports, kTox2PpuImportCount);

    CHECK(kTox2LibsrePpuExportCount > 0);
    for (uint32_t i = 0; i < kTox2LibsrePpuExportCount; i++) {
        CHECK(kTox2LibsrePpuExports[i].module != nullptr);
    }

    // The game registers these tables next to the PPU ones; here they only have to link and be well formed.
    CHECK(kTox2SpuImageCount > 0);
    for (uint32_t i = 0; i < kTox2SpuImageCount; i++) {
        CHECK(kTox2SpuImages[i].name != nullptr);
        CHECK(kTox2SpuImages[i].entry_count > 0 && kTox2SpuImages[i].entries != nullptr);
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "ppu link: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf(
            "ppu link ok: %u mappings, %u imports, executable %.16s; libsre: %u mappings, %u imports, %u exports, "
            "image %.16s; spu: %u images\n",
            count, kTox2PpuImportCount, kTox2PpuExecutableSha256, kTox2LibsrePpuFuncMappingCount,
            kTox2LibsrePpuImportCount, kTox2LibsrePpuExportCount, kTox2LibsrePpuExecutableSha256, kTox2SpuImageCount);
    return 0;
}
