# Analysis tools

`tools/ps3tool.py` has nine subcommands. Each takes the path to the decrypted ELF (`<elf>`; the
supported executable is the one pinned in `config/target.json`, see [Target pin](#target-pin)),
except `nidgen`, which reads an RPCS3 checkout.

```sh
python tools/ps3tool.py info   <elf> [-o OUTPUT] [--target-config FILE] [--nids FILE] [--no-verify]
python tools/ps3tool.py probe  <elf> [-o OUTPUT] [--syscalls FILE]
python tools/ps3tool.py disasm <elf> (--start ADDR (--count N | --end ADDR) | --entry [--count N] | --stats [--start ADDR --end ADDR] [--json OUT]) [--raw] [--strict]
python tools/ps3tool.py ghidra-export <elf> [--ghidra-dir DIR] [--language ID] [--cspec ID] [--project-dir DIR] [-o OUT] [--start ADDR] [--end ADDR] [--timeout SEC] [--seeds-file PATH] [--check-only]
python tools/ps3tool.py ghidra-diff   <elf> --ghidra JSON [--start ADDR] [--end ADDR] [--strict] [-o OUT] [--top N] [--examples K]
python tools/ps3tool.py ghidra-summary <elf> --ghidra JSON [-o OUT]
python tools/ps3tool.py recover <elf> [--ghidra-summary PATH | --no-ghidra] [--extra-seeds PATH] [--nids FILE] [--syscalls FILE] [-o OUT] [--report PATH] [--seeds-out PATH] [--prologue-seeds] [--max-rounds N]
python tools/ps3tool.py emit <elf> [--db PATH] [--out DIR] [--chunks N] [--comments] [--report PATH] [--census] [--census-out PATH] [--max-unhandled N]
python tools/ps3tool.py nidgen --rpcs3 DIR [--source-commit COMMIT] [--nids-out FILE] [--syscalls-out FILE]
```

- `info` checks the file against the pinned SHA-256 and writes a JSON summary (segments,
  sections, imports, exports) to `generated/analysis/eboot_info.json`. It refuses encrypted
  input. `--no-verify` skips the hash check. Each import is given a name from the FNID
  table (`--nids`, default `config/nids.json`). The summary line reports how many imports
  are named, and every import without a name is listed as `unresolved import`. If the
  default table is missing, names stay `null` and a note is printed.
- `probe` searches for embedded SPU images and SPU-related imports, counts lv2 syscall
  sites and numbers, and writes `generated/analysis/probe.json`. The syscall number is
  recovered by scanning backwards from each `sc` for the `r11` load, through conditional
  branches; sites where it cannot be determined are counted as unresolved. Numbers are
  mapped to names with the table given by `--syscalls` (default
  `config/lv2_syscalls.json`).
- `disasm` prints PPU disassembly for an address range (`--start` with `--count` or
  `--end`) or from the entry function (`--entry`). `--raw` disables simplified mnemonics.
  `--strict` prints words whose reserved bits are set as `.long` instead of decoding them.
  `--stats` decodes every word of the executable segments (or of `[--start, --end)`) and
  reports the number of decoded and unknown words, the most common mnemonics and unknown
  opcode patterns, and a strict section: how many words still decode under reserved-bit
  checking and which mnemonics are rejected most often. `--json OUT` also writes the
  statistics as JSON.
- `ghidra-export` and `ghidra-diff` cross-check the decoder against Ghidra; see below.
- `ghidra-summary` and `recover` find and delimit functions; see [Function recovery](function-recovery.md#function-recovery).
- `emit` writes C source for the recovered functions; see [Emitting C](emitter.md#emitting-c).
- `nidgen` builds the name tables from RPCS3 sources; see below.

Run `python tools/ps3tool.py <command> --help` for the full option list.

## On this executable

What the first three commands report for the pinned *Tales of Xillia 2* executable (re-run with the
current tools; the `disasm --stats` numbers are from the first measurement of the executable):

- `info`: entry function `0x00010240` with TOC `0x010b6330`, 8 segments, 32 sections, 17 import
  modules with 173 functions, all of them named, no exports.
- `probe`: 8 embedded SPU images (at `0x00f75400`, `0x00f76700`, `0x00f7b080`, `0x00f90280`,
  `0x00fb0b80`, `0x00fe1480`, `0x00ff7580` and `0x01027680`), `cellSpurs` as the SPU-related import,
  and 267 syscall sites with 77 distinct numbers, 4 of them unresolved by the backward scan
  (`recover` resolves 266 of the 267 over the control-flow graph, see
  [Function recovery](function-recovery.md#report)).
- `disasm --stats`: `.text` is `0x00010240..0x00c8f3d4`, 3,275,877 words, of which 9,687 (0.30%) do
  not decode. Strict decoding rejects 213 more (`fdiv` 127, `frsqrte` 58, `fsqrt` 24, `fsub` 4).

## Ghidra cross-check

`ghidra-export` runs Ghidra headless on the ELF and writes a JSON file with the function
list and a per-word listing of the code range. `ghidra-diff` then compares that listing
with this repository's PPU decoder word by word. This is a validation aid for the decoder;
nothing else depends on it.

Requirements: Ghidra 12.x and a JDK 21 (the JDK that Ghidra 12 itself requires). Point the
tool at the Ghidra install directory with the `GHIDRA_INSTALL_DIR` environment variable or
`--ghidra-dir DIR`. The export scripts are Java `GhidraScript`s in `tools/ghidra/`, so no
Python environment for Ghidra is needed.

```sh
python tools/ps3tool.py ghidra-export <elf> --timeout 43200
python tools/ps3tool.py ghidra-diff   <elf> --ghidra generated/ghidra/eboot_ghidra.json
python tools/ps3tool.py ghidra-diff   <elf> --ghidra generated/ghidra/eboot_ghidra.json --strict
```

- `ghidra-export` imports the ELF with language `PowerPC:BE:64:A2ALT-32addr` (override with
  `--language` and `--cspec`), runs auto-analysis, and exports the range given by
  `--start`/`--end` (default: the largest executable section). Ghidra is seeded with the
  entry point and the exported functions; `--seeds-file` adds more starts (see "Feeding
  recovered starts back to Ghidra"). Analysis is cut off after `--timeout` seconds. The default
  is 3600. An export of this executable with a seeds file took 287 s here, but the time grows
  with the code size, so the commands in this documentation pass `--timeout 43200` as a margin.
  The tool then checks that Ghidra decodes a few probe instructions (including AltiVec
  ones) as expected, so a wrong language setting is reported. `--check-only` skips Ghidra
  and only validates and language-checks an existing export.
- Output goes under `generated/ghidra/`: the export JSON (`eboot_ghidra.json`), the Ghidra
  project (`project/`) and the Ghidra logs (`logs/`). All of it is git-ignored.
- `ghidra-diff` classifies every word of the range into one category and prints the
  counts. The report is written to `generated/analysis/ghidra_diff.json` (`-o` to change
  it), with the most frequent mismatching pairs and a few examples for each.

  | Category | Meaning |
  | --- | --- |
  | `match` | Ghidra treats the word as code and both decoders agree |
  | `known_difference` | Both decode it, but the mnemonic pair is a recorded, explained difference |
  | `ghidra_code_we_unknown` | Ghidra treats the word as code and this decoder does not know it |
  | `mnemonic_mismatch` | Both decode it and the mnemonics differ |
  | `operand_mismatch` | Both decode it with the same mnemonic and the operands differ |
  | `we_code_ghidra_noncode` | Ghidra treats the word as data or undefined, this decoder decodes it |
  | `both_noncode_unknown` | Ghidra treats the word as data or undefined and this decoder does not know it |

  `ghidra_code_we_unknown`, `mnemonic_mismatch` and `operand_mismatch` are the categories
  that point at decoder bugs. The last two are only recorded.

  For this executable, the diff against the export that produced the committed
  `config/ghidra_summary.json` gave 2,882,572 `match` words, 0 `ghidra_code_we_unknown`, 3
  `mnemonic_mismatch` (all `tw` traps) and 9 `operand_mismatch` (`tw` and `ldarx`). These counts
  were recorded when the summary was made.
- `--strict` makes `ghidra-diff` decode with reserved-bit checking, as `disasm --strict`
  does. Words that have reserved bits set then count as unknown to this decoder, so the
  category counts change accordingly.

## NID and syscall name tables

Imports are identified by FNID and lv2 syscalls by number. `nidgen` extracts both name
tables from the RPCS3 sources, which are not included in this repository. The generated
tables are committed:

- `config/nids.json`: module name, then FNID, to function name.
- `config/lv2_syscalls.json`: syscall number to name.

Each file records the RPCS3 repository URL and the commit it was generated from, so the
names can be traced to a specific revision. Regenerate them when the RPCS3 sources change:

```sh
git clone --depth 1 --filter=blob:none --sparse https://github.com/RPCS3/rpcs3.git <rpcs3-dir>
git -C <rpcs3-dir> sparse-checkout set rpcs3/Emu/Cell
python tools/ps3tool.py nidgen --rpcs3 <rpcs3-dir>
```

`--rpcs3` is the checkout root. The commit is read from the checkout's `.git` directory
(`--source-commit` overrides it). `--nids-out` and `--syscalls-out` select other output
paths, for example to compare a regenerated table with the committed one before replacing
it. `nidgen` only reads text files in the checkout and never executes anything from it. It
prints macro counts and any constructs it could not handle, and exits with a non-zero
status if the syscall table disagrees with its own comments.

## Target pin

`config/target.json` records the title ID and the size and SHA-256 of both the encrypted
`EBOOT.bin` and the decrypted `EBOOT.elf` that the tools were written against. It never
contains paths. `ps3tool.py info` compares the input with the `eboot_elf` entry so analysis
results are only trusted for the expected build. The file also pins the size, SHA-256 and firmware
version of `libsre.prx`, which `tox2_decrypt` checks (see
[Decrypting the inputs](building.md#decrypting-the-inputs)).
