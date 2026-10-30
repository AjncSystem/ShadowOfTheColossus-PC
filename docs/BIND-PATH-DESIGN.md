# Bind path redesign for the draw thread

Design notes, 29 Oct 2026. Every component ships behind a `SOTC_*` switch, default off, with a
verify mode where it skips work, and is accepted only on the fixed-route protocol below.

## Status (29 Oct 2026): built, not measured in the game

| # | Switch | Built | Differences from the design |
|---|---|---|---|
| C0 | `SOTC_OP_PHASES=1` / `=2` | yes | `=2` drops the per-binding probes; `=1` costs ~2 ms per frame itself. RS-memo miss reasons need `SOTC_RS_MEMO>=1`. |
| C1 | `SOTC_WRITE_DIET=1` / `=2` | yes | The hull is set right before each push into `inflight_readbacks` (`BeginAsyncReadbackImpl`, `PrefetchIssue`, `TryServePrevious`, `RecordLabelDownloads`, `RbgAtFault`) and widened in `TryServeExact`. |
| C2 | `SOTC_SYNC_PIPE=1` / `=2` | partly | Single scan, per-word SRT bits and the per-op protect counter. The one-protect-batch-per-op reorder is NOT built: it changes the order of uploads and barriers. Build it only if the counter shows many ops with 2+ protects. |
| C3 | `SOTC_TEX_BLOCK=1` / `=2` | yes | Hits still run `UpdateImage` and `Transit` for every image. No `TransitAspect` early return. |
| C4 | `SOTC_RS_MEMO=2` / `=3` | yes | `=1` is unchanged. |
| C5 | `SOTC_HLE_SKIP=1` / `=2` | partly | Mark coalescing only. The replicate skip is not built: there is no exact "unchanged since" signal for GPU writes. |

Each component was reviewed (read-only) and the findings fixed; the switch-off paths were checked
to be identical to the old code.

### Verify session (29 Oct 2026, ~80 s, `=2`/`=3` modes + `SOTC_OP_PHASES=2`)

Zero mismatches in every component (WRITE DIET, SYNC PIPE, TEX BLOCK, RS MEMO, HLE SKIP, PR).
What the counters say about the expected gain:

| # | Finding | Consequence |
|---|---|---|
| C1 | ~35k write-diet checks/s, ~65–75k inflight walks/s at 1.6–4 entries per walk | Gain unknown; needs the timed A/B. |
| C2 | ~3k–3.8k protects/s; ~15% of ops with protects have 2+ | The per-op batch would save ~20% of ~10 ms/s: not worth building. |
| C3 | Only ~2.7k lookups/s (~90 per frame), 84% hits | Every bind-cache miss sets `bind_cache_recording`, and `MakeTextureBlockKey` refused while recording; with `SOTC_BIND_CACHE=1` C3 almost never ran. **Fixed (29 Oct):** a hit now makes the bind-cache records the full bind would (LOD banks, `BindCacheNoteImage` per sampled image, the UI-linear/hudless uncacheable flag; samplers through `BindSamplers`), after the last check that can fail. Still refused while recording under `SOTC_BIND_CACHE_FRAME` (its lookups need the image desc) or the composite probes. `=2` also compares the records. Not yet measured. |
| C4 | In mode 1, "tick" misses are ~1.1k per 5 s (~7 per frame); "regs" misses dominate | Mode 2 recovers ~7 render-state rebuilds per frame: small. |
| C5 | `marks merged 0`; ~20% of runs already no-op | Mark coalescing finds nothing to merge: no gain. |
| C0 | bind_resources 230–440 ms/s, textures 64–138 ms/s of it; 1.25k–1.8k dirty syncs/s, 74–135 MB/s uploaded | Absolute times are inflated by the verify modes. |

### Timed A/B, 1+1 (29 Oct 2026, ~80 s each, same save and route)

Base `SOTC_FPS=1` vs `SOTC_FPS=1 SOTC_WRITE_DIET=1 SOTC_SYNC_PIPE=1 SOTC_TEX_BLOCK=1 SOTC_RS_MEMO=2`,
both with `SOTC_CVARS="skipSplash=1 nointro"`. Default config (PS4 mode). 13 windows each after
dropping 2. All four were active: RS MEMO hits 76-90%, TEX BLOCK 81% of 4.1 M lookups, SYNC PIPE
and TEX BLOCK 0 verify mismatches.

| | base | C1-C4 |
|---|---|---|
| draw thread, median | 27.32 ms | 28.80 ms |
| draw thread per 1000 cmds | 4.92 ms | 4.78 ms (-2.9%) |
| cmds/frame, median | 5,327 | 5,228 (heavier windows: 8,355 vs 7,219 peak) |
| CP, median | 26.65 ms | 26.82 ms |
| frame p50 / p99 | 33.4 / 42.0 ms | 33.4 / 42.0 ms |
| frames > 50 ms / worst | 31 / 417 ms | 23 / 376 ms |

**Not accepted.** No visual defect seen by the user in the C1-C4 session. The raw median is worse. Normalized per command it is ~3% (~0.8 ms) cheaper, which
is inside the session-to-session noise and far below the 2.5-4.5 ms estimate of §6. Only 1+1
sessions, so this is not a full §6 protocol result. A 3+3 would be needed to accept it, and these
numbers do not justify running one. The draw thread and the CP are both at ~27 ms in these windows.

## 1. Goal

The serial draw thread (`GpuDraw`: `Rasterizer::Draw`, `DrawIndirect`, `DispatchDirect`,
`DispatchIndirect`) is the limiter. Lean config, fixed route: **18.1 ms CPU per frame** (median),
~4,400 ops per frame, ~5 µs per op. At the 30 FPS cap the average fits in 33 ms; the stalls are
peaks where the frame doubles (~14% more ops at ~21% more cost each).

Target: **~12 ms per normal frame**, so that a doubled frame still fits in 33 ms. That is ~6 ms
to remove. The bind work is the largest share (~9 ms), so most of it must come from there.

## 2. What is already measured (do not redo)

| Fact | Source |
|---|---|
| Fast bind / bind cache hit 30–50% of eligible draws; misses are pipeline+hash 30–50%, already invalid 25–50%, hash only 20–25%, epoch only 1–2% | `SOTC_BIND_DIFF` |
| A buffer V# memo hits 96–99% and gains nothing: clean buffer lookups are already cheap | `SOTC_BUFFER_MEMO` |
| Dispatches (~1,100–1,200 per frame) all write memory, never repeat their bind state, and cost ~4.5 ms of bind per frame; changing resources costs ~5.8 µs, offset-only ~1.4 µs | `SOTC_CS_STATS` |
| The game sends no empty draws; cutting particle shadows, decals and grass shadows removes 6% of ops and 0 ms | `SOTC_CMD_STATS`, cvar session |
| Session-to-session noise is ±2–3 ms on different routes | baseline sessions |

Conclusion: memo-style caching of lookups is exhausted. What remains is the work done when
something **did** change: dirty memory, written ranges, image state, render targets.

## 3. Where the per-op time goes

From the code (five read-only passes) and the dispatch phase breakdown in AjncSystem PR #308
(`docs/sotc/DISPATCH-FASTPATH.md`, measured on the plus10 tree, not ours):

| Work | Where | Cost | Confidence |
|---|---|---|---|
| Uploads of dirty ranges inside binds, with re-protection | `SynchronizeMemoryFull` → `ForEachModifiedRange` → `UpdatePageWatchersForRegion` → `Protect` | ~1.2 ms/frame in dispatches alone; `VirtualProtect` ~2.7 µs per call | measured on #308's tree |
| HLE copy/replicate shaders run on the CPU | `vk_shader_hle.cpp` | ~1 ms/frame | measured on #308's tree |
| Written-binding bookkeeping | `AddGpuModified`, `MarkInflightReadbacksDirty`, `RecordCurrentWrite`, `InvalidateMemoryFromGPU`, `BindCacheInvalidate` | ~0.27 µs per mark × ~2.3 written buffers × ~1,200 dispatches ≈ 0.75 ms ceiling | measured on #308's tree |
| Texture binding glue (struct copies, 2–3 hashes, 5 LRU touches per texture, second pass, transitions) | `BindTextures`, `FindImage`, `FindTexture`, `Image::GetBarriers` | part of ~2.5 ms | code reading |
| Render-target lookup and transitions per draw | `PrepareRenderState`, `BeginRendering` | #308 removed 1.06 ms with a memo | measured on #308's tree |
| Clean buffer binding | `SynchronizeMemory` with `SOTC_SYNC_FAST` | 1–2 atomic loads | code + `SOTC_BUFFER_MEMO` |

With `SOTC_PARALLEL_RECORD` on, pipeline selection (`GetProgram`, the SRT walk,
`StageSpecialization`) runs ahead on the prep threads, not on the draw thread.

Defects found while reading (each small, all on the written-binding path):

- `MarkInflightReadbacksDirty` (`buffer_cache.cpp:1598`) walks every in-flight readback's pending
  items for every written binding. Prefetches have a hull test (`pf_lo`/`pf_hi`); ordinary
  readbacks have none.
- A prefetch is marked dirty twice for the same write: in that loop (`:1602-1614`) and again in
  `PrefetchMarkDirty` from `RecordCurrentWrite` (`:5346-5348`).
- `NoteHostWrite` runs twice per written binding (`buffer_cache.cpp:5024`,
  `vk_rasterizer.cpp:5258`); `SOTC_BIND_MARK_ONCE` exists and is off.
- On a clean-but-missed sync, the range is scanned twice (`ForEachUploadRange`, then
  `IsSyncClean` at `buffer_cache.cpp:6116`).
- `SOTC_PROTECT_BATCH` batches the re-protects of one sync, not of one op: an op with five dirty
  buffers can still make five or more `VirtualProtect` calls.

## 4. Principles

1. **Pay per change, not per op.** A step runs only when its inputs changed, proved by a
   counter or generation that every writer bumps.
2. **Batch per op, not per buffer.** Kernel calls and lock round trips are made once per op for
   all its buffers.
3. **Keep the order.** Protection before the copy that reads guest memory; GPU-written marks
   before the next readback decision. No component moves work to another thread where that
   changes what the guest or the GPU sees.
4. **Prove every skip.** Each component has `=2`: run the old path too, compare, count
   mismatches, and use the old result.
5. **Vendor-neutral.** Nothing depends on one GPU vendor or one CPU.

## 5. Components

### C0. Op phase counters (`SOTC_OP_PHASES=1`) — first, decides the rest

- TSC timestamps (`__rdtsc`, ~7 ns, no syscall) around each phase of each op kind: pipeline,
  render state, begin rendering, bind buffers (clean / dirty / written), bind textures, vertex
  and index, descriptor push, dynamic state, HLE, reset.
- Counters per frame: dirty syncs, bytes uploaded inside binds, `Protect` calls and pages from
  the draw thread, in-flight readbacks walked per written binding, RS-memo misses by reason
  (`RsMissTick`, `RsMissPass`, ...).
- One line per 5 s, as the other `SOTC_*` stats. Model: `SOTC_DISP_PHASES` in #308
  (`sotc_dispphase.h`, ~300 lines), extended to draws.
- No behaviour change. Accept: overhead under 0.5 ms per frame against the lean baseline.

### C1. Written-binding diet (`SOTC_WRITE_DIET=1`)

- **Hull per readback.** Each `AsyncReadback` keeps `[lo, hi)` over its pending items (and their
  `gpu_ranges` with `SOTC_RB_OWNER`), set in `BeginAsyncReadback`, widened in `TryServeExact`
  (`buffer_cache.cpp:3573`, the only push after start), never shrunk (trims keep it a superset).
  `MarkInflightReadbacksDirty` skips readbacks whose hull misses the range. Same idea as #308's
  DispatchFastPath, adapted to our `RbxTrim`, `RbgTrim`, `later_gpu`.
- **One prefetch dirtying per write.** Drop the second walk in `RecordCurrentWrite` when the write
  came through `AddGpuModified` (which already ran the loop). Keep it for the other arena writers
  (uploads, image syncs, write-throughs), which do not.
- **Early outs.** No in-flight readbacks: skip the loop (one size check). `SOTC_BIND_MARK_ONCE`
  and `SOTC_INVAL_INDEX` folded in.
- Verify `=2`: run the old marking on a copy of the flags (`dirty`, `later_gpu` sizes) and count
  differences.
- Expected: 0.3–0.75 ms per frame. Risk: low–medium (readback correctness; the verify mode covers
  it). Size: ~150 lines.

### C2. Two-phase bind with one protect batch per op (`SOTC_SYNC_PIPE=1`)

Today each buffer runs scan → protect → copy on its own. New order for one op:

1. **Resolve.** For every buffer of the op (vertex, index, storage), clamp, arena, residency, and
   a single tracker pass that returns the dirty runs **and** the clean verdict (removes the second
   `IsSyncClean` scan).
2. **Protect once.** All runs of the op go into one `ProtectBatch`; runs are merged across
   buffers, so adjacent or shared pages cost one `VirtualProtect`. The batch flushes before any
   copy (principle 3).
3. **Copy.** Uploads as today (CPU staging, `SOTC_ASYNC_UPLOAD`, `SOTC_HOST_IMPORT`).
4. **Mark.** GPU-written ranges, then C1.

Also: set the SRT clean-page bits one `u64` at a time instead of one atomic per page
(`flatten_extended_userdata_pass.cpp:127-136`), and take the runtime read lock once per batch
instead of once per `Protect`.

- Verify `=2`: compare the uploaded byte ranges and the final tracker bits with the per-buffer
  path.
- Expected: depends on C0's protect-call count. Ceiling ~1.2 ms per frame in dispatches plus the
  vertex and index share. Risk: medium (fault and protect ordering). Size: ~300 lines.

### C3. Texture block memo (`SOTC_TEX_BLOCK=1`)

Port of AjncSystem PR #69's `SOTC_TEX_BLOCK_MEMO`, rewritten for our `BindTextures(stage,
binding, set, pr, pr_stage)` and `BatchLock`:

- Key: per stage, the raw T# and S# words, the descriptor flags, `ta_bc_base`.
- Valid while: the per-address image generation (`MemoGeneration`, not the global
  `BindGeneration`, which every streamed image bumps), each image's backing pointer and a new
  per-image `state_version` (bumped on layout, access or backing change), the depth link, and no
  `needs_rebind`.
- A hit replays: image ids, view indices, descriptor infos, bindless slots, LOD marks. It still
  runs, per image, the dirty and track test (`UpdateImage`: CPU faults set flags without bumping
  a generation) and `Transit` unless a no-op check proves nothing to do.
- Not stored: storage images, descriptor arrays, feedback or target binds, blocks whose images
  are written in the viewed range.
- Plus the `TransitAspect` early return and merge-back from #69 (partially transited images). Not
  #69's `NeedsBarrier` rule: it changes sync semantics (630 → 18 barriers per frame) and needs its
  own review.
- Verify `=2` as in #69.
- Expected: 0.3–0.7 ms per frame. Risk: high (image state). Size: ~500 lines.

### C4. Render-target memo across submits (`SOTC_RS_MEMO=2`)

`SOTC_RS_MEMO` is valid within one submit only. If C0 shows misses dominated by `RsMissTick`,
extend it across submits using #308's G-buffer check (image ids and backings, generations, meta
epoch, per-attachment no-op transit), keyed on our `RsMemoBuildKey`. No register serials (#308's
serial transport is large and fails silently on a missing register). If `RsMissPass` dominates,
drop C4.

- Expected: up to ~1 ms per frame (#308 measured −1.06 ms with a serial-based version). Risk:
  medium. Size: ~200 lines.

### C5. HLE replicator: skip unchanged copies (`SOTC_HLE_SKIP=1`)

The CPU-emulated copy and replicate shaders (`vk_shader_hle.cpp:227-341`) cost ~1 ms per frame on
#308's tree. Skip a replicate whose source pages are clean since the last identical run (per-page
CPU-modified query plus a source generation), and coalesce the marks of merged intervals once
(`:111-116`).

- Expected: part of ~1 ms. Risk: medium. Size: ~150 lines.

## 6. Order and acceptance

| # | Component | Expected | Risk | Gate |
|---|---|---|---|---|
| C0 | Phase counters | 0 (decides the rest) | none | overhead < 0.5 ms |
| C1 | Written-binding diet | 0.3–0.75 ms | low–medium | verify 0 mismatches |
| C2 | Two-phase bind, one protect batch per op | up to ~1.2 ms | medium | verify 0 mismatches |
| C4 | RT memo across submits (only if C0 says ticks) | up to ~1 ms | medium | verify 0 mismatches |
| C3 | Texture block memo | 0.3–0.7 ms | high | verify 0 mismatches |
| C5 | HLE replicator skip | part of ~1 ms | medium | verify 0 mismatches |

Expected total: **~2.5–4.5 ms** of the 6 ms target. The remaining gap is honest: reaching 12 ms
probably also needs fewer ops per frame (shadow cascade distances, not yet measured) or less
re-protection overall (a different write-tracking model, out of scope here).

Acceptance protocol, per component:

1. Build with the switch; `=2` on the fixed route (same save, same path); 0 mismatches.
2. Three sessions with `=1` and three without, alternating, same route, lean config.
3. Accept if the draw-thread median drops by at least 0.5 ms more than the spread between the
   baseline sessions, and the count of frames over 60 ms does not rise.
4. CHANGELOG entry (Portuguese) with the numbers, kept or not.

## 7. Ruled out (measured or analysed)

- Lookup memos for buffers and textures alone (`SOTC_BUFFER_MEMO`: 96–99% hits, no gain).
- A compute fast bind (0% repeats; offset-only repeats are already cheap).
- DrawReplay from #308 here: ~0.3–0.6 ms on our op mix, and its flat-buffer swap races with the
  prep threads of `SOTC_PARALLEL_RECORD`.
- #308's depth-only and register-serial fast paths: large transport, silent staleness on a
  missing register mask bit.
- Access-list dedupe from #69: ≤1% (#69's own A/B was within noise).
- Parallel draw recording: ~1.1–1.3x estimated, most work mutates shared caches.
- Engine cvars for particle shadows, decals and grass shadows: −6% ops, no draw-thread change.
