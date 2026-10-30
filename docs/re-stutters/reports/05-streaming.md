# 05 - Streaming and area changes in the engine

## 1. Answer

- The resource system is `ResourceFactory` (vtable 0x1598518, C++ names from the eboot's own debug strings). Its loader is the guest thread "Resource Loading" (prio 700, 1 MiB stack), created in the constructor sub_144ca0. Thread body: `ResourceFactory::ThreadFunc` (0x1464d0) calls `ProcessLoadingThreadLoop` (0x148600) in a loop. **observed**
- Requests go into 10 priority queues (ELoadingPriority 0..9 = StreamingTexture, SyncLoad, Highest ... Background; "Total"=10). The loader pops them, starts Fios reads into 1024 job slots of 256 bytes (cap 10 per pass), waits, decodes, creates the object and posts a completion. **observed (code), queue order inferred from string order**
- **Main thread barrier (most important finding):** `GameCoreLoop::GameLoop` (0x25c080, call at 0x25cb12, and via sub_3c5a00 at 0x25cba9) calls `WaitForAllCmdsToFinish` (0x1469d0). It loops `while HasPendingWork()` (0x147f40 = any of the 10 queues non-empty): each iteration it signals the loader, blocks in `sceKernelWaitEventFlag` on the loader's completion event, and drains completions with `SyncPointProcessPendingResources` (0x1480f0). Streaming-texture and background loads are included, so the frame end waits for the whole queue. **observed in code; the per-frame cost is inferred until measured.**
- Area change load: `CGame::LoadInitialWorld` (0x3c5170) prints the queue size (`GetPendingCount`, 0x147f00, the same 10 counters). Logs show about 16,500 pending requests at that point. **observed**
- Reads go through Fios2. Fios2 is the dump's own PRX linked natively (not port code; `docs/sotc/REVISAO-DO-PROJETO.md:22`). Its host reads land in the port's `do_read/do_pread` (`src/runtime_file.c:380-415`). Decompression of resource payloads is `bpe::Compression::Decompress` (0x1aa2a0) with Oodle (`OodleLZ_Decompress` via sub_89d3b0). zlib appears only inside Fios2 (thread `SceFiosZlib0`). **observed/inferred**
- The port's "[GAME] Loading a save or a new area (unused textures dropped)" line is emitted by `note_game_load` (`src/runtime_file.c:418-431`) when the guest writes "st_gameflow() starting game" to stdout; it then calls `bbgpu_request_texture_flush`. **observed**

## 2. Evidence

**Loader thread**
- 0x144ca0 ResourceSystem ctor: `ResourceFactory` at vtable 0x1598518; creates thread "Resource Loading" (stack 0x100000, prio 0x2bc=700) with functor fn 0x1464d0; log: `guest thread 'Resource Loading' created (stack=1048576, prio=700)`.
- 0x1464d0 ThreadFunc: `sub_3b4a0` (sceKernelWaitEventFlag) on [obj+0x98] = idle wait; 0x146716 calls the loop functor (`ProcessLoadingThreadLoop`, fn 0x148600).
- 0x148600 loop: the batch cap is 10 (`cmp ..., 0xa` at 0x1488d0); job pool 0x400 slots (`[obj+0xb4]=0x400`, slot = `shl 8`, sub_149df0). Callees on the job path: 0x146ea0 PrepareNextResourceToLoad, 0x14b800 StartLoadJob (Fios read), 0x14ae70 IsJobDone (`[obj+0xd0]==0`), 0x14cf00 ready-to-create (state 0xb and data present), 0x149df0 / 0x1499c0 release/wait a job slot (`sceFiosOpWait`, sub_312a0), 0x149c80, 0x14d5d0 GetState. The "ResourceFactory::CreateResource" scope (0x148c96) calls the build callback `[task+0x18]` indirectly (0x148d14), so async creation runs on the loader thread (inferred from the scope and indirect call).
- Fios wrapper: sub_31830 `sceFiosFHPread`, sub_317f0 `sceFiosFHRead`, sub_31700 `sceFiosFHOpen`, sub_312a0 `sceFiosOpWait`+finish, sub_31900 `sceFiosOpIsDone`, sub_31c90 `sceFiosArchiveMountSync`; mount list of `$/pak0..N.psarc` and `cachedFiles.psarc` in sub_b10f60 (strings 0x12d0051..0x12d009b). Wrapper objects are vtable-dispatched (vtables 0x1595258, 0x1595408, 0x1595470); the exact call slot from StartLoadJob is unverified.
- Decompression: `CreateResource` (0x147160) calls `bpe::Compression::Decompress` (0x1aa2a0). Method = top 3 bits of the first dword (jump table 0xfadd4c): 0 = memcpy (0x1aa3cb); 1 = sub_1b9130 (codec unknown; maybe zlib); 2,3,4,5 = Oodle (sub_89d3b0 -> OodleLZ_Decompress; asserts for threadPhase at 0x89d5cf).

**Requests**
- `BuildAsync` (vtable slot 3, 0x147490): locks [obj+0x58], moves the item to queue `prio` (`lea rax,[rax*5]` = 40-byte blocks at obj+0xf0, count at +0x110+k*0x28), signals the loader event (0x3b450). Priority index is the enum value.
- `BuildSync` (slot 2, 0x146b60): does PrepareNext, CreateResource (decode inline), Fios wait, all on the calling thread. Callers unknown (open question).
- `DeleteResource` (slot 5, 0x1478d0) and the destructor (slot 0, 0x146770) and sub_147d60 all call `WaitForAllCmdsToFinish`: unloading also waits for the whole queue.
- Candidates for the game-side request code (use the global RF pointer 0x2ee2d00): sub_2569d0 (also calls texture streaming sub_98a90), sub_2556c0, sub_25c080 (GameLoop). Not verified as the BuildAsync callers.

**Barrier and completion**
- 0x1469d0 WaitForAll: lock [obj+0x68]/[obj+0x70]; `while (HasPendingWork) { SetEvent([obj+0x98]); unlock; WaitEventFlag([obj+0xb8], AND|CLEAR, sub_3b4a0); lock; SyncPoint; }` (0x146aa0..0x146af3). The loader sets [obj+0xb8] only if [obj+0x91] was set by the waiter (0x146709).
- 0x147f40 HasPendingWork sums the 10 counts at +0x110..+0x278 and checks four lists (+0x280, +0x2a8, +0x2d0, +0x2f8) empty.
- 0x1480f0 SyncPoint: pops completions under a spinlock (`lock cmpxchg` at [obj+0x80]; after 10,001 spins `sceKernelUsleep(1)`, sub_3b570). Drains the completed lists in a loop (0x148550..0x14856d) with **no item or time budget**; invokes per-node callbacks (`[node+0x70]` at 0x148480, `[rax+0x38]` at 0x1482b3) on the main thread.
- Log: `event flag 'PS4SyncEvent' waited 5s for 0x1 (mode 0x11)` (last_run.3.log:235; last_run.4.log:189). All events are created as "PS4SyncEvent" (sub_3b3d0), so these lines cannot be attributed to the barrier vs idle loader waits.

**Area and texture side**
- Area state machine (game objects): `AreaLoaderState::ELoadState` Auto/Unavailable/NotLoaded/Loading/Loaded/Creating/Created(+FadeIn/FadeOut/Hidden); `CCPLdrAreaLoaderAreaLoaderManager`, `CCPLdrAreaLoaderAreaLoaderMap`, `cp10_setAreaLoaderState` (0xffc9bc).
- Texture streaming: `TextureManager` cvars (sub_13a9a0, sub_1418d0): "Enable Streaming", "Streaming Pool Total/Base Size MiBs", "Reserve System Free Mem MiBs", "Low Mem Recovery Size MiBs", "Num Mip Requests" / `numMipRequestsPerFrame`, "Max simultaneous load requests", "Min Num Frames Before Unload", "Unload High Mips" / `tx_unload_high`, "Cache Unused Textures". Defaults not extracted.
- Per frame: TextureManager update (sub_13a3e0, inferred) -> `TextureStreamingChunk::ProcessTextureData` (0x112b80); render job sub_d0710 -> sub_117140 -> `CRenderer::UpdateTextureStreamingGPUData<CRenderContext>` (0x117930) / `<CComputeContext>` (0x117160), which memcpy's and calls Gnm helpers (sub_16b8c0/16b900, unnamed).
- `TextureManager::ReleaseCachedHighMips` (0xfa850f, user sub_13e4b0).

**Port side (observed)**
- SOTCSTALL frame 154, 149 ms: `images 659 (6 ms)`, `upload 137.9 ms`, `tc ... (65)` (user/last_run.log, dev copy). Frames 155 and 1965 show 23 and 5 images but 81 and 94 ms.
- The port already gates first uploads: `texture_cache.cpp:1350-1363` (32 MB budget, log in `driver.cpp:1560`).
- Area events: `[GAME] Loading a save or a new area` at 00:00:49.912 and 00:00:09.957 etc. (SotC/user/last_run.log:276; other runs).

## 3. Stutter mechanism

1. **Frame-end barrier over all priorities (observed code; effect inferred).** Any request still pending at the end of GameLoop (streaming textures and background included) makes the main thread block on the loader's completion event for each batch until the queue is empty. Frame time becomes max(game work, loader drain time). The loader's drain time grows with the port's costs: sceFios reads into the host, Oodle decode on the loader thread, and guest writes into memory the GPU has used (write faults in the port). Area change puts ~16.5k requests in the queue, so the barrier waits for the entire load.
2. **Unbounded completion burst (observed code).** SyncPoint finalizes every completed node in one call. When many loads finish together (the end of a LoadInitialWorld or area stage), the callbacks and the port's first binds all land in one frame ("images 659").
3. **Unloading waits too (observed).** `DeleteResource` calls the same barrier, so releasing textures during a stream waits for pending loads.
4. **Possible synchronous loads on the game thread (inferred).** `BuildSync` decodes and waits inline. If the game calls it from GameLoop, the worst stall is a full read plus decode. Unverified.
5. **Port-side (inferred from port code and logs).** New guest resources become VkImages on first bind (the "images" count). The Gnm texture data is written by the loader into guest memory that the GPU mirrors, so the loader's writes trigger page-protection faults. The per-area `bbgpu_request_texture_flush` drops images right when the area loads.

## 4. Hook / redesign options

**A. Stop the frame barrier from waiting on streaming (cheap; test first).**
- Patch `HasPendingWork` (0x147f40) to ignore queues 0 (StreamingTexture) and 9 (Background), keep 1..8, and keep `SyncPoint` draining completions every frame. Ready items are still finalized on the main thread; the frame never waits for loads that are not needed this frame.
- Risk: game code that expects a resource to exist after the barrier (texture not yet resident). Mitigate with the engine's own fallbacks (low mips) and check for pop-in.
- Verify: time spent inside 0x1469d0 per frame (hook entry/exit), SOTCSTALL before/after, and visual check of texture pop-in in area load.

**B. Bound the completion burst.** Give `SyncPointProcessPendingResources` a per-call budget (count or ms) and leave the rest for the next frame. Same risk class as A; combine with A.

**C. Pre-stage Vulkan images on worker threads (the request).**
- Hook the loader's completion of CreateResource (after the `[task+0x18]` call at 0x148d14, or at the completion push in SyncPoint). At that point the decoded bytes are in guest memory and the completion event gives the happens-before edge. Enqueue (guest address, size, format from the resource header) to a port worker that creates the VkImage and fills a staging buffer, using the existing texture_cache upload budget as the pacer.
- The draw thread binds the prebuilt image; the first bind waits only on that image's fence, not on a full upload.
- Unknown: whether the GCN tile/format info is available at completion without the T# descriptor (see open questions).

**D. Radical: separate the loader's memory from the GPU mirror.** Make the loader write decoded data into a host-visible staging ring (not the page-tracked guest heap), and copy to device-local images on a transfer queue; the guest-side resource pointer then refers to a port-owned image handle. This removes the loader's write faults and the readback path for streamed data. Cost: requires the BPE heap/resource allocation to be redirected (the brief's heap guard issues apply).

**E. Pacing knobs.** Use the TextureManager cvars (`numMipRequestsPerFrame`, "Max simultaneous load requests", "Min Num Frames Before Unload") to cap new textures per frame, set via the port's cvar config; verify their defaults first.

Recommended order: A (+B) to remove the barrier stall, then C with the existing budget, D later.

## 5. Proposed names

- 0x144ca0 ResourceFactory::ResourceFactory (ctor; starts "Resource Loading")
- 0x1464d0 ResourceFactory::ThreadFunc
- 0x148600 ResourceFactory::ProcessLoadingThreadLoop
- 0x146770 ResourceFactory::~ResourceFactory
- 0x146b60 ResourceFactory::BuildSync
- 0x147490 ResourceFactory::BuildAsync
- 0x1478d0 ResourceFactory::DeleteResource
- 0x1469d0 ResourceFactory::WaitForAllCmdsToFinish (frame barrier)
- 0x147f40 ResourceFactory::HasPendingWork
- 0x147f00 ResourceFactory::GetPendingCount
- 0x1480f0 ResourceFactory::SyncPointProcessPendingResources
- 0x146ea0 ResourceFactory::PrepareNextResourceToLoad
- 0x147160 ResourceFactory::CreateResource
- 0x1451a0 ResourceFactory::DebugDumpStats (gated by debug flags)
- 0x149670 ResourceFactoryResourceLoader::ctor
- 0x14b800 ResourceFactoryResourceLoader::StartLoadJob
- 0x14b980 ResourceFactoryResourceLoader::LoadFileInternal
- 0x14aee0 ResourceFactoryResourceLoader::PrepareForLoad
- 0x14ae70 ResourceFactoryResourceLoader::IsJobDone
- 0x149df0 ResourceFactoryResourceLoader::ReleaseJobSlot
- 0x14cf00 ResourceWrapper::IsReadyToCreate
- 0x14d5d0 ResourceWrapper::GetState
- 0x1aa2a0 bpe::Compression::Decompress
- 0x89d3b0 bpe::Compression::OodleDecode (wrapper)
- 0x31830 FiosFile::PreadAsync (sceFiosFHPread)
- 0x312a0 FiosOp::WaitAndRelease
- 0x31900 FiosOp::IsDone
- 0xb10f60 GameFiles::MountPaks
- 0x3b3d0 bpe::threading::Event::Create ("PS4SyncEvent")
- 0x3b4a0 Event::Wait (sceKernelWaitEventFlag)
- 0x3b450 Event::Set
- 0x25c080 GameCoreLoop::GameLoop
- 0x3c5170 CGame::LoadInitialWorld
- 0x3c5a00 (calls the barrier; game loop tail)
- 0x117930 CRenderer::UpdateTextureStreamingGPUData<CRenderContext>
- 0x112b80 TextureStreamingChunk::ProcessTextureData
- 0x13a9a0 TextureManager::RegisterCvars (streaming pool)

## 6. Open questions

1. Which game code calls `BuildAsync` (vtable slot 3) for areas and mips, with what priority? Hook 0x147490 entry (log priority, resource id) during a run that crosses an area boundary.
2. Does `BuildSync` (slot 2) run on the game thread? Hook 0x146b60 and record the thread id.
3. Does the barrier actually block per frame? Hook 0x1469d0 entry/exit and sum time and iterations; compare with SOTCSTALL.
4. The codec of method 1 (sub_1b9130): zlib or other? Check its strings or the Fios header.
5. Which Fios2 thread performs `sceKernelRead` and where its zlib work happens (`SceFiosZlib0`).
6. TextureManager cvar defaults (`numMipRequestsPerFrame`, "Max simultaneous load requests", pool sizes). Read the default values from the registration code (sub_1418d0 / sub_13a9a0).
7. Thread identity: the thread that runs `GameCoreLoop::Run` (0x25a8d0 caller chain) is not confirmed as the main thread.
8. Whether GCN tile/format data is available at CreateResource completion, without the T# descriptor, for option C.
9. The "PS4SyncEvent waited 5s" lines: attribute them by naming events per caller (port-side log of the event handle).

Other-topic note: the "XPR occluder" readbacks and `sub_3c5a00` both sit at the frame tail; the barrier here runs right before that CPU-GPU sync (topic 04/readback owner).
