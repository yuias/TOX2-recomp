# The milestone is a main-thread line (thread id 01000000). The current value is a placeholder that
# matches the first default-handled HLE function or syscall; it is replaced by the last main-thread
# line that appears in every run once the first headless runs are analysed.
set(DAL_GAME_BOOT_ARGS "--no-window --seconds 20 --hdd-dir \"${CMAKE_BINARY_DIR}/game_hdd\"")
set(DAL_GAME_BOOT_MILESTONE "\\[t 01000000\\] dal: first (hle|sc) ")
