# 01 - XPR occlusion / visibility cull, end to end

## 1. Answer

- The XPR cull is two GPU jobs per frame plus a CPU consumer. (a) `IndexCullXPRObjects` (0x11dbe0) clears a dispatch-draw buffer and runs compute dispatches (sceGnmDispatchDirect) per object set. (b) `XPRPreRender` (0xca6d0) queues `BPXPR_RunCullPasses` (0xc6670), which runs the shadow-scene mask cull (0xc6bd0) and the main-scene mask cull (0xc6f70). Each cull ends with an EVENT_WRITE_EOP (event 0x28, BOTTOM_OF_PIPE_TS, 32-bit data 1) to a label in the per-frame context.
- The GPU-side waits in the command stream are WAIT_REG_MEM packets (0x86edc0, function 5 = GTE, ref 1). They gate the GPU, not the CPU.
- The CPU consumer is 0x1a6d20. It scans the visibility bitset word by word (blsi/lzcnt) and records one sceGnmDrawIndexIndirect (wrapper 0x86b3d0) per visible object. The draw list therefore depends on mask bits at record time. This is the CPU-visible round trip.
- **Open, and the most important point:** I found no CPU spin or event wait on the CS label anywhere on the consumer path. The consumer reads the mask memory directly during the RenderDepth/GBuffer job. Either the game waits in a place I did not locate, or the masks are consumed one frame later than the label suggests. The port's readback fault is currently the only real wait.
- The port already measured that 78-89% of the XPR mask readbacks find the GPU finished (CHANGELOG, `SOTC_RB_MATERIALIZE`). So most of the stall is the fault/draw-thread round trip, not GPU execution time.
- 1-frame latency is feasible in the engine with one argument swap at the consumer (hook 0x1a6d20, pass a previous-frame copy of the mask). The port already has an experimental form (`SOTC_RB_PREV`) with no recorded measurement.
- The radical option is GPU-driven selection: a compute pass compacts visible object draws into a buffer and the game issues DrawIndexIndirectCountMulti (wrapper 0x86b470 exists but is unused by the consumer). The CPU would never read a mask.
- A CPU-side cull replacement is not viable. The occluder test depends on the occluder depth written this frame (inferred).

## 2. Evidence

Status tags: **observed** (code/strings/logs), **inferred**, **unknown**.

### 2.1 Cvars and enums (observed)
- `RegisterXprCvars` = sub_6efe0 (0x6efe0). It registers: `XPR Occluder Debug Mode` (0x6fc30), `enableNonPower2DepthOccluders`, `dumpXPRInfoStats` (0x6fe11), `useSunShadowIndices` (0x6fef1), `doIndexBufferCulling` (0x6ffd9), `doSunShadowIndexBufferCulling`, `enableSmallPrimitiveIndexBufferCulling`, `forceUseLastIndexBufferCullingResults` (0x70289), `drawJustOccluders`, `enableOccluderRandomClearColor`. Defaults are stored by the constructors and were not decoded.
- sub_5bd70 (0x5bd70) only builds the `EXPROccluderDebugMode::{Off,OccluderDepthBuffer,ShowOverdraw}` name table (static init, guard at 0xf8f3d0), then tail-jumps to 0x5c520 (reflection). It is not cull logic.
- `r_EnableXPR` is read in sub_277560 (0x277560).

### 2.2 Dispatch and the CPU wait primitives (observed)
- `sceGnmDispatchDirect` wrapper 0x86b9e0 (import at 0x86ba40). Callers include 0x928f0 (site 0x92ac6) and 0x92bd0 (site 0x92e47).
- `sub_92b50` (0x92b50): calls 0x928f0 (dispatch), then 0x86eb60 (EOP), then tail-jumps to 0x86edc0 (WAIT_REG_MEM, mode 5, ref 1, mask 0xffffffff). Used by IndexCull.
- `sub_925c0` (0x925c0): the XPR completion label writer. It calls 0x86eb60 with event 0x28, data 1, 32-bit. The port's comment at `liverpool.cpp:1815` says CS_DONE (0x2f). The eboot uses 0x28, so that comment is wrong and `liverpool.cpp:3483` is right.
- `sub_86eb60` (0x86eb60): emits PM4 header 0xC0044700 (EVENT_WRITE_EOP).
- `sub_86edc0` (0x86edc0): emits PM4 header 0xC0053C00 (WAIT_REG_MEM).
- `sub_86ad40`, `sub_86b3d0` (DrawIndexIndirect wrapper), `sub_86b470` (DrawIndexIndirectCountMulti, no caller in the consumer).

### 2.3 Index-cull dispatch (observed)
- `IndexCullXPRObjects` = sub_11dbe0 (0x11dbe0). Strings: `IndexCullXPRObjects` (0xfa6818), `Clear Dispatch Draw Buffer` (0x11de33). It allocates from the frame heap (sub_12820), dispatches through 0x92b50, and calls 0x19b330 per object.
- Callers: sub_c5110 (job body, string `XPR_IndexCullObjectsNormal`), queued by sub_c5130 with flag 8 and gated by config byte `[cfg+0xae3c]`. Other callers: sub_cdb00, sub_ceae0.

### 2.4 Mask cull passes (observed, with one inference)
- `XPRPreRender` = sub_ca6d0 (0xca6d0). It clears the CPU-side labels `[ctx+0xe8]+0x40` and `+0x68` to 0 (0xca99b, 0xca9a3), queues a job with callback 0xc6670, and emits GPU waits on `[ctx+0xe8]+8` and `+0x10` (0xcaa2c-0xcaa73). It then queues `RenderRainBlockers` and `RenderParticleBlockers`.
- `BPXPR_RunCullPasses` = sub_c6670 (0xc6670): calls sub_c6810 (`XPRRenderInstancePreShaders`), then sub_c6bd0 (`XPRRunFragmentCullShaderShadowScenes`), then tail-jumps to sub_c6f70 (`XPRRunFragmentCullShaderMainScene`).
- sub_c6810 (0xc6810): builds a 2048-bit occupancy bitmap (32 qwords, bound `cmp 0x800`) from each object's XPR index at `[obj+0x148]`, picks a free slot with sub_35930, and calls sub_19b400 per object.
- sub_c6bd0 writes the shadow EOP at 0xc6f2f: `sub_925c0(rsi = [ctx+0xe8]+0x40)`.
- sub_c6f70 writes the main EOP at 0xc7264: `sub_925c0(rsi = [ctx+0xe8]+0x68)`. Its main dispatch goes sub_932a0 -> sub_92bd0 -> sub_86b9e0. **Inferred:** this is a compute dispatch despite the "FragmentCullShader" debug name. To confirm.

### 2.5 Draw recording and the consumer (observed)
- `RenderDepth` / `RenderGBufferBakedPerVertex` / `...PerProbe` driver = sub_c59d0 (0xc59d0). It emits GPU waits on `[ctx+0xe8]+0x18`, `+0x30` (0xc5dc2, 0xc5de9) and `+0xd0` (0xc60fa), then queues the RenderDepth job (sub_c1860, flags 0x302) through sub_c12a0.
- Queued jobs run from `sub_c12a0` (its immediate branch is `test cl,1`). Job order is the same frame and the same DCB as the cull.
- sub_11d7a0 (0x11d7a0) is the per-draw recorder. It calls sub_19af20 at 0x11d949. Its callers are sub_c1860, sub_c1ce0, sub_c1fc0, sub_c46c0, sub_cd330, sub_ce850.
- `sub_19af20` (0x19af20) selects the mask source. Two jump tables (0xfad464 for mode `[draw+0x44]`, 0xfad484 for mode `[draw+0x20]`) pick a pointer from the 0xa8-byte per-set entry at `[ctx+0x38] + 0xa8 * object_index`. It then calls sub_1a6d20 (call site 0x19b0d8) or sub_1a7290 (call site 0x19b0c4), based on `test r8, 0xff00000000` and config byte `[cfg+0x22]`.
- `sub_1a6d20` (0x1a6d20) is the CPU consumer. `count = u16 [ctx+0x64]`, `words = (count+63)>>6`. It iterates set bits (blsi/lzcnt), tests a u32 table `[ctx+0x20]`, and calls sub_86b3d0 twice (0x1a716b, 0x1a71b2) per visible object. sub_1a7290 is the same loop with an indirect-args variant (`[rbx+0xc398]`, `[rbx+0xc3a8]`).
- `sub_19b760` (0x19b760) chooses between context buffers `[ctx+0x2238]` and `[ctx+0x21e8]` by `[cfg+0x1c]`. sub_a0290 (0xa0290, called from sub_9aef0 which creates `kRTT_OccluderDepth`) reads `[ctx+0x1fb8]`, `[0x21e8]`, `[0x2238]` and loops 9 times creating views. **Inferred:** these three are occluder-depth targets (shadow, main, other), not the masks.

### 2.6 Buffers and sizes
- Mask (bitset): at most 2048 bits = 256 B per set (32 qwords). Entry count is `u16` at `[ctx+0x64]`.
- Dispatch-draw buffer: frame-heap allocations (sub_12820) of 0x14, 0x18, 0xa0 bytes, 16-byte aligned. The brief's RenderSingleFrameGPU heap (32 MiB) is consistent with this but I did not verify the heap size.
- Count: 9 cull dispatches per frame (port comment `buffer_cache.cpp:3740`). One heavy frame: 442 readbacks, ~200 ms waiting (same comment).
- Port measurements (CHANGELOG): XPR masks are ~80% of guest readbacks. With `r_EnableXPR=0` readbacks drop from ~900/s to ~200/s, but draws double and FPS falls to 8-17. 63% of readbacks are in 0x1a6d20 (via sub_1a6d20 from sub_19af20). 78-89% of those reads find the GPU already done.

### 2.7 Port side (observed)
- `liverpool.cpp:1332-1383`: `SOTC_CULL_EXPRESS` (express lane). It is incomplete, per CHANGELOG.
- `liverpool.cpp:1794-1835`: CS label deferral (`SOTC_RB_EXACT`). Comment at 1815 says CS_DONE (see 2.2).
- `buffer_cache.cpp:2420-2450`: `SOTC_RB_PREV` (serves the last applied bytes, 1-2 frames old). Risk documented as "an object that just became visible may be culled for a frame or two". No measured result recorded in CHANGELOG.
- `buffer_cache.cpp:2675-2680`: `SOTC_RB_MATERIALIZE`.
- `buffer_cache.cpp:3740`: `SOTC_RB_GROUP`.

## 3. Stutter mechanism

- **Observed:** a heavy frame produces ~440 small guest reads (9 sets, one fault per small allocation). Each read is a write-protect fault, then a draw-thread command, then a copy, then a wait (CHANGELOG, `SOTC_RB_GROUP` comment). The masks are read from `sub_1a6d20` during the RenderDepth job, so the job queue and the draw thread serialize behind them.
- **Observed:** most of that wait is the port's round trip, not GPU time (78-89% already finished).
- **Inferred:** the stall pattern is the same-frame dependence itself. The CPU needs the mask to decide which draws to record, and the port satisfies that by faulting on each read. Every object set costs one fault chain.
- **Unknown:** whether the game's own CPU ever waits for the label. If it does not, its frame N is consuming frame N-1 data (the mask was produced in the previous DCB), and the port's fault wait is only forcing exact bytes.

## 4. Hook / redesign options

1. **One-argument mask swap (lowest risk).** Hook guest 0x1a6d20 (entry). `rsi` is the bitset pointer; the function reads only those words plus the u16 count and a u32 table from `rdi` (not the mask). Substitute a copy of the mask produced by the previous frame (the port keeps the last applied bytes; `SOTC_RB_PREV` is the existing version). Expected effect: no fault or draw-thread wait on mask reads; one frame of visibility latency. Risk: objects that just became visible are missing for one frame. Verify: count `SOTC RB` faults and draw-thread time with the swap on; check pop-in in a camera-pan scene; compare the frame where 1a6d20 reads a label of 0 (see open question 1).
2. **Compact on the GPU, draw with count (medium effort, the PC-native option).** Add a compute pass after the mask cull that writes a compact list of visible draws and a GPU draw count. In the game, replace the per-object loop in 0x1a6d20 with sceGnmDrawIndexIndirectCountMulti (wrapper 0x86b470, already present). The CPU never reads a mask. Risk: the per-object state setters (0x86ad40, 0x86bed0, 0x86bf50, 0x86bfb0, 0x86c000) must become constant per set or move into a per-draw indirect buffer. Verify: draws/frame and CPU time on the draw thread; image diff against the current frame.
3. **Double-buffered masks in the engine (radical version of 1).** Give each of the 9 mask sets a two-frame ring in the context and read the previous slot in the consumer. Same behavior as 1 without port memory tricks, cleaner to reason about. Needs a change in `sub_19b760` (buffer selection by `[cfg+0x1c]`).
4. **CPU cull (not recommended).** The occlusion test needs this frame's occluder depth (inferred from `kRTT_OccluderDepth`). A CPU frustum-only fallback draws far more objects; the port measured that the same scale of draws (r_EnableXPR=0) drops to 8-17 FPS. Not a viable replacement.
5. **Port-side only (already present).** `SOTC_RB_PREV=1` (option 1 implemented in the buffer cache). Needs a measurement, none is recorded.

## 5. Proposed names

- 0x11dbe0 BPXPR_IndexCullObjects (string `IndexCullXPRObjects`)
- 0x11d7a0 BPXPR_RecordMaskedDraws
- 0x19af20 BPXPR_SelectMaskSource
- 0x1a6d20 BPXPR_ConsumeVisibilityMask (CPU reader, per-visible-object draw)
- 0x1a7290 BPXPR_ConsumeVisibilityMaskIndirect
- 0x19b400 BPXPR_BuildSetEntry
- 0x19b760 BPXPR_BuildSetDraws
- 0xca6d0 BPXPR_PreRender (XPRPreRender)
- 0xc6670 BPXPR_RunCullPasses
- 0xc6810 BPXPR_BuildPreShaderSlots (XPRRenderInstancePreShaders)
- 0xc6bd0 BPXPR_CullShadowScenes
- 0xc6f70 BPXPR_CullMainScene
- 0xc5110 BPXPR_IndexCullJob
- 0xc5130 BPXPR_QueueIndexCull
- 0xc59d0 BPRender_DepthAndGBufferPasses (inferred)
- 0x92b50 BPGnm_DispatchWithLabelWait (inferred)
- 0x925c0 BPGnm_WriteCsDoneLabel (EOP 0x28, data 1)
- 0x86eb60 BPGnm_EmitEventWriteEop
- 0x86edc0 BPGnm_EmitWaitRegMem
- 0x86b3d0 BPGnm_DrawIndexIndirect (wrapper)
- 0x86b9e0 BPGnm_DispatchDirect (wrapper)
- 0x9aef0 BPXPR_CreateOccluderTargets (inferred)
- 0x6efe0 RegisterXprCvars
- 0x5bd70 InitEXPROccluderDebugModeNames

## 6. Open questions

1. **Does the game wait on the CS label on the CPU?** Search found none. Settle it at runtime: log at guest 0x1a6d20 (entry) the mask address `rsi`, the label values `[ctx+0xe8]+0x40` and `+0x68`, and the frame counter `[ctx+0xe88]`. A label of 0 at read time means the game consumes stale data (one frame late), and option 1 changes nothing for correctness.
2. **Are `[ctx+0x1fb8]`, `[ctx+0x21e8]`, `[ctx+0x2238]` occluder depth or mask targets?** Settle by dumping the value and the resource type in `sub_a0290`.
3. **Is the main-scene cull (sub_c6f70) a compute dispatch or a fragment draw?** Trace the chain sub_932a0 -> sub_92bd0 -> sub_86b9e0 with a runtime breakpoint on 0x86b9e0 when `XPRRunFragmentCullShaderMainScene` runs.
4. **Default values** of the `doIndexBufferCulling` and `forceUseLastIndexBufferCullingResults` cvars (set by constructors not decoded). `forceUseLastIndexBufferCullingResults` suggests the game already reuses last frame's index-cull result. Decode the constructor stores of `[0x2ea8ee8+0x1c..]` at runtime.
5. **Fix the comment** `liverpool.cpp:1815`: the XPR label is EVENT 0x28 BOTTOM_OF_PIPE_TS, not CS_DONE.

## Out of topic (seen, not analysed)

- `sub_1018c0` (eboot 0x101a98, the 29% reader in CHANGELOG) is `ParticleSystemPool::ProcessStats` (0x10998e), not the XPR. It belongs to the particles topic.
