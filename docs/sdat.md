# SDAT files

The game reads several `.sdat` files (encrypted data containers) through `cellFsSdataOpen`. The
tool `dal_sdat` decrypts them ahead of time, and `dal_game` serves the plain copies from a cache
directory (`--sdata-cache`, by default `build/<preset>/sdata_cache`).

## Keys

Decryption needs four AES constants (`SDAT_KEY`, `EDAT_KEY_0`, `EDAT_KEY_1`, `EDAT_IV`). They are
not stored in this repository. `dal_sdat --keys` reads them from RPCS3's `rpcs3/Crypto/key_vault.h`,
a source file it only parses; no RPCS3 code is compiled or linked.

When the build is configured with a game directory (`-GameDir`, CMake variable `DAL_GAME_DIR`),
configuration downloads `key_vault.h` from a pinned RPCS3 commit into
`generated/deps/rpcs3/<commit>/` (git-ignored) and checks its SHA-256; the pin is in
`cmake/sdat_keys.cmake`. A copy that is already there and matches is not downloaded again. To build
without the download, set the CMake cache variable `DAL_RPCS3_CRYPTO_DIR` to a local `key_vault.h`
or the directory that holds it (`cmake -S . -B build/<preset> -DDAL_RPCS3_CRYPTO_DIR=<dir>`). If
the download fails, configuration warns and the build skips the cache; the game then stops at its
own error dialog for the missing file.

## Build step

With the keys and a game directory, the build target `sdata_cache` runs

```sh
dal_sdat --keys <key_vault.h> --game-dir <game-dir> --out build/<preset>/sdata_cache
```

and the `game_boot`, `game_input`, `game_audio` and `atrac_decode` tests use that cache. The step
runs again only when `dal_sdat` changes; delete the directory to force it.

## `dal_sdat`

```sh
dal_sdat --keys <key_vault.h or its directory> --game-dir <game-dir> --out <cache-dir>
```

- `--game-dir` is searched recursively for `*.sdat`; each file is written under `--out` at its
  path relative to the game directory. The tool verifies every block's AES-CMAC, so a wrong key
  or a damaged file is an error (exit 1). It uses Windows CNG for AES and is built together with
  the runtime (its tests are the CTest entry `sdat_tests`, which use made-up keys).
- The container layout and the block compression follow RPCS3's `Crypto/unedat.cpp` and
  `Crypto/lz.cpp` (behaviour only; the code is independent).
- When a file is missing from the cache, `cellFsSdataOpen` fails and the game shows its own error
  dialog (auto-answered, see [Headless policy](runtime.md#headless-policy)).
- A `files` entry of the VFS configuration for the sdata path (for example
  `/dev_bdvd/PS3_GAME/USRDIR/Exi_DAL.sdat`) serves that host file instead of the cache copy.
