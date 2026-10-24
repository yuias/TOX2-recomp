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
  guest bytes into B8G8R8A8 images (A8R8G8B8, linear or swizzled), R8 images (B8, one byte per texel,
  the planes of the game's decoded movies) or BC1/BC2/BC3 images (DXT1/DXT23/DXT45), as 2D, 3D or cube images with the whole mip chain (see [Textures](#textures));
  a unit without a usable texture reads a 1x1 magenta image of the type the fragment program samples
  (2D, 3D or cube).
  Cull mode, depth state, viewport, scissor, topology, blend constants and the whole stencil state (test
  enable, ops, compare mask, write mask and reference, for both faces; see [Stencil](#stencil)) are dynamic
  state; pipelines are cached by what is baked into them.
- **A draw binds the depth surface only when it uses it.** The game binds a depth surface to most post-processing
  and UI draws. A draw with no depth test, no depth write and no stencil test (a stencil write needs the test, see
  [Stencil](#stencil)) does not acquire the depth target and renders with a colour attachment alone, and its
  pipeline has no depth format. This avoids resizing or copying a depth target the draw never touches (a depth
  target at another layout size is otherwise made and filled from the old one). The colour target is always acquired, also for a
  skipped draw. The one-sample (`dlaa`) scene pass is the exception: its colour target remembers the latest depth it
  was drawn with, so its draws acquire the depth as before. A draw without a depth attachment cannot continue an
  instance that has one (next item), so a depth clear followed by such a draw costs one more instance.
- **Consecutive draws share a rendering instance.** A draw leaves its rendering instance open. The next draw
  with the same colour view, depth view, stencil use and render area records straight into it: no layout
  transition, no barrier and no `vkCmdBeginRendering`. Draws inside one instance are ordered by rasterization
  order, which replaces the barrier between them. Every command recorded outside a draw's instance (texture
  uploads, the copy a draw makes to sample the target it renders to, target creation, resizing and layout syncs,
  the flip and readback commands) takes the frame's command buffer from one function, `outsideCmd`, which ends the
  open instance first. A draw with other attachments ends it before its own transitions, and the frame's submit ends
  it before `vkEndCommandBuffer`. An open instance therefore also means that nothing touched its attachments since
  the last draw, so they are still in their attachment layouts. `RendererStats::rendering_instances` counts the
  instances.
- **Clears.** A clear is one rendering instance with the attachments it binds. When its rectangle covers the whole
  render area, each aspect the clear touches (the colour, the depth, the stencil of a Z24S8 target) is cleared by the
  attachment's load operation (`VK_ATTACHMENT_LOAD_OP_CLEAR`), so a tile-based GPU neither reads the old contents nor
  loads them into the tile; an aspect the clear leaves alone (the stencil of a depth-only clear) keeps `LOAD`. A
  smaller rectangle loads the attachments and clears the rectangle with `vkCmdClearAttachments`. A clear with a colour
  attachment leaves its instance open like a draw does, so the draws that follow on the same attachments continue it
  (one instance for the clear and its draws, and no store and reload of the target between them). A depth-only clear
  ends its instance, because every draw has a colour attachment. A clear through a colour write mask is a draw of a
  full-screen triangle (`masked-clear`) and ends its instance too.
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
  selected by its condition passes. The alpha test (the `alpha_func` and `alpha_ref` members of the draw
  uniforms) runs only while the specialization constant `kAlphaTest` (id `kFpAlphaTestConstantId`) is true.
  `PipelineKey::alpha_test` sets it, and it is false for a draw whose function is 7 (test disabled, or a
  float target); function 0 ("never") is a test. A pipeline without the test has no alpha-test discard, because
  Apple GPUs lose hidden surface removal for a shader that can discard, even when the draw never takes the
  discard. A program with KIL keeps that discard in both variants. The same SPIR-V module serves both
  pipeline variants, so a program that is drawn with and without the test costs one more pipeline only. Other opcodes (for example RSQ, DP2, LIT, the pack and
  unpack group and the flow-control opcodes), the WPOS, FOGC and SSA inputs, and the texture
  expand bit are not translated.
- **Flips and vsync.** A flip is done when the command executes, once the flip limit allows it: with the default
  `--frame-limit vsync` a flip in the game's VSYNC flip mode waits for the next vblank, otherwise the limit is
  60 flips per second (see [Fake RSX](#fake-rsx)); `--frame-limit off` removes the limit. With the window the displayed buffer is
  also blitted, letterboxed to 16:9, to a swapchain with the FIFO present mode, once per completed flip (MAILBOX
  while [frame generation](#frame-generation) is loaded, which then presents more frames than the game flips);
  presenting runs outside the guest lock. The window's client area is a size preset in physical pixels
  (`--window-size`, 720p by default; see [Window](runtime.md#window)); the swapchain follows it through
  `WM_SIZE` (on macOS through the layout notifications of the Cocoa window), and the blit scales the frame to it, so a frame of `N` times 1280x720 maps one to one onto
  the preset of the same size (`--resolution-scale 2` and 1440p, 3 and 2160p). The limit paces the flips before they reach the swapchain, so a
  display that refreshes faster than 60 Hz does not speed the game up; with `--frame-limit off` the display's
  refresh paces the RSX thread instead. Frame generation changes none of this: the flips, the vblank timer and the
  flip limit stay as the game and `--frame-limit` set them, and only the number of frames the window shows grows.
  Without a window nothing waits for a vertical blank in the renderer.
  The [debug overlay](runtime.md#debug-overlay) is drawn on the swapchain image after that blit, in a
  separate pass before the image is presented. It is not drawn into the render target, so it never
  appears in `--dump-frames` files, requested PNG dumps, the display readback or captures, and it only
  redraws when a flip completes, apart from the idle presents described under [Overlays](#overlays).
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

### Vulkan on macOS

On macOS the renderer runs on a Vulkan driver that sits on top of Metal. The window is a Cocoa window
whose content view is backed by a `CAMetalLayer`; the surface is made with `VK_EXT_metal_surface`, and
the swapchain, the letterboxing and the overlay work as described above. The window belongs to the
process main thread, which runs the event loop while the game runs on another thread (see
[Building](building.md#running-and-debugging-on-a-mac)). The swapchain size is the window's size in
pixels, the content size in points times the backing scale of the screen. Without a window-server
session (an SSH login, for example) no window can be created: the RSX thread logs the reason and the
game goes on without rendering, as for any other renderer failure.

- **Drivers.** The LunarG Vulkan SDK installs two: MoltenVK and KosmicKrisp. macOS can list both, in no
  fixed order, so `tox2_game` does not depend on the order. By default (`--vk-driver auto`) it takes,
  among the usable devices, a discrete GPU first, then the device whose driver is MoltenVK, then the
  first usable one. MoltenVK is the default because KosmicKrisp is slower on the same hardware: on an
  Apple M4 Pro it costs about 6 to 8 microseconds more GPU time per draw and about 30% more per pixel.
  Replaying `generated/rsx/captures/frame_4200.tox2cap` with `--resolution-scale 4 --aa-mode msaa` takes
  about 36 to 40 ms of GPU time per frame on KosmicKrisp and 18 to 21 ms on MoltenVK. The log line
  `tox2: render: vulkan device ...` names the driver (`driver <name>`). `vulkaninfo --summary` shows
  which drivers are installed.
- **Choosing a driver yourself.** `--vk-driver auto|moltenvk|kosmickrisp` on `tox2_game` and
  `tox2_rsxreplay`, or the `vk_driver` key of `config.json` with the same values. `moltenvk` and
  `kosmickrisp` accept only a device with that driver. When there is none, no renderer is created: the log
  names the driver that was asked for and the devices found (`no Vulkan device with driver MoltenVK
  (found: Apple M4 Pro (KosmicKrisp))`), and the game goes on without rendering. The device is created
  once, so the debug overlay has no setting for it. The Vulkan loader's variables still work, and the loader
  applies them before this choice: a driver they remove is not found by `--vk-driver`.
  `VK_LOADER_DRIVERS_SELECT` takes a comma-separated list of patterns for the driver manifest file
  names and uses only the matching drivers; the manifests are in `/usr/local/share/vulkan/icd.d`
  after the SDK's system-global install (`libkosmickrisp_icd.json`, `MoltenVK_icd.json`).
  `VK_DRIVER_FILES` replaces the list of manifests with the files you name. `VK_ADD_DRIVER_FILES`
  adds files to the list and so does not restrict it.

  ```sh
  build/macos-release/bin/tox2_game --game-dir <game-dir> --vk-driver kosmickrisp
  VK_LOADER_DRIVERS_SELECT='*MoltenVK*' build/macos-release/bin/tox2_game --game-dir <game-dir>
  VK_DRIVER_FILES=/usr/local/share/vulkan/icd.d/libkosmickrisp_icd.json build/macos-release/bin/tox2_game --game-dir <game-dir>
  ```

- **Portability.** The loader hides portability drivers unless the application asks, so the instance
  is created with `VK_KHR_portability_enumeration` and its enumeration flag when the loader offers
  it. A device that advertises `VK_KHR_portability_subset` (MoltenVK) is created with that extension
  and with every subset feature it reports as supported.
- **Depth format.** Apple GPUs have no `D24_UNORM_S8_UINT`; the renderer uses `D32_SFLOAT_S8_UINT`,
  its existing fallback. The log line `tox2: render: vulkan device ...` shows `d24s8 no`.
- **Triangle fans.** MoltenVK reports triangle fans as supported but draws only the first triangle of a
  fan when the primitive topology is set dynamically, which is how the renderer sets it. The renderer
  therefore draws a triangle fan as a triangle list wherever the device cannot do fans: on MoltenVK,
  and on any portability-subset device that reports no `triangleFans`. A fan of N vertices becomes
  N-2 triangles `first, first+i, first+i+1`; an array draw becomes an indexed draw with 32-bit indices,
  and an indexed fan is expanded by its indices, split at the restart index when primitive restart is
  on. The list topology takes no restart. KosmicKrisp draws fans directly. The same log line shows
  `triangle fans yes` or `no`.
- **Resolution scale and depth.** The renderer rescales a depth target with a blit when the device offers
  one for the depth format (a format feature query on the chosen device). KosmicKrisp has no blit for
  `D32_SFLOAT_S8_UINT`; MoltenVK has one. Changing the internal resolution scale while the game runs
  therefore cannot rescale depth targets on KosmicKrisp: their contents restart for that frame, and the
  census counts `state rescale of a depth target unavailable` (see [Resolution scale](#resolution-scale)).
  On MoltenVK they are rescaled. The render tests skip the depth rescale checks where depth cannot be
  blitted.
- **Loader.** The loader is found at run time. If the normal search fails, the paths in
  [Building](building.md#vulkan-shaderc-and-ffmpeg) are tried.
- **Windows-only features.** DLAA and frame generation need NVIDIA Streamline, which is built on
  Windows only; the macOS build always reports it as off (see [DLAA](#dlaa) and
  [Frame generation](#frame-generation)). The RSX write watch is off too (see
  [Textures](#textures)).

### Overlays

With a window the renderer's one `PresentOverlay` slot holds an `OverlayStack`
(`runtime/src/overlay/overlay_stack.h`): the save data dialog overlay and, drawn after it so that it stays on top,
the [debug overlay](runtime.md#debug-overlay). The stack gives every child every flip (`onFlip`), merges what they
request (a field set by a later child wins), draws the children that want to draw (`wantsDraw`), and wants an idle
present when any child does. Each child has its own ImGui context; all use of any context happens under the shared
lock `imguiMutex()` (`runtime/src/overlay/imgui_context.h`), so the two never share windows, focus or input.
While the dialog is shown `wantsDraw()` is true, so [frame generation](#frame-generation) reports `overlay open` and
passes the frames through, as it does for the debug overlay.

**Save data dialog overlay** (`runtime/src/overlay/save_dialog_overlay.*`). It draws the latest snapshot of the
`SaveDialogBoard` that the waiting guest thread publishes (see [Save data dialog](runtime.md#save-data-dialog)), with
the foreground draw list and no ImGui windows, on the swapchain image after the letterboxed blit like the debug overlay.
It takes no window input; the pad drives the dialog.

- *Layout.* A canvas of 1280x720 is mapped onto the 16:9 image area of the target (`scale` is that area's height over 720),
  and the dim covers the whole target. A list has the heading (`セーブ` or `ロード`) at the top left, a rule, a box of
  four rows of 130 px, a rule and a legend with the enter and back symbols. A row has the icon in a 160x88 cell, the
  title, the date and size, and up to three lines of detail text. The selected row has a frame with a glow that pulses
  about once per 1.2 s, and a scrollbar shows when there are more than four rows. The symbols are drawn as shapes, a
  circle and a cross, in the order that `dialogButtons()` gives. A confirmation or an error message is centred text
  between two rules with the buttons below, on a darker dim. Every alpha is multiplied by the snapshot's opacity,
  which the model fades over 150 ms when the dialog opens and closes.
- *Fonts.* The text is Japanese. At run time the first usable font of this list is used: the `--ui-font` file,
  `SCE-PS3-SR-R-JPN.TTF` next to `libsre.reloc.elf`, the same file next to the executable's ELF, then
  `fonts/BIZUDPGothic-Regular.ttf` next to `tox2_game` (see [Fallback UI font](building.md#fallback-ui-font)). A
  candidate counts only if it can be read, stb_truetype parses it and it has a glyph for `セ`; the check is made before
  the font reaches ImGui, whose font loader asserts on a font it cannot parse. The log names the font used. Without a
  usable font the dialog uses ImGui's built-in font and Japanese text shows as `?`.
- *Atlas.* ImGui 1.91.9b has no dynamic glyph loading, so the atlas bakes a fixed set of code points at three sizes
  (32, 22 and 17 px times `scale`). The set holds printable ASCII, every string of the dialog text and every string of the
  dialog being shown (titles and details come from the game's save files). It is rebuilt, and the font texture
  uploaded again, when a dialog needs a glyph that is not baked or the image area changes height; a rebuild waits for the
  queue, so a dialog with new glyphs costs one hitch when it opens. Baked sets are only added to, up to 8000 code
  points.
- *Icons.* `ICON0.PNG` of the rows near the visible ones is decoded with libpng (`decodePngRgba8`, at most four per
  frame) and uploaded as a texture, at most 32 at once. A row whose icon cannot be decoded keeps the placeholder. A texture
  that leaves the window is released after the frames in flight have finished.
- *Failure.* A set-up error (the Vulkan backend, the descriptor pool, the font atlas) is logged once, switches the
  overlay off for the rest of the run and tells the board that nothing can draw, so a dialog that is open or opens
  later is answered as cancelled instead of waiting. Destroying the overlay does the same.

**Idle present.** The game normally keeps flipping while a dialog is open, but a stall (a load, a hang) would freeze
the dialog on its last frame. When the RSX consumer loop finds a pass without FIFO work it calls
`RsxExecutor::idle()`, which `tox2_game` forwards to `RsxRenderer::presentIdle()`; the call is not made while a flip is
held back by the flip limit or a vblank, because that flip is presented shortly. `presentIdle` re-presents the last
displayed image, black when the last flip was black, with the overlays, and only when:

- a presenter and an overlay are set, the overlay `wantsIdlePresent()` (the dialog is shown), at least one flip has
  been presented and the renderer is valid;
- no real flip happened for 100 ms (`kIdleStallThreshold`, above the 33 ms interval of a 30 fps game, so idle presents
  never interleave with the game's own flips) and the last idle present is at least 33 ms ago
  (`kIdlePresentInterval`, about 30 per second), tested by `RsxRenderer::idlePresentDue`;
- no frame is open, that is no draw or clear has been recorded since the last flip (the game is then mid-frame and its
  next flip presents); this logs once, `tox2: render: idle present skipped, a frame is open; an overlay shows only when
  the game flips`;
- frame generation is not loaded; this logs once, `tox2: render: idle present skipped, frame generation is loaded; an
  overlay shows only when the game flips`.

The last condition is a known limit. An idle frame would reuse the frame index of the next real frame, and Streamline's
frame token and latency markers expect each index once, so with frame generation loaded (a windowed run whose
`--frame-gen` target is not `off`; the plugins are unloaded while it is `off`) the dialog is drawn only on real flips. The game keeps
flipping while a save dialog is open, so this is not seen in normal use; a stall with frame generation loaded would
freeze the dialog. An idle present is not a flip: it does not call `onFlip`, `flipEntered` or the frame generation
present, does not count in the flip statistics, makes no readback and no dump, and leaves `readDisplay` reading the
last real flip.

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
  neither `kFpFlagRound8Bit` nor the alpha function, and its pipeline is the variant without the alpha test.
  The fragment GLSL does not change.
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

The renderer has three ways to draw such a surface, chosen by `--aa-mode` (see
[Capture and replay](#capture-and-replay) and the overlay's Settings, [Debug overlay](runtime.md#debug-overlay)).
All apply to mode 3, the only one the game uses; the 2x2 modes (4 and 5) are always drawn as supersampled
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
  - a colour target is copied by a draw that fetches each layout texel from the sidecar; a depth target by one
    pass that writes the depth through `gl_FragDepth` and, for Z24S8, the stencil through `gl_FragStencilRefARB`
    (the device is created with `VK_EXT_shader_stencil_export` when it offers it; the stencil test is on with
    op REPLACE, so the exported value is stored). A device without that extension takes a depth pass plus eight
    passes that rebuild the stencil one bit at a time with a stencil-test write mask;
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
- **`dlaa`.** Colour and depth are drawn into one-sample images of the clip size times the resolution scale, with a
  sub-pixel jitter, and NVIDIA DLSS in its DLAA mode anti-aliases the colour before the game's resolve reads it. It
  needs an NVIDIA RTX GPU and the Streamline DLLs and falls back to `msaa` without them; see [DLAA](#dlaa).

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

##### DLAA

`--aa-mode dlaa` draws the game's anti-aliased scene (the mode-3 colour and depth pair) with one sample per pixel and a
sub-pixel jitter, and lets NVIDIA DLSS in its DLAA mode (anti-aliasing at the input resolution, no upscaling) produce
the anti-aliased picture that the game's resolve pass then reads. It runs at the scaled scene resolution: the clip size
times `--resolution-scale`. DLSS is reached through the NVIDIA Streamline SDK; building with the SDK is described in
[NVIDIA Streamline (DLAA)](building.md#nvidia-streamline-dlaa). This mode is Windows only: the macOS build has no
Streamline, so `dlaa` falls back to `msaa` there.

- **Availability.** `sl.interposer.dll` is loaded at run time from the directory of the executable, after its NVIDIA
  signature has been checked; it is never linked. Over-the-air updates and downloaded plugins are switched off, so
  only the DLLs next to the executable are used (`sl.interposer`, `sl.common`, `sl.dlss` and
  `nvngx_dlss`). The Vulkan device is created with the instance and device
  extensions, features and queues that DLSS asks for; the rest of the creation is unchanged. DLAA is available when
  all of this works, Streamline reports DLSS supported on the GPU and driver, and the device has the image formats the
  pass uses (blit support for the formats of the scene image's layout copy, storage, linear sampling and blit for the
  R8G8B8A8 output, and sampling and transfer for the R16G16 motion vector image). Otherwise it is unavailable for one reason, which is logged, and a requested `dlaa` falls back to
  `msaa` (or `supersample` on a device without 2-sample support): the build has no Streamline SDK, the DLLs are
  missing or not signed by NVIDIA, the GPU is not an NVIDIA RTX GPU or the driver is too old, or a format is
  missing. The executables start without the DLLs. `tox2_game` loads Streamline on every Vulkan start, because
  availability is fixed when the context is created and the overlay may switch to DLAA later; the overlay lists
  DLAA only when it is available. `tox2_rsxreplay` loads it only with `--aa-mode dlaa`. In a windowed run the
  swapchain, present, surface-creation and wait-idle calls go through Streamline's proxies, which its manual-hooking
  mode requires. The present call is where Streamline runs its resource garbage collection; a run without a window
  (`--no-window`, the replay, the tests) never presents, so that step does not run there. The effect was measured by
  replaying a two-capture cutscene pair for 2000 DLAA evaluations: private memory went from 990 to 991 MB and
  dedicated VRAM stayed at 487 MB (the `msaa` control went from 582 to 587 MB, with 284 MB of VRAM). The resource
  tags are valid only until the evaluation, so nothing waits for the collection. After 60 evaluations Streamline
  logs that `presentCommon()` was not observed; the runtime logs that notice at debug level in windowless runs, and it
  stays an error in windowed runs, where it is the only sign that presents bypass Streamline's proxy.
- **The scene image.** A mode-3 pass in `dlaa` mode draws into a one-sample sidecar of the clip size times the
  resolution scale (the same sidecar machinery as `msaa`, with 1 sample). The target keeps its 2W x H layout image,
  because the edge effect's depth reads, the rebinding of the depth surface as a 1280x720 plain surface and the
  cache identity depend on it, and that image is written only when something reads it: by one NEAREST blit from
  the sidecar in which texel `x` of the layout takes pixel `x >> 1`, so both columns of a pixel hold the same value.
  A depth surface read as A8R8G8B8 is brought up to date by the same blit first and then converted from the layout
  image like a plain surface. Switching the mode while the game runs retires the sidecars of the other sample count
  into the frame slot's keep-alive list, which is released when the slot's fence has signalled, and the next pass
  creates new ones.
- **Where DLAA runs.** DLAA is evaluated once per scene write, at the first draw that reads the scene as a texture
  from outside the scene pass. That draw is found by its properties, never by an address: it samples a one-sample
  A8R8G8B8 anti-aliased colour target as a texture of exactly the target's layout size (2W x H), its own pass is not
  an anti-aliased pass (it writes the plain display buffer, which excludes a feedback read and any read inside the
  scene), and the target holds content that DLAA has not processed yet. In all the captures this is the game's
  resolve pass, and no other draw reads the anti-aliased colour. The inputs are the colour sidecar, the depth sidecar
  of the depth target that the latest colour-writing draw or clear of the scene had bound (the colour target keeps it
  alive after the resolve has rebound the depth address as a non-anti-aliased surface), an all-zero motion vector
  image, the jitter and the camera matrices described below. The scene pass of a dialog or menu frame is a clear
  and screen-space draws without a perspective draw; its depth of 1.0 is a valid input, so those frames are
  evaluated too. The output is an R8G8B8A8 image of the input size. Later reads of the same scene reuse it. If the
  evaluation cannot run (no matching depth image, or a Streamline error), one warning `tox2: render: DLAA evaluation
  failed (<reason>); scene read unfiltered` is logged, the census counts `state DLAA evaluation failed`, the failure
  counter of the renderer is incremented, and the resolve reads the layout image written from the one-sample scene,
  without anti-aliasing. The first evaluation logs `tox2: render: DLAA evaluating <w>x<h>`.
- **The output is widened to 2W.** The resolve samples the scene with a texture coordinate offset of -0.5 / 2W (the
  centre of layout texel 2X), so a W-wide output sampled directly would mix a quarter of the pixel to the left into
  every result. The output is therefore expanded by a NEAREST blit into a 2W x H image, which has the layout's two
  equal columns per pixel, and the resolve samples that image.
- **Alpha is kept.** The resolve copies the scene's alpha into the display buffer, and a later translucent draw uses
  that alpha as a blend weight. Alpha upscaling is therefore on, so the alpha channel goes through DLSS. The scene's
  alpha is a constant 1.0 in all captures, and the display alpha after the resolve stays 1.0. DLSS runs with SDR
  colour, automatic exposure and its default preset.
- **State after the evaluation.** Streamline's hooks for command-buffer state (pipeline, descriptor and
  begin-command-buffer) are not active in version 2.14.1: it logs at debug level that they are not supported, and it
  neither tracks nor restores what is bound. Streamline binds its own pipelines and descriptors during the
  evaluation, so the renderer invalidates its shadow of the draw state after each one, and the next draw records
  pipeline, descriptors, push constants and dynamic state again. The depth image is tagged as a shader-read image,
  because Streamline samples it in a compute pass.
- **Jitter.** Every translated vertex program ends with `pos.xy += jitter_ndc * pos.w`, where `jitter_ndc` is a
  vertex push constant of two floats that the renderer keeps current for every draw. It is 0.0 for every draw outside
  the gate below, so all other draws and the other anti-aliasing modes render bit for bit as before. The gate: the
  draw is in a `dlaa` scene pass and its transform is a perspective one, that is one of `c0.w`, `c1.w` and `c2.w`
  (floats 3, 7 and 11 of the vertex constants `c0` to `c3`) is not zero. An orthographic matrix has all three at 0,
  and 2D and screen-space draws must not move. All gated draws of a frame share one offset, which keeps passes that
  depth-test against each other exact. The offset is Halton(2, 3) over 8 phases (the DLSS guide asks for at least 8
  at the ratio 1 of DLAA): the values at index `(flip % 8) + 1` minus 0.5 in each axis, in input pixels, x to the
  right and y down. A frame without a gated draw moves nothing and reports no offset.
- **The reported offset is not flipped.** Streamline is told the offset the geometry was moved by. The viewport has
  no y flip, so a positive offset moves the content right and down, which is the direction DLSS expects in its
  motion-vector space (origin at the top left). All four sign combinations were measured against 4x renders (see
  below) and the unflipped offset scored best.
- **Camera motion.** There is no per-object motion vector image: it is all zero and `cameraMotionIncluded` is false,
  so DLSS reprojects the history from the clip-to-previous-clip matrix alone. Every gated draw reports its transform
  (`c0` to `c3`, the viewport scale and offset, the clip size and the depth range) to a per-frame table of at most
  16 distinct cameras. The camera of most draws is the frame's camera (the first seen on a tie), and only the draws
  before the resolve count, because those are what the evaluation sees. The renderer builds from it the matrix that
  maps a guest vertex to the clip space whose depth is stored in the buffer (the transform, the translator's
  viewport mapping and the depth range remap), and passes Streamline the clip matrices and the clip-to-previous-clip
  matrix and its inverse. The guest rows are used as they are: Streamline's matrices are row-major with row
  vectors, like the guest's.
- **The y mirror.** Streamline's motion-vector shader builds a y-up clip position, while the renderer's clip space is
  y-down (the viewport height is positive and the vertex epilogue does not flip). The two clip-to-clip matrices are
  therefore mirrored in y (`F * C * F` with `F = diag(1, -1, 1, 1)`). The jitter is in pixels and needs no mirror.
  `motionVectorsInvalidValue` is set to 0; the value does not change the picture, and leaving it unset makes
  Streamline warn once per process.
- **When the history is dropped.** The evaluation is told to reset the history on the first evaluation, when the
  input size changed (a new resolution scale or target size), when the previous evaluation was not in the
  immediately preceding flip, when the mode was switched to `dlaa` since the last evaluation, when this frame or the
  previous one has no camera (a dialog or menu frame has no perspective draw) or a matrix cannot be inverted, when a
  camera appears or disappears between consecutive frames, and on a camera cut. A cut is detected by mapping nine
  probe points (x and y in {-0.5, 0, 0.5}, z 0.5, w 1) through the clip-to-previous-clip matrix: a probe with
  w <= 0, or one that lands more than 0.5 NDC from where it started, is a cut. The probe depth of 0.5 is about
  twice the near plane, so a dolly back of 20 units puts the probe behind the previous camera. The rule therefore
  fires for the effects pair 9100 then 9101 although the content is continuous, and not for 9101 then 9100. Keeping
  the history over that step ghosts the screen-space effects, so the rule is kept as it is.
- **Known limits.** One scene target per frame: there is one DLSS viewport and one output per renderer, and a second
  anti-aliased scene target would fail the evaluation (the game has one). Only camera motion is described, so moving
  objects, skinned meshes, scrolling textures and screen-space effects that move on their own can ghost. The camera
  table keeps 16 distinct cameras per frame, and a dominant one first seen after 16 others would be missed (no
  capture has so many). In `dlaa` mode the edge effect, which reads the anti-aliased depth as a 2560-wide texture and
  compares the two samples of a pixel, sees equal samples, so it loses the half-intensity steps that `msaa` gives:
  at most 25 to 31 of 255 on 0.3 to 0.7 % of the pixels, about the spread between `msaa` and `supersample`. The
  anti-aliasing is DLSS's, not the hardware's sample pattern, and because it uses history it also differs from
  frame to frame.
- **Measured.** The checks compare a 1x frame with the 4x `msaa` render of the same capture, box-downsampled
  (`tox2_rsxreplay <capture> --resolution-scale 4 --aa-mode msaa --compare <1x png>`); a lower mean difference is
  closer to the 4x render. For the jitter sign, the room, effects and cutscene captures were replayed 16 times and the
  mean difference was: room 1.016 without jitter, 0.989 unflipped, 1.171 with x flipped, 1.109 with y flipped and
  1.294 with both; effects 0.293, 0.290, 0.346, 0.311 and 0.362; cutscene 0.961, 0.875, 0.950, 0.950 and 1.039. Over
  the edge pixels alone the unflipped offset was 5.8 (room), 4.3 (effects) and 6.3 % (cutscene) below no jitter.
  Most of the full-frame mean is texture and mip detail that the jitter cannot change: the 1x `msaa` reference
  scores 1.040 on the room and the 1x `supersample` one 0.761, so on that capture DLAA is closer to the 4x render than
  `msaa` and farther than `supersample`. For the camera, the
  cutscene pair 8700 then 8701 was replayed 8 times and its last frame compared: 3.538 with the camera off, 1.072
  with a reset at every frame, 3.325 with the camera on, and 0.967 with the camera on and the y mirror, which is the
  default.
- **Not done.** Per-object motion vectors (matching the draws of two frames, the previous frame's vertex data,
  skinned meshes, scrolling textures) would remove the ghosting named under the known limits, and are future work.
  DLSS upscaling, a render scale per phase and HDR colour are not used.

#### Frame generation

With a window and `--aa-mode dlaa`, `--frame-gen off|60|90|120|180|unlimited` (or the overlay's `Frame generation`
combo) lets NVIDIA DLSS Frame Generation (DLSS-G, multi frame generation, through Streamline 2.14.1) insert generated
frames between the game's flips, so that the window shows `game rate x multiplier` frames per second. It changes
presentation only. The guest's simulation, its flip rate, the vblank timer and `--frame-limit` are as they were: a
260 s dungeon run counted 9232 flips and 15449 vblanks with generation off and 9228 and 15452 with target
`120` (generating at 2x and 4x), and no vblank was coalesced. Building with the SDK and the DLLs it needs are
described in [NVIDIA Streamline (DLAA)](building.md#nvidia-streamline-dlaa). Frame generation is Windows only; the macOS
build never has it.

- **Availability.** Everything DLAA needs, and in addition: `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll` and
  `nvngx_dlssg.dll` next to the executable, a GPU and driver for which DLSS-G reports itself supported (RTX 40 or 50
  series), Windows hardware-accelerated GPU scheduling, a device with `VK_NV_low_latency2`, and a window. The Vulkan
  device is created with the extensions, features and queues the three plugins ask for, including a queue family for
  native optical flow when the device has one. Where any of this is missing, frame generation is unavailable for one
  reason, and the run presents as it did before: no generation, the FIFO swapchain, no Reflex markers. The start-up
  log says which case applies, `tox2: render: frame generation available (DLSS-G up to 6x, optical flow native)` or
  `tox2: render: frame generation unavailable: <reason>` (`no window` for `--no-window`); a requested target that
  cannot run logs one warning, `tox2: render: frame generation unavailable (<reason>); presenting without it`.
  `tox2_rsxreplay` and the tests never use it. The overlay lists the combo only where it is available. In a windowed
  run the plugins are loaded at start-up so that the overlay can turn generation on later, and they are unloaded
  before the swapchain is created while the target is `off`. Switching between `off` and a target therefore
  recreates the swapchain, like a window resize: the RSX thread stalls once.
- **Only with DLAA.** The inputs below exist only in the `dlaa` scene pass. With another anti-aliasing mode, a
  target is accepted and generation stays inactive (`needs DLAA anti-aliasing`, one info line); the overlay draws the
  combo disabled with that note and keeps the saved target, which applies again when DLAA is selected.
- **Targets.** The multiplier is `floor(target / game fps)`, at most the maximum DLSS-G reports (5 generated frames,
  so 6x, on the measured RTX 5070 Ti), and generation is inactive when it is below 2. `unlimited` uses the maximum.
  The ratio is never rounded up and the game is never slowed down to make it whole: the target is an upper bound.

  | target | game 30 fps | game 60 fps |
  | --- | --- | --- |
  | `off` | none (30) | none (60) |
  | `60` | 2x (60) | none (60) |
  | `90` | 3x (90) | none (60): `target is less than twice the game rate` |
  | `120` | 4x (120) | 2x (120) |
  | `180` (unverified) | 6x (180) | 3x (180) |
  | `unlimited` | 6x (180) | 6x (360) |

  Measured with a 144 Hz display: target `120` gave 2x at 60 fps and 4x at 30 fps and presented 120.0 fps; `90` gave
  3x at 30 fps (90.0 fps) and nothing at 60 fps; `unlimited` gave 6x, 360.0 and 180.0 fps. `180` could not be
  checked: no display of the development machine runs at 180 Hz (the frames handed to the swapchain were counted, not
  seen).
- **Game rate.** The rate comes from the flip completion times the RSX thread records. An interval counts as a whole
  number of vblanks when it is within 4 ms of one, and a class (1 vblank is 60 fps, 2 vblanks is 30 fps) is adopted
  after 6 consecutive intervals of it. Only 60 and 30 fps generate. A game below 30 fps, or one with irregular
  intervals (for example `--frame-limit off`), is presented as it is. An interval over 100 ms (a load) drops the
  history. The title and menus run at 60 fps and the dungeon at 30 fps, so the multiplier changes when the game does.
- **Inputs.** DLSS-G is given, for every guest frame, the depth and motion vectors of the DLAA frame and the camera.
  The depth is the DLAA depth sidecar, copied once per frame slot. The motion vectors are camera motion only: a
  fragment pass computes, for every pixel, where its depth projects in the previous frame from the
  clip-to-previous-clip matrix of the DLAA camera (the unmirrored one), as an R16G16 image of the scene size. Both
  are valid until the present; the vector image is shared by all frames, which is safe because Streamline's default
  blocks the presenting queue until DLSS-G has read it. The black bars of the letterbox are described as a
  sub-rectangle of the backbuffer, so only the picture is interpolated. The history is reset on the first generated
  frame after a frame without generation, when the camera appears, disappears or cuts, and when the game rate class
  or the scene size changes. The convention was measured headless with `tox2_rsxreplay --fg-mv-check`, which warps the
  previous DLAA output with the vectors and compares it with the current one (mean absolute difference over RGB, 0 to
  255): the vectors cut the difference by 66 % on the cutscene pair 8700 to 8701 and by 87 to 88 % on four dungeon pairs
  (walking forward, and turning the camera), where the warp scored 1.6 to 2.7 against 12.5 to 20.6 without motion; of the four
  sign combinations, the unflipped one was lowest on every motion pair.
- **Limits of the inputs.** Moving objects, characters, particles and screen-space effects have no vectors of their own
  (DLSS-G's optical flow is all that covers them). No HUD-less colour and no UI colour or alpha is tagged: the game
  draws its HUD and text into the same buffer as the scene, so during camera motion they take the scene's motion and
  can ghost or warp. The debug overlay and the save data dialog would be interpolated the same way, so generation is
  suspended while either is shown (`overlay open`) and resumes when it is closed. Whether the generated frames look right was not judged by these
  measurements. `--fg-only-generated` presents only the generated frames, to look at them without the real ones.
- **Reflex.** DLSS-G needs Reflex: with it off, DLSS-G reports `Reflex not detected` at the first activation and
  stops, and the controller keeps generation off until the target changes (one warning names the status bits). The
  renderer therefore sends Reflex markers for every guest frame (simulation start and end, render submit, present)
  with the DLAA frame index, which is the flip number, and a sleeper thread calls the Reflex sleep once per frame.
  Nothing waits for that thread: a sleep in front of the simulation would change the guest's timing, and the RSX
  thread also delivers the vblank ticks, so it must never sleep. The sleeps take about 0.04 ms on average (0.78 ms at
  most, none dropped), and the thread is stopped before every plugin load or unload and before the swapchain is
  released. Reflex runs in low-latency mode without a frame limit; `--fg-reflex off` is a diagnostic that shows the
  failure above. `NvLowLatencyVk.dll` is not shipped (see [Third-party notices](../THIRD_PARTY_NOTICES.md)); Streamline
  uses `VK_NV_low_latency2` instead, and its message about the missing DLL is logged at debug level.
- **Present mode.** DLSS-G requires vsync off, and Streamline documents that Vulkan has no vsync with frame
  generation. While the plugins are loaded the swapchain is therefore MAILBOX, else IMMEDIATE, else FIFO with one
  warning (`tox2: render: frame generation: no present mode without vsync; using fifo (DLSS-G requires vsync off)`);
  with generation off it is FIFO as before. All three modes were measured for 260 s with targets `120` and
  `unlimited`. MAILBOX and IMMEDIATE present exactly game fps times the multiplier (120.0, and 360.0 or 180.0), with
  a presenter wait of at most 1.13 ms under MAILBOX and 3.25 ms under IMMEDIATE (outside the first activation). FIFO
  cannot show more frames than the refresh, so the multiplier is capped at `floor(refresh / game fps)`: with
  `unlimited` on a 144 Hz display it gave 2x at 60 fps and 4x at 30 fps, 120.0 fps, with the presenter waiting at
  most 1.31 ms. MAILBOX is the default because it measured like IMMEDIATE and does not tear; frames above the refresh
  rate are replaced in the mailbox and never shown, so `unlimited` at 60 fps on a 144 Hz display presents 360 fps and
  shows at most 144. `--fg-present-mode fifo|mailbox|immediate` selects the mode; FIFO is a diagnostic. Under MAILBOX
  DLSS-G logs `VSync interval 1 not supported with FG, clamping to 1` at every present, although its own state line
  shows sync interval 0 while it generates in every mode, so that line is logged at debug level.
- **Log.** On every change of the active state, the multiplier, the game rate class or the target, held for 1 s so
  that a short alternation is not logged, one info line states the expected rate:
  `tox2: render: frame generation active: <m>x (game <g> fps, expected <g*m> fps, display <r> Hz, target <t>)`, or
  `tox2: render: frame generation inactive: <reason> (presenting game <g> fps)`. The expected rate is the game rate
  times the multiplier, not a measurement; under MAILBOX it can exceed the display refresh. The reasons are
  `needs DLAA anti-aliasing`, `measuring the game rate`, `game rate irregular`, `game below 30 fps`, `target is less
  than twice the game rate`, `overlay open`, `swapchain recreated` (the two frames after any swapchain recreation),
  `window too small` (below DLSS-G's minimum size), `no 3D camera` (a dialog or menu frame), `inputs not tagged`,
  `scene size differs from the display` and `status <bits>` (DLSS-G reported a failure; it stays off until the target
  changes). At debug level every 10 s while active: `tox2: render: frame generation: <m>x, game <g> fps, presented <p>
  fps, sleep avg <a> max <b> ms`, where `<p>` is the number of frames DLSS-G reports presented per second. Once, at
  the first activation, `tox2: render: frame generation needs about <n> MB of video memory`. At stop, at info level,
  `tox2: render: frame generation summary: <n> frames presented, <k> of <N> game frames with generation (average
  <x>x), suspended frames: <reason n, ...>`. The overlay's status line shows the same figures, for example
  `4x: 30 -> 120 fps, measured 119.6` (see [Debug overlay](runtime.md#debug-overlay)).
- **Known limits.** The first activation stalls one present for about 40 ms (DLSS-G allocates its resources inside the
  present); an activation that follows a plugin reload (a switch from `off` to a target, as `--fg-cycle` does) took
  about 160 ms. DLSS-G
  disables itself while the window is not focused (it logs `DLSS-G disabled: window not focused`) and reports a status
  of ok, so the controller still logs `active` while the presented rate equals the game rate. At some scene changes,
  such as the load of a save that takes the game from 60 to 30 fps, DLSS-G was off for about 2 s. With
  `--vk-validation` the layer reports 8 or 9 errors per activation, all on Streamline's own objects
  (`nv.sl.dlss_g.*`: the layout of its output images on first use, its present semaphore and, once, a thread-safety
  report on a latency marker between two of its threads), none on the renderer's. While the plugins are loaded the
  backbuffer is left in `TRANSFER_SRC_OPTIMAL` instead of `PRESENT_SRC_KHR`, because DLSS-G expects that layout.
  Streamline's warning `slDLSSGGetState must be synchronized with the present thread` is logged once per process. The
  generated frames add about one game frame of latency by their nature; it was not measured. Display behaviour
  (variable refresh rate, the NVIDIA indicator text, the look of the generated frames) was not checked.
- **Not done.** Per-object motion vectors, HUD-less colour and UI colour or alpha tags (which would remove the HUD
  ghosting), tagging the overlay as UI so that it need not suspend generation, a frame rate readout in the window
  title, dynamic multi frame generation (D3D12 only) and frame generation in the `msaa` and `supersample` modes
  (which have no depth image of the scene and no camera tracker).
- **Diagnostics.** `--fg-present-mode`, `--fg-reflex off`, `--fg-only-generated` and `--fg-cycle SECONDS` (alternates
  between `off` and the target every `SECONDS` seconds, recreating the swapchain each time) need `--frame-gen` other
  than `off`; see [`tox2_game` options](runtime-options.md#rendering). `tox2_rsxreplay --fg-mv-check [--fg-mv-flip
  none|x|y|xy]` checks the motion vectors without a window (see [Capture and replay](#capture-and-replay)).

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

The frame builder converts the bytes of every 2D, 3D and cube A8R8G8B8, B8 and DXT1/DXT23/DXT45 texture a
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
- **B8.** One byte per texel in the same layouts as A8R8G8B8 (the Morton order and the pitch rule do not
  depend on the texel size). The image is `R8_UNORM`; its view has the base channels A = 1 and R = G = B =
  the byte (RPCS3 `get_component_mapping`: ONE, R, R, R), and the texture's remap applies on top as for
  any other format. A B8 texture at a render target address is not supported (`texture format B8`).
- **DXT.** Never swizzled. Without the LN flag the block rows of a level are contiguous (the pitch
  register is ignored); with it they use the pitch register like a linear texture.
- **Cube maps.** Six faces, each with its own mip chain, in the order +X, -X, +Y, -Y, +Z, -Z (the
  order of the Vulkan array layers). A texture without the LN flag aligns each face to 128 bytes. The
  faces must be square.
- **3D textures.** A level is `depth` slices of the 2D level one after the other (a linear level keeps
  the pitch of level 0 for every row of every slice), and the depth halves, to at least 1, from one
  level to the next like the sides. A swizzled A8R8G8B8 or B8 level interleaves one bit of x, y and z per
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
`CUBE_COMPATIBLE`, a region per face and level and a cube view). A8R8G8B8 words are byte swapped first;
B8 bytes and DXT blocks are copied as stored. The image cache key includes the depth and the kind, so a 3D and a 2D
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
  RSX has got, or the RSX writes memory itself. The consumer performs the `NV0039_BUFFER_NOTIFY` copy before
  the builder sees the write (main and local memory, plain byte format `0x101`; other formats are counted as
  unimplemented), and the builder then starts the epoch. A game has to synchronize with the RSX before it reuses
  texture memory the RSX may still read, so a change it makes behind the RSX's back shows up at the next of
  these points (this title does two acquires per flip); a guest write inside an epoch, with none of them
  in between, is not seen until the epoch ends. Inside an epoch an entry is used without reading guest
  memory. On the first use in a later epoch the guest range is hashed where it lies (`RsxMemoryView::hashRange`,
  XXH3-64 as in `rsxHashBytes`, streamed over the pieces of the range); an unchanged hash keeps the blob and
  its id without a copy. A changed hash reads the range into a scratch buffer, hashes that copy and converts a
  new blob under a new id. A capture records the bytes of the reads, so while one is active the recording view
  reads the range instead of hashing it in place, and the replay builder, which starts with an empty cache,
  makes the same reads. `tox2_rsxreplay --repeat` shows the counts on its `builder:` line.
- *Writes by the RSX.* A color or depth surface that a clear or a draw writes ends the trust in every entry
  whose range overlaps it, at once and without a new epoch; such an entry is read and hashed again at its
  next use. (The renderer keeps surface contents on the GPU, so the guest bytes are normally unchanged and
  the hash keeps the blob.)
- *Unit reuse.* Within an epoch, a texture unit whose registers, known render targets and texture cache state
  (no entry created or dropped, no overlapping write since) are unchanged since its previous draw reuses that
  draw's descriptor instead of decoding the unit and looking the texture up again. A unit whose full path counted
  an unimplemented feature is never reused, so those counts do not change. A reuse does not refresh the entry's
  least-recently-used tick inside the epoch.
- *Write tracking* (`TOX2_RSX_WRITE_WATCH`, on by default; see [Building](building.md#cmake-variables)). Hashing the
  guest bytes of every validated texture once per epoch is most of the texture cost in a frame, and most of those
  textures have not changed. With the option on, the guest memory reservation is made with the Windows write
  watch (`MEM_WRITE_WATCH`), which records the pages any user-mode store of the process wrote. The first use of an
  entry in an epoch first asks the memory view, `RsxMemoryView::checkWrites`, whether a page of the texture's range
  was written since the entry was last checked. If none was, the entry keeps its blob and content id and the bytes
  are not read (a hash skip, still counted as a validation). Otherwise the range is hashed as before.
  - *Why per validation.* One query for the whole local memory at the start of each epoch would be cheaper, but a
    write that lands after the epoch began and before a texture's first use in it would be seen one epoch later
    than the hash path sees it. Asking at the first use, for exactly the pages of the range, keeps today's
    behaviour, and the check is made before the bytes are hashed or read, so a write that lands during the hash
    is reported by the next check.
  - *Page stamps.* Because the OS reports a write once, the game's view keeps one process-wide table with a stamp
    per local-memory page: a check stamps the pages it collected with a new serial, and an entry is clean when no
    page of its range has a stamp newer than the one it stored at its last check. A second builder or view
    therefore cannot consume a report that the first one still needs. Only the local memory (location 0) is
    tracked; main memory textures always take the hash path.
  - *Safety net.* A writer that the watch does not report would keep a stale blob. Each entry is therefore hashed
    on one flip in every 16 (`RsxTextureCache::kHashCheckFlips`) even when its range is reported clean; the entries'
    flips are spread over the 16, so the forced hashes do not arrive together. A stale blob lives at most 16
    flips. A forced hash that finds changed bytes after a clean report counts as an unreported change and logs
    `changed with no reported write` once.
  - *Explicit marks.* Writes that the watch may not see are marked in the guest memory by hand
    (`GuestMemory::markWritten`): the bytes a file read delivers, and commit and decommit of guest pages. Ordinary
    stores (generated PPU code, HLE copies, SPU DMA puts, the inline blit and buffer copy methods) are reported by the watch.
  - *Untracked cases.* While a capture records, the recording view reports nothing tracked, because it has to read
    the bytes to record them; a capture made by a build with the option on therefore holds the same texture bytes
    as one made with it off. Views that do not implement `checkWrites` (test doubles) take the hash path.
  - *Replay.* The replay view has no OS watch. Its `add` compares each incoming chunk with the page it already
    holds and stamps only the pages that differ, so a repeated frame reports its unchanged textures clean. That
    byte compare reads about as many bytes as the hash it replaces, so `tox2_rsxreplay` is not faster with the
    option on, and can be slightly slower; the gain is in the game and in `tox2_bench rsx-fifo`, where the hash
    reads cold guest memory.
  - *Switch.* `TOX2_RSX_WRITE_WATCH=OFF` removes the tracking from the build (no `MEM_WRITE_WATCH`, no marks, no
    checks) and hashes every validated texture once per epoch, as before. The `release-wwoff` preset builds with it
    off into `build/release-wwoff`. The three counters on the replay's `builder` line (hash skips, safety hashes,
    unreported changes) stay 0 then.
  - *Platform.* The tracking uses `GetWriteWatch`, so it is specific to Windows. The macOS build forces
    `TOX2_RSX_WRITE_WATCH` to `OFF` (`cmake/Runtime.cmake`), so every validated texture is hashed there. A host whose write tracking
    reports a granularity other than the guest's 4 KiB page (16 KiB pages on Apple silicon or recent Android) would
    need its own tracking; a mismatch is reported as "range written", so every validation hashes.
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
NEAREST_LINEAR and LINEAR_LINEAR. Otherwise the sampler reads level 0 only. The guest's own anisotropy
field of SET_TEXTURE_CONTROL0 and texels around the image (`CELL_GCM_TEXTURE_BORDER_TEXTURE`) are not
modelled; the player can force anisotropic filtering instead (below).

#### Anisotropic filtering

`--anisotropic-filter off|2|4|8|16` (default `off`), or `Anisotropic filtering` in the
[debug overlay](runtime.md#debug-overlay), sets one fixed level that replaces "no anisotropy" on the
samplers that qualify. It does not combine with a guest value, because the guest's field is not modelled,
so there is none to combine with. This rule is this project's own choice; no RPCS3 parity is claimed. With
`off`, every sampler is exactly what `update_fragment_sampler` made of the game's state.

- *Qualifying samplers.* After the fallback for formats the device cannot filter linearly, the min filter
  is `VK_FILTER_LINEAR` and the sampler reads more than level 0. Point-sampled textures, single-level
  textures (the user interface, fonts and sprites, render targets read as textures, depth read as
  A8R8G8B8) and the placeholder texture keep their samplers, so 2D elements and post-processing reads are
  unchanged. Ground and wall textures seen at a flat angle are what gets sharper.
- *Level.* The sampler gets `min(level, maxSamplerAnisotropy)` of the device. The effective level is what
  the overlay shows and what the start-up line `tox2: render: anisotropic filtering Nx` (written when it is
  above 1) reports; the setting that is saved stays the player's choice.
- *No device support.* Without the `samplerAnisotropy` feature, which the device context enables when it is
  there, every sampler is unchanged, the renderer reports level 1, the log gets `tox2: render: anisotropic
  filtering unavailable on this device` once when a level above 1 was asked for, and the overlay combo is
  disabled with `unavailable on this device`.
- *Changing the level.* The level is part of the sampler cache key. A change made in the overlay applies
  right after the next flip, as the resolution scale does; samplers created afterwards use it and cached
  ones stay (they may be in use by frames in flight). A texture therefore gets its new sampler at
  its next lookup.

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
tox2_rsxreplay CAPTURE [CAPTURE...] [--loop N] [--out DIR]
                      [--compare PNG [--compare-rect X,Y,W,H] [--max-diff N] [--max-mean X] [--max-differing P]]
                      [--resolution-scale N] [--aa-mode supersample|msaa|dlaa] [--no-validation] [--shader-cache DIR]
                      [--vk-driver auto|moltenvk|kosmickrisp]
                      [--dlaa-jitter on|off] [--dlaa-jitter-flip none|x|y|xy] [--dlaa-camera on|off|reset]
                      [--dlaa-clip-flip-y on|off] [--dlaa-mv-invalid zero|unset]
                      [--fg-mv-check [--fg-mv-flip none|x|y|xy]]
                      [--repeat N [--gpu-timing]] [--quiet]
```

| Option | Meaning |
| --- | --- |
| `CAPTURE` | A `.tox2cap` file. Several captures are replayed in the order given, each one a frame, through one renderer, so the state one frame leaves in the targets and caches is there for the next (see below). |
| `--loop N` | Play the list of captures `N` times in order (1 to 1000, default 1). Together with several captures this is how the temporal behaviour of DLAA is tested. |
| `--out DIR` | Write the displayed frame of the last flip as `DIR/<capture name>.png`. With several frames it is written after every frame, so a later frame of the same capture overwrites the earlier file. |
| `--compare PNG` | Compare the last frame (of the last capture of the last pass) with a reference image. The sizes must match, except that a frame whose width and height are whole multiples of the reference's is box-averaged down to it first (the summary line then ends with `; downsampled <fx>x<fy>`). |
| `--compare-rect X,Y,W,H` | With `--compare`: diff only the `W` x `H` rectangle at `(X, Y)`, in the reference's pixels (the rectangle is cut from both images after the downsample, and the summary line gets `; rect <W>x<H>+<X>+<Y>`). A malformed value is a usage error; a rectangle that is empty or outside the image is an error (exit code 1). |
| `--max-diff N` | With `--compare`: fail when the largest per-channel difference exceeds `N`. |
| `--max-mean X` | With `--compare`: fail when the mean per-channel difference exceeds `X`. |
| `--max-differing P` | With `--compare`: fail when more than `P` percent of the pixels differ (by more than 2 in any channel). |
| `--resolution-scale N` | Render every target at `N` times its guest size (1 to 4, default 1; see [Resolution scale](#resolution-scale)). The PNG is `N` times the guest size, and the summary line ends with `resolution scale N` when `N` is above 1. |
| `--aa-mode supersample\|msaa\|dlaa` | How surfaces in the game's anti-aliased mode are rendered (default `msaa`; see [Anti-aliased surfaces](#anti-aliased-surfaces)). `supersample` is the single-sample memory-layout image shaded per sample. `msaa` falls back to it on a device without 2-sample support (`state 2-sample multisampling unavailable` in the census). `dlaa` runs NVIDIA DLAA (see [DLAA](#dlaa)) and needs an NVIDIA RTX GPU and the Streamline DLLs; the tool loads Streamline only with this value. When DLAA is unavailable it uses `msaa` (or `supersample`), prints the warning `tox2: render: DLAA unavailable (<reason>); anti-aliased surfaces use <mode>` and counts `state DLAA unavailable`. The summary line gets `, aa <mode>` when the effective mode is not `msaa`, and `, aa <mode> (dlaa unavailable)` when `dlaa` was asked for and another mode replaced it. Any other value is a usage error. |
| `--dlaa-jitter on\|off` | Diagnostic for the DLAA checks, needs `--aa-mode dlaa` (a usage error otherwise, like the four options below). `off` draws without the sub-pixel jitter. Default `on`. |
| `--dlaa-jitter-flip none\|x\|y\|xy` | Negate the jitter offset reported to DLAA on the given axes. Only the reported value changes, never the geometry. Default `none`, the measured winner. |
| `--dlaa-camera on\|off\|reset` | `on` (default) describes the camera motion between frames from the scene draws' projections. `off` passes identity matrices. `reset` passes identity matrices and drops the history at every frame. |
| `--dlaa-clip-flip-y on\|off` | Whether the camera's clip-to-clip matrices are mirrored in y for Streamline's y-up clip space. Default `on`. |
| `--dlaa-mv-invalid zero\|unset` | Set `motionVectorsInvalidValue` to 0 (`zero`, default) or leave the SDK default (`unset`, which makes Streamline warn once per process). It does not change the picture. |
| `--fg-mv-check` | Diagnostic for [frame generation](#frame-generation), needs `--aa-mode dlaa` (a usage error otherwise, and together with `--repeat`). Draws the camera motion vectors that frame generation would use and, from the second frame on, prints `fg mv: frame <i>, warp mean <w>, no-motion mean <z>, <n> pixels` (`, reset` when the history was dropped for that frame): the mean absolute difference (RGB, 0 to 255) between the current DLAA output and the previous one warped with the vectors, and between the two unwarped. A lower `warp mean` than `no-motion mean` means the vectors follow the camera. The displayed frame does not change. |
| `--fg-mv-flip none\|x\|y\|xy` | Diagnostic, needs `--fg-mv-check`: negate the vectors on the given axes. Default `none`, the measured winner. |
| `--vk-driver auto\|moltenvk\|kosmickrisp` | The Vulkan driver, as for `tox2_game` (default `auto`; see [Vulkan on macOS](#vulkan-on-macos)). The tool prints the device it used as `render: vulkan device <name> (driver <driver>)`. A driver that is not installed leaves the replay without a device (`render: unavailable: no Vulkan device with driver <driver> (found: ...)`; exit code 1 with `--repeat`). Any other value is a usage error. |
| `--no-validation` | Do not enable the Vulkan validation layer, which is on by default. |
| `--shader-cache DIR` | SPIR-V cache directory. Without it nothing is cached. |
| `--repeat N` | Replay and render the capture `N` times (1 to 100000) and print the median time of each phase (see below). A usage error together with `--out`, `--compare`, more than one capture or `--loop`; exit code 1 without a Vulkan device. |
| `--gpu-timing` | Needs `--repeat` (a usage error otherwise). Time every rendering instance on the GPU with timestamp queries and print the `gpu` and `gpu top` lines (see below). Off, the renderer creates no query pool and records no extra command. |
| `--quiet` | Do not print the per-command lines (`clear`, `draw`, `flip`); everything else is printed. |

The thresholds are optional and unchecked unless given, and they need `--compare`. The tool prints
the commands the frame builder produced (`clear`, `draw` with its colour and depth target (with `zt=`, `zw=` and
`st=` for its depth test, depth write and stencil test) and, for each texture, the render target it aliases or the format and mip count of its bytes, `flip`), then, with a device, the line
`render: vulkan device <name> (driver <driver>)`, then a summary line
`replay: N commands, N draws, N skipped` (with the diff figures when comparing), the
`rsx unimplemented` section (`census: rsx unimplemented 0 kinds` when it is empty), the path of
the PNG, and `validation: active|inactive|disabled, N errors`. The exit code is 1 when a
threshold is exceeded, a file cannot be read or written, there is no frame to display or the
validation layer reported an error, and 2 for a usage error. Without a usable Vulkan device it
still prints the command list and counts every draw as skipped.

With several captures or `--loop`, each capture has its own file reader, memory view and frame builder, so one
frame's guest memory never leaks into the next, and all frames execute on one renderer. A frame is replayed
(`replayCapture`, `takeCommands`, `recycle`, as `--repeat` does) and executed, and the list is played `N` times in
order. The frames are separated by their flips, so every capture must end with a flip when more than one frame is
replayed (otherwise a usage error names the capture). The command lines are printed for the first pass over the
list only. `--out` writes `<capture name>.png` after every frame, `--compare` and its thresholds apply to the last
frame, and the summary line counts the commands and draws of all frames and ends its counts with `, frames F` when
`F` is above 1 (the order is `frames`, `resolution scale`, `aa`, then the diff figures). When the effective
anti-aliasing mode is `dlaa`, one more line follows the summary:
`dlaa: <n> evaluations, <n> resets, <n> failures`. For example, the two consecutive captures of the cutscene,
replayed 8 times, are `tox2_rsxreplay frame_8700_cutscene.tox2cap frame_8701_cutscene.tox2cap --loop 8 --aa-mode dlaa`.

`--repeat N` measures the frame builder and the renderer without the game. One process runs the capture `N` times
with one memory view, one frame builder and one renderer, so the texture cache, the program caches and the pipelines
are warm, as they are in the game's steady state. An iteration replays the capture's writes into the builder, takes
the commands, executes them one command at a time and gives them back to the builder (`recycle`). The first
iteration creates the pipelines and uploads the textures, so it is left out when `N` is 2 or more. Turn the
validation layer off with `--no-validation` when timing, because it costs CPU for every Vulkan call. The per-command
lines are not printed; the summary line, the `rsx unimplemented` section and the validation line follow the four
lines below and describe the last iteration. A later iteration draws over the targets of the one before, so the image
is not the capture's frame, which is why `--out` and `--compare` are refused. Four lines are printed, here with
illustrative figures:

```
timing: frame_9400_room, 30 iterations (first excluded), median ms per iteration: build 6.10 (buildDraw 4.83), recordDraw 5.33, recordClear 0.10, submit 0.41, gpu wait 0.02, total 11.94
cycles: frame_9400_room, 30 iterations (first excluded), median thread cycles per iteration: build 21334455, record 18273645, total 40112233
builder: per iteration 627 draws, fp cache 620 hits 7 misses, vp cache 600 hits 27 misses, vertex bytes 1234567 copied (union 1234567), texture validations 40 (3456789 bytes hashed in place, 0 bytes read), texture unit cache 0 hits 0 misses, texture hash skips 0 (safety hashes 0, unreported changes 0)
render: per iteration 115 barriers (3 skipped), 31 rendering instances, 552 state calls (11988 skipped), 12 vp constant uploads reused, texture binds 0 cached 0 full; upload memory 0x7 device-local host-visible host-coherent
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

The `cycles` line gives the median over the same iterations of the cycles the replaying thread ran
(`QueryThreadCycleTime`, which counts at the time-stamp counter rate), in three columns: `build` is `replayCapture`
plus `takeCommands`, `record` is the whole loop that executes the commands (the three recording phases and the
`gpu wait` above), and `total` is the iteration from before `replayCapture` to after `recycle`. Time the thread is
not scheduled does not count, so other load on the machine moves these figures far less than the `timing` line, and
they are the figures to compare two builds by. Each column is a median of its own, so `build` plus `record` need not
equal `total` exactly.

The `builder` line gives, per iteration, the draws, the hits and misses of the fragment and vertex program caches
and the vertex bytes copied. The union figure is the size of the union of the attribute ranges per location, counted
separately from the merge; it equals the bytes copied unless the merge is wrong. The line ends with the texture
unit cache: the enabled texture units the builder reused from the unit's previous descriptor (hits) and the ones it
decoded and looked up in full (misses). After it come the write-tracking figures (`TOX2_RSX_WRITE_WATCH`): the
validations that kept a cached texture because the memory view reported no write to its pages (hash skips, nothing
hashed), the validations hashed only because the entry's safety-net flip came (safety hashes), and, of those, the ones
whose bytes had changed with no write reported (unreported changes, a writer the view misses). Unreported changes
should stay 0; all three are always 0 in a build with the option off. In the replay, the skips come from the textures
that repeat unchanged between iterations. The `render` line gives the
barriers recorded and the transitions that needed none, the rendering instances begun, the state calls recorded and
skipped, the vertex constant uploads reused (see [Rendering](#rendering)), the texture binds that reused the unit's
previous bind of the frame (cached) and the ones that ran the full acquire, view and sampler path (full), and the property flags of the memory
that uploads go to (`device-local`, `host-visible`, `host-coherent`, `host-cached`). Without `host-cached` that
memory is write-combined, where reads are slow and single-byte stores are slower than whole-element stores.
Run-to-run spread on one machine was a few percent for `buildDraw` and `total` and up to 7 % for `recordDraw`, so
a difference smaller than that is noise; compare interleaved runs of the two binaries.

With `--gpu-timing` the renderer also times the GPU. Each frame's command buffer records a timestamp at its first and
last command, and one before every rendering instance begins and one after it ends. Timestamps are never written
inside an instance, because a tile-based GPU (Apple, through MoltenVK) cannot sample in the middle of a pass. The
results are read when the frame slot's fence has been waited for, and the frames still in flight are collected at the
end of the run. After the four lines above come:

```
gpu: frame_4300, 10 iterations (first excluded), median ms per iteration: frame 34.52, instances 32.21 (draw 22.15, clear 2.15, masked-clear 0.00, layout 5.75, depth-argb 2.10), outside 2.34
gpu top: #27 layout 10240x2880 s1 none depth: 4.70 ms
gpu top: #16 draw 5120x2880 s2 bgra8 depth draws 844: 4.55 ms
```

The `gpu` line gives the median over the same counted iterations (the first is left out when `N` is 2 or more):

- `frame`: the time from the first to the last command of the frame on the GPU.
- `instances`: the time of all rendering instances added up, then the same split by kind. `draw` is the instance a run
  of draws with the same attachments shares, including the clear that began it (a clear leaves its instance open for
  the draws after it, and the instance is timed as one, so the clear's own cost is not separate), `clear` a clear of
  whole channels that no draw continued, `masked-clear` a clear through a colour write mask, `layout` the pass that rewrites the memory layout of a 2-sample target from its sidecar image, and
  `depth-argb` the pass that rewrites a depth surface as A8R8G8B8. An instance's time runs from the timestamp
  before its begin to the one after its end, so the barriers and copies recorded between instances are not in it.
- `outside`: `frame` minus `instances`. It covers everything between instances: transfers and blits, clears
  outside an instance, uploads, texture work, and the present blit.

Each `gpu top` line is one of the up to 15 most expensive instances of the last counted iteration, longest first:
`#<index>` is the instance's position in the frame (counting from 0), then its kind, render area in pixels, samples
(`s1`, `s2`), colour format (`bgra8`, `r32f`, ... or `none`), `depth` when a depth attachment is bound, and, for
`draw`, the number of draws recorded into the instance, followed by `after clear` when a clear began it. A frame has
room for about 2000 timed instances; later ones are not timed and a warning on standard error gives the count. When the device has no timestamp support the `gpu`
line says `unavailable` and why. The timestamps themselves cost some GPU time, so the `gpu wait` of the `timing` line
can be higher with `--gpu-timing` than without it (17.6 ms against 14.1 ms for `frame_4300` at scale 4).

The CTest entry `rsx_replay_repeat` runs `tox2_rsxreplay generated/rsx/captures/frame_9400_room.tox2cap --repeat 3
--quiet --no-validation` (the capture is named in `cmake/rsx_replay.cmake`) and passes when the `cycles` line is
printed. It is registered only when the capture exists. The entries `rsx_replay_builder_counters`,
`rsx_replay_hash_counters` and `rsx_replay_render_counters` run the same command and pass when the `builder` line holds
the texture unit cache figures, the `builder` line ends with the hash skip figures, and the `render` line holds the
texture bind figures. The entry `rsx_replay_hash_skip` runs it too and checks the value of the hash skip figures for the
build: more than 0 skips and 0 unreported changes with `TOX2_RSX_WRITE_WATCH` on, and all three figures 0 with it off.

The CTest entry `rsx_replay` runs `tox2_rsxreplay` on `generated/rsx/captures/title.tox2cap` with
the options pinned in `cmake/rsx_replay.cmake`. The capture name `title` is only the name the test looks up; it says nothing about the
scene in the capture. The file is game-derived and git-ignored, so the test is registered
only when the capture exists; configuration prints the reason otherwise. The pin file also holds the
slot for the SHA-256 of a reference image (`generated/rsx/reference/title.png`) and the thresholds
(largest difference 8, at most 0.5 percent of the pixels differing). The SHA-256 is empty, because
no accepted reference image exists for this game, so the test only replays the capture and
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
