# The milestone is a main-thread line (thread id 01000000): the last `tox2: first ...` line that
# appears in every run, from an empty HDD directory and from one with existing game data, before the
# game's own abort() output. The lines after it (sys_tty_write, sys_process_exit) are the abort path,
# which an earlier failure would also reach, so they would not show that the run got this far.
# Update it when a change lets the main thread get further.
set(TOX2_GAME_BOOT_ARGS "--no-window --seconds 20 --hdd-dir \"${CMAKE_BINARY_DIR}/game_hdd\"")
set(TOX2_GAME_BOOT_MILESTONE "\\[t 01000000\\] tox2: first sc 988 ")
