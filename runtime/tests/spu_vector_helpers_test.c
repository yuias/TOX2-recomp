// The SIMD forms of the SPU helpers against their lane-by-lane definitions: the single-precision
// arithmetic of spu_fp.h against the scalar tox2SpuSf* functions, and shufb against a byte loop that
// follows the ISA text. Random words weighted toward the cases that differ (zero and denormal exponents,
// exponent 255, results near Smin and Smax, cancellation).

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

static const uint32_t kEdges[] = {0x00000000u, 0x80000000u, 0x00000001u, 0x00800000u, 0x80800000u, 0x007fffffu,
                                  0x7fffffffu, 0xffffffffu, 0x7f800000u, 0xff800000u, 0x3f800000u, 0xbf800000u,
                                  0x00800001u, 0x01000000u, 0x7f7fffffu, 0x34000000u, 0x33800000u, 0x4b800000u};

static uint32_t pickWord(void) {
    switch (next() & 7u) {
        case 0: return kEdges[next() % (sizeof kEdges / sizeof kEdges[0])];
        case 1: return (next() & 0x807fffffu) | ((uint32_t)(0x7c + next() % 7u) << 23);  // near 1.0
        case 2: return (next() & 0x80ffffffu) | 0x00800000u;                             // near Smin
        case 3: return next() | 0x7f000000u;                                             // near Smax
        default: return next();
    }
}

static int checkFloat(void) {
    long bad = 0;
    for (long it = 0; it < 2000000; it++) {
        Tox2Vr a;
        Tox2Vr b;
        Tox2Vr c;
        for (int i = 0; i < 4; i++) {
            a.u32[i] = pickWord();
            b.u32[i] = (it & 3) == 0 ? (a.u32[i] ^ 0x80000000u) + next() % 5u - 2u : pickWord();
            c.u32[i] = pickWord();
        }
        const Tox2Vr got[6] = {tox2SpuFa(a, b),     tox2SpuFs(a, b),     tox2SpuFm(a, b),
                               tox2SpuFma(a, b, c), tox2SpuFms(a, b, c), tox2SpuFnms(a, b, c)};
        for (int i = 0; i < 4; i++) {
            const uint32_t x = a.u32[i];
            const uint32_t y = b.u32[i];
            const uint32_t z = c.u32[i];
            const uint32_t want[6] = {tox2SpuSfAdd(x, y),       tox2SpuSfSub(x, y),       tox2SpuSfMul(x, y),
                                      tox2SpuSfMulAdd(x, y, z), tox2SpuSfMulSub(x, y, z), tox2SpuSfNegMulSub(x, y, z)};
            for (int op = 0; op < 6; op++) {
                if (got[op].u32[i] != want[op] && bad++ < 10) {
                    printf("float op %d (%08x, %08x, %08x): %08x, expected %08x\n", op, x, y, z, got[op].u32[i],
                           want[op]);
                }
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

int main(void) {
    const int ok = checkFloat() & checkShufb();
    if (ok) printf("spu vector helpers ok\n");
    return ok ? 0 : 1;
}
