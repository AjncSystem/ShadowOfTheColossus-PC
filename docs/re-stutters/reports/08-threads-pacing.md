# 08 — Threads, job system, frame pacing (saved by the lead from the agent's reply)

Labels: [O] observed in code/logs, [I] inferred, [U] unknown.

## 1. Answer
- Near-lockstep handoff between "Shadow Game Update" (sub_534690) and "Game Render"
  (RenderThread::ThreadFunction sub_bd6d0) through PS4SyncEvent event flags (one per bpe Thread,
  created sub_3b5c0 -> sub_3b3d0) [O]. Render runs the frame as a RenderJob array
  (sub_35480 -> sub_cef90 -> sub_cefa0 -> sub_118470) and waits for it with sub_35730 at 0xbd922
  [O]. Update cannot run more than ~one frame ahead [I].
- Job system: 4 pthread workers "BPE JobWorkerThread CPU1..4" spawned by sub_335d0 (one per bit
  of mask params+0x30, pinned via scePthreadAttrSetaffinity); fibers ("BPE Job Fiber %d"); a
  waiting job switches fiber (sub_37500); a non-job waiter spins then usleeps (sub_35590, 10000
  spins); idle workers spin with pause then sleep on their PS4SyncEvent (sub_38b40 -> sub_3b4a0
  at 0x38bb7) [O].
- Timing: raw rdtsc (sub_324e0, sub_32430, sub_323c0), frequency from sceKernelGetTscFrequency
  (sub_323d0) [O]; frame delta is real time [I]; FixedTimeStep / MaxUpdatesPerFrame cvars
  registered in sub_2534c0 [O], readers not found [U].
- Flip cap: sceVideoOutSetFlipRate (sub_95a90 at 0x95b81, 0x95e3f) [O]; port applies the cap in
  PresentThread (sotc_fps_cap.h) [O].
- **Key port finding:** at default settings guest submit paths block until the port's GPU is
  idle when the GPU is busy: sceGnmSubmitAndFlipCommandBuffersForWorkload -> GateWait(0) ->
  WaitGpuIdle (gnmdriver.cpp:2417, 156-173, 92-96); DingDong -> GateWait(1) (line 407);
  sceGnmSubmitDone -> GateWait(2) + submission_lock (2475-2490); SOTC_CP_AHEAD unset = mode 0
  (liverpool.cpp:4384-4389) [O]. So the port's serial CP+draw chain lands on the guest render
  thread when the GPU is behind [I]. Whether the eboot calls these imports directly is [U]: 0
  direct call sites for sceGnmSubmit*/SubmitDone/RequestFlipAndSubmitDone (indirect calls not
  indexed).
- Critical path per frame [I]: update work to the kick (0x534960) -> RenderJob (PM4 build in
  sub_118470; sceGnmInsertWaitFlipDone via sub_91f50 / sub_86f700 is a GPU-side wait) -> counter
  wait 0xbd922 -> event back to update -> update's second wait 0x53496d.

## 2. Evidence
- sub_534690: creates "Shadow Game Update" at 0x534745 once ([r12+0x3ca0]); waits 0x534800 on
  ([r13]+0x10), clears 0x53480d, sets [r13] at 0x534960 (sub_3b450), waits again 0x53496d;
  samples time (sub_324e0) at 0x5346c4, 0x534951, 0x534975 [O].
- sub_bd6d0 (string 0xfa1672) created in sub_bd170 at 0xbd2d2 "Game Render"; waits 0xbd717,
  0xbd93d; polls 0xbd728, 0xbd947; AllocateCounter 0xbd801 (sub_34b10); kick 0xbd8fe (sub_35480,
  entry sub_cef90 "RenderJob"); wait+free 0xbd922 (sub_35730) [O].
- sub_bcf90/bd010/bd0a0/bd110/bdf20: TSC, wait (sub_3b4a0), TSC — timed waits for stats [I].
- sub_118470 ("R::Hud", "VBlank", "R::AfterHud") calls sub_91f50 (GPU wait-flip-done) [O].
- sub_95760 (RenderFlip setup): SubmitFlip, WaitVblank, busy-poll IsFlipPending 0x95880-0x95888,
  creates "RenderFlip" at 0x95907 [O]; per-frame flip not visible as an eboot call [U].
- sub_1464d0 ResourceFactory::ThreadFunc waits 0x1465a7, 0x1466c2 [O].
- JobSystem: Initialize sub_335d0 (JobSystem.cpp), EThreadAffinity table sub_34230 (CPU0..7,
  Cluster0 0x0f, Cluster1 0x30, AllReal 0x3f, All 0xff); mask 0x1e -> CPU1..4 [I]; worker start
  sub_36140; DoJobScheduling sub_38700, ExecuteJob sub_39800, MarkEndOfJob sub_3a380,
  SwitchFiberDueToStackSize sub_39610, SuspendActiveFiberAndSwitchToFree sub_37500,
  CanWorkerThreadGoToSleep sub_39cc0, CheckForDeadlock sub_39de0; counters sub_34b10/34bd0,
  NotifyCounterUpdate sub_33400/334b0, BeginJobArray sub_34d90/35480, WaitForCounter sub_35590
  [O].
- Sync wrappers: mutex sub_3b390/3b3c0; PS4SyncEvent create sub_3b3d0, set 3b450, clear 3b460,
  poll 3b470, infinite wait 3b4a0 (mode 0x11 or 1), timed 3b4e0; thread create 3b5c0/3b6a0;
  join 3b8c0; usleep 3b570 [O].
- Port: src/runtime_sotc.c:303-440 event flags (mutex + cond per waiter; 5 s wake logs;
  SOTC_EVF_TRACE=<name|*> logs set/wait with host return addresses). runtime_thread.c:4,147
  guest priorities/affinity recorded not enforced, default 0x7f. bbport_threads.h:23-150
  SOTC_CPUSETS pins only port threads. compat_win.c:115-127 high-res sleeps; probe.c:1358
  timeBeginPeriod(1). liverpool.cpp:642 full back-op ring blocks the CP on the draw thread [O].
- Logs (user/last_run.1.log): the 5/10/15 s 'PS4SyncEvent' / 'sceSndxLoaderNotify' waits come
  in batches with sound-loader flags — idle threads, not stalls [I]. SOTCSTALL frame 157 201 ms:
  cpu draw 120.4, cp 125.0, rec 41.2, upload 208.1 (line 295).

## 3. Stutter mechanism
- Stall time is measured on the port's CP/draw threads [O]. When the GPU is behind at a submit,
  the guest render thread blocks in WaitGpuIdle; the render job finishes late; update (waiting at
  0x53496d) follows: a 200 ms GPU backlog becomes a 200 ms game frame [I]. Real-time dt: the
  simulation takes the hitch in one step [I].

## 4. Hook / redesign options
- A. Remove the submit gate by default (SOTC_CP_AHEAD=1/2, SOTC_CP_AHEAD_FRAMES 0-3 exist,
  gnmdriver.cpp:98-154). Risk: copy-reuse hazards. Verify with SOTC_FPS gate counters.
- B. Pipeline update one frame ahead of render (move the 0x53496d wait, double-buffer per-frame
  state). High risk; useless while the submit gate blocks. Verify overlap with
  SOTC_EVF_TRACE=PS4SyncEvent.
- C. More job workers / pin guest workers off the port's CP/draw cores (guest threads are
  unpinned today). Low critical-path benefit.
- D. Spin-then-wait handoffs: low benefit.
- E. Clamp/fix dt (find FixedTimeStep/MaxUpdatesPerFrame readers): stops a stall from becoming a
  simulation jump.

## 5. Proposed names
0x335d0 JobSystem_Initialize; 0x34230 EThreadAffinity_Name; 0x36140 WorkerThread_StartFunc;
0x37500 JobSystemScheduler_SuspendFiberSwitchFree; 0x38700 JobSystemScheduler_DoJobScheduling;
0x38b40 JobSystemScheduler_WorkerIdleWait; 0x39800 JobSystemScheduler_ExecuteJob;
0x39cc0 JobSystemScheduler_CanWorkerSleep; 0x39de0 JobSystemScheduler_CheckForDeadlock;
0x35590 JobSystem_WaitForCounter; 0x35730 JobSystem_WaitAndFreeCounter;
0x34b10 JobSystem_AllocateCounter; 0x34bd0 JobSystem_FreeCounter;
0x33400 JobSystem_NotifyCounterUpdate; 0x34d90 JobSystem_BeginJobArray;
0x35480 JobSystem_KickJobArray; 0x3b3d0 EventFlag_CreatePS4SyncEvent; 0x3b450 EventFlag_Set;
0x3b460 EventFlag_Clear; 0x3b470 EventFlag_Poll; 0x3b4a0 EventFlag_WaitInfinite;
0x3b4e0 EventFlag_WaitTimed; 0x3b5c0 Thread_Create; 0x3b6a0 Thread_PthreadCreateAffinity;
0x3b570 Thread_Usleep; 0x323c0 Rdtsc_Read; 0x324e0 Time_SecondsSinceBase;
0x534690 ShadowGameUpdate_Frame; 0xbd170 RenderThread_Init; 0xbd6d0 RenderThread_ThreadFunction;
0xbcde0 RenderSync_KickFrame; 0xc12a0 RenderThread_RunFunctionAsRenderJob;
0xc5560 RenderThread_KickoffSubmitJob; 0xcef90 RenderJob_Entry; 0x118470 Renderer_FrameTail;
0x86f700 Gnm_InsertWaitFlipDone_Frame; 0x95760 VideoOut_ReconfigureBuffers;
0x956e0 VideoOut_FlipShutdown; 0x95a90 VideoOut_OpenSetFlipRate;
0x1464d0 ResourceFactory_ThreadFunc; 0x2534c0 Cvars_RegisterDebugCamera_FixedStep

## 6. Open questions
1. Does the eboot call a submit import at runtime (indirect calls)? Decides whether the gate is
   on the critical path; the port counts gate waits under SOTC_FPS.
2. Which threads sit in the 5-15 s PS4SyncEvent waits (SOTC_EVF_TRACE=PS4SyncEvent).
3. Exact worker mask (callers sub_255130 / sub_255180 / sub_255610).
4. Readers of FixedTimeStep / MaxUpdatesPerFrame (xrefs of 0xfc3885, 0xfc3893).
5. Is sub_534690 the per-frame function or a thread body (vtable at 0x15c8d78 + 0x10).
6. Purpose of the timed waits in sub_bcf90/bd010/bd0a0/bd110/bdf20.
