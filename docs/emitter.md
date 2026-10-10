# Emitting C

`emit` turns the function database and the ELF into C source. The flow is `recover`, then
`emit`, then a build with `-GeneratedDir`:

```sh
python tools/ps3tool.py recover <elf> -o config/functions.json
python tools/ps3tool.py emit    <elf> --out generated/ppu --max-unhandled 0
pwsh -NoProfile -File tools/build.ps1 -GeneratedDir generated/ppu -Test
```

`emit` reads the function database (`--db`, default `config/functions.json`), checks that it
matches the ELF's hash and code range, and emits every function of the database as one C
function named `fn_<address>`. Branches inside a function become labels `L_<address>` and
`goto`, and resolved jump tables become `switch`. Direct calls are C calls, tail calls are
`musttail` returns, and transfers to computed targets go through `dalDispatch`. Backward
edges carry a hook (`DAL_LOOP`) that counts a preemption budget. Import stubs become
trampolines `imp_<address>` that call the runtime hook `dalHleCall` with an index into the
generated import table. The output is deterministic: two runs on the same inputs write
byte-identical files, and a file is only rewritten when its content changes.

## Output

Everything goes to `--out` (default `generated/ppu`; git-ignored):

| File | Contents |
| --- | --- |
| `ppu_chunk_NNN.c` | The function bodies, split into `--chunks` files (default 16) of similar instruction counts, so that they compile in parallel |
| `ppu_imports.c` | The import trampolines |
| `ppu_tables.c` | `kDalPpuFuncMappings` (guest address to host function, one row per function and per trampoline), `kDalPpuImports` (stub address, FNID, module, name) and the executable's SHA-256 |
| `ppu_funcs.h` | Declarations of the functions and trampolines |
| `sources.cmake` | The list of sources that `DAL_PPU_GENERATED_DIR` includes |

Chunk files that a smaller `--chunks` makes unnecessary are removed. `--comments` annotates
the code with the guest disassembly; it is off by default because it makes the files larger.

The report (`--report`, default `generated/analysis/emit_report.json`) is JSON, and
`emit` prints a summary on stdout:

| Field | Contents |
| --- | --- |
| `functions`, `instructions`, `import_trampolines` | Emitted counts |
| `thunks_without_toc_save` | Import stubs that do not save the caller's TOC pointer |
| `chunks`, `largest_functions`, `output_bytes` | Size of the output |
| `unhandled` | Words emitted as a call to `dalUnhandled`: total, per mnemonic, per group, with example addresses |
| `edges` | Calls, tail calls, switches, syscalls and traps by kind, for example `calls_direct` and `calls_indirect` |
| `dynamic_targets` | Transfers whose constant target is not a recovered function or stub, with site and target |
| `labels`, `loop_hooks`, `elapsed_s` | Emitted labels, loop hooks, run time |

`--census` only counts the owned instructions by mnemonic and exits, without writing C.
The census (`--census-out`, default `generated/analysis/emit_census.json`) lists each
mnemonic with its count and handler group. `--max-unhandled N` makes `emit` exit with 9 if
more than `N` words are emitted as unhandled. For the committed function database and the
pinned ELF, the code range holds 206065 words of 168 distinct mnemonics in 2531 functions,
`emit` writes 16 chunks and 166 import trampolines, and `--max-unhandled 0` exits with 0.

## Contract header and stub runtime

Generated code depends only on the headers in `runtime/include/dal/` (`dal/ppu.h` includes
all of them). `ppu_context.h` is the contract between generated code and a runtime:

- the CPU context `DalPpuContext` (registers, condition register and XER as one byte per bit,
  FPSCR, VSCR, reservation, call trace) and the vector and FPR types;
- guest memory accessors for a flat big-endian 32-bit address space, where `base` must point
  at a reservation of 4 GiB plus a guard area;
- the hooks the runtime implements: `dalDispatch`, `dalHleCall`, `dalSyscall`, `dalYield`,
  `dalUnhandled`, `dalTrap`, the reservation and cache operations, `dalMftb` and the FPSCR
  moves.

`ppu_int.h`, `ppu_fp.h` and `ppu_vmx.h` hold the inline helpers that emitted code calls.
Vector registers are stored byte-reversed relative to guest memory; the `DAL_VR_*`
accessors take big-endian element numbers. Most helpers cite the RPCS3 interpreter handler
they follow.

`runtime/src/ppu_stub_runtime.cpp` (library `dal_ppu_stub`) implements the hooks only far
enough to link and test generated code: dispatch works, and every hook that would need the
system (HLE calls, unknown targets, unhandled words, traps) prints a message and exits with
an error status. Syscalls only set `r3` to 0. `ppu_stub.h` holds test-only additions. The stub
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
`unhandled` in the report is 0. Known deviations from the architecture or from RPCS3:

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
  allocation: it exits on import calls and returns 0 from syscalls. The runtime in `dal_game`
  implements a subset; see [Runtime internals](runtime.md).
