# Applying the method to another PS3 game

This project recompiles one game, but most of its tools and runtime are not specific to it. It was
first written for another game and then pointed at this one, so the steps below are also the ones
that had to be repeated for the second executable, which is about 15 times larger. They are listed
in the order they were taken, with what to check at each one. The other files in `docs/` describe
each tool in detail.

## 1. Get and pin the executable

- Decrypt your own copy's `EBOOT.BIN` to `EBOOT.elf` with `tox2_decrypt` (see
  [Decrypting the inputs](building.md#decrypting-the-inputs)); it needs only the keys that the build
  downloads, not an emulator. Keys and decrypted files stay out of version control.
- Check the `TITLE` in `PARAM.SFO`; a directory name can belong to a different game.
- Record the size and SHA-256 of the executable in `config/target.json`, so every tool and the
  generated code refuse a different build (see [Target pin](analysis-tools.md#target-pin)).

## 2. Check feasibility

Run the analysis tools first (see [Analysis tools](analysis-tools.md)); this takes about an hour:

- The ELF summary: a 64-bit big-endian PowerPC executable, its process parameters, TLS segment and
  entry descriptor.
- The import stubs: which modules and functions the game imports (FNIDs resolved through
  `config/nids.json`). This is the list of HLE work ahead.
- The SPU probe: embedded SPU ELF images and SPU-related imports and system calls. A game that
  creates no SPU thread groups and only initialises and finalises SPURS needs no SPU recompiler;
  one that runs SPU code does, and that is not covered here.
- Data inside `.text` (permute control vectors, switch tables): never assume every word is code.
- The size of the code: count the `.text` words and the functions before the first `emit`. The first
  game had about 210 thousand code words in 2.5 thousand functions; this one has 3.3 million words
  in 29 thousand functions, which changes how the output is chunked (step 4).

## 3. Disassemble and recover functions

- Validate the decoder against Ghidra once (see [Ghidra cross-check](analysis-tools.md#ghidra-cross-check));
  differences in rarely used instructions show up there. Ghidra does not decode some Cell
  instructions (the `lvlx`/`lvrx` family), so expect it to cut some functions short; the Cell PPU
  language of `ghidra-export --cell` removes that truncation.
- Recover function extents with `recover` (see [Function recovery](function-recovery.md)): Ghidra's
  starts and data references as seeds, then the own partition. Look at the real `bctr` sites
  before relying on the switch table matcher, add manual seeds for small wrappers that fall
  through into their callee, feed new starts back to Ghidra and repeat.
- Check how many TOC bases the function descriptors in `.opd` carry. A switch loads its table
  address relative to the TOC register of its function, so a matcher that knows only the entry
  TOC reads the wrong table, or none, for functions that use another one. This executable has two
  (`0x010b6330` and `0x010c62d0`), and the second belongs to a large share of the code. The
  symptom was a switch left unresolved, then an unknown dispatch target at run time and a fault
  at a small guest address. `recover` now collects the TOC set, tries each TOC per site and prefers
  the TOC of the owning function's descriptor when they disagree (see
  [Function recovery](function-recovery.md)).
- Commit the function database, so later builds do not need Ghidra.

## 4. Emit C

- `emit` writes one C function per guest function with one signature, so every call and tail
  call can be a `musttail` call (see [Emitting C](emitter.md)).
- Implement instructions in order of frequency, and give each group cases in the semantics harness
  that are derived by hand from the architecture manual or a reference interpreter.
- Choose the number of chunks from the size of the code. The default of 16 gives chunks of about
  13 thousand instructions for 210 thousand code words; 3 million words need `--chunks 256` for
  chunks of a similar size. One huge function sets a floor for the heaviest compile. If a compile
  runs out of memory, lower the build parallelism with the `CMAKE_BUILD_PARALLEL_LEVEL`
  environment variable before touching the emitter. `recover`, `emit` and the Ghidra export all
  take longer on a large image (the Ghidra export needs a larger `--timeout`), but `recover` and
  `emit` still finish in well under a minute here. See [Emitting C](emitter.md#chunks-and-build-time).
- A second executable can break assumptions the first never tested. A label directly in front of a
  `musttail` return is parsed by C23 as a label attribute and rejected, so the tail-call macro
  must be a block (`TOX2_TAIL`, see [Emitting C](emitter.md#contract-header-and-stub-runtime)).
  Expect one or two fixes of this kind and fix them in the emitter or the header, not in the
  generated files.
- The PS3 runs PowerPC in 64-bit mode: record forms compare signed 64-bit values, and `dcbz`
  clears 128 bytes.

## 5. Bring up the runtime

- Take the initial thread state (stack, registers, argument block, TLS) from how RPCS3 starts a
  process instead of from memory.
- Start with default handlers for every import and system call: they log, count and return 0, and
  the census at the end of a run shows what the game used (see [Census](runtime.md#census)). Then
  implement what the census and the stops point at, in that order.
- A default handler does not fill the game's out-parameters. When a run faults at a small address,
  look for the last default-handled call that was passed a pointer (the census marks those). A
  fault at a small address can also come from an unresolved jump table (see step 3).
- A game that checks its own data (`cellGameBootCheck`, a stat of its archive) may take an
  install path when the default handler leaves a directory name empty. Look at what the main
  thread calls just before a surprising turn, not only at the crash.
- Pin progress with the first call that the main thread makes after each new milestone (see
  [The `game_boot` test](tests.md#the-game_boot-test)); lines from other threads depend on
  scheduling.
- Never let the runtime abort into a modal dialog; unattended runs must end with an exit code.

## 6. Graphics, files and media

- The fake RSX and its census show which methods, formats and shader programs the game uses
  before any rendering exists (see [Fake RSX](rendering.md#fake-rsx)). Translate only those, and use
  capture and replay to work on single frames without running the game.
- The [virtual file system](virtual-file-system.md) maps guest paths to host files. Check how the
  game reads its data: this one has a single archive of about 3 GB, so file offsets above 2 GiB
  must survive the `read` and `seek` code.
- Movies: check the container and codecs of the game's files first. The first game used PAMF with
  H.264 and ATRAC3plus, which the inherited code demuxes itself and decodes with FFmpeg as LGPL
  DLLs. This game uses Bink, which is not handled (see [Movies](movies.md)).

## 7. Licences

The reference emulators and recompilers are GPL or MIT licensed. Read them for behaviour, but do
not copy their code. Where an implementation must follow GPL code closely (a decompressor, a
container format), use a clean-room process: one person or agent reads the reference and writes a
prose description of the format, and another implements it from that description alone. The PAMF
demuxer in this repository was written that way.
