# The milestone is the stop line of the first sys_spu_thread_group_start, the point where the game's
# cellSpursInitializeWithAttribute, running as recompiled libsre code, has built SPURS instance A and
# its handler thread starts the kernel group. A `tox2: first ...` line cannot be the milestone any more:
# the handler thread has a higher priority than the main thread, so the run stops inside the main
# thread's call before it reaches later lines. The line names the kernel image that libsre imported and
# its entry point, so a wrong kernel, a failed import or an earlier stop leaves the pattern unmatched.
# The group id and the thread count are not pinned. Run it from an empty HDD directory and from one with
# existing game data; both must reach the same line. Update it when SPU execution moves the stop.
set(TOX2_GAME_BOOT_ARGS "--no-window --seconds 20 --hdd-dir \"${CMAKE_BINARY_DIR}/game_hdd\"")
set(TOX2_GAME_BOOT_MILESTONE
    "tox2: stop: spu: sys_spu_thread_group_start group 0x[0-9a-f]+ \"[^\"]*CellSpursKernelGroup\" \\([0-9]+ threads, image 0x011f0380 entry 0x818\\)")
