# Pins the capture the rsx_replay test replays. The capture is game-derived and git-ignored; it is
# taken with dal_game --rsx-capture and copied to generated/rsx/captures/<name>.dalcap.
set(DAL_RSX_REPLAY_CAPTURE title)

# The accepted reference image for the capture: the SHA-256 of generated/rsx/reference/<name>.png and
# the largest per-channel and mean differences (and the share of differing pixels) a replay may show
# against it. The test compares the replay with the reference and fails above a threshold; an empty
# threshold is not checked. While the SHA-256 is empty, the test only checks the summary line.
set(DAL_RSX_REPLAY_REFERENCE_SHA256 "")
set(DAL_RSX_REPLAY_MAX_DIFF "8")
set(DAL_RSX_REPLAY_MEAN_DIFF "")
set(DAL_RSX_REPLAY_DIFFERING_PERCENT "0.5")
