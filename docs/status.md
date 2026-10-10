# Status

What the runtime does on the pinned executable today, and what it does not implement. The
[top-level README](../README.md) has the short version for players.

## Components

- Analysis tools for the PPU executable: ELF summary, SPU probe, disassembler, Ghidra cross-check,
  NID and syscall name tables, function recovery (see [Analysis tools](analysis-tools.md) and
  [Function recovery](function-recovery.md)).
- An emitter that turns the recovered functions into C source (see [Emitting C](emitter.md)).
- `dal_sdat`, which decrypts the game's `.sdat` files (see [SDAT files](sdat.md)).
- `dal_game`, which loads the executable, runs it through the generated code on real guest threads,
  serves the lv2 system calls and HLE modules the game uses, and runs a fake RSX that consumes the
  game's command buffer (see [Runtime internals](runtime.md)).
- A Vulkan 1.3 renderer that draws what the RSX commands describe into a window or PNG files, and
  `dal_rsxreplay`, which renders single recorded frames without the game (see
  [Rendering](rendering.md)).
- Movie playback through FFmpeg (see [Movies](movies.md)) and sound through the game's own mixer
  and WASAPI (see [Audio](audio.md)).

## How far a run gets

With the `.sdat` cache, the run loads, initializes thread-local storage and starts the game's
helper threads. It initializes GCM and the fake RSX, completes the game-data handshake, loads its
assets (the decrypted `.sdat` files and the texture files its decode threads open), reads the pad,
registers the trophy context, runs the save-data load (a first boot finds no data, which the game
accepts), and drives its frame loop with the vblank handler running at 60 Hz. After the warning
screen it plays its three intro movies (`if_logo.pam`, `ch_logo.pam`, `op.pam`, about 7 s, 7 s and
95 s) at their 29.97 fps rate, then shows the title screen.

With the keyboard, a gamepad or a pad script it leaves the title screen: Circle skips the notice screen and the
movies and confirms NEW GAME, after which the game loads its event script and plays `world.pam`
(about 83 s, not skippable). The first story scenes follow: event pictures and the message window
with the speaker's name and the dialogue text are drawn. The frames were checked for about half a
minute into the story and the in-game sound for about five minutes; later scenes, choices and saving
were not tried.
The game's sound runs through its own mixer: the movie audio, the music and the sound effects reach
the speakers when the window is shown and appear in the `--audio-dump` file in every run. Voice, which the
game streams through `cellAtrac`, is covered by unit tests only: no scene a standard run reaches has
voice. With `--render vulkan` the only entry in the `rsx unimplemented` census is the constant ZPASS
report value.

Without the `.sdat` cache the game stops earlier, at its error dialog for the missing file; without
the FFmpeg DLLs the movies run their full length as black frames and the game's music and sound
effects are silent.

## Not implemented

- Graphics beyond the first story scenes: only what the scenes reached so far use is implemented
  (for example, no swizzled, mipmapped or cube textures). Whatever the renderer meets and cannot
  handle is listed in the `rsx unimplemented` census (see
  [Rendering](rendering.md#rendering)). Known gaps are the ZPASS report values (the constant 0 is
  written, no occlusion query runs) and user clip planes (the vertex translator does not write the
  clip distances).
- Audio: WASAPI is the only device output, in shared mode on the default device. The output is
  stereo (the game's 8-channel port is reduced to its front pair), and there is no volume setting or
  device choice. ATRAC3 (non-plus) is not supported; the game has none. Only the `cellAudio` and
  `cellAtrac` calls this game makes are implemented.
- Movies: only the `cellSail` calls this game makes are implemented (PAMF files with H.264 video and
  ATRAC3plus audio). Repeat mode is stored but not acted on.
- Input: the `neutral`, `none`, `keyboard`, `gamepad` and `keyboard+gamepad` pad providers. The
  keyboard has a fixed mapping, no analog sticks and no vibration; gamepads are read through
  GameInput with a fixed mapping; an Xbox Wireless Controller and a DualShock 4 (second model, no
  vibration) were verified, and the first DualShock 4 model is not used. Only port 0 exists; there is no mouse or `cellKb` input. See [Input](input.md).
- SPU and SPURS: SPU system calls only keep bookkeeping, no SPU code runs, SPURS workloads are not
  scheduled.
- Every import of this game has a handler, but some are minimal (for example `cellSpurs`, which only
  keeps bookkeeping); the census of a run lists what the game called and what the default handler
  served.
- Priority scheduling beyond the creation rule (a new thread with a numerically lower priority than
  its creator runs first), lv2 interrupt threads, savestates.
