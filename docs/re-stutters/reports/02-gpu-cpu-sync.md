# 02 - Every point where the CPU waits for, or reads from, the GPU

Addresses are eboot VAs. `out/re/decomp` was empty, so everything is from disassembly, strings and the port source. Labels: **observed**, **inferred**, **unknown**.

## 1. Answer

- No render-code loop spins on a GPU-written label. GPU-to-CPU coupling is frame-level: the update thread, a "Game Render" thread and the GPU run a handshake built on BPE event flags (**observed**). Whether it is depth 1 is **inferred** (see open question 2).
- Top stall candidate: the per-frame game/render handshake. The update thread waits on the render thread at the top of the frame function (0x25aead). The render thread waits on its gates twice per loop (0xbd717, 0xbd93d). If these are not overlapped, the whole render job (~18 ms median draw per the brief) serialises with simulation.
- Second: `RenderDeferredMemoryDeleter::FlushPendingFree(wait=true)` (0x8ef20) on the render backend frame. It waits on a job counter with spin-then-`usleep(1)`. It is the only wait I found designed to hold for GPU completion, but what decrements its counter is **unknown**.
- Third: the gnmx compute-queue-full retry (0x863f80) spins with no pause or sleep. Not hit in the five dev-copy logs.
- GPU-side waits (`sceGnmComputeWaitOnAddress`, `sceGnmInsertWaitFlipDone`) become PM4 `WAIT_REG_MEM` and do not stall the CPU. `sceGnmComputeWaitSemaphore` is a port stub that emits nothing (correctness, not stall).
- No occlusion, histogram, exposure or particle-count readback was confirmed.
- The 95 `sceKernelUsleep` callers are sound middleware (sceSndx), not render.

## 2. Evidence

### 2.1 Event flag primitives (observed)

| Address | Proposed name | Behaviour |
|---|---|---|
| 0x3b3d0 | `EventFlag_Init` | `sceKernelCreateEventFlag("PS4SyncEvent")`; handle at +0, mode byte at +8 (string 0xf97df3, one user). |
| 0x3b450 / 0x3b460 | `EventFlag_Set` / `_Clear` | `sceKernelSetEventFlag(h,1)` / `ClearEventFlag(h,~1)`. |
| 0x3b470 | `EventFlag_Poll` | `sceKernelPollEventFlag`. |
| 0x3b4a0 | `EventFlag_WaitInfinite` | `sceKernelWaitEventFlag(h,1,mode,NULL,NULL)`: **no timeout**. |
| 0x3b4e0 | `EventFlag_WaitMs` | Timed variant; no direct callers found. |
| 0x3b570 | `Sleep_Usec1` | `jmp sceKernelUsleep`. Common thunk for all BPE spin-then-sleep loops. |

Port: `evf_wait_common` (`src/runtime_sotc.c:375`) uses a condition variable per waiter, so no spin in the port itself. Waits over 5 s print a warning.

### 2.2 Render thread and frame handshake (observed calls, inferred ordering)

- `sub_bd170` (RenderThread ctor, called from `sub_112db0`): creates flags at this+0x00, +0x10, +0x20; starts thread "Game Render" (0xbd2d2, `sub_3b5c0`) whose object is at this+0x62b8 (quit flag +0x62d0).
- `sub_bd6d0` = RenderThread::ThreadFunction (string 0xfa1672). Per loop: `WaitInfinite(gate)` 0xbd717; `Poll(quit)` 0xbd728; job create `sub_35480` ("RenderJob") 0xbd8fe; job counter wait `sub_35730` 0xbd922; frame via vtable `[+0x18]` 0xbd937; `WaitInfinite(gate)` 0xbd908 and 0xbd93d; `Poll(quit)` 0xbd947.
- `sub_bcde0` / `sub_bcf60` (release/kick): clear gates +0x10 and +0x20 (0xbcf3a, 0xbcf43), set +0 (0xbcf84). `sub_bda00` (dtor): sets quit flags.
- Timed gate wrappers `sub_bcf90`, `sub_bd010`, `sub_bd0a0`, `sub_bd110`, `sub_bdf20`: bracket the wait with rdtsc (`sub_323d0`/`sub_32430`) and add elapsed seconds to render obj+0x30. Callers: `sub_bd0a0` from `sub_25ae80` (0x25aead, frame start), `sub_534690` (0x53491e), `sub_536480`; `sub_bcf90` from `sub_f9530`, `sub_25ddd0` (0x25df83), `sub_26fcf0`; `sub_bd010` from `sub_a2aa0`.
- `sub_25ae80` (frame update): first action after entry is the render-gate wait `sub_bd0a0`.
- `sub_534690` ("Shadow Game Update", string 0x5346f6): `WaitInfinite([r13]+0x10)` 0x534800; `Clear` 0x53480d; work; `sub_bd0a0` 0x53491e (conditional); `Set([r13])` 0x534960; `WaitInfinite([r13]+0x10)` 0x53496d.

### 2.3 Render backend and deferred free (observed)

- `sub_90ea0` (RenderBackend frame, called from frame job body `sub_cefa0` at 0xcfd6c): timer; acquire of a counter at `[r14]` with `lock cmpxchg`, spinning to 10,000 iterations (0x90fa4) then `usleep(1)` (0x90fb2); then `sub_8ef20` (0x90fc7); `sub_8c860`.
- `sub_8ef20` = `RenderDeferredMemoryDeleter::FlushPendingFree(bool wait)` (string 0xf9ef87). If `wait` and the counter at `[rbx+0x80]` is non-zero, calls `sub_35730` (0x8ef80) = `sub_35590` (`JobSystem::WaitForCounter`): spin with `pause` to 0x3e8 iterations, then `usleep(1)` (0x356cd). Then rotates the deferred-free buckets. Three buckets suggest the engine expects about two frames of GPU lag (**inferred**).
- Port: `sceKernelUsleep` = `sleep_ns` = high-resolution waitable timer on Windows (`src/compat_win.c:115-127`). `usleep(1)` therefore costs about 0.1-1 ms, not 1 us.

### 2.4 Usleep callers (observed)

95 direct callers. `mov edi` before the call is 0x63 (99 us) for almost all, 0x3e8 (1 ms) for some. Nearby strings are `sceSndx*`, `GetFadeInfo`, `SetXfaderById`, `Track%d`, `sceSndxShutdown` (0x95c330-0x9b36e0). **Sound middleware, not render.**

### 2.5 Compute queue full spin (observed)

- `sub_863af0` = gnmx `internalSubmitAndFlip` (`lwgfxcontext.cpp`, 0x863f46). When `sub_85d380` returns busy (0x80000001), it logs "ComputeQueue for dispatch draw is full; waiting for space..." once (0x863f54), then loops calling `sub_85d380` until not busy (0x863f80-0x863f95). **No pause, no sleep.**
- `sub_85d380` calls `sub_85d3d0` (space check, reads a dword at `[rsi]`, likely the GPU-written ring read pointer, **inferred**) and on success `sub_882bd0` = `sceGnmDingDong`.
- Not present in `user/last_run*.log` (five files). Install logs not checked.

### 2.6 GPU-side waits and the port (observed)

| Import | Engine caller | Port | CPU stall? |
|---|---|---|---|
| `sceGnmComputeWaitOnAddress` | `sub_884d40` (0x884d99) | `gnmdriver.cpp:281`: `WAIT_REG_MEM` | No |
| `sceGnmInsertWaitFlipDone` | `sub_86f700` (0x86f749) | `gnmdriver.cpp:1268`: `WAIT_REG_MEM` on flip label | No |
| `sceGnmComputeWaitSemaphore` | `sub_885320` (0x885369) | `gnmdriver.cpp:307`: **stub, emits nothing** | Dropped wait |
| `sceGnmGetPhysicalCounterFromVirtualized` | `sub_87bd00`/`87bd70`/`87bde0`/`87be50`, `sub_882c70` | `gnmdriver.cpp:1094`: returns error | Unknown reaction |

`sceGnmSubmit*` show 0 direct callers because gnmx reaches them through thunks (`sub_882ad0`, `sub_882ae0`).

### 2.7 Flip path (observed)

- `sub_956e0` (flip-and-wait): `sceVideoOutSubmitFlip(h,-1,1,0)` (0x956f8); then `while (sceVideoOutIsFlipPending(h)) sceVideoOutWaitVblank(h)` (0x95700-0x95712); then `sceKernelTriggerUserEvent` (0x9571f). Callers `sub_95690` (0x956aa), `sub_96010`.
- `sub_960a0` consumes the flip equeue with blocking `sceKernelWaitEqueue` (0x960f2). Setup in `sub_95760` ("RenderFlip").
- Vblank-bound by design; cost depends on the port's `sceVideoOutWaitVblank`/`IsFlipPending` (not checked).

### 2.8 Other waits (observed, not analysed)

- `sceKernelWaitEventFlag` direct callers: `sub_949f80`, `sub_993220`, `sub_998630`, `sub_9b2ac0` (0x94-0x9b, sound area). Logs name `sndx_stream_schedular_notify` and `sceSndxLoaderNotify` (**inferred**: sound).
- `sub_1469d0` = `ResourceFactory::WaitForAllCmdsToFinish`. Caller `sub_25ddd0` (0x25dfa6) also calls the render gate `sub_bcf90` (0x25df83): one barrier waits for render and resources (**inferred** area-change stall).

### 2.9 Logs (observed)

- `PS4SyncEvent waited 5s/10s/15s`: 8-11 lines per log; `user/last_run.log:229-232` sits beside `ProcessGraphics` warnings, so boot or idle, not per-frame (**inferred**).
- `SOTCSTALL` lines (3 in `user/last_run.log`); their "draw waits" are port-side, not game waits.

### 2.10 Readback candidates (unverified)

- `ParticleSystemPool::ProcessStats` `sub_1018c0` (called from `sub_109970`). Name suggests GPU stats; no GPU-memory read confirmed.
- `ComputeExposure` `sub_b3590` (string 0xfa0700): dispatches via `sub_8619a0`/`sub_861fe0`; no readback confirmed.
- `mValidParticleCount` (`sub_d5a990`) is PhysX, CPU-side.

## 3. Stutter mechanism

1. **Serialisation (highest).** Sim waits for render at frame start (0x25aead). If the wait at 0x53496d is same-frame (open question 2), sim and render never overlap, so each frame costs sim + render. Any render-side stall lands on the game thread. (**inferred**)
2. **Spin then usleep(1).** Render-side counter waits burn ~1,000-10,000 spins, then sleep 0.1-1 ms per poll. Long counter waits cost many ms but not hundreds unless the counter is slow. (observed code, inferred cost)
3. **Compute ring full.** Tight retry on the submitting thread; wall time only while full, but saturates a core. (observed code)
4. **Area-change barrier** `sub_25ddd0` waits on render and resource loading together. (**inferred**)

## 4. Hook / redesign options

1. **Decouple sim from render (depth 2+).** Relax the waits at 0x25aead and 0x534800/0x53496d (if they are same-frame acks) so sim(N+1) runs while render(N) runs; index per-frame buffers (`RenderSingleFrameGPU`, `RenderContextFrameGPU`, the deferred-free buckets) by frame mod 2 or 3. Risk: render reads sim state (`SRenderJobParams`). Verify: `SOTCSTALL` counters and obj+0x30 per frame, before and after.
2. **Measure first (no engine change).** Read render obj+0x30 once per frame (written by the timed gate wrappers) to quantify handshake time. Decides option 1.
3. **Single patch point for usleep spins.** `sub_3b570` serves `sub_35590`, `sub_34bd0`, `sub_90ea0`. Replace in the port with an event or futex-style wait on frame end. Expected: removes 0.1-1 ms granularity and spin CPU. Risk: low. Verify: count calls and per-frame time.
4. **Compute ring.** Hook the retry at `sub_863af0` 0x863f80: block in the port's submit until ring space frees, or enlarge the ring. Verify: count busy returns of `sub_85d380`.
5. **Implement `sceGnmComputeWaitSemaphore`** as a real PM4 wait (`gnmdriver.cpp:307`). Correctness, not stall.
6. **Radical.** Double-buffer the draw list per frame so sim(N+1) and draw(N) run in parallel, and move the flip wait (`sub_956e0`) off the render thread. With option 1 this is the PC-native pipeline. Game logic does not read GPU results in these paths, so lag costs nothing there, provided the XPR agent confirms its readbacks can lag one or two frames.

## 5. Proposed names

- 0x3b3d0 EventFlag_Init; 0x3b450 EventFlag_Set; 0x3b460 EventFlag_Clear; 0x3b470 EventFlag_Poll; 0x3b4a0 EventFlag_WaitInfinite; 0x3b4e0 EventFlag_WaitMs; 0x3b570 Sleep_Usec1
- 0xbd170 RenderThread_Ctor; 0xbd6d0 RenderThread_ThreadFunction; 0xbcde0 RenderThread_Kick; 0xbcf60 RenderThread_SignalGate; 0xbda00 RenderThread_Dtor
- 0xbcf90, 0xbd010, 0xbd0a0, 0xbd110, 0xbdf20 RenderThread_WaitGateTimed_{A..E}
- 0x25ae80 GameFrame_Update; 0x534690 ShadowGameUpdate_ThreadFunc; 0x536480 GameUpdate_FrameGate
- 0x90ea0 RenderBackend_Frame; 0x8ef20 RenderDeferredMemoryDeleter_FlushPendingFree
- 0x35590 BpeJobSystem_WaitCounter; 0x34bd0 BpeJobSystem_FreeCounter; 0x35730 BpeJobSystem_WaitCounterWrapper
- 0x863af0 Gnmx_InternalSubmitAndFlip; 0x85d380 Gnmx_ComputeQueue_TrySubmit; 0x85d3d0 Gnmx_ComputeRing_ReserveSpace
- 0x884d40 Gnmx_ComputeWaitOnAddress_Emit; 0x885320 Gnmx_ComputeWaitSemaphore_Emit; 0x86f700 Gnmx_InsertWaitFlipDone_Emit
- 0x956e0 Flip_SubmitAndWaitVblank; 0x960a0 FlipEvent_WaitEqueue; 0x95760 RenderFlip_Setup
- 0x1469d0 ResourceFactory_WaitForAllCmdsToFinish; 0x1018c0 ParticleSystemPool_ProcessStats (unverified)

## 6. Open questions

1. Which flag does each side set and wait on? Map `sub_534690`'s `[r13]` and the RenderThread object (passed to `sub_3b5c0` at 0xbd2d2). The port traces events when `f->traced` is set (`runtime_sotc.c`); one traced run gives the answer.
2. Is the wait at 0x53496d same-frame or previous-frame? The same trace answers it. Same-frame means the frame is fully serial and option 1 is the top fix.
3. What decrements the counter at render obj+0x80 (used by `sub_8ef20` and `sub_90ea0`)? If a GPU label drives it, this is the real GPU-to-CPU wait for frame completion. Check callers `sub_8ed90`, `sub_8ee00`, `sub_910d0` (via `sub_90050`).
4. Compute-ring-full frequency in the user's install logs (`<install>\user\last_run*.log`).
5. Does the game react to `sceGnmGetPhysicalCounterFromVirtualized` failing (`sub_882c70` fallback)?
6. Confirm or drop `ProcessStats` (0x1018c0) and `ComputeExposure` (0xb3590) as readbacks by tracing what they read.
