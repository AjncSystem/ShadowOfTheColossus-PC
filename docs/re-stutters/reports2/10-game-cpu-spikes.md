# 10 - Hitches from the game's own CPU work (update thread, GC, objects, area entry)

Labels: **observed** (disassembly, pseudocode or a log line, with its address), **inferred**, **unknown**.
Addresses are eboot image offsets. Pseudocode: `out/re/decomp/<4 hex>/<addr>.c`.

Scope note: `reports2/05` does not exist in the tree. I read `reports/08` and `reports/09` in full,
and `reports2/01` and `reports2/02` for their answers only. `reports2/06`-`08` were not read.

## 1. Answer

- **observed:** the existing traces put little of the hitch time on the game's CPU. In 8 trace
  sessions (157 slow frames, `reports/09`), one slow frame is blamed on "game-cpu". The draw
  thread is blamed on 95 of the 98 frames from session 013009 on. Game-side time in those frames
  is mostly blocking waits (event flags), not work. Treat the game-CPU items as second tier.
- **observed:** `CGameManager::Update` is `sub_1c6150` (1862 bytes), called from `sub_3c5bb0`
  (37 bytes). It runs the physics step `sub_21e770` (2719 bytes) and the touch-physics step
  `sub_220210` under the mutex at `param+0x205578`. Then it runs a "think" pass, repeated in a
  `do { } while` loop until no object reports a change. The loop has no bound in the code.
- **observed:** the garbage heap (`iosMalloc` garbage partition, used by
  `BP_LoadResourceGarbageCollected` = `sub_6ac2c0`) has one global mutex, `DAT_02f35898`. The
  compaction tick `sub_57a360` (1401 bytes) holds that mutex while it `memmove`s whole blocks.
  Every allocation and free in that partition waits for it. This is the best candidate for an
  update-thread stall that the port can see.
- **observed:** when a garbage allocation fails, `sub_6ac2c0` retries forever: it calls the no-op
  `sub_578ee0`, then `sub_6af0d0` (sets a flag, calls the scheduler step wait `0x5746f0`), then
  allocates again. How often this happens: **unknown**.
- **observed:** the scheduler thread `vScheduler` (`sub_579230`) is a fixed 1/60 s loop. Each step
  calls `sub_57bb90` and an optional callback in `DAT_02f29720`. The compaction runs at that pace,
  so its work reaches the update thread only through the mutex.
- **observed:** `ResourceFactory::BuildSync` (`0x146b60`, 791 bytes, unique signature) builds a
  resource synchronously. The name string `CGameObjectPropertyPackageMultiLoader::ForceSyncBuildIfNeeded`
  is referenced from `sub_1d2ec0`, `sub_1d32d0` and `sub_1d3410`. Whether these run on the update
  thread at area entry: **unknown** (indirect calls; no static caller found for BuildSync).
- **inferred:** the safe first step is measurement (Wrap, `=2`) on the GameManager phases, the
  compaction tick and the allocation retry. None of these changes game logic. Spreading the work
  (budgeting the tick, bounding the think loop, moving the sort) changes logic or timing; I propose
  those only after the measurements.
- **unknown:** the gain. No existing log separates game-CPU time from port time per frame. H1, H3
  and H4 below decide whether any of the other proposals is worth doing.

## 2. Evidence

### 2.1 Frame structure
- **observed:** `sub_3c5bb0` (37 bytes) calls only `sub_a3d1d0` and `sub_1c6150`. Its own caller is
  not known, so whether it runs on the update thread is **unknown**.
- **observed:** `ShadowGameUpdate_ThreadFunc` (`0x534690`, see `reports/08`) does not call
  `sub_1c6150` directly. Its direct callees are small. The path from the update to CGameManager
  is **unknown**.
- **observed, `sub_1c6150` (CGameManager::Update, string `void CGameManager::Update()` at 0xfb45bf):**
  - the timers `GameManager: object count`, `component count`, `object deletion list size`,
    `physics time`, `touch physics time`, `touch sensors time`, `build renderlist time`,
    `overall update time`, `render time`, `pre-think`, `think`, `post-think`, `flush object deletion
    time` are registered in `sub_1d0770` (4049 bytes, init code). Whether their stored values are
    the last frame or an average: **unknown**;
  - `scePthreadMutexLock(param+0x205578)` precedes `sub_21e770` (physics) and `sub_220210` (touch
    physics), and unlocks after them;
  - `sub_231ce0` (59 bytes): touch sensors; `sub_a67480` (576 bytes): build renderlist;
  - the think loop: `do { sub_1d6c90(...); if (queue at +0x102118 has work) sub_a739c0(...) }
    while (+0x78 != 0 || +0x205520 != 0)`. `sub_1d6c90` (369 bytes) calls itself (a tree walk). The
    iteration count per frame is **unknown**;
  - `sub_1ccb40` (321 bytes, unique signature): top-down merge sort over 72-byte (`0x48`)
    elements. The temporary buffer is `operator new(count*0x48, nothrow)`, retried with half the
    count on failure, and kept at `param_4+0x20`. Array size at area entry: **unknown**.
- **observed:** `sub_a739c0` (about 1.2 KB) is called from `sub_1c6150`, `sub_1d6c90` and
  `sub_26fcf0`. It loops while the queue at `GameMgr+0x102118` has work, with a time parameter
  `param_2 + 0.0001`. Its cost per call is **unknown**.

### 2.2 Garbage heap and its mutex
- **observed:** `DAT_02f35898` is locked in `sub_57a360` (tick, source line `0x2bd`), `sub_57a8e0`
  (allocate, `0x2e8`), `sub_57a9b0` (set callback, `0x30d`), and `sub_579ae0` (core allocator,
  2133 bytes).
- **observed, `sub_57a360`:** `while (callback(arg) != 0)`: for each block, it moves the next block
  down over a free gap with `import_memmove` (length = block size, at `puVar17[4]`) and fixes the
  list links. A single iteration can move a block of any size under the mutex. The callback is the
  only budget.
- **observed:** `sub_57b230` (410 bytes) creates the partition. `sub_579100` creates the threads
  `vScheduler` (at `0x579230`, stack 0x1000) and `iScheduler` (at `0x5792f0`, stack 0x2000), and the
  semaphore `garbageExecSema`.
- **unknown:** how the callback (`DAT_02f358b8`) is set and what budget it has. The tick is reached
  through a pointer table: `refs` finds no code reference, and the file holds no static pointer to
  `0x57a360` (the table is relocated at run time).
- **observed:** the out-of-memory path in `sub_579ae0` (lines 363 and 377) prints
  "Out of CP3 Memory!" and "callback %p". **inferred:** it returns 0, which feeds the retry in
  `sub_6ac2c0`.

### 2.3 Allocation retry loop
- **observed, `sub_6ac2c0`** (`BP_LoadResourceGarbageCollected(char*, int, int*)`, unique 28-byte
  signature): allocate; if the result is 0, `do { sub_578ee0(); sub_6af0d0(); allocate again }
  while (result == 0)`. `sub_6af0d0` (12 bytes) sets `DAT_03017e98 = 1` and calls
  `thunk_FUN_005746f0`, the scheduler step wait. Only a successful allocation exits the loop.
- **unknown:** callers of `sub_6ac2c0` (indirect). `sub_6ac470` and `sub_6ae600` call the same
  allocation helpers, so loader and object code both use this path.

### 2.4 Synchronous resource builds
- **observed:** `ResourceFactory::BuildSync` (`0x146b60`, 791 bytes, unique 32-byte signature).
  Its direct callers are **unknown** (no static call site).
- **observed:** the name string `ForceSyncBuildIfNeeded` (0xfb4a26) is referenced from `sub_1d2ec0`
  (205 bytes), `sub_1d32d0` and `sub_1d3410`. These are object-property loaders close to
  CGameManager. Which of them calls the sync build at area entry: **unknown**.
- **observed:** `res_showResourceSyncWarnings` (`0xfa9303`) is read by `sub_149f00` (3009 bytes).
  The game has a switch to log sync builds, which gives a cheap count (H5).

### 2.5 Area entry
- **observed names** (`tools/re/names.txt`): `CGame::LoadInitialWorld` (`0x3c5170`, 1212 bytes) and
  `GameCoreLoop::LoadResourceDependencyFile` (`0x2567f0`, 375 bytes).
- `AreaLoad_PrecacheLists` (`0x533100`, 3962 bytes) is already probed by the port (`probe_area.cpp`,
  `reports/07`). Not repeated here.
- **observed:** the area loader state machine `sub_344850` (1300 bytes, cvar
  `areaLoader_HoldInLoadStateBeforeUnloadingTime`). Its thread and callers: **unknown**.
- **observed:** `CComponentGeneralOptimizeObjectTree` has `enableOptimizePass` (0xfc2f60) and the
  log `OptimizeObjectOwner(Think: %d, Render: %d)` (0xfc31a0). The cvar registration is `sub_252ae0`
  (301 bytes); the owner code is `sub_251ac0`. Whether the object-tree rebuild runs at area entry:
  **unknown**. This is the next function to read.

### 2.6 Physics
- **observed:** `sub_21e770` (2719 bytes, unique 32-byte signature) runs under the object-manager
  mutex once per update. Its body was not read. Whether it rebuilds a broadphase grid or tree:
  **unknown**.
- **observed strings:** `Enable BP Collision Grid`, `Enable Static Collision`, `Use HiRes Collision`
  (cvars, 0xf929ad-0xf92b02). A grid rebuild at area entry is plausible but **unknown**.

## 3. Hook proposals

Signatures were checked with `eboot_re.py sig` (uniqueness within the stated byte count).

### H1. Observe the GameManager phases (Wrap, measurement only)
- **Site:** `sub_1c6150` (CGameManager::Update), entry and exit. Unique within 32 bytes at
  `0x1c6150`. Also `sub_21e770` (physics, unique 32 bytes) and `sub_1d6c90` (think, unique 24 bytes).
- **Mode:** Wrap, returns `Continue`.
- **What it changes:** nothing. It records the TSC time of physics, touch physics and think, and the
  number of think iterations per frame.
- **Why it helps:** it shows whether a hitch is in the game's update or in the port, and whether the
  think loop ever runs more than a few times in a frame.
- **What could break:** nothing in the guest state. A wrong signature disables the hook (safe by
  HOOK-DESIGN).
- **Verify:** `=2` (log only). Compare with the port's 5 s lines.
- **Signature:** `sub_1c6150` unique (32 bytes); `sub_21e770` unique (32); `sub_1d6c90` unique (24).
- **Expected gain:** none; diagnostic. It decides H2 and H6.

### H2. Bound the think loop (Wrap, game-logic change, high risk)
- **Site:** the `do { } while` loop in `sub_1c6150` around `sub_1d6c90`. The loop reads the flags at
  `+0x78` and `+0x205520`, set by `sub_1d6c90`.
- **Mode:** Wrap on `sub_1d6c90` with a counter; from iteration N, clear the two flags so the loop
  ends. Objects that did not settle keep their state for the next frame.
- **What it changes:** caps the think work of one frame in the worst case.
- **Why it shortens a hitch:** a frame with many state changes (area entry, a colossus action) runs
  many think iterations. The cap spreads them over frames.
- **What could break:** game logic. Anything that expects a change to be visible in the same frame
  (touch, a Think that enables another object, scripted sequences) sees it one frame late. Soft locks
  in scripts are possible.
- **Verify:** first H1, to count iterations. Only if spike frames regularly exceed the cap, test N=4
  on colossus 1 and the first area's scripted sequences.
- **Signature:** `sub_1d6c90` unique (24 bytes). The loop itself has no signature yet.
- **Expected gain:** **unknown**. The upper bound is the think share of the worst frames, which H1 measures.

### H3. Bound the compaction tick (Wrap, partial logic change)
- **Site:** `sub_57a360` (compaction tick, 1401 bytes, unique 32-byte signature). Wrap on entry:
  replace the callback argument (`param_1`) with a wrapper that calls the original and returns 0 after
  a budget (for example 1 MB moved, or 2 ms, per call).
- **Mode:** Wrap (argument replacement on entry).
- **What it changes:** the tick stops early and releases `DAT_02f35898` sooner. Allocations on the
  update thread wait less. The tick resumes on the next step.
- **Why it shortens a hitch:** one large `memmove` under the mutex blocks every garbage allocation
  (observed, 2.2). A byte budget bounds that wait.
- **What could break:** compaction stays incomplete, so free space stays fragmented. An allocation then
  fails and `sub_6ac2c0` spins (2.3). That spin could cost more than the stall it removes. Also, the
  callbacks may assume a complete pass (**unknown**; check the callbacks set in `sub_57a9b0`).
- **Verify:** Wrap with `=2`: per call, log the bytes moved, the blocks moved and the TSC time under the
  mutex. If the longest hold is under about 1 ms, drop this proposal. Otherwise try `=1` with the H4
  retry counter.
- **Signature:** `sub_57a360` unique (32 bytes).
- **Expected gain:** **unknown**. Only worth it if the logs show long mutex holds.

### H4. Count allocation retries (Wrap, measurement only)
- **Site:** `sub_6ac2c0` (`BP_LoadResourceGarbageCollected`), unique 28-byte signature. Wrap: count
  calls, failures and loop iterations, and the time inside the loop.
- **Mode:** Wrap, returns `Continue`.
- **What it changes:** nothing.
- **Why it helps:** zero retries means the compaction does not stall the loader through this path, and
  H3 can be dropped.
- **What could break:** nothing (observe only).
- **Verify:** `=2`. Boot and area entry, then compare the counts with the port's allocation log.
- **Signature:** `sub_6ac2c0` unique (28 bytes).
- **Expected gain:** none; diagnostic.

### H5. Count synchronous resource builds (Entry, measurement only)
- **Site:** `ResourceFactory::BuildSync` (`0x146b60`, 791 bytes), unique 32-byte signature. Entry, with a
  Wrap on exit for the duration.
- **Mode:** Entry plus Wrap, both `Continue`.
- **What it changes:** nothing.
- **Why it helps:** a sync build on the update thread is a direct hitch. Zero builds during area entry
  removes the question from §2.4. It also shows whether `ForceSyncBuildIfNeeded` reaches BuildSync.
- **What could break:** nothing.
- **Verify:** `=2`. Compare with the game's own warnings from `res_showResourceSyncWarnings`
  (0xfa9303), which the port can enable by its cvar path.
- **Signature:** `0x146b60` unique (32 bytes).
- **Expected gain:** none; diagnostic.

### H6. Move the object sort to a worker (not recommended yet)
- **Site:** `sub_1ccb40` (merge sort, 321 bytes, unique 24-byte signature), called from `sub_1c6150`.
- **Mode:** Entry. A worker sorts a copy; the update waits for it.
- **What it changes:** the sort runs on another thread, but the update still waits for the result, so
  little work leaves the critical path.
- **Why it shortens a hitch:** only if the sort is large. **unknown** (the size is in `param_4+0x20`).
  **inferred:** a few thousand 72-byte entries sort in well under a millisecond.
- **What could break:** the order of equal keys (the merge sort is stable and the worker must stay
  so) and the cached buffer at `param_4+0x20`.
- **Verify:** H1 first. Only if the sort measures above about 1 ms in a spike frame.
- **Signature:** `0x1ccb40` unique (24 bytes).
- **Expected gain:** **unknown**, probably small.

## 4. Open questions

1. Which thread calls `sub_3c5bb0` and `sub_6ac2c0`? This decides whether the compaction mutex (2.2)
   and the retry loop (2.3) stall the update thread. Find the pointer tables that hold them (RELA
   entries in `out/boot.bin`).
2. What sets the compaction callback `DAT_02f358b8`, and what is its budget? (2.2)
3. How many think iterations run in a spike frame? (H1)
4. Does the physics step `sub_21e770` rebuild a grid or tree when the area changes? (2.6)
5. Does the object-tree optimize pass (`sub_251ac0`, `sub_252630`) run at area entry? (2.5)
6. Which `ForceSyncBuildIfNeeded` caller runs on the update thread at area entry, and does it reach
   `BuildSync`? (2.4, H5)
7. What is the size distribution of the garbage blocks moved by the tick? (H3)
8. Are the `GameManager: ... time` values last-frame values? If so, the port can read them without a
   hook. (2.1)

## 5. Ranked summary

1. H1 (Wrap, observe): log GameManager phase times and think iterations. Zero risk; decides the rest.
2. H4 (Wrap, observe): count `BP_LoadResourceGarbageCollected` retries.
3. H3 (Wrap, budget the compaction tick): only if H1 or H4 show long mutex holds or retries.
4. H5 (Entry, observe): count synchronous resource builds at area entry.
5. Read the object-tree optimize pass (`sub_251ac0`, `sub_252630`) next: a possible area-entry rebuild.
6. Read the physics step `sub_21e770` for a grid or tree rebuild at area entry.
7. H2 (think-loop bound): a logic change; only with H1 data and scripted-sequence tests.
8. H6 (object sort to a worker): probably small; only if measured above 1 ms.
9. Move synchronous builds of area entry into the port's area prewarm (`gpu/sotc/area_prewarm.cpp`), if H5 shows them.
10. Gain estimates for all game-CPU items are unknown. The existing traces put game CPU in 1 of 98 slow frames.
