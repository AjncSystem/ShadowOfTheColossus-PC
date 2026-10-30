# Topic 08: shader programs and material binding (first use)

Read only. No build, no game run, no repository edits except this file.
Tools: `tools/re/eboot_re.py` on `out/eboot.elf` (EU 1.01), `out/re/decomp`, the port source,
`user/cache`, `user/last_run*.log`, `user/crashes/`. The game dump (`../CUSA08809`) is not in this
checkout, so no `.psarc` or `.cslt` file was read.

## 1. Answer

- A GCN shader binary identifies itself. Its binary info footer starts with the 7 bytes `OrbShdr`
  and holds `shader_hash` (u64), `length` and the stage type. The port looks it up with
  `SearchBinaryInfo` (`regs_shader.h:224`), and the port's program key is that hash. So the set of
  program hashes can be read from loaded guest memory without running any draw. **observed (code)**
- The engine's `.cslt` shader library does not hold the binaries. Its parser (`sub_b0e4d0`, 0xb0e4d0)
  reads a count and a table of offsets to names, and resolves each name through the resource factory
  (`vtbl+0x18`). The binaries live in the named resources. Which resource type carries the `OrbShdr`
  blobs is **unknown** (needs the dump). **observed (parser) / unknown (data)**
- The engine chooses shaders by object, not by permutation. A draw binds a VS object at
  `this+0xc0b0`, a PS object at `+0xc0a8` and a CS object at `+0xc0d8`. Each object has the code
  address at `obj+8` (`<<8`) and a size in `obj+0` (`& 0x7fffff`). A usage block at `+0xc190` maps
  user-data SGPR slots to resource classes. **observed (disassembly, 0x85f410, 0x861730, 0x861880)**
- Permutations (`StageSpecialization`: fetch formats, bindings, runtime info) exist only in the port.
  They cannot be read from data; they depend on draw state. In the user's cache, 4,027 programs have
  5,308 modules, so about 1,281 modules (24%) are extra permutations. **observed (cache) / inferred (cause)**
- Today the port translates on the draw path (`GetProgram`, `vk_pipeline_cache.cpp:1481`). Shaders
  are queued to workers only for a draw that is not a tiny draw (<= 6 indices), not depth-only and
  has dynamic vertex input. Other draws translate on the command processor. **observed**
- A ready hook point exists: `ResourceFactoryResourceLoader::LoadFileInternal` (0x14b980, unique
  24-byte signature). After a file is read, scan it for `OrbShdr` footers, then queue the base
  permutation of each program with a new `PrefetchProgram`. This copies `PrefetchComputeProgram`
  (`vk_pipeline_cache.cpp:982`), which already does this for compute. **inferred (design)**
- Size of the win: routine area-entry frames lose 6-20 ms to 4 translations (about 2 ms each).
  Over all 44 logged stall frames, translation is 52 ms of 6,488 ms (0.8%), not counting one outlier.
  The outlier frame (crash log `2026-10-29_04-28-22_0x3`, frame 148) spent 1,151 ms of its 1,979 ms
  on 544 translations. The cause is unknown; it matches the stale-permutation defect in report 07
  (462 stale pipelines in that run). **observed (counts) / inferred (cause)**
- Expected reduction: routine area-entry shader time falls by most of 6-20 ms per frame (up to about
  10% of a 150-200 ms frame), and the frame is still dominated by uploads (60-230 ms). The outlier
  case is the one with real gain, and only if its translations are base permutations (unknown).
- Cheapest first step: fix the stale-permutation index (report 07). It likely removes most of the
  544-translation burst and needs no engine hook.

## 2. Evidence

### 2a. Engine shader binding (observed)

- `Material_BindGraphicsShaders_A` (0x85f410, 2,042 bytes) and `_B` (0x85fc10, 3,995 bytes) are the
  only callers of the graphics shader imports (`GnmShader_SetVs` 0x86c170, `SetPs350` 0x86c050,
  `UpdateVs` 0x86c250, `UpdatePs350` 0x86c110). `eboot_re.py callers` confirms this.
- Object layout, from the decompiled 0x85f410:
  - VS object `*(r13+0xc0b0)`; code `(u32)[obj+8] << 8`; size `[obj+0] & 0x7fffff`; regs block at `obj+8`.
  - PS object `*(r13+0xc0a8)`; regs block `obj+8`; setter not found.
  - CS object `*(r13+0xc0d8)`; set by `sub_861880` (0x861880, a short setter that also stores the
    CS usage fields `+0xc5d8`, `+0xc5e0`).
  - Each bind emits `sub_86a7f0` (0x86a7f0) with (code address, size): a PM4 DMA packet
    (`0xC0055000`, opcode 0x50), which prefetches the shader code. This is per bind, not per program.
- Usage block `*(r13+0xc190)` (set by `sub_861730`, 0x861730): a `u16` count at +0 (dwords of user
  data, copied from `r13+0x3090` through `sub_8643f0` and `memcpy`), slot bytes at +7, +8, +9, +0xa,
  +0xc, +0xd, +0xe, and dword offsets at +0x16, +0x1a, +0x1c, +0x1e. Each slot is written with
  `sub_86d4c0` (SET_SH_REG to a user-data register). This is the SGPR layout that maps user data to
  V#/T#/S#/CB classes. **observed (decomp) / inferred (meaning of each field)**
- `sub_861730` (VS state setter) reads a 12-byte identity from the new VS object at `+0x18..+0x20`.
  It compares it with the previous object to skip rebinds. Its callers are indirect (none found).
  **observed / unknown (identity meaning)**
- Shader selection is per material pass: `sub_862c50` (0x862c50, 1,249 bytes) builds a 0x54-byte draw
  record at `this+0xc8b8` from the VS object (`+0x10`, `+0x18`, `+4`, `+8`) and the draw state. Its
  callers are not found. **observed**

### 2b. Loaded resources (observed unless marked)

- `.cslt` registration: `sub_b0e3a0` (0xb0e3a0) registers the FourCC `0x63736c74` (`cslt`) with the
  loader `sub_b0e440` (0xb0e440). `sub_b0e440` calls `sub_b0e4d0` (0xb0e4d0, 719 bytes). The parser
  reads a count, then for each entry a relative offset to a name. It resolves each name through the
  factory (`DAT_02ee2cf8`, `vtbl+0x18`), which yields a referenced resource. The result is a list of
  16-byte references (ptr + flag). So a `.cslt` is an index of named resources. **observed**
- Engine shader libraries named by string: `engine-shaders.cslt`, `particle-shaders.cslt`,
  `lightprobe-shaders.cslt` (`sub_143f50`, 0x143f50). Other resource strings are in the same table
  (`default.cmat`, `default.cmdl`). **observed**
- `shaderTypeToLoad` (cvar string 0x12cfbe0; the log shows `Shipping`) selects the shader build type.
  It does not select permutations. **observed (report 07, log)**
- The resource loader: `ResourceFactoryResourceLoader::LoadFileInternal` (0x14b980, 1,627 bytes)
  calls two virtuals of the loader object: `vtbl+0x80` with `param_1[0x12]` and `param_1[0xf]`, and
  `vtbl+0xa8` as a completion test. Those are the buffer and size of a file read (**inferred** from the
  call arguments; not confirmed by a probe). Entry `0x14b800` is the job start (`StartLoadJob`).

### 2c. Port side (observed)

- `GetProgram` (`vk_pipeline_cache.cpp:1716`): program cache keyed by `params.hash`. A miss, with
  `async_ok`, queues `QueueAsyncTranslate` and returns `translation_pending`. Without `async_ok`,
  `CompileModule` runs on the command processor (`shaders_compiled` counts it, line 1841).
- `async_translate_ok` (`vk_pipeline_cache.cpp:1326-1327`): needs workers, `num_indices > 6`,
  dynamic vertex input, no shader collection, and not `depth_only`.
- `PrefetchComputeProgram` (`vk_pipeline_cache.cpp:982`): the compute analogue. It takes params from
  the code address, returns when `IsProgramKnown`, and calls `QueueAsyncTranslate` with a default
  `RuntimeInfo` and `Bindings{}`. `Liverpool::PrefetchComputeAhead` (`liverpool.cpp:1296`) scans the
  rest of the command buffer for compute dispatches (`SOTC_ASYNC_COMPUTE_XLATE`, off by default).
- `SearchBinaryInfoCached` (`sotc_bininfo.cpp:64`) caches the footer per code address, validated
  through the backing view. A scan of loaded memory can reuse this, and avoids readbacks.
- `info.h:195-198`: the flat buffer fix-up reads user-data values (`RefreshFlatBuf`). The SPIR-V
  translation itself is not stated to depend on user-data values (**inferred**). The port already
  resets this per draw (`info.UserData() = params.user_data`).
- Area manifest: `SotcAreaPrewarm::OnAreaChange` (`gpu/sotc/area_prewarm.h`) already records per-area
  image records in `user/cache/area_manifest/<area>.bin` (mode 2) and replays them (mode 3). It is
  called from `probe_area.cpp:199` with the area name. No program manifest exists (the
  `user/cache/area_manifest` directory is empty here). **observed**
- `TinyAsyncMode` comment (`vk_pipeline_cache.cpp:600-611`): the per-area prewarm
  (`SOTC_AREA_PREWARM`) is "the intended fix for first-use builds". Program translation is not part
  of it yet. **observed**

### 2d. Measurements (observed)

| Source | Value |
|---|---|
| `user/cache/CUSA08809_nodma` | 4,027 distinct program hashes; 5,308 `.spv` modules (770 programs with 2, 226 with 3, ...; 1,281 extra permutations) |
| SOTCSTALL lines with `shaders > 0`, all logs (44 stall lines, 6,488 ms) | 8 lines; 1,204 ms in total |
| same, without the outlier frame | 7 lines, 53 ms (about 4 shaders at 2 ms each, 1-20 ms per frame) |
| outlier: crash `2026-10-29_04-28-22_0x3`, frame 148 | 1,979 ms frame; `shaders 544 (1151 ms)`, `pipelines 3 (216 ms)`, `cp 1555.9 ms` |
| same run | `WarmUp: 462 stale pipelines`; `SOTC ASYNC TRANSLATE 628 stages (1269 ms on workers)`; `shader cache: 28 old binaries replaced, 2 permutations left out (slot taken)` |
| same run | `RegisterShaderBinary: Shader permutation ... is another one on disk: not cached` (several lines) |
| `last_run*.log` area-entry frames (148-157) | `shaders 4 (7-20 ms)`, next to `upload 137-230 ms`, `cp 37-125 ms` |

The 544 count is close to the 462 stale pipelines in that run. That is a correlation only (**inferred**);
the stale keys were not matched one to one against the translated keys.

## 3. Hook proposals

### H1. Program prefetch from loaded resources (ranked first)

- **Site.** `ResourceFactoryResourceLoader::LoadFileInternal`, 0x14b980 (names.txt; decomp
  `out/re/decomp/0014/0014b980.c`).
- **Mode.** `wrap`. Pre phase: none. Post phase: scan the file buffer just read (exact fields to be
  confirmed by a `=2` probe that logs the pair of values).
- **Signature.** Unique, 24 bytes:
  `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 e4 e0 48 81 ec 00 05 00 00`
  (checked with `eboot_re.py sig 0x14b980`). Expected address: 0x14b980 for EU 1.01.
- **What it changes.** The post phase searches the buffer for the 7-byte `OrbShdr` signature. For each
  hit it reads `shader_hash`, `length` and the stage, and finds the code start from the
  `0xBEEB03FF` header (the same rule as `SearchBinaryInfo`). It then calls a new
  `PipelineCache::PrefetchProgram` (copy of `PrefetchComputeProgram`) with a default runtime
  info. It queues the base permutation (perm 0) for a worker. Pre-existing programs are skipped.
- **Why it shortens hitches.** A base permutation is then translated before the first draw that
  binds it. The draw finds it in `program_cache` (no `shaders_compiled` on the command processor).
  Routine area-entry frames lose their 4 translations.
- **What could break.** (1) Scan cost on large files; limit the scan to buffers the port already
  reads, and use the backing view as `SearchBinaryInfoCached` does (no readbacks). (2) Wrong
  length or stage, if the footer is found inside data rather than a binary: validate `Valid()`
  and the head token. (3) Permutations still translate on demand, so the win is partial. (4) Memory
  for queued SPIR-V that is never used (about 4,000 programs seen in one cache).
- **Verify.** `=2`: log every hash found and every hash a draw later binds. Coverage is the share of
  bound hashes that the scan found. `=1`: cold cache (move `user/cache/CUSA08809_nodma` aside), count
  `shaders` in SOTCSTALL at the first area entry.
- **Gain estimate.** Routine area frames: -6 to -20 ms each (observed range). Outlier: unknown,
  up to the 1,151 ms of that frame, only for base permutations.

### H2. Per-area program manifest (no new engine hook)

- **Site.** `AREA_PRECACHE` (0x533100, entry) and `SotcAreaPrewarm::OnAreaChange` (already wired:
  `scripts/hooks.d/area.py`, `probe_area.cpp:199`). Signature for 0x533100 already declared and unique.
- **Mode.** Port-side: `entry` probe already in place; add records to the area manifest.
- **What it changes.** Mode 2 records (hash, stage, length, code address) for each program translated
  while the area is current, in `user/cache/area_manifest/<area>.bin`. On the next visit, mode 3 queues
  those hashes via `PrefetchProgram` when the area changes. The code address comes from H1 (the scan),
  because addresses may differ between boots (**unknown**).
- **Why.** The second and later visits of an area become first-visit-free. This matches the existing
  image design.
- **What could break.** Stale manifest after a patch (the hash is the content hash, so a changed
  shader is a new key: low risk). Manifest size. Replay memory budget (reuse `SOTC_AREA_PREWARM_MB`).
- **Verify.** Mode 2 in a run, then mode 3 in the next run. Compare `shaders` in SOTCSTALL on the
  second area visit.
- **Gain estimate.** Same order as H1 on second visits; no effect on the first ever visit.
  Unknown.

### H3. Read-only probe on the VS state setter (coverage check, not a fix)

- **Site.** `sub_861730` (0x861730, VS object and usage block setter).
- **Signature.** Unique, 8 bytes: `55 41 56 53 0f b7 46 24` (checked with `eboot_re.py sig 0x861730`).
- **Mode.** `entry`, read only. It reads the VS object pointer `rsi` and usage block `r8`.
- **What it changes.** Nothing. It logs each bound VS hash (`[rsi+0x18]`, `[rsi+8]`) so that H1's
  coverage can be checked. It also gives the usage layout for the permutations work.
- **Why.** It does not shorten a hitch by itself.
- **What could break.** Nothing if it only reads. Hot path: one log per draw is expensive; sample it.
- **Verify.** Compare its hash set with the `PrefetchProgram` set (H1, mode 2).
- **Gain estimate.** None directly.

### H4. Permutation prediction (not proposed for now)

Permutations depend on draw state (fetch and vertex formats, blend and target state). They cannot be
read from loaded data. Predicting them would need the V# formats of each draw. About 24% of cached
modules are extra permutations. **unknown** gain; treat as later work.

### Port-only items with a clear measurement

- **P1. Stale permutation index (report 07, 2b).** Fix `LoadPipelineStage` (`vk_pipeline_serialization.cpp:474-484`).
  Expected: the stale count (419 in the dev runs, 462 in the outlier run) goes to zero, and the
  outlier's translation burst should shrink (unverified).
  Verify: `WarmUp: N stale` and `permutation ... conflicts` lines are gone; the first area entry
  shows `shaders 0`. **Cheapest, and it may explain the outlier.**
- **P2. Tiny draws and depth-only draws translate on the command processor** (`vk_pipeline_cache.cpp:1326`).
  They are never queued. H1 or H2 can cover them when the program is known from the scan.
  Verify: count CP translations by reason (add counters). **unknown** gain.

## 4. Open questions

- Which resource type holds the `OrbShdr` blobs (`.cmdl`, `.cslt`, `.cmat` or a separate type)? Check
  first with an offline scan of the user's dump at prepare time: search the decompressed `pak*.psarc`
  content for the bytes `4f 72 62 53 68 64 72` and `ff 03 eb be`, and count. That count is the true
  size of the program set (4,027 seen so far is a lower bound). Needs `SOTC_GAME_DIR`.
- Does the loader keep shader resources in memory for the whole session, or unload them per area?
  If they unload, the code address of H1 changes and H2 must rescan.
- The buffer fields of `LoadFileInternal` (`param_1[0xf]`, `param_1[0x12]`): confirm with a `=2` probe.
- The PS object setter (`this+0xc0a8`) is not found. Find its writer.
- Callers of `sub_861730` and `sub_862c50` are indirect (vtable). Which draw path calls them is unknown.
- Does a `.spv` cache hit at draw time skip translation? The cache is read at boot (WarmUp) and by
  `RegisterShaderBinary`. The draw-time translation path was not traced.
- What was the 544-translation frame: base programs, permutations, or a sync path (tiny draw or
  depth-only)? A counter by path would answer it.
- Are the 462 stale pipelines the same keys as the 544 translated programs? A key dump would show it.
