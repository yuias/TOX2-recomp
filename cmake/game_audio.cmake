# Pins what the audio path must show in a headless run. The run is the one of game_input (the pad
# script leaves the title screen and confirms NEW GAME), with the null output and a dump file. The
# census line of the cellAtrac module has to report at least one decoded frame with audible content:
# the title BGM and the confirm sound effect are both ATRAC3plus. The pattern is only the leading
# part of the line, so counters that vary from run to run do not matter.
set(DAL_GAME_AUDIO_SECONDS 40)
set(DAL_GAME_AUDIO_ATRAC_PATTERN "atrac [0-9]+ decoders, [0-9]+ frames decoded \\([1-9][0-9]* non-silent\\)")
