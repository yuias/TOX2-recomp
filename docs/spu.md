# SPU usage

The runtime cannot run SPU code yet. This page records how the game uses the SPUs, measured from
the executable, from libsre and from headless runs, as the basis for choosing how to run that code.
Addresses are guest addresses in the pinned `EBOOT.elf`, or in the relocated `libsre.reloc.elf` where
the text says so (libsre sits at `0x011d0000`).

## SPU images

All SPU code is embedded in the executable. Every loader site passes a link-time constant (a TOC
slot or a table in the executable), and no SPU image is opened from a file. `probe` lists the eight
images (see [Analysis tools](analysis-tools.md)); the names come from each image's SPUNAME note.

| # | Address | Text bytes | LS base / entry | Name | Role |
| --- | --- | --- | --- | --- | --- |
| 1 | `0x00f75400` | 4,480 | `0x3000` / `0x3070` | SpursTaskSpline.elf | SPURS task: spline evaluation |
| 2 | `0x00f76700` | 18,336 | `0x3000` / `0x3090` | edgelzma_inflate_task | SPURS task: Edge LZMA inflate |
| 3 | `0x00f7b080` | 85,008 | `0x80` / `0xd0` | bnusCoreSpu.elf | Raw SPU thread: core of the game's sound engine |
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
| `0x011f5500` | `0x670` | taskset 2 exit handler | position independent | `cellSpursCreateTaskset2` |
| `0x011f5b80` | `0x710` | exit code plugin | not recorded | `cellSpursCreateTaskset2` |
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
- Sound engine (fn `0x00955380`): `cellAudioInit`, then a raw SPU thread group "bnusCore" with one
  thread "bnusCoreMain" running image 3, an SPU queue `'nuAS'` bound to an event queue, an event
  port connected for `cellAudio` notifications, and the PPU threads "bnusCoreUpdateThread",
  "bnusCoreDecoderServerThread" and "bnusCoreDecoderAT3PThread". Shutdown is fn `0x00955804`.
- SPURS instance B "atrac3plus" (fn `0x009568c0`): one SPU, created only to be passed to
  `cellAtracCreateDecoderExt`, so that the firmware ATRAC3plus decoder can run on it.
- The SPU printf server (`0x0098dd24`-`0x0098e26c`, `0x00997928`-`0x00998dd4`) is the only code
  that uses `sys_spu_thread_read_ls`/`write_ls` and the SPU thread event syscalls; the game itself
  does not exchange LS data or mailbox values with an SPU program.

## What the current run reaches

The headless run (see [Status](status.md)) executes the game's calls up to
`cellSpursInitializeWithAttribute` as recompiled code. libsre builds SPURS instance A: it creates 16
semaphores for the workload slots and one for the private lock, imports the version 1 kernel from
`0x011f0380`, creates the SPU thread group `SpursTLGraphicsCellSpursKernelGroup` with 5 threads,
initialises the threads, creates the event queue, port and helper threads, and its handler thread
starts the group. The runtime logs the group, its threads (each with `image 0x011f0380 entry 0x818`,
one copy segment of `0x780` bytes to LS `0x100`, and the arguments), the event queue and the ports,
then stops with exit code 8:

```
tox2: stop: spu: sys_spu_thread_group_start group 0x04000100 "SpursTLGraphicsCellSpursKernelGroup" (5 threads, image 0x011f0380 entry 0x818): SPU execution is not implemented
```

The game does not return from `cellSpursInitializeWithAttribute`, so it creates no taskset, task or
queue, pushes no work and waits on nothing. The LZMA taskset, the engine tasksets, the sound engine,
the spline tasks and instance B are not reached.

Once SPU code runs, the game's first likely dependency is the LZMA path: fn `0x0080d370` parses a
0x18-byte header and, for one compression type, fn `0x0080d5b0` waits on the event flags after
`LFQueue` pushes at `0x00c2e294`/`0x00c2e450`. Whether the game's archive data goes through this path
has not been checked. In the earlier runs, where the `cellSpurs` functions were default-handled and
returned success at once, the first symptom was a worker thread that polled `cellSpursEventFlagWait`
without end.

## Entry points to cover

- SPURS task entry at LS `0x3070` in images 1, 4, 5, 6, 7 and 8.
- SPURS task entry at LS `0x3090` in image 2.
- Raw SPU thread entry at LS `0xd0` in image 3.
- SPURS kernel entry at LS `0x818` (version 1, in libsre at `0x011f0380`), the first code a run
  would execute. The taskset policy module (libsre, `0x011f3680`) that the task images depend on is
  loaded by the kernel.
- Not in the executable or in libsre: the firmware ATRAC3plus SPU decoder (not needed if `cellAtrac`
  is implemented on the host).

## Facts that bear on the method

- The SPU code is a closed set of eight binaries in the executable plus the kernel and policy modules
  in libsre, all with known entry points, as on the PPU side, which suits static recompilation.
- Static recompilation and an interpreter both need a model of MFC DMA and atomic reservations,
  mailboxes and event queues. The SPURS kernel and taskset interface at the fixed LS locations above
  need no host implementation if the SPU code in libsre runs unchanged; replacing them with host code
  is the alternative. The sound engine also needs a raw SPU thread group model.
- Replacing single tasks with host code: Edge LZMA is a published Sony library with a known work
  element format, and LZMA is simple to implement on the host. The engine tasks would each need
  their element format and computation worked out. The sound engine core (85 KB) is not practical
  to replace this way.

## References

The SPU decoder, emitter and semantics cases are written from these public documents. They are copyrighted by
IBM, Sony and Toshiba and are not included in this repository.

- *Synergistic Processor Unit Instruction Set Architecture*, Version 1.2, January 27, 2007: instruction formats,
  semantics and the interrupt and channel instructions. A copy is at
  <https://arcm.csc.ncsu.edu/~mueller/cluster/ps3/SDK3.0/docs/arch/SPU_ISA_v1.2_27Jan2007_pub.pdf>.
- *SPU Assembly Language Specification*, Version 1.7: assembler mnemonics and operand syntax. A copy is at
  <https://support.scinet.utoronto.ca/Manuals/Cell/SPU_Assembly_Language_Specification_1.7.pdf>.
