# Emitting C

`emit` turns the function database and the ELF into C source. The flow is `recover`, then
`emit`, then a build with `-GeneratedDir`:

```sh
python tools/ps3tool.py recover <elf> -o config/functions.json
python tools/ps3tool.py emit    <elf> --chunks 256 --out generated/ppu --max-unhandled 0
pwsh -NoProfile -File tools/build.ps1 -Preset release -GeneratedDir generated/ppu -Test
```

The `--chunks` default is 16. This executable has about 3 million code words, so it is emitted
with `--chunks 256`; see [Chunks and build time](#chunks-and-build-time). The firmware module libsre
is emitted as a second tree with its own symbol prefix; see [A PRX module (libsre)](#a-prx-module-libsre).
A build with `-GeneratedDir` needs both trees.

`emit` reads the function database (`--db`, default `config/functions.json`), checks that it
matches the ELF's hash and code range, and emits every function of the database as one C
function named `fn_<address>`. Branches inside a function become labels `L_<address>` and
`goto`, and resolved jump tables become `switch`. Direct calls are C calls, tail calls are
`musttail` returns, and transfers to computed targets go through `tox2Dispatch`. Backward
edges carry a hook (`TOX2_LOOP`) that counts a preemption budget. Import stubs become
trampolines `imp_<address>` that call the runtime hook `tox2HleCall` with an index into the
generated import table (in a prefixed tree they call `tox2HleCallImport`, see below). The output is deterministic: two runs on the same inputs write
byte-identical files, and a file is only rewritten when its content changes.

## Output

Everything goes to `--out` (default `generated/ppu`; git-ignored):

| File | Contents |
| --- | --- |
| `ppu_chunk_NNN.c` | The function bodies, split into `--chunks` files (default 16; 256 for this executable) of similar instruction counts, so that they compile in parallel |
| `ppu_imports.c` | The import trampolines |
| `ppu_tables.c` | `kTox2PpuFuncMappings` (guest address to host function, one row per function and per trampoline), `kTox2PpuImports` (stub address, FNID, module, name) and the executable's SHA-256 |
| `ppu_funcs.h` | Declarations of the functions and trampolines |
| `sources.cmake` | The list of sources that `TOX2_PPU_GENERATED_DIR` includes |

Chunk files that a smaller `--chunks` makes unnecessary are removed. `--comments` annotates
the code with the guest disassembly; it is off by default because it makes the files larger.

The report (`--report`, default `generated/analysis/emit_report.json`) is JSON, and
`emit` prints a summary on stdout:

| Field | Contents |
| --- | --- |
| `functions`, `instructions`, `import_trampolines` | Emitted counts |
| `thunks_without_toc_save` | Import stubs that do not save the caller's TOC pointer |
| `chunks`, `largest_functions`, `output_bytes` | Size of the output |
| `unhandled` | Words emitted as a call to `tox2Unhandled`: total, per mnemonic, per group, with example addresses |
| `edges` | Calls, tail calls, switches, syscalls and traps by kind, for example `calls_direct` and `calls_indirect` |
| `dynamic_targets` | Transfers whose constant target is not a recovered function or stub, with site and target |
| `labels`, `loop_hooks`, `elapsed_s` | Emitted labels, loop hooks, run time |

`--census` only counts the owned instructions by mnemonic and exits, without writing C.
The census (`--census-out`, default `generated/analysis/emit_census.json`) lists each
mnemonic with its count and handler group. `--max-unhandled N` makes `emit` exit with 9 if
more than `N` words are emitted as unhandled.

For the committed function database and the pinned ELF, the code range holds 3,012,703 words
of 188 distinct mnemonics in 29,196 functions. `emit --chunks 256` writes 256 chunks and 173
import trampolines (467 `switch` statements, 3,741 cases), and `--max-unhandled 0` exits with 0.
These numbers come from running `emit --census` and `emit --chunks 256` on the committed
database. The heaviest mnemonics are `addi` (12.1%), `lwz` (11.7%), `rldicl` (8.1%) and `bc` (8.0%).

## A PRX module (libsre)

`emit` also reads a relocated PRX, the file `generated/inputs/libsre.reloc.elf` that the build with
`-Pup` writes. The module's code is emitted with `--symbol-prefix`, which every generated file name and
global symbol then carries, so that two images can be linked into one program:

```sh
python tools/ps3tool.py emit generated/inputs/libsre.reloc.elf --db config/libsre_functions.json --symbol-prefix libsre --chunks 4 --out generated/ppu/libsre --max-unhandled 0
```

`--symbol-prefix NAME` (`[a-z][a-z0-9]*`) is required for a PRX and refused for the main executable
(exit 2), so the executable is always the unprefixed image. An unrelocated PRX is refused (exit 3).
The `--report` and `--census-out` defaults get the prefix too
(`generated/analysis/libsre_emit_report.json`, `libsre_emit_census.json`). With the prefix `libsre`
(Pascal form `Libsre`) the output differs from the executable's as follows:

| Item | Executable | libsre |
| --- | --- | --- |
| Function, trampoline | `fn_<address>`, `imp_<address>` | `libsre_fn_<address>`, `libsre_imp_<address>` |
| Files | `ppu_chunk_NNN.c`, `ppu_imports.c`, `ppu_tables.c`, `ppu_funcs.h`, `sources.cmake` | `libsre_ppu_chunk_NNN.c`, `libsre_ppu_imports.c`, `libsre_ppu_tables.c`, `libsre_ppu_funcs.h`, `libsre_ppu_tables.h`, `libsre_sources.cmake` |
| Source list variable | `TOX2_PPU_GENERATED_SOURCES` | `TOX2_LIBSRE_PPU_GENERATED_SOURCES` |
| Tables | `kTox2PpuFuncMappings`, `kTox2PpuImports`, `kTox2PpuExecutableSha256` and their counts | `kTox2LibsrePpuFuncMappings`, `kTox2LibsrePpuImports`, `kTox2LibsrePpuExecutableSha256`, their counts, and the export table `kTox2LibsrePpuExports` with `kTox2LibsrePpuExportCount` |
| Trampoline body | `TOX2_HLE(i);` | `TOX2_HLE_IMPORT(kTox2LibsrePpuImports, i);` |

The trampolines of a module call `tox2HleCallImport` with a pointer into that module's own import
table, so a module needs no agreed numeric id and the executable's output stays byte-identical to what
it was before libsre. `libsre_ppu_tables.h` declares the tables inside `extern "C"` for `game_main.cpp`.
The export table has one row per function export of the module info, sorted by module and FNID:
`{descriptor address, fnid, module}`. Exports that share a descriptor have separate rows. `emit` fails
(exit 3) when an export's descriptor names a function that is not in the database. An empty table is
written with one zero entry and a count of 0.

The CMake function that adds the generated tree (`tox2_ppu_add_generated_tree`) includes the
executable's `sources.cmake` and `<dir>/libsre/libsre_sources.cmake`, and stops with the emit command
above when the second one is missing. `ppu_link` also registers and checks libsre's tables.

For the committed database `config/libsre_functions.json` (352 functions, see
[Function recovery](function-recovery.md#on-libsre)), the emit run gives:

- 352 functions, 30,136 instructions, 32 import trampolines, 267 export rows and 384 function
  mappings (the functions plus the trampolines). `--max-unhandled 0` exits with 0.
- 4 chunks of 84 to 91 functions and 7,371 to 7,644 instructions each, 1.96 MB of C in total. The
  heaviest function (`0x011d75dc`) has 851 instructions, and `emit` runs in under a second.
- 2 `switch` statements with 14 cases, 177 system call sites, no traps, 481 direct calls, 450 calls
  of import trampolines, 28 indirect calls and 15 direct tail calls.

## Chunks and build time

The size of the C output follows the code size: with `--chunks 256` the 29,196 functions
become about 197 MB of C in 260 files (256 chunks plus the four other files), and `emit` takes
about 24 s. The chunks hold between 6,135 and 17,789 instructions (median 11,775). A function is
never split across chunks, and the largest one in this executable, `fn_003a1b34`, has 15,255
instructions, so the chunk that holds it is the heaviest to compile.

The first release build of the 256-chunk tree (made from a function database recovered without the
Ghidra summary) took about 72 s from scratch at about 18 parallel jobs on a 16-CPU machine. The
heaviest compile, the chunk with `fn_003a1b34` (about 1.6 MB of C), needed about 317 MB and 3 s.
These are measurements from that run, not limits. If the compiler runs out of memory on a smaller
machine, lower the parallelism with the `CMAKE_BUILD_PARALLEL_LEVEL` environment variable
(`cmake --build` inside `tools/build.ps1` honours it) before changing the emitter.

## Contract header and stub runtime

Generated code depends only on the headers in `runtime/include/tox2/` (`tox2/ppu.h` includes
all of them). `ppu_context.h` is the contract between generated code and a runtime:

- the CPU context `Tox2PpuContext` (registers, condition register and XER as one byte per bit,
  FPSCR, VSCR, reservation, call trace) and the vector and FPR types;
- guest memory accessors for a flat big-endian 32-bit address space, where `base` must point
  at a reservation of 4 GiB plus a guard area;
- the hooks the runtime implements: `tox2Dispatch`, `tox2HleCall`, `tox2HleCallImport`,
  `tox2Syscall`, `tox2Yield`, `tox2Unhandled`, `tox2Trap`, the reservation and cache operations,
  `tox2Mftb` and the FPSCR moves;
- `Tox2ExportInfo` (function descriptor address, FNID and module name of one export), which the
  tables of a prefixed tree use.

`TOX2_TAIL` expands to a `do { ... } while (0)` block around the `musttail` return. Without the
block, a label directly in front of a tail call (the emitter writes `L_<address>:` before the
statement) makes C23 parse the attribute as a label attribute and reject it. One function of
this executable has that shape.

`ppu_int.h`, `ppu_fp.h` and `ppu_vmx.h` hold the inline helpers that emitted code calls.
Vector registers are stored byte-reversed relative to guest memory; the `TOX2_VR_*`
accessors take big-endian element numbers. Most helpers cite the RPCS3 interpreter handler
they follow.

`runtime/src/ppu_stub_runtime.cpp` (library `tox2_ppu_stub`) implements the hooks only far
enough to link and test generated code: dispatch works, and every hook that would need the
system (HLE calls including `tox2HleCallImport`, unknown targets, unhandled words, traps) prints a
message and exits with an error status. Syscalls only set `r3` to 0. `ppu_stub.h` holds test-only additions. The stub
is only for the link check, the header check and the semantics harness; the runtime that runs the
game is described in [Runtime internals](runtime.md).

## Semantics harness

`tools/tests/ppu_semantics/` holds hand-written cases, one module per instruction group
(`cases_<group>.py`). `tools/tests/ppu_semantics/gen.py` assembles each case, runs it through
the emitter's own `emit_function`, and writes `ppu_semantics_cases.{h,c}` into the build
directory. `runtime/tests/ppu_semantics_main.c` runs every case natively: it sets the initial
state, calls the emitted function, and compares registers, flags and memory with the expected
state. CTest runs it as `ppu_semantics`; it needs neither the ELF nor a generated tree.

To add a case, append to `CASES` in the module of the instruction's group:

```python
case("addi_wraps_64", "ADDI", enc("addi", rD=3, rA=4, SIMM=1), {"r4": M64}, {"r3": 0})
```

The arguments are a unique name (`[a-z0-9_]+`), the reference (the RPCS3 handler or the
architecture rule the expected value comes from), the instruction words (built with
`tools/tests/ppc_asm.py`), the initial state and the expected state. State keys are `rN`,
`fN` and `vN` registers, `crN` field nibbles, `lr`, `ctr`, `vrsave`, `fpscr`, and the flags
`xer_so`, `xer_ov`, `xer_ca`, `vscr_sat` and `vscr_nj`. `Case` in
`tools/tests/ppu_semantics/__init__.py` also takes memory images (`mem_init`, `mem_expect`),
extra callee functions and switch targets. Expected values are derived by hand, not by
running the emitter.

## What is and is not implemented

Every instruction word in the code range of the pinned ELF has an emitter handler, so
`unhandled` in the report is 0. A word that needs a new handler goes into the module of its
group (`tools/ps3recomp/emit_ops_<group>.py`) together with semantics cases. Known deviations from the architecture or from RPCS3:

- FPSCR is modelled only as far as RPCS3 models it. `fcmpu` and `fcmpo` write the FPCC
  field; `mffs`, `mtfsf`, `mtfsfi`, `mtfsb0` and `mtfsb1` move the raw image. Exception
  flags, the enables and the rounding-control bits are never derived from an operation or
  acted on: rounding is always to nearest even and non-IEEE mode is not modelled. `mcrfs`
  does not clear the copied exception bits. With Rc=1, CR1 receives FPSCR bits 0..3, as the
  architecture defines (RPCS3 writes other bits).
- Scalar floating-point arithmetic is host double arithmetic. The sign of a default NaN that
  an invalid operation produces is the host's (negative on x86-64); it is not normalized to
  the PowerPC value. The vector floating-point helpers do normalize: a NaN result becomes
  `0x7fc00000`.
- `vsl` and `vsr` use the low three bits of byte 15 of `vB` as the shift count, which is the
  architectural definition (the result is undefined there when the bytes of `vB` differ).
  RPCS3 shifts each byte by its own count, so the two differ for such inputs.
- VSCR keeps NJ and SAT as separate fields in the context. `mfvscr` and `mtvscr` place them at
  bits 16 and 0 of word 3 of the register in big-endian numbering, as in the AltiVec manual.
  RPCS3's interpreter and its LLVM translator disagree on that word, and the manual's
  position is used.
- `fres`, `frsqrte` and the vector estimate instructions return exactly computed results,
  which lie within the architected estimate error, not the hardware estimate values.
- The stub runtime does not implement import calls, lv2 syscalls, threads or memory
  allocation: it exits on import calls and returns 0 from syscalls. The runtime in `tox2_game`
  implements a subset; see [Runtime internals](runtime.md).
