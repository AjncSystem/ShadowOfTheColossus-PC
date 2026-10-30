# Step 7c: parallel work on the draw thread

Design notes, 29 Oct 2026, for step 7c of the stutter plan (`docs/re-stutters/STUTTER-ARCHITECTURE.md`
§4, step 7). Nothing here is implemented. It builds on `docs/sotc/PLANO-THREAD-DESENHO.md` (the
measured draw-thread plan), `docs/BIND-PATH-DESIGN.md` (C0-C5), `docs/ARCHITECTURE-TARGET.md` §1
and report 10 (`docs/re-stutters/reports/10-port-redesign.md`, option (a)).

## 0. Result of 7c.0 (session of 29 Oct, ~5.4 min, 65 windows of 5 s)

`SOTC_PARALLEL_RECORD=1 SOTC_AB_PR=1 SOTC_OP_OVERLAP=1 SOTC_OP_PHASES=2 SOTC_FPS=1`. Default
config (PS4 mode), not the user's launcher. Route: save, ride out, a colossus arena. The first 4
windows (boot) are dropped. Parser: kept in the session scratchpad, not in the repo.

- **Footprint conflicts: 92.9%** of 16.6 M consecutive pairs (71-98% per window; 91.9% in the 15
  heaviest windows). 93% of ops had a footprint (7% not ready, 0.06% unknown). A pipeline of draw
  stages (§4.3) would wait on almost every op, so **7c.1-7c.4 are dropped**. Caveat: the check is
  page-granular and counts two draws writing the same render target as a conflict. That is the
  dominant pattern, and those draws are ordered by nature (blend order, render-target state).
- **Pure emission is ~5% of the draw thread.** In the 15 heaviest windows (2,621-5,351 cmds/frame;
  draw thread 26.0 ms, CP 24.9 ms) the phases per frame are `bind_resources` 12.05 (textures 4.57),
  `begin_rendering` 1.62, `render_state` 1.15, `hle` 1.11, `reset` 0.99, `vertex_index` 0.42,
  `descriptor_push` 0.68, `dynamic_state` 0.39, `other` 1.86 (sum 20.3). Emission
  (`descriptor_push` + `dynamic_state`) is ~1.1 ms, so §4.1 would be ~1.04x.
- **`SOTC_PARALLEL_RECORD`: no measurable gain on the draw thread.** Medians on/off: 12.91 / 13.84
  ms. The paired comparison (each "off" window against its two "on" neighbours) gives the other
  sign: median -0.27 ms, with "off" lower in 20 of 29 pairs. Heaviest windows: 25.58 / 26.07 ms
  (n = 8 / 7). Frames over 50 ms: 5 on / 13 off, too few to read. CP unchanged (11.38 / 11.70). It
  stays off; it costs one or two threads for a gain that, if it exists, is under 1 ms.
- **The command processor is as loaded as the draw thread** in heavy windows (24.9 vs 26.0 ms). Any
  draw-thread gain is capped by it.

**What is left for the draw thread** is making `bind_resources` cheaper. That is the timed C1-C4
(plan step 7a) and fewer re-protects and uploads (step 6, memory classes). This session saw 2,435
protects/s and ~239 MB/s uploaded in a light window. The CP is the next target for 60 FPS.

### Command processor profile (29 Oct, 45 s sampled, 25,219 samples)

`SOTC_PROFILE=GpuCommandProcessor SOTC_PROFILE_DELAY=20 SOTC_PROFILE_SECONDS=45`, same route,
heavy windows of 4,100-7,500 cmds/frame, CP 22-31 ms CPU per frame (`SOTC cpu/frame`).
Symbolized with `tools/profile_functions.py --inlines --qualified`. Small-function names are
unreliable in this LTO build (ICF), so only the large blocks below are used.

| Share of the CP thread's samples | What | Source |
|---|---|---|
| ~20% | idle: condition-variable wait for the game to submit | `condition_variable::wait` |
| ~29% | spinning in `WAIT_REG_MEM`: the game's stream waits for a label, and the port writes labels when the draw thread runs the op. `SOTC CP spin` counts ~38k rounds per 5 s, all of kind "label". | `this_thread::yield` in `Liverpool::Process` (`liverpool.cpp:2766`), the loop at `:3846` |
| ~22% | pipeline lookup: graphics `LookupGraphicsPipeline` 14.5% (`RefreshGraphicsKey`/`RefreshGraphicsStages` 13%, `RefreshFlatBuf` 9.5%, `StageSpecialization` 3.7%), compute `LookupComputePipeline` 7.3% | inclusive |
| ~12% | PM4 parse itself (`Process`, `ProcessCommands`, `ProcessGraphics`/`ProcessCompute` self time) | self |
| ~6% | guest memory reads for the SRT / flat buffer (`BufferCache::ReadClean`) | inclusive |

Reading of it:

- The ~27 ms of CP CPU per frame is **~15 ms of work plus ~10 ms of spinning on labels** (spinning
  counts as CPU time). At 30 FPS the CP is not an independent limit: it waits for the draw thread
  through the game's own label waits. The critical path is the draw thread.
- Each label wait can also leave the draw thread without work. When the CP resumes, it must parse
  and look up pipelines before the next op reaches the ring. `SOTC CP waits for the draw thread`
  is 62-82/s, about 2-3 per frame.
- For 60 FPS, the CP's ~15 ms of work is already at the 16.7 ms budget, and pipeline lookup is half
  of it.
- Diagnostics defect: `SOTC CP spin` prints 0.00 ms/frame while counting 38k rounds, so its time
  accumulation does not work.

## 1. Decision (before 7c.0)

- **The textbook 7c does not pay here.** "N draw workers that each record a slice of a pass into a
  secondary command buffer, with per-worker recorders" has an Amdahl ceiling of about 1.1-1.15x on
  this port. With the record thread on (`SOTC_RECORD_THREAD=1`, the launcher default), the draw
  thread makes no Vulkan calls: it copies arguments into the recorder's ordered stream. What it
  spends its time on is cache mutation, which workers cannot share.
- **The recorder does not need splitting.** It shows 33-35 ms of CPU per frame, but its own `busy`
  counter is 5.6-6.3 ms; the rest is spin. This answers report 10, open question 1.
- **The command processor is an equal co-limiter.** It runs 21-27 ms per frame, the same as the
  draw thread. Speeding up only the draw thread leaves the frame waiting on the command processor.
- **Order:** first time the levers that are already built and never timed (§5, 7c.0). Only if the
  draw thread still goes past ~25 ms in area-entry bursts, start the structural design (§4.3).
  That design starts by removing one cache coupling, measured on its own, before adding any thread.

## 2. Measured budget

| Fact | Value | Source |
|---|---|---|
| Draw thread CPU per frame, normal play | 21-26 ms | `SOTC cpu/frame`, 3 sessions of 29 Oct (default config, PS4 mode) |
| Command processor CPU per frame | 21-27 ms | same |
| Recorder CPU per frame / real work | 33-35 ms / 5.6-6.3 ms (`busy`) | `SOTC cpu/frame`, `SOTC recorder/frame`, same |
| `passsec` (pass secondaries) | 0.0 ms (switch off) | same |
| `WaitBackWrites` (CP waits on queued writes) | ~0/s | `SOTC CP back-write waits`, same; step 7b dropped |
| Draw thread, heavy areas / area-entry bursts | 18-22 / 29-32 ms of real work | PLANO §1.2, stage 0 (user's launcher config) |
| Command processor in bursts | 24-39 ms | PLANO §1.2 |

Per-op phases, heavy window (`SOTC OP PHASES`, a verify-mode session, so absolute times are
inflated; the ratios are what matter), per frame at 30 FPS:

| Phase | ms/frame | Kind |
|---|---|---|
| `bind_resources` (textures ~6.2 of it) | ~15 | cache mutation + emission of uploads/barriers |
| `begin_rendering` | ~1.7 | render-target lookup and transitions (texture cache) |
| `render_state` | ~1.3 | render-target lookup (texture cache) |
| `hle` | ~1.3 | CPU copy shaders over guest/GPU memory |
| `reset` | ~1.5 | per-draw state reset |
| `descriptor_push` | ~1.3 | emission |
| `dynamic_state` | ~0.4 | emission |
| `vertex_index` | ~0.7 | index/vertex binding (buffer cache) |
| `other` | ~1.5 | mixed |

Pure emission (descriptor push, dynamic state, the draw call) is about 3-4 ms of ~25 ms. The rest
touches the caches.

## 3. What already exists (do not rebuild)

| Piece | Switch | Status | What it moves |
|---|---|---|---|
| Record thread | `SOTC_RECORD_THREAD` | on by default (launcher) | Vulkan calls to `GpuRecorder` |
| Prep threads | `SOTC_PARALLEL_RECORD` | built, off, **never timed** | register replay, dynamic values, push data, fast-bind and bind-cache hashes off the draw thread; image prefetch off the CP |
| Prepared descriptors | `SOTC_PR_RESOLVE` | built, off, **diverged 12 times: do not use** | V#/T# decode and mapping checks |
| Pass secondaries | `SOTC_PASS_SEC` | built, off | recorder replay of large passes on another thread; not needed (§1) |
| Bind path C0-C5 | `SOTC_OP_PHASES`, `SOTC_WRITE_DIET`, `SOTC_SYNC_PIPE`, `SOTC_TEX_BLOCK`, `SOTC_RS_MEMO`, `SOTC_HLE_SKIP` | built, verified (0 mismatches), **never timed** | 2.5-4.5 ms of bind work (BIND-PATH §6 estimate) |

Option A1 of PLANO ("the CP decodes the sharps") is in substance `SOTC_PARALLEL_RECORD` +
`SOTC_PR_RESOLVE`. The pipeline lookup itself still runs on the CP (`LookupGraphicsPipeline` in
`Liverpool::SubmitDraw`, `liverpool.cpp:1183`).

## 4. Designs evaluated

### 4.1 N emit workers (report 10 (a), ARCHITECTURE-TARGET §1) — rejected

A sequencer keeps everything that mutates the caches; N workers record slices into secondary
command buffers; per-worker recorders.

- **Ceiling:** workers get only the emission share, ~3-4 ms of ~25 ms → ~1.1-1.15x on the draw
  thread, and the frame then waits on the CP (21-27 ms).
- **No clean cut:** `BindResources` interleaves cache mutation with emission. It records upload
  copies and image barriers into the same ordered stream as the draws, so the sequencer would have
  to forward a command fragment per op anyway.
- **Cost:** 3-5 weeks, high risk. The risk items are the per-draw members of `Rasterizer`
  (`vk_rasterizer.h:229-460`), the cross-draw state (`ReuseShadowCascade`, `SkipStaggeredShadow`,
  DLSS `NoteDraw`, hudless, `ui_shadow`), and the render-pass cuts made by `FlushPeriodic` every
  256 commands.

### 4.2 Two-stage sequencer → emitter — rejected as a goal

One extra thread takes descriptor push, dynamic state and the draw call; ordering is trivial.

- **Ceiling:** the same ~3-4 ms, so ~1.15x.
- It would only matter as a prerequisite for 4.1, which is rejected.

### 4.3 Pipeline by cache owner, with a footprint hazard check — the only structural option worth designing

Each op flows through stages in order, and each stage owns one piece of state exclusively, so no
cache gains locks:

```
op i ──► texture stage ──► buffer stage ──► emit stage ──► recorder stream (op order)
         (render targets,   (buffer sync,    (descriptor
          BindTextures,      uploads,         push, dynamic
          transitions)       protects,        state, draw)
                             vertex/index)
```

- **Throughput:** max(stage) instead of the sum. From the §2 ratios, the stages would be roughly
  texture ~9-10 ms, buffer ~9 ms, emit ~4 ms, plus ~3 ms that stays serial (`hle`, `reset`).
  Ideal ~2x; realistically **1.4-1.6x** after hazard stalls and hand-off cost (inferred, not
  measured).
- **Ordering of emitted commands:** each stage writes its commands for op i into a per-op fragment.
  The emit stage appends the fragments in op order (texture, buffer, draw). This reorders buffer
  and texture uploads inside one op. That needs a proof of safety first (BIND-PATH principle 3:
  keep the order).
- **Hazard check:** a stage may start op i+1 while the next stage is on op i only when the two ops'
  guest footprints do not overlap. Otherwise it waits, and that op runs as if serial. The footprint
  already exists: `AmdGpu::OpFootprint`, computed on the prep threads for `SOTC_CULL_EXPRESS`
  (`draw_prep.h:124-130`).

**Blockers, observed in the code, that must go first:**

1. **Texture uploads go through the buffer cache.** `texture_cache.cpp:1460-1466`
   (`ObtainBufferForImage`, `ObtainGuestCopyForImage`, `IsRegionGpuModified`). The texture stage
   would mutate the buffer cache. Fix: an image-owned upload path (staging owned by the texture
   stage). That fix is a change of its own, with its own switch and measurement.
2. **Buffer sync queries the texture cache in 7 places** (`buffer_cache.cpp`: 5181
   `HasImageInRange`, 6759 `IsMeta`, 6774-6778 `FindImageFromRange`/`GetImage`, 6809 tile manager,
   6902 `EmergencyCollect`). These need either read-only snapshots with a generation counter, or
   routing to the texture stage.
3. **The page manager is shared** by both caches and by the guest fault handler. It already has
   region locks (faults come from guest threads), so it is the one shared piece that is already
   concurrent. Its lock order with the cache locks must be audited before two stages call it.
4. **GPU-written marks cross caches.** A dispatch writing a buffer that a later draw samples as a
   texture invalidates that texture through `InvalidateMemoryFromGPU`. The hazard check must treat
   written footprints as hard dependencies, or green blocks come back.
5. **The texture cache lock** (`ReentrantMutex` + `BatchLock`, `texture_cache.h:434`) is taken by
   guest write faults (`InvalidateMemoryOnGuestFault`). A texture stage holding the batch lock
   longer would make guest faults wait longer (report 10 S5). The `tc` field of `SOTCSTALL`
   measures this.

## 5. Order

Every step is behind a `SOTC_*` switch, default off, and accepted per BIND-PATH §6: 3+3 timed
sessions on the same route, alternating; the draw-thread median must drop by more than the
baseline spread; frames over 60 ms must not rise; no visual change. Image identity is checked
against the serial reference `SOTC_CP_THREADS=0 SOTC_CP_STRICT=1`.

| # | Step | Switch | Code | Gate |
|---|---|---|---|---|
| 7c.0 | Time what is built: C1-C5 (this is plan step 7a), then `SOTC_PARALLEL_RECORD`. One session with `SOTC_OP_PHASES=2` and no verify modes gives the real per-phase split. | existing, plus `SOTC_OP_OVERLAP` (built 29 Oct) | one counter: how often consecutive ops' footprints conflict (open question 5). Needs `SOTC_PARALLEL_RECORD=1` and `SOTC_FPS=1`; line `SOTC OP OVERLAP` | decides whether 7c.1 is needed |
| 7c.1 | Only if the draw thread is still over ~25 ms in area-entry bursts: an image-owned upload path, so the texture side stops mutating the buffer cache (blocker 1). Serial; no new thread. | new, `SOTC_IMG_UPLOAD_OWN` | `texture_cache.cpp`, `tile_manager`, staging | 0 visual differences; draw time not worse |
| 7c.2 | Read-only texture-cache queries from the buffer side, with a generation counter (blocker 2). Serial. | new | `buffer_cache.cpp`, `texture_cache.h` | verify mode `=2` compares against the locked query: 0 mismatches |
| 7c.3 | Two stages (texture ‖ buffer+emit) with the footprint hazard check. `=2` runs the stages but waits on every op (serial order, parallel threads): it must match `=0` exactly before `=1` lets stages overlap. | new, `SOTC_DRAW_PIPE` | `liverpool.cpp` (op ring), `vk_rasterizer.cpp` (Draw split), `vk_scheduler.cpp` (per-op fragments) | identity vs serial; `tc` wait not up; draw median down |
| 7c.4 | Split the emit stage off (third stage), only if 7c.3 shows the buffer+emit stage as the limit. | `SOTC_DRAW_PIPE=1` + stage count | as 7c.3 | as 7c.3 |

7c.0 needs user sessions and one small counter. The structural part (7c.1-7c.4) is weeks of work
with high risk.
It is worth starting only if 7c.0 leaves the bursts over budget.

**The command processor.** At 30 FPS it fits on average. In bursts it reaches 24-39 ms, so even a
perfect draw thread leaves those frames late. At 60 FPS (PLANO phase 4) the CP needs its own
design: move the pipeline lookup to the prep threads and measure the PM4 parse. That is outside 7c.

## 6. Ruled out

- N identical emit workers (4.1): ~1.1x ceiling.
- A separate emitter thread as a goal (4.2): ~1.15x ceiling.
- Splitting the recorder (`SOTC_PASS_SEC` as a requirement): real recorder work is ~6 ms.
- Fine-grained locks in the texture and buffer caches. The T48 lesson: locks in the wrong place
  produced the worst stalls.
- Making `SOTC_PR_RESOLVE` the base of anything: it diverged in its verify session.

## 7. Open questions

1. The real per-phase split without verify modes (7c.0). The §2 ratios come from a verify session.
2. Does `SOTC_PARALLEL_RECORD` lower the draw-thread median measurably? It has never been timed.
3. Is reordering buffer and texture uploads within one op safe? This needs a read of every upload
   and barrier site in `BindResources` before 7c.3.
4. The lock order between the page manager's region locks and the texture cache's `ReentrantMutex`
   when two stages call them at once.
5. How often consecutive ops have overlapping written footprints. If it is most of them, the hazard
   check serializes the pipeline and 7c.3 gains nothing. `SOTC_OP_OVERLAP=1` (7c.0) answers
   this before any thread is added. It runs in `Liverpool::ExecuteBackOp`, outside the
   `SOTC_OP_PHASES` scopes, so it can share the phase-split session; it does add draw-thread work,
   so that session is not a timed one.
