# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Native Windows port of PS4 *Shadow of the Colossus* (CUSA08809 EU 1.01 and CUSA08034 US 1.00, the same
eboot build; GPU workarounds gate on `ElfInfo::IsSotc()`, never on one serial). Not an emulator: the game's
own x86-64 code runs directly on the host CPU, a C loader/runtime implements the PS4 system functions
it imports, and a vendored shadPS4 video core translates the PS4 GPU (GCN command streams + shaders)
to Vulkan/SPIR-V. Forked from bloodborne_pc ("bbport"): engine names (`bb-probe`, `bbgpu`, `bb_setjmp`,
`BB_*` env vars, `bbport_*` sources) are kept from it; the Bloodborne-only files were removed. The
Shadow of the Colossus path is `sotc.py` / `launcher/sotc_launcher_win.py` / `packaging/windows/package_sotc.sh`.

No game files or Sony firmware are in the repo, and none may be added: packaging refuses any
`.sprx`/`.prx`. Running the game needs a local dump (`SOTC_GAME_DIR`, default `../CUSA08809`) and,
for sound, the user's own `libSceNgs2.sprx`/`libSceUlt.sprx` (`SOTC_SYS_MODULES`).

User-facing docs (README, CHANGELOG, `docs/sotc/*`) are written in Portuguese (README also has
English). Code comments are English.

## Build / run / test

Toolchain: MSYS2 **CLANG64** (packages listed in `packaging/windows/README.md`). `msys2-env.sh`
puts `$MSYS2_ROOT` (default `C:/msys64`; some tools default to `/i/msys64`) first on PATH.

```bash
bash build.sh                            # CMake+Ninja in out/gpu -> out/bb-probe.exe, bb-gpu-capabilities.exe
                                         # build errors: out/gpu-build.log. ThinLTO by default (BB_LTO=OFF disables);
                                         # Clang PGO from pgo/bbgpu.profdata when present (BB_PGO=off skips it)
bash tools/pgo_train.sh                  # instrumented build + attract/shrine training -> pgo/bbgpu.profdata, rebuild
ninja -C out/gpu bb-probe && cp out/gpu/bb-probe.exe out/   # incremental rebuild after first build.sh
python sotc.py                           # prepare image + run game (settings: env or sotc.ini, see sotc.py docstring)
python launcher/sotc_launcher_win.py     # Tkinter launcher (settings in %APPDATA%/sotc-launcher/settings.json)
bash packaging/windows/package_sotc.sh   # dist/SotC-PC.zip; needs WINPYTHON with PyInstaller + Pillow
```

`build.sh` inits submodules (`gpu/third_party/*`, `third_party/LibAtrac9`) on first run.

Tests:
- Python (scripts/): `python -m unittest discover -s tests` — single file/test:
  `python -m unittest discover -s tests -p test_content.py -k <name>`. Some skip unless
  `bash build.sh --test` built the C test binaries first.
- C tests (`bash build.sh --test`) only build on the Linux path; several use Linux-only APIs.
- GPU C++ tests are `EXCLUDE_FROM_ALL` CMake targets, e.g. `ninja -C out/gpu taa-shader-test`
  (see bottom of `gpu/CMakeLists.txt`).

Dev/bench helpers in `tools/` (bash, CLANG64): `sotc_run.sh [seconds] [log]` (rebuild + timed run),
`sotc_bench_attract.sh` (title-screen attract demo FPS benchmark, used for numbers in CHANGELOG),
`sotc_play.py`, `sotc_hangtest.py`, `sotc_bench_shrine.sh` (idle Shrine of Worship scene, ~5800 draws/frame;
needs `out/save-template/CUSA08809` (or `CUSA08034`), a save after colossus 1 whose profile has the language set; the attract
benchmark restores the same template and sits on the language menu without it). Profiling: `SOTC_PROFILE=<thread
name part>` (e.g. `GpuCommandProcessor`, sampler in `src/probe.c`) with `SOTC_PROFILE_OUT=<file>`, then
`tools/profile_functions.py <file> out/gpu/bb-probe.exe 60 --inlines --qualified`. `tools/rdoc/` are RenderDoc
Python scripts; RenderDoc capture via `SOTC_WRAP` (JSON command prefix, see `sotc.py`) + F12.

## Performance work

- **Which thread limits.** The GPU side runs as a pipeline: CP (`shadPS4:GpuCommandProcessor`, parses PM4 and
  captures back ops) → back-op ring → `shadPS4:GpuDraw` (executes ops: binds, uploads, readbacks) → recorder
  thread (Vulkan calls). With the draw-thread fast paths in place, the heavy scenes (temple surroundings,
  >= 7000 cmds/frame) are limited by `GpuDraw` (~93% busy), not by the CP (~55% own work) or the GPU (~50%).
  Only work removed from the limiting thread moves FPS; `SOTC_FPS=1` logs per-thread busy time and CP own work.
- **BbToggle bits** (`gpu/shim/bbport_toggles.h`; default mask in `src/runtime_memory.c`): every optimization
  sits behind a bit, and a set bit means the old path. `BB_TOGGLE_FILE=<file>` holding a mask is re-read live,
  which allows in-run A/B (flip the mask in ABBA/BAAB order and compare windows by cmds/frame). New fast paths
  come with a `SOTC_<NAME>_VERIFY=1` env that also runs the slow path and counts mismatches; it must report 0.
  Bits added by the performance work: 20 `CpFastPaths` (off by default, `SOTC_CP_FAST=1`), 21 `BpeGuardDiff`,
  22 `ReadbackIntervals`, 23 `MicroCostSweep`, 24 `WriteMarkMemo`, 25 `ColdImageSweep`, 26 `ArenaImageRelease`,
  27 `StagingScale`, 28 `DepthFastPath`, 29 `GBufferFastPath`, 31 `TexBindFastPath`, 40 `DispatchFastPath` (off
  by default, `SOTC_DISPFAST=1`), 41 `DrawReplay` (`SOTC_DRAWREPLAY=0` disables), 56 `BufferBindMemo`, 58
  `PipelineKeyMemo`, 59 `GcLruAdvance`, 60 `DrawFixedCostOpt`, 61 `LowLatencyWait`, 62 `CpPollBackoff`, 63
  `DrawBindMemo`.
- **Measuring.** Compare PGO builds only (PGO is worth +15–20%); retrain after GPU-side changes. Run
  `tools/pgo_train.sh` from the MSYS2 bash (`C:/msys64/usr/bin/bash.exe`): started from Git Bash, the `bash` it
  calls can drop `SOTC_GAME_DIR`/`PY`, and llvm-windres needs a Windows `TMP`. Stop OneDrive (both
  `OneDrive.exe` and `OneDrive.Sync.Service.exe`) and other heavy processes during benchmarks. End-to-end runs
  are interleaved (new/old/old/new); a replayed route (`sotc_bench_shrine.py --replay <route>`) diverges between
  runs, so bin its windows by cmds/frame instead of comparing averages.
- Technical notes per change, in Spanish: `docs/sotc/DEPTH-FASTPATH.md`, `GBUFFER-FASTPATH.md`,
  `SIGUIENTE-LIMITE.md` (frame-time breakdown), `DISPATCH-FASTPATH.md`, `DRAW-REPLAY.md`.

## Architecture

**Launch pipeline** (`sotc.py`, mirrored by the launcher's `--run`/`--play`):
1. Offline preparation by `scripts/` (re-run only when the fingerprint of eboot, modules and
   `scripts/*.py` changes; stamp in `out/prepared.txt`):
   - `prepare.py` parses the plaintext SELF; `link_modules.py` links eboot + bundled `sce_module`
     PRXs (+ user's sys modules) into one image with a shared import table → `out/boot-linked.bin` (BBPROBE5).
   - `content_profile.py` → `out/content.bin`; `sotc_cpu.py` → `out/sotc-cpu.bin` (function starts
     from `PT_GNU_EH_FRAME` for SSE4a / red-zone CPU patches).
   - `sotc_modes.py` (every launch) → `out/patches.bin` (60 FPS etc., EU 1.01 / US 1.00 only) and
     `out/cmdline.bin` (Bluepoint engine cvars).
   - Packaged builds: `SotC.exe --script` runs these scripts, so any new stdlib import in a script
     may need a `--hidden-import` in `package_sotc.sh`, and new scripts must be copied there.
2. `bb-probe.exe` (`src/probe.c`) maps the image below 2 GiB (the game stores 31-bit pointers),
   applies CPU patches (`gpu/sotc/sotc_patches.cpp`), binds imports, and jumps to the entry point.

**Import binding**: each imported NID goes to `runtime_resolve()` (`src/runtime.c`), which tries
host implementations in order — the per-subsystem `runtime_*_resolve()` tables in `src/runtime_*.c`
(kernel, memory, threads, fibers, files, savedata, pad, audio/AJM, ...; `runtime_sotc.c` holds
SotC-only imports), then `bbgpu_resolve()` in the GPU library. Unresolved imports fall back to the
native export from the linked PRXs (e.g. libc.prx). Matching is by NID only (library/module suffix
letters differ per executable). To add a missing system function, implement it in the matching
`runtime_*.c` following shadPS4's contract and add it to that file's lookup table.

**GPU library** (`gpu/`, C++, static `libbbgpu.a` on Windows, C API in `gpu/bbgpu.h`):
- `gpu/shadps4/` — vendored shadPS4 (`video_core`, `shader_recompiler`, `common`, parts of `core`);
  commit and backports listed in `gpu/VENDOR.txt`. Local edits are marked with `bbport:` or
  `sotc:` comments — keep marking them. Most rendering bugs (water, AMD tint, garbage textures)
  are fixed in `shader_recompiler` (GCN instruction translation) or `video_core/renderer_vulkan`.
- `gpu/shim/` — glue replacing shadPS4's emulator core: settings (`bbport_settings.cpp`, reads
  `BB_*`/`SOTC_*` env and `BB_CONFIG`), window/SDL, overlay, threads.
- `gpu/sotc/` — game-specific CPU patches and TLS handling.
- `bbgpu_handle_fault` is called first by the loader's exception handler (GPU page tracking).

Windows-vs-Linux loader differences (TLS via TEB slots instead of `fs:`, placeholder-reserved
PS4 address range, vectored exception handler + `bb_setjmp`, fully committed guest stacks) are
documented in `packaging/windows/README.md`; `src/compat_win.c` holds the Windows shims.

**Behaviour switches** are environment variables (`BB_*` inherited from bbport, `SOTC_*` added
here), set by the launcher or `sotc.ini`. Diagnostic ones are listed in `docs/sotc/README-SotC.md`;
newer ones appear in `CHANGELOG.md` entries. Session log goes to `user/last_run.log`.

## Conventions

- Commit subjects are prefixed by area: `shader:`, `shader_recompiler:`, `vulkan:`, `sotc:`,
  `launcher:`, `docs:`, `diagnostics:`, `lod stats:`; releases are `Release X.YY: <summary>`.
- A release bumps `VERSION` in `launcher/sotc_launcher_win.py`, the version/table in `README.md`
  (both languages), and adds a Portuguese `CHANGELOG.md` entry.
- Changes to recompiler/shader output invalidate the user's shader cache; CHANGELOG entries note
  "O cache de shaders é refeito na primeira abertura" when that happens.
- C code: C11, `-Wall -Wextra -Werror` on the Linux build; third-party decoders (LibAtrac9) are
  compiled separately without `-Werror`.
