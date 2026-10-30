# 02 - Label waits: who emits them, what they protect, and what to remove

Read only. No repository file was edited except this report. Addresses are eboot VAs unless marked
port (file:line in the current working tree). Labels: **observed** (disassembly, decompiled
pseudocode, source line or log line), **inferred**, **unknown**.

## 1. Answer

- **observed:** the game emits every GPU label wait through one emitter, `BPGnm_EmitWaitRegMem`
  (0x86edc0, unique signature). Its PM4 header is 0xC0053C00. Almost all call sites use
  function 5 (GTE), mask 0xffffffff, reference 1, so they mean "wait until the label is >= 1".
  Two sites use function 3 (EQUAL) at 0x93ef0.
- **observed:** the label writer is `BPGnm_EmitEventWriteEop` (0x86eb60, unique signature), header
  0xC0044700. The fence wrappers write `EVENT_WRITE_EOP` event 0x28 (BOTTOM_OF_PIPE_TS), data 1, and
  then `WAIT_REG_MEM` on the same label. Two other patterns exist, described in section 2.2.
- **observed:** in the port, a label wait spins the command processor (CP) until the draw thread has
  run the queued EOP. That is a full drain of the draw queue at every fence. This is the source of
  the "label" spin kind (`liverpool.cpp:3853-3880`, `:2359-2373`).
- **observed:** the existing counter "SOTC CP waits for the draw thread" (49-54/s in the logs, 0/s in
  some windows) does **not** count WAIT_REG_MEM spins. It counts `WaitBackTail` calls
  (`liverpool.cpp:616-660`, `sotc_stats.h:689`). The callers are DrainBack paths (COND_EXEC, DUMP_CONST_RAM,
  guest-thread SendCommand, faults). The WAIT_REG_MEM spin never calls `WaitBackTail`. So the brief's
  "2-3 waits per frame" is a different event. The number of WAIT_REG_MEM packets per frame is **unknown**.
- **observed:** the port already has an unmerged, off-by-default mechanism for the main case:
  `SOTC_CP_LABEL_SKIP` (`liverpool.cpp:111-135`, `:2378-2450`, `:3995`, `:4443`; declared in
  `liverpool.h:833-850`). A WAIT on a label that is the newest publish of its queue, and whose
  queued value already satisfies the wait, passes at once after `WaitBackWrites()`. It is not in any
  commit (`git log -S` finds nothing); it sits in the working tree.
- **observed:** `SOTC_CP_LABEL_SKIP=2` (count only) has no report line in the tree.
  `ReportCpLabelSkip` (`liverpool.cpp:127`) is defined and declared, but nothing calls it. The
  first step is a one-line wiring in `driver.cpp` near line 1927, next to `ReportXpr`.
- **observed:** the skip does not cover the `WRITE_DATA 1` + `WAIT` pattern (sub_92640, sub_926c0).
  `NoteQueuedLabel` is called only from the EOS, EOP and compute EOP paths (lines 3673, 3825, 4536).
  The WRITE_DATA case (`liverpool.cpp:3777-3806`) never records its label, so those waits always spin.
- **inferred:** the game's labels are early. The draw thread writes them when it runs the EOP
  (`liverpool.cpp:3564-3830`, default `SOTC_RB_EXACT` off). Nothing in the port waits for the GPU
  to finish. So a guest-side removal of a fence removes only the CP-to-draw-thread ordering. The
  draw thread already runs the ops in order.
- **unknown:** whether any game CPU thread polls a GPU label (see section 4). None found beyond the
  XPR case in report 01 and the event-flag findings in report 02 (`02-gpu-cpu-sync.md`).

## 2. Evidence

### 2.1 Emitters (observed)

| Address | Name | Packet | Arguments (from the disassembly) |
|---|---|---|---|
| 0x86edc0 | `BPGnm_EmitWaitRegMem` | 0xC0053C00, 7 dwords | rdi cb, rsi address (memory space bit set when > 0xffff), edx mask, ecx function (low 3 bits), r8 reference; poll interval 0xa (`0x86ee4f`) |
| 0x86eb60 | `BPGnm_EmitEventWriteEop` | 0xC0044700, 6 dwords | rdi cb, esi event type, edx/ecx data select, rcx address, r9 data |
| 0x86eec0 | alternate wait emitter, packet at 0x86ef15 is 0xC0053C00 | | used by sub_92720 when its flag is set (`0x92799`) |
| 0x86f5e0 | WRITE_DATA emitter, header 0xC0034800 (ME) | | rsi = dst select, rdx = address, ecx = first data dword |
| 0x86f1d0 | a 0xC0055800 emitter (WRITE_REG_MEM-like) | | not a wait |

Signature check (`eboot_re.py sig`): 0x86edc0 and 0x86eb60 unique, 20 bytes each.

### 2.2 Fence wrappers that emit a label wait (observed, decompiled `out/re/decomp/0009/*`)

| Wrapper | Pattern | Label writer before the wait | Callers (count of sites) |
|---|---|---|---|
| sub_92b50 `BPGnm_DispatchWithLabelWait` | dispatch (sub_928f0), zeroed CPU slot, EOP 0x28 data 1, WAIT GTE 1 | the EOP (the dispatch has a publish before it) | about 14 sites (`sub_ab630`, `sub_bdfd0`, `SetViewParams`, `BPXPR_IndexCullObjects` 0x11de8e, ...) |
| sub_92720 | zeroed CPU slot, EOP 0x28 data 1, WAIT GTE 1 (tail-jump at 0x927c1) | the EOP, nothing queued between EOP and WAIT | 21 sites: `RenderView::MainCamera` (0xcb67d, 0xcb876), `ParticleRenderer::UpdateSimulation` (0x105e27, 0x1065a0), `sub_106ce0`, `UpdateGPUData_InstanceData` (0x1182fe), `BPXPR_BuildPreShaderSlots` (0xc6b77), ... |
| sub_92640 | zeroed CPU slot, **WRITE_DATA 1**, WAIT GTE 1 | WRITE_DATA, not EOP | callers `sub_bdfd0` (0xbe131), `SetViewParams` (0xc0a96) |
| sub_926c0 | same as sub_92640 | WRITE_DATA | callers `sub_181420`, `sub_1817a0` |
| sub_92850, 927d0, 92720 variants | EOP + WAIT, one of them with the 0x86eec0 emitter | EOP | several |

Signatures (`eboot_re.py sig`): sub_92b50 unique (20 bytes); sub_92720 unique (20 bytes); sub_927d0
unique (36 bytes); sub_92850 unique (20 bytes); sub_93960 unique (40 bytes); sub_ca6d0 unique (32 bytes).
**sub_92640 and sub_926c0 are not unique within 64 bytes** (2 matches each).
The site `0x927c1` (the tail-jump in sub_92720) is unique, 28 bytes.

Other wait-emitting functions in the decompilation (`grep FUN_0086edc0`, 47 files): the per-frame
labels are at `[ctx+0xe8] + N` for N in {0x8, 0x10, 0x18, 0x30, 0x38, 0x40, 0x68, 0x80, 0x88, 0x98, 0xa8, 0xb0, 0xb8, 0xd0, 0xd8, 0xe0, 0xf0, 0xf8, 0x100, 0x110, 0x118, 0x120, 0x128}. These are the same layout as the XPR labels (report 01). Almost all of them gate a GPU pass on the EOP of the previous job. The labels are cleared by the CPU at record time (for example `XPRPreRender` at 0xca99b and 0xca9a3, `sub_92640` at `*puVar1 = 0`).

### 2.3 What each wait protects (inferred from the pattern, observed in the code)

- **Dispatch to draw (sub_92b50, IndexCull):** the next packets (DrawIndirect) use the dispatch
  output. On the GPU this needs an ordering. In the port the draw thread runs both in order, and the
  Vulkan commands are recorded in that order. The host-side wait only delays the CP's parsing.
- **Pass-to-pass fence (sub_92720):** "all earlier work must finish before the next pass", for
  example render target to texture. The real hazard on the GPU is covered by barriers in the port's
  recorder. **unknown:** whether every render-target-to-sampled and compute-to-draw transition emits a
  Vulkan barrier. The port does not wait on the CP for it, so a missing barrier would be a bug today as
  well (inferred).
- **WRITE_DATA 1 then wait (sub_92640, sub_926c0):** the wait is on a value the same stream wrote
  immediately before. It is a self-inflicted round trip: the WRITE_DATA is queued to the draw thread
  (`liverpool.cpp:3777-3806`, `FuseWriteData` may hold it, `FlushWd` queues it when the WAIT arrives,
  since WAIT_REG_MEM is not in `FuseTransparent`, `liverpool.cpp:944-975`). The CP then spins until the
  draw thread writes the value. The protection is nil, because the write is already ordered before any
  later op.
- **CPU-written data:** not seen in these wrappers. The CPU clears the label at record time, before
  the packets are submitted. Waits on CPU-written memory from a later CPU write are **unknown** (none found).

### 2.4 The port side (observed)

- The CP spin: `WaitRegMem` handler (`liverpool.cpp:3853-3885`). It first tries the VO-label path
  (flip), then `LabelWaitSkip` (when `SOTC_CP_LABEL_SKIP` is on), then `while (!Test) YIELD_GFX()`.
  The loop does not call `DrainBack`, so it is not in `cp_waits`.
- `MemWaitKind` (`liverpool.cpp:2359-2373`): "label" when the back queue is non-empty, otherwise
  "wait reg mem". The log's 22524 "label" rounds vs 1673 "wait reg mem" rounds per 5 s
  (`c14_base.log:244`) mean almost all spin is waiting for queued work.
- EOP write (`liverpool.cpp:3564-3830`): the label is published with `BackOrRun`, so it lands on the
  draw thread. The non-deferred path calls `NoteQueuedLabel` (line 3825).
- `NoteQueuedLabel` and `BackPublish` (`liverpool.cpp:2380-2392`, `:~720`): any publish of another
  kind sets `label_barrier[q] = head` for all queues. A recorded label restores the previous barrier.
  So a skip is only legal when the label is the **last publish** of its queue.
- `LabelWaitSkip` (`liverpool.cpp:2413-2450`): checks the newest record for the address, the barrier,
  whether the draw thread has already run it, `wait.TestValue(value)`. In mode 1 it calls
  `WaitBackWrites()` and returns true. It returns false under `SOTC_CP_STRICT=1` (`back_strict`).
- Default switches (observed): `SOTC_EOP_TICK` off (`liverpool.cpp:200-201`), `SOTC_RB_EXACT` off
  (`launcher/sotc_tuning.py:677`, "measured: worse"), `SOTC_CP_STRICT` off (`:150`), `SOTC_CP_LABEL_SKIP`
  off (`:112-120`).

### 2.5 Logs (observed)

- `c14_base.log` (heavy-ish windows, 5 s counters): `SOTC CP spin: 0.00 ms/frame in 24197 rounds (wait
  reg mem 1673, label 22524, ...)` (line 244), 12958 rounds (291), 11676 (334), 31686 (382), 36904
  (424). The `0.00 ms/frame` is a known defect of the spin timer (the design doc says the same).
- `SOTC CP waits for the draw thread`: 49, 51, 48, 54 and 0 per second in the logs (12, 8, 7, 5 and 10
  windows). `SOTC CP ops ... back-write waits 0.0` in every sampled window, so `WaitBackWrites` never
  waits. The 49-54/s is therefore DrainBack, not label spins.
- The design doc (`docs/DRAW-THREAD-7C-DESIGN.md` section 0) measured the CP at about 29% spin in a
  45 s profile (25,219 samples), about 7 ms per frame of the ~25 ms CP time. This is the best available
  estimate of what the spin costs.

### 2.6 CPU-side waits on labels (observed negative results)

- The eboot imports no `sceKernelWaitEqueue` and no `sceGnmAddEqEvent` call sites (`eboot_re.py imports`).
  No equeue wait on the GfxEop interrupt exists, so the EOP interrupt is not consumed by the eboot.
- The CPU waits found in report 02 are event flags (`EventFlag_WaitInfinite` 0x3b4a0), the job
  counter (`JobSystem::WaitForCounter` 0x35590, spin then `usleep(1)`), and `pthread_cond_wait`. None
  is tied to a GPU label. The one open case is the XPR mask consumer (report 01).

## 3. Hook proposals

Ranked by expected gain. The first two are port changes, not guest hooks. A guest hook is the
wrong tool here: it cannot know whether the CPU or the port still needs the CP to wait. The port
change keeps the WaitBackWrites guard.

### H1. Enable and measure SOTC_CP_LABEL_SKIP (port, existing code)

- **Site:** `Liverpool::LabelWaitSkip` (`liverpool.cpp:2413`), called from the WaitRegMem handler
  (`:3995`) and the compute WaitRegMem (`:4443`). Records written at `:3673`, `:3825`, `:4536`.
- **Mode:** no guest hook. Switch `SOTC_CP_LABEL_SKIP=2` (count), then `=1` (skip).
- **Change:** nothing new, except wiring `ReportCpLabelSkip(report, seconds)` into
  `driver.cpp` near line 1927 (next to `ReportXpr`). Without it mode 2 prints nothing.
- **Why it shortens a hitch:** a skipped wait does not drain the draw queue. The CP keeps parsing,
  and the draw thread never runs out of work at the fence. The spin itself (about 7 ms per frame of CP
  time in heavy windows) disappears.
- **What could break:**
  - A CP read of guest memory that a queued op writes. `WaitBackWrites()` covers CPU writes
    (WRITE_DATA, DMA). GPU-written memory is protected by read faults on the draw thread (report 10).
    That protection is unchanged, but it is not re-verified in this round.
  - A label whose value is set by a later CPU store. The record only forwards a value the port queued
    itself, so this should not happen (inferred).
  - Deadlock: none added. A skipped wait never blocks. Unchanged waits still spin.
  - Compute to draw ordering: unchanged on the draw thread. Vulkan barriers are the real protection
    (see 2.3, unknown).
- **How to verify:** mode 2 gives `waits`, `skippable`, `skipped` per 5 s. Then mode 1 with the same
  route: draw thread ms per frame, CP ms per frame, `SOTC CP spin` rounds, and a frame-time comparison.
  Correctness: a debug assert on the draw thread (`Test(label)` at the would-be wait position); any
  failure counts as a mismatch.
- **Signature:** not applicable (port code).

### H2. Record WRITE_DATA labels (port, new code in the same mechanism)

- **Site:** the WriteData case (`liverpool.cpp:3777-3806`) and `FuseWriteData` / `FlushWd`
  (`:828-900`). Add `NoteQueuedLabel(GfxQueueId, lb_prev, true, address, false, data[0], 0)` when the
  value is a 32-bit write to a memory address. Held-back WRITE_DATA must record when it is published
  (`FlushWd`), not when it is parsed.
- **Mode:** none. Covers sub_92640 and sub_926c0 (WRITE_DATA 1 then GTE 1).
- **Why it shortens a hitch:** removes a self-inflicted drain at every SetViewParams and at the
  callers of sub_926c0 (per view per frame). The count is **unknown**.
- **What could break:** the same as H1. The WRITE_DATA is also a publish (it sets `label_barrier`),
  so the record must be made after the publish, and `label_barrier` must be restored the same way as
  for EOP. Under `SOTC_WD_FUSE=1` the held WRITE_DATA must not be in `wd_pending` when the wait runs.
- **Verify:** mode 2 counts the new skippable waits. Signature check: sub_92640 and sub_926c0 are not
  unique in 64 bytes, so a guest-side approach would need an anchor. The port change does not need one.

### H3. Guest call-site redirect of the sub_92720 fence (rejected as a first step)

- **Site:** the tail-jump `jmp 0x86edc0` at 0x927c1 (sub_92720+0xa1). Signature: unique, 28 bytes
  (`eboot_re.py sig 0x927c1`).
- **Mode:** immediate patch of the rel32, redirecting to a trampoline that returns at once. This is
  the same mechanism as `scripts/sotc_modes.py`, but resolved by signature.
- **Why:** removes the fence for all 21 callers at once.
- **What could break:** removes the GPU ordering and the CP's WaitBackWrites guard together. Any CP
  read of GPU-written memory after the fence then runs without protection. H1 keeps that guard, so H1
  reaches the same gain on these sites with less risk. Use H3 only if H1 measures no skippable waits on
  these sites.
- **Verify:** mode `=2` (verify): run the original emitter too, and at the would-be wait check on the
  draw thread that the label has the expected value; count mismatches.

### H4. Probe hook on WAIT_REG_MEM and EOP emission (observe only)

- **Site:** Entry on `BPGnm_EmitWaitRegMem` (0x86edc0, unique signature 20 bytes) and
  `BPGnm_EmitEventWriteEop` (0x86eb60, unique signature 20 bytes).
- **Mode:** entry, `HookAction::Continue` only (observe). Log per frame: the return address (the
  caller), mask, function, reference, address class (`[ctx+0xe8]+N` or other). The return address is at `[rsp]` on entry.
- **Why:** nothing in the code tells the number of WAIT_REG_MEM packets per frame, or which callers
  produce the label spin. This is the missing measurement. It also answers the open questions in 4.
- **What could break:** nothing (observe only). Cost: one call per packet at the emitter; keep it
  off by default (`SOTC_HOOK_WAIT_PROBE`).
- **Verify:** the counts add up to the CP spin rounds of the same window.

### Rejected: a full guest hook on the emitter (Return at 0x86edc0)

- Returning from 0x86edc0 for every call would remove all waits, including any that wait on CPU-written
  data or the VO label. Not safe. Not proposed.

## 4. Open questions

1. How many WAIT_REG_MEM packets run per frame, and from which callers? (H4 measures it.)
2. Which of the 49-54 DrainBack calls per second are COND_EXEC (`liverpool.cpp:3977`), DUMP_CONST_RAM
   (`:2945`), guest-thread SendCommand (buffer_cache.cpp 623, 946, 975, 1020), or faults
   (`PrepareGpuSideFault`)? A per-caller tag in `WaitBackTail` answers it.
3. Are the compute-to-draw and render-target-to-texture transitions covered by Vulkan barriers in the
   port recorder? Only then is H1 safe for sub_92b50 and sub_92720 sites. Check the dispatch path in
   `renderer_vulkan` for `pipelineBarrier` before the first draw that reads the output.
4. Does any game CPU thread poll a GPU label? Not found. A search for loops with `pause` that read the
   label slots `[ctx+0xe8]+N` was not completed. (Report 01 open question on the XPR consumer also
   remains.)
5. Does the port's record thread (`SOTC_RECORD_THREAD`) keep WRITE_DATA and EOP ordered with draws? Assumed yes
   (the draw thread makes no Vulkan calls, per `docs/DRAW-THREAD-7C-DESIGN.md` section 1), not re-checked.
6. RELEASE_MEM (0xC0064900 family): not identified in the eboot in this round. The grep was stopped for
   time (unknown).
7. The sibling report `01-cmdbuf-lifetime.md` says a fast drop can unmap a nested IB before the CP
   parses it. H1 makes the CP parse sooner (less lag), which should shrink that window. It does not
   close it. Unverified.
8. Gain: unknown. H1 removes up to about 7 ms of CP spin per frame in heavy windows. How much draw time
   is idle waiting on the CP is not measured.

## 5. Ranked summary (for the reply)

1. H1: wire `ReportCpLabelSkip` into `driver.cpp`, run `SOTC_CP_LABEL_SKIP=2`, then `=1`. Port-side.
2. H2: record WRITE_DATA labels in the same mechanism (covers sub_92640 and sub_926c0).
3. H4: entry probes on 0x86edc0 and 0x86eb60 (observe) to count waits and callers per frame.
4. Attribute the 49-54 DrainBack/s with a per-caller tag in `WaitBackTail`.
5. H3: a guest redirect at 0x927c1 only if H1 finds no skippable waits on sub_92720 sites.
6. Verify the Vulkan barriers for compute-to-draw and render-target-to-texture before H1 is enabled by default.
</content>
