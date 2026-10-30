# Topic 07: first-use costs (shaders, pipelines, textures/images)

## 1. Answer

- In the current dev logs, first-use work is a minority of the stall time. Across 44 SOTCSTALL lines (frames over 60 ms, 4,594 ms total) pipeline, shader and image creation account for 381 ms (8.3%). One stall (frame 472, 250 ms) is 109% first-use: a synchronous pipeline build for a "tiny draw" (<= 6 indices), 254 ms. **observed**
- Image object creation is cheap now (about 15 us per image in the stall lines). The large first-use texture cost is the data upload: 3,400-4,400 uploads and 9-13 GB per session by the copy threads, plus the 32 MB/frame budget that puts first uploads off (up to 2,969 puts, 11.9 GB, 98 images filled with zeros in one run). **observed**
- The biggest pipeline cost in the whole log set was an older configuration, not the current default: 96 "graphics (depth only)" sync builds in crash dump 2026-10-27_22-36-19_0x17 (4.2 s total, median 43 ms, max 102 ms, in 18 seconds of play). That label is only printed when `SOTC_DEPTH_SYNC_PIPE=1`. The default is now off, and shadow pipelines are queued with priority. **observed (log) / inferred (env, from label logic)**
- A second, persistent cost: the boot-time cache preload skips the same 419 stored pipelines in every dev run (`WarmUp: 419 stale pipelines`). They are built again at first use. Cause: permutation index collisions (`Cached permutation N ... conflicts with index M`, 64 per run). **observed**
- Engine side: the engine does know an area's resource set ahead of time, but only as resource names, not as GCN shader addresses. Candidates: the dependency graph `shadow.cwks` (`ResourceDependenciesFile`), per-area precache lists (`$/Models/****/shadowmodels.crmp`, `cp3shadowprecache.calt`, ...), and a `ShaderList` reader (`sub_b0e3a0`). The port identifies a shader only when a draw binds its guest code (hash of code at `pgm` base). Bridging these needs a per-area manifest recorded on the first visit. **inferred**
- Recommendation: fix the two concrete defects first (no sync tiny-draw builds; stale permutation index), then consider per-area preloading driven by the engine's area load (`sub_533100`). Texture work should focus on the upload path, not image objects.

## 2. Evidence

### 2a. Log quantification (observed)

Source: `user/last_run*.log` (5 dev runs) and `user/crashes/*.log`; parsed with `scratchpad/re/07/stall_stats.py` and `sync_stats.py`.

| Source | Metric | Value |
|---|---|---|
| 5 dev runs | SOTCSTALL lines (frames > SOTC_STALL_MS, launcher sets 60) | 44 |
| 5 dev runs | Sum of stalled frame time | 4,594 ms |
| 5 dev runs | Pipelines built on the CP (sync) | 3 builds, 272 ms (one frame: 472) |
| 5 dev runs | Shaders compiled in stall frames | 24, 53 ms |
| 5 dev runs | New images in stall frames | 3,742, 56 ms (about 15 us each) |
| 5 dev runs | Stall lines dominated (>= 50%) by first-use | 1 of 44 |
| 5 dev runs | Area-change frames 148-157 (600-700 images, 4 shaders each) | first-use 6-15 ms; cp 87-125 ms, cpu draw 90-120 ms |
| `last_run.1.log:frame 472` | `SOTC SYNC PIPELINE graphics (tiny draw) 254 ms` | at 41.4 ms after first sync build |
| crash 0x17 (2026-10-27) | `SOTC SYNC PIPELINE`: 96 lines, 94 "depth only" + 2 "tiny draw" | 4,230 ms; median 43; max 102; 18 s with builds, up to 16 per second |
| crash 0x3 (2026-10-28, 10-29) | sync builds, tiny draw | 2 lines, 174 ms, max 136 |
| All logs | sync builds total | 99, 4,658 ms (median 43, p90 58, max 254) |
| 5 dev runs | `WarmUp: N stale pipelines` | 419 each run (identical) |
| crash logs | stale pipelines | 1,762 / 1,062 / 462 |
| 5 dev runs | `Cached permutation ... conflicts with index` | 64 unique lines per run |
| 5 dev runs | `SOTC async image create` at end of run | queued 607-790, made by workers 595-769, taken ready 573-731, created on the spot 1,046-1,445 (`async_image_sync`) |
| 5 dev runs | `SOTC async upload` at end of run | 3,436-4,430 uploads, 9.0-13.3 GB by copy threads, submits waited 440-573 ms |
| 5 dev runs | `SOTC ASYNC COMPUTE built 702` (workers) | 109-196 ms, recorder waited 0 times |
| 5 dev runs | `SOTC ASYNC TRANSLATE` (workers) | 4-11 stages, 7-29 ms |
| Cache dir `user/cache/CUSA08809_nodma` | files | 5,308 `.spv`, 5,307 `.meta`, 3,888 `.key`; `sotc_vk_pipelines.bin` 25.8 MB |

Caveat: SOTCSTALL is printed only for slow frames, so these counters are biased toward big stalls. The total number of shader compiles per run is not printed anywhere (`shaders_compiled` is only in the SOTCSTALL line). **observed**

### 2b. Port code paths (observed)

- Pipeline lookup and sync/async decision: `gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp` `GetGraphicsPipeline` (1028). Flags: `depth_sync` 1030-1038, `depth_only` 1042. Tiny draw (`num_indices <= 6`) 1084, always built on the CP thread (the async branch at 1085-1087 excludes it). Sync build and log at 1095-1106 (`LogSyncPipeline`, threshold `SOTC_SYNC_PIPELINE_MS`, default 30 ms; lines 589-609).
- Async pipeline queue: `QueueAsyncPipeline` (653), priority insertion for depth-only draws. Worker loop `AsyncPipelineWorker` (687). Translation and compute workers: lines 802-1008. Shader translation `CompileModule` (1418; `shaders_compiled` at 1433).
- Shader identity: `GetProgram` (1481) keys by `params.hash` (hash of guest code). `perm_idx = program->modules.size()` (1528). The port sees a shader only at its draw.
- Boot preload: `PipelineCache::WarmUp` (`vk_pipeline_serialization.cpp:507`). `LoadPipelineStage` (432) refuses a stored permutation whose index differs from the spec's current slot (474-484, warning at 481-484). Stale count logged at 618-620. `InsertPermut` (`vk_pipeline_cache.h:73`) resizes the vector and writes at `perm_idx`.
- Image creation: `TextureCache::FindImage` sync create (`texture_cache.cpp:946-947`, `image_ns`, `images_new`). Prefetch design in `texture_cache/image_prefetch.h` (header comment). `SOTC_ASYNC_IMAGE_CREATE` is off in code (`image_prefetch.cpp:53`) but on in the user's `user/launcher_settings.json` extra_env.
- Stall emitter: `vk_presenter.cpp:1615` (SOTCSTALL), threshold at 1560-1565 (`SOTC_STALL_MS`, default 70).
- Cache pack: `cache_storage.cpp:147-345` (`SOTC_CACHE_PACK`, `cache.sotcpack`).
- Upload budget log: `driver.cpp:1558-1560`.

### 2c. Engine side (observed unless marked)

Startup and dependency file:
- `0x536730` builds the startup command line with `+ResourceDependenciesFile=$/workspaces/_ps4/shadow.cwks` (string `0x1015285`), `+StartupScript=$/scripts/shadow/shadowmain.sgpr`, `+startupArea=%s`.
- `0x2534c0` references the cvar name `ResourceDependenciesFile` (`0xfc38b4`).
- `0x2567f0` = `GameCoreLoop::LoadResourceDependencyFile()` (string `0xfc3bb8`). It checks the cvar string and calls `0x3e7e0`. Asserts at GameCoreLoop.cpp lines 652/658/662 (inferred).
- `0x14efb0` = `ResourcePool::LoadResourceDependencies(const String&)` (virtual; string `0xfaa555`). `0x14edc0` = `ResourcePool::RecordResourceDependencies()` (string `0xfaa516`). `0x14e560` = `ResourcePool::GetResourceDependencies(ResourceId, vector<ResourceRef>&)` (string `0xfaa3c2`). The dependency graph is stored as `ResourceRef` lists. Whether `0x14efb0` reads shadow.cwks directly is inferred, not verified.

Per-area lists and shader list:
- `0x533100`: contains `$/Models/****/shadowmodels.crmp`, `shadowskels.crmp`, `shadowanims.crmp`, sound banks/streams/random events/boss music `.crmp`, `$/characters/shadow/****/cp3Characters.calt`, `$/cp3shadow/****/cp3shadowprecache.calt` (`0x1014e65`-`0x1014f90`), plus `show_areaInfo` and `r_fps`. The `****` placeholder is literal in the binary, so it is most likely the area name (inferred). It calls `0x2569d0`, which calls `0xb0e3a0` (the function with the string `ShaderList`, `0x12cfce8`). So `0x533100` is the likely per-area load step that also reads the shader list (inferred).
- `0x101ce94` `collision_map.crmp` referenced from `0x560b60`. `0x12720b3` `effect_map.crmp` referenced from `0x690060`. `0x128ac45` `mainMenuPrecacheList.calt` referenced from `0x7ea310`.
- `bp_precache/` path built in `0x56c620`, called by `0x56cdb0` (callers include `0x6ac2c0`, `0x6ac470`, `0x6ad9b0`). `BP_PrecacheSegmentData` in `0x6ac470`. `nico_precachefiles.txt` (`0x12d12a9`) has no references found.
- `shaderTypeToLoad` cvar: `0xb0e250` (string `0x12cfbe0`), enum `EShaderTypeToLoad {Debug, Shipping, PS4Neo_*}` (`0x12cfc22`-`0x12cfcaf`). The log shows `+shaderTypeToLoad=Shipping`.

Resource loader (names from strings, call graph partial):
- `0x146ea0` `ResourceFactory::PrepareNextResourceToLoad` (string `0xfa976d`). `0x14aee0` `ResourceFactoryResourceLoader::PrepareForLoad` (`0xfa9e69`). `0x14b800` `StartLoadJob` (`0xfa9f5d`). `0x14b980` `LoadFileInternal` (`0xfa9f90`). `LoadAsDependency` priority (`0xfa9c18`, used in `0x1451a0`). Thread name `Resource Loading` (`0xfa94a5`).
- Resource types that carry shader and material references: `.cslt` shader libraries (`0xfa8d4b` `engine-shaders.cslt`, `particle-shaders`, `lightprobe-shaders`), `.cmat` materials (`0xfa9002`-`0xfa9169`), `.cmdl` models (`0xfa8e31`). Game data resources are not in the repo; the dump at `<install>\CUSA08809` has `pak*.psarc` and `cachedFiles.psarc` (not inspected).

GPU shader binding (sceGnm imports):
- Wrappers, each calling one import: `0x86c050` (`sceGnmSetPsShader350`), `0x86c110` (`sceGnmUpdatePsShader350`), `0x86c170` (`sceGnmSetVsShader`), `0x86c250` (`sceGnmUpdateVsShader`), `0x86c330`/`0x86c3a0` (`sceGnmSetHsShader`), `0x86c0b0` (`sceGnmSetEmbeddedPsShader`), `0x86c1e0` (`sceGnmSetEmbeddedVsShader`), `0x86c7d0` (`sceGnmSetCsShaderWithModifier`).
- Callers of the graphics wrappers: `0x85f410` (2,042 bytes) and `0x85fc10` (3,995 bytes). Callers of the compute wrapper: `0x85f220`, which is called from about 10 functions (`0x928f0`, `0x123410`, ...). `0x85f410` has one direct caller, `0x862c50`, which has no direct callers (likely reached through a vtable; unknown).
- Ghidra decompilation is not present (`out/re/decomp` empty), so none of the above is checked at the pseudocode level.

## 3. Stutter mechanism

- **Sync tiny draw (observed):** a full-screen or post pass with <= 6 indices is always built on the CP thread (`vk_pipeline_cache.cpp:1084`). A new key costs the driver's full pipeline creation. The 254 ms case is a single pipeline (plausibly driver compilation of a missing permutation; inferred). It stalls the whole CP and draw chain for that frame.
- **Stale cache each boot (observed count, inferred effect):** 419 stored pipelines are not preloaded, so they are built at first draw. Each boot repeats the cost, and the count stays at 419 across the five runs. The code keeps the mismatched entry as is (`vk_pipeline_serialization.cpp:474-480` says the store mixes cache generations; the slot-meta comment at 96-100 notes the same kind of leftover). Whether anything rewrites the store is inferred, not checked.
- **Depth-only sync (observed in crash 0x17):** with `SOTC_DEPTH_SYNC_PIPE=1`, each shadow pipeline build was 32-102 ms on the CP thread, and area changes triggered 16 per second. This was the largest first-use cost in the logs (4.2 s). The current default (async with priority) removes it, but this is not verified in a current run.
- **Textures (observed):** object creation is cheap, but each area change uploads hundreds of MB to GB. The copy threads' submits waited 440-573 ms per run. The per-frame budget defers first uploads, which shows as zero-filled images (98 in one run).
- **Engine knowledge (inferred):** the engine lists an area's resources before drawing (precache lists, `shadow.cwks`). The port cannot use them because it keys shaders by guest code hash at draw time, not by resource name.

## 4. Hook / redesign options

1. **Remove synchronous tiny-draw builds (cheap, direct).** In `GetGraphicsPipeline` (`vk_pipeline_cache.cpp:1084-1095`), a tiny draw with a new key should not block. Options: (a) draw it with the last pipeline that shares the same fragment and vertex modules, accepting one frame of a slightly wrong full-screen pass; (b) queue it and draw with the previous frame's output (skip the pass for one frame). Verify: no `SYNC PIPELINE ... tiny draw` lines in a run with `SOTC_SYNC_PIPELINE_MS=1`; SOTCSTALL `pipelines` 0 at area changes. Risk: visible one-frame artifacts on post passes.
2. **Fix the stale permutation index (cheap, direct, removes 419 per boot).** In `LoadPipelineStage` (`vk_pipeline_serialization.cpp:474-484`), do not reject a permutation whose stored index differs. Either place the module at its stored index (`modules.resize(perm_idx + 1)`, filling gaps) or remap the key to the existing index on load and rewrite the stored key. Verify: `stale pipelines` warning gone; `conflicts with index` 0; a second boot with no compiles at first draw (count `SOTCSTALL shaders` and `pipelines` in the first area visit).
3. **Per-area manifest and preload (medium).** Record, on each area's first visit, the set of graphics keys, shader hashes and T# signatures the draws used, keyed by the engine's area name (`startupArea`). Then, when the engine starts that area (hook: `0x533100` or the return of `0x14efb0`), queue translations (`QueueAsyncTranslate`), pipelines (`QueueAsyncPipeline`, priority) and `PrefetchImages` for the manifest. Expected effect: the first visit of an area looks like a second visit. Costs: worker time, memory for unused pipelines, and the manifest format. Risk: low if preloads are only queued (draws still build what is missing). Verify: run one area twice in one session and compare SOTCSTALL first-use counts; the boot cache already covers most keys, so measure the first-ever visit with an empty cache.
4. **Engine hook at resource load (radical, uncertain).** At the end of `ResourceFactoryResourceLoader::LoadFileInternal` (`0x14b980`) for `.cslt`/`.cmat`/`.cmdl`, pass the loaded buffer to the port. The port could translate the GCN shader binaries on a loader thread, and parse texture references for `PrefetchImages`. Limit: pipelines still need draw state (blend, depth, vertex format), so only shader translation moves off the draw path. Requires the resource format (not yet read).
5. **Texture uploads (radical for textures).** Do the texel conversion (tiling, decode) on the resource-loading thread so the copy threads only memcpy into staging. Measure first with a per-run `upload bytes` and `first uploads put off` count against stall frames. Keep `SOTC_ASYNC_IMAGE_CREATE` as is: the measured gain is small (56 ms of image work in 44 stalls).

## 5. Proposed names

- `0x2567f0` GameCoreLoop::LoadResourceDependencyFile (string-confirmed)
- `0x14efb0` ResourcePool::LoadResourceDependencies (string-confirmed; body inferred)
- `0x14edc0` ResourcePool::RecordResourceDependencies (string-confirmed)
- `0x14e560` ResourcePool::GetResourceDependencies (string-confirmed)
- `0x2534c0` Cvars::RegisterResourceDependenciesFile (inferred)
- `0x533100` AreaLoad_PrecacheLists (inferred; shadowmodels.crmp et al.)
- `0x2569d0` AreaLoad_ShaderListAndAssets (inferred)
- `0xb0e3a0` ShaderList_Load (string `ShaderList`)
- `0xb0e250` Cvar_ShaderTypeToLoad (string-confirmed)
- `0x536730` BuildStartupCommandLine (strings)
- `0x56c620` PrecacheBuildPath_bp_precache (string `bp_precache/`)
- `0x6ac470` PrecacheSegmentData_Load (string `BP_PrecacheSegmentData`)
- `0x146ea0` ResourceFactory::PrepareNextResourceToLoad (string-confirmed)
- `0x14aee0` ResourceFactoryResourceLoader::PrepareForLoad (string-confirmed)
- `0x14b800` ResourceFactoryResourceLoader::StartLoadJob (string-confirmed)
- `0x14b980` ResourceFactoryResourceLoader::LoadFileInternal (string-confirmed)
- `0x86c050` GnmShader_SetPs350 (import `sceGnmSetPsShader350`)
- `0x86c110` GnmShader_UpdatePs350 (import `sceGnmUpdatePsShader350`)
- `0x86c170` GnmShader_SetVs (import `sceGnmSetVsShader`)
- `0x86c250` GnmShader_UpdateVs (import `sceGnmUpdateVsShader`)
- `0x86c7d0` GnmShader_SetCsWithModifier (import)
- `0x85f410` Material_BindGraphicsShaders_A (inferred, 2,042 bytes)
- `0x85fc10` Material_BindGraphicsShaders_B (inferred, 3,995 bytes)
- `0x85f220` Material_BindComputeShader (inferred)

## 6. Open questions

- Total shader compiles and translation time per run: add one summary line (`SOTC shaders: N compiles, M ms`) to see the share outside stall frames. Settles whether the 24 compiles in stalls is representative.
- Are the 419 stale keys the same as the tiny-draw keys? Check by dumping the key set (`SOTC_SYNC_PIPELINE_MS=1` plus a key dump) and comparing the sync-built keys with the stale list.
- Did crash 0x17 run with `SOTC_DEPTH_SYNC_PIPE=1`? The "depth only" label implies it. Check `user/launcher_settings.json.bak` (27 Oct 21:30) or the events log.
- What do `0x533100` and `0x2569d0` do at runtime? Trace by breakpoint or log in a dev build (not done here: read-only). Whether they run per area or at boot decides the hook point for option 3.
- How `shadow.cwks` maps resources to shaders: need the file format (inside `pak*.psarc`, not inspected) and the `0x14efb0` body.
- Whether `0x85f410`/`0x85fc10` are reached through vtables and which renderer calls them (needs an indirect-call trace).
- Out of topic, seen in passing: `PS4SyncEvent waited 5s/10s/15s` lines (about 47 per run) belong to the wait/sync topic.
