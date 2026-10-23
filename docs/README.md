# Development documentation

How to build the project is in the [top-level README](../README.md). These files list every
parameter and describe how the project works:

| File | Contents |
| --- | --- |
| [building.md](building.md) | Every `tools/build.ps1` and `tools/build.sh` parameter, the CMake variables, the macOS toolchain and the commands for test builds |
| [runtime-options.md](runtime-options.md) | Every `tox2_game` option, exit codes and stop lines, and example runs by purpose |
| [status.md](status.md) | How far a run gets and what is not implemented |
| [analysis-tools.md](analysis-tools.md) | ELF summary, SPU probe, disassembler, Ghidra cross-check, NID and syscall name tables, target pin |
| [savedata-tool.md](savedata-tool.md) | `tools/tox2_savetool.py`: decrypting a PS3 save, `info`, Game Genie `patch`, `install` into the hdd tree, keys, exit codes, the known `SAVE` fields |
| [function-recovery.md](function-recovery.md) | Recovering function starts and extents, the function database and its report |
| [emitter.md](emitter.md) | Turning the recovered functions into C, the contract header and the semantics harness |
| [runtime.md](runtime.md) | Runtime internals: logging, census, memory map, dispatch, threads, system calls, HLE imports, headless policy, PNG decoding, default handlers |
| [virtual-file-system.md](virtual-file-system.md) | Guest path mapping and the VFS configuration file |
| [rendering.md](rendering.md) | Fake RSX, the Vulkan renderer (including Vulkan on macOS), and capture and replay |
| [movies.md](movies.md) | The Bink movies of this game (not handled), and the inherited cellSail HLE, PAMF demuxing and FFmpeg movie decoding (built, unused) |
| [audio.md](audio.md) | `cellAudio` and `cellAtrac`, the audio engine and block clock, the WASAPI and CoreAudio outputs, options, log lines and `tox2_audiotool` |
| [spu.md](spu.md) | How the game uses the SPUs: the embedded SPU images, where the SPURS code lives (libsre), SPURS and SPU thread group setup, how the runtime runs SPU code (threads, image tracking, reservations, MFC, channels, lv2 calls, log lines), what a run reaches, open issues, entry points |
| [input.md](input.md) | Gamepads: supported pads, mapping, the GameInput runtime (Windows), gamepads and the keyboard on macOS, Steam, the gamepad thread, device selection, log lines, `--pad-probe` and `tox2_padtool` |
| [tests.md](tests.md) | CTest targets (Windows only and macOS only ones are marked), the `game_boot` test and the Python unit tests |
| [layout.md](layout.md) | Repository layout |
| [porting-guide.md](porting-guide.md) | Applying the method to another PS3 game |
