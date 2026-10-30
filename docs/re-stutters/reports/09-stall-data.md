# 09 — Stalls measured from existing logs (condensed by the lead from the agent's reply)

Sources: dev copy `user/last_run*.log`, `user/events*.log`, `user/trace/` (8 sessions, 157 slow
frames); install `<install>\user\`. Agent scripts and the full per-frame table:
`scratchpad/09/` (parse_stalls.py, analysis.py, trace_agg.py, stall_table.md, classify_*.txt).

## Answer
- 43 dev stall frames (SOTC_STALL_MS=60) + 1 install frame. Excess over 33.3 ms: 3,077 ms. By
  largest component: **draw-thread CPU bursts 2,281 ms (38 frames)**, CP-thread CPU bursts
  580 ms (4), pipeline compile 217 ms (1). Wait fields read 0.0 everywhere: an artifact of
  SOTC_FPS_LEAN=1 (it disables those clocks).
- **Boot spike every session:** largest boot frame 149-213 ms at 4.9-5.3 s in all 5 dev runs and
  frames 140-163 of all 8 trace sessions; 624-705 images created in that one frame, copy-thread
  CPU 138-287 ms, draw 91-120 ms, CP 87-125 ms. 14 of 43 frames, 1,296 ms excess.
- **Gameplay episodes at the same game time:** 1-11 frames of 66-134 ms starting ~60-68 s in 4 of
  5 dev runs and 7 of 8 trace sessions (one at 97-98 s). A repro point (fixed scene, inferred).
- **Worst spike 1,437 ms** (trace 20261029-012353 frame 91, ~3 s): a BPE JobWorkerThread write
  fault waits 1,411 ms on a readback; CP, draw and GPU idle; SceFiosIO and Game Render blocked.
  Only in that early session.
- Default install: no per-frame data (4 of 5 sessions had FSR 3 frame gen on); one [PERF] line:
  51 frames > 50 ms, worst 917 ms, 49.9 FPS (60 mode).

## Phase totals (dev A-E)
| Phase | Frames | Excess ms | Worst ms | Blamed |
|---|---|---|---|---|
| boot | 14 | 1,296 | 213 | draw 11 (859), CP 3 (437) |
| area change | 1 | 217 | 250 | pipelines (272 ms in one frame) |
| gameplay | 23 | 1,256 | 134 | draw 23 |
| menu/mouse released | 5 | 308 | 176 | draw 4, CP 1 |

Gameplay stall frames: draw CPU median 48.2 ms (33.6-67.2). Steady state: draw CPU 33.33 ms per
frame (median of 70 windows, max 37.66): the draw thread runs at the frame period with no slack
(or pacing is counted as CPU; unsettled).

## Window-level costs (per run; candidates, not attributed to frames)
| Cost | A | B | C | D | E | install 15:17 |
|---|---|---|---|---|---|---|
| View-split freeze ms (boot window ~200-217) | 326 | 308 | 306 | 293 | 258 | 214 |
| Heap-guard walks ms (0.70 only) | 576 | - | - | - | - | - |
| Memory-lock reader waits ms | 216 | - | - | - | - | 91 |
| Unmap drop waits ms | 13 | 148 | 92 | 75 | 34 | **7,527** (3,151 / 2,881 / 1,494 in first 15 s) |
| Copy-thread submit waits ms | 5,965 | 7,443 | 4,424 | 5,127 | 6,034 | - |
Dev runs all set SOTC_DROP_FAST=1; the default install does not: its guest unmap waits are the
largest config difference (driver.cpp:1791 says these are guest-thread waits).

## Traces (8 sessions, 157 slow frames)
| Session | Frames | Median ms | Max ms | Causes |
|---|---|---|---|---|
| 011831 | 40 | 71 | 182 | fault 38, draw 2 |
| 012353 | 18 | 75 | 1,437 | fault 8, draw 8, gpu 2 |
| 013009 | 18 | 75 | 174 | draw 17, fault 1 |
| 013634 | 15 | 77 | 183 | draw 14, fault 1 |
| 014212 | 12 | 90 | 197 | draw 10, game-cpu 1, fault 1 |
| 014525 | 17 | 81 | 150 | draw 17 |
| 031433 | 26 | 76 | 210 | draw 26 |
| 044024 | 11 | 79 | 148 | draw 11 |
Early sessions fault/readback bound (game threads wait 100-240 ms/frame on readbacks and write
faults); from 013009 on, draw is the cause in 95 of 98 slow frames. Mean per slow frame (thread
sums): game event-flag waits 400 ms, cond waits 148, equeue 104, mutex 92, readbacks 84, write
faults 46, read faults 45; CP parse 44, idle 49, blocked on draw 19; GPU queue 49. In a slow
frame the guest render/job threads sit in event-flag waits while the port's draw thread is busy:
**the guest waits on the port** (inferred). Trace frame 140 (044024): CP parse 103 ms; draw
sub-spans bind resources 94 ms, draw indirect 64 ms.

## Caveats
- SOTC_FPS_LEAN=1 zeroes wait/fault-latency/cause fields in dev logs; traces have their own
  clocks. Install 15:17 (no LEAN) still reports fault latency 0.0 us (timer cause not found).
- No readback waits in any SOTC window of current builds; older traces show 100-240 ms/frame.
- SOTC_UPLOAD_BUDGET_MB=0 install run: boot frame 85 ms without an image burst (one sample, frame
  gen on).
- OP PHASES (dev A 5-10 s window): bind_resources 412.5 ms/s (textures 146.2), ~41% of the draw
  thread.

## Open questions
1. Boot spike split (CP parse vs draw sub-spans) on the current build: needs SOTC_TRACE.
2. Is the 60-68 s episode one scene? No position field in traces.
3. Does the 1.4 s readback-in-fault freeze still happen?
4. First default-config stall table: run with SOTC_FPS=1 only (no LEAN, frame gen off).
5. Does SOTC_DROP_FAST remove a frame-visible guest wait in the default config?
