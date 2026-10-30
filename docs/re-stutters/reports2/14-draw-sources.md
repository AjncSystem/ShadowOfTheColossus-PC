# 14 - Draw sources: which engine systems emit the 4,000-8,000 draws per heavy frame

Read-only. Static analysis of `out/eboot.elf` (`tools/re/eboot_re.py`, `out/re/decomp`), the port
source (`gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp`, `sotc_adaptive.h`), and
`launcher/sotc_launcher_win.py`. Reports 01, 04, 06 and 07 were read first. No game run, no build.

Labels: **observed** (disassembly, pseudocode, string, or repo code/comment, with address or
line), **inferred**, **unknown**. No per-system draw counts exist in the logs in the scratchpad,
so every "how many draws" figure below is unknown unless labelled otherwise.

## 1. Answer

- **Observed (port comment, vk_rasterizer.cpp:881-882):** a SotC frame has about 6,500 draws into
  the sun shadow map (5 slices, 1024x1024). Slice 3 had 3,041 and slice 2 had 2,049 in one
  `SOTC_PASS_TRACE` run. The log is not in the repo. So the sun shadow cascades are roughly 78%
  of the draws in that frame, and the far cascades (slices 2 and 3) are about 5,090 of them.
- **Observed:** all sun cascades are emitted by one game function, `RenderShadows` (`0xcc9b0`,
  called from `RenderView::MainCamera` at `0xcb04c`). It loops over five cascades. Each cascade
  runs `RenderShadowCascade` as a render job (`RenderThread::RunFunctionAsRenderJob`, `0xc12a0`),
  whose body is `0xcdbc0` and then `0xce850`.
- **Observed:** the per-cascade work in `0xce850` is, in order: object passes (`0x11d3d0`, passes
  0, 1 and 3), two XPR masked-draw lists (`0x11d7a0`, skipped for cascade index 4, the "Super
  Close" one), virtual geometry (`0xdb700`, gated by the cascade index), and particle models
  (`0x10b060`, gated by a flag). **Fur is not called from the cascade path.**
- **Observed:** the virtual geometry (grass) shadow draws are gated by
  `cascade index < [cfg+0x49c]`. This is the cvar `r_virtualGeometryNumShadowCascades` (the
  mapping from cvar to field is **inferred**). The launcher already exposes it (1 / 2 / 3, default 2).
- **Observed:** `doIndexBufferCulling=0` and `doSunShadowIndexBufferCulling=0` are the launcher's
  default (`perf_culling`). They remove GPU cull dispatches and index work. They do **not**
  remove draw commands, so turning them on is not a draw-count lever.
- **Best known lever, and the most visible one:** the far cascades. The port already has
  `SOTC_SHADOW_REUSE` and `SOTC_SHADOW_STAGGER` (default off). The stagger blinks with a still
  camera (15 Hz, 0.36% of pixels per frame, per the code comment). Any new version of this has to
  fix that blink first. An engine-level version would also skip the game's own emission and the
  XPR consumer for those cascades, not only the port's draw, so its gain is larger.
- **Smallest visible change:** `r_virtualGeometryNumShadowCascades=1` (grass shadows only in the
  nearest cascade, about 3 m). Gain is unknown, and it already exists in the launcher.
- **Not located:** spot shadows (`RenderShadowSpot 0-3`), terrain, foliage, UI and water draws.
  Their names are in data tables, and the code that reads those tables was not found.
- **Measure first:** the draws emitted by each game render job are not counted anywhere. Hook
  proposal H1 (tag each draw with the current render-job name) would give the per-system split.

## 2. Evidence

### 2.1 Frame placement and the cascade loop

- **Observed**, `RenderView::MainCamera` calls `RenderShadows` at `0xcb04c` (`eboot_re.py callers`).
- **Observed**, `RenderShadows` `0xcc9b0` (pseudocode `out/re/decomp/000c/000cc9b0.c`):
  - It runs only when a view flag is set and `[param_4[9]+0x188] == 0`.
  - It calls `FUN_00092140` (view constants), then `FUN_000932a0` (clear or setup).
  - The loop runs five times (cascade names `RenderShadowCascade 0..4 (Super Close)`, strings
    `0xfa139d`-`0xfa13f5`). Each pass calls `RunFunctionAsRenderJob` (`0xc12a0`) with
    `RenderShadowCascade` (string `0xfa1436`, user `RenderShadows`, from `refs`).
  - For cascades other than index 0, it also queues `IndexBufferCullShadowCascade` (string
    `0xfa1419`, job body `0xcdb00`) when a per-cascade flag in `param_4[7]` is set.
  - After the loop, five pairs of `0x869f50` and `0x93d70` run (at `param_4[3]+0x2120`,
    `+0x2128`, `+0x2130`, `+0x2138`, `+0x2140`). This is the spot-shadow or resolve section.
    **Unknown:** which of these is spot shadows.
- **Observed**, the cascade object index is `param_1[0x11]`. The loop sets it to `lVar5 + 4`,
  so the indices are 4 (Super Close) down to 0.

### 2.2 What one cascade emits (`0xce850`, 648 bytes)

Observed callees (`eboot_re.py callees`) and pseudocode:

| Call | Meaning (name source) | Gate | Draws |
|---|---|---|---|
| `0xbe1f0` SetViewParams | view state | always | none |
| `0x11d3d0` x3, passes 0, 1 and 3 | object passes (name **unknown**) | always | yes, the main object set |
| `0x11d7a0` x2 (`index` and `index+4`) | `BPXPR_RecordMaskedDraws` (`0x11d7a0`) | index != 4 | yes, XPR visible objects (`ConsumeVisibilityMask` `0x1a6d20` emits one draw per visible object, report 01) |
| `0xdb700` | **`RenderVirtualGeometry`** (scope name `"RenderVirtualGeometry"`, `"VirtualGeo"`) | `index < [param_1[9]+0x49c]` | yes: per object `0xdb390`, which calls the draw wrapper `0xdb620` |
| `0x10b060` | **`ParticleRenderer::RenderModels`** (string in `0x10aef0`) | `[[ctx+0x40]+0xdc0] != 0` | yes, particle meshes (**inferred** from the callee chain `0x10b060` to `0x10aef0` to `0x10b0a0` to `0xdb620`) |
| `0x8f410` x3 | render target or setup | always | none |

- **Observed:** the cascade code does not call `FurRendering::*` (fur appears only in
  `RenderView::MainCamera` (`0xc2650`) and `BPRender_DepthAndGBufferPasses` (`0xc2750`), report 04).
- **Observed:** `0xdb700` is also called from `0xc1860`, `0xc1fc0` and `0xc46c0` (main view).
  Virtual geometry therefore draws in the main view too, through the same function.

### 2.3 Draw wrappers and their callers

- **Observed** (report 06 said the wrappers have no direct callers): the wrappers `0x86a9d0`,
  `0x86aab0`, `0x86abb0`, `0x86ac10`, `0x86adc0`, `0x86ae30`, `0x86afd0`, `0x86b040`, `0x86b340`,
  `0x86b3d0`, `0x86b470`, `0x86b510` are called from only 31 decompiled functions (direct sites).
  Most draws therefore go through indirect calls.
- **Observed**, direct draw emitters found: `0x93690` (called by `0x93960`, `0x93ef0`, `0x94600`),
  `0x94d00` (by `0x128d20`), `0x94ec0` (by about 12 passes, including `0xb6e20` DOF, `0xb9710`
  (5 sites) and `0xb0570`, a post or water helper, **inferred**), `0x133550`/`0x133a50`/`0x133bd0`
  (by `0x187ce0`, `0x12a930`), `0x1a6d20` and `0x1a8c60` (XPR consumers), `0xdb620` (by `0xdb390`,
  `0x10b390`, `0x189060`, `0x18d6f0`).
- **Observed**, the `0x1a6d20` family is the XPR visibility-mask consumer. It emits one
  `sceGnmDrawIndexIndirect` per visible object (report 01, `0x86b3d0` at `0x1a716b`, `0x1a71b2`).
  The masked draws in cascades therefore scale with the visible shadow casters.

### 2.4 Pass names, and where they are referenced

| System | Pass or job name (string address) | Code located | Notes |
|---|---|---|---|
| Sun shadows, cascades 0-4 | `RenderShadowCascade 0..4` (`0xfa139d`-`0xfa13f5`) | `RenderShadows` `0xcc9b0` | observed |
| Cascade cull (compute) | `CullSunShadowCascade 0..4` (`0xfa0fd2`-`0xfa102e`) | pointer table `0x159b930` | compute, report 04 |
| Shadow index cull (compute) | `IndexBufferCullShadowCascade` (`0xfa1419`) | `0xcdb00` (`RenderShadows`) | observed |
| Spot shadows | `RenderShadowSpot 0..3` (`0xfa129f`-`0xfa12d8`) | pointer table `0x159b9c0` only | **unknown function** |
| Spot cull | `CullSpotShadow 0..3` (`0xfa1053`-`0xfa1086`) | pointer table `0x159b960` only | **unknown function** |
| Virtual geometry (grass) | `VirtualGeometryRendering::PreRenderJob` (`0xfa0d9f`), `RenderVirtualGeometry` | `0xdb700` | observed |
| Fur | `FurRendering::UpdateJob`, `PreRenderJob`, `PostRenderJob` (`0xfa0dc6`-`0xfa0df9`) | `0xc2560`, `0xc2650`, `0xc2750` | main view only |
| Particles | `ParticleRenderer::RenderTranslucent*`, `RenderDistortion`, `RenderFog`, `RenderDisplacement`, `RenderModels` | `0x10a7a0`-`0x10b0a0` | per-frame, one or more jobs |
| Decals | `RenderDecal` (`0xfa0f09`), `XPRDecalCull` (`0xfa0ed9`), `XPR_ProjectedDecalCull` (`0xfa0ee6`) | `0xc59d0` (depth and G-buffer), `0xc5200`, `0xc5300` | depth and G-buffer pass |
| Water | `WaterRendering::UpdateJob` (`0xfa14d0`), `ResetJob` (`0xfa15c2`) | `EndFrameViewParams` (`0xcefa0`) | draws not located |
| Fog | `FogRendering::UpdateJob` (`0xfa11bf`) | `RenderView::MainCamera` | not a draw source found |
| Specular IBL | `RenderSpecularIBL` (`0xfa0e7b`) | `0xc4210` | post |
| Terrain, foliage, UI | none found by string | - | **unknown** |

- **Observed**, the "Foliage" strings are `FoliageDisplacement` and
  `CCPLdrShadowFoliageDisplacement` (`0x100eb6f`, `0x100ecab`): gameplay displacement, not draws.

### 2.5 Grass, per the port

- **Observed** (vk_rasterizer.cpp:1432-1446): the grass is generated by compute patches
  (`kGrassPatchHash = 0x6172cbb0`). Blades are not individual draws. The patch count per frame
  follows the view distance (`SOTC_GRASS` distance multiplier), and the blade count per patch
  follows the LOD scale (`kGrassPatchLodDword = 30`). So the grass draw count follows the
  number of patches in view, not the number of blades.
- **Observed** (launcher comment, `GRASS_SHADOWS`): the cascade count of r_virtualGeometry sets
  the shadow reach: 1 = about 3 m, 2 = about 8 m (PS4), 3 = about 24 m. This matches the gate
  `index < N` in `0xce850`, with N = 2 covering cascades 0 and 1.

### 2.6 Cvar registration

- **Observed**, `RegisterXprCvars` (`0x6efe0`; the decompiler timed out): the strings for the
  shadow and grass cvars are `r_enableCascadeShadows` (`0xf9b505`), `sunShadowCascadeDistance0..4`
  (`0xf9b62a`-`0xf9b692`), `r_SunCascade0..3FadeScale` (`0xf9b4a9`-`0xf9b4ee`),
  `r_virtualGeometryNumShadowCascades` (`0xf9cb83`) and its `_Cinematic`, `_BasePS4`, `_Perf`
  variants (`0xf9cba6`-`0xf9cbfe`), `r_virtualGeometryFadeMultiplier` (`0xf9cb42`),
  `particleNumShadowCascades` (`0xf9af52`), `r_projectedDecalEnable` (`0xf9c845`).
- **Unknown:** the default values of these cvars. Report 01 did not decode them either.
  `particleNumShadowCascades` has no located reader, so its effect is **unknown**.

### 2.7 Port state

- `SOTC_SHADOW_REUSE` (`vk_rasterizer.cpp` `Rasterizer::ReuseShadowCascade`, around line 963)
  and `SOTC_SHADOW_STAGGER` (`SkipStaggeredShadow`, around line 886) are both off by default.
  Their comments record the blink: with a still camera, far shadows on the Shrine ground flicker
  at 15 Hz (0.36% of pixels per frame, against 0.01% off). The stagger is the simpler version, and
  it blinks too.
- `SOTC_ADAPTIVE_SHADOW` (`sotc_adaptive.h`) turns the reuse on only while the frame governor is
  relieved (below 27.8 FPS). It does not change the draw count when the game runs fast.
- The launcher's default for the cull dispatches is off (`perf_culling`), so the default run
  issues all cascade draws.

## 3. Hook proposals

Ranked by expected draws removed per heavy frame, with visual risk in mind. Gains are **unknown**
unless labelled. The draw-thread estimate assumes cost is linear in draws, which is **inferred**.

### H1. Tag draws with their render-job name (measure only, first)

- **Function:** `RenderThread::RunFunctionAsRenderJob`, `0xc12a0`.
  Signature (28 bytes, unique by `eboot_re.py sig`):
  `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 38 01 00 00 4c 8b 25 ?? ?? ?? ?? 41`.
- **Mode:** Wrap (entry and exit). The name is read from the job's `char*` argument, which the
  callers set from the string (e.g. `RenderShadowCascade` at `0xcc9b0`).
- **What it changes:** a thread-local tag. The port increments a per-tag draw counter in
  `Rasterizer::Draw`. Nothing else changes.
- **Why it helps:** it turns the 6,500 draws into a per-system split. Without it, every other
  proposal here is a guess.
- **What could break:** a job can nest another job. Use a stack, not one slot.
- **Verify:** `=2` mode runs the original and the tagged path, and checks that the tags sum to the
  `SOTC_PASS_TRACE` total.
- **Signature:** unique (above).

### H2. Far cascades on alternate frames, at the job level (highest gain, highest risk)

- **Function:** `RenderShadowCascade` body, `0xcdbc0`.
  Signature (28 bytes, unique): `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 78 4c 8b 25 ?? ?? ?? ?? 48 89 fb 4c`.
- **Mode:** Entry (Wrap). For cascade index 2 and 3 on odd frames, return without calling
  `0xce850`. The cascade's depth stays from the previous frame.
- **What it changes:** removes the object passes, the two XPR masked-draw lists and the
  virtual-geometry draws for those cascades on half the frames.
- **Draws removed:** about 2,545 per frame on average, about 39% of the 6,500 (**inferred** from the
  observed 3,041 and 2,049 per slice, averaged over frames). Draw-thread time: about 10 ms if linear
  (**inferred**). It also removes the game's own emission and the consumer `0x1a6d20` for those
  cascades, which the port-level skip (`SkipStaggeredShadow`) does not.
- **Visual risk: high.** The port's measured blink (15 Hz, still camera) came from this kind of
  skip, applied at the port's draw level. An engine-level skip has the same stale-texel problem.
  Far shadows of moving casters, including grass in wind, would lag one frame, and the blink is
  not yet explained.
- **Why it helps:** it shortens the draw-thread and command-processor work on those frames.
- **What could break:** the blink; the sun cascade stays valid only if the light matrix is
  unchanged (`vk_shadow_scroll.h` covers this for the port's reuse, not for the engine skip).
- **Verify:** `=2` mode, with the skipped cascade drawn into a scratch slice. Compare the two
  images per frame (texel change, count of changed pixels, and whether the blink appears at 15 Hz).
- **Signature:** unique (above). **Do not enable by default** without fixing the blink.

### H3. Particle models out of the far cascades

- **Function:** the call `call 0x10b060` inside `0xce850`, site `0xcea4e` (offset `0x1fe` from the
  function start). Function signature (24 bytes, unique): `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 48 89 fb 31 f6 c5 fa`.
- **Mode:** Site (redirect the one call). For cascade index 2 to 4, skip the call.
- **What it changes:** particle meshes stop casting into the far cascades. Near cascades and the
  main view are unchanged.
- **Draws removed:** unknown. Particle models are a small share in most areas (**inferred**).
- **Visual risk:** low to medium. Particle meshes lose far shadows (for example, smoke or
  debris). Check at the Shrine of Worship and in an area with many particle emitters.
- **What could break:** particle meshes that the game expects to be in the cascade's cull lists.
- **Verify:** `=2` mode with the call kept, comparing the particle-mesh draw count with H1 tags.
- **Signature:** the function signature is unique. The call-site offset is fixed for this eboot
  fingerprint only (the hook design allows a re-scan with the function signature).

### H4. Cvar: `r_virtualGeometryNumShadowCascades=1` (no code)

- **Where:** the launcher already maps it (`GRASS_SHADOWS`, "Near"). The cvar is read at `0xce850`
  (`index < [cfg+0x49c]`).
- **Draws removed:** the virtual-geometry shadow draws of cascade 1. Unknown count.
- **Visual risk:** low to medium. Grass shadows end at about 3 m, not 8 m.
- **Verify:** `SOTC_CVARS="r_virtualGeometryNumShadowCascades=1"`, with H1 tags on the
  `RenderVirtualGeometry` job.

### H5. Cvars for the far sun cascades (no code, unknown defaults)

- **Where:** `sunShadowCascadeDistance2` and `sunShadowCascadeDistance3`
  (`0xf9b65e`, `0xf9b678`). Defaults unknown.
- **Mode:** cvar. Lower the far distances, so the far cascades cover less area and fewer casters
  are visible.
- **Draws removed:** unknown. Roughly proportional to the area of the cascade if the masked lists
  follow the frustum (**inferred**).
- **Visual risk:** medium. Far shadows end closer to the camera (a visible pop line).
- **Verify:** H1 tags on the `RenderShadowCascade 2/3` jobs, before and after.

### H6. Not proposed

- `doIndexBufferCulling` and `doSunShadowIndexBufferCulling`: they cut index work and GPU
  dispatches, not draw commands. Keep the launcher default. Enabling them would not reduce the
  number of draws.
- `r_enableCascadeShadows=0`: removes all sun shadows (the whole 78%), with a clear visible loss.
  Not a candidate.
- `r_projectedDecalEnable=0`: removes decals, a visible change. Draw count unknown.
- Spot shadows, water, fur, UI: no located hook, and the draw counts are unknown. Revisit after H1.

## 4. Open questions

1. **Per-system draw counts.** H1 is the only way to get them. How many draws does each job emit
   in a heavy area? Is `0x11d3d0` (passes 0, 1, 3) the bulk of the 5,090 far-cascade draws, or
   the masked lists?
2. **Blink cause.** Is the 15 Hz blink in the port's stagger caused by the texel scroll, by
   casters that move, or by the cascade snapping? H2 needs that answer first.
3. **Spot shadows.** Which function reads the `RenderShadowSpot` name table at `0x159b9c0`?
   The five pairs after the cascade loop in `0xcc9b0` are the lead.
4. **Cvar defaults.** `RegisterXprCvars` (`0x6efe0`) is too large to decompile in one pass. Read
   the constructors of the shadow and VG cvars for their defaults, and the
   `particleNumShadowCascades` reader.
5. **Index-to-slice map.** Is cascade index `i` the same as port slice `i` (inferred from the
   `0xce850` gate)? Check with H1 and `SOTC_PASS_TRACE` together.
6. **Terrain, foliage, UI, water draws.** No function located by string. The UI path is likely
   separate from the scene (`0x94ec0` helper callers are a first lead).
7. **Update rates.** `RenderShadows` runs once per main-view frame (observed). Whether any cascade
   or VG job runs less often than every frame is **unknown**.
