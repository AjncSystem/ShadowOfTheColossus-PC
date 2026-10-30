# .sotctrace file format (SOTC_TRACE v2)

Written by the recorder (`gpu/shim/sotc_trace.cpp`), read by `tools/trace/sotc_trace.py`.
All integers are little endian. This is a developer document (English).

## Files

| Mode | File |
|---|---|
| `SOTC_TRACE=1` | `<trace dir>/<session>/frameNNNNNN_XXms.sotctrace` per slow frame; `manualNNN.sotctrace` per `RequestDump` |
| `SOTC_TRACE=2` | `<trace dir>/<session>/session.sotctrace`, appended while the game runs |

`<trace dir>` is `SOTC_TRACE_DIR`, else `<user dir>/trace`. `<session>` is `YYYYmmdd-HHMMSS`.
With `SOTC_TRACE_JSON=1`, mode 1 also writes the old Chrome JSON file with the same base name.

## Layout

```
magic   8 bytes   "SOTCTRC" 0x01
chunk*            until end of file
```

Chunk header, 16 bytes:

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | `type` |
| 4 | u32 | `flags`: bit 0 = payload is a zlib stream (miniz `mz_compress2`, level 1) |
| 8 | u32 | `stored_size`: payload bytes that follow the header |
| 12 | u32 | `raw_size`: payload size after inflating (equals `stored_size` when not compressed) |

A reader skips chunk types it does not know. A stream file may end without an `END` chunk (the
game was closed or crashed). A truncated last chunk is ignored.

## Chunk types

| Type | Name | Payload |
|---|---|---|
| 1 | `META` | UTF-8 JSON object. Several may appear; later keys replace earlier ones. |
| 2 | `THREADS` | UTF-8 JSON array of thread objects. Later entries with the same `id` replace earlier ones. |
| 3 | `NAMES` | UTF-8 JSON object: decimal id string → name. Later chunks add ids. |
| 4 | `EVENTS` | binary, one ring's events (below). |
| 5 | `ZONE_STATS` | UTF-8 JSON array, cumulative since the game started; the last one wins. |
| 6 | `IMPORT_STATS` | reserved (phase 3) |
| 7 | `STACKS` | reserved (phase 5) |
| 8 | `GPU_CALIB` | reserved (phase 2) |
| 9 | `MODULES` | UTF-8 JSON array of host modules, cumulative; the last one wins (phase 5, sampler) |
| 0xFFFFFFFF | `END` | empty; written when a file is closed cleanly |

### META keys

| Key | Meaning |
|---|---|
| `version` | 1 |
| `mode` | 1 or 2 |
| `kind` | `"dump"` or `"stream"` |
| `reason` | `"slow"` or `"manual: <text>"` (mode 1) |
| `frame`, `worst_ms`, `slow_frames` | the slow frame(s) of a dump |
| `window_ns` | `[start, end]` of a dump, in event time |
| `clock` | `"steady_ns"`: every time is `SotcStats::NowNs()` in ns |
| `image_base`, `image_size` | the game image (eboot first); guest addresses are `image_base + eboot offset` |
| `threshold_ms`, `pre_ms`, `post_ms` | the recorder's settings |
| `created_unix` | wall-clock seconds when the file was created |
| `stats` | the self-stats object (same as Lab `trace stats`) |

### THREADS entries

`{"id": u32, "os_tid": u32, "name": str, "kind": str, "sort": u32}`.
`kind` is one of `frames`, `guest`, `cp`, `draw`, `recorder`, `gpu`, `queue`.
`sort` orders the tracks in a viewer (lower first).

### NAMES

Ids are assigned per file by the writer, starting at 1. Id 0 means "no name".

### EVENTS payload

```
u32 ring_id     the THREADS id
u32 count
count × 48-byte record
```

Record (Python `struct` format `'<QQIBBHIIQQ'`):

| Offset | Type | Field |
|---|---|---|
| 0 | u64 | `t0` (ns) |
| 8 | u64 | `t1` (ns); equals `t0` for an instant |
| 16 | u32 | `name_id` (NAMES) |
| 20 | u8 | `kind` (below) |
| 21 | u8 | reserved, 0 |
| 22 | u16 | `flags` |
| 24 | u32 | `aux` |
| 28 | u32 | reserved, 0 |
| 32 | u64 | `a` |
| 40 | u64 | `b`: usually the guest return address |

Records of one chunk are in the order the ring holds them: the end-time order of the spans, except
that a merged op batch may follow spans that ended later. Readers sort by `t0` when they need to.

| `kind` | Meaning |
|---|---|
| 0 span | `[t0, t1)` on the ring's thread |
| 1 instant | `t0` |
| 2 counter | value in `a` |
| 3 sample | `aux` = stack id (phase 5) |
| 4 import | `aux` = import index (phase 3) |
| 5 gpu | GPU execution span (phase 2) |
| 6 frame | frames track: `a` = frame number, `b` = lap, name = Lab segment (`"frame"` when none), `flags` bit 0 = slow |
| 7 mark | frames track: name = mark text |

Extra tracks (`SotcTrace::NewTrack`) also appear in THREADS, with their own `kind`:
`gpu` (GPU execution timeline, phase 2) and `samples` (the sampler's samples of one thread; its
`os_tid` is the sampled thread's, so a reader pairs it with that thread's own track).

### IMPORT_STATS (type 6, phase 3)

UTF-8 JSON array, cumulative since the game started, the last chunk wins:
`{"index": u32, "name": str, "calls": u64, "total_ns": u64, "max_ns": u64}`.
`name` is the readable symbol from `src/import_names.inc` (else the NID). EvImport events
(kind 4, `SOTC_TRACE_IMPORTS=2`): `aux` = import index, `b` = guest caller, name = symbol name.

### STACKS (type 7, phase 5)

UTF-8 JSON object, incremental (later chunks add ids): `{"<stack id>": [addr, ...]}`, leaf
first, absolute addresses (guest addresses are inside `image_base`/`image_size`). EvSample events
(kind 3) live on `samples` tracks: `t0` = sample time, `aux` = stack id, `a` = leaf address,
`flags` bit 0 = the walk stopped early (truncated or unknown frame).

### GPU_CALIB (type 8, phase 2)

UTF-8 JSON object, informational: `{"timestamp_period_ns": f, "valid_bits": u32,
"calibrated": bool, "pairs": [[gpu_tick, cpu_ns], ...]}`. EvGpu events (kind 5) are already
converted to the CPU clock by the recorder: `[t0, t1)` = GPU execution, `a` = submit serial,
`b` = command count when known, name = what was measured (`"cmdbuf"`, `"batch"`).

### MODULES (type 9)

UTF-8 JSON array, cumulative: the last chunk wins. It lists the host modules only (the EXE and
every DLL of the process, from the sampler's Toolhelp snapshot, refreshed once a second, at most
256); the game image is not in it. Each entry:

`{"name": str, "base": u64, "size": u64, "preferred_base": u64}`

All numbers are decimal integers (not hex text). `name` is the module base name in UTF-8
(`"bb-probe.exe"`, `"ntdll.dll"`, the Vulkan driver DLL). `base` and `size` are the loaded image
(the Toolhelp entry); `preferred_base` is the PE OptionalHeader.ImageBase, or 0 when the header
did not parse (unknown).

A host sample address x (a STACKS address outside `image_base`/`image_size`, or the `a` of a
`samples` event) belongs to the module with `base <= x < base + size`. Its RVA is `x - base`.
Its slide is `base - preferred_base`: 0 for a module loaded at its preferred base, negative when
it was loaded below it. `llvm-symbolizer --obj=<file> --adjust-vma=<slide>` resolves the runtime
address x directly, because it shifts the file's addresses by the slide. The Python reader
(`tools/trace/sotc_trace.py`) does exactly that for `bb-probe.exe`, and names every other module
`name+0x<rva>` (DLLs have no DWARF here).

### ZONE_STATS entries

`{"name": str, "file": str, "line": u32, "calls": u64, "total_ns": u64, "max_ns": u64}`.

## Self-stats object

`{"mode", "rings", "events", "events_per_s", "bytes_per_s", "dropped", "windows_overwritten",
"dumps", "stream_bytes", "threshold_ms"}`. `dropped` counts events overwritten before mode 2's
writer read them. `windows_overwritten` counts mode 1 dumps whose window start was already
overwritten in at least one ring.
