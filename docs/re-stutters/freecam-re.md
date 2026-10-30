# Free-cam reverse engineering (SotC Lab, Phase A)

Scope: find the GameCamera update, the Wander (player) transform and physics step, and what drives area
streaming. Target: EU 1.01 eboot (`out/eboot.elf`, fingerprint `72c1d4a9...bb225`); addresses are eboot
VAs (image offsets, base 0). Labels: **observed** (decompilation or disassembly read), **inferred**,
**unknown**. Tools: `tools/re/eboot_re.py` (`str`, `callers`, `callees`, `fstr`, `refs`, `sig`,
`sigfind`), pseudocode under `out/re/decomp/<hi>/<addr>.c` (grep it).

## 1. Summary

- **observed:** the engine already has a debug camera and position console commands. This is the
  strongest lead for Phase 0 (command executor): see section 5.
- **observed:** the player transform is reachable through the printPlayerPos and setPlayerPos
  commands. Its position and rotation are pointers in a transform object (section 2).
- **not established:** the per-frame GameCamera pose (its position and orientation fields), the
  Wander physics step (gravity and collision), and the vec3 that drives area streaming. Their offsets
  stay `kUnknown` in `gpu/sotc/lab_layout.h`, so `SotcFreecam::Enabled()` is false.
- The hooks that exist are observe-only (`PLAYER_XFORM`) or the camera request anchor (`FC_CAM`, which
  writes nothing until the camera offsets are known).

## 2. Player (Wander) transform

- **observed:** `printPlayerPos` handler is `sub_0x330`. It calls `thunk_FUN_007c9820(&DAT_0325e0a8)`,
  which resolves the player object (`sub_0x7c9820`, 12-byte unique signature `55 48 89 e5 41 56 53 31 f6
  31 db e8`). It then calls `sub_0xb0` with the object and prints "Player Position: SetPlayerPos %.2f ...".
- **observed:** `sub_0xb0` (28-byte unique signature `55 48 89 e5 41 57 41 56 41 54 53 48 81 ec 80 00 00 00
  4c 8b 25 ?? ?? ?? ?? 48 8b 0d`) calls `sub_0x660fc0(obj)` (a type switch on the byte at obj+0xc),
  reads `T+0x150` (pointer to position float[3]) and `T+0x160` (pointer to rotation float[4]), and
  builds a 3x3 matrix from the quaternion.
- **observed, type switch `sub_0x660fc0`:** type 9, 13, 22 gives `obj+0x120`; type 10 gives `obj+0x40`;
  type 11 gives `sub_0x682360()+0xd0`; type 17 gives `sub_0x5915d0(sub_0x591440())`; type 19 gives
  `sub_0x664b50()`. Wander's type is **unknown**.
- **inferred:** `setPlayerPos` (`sub_0x570`, 28-byte unique signature) writes the same chain, through
  helpers `sub_0x1c1540` and `sub_0x216480`. Not confirmed at run time.
- Confidence: chain structure **observed**; that `DAT_0325e0a8` is Wander: **inferred** (name of the
  command and the position print); the write path through `sub_0x1c1540`: **unknown**.

## 3. GameCamera

- **observed:** the update thread (`ShadowGameUpdate_ThreadFunc`, 0x534690) requests the GameCamera
  resource at call site 0x535310 (the AudioListener request is at 0x5357bd): `sub_0x4948b0(holder, "GameCamera", fn, 1, &buf)`
  (decomp line 520 of `out/re/decomp/0053/00534690.c`). The holder is `param_1[0x795]` of the update
  object; the resource's file is `$/scripts/shadow/entities/camera/****/camera.cgpr`.
- **observed:** `sub_0x4948b0` (32-byte unique signature, see `scripts/hooks.d/freecam.py`) stores the
  resource pointer at `*holder`, so the camera object is `*(holder)`.
- **observed, not the camera:** `sub_0x79d1e0` prints "Camera Common Name" / "Camera Normal Name" but
  its body is a frontend routine (cursor and cancel handling). **Rejected** as the camera update.
- **unknown:** the camera object's position and orientation field offsets, and which function writes
  them per frame. `RenderView::MainCamera` (0xcaed0) is the render side; its body was not read to the
  end. The GPU view block keeps the camera position at +0x230 (`vk_dlssg.cpp:291`), which is a GPU
  copy, not a guest offset.
- Confidence: request site and holder chain **observed**; that `*holder` is the camera object
  **inferred** from the store in `sub_0x4948b0`; offsets **unknown**.

## 4. Physics step and streaming

- **observed:** `sub_0x79f3a0` contains "Wander_Vel" and "Agro_Vel" strings, but it is an AI routine
  (reports2/07). It is **not** the player's gravity or collision step.
- **unknown:** the Wander physics step. Candidates would be functions that take the player object and
  write its position with a gravity term. Not found in this pass.
- **unknown:** the streaming vec3. `AreaLoad_PrecacheLists` (0x533100) and `sub_0x343890` (area state
  setter) are hooked by `scripts/hooks.d/area.py`, but their position argument was not identified.
  Whether streaming reads the camera or Wander position is **unknown**.

## 5. Built-in debug camera and console commands (lead for Phase 0)

- **observed strings and registrations:** `ToggleDebugCamera` (`sub_0xa90`, 0xf91cb4), `enableDebugCamera`
  (0xfc4468), `SetCameraPos` (0xfc37e9), `PrintCameraPos` (0xfc3835, handler
  `GameCoreLoop::PrintCameraPos_command`), `setCameraPos %.2f %.2f %.2f %.2f %.2f %.2f` (0xfc4036, used
  by `sub_0x25a4d0`), `printPlayerAndCameraPos` (0xf917c0), "Set Player Position to Camera Position"
  (0xf91941), `setPlayerPos` (sub_0x1a20 registrations), and the debug camera class
  `CCPLdrCameraDebugCamera` (0xfbf22d).
- **observed:** `sub_0xa90` calls the console executor `sub_0x217f0(DAT_02dfcbc8, "ToggleDebugCamera")`.
  `sub_0x217f0` looks up a command by name (`sub_0x21620`) and calls its virtual slot at +0x20. So
  **the console executor is `sub_0x217f0(console, const char* command)`** (inferred from the pattern; it
  is also called from `sub_0x574f00` and `sub_0x689fe0`). This is the entry point Phase 0 needs.
- **inferred:** running `ToggleDebugCamera` through this executor would give a built-in free camera
  that moves the game camera. Whether the area streaming follows it, and whether the debug camera
  moves Wander, is **unknown**. Test in game before relying on it.

## 6. Signatures and hooks

| Name | Address | Bytes | Unique | Use |
|---|---|---|---|---|
| `PLAYER_XFORM` | 0xb0 | 28 | yes (`eboot_re.py sigfind`) | observe only, `SOTC_CAM_PROBE` |
| `FC_CAM` | 0x4948b0 | 32 | yes | wrap, `SOTC_FREECAM`, writes nothing until offsets known |
| `sub_0x7c9820` (player getter) | 0x7c9820 | 12 | yes | reference only |
| `sub_0x570` (setPlayerPos) | 0x570 | 28 | yes | reference only |
| console executor `sub_0x217f0` | 0x217f0 | | **no** (2 matches in 64 bytes) | needs a longer signature for any hook |

`tools/re/hooks_check.py` resolves 22/22 hooks, including PLAYER_XFORM at 0xb0 and FC_CAM at 0x4948b0.
`python -m unittest discover -s tests -p test_hooks.py` passes (29 tests).

## 7. Offsets and what is kUnknown

- `kCamPos`, `kCamRot`: **unknown**. Therefore `SotcFreecam::Enabled()` is false for every build.
- `kPlayerPos`, `kPlayerVel`, `kPlayerHidden`: **unknown**.
- `kXformPosPtrOffset` (0x150), `kXformRotPtrOffset` (0x160), `kPlayerXformTypeOffset` (0xc): **observed**
  in `sub_0xb0` and `sub_0x660fc0`; not used by the freecam path yet.

## 8. Not done / next steps

1. Find the camera object's position field: step through `sub_0x4948b0`'s post state in game (a probe
   that logs `*holder` and reads candidate floats against `SOTC CAM` in `vk_dlssg.cpp:481`).
2. Identify the Wander physics function (callers of the player transform writes, or a probe on
   `PLAYER_XFORM` that logs callers of `sub_0xb0` during play).
3. Test `ToggleDebugCamera` via the console executor in game, and check whether streaming follows it.

## 9. Integration needed elsewhere (owner: coordinator)

- `gpu/sotc/hook_handlers.cpp`: `#include "freecam.h"` and call `SotcFreecam::Register();` in
  `RegisterAllHandlers()`. It also registers the probe, so do not call `SotcProbes::RegisterCamProbe()`
  separately.
- `gpu/shim/bbport_overlay.cpp` (F3): `SotcFreecam::SetActive(!SotcFreecam::Active());`. Mouse motion and
  wheel while active: `SotcFreecam::FeedMouse(dx, dy, wheel)`.
- `src/runtime_pad.c`: when `bbgpu_freecam_captures_input()` is non-zero, call
  `bbgpu_freecam_feed_pad(lx, ly, rx, ry, l2, r2, buttons)` with the host pad and give the game a
  neutral pad (next to the existing `bbgpu_overlay_captures_input()` checks).
- `CMakeLists.txt`: nothing; `gpu/sotc/*.cpp` is globbed. `freecam.cpp` includes `../shim/bbport_overlay.h`
  and `<SDL3/SDL.h>`, which the GPU library already uses.
- `bbport_overlay.h` declares `BbOverlay::LoadingShown()`; the definition must exist in the overlay
  implementation (it was not checked here).
