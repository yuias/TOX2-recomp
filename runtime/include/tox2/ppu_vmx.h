#pragma once

#include "ppu_context.h"
#include "ppu_int.h"

// Vector helpers for generated code. Register layout: see "Vector registers" in ppu_context.h
// (host lane i = big-endian element n-1-i, so the 128-bit value of a register has big-endian byte 0
// in its most significant byte).

// Partial vector loads and stores. `sh` below is ea & 15.

TOX2_FORCEINLINE Tox2U128 tox2VrBits(Tox2Vr v) {
    Tox2U128 bits;
    __builtin_memcpy(&bits, &v, sizeof bits);
    return bits;
}

TOX2_FORCEINLINE Tox2Vr tox2VrFromBits(Tox2U128 bits) {
    Tox2Vr v;
    __builtin_memcpy(&v, &bits, sizeof v);
    return v;
}

// lvlx/lvlxl: the bytes from ea to the end of its 16-byte block go to the left (big-endian low
// numbers) of the register, the rest is zero (RPCS3 `LVLX`).
TOX2_FORCEINLINE Tox2Vr tox2Lvlx(const uint8_t* base, uint32_t ea) {
    return tox2VrFromBits(tox2VrBits(tox2ReadVr(base, ea)) << ((ea & 15u) * 8u));
}

// lvrx/lvrxl: the bytes from the block start up to (not including) ea go to the right of the
// register, the rest is zero; an aligned ea loads nothing and does not touch memory (RPCS3 `LVRX`).
TOX2_FORCEINLINE Tox2Vr tox2Lvrx(const uint8_t* base, uint32_t ea) {
    uint32_t sh = ea & 15u;
    if (sh == 0) return tox2VrFromBits(0);
    return tox2VrFromBits(tox2VrBits(tox2ReadVr(base, ea)) >> ((16u - sh) * 8u));
}

// stvlx/stvlxl: big-endian bytes 0 .. 15-sh go to ea .. (RPCS3 `STVLX`).
TOX2_FORCEINLINE void tox2Stvlx(uint8_t* base, uint32_t ea, Tox2Vr v) {
    uint8_t* p = base + ea;
    for (uint32_t j = 0; j < 16u - (ea & 15u); j++) p[j] = TOX2_VR_B(v, j);
}

// stvrx/stvrxl: big-endian bytes 16-sh .. 15 go to the start of the block; nothing when sh == 0
// (RPCS3 `STVRX`).
TOX2_FORCEINLINE void tox2Stvrx(uint8_t* base, uint32_t ea, Tox2Vr v) {
    uint32_t sh = ea & 15u;
    uint8_t* p = base + (ea & ~15u);
    for (uint32_t k = 0; k < sh; k++) p[k] = TOX2_VR_B(v, 16u - sh + k);
}

// Element stores write the element at its natural position in the register, at the element-aligned
// address (RPCS3 `STVEBX`, `STVEHX`, `STVEWX`). The element loads need no helper: RPCS3 `LVEBX`,
// `LVEHX` and `LVEWX` load the whole aligned block like `LVX`, which leaves the other elements
// undefined by the architecture.
TOX2_FORCEINLINE void tox2Stvebx(uint8_t* base, uint32_t ea, Tox2Vr v) { tox2Write8(base, ea, TOX2_VR_B(v, ea & 15u)); }

TOX2_FORCEINLINE void tox2Stvehx(uint8_t* base, uint32_t ea, Tox2Vr v) {
    ea &= ~1u;
    tox2Write16(base, ea, TOX2_VR_H(v, (ea & 15u) >> 1));
}

TOX2_FORCEINLINE void tox2Stvewx(uint8_t* base, uint32_t ea, Tox2Vr v) {
    ea &= ~3u;
    tox2Write32(base, ea, TOX2_VR_W(v, (ea & 15u) >> 2));
}

// lvsl/lvsr: permute control vectors; big-endian byte j is sh + j (lvsl) or 16 - sh + j (lvsr)
// (RPCS3 `LVSL`, `LVSR`).
TOX2_FORCEINLINE Tox2Vr tox2Lvsl(uint32_t ea) {
    Tox2U8x16 lanes = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    Tox2Vr v;
    v.u8 = lanes + (uint8_t)(ea & 15u);
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Lvsr(uint32_t ea) {
    Tox2U8x16 lanes = {31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16};
    Tox2Vr v;
    v.u8 = lanes - (uint8_t)(ea & 15u);
    return v;
}

// vsel: bits of b where c is set, bits of a elsewhere (RPCS3 `VSEL`).
TOX2_FORCEINLINE Tox2Vr tox2Vsel(Tox2Vr a, Tox2Vr b, Tox2Vr c) {
    Tox2Vr v;
    v.u32 = (b.u32 & c.u32) | (a.u32 & ~c.u32);
    return v;
}

// Permute, splat, pack, unpack and whole-register shift helpers. Merges, vsldoi and the splats are
// emitted as __builtin_shufflevector calls with constant host lane indices (see emit_ops_vmx_perm.py).

typedef int8_t Tox2S8x8 __attribute__((vector_size(8)));
typedef uint8_t Tox2U8x8 __attribute__((vector_size(8)));
typedef int16_t Tox2S16x4 __attribute__((vector_size(8)));
typedef uint16_t Tox2U16x4 __attribute__((vector_size(8)));

// SAT is sticky: it is only ever set here, never cleared (mtvscr clears it).
TOX2_FORCEINLINE void tox2SetSat(Tox2PpuContext* ctx, int clamped) { ctx->vscr_sat |= (uint8_t)(clamped != 0); }

// vperm: big-endian byte i of the result is concat(a, b)[c[i] & 31] (RPCS3 `VPERM`). In the host
// layout that is b || a indexed by ~c & 31.
TOX2_FORCEINLINE Tox2Vr tox2Vperm(Tox2Vr a, Tox2Vr b, Tox2Vr c) {
    Tox2U8x16 idx = ~c.u8 & 0x1f;
    Tox2Vr v;
#if defined(__SSSE3__)
    // pshufb reads the low four index bits; indices above 15 take a, the rest take b.
    Tox2U8x16 from_a = (Tox2U8x16)(idx > 15);
    Tox2U8x16 pa = (Tox2U8x16)__builtin_ia32_pshufb128((Tox2S8x16)a.u8, (Tox2S8x16)idx);
    Tox2U8x16 pb = (Tox2U8x16)__builtin_ia32_pshufb128((Tox2S8x16)b.u8, (Tox2S8x16)idx);
    v.u8 = (pa & from_a) | (pb & ~from_a);
#else
    uint8_t ab[32];
    __builtin_memcpy(ab, &b, 16);
    __builtin_memcpy(ab + 16, &a, 16);
    for (uint32_t i = 0; i < 16u; i++) v.u8[i] = ab[idx[i]];
#endif
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vspltisb(int8_t imm) {
    Tox2S8x16 zero = {};
    Tox2Vr v;
    v.s8 = zero + imm;
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vspltish(int16_t imm) {
    Tox2S16x8 zero = {};
    Tox2Vr v;
    v.s16 = zero + imm;
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vspltisw(int32_t imm) {
    Tox2S32x4 zero = {};
    Tox2Vr v;
    v.s32 = zero + imm;
    return v;
}

// vsl/vsr/vslo/vsro shift the whole 128-bit register. vsl/vsr use the low three bits of vB's
// big-endian byte 15 for the bit count; the architecture leaves the result undefined when the
// other bytes differ, where RPCS3 `VSL`/`VSR` shift each byte by its own count. vslo/vsro shift by
// whole octets, count = byte 15 bits 121:124 (RPCS3 `VSLO`: `b._u8[0] & 0x78`, host byte 0).
TOX2_FORCEINLINE Tox2Vr tox2Vsl(Tox2Vr a, Tox2Vr b) { return tox2VrFromBits(tox2VrBits(a) << (b.u8[0] & 7u)); }

TOX2_FORCEINLINE Tox2Vr tox2Vsr(Tox2Vr a, Tox2Vr b) { return tox2VrFromBits(tox2VrBits(a) >> (b.u8[0] & 7u)); }

TOX2_FORCEINLINE Tox2Vr tox2Vslo(Tox2Vr a, Tox2Vr b) { return tox2VrFromBits(tox2VrBits(a) << (b.u8[0] & 0x78u)); }

TOX2_FORCEINLINE Tox2Vr tox2Vsro(Tox2Vr a, Tox2Vr b) { return tox2VrFromBits(tox2VrBits(a) >> (b.u8[0] & 0x78u)); }

// Packs: the result holds a's elements in big-endian elements 0 .. n/2-1 and b's in the rest, which
// is `b` then `a` in host lane order. The saturating packs set SAT when any element is clamped
// (RPCS3 `VPKSHSS` etc.). Signed-to-unsigned packs clamp negative values to 0 and also set SAT.
TOX2_FORCEINLINE Tox2Vr tox2Vpkshss(Tox2PpuContext* ctx, Tox2Vr a, Tox2Vr b) {
    Tox2S16x8 zero = {};
    Tox2S16x8 lo = zero - 128, hi = zero + 127;
    Tox2S16x8 ca = __builtin_elementwise_min(__builtin_elementwise_max(a.s16, lo), hi);
    Tox2S16x8 cb = __builtin_elementwise_min(__builtin_elementwise_max(b.s16, lo), hi);
    tox2SetSat(ctx, __builtin_reduce_or((ca != a.s16) | (cb != b.s16)));
    Tox2S8x8 na = __builtin_convertvector(ca, Tox2S8x8);
    Tox2S8x8 nb = __builtin_convertvector(cb, Tox2S8x8);
    Tox2Vr v;
    v.s8 = __builtin_shufflevector(nb, na, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vpkshus(Tox2PpuContext* ctx, Tox2Vr a, Tox2Vr b) {
    Tox2S16x8 zero = {};
    Tox2S16x8 hi = zero + 255;
    Tox2S16x8 ca = __builtin_elementwise_min(__builtin_elementwise_max(a.s16, zero), hi);
    Tox2S16x8 cb = __builtin_elementwise_min(__builtin_elementwise_max(b.s16, zero), hi);
    tox2SetSat(ctx, __builtin_reduce_or((ca != a.s16) | (cb != b.s16)));
    Tox2U8x8 na = __builtin_convertvector(ca, Tox2U8x8);
    Tox2U8x8 nb = __builtin_convertvector(cb, Tox2U8x8);
    Tox2Vr v;
    v.u8 = __builtin_shufflevector(nb, na, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vpkuhus(Tox2PpuContext* ctx, Tox2Vr a, Tox2Vr b) {
    Tox2U16x8 zero = {};
    Tox2U16x8 hi = zero + 255;
    Tox2U16x8 ca = __builtin_elementwise_min(a.u16, hi);
    Tox2U16x8 cb = __builtin_elementwise_min(b.u16, hi);
    tox2SetSat(ctx, __builtin_reduce_or((ca != a.u16) | (cb != b.u16)));
    Tox2U8x8 na = __builtin_convertvector(ca, Tox2U8x8);
    Tox2U8x8 nb = __builtin_convertvector(cb, Tox2U8x8);
    Tox2Vr v;
    v.u8 = __builtin_shufflevector(nb, na, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vpkswss(Tox2PpuContext* ctx, Tox2Vr a, Tox2Vr b) {
    Tox2S32x4 zero = {};
    Tox2S32x4 lo = zero - 32768, hi = zero + 32767;
    Tox2S32x4 ca = __builtin_elementwise_min(__builtin_elementwise_max(a.s32, lo), hi);
    Tox2S32x4 cb = __builtin_elementwise_min(__builtin_elementwise_max(b.s32, lo), hi);
    tox2SetSat(ctx, __builtin_reduce_or((ca != a.s32) | (cb != b.s32)));
    Tox2S16x4 na = __builtin_convertvector(ca, Tox2S16x4);
    Tox2S16x4 nb = __builtin_convertvector(cb, Tox2S16x4);
    Tox2Vr v;
    v.s16 = __builtin_shufflevector(nb, na, 0, 1, 2, 3, 4, 5, 6, 7);
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vpkswus(Tox2PpuContext* ctx, Tox2Vr a, Tox2Vr b) {
    Tox2S32x4 zero = {};
    Tox2S32x4 hi = zero + 65535;
    Tox2S32x4 ca = __builtin_elementwise_min(__builtin_elementwise_max(a.s32, zero), hi);
    Tox2S32x4 cb = __builtin_elementwise_min(__builtin_elementwise_max(b.s32, zero), hi);
    tox2SetSat(ctx, __builtin_reduce_or((ca != a.s32) | (cb != b.s32)));
    Tox2U16x4 na = __builtin_convertvector(ca, Tox2U16x4);
    Tox2U16x4 nb = __builtin_convertvector(cb, Tox2U16x4);
    Tox2Vr v;
    v.u16 = __builtin_shufflevector(nb, na, 0, 1, 2, 3, 4, 5, 6, 7);
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vpkuwus(Tox2PpuContext* ctx, Tox2Vr a, Tox2Vr b) {
    Tox2U32x4 zero = {};
    Tox2U32x4 hi = zero + 65535u;
    Tox2U32x4 ca = __builtin_elementwise_min(a.u32, hi);
    Tox2U32x4 cb = __builtin_elementwise_min(b.u32, hi);
    tox2SetSat(ctx, __builtin_reduce_or((ca != a.u32) | (cb != b.u32)));
    Tox2U16x4 na = __builtin_convertvector(ca, Tox2U16x4);
    Tox2U16x4 nb = __builtin_convertvector(cb, Tox2U16x4);
    Tox2Vr v;
    v.u16 = __builtin_shufflevector(nb, na, 0, 1, 2, 3, 4, 5, 6, 7);
    return v;
}

// vpkpx: each 32-bit pixel a:8 r:8 g:8 b:8 becomes 1:5:5:5 from the low bit of a and the top five
// bits of r, g and b (RPCS3 `VPKPX`).
TOX2_FORCEINLINE Tox2U16x4 tox2PackPixels(Tox2U32x4 w) {
    Tox2U32x4 h = ((w >> 9) & 0xfc00u) | ((w >> 6) & 0x3e0u) | ((w >> 3) & 0x1fu);
    return __builtin_convertvector(h, Tox2U16x4);
}

TOX2_FORCEINLINE Tox2Vr tox2Vpkpx(Tox2Vr a, Tox2Vr b) {
    Tox2Vr v;
    v.u16 = __builtin_shufflevector(tox2PackPixels(b.u32), tox2PackPixels(a.u32), 0, 1, 2, 3, 4, 5, 6, 7);
    return v;
}

// Unpacks sign-extend half of the elements; `h` (high) is big-endian elements 0 .. n/2-1, which are
// the upper host lanes. vupk*px widen 1:5:5:5 pixels to a:8 r:8 g:8 b:8, the 1-bit field becoming
// 0x00 or 0xff (RPCS3 `VUPKHPX`).
TOX2_FORCEINLINE Tox2Vr tox2Vupkhsb(Tox2Vr b) {
    Tox2Vr v;
    v.s16 = __builtin_convertvector(__builtin_shufflevector(b.s8, b.s8, 8, 9, 10, 11, 12, 13, 14, 15), Tox2S16x8);
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vupklsb(Tox2Vr b) {
    Tox2Vr v;
    v.s16 = __builtin_convertvector(__builtin_shufflevector(b.s8, b.s8, 0, 1, 2, 3, 4, 5, 6, 7), Tox2S16x8);
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vupkhsh(Tox2Vr b) {
    Tox2Vr v;
    v.s32 = __builtin_convertvector(__builtin_shufflevector(b.s16, b.s16, 4, 5, 6, 7), Tox2S32x4);
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vupklsh(Tox2Vr b) {
    Tox2Vr v;
    v.s32 = __builtin_convertvector(__builtin_shufflevector(b.s16, b.s16, 0, 1, 2, 3), Tox2S32x4);
    return v;
}

TOX2_FORCEINLINE Tox2U32x4 tox2UnpackPixels(Tox2S32x4 x) {
    Tox2U32x4 u = (Tox2U32x4)x;
    return ((u << 6) & 0x1f0000u) | ((u << 3) & 0x1f00u) | (u & 0xff00001fu);
}

TOX2_FORCEINLINE Tox2Vr tox2Vupkhpx(Tox2Vr b) {
    Tox2Vr v;
    v.u32 = tox2UnpackPixels(__builtin_convertvector(__builtin_shufflevector(b.s16, b.s16, 4, 5, 6, 7), Tox2S32x4));
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vupklpx(Tox2Vr b) {
    Tox2Vr v;
    v.u32 = tox2UnpackPixels(__builtin_convertvector(__builtin_shufflevector(b.s16, b.s16, 0, 1, 2, 3), Tox2S32x4));
    return v;
}

// VSCR: NJ is bit 16 and SAT bit 0 of the register's big-endian word 3 (AltiVec PEM). RPCS3's
// interpreter indexes `_u32[3]`, which is big-endian word 0 in its byte-reversed register layout,
// while its LLVM translator uses element 0 of the same byte-reversed layout, which is the PEM word;
// the PEM position is kept here because guest code is built for hardware.
TOX2_FORCEINLINE Tox2Vr tox2Mfvscr(const Tox2PpuContext* ctx) {
    return tox2VrFromBeWords(0, 0, 0, ((uint32_t)(ctx->vscr_nj & 1) << 16) | (ctx->vscr_sat & 1));
}

TOX2_FORCEINLINE void tox2Mtvscr(Tox2PpuContext* ctx, Tox2Vr v) {
    uint32_t vscr = tox2VrBeWord(v, 3);
    ctx->vscr_nj = (vscr >> 16) & 1;
    ctx->vscr_sat = vscr & 1;
}

// Integer add/subtract with saturation. SAT is set when any lane clamps, i.e. when the clamped result
// differs from the wrapped one (RPCS3 `VADDSBS` etc. compare the same two values). The wrapped sum
// is formed on the unsigned view because signed vector overflow is not defined.
#define TOX2_VMX_SATURATE(fn, builtin, op, view, uview)                        \
    TOX2_FORCEINLINE Tox2Vr fn(Tox2PpuContext* ctx, Tox2Vr a, Tox2Vr b) {      \
        Tox2Vr v;                                                              \
        v.view = builtin(a.view, b.view);                                      \
        tox2SetSat(ctx, __builtin_reduce_or(v.uview != (a.uview op b.uview))); \
        return v;                                                              \
    }

TOX2_VMX_SATURATE(tox2Vaddubs, __builtin_elementwise_add_sat, +, u8, u8)
TOX2_VMX_SATURATE(tox2Vadduhs, __builtin_elementwise_add_sat, +, u16, u16)
TOX2_VMX_SATURATE(tox2Vadduws, __builtin_elementwise_add_sat, +, u32, u32)
TOX2_VMX_SATURATE(tox2Vaddsbs, __builtin_elementwise_add_sat, +, s8, u8)
TOX2_VMX_SATURATE(tox2Vaddshs, __builtin_elementwise_add_sat, +, s16, u16)
TOX2_VMX_SATURATE(tox2Vaddsws, __builtin_elementwise_add_sat, +, s32, u32)
TOX2_VMX_SATURATE(tox2Vsububs, __builtin_elementwise_sub_sat, -, u8, u8)
TOX2_VMX_SATURATE(tox2Vsubuhs, __builtin_elementwise_sub_sat, -, u16, u16)
TOX2_VMX_SATURATE(tox2Vsubuws, __builtin_elementwise_sub_sat, -, u32, u32)
TOX2_VMX_SATURATE(tox2Vsubsbs, __builtin_elementwise_sub_sat, -, s8, u8)
TOX2_VMX_SATURATE(tox2Vsubshs, __builtin_elementwise_sub_sat, -, s16, u16)
TOX2_VMX_SATURATE(tox2Vsubsws, __builtin_elementwise_sub_sat, -, s32, u32)
#undef TOX2_VMX_SATURATE

// vaddcuw: 1 where the unsigned sum carries out of 32 bits (b > ~a); vsubcuw: 1 where a - b does not
// borrow, i.e. a >= b (RPCS3 `VADDCUW`, `VSUBCUW`).
TOX2_FORCEINLINE Tox2Vr tox2Vaddcuw(Tox2Vr a, Tox2Vr b) {
    Tox2Vr v;
    v.u32 = (Tox2U32x4)(b.u32 > ~a.u32) & 1u;
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vsubcuw(Tox2Vr a, Tox2Vr b) {
    Tox2Vr v;
    v.u32 = (Tox2U32x4)(a.u32 >= b.u32) & 1u;
    return v;
}

// Averages round up: (a + b + 1) >> 1 as (a | b) - ((a ^ b) >> 1), which cannot overflow. The shift
// is arithmetic for the signed forms (RPCS3 `VAVGSB` etc.).
#define TOX2_VMX_AVERAGE(fn, view, uview, utype)                         \
    TOX2_FORCEINLINE Tox2Vr fn(Tox2Vr a, Tox2Vr b) {                     \
        Tox2Vr v;                                                        \
        v.uview = (a.uview | b.uview) - (utype)((a.view ^ b.view) >> 1); \
        return v;                                                        \
    }

TOX2_VMX_AVERAGE(tox2Vavgub, u8, u8, Tox2U8x16)
TOX2_VMX_AVERAGE(tox2Vavguh, u16, u16, Tox2U16x8)
TOX2_VMX_AVERAGE(tox2Vavguw, u32, u32, Tox2U32x4)
TOX2_VMX_AVERAGE(tox2Vavgsb, s8, u8, Tox2U8x16)
TOX2_VMX_AVERAGE(tox2Vavgsh, s16, u16, Tox2U16x8)
TOX2_VMX_AVERAGE(tox2Vavgsw, s32, u32, Tox2U32x4)
#undef TOX2_VMX_AVERAGE

// vcmp*. with Rc=1: CR6 = {all lanes true, 0, no lane true, 0} (RPCS3 `ppu_cr_set(ppu, 6, ...)`).
TOX2_FORCEINLINE void tox2SetCr6(Tox2PpuContext* ctx, Tox2Vr result) {
    uint64_t all = result.u64[0] & result.u64[1];
    uint64_t any = result.u64[0] | result.u64[1];
    tox2SetCrField(ctx, 6, ((uint32_t)(all == ~0ull) << 3) | ((uint32_t)(any == 0) << 1));
}

// Per-element shifts and rotates (RPCS3 `VSLB`, `VSRAW`, `VRLH` etc.): each element of vB supplies
// its own count, taken modulo the element width, so no shift amount reaches the width. Left shifts
// run on the unsigned view because shifting a negative value left is not defined in C. The rotate
// uses (width - n) & (width - 1) so that n == 0 shifts by 0 instead of by the full width.
#define TOX2_VMX_SHIFTS(sfx, view, sview, utype, stype, mask)         \
    TOX2_FORCEINLINE Tox2Vr tox2Vrl##sfx(Tox2Vr a, Tox2Vr b) {        \
        Tox2Vr v;                                                     \
        utype n = b.view & mask;                                      \
        v.view = (a.view << n) | (a.view >> ((mask + 1 - n) & mask)); \
        return v;                                                     \
    }                                                                 \
    TOX2_FORCEINLINE Tox2Vr tox2Vsl##sfx(Tox2Vr a, Tox2Vr b) {        \
        Tox2Vr v;                                                     \
        v.view = a.view << (b.view & mask);                           \
        return v;                                                     \
    }                                                                 \
    TOX2_FORCEINLINE Tox2Vr tox2Vsr##sfx(Tox2Vr a, Tox2Vr b) {        \
        Tox2Vr v;                                                     \
        v.view = a.view >> (b.view & mask);                           \
        return v;                                                     \
    }                                                                 \
    TOX2_FORCEINLINE Tox2Vr tox2Vsra##sfx(Tox2Vr a, Tox2Vr b) {       \
        Tox2Vr v;                                                     \
        v.sview = a.sview >> (stype)(b.view & mask);                  \
        return v;                                                     \
    }

TOX2_VMX_SHIFTS(b, u8, s8, Tox2U8x16, Tox2S8x16, 7)
TOX2_VMX_SHIFTS(h, u16, s16, Tox2U16x8, Tox2S16x8, 15)
TOX2_VMX_SHIFTS(w, u32, s32, Tox2U32x4, Tox2S32x4, 31)
#undef TOX2_VMX_SHIFTS

// Widening multiplies. "Even" and "odd" count elements in big-endian order (element 0 is even), so
// even elements are the odd host lanes (RPCS3 `VMULEUB` reads the high byte of each host halfword).
// The products of 8-bit and 16-bit elements fit the result type, so no lane overflows.
TOX2_FORCEINLINE Tox2Vr tox2Vmuleub(Tox2Vr a, Tox2Vr b) {
    Tox2Vr v;
    v.u16 = (a.u16 >> 8) * (b.u16 >> 8);
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vmuloub(Tox2Vr a, Tox2Vr b) {
    Tox2Vr v;
    v.u16 = (a.u16 & 0xffu) * (b.u16 & 0xffu);
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vmulesb(Tox2Vr a, Tox2Vr b) {
    Tox2Vr v;
    v.s16 = (a.s16 >> 8) * (b.s16 >> 8);
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vmulosb(Tox2Vr a, Tox2Vr b) {
    Tox2Vr v;
    Tox2S16x8 ea = __builtin_convertvector(__builtin_shufflevector(a.s8, a.s8, 0, 2, 4, 6, 8, 10, 12, 14), Tox2S16x8);
    Tox2S16x8 eb = __builtin_convertvector(__builtin_shufflevector(b.s8, b.s8, 0, 2, 4, 6, 8, 10, 12, 14), Tox2S16x8);
    v.s16 = ea * eb;
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vmuleuh(Tox2Vr a, Tox2Vr b) {
    Tox2Vr v;
    v.u32 = __builtin_convertvector(__builtin_shufflevector(a.u16, a.u16, 1, 3, 5, 7), Tox2U32x4) *
            __builtin_convertvector(__builtin_shufflevector(b.u16, b.u16, 1, 3, 5, 7), Tox2U32x4);
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vmulouh(Tox2Vr a, Tox2Vr b) {
    Tox2Vr v;
    v.u32 = __builtin_convertvector(__builtin_shufflevector(a.u16, a.u16, 0, 2, 4, 6), Tox2U32x4) *
            __builtin_convertvector(__builtin_shufflevector(b.u16, b.u16, 0, 2, 4, 6), Tox2U32x4);
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vmulesh(Tox2Vr a, Tox2Vr b) {
    Tox2Vr v;
    v.s32 = __builtin_convertvector(__builtin_shufflevector(a.s16, a.s16, 1, 3, 5, 7), Tox2S32x4) *
            __builtin_convertvector(__builtin_shufflevector(b.s16, b.s16, 1, 3, 5, 7), Tox2S32x4);
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vmulosh(Tox2Vr a, Tox2Vr b) {
    Tox2Vr v;
    v.s32 = __builtin_convertvector(__builtin_shufflevector(a.s16, a.s16, 0, 2, 4, 6), Tox2S32x4) *
            __builtin_convertvector(__builtin_shufflevector(b.s16, b.s16, 0, 2, 4, 6), Tox2S32x4);
    return v;
}

// Clamp to the signed 16-bit, signed 32-bit or unsigned 32-bit range and set SAT when the value
// changed.
TOX2_FORCEINLINE int16_t tox2ClampS16(Tox2PpuContext* ctx, int32_t x) {
    int32_t r = x < -32768 ? -32768 : x > 32767 ? 32767 : x;
    tox2SetSat(ctx, r != x);
    return (int16_t)r;
}

TOX2_FORCEINLINE int32_t tox2ClampS32(Tox2PpuContext* ctx, int64_t x) {
    int64_t r = x < INT32_MIN ? INT32_MIN : x > INT32_MAX ? INT32_MAX : x;
    tox2SetSat(ctx, r != x);
    return (int32_t)r;
}

TOX2_FORCEINLINE uint32_t tox2ClampU32(Tox2PpuContext* ctx, uint64_t x) {
    uint64_t r = x > UINT32_MAX ? UINT32_MAX : x;
    tox2SetSat(ctx, r != x);
    return (uint32_t)r;
}

// vmhaddshs: (a * b >> 15) + c per halfword, saturated. The shifted product of -32768 * -32768 is
// 32768, which only the 32-bit intermediate keeps (RPCS3 `VMHADDSHS` special-cases it).
// vmhraddshs adds 0x4000 before the shift (RPCS3 `VMHRADDSHS`).
TOX2_FORCEINLINE Tox2Vr tox2Vmhaddshs(Tox2PpuContext* ctx, Tox2Vr a, Tox2Vr b, Tox2Vr c) {
    Tox2Vr v;
    for (int i = 0; i < 8; i++) v.s16[i] = tox2ClampS16(ctx, (((int32_t)a.s16[i] * b.s16[i]) >> 15) + c.s16[i]);
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vmhraddshs(Tox2PpuContext* ctx, Tox2Vr a, Tox2Vr b, Tox2Vr c) {
    Tox2Vr v;
    for (int i = 0; i < 8; i++)
        v.s16[i] = tox2ClampS16(ctx, (((int32_t)a.s16[i] * b.s16[i] + 0x4000) >> 15) + c.s16[i]);
    return v;
}

// vmladduhm: low 16 bits of a * b + c per halfword (RPCS3 `VMLADDUHM`).
TOX2_FORCEINLINE Tox2Vr tox2Vmladduhm(Tox2Vr a, Tox2Vr b, Tox2Vr c) {
    Tox2Vr v;
    v.u16 = a.u16 * b.u16 + c.u16;
    return v;
}

// vmsum*: word i of the result is c's word i plus the products of the elements of a and b that lie
// in word i. Elements of one word share a host lane, so no big-endian mapping is needed. The modulo
// forms wrap in 32 bits; the saturating forms add exactly in 64 bits and clamp (RPCS3 `VMSUMUBM`,
// `VMSUMMBM` (a signed, b unsigned), `VMSUMUHM`, `VMSUMSHM`, `VMSUMUHS`, `VMSUMSHS`).
TOX2_FORCEINLINE Tox2Vr tox2Vmsumubm(Tox2Vr a, Tox2Vr b, Tox2Vr c) {
    Tox2Vr v;
    for (int i = 0; i < 4; i++) {
        uint32_t sum = c.u32[i];
        for (int k = 0; k < 4; k++) sum += (uint32_t)a.u8[4 * i + k] * b.u8[4 * i + k];
        v.u32[i] = sum;
    }
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vmsummbm(Tox2Vr a, Tox2Vr b, Tox2Vr c) {
    Tox2Vr v;
    for (int i = 0; i < 4; i++) {
        uint32_t sum = c.u32[i];
        for (int k = 0; k < 4; k++) sum += (uint32_t)((int32_t)a.s8[4 * i + k] * b.u8[4 * i + k]);
        v.u32[i] = sum;
    }
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vmsumuhm(Tox2Vr a, Tox2Vr b, Tox2Vr c) {
    Tox2Vr v;
    for (int i = 0; i < 4; i++)
        v.u32[i] = c.u32[i] + (uint32_t)a.u16[2 * i] * b.u16[2 * i] + (uint32_t)a.u16[2 * i + 1] * b.u16[2 * i + 1];
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vmsumshm(Tox2Vr a, Tox2Vr b, Tox2Vr c) {
    Tox2Vr v;
    for (int i = 0; i < 4; i++)
        v.u32[i] = c.u32[i] + (uint32_t)((int32_t)a.s16[2 * i] * b.s16[2 * i]) +
                   (uint32_t)((int32_t)a.s16[2 * i + 1] * b.s16[2 * i + 1]);
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vmsumuhs(Tox2PpuContext* ctx, Tox2Vr a, Tox2Vr b, Tox2Vr c) {
    Tox2Vr v;
    for (int i = 0; i < 4; i++)
        v.u32[i] = tox2ClampU32(ctx, (uint64_t)c.u32[i] + (uint64_t)a.u16[2 * i] * b.u16[2 * i] +
                                             (uint64_t)a.u16[2 * i + 1] * b.u16[2 * i + 1]);
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vmsumshs(Tox2PpuContext* ctx, Tox2Vr a, Tox2Vr b, Tox2Vr c) {
    Tox2Vr v;
    for (int i = 0; i < 4; i++)
        v.s32[i] = tox2ClampS32(ctx, (int64_t)c.s32[i] + (int64_t)a.s16[2 * i] * b.s16[2 * i] +
                                             (int64_t)a.s16[2 * i + 1] * b.s16[2 * i + 1]);
    return v;
}

// vsum4*: word i of the result is b's word i plus the elements of a's word i, summed exactly in
// 64 bits before the clamp (RPCS3 `VSUM4SBS`, `VSUM4SHS`, `VSUM4UBS`).
TOX2_FORCEINLINE Tox2Vr tox2Vsum4sbs(Tox2PpuContext* ctx, Tox2Vr a, Tox2Vr b) {
    Tox2Vr v;
    for (int i = 0; i < 4; i++) {
        int64_t sum = b.s32[i];
        for (int k = 0; k < 4; k++) sum += a.s8[4 * i + k];
        v.s32[i] = tox2ClampS32(ctx, sum);
    }
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vsum4shs(Tox2PpuContext* ctx, Tox2Vr a, Tox2Vr b) {
    Tox2Vr v;
    for (int i = 0; i < 4; i++) v.s32[i] = tox2ClampS32(ctx, (int64_t)b.s32[i] + a.s16[2 * i] + a.s16[2 * i + 1]);
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vsum4ubs(Tox2PpuContext* ctx, Tox2Vr a, Tox2Vr b) {
    Tox2Vr v;
    for (int i = 0; i < 4; i++) {
        uint64_t sum = b.u32[i];
        for (int k = 0; k < 4; k++) sum += a.u8[4 * i + k];
        v.u32[i] = tox2ClampU32(ctx, sum);
    }
    return v;
}

// vsum2sws: big-endian word 1 = a0 + a1 + b1 and word 3 = a2 + a3 + b3, saturated; words 0 and 2
// are zero. vsumsws: word 3 = a0 + a1 + a2 + a3 + b3, the other words are zero. Big-endian word n is
// host lane 3 - n (RPCS3 `VSUM2SWS`, `VSUMSWS` read b's host lane 0 and 2 the same way).
TOX2_FORCEINLINE Tox2Vr tox2Vsum2sws(Tox2PpuContext* ctx, Tox2Vr a, Tox2Vr b) {
    Tox2Vr v = {};
    v.s32[2] = tox2ClampS32(ctx, (int64_t)a.s32[3] + a.s32[2] + b.s32[2]);
    v.s32[0] = tox2ClampS32(ctx, (int64_t)a.s32[1] + a.s32[0] + b.s32[0]);
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2Vsumsws(Tox2PpuContext* ctx, Tox2Vr a, Tox2Vr b) {
    Tox2Vr v = {};
    v.s32[0] = tox2ClampS32(ctx, (int64_t)a.s32[3] + a.s32[2] + a.s32[1] + a.s32[0] + b.s32[0]);
    return v;
}

// Floating-point vector helpers. Each follows the body of the RPCS3 handler named in its comment,
// which is not always what the handler's `select<>` flag list suggests: the `exec` body decides where
// denormals are flushed and which operands `ppu_set_vnan` receives. Non-Java mode (VSCR[NJ]) is
// honoured per the context and NaN propagation is the "full" variant of `ppu_set_vnan`. Lanes use
// plain IEEE operations in round-to-nearest; denormals are flushed in software, never through MXCSR.

// RPCS3 `ppu_flush_denormal`: with NJ set, a lane whose exponent field is 0 keeps only its sign.
TOX2_FORCEINLINE Tox2Vr tox2FlushDenormals(const Tox2PpuContext* ctx, Tox2Vr v) {
    Tox2U32x4 denormal = (Tox2U32x4)((v.u32 & 0x7f800000u) == 0u);
    denormal = denormal & (0u - (uint32_t)(ctx->vscr_nj & 1u));
    v.u32 = v.u32 & ~(denormal >> 1);
    return v;
}

TOX2_FORCEINLINE Tox2U32x4 tox2IsNanBits(Tox2U32x4 bits) { return (Tox2U32x4)((bits & 0x7fffffffu) > 0x7f800000u); }

// RPCS3 `ppu_fix_vnan`: a NaN result becomes the default quiet NaN, whatever the host produced.
TOX2_FORCEINLINE Tox2U32x4 tox2FixNanBits(Tox2U32x4 r) {
    Tox2U32x4 nan = tox2IsNanBits(r);
    return (r & ~nan) | (nan & 0x7fc00000u);
}

// RPCS3 `ppu_select_vnan`: operand `a` if it is a NaN (quieted, payload and sign kept), else `rest`.
TOX2_FORCEINLINE Tox2U32x4 tox2SelectNan(Tox2U32x4 a, Tox2U32x4 rest) {
    Tox2U32x4 na = tox2IsNanBits(a);
    return ((a | 0x7fc00000u) & na) | (rest & ~na);
}

// RPCS3 `ppu_set_vnan` with operands: the first NaN among the operands wins, otherwise the result
// with a NaN replaced by the default NaN.
TOX2_FORCEINLINE Tox2U32x4 tox2SetVnan1(Tox2U32x4 r, Tox2U32x4 a) { return tox2SelectNan(a, tox2FixNanBits(r)); }

TOX2_FORCEINLINE Tox2U32x4 tox2SetVnan2(Tox2U32x4 r, Tox2U32x4 a, Tox2U32x4 b) {
    return tox2SelectNan(a, tox2SelectNan(b, tox2FixNanBits(r)));
}

// vaddfp, vsubfp (RPCS3 `VADDFP`, `VSUBFP`): flags use_nj fix_nj set_vnan fix_vnan; inputs and result
// are flushed, NaN operands a then b propagate.
TOX2_FORCEINLINE Tox2Vr tox2Vaddfp(const Tox2PpuContext* ctx, Tox2Vr a, Tox2Vr b) {
    a = tox2FlushDenormals(ctx, a);
    b = tox2FlushDenormals(ctx, b);
    Tox2Vr r;
    r.f32 = a.f32 + b.f32;
    r.u32 = tox2SetVnan2(r.u32, a.u32, b.u32);
    return tox2FlushDenormals(ctx, r);
}

TOX2_FORCEINLINE Tox2Vr tox2Vsubfp(const Tox2PpuContext* ctx, Tox2Vr a, Tox2Vr b) {
    a = tox2FlushDenormals(ctx, a);
    b = tox2FlushDenormals(ctx, b);
    Tox2Vr r;
    r.f32 = a.f32 - b.f32;
    r.u32 = tox2SetVnan2(r.u32, a.u32, b.u32);
    return tox2FlushDenormals(ctx, r);
}

// vmaddfp = a * c + b, vnmsubfp = -(a * c - b), both with a single rounding (RPCS3 `VMADDFP`,
// `VNMSUBFP`, `gv_fmafs`): flags use_nj fix_nj set_vnan fix_vnan, but `ppu_set_vnan` gets no operands
// so any NaN result, including one propagated from an operand, is the default NaN. vnmsubfp negates
// by sign-bit xor on the addend and on the result, as the handler does.
TOX2_FORCEINLINE Tox2Vr tox2Vmaddfp(const Tox2PpuContext* ctx, Tox2Vr a, Tox2Vr b, Tox2Vr c) {
    a = tox2FlushDenormals(ctx, a);
    b = tox2FlushDenormals(ctx, b);
    c = tox2FlushDenormals(ctx, c);
    Tox2Vr r;
    r.f32 = __builtin_elementwise_fma(a.f32, c.f32, b.f32);
    r.u32 = tox2FixNanBits(r.u32);
    return tox2FlushDenormals(ctx, r);
}

TOX2_FORCEINLINE Tox2Vr tox2Vnmsubfp(const Tox2PpuContext* ctx, Tox2Vr a, Tox2Vr b, Tox2Vr c) {
    a = tox2FlushDenormals(ctx, a);
    b = tox2FlushDenormals(ctx, b);
    c = tox2FlushDenormals(ctx, c);
    Tox2Vr r;
    r.f32 = __builtin_elementwise_fma(a.f32, c.f32, (Tox2F32x4)(b.u32 ^ 0x80000000u));
    r.u32 = tox2FixNanBits(r.u32 ^ 0x80000000u);
    return tox2FlushDenormals(ctx, r);
}

// vmaxfp, vminfp (RPCS3 `VMAXFP`, `VMINFP`): the result is flushed but the inputs are not (the
// handlers apply no input flush although VMAXFP lists use_nj); NaN operands a then b propagate. For
// equal operands, including +0 and -0, the second operand is returned, which is what `gv_maxfs` /
// `gv_minfs` give as x86 `maxps` / `minps` (their definition is not among the reference sources).
TOX2_FORCEINLINE Tox2Vr tox2Vmaxfp(const Tox2PpuContext* ctx, Tox2Vr a, Tox2Vr b) {
    Tox2U32x4 pick_a = (Tox2U32x4)(a.f32 > b.f32);
    Tox2Vr r;
    r.u32 = tox2SetVnan2((a.u32 & pick_a) | (b.u32 & ~pick_a), a.u32, b.u32);
    return tox2FlushDenormals(ctx, r);
}

TOX2_FORCEINLINE Tox2Vr tox2Vminfp(const Tox2PpuContext* ctx, Tox2Vr a, Tox2Vr b) {
    Tox2U32x4 pick_a = (Tox2U32x4)(a.f32 < b.f32);
    Tox2Vr r;
    r.u32 = tox2SetVnan2((a.u32 & pick_a) | (b.u32 & ~pick_a), a.u32, b.u32);
    return tox2FlushDenormals(ctx, r);
}

// vrefp, vrsqrtefp (RPCS3 `VREFP`, `VRSQRTEFP`): exact reciprocal and reciprocal square root, which
// is within the architected estimate error. Flags use_nj fix_nj set_vnan fix_vnan; operand b.
TOX2_FORCEINLINE Tox2Vr tox2Vrefp(const Tox2PpuContext* ctx, Tox2Vr b) {
    Tox2F32x4 one = {1.0f, 1.0f, 1.0f, 1.0f};
    b = tox2FlushDenormals(ctx, b);
    Tox2Vr r;
    r.f32 = one / b.f32;
    r.u32 = tox2SetVnan1(r.u32, b.u32);
    return tox2FlushDenormals(ctx, r);
}

TOX2_FORCEINLINE Tox2Vr tox2Vrsqrtefp(const Tox2PpuContext* ctx, Tox2Vr b) {
    Tox2F32x4 one = {1.0f, 1.0f, 1.0f, 1.0f};
    b = tox2FlushDenormals(ctx, b);
    Tox2Vr r;
    r.f32 = one / __builtin_elementwise_sqrt(b.f32);
    r.u32 = tox2SetVnan1(r.u32, b.u32);
    return tox2FlushDenormals(ctx, r);
}

// vexptefp, vlogefp (RPCS3 `VEXPTEFP`, `VLOGEFP`): flag fix_vnan only, so no flush and no operand
// NaN propagation; a NaN result is the default NaN.
TOX2_FORCEINLINE Tox2Vr tox2Vexptefp(Tox2Vr b) {
    Tox2Vr r;
    for (int i = 0; i < 4; i++) r.f32[i] = __builtin_exp2f(b.f32[i]);
    r.u32 = tox2FixNanBits(r.u32);
    return r;
}

TOX2_FORCEINLINE Tox2Vr tox2Vlogefp(Tox2Vr b) {
    Tox2Vr r;
    for (int i = 0; i < 4; i++) r.f32[i] = __builtin_log2f(b.f32[i]);
    r.u32 = tox2FixNanBits(r.u32);
    return r;
}

// vrfin/vrfiz/vrfip/vrfim (RPCS3 `VRFIN`, `VRFIZ`, `VRFIP`, `VRFIM`): round to nearest even, toward
// zero, up, down; NaN operand b propagates. VRFIP and VRFIM flush the input as well (flags use_nj
// fix_nj), which matters: a negative denormal rounds down to -1.0 unflushed but to -0 flushed.
// VRFIN and VRFIZ flush the result only; flushing their input first gives the same result, so all
// four share one shape here.
#define TOX2_VMX_ROUND(fn, builtin)                                   \
    TOX2_FORCEINLINE Tox2Vr fn(const Tox2PpuContext* ctx, Tox2Vr b) { \
        b = tox2FlushDenormals(ctx, b);                               \
        Tox2Vr r;                                                     \
        r.f32 = builtin(b.f32);                                       \
        r.u32 = tox2SetVnan1(r.u32, b.u32);                           \
        return tox2FlushDenormals(ctx, r);                            \
    }

TOX2_VMX_ROUND(tox2Vrfin, __builtin_elementwise_roundeven)
TOX2_VMX_ROUND(tox2Vrfiz, __builtin_elementwise_trunc)
TOX2_VMX_ROUND(tox2Vrfip, __builtin_elementwise_ceil)
TOX2_VMX_ROUND(tox2Vrfim, __builtin_elementwise_floor)
#undef TOX2_VMX_ROUND

// 2^e as a float for |e| <= 31, built from the exponent field so no libm call is needed.
TOX2_FORCEINLINE float tox2Pow2f(int32_t e) {
    uint32_t bits = (uint32_t)(127 + e) << 23;
    float f;
    __builtin_memcpy(&f, &bits, sizeof f);
    return f;
}

// vcfsx/vcfux (RPCS3 `VCFSX`, `VCFUX`): convert and divide by 2^uimm. The handlers subtract the
// shift from the exponent field instead of multiplying; the two agree for every input because a
// nonzero integer converts to an exponent of at least 127 and the shift is at most 31.
TOX2_FORCEINLINE Tox2Vr tox2Vcfsx(Tox2Vr b, uint32_t uimm) {
    Tox2Vr r;
    r.f32 = __builtin_convertvector(b.s32, Tox2F32x4) * tox2Pow2f(-(int32_t)uimm);
    return r;
}

TOX2_FORCEINLINE Tox2Vr tox2Vcfux(Tox2Vr b, uint32_t uimm) {
    Tox2Vr r;
    r.f32 = __builtin_convertvector(b.u32, Tox2F32x4) * tox2Pow2f(-(int32_t)uimm);
    return r;
}

// vctsxs/vctuxs (RPCS3 `VCTSXS`, `VCTUXS`): multiply by 2^uimm, truncate, saturate. SAT is set when
// the scaled value is below the range (unsigned: below 0, even for -0.5) or at or above the upper
// bound; NaN gives 0 without SAT (flag fix_vnan, and the handler's comparisons are ordered). Lanes
// are range-checked before the C conversion, which is undefined for out-of-range values.
TOX2_FORCEINLINE Tox2Vr tox2Vctsxs(Tox2PpuContext* ctx, Tox2Vr b, uint32_t uimm) {
    Tox2F32x4 scaled = b.f32 * tox2Pow2f((int32_t)uimm);
    Tox2Vr r;
    int sat = 0;
    for (int i = 0; i < 4; i++) {
        float x = scaled[i];
        if (x < -2147483648.0f) {
            r.s32[i] = INT32_MIN;
            sat = 1;
        } else if (x >= 2147483648.0f) {
            r.s32[i] = INT32_MAX;
            sat = 1;
        } else if (x != x) {
            r.s32[i] = 0;
        } else {
            r.s32[i] = (int32_t)x;
        }
    }
    tox2SetSat(ctx, sat);
    return r;
}

TOX2_FORCEINLINE Tox2Vr tox2Vctuxs(Tox2PpuContext* ctx, Tox2Vr b, uint32_t uimm) {
    Tox2F32x4 scaled = b.f32 * tox2Pow2f((int32_t)uimm);
    Tox2Vr r;
    int sat = 0;
    for (int i = 0; i < 4; i++) {
        float x = scaled[i];
        if (x < 0.0f) {
            r.u32[i] = 0;
            sat = 1;
        } else if (x >= 4294967296.0f) {
            r.u32[i] = UINT32_MAX;
            sat = 1;
        } else if (x != x) {
            r.u32[i] = 0;
        } else {
            r.u32[i] = (uint32_t)x;
        }
    }
    tox2SetSat(ctx, sat);
    return r;
}

// vcmpbfp (RPCS3 `VCMPBFP`): bit 0 (value 0x80000000) is set when a > b or unordered, bit 1
// (0x40000000) when a < -b or unordered; a lane is within bounds when both are clear. The record
// form sets CR6 eq when every lane is in bounds (and lt = gt = so = 0); tox2SetCr6 produces exactly
// that because the result lanes can never be all ones.
TOX2_FORCEINLINE Tox2Vr tox2Vcmpbfp(Tox2Vr a, Tox2Vr b) {
    Tox2U32x4 above = ~(Tox2U32x4)(a.f32 <= b.f32);
    Tox2U32x4 below = ~(Tox2U32x4)(a.f32 >= (Tox2F32x4)(b.u32 ^ 0x80000000u));
    Tox2Vr r;
    r.u32 = (above & 0x80000000u) | ((below & 0x80000000u) >> 1);
    return r;
}
