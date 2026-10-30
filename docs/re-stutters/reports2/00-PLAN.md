# Stutter plan, round 2: complete plan for approval

29 Oct 2026. Based on reports 01-17 in this folder and the measured sessions of 29 Oct
(`docs/DRAW-THREAD-7C-DESIGN.md` section 0, `docs/BIND-PATH-DESIGN.md`, the "Em desenvolvimento"
section of `CHANGELOG.md`).

Ground rules for every item:
- each has its own `SOTC_*` switch, off by default;
- where it skips work, it has a `=2` verify or observe mode and runs that first;
- the user runs the game; nothing opens without the user's go-ahead;
- commits only when asked.

## 0. What is already settled

| Area | Result | Report |
|---|---|---|
| Draw-thread parallelism (7c) | Ruled out: 93% of consecutive ops conflict; emission is ~5% of the thread | 7C |
| `SOTC_PARALLEL_RECORD`, bind path C1-C4 | No measurable gain, no visual defect | 7C, BIND-PATH |
| XPR and loader in play | Little stalling: ~90% of mask reads are after the label; the barrier ran 0 times | probes |
| Command processor | ~15 ms real work + ~10 ms spinning on labels; pipeline lookup is ~half the work | 7C |
| Unmap waits | 9.3 s/session at loads; `SOTC_DROP_FAST` cuts them to <50 ms (worst frame 417 -> 125-185 ms) but crashed 1 run in 4 | CHANGELOG |
| File I/O, game-logic CPU, render-target aliasing | Not hitch causes | 09, 10, 13 |
| Flip wait | In the port: present runs under `submit_mutex`, shared with every draw submit | 12 |
| Job system | Port ignores guest priority/affinity; a contended guest mutex goes straight to a kernel wait; `usleep(1)` sleeps 0.52 ms | 16, 05 |
| `BuildSync` | Reached through `EnsureBuilt` (0x14d7b0) on the update thread (`GameCamera`, `AudioListener`, per frame), render thread, main loop and area load; in-play frequency unknown | 17 |
| Image churn | ~5,000-7,700 images leave the texture cache uncounted; likely via `TextureCache::UnmapMemory` | 13 |
| Draws | Sun shadow cascades ~6,500/frame; far slices ~5,090 | 14 |

## 1. Built and reviewed, not measured (Wave C)

| Switch | What it does | Verify mode |
|---|---|---|
| `SOTC_PIPE_MEMO=1` | CP: skips the stage specialization and module search when all inputs match (fetch code and memory generation included) | `=2` counts mismatches |
| `SOTC_CP_LABEL_SKIP=1` | CP: skips a label wait it queued itself, after waiting for queued memory and GPU writes | `=2` counts without skipping |
| `SOTC_CLASS_TRACK=1` | Stops write-protecting PageAllocator's CPU-only ranges; ranges overlapping an image are excluded | `=2` shadow: GPU pages, faults and image ranges per pool |
| `SOTC_AREA_PREWARM=3` | Records the images each area creates and pre-creates them on re-entry (boot counts as an area) | `=2` records only |
| `SOTC_DROP_FAST=1` (guarded) | Skips the unmap wait unless the range holds a command buffer in use; nested IBs registered at submit and mapping-checked | counters + bad-packet diagnostics |

## 2. Phase A: measurement, no new code

| Session | Switches | Answers |
|---|---|---|
| A1 (not timed) | `SOTC_PIPE_MEMO=2 SOTC_CP_LABEL_SKIP=2 SOTC_CLASS_TRACK=2 SOTC_HOOK_ALLOC_PROBE=1 SOTC_AREA_PREWARM=2 SOTC_FPS=1` | memo mismatches; skippable label waits; faults/image ranges per pool; manifest sizes |
| A2 (timed) | `SOTC_USLEEP_YIELD=1` vs the base | cheapest lever (05, 16) |
| A3 (timed) | `BB_PRESENT_MODE=Fifo` vs `Mailbox` | the flip wait (12) |
| A4 (timed, load route) | `SOTC_DROP_FAST=1` guarded, 2-3 runs | is the crash gone; the `bad PM4 packet` line locates it if it returns |

Then each switch that passed A1 is timed one at a time against the base: `SOTC_PIPE_MEMO=1`,
`SOTC_CP_LABEL_SKIP=1`, `SOTC_CLASS_TRACK=1`, `SOTC_AREA_PREWARM=3`.

## 3. Phase B: new observe-only code (one Haiku wave)

| # | What | Kind | Report |
|---|---|---|---|
| B1 | `BuildSync` probe: Wrap on 0x146b60 and `EnsureBuilt` 0x14d7b0; logs thread, caller chain, state, time | engine hook, observe | 17, 09 |
| B2 | dt probes 0x25ccc0 and 0x534a73: which dt the update uses; jump vs slow motion on a hitch | engine hook, observe | 05 |
| B3 | Per-render-job draw tags, Wrap on 0xc12a0: draws per system per frame | engine hook, observe | 14, 04 |
| B4 | Image frees by reason (confirms the churn source) | port counter | 13 |
| B5 | Flip wait split into free-frame / present_done / submit_mutex | port timers | 12 |
| B6 | Readbacks per guest address | port counter | 07 |

## 4. Phase C: port changes, gated on Phase A/B data

| # | What | Switch | Report | Risk |
|---|---|---|---|---|
| C1 | Short spin before the kernel wait in the guest mutex | `SOTC_MUTEX_SPIN_US` | 16 | low; CPU burn if set too high |
| C2 | Place the game's job workers off the CP/draw/recorder cores | extends `SOTC_CPUSETS` | 16 | low-medium; few cores on some CPUs |
| C3 | Defer texture-pool decommits to N per frame (targets the unmap waits without the DROP_FAST race) | new (hook 0x98ae0) | 03 | medium |
| C4 | Cap mip copies per frame | new (site 0x13d01e) | 03 | pop-in |
| C5 | Park images on unmap, re-arm on remap | `SOTC_UNMAP_PARK` | 13 | stale contents; VRAM |
| C6 | Present not under `submit_mutex`; larger present ring | `SOTC_PRESENT_FRAMES` | 12 | VRAM |
| C7 | Particle stats from a previous-frame copy | hook `sub_8c1b0` | 07 | stale count |
| C8 | Fix `direct_release` order (GPU unmap hook before zeroing) | always on | 01 | real bug; check lock order |

## 5. Cvars to A/B (no code; `SOTC_CVARS` or the launcher)

`forceUseLastIndexBufferCullingResults=1`; `cacheUnusedTextures=1` and
`textureMinUnusedFramesForUnload`; `textureMaxLoadsInFlight` / `numMipRequestsPerFrame`;
`threadedRendering` / `threadedCulling` / `threadedSumbit` (decode their defaults first);
`gameMaxDeltaTime=0.05` only after the dt probe (B2). Full table: report 15.

## 6. Rejected

- Draw-thread parallelism (7c); `PARALLEL_RECORD`; bind path as a frame win (measured, no gain).
- Removing `WAIT_REG_MEM` packets in the game (drops the CP write guard, 02).
- Changing `HasPendingWork` globally (use-after-free on destruction, 03).
- Routing allocations by kind alone (kind 0 holds GPU-written heaps, 06).
- Removing `InsertWaitFlipDone` (tearing, 12).
- Cutting dispatch counts (~3.9 us each, no gain, 04).

## Order

1. Phase A sessions. Keep what beats the base spread with no visual regression.
2. Phase B probes, then one user session, to pick among C3-C7 and settle the dt question.
3. Phase C in Haiku waves, each with a verify mode and its own session.
4. Each accepted switch gets a launcher row, a CHANGELOG entry and a README line (Portuguese).
