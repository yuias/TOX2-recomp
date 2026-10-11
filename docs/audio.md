# Audio

This page covers the audio code in the runtime: the `cellAudio` and `cellAtrac` HLE, the audio
engine and the WASAPI output. The code was written for and checked against an earlier game, with
synthetic data in the tests. In a 120 s headless run of *Tales of Xillia 2* the game calls
`cellAudioInit`, opens and starts a port and sets the notify queue, and creates two ATRAC decoders
(both for sounds held in memory); the audio census counts 22,071 blocks, 4,017 of them non-silent,
and the dump has non-silent audio from about 50 s on (see [Status](status.md)). Where this page describes how "the game" uses the library (thread priorities,
buffer sizes, sound formats), that is how the earlier game behaved, and it is not verified for this
one.

## What this game uses

The executable imports 10 `cellAudio` functions and 15 `cellAtrac` functions:

| Module | Imported | Handler in the runtime |
| --- | --- | --- |
| `cellAudio` | `cellAudioInit`, `cellAudioQuit`, `cellAudioPortOpen`, `cellAudioPortStart`, `cellAudioPortStop`, `cellAudioPortClose`, `cellAudioGetPortConfig`, `cellAudioSetNotifyEventQueue`, `cellAudioRemoveNotifyEventQueue` | yes |
| `cellAudio` | `cellAudioSetPortLevel` | yes |
| `cellAtrac` | `cellAtracSetDataAndGetMemSize`, `cellAtracCreateDecoderExt`, `cellAtracDeleteDecoder`, `cellAtracDecode`, `cellAtracGetStreamDataInfo`, `cellAtracAddStreamData`, `cellAtracGetVacantSize`, `cellAtracGetInternalErrorInfo`, `cellAtracIsSecondBufferNeeded`, `cellAtracGetSecondBufferInfo`, `cellAtracSetSecondBuffer`, `cellAtracGetLoopInfo`, `cellAtracSetLoopNum`, `cellAtracGetChannel`, `cellAtracGetMaxSample` | yes |

The `cellAudioOut` calls come from `cellSysutil` and all four have handlers
(`runtime/src/hle/cell_sysutil.cpp`): `cellAudioOutGetState`, `cellAudioOutGetConfiguration`,
`cellAudioOutGetSoundAvailability` and `cellAudioOutConfigure`. They share one list of sound modes,
which holds LPCM 2 ch at 48 kHz only, because the mixer outputs stereo, and one stored setting
(channel count, coding type and downmixer). In the current headless run the game calls `cellAudioOutGetConfiguration` once and none of the other three.

The game does not import `cellAtracCreateDecoder`, `cellAtracGetRemainFrame` or
`cellAtracGetSoundInfo`, which the runtime implements. It imports `cellAtracCreateDecoderExt`,
which takes a SPURS instance (the argument list is the handle, the work memory, a PPU priority and
a `CellAtracExtRes` of `{u32 spurs; u8 priority[8]}`). The code that calls it creates a SPURS
instance with one SPU, named "atrac3plus", for it; the runs create that instance's thread group but
nothing starts it. On the console the ATRAC3plus decoder runs on that SPU and the firmware decoder
is not part of this runtime, so `CreateDecoderExt` ignores its last three arguments and attaches a
host decoder, exactly as `CreateDecoder` does. The stream protocol that `cellAtrac` is implemented
with here (including `remain`, below) was derived from the earlier game, which used
`cellAtracCreateDecoder` and `cellAtracGetRemainFrame`; this game also uses `cellAtracGetVacantSize`
and the starvation code described under `Decode` below.

The game's sound engine core is a raw SPU thread group named "bnusCore" (the SPU image
`bnusCoreSpu.elf`, embedded in the executable at `0x00f7b080`, 85,008 bytes of text). The code
that starts it uses raw SPU thread group system calls and binds an event queue to it. It
is not established how the game's sound is split between that engine, the PPU mixer and
`cellAudio`, so it is not known whether the `cellAudio` code sees the game's whole mix or only
part of it. In the 120 s headless run above, the dump (`--audio-dump`) has non-silent audio in
three stretches (about 50 s to 52 s, 62 s to 79 s and 113 s to 118 s); what the sound is, and whether
it is complete, has not been checked by listening.

```
game mixer thread -> cellAudio port (guest memory) -> audio engine thread -> output ring -> WASAPI device thread
game sound thread -> cellAtrac (FFmpeg ATRAC3plus) -> game mixer
```

The code is in `runtime/src/audio/` (engine, outputs, WAV files), `runtime/src/hle/cell_audio*.cpp`,
`runtime/src/hle/cell_atrac*.cpp` and `runtime/src/media/at3_file.*`, `atrac_decoder.h` and
`ffmpeg_codecs.*` (the AT3 parser and the decoder). `tools/tox2_audiotool/` is the command-line
tool.

## Options

| Option | Meaning |
| --- | --- |
| `--audio none\|wasapi` | The output. `wasapi` plays through the default render device; `none` has no device but the clock and the game's mixer run the same, without the device-clock correction. Default: `wasapi` with the window, `none` with `--no-window`, so unattended runs and tests need no sound device. Any other value is a usage error. |
| `--audio-dump FILE` | Write the mix to `FILE` as a WAV file (48 kHz, stereo, 32-bit float), with either output. The file is created while the arguments are handled, so a path that cannot be written is a usage error (exit code 2). |

## Architecture

### Engine and block clock

`cellAudioInit` starts the audio engine, one plain host thread (not a guest thread). A
`cellAudio` mixer expects one period every 256 samples at 48 kHz (5.333 ms). The engine ticks on
the host's steady clock with a high-resolution waitable timer and, on every tick:

1. calls the producer once, which takes the guest lock, runs one `cellAudio` period (mix the
   started ports, advance read indices, tags and timestamps, notify the registered event queues)
   and returns one stereo block;
2. counts the block (non-silent when any sample exceeds -80 dBFS), writes it to the dump file and
   hands it to the output.

The schedule is absolute (`BlockClock`: the next period is due one period after the previous due
time, not after the previous wake-up), so a late wake-up does not shift later periods.

**No catch-up.** When the thread wakes and several periods are due, it produces one block and
covers the others with silence (`skipped` in the census). The earlier game's mixer wrote exactly one
block per notify and needs the guest lock to do it; two periods produced back to back under one
lock hold would consume a block the mixer never rewrote and lose the next one. A skipped period
does not call the producer and does not advance the read index, tags or timestamps; its silent
block still goes to the dump and the output, so the dump stays aligned with wall time and the
device does not underrun because of it. `kMaxCatchUpBlocks` is 0.

### Guest lock

Guest memory and lv2 objects are touched only under the guest lock, which is a FIFO ticket lock
shared with every guest thread and with the RSX thread. The engine takes it for one period at a
time and never holds it while sleeping, writing the dump or talking to a device. The `cellAudio`
and `cellAtrac` handlers never wait for the engine thread or the device, because the engine may
itself be waiting for the lock. Device threads never take the lock and never touch guest memory.

After each notify the earlier game's mixer needed 90 to 250 us and several passes through the lock (each hold
under 0.1 ms) to write its block, and it only gets the lock in arrival order. A long hold by another thread makes periods run back to back
and a block is then consumed before the mixer rewrote it, which plays as silence in the middle of
the music. The long holder was the RSX thread with the frame builder on (the window, `--render
vulkan`): it now pumps the command buffer in slices of 128 words and ends a hold once it has lasted
300 us (`kPumpSliceWords`, `kMaxGuestLockHold` in `runtime/src/rsx/rsx_thread.cpp`), then queues
again behind the other waiters. A pump also stops after each draw end, and the draw's index, vertex
and texture bytes are copied after the lock is released, so a hold no longer contains a draw's copy.

Two more timing facts mattered for its sound:

- The earlier game's decode threads polled with 1 ms `sys_timer_usleep` calls. Windows rounds timed waits to
  its timer tick (15.6 ms), so `tox2_game` requests a 1 ms system timer resolution for the whole run
  (`runtime/src/core/timer_resolution.*`, `timeBeginPeriod`). Without it that decode loop would
  poll about six times a second instead of about sixty and could not feed the mixer.
- A guest thread created with a numerically lower priority than its creator runs before the
  creator continues (see [Runtime internals](runtime.md#threads)). In the earlier game the mixer
  thread (priority 90) was created by the main thread (1005) and had to store its notify queue key
  before main registered it with `cellAudioSetNotifyEventQueue`. This game's thread layout is not
  known yet.

### Outputs

An output implements `AudioOutput` (`audio_output.h`): `name`, `open`, `submit` (called by the engine
thread only, never blocks), `description`, `stats` and `fillFrames`.

- **none** discards every block and counts it. It has no device clock, so the engine's clock is
  exactly the steady clock.
- **wasapi** (`wasapi_output.cpp`, library `tox2_audio_wasapi`, links `ole32` and `avrt`) opens the
  default render device in shared mode with an event callback. The stream format is 48 kHz stereo
  float32 and the Windows audio engine converts it to the device's mix format
  (`AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`). The device buffer is 40 ms. The device thread runs with
  the "Pro Audio" multimedia scheduler class when that is available.

The engine thread and the device thread are joined by a lock-free ring of 32 blocks. The device
starts playing once 4 blocks are queued (`kPrimeBlocks`), and above 12 blocks (`kHighWaterBlocks`)
`submit` drops the oldest block and counts it. When the device runs dry it plays silence and the
missing frames are counted as underrun.

### Following the device clock

The device's clock differs from the steady clock by tens of parts per million. Left alone, the
queue would grow until a block is dropped, or shrink until the device underruns. An output that
reports a fill level (frames submitted and not yet played) therefore steers the engine's period:
once per second a PI controller compares the mean fill of that second with a target of 6 blocks
(1536 frames) and scales the period by a correction of at most +-500 ppm (+-2.7 us on 5.333 ms).
The game sees periods of 5.333 ms within a few microseconds and nothing is resampled. The controller
holds its state while the device is not playing (priming, outage). The null output reports no fill
level and gets no correction. The current correction is printed as `clock +NN ppm` in the census.

### Device loss

If the device is lost while running (`AUDCLNT_E_DEVICE_INVALIDATED` or another stream error), the
output logs `tox2: audio: wasapi: device lost (<hresult>); retrying`, keeps draining its ring at the
nominal rate so the engine does not notice, and tries to open the default device again at once and
then every 2 seconds. A successful reopen logs `tox2: audio: wasapi: reopened <description>`.

### Dump file

The dump is a WAV file with 32-bit float samples, 48 kHz, 2 channels. Only the engine thread writes
it. The header is patched (and flushed) after every 48000 frames and by the census provider on
every stop path, because the process ends with `std::_Exit` and runs no destructors; a file cut
short is valid up to its last full second. `tox2_audiotool wavstat` reads it.

## cellAudio

All 11 `cellAudio` functions the earlier game imported and `cellAudioSetPortLevel` are implemented (`runtime/src/hle/cell_audio.cpp` for the handlers,
`cell_audio_ports.*` for the state): `cellAudioInit`, `cellAudioQuit`, `cellAudioPortOpen`,
`cellAudioPortStart`, `cellAudioPortStop`, `cellAudioPortClose`, `cellAudioGetPortConfig`,
`cellAudioGetPortBlockTag`, `cellAudioGetPortTimestamp`, `cellAudioSetNotifyEventQueue` and
`cellAudioRemoveNotifyEventQueue`. `cellAudioSetPortLevel(port, level)` sets the level of an open
port (the mixer multiplies the port's samples by it; a negative level or NaN is refused with
`CELL_AUDIO_ERROR_PARAM`, a closed port is `CELL_AUDIO_ERROR_PORT_NOT_OPEN`), and a reopened port
starts at 1.0 again. This game does not import `cellAudioGetPortBlockTag` and `cellAudioGetPortTimestamp`. Other
functions (for example `cellAudioCreateNotifyEventQueue` and `cellAudioAddData`) are not
implemented.

- **Ports.** There are 8 ports. The earlier game opened port 0 with 8 channels, 8 blocks, no
  attributes and level 1.0 (the logged line is below). Channel counts 0 (mono), 2 and 8 and block counts 2, 4, 8,
  16 and 32 are accepted. A port's memory is `max(1, channels) * blocks * 256 * 4` bytes (a mono port counts as one channel) of big-endian
  float32, allocated in 64 KiB units in the main region, and `cellAudioGetPortConfig` reports it
  (address at offset `+0x1c`).
- **Read index.** The read-index cell (address at config offset `+0x00`) is a big-endian 64-bit
  value, as the game reads it. It holds the block that the next period mixes, so the game's mixer
  writes that block after each notify.
- **A period** mixes the block at the read index of every started port into the stereo output,
  scaled by the port level, then zeroes the block (a block the game did not rewrite in time plays as
  silence, not as a repeat), advances the read index, records the tag and timestamp of the port,
  clamps the sum to [-1, 1] (a NaN becomes 0) and sends the notify event to every registered
  queue. Ports that are open but not started contribute nothing and do not advance.
- **Downmix.** The host output is stereo. An 8-channel port keeps its front left and right
  channels at unity gain and drops the other six, which is the "no downmix" mode that
  `cellAudioOutGetState` reports to the game. A mono port is copied to both sides. The census counts
  non-zero samples per input channel, and the first non-zero sample on channel 2 to 7 logs
  `tox2: audio: input channel N carries signal; the stereo downmix drops it`.
- **Tags and timestamps.** The tag of a block is the number of the period that published it, so
  tags are unique and increasing. `cellAudioGetPortTimestamp` returns the time (microseconds, the
  `sys_time_get_system_time` clock) of the period that published the tag; older tags are
  extrapolated back at 5333 us per period, and a tag that was not issued yet returns
  `CELL_AUDIO_ERROR_TAG_NOT_FOUND` (`0x8031070f`).
- **Notify.** `cellAudioSetNotifyEventQueue(key)` looks the queue up by its IPC key
  (see [lv2 IPC keys](runtime.md#implemented-system-calls)) and registers it; a period then sends
  one event per registered queue with data words 0. A queue that is full (the earlier game's held two
  events) or gone only counts as a failed notify and is never an error to the game. A key that
  matches no queue returns `CELL_AUDIO_ERROR_TRANS_EVENT` (`0x80310709`) and logs `not found`.
- **Errors.** The other error codes follow the library: not initialised (`0x80310703`), already
  initialised (`0x80310701`), bad parameter (`0x80310704`), no free port (`0x80310705`), port
  already running (`0x80310706`), not open (`0x80310707`), not running (`0x80310708`), out of
  shared memory (`0x8031070b`). `cellAudioQuit` stops the engine without waiting for its thread.

## cellAtrac

The runtime implements 18 functions (`runtime/src/hle/cell_atrac.cpp` for the handlers,
`cell_atrac_stream.*` for the per-handle model, which never touches guest memory itself):
`SetDataAndGetMemSize`, `CreateDecoder`, `CreateDecoderExt`, `DeleteDecoder`, `Decode`,
`GetStreamDataInfo`, `AddStreamData`, `GetVacantSize`, `GetInternalErrorInfo`, `GetRemainFrame`,
`IsSecondBufferNeeded`, `GetSecondBufferInfo`, `SetSecondBuffer`, `GetLoopInfo`, `SetLoopNum`,
`GetChannel`, `GetMaxSample` and `GetSoundInfo`. This game imports 15 of them (see above). The
other `cellAtrac` functions and `cellAtracMulti` are not implemented.

The earlier game's own library never read the `CellAtracHandle` it passed, and its sound manager
owned both sides of the stream protocol, so the state is kept on the host, keyed by the handle's guest
address, and nothing is written into the handle. Every value written through an output pointer is a
4-byte big-endian cell. The work memory size reported by `SetDataAndGetMemSize` is `0x1000`; the
decoder state is on the host.

- **Files.** All the earlier game's sounds were ATRAC3plus in RIFF/WAVE files (`WAVE_FORMAT_EXTENSIBLE`
  with the ATRAC3plus sub-format GUID, 48 kHz, mono or stereo, 2048 samples per frame). The parser
  (`media/at3_file.*`) reads `fmt `, `fact`, `smpl` and `data`, skips other chunks, and rejects
  other formats. `fact` holds the sample count and, as its third value, the encoder delay; the valid
  audio is the decoder output from sample `delay` for `count` samples. A loop is the first `smpl`
  loop, in decoder output positions.
- **Decoder.** Frames are decoded by FFmpeg's `atrac3plus` decoder through the pinned LGPL DLLs
  (see [Movies](movies.md)), one frame per packet, synchronously inside `cellAtracDecode` under the
  guest lock (a frame takes tens of microseconds). The decoder is behind the
  `AtracFrameDecoder` interface (`media/atrac_decoder.h`), so the runtime core has no FFmpeg types.
- **Two modes.** In memory mode (background music, sound effects) the whole file is in the buffer
  (`read_bytes == buffer_bytes`). In streaming mode (voice) the buffer is a ring of `buffer_bytes`
  (the earlier game used `0x20000`) that holds file bytes from offset 0 to `read_bytes` at the start.
  `GetStreamDataInfo` returns the write pointer in the ring, the contiguous writable bytes and the
  file offset of the next byte to read; `AddStreamData` commits bytes at that pointer. A frame that
  wraps around the ring is gathered from both parts.
- **`Decode`** decodes the frames at the cursor until it has output, the end is reached or the
  buffered data runs out. It writes interleaved big-endian float32 (a mono sound stays one channel),
  cuts the encoder delay at the start and the samples after `delay + count` at the end, and returns
  at most 2048 frames per call (`GetMaxSample`). `finish` is set when the last valid sample was
  output and no loop-back is left. **Starvation** (the next frame is not completely buffered and
  nothing was decoded) returns `0x80610332` (`NODATA_IN_BUFFER`) and writes none of the output cells.
  This game's decode thread (`bnusCoreDecoderAT3PThread`) waits on a condition variable and retries
  on that code, publishes a PCM buffer on 0, ends the sound quietly on `0x80610331`
  (`ALLDATA_WAS_DECODED`, never returned here: the end is `finish = 1`), and treats every other code as
  fatal (it calls `GetInternalErrorInfo`, aborts, deletes the decoder and exits the thread). Success
  with 0 samples and `finish = 0` would make it publish an empty PCM buffer. The earlier game ended a
  sound for good on any non-zero result and so needed success with 0 samples; that rule was replaced
  because this game's loop needs the code. In memory mode the whole file is buffered, so
  starvation does not occur.
  Other errors are for a bad handle state only: `Decode` before `CreateDecoder` returns `0x80610321`, a
  handle with no data `0x80610322`, `CreateDecoder` twice `0x80610323`.
- **`GetVacantSize`** writes the number of ring bytes that `AddStreamData` could still take in
  total: the buffer size minus the bytes from the decode cursor to the end of the buffered data,
  limited by what is left of the file. This is not the contiguous run that `GetStreamDataInfo`
  returns, which also stops at the ring end. The game stores the value without looking at the return
  code and sizes its next file read from it. In memory mode there is nothing to add: the cell gets 0 and
  the return value is `0x80610341` (`ALLDATA_IS_ON_MEMORY`, as for the other stream calls); this game
  calls it in memory mode and ignores the code.
- **`GetInternalErrorInfo`** writes 0 and returns success: the host decoder has no internal error.
- **`remain`.** `GetRemainFrame` and `Decode` report the number of complete frames buffered ahead of
  the cursor. In memory mode it is -1. In streaming mode it is -2 once the end of the file is
  buffered and no loop-back is left. The earlier game refilled the ring only while `remain` was not negative.
- **Loops.** `SetLoopNum(n)` is the number of passes through the loop region still to play, the
  current one included (-1 forever); 0 and 1 mean no loop-back follows. When decoding reaches the
  loop end and a loop-back follows, the count is decremented, the cursor goes back to the frame
  before the one holding the loop start (in streaming mode the buffered bytes are dropped and the
  game refills from the new read position), the decoder is reset, and the output is discarded up to
  the loop start; the extra frame primes the decoder's overlap. `GetLoopInfo` reports status 1 when
  the file has a usable loop (queried before any decode) and the current count, so the earlier game's "last
  pass" test (`status == 1 && loops == 1`) works. A loop whose region holds no valid audio is
  ignored. A sound without loop points that the earlier game repeated was started again by the game itself
  with `DeleteDecoder` and `SetDataAndGetMemSize` on the same handle, which resets the state.
- **Other calls.** `IsSecondBufferNeeded` returns 0 in `r3` (the answer is the return value; a
  second buffer is never needed), `GetSecondBufferInfo` and `SetSecondBuffer` accept and write
  zeros. `GetSoundInfo` returns the last sample index (`count - 1`) and the loop points in
  valid-audio coordinates. `SetDataAndGetMemSize` returns `0x80610312` for a header that is not a
  supported ATRAC3plus RIFF file, `0x80610311` when `read_bytes` is larger than the buffer,
  `0x80610314` for a sampling rate other than 32000, 44100 or 48000 and `0x80610315` for data
  without a complete frame.

## Failure policy

Audio problems never stop the game: the game must not stall because of sound.

| Case | Behaviour |
| --- | --- |
| WASAPI cannot be opened (no device, COM error, format rejected, no answer within 2 s) | One warning `tox2: audio: wasapi: <reason>; continuing without sound`, the engine switches to the null output and the clock keeps running. The census shows the output as `none (wasapi failed)`. |
| Device lost while running | `device lost` warning, silence, reopen attempts (see above). The clock keeps running. |
| The engine wakes late | One block is produced; the missed periods are skipped as silence and counted. |
| `--audio-dump` file cannot be created or written | A usage error before the run starts; a write error during the run logs one warning and stops the dump. |
| FFmpeg DLLs missing or no `atrac3plus` decoder | One warning `tox2: atrac: no decoder: <reason>`. `Decode` outputs silence of the right length; the stream protocol is unchanged, and the census counts the frames as "without decoder". |
| Unusable AT3 data | `SetDataAndGetMemSize` returns the error code; one warning per handle. |
| A frame the decoder rejects | Silence for that frame, counted as an error; one warning per handle. |
| Notify queue full or gone | Counted as a failed notify, never an error to the game. |

## Log lines

Lines start with `tox2: audio:`, `tox2: atrac:`, `tox2: timer:` or `tox2: thread:`.

| Line | Level | When |
| --- | --- | --- |
| `tox2: audio: output <name>[ (<description>)]` | info | The engine opened its output. WASAPI's description is `<device>, mix <rate> Hz <ch> ch, buffer <ms> ms, period <ms> ms`. |
| `tox2: audio: dump <file name>` | info | The dump file was created. |
| `tox2: audio: port <n> open: <ch> ch, <blocks> blocks, attr <hex>, level <f>` | info | `cellAudioPortOpen` |
| `tox2: audio: notify queue key <hex> -> <queue id hex\|not found>` | info | `cellAudioSetNotifyEventQueue` |
| `tox2: audio: input channel <n> carries signal; the stereo downmix drops it` | info | First non-zero sample on a dropped channel |
| `tox2: audio: <output>: <reason>; continuing without sound` | warn | The output could not be opened |
| `tox2: audio: dump <file name>: <reason>; continuing without a dump`, `tox2: audio: writing the dump failed; the dump stops here` | warn | Dump problems |
| `tox2: audio: wasapi: device lost (<hresult>); retrying` | warn | Device lost |
| `tox2: audio: wasapi: reopened <description>` | info | The device came back |
| `tox2: audio: wasapi: reopen failed: <reason>` | debug | A reopen attempt failed (retried every 2 s) |
| `tox2: audio: wasapi: device thread failed[: <reason>]` | warn | The device thread stopped on an error |
| `tox2: audio: cannot start the engine thread: <reason>; continuing without sound` | warn | The engine thread could not be created |
| `tox2: audio: clock <+-ppm> ppm, mean fill <n> frames (target <n>), dropped <n>, underrun <n> frames` | debug | Every 10 s |
| `tox2: atrac: decoder <handle hex>: <ch> ch <rate> Hz, <frames> frames, loop <start>-<end>\|none, all on memory\|streaming` | info | `cellAtracCreateDecoder`, `cellAtracCreateDecoderExt` |
| `tox2: atrac: no decoder: <reason>` | warn | No FFmpeg decoder (once) |
| `tox2: atrac: handle <hex>: unusable data: <reason>` | warn | `SetDataAndGetMemSize` failed (once per handle) |
| `tox2: atrac: handle <hex>: decode error, playing silence for such frames: <reason>` | warn | First rejected frame of a handle |
| `tox2: timer: requested <n> ms, system period <x> ms\|unknown` | debug | Start-up |
| `tox2: timer: the 1 ms resolution request was refused; timed waits use the default tick` | warn | Start-up |
| `tox2: thread: <id> <name> prio <p> starts before creator <id> prio <q>` | debug | A thread was created with a lower priority number than its creator |
| `tox2: thread: <id> <name> not queued within 5 ms; creator continues` | info | The new thread did not queue for the guest lock in time |

## Census

When the game called `cellAudioInit` or created a cellAtrac decoder, the stop census has a `census: audio`
section. The `audio` and `audio input` lines need `cellAudioInit`; the `atrac` line needs at least one
decoder:

```
census: audio
  audio wasapi: 78733 blocks (61204 non-silent), skipped 13, dropped 0, underrun 416 frames, notify 78733 sent 0 failed, max producer 23.77 ms, clock -31 ppm
  audio input: 78733 periods, non-zero samples per channel 0:8190004 1:8190004 2:0 3:0 4:0 5:0 6:0 7:0
  atrac 6 decoders, 7623 frames decoded (6120 non-silent), 0 errors, 0 without decoder
```

(The numbers are examples.) The first line is the engine: the output name, blocks produced and how
many were non-silent, periods skipped, blocks dropped and frames of underrun at the output, notifies
sent and failed, the longest producer call (it includes the wait for the guest lock; a few ms of
it at start-up is normal) and the clock correction. `audio input` counts periods and the non-zero
samples per input channel. The `atrac` line appears when a decoder was created: decoders created,
frames decoded, how many of them had content above -80 dBFS, frames the decoder rejected and
frames passed through without a decoder.

## `tox2_audiotool`

`tox2_audiotool SUBCOMMAND ...` works without the game and exits with 0 on success, 1 when the work
fails and 2 on a usage error.

| Subcommand | What it does |
| --- | --- |
| `wavstat FILE [--window S]` | Per window of `S` seconds (default 1) the RMS and peak level in dBFS and the number of non-silent frames, then `wavstat: <seconds> s, non-silent windows <k> of <m>`. Reads the dump file, PCM 16-bit or float32, mono or stereo. |
| `devices` | Lists the active render devices with their mix format and device periods; the default one is marked. Exit code 1 when there is none. |
| `tone [--output none\|wasapi] [--seconds N] [--hz F] [--db L]` | Plays a sine through an output (default `wasapi`, 2 s, 440 Hz, -18 dBFS) and prints the output statistics (submitted, dropped, underrun, played, device errors). |
| `play FILE.wav [--output none\|wasapi]` | Plays a 48 kHz mono or stereo WAV file the same way. |
| `at3scan FILE` | Lists the ATRAC3plus RIFF entries of a file that holds them one after the other (offset, size, channels, rate, block size, samples, delay, loop) with summary lines. This game's sound containers have not been examined. |
| `at3 FILE [--index N \| --offset N] [--wav OUT] [--hash] [--play] [--expect-frames N] [--expect-samples N] [--expect-pcm SHA256]` | Decodes one entry (the `N`-th of the `at3scan` list, default 0, or the one at a byte offset) through FFmpeg, cuts it to the valid samples, prints `at3: <frames> frames, <samples> samples[, pcm-sha256 <hash>], <us> us/frame` and `at3 ok`. `--hash` prints the SHA-256 of the interleaved little-endian float32 result, `--wav` writes it, `--play` plays it through WASAPI. The `--expect-*` options turn it into a check (exit code 1). It also fails when the sample count differs from the file's `fact` count. |

`at3` needs the FFmpeg DLLs next to the executable, like `tox2_game`.

## Tests

`audio_core`, `audio_wasapi` and `media_ffmpeg` are described in [Tests](tests.md). All of them use
synthetic data; no test plays this game's sound.
The creation rule for threads, the timer resolution and the event queue keys are covered by
`runtime_core`.

## Clean room

The behaviour of `cellAudio` (ports, blocks, read index, tags, timestamps, notify events, error
codes), of the event queue IPC keys and the layouts and error codes of `cellAtrac` was implemented
from a written description of RPCS3's behaviour, produced separately from the implementation. No
RPCS3 code was copied or read while writing it. The file headers name the RPCS3 source file and
symbol that each fact comes from. The `cellAtrac` stream protocol is not RPCS3's (its HLE for
`cellAtrac` only returns constants); it comes from how the earlier game's own code used the library.
The WASAPI output follows Microsoft's documentation.

## Not implemented

- Output libraries other than WASAPI, exclusive mode, multichannel output (an 8-channel port
  is downmixed to stereo), a volume option and the choice of the output device (the default render
  device is used).
- ATRAC3 (non-plus) data: the earlier game had none; this game's data has not been checked.
- `cellAudio` functions that no examined game imports, `cellAtracMulti`, `cellAudioIn` and microphone
  input.
- The firmware SPURS-based ATRAC3plus decoder behind `cellAtracCreateDecoderExt`: its SPURS instance
  is created and never started, and the sound is decoded on the host (see
  [What this game uses](#what-this-game-uses)).
