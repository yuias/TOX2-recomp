# The milestone is a main-thread line (thread id 01000000): the last `tox2: first ...` line that
# appears in every run, from an empty HDD directory and from one with existing game data. The game
# stats TLFILE.TLDAT there, the stat fails, and the game takes the game-data install path on
# another thread before it stops at a guest trap. Update it when a change lets the main thread
# get further.
set(TOX2_GAME_BOOT_ARGS "--no-window --seconds 20 --hdd-dir \"${CMAKE_BINARY_DIR}/game_hdd\"")
set(TOX2_GAME_BOOT_MILESTONE "\\[t 01000000\\] tox2: first hle sys_fs::cellFsStat")
