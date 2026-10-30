# 16 - BPE job system: scheduler lock, worker waits and the PC port

Read only. Nothing built, run or edited except this file. Addresses are eboot VAs (EU 1.01, `out/eboot.elf`,
decompiles in `out/re/decomp/0003/`) unless marked "port" (file:line in the working tree). Labels: **observed**
(decompile, source line or log line), **inferred**, **unknown**. Read first: reports 01, 02, 07, 08, 09 and
`docs/sotc/PLANO-THREAD-DESENHO.md` (sections 1.2, 1.3 and the all-threads profile).

## 1. Answer

- **observed:** one scheduler object serves 4 worker threads "BPE JobWorkerThread CPU1..CPU4". `JobSystem_Initialize`
  (0x335d0) creates one thread per set bit of the mask at `param_2+0x30`, at priority 700 with that bit as affinity.
  The port logs the four creations.
- **observed:** all queues, lists and wake decisions of the scheduler sit behind one recursive mutex ("CritSec",
  `scheduler+0xb80`, created with type 2 at 0x3b310). Every idle worker takes it in `DoJobScheduling` (0x38700), and
  every enqueue takes it too (0x35250, 0x38550). This is the lock behind the 33% in the profile.
- **observed:** the lock is held for more than the queue pop. Inside it the game runs the job's predicate callback
  (0x38700), the counter completion `FUN_00038030` (which calls `EventFlag_Set` on waiting workers), and, on enqueue,
  one `EventFlag_Set` per allowed worker (0x38550). **inferred** that this makes the hold long; it is not measured.
- **observed:** a worker sleeps on its own event only when `CanWorkerSleep` (0x39cc0) is true. That needs empty worker
  lists, no job array waiting on a counter (`+0xc10`, `+0xc20`) and an empty `+0x2038` list. While a counter wait is
  pending, idle workers do not sleep: they spin on `scePthreadYield`, which is SwitchToThread on the port.
  **inferred** that this costs up to 4 busy threads during render waits.
- **observed:** the render side waits for job counters in `WaitForCounter` (0x35590): 1000 spins, then, when its mode
  flag is set, `sceKernelUsleep(1)` per poll. The port turns that into a high-resolution timer sleep. One log window
  shows 1,343 such calls that slept 695.8 ms in total (about 0.52 ms per call). About half of those calls come from
  0x356d7, inside 0x35590 (**inferred**, from the image base of that run).
- **observed:** the port records guest affinities and priorities and applies neither. `thread_set_affinity` and
  `attr_set_affinity` only store the mask (`src/runtime_thread.c:267`, `:399`), `set_sched_param` is a no-op
  (`src/runtime_sotc.c:280-283`), and `create()` (`src/runtime_thread.c:326`) never calls a Windows affinity or priority
  API. The workers float over all 16 logical CPUs of this machine (Ryzen 7 5700X, 8C/16T, from the session log).
- **Ranking (expected gain, honest):** (1) an adaptive spin in the port's `mutex_lock` before the blocking timed lock
  (port only); (2) test the existing `SOTC_USLEEP_YIELD=1` on counter polls; (3) a short spin window before a worker
  sleeps (Wrap on 0x39cc0). Affinity as Windows CPU sets comes next. More workers is **not** recommended: the lock is
  the limit, not the thread count.
- **Read faults inside jobs** cannot be removed on the job side. Readbacks are "Precise" by design
  (`BB_READBACKS` defaults to Precise, `gpu/shim/core/emulator_settings.h:36`): a job must get the bytes the GPU just
  wrote. The 24% can only shrink with a shorter GPU queue or with the fence knobs in section 3 (P6).

## 2. Evidence

### 2.1 The job system in the eboot

| Function | Address | What it does (observed) |
|---|---|---|
| JobSystem_Initialize | 0x335d0 | Allocates the scheduler object, then creates one thread per set bit of `param_2+0x30` through `FUN_0003b6a0` (prio 700, affinity = bit). Names come from 0x34230 ("BPE JobWorkerThread CPU%s"). |
| WorkerThread_StartFunc | 0x36140 | Takes `+0xbc0`, then runs the first fiber (`sceFiberRun`, 0x3bbb0). |
| JobSystemScheduler_DoJobScheduling | 0x38700 | Outer loop: `WorkerIdleWait` (0x38b40), lock `+0xb80`, fetch (0x38d40), predicate, completion (0x38030), unlock, then `ExecuteJob` (0x39800) outside the lock. |
| JobSystemScheduler_WorkerIdleWait | 0x38b40 | Scans the lists without a lock. If nothing is runnable: `CanWorkerSleep`; if true, waits on its own PS4SyncEvent (0x3b4a0, mode 0x11). If false, `scePthreadYield`; after 10 s (10 x TSC frequency) a deadlock check and `usleep(1 ms)`. |
| JobSystemScheduler_CanWorkerSleep | 0x39cc0 | False if any worker list is non-empty, if `+0xc10` or `+0xc20` is set, or if `+0x2038` is set. Takes `+0xc30` and `+0x1480`. |
| JobSystemScheduler_ExecuteJob | 0x39800 | Stamps the start time at `+0xd88`, then runs the job. Not under `+0xb80`. |
| JobSystemScheduler_CheckForDeadlock | 0x39de0 | Takes `+0x1480`, `+0xb80`, `+0xc30` in that order; prints "JobArray waiting on counter". |
| KickJobArray and enqueue | 0x35250, 0x38550 | Enqueue under `+0xb80` (0x35250 unlocks it). 0x38550 calls `EventFlag_Set` (0x3b450) for each allowed worker. |
| JobSystem_WaitForCounter | 0x35590 | Spins 1000 times. Then, only when `param_5 != 0`, `sceKernelUsleep(1)` per poll; otherwise a fiber-suspend loop (0x37500). |
| CritSec mutex init | 0x3b310 | `pthread_mutexattr_settype(2)`, which is recursive. |

Sources: `out/re/decomp/0003/*.c`; function names in `tools/re/names.txt`; the unique patterns come from
`tools/re/eboot_re.py sig` (section 3).

### 2.2 Who takes which lock

- `+0xb80` (scheduler, "CritSec"): the DoJobScheduling loop, enqueue (0x35250, 0x35450, 0x38550), CheckForDeadlock.
  It is held across the scan (0x38d40), the predicate and the notify (0x38030).
- `+0xbc0`: worker start (0x36140), 0x33550, 0x37500, and 0x39610 (the fast path that tries a job).
- `+0xd80`: counter and job-array allocation (0x35250, 0x35bb0), held briefly.
- `+0xc30` and `+0x1480`: CanWorkerSleep and CheckForDeadlock only.

### 2.3 Port side

- `src/runtime_mutex.c:89-109` (`mutex_lock`): `pthread_mutex_trylock`, then on EBUSY `pthread_mutex_timedlock` in 5 s
  slices. There is no spin before the blocking call. The host is Windows with winpthreads. Whether its timed lock polls
  or waits on an OS object is **unknown**: no winpthreads source is in this checkout, and the msys64 package is binary
  only. Contended locks already emit a "mutex wait" trace span keyed by lock address (`trace_span`).
- `src/runtime_mutex.c:55-63` (`mutex_init`): type 2 maps to `PTHREAD_MUTEX_RECURSIVE`.
- `src/runtime_sotc.c:290-375` (event flags): one host mutex and one `pthread_cond` per flag. `evf_set` wakes waiters
  under the flag lock. Each worker has its own flag, so each enqueue costs one lock and one signal per allowed worker,
  and that happens inside the game's `+0xb80`.
- `src/runtime_kernel.c:141-160` (`usleep_read_config`): `SOTC_USLEEP_YIELD` (1 = yield, 2 = count only) and
  `SOTC_USLEEP_YIELD_US` (default 10). `usleep_guest` (199-216) yields only when mode is 1 and the request is at or below
  the threshold. The default sleeps. No result for this knob is recorded in the CHANGELOG.
- `src/compat_win.c:115-127` (`compat_sleep_ns`): high-resolution waitable timer, or `Sleep` as fallback.
- `gpu/shim/bbport_threads.h:171-197` (`PlaceThread`): CPU sets for the port's own threads only, and only with
  `SOTC_CPUSETS=1` (default off). Game threads are never placed.
- `src/runtime_fiber.c`: a fiber switch is a register swap on the same host thread. It is cheap and not a cost source.

### 2.4 Measurements

- **observed** (PLANO 1.2): 4 x `BPE JobWorkerTh`, about 14 ms CPU per worker per 33 ms frame.
- **observed** (PLANO, all-thread profile, 30 s to 2 min of play): workers 33% in the game mutex, about 25% in event
  waits, 24% inside read faults (`vectored_fault_impl` -> `ReadMemory`).
- **observed** (waveB session logs, `SOTC USLEEP`, several windows): 146 to 268 calls/s. One window has
  `requested=2.3 ms` and `actual=695.8 ms` for 1,343 calls, about 0.52 ms per call. Its top caller offsets are 0x356d7
  (659 calls), 0x19b4f (481), 0x18987, 0x18849, 0x1a6df and 0x1b13f. With that run's image base (0x18240000), 0x356d7
  is inside WaitForCounter (0x35590) and 0x19b4f is `pool_alloc_block_wrapper` (0x19ae0, the allocator).
  **inferred**: the base comes from the matching `Image at` log line, not from the USLEEP line.
- **observed** (report 09): the worst single spike, 1,437 ms, is a BPE worker write fault that waits 1,411 ms on a
  readback, with CP, draw and GPU idle.
- **observed** (`gpu/shim/bbgpu.cpp:416-447`): a read fault goes through `bbgpu_handle_fault` ->
  `DispatchAccessViolation` -> readback. The readback waits for the work it depends on (`WaitUpTo`), so the faulting
  worker waits for the GPU side. `SOTC_RB_ASYNC` is on by default; `SOTC_FENCE_PIECES` is off.
- **unknown:** ETW. `scratchpad/etw/events.csv` (3.3 GB) has the CSwitch rows, but their payload (new and old thread and
  wait reason) is not expanded (`DataSize=28` only). Off-CPU reasons per worker cannot be recovered from it.

## 3. Hook and port proposals

Ranked by expected gain per unit of risk. "Hitch" means the frame-time classes of report 09 and PLANO 1.3.

### P1. Adaptive spin in the port's mutex_lock (port, not a hook)

- **Where:** `src/runtime_mutex.c:89-109`, before the `pthread_mutex_timedlock` loop. It applies to every guest mutex,
  including scheduler `+0xb80`.
- **Change:** after the first failed trylock, spin with `_mm_pause` for a bounded time (for example 2-20 us), retrying
  trylock each step. Keep the 5 s timed lock and its diagnostics after the spin. Gate it with `SOTC_MUTEX_SPIN_US=<us>`,
  default 0 at first.
- **Why it helps:** the 33% is time inside the wrapper, so it is waiting, not work. If the holds are short
  (**inferred**), a spin that succeeds avoids the kernel wait and the wake latency.
- **Hitch it shortens:** frames where the render waits on counters while workers are slow to pick up jobs (the "idle"
  bucket of heavy-area frames, 40-60 ms). A partial gain; the 100+ ms spikes are GPU readbacks.
- **Could break:** CPU burn if the bound is large. With 4 workers plus CP and draw on 16 logical CPUs, the cost is real.
  Recursive semantics do not change. Fairness can get worse if one thread keeps retrying.
- **Verify:** `SOTC_TRACE` "mutex wait" spans (count and total per second, grouped by lock address) with
  `SOTC_MUTEX_SPIN_US=0` and `=10`; `SOTC_FPS=1` frame-time histogram; `SOTC_PROFILE` worker share.
- **Signature:** not needed (port code).
- **Gain:** unknown. If most of the 33% is the sleep inside the timed lock, it could be most of it. If it is the holds, less.

### P2. Existing knob: SOTC_USLEEP_YIELD=1 for counter polls (config, no code)

- **Where:** `WaitForCounter` 0x35590 (caller 0x356d7 in the sampled window), and any `usleep` of 10 us or less.
- **Change:** `SOTC_USLEEP_YIELD=1` (threshold `SOTC_USLEEP_YIELD_US=10`, the default) turns the 1 us request into
  SwitchToThread instead of a timer sleep of about 0.5 ms.
- **Why it helps:** the render waits on counters in roughly 0.5 ms steps. A counter that completes after 50 us can cost
  up to about 0.5 ms more than needed, and that delay sits on the frame's critical path (**inferred**).
- **Hitch it shortens:** a few ms per heavy frame (the render's tail wait). It does not touch 100 ms spikes.
- **Could break:** a spinning render thread takes a core from CP or draw when cores are scarce.
- **Verify:** the `SOTC USLEEP` line with `SOTC_FPS=1` (calls, requested, actual ms, yielded count), before and after,
  then the frame histogram.
- **Signature:** not needed.
- **Gain:** about 49% of the timer sleeps in the sampled window were attributed to this site (**inferred**), about
  0.5 ms each. The frame gain is unknown. The knob exists and has no recorded result, so it is the cheapest test.

### P3. Spin window before the worker sleeps (Wrap on CanWorkerSleep)

- **Where:** `JobSystemScheduler_CanWorkerSleep`, 0x39cc0. Pattern unique, 28 bytes:
  `55 48 89 e5 41 57 41 56 53 48 83 ec 58 4c 8b 3d ?? ?? ?? ?? 48 89 fb 49 8b 07 48 89` (`eboot_re.py sig 0x39cc0`).
- **Mode:** Wrap. The post phase returns "cannot sleep" (0) while the worker's last job ended less than X us ago
  (for example 100-200 us). The end time needs a second hook at the return of `ExecuteJob` (0x39800), since its start
  stamp at `+0xd88` is observed but its end is not.
- **Change:** it only turns "sleep" into "keep spinning" for a short window. It never skips work and never changes the
  deadlock check.
- **Why it helps:** a worker that sleeps and is woken by the next enqueue pays the wake latency (signal, then the
  scheduler). Jobs that arrive in bursts would be picked up sooner.
- **Hitch it shortens:** burst-start hitches, the 200-400 ms boot and area-change frames where many jobs arrive together.
- **Could break:** CPU burn during the window, and more load on the cores CP and draw use. It cannot cause a deadlock,
  because the only effect is a longer spin, which already has the 10 s check.
- **Verify:** `SOTC_EVF_TRACE=PS4SyncEvent` (worker sleeps per second, set-to-wake times) before and after.
- **Gain:** unknown. Test it after P1, because P1 removes the lock part of the same wake path.

### P4. Affinity from the game's masks as Windows CPU sets (port)

- **Where:** `create()` (`src/runtime_thread.c:326`) and `host_start` (`:309`), using `SetThreadSelectedCpuSets`, the
  same API as `gpu/shim/bbport_threads.h:194`. The masks come from the game's calls to `FUN_0003b5c0` and `FUN_0003b6a0`:
  Shadow Game Update mask 1, Game Render mask 2, Resource Loading mask 0x40, RenderFlip 0x3f, and the job workers at
  masks 2, 4, 8, 0x10. **observed** (call arguments). That bit i means CPU i is **inferred** from the table at 0x34230
  and the thread names.
- **Change:** when `SOTC_CPUSETS=1`, place the guest threads on logical CPUs 8-15 (physical cores 4-7 on this CPU), so
  they never share a core with the port's exclusive CP, draw, recorder and present threads (LP 0, 2, 4, 6). Keep the
  game's relative placement. Fall back when there are fewer than 8 physical cores.
- **Why it helps:** the port's CP and draw threads are CPU-bound (PLANO 1.2: about 17 ms and 22 ms per frame). Four
  workers at about 14 ms each, plus render and update, can land on their SMT siblings or on those cores (**inferred**,
  not measured).
- **Hitch it shortens:** the double-saturation frames where CP and draw both reach 33-40 ms (PLANO 1.3). Not the
  readback spikes.
- **Could break:** the placement only covers 8 logical CPUs on this machine. Copy and prep helpers use the SMT siblings
  (`BbThreads::siblings`), so they compete with the guest threads.
- **Verify:** `SOTC_CPUSETS=1` with the new placement, then the `SOTC_FPS` frame histogram and the `SOTC DRAW SPLIT` line.
- **Gain:** unknown. Low to medium. Judge it after P1 and P2 are measured, so placement is tested against the real bottleneck.

### P5. Worker count (Entry on JobSystem_Initialize)

- **Where:** `JobSystem_Initialize` 0x335d0. Pattern unique, 32 bytes (`eboot_re.py sig 0x335d0`).
- **Mode:** Entry. Rewrite the mask at `param_2+0x30` (the game passes `0x1e`, which gives 4 workers) before the original
  body runs.
- **Change (test only):** 3 workers (`0x0e`), to see whether less lock contention outweighs the loss of job throughput.
- **Why not more workers:** the lock is the limit. More workers add waiters on the same mutex and more spinning.
- **Hitch it shortens:** unknown; probably none on its own.
- **Could break:** job throughput drops. The worker count is derived from the mask (popcount, observed at 0x335d0), so the
  change is consistent in the data, but untested.
- **Verify:** `SOTC_PROFILE` worker share and the `SOTC_FPS` frame histogram.
- **Gain:** unknown.

### P6. Read faults inside jobs: existing fence knob (config)

- **Where:** `bbgpu_handle_fault`, `gpu/shim/bbgpu.cpp:416-447`.
- **Knob:** `SOTC_FENCE_PIECES=1` (default off). A read fault then skips the write-fence wait (`WaitVa`) when a copy is
  pending. The readback still waits for its own download (`WaitUpTo`), so only part of the wait goes away.
- **Why it could help:** the worker's 24% in read faults includes that write fence. The share it contributes is
  **unknown**.
- **Hitch it shortens:** GPU-read frames (the 599 ms worst frame in PLANO 1.3), if the fence was the wait.
- **Could break:** the code comments say readback ordering depends on that wait. Keep it as a measurement only.
- **Verify:** the `SOTC FAULTS` line with full `SOTC_FPS=1`. The line shows 0.0 with `SOTC_FPS_LEAN=1`, so do not use LEAN here.
- **Gain:** unknown. Do not ship without that measurement.

### Not proposed now

- **Move the event sets and the predicate out of `+0xb80` (Site in 0x38700).** This would shorten the hold the most, but
  `FUN_00038030` changes shared lists under the lock. High risk; it needs a full read of 0x38030 first.
- **Wake one worker per enqueue (Entry on 0x38550).** Fewer `EventFlag_Set` calls under the lock, but a missed wake
  stalls the job system. CheckForDeadlock exists because waits can wedge. High risk.
- **Raise thread priorities (Game Render and Update above normal).** In heavy frames the port's CP and draw are the
  bottleneck (reports 08 and 09). Raising guest threads can starve them. `SOTC_GPU_THREAD_HIGH` (`liverpool.cpp:53-64`)
  already records this risk.
- **Let workers sleep while counter waiters exist (change the counter clause of CanWorkerSleep).** It removes the spin,
  but the game's deadlock check suggests the clause is intentional. High risk without more RE.

## 4. Open questions

1. Which mutex is the 33%? Use the "mutex wait" trace spans (SOTC_TRACE) grouped by lock address, and map the address to
   the scheduler object (`+0xb80`) within one run. **unknown** today.
2. How does winpthreads `pthread_mutex_timedlock` wait: by polling or by an OS wait? This decides how much P1 can gain.
   **unknown.**
3. What share of worker wall time is on the render's critical path (a worker busy while a `WaitForCounter` is blocked)?
   A port counter for that would decide between P1, P2 and P4. **unknown.**
4. Are the 25% event waits idle time or wake latency? Measure set-to-wake with `SOTC_EVF_TRACE=PS4SyncEvent`. **unknown.**
5. What does the predicate at `local_e8` (run under `+0xb80`, 0x38700) cost? Its target is not decoded. **unknown.**
6. How much of the workers' ~14 ms CPU per frame is `sched_yield` spin (CanWorkerSleep false while counters wait)? A yield
   counter in the port would show it. **unknown.**
7. The usleep callers (0x356d7 and 0x19b4f) were mapped from one run's base. Confirm with a USLEEP line and its base from
   the same session. **inferred.**
8. ETW context switches with wait reasons would settle the worker wait classes, but the existing capture lacks the payload.
   A new capture is needed. **unknown.**
9. Is the counter clause of CanWorkerSleep needed for deadlock safety? That must be answered before any change to the
   sleep policy. **unknown.**
