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
| `decrypt_font` | `tox2_decrypt pup --raw` on the real `PS3UPDAT.PUP`; the output must match the pin of `SCE-PS3-SR-R-JPN.TTF` | `-Pup` |
| `runtime_version`, `runtime_musttail` | The runtime stub starts, and the compiler supports `musttail` | - |
| `tools_unittest` | The Python unit tests (below). CTest passes `TOX2_EBOOT_ELF` and `TOX2_LIBSRE_ELF` to them, so they include the real-executable tests when `-EbootElf` or `-EbootBin` is given, and the real-libsre tests when `-Pup` is given | - |
| `ppu_header_check`, `ppu_semantics` | Contract header and the semantics harness (see [Emitting C](emitter.md)) | - |
| `ppu_link` | The generated code of the executable and of libsre links; the function mappings, imports and export table of both trees register and pass the checks of `runtime/tests/ppu_link_test.cpp` | `-GeneratedDir` (with the libsre tree under `<dir>/libsre`) |
| `ppu_overrides` | The hand-written replacements of guest functions (see [Overrides in use](emitter.md#overrides-in-use)), linked against fakes of the functions they call. The loader pushes into the ring or into staging under the stream mutex with the count updated in the same hold; the decoder setup runs SetData under the mutex, raises the flag only after it succeeded, sends a push that arrives during SetData to staging, and ends at the stop byte on a failed SetData without raising the flag. Each case runs in the Legacy and the Split2 thread mode, where every lv2 call opens its own kernel section | - |
| `runtime_core` | Runtime core unit tests (memory, threads, lv2, HLE, VFS, fake RSX, the module loader and the libsre layout check; the list is below). It also covers the pad interception of system dialogs and its release phase (`testCellPadIntercept`) and `cellSaveData` with a scripted dialog driver: the list, the confirmation loop, Fixed saves without a confirmation and the error messages (`testCellSaveData`) | - |
| `save_dialog` | The save data dialog without a window or a game (see [Save data dialog](runtime.md#save-data-dialog)). The model runs on an injected clock, so nothing sleeps: input edges, d-pad and stick auto-repeat, L1/R1 jumps, the circle/cross assignment, the fade-in and fade-out, scrolling, clamping and the results of a list, a Yes/No and an OK dialog. The text: sizes, dates, confirmation and error texts. The Auto and Cancel drivers, the board and the driver registry. The interactive driver runs against a fake pad provider: a list answered by pad input (in all three PPU thread modes), a message answered by pad input, a dialog with no drawer or whose drawer fails during the wait answering as cancelled, a refused `DRAWING_BEGIN`, and `cellMsgDialogOpen2` answering BUSY while a dialog is open | - |
| `spu_header_check` | The SPU contract headers (`spu_context.h`, `spu_ops.h`, `spu_fp.h`, `spu_lower_stackfree.h`) compile as C23, as C++26 and with `-ffreestanding`, and a run-time check of the context layout, the inline helpers and the stack-free macros against a minimal dispatcher passes (`spu header ok`) | - |
| `spu_vector_helpers` | The vector forms of the SPU single-precision arithmetic (`fa`, `fs`, `fm`, `fma`, `fms`, `fnms`), run in the SPU MXCSR (round toward zero, FTZ, DAZ), match the scalar `tox2SpuSf*` functions of `spu_fp.h` run in round to nearest, also over a grid of edge words (denormals, -0, the limits, exponent 255); the helpers that switch MXCSR give the same words in both environments; and the `pshufb` form of `shufb` matches a byte loop that follows the ISA, on random words weighted toward zero and denormal exponents, exponent 255, results near the limits and cancellation (`spu vector helpers ok`) | - |
| `spu_semantics` | The SPU instruction semantics: hand-derived cases per op group (`tools/tests/spu_semantics/cases_<group>.py`, from the SPU ISA text) go through the real recovery and emitter, are compiled like emitted code and run natively (see [Emitting C](emitter.md#semantics-harness)). Pass case names as arguments to run only those | - |
| `spu_link` | The generated SPU tree links with the stub runtime, its image tables have the expected shape (hashes, segments, entries) and every entry can be dispatched once with a halt request set, so no SPU code runs (`spu link ok`) | `-GeneratedDir` (with `spu/` under it) |
| `spu_runtime` | The SPU runtime in `tox2_runtime_core` on synthetic images and plain host buffers: the reservation table (including the race window through a test seam, stripe sharing, wake-ups and stress runs), SPU threads and the dispatcher (flat host stack over 10,000 transfers, transfers across images, image tracking and its edge cases), MFC commands, mailboxes, events and the decrementer, lv2 thread group start, exit, terminate, join and teardown, the `[spu XXXXXXXX]` log prefix, the supervisor and the census. The fatal stops run in child processes | - |
| `spu_programs` | Four assembled SPU programs (a sum over an array, an atomic counter, a jump table, an event throw) go through recovery, the emitter and the runtime: they run as a thread group of up to 6 threads on the real runtime and their results are checked | - |
| `spu_lzma_inflate` | The recompiled Edge LZMA inflate routine of the game's LZMA task decompresses a real item of `TLFILE.TLDAT` on one SPU thread, chunk by chunk as the task does, and the SHA-256 of the output must be `f4ad1f71...c751` (the value of the reference decoder). A second line gives the time spent in the recompiled code and the throughput (about 25 MB/s at `-O3` on the development machine, 3.7 MB/s at `-O1`); it is not checked. Timeout 120 s | `-GeneratedDir`, `-GameDir` and `-EbootElf` or `-EbootBin` |
| `render_core`, `vp_translator`, `fp_translator` | Renderer and shader translators (see [Rendering](rendering.md)). `vp_translator` and `fp_translator` also translate every program dumped under `generated/rsx`: the known ones are compared with their expected GLSL, the others must compile when they translate, and the programs the translators report are counted by report text in a printed summary, not failed (the tree has 33 vertex and 749 fragment dumps). `render_core` also covers the stencil state (decode, the zeta write rule, the test itself and a stencil-only pass followed by a read of the depth surface as a texture), user clip planes (decode and clipping), rebinding a target at another size (the overlap copy, its skips and the counted refusals) and vertex program condition codes drawn on the device. `vp_translator` also checks the condition code update and its fallback destination, conditional writes, the vector-before-scalar order and the user clip distance output. `render_core` also checks the idle present rule (`idlePresentDue`: 100 ms after the last flip, then every 33 ms, measured from the last real flip) and that `presentIdle` does nothing without a presenter and never counts as a flip | a Vulkan 1.3 device for the rendering parts |
| `rsx_replay` | Replays a local capture and, when a reference image is pinned, compares the result with it | a local capture (see [Capture and replay](rendering.md#capture-and-replay)) |
| `media_core` | PAMF parser and demuxer, the movie player and the cellSail HLE on synthetic data, with fake codecs (inherited code that this game does not use, see [Movies](movies.md)) | - |
| `media_ffmpeg` | The FFmpeg loader, the H.264 and ATRAC3plus decoders (the movie decoder and the raw-frame decoder behind `cellAtrac`) and the colour and PCM conversion, all on synthetic data | the fetched FFmpeg DLLs |
| `audio_core` | Audio on synthetic data, with no device and no FFmpeg: the block ring, null output, WAV writer and reader, `wavStats`, `BlockClock` and its correction, the clock controller against a fake device, the engine with fake producers and outputs, the `cellAudio` state and handlers on synthetic guest memory, the AT3 parser, and the `cellAtrac` stream model and handlers with a fake decoder | - |
| `audio_wasapi` | The WASAPI output: device loss and reopen, priming, high-water drops and the fill level on fake streams, then (when a render device exists) opening the default device, playing silence and a real device loss and reopen. Exit code 77 (skipped) when the machine has no render device. Runs serially, because it measures timing | a render device for the real-device part |
| `overlay_core` | The debug overlay (see [Debug overlay](runtime.md#debug-overlay)): the flip statistics (flips per second, the frame-time ring, draw and skip deltas), the refresh intervals of the audio and unimplemented providers, the panels drawn headless on a private ImGui context with full and empty sources, the runtime providers (flip limiter, `rsx unimplemented` counts, log level; no audio engine or RSX thread runs), and the `F1` toggle and key gate. With a Vulkan device it also records the overlay over an offscreen image twice (title bar lit, far corner untouched), then a second overlay on the same device, and feeds a Win32 mouse move through a hidden window; it passes when the validation layer reports no error. For the save data dialog it covers the overlay stack (merged requests, draw order, `wantsDraw` and `wantsIdlePresent` of the children), the font candidate order and validation, the code points an atlas needs for a dialog, and, with a device, the save dialog overlay recorded over an offscreen image (a message, a list with the legend and scrollbar, the atlas rebuilds and the icon textures); the check reads the pixels back, and the validation layer must report no error Without a device it prints `overlay tests: vulkan: skipped (<reason>)` and still passes | a Vulkan 1.3 device for the recording parts |
| `input_core` | Gamepad code behind the backend interface, with a fake backend and no GameInput: value, reading and device formatting; the `--pad-probe` logic (a line per change, one `no gamepad reading` per device, devices coming and going, the `SteamVirtualGamepadInfo` lines); the button table, dead zone, stick bytes, trigger threshold, merging of pads and motor conversion; Steam virtual path parsing, `SDL_GAMECONTROLLER_IGNORE_DEVICES` parsing and the selection rule (recorded runs, a virtual pad, the ignore list, no Steam); the hub (use of a pad, removal, two pads, a virtual pad and its physical pad, the focus gate, vibration, the provider following plugging) and the gamepad thread with a fake backend (unavailable backend, feeding the pad, start lines, probe alone) | - |
| `input_gameinput` | The GameInput backend: the Steam context, then (with a GameInput runtime of API version 3) the device list, unique non-zero ids, a stable device generation, and `read`, `readRaw` and `setRumble` on unknown ids. No pad has to be connected. Exit code 77 (skipped) with the reason when no such runtime is installed | a GameInput runtime for all but the Steam context part (see [Input](input.md#the-gameinput-runtime-and-the-gameinput-unavailable-warning)) |
| `game_boot` | The game reaches a milestone in a 30 s run (below) | `-GeneratedDir`, `-GameDir`, `-Pup` and `-EbootElf` or `-EbootBin` |

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
file (the run keeps its game data in `build/<preset>/hdd`), with a 60 s time limit, and passes when
the output matches the milestone regular expression. The exit code is not checked. The pin file passes
`--no-config`, so a `config.json` in the working directory or next to `tox2_game` never changes the
run; do the same in every other scripted run of `tox2_game`.

The pin file `cmake/game_boot.cmake` holds the arguments and the milestone:

```cmake
set(TOX2_GAME_BOOT_ARGS "--no-config --no-window --seconds 30 --hdd-dir \"${CMAKE_BINARY_DIR}/hdd\"")
set(TOX2_GAME_BOOT_MILESTONE "\\[t 01000000\\] tox2: first hle cellSpurs::cellSpursEventFlagTryWait")
```

The milestone is the main thread's first call of `cellSpursEventFlagTryWait`, the last
`tox2: first ...` line of the main thread. The game reaches it only after the first LZMA batch of the
SPURS engine tasks has run on the recompiled SPU code, so a run that stops earlier (an SPU stop, a
stall, or the watchdog before the batch ends) leaves the pattern unmatched. The line first appears at
12 to 14 s when the hdd directory already holds the installed game data and at 14 to 16 s from an empty
directory; the test must pass from both. The run is limited to 30 s because the milestone appears
within 16 s and a longer run only adds time. A run that passes ends with
`tox2: stop: watchdog 30 s` (exit code 5), which the test does not check. The milestone guards how far
the main thread gets; it does not mean that the game works.

Earlier pins were the main thread's `first sc 350` line (before libsre was loaded) and the stop line of
the first `sys_spu_thread_group_start`. Each moved on purpose when the game got further. When a change
lets the game get further again, run `tox2_game --game-dir <dir> --elf <elf> --libsre <libsre.reloc.elf>
--no-config --no-window --seconds 30 --hdd-dir <dir>` three times, from an empty directory and from one with
existing game data, take a line that appears in every run (the last main-thread `tox2: first ...`
line, or the stop line if the run stops) and set the milestone to it. Update only that file; the test
then guards against regressions.

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
`testSysPrxForUserLibc` (the printf formatter and the string, printf and abort handlers), and
`testSpuJoinNotInitialized` (a join on a group that is not fully initialised does not wait). Tests that
run a child process, because the stop ends the process, include `testOverlappingSpanChild`,
`testAbortChild` and `testSpuStartChild` (a group started with no SPU images registered stops at its
first dispatch, `unknown target ls 0x00818 ... (untracked)`). The execution of SPU code is tested by
`spu_runtime`, `spu_programs` and `spu_lzma_inflate`, not here.

## Python unit tests

Python unit tests use synthetic ELF and PRX files built in memory. Tests that need the real
executable are skipped unless `TOX2_EBOOT_ELF` points at it, and `test_real_libsre.py` (the recovery
of the committed `config/libsre_functions.json` from the relocated module) is skipped unless
`TOX2_LIBSRE_ELF` points at `generated/inputs/libsre.reloc.elf`. The Ghidra comparison tests
additionally need `TOX2_GHIDRA_JSON`, the path of an export written by `ghidra-export --cell`, and
are skipped without it. The tests never run Ghidra themselves. The tests of `recover` and
`emit` that use the real executable also need `TOX2_EBOOT_ELF`; they read the committed
`config/ghidra_summary.json` and `config/functions.json`. The `emit` tests assert the
counts in [Emitting C](emitter.md) (29,196 functions, 173 import trampolines, 480 switches) and
that no word is unhandled. The `recover` tests check, among other things, that the two TOCs are
found and that the switch at `0x0081a10c`, which belongs to the second TOC, is resolved.

```sh
python -m unittest discover -s tools/tests -t tools -v
TOX2_EBOOT_ELF=game/EBOOT.elf python -m unittest discover -s tools/tests -t tools -v
TOX2_EBOOT_ELF=game/EBOOT.elf TOX2_LIBSRE_ELF=generated/inputs/libsre.reloc.elf TOX2_GHIDRA_JSON=generated/ghidra/eboot_ghidra_cell.json python -m unittest discover -s tools/tests -t tools -v
```

The suite has 1208 tests. Without the variables 90 are skipped; with `TOX2_EBOOT_ELF` alone 31 are,
and with all three none are (that run takes a few minutes). When the pinned executable or the recovery
changes, update the values in `tools/tests/test_real_eboot.py` and the other real-executable tests
from the tool output.
