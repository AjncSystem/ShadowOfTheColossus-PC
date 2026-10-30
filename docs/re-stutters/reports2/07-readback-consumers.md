# 07 - Readback consumers: who reads GPU-written memory, and does it need this frame

Read-only analysis. Nothing built, run or edited. Addresses are eboot VAs (EU 1.01 image, `out/eboot.elf`). Labels: **observed** (disassembly, trace, log or CHANGELOG line), **inferred**, **unknown**.

Data used: 157 slow-frame trace windows in `user/trace/*/*.json` (20,286 read faults, 19,962 at eboot addresses, 324 in host code). Attribution by `tools/re/eboot_re.py func` over the trace `b` field. The sample is biased to slow frames. Script: `scratchpad/waveC/rd_sites.py`. Earlier work: reports 01, 02, 03 and the CHANGELOG readback notes.

## 1. Answer

- Read faults are mostly not GPU data. About 31% (6,298) are BPE heap / allocator metadata and about 6% (1,229) are game-object fields, all in pages the GPU also writes. The CPU owns those bytes, so they need no same-frame GPU data. They need byte-level ownership or a page layout change. A stale copy is wrong for them.
- The XPR visibility family is the largest real GPU consumer: about 8,650 read faults (43%). That is 7,425 in `BPXPR_ConsumeVisibilityMask` (0x1a6d20), plus the range tests 0x1a8a40 and 0x1a8c60 and the callers in the decal, set-build and recorder code.
- ParticleSystemPool::ProcessStats (0x1018c0, read at 0x101a98) is the second real GPU consumer: 3,699 faults (18%). It reads a per-system GPU count from a frame-ring slot. Previous-frame data is the right thing here: the CHANGELOG reports the slot is two frames back, and no CPU wait sits between the dispatch emit and the read (inferred from the code, see 2.3).
- WaterRendering::ReadQueries (0xe48b0) reads one dword from the same kind of ring slot. It is small (5 faults) and has the same previous-frame profile (observed: the CPU does not wait on the GPU, it only emits a GPU-side WAIT_REG_MEM).
- Not found in the sample: auto-exposure, skinning results, picking, indirect argument buffers, GPU timestamps. Zero read faults there. The sample is slow frames only, so this means "absent from slow frames", not "absent".
- The XPR mask is the open question that matters. Report 01 found no CPU wait on the completion label. The CHANGELOG says the game waits on that label, and "o jogo travava na leitura" (the game stalls on the read). The two cannot both be right as stated. Until that is settled, a one-frame-old mask is a guess about what the game does on PS4, not a restoration of it.
- The port's FPS readback counter is mislabelled. "heap" is a subset (`heap_guard_builds`, readbacks into the BPE heap) and "early" counts submits (`early_flushes`). Readbacks per second are `ahead + finish` only (CHANGELOG-era log line: `54/s` = `(23 + 246) / 5`).
- Best next step, before any new hook: a per-consumer readback counter keyed by the faulting guest RIP. The trace already has the RIP per fault, so this costs almost nothing and turns the estimates in section 3 into measurements.
- Ranked by expected gain, if the open questions resolve the way the code suggests: (1) the XPR mask swap (P1), (2) a ring-accessor wrap covering ProcessStats and ReadQueries (P2), (3) heap metadata out of GPU pages (P4, not a readback hook), (4) memcpy-source attribution (P5, measurement).

## 2. Evidence

### 2.1 Read-fault consumers (observed, 157 windows)

Share of 20,286 read faults. "Class" is my classification.

| Function (eboot) | Reads | ms (trace) | Class | Notes |
|---|---:|---:|---|---|
| `BPXPR_ConsumeVisibilityMask` 0x1a6d20 | 7,425 | 12,632 | XPR mask, GPU | CPU walks the bitset to emit draws. Read at 0x1a6d60 |
| `ParticleSystemPool::ProcessStats` 0x1018c0 | 3,699 | 2,043 | GPU count, ring | Read at 0x101a98 |
| `BpeHeap_Walk` 0x48a70 | 1,393 | 935 | heap metadata | Read at 0x48ac5 |
| `BpeHeap_Link` 0x493d0 | 1,278 | 643 | heap metadata | Read at 0x4944d |
| `sub_495e0` | 1,061 | 480 | heap metadata | Read at 0x495fc |
| `sub_179c0` | 991 | 1,110 | heap-like, inferred | `lock cmpxchg` retry loop with a vtable call; report 03 saw writes here too |
| `sub_1a8a40` | 723 | 1,631 | XPR mask, GPU | Range test: "any bit set in [lo, hi)" on the mask at `rsi` (0x1a8b38) |
| `BpeHeap_Find` 0x490a0 | 690 | 703 | heap metadata | |
| `sub_19d410` | 303 | 120 | game object | Reads `[r15+0x4c]` where `r15 = [r13]` (0x19d624), a CPU object. Caller `BPXPR_PreRender` via 0x19d340 |
| `BpeHeap_Alloc` 0x49843 | 303 | 192 | heap metadata | |
| `sub_4a210` / `sub_4a040` | 258 / 118 | 286 / 91 | heap metadata | Inside the BPE heap range |
| `sub_f9c40` | 247 | 411 | game object | 76-byte copy loop: 32-byte blocks from `[object+0xc4]` into a destination (`vmovups`). Source is CPU data |
| `sub_17e90` | 206 | 126 | heap-like, inferred | |
| `sub_1a8c60` | 196 | 102 | XPR mask, GPU | Sibling range test; called from 0x1a8a40 and 0x1a8dd0 |
| `sub_11e190` (`RenderXPRDecals`) | 169 | 355 | XPR-related | Reads `[r13]` (0x11e222). Calls `sub_19c740` -> 0x1a8a40 |
| `sub_937c0` | 127 | 75 | game object | Setter that stores into `[rdi+rax*8+0x1d8]` and tail-calls 0x861520 / 0x860bb0 |
| `sub_79f3a0` | 83 | 109 | game object (AI) | Strings "Wander_Vel", "Agro_Vel" in the function |
| `sub_2512a0` | 78 | 73 | CPU scene tree | `CComponentGeneralOptimizeObjectTree::Render` |
| `Profiler::PushScope` | 71 | 70 | tooling | |
| `BPXPR_BuildSetEntry` 0x19b400 | 95 | 42 | XPR mask, GPU | Reads at 0x19b435 (object-set record) |
| `sub_95580` | 12 (in 0x1019c7 chain) | 46 | object flag | Lazy-init guard: `cmp byte ptr [rbx+8], 0` at 0x955e2. Called from many dispatch and particle paths |
| `sub_8ac710` | 20 | 11 | Oodle decode | Source bytes, not GPU |
| `RenderThread::KickoffSubmitJob` | 21 | 38 | unknown | 0xd07e2 |
| Other (about 30 functions, under 40 faults each) | about 500 | about 1,000 | mixed | Listed in the script output |
| Host code (0x7ffd..., 0x7ff6..., others) | 324 | about 600 | unattributed | 114 at `0x7ffda260e44d`, probably `guest_memcpy` reading its source (inferred) |

Subtotals: XPR family about 8,650 (43%); ProcessStats 3,699 (18%); allocator metadata about 6,300 (31%); game objects about 1,230 (6%); WaterReadQueries 5; host 324 (2%).

### 2.2 XPR family (see report 01 for the mask flow)

- **Observed:** `BPXPR_ConsumeVisibilityMask` 0x1a6d20 is unique with a 20-byte signature (`55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 68 49 89 f9`). It is already declared as the observe-only probe `XPR_CONSUME_MASK` in `scripts/hooks.d/xpr.py`.
- **Observed:** `sub_1a8c60` has a unique 24-byte signature. `sub_1a8a40` does not (two matches in 24 bytes); it needs a longer pattern or an anchor on its callers `sub_19c740` / `sub_11e190`.
- **Observed:** the mask-and-draw-list buffers are chosen by a config byte (`[cfg+0x1c]`) in `BPXPR_BuildSetDraws` 0x19b760, not by a frame parity. The buffers live in the per-frame context (`[ctx+0x21e8]` / `[ctx+0x2238]`, see report 01).
- **Unknown:** whether the game waits for the completion label (EOP 0x28 at 0x925c0) before the read at 0x1a6d60. Report 01 found no such wait. The CHANGELOG (section on `SOTC_RB_EXACT`) says the read is "deste quadro" (this frame) after the label wait, and that the port's old label write "travava" the game. These two statements conflict.
- **Inferred:** if the game does not wait, its consumer reads whatever the context's mask held last time that context was used (two or three frames back), and the same-frame draw list is only correct on the GPU side. That would be visible on PS4 too, so a wait somewhere (for example a context fence in `FlushPendingFree`, report 02 section 2.3) is more likely. Settle this at runtime; see open question 1.

### 2.3 ProcessStats and WaterReadQueries: a two-slot ring

- **Observed:** `sub_8c1b0` (unique 8-byte signature `8b 4f 04 8b 07 48 81 e1`) returns a 44-bit guest address built from a descriptor (`[rdi]` low 32 bits, `[rdi+4]` masked to 12 bits). The only two callers are `WaterRendering::ReadQueries` (site 0xe492b) and `ParticleSystemPool::ProcessStats` (site 0x1019af).
- **Observed (WaterReadQueries 0xe48b0-0xe495d):** it emits `WAIT_REG_MEM` (`BPGnm_EmitWaitRegMem`, label `[rbx+0xe8]+0x128`) for the GPU, then reads `[slot_addr + 4]`, where `slot = r14 + 0xc8 + 40 * (count >= 2 ? (count & 1) : 0)`, `count = [r14+0x118]`. The CPU does not wait on the GPU. The value is stored in `[r14+0x11c]`.
- **Observed (ProcessStats):** `sub_928f0` (dispatch emit) at 0x10197d, then `sub_8c1b0` at 0x1019af, then the per-system read `mov r15d, [r12+rbx*4]` at 0x101a98 (`rbx` = active-system index from a bitmap at `[r13+0x198]`). The value is added to `[r13+0x70]`, and passed to `sub_fc2d0`, which writes it to the particle record at `+0xb4` (observed), after checking a handle at `[rsi+8]`.
- **Observed (CHANGELOG, `SOTC_RB_EXACT`):** the read at 0x101a98 is of "um anel de 3 buffers, o de 2 quadros atrás, já pronto" (a ring of three, the one two frames back, already done). The port waited anyway because the page was shared with the buffer being written now. Note that the code I read shows a two-slot parity for WaterReadQueries, not three. Which slot the current dispatch writes is **unknown**; it is not traced in this session.
- **Observed:** `SOTC_HOOK_PSTATS` (CHANGELOG) removed the write at 0x101ae4 and did not reduce the write-fault count. That is a write-side result. It says nothing about the read side at 0x101a98.
- **Unknown:** what reads `particle+0xb4` later. If the GPU draw size comes from this count, a stale count is a visible risk. If it is only stats or pool recycling, a stale count is harmless.

### 2.4 Allocator metadata and game objects (not GPU data)

- **Observed:** the top BPE heap readers (`BpeHeap_Walk`, `_Link`, `_Find`, `_Alloc`, `sub_495e0`, `sub_4a040`, `sub_4a210`) are the allocator's own headers (see report 03 section 2.1). Their pages hold GPU-written data in the same 64 KiB block (report 03 section 3).
- **Observed:** the game-object readers (`sub_19d410`, `sub_f9c40`, `sub_937c0`, `sub_79f3a0`, `sub_2512a0`) read CPU-written objects.
- **Inferred:** these reads take a readback only because the page is protected and shared. A previous-frame copy is not the right answer for them. The right answer is "these bytes were last written by the CPU, do not wait for the GPU", which needs a byte-level owner map (the port has one for `SOTC_RB_EXACT`) or a page layout change.

### 2.5 Classes with no read faults in the sample

- Auto-exposure (`ComputeExposure`, `sub_95320` family): no read faults. Report 02 found no histogram or exposure readback either. Unknown whether the game reads it at all.
- Skinning results (`sub_118100`, `sub_123410`): writers only (report 03). No CPU read seen.
- Picking, indirect argument buffers (`BPXPR_ConsumeVisibilityMaskIndirect` 0x1a7290): no read faults. 0x1a7290 is the variant used when `[cfg+0x22]` is set; the sample did not hit it.
- GPU timestamps / `Profiler::PushScope` (71 faults): these read CPU-side scope state, not GPU data (inferred).

### 2.6 Readback counter labels (observed)

- `driver.cpp:804-812`: the `SOTC FPS` line prints `readbacks/s (ahead, finish, heap, early)`.
  - `ahead` = `SotcStats::readback_ahead` (downloads of already-submitted work, `buffer_cache.cpp:4515`).
  - `finish` = `readback_finish` (downloads that flush the current work, `buffer_cache.cpp:4517`, `:1625`).
  - `heap` = `heap_guard_builds`, "readbacks into the BPE heap" (`sotc_stats.h:547`), incremented at `buffer_cache.cpp:4867` and `:4944`. A subset, not a partition.
  - `early` = `early_flushes`, "submits right after a write to a read-back page" (`vk_rasterizer.cpp:4687`). Flushes, not readbacks.
- So readbacks/s = ahead + finish. In the logs, `finish` is about 90% of the total (`c14_base.log`: 246 of 269 in the first window; 730 of 743 in `c14_feat.log`).

## 3. Consumer summary: same frame or previous frame

| Consumer | Same frame needed? | Previous frame acceptable? | Visual risk of previous frame | Expected gain (sample) |
|---|---|---|---|---|
| XPR mask (0x1a6d20 family, about 8,650 faults) | Probably yes for the draw list, unless the game's own design is one frame late (open question 1) | Yes for objects that stay visible; an object that becomes visible is missing for 1-2 frames | Pop-in at occluder edges on camera turns; visible at 30 FPS | About 37% of read faults (12.6 s of trace time). Readbacks: unknown |
| ProcessStats count (0x101a98, 3,699 faults) | No, if the game reads the slot two frames back (CHANGELOG) | Yes for stats and recycling; unknown for draw sizing | Low if the count is stats-only; a wrong draw count would be a glitch | 18% of read faults (2.0 s) |
| WaterReadQueries (5 faults) | No (the game does not wait) | Yes, the game reads it without a wait | Water effect one frame late | Negligible |
| Heap metadata (about 6,300 faults) | Not a GPU consumer | N/A. The CPU's own latest value is the only correct answer | None if done as byte ownership | 31% of read faults (about 3.5 s) |
| Game objects (about 1,230 faults) | Not a GPU consumer | N/A, same as above | None | 6% of read faults |
| Auto-exposure, skinning, picking, indirect args | Not observed | Not observed | Not observed | Not observed |

## 4. Hook proposals

Mode names follow `docs/HOOK-DESIGN.md` (Entry, Wrap, Site, Range, Import). A "takeover" hook changes registers or memory and must follow that document's takeover rules; the existing XPR probe is observe-only.

### P0 (measurement, no behaviour change): readback counts per guest RIP

- **Function or site:** `BufferCache::DownloadMemory` (`buffer_cache.cpp:4448`) and `BeginAsyncReadbackImpl` (`:1146`). The guest RIP is already in the fault trace. Pass it down, or read it at fault time.
- **Mode:** none (a counter, like `SOTC_FPS`).
- **Change:** count readbacks and downloaded bytes per guest return address, print the top 8 in the 5 s line. Also rename `heap` and `early` in `driver.cpp:804-812` to say what they count.
- **Why:** removes the guesswork in section 3. The XPR and ProcessStats shares can then be read as readbacks, not faults.
- **What could break:** nothing, if the counter is relaxed-atomic and off by default.
- **Verify:** the per-RIP sum equals `ahead + finish` for the same window.
- **Signature:** not needed (C++ change in the port, not an eboot hook).

### P1 (highest gain, needs open question 1 first): XPR mask swap at 0x1a6d20

- **Function:** `BPXPR_ConsumeVisibilityMask` 0x1a6d20, entry. Already declared as `XPR_CONSUME_MASK` in `scripts/hooks.d/xpr.py`.
- **Mode:** Entry with an argument swap. `rsi` (the bitset) is replaced with a port-owned copy: the last applied bytes of the same object-set, copied async by the port.
- **What it changes:** the CPU reads a bitset that is one or two frames old. The 7,425 read faults and the wait behind them go away.
- **Why it shortens a hitch:** removes the readback round trip (fault, draw-thread copy, wait) from the frame's critical path. Report 01 and the CHANGELOG measured the round trip as most of the wait (78-89% of reads find the GPU finished).
- **What could break:** objects that just became visible are missing for one or two frames (pop-in). Objects culled by a stale occluder mask may draw for a frame. A mask copy of the wrong object set would drop whole sets. The `[ctx+0x64]` count must match the copy.
- **Verify:** `=2` mode: run the original and the swap in one frame, log the bit difference of the mask words and the visible-object count per set; then a camera-pan capture in a scene with many occluders. Compare the frame in which the label is zero (report 01 open question 1).
- **Signature:** unique, 20 bytes (`55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 68 49 89 f9`). Checked this session with `eboot_re.py sig`.
- **Note:** the port already has the same effect in the buffer cache (`SOTC_RB_PREV`, `buffer_cache.cpp:2420-2450`, unmeasured). A hook is only worth writing if the buffer-cache version is measured and shows the cause is at the consumer.

### P1b: the range-test readers 0x1a8a40 / 0x1a8c60 (same family as P1)

- **Function:** `sub_1a8a40` (range test, 723 faults; caller `sub_19c740` from `RenderXPRDecals`) and `sub_1a8c60` (196 faults, unique 24-byte signature).
- **Mode:** Entry with the same argument swap as P1 on the mask pointer.
- **Change, risk, verify:** as P1. Only do it together with P1; the mask copy must be the same frame for both.
- **Signature:** `sub_1a8c60` unique at 24 bytes. `sub_1a8a40` is not unique in 24 bytes (2 matches). Use `eboot_re.py sig 0x1a8a40 -n 40` or anchor on the caller `sub_19c740` (not checked).

### P2 (second-highest gain, low risk): Wrap on the ring accessor `sub_8c1b0`

- **Function:** `sub_8c1b0` (unique 8-byte signature `8b 4f 04 8b 07 48 81 e1`). Its only two callers are `WaterReadQueries` (0xe492b) and `ProcessStats` (0x1019af).
- **Mode:** Wrap. The hook returns the address of a port-owned shadow copy of the slot instead of the guest address, with the same 44-bit layout.
- **What it changes:** both consumers read the shadow. The shadow is a copy of the same ring slot, downloaded async when the GPU finishes it. Before the first copy is ready, the hook returns the guest address (so behaviour falls back to today's).
- **Why it shortens a hitch:** removes the wait in ProcessStats (18% of read faults) and WaterReadQueries. No CPU wait on the GPU for these values.
- **What could break:** a stale count drives a draw size (open question 3). A slot never copied yet (first frames after an area load) must fall back to the guest address, or the count is zero.
- **Verify:** `=2` mode: read both (shadow and guest) and log the mismatch per system; expect a one- or two-frame lag only. Check that particle systems stop and clear correctly on screen.
- **Signature:** unique, 8 bytes. Checked with `eboot_re.py sig`.
- **Gain estimate:** about 18% of read faults in the sample (about 2.0 s of trace time), plus the small water case. The CHANGELOG says `SOTC_RB_EXACT` already removes part of this wait; the gain on top of it is **unknown**.

### P3 (site-level, optional): ProcessStats read site 0x101a98

- **Function:** `ParticleSystemPool::ProcessStats` at 0x101a98 (the load `mov r15d, [r12+rbx*4]`).
- **Mode:** Site (replace one load). Use this only if P2 is not wanted.
- **Signature:** unique, 8 bytes (`45 8b 3c 9c 45 8b 75 74`), no wildcard needed.
- **Risk:** same as P2, but covers only one reader.

### P4 (largest total, not a readback hook): heap metadata out of GPU pages

- **Function:** `BpeHeap_Walk` 0x48a70 (unique 20 bytes), `sub_495e0` (unique 24 bytes), `sub_179c0` (unique 20 bytes), inside the existing `BPE_HEAP_CODE` range (`scripts/hooks.d/alloc.py`).
- **Mode:** Range (the `BPE_HEAP_CODE` range already declared), changing where the headers live.
- **What it changes:** block headers and free-tree links move into a side table in pages the GPU does not write. The CPU's reads stop hitting protected pages, so they stop waiting for readbacks.
- **Why it shortens a hitch:** about 31% of read faults and about 18% of write faults (report 03) come from these pages. Each fault costs 0.4-0.6 ms (report 03, observed in the trace durations).
- **What could break:** block layout and the heap guard (`bpe_heap_guard.h`). This is a large change.
- **Verify:** `=2` mode with the original header writes kept beside the side-table writes; the heap guard already checks walks.
- **Gain estimate:** the read-side part is about 3.5 s of trace time in the sample. Write-side gain is in report 03; the total is **unknown** without a run.

### P5 (attribution only): memcpy source reads

- **Function:** the host `guest_memcpy` / `guest_memset` / `guest_memmove` in `src/runtime.c:170-176`, reached through the import table (Import mode, by NID, no address).
- **Mode:** Import hook, observe only.
- **Change:** when the source is in a tracked page, count per return address (`__builtin_return_address(0)`, the eboot caller). This extends report 03 option 1 from writes to reads.
- **Why:** the 324 host-code read faults (about 2%) and the `0x7ffda260e44d` block (114) are unattributed. A memcpy that reads GPU-written bytes would be a consumer nobody has named yet.
- **What could break:** nothing (observe only).
- **Verify:** the counts sum to the host read faults in the same window.
- **Signature:** not needed (import).

### Not proposed

- A CPU-side fix for the game-object readers (section 2.4). They do not need GPU data; the only change needed is byte ownership or the page layout in P4.
- A previous-frame copy for heap metadata. It would return the wrong value. Use byte ownership.

## 5. Open questions

1. **Does the game wait for the XPR label before reading the mask?** This decides whether P1 changes behaviour or only removes a wait the port added. Settle with the runtime check from report 01 question 1, plus the writer-tick check the port already has (`RbxWaitSeq`, `buffer_cache.cpp:1600-1630`). Record, at 0x1a6d20 entry, the label value at `[ctx+0xe8]+0x40` / `+0x68` and whether the mask bytes were last written by the current or an earlier tick.
2. **Which slot does the current dispatch write in the ProcessStats / WaterReadQueries ring?** Two slots (WaterReadQueries) or three (CHANGELOG)? Trace the writes to `[ctx+0xc8 + 40*i]` in `sub_928f0`'s caller chain. This decides whether P2 gives one or two frames of lag.
3. **What reads `particle+0xb4`?** If a draw size or instance count comes from it, P2 needs a stricter rule (for example, previous-frame count only for systems with no spawns this frame).
4. **Sample bias.** The 157 windows are slow frames. Confirm the readback mix on normal frames with `SOTC_TRACE` on normal windows or with P0.
5. **Host read faults (324).** Attribute them with P5 before deciding whether any host-side consumer exists.
6. **Auto-exposure and skinning reads.** No CPU read is seen in slow frames. Check with P0 on a camera-cut scene (exposure change) before concluding the game does not read them.
7. **Heap metadata share.** How much of the 31% goes away with byte ownership alone (no layout change)? The port's `SOTC_RB_EXACT` has a byte owner map; check how many of these faults it already serves without a wait.
8. **`sub_179c0` and `sub_17e90`.** Confirmed as allocator code? Only inferred here (lock cmpxchg loop, vtable call at `[rax+0xc0]`).
9. **Gain numbers.** Readback counts per consumer are not logged. Every gain above is a share of read faults in slow frames, not a readback count or a milliseconds-per-second figure. P0 is needed to convert them.

## 6. Summary (ranked by expected gain)

1. P0: readback counter per guest RIP. Cheap; turns every estimate here into a measurement.
2. P1: XPR mask swap at 0x1a6d20 (unique 20-byte signature). Largest gain (43% of read faults) but depends on open question 1.
3. P2: Wrap on the ring accessor `sub_8c1b0` (unique 8-byte signature). Covers ProcessStats (18%) and WaterReadQueries with one hook. Low visual risk if the count is stats-only.
4. P4: heap metadata out of GPU-written pages (Range hook on the existing `BPE_HEAP_CODE`). About 31% of read faults, plus write-side gain from report 03. Large change.
5. P1b: range-test readers 0x1a8a40 / 0x1a8c60, same family as P1, do together with P1.
6. P3: ProcessStats read site 0x101a98, only if P2 is not taken.
7. P5: memcpy source-read attribution (observe only). Names the 324 host read faults.
8. Not hooked: game-object reads (section 2.4). Fixed by byte ownership, not by a stale copy.
9. Not observed: auto-exposure, skinning, picking, indirect args. Check before dropping.
10. Settle the XPR wait (open question 1) before writing P1. The CHANGELOG and report 01 disagree.
