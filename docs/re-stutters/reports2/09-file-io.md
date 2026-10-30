# 09 - File I/O and decompression: where reads and decodes run, and whether they are on the frame path

Read only. Nothing built, run or edited except this report. Addresses are eboot VAs (EU 1.01,
`out/eboot.elf`) unless marked port. Labels: **observed** (disassembly, pseudocode, trace or log
line, with its source), **inferred**, **unknown**.

Sources: `tools/re/eboot_re.py` (`func`, `dis`, `callers`, `callees`, `str`, `sig`), Ghidra
pseudocode in `out/re/decomp/`, 157 trace windows in `user/trace/*/*.json` (`SOTC_TRACE`, summed
with a short script; not saved), the port code in `src/runtime_file.c`, `src/probe.c`,
`gpu/shadps4/core/libraries/videoout/driver.cpp`, and round 1 report `05-streaming.md`, which this
report extends and does not repeat.

## 1. Answer

- **observed:** the file reads themselves never run on a frame thread. In all 157 traced windows
  (44.9 s of frames, mostly slow ones) the only thread with `file read` spans is `game: SceFiosIO`
  (42,419 spans, 1.73 s total, 3.9% of the window time). The longest single host read is 2.43 ms.
  No read is above 5 ms. Median file-read time per window is 11.9 ms.
- **observed:** the port's `io` column is not frame time. `runtime_file_read_ns_take()` sums the
  `TIMED_READ` time of every thread that calls `read`/`pread` (src/runtime_file.c:67-70), so it
  can exceed the frame and it counts background threads. `copyMB` is the guest-to-GPU copy volume
  (`frame_copy_bytes`, sotc_stats.h:1174), not file bytes.
- **observed:** the big logged frames do not show I/O. Worst-frame samples from the logs: 3829 ms
  with io 2.3 / copyMB 251.7; 417 ms with io 7.4 / copyMB 260.7; 376 ms with io 8.3 / copyMB
  308.0. The traced 1436 ms frame (frame000091) shows `RenderFlip equeue wait` and write faults and
  readbacks on `BPE JobWorkerThread` at 51-55 ms, with 7.8 ms of reads in the whole window.
- **observed:** the 172 ms wait on "io" from earlier logs is not reproduced. The closest trace
  (frame000158, 209.8 ms) has `SceFiosIO` idle in `cond wait` for 204.7 ms while the frame sits in
  `RenderFlip equeue wait` (210.6 ms). **inferred:** the old "io" label was a wait on the loader
  or a barrier, not a disk read. The trace does not record the wait's site.
- **observed:** the decode path is `ResourceFactory::CreateResource` (0x147160) ->
  `bpe::Compression::Decompress` (0x1aa2a0) -> Oodle (0x89d3b0). Method is the top 3 bits of the
  first dword (round 1). There is no `zlib` or `lz4` string in the eboot, and no `lz4` in the code.
  zlib exists only inside the linked `libSceFios2` (thread `SceFiosZlib0`, prio 698).
- **observed (code), unknown (caller):** `ResourceFactory::BuildSync` (vtable slot 2, 0x146b60) is
  a synchronous path on the calling thread. It starts a load job, waits on its job counter
  (`sub_14ae80`, which tail-jumps to `JobSystem_WaitForCounter` 0x35590), and then decodes inline.
  No direct caller is found. The RF global `DAT_02ee2d00` is used by many functions, but none of
  the indirect call sites I checked is a slot-2 call on it. Whether `GameLoop` or any update-thread
  code reaches `BuildSync` is **unknown**. This is the single most important open question for
  the frame path.
- **observed:** the frame-end drain (`WaitForAllCmdsToFinish`, 0x1469d0) is called once at
  `GameLoop` exit (0x25cb12, round 1 correction), and from `DeleteResource` (slot 5, 0x1478d0),
  the destructor and 0x147d60. Unloads wait for the whole queue. How often `DeleteResource` runs in
  play is **unknown**.
- **Proposal ranking (see section 3):** (1) log-only Entry/Wrap hooks on `BuildSync`, `CreateResource`
  and `FiosFile::PreadAsync` to learn the caller thread and the time spent; (2) only if (1) shows
  an update-thread caller, a bounded async route for it; (3) read-ahead of the next archive ranges
  in the port, which is low gain on this SSD setup. Expected gain on the logged frames is small
  (see section 3 for numbers).

## 2. Evidence

### 2.1 Archives and mount (observed)

- `GameFiles::MountPaks` (sub_b10f60) mounts the strings `$/devpsarcs/devpsarcs.txt`, `$/pak0.psarc`
  to `$/pak3.psarc`, `$/pakN.psarc` and `$/cachedFiles.psarc` (0x12d002d-0x12d0092, `str psarc`).
- Round 1 notes the mount via `sceFiosArchiveMountSync` (sub_31c90). Not re-checked here.
- The dump is at `SOTC_GAME_DIR`. Its archives are not in the repo and were not inspected.
  **unknown:** the psarc internal layout and whether `cachedFiles.psarc` is the hot file.

### 2.2 Threads that touch I/O (observed, from thread names and trace spans)

Thread names come from `guest thread '...' created` lines in the logs (waveB and user logs):

| Thread | Prio | Role | Evidence |
|---|---|---|---|
| `SceFiosIO` | 700 | host reads (`read`/`pread`) | 42,419 `file read` spans, all on this thread |
| `SceFiosZlib0` | 698 | Fios2 zlib (compressed chunks) | thread name only; inferred |
| `SceFiosCallback` | 700 | Fios completions | idle in `cond wait` (38,987 spans, 33.2 s) |
| `Resource Loading` | 700 | ResourceFactory loop (0x1464d0 -> 0x148600) | `evf wait` 11.2 s total, few spans |
| `BPE JobWorkerThread CPU1-4` | 700 | job-system jobs, which carry the load jobs | write faults (below), readbacks |
| `Shadow Game Update` | 700 | game update | readback 239 ms, read fault 226 ms (GPU tracking) |
| `Game Render`, `RenderFlip` | 700 | render and flip | `equeue wait` 39.3 s in `RenderFlip` |

- **observed:** `Resource Loading` starts load jobs with `StartLoadJob` (0x14b800), which calls
  `JobSystem_KickJobArray` (0x35480), and then `LoadFileInternal` (0x14b980) runs under the job.
  **inferred:** the `FiosFile::PreadAsync` call (0x31830) happens in that job, on a job worker. The
  host `read` is then done by `SceFiosIO`. The trace supports the second part (all reads on
  `SceFiosIO`); the first part is inferred from the call graph.
- **observed:** `BuildSync` waits on the job counter (`[obj+0xd0]`, round 1 "IsJobDone") with
  `JobSystem_WaitForCounter`, so a synchronous caller waits for the job's read and that job's worker.

### 2.3 Decode (observed, with inferred thread)

- `CreateResource` (0x147160, signature 28 bytes, unique) calls `bpe::Compression::Decompress`
  (0x1aa2a0, signature 32 bytes, unique) at 0x147257. Its callers are `BuildSync` (0x146c53) and a
  second site inside `CreateResource` (0x1473a0). **unknown:** the async caller (the build callback
  `[task+0x18]` from round 1) and which thread runs it.
- **inferred:** the Oodle decode runs wherever `CreateResource` runs: the loader thread for async
  requests and the caller for `BuildSync`. It is not measured by the port (no `TIMED_*` span).
- **observed:** the job workers' write faults are the largest write-fault source in the traces:
  about 25,200 at host address `0x7ffda260e62e` (1,335 ms), 8,470 at `eboot+0x101817`
  (`ParticleRenderer_ShiftRecords`, 1,079 ms), 6,306 at `0x7ffda260e4f6` (995 ms), and 4,491 at
  `eboot+0x48f07` (`BpeHeap_Free`, 1,798 ms). **inferred:** the decode writes into guest memory on
  the job threads, which the GPU tracker protects. This is the same effect as report 05 section 3.5,
  now seen on the job threads.

### 2.4 Port side: `src/runtime_file.c`

- `do_read` (around line 380) and `do_pread` (around line 400) read into a thread-local bounce buffer
  (`bounce_buffer`, up to `BOUNCE_LIMIT` = 64 MiB; realloc on growth, never shrunk) and then `memcpy`
  into guest memory. Above 64 MiB they `touch_for_write` each page and read straight into guest
  memory. **observed.**
- The `memcpy` from the bounce buffer and `touch_for_write` are not inside `TIMED_READ`, so `io`
  does not include them. **observed.** Their cost is **unknown** (not measured).
- `TIMED_READ` (lines 67-70) also emits a `file read` trace span with the byte count `a`.
  **observed.**
- Prefetch of the archives already exists: `prefetch_thread` (src/probe.c:374-430). After the
  shader preload and 1200 flips (about 20 s of frames), it reads every `*.psarc` once, in order,
  with 4 MiB chunks, low thread priority and an I/O priority hint, unless the archives are more than
  half the RAM. On this machine (32 GB RAM, about 12 GB of archives, per the comment) it runs.
  **observed (code); the 12 GB figure is from the comment, not measured here.** `SOTC_PREFETCH=0`
  turns it off.

### 2.5 Measured budget (observed, traces)

- Reads on `SceFiosIO`: 1.73 s over 157 windows (44.9 s), per-window median 11.9 ms, max 28.8 ms.
- Longest single read: 2.43 ms. No read above 5 ms.
- The worst windows (for example 209.8 ms, 197.5 ms, 182.9 ms) have 8-10 ms of reads each.
- `SceFiosCallback` and `SceFiosIO` spend about 30 s each in `cond wait`, which is idle time.
- **inferred:** the game's disk is an SSD in these sessions (the session header says `(SSD)` for
  `Documents\SOTC\CUSA08809`). The probe comment about "the user's HDD" (src/probe.c:369) is from an
  earlier setup. A slow disk would change these numbers; that case is **unknown** here.

## 3. Hook proposals

All hooks follow `docs/HOOK-DESIGN.md` (declared in `scripts/hooks.d/*.py`). The signatures are
from `tools/re/eboot_re.py sig`. Each one is unique in the eboot unless noted. No hook of these is
declared yet, except the three loader hooks in `scripts/hooks.d/loader.py` (`LOADER_WAIT_ALL`,
`LOADER_SYNCPOINT`, `LOADER_HAS_PENDING`), which already cover the drain.

### H1. Log-only probe on `ResourceFactory::BuildSync` (highest priority)

- **Function:** `ResourceFactory::BuildSync`, 0x146b60, vtable slot 2. Signature (32 bytes,
  unique): `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec d8 01 00 00 48 8b 1d ?? ?? ?? ?? 49 89 f5 49 89`.
- **Mode:** Wrap, log only. It returns Continue and writes the caller's thread id and name, the
  wall time spent (start and end timestamps), and the request's size and priority. The wrapper must
  not change the return value.
- **What it changes:** nothing. It only answers: does `BuildSync` run during play, and on which
  thread? Also whether it waits on the job counter for long.
- **Why it helps:** if no update-thread call exists, the sync path is not a hitch source and the
  rest of this report is about the loader only. If one exists, its wait time is the hitch.
- **What could break:** nothing, if log-only. Cost is one timestamp pair per call.
- **Verify:** switch to a log value (proposed `2`, not yet in `HOOK-DESIGN.md`; check the design
  before using it). Expected result: a short list of caller threads. Count `BuildSync` calls per
  second with `SOTC_TRACE`, and compare with the `Shadow Game Update` span list.
- **Signature:** unique.

### H2. Log-only probe on `ResourceFactory::CreateResource`, the decode

- **Function:** `ResourceFactory::CreateResource`, 0x147160. Signature (28 bytes, unique):
  `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec c8 01 00 00 4c 8b 2d ?? ?? ?? ?? 48`.
- **Mode:** Wrap, log only. Record the thread and the wall time, and the size of the decoded data
  if it is available from the request (layout unknown; log raw fields first).
- **What it changes:** nothing.
- **Why it helps:** the decode time is not measured today (section 2.3). It shows whether the decode
  is long enough to matter on the loader, on the job workers, or on an update-thread caller (H1).
- **What could break:** nothing, if log-only.
- **Verify:** `SOTC_TRACE` span per decode, and a per-second total. Compare with the job-worker
  write-fault counts.
- **Signature:** unique.

### H3. Log-only probe on `FiosFile::PreadAsync`, the archive reads

- **Function:** `FiosFile::PreadAsync`, 0x31830, which is `sceFiosFHPread`. Signature (16 bytes,
  unique): `55 48 89 e5 53 50 48 89 c8 48 89 fb 48 89 d1 48`.
- **Mode:** Entry, log only. Record the file handle, buffer, size and offset (argument order
  **unknown**: check against `sceFiosFHPread`'s documented shape before trusting the fields).
- **What it changes:** nothing. It gives the access pattern: the offsets of a load batch, the
  distance between them, and whether they are sequential.
- **Why it helps:** it is the input for H4 and for judging whether a read-ahead of the next ranges
  would hit. It also shows whether the game's own pattern competes with `prefetch_thread`.
- **What could break:** nothing, if log-only. A large log volume: rate-limit it.
- **Verify:** log count per second and the offset histogram against `SceFiosIO` spans.
- **Signature:** unique.

### H4. Read-ahead of the next archive ranges (port side, not a game hook)

- **Where:** `do_pread` in `src/runtime_file.c` (around line 400), using H3's data. On a read at
  offset X of N bytes, post a low-priority read of the next 4-8 MiB of the same handle on a separate
  thread, or call `PrefetchVirtualMemory`/`ReadFile` with low priority. Do not block `do_pread`.
- **Mode:** not a hook; a port change behind an env switch (for example `SOTC_READAHEAD=1`).
- **What it changes:** the bytes behind the next read are already in the Windows file cache.
- **Why it helps:** removes disk latency from `SceFiosIO` if reads are not sequential. Gain is
  **small on this SSD**: no read in 157 windows was above 2.5 ms (section 2.5). It may help on the
  HDD case that `SOTC_PREFETCH` was written for.
- **What could break:** RAM use (the file cache competes with the game and the GPU mappings); extra
  disk traffic while an area loads (the existing prefetch already runs once after 20 s, so this
  could overlap it); wrong guesses waste bandwidth.
- **Verify:** `SOTC_FPS` counters, the `file read` span times in `SOTC_TRACE`, and H3 offsets.
- **Expected gain:** on the traced windows, under 2.5 ms per read and about 12 ms per window.
  **unknown** on a slow disk.

### H5. Bounded async route for an update-thread `BuildSync` (only if H1 shows an update-thread caller)

- **Function:** `ResourceFactory::BuildSync`, 0x146b60 (same signature as H1).
- **Mode:** Wrap, active. For a call from the update thread, do not wait on the job counter
  (`sub_14ae80`) and decode inline. Instead queue the request on the async path and return the same
  result the caller would get.
- **What it changes:** moves the job wait and the decode off the update thread.
- **Why it helps:** removes the wait on the job and the decode from the frame.
- **What could break:** the caller expects a loaded object on return. A deferred load means the
  caller may read an empty resource: a crash or a wrong frame. This is the main risk. Also the
  decode's writes would land later, on the loader, with the same GPU-tracking issues as today.
- **Verify:** `=2` log-only first (H1); then active only in a test scene. Check the caller's
  return value and object state.
- **Expected gain:** **unknown**; it depends on H1.
- **Signature:** unique.

### H6. Per-item drain on `DeleteResource` (not a frame-path item unless H1/H7 show it runs in play)

- **Function:** `ResourceFactory::DeleteResource`, 0x1478d0, vtable slot 5. It calls
  `WaitForAllCmdsToFinish` (0x1469d0) and waits for the whole queue. The hook would replace that
  with a wait on this item only.
- **Mode:** Wrap, active, only for `DeleteResource`.
- **What it changes:** an unload waits for its own load, not for all pending loads.
- **Why it helps:** a texture unload during streaming no longer waits for 16,500 queued requests.
- **What could break:** an in-flight load writes into freed memory. Requires a per-item wait with
  the lock (`[obj+0x58]`). High risk; medium complexity.
- **Verify:** H7 first (count how often it runs in play).
- **Signature:** 0x1478d0 not yet checked with `sig`; **unknown**.

### H7. Log-only count of `DeleteResource` and `WaitForAll` calls during play

- **Function:** `DeleteResource`, 0x1478d0 (not yet signed), and `WaitForAllCmdsToFinish`, 0x1469d0
  (already declared as `LOADER_WAIT_ALL`).
- **Mode:** Entry, log only, counts and thread. The existing `LOADER_WAIT_ALL` (loader probe switch)
  may already give the second count. Check its output first.
- **Why it helps:** decides whether H6 is worth doing.

## 4. Open questions

1. Which code calls `ResourceFactory::BuildSync` (vtable slot 2)? Find the indirect call sites on
   the RF object. A caller on `Shadow Game Update` or `Game Render` would make H1/H5 the main work.
2. Does `DeleteResource` (slot 5) run during play, or only on area unload and exit? (H6/H7.)
3. Who is the async build callback `[task+0x18]` (round 1), and does it run on the job workers or
   on `Resource Loading`? The write-fault data points to the job workers.
4. What does `LoadFileInternal` (0x14b980) do with a read's result: the copy into the job's buffer,
   and does it need the `memcpy` seen in its callees (`import:memcpy`)?
5. The 172 ms "io" wait: the trace has no span for a barrier or a job wait, only `evf wait` with
   address `eboot+0x3b4c7` for every event wait. A span with the caller's address on the
   `WaitForAll` loop (0x146aa0-0x146af3) would separate them. Add it to `SOTC_TRACE` as an
   observation, not a fix.
6. The codec of `sub_1b9130` (method 1 in round 1) is still unknown. Is it used by any resource?
7. Does `prefetch_thread` compete with the game's reads on a slow disk? A trace of a session with
   `SOTC_PREFETCH=0` and one with it on would show it. Not done here.
8. What is the cost of the bounce `memcpy` and `touch_for_write` on `SceFiosIO`? Not measured
   (outside `TIMED_READ`).
9. How is the `SceFiosZlib0` decompression split from Oodle for a given resource? Only the
   per-resource header (round 1) would tell.
