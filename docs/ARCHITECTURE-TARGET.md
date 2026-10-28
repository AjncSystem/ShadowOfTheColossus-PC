# Target architecture: a high-performance GPU path

Design notes from a code review (Oct 2026) of the GPU path of this port, with a migration plan.
Nothing here has been measured on hardware yet. Each step ships behind a `SOTC_*` switch,
default off, and turns on only after the attract and Shrine benchmarks and an image comparison
with the serial path (`SOTC_CP_THREADS=0 SOTC_CP_STRICT=1`) pass.

## Where the time goes today

- **One thread per stage.** `GpuCommandProcessor` (PM4 parse, `amdgpu/liverpool.cpp`), then
  `GpuDraw` (`Rasterizer::Draw`, `renderer_vulkan/vk_rasterizer.cpp`), then `GpuRecorder`
  (replays a closure stream into real command buffers, `vk_recorder.cpp`). Each stage is serial.
  At ~5800 draws a frame the draw and recorder threads saturate while the GPU is ~75% idle.
- **One frame in flight on the guest side.** `sceGnmSubmitAndFlipCommandBuffers*` calls
  `WaitGpuIdle()` before every submit (`core/libraries/gnmdriver/gnmdriver.cpp:2299`, `:2342`,
  `:2359`). That waits for the command processor and the draw ring, not for the GPU.
- **One Vulkan queue.** Copies, detiling, sparse binds and draws share it
  (`vk_instance.cpp:461-494`).
- **Per-draw descriptor traffic.** Push descriptors or `updateDescriptorSets` on every draw, and
  the recorder deep-copies every write (`vk_recorder.cpp:230-266`).
- **Pipelines compile per VS×FS combination.** Binding numbers run across stages
  (`vk_pipeline_cache.cpp:1162-1200`, `spirv_emit_context.cpp:849-1082`), so a stage's SPIR-V
  depends on the other stages. Pipeline-library parts therefore cannot be shared.
- **CPU waits on the streaming path.** `scheduler.Finish()` runs in `ReleaseMemory`,
  `AllocateArenaBlocks` (VRAM full) and `FinishEviction`. Write tracking takes one exception per
  4 KiB page written, and live view splits stop every guest thread (`src/runtime_memory.c`).

## Target

```
guest threads ──► Command processor ──► Sequencer ──► Draw workers (N) ──► primary CB
 (no per-submit      PM4 parse,           render-pass      per-worker regs,     vkCmdExecuteCommands
  idle gate)         draw packets          batches,         DynamicState,       in slice order
                     + reg deltas          barriers,        secondary CBs
                                           cache mutations
                                                │
              bindless heap (images, samplers) ◄┘   transfer queue: copies, sparse binds
              stage-independent descriptor sets ──► GPL fast-link / shader objects
```

### 1. Parallel draw recording (largest CPU win)
- Draws are cut into **render-pass batches**. A single sequencer thread resolves attachments,
  barriers, `FlushPeriodic`, readbacks, and everything that mutates the caches.
- **N draw workers** each record a contiguous slice of a batch into a secondary command buffer.
  The primary runs them in order with `vkCmdExecuteCommands`, so blend and depth order are kept.
- Register state reaches each worker through the **existing delta mechanism** (64-dword chunks
  from `MarkRegs`, copied in `BackAlloc`, replayed in `ExecuteBackOp`, `liverpool.cpp:209-330`).
  Each worker keeps its own copy of the registers. A full 208 KB snapshot per draw is not viable.
- Blockers to remove first:
  - `Shader::Info::runtime` has only 2 slots (`shader_recompiler/info.h:118-180`).
  - The Rasterizer's per-draw members (`vk_rasterizer.h:229-294`).
  - The single `DynamicState`.
  - `RenderState` lacks formats and sample count.
  - `Real()` uses one global table, and recorder blocks carry no category.
- The texture cache becomes read-mostly: a generation-checked table, a per-worker memo, and
  misses and mutations queued to the sequencer.

### 2. Bindless descriptors
- One global **descriptor-indexing heap** (UpdateAfterBind, PartiallyBound) for sampled images,
  storage images and samplers. Each image view and sampler gets a stable index when it is created.
- Shaders read `heap[table[slot]]`. `table` is a small per-draw array in stream memory, reached
  through a buffer-device-address pointer in push constants (PushData is 56 bytes, so 8 more fit).
- Per-draw cost becomes one small table write. There is no `updateDescriptorSets` and no recorder
  copy.
- Stream buffers (Flatbuf, ClipPlanes, LDS) are passed as device addresses. Guest buffers stay on
  descriptors until the recompiler clamps addresses itself.
- Images that need a feedback-loop or depth-read-only layout keep the legacy path; heap entries
  are pinned to GENERAL.
- This replaces the descriptor-buffer experiment (`SOTC_DESC_BUFFER`). It needs the same shader
  work, saves more per draw, and adds no extra buffer management.

### 3. Pipelines without stutter
- **One descriptor set per stage** (FS=0, VS=1, ...), so each stage's SPIR-V depends only on its
  own resources. Set layouts are cached by binding signature.
- Then **VK_EXT_graphics_pipeline_library**: shared pre-raster and fragment parts, a synchronous
  fast link when a combination is first seen (no skipped draws), and an LTO link on the workers
  in the background. Branch `worktree-agent-af29089847845767a` enables the extension.
- Later, **VK_EXT_shader_object**: no pipeline objects at all, and all state set through
  `vkCmdSet*`.
- This changes every shader, so the cache versions in `vk_pipeline_serialization.cpp:26-28` must
  be bumped, and the cache is rebuilt once.
- The play-through warm cache (`SOTC_ASYNC_PRELOAD`) stays. A static scan of the game cannot
  predict permutations.

### 4. Memory and streaming
- **Arena blocks freed by GPU tick**: each freed run keeps the tick that last used it. No
  `Finish()` on unmap, VRAM exhaustion or eviction. `SOTC_UNBIND_GPU_WAIT` is the first half.
- **Coarser write tracking**: unprotect a 64 KiB granule on the first fault (about 16x fewer
  exceptions while streaming), and hash on use for small vertex, index and constant ranges.
- **A transfer queue** for buffer copies and sparse binds, handed over with timeline semaphores
  (concurrent sharing or ownership barriers).
- **ReBAR upload heap** (DEVICE_LOCAL|HOST_VISIBLE) for staging when the driver exposes it.
- **VK_EXT_host_image_copy** for linear images only, since tiled images still need the detiler.
- Chunked host views (`SOTC_VIEW_CHUNK_MB`) bound the freeze of a live view split.

### 5. Frame pipelining, threads and presentation
- Relax the per-submit gate so it waits for the command processor only (`SOTC_CP_AHEAD`). This
  lets frame N+1 be parsed while frame N records. `DrainBack` (`liverpool.cpp:681`) already orders
  constant writes; reuse of command buffers by the guest must be tested.
- **Thread placement with CPU Sets** (`GetSystemCpuSetInformation`):
  - The command processor, draw thread(s) and recorder each get their own physical core.
  - Pipeline workers are limited to 4, run below normal, and use SMT siblings.
  - Spinning gives way to waiting on an atomic after 10–50 µs (copy pool, draw ring, recorder).
- **Presentation paced to the display**: VK_KHR_present_id and VK_KHR_present_wait on FIFO, with
  flips on the refresh grid (k = refresh/30). Use VRR at 144 Hz, where 30 FPS cannot divide evenly.
- **Reflex** (VK_NV_low_latency2 through Streamline) without DLSS-G.
- EOP labels signalled after the GPU tick, without one flush per EOP.

## Migration order

| # | Step | Switch | Files | Risk |
|---|---|---|---|---|
| 1 | Measure: draw_idle_ns, RecorderBusyNs, the `SOTC unmap` line, `SOTC SYNC PIPELINE` reasons | `SOTC_FPS=1` | — | none |
| 2 | Arena free by tick; GPU-side unbind on by default | `SOTC_ARENA_TICK_FREE`, `SOTC_UNBIND_GPU_WAIT` | `buffer_cache.cpp/.h` | medium |
| 3 | CPU Sets, worker cap, no spinning | `SOTC_CPUSETS`, `SOTC_WAIT_NOSPIN` | `bbport_threads.h`, `liverpool.cpp`, `vk_recorder.cpp`, `bbport_copy.cpp` | low |
| 4 | Present pacing (try `BB_PRESENT_MODE=Fifo` first) | `SOTC_PRESENT_WAIT` | `vk_instance.cpp`, `vk_presenter.cpp`, `driver.cpp` | low |
| 5 | Command processor runs ahead of the GPU idle gate | `SOTC_CP_AHEAD` | `gnmdriver.cpp` | medium |
| 6 | RenderState formats, per-slot `Info::runtime`, one worker recording secondaries | `SOTC_PASS_SEC` | `vk_scheduler.h`, `info.h`, `liverpool.cpp`, `vk_rasterizer.cpp` | high |
| 7 | N draw workers, read-mostly texture cache | `SOTC_DRAW_WORKERS=N`, `SOTC_TC_SHARED` | same + `texture_cache.*` | high |
| 8 | Bindless samplers → textures → storage → stream BDA | `SOTC_BINDLESS*` | `spirv_emit_context.cpp`, `emit_spirv_image.cpp`, `vk_rasterizer.cpp`, `texture_cache/*` | high |
| 9 | Per-stage descriptor sets, then GPL, then shader objects | `SOTC_STAGE_BINDINGS`, `SOTC_GPL`, `SOTC_SHADER_OBJECT` | pipeline cache, recompiler backend | high |
| 10 | Coarse write tracking, transfer queue, ReBAR, host image copy | `SOTC_FAULT_GRANULE`, `SOTC_XFER_QUEUE`, `SOTC_REBAR_UPLOAD`, `SOTC_HOST_IMAGE_COPY` | `page_manager.cpp`, `vk_instance.cpp`, `vk_scheduler.cpp`, `buffer.cpp` | medium–high |

Steps 2–5 target stutter and frame pacing with small changes. Steps 6–8 target the CPU limit on
draws, and step 9 the remaining compile stalls.

## What was ruled out
- Full register snapshots per draw (208 KB × 5800 draws per frame).
- Suspending only some threads during a live view split. Guest code can touch any address.
- Large pages, since 2 MiB pages break 4 KiB write tracking.
- `MEM_WRITE_WATCH`, which does not work with section views.
- DirectStorage and IoRing: archives are read into RAM ahead of time, so reads are memcpy.
- mimalloc: no allocation churn has been measured.
- A pre-translated shader cache shipped with the port: permutations depend on runtime state, and
  game data cannot be redistributed.
