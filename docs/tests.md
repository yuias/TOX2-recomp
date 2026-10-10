# Tests

## CTest targets

`tools/build.ps1 -Test` runs every registered CTest target. Targets that need the game or local
files are registered only when those are configured, and configuration prints why one is not.

| Target | What it checks | Needs |
| --- | --- | --- |
| `runtime_version`, `runtime_musttail` | The runtime stub starts, and the compiler supports `musttail` | - |
| `tools_unittest` | The Python unit tests (below) | - |
| `ppu_header_check`, `ppu_semantics` | Contract header and the semantics harness (see [Emitting C](emitter.md)) | - |
| `ppu_link` | The generated code links | `-GeneratedDir` |
| `runtime_core` | Runtime core unit tests (memory, threads, lv2, HLE, VFS, fake RSX) | - |
| `sdat_tests` | SDAT decryption and decompression on synthetic data | - |
| `render_core`, `vp_translator`, `fp_translator` | Renderer and shader translators (see [Rendering](rendering.md)) | a Vulkan 1.3 device for the rendering parts |
| `rsx_replay` | Replays a local capture and compares it with a reference image | a local capture (see [Capture and replay](rendering.md#capture-and-replay)) |
| `media_core` | PAMF parser and demuxer, the movie player and the cellSail HLE on synthetic data, with fake codecs; with `DAL_GAME_DIR` set it also demuxes every `.pam` file of the game | - |
| `media_ffmpeg` | The FFmpeg loader, the H.264 and ATRAC3plus decoders (the movie decoder and the raw-frame decoder behind `cellAtrac`) and the colour and PCM conversion; with `DAL_GAME_DIR` set it also plays `ch_logo.pam` through the movie player with a fast clock | the fetched FFmpeg DLLs |
| `movie_decode` | `dal_moviedump` on the first 60 frames of `ch_logo.pam`: frame count, YUV and RGB hashes of three frames and the hash of the whole decoded audio, pinned in `cmake/movie_decode.cmake` | `-GameDir` (see [Movies](movies.md#tests)) |
| `audio_core` | Audio on synthetic data, with no device and no FFmpeg: the block ring, null output, WAV writer and reader, `wavStats`, `BlockClock` and its correction, the clock controller against a fake device, the engine with fake producers and outputs, the `cellAudio` state and handlers on synthetic guest memory, the AT3 parser, and the `cellAtrac` stream model and handlers with a fake decoder | - |
| `audio_wasapi` | The WASAPI output: device loss and reopen, priming, high-water drops and the fill level on fake streams, then (when a render device exists) opening the default device, playing silence and a real device loss and reopen. Exit code 77 (skipped) when the machine has no render device. Runs serially, because it measures timing | a render device for the real-device part |
| `input_core` | Gamepad code behind the backend interface, with a fake backend and no GameInput: value, reading and device formatting; the `--pad-probe` logic (a line per change, one `no gamepad reading` per device, devices coming and going, the `SteamVirtualGamepadInfo` lines); the button table, dead zone, stick bytes, trigger threshold, merging of pads and motor conversion; Steam virtual path parsing, `SDL_GAMECONTROLLER_IGNORE_DEVICES` parsing and the selection rule (recorded runs, a virtual pad, the ignore list, no Steam); the hub (use of a pad, removal, two pads, a virtual pad and its physical pad, the focus gate, vibration, the provider following plugging) and the gamepad thread with a fake backend (unavailable backend, feeding the pad, start lines, probe alone) | - |
| `input_gameinput` | The GameInput backend: the Steam context, then (with a GameInput runtime of API version 3) the device list, unique non-zero ids, a stable device generation, and `read`, `readRaw` and `setRumble` on unknown ids. No pad has to be connected. Exit code 77 (skipped) with the reason when no such runtime is installed | a GameInput runtime for all but the Steam context part (see [Input](input.md#the-gameinput-runtime-and-the-gameinput-unavailable-warning)) |
| `atrac_decode` | `dal_audiotool at3` on the title BGM (entry 19 of the decrypted `Bgm.sdat`): frame count, sample count and the SHA-256 of the decoded PCM, pinned in `cmake/atrac_decode.cmake` | the sdata cache (registered when it exists or can be built), the FFmpeg DLLs |
| `game_boot` | The game reaches a milestone in a 20 s run (below) | `-GeneratedDir`, `-EbootElf`, `-GameDir` |
| `game_input_hdd_clean`, `game_input` | A pad script leaves the title screen and the game reaches a milestone in a 40 s run (below) | as `game_boot`, plus the `.sdat` keys ([SDAT files](sdat.md#keys)) |
| `game_audio_hdd_clean`, `game_audio` | The same run as `game_input` with `--audio none --audio-dump`: the `atrac` line of the audio census reports decoded frames with audible content (below) | as `game_input` |

The game-file parts of `media_core` and `media_ffmpeg` read the `DAL_GAME_DIR` environment
variable; `tools/build.ps1` does not set it, so export it before running CTest to include them.
The pins of `movie_decode` hold for the pinned FFmpeg build; another build may round the RGB
conversion or the audio decoder differently, which needs new pins (the YUV hashes stay valid).

## The `game_boot` test

CTest registers `game_boot` only when `DAL_PPU_GENERATED_DIR`, `DAL_EBOOT_ELF` and
`DAL_GAME_DIR` are all set (`-GeneratedDir`, `-EbootElf`, `-GameDir`); otherwise configuration prints
that it is not registered. It runs `dal_game --game-dir <dir> --elf <elf>` with the arguments
from the pin file (the run keeps its game data in `build/<preset>/game_hdd`), with a 60 s time
limit, and passes when the output matches the milestone regular expression. The exit code is not
checked: the run ends at the 20 s watchdog or at a stop somewhere after the milestone, and a
stop before the milestone leaves the pattern unmatched.

The pin file `cmake/game_boot.cmake` holds the arguments and two milestones:

```cmake
set(DAL_GAME_BOOT_ARGS "--no-window --seconds 20 --hdd-dir \"${CMAKE_BINARY_DIR}/game_hdd\"")
set(DAL_GAME_BOOT_MILESTONE "\\[t 01000000\\] dal: first hle sceNpTrophy::sceNpTrophyGetTrophyUnlockState")
set(DAL_GAME_BOOT_MILESTONE_NO_SDATA "\\[t 01000000\\] dal: first hle cellSysutil::cellMsgDialogOpen2")
```

A milestone is a `dal: first ...` line written by the main thread (thread id `01000000`), because
a line from another thread can appear or not depending on scheduling. The first one applies when
the build has the `.sdat` keys (the test also passes `--sdata-cache`), the second when it has not and
the game stops at its error dialog for the missing `.sdat` file. When a change lets the game get
further, run `dal_game --game-dir <dir> --no-window --seconds 20 --hdd-dir <empty-dir>` three times, take the
last main-thread `dal: first ...` line that appears in every run, and set the matching milestone.
Update only that file; the test then guards against regressions.

## The `game_input` test

CTest registers `game_input` under the same conditions as `game_boot` when the build also has the
`.sdat` keys, because the game needs its decrypted `.sdat` files to get past the title screen. It
runs `dal_game` with `--sdata-cache`, `--pad-script tools/pad_scripts/leave_title.txt` and its own
hdd directory `build/<preset>/game_hdd_input`, which the setup test `game_input_hdd_clean` deletes
before every run, so that save data written after NEW GAME never reaches `game_boot` or the next
run. The pin file `cmake/game_input.cmake` holds the run length (`DAL_GAME_INPUT_SECONDS`, 40) and
the milestone (`DAL_GAME_INPUT_MILESTONE`): the main thread opening `Script/Event.sdat`, which a
run without input does not reach in that time. The time limit is the run length plus 60 s. The
script and the pad log lines are described in
[Pad script](runtime.md#pad-script) and [Log lines](runtime.md#log-lines). The runs are headless
(`--no-window`), so they never load GameInput and their logs have no `dal: pad: gameinput` line.

## The `game_audio` test

CTest registers `game_audio` under the same conditions as `game_input`. It runs the same pad script
for 40 s (`DAL_GAME_AUDIO_SECONDS` in `cmake/game_audio.cmake`) with `--audio none` and
`--audio-dump build/<preset>/game_audio.wav`, and its own hdd directory `build/<preset>/game_hdd_audio`,
which the setup test `game_audio_hdd_clean` deletes before every run. The time limit is 100 s. It
passes when the stop census has an `atrac` line with at least one non-silent decoded frame
(`DAL_GAME_AUDIO_ATRAC_PATTERN`): the title music and the confirm sound effect are both
ATRAC3plus, so it fails if the FFmpeg DLLs are missing, if the `cellAudio` mixer is not driven, or if
`cellAtrac` stops decoding. The WAV file is not inspected by the test; `dal_audiotool wavstat` reads
it (see [Audio](audio.md#dal_audiotool)).

`atrac_decode` takes the archive, the entry index and the expected counts from
`cmake/atrac_decode.cmake`. The hash of the decoded PCM holds for the pinned FFmpeg build; another
build or CPU feature set may round the float output differently and needs a new hash (the counts
stay).

Other audio code is covered by `runtime_core` (`testEventQueueIpcKeys`,
`testPpuThreadCreatePriority`, `testTimerResolution`) and `media_ffmpeg` (`testAtracFrameDecoder`,
`testAtracResetWithGameFile`, which is skipped unless `Se.sdat` is in the sdata cache of the build directory).

## Python unit tests

Python unit tests use synthetic ELF files built in memory. Tests that need the real
executable are skipped unless `DAL_EBOOT_ELF` points at it. The Ghidra comparison tests
additionally need `DAL_GHIDRA_JSON`, the path of an export written by `ghidra-export`, and
are skipped without it. The tests never run Ghidra themselves. The tests of `recover` and
`emit` that use the real executable also need `DAL_EBOOT_ELF`; the `emit` tests assert the
counts above and that no word is unhandled.

```sh
python -m unittest discover -s tools/tests -t tools -v
DAL_EBOOT_ELF=game/EBOOT.elf python -m unittest discover -s tools/tests -t tools -v
DAL_EBOOT_ELF=game/EBOOT.elf DAL_GHIDRA_JSON=generated/ghidra/eboot_ghidra.json python -m unittest discover -s tools/tests -t tools -v
```
