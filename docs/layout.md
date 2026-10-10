# Repository layout

## Layout

| Path | Contents |
| --- | --- |
| `CMakeLists.txt`, `CMakePresets.json`, `cmake/` | Build configuration (clang, Ninja, `debug` and `release` presets) |
| `runtime/` | Runtime executable stub (`tox2_runtime`), stub runtime for generated code (`tox2_ppu_stub`), the runtime core and `tox2_game` (`runtime/src/core`, `lv2`, `hle`, `input`, `rsx`, `media`, `audio`, `game_main.cpp`; see [Runtime internals](runtime.md) and [`tox2_game` options](runtime-options.md)) and runtime-side tests |
| `externals/spdlog` | spdlog submodule used by the runtime logger |
| `externals/libpng`, `externals/zlib` | libpng (v1.6.59) and zlib (v1.3.2) submodules, the PNG codec behind `cellPngDec` |
| `externals/yyjson` | yyjson submodule (0.13.0, built as a static library), used only to parse the VFS configuration |
| `externals/Vulkan-Headers`, `externals/volk`, `externals/VulkanMemoryAllocator` | Vulkan headers, the run-time loader and the memory allocator of the renderer |
| `runtime/src/render/` | The renderer (`tox2_render`): Vulkan context, render targets, textures, pipelines, window and swapchain, vertex program and fragment program translators, shader compiler and cache, PNG output; see [Rendering](rendering.md#rendering) |
| `runtime/src/media/` | ATRAC3plus support and the inherited movie code: the AT3 file parser and the ATRAC frame-decoder interface, the PAMF header parser and MPEG-PS demuxer and the host movie player (all in the runtime core), and the FFmpeg loader with the ATRAC3plus and movie decoders (`tox2_media`). The PAMF and movie code is built but unused by this game; see [Movies](movies.md) and [Audio](audio.md#cellatrac) |
| `tools/fetch-ffmpeg.ps1`, `cmake/ffmpeg.cmake`, `cmake/Media.cmake` | FFmpeg fetch script and pin, and the media targets |
| `tools/tox2_rsxreplay/` | `tox2_rsxreplay`, which replays an RSX capture without the game; see [Capture and replay](rendering.md#capture-and-replay) |
| `cmake/Render.cmake`, `cmake/rsx_replay.cmake` | Renderer targets and tests, and the pin file of the `rsx_replay` test |
| `cmake/game_boot.cmake` | Pin file of the `game_boot` test: arguments and milestones |
| `runtime/src/input/` | Pad buttons, the host pad state and provider, the pad script player (no pad scripts are shipped), and the gamepad code (backend interface, GameInput backend `tox2_input_gameinput`, mapping, selection, hub, probe); see [Input](runtime.md#input) and [Gamepads](input.md) |
| `tools/tox2_padtool/` | `tox2_padtool`: GameInput devices, live readings and rumble; see [`tox2_padtool`](input.md#tox2_padtool) |
| `tools/fetch-gameinput.ps1`, `cmake/gameinput.cmake`, `cmake/Input.cmake` | GameInput package fetch script and pin, and the input targets and tests (`input_core`, `input_gameinput`) |
| `runtime/src/audio/` | Audio: the output interface, the null output, the audio engine with its block clock, WAV files (in the runtime core), and the WASAPI output (`tox2_audio_wasapi`); see [Audio](audio.md) |
| `tools/tox2_audiotool/` | `tox2_audiotool`: WAV statistics, device list, tone and WAV playback, ATRAC3plus archive scan and decode; see [`tox2_audiotool`](audio.md#tox2_audiotool) |
| `cmake/Audio.cmake` | Audio targets and tests (`audio_core`, `audio_wasapi`) |
| `tools/build.ps1` | Windows build wrapper |
| `tools/tox2_decrypt/` | `tox2_decrypt`: decrypts `EBOOT.BIN`, extracts `libsre.prx` from a firmware `PS3UPDAT.PUP` and relocates it to `libsre.reloc.elf` (`prx_reloc.*`), with its synthetic unit tests; see [Decrypting the inputs](building.md#decrypting-the-inputs) |
| `cmake/Decrypt.cmake`, `cmake/rpcs3_keys.cmake` | The `tox2_decrypt` targets, the `tox2_inputs` target that writes `generated/inputs` and the tests `decrypt_core`, `decrypt_eboot`, `decrypt_libsre`, `decrypt_libsre_reloc`; and the pin of the two RPCS3 key files and their download |
| `tools/ps3tool.py` | Command-line entry point for the analysis tools |
| `tools/ps3recomp/` | ELF/PRX parsing (module info of a relocated PRX), PPU decoder and formatter, probe, Ghidra export, diff and summary, function recovery (seeds, partition, jump tables, coverage, syscall resolution), name table generator |
| `tools/ghidra/` | Java `GhidraScript`s run by `ghidra-export` |
| `tools/ps3recomp/emit*.py` | The `emit` command: function DB loader and census, per-instruction handlers by group, output writer and report |
| `tools/tests/` | Unit tests |
| `tools/tests/ppu_semantics/` | Semantics harness cases and the C generator; see [Semantics harness](emitter.md#semantics-harness) |
| `runtime/include/tox2/` | Contract header and inline helpers for generated code |
| `config/target.json` | Pinned size and hashes of the target executable (`EBOOT.BIN` and `EBOOT.elf`), of the firmware `libsre.prx` with its firmware version, and of `libsre.reloc.elf` with its base address |
| `config/vfs.example.json` | Example VFS configuration for `tox2_game --vfs-config`, placeholder paths only; see [VFS configuration file](virtual-file-system.md#vfs-configuration-file) |
| `config/nids.json`, `config/lv2_syscalls.json` | FNID and lv2 syscall name tables generated from RPCS3 sources |
| `config/ghidra_summary.json`, `config/extra_seeds.json`, `config/functions.json` | Ghidra summary, extra function starts and the recovered function database. See [Function recovery](function-recovery.md#function-recovery) |
| `config/libsre_functions.json` | The recovered function database of libsre, committed like `config/functions.json`; see [Function recovery](function-recovery.md#on-libsre) |
| `runtime/src/hle/libsre.*` | The libsre layout header (the guest addresses of the relocated image, `kBase` and the others) and the check of a loaded image against it; see [libsre](runtime.md#libsre) |
| `runtime/src/core/elf_loader.*`, `dispatch.*` | The executable and PRX loaders, and the dispatch tables with one span per loaded image |
| `docs/` | Development documentation |
| `LICENSE`, `THIRD_PARTY_NOTICES.md` | Licence of this project (0BSD) and notices of third-party components (FFmpeg, GameInput, the RPCS3 key files) |
| `generated/` | Tool output, including `generated/ppu` from `emit` (git-ignored) |
| `generated/ppu/libsre/` | The C code of libsre from `emit --symbol-prefix libsre`, with every file and symbol prefixed `libsre` (git-ignored); see [A PRX module](emitter.md#a-prx-module-libsre) |
| `generated/inputs/` | The decrypted `EBOOT.elf`, `libsre.prx` and `libsre.reloc.elf` (`libsre.prx` relocated to `0x011d0000`) written by the build with `-EbootBin` and `-Pup` (git-ignored) |
| `generated/deps/rpcs3/` | The downloaded RPCS3 key files `key_vault.h` and `key_vault.cpp`, one directory per commit (git-ignored) |
| `build/` | Build output (git-ignored) |
| `game/` | Suggested place for your own `EBOOT.elf` (git-ignored) |
