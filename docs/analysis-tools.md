# Analysis tools

`tools/ps3tool.py` has fourteen subcommands. The first nine take the path to the decrypted ELF
(`<elf>`; the supported executable is the one pinned in `config/target.json`, see
[Target pin](#target-pin)), except `nidgen`, which reads an RPCS3 checkout. `info`, `recover` and `emit`
also accept the firmware module libsre as a relocated PRX (see [PRX input](#prx-input)). The five `spu-*`
commands work on the SPU programs that are embedded in the executable and in libsre; they take no
positional argument (see [SPU images](#spu-images)).

```sh
python tools/ps3tool.py info   <elf> [-o OUTPUT] [--target-config FILE] [--nids FILE] [--no-verify]
python tools/ps3tool.py probe  <elf> [-o OUTPUT] [--syscalls FILE]
python tools/ps3tool.py disasm <elf> (--start ADDR (--count N | --end ADDR) | --entry [--count N] | --stats [--start ADDR --end ADDR] [--json OUT]) [--raw] [--strict]
python tools/ps3tool.py ghidra-export <elf> [--ghidra-dir DIR] [--language ID] [--cspec ID] [--project-dir DIR] [-o OUT] [--start ADDR] [--end ADDR] [--timeout SEC] [--seeds-file PATH] [--check-only] [--cell]
python tools/ps3tool.py ghidra-diff   <elf> --ghidra JSON [--start ADDR] [--end ADDR] [--strict] [-o OUT] [--top N] [--examples K]
python tools/ps3tool.py ghidra-summary <elf> --ghidra JSON [-o OUT]
python tools/ps3tool.py recover <elf> [--ghidra-summary PATH | --no-ghidra] [--extra-seeds PATH] [--nids FILE] [--syscalls FILE] [-o OUT] [--report PATH] [--seeds-out PATH] [--prologue-seeds] [--max-rounds N]
python tools/ps3tool.py emit <elf> [--db PATH] [--out DIR] [--chunks N] [--comments] [--report PATH] [--census] [--census-out PATH] [--symbol-prefix NAME] [--max-unhandled N]
python tools/ps3tool.py nidgen --rpcs3 DIR [--source-commit COMMIT] [--nids-out FILE] [--syscalls-out FILE]
python tools/ps3tool.py spu-extract      [--out DIR]
python tools/ps3tool.py spu-disasm       (--image NAME [--start LS [--end LS]] | --stats [--json OUT])
python tools/ps3tool.py spu-objdump-diff [--objdump PATH] [--wsl-distro NAME] [--listing-dir DIR] [--report PATH] [--top N] [--examples K]
python tools/ps3tool.py spu-recover      [--out DIR] [--report PATH]
python tools/ps3tool.py spu-emit         [--db-dir DIR] [--out DIR] [--report PATH] [--comments] [--census] [--max-unhandled N]
```

Every `spu-*` command also takes `[--eboot ELF] [--libsre ELF] [--images a,b,...] [--registry FILE]
[--target-config FILE]`.

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
  `--symbol-prefix NAME` prefixes the file names and global symbols of the output; a PRX needs it and
  the main executable refuses it (see [A PRX module](emitter.md#a-prx-module-libsre)).
- `nidgen` builds the name tables from RPCS3 sources; see below.
- `spu-extract`, `spu-disasm` and `spu-objdump-diff` read the SPU images; see [SPU images](#spu-images) and
  [SPU decoder cross-check with objdump](#spu-decoder-cross-check-with-objdump). `spu-recover` and `spu-emit`
  recover and emit them; see [Function recovery](function-recovery.md#on-the-spu-images) and
  [Emitting C](emitter.md#spu-images).

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

## PRX input

libsre (`libsre.prx`) is a PRX, a shared module without sections whose module info (name, version,
TOC and the export and import tables) sits at the file offset in `p_paddr` of its first program header.
The tools read it after `tox2_decrypt reloc` has placed it at `0x011d0000` and applied its
relocations (`generated/inputs/libsre.reloc.elf`, see [Decrypting the inputs](building.md#decrypting-the-inputs)):

- `info` takes imports and exports from the module info and prints `module:   cellSre_Library 1.1 toc
  0x01209ab0`. The summary has a `module_info` object. A PRX has no entry function, so `entry` shows
  `func=None` with the module TOC. The pin that it checks is `libsre_reloc_elf` for the relocated file
  and `libsre_prx` for the unrelocated one. The default output file is still
  `generated/analysis/eboot_info.json`, so pass `-o` for libsre.
- `recover` and `emit` refuse an unrelocated PRX (`PRX with unapplied relocations; pass the file written
  by tox2_decrypt reloc`). The text range is the code plus the 32 import thunks, and the function
  descriptor scan steps by 4 bytes, because libsre's descriptors sit at offsets that are 4 modulo 8.
  The default `config/extra_seeds.json` belongs to the executable and is not read for a PRX.
  `recover` needs `--no-ghidra`, and `emit` needs `--symbol-prefix` and `--db config/libsre_functions.json`.
- `probe`, `disasm` and the Ghidra commands were written for the executable and have not been run on libsre.

What `info` reports for libsre: 3 program headers (the relocation header is kept with its type
cleared), 2 import modules with 32 functions (31 have a name; sysPrxForUser FNID `0x9fb6228e` has
none in `config/nids.json`) and 267 exports. `recover` and `emit` are described in
[Function recovery](function-recovery.md#on-libsre) and [Emitting C](emitter.md#a-prx-module-libsre).

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
- `--cell` selects the Cell PPU language `PowerPC:BE:64:CellPPU-32addr` instead, which also
  decodes the Cell-only vector loads and stores (`lvlx`, `lvrx` and related forms). It cannot be
  combined with `--language` (exit 2) and has its own project, export and log files under
  `generated/ghidra/` (`project_cell/`, `eboot_ghidra_cell.json`, `logs_cell/`); `--project-dir`
  and `-o` override the first two, and `--check-only --cell` checks `eboot_ghidra_cell.json`. With
  this language the language check also requires `lvlx` and `lvrx` to decode like this
  repository's decoder, and the run exits with 7 if they do not; with other languages those two
  probes are informational. This repository does not build the language: it must be installed in
  Ghidra as the `CellPPU` module (for example by `tools/install-ghidra-cell.ps1` of
  [DAL-Rinne-Utopia-recomp](https://github.com/yuias/DAL-Rinne-Utopia-recomp)). If Ghidra exits
  with an error in this mode, the message asks whether the language is installed.
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

## SPU images

The game runs SPU code that is embedded as data: eight programs in the executable and eight blobs in
libsre (see [SPU usage](spu.md)). `config/spu_images.json` is the registry of these 16 images. It holds
addresses, sizes, entries and roles only; the bytes are read from `generated/inputs/EBOOT.elf` and
`generated/inputs/libsre.reloc.elf`, which every `spu-*` command checks against the `eboot_elf` and
`libsre_reloc_elf` pins of `config/target.json` (exit 4 on a mismatch). The inputs are the defaults of
`--eboot` and `--libsre`; `--images a,b,...` selects images by registry name, `--registry` and
`--target-config` select other files.

Registry fields of one image:

| Field | Meaning |
| --- | --- |
| `name` | Unique, `[a-z][a-z0-9_]*`; used in file names and C symbols (`eboot_spline`, `libsre_taskset_pm`, ...) |
| `container`, `ea` | `eboot` or `libsre`, and the absolute guest address of the image (libsre at `0x011d0000` plus its offset) |
| `kind` | `elf`: LS addresses, entry and code segments come from the SPU ELF (the `PF_X` segments are code). `raw`: `size`, `ls_base` and `entry` (default `ls_base`) are given, and the whole blob is one code segment |
| `role` | `spurs_kernel`, `spurs_system_service`, `spurs_policy_module`, `spurs_task`, `raw_thread`, `exit_handler` or `job_chain_pm`; informational |
| `lowering` | How control transfers are emitted. Only `stackfree` exists; any other value is an error (exit 2). `lowering_overrides` (`{"0xLS": "<lowering>"}`) is the per-function form of the same choice |
| `gate` | `true` for every image with a non-null `ls_base`: `spu-emit --max-unhandled` and `spu-objdump-diff` count the image toward their gate |
| `extra_entries` | `[{ls, reason}]`: LS addresses that other code reaches through a pointer that recovery cannot see, for example the exit address that the kernel stores at LS `0x1e0` for the policy modules |
| `data_ranges` | `[{start, end, reason}]`: LS ranges inside a code segment that hold data, not code (the kernel context of `libsre_kernel1`, `0x100..0x290`) |
| `pic_delta_regs` | Registers that hold the load delta of position-independent code (`eboot_edgelzma`: `$126`). The delta is 0 for an image at its link address, so recovery evaluates them as 0 and `a rX,rY,$126` with a constant `rY` is a constant address |

The two libsre blobs `libsre_taskset2_exit` and `libsre_exit_code_plugin` have `ls_base` null: they are
position independent and have no fixed LS address. `spu-extract`, `spu-disasm` (listing mode, at LS base
0) and `spu-objdump-diff` handle them; `spu-disasm --stats`, `spu-recover` and `spu-emit` skip them, and
an explicit `--images` naming one of them is an error. They are neither recovered, emitted nor gated.

```sh
python tools/ps3tool.py spu-extract
python tools/ps3tool.py spu-disasm --image eboot_edgelzma --start 0x3090 --end 0x30b0
python tools/ps3tool.py spu-disasm --stats --json generated/analysis/spu_census.json
```

- `spu-extract` writes each image as a standalone file for inspection with other tools into `--out`
  (default `generated/spu/images`): `<name>.elf` for an ELF image, `<name>.bin` for a raw blob, and
  `manifest.json` with the SHA-256, LS range, entry and segments of every image. It prints one line per
  image, for example `eboot_edgelzma elf ea 0x00f76700 ls 0x3000-0x37c80 entry 0x3090 code segments 1`.
  The other SPU commands do not read these files.
- `spu-disasm --image NAME` lists the code of one image as `<LS>: <word>  <text>`, for example
  `03090: 43ffe808  ila $8,0x3ffd0`. Without `--start` it lists every code segment; with `--start` and no
  `--end` it lists one instruction. Registers print as `$0`..`$127` (no aliases), channels as `$ch<N>`
  and branch targets as LS addresses. The decoder covers every instruction of SPU ISA 1.2; any other word
  is printed as `.long`.
- `spu-disasm --stats` prints a census instead of a listing, per image: decoded, `.long` and all-zero words
  of the code segments, the 20 most common mnemonics, the channels used by `rdch`, `wrch` and `rchcnt`
  (with their names), every `stop` code, and the constants written to `MFC_Cmd`. `--json OUT` also writes
  it. It was written to find what an image needs before recovery existed.

The code words of the 14 images that have an LS base (the first line of each image in the census):

| Image | Code words | Decoded | `.long` | Zero |
| --- | --- | --- | --- | --- |
| `eboot_spline` | 1,120 | 1,119 | 0 | 1 |
| `eboot_edgelzma` | 4,584 | 4,259 | 248 | 77 |
| `eboot_bnuscore` | 21,252 | 20,248 | 851 | 153 |
| `eboot_animation` | 4,892 | 4,864 | 23 | 5 |
| `eboot_updatematrix` | 14,888 | 14,846 | 5 | 37 |
| `eboot_renderparticle` | 8,980 | 8,779 | 184 | 17 |
| `eboot_morph` | 3,864 | 3,843 | 16 | 5 |
| `eboot_skin` | 3,852 | 3,847 | 0 | 5 |
| `libsre_kernel1` | 480 | 388 | 1 | 91 |
| `libsre_kernel2` | 484 | 391 | 0 | 93 |
| `libsre_system_service` | 2,176 | 2,142 | 8 | 26 |
| `libsre_taskset_pm` | 1,936 | 1,817 | 11 | 108 |
| `libsre_jobchain_pm1` | 3,980 | 3,930 | 3 | 47 |
| `libsre_jobchain_pm2` | 3,980 | 3,922 | 3 | 55 |

The `.long` words are data inside code segments (constants, jump tables, the kernel context) and
undecodable words; the zero words are padding.

## SPU decoder cross-check with objdump

`spu-objdump-diff` compares the SPU decoder and formatter with the `objdump` of GNU binutils, word by word,
on every code segment of the registry images (all 16, including the two position-independent blobs, which
are fed at LS base 0 because SPU branches are PC-relative in both tools). It is a developer check: nothing
else depends on it, and it needs WSL, which the other commands do not.

The reference `objdump` is built from the binutils release tarball (version 2.47, SHA-256 checked before
extraction) by `tools/build-spu-objdump.sh`, which runs inside WSL, builds only `objdump` and the other
binutils programs for `--target=spu-elf` and installs them under the WSL home, in
`~/.local/opt/tox2-spu-binutils`. The script reads no repository file, so it is fed through stdin; run it
from the repository root, in a distribution that has `gcc`, `make`, `curl`, `xz` and `tar` (the script
names the packages to install if one is missing). Ubuntu-26.04 is the distribution that was verified:

```sh
wsl.exe -d Ubuntu-26.04 -e bash -s -- < tools/build-spu-objdump.sh
python tools/ps3tool.py spu-objdump-diff
```

A second run of the script prints `spu-objdump: up to date (binutils 2.47)`. `--from TARBALL` (a WSL
path to a local copy of the tarball, which gets the same SHA-256 check; in Git Bash prefix the command with
`MSYS_NO_PATHCONV=1`) avoids the download, and `--force` builds again.

- `--objdump PATH` names the `objdump` inside WSL; the default is the environment variable
  `TOX2_SPU_OBJDUMP`, then the install of the script. `--wsl-distro NAME` selects the distribution (default
  `Ubuntu-26.04`; an empty name selects the WSL default). The code bytes go in through stdin, so WSL needs no
  access to the repository drive. Each listing is also written to `generated/analysis/spu_objdump/` as
  `<image>_<ls>.txt`, and `--listing-dir DIR` reads such listings instead of running `objdump`, which
  needs no WSL (`objdump_version` in the report is then `null`).
- Both sides go through one parser of the printed text that normalizes the printing differences (signed
  against unsigned fields, register and channel names, branch targets modulo the LS size), so the
  formatter is checked as well as the decoder. The cases where the ISA shows that the decoder is right and `objdump` is not are
  recorded in `KNOWN_DIFFERENCES` in `tools/ps3recomp/spu_objdump_diff.py`, each with the ISA page: `objdump`
  reads the reserved bit 11 of `bihnz` as a hint flag (`bihnzp`, `bihnzpd`) and the reserved bits 12 and
  13 of `sync` as `d` and `e` flags (`syncd`, `synce`, ...). It also prints the false-target register of
  the conditional halts as an extra first operand, and a displacement of 0 of `br`, `brz`, `brnz`, `brhz`,
  `brhnz` and `hbrr` as the plain number 0; the comparison accounts for both. A disagreement is settled
  from the ISA document, not by adopting `objdump`'s behaviour.
- Each word is put into one category:

  | Category | Meaning |
  | --- | --- |
  | `match` | Both decode it and the text agrees |
  | `known_difference` | Both decode it; the pair is a recorded, explained difference |
  | `objdump_code_we_unknown` | `objdump` decodes it and this decoder does not |
  | `we_code_objdump_unknown` | This decoder decodes it and `objdump` prints `.long` |
  | `mnemonic_mismatch` | Both decode it and the mnemonics (or flags) differ |
  | `operand_mismatch` | Both decode it with the same mnemonic and the operands differ |
  | `both_unknown` | Neither decodes it (data and padding in code segments) |

  The four disagreement categories (`objdump_code_we_unknown` to `operand_mismatch`) must be 0 in every
  gate image; every image is a gate image except the two without an LS base. The command prints one line per image with the seven counts, then for each non-gate image with
  disagreements `ungated disagreement: <image> <category> <n>`, then `gate: ok` or
  `gate: <n> unexplained disagreements in <images>`. The report (`--report`, default
  `generated/analysis/spu_objdump_diff.json`) has the first line of `objdump --version`, the category counts
  per image and segment, and for each image the `--top` (default 20) most frequent
  `(category, ours, objdump)` triples with up to `--examples` (default 5) examples each. Exit codes: 0 gate
  ok, 1 tool error (for example an unreadable listing), 2 usage, 4 hash mismatch, 6 `objdump` not
  available, 9 an unexplained disagreement in a gate image.

On this executable and libsre the run takes about 6 s with `objdump` 2.47.20260726 (`GNU objdump (GNU
Binutils) 2.47.20260726`) and ends with `gate: ok`. Over the 16 images and 77,332 words: 75,945 `match`,
34 `known_difference` (2 `bihnz` in `eboot_edgelzma` and 32 `sync` in `eboot_bnuscore`), 0 in each of the
four disagreement categories, and 1,353 `both_unknown`. The two blobs without an LS base also have
0 disagreements.

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
version of `libsre.prx`, and the base address, size and SHA-256 of `libsre.reloc.elf` in the
`libsre_reloc_elf` entry (the base `0x011d0000` is read by CMake for the build and has its second
home in `runtime/src/hle/libsre.h`). `tox2_decrypt` checks these (see
[Decrypting the inputs](building.md#decrypting-the-inputs)).
