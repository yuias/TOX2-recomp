"""Function DB loader, mnemonic groups and the instruction census for the `emit` command."""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from pathlib import Path

from .info import EXIT_IO, EXIT_PIN_MISMATCH, CliError
from .ppu_flow import CodeImage

SCHEMA = 1
CENSUS_SCHEMA = 1
CENSUS_TOP = 15


@dataclass(frozen=True)
class DbFunction:
    entry: int
    name: str | None
    ranges: tuple[tuple[int, int], ...]   # code words only (jump-table words are excluded)

    def addrs(self) -> list[int]:
        return [a for start, end in self.ranges for a in range(start, end, 4)]


@dataclass(frozen=True)
class DbThunk:
    addr: int
    module: str
    fnid: int
    name: str | None
    slot: int


@dataclass(frozen=True)
class DbJumpTable:
    branch: int
    table: int
    kind: str
    count: int
    targets: tuple[int, ...]


@dataclass(frozen=True)
class FunctionDb:
    sha256: str                                 # SHA-256 of the executable the DB was made from
    file_sha256: str                            # SHA-256 of the DB file bytes
    text: tuple[int, int]
    code: tuple[int, int]                       # range the functions were recovered in
    toc: int | None
    functions: tuple[DbFunction, ...]           # sorted by entry
    thunks: tuple[DbThunk, ...]                 # sorted by addr
    jump_tables: dict[int, DbJumpTable]         # by branch


# Handlers for group `g` live in `emit_ops_<g>.py`, except `control` (in `emit_core.py`).
GROUPS: dict[str, tuple[str, ...]] = {
    "control": tuple("b bc bclr bcctr sc tw twi td tdi".split()),
    "int_alu": tuple(
        "addi addis add subf neg and andc or orc xor nor nand eqv andi. andis. ori oris xori "
        "xoris extsb extsh extsw cntlzw cntlzd cmp cmpi cmpl cmpli".split()),
    "int_carry_muldiv": tuple(
        "addic addic. addc adde addze addme subfc subfe subfze subfme subfic mulli mullw mulld "
        "mulhw mulhwu mulhd mulhdu divw divwu divd divdu".split()),
    "int_rotate": tuple(
        "rlwinm rlwimi rlwnm rldicl rldicr rldic rldimi rldcl rldcr slw srw sraw srawi sld srd "
        "srad sradi".split()),
    "mem": tuple(
        "lbz lbzu lbzx lbzux lhz lhzu lhzx lhzux lha lhau lhax lhaux lwz lwzu lwzx lwzux lwa "
        "lwax lwaux ld ldu ldx ldux stb stbu stbx stbux sth sthu sthx sthux stw stwu stwx stwux "
        "std stdu stdx stdux lwbrx lhbrx ldbrx stwbrx sthbrx stdbrx lmw stmw lswi lswx stswi "
        "stswx lfs lfsu lfsx lfsux lfd lfdu lfdx lfdux stfs stfsu stfsx stfsux stfd stfdu stfdx "
        "stfdux stfiwx".split()),
    "sys": tuple(
        "mfspr mtspr mftb mfcr mtcrf mfocrf mtocrf mcrf crand crandc creqv crnand crnor cror "
        "crorc crxor sync eieio isync lwarx ldarx stwcx. stdcx. dcbz dcbt dcbtst dcbf dcbst "
        "icbi dst dstst dss".split()),
    "fp": tuple(
        "fadd fadds fsub fsubs fmul fmuls fdiv fdivs fsqrt fsqrts fres frsqrte fmadd fmadds "
        "fmsub fmsubs fnmadd fnmadds fnmsub fnmsubs fsel frsp fctiw fctiwz fctid fctidz fcfid "
        "fcmpu fcmpo fneg fabs fnabs fmr mffs mtfsf mtfsfi mtfsb0 mtfsb1 mcrfs".split()),
    "vmx_mem": tuple(
        "lvx lvxl stvx stvxl lvebx lvehx lvewx stvebx stvehx stvewx lvsl lvsr lvlx lvrx lvlxl "
        "lvrxl stvlx stvrx stvlxl stvrxl vand vandc vor vxor vnor vsel mfvscr mtvscr".split()),
    "vmx_perm": tuple(
        "vperm vsldoi vmrghb vmrghh vmrghw vmrglb vmrglh vmrglw vspltb vsplth vspltw vspltisb "
        "vspltish vspltisw vpkuhum vpkuwum vpkuhus vpkuwus vpkshus vpkswus vpkshss vpkswss "
        "vpkpx vupkhsb vupkhsh vupklsb vupklsh vupkhpx vupklpx vsl vsr vslo vsro".split()),
    "vmx_int": tuple(
        "vaddubm vadduhm vadduwm vaddcuw vaddubs vadduhs vadduws vaddsbs vaddshs vaddsws vsububm "
        "vsubuhm vsubuwm vsubcuw vsububs vsubuhs vsubuws vsubsbs vsubshs vsubsws vavgub vavguh "
        "vavguw vavgsb vavgsh vavgsw vmaxub vmaxuh vmaxuw vmaxsb vmaxsh vmaxsw vminub vminuh "
        "vminuw vminsb vminsh vminsw vcmpequb vcmpequh vcmpequw vcmpgtub vcmpgtuh vcmpgtuw "
        "vcmpgtsb vcmpgtsh vcmpgtsw".split()),
    "vmx_mul": tuple(
        "vrlb vrlh vrlw vslb vslh vslw vsrb vsrh vsrw vsrab vsrah vsraw vmuloub vmulouh vmulosb "
        "vmulosh vmuleub vmuleuh vmulesb vmulesh vmsumubm vmsummbm vmsumuhm vmsumuhs vmsumshm "
        "vmsumshs vmhaddshs vmhraddshs vmladduhm vsum4ubs vsum4sbs vsum4shs vsum2sws "
        "vsumsws".split()),
    "vmx_fp": tuple(
        "vaddfp vsubfp vmaddfp vnmsubfp vmaxfp vminfp vrefp vrsqrtefp vexptefp vlogefp vrfin "
        "vrfiz vrfip vrfim vcfux vcfsx vctuxs vctsxs vcmpeqfp vcmpgefp vcmpgtfp "
        "vcmpbfp".split()),
}

GROUP_OF: dict[str, str] = {name: group for group, names in GROUPS.items() for name in names}

# Checked at import so a table edit that puts one name in two groups fails immediately.
if len(GROUP_OF) != sum(len(names) for names in GROUPS.values()):
    raise ValueError("a mnemonic appears in more than one emitter group")


def _parse_hex(value) -> int:
    if not isinstance(value, str):
        raise ValueError("expected a hex string, got %r" % (value,))
    return int(value, 16)


def _parse_db(doc: dict, file_sha256: str) -> FunctionDb:
    if doc["schema"] != SCHEMA:
        raise ValueError("unsupported schema %r" % (doc["schema"],))
    text = (_parse_hex(doc["text"]["start"]), _parse_hex(doc["text"]["end"]))
    code = (_parse_hex(doc["code"]["start"]), _parse_hex(doc["code"]["end"]))
    toc = doc["toc"]
    functions = []
    for item in doc["functions"]:
        ranges = tuple((_parse_hex(a), _parse_hex(b)) for a, b in item["ranges"])
        for start, end in ranges:
            if start >= end or start % 4 or end % 4:
                raise ValueError("bad range 0x%08x-0x%08x" % (start, end))
        functions.append(DbFunction(_parse_hex(item["entry"]), item["name"], ranges))
    functions.sort(key=lambda f: f.entry)
    spans = sorted(span for fn in functions for span in fn.ranges)
    for (_, prev_end), (next_start, _) in zip(spans, spans[1:]):
        if next_start < prev_end:
            raise ValueError("overlapping function ranges at 0x%08x" % next_start)
    thunks = sorted((DbThunk(_parse_hex(t["addr"]), t["module"], _parse_hex(t["fnid"]),
                             t["name"], _parse_hex(t["slot"]))
                     for t in doc["import_thunks"]), key=lambda t: t.addr)
    tables = {}
    for t in doc["jump_tables"]:
        table = DbJumpTable(_parse_hex(t["branch"]), _parse_hex(t["table"]), t["kind"],
                            int(t["count"]), tuple(_parse_hex(x) for x in t["targets"]))
        tables[table.branch] = table
    return FunctionDb(doc["input"]["executable_sha256"].lower(), file_sha256, text, code,
                      None if toc is None else _parse_hex(toc), tuple(functions),
                      tuple(thunks), tables)


def load_function_db(path: Path) -> FunctionDb:
    try:
        raw = Path(path).read_bytes()
    except OSError as e:
        raise CliError(EXIT_IO, "cannot read function DB %s: %s" % (path, e.strerror))
    try:
        doc = json.loads(raw.decode("utf-8"))
        return _parse_db(doc, hashlib.sha256(raw).hexdigest())
    except (ValueError, KeyError, TypeError, AttributeError) as e:
        # KeyError's str() is the repr of the key, which reads fine as "missing key 'x'".
        detail = "missing key %s" % e if isinstance(e, KeyError) else str(e)
        raise CliError(EXIT_IO, "invalid function DB %s: %s" % (path, detail))


def check_db_matches(db: FunctionDb, elf_sha256: str, text: tuple[int, int],
                     code: tuple[int, int]) -> None:
    if db.sha256 != elf_sha256.lower():
        raise CliError(EXIT_PIN_MISMATCH,
                       "function DB was made from a different ELF (SHA-256 %s, expected %s)"
                       % (db.sha256, elf_sha256.lower()))
    if tuple(db.text) != tuple(text):
        raise CliError(EXIT_PIN_MISMATCH,
                       "function DB text range 0x%08x-0x%08x differs from the ELF's "
                       "0x%08x-0x%08x" % (*db.text, *text))
    if tuple(db.code) != tuple(code):
        raise CliError(EXIT_PIN_MISMATCH,
                       "function DB code range 0x%08x-0x%08x differs from the ELF's "
                       "0x%08x-0x%08x" % (*db.code, *code))


@dataclass(frozen=True)
class Census:
    words: int                          # owned code words
    by_name: dict[str, int]             # strictly valid owned words by spec name
    invalid_owned: tuple[int, ...]      # owned addresses that do not decode strictly


def take_census(image: CodeImage, db: FunctionDb) -> Census:
    by_name: dict[str, int] = {}
    invalid: list[int] = []
    words = 0
    for fn in db.functions:
        for addr in fn.addrs():
            words += 1
            if not image.valid(addr):
                invalid.append(addr)
                continue
            name = image.insn(addr).spec.name
            by_name[name] = by_name.get(name, 0) + 1
    return Census(words, by_name, tuple(sorted(invalid)))


def _sorted_counts(by_name: dict[str, int]) -> list[tuple[str, int]]:
    return sorted(by_name.items(), key=lambda kv: (-kv[1], kv[0]))


def census_doc(census: Census) -> dict:
    total = census.words
    ranked = _sorted_counts(census.by_name)
    mnemonics = []
    running = 0
    for name, count in ranked:
        running += count
        mnemonics.append({"name": name, "group": GROUP_OF.get(name), "count": count,
                          "pct": round(100.0 * count / total, 2) if total else 0.0,
                          "cum_pct": round(100.0 * running / total, 2) if total else 0.0})
    by_group = {}
    for group, names in GROUPS.items():
        counts = {n: census.by_name[n] for n in names if n in census.by_name}
        by_group[group] = {"words": sum(counts.values()),
                           "mnemonics": [[n, c] for n, c in _sorted_counts(counts)]}
    return {
        "schema": CENSUS_SCHEMA,
        "words": total,
        "distinct": len(census.by_name),
        "invalid_owned": ["0x%08x" % a for a in census.invalid_owned],
        "mnemonics": mnemonics,
        "by_group": by_group,
    }


def census_lines(census: Census) -> list[str]:
    doc = census_doc(census)
    top = ", ".join("%s %d (%.1f%%)" % (m["name"], m["count"], m["pct"])
                    for m in doc["mnemonics"][:CENSUS_TOP])
    lines = ["census: %d words, %d distinct mnemonics, %d invalid owned"
             % (doc["words"], doc["distinct"], len(doc["invalid_owned"])),
             "top: " + top]
    for group, info in doc["by_group"].items():
        lines.append("group %s: %d words, %d mnemonics: %s"
                     % (group, info["words"], len(info["mnemonics"]),
                        ", ".join("%s %d" % (n, c) for n, c in info["mnemonics"])))
    return lines
