# Tests

## CTest targets

`tools/build.ps1 -Test` runs every registered CTest target. Targets that need the game or local
files are registered only when those are configured, and configuration prints why one is not.

| Target | What it checks | Needs |
| --- | --- | --- |
| `runtime_version`, `runtime_musttail` | The runtime stub starts, and the compiler supports `musttail` | - |
| `tools_unittest` | The Python unit tests (below). CTest passes `TOX2_EBOOT_ELF` to them, so they include the real-executable tests when `-EbootElf` is given | - |
| `ppu_header_check`, `ppu_semantics` | Contract header and the semantics harness (see [Emitting C](emitter.md)) | - |
| `ppu_link` | The generated code links | `-GeneratedDir` |
| `runtime_core` | Runtime core unit tests (memory, threads, lv2, HLE, VFS, fake RSX) | - |
| `render_core`, `vp_translator`, `fp_translator` | Renderer and shader translators (see [Rendering](rendering.md)) | a Vulkan 1.3 device for the rendering parts |
| `rsx_replay` | Replays a local capture and, when a reference image is pinned, compares the result with it | a local capture (see [Capture and replay](rendering.md#capture-and-replay)) |
| `media_core` | PAMF parser and demuxer, the movie player and the cellSail HLE on synthetic data, with fake codecs (inherited code that this game does not use, see [Movies](movies.md)) | - |
| `media_ffmpeg` | The FFmpeg loader, the H.264 and ATRAC3plus decoders (the movie decoder and the raw-frame decoder behind `cellAtrac`) and the colour and PCM conversion, all on synthetic data | the fetched FFmpeg DLLs |
| `audio_core` | Audio on synthetic data, with no device and no FFmpeg: the block ring, null output, WAV writer and reader, `wavStats`, `BlockClock` and its correction, the clock controller against a fake device, the engine with fake producers and outputs, the `cellAudio` state and handlers on synthetic guest memory, the AT3 parser, and the `cellAtrac` stream model and handlers with a fake decoder | - |
| `audio_wasapi` | The WASAPI output: device loss and reopen, priming, high-water drops and the fill level on fake streams, then (when a render device exists) opening the default device, playing silence and a real device loss and reopen. Exit code 77 (skipped) when the machine has no render device. Runs serially, because it measures timing | a render device for the real-device part |
| `input_core` | Gamepad code behind the backend interface, with a fake backend and no GameInput: value, reading and device formatting; the `--pad-probe` logic (a line per change, one `no gamepad reading` per device, devices coming and going, the `SteamVirtualGamepadInfo` lines); the button table, dead zone, stick bytes, trigger threshold, merging of pads and motor conversion; Steam virtual path parsing, `SDL_GAMECONTROLLER_IGNORE_DEVICES` parsing and the selection rule (recorded runs, a virtual pad, the ignore list, no Steam); the hub (use of a pad, removal, two pads, a virtual pad and its physical pad, the focus gate, vibration, the provider following plugging) and the gamepad thread with a fake backend (unavailable backend, feeding the pad, start lines, probe alone) | - |
| `input_gameinput` | The GameInput backend: the Steam context, then (with a GameInput runtime of API version 3) the device list, unique non-zero ids, a stable device generation, and `read`, `readRaw` and `setRumble` on unknown ids. No pad has to be connected. Exit code 77 (skipped) with the reason when no such runtime is installed | a GameInput runtime for all but the Steam context part (see [Input](input.md#the-gameinput-runtime-and-the-gameinput-unavailable-warning)) |
| `game_boot` | The game reaches a milestone in a 20 s run (below) | `-GeneratedDir`, `-EbootElf`, `-GameDir` |

`rsx_replay` is registered when the capture named in `cmake/rsx_replay.cmake` exists under
`generated/rsx/captures/`. While `TOX2_RSX_REPLAY_REFERENCE_SHA256` in that file is empty, as it is
now, the test only replays the capture and checks the summary line. With a SHA-256 set, the test
also needs the matching reference image and applies the difference thresholds.

This game has no `.sdat` files and its movies are not PAMF, so there are no SDAT tests, no media
tests that read game files, and no pad-script or audio-content runs of the game. The synthetic
tests of the inherited media and audio code remain.

## The `game_boot` test

CTest registers `game_boot` only when `TOX2_PPU_GENERATED_DIR`, `TOX2_EBOOT_ELF` and
`TOX2_GAME_DIR` are all set (`-GeneratedDir`, `-EbootElf`, `-GameDir`); otherwise configuration prints
that it is not registered. It runs `tox2_game --game-dir <dir> --elf <elf>` with the arguments
from the pin file (the run keeps its game data in `build/<preset>/game_hdd`), with a 60 s time
limit, and passes when the output matches the milestone regular expression. The exit code is not
checked: the run ends at the 20 s watchdog or at a stop somewhere after the milestone, and a
stop before the milestone leaves the pattern unmatched.

The pin file `cmake/game_boot.cmake` holds the arguments and the milestone:

```cmake
set(TOX2_GAME_BOOT_ARGS "--no-window --seconds 20 --hdd-dir \"${CMAKE_BINARY_DIR}/game_hdd\"")
set(TOX2_GAME_BOOT_MILESTONE "\\[t 01000000\\] tox2: first sc 988 ")
```

A milestone is a `tox2: first ...` line written by the main thread (thread id `01000000`), because
a line from another thread can appear or not depending on scheduling. The current one is the
main thread's first call of system call 988 (default-handled), the last main-thread first line
before the game calls `abort()` itself. The lines after it (`sys_tty_write`, `sys_process_exit`)
belong to the abort path, which an earlier failure would also reach, so they are not used. The
run ends with `tox2: stop: process exit status 1`. The first run of the test copies the 3 GB
`TLFILE.TLDAT` into `build/<preset>/game_hdd`, where it stays for the later runs. The milestone guards how far the main thread gets; it does not mean that the game works.

The pin file started with a placeholder milestone (the first `hle` or `sc` line of the main
thread), which was replaced once the game reached a census stop. When a change lets the game get
further, run `tox2_game --game-dir <dir> --no-window --seconds 20 --hdd-dir <empty-dir>` three
times, take the last main-thread `tox2: first ...` line that appears in every run, and set the
milestone to it. Update only that file; the test then guards against regressions.

Audio code is covered by `audio_core`, by `runtime_core` (`testEventQueueIpcKeys`,
`testPpuThreadCreatePriority`, `testTimerResolution`) and by `media_ffmpeg` (`testAtracFrameDecoder`).

## Python unit tests

Python unit tests use synthetic ELF files built in memory. Tests that need the real
executable are skipped unless `TOX2_EBOOT_ELF` points at it. The Ghidra comparison tests
additionally need `TOX2_GHIDRA_JSON`, the path of an export written by `ghidra-export`, and
are skipped without it. The tests never run Ghidra themselves. The tests of `recover` and
`emit` that use the real executable also need `TOX2_EBOOT_ELF`; they read the committed
`config/ghidra_summary.json` and `config/functions.json`. The `emit` tests assert the
counts in [Emitting C](emitter.md) (29,196 functions, 173 import trampolines, 467 switches) and
that no word is unhandled. The `recover` tests check, among other things, that the two TOCs are
found and that the switch at `0x0081a10c`, which belongs to the second TOC, is resolved.

```sh
python -m unittest discover -s tools/tests -t tools -v
TOX2_EBOOT_ELF=game/EBOOT.elf python -m unittest discover -s tools/tests -t tools -v
TOX2_EBOOT_ELF=game/EBOOT.elf TOX2_GHIDRA_JSON=generated/ghidra/eboot_ghidra.json python -m unittest discover -s tools/tests -t tools -v
```

The suite has 810 tests. Without the variables 66 are skipped; with `TOX2_EBOOT_ELF` alone 7 are,
and with both none are (that run takes a few minutes). When the pinned executable or the recovery
changes, update the values in `tools/tests/test_real_eboot.py` and the other real-executable tests
from the tool output.
