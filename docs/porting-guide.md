# Applying the method to another PS3 game

This project recompiles one game, but most of its tools and runtime are not specific to it. This
guide lists the steps in the order they were taken here, with what to check at each one. The
other files in `docs/` describe each tool in detail.

## 1. Get and pin the executable

- Decrypt your own copy's `EBOOT.BIN` to `EBOOT.elf` with RPCS3 (Utilities > Decrypt PS3
  Binaries). Keys and decrypted files stay out of version control.
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

## 3. Disassemble and recover functions

- Validate the decoder against Ghidra once (see [Ghidra cross-check](analysis-tools.md#ghidra-cross-check));
  differences in rarely used instructions show up there. Ghidra does not decode some Cell
  instructions (the `lvlx`/`lvrx` family), so expect it to cut some functions short.
- Recover function extents with `recover` (see [Function recovery](function-recovery.md)): Ghidra's
  starts and data references as seeds, then the own partition. Look at the real `bctr` sites
  before relying on the switch table matcher, add manual seeds for small wrappers that fall
  through into their callee, feed new starts back to Ghidra and repeat.
- Commit the function database, so later builds do not need Ghidra.

## 4. Emit C

- `emit` writes one C function per guest function with one signature, so every call and tail
  call can be a `musttail` call (see [Emitting C](emitter.md)).
- Implement instructions in order of frequency, and give each group cases in the semantics harness
  that are derived by hand from the architecture manual or a reference interpreter.
- The PS3 runs PowerPC in 64-bit mode: record forms compare signed 64-bit values, and `dcbz`
  clears 128 bytes.

## 5. Bring up the runtime

- Take the initial thread state (stack, registers, argument block, TLS) from how RPCS3 starts a
  process instead of from memory.
- Start with default handlers for every import and system call: they log, count and return 0, and
  the census at the end of a run shows what the game used (see [Census](runtime.md#census)). Then
  implement what the census and the stops point at, in that order.
- A default handler does not fill the game's out-parameters. When a run faults at a small address,
  look for the last default-handled call that was passed a pointer (the census marks those).
- Pin progress with the first call that the main thread makes after each new milestone (see
  [The `game_boot` test](tests.md#the-game_boot-test)); lines from other threads depend on
  scheduling.
- Never let the runtime abort into a modal dialog; unattended runs must end with an exit code.

## 6. Graphics, files and media

- The fake RSX and its census show which methods, formats and shader programs the game uses
  before any rendering exists (see [Fake RSX](rendering.md#fake-rsx)). Translate only those, and use
  capture and replay to work on single frames without running the game.
- Encrypted `.sdat` files are decrypted once into a cache; the [virtual file system](virtual-file-system.md)
  maps guest paths to host files.
- Movies: check the container and codecs of the game's files first. Here, PAMF with H.264 and
  ATRAC3plus is demuxed by own code and decoded by FFmpeg as LGPL DLLs (see [Movies](movies.md)).

## 7. Licences

The reference emulators and recompilers are GPL or MIT licensed. Read them for behaviour, but do
not copy their code. Where an implementation must follow GPL code closely (a decompressor, a
container format), use a clean-room process: one person or agent reads the reference and writes a
prose description of the format, and another implements it from that description alone. The PAMF
demuxer and the SDAT decompressor in this repository were written that way.
