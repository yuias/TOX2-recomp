# RSX and rendering

This page describes the fake RSX, the frame builder, the Vulkan renderer, the shader translators
and the capture and replay tools. The code was taken over from the project this one was derived
from and has run with *Tales of Xillia 2* since (see [Status](status.md)): the game's flips run in
the fake RSX, and the renderer draws the frames into the window or, with `--dump-frames`, into PNG
files. The dumped frames show the game's legal notice, dialog, "Presented By" and title screens,
`tox2_rsxreplay` replays frames recorded from the game, and the resolution scale, the anti-aliased
surfaces and the flip rate with the renderer on have been run and measured with this game (see
[Image reuse](#image-reuse), [Resolution scale](#resolution-scale) and [Anti-aliased
surfaces](#anti-aliased-surfaces)). A warm 260 s run lists one `rsx unimplemented` kind and skips
no draw.

The output has not been compared with the console's. The methods, the frame limits and the surface
handling follow RPCS3's sources, and the hardware details that those sources do not give, such as
the sample positions of the anti-aliased surfaces, are marked below as not known.

## Fake RSX

`cellGcmSys` initialization creates the RSX state and starts two things that stand in for the
graphics hardware. Without `--render` (or `--rsx-capture`) the FIFO is only parsed and nothing is
drawn; with `--render vulkan` the same thread also feeds a renderer (see [Rendering](#rendering)).

- The RSX thread is a host thread without a guest context. It follows the `put` pointer that the
  game writes into the control register, parses the FIFO (`runtime/src/rsx/fifo_parser.cpp`:
  methods, increments, jumps, calls and returns, non-incrementing runs) and executes the
  synchronization methods: semaphores and labels, reports and timestamps, `ref`, the flip
  command and the user command. It then publishes `get`. Method semantics follow RPCS3's
  `Emu/RSX/NV47/HW` sources. Every register write is counted for the census, and the thread
  raises one vertical-blank event every 1/60 s. How a flip from the command buffer is paced depends on
  `--frame-limit`. In every mode the flip status, the flip handler, the last flip time and the flip
  label are updated only when the flip completes, and while a flip is held the FIFO stays at that
  command (`get` stays at its header) and nothing behind it runs.
  - `vsync` (the default) follows RPCS3's "PS3 Native" frame limit and the game's flip mode
    (`cellGcmSetFlipMode`). A flip in VSYNC mode, the mode this game sets, completes at the first
    vblank after its command executes: the command records the vblank count and completes once the
    count has moved on. The first flip is held too, and at most one flip completes per vblank, so the
    game runs at 60 flips per second at most, and at 30 when its frame interval is 2, with no
    wall-clock deadline. At a tick the vblank interrupt is queued before the flip interrupt, as it is
    on the hardware, because the game's frame-interval countdown in the vblank handler depends on that
    order. A flip in HSYNC mode is limited as in `60`.
  - `60` is RPCS3's default "Auto" frame limit and ignores the flip mode: the first flip is
    immediate, and each later flip completes no earlier than a deadline that moves on by exactly 1/60 s
    per flip. A flip that is late does not make up for it: the next deadline is then 1/60 s after that
    flip.
  - `off` completes a flip as soon as its command executes.

  The wait happens with the guest lock (the kernel lock in `--ppu-threads 1|2`, see [Threads](runtime.md#threads)) released. The RSX thread sleeps until the flip's deadline or
  the next vblank, whichever is first, and a flip held for a vblank sleeps until the next one, so
  vblanks are still delivered on time while a flip is held. The vblank is due on a 1/60 s deadline
  that is checked between slices of the command buffer too, so a consumer that is behind (a long draw
  backlog) still delivers it on time. The game's frame rate is therefore bounded by the limit,
  whatever the display's refresh rate is; see also [Rendering](#rendering).
- `_gcm_intr_thread` is a guest service thread, created as in RPCS3. The RSX thread queues the
  interrupts and this thread runs the handlers the game registered with
  `cellGcmSetVBlankHandler`, `cellGcmSetFlipHandler` and `cellGcmSetUserHandler`, with the guest
  lock held. A vblank is not queued while an earlier one is still waiting (the census adds
  `coalesced vblanks N` to its interrupt line when that happened); flips and user
  commands are never dropped.

The method census (the `rsx` section of the census) lists the number of FIFO headers, the number
of distinct methods and flips, how often each interrupt handler ran, and the count per method,
named when the method has a known name. With the renderer on, it also gives the number of draws
rendered and skipped.

With the renderer on, the RSX thread builds a draw's command after it has released the guest lock (see
[Threads](runtime.md#threads)). The builder copies each draw's vertex data in two passes. The first computes the
guest range of every attribute and the size of the whole copy, so the draw's vertex buffer is sized once. The second
reads the ranges. Attributes of one location whose ranges overlap or touch, such as the fields of an interleaved
array, share one copy of the merged range, and each attribute's `data_offset` points into it, so only the bytes some
attribute reads are copied and each is copied once. The limit on a draw's vertex data (64 MiB) counts the ranges as if
they were not shared, so the same draws are dropped as without sharing. An attribute that fails the size or address
check ends the plan: the attributes before it are still read, and an unreadable one among them is the note that is
counted.

The vectors of a draw (attributes, vertex and index data, textures, program words and constants) are large and
the same size from frame to frame. After the commands of a pass have been executed and no command sink keeps them,
the RSX thread gives them back to the builder (`RsxFrameBuilder::recycle`), which keeps the emptied vectors of up to
64 draws and moves a set into the next draw it builds. A kept vertex buffer that grew past 1 MiB or index buffer that
grew past 256 KiB is freed instead, so one huge draw does not pin its memory. Texture blobs and constant snapshots are
released at once, not when the set is used again. The emptied command vector is kept as well.

With `--log-level debug`, the RSX thread reports where its time went once per second, as one of the perf lines (see
[Logging](runtime.md#logging)): `tox2: perf: t <s> s: rsx busy 6.20 ms/flip: lock 1.80 pump 1.04 build 1.67 record
1.69, idle 3.90`. The figures are milliseconds per completed flip of that second (the line is left out in a second
without a flip) and split the thread's wall time by activity. Every moment of its loop is charged to exactly one of
them:

- `lock`: waiting for the guest lock.
- `pump`: holding the lock, parsing the FIFO in slices and delivering vblank ticks.
- `build`: `takeCommands`, which builds the pending draw (the guest-byte copies and the program and texture
  work) outside the lock.
- `record`: handing the commands to the renderer (recording and submitting) and writing capture files.
- `idle`: sleeping between passes. A pass that finds nothing to do counts as idle.

`busy` is `lock`, `pump`, `build` and `record` added together. Busy plus idle is about 1000 ms divided by the
flips of that second; idle time left in the flip interval means the thread is not what limits the frame rate.

## Rendering

The renderer is on when `tox2_game` shows its window (the default) or `--dump-frames` is given, and off
with `--no-window` alone (a run with `--no-window --dump-frames` renders without a window).
`--render vulkan` (implied by the window and by `--dump-frames`) creates it on the RSX thread when that thread starts; the RSX thread then hands each
frame's commands to it outside the guest lock, so rendering does not hold up guest threads. The
code is in `runtime/src/rsx` (the frame builder) and `runtime/src/render` (the renderer).

- **Frame builder** (`rsx_frame_builder.cpp`). It keeps a host copy of the RSX register file and the
  vertex program and constant stores (`rsx_registers.cpp`), applies every completed register write
  to it, and turns the stream into three kinds of plain commands: clear, draw and flip. The state
  decoders (`rsx_decode.cpp`) read surface, blend, depth, texture, vertex and program state from
  the register file. Each command carries every guest byte it needs (vertex and index data,
  texture texels, shader ucode), so the renderer never touches guest memory. Texture texels are kept
  in a cache between draws (see [Textures](#textures)). Draw boundaries,
  addressing and register meanings follow RPCS3's `Emu/RSX` sources. At a flip the registers are
  reset the way RPCS3 resets them, so the next frame starts from the same state.
- **Renderer** (`vk_renderer.cpp` and neighbours). Vulkan 1.3 with dynamic rendering, the loader
  through volk (`externals/volk`, loaded at run time, so no import library is linked) and memory
  through VulkanMemoryAllocator (`externals/VulkanMemoryAllocator`). Colour and depth surfaces are
  kept in a render target cache keyed by memory location and offset (see
  [Render targets](#render-targets)). A texture whose address is
  a known render target is sampled from that target (render target aliasing), and a draw that
  samples the target it renders to reads a copy of it. Other textures are uploaded from the
  guest bytes into B8G8R8A8 images (A8R8G8B8, linear or swizzled) or BC1/BC2/BC3 images
  (DXT1/DXT23/DXT45), as 2D, 3D or cube images with the whole mip chain (see [Textures](#textures));
  a unit without a usable texture reads a 1x1 magenta image of the type the fragment program samples
  (2D, 3D or cube).
  Cull mode, depth state, viewport, scissor, topology, blend constants and the whole stencil state (test
  enable, ops, compare mask, write mask and reference, for both faces; see [Stencil](#stencil)) are dynamic
  state; pipelines are cached by what is baked into them.
- **Consecutive draws share a rendering instance.** A draw leaves its rendering instance open. The next draw
  with the same colour view, depth view, stencil use and render area records straight into it: no layout
  transition, no barrier and no `vkCmdBeginRendering`. Draws inside one instance are ordered by rasterization
  order, which replaces the barrier between them. Every command recorded outside a draw's instance (clears, texture
  uploads, the copy a draw makes to sample the target it renders to, target creation, resizing and layout syncs,
  the flip and readback commands) takes the frame's command buffer from one function, `outsideCmd`, which ends the
  open instance first. A draw with other attachments ends it before its own transitions, and the frame's submit ends
  it before `vkEndCommandBuffer`. An open instance therefore also means that nothing touched its attachments since
  the last draw, so they are still in their attachment layouts. `RendererStats::rendering_instances` counts the
  instances.
- **Barriers.** A layout transition records a full image barrier (all commands to all commands), except when the
  image is already in the requested layout and that layout is a read-only one: shader read, depth-stencil read or
  transfer source. A read after a read has no hazard, and every write to the image needs another layout, whose
  transition records its own barrier. A transition to the layout the image is in already, for an attachment or a
  transfer destination, keeps its barrier; it orders successive rendering instances over one attachment. A draw that
  continues an open instance makes no transition at all. `RendererStats::barriers` counts the barriers recorded and
  `barriers_skipped` the read-only transitions that needed none.
- **Dynamic state shadowing.** `recordDraw` remembers the pipeline and the dynamic state it last recorded on the
  frame's command buffer (viewport, scissor, cull mode, front face, topology, primitive restart, the depth state,
  the stencil state of both faces and the blend constants) and records a value only when it differs or is unknown;
  floats are compared bit for bit. Dynamic state belongs to the command buffer, not to the rendering instance, so the
  shadow outlives instance boundaries. It is cleared when a frame's recording starts and by every other function that
  binds a pipeline or sets state (the layout pass of a multisampled target, the masked colour clear, the depth
  conversion and the debug overlay), because a pipeline that does not declare a state as dynamic overwrites it. All
  draw pipelines declare the same dynamic states, so binding another draw pipeline keeps the rest. Descriptors and
  vertex and index buffer bindings are recorded for every draw. `RendererStats::state_calls` counts the calls
  recorded (the pipeline bind included) and `state_calls_skipped` the ones left out.
- **Vertex constant uploads.** The vertex program constants of a draw are one shared snapshot (see [Vertex
  programs](#vertex-programs)). The renderer copies it into upload memory once; later draws of the same frame with
  the same non-zero snapshot id bind that upload instead of allocating and copying another one
  (`vp_constant_uploads_reused`). The remembered upload is dropped when a frame's recording starts.
- **Vertex and index conversion.** Guest vertex arrays are converted to host byte order by one loop per component
  size, component count and padding, which loads and stores whole components or elements (the upload memory may be
  write-combined, where single-byte stores are slow). Indices are converted and range-checked in one pass
  (`convertIndicesChecked`): each must lie in the draw's `[min_index, max_index]`, or equal the restart index when
  the draw uses restart. A draw with an index outside that range is skipped and counted as
  `state index outside the vertex range`. `RsxDraw::index_data` stays big-endian in the command; the converted indices
  go straight into the frame's upload buffer.
- **Readback** is gated. Copying the display buffer back to the host costs 8 MiB per flip at 1080p,
  so a live run does it only for flips that are dumped (`--dump-frames`). `tox2_rsxreplay` always
  reads the last flip back. The readback is the display target's image, so at a [resolution
  scale](#resolution-scale) N above 1 it is N times the guest size in each direction (dumps, requested
  dumps, `readDisplay` and the replay PNG alike), and its size and PNG encoding cost grow with N squared.
- **Shader translation** (`vp_translator.cpp`, `fp_translator.cpp`). Vertex and fragment programs
  are translated from RSX microcode to GLSL 4.50 and compiled with shaderc (the SDK's static
  library) to SPIR-V 1.6 for Vulkan 1.3, without optimisation. The result is cached on disk in
  `generated/shader_cache` (`--shader-cache DIR` changes it) as
  `<stage>_<program hash>_<GLSL hash>_<translator version>.spv`. A missing or corrupt file is
  recompiled and rewritten, and the translator version in the name keeps SPIR-V from an older
  translation from being reused. The same directory holds `pipelines.bin`, the driver's pipeline
  cache data: it is loaded at start-up when its header names this device and driver, and written when
  the run stops. Many drivers keep a disk cache of their own, so the gain is small there (in a 150 s
  headless run on an NVIDIA driver, creating 111 pipelines took 95 ms without the file and 13 ms with
  it). A program with an opcode that is not translated skips its draw
  and is counted. The vertex translator is described in [Vertex programs](#vertex-programs). The fragment translator handles MOV, MUL, ADD, MAD, DP3, DP4, MIN, MAX, the six
  compares (SLT to SEQ), FRC, FLR, RCP, EX2, LG2, DIV, DIVSQ, TEX, TXP and KIL, and ignores NOP and
  the FENCT/FENCB fences. TEX and TXP read two coordinates from a 2D texture and three from a 3D or
  cube texture (see [Sampler types](#sampler-types)). RCP, EX2 and LG2 read the x lane of source 0 and replicate the result;
  DIV and DIVSQ divide the whole of source 0 by the x lane of source 1 (DIVSQ leaves a zero
  numerator at zero and ignores the sign of the divisor). KIL discards the fragment when any lane
  selected by its condition passes. Other opcodes (for example RSQ, DP2, LIT, the pack and
  unpack group and the flow-control opcodes), the WPOS, FOGC and SSA inputs, and the texture
  expand bit are not translated.
- **Flips and vsync.** A flip is done when the command executes, once the flip limit allows it: with the default
  `--frame-limit vsync` a flip in the game's VSYNC flip mode waits for the next vblank, otherwise the limit is
  60 flips per second (see [Fake RSX](#fake-rsx)); `--frame-limit off` removes the limit. With the window the displayed buffer is
  also blitted, letterboxed to 16:9, to a swapchain with the FIFO present mode, once per completed flip;
  presenting runs outside the guest lock. The limit paces the flips before they reach the swapchain, so a
  display that refreshes faster than 60 Hz does not speed the game up; with `--frame-limit off` the display's
  refresh paces the RSX thread instead. Without a window nothing waits for a vertical blank in the renderer.
  The [debug overlay](runtime.md#debug-overlay) is drawn on the swapchain image after that blit, in a
  separate pass before the image is presented. It is not drawn into the render target, so it never
  appears in `--dump-frames` files, requested PNG dumps, the display readback or captures, and it only
  redraws when a flip completes.
- **`rsx unimplemented` census.** The decoders, the frame builder and the renderer count what they
  meet but cannot handle yet (surface, depth and texture formats, texture layouts, vertex types,
  primitives, vertex and fragment opcodes, blend modes, other state and unknown methods) under
  `<category> <key>`. The stop census prints them as a `census: rsx unimplemented N kinds`
  section, most frequent first (omitted when empty). It is the work list for later scenes: a draw
  that depends on one of them is skipped or drawn incompletely, and the `skipped` count in the
  `rsx` section says which. A warm 260 s run of the game lists one kind, `state zpass report constant`, and
  skips no draw. Stencil, user clip planes, surface resizing, vertex program condition codes and the
  anti-aliased surfaces (`state antialiasing 0x3`, `texture layout render target alias of another size`) no
  longer appear. The [resolution scale](#resolution-scale) adds kinds that appear only above scale 1 or at the
  device's image size limit, and [anti-aliased surfaces](#anti-aliased-surfaces) add the coverage controls
  the renderer does not model, a multisampled pass over newer layout data (`state anti-aliased pass over layout
  data`) and, on a device without 2-sample support, the fallback to supersampling (`state 2-sample multisampling
  unavailable`). The game's frames count none of the last two.

### Vertex programs

`vp_translator.cpp` translates one vertex program to one GLSL `main()` that ends with the viewport
transform and the varying outputs.

- **Opcodes.** Vector: MOV, MUL, ADD, MAD, DP3, DPH, DP4, DST, MIN, MAX, SLT, SGE, FRC, FLR, SEQ, SFL,
  SGT, SLE, SNE, STR and SSG. Scalar: MOV, RCP, RCC, RSQ, EXP, LOG, LIT, LG2, EX2, SIN and COS, and the
  flow-control opcodes below. Not translated, and the draw is skipped and counted: ARL and relative
  constant addressing, TXL, BRA (its target comes from the address register) and PSH and POP.
- **Condition codes.** Two condition registers, `cc_0` and `cc_1` (instruction bit 25 selects the register
  for both the test and the update), are `vec4` locals that start at zero and are declared only when a
  program uses them. They follow RPCS3's `VertexProgramDecompiler` (`SetDST`, `GetDST`):
  - *Destination.* The register is a fallback destination of one instruction part, the vector and the
    scalar part being decided separately. A part that has a temp, or that owns the output register
    (instruction bit 30 names the owner), writes those and never the register, whatever the update bits say.
    A part with neither writes the register when the instruction has no output at all and update bit 14 or
    bit 29 is set (either one), or when the output belongs to the other part (the update bits are not read).
    Otherwise it writes nothing and is not reported. The value is the part's result after saturation, in
    the part's write-mask lanes.
  - *Conditional writes.* With the test bit (13) set and a condition of 1 to 6 (`<`, `==`, `<=`, `>`, `!=`,
    `>=` against zero), destination lane `l` is replaced only where the compare of condition register
    component `swizzle[l]` holds, as `dest.L = mix(dest.L, value.L, compare.L)`. The register update is
    conditional the same way. Bit 13 clear or condition 7 ("always") writes unconditionally, and condition
    0 ("never") drops the whole instruction, register update included, whatever bit 13 says.
  - *Order.* The vector part is emitted first and the scalar part after it, each testing the register where
    it is emitted, so a scalar part sees the register the vector part of the same instruction just updated.
    A branch in the scalar slot is emitted after both parts.
- **Flow control.** BRB and BRI branch, CAL, CLI and CLB call, and RET returns. The target is a 10-bit
  instruction index; the boolean index (0 to 31) and the branch sense of BRB and CLB share the bits
  of source 2, as in RPCS3's `RSXVertexProgram.h`. BRB and CLB read bit `n` of the branch bits and take
  the jump when the bit is set if the instruction's sense bit is set, and when it is clear otherwise.
  BRI, CAL, CLI and RET are translated when their condition is "always" (a condition of "never"
  skips them); any other condition tests a condition register and is reported as `state vp branch condition
  other than always`, and the draw is skipped. Conditional branches are not translated because no vertex
  program that a 260 s game run reaches uses them; the condition registers above are only used by data
  writes.
  BRB and BRI are translated outside subroutines only; one inside a subroutine is reported.
  The vector operation of an instruction that carries a branch still runs first.
- **Structure of the output.** A program without flow control is emitted as straight-line code, as
  before. Otherwise the body is a `while (true)` loop whose blocks start at the branch targets: block
  `k` runs when `jump_position <= k`, so leaving a block enters the next one, and a branch stores the
  level of its target block and `continue`s. This is the layout RPCS3's `VertexProgramDecompiler`
  uses, and it covers backward branches. An end bit inside the loop is a `break`, so the output
  epilogue still runs. A call is inlined at its call site, up to 8 deep, which needs no call stack.
- **Which instructions are translated.** `analyzeVertexProgramFlow` (`rsx_census.cpp`) follows the
  program from its first instruction: it falls through, follows every branch target, and runs a call
  to its return. The program may extend past its first end bit; the end bit stops the walk only on the
  path that reaches it. The frame builder uses it to cut the program out of the store, zeroing the
  instructions no path reaches (stale words from an earlier, longer program) and rebasing branch
  targets to the first instruction, so that equal programs hash equal wherever they were loaded. A
  target before the first instruction or outside the store is not accepted: the draw is counted as
  `state vertex program branch outside the program` and has no vertex program.
- **Branch bits.** `NV4097_SET_TRANSFORM_BRANCH_BITS` (method 0x1FF8) sits in the register file like
  any other register, so the capture holds its value at the start of the frame and every write to it
  after that, and a replay needs nothing extra. The frame builder copies it into `RsxDraw::vp_branch_bits`
  at each draw and the renderer writes it into the `branch_bits` member of the per-draw uniform block
  (binding 2). It is a uniform, not part of the program or pipeline key, because a 32-bit value in the
  key would build up to 2^32 variants of one program, while a uniform costs a bit test at each branch
  and lets draws that differ only in the bits share a shader module and a pipeline.
- **User clip planes.** `NV4097_SET_USER_CLIP_PLANE_CONTROL` (method 0x1478) holds one nibble per plane
  (plane `n` in bits `4n` to `4n+3`): 0 disabled, 1 enabled "less than", 2 enabled "greater or equal".
  Plane 0 to 2 take their distance from the `y`, `z` and `w` lanes of vertex output slot 5 (`o[5]`;
  `o[5].x` is the fog), planes 3 to 5 from `o[6].y`, `.z` and `.w` (`o[6].x` is the point size). The
  distance is `v` for "greater or equal" and `-v` for "less than", and a primitive is kept where it is not
  negative. A plane is live only when its control nibble is 1 or 2, bit `6+n` of the vertex attribute output
  mask (UC0 to UC5) is set, and the program writes the plane's slot in some lane; a plane that is not live
  has the constant distance 0.5 and cuts nothing (RPCS3 leaves it unassigned). The translator declares
  `gl_ClipDistance[6]` and writes it only when the program writes slot 5 or 6. A slot register starts
  at (0, 0, 0, 1), so an unwritten `.y` or `.z` lane reads 0 and an unwritten `.w` lane reads 1: planes 2
  and 5 then keep everything with "greater or equal" and cut everything with "less than", as in RPCS3. The
  control word is the `user_clip` member of the per-draw uniform block and the output mask is already
  one, so both are evaluated at run time and neither enters the program or pipeline key. The facts come
  from RPCS3's `Core/RSXDrawCommands.cpp`, `Program/GLSLCommon.cpp` and `VK/VKVertexProgram.cpp`.
  - Clip distances need the device feature `shaderClipDistance`, which the context enables when the device
    has it. The translator output depends on it (`VpTranslateOptions::clip_distances`), and the flag is
    constant for a device, so programs are still keyed by their hash. Without the feature a draw is not
    clipped and the renderer counts `state user clip unavailable` when a plane with a control nibble of 1
    or 2 has its output-mask bit set (it does not know which slots the program writes, so this can count
    a little too much). RPCS3 emits the clip writes anyway on such a device and renders wrongly.
  - A nibble above 2 is not valid. RPCS3 aborts on it; here the plane is disabled and the decoder counts
    `state user clip plane op ?0x<nibble>`.
- **Program caches.** The frame builder keeps the programs of earlier draws, so a draw that reuses one does not
  read and hash it again. The result is the same as without the caches, including the census notes.
  - *Fragment programs.* Entries are keyed by the `SET_SHADER_PROGRAM` value (location and offset), up to 1024; the
    map is cleared when full (a heavy frame uses about 650 program addresses). An entry holds the big-endian image
    of the program as hashed, with the inline constant blocks zeroed, the word index of each constant block, the
    decoded words and the hash. A draw reads as many bytes as the image has in one speculative read
    (`RsxMemoryView::tryRead`), decodes the constant blocks into `fp_constants`, zeroes them in what it read and
    compares the rest with the image. Equal bytes are a hit: the draw takes the cached words and hash and skips the
    word-by-word scan. A missing entry, an unreadable range or different bytes fall back to the scan, and a scan that
    reaches the end flag stores the entry. A hit is exact because the scan's shape (which blocks are constants, where
    the end flag is) depends only on the instruction words, and those are the bytes compared; the probe reads only
    addresses the scan would read for an unchanged program. `tryRead` differs from `read` only in that a view which
    counts failed reads, such as the replay view's `capture missing range`, does not count it, so the census is
    the same with and without the cache.
  - *Vertex programs.* Entries are keyed by the start address, up to 32, most recently used first (the last is
    dropped when full). An entry holds the words of the store that the flow walk read (from the start to the last
    reached instruction), the result of the walk (terminated or not, whether a branch target was outside the
    program), the cut and rebased words with their hash, and the name of the program store when the entry last
    matched. A draw hits on the first entry with its start address whose store name is the same or whose words equal
    the live ones; a program uploaded again with the same words gets a new name and still hits through the
    comparison. A hit repeats the notes of the walk (`unterminated vertex program`, `vertex program branch outside
    the program`) so the per-draw census counts do not depend on the cache. A hit is exact because the walk reads
    only instructions from the start to the last reached one, so its result is a function of those words and the
    start address.
- **Generation names.** `RsxRegisters` names the current contents of the program store (`vp_ucode`) and the
  constant store (`vp_constants`) with numbers from a process-wide counter (`vpUcodeGeneration()`,
  `vpConstantsGeneration()`). Applying a write to a store, or resetting the registers, makes its name unknown (0); the
  next call to the accessor gives the new contents a new name. Unchanged contents keep their name, and a write
  counts as a change even when it stores the same value. Two equal non-zero names, from any `RsxRegisters` in the
  process, mean equal contents, so consumers key caches on them without reading the stores. A copy of the registers
  keeps the names, since it holds the same contents. Code that writes the stores directly (the capture reader after
  it fills the registers from the REGS chunk, tests) must call `markVpStoresChanged()`. The names are not part of
  the capture file.
- **Constant snapshots.** A draw carries the vertex program constants as `RsxDraw::vp_constants`, a shared
  snapshot of the whole store, with `vp_constants_id`, the store's name when the snapshot was taken. Draws between
  two constant writes share one snapshot and one id, instead of each copying the 7.5 KB. When the name changes the
  builder copies the registers into the same vector if no draw holds it any more, and into a new one otherwise, so
  a draw still waiting for the renderer keeps the values it saw. A null snapshot reads as all zeros.
- **Limits.** The census program dumps (`vp_*.bin`) still end at the first end bit, and the translator
  tests that read them use programs without flow control.

### Render targets

Colour surfaces are A8R8G8B8 (a B8G8R8A8 image) and F_X32 (surface format code 13, an `R32_SFLOAT` image),
depth surfaces are Z24S8 (the device's D24S8 or D32S8 format) and Z16. Another colour format is counted as
`surface format <name>` and its draws are skipped. The surface size is the clip rectangle. The facts for F_X32
come from RPCS3 (`VKGSRender.cpp` `get_compatible_surface_format`, `rsx_methods.h`, `RSXThread.cpp`):

- The fragment program writes its usual four-component colour and the attachment keeps the red channel.
  RPCS3 applies no output swizzle to a float surface (the swizzle word is only set for the integer formats).
- A float surface has no 8-bit quantisation of the output and no alpha test, so the draw uniform block gets
  neither `kFpFlagRound8Bit` nor the alpha function. The fragment GLSL does not change.
- A colour clear of an F_X32 surface does nothing, as in RPCS3 `clear_surface`; the depth and stencil bits of
  the same clear still apply.
- Blending into `R32_SFLOAT` is optional in Vulkan: the draw is made without blending, and counted
  (`blend blending into a format the device cannot blend`), when the device cannot blend that format.
- A flip of a surface that is not A8R8G8B8 shows black (`state flip of non-render-target`).

The frame builder keeps one list of known surfaces, colour and depth together, and a texture whose address is
in it is an alias of that surface (`RsxTextureDesc::rt_alias`, with `rt_depth` for a depth surface). A colour
surface becomes known when a kept draw writes it (nonzero colour mask) or a clear covers it, a depth surface
when a clear has its depth or stencil bit or a draw can write it (depth test and depth write on, or a stencil
write; see [Stencil](#stencil); RPCS3 `zeta_write_enabled`), so the unused depth binding of a pass that only needs a colour target is not a surface.
A colour and a depth surface at one address share an entry; the later one wins. A capture stores the list at
the start of the frame, the depth format in bits 8 to 15 of the format word of each record (older files have
zero there and still load; a file with a depth record does not load in an older build). RPCS3 also ignores a
depth binding whose pitch is below the row size (`RSXThread.cpp`, `minimum_zeta_pitch`); the frame builder does
not model that rule. The passes of the game's frames that bind such a depth surface (offset 0, pitch 0x40) do not
write depth, so none becomes known.

A texture at a known surface is read from the GPU image, not from guest memory:

- **A8R8G8B8 texture, A8R8G8B8 surface.** The image itself, through a view with the texture's remap. A draw that
  also renders to the surface reads a copy.
- **X32_FLOAT texture, F_X32 surface.** The `R32_SFLOAT` image through a view whose four channels all read the
  red channel; only the remap's forced-zero and forced-one controls change that (RPCS3 `VKFormats.cpp`
  `get_component_mapping` and `RSXTexture.cpp` `decoded_remap`). The sampler uses NEAREST filters when the
  device cannot filter the format linearly (`texture sampler linear filter on a format the device cannot
  filter`). A draw that reads the float surface it renders to is not supported (`texture placeholder:
  feedback copy unavailable`).
- **A8R8G8B8 texture, Z24S8 depth surface.** The RSX reads the surface words as A8R8G8B8 texels: A is depth
  bits 23 to 16, R is 15 to 8, G is 7 to 0, B is the stencil (RPCS3 rebuilds the same bytes in
  `RSXFragmentTextureDepthConversion.glsl`). The renderer rewrites the surface into a B8G8R8A8 image of the same
  size with a full-target pass before the draw (`DrawPipelines::depthConvertPipeline`, reading the depth and
  stencil aspects with `texelFetch` and rounding the depth to 24 bits), and the draw samples that image through
  its remap like any A8R8G8B8 texture. The image stays with the depth target and is made again only after a
  clear or a draw that can write the depth or the stencil (the same zeta write rule as for known surfaces; so a
  stencil-only pass makes the next read rebuild the copy, whose B channel is the stencil;
  `RendererStats::depth_conversions` counts the passes). It is a
  separate image, so a draw that renders with that depth surface bound can read it.
- Not supported and counted: a texture format that does not match the surface kind (`texture format
  <name> of a depth surface`, `texture layout render target alias of another format`), DEPTH24_D8 and DEPTH16
  textures, an A8R8G8B8 read of a Z16 surface (`texture format A8R8G8B8 read of a surface without Z24S8`), an
  X32_FLOAT texture whose address is no known surface (`texture format X32_FLOAT_LN_NR`; its bytes are never
  copied), and a surface that the renderer has not drawn to (`texture alias of unknown render target`: a
  colour target is then created empty, a depth surface reads the placeholder).

A texture and its surface of different sizes are sampled with the texture's normalized coordinates and counted as
`texture layout render target alias of another size`. The sizes compared are the texture's and the surface's memory
layout (see [Anti-aliased surfaces](#anti-aliased-surfaces)), so a texture that reads an anti-aliased surface as
the wide image it is in memory is not counted. What remains is an approximation (a texture that covers part of a
surface or is another size); the game's frames do not count it.

#### Image reuse

A target is replaced by a new image whenever a pass binds its address with another size or format, and this game
binds the depth surface at `0x800000` with four different sizes in one frame (1280x720 four times, 640x360 three
times, 64x64 and the 2560x720 anti-aliased one). A scene of the game created about 12 targets, one sidecar and
one depth-as-A8R8G8B8 image per flip, 42 MB at scale 1 and 381 MB at scale 3 (18.6 GB/s), and each was a kernel
allocation. Images that are retired now go to a pool (`runtime/src/render/image_pool.*`) and the next
creation of the same format, size, sample count and usage takes one from it:

- **What is pooled.** Render-target images, the multisampled sidecars of anti-aliased surfaces and the
  A8R8G8B8 copy of a depth target (see above). Textures and upload blocks are not (they were created almost never
  in the measured scenes). A reused image starts in the undefined layout and is cleared like a new one, so the
  output is the same; all replay captures still match their reference images.
- **When an image comes back.** A replaced target stays alive until the frame slots that touched it have waited
  for their fence, and only then is its image given to the pool. With two frames in flight, the images retired
  while recording a frame come back at the start of the frame after next, so the pool holds about one frame's worth
  of images in steady state.
- **Limits.** At most 8 images per key (the largest per-frame count of one key seen is 4), at most 768 MB
  in total (a frame at scale 3 is 316 to 459 MB; at scale 1 the limit does not bind), and an entry that is not
  taken within 300 flips (about 6 s) is destroyed, which releases the sizes of a scene that has ended or of
  an earlier resolution scale. The oldest entry of a key is handed out first, so every entry rotates.
- **Measured.** In a scripted run of this game at scale 3 (about 53 flips per second), the creations per
  flip fell from 381 MB to 0.08 MB after the first two log lines, with about 12 target and 1.2 depth-argb hits
  and 1 sidecar hit per flip, and the whole-run flip rate rose from 48.7 to 53.8 per second. At scale 1 it rose
  from 50.5 to 52.9 per second (measured with only targets and sidecars pooled).

With `--log-level debug` the renderer logs, every 600 flips, the images created in that interval and the pool
activity:

```
tox2: render: last 600 flips: created target 1 (0.0 MB), sidecar 0 (0.0 MB), depth-argb 0 (0.0 MB), texture 0 (0.0 MB), upload-block 0 (0.0 MB); pool hits target 7404, sidecar 600, depth-argb 732; misses no-entry 0, key-cap 0, byte-cap 0, idle 0; discarded key-cap 0, byte-cap 0, idle 0; pooled 301.5 MB
```

(Invented numbers, for the shape.) The creations of target, sidecar and depth-argb together equal the sum of the four miss counts. A
miss `no-entry` means no image of that key had been given back and discarded since its last use, as at a scene
change or while the images of the frame before are still in flight; `key-cap`, `byte-cap` and `idle` mean that an
image of the key was given back and destroyed by that limit, so the creation is one that a larger limit would
have saved. `discarded` counts the destroyed images whether or not a creation followed, and `pooled` is the
size of the pool at the last flip. `RendererStats` carries the same figures (`images_created`,
`image_bytes_created`, `image_pool_hits`, `image_pool_misses`, `image_pool_bytes`, `image_pool_discards`).

#### Resolution scale

`--resolution-scale N` (`tox2_game` and `tox2_rsxreplay`; a whole number from 1 to 4, default 1) and the
overlay's Settings control `Internal resolution scale` (see [Debug overlay](runtime.md#debug-overlay)) render
every colour and depth target at N times its guest size. The frame builder and the capture format know nothing
about it, so any capture replays at any scale.

- **Sizes.** A target has a layout size (the guest memory layout: the clip rectangle times the samples per pixel
  of an anti-aliased surface), a scale (image pixels per layout pixel) and an image size (layout times scale).
  The cache identity is the memory location, the offset, the kind (colour or depth), the layout size and the
  format; the scale is the renderer's choice and not part of it.
- **Passes.** The vertex shader still divides by the guest clip size, so only the Vulkan viewport carries
  pixels: viewport, scissor, clear rectangles and the render area are multiplied by the pass's pixel scale (the
  target's scale times the samples per pixel; the scale alone for a multisampled pass, whose image is the clip
  size). Clears, masked clears, scissors, render target textures,
  feedback copies, depth read as A8R8G8B8, inherit copies and the present blit all follow, so the window shows
  a sharper picture of the same frame. A texture that aliases a target is sampled with normalized coordinates
  (or unnormalized ones divided by the texture's guest size), so an image of any size is read at the same
  place; the A8R8G8B8 image made from a depth target has the size of the depth target's layout image (layout times
  scale).
- **Changing the scale.** The overlay's choice is a request that the overlay hands back to the renderer at
  the next flip, and the renderer applies it between frames, right after that flip. Targets are not touched then. When a draw
  or clear binds a target whose scale is no longer the current one, the renderer replaces it with a new image
  at the current scale and fills it from the old one with a whole-image `vkCmdBlitImage`: NEAREST when
  enlarging (replicating texels is exact), LINEAR when shrinking a colour target (an exact 2:1 is a box
  filter), NEAREST for float and depth targets. A format without blit support keeps its initial contents and is
  counted (`state rescale of a colour target unavailable`, `state rescale of a depth target unavailable`). The
  new target starts without its A8R8G8B8 depth copy and sampled views, which are made again on demand. A target
  that is only sampled or flipped keeps its old scale until it is bound again; every read path uses the
  target's own image size, so a mix of scales is harmless.
- **Size limit.** A target whose layout times the scale would exceed the device's largest image is made at the
  largest scale that fits and counted (`state surface too large for the resolution scale`). A draw whose colour
  and depth attachments then have different scales is skipped (unless it neither tests nor writes depth, in
  which case it runs without the depth attachment) and a clear of such a pair is not recorded; both are counted
  as `state attachments at different resolution scales`. Copying the overlap of an old surface into a resized
  one needs the same scale; across two scales the copy is skipped and counted (`state surface resize across
  resolution scales`).
- **Readback and comparisons.** Dumps, `readDisplay` and the replay PNG are at image size, so a scaled run's
  dump shows the scaled picture and the flip path needs no extra pass. `tox2_rsxreplay --compare` box-averages a
  frame that is a whole multiple of the reference down to its size first, and the render tests do the same
  (`downsampleBox`, `downsampleBoxBgra`). A flip of a buffer that was never a render target reads back black at
  the flip size times the scale.
- **Points and lines.** Point sprites follow the vertical pixel scale: the vertex shader multiplies
  `gl_PointSize` by the `w` lane of the viewport scale uniform, which the renderer sets to that scale (the lane
  is unused by the RSX). That needs the device feature `largePoints`, which the context enables when present;
  without it `gl_PointSize` is clamped to 1 and a point draw that should be larger is counted as `state point
  size unavailable`. This also makes a point of SET_POINT_SIZE above 1 that large at scale 1. Lines stay one
  image pixel wide at any scale (SET_LINE_WIDTH is not decoded, and wide lines are not enabled), so they are
  thinner than the guest's at scales above 1. No frame of the game draws points or lines.
- **Not modelled.** Fragment inputs that depend on the pixel position (WPOS, derivatives) are not translated;
  they would have to be divided by the pass's pixel scale, and so would a polygon offset slope. Nothing writes
  surfaces back to guest memory.
- **Cost.** Memory grows with N squared: a 1280x720 colour target is 3.7 MB at scale 1 and 59 MB at scale 4, and
  there are many targets, depth copies and feedback images (retired images are reused, see
  [Image reuse](#image-reuse)). An allocation failure stops the renderer.

#### Anti-aliased surfaces

The AA field (bits 12 to 15) of SET_SURFACE_FORMAT selects a sample layout while the clip rectangle stays in
pixels. Mode 0 is one sample, mode 3 (RPCS3's `diagonal_centered_2`) stores two samples per pixel side by side,
so the surface is 2W x H in memory, and modes 4 and 5 (`square_centered_4`, `square_rotated_4`) store 2x2
samples, 2W x 2H. The pitch covers the expanded row, and colour and depth share the field. The hardware shades
once per pixel and stores the colour in each covered sample; nothing resolves the surface for display, and the
game resolves it itself by sampling it as a 2W x H texture with a linear filter. The exact sample positions, and
which of the two samples the hardware stores in the left column, are not known here (two on a diagonal through
the pixel centre for mode 3; the renderer assumes +-1/4 pixel on both axes). SET_ANTI_ALIASING_CONTROL (method 0x1D7C) holds the multisample
enable (bit 0), alpha to coverage (bit 4), alpha to one (bit 8) and the sample mask (bits 16 to 31); the decoder
reads all of them. RPCS3 sizes such surfaces by `width * samples` when it matches textures to them
(`Common/surface_store.h`); its "Auto" mode renders into a multisampled image and rewrites the samples into the
PS3 layout when the surface is read as a texture (`VK/VKResolveHelper.*`), as far as these notes recall; the
details were not checked.

The game uses one pair: a 1280x720 A8R8G8B8 colour surface and its Z24S8 depth surface in mode 3 with the
control word 0xFFFF0001 (multisampling on, no alpha to coverage, no alpha to one, full mask), cleared once per
frame and drawn by the 3D passes. A later resolve pass samples the colour surface as a 2560x720 texture, and
the depth surface is read as a 2560x720 A8R8G8B8 texture inside and after the pass.

The renderer has two ways to draw such a surface, chosen by `--aa-mode` (see
[Capture and replay](#capture-and-replay) and the overlay's Settings, [Debug overlay](runtime.md#debug-overlay)).
Both apply to mode 3, the only one the game uses; the 2x2 modes (4 and 5) are always drawn as supersampled
single-sample images, 2W x 2H in memory. Other values of the AA field are counted (`state antialiasing 0x<n>`) and
drawn with one sample per pixel.

- **`supersample`.** The surface is a single-sample image in its memory layout, with one image pixel per stored
  sample (2560x720 at scale 1) and a pixel scale of 2 by 1 times the resolution scale. The clip rectangle, the
  viewport and the scissor grow by the samples per pixel, and the depth surface gets the same layout. The game's
  resolve reads every sample exactly where the RSX stores it, so near-vertical edges come out smoothed. Textures,
  inherit copies, feedback copies and the depth conversion need nothing special because the memory layout is the
  image layout, and an anti-aliased binding of a W x H clip and a plain binding of 2W x H at the same address are the
  same image.
- **`msaa` (the default).** Colour and depth are also drawn into 2-sample multisampled images of the clip size
  times the resolution scale, shaded once per pixel with two coverage samples, like the hardware. A target keeps
  its single-sample memory-layout image as well (its identity, size and every read path are as in `supersample`)
  and the multisampled image is a sidecar of it. Multisampled passes do not draw into the layout image; it is
  written from the sidecar by a full-target pass at the moment something reads it, and not before:
  - a colour target is copied by a draw that fetches each layout texel from the sidecar; a depth target by a
    depth pass plus, for Z24S8, eight passes that rebuild the stencil one bit at a time with a stencil-test
    write mask (no extension is needed for either);
  - the readers are a texture that aliases the surface (including a draw that samples the surface it renders into:
    it reads the layout image and draws into the sidecar, so it needs no feedback copy), an inherit copy or a
    rescale blit from it, a flip of it, and a single-sample pass that is about to draw into the same memory (a plain
    2W x H binding). `RendererStats::layout_copies` counts these copies;
  - the depth surface read as A8R8G8B8 (the game's edge effect) is converted straight from the multisampled depth
    and stencil, without the layout copy.

  Texel `(x, y)` of the layout image comes from pixel `(x >> 1, y)` of the sidecar, sample 1 for even `x` and
  sample 0 for odd `x` (the Vulkan standard 2x positions put sample 1 at (0.25, 0.25) and sample 0 at (0.75, 0.75),
  so the left column is the left sample; which order the RSX uses is not known). That interleave holds at every
  resolution scale, which a copy of s x s blocks per sample would not: the game's resolve samples the layout at
  the boundary between the two texels of one pixel, so with the interleave its linear filter averages the two
  samples of that pixel, and with blocks it would average two neighbouring sub-pixels of one sample and smooth
  nothing. The layout image is never copied back into the sidecar. A multisampled pass that starts on a target whose
  layout image holds newer data (an inherit or a rescale wrote it) and that does not begin with a full clear
  draws over the older sidecar contents and is counted (`state anti-aliased pass over layout data`); the game's
  first pass over its surface each frame is the full clear, so its frames do not count it.

  A device without 2-sample support for every framebuffer and sampled-image count of colour, depth and stencil, and
  for the colour and depth formats the renderer uses, runs in `supersample` instead: one `tox2: render: 2-sample
  multisampling unavailable (<reason>)` warning and the census kind `state 2-sample multisampling unavailable`.
  The multisampled images use the standard sample positions, since no position of the hardware is known to
  match.

Differences from the hardware:

- In `supersample` shading runs once per sample instead of once per pixel, so texture detail is slightly sharper
  after the game's resolve (interior texels are sampled at two positions and the mip level comes from
  per-sample derivatives), with faint vertical streaks in textured areas, and the two samples sit at (0.25, 0.5)
  and (0.75, 0.5) of the pixel instead of on a diagonal, so the smoothing is horizontal only. In `msaa` the streaks
  are gone (in a textured wall of the 3D room capture, 13% of the pixels differed from the one-sample picture by
  more than 2 in `supersample` and under 0.1% do in `msaa`) and edges are smoothed vertically as well.
- With a diagonal sample pattern an edge that runs parallel to the diagonal gets no anti-aliasing, because both
  samples fall on the same side of it. The hardware pattern would behave the same, whichever diagonal it uses;
  which one that is is not known, and a mirrored choice would smooth the other family of edges. Only the
  depth reads that sample one layout column (the edge effect) see the column order.
- The multisample enable bit clear, alpha to coverage, alpha to one and a sample mask other than 0xFFFF are decoded
  but do not change the rendering (a draw with the enable bit clear is shaded and covered like any other). They are
  counted when a draw uses them on an anti-aliased surface (`state alpha to coverage`, `state alpha to one`,
  `state sample mask 0x<mask>`); the game writes none of them.
- Custom sample positions (`VK_EXT_sample_locations`) are not used.

#### Stencil

The stencil state is decoded from the registers SET_STENCIL_TEST_ENABLE, SET_STENCIL_MASK, SET_STENCIL_FUNC,
SET_STENCIL_FUNC_REF, SET_STENCIL_FUNC_MASK and SET_STENCIL_OP_FAIL/ZFAIL/ZPASS for the front face,
SET_TWO_SIDED_STENCIL_TEST_ENABLE, and the matching SET_BACK_STENCIL_* registers for the back face. The
reference, compare mask and write mask are the low 8 bits of their registers. The facts come from RPCS3
(`RSXThread.cpp`, `VK/VKGSRender.cpp`, `VK/VKDraw.cpp`, `Common/gcm_enums.h`).

- **Two faces.** With two-sided stencil off the back face uses the front function, reference, masks and ops;
  the decoder copies the front face, so the back registers are read, and validated, only when two-sided
  stencil is on. Vulkan's front and back faces follow the dynamic front face, which maps one to one to the RSX
  front face.
- **Stencil ops.** The op word is compared in full (nothing is masked to 16 bits): 0x1E00 KEEP, 0x0000 ZERO,
  0x1E01 REPLACE, 0x1E02 INCR (increment and clamp), 0x1E03 DECR (decrement and clamp), 0x150A INVERT,
  0x8507 INCR_WRAP and 0x8508 DECR_WRAP.
- **Compare words.** 0x200 NEVER to 0x207 ALWAYS in `VkCompareOp` order. RPCS3 ORs the word with 0x200 before
  it checks the range, so 0x000 to 0x007 are valid too; the rule is shared by the stencil, depth and alpha
  functions.
- **Unknown words.** RPCS3 aborts on a compare or op word outside these sets. Here it is counted while the
  test is on (`state stencil func ?0x...`, `state stencil op ?0x...`, with the word) and the renderer uses
  `func & 7` for a compare word and KEEP for an op word.
- **Test and depth test.** The stencil test runs whether or not the depth test is on; with the depth test off
  the depth test passes, so the ZPASS op applies.
- **When the test is off.** The test is applied only to a Z24S8 surface. With the stencil test enabled and a
  Z16 depth surface or no depth surface, the draw runs without it and is counted as
  `state stencil test without a stencil surface`. Every draw gives all five dynamic stencil states of both
  faces a value, also with the test off, so no draw inherits the state of the previous one (the call is left out
  when the command buffer already holds that value, see [Rendering](#rendering)).
- **Zeta write rule.** A draw writes the depth surface when `(depth test && depth write) || stencil write`
  (RPCS3 `zeta_write_enabled`). A stencil write needs a Z24S8 surface and the stencil test on, and a write
  mask that is not 0 together with at least one op that is not KEEP. The compare function is not consulted,
  and a nonzero write mask alone is not enough. The back face is folded in only when two-sided stencil is on
  and the front mask is 0 or its ops are all KEEP; its mask and its ops are then ORed in separately, so a
  front face with a nonzero mask and all-KEEP ops plus a back face with mask 0 and a REPLACE op counts as a
  write, as in RPCS3. Unknown op words count as KEEP here. The frame builder uses the rule to make the depth
  surface known, and the renderer to advance the write counter that makes the A8R8G8B8 copy of the depth
  surface be rebuilt.
- **Clears.** The stencil clear value is bits 0 to 7 of SET_ZSTENCIL_CLEAR_VALUE (depth is bits 8 to 31). The
  stencil bit of a clear is dropped for a write mask of 0 and for a surface that is not Z24S8; a new depth
  target starts at depth 1.0 and stencil 0xFF; every clear of depth or stencil advances the write counter.
  A clear with a partial stencil write mask clears the whole byte (RPCS3 uses a shader pass for it) and is
  counted as `state partial stencil clear mask`.

#### Initial contents and resizing

The renderer never loads a surface from guest memory, so a target it creates starts defined: colour is opaque
black (0, 0, 0, 1), depth 1.0 and stencil 0xFF, which are RPCS3's values for a surface with nothing inherited
and "read color/depth buffers" off (`VK/VKRenderTargets.cpp`, `rpcs3/Emu/system_config.h`).

A target bound at the location and offset of an existing one with another width, height or format replaces
it and inherits the overlap, as RPCS3's surface store does (`Common/surface_store.h`,
`Common/surface_utils.h`, `VK/VKRenderTargets.*`). The rectangle from (0, 0) to the smaller width and
height is copied unscaled with `vkCmdCopyImage`, the old image going to `TRANSFER_SRC_OPTIMAL` and the new one
staying in `TRANSFER_DST_OPTIMAL` behind a full barrier, when:

- the row pitch is the same (the rows of both lie at the same addresses), and
- the formats can be copied bit for bit: equal formats (a depth target copies depth and stencil together), or
  A8R8G8B8 and F_X32, which hold the same 32-bit words in memory.

The copy is skipped when the command that creates the new target is a clear that overwrites all of it: every
colour channel over the whole target, or the depth bit plus the stencil bit (when the format has stencil)
over the whole target. The stencil bit counts after it has been dropped for a write mask of 0, so a Z24S8
clear under stencil mask 0 still copies the old stencil. Draws never skip the copy.
`RendererStats::surface_inherits` counts the copies. Pairs that are not copied leave the new target at its
initial contents and are counted, unless a full clear made the copy unnecessary:
`state surface resize with another pitch` and `state surface resize to an incompatible format` (today only Z24S8
to Z16: the supported colour formats are both 32-bit, and colour and depth targets are kept apart, so a colour
target never replaces a depth one).

Differences from RPCS3:

- RPCS3 copies lazily, at the first barrier that needs the data; the copy here is recorded when the new
  target is created, so a target that is never read or written afterwards still costs a copy.
- RPCS3 keeps the parts of a larger old surface that lie outside the new extent as separate surfaces at their
  own addresses; they are dropped here.
- RPCS3 converts other-bytes-per-pixel colour, other depth formats and colour/depth pairs through a
  typeless transfer; here other depth formats are counted, and the other two cases cannot arise (see above).
- RPCS3 samples guest memory at each write and drops an inheritance that guest writes made stale; a guest write
  between two bindings is not detected here.
- The layout overlap is what is copied, so the game's anti-aliased depth surface (2560x720 in memory) that is
  later bound plain at 1280x720 with the same pitch keeps its left half of each row. A copy needs the two
  targets to have the same [resolution scale](#resolution-scale). When the old surface is multisampled, it is
  first copied into its memory layout (see [Anti-aliased surfaces](#anti-aliased-surfaces)), and that layout is
  what the new target inherits.

### Textures

The frame builder converts the bytes of every 2D, 3D and cube A8R8G8B8 and DXT1/DXT23/DXT45 texture a
draw has enabled (`rsx_texture_layout.cpp`), and the renderer uploads them (`vk_textures.cpp`). The
dimension is the field in bits 4 to 7 of SET_TEXTURE_FORMAT (1 = 1D, 2 = 2D, 3 = 3D) and the cube bit is
bit 2. As in RPCS3 (`get_extended_texture_dimension`), dimension 3 is a 3D texture whatever the cube bit
says, and dimension 2 with the cube bit is a cube map. A 1D texture, or a dimension outside 1 to 3, is
counted as `texture layout dimension N` and a draw that samples one is skipped. The depth of a 3D
texture is the top 12 bits of SET_TEXTURE_CONTROL3 (the pitch register holds the pitch in the low 20
bits); a depth of 0 counts as 1. The guest layout follows RPCS3 (`Emu/RSX/Common/TextureUtils.cpp`,
`RSXTexture.cpp`, `Utils/image_utils.hpp`):

- **Levels.** The register's mip count (0 counts as 1) is clamped to the chain that ends at one
  texel, or at one 4x4 block for DXT. Each level is `max(size / 2, 1)` in both dimensions.
- **Linear (LN flag).** Every level keeps the pitch register of level 0 (0 means the row size), so a
  level takes `pitch * rows` bytes and the next one follows without padding. The last row of a level
  is read without the padding after it. A pitch below one row is counted, not uploaded.
- **Swizzled (no LN flag).** A level is `width * height` texels with no row padding, in Morton order:
  the x and y bits are interleaved up to the shorter side and the remaining bits of the longer side
  count above them (an 8x2 texture holds `x0 | y0 << 1 | (x >> 1) << 2`). The size must be a power
  of two; other sizes are counted as `texture layout swizzled non-power-of-two`.
- **DXT.** Never swizzled. Without the LN flag the block rows of a level are contiguous (the pitch
  register is ignored); with it they use the pitch register like a linear texture.
- **Cube maps.** Six faces, each with its own mip chain, in the order +X, -X, +Y, -Y, +Z, -Z (the
  order of the Vulkan array layers). A texture without the LN flag aligns each face to 128 bytes. The
  faces must be square.
- **3D textures.** A level is `depth` slices of the 2D level one after the other (a linear level keeps
  the pitch of level 0 for every row of every slice), and the depth halves, to at least 1, from one
  level to the next like the sides. A swizzled A8R8G8B8 level interleaves one bit of x, y and z per
  round (x first) while a coordinate has bits left (`calculate_z_index`), so its width, height and depth
  must be powers of two (otherwise `texture layout swizzled non-power-of-two`). A DXT level whose width
  and height are powers of two and whose depth is above 1 is stored in the NVIDIA VTC order, which
  Vulkan cannot read directly: the blocks of one position in 4 consecutive slices are adjacent, and the
  last group holds the remaining 1 to 3 slices (`copy_unmodified_block_vtc`). The frame builder puts
  the slices back in plain order. Other DXT 3D levels are plain slices of block rows. The RPCS3 loader
  applies the VTC rule to every DXT 3D level that meets the conditions above, with or without the LN flag,
  and so does this one.

The converted texture, `RsxTextureDesc::data`, is a blob shared between draws: levels one after the other
(a cube map face after face), each in tightly packed rows and slices and de-swizzled, so the renderer needs
only the texture size, depth, kind and mip count to find them. It uploads all levels in one
`vkCmdCopyBufferToImage` into an image with the full chain: a 2D image, a 3D image (`VK_IMAGE_TYPE_3D`, a
copy region per level with the level's depth) or a cube map (a 2D image of six array layers with
`CUBE_COMPATIBLE`, a region per face and level and a cube view). A8R8G8B8 words are byte swapped first and
DXT blocks are copied as stored. The image cache key includes the depth and the kind, so a 3D and a 2D
texture at one address do not share an image. A texture of the render target cache has one level; a 3D or
cube texture at a render target address is counted (`texture layout render target alias of a 3D or cube
texture`) and reads the placeholder.

**Texture cache of the frame builder** (`rsx_texture_cache.cpp`). Copying and converting every enabled unit
for every draw dominated the cost of UI screens (a frame has thousands of draws that enable the same
handful of textures, some of them several megabytes), so the builder keeps the converted blobs:

- *Key.* Everything that changes the packed bytes: location, offset, the format byte (it carries the
  linear and normalized flags), the linear flag, width, height, depth, mip count, the pitch register and the
  face count. Two descriptors that differ in any of them get separate entries; the filter, address, remap
  and border words only affect sampling and are not part of the key. A texture at the address of a known
  render target is an alias and never reaches the cache.
- *Entry.* The shared blob, a content id, a 64-bit hash of the guest bytes it was made from, the guest range
  and the layout. The content id comes from a process-wide counter, so two blobs never share an id, whichever
  builder made them. A draw carries the blob and the id in its `RsxTextureDesc`.
- *Epochs.* Guest memory can change between draws and the builder cannot see it, so an entry is trusted only
  inside an epoch. A new epoch starts at every flip, every `NV406E_SEMAPHORE_ACQUIRE` and
  `NV406E_SEMAPHORE_RELEASE`, every `NV4097` semaphore release, and on the inline blit and image-in methods
  (`NV308A_COLOR`, `NV3089_IMAGE_IN`, `NV0039_BUFFER_NOTIFY`): the points where the game learns how far the
  RSX has got, or the RSX writes memory itself. A game has to synchronize with the RSX before it reuses
  texture memory the RSX may still read, so a change it makes behind the RSX's back shows up at the next of
  these points (this title does two acquires per flip); a guest write inside an epoch, with none of them
  in between, is not seen until the epoch ends. Inside an epoch an entry is used without reading guest
  memory. On the first use in a later epoch the guest range is read into a scratch buffer and hashed
  (`rsxHashBytes`, four multiply-fold lanes over 64 bytes per step); an unchanged hash keeps the blob and
  its id, a changed one converts a new blob under a new id. The reads go through the memory view, so a
  capture records them where the replay builder, which starts with an empty cache, makes the same reads.
- *Writes by the RSX.* A color or depth surface that a clear or a draw writes ends the trust in every entry
  whose range overlaps it, at once and without a new epoch; such an entry is read and hashed again at its
  next use. (The renderer keeps surface contents on the GPU, so the guest bytes are normally unchanged and
  the hash keeps the blob.)
- *Bounds.* Blobs are kept up to 256 MiB; past that the least recently used entries go. Every 32 flips
  entries unused for 600 flips are dropped. A draw that still holds a dropped blob keeps it alive.

The renderer uploads from a blob only when the image does not hold it already: each `TextureImage` remembers
the content id it was last given and `prepareTextures` skips the upload when the id matches (an id of 0,
used by hand-made descriptors, always uploads). `RendererStats::texture_uploads` counts the uploads. An upload
that replaces content mid-frame is ordered after the draws recorded earlier by the image barrier of
`recordUpload`, whose source scope is all commands.

The sampler follows RPCS3's `update_fragment_sampler`: the mip chain is sampled only when the min
filter is one of the four mip filters (NEAREST_NEAREST, LINEAR_NEAREST, NEAREST_LINEAR,
LINEAR_LINEAR) and the texture has more than one level. The level range is the min and max LOD of
SET_TEXTURE_CONTROL0 clamped to the last level, the bias is the signed 5.8 field of
SET_TEXTURE_FILTER (rounded to half a level for a NEAREST mip mode), and the mip mode is LINEAR for
NEAREST_LINEAR and LINEAR_LINEAR. Otherwise the sampler reads level 0 only. Anisotropic filtering
and texels around the image (`CELL_GCM_TEXTURE_BORDER_TEXTURE`) are not modelled.

#### Sampler types

The fragment program declares one sampler type per sampled unit: `sampler2D`, `sampler3D` or
`samplerCube`, from the kind of the texture that is bound to the unit when the draw is made. The types
are 2 bits per unit (`fpTextureType` in `fp_translator.h`) and are an input of the translation, as in RPCS3,
where the texture state of a draw is part of the fragment program key. The renderer does not bind a
unit to an image of another type, because the shader and the descriptor would disagree; the choice made is
to build program variants, not to bind by unit type:

- `drawTextureTypes` (`vk_pipelines.cpp`) reads the kinds from the draw's textures. Most draws have only
  2D textures, so the word is 0 and the program is the one translated for the ucode, control words and
  texcoord control as before.
- For a draw with a 3D or cube texture on some unit, the program is first translated with all units 2D,
  which says which units it samples, and then with the types of the sampled units alone. A texture on a
  unit the program does not sample therefore does not cause a variant. The variant is keyed by the ucode
  hash, the two control words and the types (also in the name of the SPIR-V cache file and, through
  `PipelineKey::fp_texture_types`, in the pipeline key), so one program used with a 2D and a 3D texture on
  a unit is two shader modules and two pipelines.
- TEX reads `xy` (2D) or `xyz` (3D, cube) of the coordinate register. TXP divides them by `w` first. The
  unnormalized-coordinate scale (`draw.texture_scale`) multiplies the 2D and 3D coordinate (its z is
  1 / depth for an unnormalized 3D texture); a cube direction is not scaled. The fragment translator
  handles TEX and TXP only, so these are the only lookups with a type; a texture-bias, texture-LOD or
  texture-gradient opcode is reported as not translated.
- A unit that is sampled but has no usable texture (no bytes, a render target alias, an image that cannot be
  created) reads a 1x1 magenta placeholder of its declared type: a 2D image, a 1x1x1 3D image or a cube
  map of six 1x1 faces. A unit without a texture is 2D.
- When SET_TEXCOORD_CONTROL declares coordinate n as 2D, the fragment input `in_texN` has z = 0 and w = the
  fragment's w whatever the vertex program wrote, so a 3D lookup through it reads slice 0 and a cube
  lookup reads the direction (x, y, 0).

### Capture and replay

`--rsx-capture N[,M...]` records the RSX inputs of the given frames: the numbers are 1-based flip
numbers, and each capture is written when its frame's flip completes, to
`generated/rsx/captures/frame_<N>.tox2cap` (relative to the working directory). A capture holds the
register writes from the previous flip to this one, the guest bytes the frame builder read while
handling them, and the register and surface state the frame started from. It holds only the bytes
the frame builder read when it was recorded, so a replay with a builder that reads more (such as
the textures it did not copy before the mip and swizzle support) counts the missing ranges as
`capture missing range` and draws those textures as placeholders; record the frame again. It needs
no renderer and no window. Captures are derived from the game, so they are git-ignored with the rest of
`generated/`.

`tox2_rsxreplay` replays a capture through the frame builder and the renderer without the game:

```sh
tox2_rsxreplay CAPTURE [--out DIR]
                      [--compare PNG [--compare-rect X,Y,W,H] [--max-diff N] [--max-mean X] [--max-differing P]]
                      [--resolution-scale N] [--aa-mode supersample|msaa] [--no-validation] [--shader-cache DIR]
                      [--repeat N] [--quiet]
```

| Option | Meaning |
| --- | --- |
| `CAPTURE` | A `.tox2cap` file. |
| `--out DIR` | Write the displayed frame of the last flip as `DIR/<capture name>.png`. |
| `--compare PNG` | Compare that frame with a reference image. The sizes must match, except that a frame whose width and height are whole multiples of the reference's is box-averaged down to it first (the summary line then ends with `; downsampled <fx>x<fy>`). |
| `--compare-rect X,Y,W,H` | With `--compare`: diff only the `W` x `H` rectangle at `(X, Y)`, in the reference's pixels (the rectangle is cut from both images after the downsample, and the summary line gets `; rect <W>x<H>+<X>+<Y>`). A malformed value is a usage error; a rectangle that is empty or outside the image is an error (exit code 1). |
| `--max-diff N` | With `--compare`: fail when the largest per-channel difference exceeds `N`. |
| `--max-mean X` | With `--compare`: fail when the mean per-channel difference exceeds `X`. |
| `--max-differing P` | With `--compare`: fail when more than `P` percent of the pixels differ (by more than 2 in any channel). |
| `--resolution-scale N` | Render every target at `N` times its guest size (1 to 4, default 1; see [Resolution scale](#resolution-scale)). The PNG is `N` times the guest size, and the summary line ends with `resolution scale N` when `N` is above 1. |
| `--aa-mode supersample\|msaa` | How surfaces in the game's anti-aliased mode are rendered (default `msaa`; see [Anti-aliased surfaces](#anti-aliased-surfaces)). `supersample` is the single-sample memory-layout image shaded per sample. `msaa` falls back to it on a device without 2-sample support (`state 2-sample multisampling unavailable` in the census). The summary line gets `, aa supersample` when the effective mode is `supersample`, and nothing for `msaa`. Any other value is a usage error. |
| `--no-validation` | Do not enable the Vulkan validation layer, which is on by default. |
| `--shader-cache DIR` | SPIR-V cache directory. Without it nothing is cached. |
| `--repeat N` | Replay and render the capture `N` times (1 to 100000) and print the median time of each phase (see below). A usage error together with `--out` or `--compare`; exit code 1 without a Vulkan device. |
| `--quiet` | Do not print the per-command lines (`clear`, `draw`, `flip`); everything else is printed. |

The thresholds are optional and unchecked unless given, and they need `--compare`. The tool prints
the commands the frame builder produced (`clear`, `draw` with its colour and depth target (with `zt=`, `zw=` and
`st=` for its depth test, depth write and stencil test) and, for each texture, the render target it aliases or the format and mip count of its bytes, `flip`), then a summary line
`replay: N commands, N draws, N skipped` (with the diff figures when comparing), the
`rsx unimplemented` section (`census: rsx unimplemented 0 kinds` when it is empty), the path of
the PNG, and `validation: active|inactive|disabled, N errors`. The exit code is 1 when a
threshold is exceeded, a file cannot be read or written, there is no frame to display or the
validation layer reported an error, and 2 for a usage error. Without a usable Vulkan device it
still prints the command list and counts every draw as skipped.

`--repeat N` measures the frame builder and the renderer without the game. One process runs the capture `N` times
with one memory view, one frame builder and one renderer, so the texture cache, the program caches and the pipelines
are warm, as they are in the game's steady state. An iteration replays the capture's writes into the builder, takes
the commands, executes them one command at a time and gives them back to the builder (`recycle`). The first
iteration creates the pipelines and uploads the textures, so it is left out when `N` is 2 or more. Turn the
validation layer off with `--no-validation` when timing, because it costs CPU for every Vulkan call. The per-command
lines are not printed; the summary line, the `rsx unimplemented` section and the validation line follow the three
lines below and describe the last iteration. A later iteration draws over the targets of the one before, so the image
is not the capture's frame, which is why `--out` and `--compare` are refused. Three lines are printed, here with
illustrative figures:

```
timing: frame_9400_room, 30 iterations (first excluded), median ms per iteration: build 6.10 (buildDraw 4.83), recordDraw 5.33, recordClear 0.10, submit 0.41, gpu wait 0.02, total 11.94
builder: per iteration 627 draws, fp cache 620 hits 7 misses, vp cache 600 hits 27 misses, vertex bytes 1234567 copied (union 1234567)
render: per iteration 115 barriers (3 skipped), 31 rendering instances, 552 state calls (11988 skipped), 12 vp constant uploads reused; upload memory 0x7 device-local host-visible host-coherent
```

The `timing` line gives the median over the counted iterations of the time each phase takes:

- `build`: replaying the writes through the register file and the frame builder, and taking the commands. It
  includes work the game does not pay on its RSX thread (applying the register writes and replaying memory
  events).
- `buildDraw`: the part of `build` spent in the builder's per-draw step (copying and converting the draw's guest
  bytes, textures and programs). This is what the game pays as `build` in its `rsx busy` line, so compare it, not
  `build`, when judging the builder.
- `recordDraw`, `recordClear`: the renderer's `execute` for draw commands and for clear commands.
- `submit`: the renderer's `execute` for flip commands (the present blit and the queue submit).
- `gpu wait`: time spent in the wait for the frame slot's fence, which is the GPU being behind the CPU. It is taken
  out of the three phases above.
- `total`: `build`, `recordDraw`, `recordClear` and `submit` added up (`gpu wait` is not in it).

The `builder` line gives, per iteration, the draws, the hits and misses of the fragment and vertex program caches
and the vertex bytes copied. The union figure is the size of the union of the attribute ranges per location, counted
separately from the merge; it equals the bytes copied unless the merge is wrong. The `render` line gives the
barriers recorded and the transitions that needed none, the rendering instances begun, the state calls recorded and
skipped, the vertex constant uploads reused (see [Rendering](#rendering)), and the property flags of the memory
that uploads go to (`device-local`, `host-visible`, `host-coherent`, `host-cached`). Without `host-cached` that
memory is write-combined, where reads are slow and single-byte stores are slower than whole-element stores.
Run-to-run spread on one machine was a few percent for `buildDraw` and `total` and up to 7 % for `recordDraw`, so
a difference smaller than that is noise; compare interleaved runs of the two binaries.

The CTest entry `rsx_replay` runs `tox2_rsxreplay` on `generated/rsx/captures/title.tox2cap` with
the options pinned in `cmake/rsx_replay.cmake`. The capture name `title` comes from the earlier game
and has no meaning for this one. The file is game-derived and git-ignored, so the test is registered
only when the capture exists; configuration prints the reason otherwise. The pin file also holds the
slot for the SHA-256 of a reference image (`generated/rsx/reference/title.png`) and the thresholds
(largest difference 8, at most 0.5 percent of the pixels differing). The SHA-256 is empty, because
the earlier game's reference image does not apply here, so the test only replays the capture and
passes when the summary line is printed and the validation layer reports no error. With a SHA-256
set, the reference image must exist and match it, and the thresholds apply. To make a capture, run
`tox2_game` with `--rsx-capture` on the frame you want and copy the file to
`generated/rsx/captures/title.tox2cap`. Captures of this game have been recorded (see the default
`frame_<N>.tox2cap` names above), so the test stays unregistered until one is copied to that name.

Three further renderer tests are registered: `render_core` (renders synthetic command lists, and
skips itself when there is no Vulkan device), and `vp_translator` and `fp_translator` (need no
GPU; they also translate the programs dumped under `generated/rsx` and skip those when absent). A dump
with a known expected body is compared with it. Any other dump is translated, and when the translation
succeeds its GLSL must compile; a translation the translator reports as unimplemented is not a failure,
and each test prints a summary line (`<n> dumps, <n> translated, <n> reported`) followed by the report
texts and their counts, most frequent first.
