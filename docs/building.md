# Building

The [top-level README](../README.md) shows the build steps. This page lists
every parameter of the build script, the CMake variables behind them, and the commands for the
test runs.

## `tools/build.ps1`

```powershell
pwsh -NoProfile -File tools/build.ps1 [-Preset debug|release] [-Clean] [-Test]
     [-GeneratedDir <dir>] [-GameDir <dir>] [-EbootElf <path>] [-LlvmBin <dir>]
```

The script enters the Visual Studio developer shell itself unless it is already running in one,
then configures, builds, and optionally runs CTest.

| Parameter | Meaning |
| --- | --- |
| `-Preset debug\|release` | The CMake preset (default `debug`). Build output goes to `build/<preset>/`, executables to `build/<preset>/bin/`. |
| `-Clean` | Delete `build/<preset>/` before configuring. This also deletes the default hdd directory `build/<preset>/hdd` (game data and saves). |
| `-Test` | Run CTest after the build. Which tests run depends on the other parameters; see [Tests](tests.md). |
| `-GeneratedDir <dir>` | The directory written by `ps3tool.py emit --out <dir>`. Compiles it into `tox2_ppu_generated` and builds `tox2_game` and the link test. This game's executable is large, so it is emitted with `--chunks 256` and a full build compiles 256 generated C files; if the compiler runs out of memory, lower the parallelism with the `CMAKE_BUILD_PARALLEL_LEVEL` environment variable. Without it only the runtime libraries, tools and unit tests are built. If the directory has no `sources.cmake`, configuration fails and prints the `emit` command to run. CMake variable `TOX2_PPU_GENERATED_DIR`. |
| `-GameDir <dir>` | The game directory (the one that contains `PS3_GAME`). The `game_boot` test runs against it (it also needs `-EbootElf`). CMake variable `TOX2_GAME_DIR`. |
| `-EbootElf <path>` | The decrypted executable for the Python tests that read the real ELF and for the game tests. Not needed to build. CMake variable `TOX2_EBOOT_ELF`. |
| `-LlvmBin <dir>` | Put this LLVM `bin` directory first on `PATH`. Defaults to the `TOX2_LLVM_BIN` environment variable. Use it when the clang bundled with Visual Studio is too old or clang is not on `PATH`. |

Relative paths are resolved against the current directory. The parameters set CMake cache
variables, so a later build of the same preset keeps them until they are given again.

## CMake variables

These can also be passed to `cmake` directly (`cmake --preset release -D<name>=<value>`):

| Variable | Default | Meaning |
| --- | --- | --- |
| `TOX2_PPU_GENERATED_DIR` | empty | Set by `-GeneratedDir`. |
| `TOX2_GAME_DIR` | empty | Set by `-GameDir`. |
| `TOX2_EBOOT_ELF` | empty | Set by `-EbootElf`. |
| `TOX2_FFMPEG_DIR` | empty | An extracted FFmpeg build to use instead of `generated/deps/ffmpeg/<pinned build>` (see [Movies](movies.md); FFmpeg is used for ATRAC3plus decoding and by the inherited movie code). |
| `TOX2_GAMEINPUT_DIR` | empty | An extracted `Microsoft.GameInput` package to use instead of `generated/deps/gameinput/<pinned version>` (see [Input](input.md#the-gameinput-package)). Configure needs the package from `tools/fetch-gameinput.ps1`. |
| `TOX2_PPU_WERROR` | `ON` | Add `-Werror` for generated code and the code that mirrors it (header check, semantics harness, link test). Every such target is compiled with `-O1 -march=x86-64-v3 -ffp-contract=off -fpatchable-function-entry=8 -Wall -Wextra`, whatever the preset's optimization level. |

No tracked file contains a game path: they are command-line arguments, CMake cache variables, or a
user-local VFS configuration (`config/vfs.example.json` shows the format with placeholders).

## What gets built

The runtime core (`tox2_runtime_core`: `runtime/src/core`, `lv2`, `hle`, `input`, `rsx`, `audio`
without the WASAPI output, and the PAMF demuxer, movie player and ATRAC3plus file parser in `runtime/src/media`, the first two unused by this game)
does not reference generated symbols and is always built, together with its unit tests. The renderer
(`tox2_render`, `runtime/src/render`) is a separate library on top of it, so the core itself needs no
GPU. `tox2_game` is built only with `-GeneratedDir`, because only `runtime/src/game_main.cpp` passes
the generated tables to the core. It checks the executable against the SHA-256 that the generated
code was emitted from.

## Test builds

Unit tests only (no game files needed):

```powershell
pwsh -NoProfile -File tools/build.ps1 -Preset debug -Clean -Test
```

Unit tests plus the Python tests that read the real executable:

```powershell
pwsh -NoProfile -File tools/build.ps1 -Preset release -EbootElf <path-to-EBOOT.elf> -Test
```

These tests expect the values of the pinned TOX2 executable (see [Tests](tests.md)).

Generated code and the game run (`game_boot`):

```powershell
pwsh -NoProfile -File tools/build.ps1 -Preset release -GeneratedDir generated/ppu -EbootElf <path-to-EBOOT.elf> -GameDir <game-dir> -Test
```

The full list of tests and the conditions under which each one is registered are in
[Tests](tests.md).

## Optional tools

- Ghidra 12.x and a JDK 21, for `ps3tool.py ghidra-export` only (see
  [Analysis tools](analysis-tools.md#ghidra-cross-check)). `recover` does not need Ghidra: it reads
  the committed Ghidra summary, or runs with `--no-ghidra`.
- git and a sparse checkout of the RPCS3 sources, for `ps3tool.py nidgen` only (see
  [Analysis tools](analysis-tools.md#nid-and-syscall-name-tables)).
