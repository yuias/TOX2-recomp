# SPU usage

The runtime cannot run SPU code yet. This page records how the game uses the SPUs, measured from
the executable and from headless runs, as the basis for choosing how to run that code. Addresses
are guest addresses in the pinned `EBOOT.elf`.

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
`0x170`, `0x1c0`-`0x1e0`, `0x2fb0` and `0x2fd0`. That firmware code is not in the executable.

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

The headless run (see [Status](status.md)) creates instance A, the LZMA taskset and the five engine
tasksets with their 30 tasks, and then stops. It pushes no work to any queue and waits on no event
flag or task, and the main thread is not blocked on an SPU result at the stop. The sound engine,
the spline tasks and instance B are not reached.

The `cellSpurs` functions are default-handled: they return `CELL_OK` and write no out-parameters.
When the game starts to push work and wait for results, the waits will return at once, so the first
symptom is expected to be missing or wrong data rather than a hang. The first likely dependency is
the LZMA path: fn `0x0080d370` parses a 0x18-byte header and, for one compression type, fn
`0x0080d5b0` waits on the event flags after `LFQueue` pushes at `0x00c2e294`/`0x00c2e450`. Whether
the game's archive data goes through this path has not been checked.

## Entry points to cover

- SPURS task entry at LS `0x3070` in images 1, 4, 5, 6, 7 and 8.
- SPURS task entry at LS `0x3090` in image 2.
- Raw SPU thread entry at LS `0xd0` in image 3.
- Not in the executable: the SPURS kernel and taskset policy module that the task images depend
  on, and the firmware ATRAC3plus SPU decoder (not needed if `cellAtrac` is implemented on the
  host).

## Facts that bear on the method

- The SPU code is a closed set of eight binaries with known entry points, as on the PPU side, which
  suits static recompilation.
- Static recompilation and an interpreter both need a model of MFC DMA and atomic reservations,
  mailboxes and event queues, and a host implementation of the SPURS kernel and taskset interface
  at the fixed LS locations above. The sound engine also needs a raw SPU thread group model.
- Replacing single tasks with host code: Edge LZMA is a published Sony library with a known work
  element format, and LZMA is simple to implement on the host. The engine tasks would each need
  their element format and computation worked out. The sound engine core (85 KB) is not practical
  to replace this way.
