# Pins the capture the rsx_replay test replays. The capture is game-derived and git-ignored; it is
# taken with tox2_game --rsx-capture and copied to generated/rsx/captures/<name>.tox2cap.
set(TOX2_RSX_REPLAY_CAPTURE title)

# The accepted reference image for the capture: the SHA-256 of generated/rsx/reference/<name>.png and
# the largest per-channel and mean differences (and the share of differing pixels) a replay may show
# against it. The test compares the replay with the reference and fails above a threshold; an empty
# threshold is not checked. While the SHA-256 is empty, the test only checks the summary line.
set(TOX2_RSX_REPLAY_REFERENCE_SHA256 "")
set(TOX2_RSX_REPLAY_MAX_DIFF "8")
set(TOX2_RSX_REPLAY_MEAN_DIFF "")
set(TOX2_RSX_REPLAY_DIFFERING_PERCENT "0.5")

# The capture the rsx_replay_repeat test replays with --repeat. It only has to replay cleanly: the test checks the
# cycles line, not the image.
set(TOX2_RSX_REPLAY_REPEAT_CAPTURE frame_9400_room)
