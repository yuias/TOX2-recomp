"""PPU opcode table: integer, branch, load/store, cache and SPR instructions."""

from __future__ import annotations

from dataclasses import replace

from .ppu import OpSpec, op

OPS: list[OpSpec] = []


def _rows(form: str, primary: int, rows: str, operands: str = "", flags: str = "") -> None:
    # `rows` is "name:xo name:xo ..." (xo omitted for primary-only forms).
    ops = tuple(operands.split(",")) if operands else ()
    for item in rows.split():
        name, _, xo = item.partition(":")
        OPS.append(op(name, form, primary, int(xo) if xo else None, ops, tuple(flags.split())))


# A.1 primary opcodes
_rows("D", 2, "tdi", "TO,rA,SIMM")
_rows("D", 3, "twi", "TO,rA,SIMM")
_rows("D", 7, "mulli", "rD,rA,SIMM")
_rows("D", 8, "subfic", "rD,rA,SIMM")
_rows("D", 10, "cmpli", "crfD,L,rA,UIMM")
_rows("D", 11, "cmpi", "crfD,L,rA,SIMM")
_rows("D", 12, "addic", "rD,rA,SIMM")
_rows("D", 13, "addic.", "rD,rA,SIMM")
_rows("D", 14, "addi", "rD,rA,SIMM")
_rows("D", 15, "addis", "rD,rA,SIMM")
_rows("B", 16, "bc", "BO,BI,BD", "aa lk")
_rows("SC", 17, "sc")
_rows("I", 18, "b", "LI", "aa lk")
_rows("M", 20, "rlwimi", "rA,rS,SH,MB,ME", "rc")
_rows("M", 21, "rlwinm", "rA,rS,SH,MB,ME", "rc")
_rows("M", 23, "rlwnm", "rA,rS,rB,MB,ME", "rc")
for _primary, _name in ((24, "ori"), (25, "oris"), (26, "xori"), (27, "xoris"),
                        (28, "andi."), (29, "andis.")):
    _rows("D", _primary, _name, "rA,rS,UIMM")
for _primary, _name in ((32, "lwz"), (33, "lwzu"), (34, "lbz"), (35, "lbzu"), (40, "lhz"),
                        (41, "lhzu"), (42, "lha"), (43, "lhau"), (46, "lmw")):
    _rows("D", _primary, _name, "rD,D(rA)")
for _primary, _name in ((36, "stw"), (37, "stwu"), (38, "stb"), (39, "stbu"), (44, "sth"),
                        (45, "sthu"), (47, "stmw")):
    _rows("D", _primary, _name, "rS,D(rA)")
for _primary, _name in ((48, "lfs"), (49, "lfsu"), (50, "lfd"), (51, "lfdu")):
    _rows("D", _primary, _name, "frD,D(rA)")
for _primary, _name in ((52, "stfs"), (53, "stfsu"), (54, "stfd"), (55, "stfdu")):
    _rows("D", _primary, _name, "frS,D(rA)")

# A.2 opcode 19
_rows("XL", 19, "mcrf:0", "crfD,crfS")
_rows("XL", 19, "bclr:16 bcctr:528", "BO,BI", "lk")
_rows("XL", 19, "crnor:33 crandc:129 crxor:193 crnand:225 crand:257 creqv:289 crorc:417 cror:449",
      "crbD,crbA,crbB")
_rows("XL", 19, "isync:150")

# A.3 opcode 30
_rows("MD", 30, "rldicl:0 rldicr:1 rldic:2 rldimi:3", "rA,rS,SH64,MB64", "rc")
_rows("MDS", 30, "rldcl:8 rldcr:9", "rA,rS,rB,MB64", "rc")

# A.4 opcodes 58 / 62
_rows("DS", 58, "ld:0 ldu:1 lwa:2", "rD,DS(rA)")
_rows("DS", 62, "std:0 stdu:1", "rS,DS(rA)")

# A.5 opcode 31 (scalar rows; vector rows live in ppu_vmx)
_rows("XO", 31, "subfc:8 addc:10 subf:40 subfe:136 adde:138 mulld:233 mullw:235 add:266 "
      "divdu:457 divwu:459 divd:489 divw:491", "rD,rA,rB", "oe rc")
_rows("XO", 31, "neg:104 subfze:200 addze:202 subfme:232 addme:234", "rD,rA", "oe rc")
_rows("XO", 31, "mulhdu:9 mulhwu:11 mulhd:73 mulhw:75", "rD,rA,rB", "rc")
_rows("XS", 31, "sradi:413", "rA,rS,SH64", "rc")

_rows("X", 31, "cmp:0 cmpl:32", "crfD,L,rA,rB")
_rows("X", 31, "tw:4 td:68", "TO,rA,rB")
_rows("X", 31, "and:28 andc:60 nor:124 eqv:284 xor:316 orc:412 or:444 nand:476", "rA,rS,rB", "rc")
_rows("X", 31, "slw:24 sld:27 srw:536 srd:539 sraw:792 srad:794", "rA,rS,rB", "rc")
_rows("X", 31, "srawi:824", "rA,rS,SH", "rc")
_rows("X", 31, "cntlzw:26 cntlzd:58 extsh:922 extsb:954 extsw:986", "rA,rS", "rc")
_rows("X", 31, "lwarx:20 ldx:21 lwzx:23 ldux:53 lwzux:55 ldarx:84 lbzx:87 lbzux:119 lhzx:279 "
      "lhzux:311 lwax:341 lhax:343 lwaux:373 lhaux:375 ldbrx:532 lswx:533 lwbrx:534 lhbrx:790",
      "rD,rA,rB")
_rows("X", 31, "lswi:597", "rD,rA,NB")
_rows("X", 31, "stdx:149 stwcx.:150 stwx:151 stdux:181 stwux:183 stdcx.:214 stbx:215 stbux:247 "
      "sthx:407 sthux:439 stdbrx:660 stswx:661 stwbrx:662 sthbrx:918", "rS,rA,rB")
_rows("X", 31, "stswi:725", "rS,rA,NB")
_rows("X", 31, "lfsx:535 lfsux:567 lfdx:599 lfdux:631", "frD,rA,rB")
_rows("X", 31, "stfsx:663 stfsux:695 stfdx:727 stfdux:759 stfiwx:983", "frS,rA,rB")
_rows("X", 31, "dcbst:54 dcbf:86 dcbtst:246 dcbt:278 icbi:982 dcbz:1014", "rA,rB")
_rows("X", 31, "sync:598", "L_sync")
_rows("X", 31, "eieio:854")
_rows("X", 31, "mfcr:19", "rD")
_rows("XFX", 31, "mtcrf:144", "CRM,rS")
# mfocrf/mtocrf are mfcr/mtcrf with bit 11 set. Their rows have one more mask bit, so the
# lookup tries them first; the plain rows keep matching words with bit 11 clear.
_OCRF_BIT = 1 << (31 - 11)
for _name, _xo, _operands in (("mfocrf", 19, ("rD", "FXM")), ("mtocrf", 144, ("CRM", "rS"))):
    _spec = op(_name, "XFX", 31, _xo, _operands)
    OPS.append(replace(_spec, mask=_spec.mask | _OCRF_BIT, match=_spec.match | _OCRF_BIT))
_rows("XFX", 31, "mfspr:339", "rD,SPR")
_rows("XFX", 31, "mtspr:467", "SPR,rS")
_rows("XFX", 31, "mftb:371", "rD,TBR")

# A.6 opcode 59 (single-precision FP)
_rows("A", 59, "fdivs:18 fsubs:20 fadds:21", "frD,frA,frB", "rc")
_rows("A", 59, "fmuls:25", "frD,frA,frC", "rc")
_rows("A", 59, "fsqrts:22 fres:24", "frD,frB", "rc")
_rows("A", 59, "fmsubs:28 fmadds:29 fnmsubs:30 fnmadds:31", "frD,frA,frC,frB", "rc")

# A.7 opcode 63 (double-precision FP and FPSCR)
_rows("A", 63, "fdiv:18 fsub:20 fadd:21", "frD,frA,frB", "rc")
_rows("A", 63, "fmul:25", "frD,frA,frC", "rc")
_rows("A", 63, "fsqrt:22 frsqrte:26", "frD,frB", "rc")
_rows("A", 63, "fsel:23 fmsub:28 fmadd:29 fnmsub:30 fnmadd:31", "frD,frA,frC,frB", "rc")
_rows("X", 63, "fcmpu:0 fcmpo:32", "crfD,frA,frB")
_rows("X", 63, "mcrfs:64", "crfD,crfS")
_rows("X", 63, "frsp:12 fctiw:14 fctiwz:15 fneg:40 fmr:72 fnabs:136 fabs:264 fctid:814 "
      "fctidz:815 fcfid:846", "frD,frB", "rc")
_rows("X", 63, "mtfsb1:38 mtfsb0:70", "crbD", "rc")
_rows("X", 63, "mtfsfi:134", "crfD,IMM_fp", "rc")
_rows("X", 63, "mffs:583", "frD", "rc")
_rows("X", 63, "mtfsf:711", "FM,frB", "rc")
