# 03 - Streaming bursts at boot, save load and area entry

Read only. No repository file was changed except this report. Addresses are eboot image offsets
(EU 1.01). Labels: **observed** (disassembly, pseudocode, or a log line with its location),
**inferred**, **unknown**.

## 1. Answer

- **Priority mapping (observed).** `ELoadingPriority` is 0 StreamingTexture, 1 SyncLoad, 2 Highest,
  3 High, 4 AboveNormal, 5 Normal, 6 BelowNormal, 7 Low, 8 Lowest, 9 Background (strings at
  0xfad98e..0xfadbae; `Total` = 10).
- **The default request code is not "Normal".** Most game call sites pass state `0xd` to
  `ResourceWrapper` request `0x14cf20`. That code maps `0xd` to the resource's stored state at +0x98
  when it is below 10, else to 5 (0x14cf20 pseudocode lines 111-120). A fresh resource has state 0, so
  its first request goes to **queue 0 (StreamingTexture)**. Area precache and texture mip upgrades both
  use this path. **observed (code); the state-0 meaning is inferred from the enum order.**
- **The frame waits for the whole loader queue.** `GameLoop` calls the barrier
  `ResourceFactory::WaitForAllCmdsToFinish` (0x1469d0) at 0x25cb12 (direct), and at 0x3c5a42 (via
  `sub_3c5a00`, called at 0x25cba9). The barrier loops while `HasPendingWork` (0x147f40) is true.
  `HasPendingWork` adds the 10 queue counts at +0x110 + k*0x28 (queue 0 at +0x110, queue 9 at +0x278)
  and checks four completed lists. **observed.**
- **Why hundreds of textures go live in one frame (inferred chain, each link observed).**
  1. The area precache `AreaLoad_PrecacheLists` (0x533100) builds resource names and issues many
     `0x14cf20` requests in one pass (about 7 visible blocks in the function, each per named file).
  2. The loader finishes them over time. `SyncPointProcessPendingResources` (0x1480f0) drains every
     completion in one call, with no item or time budget (report 05).
  3. `TextureManager` update `0x13ccd0` runs every frame (called from the render job through
     `0x13cae0` at 0x117930 and 0x117160). It walks every active texture slot (bitset, up to 0xffff) and
     calls `0x13a3e0` per texture. A texture whose resource is ready (`0x14cf00`) gets its mip copied
     into the pool by `0x98ae0` (`memcpy` at the end of that function). No global per-frame count cap
     was found on this path.
  4. In the port, the first bind of each new guest texture creates a VkImage. That is the `images`
     counter.
- **Images are not the expensive part (observed, log).** `c14_base.log:365`, frame 502, 416 ms:
  `images 670 (17 ms)`, `cp 315.7`, `upload 190.4 ms`. `dropfast2.log:282`, frame 189, 200 ms:
  `images 716 (10 ms)`, `upload 139.3 ms`. Spreading image creation alone would save little. The cost is
  command processing and the byte upload.
- **Area entry may spin inside a frame (inferred from code, observed loop).** `CGame::LoadInitialWorld`
  (0x3c5170) runs a `do { ... } while (!ready)` loop. Its predicate is `0x1d2f90` at the exit test
  0x3c55f6. The body calls only a timer and a log line: no sleep, no pump, no barrier (observed). It is
  reached from `GameLoop` through `0x3c4fa0` (call at 0x25c1fa). If the port's frame timer spans that
  call, the 300-400 ms frame is mostly this wait, and the burst is the first draw after it.
- **Unmap waits match the streaming decommit (inferred).** The port log shows `drop waits 2859 ms` and
  `2555 ms` per 5 s window (`c14_base.log`, SOTC unmap lines). The guest pool shrink in `0x98ae0` calls
  `0x12950` with mode 2 (decommit) at the mip drop path. The port's unmap path behind that call is not
  read here.
- **Cvar defaults are set (observed).** Written by `0x1418d0`, read from the storage stores:
  `enableTextureStreaming` 1; `Streaming Pool Total Size MiBs` 5120; `Streaming Pool Base Size MiBs`
  1024; `Reserve System Free Mem MiBs` 220; `Low Mem Recovery Size MiBs` 0; `Num Mip Requests` (the
  code name is `numMipRequestsPerFrame`) 256; `Max simultaneous load requests` 4;
  `Min Num Frames Before Unload` 5.
- **The per-frame caps in report 05 are not confirmed.** No reader of `Num Mip Requests` or
  `Max simultaneous load requests` was found (direct RIP-relative scan of the eboot finds only the
  registration). Setting them may do nothing. **unknown.**
- **Best candidate hook:** a call-site redirect of the two GameLoop barrier calls (0x25cb12, 0x3c5a42)
  to a variant that ignores queues 0 and 9 at that point. Do not change `HasPendingWork` globally: the
  destructor and `DeleteResource` also wait, and they need the full wait to avoid freeing a resource
  the loader still writes.

## 2. Evidence

**Priority and state (0x14cf20, `ResourceFactory` request)**
- Entry: `FUN_0014cf20(resource, prio, &DAT_02ed3640)`. The `0xd` branch is at pseudocode lines 111-116 (`if (param_2 == 0xd) { param_2 = 5; if (state < 10) param_2 = state; }`), the state store is line 120 (`state = param_2`), and `BuildAsync` is called through the factory vtable slot 3 at line 122 (`(**(code**)(*plVar6+0x18))`). **observed.**
- Callers passing `0xd`: about 150 files in the 0x9xxxx-0xexxxx range, including `FUN_00098ae0` (texture mip request, lines 36 and 49), `0x533100` (area precache, 7 sites) and `0x3c4fa0`. Explicit priority 1 (SyncLoad) and 2 (Highest) appear only a few times. **observed (grep of decomp).**
- Queue 0 is therefore the path for fresh resources. Report 05 named queue 0 but did not identify what fills it; this is that path. **inferred from the state logic.**

**Barrier and completions (report 05, rechecked)**
- `HasPendingWork` (0x147f40): lock `[obj+0x58]` (spin, `sceKernelUsleep(1)` after 10,000 spins); sums `[obj+0x110]`, `+0x138`, ..., `+0x278`; checks lists at `+0x280`, `+0x2a8`, `+0x2d0`, `+0x2f8`. **observed (disassembly 0x147f40..0x148050).**
- Barrier callers: `GameCoreLoop::GameLoop` 0x25cb12 (sig `e8 ?? ?? ?? ?? 48 8d 0d ?? ?? ?? ?? 48 8b 44 24 38 c5 f8 28`, unique); `sub_3c5a00` at 0x3c5a42 (sig `e8 ?? ?? ?? ?? c5 f8 57 c0 c5 f8 11 45 c0 49 8b`, unique); `sub_25ddd0` at 0x25dfa6 (no static caller found). Also `~ResourceFactory` 0x14678b, `DeleteResource` 0x147b6d, `sub_147d60` 0x147d67. **observed.**
- `SyncPoint` callers: the barrier (0x146ae4) and `sub_3c5bf0` (0x3c5c07). **observed.**

**Texture streaming (0x13ccd0 family)**
- `0x13ccd0` (`TextureManager` update loop): gated by `*ptr(0x2ee1590)` (`enableTextureStreaming`) and by `*ptr(0x2ee1720) == 0 || *ptr(0x2ee1658) == 0`. Iterates a bitset of active slots; calls `0x13a3e0` (`sub_13a3e0`, call site 0x13d01e, sig unique, 48 bytes). **observed.**
- `0x13a3e0`: per-texture state machine, stride 0x50 chunks, cursor bytes at `obj+0xa..0xc`. Load states from strings: `None, Idle, AutomaticLoading, Unloading_Stage0..3, Unloading_End`. Calls `0x14cf00` (ready), `0x14d2c0`, `0x112b80` (`ProcessTextureData`), `0x95580`, `0x98ae0`. **observed (code); the cursor values are a per-frame slice inside a texture, not across textures.**
- `0x98ae0` (mip upgrade/downgrade for chunk N): if the chunk's resource is ready, copies its data into the streaming pool with `memcpy`, sets `obj+0xc0 = N`. For a drop (N below the current mip), calls `0x12950` with mode 2. For a request, calls `0x14cf20(..., 0xd, ...)`. **observed.**
- `0x12950` is a guest-memory operation through a vtable slot 0xb8 on a memory object (`FUN_000124f0` on failure). The port side is not read. **observed (call), port behaviour unknown.**
- `0x13cb10` and `0x13d0f0`, `0x13d620`, `0x13d6d0` also read the same two gate flags (`0x2ee1720`, `0x2ee1658`). Their meaning is unknown.

**Cvar storage (0x1418d0 init, observed)**
- Object at `DAT_02ee1fc8` (`Num Mip Requests`): value 0x100 at `+0xd0`.
- `DAT_02ee2400` (`Min Num Frames Before Unload`): 5.
- `DAT_02ee24d8` (`Max simultaneous load requests`): 4.
- `DAT_02ee2178` (`Streaming Pool Total Size MiBs`): 0x1400 = 5120.
- `DAT_02ee20a0` (`Streaming Pool Base Size MiBs`): 0x400 = 1024.
- `DAT_02ee2250` (`Reserve System Free Mem MiBs`): 0xdc = 220.
- `DAT_02ee2328` (`Low Mem Recovery Size MiBs`): 0.
- `DAT_02ee1530` (`enableTextureStreaming`): 1 (`DAT_02ee15f0 = 1`).
- Name-to-object mapping is from `0x13a9a0` (`TextureManager::RegisterCvars`), which passes each object address with its name. The `+0xd0` value offset is consistent across objects. **observed; the mapping from object to name is inferred from adjacent registration.**
- Readers: the RIP-relative scan (all executable segments) finds only registration and init for the numeric objects. Reads must go through the registry. **unknown.**

**Log lines (observed)**
- `c14_base.log:365`: `SOTCSTALL frame 502 416 ms ... images 670 (17 ms ...) ... cp 315.7 ... upload 190.4 ms`.
- `c14_base.log:366`: frame 503, 102 ms, `images 113 (4 ms)`, `upload 61.2 ms`.
- `dropfast2.log:282`: frame 189, 200 ms, `images 716 (10 ms)`, `upload 139.3 ms`.
- `dropfast2.log:584`: frame 1260, 149 ms, `images 5 (1 ms)`, `upload 247.1 ms`. Upload dominates without many new images.
- `c14_base.log` 5 s unmap lines: `drop waits 2859.51 ms`, `2555.52 ms`, `2578.11 ms`.
- Port budget: `texture_cache.cpp:1424-1491`. `SOTC_UPLOAD_BUDGET_MB` (default 32) defers a first upload only for colour images of 256 KB or more, for at most 2 flips. Smaller images are never budgeted. **observed.**
- Port area prewarm (step 5d, `gpu/sotc/area_prewarm.cpp`): replays a saved image manifest per area on re-entry (`user/cache/area_manifest`). The first visit in a session is not covered. **observed (code header).**

## 3. Hook proposals

Mode vocabulary follows `docs/HOOK-DESIGN.md`: **Site** = redirect the `call rel32` of one caller;
**Entry** = detour at a function entry; **Wrap** = entry plus the original.

**H1 (best candidate): Site redirect of the GameLoop barrier, ignore streaming queues.**
- Site: the `call` at 0x25cb12 (GameLoop) and at 0x3c5a42 (`sub_3c5a00`). Leave `DeleteResource`,
  the destructor and `sub_147d60` alone.
- Mode: Site (redirect to a host handler that runs a copy of the barrier with `HasPendingWork`
  computed without `+0x110` (queue 0) and `+0x278` (queue 9), and without the completed-list entries
  of those two priorities; then calls `SyncPoint` as now).
- Change: the frame end no longer waits for streaming and background loads. Their completions are
  still drained when they finish, on the next call.
- Why it helps: removes the part of the frame-end wait caused by mip and background loads. The
  worst frames are not explained by this alone (see section 2: `cp` and `upload` dominate), so the
  gain is unknown until the barrier time is measured.
- What could break: (a) the four completed lists are shared by priorities; a per-priority filter needs the
  node layout, which is not read here. (b) Game code may assume a texture or a file has finished after
  the barrier, so it would read an unfinished resource. The fallback is the engine's low mip, so
  pop-in is the expected symptom, not a crash, but unverified. (c) Redirecting only GameLoop keeps
  destruction safe.
- Verify: `=2` mode runs the old barrier, compares the two decisions, and logs any frame where the new
  barrier skipped a request that the old one would have waited for. Time spent inside the barrier per
  frame (the `LOADER_WAIT_ALL` probe already measures it, `SOTC_HOOK_LOADER_PROBE=1`).
- Signature: 0x25cb12 `e8 ?? ?? ?? ?? 48 8d 0d ?? ?? ?? ?? 48 8b 44 24 38 c5 f8 28` (20 bytes, unique).
  0x3c5a42 `e8 ?? ?? ?? ?? c5 f8 57 c0 c5 f8 11 45 c0 49 8b` (16 bytes, unique).

**H2: Declared `LOADER_HAS_PENDING` (0x147f40, switch `SOTC_HOOK_LOADER_NOSTREAM`): do not use as is.**
- Its declaration in `scripts/hooks.d/loader.py` is a Wrap at function entry. A global change also
  affects the destructor and `DeleteResource`, which must wait for in-flight loads (report 05). That risk
  is a use-after-free. Use H1 instead, or keep this declaration only for a measurement probe.
- Signature: pattern already in `loader.py`; expected 0x147f40.

**H3: Entry on the area wait (LoadInitialWorld readiness test) to yield.**
- Site: the exit test at 0x3c55f6 (`call 0x1d2f90; test al,al; je loop`). Sig
  `e8 ?? ?? ?? ?? 84 c0 0f 84 ?? ?? ?? ?? 48 8b 05 ?? ?? ?? ?? 48 8b 00 48 3b 84 24 80` (28 bytes, unique).
- Mode: Site with a host handler that sleeps 1 ms (or pumps one present) per iteration.
- Change: the game thread stops spinning while the area streams. This gives the loader threads CPU,
  and it may split the 300-400 ms frame.
- Why: if the loader is starved by the spin on machines with fewer free cores, the wait becomes shorter.
  Not proven. If the spin is not the problem, this does nothing.
- What could break: a pumped present during load may show a half-loaded area or the loading screen.
  Use sleep-only first.
- Verify: `=2` counts iterations and the readiness flips; compare the time between the first and last
  iteration with and without the sleep. Gain: unknown.

**H4: Per-frame cap on texture mip swaps, in the texture update.**
- Site: the call to `0x13a3e0` at 0x13d01e (sig `e8 ?? ?? ?? ?? 84 c0 74 ?? 89 d9 48 89 d8 ba 01 00 00 00 48 c7 c6 fe ff ff ff 83 e1 3f 48 c1 e8 06 48 d3 e2 48 d3 c6 49 8d 8c c6 30 20 20 00 49`, 48 bytes, unique).
- Mode: Site. A counter of mip copies in `0x98ae0` per frame; after N, the handler returns 0 (not
  done), so the bit stays set and the texture is processed next frame.
- Change: the number of textures that become live per frame is capped. The rest stay at their current
  mip.
- Why: the 720-image frames come from many completions processed in one update. A cap spreads them.
- What could break: pop-in lasts more frames. Unloads are delayed the same way. The bitset keeps the
  textures in the next pass, so no state is lost.
- Verify: `=2` reports how many mips a frame would have copied and how many were deferred. Compare the
  frame time and the number of new images per frame.
- Gain: unknown. Expected to cut the spike, not the total work.

**H5: Spread the completion drain (SyncPoint).**
- Site: the callee of the barrier (`0x1480f0`, sig known from `probe_loader.cpp`), or the call at 0x3c5c07.
- Change: budget the completion nodes per call (count or ms). Only safe together with H1: the barrier
  loops until the completed lists are empty, so an unbudgeted list would still spin the frame.
- Gain: unknown. Do not ship alone.

**H6: Defer the guest pool decommit (`0x12950` mode 2 from `0x98ae0`).**
- Site: the `call 0x12950` with mode 2 in `0x98ae0` (inside the drop branch). Mode: Entry for
  `0x98ae0` with a host queue: decommits are recorded and run at most N per frame, or after the frame.
- Change: moves the unmap waits (2.5-2.9 s per 5 s) off the frame that drops the mip. This overlaps with
  `SOTC_DROP_FAST`.
- What could break: memory pressure if the pool is not decommitted before a new allocation. The pool
  budget is 5120 MiB total, so the pressure is probably low; unverified.
- Verify: `drop waits` in the port log; `=2` checks that the deferred ranges are not read before they are
  unmapped.

**H7: Port-side announcement (partly implemented).**
- The area prewarm already replays images per area on re-entry. The first visit gets nothing.
- Using the area precache entry (`AREA_PRECACHE`, 0x533100, probe declared) to announce the names before
  the loads finish would let the port create images early. But image creation costs about 17 ms per 670
  images (section 2), so the gain is small. The byte upload (`upload 190 ms`) is the real cost. The
  budget could be widened to smaller images, which are not budgeted today.

## 4. Open questions

1. Readers of the numeric cvars (`Num Mip Requests` 256, `Max simultaneous load requests` 4,
   `Min Num Frames Before Unload` 5). Find the getter by following the registry (`0x2ee...` objects are
   reached by name). Until then, do not rely on them as caps.
2. The meaning of the gate flags `0x2ee1720` and `0x2ee1658` (read by `0x13cb10`, `0x13ccd0`,
   `0x13d0f0`, `0x13d620`, `0x13d6d0`).
3. The completed-list layout: which priority each of the four lists at +0x280..+0x2f8 holds. This decides
   whether H1 can filter per priority.
4. Whether the area load really runs inside a frame (the port frame timer spanning `0x3c4fa0`) or
   between frames. Check with the `LOADER_WAIT_ALL` probe and a timer around `0x3c5170`.
5. Who calls `0x533100` (area precache) and on which thread. The callers list is empty in the index.
6. The port's implementation of `0x12950` mode 2 (unmap) and its wait, which is the 2.5-2.9 s.
7. Whether 0x14cf20's state-0 path (queue 0) is the common case for mip requests, or whether state is set
   earlier by a load with `0xc` ("LoadAsDependency").

## 5. Summary (ranked by expected gain, all unverified)

1. H1: redirect the two GameLoop barrier calls to a barrier without streaming queues (sigs unique). Biggest
   expected cut to the frame-end wait; keep destruction on the full barrier.
2. H4: per-frame cap on mip copies in the texture update (site 0x13d01e). Cuts the 700-image frames; costs pop-in.
3. H6: defer the guest pool decommit (`0x12950` mode 2). Targets the 2.5-2.9 s unmap waits; overlaps SOTC_DROP_FAST.
4. H3: sleep in the LoadInitialWorld readiness spin (site 0x3c55f6). Possibly shortens the area-entry frame; unproven.
5. H5: budget the SyncPoint drain (0x1480f0), only together with H1.
6. H7: widen the upload budget to small images and announce area resources; image creation is not the cost.
7. Do not use `HasPendingWork` (0x147f40) globally (H2).
8. Do not rely on `numMipRequestsPerFrame` until its reader is found; report 05's option E is weakened.
9. Measure first: time in the barrier (LOADER_WAIT_ALL probe) and the frame timer across `0x3c4fa0`.
10. Expected overall gain: unknown; the worst frames are dominated by command processing and upload bytes (`cp 316 ms`, `upload 190 ms`), so the barrier and the burst may explain only part of them.
