# Status

What works on the pinned executable today, and what does not. The [top-level README](../README.md)
has the short version.

The game does not run in any useful sense. `tox2_game` loads the executable, runs its start-up code
on real guest threads for a few seconds and then stops at a guest fault. This is work in progress.
The code was taken over from an earlier recompilation project of another game and is being pointed
at *Tales of Xillia 2* (BLJS10188); the parts that this game has not reached have never run against
its executable.

## What has been run

Three headless runs (`--no-window --seconds 20`; the first with an empty `--hdd-dir`, the other two
reusing it) of the build from the generated code ended the same way:

- The runtime loaded the executable and booted the main thread. The stop was
  `tox2: stop: fatal guest fault: write to guest address 0x00000000` (exit code 3) on the GCM
  interrupt thread `0x01000002`, in fn `0x008a090c` (return address `0x008a0940`), well before the
  watchdog. The registers, a call trace and the census were printed every time. The census listed no
  unknown dispatch target: every call and jump that ran before the stop reached a recovered
  function. The call that precedes the fault is the default-handled `cellGcmGetLabelAddress`, which
  returns 0 where the game expects an address; that link has not been tested.
- `cellGameBootCheck` has a handler (a disc boot, see [Runtime internals](runtime.md#implemented-imports)),
  so the game's data paths now start with `/dev_bdvd/PS3_GAME`: the stats of `TLFILE.TLDAT`,
  `FILEHEADER.TOFHDB` (both in `USRDIR`) and `ICON0.PNG` succeed.
- First run: `cellGameDataCheck` for `BLJS10188_INSTALL` finds no directory, so thread
  `0x01000007` calls `cellGameCreateGameData`, opens a message dialog and copies the three files
  from the disc to the temporary game data directory with `cellFsRead` and `cellFsWrite` (724 of
  each in total; `TLFILE.TLDAT` is 3,017,411,417 bytes, `FILEHEADER.TOFHDB` 10.6 MB). The copy is
  complete within the 20 s. `cellGameContentPermit`
  then moves it to `/dev_hdd0/game/BLJS10188_INSTALL`, the dialog is closed and the main thread waits
  on an event flag. The reads are sequential and cross 2 GiB; `cellFsLseek` was not called.
- The two later runs find the installed directory: the game reads `FILEHEADER.TOFHDB` from the disc
  again, opens the installed copy, stats and fstats it, calls the memory container calls 324 and 343
  (default-handled) and runs on until the same fault.
- The last line of the form `tox2: first ...` that the main thread (`[t 01000000]`) wrote in all
  three runs is `first sc 85 sys_event_flag_wait`. This is the milestone that the `game_boot` test
  pins (see [Tests](tests.md#the-game_boot-test)). The test run leaves the 3 GB copy in the build
  directory's `game_hdd`.
- `cellSpursEventFlagWait` and `_cellSpursLFQueuePushBody` (the path that the Edge LZMA taskset
  would take) were not called. `TLFILE.TLDAT` was read only as the sequential copy above; nothing
  parses it.

What the census of the first run showed (the other two runs differ in counts that depend on
scheduling, and in the system calls 324 and 343):

- System calls: 26,422 calls, 13 distinct, 1 default-handled. The default-handled one is number 45
  (`sys_ppu_thread_detach`), which the runtime does not name or implement. `sys_ppu_thread_yield`
  accounts for 26,255 of the calls.
- Imports: 67,507 calls, 78 distinct, 26 default-handled. The default-handled ones are 19
  `cellSpurs` functions (attribute, taskset, task, queue, event flag and lock-free queue setup),
  `cellRtcGetCurrentTick`, `cellRtcConvertUtcToLocalTime`, `cellGcmGetFlipStatus`,
  `cellGcmGetLabelAddress`, `cellVideoOutGetResolution`, `cellVideoOutSetGamma` and
  `cellSaveDataListAutoLoad`. Each is logged with the argument registers that look like
  out-pointers; the game's later behaviour may depend on the values that were never written.
- No SPU code ran. The game set up a SPURS instance and six tasksets with 30 tasks through the
  default handlers (nothing was created behind them), and the only SPU system call was
  `sys_spu_initialize`. No SPU thread group or raw SPU system call was issued before the stop.
- RSX: 186 FIFO headers, no flip, so no frame was drawn or displayed. `rsx unimplemented` listed
  one kind, the ZPASS report constant.
- Threads: 8 created, 5 live at the stop (the main thread and a worker blocked in
  `sys_event_flag_wait`, the GCM interrupt thread that faulted, a worker blocked in
  `sys_timer_usleep` and one running; the install thread and two others had exited). The game's own
  threads are named `CThread`.

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
  handler for 111 of them (`tox2: hle: 173 imports, 111 with handlers`); the other 62 get the
  default handler (see [Runtime internals](runtime.md#default-handlers)). Among them are
  `cellGcmGetLabelAddress`, `cellVideoOutGetResolution`, `cellAudioOutGetConfiguration`,
  `sys_spu_image_import` and 28 of the 30 `cellSpurs` functions.
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
- The game's data archive `TLFILE.TLDAT` (about 3 GB) and its index `FILEHEADER.TOFHDB` are only
  copied by the game's install step, and no reader for their format exists. The file system code
  uses 64-bit offsets (see [Virtual file system](virtual-file-system.md#large-files)); the copy
  read the whole archive sequentially, but no read at a chosen offset above 2 GiB has happened.
- Movies: the game's movies are Bink files (`USRDIR/MOVIE/*.DAT`). Bink decoding is not
  implemented.
- Imports and system calls without a handler (see above), and imports whose handler is minimal.
  The census at the end of a run lists what the game called and what the default handler served.
  The next step is to give `cellGcmGetLabelAddress` a handler and see how far the game then gets.
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
