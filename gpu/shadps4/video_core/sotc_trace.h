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
// per session, default 40), SOTC_TRACE_DIR (instead of <user dir>/trace). SOTC_TRACE=1 also turns on
// the SOTC_FPS accounting (kFpsLog), whose timers feed most spans.
//
// Names passed to Span/Instant must be string literals (only the pointer is stored).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace SotcTrace {

inline const bool kOn = [] {
    const char* v = std::getenv("SOTC_TRACE");
    return v && v[0] == '1';
}();

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
/// The directory the session folder is created in (BbGpuConfig::user_dir + "/trace").
void SetUserDir(const char* user_dir);

/// Called by the presenter at every game flip: records the frame span and, when the frame took
/// at least SOTC_TRACE_MS, queues the dump of its window.
void FrameEnd(uint64_t frame, uint64_t start_ns, uint64_t end_ns);

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
