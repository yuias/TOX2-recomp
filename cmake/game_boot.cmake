# The milestone is the main thread's first call of cellSpursEventFlagTryWait, the last `tox2: first ...`
# line of the main thread. The game reaches it only after the first LZMA batch of the SPURS engine tasks
# has run on the recompiled SPU code, so a run that stops earlier (an SPU stop, a stall, a watchdog before
# the batch ends) leaves the pattern unmatched. It first appears at 12 to 14 s with game data installed in
# the hdd directory and at 14 to 16 s from an empty one; both must reach it. The run is limited to 30 s
# because a gap in a PPU jump table stops the game at about 53 s, so a longer run would end differently.
# Update it when the game gets further.
set(TOX2_GAME_BOOT_ARGS "--no-config --no-window --seconds 30 --hdd-dir \"${CMAKE_BINARY_DIR}/hdd\"")
set(TOX2_GAME_BOOT_MILESTONE "\\[t 01000000\\] tox2: first hle cellSpurs::cellSpursEventFlagTryWait")
