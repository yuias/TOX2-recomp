# Pins the atrac_decode test: dal_audiotool decodes one ATRAC3plus entry of the decrypted sound
# archive and compares the result with the values below. The archive is game-derived and is read from
# the sdata cache in the build directory.
set(DAL_ATRAC_DECODE_ARCHIVE "PS3_GAME/USRDIR/Bgm.sdat")  # relative to the sdata cache
set(DAL_ATRAC_DECODE_INDEX 19)  # the title BGM: stereo, no loop

# 2048-sample frames in the entry, and the valid samples per channel: the `fact` sample count, taken
# from the decoder output after cutting the encoder delay (2232 samples) at the start.
set(DAL_ATRAC_DECODE_FRAMES 1366)
set(DAL_ATRAC_DECODE_SAMPLES 2794736)

# SHA-256 of the cut output (interleaved little-endian float32). The decoder is single-threaded and
# the hash was identical over repeated runs, but FFmpeg's float DSP may round differently between
# builds or CPU feature sets; re-pin then (the counts stay).
set(DAL_ATRAC_DECODE_PCM_SHA256 "b1279257925d9804c2058529afdae2c0d8bb79bc2209019643ca9edb3422f385")
