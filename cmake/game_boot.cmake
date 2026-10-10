# The milestone is a main-thread line (thread id 01000000): the last `tox2: first ...` line that
# appears in every run, from an empty HDD directory and from one with existing game data. The main
# thread finishes the game-data check (installing the 3 GB archive on another thread on the first
# run), waits on an event flag, and the process stops at a guest fault on the GCM interrupt thread.
# Update it when a change lets the main thread get further.
set(TOX2_GAME_BOOT_ARGS "--no-window --seconds 20 --hdd-dir \"${CMAKE_BINARY_DIR}/game_hdd\"")
set(TOX2_GAME_BOOT_MILESTONE "\\[t 01000000\\] tox2: first sc 85 sys_event_flag_wait")
