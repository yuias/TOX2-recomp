"""vmx_int cases: integer vector add, subtract, average, min/max and compare.

Expected values come from two sources, never from the emitter. The literal cases pin boundaries with
values worked out by hand (derivations in the comments). The matrix cases feed boundary lane pairs
to every mnemonic and compute the expectation lane by lane with plain Python integers from the
AltiVec definitions (`model_*` below: wrap, clamp, `(a + b + 1) >> 1`, comparison), which share no
code or bit tricks with the C helpers. Vector words are in big-endian element order.
"""

from tests import ppc_asm as asm

from . import Case

REF = "RPCS3 PPUInterpreter.cpp "

enc = asm.encode

CASES = []


def case(name: str, ref: str, words, init=None, expect=None) -> Case:
    if not isinstance(words, (list, tuple)):
        words = [words]
    return Case(name, REF + ref, tuple(words), init or {}, expect or {})


def binop(name: str, a, b, expect, tag: str, init=None, rc=False, **extra) -> Case:
    fields = {"Rc_vc": 1} if rc else {}
    word = enc(name, vD=3, vA=1, vB=2, **fields)
    return case("%s_%s" % (name, tag), name.upper() + ("_" if rc else ""), word,
                {"v1": a, "v2": b, **(init or {})}, {"v3": expect, **extra})


# --- lane model ---------------------------------------------------------------------------------

BITS = {"b": 8, "h": 16, "w": 32}
SIZE = {"b": 1, "h": 2, "w": 4}


def to_words(lanes, bits: int) -> tuple[int, ...]:
    """Four big-endian words from lanes given in big-endian element order."""
    value = 0
    for lane in lanes:
        value = (value << bits) | (lane & ((1 << bits) - 1))
    return tuple((value >> shift) & 0xFFFFFFFF for shift in (96, 64, 32, 0))


def signed(x: int, bits: int) -> int:
    return x - (1 << bits) if x >> (bits - 1) else x


def boundary_pairs(bits: int) -> list[tuple[int, int]]:
    """Sixteen raw (a, b) lane patterns: signed and unsigned limits, equal and opposite values."""
    mask = (1 << bits) - 1
    smax, smin = (1 << (bits - 1)) - 1, 1 << (bits - 1)
    return [(smax, 1), (smin, mask), (smin, smin), (smax, smax), (0, 0), (1, mask), (5, 7),
            (mask - 4, mask - 6), (mask, 1), (0, mask), (mask, mask), (1, 2), (smin, 1),
            (smax, mask), (3, 3), (mask, 0)]


def chunks(letter: str):
    """The boundary pairs split into vectors: (chunk index, a words, b words, a lanes, b lanes)."""
    bits = BITS[letter]
    lanes = 16 // SIZE[letter]
    pairs = boundary_pairs(bits)
    for index in range(0, 16, lanes):
        a = [p[0] for p in pairs[index:index + lanes]]
        b = [p[1] for p in pairs[index:index + lanes]]
        yield index // lanes, to_words(a, bits), to_words(b, bits), a, b


def model_saturate(op: str, is_signed: bool, bits: int):
    lo, hi = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if is_signed else (0, (1 << bits) - 1)

    def lane(a: int, b: int):
        if is_signed:
            a, b = signed(a, bits), signed(b, bits)
        raw = a + b if op == "add" else a - b
        clamped = min(max(raw, lo), hi)
        return clamped, clamped != raw
    return lane


def model_average(is_signed: bool, bits: int):
    def lane(a: int, b: int):
        if is_signed:
            a, b = signed(a, bits), signed(b, bits)
        return (a + b + 1) >> 1, False
    return lane


def model_extreme(pick, is_signed: bool, bits: int):
    def lane(a: int, b: int):
        if is_signed:
            a, b = signed(a, bits), signed(b, bits)
        return pick(a, b), False
    return lane


def model_compare(kind: str, bits: int):
    def lane(a: int, b: int):
        if kind == "equ":
            true = a == b
        elif kind == "gtu":
            true = a > b
        else:
            true = signed(a, bits) > signed(b, bits)
        return (1 << bits) - 1 if true else 0, False
    return lane


def run_model(lane, a_lanes, b_lanes, bits: int):
    results = [lane(a, b) for a, b in zip(a_lanes, b_lanes)]
    return to_words([r[0] for r in results], bits), int(any(r[1] for r in results))


def add_matrix(name: str, letter: str, lane, saturating: bool) -> None:
    bits = BITS[letter]
    for index, a_words, b_words, a, b in chunks(letter):
        words, sat = run_model(lane, a, b, bits)
        extra = {"vscr_sat": sat} if saturating else {}
        CASES.append(binop(name, a_words, b_words, words, "boundaries_%d" % index, **extra))


# Modulo arithmetic: a lane wraps, no SAT.
for _op, _sign in (("add", 1), ("sub", -1)):
    for _letter in "bhw":
        _bits = BITS[_letter]
        add_matrix("v%su%sm" % (_op, _letter), _letter,
                   lambda a, b, s=_sign, m=(1 << _bits) - 1: ((a + s * b) & m, False), False)

# Saturating arithmetic.
for _op in ("add", "sub"):
    for _letter in "bhw":
        for _is_signed in (False, True):
            _name = "v%s%s%ss" % (_op, "s" if _is_signed else "u", _letter)
            add_matrix(_name, _letter, model_saturate(_op, _is_signed, BITS[_letter]), True)

for _letter in "bhw":
    for _is_signed in (False, True):
        _tag = ("s" if _is_signed else "u") + _letter
        add_matrix("vavg" + _tag, _letter, model_average(_is_signed, BITS[_letter]), False)
        add_matrix("vmax" + _tag, _letter, model_extreme(max, _is_signed, BITS[_letter]), False)
        add_matrix("vmin" + _tag, _letter, model_extreme(min, _is_signed, BITS[_letter]), False)

for _letter in "bhw":
    for _kind in ("equ", "gtu", "gts"):
        add_matrix("vcmp%s%s" % (_kind, _letter), _letter, model_compare(_kind, BITS[_letter]), False)


# --- literal cases: modulo arithmetic -----------------------------------------------------------

CASES += [
    binop("vadduwm", (0xFFFFFFFF, 1, 0x80000000, 0), (1, 2, 0x80000000, 0xFFFFFFFF),
          (0, 3, 0, 0xFFFFFFFF), "wraps", vscr_sat=0),
    binop("vsubuwm", (0, 1, 0x80000000, 5), (1, 2, 1, 5),
          (0xFFFFFFFF, 0xFFFFFFFF, 0x7FFFFFFF, 0), "wraps", vscr_sat=0),
    # FFFF+1 = 0000, 1+1 = 2, 8000+8000 = 0, 0+FFFF, 0+1, 1234+1, ABCD+FFFF = ABCC.
    binop("vadduhm", (0xFFFF0001, 0x80008000, 0, 0x1234ABCD), (0x00010001, 0x80008000, 0xFFFF0001, 0x0001FFFF),
          (0x00000002, 0, 0xFFFF0001, 0x1235ABCC), "wraps"),
    # 0-1 = FFFF, 1-2 = FFFF, 1234-0234 = 1000, 5678-5679 = FFFF.
    binop("vsubuhm", (0x00000001, 0x12345678, 0, 0), (0x00010002, 0x02345679, 0, 0),
          (0xFFFFFFFF, 0x1000FFFF, 0, 0), "wraps"),
    # Bytes FF01807F + 01FF8001 = 00 00 00 80, F0..: 10+F1 = 01 ... 40+C4 = 04.
    binop("vaddubm", (0xFF01807F, 0, 0x10203040, 0xFFFFFFFF), (0x01FF8001, 0, 0xF1E2D3C4, 0x01010101),
          (0x00000080, 0, 0x01020304, 0), "wraps"),
    # 00-01 = FF, 01-01 = 0, 02-01 = 1, 03-01 = 2; 80-7F = 1, FF-01 = FE, 7F-FF = 80, 10-11 = FF.
    binop("vsububm", (0x00010203, 0x80FF7F10, 0, 0), (0x01010101, 0x7F01FF11, 0, 0),
          (0xFF000102, 0x01FE80FF, 0, 0), "wraps"),
    binop("vadduwm", (1, 2, 3, 4), (10, 20, 30, 40), (11, 22, 33, 44), "leaves_sat_alone",
          init={"vscr_sat": 1}, vscr_sat=1),
]


# --- literal cases: saturating arithmetic -------------------------------------------------------

CASES += [
    # Bytes a = FF 01 80 7F | 00.. | 10 20 30 40 | FF.., b = 01 FF 80 01 | 00.. | F1 E2 D3 C4 | 01..
    # Unsigned: FF+01 -> FF, 01+FF -> FF, 80+80 -> FF, 7F+01 = 80; 10+F1 = 101 -> FF, ...
    binop("vaddubs", (0xFF01807F, 0, 0x10203040, 0xFFFFFFFF), (0x01FF8001, 0, 0xF1E2D3C4, 0x01010101),
          (0xFFFFFF80, 0, 0xFFFFFFFF, 0xFFFFFFFF), "clamps_high", vscr_sat=1),
    # Signed: -1+1 = 0, 1-1 = 0, -128-128 -> 80, 127+1 -> 7F; 16-15 = 1, 32-30 = 2, 48-45 = 3,
    # 64-60 = 4; -1+1 = 0.
    binop("vaddsbs", (0xFF01807F, 0, 0x10203040, 0xFFFFFFFF), (0x01FF8001, 0, 0xF1E2D3C4, 0x01010101),
          (0x0000807F, 0, 0x01020304, 0), "clamps_both_ways", vscr_sat=1),
    # 0001-0002 -> 0, 0005-0003 = 2, FFFF-0001 = FFFE, 8000-8000 = 0, 1234-1234 = 0,
    # 0000-0001 -> 0, 0002-0004 -> 0, 0003-0003 = 0.
    binop("vsubuhs", (0x00010005, 0xFFFF8000, 0x12340000, 0x00020003),
          (0x00020003, 0x00018000, 0x12340001, 0x00040003),
          (0x00000002, 0xFFFE0000, 0, 0), "clamps_low", vscr_sat=1),
    # 7FFFFFFF+1 -> 7FFFFFFF, -2^31 + -1 -> 80000000, 1+2 = 3, -1 + -1 = -2.
    binop("vaddsws", (0x7FFFFFFF, 0x80000000, 1, 0xFFFFFFFF), (1, 0xFFFFFFFF, 2, 0xFFFFFFFF),
          (0x7FFFFFFF, 0x80000000, 3, 0xFFFFFFFE), "clamps_both_ways", vscr_sat=1),
    # -2^31 - 1 -> 80000000, 2^31-1 - -1 -> 7FFFFFFF, 5-2 = 3, 0.
    binop("vsubsws", (0x80000000, 0x7FFFFFFF, 5, 0), (1, 0xFFFFFFFF, 2, 0),
          (0x80000000, 0x7FFFFFFF, 3, 0), "clamps_both_ways", vscr_sat=1),
    binop("vadduws", (0xFFFFFFFF, 1, 0x80000000, 0), (1, 2, 0x80000000, 0),
          (0xFFFFFFFF, 3, 0xFFFFFFFF, 0), "clamps_high", vscr_sat=1),
    binop("vsubuws", (0, 5, 0x80000000, 3), (1, 3, 0x80000000, 3), (0, 2, 0, 0), "clamps_low",
          vscr_sat=1),
]

# In range: SAT stays clear, and an earlier SAT is not cleared (it is sticky).
for _name, _a, _b, _sum in (("vaddubs", 1, 2, 3), ("vadduhs", 1, 2, 3), ("vadduws", 1, 2, 3),
                            ("vaddsbs", 1, 2, 3), ("vaddshs", 1, 2, 3), ("vaddsws", 1, 2, 3),
                            ("vsububs", 5, 2, 3), ("vsubuhs", 5, 2, 3), ("vsubuws", 5, 2, 3),
                            ("vsubsbs", 5, 2, 3), ("vsubshs", 5, 2, 3), ("vsubsws", 5, 2, 3)):
    _bits = BITS[_name[-2]]
    _lanes = 128 // _bits
    _in = (to_words([_a] * _lanes, _bits), to_words([_b] * _lanes, _bits),
           to_words([_sum] * _lanes, _bits))
    CASES += [
        binop(_name, _in[0], _in[1], _in[2], "in_range_leaves_sat_clear", vscr_sat=0),
        binop(_name, _in[0], _in[1], _in[2], "sat_stays_set", init={"vscr_sat": 1}, vscr_sat=1),
    ]


# --- literal cases: carry, average, min and max -------------------------------------------------

CASES += [
    # Carry out: FFFFFFFF+1 yes, 80000000+80000000 yes, 1+FFFFFFFE = FFFFFFFF no, 0+FFFFFFFF no.
    binop("vaddcuw", (0xFFFFFFFF, 0x80000000, 1, 0), (1, 0x80000000, 0xFFFFFFFE, 0xFFFFFFFF),
          (1, 1, 0, 0), "carry_lanes"),
    # No borrow when a >= b: 5>=5, 5<6 no, 0>=0, FFFFFFFF>=0.
    binop("vsubcuw", (5, 5, 0, 0xFFFFFFFF), (5, 6, 0, 0), (1, 0, 1, 1), "no_borrow_lanes"),
    # Rounds up: (1,2) -> 2, (2,2) -> 2, (3,2) -> 3, (4,2) -> 3; FF,FF -> FF; (0,1) -> 1;
    # (FF,FE) -> FF; (7F,80) -> 80; (7F,01) -> 40; (80,00) -> 40; (01,01) -> 1.
    binop("vavgub", (0x01020304, 0xFF00FF00, 0x7F7F8080, 0x00010203),
          (0x02020202, 0xFF01FE01, 0x80017F00, 0x00010203),
          (0x02020303, 0xFF01FF01, 0x80408040, 0x00010203), "rounds_up"),
    # Signed: (-1,-2) -> -1, (-128,-128) -> -128, (127,127) -> 127, (0,-1) -> 0 (rounds toward +inf);
    # (3,4) -> 4, (-3,-4) -> -3, (0,0) -> 0.
    binop("vavgsb", (0xFF807F00, 0x03FD0000, 0, 0), (0xFE807FFF, 0x04FC0000, 0, 0),
          (0xFF807F00, 0x04FD0000, 0, 0), "rounds_toward_plus_infinity"),
    binop("vmaxsw", (0x80000000, 1, 0xFFFFFFFF, 0x7FFFFFFF), (0, 0xFFFFFFFF, 0xFFFFFFFE, 0x80000000),
          (0, 1, 0xFFFFFFFF, 0x7FFFFFFF), "signed_order"),
    binop("vminsw", (0x80000000, 1, 0xFFFFFFFF, 0x7FFFFFFF), (0, 0xFFFFFFFF, 0xFFFFFFFE, 0x80000000),
          (0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 0x80000000), "signed_order"),
    binop("vmaxuw", (0x80000000, 1, 0xFFFFFFFF, 0x7FFFFFFF), (0, 0xFFFFFFFF, 0xFFFFFFFE, 0x80000000),
          (0x80000000, 0xFFFFFFFF, 0xFFFFFFFF, 0x80000000), "unsigned_order"),
    binop("vminuw", (0x80000000, 1, 0xFFFFFFFF, 0x7FFFFFFF), (0, 0xFFFFFFFF, 0xFFFFFFFE, 0x80000000),
          (0, 1, 0xFFFFFFFE, 0x7FFFFFFF), "unsigned_order"),
]


# --- literal cases: compares and CR6 ------------------------------------------------------------

P = (0x01122334, 0x45566778, 0x899AABBC, 0xCDDEEFF0)
ONES = (0xFFFFFFFF,) * 4

CASES += [
    # -1 > 0 no, 1 > -1 yes, -2^31 > 2^31-1 no, 5 > 5 no; unsigned: yes, no, yes, no.
    binop("vcmpgtsw", (0xFFFFFFFF, 1, 0x80000000, 5), (0, 0xFFFFFFFF, 0x7FFFFFFF, 5),
          (0, 0xFFFFFFFF, 0, 0), "signed_order"),
    binop("vcmpgtuw", (0xFFFFFFFF, 1, 0x80000000, 5), (0, 0xFFFFFFFF, 0x7FFFFFFF, 5),
          (0xFFFFFFFF, 0, 0xFFFFFFFF, 0), "unsigned_order"),
    # The plain form leaves CR6 alone.
    binop("vcmpequw", P, P, ONES, "without_rc_keeps_cr6", init={"cr6": 5}, cr6=5),
    binop("vcmpequw", P, P, ONES, "rc_all_true_same_words", init={"cr6": 5}, rc=True, cr6=8),
    binop("vcmpequw", (1, 2, 3, 4), (5, 6, 7, 8), (0,) * 4, "rc_none_true_distinct_words", init={"cr6": 5}, rc=True,
          cr6=2),
    binop("vcmpequw", (1, 2, 3, 4), (1, 0, 3, 0), (0xFFFFFFFF, 0, 0xFFFFFFFF, 0), "rc_mixed_words",
          init={"cr6": 5}, rc=True, cr6=0),
    # A single true lane anywhere (here the last byte) is neither all-true nor none-true.
    binop("vcmpequb", (0x01020304, 0x05060708, 0x090A0B0C, 0x0D0E0F10),
          (0x00000000, 0x00000000, 0x00000000, 0x00000010),
          (0, 0, 0, 0x000000FF), "rc_single_true_lane", rc=True, cr6=0),
]

# Rc form of every compare: all lanes true, no lane true, a mix (the boundary chunk).
for _kind in ("equ", "gtu", "gts"):
    for _letter in "bhw":
        _name = "vcmp%s%s" % (_kind, _letter)
        _bits = BITS[_letter]
        _lanes = 128 // _bits
        _ones = to_words([(1 << _bits) - 1] * _lanes, _bits)
        _zero = (0,) * 4
        _one = to_words([1] * _lanes, _bits)
        # eq: a == b everywhere or nowhere; gt: 1 > 0 everywhere or 0 > 1 nowhere.
        if _kind == "equ":
            _true_pair, _false_pair = (_one, _one), (_one, _zero)
        else:
            _true_pair, _false_pair = (_one, _zero), (_zero, _one)
        CASES += [
            binop(_name, *_true_pair, _ones, "rc_all_true", init={"cr6": 5}, rc=True, cr6=8),
            binop(_name, *_false_pair, _zero, "rc_none_true", init={"cr6": 5}, rc=True, cr6=2),
        ]
        _index, _a_words, _b_words, _a, _b = next(chunks(_letter))
        _words, _ = run_model(model_compare(_kind, _bits), _a, _b, _bits)
        CASES.append(binop(_name, _a_words, _b_words, _words, "rc_mixed", init={"cr6": 5}, rc=True,
                           cr6=0))

# vD may be one of the sources.
CASES += [
    case("vadduwm_destination_is_vra", "VADDUWM", enc("vadduwm", vD=1, vA=1, vB=2),
         {"v1": (1, 2, 3, 4), "v2": (10, 20, 30, 40)}, {"v1": (11, 22, 33, 44), "v2": (10, 20, 30, 40)}),
    case("vaddsws_destination_is_vrb", "VADDSWS", enc("vaddsws", vD=2, vA=1, vB=2),
         {"v1": (0x7FFFFFFF, 0, 0, 0), "v2": (1, 0, 0, 0)}, {"v2": (0x7FFFFFFF, 0, 0, 0), "vscr_sat": 1}),
    case("vcmpequw_rc_destination_is_vra", "VCMPEQUW_", enc("vcmpequw", vD=1, vA=1, vB=2, Rc_vc=1),
         {"v1": (1, 2, 3, 4), "v2": (1, 2, 3, 4)}, {"v1": ONES, "cr6": 8}),
    case("vavgsw_destination_is_vra", "VAVGSW", enc("vavgsw", vD=1, vA=1, vB=2),
         {"v1": (0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 1), "v2": (0xFFFFFFFE, 0x7FFFFFFF, 0x80000000, 2)},
         {"v1": (0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 2)}),
]
