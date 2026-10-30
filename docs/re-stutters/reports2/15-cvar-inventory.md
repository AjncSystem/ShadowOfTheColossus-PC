# 15 - Engine cvar inventory

Read only. Addresses are eboot VAs. Labels: **observed** (disassembly with address), **inferred**,
**unknown**. The agent could not write this file, so the lead wrote it from the agent's report.
The full 1,316-row table (name, registration site, constructor, range, stored default) and the
scan scripts are not in the repository. They stay in the session scratchpad
(`waveC/cvar_inventory.csv`, `cvar_scan.py`, `defaults.py`, `inventory.py`).

## 1. Answer

- **observed: 1,316 identifier-style cvars.** Registration pushes the name pointer and its length
  on the stack, then calls a constructor. The constructors and their call-site counts:
  - `sub_c300` (495 sites);
  - `sub_20af0` (462);
  - `sub_6aa00` (59);
  - `sub_a81180` (37);
  - `sub_3a980` (32);
  - `sub_c580` (30);
  - `sub_f730` (28).
- **observed: where the main cvars are registered.**
  - The engine core is in `sub_262a80`: `r_fps`, `gameMaxDeltaTime`, `VSyncWindow`,
    `TargetFramesPerSeconds`.
  - `RegisterXprCvars` (0x6efe0) registers the XPR, shadow and render block. Its defaults are
    immediates in `sub_53fc0` on the struct at 0x2ea8ee8.
- **observed: the dt cvars.**
  - `gameMaxDeltaTime` defaults to 0.1 s (`mov dword [rbx+0xd0], 0x3dcccccd` at 0x263542; also at
    `[rbx+0x68]`, 0x26354c).
  - `gameDeltaTimeCorrectionTolorance` defaults to 0.5.
  - Report reports2/05 shows that the update thread's dt comes from another float, so the effect
    of lowering `gameMaxDeltaTime` must be measured with a dt probe first.
- **observed: what the launcher and the shipped command line already set.**
  - The launcher sets `doIndexBufferCulling=0` and `doSunShadowIndexBufferCulling=0`.
  - `VSyncWindow` has a stored default of 0; the shipped command line sets `+VSyncWindow=10%`
    (`scripts/sotc_modes.py:41`).
  - `TargetFramesPerSeconds` has a stored default of 0 (30 FPS). The `fps60game` mode sets 1.
- **observed: the streamer cvars have ranges, but their defaults come through data pointers and
  were not decoded.** These are the texture pool sizes, the load limits, `numMipRequestsPerFrame`
  (1-256) and the unload timers.
- **unknown: 893 of the 1,316 defaults.** Most are bools of the `sub_20af0` family, including the
  threading and async switches (`threadedRendering`, `threadedCulling`, `threadedSumbit`,
  `enableAsyncCompute`).
- **inferred: no engine hook is needed.** Every lever below is a command-line cvar, set with
  `SOTC_CVARS` or the launcher's `engine_cvars`.

## 2. Evidence

**Method.** `cvar_scan.py` finds a `lea` of a C string, followed within 14 instructions by a qword
stack store of that pointer and a store of `len(name)` at +8.

- That gives 2,437 sites and 2,164 names.
- Keeping only names made of letters, digits and underscores leaves 1,316. The rest are
  debug-menu labels with spaces.
- Hand-checked: `r_fps` (0x262b55, len 5), `gameMaxDeltaTime` (0x2634d1, len 16), `VSyncWindow`
  (0x263f69, len 11), `TargetFramesPerSeconds` (0x263e99, len 22).

**Constructors.** The types are inferred from each constructor's shape.

| Constructor | Inferred type | Where the values go |
|---|---|---|
| `sub_c300` | float with a range | min at +0x6c, max at +0x74; the caller stores the default at +0x68 and +0xd0 |
| `sub_20af0` | bool or int | flag bytes at +0x68 and +0xc0 |
| `sub_6aa00` | float | f32 default at +0x68 and +0xd0 |
| `sub_f730`, `sub_c580` | int with a range | three stack slots: default `[rsp+0x18]`, then `[rsp+0x3c]` and `[rsp+0x38]` (min/max order inferred) |

**Defaults checked by hand.**
- `gameMaxDeltaTime` 0.1, `gameDeltaTimeCorrectionTolorance` 0.5.
- `gameSpeed` 1.0 (not re-verified).
- `VSyncWindow`: 0, with range 0-5.
- `TargetFramesPerSeconds`: 0, with range 0-5.
- Ranges:

| cvar | Range |
|---|---|
| `sunShadowMaxDistance` | 0-1000 |
| `r_fadeOutInstanceOverDistance` | 0-200 |
| `r_ambientShadowRadiusToFadeRange` | 5-50 |
| `r_forceLod` | -1 to 10 |
| `numMipRequestsPerFrame` | 1-256 |
| `r_virtualGeometryScaleMultiplier` | 0-10 |
| `samplerLodBias` | -4 to 4 |
| `particleLowResBudgetFactor` | 0-1 |

**Limits.**
- The consumers of most cvars were not traced, so "controls" below is inferred from the name.
- The scanner gives a lower bound: some registrations may be missed.
- The logs show only the engine command line; they do not show cvar values at run time.

## 3. Cvar levers (no hook; command line)

| cvar | Default | What it controls (inferred) | Test | Visual cost |
|---|---|---|---|---|
| `gameMaxDeltaTime` | 0.1 (observed) | sim dt cap | 0.033-0.05, after a dt probe | none on the picture; hitch frames run slower in game time |
| `forceUseLastIndexBufferCullingResults` | n/d | reuse the last frame's cull results (reports2/04 L1) | 1 | one frame of popping on a view change |
| `textureMinUnusedFramesForUnload` / `cacheUnusedTextures` | n/d | unload delay / keep unused textures | 600 / 1 | more memory |
| `textureMaxLoadsInFlight` / `numMipRequestsPerFrame` | n/d | streaming per frame | 2-4 / 4-8 | longer pop-in |
| `MaxSimaltaneousAreaLoadRequests` | n/d | parallel area loads | lower | longer loads |
| `threadedRendering` / `threadedCulling` / `threadedSumbit` | n/d | engine threading | 1, if the default is 0 | none expected; not tested |
| `enableAsyncCompute` and its variants | n/d | async compute | one at a time | none expected; low gain on a CP-bound port |
| `r_fadeOutInstanceOverDistance` | n/d (0-200) | instance fade distance | 120 | distant detail ends sooner |
| `sunShadowMaxDistance` | n/d (0-1000) | shadow distance | 400 | shadows end sooner |
| `r_virtualGeometryScaleMultiplier` | n/d (0-10) | VG detail | 0.5 | less distant detail |
| `r_virtualGeometryNumShadowCascades` | n/d (0-4) | grass shadows (launcher) | 1-2 | grass shadows lost |
| `particleNumShadowCascades` / `particleEnableLOD` | n/d | particle shadows / particle LOD | 2 / 1 | fewer distant particles |
| `r_motionBlurEnable` / `r_fullscreenBlurNumPasses` | n/d | post effects | 0 / 1 | visible; GPU only |
| `AO_Mode` / `fog_enable` | launcher sets 0 | AO / fog | 0 | visible |
| `progSamplerAnisoLevel` | af16 mode sets 16 | anisotropy | 4 | blurrier textures at oblique angles; GPU only |

Not recommended: `r_forceLod` (it changes the LOD of the whole scene).

## 4. Open questions

1. Decode the bool defaults of the `sub_20af0` family, and check whether they live in the struct at
   0x2ea8ee8 (`sub_53fc0`).
2. Decode the streamer defaults. They are passed by data pointer in the `sub_3a980` and
   `sub_c580` calls.
3. Trace the reader of `gameMaxDeltaTime` to confirm the clamp, and find what
   `gameDeltaTimeCorrectionTolorance` does.
4. Confirm the min/max argument order of `sub_f730` and `sub_c580`.
5. Trace the consumers of `threadedRendering` / `threadedCulling` / `threadedSumbit`. If one of
   them moves work off the command processor, it is the best untested lever.
6. Find the registrations the scanner missed (the texture and streamer families).
