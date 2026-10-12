# SPU usage

The runtime runs SPU code. The SPU programs of the game and of libsre are recompiled to C like the PPU
code (see [Emitting C](emitter.md#spu-images)) and run on host threads, one per SPU. This page records
how the game uses the SPUs, measured from the executable, from libsre and from headless runs, then how
the runtime runs them and what a run reaches today. Addresses are guest addresses in the pinned
`EBOOT.elf`, or in the relocated `libsre.reloc.elf` where the text says so (libsre sits at
`0x011d0000`).

## SPU images

All SPU code is embedded in the executable. Every loader site passes a link-time constant (a TOC
slot or a table in the executable), and no SPU image is opened from a file. `probe` lists the eight
images (see [Analysis tools](analysis-tools.md)); the names come from each image's SPUNAME note.

| # | Address | Text bytes | LS base / entry | Name | Role |
| --- | --- | --- | --- | --- | --- |
| 1 | `0x00f75400` | 4,480 | `0x3000` / `0x3070` | SpursTaskSpline.elf | SPURS task: spline evaluation |
| 2 | `0x00f76700` | 18,336 | `0x3000` / `0x3090` | edgelzma_inflate_task | SPURS task: Edge LZMA inflate |
| 3 | `0x00f7b080` | 85,008 | `0x80` / `0xd0` | bnusCoreSpu.elf | SPU thread of a normal group: core of the game's sound engine |
| 4 | `0x00f90280` | 19,568 | `0x3000` / `0x3070` | SpursTaskAnimation.elf | SPURS task: animation |
| 5 | `0x00fb0b80` | 59,552 | `0x3000` / `0x3070` | SpursTaskUpdateMatrix | SPURS task: matrix and hierarchy update |
| 6 | `0x00fe1480` | 35,920 | `0x3000` / `0x3070` | SpursTaskRenderParticle | SPURS task: particle rendering |
| 7 | `0x00ff7580` | 15,456 | `0x3000` / `0x3070` | SpursTaskMorph.elf | SPURS task: morph targets |
| 8 | `0x01027680` | 15,408 | `0x3000` / `0x3070` | SpursTaskSkin.elf | SPURS task: skinning |

The text segments total about 254 KB (about 63,000 instructions). Images 4 to 8 also carry large
zero-filled data segments (LS work buffers), and the 0x80 bytes in front of each of them are its
`CellSpursTaskLsPattern`. The movies are Bink and are decoded on the PPU; there is no Bink SPU
image.

The SPURS task images use MFC DMA channels 16 to 24, the atomic channel 27 (consistent with a
statically linked `cellSpursQueue`/`EventFlag` library), the outbound mailboxes 28 and 30, and
fixed LS locations that belong to the SPURS kernel and the taskset policy module: `0x80`-`0xf0`,
`0x170`, `0x1c0`-`0x1e0`, `0x2fb0` and `0x2fd0`. That firmware code is not in the executable; it is
in libsre (see [Where the SPURS code lives](#where-the-spurs-code-lives)).

## Where the SPURS code lives

The game's `cellSpurs` imports resolve to the firmware module `libsre.prx` (`cellSre_Library` 1.1,
firmware 4.93). It contains the PPU side of SPURS, which the runtime now recompiles and runs, and the
SPU programs that SPURS loads, which are embedded as data. The build relocates it to `0x011d0000`
(see [Decrypting the inputs](building.md#decrypting-the-inputs)), and the addresses below are in
that image. Each program is referenced by the PPU code: libsre imports the kernel with
`sys_spu_image_import` and hands the others to the SPURS kernel as workload modules.

| Address | Size | What | LS base / entry | Used by |
| --- | --- | --- | --- | --- |
| `0x011f0380` | ELF, one `0x780` byte segment | SPURS kernel, version 1 (`spurs_kernel_executable.elf`) | `0x100` / `0x818` | `cellSpursInitializeWithAttribute` when the attribute flag `0x4` is clear (this game) |
| `0x011f0c00` | ELF, one `0x790` byte segment | SPURS kernel, version 2 (`spurs_kernel2_executable.elf`) | `0x100` / `0x848` | the same call with the flag set; not imported by this game |
| `0x011f1480` | `0x2200` | system service module | `0xa00` | SPURS instance setup (its address and size are stored in the `CellSpurs` structure) |
| `0x011f3680` | `0x1e40` | taskset policy module | `0xa00`..`0x2840` | `_cellSpursCreateTaskset`, which adds the taskset as a workload |
| `0x011f5500` | `0x670` | taskset 2 exit handler | `0x10000` | `cellSpursCreateTaskset2`; the taskset policy module loads it when a task exits (word at LS `0x2fd4` equal to 4) |
| `0x011f5b80` | `0x710` | exit code plugin | `0x10000` | `cellSpursCreateTaskset2`; the policy module's other exit path loads it |
| `0x011f6300`, `0x011fa180` | `0x3e30` each | job chain policy modules | `0xa00` | job chains; this game uses none |

The two kernels are SPU ELF files (class 1, big-endian, machine 23); the other six blobs are raw and
begin with four `ila $2` instructions. When it loads libsre the runtime checks the ELF headers of the
kernels at these addresses and the first word of each raw blob (`tox2: libsre: layout ok`). The task images of the game depend on
the kernel and the taskset policy module for the LS locations listed above, so the same SPU code has
to run for them.

The kernel of version 1 is the one that is imported: the thread group has five threads, each starts at
the kernel entry `0x818` and receives its index in argument 1 (`index << 32`) and the `CellSpurs`
address (`0x01127c80` for instance A) in argument 2.

## How the game sets them up

The game imports 30 `cellSpurs` functions, `sys_spu_image_import`/`sys_spu_image_close` and the
`_sys_spu_printf_*` functions. It uses no job chains, workloads, `cellSpursJq` or `cellSync`.

- SPURS instance A at `0x01127c80` (fn `0x0080dbec`): name prefix "SpursTLGraphics", 5 SPUs, SPU
  priority 250, PPU priority 1000, SPU thread group type `0x18`. `sys_spu_initialize(6, 0)` comes
  first.
- Taskset "SpursTasksetLZMA" on instance A (fns `0x0080d8c0`, `0x00c2e5e0`, `0x00c2e6c8`): five
  tasks of image 2, an `LFQueue` (PPU to SPU) for work and an event flag (SPU to PPU) for results.
- Five engine tasksets on instance A, all created by fn `0x00983cb8`, each with five tasks of one
  image, a `cellSpursQueue` (PPU to SPU) and an event flag (SPU to PPU):

  | Caller | Taskset | Image | Queue element x depth |
  | --- | --- | --- | --- |
  | `0x009097c0` | `0x01195980` | 8 Skin | 0x80 x 0x20 |
  | `0x009097e8` | `0x01192e00` | 7 Morph | 0x80 x 0x20 |
  | `0x00910968` | `0x011a6600` | 4 Animation | 0x80 x 0x80 |
  | `0x008e0d5c` | `0x0116dc00` | 5 UpdateMatrix | 0x80 x 0x80 |
  | `0x0090b1e4` | `0x011a0580` | 6 RenderParticle | 0x100 x 0x100 |

- Spline tasks (fns `0x004a77b4`, `0x004a7d74`, `0x004a947c`): four more tasksets on instance A;
  tasks of image 1 are created on demand with `cellSpursCreateTask2WithBinInfo` from a
  `CellSpursTaskBinInfo` table at `0x0104a700` and joined afterwards.
- Sound engine (fn `0x00955380`): `cellAudioInit`, then an SPU thread group "bnusCore" (type 0, so it has a
  scheduler context; it is not a raw SPU) with one thread "bnusCoreMain" running image 3, initialised as SPU
  number 1. The function then creates two event queues and a local port for each: queue A of SPU type
  (size 127) and queue B of PPU type (size 127). It binds queue A to the thread under the key `'nuAS'`
  (`0x6e754153`) with `sys_spu_thread_bind_queue` (`0x0095570c`), connects queue B to the thread's SPU port 7
  with `sys_spu_thread_connect_event`, and only then starts the group (`0x00955730`). It also creates the
  PPU threads "bnusCoreUpdateThread", "bnusCoreDecoderServerThread" and "bnusCoreDecoderAT3PThread".
  Shutdown is fn `0x00955804`. The PPU sends `STAT` (`0x53544154`) and `QUIT` (`0x51554954`) commands to
  queue A through its port; the SPU thread receives them with `stop 0x110` on `'nuAS'` and answers on
  port 7.
- SPURS instance B "atrac3plus" (fn `0x009568c0`): one SPU, created only to be passed to
  `cellAtracCreateDecoderExt`, so that the firmware ATRAC3plus decoder can run on it. The runtime
  decodes ATRAC3plus on the host and ignores this instance (see [Audio](audio.md#what-this-game-uses)).
- The SPU printf server (`0x0098dd24`-`0x0098e26c`, `0x00997928`-`0x00998dd4`) is the only code
  that uses `sys_spu_thread_read_ls`/`write_ls` and the SPU thread event syscalls; the game itself
  does not exchange LS data or mailbox values with an SPU program.

## How the runtime runs SPU code

The code is in `runtime/src/spu/` (threads, dispatch, MFC, channels, census), `runtime/src/core/reservation.*`
(atomic reservations) and `runtime/src/lv2/sys_spu.*` (thread groups). The emitted C is described in
[Emitting C](emitter.md#spu-images); this section covers what happens at run time.

### Execution model

- The firmware SPU programs from libsre (the version 1 kernel, the system service module and the
  taskset policy module) and the game's task images run as they are, as recompiled C. Nothing of SPURS is
  reimplemented on the host: the PPU side is libsre's recompiled PPU code and the SPU side is its
  recompiled SPU code.
- Control flow does not use the host stack. A call or branch to another function writes the link register
  and tail-calls the target; every return and every other indirect branch goes through `tox2SpuDispatch`,
  keyed by the LS address. A restored task context (the taskset policy module branches to a saved `$0`)
  is a dispatch like any other.
- The dispatch finds the image that occupies the target address (see [Image tracking](#image-tracking)),
  looks the address up in that image's table of dispatchable entries, compares a few instruction words of
  the entry with the LS bytes, and tail-calls the entry. A target that is not a dispatchable entry, or code
  that does not match the image, is a fatal stop: with no host stack there is no caller to return to.
- Every SPU thread is a `SpuThread` with a 256 KiB LS, the 128 registers, the channels, the MFC and its own
  host thread. A group start creates fresh `SpuThread` objects, so a restarted group starts from a zeroed
  LS. A thread ends when it reaches its exit address, executes a `stop` that ends it, or is halted: group
  termination and `stop 0x101` set a halt flag that generated code checks at backward branches and that
  wakes channel waits.
- **Guest lock.** SPU threads run outside the guest lock, in parallel with the PPU threads and with each
  other. Generated SPU code never sees guest memory; only MFC commands reach it. An SPU thread takes the
  guest lock only around the lv2 state its callbacks touch (sending an event, replying in the inbound
  mailbox, group bookkeeping when a thread stops or ends), and never while it waits on a channel. The
  PPU-side calls that wait for SPU threads (`sys_spu_thread_group_join` and `_terminate`) give the guest
  lock up while they wait. The PPU `sync` instruction is a full host fence, because SPU threads observe
  guest memory while PPU code runs (see [Threads](runtime.md#threads)).
- The supervisor counts an SPU thread as running, or as blocked in a channel wait. A run in which every PPU
  thread waits for SPU work is therefore not a stall while an SPU thread runs.

### Image tracking

The LS has one tag per 16 bytes. A tag says that these LS bytes are code of a registered image at its
link address. A copy of guest bytes into LS that covers part of a code segment of an image (an MFC GET,
or the segment load of a group start) tags the 16-byte units that lie wholly inside that part. The
dispatch accepts a target only when its unit is tagged, so an overlay (the system service module and the taskset policy
module share the LS area at `0xa00`, and the kernel swaps between them) is found by what was copied, and the first time an image is tagged the runtime
logs `tox2: spu: first image <name> at ls 0x<start>..0x<end> on thread 0x<id>`.

- A copy (an MFC GET or a segment load) that overwrites tagged units untags only the units whose bytes
  change. The taskset policy module restores a task's LS pattern from its context save area with a GET
  that rewrites the code with the bytes it already holds; those units stay tagged.
- The other LS writes untag every unit they touch: GETLLAR (its 128 bytes), a FILL segment and
  `sys_spu_thread_write_ls`. These untag the whole range even when the bytes would be the same.
- SPU store instructions do not change tags. Code that a store overwrites is found by the check words at
  the dispatch (`code mismatch` stop), not by the tags.
- Limitation: when a copy writes different bytes over tagged code, the next dispatch there stops as
  `unknown target ... (untracked)`; the runtime does not identify code by its content. Only images at their
  link address are accepted (a tagged address with another offset stops with `loaded at a non-link
  address`), and one thread can hold at most 255 distinct load records.

### Reservations and memory ordering

GETLLAR, PUTLLC, PUTLLUC and PUT, and the PPU store-conditionals (`stwcx.`, `stdcx.`), share one table of
65,536 stripes indexed by the 128-byte line (`(ea >> 7) & 0xffff`). A stripe word has a lock bit and a
version. A writer locks the stripes of the lines it writes, writes, and bumps the version; a line that
shares a stripe with another one causes at worst a spurious PUTLLC or store-conditional failure, which the guest code retries.

- GETLLAR copies the line (retrying while a writer holds the stripe) and keeps the version and a 128-byte
  snapshot. PUTLLC fails at once when the version moved, fails when the line differs from the snapshot,
  and otherwise writes only the aligned 8-byte chunks whose value differs from the snapshot, each with one
  atomic store. The reservation is cleared either way.
- **PPU reservations are line-granular.** `lwarx` and `ldarx` read the stripe version before the word
  (`ReservationTable::loadReserved`, which retries while a writer holds the stripe) and keep it in the PPU
  context. `stwcx.` and `stdcx.` take the stripe lock and succeed only when the version is unchanged and the
  word still has the loaded value. So a PUTLLC, PUTLLUC, MFC PUT or another store-conditional that touched
  any word of the line since the `lwarx` fails the store, as on hardware, even when the reserved word
  itself is intact. libsre needs this: `cellSpursSendSignal` does `lwarx` on a taskset word, plain loads
  of other words of the same line, then `stwcx.`, and retries only when the `stwcx.` fails. When the
  taskset policy module's PUTLLC moved a task from running to waiting between those loads, a value-only
  reservation let the `stwcx.` succeed on a torn snapshot, the call returned
  `CELL_SPURS_TASK_ERROR_SRCH` and an assertion in libsre aborted the game. The loop in `cellSpursQueuePushBody` that
  decides whether to signal a waiting task has the same shape against the SPU pop. Lines that share a stripe can fail a `stwcx.` spuriously; the guest
  retries. Plain PPU stores to other words of the line do not cancel the reservation (see the race window
  below); a plain store to the reserved word itself is still caught by the value compare.
- A thread waiting in `RdEventStat` for the reservation-lost event (`0x400`) sleeps until a reservation-aware
  write changes the store epoch or 1 ms passes, then compares the line with the snapshot. A plain PPU
  store is therefore seen within about 1 ms.
- **Known race window.** PPU plain stores (and host plain writes) are not instrumented. A plain store to
  an 8-byte chunk that a PUTLLC changes, landing after that PUTLLC's snapshot compare and before its chunk
  store (well under a microsecond), is overwritten without the PUTLLC failing. Plain stores before the
  compare make the PUTLLC fail; plain stores to chunks the PUTLLC leaves alone are kept; a plain-store
  sequence that restores the snapshot value is not seen; a PPU load can observe a PUTLLC of several chunks
  half applied.
- Reserved lines that receive PPU plain stores, from a sweep of the code (medium confidence; a runtime
  watchpoint would confirm). All live ones are in libsre's recompiled PPU code:

  | Line (instance A) | Plain stores | Overlaps SPU PUTLLC chunks |
  | --- | --- | --- |
  | `CellSpurs` line 0, `0x01127c80..0x01127cff` | `cellSpursAddWorkload` (`stbx` at `+0x00+wid`, `stb` at `+0x10+wid`, `+0x40+wid` and `+0x72`), `ShutdownWorkload` (`+0x72`), `Finalize`. The workload slots are written when each taskset is created, five times while the group runs | `+0x72` shares a chunk with the kernel's workload-select PUTLLC and the system service's idle PUTLLC; this plain store is also the only PPU wake-up of the idle system service |
  | `CellSpurs` `+0x80` line, `0x01127d00..0x01127d7f` | `AddWorkload` (`+0x80+wid`, `+0x90+wid`, `+0xa0+wid`, `+0xbd`), `ShutdownWorkload`, `Finalize` | the system service's activate-workloads PUTLLCs |
  | event flag at `0x01127b00` | `cellSpursEventFlagWait` stores byte `+0x07` after the lv2 receive | the SPU event flag set PUTLLC rewrites the same chunk |
  | counter line at `0x01127a80` | the game stores the counter once before the first push; its lwmutex (host-side, at `0x01127a88`) is in another chunk than the counter | a worst case is one PUTLLC retry |

  Task set lines, the lock-free queue header and the `+0x900` line of `CellSpurs` receive plain stores only
  before any task exists, or none.
- **Decision.** libsre's plain stores are not routed through the table (`reservations().put`). In the
  runs with the engine tasks the PUTLLC failure rate was about 62 % (26,075 succeeded and 41,879 failed in
  the 30 s run quoted below), which is what retries on shared lines look like when five kernels contend
  for them. No livelock and no stale data was seen: the first inflated archive item matches its
  oracle hash in every run. The race window above and the line list stay a known limitation; if a hang
  or lost workload shows up, those stores are the first suspects.
- Barriers: SPU `sync` and `dsync` need no host fence (the LS is private); the MFC barrier, `mfcsync` and
  `mfceieio` are sequentially consistent fences, and atomic commands are sequentially consistent.

### MFC

Commands run synchronously when `MFC_Cmd` is written, so a transfer is complete before the SPU reads its
tag status and the command queue is never full.

- Implemented: GET and PUT with their barrier and fence forms, the list forms (`getl`, `putl`; a list
  element with the stall-and-notify bit is a fatal stop), GETLLAR, PUTLLC, PUTLLUC, PUTQLLUC, the barrier
  command, `mfceieio` and `mfcsync`. `sndsig` and any other opcode stop the run and name the command.
- Sizes and alignment follow the hardware rules: 1, 2, 4, 8 or a multiple of 16 bytes up to `0x4000`,
  naturally aligned below 16 bytes. A violation, a tag id above 31 or a guest fault is a fatal stop
  (`tox2: stop: spu: mfc <name> (command 0x<op>): <reason> at ls 0x<site> on thread 0x<id>`, followed by
  the parameters). LS addresses wrap at 256 KiB.
- Tag groups: `WrTagUpdate` 0, 1 and 2 are all satisfied at once and `RdTagStat` returns the tag mask.
  `RdAtomicStat` returns the status of the last atomic command (GETLLAR 4, PUTLLC 0 or 1, PUTLLUC 2).
- A GET tracks the LS image (above); PUT, PUTLLUC and atomic commands go through the reservation table.

### Channels and mailboxes

- Inbound mailbox (4 entries, `RdInMbox` blocks when empty), outbound mailbox (1 entry, `WrOutMbox` blocks
  when full) and the interrupt mailbox (`WrOutIntrMbox`, which calls the owner, see below).
- Events (`WrEventMask`, `RdEventMask`, `WrEventAck`, `RdEventStat`): the lock-line-lost event `0x400`, the
  inbound mailbox event `0x10`, MFC queue vacancy `0x8` (always set), tag status update `0x1` (after a tag
  update request) and the decrementer `0x20`. Waiting on a mask with none of these is a fatal stop that
  names the mask. The decrementer counts down at the timebase frequency.
- `RdMachStat` returns the interrupt-enable state in bit 0. `WrSRR0`/`RdSRR0` hold `srr0`. Every other
  channel is a fatal stop that names the channel, for example the signal notification channels.
- Every blocking wait uses the thread's own mutex and condition variable (or the store epoch of the
  reservation table), never the guest lock, and ends when a halt is requested.
- No SPU interrupt is ever delivered: `iret` and the interrupt enable bits are executed, but nothing
  raises an interrupt.

### lv2 SPU calls

The calls and their checks are listed under [SPU system calls](runtime.md#spu-system-calls). What is
specific to execution:

- `sys_spu_thread_group_start` zeroes each thread's LS, applies the image segments (a copy goes through the
  tracked copy, a fill is an untagging fill), sets `$3` to `$6` to the four arguments as 64-bit values in
  the preferred doublewords (argument 1 is `index << 32` for SPURS), clears all other registers, sends the
  group's RUN event when a queue is connected, and starts the host threads. The group is running until the
  last thread ends.
- `stop` codes: `0x100` continues, `0x102` ends the thread with the status from the outbound mailbox,
  `0x101` ends the group with that status and halts the other threads. `0x110` and `0x111` receive an event
  (below). Any other code, `0x120` included, is a fatal stop
  (`tox2: stop: spu: stop 0x<code> at ls 0x<site> on thread 0x<id>: not implemented`). When the last thread
  has ended the group returns to the initialised state and `sys_spu_thread_group_join` wakes with cause 1
  (group exit), 2 (all threads exited) or 4 (terminated) and the status. A finished group can be started
  again.
- `WrOutIntrMbox` is the SPU thread's system call: codes 0 to 63 send an event to the queue bound to the
  port in the low bits and put the result in the inbound mailbox, codes 64 to 127 throw an event, and codes
  128 and 192 set a bit of an event flag. The event source is `0xFFFFFFFF53505501`. A full event queue is
  retried every millisecond. Other codes (129 to 191, above 192) are fatal stops.
- `sys_spu_thread_write_ls`, `_read_ls` and `_write_spu_mb` reach the LS and mailbox of the latest run of a
  thread. `write_spu_mb` into a full mailbox replaces the newest entry, as the hardware does, and logs a
  warning once per thread. Raw SPU threads are not implemented.
- Receiving an event: `sys_spu_thread_bind_queue` (193) gives a thread a key (any 32-bit number) for an SPU
  event queue, and the SPU program receives from it with the key in the outbound mailbox (`wrch 28`) and
  `stop 0x110` (blocking) or `stop 0x111` (not blocking). The answer is in the inbound mailbox: a status word
  (0, or an error code) and, when it is 0, three words with the low 32 bits of the event's `data1`, `data2` and
  `data3` (the source is not delivered). The queue is looked up in the thread's own table; the replies are:

  | Situation | Reply |
  | --- | --- |
  | an event is stored | 0 and the three data words |
  | inbound mailbox not empty | `CELL_EBUSY` (the old contents are replaced by this word) |
  | key not bound, or its queue destroyed | `CELL_EINVAL` |
  | `0x110` in a group of type `0x08`/`0x18` (no scheduler context) | `CELL_EINVAL` |
  | `0x111`, no event | `CELL_EBUSY` |
  | `0x110`, no event | the thread waits; an event sent to the queue gives the first row |
  | `0x110`, the queue is destroyed with the force mode meanwhile | `CELL_ECANCELED` |

  An empty outbound mailbox at `0x110` or `0x111` is a fatal stop. The key is always taken from the mailbox,
  also when the answer is `CELL_EBUSY`; for `0x110` lv2 leaves it there on that path. A waiting thread is shown as
  `blocked in stop 0x110 sys_spu_thread_receive_event` in the census and the thread health, and counts as an
  indefinite wait; `stop 0x110: <n>` in the census counts the receives started, including one that still waits.
  Deviation: lv2 suspends the whole group while one of its threads waits in `0x110`, and refuses
  `sys_spu_thread_group_terminate` with `CELL_ESTAT` in that state. Here only the calling SPU thread waits (it
  is a host thread; the game's group has one thread), and terminate, `stop 0x101` of another thread and
  teardown end the wait: the thread leaves, as at any other halt. Group states such as waiting or suspended
  are not modelled.

### Floating point

The SPU's own floating-point rules are emitted as written (the full list of deviations is in
[Emitting C](emitter.md#what-is-and-is-not-implemented)):

- Single precision uses the extended range: an exponent field of 0 is zero whatever the other bits are
  (denormals are zero), exponent 255 is an ordinary number, results are truncated toward zero to 24 bits,
  and a result below the smallest normal number is +0 (every zero result is +0) while a larger one
  saturates to the largest magnitude with its sign.
- `frest` and `frsqest` return the exact reciprocal and reciprocal square root truncated to 24 bits, and `fi`
  returns its second operand, because the architecture does not publish the hardware's estimate tables. A
  reciprocal refined with `fi`, `fnms` and `fma` stays within the architected one-ulp bound but can differ
  from the hardware by an ulp.
- The FPSCR exception flags (single and double precision) are never set by operations.
- Double precision is host `double` arithmetic. Denormal operands are flushed to zero, every NaN result is
  the default quiet NaN, and only round to nearest is implemented.
- A directed rounding mode selected through FPSCR stops the run with `tox2: stop: spu: unhandled word
  0x<word> at ls 0x<site> on thread 0x<id>` at the double-precision instruction that would use it.
  `eboot_updatematrix` contains a routine at LS `0x0ef88` that rewrites FPSCR fields from its argument. No
  run of this game has selected a directed mode so far.

### Log lines and census

- Lines written by an SPU thread carry the prefix `[spu XXXXXXXX]` (the lv2 SPU thread id) instead of
  `[t XXXXXXXX]`.
- At start-up `tox2: spu: 16 images ok` reports that the SHA-256 of each emitted image matches the bytes in
  guest memory (a mismatch is a load error, exit code 4). A group logs `tox2: spu: group ... created`,
  `... initialized`, `start group ...` with a line per thread, segment, connected queue and bound port,
  then `thread ... started at <image> ls 0x<entry>` and, when the last thread has ended,
  `group ... stopped: cause <n>, status 0x<s>`.
- A stop on an SPU thread ends the process with exit code 3 and prints the stop line, then `jump_target`,
  `jump_site`, the trace ring of recent dispatches, the four words of every non-zero register and the LS
  map (which image occupies which LS range). The stop lines are `unknown target ls ... from ls ... (<image>
  or untracked)`, `code mismatch at ls ...`, `unhandled word ...`, `halt at ls ...`, the MFC and channel
  stops above, `stop 0x... not implemented`, the empty-mailbox stop of `0x110`/`0x111` and the
  `WrOutIntrMbox` stops.
- The census has a `census: spu` section after `threads` (see the run below): the thread count, the total
  number of dispatches and the number of unknown targets; one line per SPU thread with its state, the
  image it ran last and its LS address; one line per image that ran, with its dispatches and the number of
  distinct entries; the MFC counters (an extra line for list commands, PUTLLUC and fences when there were
  any); the number of reads of each channel; and the `stop` codes that were seen. The counters add up over
  all SPU threads of the run, also those of an earlier start of a group.

## What the current run reaches

The run below was made with the build of commit `e013aa8`, headless, from a hdd directory that already holds
the installed game data (`<scratch>` is any directory outside the repository):

```sh
build/release/bin/tox2_game --game-dir <game-dir> --elf generated/inputs/EBOOT.elf --libsre generated/inputs/libsre.reloc.elf --no-window --seconds 30 --hdd-dir <scratch>/hdd
```

It ends at the watchdog (exit code 5), with no SPU stop and no unknown dispatch target:

```
tox2: stop: watchdog 30 s
census: spu 5 threads, 87070 dispatches, 0 unknown targets
  thread 0x01000100 SpursTLGraphicsCellSpursKernel0: blocked in rdch 0 SPU_RdEventStat libsre_system_service ls 0x013f4
  thread 0x02000100 SpursTLGraphicsCellSpursKernel1: blocked in rdch 0 SPU_RdEventStat libsre_system_service ls 0x013f4
  thread 0x03000100 SpursTLGraphicsCellSpursKernel2: running eboot_edgelzma ls 0x03688
  thread 0x04000100 SpursTLGraphicsCellSpursKernel3: blocked in rdch 0 SPU_RdEventStat libsre_system_service ls 0x013f4
  thread 0x05000100 SpursTLGraphicsCellSpursKernel4: blocked in rdch 0 SPU_RdEventStat libsre_system_service ls 0x013f4
  image eboot_edgelzma: 29569 dispatches, 28 distinct entries
  image eboot_animation: 309 dispatches, 57 distinct entries
  image eboot_updatematrix: 154 dispatches, 61 distinct entries
  image eboot_renderparticle: 90 dispatches, 12 distinct entries
  image eboot_morph: 47 dispatches, 10 distinct entries
  image eboot_skin: 37 dispatches, 9 distinct entries
  image libsre_kernel1: 8407 dispatches, 4 distinct entries
  image libsre_system_service: 14926 dispatches, 15 distinct entries
  image libsre_taskset_pm: 33531 dispatches, 40 distinct entries
  mfc get: 12006, put: 6458, getllar: 67982, putllc: 26075 ok 41879 failed
  channel rdch 0 SPU_RdEventStat: 25420
  channel rdch 11 SPU_RdEventMask: 994
  channel rdch 12 MFC_RdTagMask: 994
  channel rdch 13 SPU_RdMachStat: 1252
  channel rdch 24 MFC_RdTagStat: 15134
  channel rdch 27 MFC_RdAtomicStat: 135936
```

The counts differ from run to run; the shape does not. In this run:

- libsre builds SPURS instance A and its handler thread starts the group of five kernel threads. The
  kernels (`libsre_kernel1`) load the system service module and the taskset policy module, which loads the
  tasks. The log shows `first image` lines for `libsre_kernel1`, `libsre_system_service`,
  `libsre_taskset_pm`, `eboot_edgelzma`, `eboot_skin`, `eboot_animation`, `eboot_updatematrix`,
  `eboot_renderparticle` and `eboot_morph`. `eboot_spline` has not run, and `eboot_bnuscore` runs only in later builds (below). Four kernels
  idle in the system service (`RdEventStat`), one runs an Edge LZMA task.
- The game loads its archive: the Edge LZMA task inflates the first archive item, and `--dump-guest
  0x31d50020:0x3073c:<file>` logs SHA-256 `f4ad1f7125d9b31abf228e8043587aebc8f1501f0f4debed6d9814d06a12c751`,
  the oracle value, in every run (from an empty hdd directory too). The work counter at `0x01127aa0` is
  0 at a quiescent point and 1 to 3 while a batch is in flight, so a dump at an arbitrary stop can show
  either. The same inflate also runs without the game in the `spu_lzma_inflate` test (see
  [Tests](tests.md)).
- The engine tasks (skin, animation, update matrix, render particle, morph) are created, started and
  dispatched, and none of them stops at an unhandled word.
- The main thread passes the LZMA load and writes `tox2: first hle cellSpurs::cellSpursEventFlagTryWait`
  (at 12 to 14 s with installed data, 14 to 16 s from an empty hdd directory). This line is the
  `game_boot` milestone. The game then runs its frame loop: about 8,200 flips and 7,800 `cellPadGetData`
  calls in the 30 s run.

Longer runs: with `--seconds 180` the run ends at the watchdog (`tox2: stop: watchdog 180 s`) and its census
says `census: spu 5 threads, 174337514 dispatches, 0 unknown targets`. An earlier recovery left the `bctr`
at `0x0092d178` (function `0x0092d0c8`) unresolved, and the run stopped there at about 75 s with
`tox2: stop: fatal unknown target: target 0x0092d1a4 from 0x0092d178`; that was a gap in the PPU function
recovery, not an SPU problem.

The bnusCore thread runs in the builds after that run. Its `sys_spu_thread_initialize` call used to fail
because the SPU number (1) was checked against the group's thread count (1); the number is now checked
against the 256 entries of lv2's per-group table, as RPCS3 does. The log shows the bound key
(`tox2: spu:   thread 1 key 0x6e754153 -> queue ...`), the thread starting at `eboot_bnuscore ls 0x000d0`, and its
first receive (`tox2: spu: thread 0x06000200 first stop 0x110 at ls 0x11e2c: key 0x6e754153: waits`). The
thread waits until the PPU sends the `STAT` command, receives it, and handles it. An earlier recovery then
stopped it:

```
tox2: stop: spu: unknown target ls 0x07900 from ls 0x0dae4 on thread 0x06000200 (eboot_bnuscore)
```

The jump is the indirect call `bisl $0,$80` at `0x0dae4`. `$80` is the result of the function at `0x0cd50`,
which looks up the id `0x564f4c50` ("VOLP") in a table of 28-byte entries at LS `0x15290` and returns a word of the
entry; here it is `0x7900`. The table is in the bss part of the data segment (the segment has 960 bytes of
file data from `0x14d00` and ends at `0x15810`); the function at `0x0cc40` copies the static records at
`0x14f20` and later into it, and the one at `0x0c770` registers those at `0x14d20` in the same way. Each record holds a 4-character id, sizes and
function pointers, so the code reaches `0x7900` through a register and no constant names it. The function
recovery now takes the words of the data segments that point at a function start as entries (source
`data`, see [Function recovery](function-recovery.md#on-the-spu-images)): 43 new functions in
`eboot_bnuscore`, among them `0x7900`. The site `0x0dae4` stays one of the image's 14 unresolved indirect
calls; only its targets became dispatchable. In a 300 s headless run (two runs, from installed `hdd` copies)
the census says `spu 6 threads, 0 unknown targets`, `eboot_bnuscore` has 3,084,088 dispatches through 154
distinct entries, and `bnusCoreMain` is blocked in `stop 0x110` (`sys_spu_thread_receive_event`) at the
watchdog. The audio census counts 55,806 blocks, 158 of them non-silent.

Open issues:

- Raw SPU thread groups and `stop 0x120` are not implemented. In the 120 s headless run, 4,017 of the
  22,071 audio blocks are non-silent (the ATRAC3plus sounds, decoded on the host); the 300 s run
  above had 158 of 55,806 before `cellAtracCreateDecoderExt` had a handler.
- SPURS instance B ("atrac3plus", one SPU, created for `cellAtracCreateDecoderExt`) gets its thread group
  and thread, but nothing starts the group; the host decoder makes that unnecessary.
- The spline task (image 1) runs, and its exit loads the taskset 2 exit handler. Both exit paths of the
  policy module call the same function (LS `0x1438`), which DMAs the blob to LS `0x10000` and jumps there
  with `bi $7`; the blob uses only PC-relative branches and returns with `bi $0` to the policy module. In a
  400 s headless run with a scripted new game, the log has
  `first image libsre_taskset2_exit at ls 0x10000..0x10670`, no unknown target, and the opening scenes
  (the event fight, the bedroom, a choice screen with L1 and R1 answers) are rendered. The exit code plugin
  has not been loaded in that run.
- The reservation race window above is documented, not closed.

## Entry points

- SPURS task entry at LS `0x3070` in images 1, 4, 5, 6, 7 and 8 (tasks 4 to 8 have run).
- SPURS task entry at LS `0x3090` in image 2 (runs).
- SPU thread entry at LS `0xd0` in image 3 (runs up to the unknown target above).
- Exit handler entry at LS `0x10000` in `libsre_taskset2_exit` and `libsre_exit_code_plugin` (the first runs).
- SPURS kernel entry at LS `0x818` (version 1, in libsre at `0x011f0380`): the first code every SPU thread
  of the SPURS group runs. The taskset policy module (libsre, `0x011f3680`) that the task images depend on
  is loaded by the kernel.
- Not in the executable or in libsre: the firmware ATRAC3plus SPU decoder (not needed if `cellAtrac`
  is implemented on the host).
- Not run: the version 2 kernel, the two job chain policy modules and the exit code plugin. The game does
  not use the first three. The exit code plugin is recovered and emitted (it loads at LS `0x10000` like the
  taskset 2 exit handler, which does run) but no run has reached its exit path. Its EA is the registry's
  assumption: the policy module reads it from LS `0x2fc0` at run time.

## References

The SPU decoder, emitter and semantics cases are written from these public documents. They are copyrighted by
IBM, Sony and Toshiba and are not included in this repository.

- *Synergistic Processor Unit Instruction Set Architecture*, Version 1.2, January 27, 2007: instruction formats,
  semantics and the interrupt and channel instructions. A copy is at
  <https://arcm.csc.ncsu.edu/~mueller/cluster/ps3/SDK3.0/docs/arch/SPU_ISA_v1.2_27Jan2007_pub.pdf>.
- *SPU Assembly Language Specification*, Version 1.7: assembler mnemonics and operand syntax. A copy is at
  <https://support.scinet.utoronto.ca/Manuals/Cell/SPU_Assembly_Language_Specification_1.7.pdf>.
