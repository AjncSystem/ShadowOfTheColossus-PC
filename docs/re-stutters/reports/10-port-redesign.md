# 10 - Port architecture: serialization points and redesign options

Read-only study of the port (`gpu/shadps4/`, `src/`) and its design docs. Labels: **observed** (code, log or doc number), **inferred**, **unknown**. File:line refs are to the current tree. Sibling drafts used for context: `02-gpu-cpu-sync.md`, `03-cpu-gpu-writers.md`, `04-memory-model.md` (in this folder; their numbers are not yet final).

## 1. Answer

- **The 18 ms draw-thread cost is mostly cache and bind work, not page protection or readbacks.** Binds are about 9 ms (BIND-PATH-DESIGN §1). Protection is about 4,200 protects/s (topic 03 §2.3), about 0.3-0.7 ms per frame on the draw thread (inferred from the 4% GpuDraw profile share in PLANO §2). Only option (a) touches the 18 ms directly, and only its emission part (about 3-5 ms, estimate).
- **The recorder is a co-bottleneck.** In 12 of 14 "SOTC cpu/frame" windows of `user/last_run.log` the on-CPU time is draw 20-30 ms and rec 16-23 ms (`driver.cpp:1944`; the other two are a light window and one at draw 17 ms). That time includes spin-waits (`vk_recorder.cpp:1189-1199`), so the real replay work is **unknown**. Any parallel draw design must also split replay.
- **The big frame killers are guest-side waits, not draw-thread CPU.** Readbacks put the game in 45-70 ms waits per frame in bursts (PLANO §1.2 table). Write faults take the cache locks the draw thread holds, and `scheduler.Finish()` drains the GPU on the draw thread. Options (b), (c) and (d) target these waits, not the 18 ms.
- **Labels are answered early.** EOP labels are written on the draw thread when the op runs (`liverpool.cpp:3456-3470`), not when the GPU finishes. Correctness for GPU results therefore rests on readback faults. Honest labels cost one GPU round trip each, and the game waits on about 130 labels per frame (`liverpool.cpp:163-168`, measured 27/10). Option (c) cannot simply add honest fences.
- **Frame pipelining (d) cannot hide game waits on readbacks.** It helps only draw-bound bursts.
- **Recommended order:** measure (S0), then range-scoped CP waits and the XPR express lane (S1-S2), then the draw-state split (S4, a prerequisite for (a)), then (a). Declared writes (b) come after the class-based tracking of topic 04, and (c) and (d) only after the consumer of the XPR bitset is analysed.
- **Corrections to `docs/ARCHITECTURE-TARGET.md`:** the "2 runtime slots" blocker is already 6 slots with a thread-local `g_info_slot` (`info.h:34`, `:118-122`; `liverpool.cpp:1066`). Its gnmdriver line numbers are stale (the gate is at `gnmdriver.cpp:407, 2417, 2462, 2481`). Its "largest CPU win" claim for parallel recording conflicts with BIND-PATH §7 (1.1-1.3x). This report sides with BIND-PATH for the current op mix.

## 2. Evidence

### 2.1 Threads, queues and what each one owns (observed)

| Stage | Thread | Entry | Notes |
|---|---|---|---|
| Parse PM4 | `GpuCommandProcessor` | `Liverpool::Process` (`liverpool.cpp:2440`) | Looks up pipelines on this thread (`SubmitDraw`, `liverpool.cpp:1174`) |
| Op ring | 512 `BackOp` slots | `BackRingSize` (`liverpool.h:556`), `BackAlloc` (`liverpool.cpp:630`) | CP blocks when the ring is full |
| Execute ops | `GpuDraw` | `BackLoop` (`liverpool.cpp:410`), `ExecuteBackOp` (`:966`) | One thread runs every op in order |
| Guest commands | same thread | `ProcessCommands` (`:2391-2413`) | Readbacks and `SendCommand` run between ops, not in order with the queue |
| Vulkan replay | `GpuRecorder` | `CommandRecorder` (`vk_recorder.h:60`) | One producer, one consumer. Only `SOTC_PASS_SEC` splits replay, per pass |
| Present | on `GpuDraw` | `Presenter::GetRenderFrame` (`vk_presenter.cpp:1817`) | Waits for a free frame and its present fence |

- One graphics queue carries draws, copies and sparse binds. The transfer queue is opt-in (`vk_instance.cpp:697-704`).
- Guest submit gate: `WaitGpuIdle` (`gnmdriver.cpp:92`, `:156-159`). Default `SOTC_CP_AHEAD=0` (`liverpool.cpp:4385`).
- `FlushPeriodic` submits every 256 commands on NVIDIA and AMD (`vk_rasterizer.cpp:4639-4660`). A submit cuts a render pass.

### 2.2 Serialization points (observed unless marked)

- **S1. CP waits for the draw queue on CPU memory writes.** `SubmitDraw`, `SubmitDrawIndirect` and `SubmitDispatch` call `WaitBackWrites()` (`liverpool.cpp:1175`, `:1201`, `:1307`). A WRITE_DATA or DMA op publishes with `writes_memory=true` by default (`liverpool.h:559`; WRITE_DATA at `liverpool.cpp:3673`). The CP then waits for the whole queue ahead of that op (`WaitBackTail`, `:611`). GPU writes count only with `SOTC_CP_STRICT=1` (default off, `liverpool.cpp:116-117`). Count and duration of these waits: **unknown** (not in the logs I read).
- **S2. Guest readback = 4 hops.** `ReadMemory` → `SendCommand<true>` (`buffer_cache.cpp:874`, `liverpool.h:188`) → `BeginAsyncReadback` on the draw thread → `WaitSubmitted` on the recorder (`vk_scheduler.cpp:129`) → GPU timeline → `FinishAsyncReadback` and `ApplyDownloads` on the draw thread (`buffer_cache.cpp:4279`, `:4748`). Each hop waits for work already queued.
- **S3. Labels are early.** The draw thread writes EOP labels at op execution (`liverpool.cpp:3456-3470`). `SOTC_EOP_TICK` (default off, `liverpool.cpp:164`) makes them honest. Its comment reports one GPU round trip per label (about 130 per frame).
- **S4. Full GPU drains on the draw thread.** `scheduler.Finish()` at arena allocation without tick-free (`buffer_cache.cpp:6894-6897`), unmap (`:7180`), sync eviction (`:7427`), sync texture download (`texture_cache.cpp:178`), and `FinishHostRead` (`buffer_cache.cpp:4517`). Each one flushes and waits for the GPU.
- **S5. Guest write faults contend with the draw thread.** `InvalidateMemoryOnGuestFault` (`vk_rasterizer.cpp:6991`) takes the buffer and texture cache locks. The draw thread holds the texture `BatchLock` during binds (`:5840`, `:6567` and others). If the page is GPU-modified, the fault first calls `ReadMemory` (`memory_tracker.h:245-267`), so a write to GPU-written memory is a readback round trip inside the fault handler (**observed**, path in code).
- **S6. Single draw state.** The `Rasterizer` holds per-draw members that one thread owns: `cb_descs`, `db_desc`, `image_infos`, `buffer_infos`, `bound_buffers`, `set_writes`, `push_data`, `bindless_slots`, `needs_barrier`, `fast_bind`, `hudless_*`, `ui_shadow` (`vk_rasterizer.h:229-460`). Also `back_regs`, `back_cs`, `back_cb_extent` (`liverpool.h:693-696`), and the `Scheduler`'s single command buffer and `is_rendering` (`vk_scheduler.cpp:149-227`). A pass ends on every render-state change (`:149-160`).
- **S7. Single recorder.** `vk_recorder.h:60` documents one producer and one consumer. Replay is per command, and the queue submit runs on the recorder (`vk_scheduler.cpp:1047-1130`).
- **S8. Page tracking.** Tracking is per 4 KiB page with 16 MiB region bitmaps (`region_definitions.h:13-22`). A write fault unprotects the page and marks it CPU-modified. Uploads re-protect it (`SynchronizeMemoryFull`, `buffer_cache.cpp:6392`). The window has 11,322 write faults (`user/last_run.log`, "SOTC FAULTS"); topic 03 gives 9.8k-16.6k per 5 s. Protects: about 4,173/s, with about 53 MB/s uploaded (topic 03 §2.3).

### 2.3 Cost numbers (observed unless marked)

- Heavy windows (PLANO §1.2, 29/10): draw CPU 20-24 ms, binds 8-13 ms, flip wait 3-10 ms, game readback waits 45-55 ms per frame.
- Area-entry bursts (PLANO §1.2, 29/10): draw 33-36 ms, binds 13-22 ms, game readback waits 60-70 ms per frame.
- GpuDraw profile (PLANO §2): protect 4%, readback guard 4-6%, CopySparse 3.5%, dispatch 15%, indirect 8%, flip wait 28%.
- Guest threads: 24% of BPE worker time is inside read faults that call `ReadMemory` (PLANO §1.2 table).
- Read faults in the sample: `sub_1a6d20` (XPR visibility cull) 7,425 of 20,286 (topic 03 §2.2).

## 3. Stutter mechanism

Ordered by measured weight:

1. **Readback chain (observed in bursts).** A game thread reading GPU-written memory waits on four hops (S2). Its latency is the draw thread finishing its current op, the flush, the recorder backlog, the GPU queue depth (all submitted work) and the apply. The 60-70 ms per frame in bursts matches this chain. **Inferred:** the recorder backlog and the GPU queue depth are the largest hops, because the draw thread is not idle in bursts.
2. **Draw-thread bursts at area entry (observed).** Draw CPU reaches 33-36 ms for about 21 frames (about 1 s). Binds dominate, and texture uploads add 50-60 ms spikes on the draw thread (PLANO §1.3, §2).
3. **Full drains (observed in code, frequency unknown).** Each `Finish()` (S4) stops the draw thread until the GPU is idle, and every op queued behind it waits.
4. **Fault-path contention (observed in design, cost unknown).** A guest write waits for the draw thread's texture batch (S5). The "tc" field of the SOTCSTALL lines measures this: its wait time was 0.0 ms in the sampled lines, though its count was not zero (for example "tc 0.0 (65)").
5. **CP waits on WRITE_DATA (inferred).** Each un-fused CPU write before a draw makes the CP wait for the whole queue ahead of it (S1). Size unknown until measured.

Observed vs inferred is marked per item. The 150-500 ms frames come from items 1 and 2 together with single-thread work (shaders, texture uploads), per PLANO §1.3.

## 4. Hook and redesign options

| Option | Files | Draw thread (est.) | Guest stalls | Breaks if wrong | Cost / risk |
|---|---|---|---|---|---|
| (a) N draw workers, sequencer owns cache mutation | `liverpool.cpp`, `liverpool.h`, `vk_rasterizer.cpp/.h`, `vk_scheduler.cpp/.h`, `vk_recorder.cpp/.h`, `texture_cache/*` | 3-5 ms of 18 ms (emission only); the recorder becomes the next limit | little | image order, blend and depth order, DLSS and HUD sequencing, render passes cut by submits | 3-5 weeks, high |
| (b) declared writes replace page protection | `page_manager.cpp` (`Protect` 1003, handler 1339-1410), `memory_tracker.h`, `buffer_cache.cpp` (`InvalidateMemory` 507, `SynchronizeMemoryFull` 6392), `runtime_memory.c` (`gpu_protect` 1746), allocator hook | 0.3-0.7 ms (protects) | fewer fault-path locks; fewer WAW readbacks if declared | silent staleness (green blocks, textures with garbage) if a writer is undeclared | depends on topics 03/04, high |
| (c) discrete model: upload via host import, eager readback ring | `renderer_vulkan/sotc_guest_memory.*`, `buffer_cache/host_import.*`, `buffer_cache.cpp` (Begin/Finish/ApplyDownloads), `vk_scheduler.cpp`, `liverpool.cpp:3456` | 1-2 ms (CopySparse 3.5%, apply and guard 4-6%) | largest potential (readback waits), only if consumers tolerate latency | same-frame consumers read stale data; label semantics change; the heap guard must stay | high |
| (d) CP one or two frames ahead | `gnmdriver.cpp` (`WaitSubmitGate` 156, `GateWait` 174), `liverpool.cpp` (frame marks 4402-4470), `vk_presenter.cpp` | about 0 | none for readback waits | unfenced guest double buffers (comment `gnmdriver.cpp:98-120`) | switch exists (`SOTC_CP_AHEAD=2`), no measured result in CHANGELOG, low |

### (a) Parallel translation of one frame

- **Shared state blocking it:** S6, plus `Shader::g_info_slot` (already thread-local, but `back_cs` and `back_regs` are single), `ReuseShadowCascade` (`vk_rasterizer.cpp:963`) and `SkipStaggeredShadow` (called from `Draw`, `:1838`), both cross-draw, the DLSS `NoteDraw` and the hudless composite (per-draw state), and the BufferCache, which is draw-thread only apart from a few mutexes (`buffer_cache.h:521-719`). The `TextureCache` uses a `ReentrantMutex` with a batch lock (`texture_cache.h:434`).
- **What can move:** pipeline lookup and prep (already on `GpuPrep` with `SOTC_PARALLEL_RECORD`, `liverpool.cpp:1059-1170`), dynamic state, push data, descriptor writes, and the emission itself. **What cannot:** `BindResources` (S6), because it creates images, uploads, syncs and transitions. BIND-PATH §1 puts binds at about 9 ms of the 18 ms, so workers cut only the remainder. That is why the ARCHITECTURE-TARGET "largest win" estimate is high.
- **Replay has to split too.** Each worker needs its own producer stream and its own replay (the `PassSecondary` pattern, `vk_recorder.h`, `SOTC_PASS_SEC`). Without that, the 16-23 ms recorder limits the gain (observed on-CPU, real work unknown).
- **Verification:** the serial path is the reference: `SOTC_CP_THREADS=0 SOTC_CP_STRICT=1` must give identical images (ARCHITECTURE-TARGET §5).

### (b) Replace page protection with declared writes

- **Hook surface** (topic 04 draft): allocator families `CPUMemory` (kind 1) and `GPUMemory` (kind 2), with the constructor at `sub_475c0` and the block allocator at `sub_479a0`. Topic 03 finds that **47%** of write faults happen in host `memcpy`/`memset`/`memmove` called by the game (inferred; `src/runtime.c:170-176`). That is one declarable call class. Particle records (8.7%) and view constants (about 2%) are named writers.
- **Why an allocator hook alone is not enough.** The PS4 type is not a safe classifier: CPU heaps sit in Garlic (topic 04 §1), and heap metadata writes (18% of faults, topic 03) share pages with GPU data (`bpe_heap_guard.h`). A kind-aware pool (topic 04 option B) or class-based tracking (option C) is needed first. Those are cheaper than declared writes and remove most of the tracked set without declarations.
- **What must hold:** each declaration precedes the write, or the draw thread checks a generation before its next bind. Undeclared writers keep protection (hybrid). Verification mode: keep protection on declared ranges as a shadow, count faults that no declaration covers, and log them over a full play-through. Red-zone functions (`0xEDFB30`, `0xF52530`, `0xF67D2B`) are not in the sampled faults (topic 03), but that is not proof.
- **What breaks:** silent staleness. Bloodborne's port hit exactly this (MODELO-MEMORIA-PC §"Lições").
- **Gain:** about 0.3-0.7 ms per frame on the draw thread (protects), and fewer fault-path locks (S5).

### (c) Discrete-GPU model

- **Already present:** device-local arena, a tick-based staging ring (`vk_staging_buffer_pool.h`), host import of guest memory in 64 MiB chunks (`host_import.h`), and readbacks through staging. What is new: GPU-written results copied at fence time into a host-cached ring, not on a fault (topic 04 option E).
- **Blocker A, same-frame data.** An eager copy at frame end cannot deliver same-frame data. The XPR cull is cited as same-frame (topic 04 §3; brief), but topic 02 found no label spin and its open question 2 (`0x53496d`) is undecided. The consumer must be analysed (`eboot+0x1a6d60` per MODELO-MEMORIA-PC).
- **Blocker B, labels.** Honest labels are one GPU round trip each (S3). Cheap completion needs timeline mapping (`eop_shadow`, `SOTC_EOP_TICK`), and the game still waits for the GPU when it needs the data.
- **Removes from the draw thread:** CopySparse memcpy (3.5%) and readback apply with guard (4-6%), about 1-2 ms per frame. The guard must stay (topic 04 and PLANO §5). Its guest-side gain is the largest of the four options, if consumers tolerate latency.
- **Memory:** host import pins 64 MiB chunks. VRAM is 6.4-7.1 GB of 8 GB (MODELO-MEMORIA-PC).

### (d) Frame pipelining

- **Math (inferred):** the budget is 33.3 ms. Heavy normal frames take 20-24 ms, so the slack is 9-13 ms. In a burst, draw takes 29-36 ms and the backlog grows by about 1-3 ms per frame, so 1-2 frames of queue can absorb a draw-bound burst.
- **But bursts are readback-bound.** Frames of 40-50 ms in bursts carry 60-70 ms of game readback waits (PLANO table). Pipelining does not shorten those waits while the game waits on the GPU in the same frame.
- **Risk:** the guest's unfenced double buffers (`gnmdriver.cpp:98-120`). That comment already names the risk.

## 5. Staged migration order

Each step is behind a `SOTC_*` switch, default off, and accepted on the image-identity check against `SOTC_CP_THREADS=0 SOTC_CP_STRICT=1`.

| # | Step | Switch | Files | Risk | Gate |
|---|---|---|---|---|---|
| S0 | Measure: readback chain per hop; WAW vs RAW readback counts; recorder real work with spin excluded; WaitBackWrites count and duration by cause; draw Amdahl split (cache-mutating vs emission) | `SOTC_FPS`, new counters | `sotc_stats.h/.cpp`, `liverpool.cpp`, `vk_rasterizer.cpp`, `vk_recorder.cpp` | none | decides the order below |
| S1 | Range-scoped `WaitBackWrites`: CP waits only for queued writes that overlap the SRT and sharp ranges it reads | new | `liverpool.h:606`, `liverpool.cpp:1175/1201/1307`, `:2422` | medium | identical images; S0 count drops |
| S2 | Finish the XPR express lane (exists, incomplete per CHANGELOG) | `SOTC_CULL_EXPRESS` | `liverpool.cpp` (`XpEvaluate` 1500-1840) | medium | readback chain shorter (S0) |
| S3 | Remove full drains in arena, unmap and eviction (existing tick-free and unbind switches) | `SOTC_ARENA_TICK_FREE`, `SOTC_UNBIND_GPU_WAIT` | `buffer_cache.cpp` | medium | `Finish` count near zero |
| S4 | Draw-state split: per-worker `back_regs`, `back_cs`, `back_cb_extent`; `RenderState` formats; measure `SOTC_PASS_SEC` first (`passsec 0.00` in the sampled windows) | `SOTC_PASS_SEC` | `vk_scheduler.*`, `liverpool.*`, `vk_rasterizer.*` | high | `passsec` shows real work |
| S5 | (a): two draw workers recording pass secondaries; sequencer keeps `BindResources`; per-worker recorders | `SOTC_DRAW_WORKERS=2` | as S4 + `texture_cache/*` | high | emission ms down; identical images |
| S6 | (b) step 1: kind-aware pool or class-based tracking (topic 04 A-C), then declared writes for the memcpy class and particle writers, in shadow mode | `SOTC_CLASS_TRACK`, `SOTC_WRITE_DECL` | `page_manager.cpp`, `memory_tracker.h`, `runtime.c`, allocator hook | high | shadow mode: zero undeclared faults over a full play-through |
| S7 | (c): eager readback of hot GPU-written ranges at frame end, only for consumers proven not same-frame (eboot analysis); honest labels only if S0 shows labels bound frames | `SOTC_RB_EAGER` | `buffer_cache.cpp`, `vk_scheduler.cpp`, `liverpool.cpp:3456` | high | readback waits down; no stale reads |
| S8 | (d): `SOTC_CP_AHEAD=2` after S7, measured on burst routes | `SOTC_CP_AHEAD=2` | `gnmdriver.cpp`, `liverpool.cpp` | low to medium | bursts shorter; no double-buffer corruption |

Relation to ARCHITECTURE-TARGET steps: its steps 1-5 stay. S0-S2 go before its step 6, and its step 6 is S4 here. Its step 10 (coarse tracking) is subsumed by S6.

## 6. Open questions

1. **Real recorder work.** Measure replay time with spin excluded (`SOTC_REC_SPIN_US`). If it is small, option (a) is limited by the draw thread alone.
2. **Readback chain per hop** (S0). Which hop dominates in bursts: queue wait, recorder backlog, GPU, or apply?
3. **WAW vs RAW readbacks.** How many of the 700-900/s readbacks are caused by protection (write to GPU-written page) rather than by game reads? Topic 03 shares the write side; topic 02 the read side.
4. **Is the XPR cull same-frame?** Topic 02 (open question 2, `0x53496d`) and topic 04 disagree on the reading. Settle it with a timestamped trace of the CS_DONE label and the read of `sub_1a6d20`. If it is previous-frame, option (c) becomes cheap.
5. **CP ordering (S1).** Does any CP-side read depend on a CPU write that the draw thread has not yet run? That decides whether range-scoped waits are safe.
6. **Declared-write surface (b).** Is `memcpy` at `src/runtime.c:170-176` the only bulk writer class? Topic 03 says 47% of faults; confirm that all calls go through that one function.
7. **Frame queue depth (d).** The double-buffer risk in `gnmdriver.cpp:98-120` needs a test on the two frame-allocator families (`GameSingleFrame`, `GameDoubleFrame`, topic 04).

**Proposed names:** none. This topic found no eboot function to name. The XPR consumer is the read site `sub_1a6d20` (topic 03), and `eboot+0x1a6d60` is the cull reader cited in MODELO-MEMORIA-PC.

**Outside this topic, noted only:** topic 04's BPE heap guard inside Garlic is the same guard that makes option (c)'s apply path mandatory (`bpe_heap_guard.h`). The XPR cull's same-frame reading is a sync question for topic 02.
