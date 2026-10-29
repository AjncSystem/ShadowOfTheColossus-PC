// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_OP_PHASES=1 (off by default). Measurement only, no behaviour change.
//
// The draw thread's ops (Rasterizer::Draw, DrawIndirect, DispatchDirect, DispatchIndirect) are
// timed with __rdtsc and split into phases: pipeline, render state, bind resources (with its
// buffer and texture parts), begin rendering, vertex and index, descriptor push, dynamic state,
// HLE and reset. Per window the header also counts what the bind path does: dirty buffer syncs
// and the bytes they upload, page protect calls, in-flight readbacks walked per written binding,
// and the render-target memo misses by reason (SotcStats::rs_memo_misses, which SOTC_OP_PHASES
// makes count without SOTC_FPS).
//
// Every 5 s, from the thread that runs the ops, three "SOTC OP PHASES" lines go to stderr. Only
// ops on the calling thread are accumulated (the thread-local state below); the counters that
// other threads can bump (buffer cache, page manager) count only while an op runs on this thread.
//
// Nothing here allocates or locks. Each probe reads kOn first, so with the switch off it is a
// load and a branch. With it on, a lean op takes about 28 timestamps plus 2 per buffer binding with =1
// (~7 ns each; about 2 ms per frame at 5,000 ops), about 28 with =2.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "video_core/sotc_stats.h" // sotc: rs_memo_misses

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace SotcOpPhases {

inline const bool kOn = [] {
    const char* v = std::getenv("SOTC_OP_PHASES");
    return v && (v[0] == '1' || v[0] == '2');
}();
/// SOTC_OP_PHASES=1 also times each buffer binding (clean / dirty / written: two timestamps per
/// binding, the largest share of the probes); =2 leaves those out, so the counter itself costs
/// less, and the buffer time stays inside "bind resources".
inline const bool kBindingProbes = [] {
    const char* v = std::getenv("SOTC_OP_PHASES");
    return v && v[0] == '1';
}();

/// Phases. The first nine are exclusive parts of an op. The buffer and texture phases are inside
/// PhBindResources and are printed as its parts. "other" is the op time left over.
enum Phase : unsigned {
    PhPipeline,
    PhRenderState,
    PhBeginRendering,
    PhBindResources,
    PhVertexIndex,
    PhDescriptorPush,
    PhDynamicState,
    PhHle,
    PhReset,
    PhBindBuffersClean,
    PhBindBuffersDirty,
    PhBindBuffersWritten,
    PhBindTextures,
    PhOther, ///< op time left over; computed when the window is printed, never charged
    PhPhases
};
inline constexpr unsigned kTopPhases = PhBindBuffersClean; ///< phases 0..8 are exclusive

enum OpKind : unsigned { OpDraw, OpDrawIndirect, OpDispatch, OpDispatchIndirect, OpKinds };

/// Counters the buffer cache, the page manager and the rasterizer bump (relaxed atomics).
enum Counter : unsigned {
    CtrDirtySyncs,    ///< SynchronizeMemoryFull calls that found bytes to upload
    CtrUploadBytes,   ///< bytes those calls uploaded
    CtrProtectCalls,  ///< PageManager::Protect calls
    CtrProtectPages,  ///< 4 KiB pages those calls covered
    CtrWalks,         ///< MarkInflightReadbacksDirty calls (one per written range)
    CtrWalkVisited,   ///< in-flight readbacks those calls looked at
    CtrCounters
};
inline std::atomic<uint64_t> counters[CtrCounters]{};

/// Reporting window. Each 5 s window prints rates over its own span.
inline constexpr uint64_t kWindowNs = 5'000'000'000ULL;

/// Per-thread accumulators: written by the thread that runs the ops only (no atomics needed).
struct ThreadState {
    uint32_t depth = 0;            ///< ops open on this thread
    uint32_t op_ends = 0;          ///< op ends seen (the clock is read every 64th)
    uint64_t upload_bytes = 0;     ///< bytes this thread's ops uploaded (dirty test per binding)
    uint64_t ops[OpKinds] = {};
    uint64_t op_ticks[OpKinds] = {};
    uint64_t phase_ticks[PhPhases] = {};
    bool started = false;          ///< the current window has begun
    uint64_t start_ns = 0;         ///< steady clock at the window start
    uint64_t start_tick = 0;       ///< TSC at the window start
    double ns_per_tick = 0;        ///< calibrated once, at the first report (against steady clock)
    uint64_t prev_counters[CtrCounters] = {};
    uint64_t prev_rs_misses[SotcStats::RsMissReasons] = {};
};
inline constinit thread_local ThreadState tls{};

inline uint64_t SteadyNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

/// One timestamp. Only ever called with kOn set (Now() checks it).
inline uint64_t Tick() {
#if defined(_MSC_VER)
    return __rdtsc();
#elif defined(__x86_64__) || defined(__i386__)
    return __builtin_ia32_rdtsc(); // clang and GCC: the instruction, no syscall
#else
    return SteadyNs();
#endif
}

/// The timestamp for a probe, or 0 when the switch is off (Charge then does nothing).
inline uint64_t Now() {
    return kOn ? Tick() : 0;
}

/// Adds the time since t0 to a phase of this thread's op. Ignored outside an op.
inline void Charge(Phase phase, uint64_t t0) {
    if (t0 != 0 && tls.depth != 0) {
        tls.phase_ticks[phase] += Tick() - t0;
    }
}

/// RAII phase. Does nothing when the switch is off.
struct Scope {
    Phase phase;
    uint64_t t0;
    explicit Scope(Phase p) : phase{p}, t0{Now()} {}
    ~Scope() {
        Charge(phase, t0);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

/// Charges a non-special buffer binding to clean, dirty (its upload bytes moved since upload0)
/// or written (the binding is a storage write).
inline void ChargeBinding(bool written, uint64_t upload0, uint64_t t0) {
    if (t0 == 0) {
        return;
    }
    const Phase phase = written                     ? PhBindBuffersWritten
                        : tls.upload_bytes != upload0 ? PhBindBuffersDirty
                                                      : PhBindBuffersClean;
    Charge(phase, t0);
}

/// The timestamp for a buffer-binding probe: 0 unless SOTC_OP_PHASES=1 (ChargeBinding then does
/// nothing).
inline uint64_t NowBinding() {
    return kBindingProbes ? Tick() : 0;
}

/// Upload bytes so far on this thread (0 when the switch is off).
inline uint64_t UploadBytes() {
    return kOn ? tls.upload_bytes : 0;
}

/// A dirty sync found bytes to upload (SynchronizeMemoryFull, on the thread running an op).
inline void NoteUpload(uint64_t bytes) {
    if (kOn && tls.depth != 0) {
        tls.upload_bytes += bytes;
        counters[CtrDirtySyncs].fetch_add(1, std::memory_order_relaxed);
        counters[CtrUploadBytes].fetch_add(bytes, std::memory_order_relaxed);
    }
}

/// A host protect call over `size` bytes (PageManager::Protect, on the thread running an op).
inline void NoteProtect(uint64_t size) {
    if (kOn && tls.depth != 0) {
        counters[CtrProtectCalls].fetch_add(1, std::memory_order_relaxed);
        counters[CtrProtectPages].fetch_add((size + 0xFFFULL) >> 12, std::memory_order_relaxed);
    }
}

/// MarkInflightReadbacksDirty walked `readbacks` in-flight readbacks for one written range.
inline void NoteWalk(uint64_t readbacks) {
    if (kOn && tls.depth != 0) {
        counters[CtrWalks].fetch_add(1, std::memory_order_relaxed);
        counters[CtrWalkVisited].fetch_add(readbacks, std::memory_order_relaxed);
    }
}

inline void ResetWindow(uint64_t now_tick, uint64_t now_ns) {
    ThreadState& t = tls;
    for (auto& v : t.ops) {
        v = 0;
    }
    for (auto& v : t.op_ticks) {
        v = 0;
    }
    for (auto& v : t.phase_ticks) {
        v = 0;
    }
    for (unsigned c = 0; c < CtrCounters; ++c) {
        t.prev_counters[c] = counters[c].load(std::memory_order_relaxed);
    }
    for (unsigned r = 0; r < SotcStats::RsMissReasons; ++r) {
        t.prev_rs_misses[r] = SotcStats::rs_memo_misses[r].load(std::memory_order_relaxed);
    }
    t.start_ns = now_ns;
    t.start_tick = now_tick;
    t.started = true;
}

/// Prints the window's rates (called on the op thread when a window is due).
inline void PrintWindow(uint64_t now_tick, uint64_t now_ns) {
    ThreadState& t = tls;
    const double secs = static_cast<double>(now_ns - t.start_ns) * 1e-9;
    if (t.ns_per_tick == 0 && now_tick != t.start_tick) {
        t.ns_per_tick = static_cast<double>(now_ns - t.start_ns) /
                        static_cast<double>(now_tick - t.start_tick);
    }
    const double npt = t.ns_per_tick;
    const auto ms_per_s = [&](uint64_t ticks) {
        return static_cast<double>(ticks) * npt * 1e-6 / secs;
    };
    const auto per_s = [&](uint64_t n) { return static_cast<double>(n) / secs; };
    const auto us_per_op = [&](unsigned k) {
        return t.ops[k] ? static_cast<double>(t.op_ticks[k]) * npt * 1e-3 /
                              static_cast<double>(t.ops[k])
                        : 0.0;
    };

    std::fprintf(stderr,
                 "SOTC OP PHASES %.1f s | draw %.0f/s %.2f us | draw_indirect %.0f/s %.2f us | "
                 "dispatch %.0f/s %.2f us | dispatch_indirect %.0f/s %.2f us\n",
                 secs, per_s(t.ops[OpDraw]), us_per_op(OpDraw), per_s(t.ops[OpDrawIndirect]),
                 us_per_op(OpDrawIndirect), per_s(t.ops[OpDispatch]), us_per_op(OpDispatch),
                 per_s(t.ops[OpDispatchIndirect]), us_per_op(OpDispatchIndirect));

    uint64_t op_total = 0;
    for (const uint64_t v : t.op_ticks) {
        op_total += v;
    }
    uint64_t top_total = 0;
    for (unsigned p = 0; p < kTopPhases; ++p) {
        top_total += t.phase_ticks[p];
    }
    t.phase_ticks[PhOther] = op_total > top_total ? op_total - top_total : 0;
    std::fprintf(stderr,
                 "SOTC OP PHASES ms/s | pipeline %.1f | render_state %.1f | begin_rendering %.1f | "
                 "bind_resources %.1f (buffers clean %.1f dirty %.1f written %.1f, textures %.1f) | "
                 "vertex_index %.1f | descriptor_push %.1f | dynamic_state %.1f | hle %.1f | "
                 "reset %.1f | other %.1f\n",
                 ms_per_s(t.phase_ticks[PhPipeline]), ms_per_s(t.phase_ticks[PhRenderState]),
                 ms_per_s(t.phase_ticks[PhBeginRendering]),
                 ms_per_s(t.phase_ticks[PhBindResources]),
                 ms_per_s(t.phase_ticks[PhBindBuffersClean]),
                 ms_per_s(t.phase_ticks[PhBindBuffersDirty]),
                 ms_per_s(t.phase_ticks[PhBindBuffersWritten]),
                 ms_per_s(t.phase_ticks[PhBindTextures]), ms_per_s(t.phase_ticks[PhVertexIndex]),
                 ms_per_s(t.phase_ticks[PhDescriptorPush]),
                 ms_per_s(t.phase_ticks[PhDynamicState]), ms_per_s(t.phase_ticks[PhHle]),
                 ms_per_s(t.phase_ticks[PhReset]), ms_per_s(t.phase_ticks[PhOther]));

    // Counter deltas. The RS memo misses are also read by SOTC_FPS, which resets them with
    // exchange(0): a value below the last one seen is taken as a fresh count.
    uint64_t delta[CtrCounters];
    for (unsigned c = 0; c < CtrCounters; ++c) {
        const uint64_t cur = counters[c].load(std::memory_order_relaxed);
        delta[c] = cur - t.prev_counters[c];
        t.prev_counters[c] = cur;
    }
    uint64_t rs[SotcStats::RsMissReasons];
    for (unsigned r = 0; r < SotcStats::RsMissReasons; ++r) {
        const uint64_t cur = SotcStats::rs_memo_misses[r].load(std::memory_order_relaxed);
        rs[r] = cur >= t.prev_rs_misses[r] ? cur - t.prev_rs_misses[r] : cur;
        t.prev_rs_misses[r] = cur;
    }
    const double walks = static_cast<double>(delta[CtrWalks]);
    std::fprintf(
        stderr,
        "SOTC OP PHASES /s | dirty_syncs %.1f upload_kb %.1f | protects %.1f pages %.1f | "
        "inflight walks %.1f visited %.1f (%.2f per walk) | rs_miss empty %.1f tick %.1f "
        "resources %.1f regs %.1f pass %.1f image %.1f meta %.1f\n",
        per_s(delta[CtrDirtySyncs]), per_s(delta[CtrUploadBytes]) / 1024.0,
        per_s(delta[CtrProtectCalls]), per_s(delta[CtrProtectPages]), per_s(delta[CtrWalks]),
        per_s(delta[CtrWalkVisited]), walks > 0 ? static_cast<double>(delta[CtrWalkVisited]) / walks
                                                 : 0.0,
        per_s(rs[SotcStats::RsMissNone]), per_s(rs[SotcStats::RsMissTick]),
        per_s(rs[SotcStats::RsMissResources]), per_s(rs[SotcStats::RsMissRegs]),
        per_s(rs[SotcStats::RsMissPass]), per_s(rs[SotcStats::RsMissImage]),
        per_s(rs[SotcStats::RsMissMeta]));
}

/// Called at every op end. The clock is read on every 64th end only.
inline void Report(uint64_t now_tick) {
    const uint64_t now_ns = SteadyNs();
    if (!tls.started) {
        ResetWindow(now_tick, now_ns);
        return;
    }
    if (now_ns - tls.start_ns < kWindowNs) {
        return;
    }
    PrintWindow(now_tick, now_ns);
    ResetWindow(now_tick, now_ns);
}

/// Times one op of the draw thread and counts it. Does nothing when the switch is off.
struct OpScope {
    OpKind kind;
    uint64_t t0;
    explicit OpScope(OpKind k) : kind{k}, t0{Now()} {
        if (t0 != 0) {
            ++tls.depth;
        }
    }
    ~OpScope() {
        if (t0 == 0) {
            return;
        }
        const uint64_t t1 = Tick();
        --tls.depth;
        ++tls.ops[kind];
        tls.op_ticks[kind] += t1 - t0;
        if ((++tls.op_ends & 63u) == 0) {
            Report(t1);
        }
    }
    OpScope(const OpScope&) = delete;
    OpScope& operator=(const OpScope&) = delete;
};

} // namespace SotcOpPhases
