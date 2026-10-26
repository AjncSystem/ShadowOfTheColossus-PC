// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_OP_STATS=1 counters, see sotc_opstats.h.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "video_core/sotc_opstats.h"
#include "video_core/sotc_stats.h"

namespace SotcOps {
namespace {

constexpr uint32_t NB = 8; // run length buckets: 1, 2, 3-4, 5-8, 9-16, 17-32, 33-64, 65+
constexpr uint32_t NumOpKinds = 5; // Func, Draw, DrawIndirect, Dispatch, DispatchIndirect
constexpr const char* kKindNames[NumKinds] = {"depth-only", "color", "color+depth", "none"};
constexpr double kReportSeconds = 5.0;

uint32_t Bucket(uint32_t len) {
    if (len <= 2) {
        return len - 1;
    }
    if (len <= 4) {
        return 2;
    }
    if (len <= 8) {
        return 3;
    }
    if (len <= 16) {
        return 4;
    }
    if (len <= 32) {
        return 5;
    }
    return len <= 64 ? 6 : 7;
}

/// Run levels: P = same pipeline + render state, D = P + same descriptors but for buffer offsets
/// and arguments, X = D + same offsets and addresses (only the draw arguments differ). A run is
/// broken by any dispatch; the strict D tracker is also broken by any other queued function.
struct Tracker {
    const char* name;
    bool strict;
    uint64_t sig = 0;
    uint32_t len = 0, kind = 0;
    uint64_t dispatch_serial = 0, func_serial = 0;
    uint64_t draws[NumKinds + 1][NB]{}; // draws in runs of each bucket; [NumKinds] = all kinds
    uint64_t runs[NumKinds + 1]{};

    void Flush() {
        if (len == 0) {
            return;
        }
        const uint32_t b = Bucket(len);
        draws[kind][b] += len;
        draws[NumKinds][b] += len;
        ++runs[kind];
        ++runs[NumKinds];
        len = 0;
    }
    /// Returns true when the draw continued the current run.
    bool Feed(uint64_t s, uint32_t k, uint64_t dser, uint64_t fser) {
        if (len != 0 && s == sig && k == kind && dser == dispatch_serial &&
            (!strict || fser == func_serial)) {
            ++len;
            return true;
        }
        Flush();
        sig = s;
        kind = k;
        dispatch_serial = dser;
        func_serial = fser;
        len = 1;
        return false;
    }
    void ResetCounts() {
        std::memset(draws, 0, sizeof(draws));
        std::memset(runs, 0, sizeof(runs));
    }
};

struct PassStat {
    uint64_t key = 0, depth_addr = 0, draws = 0, segments = 0, frames = 0, stamp = ~0ull;
    uint32_t kind = 0, slice = 0, width = 0, height = 0, num_color = 0;
    bool shadow = false, used = false;
};
constexpr uint32_t kPassSlots = 1024;

struct Rec {
    uint64_t full, stat;
    uint8_t kind;
    bool shadow;
};
struct FrameRec {
    std::vector<Rec> d;
    std::vector<uint64_t> sfull, sstat; // sorted hashes
};
struct MatchTotals {
    uint64_t same_kind[NumKinds]{}, shadow{}, total{}, positional{}, compared{};
};

struct State {
    uint64_t frame_seen = ~0ull;
    uint64_t window_start_ns = 0;
    // window sums
    uint64_t frames = 0;
    uint64_t ops[NumOpKinds]{};
    uint64_t dropped = 0;
    uint64_t drawn[NumKinds][2]{}; // [kind][indexed]
    uint64_t gpu_args = 0, shadow_draws = 0;
    uint64_t pass_segments = 0, pass_distinct = 0, shadow_pass_distinct = 0;
    uint64_t x_contig = 0, x_cont_pairs = 0;
    // draw shape per kind: indices, descriptor counts, draws with no image, size histogram
    uint64_t idx_sum[NumKinds]{}, buf_sum[NumKinds]{}, img_sum[NumKinds]{}, no_img[NumKinds]{};
    uint64_t idx_hist[NumKinds][5]{}; // <=36, <=144, <=576, <=2304, more indices
    uint64_t instanced = 0; // draws with more than one instance
    // draw/dispatch interleaving and dispatch shaders
    uint32_t last_class = 0; // 0 none, 1 draw, 2 dispatch
    uint64_t to_dispatch = 0, to_draw = 0;
    struct DispatchSlot {
        uint64_t hash = 0, n = 0, groups = 0, runs = 0;
        bool used = false;
    };
    std::array<DispatchSlot, 1024> dispatches;
    uint64_t dispatch_distinct = 0;
    uint64_t dispatch_overflow = 0, dispatch_runs = 0, dispatch_total = 0, last_dispatch_hash = 0;
    bool last_was_dispatch = false;
    // run trackers
    std::array<Tracker, 4> trackers{Tracker{"P pipeline+state", false},
                                    Tracker{"D descriptors, offsets/args free", false},
                                    Tracker{"X exact (args only differ)", false},
                                    Tracker{"D strict (any queued func breaks)", true}};
    // passes
    std::array<PassStat, kPassSlots> passes;
    uint64_t pass_overflow = 0;
    uint64_t cur_pass_key = 0;
    bool in_pass = false;
    uint64_t frame_index = 0;
    // run context
    uint64_t dispatch_serial = 0, func_serial = 0;
    // previous-draw args for the X contiguity count
    bool prev_args_valid = false;
    DrawNote prev_note{};
    // frame history
    FrameRec cur, p1, p2;
    MatchTotals m_full1, m_full2, m_stat1, m_stat2;
};
State g;

// Depth pre-pass analysis (defined at the end of this file).
void PpReset();
void PpCloseFrame();
void PpReport(double frames);
void PpBackOp(uint32_t kind);
void PpDispatch(uint64_t hash, uint64_t groups);
void PpDraw(const DrawNote& n);

std::atomic<uint64_t> cp_packets[512], cp_dwords[512];
struct NopSlot {
    std::atomic<uint64_t> key{0}; // payload + 1
    std::atomic<uint64_t> n{0}, dwords{0};
};
NopSlot nop_slots[32];

const char* OpcodeName(uint32_t idx) {
    switch (idx & 0xFF) {
    case 0x10: return "Nop";
    case 0x11: return "SetBase";
    case 0x12: return "ClearState";
    case 0x13: return "IndexBufferSize";
    case 0x15: return "DispatchDirect";
    case 0x16: return "DispatchIndirect";
    case 0x1D: return "AtomicGds";
    case 0x1E: return "Atomic";
    case 0x20: return "SetPredication";
    case 0x22: return "CondExec";
    case 0x24: return "DrawIndirect";
    case 0x25: return "DrawIndexIndirect";
    case 0x26: return "IndexBase";
    case 0x27: return "DrawIndex2";
    case 0x28: return "ContextControl";
    case 0x2A: return "IndexType";
    case 0x2C: return "DrawIndirectMulti";
    case 0x2D: return "DrawIndexAuto";
    case 0x2F: return "NumInstances";
    case 0x33: return "IndirectBufferConst";
    case 0x35: return "DrawIndexOffset2";
    case 0x37: return "WriteData";
    case 0x38: return "DrawIndexIndirectMulti";
    case 0x39: return "MemSemaphore";
    case 0x3c: return "WaitRegMem";
    case 0x3F: return "IndirectBuffer";
    case 0x40: return "CopyData";
    case 0x42: return "PfpSyncMe";
    case 0x46: return "EventWrite";
    case 0x47: return "EventWriteEop";
    case 0x48: return "EventWriteEos";
    case 0x4A: return "PreambleCntl";
    case 0x50: return "DmaData";
    case 0x58: return "AcquireMem";
    case 0x59: return "Rewind";
    case 0x68: return "SetConfigReg";
    case 0x69: return "SetContextReg";
    case 0x76: return "SetShReg";
    case 0x77: return "SetShRegOffset";
    case 0x78: return "SetQueueReg";
    case 0x79: return "SetUconfigReg";
    case 0x9d: return "DrawIndexIndirectCountMulti";
    default: return "other";
    }
}

PassStat* FindPass(const DrawNote& n) {
    uint32_t i = static_cast<uint32_t>(n.pass_key * 0x9E3779B97F4A7C15ull >> 54); // 10 bits
    for (uint32_t probe = 0; probe < 64; ++probe, i = (i + 1) & (kPassSlots - 1)) {
        PassStat& p = g.passes[i];
        if (!p.used) {
            p = {};
            p.used = true;
            p.key = n.pass_key;
            p.depth_addr = n.depth_addr;
            p.kind = n.kind;
            p.slice = n.slice;
            p.width = n.width;
            p.height = n.height;
            p.num_color = n.num_color;
            p.shadow = n.shadow;
            return &p;
        }
        if (p.key == n.pass_key) {
            return &p;
        }
    }
    return nullptr;
}

void MatchFrames(const FrameRec& cur, const FrameRec& prev, MatchTotals& full, MatchTotals& stat,
                 bool positional) {
    for (int pass = 0; pass < 2; ++pass) {
        const bool use_stat = pass == 1;
        const std::vector<uint64_t>& sorted = use_stat ? prev.sstat : prev.sfull;
        MatchTotals& m = use_stat ? stat : full;
        std::vector<uint8_t> used(sorted.size(), 0);
        for (const Rec& r : cur.d) {
            const uint64_t h = use_stat ? r.stat : r.full;
            size_t i = std::lower_bound(sorted.begin(), sorted.end(), h) - sorted.begin();
            while (i < sorted.size() && sorted[i] == h && used[i]) {
                ++i;
            }
            ++m.compared;
            if (i < sorted.size() && sorted[i] == h) {
                used[i] = 1;
                ++m.same_kind[r.kind];
                m.shadow += r.shadow;
                ++m.total;
            }
        }
        if (positional) {
            const size_t n = std::min(cur.d.size(), prev.d.size());
            for (size_t i = 0; i < n; ++i) {
                m.positional += (use_stat ? cur.d[i].stat == prev.d[i].stat
                                          : cur.d[i].full == prev.d[i].full);
            }
        }
    }
}

void PrintRuns(double frames) {
    for (const Tracker& t : g.trackers) {
        for (uint32_t k : {uint32_t{NumKinds}, uint32_t{KindDepthOnly}, uint32_t{KindColorDepth}}) {
            uint64_t total = 0;
            for (uint32_t b = 0; b < NB; ++b) {
                total += t.draws[k][b];
            }
            if (total == 0) {
                continue;
            }
            const double runs = double(t.runs[k]);
            // share of the draws that sit in runs of at least 2, 5, 9, 17, 33 draws
            double ge[5];
            uint64_t acc = 0;
            for (int i = 4; i >= 0; --i) {
                static const uint32_t first_bucket[5] = {1, 3, 4, 5, 6};
                // buckets >= first_bucket[i]
                acc = 0;
                for (uint32_t b = first_bucket[i]; b < NB; ++b) {
                    acc += t.draws[k][b];
                }
                ge[i] = 100.0 * double(acc) / double(total);
            }
            std::fprintf(stderr,
                         "SOTC OPS runs %-36s %-11s: %.0f draws/frame in %.0f runs/frame, avg "
                         "length %.2f; draws in runs >=2 %.0f%%, >=5 %.0f%%, >=9 %.0f%%, >=17 "
                         "%.0f%%, >=33 %.0f%%\n",
                         t.name, k == NumKinds ? "all" : kKindNames[k], double(total) / frames,
                         runs / frames, double(total) / runs, ge[0], ge[1], ge[2], ge[3], ge[4]);
        }
    }
}

void PrintMatch(const char* what, const MatchTotals& m, const uint64_t kind_draws[NumKinds],
                uint64_t shadow_draws, uint64_t draws) {
    if (m.compared == 0) {
        return;
    }
    std::fprintf(stderr,
                 "SOTC OPS same as %s: %.1f%% of draws (depth-only %.1f%%, color %.1f%%, "
                 "color+depth %.1f%%, shadow cascades %.1f%%)",
                 what, 100.0 * double(m.total) / double(draws),
                 kind_draws[0] ? 100.0 * double(m.same_kind[0]) / double(kind_draws[0]) : 0.0,
                 kind_draws[1] ? 100.0 * double(m.same_kind[1]) / double(kind_draws[1]) : 0.0,
                 kind_draws[2] ? 100.0 * double(m.same_kind[2]) / double(kind_draws[2]) : 0.0,
                 shadow_draws ? 100.0 * double(m.shadow) / double(shadow_draws) : 0.0);
    if (m.positional) {
        std::fprintf(stderr, ", same position %.1f%%", 100.0 * double(m.positional) / double(draws));
    }
    std::fprintf(stderr, "\n");
}

void Report(double seconds) {
    const double frames = double(g.frames);
    uint64_t draws = 0, kind_draws[NumKinds]{};
    for (uint32_t k = 0; k < NumKinds; ++k) {
        kind_draws[k] = g.drawn[k][0] + g.drawn[k][1];
        draws += kind_draws[k];
    }
    uint64_t indexed = 0;
    for (uint32_t k = 0; k < NumKinds; ++k) {
        indexed += g.drawn[k][1];
    }
    const uint64_t cmds = g.ops[1] + g.ops[2] + g.ops[3] + g.ops[4];
    std::fprintf(stderr,
                 "SOTC OPS window: %.0f frames in %.1f s. Per frame: %.0f BackOps (func %.0f, draw "
                 "%.0f, draw-indirect %.0f, dispatch %.0f, dispatch-indirect %.0f); draws recorded "
                 "%.0f (filtered/skipped %.0f, dropped %.0f), indexed %.0f%%, real GPU-argument "
                 "indirect %.0f\n",
                 frames, seconds,
                 double(g.ops[0] + cmds) / frames, g.ops[0] / frames, g.ops[1] / frames,
                 g.ops[2] / frames, g.ops[3] / frames, g.ops[4] / frames, draws / frames,
                 double(int64_t(g.ops[1] + g.ops[2]) - int64_t(draws + g.dropped)) / frames,
                 g.dropped / frames, draws ? 100.0 * double(indexed) / double(draws) : 0.0,
                 g.gpu_args / frames);
    std::fprintf(stderr,
                 "SOTC OPS targets per frame: depth-only %.0f (%.0f%%), color %.0f (%.0f%%), "
                 "color+depth %.0f (%.0f%%), none %.0f; sun-shadow-cascade draws %.0f (%.0f%% of "
                 "draws) in %.1f distinct passes\n",
                 kind_draws[0] / frames, draws ? 100.0 * kind_draws[0] / double(draws) : 0.0,
                 kind_draws[1] / frames, draws ? 100.0 * kind_draws[1] / double(draws) : 0.0,
                 kind_draws[2] / frames, draws ? 100.0 * kind_draws[2] / double(draws) : 0.0,
                 kind_draws[3] / frames, g.shadow_draws / frames,
                 draws ? 100.0 * double(g.shadow_draws) / double(draws) : 0.0,
                 g.shadow_pass_distinct / frames);
    std::fprintf(stderr,
                 "SOTC OPS passes per frame: %.1f distinct targets sets, %.1f segments (target "
                 "changes), %.1f draws per segment\n",
                 g.pass_distinct / frames, g.pass_segments / frames,
                 g.pass_segments ? double(draws) / double(g.pass_segments) : 0.0);
    // Top passes by draws.
    std::array<const PassStat*, kPassSlots> order{};
    size_t n = 0;
    for (const PassStat& p : g.passes) {
        if (p.used) {
            order[n++] = &p;
        }
    }
    std::sort(order.begin(), order.begin() + n,
              [](const PassStat* a, const PassStat* b) { return a->draws > b->draws; });
    for (size_t i = 0; i < std::min<size_t>(n, 10); ++i) {
        const PassStat& p = *order[i];
        std::fprintf(stderr,
                     "SOTC OPS pass %zu: %s%s depth %#llx slice %u %ux%u colors %u: %.0f "
                     "draws/frame in %.1f segments/frame, present in %.0f%% of frames\n",
                     i + 1, kKindNames[p.kind], p.shadow ? " SHADOW" : "",
                     static_cast<unsigned long long>(p.depth_addr), p.slice, p.width, p.height,
                     p.num_color, p.draws / frames, p.segments / frames,
                     100.0 * double(p.frames) / frames);
    }
    if (g.pass_overflow) {
        std::fprintf(stderr, "SOTC OPS pass table overflow: %llu draws not attributed\n",
                     static_cast<unsigned long long>(g.pass_overflow));
    }
    PrintRuns(frames);
    if (g.x_cont_pairs) {
        std::fprintf(stderr,
                     "SOTC OPS exact runs: %.0f draw pairs/frame with equal state, of which %.0f "
                     "(%.0f%%) have contiguous index ranges (mergeable into one draw)\n",
                     g.x_cont_pairs / frames, g.x_contig / frames,
                     100.0 * double(g.x_contig) / double(g.x_cont_pairs));
    }
    for (uint32_t k = 0; k < NumKinds; ++k) {
        const uint64_t dk = kind_draws[k];
        if (!dk) {
            continue;
        }
        std::fprintf(stderr,
                     "SOTC OPS shape %-11s: avg %.0f indices/draw (draws with <=36 %.0f%%, <=144 "
                     "%.0f%%, <=576 %.0f%%, <=2304 %.0f%%, more %.0f%%), %.1f buffers + %.1f "
                     "images bound, no image %.0f%%\n",
                     kKindNames[k], double(g.idx_sum[k]) / double(dk),
                     100.0 * g.idx_hist[k][0] / double(dk), 100.0 * g.idx_hist[k][1] / double(dk),
                     100.0 * g.idx_hist[k][2] / double(dk), 100.0 * g.idx_hist[k][3] / double(dk),
                     100.0 * g.idx_hist[k][4] / double(dk), double(g.buf_sum[k]) / double(dk),
                     double(g.img_sum[k]) / double(dk), 100.0 * g.no_img[k] / double(dk));
    }
    std::fprintf(stderr,
                 "SOTC OPS interleave per frame: %.0f draw->dispatch switches, %.0f "
                 "dispatch->draw; instanced draws %.0f; dispatches recorded %.0f in %.0f "
                 "same-shader runs, %llu distinct compute shaders in the window\n",
                 g.to_dispatch / frames, g.to_draw / frames, g.instanced / frames,
                 g.dispatch_total / frames, g.dispatch_runs / frames,
                 static_cast<unsigned long long>(g.dispatch_distinct));
    {
        std::array<const State::DispatchSlot*, 1024> dorder{};
        size_t dn = 0;
        for (const auto& d : g.dispatches) {
            if (d.used) {
                dorder[dn++] = &d;
            }
        }
        std::sort(dorder.begin(), dorder.begin() + dn,
                  [](auto* a, auto* b) { return a->n > b->n; });
        for (size_t i = 0; i < std::min<size_t>(dn, 12); ++i) {
            std::fprintf(stderr,
                         "SOTC OPS dispatch %zu: shader %016llx %.0f/frame in %.0f runs/frame, "
                         "avg %.0f groups\n",
                         i + 1, static_cast<unsigned long long>(dorder[i]->hash),
                         dorder[i]->n / frames, dorder[i]->runs / frames,
                         double(dorder[i]->groups) / double(dorder[i]->n));
        }
        if (g.dispatch_overflow) {
            std::fprintf(stderr, "SOTC OPS dispatch table overflow %llu\n",
                         static_cast<unsigned long long>(g.dispatch_overflow));
        }
    }
    PrintMatch("previous frame (full signature)", g.m_full1, kind_draws, g.shadow_draws, draws);
    PrintMatch("2 frames ago (full signature)", g.m_full2, kind_draws, g.shadow_draws, draws);
    PrintMatch("previous frame (no descriptor buffer offsets)", g.m_stat1, kind_draws,
               g.shadow_draws, draws);
    PrintMatch("2 frames ago (no descriptor buffer offsets)", g.m_stat2, kind_draws,
               g.shadow_draws, draws);
    // Command processor packets.
    uint64_t pk_total = 0, dw_total = 0;
    std::array<std::pair<uint64_t, uint32_t>, 512> pk{};
    for (uint32_t i = 0; i < 512; ++i) {
        const uint64_t p = cp_packets[i].exchange(0, std::memory_order_relaxed);
        dw_total += cp_dwords[i].exchange(0, std::memory_order_relaxed);
        pk[i] = {p, i};
        pk_total += p;
    }
    std::sort(pk.begin(), pk.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::fprintf(stderr,
                 "SOTC OPS CP per frame: %.0f PM4 packets (%.0f dwords), %.2f packets and %.1f "
                 "dwords per draw/dispatch op;",
                 pk_total / frames, dw_total / frames,
                 cmds ? double(pk_total) / double(cmds) : 0.0,
                 cmds ? double(dw_total) / double(cmds) : 0.0);
    for (size_t i = 0; i < 9 && pk[i].first; ++i) {
        std::fprintf(stderr, " %s%s %.0f", OpcodeName(pk[i].second),
                     pk[i].second >= 256 ? "(acb)" : "", pk[i].first / frames);
    }
    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "SOTC OPS CP Nop payloads per frame:");
    for (NopSlot& s : nop_slots) {
        const uint64_t key = s.key.load(std::memory_order_relaxed);
        const uint64_t n = s.n.exchange(0, std::memory_order_relaxed);
        const uint64_t dw = s.dwords.exchange(0, std::memory_order_relaxed);
        if (key && n) {
            std::fprintf(stderr, " %#x: %.0f packets/%.0f dwords;", unsigned(key - 1), n / frames,
                         dw / frames);
        }
    }
    std::fprintf(stderr, "\n");
    PpReport(frames);
}

void ResetWindow() {
    g.frames = 0;
    std::memset(g.ops, 0, sizeof(g.ops));
    g.dropped = 0;
    std::memset(g.drawn, 0, sizeof(g.drawn));
    g.gpu_args = g.shadow_draws = 0;
    g.pass_segments = g.pass_distinct = g.shadow_pass_distinct = 0;
    g.x_contig = g.x_cont_pairs = 0;
    std::memset(g.idx_sum, 0, sizeof(g.idx_sum));
    std::memset(g.buf_sum, 0, sizeof(g.buf_sum));
    std::memset(g.img_sum, 0, sizeof(g.img_sum));
    std::memset(g.no_img, 0, sizeof(g.no_img));
    std::memset(g.idx_hist, 0, sizeof(g.idx_hist));
    g.instanced = 0;
    g.to_dispatch = g.to_draw = 0;
    for (auto& d : g.dispatches) {
        d = {};
    }
    g.dispatch_overflow = g.dispatch_runs = g.dispatch_total = g.dispatch_distinct = 0;
    for (Tracker& t : g.trackers) {
        t.ResetCounts();
    }
    for (PassStat& p : g.passes) {
        p = {};
    }
    g.pass_overflow = 0;
    g.m_full1 = g.m_full2 = g.m_stat1 = g.m_stat2 = {};
    PpReset();
}

void CloseFrame() {
    PpCloseFrame();
    for (Tracker& t : g.trackers) {
        t.Flush();
    }
    g.in_pass = false;
    g.prev_args_valid = false;
    ++g.frame_index;
    // Compare with the two previous frames (skip empty frames: menus, loading).
    if (!g.cur.d.empty()) {
        g.cur.sfull.clear();
        g.cur.sstat.clear();
        for (const Rec& r : g.cur.d) {
            g.cur.sfull.push_back(r.full);
            g.cur.sstat.push_back(r.stat);
        }
        std::sort(g.cur.sfull.begin(), g.cur.sfull.end());
        std::sort(g.cur.sstat.begin(), g.cur.sstat.end());
        if (!g.p1.d.empty()) {
            MatchFrames(g.cur, g.p1, g.m_full1, g.m_stat1, true);
        }
        if (!g.p2.d.empty()) {
            MatchFrames(g.cur, g.p2, g.m_full2, g.m_stat2, false);
        }
        std::swap(g.p2, g.p1);
        std::swap(g.p1, g.cur);
    }
    g.cur.d.clear();
    ++g.frames;
    const uint64_t now = SotcStats::NowNs();
    if (g.window_start_ns == 0) {
        g.window_start_ns = now;
    } else if (double(now - g.window_start_ns) / 1e9 >= kReportSeconds) {
        Report(double(now - g.window_start_ns) / 1e9);
        ResetWindow();
        g.window_start_ns = now;
    }
}

} // namespace

void CpPacket(uint32_t opcode, uint32_t dwords, bool compute) {
    const uint32_t i = (opcode & 0xFF) + (compute ? 256 : 0);
    cp_packets[i].fetch_add(1, std::memory_order_relaxed);
    cp_dwords[i].fetch_add(dwords, std::memory_order_relaxed);
}

void NoteBackOp(uint32_t kind) {
    const uint64_t frame = SotcStats::cp_frame.load(std::memory_order_relaxed);
    if (frame != g.frame_seen) {
        if (g.frame_seen != ~0ull) {
            CloseFrame();
        }
        g.frame_seen = frame;
    }
    PpBackOp(kind);
    if (kind < NumOpKinds) {
        ++g.ops[kind];
    }
    if (kind == 0) {
        ++g.func_serial;
    } else if (kind >= 3) {
        ++g.dispatch_serial;
        g.in_pass = false;
        g.prev_args_valid = false;
        g.to_dispatch += g.last_class == 1;
        g.last_class = 2;
    } else {
        g.to_draw += g.last_class == 2;
        g.last_class = 1;
        g.last_was_dispatch = false;
    }
}

void CpNop(uint32_t dwords, uint32_t payload) {
    const uint64_t key = uint64_t{payload} + 1;
    for (NopSlot& s : nop_slots) {
        uint64_t cur = s.key.load(std::memory_order_relaxed);
        if (cur == 0) {
            s.key.compare_exchange_strong(cur, key, std::memory_order_relaxed);
            cur = s.key.load(std::memory_order_relaxed);
        }
        if (cur == key) {
            s.n.fetch_add(1, std::memory_order_relaxed);
            s.dwords.fetch_add(dwords, std::memory_order_relaxed);
            return;
        }
    }
}

void NoteDispatch(uint64_t hash, uint64_t groups) {
    PpDispatch(hash, groups);
    ++g.dispatch_total;
    const bool same = g.last_was_dispatch && g.last_dispatch_hash == hash;
    g.dispatch_runs += !same;
    uint32_t i = static_cast<uint32_t>(hash * 0x9E3779B97F4A7C15ull >> 54);
    State::DispatchSlot* slot = nullptr;
    for (uint32_t probe = 0; probe < 64; ++probe, i = (i + 1) & 1023) {
        State::DispatchSlot& d = g.dispatches[i];
        if (!d.used) {
            d.used = true;
            d.hash = hash;
            ++g.dispatch_distinct;
            slot = &d;
            break;
        }
        if (d.hash == hash) {
            slot = &d;
            break;
        }
    }
    if (slot) {
        ++slot->n;
        slot->groups += groups;
        slot->runs += !same;
    } else {
        ++g.dispatch_overflow;
    }
    g.last_was_dispatch = true;
    g.last_dispatch_hash = hash;
}

void NoteDrawDropped() {
    ++g.dropped;
    vb_desc = vb_exact = ib_desc = ib_exact = 0;
}

void NoteDraw(const DrawNote& n) {
    PpDraw(n);
    ++g.drawn[n.kind][n.indexed];
    if (!n.gpu_args) {
        g.idx_sum[n.kind] += n.count;
        g.idx_hist[n.kind][n.count <= 36 ? 0 : n.count <= 144 ? 1 : n.count <= 576 ? 2
                                                : n.count <= 2304 ? 3 : 4]++;
        g.instanced += n.instances > 1;
    }
    g.buf_sum[n.kind] += n.n_buf;
    g.img_sum[n.kind] += n.n_img;
    g.no_img[n.kind] += n.n_img == 0;
    g.gpu_args += n.gpu_args;
    g.shadow_draws += n.shadow;
    // Pass accounting.
    PassStat* p = FindPass(n);
    if (!p) {
        ++g.pass_overflow;
    } else {
        ++p->draws;
        if (p->stamp != g.frame_index) {
            p->stamp = g.frame_index;
            ++p->frames;
            ++g.pass_distinct;
            g.shadow_pass_distinct += p->shadow;
        }
        if (!g.in_pass || g.cur_pass_key != n.pass_key) {
            ++p->segments;
            ++g.pass_segments;
        }
    }
    g.in_pass = true;
    g.cur_pass_key = n.pass_key;
    // Runs.
    const uint64_t sigs[4] = {n.h_pipe, n.h_desc, n.h_exact, n.h_desc};
    bool x_continued = false;
    for (uint32_t i = 0; i < 4; ++i) {
        const bool cont = g.trackers[i].Feed(sigs[i], n.kind, g.dispatch_serial, g.func_serial);
        if (i == 2) {
            x_continued = cont;
        }
    }
    if (x_continued && g.prev_args_valid && n.indexed && !n.gpu_args && !g.prev_note.gpu_args) {
        ++g.x_cont_pairs;
        const DrawNote& q = g.prev_note;
        if (q.instances == 1 && n.instances == 1 && q.vertex_offset == n.vertex_offset &&
            q.first_instance == n.first_instance && n.first_index == q.first_index + q.count) {
            ++g.x_contig;
        }
    }
    g.prev_note = n;
    g.prev_args_valid = true;
    // Frame history.
    g.cur.d.push_back({Mix(n.h_exact, n.h_args), n.h_stat, static_cast<uint8_t>(n.kind), n.shadow});
    vb_desc = vb_exact = ib_desc = ib_exact = 0;
}


// ---------------------------------------------------------------------------------------------
// Depth pre-pass analysis (docs/sotc/ESCENA-TEMPLO.md, "Pre-pase de profundidad"). Diagnostic only.
// Per draw: raw state registers (depth/stencil/color control, write masks, discard in the pixel
// shader), per frame: pairing of the draws of the depth-only pre-pass with the draws of the G-buffer
// pass by geometry (IB address + index range [+ vertex buffers + instance index]), a timeline of
// passes/dispatches and every dispatch/draw/copy that touches a depth target (what reads the
// depth between the passes). Draw thread only, except NoteCpuAccess (atomics).
// ---------------------------------------------------------------------------------------------
namespace {

constexpr uint32_t kRanges = 16;
enum Role : uint8_t { RolePre, RoleGb, RoleShadow, RoleRo, RoleOther, NumRoles };
// PRE = depth-only draws that write depth (the pre-pass proper), DEPTH-RO = depth-only draws that
// do not write it (they test against it and may sample it: decals/volumes).
constexpr const char* kRoleNames[NumRoles] = {"PRE-Z", "GBUF", "SHADOW", "DEPTH-RO", "other"};
constexpr const char* kTouchNames[NumTouchKinds] = {"image", "buffer", "copy-src", "copy-dst",
                                                    "fill", "dma-shader"};
constexpr const char* kCmpNames[8] = {"NEVER", "LESS", "EQUAL", "LEQUAL",
                                      "GREATER", "NOTEQUAL", "GEQUAL", "ALWAYS"};
constexpr const char* kPhaseNames[5] = {"before-pre", "inside-pre", "pre..gbuf-end", "after-gbuf",
                                        "n/a"};

std::atomic<uint64_t> r_addr[kRanges], r_size[kRanges];
std::atomic<uint64_t> cpu_hits[kRanges][2]; // [range][read/write], window counters

struct RangeInfo {
    uint64_t addr = 0, size = 0;
    uint32_t pitch = 0, height = 0;
    bool shadow = false;
};

struct PRec {
    uint32_t seq;
    uint8_t role, range;
    uint32_t dctl, pctl, wmask, first_instance, count, slice;
    uint64_t g1, g2, vs, ps, vp, pos, mat, vpm, posc, lvp, flat;
    uint64_t vsbuf[12];
    uint32_t n_vsbuf;
};

/// One cascade slice of one frame, to compare with the previous frame.
struct ShadowAgg {
    uint64_t n = 0, geo = 0, geo_mat = 0, vpsum = 0, unknown = 0;
};
struct ShadowWin {
    uint64_t frames = 0, compared = 0, geo_same = 0, all_same = 0, vp_same = 0, draws = 0, unknown = 0;
    uint32_t range = 0, slice = 0;
};

struct TlEv {
    uint32_t seq0, seq1, n;
    uint8_t type; // 0 draw segment, 1 dispatch run
    uint8_t role, range;
    uint64_t key; // pass key / shader hash
    uint32_t dctl, wmask, flags;
};

struct TouchRow {
    uint32_t first_seq, last_seq;
    uint64_t n;
    uint8_t what, written, sw_stage, range, opclass;
    uint64_t hash;
};

struct SRow {
    uint32_t role, dctl, sctl, dsc, pctl, rctl, rovr, cctl, wmask, blend, mrt, flags, samples;
    uint32_t sref_f, sref_b;
    uint64_t n;
};
struct ShRow {
    uint32_t role;
    uint64_t vs, ps, n;
    uint32_t dctl;
};
struct PairRow {
    uint64_t a, b, n;
    uint32_t role;
};
struct ConsumerRow {
    uint64_t hash = 0, frames = 0, calls = 0, late_sum = 0, twins_sum = 0, pos_sum = 0;
    uint8_t what = 0, written = 0, opclass = 0;
};
struct WinTouch {
    TouchRow r;
    uint8_t phase;
    uint64_t frames; // sum over frames of rows
};

struct Pp {
    uint32_t seq = 0, cur_class = 0;
    uint64_t frame = 0;
    std::vector<PRec> recs;
    std::vector<TlEv> tl;
    std::unordered_map<uint64_t, TouchRow> touches;
    std::array<RangeInfo, kRanges> ranges;
    uint32_t num_ranges = 0;
    // timeline of the last closed frame
    std::vector<TlEv> last_tl;
    std::vector<TouchRow> last_touches;
    std::array<uint8_t, 64> last_touch_phase{};
    uint64_t last_frame_ops = 0, last_frame = 0, last_recs = 0;
    uint32_t last_bounds[kRanges][4]{};
    // window aggregates
    uint64_t frames_both = 0, frames_no_gb = 0, pre_total = 0, gb_total = 0, m1 = 0, m2 = 0;
    uint64_t other_pre_ranges = 0;
    uint64_t posc_known = 0, posc_eq = 0, buf_pairs = 0, buf_subset = 0, buf_total = 0, buf_in = 0;
    uint64_t pairs = 0, same_vs = 0, same_vp = 0, same_pctl = 0, same_inst = 0;
    uint64_t pos_known = 0, pos_eq = 0, mat_known = 0, mat_eq = 0, vpm_known = 0, vpm_eq = 0,
             all_eq = 0, all_known = 0;
    uint64_t pre_after_gb_start = 0, order_pairs = 0, order_inc = 0;
    uint64_t frames_pre_before_gb = 0;
    std::unordered_map<uint64_t, SRow> states;
    std::unordered_map<uint64_t, ShRow> shaders;
    std::unordered_map<uint64_t, PairRow> vs_pairs, pre_only, gb_only, pctl_diff;
    std::unordered_map<uint64_t, WinTouch> wtouch;
    std::unordered_map<uint64_t, ConsumerRow> consumers;
    std::unordered_map<uint64_t, ShadowAgg> shadow_prev;
    std::unordered_map<uint64_t, ShadowWin> shadow_win;
    uint64_t ps_total[NumRoles]{}, ps_discard[NumRoles]{}, ps_zexp[NumRoles]{}, ps_none[NumRoles]{},
             ps_uav[NumRoles]{};
    uint64_t role_draws[NumRoles]{};
};
Pp P;

uint32_t FindRange(const DrawNote& n) {
    const uint64_t addr = n.depth_write_addr ? n.depth_write_addr : n.depth_read_addr;
    for (uint32_t i = 0; i < P.num_ranges; ++i) {
        if (P.ranges[i].addr == addr) {
            return i;
        }
    }
    if (P.num_ranges >= kRanges || addr == 0) {
        return 255;
    }
    const uint32_t i = P.num_ranges++;
    RangeInfo& r = P.ranges[i];
    r.addr = addr;
    r.pitch = n.depth_pitch;
    r.height = n.depth_height;
    r.size = uint64_t{n.depth_pitch} * n.depth_height * 4;
    r.shadow = n.shadow;
    r_size[i].store(r.size, std::memory_order_relaxed);
    r_addr[i].store(addr, std::memory_order_release);
    return i;
}

std::string DepthDesc(uint32_t d) {
    char b[96];
    std::snprintf(b, sizeof(b), "de=%u zw=%u fn=%s st=%u db=%u", (d >> 1) & 1, (d >> 2) & 1,
                  kCmpNames[(d >> 4) & 7], d & 1, (d >> 3) & 1);
    return b;
}
std::string ShaderCtlDesc(uint32_t d) {
    char b[160];
    std::snprintf(b, sizeof(b),
                  "zexp=%u zorder=%u kill=%u cov2mask=%u maskexp=%u a2m_dis=%u depth_before_sh=%u "
                  "consz=%u",
                  d & 1, (d >> 4) & 3, (d >> 6) & 1, (d >> 7) & 1, (d >> 8) & 1, (d >> 11) & 1,
                  (d >> 12) & 1, (d >> 13) & 3);
    return b;
}

void PpReset() {
    P.frames_both = P.frames_no_gb = P.pre_total = P.gb_total = P.m1 = P.m2 = 0;
    P.other_pre_ranges = 0;
    P.pairs = P.same_vs = P.same_vp = P.same_pctl = P.same_inst = 0;
    P.posc_known = P.posc_eq = 0;
    P.buf_pairs = P.buf_subset = P.buf_total = P.buf_in = 0;
    P.pos_known = P.pos_eq = P.mat_known = P.mat_eq = P.vpm_known = P.vpm_eq = P.all_eq =
        P.all_known = 0;
    P.pre_after_gb_start = P.order_pairs = P.order_inc = P.frames_pre_before_gb = 0;
    P.states.clear();
    P.shaders.clear();
    P.vs_pairs.clear();
    P.pre_only.clear();
    P.gb_only.clear();
    P.pctl_diff.clear();
    P.wtouch.clear();
    P.consumers.clear();
    P.shadow_win.clear();
    std::memset(P.ps_total, 0, sizeof(P.ps_total));
    std::memset(P.ps_discard, 0, sizeof(P.ps_discard));
    std::memset(P.ps_zexp, 0, sizeof(P.ps_zexp));
    std::memset(P.ps_none, 0, sizeof(P.ps_none));
    std::memset(P.ps_uav, 0, sizeof(P.ps_uav));
    std::memset(P.role_draws, 0, sizeof(P.role_draws));
    for (auto& r : cpu_hits) {
        r[0].store(0, std::memory_order_relaxed);
        r[1].store(0, std::memory_order_relaxed);
    }
}

void PpBackOp(uint32_t kind) {
    ++P.seq;
    P.cur_class = kind == 0 ? 0 : kind >= 3 ? 2 : 1;
}

void PpDispatch(uint64_t hash, uint64_t groups) {
    (void)groups;
    if (!P.tl.empty() && P.tl.back().type == 1 && P.tl.back().key == hash) {
        P.tl.back().seq1 = P.seq;
        ++P.tl.back().n;
        return;
    }
    P.tl.push_back({P.seq, P.seq, 1, 1, 0, 0, hash, 0, 0, 0});
}

void PpDraw(const DrawNote& n) {
    const uint8_t role = n.shadow                   ? RoleShadow
                         : n.kind == KindDepthOnly  ? ((n.dctl >> 2) & 1 ? RolePre : RoleRo)
                         : n.kind == KindColorDepth ? RoleGb
                                                    : RoleOther;
    const uint32_t range = FindRange(n);
    ++P.role_draws[role];
    ++P.ps_total[role];
    P.ps_discard[role] += n.ps_discard;
    P.ps_zexp[role] += n.ps_zexport;
    P.ps_none[role] += !n.has_ps;
    P.ps_uav[role] += n.ps_uav;
    PRec r{};
    r.seq = P.seq;
    r.role = role;
    r.range = static_cast<uint8_t>(range);
    r.dctl = n.dctl;
    r.pctl = n.pctl;
    r.wmask = n.wmask;
    r.first_instance = n.first_instance;
    r.count = n.count;
    r.slice = n.slice;
    r.g1 = n.g1;
    r.g2 = n.g2;
    r.vs = n.vs_hash;
    r.ps = n.ps_hash;
    r.vp = n.vp_hash;
    r.pos = n.in_pos;
    r.mat = n.in_mat;
    r.vpm = n.in_vp;
    r.posc = n.in_posc;
    r.lvp = n.in_lvp;
    std::memcpy(r.vsbuf, n.vsbuf, sizeof(r.vsbuf));
    r.n_vsbuf = n.n_vsbuf;
    r.flat = n.in_flat;
    P.recs.push_back(r);
    // State table.
    const uint32_t flags = uint32_t{n.has_ps} | uint32_t{n.ps_discard} << 1 |
                           uint32_t{n.ps_zexport} << 2 | uint32_t{n.ps_other_export} << 3 |
                           uint32_t{n.bias} << 4 | uint32_t{n.gpu_args} << 5;
    SRow s{role, n.dctl, n.sctl, n.dsc, n.pctl, n.rctl, n.rovr, n.cctl, n.wmask, n.blend_en,
           n.mrt_mask, flags, uint32_t{n.num_samples} | uint32_t{n.depth_samples} << 8,
           n.sref_f, n.sref_b, 1};
    uint64_t h = 0x5354;
    h = MixBytes(h, &s, sizeof(s) - sizeof(s.n));
    auto [it, fresh] = P.states.try_emplace(h, s);
    if (!fresh) {
        ++it->second.n;
    }
    uint64_t hs = Mix(Mix(role + 77, n.vs_hash), n.ps_hash);
    auto [it2, fresh2] = P.shaders.try_emplace(hs, ShRow{role, n.vs_hash, n.ps_hash, 1, n.dctl});
    if (!fresh2) {
        ++it2->second.n;
    }
    // Timeline: a segment per pass-key change (draws between the same two dispatches).
    if (!P.tl.empty() && P.tl.back().type == 0 && P.tl.back().key == n.pass_key) {
        P.tl.back().seq1 = P.seq;
        ++P.tl.back().n;
        P.tl.back().flags += ((n.dctl >> 2) & 1) << 1;
    } else {
        P.tl.push_back({P.seq, P.seq, 1, 0, role, static_cast<uint8_t>(range), n.pass_key, n.dctl,
                        n.wmask, (n.has_ps ? 1u : 0u) | (((n.dctl >> 2) & 1) << 1)});
    }
}

void PpCloseFrame() {
    ++P.frame;
    uint32_t cnt_gb[kRanges]{}, cnt_pre[kRanges]{};
    for (const PRec& r : P.recs) {
        if (r.range < kRanges) {
            cnt_gb[r.range] += r.role == RoleGb;
            cnt_pre[r.range] += r.role == RolePre;
        }
    }
    uint32_t main = 0;
    for (uint32_t i = 1; i < kRanges; ++i) {
        if (cnt_gb[i] > cnt_gb[main]) {
            main = i;
        }
    }
    // Sequence bounds per range: pre first/last, gb first/last.
    uint32_t bounds[kRanges][4];
    for (auto& b : bounds) {
        b[0] = ~0u;
        b[1] = 0;
        b[2] = ~0u;
        b[3] = 0;
    }
    for (const PRec& r : P.recs) {
        if (r.range >= kRanges) {
            continue;
        }
        if (r.role == RolePre) {
            bounds[r.range][0] = std::min(bounds[r.range][0], r.seq);
            bounds[r.range][1] = std::max(bounds[r.range][1], r.seq);
        } else if (r.role == RoleGb) {
            bounds[r.range][2] = std::min(bounds[r.range][2], r.seq);
            bounds[r.range][3] = std::max(bounds[r.range][3], r.seq);
        }
    }
    std::vector<uint32_t> main_twin_gb;
    if (cnt_gb[main] > 0 && cnt_pre[main] > 0) {
        ++P.frames_both;
        std::vector<uint32_t> pre, gb;
        for (uint32_t i = 0; i < P.recs.size(); ++i) {
            const PRec& r = P.recs[i];
            if (r.range == main) {
                if (r.role == RolePre) {
                    pre.push_back(i);
                } else if (r.role == RoleGb) {
                    gb.push_back(i);
                }
            }
        }
        P.pre_total += pre.size();
        P.gb_total += gb.size();
        P.frames_pre_before_gb += bounds[main][1] < bounds[main][2];
        for (uint32_t i : pre) {
            P.pre_after_gb_start += P.recs[i].seq > bounds[main][2];
        }
        for (int level = 0; level < 2; ++level) {
            auto key = [&](const PRec& r) { return level == 0 ? r.g1 : r.g2; };
            std::vector<std::pair<uint64_t, uint32_t>> sorted;
            sorted.reserve(gb.size());
            for (uint32_t i : gb) {
                sorted.emplace_back(key(P.recs[i]), i);
            }
            std::sort(sorted.begin(), sorted.end());
            std::vector<uint8_t> used(sorted.size(), 0);
            std::vector<uint8_t> pre_matched(pre.size(), 0);
            uint32_t prev_gb_seq = 0;
            std::vector<uint32_t> twin_gb, twin_pre;
            for (size_t pi = 0; pi < pre.size(); ++pi) {
                const PRec& a = P.recs[pre[pi]];
                const uint64_t k = key(a);
                size_t j = std::lower_bound(sorted.begin(), sorted.end(),
                                            std::make_pair(k, uint32_t{0})) -
                           sorted.begin();
                while (j < sorted.size() && sorted[j].first == k && used[j]) {
                    ++j;
                }
                if (j >= sorted.size() || sorted[j].first != k) {
                    continue;
                }
                used[j] = 1;
                pre_matched[pi] = 1;
                const PRec& b = P.recs[sorted[j].second];
                (level == 0 ? P.m1 : P.m2) += 1;
                if (level == 1) {
                    twin_gb.push_back(b.seq);
                    twin_pre.push_back(a.seq);
                }
                if (level == 1) {
                    ++P.pairs;
                    P.same_vs += a.vs == b.vs;
                    P.same_vp += a.vp == b.vp;
                    P.same_pctl += a.pctl == b.pctl;
                    P.same_inst += (a.first_instance & 0xFFFF) == (b.first_instance & 0xFFFF);
                    const bool pk = a.pos && b.pos, mk = a.mat && b.mat, vk = a.vpm && b.vpm;
                    P.pos_known += pk;
                    P.pos_eq += pk && a.pos == b.pos;
                    {
                        // Every guest buffer (address, size, stride) of the position-only vertex
                        // stage is also bound by the G-buffer twin's vertex stage?
                        uint32_t in = 0;
                        for (uint32_t x = 0; x < a.n_vsbuf; ++x) {
                            for (uint32_t y = 0; y < b.n_vsbuf; ++y) {
                                if (a.vsbuf[x] == b.vsbuf[y]) {
                                    ++in;
                                    break;
                                }
                            }
                        }
                        ++P.buf_pairs;
                        P.buf_subset += in == a.n_vsbuf;
                        P.buf_total += a.n_vsbuf;
                        P.buf_in += in;
                    }
                    P.posc_known += a.posc && b.posc;
                    P.posc_eq += a.posc && b.posc && a.posc == b.posc;
                    P.mat_known += mk;
                    P.mat_eq += mk && a.mat == b.mat;
                    P.vpm_known += vk;
                    P.vpm_eq += vk && a.vpm == b.vpm;
                    if (pk && mk && vk) {
                        ++P.all_known;
                        P.all_eq += a.pos == b.pos && a.mat == b.mat && a.vpm == b.vpm;
                    }
                    if (a.pctl != b.pctl) {
                        auto [pit, pfresh] = P.pctl_diff.try_emplace(
                            Mix(a.pctl, b.pctl), PairRow{a.pctl, b.pctl, 1, 0});
                        if (!pfresh) {
                            ++pit->second.n;
                        }
                    }
                    const uint64_t hk = Mix(a.vs, b.vs);
                    auto [it, fresh] = P.vs_pairs.try_emplace(hk, PairRow{a.vs, b.vs, 1, 0});
                    if (!fresh) {
                        ++it->second.n;
                    }
                    if (prev_gb_seq) {
                        ++P.order_pairs;
                        P.order_inc += b.seq > prev_gb_seq;
                    }
                    prev_gb_seq = b.seq;
                }
            }
            if (level == 1) {
                // Bounds of the pre-pass proper and of the G-buffer pass: their twins only.
                if (!twin_gb.empty()) {
                    std::sort(twin_gb.begin(), twin_gb.end());
                    std::sort(twin_pre.begin(), twin_pre.end());
                    bounds[main][0] = twin_pre.front();
                    bounds[main][1] = twin_pre.back();
                    bounds[main][2] = twin_gb.front();
                    bounds[main][3] = twin_gb.back();
                    main_twin_gb = twin_gb;
                }
                for (size_t pi = 0; pi < pre.size(); ++pi) {
                    if (!pre_matched[pi]) {
                        const PRec& a = P.recs[pre[pi]];
                        auto [it, fresh] = P.pre_only.try_emplace(Mix(a.vs, a.ps),
                                                                  PairRow{a.vs, a.ps, 1, 0});
                        if (!fresh) {
                            ++it->second.n;
                        }
                    }
                }
                for (size_t j = 0; j < sorted.size(); ++j) {
                    if (!used[j]) {
                        const PRec& b = P.recs[sorted[j].second];
                        auto [it, fresh] = P.gb_only.try_emplace(Mix(b.vs, b.ps),
                                                                 PairRow{b.vs, b.ps, 1, 0});
                        if (!fresh) {
                            ++it->second.n;
                        }
                    }
                }
            }
        }
    } else if (!P.recs.empty()) {
        ++P.frames_no_gb;
    }
    // Shadow cascades: is a slice the same as in the previous frame (draw list, matrices, light
    // view-projection)? Only the draws of the known position-only VS carry matrix/VP hashes.
    {
        std::unordered_map<uint64_t, ShadowAgg> cur;
        for (const PRec& r : P.recs) {
            if (r.role != RoleShadow || r.range >= kRanges) {
                continue;
            }
            ShadowAgg& a = cur[(uint64_t{r.range} << 8) | (r.slice & 0xFF)];
            ++a.n;
            a.geo += Mix(r.g1, 0x67);
            a.geo_mat += Mix(Mix(r.g1, r.flat), r.lvp);
            a.vpsum += Mix(r.lvp, 0x76);
            a.unknown += !(r.flat && r.lvp);
        }
        for (const auto& [k, a] : cur) {
            ShadowWin& w = P.shadow_win[k];
            w.range = static_cast<uint32_t>(k >> 8);
            w.slice = static_cast<uint32_t>(k & 0xFF);
            ++w.frames;
            w.draws += a.n;
            w.unknown += a.unknown;
            const auto it = P.shadow_prev.find(k);
            if (it != P.shadow_prev.end()) {
                const ShadowAgg& b = it->second;
                ++w.compared;
                w.geo_same += a.n == b.n && a.geo == b.geo;
                w.vp_same += a.vpsum == b.vpsum;
                w.all_same += a.n == b.n && a.geo_mat == b.geo_mat && a.vpsum == b.vpsum &&
                              a.unknown == 0 && b.unknown == 0;
            }
        }
        P.shadow_prev.swap(cur);
    }
    // Touch rows: classify against the bounds of their range and fold into the window table.
    std::vector<TouchRow> rows;
    rows.reserve(P.touches.size());
    for (auto& [k, t] : P.touches) {
        rows.push_back(t);
    }
    std::sort(rows.begin(), rows.end(),
              [](const TouchRow& a, const TouchRow& b) { return a.first_seq < b.first_seq; });
    for (const TouchRow& t : rows) {
        uint8_t phase = 4;
        if (t.range < kRanges && bounds[t.range][1] != 0 && bounds[t.range][3] != 0) {
            const uint32_t* b = bounds[t.range];
            phase = t.first_seq < b[0]   ? 0
                    : t.first_seq <= b[1] ? 1
                    : t.first_seq < b[3]  ? 2
                                          : 3;
        }
        if (t.range == main && (phase == 1 || phase == 2) && t.opclass != 1 && !main_twin_gb.empty()) {
            // A dispatch/copy touching the main depth between the first pre-pass twin and the
            // last G-buffer twin: how many G-buffer twins are still to come.
            const size_t late = main_twin_gb.end() -
                                std::upper_bound(main_twin_gb.begin(), main_twin_gb.end(), t.first_seq);
            const uint64_t ck = Mix(Mix(t.hash, t.what), Mix(t.written, t.opclass));
            ConsumerRow& c = P.consumers[ck];
            c.hash = t.hash;
            c.what = t.what;
            c.written = t.written;
            c.opclass = t.opclass;
            c.frames += 1;
            c.calls += t.n;
            c.late_sum += late;
            c.twins_sum += main_twin_gb.size();
            c.pos_sum += t.first_seq - main_twin_gb.front();
        }
        uint64_t k = Mix(Mix(t.what, t.written), Mix(t.sw_stage, t.opclass));
        k = Mix(Mix(k, t.hash), Mix(t.range, phase));
        auto [it, fresh] = P.wtouch.try_emplace(k, WinTouch{t, phase, 1});
        if (!fresh) {
            it->second.r.n += t.n;
            ++it->second.frames;
        }
    }
    // Snapshot of the last frame for the timeline print.
    P.last_tl.swap(P.tl);
    P.last_touches = rows;
    P.last_recs = P.recs.size();
    P.last_frame_ops = P.seq;
    P.last_frame = P.frame;
    std::memcpy(P.last_bounds, bounds, sizeof(bounds));
    for (size_t i = 0; i < rows.size() && i < P.last_touch_phase.size(); ++i) {
        const TouchRow& t = rows[i];
        uint8_t phase = 4;
        if (t.range < kRanges && bounds[t.range][1] != 0 && bounds[t.range][3] != 0) {
            const uint32_t* b = bounds[t.range];
            phase = t.first_seq < b[0] ? 0 : t.first_seq <= b[1] ? 1 : t.first_seq < b[3] ? 2 : 3;
        }
        P.last_touch_phase[i] = phase;
    }
    P.tl.clear();
    P.recs.clear();
    P.touches.clear();
    P.seq = 0;
}

std::string RangeDesc(uint32_t r) {
    char b[96];
    if (r >= kRanges || r >= P.num_ranges) {
        return "R?";
    }
    std::snprintf(b, sizeof(b), "R%u(%#llx %ux%u%s)", r,
                  static_cast<unsigned long long>(P.ranges[r].addr), P.ranges[r].pitch,
                  P.ranges[r].height, P.ranges[r].shadow ? " shadow" : "");
    return b;
}

void PpReport(double frames) {
    const double f = frames;
    std::fprintf(stderr,
                 "SOTC PREPASS window: %llu frames with PRE and GBUF on the main depth, %llu with "
                 "draws but no pairing; draws/frame by role: PRE %.0f, GBUF %.0f, SHADOW %.0f, "
                 "other %.0f\n",
                 static_cast<unsigned long long>(P.frames_both),
                 static_cast<unsigned long long>(P.frames_no_gb), P.role_draws[RolePre] / f,
                 P.role_draws[RoleGb] / f, P.role_draws[RoleShadow] / f,
                 P.role_draws[RoleOther] / f);
    for (uint32_t r = 0; r < NumRoles; ++r) {
        if (!P.ps_total[r]) {
            continue;
        }
        std::fprintf(stderr,
                     "SOTC PREPASS ps %-8s: %.0f draws/frame, no pixel shader %.1f%%, pixel shader "
                     "with discard %.1f%%, pixel shader exports depth %.1f%%, pixel shader writes "
                     "storage images/buffers %.1f%%\n",
                     kRoleNames[r], P.ps_total[r] / f, 100.0 * P.ps_none[r] / P.ps_total[r],
                     100.0 * P.ps_discard[r] / P.ps_total[r],
                     100.0 * P.ps_zexp[r] / P.ps_total[r], 100.0 * P.ps_uav[r] / P.ps_total[r]);
    }
    // Ranges.
    for (uint32_t i = 0; i < P.num_ranges; ++i) {
        std::fprintf(stderr, "SOTC PREPASS range %s", RangeDesc(i).c_str());
        const uint64_t rd = cpu_hits[i][0].load(std::memory_order_relaxed);
        const uint64_t wr = cpu_hits[i][1].load(std::memory_order_relaxed);
        std::fprintf(stderr, ": guest CPU reads %.1f/frame, writes %.1f/frame\n", rd / f, wr / f);
    }
    // State table, top 5 per role.
    std::vector<const SRow*> rows;
    for (const auto& kv : P.states) {
        rows.push_back(&kv.second);
    }
    std::sort(rows.begin(), rows.end(), [](auto* a, auto* b) { return a->n > b->n; });
    for (uint32_t role = 0; role < NumRoles; ++role) {
        int shown = 0;
        for (const SRow* s : rows) {
            if (s->role != role || shown >= 6) {
                continue;
            }
            ++shown;
            std::fprintf(stderr,
                         "SOTC PREPASS state %-8s %.0f/frame: dctl %#x %s stfunc=%s bfunc=%s | "
                         "%s | stencilctl %#x stref %#x/%#x polyctl %#x "
                         "(cull %u bias %u) renderctl %#x override %#x colorctl %#x wmask %#08x "
                         "blend %#x mrt %#x ps %u discard %u zexp %u otherexp %u gpuargs %u "
                         "samples %u/%u\n",
                         kRoleNames[role], s->n / f, s->dctl, DepthDesc(s->dctl).c_str(),
                         kCmpNames[(s->dctl >> 8) & 7], kCmpNames[(s->dctl >> 24) & 7],
                         ShaderCtlDesc(s->dsc).c_str(), s->sctl, s->sref_f, s->sref_b, s->pctl,
                         (s->pctl & 3),
                         (s->flags >> 4) & 1, s->rctl, s->rovr, s->cctl, s->wmask, s->blend,
                         s->mrt, s->flags & 1, (s->flags >> 1) & 1, (s->flags >> 2) & 1,
                         (s->flags >> 3) & 1, (s->flags >> 5) & 1, s->samples & 0xFF,
                         s->samples >> 8);
        }
    }
    // Shader pairs, top 6 per role.
    std::vector<const ShRow*> srows;
    for (const auto& kv : P.shaders) {
        srows.push_back(&kv.second);
    }
    std::sort(srows.begin(), srows.end(), [](auto* a, auto* b) { return a->n > b->n; });
    for (uint32_t role = 0; role < NumRoles; ++role) {
        int shown = 0;
        for (const ShRow* s : srows) {
            if (s->role != role || shown >= 6) {
                continue;
            }
            ++shown;
            std::fprintf(stderr, "SOTC PREPASS shaders %-8s vs %08llx ps %08llx: %.0f/frame (%s)\n",
                         kRoleNames[role], static_cast<unsigned long long>(s->vs & 0xFFFFFFFF),
                         static_cast<unsigned long long>(s->ps & 0xFFFFFFFF), s->n / f,
                         DepthDesc(s->dctl).c_str());
        }
    }
    // Pairing.
    const double fb = std::max<double>(1.0, double(P.frames_both));
    std::fprintf(stderr,
                 "SOTC PREPASS pairing per frame (main depth, %llu frames): PRE-Z %.0f draws, GBUF "
                 "%.0f draws; matched by geometry (IB+range) %.0f; matched by IB+range+instance index+vertex "
                 "buffers %.0f (PRE-Z-only %.0f, GBUF-only %.0f); PRE-Z ends before the first GBUF draw in %.0f%% "
                 "of frames, PRE-Z draws after the first GBUF draw %.1f/frame\n",
                 static_cast<unsigned long long>(P.frames_both), P.pre_total / fb, P.gb_total / fb,
                 P.m1 / fb, P.m2 / fb, (double(P.pre_total) - double(P.m2)) / fb,
                 (double(P.gb_total) - double(P.m2)) / fb,
                 100.0 * P.frames_pre_before_gb / fb, P.pre_after_gb_start / fb);
    if (P.pairs) {
        const double pc = double(P.pairs);
        std::fprintf(stderr,
                     "SOTC PREPASS pairs (%.0f/frame): same VS shader %.1f%%, same viewport/"
                     "scissor regs %.1f%%, same polygon control %.1f%%, same instance index "
                     "%.1f%%, GBUF order follows PRE order %.1f%%\n",
                     P.pairs / fb, 100.0 * P.same_vs / pc, 100.0 * P.same_vp / pc,
                     100.0 * P.same_pctl / pc, 100.0 * P.same_inst / pc,
                     P.order_pairs ? 100.0 * P.order_inc / P.order_pairs : 0.0);
        std::fprintf(stderr,
                     "SOTC PREPASS position inputs of paired draws (known VS only): position "
                     "buffer V# address equal %llu/%llu (position bytes equal %llu/%llu), instance "
                     "matrix equal %llu/%llu, view-proj "
                     "constants equal %llu/%llu, all three equal %llu/%llu\n",
                     static_cast<unsigned long long>(P.pos_eq),
                     static_cast<unsigned long long>(P.pos_known),
                     static_cast<unsigned long long>(P.posc_eq),
                     static_cast<unsigned long long>(P.posc_known),
                     static_cast<unsigned long long>(P.mat_eq),
                     static_cast<unsigned long long>(P.mat_known),
                     static_cast<unsigned long long>(P.vpm_eq),
                     static_cast<unsigned long long>(P.vpm_known),
                     static_cast<unsigned long long>(P.all_eq),
                     static_cast<unsigned long long>(P.all_known));
    }
    if (P.buf_pairs) {
        std::fprintf(stderr,
                     "SOTC PREPASS paired draws whose pre-pass vertex-stage guest buffers (position "
                     "stream, matrix table) are all also bound by the G-buffer twin at the same "
                     "address, size and stride: %.2f%% of pairs (%.2f%% of buffers)\n",
                     100.0 * P.buf_subset / P.buf_pairs, 100.0 * P.buf_in / std::max<uint64_t>(1, P.buf_total));
    }
    auto print_pairs = [&](const char* what, std::unordered_map<uint64_t, PairRow>& m,
                           bool second_is_ps, uint64_t denom) {
        std::vector<const PairRow*> v;
        for (const auto& kv : m) {
            v.push_back(&kv.second);
        }
        std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->n > b->n; });
        for (size_t i = 0; i < std::min<size_t>(v.size(), 6); ++i) {
            std::fprintf(stderr, "SOTC PREPASS %s vs %08llx %s %08llx: %.1f/frame (%.1f%%)\n", what,
                         static_cast<unsigned long long>(v[i]->a & 0xFFFFFFFF),
                         second_is_ps ? "ps" : "-> gbuf vs",
                         static_cast<unsigned long long>(v[i]->b & 0xFFFFFFFF), v[i]->n / fb,
                         denom ? 100.0 * v[i]->n / denom : 0.0);
        }
    };
    print_pairs("pair", P.vs_pairs, false, P.pairs);
    {
        std::vector<const PairRow*> v;
        for (const auto& kv : P.pctl_diff) {
            v.push_back(&kv.second);
        }
        std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->n > b->n; });
        for (size_t i = 0; i < std::min<size_t>(v.size(), 6); ++i) {
            std::fprintf(stderr,
                         "SOTC PREPASS pairs with different polygon control: PRE %#llx (cull %llu) "
                         "-> GBUF %#llx (cull %llu): %.1f/frame\n",
                         static_cast<unsigned long long>(v[i]->a),
                         static_cast<unsigned long long>(v[i]->a & 3),
                         static_cast<unsigned long long>(v[i]->b),
                         static_cast<unsigned long long>(v[i]->b & 3), v[i]->n / fb);
        }
    }
    print_pairs("PRE-Z-only", P.pre_only, true, P.pre_total - P.m2);
    print_pairs("GBUF-only", P.gb_only, true, P.gb_total - P.m2);
    for (const auto& kv : P.shadow_win) {
        const ShadowWin& w = kv.second;
        const double c = std::max<double>(1.0, double(w.compared));
        std::fprintf(stderr,
                     "SOTC PREPASS shadow %s slice %u: %.0f draws/frame, vs previous frame: same "
                     "draw list %.1f%%, same light view-proj %.1f%%, same list + light matrix + VS user data (upper bound of redundancy) "
                     "%.1f%% (draws without readable inputs %.1f%%) over %llu frames\n",
                     RangeDesc(w.range).c_str(), w.slice, double(w.draws) / double(w.frames),
                     100.0 * w.geo_same / c, 100.0 * w.vp_same / c, 100.0 * w.all_same / c,
                     w.draws ? 100.0 * w.unknown / w.draws : 0.0,
                     static_cast<unsigned long long>(w.compared));
    }
    {
        std::vector<const ConsumerRow*> v;
        for (const auto& kv : P.consumers) {
            v.push_back(&kv.second);
        }
        std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->frames > b->frames; });
        for (const ConsumerRow* c : v) {
            std::fprintf(
                stderr,
                "SOTC PREPASS consumer of the main depth between the first pre-pass twin and the "
                "last G-buffer twin: %s %s%s shader %08llx, %.2f calls/frame, in %.0f%% of frames, "
                "at %.0f ops after the first G-buffer twin, G-buffer twins still to come: avg %.0f of %.0f\n",
                kTouchNames[c->what], c->written ? "WRITE" : "read",
                c->opclass == 2 ? " [dispatch]" : " [func]",
                static_cast<unsigned long long>(c->hash & 0xFFFFFFFF), double(c->calls) / f,
                100.0 * double(c->frames) / f, double(c->pos_sum) / double(c->frames),
                double(c->late_sum) / double(c->frames), double(c->twins_sum) / double(c->frames));
        }
    }
    // Touches.
    std::vector<const WinTouch*> t;
    for (const auto& kv : P.wtouch) {
        t.push_back(&kv.second);
    }
    std::sort(t.begin(), t.end(), [](auto* a, auto* b) { return a->r.n > b->r.n; });
    // Role of a pixel shader: the role it draws in most.
    auto ps_role = [&](uint64_t hash) {
        uint64_t best = 0;
        const char* name = "?";
        for (const auto& kv : P.shaders) {
            if (kv.second.ps == hash && kv.second.n > best) {
                best = kv.second.n;
                name = kRoleNames[kv.second.role];
            }
        }
        return name;
    };
    size_t shown_draw = 0, shown_other = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        const TouchRow& r = t[i]->r;
        if (r.range >= kRanges && r.what != TouchDma) {
            continue;
        }
        size_t& shown = r.opclass == 1 ? shown_draw : shown_other;
        if (shown++ >= (r.opclass == 1 ? 14u : 40u)) {
            continue;
        }
        std::fprintf(stderr,
                     "SOTC PREPASS touch %s %s%s by %s shader %08llx (sw stage %u, draws as %s) on %s: "
                     "%.1f/frame, phase %s, seen in %.0f%% of frames\n",
                     kTouchNames[r.what], r.written ? "WRITE " : "read ",
                     r.opclass == 1 ? "[draw]" : r.opclass == 2 ? "[dispatch]" : "[func]",
                     r.opclass == 2 ? "compute" : "graphics",
                     static_cast<unsigned long long>(r.hash & 0xFFFFFFFF), r.sw_stage,
                     r.opclass == 1 ? ps_role(r.hash) : "-", RangeDesc(r.range).c_str(),
                     double(r.n) / f, kPhaseNames[t[i]->phase],
                     100.0 * double(t[i]->frames) / f);
    }
    // Timeline of the last closed frame.
    std::fprintf(stderr, "SOTC PREPASS timeline of frame %llu (%llu ops, %llu draws recorded):\n",
                 static_cast<unsigned long long>(P.last_frame),
                 static_cast<unsigned long long>(P.last_frame_ops),
                 static_cast<unsigned long long>(P.last_recs));
    for (uint32_t r = 0; r < P.num_ranges; ++r) {
        const uint32_t* b = P.last_bounds[r];
        if (b[1] || b[3]) {
            std::fprintf(stderr,
                         "SOTC PREPASS   range %s: PRE ops %u..%u, GBUF ops %u..%u\n",
                         RangeDesc(r).c_str(), b[0] == ~0u ? 0 : b[0], b[1],
                         b[2] == ~0u ? 0 : b[2], b[3]);
        }
    }
    size_t lines = 0;
    for (size_t ei = 0; ei < P.last_tl.size(); ++ei) {
        const TlEv& e = P.last_tl[ei];
        if (lines++ >= 260) {
            std::fprintf(stderr, "SOTC PREPASS   ... (%zu more events)\n", P.last_tl.size() - ei);
            break;
        }
        if (e.type == 1) {
            // A block of consecutive dispatch runs: one line.
            size_t ej = ei;
            uint64_t total = 0, runs = 0;
            std::vector<uint64_t> distinct;
            while (ej < P.last_tl.size() && P.last_tl[ej].type == 1) {
                total += P.last_tl[ej].n;
                ++runs;
                if (std::find(distinct.begin(), distinct.end(), P.last_tl[ej].key) ==
                    distinct.end()) {
                    distinct.push_back(P.last_tl[ej].key);
                }
                ++ej;
            }
            std::fprintf(stderr, "SOTC PREPASS   ops %u..%u %llu dispatches in %llu runs, %zu shaders:",
                         e.seq0, P.last_tl[ej - 1].seq1, static_cast<unsigned long long>(total),
                         static_cast<unsigned long long>(runs), distinct.size());
            for (size_t k = 0; k < distinct.size() && k < 5; ++k) {
                std::fprintf(stderr, " %08llx",
                             static_cast<unsigned long long>(distinct[k] & 0xFFFFFFFF));
            }
            std::fprintf(stderr, "%s\n", distinct.size() > 5 ? " ..." : "");
            ei = ej - 1;
            continue;
        }
        if (e.type == 0) {
            std::fprintf(stderr,
                         "SOTC PREPASS   ops %u..%u draw-segment %s %s n=%u (first draw: %s | "
                         "wmask %#x ps %u; %u draws write depth)\n",
                         e.seq0, e.seq1, kRoleNames[e.role], RangeDesc(e.range).c_str(), e.n,
                         DepthDesc(e.dctl).c_str(), e.wmask, e.flags & 1, e.flags >> 1);
        } else {
            std::fprintf(stderr, "SOTC PREPASS   ops %u..%u dispatch shader %08llx x%u\n", e.seq0,
                         e.seq1, static_cast<unsigned long long>(e.key & 0xFFFFFFFF), e.n);
        }
    }
    lines = 0;
    for (size_t i = 0; i < P.last_touches.size(); ++i) {
        const TouchRow& r = P.last_touches[i];
        if (lines++ >= 60) {
            break;
        }
        std::fprintf(stderr,
                     "SOTC PREPASS   touch ops %u..%u x%llu %s %s%s shader %08llx on %s phase %s\n",
                     r.first_seq, r.last_seq, static_cast<unsigned long long>(r.n),
                     kTouchNames[r.what], r.written ? "WRITE" : "read",
                     r.opclass == 1 ? " [draw]" : r.opclass == 2 ? " [dispatch]" : " [func]",
                     static_cast<unsigned long long>(r.hash & 0xFFFFFFFF),
                     RangeDesc(r.range).c_str(),
                     kPhaseNames[i < P.last_touch_phase.size() ? P.last_touch_phase[i] : 4]);
    }
}

} // namespace

void NoteTouch(uint32_t what, uint64_t shader_hash, uint32_t sw_stage, bool written, uint64_t addr,
               uint64_t size) {
    if (size == 0) {
        size = 1;
    }
    const uint8_t opclass = static_cast<uint8_t>(P.cur_class);
    auto add = [&](uint32_t range) {
        uint64_t k = Mix(Mix(what, written), Mix(sw_stage, opclass));
        k = Mix(Mix(k, shader_hash), range);
        auto [it, fresh] = P.touches.try_emplace(
            k, TouchRow{P.seq, P.seq, 1, static_cast<uint8_t>(what), written,
                        static_cast<uint8_t>(sw_stage), static_cast<uint8_t>(range), opclass,
                        shader_hash});
        if (!fresh) {
            it->second.last_seq = P.seq;
            ++it->second.n;
        }
    };
    if (what == TouchDma) {
        add(255);
        return;
    }
    for (uint32_t i = 0; i < kRanges; ++i) {
        const uint64_t ra = r_addr[i].load(std::memory_order_acquire);
        if (!ra) {
            break;
        }
        const uint64_t rs = r_size[i].load(std::memory_order_relaxed);
        // A buffer V# can be a window as large as the whole mapping: only count one that starts
        // inside the target or is small (<= 32 MiB) and overlaps it.
        const bool big_buffer = what == TouchBuffer && size > (32ull << 20);
        const bool starts_inside = addr >= ra && addr < ra + rs;
        if (addr < ra + rs && addr + size > ra && (!big_buffer || starts_inside)) {
            add(i);
        }
    }
}

void NoteCpuAccess(bool write, uint64_t addr, uint64_t size) {
    if (size == 0) {
        size = 1;
    }
    for (uint32_t i = 0; i < kRanges; ++i) {
        const uint64_t ra = r_addr[i].load(std::memory_order_acquire);
        if (!ra) {
            break;
        }
        const uint64_t rs = r_size[i].load(std::memory_order_relaxed);
        if (addr < ra + rs && addr + size > ra) {
            cpu_hits[i][write ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
        }
    }
}

} // namespace SotcOps
