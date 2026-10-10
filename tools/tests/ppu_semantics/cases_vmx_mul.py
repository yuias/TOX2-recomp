"""vmx_mul cases: per-element shifts and rotates, widening multiplies, multiply-sum, sum-across.

Expected values come from two sources, never from the emitter. The literal cases pin the key edge
cases with values worked out by hand (derivations in the comments). The matrix cases feed boundary
lane patterns to every mnemonic and compute the expectation lane by lane with plain Python integers
from the AltiVec definitions (`model` below: modulo shift counts, even/odd element products, exact
sums followed by a clamp), which share no code or bit tricks with the C helpers. Vector words are
in big-endian element order; "even" and "odd" count elements from big-endian element 0.
"""

from __future__ import annotations

import re

from tests import ppc_asm as asm

from . import Case
from .cases_vmx_int import signed, to_words

REF = "RPCS3 PPUInterpreter.cpp "

enc = asm.encode

CASES = []

THREE_SOURCE = ("vmhaddshs", "vmhraddshs", "vmladduhm", "vmsumubm", "vmsummbm", "vmsumuhm",
                "vmsumuhs", "vmsumshm", "vmsumshs")
BITS = {"b": 8, "h": 16, "w": 32}


def case(name: str, ref: str, words, init=None, expect=None) -> Case:
    if not isinstance(words, (list, tuple)):
        words = [words]
    return Case(name, REF + ref, tuple(words), init or {}, expect or {})


def op(name: str, a, b, expect, tag: str, c=None, init=None, **extra) -> Case:
    """vD = name(vA = v1, vB = v2[, vC = v4]) into v3."""
    fields = {"vC": 4} if c is not None else {}
    state = {"v1": a, "v2": b, **({"v4": c} if c is not None else {}), **(init or {})}
    return case("%s_%s" % (name, tag), name.upper(), enc(name, vD=3, vA=1, vB=2, **fields), state,
                {"v3": expect, **extra})


# --- lane model ---------------------------------------------------------------------------------


def from_words(words, bits: int) -> list[int]:
    """Unsigned lanes in big-endian element order."""
    value = 0
    for word in words:
        value = (value << 32) | word
    count = 128 // bits
    return [(value >> (bits * (count - 1 - i))) & ((1 << bits) - 1) for i in range(count)]


def clamp(x: int, lo: int, hi: int) -> tuple[int, bool]:
    return min(max(x, lo), hi), not lo <= x <= hi


S16 = (-(1 << 15), (1 << 15) - 1)
S32 = (-(1 << 31), (1 << 31) - 1)
U32 = (0, (1 << 32) - 1)


def model(name: str, a_words, b_words, c_words=None) -> tuple[tuple[int, ...], int]:
    """Result words and SAT for one instruction, from the architecture definitions."""
    sat = False
    if match := re.fullmatch(r"v(rl|sl|sr|sra)([bhw])", name):
        kind, letter = match.groups()
        bits = BITS[letter]
        a, b = from_words(a_words, bits), from_words(b_words, bits)
        out = []
        for x, count in zip(a, b):
            n = count % bits
            if kind == "rl":
                out.append((x << n) | (x >> (bits - n)))
            elif kind == "sl":
                out.append(x << n)
            elif kind == "sr":
                out.append(x >> n)
            else:
                out.append(signed(x, bits) >> n)
        return to_words(out, bits), 0
    if match := re.fullmatch(r"vmul([eo])([su])([bh])", name):
        parity, sign, letter = match.groups()
        bits = BITS[letter]
        a, b = from_words(a_words, bits), from_words(b_words, bits)
        start = 0 if parity == "e" else 1
        conv = (lambda x: signed(x, bits)) if sign == "s" else (lambda x: x)
        out = [conv(x) * conv(y) for x, y in zip(a[start::2], b[start::2])]
        return to_words(out, 2 * bits), 0
    a, b = from_words(a_words, 16), from_words(b_words, 16)
    if name in ("vmhaddshs", "vmhraddshs"):
        c = from_words(c_words, 16)
        out = []
        for x, y, z in zip(a, b, c):
            product = signed(x, 16) * signed(y, 16) + (0x4000 if name == "vmhraddshs" else 0)
            value, clamped = clamp((product >> 15) + signed(z, 16), *S16)
            out.append(value)
            sat |= clamped
        return to_words(out, 16), int(sat)
    if name == "vmladduhm":
        c = from_words(c_words, 16)
        return to_words([x * y + z for x, y, z in zip(a, b, c)], 16), 0
    if match := re.fullmatch(r"vmsum(ub|mb|uh|sh)([ms])", name):
        element, mode = match.groups()
        bits = 8 if element in ("ub", "mb") else 16
        a, b = from_words(a_words, bits), from_words(b_words, bits)
        a_signed = element in ("mb", "sh")
        b_signed = element == "sh"
        per_word = 32 // bits
        out = []
        for j, c in enumerate(from_words(c_words, 32)):
            total = signed(c, 32) if element == "sh" and mode == "s" else c
            for k in range(per_word * j, per_word * (j + 1)):
                total += (signed(a[k], bits) if a_signed else a[k]) * \
                         (signed(b[k], bits) if b_signed else b[k])
            if mode == "m":
                out.append(total)
            else:
                value, clamped = clamp(total, *(S32 if element == "sh" else U32))
                out.append(value)
                sat |= clamped
        return to_words(out, 32), int(sat)
    if match := re.fullmatch(r"vsum4(sb|sh|ub)s", name):
        element = match.group(1)
        bits = 8 if element in ("sb", "ub") else 16
        a = from_words(a_words, bits)
        per_word = 32 // bits
        out = []
        for j, bw in enumerate(from_words(b_words, 32)):
            if element == "ub":
                total = bw + sum(a[per_word * j:per_word * (j + 1)])
                value, clamped = clamp(total, *U32)
            else:
                total = signed(bw, 32) + sum(signed(x, bits) for x in a[per_word * j:per_word * (j + 1)])
                value, clamped = clamp(total, *S32)
            out.append(value)
            sat |= clamped
        return to_words(out, 32), int(sat)
    a, b = from_words(a_words, 32), from_words(b_words, 32)
    a, b = [signed(x, 32) for x in a], [signed(x, 32) for x in b]
    if name == "vsum2sws":
        low, clamped_low = clamp(a[0] + a[1] + b[1], *S32)
        high, clamped_high = clamp(a[2] + a[3] + b[3], *S32)
        return to_words([0, low, 0, high], 32), int(clamped_low or clamped_high)
    if name == "vsumsws":
        value, clamped = clamp(sum(a) + b[3], *S32)
        return to_words([0, 0, 0, value], 32), int(clamped)
    raise ValueError(name)


def boundary_values(bits: int) -> list[int]:
    mask = (1 << bits) - 1
    smax, smin = (1 << (bits - 1)) - 1, 1 << (bits - 1)
    return [0, 1, 2, smax, smin, mask, mask - 1, smin + 1, smax - 1, 3, 5, 7, mask - 2,
            0x55555555 & mask, 0xAAAAAAAA & mask, 0x12345678 & mask]


def shift_counts(bits: int) -> list[int]:
    mask = (1 << bits) - 1
    return [0, 1, bits - 1, bits, bits + 1, 2 * bits - 1, mask, 3, 5, 6, 2, 4, 7, bits // 2,
            (1 << (bits - 1)) | 3, mask - bits + 3]


def pick(values: list[int], count: int, mul: int, add: int) -> list[int]:
    return [values[(i * mul + add) % len(values)] for i in range(count)]


def add_matrix(name: str, element_bits: int, c_bits: int | None, counts: bool = False) -> None:
    lanes = 128 // element_bits
    for k in range(4):
        a = pick(boundary_values(element_bits), lanes, 5, 3 * k)
        b = pick(shift_counts(element_bits) if counts else boundary_values(element_bits), lanes, 7,
                 5 * k + 1)
        c = pick(boundary_values(c_bits), 128 // c_bits, 3, k) if c_bits else None
        a_words, b_words = to_words(a, element_bits), to_words(b, element_bits)
        c_words = to_words(c, c_bits) if c else None
        words, sat = model(name, a_words, b_words, c_words)
        CASES.append(op(name, a_words, b_words, words, "boundaries_%d" % k, c=c_words, vscr_sat=sat))


for _kind in ("rl", "sl", "sr", "sra"):
    for _letter in "bhw":
        add_matrix("v%s%s" % (_kind, _letter), BITS[_letter], None, counts=True)

for _sign in "su":
    for _letter in "bh":
        for _parity in "eo":
            add_matrix("vmul%s%s%s" % (_parity, _sign, _letter), BITS[_letter], None)

add_matrix("vmhaddshs", 16, 16)
add_matrix("vmhraddshs", 16, 16)
add_matrix("vmladduhm", 16, 16)
for _name, _bits in (("vmsumubm", 8), ("vmsummbm", 8), ("vmsumuhm", 16), ("vmsumshm", 16),
                     ("vmsumuhs", 16), ("vmsumshs", 16)):
    add_matrix(_name, _bits, 32)
for _name, _bits in (("vsum4ubs", 8), ("vsum4sbs", 8), ("vsum4shs", 16)):
    add_matrix(_name, _bits, None)
add_matrix("vsum2sws", 32, None)
add_matrix("vsumsws", 32, None)


# --- literal cases: shifts and rotates ----------------------------------------------------------

CASES += [
    # Counts modulo 32: 0 -> 0, 1 -> 1, 32 -> 0, 0xFFFFFFE4 -> 4 (the high bits are ignored).
    op("vslw", (1, 0x80000001, 0xFFFFFFFF, 0x12345678), (0, 1, 32, 0xFFFFFFE4),
       (1, 2, 0xFFFFFFFF, 0x23456780), "counts_mod_32", vscr_sat=0),
    # 80000000 >> 31 = 1, FFFFFFFF >> 1, 12345678 >> 4, count 32 -> 0 leaves the value.
    op("vsrw", (0x80000000, 0xFFFFFFFF, 0x12345678, 0x80000000), (31, 1, 0xFFFFFFE4, 32),
       (1, 0x7FFFFFFF, 0x01234567, 0x80000000), "logical_counts_mod_32"),
    # Arithmetic: sign fills in; count 32 -> 0.
    op("vsraw", (0x80000000, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000), (31, 1, 31, 32),
       (0xFFFFFFFF, 0xFFFFFFFF, 0, 0x80000000), "sign_fill"),
    # 80000001 rol 1 = 00000003, 12345678 rol 8, rol 0, 1 rol 31 = 80000000.
    op("vrlw", (0x80000001, 0x12345678, 0xDEADBEEF, 1), (1, 8, 0, 31),
       (3, 0x34567812, 0xDEADBEEF, 0x80000000), "rotates"),
    # Bytes: 81 rol 1 = 03; 40 rol (8&7=0) = 40; 20 rol (9&7=1) = 40; 10 rol 2 = 40;
    # FF rol 7 = FF; 00 rol (FF&7=7) = 00; AA rol (0F&7=7) = 55; 55 rol 0 = 55.
    op("vrlb", (0x81402010, 0xFF00AA55, 0, 0), (0x01080902, 0x07FF0F00, 0, 0),
       (0x03404040, 0xFF005555, 0, 0), "counts_mod_8"),
    # Halfwords: 1<<15 = 8000, FFFF<<(16&15=0), 8000<<1 = 0, 8001<<(31&15=15) = 8000.
    op("vslh", (0x0001FFFF, 0x80008001, 0, 0), (0x000F0010, 0x0001001F, 0, 0),
       (0x8000FFFF, 0x00008000, 0, 0), "counts_mod_16"),
    # 8000 >>s 15 = FFFF, 7FFF >>s 15 = 0, FFFF >>s 1 = FFFF, 8000 >>s (16&15=0) = 8000.
    op("vsrah", (0x80007FFF, 0xFFFF8000, 0, 0), (0x000F000F, 0x00010010, 0, 0),
       (0xFFFF0000, 0xFFFF8000, 0, 0), "sign_fill"),
    op("vsrh", (0x8000FFFF, 0x80008000, 0, 0), (0x000F0010, 0x00010011, 0, 0),
       (0x0001FFFF, 0x40004000, 0, 0), "logical"),
    # Bytes: 80 >>s 7 = FF, 7F >>s 7 = 00, FF >>s 1 = FF, 01 >>s (8&7=0) = 01.
    op("vsrab", (0x807FFF01, 0, 0, 0), (0x07070108, 0, 0, 0), (0xFF00FF01, 0, 0, 0), "sign_fill"),
    # 80 >> 7 = 01, 7F >> 7 = 00, FF >> 1 = 7F, 01 >> 0 = 01.
    op("vsrb", (0x807FFF01, 0, 0, 0), (0x07070108, 0, 0, 0), (0x01007F01, 0, 0, 0), "logical"),
    # 01 << 7 = 80, 80 << 1 = 00, 7F << 1 = FE, FF << (8&7=0) = FF.
    op("vslb", (0x01807FFF, 0, 0, 0), (0x07010108, 0, 0, 0), (0x8000FEFF, 0, 0, 0), "counts_mod_8"),
    case("vslw_destination_is_vrb", "VSLW", enc("vslw", vD=2, vA=1, vB=2),
         {"v1": (1, 1, 1, 1), "v2": (1, 2, 3, 4)}, {"v2": (2, 4, 8, 16)}),
]


# --- literal cases: widening multiplies ---------------------------------------------------------

A8 = (0x01020304, 0x05060708, 0xFF80FE7F, 0x00FF0102)
B8 = (0x10203040, 0x50607080, 0xFF020301, 0x0A0B0C0D)
A16 = (0x00010002, 0xFFFF8000, 0x1234ABCD, 0x7FFF0003)
B16 = (0x00030004, 0xFFFF0002, 0x00020001, 0x80000005)

CASES += [
    # Even bytes of A: 01 03 05 07 FF FE 00 01; of B: 10 30 50 70 FF 03 0A 0C. Products 0010, 0090,
    # 0190, 0310, FE01, 02FA, 0000, 000C.
    op("vmuleub", A8, B8, (0x00100090, 0x01900310, 0xFE0102FA, 0x0000000C), "even_bytes"),
    # Odd bytes of A: 02 04 06 08 80 7F FF 02; of B: 20 40 60 80 02 01 0B 0D. Products 0040, 0100,
    # 0240, 0400, 0100, 007F, 0AF5, 001A.
    op("vmuloub", A8, B8, (0x00400100, 0x02400400, 0x0100007F, 0x0AF5001A), "odd_bytes"),
    # Signed even: FF = -1, FE = -2, 70 = 112. Products 16, 144, 400, 784, (-1)(-1) = 1, (-2)(3) = FFFA,
    # 0, 12.
    op("vmulesb", A8, B8, (0x00100090, 0x01900310, 0x0001FFFA, 0x0000000C), "even_bytes"),
    # Signed odd: 80 = -128, 7F = 127, FF = -1. Products 64, 256, 576, 8 * -128 = FC00,
    # -128 * 2 = FF00, 127, -1 * 11 = FFF5, 26.
    op("vmulosb", A8, B8, (0x00400100, 0x0240FC00, 0xFF00007F, 0xFFF5001A), "odd_bytes"),
    # Even halfwords of A: 0001 FFFF 1234 7FFF; of B: 0003 FFFF 0002 8000.
    op("vmuleuh", A16, B16, (3, 0xFFFE0001, 0x2468, 0x3FFF8000), "even_halfwords"),
    # Odd halfwords of A: 0002 8000 ABCD 0003; of B: 0004 0002 0001 0005.
    op("vmulouh", A16, B16, (8, 0x10000, 0xABCD, 0xF), "odd_halfwords"),
    # Signed even: 1 * 3, (-1)(-1), 0x1234 * 2, 32767 * -32768 = -0x3FFF8000 = C0008000.
    op("vmulesh", A16, B16, (3, 1, 0x2468, 0xC0008000), "even_halfwords"),
    # Signed odd: 2 * 4, -32768 * 2 = FFFF0000, ABCD (-21555) * 1, 3 * 5.
    op("vmulosh", A16, B16, (8, 0xFFFF0000, 0xFFFFABCD, 0xF), "odd_halfwords"),
    # -32768 squared is the one product that needs all 31 bits.
    op("vmulesh", (0x80000000, 0, 0, 0), (0x80000000, 0, 0, 0), (0x40000000, 0, 0, 0), "min_squared"),
    op("vmulosb", (0x00800000, 0, 0, 0), (0x00800000, 0, 0, 0), (0x40000000, 0, 0, 0), "min_squared"),
]


# --- literal cases: halfword multiply-add -------------------------------------------------------

# Lanes 0..5 (hex): 8000*8000 + FFFF, 4000*4000, FFFF*0001, 2*3 + 5, 4000*1, C000*1.
HADD_A = (0x80004000, 0xFFFF0002, 0x4000C000, 0)
HADD_B = (0x80004000, 0x00010003, 0x00010001, 0)
HADD_C = (0xFFFF0000, 0x00000005, 0, 0)
# Lanes: 8000*8000 + 1, 7FFF*7FFF + 7FFF, 8000*7FFF + 8000, 0.
SAT_A = (0x80007FFF, 0x80000000, 0, 0)
SAT_B = (0x80007FFF, 0x7FFF0000, 0, 0)
SAT_C = (0x00017FFF, 0x80000000, 0, 0)

CASES += [
    # Lane 0: (2^30 >> 15) = 32768, plus -1 = 32767 without clamping. Lane 1: 2^28 >> 15 = 0x2000.
    # Lane 2: -1 >> 15 = -1 (floor). Lane 3: 0 + 5. Lane 4: 0x4000 >> 15 = 0. Lane 5: -0x4000 >> 15 = -1.
    op("vmhaddshs", HADD_A, HADD_B, (0x7FFF2000, 0xFFFF0005, 0x0000FFFF, 0), "floor_no_clamp",
       c=HADD_C, vscr_sat=0),
    # With rounding: lane 2: (-1 + 0x4000) >> 15 = 0, lane 4: (0x4000 + 0x4000) >> 15 = 1, lane 5: 0.
    op("vmhraddshs", HADD_A, HADD_B, (0x7FFF2000, 0x00000005, 0x00010000, 0), "rounds_half_up",
       c=HADD_C, vscr_sat=0),
    # Lane 0: 32768 + 1 -> 7FFF, lane 1: 7FFE + 7FFF -> 7FFF, lane 2: -32767 + -32768 -> 8000.
    op("vmhaddshs", SAT_A, SAT_B, (0x7FFF7FFF, 0x80000000, 0, 0), "clamps_both_ways", c=SAT_C,
       vscr_sat=1),
    op("vmhraddshs", SAT_A, SAT_B, (0x7FFF7FFF, 0x80000000, 0, 0), "clamps_both_ways", c=SAT_C,
       vscr_sat=1),
    op("vmhaddshs", (1, 2, 3, 4), (5, 6, 7, 8), (0, 0, 0, 0), "small_products_vanish",
       c=(0, 0, 0, 0), init={"vscr_sat": 1}, vscr_sat=1),
    # Low 16 bits of a*b + c: FFFF*FFFF + 1 = FFFE0002 -> 0002; 100*100 + 5 -> 0005;
    # 3*4 + FFFF = 1000B -> 000B; 8000*2 + 7 -> 0007.
    op("vmladduhm", (0xFFFF0100, 0x00038000, 0, 0), (0xFFFF0100, 0x00040002, 0, 0),
       (0x00020005, 0x000B0007, 0, 0), "low_half", c=(0x00010005, 0xFFFF0007, 0, 0), vscr_sat=0),
    case("vmladduhm_destination_is_vrc", "VMLADDUHM", enc("vmladduhm", vD=4, vA=1, vB=2, vC=4),
         {"v1": (0x00020002, 0, 0, 0), "v2": (0x00030003, 0, 0, 0), "v4": (0x00010001, 0, 0, 0)},
         {"v4": (0x00070007, 0, 0, 0)}),
]


# --- literal cases: multiply-sum ----------------------------------------------------------------

CASES += [
    # w0: 1*5 + 2*6 + 3*7 + 4*8 = 70, + 0x10 = 0x56. w1: 4 * FF*FF = 3F804. w2: 4 * 80*2 = 400 plus
    # FFFFFFFF wraps to 3FF. w3: 0 + 7.
    op("vmsumubm", (0x01020304, 0xFFFFFFFF, 0x80808080, 0), (0x05060708, 0xFFFFFFFF, 0x02020202, 0),
       (0x56, 0x3F804, 0x3FF, 7), "wraps", c=(0x10, 0, 0xFFFFFFFF, 7), vscr_sat=0),
    # A signed, B unsigned. w0: 1*2 - 1*255 - 128*2 + 127*1 = -382, + 1000 = 618 = 26A.
    # w1: 4 * -255 = -1020 = FFFFFC04. w2: -128*255*4 = -1FE00 + 80000000 = 7FFE0200.
    # w3: 127*255*4 = 1FA04 + 7FFFFFFF = 8001FA03 (the sum wraps, it does not clamp).
    op("vmsummbm", (0x01FF807F, 0xFFFFFFFF, 0x80808080, 0x7F7F7F7F),
       (0x02FF0201, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF), (0x26A, 0xFFFFFC04, 0x7FFE0200, 0x8001FA03),
       "signed_by_unsigned", c=(0x3E8, 0, 0x80000000, 0x7FFFFFFF), vscr_sat=0),
    # w0: 1*3 + 2*4 + 10 = 1B. w1: 2 * FFFF*FFFF = 1FFFC0002, + FFFFFFFF wraps to FFFC0001.
    # w2: 2 * 8000*2 = 20000.
    op("vmsumuhm", (0x00010002, 0xFFFFFFFF, 0x80008000, 0), (0x00030004, 0xFFFFFFFF, 0x00020002, 0),
       (0x1B, 0xFFFC0001, 0x20000, 0), "wraps", c=(0x10, 0xFFFFFFFF, 0, 0), vscr_sat=0),
    # w0: 1*3 + (-1)*3 = 0. w1: 2 * 2^30 = 80000000 (wraps into the sign bit). w2: 2 * 7FFF^2 =
    # 7FFE0002. w3: 5.
    op("vmsumshm", (0x0001FFFF, 0x80008000, 0x7FFF7FFF, 0), (0x00030003, 0x80008000, 0x7FFF7FFF, 0),
       (0, 0x80000000, 0x7FFE0002, 5), "signed_wraps", c=(0, 0, 0, 5), vscr_sat=0),
    # w0: 2 * FFFE0001 clamps; w1: 3 + 8 + 10 = 1B; w3: 0 + FFFFFFFF is exact, no clamp.
    op("vmsumuhs", (0xFFFFFFFF, 0x00010002, 0, 0), (0xFFFFFFFF, 0x00030004, 0, 0),
       (0xFFFFFFFF, 0x1B, 0, 0xFFFFFFFF), "clamps_high", c=(0, 0x10, 0, 0xFFFFFFFF), vscr_sat=1),
    # w3: FFFF*FFFF + 1FFFE = FFFFFFFF exactly: SAT stays clear.
    op("vmsumuhs", (0x00010002, 0, 0, 0xFFFF0000), (0x00030004, 0, 0, 0xFFFF0000),
       (0x1B, 7, 0, 0xFFFFFFFF), "exact_maximum", c=(0x10, 7, 0, 0x0001FFFE), vscr_sat=0),
    # w0: 2 * (-32768)^2 = 2^31 clamps to 7FFFFFFF. w1: 7FFE0002 + 10000 = 7FFF0002. w2: -2 + 3 + -2^31
    # = 80000001. w3: 2 * -32768*32767 + -2^31 clamps to 80000000.
    op("vmsumshs", (0x80008000, 0x7FFF7FFF, 0xFFFF0001, 0x80008000),
       (0x80008000, 0x7FFF7FFF, 0x00020003, 0x7FFF7FFF),
       (0x7FFFFFFF, 0x7FFF0002, 0x80000001, 0x80000000), "clamps_both_ways",
       c=(0, 0x10000, 0x80000000, 0x80000000), vscr_sat=1),
    # w0: 3 + 8 + 10 = 21; w1: -10 + 18 + 20 = 28; the rest only c.
    op("vmsumshs", (0x00010002, 0xFFFE0003, 0, 0), (0x00030004, 0x00050006, 0, 0), (21, 28, 30, 40),
       "in_range", c=(10, 20, 30, 40), vscr_sat=0),
    op("vmsumshs", (0x00010002, 0xFFFE0003, 0, 0), (0x00030004, 0x00050006, 0, 0), (21, 28, 30, 40),
       "sat_stays_set", c=(10, 20, 30, 40), init={"vscr_sat": 1}, vscr_sat=1),
]


# --- literal cases: sum across ------------------------------------------------------------------

CASES += [
    # Unsigned bytes. w0: 1+2+3+4 + 10 = 1A. w1: 4*255 = 3FC. w2: 4*80 + FFFFFFFF clamps.
    # w3: 0 + FFFFFFFF is exact.
    op("vsum4ubs", (0x01020304, 0xFFFFFFFF, 0x80808080, 0), (0x10, 0, 0xFFFFFFFF, 0xFFFFFFFF),
       (0x1A, 0x3FC, 0xFFFFFFFF, 0xFFFFFFFF), "clamps_high"),
    # w1: 1020 + FFFFFC03 = FFFFFFFF exactly.
    op("vsum4ubs", (0x01020304, 0xFFFFFFFF, 0, 0), (0x10, 0xFFFFFC03, 0, 0), (0x1A, 0xFFFFFFFF, 0, 0),
       "exact_maximum", vscr_sat=0),
    # Signed bytes. w0: 4*127 + 7FFFFFFF clamps; w1: 4*-128 + 80000000 clamps; w2: -1+1+2+3 + A = F;
    # w3: 0 + -1.
    op("vsum4sbs", (0x7F7F7F7F, 0x80808080, 0xFF010203, 0), (0x7FFFFFFF, 0x80000000, 0xA, 0xFFFFFFFF),
       (0x7FFFFFFF, 0x80000000, 0xF, 0xFFFFFFFF), "clamps_both_ways", vscr_sat=1),
    # w0: 1-1+2+3 + 10 = 15; w1: -512 + 512 = 0.
    op("vsum4sbs", (0x01FF0203, 0x80808080, 0, 0), (0x10, 0x200, 0, 0), (0x15, 0, 0, 0), "in_range",
       vscr_sat=0),
    # Signed halfwords. w0: 2*7FFF + 7FFFFFFF clamps; w1: 2*-32768 + 80000000 clamps;
    # w2: -1 + 2 + 5 = 6; w3: 0 + -1.
    op("vsum4shs", (0x7FFF7FFF, 0x80008000, 0xFFFF0002, 0), (0x7FFFFFFF, 0x80000000, 5, 0xFFFFFFFF),
       (0x7FFFFFFF, 0x80000000, 6, 0xFFFFFFFF), "clamps_both_ways", vscr_sat=1),
    # w0: 2 * 7FFF + 2 = 0x10000; w1: 2 * -32768 + 0x10000 = 0.
    op("vsum4shs", (0x7FFF7FFF, 0x80008000, 0, 0), (2, 0x10000, 0, 0), (0x10000, 0, 0, 0), "in_range",
       vscr_sat=0),
    # Word 1 = a0 + a1 + b1 = 1 + 2 + B, word 3 = a2 + a3 + b3 = 3 + 4 + D; b0 and b2 are ignored.
    op("vsum2sws", (1, 2, 3, 4), (0x1000, 0xB, 0x2000, 0xD), (0, 0xE, 0, 0x14), "pairs", vscr_sat=0),
    # w1: 7FFFFFFF + 1 clamps; w3: -2^31 + -1 clamps.
    op("vsum2sws", (0x7FFFFFFF, 1, 0x80000000, 0xFFFFFFFF), (0, 0, 0, 0), (0, 0x7FFFFFFF, 0, 0x80000000),
       "clamps_both_ways", vscr_sat=1),
    # Word 3 = 1 + 2 + 3 + 4 + b3; the other b words are ignored.
    op("vsumsws", (1, 2, 3, 4), (0x1000, 0x2000, 0x3000, 5), (0, 0, 0, 0xF), "all_words", vscr_sat=0),
    op("vsumsws", (0x7FFFFFFF, 1, 0, 0), (0, 0, 0, 0), (0, 0, 0, 0x7FFFFFFF), "clamps_high",
       vscr_sat=1),
    op("vsumsws", (0x80000000, 0xFFFFFFFF, 0, 0), (0, 0, 0, 0), (0, 0, 0, 0x80000000), "clamps_low",
       vscr_sat=1),
    # 7FFFFFFF + -2^31 + -1 + 3 = 1: the exact total fits, so no partial sum clamps.
    op("vsumsws", (0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0), (0, 0, 0, 3), (0, 0, 0, 1),
       "exact_total_fits", vscr_sat=0),
]
