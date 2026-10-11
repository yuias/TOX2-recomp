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
  raises one vertical-blank event every 1/60 s. A flip from the command buffer completes where
  the command executes, as in RPCS3, and does not wait for a vertical blank. Where the game's
  frame rate comes from is described under [Rendering](#rendering).
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
  texture texels, shader ucode), so the renderer never touches guest memory. Draw boundaries,
  addressing and register meanings follow RPCS3's `Emu/RSX` sources. At a flip the registers are
  reset the way RPCS3 resets them, so the next frame starts from the same state.
- **Renderer** (`vk_renderer.cpp` and neighbours). Vulkan 1.3 with dynamic rendering, the loader
  through volk (`externals/volk`, loaded at run time, so no import library is linked) and memory
  through VulkanMemoryAllocator (`externals/VulkanMemoryAllocator`). Colour and depth surfaces are
  kept in a render target cache keyed by memory location and offset. A texture whose address is
  a known render target is sampled from that target (render target aliasing), and a draw that
  samples the target it renders to reads a copy of it. Other textures are uploaded from the
  guest bytes into B8G8R8A8 images (A8R8G8B8, linear or swizzled) or BC1/BC2/BC3 images
  (DXT1/DXT23/DXT45), with the whole mip chain (see [Textures](#textures)); a unit without a
  usable texture reads a 1x1 magenta image.
  Cull mode, depth state, viewport, scissor, topology and blend constants are dynamic state;
  pipelines are cached by what is baked into them.
- **Readback** is gated. Copying the display buffer back to the host costs 8 MiB per flip at 1080p,
  so a live run does it only for flips that are dumped (`--dump-frames`). `tox2_rsxreplay` always
  reads the last flip back.
- **Shader translation** (`vp_translator.cpp`, `fp_translator.cpp`). Vertex and fragment programs
  are translated from RSX microcode to GLSL 4.50 and compiled with shaderc (the SDK's static
  library) to SPIR-V 1.6 for Vulkan 1.3, without optimisation. The result is cached on disk in
  `generated/shader_cache` (`--shader-cache DIR` changes it) as
  `<stage>_<program hash>_<GLSL hash>_<translator version>.spv`. A missing or corrupt file is
  recompiled and rewritten, and the translator version in the name keeps SPIR-V from an older
  translation from being reused. A program with an opcode that is not translated skips its draw
  and is counted. The vertex translator is described in [Vertex programs](#vertex-programs). The fragment translator handles MOV, MUL, ADD, MAD, DP3, DP4, MIN, MAX, the six
  compares (SLT to SEQ), FRC, FLR, RCP, EX2, LG2, DIV, DIVSQ, TEX, TXP and KIL, and ignores NOP and
  the FENCT/FENCB fences. RCP, EX2 and LG2 read the x lane of source 0 and replicate the result;
  DIV and DIVSQ divide the whole of source 0 by the x lane of source 1 (DIVSQ leaves a zero
  numerator at zero and ignores the sign of the divisor). KIL discards the fragment when any lane
  selected by its condition passes. Other opcodes (for example RSQ, DP2, LIT, the pack and
  unpack group and the flow-control opcodes), the WPOS, FOGC and SSA inputs, and the texture
  expand bit are not translated.
- **Flips and vsync.** A flip is done when the command executes (see [Fake RSX](#fake-rsx)). With the window
  the displayed buffer is also blitted, letterboxed to 16:9, to a swapchain with the FIFO present
  mode, so the display's refresh is what paces the RSX thread; presenting runs outside the guest
  lock. Without a window nothing waits for a vertical blank in the renderer, and the game's own
  waits on the 60 Hz vblank handler set its frame rate.
- **`rsx unimplemented` census.** The decoders, the frame builder and the renderer count what they
  meet but cannot handle yet (surface, depth and texture formats, texture layouts, vertex types,
  primitives, vertex and fragment opcodes, blend modes, other state and unknown methods) under
  `<category> <key>`. The stop census prints them as a `census: rsx unimplemented N kinds`
  section, most frequent first (omitted when empty). It is the work list for later scenes: a draw
  that depends on one of them is skipped or drawn incompletely, and the `skipped` count in the
  `rsx` section says which.

### Vertex programs

`vp_translator.cpp` translates one vertex program to one GLSL `main()` that ends with the viewport
transform and the varying outputs.

- **Opcodes.** Vector: MOV, MUL, ADD, MAD, DP3, DPH, DP4, DST, MIN, MAX, SLT, SGE, FRC, FLR, SEQ, SFL,
  SGT, SLE, SNE, STR and SSG. Scalar: MOV, RCP, RCC, RSQ, EXP, LOG, LIT, LG2, EX2, SIN and COS, and the
  flow-control opcodes below. Not translated, and the draw is skipped and counted: ARL and relative
  constant addressing, TXL, the condition code registers (a condition other than "always", or a
  write that only updates a condition code), BRA (its target comes from the address register) and
  PSH and POP.
- **Flow control.** BRB and BRI branch, CAL, CLI and CLB call, and RET returns. The target is a 10-bit
  instruction index; the boolean index (0 to 31) and the branch sense of BRB and CLB share the bits
  of source 2, as in RPCS3's `RSXVertexProgram.h`. BRB and CLB read bit `n` of the branch bits and take
  the jump when the bit is set if the instruction's sense bit is set, and when it is clear otherwise.
  BRI, CAL, CLI and RET are translated when their condition is "always" (a condition of "never"
  skips them); any other condition tests a condition register and is reported as not translated.
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
- **Limits.** The census program dumps (`vp_*.bin`) still end at the first end bit, and the translator
  tests that read them use programs without flow control.

### Textures

The frame builder copies the bytes of every 2D A8R8G8B8 and DXT1/DXT23/DXT45 texture a draw has
enabled (`rsx_texture_layout.cpp`), and the renderer uploads them (`vk_textures.cpp`). The guest layout
follows RPCS3 (`Emu/RSX/Common/TextureUtils.cpp`, `RSXTexture.cpp`, `Utils/image_utils.hpp`):

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
- **Cube maps.** The layout code handles faces (a texture without the LN flag aligns each face to
  128 bytes), but the renderer has no cube sampler, so cube and 3D textures are still counted and
  skipped.

The copy in `RsxDraw::texture_data` is already converted: levels one after the other, each in
tightly packed rows and de-swizzled, so the renderer needs only the texture size and mip count to
find them. It uploads all levels in one `vkCmdCopyBufferToImage` into an image with the full chain
(A8R8G8B8 words are byte swapped first, DXT blocks are copied as stored). A texture of the render
target cache has one level.

The sampler follows RPCS3's `update_fragment_sampler`: the mip chain is sampled only when the min
filter is one of the four mip filters (NEAREST_NEAREST, LINEAR_NEAREST, NEAREST_LINEAR,
LINEAR_LINEAR) and the texture has more than one level. The level range is the min and max LOD of
SET_TEXTURE_CONTROL0 clamped to the last level, the bias is the signed 5.8 field of
SET_TEXTURE_FILTER (rounded to half a level for a NEAREST mip mode), and the mip mode is LINEAR for
NEAREST_LINEAR and LINEAR_LINEAR. Otherwise the sampler reads level 0 only. Anisotropic filtering
and texels around the image (`CELL_GCM_TEXTURE_BORDER_TEXTURE`) are not modelled.

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
the commands the frame builder produced (`clear`, `draw` with the format and mip count of each
texture that has bytes, `flip`), then a summary line
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
GPU; they also check programs dumped under `generated/rsx` and skip those when absent).
