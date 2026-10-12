# `tox2_game` options

The [top-level README](../README.md) shows the command for playing. This page lists every option of
`tox2_game`, its exit codes, and the options for unattended and diagnostic runs.

```sh
tox2_game [--game-dir DIR] [--vfs-config FILE] [--elf PATH] [--libsre PATH] [--hdd-dir DIR] [--sdata-cache DIR]
         [--seconds N] [--strict] [--log FILE] [--log-level LEVEL] [--quiet-hle] [--dialog-answer yes|no|ok]
         [--pad none|neutral|keyboard|gamepad|keyboard+gamepad] [--pad-script FILE] [--pad-probe]
         [--audio none|wasapi] [--audio-dump FILE]
         [--load-only] [--png-dump DIR] [--dump-guest ADDR:SIZE:FILE]...
         [--render none|vulkan] [--rsx-capture N[,M...]] [--rsx-census FILE]
         [--dump-frames DIR] [--dump-frames-every K]
         [--shader-cache DIR] [--vk-validation] [--no-window] [--frame-limit 60|off]
```

`tox2_game` loads the decrypted `EBOOT.elf` into a 4 GiB guest address space, sets up the main PPU
thread the way RPCS3 does and calls the entry function through the generated code. The executable
must be the one the generated code was made from (SHA-256
`a1d8a56ce1d6300bff704a43218f1b8b87c80c1e1342929a266e58af4e5891e9`); any other file is a load error.
It also loads `libsre.reloc.elf`, the relocated firmware SPURS module, at `0x011d0000` (see `--libsre`);
that file comes from a build with `-Pup`, and its SHA-256 is checked the same way.
The runtime's own log lines carry a `tox2: ` prefix after the `[host]` or `[t XXXXXXXX]` thread
prefix (the census lines and the register dump of a fatal stop do not).

## Options

### Game files

| Option | Meaning |
| --- | --- |
| `--game-dir DIR` | The game directory, the one that contains `PS3_GAME`. It must exist. It is mounted read-only as `/dev_bdvd` and `/app_home` (`DIR/PS3_GAME/USRDIR`), `PS3_GAME/PARAM.SFO` is read from it, and it holds the default ELF location. Required unless `--vfs-config` mounts `/dev_bdvd` (the executable can also come from `--elf`). `DIR/PS3_GAME/USRDIR` need not exist. |
| `--elf PATH` | The decrypted executable. Default: `PS3_GAME/USRDIR/EBOOT.elf` on `/dev_bdvd` (`DIR/PS3_GAME/USRDIR/EBOOT.elf`, or where the configuration puts it). |
| `--libsre PATH` | The relocated firmware module `libsre.reloc.elf` (written by the build with `-Pup` to `generated/inputs/libsre.reloc.elf`). Default: `libsre.reloc.elf` in the directory of the executable (`--elf`, or the default ELF location), so a player who copies `EBOOT.elf` into `USRDIR` copies `libsre.reloc.elf` next to it. A missing file, an unrelocated `libsre.prx`, a hash that differs from the generated code or a failed layout check is a load error (exit code 4). |
| `--hdd-dir DIR` | Host directory behind `/dev_hdd0` and `/dev_hdd1` (`DIR/dev_hdd0`, `DIR/dev_hdd1`; created on demand), where the game keeps its game data and saves. Default: `build/<preset>/hdd`, next to the `bin` directory of the executable. |
| `--sdata-cache DIR` | Directory of decrypted `.sdat` files, each at its path relative to the game directory. Inherited from the earlier game and unused here: this game has no `.sdat` files, nothing in the repository creates a cache, and without one `cellFsSdataOpen` fails. Default: `build/<preset>/sdata_cache` if that directory exists. |
| `--vfs-config FILE` | A JSON file that describes the virtual file system. The options above override the matching parts of it. See [VFS configuration file](virtual-file-system.md#vfs-configuration-file). |

### Window, input and sound

| Option | Meaning |
| --- | --- |
| `--no-window` | Run without a window, for unattended runs and tests. By default `tox2_game` shows the frames in a window (which implies `--render vulkan`), and closing it stops the run with exit code 0. An explicit `--render none` also turns the window off. Without the window GameInput is never loaded, so these runs need no gamepad or GameInput runtime. |
| `--pad none\|neutral\|keyboard\|gamepad\|keyboard+gamepad` | Input source for port 0. `neutral` connects a standard pad that never changes, `none` connects no pad, `keyboard` makes the keyboard the pad, `gamepad` the connected gamepads, and `keyboard+gamepad` both at once. With `gamepad`, port 0 is connected only while a gamepad is in use; otherwise it is always connected. Default: `keyboard+gamepad` with the window; `keyboard` with `--no-window` and `--pad-script`; otherwise `neutral`. `keyboard` without the window or `--pad-script` is a usage error, and so are `gamepad` and `keyboard+gamepad` with `--no-window`: unattended runs never load GameInput. See [Input](runtime.md#input) and [Gamepads](input.md#gamepads). |
| `--pad-script FILE` | Press pad buttons at given times, for runs without a person at the keyboard. The file is read and checked at start-up; an error is a usage error that names the line. It needs `--pad keyboard` or `keyboard+gamepad` (the default with the window) and cannot be combined with `none`, `neutral` or `gamepad`. See [Pad script](runtime.md#pad-script). |
| `--pad-probe` | Diagnostic for gamepads: logs every game controller that GameInput reports, every change of their readings and what the process knows about Steam, as `tox2: pad: probe ...` lines. Needs the window (a usage error with `--no-window`) and is independent of `--pad`; it never changes the pad. See [`--pad-probe`](input.md#--pad-probe). |
| `--audio none\|wasapi` | Where the game's sound goes. `wasapi` plays it through the default Windows audio device; `none` plays nothing, but the game's mixer and the audio clock run the same. Default: `wasapi` with the window, `none` with `--no-window`, so unattended runs and tests need no sound device. See [Audio](audio.md#options). |
| `--audio-dump FILE` | Write the mixed sound to `FILE` as a WAV file (48 kHz, stereo, 32-bit float), with either output. A file that cannot be created is a usage error. Its header is updated every second, so a run stopped by `--seconds` or by closing the window leaves a usable file. `tox2_audiotool wavstat FILE` prints its levels per second (see [`tox2_audiotool`](audio.md#tox2_audiotool)). |
| `--dialog-answer yes\|no\|ok` | What an auto-answered Yes/No message dialog replies (default `yes`; `ok` is the same as `yes`). OK dialogs always answer OK. |

### Rendering

| Option | Meaning |
| --- | --- |
| `--render none\|vulkan` | Whether the RSX commands are drawn (default `vulkan` with the window or `--dump-frames`, otherwise `none`). See [Rendering](rendering.md#rendering). |
| `--dump-frames DIR` | Write the displayed frame of every flip as `DIR/frame_<flip>.png` (flips counted from 1, six digits; RGB, 8 bits), with or without the window. `DIR` is created. |
| `--dump-frames-every K` | Dump every `K`-th flip instead of every one (default 1). |
| `--shader-cache DIR` | Directory of the SPIR-V cache and the pipeline cache `pipelines.bin` (default `generated/shader_cache`). |
| `--vk-validation` | Enable the Khronos validation layer. It comes with the Vulkan SDK; if it is not installed, `tox2_game` logs a warning and goes on without it. |
| `--rsx-capture N[,M...]` | Record the given frames (1-based flip numbers) as `.tox2cap` files. See [Capture and replay](rendering.md#capture-and-replay). |
| `--rsx-census FILE` | Count the arguments the game writes to the RSX registers (surface and texture formats, vertex layouts, primitive types, draw sizes, render state, flip and vblank rates) and write them to `FILE` as JSON when the run stops. The fragment and vertex programs the game used are written next to it as `generated/rsx/fp_<hash>.bin` and `vp_<hash>.bin` (relative to the working directory). The directories are created; the file is truncated. |
| `--frame-limit 60\|off` | Pace the flips. `60` (the default) completes at most 60 flips per second, like RPCS3's default "Auto" frame limit: a flip command that arrives early holds the command FIFO until its time. `off` completes every flip as soon as its command executes, so a headless run flips as fast as the game produces frames. Any other value is a usage error. See [Fake RSX](rendering.md#fake-rsx). |

The renderer options are a usage error (exit code 2) when the renderer is off. The window and
`--dump-frames` switch it on; `--dump-frames-every`, `--shader-cache` and `--vk-validation` need one
of them or `--render vulkan`; an explicit `--render none` conflicts with all of them.
`--rsx-capture`, `--rsx-census` and `--frame-limit` do not need the renderer.

Frame dumps are encoded on a worker thread, so dumping does not slow the game down. When the worker
falls more than four frames behind, a frame is skipped and counted (`tox2: render: dump skipped
(queue full) at flip N`, and `dumps skipped (queue full)` in the end-of-run `tox2: render:` line). At
the end of a run the pending frames are written within 2 s. Each file is written as `.png.tmp` and
renamed, so an interrupted run never leaves a truncated `.png`. A file that cannot be written is
reported once and counted, and the run goes on.

The log says which Vulkan device was chosen and how long the device and renderer took to start. If
no device meets the requirements (Vulkan 1.3 with dynamic rendering, synchronization2 and shader
demote-to-helper-invocation; a window also needs the Win32 surface and swapchain extensions), or
the window cannot be created, the RSX thread logs the reason and the game goes on without rendering.

### Diagnostics

| Option | Meaning |
| --- | --- |
| `--seconds N` | Watchdog: stop after `N` seconds. 0 (the default) disables it. |
| `--log FILE` | Also write every log line to `FILE`, which is truncated. |
| `--log-level trace\|debug\|info\|warn\|error` | Lowest level that is logged (default `info`). `debug` adds diagnostics such as the 10 s window statistics; `trace` is the most verbose. Debug and trace lines carry a `debug: ` / `trace: ` tag. |
| `--quiet-hle` | Do not log the first calls of default-handled imports. The counts in the census are unaffected. |
| `--strict` | Treat a jump to an address that is not a recovered function as a fatal error (see [Dispatch](runtime.md#dispatch)). |
| `--load-only` | Load and verify the executable and libsre, print the load summary and exit with 0 without running any guest code. |
| `--png-dump DIR` | Write every image `cellPngDec` decodes to `DIR/png_<n>.png` (8-bit RGBA, at most 64), to check what the guest was given. `DIR` is created. |
| `--dump-guest ADDR:SIZE:FILE` | Write `SIZE` bytes of guest memory from `ADDR` to `FILE` when the run stops, and log their SHA-256. Repeatable. `ADDR` and `SIZE` are decimal or `0x` hex; `SIZE` is at least 1 and `ADDR + SIZE` must stay within 4 GiB, and `FILE` is the rest of the argument after the second colon. The dump runs on every stop path (exit code, watchdog, stall, fatal stop), in the order given and before the census, so it shows the memory as the run left it while other threads may still run. `FILE` is truncated. |

The dump writes one line per option: `tox2: dump: 0x<addr>..0x<end> -> <file> sha256 <hex>`, followed by
` bytes <xx> <xx> ...` (the bytes in hex) when `SIZE` is 16 or less. A range that is not entirely committed
guest memory gives `tox2: dump: 0x<addr>..0x<end> -> <file> not committed` and writes no file. A file that
cannot be written gives `... write failed`.

## Exit codes and stop lines

Every run ends with one stop line on stderr and a census (see [Census](runtime.md#census)), then
exits:

| Code | Meaning | Stop line |
| --- | --- | --- |
| 0 | The guest called `sys_process_exit` or `_sys_process_exit`, the window was closed, or `--load-only` finished loading | `tox2: stop: process exit status N`, `tox2: stop: window closed` |
| 2 | Usage error: bad option, neither `--game-dir` nor `--vfs-config`, game directory missing, log file or audio dump file not writable | `error: ...` (usage text) |
| 3 | Fatal error: unhandled instruction, trap, guest fault (access outside the committed guest memory), unknown dispatch target with `--strict`, guest address space cannot be reserved; a fatal stop on an SPU thread (an unknown SPU dispatch target, code that does not match its image, an unhandled SPU word, an MFC command or channel the runtime does not implement, a `stop` code or `WrOutIntrMbox` code it does not know; see [SPU usage](spu.md#log-lines-and-census)); also the stops the game or libsre itself asks for (`cellGameContentErrorDialog`, a failed trophy registration, libsre's `abort`) | `tox2: stop: fatal <kind>: ...`, or a `tox2: stop: ...` line naming the call, for example `tox2: stop: abort from 0x<address> (sysPrxForUser 0x9fb6228e)` |
| 4 | Load error: file missing, not a PPU executable, SHA-256 different from the one in the generated code, libsre missing, not relocated, of another hash or failing its layout check, invalid VFS configuration, or no executable can be located | `error: ...` |
| 5 | The `--seconds` watchdog elapsed | `tox2: stop: watchdog N s` |
| 6 | Stall: every live guest thread has been in an indefinite wait for at least 2 seconds | `tox2: stop: stall (N threads blocked)` |
| 7 | All guest threads ended without a process exit | `tox2: stop: no guest threads left` |
| 8 | Reserved for a stop where the guest needs something the runtime deliberately does not do yet. No stop uses it at present: SPU thread groups run, and a stop on an SPU thread ends with code 3 | - |

A fatal stop also prints the registers and the call trace (the most recent function entries) of
the faulting guest thread; a stop on an SPU thread prints that thread's registers, its trace of recent
dispatches and its LS map instead (see [SPU usage](spu.md#log-lines-and-census)). A run of this game
ends with code 5 at the `--seconds` watchdog (seen with limits of 30 s and 180 s). A stop with code 3 at an unknown
dispatch target means a jump table that recovery left unresolved (see [Status](status.md)). The runtime
never calls
`abort`: the CRT and Windows error dialogs are disabled, so a run always ends with an exit code,
also when it is started from a script.

## Examples by purpose

A short unattended run that keeps its log and starts from an empty hdd directory, like the
`game_boot` test (see [Status](status.md) for how far this game gets):

```sh
tox2_game --game-dir <game-dir> --no-window --seconds 30 --hdd-dir <empty-dir> --log run.log
```

The same run with an explicit executable, for a game directory that does not hold `EBOOT.elf`
(libsre is looked up next to the ELF; `--libsre` names it when it is elsewhere):

```sh
tox2_game --game-dir <game-dir> --elf <elf> --libsre <libsre.reloc.elf> --no-window --seconds 30 --hdd-dir <empty-dir>
```

Every 600th flip of the first 30 s as PNG files, without a window (the fake RSX executes the game's
flips, so files appear; the frames written in the first 30 s are still the loading screen, a plain grey):

```sh
tox2_game --game-dir <game-dir> --no-window --seconds 30 --dump-frames out/frames --dump-frames-every 600
```

Recording the game's sound to a file without playing it (the game initialises `cellAudio` and its mixer
runs, but the sound engine's SPU thread does not start yet, so the file holds silence only):

```sh
tox2_game --game-dir <game-dir> --no-window --hdd-dir <empty-dir> --seconds 30 --audio-dump out/run.wav
```

Saving two ranges of guest memory when the run ends, 16 bytes (which are also logged) and 4 KiB:

```sh
tox2_game --game-dir <game-dir> --no-window --seconds 30 --hdd-dir <empty-dir> --dump-guest 0x10000:16:head.bin --dump-guest 0x10000:4096:page.bin
```
