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
The SPU programs of the game and of libsre are emitted by a third command, `spu-emit`; see
[SPU images](#spu-images). A build with `-GeneratedDir` needs all three trees.

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
`emit` prints a summary on stdout. Its last line is `overrides: <n>`, followed by the entries in
parentheses when there are any:

| Field | Contents |
| --- | --- |
| `functions`, `instructions`, `import_trampolines` | Emitted counts |
| `thunks_without_toc_save` | Import stubs that do not save the caller's TOC pointer |
| `chunks`, `largest_functions`, `output_bytes` | Size of the output |
| `unhandled` | Words emitted as a call to `tox2Unhandled`: total, per mnemonic, per group, with example addresses |
| `edges` | Calls, tail calls, switches, syscalls and traps by kind, for example `calls_direct` and `calls_indirect` |
| `dynamic_targets` | Transfers whose constant target is not a recovered function or stub, with site and target |
| `overrides` | The functions replaced by hand-written code, with entry and reason; see [Overrides](#overrides) |
| `labels`, `loop_hooks`, `elapsed_s` | Emitted labels, loop hooks, run time |

`--census` only counts the owned instructions by mnemonic and exits, without writing C.
The census (`--census-out`, default `generated/analysis/emit_census.json`) lists each
mnemonic with its count and handler group. `--max-unhandled N` makes `emit` exit with 9 if
more than `N` words are emitted as unhandled.

For the committed function database and the pinned ELF, the code range holds 3,025,208 words
of 188 distinct mnemonics in 29,196 functions. `emit --chunks 256` writes 256 chunks and 173
import trampolines (480 `switch` statements, 3,858 cases), and `--max-unhandled 0` exits with 0.
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

## Overrides

A guest function can be replaced by hand-written C. Every direct call, tail call and dispatch table
row binds the symbol `fn_<address>`, so a source that defines that symbol replaces the function for
all of them, with no run-time cost. The override file names the functions, and `emit` stops emitting
`fn_<address>` for them:

```json
{
  "overrides": [
    {
      "entry": "0x00956a00",
      "generated_sha256": "<64 hex digits>",
      "reason": "why the generated code is not enough"
    }
  ]
}
```

`emit --overrides PATH` reads the file. The default is `config/ppu_overrides.json`; a missing default
file means no overrides, and without overrides the output is byte-identical to what it would be without
this feature. A missing file named with `--overrides` is an I/O error (exit 1). The option cannot be
combined with `--symbol-prefix` (exit 2), and a prefixed image never reads the default file.

- `entry` is a hex address (`0x` and one to eight digits) of a function in the database. `reason` is a
  non-empty string. The three keys are required and no other key is allowed.
- `generated_sha256` pins the code the override was derived from. It is the SHA-256 of the UTF-8 text
  that `emit` writes for the function under the name `fn_<address>`, that is the title comment (the
  database name, if any) and the function body as they appear in a chunk without `--comments`,
  including the final newline. It is computed whatever `--comments` says. When the ELF, the database
  or the emitter changes the text, the pin fails and the override has to be derived again.
- A bad address, a duplicate entry, an entry that is not a function of the database, an empty
  `reason`, a malformed file or a `generated_sha256` mismatch fails with exit 3 before any file is
  written. The mismatch message prints the hash that `emit` computed.

For an overridden function the chunk holds the body under the name `fn_<address>_generated`. Nothing
calls it; it stays so that the override can be reviewed against the generated code, and it still counts
in the chunk plan. `ppu_funcs.h` still declares `fn_<address>`, and `ppu_tables.c` still maps the guest
address to `fn_<address>`. The C sources of the overrides are not named in the file: the build lists
them, and the linker reports a function with an entry but no source (undefined `fn_<address>`) and a
source without an entry (defined twice).

## SPU images

`spu-emit` turns the committed SPU function databases (`config/spu/<name>.json`, written by `spu-recover`; see
[Function recovery](function-recovery.md#on-the-spu-images)) and the image bytes into C source. It is the
third emit command of a build; the registry, the databases and the inputs are the ones of the other `spu-*`
commands (see [SPU images](analysis-tools.md#spu-images)):

```sh
python tools/ps3tool.py spu-emit --eboot generated/inputs/EBOOT.elf --libsre generated/inputs/libsre.reloc.elf --out generated/ppu/spu --max-unhandled 0
```

`--out` defaults to `generated/ppu/spu` (git-ignored), the directory that `-GeneratedDir generated/ppu` makes
CMake read. `--db-dir` (default `config/spu`) selects the databases, `--images a,b,...` a subset, `--report`
the report (default `generated/analysis/spu_emit_report.json`) and `--comments` annotates the code with the
disassembly. `spu-emit` refuses a database whose recorded hashes do not match the image (exit 3). The
output is deterministic and a file is only rewritten when its content changes, so a second run prints
`output: 22 files, 0 written`. `--census` counts the instructions and the unhandled words of each image
and writes no files.

| File | Contents |
| --- | --- |
| `spu_<image>_NNN.c` | The functions of one image in address order. A new chunk starts once 4096 instructions are reached; a function is never split |
| `spu_tables.c` | Per image the segments (guest address, LS address, size, code flag), the dispatch entries and the `kTox2SpuImages` table with the image's SHA-256 |
| `spu_funcs.h` | Declarations of the functions; includes `tox2/spu_ops.h` and `tox2/spu_lower_stackfree.h` |
| `spu_sources.cmake` | `TOX2_SPU_GENERATED_SOURCES`, the list of sources that `tox2_spu_generated` compiles |

- Every function has the form `TOX2_SPU_FUNC(spu_<image>_<LS in 5 hex digits>)`, that is
  `void (Tox2SpuContext* __restrict ctx, uint8_t* __restrict ls)`. Guest memory is never visible to generated
  SPU code: only the runtime's channel hooks (the MFC among them) reach it.
- A dispatch entry is one block that an indirect branch can reach (the `dispatch_seeds` of the database),
  with the first one to four instruction words. The runtime compares these words with the local store
  before it jumps, so a block whose code is not the emitted code is detected instead of run.
- The semantics of an instruction and the way control moves are separate. The handlers of the six op
  groups (below) return C statements; the control group computes conditions and target expressions in
  `spu_emit_core.py`; the lowering module `tools/ps3recomp/spu_lower_stackfree.py` turns each transfer into
  C. The registry names the lowering per image (`lowering`, only `stackfree` exists). The stack-free
  lowering uses no host stack for the guest call stack:

  | Transfer | Emitted as |
  | --- | --- |
  | Branch to a block of the same function | `goto L_xxxxx;` (a backward edge also gets `TOX2_SPU_SF_LOOP(site)`, the place where a halt request is noticed) |
  | Branch or call to a dispatchable block of another function | the link register is written, then `TOX2_SPU_SF_JUMP(fn, target)`, a `musttail` call |
  | `bi`, `bisl`, `bisled`, `iret`, conditional indirect branches, a direct branch outside the image's code | the target is computed, the link is written, then `TOX2_SPU_SF_DISPATCH(site, target)` |
  | Fall-through off the end of a function | `TOX2_SPU_SF_JUMP` to the next function |
  | Halt, or a `stop` that ends the thread | `TOX2_SPU_SF_LEAVE()`, a `return` to the run loop |

  Every transfer between functions is a `musttail` call or a return, so the host stack holds the run loop
  frame, one generated frame and at most one hook frame. Every indirect branch is a computed dispatch: the
  resolutions in the database only supply entries and jump tables. A branch target is masked with
  `0x3fffc`, the E and D bits of an indirect branch set or clear `int_enabled` before the transfer, and
  `bisled` is taken when `tox2SpuChannelCount` of channel 0 is non-zero.
- The op groups follow the decoder's groups, one module `tools/ps3recomp/spu_emit_ops_<group>.py` each:
  `int` (arithmetic, logic, compare, select, multiply, byte, shift and rotate, and the conditional halts),
  `perm` (shuffle, quadword shifts and rotates, generate controls, select masks, gathers), `mem` (quadword
  loads and stores), `chan` (channels, `stopd`, `mfspr`, `mtspr`, `fscrrd`, `fscrwr`), `fp_single` and
  `fp_double`. A mnemonic whose group module is missing is emitted as a call to `tox2SpuUnhandled`.
  `sync`, `dsync`, `nop`, `lnop` and the branch hints emit no code, because the local store is private to the
  thread.
- The report is JSON. Per image it has `functions`, `instructions`, `dispatchable`, `chunks` (file, functions,
  instructions, bytes), `unhandled` (total, per mnemonic, per group, with example addresses), `edges`
  (`gotos`, `jumps`, `dispatch_direct`, `dispatch_indirect`, `fallthrough_jumps`, `fallthrough_dispatch`,
  `returns`, `stops`, `leaves`, `invalid_flow`), `invalid_flow`, `unlisted_targets`, `labels` and `loop_hooks`;
  at the top it has `gate_unhandled` and `ungated_unhandled`. Stdout has three lines per image in `--census`
  mode (counts, the commonest mnemonics, the unhandled ones) and one line per image otherwise, then
  `gate unhandled: N`.
- `--max-unhandled N` makes `spu-emit` exit with 9 if the gate images together have more than `N` unhandled
  words. Every emitted image is a gate image (a registry `gate` is true for each image with an LS base, which
  is every image today), so no `ungated unhandled: <image> <n>` line appears. The gate is checked after the files are written.
- A word whose group has no handler, an operand the handler refuses (a conversion scale outside 0..127), or a
  rounding mode in the double-precision operations (below) reaches `tox2SpuUnhandled` at run time.

For the committed databases the run gives 16 images, 719 functions, 71,532 instructions and 2,296 dispatch
entries, written as 25 chunk files and the three shared files (28 files, about 5.4 MB of C, under one
second). `gate unhandled: 0`, so `--max-unhandled 0` exits with 0; no mnemonic of any emitted image lacks a
handler. The edges are 3,242 `goto`s, 1,509 direct jumps, 851 indirect dispatches, 688 returns, 15 fall-through
jumps, 6 `stop`s and 4 leaves, with 2,462 labels and 1,299 loop hooks. Two flows end in a non-code word (`invalid_flow`
2: one each in `libsre_kernel1` and `libsre_kernel2`, where a fall-through runs into a non-code word); the
emitted code calls `tox2SpuUnhandled` there, and they are not counted as unhandled words. Per image:

| Image | Functions | Instructions | Dispatchable | Chunks |
| --- | --- | --- | --- | --- |
| `eboot_spline` | 16 | 1,106 | 41 | 1 |
| `eboot_edgelzma` | 38 | 3,823 | 87 | 1 |
| `eboot_bnuscore` | 133 | 18,248 | 518 | 5 |
| `eboot_animation` | 55 | 4,741 | 133 | 2 |
| `eboot_updatematrix` | 182 | 14,567 | 608 | 4 |
| `eboot_renderparticle` | 73 | 8,303 | 268 | 2 |
| `eboot_morph` | 62 | 3,745 | 145 | 1 |
| `eboot_skin` | 43 | 3,811 | 100 | 1 |
| `libsre_kernel1` | 4 | 366 | 7 | 1 |
| `libsre_kernel2` | 3 | 377 | 6 | 1 |
| `libsre_system_service` | 18 | 2,104 | 50 | 1 |
| `libsre_taskset_pm` | 19 | 1,766 | 89 | 1 |
| `libsre_taskset2_exit` | 5 | 401 | 9 | 1 |
| `libsre_exit_code_plugin` | 6 | 441 | 11 | 1 |
| `libsre_jobchain_pm1` | 32 | 3,872 | 114 | 1 |
| `libsre_jobchain_pm2` | 30 | 3,861 | 110 | 1 |

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

Generated SPU code has its own contract headers in the same directory. They compile as C23 and C++26 and
with `-ffreestanding`, and the test `spu_header_check` compiles the headers in all three ways:

- `spu_context.h` is the contract between generated SPU code and a runtime and does not depend on how
  control is lowered. It has the context `Tox2SpuContext` (128 registers as `Tox2Vr` with the PPU's
  byte-reversed lanes, so `TOX2_VR_W(v, 0)` is the preferred-slot word; FPSCR image; the dispatch input
  `jump_target`, the transferring instruction `jump_site`; `srr0`; the `halt` word that other threads set;
  `exit_ls`, the LS address whose dispatch returns to the run loop; `int_enabled`; the trace ring), the
  table types `Tox2SpuEntry`, `Tox2SpuSegment` and `Tox2SpuImageInfo` with the generated `kTox2SpuImages`,
  the inline local-store accessors (`tox2SpuLoadQ` and `tox2SpuStoreQ` wrap at the 256 KiB LS and ignore the
  low four address bits), and the hooks the runtime implements: `tox2SpuDispatch`, `tox2SpuReadChannel`,
  `tox2SpuWriteChannel`, `tox2SpuChannelCount`, `tox2SpuStop`, `tox2SpuHalt` and `tox2SpuUnhandled`. The
  channel and stop hooks return false when the thread must leave.
- `spu_ops.h` holds the inline semantics helpers of the integer, permute and memory groups. A helper takes
  the source registers by value and returns the new register, so `rt` may alias a source, and every shift
  count is in range, because the harness builds with UBSan traps. `spu_fp.h` is included at its end
  and holds the floating-point helpers.
- `spu_lower_stackfree.h` holds the `TOX2_SPU_SF_*` macros (`ENTER`, `LOOP`, `JUMP`, `DISPATCH`, `LEAVE`). It
  is the only header that knows the stack-free model; another lowering would add its own header and emitter
  module, and the context, the ops and the hooks stay.
- `spu_stub.h` is test support of the stub runtime (`tox2SpuStubSelectImage`, `tox2SpuStubSetLabel`) and
  not part of the contract.

`runtime/src/spu_stub_runtime.cpp` (library `tox2_spu_stub`) implements the SPU hooks only far enough to link
and test generated code: dispatch looks the target up in the image that the test selected, and every hook
that would need a thread (channels, stops, halts, unknown targets, unhandled words) prints a message and
exits. It defines the same symbols as the SPU runtime in `tox2_runtime_core`, so a program links one of the
two, never both. The link checks `spu_link` and `ppu_link` use the stub; `tox2_game`, `spu_programs` and
`spu_lzma_inflate` use the runtime, and the semantics harness has its own driver.

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

The SPU emitter has the same kind of harness in `tools/tests/spu_semantics/` (`cases_<group>.py` for `int`,
`perm`, `mem`, `control`, `chan`, `fp_single` and `fp_double`). `gen.py` encodes each case with the test
assembler `tools/tests/spu_asm.py`, which builds the word of any mnemonic of the decoder's table, runs the
words through recovery and the real emitter, and writes `spu_semantics_cases.{h,c}` into the build
directory. `runtime/tests/spu_semantics_main.c` is the whole runtime of a case: it dispatches over the case's
entry table, answers scripted channel reads and event counts, and records channel writes, stop codes, taken
halts and unhandled words, which a case can expect. The expected values come from the SPU ISA 1.2 text,
not from the emitter. CTest runs it as `spu_semantics`; pass case names as arguments to run only those.
`tools/tests/spu_programs/` assembles four whole programs (a sum over an array, an atomic counter, a jump
table with six threads, an event to the PPU), emits them into synthetic images through the real recovery
and emitter, and runs them through the lv2 SPU thread group calls on the runtime core (`spu_programs`).

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

For the SPU, every instruction word of the 16 emitted images has a handler (`gate unhandled: 0`), including the
single-precision and double-precision groups that the engine tasks use. A word that needs a new handler goes
into the module of its group (`tools/ps3recomp/spu_emit_ops_<group>.py`) together with semantics cases in
`tools/tests/spu_semantics/cases_<group>.py`. Known deviations from the architecture:

- Single precision is the SPU's extended range: an exponent field of 0 is zero whatever the other bits are
  (denormals are zero), 255 is an ordinary exponent, results are truncated toward zero to 24 bits, and a
  result below the smallest normal number is +0 or above the largest saturates to `sign | 0x7fffffff`.
  `spu_fp.h` computes this on bits, with the host `double` only as exact arithmetic (an exact product and an
  error-free sum), so it needs IEEE double arithmetic in the default environment and `-ffp-contract=off`,
  which the codegen options set. `fa`, `fs`, `fm`, `fma`, `fms` and `fnms` run as native single-precision
  instructions: `runSpu` sets MXCSR to round toward zero with FTZ and DAZ while generated code runs (and
  restores the caller's value), a zero result is turned into +0, and a vector with a result lane of
  magnitude 0x7f7fffff or more (an overflow, or an Inf or NaN from an exponent-255 operand) is recomputed
  exactly with the host `double` in round to nearest. The helpers that need round to nearest or
  denormals (the exact path, double precision, `frest`, `frsqest`, `frds`) switch MXCSR around their
  arithmetic, and the SPU targets are compiled with `-frounding-math` so that no float operation is folded
  or moved across a switch. Host code that SPU code calls runs in the same environment. The scalar
  `tox2SpuSf*` functions stay as the definition, and the `spu_vector_helpers` test compares the two.
- `frest` and `frsqest` return the exact reciprocal (square root) truncated to 24 bits, and `fi` returns its
  second operand unchanged, because the ISA does not publish the hardware's estimate tables. Results of
  sequences that use them (a reciprocal refined by `fi`, `fnms` and `fma`) stay within the ISA's one-ulp
  bound but can differ from the hardware's bits by an ulp.
- FPSCR is not derived from operations: no exception flag of either precision is ever set, and
  `fscrwr` stores the register as it is, and `fscrrd` returns it with the unused bits forced to zero.
- Double precision is host `double` arithmetic in round to nearest. A denormal operand is replaced by a zero
  of the same sign, and every NaN result is the default quiet NaN `0x7ff8000000000000`. Only round to
  nearest is implemented: when FPSCR selects another rounding mode (the rounding control bits `0x00000f00` of
  word 0), the rounding operations (`dfa`, `dfs`, `dfm`, `dfma`, `dfms`, `dfnms`, `dfnma`, `frds`) call
  `tox2SpuUnhandled` instead of computing another value, so the run stops with `spu: unhandled word` at that
  instruction. `eboot_updatematrix` has a routine at LS `0x0ef88` that rewrites FPSCR fields from its
  argument; whether the game ever selects a directed mode is not known.
- `mfspr` returns 0 and `mtspr` is ignored (the architecture implements no SPR); `sync` and `dsync` emit
  no code.
