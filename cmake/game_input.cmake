# Pins how far the game gets when circle is tapped from tools/pad_scripts/leave_title.txt. The
# script skips the notice screen and the start-up movies and confirms NEW GAME; the main thread then
# opens the event script (Script/Event.sdat), about 20 s into a headless run. A run without input
# never gets there (it is still in the movies at that time), so the line shows that the pad input
# reaches the game. It is a main-thread line (thread id 01000000) like the game_boot milestone;
# the later world.pam movie start and the trophy unlock come from other threads, whose first-call
# order varies. The run is stopped at S, after the milestone and before the 83 s world.pam movie
# (which circle cannot skip) ends.
set(DAL_GAME_INPUT_SECONDS 40)
set(DAL_GAME_INPUT_MILESTONE "\\[t 01000000\\] dal: fs: sdata-open \"/dev_bdvd/PS3_GAME/USRDIR/Script/Event\\.sdat\" ok")
