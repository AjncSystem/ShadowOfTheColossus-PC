// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_TRACE=1, a flight recorder for slow frames (off by default).
//
// Every thread that takes part (guest threads in their waits and faults, the command processor,
// the draw thread, the recording thread, a watcher per GPU queue, the presenter) appends timed
// spans to its own ring buffer: one store per span, no lock. When a game frame takes longer than
// SOTC_TRACE_MS (default 60), a writer thread copies the spans of the window around it out of the
// rings and saves them as a Chrome trace (JSON) under <user dir>/trace/<session>/, one file per
// slow frame; ui.perfetto.dev or chrome://tracing open it. Each thread is one track, so the chain
// of waits that held the flip back can be read off directly.
//
// Settings: SOTC_TRACE_MS (threshold, default 60), SOTC_TRACE_PRE_MS (time kept before the slow
// frame starts, default 150), SOTC_TRACE_POST_MS (after it ends, default 40), SOTC_TRACE_MAX (files
// per session, default 40), SOTC_TRACE_DIR (instead of <user dir>/trace).
//
// Trace v2 (docs/sotc/TRACE-FORMAT.md):
//   SOTC_TRACE=1  flight recorder: one .sotctrace file per slow frame (or per RequestDump).
//   SOTC_TRACE=2  continuous: the whole session is streamed into <session>/session.sotctrace.
//   SOTC_TRACE_JSON=1  also writes the old Chrome JSON next to each dump (mode 1).
//   SOTC_TRACE_RING=<n>  ring size multiplier (1, 2, 4 or 8; default 1).
//   SOTC_TRACE_ZONES=1  the shadPS4 zone macros (ZoneScoped, RENDERER_TRACE, FIBER_ENTER...) feed
//                       the recorder; SOTC_TRACE_ZONE_MIN_US (default 20) keeps a zone as a span
//                       only when it lasts that long; every call is counted in the zone stats.
// The recorder no longer turns on the SOTC_FPS accounting: set SOTC_FPS=1 as well for the 5 s
// report. The writer prints one "SOTC TRACE:" self-stats line every 5 s while the recorder is on.
//
// Names passed to Span/Instant/Counter must be string literals or InternString results (only the
// pointer is stored).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace SotcTrace {

/// 0 off, 1 flight recorder, 2 continuous stream. Read once at static initialisation.
// Each flag reads the environment on its own: an `inline` variable must not be initialized from
// another one, because their dynamic-init order across translation units is not guaranteed (kOn
// read kMode before kMode's own init and came out false, leaving the whole recorder off).
inline const int kMode = [] {
    const char* v = std::getenv("SOTC_TRACE");
    if (!v) {
        return 0;
    }
    return v[0] == '1' ? 1 : v[0] == '2' ? 2 : 0;
}();
inline const bool kOn = [] {
    const char* v = std::getenv("SOTC_TRACE");
    return v && (v[0] == '1' || v[0] == '2');
}();
/// SOTC_TRACE_ZONES=1 (needs the recorder on).
inline const bool kZones = [] {
    const char* t = std::getenv("SOTC_TRACE");
    const char* z = std::getenv("SOTC_TRACE_ZONES");
    return t && (t[0] == '1' || t[0] == '2') && z && z[0] == '1';
}();
/// SOTC_TRACE_ZONE_MIN_US (default 20), in ns.
inline const uint64_t kZoneMinNs = [] {
    const char* v = std::getenv("SOTC_TRACE_ZONE_MIN_US");
    return (v && *v ? std::strtoull(v, nullptr, 10) : 20ULL) * 1000ULL;
}();

/// Event kinds stored in the rings and in the file (TRACE-FORMAT.md, EVENTS chunk).
enum EventKind : uint8_t {
    EvSpan = 0,
    EvInstant = 1,
    EvCounter = 2, ///< a = value
    EvSample = 3,  ///< aux = stack id (sampler, phase 5)
    EvImport = 4,  ///< aux = import index (phase 3)
    EvGpu = 5,     ///< GPU execution span (phase 2)
    EvFrame = 6,   ///< frames track: a = frame, b = lap, name = segment, flags bit 0 = slow
    EvMark = 7,    ///< frames track: name = mark text
};

uint64_t Now(); ///< steady clock, ns (the SotcStats::NowNs clock)

/// A span [t0, t1) on the calling thread. a and b are shown as hex arguments; a value that falls
/// in the game image is shown as an eboot offset.
void Span(const char* name, uint64_t t0, uint64_t t1, uint64_t a = 0, uint64_t b = 0);
/// Span that never allocates: dropped when the calling thread has no ring yet (the fault
/// handler; guest threads get theirs at start, ThreadInit).
void SpanIfReady(const char* name, uint64_t t0, uint64_t t1, uint64_t a = 0, uint64_t b = 0);
/// Creates the calling thread's ring now (guest thread start, the main guest thread).
void ThreadInit();
/// A point event on the calling thread.
void Instant(const char* name, uint64_t a = 0, uint64_t b = 0);

/// The game image (eboot first), for showing guest return addresses as eboot offsets.
void SetImage(uint64_t base, uint64_t size);
/// The image base SetImage recorded (0 before the loader reported it); kept with the recorder off.
uint64_t ImageBase();
/// The image size SetImage recorded (0 before the loader reported it).
uint64_t ImageSize();
/// The directory the session folder is created in (BbGpuConfig::user_dir + "/trace").
void SetUserDir(const char* user_dir);

/// Called by the presenter at every game flip: records the frame span and, when the frame took
/// at least SOTC_TRACE_MS, queues the dump of its window.
void FrameEnd(uint64_t frame, uint64_t start_ns, uint64_t end_ns);
/// The game's own flip (VideoOutDriver::Flip): an instant on the frames track, a = flip_count.
void FlipMark(uint64_t flip);

/// A value over time (EvCounter) on the calling thread's track.
void Counter(const char* name, uint64_t value);
/// A free-text mark on the frames track (Lab "trace mark", route segments). The text is copied.
void Mark(const char* text);
/// The Lab segment and lap stored with every following frame (EvFrame name and b). The text is
/// copied; nullptr or "" clears it.
void SetSegment(const char* segment, uint32_t lap);
/// Mode 1: queues a dump of the last SOTC_TRACE_PRE_MS up to now (reason in the file's META).
/// Mode 2: writes a mark and flushes the stream. False when off or SOTC_TRACE_MAX is reached.
bool RequestDump(const char* reason);
/// Live slow-frame threshold in ms (0: no automatic dumps). Any thread.
void SetThresholdMs(uint64_t ms);
uint64_t ThresholdMs();
/// A stable copy of a string that is not a literal (engine scope names, import names, marks).
/// Never freed; equal texts return the same pointer. Takes a lock; not for hot paths.
const char* InternString(const char* text, size_t len);
/// A message (shadPS4 TracyMessage): an instant whose name is the interned text.
void Message(const char* text, size_t len);

/// The last file the writer finished (serial 0: none yet). Any thread, no lock on the hot side.
struct DumpInfo {
    uint64_t serial = 0;
    uint64_t frame = 0;
    double worst_ms = 0;
    std::string path;
    std::string reason;
};
DumpInfo LastDump();
/// Self-stats as one JSON object (Lab "trace stats"): mode, rings, events/s, bytes/s, dropped,
/// overwritten windows, dumps written, stream bytes, threshold.
std::string StatsJson();

/// shadPS4 zone site (Tracy shim, gpu/shim/tracy/Tracy.hpp): one static instance per macro use.
/// Every call adds to calls/total_ns/max_ns; a call of at least kZoneMinNs is also a span.
struct ZoneSite {
    const char* name;
    const char* file;
    uint32_t line;
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> total_ns{0};
    std::atomic<uint64_t> max_ns{0};
    std::atomic<bool> registered{false};
    constexpr ZoneSite(const char* n, const char* f, uint32_t l) : name{n}, file{f}, line{l} {}
};
/// Adds the site to the list written in ZONE_STATS chunks (idempotent).
void RegisterZoneSite(ZoneSite* site);

/// RAII zone; reads no clock when SOTC_TRACE_ZONES is off.
struct ZoneScope {
    ZoneSite* site;
    uint64_t t0 = 0;
    explicit ZoneScope(ZoneSite* s) : site{s} {
        if (kZones) {
            t0 = Now();
        }
    }
    ~ZoneScope() {
        if (!t0) {
            return;
        }
        const uint64_t t1 = Now();
        const uint64_t d = t1 - t0;
        if (!site->registered.load(std::memory_order_relaxed)) {
            RegisterZoneSite(site);
        }
        site->calls.fetch_add(1, std::memory_order_relaxed);
        site->total_ns.fetch_add(d, std::memory_order_relaxed);
        if (d > site->max_ns.load(std::memory_order_relaxed)) {
            site->max_ns.store(d, std::memory_order_relaxed); // racy max: statistics only
        }
        if (d >= kZoneMinNs) {
            Span(site->name, t0, t1);
        }
    }
    ZoneScope(const ZoneScope&) = delete;
    ZoneScope& operator=(const ZoneScope&) = delete;
};

/// Any event kind on the calling thread's ring (like Span). Names: literal or InternString.
void PushEvent(uint8_t kind, const char* name, uint64_t t0, uint64_t t1, uint64_t a = 0,
               uint64_t b = 0, uint32_t aux = 0, uint16_t flags = 0);
/// An extra track written by one thread on behalf of something else (the GPU timeline, the
/// sampler's samples of a thread). kind is written verbatim in THREADS ("gpu", "samples", ...);
/// os_tid is the subject thread for "samples" tracks. Returns the track id (0 when off). Locks.
uint32_t NewTrack(const char* name, const char* kind, uint32_t os_tid);
/// Push on a NewTrack track. Single writer per track; never locks.
void PushEventOn(uint32_t track, uint8_t kind, const char* name, uint64_t t0, uint64_t t1,
                 uint64_t a = 0, uint64_t b = 0, uint32_t aux = 0, uint16_t flags = 0);
/// Extra file chunks (IMPORT_STATS, STACKS, GPU_CALIB...): the writer thread calls each provider
/// when it writes a dump, every 5 s in mode 2 and at exit; a non-empty result is one chunk.
using ChunkProvider = std::string (*)();
void RegisterChunkProvider(uint32_t chunk_type, ChunkProvider provider);
/// The rings of real threads (not frames, queues or extra tracks). Locks; not for hot paths.
struct ThreadInfo {
    uint32_t id;
    uint32_t os_tid;
    std::string name;
    std::string kind;
};
std::vector<ThreadInfo> Threads();

/// shadPS4 FIBER_ENTER/FIBER_EXIT (the command processor's queue tasks): the time between them
/// becomes a span named after the task on the calling thread. Nesting is not tracked (one open
/// fiber per thread, as the macros are used).
void FiberEnter(const char* name);
void FiberExit();

/// GPU queue watcher: Submitted is called right after a queue submit that signals `tick` on the
/// queue `queue` (an index from RegisterQueue); Completed by the watcher thread when the queue's
/// timeline reached `tick`. The span drawn for tick N runs from max(submit N, completion N-1) to
/// completion N: the queue executes its submissions in order.
uint32_t RegisterQueue(const char* name);
void Submitted(uint32_t queue, uint64_t tick, uint64_t now_ns);
void Completed(uint32_t queue, uint64_t first_tick, uint64_t last_tick, uint64_t now_ns);

/// RAII span on the calling thread; reads no clock when the recorder is off. NoAlloc: for code
/// that also runs inside the fault handler (SpanIfReady).
template <bool NoAlloc = false>
struct BasicScope {
    const char* name;
    uint64_t t0 = 0;
    uint64_t a, b;
    explicit BasicScope(const char* n, uint64_t a_ = 0, uint64_t b_ = 0) : name{n}, a{a_}, b{b_} {
        if (kOn) {
            t0 = Now();
        }
    }
    ~BasicScope() {
        if (t0) {
            if constexpr (NoAlloc) {
                SpanIfReady(name, t0, Now(), a, b);
            } else {
                Span(name, t0, Now(), a, b);
            }
        }
    }
    BasicScope(const BasicScope&) = delete;
    BasicScope& operator=(const BasicScope&) = delete;
};
using Scope = BasicScope<false>;
using ScopeNoAlloc = BasicScope<true>;

/// Per-thread work counters, bumped only while the recorder is on: bytes the buffer cache
/// uploaded from guest memory, and page protect calls (BindScope reports both per bind).
inline thread_local uint64_t tl_upload_bytes = 0;
inline thread_local uint64_t tl_protect_calls = 0;
inline void NoteUpload(uint64_t bytes) {
    if (kOn) {
        tl_upload_bytes += bytes;
    }
}
inline void NoteProtect() {
    if (kOn) {
        ++tl_protect_calls;
    }
}

/// A bind step kept when over 20 us, with a = bytes uploaded and b = protect calls inside it.
struct BindScope {
    const char* name;
    uint64_t t0 = 0, bytes0 = 0, protects0 = 0;
    explicit BindScope(const char* n) : name{n} {
        if (kOn) {
            t0 = Now();
            bytes0 = tl_upload_bytes;
            protects0 = tl_protect_calls;
        }
    }
    ~BindScope() {
        if (t0) {
            const uint64_t t1 = Now();
            if (t1 - t0 >= 20'000) {
                Span(name, t0, t1, tl_upload_bytes - bytes0, tl_protect_calls - protects0);
            }
        }
    }
    BindScope(const BindScope&) = delete;
    BindScope& operator=(const BindScope&) = delete;
};

/// Like Scope, but kept only when it took at least 20 us: for steps that run thousands of
/// times per frame (the parts of a draw), where only the expensive ones matter.
struct ScopeMin {
    static constexpr uint64_t kMinNs = 20'000;
    const char* name;
    uint64_t t0 = 0;
    uint64_t a;
    explicit ScopeMin(const char* n, uint64_t a_ = 0) : name{n}, a{a_} {
        if (kOn) {
            t0 = Now();
        }
    }
    ~ScopeMin() {
        if (t0) {
            const uint64_t t1 = Now();
            if (t1 - t0 >= kMinNs) {
                Span(name, t0, t1, a);
            }
        }
    }
    ScopeMin(const ScopeMin&) = delete;
    ScopeMin& operator=(const ScopeMin&) = delete;
};

/// Draw-thread back ops: thousands per frame, so short ones are merged. An op of at least
/// kLongNs gets its own span; runs of shorter ops become one "ops xN" span, closed at a gap of
/// more than kGapNs, at a long op, or after 512 ops.
struct OpBatch {
    static constexpr uint64_t kLongNs = 50'000;
    static constexpr uint64_t kGapNs = 20'000;
    uint64_t start = 0, end = 0;
    uint32_t count = 0;
    void Flush();
    void Note(const char* name, uint64_t t0, uint64_t t1, uint64_t a);
};

} // namespace SotcTrace
