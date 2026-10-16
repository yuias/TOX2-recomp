# Building

The [top-level README](../README.md) shows the build steps. This page lists
every parameter of the build script, the CMake variables behind them, and the commands for the
test runs.

## `tools/build.ps1`

```powershell
pwsh -NoProfile -File tools/build.ps1 [-Preset debug|release] [-Clean] [-Test]
     [-GeneratedDir <dir>] [-GameDir <dir>] [-EbootElf <path>] [-LlvmBin <dir>]
     [-EbootBin <path>] [-Pup <path>] [-Rpcs3CryptoDir <dir>]
```

The script enters the Visual Studio developer shell itself unless it is already running in one,
then configures, builds, and optionally runs CTest.

| Parameter | Meaning |
| --- | --- |
| `-Preset debug\|release` | The CMake preset (default `debug`). Build output goes to `build/<preset>/`, executables to `build/<preset>/bin/`. |
| `-Clean` | Delete `build/<preset>/` before configuring. This also deletes the default hdd directory `build/<preset>/hdd` (game data and saves). |
| `-Test` | Run CTest after the build. Which tests run depends on the other parameters; see [Tests](tests.md). |
| `-GeneratedDir <dir>` | The directory written by `ps3tool.py emit --out <dir>`. Compiles it into `tox2_ppu_generated` and `tox2_spu_generated` and builds `tox2_game` and the link tests. This game's executable is large, so it is emitted with `--chunks 256` and a full build compiles 256 generated C files; if the compiler runs out of memory, lower the parallelism with the `CMAKE_BUILD_PARALLEL_LEVEL` environment variable. Without it only the runtime libraries, tools and unit tests are built. The directory must hold the executable's `sources.cmake`, the libsre tree `<dir>/libsre/libsre_sources.cmake` (the second `emit` command in [Emitting C](emitter.md#a-prx-module-libsre)) and the SPU tree `<dir>/spu/spu_sources.cmake` (the `spu-emit` command in [Emitting C](emitter.md#spu-images)); if one is missing, configuration fails and prints the command to run. CMake variable `TOX2_PPU_GENERATED_DIR`. |
| `-GameDir <dir>` | The game directory (the one that contains `PS3_GAME`). The `game_boot` test runs against it (it also needs `-EbootElf` or `-EbootBin`). With it, configuration also writes the starter `build/<preset>/bin/config.json` (see [The starter `config.json`](#the-starter-configjson)). CMake variable `TOX2_GAME_DIR`. |
| `-EbootElf <path>` | The decrypted executable for the Python tests that read the real ELF and for the game tests. Not needed to build. When `-EbootBin` is given and this is not, it defaults to `generated/inputs/EBOOT.elf`, the file that `-EbootBin` produces. CMake variable `TOX2_EBOOT_ELF`. |
| `-EbootBin <path>` | The disc's `PS3_GAME/USRDIR/EBOOT.BIN`. The build decrypts it into `generated/inputs/EBOOT.elf`; see [Decrypting the inputs](#decrypting-the-inputs). The file must exist. CMake variable `TOX2_EBOOT_BIN`. |
| `-Pup <path>` | The firmware 4.93 `PS3UPDAT.PUP`. The build extracts `libsre.prx` from it into `generated/inputs/libsre.prx` and relocates it into `generated/inputs/libsre.reloc.elf`. It is required to play: `tox2_game` loads `libsre.reloc.elf`, the libsre emit step reads it, and `game_boot` is registered only with it. The file must exist. CMake variable `TOX2_PUP`. |
| `-Rpcs3CryptoDir <dir>` | The `rpcs3/Crypto` directory of a local RPCS3 checkout, used for the keys instead of the download. The directory must exist. CMake variable `TOX2_RPCS3_CRYPTO_DIR`. |
| `-LlvmBin <dir>` | Put this LLVM `bin` directory first on `PATH`. Defaults to the `TOX2_LLVM_BIN` environment variable. Use it when the clang bundled with Visual Studio is too old or clang is not on `PATH`. |

Relative paths are resolved against the current directory. The parameters set CMake cache
variables, so a later build of the same preset keeps them until they are given again.

## CMake variables

These can also be passed to `cmake` directly (`cmake --preset release -D<name>=<value>`):

| Variable | Default | Meaning |
| --- | --- | --- |
| `TOX2_PPU_GENERATED_DIR` | empty | Set by `-GeneratedDir`. |
| `TOX2_GAME_DIR` | empty | Set by `-GameDir`. |
| `TOX2_EBOOT_ELF` | empty | Set by `-EbootElf`. While it is empty and `TOX2_EBOOT_BIN` is set, configuration sets it to `generated/inputs/EBOOT.elf`. |
| `TOX2_EBOOT_BIN` | empty | Set by `-EbootBin`. |
| `TOX2_PUP` | empty | Set by `-Pup`. |
| `TOX2_RPCS3_CRYPTO_DIR` | empty | Set by `-Rpcs3CryptoDir`. |
| `TOX2_FFMPEG_DIR` | empty | An extracted FFmpeg build to use instead of `generated/deps/ffmpeg/<pinned build>` (see [Movies](movies.md); FFmpeg is used for ATRAC3plus decoding and by the inherited movie code). |
| `TOX2_GAMEINPUT_DIR` | empty | An extracted `Microsoft.GameInput` package to use instead of `generated/deps/gameinput/<pinned version>` (see [Input](input.md#the-gameinput-package)). Configure needs the package from `tools/fetch-gameinput.ps1`. |
| `TOX2_ENABLE_DLSS` | `AUTO` | `AUTO`, `ON` or `OFF`: whether to build the DLAA anti-aliasing mode on NVIDIA Streamline (see [NVIDIA Streamline (DLAA)](#nvidia-streamline-dlaa)). |
| `TOX2_STREAMLINE_DIR` | `externals/Streamline` | The unpacked Streamline SDK to build against. |
| `TOX2_PPU_WERROR` | `ON` | Add `-Werror` for generated code and the code that mirrors it (header check, semantics harness, link test). Every such target is compiled with `-O1 -march=x86-64-v3 -ffp-contract=off -fpatchable-function-entry=8 -Wall -Wextra`, whatever the preset's optimization level; the generated SPU tree and the SPU tests that run generated code (`spu_semantics`, `spu_programs`) use `-O3` instead of `-O1` (`TOX2_SPU_CODEGEN_OPT_LEVEL` in `cmake/SpuCodegen.cmake`), because the game's SPU decode loops run several times faster at that level. |

No tracked file contains a game path: they are command-line arguments, CMake cache variables, a
user-local VFS configuration (`config/vfs.example.json` shows the format with placeholders) or the
git-ignored `build/<preset>/bin/config.json` below.

## The starter `config.json`

When `TOX2_GAME_DIR` is set (`-GameDir`) and `tox2_game` is built (`-GeneratedDir`), configuration
writes `build/<preset>/bin/config.json`, the file `tox2_game` reads when it is started without
`--config` or `--no-config` (see [`tox2_game` options](runtime-options.md)). The executable can then
be started from any working directory without arguments. The file holds:

| Key | Value | Written when |
| --- | --- | --- |
| `game_dir` | `TOX2_GAME_DIR` | always (nothing is written without it) |
| `elf` | `TOX2_EBOOT_ELF`: the file `-EbootElf` names, or `generated/inputs/EBOOT.elf` from `-EbootBin` | the variable is set |
| `libsre` | `generated/inputs/libsre.reloc.elf` | `-Pup` is given |

`hdd_dir` is not written: the default `build/<preset>/hdd` is the directory the README describes. A
path inside the repository or the build directory is stored relative to `bin/`, any other path is
absolute (the file is git-ignored and machine-local).

The file is written at configuration, which also covers `cmake --preset <preset>` without the script.
An existing file is never changed, so hand edits and settings the game saved survive; if its
`game_dir`, `elf` or `libsre` differ from the build's, configuration prints one line naming them. To
get a new starter file, delete the old one and configure again. The `game_boot` test passes
`--no-config`, so the file does not affect the tests.

## Decrypting the inputs

`tox2_decrypt` turns two files you own into the decrypted inputs of this project, with no RPCS3
install. It is configured by `-EbootBin` and `-Pup`. With neither, configuration prints
`TOX2: input decryption not configured`, uses no network, and the build contains only the tool and
its unit tests.

```powershell
pwsh -NoProfile -File tools/build.ps1 -Preset release -EbootBin <game-dir>/PS3_GAME/USRDIR/EBOOT.BIN -Pup <path>/PS3UPDAT.PUP
```

The build target `tox2_inputs` runs the tool and writes the outputs to `generated/inputs/` (shared by
both presets, never into the game directory):

| Output | From | Written when |
| --- | --- | --- |
| `generated/inputs/EBOOT.elf` | The decrypted `EBOOT.BIN` | `-EbootBin` is given |
| `generated/inputs/libsre.prx` | `dev_flash/sys/external/libsre.sprx`, extracted from the PUP and decrypted | `-Pup` is given |
| `generated/inputs/libsre.reloc.elf` | `libsre.prx` placed at the guest address `0x011d0000` with its relocations applied (`tox2_decrypt reloc`). `emit` and `recover` read it, and `tox2_game` loads it | `-Pup` is given |

Decryption runs at build time, not at configure time, and again only when an input file or the tool
has changed. The produced `EBOOT.elf` is the default of `TOX2_EBOOT_ELF`, so `emit`, the Python
real-executable tests and `game_boot` use it. The usual order is: build once with the two files, run
the three emit commands (`ps3tool.py emit generated/inputs/EBOOT.elf --chunks 256 --out generated/ppu`,
`ps3tool.py emit generated/inputs/libsre.reloc.elf --db config/libsre_functions.json --symbol-prefix libsre --chunks 4 --out generated/ppu/libsre --max-unhandled 0`
and `ps3tool.py spu-emit --eboot generated/inputs/EBOOT.elf --libsre generated/inputs/libsre.reloc.elf --out generated/ppu/spu --max-unhandled 0`),
then build again with `-GeneratedDir generated/ppu -GameDir <game-dir>`; the cache keeps the earlier
parameters. The SPU function databases (`config/spu/`) are committed, so `spu-emit` needs no recovery run.

### Keys

No key is stored in this repository. With `-EbootBin` or `-Pup`, configuration downloads
`key_vault.h` and `key_vault.cpp` from RPCS3 at commit `f9b8c209a3800c1865bcfeade611f65f5237138c`
(`rpcs3/Crypto` in <https://github.com/RPCS3/rpcs3>) into `generated/deps/rpcs3/<commit>/`. Each
download is checked against a SHA-256 pinned in `cmake/rpcs3_keys.cmake`:

| File | SHA-256 |
| --- | --- |
| `key_vault.h` | `d5d0cede74c7900bb23ca8d60066194648ed4b976eaee60315c0bcbed7dc2aaf` |
| `key_vault.cpp` | `a2947ff6b681258c198552ec6344e16685e2df649018206daa3f41a0a1aadbd7` |

`tox2_decrypt` reads only the key constants from these files, as data. If the download fails,
configuration stops with an error that names `TOX2_RPCS3_CRYPTO_DIR` and `-Rpcs3CryptoDir`. To build
offline, point `-Rpcs3CryptoDir` (`TOX2_RPCS3_CRYPTO_DIR`) at the `rpcs3/Crypto` directory of a local
RPCS3 checkout; it must hold both files. Their hashes are not enforced: configuration prints a line
when one differs from the pin, and the output pins below catch a real problem.

### What is checked

- `EBOOT.BIN` must have the SHA-256 in the `eboot_bin` entry of `config/target.json`. A game-update
  `EBOOT.BIN` or another title is refused before decryption.
- For the PUP: the file layout, the HMAC of the two entries the tool reads (the version string and
  the update packages), and the firmware version, which must equal the `firmware` of the
  `libsre_prx` entry (`4.93`). A damaged or truncated download fails the HMAC check. A PUP of another
  firmware version is refused by the pin; Sony publishes only the latest firmware, so a 4.93 PUP must
  come from an earlier download. The tested file is 206197916 bytes with SHA-256
  `158471fd834f8ea8036136b6aab43cd86c7ba73d79ca30e0af3c0fe0001cf365`; the hash of the PUP itself is
  not enforced.
- For `libsre.reloc.elf`: the input `libsre.prx` must match the `libsre_prx` pin, and the base
  address (`0x011d0000`, a multiple of 64 KiB) comes from the `libsre_reloc_elf` entry. Every
  relocation record must apply to bytes that equal the value for the unrelocated, pre-linked file,
  and an unknown relocation type is an error.
- The output: its size and SHA-256 must match the `eboot_elf`, `libsre_prx` or `libsre_reloc_elf` entry
  of `config/target.json`. On a mismatch the tool prints both values and writes nothing, and an existing
  output file stays as it was. A match is written as `<name>.part` and then renamed.

NPDRM SELFs, debug SELFs and 32-bit (SPU) SELFs are refused with an error.

### The tool

`tox2_decrypt` has four commands. The build runs `self`, `pup` and `reloc` with the pins read from
`config/target.json`; running `tox2_decrypt` without arguments prints the usage text and exits with 2.

```text
tox2_decrypt self --keys DIR --in EBOOT.BIN --out EBOOT.elf --expect-size N --expect-sha256 HEX
                  [--expect-input-sha256 HEX]
tox2_decrypt pup  --keys DIR --in PS3UPDAT.PUP --path PATH --firmware VERSION --out libsre.prx
                  --expect-size N --expect-sha256 HEX
tox2_decrypt reloc --in libsre.prx --base 0xADDR --out libsre.reloc.elf --expect-size N
                  --expect-sha256 HEX [--expect-input-sha256 HEX]
tox2_decrypt keys --keys DIR
```

`--keys` is the directory with `key_vault.h` and `key_vault.cpp`. `pup` finds the file `PATH` (the
build uses `dev_flash/sys/external/libsre.sprx`) in the `dev_flash_*` packages of the PUP. `keys`
prints how many keys of each type were read, never the keys. `reloc` needs no keys: it reads the
unrelocated `libsre.prx`, applies the relocations for the base `--base` (`0x`-prefixed hexadecimal, a
multiple of `0x10000`; the build passes the pinned `0x011d0000`) and prints
`relocated: <n> relocations (ADDR32 <a>, ADDR16_LO <b>, ADDR16_HI <c>, ADDR16_HA <d>), image <begin>..<end>`
before the pin check. A successful `self`, `pup` or `reloc` prints
`<name>: <size> bytes, sha256 <hex>: matches the pin`. Every error is one line that starts with
`error: `, and no key bytes appear in any output. The exit code is 0 on success, 1 on a failure and 2
on a usage error. The tests `decrypt_core`, `decrypt_eboot`, `decrypt_libsre` and `decrypt_libsre_reloc` are listed in
[Tests](tests.md).

## What gets built

The runtime core (`tox2_runtime_core`: `runtime/src/core`, `lv2`, `hle`, `input`, `rsx`, `audio`
without the WASAPI output, and the PAMF demuxer, movie player and ATRAC3plus file parser in `runtime/src/media`, the first two unused by this game)
does not reference generated symbols and is always built, together with its unit tests. The renderer
(`tox2_render`, `runtime/src/render`) is a separate library on top of it, so the core itself needs no
GPU. `tox2_game` is built only with `-GeneratedDir`, because only `runtime/src/game_main.cpp` passes
the generated tables to the core. It checks the executable against the SHA-256 that the generated
code was emitted from.

`tox2_decrypt` (with its library and the unit tests `tox2_decrypt_tests`) is always built. `tox2_inputs`,
the target that writes `generated/inputs`, exists only with `-EbootBin` or `-Pup`. The generated code
of the executable and of libsre are compiled together into `tox2_ppu_generated`, and the generated SPU code
of the game and of libsre into `tox2_spu_generated`. The stub runtimes `tox2_ppu_stub` and `tox2_spu_stub`
supply the hooks for the link checks and the header and semantics tests; `tox2_game` links the runtime core instead.

## NVIDIA Streamline (DLAA)

The `dlaa` anti-aliasing mode (`--aa-mode dlaa`, see [DLAA](rendering.md#dlaa)) runs NVIDIA
DLSS in its DLAA mode through the NVIDIA Streamline SDK. The SDK is optional and is not part of the
repository. Without it the build succeeds, DLAA is compiled out and a requested `dlaa` falls back to `msaa`.

```powershell
pwsh -NoProfile -File tools/fetch-streamline.ps1 [-From <zip>] [-Force]
```

The script downloads the pinned release, `v2.14.1` (the version, file name and SHA-256 are in
`cmake/streamline.cmake`), from <https://github.com/NVIDIA-RTX/Streamline/releases>, checks its SHA-256
and extracts it into `externals/Streamline`, which is git-ignored. It writes
`externals/Streamline/.verified-sha256` and prints `streamline: up to date` and does nothing when that marker
already matches the pin. `-From <zip>` uses a local copy of the release zip instead of downloading it, with the
same check, and `-Force` extracts again over a tree that is up to date. A tree without a matching marker, for
example one unpacked by hand, is not replaced unless `-Force` or `-From` is given. The exit code is 0 on success, 1 on a failure and 2 on a
usage error. It needs PowerShell 7.

Configuration looks for the SDK in `TOX2_STREAMLINE_DIR` and prints one status line:

| `TOX2_ENABLE_DLSS` | Result |
| --- | --- |
| `AUTO` (default) | With `include/sl.h` and the DLLs below present: `TOX2: Streamline: <dir> (v2.14.1)`, plus `, not verified by tools/fetch-streamline.ps1` when the marker is missing or differs from the pin. Otherwise `TOX2: Streamline: not found (<dir>/include/sl.h); DLAA disabled. Run: pwsh -NoProfile -File tools/fetch-streamline.ps1`, and the build goes on without it. |
| `ON` | As `AUTO`, but a missing SDK is a configuration error with the same hint. |
| `OFF` | `TOX2: Streamline: off (TOX2_ENABLE_DLSS=OFF)`; the SDK is not used even when present. |

With the SDK, the renderer is built with its headers (`TOX2_HAS_STREAMLINE=1`). Nothing is linked: the
SDK's import library is not used, because it exports the Vulkan entry points that the renderer's function
loader already defines. `sl.interposer.dll` is loaded at run time instead (see
[DLAA](rendering.md#dlaa)). The build copies the DLLs next to the executables, in
`bin/` of the build directory, by the target `tox2_streamline_runtime` (a dependency of `tox2_game`,
`tox2_rsxreplay` and the render tests):

| File | Source in the SDK |
| --- | --- |
| `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `nvngx_dlss.dll` | `bin/x64/` (the production DLLs, not `bin/x64/development`) |
| `licenses/streamline/LICENSE.txt` | `license.txt` |
| `licenses/streamline/nvngx_dlss.license.txt` | `bin/x64/` |
| `licenses/streamline/3rd-party-licenses.md` | `3rd-party-licenses.md` |

`NvLowLatencyVk.dll` (Reflex), which `sl.common.dll` tries to load at start-up, is not copied: its licence
does not list it as distributable and DLAA does not use it; the load failure is logged at debug level.
Streamline's own signature check still prints `File '...NvLowLatencyVk.dll' is NOT correctly signed` to
standard output, which only means the file is absent. The licences of these files, which must accompany
a redistributed copy, are listed in [Third-party notices](../THIRD_PARTY_NOTICES.md). At run time DLAA is
off, with one log line that gives the reason, when the DLLs are missing or not signed by NVIDIA, or the GPU
is not an RTX GPU that supports DLSS, or the driver is too old.

The DLAA tests print a skip line instead of failing when DLAA is unavailable. To check the build without the
SDK, configure a second build directory with `-DTOX2_ENABLE_DLSS=OFF`.

## Test builds

Unit tests only (no game files needed):

```powershell
pwsh -NoProfile -File tools/build.ps1 -Preset debug -Clean -Test
```

Unit tests plus the Python tests that read the real executable:

```powershell
pwsh -NoProfile -File tools/build.ps1 -Preset release -EbootBin <game-dir>/PS3_GAME/USRDIR/EBOOT.BIN -Pup <path>/PS3UPDAT.PUP -Test
```

This also runs `decrypt_eboot`, `decrypt_libsre` and `decrypt_libsre_reloc`, and the Python tests that
read the real `libsre.reloc.elf`. With an `EBOOT.elf` that you already have,
`-EbootElf <path-to-EBOOT.elf>` replaces `-EbootBin` and `-Pup`.

These tests expect the values of the pinned TOX2 executable (see [Tests](tests.md)).

Generated code and the game run (`game_boot`, which needs `-Pup` because the run loads libsre; the
earlier `-Pup` stays in the cache):

```powershell
pwsh -NoProfile -File tools/build.ps1 -Preset release -GeneratedDir generated/ppu -EbootBin <game-dir>/PS3_GAME/USRDIR/EBOOT.BIN -Pup <path>/PS3UPDAT.PUP -GameDir <game-dir> -Test
```

The full list of tests and the conditions under which each one is registered are in
[Tests](tests.md).

## Optional tools

- Ghidra 12.x and a JDK 21, for `ps3tool.py ghidra-export` only (see
  [Analysis tools](analysis-tools.md#ghidra-cross-check)). `recover` does not need Ghidra: it reads
  the committed Ghidra summary, or runs with `--no-ghidra`.
- git and a sparse checkout of the RPCS3 sources, for `ps3tool.py nidgen` only (see
  [Analysis tools](analysis-tools.md#nid-and-syscall-name-tables)).
- WSL with a distribution that has a C toolchain (Ubuntu-26.04 is the verified one), for
  `ps3tool.py spu-objdump-diff` only. The `objdump` it runs is built by `tools/build-spu-objdump.sh` (see
  [Analysis tools](analysis-tools.md#spu-decoder-cross-check-with-objdump)); no other command and no build
  needs it.
