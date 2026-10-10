"""PPU VMX/AltiVec opcode table: opcode 4 and the vector rows of opcode 31."""

from __future__ import annotations

from .ppu import OpSpec, op

OPS: list[OpSpec] = []


def _rows(form: str, primary: int, rows: str, operands: str = "", flags: str = "") -> None:
    # `rows` is "name:xo name:xo ...".
    ops = tuple(operands.split(",")) if operands else ()
    for item in rows.split():
        name, _, xo = item.partition(":")
        OPS.append(op(name, form, primary, int(xo), ops, tuple(flags.split())))


# A.5 opcode 31 vector rows
_rows("X", 31, "lvsl:6 lvebx:7 lvsr:38 lvehx:39 lvewx:71 lvx:103 lvxl:359 lvlx:519 lvrx:551 "
      "lvlxl:775 lvrxl:807", "vD,rA,rB")
_rows("X", 31, "stvebx:135 stvehx:167 stvewx:199 stvx:231 stvxl:487 stvlx:647 stvrx:679 "
      "stvlxl:903 stvrxl:935", "vS,rA,rB")
_rows("X", 31, "dst:342 dstst:374", "rA,rB,STRM")
_rows("X", 31, "dss:822", "STRM")

# A.8 opcode 4: VA form
_rows("VA", 4, "vmhaddshs:32 vmhraddshs:33 vmladduhm:34 vmsumubm:36 vmsummbm:37 vmsumuhm:38 "
      "vmsumuhs:39 vmsumshm:40 vmsumshs:41 vsel:42 vperm:43", "vD,vA,vB,vC")
_rows("VA", 4, "vsldoi:44", "vD,vA,vB,SHB")
# Assembly order differs from field order (vB and vC swapped).
_rows("VA", 4, "vmaddfp:46 vnmsubfp:47", "vD,vA,vC,vB")

# A.8 opcode 4: VC form (compares)
_rows("VC", 4, "vcmpequb:6 vcmpequh:70 vcmpequw:134 vcmpeqfp:198 vcmpgefp:454 vcmpgtub:518 "
      "vcmpgtuh:582 vcmpgtuw:646 vcmpgtfp:710 vcmpgtsb:774 vcmpgtsh:838 vcmpgtsw:902 "
      "vcmpbfp:966", "vD,vA,vB", "rc_vc")

# A.8 opcode 4: VX form
_rows("VX", 4,
      "vaddubm:0 vmaxub:2 vrlb:4 vmuloub:8 vaddfp:10 vmrghb:12 vpkuhum:14 "
      "vadduhm:64 vmaxuh:66 vrlh:68 vmulouh:72 vsubfp:74 vmrghh:76 vpkuwum:78 "
      "vadduwm:128 vmaxuw:130 vrlw:132 vmrghw:140 vpkuhus:142 vpkuwus:206 "
      "vmaxsb:258 vslb:260 vmulosb:264 vmrglb:268 vpkshus:270 "
      "vmaxsh:322 vslh:324 vmulosh:328 vmrglh:332 vpkswus:334 "
      "vaddcuw:384 vmaxsw:386 vslw:388 vmrglw:396 vpkshss:398 "
      "vsl:452 vpkswss:462 "
      "vaddubs:512 vminub:514 vsrb:516 vmuleub:520 "
      "vadduhs:576 vminuh:578 vsrh:580 vmuleuh:584 "
      "vadduws:640 vminuw:642 vsrw:644 "
      "vsr:708 "
      "vaddsbs:768 vminsb:770 vsrab:772 vmulesb:776 vpkpx:782 "
      "vaddshs:832 vminsh:834 vsrah:836 vmulesh:840 "
      "vaddsws:896 vminsw:898 vsraw:900 "
      "vsububm:1024 vavgub:1026 vand:1028 vmaxfp:1034 vslo:1036 "
      "vsubuhm:1088 vavguh:1090 vandc:1092 vminfp:1098 vsro:1100 "
      "vsubuwm:1152 vavguw:1154 vor:1156 "
      "vxor:1220 vavgsb:1282 vnor:1284 vavgsh:1346 "
      "vsubcuw:1408 vavgsw:1410 "
      "vsububs:1536 vsum4ubs:1544 vsubuhs:1600 vsum4shs:1608 vsubuws:1664 vsum2sws:1672 "
      "vsubsbs:1792 vsum4sbs:1800 vsubshs:1856 vsubsws:1920 vsumsws:1928",
      "vD,vA,vB")
_rows("VX", 4,
      "vrefp:266 vrsqrtefp:330 vexptefp:394 vlogefp:458 vrfin:522 vrfiz:586 vrfip:650 "
      "vrfim:714 vupkhsb:526 vupkhsh:590 vupklsb:654 vupklsh:718 vupkhpx:846 vupklpx:974",
      "vD,vB")
_rows("VX", 4, "vspltb:524 vsplth:588 vspltw:652 vcfux:778 vcfsx:842 vctuxs:906 vctsxs:970",
      "vD,vB,UIMM5")
_rows("VX", 4, "vspltisb:780 vspltish:844 vspltisw:908", "vD,SIMM5")
_rows("VX", 4, "mfvscr:1540", "vD")
_rows("VX", 4, "mtvscr:1604", "vB")
