# Status

What works on the pinned executable today, and what does not. The [top-level README](../README.md)
has the short version.

The game does not run in any useful sense. `tox2_game` loads the executable and the firmware's SPURS
module (`libsre`), runs the game's start-up code on real guest threads, and stops with exit code 8 when
the SPURS code that runs inside libsre starts its first SPU thread group, because no SPU code can run.
This is work in progress.
The code was taken over from an earlier recompilation project of another game and is being pointed
at *Tales of Xillia 2* (BLJS10188); the parts that this game has not reached have never run against
its executable.

## What the current run does

`libsre.prx` of firmware 4.93 is part of the program (see
[SPU usage](spu.md#where-the-spurs-code-lives)). `tox2_game` loads `libsre.reloc.elf` at `0x011d0000`,
registers its 352 functions for dispatch and binds all 30 `cellSpurs` imports of the game to libsre's
exported functions. Of the game's 173 imports, 120 have a handler, 30 are bound to libsre and 23 get the
default handler. The load log says `tox2: hle: 173 imports, 120 with handlers, 30 bound to libsre`, and
`tox2: hle: libsre: 32 imports, 30 with handlers` for libsre's own imports.

The headless run (`--no-window --seconds 20`) from an empty `--hdd-dir` now goes like this:

- The main thread runs the start-up code up to `sys_spu_initialize`. Then the game's calls
  `_cellSpursAttributeInitialize`, `cellSpursAttributeSetNamePrefix`,
  `cellSpursAttributeEnableSpuPrintfIfAvailable`, `cellSpursAttributeSetSpuThreadGroupType` and
  `cellSpursInitializeWithAttribute` execute as recompiled libsre code. The census marks them `[libsre]`.
- Inside `cellSpursInitializeWithAttribute` libsre creates 17 semaphores, imports its version 1 SPURS
  kernel from `0x011f0380` (`sys_spu_image_import`, entry `0x818`), creates the SPU thread group
  `SpursTLGraphicsCellSpursKernelGroup` (5 threads, priority 250, type `0x18`), initialises its five
  threads, creates a lightweight mutex, a lightweight condition variable and an event queue, connects
  the queue to the group, and creates the helper threads `SpursTLGraphicsSpursHdlr1` and
  `SpursTLGraphicsSpursHdlr0`.
- The game creates SPURS with PPU priority 1000 and its main thread has priority 1001, so the creation
  rule of `sys_ppu_thread_create` (see [Threads](runtime.md#threads)) runs both helper threads before
  the main thread continues. `Hdlr1` blocks on the event queue. `Hdlr0` calls
  `sys_spu_thread_group_start`, and the run stops there with exit code 8. The main thread has not
  returned from `cellSpursInitializeWithAttribute`, so the game no longer reaches its install copy or
  the LZMA wait.

The stop line and the census of that run:

```
tox2: stop: spu: sys_spu_thread_group_start group 0x04000100 "SpursTLGraphicsCellSpursKernelGroup" (5 threads, image 0x011f0380 entry 0x818): SPU execution is not implemented
census: syscalls 136 calls, 17 distinct, 0 default-handled
census: hle 12498 calls, 27 distinct, 0 default-handled
census: unknown targets 0 distinct
census: threads 3 created, 3 live
```

Three runs gave the same stop line apart from the thread group id. Before the stop the log lists the
group, its five threads (each with the image, entry point and arguments), the event queue that was
connected and the SPU ports that were bound to it. Arguments 3 and 4 of every thread are
`0x554e444552464c4f`: libsre passes uninitialised stack words there, and the runtime only logs them. No
import and no system call was default-handled.

The next step is SPU execution: the SPURS kernel that libsre imports, and then the task images of the
game, have to run. See [SPU usage](spu.md).

## Earlier runs

These runs were made before libsre was loaded, when the `cellSpurs` calls were default-handled and
returned success without creating anything. The game no longer gets this far, because the SPURS
initialisation now stops the run (see above); the findings about the rest of the start-up still hold.
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
  [Rendering](rendering.md)). The game's flips ran in the fake RSX, but the headless runs had no
  renderer, so neither has been exercised with this game's commands.
- Sound through the runtime's audio engine and WASAPI (see [Audio](audio.md)). The game has not
  initialised `cellAudio` before the stop.
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
  container and detach calls among them and for the SPU thread group calls 170 to 173 and 178; it has
  none for the SPU thread calls 193 and 194, the vm calls (300, 301) and the file system calls 810,
  812, 813, 815, 820, 831, 832 and 834. None of those ran before the stop.
- libsre is 239,344 bytes with 352 functions, 32 imports and 267 exports; its recovery and its C
  output are described in [Function recovery](function-recovery.md#on-libsre) and
  [Emitting C](emitter.md#a-prx-module-libsre).

## Not implemented

- SPU and SPURS: the game embeds 8 SPU images, uses SPURS and creates raw SPU thread groups, and
  libsre embeds the SPURS kernels and policy modules. SPURS initialisation runs in recompiled libsre
  code, but no SPU code runs: the SPU thread group calls only keep bookkeeping, and
  `sys_spu_thread_group_start` stops the run (exit code 8). Whether and how to run SPU code
  (recompiling the SPU blobs, or an interpreter) is undecided; [SPU usage](spu.md) describes what runs
  on the SPUs. There is no SPU local store, MFC or mailbox, and `sys_spu_thread_write_ls`, `_read_ls`,
  `_write_spu_mb`, `_bind_queue` and `_unbind_queue` have no handler.
- The game's data archive `TLFILE.TLDAT` (about 3 GB) and its index `FILEHEADER.TOFHDB` are only
  copied by the game's install step, and no reader for their format exists. The file system code
  uses 64-bit offsets (see [Virtual file system](virtual-file-system.md#large-files)); the copy
  read the whole archive sequentially, but no read at a chosen offset above 2 GiB has happened.
- Movies: the game's movies are Bink files (`USRDIR/MOVIE/*.DAT`). Bink decoding is not
  implemented.
- Imports and system calls without a handler (see above), and imports whose handler is minimal.
  The census at the end of a run lists what the game called and what the default handler served.
  The census of the current run lists none, because the run stops earlier; the imports that the game
  reaches after SPURS (for example `cellSaveDataListAutoLoad`) are still default-handled.
- Graphics: only what the inherited fake RSX and renderer implement. Whatever the renderer meets and
  cannot handle is listed in the `rsx unimplemented` census (see
  [Rendering](rendering.md#rendering)). Known gaps are the ZPASS report values (the constant 0 is
  written, no occlusion query runs) and user clip planes (the vertex translator does not write the
  clip distances). Nothing in this game's command stream beyond the first 186 headers has been seen.
- Audio: WASAPI is the only device output, in shared mode on the default device. The output is
  stereo (an 8-channel port is reduced to its front pair), and there is no volume setting or device
  choice. ATRAC3 (non-plus) is not supported. Only the `cellAudio` and `cellAtrac` calls of the
  earlier game are implemented; this game's use of them has not been checked, and it imports
  `cellAtracCreateDecoderExt`, `cellAtracGetInternalErrorInfo` and `cellAtracGetVacantSize`, which
  have no handler. `cellAudioSetPortLevel` and the `cellAudioOut` imports have handlers; the mixer
  is stereo only, so `cellAudioOutGetSoundAvailability` offers LPCM 2 ch at 48 kHz only.
- Input: the `neutral`, `none`, `keyboard`, `gamepad` and `keyboard+gamepad` pad providers. The
  keyboard has a fixed mapping, no analog sticks and no vibration; gamepads are read through
  GameInput with a fixed mapping. Only port 0 exists; there is no mouse or `cellKb` input. See
  [Input](input.md). The game initialised `cellPad` in the runs but never read it before the stop.
- Priority scheduling beyond the creation rule (a new thread with a numerically lower priority than
  its creator runs first), lv2 interrupt threads, savestates.
