# Shadow of the Colossus PC — full project review

Status as of 12/10/2026 (branch `sotc`, commit `e5d6233` and later). Public repository:
<https://github.com/AjncSystem/ShadowOfTheColossus-PC> (versions 0.0.8 and 0.0.9 published).

This document brings together, in one place, what the project is, how it works, everything done
so far, performance numbers, known issues, and next steps.

---

## 1. What it is

A **native Windows port** of PS4 *Shadow of the Colossus* (CUSA08809, EU, executable version
1.01). It is not a full emulator like shadPS4: the game code runs **directly on the PC processor**,
and only PS4-specific functionality is recreated:

| PS4 component | How the port handles it |
|---|---|
| CPU (AMD Jaguar, x86-64) | The game code runs **natively** on the PC’s x86-64 (same architecture). Only the few instructions Intel lacks (SSE4a) and use of the *red zone* are fixed during loading. |
| Operating system (Orbis OS) | A custom runtime (`src/runtime_*.c`) implements the system functions called by the game: memory, files, threads, saves, audio, controller, dialogs… |
| GPU (AMD GCN "Liverpool") | **Emulated**: the game’s PM4 commands and GCN shaders are translated to **Vulkan** (the shadPS4 video core, with our fork’s fixes). |
| Game libraries (libc, Fios) | Those from the game dump itself are linked natively during preparation. |
| Audio (libSceNgs2 / libSceUlt) | The **user’s own console** modules are linked (never distributed). |

Based on the *bbport* project (native Bloodborne port, GPL-2.0), adapted for SotC.

## 2. How it works (execution flow)

1. **`SotC.exe`** (launcher, Python/Tkinter frozen with PyInstaller): initial setup,
   game library, graphics/controller options, then calls `sotc.py`.
2. **`sotc.py`**: prepares the game once (and again only when something changes):
   - `scripts/prepare.py` converts `eboot.bin` (SELF) into a flat image;
   - `link_modules.py` / `link_libc.py` link libc, Fios, and the user’s audio modules;
   - `content_profile.py`, `sotc_cpu.py` (CPU fixes), `sotc_modes.py` (60 FPS, AF 16x,
     engine cvars);
   - if an update (`CUSA08809-UPDATE`/`-patch`) with `eboot.bin` exists, its image is used.
3. **`bb-probe.exe`** (the "game"): C loader (`src/probe.c`) + runtime + video core
   (`gpu/`). Loads the image **below 2 GiB** (the game stores 31-bit pointers),
   creates the game’s threads/fibers, and starts.

### Directories

| Folder | Contents |
|---|---|
| `src/` | loader and runtime (memory, files, saves, audio, pad, services, fibers, SotC) |
| `gpu/shadps4/` | video core (video_core, shader_recompiler, gnmdriver, videoout) |
| `gpu/shim/` | bridge between runtime and video core, SDL window, ImGui overlay |
| `gpu/sotc/` | CPU fixes (SSE4a, red zone) ported from the fork |
| `scripts/` | game preparation |
| `launcher/` | `SotC.exe` (sotc_launcher_win.py, sotc_discovery.py, icons) |
| `tools/` | automated tests, benchmark, A/B, profiler |
| `packaging/windows/` | `package_sotc.sh` → `dist/SotC-PC.zip` (rejects `.sprx/.prx`) |
| `docs/sotc/` | this document, package README, "Path B" plan |

## 3. Timeline

### 07/10 — the game starts
- Imports matched by NID; SotC functions in their own file (`runtime_sotc.c`).
- Extra memory window `0x700000000–0x800000000` (the game maps at a fixed address).
- The game reaches its own initialization.

### 08/10 — playable
- **Fibers** (`runtime_fiber.c`), **CPU fixes** (SSE4a, red zone), and **image below
  2 GiB** → first frames (falcon in the canyon).
- Fork GPU fixes applied to bbport’s core → intro without black polygons.
- **Turning point:** the video core from **our shadPS4 fork** replaced bbport’s → logos,
  menus with text, save loading, open field at 30 FPS, 10 min without a crash.
- Silent crash fixed: the page tracker was protecting the fiber stack.
- **Real audio** with the user’s modules; `sotc.py`/`SotC.bat` launcher; Windows package.
- **Modes**: AF 16x and 60 FPS (patch at load time, `BB_VBLANK_HZ=120`).
- `SotC.exe` (graphical launcher), bbport 0.4 controls (keyboard + controller together,
  remapping), mouse camera, 720p–4K output resolution, PS4 Pro mode, engine effects.

### 08–09/10 — performance
- Fixed benchmark: the **title-screen demo** (same route every time).
- Improvements: shader information cache, HLE compute that fills tables, per-thread memory
  pointer cache, Pink culling cvars, BPE heap guard, non-blocking buffer sync, indirect draws
  with CPU-side arguments converted to direct draws, Vulkan recording on a separate thread,
  image description/search caches, fewer write faults, periodic time-based submission
  (`SOTC_FLUSH_US=500`).
- Demo result (60 FPS, i9-13900K + RTX 2060 SUPER): **average 37–45 → ~67 FPS**, heaviest section
  (canyon bridge) **~17 → 29–34 FPS**.

### 12/10 — launcher, release, saves, textures
- **Public 0.0.8**: game library, automatic search, key rebinding, new icon, GPU work submission
  option (AMD/Intel).
- **Public 0.0.9**: initial setup screen (asks only for the main folder, like shadPS4),
  updates handled like shadPS4 (`-UPDATE`/`-patch` over the base game), **DLC** in folders or
  **`.pkg` without installing** (all 6 DLCs listed in the game).
- Project consolidated into a **single folder** (`SotC-PC`).
- **Saving fixed**: the game could never save ("not enough space").
- **Texture streaming** (`GET_LOD_STATS`): sharp nearby textures, waterfalls with water curtains,
  rippling water surface.
- **On-screen save list** for loading and saving, with **save deletion**.

## 4. Current features

- **Launcher** (`SotC.exe`): first-run setup; finds the game, updates, DLCs, shadPS4 saves, and audio
  modules; library; graphics options (output resolution, FSR 1, sharpening, AF 16x, 60 FPS, PS4 Pro,
  effects), controls (keyboard/controller/mouse, remapping), GPU work submission; desktop shortcut
  with the game icon.
- **Game**: runs from beginning to end in normal mode; real audio; saves (save, load, delete);
  DLCs; 1.01 update; PS4 Pro mode (Cinematic/Performance in the game menu).

## 5. Main technical fixes

| Issue | Cause | Fix |
|---|---|---|
| Silent crash with no message | fiber stack page protected by GPU tracker | fiber stacks registered and excluded from tracking |
| Broken game pointers | game stores 31-bit pointers | image loaded below 2 GiB |
| Black polygons in the intro | missing fork GPU fixes | fork video core |
| Intermittent black screen | bbport optimizations incompatible with SotC | fork core; precise GPU readbacks |
| Game could never save | port reported every save as 1 GiB | actual save sizes |
| Loading looped / every save created a new slot | list dialog "completed" without a selection | on-screen list (ImGui overlay) |
| Blurry textures, black water, waterfalls without curtains | `GET_LOD_STATS` command ignored → game never loaded fine mips | statistics emulated from used textures; textures resubmitted when the game maps new memory |
| GPU queue stalled with threaded recording | presenter waited one tick before it was submitted | wait for submission before presenting |

## 6. Known issues (priority)

1. **Memory corruption with texture streaming.** Requesting maximum resolution (mip 0/1) for
   everything causes the game to corrupt its own memory within 1–4 minutes; with the current limit (mip 2,
   512×512), the demo runs to completion, but **loading another save without restarting the game may close it**.
   Recommendation: restart the game before switching saves. Tested and **ruled out** as causes
   (12/10): EOP labels written early (`SOTC_EOP_DEFER=1` writes when the GPU finishes: FPS drops
   by half and the failure continues), the HLE copy marking the entire range as GPU-written
   (fixed to mark only the chunks; the failure continues), and runtime memory allocation failures (none).
   The symptom is always game-reused memory still being read/written with stale data
   (garbage heap nodes, descriptors, and shaders) around 250–270 s into the demo with mip 0/1.
   Accounting fix (15/10): with `Linger=0`, banks used in the current report were also
   marked as unsampled (`idle < Linger` is never true). A bank marked in this interval is now
   reported as used; the default limit remains mip 2. This fixes `GET_LOD_STATS` reporting,
   but does not prove that mip 0/1 or switching saves without restarting is safe. The unit test
   confirmed the used/unused cycle. A 390 s attract soak, with a temporary user and mip 2,
   recorded sampled banks (0–82 per report) without heap failure or device lost; the process was
   ended by the test timeout. Since it did not load a progression save or test mip 0/1, the original
   corruption cannot be declared fixed.
   Additional validation (15/10): a temporary user loaded `SAVEDATA0ShadowProgress0`
   read-only; the original save was not used. In the 600 s soak, at mip 2, there were
   54 FPS samples between 331–597 s: median 19.7 FPS, average 20.6, no windows at 50 FPS;
   median frame-time p50 was 50.07 ms and p99 was 59.27 ms. Captures show the intro at the
   Shrine of Worship with no obvious visual corruption, and the log contains no `device lost`
   or heap corruption. It records eight `Not protecting guest stack pages` errors, which
   remain a separate diagnostic to investigate. The sequence remained in the intro/cinematic,
   not the canyon or combat; therefore it does not measure the demanding case or prove the
   original failure fixed. Mip 0/1 remains untested.
   Additional profile (15/10): a 430 s run confirmed `SAVEDATA0ShadowProgress0`
   mounted read-only from a save copy; captures show Wander at the Shrine
   of Worship, with no obvious visual corruption. With the profiler active, 16 windows between 351–426 s
   averaged 24.85 FPS, average p50 of 41.47 ms, and average p99 of 50.11 ms. The
   `GpuCommandProcessor` profile had 11,078 samples, with `DrawIndirect` (50.2%) and `BindResources`
   (40.4%) at the top inclusively; these times overlap and are not an A/B benchmark. The log records
   no `device lost` or heap corruption. This test remained in the temple at mip 2, so it does not
   replace a canyon/combat soak or demonstrate that the original failure is fixed.
   Controlled reproduction (16/10): loading `SAVEDATA0ShadowProgress1` from an isolated copy,
   with mip 2 and default LOD feedback, exploration of the same location/castle closed after 1085 s. The crash
   repeated at offset `0x1a371`; registers show the game routine scanning 1264 slots without
   finding an available one and using `-1` as the index. With `SOTC_LOD_STATS=0`, the same route ran
   for about 8 min without closing, but textures lost sharpness; this suggests streaming pressure,
   but does not prove the cause or stability. `SOTC_LOD_MAX_BANKS` was added as an opt-in test;
   there is no change to the default limit until duration and image quality are validated.
   With caps of 64 and 32 banks, the `0x1a371` crash continued (950 s and 308 s). With
   `SOTC_LOD_STATS=0`, a hang occurred at ~688 s (process still alive, `GpuCommandProcessor` blocked in
   `runtime_memory_is_mapped` waiting for the memory-table rwlock, `PS4SyncEvent` waiting
   15 s), while a second identical run passed 1594 s without failure: the mode is better, but not
   proven stable. The stack report (`BB_STACKS_TRIGGER`) now prints the lock’s
   `writer_tid`/`readers`/`writer_waiters`/`acquired_at` to identify its owner in a future hang.
   Second hang (session 11, ~51 min, `SOTC_LOD_STATS=0`): a `BPE JobWorker` held the write lock
   inside `batch_map2 → unmap_locked`, with no other writers or readers, blocking the
   `GpuCommandProcessor`; what it was waiting for was not established.
   Sessions 12–15 (default): three closed with `0x1a371` at ~82–87 s, preceded by 7–17 s stalls
   with no slow pipeline compilation or `DrawIndirect` scope ≥500 ms. Short stalls
   (0.5–1.5 s) coincide with shader translation (`RefreshGraphicsKey`). Opt-in diagnostics:
   `SOTC_STALL_SCOPE=1` (scopes ≥500 ms) and `ReportSlowCompile` warnings (≥100 ms) on stderr.
   Session 15 froze due to a bug in `BB_STACKS_TRIGGER` itself: the dumper printed to
   `stderr` while the target thread was suspended (and held the `stderr` lock), causing a deadlock; output is now
   buffered and written after `ResumeThread`. This freeze is not evidence of a game issue.
   Cause of `0x1a371` (sessions 16–21): the game has a 64 KB block heap (guest object
   `0x1000000140` in this EU CUSA08809 build; bitmap at `+0x50`, words at `+0x58`). With no free blocks,
   the routine uses index `-1` without checking and crashes. With LOD feedback enabled, free blocks
   drain (~250/s) to 0; with `SOTC_LOD_STATS=0`, they remain balanced (~10k free), but textures are blurry.
   `SOTC_HEAP_WATCH=<address>` records the count; `SOTC_LOD_HEAP_GUARD=<address>[,low[,high]]`
   (opt-in, build-specific address) reports banks as "unsampled" when few blocks are free.
   Horse route with guard: >393 s and 417 s without a crash (sessions 19–20); visual impact not
   verified. Attract: the demo starts only ~315 s after booting to the title screen; use ≥900 s and measure
   from ~330 s onward. 900 s A/B (n=1): default alive for 892 s, average 47 FPS; guard alive for 867 s, average
   58 FPS (the guard never activated); sample too small to conclude an effect.
   Six-run, 720 s battery (n=3 per arm, ≥330 s, no crashes): default average
   45.2/46.0/50.9 FPS; guard 44.6/50.8/48.7; minimum ~16 FPS and p95 ~31–35 ms in both: no measurable
   effect. `SOTC_WAIT_MARKER=2 SOTC_WAIT_SPIN_US=200` (n=3): 48.8/56.7/45.6 FPS, minimum ~16,
   p95 29–34 ms: within noise (±3 FPS across identical runs), default retained. Stable 50 FPS
   not achieved: minimums of ~16 FPS come from demanding attract sections.
   `GpuCommandProcessor` thread profile in the demanding section (345–385 s, 22,520 samples, thread ~100%
   busy): flat, with no single hotspot. Inclusive: `BindResources` ~20% (`BindBuffers` 13%,
   `BindTextures` 4%), GPU wait in readbacks (`DownloadMemory`→`FinishHostRead`→`Wait`) ~17%,
   `GetGraphicsPipeline`→`RefreshGraphicsKey` ~11% (`GetProgram` ~6%: `RefreshFlatBuf`,
   `StageSpecialization`, and linear permutation search on every draw), `DrawIndirect` 16%,
   `DispatchDirect` 15%. No single item exceeds ~20%; no sustained code change identified.
   Candidates (not implemented, require A/B): Pink’s `incremental_bind`/`mapped_page_table` for
   `BindResources`, and caching the last permutation per program in `GetProgram`.
2. **Black shadows**: areas in shadow lack ambient light (not made worse by recent changes).
   Clue (RenderDoc capture of the intro temple, ~178 s): a half-resolution input to the
   lighting passes (960×540, A2B10G10R10, read by ~16 light draws) is **never written
   during the frame and remains zero** — likely missing ambient occlusion/indirect light. It is not yet
   known which pass should write it (no writes as an image or buffer have been found).
3. **Water**: lake is opaque/smooth in some places; wave balance not confirmed.
4. **Exit during the title-screen demo** after several minutes (same family as issue 1).
5. Older saves copied from shadPS4 appear with the label "Profile" (the label is corrected when
   saving over them).
6. **AMD/Intel**: submission optimizations remain disabled by default and have had little testing. In
   the AMD report in issue #7, Vulkan format-property queries reportedly took ~5 ms per
   texture and caused 200–500 ms frames when rotating the camera; image creation repeated the query
   even for combinations already checked. The result is now memoized per device
   and the five query parameters (format, type, tiling, usage, and flags). This optimization is
   based on the report and has not yet been measured or visually confirmed on this machine. The corruption reported
   on RDNA3 in issue #43 (reportedly dependent on area/window size in one report) still has no isolated
   cause and requires logs/capture and testing on AMD.

## 7. Diagnostics (environment variables)

| Variable | Function |
|---|---|
| `SOTC_FPS=1` | FPS and GPU readbacks in the log every 5 s |
| `SOTC_SAVE_TRACE=1` | records save calls |
| `SOTC_LOD_STATS=0` / `SOTC_LOD_LEVEL=n` / `SOTC_LOD_MAX_BANKS=0..256` / `SOTC_LOD_TRACE=1` | streaming: disable, level, limit sampled banks (opt-in), trace |
| `SOTC_HEAP_WATCH=<address>` / `SOTC_LOD_HEAP_GUARD=<address>[,low[,high]]` | game block-heap diagnostics / reduces LOD feedback under pressure (opt-in, build-specific) |
| `SOTC_MAP_INVALIDATE=0` | does not resubmit textures when the game maps memory |
| `BB_WRITE_LOG=1` | logs port writes to game memory (dumped on failure, with totals by origin) |
| `SOTC_RDOC_TRIGGER` + `SOTC_PLAY_WRAP` | RenderDoc capture of a GPU submission (`renderdoccmd capture`) |
| `SOTC_SHOT_OVERLAY=1` | screenshots with the overlay (save list) |
| `BB_FRAME_DUMP_TRIGGER` | screenshot of the frame when the file appears |
| `BB_PAD_FILE` / `BB_PAD_RECORD` / `BB_PAD_REPLAY` | file-based controller input, record/replay a route |
| `SOTC_PROFILE=<thread>` | sampling profiler |
| `SOTC_FLUSH_US`, `SOTC_RB_AHEAD`, `SOTC_RECORD_THREAD`, `SOTC_INDIRECT_DIRECT`, `SOTC_FILL_HLE` | optimizations (toggle for testing) |
| `SOTC_WAVE64_UNIFORM=0/1` | forces wave64 uniform-branch analysis off/on; each mode uses its own pipeline cache |

Tools: `tools/sotc_play.py` (scripts: `t:cross`, `t:shot`, `t:rdoc`…),
`tools/sotc_bench_attract.sh` (benchmark), `tools/sotc_intro_ab.sh` (A/B on the intro river),
`tools/profile_sym.py`.

## 8. Build, package, test

```bash
# build (MSYS2 CLANG64 at I:\msys64)
TMP=$(cygpath -w /tmp) PATH=/i/msys64/clang64/bin:/i/msys64/usr/bin:$PATH ninja -C out/gpu bb-probe bb-gpu-capabilities
cp out/gpu/bb-probe.exe out/gpu/bb-gpu-capabilities.exe out/
# package (dist/SotC-PC.zip and dist/SotC-PC/ with saves preserved)
bash packaging/windows/package_sotc.sh
# benchmark (60 FPS, with the actual game culling cvars)
BENCH_CVARS="doIndexBufferCulling=0 doSunShadowIndexBufferCulling=0" bash tools/sotc_bench_attract.sh
```

Project rules: commits as `AjncSystem <AjncSystem@users.noreply.github.com>`; never
distribute Sony firmware (`.sprx`); large dumps go on the I: drive.

## 9. Next steps

1. **Streaming stability** (issue 1): EOP labels written when the GPU
   finishes ("Path B", a step already prototyped with `VK_EXT_external_memory_host`), then
   unlock maximum texture resolution.
2. **Shadows and ambient light**: capture a scene with RenderDoc and trace the lighting pass.
3. **Water**: surface shader (draw found in the capture) and wave animation.
4. Progress indicator with % on the initial black screen.
5. Tests on AMD and Intel.

### About DirectX 12

The port **is already native Windows x86-64**: the game code runs directly on the processor. The only
"emulated" part is the GPU, which is currently translated to **Vulkan**. Switching to DirectX 12 would mean
rewriting the entire video core (thousands of command and shader translation files
GCN → SPIR-V) for D3D12/DXIL, with no real benefit: Vulkan works on all Windows graphics cards
(NVIDIA, AMD, Intel), and performance depends on the translation, not the API. A "complete" reverse
engineering effort (recreating the game renderer in DirectX, without GPU translation) would be another project
taking many months. The productive path is to keep improving the current translation.
