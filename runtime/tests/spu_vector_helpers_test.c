// The SIMD forms of the SPU helpers against their lane-by-lane definitions: the single-precision
// arithmetic of spu_fp.h, run in the SPU float environment (round toward zero, FTZ, DAZ), against the
// scalar tox2SpuSf* functions run in round to nearest, shufb against a byte loop that follows the
// ISA text, and the quadword shifts and rotates against their unsigned __int128 definitions. Random
// words weighted toward the cases that differ (zero and denormal exponents, exponent 255, results near
// Smin and Smax, cancellation), and every combination of a list of edge words. The helpers that need
// round to nearest must give the same result in both environments.

#include <stdio.h>

#include "tox2/spu_context.h"
#include "tox2/spu_ops.h"

static uint64_t g_state = 0x9e3779b97f4a7c15ull;

static uint32_t next(void) {
    g_state ^= g_state << 13;
    g_state ^= g_state >> 7;
    g_state ^= g_state << 17;
    return (uint32_t)g_state;
}

static const uint32_t kEdges[] = {
        0x00000000u, 0x80000000u,                            // zeros
        0x00000001u, 0x80000001u, 0x007fffffu, 0x807fffffu,  // denormals, both signs
        0x00800000u, 0x80800000u, 0x00800001u, 0x01000000u,  // Smin and just above
        0x3f800000u, 0xbf800000u, 0x3f7fffffu, 0x34000000u, 0x33800000u, 0x4b800000u,
        0x00ffffffu, 0x3fffffffu, 0x1f800000u, 0x5f800000u, 0xdf800000u,               // 2^-64, 2^64
        0x7f000000u, 0x7f7ffffeu, 0x7f7fffffu, 0xff7fffffu,                            // up to the largest single
        0x7f800000u, 0xff800000u, 0x7f800001u, 0xff800001u, 0x7fffffffu, 0xffffffffu,  // exponent 255
};
static const int kEdgeCount = (int)(sizeof kEdges / sizeof kEdges[0]);

static uint32_t exponentOf(uint32_t w) { return (w >> 23) & 0xffu; }

// A word with exponent `e` clamped to 1..255, and a random sign and fraction.
static uint32_t withExponent(int e) {
    if (e < 1) e = 1;
    if (e > 255) e = 255;
    return (next() & 0x807fffffu) | ((uint32_t)e << 23);
}

static uint32_t pickWord(void) {
    switch (next() & 7u) {
        case 0: return kEdges[next() % (uint32_t)kEdgeCount];
        case 1: return (next() & 0x807fffffu) | ((uint32_t)(0x7c + next() % 7u) << 23);  // near 1.0
        case 2: return (next() & 0x80ffffffu) | 0x00800000u;                             // near Smin
        case 3: return next() | 0x7f000000u;                                             // near Smax
        default: return next();
    }
}

enum { kBatch = 512 };

// The sticky exception flags in the low bits of the MXCSR are not part of the environment.
static const uint32_t kControlBits = 0xffc0u;

static Tox2Vr g_a[kBatch];
static Tox2Vr g_b[kBatch];
static Tox2Vr g_c[kBatch];
static Tox2Vr g_got[6][kBatch];
static long g_bad;

// The vector helpers run in the SPU environment, as in the runtime; the scalar definitions need round
// to nearest, so they run in the default one.
static void checkBatch(int n) {
    const uint32_t saved = tox2SpuMxcsrGet();
    tox2SpuMxcsrSet(kTox2SpuMxcsrSpu);
    for (int i = 0; i < n; i++) {
        g_got[0][i] = tox2SpuFa(g_a[i], g_b[i]);
        g_got[1][i] = tox2SpuFs(g_a[i], g_b[i]);
        g_got[2][i] = tox2SpuFm(g_a[i], g_b[i]);
        g_got[3][i] = tox2SpuFma(g_a[i], g_b[i], g_c[i]);
        g_got[4][i] = tox2SpuFms(g_a[i], g_b[i], g_c[i]);
        g_got[5][i] = tox2SpuFnms(g_a[i], g_b[i], g_c[i]);
    }
    if ((tox2SpuMxcsrGet() & kControlBits) != kTox2SpuMxcsrSpu && g_bad++ < 10) {
        printf("the vector helpers changed the MXCSR to %08x\n", (unsigned)tox2SpuMxcsrGet());
    }
    tox2SpuMxcsrSet(kTox2SpuMxcsrHost);
    for (int i = 0; i < n; i++) {
        for (int lane = 0; lane < 4; lane++) {
            const uint32_t x = g_a[i].u32[lane];
            const uint32_t y = g_b[i].u32[lane];
            const uint32_t z = g_c[i].u32[lane];
            const uint32_t want[6] = {tox2SpuSfAdd(x, y),       tox2SpuSfSub(x, y),       tox2SpuSfMul(x, y),
                                      tox2SpuSfMulAdd(x, y, z), tox2SpuSfMulSub(x, y, z), tox2SpuSfNegMulSub(x, y, z)};
            for (int op = 0; op < 6; op++) {
                if (g_got[op][i].u32[lane] != want[op] && g_bad++ < 10) {
                    printf("float op %d (%08x, %08x, %08x): %08x, expected %08x\n", op, x, y, z, g_got[op][i].u32[lane],
                           want[op]);
                }
            }
        }
    }
    tox2SpuMxcsrSet(saved);
}

static int checkFloat(void) {
    g_bad = 0;
    int n = 0;
    for (long it = 0; it < 1000000; it++) {
        Tox2Vr a;
        Tox2Vr b;
        Tox2Vr c;
        for (int i = 0; i < 4; i++) {
            a.u32[i] = pickWord();
            b.u32[i] = pickWord();
            c.u32[i] = pickWord();
            switch (it & 7) {
                case 0: b.u32[i] = (a.u32[i] ^ 0x80000000u) + next() % 5u - 2u; break;  // cancellation
                case 1:                                                                 // product at Smin
                    b.u32[i] = withExponent(128 - (int)exponentOf(a.u32[i]) + (int)(next() % 3u) - 1);
                    break;
                case 2:  // product at the largest single, and beyond it
                    b.u32[i] = withExponent(381 - (int)exponentOf(a.u32[i]) + (int)(next() % 3u) - 1);
                    break;
                case 3:  // the accumulator cancels the product, or sits a few steps from it
                    c.u32[i] = (tox2SpuSfMul(a.u32[i], b.u32[i]) ^ (next() & 0x80000000u)) + next() % 5u - 2u;
                    break;
                default: break;
            }
        }
        g_a[n] = a;
        g_b[n] = b;
        g_c[n] = c;
        if (++n == kBatch) {
            checkBatch(n);
            n = 0;
        }
    }
    if (n > 0) checkBatch(n);
    return g_bad == 0;
}

// Operands that send a lane through the exact path: overflow of the products and sums, and exponent-255
// operands, with both signs, next to values that stay on the fast path.
static const uint32_t kForcing[] = {0x7f7fffffu, 0xff7fffffu, 0x7f000000u, 0x40800000u, 0xc0800000u, 0x7f800000u,
                                    0xff800000u, 0x7fffffffu, 0xffffffffu, 0x3f800000u, 0x00000000u, 0x80000000u};
static const int kForcingCount = (int)(sizeof kForcing / sizeof kForcing[0]);

// True when the fast path of operation `op` rejects the vector, so the exact path produces the merged result.
static int needsExact(int op, Tox2Vr a, Tox2Vr b, Tox2Vr c) {
    Tox2Vr r;
    switch (op) {
        case 0: r.f32 = a.f32 + b.f32; break;
        case 1: r.f32 = a.f32 - b.f32; break;
        case 2: r.f32 = a.f32 * b.f32; break;
        case 3: r.f32 = __builtin_elementwise_fma(a.f32, b.f32, c.f32); break;
        case 4: r.f32 = __builtin_elementwise_fma(a.f32, b.f32, -c.f32); break;
        default: r.f32 = __builtin_elementwise_fma(-a.f32, b.f32, c.f32); break;
    }
    return tox2SpuSf4NeedsExact(r.u32);
}

// The wrappers on vectors where one lane takes the exact path and the other three stay on the fast
// path, and the exact-path functions themselves on the same vectors. Checks the merge of the two results
// per lane, and that the forcing inputs do reach the exact path of every operation.
static int checkExactPaths(void) {
    long bad = 0;
    long forced[6] = {0, 0, 0, 0, 0, 0};
    const uint32_t saved = tox2SpuMxcsrGet();
    for (int x = 0; x < kForcingCount; x++) {
        for (int y = 0; y < kForcingCount; y++) {
            for (int z = 0; z < kForcingCount; z++) {
                for (int lane = 0; lane < 4; lane++) {
                    Tox2Vr a;
                    Tox2Vr b;
                    Tox2Vr c;
                    for (int i = 0; i < 4; i++) {
                        a.u32[i] = 0x3f800000u + (uint32_t)(i + 1) * 0x100000u;
                        b.u32[i] = 0x40000000u + (uint32_t)i * 0x80000u;
                        c.u32[i] = 0x3f000000u + (uint32_t)i * 0x10000u;
                    }
                    a.u32[lane] = kForcing[x];
                    b.u32[lane] = kForcing[y];
                    c.u32[lane] = kForcing[z];

                    Tox2Vr got[6];
                    Tox2U32x4 exact[6];
                    int hit[6];
                    tox2SpuMxcsrSet(kTox2SpuMxcsrSpu);
                    for (int op = 0; op < 6; op++) hit[op] = needsExact(op, a, b, c);
                    got[0] = tox2SpuFa(a, b);
                    got[1] = tox2SpuFs(a, b);
                    got[2] = tox2SpuFm(a, b);
                    got[3] = tox2SpuFma(a, b, c);
                    got[4] = tox2SpuFms(a, b, c);
                    got[5] = tox2SpuFnms(a, b, c);
                    exact[0] = tox2SpuFaExact(a, b);
                    exact[1] = tox2SpuFsExact(a, b);
                    exact[2] = tox2SpuFmExact(a, b);
                    exact[3] = tox2SpuFmaExact(a, b, c);
                    exact[4] = tox2SpuFmsExact(a, b, c);
                    exact[5] = tox2SpuFnmsExact(a, b, c);
                    if ((tox2SpuMxcsrGet() & kControlBits) != kTox2SpuMxcsrSpu && bad++ < 10) {
                        printf("an exact-path function changed the MXCSR to %08x\n", (unsigned)tox2SpuMxcsrGet());
                    }
                    tox2SpuMxcsrSet(kTox2SpuMxcsrHost);
                    for (int i = 0; i < 4; i++) {
                        const uint32_t p = a.u32[i];
                        const uint32_t q = b.u32[i];
                        const uint32_t r = c.u32[i];
                        const uint32_t want[6] = {tox2SpuSfAdd(p, q),       tox2SpuSfSub(p, q),
                                                  tox2SpuSfMul(p, q),       tox2SpuSfMulAdd(p, q, r),
                                                  tox2SpuSfMulSub(p, q, r), tox2SpuSfNegMulSub(p, q, r)};
                        for (int op = 0; op < 6; op++) {
                            if ((got[op].u32[i] != want[op] || exact[op][i] != want[op]) && bad++ < 10) {
                                printf("exact path op %d lane %d (%08x, %08x, %08x): %08x and %08x, expected %08x\n",
                                       op, i, p, q, r, got[op].u32[i], exact[op][i], want[op]);
                            }
                        }
                    }
                    for (int op = 0; op < 6; op++) forced[op] += hit[op];
                }
            }
        }
    }
    tox2SpuMxcsrSet(saved);
    for (int op = 0; op < 6; op++) {
        if (forced[op] == 0) {
            printf("no input reached the exact path of op %d\n", op);
            bad++;
        }
    }
    printf("exact paths: %ld/%ld/%ld/%ld/%ld/%ld vectors forced, %ld mismatches\n", forced[0], forced[1], forced[2],
           forced[3], forced[4], forced[5], bad);
    return bad == 0;
}

// Every combination of the edge words, twice: spread over the four lanes of the vectors, and with the
// same combination in all lanes, so that the fast path sees an edge without any other lane to hide it.
static int checkEdges(void) {
    g_bad = 0;
    int n = 0;
    int lane = 0;
    for (int x = 0; x < kEdgeCount; x++) {
        for (int y = 0; y < kEdgeCount; y++) {
            for (int z = 0; z < kEdgeCount; z++) {
                g_a[n] = tox2SpuSplatW(kEdges[x]);
                g_b[n] = tox2SpuSplatW(kEdges[y]);
                g_c[n] = tox2SpuSplatW(kEdges[z]);
                if (++n == kBatch - 1) {
                    checkBatch(n);
                    n = 0;
                }
                g_a[kBatch - 1].u32[lane] = kEdges[x];
                g_b[kBatch - 1].u32[lane] = kEdges[y];
                g_c[kBatch - 1].u32[lane] = kEdges[z];
                if (++lane == 4) {
                    g_a[n] = g_a[kBatch - 1];
                    g_b[n] = g_b[kBatch - 1];
                    g_c[n] = g_c[kBatch - 1];
                    lane = 0;
                    if (++n == kBatch - 1) {
                        checkBatch(n);
                        n = 0;
                    }
                }
            }
        }
    }
    if (n > 0) checkBatch(n);
    return g_bad == 0;
}

static uint64_t pickDouble(void) {
    const uint64_t bits = ((uint64_t)next() << 32) | next();
    const uint64_t rest = bits & 0x800fffffffffffffull;
    switch (next() & 3u) {
        case 0: return rest | ((uint64_t)(0x380 + next() % 0x1f) << 52);  // the single denormals and Smin
        case 1: return rest | ((uint64_t)(0x1e0 + next() % 0x40) << 52);  // products reach double denormals
        case 2: return rest | ((uint64_t)(0x3f0 + next() % 0x20) << 52);
        default: return bits;
    }
}

// The helpers that need round to nearest or denormals give the same words in the SPU environment as
// in the default one, and leave the environment as they found it.
static int checkRoundedHelpers(void) {
    enum { kHelpers = 14 };
    long bad = 0;
    for (long it = 0; it < 200000; it++) {
        Tox2Vr a;
        Tox2Vr b;
        Tox2Vr c;
        Tox2Vr w;
        for (int i = 0; i < 2; i++) {
            a.u64[i] = pickDouble();
            b.u64[i] = pickDouble();
            c.u64[i] = pickDouble();
        }
        for (int i = 0; i < 4; i++) w.u32[i] = pickWord();
        Tox2Vr got[kHelpers];
        Tox2Vr want[kHelpers];
        for (int env = 0; env < 2; env++) {
            const uint32_t mxcsr = env == 0 ? kTox2SpuMxcsrHost : kTox2SpuMxcsrSpu;
            Tox2Vr* r = env == 0 ? want : got;
            tox2SpuMxcsrSet(mxcsr);
            r[0] = tox2SpuDfa(a, b);
            r[1] = tox2SpuDfs(a, b);
            r[2] = tox2SpuDfm(a, b);
            r[3] = tox2SpuDfma(a, b, c);
            r[4] = tox2SpuDfms(a, b, c);
            r[5] = tox2SpuDfnms(a, b, c);
            r[6] = tox2SpuDfnma(a, b, c);
            r[7] = tox2SpuFrds(a);
            r[8] = tox2SpuFrest(w);
            r[9] = tox2SpuFrsqest(w);
            r[10] = tox2SpuCsflt(w, 3);
            r[11] = tox2SpuCuflt(w, 127);
            r[12] = tox2SpuCflts(w, 20);
            r[13] = tox2SpuCfltu(w, 20);
            if ((tox2SpuMxcsrGet() & kControlBits) != mxcsr && bad++ < 10) {
                printf("a helper changed the MXCSR to %08x\n", (unsigned)tox2SpuMxcsrGet());
            }
        }
        tox2SpuMxcsrSet(kTox2SpuMxcsrHost);
        for (int k = 0; k < kHelpers; k++) {
            if ((got[k].u64[0] != want[k].u64[0] || got[k].u64[1] != want[k].u64[1]) && bad++ < 10) {
                printf("helper %d differs between the environments: %016llx %016llx, expected %016llx %016llx\n", k,
                       (unsigned long long)got[k].u64[0], (unsigned long long)got[k].u64[1],
                       (unsigned long long)want[k].u64[0], (unsigned long long)want[k].u64[1]);
            }
        }
    }
    return bad == 0;
}

// Byte k of RA || RB (architected numbering) for each control byte, with the three constant forms.
static Tox2Vr shufbReference(Tox2Vr a, Tox2Vr b, Tox2Vr c) {
    uint8_t concat[32];
    for (int k = 0; k < 16; k++) {
        concat[k] = a.u8[15 - k];
        concat[16 + k] = b.u8[15 - k];
    }
    Tox2Vr r;
    for (int lane = 0; lane < 16; lane++) {
        const uint8_t sel = c.u8[lane];
        uint8_t v = concat[sel & 0x1fu];
        if ((sel & 0xc0u) == 0x80u) {
            v = 0x00;
        } else if ((sel & 0xe0u) == 0xc0u) {
            v = 0xff;
        } else if ((sel & 0xe0u) == 0xe0u) {
            v = 0x80;
        }
        r.u8[lane] = v;
    }
    return r;
}

static int checkShufb(void) {
    long bad = 0;
    for (long it = 0; it < 200000; it++) {
        Tox2Vr a;
        Tox2Vr b;
        Tox2Vr c;
        for (int i = 0; i < 16; i++) {
            a.u8[i] = (uint8_t)next();
            b.u8[i] = (uint8_t)next();
            c.u8[i] = (uint8_t)(it < 256 ? it + i : next());  // every control byte in every lane first
        }
        const Tox2Vr got = tox2SpuShufb(a, b, c);
        const Tox2Vr want = shufbReference(a, b, c);
        for (int i = 0; i < 16; i++) {
            if (got.u8[i] != want.u8[i] && bad++ < 10) {
                printf("shufb lane %d control %02x: %02x, expected %02x\n", i, c.u8[i], got.u8[i], want.u8[i]);
            }
        }
    }
    return bad == 0;
}

// The previous definition of the insertion mask: a byte store at a variable index per element byte.
static Tox2Vr genControlReference(uint32_t addr, uint32_t size) {
    uint32_t t = addr & (16u - size);
    uint32_t first = size < 8u ? 4u - size : 0u;
    Tox2Vr r;
    for (uint32_t j = 0; j < 16; j++) TOX2_VR_B(r, j) = (uint8_t)(0x10u + j);
    for (uint32_t i = 0; i < size; i++) TOX2_VR_B(r, t + i) = (uint8_t)(first + i);
    return r;
}

// Every size and every address class: 0..31 and the top 32 addresses cover each `addr & 15` with and without high bits.
static int checkGenControl(void) {
    long cases = 0;
    long bad = 0;
    for (uint32_t size = 1; size <= 8; size <<= 1) {
        for (uint32_t k = 0; k < 64; k++) {
            const uint32_t addr = k < 32 ? k : 0xffffffe0u + (k - 32);
            const Tox2Vr got = tox2SpuGenControl(addr, size);
            const Tox2Vr want = genControlReference(addr, size);
            cases++;
            for (int i = 0; i < 16; i++) {
                if (got.u8[i] != want.u8[i] && bad++ < 10) {
                    printf("genControl addr %08x size %u lane %d: %02x, expected %02x\n", (unsigned)addr,
                           (unsigned)size, i, got.u8[i], want.u8[i]);
                }
            }
        }
    }
    printf("genControl: %ld cases (4 sizes x 64 addresses), %ld mismatches\n", cases, bad);
    return bad == 0;
}

// The previous definitions of the quadword shifts and rotates, through unsigned __int128, kept as the
// reference for the pshufb and u64 lane forms.
__extension__ typedef unsigned __int128 RefU128;

static RefU128 refToInt(Tox2Vr v) {
    RefU128 x;
    __builtin_memcpy(&x, &v, sizeof x);
    return x;
}

static Tox2Vr refFromInt(RefU128 x) {
    Tox2Vr v;
    __builtin_memcpy(&v, &x, sizeof v);
    return v;
}

static RefU128 refRotl128(RefU128 x, uint32_t n) { return (x << n) | (x >> ((128u - n) & 127u)); }

static Tox2Vr refShlqbi(Tox2Vr a, uint32_t count) { return refFromInt(refToInt(a) << (count & 7u)); }

static Tox2Vr refShlqby(Tox2Vr a, uint32_t count) {
    uint32_t s = count & 0x1fu;
    return refFromInt(s < 16u ? refToInt(a) << (8u * s) : (RefU128)0);
}

static Tox2Vr refShlqbybi(Tox2Vr a, uint32_t count) { return refShlqby(a, count >> 3); }

static Tox2Vr refRotqbi(Tox2Vr a, uint32_t count) { return refFromInt(refRotl128(refToInt(a), count & 7u)); }

static Tox2Vr refRotqby(Tox2Vr a, uint32_t count) { return refFromInt(refRotl128(refToInt(a), 8u * (count & 15u))); }

static Tox2Vr refRotqbybi(Tox2Vr a, uint32_t count) { return refRotqby(a, count >> 3); }

static Tox2Vr refRotqmbi(Tox2Vr a, uint32_t count) { return refFromInt(refToInt(a) >> ((0u - count) & 7u)); }

static Tox2Vr refRotqmby(Tox2Vr a, uint32_t count) {
    uint32_t s = (0u - count) & 0x1fu;
    return refFromInt(s < 16u ? refToInt(a) >> (8u * s) : (RefU128)0);
}

static Tox2Vr refRotqmbybi(Tox2Vr a, uint32_t count) { return refRotqmby(a, count >> 3); }

enum { kQuadwordHelpers = 9 };

static long g_qw_bad;
static long g_qw_cases;

// All nine helpers on one vector and count. The count is read through a volatile so that the helpers
// are compiled with a variable count, as in the register forms; the constant counts below cover the
// immediate forms.
static void checkQuadword(Tox2Vr v, uint32_t count) {
    const Tox2Vr got[kQuadwordHelpers] = {
            tox2SpuShlqbi(v, count),  tox2SpuShlqby(v, count),  tox2SpuShlqbybi(v, count),
            tox2SpuRotqbi(v, count),  tox2SpuRotqby(v, count),  tox2SpuRotqbybi(v, count),
            tox2SpuRotqmbi(v, count), tox2SpuRotqmby(v, count), tox2SpuRotqmbybi(v, count)};
    const Tox2Vr want[kQuadwordHelpers] = {refShlqbi(v, count),  refShlqby(v, count),  refShlqbybi(v, count),
                                           refRotqbi(v, count),  refRotqby(v, count),  refRotqbybi(v, count),
                                           refRotqmbi(v, count), refRotqmby(v, count), refRotqmbybi(v, count)};
    for (int k = 0; k < kQuadwordHelpers; k++) {
        g_qw_cases++;
        if ((got[k].u64[0] != want[k].u64[0] || got[k].u64[1] != want[k].u64[1]) && g_qw_bad++ < 10) {
            printf("quadword helper %d count %08x: %016llx %016llx, expected %016llx %016llx\n", k, (unsigned)count,
                   (unsigned long long)got[k].u64[1], (unsigned long long)got[k].u64[0],
                   (unsigned long long)want[k].u64[1], (unsigned long long)want[k].u64[0]);
        }
    }
}

#define CHECK_QUADWORD_CONSTANT(v, c) checkQuadword(v, (uint32_t)(c))

// Every count value the helpers read: the byte forms read bits 0 to 4 of the count, the bybi forms read
// bits 3 to 7, and the mask forms negate first, so the full range of the low 10 bits and of the top 10
// bits (the negation of a small count) is walked, then single-bit counts and random words.
static int checkQuadwords(void) {
    g_qw_bad = 0;
    g_qw_cases = 0;
    for (int t = 0; t < 64; t++) {
        Tox2Vr v;
        for (int i = 0; i < 4; i++) v.u32[i] = next();
        if (t == 0) v.u32[0] = v.u32[1] = v.u32[2] = v.u32[3] = 0xffffffffu;
        if (t == 1) {
            v.u32[0] = v.u32[1] = v.u32[2] = v.u32[3] = 0;
            v.u8[0] = 1;
            v.u8[15] = 0x80;
        }
        volatile uint32_t count;
        for (uint32_t c = 0; c < 1024; c++) {
            count = c;
            checkQuadword(v, count);
        }
        for (uint32_t c = 0xfffffc00u; c != 0; c++) {
            count = c;
            checkQuadword(v, count);
        }
        for (int bit = 0; bit < 32; bit++) {
            count = 1u << bit;
            checkQuadword(v, count);
            count = ~(1u << bit);
            checkQuadword(v, count);
        }
        for (int k = 0; k < 4096; k++) {
            count = next();
            checkQuadword(v, count);
        }
        // Constant counts, as the generated code writes for the immediate forms.
        CHECK_QUADWORD_CONSTANT(v, 0);
        CHECK_QUADWORD_CONSTANT(v, 1);
        CHECK_QUADWORD_CONSTANT(v, 2);
        CHECK_QUADWORD_CONSTANT(v, 3);
        CHECK_QUADWORD_CONSTANT(v, 4);
        CHECK_QUADWORD_CONSTANT(v, 5);
        CHECK_QUADWORD_CONSTANT(v, 7);
        CHECK_QUADWORD_CONSTANT(v, 8);
        CHECK_QUADWORD_CONSTANT(v, 9);
        CHECK_QUADWORD_CONSTANT(v, 12);
        CHECK_QUADWORD_CONSTANT(v, 15);
        CHECK_QUADWORD_CONSTANT(v, 16);
        CHECK_QUADWORD_CONSTANT(v, 17);
        CHECK_QUADWORD_CONSTANT(v, 24);
        CHECK_QUADWORD_CONSTANT(v, 31);
        CHECK_QUADWORD_CONSTANT(v, 32);
        CHECK_QUADWORD_CONSTANT(v, 33);
        CHECK_QUADWORD_CONSTANT(v, 0x18);
        CHECK_QUADWORD_CONSTANT(v, 0x78);
        CHECK_QUADWORD_CONSTANT(v, 0x80);
        CHECK_QUADWORD_CONSTANT(v, 0xffffffffu);
        CHECK_QUADWORD_CONSTANT(v, 0xfffffffeu);
        CHECK_QUADWORD_CONSTANT(v, 0xfffffff9u);
        CHECK_QUADWORD_CONSTANT(v, 0xfffffff8u);
        CHECK_QUADWORD_CONSTANT(v, 0xfffffff0u);
        CHECK_QUADWORD_CONSTANT(v, 0xffffffe1u);
        CHECK_QUADWORD_CONSTANT(v, 0xffffffe0u);
        CHECK_QUADWORD_CONSTANT(v, 0xffffffc0u);
        CHECK_QUADWORD_CONSTANT(v, 0xffffff88u);
    }
    printf("quadword shifts: %ld results (9 helpers, 64 vectors), %ld mismatches\n", g_qw_cases, g_qw_bad);
    return g_qw_bad == 0;
}

int main(void) {
    const int ok = checkFloat() & checkEdges() & checkExactPaths() & checkRoundedHelpers() & checkShufb() &
                   checkGenControl() & checkQuadwords();
    if (ok) printf("spu vector helpers ok\n");
    return ok ? 0 : 1;
}
