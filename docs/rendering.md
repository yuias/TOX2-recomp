# RSX and rendering

Everything on this page is inherited from the project this one was derived from, and it was
written against that game. For *Tales of Xillia 2* the fake RSX has run only until the game's
start-up ends (see [Status](status.md)): in the three headless runs the game initialised GCM,
produced 939 or 563 FIFO headers and issued 5 or 3 flips, which the fake RSX executed; no renderer
ran, so no frame was built, drawn or displayed. The frame builder, the renderer, the shader translators and the capture and replay
tools have not been exercised with this game's commands. The statements below about what the
game does (frame rate, flips, vblank waits) describe the inherited behaviour and are unverified
for this game.

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
  raises one vertical-blank event every 1/60 s. A flip from the command buffer does not wait for a
  vertical blank. It is limited to 60 flips per second, like RPCS3's default "Auto" frame limit: the
  first flip is immediate, and each later flip completes no earlier than a deadline that moves on by
  exactly 1/60 s per flip. When a flip command arrives before its deadline, the FIFO is held at that
  command (`get` stays at its header) and nothing behind it runs. A flip that is late does not make up
  for it: the next deadline is then 1/60 s after that flip. The flip status, the flip handler, the last
  flip time and the flip label are updated only when the flip completes. The wait happens with the
  guest lock released, and the RSX thread's sleep ends at the earlier of the deadline and the next
  vblank, so vblanks are still delivered on time while a flip is held. `--frame-limit off` completes a
  flip as soon as its command executes. The vblank is due on a 1/60 s deadline that is checked
  between slices of the command buffer too, so a consumer that is behind (a long draw backlog) still
  delivers it on time. The game's frame rate is therefore bounded by this limit, whatever the
  display's refresh rate is; see also [Rendering](#rendering).
- `_gcm_intr_thread` is a guest service thread, created as in RPCS3. The RSX thread queues the
  interrupts and this thread runs the handlers the game registered with
  `cellGcmSetVBlankHandler`, `cellGcmSetFlipHandler` and `cellGcmSetUserHandler`, with the guest
  lock held. A vblank is not queued while an earlier one is still waiting; flips and user
  commands are never dropped.

The method census (the `rsx` section of the census) lists the number of FIFO headers, the number
of distinct methods and flips, how often each interrupt handler ran, and the count per method,
named when the method has a known name. With the renderer on, it also gives the number of draws
rendered and skipped.

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
- **Readback** is gated. Copying the display buffer back to the host costs 8 MiB per flip at 1080p,
  so a live run does it only for flips that are dumped (`--dump-frames`). `tox2_rsxreplay` always
  reads the last flip back.
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
- **Flips and vsync.** A flip is done when the command executes, once the 60 flips per second limit allows it
  (see [Fake RSX](#fake-rsx)); `--frame-limit off` removes the limit. With the window the displayed buffer is
  also blitted, letterboxed to 16:9, to a swapchain with the FIFO present mode, once per completed flip;
  presenting runs outside the guest lock. The limiter paces the flips before they reach the swapchain, so a
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
  `rsx` section says which. A warm 260 s run of the game lists three kinds, `state antialiasing 0x3`,
  `texture layout render target alias of another size` (both are the documented anti-aliasing approximation,
  see [Render targets](#render-targets)) and `state zpass report constant`, and skips no draw. Stencil, user
  clip planes, surface resizing and vertex program condition codes no longer appear.

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
`texture layout render target alias of another size`. The game's anti-aliased surfaces (`state antialiasing 0x3`)
are twice as wide in memory as their clip rectangle, so the texture that reads one is twice as wide as the
surface; the renderer draws them without anti-aliasing, so the same normalized coordinates land on the same
picture up to the filtering across the missing samples. The count marks that approximation, not a failure.

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
  `state stencil test without a stencil surface`. Every draw sets all five dynamic stencil states for both
  faces, also with the test off, so no draw inherits the state of the previous one.
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
- A surface first drawn at its clip size and then bound at twice the width (the anti-aliased layouts) gets an
  unscaled top-left copy, part of the anti-aliasing approximation described above.

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
tox2_rsxreplay CAPTURE [--out DIR] [--compare PNG [--max-diff N] [--max-mean X] [--max-differing P]]
                      [--no-validation] [--shader-cache DIR]
```

| Option | Meaning |
| --- | --- |
| `CAPTURE` | A `.tox2cap` file. |
| `--out DIR` | Write the displayed frame of the last flip as `DIR/<capture name>.png`. |
| `--compare PNG` | Compare that frame with a reference image. The sizes must match. |
| `--max-diff N` | With `--compare`: fail when the largest per-channel difference exceeds `N`. |
| `--max-mean X` | With `--compare`: fail when the mean per-channel difference exceeds `X`. |
| `--max-differing P` | With `--compare`: fail when more than `P` percent of the pixels differ (by more than 2 in any channel). |
| `--no-validation` | Do not enable the Vulkan validation layer, which is on by default. |
| `--shader-cache DIR` | SPIR-V cache directory. Without it nothing is cached. |

The thresholds are optional and unchecked unless given, and they need `--compare`. The tool prints
the commands the frame builder produced (`clear`, `draw` with its colour and depth target and, for each
texture, the render target it aliases or the format and mip count of its bytes, `flip`), then a summary line
`replay: N commands, N draws, N skipped` (with the diff figures when comparing), the
`rsx unimplemented` section (`census: rsx unimplemented 0 kinds` when it is empty), the path of
the PNG, and `validation: active|inactive|disabled, N errors`. The exit code is 1 when a
threshold is exceeded, a file cannot be read or written, there is no frame to display or the
validation layer reported an error, and 2 for a usage error. Without a usable Vulkan device it
still prints the command list and counts every draw as skipped.

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
`generated/rsx/captures/title.tox2cap`. No capture of this game exists yet: its flips have only run
without a renderer.

Three further renderer tests are registered: `render_core` (renders synthetic command lists, and
skips itself when there is no Vulkan device), and `vp_translator` and `fp_translator` (need no
GPU; they also translate the programs dumped under `generated/rsx` and skip those when absent). A dump
with a known expected body is compared with it. Any other dump is translated, and when the translation
succeeds its GLSL must compile; a translation the translator reports as unimplemented is not a failure,
and each test prints a summary line (`<n> dumps, <n> translated, <n> reported`) followed by the report
texts and their counts, most frequent first.
