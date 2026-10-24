# Audio

This page covers the audio code in the runtime: the `cellAudio` and `cellAtrac` HLE, the audio
engine and the device outputs (WASAPI on Windows, CoreAudio on macOS). The tests use synthetic data.
In a 120 s headless run of *Tales of Xillia 2* the game calls
`cellAudioInit`, opens and starts a port and sets the notify queue, and creates two ATRAC decoders
(both for sounds held in memory); the audio census counts 22,071 blocks, 4,017 of them non-silent,
and the dump has non-silent audio from about 50 s on (see [Status](status.md)). Longer windowed runs create dozens of
decoders for mono and stereo sounds at 48 kHz, held in memory or streamed through a ring of 0x40000 bytes, and they report
no decoder error. What this page says about how the game uses the library (thread names and priorities, port
parameters, sound formats, the order of the `cellAtrac` calls) comes from the debug log and census of those runs and from the
game's code; a statement that has not been checked with this game says so.

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
`cellAtracGetSoundInfo`, which the runtime implements and only the tests call. It imports `cellAtracCreateDecoderExt`,
which takes a SPURS instance (the argument list is the handle, the work memory, a PPU priority and
a `CellAtracExtRes` of `{u32 spurs; u8 priority[8]}`). The code that calls it creates a SPURS
instance with one SPU, named "atrac3plus", for it; the runs create that instance's thread group but
nothing starts it. On the console the ATRAC3plus decoder runs on that SPU and the firmware decoder
is not part of this runtime, so `CreateDecoderExt` ignores its last three arguments and attaches a
host decoder, exactly as `CreateDecoder` does. The stream protocol that `cellAtrac` is implemented
with here follows how this game's decode and file-reading threads drive the library: the order of the
calls, `cellAtracGetVacantSize` for the size of the next read, and the starvation code described under `Decode`
below.

The game's sound engine core is a raw SPU thread group named "bnusCore" (the SPU image
`bnusCoreSpu.elf`, embedded in the executable at `0x00f7b080`, 85,008 bytes of text). The code
that starts it uses raw SPU thread group system calls and binds an event queue to it. It
is not established how the game's sound is split between that engine, the PPU mixer and
`cellAudio`, so it is not known whether the `cellAudio` code sees the game's whole mix or only
part of it. In the 120 s headless run above, the dump (`--audio-dump`) has non-silent audio in
three stretches (about 50 s to 52 s, 62 s to 79 s and 113 s to 118 s); what the sound is, and whether
it is complete, has not been checked by listening.

```
game mixer thread -> cellAudio port (guest memory) -> audio engine thread -> output ring -> device thread (WASAPI or CoreAudio)
game sound thread -> cellAtrac (FFmpeg ATRAC3plus) -> game mixer
```

The code is in `runtime/src/audio/` (engine, outputs, WAV files; `stream_output.*` holds the device thread that both device outputs share,
`wasapi_output.*` and `coreaudio_output.*` the two APIs), `runtime/src/hle/cell_audio*.cpp`,
`runtime/src/hle/cell_atrac*.cpp` and `runtime/src/media/at3_file.*`, `atrac_decoder.h` and
`ffmpeg_codecs.*` (the AT3 parser and the decoder). `tools/tox2_audiotool/` is the command-line
tool.

## Options

| Option | Meaning |
| --- | --- |
| `--audio none\|wasapi` (Windows), `--audio none\|coreaudio` (macOS) | The output. `wasapi` and `coreaudio` play through the default render device (the default output device on macOS); `none` has no device but the clock and the game's mixer run the same, without the device-clock correction. Default: the system's device output (`wasapi` or `coreaudio`) with the window, `none` with `--no-window`, so unattended runs and tests need no sound device. Any other value, including the other system's output name, is a usage error. |
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
covers the others with silence (`skipped` in the census). A `cellAudio` mixer writes one block per
notify and needs the guest lock to do it; two periods produced back to back under one
lock hold would consume a block the mixer never rewrote and lose the next one. This game's mixer
behaves that way: in the logged windowed runs (5,000 to 79,000 periods each) 0 to 32 periods consumed a block
that it had not rewritten. A skipped period
does not call the producer and does not advance the read index, tags or timestamps; its silent
block still goes to the dump and the output, so the dump stays aligned with wall time and the
device does not underrun because of it. `kMaxCatchUpBlocks` is 0.

### Guest lock

lv2 and HLE state is touched only under the guest lock, which is shared with every guest thread and with the RSX
thread. With `--ppu-threads legacy` it is also the right to run guest code, so guest memory is touched only under it
too and PPU guest threads never run in parallel. With `--ppu-threads 1` or `2` (the default is `2`) the same lock is
the kernel lock: guest threads take it only inside syscall and HLE handlers and run guest code in a PPU slot without
it, so the mixer and the other guest threads run beside the audio engine, which still takes the lock for the
producer call (see [Threads](runtime.md#threads)). Waiters queue in a list and an unlock wakes only the one it hands
the lock to. The audio engine, the RSX thread and SPU threads are served in arrival order. Guest threads fill the "PPU
turns" of that order by lv2 priority, so a thread with a lower priority number, such as the game's sound threads at
priority 0, gets a turn before a thread at 1001. The engine takes it for one period at a
time and never holds it while sleeping, writing the dump or talking to a device. The `cellAudio`
and `cellAtrac` handlers never wait for the engine thread or the device, because the engine may
itself be waiting for the lock. Device threads never take the lock and never touch guest memory.

After each notify the game's mixer has to write its block under the lock, and it gets the lock only after the
holders ahead of it. A long hold by another thread makes periods run back to back
and a block is then consumed before the mixer rewrote it, which plays as silence in the middle of
the music. The RSX thread with the frame builder on (the window, `--render
vulkan`) is the thread that can hold the lock longest, so it pumps the command buffer in slices of 128 words and ends a hold once it has lasted
300 us (`kPumpSliceWords`, `kMaxGuestLockHold` in `runtime/src/rsx/rsx_thread.cpp`), then queues
again behind the other waiters. A pump also stops after each draw end, and the draw's index, vertex
and texture bytes are copied after the lock is released, so a hold no longer contains a draw's copy.

The lock records how long each kind of caller waits for it and holds it (classes `ppu`, `ppu-high` for guest
threads of lv2 priority below 100, `spu`, `audio`, `rsx` and `other`), the length of the queue ahead of each
contended acquisition, which holder classes the waits were spent behind, the hand-off time from one holder to the
next waiter and the number of wake-ups that found it was not their turn. It prints as the `census: guest lock`
section, once per second in the `lock` perf line and as a table in the overlay (see [Log lines](#log-lines)). In
a 400 s scripted run of this game at resolution scale 1 and 3, the lock was not what made the mixer miss a block:
the wait of the audio, SPU and high-priority guest threads had a 99th percentile of 0.11 to 0.16 ms and a
maximum under 2 ms, and looked the same in the seconds with a missed block and in the others; a hand-off took
under 5 us, and a contended acquisition caused about 0.07 spurious wake-ups. Those figures come from the lock as it was
before guest threads were ordered by priority, which served every class in arrival order and woke all waiters on each hand-off. They did not point at the
lock, so the lock's order for host-class waiters and their place in the queue were left as they were when guest
threads started to be ordered by priority.

### Host thread priorities

The audio engine thread runs at `THREAD_PRIORITY_TIME_CRITICAL` and the host threads of an SPU thread group with a
scheduler context and an lv2 priority of 31 or less (the game's sound engine `bnusCore`, priority 16; the SPURS
kernels use 250) at `THREAD_PRIORITY_HIGHEST` (`runtime/src/core/host_priority.*`), so a saturated renderer cannot
take their time slices. All of them sleep or block while they wait, so the raise cannot starve other threads.
On macOS the same roles get QoS classes instead of thread priorities: the audio engine thread `QOS_CLASS_USER_INTERACTIVE` and the
high-priority SPU threads `QOS_CLASS_USER_INITIATED`.
PPU guest threads stay at normal priority: the lock or the PPU slots already order them by lv2 priority, and at most two
run at once (one in `legacy`). The RSX thread stays at normal priority too. `--host-priorities off` (see [Options](runtime-options.md)) leaves every thread
at normal priority. The first use of each role logs `tox2: host: <role> thread priority <n>` at debug level; a
refused change logs a warning and the thread keeps its priority. With the lock and reservation changes in place
the raise made no measurable difference to the number of unwritten blocks; there were fewer guest-lock waits above
1 ms with it.

Two more timing facts concern the sound threads:

- Windows rounds timed waits to its timer tick (15.6 ms), so `tox2_game` requests a 1 ms system timer
  resolution for the whole run (`runtime/src/core/timer_resolution.*`, `timeBeginPeriod`); without it a short
  `sys_timer_usleep` lasts about 15 ms. This game's ATRAC decode thread waits on a condition variable instead of
  sleeping; the threads that poll with `sys_timer_usleep` are its main thread and a worker (see
  [Runtime internals](runtime.md#threads)). Whether the sound of this game depends on the request has not been measured. macOS has no coarse tick to raise, so
  timed waits there already have the precision requested and the request counts as granted (the debug line says
  `system period unknown`); the host event loop of `tox2_game` turns off App Nap and timer coalescing for the
  process while it runs.
- A guest thread created with a numerically lower priority than its creator runs before the
  creator continues (see [Runtime internals](runtime.md#threads)). In this game the main thread (priority 1001)
  creates `bnusCoreDecoderServerThread` and `bnusCoreUpdateThread` (priority 0) and `CnuSound2Thread` (499), and
  each `CnuSound2Thread` creates a `bnusCoreDecoderAT3PThread` (priority 0), so all of them start before their creator
  continues (the debug lines `tox2: thread: ... starts before creator`). The main thread opens
  the audio port and registers the notify queue, and the queue key is found at that point. Whether the registration
  depends on the creation rule has not been checked.

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
- **coreaudio** (`coreaudio_output.cpp`, library `tox2_audio_coreaudio`, uses the C APIs of
  AudioToolbox and CoreAudio) plays through the system's default output `AudioUnit`, which follows a
  change of the default device. The stream format is 48 kHz stereo float32 and the unit converts it to
  the device's. The render callback takes frames from a lock-free FIFO of 40 ms (1920 frames) that the
  device thread fills, plays silence for what is missing and wakes the device thread; the callback does
  not lock, allocate, log or touch guest memory. The device period is the device's buffer size. The unit
  is started when the output is opened and plays silence until the device thread starts the stream, so
  that CoreAudio's start-up time is spent before the engine's clock runs and not in its fill level.

The engine thread and the device thread are joined by a lock-free ring of 32 blocks; this and the
device thread (`tox2_audio_stream`) are the same for both outputs. The device
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

CoreAudio does the same with `coreaudio` in the lines. It counts the default output device changing
or the device in use stopping being alive (both reported by CoreAudio property listeners) as a loss,
so that the reopened stream reads the new device's rate and buffer size. A status that is a
four-character code with printable characters is shown as that code in quotes, for example `'!dev'`,
otherwise as a number.

### Dump file

The dump is a WAV file with 32-bit float samples, 48 kHz, 2 channels. Only the engine thread writes
it. The header is patched (and flushed) after every 48000 frames and by the census provider on
every stop path, because the process ends with `std::_Exit` and runs no destructors; a file cut
short is valid up to its last full second. `tox2_audiotool wavstat` reads it.

## cellAudio

Twelve `cellAudio` functions are implemented (`runtime/src/hle/cell_audio.cpp` for the handlers,
`cell_audio_ports.*` for the state): `cellAudioInit`, `cellAudioQuit`, `cellAudioPortOpen`,
`cellAudioPortStart`, `cellAudioPortStop`, `cellAudioPortClose`, `cellAudioGetPortConfig`,
`cellAudioGetPortBlockTag`, `cellAudioGetPortTimestamp`, `cellAudioSetNotifyEventQueue` and
`cellAudioRemoveNotifyEventQueue`, and `cellAudioSetPortLevel`. `cellAudioSetPortLevel(port, level)` sets the level of an open
port (the mixer multiplies the port's samples by it; a negative level or NaN is refused with
`CELL_AUDIO_ERROR_PARAM`, a closed port is `CELL_AUDIO_ERROR_PORT_NOT_OPEN`), and a reopened port
starts at 1.0 again. This game does not import `cellAudioGetPortBlockTag` and `cellAudioGetPortTimestamp`, so only the tests exercise
them (see Tags and timestamps below). Other
functions (for example `cellAudioCreateNotifyEventQueue` and `cellAudioAddData`) are not
implemented.

- **Ports.** There are 8 ports. This game opens port 0 with 8 channels, 8 blocks, no
  attributes and level 1.0 (the logged line is below), and no other port. Channel counts 0 (mono), 2 and 8 and block counts 2, 4, 8,
  16 and 32 are accepted. A port's memory is `max(1, channels) * blocks * 256 * 4` bytes (a mono port counts as one channel) of big-endian
  float32, allocated in 64 KiB units in the main region, and `cellAudioGetPortConfig` reports it
  (address at offset `+0x1c`).
- **Read index.** The read-index cell (address at config offset `+0x00`) is a big-endian 64-bit
  value, as the game reads it. It holds the block that the next period mixes, so the game's mixer
  writes that block after each notify.
- **A period** mixes the block at the read index of every started port into the stereo output,
  scaled by the port level, then refills the block with the unwritten marker (below; a block the game
  did not rewrite in time plays as silence, not as a repeat), advances the read index, records the tag
  and timestamp of the port, clamps the sum to [-1, 1] (a NaN becomes 0) and sends the notify event to
  every registered queue. Ports that are open but not started contribute nothing and do not advance.
- **Unwritten blocks.** The refill value is a big-endian -0.0f (`80 00 00 00`) in every sample, and
  `cellAudioPortStart` fills the whole port with it. It mixes as silence, but it differs from the +0.0 the
  game writes in a quiet passage, so a period can tell a block the game never rewrote from one it wrote as
  zeros. A block is unwritten when every sample of it still holds the marker; one written sample, in any
  channel, makes it written (a block written with exactly -0.0 everywhere cannot be told apart from an
  unwritten one). A port counts unwritten blocks only from its first written block after the start, so the
  start-up blocks are not counted. The census and the overlay show the number of unwritten blocks, the runs of
  consecutive ones (a run counts when it starts) and the longest run. This is the measure of an audible
  drop: one unwritten block is one 5.33 ms silence in the output.
- **Downmix.** The host output is stereo. An 8-channel port keeps its front left and right
  channels at unity gain and drops the other six, which is the "no downmix" mode that
  `cellAudioOutGetState` reports to the game. A mono port is copied to both sides. The census counts
  non-zero samples per input channel, and the first non-zero sample on channel 2 to 7 logs
  `tox2: audio: input channel N carries signal; the stereo downmix drops it`. In this game's censuses channels 2 to 7
  hold no non-zero sample, so the downmix drops no content.
- **Tags and timestamps.** The tag of a block is the number of the period that published it, so
  tags are unique and increasing. `cellAudioGetPortTimestamp` returns the time (microseconds, the
  `sys_time_get_system_time` clock) of the period that published the tag; older tags are
  extrapolated back at 5333 us per period, and a tag that was not issued yet returns
  `CELL_AUDIO_ERROR_TAG_NOT_FOUND` (`0x8031070f`).
- **Notify.** `cellAudioSetNotifyEventQueue(key)` looks the queue up by its IPC key
  (see [lv2 IPC keys](runtime.md#implemented-system-calls)) and registers it; a period then sends
  one event per registered queue with data words 0. A queue that is full
  or gone only counts as a failed notify and is never an error to the game; this game's censuses show every notify
  sent and none failed. A key that
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

The game's decode thread owns both sides of the stream protocol, so the state is kept on the host, keyed by
the handle's guest address, and nothing is written into the handle. That the game's own code never reads the
`CellAtracHandle` has not been checked in the code; the sounds of the logged runs decode without error with the handle
left untouched. Every value written through an output pointer is a
4-byte big-endian cell. The work memory size reported by `SetDataAndGetMemSize` is `0x1000`; the
decoder state is on the host.

- **Files.** Every sound of this game that reached `SetDataAndGetMemSize` in the logged runs was accepted: ATRAC3plus in
  RIFF/WAVE files (`WAVE_FORMAT_EXTENSIBLE` with the ATRAC3plus sub-format GUID, 48 kHz, 2048 samples per frame),
  mono (block size `0x158`, held in memory, no loop) or stereo (block size `0x2b0`, with a loop, held in memory or
  streamed). The parser
  (`media/at3_file.*`) reads `fmt `, `fact`, `smpl` and `data`, skips other chunks, and rejects
  other formats. `fact` holds the sample count and, as its third value, the encoder delay; the valid
  audio is the decoder output from sample `delay` for `count` samples. A loop is the first `smpl`
  loop, in decoder output positions.
- **Decoder.** Frames are decoded by FFmpeg's `atrac3plus` decoder through the pinned LGPL DLLs
  (Homebrew's libraries on macOS; see [Movies](movies.md)), one frame per packet, synchronously inside `cellAtracDecode` under the
  guest lock (a frame takes tens of microseconds). The decoder is behind the
  `AtracFrameDecoder` interface (`media/atrac_decoder.h`), so the runtime core has no FFmpeg types.
- **Two modes.** The mode follows the data, not the sizes. In memory mode (sound effects) every
  frame is in the first `read_bytes` bytes, so the whole file is in the buffer whatever `buffer_bytes`
  is. Anything else is streaming mode: the buffer is a ring of `buffer_bytes` (`0x40000` for this game's streamed
  background music, whose first read is between `0x10000` and `0x40000` bytes) that holds file bytes from offset 0 to
  `read_bytes` at the start, also when `read_bytes == buffer_bytes` (a ring that starts full).
  The ring is a FIFO of stream bytes in the order the game feeds them: stream byte `s` lives at ring
  offset `s % buffer_bytes`, and `buffered_end` is the stream position of the next byte to write.
  `GetStreamDataInfo` returns the write pointer in the ring, the contiguous writable bytes and the
  file offset of the next byte to read (see Loops: this is not `buffered_end` after a loop);
  `AddStreamData` commits bytes at that pointer. A frame that wraps around the ring is gathered from
  both parts.
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
  with 0 samples and `finish = 0` would make it publish an empty PCM buffer, so starvation is the error code. In memory
  mode the whole file is buffered, so starvation does not occur; in this game's streamed sounds it does.
  Other errors are for a bad handle state only: `Decode` before `CreateDecoder` returns `0x80610321`, a
  handle with no data `0x80610322`, `CreateDecoder` twice `0x80610323`.
- **`GetVacantSize`** writes the number of ring bytes that `AddStreamData` could still take in
  total: the buffer size minus the stream bytes from the decode cursor to the end of the buffered data,
  limited only by the stream limit (see Loops). It is not limited by the file size when the file
  loops, because the game feeds the loop region again after the pass end; a cap at the file end would
  keep the value below the game's `0x10000` read size, which its loader takes only whole, and the
  reader would stall before the loop end. This is not the contiguous run that `GetStreamDataInfo`
  returns, which also stops at the ring end. The game stores the value without looking at the return
  code and sizes its next file read from it. In memory mode there is nothing to add: the cell gets 0 and
  the return value is `0x80610341` (`ALLDATA_IS_ON_MEMORY`, as for the other stream calls); this game
  calls it in memory mode and ignores the code.
- **`GetInternalErrorInfo`** writes 0 and returns success: the host decoder has no internal error.
- **`remain`.** `GetRemainFrame` and `Decode` report the number of complete frames buffered ahead of
  the cursor. In memory mode it is -1. In streaming mode it is -2 once the stream limit is buffered
  (the end of the file, or the end of the last pass to play for a looping file). This game never reads the
  value, so these results have not been checked against it.
- **Loops.** `SetLoopNum(n)` is the number of passes through the loop region still to play, the
  current one included (-1 forever); 0 and 1 mean no loop-back follows. When decoding reaches the
  loop end and a loop-back follows, the count is decremented, the cursor goes back to the frame
  before the one holding the loop start, the decoder is reset, and the output is discarded up to
  the loop start; the extra frame primes the decoder's overlap.
  In streaming mode nothing buffered is dropped at a loop-back. This game's reader does not follow
  `read_pos`: it feeds the file in order up to the pass end `E`, then feeds again from a fixed offset
  `R`, whatever the loop count. With `smpl` positions `start` and `end` in 2048-sample frames,
  `R = frameOffset(start / 2048 - 1)` (the priming frame, frame 0 when `start` is in frame 0) and
  `E = frameOffset(end / 2048 + 1)`, the end of the frame that holds the loop end (never above the
  file end; the file end when there is no `smpl` loop with a non-zero end, and then the reader stops
  there). `L = E - R`. Stream position `s` is file offset `s` while `s < E`, and `R + (s - E) % L`
  after that: pass 0 holds file bytes `[0, E)` and every later pass `[R, E)`. The model keeps a pass
  counter and maps the decode cursor to a stream position, so after a loop-back the frames it decodes
  are the ones the game fed next. `GetStreamDataInfo` reports `read_pos` as the file offset of
  `buffered_end`, which reads `R` once `E` is buffered; the game ignores it.
  The stream limit is where the last pass to play ends: none for a usable loop with count -1,
  `E + last * L` with `last` the pass number of the last pass otherwise (`E` without a usable loop).
  `GetVacantSize` is 0 and `remain` is -2 once `buffered_end` reaches it. The frames after `E` of
  a looping file are never fed, so in streaming mode the last pass, and a pass whose loop is not
  usable, end at the end of the frame that holds the loop end (`(end / 2048 + 1) * 2048` samples)
  instead of the end of the valid audio. A file whose `smpl` loop is not usable (its region holds no
  valid audio) is played once and stops at `E`. `GetLoopInfo` reports status 1 when
  the file has a usable loop (queried before any decode) and the current count. This game imports it, but what it does
  with the two values has not been checked. A loop whose region holds no valid audio is
  ignored. `SetDataAndGetMemSize` on a handle that already has state discards that state, whether or not
  `DeleteDecoder` was called, so a sound can be started again on the same handle.
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
| The device output cannot be opened (WASAPI: no device, COM error, format rejected, no answer within 2 s; CoreAudio: no default output device or a failing call, with the status) | One warning `tox2: audio: <wasapi\|coreaudio>: <reason>; continuing without sound`, the engine switches to the null output and the clock keeps running. The census shows the output as `none (wasapi failed)` or `none (coreaudio failed)`. |
| Device lost while running | `device lost` warning, silence, reopen attempts (see above). The clock keeps running. |
| The engine wakes late | One block is produced; the missed periods are skipped as silence and counted. |
| `--audio-dump` file cannot be created or written | A usage error before the run starts; a write error during the run logs one warning and stops the dump. |
| FFmpeg DLLs (libraries on macOS) missing or no `atrac3plus` decoder | One warning `tox2: atrac: no decoder: <reason>`. `Decode` outputs silence of the right length; the stream protocol is unchanged, and the census counts the frames as "without decoder". |
| Unusable AT3 data | `SetDataAndGetMemSize` returns the error code; one warning per handle. |
| A frame the decoder rejects | Silence for that frame, counted as an error; one warning per handle. |
| Notify queue full or gone | Counted as a failed notify, never an error to the game. |

## Log lines

Lines start with `tox2: audio:`, `tox2: atrac:`, `tox2: timer:` or `tox2: thread:`.

| Line | Level | When |
| --- | --- | --- |
| `tox2: audio: output <name>[ (<description>)]` | info | The engine opened its output. The description of both device outputs is `<device>, mix <rate> Hz <ch> ch, buffer <ms> ms, period <ms> ms`; on macOS the mix is the device's nominal rate and output channel count, the buffer is the 40 ms FIFO and the period is the device's buffer size. |
| `tox2: audio: dump <file name>` | info | The dump file was created. |
| `tox2: audio: port <n> open: <ch> ch, <blocks> blocks, attr <hex>, level <f>` | info | `cellAudioPortOpen` |
| `tox2: audio: notify queue key <hex> -> <queue id hex\|not found>` | info | `cellAudioSetNotifyEventQueue` |
| `tox2: audio: input channel <n> carries signal; the stereo downmix drops it` | info | First non-zero sample on a dropped channel |
| `tox2: audio: <output>: <reason>; continuing without sound` | warn | The output could not be opened (`<output>` is `wasapi` or `coreaudio`) |
| `tox2: audio: dump <file name>: <reason>; continuing without a dump`, `tox2: audio: writing the dump failed; the dump stops here` | warn | Dump problems |
| `tox2: audio: wasapi: device lost (<hresult>); retrying` | warn | Device lost. On macOS the line says `coreaudio` and gives the status instead of an HRESULT |
| `tox2: audio: wasapi: reopened <description>` | info | The device came back (`coreaudio` on macOS) |
| `tox2: audio: wasapi: reopen failed: <reason>` | debug | A reopen attempt failed (retried every 2 s; `coreaudio` on macOS) |
| `tox2: audio: wasapi: device thread failed[: <reason>]` | warn | The device thread stopped on an error (`coreaudio` on macOS) |
| `tox2: audio: cannot start the engine thread: <reason>; continuing without sound` | warn | The engine thread could not be created |
| `tox2: audio: clock <+-ppm> ppm, mean fill <n> frames (target <n>), dropped <n>, underrun <n> frames` | debug | Every 10 s |
| `tox2: atrac: decoder <handle hex>: <ch> ch <rate> Hz, <frames> frames, loop <start>-<end>\|none, all on memory\|streaming` | info | `cellAtracCreateDecoder`, `cellAtracCreateDecoderExt` |
| `tox2: atrac: no decoder: <reason>` | warn | No FFmpeg decoder (once) |
| `tox2: atrac: handle <hex>: unusable data: <reason>` | warn | `SetDataAndGetMemSize` failed (once per handle) |
| `tox2: atrac: handle <hex>: decode error, playing silence for such frames: <reason>` | warn | First rejected frame of a handle. The reason starts with `frame <i> pass <k> (file <hex>, stream <hex>, ring offset <hex>, buffered to <hex>, bytes <16-byte head hex>)` |
| `tox2: atrac: set data handle <hex>: all on memory\|streaming, read <hex> of buffer <hex>, data at <hex> size <hex>, block <hex>` | debug | `SetDataAndGetMemSize` accepted the data |
| `tox2: atrac: stream info handle <hex>: write at <hex>, writable <hex>, read pos <hex>` | debug | `GetStreamDataInfo` (write at is the ring offset) |
| `tox2: atrac: add stream data handle <hex>: <hex> bytes, result <hex>, buffered to <hex>` | debug | `AddStreamData` (buffered to is the stream position) |
| `tox2: atrac: handle <hex>: frame <i> pass <k> file <hex> stream <hex> ring <hex>: <16-byte head hex>` | debug | The first frame decoded after `SetDataAndGetMemSize` and the first one after each loop-back. After a loop-back the file offset is `R`, and the head must equal the file bytes there |
| `tox2: timer: requested <n> ms, system period <x> ms\|unknown` | debug | Start-up (always `unknown` on macOS) |
| `tox2: timer: the 1 ms resolution request was refused; timed waits use the default tick` | warn | Start-up |
| `tox2: thread: <id> <name> prio <p> starts before creator <id> prio <q>` | debug | A thread was created with a lower priority number than its creator |
| `tox2: thread: <id> <name> not queued within 5 ms; creator continues` | info | The new thread did not queue for the guest lock (a PPU slot in `--ppu-threads 1\|2`) in time |
| `tox2: host: <audio engine\|high-priority SPU> thread priority <n>` | debug | First use of a host priority role (`<n>` is the Windows thread priority value or, on macOS, the QoS class value); a refused change is a warning `tox2: host: cannot raise the <role> thread priority (error <n>)` |
| `tox2: perf: t <s> s: audio flips <n> periods <n> unwritten <n> (runs <n>)` | debug | Once per second from the first `cellAudioInit`: the deltas over the last second. `t` is the guest system time |
| `tox2: perf: t <s> s: lock <class> n <n> avg <x> p99 <x> max <x> ms >1.00ms <n> hold p99 <x> max <x> \| <class> ...` | debug | Once per second, for each guest-lock class that took the lock in that second: its wait (count, mean, 99th percentile, maximum in ms and the number of waits above 1 ms), then its hold. The classes `audio`, `spu` and `ppu-high` add `ahead avg <n> behind <class> <ms> ... ms`: the mean queue length of the contended acquisitions and the time the waits were spent behind each holder class (sums over the waiters, so read them as ratios). Further fields follow, separated by `\|`, when they apply in that second: `yield calls <n> ok <n> host <n> kept <n> backoff <n>` (the outcomes of `sys_ppu_thread_yield`, see [Threads](runtime.md#threads)), `lwmutex wait <n> handoff <n> mutex wait <n> handoff <n>` (waits on a lightweight mutex or mutex and how many ended in an ownership hand-off), `long holds <n>` (guest-lock holds of 5 ms or more) and `fs read <n> <bytes> bytes max <x> ms write <n> <bytes> bytes max <x> ms` (the file system transfers of that second and the longest one). The last field is `lwmutex slow lock <n> unlock <n>` (with `misaligned <n>` when non-zero): the lightweight mutex calls that left the compare-and-swap path in that second (see [Threads](runtime.md#threads)) |
| `tox2: guest lock long hold: <x> ms holder <class> thread <id> prio <p> (<name>) at release: <a> last in hold: <b> previous in hold: <c> cpu <t>` | warn | A guest-lock hold lasted 5 ms or more, at most 20 lines per second. The three labels are the syscall or imported function running at the release, the last one entered during the hold and the one before it; `cpu` is the holder thread's CPU time in the hold (PPU holders only, else `n/a`); a holder that is not a guest thread has no `thread ... (<name>)` part |
| `tox2: perf: t <s> s: spu <id> <name> busy n <n> avg <x> p99 <x> max <x> ms >5.33ms <n>; wait p99 <x> max <x> \| rsv-wait epoch <n> timeout <n> lost-after-timeout <n> pre-sleep <n> woken <n> woken-us <2 <n> 2-5 <n> 5-10 <n> 10-20 <n> 20-50 <n> 50-100 <n> 100-500 <n> 500-1000 <n> >1000 <n> notify putllc <n> put <n> putlluc <n> ppu <n> wake <n> lost-after-timeout table <n> plain <n>[ | putllc-fail\| putllc-fail lost <n> locked <n> moved <n> data <n>]` | debug | Once per second: per SPU thread that received events, its busy time (reply to the next receive; the last number counts intervals over one 5.33 ms period) and wait time (receive to reply), and how the SPU reservation waits ended (see [SPU usage](spu.md#log-lines-and-census)) |

## Census

When the game called `cellAudioInit` or created a cellAtrac decoder, the stop census has a `census: audio`
section. The `audio` and `audio input` lines need `cellAudioInit`; the `atrac` line needs at least one
decoder:

```
census: audio
  audio wasapi: 78733 blocks (61204 non-silent), skipped 13, dropped 0, underrun 416 frames, notify 78733 sent 0 failed, max producer 23.77 ms, clock -31 ppm
  audio input: 78733 periods, non-zero samples per channel 0:8190004 1:8190004 2:0 3:0 4:0 5:0 6:0 7:0, unwritten blocks 2 (runs 2, longest 1)
  atrac 6 decoders, 7623 frames decoded (6120 non-silent), 0 errors, 0 without decoder
```

(The numbers are examples; the first line says `audio coreaudio` on macOS.) The first line is the engine: the output name, blocks produced and how
many were non-silent, periods skipped, blocks dropped and frames of underrun at the output, notifies
sent and failed, the longest producer call (it includes the wait for the guest lock; a few ms of
it at start-up is normal) and the clock correction. `audio input` counts periods and the non-zero
samples per input channel, then the [unwritten blocks](#cellaudio) the mixer found: blocks the game did not
rewrite between two mixes of their port, the runs of them and the longest run. In a 400 s scripted run of
this game, the count between 200 s and 380 s, the stretch with the most renderer load, fell from 75 at
resolution scale 1 and 52 at scale 3 to 1 at each scale after the changes to the reservation table (see
[SPU usage](spu.md#reservations-and-memory-ordering); the host priorities above made no measurable difference);
the remaining one was at a scene event. The `atrac` line appears when a decoder was created: decoders created,
frames decoded, how many of them had content above -80 dBFS, frames the decoder rejected and
frames passed through without a decoder.

## `tox2_audiotool`

`tox2_audiotool SUBCOMMAND ...` works without the game and exits with 0 on success, 1 when the work
fails and 2 on a usage error.

| Subcommand | What it does |
| --- | --- |
| `wavstat FILE [--window S]` | Per window of `S` seconds (default 1) the RMS and peak level in dBFS and the number of non-silent frames, then `wavstat: <seconds> s, non-silent windows <k> of <m>`. Reads the dump file, PCM 16-bit or float32, mono or stereo. |
| `devices` | Lists the active render devices with their mix format and device periods; the default one is marked. On macOS these are the output devices, with their nominal rate, channel count and buffer size. Exit code 1 when there is none. |
| `tone [--output none\|wasapi] [--seconds N] [--hz F] [--db L]` | Plays a sine through an output (default `wasapi`, 2 s, 440 Hz, -18 dBFS) and prints the output statistics (submitted, dropped, underrun, played, device errors). On macOS the output is `none` or `coreaudio`, the default. |
| `play FILE.wav [--output none\|wasapi]` | Plays a 48 kHz mono or stereo WAV file the same way (`coreaudio` instead of `wasapi` on macOS). |
| `at3scan FILE` | Lists the ATRAC3plus RIFF entries of a file that holds them one after the other (offset, size, channels, rate, block size, samples, delay, loop) with summary lines. This game's sound containers have not been examined. |
| `at3 FILE [--index N \| --offset N] [--wav OUT] [--hash] [--play] [--expect-frames N] [--expect-samples N] [--expect-pcm SHA256]` | Decodes one entry (the `N`-th of the `at3scan` list, default 0, or the one at a byte offset) through FFmpeg, cuts it to the valid samples, prints `at3: <frames> frames, <samples> samples[, pcm-sha256 <hash>], <us> us/frame` and `at3 ok`. `--hash` prints the SHA-256 of the interleaved little-endian float32 result, `--wav` writes it, `--play` plays it through the system's device output (WASAPI or CoreAudio). The `--expect-*` options turn it into a check (exit code 1). It also fails when the sample count differs from the file's `fact` count. |

`at3` needs the FFmpeg libraries like `tox2_game`: the DLLs next to the executable on Windows,
Homebrew's libraries on macOS (see [Movies](movies.md#ffmpeg-on-macos)).

## Tests

`audio_core`, `audio_wasapi` (Windows), `audio_coreaudio` (macOS) and `media_ffmpeg` are described in [Tests](tests.md). All of them use
synthetic data; no test plays this game's sound.
The creation rule for threads, the timer resolution and the event queue keys are covered by
`runtime_core`.

## Clean room

The behaviour of `cellAudio` (ports, blocks, read index, tags, timestamps, notify events, error
codes), of the event queue IPC keys and the layouts and error codes of `cellAtrac` was implemented
from a written description of RPCS3's behaviour, produced separately from the implementation. No
RPCS3 code was copied or read while writing it. The file headers name the RPCS3 source file and
symbol that each fact comes from. The `cellAtrac` stream protocol is not RPCS3's (its HLE for
`cellAtrac` only returns constants); it follows how the game's own code feeds and decodes its streams (the loop pass
order and the read sizes were fitted to this game's reader).
The WASAPI output follows Microsoft's documentation.

## Not implemented

- Output libraries other than WASAPI (Windows) and CoreAudio (macOS), exclusive mode, multichannel output (an 8-channel port
  is downmixed to stereo), a volume option and the choice of the output device (the default render
  or output device is used).
- ATRAC3 (non-plus) data. No logged run of this game passed such data to `SetDataAndGetMemSize` (no `unusable data`
  warning), but its sound containers have not been scanned for it.
- `cellAudio` functions that no examined game imports, `cellAtracMulti`, `cellAudioIn` and microphone
  input.
- The firmware SPURS-based ATRAC3plus decoder behind `cellAtracCreateDecoderExt`: its SPURS instance
  is created and never started, and the sound is decoded on the host (see
  [What this game uses](#what-this-game-uses)).
