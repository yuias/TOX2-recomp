# Function recovery

Function recovery uses Ghidra for function starts and data references, and this
repository's own decoder for function extents. Ghidra truncates some functions at the
Cell-only vector load/store instructions (`lvlx`, `lvrx`, `stvlx`, `stvrx` and their `l`
variants) that it cannot decode, so `recover` re-derives every function body itself from the
seeds. It also resolves `bctr` jump tables, recognises import thunks, resolves lv2 syscall
numbers over the recovered control-flow graph and reports coverage.

## Commands

```sh
python tools/ps3tool.py ghidra-export  <elf>
python tools/ps3tool.py ghidra-summary <elf> --ghidra generated/ghidra/eboot_ghidra.json -o config/ghidra_summary.json
python tools/ps3tool.py recover        <elf> -o config/functions.json
```

- `ghidra-summary` reduces the large `ghidra-export` JSON to a small file: function starts,
  names (only non-default ones), bodies, references from data into `.text`, Ghidra's code and
  data ranges, and the `bctr` sites. It records the SHA-256 of the ELF, the `.text` range and
  the Ghidra version and language, and prints a count for each kind of entry. The summary
  contains addresses and names only, never instruction words.
- `recover` needs no Ghidra installation. It reads the committed summary
  (`--ghidra-summary`, default `config/ghidra_summary.json`) and combines it with its own
  seeds: `bl` targets, function-descriptor (OPD) entries, the entry point, exports and
  extra seeds. It then partitions the code range into functions until the set of functions
  no longer changes. The code range is `.text` plus the executable sections that adjoin it
  (the small initializer and finalizer sections), up to but excluding the import stubs.
  `disasm`, `ghidra-diff` and `probe` still work on `.text` alone. It writes the function
  database (`-o`, default `generated/analysis/functions.json`), a report (`--report`, default
  `generated/analysis/recovery_report.json`) and the list of recovered starts that Ghidra
  lacks (`--seeds-out`, default `generated/analysis/ghidra_seeds.txt`).
- `--no-ghidra` runs `recover` without a summary, from the ELF's own seeds only. Without
  it, a missing default summary is an error that points at `ghidra-summary` and
  `--no-ghidra`.
- `--prologue-seeds` additionally seeds code that looks like a function prologue after a
  flow boundary. It is off by default; the report counts such candidates either way.
- `--max-rounds N` limits the partition rounds (default 64).
- `--nids` and `--syscalls` select the name tables (default: the committed ones).

`recover` exits with 4 if the summary does not match the ELF's hash or `.text` range, and
with 8 if the partition does not converge within `--max-rounds`. In the second case the
function database is not written.

## Extra seeds

`config/extra_seeds.json` lists function starts that neither Ghidra nor the `bl` scan
provides:

```json
{"schema": 1, "seeds": [{"addr": "0x000cc194", "reason": "..."}]}
```

`reason` is a free-text justification. `recover` reads the file by default when it exists
(`--extra-seeds PATH` selects another one, which must exist). The entries are counted as
`manual` seeds. Two kinds of reason occur in the committed file:

- a hand-written explanation, for a start that Ghidra missed and that would otherwise be
  absorbed into the body of a neighbouring function;
- `ghidra-feedback`, for starts that Ghidra reported only after being given the recovered
  starts (see below).

## Feeding recovered starts back to Ghidra

`recover` writes the starts it found that Ghidra's summary lacks to `--seeds-out`. Passing
that file to `ghidra-export --seeds-file` seeds a second Ghidra run with them. The file
holds one hex address per line; blank lines and `#` comments are ignored. The seeded run
needs its own project directory and output file so that the base export is left alone:

```sh
python tools/ps3tool.py ghidra-export  <elf> --seeds-file generated/analysis/ghidra_seeds.txt --project-dir generated/ghidra/project_seeded -o generated/ghidra/eboot_ghidra_seeded.json
python tools/ps3tool.py ghidra-summary <elf> --ghidra generated/ghidra/eboot_ghidra_seeded.json -o generated/ghidra/summary_seeded.json
python tools/ps3tool.py recover        <elf> --ghidra-summary generated/ghidra/summary_seeded.json -o generated/analysis/functions_seeded.json --report generated/analysis/recovery_report_seeded.json
```

Function entries in the seeded summary that are missing from `config/functions.json` are
candidates for `config/extra_seeds.json` with the reason `ghidra-feedback`. The committed
`config/ghidra_summary.json` always comes from the base export (entry point and exports as
the only seeds), so it does not depend on an earlier recovery run.

## Committed files and regeneration

| File | Produced by | Notes |
| --- | --- | --- |
| `config/ghidra_summary.json` | `ghidra-summary` on the base `ghidra-export` | Needs Ghidra and the ELF; regenerate when the Ghidra version, language setting or base seeding changes |
| `config/extra_seeds.json` | by hand, or from the feedback run | Small and reviewable; edit it directly |
| `config/functions.json` | `recover -o config/functions.json` | Input for later stages; needs only the ELF and the files above |

None of them contains instruction words or other code bytes. `generated/` is not committed:
the exports, the seeded export and the report stay local.

Run `recover -o config/functions.json` again whenever the recovery code, the summary, the
extra seeds or the name tables change. Two runs on the same inputs write byte-identical
files, so `git diff config/` shows exactly what the change affected. To rebuild all three
from scratch:

```sh
python tools/ps3tool.py ghidra-export  <elf>
python tools/ps3tool.py ghidra-summary <elf> --ghidra generated/ghidra/eboot_ghidra.json -o config/ghidra_summary.json
python tools/ps3tool.py recover        <elf> -o config/functions.json
```

## Function database

`config/functions.json` contains:

- `input`: the ELF SHA-256, the summary SHA-256 (`null` with `--no-ghidra`) and the number
  of extra seeds;
- `text`, `code` and `toc`: the `.text` range, the code range that was recovered and emitted,
  and the TOC base;
- `functions`: for each function its entry, a name (Ghidra's non-default name, otherwise
  `null`), the sources that found it (`bl`, `ghidra`, `ghidra_data_ref`, `opd`, `entry`,
  `manual`, and `prologue` with `--prologue-seeds`) and its address ranges;
- `import_thunks`: the import stubs, with module, FNID, resolved name and the slot they
  load from;
- `jump_tables`: resolved `bctr` tables with their branch site, table address, entry kind
  and targets;
- `unresolved_indirect`: indirect branches that were not resolved, each with a reason.

## Report

The report is JSON; the same information is summarised on stdout, one line per topic. Its
top-level sections:

| Section | Contents |
| --- | --- |
| `seeds` | Seed counts per source and the rejected candidates with reasons |
| `thunks` | Import thunk count, slot mismatches, thunks that no `bl` reaches |
| `jump_tables` | `bctr` sites, resolved tables and targets, unresolved sites by reason |
| `partition` | Rounds, convergence, functions by source, promoted shared code, tail-edge kinds, invalid flow, rejected calls, external targets, functions seeded only by an unowned `bl` |
| `coverage` | Words owned and not owned in the code range; uncovered runs classified as `zero`, `padding`, `jump_table`, `data` or `code`; prologue candidates by position |
| `ghidra` | Function extents compared with Ghidra's (`equal`, `ours_larger`, `ours_smaller`, `different`, `missing`), Ghidra functions cut at Cell-only vector instructions and whether ours extend past them, Ghidra code not owned by any function, data references by target class, Ghidra starts reached only by branches |
| `syscalls` | `sc` sites and the lv2 number resolved at each by walking the CFG back to the `r11` load, unresolved sites with reasons, and disagreements with `probe` |

Lists in the report are capped unless the field is documented as complete. `coverage`,
`ghidra` and `syscalls` are only present when the partition converged, and `ghidra` is
absent with `--no-ghidra`.
