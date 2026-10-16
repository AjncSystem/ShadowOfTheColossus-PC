# Path B: bbport GPU optimizations in shadPS4 SotC

Original request: bring bbport’s GPU optimizations to our shadPS4, such as the
two-thread draw pipeline and FSR.

## What the tests showed (07–08/10/2026)

The native port initially ran with bbport’s video core, which includes ~40 optimizations
that can be disabled with `BB_TOGGLE_FILE`, listed in bbport’s `gpu/shim/bbport_toggles.h`.

| bbport optimization | Effect on Shadow of the Colossus |
|---|---|
| `PendingFenceWaits` (bit 39) | intermittent black screen at boot (50–75% of runs) |
| `DrawPipeline`, `DrawPreparation`, and related (ahead-of-time and deferred preparation) | stale or garbage GPU state while streaming as a save loads: invalid shader programs, `Unpack10_11_11`, device lost |
| relaxed GPU readbacks (bbport default) | the game hangs; SotC requires precise readbacks (`BB_READBACKS=2`) |

With the fork’s video core replacing bbport’s (while keeping bbport’s loader and runtime), the
game worked correctly: menus, saves, and open world, with no crash for 10 min. These optimizations
assume the game does not read on the CPU what the GPU has just written. This is true for Bloodborne,
but not for SotC.

## Current measurement (native port, i9-13900K + RTX 2060 SUPER, 1080p)

- Default: locked at 30 FPS, as on PS4; 22–25 under load and in the temple.
- `fps60` mode (120 Hz vblank): 38 FPS in the open field, ~25 in the temple.
- Bottleneck: the `GpuCommandProcessor` thread is 72% busy. Time is spent in the Vulkan driver and
  waiting on fences for precise readbacks. The game’s job workers are at 36–43%.
- The shadPS4 fork ran at 17–18 FPS (consolidated report), so the native port is already about
  1.7x faster.

## Conclusion

Copying bbport’s two-thread pipeline into the fork as-is would break SotC for the reasons in the
table. FSR provides no benefit because the game already renders at native 1080p.

The native port and the fork now use **the same video core** (the files in
`gpu/shadps4/video_core` come from the fork). Therefore, any optimization made here applies
to both. The viable path is to optimize this shared core one component at a time, with A/B
testing: `tools/sotc_hangtest.py` and the save-loading route in `tools/sotc_play.py`.
Candidates, in order of risk:

1. Pure bbport caches, without deferral: `RegionCache`, `FetchShaderCache`,
   `ImageDescCache`, `FindImageCache`, `TextureBindingMemo`, `RenderStateMemo`.
   These target command-thread overhead.
2. `ThreadedRecording` (Vulkan recording on another thread), while preserving strict ordering
   relative to precise readbacks.
3. Cheaper precise readbacks: copy only the pages being read and wait for the correct fence instead
   of waiting for the GPU to become idle.

Each item is a manual merge between cores that have diverged by ~17 thousand lines. The expected
gain is in 60 FPS mode: the default mode already reaches the PS4’s 30 FPS.

**Discarded test (15/10/2026):** the `TextureCache::UpdateImage` fast path was tested
with protection against concurrent invalidation and deallocation. In the attract demo, with a 360 s
run per state and the first 60 s discarded, the unoptimized state averaged 56.35 FPS and had
37/60 5 s windows at 50 FPS or higher; with the fast path, it averaged 55.99 FPS and also had 37/60.
The average p95 per window barely changed (26.21 → 26.17 ms), and the worst observed p99 was
74.99 ms disabled and 83.42 ms enabled. There was no frame-time improvement to justify
keeping the added complexity; the change was not kept. This does not reach a stable 50 FPS and
still requires measurement on the save/streaming route to assess long hangs.

**Menu/save-route soak (15/10/2026):** the 360 s run remained active and the captures
show game scenes after the studio screen, but they do not confirm that a progression save was
loaded: the log records only `SAVEDATA0ShadowProfile0` mounted read-only, with no
`ShadowProgress` mount. Therefore, the data below are not a confirmed benchmark of loaded
exploration. Discarding the first 60 s, the average was 54.74 FPS, with a minimum
of 24.4 FPS and 35/60 5 s windows at 50 FPS or higher; average p95 of 26.62 ms, worst p99
of 91.53 ms, and maximum frame time of 234.27 ms. A capture at 120 s was completely white,
which also needs visual verification. The script must confirm the selection screen and
progression-save mount before repeating the soak; `rc=1` came from the harness timeout and,
on its own, does not prove a crash.

**Profile of the confirmed progression-save route (15/10/2026):** a 430 s run
used a temporary copy of the save and mounted `SAVEDATA0ShadowProgress0` read-only.
The captures show Wander at the Shrine of Worship, with no obvious visual corruption. Between 351–426 s,
16 windows recorded an average of 24.85 FPS, average p50 of 41.47 ms, average p95 of 44.91 ms,
average p99 of 50.11 ms, and worst frame of 51.17 ms. Sampling profiler and counters were enabled
for this run; it is useful for locating cost, not as an A/B performance test. There were 11,078
`GpuCommandProcessor` samples; inclusive times were `DrawIndirect` 50.2%,
`BindResources` 40.4%, `BindBuffers` 20.4%, `DispatchDirect` 18.9%, and `GetGraphicsPipeline`
15.6%. These values overlap. Workload was about 7,520 commands per frame
(around 676 draws and 1,070 dispatches); downloads/readbacks ranked below draw and state costs
in the profile. No `device lost` or heap-corruption message appeared in the log.
This reinforces that the next target is reducing per-command and resource-binding overhead, without
deferring draws or relaxing precise readbacks. The test remained at mip 2 and in the temple:
it does not prove the original corruption is fixed, nor measure the canyon or combat.

**LTO build A/B (15/10/2026):** two 360 s runs per state, same cvars and attract
benchmark, discarding the first 60 s. Both used `RelWithDebInfo` with PGO disabled; the only
factor was `BB_LTO=OFF` versus `ON`, compiled in a separate `out/gpu-lto`.
Without LTO, averages were 55.84 and 55.93 FPS, with 37/60 windows at 50 FPS or higher in both.
With LTO, they were 56.45 and 56.37 FPS (average gain ~0.53 FPS, ~0.9%), with 41/60 and 38/60
windows at 50 FPS or higher; windows at 60 FPS were 16–17/60 in both states. Average p95
per window was essentially unchanged (26.25–26.28 ms without LTO; 26.20–26.39 ms with LTO).
At the canyon bridge, around 185 s, runs were at 24.1–24.9 FPS without LTO and
24.8–24.9 with LTO. Thus, LTO provides a small throughput gain, not a solution for
stable frame times or 50–60 FPS in the demanding section; it does not justify changing the renderer.
The LTO-run captures show no obvious visual corruption.

**PGO A/B focused on the canyon bridge (15/10/2026):** the training profile was collected in the
reproducible title-screen demo; the capture confirms the bridge crossing. Training instrumentation
reduced performance, so its FPS are not used as measurements. The Clang LLVM profile
was merged into `default.profdata`; the `RelWithDebInfo`, LTO+PGO build was compiled in
`out/gpu-pgo-use` without changing the base binary or packaged installation.

Two complete 360 s runs with PGO-use, using the same cvars and discarding the first 60 s,
recorded average rates of 59.1 and 59.4 FPS; 42/60 and 43/60 windows at 50 FPS or higher, and 20/60 and
21/60 at 60 FPS or higher. The reference LTO build without PGO recorded 56.37 FPS, 38/60 windows at
50 FPS, and 16/60 at 60 FPS. Average p95 per window fell from 26.20 ms to 25.1 and 24.6 ms. The
overall average gain is real, but does not amount to a stable 50 FPS: the worst frames in the PGO runs
were 275.4 and 266.6 ms. The harness ended each run at the 360 s time limit (`rc=1`); this
was not treated as a normal game crash.

In the demanding crossing, using the 5 s windows at 185/190/195/200 s, LTO without PGO recorded
24.8/26.1/27.5/31.0 FPS. LTO+PGO recorded 27.0/28.5/30.7/33.7 in the first run and
28.1/28.9/30.2/34.2 in the second: an average of 30.16 versus 27.35 FPS, about a **repeated 10%
gain** at the bridge. Workload was comparable (about 8,500 commands/frame at 185 s).
The average p50 of these four windows fell from 37.40 ms to 33.31 and 33.07 ms; average p95
from 42.11 ms to 38.21 and 39.71 ms. Even so, none of these intervals reached 50 FPS.
There was an isolated 108 ms spike in the first PGO run (185 s window); in the repeat, the
worst frame in the bridge window was 42.85 ms, but there were spikes of 75.46 ms during the
180 s transition and 100.05 ms at 205 s. PGO is promising for this scene, but does not prove global stability.
Captures from both runs show the bridge with no obvious visual corruption; logs contain no `device lost`
or heap-corruption message. The streaming limit remained at mip 2.

This work also fixed Clang PGO integration: the periodic writer uses
`__llvm_profile_write_file`, and Clang’s `BB_PGO=use` consumes
`out/pgo-sotc/default.profdata` (merged with `llvm-profdata`). GCC’s training/use flags
remain on the separate gcov path. Without the Clang writer, the binary only announced the write
without producing a profile; without conditional flags, Clang rejected GCC-specific options.
The ablation tests do not justify disabling caches or the fast path: `SOTC_FIND_MEMO=0`
degraded FPS in comparable windows, and the process later terminated with a game-read failure
(with insufficient evidence to attribute causality to the toggle);
`SOTC_INDIRECT_DIRECT=0` reduced the average and left the bridge at 22–24 FPS. These paths
were left unchanged.

**Small per-draw buffer A/B (16/10/2026):** the `ObtainBuffer` path that copies small
read-only buffers to the stream buffer was measured with thresholds of 128 B (two runs),
256 B (two), 4 KiB, and 16 KiB, against two references with threshold 0. Each run used 240 s of the same
demo/patch; I compared the 60–220 s and 185–220 s bridge windows. At 128 B, the bridge average was
37.75 FPS versus 37.22 in the references (+0.54 FPS, ~1.4%); average p95 per window was
26.13 ms versus 26.46 ms. The total number of windows >=50 FPS between 60–220 s was 41/66 versus 37/66,
but no bridge window from 185–220 s reached 50 FPS. The worst frame remained above
267 ms, so this does not solve stability. At 256 B, the bridge gain did not repeat; 4 KiB and 16 KiB were worse.
The two 128 B runs showed no `device lost`, heap corruption, or obvious visual difference in the captures.

**Revalidation in the active checkout (16/10/2026):** I repeated 240 s with the same binary and isolated
save copies, alternating the candidate 128 B default with `SOTC_STREAM_THRESHOLD=0`.
Between 60–220 s, the 128 B run recorded 27.14 FPS and average p95 of 42.52 ms; the 0 run recorded
27.39 FPS and 42.51 ms. At the bridge (185–220 s), results were 21.36 versus 22.13 FPS and p95 of
62.61 versus 62.71 ms. The 128 B result did not repeat and did not measurably improve frame time;
both runs also had spikes above 1 s. Therefore, **I did not adopt 128 B as the default**: the value remains
`0`, and the override is still available for further measurements. Bridge captures looked similar,
with no obvious corruption. This A/B fell short of stable 50 FPS.

## In-place game memory: prototype (08/10/2026)

The original bbport (deadinside28, version 0.3) has a "PC memory model"
(`BB_GUEST_IN_PLACE`): the GPU uses game memory in place, and EOP labels are written
when the GPU has actually finished. This eliminates copies to VRAM, page protection, and readbacks.
Measured in Bloodborne: 95 → 115–124 FPS in demanding scenes. There it uses dma-buf,
which is available only on Linux and works only on AMD.

On Windows, the path is `VK_EXT_external_memory_host`. The `tools/host_import_test.c` test maps
a pagefile section the same way `src/runtime_memory.c` does (a "backing" view and the game’s view
over a placeholder) and imports the "backing" view. Results on the RTX 2060 SUPER:

| Size | Import | GPU writes → CPU sees | CPU writes → GPU reads | GPU reads imported memory |
|---|---|---|---|---|
| 256 MiB | yes (50 ms) | yes | yes | 5,4 GB/s |
| 2 GiB | yes (460 ms) | yes | yes | 5,7 GB/s |
| 5 GiB | yes (1,5 s) | yes | yes | 5,5 GB/s |

The memory type is host-coherent, uncached on the CPU, and has a minimum alignment of 4 KiB. VRAM-to-VRAM
copy bandwidth was 65–184 GB/s. Therefore, textures and geometry remain in VRAM; the benefit is for
data written by the GPU and read by the CPU (which currently requires precise readbacks), and for labels.

**Proposed integration, in measurable stages:**

1. Import the entire "backing" view at startup as a `VkDeviceMemory`.
2. Make readbacks copy directly from VRAM to game memory, without staging or memcpy.
   The risk is low and the gain is small, but it validates the component.
3. Move the regions that become "hot readbacks" (those the game rereads
   every frame) to imported memory. The GPU then writes to them directly, without page protection or readback.
4. "Honest" EOP labels (written after GPU completion) for these regions. This makes the game’s own
   synchronization sufficient.
