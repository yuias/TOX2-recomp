# Tales of Xillia 2 / recompiled

Static recompilation of the PS3 title *テイルズ オブ エクシリア2* (*Tales of Xillia 2*, title ID
BLJS10188) for Windows and for macOS on Apple Silicon, built from your own copy of the game.

Only the Japanese release, BLJS10188, is supported. The North American release, BLUS31397, is not.

This repository contains no game data (see [Licence and legal notice](#licence-and-legal-notice)).
No executables, keys, or decrypted binaries are included or accepted by version control. You
supply your own `EBOOT.BIN` and the firmware's `PS3UPDAT.PUP`; the decrypted files are written to
the ignored `generated/inputs/`, and the keys they need are downloaded at configure time, not
stored (see [Decrypting `EBOOT.elf` and `libsre.prx`](#decrypting-ebootelf-and-libsreprx)).

## Status

Work in progress. The game's executable, the firmware's SPURS module (`libsre`) and all SPU programs
are recompiled. The game can be started from the title screen and played, with picture and sound; a
full play-through has not been checked, and the attract movie is still drawn with a placeholder
texture. On macOS it has been run from boot into the opening scenes. See [Status](docs/status.md).

## Features

On top of the recompiled game, the port adds:

- **Windows and macOS.** Native builds for Windows (x64) and for macOS on Apple Silicon (arm64) from
  the same code. Both draw with Vulkan; on macOS Vulkan runs on top of Metal, through MoltenVK or
  KosmicKrisp.
- **Internal resolution scale.** Every render target can be drawn at 1 to 4 times the game's size,
  for a sharper picture of the same frame.
- **DLAA** (Windows, NVIDIA RTX). The game's anti-aliased scene can be anti-aliased by NVIDIA DLSS in
  its DLAA mode, with per-object motion vectors. MSAA and supersampling are available on every GPU.
- **Frame generation** (Windows, NVIDIA RTX 40 and 50 series). DLSS Frame Generation presents 60, 90,
  120, 180 or as many frames per second as possible by inserting frames between the game's own, while
  the game itself keeps its speed. It needs DLAA.
- **Anisotropic filtering** from 2x to 16x.
- **Window and settings.** Window sizes from 720p to 2160p and borderless fullscreen. The debug
  overlay (`F1`) changes these and the options above while the game runs, and saves them.

DLAA and frame generation need the optional NVIDIA Streamline SDK at build time (see
[NVIDIA Streamline (DLAA)](docs/building.md#nvidia-streamline-dlaa)).

## Prerequisites

Sections that differ by system are marked Windows or macOS; the others apply to both.

### Windows

- Visual Studio 2026 with the Windows SDK (provides the MSVC headers and libraries)
- PowerShell 7 and the `VSSetup` module (`Install-Module VSSetup -Scope CurrentUser`)
- clang and clang++ with lld (a standalone LLVM install, on `PATH`)
- CMake 3.29 or newer
- Ninja
- git, with the submodules initialised: `git submodule update --init` (clone with
  `--recurse-submodules` to do it in one step)
- The Vulkan SDK, with the `VULKAN_SDK` environment variable set
- A Vulkan 1.3 device and driver
- Python 3.10 or newer (the tools use only the standard library)
- Internet access for the first configuration with `-EbootBin` or `-Pup`: two RPCS3 source files that
  contain the keys are downloaded (see Building for an offline alternative)

### macOS

- A Mac with Apple Silicon (arm64) running macOS 14.4 or newer, and the Xcode Command Line Tools
  (`xcode-select --install`), which Homebrew needs and which provide the macOS SDK. The port was built
  and run on Apple Silicon only.
- [Homebrew](https://brew.sh), with these packages: `brew install llvm cmake ninja python ffmpeg`.
  The build uses Homebrew's clang (found with `brew --prefix llvm`); Apple's clang is refused when
  CMake configures. Another LLVM install can be named with `--llvm-prefix` or the `TOX2_LLVM_PREFIX`
  environment variable (see [Building](docs/building.md#toolsbuildsh)). CMake 3.29 or newer is needed.
- Python 3.10 or newer as `python3` (the tools use only the standard library). The `python3` that macOS
  provides (`/usr/bin/python3`) can be 3.9; Homebrew's `python` is newer. Check `python3 --version`
  before running `ps3tool.py`.
- The LunarG Vulkan SDK for macOS, installed with its "System Global Installation" component. It
  provides the Vulkan loader, the shader compiler library and the two Metal drivers (KosmicKrisp and
  MoltenVK) under `/usr/local`. If `VULKAN_SDK` is exported (the SDK's `macOS` directory), configure
  takes the shader compiler from there; otherwise it uses the copy in `/usr/local`.
- git, with the submodules initialised: `git submodule update --init` (clone with
  `--recurse-submodules` to do it in one step)
- Internet access for the first configuration with `--eboot-bin` or `--pup`, as on Windows.
- No FFmpeg download and no GameInput runtime: FFmpeg comes from Homebrew and gamepads need nothing
  to install.

## Decrypting `EBOOT.elf` and `libsre.prx`

The tools work on the decrypted PPU executable, not on `EBOOT.BIN`. The game calls the SPURS code of
the firmware's `libsre.prx`, which this project recompiles and loads next to the executable, so the
build needs the PUP as well: `-Pup` is required. `tox2_decrypt` produces all inputs from files you own;
RPCS3 is not needed.

1. Dump your own disc or digital copy. The directory that contains `PS3_GAME` is the game
   directory (`<game-dir>` below).
2. Download the PS3 system software update 4.93, `PS3UPDAT.PUP`, from PlayStation's official site.
   It is not installed anywhere; the tool reads it as a file. The tested file is 206197916 bytes with
   SHA-256 `158471fd834f8ea8036136b6aab43cd86c7ba73d79ca30e0af3c0fe0001cf365`. Sony publishes only
   the latest firmware, and a PUP of another version is refused, because `config/target.json` pins `libsre.prx` of
   version 4.93.
3. Build once with both files. This configures, downloads the key files, builds `tox2_decrypt` and
   runs it:

   ```powershell
   pwsh -NoProfile -File tools/build.ps1 -Preset release -EbootBin <game-dir>/PS3_GAME/USRDIR/EBOOT.BIN -Pup <path>/PS3UPDAT.PUP
   ```

   On macOS:

   ```sh
   tools/build.sh --preset macos-release --eboot-bin <game-dir>/PS3_GAME/USRDIR/EBOOT.BIN --pup <path>/PS3UPDAT.PUP
   ```

   The outputs are `generated/inputs/EBOOT.elf`, `generated/inputs/libsre.prx` and
   `generated/inputs/libsre.reloc.elf` (`libsre.prx` placed at the guest address `0x011d0000` with its
   relocations applied). Each is checked against the size and SHA-256 in `config/target.json`; a file
   that does not match is refused and nothing is written. The build log shows `matches the pin` for
   all three.

The supported executable has the SHA-256
`a1d8a56ce1d6300bff704a43218f1b8b87c80c1e1342929a266e58af4e5891e9`; `tox2_game` refuses any other.
`libsre.reloc.elf` is the input of the libsre recompilation, and `tox2_game` loads it at run time
(see Running the game). None of the decrypted files may be shared. The key download, the offline alternative and the checks are described in
[Building](docs/building.md#decrypting-the-inputs).

## Building

### Windows

Run these from the repository root:

1. Fetch FFmpeg, which decodes the game's sound, and the GameInput package, which provides the
   gamepad API. This is needed once.

   ```powershell
   pwsh -NoProfile -File tools/fetch-ffmpeg.ps1
   pwsh -NoProfile -File tools/fetch-gameinput.ps1
   ```

2. Install the GameInput runtime, which reads the gamepads. This is needed once and requires
   administrator rights; without it gamepads are disabled and the keyboard still works.
   `<version>` is the directory under `generated/deps/gameinput`.

   ```powershell
   msiexec /i generated\deps\gameinput\<version>\redist\GameInputRedist.msi
   ```

3. Recompile the executable, libsre and the SPU programs into C source under `generated/ppu`. The
   executable is large, so its output is split into 256 chunks (the tool default is 16); libsre needs 4:

   ```powershell
   python tools/ps3tool.py emit generated/inputs/EBOOT.elf --chunks 256 --out generated/ppu
   python tools/ps3tool.py emit generated/inputs/libsre.reloc.elf --db config/libsre_functions.json --symbol-prefix libsre --chunks 4 --out generated/ppu/libsre --max-unhandled 0
   python tools/ps3tool.py spu-emit --eboot generated/inputs/EBOOT.elf --libsre generated/inputs/libsre.reloc.elf --out generated/ppu/spu --max-unhandled 0
   ```

   `emit` reads the committed function databases `config/functions.json` and
   `config/libsre_functions.json`; `spu-emit` reads the committed ones in `config/spu/`. `recover` is
   needed only to regenerate them (see [Function recovery](docs/function-recovery.md)).

4. Build. The cache of the release build already points at `generated/inputs/EBOOT.elf` and keeps the
   `-EbootBin` and `-Pup` of the first build, so they are not needed again:

   ```powershell
   pwsh -NoProfile -File tools/build.ps1 -Preset release -GeneratedDir generated/ppu -GameDir <game-dir>
   ```

The build writes `tox2_game` to `build/release/bin/`. The generated code is compiled in many
chunks; if the compiler runs out of memory, lower the parallelism with the
`CMAKE_BUILD_PARALLEL_LEVEL` environment variable.

Every build parameter, the CMake variables and the test builds are described in
[Building](docs/building.md).

### macOS

Run these from the repository root. `tools/build.sh` is the counterpart of `tools/build.ps1`; its
preset `macos-release` is the optimised build (`macos-debug`, the default, is slower; use
`macos-release` to play).

1. Decrypt the executable and `libsre`. This configures, downloads the key files, builds
   `tox2_decrypt` and runs it:

   ```sh
   tools/build.sh --preset macos-release --eboot-bin <game-dir>/PS3_GAME/USRDIR/EBOOT.BIN --pup <path>/PS3UPDAT.PUP
   ```

   The outputs are the same as on Windows, checked against `config/target.json`. The first line of
   the output is the version of the clang that is used, which must be Homebrew's.

2. Recompile the executable, libsre and the SPU programs into C source under `generated/ppu`. Use
   `python3`, and make sure it is 3.10 or newer:

   ```sh
   python3 tools/ps3tool.py emit generated/inputs/EBOOT.elf --chunks 256 --out generated/ppu
   python3 tools/ps3tool.py emit generated/inputs/libsre.reloc.elf --db config/libsre_functions.json --symbol-prefix libsre --chunks 4 --out generated/ppu/libsre --max-unhandled 0
   python3 tools/ps3tool.py spu-emit --eboot generated/inputs/EBOOT.elf --libsre generated/inputs/libsre.reloc.elf --out generated/ppu/spu --max-unhandled 0
   ```

3. Build:

   ```sh
   tools/build.sh --preset macos-release --generated-dir generated/ppu --game-dir <game-dir>
   ```

The build writes `tox2_game` to `build/macos-release/bin/`. FFmpeg is not copied: `tox2_game` loads
Homebrew's libraries from where they are installed (see [Movies](docs/movies.md#ffmpeg-on-macos)).
Presets, the options of `tools/build.sh`, the Vulkan drivers and debugging are in
[Building](docs/building.md#building-on-macos).

## Running the game

### Windows

A build configured with `-GameDir` (and `-EbootBin` or `-EbootElf`, and `-Pup`) writes
`build/release/bin/config.json` with the game directory and the paths of the decrypted executable and
`libsre.reloc.elf`, so the game starts without arguments from any working directory:

```powershell
build/release/bin/tox2_game.exe
```

The file is written only when it does not exist, so edits survive later builds; when it differs from
the build's paths, configuration says so and leaves it alone. Without the file, name the paths on the
command line:

```powershell
build/release/bin/tox2_game.exe --game-dir <game-dir> --elf generated/inputs/EBOOT.elf
```

`--elf` names the decrypted executable. Without it `tox2_game` looks for
`<game-dir>/PS3_GAME/USRDIR/EBOOT.elf`; copying the produced file there works too. The relocated
firmware module `libsre.reloc.elf` is loaded as well. It is looked up in the directory of the
executable, so with `--elf generated/inputs/EBOOT.elf` it is the file the build wrote, and when you copy
`EBOOT.elf` into `USRDIR` you copy `libsre.reloc.elf` next to it. `--libsre <path>` names it
explicitly.

In the headless runs so far the game ran its start-up code, libsre built its SPURS instance, the SPURS
kernels and the game's SPU tasks ran, and the game reached its frame loop; a run with `--seconds 30` or
`--seconds 180` ends at the watchdog. The renderer draws the game's frames: the frames dumped in the first
30 s are a plain grey loading screen, and those of longer runs show the legal notice, the
downloadable-content dialog and the title screen. In a windowed run the game can be played past the
title screen with a gamepad or the keyboard, with sound. [Status](docs/status.md) has the details. Closing the window ends the run. Game data and
saves are written to `build/release/hdd`; `tools/build.ps1 -Clean` deletes them together with the
rest of `build/release`.

`config.json` also holds the settings changed while playing. The debug overlay (`F1`, Settings)
changes the frame limiter, the internal resolution scale, anti-aliasing, anisotropic filtering and the
window size (720p to 2160p), and `Alt+Enter` or `F11` switches fullscreen (on macOS the system's
fullscreen, see below). Each change is saved to the file as it is made and restored at the next start. Options given on the command line win and are never saved,
and `--no-config` ignores the file. The format, the lookup and the rules for saving are in
[Configuration file](docs/runtime-options.md#configuration-file).

Every option of `tox2_game` is described in [`tox2_game` options](docs/runtime-options.md).

### Running on macOS

```sh
build/macos-release/bin/tox2_game
```

A build configured with `--game-dir` (and `--eboot-bin` or `--eboot-elf`, and `--pup`) writes
`build/macos-release/bin/config.json` under the same rules as on Windows, so the game starts without
arguments. Without the file, name the paths as above:

```sh
build/macos-release/bin/tox2_game --game-dir <game-dir> --elf generated/inputs/EBOOT.elf
```

The options, `--elf`, `--libsre`, `config.json` and the `hdd` directory work as on Windows, with the
executable called `tox2_game` (no `.exe`). Closing the window and `Cmd+Q` both end the run. Start the
game from a session where you are logged in: the window needs the window server, so a run over SSH
works only with `--no-window`. Fullscreen is the system's: `Ctrl+Cmd+F` (View menu, "Toggle Full Screen")
or the green window button; `--fullscreen on` starts in it. The window size presets are physical
pixels, so on a Retina screen `720p` is a 640x360-point window. DLAA and frame generation are
Windows only.

### Keyboard

The keyboard is the pad on port 0. The keys are physical key positions, so the layout stays in
place on AZERTY and QWERTZ keyboards:

| Keys | Pad |
| --- | --- |
| Arrow keys (not the numpad) | D-pad |
| `Z` / `X` / `S` / `D` | Cross / Circle / Square / Triangle |
| `W` / `E` / `R` / `U` | L1 / L2 / R1 / R2 |
| `1` / `3` (main row) | L3 / R3 |
| `V` / `C` | Start / Select |
| `F1` | Show or hide the debug overlay (not a pad button) |

While the overlay has the keyboard focus (after a click on one of its text boxes), key presses go to
the overlay and not to the pad; click outside the overlay to give the keys back to the game. The
overlay shows the flip rate, the draw and audio counters, the run-time switches, the capture
buttons and the `rsx unimplemented` counts; see [Debug overlay](docs/runtime.md#debug-overlay).

On macOS the same physical key positions are used. Key presses that include `Cmd` are system
shortcuts, such as `Cmd+Q`, and are never pad input. Keys that macOS reports without a PC scan code
(`Fn`, media keys, the Japanese `Eisu` and `Kana` keys) do nothing.

If keys do nothing, click the window to give it keyboard focus. The input code is unchanged from the
project this one was derived from, and it has not been tried with this game.

### Gamepad

A connected Xbox-family gamepad or DualShock 4 (second model) works in the window together with the
keyboard; it needs the GameInput runtime from the build step above. Quit Steam, or turn off its
PlayStation controller support, before using a DualShock 4. The button mapping, the dead zone, Steam
and the `gameinput unavailable` warning are in [Input](docs/input.md).

On macOS gamepads are read through GameController.framework, which needs nothing to install: a
controller that macOS reports with an extended gamepad profile works in the window together with the
keyboard, with the same button mapping, and vibrates through Core Haptics when it supports that.
Input reaches the game only while its window is the active one. The Steam and GameInput notes above
apply to Windows only. This has not been tried with a controller; see
[Input](docs/input.md#gamepads-on-macos).

## Documentation

Development material is under [`docs/`](docs/README.md): build and run parameters, tests, the
analysis tools, function recovery, the C emitter, runtime internals, the virtual file system,
rendering, movies, audio, gamepad input, the repository layout, and a guide for applying the method
to other PS3 games.

## Licence and legal notice

The code in this repository is licensed under 0BSD; see [LICENSE](LICENSE). Third-party components keep
their own licences; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

This is an unofficial project. It is not affiliated with, authorised by or endorsed by any rights
holder of the game.

*テイルズ オブ エクシリア2*, *Tales of Xillia 2* and all related names and logos are trademarks or
registered trademarks of their respective owners. They are used here only to identify the game and
platform this project works with.

No game files, keys or other copyrighted game data are included in this repository, and none
will be provided. Please do not ask for them.
