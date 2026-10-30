# 06 - Allocator placement: which allocations land in which pool, and what a hook can route

Read-only. Only this file was written. Static analysis of `out/eboot.elf` (`tools/re/eboot_re.py`,
`out/re/decomp`), plus the existing reports 03 and 04 and `scripts/hooks.d/alloc.py`. No logs from
the shadow session were available in the scratchpad for this pass, so the shadow claim is taken from
the brief and checked only against code.

Labels: **observed** (disassembly or pseudocode, with address), **inferred**, **unknown**.

## 1. Answer

- The allocator family is decided by the object, not by the call. Every block request goes to
  `mem_allocator_alloc` (`0x479a0`) with the allocator object as the only owner tag. The object's
  `kind` field (`+0x18`) is what picks the pool. **Observed** (`0x19e00` reads the flag from
  `0x1a660`, `0x47a10` passes `*(obj+0x18)` as the 7th argument; see section 2.2).
- **Inferred, and it matches report 04's pool offsets:** `kind 1` requests go to the pool set at
  root+`0xa0` (its kind byte is `[pool+0x118]=1`, Onion). Every other kind (0 and 2) goes to the set
  at root+`0x140` (`[pool+0x1b8]=2`, Garlic). So Garlic holds all kind-0 allocators too, not only
  GPU kind 2. Report 04's option B is therefore already the code's behaviour; what is missing is a
  rule for kind-0 objects.
- **Observed:** the same names exist twice. `HeapAllocator`, `BucketAllocator`, `PageAllocator` and
  `Default TaggedArena` are created once with kind 1 (CPU family, `cpu_memory_family_init` `0x13380`)
  and once with kind 2 (GPU family, `memory_system_init` `0x110a0`). The shadow tool's name-based
  lookup cannot tell them apart. The brief's "all CPU kind" therefore needs a per-instance check.
- **Observed:** the kind-2 `HeapAllocator`, `BucketAllocator` and `PageAllocator` are attached to the
  `GPUMemory` family with size classes: bucket up to 0x400 bytes, heap up to 0x100000 bytes, page for
  the rest (`sub_47dd0` calls at `0x11470`, `0x11480`, `0x11492`).
- **Observed, and it explains the GPU-written data inside "CPU" names:** `Default TaggedArena` exists
  as two objects. Kind 1 (object 0x5d0 bytes, pointer `0x2dfb2c0`, accessor `sub_130f0`) and kind 2
  (object 0x320 bytes, pointer `0x2dfb2b8`, accessor `sub_130d0`). Callers of the kind-2 accessor
  include the particle double-frame init (`sub_101ee0` at `0x102173`) and `sub_a3da30`. Callers of the
  kind-1 accessor include `sub_8c370` (render named heaps, indices 3 and 6) and `sub_19aad0`.
- **Observed:** the per-frame render context `RenderContextFrameGPU` is created with kind **0**
  (`sub_8f6f0`, `0x8f6f0` region, `"RenderContextFrameGPU", 0`). By the mapping above it lands in
  Garlic, which is what the write faults show, but its name says GPU and its kind says neither. Any
  kind-based rule that moves kind-0 to Onion would move a GPU-written allocator. Do not do that.
- **Observed:** the allocation API carries no usage tag and no per-call alignment. Each call is
  `(obj, size)`. Size is rounded up to 64 KiB (`0x479a0`). Alignment is always 0x10000 (the callers
  pass it in `rdx`). The kind is passed in `rcx` for the heap and render-target paths (section 2.2).
  A `uint` "tag" word exists in the tracking helper `sub_452f0`, but its meaning is **unknown**.
- Sub-64 KiB data (constant blocks, particle records, instance data) does not get its own 64 KiB
  block. It is carved by the BPE heap (`0x48a70`-`0x4a477`), so the 16-byte headers and free-tree links
  sit in the same 64 KiB blocks as the data. This is why report 03's metadata faults share pages with
  GPU data (**observed** code structure; the page sharing itself is **inferred** from report 03).
- Hook direction, ranked in section 3: (1) extend the existing `ALLOC_BLOCK` probe to log the owner
  name, kind and forwarded `rcx`; (2) use that owner table to exempt CPU-only blocks from GPU
  tracking in the port; (3) a name-allowlisted kind-0 route to Onion, only after (1) confirms which
  kind-0 objects the GPU touches. Moving BPE header writes out of GPU pages is a large change with
  a bounded gain (section 3, item 5).

## 2. Evidence

### 2.1 Allocator objects and where they are created

Observed, `memory_system_init` `0x110a0` (`sub_110a0`):

| Object (name string) | Kind | Creation | Notes |
|---|---|---|---|
| `GPUMemory` family | 2 | `0x111e8` `sub_47b20` | family root `0x47b60` at `0x111f3` |
| `BucketAllocator` (GPU) | 2 | `0x11261`, `sub_13990` | 64 size classes, 64 KiB pages (see 2.4) |
| `HeapAllocator` (GPU) | 2 | `0x11314`, `mem_allocator_ctor` | block size 0x100000, `sub_488f0` |
| `PageAllocator` (GPU) | 2 | `0x113ed`, `mem_allocator_ctor` | takes everything above 0x100000 |
| `Default TaggedArena` (CPU) | 1 | `0x114bb`, object 0x5d0 | pointer stored at `0x115be` into `0x2dfb2c0` |
| `Default TaggedArena` (GPU) | 2 | `0x115e0`, object 0x320 | pointer stored at `0x11718` into `0x2dfb2b8` |
| `GameSingleFrame` | 0 | `0x11738` | plain, not a family |
| `Default`, `TFrameAllocators`, `STL Containers` | 0 | `0x1113b`, `0x1116e`, `0x11196` | plain |

Observed, `cpu_memory_family_init` `0x13380`:

| Object | Kind | Creation | Notes |
|---|---|---|---|
| `CPUMemory` family | 1 | `0x133c0` `sub_47b20`, root `0x133cf` | `bpe_PlatformPageAllocator_Init` (`0x18aa0`) first |
| `BucketAllocator` (CPU) | 1 | `0x13428` | same size-class loop as the GPU one |
| `HeapAllocator` (CPU) | 1 | `0x134d4`-`0x13547` | block size 0x100000, then `sub_48910` with 0x9600000 at `0x135a5` |
| `PageAllocator` (CPU) | 1 | after `0x135b1` | not fully read |

Observed, `sub_8f6f0` (render context setup, called from `AreaLoad_ShaderListAndAssets` `0x2569d0`):
- `"RenderContextCPU", 1` as a family (object +0x3c0);
- `"RenderContextGPU", 2` as a family (object +0x4a0);
- `"RenderContextFrameCPU", 0` and `"RenderContextFrameGPU", 0` as plain allocators.

Observed: `sub_47dd0(family, allocator, max)` appends the allocator to the family's array (count at
`+0x80`, entries at `+0x90`) and sets `allocator+0x20 = family`. Its only callers are the three
init functions above (`0x11470`-`0x11492`, `0x1364a`-`0x13678`, and four sites in `sub_8f6f0`).

### 2.2 The kind reaches the pool selector

Observed, `mem_allocator_alloc` `0x479a0` (disassembly): `push rbp; mov rbp,rsp; push rbx; push rax;
mov rbx,rdi; xor edi,edi; cmp byte [rbx+9],0`. It rounds `rsi` up to 64 KiB and calls
`pool_alloc_block_wrapper` `0x19ae0` with `rdi=obj, rsi=size`. It does not write `rdx` or `rcx`, so the
caller's values pass through.

Observed, `0x19ae0` (disassembly): `mov [rbp-0x40], edx` and `mov ebx, ecx`. These become the
`param_6` (alignment) and `param_7` (flag) of `pool_alloc_region` `0x19e00` (pseudocode, call at
`0x19b71`).

Observed, callers that set `rcx` as the kind: `sub_48910` (`0x4892a`) calls
`479a0(heap, size, 0x10000, *(heap+0x18))`; `sub_9aa10` does the same. So the heap refill path and
the render-target path (`render_targets_allocate` `0x9b778`) carry the owner's kind in `rcx`.

Observed, `pool_alloc_region` `0x19e00` (pseudocode): the pool set is
`lVar21 = ((param_7 != 1) + 1) * 0xa0`, so kind 1 uses `+0xa0` and anything else uses `+0x140`. The
block type byte is read from `root + 0x78 + lVar21`, that is `+0x118` or `+0x1b8`. Report 04 records
`[pool+0x118]=1` (Onion) and `[pool+0x1b8]=2` (Garlic), so the mapping is consistent with the logs.

Observed, `sub_47a10` (`mem_allocator_alloc_reuse`) passes `*(obj+0x18)` as the 7th argument to
`pool_alloc_region_aligned` `0x1a660`. That is the family path from `sub_8d770`.

Inferred: for kind 1, `0x19e00` first tries the free-run list of the root pool (its first branch
requires `param_7 == 1`) before the Onion set. Which pool that root is was not identified.

### 2.3 Who calls the allocation entry points

Observed (`eboot_re.py callers`):
- `mem_allocator_alloc` (`0x479a0`): `sub_14e80` (2 sites), `sub_15ee0` (2), `sub_17d20`, `sub_187e0`,
  `sub_48910`, `BpeHeap_Free` (`0x48db9`), `sub_49200`, `sub_9aa10`, `render_targets_allocate`.
- `sub_8d770` (family dispatch, calls `47ab0`, `47a10`, `452f0`): no static caller found (likely an
  indirect call). **Unknown.**
- `sub_452f0` (the tracker with a `uint` word, called by the family path and the heap paths): its
  bit tests on that word (`0x40000000`, shifts `>>0x18 & 0x1f`, `& 0x3f`) suggest a packed tag, but
  its exact meaning is **unknown**.
- `sub_101ee0` (`ParticleDoubleFrame`, kind 0, `0x475c0`) is called from `sub_1c50e0`, and from
  `sub_3c4fa0` (`GameCoreLoop::GameLoop`, `0x25c1fa`).
- `sub_90470` (`RequestNewRenderContext`) default size is 0x500000 when `param_2 == 0`. It builds the
  context with `sub_917c0` using the CPU family (+0x3c0) and the GPU family (+0x4a0). It also calls
  `0x86f1d0(...,0x38,0x8000000,1)`, whose meaning is **unknown**.

### 2.4 Size classes and page size

Observed, the loop at `0x11290`-`0x112af` (also `0x13440`-`0x1345f`) calls `sub_18750(bucket_i, 0x10000,
16*(i+1))` for `i` in 0..63, and `sub_17990(bucket, 0x10000, 0x10, 0x400)` at `0x112bb`. So
`BucketAllocator` serves 16..1024 bytes in 64 KiB pages.

### 2.5 Which pool each caller's data goes to (kind mapping applied)

| Data (caller) | Object and kind | Inferred pool | Written by GPU? |
|---|---|---|---|
| Render targets (`render_targets_allocate` `0x9b778`, via `0x9aef0`) | the object passed by `VideoOut_OpenSetFlipRate`, kind 2 | Garlic | yes, by design |
| Per-frame render context (`RequestNewRenderContext`) | `RenderContextFrameGPU`, kind 0 | Garlic | yes, name says so |
| Per-frame CPU context | `RenderContextFrameCPU`, kind 0 | Garlic | no (name) |
| Particle double frame (`sub_101ee0`) | `ParticleDoubleFrame`, kind 0; its GPU heap via `sub_130d0`, kind 2 | Garlic | yes (brief, report 03) |
| Render named heaps (`sub_8c370`, indices 3 and 6) | `Default TaggedArena` kind 1 (`sub_130f0`) | Onion | unknown; **the one candidate for GPU-written data in a CPU-kind pool** |
| Render named heaps (`sub_8c370`, indices 5 and 2) | `Default TaggedArena` kind 2 (`sub_130d0`) | Garlic | yes (name) |
| Readback targets (visibility, CS_DONE) | not identified | unknown | yes |
| CPU-only heaps and families (`CPUMemory`, `Default`, `TFrameAllocators`, `STL`) | kind 1 or 0 | Onion (kind 1) or Garlic (kind 0) | no |

The indices of `sub_8c370` are not yet tied to names (names come from `sub_8c880`, report 04). That
mapping is the main open item for the "CPU kind but GPU-written" puzzle.

### 2.6 Relation to reports 03 and 04

- Report 04 said CPU heaps sit in Garlic. Per 2.2, kind-1 heaps go to Onion; only kind-0 and kind-2
  heaps go to Garlic. Report 04 open question 1 (which family allocated `0x1042700010`) is now narrowed:
  it is a kind-0 or kind-2 object, unless the first-branch root path (2.2) put a kind-1 block there.
  **Inferred.**
- Report 03 said GPU-visible memory comes from the `GPUMemory` family (inferred). Observed: the kind-2
  family objects are one source, but `RenderContextFrameGPU` and `ParticleDoubleFrame` (kind 0) and
  the kind-2 `Default TaggedArena` are also GPU-written sources that are outside the `GPUMemory` family.

### 2.7 Existing probe and a doc error to fix

`scripts/hooks.d/alloc.py` declares `ALLOC_BLOCK` (wrap at `0x479a0`, 12-byte signature, unique)
and says "rdx and rcx are not read". That is true for `0x479a0` itself, but both values are forwarded
to `0x19ae0` and then to `0x19e00` (2.2). Calls from `sub_48910` and `sub_9aa10` depend on `rcx`. The
probe should record `rcx` (the kind) and `rdx` (alignment). This is a comment fix in `alloc.py` and a
probe change, not a code change in this report.

## 3. Hook proposals

Ranked by expected gain, with the honest gain estimates noted.

### 3.1 Owner table from the existing ALLOC_BLOCK probe (observer)

- **Site:** `mem_allocator_alloc`, `0x479a0`. Already declared as `ALLOC_BLOCK`, mode `wrap`.
- **Signature:** `55 48 89 e5 53 50 48 89 fb 31 ff 80`, unique (checked with `eboot_re.py sig`).
- **Change:** log, for each return value, `obj+0x10` (name), `obj+0x18` (kind), `rsi` (size), `rdx`
  (alignment) and `rcx` (forwarded kind). Store `block base -> (name, kind)` in a table the port can
  query. No behaviour change.
- **Why it helps a hitch:** nothing directly. It is the input for 3.2 and turns the kind mapping from
  inference into data.
- **What could break:** a wrap that clobbers `rcx` or `rdx`. Keep those registers unchanged.
- **How to verify:** `=2` mode (the probe runs the original too and compares the return value). Then
  count blocks per name per kind and check that the Onion blocks (`0x1000300000` range) match kind 1.
- **Gain:** 0 (enabler). Estimate: unknown.

### 3.2 Exempt CPU-only blocks from GPU tracking (port side, uses 3.1)

- **Site:** no eboot site. The port's `runtime_memory_gpu_protect` and the range list
  `SOTC_UNTRACK_RANGES` (`region_manager.h:192-330`, report 03).
- **Change:** blocks whose owner is kind 1 and which the port has never seen bound as a buffer or
  image are left unprotected and not uploaded. Blocks are added to the exempt set only after a
  bind-log check.
- **Why it helps:** the CPU-only blocks stop taking write faults (report 03: 9.8k-16.6k write faults
  per 5 s). Only the part of these faults that hits kind-1 blocks is removed. The metadata that shares
  the same pages is removed only if the whole block is exempt.
- **What could break:** if any kind-1 block is bound by the GPU, the GPU reads stale or unprotected
  data. The per-range bind counter (3.1 plus the existing bind log) must show zero binds into exempt
  ranges before any exemption is enabled. The Default TaggedArena kind-1 candidate in 2.5 must be
  checked first.
- **How to verify:** `=2` mode: exempt ranges are tracked as before, and a counter counts writes that
  would have been skipped. Compare the checksum of exempt ranges at each bind (the same method as
  `SOTC_HOST_IMPORT`).
- **Gain:** unknown. Upper bound is the share of write faults that land in kind-1 blocks; the trace
  in report 03 does not attribute faults to pools. Estimate: unknown.

### 3.3 Kind-0 route to Onion, by name allowlist (site hook, risky)

- **Site:** `pool_alloc_region`, entry at `0x19e00`. Signature `55 48 89 e5 41 57 41 56 41 55 41 54 53 48
  83 ec 58 48 8b 05 ?? ?? ?? ?? 44 89 cb 48`, unique (checked). Read the owner name from `obj+0x10`
  (in the `0x19ae0` path `rdi` is the object) and the flag from the 7th argument at `[rsp+8]`.
  Change the `lVar21` choice (`((param_7 != 1) + 1) * 0xa0`) for an allowlisted name.
- **Why it helps:** moves CPU-only kind-0 allocators (`Default`, `TFrameAllocators`, `STL Containers`,
  `GameSingleFrame`) out of Garlic into Onion, so they leave the GPU-tracked pool.
- **What could break:** `RenderContextFrameGPU` and `ParticleDoubleFrame` are kind 0 and GPU-written
  (2.5). The allowlist must exclude them. Onion is 1 GiB; if it fills, the allocation fails and the
  game must handle `NO_MEMORY`. The root-pool first branch (2.2) is not yet understood.
- **How to verify:** `=2` mode with the allocation made and the old pool result logged. Check that
  heap-guard lines (report 04, log 154) disappear for those heaps and that no kind-0 GPU-written
  allocator is moved (3.1 log).
- **Gain:** unknown. Depends on how many write faults come from kind-0 CPU-only heaps. Estimate:
  unknown; lower than 3.2 because it changes game memory placement.

### 3.4 Keep BPE metadata off GPU pages (larger change, bounded gain)

- **Site:** the BPE heap code range `0x48a70`-`0x4a477` (already a `range` probe, `BPE_HEAP_CODE`,
  in `alloc.py`), plus the refill calls `479a0` from `sub_49200` and `BpeHeap_Free` (`0x48db9`).
- **Change (design, not one hook):** place the 16-byte block headers and free-tree links in the
  untracked backing view (`runtime_memory_backing_view`, report 04) instead of the guest address, so
  metadata writes do not fault. This means redirecting many stores in 20 functions, or moving the
  metadata to a separate untracked 4 KiB page per 64 KiB block.
- **Why it helps:** the metadata share is 18% of sampled write faults (report 03, 17,903 of 97,229),
  and the slowest per fault (0.4-0.6 ms).
- **What could break:** heap corruption if any store is missed. The heap is shared by CPU and GPU data
  in the same block. A missed store is a silent corruption, not a crash.
- **How to verify:** the existing `BPE_HEAP_CODE` range counts writes per function. After a change,
  the count of faults inside `0x48a70`-`0x4a477` must drop to near zero, and a heap walk checksum must
  match a `=2` shadow run.
- **Gain:** up to about 18% of write faults and most of the slowest-per-fault cost. Estimate: upper
  bound only; the real gain depends on how many metadata pages are also GPU-tracked.

### 3.5 Observer only: readback allocation site (no hook yet)

- The allocation of readback targets (visibility, CS_DONE) is **unknown**. Report 04 places the
  visibility buffer in Garlic and flexible memory. The first step is an entry log at `0x19e00` for
  requests whose size matches the visibility buffer. No change to behaviour.

## 4. Open questions

1. **Per-instance kind of the duplicated names.** The shadow session saw `HeapAllocator`,
   `BucketAllocator` and `Default TaggedArena` as CPU kind. Both kinds exist (2.1). Which instance
   held the GPU-written pages? Test: 3.1 log with `(name, kind, base)`.
2. **Named heaps on the CPU arena.** Which of `sub_8c370`'s heap indices (3, 6 on the kind-1 arena;
   5, 2 on the kind-2 arena) are the named heaps from `sub_8c880`? If indices 3 or 6 are GPU-visible,
   that is the "CPU kind with GPU writes" source. Test: string table of `sub_1cfb0` arguments.
3. **Root-pool first branch.** For kind 1, `0x19e00` tries a free-run of the root pool first (2.2).
   Which pool is the root (`DAT_02dfca78`, offset 0)? Test: read its type byte in a log.
4. **Kind-1 heap in Garlic.** Report 04 logged a heap at `0x124560fd20` with its guard in Garlic. Is
   that heap kind 0 or 2 (expected under 2.2), or did the root-pool branch give it a kind-1 block?
   Test: its `+0x18` in the log.
5. **`sub_8d770` callers.** The family dispatch has no static caller; it is probably called through a
   vtable. Which vtable slot, and which families use it?
6. **`sub_452f0` tag word.** What is the packed `uint` (the caller's value at `[rsi]` or the constant
   `0xffffffff`)? Test: log its value at the `0x14f82` call.
7. **`RenderSingleFrameGPU` size.** Still not located (report 04 open question 6).
8. **Readback allocator.** Which object backs the GPU-to-CPU visibility and CS_DONE buffers? Not in
   the callers listed here.
9. **Doc correction.** `alloc.py` says `rcx` is not read. It is forwarded and read by `0x19ae0`
   (2.7). Fix in the probe, not in this report.
10. **Heap refill frequency.** How often does `sub_49200` or `BpeHeap_Free` call `479a0` during play?
    Test: count `ALLOC_BLOCK` per second with the existing probe.

## Summary (ranked by expected gain)

1. Extend `ALLOC_BLOCK` (`0x479a0`, wrap, unique 12-byte signature) to record owner name, kind,
   alignment and forwarded `rcx`. Enabler; no gain itself.
2. Port-side exemption of kind-1 (CPU-only) blocks from GPU tracking, after a zero-bind check. Gain
   unknown; upper bound is the kind-1 share of write faults.
3. Record the per-instance kind of the duplicated `HeapAllocator`, `BucketAllocator` and
   `Default TaggedArena` names, to settle the brief's "CPU kind" claim (open question 1).
4. Keep `RenderContextFrameGPU`, `ParticleDoubleFrame` and kind-2 arenas out of any kind-0 route.
5. Name-allowlisted kind-0 route to Onion at `pool_alloc_region` (`0x19e00`, unique 28-byte
   signature). Gain unknown; risk of moving GPU-written data.
6. Identify the named heaps in `sub_8c370` (indices 3, 6 on kind 1; 5, 2 on kind 2).
7. Readback allocation site: entry log at `0x19e00` for visibility-sized requests (observer).
8. BPE metadata off GPU pages (up to about 18% of write faults; large change; silent-corruption risk).
9. Correct the `alloc.py` docstring about `rcx`.
10. Do not route by kind alone: kind 0 includes GPU-written allocators.
