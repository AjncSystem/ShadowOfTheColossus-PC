# 17 - Who calls ResourceFactory::BuildSync (0x146b60), and on which thread

Read only. Nothing built, run or edited except this report. Addresses are eboot VAs (EU 1.01,
`out/eboot.elf`). The eboot is mapped at base 0 in `out/boot-linked.bin`, so image offsets equal
VAs. Labels: **observed** (disassembly, relocation entry, or a call-graph edge from `tools/re/eboot_re.py`
with the address), **inferred**, **unknown**.

Scratch scripts (not in the repo) are in
`C:\Users\ptr\AppData\Local\Temp\claude\C--Users-ptr-Documents-sotc-pc\bc826a72-eefc-4071-b69e-450bd508be04\scratchpad\bsc\`.

## 1. Answer

- **observed:** `BuildSync` is slot 2 of the ResourceFactory vtable at 0x1598518 (vptr = vtable+0x10 stored by the
  destructor, 0x14677a-0x146788). It is the only vtable that holds 0x146b60: one RELATIVE relocation in
  `boot-linked.bin`, image offset 0x1598528. No code calls 0x146b60 by a direct `call`.
- **observed:** the code reaches slot 2 through `sub_14d7b0` (call it EnsureBuilt, 190 bytes), not through the RF
  global. It reads `wrapper+0x68` (a ResourcePool), calls the pool's slot 13 (0x14fc00, returns `pool+8`), which is
  the RF singleton, then calls RF slot 2 (`call [rcx+0x10]` at 0x14d85c). A second copy of the same pattern is in
  `sub_14d900` (call at 0x14d9f5).
- **observed:** `EnsureBuilt` (sub_14d7b0) runs a sync build only when the wrapper has state `[rbx+0x98] == 0xb`,
  no data (`[rbx+0x20] == 0`), and `[rbx+0x60]` dword == 0. Otherwise it returns at once.
- **observed:** 550 distinct functions call `sub_14d7b0` directly. Its callers include the three threads below.
  This is the answer to "does a caller run on the update or render thread": **yes, statically, on all three**
  (update, render, and the main loop).
- **observed, update thread:** `ShadowGameUpdate_ThreadFunc` (0x534690) calls `sub_4948b0` at 0x535310 (name
  argument `'GameCamera'`, 0x5352ad) and at 0x5357bd (`'AudioListener'`, 0x535771). `sub_4948b0` calls
  `sub_14cf20` (async request) and then `sub_14d7b0` (EnsureBuilt) unconditionally, at 0x494cc7, 0x494d91,
  0x494fae and 0x4951d8. So the update thread calls EnsureBuilt on each of these calls.
- **observed, render thread:** `RenderThread::FrameJobEntry` (0xcef90, a jump to `EndFrameViewParams` 0xcefa0)
  calls `sub_187510` at 0xcf42c, and `sub_187510` calls `sub_14d7b0` at 0x187695.
- **observed, main loop:** `GameCoreLoop::GameLoop` (0x25c080) calls `sub_3c5630` at 0x25c483, which calls
  `sub_1d3500` (0x3c5855), then `sub_1d32d0` (0x1d3553), then `sub_1d3410` (0x1d33b4, gated by
  `ResourceWrapper::IsReadyToCreate`), then `sub_14d7b0` (0x1d34d0). This is the `ForceSyncBuildIfNeeded` chain.
- **observed, area load:** `AreaLoad_ShaderListAndAssets` (0x2569d0) calls `sub_169400` (0x256ca8), which calls
  `sub_14d7b0` at 0x1695f3 and 0x1697c8. `sub_25ddd0` calls `sub_14d900` at 0x25dfd3 (no static caller).
- **unknown:** whether any of these sites fires in play. The state precondition (`0xb`, no data) is met only in a
  window after a request, and the rate is unmeasured. The static graph does not show the frequency, and the
  caller of `AreaLoad_PrecacheLists` and `sub_25ddd0` is a pointer (no static caller).
- **Proposal:** a log-only Wrap on `BuildSync` (32-byte unique signature, 0x146b60) that records the thread, the
  time spent, the wrapper state and the return address chain. Section 3, H1.

## 2. Evidence

### 2.1 The vtable and the slot (observed)

- The ResourceFactory dtor `~ResourceFactory` (0x146770) writes `vptr = 0x1598508 + 0x10` (`lea rax,[rip+...]` at
  0x14677a, `add rax,0x10` at 0x146784, store at 0x146788). So vptr = 0x1598518.
- Slots from the relocation table (`boot-linked.bin` RELATIVE entries, 0x1598518 + 8*i):
  - slot 0 (0x1598518) 0x146770 dtor; slot 1 (0x1598520) 0x146b40;
  - **slot 2 (0x1598528) 0x146b60 BuildSync**;
  - slot 3 (0x1598530) 0x147490 BuildAsync; slot 4 (0x1598538) 0x147640;
  - slot 5 (0x1598540) 0x1478d0 DeleteResource; slot 6 (0x1598548) 0x147ba0;
  - slot 7 (0x1598550) 0x147c70; slot 8 (0x1598558) 0x149600; slot 9 (0x1598560) 0x147d60.
  The table ends at 0x1598568, which belongs to another class. So the RF has 10 slots.
- The scan of all relocations in `boot-linked.bin` (714,588 entries) finds one RELATIVE entry with value 0x146b60.
  No other vtable holds BuildSync, so no other class inherits it.
- `WaitForAllCmdsToFinish` (0x1469d0) is a direct call, not a slot. `GameLoop` calls RF slot 9 (0x25cb05) and then
  `WaitForAll` directly (0x25cb12).

### 2.2 The RF singleton (observed)

- `DAT_02ee2d00` is the RF singleton. `AreaLoad_ShaderListAndAssets` (0x2569d0) allocates 0x350 bytes (`sub_17070`
  at 0x256a15), constructs the RF (`ResourceFactory::ResourceFactory` 0x144ca0 at 0x256a20), deletes the old one
  through its slot 1 (0x256a3d), and stores the new one at 0x256a40 (`r12` = 0x2ee2d00).
- The RF object is larger than 0x338 bytes (`sub_144320` reads `rbx+0x338`), which fits this.
- **observed:** `AreaLoad_ShaderListAndAssets` has one static caller, `AreaLoad_PrecacheLists` (0x5331e9).
  `AreaLoad_PrecacheLists` has no static caller. So the RF is created on the area-load path. Whether it is
  created once per boot or once per area: **unknown**.

### 2.3 The pool and the slot-13 getter (observed)

- `ResourcePool` vtable at 0x1598628 (dtor 0x14de00, its vptr store at 0x14de0a and ctor 0x14db18 use 0x1598618).
  Slot 3 = `GetResource` (0x14e260), slot 4 = `TryGetResource` (0x14e440), slot 13 (0x1598690) = **0x14fc00**,
  whose whole body is `mov rax,[rdi+8]; ret` (5 bytes).
- The pool ctor `sub_14db00` stores `[rbx+8] = rsi` (0x14db3e). Its only caller is `AreaLoad_ShaderListAndAssets`
  at 0x256b7f, where `rsi = [r12]` = `[0x2ee2d00]`, the RF just stored (0x256b75). So **pool+8 = RF singleton**.
- `ResourcePool::GetResource` (0x14e260) creates the wrapper through `sub_14c8a0` (call at 0x14e36c), passing
  `rdx = r12 = this` (0x14e361). `sub_14c8a0` stores that as `wrapper+0x68 = rdx` (0x14c8e0). Its only caller is
  `GetResource`. So a wrapper created by `GetResource` has `+0x68 = its pool`.
- **inferred:** the wrappers that reach `EnsureBuilt` are those created by `GetResource`. The other wrapper
  constructor `sub_14c9a0` writes `+0x68 = 0` (0x14c9f1). Such a wrapper would crash in `EnsureBuilt`, so
  presumably it never reaches state 0xb with no data. **unknown.**

### 2.4 EnsureBuilt, sub_14d7b0 (observed, full disassembly read)

```
cmp  dword [rbx+0x98], 0xb ; jne  end         (state must be 0xb)
cmp  qword [rbx+0x20], 0   ; jne  end         (no data yet)
...  r15 = &[0x2ee3b00] static dummy ... cmp rax, rbx ; je end
cmp  dword [rbx+0x60], 0  ; jne  end
rdi = [rbx+0x68]                              (pool)
rax = [rdi]; call [rax+0x68]                  (pool slot 13 -> RF)      0x14d850
rcx = [rax]; rdi = rax; rsi = rbx
call [rcx+0x10]                               (RF slot 2 = BuildSync)   0x14d85c
```

### 2.5 The other copy: sub_14d900 (observed)

- `sub_14d900` (0x14d900, 500 bytes) has the same shape: `rdi = [rbx+0x68]`, pool slot 13 (`call [rax+0x68]` at
  0x14d9e5), then `call [rcx+0x10]` (RF slot 2) at 0x14d9f5, with `rsi = [rbp-0x130]` (a wrapper built by
  `sub_14c9a0` at 0x14d955). Its only static caller is `sub_25ddd0` (0x25dfd3). `sub_25ddd0` has no static
  caller. It is the area-change barrier that report 02 names (its `WaitForAll` at 0x25dfa6).

### 2.6 Other slot-2 sites that are not BuildSync (observed)

I scanned every function that references `0x2ee2d00` for `call` or `jmp` through `[reg+0x10]`:

- `GameLoop` 0x25c269 calls `[rbx]` slot 2 (a refcount release on a different object), not RF.
- `sub_2556c0` 0x2556fc calls slot 2 on the object at 0x2ee2cf8 (another global), not RF.
- `sub_3c5a00` 0x3c5a81, 0x3c5ac4, 0x3c5b5d: refcount releases on objects from `[r14+0x10]` and `[rbp-0x38]`.
- `sub_14d900` 0x14d9f5: see 2.5.

No function that loads the RF global calls its slot 2 directly. This scan covers only functions that reference
0x2ee2d00. It does not cover every `call [reg+0x10]` in the image (about 20 tail jumps `jmp [reg+0x10]` exist in
the image). **unknown** whether another path reaches RF slot 2 through a different object.

### 2.7 The callers of EnsureBuilt, by thread (observed call edges)

| Thread / entry | Chain (direct calls, address of each call) | Frequency |
|---|---|---|
| Update thread `ShadowGameUpdate_ThreadFunc` 0x534690 | 0x535310 and 0x5357bd call `sub_4948b0`; `sub_4948b0` calls `sub_14cf20` (0x494cb2, request, mode 0xd) then `sub_14d7b0` (0x494cc7, 0x494d91, 0x494fae, 0x4951d8) | per call of the update function; loop structure **unknown** |
| Render, `RenderThread::FrameJobEntry` 0xcef90 | 0xcef90 jumps to `EndFrameViewParams` 0xcefa0; call at 0xcf42c `sub_187510`; call at 0x187695 `sub_14d7b0` | per frame of the render job (inferred from the frame-job name) |
| Main loop `GameCoreLoop::GameLoop` 0x25c080 | 0x25c483 `sub_3c5630` -> 0x3c5855 `sub_1d3500` -> 0x1d3553 `sub_1d32d0` -> 0x1d33b4 `sub_1d3410` (after `IsReadyToCreate` 0x1d33a5) -> 0x1d34d0 `sub_14d7b0` | per GameLoop iteration, once (0x25c483 is outside the loop of 4) |
| Area load `AreaLoad_ShaderListAndAssets` 0x2569d0 | 0x256ca8 `sub_169400` -> 0x1695f3 and 0x1697c8 `sub_14d7b0` | once per area load; thread **unknown** (no static caller of `AreaLoad_PrecacheLists`) |
| Area change `sub_25ddd0` 0x25ddd0 | 0x25dfd3 `sub_14d900` -> RF slot 2 | unknown; no static caller |

Other static callers of `sub_14d7b0` (550 functions in total) include `sub_932a0`, `sub_a78a0`, `sub_ab430`,
`sub_11d3d0`, `sub_18e3d0`, `sub_98ae0` and many object accessors. Their thread is **unknown** beyond the chains above.

Also observed: `sub_14cf20` (0x14cf20, a request with a priority argument) is reached by the same three threads and
by `CGame::LoadInitialWorld`. It calls the pool's slot 13 and then RF slot 3 (BuildAsync) at 0x14d1d5. So
`sub_14cf20` is the **async** path, not a sync build (no slot-2 call in it).

### 2.8 Sync-warning switch and the ForceSync strings (observed)

- `res_showResourceSyncWarnings` (0xfa9303) is referenced only by `sub_149f00` (0x14a192), a cvar registration
  routine. Neither `sub_149f00` nor `BuildSync` calls `sub_14d7b0`. The consumer of the cvar is **unknown**.
- The string `ForceSyncBuildIfNeeded` (0xfb4a26) is used by `sub_1d2ec0`, `sub_1d32d0`, `sub_1d3410`, `sub_1d3580` and
  `sub_1d3670`. `sub_1d2ec0` has **no static caller** (pointer use). `sub_1d3410` is reached from `sub_1d32d0` (0x1d33b4),
  `sub_1d3580` (0x1d3638) and `sub_1d3670` (0x1d3733). So `ForceSyncBuildIfNeeded` does reach BuildSync, through
  `sub_14d7b0`, by a direct chain (section 2.7, main loop row).

### 2.9 Signatures (checked with `eboot_re.py sig`)

- `BuildSync` 0x146b60: 32 bytes, unique (`55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec d8 01 00 00 48 8b 1d ?? ?? ?? ?? 49 89 f5 49 89`).
- `sub_14d7b0` (EnsureBuilt): 16 bytes, unique.
- `sub_14d900`: 32 bytes, unique.
- `sub_4948b0`: 32 bytes, unique.
- `sub_187510`: 32 bytes, unique.
- `sub_1d32d0`: 32 bytes, unique.
- `sub_14cf20`: 24 bytes, unique.
- `sub_14fc00` (pool slot 13 getter): 32 bytes, unique.
- `sub_1d3410`: not unique in 32 bytes (5 matches). `sub_3c5630`: not unique in 32 bytes (3 matches). Use a longer
  signature, or hook their callees.

## 3. Hook proposals

All hooks follow `docs/HOOK-DESIGN.md`. None is declared yet. Check the `=2` log mode in that design before use.
The eboot uses frame pointers (`push rbp; mov rbp, rsp` in every function read), so `[rbp+8]` is a return address
and `[rbp]` is the saved frame pointer. A log hook can walk the chain 3-4 levels deep.

### H1. Log-only Wrap on BuildSync (highest priority)

- **Function:** `ResourceFactory::BuildSync`, 0x146b60, vtable slot 2 (RF vtable 0x1598528). Signature 32 bytes, unique.
- **Mode:** Wrap, log only, returns Continue. Log: thread id and name, the return address at `[rsp]` at entry and the
  caller chain from `[rbp+8]` (3-4 levels), the wrapper's `+0x98` state and `+0x20` data presence, and the wall
  time from entry to exit.
- **What it changes:** nothing.
- **Why it helps:** it answers the question at run time: which chain fires (update, render, main, area load), how
  often, and how long the sync build waits. It also tells whether the state precondition in `sub_14d7b0` is met
  in play.
- **What could break:** nothing, if log-only. One timestamp pair per call. The volume should be rate-limited (the
  count is unknown). A wrong signature disables the hook (safe).
- **Verify:** `=2`. Expect the caller chain to match one of the four rows in section 2.7. Count calls per second and
  compare with the `Shadow Game Update` and `Game Render` span counts in `SOTC_TRACE`.
- **Gain:** none; diagnostic. It decides H3 and H4.

### H2. Log-only Wrap on EnsureBuilt, sub_14d7b0 (cheaper than H1, counts the precondition)

- **Function:** `sub_14d7b0`, 0x14d7b0, 16-byte unique signature.
- **Mode:** Wrap, log only. Count the calls, the calls that pass the precondition (`+0x98 == 0xb`, `+0x20 == 0`,
  `+0x60 == 0`) and the time spent in those. Per thread.
- **What it changes:** nothing.
- **Why it helps:** it counts the candidate sync builds even when the build is skipped by the port. Compare with H1.
- **What could break:** nothing (log only). Called from about 550 functions, so the hook cost is per call; keep it cheap.
- **Verify:** `=2`; ratio of precondition-true calls to all calls, per thread.

### H3. Log-only Entry on sub_4948b0 (the update-thread request for GameCamera and AudioListener)

- **Function:** `sub_4948b0`, 0x4948b0, 32-byte unique signature.
- **Mode:** Entry, log only. Log the name string argument (the `'GameCamera'` or `'AudioListener'` pointer, passed in
  `rsi`/`rdx` from 0x5352ad or 0x535771; **the argument order is unknown** and must be checked before trusting the field),
  the thread, and the time.
- **What it changes:** nothing.
- **Why it helps:** it shows whether the update thread asks for the camera and audio listener every frame, and
  whether those requests are the first in an area (area entry).
- **What could break:** nothing (log only).
- **Verify:** `=2`; count per second; check the strings at area entry.

### H4. Bounded sync build on the update thread, only if H1 shows a long sync build there (active, risky)

- **Function:** `sub_14d7b0` (EnsureBuilt), 0x14d7b0.
- **Mode:** Wrap, active, only when the caller is `sub_4948b0` on the update thread. For a wrapper in state 0xb with
  no data, do not call the pool's slot 13 and RF slot 2; instead keep the request queued and return. The caller then
  reads the object before it is built.
- **What it changes:** the update thread does not wait for the file read and the decode.
- **Why it helps:** it removes the wait on the update thread, if that is a hitch source.
- **What could break:** `sub_4948b0` returns an object the caller uses at once (camera and audio listener). A
  deferred build means the caller may use an unbuilt object: a crash or a wrong camera for one frame. This is the
  main risk. A safer variant: do the build early on a worker, before the update thread asks (see proposal H5).
- **Verify:** H1 first. Then test in one scene (the first area's camera) with the caller's return value checked.
- **Gain:** **unknown**; depends on H1.

### H5. Prewarm the camera and audio listener wrappers before the update thread asks (port side, not a game hook)

- **Where:** `gpu/sotc/area_prewarm.cpp` (already exists) or a port call at area entry. The port would request the
  `'GameCamera'` and `'AudioListener'` resources early, so that `sub_4948b0` finds them built.
- **Mode:** not a hook. It needs the wrapper for those names (the lookup of `sub_4948b0` must be understood first,
  **unknown**).
- **What it changes:** the first update-thread call finds data present, so `sub_14d7b0` skips the sync build.
- **Why it helps:** no change to the calling code.
- **What could break:** the wrapper pointers are per area; a prewarm on the wrong area builds the wrong object.
- **Verify:** H1 and H3 show whether the update thread builds these at area entry.

## 4. Open questions

1. Frequency: does `sub_14d7b0` pass the precondition (state 0xb, no data) on the update thread in play, and how often
   (H1, H2)? This decides everything else.
2. The sub_4948b0 argument order: which argument is the name string (H3)?
3. Wrapper `+0x68` for wrappers not created by `GetResource` (`sub_14c9a0` writes 0): can those reach state 0xb with
   no data? If so, `sub_14d7b0` would call through null. The static read says they do not, but this is not proven.
4. Whether `AreaLoad_ShaderListAndAssets` runs once at boot or once per area (its only caller, `AreaLoad_PrecacheLists`,
   has no static caller). This decides whether the RF object and its pool are per area.
5. Thread of `AreaLoad_PrecacheLists` (0x533100) and `sub_25ddd0` (0x25ddd0): both have no static caller. The area
   state machine (`sub_344850`, report 10) may call them. Not read here.
6. The RF slot-2 scan covered only functions that reference 0x2ee2d00. A full image scan for `call [reg+0x10]` with
   an RF object is not done.
7. The cvar `res_showResourceSyncWarnings` (0xfa9303) has no consumer found: `sub_149f00` only registers it.
8. The meaning of the priority argument `0xd` passed by `sub_4948b0` to `sub_14cf20` (the mode branch in 0x14d196-0x14d1b4).

## 5. Summary (ranked)

1. H1 (log-only Wrap on BuildSync, 0x146b60, 32-byte unique signature): thread, return-address chain, time and state. Run `=2` in play.
2. H2 (log-only Wrap on EnsureBuilt, sub_14d7b0, 16-byte unique): count how often the sync precondition holds per thread.
3. H3 (log-only Entry on sub_4948b0): the GameCamera and AudioListener requests per frame and at area entry.
4. The update-thread path is the candidate hitch: sub_4948b0 -> sub_14d7b0 -> RF slot 2, called from ShadowGameUpdate (0x535310, 0x5357bd).
5. The render-thread path (sub_187510 at 0x187695) and the main-loop path (sub_1d3410 at 0x1d34d0) are the other two live candidates.
6. H5 (prewarm the camera and listener wrappers in area_prewarm) is the safe fix if H1/H3 show a long sync build on the update thread.
7. H4 (skip the sync build on the update thread) is risky: the caller uses the object at once. Only after H1 shows a long wait.
8. Gain: unknown until H1 runs.
9. Corrections to earlier reports: the RF global is the singleton created in AreaLoad_ShaderListAndAssets; BuildSync is reached through the pool (wrapper+0x68), not a direct RF call. `ForceSyncBuildIfNeeded` does reach BuildSync via sub_14d7b0.
10. The sync build is reached from a static chain from three threads, but its frequency in play is unknown.
