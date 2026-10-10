// Links the generated tree against the stub runtime and checks its tables. No game code runs.

#include <cstdint>
#include <cstdio>

#include "tox2/ppu_context.h"

namespace {

int g_failures;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
            g_failures++;                                                          \
        }                                                                          \
    } while (0)

}  // namespace

int main() {
    const uint32_t count = kTox2PpuFuncMappingCount;
    tox2RegisterFuncs(kTox2PpuFuncMappings, count);

    for (uint32_t i = 0; i < count; i++) {
        const Tox2FuncMapping& m = kTox2PpuFuncMappings[i];
        CHECK(m.host != nullptr);
        CHECK(i == 0 || kTox2PpuFuncMappings[i - 1].guest < m.guest);
        CHECK(tox2Lookup(m.guest) == m.host);
    }

    for (uint32_t i = 0; i < kTox2PpuImportCount; i++) {
        const Tox2ImportInfo& info = kTox2PpuImports[i];
        CHECK(info.module != nullptr);
        Tox2PpuFunc* host = tox2Lookup(info.stub);
        CHECK(host != nullptr);
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "ppu link: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ppu link ok: %u mappings, %u imports, executable %.16s\n", count, kTox2PpuImportCount,
                kTox2PpuExecutableSha256);
    return 0;
}
