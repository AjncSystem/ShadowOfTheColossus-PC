# 04 - Compute passes: which systems dispatch, and what could be skipped

Read-only study of `out/eboot.elf` (eboot_re.py, Ghidra pseudocode, `tools/re/names.txt`) and the
port's counters. Nothing was run and nothing was built. Addresses are eboot virtual addresses.
Builds on `06-render-frame.md` (frame phases). Labels: **observed**, **inferred**, **unknown**.

## 1. Answer

- **Dispatch wrappers have static callers.** Report 06 said the draw/dispatch wrappers have no
  visible callers. That holds for `CmdBuf::DispatchDirect` (`0x86b970`, indirect only), but not for
  the two families that do the work: family A `sub_883870` (36 direct caller functions) and family
  B `BPGnm_DispatchDirect` `0x86b9e0` (64 direct caller functions). **observed** (`eboot_re.py callers`).
- **Systems that issue dispatches, by their scope strings:** fog (`Fog`, `0xfa009d`), particles
  (`ParticleRenderer::UpdateSimulation`, sort, lightmaps, `BeginFrame`, prep indirect), fur
  (`UpdateFur`, `Physics`, `PreRenderFur`, `CullClumps`, `GenerateStrands`, `RenderFur`), virtual
  geometry and instances (`PreRenderVirtualGeometry`, `CullBatches`, `PrepPlacementArgs`,
  `PrepRenderArgs`, `CreateInstances`), XPR culls (`CullSunShadowCascade 0-4`, `CullSpotShadow 0-3`,
  `IndexCulling SunShadow0-3`, `XPRRunFragmentCullShader*`), water (`WaterSimulation`, `WaterReset`,
  `WaterRelocate`, `WaterQuery`), probes and lights (`CStaticLightDynamicProbeCache::UpdateProbes`,
  `AmbientShadowing`, `AmbientOcclusion`, `ResolveSunShadows`, `RenderSpecularIBL`), post
  (`PrepareMotionBlur`, `ComputeExposure`, `TemporalAA`, `ApplyDOF`, `PrepareBloom`, `BlurX/BlurY`),
  depth pyramids (`XPRCreateOccluderDepthMipMaps`, `CreateDepthTextureMips`, `CreateFogPerTileDepthTexture`),
  clusters (`CreateClusters`), and texture or instance uploads (`UpdateTextureStreamingGPUData`,
  `InstanceData_WriteStream`, `Update InstanceDynamicData`). **observed** (strings and call edges).
- **Per-frame dispatch counts: unknown.** Nothing in the session logs gives them. The port already
  has the tool that does: `SOTC_CMD_STATS=2` prints the most used compute shaders per window
  (`docs/sotc/README-SotC.md:152`). Mapping shader hashes to systems still needs the caller probe
  in section 3 (H1).
- **Cost per dispatch is small.** BIND-PATH §2 gives ~4.5 ms of bind for ~1,150 dispatches, so
  ~3.9 µs each. Removing a whole 50-dispatch system saves ~0.2 ms of bind, before its GPU cost.
  Pure dispatch-count cuts will not remove hitches. **inferred** (from the BIND-PATH numbers).
- **The hitch-relevant dispatches are the ones with a label wait.** `BPGnm_DispatchWithLabelWait`
  (`0x92b50`, unique signature) has 14 direct callers, and `sub_928f0` (a second label-wait wrapper)
  has 9 callers. Several are per-frame systems: `ParticleSystemPool::ProcessStats`,
  `UpdateTextureStreamingGPUData`, `InstanceData_WriteStream`, `Update InstanceDynamicData`,
  `BPXPR_IndexCullObjects`, and the cluster build (`sub_ab630`, label wait at `0xab746`).
  **observed** (call edges). Whether they block the CPU is **inferred**: the name and the
  `BPGnm_WriteCsDoneLabel` pairing (report 01) suggest a CPU wait, not measured.
- **Best cache candidates already exist as cvars.** `forceUseLastIndexBufferCullingResults`
  (`0xf9a971`) reuses last frame's index-cull result, which skips the cull dispatches on frames
  where it applies. `r_motionBlurEnable` (`0xf9c3b5`) removes the 5 `PrepareMotionBlur` call sites.
  Both change the picture. **observed** (names); effect **unknown**.
- **Some work is already cached or event-driven, so it needs no hook.** The BRDF LUT is built once
  (`sub_187510` gate `[obj+0x268] == 0`), the ambient-shadow LUT once (`sub_17cac0`, gate
  `plVar4 == 0`), and dynamic probe updates run only when a queue is non-empty
  (`sub_136b30`, queue at `DAT_02ee1500..1508`). **observed**.
- **Ranked proposals:** (1) defer or merge the label-wait dispatches (H2, H3), which is the same
  family as the readback topic; (2) measure before cutting: caller probe (H1); (3) cull reuse
  (cvar, section 3 L1); (4) cluster build cache (H5); (5) fog and water sub-rate (H6, H7), which
  need visual checks. Gain estimates are **unknown** until the caller probe gives counts.

## 2. Evidence

### 2.1 Wrapper families and their direct callers (observed)

| Wrapper | Address | Import site | Direct callers (static) |
|---|---|---|---|
| Family A: `sub_883800`, `sub_883870` | `0x883870` | `0x8838d0` (`sceGnmDispatchDirect`) | 36 functions: `a1e10`, `a7310`, `a7470`, `a75b0`, `a78a0`, `a7eb0`, `a83f0`, `ab430`, `afcc0`, `b50c0`, `d83b0`, `dc740`, `dcf40`, `e60c0`, `e69e0`, `e7390`, `e7b60`, `103ad0` (ParticleRenderer::UpdateSimulation), `10bfd0`, `10c140`, `10c8c0`, `10cdb0`, `10cf60`, `10db10`, `1375e0`, `1379e0`, `16eb90`, `17e6b0`, `17ecf0`, `18a2c0`, `1907f0`, `191a90`, `193810`, `1a5310`, `1a85f0`, `1acdc0`, `1adc90` |
| Family B: `BPGnm_DispatchDirect` | `0x86b9e0` | `0x86ba40` | 64 functions, including `928f0`, `a20b0` (scene view), `a8c50`/`a8e10`/`a8f80` (fog wrappers), `b3590` (exposure), `b5b10` (motion blur), `b6760` (TAA), `b6e20` (DOF), `ba640` (depth mips), `bae70` (depth set), `bb310` (fog depth), `e6350`/`e84b0`/`e8e80`/`e9670` (water), `1051f0` (particles), `17c6c0`/`17cac0`/`17daa0` (AO), `17e000` (blur), `17ecf0`-family (bloom), `186df0` (IBL), `187510` (BRDF), `194a00` (source indices), `198bb0` (called from the frame job `d0040`), `19c7e0` (decal cull) and more |
| Label-wait dispatch | `0x92b50` (`BPGnm_DispatchWithLabelWait`, unique 20-byte signature) | - | 14 functions: `ab630`, `bdfd0`, `SetViewParams`, `d8660`, `dfcf0`, `InstanceDynamicData_Update`, `BPXPR_IndexCullObjects`, `18b5b0`, `18b6f0`, `18cf40`, `18e2c0`, `18ed40`, `1a42e0`, `1ac530`; and `sub_928f0` (family B) has 9 callers: `94320`, `95420`, `ParticleSystemPool::ProcessStats` (`1018c0`), `CRenderer::UpdateTextureStreamingGPUData` (`117930`), `InstanceData_WriteStream` (`123410`, 2 sites), `189000`, `18a000`, `18f830` |
| Indirect only | `0x86b970` (`CmdBuf::DispatchDirect`), `0x86ba60`/`0x86bac0` (DispatchIndirect), `0x8838f0`/`0x883950` (DispatchIndirectOnMec) | - | none found statically |

Each direct caller has one call site. The number of calls per frame depends on the caller's loops,
which were not counted. **unknown** per frame.

Two wrappers with the same name exist: `ParticleRenderer::UpdateSimulation` at `0x103ad0`
(family A) and `0x1051f0` (family B). Both carry the same scope string. **observed**. Why there are
two is **unknown**.

### 2.2 Systems by scope string (observed scope names; call edges as in the walk)

Scope strings are pushed by `FUN_0016b8c0(obj, "<name>", ...)`. The name at each address was
read from the eboot data (`fstr`/`str`). The call edges come from `eboot_re.py callers`.

**Fog (scope "Fog", string `0xfa009d`).** `sub_a5e80` pushes "Fog". It calls `a7310`, `a7470`,
`a75b0`, `a78a0` (twice), `a7eb0` and `a83f0`, which are family A wrappers. It is called through
`sub_a6260`, which also calls family B wrappers `a8c50`, `a8e10`, `a8f80`. `a20b0` (family B) calls
`sub_a6260` in the scene view, and `sub_186df0` (IBL) calls `a20b0` too. Each call to `a7eb0`
issues one dispatch (observed: one call to `FUN_00883870` at line 247, with the slice count as the
third group dimension, `param_9 - param_8`). So the fog pass issues about **6-9 dispatches per
frame when its gate is open**. The gate is observed as: `0 < f(+0xc)`, `0.5 < f(+0x38)`,
`byte(+0x1f0) != 0`, `0 < f(+0x210)` (field names unknown). `+0xb4` is a per-frame counter that picks
one of two history buffers (ping-pong), so the pass is temporal. **observed**.

**Particles.** `ParticleRenderer::UpdateSimulation` (`0x103ad0` A, `0x1051f0` B) is called from the
frame job `d0040` (the "UpdateSimulationJob" in report 06, step 4). `PrepForRendering` (`0x106950`
A, `0x106ce0` B) runs "sort draw-sort records", "sort ribbon-sort records" and "connect ribbons",
and calls `sort pairs` / `merge blocks` (`0x10c8c0` unique 28-byte signature; `0x10d1b0`). The
number of passes depends on the particle count (**unknown**). `ParticleLighting` /
`ParticleRenderer::UpdateLightmaps` (`0x1070e0` A, unique 20-byte signature; `0x1072c0` B, 4 call
sites each) is in `MainCamera`'s "UpdateLightmapsJob". `ParticleRenderer::BeginFrame` (`0x107540`)
clears counters. `ParticleSystemPool::ProcessStats` (`0x1018c0`, unique 32-byte signature) calls
`sub_928f0`, the label-wait wrapper, and is called from `ParticleRenderer::ProcessStats` (`0x109970`,
part of `EndFramesWaterAndParticle`). **observed**.

**Fur.** `UpdateFur`, `UpdateConcavity`, `Physics`, `CollectInteractiveTriangles`,
`PreUpdateInteractive`, `UpdateInteractiveTriangles` (`sub_182620` A, unique 44-byte signature;
`sub_182c60` B). `PreRenderFur`, `CullClumps`, `PrepIndirect`, `GenerateStrands` (`sub_183310` A;
`sub_183640` B). `RenderFur`, `RenderStrands`, `RenderShells` (`sub_183d10`, label wait via
`sub_18e2c0`). `FurClump` (`sub_1848d0`), `FurStrandNormalWeight` (`sub_184d20`),
`FurGenerateShell` (`sub_18cf40`, label wait, 2 call sites). The update runs in step 4 of report 06.
**observed**.

**Virtual geometry and instances.** `PreRenderVirtualGeometry`, `VirtualGeo`, `CullBatches`
(`sub_da220` A, `sub_da560` B), `PrepPlacementArgs` (`sub_dc740` A, `sub_de0c0` B), `PrepRenderArgs`
(`sub_dcf40` A, `sub_de9d0` B), `CreateInstances` (`sub_dcc80`, `sub_de680`), `FoliageDisplacerClusters`
(`sub_d8330`), `VirtualGeometryAmbientSoundPrivate` (`sub_1a4b70` A, `sub_1a3e50` B). The instance
path is gated by `r_enablInstanceDistanceFading` and `r_fadeOutInstanceOverDistance`. **observed**
(names); per-frame counts **unknown**.

**XPR culls.** `BPXPR_CullMainScene` (`0xc6f70`, "XPRRunFragmentCullShaderMainScene"),
`BPXPR_CullShadowScenes` (`0xc6bd0`, unique 32-byte signature, called from `sub_19c1a0` at 5 sites),
`IndexCulling SunShadow0-3`, `CullSunShadowCascade 0-4`, `CullSpotShadow 0-3`, `XPRDecalCull`
(`sub_c5200` via `sub_19c7e0`), `IndexCullXPRObjects` (`BPXPR_IndexCullObjects` `0x11dbe0`, label
wait). Cvars in the same table: `doIndexBufferCulling`, `doSunShadowIndexBufferCulling`,
`enableSmallPrimitiveIndexBufferCulling`, `forceUseLastIndexBufferCullingResults`,
`threadedCulling`, `groupCulling`, `enableAsyncCompute`. **observed** (strings); the effect of each
cvar is **unknown**. The cull dispatches are the 9 cascade/spot culls plus the main cull, every
frame.

**Water.** `WaterReset` (`sub_e60c0` A, `sub_e6350` B), `WaterRelocate` (`sub_e69e0` A, `sub_e84b0` B),
`WaterSimulation` (`sub_e7390` A, unique 52-byte signature; `sub_e80d0`, `sub_e8e80` B), `WaterQuery`
(`sub_e7b60`, `sub_e9670`). `WaterSimulation` clamps its time step: `min(dt, 1/30)` with `dt` from
`[ctx+0xd50]` (observed, `0xe7390` lines 62-68). It has three variants by mode `[param_4+0x1dc]`
(0, 1, 2; `lVar19` 0x157-0x15a). The water readback is `WaterReadQueries` (`0xe48b0`, report 06).

**Lights, probes and shadows.** `CStaticLightDynamicProbeCache::UpdateProbes` (`0x136b30`, unique
32-byte signature): it does work only when `[obj+0x148] != 0` and a queue is non-empty
(`DAT_02ee1500..1508`), so it is already event-driven. `CStaticLightContext::UpdateDynamicProbesJob`
runs in the step-4 job. `AmbientShadowing` (`sub_17c6c0`), `AmbientOcclusion` (`sub_17daa0`; a
distance gate `4.0/local_a0 <= ...`), `ResolveSunShadows` (`sub_afcc0` A, `sub_b0100` B). The LUT
`AmbientShadowLUT` (`sub_17cac0`) is created once (gate `plVar4 == 0`). `RenderSpecularIBL`
(`sub_c4210`) runs `sub_186df0`, which calls `sub_185840` (a 3.3 KB decision helper, not read in
full) and, on one path, the scene view `sub_a20b0` and the mip prefilter `sub_1871a0`. The BRDF
texture `CreateBRDFTexture` (`sub_187510`) is created once. **observed** (gates).

**Post.** `PrepareMotionBlur` (`sub_b50c0` A, 5 sites; `sub_b5b10` B, 5 sites), `ComputeExposure`
(`sub_b3590`, unique 28-byte signature; gated by `byte(+0x28c)` and `byte(+0x63) == 0`, and the
resolution must be ≤ 0x1000 in both axes), `TemporalAA` (`sub_b6760`), `ApplyDOF` (`sub_b6e20`),
`PrepareBloom` (`sub_17ecf0` A, unique 32-byte signature, 3 sites; `sub_17f790` B, 3 sites),
`BlurX`/`BlurY` (`sub_17e000` unique 32-byte signature, 2 sites), `PreparePostBuffer`
(`sub_17e6b0`, `sub_17e9c0`). **observed**.

**Depth pyramids and fog depth.** `CreateDepthTextureSet` (`sub_bae70`, from the depth pass
`0xc59d0`) calls `sub_ba640`, which calls `XPRCreateOccluderDepthMipMaps` (`sub_bab40`, 17 call
sites), `CreateDepthTextureMips` (`sub_bb190`), `CreateHalfResDepthTarget` (`sub_bb840`).
`CreateFogPerTileDepthTexture` (`sub_bb310`, 3 sites). **observed**. These run every frame in the
main camera path.

**Clusters.** `CreateClusters` (`sub_ab330`, `sub_ab3b0`) calls `sub_ab430` (family A) or `sub_ab630`
(family B). `sub_ab630` calls the label-wait dispatch at `0xab746`, then the normal dispatch at
`0xab815`. It is called from `ForwardRendering::CreateClustersJob` in the main camera. **observed**.

**Texture and instance uploads.** `CRenderer::UpdateTextureStreamingGPUData` (`0x117930`, calls
`sub_928f0`), "Material Param Update" / "updateDynamicGPUDataJob" (`sub_117160`, `sub_16eb90`
family A), `InstanceData_WriteStream` (`0x123410`), `InstanceDynamicData_Update` (`0x11c710`, label
wait). **observed**.

**Skinning.** The scope "Models Skinning" (`0xf9ee01`) is used by `sub_8c370`. Whether it
dispatches is **unknown**; it was not read.

### 2.3 Port side (observed)

- The dispatch shader hash (`cs.pgm_hash`) is already set per dispatch in
  `vk_rasterizer.cpp` (`t_direct_dispatch`, about line 2400). `Liverpool` counts compute prefetches
  (`dispatches`, `liverpool.cpp:1304-1316`).
- `SOTC_CMD_STATS=2` prints the most used compute shaders per window
  (`docs/sotc/README-SotC.md:152`). The hash-to-system mapping is not in any log yet.
- `SOTC_CS_STATS=1` (`vk_rasterizer.cpp:1654`) measures "same", "clean" and "seen" dispatch keys
  (the numbers quoted in BIND-PATH §2).
- `sceGnmDispatchDirect` (`gnmdriver.cpp:492`) writes a PM4 DispatchDirect and has no guest
  return address. `__builtin_return_address(0)` there is the trampoline, not the eboot caller
  (**inferred**; the existing SotcTrace sites use it for submits only).

### 2.4 Signatures (`eboot_re.py sig`, observed)

Unique: `0xb3590` (28 B), `0x136b30` (32 B), `0xe7390` (52 B), `0x103ad0` (32 B), `0x17ecf0` (32 B),
`0x1070e0` (20 B), `0xbab40` (32 B), `0x92b50` (20 B), `0x1018c0` (32 B), `0xc6bd0` (32 B),
`0x182620` (44 B), `0x10c8c0` (28 B), `0x17e000` (32 B), `0x1051f0` (32 B).

Not unique at 128 B: `0x86b9e0` (2 matches), `0x883870` (2), `0xa5e80` (2), `0xab330` (3),
`0x117930` (2). Those need a site-based or longer signature before a hook can be declared on them.

## 3. Hook proposals

Existing cvars are set with `SOTC_CVARS="NAME=VALUE ..."` (`sotc.py:26`). That is a no-code
lever for the L-items below.

**H1. Caller probe for both dispatch families (measure only).** Entry on family B `0x86b9e0` (or
family A `0x883870`) and read the return address at `[rsp]`, then count per caller. Mode: `entry`.
Change: none (the probe only observes). Why: gives per-system dispatch counts per frame, which
section 1 says are unknown. What could break: nothing functional; the probe must not change
registers. Verify: `=2` mode prints the top 20 caller addresses per 5 s window and maps them to
the scope list in 2.2. Signature: **not unique** at 128 B. Declare it at the import site instead
(`0x86ba40`, `sceGnmDispatchDirect` call in family B) or add a 2-byte disambiguating prefix. The
site form needs checking with `sig`.

**H2. Label-wait dispatch: defer the wait (the main candidate).** Wrap on `BPGnm_DispatchWithLabelWait`
`0x92b50` (unique 20-byte signature), and on `sub_928f0` (family-B label-wait wrapper, signature
not checked). Change: issue the dispatch and its label write, and move the CPU wait to the first
reader of the label (or to the frame-end gate), instead of waiting at dispatch time. Why: the
engine waits at dispatch time for 14 + 9 callers, several per frame (particle stats, texture
streaming, instance data, cluster build). Waiting only when the result is read removes the stall
from the frame body. Gain: **unknown**; it depends on whether the wait blocks (inferred) and how
many labels per frame (unknown). What could break: a reader that runs before the GPU finishes
reads stale data (particle counts, streaming state, cluster lists). Each caller must be checked
before its wait is moved. Verify: `=2` mode keeps the wait and also counts how many labels the
reader later finds already signalled; zero means the reader does not need it. Signature: unique
for `0x92b50`. Overlaps the readback topic: check that report before implementing.

**H3. Particle stats every N frames.** Wrap on `ParticleSystemPool::ProcessStats` `0x1018c0`
(unique 32-byte signature). Change: run the stats dispatch and label wait every 4th frame and skip
it otherwise. Why: it adds a label-wait dispatch to every frame. Gain: about 1 label wait per frame
(**inferred**). What could break: the stats feed the emitter budget; a stale budget can spawn too
many or too few particles for up to 3 frames. Verify: `=2` mode runs the original and compares the
stats it would have produced; count mismatches and the spawn counts. Signature: unique.

**H4. Cluster build cache.** Wrap on `CreateClusters` `sub_ab330` (signature not unique at 128 B,
3 matches; need a longer prefix). Change: skip the build when the projection, the view and the light
list are unchanged since the last build. Why: `sub_ab630` builds the cluster list every frame, and
it includes a label wait (`0xab746`). Gain: **unknown**; in gameplay the camera moves every frame,
so the hit rate is probably low (**inferred**). In static shots (title screen, idle Shrine) it is high.
What could break: stale light clusters on a fast camera move or a light change. Verify: `=2` mode
builds both and compares the cluster index buffers byte for byte; count differences.

**H5. Exposure every other frame (template only).** Entry on `ComputeExposure` `0xb3590` (unique
28-byte signature). Change: on odd frames, return early and keep last frame's exposure. Why: it is
an adaptation pass and the result changes slowly. Gain: small (1-2 dispatches; about 0.01 ms of
bind). What could break: exposure lag of one frame, which is not visible. Verify: `=2` mode
computes both and checks the luminance difference against a threshold. Use this as the pattern for
the other half-rate candidates, not as a gain.

**H6. Water sub-rate.** Wrap on `WaterSimulation` `0xe7390` (unique 52-byte signature). Change: run
the sim every other frame with `dt` doubled (still clamped at 1/30 per step). Why: it dispatches
every frame. Gain: **unknown** (the water dispatch count per frame is unknown). What could break:
visible wave stepping at 30 FPS, since the clamp is already at 1/30. Verify: `=2` mode runs both and
compares the height field; count frames above the tolerance. Lower priority; visual risk is high.

**H7. Fog gate and cache.** Wrap on `sub_a5e80` (Fog pass, not unique at 64 B; use the `a6260` site
or a longer prefix). Change: skip the whole pass when the view-projection did not change and the
history buffer is valid. Why: the fog pass issues 6-9 dispatches when open. Gain: **unknown**; the
view changes every frame in gameplay, so the hit rate is low. What could break: fog ghosting on the
frame the view changes. Verify: `=2` mode runs both and compares the fog output. Not recommended as
a first step.

**L-items (cvar levers, no hook; for measurement first).**
- **L1.** `forceUseLastIndexBufferCullingResults` (`0xf9a971`): reuse last frame's index cull results.
  Removes the cull dispatches on frames where it applies. Visual risk: popping for one frame. Test:
  `SOTC_CVARS="forceUseLastIndexBufferCullingResults=1"`, then compare the draw count (`SOTC_CMD_STATS`)
  and the frame time.
- **L2.** `r_motionBlurEnable=0` (`0xf9c3b5`): removes the 5 `PrepareMotionBlur` call sites. Visual
  change; the user must accept it.
- **L3.** `r_fullscreenBlurNumPasses=1` (`0xf9c2bc`): fewer blur passes. Visual change.
- **L4.** `doSunShadowIndexBufferCulling=0` (`0xf9a92c`): does a cull dispatch cost more than the
  draws it saves? Measure only.
- Do not test `r_enableVirtualGeometry=0`: it removes the whole VG pipeline and changes the picture.

## 4. Open questions

1. The per-frame dispatch count per system. Run H1 (or `SOTC_CMD_STATS=2`) on the attract scene and
   the Shrine, then map the caller addresses to the scope list in 2.2. Until then all gain numbers
   are **unknown**.
2. Does `BPGnm_DispatchWithLabelWait` block the CPU? A trace of the label write and the wait on one
   frame would settle it. This is the same question as the readback topic.
3. Why are there two `ParticleRenderer::UpdateSimulation` functions (`0x103ad0` and `0x1051f0`), two
   wrapper families, and an extra `sub_928f0` label-wait wrapper? Is one family used for the
   engine's first pass and the other for a later one?
4. Is the caller's return address reachable inside `sceGnmDispatchDirect`? If not, H1 must be
   declared as a site hook on the eboot's import call.
5. `sub_185840` (IBL decision, 3.3 KB) and `sub_8c370` (Models Skinning): do they dispatch, and how
   often?
6. The cvar effects (`forceUseLastIndexBufferCullingResults`, `doSunShadowIndexBufferCulling`,
   `r_motionBlurEnable`) are named but not read. Their code paths need reading before L1-L4 are
   trusted.
7. `ComputeExposure`'s `+0x28c` and `+0x63` gate fields and the `DAT_0160445c` value are not named.

## Summary (10 lines, ranked by expected gain)

1. H2: defer the label waits of the 14+9 label-wait dispatch callers (gain unknown; stall-related).
2. H1: measure first with a caller probe (no gain; gives per-system counts).
3. L1: `forceUseLastIndexBufferCullingResults=1` as a cut to the cull dispatches (gain unknown).
4. H3: particle stats every 4th frame (about 1 label wait per frame saved).
5. H4: cluster build cache for static views (high hit rate only in static shots).
6. H6: water sim every other frame with dt doubled (visual risk; gain unknown).
7. H7: fog cache when the view is unchanged (low hit rate in gameplay).
8. L2/L3: motion blur and blur passes off or reduced (visual change; cheap to test).
9. H5: exposure every other frame (tiny gain, but a clean template for half-rate passes).
10. Per-dispatch bind is ~3.9 µs, so pure count cuts give ~0.2 ms per 50 dispatches; only the stalls matter.
