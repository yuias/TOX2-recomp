# Runtime internals

## Logging

Log lines go to stderr, and to `--log FILE` when given, one line each. The logger is a thin layer
over spdlog (the `externals/spdlog` submodule, built as a static library with `std::format`
instead of the bundled fmt); spdlog types do not appear in the runtime's own headers. A line is
prefixed with `[host]` or with `[t XXXXXXXX]`, the id of the guest thread that wrote it, followed
by `warn: ` or `error: ` for those levels. The runtime's own messages then start with `tox2: ` and a
topic (`tox2: fs: ...`, `tox2: game: ...`, `tox2: pad: ...`); the census lines and the register dump
of a fatal stop have no such prefix. Output from the guest itself (`sys_tty_write`) goes to stdout,
unprefixed (the game's own assertion text appears there).

A run starts with the load log, for example (TOX2 values):

```
[host] tox2: sfo: TITLE_ID BLJS10188 RESOLUTION 0x15 SOUND_FORMAT 0x107
[host] tox2: load: phdr 0 type 0x1 flags 0x00400005 vaddr 0x00010000 filesz 0x103a828 memsz 0x103a828 ...
[host] tox2: dispatch: 29369 entries, span 0x00010200..0x00c9098c
[host] tox2: hle: 173 imports, 120 with handlers
[host] tox2: loaded EBOOT.elf: entry 0x00010240 toc 0x010b6330 opd 0x0105ddb8 ... image end 0x011d0000
[host] tox2: sha256 a1d8a56ce1d6300bff704a43218f1b8b87c80c1e1342929a266e58af4e5891e9
[host] tox2: boot: stack 0xd0001000..0xd0011000 entry 0x00010240 toc 0x010b6330 r1 0xd0010f40 ...
```

## Census

On every stop the runtime prints what the game did, sorted by count and then by name:

```
census: syscalls 57410 calls, 17 distinct, 4 default-handled
  sc 43 sys_ppu_thread_yield: 57173 [impl]
  sc 45 ?: 3 [default]
  sc 169 sys_spu_initialize: 1 [impl]
census: hle 129493 calls, 79 distinct, 22 default-handled
  sysPrxForUser::sys_lwmutex_lock: 61939 [impl]
  cellSpurs::cellSpursCreateTask2: 25 [default] ptr r3,r4,r5,r6,r7,r8,r9
  cellSysutil::cellSaveDataListAutoLoad: 1 [default] ptr r5,r6,r7,r8,r9
census: unknown targets 0 distinct
census: rsx 939 headers, 172 distinct methods, 5 flips
  interrupt handler calls: vblank 3, flip 5, user 0
  method 0x1efc: 105
census: rsx unimplemented 1 kinds
  state zpass report constant: 9
census: threads 8 created, 5 live
  0x01000000 main_thread: running
  0x01000002 _gcm_intr_thread: blocked in gcm interrupt wait [service]
  0x01000004 CThread: running
```

This is an excerpt of the census of a headless run of this game (see [Status](status.md)); the
counts differ a little from run to run. Syscall 45 prints `?` because the runtime's table has no
entry for it (it is `sys_ppu_thread_detach` in `config/lv2_syscalls.json`).

`[impl]` marks a call that an implemented handler served and `[default]` one that the default
handler took (see below). An unknown target is printed as `0x<target> from 0x<site>: count`. A default-handled import is
followed by `ptr r3,r5,...`, the argument registers that held a pointer to committed guest memory
(an out-pointer candidate, see [Default handlers](#default-handlers)). The `rsx` section is described under [Fake RSX](rendering.md#fake-rsx);
the `, rendered ... draws, skipped ...` part appears when the renderer runs, and the
`rsx unimplemented` section under [Rendering](rendering.md#rendering).
After a run that played movies through cellSail, a `census: sail <n> movies` section repeats the
per-movie summary lines (see [Movies](movies.md#log-lines-and-census)); this game imports no
cellSail, so the section does not appear. After a run in which the game
initialised `cellAudio`, a `census: audio` section gives the audio engine, mixer input and
`cellAtrac` counters (see [Audio](audio.md#census)).
Thread states are `running`, `blocked in <what>` or `exited`. The first call of each syscall and
each import also logs `tox2: first sc <number> <name>` or `tox2: first hle <module>::<name>` while
the game runs; these lines are the progress markers that `game_boot` checks (see
[Tests](tests.md#the-game_boot-test)).

## Memory map

The guest space is one reservation of 4 GiB plus a guard area, big-endian, accessed only through
the `tox2Read*` and `tox2Write*` helpers. Pages are committed on demand by the allocators below. An
access to an uncommitted page raises a fault that the runtime turns into a fatal stop with the
guest address and the access kind.

| Range | Use |
| --- | --- |
| `0x00010000..0x10000000` | Main region: the ELF image (committed at its segment addresses), per-thread TLS blocks and the heap handlers' allocations. 64 KiB granularity. |
| `0x10000000..0x20000000` | RSX context area, fixed by the console memory map: the default command buffer context (`0x10000000`), the DMA control and `put`/`get`/`ref` registers (`0x10100000`, `0x10100040`), the driver info block (`0x10200000`) and the reports and semaphores (`0x10300000`). The first 4 MiB are committed when the game initializes GCM. |
| `0x20000000..0x30000000` | User memory with 1 MiB pages (`sys_memory_allocate` with flags 0x400 or 0). |
| `0x30000000..0x40000000` | User memory with 64 KiB pages (`sys_memory_allocate` with flags 0x200). |
| `0xC0000000..0xCF900000` | RSX local memory (video memory), committed when the game initializes GCM. |
| `0xD0000000..0xE0000000` | Thread stacks, 4 KiB pages, with an uncommitted 4 KiB guard page on each side of every stack. |

The top 64 KiB, `0xFFFF0000..0xFFFFFFFF`, are never committed: they hold the entry keys of host
functions, the function descriptors that the HLE layer hands to guest code (for example a callback
the runtime itself provides). Guest code can call or start a thread on such a descriptor, and the
dispatcher resolves the key to the host handler.

The primary stack size from the executable's process parameters is clamped as RPCS3 does, between
0x10000 and 0x100000 bytes; this game asks for 0x10000, so the stack is `0xd0001000..0xd0011000`. The argument block (`argv`, `envp`, strings) sits at the top of
the stack. `sys_memory_get_user_memory_size` reports a 0xD500000 container. The timebase runs at
79.8 MHz, scaled from the host performance counter.

## Dispatch

`tox2RegisterFuncs` builds a flat table indexed by guest address from the generated function
mappings, so a computed jump or an indirect call costs one lookup. For this game the table has
29,369 entries over the span `0x00010200..0x00c9098c` (`tox2: dispatch:` in the load log). A target that is not in the
table is an unknown target: the first occurrence of each (target, call site) pair is logged with
the call trace and counted in the census. With `--strict` it is a fatal stop. Without it the
dispatch returns, which acts like a callee that returned immediately, so the run goes on and the
census lists every such target.

## Threads

Each guest thread is a host thread (16 MiB host stack) with its own context. All guest code runs
under one global guest lock, a FIFO ticket lock, so guest threads never run in parallel. A thread
gives up the lock only in `tox2Yield` (called from loop hooks, when another thread is waiting), in
a blocking wait and when it exits. Blocking system calls (mutex, condition variable, event flag,
event queue, sleep, join) wait on host condition variables with the guest lock, and record what
they wait for for the census and for stall detection.

Priority scheduling is deferred, with one exception. Runnable threads get the lock in arrival order,
and `sys_ppu_thread_set_priority` and `sys_ppu_thread_get_priority` only store and return the value.
The exception is the creation rule of `sys_ppu_thread_create`: a new thread whose priority number is
lower than the stored priority of its creator (lower number means higher precedence on lv2) runs
before the creator continues. `startGuestThreadFirst` starts the host thread, waits until it has
queued for the guest lock (at most 5 ms), and yields the lock to it. The new thread then runs until
it blocks, exits, yields or uses up its preemption budget; the creator's budget is reset afterwards.
A thread with an equal or higher number, an interrupt thread, and the threads the runtime itself
creates are started as before. The earlier game depended on this rule: its audio mixer thread was
created by a main thread of lower precedence and had to store its notify queue key before main
registered it with `cellAudio`. Whether this game needs it has not been checked (its main thread
has priority 1001; the priorities of its worker threads were not recorded). The rule is logged at
debug level, and a new thread that did not queue within 5 ms
logs `tox2: thread: ... not queued within 5 ms; creator continues` at info level. There is no other
priority behaviour: no priority-ordered hand-off, no wake order by priority, no preemption, and no
rescheduling when a priority changes.

`tox2_game` holds a 1 ms system timer resolution for the whole run (`timeBeginPeriod`,
`runtime/src/core/timer_resolution.*`). Timed waits (`sys_timer_usleep`, lock and queue time-outs)
are host condition variable waits, and Windows rounds them to its timer tick (15.6 ms by default),
so a 1 ms guest sleep would take 15 ms. The earlier game's decode loops polled with 1 ms sleeps;
this game's worker threads also sleep with `sys_timer_usleep` (24 calls before the stop). The
request ends with the process; a refusal is logged as a warning and the default tick stays.

The RSX thread and the audio engine are host threads that take the same guest lock. The RSX thread
therefore pumps the command buffer in slices of 128 words and leaves the lock once a hold has lasted
300 us, so that the game's mixer can run between the slices (see [Audio](audio.md#guest-lock); this
game stops before it starts a mixer, so the figures come from the earlier game). It
also leaves the lock after each draw end and copies the draw's guest bytes without it; the game only
reuses that memory after a later label, report or flip, which the RSX thread writes after the copy.

Thread ids start at `0x01000000`. lv2 interrupt threads are not implemented; the one interrupt
thread the game needs is the GCM interrupt thread described under [Fake RSX](rendering.md#fake-rsx).

A service thread is a guest thread that the runtime starts on the game's behalf and that waits
for work from the host (the GCM interrupt thread, and `_libsail-control`, which delivers the
cellSail notify events and allocator calls; see [Movies](movies.md). The latter is never started for
this game, which does not import cellSail). The census marks it `[service]`, and the
stall detector and the check for "no guest threads left" ignore it, so a game whose own threads
are all blocked still ends with a stall stop.

The host main thread is the supervisor. After boot it checks every 100 ms for the watchdog, a
stall and the absence of live guest threads.

## Implemented system calls

Everything else goes to the default handler. Behaviour follows the RPCS3 handler of the same
name; struct layouts and error codes are restated from its headers. The table lists what the
runtime implements; this game's executable uses 24 system call numbers that are not in it
(among them the semaphores 90, 92 and 93, the SPU thread group calls 170 to 178 and the vm and
memory container calls), and only some of those were reached before the stop (the census shows
number 45, `sys_ppu_thread_detach`, as default-handled, and later also 324 and 343,
`sys_memory_container_create` and `_get_size`, and 988, which the table does not name).

| Group | Calls |
| --- | --- |
| Process | `_sys_process_exit` (22) |
| TTY | `sys_tty_write` (403); `sys_tty_read` (402) returns `CELL_EIO` |
| Time | `sys_timer_usleep` (141), `sys_timer_sleep` (142), `sys_time_get_timebase_frequency` (147) |
| Memory | `sys_memory_allocate` (348), `sys_memory_free` (349), `sys_memory_get_user_memory_size` (352) |
| Mutex and condition variable | `sys_mutex_create`, `_destroy`, `_lock`, `_trylock`, `_unlock` (100-104); `sys_cond_create`, `_destroy`, `_wait`, `_signal`, `_signal_all` (105-109) |
| Event flag | `sys_event_flag_create` (82), `_destroy` (83), `_wait` (85), `_set` (87), `_clear` (118), `_cancel` (132), `_get` (139) |
| Event queue and port | `sys_event_queue_create` (128), `_destroy` (129), `_receive` (130), `_drain` (133); `sys_event_port_create` (134), `_destroy` (135), `_connect_local` (136), `_disconnect` (137), `_send` (138). A queue created with a non-zero IPC key can be found by that key (the host looks it up and sends to it; the `cellAudio` notify uses this); a second live queue with the same key fails with `CELL_EEXIST`, key 0 makes a local queue, and destroying the queue forgets the key. IPC ports (`sys_event_port_connect_ipc`) are not supported. |
| SPU | `sys_spu_initialize` (169) and `sys_spu_thread_group_connect_event_all_threads` (251) on a thread group id that the runtime handed out. Both only keep bookkeeping; no SPU code runs. The table names `sys_spu_thread_write_ls` (181), `_read_ls` (182), `_write_spu_mb` (190), `_connect_event` (191), `_disconnect_event` (192) and `sys_spu_thread_group_disconnect_event_all_threads` (252) but has no handler for them, so they are default-handled. |
| File system | `sys_fs_open` (801), `_read` (802), `_write` (803), `_close` (804), `_opendir` (805), `_readdir` (806), `_closedir` (807), `_stat` (808), `_fstat` (809), `_mkdir` (811), `_unlink` (814), `_lseek` (818); see [Virtual file system](virtual-file-system.md#virtual-file-system) |
| Thread | `_sys_ppu_thread_exit` (41), `sys_ppu_thread_yield` (43), `sys_ppu_thread_join` (44), `sys_ppu_thread_set_priority` (47), `sys_ppu_thread_get_priority` (48); the creation rule of `sys_ppu_thread_create` is under [Threads](#threads) |

## Implemented imports

Imports are matched to handlers by module and FNID. The executable of this game has 173 imports from
17 modules, and 120 of them have a handler (the load log says `tox2: hle: 173 imports, 120 with
handlers`). The other 53 are served by the [default handler](#default-handlers). Behaviour follows
the RPCS3 function of the same name (`Emu/Cell/Modules/`); each handler file lists its deviations in
its header comment. `cellAudio` and `cellAtrac` were implemented from a written description of
RPCS3's behaviour, not from its code (see [Audio](audio.md#clean-room)).

The first table lists, for this game's imports, what is bound to a handler and what is not. All
handlers were written for the earlier game; that they bind here only means that the module and
FNID match, not that the behaviour suits this game.

| Module | Bound / imported | Handled | Without a handler |
| --- | --- | --- | --- |
| `sysPrxForUser` | 18 / 20 | `sys_initialize_tls` (per-thread TLS blocks laid out as RPCS3 does), `sys_ppu_thread_create`, `_get_id`, `_once`, `_exit`; `sys_lwmutex_create`, `_destroy`, `_lock`, `_trylock`, `_unlock`; `sys_time_get_system_time`, `sys_process_exit`; `_sys_process_atexitspawn`, `_sys_process_at_Exitspawn`, `sys_prx_exitspawn_with_level`, `sys_prx_register_library`, `_sys_spu_printf_initialize`, `_sys_spu_printf_finalize` (the last six return `CELL_OK` and do nothing else) | `sys_spu_image_import`, `sys_spu_image_close` |
| `cellSysmodule` | 2 / 2 | `cellSysmoduleLoadModule`, `cellSysmoduleUnloadModule`: every module id succeeds, because the HLE functions are always available | |
| `cellSpurs` | 2 / 30 | `_cellSpursAttributeInitialize` (it fills the attribute structure) and `cellSpursFinalize` | the other 28: attribute setters, `cellSpursInitializeWithAttribute`, tasksets and tasks (versions 1 and 2), queues, lock-free queues, event flags, `cellSpursTaskGetContextSaveAreaSize`, join and push calls |
| `cellGcmSys` | 26 / 26 | initialization (`_cellGcmInitBody`), configuration, IO mapping (`cellGcmMapMainMemory`, `cellGcmAddressToOffset`, `cellGcmUnmapEaIoAddress`), display buffers, flip mode, the vblank and flip handlers, tiles and zcull, the control register, the flip commands and the default command buffer sizes; see [Fake RSX](rendering.md#fake-rsx). `cellGcmGetFlipStatus` returns the flip status that `cellGcmResetFlipStatus` (1, waiting) and an executed flip (0, done) leave. `cellGcmGetLabelAddress(index)` returns `0x10300000 + 16 * index` (`index` is a `u8`, so up to `0x10300ff0`), 0 before `_cellGcmInitBody`: the semaphore area that the RSX thread resolves its label offsets against, as for `_cellGcmSetFlipCommandWithWaitLabel`, whose label wait is at offset `16 * index` | |
| `cellSysutil` | 19 / 23 | the callback pump (`cellSysutilRegisterCallback`, `_CheckCallback`), BGM playback status, enabling and disabling, `cellVideoOutConfigure`, `_GetState`, `_GetResolution` (the pixel size of a resolution id, the same ids that `cellVideoOutConfigure` accepts), `cellAudioOutGetState`, `_GetConfiguration`, `_GetSoundAvailability` (the largest channel count among the modes with that coding type and rate: 2 for LPCM at 48 kHz, 0 otherwise, because the mixer is stereo) and `_Configure` (stores the downmixer, which the state and the configuration then report; a channel count other than 2 or a coding type other than LPCM is `CELL_AUDIO_OUT_ERROR_UNSUPPORTED_SOUND_MODE`, a downmixer above 2 is an illegal parameter), the message dialog (`cellMsgDialogOpen2`, `_Close`, `ProgressBarInc`) and save data (`cellSaveDataListLoad2`, `ListSave2`, `AutoSave2`, `Delete2`) | `cellMsgDialogOpenErrorCode`, `cellSaveDataListAutoLoad`, `cellSaveDataFixedLoad2`, `cellSaveDataFixedSave2` |
| `cellGame` | 5 / 5 | `cellGameBootCheck` (always a disc boot: type `CELL_GAME_GAMETYPE_DISC`, attributes 0, an empty directory name, and the permit that follows returns `/dev_bdvd/PS3_GAME` and its `USRDIR`), `cellGameDataCheck`, `cellGameCreateGameData`, `cellGameContentPermit`, `cellGameContentErrorDialog` | |
| `sys_fs` | 7 / 7 | `cellFsOpen`, `cellFsClose`, `cellFsStat`, `cellFsFstat`, `cellFsRead`, `cellFsLseek`, `cellFsWrite` (the last three call the lv2 handlers of `sys_fs_read`, `_lseek` and `_write`, so offsets and counts stay 64-bit) | |
| `sys_io` | 6 / 6 | `cellPadInit`, `cellPadEnd`, `cellPadSetPortSetting`, `cellPadGetData`, `cellPadGetInfo2`, `cellPadSetActDirect` | |
| `cellAudio` | 10 / 10 | `Init`, `Quit`, `PortOpen`, `PortStart`, `PortStop`, `PortClose`, `GetPortConfig`, `SetNotifyEventQueue`, `RemoveNotifyEventQueue`, `SetPortLevel` (the level of an open port, taken from `f1`; a negative level or NaN is `CELL_AUDIO_ERROR_PARAM`); the ports and the 8-block ring live in guest memory and a host clock drives the game's mixer; see [Audio](audio.md) | |
| `cellAtrac` | 12 / 15 | `SetDataAndGetMemSize`, `DeleteDecoder`, `Decode`, the stream calls (`GetStreamDataInfo`, `AddStreamData`), the loop and info calls, `GetMaxSample`, `GetChannel` and the second-buffer calls; ATRAC3plus is decoded through FFmpeg; see [Audio](audio.md#cellatrac) | `cellAtracCreateDecoderExt`, `cellAtracGetInternalErrorInfo`, `cellAtracGetVacantSize` |
| `sceNp` | 3 / 5 | `sceNpInit`, `sceNpTerm`, `sceNpDrmIsAvailable` | `sceNpManagerGetStatus`, `sceNpManagerGetContentRatingFlag` |
| `sceNpTrophy` | 9 / 10 | `sceNpTrophyInit`, `_Term`, `CreateContext`, `DestroyContext`, `CreateHandle`, `DestroyHandle`, `RegisterContext`, `GetRequiredDiskSpace`, `UnlockTrophy` | `sceNpTrophyAbortHandle` |
| `cellRtc` | 0 / 4 | | `cellRtcGetCurrentTick`, `cellRtcConvertUtcToLocalTime`, `cellRtcSetTick`, `cellRtcGetDayOfWeek` |
| `cellSysutilAvconfExt` | 1 / 1 | `cellVideoOutSetGamma` (accepts 0.8 to 1.8 from `f1` on the primary output, otherwise an illegal parameter or an unsupported video out; the gamma is not applied) | |
| `cellScreenShotUtility` | 0 / 4 | | all four |
| `sceNpCommerce2` | 0 / 2 | | both |
| `cellNetCtl` | 0 / 3 | | all three |

Handlers that exist but that no import of this game binds, because the game does not import the
function or the module:

- `cellSpurs`: `cellSpursInitializeWithAttribute2` and `cellSpursGetSpuThreadGroupId`. They create a
  SPURS instance and query it, but no SPU thread, workload or helper thread exists. This game
  initialises SPURS through the version 1 call `cellSpursInitializeWithAttribute`, which is
  default-handled.
- `sysPrxForUser`: the `_sys_heap_*` functions (create, delete, malloc, memalign, free).
- `cellSysutil`: `cellSysutilUnregisterCallback`, `cellVideoOutGetResolutionAvailability`,
  `cellMsgDialogProgressBarReset`, `cellSaveDataAutoLoad2`; `sceNpTrophyGetTrophyUnlockState`;
  `cellAtracCreateDecoder`, `cellAtracGetRemainFrame`, `cellAtracGetSoundInfo`;
  `cellAudioGetPortBlockTag`, `cellAudioGetPortTimestamp`.
- `cellPngDec` (7 functions, see [PNG decoding](#png-decoding)), `cellSail` (27 functions) and
  `cellPamf` (1 function): the game imports none of these modules, so the code is built and never
  runs for it (see [Movies](movies.md)).

Video out reports what the game's `PARAM.SFO` allows (the `RESOLUTION` attribute); a missing or
unreadable file keeps the defaults. Audio out offers LPCM 2 ch at 48 kHz whatever `SOUND_FORMAT`
says. `cellVideoOutConfigure` and `cellAudioOutConfigure` store the setting, and the `Get` calls of
the same output read it back, so the mode the game configured is the one it is told afterwards.

## Headless policy

RPCS3 opens dialogs and waits for the user in several of these modules. `tox2_game` has no user
interface, so each module follows a documented policy that never blocks on a person:

- **Message dialogs** (`cellMsgDialogOpen2`): the type and the text are logged. A dialog with
  buttons answers itself after 200 ms: OK for an OK dialog, and the `--dialog-answer` value
  (default yes) for a Yes/No dialog. A dialog without buttons stays open until
  `cellMsgDialogClose`. This game opens a dialog without buttons and with a progress bar (a game
  data installation message) just before its trap. The answer is delivered through the cellSysutil
  callback queue, so the guest sees it from `cellSysutilCheckCallback`, as on the console.
- **Save data**: a save lives in `/dev_hdd0/home/00000001/savedata/<dir name>/`. Error dialogs
  are logged and their error code is returned, without a dialog object. A save list is never
  shown; the entry is the one the game's list callback highlights, with RPCS3's fallbacks, and
  without a choice the call returns `CELL_CANCEL`. `cellSaveDataDelete2` returns `CELL_CANCEL`.
  Secure files are stored in plain text.
- **Trophies**: no trophy pack is installed from the network and no notification is shown. The
  pack is read for its trophy list, and the unlock state is kept as a text file in the trophy
  directory under `/dev_hdd0/home/00000001`. The registration status callbacks run inside
  `sceNpTrophyRegisterContext` instead of being spread over two seconds.
- **cellSysutil callbacks**: they run on the guest thread that calls `cellSysutilCheckCallback`,
  from a queue that the modules above feed. `CELL_SYSUTIL_REQUEST_EXITGAME` is not acted on.
- **cellGame**: the content directories are created on the host through the VFS, under the
  `--hdd-dir` directory.
- **Audio**: the output is `none` with `--no-window` unless `--audio wasapi` asks for the device,
  so unattended runs need no sound device; the game's mixer and the `cellAudio` clock run either way
  (see [Audio](audio.md#options)). A device or decoder problem degrades to silence with one warning.
- **Input**: see `--pad` and [Input](#input). The pad on port 0 comes from a provider
  (`runtime/src/hle/hle.h`): none, neutral, or the host pad state that the keyboard, a pad script and
  gamepads write. Runs with `--no-window` never load a gamepad backend.

## Input

`cellPad` asks a `PadProvider` for the state of a port on every `cellPadGetData` call and reports
the changes to the game. `--pad` picks the provider: `none` (no pad), `neutral` (a standard pad on
port 0 that never changes), `keyboard`, `gamepad` or `keyboard+gamepad`. The last three are set up in
`runtime/src/game_main.cpp` with `setPadProvider`, because they need the window and the script or
gamepad thread; `setPadBackend` accepts only `none` and `neutral`. The option defaults are in
[`tox2_game` options](runtime-options.md#window-input-and-sound) and the key table in the
[README](../README.md#keyboard); the script format is [below](#pad-script). Gamepads (mapping,
selection, Steam, the GameInput runtime, log lines and tools) are described in [Input](input.md).

| Part | File | Role |
| --- | --- | --- |
| Buttons | `runtime/src/input/pad_buttons.*` | The 16 buttons as a bit mask, `PadAnalog` (sticks and trigger pressure), `padStateFromButtons` and `padStateFromInput` (digital words, pressure words at 0xFF for pressed buttons or the analog trigger value, sticks, everything else as the neutral pad) and the button names used by scripts |
| State and provider | `runtime/src/input/keyboard_pad.*` | The scan-code table, `KeyboardPadState` (the state of every host source: keyboard, script, gamepad) and the provider for port 0 (always connected, or connected only while a gamepad is in use with `--pad gamepad`; other ports empty) |
| Script | `runtime/src/input/pad_script.*` | The script parser and the player thread |
| Gamepad | `runtime/src/input/gamepad_*`, `gameinput_backend.*` | Backend interface, GameInput backend, mapping, selection, the gamepad thread and `--pad-probe`; see [Input](input.md#design) |
| Window | `runtime/src/render/vk_window.*` | Key and focus messages from the window thread; turns the IME off for the window |

The renderer library does not depend on `input/`: the window reports raw scan codes through
callbacks, and `game_main.cpp` connects them to the state.

### Threading

`poll` runs on guest threads under the guest lock, so it must never block or take a lock that the
window thread, the script thread or the gamepad thread holds. `KeyboardPadState` is therefore
lock-free: one atomic mask of held buttons and one atomic press time per button for each source
(`Keyboard`, written by the window thread, `Script`, written by the script thread, and `Gamepad`,
written by the gamepad thread), one atomic with the sticks and trigger pressure of the gamepad, one
for its connection flag and one for the latest vibration request. `poll` only reads them. All
accesses are relaxed, because each bit and each time is independent, and a stale read delays a
change by one poll. The script and gamepad threads are detached and own a `shared_ptr` to the state;
nothing joins them, because the process ends with `std::_Exit`.

### Key handling

- Keys are identified by scan code (PC/AT set 1 make code from `lParam` bits 16 to 23, plus `0xE000`
  for an extended key), not by virtual key, so the mapping is a positional cluster on any layout
  and works while an IME is open. A zero scan code (some injected input) falls back to
  `MapVirtualKeyW`. Only the dedicated arrow keys map; the numpad does not.
- Key repeats are dropped, so they do not refresh a press time.
- A press stays in the reported state for at least `KeyboardPadState::kMinPressVisible` (60 ms)
  after it started, also when the key is already up. The game reads the pad about every 33 ms and
  `cellPadGetData` reports only changes, so a shorter tap could fall between two reads.
- Both opposite D-pad directions held cancel each other. A held direction beats the 60 ms tail of a
  released opposite one; with two tails the later press wins.
- `WM_KILLFOCUS` releases every `Keyboard` button at once (a key-up is not delivered to a window
  without focus). Script buttons are a separate source and stay; the gamepad thread releases its own
  source when the process loses the foreground window. `Alt` combinations still reach the
  default window procedure, so `Alt+F4` closes the window.
- The press mode that the game sets with `cellPadSetPortSetting` decides whether `cellPad` copies
  the pressure words to the game; this game sets 0x6 (press and sensor on) on ports 0 to 6
  (`tox2: pad: port 0 setting 0x6 (press on, sensor on)`), after `cellPadInit` with a maximum of 7
  pads.

### Pad script

A script is a text file of timed commands, one per line, read by `loadPadScript` while the options
are parsed. `#` starts a comment, blank lines are ignored, tokens are separated by spaces or tabs:

```
<time> press   <buttons>
<time> release <buttons>
<time> tap     <buttons> [every <interval> until <time>]
```

- `<time>` and `<interval>` are decimal seconds from the start of the script, with millisecond
  precision (more decimals are rounded). `<buttons>` are the names `select l3 r3 start up right down
  left l2 r2 l1 r1 triangle circle cross square`, joined with `+`.
- `tap` is a press at `<time>` and a release `kPadScriptTapHold` (150 ms) later, which is longer
  than the minimum press time. `every <interval> until <time>` repeats it at `<time> + k *
  <interval>` while that is not after the `until` time; `<interval>` must be longer than the hold.
  A script has at most 100000 events and times of at most 1000000 s.
- The events are sorted by time (stable); at the same time, releases come before presses.
- An error names the line (`line 7: unknown button "crosss"`), is printed to stderr and ends the
  start-up with exit code 2.
- The clock starts right before the main guest thread is booted, so it matches `--seconds` within
  a few milliseconds. The player thread sleeps until each event, applies it to the `Script` source
  and logs it.

No pad script ships with the repository. A script that taps Circle every 2 s from 4 s to 30 s:

```
4 tap circle every 2 until 30
```

A script that holds a direction while it taps a button:

```
10   press   down
10.5 tap     cross+r1
12   release down
```

### Log lines

All pad lines start with `tox2: pad:`; the script lines carry the `[host]` prefix. The lines of
gamepads (`gameinput`, `steam`, `device`, `gamepad`, `probe`) are in [Input](input.md#log-lines).

| Line | Level | Source |
| --- | --- | --- |
| `tox2: pad: init max_connect N` | info | `cellPadInit` |
| `tox2: pad: port N connected` or `disconnected` | info | the first change of a port to that state |
| `tox2: pad: port N setting 0xS (press on\|off, sensor on\|off)` | info | `cellPadSetPortSetting`, once per port and value |
| `tox2: pad: <pad>` followed by ` (window)` and ` (script <file name>, <n> events)` as they apply | info | start-up with `--pad keyboard`, `gamepad` or `keyboard+gamepad` (`<pad>` is the value) |
| `tox2: pad: script <s>.<ms> s: press\|release <buttons>` | info | the player thread, one line per event |
| `tox2: pad: key 0x<scan code> down\|up -> <button>` | debug | a mapped key, window thread; unmapped keys and repeats are not logged |

### Scripted runs and tests

Pressing buttons can make a game write save or system data under `--hdd-dir`, so a scripted run
should use its own directory and delete it first, never the directory of a normal run or of
`game_boot`. This repository has no test that runs the game with a pad script. The pad code itself
is covered by `runtime_core` (`testPadButtons`, `testKeyboardPadScanCodes`, `testKeyboardPadState`,
`testKeyboardPadProvider`, `testKeyboardPadThreads`, `testPadScriptParse`, `testPadScriptErrors`,
`testPadScriptPlay`), the gamepad source of the state by `runtime_core` (`testPadStateFromInput`,
`testGamepadSourceState`, `testGamepadPadProvider`, `testGamepadCellPad`), the rest of the gamepad
code by `input_core` and `input_gameinput` (see [Tests](tests.md)), and the key message conversion
by `render_core` (`testKeyMessages`).

## PNG decoding

This game does not import `cellPngDec`, so none of this runs for it; the description is of the
inherited code. `cellPngDec` decodes with libpng's classic API (`externals/libpng` at v1.6.59, built as a static
library together with zlib v1.3.2 from `externals/zlib`; nothing is installed or downloaded).
libpng reports errors by `longjmp`, so every call that can fail runs in a function that owns the
`setjmp` and has no locals with destructors. The handle and stream objects are allocated through
the game's malloc and free callbacks, as on the console. A buffer or file source is read into one
host buffer; with a `cbCtrlStrm` callback the runtime calls it for more data until the IEND chunk
is complete and then decodes the whole image. Output RGBA, ARGB, RGB, gray and gray-alpha come
from the stored format (palette, tRNS and sub-8-bit samples expand, 16-bit samples drop to the
high byte for 8-bit output, Adam7 images are de-interlaced); rows are written
`outputBytesPerLine` apart, top to bottom or bottom to top. Not implemented: RGB to grayscale, partial output through `cbCtrlDisp`, the
`cellPngDecGet*` chunk accessors. Each stream's source, header and output format are logged with
the `tox2: pngdec:` prefix.

## Default handlers

A system call without a handler, and an import without one, return 0 (`CELL_OK`) in `r3` and
change nothing else. The first call of each distinct syscall is logged as
`tox2: default sc <number> <name>(r3..r6)`, and the first 8 calls of each import as
`tox2: default hle <module>::<name>(r3..r8)` (none with `--quiet-hle`); later calls are only
counted. Returning success is a probe, not an implementation: guest code that depends on an
output parameter reads whatever was in memory, so a default-handled call can lead to a later
fault. The census shows which calls were default-handled, which is the work list for the next
handlers. On the first call of each default-handled import the runtime also logs the argument
registers that look like out-pointers (a nonzero 32-bit value that addresses committed memory
outside the code), as `tox2: default hle <module>::<name>: possible out-pointer r<N>=<address>
(<region>)`, and the census repeats the register list as `ptr r<N>,...`. These are hints for writing
the handler, not facts: a register left over from an earlier call can look like a pointer too.

In this game's runs the default handlers served 22 imports and four system calls. The earlier
stop, a write to address 0 in fn `0x008a090c` on the GCM interrupt thread, followed the
default-handled `cellGcmGetLabelAddress`, which returned 0 instead of a label address; with a
handler for it the thread no longer faults. The runs now end when the game calls `abort()` itself;
what leads to that is not known (see [Status](status.md)).

With the default result of `cellGameBootCheck` the game used data file paths without a directory
prefix; with the handler they start with `/dev_bdvd/PS3_GAME`.
