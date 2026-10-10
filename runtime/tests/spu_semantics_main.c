// Runs the generated SPU semantics cases (spu_semantics_cases.c).
//
// The driver is the whole runtime of a case: it provides the dispatcher over the case's entry table,
// scripted channel reads and event counts, recorded channel writes and stop codes. A case ends when
// the generated code leaves (stop, halt) or dispatches to the exit address. A case that reaches a
// fatal hook (unknown target, unhandled word, halt, an unscripted channel read) ends the process with
// status 3 before the summary line is printed, so CTest fails; the message names the case. Pass case
// names as arguments to run only those.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spu_semantics_cases.h"

static const Tox2SemanticsCase* g_case;
static Tox2SemanticsLog g_log;
static bool g_read_used[1024];

[[noreturn]] static void fatal(const char* what, uint32_t a, uint32_t b) {
    fprintf(stderr, "spu semantics: case %s: %s (0x%05x, 0x%08x)\n", g_case != nullptr ? g_case->name : "?", what, a,
            b);
    exit(3);
}

static uint32_t loadWord(const uint8_t* ls, uint32_t addr) {
    return (uint32_t)ls[addr] << 24 | (uint32_t)ls[addr + 1] << 16 | (uint32_t)ls[addr + 2] << 8 | ls[addr + 3];
}

// The dispatcher of the real runtime reduced to what a case needs: mask, exit address, entry table,
// check words, tail call.
void tox2SpuDispatch(Tox2SpuContext* __restrict ctx, uint8_t* __restrict ls) {
    uint32_t target = ctx->jump_target & 0x3fffcu;
    if (target == ctx->exit_ls) return;
    for (uint32_t i = 0; i < g_case->entry_count; i++) {
        const Tox2SpuEntry* entry = &g_case->entries[i];
        if (entry->ls != target) continue;
        for (uint32_t w = 0; w < entry->check_count; w++) {
            if (loadWord(ls, target + 4 * w) != entry->check[w]) fatal("code mismatch", target, ctx->jump_site);
        }
        TOX2_MUSTTAIL return entry->host(ctx, ls);
    }
    fatal("unknown dispatch target", target, ctx->jump_site);
}

bool tox2SpuReadChannel(Tox2SpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t ch, uint32_t* value) {
    (void)ctx;
    (void)ls;
    for (uint32_t i = 0; i < g_case->read_count && i < sizeof g_read_used / sizeof g_read_used[0]; i++) {
        if (g_read_used[i] || g_case->reads[i].channel != ch) continue;
        g_read_used[i] = true;
        *value = g_case->reads[i].value;
        return true;
    }
    fatal("unscripted channel read", site, ch);
}

bool tox2SpuWriteChannel(Tox2SpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t ch, uint32_t value) {
    (void)ctx;
    (void)ls;
    if (g_log.write_count >= kTox2SemanticsLogSize) fatal("too many channel writes", site, ch);
    g_log.writes[g_log.write_count++] = (uint64_t)ch << 32 | value;
    return true;
}

uint32_t tox2SpuChannelCount(Tox2SpuContext* ctx, uint8_t* ls, uint32_t ch) {
    (void)ctx;
    (void)ls;
    for (uint32_t i = 0; i < g_case->count_count; i++) {
        if (g_case->counts[i].channel == ch) return g_case->counts[i].value;
    }
    return 0;
}

// Continues unless the case scripts a refusal for this code. The generated code ends the thread
// after 0x101 and 0x102 whatever the hook returns.
bool tox2SpuStop(Tox2SpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t code) {
    (void)ctx;
    (void)ls;
    if (g_log.stop_count >= kTox2SemanticsLogSize) fatal("too many stops", site, code);
    g_log.stops[g_log.stop_count++] = code;
    for (uint32_t i = 0; i < g_case->leave_stop_count; i++) {
        if (g_case->leave_stops[i] == code) return false;
    }
    return true;
}

[[noreturn]] void tox2SpuHalt(Tox2SpuContext* ctx, uint8_t* ls, uint32_t site) {
    (void)ctx;
    (void)ls;
    fatal("halt", site, 0);
}

[[noreturn]] void tox2SpuUnhandled(Tox2SpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t word) {
    (void)ctx;
    (void)ls;
    fatal("unhandled word", site, word);
}

static bool selected(const Tox2SemanticsCase* c, int argc, char** argv) {
    if (argc <= 1) return true;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], c->name) == 0) return true;
    }
    return false;
}

// Returns true if the case passed.
static bool runCase(const Tox2SemanticsCase* c, Tox2SpuContext* ctx, uint8_t* ls) {
    memset(ctx, 0, sizeof *ctx);
    memset(ls, 0, kTox2SpuLsSize);
    memset(&g_log, 0, sizeof g_log);
    memset(g_read_used, 0, sizeof g_read_used);
    ctx->exit_ls = kTox2SemanticsExitLs;
    g_case = c;

    c->init(ctx, ls);
    ctx->jump_target = kTox2SemanticsCodeBase;
    tox2SpuDispatch(ctx, ls);

    char msg[256] = "";
    uint32_t mismatches = c->check(ctx, ls, &g_log, msg, sizeof msg);
    g_case = nullptr;
    if (mismatches == 0) return true;

    printf("FAIL %s: %s", c->name, msg);
    if (mismatches > 1) printf(" (+%u more)", (unsigned)(mismatches - 1));
    putchar('\n');
    return false;
}

int main(int argc, char** argv) {
    static Tox2SpuContext ctx;
    alignas(16) static uint8_t ls[kTox2SpuLsSize];

    uint32_t passed = 0, failed = 0;
    for (uint32_t i = 0; i < kTox2SemanticsCaseCount; i++) {
        const Tox2SemanticsCase* c = &kTox2SemanticsCases[i];
        if (!selected(c, argc, argv)) continue;
        if (runCase(c, &ctx, ls)) {
            passed++;
        } else {
            failed++;
        }
    }

    if (argc > 1 && passed + failed == 0) {
        fputs("spu semantics: no case matches the given names\n", stderr);
        return 2;
    }
    printf("spu semantics: %u passed, %u failed\n", passed, failed);
    return failed != 0;
}
