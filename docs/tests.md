# Tests

## CTest targets

`tools/build.ps1 -Test` runs every registered CTest target. Targets that need the game or local
files are registered only when those are configured, and configuration prints why one is not.

| Target | What it checks | Needs |
| --- | --- | --- |
| `decrypt_core` | `tox2_decrypt` on synthetic data: crypto known answers, key file parsing, SCE packages, SELF reassembly, PUP and TAR reading, and the command line | - |
| `decrypt_eboot` | `tox2_decrypt self` on the real `EBOOT.BIN`; the output must match the pin of `EBOOT.elf` (the file is written under `build/<preset>/decrypt_test`) | `-EbootBin` |
| `decrypt_libsre` | `tox2_decrypt pup` on the real `PS3UPDAT.PUP`; the output must match the pin of `libsre.prx` | `-Pup` |
| `decrypt_libsre_reloc` | `tox2_decrypt reloc` on `generated/inputs/libsre.prx` at `0x011d0000`; the output must match the pin of `libsre.reloc.elf` (written under `build/<preset>/decrypt_test`). The input file is written by the build step before this test runs | `-Pup` |
| `runtime_version`, `runtime_musttail` | The runtime stub starts, and the compiler supports `musttail` | - |
| `tools_unittest` | The Python unit tests (below). CTest passes `TOX2_EBOOT_ELF` and `TOX2_LIBSRE_ELF` to them, so they include the real-executable tests when `-EbootElf` or `-EbootBin` is given, and the real-libsre tests when `-Pup` is given | - |
| `ppu_header_check`, `ppu_semantics` | Contract header and the semantics harness (see [Emitting C](emitter.md)) | - |
| `ppu_link` | The generated code of the executable and of libsre links; the function mappings, imports and export table of both trees register and pass the checks of `runtime/tests/ppu_link_test.cpp` | `-GeneratedDir` (with the libsre tree under `<dir>/libsre`) |
| `runtime_core` | Runtime core unit tests (memory, threads, lv2, HLE, VFS, fake RSX, the module loader and the libsre layout check; the list is below) | - |
| `render_core`, `vp_translator`, `fp_translator` | Renderer and shader translators (see [Rendering](rendering.md)) | a Vulkan 1.3 device for the rendering parts |
| `rsx_replay` | Replays a local capture and, when a reference image is pinned, compares the result with it | a local capture (see [Capture and replay](rendering.md#capture-and-replay)) |
| `media_core` | PAMF parser and demuxer, the movie player and the cellSail HLE on synthetic data, with fake codecs (inherited code that this game does not use, see [Movies](movies.md)) | - |
| `media_ffmpeg` | The FFmpeg loader, the H.264 and ATRAC3plus decoders (the movie decoder and the raw-frame decoder behind `cellAtrac`) and the colour and PCM conversion, all on synthetic data | the fetched FFmpeg DLLs |
| `audio_core` | Audio on synthetic data, with no device and no FFmpeg: the block ring, null output, WAV writer and reader, `wavStats`, `BlockClock` and its correction, the clock controller against a fake device, the engine with fake producers and outputs, the `cellAudio` state and handlers on synthetic guest memory, the AT3 parser, and the `cellAtrac` stream model and handlers with a fake decoder | - |
| `audio_wasapi` | The WASAPI output: device loss and reopen, priming, high-water drops and the fill level on fake streams, then (when a render device exists) opening the default device, playing silence and a real device loss and reopen. Exit code 77 (skipped) when the machine has no render device. Runs serially, because it measures timing | a render device for the real-device part |
| `input_core` | Gamepad code behind the backend interface, with a fake backend and no GameInput: value, reading and device formatting; the `--pad-probe` logic (a line per change, one `no gamepad reading` per device, devices coming and going, the `SteamVirtualGamepadInfo` lines); the button table, dead zone, stick bytes, trigger threshold, merging of pads and motor conversion; Steam virtual path parsing, `SDL_GAMECONTROLLER_IGNORE_DEVICES` parsing and the selection rule (recorded runs, a virtual pad, the ignore list, no Steam); the hub (use of a pad, removal, two pads, a virtual pad and its physical pad, the focus gate, vibration, the provider following plugging) and the gamepad thread with a fake backend (unavailable backend, feeding the pad, start lines, probe alone) | - |
| `input_gameinput` | The GameInput backend: the Steam context, then (with a GameInput runtime of API version 3) the device list, unique non-zero ids, a stable device generation, and `read`, `readRaw` and `setRumble` on unknown ids. No pad has to be connected. Exit code 77 (skipped) with the reason when no such runtime is installed | a GameInput runtime for all but the Steam context part (see [Input](input.md#the-gameinput-runtime-and-the-gameinput-unavailable-warning)) |
| `game_boot` | The game reaches a milestone in a 20 s run (below) | `-GeneratedDir`, `-GameDir`, `-Pup` and `-EbootElf` or `-EbootBin` |

`rsx_replay` is registered when the capture named in `cmake/rsx_replay.cmake` exists under
`generated/rsx/captures/`. While `TOX2_RSX_REPLAY_REFERENCE_SHA256` in that file is empty, as it is
now, the test only replays the capture and checks the summary line. With a SHA-256 set, the test
also needs the matching reference image and applies the difference thresholds.

This game has no `.sdat` files and its movies are not PAMF, so there are no SDAT tests, no media
tests that read game files, and no pad-script or audio-content runs of the game. The synthetic
tests of the inherited media and audio code remain.

## The `game_boot` test

CTest registers `game_boot` only when `TOX2_PPU_GENERATED_DIR`, `TOX2_EBOOT_ELF`, `TOX2_GAME_DIR` and
`TOX2_PUP` are all set (`-GeneratedDir`, `-EbootElf`, `-GameDir`, `-Pup`); otherwise configuration prints
which of them is empty. `-EbootBin` also sets `TOX2_EBOOT_ELF`, to the produced
`generated/inputs/EBOOT.elf`, unless `-EbootElf` is given. `-Pup` is needed because the run loads libsre:
the test passes `--libsre generated/inputs/libsre.reloc.elf`. It runs
`tox2_game --game-dir <dir> --elf <elf> --libsre <libsre.reloc.elf>` with the arguments from the pin
file (the run keeps its game data in `build/<preset>/game_hdd`), with a 60 s time limit, and passes when
the output matches the milestone regular expression. The exit code is not checked.

The pin file `cmake/game_boot.cmake` holds the arguments and the milestone:

```cmake
set(TOX2_GAME_BOOT_ARGS "--no-window --seconds 20 --hdd-dir \"${CMAKE_BINARY_DIR}/game_hdd\"")
set(TOX2_GAME_BOOT_MILESTONE
    "tox2: stop: spu: sys_spu_thread_group_start group 0x[0-9a-f]+ \"[^\"]*CellSpursKernelGroup\" \\([0-9]+ threads, image 0x011f0380 entry 0x818\\)")
```

The milestone is the stop line of the first `sys_spu_thread_group_start`. Until libsre was loaded it was
the main thread's first call of system call 350 (`first sc 350`,
`sys_memory_allocate_from_container`), the last main-thread `tox2: first ...` line of a run. It moved
on purpose. The game creates SPURS instance A with PPU priority 1000 while its main thread has priority
1001, so under the creation rule of `sys_ppu_thread_create` (see [Threads](runtime.md#threads)) libsre's
handler thread `SpursHdlr0` runs before the main thread continues and starts the SPU thread group. The
run therefore ends, with exit code 8, inside the main thread's `cellSpursInitializeWithAttribute`,
before the main thread reaches any later `tox2: first ...` line, so no such line can be the milestone.
The stop line names the kernel image that libsre imported (`0x011f0380`) and its entry point (`0x818`),
so a wrong kernel, a failed image import, a libsre that did not run or an earlier stop leaves the
pattern unmatched. The group id and the thread count are not pinned. The run no longer writes the 3 GB
`TLFILE.TLDAT` copy into `build/<preset>/game_hdd`, because the stop comes before the install copy. The
milestone guards how far the main thread gets; it does not mean that the game works.

The pin file started with a placeholder milestone (the first `hle` or `sc` line of the main
thread), which was replaced once the game reached a census stop. When a change lets the game get
further, for example when SPU execution moves the stop, run `tox2_game --game-dir <dir> --elf <elf>
--no-window --seconds 20 --hdd-dir <empty-dir>` three times, from an empty directory and from one with
existing game data, take a line that appears in every run (the stop line, or the last main-thread
`tox2: first ...` line if the run no longer stops) and set the milestone to it. Update only that file;
the test then guards against regressions.

Audio code is covered by `audio_core`, by `runtime_core` (`testEventQueueIpcKeys`,
`testPpuThreadCreatePriority`, `testTimerResolution`) and by `media_ffmpeg` (`testAtracFrameDecoder`).

The tests of `runtime_core` that cover the libsre support are `testLoadModule` (the PRX loader and its
refusals), `testLibsreLayout` (the structural checks of the layout header, passing on synthetic guest
memory and failing once per check), `testDispatchSpans` (one dispatch span per image),
`testHleGuestExports`, `testModuleImports` and `testGuestExportConflicts` (binding the executable's
imports to libsre's exports, libsre's own import table, and the conflict check),
`testProcessAndPageSyscalls` (system calls 1, 14, 25, 131 and 351), `testSpuThreadGroups` and
`testSpuImageImport` (the SPU thread group bookkeeping and the image import), `testSpuPrintfCallbacks`,
`testPrxModuleId` and `testProcessParamsfo`, `testLwcond`, `testGuestPrintf` and
`testSysPrxForUserLibc` (the printf formatter and the string, printf and abort handlers). Four tests run
a child process, because the stop ends the process: `testOverlappingSpanChild`, `testAbortChild`,
`testSpuStartChild` and `testSpuJoinChild`.

## Python unit tests

Python unit tests use synthetic ELF and PRX files built in memory. Tests that need the real
executable are skipped unless `TOX2_EBOOT_ELF` points at it, and `test_real_libsre.py` (the recovery
of the committed `config/libsre_functions.json` from the relocated module) is skipped unless
`TOX2_LIBSRE_ELF` points at `generated/inputs/libsre.reloc.elf`. The Ghidra comparison tests
additionally need `TOX2_GHIDRA_JSON`, the path of an export written by `ghidra-export --cell`, and
are skipped without it. The tests never run Ghidra themselves. The tests of `recover` and
`emit` that use the real executable also need `TOX2_EBOOT_ELF`; they read the committed
`config/ghidra_summary.json` and `config/functions.json`. The `emit` tests assert the
counts in [Emitting C](emitter.md) (29,196 functions, 173 import trampolines, 467 switches) and
that no word is unhandled. The `recover` tests check, among other things, that the two TOCs are
found and that the switch at `0x0081a10c`, which belongs to the second TOC, is resolved.

```sh
python -m unittest discover -s tools/tests -t tools -v
TOX2_EBOOT_ELF=game/EBOOT.elf python -m unittest discover -s tools/tests -t tools -v
TOX2_EBOOT_ELF=game/EBOOT.elf TOX2_LIBSRE_ELF=generated/inputs/libsre.reloc.elf TOX2_GHIDRA_JSON=generated/ghidra/eboot_ghidra_cell.json python -m unittest discover -s tools/tests -t tools -v
```

The suite has 1180 tests. Without the variables 89 are skipped; with `TOX2_EBOOT_ELF` alone 30 are,
and with all three none are (that run takes a few minutes). When the pinned executable or the recovery
changes, update the values in `tools/tests/test_real_eboot.py` and the other real-executable tests
from the tool output.
