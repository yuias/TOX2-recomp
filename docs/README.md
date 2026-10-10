# Development documentation

How to build and play the game is in the [top-level README](../README.md). These files list every
parameter and describe how the project works:

| File | Contents |
| --- | --- |
| [building.md](building.md) | Every `tools/build.ps1` parameter, the CMake variables and the commands for test builds |
| [runtime-options.md](runtime-options.md) | Every `dal_game` option, exit codes and stop lines, and example runs by purpose |
| [status.md](status.md) | How far a run gets and what is not implemented |
| [analysis-tools.md](analysis-tools.md) | ELF summary, SPU probe, disassembler, Ghidra cross-check, NID and syscall name tables, target pin |
| [function-recovery.md](function-recovery.md) | Recovering function starts and extents, the function database and its report |
| [emitter.md](emitter.md) | Turning the recovered functions into C, the contract header and the semantics harness |
| [runtime.md](runtime.md) | Runtime internals: logging, census, memory map, dispatch, threads, system calls, HLE imports, headless policy, PNG decoding, default handlers |
| [virtual-file-system.md](virtual-file-system.md) | Guest path mapping and the VFS configuration file |
| [sdat.md](sdat.md) | Decrypting the game's `.sdat` files: where the keys come from, the build step and `dal_sdat` |
| [rendering.md](rendering.md) | Fake RSX, the Vulkan renderer, and capture and replay |
| [movies.md](movies.md) | cellSail HLE, PAMF demuxing, FFmpeg decoding and `dal_moviedump` |
| [audio.md](audio.md) | `cellAudio` and `cellAtrac`, the audio engine and block clock, the WASAPI output, options, log lines and `dal_audiotool` |
| [input.md](input.md) | Gamepads: supported pads, mapping, the GameInput runtime, Steam, the gamepad thread, device selection, log lines, `--pad-probe` and `dal_padtool` |
| [tests.md](tests.md) | CTest targets, the `game_boot`, `game_input` and `game_audio` tests and the Python unit tests |
| [layout.md](layout.md) | Repository layout |
| [porting-guide.md](porting-guide.md) | Applying the method to another PS3 game |
