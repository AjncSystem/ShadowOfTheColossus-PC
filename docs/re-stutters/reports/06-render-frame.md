# 06 - Render frame structure and command submission

## 1. Answer

- The engine runs two render-side threads: "Game Render" (`RenderThread::ThreadFunction`, `0xbd6d0`) and "RenderFlip" (`0x960a0`). Game Render builds each frame as one job-system job (`0xcef90` -> `0xcefa0`, the frame body) and then waits on event flags before it starts the next iteration. **observed** (the loop structure), **inferred** (the exact meaning of each wait).
- The frame body is a fixed sequence of named phases: flip-state setup, skinning/texture-streaming submit, PreRender jobs, the `main cam` view (shadows, depth/G-buffer, decals, sun-shadow resolve), debug, post-FX prep, water/particle end-of-frame, and the final submit. **observed**.
- A frame is split into **6 static DCB submits**: 5 mid-frame submits (`Mode` 0) plus one final submit (`Mode` 1) at `0xcfd52`. Only the final one runs `sceGnmSubmitDone`, so SubmitDone is the frame boundary. **observed**.
- The command buffers are written by the engine through `sceGnm*` import calls wrapped by the `0x86xxxx` family (PM4 writers with a chunk-refill callback). The draw wrappers have no statically visible callers, so they are called indirectly. **observed** (wrappers), **unknown** (their callers).
- Frame pacing: the render side has at most one frame job in flight by design, and the port's `sceGnmSubmitDone` gate is what really limits how far the CPU runs ahead of the draw thread. Default port behaviour: 0 frames of run-ahead (see section 3). **observed** (port code), **inferred** (engine intent).
- Pipelining (1-2 frames ahead) is already partly implemented in the port (`SOTC_CP_AHEAD=2`, `SOTC_CP_AHEAD_FRAMES=0..3`, default off). The engine waits that matter are the event-flag waits in `ThreadFunction` (`0xbd717`, `0xbd908`, `0xbd93d`) and the SubmitDone gate. Parallel translation of the 6 submits is limited by shared context state between them.

## 2. Evidence

### 2.1 Threads and thread creation
- `0xfa0c00` "Game Render" is used only by `sub_bd170` (RenderThread init). It calls the thread creator `sub_3b5c0` with entry `0xbd6d0` (read from data `0x15fe778`), priority `0x2bc` (700), stack `0x100000`, and `this` = render object. **observed**
- `0xbd6d0` contains the string `d:\dev\cp10\Main\Code\BPE\Source\Renderer\Frontend\RenderThread.cpp` and the signature `int RenderThread::ThreadFunction(bpe::threading::Thread &)`. **observed**
- `0xf9f353` "RenderFlip" is used by `sub_95760` (flip setup). The thread entry is `0x960a0` (data `0x15fe968`), priority `0x2bc`, stack `0x100000`. It creates the event queue "flip event queue" (`0x95782`), registers `sceVideoOutAddFlipEvent`, and loops on `sceKernelWaitEqueue` / `sceVideoOutGetEventData` / `sceVideoOutGetEventId`. It exits on a user event (filter `-0xb`). It does not submit frames. **observed**
- Event objects (`sub_3b3d0`) call `sceKernelCreateEventFlag` with name `PS4SyncEvent` and attr `0x21`. This matches the log message `event flag 'PS4SyncEvent' waited 5s`. The helpers are `0x3b3d0` create, `0x3b450` set bit 1, `0x3b460` clear, `0x3b470` poll (exit check), `0x3b4a0` wait bit 1 (`sceKernelWaitEventFlag`). **observed**

### 2.2 Game Render loop (`0xbd6d0`)
Observed call order per iteration:
1. Entry: `0xbd717` `sub_3b4a0(rbx)` = wait event `[R+0]`.
2. Poll exit flag `T+0x18` (`0xbd728`, `sub_3b470`).
3. Build the job array: `sub_35480` (`BeginJobArray`) with entry `sub_cef90` and argument `R` (`0xbd8fe`).
4. `0xbd908` `sub_3b4a0([R+0x20])` = wait event `R+0x20`, before the kick.
5. Kick: `sub_35730` (`0xbd922`), which calls `sub_35590` then `jmp sub_34bd0` (name uncertain; `0x34bd0` uses the string `JobSystem::FreeCounter`).
6. `0xbd937` `call [0x1597a08+0x18]` = `0x358f0`, a bare `ret`.
7. `0xbd93d` `sub_3b4a0(rbx)` = wait `[R+0]` again.
8. Poll exit (`0xbd947`), then loop at `0xbd790`.

**inferred**: `[R+0]` (event A) is the "frame ready" signal from the game thread. `sub_bda00` (`0xbda00`, called from `0x112db0` and `0x113690`) sets A = `[R+0]` and `[R+0x62d0]`. `sub_bcf60` clears `R+0x10` and `R+0x20` and sets A. Event C (`R+0x20`) is the frame-job completion signal. The render job's end does `sub_3b450([r12+0x10])` and `sub_3b450([r12+0x20])` (`0xcfd76`, `0xcfd82`). **unknown**: whether `r12` (the job parameter block) is the same object as `R`. A runtime trace of the event handles resolves this.

### 2.3 Frame body `sub_cefa0` (3.6 KB, phases by `sub_16b8c0`/`sub_16b900` scope names)
In call order:
1. `sub_118440` (`0xcf10f`) -> `sub_90860`: flip-state setup (mode check, EOP-style flip packet; `sub_95690` is reached only on mode change).
2. Scene update: `sub_92140`, `sub_cedf0` (x3), `sub_19fed0`, `sub_16c500`, `sub_187510`, `sub_16bb70`, `sub_e2500`, `sub_c95c0`, `sub_bc010`, `sub_bbb00`.
3. `sub_cfdb0` (`UpdateSkinningRenderObjects`, `UpdateTextureStreamingGPUData`): 2 mode-0 submits (`0xcfe56`, `0xcff01`).
4. Job `MeshTessellator::PreRenderJob` (`sub_c12a0`, `0xcf756`), `sub_d0040` (VirtualGeometryDisplacement::UpdateJob, WaterRendering::ResetJob, FurRendering::UpdateJob, ParticleRenderer::UpdateSimulationJob, six `sub_c12a0` jobs).
5. Scope `main cam`: `sub_caed0` (`0xcf88f`). Its jobs are VirtualGeometryRendering::PreRenderJob, ForwardRendering::CreateClustersJob, FurRendering::PreRenderJob, ParticleRenderer::UpdateLightmapsJob, FogRendering::UpdateJob, PreDebug. Within it: `sub_cc9b0` (RenderShadows, CascadedShadows, RenderShadowCascade 0-4, RenderShadowSpot 0-3, CullSunShadowCascade 0-4), `sub_c59d0` (RenderDepth, RenderGBufferBakedPerVertex/PerProbe, RenderDecal, LightRendering::ResolveSunShadowJob), and two mode-0 submits (`0xcb00b`, `0xcb0b2`).
6. `sub_c5560` (submit) is also called from `sub_c53d0` (XPR_IndexCullObjectsNormal, XPR_ProjectedDecalCull) and `sub_cfdb0` (see step 3). `sub_ccdf0` (RenderCubeMapFace) reaches the submit only through `sub_caed0`.
7. `sub_d04e0` (`PostProcessing::AsyncPreparePostFX`, `sub_b9610` PostProcessing), `sub_be1f0` (SetViewParams).
8. Scope `EndFrameViewParams`, then `EndFramesWaterAndParticle`: `sub_da110`, `sub_e48b0` (`WaterReadQueries`: emits a PM4 packet via `0x86edc0`, then `sub_8c1b0`, so a GPU-written result is read in the frame), `sub_109970`, `sub_1038a0`.
9. `sub_118470` (`0xcfd45`): `sub_91f50` -> `sceGnmInsertWaitFlipDone` into the DCB.
10. `sub_c5560(params, Mode=1)` (`0xcfd52`) -> `sub_d07c0` = `RenderThread::KickoffSubmitJob`.
11. `sub_ccfd0`, `sub_90ea0`, two `sub_3b450` signals.

### 2.4 Submit path (`sub_c5560` -> `sub_d07c0` -> PS4 SDK)
- `sub_c5560` allocates a 0x610-byte `SubmitJobParams` (`sub_17170`). It stores `mode` at `+0`, the DCB vector at `+0x210..0x218`, the CCB vector at `+0x410..0x418`, and the parameter block (`+0x10`, `+0x48`) from its caller. **observed**
- `sub_d07c0` (KickoffSubmitJob): waits on job handles (`sub_35590`), then for each DCB calls `sub_91f80` -> `sub_8636e0` -> `sub_882ac0` -> `jmp sceGnmSubmitCommandBuffersForWorkload` (`0xf909b0`). For each CCB it calls `sub_16f100` -> `sub_85d600`. Only when `mode == 1` does it ring the compute doorbell (`sub_91f90` -> `sub_863aa0`) and then call `sub_882ce0` -> `jmp sceGnmSubmitDone` (`0xf909c0`). **observed**
- Engine imports that are direct import calls (`call` stubs): `sceGnmDrawIndex` (4 sites), `sceGnmDrawIndexAuto` (2), `sceGnmDrawIndexOffset` (2), `sceGnmDrawIndexIndirect` (2), `sceGnmDispatchDirect` (4, incl. `sub_883800`/`sub_883870`), `sceGnmSetVgtControl` (4), `sceGnmSetPsShader350`, `sceGnmSetCsShaderWithModifier` (3), `sceGnmInsertWaitFlipDone` (1, `sub_86f700`). **observed**. The index shows 0 callers for `sceGnmSubmit*` because the calls go through the `jmp` thunks above.
- `sceGnmSubmitAndFlipCommandBuffers`, `sceGnmRequestFlipAndSubmitDone` and `sceVideoOutSubmitEopFlip` are not called from the engine's code paths I could reach. `sceVideoOutSubmitFlip` (`0xf900c0`) is called only from `sub_956e0` (flip-mode change and teardown). **observed**. The per-frame flip path is **unknown** (see open questions).

### 2.5 Command writer (PM4 builder)
- The writers share a layout: `[obj+8]` end, `[obj+0x10]` cursor, `[obj+0x18]` chunk-refill callback, `[obj+0x20]` context. Example: `sub_86f700` (wrapper of `sceGnmInsertWaitFlipDone`, 7 dwords): if fewer than 7 free dwords it calls `[obj+0x18]`, then writes the packet and adds `0x1c` to the cursor. `sub_86edc0` uses the same pattern (6 dwords). **observed**
- The draw op wrappers are `sub_86a9d0`, `sub_86aab0`, `sub_86adc0`, `sub_86ae30` (DrawIndex), `sub_86abb0`, `sub_86ac10` (DrawIndexAuto), `sub_86afd0`, `sub_86b040` (DrawIndexOffset), `sub_86b340`..`sub_86b510` (DrawIndexIndirect and CountMulti), `sub_86b970`, `sub_86b9e0`, `sub_883800`, `sub_883870` (DispatchDirect), `sub_86c950`..`sub_86ca90` (SetVgtControl), `sub_86c050` (SetPsShader350), `sub_86c7d0`, `sub_883e60`, `sub_883ed0` (SetCsShader). **observed**. Their callers are not statically visible: no data-section pointer to them, and no direct call.
- Chunks: `sub_c5560` gives each submit its own DCB list. The per-frame `RenderSingleFrameGPU` heap (32 MiB chunk) is from the brief; `sub_8c880` holds the labels `GameToRenderThread`, `GameToGPU`, `CPU_RenderToGPUToGame` and `RenderSingleFrameGPU`, which look like frame-stage profiler labels. **inferred**

### 2.6 Port side (`gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp`)
- `sceGnmSubmitCommandBuffersForWorkload` (`:2429`) and `sceGnmSubmitAndFlipCommandBuffersForWorkload` (`:2374`) take `m_submit_lock` and call `GateWait(0)`. `sceGnmSubmitDone` (`:2475`) calls `GateWait(2)`, then either sets `submission_lock` (mode 0/1) or uses the frame tickets (mode 2), then `liverpool->SubmitDone()` and `++frames_submitted`.
- `GateWait` (`:171`): mode 0/1 waits for the GpuIdle IRQ (`WaitGpuIdle`, `:86`) or for `WaitCommandsConsumed()`. Mode 2 waits until `marked - lag` frames are recorded, with `lag = SOTC_CP_AHEAD_FRAMES` (default 1, clamped 0..3, `:148`). **observed**
- `Liverpool::CpAheadMode()` (`liverpool.cpp:4383`) defaults to 0 (off). `frame_tickets = CpAheadMode() == 2` (`liverpool.cpp:183`).
- The port's comment (`gnmdriver.cpp:116-143`) says the guest blocks in the submit thread, and that the game's CPU writes after a fence are not ordered by this gate. So a frame's CPU build can run while the draw thread still works on the previous frame, but only the frame job's own thread waits.
- `sceVideoOutSubmitFlip` is at `videoout/video_out.cpp:164`; `sceVideoOutSubmitEopFlip` (`:355`) is a GPU-IRQ-driven flip, not imported by the eboot.

## 3. Stutter mechanism

- **Serial frame, serial submit.** The engine builds frame N in one job and waits for it (`0xbd93d`) before it begins N+1. Inside the job, the submit at `KickoffSubmitJob` blocks in the port's `sceGnmSubmitDone` until the draw thread has drained what is queued (default gate). So the game's frame time is the CPU build plus the draw thread's backlog, and any draw-thread spike (the 18 ms median and the 500 ms outliers in the brief) is seen by the Game Render thread directly. **inferred** from code, **observed** that the default gate is `WaitGpuIdle`.
- **Five mid-frame submits.** Each mid-frame submit (`Mode` 0) hands a DCB to the port. The port parses them on the CP thread and records them on the single draw thread. Any fence that the game places between these submits makes the next DCB wait on the draw thread. **inferred**.
- **Readbacks inside the frame.** `WaterReadQueries` (`sub_e48b0`) reads a GPU-written result during `EndFramesWaterAndParticle`. Any readback there is a CPU-visible wait on the draw thread in the same frame. The XPR occlusion path is covered by another topic. **observed** (call), **inferred** (GPU-written result).
- **Vblank spin on mode change.** `sub_956e0` spins on `sceVideoOutIsFlipPending` and calls `sceVideoOutWaitVblank` (`0x95700`-`0x95712`). It runs only on flip-mode changes, so it is rare. **observed**

## 4. Hook / redesign options

1. **Let the port run the frame job ahead (smallest change, already present).** Set `SOTC_CP_AHEAD=2` and `SOTC_CP_AHEAD_FRAMES=1`. The gate moves from `sceGnmSubmitDone` (`gnmdriver.cpp:2481`) to frame tickets (`liverpool.cpp:2535-2702`). Expected effect: one frame of run-ahead for the draw thread. Risk: the port's own comments say the draw thread may read guest memory the game rewrites without a fence; the game's double-buffered constants (SRenderJobParams, the 0x610-byte SubmitJobParams) must be safe at lag 1. Verify: `tools/sotc_bench_attract.sh` with `SOTC_CP_AHEAD` 0/2 and frame-time histogram; check the `SOTCSTALL` lines for `gate` time.
2. **Hook the two engine waits on the frame.** `sub_3b4a0` is called from `ThreadFunction` at `0xbd717`, `0xbd908`, `0xbd93d`. A port hook at those call sites (by guest address, since the wrapper is shared with ~40 callers) could replace the frame-completion wait with "return when the draw thread has consumed the frame's last label", i.e. a per-frame ticket. That lets the Game Render thread start frame N+1 while the GPU still runs N, without changing the engine. Risk: `R+0x20` (event C) gates the job array before the kick; its producer is not identified. Verify by logging the event handles and the set/wait order for one frame.
3. **Split the frame's submits across threads.** The five mid-frame submits come from `sub_cfdb0`, `sub_caed0`, `sub_c59d0`, `sub_c53d0` and `sub_c5560` and are ordered by DCB order in the job. A parallel translation needs per-submit independence: the context registers and constant buffers are shared across the view, shadow and G-buffer passes. Realistic first step: parallel PM4 parse (already in `SOTC_PARALLEL_RECORD`) plus per-pass Vulkan secondary command buffers. Not recommended before options 1-2 are measured.
4. **Radical option: change how the engine submits.** Replace `sceGnmSubmitCommandBuffers` in `sub_882ac0` with a port-side copy of the DCB (the port already has `BB_COPY_GPU_BUFFERS` for copies of this kind) and let the port's frame ticket own the lifetime of that copy. Then the engine's DCB buffers can be reused one frame early, which removes the lock in `KickoffSubmitJob`. Risk: `sub_c5560` allocates DCBs per submit; the port must keep the copy until the draw thread consumes it. Verify with the existing `BB_COPY_GPU_BUFFERS` path.

## 5. Proposed names

```
0xbd170 RenderThread::Init
0xbd6d0 RenderThread::ThreadFunction
0xcef90 RenderThread::FrameJobEntry
0xcefa0 RenderThread::RenderFrame
0xbda00 RenderThread::KickFrame (game -> render)
0xbcf60 RenderThread::ResetFrameEvents
0xc12a0 RenderThread::RunFunctionAsRenderJob
0xc5560 RenderThread::SubmitJob (allocates SubmitJobParams)
0xd07c0 RenderThread::KickoffSubmitJob
0xcaed0 RenderView::MainCamera
0xcc9b0 RenderShadows
0xc59d0 RenderDepthAndGBuffer
0xc53d0 RenderCullXPR
0xcfdb0 RenderSkinningAndStreaming
0xcedf0 RenderPhase (unknown)
0x8636e0 GfxCmdList::Submit (wrapper)
0x882ac0 GfxCmdList::SubmitThunk
0x882ce0 GfxCmdList::SubmitDoneThunk
0x86f700 CmdBuf::InsertWaitFlipDone
0x86edc0 CmdBuf::EmitPacket (unknown opcode)
0x86a9d0 CmdBuf::DrawIndex
0x86abb0 CmdBuf::DrawIndexAuto
0x86b970 CmdBuf::DispatchDirect
0x86c950 CmdBuf::SetVgtControl
0x3b3d0 SyncEvent::Create (PS4SyncEvent)
0x3b450 SyncEvent::Set
0x3b460 SyncEvent::Clear
0x3b470 SyncEvent::Poll
0x3b4a0 SyncEvent::Wait
0x3b5c0 Thread::Create
0x35480 JobSystem::BeginJobArray
0x34b10 JobSystem::AllocateCounter
0x35730 JobSystem::KickArray (name uncertain)
0x95760 Flip::Setup
0x96f50 Flip::GetBufferIndex (reads [obj+0x198])
0x960a0 RenderFlip::ThreadFunction
0x956e0 Flip::SubmitAndWaitVblank
0x90860 Flip::SetupFlipState
0x118440 Flip::FrameStart
0x118470 Frame::InsertWaitFlipDone
0xe48b0 WaterRendering::ReadQueries
0x16b8c0 Profiler::PushScope
0x16b900 Profiler::PopScope
```

## 6. Open questions

1. **Object identity of the event flags.** Whether `R+0` (A), `R+0x20` (C) and the frame job's `r12+0x10` / `r12+0x20` are the same object. Settle with a runtime trace: log the `sceKernelSetEventFlag` / `sceKernelWaitEventFlag` handles with the return address for one frame (the port's `PS4SyncEvent` path in `kernel`).
2. **Per-frame flip.** The engine imports only `sceVideoOutSubmitFlip` in the mode-change path, and never `sceGnmSubmitAndFlipCommandBuffers`. The per-frame flip is not identified. Candidates: the EOP flip packet built by `sub_90860` (constant `0x4100000`), or a flip requested through a `sub_16b10` ring. Settle by tracing the PM4 packets that the draw thread sees before the first `sceVideoOutSubmitFlip`.
3. **Callers of the draw wrappers.** Indirect calls through a function table not visible to the index. Settle with a trace of return addresses in `SotcTrace` for `sceGnmDrawIndex`, or by a `refs` search for the table.
4. **Per-frame op counts.** The port's 4,400 ops/frame cannot be split by pass from the static side. Count with `SotcTrace` by return address (`__builtin_return_address(0)` already in `gnmdriver.cpp`), grouped by the phase scope.
5. **Frames in flight in the engine.** Whether `SRenderJobParams` and `SubmitJobParams` are double-buffered. Only `sub_c5560` (0x610 bytes) was read; the allocation pattern across frames is unverified.
6. **Other topics seen in passing**: `sub_e48b0` (water query readback, topic: readbacks), `sub_956e0` vblank spin (topic: presentation), `sub_bda00` game-to-render kick (topic: game thread).
