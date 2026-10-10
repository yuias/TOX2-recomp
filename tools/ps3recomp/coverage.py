"""Coverage of the code range by the recovered functions and consistency with the Ghidra summary."""

from __future__ import annotations

from bisect import bisect_right
from collections.abc import Iterable, Mapping, Sequence

from .ghidra_summary import GhidraSummary
from .jumptab import JumpTable
from .partition import RecoveredFunction
from .ppu_flow import CodeImage
from .seeds import is_prologue

UNCOVERED_CLASSES = ("zero", "padding", "jump_table", "data", "code")
EXTENT_CATEGORIES = ("equal", "ours_larger", "ours_smaller", "different", "missing")
DATA_REF_CLASSES = ("function_start", "jump_table_target", "mid_function", "jump_table_data",
                    "uncovered", "outside_text")

# Cell-only vector load/store forms that Ghidra's PowerPC language cannot decode.
CELL_ONLY_FORMS = frozenset(
    "lvlx lvrx lvlxl lvrxl stvlx stvrx stvlxl stvrxl".split())

_NOP = 0x60000000
_LARGEST_CODE_RUNS = 20
_EXTENT_EXAMPLES = 20
_EXAMPLE_LIMIT = 50
# A run with fewer decodable words than this fraction is data, not code.
_VALID_FRACTION = (3, 4)

# Sources that mean a function is entered other than by falling or branching into it.
_ENTERED_SOURCES = frozenset({"bl", "call", "opd", "export", "entry", "ghidra_data_ref",
                              "data_ptr"})


def _hex(value: int) -> str:
    return "0x%08x" % value


def _merge(ranges: Iterable[tuple[int, int]]) -> list[tuple[int, int]]:
    merged: list[list[int]] = []
    for start, end in sorted(ranges):
        if end <= start:
            continue
        if merged and start <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end])
    return [(a, b) for a, b in merged]


class _RangeIndex:
    """Overlap queries over sorted, disjoint end-exclusive ranges."""

    def __init__(self, ranges: Iterable[tuple[int, int]]):
        self.ranges = _merge(ranges)
        self._starts = [a for a, _ in self.ranges]

    def overlap_bytes(self, start: int, end: int) -> int:
        total = 0
        i = max(bisect_right(self._starts, start) - 1, 0)
        while i < len(self.ranges) and self.ranges[i][0] < end:
            a, b = self.ranges[i]
            total += max(0, min(b, end) - max(a, start))
            i += 1
        return total


def _runs(addrs: Iterable[int]) -> list[tuple[int, int]]:
    """Maximal word runs (end-exclusive) of the given word addresses."""
    runs: list[list[int]] = []
    for addr in sorted(addrs):
        if runs and runs[-1][1] == addr:
            runs[-1][1] = addr + 4
        else:
            runs.append([addr, addr + 4])
    return [(a, b) for a, b in runs]


def _clip_words(ranges: Iterable[tuple[int, int]], image: CodeImage) -> set[int]:
    """Word addresses of `ranges` inside the image."""
    out: set[int] = set()
    for a, b in ranges:
        a = max(a, image.start)
        b = min(b, image.end)
        out.update(range(a + (-a) % 4, b, 4))
    return out


def _classify(image: CodeImage, start: int, end: int, table_words: set[int],
              ghidra_data: _RangeIndex | None) -> str:
    first = (start - image.start) // 4
    words = image.words[first:first + (end - start) // 4]
    if not any(words):
        return "zero"
    if all(w == 0 or w == _NOP for w in words):
        return "padding"
    if all(a in table_words for a in range(start, end, 4)):
        return "jump_table"
    valid = sum(1 for a in range(start, end, 4) if image.valid(a))
    num, den = _VALID_FRACTION
    if (ghidra_data is not None and ghidra_data.overlap_bytes(start, end) > 0) \
            or valid * den < num * len(words):
        return "data"
    return "code"


def _table_words(image: CodeImage, tables: Mapping[int, JumpTable]) -> set[int]:
    words: set[int] = set()
    for table in tables.values():
        if table.table_in_text:
            words.update(a for a in (table.table + 4 * i for i in range(table.count))
                         if image.contains(a))
    return words


def _coverage_section(image: CodeImage, functions: Mapping[int, RecoveredFunction],
                      owner: Mapping[int, int], table_words: set[int],
                      prologues: Sequence[int], summary: GhidraSummary | None) -> dict:
    ghidra_data = None if summary is None else _RangeIndex(summary.data_ranges)
    ghidra_code = None if summary is None else _RangeIndex(summary.code_ranges)

    unowned = (a for a in range(image.start, image.end, 4) if a not in owner)
    runs = _runs(unowned)
    by_class = {c: {"words": 0, "runs": 0} for c in UNCOVERED_CLASSES}
    code_runs = []
    class_of_run: dict[int, str] = {}
    for start, end in runs:
        cls = _classify(image, start, end, table_words, ghidra_data)
        by_class[cls]["words"] += (end - start) // 4
        by_class[cls]["runs"] += 1
        class_of_run[start] = cls
        if cls == "code":
            code_runs.append((start, end))
    code_runs.sort(key=lambda r: (-(r[1] - r[0]), r[0]))

    run_starts = [s for s, _ in runs]

    def run_class(addr: int) -> str:
        return class_of_run[run_starts[bisect_right(run_starts, addr) - 1]]

    largest = []
    for start, end in code_runs[:_LARGEST_CODE_RUNS]:
        largest.append({
            "start": _hex(start), "end": _hex(end), "words": (end - start) // 4,
            "starts_with_prologue": is_prologue(image, start),
            "ghidra_code_words": None if ghidra_code is None
            else ghidra_code.overlap_bytes(start, end) // 4,
        })

    already = sum(1 for a in prologues if a in functions)
    inside = sum(1 for a in prologues if a not in functions and a in owner)
    in_uncovered = [a for a in prologues if a not in owner]
    in_code_run = sum(1 for a in in_uncovered if run_class(a) == "code")

    uncovered_words = sum(v["words"] for v in by_class.values())
    text_words = len(image.words)
    owned = len(owner)
    return {
        "text_words": text_words,
        "owned_words": owned,
        "owned_pct": round(100.0 * owned / text_words, 2) if text_words else 0.0,
        "uncovered": {
            "words": uncovered_words,
            "runs": len(runs),
            "by_class": by_class,
            "largest_code_runs": largest,
        },
        "prologue_candidates": {
            "total": len(prologues),
            "already_start": already,
            "inside_function": inside,
            "in_uncovered": len(in_uncovered),
            "in_uncovered_code_runs": in_code_run,
        },
    }


def _category(items: list[dict]) -> dict:
    return {"count": len(items), "examples": items[:_EXTENT_EXAMPLES]}


def _extents(image: CodeImage, functions: Mapping[int, RecoveredFunction],
             summary: GhidraSummary) -> dict:
    found: dict[str, list[dict]] = {c: [] for c in EXTENT_CATEGORIES}
    for fn in summary.functions:
        if not image.contains(fn.entry):
            continue
        ghidra_words = _clip_words(fn.body, image)
        ours = functions.get(fn.entry)
        item = {"entry": _hex(fn.entry), "ghidra_words": len(ghidra_words)}
        if ours is None:
            found["missing"].append(item)
            continue
        our_words = ours.addrs
        item["our_words"] = len(our_words)
        if our_words == ghidra_words:
            cat = "equal"
        elif our_words > ghidra_words:
            cat = "ours_larger"
        elif our_words < ghidra_words:
            cat = "ours_smaller"
        else:
            cat = "different"
        if cat != "equal":
            item["only_ours"] = len(our_words - ghidra_words)
            item["only_ghidra"] = len(ghidra_words - our_words)
        found[cat].append(item)
    return {c: _category(found[c]) for c in EXTENT_CATEGORIES}


def _cell_truncated(image: CodeImage, owner: Mapping[int, int],
                    summary: GhidraSummary) -> dict:
    """Ghidra functions with a body range ending at a Cell-only word.

    Counted per function; `points` counts every such range end. A function is `extended` when
    all of its truncation points are owned by our function with the same entry.
    """
    truncated = 0
    extended = 0
    points = 0
    extended_points = 0
    not_extended = []
    for fn in summary.functions:
        body = _clip_words(fn.body, image)
        found = 0
        owned_here = 0
        for _, end in fn.body:
            if not image.contains(end) or end in body:
                continue
            spec = image.insn(end).spec
            if spec is None or spec.name not in CELL_ONLY_FORMS:
                continue
            found += 1
            owner_entry = owner.get(end)
            if owner_entry == fn.entry:
                owned_here += 1
            else:
                not_extended.append({
                    "function": _hex(fn.entry), "end": _hex(end),
                    "owner": None if owner_entry is None else _hex(owner_entry)})
        if found:
            truncated += 1
            points += found
            extended_points += owned_here
            extended += owned_here == found
    return {"count": truncated, "extended": extended, "points": points,
            "extended_points": extended_points, "not_extended": not_extended}


def _code_not_owned(image: CodeImage, owner: Mapping[int, int],
                    summary: GhidraSummary) -> dict:
    words = [a for a in sorted(_clip_words(summary.code_ranges, image)) if a not in owner]
    ranges = _runs(words)
    return {"words": len(words), "runs": len(ranges),
            "ranges": [[_hex(a), _hex(b)] for a, b in ranges]}


def _data_refs(image: CodeImage, functions: Mapping[int, RecoveredFunction],
               owner: Mapping[int, int], tables: Mapping[int, JumpTable],
               table_words: set[int], summary: GhidraSummary) -> dict:
    table_targets = {t for table in tables.values() for t in table.targets}
    counts = {key: {c: 0 for c in DATA_REF_CLASSES} for key in ("from_exec", "not_from_exec")}
    for _, to, _, from_exec in summary.data_refs:
        if not image.contains(to):
            cls = "outside_text"
        elif to in functions:
            cls = "function_start"
        elif to in table_targets:
            cls = "jump_table_target"
        elif to in owner:
            cls = "mid_function"
        elif to in table_words:
            cls = "jump_table_data"
        else:
            cls = "uncovered"
        counts["from_exec" if from_exec else "not_from_exec"][cls] += 1
    return counts


def _starts_only_branch_reached(functions: Mapping[int, RecoveredFunction],
                                summary: GhidraSummary) -> dict:
    called = {t for fn in functions.values() for t in fn.calls}
    inbound: dict[int, set[int]] = {}
    for fn in functions.values():
        for edge in fn.tail_edges:
            if edge.kind != "import":
                inbound.setdefault(edge.target, set()).add(fn.entry)
    found = []
    for g in summary.functions:
        fn = functions.get(g.entry)
        if fn is None or g.entry in called or fn.sources & _ENTERED_SOURCES:
            continue
        # Reached only by tail edges; a start nothing reaches at all is not a split candidate.
        if g.entry in inbound:
            found.append({"entry": _hex(g.entry),
                          "from": [_hex(a) for a in sorted(inbound[g.entry])]})
    found.sort(key=lambda item: item["entry"])
    return {"count": len(found), "examples": found[:_EXAMPLE_LIMIT]}


def build_coverage(image: CodeImage, functions: Mapping[int, RecoveredFunction],
                   tables: Mapping[int, JumpTable], prologues: Sequence[int],
                   summary: GhidraSummary | None = None) -> tuple[dict, dict | None]:
    """The `coverage` report section and, with a summary, the `ghidra` section."""
    owner = {addr: entry for entry, fn in functions.items() for addr in fn.addrs}
    table_words = _table_words(image, tables)
    coverage = _coverage_section(image, functions, owner, table_words, prologues, summary)
    if summary is None:
        return coverage, None
    ghidra = {
        "extents": _extents(image, functions, summary),
        "cell_truncated": _cell_truncated(image, owner, summary),
        "code_not_owned": _code_not_owned(image, owner, summary),
        "data_refs": _data_refs(image, functions, owner, tables, table_words, summary),
        "starts_only_branch_reached": _starts_only_branch_reached(functions, summary),
    }
    return coverage, ghidra


def coverage_lines(coverage: dict, ghidra: dict | None) -> list[str]:
    unc = coverage["uncovered"]
    classes = ", ".join("%s %d" % (c, unc["by_class"][c]["words"]) for c in UNCOVERED_CLASSES)
    lines = ["coverage: owned %d / %d words (%.1f%%); uncovered %d words: %s"
             % (coverage["owned_words"], coverage["text_words"], coverage["owned_pct"],
                unc["words"], classes)]
    if ghidra is not None:
        ext = ghidra["extents"]
        cell = ghidra["cell_truncated"]
        lines.append("ghidra extents: %s; cell-truncated %d, extended %d"
                     % (", ".join("%s %d" % (c, ext[c]["count"]) for c in EXTENT_CATEGORIES),
                        cell["count"], cell["extended"]))
        lines.append("ghidra code not owned: %d words in %d ranges"
                     % (ghidra["code_not_owned"]["words"], ghidra["code_not_owned"]["runs"]))
    return lines
