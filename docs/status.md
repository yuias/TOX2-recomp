# Status

What works on the pinned executable today, and what does not. The [top-level README](../README.md)
has the short version.

The game does not run in any useful sense yet. `tox2_game` loads the executable and the firmware's SPURS
module (`libsre`), runs the game's start-up code on real guest threads, runs SPURS (libsre's recompiled
PPU code and the recompiled SPU programs of the kernels and the game's tasks), loads the first archive
items and reaches the game's frame loop. Headless runs of 30 s and of 180 s end at the watchdog (no longer run has
been made). Nothing visible has been drawn and nothing has been heard.
This is work in progress.
The code was taken over from an earlier recompilation project of another game and is being pointed
at *Tales of Xillia 2* (BLJS10188); the parts that this game has not reached have never run against
its executable.

## What the current run does

The run below was made with the build of commit `e013aa8`. `libsre.prx` of firmware 4.93 is part of the program (see
[SPU usage](spu.md#where-the-spurs-code-lives)). `tox2_game` loads `libsre.reloc.elf` at `0x011d0000`,
registers its 352 functions for dispatch and binds all 30 `cellSpurs` imports of the game to libsre's
exported functions. Of the game's 173 imports, 120 have a handler, 30 are bound to libsre and 23 get the
default handler. The load log says `tox2: hle: 173 imports, 120 with handlers, 30 bound to libsre`, and
`tox2: hle: libsre: 32 imports, 30 with handlers` for libsre's own imports. It also checks the 14 emitted
SPU images against guest memory (`tox2: spu: 14 images ok`).

The headless run (`--no-window --seconds 30`; the `game_boot` test uses the same arguments) goes like this:

- The main thread runs the start-up code, creates the memory container and, from an empty `--hdd-dir`,
  the game's install step copies the three data files (see [Earlier runs](#earlier-runs)). The game
  creates SPURS instance A: libsre creates its semaphores, imports the version 1 SPURS kernel from
  `0x011f0380`, creates the SPU thread group `SpursTLGraphicsCellSpursKernelGroup` (5 threads, priority
  250, type `0x18`), the lightweight mutex and condition variable, the event queue and the helper threads.
  The creation rule of `sys_ppu_thread_create` (see [Threads](runtime.md#threads)) runs the helper threads
  first. `SpursHdlr0` starts the group and waits in `sys_spu_thread_group_join`.
- The five SPU threads run the kernel, which loads the system service module and the taskset policy
  module. The game creates the Edge LZMA taskset and the engine tasksets (skin, morph, animation, update
  matrix, render particle). The LZMA task inflates the first archive item, which matches its oracle
  SHA-256, and the engine tasks are dispatched. At the stop four of the five SPU threads idle in the
  system service and one runs an LZMA task.
- The main thread also initialises `cellAudio` and creates the sound engine's thread group `bnusCore`
  (binding an SPU event queue to its thread before the group start) and SPURS instance B. The bnusCore
  thread runs, waits in `stop 0x110` and handles the commands that the PPU sends; in a 300 s run it ends
  blocked in `sys_spu_thread_receive_event` and the run ends at the watchdog with no unknown target.
- The main thread passes the LZMA load and writes `tox2: first hle cellSpurs::cellSpursEventFlagTryWait`
  (at 12 to 14 s with the installed data, 14 to 16 s from an empty `--hdd-dir`). That is the `game_boot`
  milestone. The game then runs its frame loop: 8,208 flips and 7,794 `cellPadGetData` calls in the 30 s
  run, executed by the fake RSX. No renderer runs in a headless run.
- A PPU worker busy-waits for the whole run (`sys_ppu_thread_yield`, `sys_lwmutex_lock` and
  `sys_lwmutex_unlock` are about 62 million calls each in 30 s). The SPU side of the run is described in
  [SPU usage](spu.md#what-the-current-run-reaches), which also quotes the whole `census: spu` block.

The stop line and the head of the census of that run (the other sections follow as in
[Census](runtime.md#census)):

```
tox2: stop: watchdog 30 s
census: syscalls 61982172 calls, 32 distinct, 1 default-handled
census: hle 125394298 calls, 110 distinct, 7 default-handled
census: unknown targets 0 distinct
census: rsx 6897906 headers, 238 distinct methods, 8208 flips
census: threads 16 created, 12 live
census: spu 5 threads, 87070 dispatches, 0 unknown targets
```

The counts differ from run to run. The default-handled system call was `sys_spu_thread_bind_queue`
(the game's sound engine; it has a handler now); the seven default-handled imports are `cellRtcGetCurrentTick`,
`cellRtcConvertUtcToLocalTime`, four `cellScreenShotUtility` functions and `cellSaveDataListAutoLoad`.

A run with `--seconds 180` also ends at the watchdog (`tox2: stop: watchdog 180 s`). Its SPU census is
`census: spu 5 threads, 174337514 dispatches, 0 unknown targets`. Before the jump table at `0x0092d178`
(function `0x0092d0c8`) was recovered, a run stopped there at about 75 s with
`tox2: stop: fatal unknown target: target 0x0092d1a4 from 0x0092d178`.

## Earlier runs

These runs were made before libsre was loaded, when the `cellSpurs` calls were default-handled and
returned success without creating anything. The game gets further now (see above); the findings about the
start-up still hold.
Headless runs (`--no-window --seconds 20`) of the build from the generated code, one with an empty
`--hdd-dir` and one reusing the installed data, ended the same way:

- The runtime loaded the executable and booted the main thread. The stop was
  `tox2: stop: watchdog 20 s` (exit code 5). The census listed no unknown dispatch target: every
  call and jump that ran reached a recovered function, and no system call was default-handled.
  The main thread ends in polling with `sys_timer_usleep`; the last `tox2: first ...` line it wrote
  is `first sc 350 sys_memory_allocate_from_container`.
- Before the memory container calls were implemented, the runs ended with the game's own
  `abort()`: `tox2: stop: process exit status 1`, after the game wrote `abort() is called from
  0x0099ebbc` and ten more return addresses to the tty. The cause was the default-handled
  `sys_memory_container_create` (324), which wrote no container id, so that the next call,
  `sys_memory_container_get_size` (343), got id 0 and the game found no container. With the container
  calls implemented (the game creates one of 0x5600000 bytes and allocates twice from it) the main
  thread goes on to open `TLFILE.TLDAT` from the installed copy and to look for
  `FILEDATABASELOCAL.TLFDBX` (not found) and two save directories (`BLJS10120`, `BLJS10188`; not
  found). `sys_ppu_thread_detach` (45) and system call 988 were default-handled in those runs and
  had no part in the abort; 988 is issued inside the abort and exit functions (see
  [Runtime internals](runtime.md#implemented-system-calls)).
- Earlier runs stopped at a write to address 0 on the GCM interrupt thread, after the
  default-handled `cellGcmGetLabelAddress`. With that import served (see [Runtime internals](runtime.md#implemented-imports)),
  the thread no longer faults: the game polls `cellGcmGetFlipStatus`, sets the display buffers
  (two of 1280x720, pitch `0x1400`, after `cellVideoOutConfigure` for 720p), queues flip commands
  and the fake RSX executes them. The RSX census counted 5 flips and 939 FIFO headers in the run
  from an empty `--hdd-dir` and 3 flips and 563 headers in the other; the flip handler ran as often
  as there were flips and the vblank handler about 1,200 times in the 20 s. No frame was inspected:
  the runs were headless.
- `cellGameBootCheck` has a handler (a disc boot, see [Runtime internals](runtime.md#implemented-imports)),
  so the game's data paths start with `/dev_bdvd/PS3_GAME`: the stats of `TLFILE.TLDAT`,
  `FILEHEADER.TOFHDB` (both in `USRDIR`) and `ICON0.PNG` succeed.
- From an empty `--hdd-dir`: `cellGameDataCheck` for `BLJS10188_INSTALL` finds no directory, so thread
  `0x01000007` calls `cellGameCreateGameData`, opens a message dialog and copies the three files
  from the disc to the temporary game data directory with `cellFsRead` and `cellFsWrite` (724 of
  each in total; `TLFILE.TLDAT` is 3,017,411,417 bytes, `FILEHEADER.TOFHDB` 10.6 MB). The copy
  is complete within the 20 s. `cellGameContentPermit` then moves it to
  `/dev_hdd0/game/BLJS10188_INSTALL`, the dialog is closed and the main thread waits on an event
  flag until the install thread sets it. The reads are sequential and cross 2 GiB; the copy does
  not call `cellFsLseek`.
- A run that finds the installed directory reads `FILEHEADER.TOFHDB` from the disc again, opens the
  installed copy, stats and fstats it, and then the main thread calls the memory container calls.
- The `game_boot` test pinned the main thread's `first sc 350` line, the last `tox2: first ...` line
  it wrote in both kinds of run. It later pinned the SPU stop line and now pins the main thread's
  `cellSpursEventFlagTryWait` line (see [Tests](tests.md#the-game_boot-test)). The run goes past the
  install copy, so the test run writes the 3 GB copy into the build directory's `game_hdd`.
- Worker `0x01000003` pushes four entries with the default-handled `_cellSpursLFQueuePushBody` and then
  calls `cellSpursEventFlagWait` (default-handled, returns success without writing anything) in a
  loop: 154 to 164 million calls in 20 s. `cellSpursQueuePushBody` was not called. `TLFILE.TLDAT` was
  read only as the sequential copy above, plus one `cellFsLseek` on a worker; nothing parses it.

What the census of the run from an empty `--hdd-dir` showed (the other run differs in counts that
depend on scheduling):

- System calls: 127,415 calls, 16 distinct, none default-handled. `sys_ppu_thread_yield` and
  `sys_timer_usleep` account for most of the calls. The memory container calls (324, 343 and 350),
  `sys_ppu_thread_detach` (45) and `sys_memory_free` (349, from the install thread) ran.
- Imports: 154,627,484 calls, 81 distinct, 24 default-handled (all `cellSpurs` calls were among them). Almost all of the calls are
  `cellSpursEventFlagWait` (154,356,636). The default-handled ones are 21 `cellSpurs` functions
  (attribute, taskset, task, queue, event flag and lock-free queue setup, `_cellSpursLFQueuePushBody`
  and `cellSpursEventFlagWait`), `cellRtcGetCurrentTick`, `cellRtcConvertUtcToLocalTime` and
  `cellSaveDataListAutoLoad`. Each is
  logged with the argument registers that look like out-pointers; the game's later behaviour may
  depend on the values that were never written. `cellGcmGetLabelAddress` (8 calls),
  `cellGcmGetFlipStatus` (16), `cellVideoOutGetResolution` (1) and `cellVideoOutSetGamma` (5) are
  served by handlers now. The game did not call the other imports that got a handler
  (the audio out calls, `cellSysutilDisableBgmPlayback`, `cellAudioSetPortLevel`).
- No SPU code ran. The game set up a SPURS instance and six tasksets with 30 tasks through the
  default handlers (nothing was created behind them), and the only SPU system call was
  `sys_spu_initialize`. No SPU thread group or raw SPU system call was issued.
- RSX: 939 FIFO headers, 172 distinct methods and 5 flips. `rsx unimplemented` listed one kind, the
  ZPASS report constant.
- Threads: 8 created, 5 live at the stop (the main thread and a worker polling with
  `sys_timer_usleep`, the GCM interrupt thread waiting for an interrupt, and two running workers; the
  install thread and two others had exited). The game's own threads are named `CThread`.

## Components

These exist and are generic, but were written for and checked against the earlier game only:

- Analysis tools for the PPU executable: ELF summary, SPU probe, disassembler, Ghidra cross-check,
  NID and syscall name tables, function recovery (see [Analysis tools](analysis-tools.md) and
  [Function recovery](function-recovery.md)).
- An emitter that turns the recovered functions into C source (see [Emitting C](emitter.md)).
- `tox2_game`, which loads the executable and libsre, runs them through the generated code on real guest threads,
  serves the lv2 system calls and HLE modules, and runs a fake RSX that consumes the game's command
  buffer (see [Runtime internals](runtime.md)).
- A Vulkan 1.3 renderer that draws what the RSX commands describe into a window or PNG files, and
  `tox2_rsxreplay`, which renders single recorded frames without the game (see
  [Rendering](rendering.md)). The game's flips run in the fake RSX. A headless run with `--dump-frames` also runs the renderer
  (8,338 draws rendered, 1,622 skipped and 1,862 clears in 30 s; the skipped draws all lack a supported
  colour target, surface format F_X32). The dumped frames of that run are the loading screen: plain grey,
  with one magenta region (a placeholder for a texture without data) in the first. Before the fragment
  translator handled the fence, divide, reciprocal, exp2, log2 and KIL opcodes, most draws of a longer
  run were skipped for those opcodes. `tox2_rsxreplay` renders frames recorded from the game: a dialog
  frame (39 draws, 10 skipped: nine 3D-texture draws and the F_X32 one) shows the dialog box on grey, and a
  3D-scene frame (968 draws, 829 skipped: 828 vertex programs with the BRB opcode and the F_X32 one)
  still comes out as a uniform dark colour.
- Sound through the runtime's audio engine and WASAPI (see [Audio](audio.md)). The game initialises
  `cellAudio` in the current run and creates two ATRAC3plus decoders, which the host decodes; a 120 s
  headless run (output `none`) counts 22,071 blocks, 4,017 of them non-silent, and the audio dump is
  non-silent in three stretches from about 50 s on. The sound engine's SPU thread runs (see
  [SPU usage](spu.md#what-the-current-run-reaches)). The dump was not listened to.
- cellSail and cellPamf HLE, the PAMF demuxer, the host movie player and FFmpeg movie decoders.
  They are built but unused: this game imports neither module, so their handlers are never bound
  (see [Movies](movies.md)).

## What has been measured

Measured on the executable with the analysis tools, and confirmed by the load log of the runs:

- The images fit the guest memory map: the executable's two loaded segments end at `0x011c6860`,
  libsre follows at `0x011d0000` and ends at `0x01210000`, and about 238 MiB of the main region
  (`0x00010000..0x10000000`) remain after them.
- `.text` is `0x00010240..0x00c8f3d4`, 3,275,877 words. 9,687 words (0.30%) do not decode. The
  committed function database holds 29,196 functions, and the emitter reports no unhandled
  instruction form.
- The executable imports 173 functions from 17 modules, all with known names. The runtime has a
  handler for 120 of them and binds the 30 `cellSpurs` functions to libsre (`tox2: hle: 173 imports,
  120 with handlers, 30 bound to libsre`); the other 23 get the default handler (see
  [Runtime internals](runtime.md#default-handlers)). Among them are the four `cellRtc` and the four
  `cellScreenShotUtility` functions.
- It embeds 8 SPU images and has 267 system call sites; the probe leaves 4 of them unresolved.
- The game issues 67 system call numbers. The runtime has a handler for the semaphore, memory
  container and detach calls among them and for the SPU thread group calls 170 to 173 and 178 and the
  SPU thread calls 193 and 194; it has none for the vm calls (300, 301) and the file system calls 810,
  812, 813, 815, 820, 831, 832 and 834. Beyond what the game issues, the runtime also implements the SPU
  calls that libsre and the SPU runtime need (177, 181, 182, 185, 186, 190 to 192 and 250 to 252).
- Of the 16 SPU images (8 in the executable, 8 in libsre) the 14 with a fixed LS address are recovered
  and recompiled, and every instruction word of them has an emitter handler (see
  [Emitting C](emitter.md#spu-images)). The two position-independent blobs of libsre are neither
  recovered nor emitted; the game does not use them.
- libsre is 239,344 bytes with 352 functions, 32 imports and 267 exports; its recovery and its C
  output are described in [Function recovery](function-recovery.md#on-libsre) and
  [Emitting C](emitter.md#a-prx-module-libsre).

## Not implemented

- Raw SPU thread groups and `stop 0x120` are not implemented. The bnusCore SPU thread, the sound engine
  core (image 3), calls its modules through a table that is filled at run time; the recovery makes the
  function pointers of the image's data records entries (source `data`), and no unknown target has been
  seen since (see [SPU usage](spu.md#what-the-current-run-reaches)).
- SPU execution has these further limits: SPURS instance B (`atrac3plus`) gets its group and thread but
  is never started, the spline task (image 1) has not run, no SPU interrupt is delivered, signal
  notification is not implemented, and reservations have a documented race window against PPU plain
  stores (the SPU floating-point deviations are listed in
  [Emitting C](emitter.md#what-is-and-is-not-implemented)). See [SPU usage](spu.md).
- Function recovery leaves three `bctr` jump tables unresolved (`no_bound`): at `0x00186a20`, `0x0025a1dc`
  and `0x0058231c`. No run has reached them.
- The game's data archive `TLFILE.TLDAT` (about 3 GB) and its index `FILEHEADER.TOFHDB`: the install
  step copies them, and the game's own code then reads items from the archive (6,975 `cellFsRead` and
  6,746 `cellFsLseek` calls in the 30 s run) and has the Edge LZMA task inflate them. The runtime has no
  reader of the format itself, and which items the game asks for has not been mapped. The file system
  code uses 64-bit offsets (see [Virtual file system](virtual-file-system.md#large-files)); whether a
  read at an offset above 2 GiB has happened has not been checked.
- Movies: the game's movies are Bink files (`USRDIR/MOVIE/*.DAT`). Bink decoding is not
  implemented.
- Imports and system calls without a handler (see above), and imports whose handler is minimal.
  The census at the end of a run lists what the game called and what the default handler served.
  The census of a run from before the sound engine thread started listed one system call
  (`sys_spu_thread_bind_queue`, since implemented) and seven imports (`cellRtcGetCurrentTick`,
  `cellRtcConvertUtcToLocalTime`, four `cellScreenShotUtility` functions and `cellSaveDataListAutoLoad`),
  which are still default-handled.
- Graphics: only what the inherited fake RSX and renderer implement. Whatever the renderer meets and
  cannot handle is listed in the `rsx unimplemented` census (see
  [Rendering](rendering.md#rendering)). Known gaps are the ZPASS report values (the constant 0 is
  written, no occlusion query runs) and user clip planes (the vertex translator does not write the
  clip distances). The 30 s headless run counts 6.9 million FIFO headers, 238 distinct methods and 8,208 flips in the
  fake RSX. With `--dump-frames` the dumped frames of the first 30 s are the plain grey loading screen.
- Audio: WASAPI is the only device output, in shared mode on the default device. The output is
  stereo (an 8-channel port is reduced to its front pair), and there is no volume setting or device
  choice. ATRAC3 (non-plus) is not supported. The `cellAtrac` calls of this game all have handlers,
  but only the two sounds held in memory have been seen; the streaming path (`GetVacantSize` with
  a ring, starvation) is covered by tests only. `cellAudioSetPortLevel` and the `cellAudioOut` imports have handlers; the mixer
  is stereo only, so `cellAudioOutGetSoundAvailability` offers LPCM 2 ch at 48 kHz only.
- Input: the `neutral`, `none`, `keyboard`, `gamepad` and `keyboard+gamepad` pad providers. The
  keyboard has a fixed mapping, no analog sticks and no vibration; gamepads are read through
  GameInput with a fixed mapping. Only port 0 exists; there is no mouse or `cellKb` input. See
  [Input](input.md). The game reads port 0 (7,794 `cellPadGetData` calls in the 30 s run); no input has been sent to it.
- Priority scheduling beyond the creation rule (a new thread with a numerically lower priority than
  its creator runs first), lv2 interrupt threads, savestates.
