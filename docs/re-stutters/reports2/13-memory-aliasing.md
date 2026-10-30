# 13 - Memory aliasing of render targets and textures

Read only. No repository file edited except this report. Addresses are eboot VAs (EU 1.01,
`out/eboot.elf`) unless marked port (file:line in the working tree). Labels: **observed**
(disassembly, pseudocode, source line or log line), **inferred**, **unknown**.

## 1. Answer

- **observed:** the engine allocates its named render targets (RTs) once per area load, not per
  frame. The chain is `AreaLoad_PrecacheLists` (0x5331e9) -> `AreaLoad_ShaderListAndAssets`
  (0x2569d0, call at 0x256c5b) -> `VideoOut_OpenSetFlipRate` (0x95a90, call at 0x95dd8) ->
  `render_targets_allocate` (0x9aef0). `render_targets_allocate` creates 40 `kRTT_*` objects through
  the factory 0x97a50 (26 call sites in that one function). The only caller of 0x95a90 is the area
  load path.
- **observed:** the RT names are fixed: the same 40 names, each created at its own call site with
  fixed constant arguments. `kRTT_MainDepth` goes through 0x8d6f0 with constant 9 among its
  arguments; `kRTT_MainColor` goes through 0x8d6c0 with constant 1. The argument meanings (format
  or usage) are **inferred**, not named. Within the RT pool, a given RT object keeps its format
  for the whole area.
- **observed (the one per-frame reinterpretation):** the port's own comment says the game switches
  some depth buffers between depth and colour use every frame, about 100 conversions/s in the open
  world (`texture_cache.cpp` ~375-420, "sotc: the game switches some depth buffers..."). The port
  answers this with depth twins (`SOTC_DEPTH_TWIN`, default on since 2026-10-16). This is the
  clearest per-frame aliasing in the data, and it is already handled.
- **observed (the port's churn counters):** in the first logs (`c14_base.log`, 16 windows of 5 s)
  the port created about 6,850 images (sum of `images new` x 5 s). The cache grew from 114 to 1,655
  images. The texture cache collector freed about 40 images in total. So roughly 5,000 images left
  the cache by a path other than the collector. **inferred:** that path is `FreeImage` called from
  overlap resolution, expansion or unmapping, and those frees are not counted anywhere.
- **observed (the path that most likely does it):** `TextureCache::UnmapMemory` (`texture_cache.cpp`
  353-362) frees every image inside an unmapped range, and it does not count the frees. The game
  unmaps and maps again inside its heaps and streaming pool (report 01, `SOTC_UNMAP_READBACK`
  comment in `vk_rasterizer.cpp` 7049-7083). **inferred:** a remap at the same guest address then
  makes `FindImage` create a new image there. That is the "new image at the same address" the brief
  describes.
- **observed (the second aliasing path, not counted):** `ResolveOverlap`, equal-address branch
  (`texture_cache.cpp` 496-499): when the cached image has a different block size or dimensions,
  the code frees it and says "Very likely this kind of overlap is caused by allocation from a
  pool." Those frees are not counted either.
- **observed:** the GC line `reused slots` is 0 in every log. I did not trace what it counts, so it
  is not evidence either way.
- **unknown:** which pool causes most of the churn. Candidates are the texture streaming pool
  (`Streaming Pool Total Size MiBs`, 0xfa82bc, `UpdateTextureStreamingGPUData` 0x117930) and the
  per-frame render contexts (`RequestNewRenderContext`, default 5 MiB linear chunk, 0x90470). The
  logs do not say which address range each created image lies in.
- **Best proposal:** first split the image frees by reason (a counter, no hook), then try to keep
  the `Image` object of an unmapped range parked (the port already has `ParkImage`, texture_cache.h
  ~750) and re-arm it on remap. Engine hooks are secondary here; see section 3.

## 2. Evidence

### 2.1 Engine RT allocator (static)

- **observed:** `tools/re/names.txt` line 81: `0x9aef0 render_targets_allocate` ("kRTT_*"). Strings
  `kRTT_Display` (0xf9f7bd) through `kRTT_Ambient1` are the 40 names; each is passed as the name
  argument of a factory call. Examples at 0x9aef0 pseudocode: line 633 `FUN_0008d6c0(...,"kRTT_Display")`,
  line 691 `FUN_00097a50(...,"kRTT_DOFAmount")`, line 891 `FUN_0008d6f0(...,9,"kRTT_MainDepth")`.
- **observed:** the factory 0x97a50 (2,834 bytes) is called by `render_targets_allocate` (26 sites)
  and by about 20 other functions. The wrappers are 0x8d6c0 (45 bytes, one call to 0x97a50) and
  0x8d6f0.
- **observed:** `VideoOut_OpenSetFlipRate` (0x95a90) has one caller, `AreaLoad_ShaderListAndAssets`.
  `AreaLoad_ShaderListAndAssets` has one caller, `AreaLoad_PrecacheLists` (0x5331e9).
  `eboot_re.py callers` output, this session.
- **observed (named heap):** `Render Targets` is a named heap (string in 0x8c370, `FUN_00011bc0`).
  Its tag is `DAT_02ed1fc8`, read at 0x9aa10 and 0x9aef0 as the allocation tag. 0x9aa10 allocates a
  0x10000-byte block from the heap set at `root+0xa0` (with `mem_allocator_alloc` 0x479a0). Its
  caller is not visible in the static call graph (the tool lists none; probably an indirect call).
- **observed:** `sub_8c370` (1,254 bytes, unique signature 16 bytes) creates the named heaps and the
  two `Default TaggedArena` objects (kind 1 and kind 2). The heap names are `Renderer`, `Render
  Targets`, `Textures`, `CodeGen Textures`, `Fur Textures`, `Water Textures`, `LightProbe
  Textures`, `Models`, `Particles`, `Lighting`, `VirtGeometry`, `Tessellation`. This matches
  report 06.

### 2.2 Per-frame render contexts (static)

- **observed:** `PerFrameRenderContexts::RequestNewRenderContext` (0x90470, 509 bytes) allocates a
  render context with a default linear chunk of 0x500000 bytes (`lVar7 = 0x500000`). Callers are
  `sub_90430` (0x90449) and `Flip::SetupFlipState` (0x90af2). So a new context per flip, each with
  its own 5 MiB scratch. These are buffers (linear memory), not images. They are the candidate for
  per-frame address reuse of GPU-written memory, which the port tracks as buffers.
- **observed:** the strings `RenderSingleFrameGPU` (0xf9ed23) and `RenderContextFrameGPU`
  (0xf9eff8) are the names of the two per-frame GPU contexts. No "transient" or "alias" allocator
  name exists in the binary (strings `Transient`: 0 hits).

### 2.3 Texture streaming pool (static, partial)

- **observed:** the pool is configured by cvars: `texturePoolBaseSizeMiBs` (0xfa81d4),
  `texturePoolSizeMiBs` (0xfa81ec), `Streaming Pool Total Size MiBs` (0xfa82bc),
  `Streaming Pool Base Size MiBs` (0xfa82db), `enableTextureStreaming` (0xfa804c).
- **observed:** `CRenderer::UpdateTextureStreamingGPUData` (0x117930, 1,982 bytes) is called by
  `sub_117140` (0x11715a). Its signature is not unique within 64 bytes (2 matches), so a hook there
  needs a longer signature or an anchor on the call site.
- **unknown:** how a streamed texture is placed in the pool (fixed slots or a sub-allocator). I did
  not trace it. The streaming chunk strings (`TextureStreamingChunk::ProcessTextureData`, 0xfa6388)
  are the next place to look.

### 2.4 Port side (observed in source)

- `gpu/shadps4/video_core/texture_cache/texture_cache.cpp`:
  - 873-1010 `FindImage`: a new image is created only when no cached image matches. The counter
    `images_new` increments at 983 (`SotcStats::images_new`).
  - 483-520 `ResolveOverlap`: equal-address branch. Different block size or dimensions frees the
    cached image (496-499) with the comment "Very likely this kind of overlap is caused by
    allocation from a pool."
  - 364-420 `ResolveDepthOverlap`: depth/colour reinterpretation, recreate or twin (`SOTC_DEPTH_TWIN`).
  - 353-362 `UnmapMemory`: frees every image in the range with a TODO ("Download image data back to
    host"). No counter.
  - 785 `ExpandImage`: frees the old image and creates a new one.
  - 1905, 1935: `cached_images` increments in `RegisterImage` and decrements in `UnregisterImage`.
  - 2204: `gc_freed` (the collector only).
- `gpu/shadps4/video_core/texture_cache/texture_cache.h` 744-748: `FreeImage` = Untrack +
  Unregister + Delete. Every free path goes through it, so one counter there can split the reasons.
- `gpu/shadps4/video_core/texture_cache/texture_cache.h` ~750: `ParkImage` (the twin code's "what
  FreeImage does except the reclaim"). It already exists.
- `gpu/shadps4/core/libraries/videoout/driver.cpp` 1239-1256: the `SOTC GC:` line (ticks/frame,
  freed/s, written back, images new/s, cached, MB, reused slots).
- `gpu/shadps4/video_core/sotc_stats.h` 1017-1026: `images_new`, `image_ns`, `image_dedicated`,
  `gc_freed`, `cached_images`.
- `gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp` 7049-7083: `Rasterizer::UnmapMemory`
  bumps `map_epoch`, calls `buffer_cache.OnMappingChanged()` and then `texture_cache.UnmapMemory`.
  Called from `gpu/shim/bbgpu.cpp:123`.

### 2.5 Logs (observed numbers)

Source: `scratchpad/waveB/*.log` (session logs from today). The `SOTC GC:` line is printed every 5 s.

| log | GC windows | images new (sum x 5 s) | cached start -> end | GC freed (sum) |
|---|---|---|---|---|
| c14_base.log | 16 | about 6,850 | 114 -> 1,655 | about 40 |
| run_s02.log | 30 | about 9,450 | 101 -> 1,498 | about 335 |

- **observed:** in `run_s02.log` the net cache growth is about 1,400 images, against about 9,450
  created. The collector freed about 335. So about 7,700 images left the cache by another path, the
  same gap as in `c14_base.log`.

- **observed:** `freed` is 0-4 per second in most windows, and `images new` is 70-240 per second.
  `cached` grows steadily. So the collector is not the source of churn.
- **observed:** per-frame spikes (`SOTCSTALL` lines, `images N`): frames with 670-721 images occur
  in each session log (for example `SOTCSTALL frame 189 ... images 716`, `frame 194 ... images 721`,
  `frame 212 ... images 710`, `frame 502 ... images 670`). Their `alloc` part is 9-14 ms; most of
  the image objects did not need new memory.
- **inferred:** the 670-720 spikes are first use of many images at boot and at area entry, not a
  per-frame RT reset. The engine creates only about 40 RTs per area, so the spikes are mostly
  textures and geometry. Frame 188-212 and 493-502 are the two windows where this shows.
- **caveat:** the spike frames were found with `grep -h` over all logs; the log file for each spike
  was not recorded. The sums above assume the window timing is 5 s.

## 3. Hook proposals

The brief asks for engine hooks. Most of the aliasing is in the port's texture cache, so the first
two proposals are port-side. The engine hooks come after them.

### H0 (port, not an eboot hook): count image frees by reason

- **Site:** `FreeImage` (`texture_cache.h` 744) with a reason argument, or one counter per caller:
  `UnmapMemory`, `ResolveOverlap` equal address (496-499), `ResolveOverlap` other branches (564,
  590, 723, 739, 747), `ExpandImage` (785), `FindImage` resolve failure (975), GC (2204), and the
  depth twin (`ResolveDepthOverlap`).
- **Mode:** a counter printed in the `SOTC GC:` line. No behaviour change.
- **Change:** adds one `SotcStats` counter per reason and prints them every 5 s. Nothing else.
- **Why it helps:** it tells whether the 5,000-image gap is `UnmapMemory`, overlap resolution or
  expansion. Every later proposal depends on this answer.
- **What could break:** nothing, if it only increments atomics. Keep the cost under the
  `SOTC_FPS` switch.
- **Verify:** run `c14_base`-style session; check that the sum of the new counters matches the
  cache-count drop (cached start + created - freed - cached end ≈ 0).
- **Signature:** not applicable (port code).

### H1 (port): park images on unmap, re-arm them on remap

- **Site:** `TextureCache::UnmapMemory` (`texture_cache.cpp` 353-362). Use `ParkImage` (texture_cache.h
  ~750) instead of `FreeImage` for images whose range is later mapped again.
- **Mode:** a port change behind an env switch (for example `SOTC_UNMAP_PARK=1`). Default off until
  measured.
- **Change:** on unmap, park the image (keep the slot and the Vulkan objects, drop the tracking). On
  the next `FindImage` at the same guest address and matching `ImageInfo`, unpark it and mark it
  `GpuDirty`, so it is uploaded again from memory before use. Cap the number of parked images
  (for example 256 or a MB budget) and free the oldest.
- **Why it helps:** it removes the create/free pair that the port pays on each remap. `images new`
  is the counter that tracks it. If the gap in 2.5 is unmap-driven, this removes most of the
  roughly 5,000 creations per 80 s, and so most of the 670-720 image spikes at area entry.
  **inferred gain; unknown in ms.** The spikes cost about 9-14 ms of `alloc` each, so the gain at
  area entry is likely one or two frames of 300-400 ms; the steady-state gain is unknown.
- **What could break:**
  - Stale contents: the game can write the range while it is unmapped. Those writes never reach the
    port. Mitigation: re-upload on unpark (the image is `GpuDirty`), and never reuse a parked image
    across an `UnmapMemory` that the game follows with a CPU write fault path. Unknown whether the
    guest always writes before the GPU reads.
  - Memory: parked images keep VRAM. The cap must include them in the GC budget.
  - Crash history: `SOTC_UNMAP_READBACK` broke the heap when a readback ran during unmap
    (`vk_rasterizer.cpp` 7055-7063). Parking must not read back guest memory during unmap.
- **Verify:** `=2`-style run: count only the parked hits and misses with H0 counters, no
  behaviour change. Then enable and check `images new` per 5 s and frame times at the Shrine and the
  attract demo.
- **Signature:** not applicable (port code).

### H2 (eboot, Entry): log every RT set build with its area

- **Site:** `VideoOut_OpenSetFlipRate`, 0x95a90 (signature unique, 32 bytes:
  `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 88 00 00 00 48 8b 05 ?? ?? ?? ?? 4c 8d 3d ?? ??`).
- **Mode:** Entry, with `=2` as the verify mode (log only, no effect).
- **Change:** on entry, log a counter and the frame number. This confirms that the RT set is built
  once per area load (expected) and not on each flip.
- **Why it helps:** it confirms or rejects the per-frame RT reset that the brief asks about. No gain
  itself.
- **What could break:** nothing if it only logs. The signature is unique.
- **Verify:** `=2`, then check that the counter moves only at area load.
- **Signature:** unique at 0x95a90 (`eboot_re.py sig` checked).

### H3 (eboot, Site): stable RT rebuild (keep RT objects across an area reload with the same sizes)

- **Site:** `render_targets_allocate`, 0x9aef0 (signature unique, 24 bytes:
  `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 e4 e0 48 81 ec 20 05 00 00`).
- **Mode:** Site at the start of the function, or Wrap that skips the body when the RT set is
  already valid for the same sizes.
- **Change:** skip the rebuild when the RT set already exists with identical sizes and formats.
- **Why it helps:** it removes about 40 RT creations per area load, if the game rebuilds the RT set
  on each load.
- **What could break:** the game may rely on the rebuild for its state (clears, initial contents,
  reset of frame counters). Skipping it leaves stale RTs. The gain is likely small: 40 creations
  against about 5,000 in the logs.
- **Verify:** `=2` logging of "would skip" with a compare of sizes and formats; then a real run with
  the skip enabled, checking for visual regressions at area entry.
- **Signature:** unique (checked).
- **Rank:** low. Keep as a measurement first (H2).

### H4 (eboot, Entry, not yet unique): texture streaming pool placement

- **Site:** `CRenderer::UpdateTextureStreamingGPUData` 0x117930 (not unique within 64 bytes), or its
  caller `sub_117140` (call at 0x11715a).
- **Mode:** Entry on `sub_117140` (needs a longer signature or an anchor on the call site), with
  `=2` as the verify mode.
- **Change:** log the streaming pool slot or range for each texture chunk (guest address, size,
  format). This tells whether a pool slot is reused by a different texture, which is the aliasing the
  brief describes.
- **Why it helps:** it tests the pool hypothesis (section 1, the candidate) without touching the
  port's cache.
- **What could break:** nothing if only logging. Requires a longer signature first.
- **Verify:** `=2` log; cross-check the addresses against H0's per-reason counts.
- **Signature:** not unique at 64 bytes (2 matches). Not yet fixed.

## 4. Open questions

1. Which reason accounts for the roughly 5,000 image frees in `c14_base.log`? H0 answers it. Until
   then, `UnmapMemory`, `ResolveOverlap` and `ExpandImage` are candidates, and the split is unknown.
2. Does the texture streaming pool reuse guest addresses for different textures? Needs H4, or a
   trace of `ProcessTextureData` (0xfa6388) with addresses.
3. Does the game unmap and remap the same range within a frame or across frames? Needs a trace of
   `Rasterizer::UnmapMemory` with address and size. The unmap path is the main suspect.
4. Which per-frame functions create images (not buffers)? The 2.2 contexts are buffers. The image
   creators need a trace: `FindImage` with the caller, aggregated per frame.
5. What is `reused slots` (always 0 in the logs)? I did not trace it.
6. Are the callers of 0x97a50 outside `render_targets_allocate` per frame or per load? I checked the
   call graph, not each caller's loop. Callers `sub_197050`, `sub_1986a0`, `sub_1ac2d0`,
   `sub_17cac0`, `sub_1848d0`, `sub_10ec60`, `sub_96ce0` are unknown on this point.
7. Does the GPU-written image have the same content as its guest bytes after an unmap and remap?
   If not, H1 must re-upload every time, which removes most of its gain.

## 5. Summary (for the reply)

1. H0 (port counter): split image frees by reason first. Cheap, and it decides every other step.
2. H1 (port, `SOTC_UNMAP_PARK`): park images on `UnmapMemory`, re-arm on remap. Best gain if the
   gap is unmap-driven; risk is stale contents and VRAM.
3. H4 (eboot, `sub_117140`, needs a longer signature): log streaming pool slot reuse to test the
   pool aliasing hypothesis.
4. H2 (eboot, 0x95a90, unique): log RT set builds per area to confirm RTs are not rebuilt per frame.
5. H3 (eboot, 0x9aef0, unique): skip RT rebuild when sizes match. Small expected gain.
6. The RT pool itself is not a per-frame aliasing source (observed: one build per area load).
7. The one per-frame RT reinterpretation (depth/colour, about 100/s) is already handled by depth twins.
8. The unexplained churn is about 5,000 images per 80 s, not counted; the strongest candidate is the
   port's `UnmapMemory` free path.
9. Gain: unknown until H0 runs. For area entry, the 670-720 image spikes cost 9-14 ms of `alloc`
   each; the steady-state gain is unknown.
10. Risks: stale contents after remap (H1), VRAM held by parked images, and the unmap/readback crash
    history.
