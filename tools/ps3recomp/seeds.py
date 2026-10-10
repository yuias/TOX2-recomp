"""Function-start seeds (entry, exports, OPD descriptors, `bl` targets, ...) and import thunks."""

from __future__ import annotations

import struct
from collections.abc import Collection
from dataclasses import dataclass

from . import cell
from .elf import PF_X, ElfFile
from .ghidra_summary import GhidraSummary
from .nids import NidTable
from .ppu_flow import CodeImage, eval_const, flow_of

SEED_SOURCES = ("entry", "export", "opd", "ghidra", "bl", "ghidra_data_ref", "data_ptr",
                "prologue", "manual", "call", "promoted")   # last two are added by partition

THUNK_SIZE = 32
_THUNK_WORDS = THUNK_SIZE // 4

_MFLR_R0 = 0x7C0802A6
_NOP = 0x60000000
# A non-entry TOC base is believed only when this many descriptors name it; stray words that
# look like `{func, toc}` occur a handful of times, real OPD tables thousands.
_MIN_TOC_DESCRIPTORS = 16
# Flow kinds after which the next word cannot be reached by falling through.
_BOUNDARY_KINDS = frozenset({"jump", "return", "indirect_jump", "trap"})


@dataclass(frozen=True)
class Seed:
    addr: int
    source: str


@dataclass(frozen=True)
class ImportThunk:
    addr: int
    module: str
    fnid: int
    name: str | None
    slot_addr: int


@dataclass(frozen=True)
class SeedCensus:
    seeds: tuple[Seed, ...]                        # sorted by (addr, source), unique
    bl_sites: dict[int, list[int]]
    rejected: tuple[tuple[int, str, str], ...]     # (addr, source, reason)
    counts: dict[str, int]


def find_import_thunks(elf: ElfFile, nid_table: NidTable | None) -> dict[int, ImportThunk]:
    """Map each import's thunk address (its `slot_value`) to the import it stands for."""
    prx = cell.library_tables(elf)
    if prx is None:
        return {}
    thunks: dict[int, ImportThunk] = {}
    for module in cell.parse_imports(elf, prx):
        if module.unknown_layout:
            continue
        for func in module.functions:
            name = None if nid_table is None else nid_table.lookup(module.name, func.fnid)
            thunks.setdefault(func.slot_value, ImportThunk(
                func.slot_value, module.name, func.fnid, name, func.slot_addr))
    return thunks


def stub_range(thunks: dict[int, ImportThunk]) -> tuple[int, int] | None:
    """`[min thunk, max thunk + 32)`, or None without imports."""
    if not thunks:
        return None
    return min(thunks), max(thunks) + THUNK_SIZE


def check_thunk_slots(elf: ElfFile, thunks: dict[int, ImportThunk]) -> list[int]:
    """Thunk addresses whose code does not load the import's own slot."""
    rng = stub_range(thunks)
    if rng is None:
        return []
    image = CodeImage.from_elf(elf, rng)
    # The entry TOC is enough: thunks build the slot address with addi/oris and save r2 before
    # loading the callee's, so no slot load goes through r2 and a TOC set would not change the
    # verdict.
    toc = cell.parse_entry(elf).toc
    bad = []
    for addr in sorted(thunks):
        if not _thunk_loads_slot(image, addr, thunks[addr].slot_addr, toc):
            bad.append(addr)
    return bad


def _thunk_loads_slot(image: CodeImage, addr: int, slot_addr: int, toc: int | None) -> bool:
    for a in range(addr, addr + THUNK_SIZE, 4):
        insn = image.insn(a)
        if insn.spec is None or insn.spec.name != "lwz":
            continue
        ra = insn.field("rA")
        base = 0 if ra == 0 else eval_const(image, a, ra, toc=toc)
        if base is None:
            # Not a slot load (e.g. the descriptor read through the loaded slot value).
            continue
        return (base + insn.field("D")) & 0xFFFFFFFF == slot_addr
    return False


def _scan_descriptors(elf: ElfFile, image: CodeImage) -> list[tuple[int, int, int]]:
    """(location, func in image, toc word) for aligned pairs in non-executable loaded data.

    Executables align descriptors to 8; a PRX's sit at 4 mod 8 (libsre's do), so its scan steps by 4.
    """
    step = 4 if cell.is_prx(elf) else 8
    found = []
    for seg in elf.loads():
        if seg.p_flags & PF_X or seg.p_filesz < 8:
            continue
        first = -seg.p_vaddr % step
        raw = elf.read_va(seg.p_vaddr, seg.p_filesz)
        for off in range(first, len(raw) - 7, step):
            func, desc_toc = struct.unpack_from(">II", raw, off)
            if image.contains(func):
                found.append((seg.p_vaddr + off, func, desc_toc))
    found.sort()
    return found


def collect_tocs(elf: ElfFile, image: CodeImage) -> tuple[int, ...]:
    """The TOC bases the executable's descriptors use: the entry TOC first, then the others.

    The executable may carry no `.opd` section header, so the descriptor scan finds them. An
    executable without an entry TOC has no set. A non-entry base must be aligned, lie in non-executable loaded memory and be named by at
    least `_MIN_TOC_DESCRIPTORS` descriptors.
    """
    entry_toc = cell.parse_entry(elf).toc
    if entry_toc is None:
        return ()
    counts: dict[int, int] = {}
    for _, _, desc_toc in _scan_descriptors(elf, image):
        counts[desc_toc] = counts.get(desc_toc, 0) + 1
    data = [seg for seg in elf.loads() if not seg.p_flags & PF_X]
    others = sorted(
        t for t, n in counts.items()
        if t != entry_toc and t % 8 == 0 and n >= _MIN_TOC_DESCRIPTORS
        and any(seg.p_vaddr <= t < seg.p_vaddr + seg.p_memsz for seg in data))
    return (entry_toc, *others)


def find_opd_descriptors(elf: ElfFile, image: CodeImage, toc: int | Collection[int]
                         ) -> list[tuple[int, int]]:
    """Function descriptors `{func in image, toc}` in non-executable loaded file data.

    `toc` is one TOC base or the executable's TOC set.
    """
    tocs = {toc} if isinstance(toc, int) else set(toc)
    return [(loc, func) for loc, func, desc_toc in _scan_descriptors(elf, image)
            if desc_toc in tocs]


def function_tocs(elf: ElfFile, image: CodeImage, tocs: Collection[int]) -> list[tuple[int, int]]:
    """(function start, TOC) for every descriptor naming one of `tocs`, sorted."""
    wanted = set(tocs)
    return sorted({(func, desc_toc) for _, func, desc_toc in _scan_descriptors(elf, image)
                   if desc_toc in wanted})


def scan_bl_sites(image: CodeImage) -> dict[int, list[int]]:
    """Map every `bl` target (I-form branch with LK=1) to the sites that call it."""
    sites: dict[int, list[int]] = {}
    for i, insn in enumerate(image.insns):
        spec = insn.spec
        if spec is None or spec.name != "b" or not insn.field("LK"):
            continue
        flow = flow_of(insn)
        if flow.kind != "call":
            continue
        sites.setdefault(flow.target, []).append(image.start + 4 * i)
    return sites


def scan_data_pointers(elf: ElfFile, image: CodeImage, exclude: set[int],
                       skip: tuple[int, int] | None = None) -> list[tuple[int, int]]:
    """(location, target) for aligned words outside the code image and `skip` that point into it.

    `exclude` holds locations to ignore (OPD function fields). `skip` is normally the import
    stub range, whose words are instructions rather than pointers.
    """
    found = []
    for seg in elf.loads():
        if seg.p_filesz < 4:
            continue
        first = -seg.p_vaddr % 4
        raw = elf.read_va(seg.p_vaddr, seg.p_filesz)
        count = (len(raw) - first) // 4
        values = struct.unpack_from(">%dI" % count, raw, first)
        base = seg.p_vaddr + first
        for i, value in enumerate(values):
            if not image.contains(value):
                continue
            loc = base + 4 * i
            if image.start <= loc < image.end or loc in exclude:
                continue
            if skip is not None and skip[0] <= loc < skip[1]:
                continue
            found.append((loc, value))
    found.sort()
    return found


def is_prologue(image: CodeImage, addr: int) -> bool:
    """`stdu r1,-N(r1)` (N > 0) or `mflr r0`."""
    if not image.contains(addr):
        return False
    insn = image.insn(addr)
    if insn.spec is None:
        return False
    if insn.word == _MFLR_R0:
        return True
    return (insn.spec.name == "stdu" and insn.field("rS") == 1 and insn.field("rA") == 1
            and insn.field("DS") < 0)


def prologue_candidates(image: CodeImage) -> list[int]:
    """Prologues preceded by padding or by a word that ends straight-line flow."""
    found = []
    for addr in range(image.start + 4, image.end, 4):
        if not is_prologue(image, addr):
            continue
        prev = image.word(addr - 4)
        if prev in (0, _NOP) or flow_of(image.insn(addr - 4)).kind in _BOUNDARY_KINDS:
            found.append(addr)
    return found


def collect_recovery_seeds(elf: ElfFile, image: CodeImage, summary: GhidraSummary | None,
                           thunks: dict[int, ImportThunk], *, jump_table_targets: set[int],
                           extra: list[int], prologue: bool,
                           tocs: Collection[int] | None = None) -> SeedCensus:
    entry = cell.parse_entry(elf)
    prx = cell.library_tables(elf)

    # A PRX has no entry function; its exports and descriptors seed it.
    candidates: set[tuple[int, str]] = set()
    if entry.func is not None:
        candidates.add((entry.func, "entry"))
    if prx is not None:
        for module in cell.parse_exports(elf, prx):
            for func in module.functions:
                if func.func is not None:
                    candidates.add((func.func, "export"))

    if tocs is None:
        tocs = collect_tocs(elf, image)
    opd = find_opd_descriptors(elf, image, tocs)
    opd_funcs = {func for _, func in opd}
    candidates.update((func, "opd") for func in opd_funcs)

    ghidra_all: set[int] = set()
    ghidra_data_refs: set[int] = set()
    if summary is not None:
        ghidra_all = {f.entry for f in summary.functions}
        candidates.update((a, "ghidra") for a in ghidra_all)
        ghidra_data_refs = {to for _, to, _, from_exec in summary.data_refs if not from_exec}

    bl_sites = scan_bl_sites(image)
    candidates.update((a, "bl") for a in bl_sites)

    ptr_targets = {t for _, t in scan_data_pointers(
        elf, image, {desc for desc, _ in opd}, stub_range(thunks))}
    data_ptr_seeds = {a for a in ptr_targets
                      if is_prologue(image, a) and a not in jump_table_targets}
    candidates.update((a, "data_ptr") for a in data_ptr_seeds)

    ref_seeds = {a for a in ghidra_data_refs
                 if is_prologue(image, a) and a not in jump_table_targets}
    candidates.update((a, "ghidra_data_ref") for a in ref_seeds)

    prologues = prologue_candidates(image)
    if prologue:
        candidates.update((a, "prologue") for a in prologues)
    candidates.update((a, "manual") for a in extra)

    seeds, rejected = [], []
    for addr, source in sorted(candidates):
        if not image.contains(addr):
            rejected.append((addr, source, "import_thunk" if addr in thunks else "outside_text"))
        elif not image.valid(addr):
            rejected.append((addr, source, "invalid_insn"))
        else:
            seeds.append(Seed(addr, source))

    bl_targets = set(bl_sites)
    bl_in_text = {a for a in bl_targets if image.contains(a)}
    bl_thunk = bl_targets & set(thunks)
    counts = {
        "ghidra_functions": len(ghidra_all),
        "ghidra_in_text": sum(1 for a in ghidra_all if image.contains(a)),
        "opd": len(opd_funcs),
        "opd_not_ghidra": len(opd_funcs - ghidra_all),
        "bl_sites": sum(len(s) for s in bl_sites.values()),
        "bl_targets": len(bl_targets),
        "bl_targets_in_text": len(bl_in_text),
        "bl_targets_not_ghidra": len(bl_in_text - ghidra_all),
        "bl_targets_thunk": len(bl_thunk),
        "bl_targets_other": len(bl_targets - bl_in_text - bl_thunk),
        "thunks": len(thunks),
        "thunks_never_bl": len(set(thunks) - bl_targets),
        "data_ptr_candidates": len(ptr_targets),
        "data_ptr_seeds": len(data_ptr_seeds),
        "ghidra_data_refs_from_data": len(ghidra_data_refs),
        "ghidra_data_ref_seeds": len(ref_seeds),
        "prologue_candidates": len(prologues),
        "prologue_seeds": len(prologues) if prologue else 0,
        "manual": len(set(extra)),
        "union_ghidra_bl_opd": len(ghidra_all | bl_in_text | opd_funcs),
        "seeds_total": len(seeds),
        "rejected": len(rejected),
    }
    return SeedCensus(tuple(seeds), bl_sites, tuple(rejected), counts)
