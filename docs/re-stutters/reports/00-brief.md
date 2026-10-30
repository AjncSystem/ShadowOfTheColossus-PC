# Brief: reverse engineering the stutters of Shadow of the Colossus (PC port)

## The project

`this repository` (branch `re/stutter-architecture`) is a native Windows port of
the PS4 game Shadow of the Colossus (Bluepoint engine, "BPE"). The game's own x86-64 code runs
directly on the PC CPU; a C loader/runtime implements the PS4 system functions it imports; a
vendored shadPS4 GPU core translates the PS4 GPU (GCN PM4 command buffers + shaders) to Vulkan.
Read `CLAUDE.md` in the project root first (architecture section).

## The problem

The game still stutters (frames of 60-500 ms), mostly when streaming new areas and in heavy
scenes. We want the root causes **inside the game's engine**, and a design where the port hooks
or replaces engine behaviour so the stutters cannot happen. Nothing is off the table: changing
how the engine works, including moving from the PS4's **unified memory (UMA, CPU and GPU share
one memory, the game reads GPU results and writes GPU data directly)** to a model native to PCs
(discrete GPU memory, explicit uploads/readbacks, frames pipelined ahead).

## What is already known (do not re-derive; build on it)

- Port pipeline: guest "Game Render" thread submits PM4 command buffers -> port's command
  processor (CP) thread parses them -> back-op ring -> ONE serial draw thread
  (`Rasterizer::Draw/DrawIndirect/Dispatch*`) -> recorder thread -> Vulkan queue.
- Guest memory is mirrored for the GPU. The port tracks guest writes to GPU-used memory with
  page protection: ~11-13k guest write faults per 5 s. GPU-written data goes back to guest
  memory through readbacks: ~700-900 per second.
- Draw thread: ~18 ms CPU per frame (median), ~4,400 ops/frame, ~5 us/op; binding ~36% of it.
  The port is CPU-bound on that thread; the GPU is ~25% busy.
- Earlier static RE found: **the XPR visibility/occlusion cull is a same-frame GPU->CPU round
  trip**: the GPU culls, signals a CS_DONE label, then the CPU reads ~400 visibility bitsets
  per heavy frame. The port pays it through faults, readbacks and the serial draw thread.
  Strings: "XPR Occluder Debug Mode", "EXPROccluderDebugMode::*", "XPR_OccluderDepth".
- Other known engine facts: a per-frame heap "RenderSingleFrameGPU" (32 MiB chunk);
  `ParticleSystemPool` writes; a shadow cull compute shader 0x4defcf7f (replayed on CPU by
  `SOTC_HLE_SHADOW_CULL`); red-zone functions 0xEDFB30, 0xF67D2B, 0xF52530 (they keep pointers
  below rsp while reading/writing tracked memory); the BPE heap guard protects the game
  allocator's free-tree links (crash at guest offset 0x49800 when a readback overwrote one).
- Guest threads seen in logs: "Game Render", "RenderFlip", "Shadow Game Update",
  "Resource Loading", "BPE JobWorkerThread CPU1..4", "SceFios*", "sndx_*", "Save Task Thread".
  Logs show waits like `event flag 'PS4SyncEvent' waited 5s`.
- Stall lines in logs look like:
  `SOTCSTALL frame 157 201 ms: pipelines 0, shaders 4 (8 ms), images 705 (7 ms), readbacks 13;
  cpu draw 120.4 cp 125.0 rec 41.2 upload 208.1 ms; ... faults 14`
- 0.70 (upstream) fixed area-change drops caused by: the guest memory table lock, view remaps,
  region-cache misses, heap-guard walks, and too many new textures in one frame (now a 32 MB
  per-frame texture upload budget).

Further reading, as needed (all in the project):
- `docs/ARCHITECTURE-TARGET.md`, `docs/BIND-PATH-DESIGN.md`, `docs/sotc/PLANO-THREAD-DESENHO.md`,
  `docs/sotc/MODELO-MEMORIA-PC.md`, `docs/sotc/CAMINHO-B.md`, `docs/sotc/REVISAO-DO-PROJETO.md`.
- `CHANGELOG.md` (Portuguese; the "Detalhes técnicos" block has every measurement of the
  stutter work) and `git log --format='%h %s%n%b' upstream/main..HEAD`.
- Port source: `src/` (loader/runtime, `runtime_memory.c`), `gpu/shadps4/video_core/`
  (`amdgpu/liverpool.cpp` CP, `buffer_cache/`, `texture_cache/`, `renderer_vulkan/`),
  `gpu/shadps4/core/libraries/` (gnmdriver, videoout).
- Logs: `user/last_run*.log` (dev copy) and `<install>\user\last_run*.log`
  (the user's install, plain defaults); `user/events*.log`; trace dumps under `user/trace/`
  if any.

## Tools for the eboot (the game executable)

The executable is `out/eboot.elf` (PS4 ET_SCE_EXEC, no sections, no symbols; code 0x0-0x1593978,
read-only data from 0x1594000, data from 0x1604000). Addresses below are eboot virtual
addresses = the "guest offsets" in the port's logs.

`python tools/re/eboot_re.py <cmd>` (run from the project root; the index is already built):
- `func ADDR` - the function containing ADDR (function ranges come from the eh_frame table)
- `dis ADDR [-n N]` - disassembly of that function, annotated with call names, import names
  and referenced strings
- `callers ADDR [-d DEPTH]`, `callees ADDR`
- `str REGEX [-l LIMIT]` - strings (109k) and the functions that use them. The engine has many
  debug/cvar/class-name strings: use them as anchors.
- `refs ADDR [-r RANGE]` - code referencing data at ADDR
- `imports [REGEX]` - PS4 system imports (sceGnm*, sceKernel*, sceVideoOut*, ...) and how many
  functions call each; `icallers NAME` - the functions calling one.
- `fstr ADDR` - strings used by a function.
Functions are named `sub_<hex>`; imports `import:<symbol>`. Indirect (virtual) calls are not in
the call graph: follow vtables through `refs` and disassembly when needed.

**Ghidra pseudocode** is being exported in the background to `out/re/decomp/<hhhh>/<addr>.c`
(one C file per function, first line lists its callers). It may be partial or absent while you
work: if files exist, `grep -rl` them (PLT stubs are named `import_<symbol>`); do not wait for it.
Also available: `capstone` in Python, `C:\msys64\clang64\bin\llvm-objdump.exe`.

## Rules

- **Read-only.** Do not edit project files, do not build, do not run the game or the launcher,
  do not run git commands that change state. Do not edit `tools/re/names.txt`.
- Write ONLY your report file (path given in your task). English.
- Every claim cites evidence: function addresses, strings, imports, disassembly lines, log
  lines, source file:line. Mark each finding as **observed** (seen in code/logs), **inferred**
  (reasoned from observations) or **unknown**. Do not invent addresses or names.
- Stay inside your topic; note in one line things you saw that belong to another topic.

## Report format (your file)

1. **Answer** - 5-10 lines: what you found about your question.
2. **Evidence** - the engine functions/data involved (address, proposed name, what it does,
   how you know), call chains, log numbers.
3. **Stutter mechanism** - how this produces frame spikes on the PC port (observed vs inferred).
4. **Hook / redesign options** - concrete: which function or data to hook/replace, what the
   port would do instead, expected effect, risk, how to verify. Include the radical option
   (changing how the engine works) when it applies.
5. **Proposed names** - `0xADDR name` lines for functions you identified.
6. **Open questions** - what you could not settle and how to settle it.
Keep it under ~1,800 words. Your final reply to the lead: 5 lines + the report path.
