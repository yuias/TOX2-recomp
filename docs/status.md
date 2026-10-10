# Status

What works on the pinned executable today, and what does not. The [top-level README](../README.md)
has the short version.

The game does not run in any useful sense. `tox2_game` loads the executable, runs its start-up code
on real guest threads for a few seconds and then stops at a guest trap. This is work in progress.
The code was taken over from an earlier recompilation project of another game and is being pointed
at *Tales of Xillia 2* (BLJS10188); the parts that this game has not reached have never run against
its executable.

## What has been run

Three headless runs (`--no-window --seconds 20`, each with its own empty `--hdd-dir`) of the build
from the generated code ended the same way:

- The runtime loaded the executable and booted the main thread. The stop was
  `tox2: stop: fatal trap: at 0x007cac98` (exit code 3) on guest thread `0x01000007`, well before
  the watchdog. The registers, a call trace and the census were printed every time. The census
  listed no unknown dispatch target: every call and jump that ran before the stop reached a
  recovered function.
- Just before the trap the game printed a boost assertion through `sys_tty_write`:
  `expr:this->is_initialized()` in `boost::optional<T>::get()` with `T = int`
  (`optional.hpp`, line 578). The trap is the `tw` instruction of that assertion, not a runtime
  fault.
- The last line of the form `tox2: first ...` that the main thread (`[t 01000000]`) wrote in all
  three runs is `first hle sys_fs::cellFsStat`. This is the milestone that the `game_boot` test
  pins (see [Tests](tests.md#the-game_boot-test)). At the stop the main thread was blocked in
  `sys_timer_usleep`.
- The game stats `/TLFILE.TLDAT`, `/FILEHEADER.TOFHDB` and `/ICON0.PNG`, each without a directory
  prefix. They resolve to no device, so `cellFsStat` fails with `0x8001003a` (not mounted). A likely
  cause is that `cellGameBootCheck` has only a default handler: it returns success and leaves the
  directory name that the game expects as an out-parameter empty. This has not been established.
  After the failed stat the game takes its game-data install path: thread `0x01000007` calls
  `cellGameCreateGameData`, opens a message dialog and opens `TLFILE.TLDAT` in the temporary game
  data directory under `--hdd-dir`, which creates an empty file. The assertion and the trap follow
  within that thread. Whether the assertion comes from the empty file or from something else has
  not been checked.
- `TLFILE.TLDAT` from the game directory was never opened, so nothing has read from it, with an
  offset above 2 GiB or otherwise.

What the census of the first run showed (the other two runs differ only in counts that depend on
scheduling):

- System calls: 34,225 calls, 11 distinct, 1 default-handled. The default-handled one is number 45
  (`sys_ppu_thread_detach`), which the runtime does not name or implement. `sys_ppu_thread_yield`
  accounts for 34,066 of the calls.
- Imports: 81,299 calls, 71 distinct, 26 default-handled. The default-handled ones are 19
  `cellSpurs` functions (attribute, taskset, task, queue, event flag and lock-free queue setup),
  `cellGameBootCheck`, `cellRtcGetCurrentTick`, `cellRtcConvertUtcToLocalTime`,
  `cellGcmGetFlipStatus`, `cellVideoOutGetResolution`, `cellVideoOutSetGamma` and
  `cellSaveDataListAutoLoad`. Each is logged with the argument registers that look like
  out-pointers; the game's later behaviour may depend on the values that were never written.
- No SPU code ran. The game set up a SPURS instance and six tasksets with 30 tasks through the
  default handlers (nothing was created behind them), and the only SPU system call was
  `sys_spu_initialize`. No SPU thread group or raw SPU system call was issued before the stop.
- RSX: 186 FIFO headers, no flip, so no frame was drawn or displayed. `rsx unimplemented` listed
  one kind, the ZPASS report constant.
- Threads: 8 created, 6 live at the stop (the main thread and two worker threads blocked in
  `sys_timer_usleep`, the GCM interrupt service thread, one worker running and the thread that
  trapped, which the census lists as running). The game's own threads are named `CThread`.

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
  [Rendering](rendering.md)). This game has not reached a flip, so neither has been exercised with
  it.
- Sound through the runtime's audio engine and WASAPI (see [Audio](audio.md)). The game has not
  initialised `cellAudio` before the stop.
- cellSail and cellPamf HLE, the PAMF demuxer, the host movie player and FFmpeg movie decoders.
  They are built but unused: this game imports neither module, so their handlers are never bound
  (see [Movies](movies.md)).

## What has been measured

Measured on the executable with the analysis tools, and confirmed by the load log of the runs:

- The image fits the guest memory map: the two loaded segments end at `0x011c6860`, and about
  238 MiB of the main region (`0x00010000..0x10000000`) remain after them.
- `.text` is `0x00010240..0x00c8f3d4`, 3,275,877 words. 9,687 words (0.30%) do not decode. The
  committed function database holds 29,196 functions, and the emitter reports no unhandled
  instruction form.
- The executable imports 173 functions from 17 modules, all with known names. The runtime has a
  handler for 107 of them (`tox2: hle: 173 imports, 107 with handlers`); the other 66 get the
  default handler (see [Runtime internals](runtime.md#default-handlers)). Among them are
  `cellFsRead`, `cellFsLseek`, `cellFsWrite`, `cellGameBootCheck`, `cellVideoOutGetResolution`,
  `cellAudioOutGetConfiguration`, `sys_spu_image_import` and 28 of the 30 `cellSpurs` functions.
- It embeds 8 SPU images and has 267 system call sites; the probe leaves 4 of them unresolved.
- The game uses 24 system call numbers that the runtime has no implementation for, among them the
  semaphore calls 90, 92 and 93, the SPU thread group calls 170 to 178, and the vm and memory
  container calls. None of them ran before the stop.

## Not implemented

- SPU and SPURS: the game embeds 8 SPU images, uses SPURS and creates raw SPU thread groups. Of the
  SPU system calls only `sys_spu_initialize` and the event connection of a thread group keep
  bookkeeping; no SPU code runs, and SPURS workloads are not scheduled. Most `cellSpurs` calls the
  game made are default-handled and return success without creating anything. Whether and how to
  run SPU code is undecided; [SPU usage](spu.md) describes what the game runs on the SPUs.
- The game's data archive `TLFILE.TLDAT` (about 3 GB) and its index `FILEHEADER.TOFHDB` are not
  read, and no reader for their format exists. The file system code uses 64-bit offsets (see
  [Virtual file system](virtual-file-system.md#large-files)), but no read of the real archive has
  happened.
- Movies: the game's movies are Bink files (`USRDIR/MOVIE/*.DAT`). Bink decoding is not
  implemented.
- Imports and system calls without a handler (see above), and imports whose handler is minimal.
  The census at the end of a run lists what the game called and what the default handler served.
  The next step is to give `cellGameBootCheck` a handler and see how far the main thread then gets.
- Graphics: only what the inherited fake RSX and renderer implement. Whatever the renderer meets and
  cannot handle is listed in the `rsx unimplemented` census (see
  [Rendering](rendering.md#rendering)). Known gaps are the ZPASS report values (the constant 0 is
  written, no occlusion query runs) and user clip planes (the vertex translator does not write the
  clip distances). Nothing in this game's command stream beyond the first 186 headers has been seen.
- Audio: WASAPI is the only device output, in shared mode on the default device. The output is
  stereo (an 8-channel port is reduced to its front pair), and there is no volume setting or device
  choice. ATRAC3 (non-plus) is not supported. Only the `cellAudio` and `cellAtrac` calls of the
  earlier game are implemented; this game's use of them has not been checked, and it imports
  `cellAudioSetPortLevel`, `cellAtracCreateDecoderExt`, `cellAtracGetInternalErrorInfo` and
  `cellAtracGetVacantSize`, which have no handler.
- Input: the `neutral`, `none`, `keyboard`, `gamepad` and `keyboard+gamepad` pad providers. The
  keyboard has a fixed mapping, no analog sticks and no vibration; gamepads are read through
  GameInput with a fixed mapping. Only port 0 exists; there is no mouse or `cellKb` input. See
  [Input](input.md). The game initialised `cellPad` in the runs but never read it before the stop.
- Priority scheduling beyond the creation rule (a new thread with a numerically lower priority than
  its creator runs first), lv2 interrupt threads, savestates.
