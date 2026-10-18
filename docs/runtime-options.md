# `tox2_game` options

The [top-level README](../README.md) shows the command for playing. This page lists every option of
`tox2_game`, its exit codes, and the options for unattended and diagnostic runs.

```sh
tox2_game [--game-dir DIR] [--vfs-config FILE] [--elf PATH] [--libsre PATH] [--hdd-dir DIR] [--sdata-cache DIR]
         [--seconds N] [--strict] [--log FILE] [--log-level LEVEL] [--quiet-hle] [--dialog-answer yes|no|ok]
         [--save-dialog auto|cancel|window] [--ui-font PATH]
         [--pad none|neutral|keyboard|gamepad|keyboard+gamepad] [--pad-script FILE] [--pad-probe]
         [--audio none|wasapi] [--audio-dump FILE]
         [--load-only] [--png-dump DIR] [--dump-guest ADDR:SIZE:FILE]...
         [--spu-capture IMAGE:S[+S...]:E[:dispatches=N]]... [--spu-capture-dir DIR]
         [--spu-capture-after SECONDS] [--spu-capture-count N] [--spu-capture-every K]
         [--render none|vulkan] [--rsx-capture N[,M...]] [--rsx-census FILE]
         [--dump-frames DIR] [--dump-frames-every K]
         [--shader-cache DIR] [--resolution-scale N] [--aa-mode supersample|msaa|dlaa] [--anisotropic-filter off|2|4|8|16]
         [--vk-validation] [--no-window] [--window-size 720p|1080p|1440p|2160p] [--fullscreen on|off]
         [--frame-gen off|60|90|120|180|unlimited] [--fg-present-mode mailbox|immediate|fifo] [--fg-reflex on|off]
         [--fg-only-generated] [--fg-cycle SECONDS]
         [--frame-limit vsync|60|off] [--host-priorities on|off] [--yield-backoff on|off] [--ppu-threads legacy|1|2]
         [--lwmutex-fast-path on|off] [--lwmutex-watch]
         [--config FILE | --no-config]
```

Every option except `--config` and `--no-config` can also be set in a `config.json` file, which also
stores the settings changed in the [debug overlay](runtime.md#debug-overlay); see
[Configuration file](#configuration-file).

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
| `--sdata-cache DIR` | Directory of decrypted `.sdat` files, each at its path relative to the game directory. Inherited from the project this runtime was derived from and unused here: this game has no `.sdat` files, nothing in the repository creates a cache, and without one `cellFsSdataOpen` fails. Default: `build/<preset>/sdata_cache` if that directory exists. |
| `--vfs-config FILE` | A JSON file that describes the virtual file system. The options above override the matching parts of it. See [VFS configuration file](virtual-file-system.md#vfs-configuration-file). |

### Window, input and sound

| Option | Meaning |
| --- | --- |
| `--no-window` | Run without a window, for unattended runs and tests. By default `tox2_game` shows the frames in a window (which implies `--render vulkan`), and closing it stops the run with exit code 0. An explicit `--render none` also turns the window off. Without the window GameInput is never loaded, so these runs need no gamepad or GameInput runtime. There is no [debug overlay](runtime.md#debug-overlay) either: it exists only with the window, where `F1` shows it. |
| `--window-size 720p\|1080p\|1440p\|2160p` | Size of the window's client area, in physical pixels: 1280x720, 1920x1080, 2560x1440 or 3840x2160 (default `720p`). A size that does not fit the work area of the window's monitor (with the window frame) gets the largest 16:9 size that does. The debug overlay can change it while the game runs (`Window size` under Settings). Needs the window: a usage error with `--no-window` or `--render none`. See [Window](runtime.md#window). |
| `--fullscreen on\|off` | Start in borderless fullscreen (`on`) or in a window (`off`, the default), with no windowed frame shown first. `Alt+Enter` and `F11` switch while the game runs; leaving fullscreen restores the window at `--window-size`. Needs the window, like `--window-size`. |
| `--pad none\|neutral\|keyboard\|gamepad\|keyboard+gamepad` | Input source for port 0. `neutral` connects a standard pad that never changes, `none` connects no pad, `keyboard` makes the keyboard the pad, `gamepad` the connected gamepads, and `keyboard+gamepad` both at once. With `gamepad`, port 0 is connected only while a gamepad is in use; otherwise it is always connected. Default: `keyboard+gamepad` with the window; `keyboard` with `--no-window` and `--pad-script`; otherwise `neutral`. `keyboard` without the window or `--pad-script` is a usage error, and so are `gamepad` and `keyboard+gamepad` with `--no-window`: unattended runs never load GameInput. See [Input](runtime.md#input) and [Gamepads](input.md#gamepads). |
| `--pad-script FILE` | Press pad buttons at given times (seconds, or flip numbers as `f<N>`), for runs without a person at the keyboard. The file is read and checked at start-up; an error is a usage error that names the line. It needs `--pad keyboard` or `keyboard+gamepad` (the default with the window) and cannot be combined with `none`, `neutral` or `gamepad`. See [Pad script](runtime.md#pad-script). |
| `--pad-probe` | Diagnostic for gamepads: logs every game controller that GameInput reports, every change of their readings and what the process knows about Steam, as `tox2: pad: probe ...` lines. Needs the window (a usage error with `--no-window`) and is independent of `--pad`; it never changes the pad. See [`--pad-probe`](input.md#--pad-probe). |
| `--audio none\|wasapi` | Where the game's sound goes. `wasapi` plays it through the default Windows audio device; `none` plays nothing, but the game's mixer and the audio clock run the same. Default: `wasapi` with the window, `none` with `--no-window`, so unattended runs and tests need no sound device. See [Audio](audio.md#options). |
| `--audio-dump FILE` | Write the mixed sound to `FILE` as a WAV file (48 kHz, stereo, 32-bit float), with either output. A file that cannot be created is a usage error. Its header is updated every second, so a run stopped by `--seconds` or by closing the window leaves a usable file. `tox2_audiotool wavstat FILE` prints its levels per second (see [`tox2_audiotool`](audio.md#tox2_audiotool)). |
| `--dialog-answer yes\|no\|ok` | What an auto-answered Yes/No message dialog replies (default `yes`; `ok` is the same as `yes`). OK dialogs always answer OK. |
| `--save-dialog auto\|cancel\|window` | How the game's save data list dialogs (`cellSaveData` with a list) are answered. `window` shows the list in the game window and the player picks an entry with the pad or the keyboard; `auto` picks the entry that is selected first and answers confirmations with yes, without showing anything, and `cancel` goes back out of every list and answers confirmations with no. Default: `window` when the run has a window, a pad that reads host input (`keyboard`, `gamepad` or `keyboard+gamepad`) and no `--pad-script`; otherwise `auto`, so unattended runs and tests never wait for a person. `window` without a window, or with `--pad none` or `neutral`, logs a warning and uses `auto`. The start-up log says which one applies, as `tox2: savedata dialog: window`, `auto` or `cancel`. Any other value is a usage error. The setting is not saved by the debug overlay. |
| `--ui-font PATH` | A TrueType font for the text of the save data dialog, tried before the fonts the run finds on its own: `SCE-PS3-SR-R-JPN.TTF` next to `libsre.reloc.elf` and next to the executable (`generated/inputs/` after a build with `-Pup`), then `fonts/BIZUDPGothic-Regular.ttf` next to `tox2_game`. A file that cannot be read, is not a font or lacks Japanese glyphs is skipped with a warning. Without any usable font the dialog uses ImGui's built-in font and Japanese text shows as `?`. |

### Rendering

| Option | Meaning |
| --- | --- |
| `--render none\|vulkan` | Whether the RSX commands are drawn (default `vulkan` with the window or `--dump-frames`, otherwise `none`). See [Rendering](rendering.md#rendering). |
| `--dump-frames DIR` | Write the displayed frame of every flip as `DIR/frame_<flip>.png` (flips counted from 1, six digits; RGB, 8 bits), with or without the window. `DIR` is created. The overlay's `Dump next flip as PNG` button writes one more flip there (as `DIR/frame_<flip>.png`, no duplicate if that flip is dumped anyway); without this option it writes to `generated/frames`. |
| `--dump-frames-every K` | Dump every `K`-th flip instead of every one (default 1). |
| `--shader-cache DIR` | Directory of the SPIR-V cache and the pipeline cache `pipelines.bin` (default `generated/shader_cache`). |
| `--resolution-scale N` | Render every colour and depth target at `N` times its guest size (`N` is a whole number from 1 to 4; default 1), so the picture is `N` times sharper. The window and the `--dump-frames` PNGs are `N` times the guest frame size (2560x1440 at `N` 2 for a 1280x720 game). A target that would exceed the GPU's image size limit is rendered at a smaller scale and counted as `state surface too large for the resolution scale`. Any other value is a usage error. |
| `--aa-mode supersample\|msaa\|dlaa` | How surfaces in the game's anti-aliased mode are rendered (default `msaa`). `supersample` draws them into a single-sample image in the memory layout, shaded once per sample; `msaa` is the 2-sample multisampled mode; `dlaa` draws the scene one sample per pixel with a sub-pixel jitter and anti-aliases it with NVIDIA DLSS in DLAA mode, which needs an NVIDIA RTX GPU and the Streamline DLLs (see [DLAA](rendering.md#dlaa) and [NVIDIA Streamline (DLAA)](building.md#nvidia-streamline-dlaa)). `msaa` falls back to `supersample` (one log line and the census kind `state 2-sample multisampling unavailable`) on a device without 2-sample support. `dlaa` falls back to `msaa` (or to `supersample` on a device without 2-sample support) when DLAA is unavailable: the build has no Streamline SDK, the DLLs are missing or not signed by NVIDIA, the GPU is not a DLSS-capable RTX GPU, the driver is too old, or the device lacks an image format the pass needs. That prints one warning, `tox2: render: DLAA unavailable (<reason>); anti-aliased surfaces use <mode>`, and counts the census kind `state DLAA unavailable`; the run goes on. At start-up the log says which case applies, `tox2: render: DLAA available (Streamline <version>)` or `tox2: render: DLAA unavailable: <reason>`. The `aa_mode` key of `config.json` takes the same three names in every build, so a saved `"dlaa"` is never an error. Any other value is a usage error. |
| `--frame-gen off\|60\|90\|120\|180\|unlimited` | Present `60`, `90`, `120`, `180` frames per second or as many as possible (`unlimited`) by letting NVIDIA DLSS Frame Generation insert frames between the game's flips (default `off`). The multiplier is `floor(target / game fps)`, at most what the GPU supports (6x on an RTX 5070 Ti), and generation is inactive when it is below 2: `120` gives 2x at 60 fps and 4x at 30 fps, `90` gives 3x at 30 fps and nothing at 60 fps. The game's own flip rate, vblank timing and frame limit do not change. It needs the window, `--aa-mode dlaa` (with another mode the target is accepted and generation stays inactive, `needs DLAA anti-aliasing`), an RTX GPU that supports it, hardware-accelerated GPU scheduling and the Streamline frame generation DLLs (see [Frame generation](rendering.md#frame-generation) and [NVIDIA Streamline (DLAA)](building.md#nvidia-streamline-dlaa)). Where it cannot run, one warning, `tox2: render: frame generation unavailable (<reason>); presenting without it`, is logged and the run presents as with `off`. `180` is unverified: no display of the development machine runs at 180 Hz. The `frame_gen` key of `config.json` takes the same six names, and the debug overlay changes it while the game runs (`Frame generation` under Settings). With `--no-window` the option is a usage error and the key is ignored. Any other value is a usage error. |
| `--fg-present-mode mailbox\|immediate\|fifo` | Diagnostic, needs `--frame-gen` other than `off`: the present mode while frame generation is loaded (default `mailbox`). DLSS-G needs vsync off, so `fifo` is only for comparison; it caps the multiplier at `floor(refresh / game fps)`. A mode the surface does not offer falls back to MAILBOX, IMMEDIATE, then FIFO with a warning. See [Frame generation](rendering.md#frame-generation). |
| `--fg-reflex on\|off` | Diagnostic, needs `--frame-gen` other than `off`: whether Reflex markers and the Reflex sleep run (default `on`). DLSS-G needs them: with `off` it reports that Reflex was not detected and stops generating. |
| `--fg-only-generated` | Diagnostic, needs `--frame-gen` other than `off`: present only the generated frames, to look at them without the real frames in between. |
| `--fg-cycle SECONDS` | Diagnostic, needs `--frame-gen` other than `off`: switch frame generation off and on again every `SECONDS` seconds (1 to 600). Each switch recreates the swapchain, as a window resize does. |
| `--anisotropic-filter off\|2\|4\|8\|16` | Force anisotropic filtering at the given level on the mipmapped, linearly filtered samplers (default `off`, which leaves every sampler as the game set it). The level is limited to what the device supports, and the option has no effect on a device without anisotropic filtering. The debug overlay can change it while the game runs (`Anisotropic filtering` under Settings). Any other value is a usage error. See [Anisotropic filtering](rendering.md#anisotropic-filtering). |
| `--vk-validation` | Enable the Khronos validation layer. It comes with the Vulkan SDK; if it is not installed, `tox2_game` logs a warning and goes on without it. |
| `--rsx-capture N[,M...]` | Record the given frames (1-based flip numbers) as `.tox2cap` files. See [Capture and replay](rendering.md#capture-and-replay). The overlay's `Capture next flip` button records one more frame into the same directory, `generated/rsx/captures`, with or without this option. |
| `--rsx-census FILE` | Count the arguments the game writes to the RSX registers (surface and texture formats, vertex layouts, primitive types, draw sizes, render state, flip and vblank rates) and write them to `FILE` as JSON when the run stops. The fragment and vertex programs the game used are written next to it as `generated/rsx/fp_<hash>.bin` and `vp_<hash>.bin` (relative to the working directory). The directories are created; the file is truncated. |
| `--frame-limit vsync\|60\|off` | Pace the flips. `vsync` (the default) follows RPCS3's "PS3 Native" frame limit: a flip that the game made in its VSYNC flip mode (`cellGcmSetFlipMode`, which this game uses) completes at the first vertical blank after its command executes, at most one per vblank, so a game that flips once per two vblanks runs at 30 flips per second. The command holds the command FIFO until then. A flip in the HSYNC flip mode is paced as with `60`. `60` completes at most 60 flips per second whatever the flip mode, like RPCS3's default "Auto" frame limit: a flip command that arrives early holds the command FIFO until its time. `off` completes every flip as soon as its command executes, so a headless run flips as fast as the game produces frames. Any other value is a usage error. With the window, the debug overlay can switch between the three while the game runs (`Flip limiter` under Settings); the option sets the value at start-up. See [Fake RSX](rendering.md#fake-rsx). |

The renderer options are a usage error (exit code 2) when the renderer is off. The window and
`--dump-frames` switch it on; `--dump-frames-every`, `--shader-cache`, `--resolution-scale`, `--aa-mode`,
`--anisotropic-filter` and `--vk-validation` need one of them or `--render vulkan`; an explicit `--render none`
conflicts with all of them (`error: --dump-frames, --dump-frames-every, --shader-cache, --resolution-scale,
--aa-mode, --anisotropic-filter and --vk-validation need --render vulkan`). `--window-size` and `--fullscreen`
need the window (`error: --window-size and --fullscreen need the window`).
`--rsx-capture`, `--rsx-census` and `--frame-limit` do not need the renderer. `--frame-gen` needs the window
(`error: --frame-gen needs the window`), and the four `--fg-` options need `--frame-gen` other than `off` (`error:
--fg-present-mode, --fg-reflex, --fg-only-generated and --fg-cycle need --frame-gen other than off`).

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
| `--spu-capture IMAGE:S[+S...]:E[:dispatches=N]` | Capture spans of generated SPU code for benchmarks. A span runs from the dispatch to an entry `S` of the SPU image `IMAGE` to the next dispatch to its entry `E` on the same SPU thread, so a call is captured from the dispatch before the call site to the return. `S` and `E` are hex local-store addresses (optional `0x`) of entries of the image; several starts can be joined with `+`. `dispatches=N` (decimal) rejects spans with more than `N` dispatches, the one to `E` included. A span is written only if it read no channel, ran no MFC command and counted no `stop` between `S` and `E`. Each accepted span becomes one file `<IMAGE>_<S>_<E>_<n>.spucap` in `--spu-capture-dir` that holds the registers and local store at both ends and the code segments the local store holds; every written file is logged as `tox2: spu capture: wrote ...`. Repeatable, one selector per occurrence. An unknown image, an `S` or `E` that is not an entry, an `E` that is also a start, or more than 16 distinct entries is a start-up error. Needs `--spu-capture-dir`. The patched entries run a small trampoline first; without this option the generated tables are used unchanged. |
| `--spu-capture-dir DIR` | Directory of the `.spucap` files, created when missing. Required with `--spu-capture`. |
| `--spu-capture-after SECONDS` | Capture only spans that start at least `SECONDS` seconds after the SPU tables were installed at boot, so loading and warm-up are skipped (whole seconds, default 0). |
| `--spu-capture-count N` | Write at most `N` files per `--spu-capture` selector (default 4; at least 1). A write that fails does not count. |
| `--spu-capture-every K` | Write every `K`-th accepted span of a selector instead of every one (default 1; at least 1). |
| `--host-priorities on\|off` | Raise the host priority of the threads that feed the audio output (default `on`): the audio engine thread to `THREAD_PRIORITY_TIME_CRITICAL`, and the host threads of an SPU thread group that has a scheduler context and an lv2 priority of 31 or less (the game's sound-processing group) to `THREAD_PRIORITY_HIGHEST`. All of them sleep or block while they wait. The RSX thread and the PPU threads stay at normal priority. `off` leaves every thread at normal priority. Any other value is a usage error. |
| `--yield-backoff on\|off` | Let a guest thread that spins on `sys_ppu_thread_yield` sleep without the guest lock, or in `--ppu-threads 1\|2` without its PPU slot (default `on`): a thread whose yields follow each other within 50 us sleeps 100 us on every 64th one, and the sleeps are counted in the census and the perf line (`yield calls ... backoff`). During a sleep any waiter may run, including a lower-priority PPU thread. `off` never sleeps, so a spinning thread keeps a core busy and the RSX thread waits longer for the lock; in a 140 s run of this game that used about 1.8 times the process CPU time in `legacy` and 1.7 times in `2`. The result of the call follows lv2's rule either way: `CELL_OK` only when a PPU thread of equal or higher priority was queued and ran, else `CELL_CANCEL` (see [Threads](runtime.md#threads)). Any other value is a usage error. |
| `--ppu-threads legacy\|1\|2` | How guest PPU threads share the CPU (default `2`). `legacy` runs one guest thread at a time, holding the guest lock for the whole time it runs guest code, so the RSX thread and the audio engine exclude PPU guest code. `1` and `2` run guest code under a PPU slot instead, with 1 or 2 slots, and take the guest lock (now the kernel lock) only inside lv2 and HLE calls, so with `2` two guest PPU threads can run at once, like the two hardware threads of the PS3, and the host threads run beside guest code. `1` is a diagnostic step between the two: no parallel PPU threads, but the host threads already run beside guest code. Compared with `legacy` in 140 s and 420 s runs of this game, `2` cut the RSX thread's lock wait from 2.0 to 0.3 ms per flip at the same flip rate and used 1.06 to 1.09 times the process CPU time (see [Threads](runtime.md#threads)). The mode is logged as `tox2: ppu threads: <mode>`. In `1` and `2` the census has a `census: ppu slots` line (slot hold time, time with both slots held, time with every slot held and a thread waiting, and the slot wait latencies), the census header and the perf `lock` line omit `ppu overlap`, and the perf `lock` line ends with `slots par X ms full Y ms` per second. Any other value is a usage error. |
| `--lwmutex-fast-path on\|off` | Whether `sys_lwmutex_lock`, `sys_lwmutex_trylock` and `sys_lwmutex_unlock` run without the kernel lock (default `on`). On, an uncontended lock or unlock is one compare-and-swap on the owner and waiter words of the guest `sys_lwmutex_t`, and a call enters a kernel section only when it finds the lwmutex owned by another thread or finds waiters at unlock; a lwmutex whose address is not 8-byte aligned always uses the kernel lock. `off` runs every one of these calls inside one kernel section, which gives the same results with every access under the lock; it is the fallback and the switch for comparing runs. It changes nothing in `legacy`, where the guest lock is held anyway. The perf `lock` line ends with `lwmutex slow lock N unlock N` (and `misaligned N` when non-zero) for the calls that left the compare-and-swap path in that second. Any other value is a usage error. |
| `--lwmutex-watch` | Diagnostic, off by default: counts the accesses to a live `sys_lwmutex_t` that do not go through the lwmutex calls. Every lightweight mutex that `sys_lwmutex_create` (and the process start-up) makes is registered, and these accesses are counted and logged (the first 16 of each kind, with kind, address, lwmutex, offset and the PPU `lr` when known): PPU `lwarx`, `ldarx`, `stwcx.`, `stdcx.` and `dcbz` on its bytes, SPU `GETLLAR`, `PUTLLC` (only for the 8-byte chunks it changes), `PUTLLUC` and `PUT` on them, `_sys_memcpy` and `_sys_memset` into them, an owner word that is neither a lwmutex state nor a thread id, and a waiter word that disagrees with the wait list. The `census: lwmutex watch` line prints the live and peak number of registered structs and one counter per kind. When it is off, each hook costs one relaxed load. |

The dump writes one line per option: `tox2: dump: 0x<addr>..0x<end> -> <file> sha256 <hex>`, followed by
` bytes <xx> <xx> ...` (the bytes in hex) when `SIZE` is 16 or less. A range that is not entirely committed
guest memory gives `tox2: dump: 0x<addr>..0x<end> -> <file> not committed` and writes no file. A file that
cannot be written gives `... write failed`.

## Configuration file

`tox2_game` reads its options from a JSON file as well as from the command line, and writes the
settings a player changes while playing back to it. The command line wins and is never written to the
file.

| Option | Meaning |
| --- | --- |
| `--config FILE` | Read `FILE` (relative to the working directory) instead of looking for `config.json`, and save changes to it. The file must exist: `error: FILE: cannot open the file`, exit code 2, without the usage text. The last occurrence wins. |
| `--no-config` | Read no file and save nothing. With `--config` it is a usage error: `error: --no-config excludes --config`. Tests and scripted runs use it so that they neither depend on nor change a player's file (the `game_boot` test does, see [Tests](tests.md)). |

### Location

Without `--config` and `--no-config`, `tox2_game` reads `config.json` in the working directory, or else
`config.json` next to the executable. Exactly one file is read; files are never merged. A build configured
with `-GameDir` writes a starter file next to the executable (see [The starter
`config.json`](building.md#the-starter-configjson)).

Changes are saved to the file that was read. When no file exists, the first saved change creates
`config.json` next to the executable; a new file is never created in the working directory, where it
could end up in the repository (`/config.json` is git-ignored all the same). With `--no-config`, or when
the executable's directory is unknown and no file was found, nothing is saved. The start-up log says which
case applies:

| Log line (info) | Meaning |
| --- | --- |
| `tox2: config: <file>` | The file was read, and changes are saved to it. |
| `tox2: config: none found; changes are saved to <file>` | No file exists; `<file>` is created at the first saved change. |
| `tox2: config: none found` | No file exists and none can be created (no executable directory). |
| `tox2: config: off (--no-config)` | `--no-config` was given. |

### Keys and values

The file is one JSON object in UTF-8 (a byte order mark is accepted). Comments, trailing commas, duplicate
keys and `null` values are errors. The key of an option is its name without `--` and with `-` written as
`_`: `--resolution-scale` is `"resolution_scale"`. `--config` and `--no-config` have no key. A value is
written the way its kind of option needs it:

| Kind | Keys | Value in the file |
| --- | --- | --- |
| Path | `game_dir`, `vfs_config`, `elf`, `libsre`, `hdd_dir`, `sdata_cache`, `log`, `png_dump`, `pad_script`, `audio_dump`, `rsx_census`, `ui_font`, `dump_frames`, `shader_cache`, `spu_capture_dir` | A non-empty string. A relative path is relative to the directory of the config file, not to the working directory (on the command line it is relative to the working directory). |
| Text | `log_level`, `dialog_answer`, `save_dialog`, `pad`, `audio`, `render`, `ppu_threads`, `frame_limit`, `aa_mode`, `anisotropic_filter`, `window_size`, `frame_gen` | A string with the text the command line takes. A whole number is also accepted and read as its decimal text, so `"frame_limit": 60` and `"ppu_threads": 2` work. |
| Number | `seconds`, `dump_frames_every`, `resolution_scale`, `spu_capture_after`, `spu_capture_count`, `spu_capture_every` | A non-negative whole number. |
| Flag | `strict`, `quiet_hle`, `load_only`, `pad_probe`, `vk_validation`, `no_window`, `lwmutex_watch` | `true` sets the option. `false` is the same as leaving the key out. |
| On or off | `host_priorities`, `yield_backoff`, `lwmutex_fast_path`, `fullscreen` | `true` or `false`, or the strings `"on"` or `"off"`. |
| Frame list | `rsx_capture` | A string such as `"300,600"`, or a non-empty array of positive whole numbers. |
| Strings | `dump_guest`, `spu_capture` | An array of strings, each one an `ADDR:SIZE:FILE` argument (`dump_guest`) or an `IMAGE:S[+S...]:E[:dispatches=N]` argument (`spu_capture`). |

```json
{
  "game_dir": "../../../game/BLJS10188",
  "elf": "../../generated/inputs/EBOOT.elf",
  "resolution_scale": 2,
  "aa_mode": "msaa",
  "anisotropic_filter": "16",
  "window_size": "1080p",
  "fullscreen": false
}
```

A key that is not an option is ignored with a warning, `tox2: config: <file>: unknown key "<key>" ignored`,
and is kept when the file is rewritten. The key of an option that no longer exists is such an unknown key.

### Precedence and errors

The order is: command line, then `config.json`, then the `--vfs-config` file, then the defaults. A key
acts exactly like the option, so `game_dir` in `config.json` overrides the `/dev_bdvd` mount of a VFS
file. When an option is on the command line, the file's value for it is not used and not checked; for the
repeatable `rsx_capture`, `dump_guest` and `spu_capture`, a command-line occurrence replaces the file's whole list.

A file that is not valid JSON, has a value of the wrong type or has a value that fails the option's check
stops the start with exit code 2. The message names the file (`<file>` is its absolute path) and the usage
text is not printed, because the command line is not at fault:

```
error: <file>: invalid JSON: <reason> at line L, column C (byte N)
error: <file>: duplicate key "x" in an object
error: <file>: the top level must be an object
error: <file>: "x": null is not a value; remove the key to use the default
error: <file>: "resolution_scale": expected a non-negative whole number, got string
error: <file>: resolution_scale needs a whole number from 1 to 4
```

A value is checked the same way wherever it comes from; only the label differs: the command line says
`error: --resolution-scale needs a whole number from 1 to 4`. A check that involves several options (the
missing `--game-dir`, the renderer and window requirements, the pad rules) keeps its usual text and, when a
file was read, adds a last line `note: settings were also read from <file>`, because the cause may be in the
file.

Some keys only have an effect with a renderer or a window. When the file holds one and the run lacks what
it needs, the key is ignored, with a debug-level log line `tox2: config: <file>: <key> ignored: the run has
no renderer` (or `no window`), and is never an error. So a saved `resolution_scale` does not break a
`--no-window` run. The same option on the command line keeps its usage error.

| Needs | Keys |
| --- | --- |
| A renderer | `dump_frames`, `dump_frames_every`, `shader_cache`, `vk_validation`, `resolution_scale`, `aa_mode`, `anisotropic_filter` |
| A window | `pad_probe`, `window_size`, `fullscreen`, `frame_gen` |

`dump_frames` in the file switches the renderer on by itself, so it is ignored only together with an
explicit `"render": "none"`. A key outside this table is checked like a command-line option: a hand-written
`"pad": "gamepad"` with `--no-window` is still an error.

### Saved settings

A key is written when, and only when, the player changes its setting in the running window. Start-up
values, whether from the command line, the file or the defaults, are never written by themselves.

| Key | Written when |
| --- | --- |
| `frame_limit` | `Flip limiter` is changed in the overlay (`"vsync"`, `"60"` or `"off"`). |
| `resolution_scale` | `Internal resolution scale` is changed (a number). |
| `aa_mode` | `Anti-aliasing` is changed (`"supersample"`, `"msaa"` or `"dlaa"`; the overlay lists DLAA only when it is available). |
| `anisotropic_filter` | `Anisotropic filtering` is changed (`"off"`, `"2"`, `"4"`, `"8"` or `"16"`). |
| `frame_gen` | `Frame generation` is changed (`"off"`, `"60"`, `"90"`, `"120"`, `"180"` or `"unlimited"`; the overlay lists it only where frame generation is available, and enables it only while the anti-aliasing mode is DLAA). |
| `window_size` | `Window size` is changed to another preset (`"720p"`, `"1080p"`, `"1440p"` or `"2160p"`). |
| `fullscreen` | `Alt+Enter` or `F11` switches fullscreen on or off (`true` or `false`). |

The log level is not saved: a saved `trace` would slow down and fill the log of every later run without
the player noticing. A `"log_level"` written by hand still applies at start-up. The volume control is a
placeholder and saves nothing. The rule has these consequences:

- A key given on the command line keeps the file's value for as long as the player does not touch that
  setting.
- If the player changes a setting that was given on the command line, the new value is written to the file
  (replacing the file's value) and the run continues with it. The command-line value itself is never
  written.
- A value that differs from the player's choice is not written: a resolution scale that fell back because
  the surface was too large, MSAA that fell back to supersampling, DLAA that fell back to MSAA, a frame generation target that cannot run, anisotropy
  limited by the device, or a window size reduced to fit the screen. The file keeps what the player chose, so a larger monitor or a
  different device gets it at the next start.
- Dragging or maximising the window, and a fullscreen state given by `--fullscreen`, write nothing.
- With `--no-config` nothing is written.

The write happens in the background: half a second after the last change, and at the latest two seconds
after the first unsaved one. It never stalls the game. When the run ends, pending changes are written
within 250 ms, so closing the window right after a change still saves it. The log line is `tox2: config:
saved <key>, <key> to <file>` (info).

The file is read again just before each write, and only the changed keys are updated. Other keys, including
unknown ones and edits made by hand while the game ran, are kept in their order and with their values; a new
key is added at the end. The file is rewritten in a fixed layout (two-space indentation, a final newline, no
byte order mark), so its spacing may change. The new text goes to `<file>.tmp` and is renamed over the file,
so a reader sees either the old or the new file.

If the file cannot be updated, nothing is written and the file stays as it was. The causes are a file that
is not valid JSON at that moment, a duplicate key, and a file that is read-only or in a directory that is
not writable. The changes stay pending and are tried again at the next change and when the run ends. Each
distinct reason is logged once per run as a warning, `tox2: config: cannot save settings to <file>:
<reason>`, and the game goes on.

## Exit codes and stop lines

Every run ends with one stop line on stderr and a census (see [Census](runtime.md#census)), then
exits:

| Code | Meaning | Stop line |
| --- | --- | --- |
| 0 | The guest called `sys_process_exit` or `_sys_process_exit`, the window was closed, or `--load-only` finished loading | `tox2: stop: process exit status N`, `tox2: stop: window closed` |
| 2 | Usage error: bad option, neither `--game-dir` nor `--vfs-config`, game directory missing, log file or audio dump file not writable; also a config file that is missing (`--config`), not valid JSON or has a bad value (see [Configuration file](#configuration-file)) | `error: ...` (usage text, except for config file problems) |
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

Unattended runs pass `--no-config`, so a `config.json` of the player is neither read nor changed. A short
unattended run that keeps its log and starts from an empty hdd directory, like the `game_boot` test (see
[Status](status.md) for how far this game gets):

```sh
tox2_game --no-config --game-dir <game-dir> --no-window --seconds 30 --hdd-dir <empty-dir> --log run.log
```

The same run with an explicit executable, for a game directory that does not hold `EBOOT.elf`
(libsre is looked up next to the ELF; `--libsre` names it when it is elsewhere):

```sh
tox2_game --no-config --game-dir <game-dir> --elf <elf> --libsre <libsre.reloc.elf> --no-window --seconds 30 --hdd-dir <empty-dir>
```

Every 600th flip of the first 30 s as PNG files, without a window (the fake RSX executes the game's
flips, so files appear; the frames written in the first 30 s are still the loading screen, a plain grey):

```sh
tox2_game --no-config --game-dir <game-dir> --no-window --seconds 30 --dump-frames out/frames --dump-frames-every 600
```

Recording the game's sound to a file without playing it (the game initialises `cellAudio` and its mixer
runs, but the sound engine's SPU thread does not start yet, so the file holds silence only):

```sh
tox2_game --no-config --game-dir <game-dir> --no-window --hdd-dir <empty-dir> --seconds 30 --audio-dump out/run.wav
```

Saving two ranges of guest memory when the run ends, 16 bytes (which are also logged) and 4 KiB:

```sh
tox2_game --no-config --game-dir <game-dir> --no-window --seconds 30 --hdd-dir <empty-dir> --dump-guest 0x10000:16:head.bin --dump-guest 0x10000:4096:page.bin
```
