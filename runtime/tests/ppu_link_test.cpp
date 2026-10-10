// Links the generated tree against the stub runtime and checks its tables. No game code runs.

#include <cstdint>
#include <cstdio>

#include "dal/ppu_context.h"

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
    const uint32_t count = kDalPpuFuncMappingCount;
    dalRegisterFuncs(kDalPpuFuncMappings, count);

    for (uint32_t i = 0; i < count; i++) {
        const DalFuncMapping& m = kDalPpuFuncMappings[i];
        CHECK(m.host != nullptr);
        CHECK(i == 0 || kDalPpuFuncMappings[i - 1].guest < m.guest);
        CHECK(dalLookup(m.guest) == m.host);
    }

    for (uint32_t i = 0; i < kDalPpuImportCount; i++) {
        const DalImportInfo& info = kDalPpuImports[i];
        CHECK(info.module != nullptr);
        DalPpuFunc* host = dalLookup(info.stub);
        CHECK(host != nullptr);
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "ppu link: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ppu link ok: %u mappings, %u imports, executable %.16s\n", count, kDalPpuImportCount,
                kDalPpuExecutableSha256);
    return 0;
}
