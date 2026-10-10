// Runs the generated PPU semantics cases (ppu_semantics_cases.c) against the stub runtime.
//
// All cases run in one process. A case that reaches a fatal stub hook (unhandled instruction,
// trap, unknown target) ends the process with the stub's exit status before the summary line is
// printed, so CTest fails; the stub names the running case on stderr. Pass case names as arguments
// to run only those.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tox2/ppu.h"
#include "tox2/ppu_stub.h"
#include "ppu_semantics_cases.h"

// Keep in sync with MEM_SIZE in tools/tests/ppu_semantics/__init__.py.
enum { kSemanticsMemSize = 0x100000 };

static bool selected(const Tox2SemanticsCase* c, int argc, char** argv) {
    if (argc <= 1) return true;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], c->name) == 0) return true;
    }
    return false;
}

// Returns true if the case passed.
static bool runCase(const Tox2SemanticsCase* c, uint8_t* mem) {
    memset(mem, 0, (size_t)kSemanticsMemSize + kTox2GuestGuardSize);

    Tox2PpuContext ctx = {};
    ctx.preempt_budget = kTox2PreemptBudget;
    ctx.vscr_nj = 1;

    tox2StubResetFuncs();
    tox2RegisterFuncs(c->mappings, c->mapping_count);
    tox2StubSetLabel(c->name);

    c->init(&ctx, mem);
    c->entry(&ctx, mem);

    char msg[256] = "";
    uint32_t mismatches = c->check(&ctx, mem, msg, sizeof msg);
    tox2StubSetLabel(nullptr);
    if (mismatches == 0) return true;

    printf("FAIL %s: %s", c->name, msg);
    if (mismatches > 1) printf(" (+%u more)", (unsigned)(mismatches - 1));
    putchar('\n');
    return false;
}

int main(int argc, char** argv) {
    uint8_t* mem = calloc((size_t)kSemanticsMemSize + kTox2GuestGuardSize, 1);
    if (mem == nullptr) {
        fputs("ppu semantics: out of memory\n", stderr);
        return 2;
    }

    uint32_t passed = 0, failed = 0;
    for (uint32_t i = 0; i < kTox2SemanticsCaseCount; i++) {
        const Tox2SemanticsCase* c = &kTox2SemanticsCases[i];
        if (!selected(c, argc, argv)) continue;
        if (runCase(c, mem)) {
            passed++;
        } else {
            failed++;
        }
    }
    free(mem);

    if (argc > 1 && passed + failed == 0) {
        fputs("ppu semantics: no case matches the given names\n", stderr);
        return 2;
    }
    printf("ppu semantics: %u passed, %u failed\n", passed, failed);
    return failed != 0;
}
