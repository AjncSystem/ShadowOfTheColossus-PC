# 12 - Flips and presentation: how the game waits, how the port waits, and what to change

Read only. No repository file was edited except this report. Nothing was built or run.
Addresses are eboot VAs (`out/eboot.elf`) unless marked "port" (file:line in the working tree).
Pseudocode is from `out/re/decomp/<4 hex>/<addr>.c`. Labels: **observed** (disassembly, pseudocode,
source line or log line), **inferred**, **unknown**.

## 1. Answer

- **observed:** the game never waits on a flip on the CPU during play. Its only per-frame flip
  wait is GPU-side: `sceGnmInsertWaitFlipDone` (emitter 0x86f700, called from 0x91f50 and the frame
  tail 0x118470), a WAIT_REG_MEM with function EQUAL 0 on the display buffer's label. The CPU calls
  `sceVideoOutWaitVblank` and `sceVideoOutIsFlipPending` only in shutdown and buffer reconfiguration
  (0x956e0, 0x95760). The flip-event thread (0x960a0) wakes on each flip event and does nothing.
- **observed:** the game registers **2 display buffers** (RenderFlip_Setup 0x95760, call at 0x958ae,
  `RegisterBuffers(h, 0, addrs, 2, ...)`) and cycles **2 render slots** (RenderBackend_Frame 0x90ea0,
  `slot = (slot + 1) % 2`, 0x1d0 bytes each). The game's own in-flight depth is therefore about two
  frames.
- **observed:** the per-frame flip is part of `sceGnmSubmitAndFlipCommandBuffers`, sent by
  RenderThread KickoffSubmitJob (0xd07c0 -> sub_91f90 -> 0x863af0), followed by `sceGnmSubmitDone`.
  The port turns the flip into a PM4 NOP (`PatchedFlip`, gnmdriver.cpp:2206-2260). The CP raises the
  `GfxFlip` IRQ from the draw queue (liverpool.cpp:3168-3190). `SubmitFlipInternal` then runs on the
  **draw thread**, and inside it `PrepareFrame -> GetRenderFrame` blocks until a presentation frame is
  free (vk_presenter.cpp:736, 1817-1845).
- **observed:** the VO label that the game's next `InsertWaitFlipDone` waits on is cleared by the port
  in `SubmitFlipInternal` (driver.cpp:2408-2440, `EarlyVoLabel`), **after** `PrepareFrame`. So a stall
  in `GetRenderFrame` stops the CP at the next buffer reuse. The chain is: draw thread blocked at flip
  N, label of the buffer not cleared, CP blocked at frame N+1's final pass, guest submits held back.
- **observed:** the 30 FPS cap is set by the game and enforced by the port. The game calls
  `sceVideoOutSetFlipRate(h, 1)` from `FUN_95ef0` (rate 1 = 30 FPS, rate 0 = 60 FPS, rate 2 = 20 FPS),
  which `GameFrame_Update` (0x25ae80) calls every frame. The port's present thread takes a flip only
  after `(rate+1) * vblanks_per_60hz` software ticks (driver.cpp:2575-2590). With the default
  `BB_VBLANK_HZ=120` (sotc.py:228) that is 4 ticks = 33.3 ms. The game itself does not sleep.
- **observed:** `VSyncWindow=10%` is not a flip-rate setting. It selects enum `VSyncWindowPercentage_10`
  (strings at 0xfc4201-0xfc42b3), read by `FUN_25ef60` -> `FUN_25f480`, which picks an fps mode.
  How that mode maps to the flip rate is **unknown** (see section 4).
- **observed, port:** `Presenter::Present` (which runs on the **present thread**, vblank driven)
  calls `vkQueuePresentKHR` while holding the global `Scheduler::submit_mutex`
  (vk_presenter.cpp:1477-1481). Every draw-thread queue submit also takes that mutex in `QueueSubmit`
  (vk_scheduler.cpp:1000-1040). In FIFO, a present can block (queue full) and then stall all submits
  from the draw thread. This is a second, unmeasured path into the draw-thread stall.
- **inferred:** the 25 ms "flip wait" in the plan (`PLANO-THREAD-DESENHO.md` section 2) has at least
  three possible sources: no free frame (free_cv), the `present_done` fence of the reused frame, and the
  `submit_mutex` held by a blocked present. `SotcStats::SplitWaitFrame` times the first two together, so
  the logs cannot tell them apart. This is the first thing to measure.
- **observed, config:** the user's launcher settings file (`user/launcher_settings.json`) has
  `present_mode: "Mailbox"`, but the plan says Fifo. Its `extra_env` also already sets
  `SOTC_CP_FLIP_PACE=1` and `SOTC_FLIP_SERVICE=1`, which move part of the wait to the CP thread. Which
  of these were active during the measurements in the plan is **unknown**.
- **Ranked proposals (section 3):** (1) split the frame-wait timer by cause; (2) A/B the present
  mode (`BB_PRESENT_MODE`) against the `submit_mutex` path; (3) a frame ring larger than the swapchain
  (`SOTC_PRESENT_FRAMES`, new); (4) move the early VO label release above `GetRenderFrame`; (5) a
  non-blocking flip that repeats the last frame when no frame is free. No game hook is needed to
  remove a flip wait, because the game has no CPU flip wait. Game hooks are only useful for
  measurement (section 3, G1).

## 2. Evidence

### 2.1 Game side: who waits for flips

- **observed:** `icallers` for the flip imports (`tools/re/eboot_re.py icallers <name>`):
  - `sceVideoOutSubmitFlip`, `sceVideoOutIsFlipPending`, `sceVideoOutWaitVblank`: only
    `Flip::SubmitAndWaitVblank` (0x956e0, the shutdown path, which then fires
    `sceKernelTriggerUserEvent`) and `RenderFlip_Setup` (0x95760, the reconfiguration path, guarded by
    `buffers registered != -1`).
  - `sceVideoOutAddFlipEvent`: only RenderFlip_Setup (0x957b5).
  - `sceVideoOutGetEventData` / `sceVideoOutGetEventId`: only `FlipEvent_WaitEqueue` (0x960a0).
  - `sceVideoOutSetFlipRate`: `VideoOut_OpenSetFlipRate` (0x95a90, at 0x95b81 and 0x95e3f). Its
    initial call is `SetFlipRate(h, 0)` (0x95a90 pseudocode).
- **observed:** `FlipEvent_WaitEqueue` (0x960a0) loops on `sceKernelWaitEqueue`. For a VideoOut flip
  event it only reads the event id and data, and it returns only on the user-event trigger from 0x956e0.
  It makes no other call, so it cannot hold the game.
- **observed:** frame tail `FUN_118470` (`Frame::InsertWaitFlipDone` / `Renderer_FrameTail`). Its
  order: R::Hud, VBlank, `sub_91f50` (InsertWaitFlipDone for the slot's buffer), then R::HudResolve with
  the copy into the display buffer. `sub_91f50` (0x91f50) calls `sub_86f700` with buffer index
  `*(DAT_02ed33f0+0x7b0)`, which is the render slot (0 or 1).
- **observed:** `sub_86f700` emits `sceGnmInsertWaitFlipDone(cmd, 7, vo_handle, buf)`. In the port
  (gnmdriver.cpp:1268-1287) that is a WAIT_REG_MEM on `buffer_labels[buf]`, raw 0x13 (EQUAL, ref 0,
  mask all ones). It waits until the flip of that buffer has been processed. The meaning of `7` is
  **inferred** (VO bus/handle enumerant).
- **observed:** `RenderBackend_Frame` (0x90ea0) starts each render with a CPU spin on the EOP label
  of the slot (`do {} while (*label != expected)`, pseudocode lines 15-18). It then waits on a second
  flag and calls `sceKernelUsleep(1)` after 10 000 spins. The EOP is written by `FUN_90e30`
  (EventWriteEop, 0x86eb60) at the end of the frame, before the flip NOP.
- **observed:** `KickoffSubmitJob` (0xd07c0): for each command buffer except the last it calls
  `FUN_91f80` (non-flip submit). For the last one, when `*param_1 == 1`, it calls `FUN_91f90`
  (`sceGnmSubmitAndFlipCommandBuffers` via 0x863af0), then `sceGnmSubmitDone`.
- **observed:** `GameFrame_Update` (0x25ae80) calls `FUN_95ef0(DAT_02ed3670, fps_mode, *DAT_02eed8d8)`
  on every frame. `FUN_95ef0` maps `fps_mode`: 0/1 -> `SetFlipRate(0)` (60 FPS), 2 -> `SetFlipRate(1)`
  (30 FPS), 3 -> `SetFlipRate(2)` (20 FPS). It returns early when nothing changed.
- **observed:** `fps_mode` comes from `*DAT_02eed800`, or from `FUN_245440` when its flag is set. The
  VSync window routine (`FUN_25ef60` / `FUN_25f480`, called from `FUN_25f290`, `FUN_49a130`,
  `FUN_558b20`, `FUN_559a10`) writes the mode through `&local_3c` and reads the cvar strings
  `VSyncWindowPercentage_100 ... _0` (0xfc4201-0xfc42db, enum order 100, 40, 30, 20, 10, 0).
  **unknown:** whether `FUN_25f480`'s output reaches `FUN_245440` / `FUN_95ef0`, and the exact tolerance
  formula. The "10%" in `+VSyncWindow=10%` (CMDLINE_LEA 0x9367b9, `scripts/sotc_modes.py:35-41`) selects
  enum value 4.
- **observed:** the 60 FPS game patch (`scripts/sotc_modes.py`, mode `fps60game`) changes
  `TargetFramesPerSeconds` from 2 to 1 at three sites (0xb0a7a6, 0x89a47f, 0x89a499). The mode
  `fps60` patches the import stub at 0x1390090 to return 0.
- **inferred:** the game does not block for the 30 FPS cap by itself. Its pacing comes from the flip
  rate the port applies, plus the GPU-side waits above (the InsertWaitFlipDone label and the submit
  gate in `sceGnmSubmitDone`, round-1 report 08).

### 2.2 Frames in flight

- **observed:** 2 display buffers (0x95760 `RegisterBuffers(..., 2, ...)`), and the slot toggle
  (0x90ea0) with the same 2. Each slot holds a 0x1d0-byte render context and an EOP label.
- **inferred:** at most about two game frames of GPU work are in flight. The second flip cannot be
  rendered into until the first flip's label has been cleared (section 2.3). So the flip queue in the
  port (`flip_pending_num`, error above 16 in driver.cpp:2385) is never deep in normal play.
- **observed, port:** the presentation ring has `num_images` frames (`Dlssg || FrameGen ? max(images, 4)
  : images`, vk_presenter.cpp:513-524). `image_count = minImageCount + 1` (vk_swapchain.cpp:263), so 3 on
  most drivers. The default present mode is Mailbox (`BB_PRESENT_MODE`, emulator_settings.h:30).

### 2.3 Port side: the flip path, step by step

1. **observed:** `PatchedFlip` NOP reached on the CP (liverpool.cpp:3168). Without
   `SOTC_CP_FLIP_PACE`, the port queues `GfxFlip` with `BackOrRun`, so it runs on the draw thread.
   With `SOTC_CP_FLIP_PACE=1` (on in the user's `extra_env`), `PaceFlip` first waits on the CP until
   `free_queue.size() > pending_flips` (vk_presenter.cpp:1778-1804), then the draw thread reaches it.
2. **observed:** `sceVideoOutSubmitEopFlip` (video_out.cpp:353-370) registers a one-shot `GfxFlip` IRQ
   handler, which calls `SubmitFlip(port, buf, arg, true)`. So `SubmitFlipInternal` runs as part of
   the draw queue (its place is the NOP in the stream).
3. **observed:** `SubmitFlipInternal` (driver.cpp:2408-2440): `PrepareFrame(group, address)` (this
   calls `GetRenderFrame`), then `EarlyVoLabel` (`buffer_labels[submit_prev_index] = 0`, `SignalVoLabel`,
   `NotifyCpWait`), then the request is queued for the present thread.
4. **observed:** `GetRenderFrame` (vk_presenter.cpp:1817-1845): `free_cv.wait` until `free_queue` is
   non-empty, then `waitForFences(frame->present_done)`, then a recreate if the size changed. It is
   timed by `SplitWaitFrame` as one figure.
5. **observed:** the present thread (`PresentThread`, driver.cpp:2443 onward) takes requests on
   software ticks, at BB_VBLANK_HZ, `(rate+1) * vblanks_per_60hz` ticks apart. It runs `Flip`, which
   calls `presenter->Present(frame)` **inline** (AsyncPresent is off by default). `Present` does
   `AcquireNextImage` (blocks in FIFO until an image frees), records and submits the copy and overlay,
   then `vkQueuePresentKHR` under `Scheduler::submit_mutex`. Only after that does `free_frame()` return
   the frame to `free_queue` (vk_presenter.cpp:1477-1493).
6. **observed:** `present_done` is signalled by the present submit. That submit waits for
   `swapchain.GetImageAcquiredSemaphore()`, which itself waits for the acquire. So the `present_done`
   fence of a frame completes only after the present thread has processed that frame.
7. **observed:** the draw thread is one FIFO (`Liverpool::BackLoop`). Readbacks, draws and binds all sit
   behind a blocked `SubmitFlipInternal`, so the readback queueing in the plan follows from this.

### 2.4 Logs and measurements already in the repo

- **observed (plan, section 2 table, 29/10):** "espera no flip" (wait at the flip) is 19-25 ms per
  frame on the santuário/menus route, 13-16 ms in medium areas, 3-10 ms in heavy areas, and 0 ms at
  the 40-50 ms frames of area entry. Draw CPU work in heavy areas is 20-24 ms (plan, same table).
- **observed (plan, section 2):** the "draw busy" counter includes the flip wait (`FrameTimer` /
  `SotcStats::SplitWaitFrame`). The 28.8-28.3 ms "draw busy" in all windows is the frame period, not
  work.
- **observed (plan, section 4 stage 1):** the texture upload copies are not the flip wait; that was
  measured separately and ruled out for the 90-175 ms frames.
- **unknown:** the split of the flip wait between free-frame wait, present-done wait and
  submit-mutex wait. No counter separates them today.

## 3. Hook proposals and port changes

Ranked by expected gain per unit of risk. "Mode" follows `docs/HOOK-DESIGN.md`: import hooks are
port-side wraps in `runtime_resolve`; game sites are Entry, Wrap, Site or Range. Signatures were
checked with `eboot_re.py sig`.

### P1. Split the frame wait by cause (measurement, no behaviour change)

- **Where:** `Presenter::GetRenderFrame` (vk_presenter.cpp:1817), `Presenter::Present` around
  `swapchain.Present` (vk_presenter.cpp:1477-1481) and `AcquireNextImage` (vk_presenter.cpp:~1190-1205
  region), plus `QueueSubmit` (vk_scheduler.cpp:1038).
- **Change:** four timers under `SOTC_FPS`: (a) free-frame wait (`free_cv`), (b) `present_done` fence
  wait, (c) `submit_mutex` wait in `QueueSubmit` on the draw thread, and (d) time spent in
  `vkQueuePresentKHR` / acquire on the present thread while it holds the mutex. Print them with the
  SOTC FPS line.
- **Why:** it decides between P2, P3 and P5. Without it, nothing else can be ranked.
- **What could break:** nothing, if it only reads clocks. Keep it behind the existing `kFpsLog` switch.
- **Verify:** the four timers add up to the existing `SplitWaitFrame` (within noise) on a 30 FPS
  route; (c)+(d) are near zero in Mailbox if the mutex theory is right.
- **Signature:** not applicable (port code).

### P2. A/B the present mode (cvar, no code): `BB_PRESENT_MODE=Mailbox` vs `Fifo`

- **Where:** `Swapchain::FindPresentMode` (vk_swapchain.cpp:205-246), read from
  `EmulatorSettings.GetPresentMode()` (emulator_settings.h:30). The launcher writes it from
  `present_mode` (launcher/sotc_launcher_win.py:625).
- **Change:** none in code. Run the same route in Mailbox and Fifo, and compare the P1 timers.
- **Why:** in FIFO, `vkQueuePresentKHR` blocks when the image queue is full, and it does so while
  holding `submit_mutex`. Mailbox never queues behind the display, so it should not block there.
- **What could break:** Mailbox can show a dropped image if two presents land in one vblank (it
  replaces the queued image). Some drivers do not offer Mailbox and fall back to FIFO with a warning.
  Latency is usually lower in Mailbox, and there is no tearing in either mode.
- **Verify:** the P1 timers, SOTC FPS line, and the frame-time histogram of `SOTC FRAMES` lines.
- **Signature:** not applicable.
- **Note:** the plan says the user runs Fifo, while the saved settings say Mailbox. Confirm which one
  the measurements used before comparing.

### P3. A larger presentation ring (new cvar `SOTC_PRESENT_FRAMES=N`)

- **Where:** `Presenter::Presenter`, `num_frames` (vk_presenter.cpp:513-524). Today it is `num_images`
  unless DLSS-G or FSR 3 is on, which use `max(num_images, 4)`.
- **Change:** `num_frames = max(num_images, N)` when the cvar is set (default unchanged). Each extra
  frame is one display-size image (about 8 MB at 1920x1080, RGBA8), plus its present fence.
- **Why:** `GetRenderFrame` can only wait for a free frame when all frames are in flight. The game
  keeps at most about two flips in flight (section 2.2), so with 3 or 4 frames the free-frame wait
  should be rare. If P1 shows that the wait is the `present_done` fence instead, a larger ring still
  helps: the reused frame is older, so its fence is more likely to be signalled.
- **What could break:** more frames means the draw thread can run one flip further ahead of the
  present thread, adding up to one frame of latency only when the present thread is behind. The
  present thread still takes one request per tick, so the steady state is unchanged. A bug in
  `RecreateFrame` on a window resize would show up as a frame size mismatch; the code already handles
  this for the DLSS case.
- **Verify:** P1 timer (a) goes to 0. Latency is checked with a button-to-screen test or with the
  `SOTC TRACE` present marks (`SotcTrace`, present scope).
- **Signature:** not applicable.

### P4. Release the early VO label before `GetRenderFrame` (port change)

- **Where:** `VideoOutDriver::SubmitFlipInternal` (driver.cpp:2408-2440). Move the `EarlyVoLabel` block
  (`buffer_labels[submit_prev_index] = 0; SignalVoLabel(); NotifyCpWait()`) to just before
  `presenter->PrepareFrame`.
- **Change:** the label of the previous buffer is cleared as soon as the flip reaches the draw thread,
  not after its frame has been obtained.
- **Why:** the game's next `InsertWaitFlipDone` (the CP's final pass into that buffer) waits on this
  label. Today it also waits for `GetRenderFrame` of the current flip. With the move, the CP is released
  while the draw thread is still waiting for a free frame. The game's draws into that buffer are queued
  after the flip op on the same ordered queue, and the previous buffer's copy was already recorded by
  the previous flip, so the data order is unchanged.
- **What could break:** the argument above assumes that the previous flip's copy is recorded before any
  later game write, and that all those writes go through the same ordered draw queue. If that
  assumption is wrong, the result is corrupted frames (not a hang). Check for corruption on the first
  flip after a buffer reuse, and run the existing `SOTC_FPS` stats.
- **Verify:** the `CP wait` / `label` split (`SOTC FPS` and `SOTC STALL` lines) shows the CP no longer
  waiting at the final pass during the draw stall. Visual check on heavy areas.
- **Signature:** not applicable.

### P5. Non-blocking flip: repeat the last frame when no frame is free (port change, larger)

- **Where:** `SubmitFlipInternal`, before `PrepareFrame`: if `free_queue` is empty (checked without
  waiting), skip `PrepareFrame` and queue a droppable present of the last frame. The label release of
  P4 stays.
- **Change:** the draw thread never blocks on a free frame. It drops that flip's image and shows the
  previous one.
- **Why:** removes the draw-thread stall. The game's buffer discipline is kept because the label is
  still released.
- **What could break:** a visible judder (one repeated frame) whenever the presenter falls behind. The
  existing `PrepareLastFrame` waits on `present_done` of the last frame, so it must not be used as is
  (it could block the draw thread again). The repeat should go to the present thread, like the existing
  droppable blank frames (`RunPresentJob(..., true)`).
- **Verify:** count the dropped flips (`SOTC FPS` line); check that the draw wait goes to 0 and the
  dropped count stays low (under 1 per second in heavy areas) at 30 FPS.
- **Signature:** not applicable.

### P6. Display-locked pacing (existing cvar `SOTC_PRESENT_WAIT=1`, off by default)

- **Where:** `PresentThread` (driver.cpp:2443-2600 region) and `Swapchain::FindPresentMode` (forces FIFO).
- **Change:** none. The flip is held until an earlier present is on screen, with a 100 ms fallback.
- **Why:** it can remove judder from software-timer jitter at 120 Hz against a 60 Hz display. It does not
  remove a draw-thread stall, and it forces FIFO, which P2 suggests is the bad case for the mutex.
- **What could break:** extra latency of up to `k` presents; the 100 ms fallback still shows the
  judder it was meant to remove when it fires.
- **Verify:** frame interval histogram of flips on the display (not the software timer).

### P7. CP-side flip pacer (existing `SOTC_CP_FLIP_PACE=1`, already on in the user's settings)

- **Where:** `Presenter::PaceFlip` (vk_presenter.cpp:1778), called from `liverpool.cpp:3173`.
- **Change:** none. It moves the wait from the draw thread to the CP.
- **Why:** it does not shorten the wait. The CP is also serial (round-1 report 01: about 15 ms of work
  per frame), so the coupling stays. Keep it only if P1 shows the wait is really in the draw thread.
- **What could break:** the CP stalls instead of the draw thread, which can be worse for the readbacks
  that go through the CP.

### Game-side options

No game hook is needed for the flip path, because the game has no CPU flip wait. These are the sites
that were checked:

- **G1, measurement only (Site, log mode first).** `Gnmx_InsertWaitFlipDone_Emit` at 0x86f700, unique
  signature `55 48 89 e5 41 57 41 56 53 50 48 89 fb 41 89 d6 41 89 f7 48 8b 7b 10 48 8b 43 08 48 29 f8
  48 c1 e8 02 83 f8 07 73 ?? 48` (40 bytes, checked with `eboot_re.py sig`). Log the buffer index and the
  CP's cycle count per call. It counts how often the final pass is gated, and which slot. Verify with a
  `=2` mode (log only, no change to the code stream). Value: confirms section 2.3 point 3 and the one-flip
  coupling.
- **G2, rejected for now (Site on `sub_86f700`, making the wait a no-op).** That would let the game render
  into a buffer still being displayed. Expect tearing or corruption. Not proposed.
- **G3, rejected (buffer count).** `RegisterBuffers(..., 2, ...)` at 0x958ae could become 3, but the slot
  index comes from `FUN_90ea0` (`% 2`), so the game's own ring would have to change. The index logic is
  not fully traced, so this is **unknown** and not proposed.
- **G4, rejected (flip-rate setter).** `FUN_95ef0` (unique signature `55 48 89 e5 53 50 48 89 fb 48 8b
  43 30 39 f0 75`, 16 bytes) already gives the rate the port applies. `SOTC_FPS_CAP` covers the cap
  without a game hook.
- **G5, observed, optional (Site at `RenderBackend_Frame` 0x90ea0, signature 28 bytes unique).** Its CPU
  spin on the slot EOP label burns a core while the GPU is behind. A `sched_yield`-style change is
  possible, but it does not shorten the wait, so it only helps CPU load.

## 4. Open questions

1. Which present mode and which `SOTC_CP_FLIP_PACE` / `SOTC_FLIP_SERVICE` settings were active for the
   plan's 29/10 measurements? The saved settings say Mailbox, the plan says Fifo, and the file has
   `SOTC_CP_FLIP_PACE=1` and `SOTC_FLIP_SERVICE=1`.
2. Which of the three waits (free frame, `present_done`, submit mutex) dominates the 25 ms? P1 answers it.
3. How the VSync window (`FUN_25f480`) feeds `FUN_245440` / `FUN_95ef0`: is the output the flip rate, or
   only the fps mode for the game's own timing? Not traced to the end.
4. The meaning of the `7` argument of `sceGnmInsertWaitFlipDone`, and the source of the slot index
   `*(DAT_02ed33f0+0x7b0)`.
5. Whether the previous flip's copy is recorded before any later write, for P4 (needs a read of
   `PrepareFrame`'s copy and `texture_cache.UpdateImage`).
6. Whether FIFO `vkQueuePresentKHR` actually blocks on the user's driver while holding
   `submit_mutex`. Only measurement (P1) answers it.
7. Whether the game's frame rate is capped by the flip alone, or also by the submit gate
   (`sceGnmSubmitDone`, round-1 report 08). Not separated.

## 5. Gain estimates

- **P1:** no gain by itself.
- **P2 (present mode):** unknown until P1 is in. If the mutex theory holds, the wait in heavy areas
  (3-10 ms in the plan) could go to near zero in Mailbox.
- **P3 + P4:** the draw-thread stall can shrink by the free-frame share of the wait. The share is
  unknown; the plan's total is 3-25 ms per frame, so up to that amount is the ceiling.
- **P5:** removes the draw-thread stall entirely, at the price of dropped flips. Expect a few drops per
  second in the worst areas. Gain is unknown until a count exists.
- **P6, P7:** no direct gain on the wait; possible judder or CPU-side effects.
