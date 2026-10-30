# 05 - Frame pacing: how a slow frame reaches the player

Labels: **observed** (disassembly, Ghidra pseudocode or a log line, with address or file),
**inferred**, **unknown**. Addresses are eboot image offsets (EU 1.01, fingerprint 72c1d4a9...).
Port log addresses are host addresses: image offset = address - image base. The base is printed
in each log ("Image at ..."); the `SOTC USLEEP` addresses were mapped with the base of the same run.

## 1. Answer

- **observed:** the game loop (`GameCoreLoop::GameLoop`, 0x25c080) has no sleep and no wall-clock
  pacing of its own. Each iteration calls the dt consumer `sub_25ccc0`, then the update, then
  the render. The loop is paced by the render/flip waits, not by a timer (**inferred** from the
  absence of any timed wait in 0x25c080 and from report 08).
- **observed:** the step handed to the simulation in GameLoop is a fixed value
  `DAT_02eee7b0 = (float)N / rate` (0x25c080, decompile line 326). It is passed to `FUN_00a3f140`
  (`sub_25bb30`, line 37) and `FUN_003c0df0` (`sub_25d8b0`, line 161). It is set to 0 when
  paused. So the step passed there does not depend on wall time.
- **observed:** the real-time dt is computed in `sub_25ccc0` (0x25ccc0-0x25ce7e) from the TSC
  timer. It is clamped to `gameMaxDeltaTime` (default 0.1 s, object 0x2eecd10, float 0x3dcccccd,
  registered at 0x263542). `gameDeltaTimeCorrectionTolorance` (default 0.5, object 0x2eecde8,
  0x3f000000, registered at 0x263652) multiplies the fixed step to form the correction quantum.
- **Correction to report 08 and STUTTER-ARCHITECTURE section 1:** the claim that "the simulation
  takes the whole hitch in one step" is **not confirmed**. The step passed to the update in
  GameLoop is fixed. The update thread (`ShadowGameUpdate_ThreadFunc`, 0x534690) reads its dt
  from a float in the time object at GameCoreLoop+0x3b78 (offset +0x48, read at 0x534a73). I did
  **not** find the writer of that float (**unknown**). That float decides whether a hitch is
  slow motion or a jump. This is the main open question.
- **observed, odd:** if `FixedTimeStep` is on (its registered default), the consumer's output,
  traced by hand from the disassembly, keeps growing with real time until it hits the cap.
  Section 2.3 has the trace. Either the active path is not this one at runtime, or I misread the
  carry. It must be settled by a probe before any dt hook is trusted (hook H1).
- **observed:** `SOTC USLEEP` in the port's logs shows about 268 `usleep(1)` calls per second
  (5 s window, c14_base), and each one really takes **0.52 ms** (695.8 ms of sleep for 1,343
  calls; requested 2.3 ms). That is about 14% of wall time summed over the threads that call it.
- **observed:** the sleeps come from three places, in this order: the job-system counter wait
  (`JobSystem_WaitForCounter`, 0x35590, about 49% of calls), the allocator spin lock (about 45%:
  `pool_alloc_block_wrapper`, `sub_18920`, `pool_alloc_region_aligned`, `BpeHeap_Free`, ...), and
  a few others. Every site is a spin of about 1,000-10,000 `pause` followed by `usleep(1)`.
- **Cheapest win, no hook:** `SOTC_USLEEP_YIELD=1` already exists (`runtime_kernel.c`). It turns
  every `usleep` of 10 us or less into `SwitchToThread`. Most of the 268 calls/s are `usleep(1)`.
  The gain is the 0.5 ms wake floor per wait, at most about 2.3 ms per frame at 60 fps if the
  waiter is on the critical path (**unknown** which thread that is; see open questions).
- **Dt levers (only after H1):** `gameMaxDeltaTime=0.05` through `sotc_modes.py --cvars` caps
  the damage of one hitch at 50 ms of game time instead of 100 ms. It does nothing for normal
  frames under 50 ms. It does change the game speed during a hitch.
- **Do not start with:** removing the update/render handshake (report 08 option B). It is the
  largest possible change to physics and animation state and it has no measured gain yet.

## 2. Evidence

### 2.1 Game loop (GameCoreLoop::GameLoop, 0x25c080)

- **observed:** the per-iteration order is `PreProcessGameLoop` profile, then
  `FUN_0025ccc0(&timer, singleStep)`, then `FUN_0025ce80(&timer)` (the message/update queue),
  then "ProcessGameLoop" (`FUN_0025d8b0`, `FUN_00a817c0`, `FUN_00a81fc0`), then "Render"
  (`FUN_002610c0`). The loop ends at `*(char*)(DAT_02ee6778+8) != 0` (quit).
- **observed:** `singleStep` (string 0xfc355d, storage DAT_02eecca8) is the only input that sets
  `cVar5` to 1 (the "reset" argument of the consumer). In normal play it is 0.
- **observed:** `FUN_001469d0` (`ResourceFactory::WaitForAllCmdsToFinish`) is called once after
  the loop, not per frame (report 05 agrees).
- **observed:** `DAT_02eee7b0` is set in GameLoop at line 326 from
  `*(int*)(DAT_02edf140+0x220350)` and `*(float*)(DAT_02ed3670+0x28)`.
  - The integer at 0x220350 is written by `Renderer_FrameTail` (0x118470, decompile line 252):
    `counter = (counter + 1) % N`. So N is a frame modulus.
  - **inferred:** `F = N / rate`, with `rate` the display or flip rate (the float at
    DAT_02ed3670+0x28 was not identified; N and the rate are **unknown** at run time).

### 2.2 Cvars (sub_262a80 and sub_2534c0)

| Cvar | Object | Default | Where | Note |
|---|---|---|---|---|
| gameMaxDeltaTime | 0x2eecd10 | 0.1 f (0x3dcccccd) | 0x263542, 0x26354c | storage at +0x60 -> +0xd0; read at 0x25ce1d |
| gameDeltaTimeCorrectionTolorance | 0x2eecde8 | 0.5 f (0x3f000000) | 0x263652 | read at 0x25cd9e (quantum = F * tol) |
| FixedTimeStep (bool) | GameCoreLoop+0x2868 | 1 (registration writes 1) | sub_2534c0 | string 0xfc3885; read at 0x25cd70 |
| MaxUpdatesPerFrame (int) | GameCoreLoop+0x2930 | 1 | sub_2534c0 | string 0xfc3893; read at 0x25cd0b; a constant 10 is passed to the same call (meaning **unknown**) |

- **observed:** `sotc_modes.py --cvars "NAME=VALUE ..."` writes these into `out/cmdline.bin`.
  The syntax accepts `NAME=0.05`, so a cvar override needs no code.

### 2.3 The dt consumer `sub_25ccc0` (0x25ccc0-0x25ce7e), instruction level

Names: `T` = timer object (GameLoop `local_5f0`, time read by `FUN_000324e0`); `prev` = T+0x18;
`carry` = +0x2c; `acc` = +0x30; `dt` = +0x28; `cnt` = +0x40; `F` = DAT_02eee7b0; `Tol` =
tolerance cvar; `C` = gameMaxDeltaTime.

1. (0x25ccd3-0x25ccf1) `now` = TSC seconds (read twice).
2. (0x25cd0b-0x25cd14) `T+0x3c` = MaxUpdatesPerFrame when FixedTimeStep is on, else 1.
3. (0x25cd1a-0x25cd2e) if `cnt == 0`: `prev = now - F`.
4. (0x25cd33-0x25cd4f) if `carry < -F`: `carry += F`.
5. (0x25cd54-0x25cd66) `cnt++`; `x = carry + (now - prev)`; `dt = x`; `prev = now`.
6. (0x25cd70-0x25cdf3) **if FixedTimeStep:** `acc = (x - F) + acc`; `dt = F`; `x = F`;
   `q = F * Tol`; while `acc >= q`: `acc -= q, x += q`; while `acc <= -q`: `acc += q, x -= q`;
   `dt = x`; `acc` stored.
7. (0x25cdf8-0x25ce06) if the reset argument is set: `dt = F`, `acc = 0`, `x = F`.
8. (0x25ce0d-0x25ce2b) `if x > C: x = C, dt = C` (the clamp).
9. (0x25ce37) `carry = x`.
10. (0x25ce3c-0x25ce6f) stats: the sum of F and of real time.

**Trace I did by hand from steps 5-9 (inferred, from the instructions above):** in FixedTimeStep
mode, `x` at the start of a call is `carry + elapsed`, and step 9 stores the output back into
`carry`. Step 5 then adds it again. With elapsed equal to F every frame, the output sequence is
F, 2F, 3F, ... until the clamp at C, after which it stays at C. At 60 fps that is about 100 ms
of game time per real frame. That contradicts normal play, so one of these holds, and each
needs a probe to tell apart:
- (a) FixedTimeStep is off at run time (a config or a launcher setting I did not find);
- (b) the field the update reads is not `dt` (+0x28) but the time object's +0x48 (section 2.4),
  and the consumer's output is only a stat;
- (c) I misread the carry in step 5.

### 2.4 Who reads the update's dt

- **observed:** `Shadow Game Update` (0x534690) reads `fVar1 = *(float*)(*(long*)(&DAT_002054e8
  + idx) + 0x48)` at 0x534a73 (decompile line 229). The pointer is set in GameLoop at
  `*(long**)(&DAT_002054e8 + local_988) = param_1 + 0x76f` (0x25c080, decompile lines 174, 184).
  `param_1 + 0x76f` (in longs) is GameCoreLoop+0x3b78, so the float is at **GameCoreLoop+0x3bc0**.
  The same float is read by `sub_25bb30` as `fVar3 = *(float*)(param_1 + 0x778)` (line 30).
- **observed:** `fVar1` is passed to `FUN_00535cd0`, `FUN_00535f20` (twice) and
  `FUN_0052b3b0` in the update (lines 231-338). It is the dt of the update.
- **unknown:** the writer of the float at GameCoreLoop+0x3bc0. My searches of the decompile for
  `0x3b78`, `0x3bc0` and `0x778` found no store. The time object is "Time Manager" (`sub_25b080`,
  called from 0x259b30; 7 `EGameTimeScaleGroup` groups, User_1 ...). Its layout puts the group
  scalars at +0..+0x18, group ids at +0x40, accumulated times at +0x60, so +0x48 is inside the id
  table. The update's index `lVar14` may point to a different object in that thread.

### 2.5 Sleeps: the `usleep(1)` sites (`SOTC USLEEP`, `runtime_kernel.c`)

- **observed:** `Sleep_Usec1` (0x3b570) is `jmp sceKernelUsleep`. The import has 95 direct call
  sites (`eboot_re.py icallers`); more than 60 functions call `Sleep_Usec1` (`callers`, list cut
  at 60 in my run). `GameLoop` (0x25c080) has no sleep callee.
- **observed:** the port's top callers are return addresses. Each return address is the call
  site + 5 (checked for every row below). Base 0x18240000 is the c14_base run; the other runs
  have their own base and give the same offsets.

| Log | Return address | Offset | Function | Lock or loop | Hits in 5 s |
|---|---|---|---|---|---|
| c14_base | 0x182756d7 | 0x356d7 | `JobSystem_WaitForCounter` (0x35590) | spin 1000x `pause`, then `usleep(1)`; `bl` argument chooses spin-only at 0x35700 | 659 |
| c14_base | 0x18259b4f | 0x19b4f | `pool_alloc_block_wrapper` (0x19ae0) | `lock cmpxchg` on global 0x2dfc8a0; spin 10000, then `usleep(1)` each try | 481 |
| c14_base | 0x18258987 | 0x18987 | `sub_18920` | `lock cmpxchg` on [obj+0x70]; same shape | 123 |
| c14_base | 0x18258849 | 0x18849 | `sub_187e0` | same | 53 |
| c14_base | 0x1825a6df | 0x1a6df | `pool_alloc_region_aligned` (0x1a660) | global 0x2dfc8a0 (shared with pool_alloc_block_wrapper) | 22 |
| c14_base | 0x1825b13f | 0x1b13f | `pool_free_block` (0x1b0e0) | same | 3 |
| c14_base | 0x18289439 | 0x49439 | `BpeHeap_Link` (0x493d0) | same | 1 |
| c14_base (2nd line), run_s02 | 0x182cd7d9 / 0x17e9d7d9 | 0x8d7d9 | `sub_8d770` | `lock cmpxchg` on [r12] | 136 / 174 |
| c14_base (2nd line), run_s02 | 0x18288ce9 / 0x17e58ce9 | 0x48ce9 | `BpeHeap_Free` (0x48c80) | same | 119 / 197 |
| c14_base (2nd line), run_s02 | 0x18257aa9 / 0x17e27aa9 | 0x17aa9 | `sub_179c0` | `lock cmpxchg` on [r13] | 66 / 51 |
| c14_base | 0x18257d79 | 0x17d79 | `sub_17d20` | `lock cmpxchg` on [r13] | 56 |
| c14_base | 0x182542a9 | 0x142a9 | `sub_14240` | same family | 9 |

- **observed:** the `WaitForCounter` loop (0x356a1-0x356f1): `cmp rbx,0x3e8; cmova rbx,1`, so
  the `usleep(1)` (0x356d2) runs about once per 1,000 `pause`. `bl` = 0 gives a pure spin with no
  `pause` (0x35700). Its callers include the render-side waits `RunFunctionAsRenderJob`
  (0xc130d, 0xc1813), `KickoffSubmitJob` (0xd0816), `BPXPR_RunCullPasses` (0xc669d...),
  `RenderView::MainCamera` (0xcb34c...), and the loader `ResourceFactory::ThreadFunc` (0x1465cc)
  and `ProcessLoadingThreadLoop` (0x148b94). The value of `bl` at each caller is **unknown**.
- **observed:** the allocator locks (0x19ae0, 0x18920, 0x187e0, 0x1a660, 0x179c0, 0x17d20,
  0x8d770, 0x48c80) spin on a `cmpxchg` lock word with `pause`, then `usleep(1)` after 10,000
  tries (`cmp rbx,0x2711`). The lock is held by whoever is inside the allocator; nothing in the
  log says who that is in the slow frames (**unknown**; report 03 suggests the write-fault path).
- **observed, measured:** `SOTC USLEEP` in c14_base: `calls=1343 (268/s) ... <=10us=1342 ...
  requested=2.3 ms actual=695.8 ms`. So each `usleep(1)` took 0.52 ms. `runtime_usleep_stats`
  counts only the time inside `usleep_guest`, not the spin before it.
- **observed:** `compat_sleep_ns` (`compat_win.c:115-127`) uses a high-resolution waitable timer.
  The floor of about 0.5 ms is the timer, not `timeBeginPeriod`.
- **observed:** `SOTC_USLEEP_YIELD` (`runtime_kernel.c:141-180`): mode 1 yields for requests of
  `SOTC_USLEEP_YIELD_US` (default 10) us or less; mode 2 only counts.

### 2.6 Pacing

- **observed:** `Frame::InsertWaitFlipDone` uses the "VBlank" event (string 0xfa6643). The render
  side waits on flip-done through `sub_91f50` / `sub_86f700` (report 08).
- **observed:** the port caps flips in `PresentThread` (report 08, `sotc_fps_cap.h`).
- **inferred:** the 60 Hz pacing comes from the flip waits. No timed sleep exists in GameLoop.

## 3. Hook proposals

Each hook is declared in `scripts/hooks.d/<name>.py` (`docs/HOOK-DESIGN.md`). Signatures were
generated with `tools/re/eboot_re.py sig`. "Unique" means the tool reported one match.

### H1. Dt probe (measure only)

- **Site:** `sub_25ccc0`, entry 0x25ccc0, mode **wrap** (pre and post).
- **Signature:** `55 48 89 e5 41 57 41 56 53 48 83 ec 18 48 89 fb 41 89 f6 4c`, 20 bytes,
  **unique** (checked with `eboot_re.py sig 0x25ccc0`).
- **Change:** none. Log per call: TSC now, the input carry (+0x2c), dt (+0x28), acc (+0x30),
  cnt (+0x40), updates (+0x3c), the FixedTimeStep byte (the bool behind GameCoreLoop+0x28c8),
  the reset flag and whether the clamp fired (x > C).
- **Why it helps:** settles section 2.3 (which of (a), (b), (c) holds) and whether a hitch is
  slow motion or a jump. Everything below depends on it.
- **What could break:** nothing, if the handler only observes. It costs a few hundred ns per
  call, once per iteration.
- **Verify:** `=2` mode runs the original too and compares the outputs (must match).

### H2. Update-dt probe (measure only)

- **Site:** `ShadowGameUpdate_ThreadFunc` at 0x534a73, mode **site**, read-only.
- **Signature:** `c5 fa 10 40 48 c5 fa 11 44 24 18 e8`, 12 bytes, **unique**.
- **Change:** none. Log the float at `[rax+0x48]` each update, with the thread name.
- **Why:** shows the value the update really uses. If it equals F every iteration, hitches are
  slow motion and the dt cap is irrelevant. If it jumps to C, the cap is the jump size.
- **What could break:** nothing (read only).
- **Verify:** `=2` compares against H1's dt.

### H3. Dt cap (cvar, no hook, after H1 and H2)

- **Change:** `sotc_modes.py --cvars "gameMaxDeltaTime=0.05"`.
- **Why:** only if H2 shows the update uses the clamped real-time dt. It halves the largest jump
  from one hitch (100 ms to 50 ms of game time). It has no effect on frames under 50 ms, so
  60 fps and 30 fps play are untouched.
- **What could break:** the game runs slower during a hitch, not faster. Anything tied to game
  time moves late: cutscene timelines, animation events, enemy timers. Audio is probably on a
  separate clock (**unknown**), so a long hitch can desync sound by up to the lost time.
- **Verify:** count frames where H1 reports the clamp fired, before and after. A save with
  a known hitch at 60-68 s (report 09) is the repro.
- **Expected gain:** unknown. It only matters if hitches reach the update as jumps.

### H4. Dt smoothing (engine, entry-exit wrap on the consumer)

- **Site:** 0x25ccc0, mode **wrap**. Post phase rewrites the dt at +0x28 to
  `min(x, k*F)` with k = 1.5, not C. Hold the excess in `acc` (+0x30) so no time is lost.
- **Why:** caps the step at a size the physics was tuned for, spreads the excess over later
  iterations.
- **What could break:** the sim loses its real-time match on long stalls; a sustained slow frame
  makes the game run slow for longer. Physics (ODE: `dJointCreateFixed` is in the eboot) and
  animation blend times assume a stable step. Test with ragdolls and jumps.
- **Verify:** `=2` runs the original and logs both values. Compare the step histogram.
- **Expected gain:** unknown. Only useful after H1 and H2 show real jumps.

### H5. Sleep to yield, existing switch (no hook)

- **Change:** `SOTC_USLEEP_YIELD=1` (threshold `SOTC_USLEEP_YIELD_US=10`, the default). Pin the
  port's CP and draw threads with `SOTC_CPUSETS=1` so a spinning guest thread does not take
  their core.
- **Why:** removes the 0.5 ms wake floor from the about 268 calls per second. Upper bound about
  2.3 ms per frame at 60 fps, if the waiting thread is on the critical path.
- **What could break:** CPU use. The `SwitchToThread` loop burns a core while it waits. Starvation
  of the CP or draw thread if they share a core.
- **Verify:** `SOTC_FPS=1` plus `SOTC_USLEEP_YIELD=1`. The `actual=` value of the USLEEP line
  should drop to about the spin time, and `calls=` should be the same. Compare the 5 s frame
  time histogram, same save and route.
- **Signature:** not needed (import binding, `runtime_kernel.c`).

### H6. WaitForCounter sleep (engine site)

- **Site:** `JobSystem_WaitForCounter`, the `call Sleep_Usec1` at 0x356d2 (the `pause` after it is
  at 0x356d7). Mode **site**: replace the call with a `SwitchToThread`-equivalent (or nop it so the
  loop only spins).
- **Signature:** `bf 01 00 00 00 e8 ?? ?? ?? ?? f3 90`, 12 bytes, **unique** (checked for 0x356cd).
- **Why:** this is the largest single site (659 of 1,343 calls). The render-side counter waits
  (`RunFunctionAsRenderJob`, `KickoffSubmitJob`) wake up without the 0.5 ms floor.
- **What could break:** a nop'd sleep turns a waiting render thread into a core-burning spin. If
  the port's draw thread shares that core, the wait gets slower, not faster. Also, the `bl`
  argument (0 = pure spin) is unknown at each caller, so changing the sleep changes both paths.
- **Verify:** `=2` mode runs the original and logs the wait time per call (both must finish in
  the same counter value). The counter of hits at 0x356d2 must drop to 0.
- **Expected gain:** up to about 0.5 ms per counter wait. Unknown per frame.

### H7. Allocator lock sleep (engine sites, 8 of them)

- **Sites:** the `call Sleep_Usec1` in `pool_alloc_block_wrapper` (0x19b4a), `pool_alloc_region_aligned`
  (0x1a6da), `sub_18920` (0x18982), `sub_187e0` (0x18844), `sub_179c0` (0x17aa4), `sub_17d20`
  (0x17d74), `sub_8d770` (0x8d7d4), `BpeHeap_Free` (0x48ce4). Mode **site** each, or one **wrap**
  at the lock functions.
- **Signature:** `bf 01 00 00 00 e8 ?? ?? ?? ?? eb ?? 0f ae e8 48 8d 7d c8 e8 ?? ?? ?? ?? 44 8b 4d c0`,
  28 bytes, **unique** at 0x19b45 (checked). The other six are **not checked**; run
  `eboot_re.py sigfind` before use. `pool_alloc_region_aligned` (0x1a6da) shares lock 0x2dfc8a0
  with `pool_alloc_block_wrapper` (0x19b45), so one lock covers both sites.
- **Why:** the allocator is the largest sleep source (about 45% of calls, about 0.25 s per 5 s in
  c14_base).
- **What could break:** the lock holder may be inside a write-fault or readback (report 03). A
  longer spin then burns the CP or draw core while the holder runs. A yield could let the holder
  run; a plain spin would not.
- **Verify:** `=2` mode runs the original and checks that the lock is held by the same owner when
  the spin ends. Count the hits.
- **Expected gain:** unknown. Depends on how often the allocator lock is on a frame's critical
  path, which is an open question.

### H8 (not recommended yet). Update one frame ahead

- **Site:** the second update wait at 0x53496d (report 08 option B), mode **site**.
- **Why:** lets the update start the next frame while the render works on the last one.
- **What could break:** physics, animation and audio read render-side data. High risk; no
  measured gain.
- **Verify:** only after H1 and H2 show the update's dt and whether the render's output is read
  back by the update.

## 4. Open questions

1. **Who writes the float at GameCoreLoop+0x3bc0 (section 2.4)?** It decides whether a hitch is
   slow motion (step = F) or a jump (step = clamped real dt). Settle it with H2, then find the
   writer with a write watch on that address during a hitch.
2. **Is FixedTimeStep on at run time?** The registration default is 1; I found no launcher or
   `sotc_modes.py` setting for it. H1 logs the byte.
3. **Is the carry in `sub_25ccc0` really fed back (section 2.3)?** H1 will show it.
4. **N and the rate (section 2.1):** the integer at 0x220350 and the float at DAT_02ed3670+0x28.
   Both set F. The 60 FPS mode may change them.
5. **Which thread calls each `usleep(1)` site?** The `SOTC USLEEP` line has no thread name. Add
   the thread name to the top list (small port change, observation only).
6. **Callers of `JobSystem_WaitForCounter` with `bl = 0` (pure spin, 0x35700)** and the meaning of
   `bl`. Needed before H6.
7. **Who holds allocator lock 0x2dfc8a0 during a slow frame?** Needed before H7.
8. **Who calls `sub_25bb30` (from `FUN_00b11280`) and how often?** It also reads F and the dt float.
9. **What does `MaxUpdatesPerFrame` do?** The update path reads it into `T+0x3c`; no reader was
   found in the loop. The second argument (10) in its registration is unexplained.
10. **Audio clock:** whether sound runs on the game clock or on real time. Decides the cost of H3.

## Summary of the best proposals, ranked by expected gain

1. **H5** (existing switch): `SOTC_USLEEP_YIELD=1` with `SOTC_CPUSETS=1`. Up to about 2.3 ms per frame, no code.
2. **H1 + H2** (probes, no behaviour change): settle the dt question first; they gate H3 and H4.
3. **H6** (WaitForCounter, unique site 0x356d2): removes the largest single sleep, 659 of 1,343 calls.
4. **H7** (allocator lock, 8 sites): the rest of the sleeps; check the six unverified signatures first.
5. **H3** (`gameMaxDeltaTime=0.05`): halves the largest jump from one hitch, only if H2 shows jumps.
6. **H4** (dt smoothing): only if H2 shows jumps and H3 is not enough.
7. **H8** (update one frame ahead): high risk, no measured gain; last.
