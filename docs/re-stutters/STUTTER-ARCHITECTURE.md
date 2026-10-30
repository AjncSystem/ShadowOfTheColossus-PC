# Stutters: root causes in the engine and the port, and the redesign

Reverse-engineering round of 29 Oct 2026 on branch `re/stutter-architecture`. Ten parallel
analyses; their reports are in `reports/` (01-10, brief in 00). This document consolidates them,
records what the lead verified or corrected, and sets the order of work. Every hook follows
`docs/HOOK-DESIGN.md`, so it works on CUSA08809 and CUSA08034 (same eboot today) and turns itself
off on an unknown eboot.

Tools used: `tools/re/eboot_re.py` (function ranges, calls, strings and imports of the eboot) and
the Ghidra pseudocode of all 56,900 functions in `out/re/decomp/` (not in git; rebuild with
`tools/re/ghidra/` and `out/re/ghidra/run.bat`).

Status labels: **verified** (checked by the lead in code or pseudocode), **reported** (from an
agent report, not re-checked), **corrected** (an agent claim the lead found wrong).

## 1. The model: one lockstep chain, one saturated thread

A frame passes through these stages, each waiting on the one before:

1. **Shadow Game Update.**
2. **Game Render.** The render job builds the command buffers.
3. **The guest submit thread** (`KickoffSubmitJob`, eboot 0xd07c0).
4. **The port's command processor (CP) thread.**
5. **The port's serial draw thread.**
6. **The recorder thread.**
7. **The GPU.**

How the stages are tied:

- **Update and render** hand frames over through BPE `PS4SyncEvent` event flags (reports 02, 06,
  08):
  - update waits at 0x534800 and 0x53496d;
  - render waits at 0xbd717 and 0xbd93d, and on its job counter at 0xbd922.
  - The engine itself runs no frame ahead. **Reported** (consistent across three reports).
- **The submit gate.** With the default `SOTC_CP_AHEAD` (mode 0), `sceGnmSubmitDone` sets
  `submission_lock` when the GPU is not idle, and the next submit or doorbell waits in
  `WaitGpuIdle`. That is until the draw thread has run everything queued
  (`gnmdriver.cpp:92-175, 407, 2417, 2462, 2481`). **Verified.** The game calls the submit
  imports indirectly: the index shows 0 direct callers, and the port's comments name
  `KickoffSubmitJob` 0xd07c0.
- **What this adds up to:** any time the draw thread (or a fault/readback chain behind it) takes
  over the frame period, the guest's render thread, and through the event handshake the update
  thread, take it too. A 200 ms port burst is a 200 ms game frame. The game clock is real time
  (rdtsc, report 08), so the simulation takes the whole hitch in one step, within its own
  `gameMaxDeltaTime` / `gameDeltaTimeCorrectionTolorance` cvars (registered in sub_262a80;
  **verified** strings, defaults not decoded).
- **The draw thread has no slack.** In steady gameplay it runs at 33.3 ms per 33.3 ms frame
  (median of 70 windows, report 09), with ~4,400 ops per frame at ~5 µs each, ~9 ms of it binds
  (BIND-PATH-DESIGN). The recorder is a co-bottleneck: 16-23 ms on-CPU per frame including spins
  (report 10). Every extra cost lands on the frame.

## 2. Measured stall causes (report 09)

Data from existing logs: 5 dev sessions (experimental config), 8 trace sessions and the user's
install.

| Cause | Share | Notes |
|---|---|---|
| Draw-thread CPU bursts | 2,281 of 3,077 ms excess, 38 of 43 frames | Gameplay episodes of 66-134 ms frames recur at ~60-68 s in 4 of 5 dev runs and 7 of 8 traces: a fixed repro point. |
| Boot spike, every session | 149-213 ms frame at ~5 s | 624-705 images in one frame; copy threads 138-287 ms, CP 87-125, draw 91-120. |
| CP-thread bursts | 580 ms, 4 frames | Trace frame 140: CP parse 103 ms, bind 94 ms. |
| Pipeline built synchronously | 250 ms, 1 frame | A "tiny draw" pipeline is always built on the CP thread (`vk_pipeline_cache.cpp:1084`, report 07). |
| Guest unmap waits, default install only | 7.5 s in ~15 s | 3.2 / 2.9 / 1.5 s in the first windows. The dev config sets `SOTC_DROP_FAST=1` and shows 13-148 ms. |
| Readback inside a write fault | 1,437 ms, one frame | Early trace session only; not seen in current builds. |

In slow frames, the guest's render and job threads sit in event-flag waits while the port's draw
thread works (traces): **the game waits on the port**, not the other way round.

## 3. Findings by area

### 3.1 XPR visibility masks (report 01)

- **The cull:** two GPU culls per frame, each closed by EVENT_WRITE_EOP event 0x28
  (BOTTOM_OF_PIPE_TS, data 1) to `[ctx+0xe8]+0x40` and `+0x68` (sub_925c0). **Reported.** The
  port's `liverpool.cpp:1815` comment says CS_DONE; the report says the eboot uses 0x28.
- **The consumer:** CPU code at 0x1a6d20 scans each set's bitset (at most 2,048 bits, 256 B) and
  records one `sceGnmDrawIndexIndirect` per visible object. On the port, those reads fault and
  become readbacks: ~440 per heavy frame. 78-89% of them find the GPU already done, so the cost is
  the port's round trip, not GPU time.
- **No CPU wait found:** no CPU spin or event wait on the label on that path. Either the game
  consumes whatever is in memory (on PS4, likely the previous frame's bits or a race it
  tolerates), or it waits somewhere not yet found. The cvar `forceUseLastIndexBufferCullingResults`
  exists. **Reported; settle at run time.**
- **Labels are answered early:** the port's EOP labels are written when the draw thread runs the
  op, not when the GPU finishes (`liverpool.cpp:3456-3470`, report 10). Correctness of GPU
  results therefore rests on the readback faults.

### 3.2 CPU writes to GPU-tracked memory (reports 03, 04)

- **Volume:** 9,769-16,600 write faults per 5 s. Of 97k faults in slow frames:
  - ~47% happen inside the **host** `memcpy` / `memset` / `memmove` the game imports;
  - ~18% are BPE heap metadata (eboot 0x48a70-0x4a477) sharing 4 KiB pages with GPU data, the
    slowest at 0.4-0.6 ms each;
  - ~15% come from named per-frame writers: particles sub_101720 (8.7%), view constants
    sub_be1f0, instances/skinning sub_118100 → sub_123410.
  
  **Reported.**
- **Memory layout:**
  - **Pools:** direct memory is Onion 1 GiB (type 0) and Garlic 7.93 GiB (type 3), plus a 2.1 MiB
    bitmap and a 406 MiB flexible pool. Two allocator families sit on top: `CPUMemory` (kind 1)
    and `GPUMemory` (kind 2).
  - **CPU data in the GPU pool:** a CPU BPE heap arena lives inside Garlic, so the PS4 memory type
    does not tell CPU data from GPU data. The port tracks the whole pool.
  
  **Reported.**

### 3.3 Streaming (report 05)

- **Loader:** the "Resource Loading" thread runs `ResourceFactory` (ThreadFunc 0x1464d0, loop
  0x148600) with 10 priority queues. Payloads are decoded with Oodle (0x1aa2a0 → 0x89d3b0), and
  reads go through Fios2, which is guest code over the port's `read`/`pread`. **Reported.**
- **The barrier `WaitForAllCmdsToFinish` (0x1469d0)** loops while any queue has work, waiting on
  the loader's event (**verified**).
- **Corrected:** the report called it a per-frame barrier. In `GameLoop` (0x25c080) the call at
  0x25cb12 sits **after** the main loop exits (decomp lines 404-413), not inside it.
- **Its other callers are virtual:**
  - `DeleteResource` (0x1478d0);
  - the destructor (0x146770);
  - 0x147d60;
  - 0x25ddd0, a callback registered in the engine setup (sub_262a80);
  - 0x3c5a00 (game exit).
  
  **Open:** whether `DeleteResource` runs during gameplay streaming. If it does, an unload waits
  for every pending load.
- **`SyncPointProcessPendingResources` (0x1480f0)** finalizes completions with no budget: a burst
  of completions lands in one frame. **Reported.**

### 3.4 First use (report 07)

- **Small share:** pipelines, shaders and images are ~8% of the stall time.
- **Two defects:**
  - the synchronous tiny-draw pipeline build;
  - 419 cached pipelines skipped at every boot by permutation index collisions
    (`vk_pipeline_serialization.cpp:481-484`), and built again on first use.
- **Upload volume:** the texture cost is the upload volume, 9-13 GB per session, not image
  creation (~15 µs each).
- **What the engine knows ahead:** the area's resource names (`shadow.cwks`, per-area precache
  lists 0x533100, `ShaderList` 0xb0e3a0), not GCN shader addresses.

**Reported.**

### 3.5 Other GPU→CPU waits (report 02)

- **`RenderDeferredMemoryDeleter::FlushPendingFree(wait)` (0x8ef20):** spins and then calls
  `usleep(1)` on a job counter.
- **gnmx compute-queue-full retry (0x863f80):** spins without sleeping. Not hit in the logs.
- **Stub:** `sceGnmComputeWaitSemaphore` is a port stub that emits nothing (correctness).
- **No other readback found:** no occlusion, histogram or particle readback was confirmed.

**Reported.**

## 4. The redesign, in order

Each step is off by default, has a verify mode where possible, and is accepted only on measured
gain (same save, same route; see BIND-PATH-DESIGN §6). The kind of hook is noted:
**import** (by NID, any build), **engine** (by signature, HOOK-DESIGN), or **port** (no game hook).

### Step 0 — Measure what the reports could not settle (no behaviour change)

| Probe | Kind | Settles |
|---|---|---|
| A default-config session with `SOTC_FPS=1` only (no `_LEAN`, frame generation off) | port | The first default stall table (report 09 §6.4). |
| `SOTC_TRACE=1` on the current build at the 60-68 s repro point and at boot | port | Boot spike split; the gameplay episodes. |
| Log at 0x1a6d20 entry: mask pointer, the two labels, frame counter | engine | Does the game read the XPR masks before the GPU signals? |
| Time and count inside 0x1469d0, with caller | engine | Does any loader barrier run during gameplay? |
| `SOTC_EVF_TRACE=PS4SyncEvent` | port | Which waits are on the frame's critical path. |
| Log name, kind, size and address at the block allocator 0x479a0 | engine | Which allocations reach Garlic; class-based tracking. |
| Counter of host `memcpy`/`memset` destinations that hit tracked pages (`src/runtime.c`) | import | The 47% fault class, per call site. |

### Step 1 — Fixes and defaults with the data in hand

- **Unmap waits in the default config.** Measure `SOTC_DROP_FAST=1` against off. 7.5 s of guest
  waits without it. If it is clean, make it the default.
- **Tiny-draw pipeline built synchronously** on the CP thread: queue it like the others.
  `vk_pipeline_cache.cpp:1084`.
- **419 stale cached pipelines:** fix the permutation index collision.
  `vk_pipeline_serialization.cpp:481-484`.
- **Fix the CS_DONE comment** at `liverpool.cpp:1815` once 0x28 is confirmed.

### Step 2 — Write faults: declare instead of fault

- **Host `memcpy`/`memset`/`memmove`.** Import kind: they are the port's own code. Before
  copying into a tracked range, mark the whole destination span written and unprotect it with one
  call, instead of one fault per page. This targets ~47% of write faults.
- **BPE heap metadata.** Engine kind. Keep heap headers off GPU pages: make the allocator in
  0x48a70-0x4a477 place block headers out of line, or have the port treat those headers as
  CPU-only (sub-page tracking). This targets ~18% of faults, the slowest.

### Step 3 — XPR without a same-frame round trip

- **Option A** (port, exists as `SOTC_RB_PREV`) and **option B** (engine, hook 0x1a6d20): serve
  the last completed mask without blocking. Choose by step 0's result:
  - **if the game already reads before the label,** this matches PS4 behaviour and costs nothing;
  - **otherwise,** it adds one frame of visibility latency (a newly visible object appears one
    frame late).
- **PC-native variant:** a compute pass compacts the visible draws, and the consumer becomes one
  `DrawIndexIndirectCountMulti` (wrapper 0x86b470 exists). The CPU never reads a mask.

### Step 4 — Run one frame ahead

- **Submit gate:** `SOTC_CP_AHEAD=2` with `SOTC_CP_AHEAD_FRAMES=1` exists. Measure it after
  step 3; with same-frame readbacks gone, the remaining hazard is the one analysed in
  `gnmdriver.cpp:110-155`.
- **Clamp the game's dt** (`gameMaxDeltaTime`) so that a remaining hitch does not become a
  simulation jump. Engine kind (cvar, version-independent through `sotc_modes`).

### Step 5 — Streaming paced, not bursted

- **Bound the completion burst** in `SyncPointProcessPendingResources` (0x1480f0): N items or
  M ms per frame.
- **If step 0 shows the barrier in gameplay** (`DeleteResource` → 0x1469d0), make
  `HasPendingWork` (0x147f40) ignore the StreamingTexture and Background queues for that call.
- **Pre-stage Vulkan images at load completion.** At 0x148d14, or when a completion is pushed,
  hand the decoded data to a port worker that creates the image and stages the upload, paced by
  the existing 32 MB budget.

### Step 6 — Memory classes (UMA → PC)

From the step 0 allocator log:

- **No mirror, no tracking:** CPU-only ranges (Onion, flexible pool, CPU heaps, the bitmap).
- **No CPU protection:** GPU-only render targets (sub_9aef0).
- **Upload ring:** per-frame GPU heaps become host-visible rings, uploaded at submit.
- **Readbacks:** go to host-cached buffers at fence time.

This is `docs/sotc/MODELO-MEMORIA-PC.md` driven by the engine's own allocator kinds instead of PS4
memory types.

### Step 7 — The draw thread itself

- **Bind path work** (BIND-PATH-DESIGN, C0-C5) and its timed measurement.
- **Parallel translation (report 10 option a):** only after the draw-state split. The expected
  gain is 1.1-1.3× for the current op mix. The recorder must be split too.

## 5. Hooks named so far

All engine hooks resolve by signature (`tools/re/eboot_re.py sig`). The addresses below are for
fingerprint `72c1d4a9…3bb225`.

| Address | Name (proposed) | Used by step |
|---|---|---|
| 0x1a6d20 | BPXPR_ConsumeVisibilityMask | 0, 3 |
| 0x86b470 | BPGnm_DrawIndexIndirectCountMulti wrapper | 3 (PC-native variant) |
| 0x1469d0 | ResourceFactory::WaitForAllCmdsToFinish | 0, 5 |
| 0x147f40 | ResourceFactory::HasPendingWork | 5 |
| 0x1480f0 | ResourceFactory::SyncPointProcessPendingResources | 5 |
| 0x148d14 | CreateResource build callback site | 5 |
| 0x479a0 | block allocator entry (CPUMemory / GPUMemory) | 0, 6 |
| 0x48a70-0x4a477 | BPE heap allocator code | 2 |
| 0x534690 / 0xbd6d0 | ShadowGameUpdate_Frame / RenderThread_ThreadFunction | 0 (event trace mapping) |
| 0xd07c0 | KickoffSubmitJob | 4 |
| 0x262a80 | engine cvar registration (`gameMaxDeltaTime`) | 4 |

About 135 proposed names are in the reports' section 5. They are to be checked and merged into
`tools/re/names.txt`.

## 6. What this round did not settle

- **Timing questions** that only a run can answer: the XPR read timing; loader barriers during
  gameplay; which event waits are on the critical path.
- **Per-frame behaviour:** whether `sceKernelBatchMap` runs per frame, and the per-frame size of
  the GPU heaps.
- **The CPU half of the 47% `memcpy` faults:** which game call sites write into tracked pages.
- **Codecs and defaults:** the codec of compression method 1, and the TextureManager streaming
  cvar defaults.
