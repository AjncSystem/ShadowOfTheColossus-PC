# 11 - Texture writes: how streamed texture data reaches GPU-visible memory

Read only. Only this file was written. Static analysis of `out/eboot.elf` with
`tools/re/eboot_re.py` (dis, func, callees, callers, icallers, imports, sig, sigfind). No game run,
no build, no logs were read for this pass (the port source was checked where noted).
Reports 03, 05, 06, 07 and 07-readback were read first; their texture findings are not repeated
except where this pass changes them.

Labels: **observed** (disassembly with address, or port source with file:line), **inferred**,
**unknown**.

## 1. Answer

- **No GPU copy, no detile in the game.** The eboot imports no Gnm DMA, copy or texture function
  (`imports` for Dma, Copy, Tile, Texture, Detile, Blit, Fill: nothing). All texel movement is host
  `memcpy` or a decompressor writing into guest memory. **observed (imports)**. Tiling is
  therefore either baked into the asset or done by the port: **inferred**, not checked against
  the asset format.
- **Two copies per streamed texture, both on the game side.** (a) The loader fills a resource
  buffer: file read, then either a raw copy or Oodle/memcpy decompression, into a CPU-family
  allocation (`CreateResource` 0x147160, `sub_12820`). (b) The streaming job copies the mip
  record into a streaming pool with `memcpy` at **0x98d67** (`sub_98ae0`). The pool address is
  what the GPU samples: **inferred** (the copy target is the pool cursor, and no other
  texture-data store was found).
- **The pool copy is the write the port must track.** It is a plain host `memcpy` into the pool
  (1 of the 1,231 memcpy sites). When the pool is recycled, its pages were GPU-read before, so the
  copy takes write faults. This is the most likely source of the memcpy share of the 47% faults
  (**inferred**, not yet attributed by return address; report 03 option 1).
- **There is a clean "copy finished" marker.** After the memcpy returns, the job stores the
  record index into `owner+0xc0` (0x98d6f). That store is observed. A Site hook there (or at the
  memcpy return 0x98d6c) fires once per record, after the bytes are complete.
- **Ready signals exist and are observable (all observed):**
  1. Resource state: `dword [res+0x98] = 0xb` with object at `[res+0x20]`. Written at 0x14d701
     (`sub_14d670`, the CreateResource tail on the loader thread) and at 0x14ca5f (`sub_14c9a0`).
  2. Texture job counter at `chunk+0x70`, waited by `sub_112d50` (0x112d50), which is
     `JobSystem_WaitAndFreeCounter`.
  3. Per-record "copied" marker `owner+0xc0` (above). No event flag is used for textures; the
     resource event at `res+0xa8` is only set when a waiter exists (0x14745f).
- **Timing relative to the first draw:** the bytes are final at state 0xb. The pool copy runs on
  a job thread after that, started by `TextureManager` (`sub_13a3e0`) and waited on in a later
  pass. The frame barrier (report 05, `WaitForAllCmdsToFinish`) should cover it, so the copy is
  done before the frame's draws are recorded. **inferred**. Whether the draw path checks residency
  before sampling a not-yet-copied texture (and uses a low mip instead) is **unknown**: no draw
  path reader of `owner+0xc0` or `res+0x98` was found yet.
- **Proposal, ranked:** (1) a Site hook after the pool memcpy (0x98d6c, sig unique) that enqueues
  the texture for upload, coalesced per texture; (2) a pre-copy Site (0x98d5e, sig unique) that
  reserves or unprotects the destination span in one call, removing the per-page faults; (3) a
  rewind hook (0x98bab) that invalidates images whose pool range is released.
- **Gain: unknown.** The bytes do not shrink (9-13 GB per session). The hook moves the port's copy
  (`CopySparseSerial`, 41-58 ms in bad frames) from the first-bind path on the draw thread to a
  job thread, and it can remove write faults from the pool copy. Savings are bounded by the 55-60
  ms `BindTextures` upload share in bad frames if the copy overlaps other work. The honest number
  needs the per-site fault counter first.
- **Stale-data risk: medium.** The pool cursor is rewound (0x98bab path) and reused, and a texture
  can be copied in several jobs (one per index). An eager upload must be done after its record's
  copy and must be invalidated on rewind. The port's existing page tracking covers later writes
  only for pages it protects after upload.

## 2. Evidence

### 2.1 Imports: no DMA, no copy shader

- `eboot_re.py imports "Dma|Copy|Tile|Texture|Image|Surface|Detile|Fill|Blit|Garlic|Cache|Invalid|Write|Flush|Hint"`:
  only `sceFiosFHWriteSync`, `sceFiosIOFilterCache` and `sceGnmFlushGarlic` (no callers listed).
  No `sceGnmDmaData`, no texture helper. **observed**
- `memcpy` 1,231 call sites, `memset` 618, `memmove` 551. **observed (imports)**

### 2.2 Loader: resource fill and ready state

- `ProcessLoadingThreadLoop` (0x148600) runs jobs whose function pointer is `CreateResource`
  (data `0x15feb00` = `0x147160`, loaded at 0x148778). **observed**
- `CreateResource` (0x147160):
  - `r13 = [r15+0x80]` (size), `r14 = sub_14c000(r15)` = `[r15+0x90]` (data pointer). **observed**
  - Compressed when `[res_hdr+0x70] & 8` (0x1471f4). Then `sub_12820(size=[hdr+0x54], align 1)`
    allocates the destination, and `bpe::Compression::Decompress` (0x1aa2a0) fills it
    (0x147257). **observed**
  - Uncompressed: the file buffer is used as is (0x14728d, `sub_14c020` clears the buffer
    fields). **observed** (code path); the ownership transfer is **inferred**.
  - `sub_144880` calls the object's virtual `+0x78` with `(data, size)`: the object is created
    from the buffer (0x1448f2). **observed**
  - Tail: `sub_14d670` sets state 0xb and the object (0x14d701, 0x14d6f1), then
    `EventFlag_Set(res+0xa8)` only if `res+0x90` is set (0x14744c-0x14745f). **observed**
- `sub_12820` (family-1 allocator) and `sub_12950` (same family, 0x100 granularity): family 1 is
  the CPU family from `cpu_memory_family_init` (0x13380), per report 06. **observed (code)**,
  **inferred** that the streaming pool belongs to this family.
- `Decompress` method 0 (stored) is a `memcpy` at 0x1aa3cb (sig `48 8b 7d a0 4c 89 fe 48`,
  unique). Methods 2-5 call Oodle (`sub_89d3b0`). Method 1 is unknown (report 05). **observed**
- `LoadFileInternal` (0x14b980) allocates via `sub_12820` (0x14bb5c), reads through a file vtable,
  and also has one `memcpy` at 0x14bc5a. **observed**
- Port side of the file read: `do_pread` (`src/runtime_file.c:398-416`) either copies from a
  bounce buffer (`memcpy`, host code) or calls `touch_for_write` and reads straight into the guest
  buffer. So file data also lands as host-code writes. **observed (port source)**. The
  `sceFiosFH*` imports are not implemented in `src/runtime*.c`; they resolve to the Fios2 PRX,
  which reaches `sceKernelPread` (`runtime_file.c:609`). **observed (import table)**.

### 2.3 TextureManager: kick, wait, copy

- `TextureManager` update `sub_13a3e0` (0x13a3e0-0x13a6ea). For each slot with a resource
  (`[slot+0x90]`), it checks `ResourceWrapper::IsReadyToCreate` (`state == 0xb` and object
  non-null, `0x14cf00`). **observed**
  - Kick (0x13a5a4-0x13a5c0): `sub_95580`, `sub_14d7b0` (returns the texture object), then
    `TextureStreamingChunk::ProcessTextureData(chunk, owner, slot+1)` (0x112b80). **observed**
  - Wait (0x13a63a-0x13a642): `sub_112d50`, the same wait, when the slot is in flight. **observed**
  - Release (0x13a65a-0x13a682): `sub_14d2d0` (`lock xadd [res+0x40]`) and clear `[slot+0x98]`.
    **observed**
- `ProcessTextureData` (0x112b80): stores `chunk+0x78 = owner` and `chunk+0x80 = index`
  (0x112be8 and the `rsi`/`edx` args), kicks a job array whose function pointer is at data
  `0x15fea88` = `0x112d30`. **observed**
- Job `0x112d30`: `sub_98ae0(owner = [chunk+0x78], index = [chunk+0x80])`. **observed**
- `sub_98ae0` (0x98ae0-0x98d95), the mip-record job:
  - Records: array at `owner+0x80`, stride `0x50`. `[rec+4]` = size, `[rec+8] & 0xffff0000`
    = pool offset, `[rec+0x40]` = `ResourceWrapper*`. **observed**
  - `owner+0xc0` = index of the last copied record. Index `<=` last goes to the release path
    (0x98bab); index `>` last goes to the load path. **observed**
  - Load path: if `IsReadyToCreate(res)` (0x98c94), compare the pool cursor `owner+0xb8` with the
    record offset `owner+0xa0 + offset`; below the cursor, `sub_12950` rewinds the pool (0x98cde).
    **observed**; the exact cursor semantics are **inferred**.
  - **The copy (0x98d4e-0x98d67):** `obj = sub_14d7b0(res)`, `src = sub_112b30(obj) = [obj+0x60]`,
    `memcpy(dst = owner+0xb8 cursor, src, size = [rec+4])`. **observed**
  - **The marker (0x98d6c-0x98d6f):** `mov [owner+0xc0], index`. **observed**
  - Release path: `sub_12950` with a negative length, `owner+0x1b0` decremented. **observed**
- Source address: the copy uses `[obj+0x60]` without adding the record offset. Whether `+0x60`
  already points at the record's mip data is **unknown**. This matters for the hook arguments.

### 2.4 Signatures (`eboot_re.py sig` / `sigfind`, all unique)

| Address | What | Pattern (`??` = wildcard) | Bytes |
|---|---|---|---|
| 0x98d5e | pre-copy: rdi=dst, rsi=src, rdx=size before the memcpy call | `4c 89 ef 48 89 c6 4c 89 f2 e8 ?? ??` | 12 |
| 0x98d6c | post-copy: `mov eax,[rbp-0x2c]` (index) before the marker store | `8b 45 d4 41 89 84 24 c0` | 8 |
| 0x98ae0 | sub_98ae0 entry (owner, index) | `55 48 89 e5 41 57 41 56 41 55 41 54 53 50 89 f3 49 89 fc 48` | 20 |
| 0x98bab | sub_98ae0 release path | `7e ?? 48 8d 04 80 49 c7` | 8 |
| 0x112d30 | job function (tail call to sub_98ae0) | `48 8b 47 78 8b b7 80 00` | 8 |
| 0x112d50 | `sub_112d50` (wait for texture job) | `48 89 f8 48 83 78 70 00` | 8 |
| 0x112b80 | `ProcessTextureData` (kick) | `55 48 89 e5 41 57 41 56 53 48 81 ec 28 01 00 00 4c 8b 3d ??` | 20 |
| 0x14d701 | `mov dword [r15+0x98],0xb` in sub_14d670 (resource ready) | `41 c7 87 98 00 00 00 0b 00 00 00 4c` | 12 |
| 0x14ca5f | `mov dword [rbx+0x98],0xb` in sub_14c9a0 | `c7 83 98 00 00 00 0b 00 00 00 4c 8d a3 c8 00 00` | 16 |
| 0x147160 | `ResourceFactory::CreateResource` | (function start, 28 bytes) | 28 |
| 0x1aa3cb | Decompress method 0 (memcpy) | `48 8b 7d a0 4c 89 fe 48` | 8 |

## 3. Hook proposals

Each hook is a Site (or Entry) on engine code. None needs an import hook. All addresses are the
EU 1.01 eboot (same code as US 1.00, see `docs/HOOK-DESIGN.md`).

### H1. Post-copy upload trigger (main proposal)

- **Site:** 0x98d6c in `sub_98ae0`. Signature `8b 45 d4 41 89 84 24 c0` (unique, 8 bytes).
  Compute the record's address from the args: `owner` (`r12`) and `index`, which the hook reads
  from `[rbp-0x2c]` (the stack slot at the site, as the game stores it).
- **Mode:** Site (after the call, the destination span is final).
- **What it changes:** enqueues `(owner, index, dst span, size)` to the port's upload worker. The
  worker copies `dst span` to staging and creates or refreshes the VkImage. A texture is
  coalesced over its records before creating an image, so that one image is built per texture,
  not per record. The first-bind path skips the copy when the image was already filled by H1 and
  the span hash matches.
- **Why it shortens a hitch:** the port's copy (`CopySparseSerial`) runs on a job thread instead
  of the draw thread at first bind. The 55-60 ms `BindTextures` upload share in bad frames is the
  target, and the write-fault for the copy is not repeated later.
- **What could break:**
  - The pool cursor can rewind (0x98bab path), so a span is reused by another texture. Such a
    span must not be copied after the rewind. Pair H1 with H3.
  - A texture can be copied in several jobs (index by index). The image must wait for its last
    record, or be refreshed per record. The "last" record is not known to H1; use the texture's
    record count from `owner+0x80` (**unknown** layout, check with `dis`).
  - The copy must not read a span that a later job is still writing. The job is per record, and the
    job pool runs them in parallel (`JobSystem_KickJobArray`).
  - The hook runs on a job worker. It must not take the port's locks for long, and it must not
    call into the draw-thread state.
- **How to verify (`=2` mode):** log `(owner, index, dst, size, XXH3 of the span)` at H1. At the
  first bind, hash the same image again and compare. Count `hash mismatch` (stale), `eager
  upload used` and `first-bind copy avoided` in the `SOTC` 5 s line.
- **Unique signature:** yes (8 bytes at 0x98d6c). Expected address check: 0x98d6c, with the anchor
  "sub_98ae0 entry pattern" (20 bytes at 0x98ae0) if the first pattern is not unique in another
  build.

### H2. Pre-copy destination reservation (fault batching)

- **Site:** 0x98d5e in `sub_98ae0`. Signature `4c 89 ef 48 89 c6 4c 89 f2 e8 ?? ??` (unique, 12
  bytes). Args at the site: `rdi = dst (r13)`, `rsi = src (rax)`, `rdx = size (r14)`.
- **Mode:** Site (before the call).
- **What it changes:** in active mode, the port unprotects (or marks as writable in one call) the
  destination span `[dst, dst+size)` before the memcpy, so the copy takes no per-page faults. The
  port re-protects the span after the copy (H1).
- **Why it shortens a hitch:** removes the per-page write faults of this one memcpy (the faults
  are the 47% host class, and the copy is one of them). One bulk unprotect replaces N faults.
- **What could break:** the span is GPU-read before the copy (the old image). Unprotecting it does
  not hurt the GPU (it reads the VkImage, not the pool), but the port's own readback tracking may
  rely on those protections. Check `page_manager.cpp` before enabling. The span is a pool range
  that might be shared with another live texture (the cursor logic), so the unprotect must cover
  exactly `size` bytes.
- **How to verify (`=2` mode):** count faults inside `sub_98ae0` with and without the hook
  (`SOTC_FAULTS` per-site counter, report 03 option 1). Expect the fault count for this site to
  drop to about 0.
- **Unique signature:** yes (12 bytes at 0x98d5e).

### H3. Pool rewind invalidation

- **Site:** 0x98bab in `sub_98ae0` (release path). Signature `7e ?? 48 8d 04 80 49 c7` (unique, 8
  bytes). At the site, the rewind range is `[owner+0xa0 + (rec+8 & 0xffff0000), owner+0xb8)`.
- **Mode:** Site (before the release).
- **What it changes:** invalidates every port image whose source span lies in the released
  range. Without it, a later copy into the same span is invisible to the port until a fault
  happens.
- **Why it shortens a hitch:** it does not shorten a hitch by itself. It removes a stale-texture
  failure mode that H1 would otherwise hit. Needed for correctness, not for speed.
- **What could break:** over-invalidation (many images re-uploaded) if the range is large. Mitigate
  by checking the span's image list first.
- **How to verify (`=2` mode):** log the released span and the count of invalidated images; compare
  with the number of re-uploads that show a changed hash.
- **Unique signature:** yes (8 bytes at 0x98bab).

### H4. Resource-ready prefetch (earlier trigger, observe first)

- **Site:** 0x14d701 in `sub_14d670` (`mov dword [r15+0x98],0xb`, sig `41 c7 87 98 00 00 00 0b 00 00 00 4c`,
  unique, 12 bytes). Also 0x14ca5f in `sub_14c9a0` (sig unique, 16 bytes).
- **Mode:** Site (after the store).
- **What it changes:** a generic resource-ready event on the loader thread, from which the port can
  start the image prefetch (`PrefetchImages`, report 07) earlier. It is not texture-specific. The
  texture type must be checked from the object (not yet decoded: **unknown**).
- **Why it shortens a hitch:** the first upload starts before the TextureManager kick. The gain
  is only the time between state 0xb and the kick, which is unknown.
- **What could break:** the port would prefetch non-texture resources too. The decompressed buffer
  might be reused after state 0xb (ownership is **inferred**, not verified).
- **How to verify (`=2` mode):** log resource type and the time from 0xb to the H1 copy.
- **Unique signature:** yes (12 bytes, 0x14d701).

### H5. Measurement only: texture job entry and wait

- **Entry** on `sub_98ae0` (sig 20 bytes at 0x98ae0, unique) and on `sub_112d50` (sig `48 89 f8
  48 83 78 70 00`, unique, 8 bytes).
- **Mode:** Entry, observe only.
- **What it changes:** nothing. It times the job and the main-thread wait, which splits the
  `ResourceFactory` barrier cost (report 05) between texture copies and the rest.
- **Verify:** `=2` mode logs the wait time per texture per frame.

## 4. Open questions

1. **Residency check on the draw path (unknown).** Find the readers of `owner+0xc0` and
   `res+0x98` outside the streaming code (`sigfind` on loads at those offsets in draw code, or
   `callers` of `sub_14cf00`). This decides whether a texture can be sampled before its copy.
2. **Texture header and format (unknown).** H1 needs the format and size before creating the
   VkImage. The header is in the resource buffer; its layout is not decoded. Read it from the
   object at `[obj+0x60]` (`dis` of the `+0x78` virtual create, `sub_144880`).
3. **Source address of the memcpy (unknown).** `sub_112b30` returns `[obj+0x60]` without the
   record offset. Confirm whether `+0x60` is per-record or per-texture.
4. **Pool cursor semantics (inferred).** Confirm `sub_12950` (alloc, free, rewind) and whether the
   pool is one `Default TaggedArena` object (report 06).
5. **Does the resource buffer outlive the copy (unknown).** If it is freed after `sub_98ae0`, the
   port can read the buffer at state 0xb; if not, the buffer is a second copy in guest memory.
6. **Which TextureManager pass waits (unknown).** The wait at 0x13a642 is in a later pass. Measure
   the pass-to-pass delay with H5 before choosing the H1 batching window.
7. **Non-streamed textures (unknown).** Render targets and dynamic textures do not pass through
   `sub_98ae0` (this path is for TextureManager streaming resources). Keep the port's first-bind
   upload for them.
8. **Zero-copy option (unknown feasibility).** The port's copy exists because the pool is in guest
   memory. If the guest range can be imported to Vulkan as host memory
   (`VK_EXT_external_memory_host`), the copy goes away. Tiling would still need to match the
   sampler's layout. Not evaluated.
9. **Method 1 of Decompress (unknown)**, from report 05.
