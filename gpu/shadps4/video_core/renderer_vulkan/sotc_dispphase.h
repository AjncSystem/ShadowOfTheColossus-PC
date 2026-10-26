// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_DISP_PHASES=1, TSC timers per phase of a compute dispatch on the draw thread, by
// class (replicator HLE, other direct, indirect) and per shader (docs/sotc/ESCENA-TEMPLO.md,
// "Despachos"). Diagnostic only: with the variable unset every hook is a load of an inline const
// and a predictable branch. Draw thread only (no atomics). Printed to stderr every ~5 s.
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace DispPhase {

inline const bool kOn = [] {
    const char* v = std::getenv("SOTC_DISP_PHASES");
    return v && v[0] != '\0' && v[0] != '0';
}();

enum Phase : uint32_t {
    Pre,      // pop pending ops, pipeline resolve, stats, env checks
    Hle,      // ExecuteShaderHLE (the replicator runs here)
    BrPre,    // BindResources: copy/clear checks, user data checks, MakeUserData, set_writes
    BbClamp,  // BindBuffers: ClampRangeSize
    BbObtR,   // BindBuffers: ObtainBuffer, read-only binding
    BbObtW,   // BindBuffers: ObtainBuffer, written binding
    BbWrite,  // BindBuffers: InvalidateMemoryFromGPU + NoteHostWrite (written bindings)
    BbFlat,   // BindBuffers: flat buffer / special buffer copies
    BbRest,   // BindBuffers: everything else (IsBufferAccessed, descriptor setup)
    BtTex,    // BindTextures: the rest (memo store, UI linear check, samplers)
    BtPre,    // BindTextures: lock, memo key and replay
    BtFind,   // BindTextures: first pass (image lookups)
    BtBind,   // BindTextures: second pass (BindResolvedImage, transitions)
    BrDma,    // DMA range synchronisation
    Barrier,  // FlushBarriers
    EndRend,  // EndRendering
    PipeBind, // Pipeline::BindResources (push constants + descriptor push)
    Issue,    // bindPipeline + dispatch + stats
    PostReset, // ResetBindings
    Post,     // FlushPeriodic
    NumPhases,
};
inline constexpr const char* kNames[NumPhases] = {"pre",   "hle",    "brpre",  "clamp", "obtR",  "obtW",
                                                  "wrnote", "flat",  "bbrest", "btex", "btpre", "btfind", "btbind", "dma",   "barrier",
                                                  "endrend", "pipe", "issue",  "reset", "post"};
inline constexpr uint32_t NumClasses = 3; // 0 replicator HLE, 1 direct, 2 indirect

struct ShaderRow {
    uint64_t hash{};
    uint64_t n{}, ticks{}, groups{}, bufs{}, wbufs{}, imgs{}, barriers{}, rendering{};
    uint64_t obt_ticks{}, btex_ticks{}, post_ticks{};
};

/// Replicator HLE (vk_shader_hle.cpp) and ObtainBuffer (buffer_cache.cpp) sub-counters.
struct Sub {
    uint64_t hle_n{}, hle_direct{}, hle_pattern{}, hle_other{};
    uint64_t hle_total{}, hle_pre{}, hle_pattern_build{}, hle_cmp{}, hle_flush{};
    uint64_t hle_cmp_bytes{}, hle_repl_bytes{}, hle_big_n{}, hle_big_ticks{}, hle_table_bytes{};
    // ObtainBuffer by [written]: calls, calls that uploaded, ticks of each stage.
    uint64_t ob_n[2]{}, ob_up[2]{}, ob_arena[2]{}, ob_sync[2]{}, ob_sync_up[2]{}, ob_mark[2]{};
    // sotc (DISPATCH-FASTPATH.md): inside the uploading calls of SynchronizeMemory, by [written].
    uint64_t up_collect[2]{}, up_flush[2]{}, up_commit[2]{}, up_ranges[2]{}, up_bytes[2]{};
    // TextureCache::FindImage inside a dispatch, by outcome: 0 memo hit, 1 perfect match without a
    // memo, 2 overlap resolved to an image, 3 resolution failed / new image; plus Expand/Free calls.
    uint64_t fi_n[4]{}, fi_ticks[4]{}, fi_expand{}, fi_free{};
};
inline Sub& Subs() {
    static Sub s;
    return s;
}

struct State {
    bool active{};
    uint64_t last{};
    std::array<uint64_t, NumPhases> t{};
    std::array<std::array<uint64_t, NumPhases>, NumClasses> total{};
    std::array<uint64_t, NumClasses> count{};
    std::vector<ShaderRow> rows = std::vector<ShaderRow>(2048);
    uint64_t window_dispatches{};
    uint64_t last_tsc{}, last_ns{};
    uint32_t uses_dma{};
    uint64_t sum_bufs{}, sum_wbufs{}, sum_imgs{}, sum_barrier{}, sum_rendering{}, sum_dma{};
};
inline State& S() {
    static State s;
    return s;
}
inline uint64_t Now() {
    return __builtin_ia32_rdtsc();
}
inline void Begin() {
    if (!kOn) {
        return;
    }
    auto& s = S();
    s.active = true;
    s.t.fill(0);
    s.last = Now();
}
inline void Mark(Phase p) {
    if (!kOn) {
        return;
    }
    auto& s = S();
    if (!s.active) {
        return;
    }
    const uint64_t now = Now();
    s.t[p] += now - s.last;
    s.last = now;
}
inline bool Active() {
    return kOn && S().active;
}
/// Drop: the dispatch ended early (nothing recorded); its phases are discarded.
inline void Abort() {
    if (kOn) {
        S().active = false;
    }
}
inline void Print(State& s) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    const uint64_t tsc = Now();
    double us_per_tick = 0.0;
    if (s.last_ns != 0 && tsc > s.last_tsc) {
        us_per_tick = double(uint64_t(ns) - s.last_ns) / 1000.0 / double(tsc - s.last_tsc);
    }
    s.last_ns = uint64_t(ns);
    s.last_tsc = tsc;
    if (us_per_tick == 0.0) {
        return;
    }
    static const char* cls[NumClasses] = {"replicator", "direct", "indirect"};
    for (uint32_t c = 0; c < NumClasses; ++c) {
        if (s.count[c] == 0) {
            continue;
        }
        uint64_t sum = 0;
        for (auto v : s.total[c]) {
            sum += v;
        }
        char line[512];
        int off = std::snprintf(line, sizeof(line), "SOTC DISPPHASE %s n=%llu total %.2f us:", cls[c],
                                static_cast<unsigned long long>(s.count[c]),
                                double(sum) * us_per_tick / double(s.count[c]));
        for (uint32_t p = 0; p < NumPhases && off < int(sizeof(line)) - 24; ++p) {
            off += std::snprintf(line + off, sizeof(line) - off, " %s %.2f", kNames[p],
                                 double(s.total[c][p]) * us_per_tick / double(s.count[c]));
        }
        std::fprintf(stderr, "%s\n", line);
    }
    const uint64_t n = s.count[0] + s.count[1] + s.count[2];
    if (n != 0) {
        std::fprintf(stderr,
                     "SOTC DISPPHASE shape per dispatch: bufs %.2f written %.2f images %.2f "
                     "needs_barrier %.2f in_render_pass %.2f dma %.2f\n",
                     double(s.sum_bufs) / n, double(s.sum_wbufs) / n, double(s.sum_imgs) / n,
                     double(s.sum_barrier) / n, double(s.sum_rendering) / n, double(s.sum_dma) / n);
    }
    {
        auto& u = Subs();
        const double tk = us_per_tick;
        if (u.hle_n != 0) {
            std::fprintf(stderr,
                         "SOTC DISPPHASE hle n=%llu (direct %llu pattern %llu other %llu) %.2f us avg: "
                         "pre %.2f pattern-build %.2f compare %.2f (%.1f KB, %.2f GB/s) flush %.2f "
                         "(%.1f KB replaced) table %.1f KB; tables >= 1 MiB: n=%llu %.1f us avg\n",
                         static_cast<unsigned long long>(u.hle_n),
                         static_cast<unsigned long long>(u.hle_direct),
                         static_cast<unsigned long long>(u.hle_pattern),
                         static_cast<unsigned long long>(u.hle_other),
                         double(u.hle_total) * tk / u.hle_n, double(u.hle_pre) * tk / u.hle_n,
                         double(u.hle_pattern_build) * tk / u.hle_n,
                         double(u.hle_cmp) * tk / u.hle_n, double(u.hle_cmp_bytes) / 1024.0 / u.hle_n,
                         u.hle_cmp ? double(u.hle_cmp_bytes) / (double(u.hle_cmp) * tk * 1000.0) : 0.0,
                         double(u.hle_flush) * tk / u.hle_n, double(u.hle_repl_bytes) / 1024.0 / u.hle_n,
                         double(u.hle_table_bytes) / 1024.0 / u.hle_n,
                         static_cast<unsigned long long>(u.hle_big_n),
                         u.hle_big_n ? double(u.hle_big_ticks) * tk / u.hle_big_n : 0.0);
        }
        for (int w = 0; w < 2; ++w) {
            if (u.ob_n[w] == 0) {
                continue;
            }
            std::fprintf(stderr,
                         "SOTC DISPPHASE obtain %s n=%llu uploaded %.1f%%: arena %.3f us, sync %.3f us "
                         "(uploading calls %.3f us each), mark-written %.3f us (per call)\n",
                         w ? "written" : "read", static_cast<unsigned long long>(u.ob_n[w]),
                         100.0 * u.ob_up[w] / u.ob_n[w], double(u.ob_arena[w]) * tk / u.ob_n[w],
                         double(u.ob_sync[w]) * tk / u.ob_n[w],
                         u.ob_up[w] ? double(u.ob_sync_up[w]) * tk / u.ob_up[w] : 0.0,
                         double(u.ob_mark[w]) * tk / u.ob_n[w]);
        }
        for (int w = 0; w < 2; ++w) {
            if (u.up_ranges[w] == 0) {
                continue;
            }
            const uint64_t n = std::max<uint64_t>(1, u.ob_up[w]);
            std::fprintf(stderr,
                         "SOTC DISPPHASE upload %s n=%llu: collect %.3f flush %.3f commit %.3f us each, "
                         "%.2f ranges, %.1f KB\n",
                         w ? "written" : "read", static_cast<unsigned long long>(u.ob_up[w]),
                         double(u.up_collect[w]) * tk / n, double(u.up_flush[w]) * tk / n,
                         double(u.up_commit[w]) * tk / n, double(u.up_ranges[w]) / n,
                         double(u.up_bytes[w]) / 1024.0 / n);
        }
        if (u.fi_n[0] + u.fi_n[1] + u.fi_n[2] + u.fi_n[3] != 0) {
            static const char* names[4] = {"memo", "perfect", "overlap", "new"};
            std::fprintf(stderr, "SOTC DISPPHASE findimage:");
            for (int k = 0; k < 4; ++k) {
                std::fprintf(stderr, " %s n=%llu %.2f us", names[k],
                             static_cast<unsigned long long>(u.fi_n[k]),
                             u.fi_n[k] ? double(u.fi_ticks[k]) * tk / u.fi_n[k] : 0.0);
            }
            std::fprintf(stderr, "; expand %llu free %llu\n",
                         static_cast<unsigned long long>(u.fi_expand),
                         static_cast<unsigned long long>(u.fi_free));
        }
        u = {};
    }
    std::vector<ShaderRow> sorted;
    for (const auto& r : s.rows) {
        if (r.n != 0) {
            sorted.push_back(r);
        }
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const ShaderRow& a, const ShaderRow& b) { return a.ticks > b.ticks; });
    uint64_t all = 0;
    for (const auto& r : sorted) {
        all += r.ticks;
    }
    std::fprintf(stderr, "SOTC DISPPHASE shaders (%zu, %.1f ms per window):\n", sorted.size(),
                 double(all) * us_per_tick / 1000.0);
    for (size_t i = 0; i < sorted.size() && i < 14; ++i) {
        const auto& r = sorted[i];
        std::fprintf(stderr,
                     "SOTC DISPPHASE   %08llx n=%llu %.2f us avg, %.1f ms, groups %.0f bufs %.1f "
                     "written %.1f imgs %.1f barrier %.2f in_rp %.2f | obt %.2f btex %.2f post %.2f\n",
                     static_cast<unsigned long long>(r.hash), static_cast<unsigned long long>(r.n),
                     double(r.ticks) * us_per_tick / double(r.n), double(r.ticks) * us_per_tick / 1000.0,
                     double(r.groups) / double(r.n), double(r.bufs) / double(r.n),
                     double(r.wbufs) / double(r.n), double(r.imgs) / double(r.n),
                     double(r.barriers) / double(r.n), double(r.rendering) / double(r.n),
                     double(r.obt_ticks) * us_per_tick / double(r.n),
                     double(r.btex_ticks) * us_per_tick / double(r.n),
                     double(r.post_ticks) * us_per_tick / double(r.n));
    }
    s.total = {};
    s.count = {};
    for (auto& r : s.rows) {
        r = {};
    }
    s.sum_bufs = s.sum_wbufs = s.sum_imgs = s.sum_barrier = s.sum_rendering = s.sum_dma = 0;
}
/// End of a dispatch that was recorded. `cls` as above, per-shader data for the table.
inline void Finish(uint32_t cls, uint64_t hash, uint64_t groups, uint32_t bufs, uint32_t wbufs,
                   uint32_t imgs, bool barrier, bool rendering, bool dma) {
    if (!kOn) {
        return;
    }
    auto& s = S();
    if (!s.active) {
        return;
    }
    s.active = false;
    uint64_t sum = 0;
    for (uint32_t p = 0; p < NumPhases; ++p) {
        s.total[cls][p] += s.t[p];
        sum += s.t[p];
    }
    ++s.count[cls];
    s.sum_bufs += bufs;
    s.sum_wbufs += wbufs;
    s.sum_imgs += imgs;
    s.sum_barrier += barrier;
    s.sum_rendering += rendering;
    s.sum_dma += dma;
    size_t i = static_cast<size_t>((hash * 0x9E3779B97F4A7C15ull) >> 53) & 2047;
    for (uint32_t probe = 0; probe < 64; ++probe, i = (i + 1) & 2047) {
        auto& r = s.rows[i];
        if (r.n == 0 || r.hash == hash) {
            r.hash = hash;
            ++r.n;
            r.ticks += sum;
            r.groups += groups;
            r.bufs += bufs;
            r.wbufs += wbufs;
            r.imgs += imgs;
            r.barriers += barrier;
            r.rendering += rendering;
            r.obt_ticks += s.t[BbObtR] + s.t[BbObtW];
            r.btex_ticks += s.t[BtTex] + s.t[BtPre] + s.t[BtFind] + s.t[BtBind];
            r.post_ticks += s.t[Post] + s.t[PostReset];
            break;
        }
    }
    if ((++s.window_dispatches & 4095) == 0) {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        static uint64_t next_ns = 0;
        if (uint64_t(ns) >= next_ns) {
            if (next_ns != 0) {
                Print(s);
            } else {
                s.last_ns = uint64_t(ns);
                s.last_tsc = Now();
                s.total = {};
                s.count = {};
                for (auto& r : s.rows) {
                    r = {};
                }
            }
            next_ns = uint64_t(ns) + 5'000'000'000ull;
        }
    }
}

} // namespace DispPhase
