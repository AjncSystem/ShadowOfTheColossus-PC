# Changelog

## 0.58-rc1 — 2026-10-15 (AMD preview; testing required)
- Repeated Vulkan image property queries are now memoized by the combination of format, type,
  tiling, usage, and flags. This addresses a report of AMD stutter caused by slow texture
  creation calls; it has not been measured on a Radeon and does not promise gains without testing.
- `SOTC_WAVE64_UNIFORM=0/1` is now part of the shader profile, and each mode uses an isolated
  pipeline cache variant, preventing reuse of shaders compiled with the other setting.
- **Pending:** test colors, shadows, stutter, and stability on AMD. The RDNA3 visual corruption
  in [issue #43](https://github.com/AjncSystem/ShadowOfTheColossus-PC/issues/43) remains without
  a confirmed fix.

## 0.57 — 2026-10-14
- **AMD cards: green/pink color fix** in the fog, Wander, and Agro (issues #6, #7, #9, possibly
  #10). AMD reports groups of 64 threads, but only allows that size to be fixed in compute
  shaders; the port required 64 in every stage and failed to convert compute shaders. AMD cards
  now use the same 32-thread path as NVIDIA (compute fixed at 32, shared-memory barriers, without
  the uniform-branch optimization that made characters black in shadow). Diagnosis by
  raulbezerro-dev in issue #7. Nothing changes on NVIDIA.
  **Not yet tested on an AMD card:** report in the issue whether it helped.
- **New launcher option:** "Full water reflections (direct memory access)," enabled by default.
  When disabled, it gains ~10% FPS (title-screen demo: 54 → 59), but distant water looks smoother.
  Each mode has its own shader cache folder.
- The shader cache is rebuilt on first launch.

## 0.56 — 2026-10-13
- **Open-world water with waves:** lakes and rivers below waterfalls were smooth as mirrors,
  without movement. This water's shader stores the active-thread mask with `s_wqm_b64 vcc, exec`
  and then uses it to decide whether to apply the flow map (the waves). The recompiler treated
  `S_WQM_B64` as "do nothing," leaving garbage in VCC and causing the shader to always write a
  straight-up normal. The instruction now does what the hardware does: expands the mask to full
  2×2-pixel quads. A simple copy fixed the water but left "colored garbage" in distant treetops,
  which use the expansion for the transparency test. Details in `docs/sotc/AGUA.md`.
- **Direct memory access (DMA) enabled by default,** as in the Pink-shadPS4 configuration for
  SotC. Reads from tables with shader-calculated indices (the *probe* records used by screen-space
  reflections) always read position 0; they now read the correct address. As a result, the upper
  lake ripples and reflects far into the distance. `BB_DIRECT_MEMORY_ACCESS=0` disables it.
- The shader cache is rebuilt on first launch (the first load stutters).

## 0.0.13 — 2026-10-13
- **0.0.12 crash fix:** the game could close at startup or in the menu (failure at `0x1a371`,
  1 in 3 tests). Cause: the port marked every seen texture as "still in use" for ~3 seconds, and
  the game tried to load more textures than its pool could hold. Now only textures used since the
  last request count (as in Pink-shadPS4). Tested: 0 crashes in 5 startup runs and a nearly
  40-minute session walking around the map, with two save loads.
- **Much larger water reflections:** the lake now reflects cliffs, temples, and vegetation over
  nearly its entire surface, with ripples. A shader loop safety limit was cutting off screen-space
  reflection rays; the limit is now per loop.
- Under investigation: where the water should reflect the sky, it is still dark and smooth.

## 0.0.12 — 2026-10-13
- **Sharp textures restored without game crashes:** the memory corruption that made the game
  close shortly after loading a save (failure at `0x1a371`) and left textures with "colored
  garbage" came from the emulator copying GPU data into memory the game was freeing. This is now
  fixed, and texture streaming is back on at level 2 (2048 to 512×512 textures): much sharper
  rocks and walls. Tested for more than 25 minutes near the waterfall, without a crash.
- **Water:** splashes, rings, and foam around Wander while swimming; the lake reflects the sky and
  sunlight. Environment reflections (local IBL pass) now use the correct *cubemap* for each
  *probe* — a fix brought over from Pink-shadPS4 (`dynamic_tsharp_array_size`).
- Under investigation: water near the camera still has no ripples.
- Options for testing: `SOTC_LOD_STATS=0` (streaming disabled), `SOTC_LOD_LEVEL`,
  `SOTC_UNMAP_READBACK=1`, `SOTC_TSHARP_ARRAY=0`.

## 0.0.11 — 2026-10-13
- **0.0.10 fix — grass with "colored garbage" texture:** at streaming level 3, the game freed
  mips still in use by the GPU (seen in the grass near the waterfall). Texture streaming is now
  **disabled**, as in 0.0.9: normal grass, visible waterfall curtains, and no crashes when loading
  a save. To test streaming: `SOTC_LOD_STATS=1` (with `SOTC_LOD_LEVEL=2`, it looks sharper but
  may crash the game).
- Under investigation: still water (mirror-like lake, no waves) and waterfall spray.

## 0.0.10 — 2026-10-13
- **Saving fixed:** the game could never save ("not enough free space"); it now saves.
- **On-screen save list:** "Load Game" and "Manual Save" display saves (region, play time, date)
  to choose from, with "+ New Save" when saving. Use arrows/directional controls to choose,
  Enter/X to confirm, Esc/O to go back, or double-click with the mouse.
- **Delete save:** in the list, press Delete (keyboard) or Triangle/Y (controller), then confirm.
- **High-resolution textures (streaming):** the port now responds to the GPU texture statistics
  command (`GET_LOD_STATS`) that the game uses to decide what to load. At level 2, 2048 textures
  reached 512×512 and waterfall curtains became visible.
- **Game crashing after loading a save:** level 2 streaming corrupted game memory (3 crashes in
  4 tests near the waterfall). The default is now level 3, which passed 4 of 4 tests without a
  crash; 2048 textures are 256×256 at this level, as before streaming (waterfall curtains have
  not yet been checked at this level). `SOTC_LOD_LEVEL=2` restores the sharper level, with a risk
  of crashing.
- **Black shadows fixed:** grass, rocks, and walls in shadow were black. Three shader recompiler
  fixes brought over from Pink-shadPS4 (ignored 1.0 fetch shader constants, `V_ALIGNBIT`/
  `V_ALIGNBYTE` with offset 0, `V_CVT_PK_U8_F32`).
- **GPU particles:** shared-memory barriers in groups smaller than a wave; a particle system that
  never appeared is back. Waterfall spray and foam still do not appear (under investigation).
- The launcher writes the game log to `user/last_run.log`, with the exit code.
- Diagnostic tools: F12 captures a frame when the game runs under RenderDoc.
- New icon (colossus face).
- **Warning:** if switching saves in the same session closes the game, restart before loading
  another one.
- Full project review in `docs/sotc/REVISAO-DO-PROJETO.md`.

## 0.0.9 — 2026-10-12
- **Automatic first-time setup:** the first time, `SotC.exe` opens to a welcome screen that only
  asks for the game folder (or offers one it found on the PC). It then sets up DLCs (inside and
  next to the game folder), saves (copies a shadPS4 save if the port does not already have one),
  and audio, and shows the results. **Configure again** repeats the process.
- **Main folder, like shadPS4:** select the game's main folder and it finds the base game
  subfolders (CUSA08809), update, and DLCs.
- **Update:** the `CUSA08809-UPDATE` or `CUSA08809-patch` folder next to the game is used as in
  shadPS4: its files take priority over the base game (`SOTC_UPDATE_DIR`). If it contains
  `eboot.bin`, that is the one that runs.
- Cleaner main screen: the game log is now a discreet link; the check shows the DLCs.
- **DLC folder** in the launcher (Game and saves), as in shadPS4. It is also found by automatic
  search: the shadPS4 `addcont` folder or a folder with one subfolder per DLC.
- The runtime lists installed DLCs for the game (`sceAppContentGetAddcontInfoList`), reading each
  one's `CONTENT_ID` from `param.sfo` (`SOTC_DLC_DIR`).
- **DLC in .pkg without installing:** DLC packages (type `0x1b`) are read directly from the
  header, without extraction. This is enough for the game's DLCs, which only unlock items (ancient
  bow, cloak, sword, horses). Search checks the selected folder, the game folder, and nearby
  folders.
- **PS4 Pro mode:** the game log shows whether it was launched as a PS4 Pro
  (`sceKernelIsNeoMode -> 1`). This option only enables PS4 Pro Graphics Mode: you must select
  **Cinematic** or **Performance** in the game under Options > Presentation.

## 0.0.8 — 2026-10-12

First public release.

### Launcher (`SotC.exe`)
- **Automatic search** on launch: finds the game (CUSA08809) on PC drives, the user's audio
  modules in an installed shadPS4 (copied to the port's data folder), and shadPS4 saves (asks
  before importing). **Search PC** repeats the search.
- **Game library** like shadPS4: game folders, list with icon, title, ID, and version.
- **Key remapping:** click an action and press the new key (saved as `key.<entrada>=` in
  `sotc.ini`).
- **New icon** (original artwork). The window and desktop shortcut use `icon0.png` from your
  dump.
- **GPU work submission:** Automatic (NVIDIA), Enabled on any card (AMD/Intel, experimental), or
  Disabled.

### Performance
Title-screen demo, 60 FPS mode, i9-13900K + RTX 2060 SUPER: average 37–45 → **~67 FPS**,
heaviest segment ~17 → **29–34 FPS**.
- Vulkan commands recorded and submitted on a dedicated thread (`cp_record_thread` from
  Pink-shadPS4).
- Periodic GPU work submission by time (every 0.5 ms), rather than by command count.
- Indirect draws with CPU arguments become direct draws.
- Caches: image descriptions by T#, image lookups, shader binary information, and per-thread
  memory pointer.
- Buffer synchronization check without a lock when nothing changed; `RangeSet::Add` does not
  reinsert already marked ranges.
- Write faults: the faulting page and following pages, without GPU data, are freed together
  (~50 thousand → ~4 thousand faults per second).
- Readbacks:
  - recently read ranges are downloaded together;
  - "clean" reads from shader resource tables, with the bitmap tracking actual page protection;
  - descriptor table filling (compute `0x50784801`) is done on the CPU;
  - prefetching.
- From Pink-shadPS4: cvars `doIndexBufferCulling=0 doSunShadowIndexBufferCulling=0` and BPE
  heap protection against GPU writes.
- Optional, for testing: asynchronous readback (`SOTC_RB_ASYNC=1`) and wait marker
  (`SOTC_WAIT_MARKER=2`).

### Fixes
- GPU queue stall with recording on a dedicated thread: presentation waits for the frame to reach
  the queue.
- Silent crash due to page protection on a fiber stack.
- Black screen on boot; PS4 Pro mode crash (`alt_tile` in video buffers).

## Earlier (development, 07–08/10/2026)
- Game boot in the bbport loader (NID imports, fibers, SSE4a, image below 2 GiB).
- Video core from the SotC shadPS4 fork; audio using the user's modules.
- 60 FPS and 16x AF modes as loader patches; frozen Tkinter launcher (`SotC.exe`).
- Keyboard and controller together, mouse camera, 720p–4K output, PS4 Pro mode, engine effects.

