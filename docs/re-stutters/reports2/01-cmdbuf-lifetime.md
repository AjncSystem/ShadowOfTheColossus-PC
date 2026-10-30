# 01 - Command buffer memory lifetime and the SOTC_DROP_FAST crash

Read only. No repository file edited except this report. Addresses are eboot VAs unless marked
port. Labels: **observed** (disassembly, source line or log), **inferred**, **unknown**.

## 1. Answer

- **observed:** the game builds DCB/CCB in `RenderThread::SubmitJob` (0xc5560) and submits them in
  `RenderThread::KickoffSubmitJob` (0xd07c0). The chain is `sub_91f80` -> `GfxCmdList::Submit`
  (0x8636e0) -> thunk 0x882ac0 -> `sceGnmSubmitCommandBuffersForWorkload`. Only the final submit
  calls thunk 0x882ce0 -> `sceGnmSubmitDone`.
- **observed:** the port's guard for SOTC_DROP_FAST (`Liverpool::CmdRanges`, liverpool.h ~350-410)
  records DCB, CCB and ACB spans at submit time and nested IBs while they are parsed
  (`CmdRangeGuard`, liverpool.cpp 3076, 4020, 4223). A fast drop over a recorded range takes the
  waiting path instead.
- **observed:** the guard does not cover a nested IB between submit and parse. An IB's range is
  recorded only when the parser reaches its IB packet. Until then a fast drop over that IB sees no
  overlap and unmaps it. The CP then reads zeroed memory and stops on "Unimplemented PM4 type 0"
  (liverpool.cpp 4076 path). This is the most likely cause of the crash. **inferred** (the crash
  itself was not reproduced here).
- **observed:** the waiting drop does not wait for the command processor. `DropGpuModified` sends
  the drop with `SendCommand<true>` (buffer_cache.cpp ~628). `command_queue` runs on the draw
  thread (`ProcessCommands`, liverpool.cpp 2602-2620). The CP only runs `ordered_queue`
  (2621+). So the default wait is a draw-thread drain, not a CP-parse drain. The fast path removes
  that drain only.
- **observed (independent of the flag):** `direct_release` (src/runtime_memory.c ~1114-1142)
  calls `host_discard` (memset to zero, or punch hole on Linux) before `flush_hooks()`. The GPU
  unmap hook, and so any wait, runs after the bytes are already zero. A released range that a CP
  still reads is zeroed in both modes. This is a port bug to fix regardless of SOTC_DROP_FAST.
- **inferred:** the game's own fences are the real protection. A CB is reused only after an EOP
  label or flip that follows its work. In the port the draw thread writes that label in stream
  order, so the label cannot pass before the CP parsed the DCB that contains it. The gap is
  memory the CP reaches through a nested IB or chunk that the game believes is free after an
  earlier label.
- **unknown:** which memory the crashed session unmapped. The port logs `sotc: bad PM4 packet at
  ...; in a fast-dropped range: yes/no` (liverpool.cpp 2906-2911). Its "no" means the cause is
  outside the fast drop ring.
- **Recommended first fix (port, no guest hook):** register IB ranges at submit time, by walking
  the DCB/CCB/ACB for IndirectBuffer packets (opcode 0x3F, base and `ib_size`, recursive with a
  depth and size cap). Then `CmdRangeInUse` sees every range the CP will read. Fallback: while any
  submit is queued (`queue.submits` non-empty), take the waiting path. That is conservative and
  will cost most of the gain during loads, when the CP is busy.
- **Recommended second fix (port, real bug):** in `direct_release`, run the GPU unmap hook before
  `host_discard`, or defer the discard until the hook has returned.
- Gain: the CHANGELOG measures the fast path at worst frame 417 -> 125-185 ms and memory-release
  waits ~9.3 s -> 25-44 ms per session. These numbers are from the existing logs. The gain from
  the fixes above is **unknown**: it depends on how often the walk finds IBs and how long the
  waiting path is kept.

## 2. Evidence

### 2.1 Guest submit path (observed)

| Step | Address | Evidence |
|---|---|---|
| `RenderThread::SubmitJob` allocates a 0x610-byte params block (`sub_17170`), stores DCB vector at +0x210..0x218 and CCB vector at +0x410..0x418 | 0xc5560 | `dis`; report 06 |
| `KickoffSubmitJob`: waits job counters (`JobSystem_WaitForCounter`, 0x35590), then `sub_323d0`, then per DCB `sub_91f80` (ring doorbell only when mode 1, `sub_91f90` -> `sub_863aa0`), then `sub_882ce0` | 0xd07c0-0xd094b | `dis`, `callees`, `callers` |
| `sub_91f80` = `jmp GfxCmdList::Submit` (unique signature `48 8b 3f 89 f6 e9 ?? ??`) | 0x91f80 | `sig` |
| `GfxCmdList::Submit` (not unique in 24 bytes) calls `Gnmx_ComputeQueue_TrySubmit` and thunks | 0x8636e0 | `callees` |
| `GfxCmdList::SubmitThunk` = `jmp sceGnmSubmitCommandBuffersForWorkload` | 0x882ac0 | `dis` |
| `GfxCmdList::SubmitDoneThunk` = `jmp sceGnmSubmitDone` | 0x882ce0 | `dis` |
| `sceGnmSubmit*` have no direct eboot callers (`icallers` returns 0); they are reached through the thunks above | - | `icallers` |

The eboot does not call `sceKernelMunmap` or `sceKernelReleaseDirectMemory` directly (`icallers`
empty). The calls come through a linked module (the BPE `PlatformPageAllocator` at
`bpe_PlatformPageAllocator_Init` calls `sceKernelMapDirectMemory`). Which module frees, and when,
is **unknown**.

### 2.2 Port submit and guard (observed)

- `sceGnmSubmitCommandBuffersForWorkload` (gnmdriver.cpp ~2430-2466): `GateWait(0)`, then
  `PerformSubmit` -> `Liverpool::SubmitGfx(dcb, ccb)`.
- `Liverpool::SubmitGfx` (liverpool.cpp 4449-4475): `ProcessGraphics` builds a coroutine (not run
  yet), then `cmd_ranges.Add` for DCB and CCB, then pushes `submits` and `submit_ranges` under
  `queue.m_access`.
- `Liverpool::SubmitAsc` (4477-4505): same for the ACB slice. The ring copy (`SOTC_ASC_RPTR=2`) is
  off by default.
- Task end (liverpool.cpp ~2770-2785): pops `submits` and `submit_ranges` and removes the ranges.
  The two queues stay in step.
- IB parse (3076, 4020, 4223): `CmdRangeGuard` holds `[ib_base, ib_size*4)` for the duration of
  the nested task. Coverage is complete for the three IB opcodes, but only from the parse point.
- Default copy mode is off (`BB_COPY_GPU_BUFFERS`, emulator_settings.h:58). The CP reads guest
  memory directly, so the guest range is what matters.
- ASC ring read pointer (default `SOTC_ASC_RPTR` mode): the CP advances `*read_addr` as it parses
  (liverpool.cpp 4184-4199, 4238, 4556). The guest's ring-full test (eboot 0x85d3d0) therefore
  sees real parse progress. **observed**. This path is sound.

### 2.3 Drop path (observed)

- `DropGpuModified` (buffer_cache.cpp 545-640). With `SOTC_DROP_FAST=1`: no GPU ranges, then
  `CmdRangeInUse` check (`drop_fast_guarded`), then `MarkCpuModifiedUnlessGpu`. Otherwise waits
  with `SendCommand<true>(drop)` (see 1, the drain is on the draw thread).
- `direct_unmap` (runtime_memory.c 1104-1112): calls `hook_unmap` **before** `unmap_locked`, so the
  GPU drop, and its wait, come first. The guest then unmaps.
- `direct_release` (1114-1142): `unmap_locked` per VMA (queues the GPU hook), then the block
  bookkeeping, then `host_discard(start,size)` (memset to zero, line 856-859 on Windows), then
  `flush_hooks()` runs the GPU hook. **The zero comes before the hook.** Same for `flex_free`
  (line 901: `host_discard` with no hook at all).

### 2.4 Counters and the crash signature (observed)

- CHANGELOG (Em desenvolvimento, lines 4-13): fast drop measured 29/10. 1 of 4 sessions closed
  during save load: the CP read a zeroed command buffer. The guard was then added; it did not fire
  in the next sessions. Cause not confirmed.
- Port log lines (driver.cpp ~1815): `SOTC drop fast %llu, waited %llu (%llu ... over a command
  buffer in use)`.
- `NoteBadPacket` (liverpool.cpp 2906-2911): `sotc: bad PM4 packet at <addr>; in a fast-dropped
  range: yes, N drop(s) ago | no`. This is the diagnostic for the next crash.

### 2.5 Game-side reuse (inferred)

- report 06 and this round: the game writes DCB/CCB through `sub_86xxxx` wrappers with a refill
  callback, into per-frame `PerFrameRenderContexts::RequestNewRenderContext` (sub_90470,
  two sizes, probably command plus data). The release path is **unknown**.
- report 02 open question 3 is still open: whether a game thread ever waits on a label before it
  reuses or frees a CB. The port's `SOTC_CP_AHEAD=2` note (gnmdriver.cpp 146-160) states that the
  game's CPU writes before its first submit still race the draw thread, and that only the game's
  own fence orders them.
- `sceGnmSubmitDone` (gnmdriver.cpp ~2480-2500): with `SOTC_CP_AHEAD=2` and no copy buffers, no
  wait for parse. The default path waits for GPU idle. Both leave the guest free to reuse a DCB
  after the label.

## 3. Hook proposals

The guest hooks below are candidates. The port-side fixes in section 1 need no hook and are
verified by counters and the `NoteBadPacket` log.

### H1. Port: IB ranges at submit time (preferred; not a guest hook)

- **Site:** `Liverpool::SubmitGfx` and `Liverpool::SubmitAsc`, before `cmd_ranges.Add`
  (liverpool.cpp 4449, 4477). Walk each span for type-3 IndirectBuffer packets (opcode 0x3F;
  `ibase_lo`, `ibase_hi`, `ib_size` in `PM4CmdIndirectBuffer`, pm4_cmds.h 922) and add their
  `[base, base + ib_size*4)` ranges. Recurse with depth 4 and a total cap, for example 64 KB of
  walked dwords. Above the cap, fall back to the waiting path (`drop_slow`).
- **Mode:** not a hook. It is a port change under `SOTC_DROP_FAST`.
- **What it changes:** the guard sees every IB before the CP reaches it.
- **Why it shortens a hitch:** unchanged for the common case, where the guard already covers the
  top-level CBs. It only affects the rare drops with an unparsed IB inside.
- **What could break:** walk cost on the guest thread (bounded by the cap). A walk that misreads
  a packet adds a false range (only the waiting path is taken, so the hitch returns but nothing
  crashes). A missed range is the crash, so the walk must cover the chain IB too.
- **Verify:** add a counter for ranges added by the walk and for drops that the walk turned into
  a wait. Run a save-load route with `SOTC_DROP_FAST=1 SOTC_FPS=1`. The `=2` check mode:
  with `SOTC_DROP_FAST_CHECK=2`, still take the fast drop but also record whether any IB reached by
  the parser later overlapped a fast-dropped range (the existing `NoteBadPacket` ring, extended to
  IB reads). Expected result: zero overlaps.
- **Signature:** not applicable (port code).
- **Expected gain:** unknown. Upper bound is the fast-path gain already measured (worst frame
  417 -> 125-185 ms, waits ~9 s -> 25-44 ms per session). Most of it should survive, because
  the walk is bounded and mostly finds only IBs that are already covered.

### H2. Port: run the GPU unmap hook before the zeroing in `direct_release`

- **Site:** `direct_release`, src/runtime_memory.c 1114-1142. Move `host_discard(start,size)` to
  after `flush_hooks()`, or queue the discard as a pending hook after the unmap hook. `flex_free`
  (line 901) needs the same ordering.
- **Mode:** not a hook. It is a port ordering fix. It applies in both modes.
- **What it changes:** the GPU sees the unmap, and its drop, before the bytes are cleared. In
  the default mode this makes the existing wait protect the release path too.
- **Why it shortens a hitch:** it does not shorten a hitch by itself. It removes a silent
  corruption that could also cause the crash, so it is a correctness fix.
- **What could break:** the wait on the release path is taken with the write lock released.
  The hook and the discard need the same order as `direct_unmap`. Deadlock risk if `flush_hooks`
  is called with the lock held (it is not, today).
- **Verify:** with `SOTC_DROP_FAST` off, count zeroes of ranges that have a CB in use
  (`CmdRangeInUse` at discard time). Expected: zero after the fix, non-zero before it.
- **Signature:** not applicable.
- **Expected gain:** unknown, probably small. It is the most likely true bug.

### H3. Guest Entry hook on `RenderThread::KickoffSubmitJob` (observation only)

- **Site:** entry of 0xd07c0. Unique signature: not found in 24 bytes (41 matches). Anchor it
  to `RenderThread::SubmitJob` (0xc5560, the call site at 0xc58f4) or use a longer pattern.
  Not verified.
- **Mode:** Entry, read-only (returns Continue). Logs the DCB and CCB vector pointers at +0x210 and
  +0x410 of the submit params, and the frame counter.
- **What it changes:** nothing. It would tie each submit to the frame and to the later
  `NoteBadPacket` address.
- **Why it helps:** it answers the open question of which frame's CB the CP read. It does not
  remove a hitch.
- **What could break:** the anchor is wrong and the hook is off (safe, by design of HOOK-DESIGN).
- **Verify:** `=2` mode (log only) and compare with the CP's submit log.
- **Expected gain:** none; diagnostic.

## 4. Open questions

1. Does a guest thread submit a DCB whose IB target it unmaps in the same frame? A walk over the
   submitted DCBs (H1) answers this in one run: count IB targets outside any live submit.
2. Which module frees the game's CB and IB chunks, and under which label? The release path is
   reached through the linked BPE allocator, not directly from the eboot. Find the caller of
   `sceKernelReleaseDirectMemory` (the NID is `MBuItvba6z8`, import_names.inc line 561) with a
   Gnm-style dispatch or a runtime trace.
3. Does `SOTC_CP_AHEAD=1` (default off) make the crash more likely? It lets the CP lag the draw
   thread, so an unparsed IB can outlive a label.
4. Is the 1-in-4 crash rate tied to the save load only because the game releases chunks there?
   That needs a log of `direct_release` calls with overlap counts.
5. Is `CmdRanges::Remove` sound when a task is destroyed on a path other than normal completion
   (for example a fault or a stopped task)? Only the normal path is visible in the code read.

## 5. Ranked summary (ten lines)

1. H2 first: move the zeroing in `direct_release` after the GPU unmap hook (real bug, both modes).
2. H1: walk submitted DCB/CCB/ACB for IB packets at submit and register their ranges.
3. Keep `NoteBadPacket`; a crash with "no" points away from the fast drop.
4. Log `direct_release` overlaps with live CBs before H1, to measure how often the bug fires.
5. H1 fallback: when the walk exceeds its cap, take the waiting path for that drop.
6. Verify H1/H2 with a save-load route, `SOTC_FPS=1`, and the `drop_fast_guarded` counter.
7. H3 (Entry, log only) to tie submits to frames; diagnostic, no gain.
8. Gain of H1 and H2: unknown; the fast-path gain is measured at 417 -> 125-185 ms worst frame.
9. Unknown: which module frees CB memory and when (report 02 open question 3 still applies).
10. Do not enable `SOTC_DROP_FAST` by default until H1 and H2 are in and a crash-free run is logged.
