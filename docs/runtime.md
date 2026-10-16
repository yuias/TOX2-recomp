# Runtime internals

## Logging

Log lines go to stderr, and to `--log FILE` when given, one line each. The logger is a thin layer
over spdlog (the `externals/spdlog` submodule, built as a static library with `std::format`
instead of the bundled fmt); spdlog types do not appear in the runtime's own headers. A line is
prefixed with `[host]`, with `[t XXXXXXXX]`, the id of the guest thread that wrote it, or with
`[spu XXXXXXXX]`, the id of the SPU thread, followed by `warn: ` or `error: ` for those levels. The runtime's own messages then start with `tox2: ` and a
topic (`tox2: fs: ...`, `tox2: game: ...`, `tox2: pad: ...`); the census lines and the register dump
of a fatal stop have no such prefix. Output from the guest itself (`sys_tty_write`) goes to stdout,
unprefixed (the game's own assertion text appears there).

With `--log-level debug`, a host thread logs `tox2: perf: t <s> s: <body>` lines once per second
(`runtime/src/core/perf_log.*`; `t` is the guest system time in seconds). Five sources write them, each with the
changes over the last second: `audio` (flips, `cellAudio` periods and unwritten port blocks), `game` (the game's
frame time, frame interval, frame counter and job counters), `rsx` (where the RSX thread's time went per flip, as
`rsx busy <x> ms/flip: lock <x> pump <x> build <x> record <x>, idle <x>`; see [Fake RSX](rendering.md#fake-rsx)),
`lock` (the wait and hold per class of the guest lock, which is the kernel lock in `--ppu-threads 1|2`, for the audio-path classes the queue length and the holders
they waited behind, and the yield outcomes, mutex hand-offs, long holds and file transfers) and `spu` (the receive timing of each SPU thread that blocks in `stop 0x110` and how the
reservation waits ended). A source with nothing to say in that second writes no line. The audio, lock and `spu`
fields are listed under [Audio](audio.md#log-lines); one second of those lines puts a missed audio block next to
the lock waits and the sound engine's processing time of the same second.

A run starts with the load log, for example (TOX2 values):

```
[host] tox2: sfo: TITLE_ID BLJS10188 RESOLUTION 0x15 SOUND_FORMAT 0x107
[host] tox2: load: phdr 0 type 0x1 flags 0x00400005 vaddr 0x00010000 filesz 0x103a828 memsz 0x103a828 ...
[host] tox2: load: libsre.reloc.elf phdr 0 type 0x1 flags 0x00400005 vaddr 0x011d0000 paddr 0x0001dd4c filesz 0x2e000 memsz 0x2e000
[host] tox2: spu: 16 images ok
[host] tox2: dispatch: 29369 entries, span 0x00010200..0x00c9098c
[host] tox2: dispatch: 384 entries, span 0x011d0000..0x011edafc
[host] tox2: hle: 173 imports, 127 with handlers, 30 bound to libsre
[host] tox2: hle: libsre: 32 imports, 30 with handlers
[host] tox2: loaded EBOOT.elf: entry 0x00010240 toc 0x010b6330 opd 0x0105ddb8 ... image end 0x011d0000
[host] tox2: sha256 a1d8a56ce1d6300bff704a43218f1b8b87c80c1e1342929a266e58af4e5891e9
[host] tox2: loaded libsre.reloc.elf: module cellSre_Library 1.1 base 0x011d0000 toc 0x01209ab0 image end 0x01210000
[host] tox2: libsre: sha256 4fc712e6967da90a4657cb8893dbaa3b58c27c3fc3a834e3edea07c5ffc2d7ca
[host] tox2: libsre: layout ok
[host] tox2: boot: stack 0xd0001000..0xd0011000 entry 0x00010240 toc 0x010b6330 r1 0xd0010f40 ...
```

The executable is loaded first and libsre directly above it (`loadModule`, `runtime/src/core/elf_loader.*`),
before anything else takes memory from the main region. The loader accepts only a relocated PRX
(`e_type` `0xFFA4`, no unapplied `0x700000A4` relocation segment) whose lowest segment is at the
address the layout header expects, and it logs one `tox2: load: libsre.reloc.elf phdr ...` line per
program header. The start-up then checks libsre's SHA-256 against the one in the generated code and the
structural facts of the layout header (see [libsre](#libsre)); a failure of either is a load error with
exit code 4.

## Census

On every stop the runtime prints what the game did, sorted by count and then by name:

```
census: syscalls 120954 calls, 15 distinct, 0 default-handled
  sc 43 sys_ppu_thread_yield: 99096 [impl]
  sc 45 sys_ppu_thread_detach: 2 [impl]
  sc 169 sys_spu_initialize: 1 [impl]
census: hle 129493 calls, 79 distinct, 22 default-handled
  sysPrxForUser::sys_lwmutex_lock: 61939 [impl]
  cellSpurs::cellSpursInitializeWithAttribute: 1 [libsre]
  cellSpurs::cellSpursCreateTask2: 25 [default] ptr r3,r4,r5,r6,r7,r8,r9
  cellSysutil::cellSaveDataListAutoLoad: 5 [impl]
census: unknown targets 0 distinct
census: rsx 939 headers, 172 distinct methods, 5 flips
  interrupt handler calls: vblank 3, flip 5, user 0
  method 0x1efc: 105
census: rsx unimplemented 1 kinds
  state zpass report constant: 9
census: threads 8 created, 5 live
  0x01000000 main_thread: running
  0x01000002 _gcm_intr_thread: blocked in gcm interrupt wait [service]
  0x01000004 CThread: running
```

This is an excerpt of the census of headless runs of this game (see [Status](status.md)); the
counts differ a little from run to run. A syscall number prints `?` when the runtime's table has no
name for it: that is 988, which `config/lv2_syscalls.json` (RPCS3's table) does not name either.

`[impl]` marks a call that an implemented handler served, `[libsre]` one that ran recompiled code of
libsre through a bound import (see [libsre](#libsre)), and `[default]` one that the default handler took
(see below). The excerpt above mixes lines of several runs. An unknown target is printed as `0x<target> from 0x<site>: count`. A default-handled import is
followed by `ptr r3,r5,...`, the argument registers that held a pointer to committed guest memory
(an out-pointer candidate, see [Default handlers](#default-handlers)). The `rsx` section is described under [Fake RSX](rendering.md#fake-rsx);
the `, rendered ... draws, skipped ...` part appears when the renderer runs, and the
`rsx unimplemented` section under [Rendering](rendering.md#rendering).
After a run that played movies through cellSail, a `census: sail <n> movies` section repeats the
per-movie summary lines (see [Movies](movies.md#log-lines-and-census)); this game imports no
cellSail, so the section does not appear. After a run in which the game
initialised `cellAudio`, a `census: audio` section gives the audio engine, mixer input and
`cellAtrac` counters (see [Audio](audio.md#census)). After a run that started an SPU thread group, a
`census: spu` section follows `census: threads`: it counts the dispatches, unknown targets, MFC commands
and channel reads of all SPU threads and gives one line per thread and per image that ran (see
[SPU usage](spu.md#log-lines-and-census) and the run quoted under
[What the current run reaches](spu.md#what-the-current-run-reaches)).
Thread states are `running`, `blocked in <what>` or `exited`. A `census: guest lock` section follows the
thread lines once the lock was used:

```
census: guest lock 3781234 acquisitions in 60.0 s, hand-off n 1480 avg 0.01 p99 0.02 max 0.31 ms >0.10ms 4, wakeups 2210 (131 spurious), yield calls 99096 ok 46 host 2100 kept 95400 backoff 1550, ppu overlap 5.0 of 12.0 s
  ppu: 46800.0/s, wait n 2808000 avg 0.02 p99 0.11 max 3.10 ms >1.00ms 12, ahead avg 1.3 max 5, hold n 2808000 avg 0.01 p99 0.04 max 1054.00 ms >1.00ms 1, waited behind rsx 120.0 ms ppu 80.0 ms
  rsx: 14600.0/s, wait ...
census: process cpu user 95.2 s kernel 14.8 s
```

(Invented numbers from a `legacy` run, for the shape.) The header gives the acquisitions since the start, the hand-off statistics, the
wake-ups, the outcomes of `sys_ppu_thread_yield` (calls, `ok` for a call that returned `CELL_OK`, `host` for a
`CELL_CANCEL` that passed the lock to queued host-class waiters and back, `kept` for a `CELL_CANCEL` that kept the
lock, and the backoff sleeps; see [Threads](#threads)) and, in `legacy`, `ppu overlap`: the time PPU threads held the lock, and the
part of it during which another PPU thread was queued, which is what a second PPU hardware thread could have run.
In `1` and `2` the header has no `ppu overlap`, the counts are those of the kernel lock, `host` stays 0 and the
`census: ppu slots`, `census: ppu kernel wait by call` and `census: ppu reservations` lines follow the class lines
(see [Threads](#threads)).

Each class line gives its acquisitions per second, the wait statistics, the queue length ahead of a
contended acquisition, the hold statistics and, as ms summed over the waiters, the holder classes its waits were
spent behind (see [Threads](#threads)). A statistics group is `n <count> avg <x> p99 <x> max <x> ms >T <count>`:
milliseconds, and the number of values above the threshold (1 ms for waits and holds, 0.1 ms for hand-offs). The
`process cpu` line after the class lines is the CPU time of the whole process (`GetProcessTimes`).
The `census: spu` section has the reservation-table and per-thread receive lines described under
[SPU usage](spu.md#log-lines-and-census). The first call of each syscall and
each import also logs `tox2: first sc <number> <name>` or `tox2: first hle <module>::<name>` while
the game runs; these lines are the progress markers that `game_boot` checks (see
[Tests](tests.md#the-game_boot-test)).

## Memory map

The guest space is one reservation of 4 GiB plus a guard area, big-endian, accessed only through
the `tox2Read*` and `tox2Write*` helpers. Pages are committed on demand by the allocators below. An
access to an uncommitted page raises a fault that the runtime turns into a fatal stop with the
guest address and the access kind.

| Range | Use |
| --- | --- |
| `0x00010000..0x10000000` | Main region: the ELF image (committed at its segment addresses), the libsre image (`0x011D0000..0x01210000`, directly above it), per-thread TLS blocks and the heap handlers' allocations. 64 KiB granularity. |
| `0x10000000..0x20000000` | RSX context area, fixed by the console memory map: the default command buffer context (`0x10000000`), the DMA control and `put`/`get`/`ref` registers (`0x10100000`, `0x10100040`), the driver info block (`0x10200000`) and the reports and semaphores (`0x10300000`). The first 4 MiB are committed when the game initializes GCM. |
| `0x20000000..0x30000000` | User memory with 1 MiB pages (`sys_memory_allocate` with flags 0x400 or 0). |
| `0x30000000..0x40000000` | User memory with 64 KiB pages (`sys_memory_allocate` with flags 0x200). |
| `0xC0000000..0xCF900000` | RSX local memory (video memory), committed when the game initializes GCM. |
| `0xD0000000..0xE0000000` | Thread stacks, 4 KiB pages, with an uncommitted 4 KiB guard page on each side of every stack. |

The top 64 KiB, `0xFFFF0000..0xFFFFFFFF`, are never committed: they hold the entry keys of host
functions, the function descriptors that the HLE layer hands to guest code (for example a callback
the runtime itself provides). Guest code can call or start a thread on such a descriptor, and the
dispatcher resolves the key to the host handler.

The primary stack size from the executable's process parameters is clamped as RPCS3 does, between
0x10000 and 0x100000 bytes; this game asks for 0x10000, so the stack is `0xd0001000..0xd0011000`. The argument block (`argv`, `envp`, strings) sits at the top of
the stack. `sys_memory_get_user_memory_size` reports a 0xD500000 container, the default one: the
primary stack, the executable's image and the libsre image (`0x40000` bytes) are charged to it at boot, `sys_memory_allocate` charges what it hands
out, and a memory container (below) takes its whole size from it when it is created. The timebase
runs at 79.8 MHz, scaled from the host performance counter.

## Dispatch

`tox2RegisterFuncs` builds a flat table indexed by guest address from the generated function
mappings, so a computed jump or an indirect call costs one lookup. Each loaded image has its own table
(a span): `appendFuncs` adds the functions of libsre as a second one, because one table over both
images would also cover the executable's data between them. For this game the executable's table has
29,369 entries over the span `0x00010200..0x00c9098c` and libsre's has 384 entries (352 functions and
32 import trampolines) over `0x011d0000..0x011edafc` (`tox2: dispatch:` lines in the load log). A span
that overlaps an existing one is a fatal error. Lookups, the search for the function that owns an
address and the wording of unknown targets ("inside a code span", "outside the code spans") go over the
list of spans. A target that is not in any table is an unknown target: the first occurrence of each (target, call site) pair is logged with
the call trace and counted in the census. With `--strict` it is a fatal stop. Without it the
dispatch returns, which acts like a callee that returned immediately, so the run goes on and the
census lists every such target.

## Threads

Each guest thread is a host thread (16 MiB host stack) with its own context. `--ppu-threads legacy|1|2` (default `2`)
selects how guest code is serialised:

| Mode | Guest code runs under | PPU guest threads in parallel | RSX, audio and SPU callbacks |
| --- | --- | --- | --- |
| `legacy` | the guest lock, held for as long as the thread runs guest code | never run in parallel | exclude PPU guest code, because they take the same lock |
| `1` | one PPU slot; the kernel lock only inside syscall and HLE handlers | no, one slot | run concurrently with PPU guest code |
| `2` (default) | two PPU slots, like the two PPU hardware threads of the PS3; the kernel lock only inside handlers | up to two | run concurrently with PPU guest code |

`legacy` is the original model: one global guest lock is both the right to run guest code and the mutex for all lv2,
HLE and runtime state, so in this mode PPU guest threads never run in parallel. A thread gives up the lock only in
`tox2Yield` (called from loop hooks, when another thread is waiting), in a blocking wait and when it exits. Blocking
system calls (mutex, condition variable, event flag, event queue, sleep, join) wait on host condition variables with
the lock, and record what they wait for for the census and for stall detection.

`1` and `2` separate the two jobs. The right to run is a PPU slot (`PpuSlots`, `runtime/src/core/threads.*`, capacity 1
or 2). A guest thread holds a slot whenever it runs guest code or is inside a syscall or HLE call, and gives it up where
`legacy` gives up the lock: a blocking wait, a yield, a budget expiry, a timed sleep and exit. Slots go to the waiting
thread with the lowest lv2 priority number, then to the one that queued first, using the priority it had when it
queued. There is no alternation (the slots are the two hardware threads, so the two best ready threads hold them and a
thread ranked third waits) and no displacement of a holder. The guest lock instance becomes the kernel lock: a guest
thread takes it only inside a kernel section, which is the run of an implemented syscall or HLE handler, a registered
host function (the GCM interrupt thread and sail service bodies) or the default handler; guest code, including
recompiled libsre code, runs without it. Host threads (RSX, audio, SPU owner callbacks, boot) never take a slot and
take only the kernel lock, as before. The lock order is slot, then kernel lock, then everything else, and a thread
never waits for a slot while it holds the kernel lock. A blocking wait releases the kernel lock and then the slot, and a
woken thread takes its slot first and then the kernel lock. A handler that calls guest code (`callGuestFunction`, for
example a sysutil callback) leaves the kernel section for the duration of the call; a call from a thread that holds no
slot is a fatal internal error. `1` has no PPU-to-PPU parallelism and exists to tell problems apart: one seen only in
`2` is a race between two PPU threads, one seen in `1` comes from the host threads running beside guest code or from a
missing kernel section. The mode is logged at start-up as `tox2: ppu threads: <mode>`.

A syscall or import can be marked kernel-free and then runs without a kernel section in `1` and `2` (in `legacy` it
still runs under the lock). Such a handler touches only the thread's context and thread-locals, guest memory, atomics
and the commit map, and does not block, allocate guest memory or touch other lv2 or HLE state; the one exception to
not blocking is a timed sleep with no waker. In use are `sys_ppu_thread_yield`, `sys_timer_usleep` and
`sys_timer_sleep` (a sleep releases its PPU slot while it waits, so a sleeper does not hold one of the two slots;
`sys_timer_usleep(0)` is a yield), `sys_time_get_timebase_frequency`, the `cellGcm` handlers that read only atomic RSX
state (`cellGcmAddressToOffset`, `cellGcmGetLabelAddress`, `cellGcmGetControlRegister`, the flip status, mode and
command handlers, the time stamp and the tiled pitch), the string and memory functions of sysPrxForUser
(`_sys_memcpy`, `_sys_memset`, `_sys_strlen` and the other `_sys_str*`, `_sys_snprintf` and `_sys_vsnprintf`),
`sys_time_get_system_time`, `sys_ppu_thread_get_id`, `cellRtcGetCurrentTick`, `cellRtcConvertUtcToLocalTime`,
`cellSysutilGetBgmPlaybackStatus`, `cellVideoOutGetResolution` and `cellVideoOutSetGamma`. The lightweight mutex
calls `sys_lwmutex_lock`, `sys_lwmutex_trylock` and `sys_lwmutex_unlock` are kernel-free in a different sense: they
do use lv2 state, but an uncontended call is a compare-and-swap on the guest struct and needs no kernel lock, and a
call enters a kernel section itself only when it finds the lwmutex owned by another thread (lock) or finds waiters
(unlock); see the hand-off paragraph below. With `--lwmutex-fast-path off` they run inside one kernel section like
every other handler. Every other handler holds the kernel lock, including `sys_lwmutex_create`, `sys_lwmutex_destroy`
and the `sys_lwcond_*` calls; see the measurements below for what the kernel lock still costs.

Running two guest threads at once exposed a race in the game's own ATRAC stream code: its loader and the decoder setup
of the player thread could interleave and misalign the stream. Two guest functions are replaced by hand-written
versions that close it (see [Overrides in use](emitter.md#overrides-in-use)).

The waiters of the kernel lock (or, in `legacy`, of the guest lock) sit in a list of nodes on their own stacks, each
with its own condition variable, and an unlock hands the lock directly to one of them and wakes only that thread. The
holder cannot take it back first. Host-class waiters (`rsx`, `audio`, `spu`, `other`) are served in arrival order. PPU
waiters (guest threads, including the GCM interrupt thread) fill the "PPU turns" of that order: when the oldest queued
entry is a PPU thread, the turn goes to the PPU waiter with the lowest lv2 priority number (0 is the highest
precedence), and among equal priorities to the one that queued first, which is not necessarily the oldest waiter. A
waiter uses the priority it had when it queued. The PPU turns are unrelated to the PPU slots above. On lv2 two threads
run at once and the rest are suspended; in `legacy` one lock serves both, so the PPU thread that held the lock last
gives the turn to the second-ranked PPU waiter when there is one. The two best ready threads therefore alternate, and a
thread ranked third waits while two better ones stay ready. The split modes do not alternate on the kernel lock, because
the two slots already model both hardware threads. A thread that gives up the lock for a blocking wait, a budget expiry
or an exit is not queued at that moment, so it cannot win the turn it just gave up. "Ready" means queued for the lock
(in `1` and `2`, for the slot): a thread woken from a condition variable counts only once it has queued.

The runtime does not displace the holder when a better thread becomes ready. That thread waits for the holder's next
release (in `1` and `2`, the next release of a slot): a blocking wait, a yield, an exit, or the end of its preemption
budget (4096 loop back edges and calls).
`sys_ppu_thread_set_priority` and `sys_ppu_thread_get_priority` only store and return the value; there is no
rescheduling, and a new priority applies the next time the thread queues for the lock or is chosen from a mutex or
lightweight mutex wait list. Priority inheritance is not implemented (`SYS_SYNC_PRIORITY_INHERIT` is served like
`SYS_SYNC_PRIORITY`), and the wake order of condition variable, semaphore, event flag and event queue waiters is not
by priority.

The creation rule of `sys_ppu_thread_create` follows from this: a new thread whose priority number is
lower than the stored priority of its creator (lower number means higher precedence on lv2) runs
before the creator continues. `startGuestThreadFirst` starts the host thread, waits until it has
queued for the guest lock (at most 5 ms), and yields the lock to it. The new thread then runs until
it blocks, exits, yields or uses up its preemption budget; the creator's budget is reset afterwards.
A thread with an equal or higher number, an interrupt thread, and the threads the runtime itself
creates are started as before. The earlier game depended on this rule: its audio mixer thread was
created by a main thread of lower precedence and had to store its notify queue key before main
registered it with `cellAudio`. This game depends on it too: its main thread has priority 1001 and it
creates SPURS with PPU priority 1000, so libsre's helper threads `SpursHdlr1` and `SpursHdlr0` run
before the main thread continues, and `SpursHdlr0` starts the SPU thread group of the SPURS kernels
and then waits in `sys_spu_thread_group_join` (see [Status](status.md)). The rule is logged at
debug level, and a new thread that did not queue within 5 ms
logs `tox2: thread: ... not queued within 5 ms; creator continues` at info level.

In `1` and `2` the creator waits for the new thread to be queued for a slot in the same way (at most 5 ms, counted by the
slots' arrival count), then drops the kernel lock it holds inside `sys_ppu_thread_create`, releases its slot and waits
until the new thread reaches its first release point, which the new thread marks whether or not a slot changed hands: a
blocking wait, a yield, a timed sleep, the end of its preemption budget or an exit. Only then does the creator take a
slot and the kernel lock again. With a free second slot the new thread may start at once, so the wait is bounded by the
new thread's first preemption budget instead of by the queue. This keeps the guarantee of `legacy` that the new thread
runs up to its first release point before the creator continues, which is stricter than lv2, where both would run at
once on two hardware threads.

`sys_ppu_thread_yield` (`yieldPpuThread`) returns lv2's result: `CELL_OK` only when a PPU thread of equal or higher
priority (a priority number less than or equal to the caller's) was queued, and that thread has run before the call
returns; otherwise `CELL_CANCEL`. A lower-priority thread never gets time from a yield. Queued host-class waiters
(RSX, audio, SPU callbacks) are still served on a `CELL_CANCEL`: on the console they run on their own hardware, so
serving them is not a guest context switch. The lock goes to the oldest of them, and the caller queues again behind
the host waiters that were queued before it and ahead of any lower-priority PPU waiter, with its position among its
priority peers unchanged, as a yield without a switch does not rotate. The back-edge preemption (`tox2Yield`) and
the creation rule instead release the lock and queue again by the ordinary rules above, so a lower-priority thread
can run there when no better one is queued.

In `1` and `2` the yield decides at entry whether a PPU thread of equal or higher priority is queued for a slot. If so,
it releases its slot and takes one again, which hands the slot to that thread, and returns `CELL_OK`; if not, it returns
`CELL_CANCEL` and keeps its slot. Host-class threads do not wait for slots, so there is nothing to serve on a
`CELL_CANCEL` and the `host` outcome stays 0. With two slots the answer is a hint: the other slot holder can take the
queued entry between the check and the release. A thread whose equal-priority partner is running on the other slot gets
`CELL_CANCEL`, because there is nothing to switch to, which is also RPCS3's result when both PPU threads are running.
The call needs no kernel lock.

The yield also has a host-side backoff (`--yield-backoff on|off`, checked before the rules above). The game's main
thread and render thread hand frames to each other by spinning on lock, check, unlock and yield, and ignore the
result; on the console they run on separate hardware threads, so the spin is free. Here each yield would be a guest
lock hand-off that the RSX thread pays for, and the spinning thread would keep a core busy. A thread whose yields
arrive within 50 us of the previous one's return is spinning; every 64th such yield releases the lock, sleeps 100 us
with the high-resolution timer (about a millisecond in practice) and takes the lock again. During the sleep the lock
is free, so any waiter may run, including a lower-priority PPU thread. The result is the one the rules above give at
the time of the call. The blocking waits are not affected. Without the backoff, 140 s runs of this game used about
1.8 times the process CPU time and the RSX thread waited about 1.3 times as long for the lock per flip, so it stays on
by default. The census header and the perf line count the outcomes of every call: `ok`, `host` (a `CELL_CANCEL`
that passed the lock to host waiters and back), `kept` (a `CELL_CANCEL` with nobody to serve) and the backoff
sleeps.

In `1` and `2` the backoff releases the thread's slot (not the kernel lock) for the sleep and takes a slot again
afterwards, with the same parameters. With the backoff off, a 140 s run with `--ppu-threads 2` used about 1.7 times
the process CPU time and the RSX thread waited about 1.7 times as long for the kernel lock per flip, so it stays on in
that mode too.

A mutex or lightweight mutex hands its ownership to one waiter when it is released. Each object keeps its waiters in
a list in arrival order. The unlock (`sys_mutex_unlock`, `sys_lwmutex_unlock`, and the release inside
`sys_cond_wait` and `sys_lwcond_wait`) picks the waiter by the object's protocol: the longest waiter for
`SYS_SYNC_FIFO`, otherwise the lowest priority number and, among equals, the longest waiter. The priority is the
waiter's current one. The chosen waiter owns the object from that moment: a mutex's owner becomes its thread id, and
the owner word of a lightweight mutex holds the reserved value until the waiter resumes and writes its id. The
unlocking thread, and any other thread, then sees an owned object, so it blocks (or gets `EBUSY` from a try-lock)
instead of taking it back first. A waiter whose time-out expires after it was chosen still owns the object and the
call returns `CELL_OK`. `SYS_SYNC_RETRY` lightweight mutexes are served like `SYS_SYNC_PRIORITY`. This differs from
lv2 and RPCS3, where a retry-protocol unlock frees the lock and the woken waiter tries again, so another thread may
take it first; the hand-off here is stricter and cannot deadlock. The perf `lock` line counts the waits and
hand-offs (`lwmutex wait <n> handoff <n> mutex wait <n> handoff <n>`; every wait of a run of this game ended in a
hand-off).

The state of a lightweight mutex is the `owner` and `waiter` word pair at the start of the guest `sys_lwmutex_t`,
which the runtime reads and writes as one 8-byte unit (`runtime/src/lv2/lwmutex_word.h`: sequentially consistent
compare-and-swaps and acquire loads on the doubleword). `{Free, 0}` is a free lwmutex with no waiter. An uncontended
`sys_lwmutex_lock` is one compare-and-swap `{Free, 0}` to `{thread id, 0}` and an uncontended `sys_lwmutex_unlock`
one `{thread id, 0}` to `{Free, 0}`, with no kernel lock; the recursion count is touched only by the owner, and
`sys_lwmutex_trylock` never leaves the compare-and-swap path. Only two cases enter a kernel section: a lock that
finds another owner, and an unlock whose compare-and-swap fails because a waiter is published. In the section the
lock queues its node and publishes the new waiter count together (or takes the lwmutex if the owner released it
meanwhile), and the unlock chooses between `Free` and the reserved value for the chosen waiter. Whenever the kernel
lock is free, the `waiter` word therefore equals the length of the object's wait list (a granted node still counts),
`Free` always comes with `waiter` 0, and the reserved value lasts exactly while a granted waiter has not resumed.
A fast lock succeeds only on `{Free, 0}`, so it never passes a queued or granted waiter; it can take the lwmutex
ahead of others only when nobody is queued. Because the waiter is published and queued in one section, the runtime
needs no stored wake-up token for an unlock that found no sleeper yet (RPCS3 keeps one for its user-side protocol).
`sys_lwmutex_destroy` stays a kernel handler and ends with a compare-and-swap `{Free, 0}` to `{Dead, 0}`, so a lock
that got in meanwhile makes it return `EBUSY`. The time-out and grant race and the `SYS_SYNC_RETRY` hand-off are
decided in the kernel section, as before.

A lightweight mutex whose address is not 8-byte aligned cannot use the atomic pair. Each of its calls runs inside a
kernel section with plain accesses, is counted, and the first call per address logs `tox2: lwmutex: <addr> is not
8-byte aligned; its calls use the kernel lock`. The process's own lightweight mutexes and all those of this game are
aligned.

The kernel lock used to give lwmutex users an ordering edge between the two PPU threads. The pair gives it now: a
successful lock compare-and-swap synchronises with the owner's releasing write of the pair (the unlock
compare-and-swap, or the reserved value written under the kernel lock followed by the granted waiter's update under
the same lock). Guest loads and stores inside the critical section are generated before and after the opaque HLE
call, which the compiler cannot move them across, and on x86-64 the locked operation orders them in hardware.

`--lwmutex-fast-path on|off` (default `on`) selects this behaviour. It stays as the fallback and the switch for
comparing runs: `off` runs the same protocol with every access inside one kernel section. When any call left the
compare-and-swap path in a second, the perf `lock` line ends with `lwmutex slow lock <n> unlock <n>` (and `misaligned
<n>` when non-zero). Uncontended calls count nothing there, so their number is the census call count minus these. In
140 s runs of this game about 0.017 % of the calls were slow (about 4,100 slow locks and 700 slow unlocks against 28.6
million calls). Slow locks outnumber the blocking waits (about 700, each with a hand-off) because roughly 3,400 of
them took the lwmutex inside the section without blocking, when its owner released it between the failed
compare-and-swap and the kernel lock.

From the reservation table's point of view the compare-and-swap is a plain host store: it moves no stripe version and
the PPU `stwcx.` compare is not atomic against it. That is harmless only while no generated code touches the words of
a lwmutex other than through the sysPrxForUser imports. `--lwmutex-watch` (default off) checks that: it registers
every lwmutex that is created and counts the PPU `lwarx`, `ldarx`, `stwcx.`, `stdcx.` and `dcbz` accesses, the SPU
`GETLLAR`, `PUTLLC` (only the 8-byte chunks it changes), `PUTLLUC` and `PUT` accesses and the `_sys_memcpy` and
`_sys_memset` destinations that fall on a live struct, plus owner words that are neither a lwmutex state nor a thread
id and waiter words that disagree with the wait list. It logs the first 16 hits of each kind and prints a `census:
lwmutex watch` line; with the option off each hook costs one relaxed load. Runs of this game with it, 140 s and 420 s
with `--ppu-threads 2` and 140 s with `legacy`, found no hit of any writing kind (up to 55 lwmutexes live), and the
game neither issued the lv2 lwmutex system calls 95 to 99 nor contains a call site for them. The only hits were the
read-only `GETLLAR` of an SPU counter in the same 128-byte line as a lwmutex, whose `PUTLLC` writes only the
counter's own 8-byte chunk (see [SPU usage](spu.md)).

`tox2_game` holds a 1 ms system timer resolution for the whole run (`timeBeginPeriod`,
`runtime/src/core/timer_resolution.*`). Timed waits (`sys_timer_usleep`, lock and queue time-outs)
are host condition variable waits, and Windows rounds them to its timer tick (15.6 ms by default),
so a 1 ms guest sleep would take 15 ms. The earlier game's decode loops polled with 1 ms sleeps;
this game's main thread and a worker poll with `sys_timer_usleep` (about 21,000 calls in a 20 s run). The
request ends with the process; a refusal is logged as a warning and the default tick stays. In `1` and `2` a
`sys_timer_usleep` or `sys_timer_sleep` call is a plain host sleep with the slot released: nothing can wake it early.

SPU threads run in parallel with the PPU threads in every mode, which in `legacy` makes them the exception to "never
in parallel". Each SPU thread is a host thread that runs recompiled SPU code outside the guest lock (in `1` and `2`:
outside the kernel lock), in parallel with the PPU threads and with the other SPU threads. It takes that lock only
around the lv2 state its owner callbacks read or change (event sends, mailbox replies, group bookkeeping at a stop) and
never while it waits on a channel; the PPU-side calls that wait for SPU threads give the lock up while they wait. Atomic reservations are shared between
PPU and SPU threads (`lwarx`/`stwcx.` and `ldarx`/`stdcx.` use the same table as PUTLLC and reserve the whole 128-byte line), and the PPU `sync` is a full
host fence; see [SPU usage](spu.md#how-the-runtime-runs-spu-code).

The RSX thread and the audio engine are host threads that take the same guest lock (the kernel lock in `1` and `2`).
The RSX thread therefore pumps the command buffer in slices of 128 words and leaves the lock once a hold has lasted
300 us, so that the game's mixer can run between the slices (see [Audio](audio.md#guest-lock); this
game stops before it starts a mixer, so the figures come from the earlier game). It
also leaves the lock after each draw end and copies the draw's guest bytes without it; the game only
reuses that memory after a later label, report or flip, which the RSX thread writes after the copy.

In `legacy` no PPU thread runs guest code while the RSX thread or the audio engine holds the lock. In `1` and `2` they
do: the FIFO parse, the label, report and `get` writes and the flip run beside PPU guest code, as the RSX does on the
console, and a PPU thread waits for them only when it needs the kernel lock for a handler. The game can therefore see
a flip before the flip handler (the GCM interrupt thread) has run; in `legacy` the interrupt thread usually won the
lock at the RSX thread's release and ran first. The vblank handler still runs before the flip handler of the same
frame, since one queue and one interrupt thread serve both.

The guest lock keeps statistics, always on (`GuestLock::stats()`, `runtime/src/core/threads.*`; the cost of an
uncontended lock and unlock pair is about 56 ns). In `1` and `2` they describe the kernel lock, so a PPU hold is a
kernel section and not the time a thread runs guest code. Each acquisition is attributed to a class: `ppu` and
`ppu-high` for guest threads (lv2 priority below 100 is `ppu-high`; the game's sound threads use 0), and for host threads
the class they name themselves with `setGuestLockClass`: `rsx` (the RSX consumer), `audio` (the audio engine's
producer call), `spu` (SPU threads in their lv2 callbacks) and `other` for the rest. Per class the lock records
the wait (queueing to turn, 0 when uncontended), the hold, how many waiters were queued ahead on contended
acquisitions, and, per holder class, the hold time that ended while the class waited. It also records the
hand-off time from an unlock to the turn of the next waiter and the wake-ups that found it was not their turn
(each waiter has its own condition variable and only the chosen one is woken, so these are rare). In `legacy` the lock
also adds up the time PPU threads held it (`ppu overlap` in the census header and the perf `lock` line) and the part of
it during which another PPU thread was queued; in runs of this game that was 41 to 43 % of the PPU hold time, the
share that a second PPU hardware thread could have run. The audio-path waits the lock
produced were not the cause of missed audio blocks (see [Audio](audio.md#guest-lock)). Host threads that are raised
above normal priority are listed under [Audio](audio.md#host-thread-priorities).

In `1` and `2` the census omits `ppu overlap` and adds two lines after the class lines:

```
census: ppu slots 2: held 36.4 s, parallel 7.4 s, full with waiters 3.2 s, wait n 2230000 avg 0.00 p99 0.01 max 2.38 ms >1.00ms 3 high n 0 avg 0.00 p99 0.00 max 0.00 ms >1.00ms 0
census: ppu kernel wait by call: lwmutex 10.0 ms 1990, timer 0.0 ms 0, gcm 11.0 ms 11, other 770.0 ms 75700, none 284.0 ms 25000
```

(Invented numbers, for the shape.) `held` is the slot-seconds of completed holds; a wait for the kernel lock inside a
handler counts as held, because the thread still has its slot. `parallel` is the wall time with two slots held, `full
with waiters` the wall time with every slot held and at least one thread queued for one, and the two `wait` groups are
the slot wait of `ppu` and `ppu-high` threads. The perf `lock` line has `slots par <x> ms full <y> ms` for the same
two times over the last second (the `lwmutex slow` field described above comes after it). `ppu kernel wait by call`
splits the contended kernel-lock acquisitions of guest threads by the call that waited (`sys_lwmutex_*`, the two timed sleeps, `cellGcm*` and `_cellGcm*`, any other call,
and none) as the waited ms and the number of waits. In both modes the census has a `census: ppu reservations lwarx <n>
stwcx <n> failed <n>` line and the perf `lock` line a `stwcx <n> fail <n>` field with the per-second counts; `lwarx` and
`stwcx` count the doubleword forms too, and `failed` includes store-conditionals that fail before reaching the
reservation table.

Measured on this game before the lightweight mutex fast path (`--lwmutex-fast-path off` reproduces these figures; its
lwmutex wait was within 6 % of the one quoted), 140 s and 420 s runs at the same revision and in the same session,
`--ppu-threads 2` against `legacy`: the RSX thread waited 0.27 to 0.30 ms per flip for the lock where `legacy` waited 2.00 ms, and its idle time
per flip rose by 2.6, 2.0 and 1.9 ms in the three heavy windows of the run, each above the spread between repeated
`legacy` runs. The flip rate was the same (60 flips per second in the first window and 30 in the others, capped by the
frame limit), the unwritten audio blocks were 0 against at most 1, process CPU time was 1.06 to 1.09 times that of
`legacy`, and the `stwcx.` failure rate was not higher. Both slots were held for 7.4 s of 140 s, and
for 3.2 s of those a third thread was waiting. Guest threads spent about 21.5 % of their slot time waiting for the
kernel lock, of which 64 % was in the lightweight mutex calls; the kernel-free handlers above lowered that share from
38.6 %, and the GCM handlers alone had been 40 % of the wait.

The fast path removed most of that wait. Same session, 140 s and 420 s runs, `--ppu-threads 2` with the fast path on
against `--lwmutex-fast-path off` (medians of two 140 s runs; the 420 s runs agree):

| Per 140 s | off | on |
| --- | --- | --- |
| kernel wait of PPU threads in the lightweight mutex calls | 5,350 ms (71 % of their kernel wait, 17.1 % of slot time) | 10 ms (1.0 %, 0.07 %) |
| kernel wait of PPU threads, all calls | 23.9 % of slot time | 7.05 % of slot time |
| contended kernel acquisitions of PPU threads | 1.63 million | 0.105 million |
| slot-seconds held | 31.4 s | 14.8 s |
| blocking lwmutex waits | 11,000 | 700 |
| kernel hand-offs, all classes | 2.1 million | 0.22 million |

Process CPU time fell from 1.00 to 1.06 times that of `legacy` to 0.91 to 0.95 times. In the two heavy windows capped
at 30 flips per second the RSX thread's idle time per flip was 0.54 and 0.62 ms longer than with the fast path off,
beyond the spread between repeated runs, and its wait for the kernel lock fell from 0.46 and 0.48 to 0.16 ms; in the
60-flip window the idle time did not change (0.05 ms shorter, inside the 0.21 ms spread), as that window is bound by
the RSX thread. The flip rate, the audio census (no underruns, no decode errors, at most 7 unwritten blocks in 420 s,
each a single block, against up to 13 in `legacy`) and the game's path through the title scene did not change. The game's yield-polling loops
iterate about 10 % more often per second, because less of their time goes into kernel waits; the backoff absorbs that
and the CPU time still falls. What remains of the PPU kernel wait is in other handlers (70 to 75 %) and in calls with
no label (23 to 27 %). The mean contended wait of a PPU thread rose from about 4.6 to about 10 us, because the many
short lwmutex waits are gone, and the 99th percentile of the kernel hand-off stayed at or below 0.02 ms.

A hold of 5 ms or more is counted and logged as a warning, so a stall can be traced to its holder:
`tox2: guest lock long hold: <x> ms holder <class> thread <id> prio <p> (<name>) at release: <a> last in hold: <b>
previous in hold: <c> cpu <t>`. A holder that is not a guest thread has no `thread ... (<name>)` part. The three
activity labels name the syscall or imported function the holder was running when it released the lock, the last one
it entered during the hold, and the one before that (a release inside a labelled call has the same last label, so the
previous label is the only trace of what ran before it); `none` means no label. For PPU holders `cpu` is the CPU
time the holder's thread used during the hold, so a hold far above its CPU time means the host descheduled it;
other classes print `n/a`. At most 20 such lines are logged per second, and the perf `lock` line counts all of them
(`long holds <n>`). In a second in which the file system calls read or write, that line also ends with `fs read <n>
<bytes> bytes max <x> ms write <n> <bytes> bytes max <x> ms`: the number of guest reads and writes, their bytes and
the longest host transfer, which runs under the guest lock (the kernel lock in `1` and `2`, so a long file transfer
stalls the RSX thread and the audio engine as it did before). In `1` and `2` a long hold is a kernel section; the
holds of about a second that `legacy` shows during boot are a thread that kept the guest lock while it ran guest code.

Reservations and memory ordering follow from the modes. `lwarx`/`ldarx` and `stwcx.`/`stdcx.` use the stripe lock, the
stripe version and a word compare, so two PPU threads that race on the same word are serialised as a PPU and an SPU
thread are, and each thread's reservation lives in its own context. A plain store by another thread to a different word
of the reserved line does not cancel the reservation, as with SPU threads today; between two PPU threads that window was
closed in `legacy` and is open in `2`. The same holds for `dcbz`, which is a 128-byte plain store, and for the bulk
stores that HLE handlers make for the calling thread (`_sys_memcpy` and `_sys_memset`, and the copies and fills of the
sail, audio, savedata and gcm handlers), none of which moves a stripe version. A rewrite of the reserved word with its
old value is not seen either (ABA), as before. Generated code reads and writes guest memory with plain accesses; on
x86-64 the compiler barrier of every `sync` kind gives `lwsync`, `eieio` and `isync` their ordering and a heavy `sync`
adds a host fence. A polling loop without a `sync` can keep a loaded value in a register until its next opaque call,
and the back-edge budget (4096 iterations) bounds that, as it already does for flags that SPU and RSX threads write.
Kernel sections give HLE-based synchronisation (mutex, condition variable, semaphore, event) an ordering edge between
the two PPU threads through the kernel lock. The lightweight mutex gets its edge from the owner and waiter pair, as
described with the hand-off above.

Two limits of `1` and `2` are accepted. When a fatal stop dumps the registers and prints the census, the other slot
holder keeps running, so the dump is not a consistent snapshot; a second failing thread waits in the stop and may hold
the kernel lock, so stop hooks and census providers must not take it, and none does. A handler that calls guest code
leaves the kernel section for the call, so another PPU thread can run handlers of the same module meanwhile (it can
also in `legacy`, when the callee blocks or its budget expires). The `cellPngDec` stream reader keeps references into
its handle and stream tables across the callback that supplies data, so a second thread that closes the same decoder
while it decodes would leave them dangling; this game does not do that.

Thread ids start at `0x01000000`. lv2 interrupt threads are not implemented; the one interrupt
thread the game needs is the GCM interrupt thread described under [Fake RSX](rendering.md#fake-rsx).

A service thread is a guest thread that the runtime starts on the game's behalf and that waits
for work from the host (the GCM interrupt thread, and `_libsail-control`, which delivers the
cellSail notify events and allocator calls; see [Movies](movies.md). The latter is never started for
this game, which does not import cellSail). The census marks it `[service]`, and the
stall detector and the check for "no guest threads left" ignore it, so a game whose own threads
are all blocked still ends with a stall stop.

The host main thread is the supervisor. After boot it checks every 100 ms for the watchdog, a
stall and the absence of live guest threads.

## Implemented system calls

Everything else goes to the default handler. Behaviour follows the RPCS3 handler of the same
name; struct layouts and error codes are restated from its headers. The SPU thread group calls, the
lightweight condition variable and the handlers that libsre needs were implemented from a written
description of RPCS3's SPURS and lv2 SPU behaviour and from the SDK-level layouts, without reading
RPCS3 code. The table lists what the runtime implements. The executable of this game issues 67 system
call numbers; the ones without a handler are the vm calls
`sys_vm_memory_map` (300) and `sys_vm_unmap` (301) and the file system calls 810 (link), 812
(rename), 813 (rmdir), 815 (utime), 820 (fsync), 831 (truncate), 832 (ftruncate) and 834 (chmod).
libsre has 177 system call sites with 41 numbers; of those 248, 249 and 253 have no handler
(248 and 249 have no name in `config/lv2_syscalls.json`, 253 is `sys_spu_thread_group_syscall_253`), and
none of them was reached in the runs so far. The game's sound engine issues `sys_spu_thread_bind_queue`
(193), which an earlier build left to the default handler (see
[SPU usage](spu.md#what-the-current-run-reaches)). The syscall table (`kSyscalls`) holds every number the game issues,
with its name from `config/lv2_syscalls.json`, so the log and the census name them whether or not
they have a handler.

Memory containers follow RPCS3's `lv2_memory_container`. `sys_memory_container_create` (324 and the
older number 341) rounds the size down to 1 MiB, fails with `CELL_ENOMEM` when the default container
cannot give that much, takes it from the default container (so `sys_memory_get_user_memory_size`
reports it as used) and writes an id from `0x3F000000` up; at most 16 containers exist at a time
(`CELL_EAGAIN`). `sys_memory_allocate_from_container` (350) charges the allocation to the container
only, `sys_memory_free` credits the container that owns the block, `_get_size` (343) reports
`{size, size - used}`, and `_destroy` (325 and 342) returns the memory to the default container and
fails with `CELL_EBUSY` while a block is in use. Creation logs `tox2: memory: container <id> created`.
The game creates a container of 0x5600000 bytes (86 MB), which leaves 0x4740000 in the default
container, and allocates from it twice. It had called `sys_memory_container_get_size` with id 0 while the container
create call did not write an id, and aborted; with the container calls implemented the abort is gone.

System call 988 is not in RPCS3's table. The game issues it at three sites, `sc 988(kind, 0)`, and
ignores the result: kind 1 at the start of its exit functions, which end with `sys_process_exit`,
and kind 4 at the start of the function that prints `abort() is called from 0x...` to the tty (the
abort path then continues into the exit function). It is accepted and ignored (`CELL_OK`). A run
that does not abort or exit does not issue it.

| Group | Calls |
| --- | --- |
| Process | `sys_process_getpid` (1), `sys_process_is_spu_lock_line_reservation_address` (14), `_sys_process_exit` (22), `sys_process_get_sdk_version` (25) |
| TTY | `sys_tty_write` (403); `sys_tty_read` (402) returns `CELL_EIO` |
| Time | `sys_timer_usleep` (141), `sys_timer_sleep` (142), `sys_time_get_timebase_frequency` (147) |
| Memory | `sys_memory_allocate` (348), `sys_memory_free` (349), `sys_memory_get_user_memory_size` (352); containers: `sys_memory_container_create` (324, 341), `_destroy` (325, 342), `_get_size` (343) and `sys_memory_allocate_from_container` (350); `sys_memory_get_page_attribute` (351) |
| Debug notification | System call 988, which is unnamed (see above) |
| Mutex and condition variable | `sys_mutex_create`, `_destroy`, `_lock`, `_trylock`, `_unlock` (100-104); `sys_cond_create`, `_destroy`, `_wait`, `_signal`, `_signal_all` (105-109). An unlock hands the mutex to the waiter its protocol chooses (see [Threads](#threads)) |
| Semaphore | `sys_semaphore_create` (90), `_destroy` (91), `_wait` (92), `_trywait` (93), `_post` (94), `_get_value` (114). Process-shared semaphores (an IPC key) are not supported; a waiter that blocks wakes in no particular order |
| Event flag | `sys_event_flag_create` (82), `_destroy` (83), `_wait` (85), `_set` (87), `_clear` (118), `_cancel` (132), `_get` (139) |
| Event queue and port | `sys_event_queue_create` (128), `_destroy` (129), `_receive` (130), `_tryreceive` (131), `_drain` (133); `sys_event_port_create` (134), `_destroy` (135), `_connect_local` (136), `_disconnect` (137), `_send` (138). A queue created with a non-zero IPC key can be found by that key (the host looks it up and sends to it; the `cellAudio` notify uses this); a second live queue with the same key fails with `CELL_EEXIST`, key 0 makes a local queue, and destroying the queue forgets the key. IPC ports (`sys_event_port_connect_ipc`) are not supported. |
| SPU | `sys_spu_initialize` (169); thread groups: `sys_spu_thread_group_create` (170), `_destroy` (171), `_start` (173), `_terminate` (177), `_join` (178), `_connect_event` (185), `_disconnect_event` (186), `_set_cooperative_victims` (250), `_connect_event_all_threads` (251), `_disconnect_event_all_threads` (252); threads: `sys_spu_thread_initialize` (172), `_write_ls` (181), `_read_ls` (182), `_write_spu_mb` (190), `_connect_event` (191), `_disconnect_event` (192), `_bind_queue` (193), `_unbind_queue` (194). A started group runs its threads as SPU host threads (see [SPU system calls](#spu-system-calls)). |
| File system | `sys_fs_open` (801), `_read` (802), `_write` (803), `_close` (804), `_opendir` (805), `_readdir` (806), `_closedir` (807), `_stat` (808), `_fstat` (809), `_mkdir` (811), `_unlink` (814), `_lseek` (818); see [Virtual file system](virtual-file-system.md#virtual-file-system) |
| Thread | `_sys_ppu_thread_exit` (41), `sys_ppu_thread_yield` (43; `CELL_OK` only when a PPU thread of equal or higher priority ran, see [Threads](#threads)), `sys_ppu_thread_join` (44), `sys_ppu_thread_detach` (45; a thread that already exited without being joined is dropped, as in RPCS3), `sys_ppu_thread_set_priority` (47; stores the value), `sys_ppu_thread_get_priority` (48); the creation rule of `sys_ppu_thread_create` is under [Threads](#threads) |

### Process and page calls

`sys_process_getpid` (1) returns the fixed process id 1. `sys_process_get_sdk_version` (25) writes the
SDK version of the executable (`0x00410001` for this game, from its process parameters) to the given
address, or fails with `CELL_EFAULT` for a null or uncommitted pointer. libsre reads it while it
creates a SPURS instance and treats a non-zero result as fatal.
`sys_process_is_spu_lock_line_reservation_address` (14) returns `CELL_OK` for an address in the main
region or in user memory and `CELL_EPERM` otherwise; the flags are not looked at.
`sys_memory_get_page_attribute` (351) fails with `CELL_EINVAL` for an uncommitted address and otherwise
writes read/write, any access right and the page size of the region (1 MiB in user memory with 1 MiB
pages, 4 KiB in the stack region, 64 KiB elsewhere). `sys_event_queue_tryreceive` (131) copies up to
`size` stored events without blocking and removes them.

### SPU system calls

The groups and threads that the calls create are records in the runtime. A group has an id from
`0x04000100` (step `0x100`, at most 255 groups) and a state: created (`NotInitialized`) until all `num`
threads are initialised, then `Initialized`, and `Running` from a start until its last thread has ended
(it then returns to `Initialized` and can be started again). How the threads execute is described in
[SPU usage](spu.md#how-the-runtime-runs-spu-code).

- `sys_spu_thread_group_create`: `num` 1 to 6, name length up to `0x80`, type 0, 4, `0x18` or `0x20`;
  unless the type has the bit `0x08` the priority must be 16 to 255; type 4 needs an existing memory
  container. Logs `tox2: spu: group <id> "<name>" created: <n> threads, priority <p>, type <t>`.
- `sys_spu_thread_initialize`: the image (`sys_spu_image`: type, entry point, segment array, segment
  count) is checked, its segment list is copied, and the thread gets an id (`(n << 24) | group`). The
  SPU number argument must be below 256 (the size of lv2's per-group table, as in RPCS3), not below the
  group's thread count: the sound engine initialises SPU number 1 in a one-thread group. The records stay
  dense, in the order of initialisation. A number that is in use gives `CELL_EBUSY`. Every error return of
  this call, of `_group_start` and of the two bind calls is logged as `tox2: spu: <call> -> 0x<code>: <why>`. The
  four 64-bit arguments are kept and logged as they are; libsre leaves arguments 3 and 4
  uninitialised, so the log shows stack garbage there.
- Event connections: `sys_spu_thread_group_connect_event` binds an event queue to the group for the
  run (1), exception (2) or system module (4) event; `sys_spu_thread_group_connect_event_all_threads`
  binds the lowest free port of a requested mask on every thread and writes it back (libsre gets port
  16); `sys_spu_thread_connect_event` binds one thread's port. The disconnect calls undo them.
  Errors are the usual ones (`CELL_ESRCH` for an unknown group, thread or queue, `CELL_EBUSY`,
  `CELL_EISCONN`, `CELL_ENOTCONN`, `CELL_EINVAL`).
- `sys_spu_thread_bind_queue` (193, thread, queue, key) and `_unbind_queue` (194, thread, key) manage a table
  of 32 slots per thread. Bind: an unknown thread or queue gives `CELL_ESRCH` (checked first), a queue that is
  not of SPU type `CELL_EINVAL`, a key or a queue that the thread has bound already `CELL_EBUSY` (any 32-bit
  key is valid, 0 too; a queue can be bound to several threads), and no free slot `CELL_EAGAIN`. Unbind:
  an unknown thread or an unbound key gives `CELL_ESRCH`. The table is part of the thread record, so it stays
  across start, terminate and restart of the group and goes away with the group on destroy. A destroyed
  queue keeps its slot (the key stays taken until it is unbound) but the SPU side no longer finds it.
  Unbinding does not wake a thread that waits on the queue. The table is used by `stop 0x110` and `0x111`
  (below).
- `sys_spu_thread_group_start` logs `tox2: spu: start group ...`, one line per thread, its image
  segments, the connected event queues, the bound ports and the bound queue keys, builds a fresh SPU thread for every thread
  record (zeroed LS, the image segments applied, the arguments in `$3` to `$6`), sends the RUN event
  to the connected queue, logs `tox2: spu: thread ... started at <image> ls 0x<entry>` for each thread
  and starts the host threads. A group that is not `Initialized` fails with `CELL_ESTAT`, and a copy
  segment whose source is not guest memory with `CELL_EFAULT` before anything changes.
- A thread ends with the `stop` code `0x102` (status from its outbound mailbox) or, for the whole group,
  `0x101`; when the last has ended the group logs `tox2: spu: group ... stopped: cause <n>, status
  0x<s>`. `sys_spu_thread_group_join` waits for the end of a run, also of one that has not started yet,
  and writes the cause (1 group exit, 2 all threads exited, 4 terminated) and the status; null result
  pointers give `CELL_EFAULT` after the wait, as SPURS joins with a null cause. A second joiner gets
  `CELL_EBUSY`, and a join on a group that is not fully initialised fails with `CELL_ESTAT` without
  waiting. `_terminate` (177) halts the threads, waits until they have ended and gives cause 4;
  `_destroy` frees the group, and fails with `CELL_EBUSY` while it runs.
- `sys_spu_thread_write_ls` (181) and `_read_ls` (182) access 1, 2, 4 or 8 bytes, aligned and big-endian,
  of the LS of the latest run of a thread (`CELL_ESTAT` for a thread that was never started);
  `_write_spu_mb` (190) pushes into the inbound mailbox and, when it is full, replaces the newest entry.
- `stop 0x110` (sys_spu_thread_receive_event) and `0x111` (`_tryreceive_event`) take a queue key from the
  outbound mailbox and answer in the inbound mailbox: a status word and, when it is 0, the low 32 bits of
  the event's `data1`, `data2` and `data3` (the details and the error replies are under
  [lv2 SPU calls](spu.md#lv2-spu-calls)). `0x110` without a stored event blocks the calling SPU thread
  until a send to the queue (`sys_event_port_send` and the other senders hand the event to the waiter, which
  takes precedence over storing it), `sys_event_queue_destroy` in force mode (the waiter gets
  `CELL_ECANCELED`; mode 0 gives `CELL_EBUSY`) or a halt (terminate, `stop 0x101` of another thread). The SPU
  host thread waits without the guest lock on its own channel condition variable, which `requestHalt` also
  signals, and takes the guest lock (the kernel lock in `--ppu-threads 1|2`) again before it removes itself from the queue or reads its reply. RPCS3
  suspends the whole group while a thread waits in `0x110` and refuses terminate with `CELL_ESTAT`; here
  only the calling thread waits and terminate works (a deviation that the one-thread bnusCore group does not
  show). `sys_event_queue_receive` and `_tryreceive` on an SPU queue return `CELL_EINVAL`. `0x120` and the
  other unknown codes remain fatal.
- An unknown SPU `WrOutIntrMbox` code, a `stop` code other than `0x100`, `0x101`, `0x102`, `0x110` and
  `0x111`, an empty outbound mailbox at `0x110` or `0x111`, and every
  condition the SPU runtime cannot execute are fatal stops on the SPU thread with exit code 3 (see
  [Exit codes](runtime-options.md#exit-codes-and-stop-lines)).
- `resetSpuThreadGroups` (test teardown) halts and joins all SPU host threads before freeing a record,
  and must not be called with the guest lock (the kernel lock in `--ppu-threads 1|2`) held while a group runs.

The image calls are sysPrxForUser imports (below): `sys_spu_image_import` builds the segment list of
an SPU ELF32 in guest memory and `sys_spu_image_close` frees it.

## Implemented imports

Imports are matched to handlers by module and FNID. The executable of this game has 173 imports from
17 modules. 127 of them have a handler and 30 (all of `cellSpurs`) are bound to functions that libsre
exports (the load log says `tox2: hle: 173 imports, 127 with handlers, 30 bound to libsre`). The other
16 are served by the [default handler](#default-handlers). Behaviour follows
the RPCS3 function of the same name (`Emu/Cell/Modules/`); each handler file lists its deviations in
its header comment. `cellAudio` and `cellAtrac` were implemented from a written description of
RPCS3's behaviour, not from its code (see [Audio](audio.md#clean-room)).

The first table lists, for this game's imports, what is bound to a handler and what is not. All
handlers were written for the earlier game; that they bind here only means that the module and
FNID match, not that the behaviour suits this game.

| Module | Bound / imported | Handled | Without a handler |
| --- | --- | --- | --- |
| `sysPrxForUser` | 20 / 20 | `sys_initialize_tls` (per-thread TLS blocks laid out as RPCS3 does), `sys_ppu_thread_create`, `_get_id`, `_once`, `_exit`; `sys_lwmutex_create`, `_destroy`, `_lock`, `_trylock`, `_unlock`; `sys_time_get_system_time`, `sys_process_exit`; `_sys_process_atexitspawn`, `_sys_process_at_Exitspawn`, `sys_prx_exitspawn_with_level`, `sys_prx_register_library` (these four return `CELL_OK` and do nothing else), `_sys_spu_printf_initialize` and `_finalize` (store and clear the four callbacks), `sys_spu_image_import`, `sys_spu_image_close` | |
| `cellSysmodule` | 2 / 2 | `cellSysmoduleLoadModule`, `cellSysmoduleUnloadModule`: every module id succeeds, because the HLE functions are always available | |
| `cellSpurs` | 30 / 30 bound to libsre | none: all 30 call libsre's recompiled exports (attribute setters, `cellSpursInitializeWithAttribute`, tasksets and tasks, queues, lock-free queues, event flags, join and push calls) | |
| `cellGcmSys` | 26 / 26 | initialization (`_cellGcmInitBody`), configuration, IO mapping (`cellGcmMapMainMemory`, `cellGcmAddressToOffset`, `cellGcmUnmapEaIoAddress`), display buffers, flip mode, the vblank and flip handlers, tiles and zcull, the control register, the flip commands and the default command buffer sizes; see [Fake RSX](rendering.md#fake-rsx). `cellGcmGetFlipStatus` returns the flip status that `cellGcmResetFlipStatus` (1, waiting) and an executed flip (0, done) leave. `cellGcmGetLabelAddress(index)` returns `0x10300000 + 16 * index` (`index` is a `u8`, so up to `0x10300ff0`), 0 before `_cellGcmInitBody`: the semaphore area that the RSX thread resolves its label offsets against, as for `_cellGcmSetFlipCommandWithWaitLabel`, whose label wait is at offset `16 * index` | |
| `cellSysutil` | 22 / 23 | the callback pump (`cellSysutilRegisterCallback`, `_CheckCallback`), BGM playback status, enabling and disabling, `cellVideoOutConfigure`, `_GetState`, `_GetResolution` (the pixel size of a resolution id, the same ids that `cellVideoOutConfigure` accepts), `cellAudioOutGetState`, `_GetConfiguration`, `_GetSoundAvailability` (the largest channel count among the modes with that coding type and rate: 2 for LPCM at 48 kHz, 0 otherwise, because the mixer is stereo) and `_Configure` (stores the downmixer, which the state and the configuration then report; a channel count other than 2 or a coding type other than LPCM is `CELL_AUDIO_OUT_ERROR_UNSUPPORTED_SOUND_MODE`, a downmixer above 2 is an illegal parameter), the message dialog (`cellMsgDialogOpen2`, `_Close`, `ProgressBarInc`) and save data (`cellSaveDataListLoad2`, `ListSave2`, `AutoSave2`, `Delete2`, `ListAutoLoad`, `FixedLoad2`, `FixedSave2`) | `cellMsgDialogOpenErrorCode` |
| `cellGame` | 5 / 5 | `cellGameBootCheck` (always a disc boot: type `CELL_GAME_GAMETYPE_DISC`, attributes 0, an empty directory name, and the permit that follows returns `/dev_bdvd/PS3_GAME` and its `USRDIR`), `cellGameDataCheck`, `cellGameCreateGameData`, `cellGameContentPermit`, `cellGameContentErrorDialog` | |
| `sys_fs` | 7 / 7 | `cellFsOpen`, `cellFsClose`, `cellFsStat`, `cellFsFstat`, `cellFsRead`, `cellFsLseek`, `cellFsWrite` (the last three call the lv2 handlers of `sys_fs_read`, `_lseek` and `_write`, so offsets and counts stay 64-bit) | |
| `sys_io` | 6 / 6 | `cellPadInit`, `cellPadEnd`, `cellPadSetPortSetting`, `cellPadGetData`, `cellPadGetInfo2`, `cellPadSetActDirect` | |
| `cellAudio` | 10 / 10 | `Init`, `Quit`, `PortOpen`, `PortStart`, `PortStop`, `PortClose`, `GetPortConfig`, `SetNotifyEventQueue`, `RemoveNotifyEventQueue`, `SetPortLevel` (the level of an open port, taken from `f1`; a negative level or NaN is `CELL_AUDIO_ERROR_PARAM`); the ports and the 8-block ring live in guest memory and a host clock drives the game's mixer; see [Audio](audio.md) | |
| `cellAtrac` | 15 / 15 | `SetDataAndGetMemSize`, `DeleteDecoder`, `Decode`, the stream calls (`GetStreamDataInfo`, `AddStreamData`), the loop and info calls, `GetMaxSample`, `GetChannel`, the second-buffer calls, `CreateDecoderExt`, `GetInternalErrorInfo` and `GetVacantSize`; ATRAC3plus is decoded through FFmpeg; see [Audio](audio.md#cellatrac) | |
| `sceNp` | 3 / 5 | `sceNpInit`, `sceNpTerm`, `sceNpDrmIsAvailable` | `sceNpManagerGetStatus`, `sceNpManagerGetContentRatingFlag` |
| `sceNpTrophy` | 9 / 10 | `sceNpTrophyInit`, `_Term`, `CreateContext`, `DestroyContext`, `CreateHandle`, `DestroyHandle`, `RegisterContext`, `GetRequiredDiskSpace`, `UnlockTrophy` | `sceNpTrophyAbortHandle` |
| `cellRtc` | 4 / 4 | `cellRtcGetCurrentTick` (the host UTC clock as microseconds since 0001-01-01), `cellRtcConvertUtcToLocalTime` (adds the host time zone offset, daylight saving included), `cellRtcSetTick` (tick to date and time) and `cellRtcGetDayOfWeek` (0 = Sunday); the pointers must be committed guest memory, otherwise `CELL_RTC_ERROR_INVALID_POINTER` | |
| `cellSysutilAvconfExt` | 1 / 1 | `cellVideoOutSetGamma` (accepts 0.8 to 1.8 from `f1` on the primary output, otherwise an illegal parameter or an unsupported video out; the gamma is not applied) | |
| `cellScreenShotUtility` | 0 / 4 | | all four |
| `sceNpCommerce2` | 0 / 2 | | both |
| `cellNetCtl` | 0 / 3 | | all three |

Handlers that exist but that no import of this game binds, because the game does not import the
function or the module:

- `sysPrxForUser`: the `_sys_heap_*` functions (create, delete, malloc, memalign, free).
- `cellSysutil`: `cellSysutilUnregisterCallback`, `cellVideoOutGetResolutionAvailability`,
  `cellMsgDialogProgressBarReset`, `cellSaveDataAutoLoad2`; `sceNpTrophyGetTrophyUnlockState`;
  `cellAtracCreateDecoder`, `cellAtracGetRemainFrame`, `cellAtracGetSoundInfo`;
  `cellAudioGetPortBlockTag`, `cellAudioGetPortTimestamp`.
- `cellPngDec` (7 functions, see [PNG decoding](#png-decoding)), `cellSail` (27 functions) and
  `cellPamf` (1 function): the game imports none of these modules, so the code is built and never
  runs for it (see [Movies](movies.md)).

Video out reports what the game's `PARAM.SFO` allows (the `RESOLUTION` attribute); a missing or
unreadable file keeps the defaults. Audio out offers LPCM 2 ch at 48 kHz whatever `SOUND_FORMAT`
says. `cellVideoOutConfigure` and `cellAudioOutConfigure` store the setting, and the `Get` calls of
the same output read it back, so the mode the game configured is the one it is told afterwards.

### sysPrxForUser handlers for libsre

libsre imports 30 sysPrxForUser functions, 24 of which had no handler. Arguments and results follow the
SDK prototypes and all handlers run under the guest lock (the kernel lock in `--ppu-threads 1|2`), except the string and
memory functions, which are kernel-free there (see [Threads](#threads)).

- String and memory functions: `_sys_strlen`, `_sys_strcpy`, `_sys_strncpy`, `_sys_strcat`,
  `_sys_strncat`, `_sys_strncmp`, `_sys_memset`, `_sys_memcpy` (overlapping ranges behave as
  `memmove`) and `_sys_memcmp`.
- Printf functions: `_sys_printf`, `_sys_vprintf`, `_sys_snprintf` and `_sys_vsnprintf` format with a
  guest `printf` (`runtime/src/hle/guest_printf.*`). Every argument is one 8-byte slot (doubles
  included), `%d`, `%u`, `%x` and so on take 32 or 64 bits by their length modifier, `%s` reads a guest
  string and `%p` prints the low 32 bits. `%n` and unknown conversions are reported once per format as
  `tox2: printf: <error> in "<fmt>"`. `_sys_printf` writes to stdout like `sys_tty_write`.
- `abort` (FNID `0x9FB6228E`, which `config/nids.json` does not name): writes its string to stdout and
  stops the run with exit code 3 and `tox2: stop: abort from 0x<lr> (sysPrxForUser 0x9fb6228e)`.
- `sys_lwcond_create`, `_destroy`, `_wait` and `_signal`: a lightweight condition variable is 8 bytes
  (the lwmutex address and an id). `_wait` needs the caller to own the lwmutex, releases it, blocks
  (the census shows `blocked in sys_lwcond_wait`) until signalled or timed out, and locks it again.
- `_sys_spu_printf_initialize` stores four callback descriptors; `_sys_spu_printf_attach_group` and
  `_detach_group` call the stored callback with the group id and return its result, or `CELL_ESTAT`
  when none is stored. The game initialises the SPU printf server after SPURS, so libsre's attach call
  sees `CELL_ESTAT` and tolerates it.
- `sys_prx_get_module_id_by_name` returns `0x8001112E` (`CELL_PRX_ERROR_UNKNOWN_MODULE`) for every name
  and logs `tox2: prx: module "<name>" is not available` once per name. No module is loadable by
  name, and libsre skips its `cellLibprof` calls when the lookup fails; its two `cellLibprof` imports
  stay default-handled.
- `sys_process_get_paramsfo` fills the buffer from the game's `PARAM.SFO`; `sys_spu_image_import`
  and `_close` are described under [SPU system calls](#spu-system-calls).

### libsre

`libsre.prx` is the firmware SPURS module. The build relocates it to `0x011d0000` and writes
`generated/inputs/libsre.reloc.elf`; its functions are recompiled like the executable's (see
[Emitting C](emitter.md#a-prx-module-libsre)) and `tox2_game` loads the relocated file.

- **Binding.** `installImports` takes the list of function exports that libsre's generated code
  carries. An import of the executable with the same (module, FNID) as an export does not get a handler.
  When it is called, the census counts it as `[libsre]`, `r2` is loaded from the export's function
  descriptor, and the dispatcher jumps to the descriptor's entry with the caller's return address
  unchanged, so libsre returns straight to the game. It is a fatal error when an export is also
  registered in a handler list, or when an export's descriptor names an address the dispatcher does not
  know.
- **libsre's own imports.** Its 32 import stubs are recompiled trampolines. They call
  `tox2HleCallImport` with a pointer into libsre's own import table, so the numbering never collides
  with the executable's. The table is served by the same handlers or the default handler, and the
  load log reports `tox2: hle: libsre: 32 imports, 30 with handlers`. The two without a handler are
  `cellLibprof`.
- **Layout check.** The runtime refuses to run with a libsre that does not match
  `runtime/src/hle/libsre.h`: the image hash, the module info (name, TOC, export and import
  ranges), the 352 function descriptors (TOC word and a 4-aligned entry in the code), the 32 import
  slots and stubs, the export descriptors, the ELF headers and entry points of the two SPU kernels
  (`0x011f0380`, `0x011f0c00`) and the first word of each raw SPU blob. Nothing from libsre's bytes
  is stored in the repository, only addresses, sizes and the SHA-256.
- **Files.** The load and the layout check are in `runtime/src/core/elf_loader.*` and
  `runtime/src/hle/libsre.*`; the binding is in `runtime/src/hle/hle.cpp`.

## Headless policy

RPCS3 opens dialogs and waits for the user in several of these modules. `tox2_game` has no user
interface, so each module follows a documented policy that never blocks on a person:

- **Message dialogs** (`cellMsgDialogOpen2`): the type and the text are logged. A dialog with
  buttons answers itself after 200 ms: OK for an OK dialog, and the `--dialog-answer` value
  (default yes) for a Yes/No dialog. A dialog without buttons stays open until
  `cellMsgDialogClose`. This game opens a dialog without buttons and with a progress bar (a game
  data installation message) just before its trap. The answer is delivered through the cellSysutil
  callback queue, so the guest sees it from `cellSysutilCheckCallback`, as on the console.
- **Save data**: a save lives in `/dev_hdd0/home/00000001/savedata/<dir name>/`. Error dialogs
  are logged and their error code is returned, without a dialog object. A save list is never
  shown; the entry is the one the game's list callback highlights, with RPCS3's fallbacks, and
  without a choice the call returns `CELL_CANCEL`. `cellSaveDataDelete2` returns `CELL_CANCEL`.
  `cellSaveDataListAutoLoad`, `FixedLoad2` and `FixedSave2` have no list either: the list of saves
  that match the prefixes (sorted, as `ListLoad2` sorts it) goes to the game's `funcFixed`, which
  names the save or ends the call with an error result. The game's boot call answers
  `CELL_SAVEDATA_ERR_NODATA`, and the handler returns `CELL_SAVEDATA_ERROR_CBRESULT` without a
  dialog, which the game counts as success. The confirmation that `FixedLoad2` and `FixedSave2` ask
  for with option NONE counts as yes. Secure files are stored in plain text.
- **Trophies**: no trophy pack is installed from the network and no notification is shown. The
  pack is read for its trophy list, and the unlock state is kept as a text file in the trophy
  directory under `/dev_hdd0/home/00000001`. The registration status callbacks run inside
  `sceNpTrophyRegisterContext` instead of being spread over two seconds.
- **cellSysutil callbacks**: they run on the guest thread that calls `cellSysutilCheckCallback`,
  from a queue that the modules above feed. `CELL_SYSUTIL_REQUEST_EXITGAME` is not acted on.
- **cellGame**: the content directories are created on the host through the VFS, under the
  `--hdd-dir` directory.
- **Audio**: the output is `none` with `--no-window` unless `--audio wasapi` asks for the device,
  so unattended runs need no sound device; the game's mixer and the `cellAudio` clock run either way
  (see [Audio](audio.md#options)). A device or decoder problem degrades to silence with one warning.
- **Input**: see `--pad` and [Input](#input). The pad on port 0 comes from a provider
  (`runtime/src/hle/hle.h`): none, neutral, or the host pad state that the keyboard, a pad script and
  gamepads write. Runs with `--no-window` never load a gamepad backend.

## Input

`cellPad` asks a `PadProvider` for the state of a port on every `cellPadGetData` call and reports
the changes to the game. `--pad` picks the provider: `none` (no pad), `neutral` (a standard pad on
port 0 that never changes), `keyboard`, `gamepad` or `keyboard+gamepad`. The last three are set up in
`runtime/src/game_main.cpp` with `setPadProvider`, because they need the window and the script or
gamepad thread; `setPadBackend` accepts only `none` and `neutral`. The option defaults are in
[`tox2_game` options](runtime-options.md#window-input-and-sound) and the key table in the
[README](../README.md#keyboard); the script format is [below](#pad-script). Gamepads (mapping,
selection, Steam, the GameInput runtime, log lines and tools) are described in [Input](input.md).

| Part | File | Role |
| --- | --- | --- |
| Buttons | `runtime/src/input/pad_buttons.*` | The 16 buttons as a bit mask, `PadAnalog` (sticks and trigger pressure), `padStateFromButtons` and `padStateFromInput` (digital words, pressure words at 0xFF for pressed buttons or the analog trigger value, sticks, everything else as the neutral pad) and the button names used by scripts |
| State and provider | `runtime/src/input/keyboard_pad.*` | The scan-code table, `KeyboardPadState` (the state of every host source: keyboard, script, gamepad) and the provider for port 0 (always connected, or connected only while a gamepad is in use with `--pad gamepad`; other ports empty) |
| Script | `runtime/src/input/pad_script.*` | The script parser and the player thread |
| Gamepad | `runtime/src/input/gamepad_*`, `gameinput_backend.*` | Backend interface, GameInput backend, mapping, selection, the gamepad thread and `--pad-probe`; see [Input](input.md#design) |
| Window | `runtime/src/render/vk_window.*` | Key and focus messages from the window thread; turns the IME off for the window |
| Overlay | `runtime/src/overlay/debug_overlay.*` | The `F1` toggle and the message hook that decides which key presses the [debug overlay](#debug-overlay) withholds from the pad |

The renderer library does not depend on `input/` or on the overlay: the window reports raw scan codes
and, through `PresenterOptions::on_message`, raw window messages through callbacks, and `game_main.cpp`
connects them to the state and to the overlay.

### Threading

`poll` runs on guest threads under the guest lock (the kernel lock in `--ppu-threads 1|2`), so it must never block or take a lock that the
window thread, the script thread or the gamepad thread holds. `KeyboardPadState` is therefore
lock-free: one atomic mask of held buttons and one atomic press time per button for each source
(`Keyboard`, written by the window thread, `Script`, written by the script thread, and `Gamepad`,
written by the gamepad thread), one atomic with the sticks and trigger pressure of the gamepad, one
for its connection flag and one for the latest vibration request. `poll` only reads them. All
accesses are relaxed, because each bit and each time is independent, and a stale read delays a
change by one poll. The script and gamepad threads are detached and own a `shared_ptr` to the state;
nothing joins them, because the process ends with `std::_Exit`.

### Key handling

- Keys are identified by scan code (PC/AT set 1 make code from `lParam` bits 16 to 23, plus `0xE000`
  for an extended key), not by virtual key, so the mapping is a positional cluster on any layout
  and works while an IME is open. A zero scan code (some injected input) falls back to
  `MapVirtualKeyW`. Only the dedicated arrow keys map; the numpad does not.
- Key repeats are dropped, so they do not refresh a press time.
- A press stays in the reported state for at least `KeyboardPadState::kMinPressVisible` (60 ms)
  after it started, also when the key is already up. The game reads the pad about every 33 ms and
  `cellPadGetData` reports only changes, so a shorter tap could fall between two reads.
- Both opposite D-pad directions held cancel each other. A held direction beats the 60 ms tail of a
  released opposite one; with two tails the later press wins.
- `WM_KILLFOCUS` releases every `Keyboard` button at once (a key-up is not delivered to a window
  without focus). Script buttons are a separate source and stay; the gamepad thread releases its own
  source when the process loses the foreground window. `Alt` combinations still reach the
  default window procedure, so `Alt+F4` closes the window.
- `F11` and `Alt+Enter` switch the window between windowed and borderless fullscreen (a popup window
  covering the monitor the window is on); the next press restores the earlier size and position. Neither key
  is in the pad map, and the window procedure handles them before the overlay and the pad see them.
  The state is saved (see [Window](#window)).
- `F1` (scan code `0x3B`) is not in the pad map: it toggles the [debug overlay](#debug-overlay) and
  never reaches the pad, with any `--pad` value. While the overlay is shown and ImGui wants the
  keyboard (a text box is active), key presses (`WM_KEYDOWN`, `WM_SYSKEYDOWN`) are withheld from the
  pad. Key-ups are always delivered, so a key that went down before the overlay took the keyboard is
  still released.
- The press mode that the game sets with `cellPadSetPortSetting` decides whether `cellPad` copies
  the pressure words to the game; this game sets 0x6 (press and sensor on) on ports 0 to 6
  (`tox2: pad: port 0 setting 0x6 (press on, sensor on)`), after `cellPadInit` with a maximum of 7
  pads.

### Pad script

A script is a text file of timed commands, one per line, read by `loadPadScript` while the options
are parsed. `#` starts a comment, blank lines are ignored, tokens are separated by spaces or tabs:

```
<time> press   <buttons>
<time> release <buttons>
<time> tap     <buttons> [every <interval> until <time>]
<time> stick   left|right <x> <y>
```

- `<time>` and `<interval>` are either decimal seconds from the start of the script, with
  millisecond precision (more decimals are rounded), or `f<N>`, a flip count (`f1200`, decimal
  digits only). `<buttons>` are the names `select l3 r3 start up right down left l2 r2 l1 r1
  triangle circle cross square`, joined with `+`.
- A script uses one clock. The first timed line decides it, and mixing seconds and flips anywhere
  (times, intervals or `until`) is an error: `line 5: flip times and second times cannot be mixed`.
- `tap` is a press at `<time>` and a release `kPadScriptTapHold` (150 ms) later, which is longer
  than the minimum press time; on the flip clock the hold is `kPadScriptTapHoldFlips` (9 flips,
  150 ms at 60 flips/s). `every <interval> until <time>` repeats it at `<time> + k * <interval>`
  while that is not after the `until` time; `<interval>` must be longer than the hold. A script has
  at most 100000 events, times of at most 1000000 s and flip times of at most 100000000.
- `stick` puts one analog stick at (`<x>`, `<y>`): decimal numbers from -1 to 1 in GameInput's
  convention, +x right and +y up (digits beyond the third decimal are rounded). A value outside the
  range or a missing, extra or non-numeric token is an error (`line 5: stick value "1.5" out of range
  -1..1`, `line 5: unknown stick "middle"`). The values become CellPadData bytes with `stickByte`
  (`runtime/src/input/gamepad_map.h`) and no deadzone: x as is, y inverted because CellPadData 0 is
  up. The stick stays at that position until the next `stick` command for the same stick, and
  `stick left 0 0` centres it; there is no `every`/`until` form. The script sticks are merged with a
  connected gamepad's per axis: the value farther from centre wins, a tie goes to the gamepad.
  Without `stick` commands the script leaves the sticks alone. This is how an unattended run walks
  the character (left stick) and turns the camera (right stick) in the field.
- The events are sorted by time (stable); at the same time, button releases come before everything
  else (presses and stick moves keep their file order).
- An error names the line (`line 7: unknown button "crosss"`), is printed to stderr and ends the
  start-up with exit code 2.
- A seconds script starts its clock right before the main guest thread is booted, so it matches
  `--seconds` within a few milliseconds. The player thread sleeps until each event, applies it to
  the `Script` source and logs it.
- A flip script waits for `rsxFlipCount()` (`runtime/src/rsx/rsx_thread.h`), the number of
  completed RSX flips since the process started, to reach each time. "At `f100`" means after the
  100th flip: the same number as in `frame_000100.png` of `--dump-frames` and in `--rsx-capture 100`.
  The flip clock removes the drift that comes from frame rate (a scene that renders below 60 flips
  per second), but not the one that comes from loading: the game keeps flipping during loads, and
  their length differs between runs. Three 240 s headless runs of the same route reached one scene
  at flips 8800 (seconds script), 9000 (flip script) and, in a third run where a load took longer, a
  menu press came too early and the route changed. A press that must land in a short window is
  safer with a margin or repeated taps. In a seconds script each log line carries the flip count,
  so one run's log gives the flip times. The count rises with every flip, also with `--render none`.

No pad script ships with the repository. A script that taps Circle every 2 s from 4 s to 30 s:

```
4 tap circle every 2 until 30
```

A script that holds a direction while it taps a button:

```
10   press   down
10.5 tap     cross+r1
12   release down
```

The same kind of script on the flip clock, which taps Circle every 120 flips from flip 600 to
flip 3000 and holds Down for 90 flips from flip 3100:

```
f600 tap circle every f120 until f3000
f3100 press down
f3190 release down
```

### Log lines

All pad lines start with `tox2: pad:`; the script lines carry the `[host]` prefix. The lines of
gamepads (`gameinput`, `steam`, `device`, `gamepad`, `probe`) are in [Input](input.md#log-lines).

| Line | Level | Source |
| --- | --- | --- |
| `tox2: pad: init max_connect N` | info | `cellPadInit` |
| `tox2: pad: port N connected` or `disconnected` | info | the first change of a port to that state |
| `tox2: pad: port N setting 0xS (press on\|off, sensor on\|off)` | info | `cellPadSetPortSetting`, once per port and value |
| `tox2: pad: <pad>` followed by ` (window)` and ` (script <file name>, <n> events)` as they apply | info | start-up with `--pad keyboard`, `gamepad` or `keyboard+gamepad` (`<pad>` is the value) |
| `tox2: pad: script <s>.<ms> s: press\|release <buttons> (flip <N>)` or `tox2: pad: script f<N>: press\|release <buttons>` | info | the player thread, one line per event; the form follows the clock of the script. In a seconds script `<N>` is the flip count when the event was applied, so a run's log gives the flip times for a flip script |
| `tox2: pad: key 0x<scan code> down\|up -> <button>` | debug | a mapped key, window thread; unmapped keys and repeats are not logged |

### Scripted runs and tests

Pressing buttons can make a game write save or system data under `--hdd-dir`, so a scripted run
should use its own directory and delete it first, never the directory of a normal run or of
`game_boot`. This repository has no test that runs the game with a pad script. The pad code itself
is covered by `runtime_core` (`testPadButtons`, `testKeyboardPadScanCodes`, `testKeyboardPadState`,
`testKeyboardPadProvider`, `testKeyboardPadThreads`, `testPadScriptParse`, `testPadScriptErrors`,
`testPadScriptPlay`; the flip count that a flip script waits on is checked in `testRsxConsumer`), the gamepad source of the state by `runtime_core` (`testPadStateFromInput`,
`testGamepadSourceState`, `testGamepadPadProvider`, `testGamepadCellPad`), the rest of the gamepad
code by `input_core` and `input_gameinput` (see [Tests](tests.md)), and the key message conversion
by `render_core` (`testKeyMessages`).

## Window

The window is created and run by one thread (the window thread), the only one that calls Win32 functions
on it. Other threads ask it for something with `PostMessageW`, never with a call that waits.

- *Size presets.* The client area is one of four presets, in physical pixels: `720p` (1280x720, the
  default), `1080p` (1920x1080), `1440p` (2560x1440) and `2160p` (3840x2160). `--window-size` sets the
  one at start-up and the overlay's `Window size` combo changes it while the game runs. DPI scaling does
  not change the sizes: at a display scale of 150 % the 720p window still has a 1280x720 client area, so
  the game's frame (1280x720 times the [resolution scale](rendering.md#resolution-scale)) maps one to one
  onto the preset of the same size.
- *Fit.* When the preset plus the window frame (at the window's DPI) does not fit the work area of the
  monitor the window is on, the window gets the largest 16:9 client size that does (a width that is a
  multiple of 16). The preset itself stays selected and saved, so a larger monitor gets the full size at
  the next start. The invisible resize borders count as frame, so a reduced window is a few pixels
  smaller than the work area would allow. The window keeps its top left corner and is moved just far
  enough to lie inside the work area; one larger than the work area is aligned to its top left.
- *Choosing a preset.* Choosing the preset that is already set sizes the window again, which snaps a
  dragged window back. A maximised window is restored first. In fullscreen nothing changes until the
  window leaves fullscreen, and then the preset is applied. While the window is minimised it is applied
  when the window is restored. A window the user resized by dragging keeps that size until a preset is
  chosen; the overlay then shows the preset with the actual size, for example `1080p (window 1500x844)`.
- *Fullscreen.* `Alt+Enter` and `F11` switch between windowed and borderless fullscreen. `--fullscreen on`
  starts in fullscreen: the preset is applied to the hidden window first and the window is shown
  fullscreen, so no windowed frame appears, and leaving fullscreen returns to the preset.
- *Saved state.* The preset (from the overlay) and the fullscreen state (from `Alt+Enter` and `F11`) are
  saved to `config.json` as soon as the player changes them, and restored at the next start. The start-up
  state, whatever its source, and dragging the frame are never saved. See
  [Saved settings](runtime-options.md#saved-settings). The window position is not saved, and a move to a
  monitor with another display scale keeps the window rectangle.

## Debug overlay

With the window open, `F1` shows or hides a [Dear ImGui](https://github.com/ocornut/imgui) window
titled `tox2` over the presented image, at the top left. It has four collapsible sections:

| Section | Contents |
| --- | --- |
| Stats | The flip number (the `frame_<N>.png` and `--rsx-capture N` numbering), flips per second over the last second, draws and skipped draws of the last flip, the total of skipped draws and a graph of the last 240 flip intervals. Below, the audio counters of the engine and `cellAudio`: the output, the longest producer time against the 5.333 ms block period and its margin, blocks, periods, failed notifies, skipped periods (with a rate), underrun frames (with a rate), dropped blocks, the clock correction in ppm and the mean output fill against its target, then the unwritten port blocks (total, rate and runs, see [Audio](audio.md#cellaudio)), one row per SPU thread that receives events (`spu <name> busy` mean, 99th percentile and maximum in ms, and the number of intervals over one 5.33 ms period) and a `guest lock` table with one row per caller class (acquisitions per second, wait 99th percentile and maximum in ms, and waits over 1 ms per second). Before `cellAudioInit` the audio part says `n/a (audio not started)` |
| Settings | The flip limiter (the `--frame-limit` choice of `vsync`, `60` or `off`, changeable at run time), the log level (the lowest level that is logged; the same filter as `--log-level`), `Internal resolution scale` (1x to 4x, the run-time form of `--resolution-scale`; disabled when no renderer reports a scale), `Anti-aliasing` (`Supersample`, `MSAA 2x` or `DLAA`, the run-time form of `--aa-mode`; disabled when no renderer reports a mode, the `MSAA 2x` item is disabled on a device without 2-sample support, and `DLAA` is listed only when the renderer reports it available: an NVIDIA RTX GPU with the Streamline DLLs next to the executable, see [DLAA](rendering.md#dlaa)), `Anisotropic filtering` (`Off`, `2x`, `4x`, `8x` or `16x`, the run-time form of `--anisotropic-filter`; items above the device's maximum are disabled, and the whole combo is disabled, reading `unavailable on this device`, when the device lacks the feature), `Frame generation` (`Off`, `60 fps`, `90 fps`, `120 fps`, `180 fps (unverified)` or `Unlimited`, the run-time form of `--frame-gen`; listed only when the renderer reports frame generation available, see [Frame generation](rendering.md#frame-generation); the combo is disabled with the note `needs DLAA anti-aliasing` while the anti-aliasing mode is not DLAA, and keeps the saved target, which applies again when DLAA is selected) and `Window size` (`720p (1280x720)` to `2160p (3840x2160)`, the run-time form of `--window-size`; an item that does not fit the screen reads for example `2160p (3840x2160, fits as 2496x1404)`; while fullscreen the preview reads for example `1080p (when windowed)`; disabled when no window reports its status). A choice is applied by the renderer right after the next flip: the picture sharpens (or softens) from the following frame, and each target is rescaled when it is next drawn to (see [Resolution scale](rendering.md#resolution-scale)). A new anti-aliasing mode takes effect at the next pass over the anti-aliased surface; no target is touched at the switch (see [Anti-aliased surfaces](rendering.md#anti-aliased-surfaces)). A new anisotropy level applies to the samplers created from then on (see [Anisotropic filtering](rendering.md#anisotropic-filtering)). A new frame generation target is applied after the next flip; switching between `Off` and a target recreates the swapchain, like a window resize. Below the combo a status line shows the multiplier and the rates, for example `4x: 30 -> 120 fps, measured 119.6`, or `inactive: <reason> (presenting game <g> fps)` with the reason of the info log, and `suspended while this panel is open; last: 4x: 30 -> 120 fps, measured 119.6` while the overlay is shown: generation is suspended while the overlay is open, because the overlay would be interpolated with the scene's motion. The same rates are in the log: the info line `tox2: render: frame generation active: <m>x (game <g> fps, expected <g*m> fps, display <r> Hz, target <t>)` on every change of the state, the multiplier, the game rate class or the target, and the measured rate every 10 s at debug level (`--log-level debug`). A new window size is applied at once (see [Window](#window)). The flip limiter, the resolution scale, anti-aliasing, anisotropic filtering, frame generation and the window size are saved to `config.json` as soon as they are changed and restored at the next start; the log level is not saved (see [Saved settings](runtime-options.md#saved-settings)). Volume is a disabled placeholder marked `(not implemented)` |
| Capture | `Capture next flip` and `Dump next flip as PNG`, each with the state of its last request, and the dump counts (written, failed, skipped) |
| RSX unimplemented | The live `rsx unimplemented` counts (see [Rendering](rendering.md#rendering)) as a table of count and kind, with a text filter, and the number of kinds dropped because the table was full |

`Capture next flip` records the first whole frame that starts after the click, which is typically
flip N+2 when the click happens while flip N is displayed, and writes it as
`generated/rsx/captures/frame_<N>.tox2cap` like `--rsx-capture`. The button is disabled while a
request is pending or recording, and when no frame builder runs. The panel shows `pending`,
`recording frame N`, then `frame N -> <path>` or `frame N failed: <reason>`.
`Dump next flip as PNG` writes the next completed flip as `frame_<N>.png` into the `--dump-frames`
directory, or `generated/frames` without that option; a flip that `--dump-frames-every` dumps anyway
is written once.

The overlay is drawn on the swapchain image after the letterboxed blit, so it is never part of
`--dump-frames` files, requested dumps, `readDisplay` or captures (see
[Rendering](rendering.md#rendering)). It redraws only when the game flips: during a stall without
flips (a load, for example) it freezes and takes no input. Without the window (`--no-window`,
`--render none`) there is no overlay and `F1` does nothing.

Design notes:

- The code is the `tox2_overlay` library in `runtime/src/overlay/`; `tox2_render` does not depend on
  it. `overlay_model.*` holds the numbers (no ImGui), `overlay_panels.*` the ImGui window,
  `overlay_sources.*` the connections to the runtime (audio engine and `cellAudio` counters, the
  `rsx unimplemented` snapshot, the flip limiter, the capture request and status, the log level) and
  `debug_overlay.*` the Vulkan and Win32 backends. The overlay only reads atomics and locked
  snapshots and never touches guest memory.
- `tox2_overlay` does not know about the config file. When the player picks a value in the Settings
  section, the panel calls `OverlaySources::setting_changed` with the setting and the value in its
  command-line form, only when the pick differs from what the combo shows (for the window size, from the
  preset the combo shows; choosing it again still sizes the window). `tox2_game` maps the call to a config
  key and hands it to a settings store, which does the file work on its own thread, so neither the
  RSX thread nor the window thread waits for a file. The fullscreen state reaches the store the same way,
  from the window's fullscreen callback.
- ImGui is initialised on the first show, not at start-up, on the RSX thread: the log gets
  `tox2: overlay: ready (imgui 1.91.9b, scale N)` (info) and the font upload costs a one-time hitch.
  `scale` is the DPI scale of the window at that moment and is not updated later. If the setup fails,
  one `tox2: overlay: ... failed, overlay disabled` warning is logged and the game goes on without
  the overlay.
- One global ImGui context is guarded by a mutex. The window thread feeds it the mouse and key
  messages (the Win32 backend calls `SetCapture` and `GetKeyState`, which only work on the thread that
  owns the window); the RSX thread holds the mutex while it builds and records a frame, and never
  across a swapchain call. `ImGui_ImplWin32_NewFrame` is not used, because its key workaround reads
  `GetKeyState` on the calling thread. Hiding the overlay sends ImGui a focus-lost event so that held
  keys and buttons are dropped, and showing it again sends a focus-gained event: ImGui 1.91.9b keeps
  clearing its input while it believes the application has no focus.
- The overlay window has no `imgui.ini`, no docking, no gamepad navigation and does not change the
  mouse cursor.

## PNG decoding

This game does not import `cellPngDec`, so none of this runs for it; the description is of the
inherited code. `cellPngDec` decodes with libpng's classic API (`externals/libpng` at v1.6.59, built as a static
library together with zlib v1.3.2 from `externals/zlib`; nothing is installed or downloaded).
libpng reports errors by `longjmp`, so every call that can fail runs in a function that owns the
`setjmp` and has no locals with destructors. The handle and stream objects are allocated through
the game's malloc and free callbacks, as on the console. A buffer or file source is read into one
host buffer; with a `cbCtrlStrm` callback the runtime calls it for more data until the IEND chunk
is complete and then decodes the whole image. Output RGBA, ARGB, RGB, gray and gray-alpha come
from the stored format (palette, tRNS and sub-8-bit samples expand, 16-bit samples drop to the
high byte for 8-bit output, Adam7 images are de-interlaced); rows are written
`outputBytesPerLine` apart, top to bottom or bottom to top. Not implemented: RGB to grayscale, partial output through `cbCtrlDisp`, the
`cellPngDecGet*` chunk accessors. Each stream's source, header and output format are logged with
the `tox2: pngdec:` prefix.

## Default handlers

A system call without a handler, and an import without one, return 0 (`CELL_OK`) in `r3` and
change nothing else. The first call of each distinct syscall is logged as
`tox2: default sc <number> <name>(r3..r6)`, and the first 8 calls of each import as
`tox2: default hle <module>::<name>(r3..r8)` (none with `--quiet-hle`); later calls are only
counted. Returning success is a probe, not an implementation: guest code that depends on an
output parameter reads whatever was in memory, so a default-handled call can lead to a later
fault. The census shows which calls were default-handled, which is the work list for the next
handlers. On the first call of each default-handled import the runtime also logs the argument
registers that look like out-pointers (a nonzero 32-bit value that addresses committed memory
outside the code), as `tox2: default hle <module>::<name>: possible out-pointer r<N>=<address>
(<region>)`, and the census repeats the register list as `ptr r<N>,...`. These are hints for writing
the handler, not facts: a register left over from an earlier call can look like a pointer too.

In the earlier runs of this game, before libsre was loaded, the default handlers served 24 imports and
no system call. The current run (see [SPU usage](spu.md#what-the-current-run-reaches)) default-handles
four imports (an earlier build also default-handled the system call `sys_spu_thread_bind_queue`, 193, and `cellRtcGetCurrentTick`,
`cellRtcConvertUtcToLocalTime` and `cellSaveDataListAutoLoad`): the four `cellScreenShotUtility` functions. An earlier
stop, a write to address 0 in fn `0x008a090c` on the GCM interrupt thread, followed the
default-handled `cellGcmGetLabelAddress`, which returned 0 instead of a label address; with a
handler for it the thread no longer faults. A later stop, the game's own `abort()`, followed the
memory container calls, which did not write a container id (see
[Implemented system calls](#implemented-system-calls)). Those runs ended at the `--seconds`
watchdog, while one worker thread looped on the default-handled `cellSpursEventFlagWait`, which
returned success at once (about 160 million calls in 20 s; see [Status](status.md)). With libsre
bound, the `cellSpurs` functions are no longer default-handled, and the SPURS code runs on recompiled
SPU code.

With the default result of `cellGameBootCheck` the game used data file paths without a directory
prefix; with the handler they start with `/dev_bdvd/PS3_GAME`.
