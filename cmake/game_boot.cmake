# Both milestones are main-thread lines (thread id 01000000). The first applies when the .sdat files
# are decrypted into a cache (the default); the second when they are not (no keys), where the
# game stops at its own "data missing" dialog.
set(DAL_GAME_BOOT_ARGS "--no-window --seconds 20 --hdd-dir \"${CMAKE_BINARY_DIR}/game_hdd\"")
set(DAL_GAME_BOOT_MILESTONE "\\[t 01000000\\] dal: first hle sceNpTrophy::sceNpTrophyGetTrophyUnlockState")
set(DAL_GAME_BOOT_MILESTONE_NO_SDATA "\\[t 01000000\\] dal: first hle cellSysutil::cellMsgDialogOpen2")
