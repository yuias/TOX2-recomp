# Status

What works on the pinned executable today, and what does not. The [top-level README](../README.md)
has the short version.

Nothing runs yet. This is work in progress. The code was taken over from an earlier recompilation
project of another game and is being pointed at *Tales of Xillia 2* (BLJS10188); most of it has not
been run against this game's executable. No run of `tox2_game` on this game has been recorded.

## Components

These exist and are generic, but were written for and checked against the earlier game only:

- Analysis tools for the PPU executable: ELF summary, SPU probe, disassembler, Ghidra cross-check,
  NID and syscall name tables, function recovery (see [Analysis tools](analysis-tools.md) and
  [Function recovery](function-recovery.md)).
- An emitter that turns the recovered functions into C source (see [Emitting C](emitter.md)).
- `tox2_game`, which loads the executable, runs it through the generated code on real guest threads,
  serves the lv2 system calls and HLE modules, and runs a fake RSX that consumes the game's command
  buffer (see [Runtime internals](runtime.md)).
- A Vulkan 1.3 renderer that draws what the RSX commands describe into a window or PNG files, and
  `tox2_rsxreplay`, which renders single recorded frames without the game (see
  [Rendering](rendering.md)).
- Sound through the runtime's audio engine and WASAPI (see [Audio](audio.md)).
- cellSail and cellPamf HLE, the PAMF demuxer, the host movie player and FFmpeg movie decoders.
  They are built but unused: this game imports neither module, so their handlers are never bound
  (see [Movies](movies.md)).

## What has been measured

Measured on the executable with the analysis tools, without running it:

- The image fits the guest memory map: the two loaded segments end at `0x011c6860`, and about
  238 MiB of the main region (`0x00010000..0x10000000`) remain after them.
- `.text` is `0x00010240..0x00c8f3d4`, 3,275,877 words. 9,687 words (0.30%) do not decode.
- The executable imports 173 functions from 17 modules, all with known names.
- It embeds 8 SPU images and has 267 system call sites; the probe leaves 4 of them unresolved.
- The runtime has no implementation for 24 of the system call numbers the game uses (among them
  the semaphore calls 90, 92 and 93, the SPU thread group calls 170 to 178, and the vm and
  memory container calls) and no HLE handler for 66 of the 173 imports (among them
  `cellFsRead`, `cellFsLseek`, `cellFsWrite`, `cellGameBootCheck`, `cellVideoOutGetResolution`,
  `cellAudioOutGetConfiguration`, `sys_spu_image_import` and 28 of the 30 `cellSpurs` functions).
  The default handlers return `CELL_OK` and write no out-parameters.

## Not implemented

- SPU and SPURS: the game embeds 8 SPU images, uses SPURS and creates raw SPU thread groups. SPU
  system calls only keep bookkeeping, no SPU code runs, and SPURS workloads are not scheduled.
  Whether and how to run SPU code is undecided.
- The game's data archive `TLFILE.TLDAT` (about 3 GB) and its index `FILEHEADER.TOFHDB` are not
  read. File offsets above 2 GiB have not been checked in the file system code.
- Movies: the game's movies are Bink files (`USRDIR/MOVIE/*.DAT`). Bink decoding is not
  implemented.
- Imports and system calls without a handler (see above), and imports whose handler is minimal.
  The census at the end of a run lists what the game called and what the default handler served.
- Graphics: only what the inherited fake RSX and renderer implement. Whatever the renderer meets and
  cannot handle is listed in the `rsx unimplemented` census (see
  [Rendering](rendering.md#rendering)). Known gaps are the ZPASS report values (the constant 0 is
  written, no occlusion query runs) and user clip planes (the vertex translator does not write the
  clip distances).
- Audio: WASAPI is the only device output, in shared mode on the default device. The output is
  stereo (an 8-channel port is reduced to its front pair), and there is no volume setting or device
  choice. ATRAC3 (non-plus) is not supported. Only the `cellAudio` and `cellAtrac` calls of the
  earlier game are implemented; this game's use of them has not been checked.
- Input: the `neutral`, `none`, `keyboard`, `gamepad` and `keyboard+gamepad` pad providers. The
  keyboard has a fixed mapping, no analog sticks and no vibration; gamepads are read through
  GameInput with a fixed mapping. Only port 0 exists; there is no mouse or `cellKb` input. See
  [Input](input.md).
- Priority scheduling beyond the creation rule (a new thread with a numerically lower priority than
  its creator runs first), lv2 interrupt threads, savestates.
