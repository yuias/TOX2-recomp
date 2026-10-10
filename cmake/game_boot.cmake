# The milestone is a main-thread line (thread id 01000000): the last `tox2: first ...` line that
# appears in every run, from an empty HDD directory and from one with existing game data. Avoid the
# lines of an abort path (sys_tty_write, sys_process_exit, and the syscall 988 inside the game's
# abort()): an earlier failure would reach them too, so they would not show that the run got this far.
# Update it when a change lets the main thread get further.
set(TOX2_GAME_BOOT_ARGS "--no-window --seconds 20 --hdd-dir \"${CMAKE_BINARY_DIR}/game_hdd\"")
set(TOX2_GAME_BOOT_MILESTONE "\\[t 01000000\\] tox2: first sc 350 ")
