# Topic 04: engine memory model and a PC (discrete GPU) memory model

## 1. Answer

- The PS4 direct memory is 9152 MiB (observed, log line 63). The engine splits it into four blocks:
  Onion 1 GiB (PS4 type 0), Garlic 7.93 GiB (type 3), a 2.1 MiB page bitmap (type 0, at the top of
  the space) and a separate 406 MiB flexible pool at guest 0x123c000000. The engine labels the two
  big pools "Onion Memory" and "Garlic Memory" (strings only, consumer not located).
- Two allocator families exist: `CPUMemory` (kind 1) and `GPUMemory` (kind 2). Their names and kinds
  are set in the allocator constructor (observed, `sub_475c0`).
- The PS4 type is not a safe classifier. A BPE heap arena sits in Garlic (guard range
  `0x1042700010..0x108baaeac8`, observed in log line 154). The heap guard exists because GPU
  readbacks wrote into free CPU heap blocks (CHANGELOG/bpe_heap_guard.h). CPU-only data is therefore
  inside the GPU-visible pool.
- The port treats the whole pool as possibly GPU-used. Page protection on that pool costs 11-13k
  guest write faults per 5 s (MODELO-MEMORIA-PC.md, measured). Every block free or alloc of GPU-kind
  memory goes through `sceKernelBatchMap` (inferred per frame, unverified).
- PC model: four classes (GPU-only, CPU-to-GPU streaming, GPU-to-CPU readback, CPU-only). CPU-only
  ranges (Onion, flexible, bitmap) need no GPU mirror and no tracking. GPU-only render targets need
  no CPU protection. The frame heap becomes an upload ring and readbacks go to a host-cached buffer.
- Next step is classification with no behaviour change: log the allocator name, kind, size and
  returned address at the block allocator entry. That settles which kind reaches Garlic.

## 2. Evidence

### Pools (init, `src/`-independent: eboot code plus the user's log)

| Block | Guest VA | Size | PS4 type | Created by | Notes |
|---|---|---|---|---|---|
| Sys block | `0x7f0000000` (phys 0) | 128 KiB | 3 (garlic) | `sub_18aa0` at `0x18b6d` | purpose unknown |
| Onion | `0x1000300000` (phys `0x100000`) | 1 GiB | 0 (onion) | `sub_18aa0` at `0x1941b` | pool kind byte 1 (`[pool+0x118]=1`) |
| Garlic | `0x1040300000` (phys `0x40100000`) | 8519548928 B (7.93 GiB) | 3 | `sub_18aa0` at `0x19513` | pool kind byte 2 (`[pool+0x1b8]=2`) |
| Bitmap | `0x1000000000` (phys `0x23bde0000`) | 2.125 MiB | 0 | `sub_18aa0` at `0x18c5c` | 64 KiB page bitmap, zeroed by `memset` |
| Flexible | `0x123c000000` | 425984000 B (6500 x 64 KiB) | flexible | `sub_197d0` | `sceKernelReserveVirtualRange` then `sceKernelMapFlexibleMemory`, size capped at `0x1964` pages |

The type numbers follow the PS4 SDK naming (0 = write-back onion, 3 = write-combined garlic). That
mapping is from memory of the SDK, not from the repo: unverified. The log labels match the use.

Engine allocator wiring (observed):
- `sub_110a0` (memory system init): creates `PreInit`, `Default`, `TFrameAllocators`, `GPUMemory`,
  `BucketAllocator`, `HeapAllocator`, `PageAllocator`, `GameSingleFrame`, `GameDoubleFrame`. It calls
  `sub_13380` first.
- `sub_13380`: `CPUMemory` family (kind 1) after `sub_18aa0` (PlatformPageAllocator::Init).
- `sub_110a0` at `0x111e8`: `GPUMemory` family (kind 2), via `sub_47b20`.
- Constructor `sub_475c0(obj, name, kind)`: `[obj+0x10]=name`, `[obj+0x18]=kind`, `[obj+9]=1`.
- Allocation chain (observed from call sites): `sub_479a0(obj, size, align, flags)` ->
  `sub_19ae0` -> `sub_19e00` (block allocator, pool selection by kind) -> `sub_1abf0`
  (`sceKernelVirtualQuery`, `sceKernelReserveVirtualRange`) -> `sub_1af80` (`sceKernelBatchMap`).
  Free path: `sub_1b0e0` -> `sub_1abf0` / `sub_1af80` (unmap by batch).
- `sub_47620` (frees every block of a family object, calls `sub_1b0e0` for each): reached from
  `sub_12040` (destroy) and from several small wrappers. Whether it runs every frame is unverified.
- `sub_1a800`: walks a free-run list per pool struct; nodes carry a type byte (`+0x68`) copied from
  the pool kind. This is how kind 1 and kind 2 regions are kept apart.

Users of GPU-kind memory (observed):
- `sub_9aef0` (render targets; strings `kRTT_*`, `DisplayBuffer`, `InterFrame`) calls
  `sub_479a0` with the allocator's kind. Block size is `0x14280000` (322 MiB) or `0x23d10000`
  (573 MiB), chosen by a flag in `r12d` (probably Pro mode; unverified).
- `sub_8c880` registers named heaps with kind 0 (static init): `RenderSingleFrameGPU`,
  `RenderSingleFrameCPU`, `GameToGPU`, `CPU_RenderToGPUToGame`, `XPRTempData`, `MaterialParamHeap`,
  `VirtualMemoryGPU`. Their sizes are set elsewhere (not found). The "32 MiB chunk" in BRIEF is not
  confirmed here.
- Kind 0 objects are not kind 1, so they take the non-CPU branch in `sub_19e00` (inferred).

Log lines (user copy, `<install>\user\last_run.log`):
- 59-73: direct allocations at phys `0x0` (128 KiB, type 3), `0x23bde0000` (2.125 MiB, type 0),
  `0x100000` (1 GiB, type 0), `0x40100000` (8519548928 B, type 3); flexible `425984000` at
  `0x123c000000`.
- 145-146: BPE heap guard on heap `0x1b00c700` (2000 frees); heap fields `+0x28 = 0x123c000020`
  (flexible, 150 MiB arena), `+0x30 = 0x9600000`.
- 154: `SOTC heap guard: heap 0x124560fd20 guarded in 0x1042700010..0x108baaeac8` (Garlic).
- `reserved virtual range` at `0x1255700000...` (just above flexible): streaming reserves beyond
  the flexible pool, mapped later.

Culling and readbacks (MODELO-MEMORIA-PC.md, 27/10, measured): the visibility buffer sits at
`0x1078c1...` and `0x109f83...` (inside Garlic) and `0x1245620000` (inside flexible). Readbacks per
stall line average 52.7 (43 lines, dev logs `user/last_run*.log`).

Port side:
- `src/runtime_memory.c`: `blocks[]` keeps PS4 type per physical block; `direct_memory_type`
  (`sceKernelGetDirectMemoryType`) returns it. `batch_map2` ops: 0 map direct, 1 unmap, 2 protect,
  3 map flexible, 4 protect with type. Every change calls `queue_hook` (`HOOK_MAP`, `HOOK_UNMAP`,
  `HOOK_INVALIDATE`) into the GPU.
- The whole pool is one pagefile section (`CreateFileMapping2`, `runtime_memory.c` `pool()`), committed
  at once (9152 MiB by default). The GPU reads a second unprotected view (`runtime_memory_backing_view`).
- `runtime_memory_gpu_protect` write-protects host views of guest ranges the GPU uses.
- `gpu/shadps4/video_core/buffer_cache/host_import.*` (SOTC_HOST_IMPORT) and
  `renderer_vulkan/sotc_guest_memory.*` (SOTC_PC_MEMORY) import that backing view in 64 MiB chunks.

## 3. Stutter mechanism

Observed: the port tracks GPU-used memory by protection. Garlic is 7.93 GiB, and CPU heaps live in
it. Each CPU write to a protected page faults, and each fault takes the table lock. Readbacks then
write GPU output into protected pages and run `ApplyDownloads` plus heap-guard walks
(`driver.cpp:1571`). These are the fault, readback and guard costs in the MODELO numbers (DMA 83-127
ms, sends 70-106 ms, read waits 47-59 ms per 5 s).

Inferred: each block free or alloc of GPU-kind memory is a `BatchMap` (map or unmap) that changes
the VMA table, fires the GPU hooks and invalidates caches. Area changes allocate many blocks at
once, so the writer waits. The XPR cull is a same-frame GPU-to-CPU round trip, so the draw thread
waits on the cull result.

## 4. Hook and redesign options

A. Classify (no behaviour change; low effort; no risk). Hook `sub_479a0` entry (and `sub_1b0e0`
entry). Log `[obj+0x10]` (name), `[obj+0x18]` (kind), size, align, returned address. Also count
`sub_1af80` calls per second. Verifies: which families reach Garlic, whether CPU heaps are in Garlic,
and whether GPU blocks are freed every frame. Measure with `BB_MEMORY_LOG=1` (reserved ranges).

B. Kind-aware pool choice. In `sub_19e00`, send kind-1 (CPU) requests to Onion and keep kind-2 in
Garlic. Expected: CPU heaps leave Garlic and stop being GPU-tracked. The heap guard (`bpe_heap_guard.*`)
may then be unnecessary. Risk: Onion is 1 GiB, so a CPU family that outgrows it fails; the game
must handle `NO_MEMORY`. Verify: the heap-guard log lines disappear, and the final stage of the game
runs without fault.

C. Track by class, not by pool. Render-target blocks (`sub_9aef0`) are GPU-only: skip CPU protection
for them, keep only the GPU mapping. Onion and flexible ranges are CPU-only: remove them from GPU
tracking once the GPU's referenced ranges are logged to show none fall there (verify with the buffer
cache's bound ranges by pool). Expected: most of the 11-13k faults per 5 s disappear.

D. Frame heap as an upload ring. The per-frame GPU blocks (kind 2, `GameSingleFrame`,
`RenderSingleFrameGPU`) become host-visible ring memory. The port copies used bytes at submit time
(host-visible `HostUncached`/`Stream`), with no page protection. Reuse is fenced by the frame
pipeline (`GameDoubleFrame` suggests two frames; unverified). Risk: CPU writes after submit into a
block that is reused. Verify: `SOTC_HOST_IMPORT`-style checksum compare in the capture demo.

E. Readbacks to HostCached buffers. Visibility and CS_DONE data are copied from device-local to a
host-cached buffer at fence time, not on a CPU fault. The guest copy happens at the game's wait.
This is the same path as the current readbacks minus the protection. Radical variant: one-frame
latency for the cull (visible objects appear one frame late). That changes game behaviour: test it
behind a switch.

F. Radical: the GPU never writes guest memory. Only the readback buffer (E) does. All other GPU
output stays device-local and is exposed to the CPU only when the game reads it. This removes the
need for the heap guard and most fault tracking.

Relation to the existing plan (`docs/sotc/MODELO-MEMORIA-PC.md`): stage 1 (import the backing view)
is still right for the upload path, since import is read-only for the GPU. The plan's stage 2 limit
("write directly only outside guarded heaps") is a symptom of CPU heaps inside Garlic. Option B
removes that limit at the source and makes stage 2 unneeded for those heaps. Stage 4 (visibility
written directly to RAM) is option E. SOTC_HOST_IMPORT covers only uploads; it does not change
readbacks or protection.

What the port's page tracking would no longer need to cover (if A-C hold): the Onion 1 GiB, the
flexible 406 MiB, the 2 MiB bitmap, all render-target blocks, and the heap-guard ranges. What stays:
textures and dynamic buffers that the CPU writes and the GPU reads (candidates for explicit dirty
marking from the resource loader or the frame heap).

## 5. Proposed names

- `0x18aa0 bpe_PlatformPageAllocator_Init` (builds the four blocks above)
- `0x13380 cpu_memory_family_init` (kind 1)
- `0x110a0 memory_system_init` (allocator sets, GameSingleFrame, GPUMemory)
- `0x197d0 pool_init_flexible` (406 MiB flexible pool)
- `0x19660 pool_node_alloc` (free-run node from the node pool)
- `0x19e00 pool_alloc_region(pool, low, high, size, kind, align, flags)` (main block allocator)
- `0x19ae0 pool_alloc_block_wrapper`
- `0x19c60 pool_alloc_region_simple`
- `0x1a800 pool_take_free_run`
- `0x1a660 pool_alloc_region_aligned`
- `0x1abf0 vmem_query_reserve_map`
- `0x1af80 vmem_batch_map`
- `0x1b0e0 pool_free_block`
- `0x475c0 mem_allocator_ctor(obj, name, kind)`
- `0x47b20 mem_family_ctor(obj, name, kind)` (vtable `0x15959c0`)
- `0x47b60 mem_family_set_root`
- `0x479a0 mem_allocator_alloc(obj, size, align, flags)`
- `0x47a10 mem_allocator_alloc_reuse`
- `0x47620 mem_allocator_free_all`
- `0x12040 mem_allocator_destroy`
- `0x9aef0 render_targets_allocate` (`kRTT_*`)
- `0x8c880 render_context_register_heaps` (static init of named heaps)

## 6. Open questions

1. Which family allocated the arena at `0x1042700010` (heap `0x124560fd20`)? Test: option A logging.
   If it is kind 1, the Onion pool is not being used for CPU requests (see `sub_1a800`).
2. Does `sub_47620` (free-all) run once per frame for GPU families? Test: count `sub_1b0e0` per
   second during gameplay (option A).
3. Are the PS4 type numbers 0 = WB onion and 3 = WC garlic? Unverified; the SDK header is not in
   the repo.
4. What is the 128 KiB block at `0x7f0000000` for? Consumer not found.
5. Which pool holds CS_DONE labels and the LOD feedback buffer? Not located.
6. The `RenderSingleFrameGPU` size (BRIEF says 32 MiB) is not found in code.
7. Is `r12d` in `sub_9aef0` the Pro flag? The render-target size changes with it.
8. Other topic, noted only: the same-frame cull (E) is a GPU-sync question; the frame pipeline
   belongs to the thread topic.
