# 03 - Who writes GPU-visible memory from the CPU, per frame

Read-only. No project file edited. Ghidra output (`out/re/decomp`) does not exist, so all reading is disassembly plus traces. Helper scripts (scratch, reproducible): `scratchpad/re/faults_agg.py`, `scratchpad/re/faults_by_func.py` (they import `tools/re/eboot_re.py` and read `user/trace/*/*.json`).

## 1. Answer

- Sampled data: 157 slow-frame trace windows in `user/trace/` hold 97,229 write faults and 20,286 read faults. The 5 s counters in `user/last_run.log` show 9,769 to 16,600 write faults per 5 s. The trace is biased to slow frames, so read the shares as shares of slow frames.
- About 47% of write faults (46,141) happen inside host code, not in eboot code. About 42,000 of them sit in one host module at `0x7ffda260xxxx`. They are almost certainly the host `memcpy`/`memset`/`memmove` that the game calls through imports (inferred, see section 3).
- About 18% of write faults (17,903) and about 23% of read faults (~4,700) are in the BPE heap's own allocator code (eboot `0x48a70`-`0x4a477`). These are block headers and free-tree links that share 4 KiB pages with GPU-bound data. They are also the slowest per fault in the sample (0.4-0.6 ms average, observed trace durations).
- Named per-frame writers account for about 15% of write faults: particle record shifts (`sub_101720`, 8.7%), view constants (`SetViewParams` `sub_be1f0`, and the `EndFrameViewParams` callees), instance and skinning data (`sub_118100` -> `sub_123410`), particle `BeginFrame` (`sub_107540`).
- Inferred: GPU-visible memory comes from the `GPUMemory` memory type set up in `sub_110a0` (bucket, 1 MiB heap, page allocator). `RenderSingleFrameGPU` is only one frame allocator. That likely explains the null result of untracking it: the writes land in other heap pieces.
- The red-zone addresses are not writers in the traces: `0xEDFB30` starts `sub_edfb30`, `0xF52530` starts `sub_f52530`, and `0xF67D2B` is a load (`mov edx, [r11+4]`) in `sub_f67990`. None of the three shows up in the sampled faults.
- Direction: attribute the memcpy class first (one counter in `src/runtime.c`), make those copies declare their destination span, then move per-frame writer destinations into untracked, explicitly uploaded arenas. The radical option is MODELO-MEMORIA-PC stage 5.

## 2. Evidence

### 2.1 Allocators (observed unless marked)

| Allocator / object | Where | Notes |
|---|---|---|
| Memory init, PreInit | `sub_110a0` (2,739 bytes) | Creates `Default`, `TFrameAllocators`, `STL Containers`, `GPUMemory`, `GameSingleFrame` (0x2e0 object), `GameDoubleFrame`, `Default TaggedArena` (two), `HeapAllocator`, `PageAllocator`, `BucketAllocator` |
| `GPUMemory` memory type | `sub_110a0`: `sub_47b20(obj, "GPUMemory", 2)`; then `sub_47dd0(type, bucket, 0x400)`, `sub_47dd0(type, heap, 0x100000)`, `sub_47dd0(type, page, -1)` | Inferred routing by size: bucket up to 1 KiB, heap up to 1 MiB, page for the rest. CHANGELOG and MODELO-MEMORIA-PC describe a 64 KiB page pool, which fits |
| Frame allocators | `sub_110a0` strings `GameSingleFrame`, `GameDoubleFrame`, `TFrameAllocators`; `FrameAllocateJanitor` in `sub_335d0` | Which writers use them: unknown |
| `RenderSingleFrameGPU` / `...CPU` (32 MiB chunks) | `sub_8c880` (string use) | Freed chunks drew zero faults in the earlier `SOTC_UNTRACK_AUTO` run (CHANGELOG) |
| `RenderContextFrameGPU` / `...CPU` | `sub_8f6f0` | Observed strings |
| `PerFrameRenderContexts::RequestNewRenderContext(size_t, size_t)` | `sub_90470` (PS4CRenderBackend.cpp) | Two sizes, probably command buffer plus data (inferred). `RequestNewComputeContext` is `sub_906d0` |
| `ParticleDoubleFrame` | `sub_101ee0` | Observed string |
| Heap allocator (BPE) | `sub_48a70`, `sub_48c80`, `sub_493d0`, `sub_497f0`, `sub_49030`, `sub_495e0`, `sub_4a040`, `sub_4a210` | `gpu/shadps4/video_core/buffer_cache/bpe_heap_guard.h`: 16-byte header before each block, tree links in free blocks. Its notes cite eboot `0x48ac5`, `0x48f3d`, `0x490a0`, `0x49476`, all inside these functions |

### 2.2 Writers, by sampled write faults (eboot function or host module)

Share = of 97,229 write faults. Per second = share times the 5 s counter (2.0k-3.3k/s).

| Writer | Data kind | Write faults (share) | Notes |
|---|---|---|---|
| Host `memcpy`/`memset`/`memmove` called by the game | bulk copies, probably constants, vertices, instances (inferred) | 46,141 (47%); ~42,000 in `0x7ffda260xxxx` | `guest_memcpy` etc. at `src/runtime.c:170-176`; the game has 1,231 memcpy, 618 memset and 551 memmove call sites (`icallers`). Host instruction only, so the caller is not in the trace |
| BPE heap metadata: `sub_48c80` 5,947; `sub_497f0` 3,563; `sub_48a70` 2,889; `sub_493d0` 2,790; `sub_4a210` 1,112; `sub_4a040` 1,108; `sub_495e0` 494 | headers, free-tree links | 17,903 (18%) | Average per fault in the sample: 0.36 ms (`sub_48c80`), 0.60 ms (`sub_48a70`) |
| `sub_101720` (called by `sub_103ad0` and `sub_1051f0`, both `ParticleRenderer::UpdateSimulation`) | particle records: 96-byte in-place shift in a 192-byte record, plus a 32-bit index write (inferred) | 8,470 (8.7%) | Write site `eboot+0x101817`, 1.08 s total in the sample |
| `sub_1018c0` `ParticleSystemPool::ProcessStats` | particle stats array (CHANGELOG: `pool+0x188`) | 2,432 (2.5%) | Also 3,699 read faults (readback) |
| `sub_179c0` (+ `sub_17e90`) | unknown (counts, linked pointers) | 5,443 (5.6%) | No string anchor; `sub_179b0` is an 8-byte thunk |
| `sub_1d750`, `sub_a3fc30` (caller `sub_a40910`) | unknown | 3,473 (3.6%) | No string anchor |
| `sub_123330`, `sub_123410` (caller `sub_118100`) | instance / skinning stream | 1,245 (1.3%) | `sub_118100` strings "Update GPU Data", "InstanceData"; caller `sub_cfdb0` ("UpdateSkinningRenderObjects") |
| `sub_11c710` "Update InstanceDynamicData" (caller `sub_ca6d0`) | dynamic instance data | 742 (0.8%) | `sub_ca6d0` strings "XPRPreRender", "RenderRainBlockers" |
| `sub_be1f0` "SetViewParams" (callers `sub_c6810`, `sub_ca6d0`, `sub_caed0`) | per-view constant block | 960 (1.0%) | `sub_caed0`: VirtualGeometry/Forward/Fur PreRenderJob |
| `sub_8b7f0`, `sub_cedf0` (callers `sub_cefa0`, `sub_ceb10`) | per-frame view constants | 1,171 (1.2%) | `sub_cefa0` = "EndFrameViewParams" |
| `sub_107540` `ParticleRenderer::BeginFrame` (caller `sub_d0040`) | particle per-frame init | 628 (0.6%) | |
| `sub_109ca0` (callers `sub_109ab0`, `sub_10a500`, `sub_10a5e0`) | particle draw data | 488 (0.5%) | RenderTranslucent |
| `sub_18ac10`, `sub_198bb0`, `sub_b3590`, `sub_bae70`, `sub_95320`, `sub_b50c0` | unknown; `ComputeExposure`, `CreateDepthTextureSet` named | ~2,500 together (2.6%) | |

Read faults (20,286 in the sample): `sub_1a6d20` (XPR visibility cull) 7,425 (12.6 s total in the sample); `sub_1018c0` 3,699; heap walkers `sub_48a70` 1,393, `sub_493d0` 1,278, `sub_495e0` 1,061, `sub_49030` 690, `sub_497f0` 303. The readback side belongs to topic 01/02.

### 2.3 Per-frame volume (5 s counters, `user/last_run.log`)

- `SOTC OP PHASES /s`: `dirty_syncs 1991.7`, `upload_kb 52802.8`, `protects 4173.2`, `pages 130749.2`. So about 53 MB/s is uploaded from guest memory, and about 131k pages/s are re-armed.
- `SOTC FAULTS`: 9.8k-16.6k write faults per 5 s; `ahead pages` 1.5k-4.6k; `granule skipped` 73k-114k per 5 s (pages marked by the granule that the guest did not write, dropped at upload).
- `SOTC SYNC PIPE`: 120,052 protect calls per 5 s, 94,913 operations with protects.
- Avg fault latency is `0.0 us` in these logs (`SOTC_FPS` timing off). Only trace durations give latency.

### 2.4 Port side (fault path)

- `gpu/shadps4/video_core/buffer_cache/memory_tracker.h:257` calls `NoteGuestWriteFaultLocked`; `region_manager.h:449` handles hot pages and fault grace.
- `buffer_cache.h:185-200`: write-ahead of 15 pages on every fault (`SOTC_WRITE_AHEAD_PAGES`), plus `SOTC_WRITE_AHEAD_ADAPT`.
- `buffer_cache.h:688`, `buffer_cache.cpp:6414`: `SOTC_FAULT_GRANULE` snapshots.
- `page_manager.h`: `RegisterGuestStack`. Guest stacks are never protected.
- `region_manager.h:192-330`: `SOTC_UNTRACK_RANGES` already implements "never protected, re-sent at every read bind". This is the building block for an explicit-upload arena.
- Host writes into guest memory: the PM4 packets are written by host code in `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:592` (`sceGnmDrawIndexAuto` and similar). The game imports those calls, so the DCB is not written by eboot code. Those faults would show up at `0x7ff...` addresses outside the CRT block (about 4.4k in the sample, not attributed).

## 3. Stutter mechanism

Observed: each guest write to a protected 4 KiB page raises an exception, is handled, the page is unprotected and write-ahead marks 15 more, and at the next sync the draw thread uploads and re-protects them. 131k pages/s and about 53 MB/s are cycled per second, on the serial draw thread (log: `bind_resources 357.8 ms/s`).

Observed: heap metadata faults are the slowest per fault (0.4-0.6 ms average in the sample), probably waiting on the region mutex (inferred), so they stall the game thread in bursts.

Inferred: the 47% memcpy class is bulk writes into tracked pages. Each `memcpy` touching N protected pages costs N faults, and granule and write-ahead logic only spread them out. This is consistent with the flat fault count in the `SOTC_FAULT_GRANULE` and `SOTC_WRITE_AHEAD_ADAPT` experiments.

Inferred: GPU data and allocator metadata share 4 KiB pages inside the same 1 MiB heap pieces, so faults depend on how often the CPU touches a page, not on how many bytes the GPU reads. Page-size tuning cannot remove them.

## 4. Hook / redesign options

1. **Attribute the memcpy class (no behaviour change; cheapest).** In `guest_memcpy`/`guest_memset`/`guest_memmove` (`src/runtime.c:170-176`), take `__builtin_return_address(0)` (the eboot return address), and when the destination is in a tracked range (`PageManager` query), count calls and bytes per return address and print them with `SOTC FAULTS`. Check that the sum matches the write-fault counter.
2. **Declare bulk writes (small to medium; likely the biggest win).** Inside the same three wrappers, before the copy, pre-mark the exact destination span as CPU-modified and unprotect it once (one `VirtualProtect`), copy, then re-arm. The span is known exactly, so faults go to zero for that class instead of one per page. Risk: concurrent writers and the region lock; only a byte-exact span is safe. Unmeasured.
3. **Heap metadata out of GPU pages (medium).** `BpeHeapGuard` already finds metadata spans (`HasMetadata`, `CollectSpans`). Options: (a) patch the allocator (`sub_48c80`, `sub_497f0`) to keep headers in a side table in untracked pages, the same way `SOTC_HOOK_PSTATS` patches an instruction; (b) keep blocks that hold GPU data out of heap pages that carry metadata. Risk: block layout, the heap guard, and every allocation. Expected: about 18% of write faults, about 23% of read faults, and the 0.4-0.6 ms per fault.
4. **Per-frame writers into untracked, explicitly uploaded arenas (medium).** For `sub_101720`, `sub_be1f0`, `sub_8b7f0`/`sub_cedf0`, `sub_123410`, `sub_11c710`, `sub_107540`: find their destination arena from the trace `a` field (group write faults by 1 MiB heap piece), then mark those pieces untracked (`SOTC_UNTRACK_RANGES`) and copy only the written prefix at submit. Cost: the whole used prefix every frame; measure the MB/s. Note: `RenderSingleFrameGPU` untrack did not help, so the destinations are probably in the world heap pieces, not the frame heap.
5. **Radical: host-visible GPU memory as guest memory (stage 5 of MODELO-MEMORIA-PC).** Back GPU-visible allocations with host-visible Vulkan memory mapped into guest VA (Win32 section plus `VK_KHR_external_memory_win32`, or `VK_EXT_external_memory_host` on a `MapViewOfFile` view). The CPU writes straight into GPU-visible memory: no protection, no faults, no uploads, coherence at submit. Risks: write-combined reads are slow (the game reads back some of this data), driver support, and the heap metadata problem stays unless it is also moved. Stage 1 (`sotc_guest_memory`) already imports the guest memory, so the first step is cheap.

Recommended order: 1, then 2, then measure the drop in `SOTC FAULTS`. Then 4 and 3, with 5 only if 2-4 do not remove the fault storm.

## 5. Proposed names

- `0x1018c0` `ParticleSystemPool::ProcessStats`, `0x107540` `ParticleRenderer::BeginFrame`, `0x103ad0` / `0x1051f0` `ParticleRenderer::UpdateSimulation` (string-confirmed)
- `0x101720` `ParticleRenderer_ShiftRecords`, `0x109ca0` `ParticleRenderer_TranslucentHelper` (inferred)
- `0xbe1f0` `SetViewParams` (string-confirmed), `0xcefa0` `EndFrameViewParams` (string-confirmed)
- `0x8b7f0`, `0xcedf0` `EndFrameView_WriteA`, `EndFrameView_WriteB` (inferred)
- `0xcfdb0` `UpdateSkinningRenderObjects`, `0x118100` `UpdateGPUData_InstanceData`, `0x11c710` `InstanceDynamicData_Update` (string-confirmed)
- `0x123410`, `0x123330` `InstanceData_WriteStream`, `_b` (inferred)
- `0x110a0` `Memory_InitAllocators` (observed); `0x8c880` `RenderSingleFrameGPU_Init` (role inferred); `0x90470` `PerFrameRenderContexts::RequestNewRenderContext` (string-confirmed)
- `0x48a70`, `0x48c80`, `0x493d0`, `0x497f0`, `0x49030` `BpeHeap_Walk`, `BpeHeap_Free`, `BpeHeap_Link`, `BpeHeap_Alloc`, `BpeHeap_Find` (inferred)
- `0xedfb30`, `0xf67990`, `0xf52530`: red-zone functions `sub_edfb30`, `sub_f67990`, `sub_f52530` (roles unknown; not in the fault sample)

## 6. Open questions

- Which memcpy/memset call sites write tracked memory? Settled by option 1.
- Is `0x7ffda260xxxx` the CRT `memcpy` (inferred from `runtime.c:173`, no module list)? Settle with a module list from a run, or with option 1.
- Which allocation calls pass memory type 2 (`GPUMemory`), and which vtable slot allocates? Trace the `sub_47b20` object's vtable and its callers.
- Data kind of `sub_179c0`, `sub_1d750`, `sub_a3fc30`, `sub_8b7f0`: no string anchors. Needs a destination histogram or the Ghidra export.
- Are DCB pages tracked? If yes, the non-CRT host faults (about 4.4k) are the gnmdriver `WritePacket` writer.
- The sample is slow frames only. Weighting by all frames needs `SOTC_TRACE` on normal windows or per-writer counters (not logged today).
- Particle record size (192 bytes) and index array are inferred from the `sub_101720` loop; confirm against the `ParticleSystemPool` layout.
- Out of scope: the XPR cull `sub_1a6d20` dominates read faults (7,425; 12.6 s in the sample), for topics 01/02.
