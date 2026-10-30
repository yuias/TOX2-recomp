# Tales of Xillia 2 / recompiled

Static recompilation of the PS3 title *テイルズ オブ エクシリア2* (*Tales of Xillia 2*, title ID
BLJS10188) for Windows and for macOS on Apple Silicon, built from your own copy of the game.

Only the Japanese release, BLJS10188, is supported. The North American release, BLUS31397, is not.

This repository contains no game data, executables, keys or decrypted binaries (see
[Licence and legal notice](#licence-and-legal-notice)). You supply your own `EBOOT.BIN` and the
firmware's `PS3UPDAT.PUP` (see [Game and firmware files](#game-and-firmware-files)).

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

## Requirements

### Windows (x64)

- Visual Studio 2026 with the Windows SDK
- LLVM (clang, clang++, lld)
- CMake 3.29 or newer
- Ninja
- Python 3.10 or newer
- git
- Vulkan SDK, and a GPU and driver with Vulkan 1.3
- PowerShell 7 with the `VSSetup` module

Put the LLVM `bin` directory on `PATH`, and install the PowerShell module with
`Install-Module VSSetup -Scope CurrentUser`. The Vulkan SDK installer sets the `VULKAN_SDK`
environment variable, which the build needs. FFmpeg and GameInput are fetched by scripts in
[Building](#building).

### macOS (Apple Silicon)

- macOS 14.4 or newer
- Xcode Command Line Tools
- Homebrew LLVM
- CMake 3.29 or newer
- Ninja
- Python 3.10 or newer
- FFmpeg
- git
- LunarG Vulkan SDK for macOS

Install the Command Line Tools with `xcode-select --install`, then [Homebrew](https://brew.sh), then
the rest with `brew install llvm cmake ninja python ffmpeg`. Install the Vulkan SDK with its
"System Global Installation" component.

### Game and firmware files

- `EBOOT.BIN` from your own dump of BLJS10188, at `<game-dir>/PS3_GAME/USRDIR/EBOOT.BIN`. `<game-dir>`
  is the directory that contains `PS3_GAME`.
- `PS3UPDAT.PUP` of PS3 system software 4.93 (206197916 bytes, SHA-256
  `158471fd834f8ea8036136b6aab43cd86c7ba73d79ca30e0af3c0fe0001cf365`). Another firmware version is
  refused. Sony publishes only the latest firmware, so the file must come from an earlier download.

## Building

Run every command from the repository root.

### Windows

1. Initialise the submodules:

   ```powershell
   git submodule update --init
   ```

2. Fetch FFmpeg and GameInput, and install the GameInput runtime (as administrator):

   ```powershell
   pwsh -NoProfile -File tools/fetch-ffmpeg.ps1
   pwsh -NoProfile -File tools/fetch-gameinput.ps1
   msiexec /i generated\deps\gameinput\<version>\redist\GameInputRedist.msi
   ```

3. Decrypt `EBOOT.BIN` and extract `libsre.prx` from the PUP into `generated/inputs/`:

   ```powershell
   pwsh -NoProfile -File tools/build.ps1 -Preset release -EbootBin <game-dir>/PS3_GAME/USRDIR/EBOOT.BIN -Pup <path>/PS3UPDAT.PUP
   ```

4. Recompile the executable, libsre and the SPU programs into C:

   ```powershell
   python tools/ps3tool.py emit generated/inputs/EBOOT.elf --chunks 256 --out generated/ppu
   python tools/ps3tool.py emit generated/inputs/libsre.reloc.elf --db config/libsre_functions.json --symbol-prefix libsre --chunks 4 --out generated/ppu/libsre --max-unhandled 0
   python tools/ps3tool.py spu-emit --eboot generated/inputs/EBOOT.elf --libsre generated/inputs/libsre.reloc.elf --out generated/ppu/spu --max-unhandled 0
   ```

5. Build the game:

   ```powershell
   pwsh -NoProfile -File tools/build.ps1 -Preset release -GeneratedDir generated/ppu -GameDir <game-dir>
   ```

The game is `build/release/bin/tox2_game.exe`.

### macOS

1. Initialise the submodules:

   ```sh
   git submodule update --init
   ```

2. Decrypt `EBOOT.BIN` and extract `libsre.prx` from the PUP into `generated/inputs/`:

   ```sh
   tools/build.sh --preset macos-release --eboot-bin <game-dir>/PS3_GAME/USRDIR/EBOOT.BIN --pup <path>/PS3UPDAT.PUP
   ```

3. Recompile the executable, libsre and the SPU programs into C:

   ```sh
   python3 tools/ps3tool.py emit generated/inputs/EBOOT.elf --chunks 256 --out generated/ppu
   python3 tools/ps3tool.py emit generated/inputs/libsre.reloc.elf --db config/libsre_functions.json --symbol-prefix libsre --chunks 4 --out generated/ppu/libsre --max-unhandled 0
   python3 tools/ps3tool.py spu-emit --eboot generated/inputs/EBOOT.elf --libsre generated/inputs/libsre.reloc.elf --out generated/ppu/spu --max-unhandled 0
   ```

4. Build the game:

   ```sh
   tools/build.sh --preset macos-release --generated-dir generated/ppu --game-dir <game-dir>
   ```

The game is `build/macos-release/bin/tox2_game`.

### Notes

- **Network.** The first configuration with `-EbootBin`/`-Pup` (`--eboot-bin`/`--pup`) downloads two
  RPCS3 source files that hold the decryption keys. To build offline, name the `rpcs3/Crypto`
  directory of a local RPCS3 checkout with `-Rpcs3CryptoDir` (`--rpcs3-crypto-dir`).
- **Checks.** Every decrypted file is checked against the size and SHA-256 in `config/target.json`,
  and the build log shows `matches the pin` for each. A file that does not match, such as a game
  update's `EBOOT.BIN`, is refused and nothing is written.
- **Later builds.** The CMake cache keeps the build parameters, so the last step does not repeat
  `-EbootBin` and `-Pup`.
- **Out of memory.** The executable is compiled as 256 C files. If the compiler runs out of memory,
  lower the parallelism with the `CMAKE_BUILD_PARALLEL_LEVEL` environment variable.
- **GameInput runtime.** `<version>` is the directory under `generated/deps/gameinput`. Without the
  runtime, gamepads are disabled and the keyboard still works.
- **Python on macOS.** The `python3` of macOS (`/usr/bin/python3`) can be 3.9; check
  `python3 --version` and use Homebrew's if needed.
- **Compiler on macOS.** Apple's clang is refused; the build uses `$(brew --prefix llvm)`. Another
  LLVM install can be named with `--llvm-prefix` or `TOX2_LLVM_PREFIX`. The first line of the build
  output shows which clang is used.
- **Decrypted files.** `generated/inputs/` is git-ignored. Do not share its contents.

Every build parameter, the CMake variables and the test builds are in [Building](docs/building.md);
decryption and the key download in detail are in
[Decrypting the inputs](docs/building.md#decrypting-the-inputs).

## Running the game

Windows:

```powershell
build/release/bin/tox2_game.exe
```

macOS:

```sh
build/macos-release/bin/tox2_game
```

The build writes `config.json` next to the executable with the game directory and the decrypted files,
so no arguments are needed. Settings changed in the debug overlay (`F1`, Settings) are saved to the
same file. `Alt+Enter` or `F11` switches fullscreen (macOS: `Ctrl+Cmd+F`).

Game data and saves are in `build/<preset>/hdd`. `-Clean` (`--clean`) deletes them with the rest of
the build directory.

Notes:

- **No `config.json`.** The file is written only by a build with `-GameDir`, and only when it does not
  exist yet. Without it, name the paths:
  `tox2_game --game-dir <game-dir> --elf generated/inputs/EBOOT.elf`. Without `--elf`, the game looks
  for `<game-dir>/PS3_GAME/USRDIR/EBOOT.elf`. `libsre.reloc.elf` is looked up next to the `EBOOT.elf`
  that is loaded; `--libsre <path>` names it explicitly.
- **macOS.** Start the game from a logged-in session; the window needs the window server. Window size
  presets are physical pixels, so on a Retina screen `720p` is a 640x360-point window. DLAA and frame
  generation are Windows only.

Every option of `tox2_game` and the `config.json` format are in
[`tox2_game` options](docs/runtime-options.md).

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

- If keys do nothing, click the window to give it keyboard focus.
- While a text box of the overlay has focus, keys go to the overlay; click outside it to give them
  back to the game. See [Debug overlay](docs/runtime.md#debug-overlay).
- On macOS, keys pressed with `Cmd` are system shortcuts and never pad input.

### Gamepad

- **Windows.** Xbox-family gamepads and the DualShock 4 (second model) work through GameInput; they need the GameInput runtime from the build steps. Quit
  Steam, or turn off its PlayStation controller support, before using a DualShock 4.
- **macOS.** Controllers with an extended gamepad profile work through GameController.framework,
  with the same mapping. This has not been tried with a real controller.

The mapping, the dead zone and troubleshooting are in [Input](docs/input.md).

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
