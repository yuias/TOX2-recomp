# Tales of Xillia 2 / recompiled

Static recompilation of the PS3 title *テイルズ オブ エクシリア2* (*Tales of Xillia 2*, title ID
BLJS10188) for Windows, built from your own copy of the game.

This repository contains no game data (see [Licence and legal notice](#licence-and-legal-notice)).
No executables, keys, or decrypted binaries are included or accepted by version control; you
supply your own `EBOOT.elf`, and all tool output is written to ignored directories.

Current state: work in progress. The tools and the runtime were taken over from an earlier
recompilation project of another game and are being pointed at this one. The executable is
recovered, recompiled to C and linked, and `tox2_game` runs its start-up code for a few seconds,
issues a few flips and ends when the game calls `abort()` itself; the game cannot be played. The SPU code (8 embedded SPU images,
SPURS and raw SPU thread groups), the `TLFILE.TLDAT` / `FILEHEADER.TOFHDB` data archive and the
Bink movies are not handled. See [Status](docs/status.md).

## Prerequisites

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
- RPCS3, to decrypt the game's executable

## Obtaining `EBOOT.elf`

The tools work on the decrypted PPU executable, not on `EBOOT.BIN`.

1. Dump your own disc or digital copy. The directory that contains `PS3_GAME` is the game
   directory (`<game-dir>` below).
2. In RPCS3, use Utilities > Decrypt PS3 Binaries and select the game's `EBOOT.BIN`.
3. Place the resulting `EBOOT.elf` at `<game-dir>/PS3_GAME/USRDIR/EBOOT.elf`, where `tox2_game`
   looks for it.

The supported executable has the SHA-256
`a1d8a56ce1d6300bff704a43218f1b8b87c80c1e1342929a266e58af4e5891e9`; `tox2_game` refuses any other.

## Building

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

3. Recompile the executable into C source under `generated/ppu`. The executable is large, so the
   output is split into 256 chunks (the tool default is 16):

   ```powershell
   python tools/ps3tool.py emit <game-dir>/PS3_GAME/USRDIR/EBOOT.elf --chunks 256 --out generated/ppu
   ```

   `emit` reads the committed function database `config/functions.json`. `recover` is needed only
   to regenerate that file (see [Function recovery](docs/function-recovery.md)).

4. Build:

   ```powershell
   pwsh -NoProfile -File tools/build.ps1 -Preset release -GeneratedDir generated/ppu -GameDir <game-dir>
   ```

The build writes `tox2_game` to `build/release/bin/`. The generated code is compiled in many
chunks; if the compiler runs out of memory, lower the parallelism with the
`CMAKE_BUILD_PARALLEL_LEVEL` environment variable.

Every build parameter, the CMake variables and the test builds are described in
[Building](docs/building.md).

## Running the game

```powershell
build/release/bin/tox2_game.exe --game-dir <game-dir>
```

In the headless runs so far the game ended within a few seconds, when it called `abort()` in its
start-up code, and nothing was drawn; [Status](docs/status.md) has the details. Closing the window ends the run. Game data and
saves are written to `build/release/hdd`; `tools/build.ps1 -Clean` deletes them together with the
rest of `build/release`.

Every option of `tox2_game` is described in [`tox2_game` options](docs/runtime-options.md).

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

If keys do nothing, click the window to give it keyboard focus. The input code is unchanged from the
project this one was derived from, and it has not been tried with this game.

### Gamepad

A connected Xbox-family gamepad or DualShock 4 (second model) works in the window together with the
keyboard; it needs the GameInput runtime from the build step above. Quit Steam, or turn off its
PlayStation controller support, before using a DualShock 4. The button mapping, the dead zone, Steam
and the `gameinput unavailable` warning are in [Input](docs/input.md).

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
