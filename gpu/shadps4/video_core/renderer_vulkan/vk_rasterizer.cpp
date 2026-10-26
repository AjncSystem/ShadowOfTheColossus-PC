// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <span>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <xxhash.h>
#include "common/path_util.h"
#include "video_core/renderdoc.h"
#include "common/debug.h"
#include "common/elf_info.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/sotc_lod_stats.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_dlssg.h"
#include "video_core/renderer_vulkan/vk_shadow_scroll.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/buffer_cache/region_manager.h"
#include "video_core/sotc_live.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "bbport_toggles.h"
#include "video_core/sotc_stats.h"
#include "video_core/sotc_opstats.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/renderer_vulkan/sotc_dispphase.h" // sotc: SOTC_DISP_PHASES
#include "video_core/renderer_vulkan/vk_framegen.h"
// After vk_common.h (via the headers above), which sets the Vulkan header defines first.
#include <vk_mem_alloc.h>
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

static Shader::PushData MakeUserData(const AmdGpu::Regs& regs) {
    // TODO(roamic): Add support for multiple viewports and geometry shaders when ViewportIndex
    // is encountered and implemented in the recompiler.
    Shader::PushData push_data{};
    push_data.xoffset = regs.viewport_control.xoffset_enable ? regs.viewports[0].xoffset : 0.f;
    push_data.xscale = regs.viewport_control.xscale_enable ? regs.viewports[0].xscale : 1.f;
    push_data.yoffset = regs.viewport_control.yoffset_enable ? regs.viewports[0].yoffset : 0.f;
    push_data.yscale = regs.viewport_control.yscale_enable ? regs.viewports[0].yscale : 1.f;
    return push_data;
}

// sotc: SOTC_DRAW_PHASES (see vk_rasterizer.h).
namespace DrawPhase {
enum Phase : u32 {
    Pre,      // pop pending ops, filters, pipeline resolve, stats
    Prep,     // PrepareRenderState
    BindBuf,  // BindBuffers (all stages)
    BindTex,  // BindTextures (all stages)
    BindRest, // rest of BindResources (set_writes resize, DMA, checks)
    Begin,    // BeginRendering
    Refresh,  // RefreshImageLayouts
    VtxIdx,   // vertex and index buffers
    Args,     // indirect args read, barriers
    PipeBind, // pipeline->BindResources (push constants + descriptor push)
    Dyn,      // UpdateDynamicState
    Sched,    // scheduler.BeginRendering, bindPipeline, draw
    Post,     // ResetBindings + FlushPeriodic
    NumPhases,
};
static constexpr const char* kNames[NumPhases] = {"pre",  "prep", "bbuf", "btex", "brest", "begin", "refresh",
                                                  "vtx",  "args", "pipe", "dyn",  "sched", "post"};
// sotc: finer laps inside the phases above (SOTC_DRAW_PHASES only, docs/sotc/DEPTH-FASTPATH.md): each
// Sub(k) takes the time since the previous Mark or Sub, so the subs of a phase partition it up to
// the stretch after its last sub (left in the phase total).
enum SubPhase : u32 {
    SPop,       // pre: scheduler.PopPendingOperations
    SFilter,    // pre: TracePass, FilterDraw, SkipStaggeredShadow, ReuseShadowCascade
    SPrepLock,  // prep: BatchLock
    SPrepCmp,   // prep: render target memo comparison
    SBrCheck,   // brest: compute-copy checks, user data check, set_writes clears
    SBrUser,    // brest: MakeUserData
    SBbFlat,    // bbuf: special buffers (flat buffer copy into the stream buffer)
    SBbClamp,   // bbuf: ClampRangeSize
    SBbObt,     // bbuf: ObtainBuffer
    SBbDesc,    // bbuf: descriptor, push offset, bound_buffers, IsBufferAccessed
    SBtPre,     // btex: lock and memo prelude
    SBtMemo,    // btex: T# hash, memo lookup and replay (ReplayTexBindMemo: BindResolvedImage per image)
    SBtImages,  // btex: first/second pass of a stage the memo did not replay, memo store, set_writes
    SBegLock,   // begin: BatchLock
    SBegFind,   // begin: FindDepthTarget
    SBegMeta,   // begin: htile TestAndTouchMeta
    SBegTrans,  // begin: Transit and the rest of the depth attachment
    SViInput,   // vtx: GetVertexInputs, input memo, setVertexInput
    SViObt,     // vtx: vertex buffer ranges (ObtainBuffer, IsBufferAccessed)
    SViBind,    // vtx: host arrays and bindVertexBuffers
    SIbuf,      // idx: BindIndexBuffer
    SPipePush,  // pipe: pushConstants thunk
    SPipeDesc,  // pipe: descriptor push thunk
    SDynCheck,  // dyn: memo test
    SDynCommit, // dyn: dynamic_state.Commit (thunks of changed state)
    SSchBegin,  // sched: scheduler.BeginRendering
    SSchBind,   // sched: bindPipeline
    SSchDraw,   // sched: drawIndexed, DebugState
    SPostReset, // post: ResetBindings
    SPostFlush, // post: FlushPeriodic
    NumSubs,
};
static constexpr const char* kSubNames[NumSubs] = {
    "pop",     "filter",  "prepLock", "prepCmp", "brCheck", "brUser",  "bbFlat",  "bbClamp",
    "bbObt",   "bbDesc",  "btPre",    "btMemo", "btImages", "begLock", "begFind", "begMeta", "begTrans", "viInput",
    "viObt",   "viBind",  "ibuf",     "pipePush", "pipeDesc", "dynCheck", "dynCommit", "schBegin",
    "schBind", "schDraw", "postReset", "postFlush"};
struct Totals {
    u64 draws{};
    std::array<u64, NumPhases> ticks{};
    std::array<u64, NumSubs> sub{};
    u64 same_push{}, same_writes{}, same_state{}, same_pipeline{}, writes_n{};
};
static std::array<Totals, 3> totals;
static u64 op_ticks, op_ops, op_chunks, op_dwords;
static std::array<u64, 8> lap_ticks;
static u64 lap_draws;
thread_local u64 lap_last = 0;
std::atomic<u64> dyn_hits, dyn_misses;
static std::atomic<u64> cp_ticks[3], cp_n;
static u64 last_tsc, last_ns;
thread_local struct Scope* cur = nullptr;

struct Scope {
    u64 last{};
    u64 sub_last{};
    std::array<u64, NumPhases> t{};
    std::array<u64, NumSubs> s{};
    bool active{};
    Scope() {
        if (kOn) {
            active = true;
            cur = this;
            last = Now();
            sub_last = last;
        }
    }
    ~Scope() {
        if (active) {
            cur = nullptr;
        }
    }
    void Mark(Phase p) {
        if (active) {
            const u64 now = Now();
            t[p] += now - last;
            last = now;
            sub_last = now;
        }
    }
    void Sub(SubPhase k) {
        if (active) {
            const u64 now = Now();
            s[k] += now - sub_last;
            sub_last = now;
        }
    }
    void Skip() { // time that belongs to nobody (the statistics themselves)
        if (active) {
            last = Now();
        }
    }
    void Finish(u32 cls, bool same_push, bool same_writes, bool same_state, bool same_pipeline,
                u32 writes) {
        if (!active) {
            return;
        }
        Totals& tot = totals[cls];
        ++tot.draws;
        for (u32 i = 0; i < NumPhases; ++i) {
            tot.ticks[i] += t[i];
        }
        for (u32 i = 0; i < NumSubs; ++i) {
            tot.sub[i] += s[i];
        }
        tot.same_push += same_push;
        tot.same_writes += same_writes;
        tot.same_state += same_state;
        tot.same_pipeline += same_pipeline;
        tot.writes_n += writes;
        const u64 all = totals[0].draws + totals[1].draws + totals[2].draws;
        if (all >= 32768) {
            const u64 tsc = Now();
            const u64 ns = SotcStats::NowNs();
            const double per_us = last_ns ? double(tsc - last_tsc) / (double(ns - last_ns) / 1000.0)
                                          : 3000.0; // ticks per microsecond
            last_tsc = tsc;
            last_ns = ns;
            static const char* const cls_names[3] = {"depth-only", "gbuffer", "other"};
            for (u32 c = 0; c < 3; ++c) {
                const Totals& x = totals[c];
                if (x.draws == 0) {
                    continue;
                }
                const double n = double(x.draws);
                char line[512];
                int at = std::snprintf(line, sizeof(line), "SOTC DRAWPHASE %s n=%llu us/draw:",
                                       cls_names[c], static_cast<unsigned long long>(x.draws));
                u64 sum = 0;
                for (u32 i = 0; i < NumPhases; ++i) {
                    sum += x.ticks[i];
                    at += std::snprintf(line + at, sizeof(line) - at, " %s=%.2f", kNames[i],
                                        double(x.ticks[i]) / n / per_us);
                }
                std::snprintf(line + at, sizeof(line) - at,
                              " total=%.2f | same%%: push=%.0f writes=%.0f rstate=%.0f pipe=%.0f "
                              "(writes/draw %.1f)",
                              double(sum) / n / per_us, 100.0 * x.same_push / n,
                              100.0 * x.same_writes / n, 100.0 * x.same_state / n,
                              100.0 * x.same_pipeline / n, double(x.writes_n) / n);
                std::fprintf(stderr, "%s\n", line);
                char subline[768];
                int at3 = std::snprintf(subline, sizeof(subline), "SOTC DRAWPHASE %s sub us/draw:",
                                        cls_names[c]);
                for (u32 i = 0; i < NumSubs; ++i) {
                    at3 += std::snprintf(subline + at3, sizeof(subline) - at3, " %s=%.3f",
                                         kSubNames[i], double(x.sub[i]) / n / per_us);
                }
                std::fprintf(stderr, "%s\n", subline);
            }
            {
                char split[256];
                int at2 = std::snprintf(split, sizeof(split), "SOTC DRAWPHASE begin split us/draw:");
                static const char* const lap_names[7] = {"find+update", "samples", "findrt", "meta",
                                                         "transit",     "colorrest", "depth"};
                for (u32 k = 0; k < 7; ++k) {
                    at2 += std::snprintf(split + at2, sizeof(split) - at2, " %s=%.2f", lap_names[k],
                                         lap_draws ? double(lap_ticks[k]) / double(lap_draws) / per_us
                                                   : 0.0);
                }
                std::fprintf(stderr, "%s (%llu draws)\n", split,
                             static_cast<unsigned long long>(lap_draws));
                lap_ticks.fill(0);
                lap_draws = 0;
            }
            const u64 cpn = cp_n.load(std::memory_order_relaxed);
            std::fprintf(stderr,
                         "SOTC DRAWPHASE dyn memo %llu hits, %llu misses\n"
                         "SOTC DRAWPHASE op prefix %.2f us/op (%.2f chunks, %.1f dwords per op, %llu ops); CP per "
                         "draw: lookup %.2f alloc %.2f capture %.2f us (%llu draws); %.0f MHz\n",
                         static_cast<unsigned long long>(dyn_hits.exchange(0)),
                         static_cast<unsigned long long>(dyn_misses.exchange(0)),
                         op_ops ? double(op_ticks) / double(op_ops) / per_us : 0.0,
                         op_ops ? double(op_chunks) / double(op_ops) : 0.0,
                         op_ops ? double(op_dwords) / double(op_ops) : 0.0,
                         static_cast<unsigned long long>(op_ops),
                         cpn ? double(cp_ticks[0].load()) / double(cpn) / per_us : 0.0,
                         cpn ? double(cp_ticks[1].load()) / double(cpn) / per_us : 0.0,
                         cpn ? double(cp_ticks[2].load()) / double(cpn) / per_us : 0.0,
                         static_cast<unsigned long long>(cpn), per_us);
            for (auto& x : totals) {
                x = {};
            }
            op_ticks = op_ops = op_chunks = op_dwords = 0;
            cp_ticks[0] = cp_ticks[1] = cp_ticks[2] = 0;
            cp_n = 0;
        }
    }
};
void AddOpPrefix(u64 ticks, u32 chunks, u32 dwords) {
    op_ticks += ticks;
    op_chunks += chunks;
    op_dwords += dwords;
    ++op_ops;
}
void AddCp(u32 what, u64 ticks) {
    cp_ticks[what].fetch_add(ticks, std::memory_order_relaxed);
    if (what == 2) {
        cp_n.fetch_add(1, std::memory_order_relaxed);
    }
}
static u32 ClassOf(u32 mrt_mask) {
    return mrt_mask == 0 ? 0 : (mrt_mask & 0x10) ? 1 : 2;
}
inline void Mark(Phase p) {
    if (kOn && cur) {
        cur->Mark(p);
    }
}
inline void Sub(SubPhase k) {
    if (kOn && cur) {
        cur->Sub(k);
    }
}
void SubPipe(unsigned which) { // called from Pipeline::BindResources (vk_pipeline_common.cpp)
    Sub(which == 0 ? SPipePush : SPipeDesc);
}
/// BeginRendering split (per draw, summed over its attachments): SOTC_DRAW_PHASES only.
/// sotc: SOTC_DRAW_LAPS=0 takes the laps (and the begin subs) out: they are ~40 rdtsc reads per
/// G-buffer draw, a large part of what the `begin` phase measures (docs/sotc/GBUFFER-FASTPATH.md).
inline const bool kLaps = kOn && [] {
    const char* v = std::getenv("SOTC_DRAW_LAPS");
    return !v || v[0] != '0';
}();
inline void SubBeg(SubPhase k) {
    if (kLaps) {
        Sub(k);
    }
}
inline void LapStart() {
    if (kLaps) {
        lap_last = Now();
        ++lap_draws;
    }
}
inline void Lap(u32 k) {
    if (kLaps) {
        const u64 now = Now();
        lap_ticks[k] += now - lap_last;
        lap_last = now;
    }
}

/// sotc: BbToggle::DepthFastPath counters (draw thread only) and the SOTC_DEPTHFAST_VERIFY=1 switch
/// (every replay is also computed in full and compared; a mismatch is counted and the full result
/// is used). One "SOTC DEPTHFAST" line per ~1M draws.
namespace DepthFastStats {
inline const bool kVerify = [] {
    const char* v = std::getenv("SOTC_DEPTHFAST_VERIFY");
    return v && v[0] == '1';
}();
struct Counters {
    u64 draws{};
    u64 prep_hit{}, prep_miss{}, prep_old_only{}, prep_verify{}, prep_bad{};
    u64 begin_cand{}, begin_hit{}, begin_record{}, begin_norecord{}, begin_verify{}, begin_bad{};
    std::array<u64, 12> cause{};
    std::array<u64, 9> why{}; ///< SOTC_DEPTHFAST_WHY: which depth side register differs
    u64 dyn_hit{}, dyn_miss{}, dyn_verify{}, dyn_bad{};
    // sotc: BbToggle::GBufferFastPath (GBufferFast): candidates (draws with colour targets), hits
    // (check passed), recorded / not recorded (and why), per cause, verify totals.
    u64 gb_cand{}, gb_hit{}, gb_record{}, gb_norecord{}, gb_verify{}, gb_bad{}, gb_count_bad{};
    std::array<u64, 16> gb_cause{};
    std::array<u64, 8> gb_norec{};
    // sotc: BbToggle::TexBindFastPath: memoized texture bindings checked / replayed / why not.
    u64 bt_cand{}, bt_hit{}, bt_verify{}, bt_bad{}, bt_count_bad{};
    std::array<u64, 8> bt_cause{};
};
inline Counters c;
inline const bool kGbCount = [] { // SOTC_GBFAST_MODE=count: check and record, never replay
    const char* v = std::getenv("SOTC_GBFAST_MODE");
    return v && std::strcmp(v, "count") == 0;
}();
inline const char* const kGbCause[16] = {"hit",   "nomemo", "mask",   "regs",  "ids",  "gen",
                                         "gpuepoch", "meta", "flags", "bound", "backing", "transit",
                                         "vo",    "dflags", "dbacking", "dtransit"};
inline const bool kBtCount = [] { // SOTC_BTFAST_MODE=count: check, never replay
    const char* v = std::getenv("SOTC_BTFAST_MODE");
    return v && std::strcmp(v, "count") == 0;
}();
inline const char* const kBtCause[8] = {"hit", "nofast", "id", "special", "backing", "update", "transit", "-"};
inline const char* const kGbNoRec[8] = {"clear", "feedback", "image", "vo", "depth", "transit", "samples", "-"};
inline const bool kWhy = [] {
    const char* v = std::getenv("SOTC_DEPTHFAST_WHY");
    return v && v[0] == '1';
}();
inline const char* const kWhyNames[9] = {"depth_control", "depth_buffer", "depth_render_control",
                                         "color_control", "depth_view",   "htile",
                                         "depth_clear",   "stencil_clear", "db_extent"};
inline const char* const kCause[12] = {"hit",  "nomemo", "regs",  "image", "gen",  "gpuepoch",
                                       "meta", "flags",  "backing", "transit", "-", "-"};
inline void Report() {
    char line[768];
    int at = std::snprintf(
        line, sizeof(line),
        "SOTC DEPTHFAST draws %llu | prep hit %llu miss %llu (old-only %llu) | begin cand %llu hit "
        "%llu (%.1f%%) recorded %llu not-recorded %llu | dyn hit %llu miss %llu |",
        static_cast<unsigned long long>(c.draws), static_cast<unsigned long long>(c.prep_hit),
        static_cast<unsigned long long>(c.prep_miss),
        static_cast<unsigned long long>(c.prep_old_only),
        static_cast<unsigned long long>(c.begin_cand),
        static_cast<unsigned long long>(c.begin_hit),
        c.begin_cand ? 100.0 * double(c.begin_hit) / double(c.begin_cand) : 0.0,
        static_cast<unsigned long long>(c.begin_record),
        static_cast<unsigned long long>(c.begin_norecord),
        static_cast<unsigned long long>(c.dyn_hit), static_cast<unsigned long long>(c.dyn_miss));
    for (u32 i = 1; i < 10; ++i) {
        at += std::snprintf(line + at, sizeof(line) - at, " %s %llu", kCause[i],
                            static_cast<unsigned long long>(c.cause[i]));
    }
    if (kWhy) {
        at += std::snprintf(line + at, sizeof(line) - at, " | why-regs");
        for (u32 i = 0; i < 9; ++i) {
            at += std::snprintf(line + at, sizeof(line) - at, " %s %llu", kWhyNames[i],
                                static_cast<unsigned long long>(c.why[i]));
        }
    }
    if (kVerify) {
        at += std::snprintf(line + at, sizeof(line) - at,
                            " | VERIFY prep %llu bad %llu, begin %llu bad %llu, dyn %llu bad %llu",
                            static_cast<unsigned long long>(c.prep_verify),
                            static_cast<unsigned long long>(c.prep_bad),
                            static_cast<unsigned long long>(c.begin_verify),
                            static_cast<unsigned long long>(c.begin_bad),
                            static_cast<unsigned long long>(c.dyn_verify),
                            static_cast<unsigned long long>(c.dyn_bad));
    }
    std::fprintf(stderr, "%s\n", line);
    {
        char gb[768];
        int g = std::snprintf(gb, sizeof(gb),
                              "SOTC GBFAST cand %llu hit %llu (%.1f%%) recorded %llu not-recorded %llu |",
                              static_cast<unsigned long long>(c.gb_cand),
                              static_cast<unsigned long long>(c.gb_hit),
                              c.gb_cand ? 100.0 * double(c.gb_hit) / double(c.gb_cand) : 0.0,
                              static_cast<unsigned long long>(c.gb_record),
                              static_cast<unsigned long long>(c.gb_norecord));
        for (u32 i = 1; i < 16; ++i) {
            g += std::snprintf(gb + g, sizeof(gb) - g, " %s %llu", kGbCause[i],
                               static_cast<unsigned long long>(c.gb_cause[i]));
        }
        g += std::snprintf(gb + g, sizeof(gb) - g, " | norec");
        for (u32 i = 0; i < 7; ++i) {
            g += std::snprintf(gb + g, sizeof(gb) - g, " %s %llu", kGbNoRec[i],
                               static_cast<unsigned long long>(c.gb_norec[i]));
        }
        if (kGbCount) {
            g += std::snprintf(gb + g, sizeof(gb) - g, " | COUNT state-differs %llu",
                               static_cast<unsigned long long>(c.gb_count_bad));
        }
        if (kVerify) {
            g += std::snprintf(gb + g, sizeof(gb) - g, " | VERIFY %llu bad %llu",
                               static_cast<unsigned long long>(c.gb_verify),
                               static_cast<unsigned long long>(c.gb_bad));
        }
        std::fprintf(stderr, "%s\n", gb);
    }
    {
        char bt[512];
        int b = std::snprintf(bt, sizeof(bt), "SOTC BTFAST cand %llu hit %llu (%.1f%%) |",
                              static_cast<unsigned long long>(c.bt_cand),
                              static_cast<unsigned long long>(c.bt_hit),
                              c.bt_cand ? 100.0 * double(c.bt_hit) / double(c.bt_cand) : 0.0);
        for (u32 i = 1; i < 7; ++i) {
            b += std::snprintf(bt + b, sizeof(bt) - b, " %s %llu", kBtCause[i],
                               static_cast<unsigned long long>(c.bt_cause[i]));
        }
        if (kBtCount) {
            b += std::snprintf(bt + b, sizeof(bt) - b, " | COUNT infos-differ %llu",
                               static_cast<unsigned long long>(c.bt_count_bad));
        }
        if (kVerify) {
            b += std::snprintf(bt + b, sizeof(bt) - b, " | VERIFY %llu bad %llu",
                               static_cast<unsigned long long>(c.bt_verify),
                               static_cast<unsigned long long>(c.bt_bad));
        }
        std::fprintf(stderr, "%s\n", bt);
    }
    // The verify totals are cumulative, the rest are per interval.
    const auto keep = c;
    c = {};
    c.gb_verify = keep.gb_verify;
    c.gb_bad = keep.gb_bad;
    c.bt_verify = keep.bt_verify;
    c.bt_bad = keep.bt_bad;
    if (kVerify) {
        c.prep_verify = keep.prep_verify;
        c.prep_bad = keep.prep_bad;
        c.begin_verify = keep.begin_verify;
        c.begin_bad = keep.begin_bad;
        c.dyn_verify = keep.dyn_verify;
        c.dyn_bad = keep.dyn_bad;
    }
}
inline void NoteDraw() {
    if ((++c.draws & 0xFFFFF) == 0) {
        Report();
    }
}
} // namespace DepthFastStats

/// SOTC_SHADOW_PROBE=1 (diagnostic, no behaviour change): is a sun shadow cascade slice's draw
/// stream, as the draw thread sees it (pipeline, push constants, descriptor writes and infos,
/// render state, indirect arguments), the same as in an earlier frame? A necessary condition for
/// skipping the slice; the contents of the bound buffers are not part of it. Printed every 300
/// frames: per slice, draws per frame and frames equal to the previous one / to one of the 4
/// frames before.
struct ShadowProbe {
    static constexpr u32 kSlices = 8, kHist = 5;
    u64 frame_seen = ~0ull;
    std::array<u64, kSlices> cur_hash{}, cur_n{};
    std::array<std::array<u64, kHist>, kSlices> hist{};
    std::array<std::array<u64, kHist>, kSlices> hist_n{};
    std::array<u64, kSlices> frames{}, eq_prev{}, eq_any{}, draws{}, eq_prev_draws{};
    u64 prev_frame_total = 0;
};
inline void Fnv(u64& h, const void* data, size_t bytes) {
    const auto* p = static_cast<const u8*>(data);
    for (size_t i = 0; i + 8 <= bytes; i += 8) {
        u64 w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    for (size_t i = bytes & ~size_t{7}; i < bytes; ++i) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
}
}
template <typename Regs, typename Writes, typename BufInfos, typename ImgInfos>
static void ShadowProbeNote(const Regs& regs, const void* pipeline, const Writes& writes,
                            const BufInfos& buf_infos, const ImgInfos& img_infos,
                            const Shader::PushData& push, const RenderState& state,
                            const void* args, size_t args_bytes) {
    static const bool on = std::getenv("SOTC_SHADOW_PROBE") != nullptr;
    if (!on || !regs.depth_buffer.DepthValid() ||
        regs.depth_buffer.Pitch() != regs.depth_buffer.Height() || regs.depth_buffer.Pitch() < 512) {
        return;
    }
    static DrawPhase::ShadowProbe s;
    const u64 frame = SotcStats::flip_count.load(std::memory_order_relaxed);
    if (frame != s.frame_seen) {
        if (s.frame_seen != ~0ull) {
            for (u32 sl = 0; sl < DrawPhase::ShadowProbe::kSlices; ++sl) {
                if (s.cur_n[sl] == 0) {
                    continue;
                }
                auto& h = s.hist[sl];
                auto& hn = s.hist_n[sl];
                ++s.frames[sl];
                s.draws[sl] += s.cur_n[sl];
                bool any = false;
                for (u32 k = 0; k < DrawPhase::ShadowProbe::kHist; ++k) {
                    any |= h[k] == s.cur_hash[sl] && hn[k] == s.cur_n[sl];
                }
                if (h[0] == s.cur_hash[sl] && hn[0] == s.cur_n[sl]) {
                    ++s.eq_prev[sl];
                    s.eq_prev_draws[sl] += s.cur_n[sl];
                }
                s.eq_any[sl] += any;
                for (u32 k = DrawPhase::ShadowProbe::kHist - 1; k > 0; --k) {
                    h[k] = h[k - 1];
                    hn[k] = hn[k - 1];
                }
                h[0] = s.cur_hash[sl];
                hn[0] = s.cur_n[sl];
            }
            if (s.frame_seen % 300 == 0) {
                for (u32 sl = 0; sl < DrawPhase::ShadowProbe::kSlices; ++sl) {
                    if (s.frames[sl]) {
                        std::fprintf(stderr,
                                     "SOTC SHADOW PROBE slice %u: %.0f draws/frame, stream equal "
                                     "to previous frame %llu/%llu, to one of 5 before %llu/%llu\n",
                                     sl, double(s.draws[sl]) / double(s.frames[sl]),
                                     static_cast<unsigned long long>(s.eq_prev[sl]),
                                     static_cast<unsigned long long>(s.frames[sl]),
                                     static_cast<unsigned long long>(s.eq_any[sl]),
                                     static_cast<unsigned long long>(s.frames[sl]));
                    }
                }
                s.frames.fill(0);
                s.eq_prev.fill(0);
                s.eq_any.fill(0);
                s.draws.fill(0);
                s.eq_prev_draws.fill(0);
            }
        }
        s.cur_hash.fill(0xcbf29ce484222325ull);
        s.cur_n.fill(0);
        s.frame_seen = frame;
    }
    const u32 sl = regs.depth_view.slice_start & 7;
    u64& h = s.cur_hash[sl];
    DrawPhase::Fnv(h, &pipeline, sizeof(pipeline));
    DrawPhase::Fnv(h, &push, sizeof(push));
    DrawPhase::Fnv(h, writes.data(), writes.size() * sizeof(*writes.data()));
    DrawPhase::Fnv(h, buf_infos.data(), buf_infos.size() * sizeof(*buf_infos.data()));
    DrawPhase::Fnv(h, img_infos.data(), img_infos.size() * sizeof(*img_infos.data()));
    DrawPhase::Fnv(h, &state, sizeof(state));
    DrawPhase::Fnv(h, args, args_bytes);
    ++s.cur_n[sl];
}
namespace DrawPhase {
/// Diagnostics: is this draw's push data / descriptor writes / render state / pipeline the same
/// as the previous graphics draw's?
template <typename Writes, typename BufInfos, typename ImgInfos>
static void Finish(Scope& ph, const GraphicsPipeline* pipeline, const Writes& writes,
                   const BufInfos& buf_infos, const ImgInfos& img_infos,
                   const Shader::PushData& push, const RenderState& state) {
    if (!kOn) {
        return;
    }
    static Shader::PushData prev_push{};
    static std::vector<u8> prev_writes, prev_bufs, prev_imgs;
    static RenderState prev_state{};
    static const GraphicsPipeline* prev_pipeline = nullptr;
    const auto bytes = [](const auto& c) {
        return std::span<const u8>{reinterpret_cast<const u8*>(c.data()),
                                   c.size() * sizeof(*c.data())};
    };
    const auto w = bytes(writes), b = bytes(buf_infos), i = bytes(img_infos);
    const bool same_push = std::memcmp(&prev_push, &push, sizeof(push)) == 0;
    const bool same_writes =
        prev_writes.size() == w.size() && prev_bufs.size() == b.size() &&
        prev_imgs.size() == i.size() && std::memcmp(prev_writes.data(), w.data(), w.size()) == 0 &&
        std::memcmp(prev_bufs.data(), b.data(), b.size()) == 0 &&
        std::memcmp(prev_imgs.data(), i.data(), i.size()) == 0;
    const bool same_state = prev_state == state;
    const bool same_pipeline = prev_pipeline == pipeline;
    prev_push = push;
    prev_writes.assign(w.begin(), w.end());
    prev_bufs.assign(b.begin(), b.end());
    prev_imgs.assign(i.begin(), i.end());
    prev_state = state;
    prev_pipeline = pipeline;
    ph.Finish(ClassOf(pipeline->GetGraphicsKey().mrt_mask), same_push, same_writes, same_state,
              same_pipeline, static_cast<u32>(writes.size()));
}
} // namespace DrawPhase

// sotc: SOTC_OP_STATS=1 (sotc_opstats.h): what one recorded draw looks like to the op composition
// counters. Called once per issued draw after its descriptors are built (before they are reset).
template <typename Regs, typename BufInfos, typename ImgInfos>
static void OpStatsNoteDraw(const Regs& regs, const GraphicsPipeline* pipeline,
                            const RenderState& state, const BufInfos& buf_infos,
                            const ImgInfos& img_infos, const Shader::PushData& push, bool indexed,
                            bool gpu_args, u32 count, u32 instances, u32 first_index,
                            u32 vertex_offset, u32 first_instance) {
    using SotcOps::Mix;
    using SotcOps::MixBytes;
    SotcOps::DrawNote n{};
    n.indexed = indexed;
    n.gpu_args = gpu_args;
    u32 num_color = 0;
    u64 pass = 0x70617373;
    for (u32 i = 0; i < state.num_color_attachments; ++i) {
        const u64 view = std::bit_cast<u64>(state.color_attachments[i].image_view);
        num_color += view != 0;
        pass = Mix(pass, view);
    }
    const u64 depth_view = std::bit_cast<u64>(state.depth_stencil_attachment.image_view);
    pass = Mix(pass, depth_view);
    n.kind = (num_color && depth_view) ? SotcOps::KindColorDepth
             : depth_view              ? SotcOps::KindDepthOnly
             : num_color               ? SotcOps::KindColor
                                       : SotcOps::KindNone;
    n.num_color = num_color;
    n.pass_key = pass;
    n.depth_addr = regs.depth_buffer.DepthValid() ? regs.depth_buffer.DepthWriteAddress() : 0;
    n.slice = regs.depth_view.slice_start;
    n.width = state.width;
    n.height = state.height;
    n.shadow = n.kind == SotcOps::KindDepthOnly && regs.depth_buffer.DepthValid() &&
               regs.depth_buffer.Pitch() == regs.depth_buffer.Height() &&
               regs.depth_buffer.Pitch() >= 512;
    n.count = count;
    n.instances = instances;
    n.first_index = first_index;
    n.vertex_offset = vertex_offset;
    n.first_instance = first_instance;
    n.n_buf = static_cast<u32>(buf_infos.size());
    n.n_img = static_cast<u32>(img_infos.size());
    u64 h = Mix(reinterpret_cast<uintptr_t>(pipeline), 1);
    h = MixBytes(h, &state, sizeof(state));
    n.h_pipe = h;
    u64 d = Mix(h, 2);
    d = MixBytes(d, img_infos.data(), img_infos.size() * sizeof(*img_infos.data()));
    for (const auto& b : buf_infos) {
        d = Mix(d, std::bit_cast<u64>(b.buffer));
    }
    d = MixBytes(d, &push, 4 * sizeof(float)); // viewport transform, no buffer offsets
    d = Mix(d, SotcOps::vb_desc);
    d = Mix(d, SotcOps::ib_desc);
    n.h_desc = d;
    u64 e = Mix(d, 3);
    e = MixBytes(e, buf_infos.data(), buf_infos.size() * sizeof(*buf_infos.data()));
    e = MixBytes(e, push.buf_offsets.data(), push.buf_offsets.size());
    e = Mix(e, SotcOps::vb_exact);
    e = Mix(e, SotcOps::ib_exact);
    n.h_exact = e;
    u64 a = Mix(count, instances);
    a = Mix(a, (u64{first_index} << 32) | vertex_offset);
    a = Mix(a, first_instance);
    n.h_args = a;
    u64 t = Mix(h, 4);
    t = MixBytes(t, img_infos.data(), img_infos.size() * sizeof(*img_infos.data()));
    t = Mix(t, SotcOps::vb_exact);
    t = Mix(t, SotcOps::ib_exact);
    t = Mix(t, a);
    n.h_stat = t;
    // sotc: depth pre-pass analysis (docs/sotc/ESCENA-TEMPLO.md): the raw state of the draw.
    const auto raw32 = [](const auto& reg) {
        u32 v = 0;
        std::memcpy(&v, &reg, std::min<size_t>(sizeof(reg), sizeof(v)));
        return v;
    };
    n.dctl = raw32(regs.depth_control);
    n.sctl = raw32(regs.stencil_control);
    n.dsc = raw32(regs.depth_shader_control);
    n.pctl = raw32(regs.polygon_control);
    n.rctl = raw32(regs.depth_render_control);
    n.rovr = raw32(regs.depth_render_override);
    n.cctl = raw32(regs.color_control);
    n.sref_f = raw32(regs.stencil_ref_front);
    n.sref_b = raw32(regs.stencil_ref_back);
    n.bias = regs.polygon_control.NeedsBias();
    n.depth_read_addr = regs.depth_buffer.DepthAddress();
    n.depth_write_addr = regs.depth_buffer.DepthWriteAddress();
    n.depth_pitch = regs.depth_buffer.Pitch();
    n.depth_height = regs.depth_buffer.Height();
    {
        const auto& key = pipeline->GetGraphicsKey();
        n.mrt_mask = key.mrt_mask;
        n.num_samples = key.num_samples;
        n.depth_samples = key.depth_samples;
        for (u32 i = 0; i < AmdGpu::NUM_COLOR_BUFFERS; ++i) {
            n.wmask |= (static_cast<u32>(key.write_masks[i]) & 0xF) << (4 * i);
            n.blend_en |= u32{key.blend_controls[i].enable != 0} << i;
        }
    }
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        if (stage->sw_stage == Shader::SwStage::Vertex) {
            n.vs_hash = stage->pgm_hash;
            for (const auto& bd : stage->buffers) {
                if (bd.IsSpecial() || n.n_vsbuf >= 12) {
                    continue;
                }
                const auto sh = bd.GetSharp(*stage);
                n.vsbuf[n.n_vsbuf++] =
                    Mix(Mix(sh.base_address, sh.GetSize()), sh.GetStride()) | 1;
            }
        } else if (stage->sw_stage == Shader::SwStage::Fragment) {
            n.has_ps = true;
            n.ps_hash = stage->pgm_hash;
            n.ps_discard = stage->has_discard;
            n.ps_zexport = stage->stores.GetAny(Shader::IR::Attribute::Depth);
            for (const auto& d : stage->images) {
                n.ps_uav |= d.is_written;
            }
            for (const auto& d : stage->buffers) {
                n.ps_uav |= d.is_written;
            }
            n.ps_other_export = stage->stores.GetAny(Shader::IR::Attribute::StencilRef) ||
                                stage->stores.GetAny(Shader::IR::Attribute::SampleMask);
        }
    }
    {
        u64 vp = 0x7670;
        vp = MixBytes(vp, &regs.viewports[0], sizeof(regs.viewports[0]));
        vp = MixBytes(vp, &regs.viewport_depths[0], sizeof(regs.viewport_depths[0]));
        vp = MixBytes(vp, &regs.viewport_scissors[0], sizeof(regs.viewport_scissors[0]));
        vp = MixBytes(vp, &regs.screen_scissor, sizeof(regs.screen_scissor));
        vp = MixBytes(vp, &regs.generic_scissor, sizeof(regs.generic_scissor));
        vp = MixBytes(vp, &regs.window_scissor, sizeof(regs.window_scissor));
        vp = MixBytes(vp, &regs.window_offset, sizeof(regs.window_offset));
        vp = MixBytes(vp, &regs.viewport_control, sizeof(regs.viewport_control));
        vp = MixBytes(vp, &regs.clipper_control, sizeof(regs.clipper_control));
        n.vp_hash = vp;
        n.bias_hash = MixBytes(0x6269, &regs.poly_offset, sizeof(regs.poly_offset));
    }
    {
        // The view constants' matrix at +0x80 (the light matrix of a shadow cascade, see
        // ReuseShadowCascade) and the whole flattened user data of the vertex stage.
        constexpr u64 kMask48 = 0xFFFFFFFFFFFFull;
        const u64 ud = (u64(regs.vs_program.user_data[0]) | (u64(regs.vs_program.user_data[1]) << 32)) &
                       kMask48;
        u64 cb = 0;
        float lvp[16];
        auto* mem = Core::Memory::Instance();
        if (ud && mem->TryReadBacking(ud + 8, &cb, sizeof(cb)) && (cb &= kMask48) &&
            mem->TryReadBacking(cb + 0x80, lvp, sizeof(lvp))) {
            n.in_lvp = MixBytes(0x6c7670, lvp, sizeof(lvp)) | 1;
        }
        for (const auto* stage : pipeline->GetStages()) {
            if (stage && stage->sw_stage == Shader::SwStage::Vertex) {
                const auto& flat = stage->FlatBuf();
                n.in_flat = MixBytes(0x666c6174, flat.data(),
                                     std::min<size_t>(flat.size(), 512) * sizeof(u32)) | 1;
            }
        }
    }
    n.g1 = Mix(Mix(Mix(SotcOps::ib_exact, count), first_index), 0x6731);
    n.g2 = Mix(Mix(Mix(n.g1, SotcOps::vb_exact), first_instance & 0xFFFF),
               (u64{static_cast<u32>(vertex_offset)} << 32) | instances);
    // Position inputs of the main twin pair (read from their SPIR-V in the shader cache): the
    // position-only pre-pass VS f5366412 (position stream = guest buffer 0, per-instance matrix
    // table = guest buffer 1, view-proj constants = flat dwords 26..43) and its G-buffer twin
    // a2a164eb (position = guest buffer 2, table = guest buffer 1, view-proj = flat dwords
    // 44..61). Both compute gl_Position with the same `precise` expression tree
    // (out/prepass/postree.py).
    {
        int pos_idx = -1, inst_idx = -1;
        u32 flat_base = 0;
        const u64 vh = n.vs_hash & 0xFFFFFFFFull;
        // stage->buffers order is not the ssbo numbering of the SPIR-V: the debug dump of the
        // first draws showed position = buffers[0] / matrix table = buffers[1] for f5366412 and
        // position = buffers[2] / matrix table = buffers[1] for a2a164eb.
        if (vh == 0xf5366412) {
            pos_idx = 0, inst_idx = 1, flat_base = 26;
        } else if (vh == 0xa2a164eb) {
            pos_idx = 2, inst_idx = 1, flat_base = 44;
        }
        if (pos_idx >= 0) {
            for (const auto* stage : pipeline->GetStages()) {
                if (!stage || stage->sw_stage != Shader::SwStage::Vertex) {
                    continue;
                }
                if (static_cast<size_t>(std::max(pos_idx, inst_idx)) < stage->buffers.size()) {
                    const auto& pb = stage->buffers[pos_idx];
                    const auto& ib = stage->buffers[inst_idx];
                    if (!pb.IsSpecial() && !ib.IsSpecial()) {
                        const auto psharp = pb.GetSharp(*stage);
                        const auto isharp = ib.GetSharp(*stage);
                        n.in_pos = Mix(psharp.base_address, 0x706f73);
                        {
                            std::array<u8, 16384> pbytes;
                            const u64 plen = std::min<u64>(psharp.GetSize(), pbytes.size());
                            if (plen != 0 && Core::Memory::Instance()->TryReadBacking(
                                                 psharp.base_address, pbytes.data(), plen)) {
                                n.in_posc = MixBytes(0x706f7363, pbytes.data(), plen) | 1;
                            }
                        }
                        u32 mat[12]{};
                        const u64 addr = isharp.base_address + u64{first_instance & 0xFFFF} * 128;
                        if (Core::Memory::Instance()->TryReadBacking(addr, mat, sizeof(mat))) {
                            n.in_mat = MixBytes(0x6d6174, mat, sizeof(mat)) | 1;
                        }
                    }
                }
                const auto& flat = stage->FlatBuf();
                if (flat.size() >= flat_base + 18) {
                    n.in_vp = MixBytes(0x7670, flat.data() + flat_base, 18 * sizeof(u32)) | 1;
                }
            }
        }
    }
    SotcOps::NoteDraw(n);
}

// sotc: SOTC_OP_STATS=1, depth pre-pass analysis: every image and buffer a stage binds, for the
// "what touches a depth target" counters (sotc_opstats.cpp NoteTouch), plus shaders with raw
// pointer access (uses_dma), which cannot be attributed.
static void OpStatsScanBindings(const Pipeline* pipeline) {
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage || (stage->UserData().data() == nullptr &&
                       stage->srt_info.flattened_bufsize_dw != 0)) {
            continue;
        }
        for (const auto& d : stage->images) {
            const auto t = d.GetSharp(*stage);
            if (t.Valid()) {
                SotcOps::NoteTouch(SotcOps::TouchImage, stage->pgm_hash,
                                   static_cast<u32>(stage->sw_stage), d.is_written, t.Address(), 1);
            }
        }
        for (const auto& d : stage->buffers) {
            if (d.IsSpecial()) {
                continue;
            }
            const auto b = d.GetSharp(*stage);
            if (b.base_address != 0) {
                SotcOps::NoteTouch(SotcOps::TouchBuffer, stage->pgm_hash,
                                   static_cast<u32>(stage->sw_stage), d.is_written, b.base_address,
                                   b.GetSize());
            }
        }
        if (stage->uses_dma) {
            SotcOps::NoteTouch(SotcOps::TouchDma, stage->pgm_hash,
                               static_cast<u32>(stage->sw_stage), true, 0, 0);
        }
    }
}

Rasterizer::Rasterizer(const Instance& instance_, Scheduler& scheduler_, Runtime& runtime_,
                       AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, page_manager{this},
      buffer_cache{instance, scheduler, runtime, liverpool_, texture_cache, page_manager},
      texture_cache{instance, scheduler, runtime, liverpool_, buffer_cache, page_manager},
      liverpool{liverpool_}, memory{Core::Memory::Instance()},
      pipeline_cache{instance, scheduler, liverpool, buffer_cache.GetSparsePageShift()},
      host_markers_enabled{EmulatorSettings.IsVkHostMarkersEnabled()},
      guest_markers_enabled{EmulatorSettings.IsVkGuestMarkersEnabled()} {
    if (!EmulatorSettings.IsNullGPU()) {
        liverpool->BindRasterizer(this);
    }
    memory->SetRasterizer(this);

    scheduler.SetSubmitCallback([this](Vulkan::SubmitInfo& info) {
        runtime.FlushBarriers();
        buffer_cache.SubmitPendingArenaBinds(info);
    });
}

Rasterizer::~Rasterizer() {
    for (auto& [address, image] : hudless_images) {
        DestroyHudless(image);
    }
    DestroyHudless(hudless_clear);
}

// Games may disable color buffer and enable force depth/stencil dirty and valid to do a copy from
// one depth-stencil surface to another, without a pixel shader: {depth copy, stencil copy}.
static std::pair<bool, bool> DepthStencilOverrideCopy(const auto& regs) {
    if (regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Disable) {
        return {false, false};
    }
    const bool depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const bool stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    return {depth_copy, stencil_copy};
}

bool Rasterizer::IsDrawFilteredOut() const {
    const auto& regs = liverpool->DrawRegs();
    using Mode = AmdGpu::ColorControl::OperationMode;
    const auto mode = regs.color_control.mode;
    if (mode == Mode::EliminateFastClear || mode == Mode::FmaskDecompress ||
        mode == Mode::Resolve || regs.primitive_type == AmdGpu::PrimitiveType::None) {
        return true;
    }
    const auto [depth_copy, stencil_copy] = DepthStencilOverrideCopy(regs);
    return depth_copy || stencil_copy;
}

bool Rasterizer::FilterDraw() {
    const auto& regs = liverpool->DrawRegs();
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear) {
        // Clears the render target if FCE is launched before any draws
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        // TODO: check for a valid MRT1 to promote the draw to the resolve pass.
        LOG_TRACE(Render_Vulkan, "FMask decompression pass skipped");
        ScopedMarkerInsert("FmaskDecompress");
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        LOG_TRACE(Render_Vulkan, "Resolve pass");
        Resolve();
        return false;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        LOG_TRACE(Render_Vulkan, "Primitive type 'None' skipped");
        ScopedMarkerInsert("PrimitiveTypeNone");
        return false;
    }

    const auto [depth_copy, stencil_copy] = DepthStencilOverrideCopy(regs);
    if (depth_copy || stencil_copy) {
        // We need to detect this case and perform the copy, otherwise it will have no effect.
        LOG_TRACE(Render_Vulkan, "Performing depth-stencil override copy");
        DepthStencilCopy(depth_copy, stencil_copy);
        return false;
    }

    return true;
}

namespace DepthFastStats = DrawPhase::DepthFastStats; // sotc: DepthFastPath counters

void Rasterizer::PrepareRenderState(const GraphicsPipeline* pipeline) {
    const auto texture_lock = texture_cache.BatchLock(); // sotc: one lock for the targets
    DrawPhase::Sub(DrawPhase::SPrepLock);
    // Prefetch render targets to handle overlaps with bound textures (e.g. mipgen)
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = liverpool->DrawRegs();

    // sotc: BbToggle::DrawBindMemo (RtMemo): the same targets as the last draw, in the same
    // registry state: cb_descs and db_desc still hold their images.
    const u32 num_cb = std::bit_width(key.mrt_mask);
    const bool cb_disabled =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    const bool memo_on = DrawBindMemoOn();
    u32 active_mask = 0;
    for (u32 cb = 0; cb < num_cb; ++cb) {
        active_mask |= u32{regs.color_target_mask.GetMask(cb) != 0} << cb;
    }
    // sotc: BbToggle::DepthFastPath: no register the render target block reads, and no extent,
    // changed in value since the memo was stored (Liverpool::DrawRelSerial), so every register
    // comparison below is known to come out equal and is not made (SOTC_DEPTHFAST_VERIFY=1 makes
    // it as well and checks).
    const bool depth_fast = memo_on && DepthFastActive();
    bool rel_hit = false;
    if (depth_fast && rt_memo.valid &&
        rt_memo.rel_depth == liverpool->DrawRelSerial(AmdGpu::Liverpool::RelRender) &&
        (num_cb == 0 ||
         rt_memo.rel_color == liverpool->DrawRelSerial(AmdGpu::Liverpool::RelColor))) {
        rel_hit = rt_memo.mrt_mask == key.mrt_mask &&
                  rt_memo.image_generation == texture_cache.ImageSetGeneration() &&
                  rt_memo.gpu_epoch == VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed);
    }
    bool depth_enable = false, stencil_enable = false, depth_write = false;
    u32 htile = 0;
    bool memo_hit = rel_hit;
    if (!rel_hit || DepthFastStats::kVerify) {
        depth_enable = regs.depth_control.depth_enable;
        stencil_enable = regs.depth_control.stencil_enable;
        depth_write = regs.depth_control.depth_write_enable;
        htile = regs.depth_htile_data_base.address;
        bool old_hit = memo_on && rt_memo.valid && rt_memo.mrt_mask == key.mrt_mask &&
            rt_memo.active_mask == active_mask && rt_memo.cb_disabled == cb_disabled &&
            rt_memo.depth_enable == depth_enable && rt_memo.stencil_enable == stencil_enable &&
            rt_memo.htile == htile &&
            rt_memo.image_generation == texture_cache.ImageSetGeneration() &&
            rt_memo.gpu_epoch == VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed) &&
            std::memcmp(&rt_memo.depth_buffer, &regs.depth_buffer, sizeof(regs.depth_buffer)) == 0 &&
            std::memcmp(&rt_memo.depth_view, &regs.depth_view, sizeof(regs.depth_view)) == 0 &&
            rt_memo.db_extent.raw == liverpool->DrawDbExtent().raw &&
            std::memcmp(rt_memo.cb_extent.data(), liverpool->DrawCbExtent().data(),
                        num_cb * sizeof(AmdGpu::CbDbExtent)) == 0 &&
            std::memcmp(rt_memo.color_buffers.data(), regs.color_buffers,
                        num_cb * sizeof(AmdGpu::ColorBuffer)) == 0;
        // sotc: db_desc.view_info.is_storage is depth_write_enable (ImageViewInfo for a depth buffer)
        // and BeginRendering picks the depth layout and the view usage from it, so a memo that
        // differs only in the write enable must not replay the old db_desc (stale layout:
        // attachment-optimal for a read-only pass, or, in the other direction, a read-only layout
        // with writes enabled).
        if (old_hit && rt_memo.depth_write != depth_write) {
            old_hit = false;
        }
        if (rel_hit) { // verify only: the register-free test said hit, the comparison must agree
            ++DepthFastStats::c.prep_verify;
            if (!old_hit) {
                if (++DepthFastStats::c.prep_bad <= 20) {
                    std::fprintf(stderr,
                                 "SOTC DEPTHFAST VERIFY: render target memo replayed by the "
                                 "register serial but the registers differ (mismatch %llu)%c",
                                 static_cast<unsigned long long>(DepthFastStats::c.prep_bad), 10);
                }
                memo_hit = false;
            }
        } else {
            memo_hit = old_hit;
            if (depth_fast) {
                ++DepthFastStats::c.prep_miss;
                DepthFastStats::c.prep_old_only += old_hit;
            }
        }
    } else {
        ++DepthFastStats::c.prep_hit;
    }
    DrawPhase::Sub(DrawPhase::SPrepCmp);
    if (memo_hit) {
        for (u32 cb = 0; cb < num_cb; ++cb) {
            if (const auto image_id = cb_descs[cb].first) {
                auto& image = texture_cache.GetImage(image_id);
                texture_cache.NoteFound(image);
                bound_images.emplace_back(image_id);
                image.binding.is_target = 1u;
            }
        }
        if (const auto image_id = db_desc.first) {
            auto& image = texture_cache.GetImage(image_id);
            texture_cache.NoteFound(image);
            bound_images.emplace_back(image_id);
            image.binding.is_target = 1u;
        }
        if (SotcStats::kFpsLog) {
            SotcStats::rt_memo_hits.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    if (SotcStats::kFpsLog) {
        SotcStats::rt_memo_misses.fetch_add(1, std::memory_order_relaxed);
    }
    rt_memo.valid = false;
    if (regs.color_control.degamma_enable) {
        // sotc: once per process, not once per draw (the LOG macro looks the logger up in a map
        // before checking the level, and this flag can stay set for a whole scene).
        static const bool shown = [] {
            LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
            return true;
        }();
        (void)shown;
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (skip_cb_binding || !col_buf || !target_mask || (key.mrt_mask & (1 << cb)) == 0) {
            image_id = {};
            continue;
        }
        const auto& hint = liverpool->DrawCbExtent()[cb];
        std::construct_at(&desc, col_buf, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    }

    if ((regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid())) {
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& hint = liverpool->DrawDbExtent();
        auto& [image_id, desc] = db_desc;
        std::construct_at(&desc, regs.depth_buffer, regs.depth_view, regs.depth_control,
                          htile_address, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    } else {
        db_desc.first = {};
    }

    if (memo_on) {
        rt_memo.valid = true;
        rt_memo.mrt_mask = key.mrt_mask;
        rt_memo.active_mask = active_mask;
        rt_memo.cb_disabled = cb_disabled;
        rt_memo.depth_enable = depth_enable;
        rt_memo.stencil_enable = stencil_enable;
        rt_memo.depth_write = depth_write;
        rt_memo.htile = htile;
        rt_memo.rel_depth = liverpool->DrawRelSerial(AmdGpu::Liverpool::RelRender); // sotc: DepthFastPath
        rt_memo.rel_color = liverpool->DrawRelSerial(AmdGpu::Liverpool::RelColor);
        rt_memo.image_generation = texture_cache.ImageSetGeneration();
        rt_memo.gpu_epoch = VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed);
        std::memcpy(&rt_memo.depth_buffer, &regs.depth_buffer, sizeof(regs.depth_buffer));
        std::memcpy(&rt_memo.depth_view, &regs.depth_view, sizeof(regs.depth_view));
        rt_memo.db_extent = liverpool->DrawDbExtent();
        std::memcpy(rt_memo.cb_extent.data(), liverpool->DrawCbExtent().data(),
                    num_cb * sizeof(AmdGpu::CbDbExtent));
        std::memcpy(rt_memo.color_buffers.data(), regs.color_buffers,
                    num_cb * sizeof(AmdGpu::ColorBuffer));
    }
}

static std::pair<u32, u32> GetDrawOffsets(const AmdGpu::Regs& regs, const Shader::Info& info,
                                          const Shader::Gcn::FetchShaderData& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (!fetch_shader.Empty()) {
        if (vertex_offset == 0 && fetch_shader.vertex_offset_sgpr != -1) {
            vertex_offset = info.UserData()[fetch_shader.vertex_offset_sgpr];
        }
        if (fetch_shader.instance_offset_sgpr != -1) {
            instance_offset = info.UserData()[fetch_shader.instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

void Rasterizer::EliminateFastClear() {
    auto& col_buf = liverpool->DrawRegs().color_buffers[0];
    if (!col_buf || !col_buf.info.fast_clear) {
        return;
    }
    VideoCore::TextureCache::ImageDesc desc(col_buf, liverpool->DrawCbExtent()[0]);
    const auto image_id = texture_cache.FindImage(desc);
    const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
    if (!texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start)) {
        return;
    }
    for (u32 slice = col_buf.view.slice_start; slice <= col_buf.view.slice_max; ++slice) {
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
    }
    auto& image = texture_cache.GetImage(image_id);
    const auto clear_value = LiverpoolToVK::ColorBufferClearValue(col_buf);

    ScopeMarkerBegin(fmt::format("EliminateFastClear:MRT={:#x}:M={:#x}", col_buf.Address(),
                                 col_buf.CmaskAddress()));
    runtime.ClearImage(&image, desc.view_info.range, clear_value);
    ScopeMarkerEnd();
}

// sotc: SOTC_PASS_TRACE=1: every 300 flips, the render passes of one frame in order (depth
// target, its size, colour target 0, screen and generic scissors, CB mode, draws) on stderr.
// A diagnostic for finding which passes are the sun shadow cascades.
template <typename Regs>
static void TracePass(const Regs& regs) {
    static const bool on = std::getenv("SOTC_PASS_TRACE") != nullptr;
    if (!on) {
        return;
    }
    struct Pass {
        u64 depth, color;
        u32 pitch, height, mode, slice0, slice1;
        s32 sx0, sy0, sx1, sy1, gx0, gy0, gx1, gy1;
        u32 draws;
        u64 mrt_key; // the bound colour targets (address and format of each)
        char mrts[320];
    };
    static std::vector<Pass> passes;
    static u64 frame_seen = 0;
    const u64 frame = SotcStats::flip_count.load(std::memory_order_relaxed);
    if (frame != frame_seen) {
        if (frame_seen % 300 == 0 && !passes.empty()) {
            std::fprintf(stderr, "SOTC PASSES frame %llu: %zu passes\n",
                         static_cast<unsigned long long>(frame_seen), passes.size());
            for (const auto& p : passes) {
                std::fprintf(stderr,
                             "  depth %#llx %ux%u slices %u-%u color %#llx mode %u screen %d,%d-%d,%d "
                             "generic %d,%d-%d,%d draws %u | mrt %s\n",
                             static_cast<unsigned long long>(p.depth), p.pitch, p.height, p.slice0, p.slice1,
                             static_cast<unsigned long long>(p.color), p.mode, p.sx0, p.sy0,
                             p.sx1, p.sy1, p.gx0, p.gy0, p.gx1, p.gy1, p.draws, p.mrts);
            }
        }
        passes.clear();
        frame_seen = frame;
    }
    if (frame % 300 != 0) {
        return;
    }
    Pass p{
        .depth = regs.depth_buffer.DepthValid() ? regs.depth_buffer.DepthWriteAddress() : 0,
        .color = regs.color_buffers[0].Address(),
        .pitch = regs.depth_buffer.Pitch(),
        .height = regs.depth_buffer.Height(),
        .mode = static_cast<u32>(regs.color_control.mode),
        .slice0 = regs.depth_view.slice_start,
        .slice1 = regs.depth_view.slice_max,
        .sx0 = regs.screen_scissor.top_left_x,
        .sy0 = regs.screen_scissor.top_left_y,
        .sx1 = regs.screen_scissor.bottom_right_x,
        .sy1 = regs.screen_scissor.bottom_right_y,
        .gx0 = regs.generic_scissor.top_left_x,
        .gy0 = regs.generic_scissor.top_left_y,
        .gx1 = regs.generic_scissor.bottom_right_x,
        .gy1 = regs.generic_scissor.bottom_right_y,
        .draws = 1,
        .mrt_key = 0,
        .mrts = {},
    };
    for (u32 i = 0; i < 8; ++i) {
        const auto& cb = regs.color_buffers[i];
        if (cb) {
            p.mrt_key = p.mrt_key * 0x100000001b3ull ^ cb.Address() ^
                        (u64(cb.info.format) << 56) ^ (u64(i) << 60);
        }
    }
    if (!passes.empty()) {
        auto& last = passes.back();
        if (last.depth == p.depth && last.color == p.color && last.pitch == p.pitch &&
            last.mrt_key == p.mrt_key &&
            last.height == p.height && last.mode == p.mode && last.slice0 == p.slice0 &&
            last.slice1 == p.slice1 && last.sx0 == p.sx0 &&
            last.sy0 == p.sy0 && last.sx1 == p.sx1 && last.sy1 == p.sy1 && last.gx0 == p.gx0 &&
            last.gy0 == p.gy0 && last.gx1 == p.gx1 && last.gy1 == p.gy1) {
            ++last.draws;
            return;
        }
    }
    int at = 0;
    for (u32 i = 0; i < 8 && at < int(sizeof(p.mrts)) - 64; ++i) {
        const auto& cb = regs.color_buffers[i];
        if (cb) {
            at += std::snprintf(p.mrts + at, sizeof(p.mrts) - at, "%u:%#llx d%u n%u %ux%u ", i,
                                static_cast<unsigned long long>(cb.Address()),
                                static_cast<u32>(cb.GetDataFmt()), static_cast<u32>(cb.GetNumberFmt()),
                                cb.Pitch(), cb.Height());
        }
    }
    passes.push_back(p);
}

// sotc: SOTC_PASS_TRACE: with the pass list, the shader hashes (all stages, as the dump file
// names) of the G-buffer draws, recognised by their R16G16 float target in MRT 4.
template <typename Regs, typename Pipe>
static void TraceGbufferShaders(const Regs& regs, const Pipe* pipeline) {
    static const bool on = std::getenv("SOTC_PASS_TRACE") != nullptr;
    if (!on || !pipeline) {
        return;
    }
    static std::map<std::array<u64, 6>, u32> counts;
    static u64 frame_seen = 0;
    const u64 frame = SotcStats::flip_count.load(std::memory_order_relaxed);
    if (frame != frame_seen) {
        if (frame_seen % 300 == 0 && !counts.empty()) {
            std::fprintf(stderr, "SOTC GBUFFER SHADERS frame %llu:\n",
                         static_cast<unsigned long long>(frame_seen));
            for (const auto& [key, draws] : counts) {
                std::fprintf(stderr, "  draws %4u:", draws);
                for (u64 hash : key) {
                    if (hash) {
                        std::fprintf(stderr, " %#018llx", static_cast<unsigned long long>(hash));
                    }
                }
                std::fprintf(stderr, "\n");
            }
        }
        counts.clear();
        frame_seen = frame;
    }
    if (frame % 300 != 0) {
        return;
    }
    const auto& cb4 = regs.color_buffers[4];
    if (!cb4 || cb4.GetDataFmt() != AmdGpu::DataFormat::Format16_16 ||
        cb4.GetNumberFmt() != AmdGpu::NumberFormat::Float) {
        return;
    }
    std::array<u64, 6> key{};
    size_t n = 0;
    for (const auto* info : pipeline->GetStages()) {
        if (info && n < key.size()) {
            key[n++] = info->pgm_hash;
        }
    }
    ++counts[key];
}

// sotc: SOTC_PASS_TRACE: the shader hashes of the draws into square depth targets of 512+ texels
// (the sun shadow map's cascades) per slice, every 300 flips.
template <typename Regs, typename Pipe>
static void TraceShadowShaders(const Regs& regs, const Pipe* pipeline) {
    static const bool on = std::getenv("SOTC_PASS_TRACE") != nullptr;
    if (!on || !pipeline || !regs.depth_buffer.DepthValid()) {
        return;
    }
    static std::map<std::pair<u32, u64>, u32> counts;
    static u64 frame_seen = 0;
    const u64 frame = SotcStats::flip_count.load(std::memory_order_relaxed);
    if (frame != frame_seen) {
        if (frame_seen % 300 == 0 && !counts.empty()) {
            std::fprintf(stderr, "SOTC SHADOW SHADERS frame %llu:\n",
                         static_cast<unsigned long long>(frame_seen));
            for (const auto& [key, draws] : counts) {
                std::fprintf(stderr, "  slice %u vs %#018llx draws %u\n", key.first,
                             static_cast<unsigned long long>(key.second), draws);
            }
        }
        counts.clear();
        frame_seen = frame;
    }
    if (frame % 300 != 0) {
        return;
    }
    const u32 pitch = regs.depth_buffer.Pitch();
    if (pitch != regs.depth_buffer.Height() || pitch < 512) {
        return;
    }
    const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
    ++counts[{regs.depth_view.slice_start, vs.pgm_hash}];
}

// sotc: SOTC_SHADOW_STAGGER=1: the two farthest sun shadow cascades are redrawn on alternate
// frames (slice 2 on odd flips, slice 3 on even ones). A skipped slice starts no render pass, so
// it is not cleared and keeps the previous frame's shadows. The sun shadow map is the square
// depth target that got the most draws in the previous frame (SotC: 1024x1024, 5 slices, ~6500
// draws per frame, 3041 of them in slice 3 and 2049 in slice 2, SOTC_PASS_TRACE). Far shadows
// then lag one frame behind the camera; the two slices cost half as much.
template <typename Regs>
static bool SkipStaggeredShadow(const Regs& regs) {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_SHADOW_STAGGER");
        return v && v[0] == '1';
    }();
    if (!on) {
        return false;
    }
    static u64 frame_seen = 0, sun_map = 0;
    static std::unordered_map<u64, u32> counts; // draws per square depth target this frame
    const u64 frame = SotcStats::flip_count.load(std::memory_order_relaxed);
    if (frame != frame_seen) {
        u64 most_addr = 0;
        u32 most = 0;
        for (const auto& [addr, draws] : counts) {
            if (draws > most) {
                most = draws;
                most_addr = addr;
            }
        }
        sun_map = most >= 500 ? most_addr : 0;
        counts.clear();
        frame_seen = frame;
    }
    if (!regs.depth_buffer.DepthValid() ||
        regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Normal) {
        return false;
    }
    const u32 pitch = regs.depth_buffer.Pitch();
    if (pitch != regs.depth_buffer.Height() || pitch < 512) {
        return false;
    }
    const u64 addr = regs.depth_buffer.DepthWriteAddress();
    ++counts[addr];
    if (addr != sun_map) {
        return false;
    }
    const u32 slice = regs.depth_view.slice_start;
    return slice == 2 ? (frame & 1) != 0 : slice == 3 ? (frame & 1) == 0 : false;
}

// sotc: SOTC_SHADOW_REUSE (default 0 = off; 1 = on, check = verify once and save the result). Off
// by default: with it, far shadows on the ground blink at 15 Hz with a still camera (the Shrine:
// a rock's shadow there on drawn frames, gone on moved ones; 0.36% of the pixels flicker per
// frame against 0.01% off). SOTC_SHADOW_STAGGER, which only skips, blinks the same, so the game
// does not keep a skipped cascade's texels until the next frame. When on: the far
// sun shadow cascades (slices 2 and 3 of the sun shadow map, the square depth target with the
// most draws in the previous frame; ~2000 draws each) are drawn on alternate frames, and on the
// other frames last frame's cascade is moved instead (vk_shadow_scroll.h). The engine snaps its
// cascades to texels: between two frames only the translation of a cascade's (orthographic)
// light matrix changes, by whole texels in x and y. Moving last frame's texels by that many
// texels, and their depths by the z change, gives the cascade a redraw would (except for things
// that moved since, whose far shadows update every other frame, and the few texels entering at
// the edge). The light matrix is the shadow view's world -> clip at +0x80 of the view
// constants, as for the camera (*(*(VS user data 0..1) + 8), vk_dlssg.cpp). Any other change
// (rotation, scale, viewport, a non-whole-texel or large move): the cascade is drawn. Unlike
// SOTC_SHADOW_STAGGER (texels of another matrix: shadows jumping as the camera moved), a reused
// cascade always matches the matrix the lighting uses.
namespace {
struct ShadowReuseState {
    static constexpr u32 kSlices = 8;
    u64 frame_seen = 0, sun_map = 0;
    std::unordered_map<u64, u32> counts; // draws per square depth target this frame
    std::array<std::array<float, 16>, kSlices> last_vp{};
    std::array<std::array<float, 3>, kSlices> last_scale{};
    std::array<bool, kSlices> have_vp{}, decided{}, skip{}, drawn_prev{}, drawn_now{};
    // The light matrix a slice was decided for this frame, and the image it was drawn into.
    std::array<std::array<float, 16>, kSlices> decided_vp{};
    std::array<VideoCore::ImageId, kSlices> last_image{};
    std::array<u64, kSlices> last_image_uid{};
    u64 prev_key = 0; // target of the previous draw (address * 8 + slice), 0: another kind
    u64 drawn = 0, scrolled = 0, refused = 0, invalid = 0, rendered_again = 0, image_changed = 0;
    s32 check_slice = -1; // verification: the slice scrolled into the scratch image
    bool check_done = false;
};
} // namespace

bool Rasterizer::ReuseShadowCascade() {
    // sotc: SOTC_SHADOW_REUSE=auto (the launcher's default): on with NVIDIA, as in 0.58/0.59, off
    // on AMD and Intel (#178). Unset: off.
    static const int env_mode = [this] {
        const char* v = std::getenv("SOTC_SHADOW_REUSE");
        if (!v) {
            return 0;
        }
        if (v[0] == 'a') {
            constexpr u32 VendorNvidia = 0x10DE;
            return instance.GetVendorID() == VendorNvidia ? 1 : 0;
        }
        return v[0] == '0' ? 0 : (v[0] == 'c' ? 2 : 1);
    }();
    // The in-game menu can switch it while playing (sotc_live.h).
    const int live = SotcLive::shadow_reuse.load(std::memory_order_relaxed);
    const int mode = live >= 0 ? live : env_mode;
    if (mode == 0) {
        return false;
    }
    static ShadowReuseState s;
    const auto& regs = liverpool->DrawRegs(); // sotc: the draw thread's copy there
    const u64 frame = SotcStats::cp_frame.load(std::memory_order_relaxed);
    if (frame != s.frame_seen) {
        u64 most_addr = 0;
        u32 most = 0;
        for (const auto& [addr, draws] : s.counts) {
            if (draws > most) {
                most = draws;
                most_addr = addr;
            }
        }
        if ((most >= 500 ? most_addr : 0) != s.sun_map) {
            s.have_vp.fill(false); // another shadow map: nothing to reuse
        }
        s.sun_map = most >= 500 ? most_addr : 0;
        s.counts.clear();
        s.drawn_prev = s.drawn_now;
        s.drawn_now.fill(false);
        s.decided.fill(false);
        s.frame_seen = frame;
        // Every ~60 s in the log (every 300 frames with SOTC_SHADOW_REUSE_STATS).
        static const bool stats = std::getenv("SOTC_SHADOW_REUSE_STATS") != nullptr;
        if (frame % (stats ? 300 : 1800) == 0) {
            std::fprintf(stderr,
                         "SOTC SHADOW REUSE: far cascades drawn %llu, moved %llu, refused %llu, "
                         "unreadable %llu, drawn twice in a frame %llu, image changed %llu\n",
                         static_cast<unsigned long long>(s.drawn),
                         static_cast<unsigned long long>(s.scrolled),
                         static_cast<unsigned long long>(s.refused),
                         static_cast<unsigned long long>(s.invalid),
                         static_cast<unsigned long long>(s.rendered_again),
                         static_cast<unsigned long long>(s.image_changed));
            s.drawn = s.scrolled = s.refused = s.invalid = s.rendered_again = s.image_changed = 0;
        }
    }
    const bool square_depth =
        regs.depth_buffer.DepthValid() &&
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Normal &&
        regs.depth_buffer.Pitch() == regs.depth_buffer.Height() && regs.depth_buffer.Pitch() >= 512;
    const u64 addr = square_depth ? regs.depth_buffer.DepthWriteAddress() : 0;
    const u32 slice = regs.depth_view.slice_start;
    const u64 key = square_depth ? addr * 8 + (slice & 7) : 0;
    const bool new_run = key != s.prev_key; // first draw after draws into another target
    s.prev_key = key;
    if (!square_depth) {
        return false;
    }
    ++s.counts[addr];
    if (addr != s.sun_map || (slice != 2 && slice != 3)) {
        return false;
    }
    if (s.decided[slice] && !new_run) {
        return s.skip[slice];
    }

    // The cascade's light matrix (rows at +0x80 of the shadow view's constants).
    constexpr u64 kMask48 = 0xFFFFFFFFFFFFull;
    const u64 ud =
        (u64(regs.vs_program.user_data[0]) | (u64(regs.vs_program.user_data[1]) << 32)) & kMask48;
    u64 cb = 0;
    std::array<float, 16> vp{};
    bool valid = ud && memory->TryReadBacking(ud + 8, &cb, sizeof(cb)) && (cb &= kMask48) &&
                 memory->TryReadBacking(cb + 0x80, vp.data(), sizeof(vp));
    if (valid) {
        for (float f : vp) {
            valid &= std::isfinite(f);
        }
        // Orthographic (sun light): the w row is (0, 0, 0, 1).
        valid &= vp[12] == 0.0f && vp[13] == 0.0f && vp[14] == 0.0f && vp[15] == 1.0f;
    }
    const auto& viewport = regs.viewports[0];
    const std::array<float, 3> scale{
        regs.viewport_control.xscale_enable ? viewport.xscale : 1.0f,
        regs.viewport_control.yscale_enable ? viewport.yscale : 1.0f,
        regs.viewport_control.zscale_enable ? viewport.zscale : 1.0f};
    // The image the cascade goes to: a moved cascade needs last frame's texels in the same image.
    VideoCore::ImageId image_id{};
    u64 image_uid = 0;
    if (valid) {
        const auto texture_lock = texture_cache.BatchLock();
        const auto htile = regs.depth_htile_data_base.GetAddress();
        VideoCore::TextureCache::ImageDesc desc(regs.depth_buffer, regs.depth_view,
                                                regs.depth_control, htile,
                                                liverpool->DrawDbExtent());
        image_id = texture_cache.FindImage(desc);
        image_uid = texture_cache.GetImage(image_id).image_uid;
    }
    if (s.decided[slice]) {
        // Another run of draws into this slice in the same frame.
        if (valid && vp == s.decided_vp[slice]) {
            return s.skip[slice]; // the same cascade, drawn in several runs
        }
        // Drawn again with another light matrix before the flip (a second scene in one frame, as
        // a slower machine may render): drawn as the game does, and a valid source next frame.
        ++s.rendered_again;
        ++s.drawn;
        s.skip[slice] = false;
        s.decided_vp[slice] = vp;
        s.last_vp[slice] = vp;
        s.last_scale[slice] = scale;
        s.have_vp[slice] = valid;
        s.drawn_now[slice] = true;
        s.last_image[slice] = image_id;
        s.last_image_uid[slice] = image_uid;
        return false;
    }
    s.decided[slice] = true;
    s.skip[slice] = false;
    s.decided_vp[slice] = vp;

    // Verification: the scratch image scrolled last frame against the cascade then drawn.
    if (mode == 2 && s.check_slice >= 0 && !s.check_done && shadow_scroll) {
        const auto texture_lock = texture_cache.BatchLock();
        const auto htile = regs.depth_htile_data_base.GetAddress();
        VideoCore::TextureCache::ImageDesc desc(regs.depth_buffer, regs.depth_view,
                                                regs.depth_control, htile,
                                                liverpool->DrawDbExtent());
        auto& image = texture_cache.GetImage(texture_cache.FindImage(desc));
        const char* dump_dir = std::getenv("BB_DUMP_DIR");
        const auto dir = std::filesystem::path(dump_dir ? dump_dir : ".");
        shadow_scroll->SaveCheck(
            image, static_cast<u32>(s.check_slice),
            (dir / ("shadow_check_slice" + std::to_string(s.check_slice))).string());
        s.check_done = true;
    }

    bool moved = false;
    const bool phase = (frame + slice) % 2 == 1; // each far cascade drawn every other frame
    if (!valid) {
        ++s.invalid;
    } else if (s.have_vp[slice] && s.drawn_prev[slice] && phase) {
        const auto& prev = s.last_vp[slice];
        // Last frame's texels are in the image only if it is the same one (not recreated).
        const bool same_image =
            image_id == s.last_image[slice] && image_uid == s.last_image_uid[slice];
        if (!same_image) {
            ++s.image_changed;
        }
        bool same_basis = same_image && s.last_scale[slice] == scale;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                same_basis &= vp[r * 4 + c] == prev[r * 4 + c];
            }
        }
        const double fx = double(scale[0]) * (double(vp[3]) - prev[3]);
        const double fy = double(scale[1]) * (double(vp[7]) - prev[7]);
        const float dz = static_cast<float>(double(scale[2]) * (double(vp[11]) - prev[11]));
        const double rx = std::round(fx), ry = std::round(fy);
        const bool whole = std::abs(fx - rx) < 0.02 && std::abs(fy - ry) < 0.02 &&
                           std::abs(rx) <= 128 && std::abs(ry) <= 128;
        static u32 logged = 0;
        if ((!same_basis || !whole) && std::getenv("SOTC_SHADOW_REUSE_STATS") && logged < 20) {
            ++logged;
            std::fprintf(stderr,
                         "SOTC SHADOW REUSE refused slice %u: basis %s, move %.4f,%.4f texels, dz "
                         "%g, scale %g %g %g (was %g %g %g)\n",
                         slice, same_basis ? "same" : "changed", fx, fy, dz, scale[0], scale[1],
                         scale[2], s.last_scale[slice][0], s.last_scale[slice][1],
                         s.last_scale[slice][2]);
        }
        if (same_basis && whole) {
            if (!shadow_scroll) {
                shadow_scroll = std::make_unique<ShadowScroll>(instance, scheduler, runtime);
            }
            const auto texture_lock = texture_cache.BatchLock();
            const auto htile = regs.depth_htile_data_base.GetAddress();
            VideoCore::TextureCache::ImageDesc desc(regs.depth_buffer, regs.depth_view,
                                                    regs.depth_control, htile,
                                                    liverpool->DrawDbExtent());
            auto& image = texture_cache.GetImage(texture_cache.FindImage(desc));
            const bool check = mode == 2;
            if (check && (s.check_slice >= 0 || (rx == 0 && ry == 0))) {
                // Verification done or pending: draw as usual.
            } else if (shadow_scroll->Scroll(image, slice, static_cast<s32>(rx),
                                             static_cast<s32>(ry), dz, regs.depth_clear, check)) {
                if (check) {
                    s.check_slice = static_cast<s32>(slice);
                    std::fprintf(stderr,
                                 "SOTC SHADOW CHECK slice %u frame %llu shift %d,%d dz %g "
                                 "(scale %g %g %g, clear %g, format %d)\n",
                                 slice, static_cast<unsigned long long>(frame), int(rx), int(ry),
                                 dz, scale[0], scale[1], scale[2], regs.depth_clear,
                                 static_cast<int>(image.info.pixel_format));
                } else {
                    moved = true;
                }
            }
        }
        if (!moved && mode != 2) {
            ++s.refused;
        }
    }
    s.last_vp[slice] = vp;
    s.last_scale[slice] = scale;
    s.have_vp[slice] = valid;
    s.last_image[slice] = image_id;
    s.last_image_uid[slice] = image_uid;
    s.skip[slice] = moved;
    s.drawn_now[slice] = !moved;
    ++(moved ? s.scrolled : s.drawn);
    return moved;
}

// sotc: TakeHudless (vk_rasterizer.h). An image of the video out size, created once per video out
// address in the format of the composite's attachment (mutable: the presenter reads it in the
// video out format).
static bool CreateHudlessImage(const Instance& instance, vk::Format format, u32 width, u32 height,
                               vk::ImageUsageFlags usage, vk::Image& image, vk::ImageView& view,
                               VmaAllocation& allocation) {
    const VkImageCreateInfo info = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = usage,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    const VmaAllocationCreateInfo alloc_info = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage raw{};
    if (vmaCreateImage(instance.GetAllocator(), &info, &alloc_info, &raw, &allocation, nullptr) !=
        VK_SUCCESS) {
        return false;
    }
    image = vk::Image{raw};
    auto [result, created] = instance.GetDevice().createImageView(vk::ImageViewCreateInfo{
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    });
    if (result != vk::Result::eSuccess) {
        vmaDestroyImage(instance.GetAllocator(), image, allocation);
        image = vk::Image{};
        return false;
    }
    view = created;
    return true;
}

void Rasterizer::DestroyHudless(HudlessImage& hudless) {
    const vk::Device device = instance.GetDevice();
    if (hudless.read_view) {
        device.destroyImageView(hudless.read_view);
    }
    if (hudless.rt_view) {
        device.destroyImageView(hudless.rt_view);
    }
    if (hudless.image) {
        vmaDestroyImage(instance.GetAllocator(), hudless.image, hudless.allocation);
    }
    hudless = {};
}

bool Rasterizer::BeginHudless(const GraphicsPipeline* pipeline, const RenderState& state) {
    // ponytail: one image per video out address, never resized: a video out of another size or
    // format (a resolution change) gets no HUD-less image; recreate it after a Finish if needed.
    auto& target = hudless_images[hudless_vo];
    if (!target.image) {
        if (!CreateHudlessImage(instance, hudless_vo_format, hudless_vo_width, hudless_vo_height,
                                vk::ImageUsageFlagBits::eColorAttachment |
                                    vk::ImageUsageFlagBits::eSampled,
                                target.image, target.rt_view, target.allocation)) {
            return false;
        }
        target.rt_format = hudless_vo_format;
        target.width = hudless_vo_width;
        target.height = hudless_vo_height;
    }
    if (target.rt_format != hudless_vo_format || target.width != hudless_vo_width ||
        target.height != hudless_vo_height) {
        return false;
    }
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::ImageSubresourceRange color_range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    if (!hudless_clear.image) {
        if (!CreateHudlessImage(instance, vk::Format::eR8G8B8A8Unorm, 1, 1,
                                vk::ImageUsageFlagBits::eSampled |
                                    vk::ImageUsageFlagBits::eTransferDst,
                                hudless_clear.image, hudless_clear.rt_view,
                                hudless_clear.allocation)) {
            return false;
        }
        const auto to_clear = vk::ImageMemoryBarrier2{
            .dstStageMask = vk::PipelineStageFlagBits2::eClear,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = hudless_clear.image,
            .subresourceRange = color_range,
        };
        cmdbuf.pipelineBarrier2(
            vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_clear});
        cmdbuf.clearColorImage(hudless_clear.image, vk::ImageLayout::eTransferDstOptimal,
                               vk::ClearColorValue{}, color_range);
        const auto to_read = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eClear,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = hudless_clear.image,
            .subresourceRange = color_range,
        };
        cmdbuf.pipelineBarrier2(
            vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    }
    // The composite writes every pixel: the last frame's content (read by the presenter) can go.
    const auto to_target = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = target.image,
        .subresourceRange = color_range,
    };
    cmdbuf.pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_target});
    auto& ui = image_infos[hudless_ui_info];
    ui.imageView = hudless_clear.rt_view;
    ui.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    pipeline->BindResources(set_writes, push_data);
    SotcStats::push_clobbers.fetch_add(1, std::memory_order_relaxed); // sotc: reused binds repush
    RenderState hudless_state = state;
    auto& attachment = hudless_state.color_attachments[0];
    attachment.image_view = target.rt_view;
    attachment.image_layout = vk::ImageLayout::eColorAttachmentOptimal;
    attachment.is_clear = 0;
    scheduler.BeginRendering(hudless_state);
    target.fresh = true;
    static bool reported = false;
    if (!std::exchange(reported, true)) {
        std::fprintf(stderr, "SOTC FG HUD-less: final composite into %#llx (%ux%u) redrawn without UI\n",
                     static_cast<unsigned long long>(hudless_vo), target.width, target.height);
    }
    return true;
}

vk::ImageView Rasterizer::TakeHudless(VAddr address, vk::Format format) {
    const auto it = hudless_images.find(address);
    if (it == hudless_images.end() || !std::exchange(it->second.fresh, false)) {
        return {};
    }
    auto& target = it->second;
    if (!target.read_view) {
        // As the presenter's view of the video out image: alpha excluded.
        auto [result, view] = instance.GetDevice().createImageView(vk::ImageViewCreateInfo{
            .image = target.image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .components = {.a = vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        });
        if (result != vk::Result::eSuccess) {
            return {};
        }
        target.read_view = view;
        target.read_format = format;
    }
    if (target.read_format != format) {
        return {}; // ponytail: one read format per video out image (its pixel format is fixed)
    }
    scheduler.EndRendering();
    const auto to_read = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = target.image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    scheduler.CommandBuffer().pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    return target.read_view;
}

void Rasterizer::Draw(bool is_indexed, u32 index_offset, const Pipeline* resolved,
                      bool use_resolved) {
    RENDERER_TRACE;
    DrawPhase::Scope ph;

    scheduler.PopPendingOperations();
    DrawPhase::Sub(DrawPhase::SPop);
    if (!df_rel_built) {
        DepthFastEnsureRelevance(); // sotc: DepthFastPath, once
    }
    TracePass(liverpool->DrawRegs());

    if (!FilterDraw()) {
        return;
    }
    if (SkipStaggeredShadow(liverpool->DrawRegs()) || ReuseShadowCascade()) {
        return;
    }
    DrawPhase::Sub(DrawPhase::SFilter);

    const auto& regs = liverpool->DrawRegs();
    const GraphicsPipeline* pipeline =
        use_resolved ? static_cast<const GraphicsPipeline*>(resolved)
                     : pipeline_cache.GetGraphicsPipeline();
    SotcStats::current_program.store(1, std::memory_order_relaxed);
    if (SotcStats::kCmdStats) { // sotc: read only by the SOTC_CMD_STATS printer (sotc_stats.h)
        SotcStats::cmd_kinds[0].fetch_add(1, std::memory_order_relaxed);
    }
    if (!pipeline) {
        SotcStats::Drop(0, regs.color_buffers[0].Address(), regs.stage_enable.raw);
        if (SotcOps::kOn) SotcOps::NoteDrawDropped(); // sotc: SOTC_OP_STATS=1
        return;
    }
    TraceGbufferShaders(regs, pipeline);
    TraceShadowShaders(regs, pipeline);
    DrawPhase::Mark(DrawPhase::Pre);

    PrepareRenderState(pipeline);
    DrawPhase::Mark(DrawPhase::Prep);
    hudless_ui_info = -1; // sotc: TakeHudless, set by BindTextures and BeginRendering
    hudless_vo = 0;
    if (Dlssg::CapturingInputs()) { // sotc: camera and inputs of frame generation
        Dlssg::NoteDraw(regs, pipeline->GetGraphicsKey().mrt_mask, db_desc.first,
                        cb_descs[4].first, texture_cache);
    }
    if (!BindResources(pipeline)) {
        SotcStats::Drop(1, regs.color_buffers[0].Address(), 0);
        if (SotcOps::kOn) SotcOps::NoteDrawDropped(); // sotc: SOTC_OP_STATS=1
        return;
    }
    DrawPhase::Mark(DrawPhase::BindRest);
    RenderState state_storage;
    u64 state_token = 0;
    const RenderState& state = BeginRenderingFast(pipeline, state_storage, state_token);
    DrawPhase::Mark(DrawPhase::Begin);
    RefreshImageLayouts();
    DrawPhase::Mark(DrawPhase::Refresh);

    BindVertexBuffers(pipeline);
    if (is_indexed) {
        BindIndexBuffer(index_offset);
    }
    DrawPhase::Mark(DrawPhase::VtxIdx);

    if (needs_barrier) {
        runtime.FlushBarriers();
    }
    DrawPhase::Mark(DrawPhase::Args);

    if (fast_bind_hit && fast_bind.clobbers != SotcStats::push_clobbers.load(std::memory_order_relaxed)) {
        fast_bind_hit = false; // sotc: a helper pass overwrote the pushed descriptors
    }
    if (!fast_bind_hit) {
        pipeline->BindResources(set_writes, push_data);
    }
    DrawPhase::Mark(DrawPhase::PipeBind);
    CommitDynamicState(pipeline, is_indexed);
    DrawPhase::Mark(DrawPhase::Dyn);
    scheduler.BeginRendering(state, state_token);
    DrawPhase::Sub(DrawPhase::SSchBegin);

    const auto& vs_info = pipeline->GetStage(Shader::SwStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    const auto [vertex_offset, instance_offset] = GetDrawOffsets(regs, vs_info, fetch_shader);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());
    DrawPhase::Sub(DrawPhase::SSchBind);

    if (is_indexed) {
        cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
                           s32(vertex_offset), instance_offset);
    } else {
        cmdbuf.draw(regs.num_indices, regs.num_instances.NumInstances(), vertex_offset,
                    instance_offset);
    }
    // sotc: the final composite again, without the UI layer (TakeHudless).
    if (hudless_ui_info >= 0 && hudless_vo && BeginHudless(pipeline, state)) {
        const auto hudless_cmdbuf = scheduler.CommandBuffer();
        if (is_indexed) {
            hudless_cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
                                       s32(vertex_offset), instance_offset);
        } else {
            hudless_cmdbuf.draw(regs.num_indices, regs.num_instances.NumInstances(),
                                vertex_offset, instance_offset);
        }
    }
    DebugState.IncDrawCall();
    DrawPhase::Sub(DrawPhase::SSchDraw);
    DrawPhase::Mark(DrawPhase::Sched);

    ResetBindings(false);
    DrawPhase::Sub(DrawPhase::SPostReset);
    FlushPeriodic();
    DrawPhase::Sub(DrawPhase::SPostFlush);
    DrawPhase::Mark(DrawPhase::Post);
    if (SotcOps::kOn) [[unlikely]] { // sotc: SOTC_OP_STATS=1
        OpStatsNoteDraw(regs, pipeline, state, buffer_infos, image_infos, push_data, is_indexed,
                        false, regs.num_indices, regs.num_instances.NumInstances(), 0,
                        vertex_offset, instance_offset);
    }
    DrawPhase::Finish(ph, pipeline, set_writes, buffer_infos, image_infos, push_data, state);
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address, u16 vertex_sgpr_offset,
                              u16 instance_sgpr_offset, const Pipeline* resolved,
                              bool use_resolved) {
    RENDERER_TRACE;
    DrawPhase::Scope ph;

    scheduler.PopPendingOperations();
    DrawPhase::Sub(DrawPhase::SPop);
    if (!df_rel_built) {
        DepthFastEnsureRelevance(); // sotc: DepthFastPath, once
    }
    TracePass(liverpool->DrawRegs());

    if (!FilterDraw()) {
        return;
    }
    if (SkipStaggeredShadow(liverpool->DrawRegs()) || ReuseShadowCascade()) {
        return;
    }
    DrawPhase::Sub(DrawPhase::SFilter);

    if (SotcStats::kCmdStats) { // sotc: the shared SOTC_CMD_STATS switch (sotc_stats.h)
        const u64 args_size = u64{stride} * max_count;
        if (buffer_cache.IsRegionGpuModified(arg_address + offset, args_size) ||
            (count_address && buffer_cache.IsRegionGpuModified(count_address, 4))) {
            SotcStats::cmd_kinds[1].fetch_add(1, std::memory_order_relaxed);
        } else {
            SotcStats::cmd_kinds[2].fetch_add(1, std::memory_order_relaxed);
            u32 first[2]{1, 1};
            if (count_address == 0 && memory->TryReadBacking(arg_address + offset, first, 8) &&
                (first[0] == 0 || first[1] == 0)) {
                SotcStats::cmd_kinds[3].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    const DrawIndirectParams params = {
        .vertex_sgpr_offset = vertex_sgpr_offset,
        .instance_sgpr_offset = instance_sgpr_offset,
    };
    const GraphicsPipeline* pipeline =
        use_resolved ? static_cast<const GraphicsPipeline*>(resolved)
                     : pipeline_cache.GetGraphicsPipeline(params);
    if (!pipeline) {
        SotcStats::Drop(0, liverpool->DrawRegs().color_buffers[0].Address(), 1);
        if (SotcOps::kOn) SotcOps::NoteDrawDropped(); // sotc: SOTC_OP_STATS=1
        return;
    }
    TraceShadowShaders(liverpool->DrawRegs(), pipeline);
    DrawPhase::Mark(DrawPhase::Pre);

    PrepareRenderState(pipeline);
    DrawPhase::Mark(DrawPhase::Prep);
    if (Dlssg::CapturingInputs()) { // sotc: camera and inputs of frame generation
        Dlssg::NoteDraw(liverpool->DrawRegs(), pipeline->GetGraphicsKey().mrt_mask, db_desc.first,
                        cb_descs[4].first, texture_cache);
    }
    if (!BindResources(pipeline)) {
        SotcStats::Drop(1, liverpool->DrawRegs().color_buffers[0].Address(), 1);
        if (SotcOps::kOn) SotcOps::NoteDrawDropped(); // sotc: SOTC_OP_STATS=1
        return;
    }
    DrawPhase::Mark(DrawPhase::BindRest);
    RenderState state_storage;
    u64 state_token = 0;
    const RenderState& state = BeginRenderingFast(pipeline, state_storage, state_token);
    DrawPhase::Mark(DrawPhase::Begin);
    RefreshImageLayouts();
    DrawPhase::Mark(DrawPhase::Refresh);

    BindVertexBuffers(pipeline);
    if (is_indexed) {
        BindIndexBuffer();
    }
    DrawPhase::Mark(DrawPhase::VtxIdx);

    // sotc: SotC writes the arguments of most indirect draws with WRITE_DATA right before them
    // (thousands per frame). With the arguments CPU-side, read them here and draw directly: no
    // upload of the argument range, no barrier against the next WRITE_DATA. The pipeline is the
    // same; the base vertex/instance reach the shader through the draw call in both cases.
    // SOTC_INDIRECT_DIRECT=0 keeps the indirect draws.
    static const bool direct_args = [] {
        const char* v = std::getenv("SOTC_INDIRECT_DIRECT");
        return !v || v[0] != '0';
    }();
    // Read through the backing view: the page may be read-protected for other GPU-written bytes.
    union {
        VkDrawIndexedIndirectCommand indexed;
        VkDrawIndirectCommand plain;
    } args;
    if (direct_args && count_address == 0 && max_count == 1 &&
        stride == (is_indexed ? sizeof(VkDrawIndexedIndirectCommand)
                              : sizeof(VkDrawIndirectCommand)) &&
        !buffer_cache.IsRegionGpuModified(arg_address + offset, stride) &&
        memory->TryReadBacking(arg_address + offset, &args, stride)) {
        if (needs_barrier) {
            runtime.FlushBarriers();
        }
        DrawPhase::Mark(DrawPhase::Args);
        if (fast_bind_hit && fast_bind.clobbers != SotcStats::push_clobbers.load(std::memory_order_relaxed)) {
            fast_bind_hit = false; // sotc: a helper pass overwrote the pushed descriptors
        }
        if (!fast_bind_hit) {
            pipeline->BindResources(set_writes, push_data);
        }
        DrawPhase::Mark(DrawPhase::PipeBind);
        CommitDynamicState(pipeline, is_indexed);
        DrawPhase::Mark(DrawPhase::Dyn);
        scheduler.BeginRendering(state, state_token);
        DrawPhase::Sub(DrawPhase::SSchBegin);
        const auto cmdbuf = scheduler.CommandBuffer();
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());
        DrawPhase::Sub(DrawPhase::SSchBind);
        if (is_indexed) {
            const auto& a = args.indexed;
            cmdbuf.drawIndexed(a.indexCount, a.instanceCount, a.firstIndex, a.vertexOffset,
                               a.firstInstance);
        } else {
            const auto& a = args.plain;
            cmdbuf.draw(a.vertexCount, a.instanceCount, a.firstVertex, a.firstInstance);
        }
        DebugState.IncDrawCall();
        DrawPhase::Sub(DrawPhase::SSchDraw);
        DrawPhase::Mark(DrawPhase::Sched);
        ResetBindings(false);
        DrawPhase::Sub(DrawPhase::SPostReset);
        FlushPeriodic();
        DrawPhase::Sub(DrawPhase::SPostFlush);
        DrawPhase::Mark(DrawPhase::Post);
        ShadowProbeNote(liverpool->DrawRegs(), pipeline, set_writes, buffer_infos, image_infos,
                        push_data, state, &args, stride);
        if (SotcOps::kOn) [[unlikely]] { // sotc: SOTC_OP_STATS=1
            if (is_indexed) {
                const auto& a = args.indexed;
                OpStatsNoteDraw(liverpool->DrawRegs(), pipeline, state, buffer_infos, image_infos,
                                push_data, true, false, a.indexCount, a.instanceCount,
                                a.firstIndex, static_cast<u32>(a.vertexOffset), a.firstInstance);
            } else {
                const auto& a = args.plain;
                OpStatsNoteDraw(liverpool->DrawRegs(), pipeline, state, buffer_infos, image_infos,
                                push_data, false, false, a.vertexCount, a.instanceCount,
                                a.firstVertex, 0, a.firstInstance);
            }
        }
        DrawPhase::Finish(ph, pipeline, set_writes, buffer_infos, image_infos, push_data, state);
        return;
    }

    // sotc: SOTC_INDIRECT_TRACE=1: the indirect draws that keep their GPU arguments (first 80):
    // where the arguments are and what guest memory holds there (missing GPU particles).
    static const bool indirect_trace = std::getenv("SOTC_INDIRECT_TRACE") != nullptr;
    if (indirect_trace) {
        static int shown = 0;
        if (shown++ < 80) {
            u32 first[4]{};
            const bool read = memory->TryReadBacking(arg_address + offset, first, sizeof(first));
            std::fprintf(stderr,
                         "SOTC indirect gpu %d read %d args %#llx+%#x stride %u max %u "
                         "count@%#llx mem %u %u %u %u vs %#llx\n",
                         int(buffer_cache.IsRegionGpuModified(arg_address + offset, stride)),
                         int(read), static_cast<unsigned long long>(arg_address), offset, stride,
                         max_count, static_cast<unsigned long long>(count_address), first[0],
                         first[1], first[2], first[3],
                         static_cast<unsigned long long>(
                             liverpool->DrawRegs().vs_program.Address<uintptr_t>()));
        }
    }
    const auto [buffer, base] =
        buffer_cache.ObtainBuffer(arg_address + offset, stride * max_count, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, stride * max_count);

    const VideoCore::Buffer* count_buffer;
    u64 count_offset;
    if (count_address != 0) {
        std::tie(count_buffer, count_offset) = buffer_cache.ObtainBuffer(count_address, 4, false);
        needs_barrier |= runtime.IsBufferAccessed(count_buffer, count_offset, 4);
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    if (fast_bind_hit && fast_bind.clobbers != SotcStats::push_clobbers.load(std::memory_order_relaxed)) {
        fast_bind_hit = false; // sotc: a helper pass overwrote the pushed descriptors
    }
    if (!fast_bind_hit) {
        pipeline->BindResources(set_writes, push_data);
    }
    CommitDynamicState(pipeline, is_indexed);
    scheduler.BeginRendering(state, state_token);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        ASSERT(sizeof(VkDrawIndexedIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndexedIndirectCount(buffer->Handle(), base, count_buffer->Handle(),
                                            count_offset, max_count, stride);
        } else {
            cmdbuf.drawIndexedIndirect(buffer->Handle(), base, max_count, stride);
        }
        DebugState.IncDrawCall();
    } else {
        ASSERT(sizeof(VkDrawIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndirectCount(buffer->Handle(), base, count_buffer->Handle(), count_offset,
                                     max_count, stride);
        } else {
            cmdbuf.drawIndirect(buffer->Handle(), base, max_count, stride);
        }
        DebugState.IncDrawCall();
    }

    // Record the indirect argument reads so a later write to the same range (e.g. a WRITE_DATA
    // packet uploading the next draw's arguments) waits for this draw instead of racing it.
    runtime.AccessBuffer(buffer, base, stride * max_count,
                         vk::PipelineStageFlagBits2::eDrawIndirect,
                         vk::AccessFlagBits2::eIndirectCommandRead);
    if (count_address != 0) {
        runtime.AccessBuffer(count_buffer, count_offset, 4,
                             vk::PipelineStageFlagBits2::eDrawIndirect,
                             vk::AccessFlagBits2::eIndirectCommandRead);
    }

    ResetBindings(false);
    FlushPeriodic();
    if (SotcOps::kOn) [[unlikely]] { // sotc: SOTC_OP_STATS=1, arguments written by the GPU
        OpStatsNoteDraw(liverpool->DrawRegs(), pipeline, state, buffer_infos, image_infos,
                        push_data, is_indexed, true, 0, 0, 0, 0, 0);
    }
}

// sotc: SOTC_DISP_PHASES: buffers (all, written) of a compute stage, for the dispatch counters.
static void DispShape(const Shader::Info& cs, u32& bufs, u32& wbufs) {
    bufs = wbufs = 0;
    for (const auto& desc : cs.buffers) {
        if (!desc.IsSpecial()) {
            ++bufs;
            wbufs += desc.is_written ? 1 : 0;
        }
    }
}

void Rasterizer::DispatchDirect(const Pipeline* resolved, bool use_resolved) {
    RENDERER_TRACE;
    DispPhase::Begin(); // sotc: SOTC_DISP_PHASES

    scheduler.PopPendingOperations();

    const auto& cs_program = liverpool->DrawCsRegs();
    const ComputePipeline* pipeline = use_resolved ? static_cast<const ComputePipeline*>(resolved)
                                                   : pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        SotcStats::Drop(2, cs_program.Address<uintptr_t>(), 0);
        DispPhase::Abort();
        return;
    }

    const auto& cs = pipeline->GetStage(Shader::SwStage::Compute);
    SotcStats::current_program.store(cs.pgm_hash, std::memory_order_relaxed);
    if (SotcOps::kOn) [[unlikely]] { // sotc: SOTC_OP_STATS=1
        SotcOps::NoteDispatch(cs.pgm_hash, u64{cs_program.dim_x} * cs_program.dim_y * cs_program.dim_z);
    }
    if (SotcStats::kCmdStats) { // sotc: read only by the SOTC_CMD_STATS printer (sotc_stats.h)
        SotcStats::cmd_kinds[4].fetch_add(1, std::memory_order_relaxed);
    }
    // sotc: SOTC_CS_TRACE=<hash>: the buffers of the first 6 dispatches of that compute shader,
    // with the first dwords guest memory holds there (diagnostic for the GPU particle counts).
    static const u64 cs_trace = [] {
        const char* v = std::getenv("SOTC_CS_TRACE");
        return v ? std::strtoull(v, nullptr, 0) : u64{0};
    }();
    // SOTC_CS_WATCH=<n>: also report the GPU writes to that shader's buffer n (buffer_cache.cpp).
    static const int cs_watch = [] {
        const char* v = std::getenv("SOTC_CS_WATCH");
        return v ? std::atoi(v) : -1;
    }();
    if (cs_trace && cs.pgm_hash == cs_trace && cs_watch >= 0 &&
        SotcStats::watch_addr.load(std::memory_order_relaxed) == 0) {
        int n = 0;
        for (const auto& desc : cs.buffers) {
            if (!desc.IsSpecial() && n++ == cs_watch) {
                const VAddr a = desc.GetSharp(cs).base_address;
                SotcStats::watch_addr.store(a, std::memory_order_relaxed);
                std::fprintf(stderr, "SOTC cs watch armed at %#llx\n",
                             static_cast<unsigned long long>(a));
            }
        }
    }
    if (cs_trace && cs.pgm_hash == cs_trace) {
        static int shown = 0;
        if (shown++ < 6) {
            std::fprintf(stderr, "SOTC cs %#llx dispatch %ux%ux%u\n",
                         static_cast<unsigned long long>(cs.pgm_hash), cs_program.dim_x,
                         cs_program.dim_y, cs_program.dim_z);
            for (const auto& desc : cs.buffers) {
                if (desc.IsSpecial()) {
                    continue;
                }
                const auto sharp = desc.GetSharp(cs);
                u32 first[8]{};
                memory->TryReadBacking(sharp.base_address, first, sizeof(first));
                std::fprintf(stderr,
                             "SOTC cs   buf %#llx+%#llx written %d gpu %d mem %u %u %u %u %u %u %u "
                             "%u\n",
                             static_cast<unsigned long long>(sharp.base_address),
                             static_cast<unsigned long long>(sharp.GetSize()),
                             int(desc.is_written),
                             int(buffer_cache.IsRegionGpuModified(sharp.base_address, 32)),
                             first[0], first[1], first[2], first[3], first[4], first[5], first[6],
                             first[7]);
            }
        }
    }
    // SOTC_CMD_STATS=2: the dispatched shaders, every 10 s (command processor thread only).
    static const bool shader_stats = [] {
        const char* v = std::getenv("SOTC_CMD_STATS");
        return v && v[0] == '2';
    }();
    if (shader_stats) {
        static std::unordered_map<u64, u64> counts;
        static auto last = std::chrono::steady_clock::now();
        ++counts[cs.pgm_hash];
        if (const auto now = std::chrono::steady_clock::now(); now - last > std::chrono::seconds(10)) {
            last = now;
            std::vector<std::pair<u64, u64>> sorted;
            for (const auto& [hash, n] : counts) {
                sorted.emplace_back(n, hash);
            }
            std::sort(sorted.rbegin(), sorted.rend());
            std::string line = "SOTC dispatches in 10 s:";
            for (size_t i = 0; i < sorted.size() && i < 12; ++i) {
                line += fmt::format(" {:#x}={}", sorted[i].second, sorted[i].first);
            }
            std::fprintf(stderr, "%s\n", line.c_str());
            counts.clear();
        }
    }
    DispPhase::Mark(DispPhase::Pre);
    if (ExecuteShaderHLE(cs, liverpool->DrawRegs(), cs_program, *this)) {
        fast_bind.valid = false; // sotc: wrote guest memory through the buffer cache
        SotcStats::Drop(5, cs_program.Address<uintptr_t>(), 0);
        DispPhase::Mark(DispPhase::Hle);
        DispPhase::Finish(0, cs.pgm_hash,
                          u64{cs_program.dim_x} * cs_program.dim_y * cs_program.dim_z, 0, 0, 0,
                          false, false, false);
        return;
    }
    DispPhase::Mark(DispPhase::Hle);

    if (!BindResources(pipeline)) {
        SotcStats::Drop(3, cs_program.Address<uintptr_t>(), 0);
        DispPhase::Abort();
        return;
    }

    const bool disp_barrier = needs_barrier;
    if (needs_barrier) {
        runtime.FlushBarriers();
    }
    DispPhase::Mark(DispPhase::Barrier);

    const bool disp_rendering = scheduler.IsRendering();
    scheduler.EndRendering();
    DispPhase::Mark(DispPhase::EndRend);
    pipeline->BindResources(set_writes, push_data);
    DispPhase::Mark(DispPhase::PipeBind);

    const auto cmdbuf = scheduler.CommandBuffer();
    pipeline->BindWhenReady(); // sotc: may still be built by a worker (SOTC_ASYNC_COMPUTE)
    cmdbuf.dispatch(cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
    DebugState.IncDispatch();
    MaybeDumpImages(cs.pgm_hash);
    DispPhase::Mark(DispPhase::Issue);

    ResetBindings(true);
    DispPhase::Mark(DispPhase::PostReset); // sotc: SOTC_DISP_PHASES
    FlushPeriodic();
    DispPhase::Mark(DispPhase::Post);
    if (DispPhase::kOn) { // sotc: SOTC_DISP_PHASES
        u32 bufs, wbufs;
        DispShape(cs, bufs, wbufs);
        DispPhase::Finish(1, cs.pgm_hash,
                          u64{cs_program.dim_x} * cs_program.dim_y * cs_program.dim_z, bufs, wbufs,
                          static_cast<u32>(cs.images.size()), disp_barrier, disp_rendering,
                          cs.uses_dma);
    }
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size, const Pipeline* resolved,
                                  bool use_resolved) {
    RENDERER_TRACE;
    if (SotcStats::kCmdStats) { // sotc: read only by the SOTC_CMD_STATS printer (sotc_stats.h)
        SotcStats::cmd_kinds[5].fetch_add(1, std::memory_order_relaxed);
    }

    DispPhase::Begin(); // sotc: SOTC_DISP_PHASES
    scheduler.PopPendingOperations();

    const auto& cs_program = liverpool->DrawCsRegs();
    const ComputePipeline* pipeline = use_resolved ? static_cast<const ComputePipeline*>(resolved)
                                                   : pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        SotcStats::Drop(2, cs_program.Address<uintptr_t>(), 0);
        DispPhase::Abort();
        return;
    }

    if (SotcOps::kOn) [[unlikely]] { // sotc: SOTC_OP_STATS=1
        SotcOps::NoteDispatch(pipeline->GetStage(Shader::SwStage::Compute).pgm_hash, 0);
    }
    DispPhase::Mark(DispPhase::Pre);
    if (!BindResources(pipeline)) {
        SotcStats::Drop(3, cs_program.Address<uintptr_t>(), 1);
        DispPhase::Abort();
        return;
    }

    const auto [buffer, base] = buffer_cache.ObtainBuffer(address + offset, size, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, size);
    DispPhase::Mark(DispPhase::BbRest);

    const bool disp_barrier = needs_barrier;
    if (needs_barrier) {
        runtime.FlushBarriers();
    }
    DispPhase::Mark(DispPhase::Barrier);

    const bool disp_rendering = scheduler.IsRendering();
    scheduler.EndRendering();
    DispPhase::Mark(DispPhase::EndRend);
    pipeline->BindResources(set_writes, push_data);
    DispPhase::Mark(DispPhase::PipeBind);

    const auto cmdbuf = scheduler.CommandBuffer();
    pipeline->BindWhenReady(); // sotc: may still be built by a worker (SOTC_ASYNC_COMPUTE)
    cmdbuf.dispatchIndirect(buffer->Handle(), base);
    DebugState.IncDispatch();
    runtime.AccessBuffer(buffer, base, size, vk::PipelineStageFlagBits2::eDrawIndirect,
                         vk::AccessFlagBits2::eIndirectCommandRead);
    MaybeDumpImages(pipeline->GetStage(Shader::SwStage::Compute).pgm_hash);
    DispPhase::Mark(DispPhase::Issue);

    ResetBindings(true);
    DispPhase::Mark(DispPhase::PostReset); // sotc: SOTC_DISP_PHASES
    FlushPeriodic();
    DispPhase::Mark(DispPhase::Post);
    if (DispPhase::kOn) { // sotc: SOTC_DISP_PHASES
        const auto& cs = pipeline->GetStage(Shader::SwStage::Compute);
        u32 bufs, wbufs;
        DispShape(cs, bufs, wbufs);
        DispPhase::Finish(2, cs.pgm_hash, 0, bufs, wbufs, static_cast<u32>(cs.images.size()),
                          disp_barrier, disp_rendering, cs.uses_dma);
    }
}

u64 Rasterizer::Flush() {
    const u64 current_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return current_tick;
}

void Rasterizer::Finish() {
    scheduler.Finish();
}

void Rasterizer::OnSubmit() {
    buffer_cache.TickFrame();
    texture_cache.ProcessDownloadImages();
    texture_cache.RunGarbageCollector();
    runtime.TickFrame();
}

void Rasterizer::OnFence() {
    ++VideoCore::g_sotc_fence_epoch;
    texture_cache.ProcessDownloadImages();
}

// sotc: the descriptors take each image's layout when its texture is bound, but BeginRendering may
// move a sampled image to another layout afterwards: a depth buffer both sampled and bound with
// stencil writes goes to DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL while its descriptor still said
// DEPTH_STENCIL_READ_ONLY_OPTIMAL (Vulkan validation; harmless on NVIDIA, wrong on GPUs that
// compress depth by layout). The descriptors are written after this, with the layouts in force.
void Rasterizer::RefreshImageLayouts() {
    // sotc: BbToggle::DrawFixedCostOpt: PeekImage, not GetImage: every image listed was touched
    // (LRU) by its own bind in this draw, and the same tick makes the second touch a no-op.
    const bool peek = !BbToggle::Disabled(BbToggle::DrawFixedCostOpt);
    for (size_t i = 0; i < image_infos.size(); ++i) {
        if (const VideoCore::ImageId id = image_info_images[i]) {
            image_infos[i].imageLayout = (peek ? texture_cache.PeekImage(id)
                                               : texture_cache.GetImage(id))
                                             .backing->state.layout;
        }
    }
}

bool Rasterizer::FastBindEligible(const Pipeline* pipeline) const {
    if (!SotcStats::kFastBind || pipeline->IsCompute() || attachment_feedback_loop) {
        return false;
    }
    const auto& regs = liverpool->DrawRegs();
    if (regs.clipper_control.user_clip_plane_enable != 0) {
        return false;
    }
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        if (stage->has_storage_images || !stage->fmasks.empty()) {
            return false;
        }
        for (const auto& desc : stage->buffers) {
            // The stream-buffer bindings are only reusable while the ring has not wrapped;
            // StreamGeneration() in the fast-bind state checks that. Shared memory is compute.
            if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                return false;
            }
            if (!desc.IsSpecial() && desc.is_written) {
                return false;
            }
        }
        for (const auto& desc : stage->images) {
            if (desc.is_written) {
                return false;
            }
        }
    }
    return true;
}

u64 Rasterizer::FastBindHash(const Pipeline* pipeline, const Shader::PushData& push_data) const {
    const auto& regs = liverpool->DrawRegs();
    u64 hash = XXH3_64bits(&pipeline, sizeof(pipeline));
    hash = XXH3_64bits_withSeed(&push_data, sizeof(push_data), hash);
    for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        const VAddr address = regs.color_buffers[cb].Address();
        hash = XXH3_64bits_withSeed(&address, sizeof(address), hash);
    }
    const VAddr depth_address = regs.depth_buffer.DepthAddress();
    hash = XXH3_64bits_withSeed(&depth_address, sizeof(depth_address), hash);
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        const auto user_data = stage->UserData();
        if (!user_data.empty()) {
            hash = XXH3_64bits_withSeed(user_data.data(), user_data.size_bytes(), hash);
        }
        const auto& flat_buf = stage->FlatBuf();
        if (!flat_buf.empty()) {
            hash = XXH3_64bits_withSeed(flat_buf.data(), flat_buf.size() * sizeof(u32), hash);
        }
    }
    return hash;
}

void Rasterizer::CommitFastBind(const Pipeline* pipeline, u64 hash, bool uses_dma, u64 pre_epoch,
                                u64 pre_clobbers) {
    fast_bind.pipeline = pipeline;
    fast_bind.hash = hash;
    fast_bind.buffer_generation = buffer_cache.BindGeneration();
    fast_bind.texture_generation = texture_cache.BindGeneration();
    fast_bind.stream_generation = buffer_cache.StreamGeneration();
    // sotc: the epoch and clobber count from BEFORE the bind: a write or helper pass that landed
    // during it is not covered by what the bind uploaded or pushed.
    fast_bind.mem_epoch = pre_epoch;
    fast_bind.clobbers = pre_clobbers;
    fast_bind.tick = scheduler.CurrentTick();
    fast_bind.uses_dma = uses_dma;
    fast_bind.lod_marks.assign(lod_marks.begin(), lod_marks.end());
    fast_bind.valid = true;
    static const bool verify_env = std::getenv("SOTC_FAST_BIND_VERIFY") != nullptr;
    if (verify_env) {
        verify_buffers.assign(buffer_infos.begin(), buffer_infos.end());
        verify_images.assign(image_infos.begin(), image_infos.end());
    }
}

// sotc: SOTC_CS_BIND_TRACE state (BindBuffers): the current dispatch prints its bindings.
static bool cs_bind_trace_on = false;

bool Rasterizer::BindResources(const Pipeline* pipeline) {
    cs_bind_trace_on = false;
    if (SotcOps::kOn) [[unlikely]] { // sotc: SOTC_OP_STATS=1, touches of depth targets
        OpStatsScanBindings(pipeline);
    }
    fast_bind_hit = false;
    lod_marks.clear();
    if (IsComputeImageCopy(pipeline) || IsComputeMetaClear(pipeline) ||
        IsComputeImageClear(pipeline)) {
        static const bool drop_trace = std::getenv("SOTC_DROP_TRACE") != nullptr;
        if (drop_trace) {
            static int shown = 0;
            if (shown++ < 12) {
                const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
                for (const auto& desc : info.buffers) {
                    if (desc.IsSpecial()) {
                        continue;
                    }
                    const auto sharp = desc.GetSharp(info);
                    std::fprintf(stderr,
                                 "SOTC drop-detail cs %#llx buf %#llx+%#llx written %d fmt %d "
                                 "meta %d xor %d images %zu\n",
                                 static_cast<unsigned long long>(info.pgm_hash),
                                 static_cast<unsigned long long>(sharp.base_address),
                                 static_cast<unsigned long long>(sharp.GetSize()),
                                 int(desc.is_written), int(desc.is_formatted),
                                 int(texture_cache.IsMeta(sharp.base_address).has_value()),
                                 int(info.has_bitwise_xor), info.images.size());
                }
            }
        }
        fast_bind.valid = false; // sotc: this pass changed images and left barriers pending
        return false;
    }

    // sotc: the buffer tracker's write/read protects of all bindings (and of the DMA sweep) are
    // issued merged when the scope ends, and before any upload copy (see ProtectBatchScope).
    // Off by default (SOTC_PROTECT_BATCH=1): the ranges were almost never adjacent.
    const VideoCore::ProtectBatchScope protect_batch;

    set_write_index = 0;
    set_writes.clear();
    buffer_infos.clear();
    image_infos.clear();

    bool uses_dma = false;
    const u64 pre_epoch = SotcStats::mem_epoch.load(std::memory_order_acquire);
    const u64 pre_clobbers = SotcStats::push_clobbers.load(std::memory_order_relaxed);

    // A stage whose user data was never attached (e.g. a program restored from the pipeline
    // cache that was not refreshed for this draw) would read its SGPRs from a null span.
    for (const auto* stage : pipeline->GetStages()) {
        if (stage && stage->UserData().data() == nullptr &&
            stage->srt_info.flattened_bufsize_dw != 0) {
            // sotc: per draw; throttled (P5), the dropped lines count in the SOTC log line.
            if (SotcStats::LogThrottled("draw-no-userdata")) {
                LOG_ERROR(Render_Vulkan,
                          "Skipping draw: shader {:#x} (sw stage {}, hw stage {}, info {}) has no "
                          "user data bound",
                          stage->pgm_hash, u32(stage->sw_stage), u32(stage->hw_stage),
                          fmt::ptr(stage));
            }
            fast_bind.valid = false;
            return false;
        }
    }

    // sotc: the previous draw left these descriptors and push constants bound. Skip rebuilding
    // them when this draw needs exactly the same pipeline, push data, render targets and
    // resource handles. The generation bumps catch reallocated buffers/images and dead samplers.
    const bool fast_eligible = FastBindEligible(pipeline);
    u64 fast_hash = 0;
    if (fast_eligible) {
        SotcStats::fast_bind_eligible.fetch_add(1, std::memory_order_relaxed);
        const auto input_push = MakeUserData(liverpool->DrawRegs());
        fast_hash = FastBindHash(pipeline, input_push);
        if (fast_bind.valid && fast_bind.pipeline == pipeline &&
            fast_bind.tick == scheduler.CurrentTick() &&
            fast_bind.buffer_generation == buffer_cache.BindGeneration() &&
            fast_bind.texture_generation == texture_cache.BindGeneration() &&
            fast_bind.stream_generation == buffer_cache.StreamGeneration() &&
            fast_bind.mem_epoch == SotcStats::mem_epoch.load(std::memory_order_acquire) &&
            fast_bind.clobbers == SotcStats::push_clobbers.load(std::memory_order_relaxed) &&
            fast_bind.hash == fast_hash) {
            static const bool verify = std::getenv("SOTC_FAST_BIND_VERIFY") != nullptr;
            if (!verify) {
                fast_bind_hit = true;
                SotcStats::fast_bind_hits.fetch_add(1, std::memory_order_relaxed);
                // sotc: the slow path marks every sampled texture bank (SotcLod::Mark counts the
                // uses the game's streaming priority reads); a reused bind marks the same banks.
                for (const u32 bank : fast_bind.lod_marks) {
                    SotcLod::Mark(bank);
                }
                if (fast_bind.uses_dma) {
                    buffer_cache.SynchronizeDmaBuffers();
                }
                return true;
            }
            // sotc: SOTC_FAST_BIND_VERIFY=1 does the full bind anyway and compares it with the
            // descriptors the previous draw left (what a hit would have reused).
            verify_pending = true;
            verify_uploads = SotcStats::thread_data_events;
        }
        push_data = input_push;
    } else {
        fast_bind.valid = false;
        push_data = MakeUserData(liverpool->DrawRegs());
    }
    // Bind resource buffers and textures.
    DrawPhase::Sub(DrawPhase::SBrCheck);
    Shader::Backend::Bindings binding{};
    DrawPhase::Sub(DrawPhase::SBrUser);
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        set_writes.resize(set_writes.size() + stage->buffers.size() + stage->images.size() +
                          stage->samplers.size());
        DrawPhase::Mark(DrawPhase::BindRest);
        DispPhase::Mark(DispPhase::BrPre);
        BindBuffers(*stage, binding, push_data);
        DrawPhase::Mark(DrawPhase::BindBuf);
        DispPhase::Mark(DispPhase::BbRest);
        BindTextures(*stage, binding);
        DrawPhase::Mark(DrawPhase::BindTex);
        DispPhase::Mark(DispPhase::BtTex);
        // sotc: a known shader's raw pointer range is synchronised alone (KnownDmaRange).
        if (stage->uses_dma) {
            VAddr dma_addr = 0;
            u64 dma_size = 0;
            if (!KnownDmaRange(*stage, dma_addr, dma_size) ||
                !buffer_cache.SynchronizeDmaRange(dma_addr, dma_size)) {
                uses_dma = true;
            }
        }
    }

    DispPhase::Mark(DispPhase::BrDma);
    if (uses_dma) {
        const u64 trace_up0 = cs_bind_trace_on ? SotcStats::upload_bytes.load() : 0;
        const u64 trace_pr0 = cs_bind_trace_on ? SotcStats::sys_protects.load() : 0;
        buffer_cache.SynchronizeDmaBuffers();
        if (cs_bind_trace_on) {
            std::fprintf(stderr, "SOTC csbind dma sweep uploaded %llu protects %llu\n",
                         static_cast<unsigned long long>(SotcStats::upload_bytes.load() - trace_up0),
                         static_cast<unsigned long long>(SotcStats::sys_protects.load() - trace_pr0));
        }
    }
    DispPhase::Mark(DispPhase::BrDma);

    if (verify_pending) {
        verify_pending = false;
        bool same = verify_buffers.size() == buffer_infos.size() &&
                    verify_images.size() == image_infos.size();
        for (size_t i = 0; same && i < buffer_infos.size(); ++i) {
            // The stream ring hands out a new offset for identical data (the hash matched).
            const bool stream = buffer_infos[i].buffer == buffer_cache.GetStreamBuffer().Handle();
            same = verify_buffers[i].buffer == buffer_infos[i].buffer &&
                   (stream || verify_buffers[i].offset == buffer_infos[i].offset) &&
                   verify_buffers[i].range == buffer_infos[i].range;
        }
        for (size_t i = 0; same && i < image_infos.size(); ++i) {
            same = verify_images[i].imageView == image_infos[i].imageView &&
                   verify_images[i].sampler == image_infos[i].sampler;
        }
        static u64 hits = 0, bad = 0, content = 0;
        ++hits;
        // A reused bind skips the uploads the full bind did: same handles, new bytes.
        if (same && verify_uploads != SotcStats::thread_data_events) {
            ++content;
        }
        if (!same && bad < 6) {
            for (size_t i = 0; i < buffer_infos.size() && i < verify_buffers.size(); ++i) {
                std::fprintf(stderr, "  buf%zu old %p+%llu/%llu new %p+%llu/%llu%s\n", i,
                             (void*)(uintptr_t)(VkBuffer)verify_buffers[i].buffer,
                             (unsigned long long)verify_buffers[i].offset,
                             (unsigned long long)verify_buffers[i].range,
                             (void*)(uintptr_t)(VkBuffer)buffer_infos[i].buffer,
                             (unsigned long long)buffer_infos[i].offset,
                             (unsigned long long)buffer_infos[i].range,
                             verify_buffers[i].buffer == buffer_infos[i].buffer &&
                                     verify_buffers[i].offset == buffer_infos[i].offset &&
                                     verify_buffers[i].range == buffer_infos[i].range
                                 ? ""
                                 : " DIFF");
            }
            for (size_t i = 0; i < image_infos.size() && i < verify_images.size(); ++i) {
                if (verify_images[i].imageView != image_infos[i].imageView ||
                    verify_images[i].sampler != image_infos[i].sampler) {
                    std::fprintf(stderr, "  img%zu DIFF\n", i);
                }
            }
        }
        if (!same && ++bad <= 20) {
            std::fprintf(stderr, "SOTC FAST_BIND_VERIFY stale: pgm %#llx buffers %zu/%zu images %zu/%zu\n",
                         static_cast<unsigned long long>(pipeline->GetStages()[0]
                                                             ? pipeline->GetStages()[0]->pgm_hash
                                                             : 0),
                         verify_buffers.size(), buffer_infos.size(), verify_images.size(),
                         image_infos.size());
        }
        if (hits % 20000 == 0) {
            std::fprintf(stderr, "SOTC FAST_BIND_VERIFY: %llu would-be hits, %llu stale, %llu with new data\n",
                         static_cast<unsigned long long>(hits), static_cast<unsigned long long>(bad),
                         static_cast<unsigned long long>(content));
        }
    }

    if (fast_eligible) {
        CommitFastBind(pipeline, fast_hash, uses_dma, pre_epoch, pre_clobbers);
    }

    return true;
}

void Rasterizer::BindVertexBuffers(const GraphicsPipeline* pipeline) {
    const auto& regs = liverpool->DrawRegs();
    VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
    VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    VertexInputs<AmdGpu::Buffer> guest_buffers;
    pipeline->GetVertexInputs(attributes, bindings, divisors, guest_buffers,
                              regs.vgt_instance_step_rate_0, regs.vgt_instance_step_rate_1);

    DrawPhase::Sub(DrawPhase::SViInput); // GetVertexInputs
    if (instance.IsVertexInputDynamicState()) {
        // sotc: BbToggle::DrawFixedCostOpt: the same vertex input as the last one set in this
        // command buffer (and no pass bound its own pipeline since: DynamicState::Invalidate)
        // is not recorded again.
        auto& dyn = scheduler.GetDynamicState();
        bool same = !BbToggle::Disabled(BbToggle::DrawFixedCostOpt) && vi_memo.valid &&
                    vi_memo.invalidations == dyn.invalidations &&
                    vi_memo.bindings.size() == bindings.size() &&
                    vi_memo.attributes.size() == attributes.size();
        for (size_t i = 0; same && i < bindings.size(); ++i) {
            const auto& a = vi_memo.bindings[i];
            const auto& b = bindings[i];
            same = a.binding == b.binding && a.stride == b.stride && a.inputRate == b.inputRate &&
                   a.divisor == b.divisor;
        }
        for (size_t i = 0; same && i < attributes.size(); ++i) {
            const auto& a = vi_memo.attributes[i];
            const auto& b = attributes[i];
            same = a.location == b.location && a.binding == b.binding && a.format == b.format &&
                   a.offset == b.offset;
        }
        if (!same) {
            // Update current vertex inputs.
            const auto cmdbuf = scheduler.CommandBuffer();
            cmdbuf.setVertexInputEXT(bindings, attributes);
            vi_memo.valid = true;
            vi_memo.invalidations = dyn.invalidations;
            vi_memo.bindings = bindings;
            vi_memo.attributes = attributes;
        }
    }

    DrawPhase::Sub(DrawPhase::SViInput);
    if (bindings.empty()) {
        // If there are no bindings, there is nothing further to do.
        return;
    }

    struct BufferRange {
        VAddr base_address;
        VAddr end_address;
        const VideoCore::Buffer* buffer;
        u64 offset;

        [[nodiscard]] size_t GetSize() const {
            return end_address - base_address;
        }
    };

    // Build list of ranges covering the requested buffers
    VertexInputs<BufferRange> ranges{};
    for (const auto& buffer : guest_buffers) {
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            ranges.emplace_back(buffer.base_address, buffer.base_address + buffer.GetSize());
        }
    }

    // Merge connecting ranges together
    VertexInputs<BufferRange> ranges_merged{};
    if (!ranges.empty()) {
        std::ranges::sort(ranges, [](const BufferRange& lhv, const BufferRange& rhv) {
            return lhv.base_address < rhv.base_address;
        });
        ranges_merged.emplace_back(ranges[0]);
        for (auto range : ranges) {
            auto& prev_range = ranges_merged.back();
            if (prev_range.end_address < range.base_address) {
                ranges_merged.emplace_back(range);
            } else {
                prev_range.end_address = std::max(prev_range.end_address, range.end_address);
            }
        }
    }

    // Map buffers for merged ranges
    for (auto& range : ranges_merged) {
        u64 size = memory->ClampRangeSize(range.base_address, range.GetSize());
        if (size == 0) {
            // Start is not mapped (garbage vertex sharp): keep a small range instead of GBs.
            size = std::min<u64>(range.GetSize(), 4096);
        }
        std::tie(range.buffer, range.offset) =
            buffer_cache.ObtainBuffer(range.base_address, size, false);
        needs_barrier |= runtime.IsBufferAccessed(range.buffer, range.offset, size);
        fixed_function_reads.push_back({range.buffer, range.offset, static_cast<u32>(size), false});
    }

    DrawPhase::Sub(DrawPhase::SViObt);
    // Bind vertex buffers
    VertexInputs<vk::Buffer> host_buffers;
    VertexInputs<vk::DeviceSize> host_offsets;
    VertexInputs<vk::DeviceSize> host_sizes;
    VertexInputs<vk::DeviceSize> host_strides;
    for (const auto& buffer : guest_buffers) {
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            const auto host_buffer_info =
                std::ranges::find_if(ranges_merged, [&](const BufferRange& range) {
                    return buffer.base_address >= range.base_address &&
                           buffer.base_address < range.end_address;
                });
            ASSERT(host_buffer_info != ranges_merged.cend());
            host_buffers.emplace_back(host_buffer_info->buffer->Handle());
            host_offsets.push_back(host_buffer_info->offset + buffer.base_address -
                                   host_buffer_info->base_address);
        } else {
            host_buffers.emplace_back(VK_NULL_HANDLE);
            host_offsets.push_back(0);
        }
        host_sizes.push_back(buffer.GetSize());
        host_strides.push_back(buffer.GetStride());
    }

    if (SotcOps::kOn) [[unlikely]] { // sotc: SOTC_OP_STATS=1
        u64 vd = 0x7662, ve = 0x7662;
        for (size_t i = 0; i < guest_buffers.size(); ++i) {
            vd = SotcOps::Mix(vd, std::bit_cast<u64>(host_buffers[i]));
            vd = SotcOps::Mix(vd, host_strides[i]);
            ve = SotcOps::Mix(ve, guest_buffers[i].base_address);
            ve = SotcOps::Mix(ve, (host_sizes[i] << 16) ^ host_strides[i]);
        }
        SotcOps::vb_desc = vd;
        SotcOps::vb_exact = ve;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    const auto num_buffers = guest_buffers.size();
    if (instance.IsVertexInputDynamicState()) {
        cmdbuf.bindVertexBuffers(0, num_buffers, host_buffers.data(), host_offsets.data());
    } else {
        cmdbuf.bindVertexBuffers2(0, num_buffers, host_buffers.data(), host_offsets.data(),
                                  host_sizes.data(), host_strides.data());
    }
    DrawPhase::Sub(DrawPhase::SViBind);
}

void Rasterizer::BindIndexBuffer(u32 index_offset) {
    const auto& regs = liverpool->DrawRegs();

    // Figure out index type and size.
    const bool is_index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    const vk::IndexType index_type = is_index16 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
    const VAddr index_address =
        regs.index_base_address.Address<VAddr>() + index_offset * index_size;

    // Bind index buffer.
    const u32 index_buffer_size = regs.num_indices * index_size;
    const auto [buffer, offset] =
        buffer_cache.ObtainBuffer(index_address, index_buffer_size, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, offset, index_buffer_size);
    fixed_function_reads.push_back({buffer, offset, index_buffer_size, false});
    if (SotcOps::kOn) [[unlikely]] { // sotc: SOTC_OP_STATS=1
        SotcOps::ib_desc = SotcOps::Mix(std::bit_cast<u64>(buffer->Handle()), u64{index_size});
        SotcOps::ib_exact = SotcOps::Mix(index_address, u64{index_size});
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindIndexBuffer(buffer->Handle(), offset, index_type);
    DrawPhase::Sub(DrawPhase::SIbuf);
}

void Rasterizer::FlushPeriodic() {
    // Readbacks drain the GPU, but between them the GPU only gets work when a readback submits:
    // it sits idle while the command processor records. Submitting every N draws/dispatches lets
    // it run the frame during the recording. A submit inside a render pass cuts it, so wait for a
    // command outside one, up to 4 * N. From periodic_flush_commands of Pink-shadPS4
    // (luizgustavs). SOTC_FLUSH_EVERY=N sets it, 0 = off.
    static const u32 every = [this] {
        if (const char* v = std::getenv("SOTC_FLUSH_EVERY")) {
            return static_cast<u32>(std::strtoul(v, nullptr, 10));
        }
        // An explicit SOTC_FLUSH_US turns the periodic submit on for any GPU (the launcher's
        // option for AMD and Intel cards, not tested there).
        if (const char* v = std::getenv("SOTC_FLUSH_US"); v && std::strtoull(v, nullptr, 10) != 0) {
            return 256u;
        }
        // SotC: +20% fps in the open world (14-15 -> 18). Other games keep the upstream behavior.
        // Pink-shadPS4 advises turning it off on AMD (black screen); with readbacks ahead on a
        // Radeon RX 6650 XT the image is right and the sanctuary goes from 17.4 to 22.4 fps.
        constexpr u32 VendorNvidia = 0x10DE;
        constexpr u32 VendorAmd = 0x1002;
        return Common::ElfInfo::Instance().IsSotc() &&
                       (instance.GetVendorID() == VendorNvidia ||
                        instance.GetVendorID() == VendorAmd)
                   ? 256u
                   : 0u;
    }();
    // sotc: the command buffer wrote a page the guest reads back: submit it now, the GPU runs
    // it while the guest gets there, and the readback finds it done (or nearly). Within noise
    // when the command processor was slower; on by default since 2026-10-16 (SOTC_RB_EARLY=0
    // off): attract demo 53.6/54.5 -> 55.3/55.7 FPS (open world: 400-490 readbacks/s waited
    // ~10% of the thread), Shrine unchanged (34.2 vs 34.1).
    if (SotcStats::kFpsLog) { // sotc: read only by the SOTC_FPS printer (sotc_stats.h)
        // sotc: written by this thread only: a plain add, no lock prefix (an atomic RMW here
        // waits for the thread's pending stores, ~1.8% of it). The printer takes deltas.
        SotcStats::commands.store(SotcStats::commands.load(std::memory_order_relaxed) + 1,
                                  std::memory_order_relaxed);
    }
    static const bool early = [] {
        const char* v = std::getenv("SOTC_RB_EARLY");
        return !v || v[0] != '0';
    }();
    if (early && !scheduler.IsRendering() && buffer_cache.TakeHotWrite()) {
        SotcStats::early_flushes.fetch_add(1, std::memory_order_relaxed);
        scheduler.Flush();
        return;
    }
    if (every == 0) {
        // sotc: without periodic submits, look for cold arena memory every 1024 commands.
        if ((++evict_poll & 1023) == 0 && !scheduler.IsRendering()) {
            buffer_cache.MaybeEvictCold(std::chrono::steady_clock::now());
        }
        return;
    }
    // sotc: submit by recording time instead (SOTC_FLUSH_US, default 1000 where the periodic
    // submit is on and SOTC_FLUSH_EVERY is not set): the GPU gets work at a steady pace whether
    // a scene has few heavy commands or thousands of light ones. Attract demo: +10-15% in most
    // scenes, canyon bridge +2% (count based: 64 helped the first and hurt the second).
    // 1000 instead of 500 (2026-10-15, RTX 3060 laptop, idle shrine ~5800 draws/frame): about +8%
    // in a single unpaired run (250: no gain, 2000: same as 1000): fewer submits, each one
    // cheaper per draw.
    static const u64 flush_us = [] {
        if (const char* v = std::getenv("SOTC_FLUSH_US")) {
            return std::strtoull(v, nullptr, 10);
        }
        return std::getenv("SOTC_FLUSH_EVERY") ? u64{0} : u64{1000};
    }();
    const u64 tick = scheduler.CurrentTick();
    if (tick != commands_tick) {
        commands_tick = tick;
        commands_since_submit = 0;
        commands_since = std::chrono::steady_clock::now();
    }
    ++commands_since_submit;
    if (flush_us != 0) {
        // sotc: the clock is read every 8th command (~40 us at the Shrine against 1000 us),
        // not on every one (~0.5% of the command processor in QueryPerformanceCounter).
        if ((commands_since_submit & 7) != 0) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = now - commands_since;
        const auto limit = std::chrono::microseconds(flush_us);
        if (elapsed < limit || (scheduler.IsRendering() && elapsed < limit * 4)) {
            return;
        }
        // sotc: cold arena memory goes back at a submit point outside a render pass.
        if (!scheduler.IsRendering() && buffer_cache.MaybeEvictCold(now)) {
            return;
        }
        scheduler.Flush();
        return;
    }
    if (commands_since_submit < every ||
        (scheduler.IsRendering() && commands_since_submit < u64{every} * 4)) {
        return;
    }
    if (!scheduler.IsRendering() &&
        buffer_cache.MaybeEvictCold(std::chrono::steady_clock::now())) {
        return;
    }
    scheduler.Flush();
}

// SOTC_DUMP="hash,hash,..." (hex, default: the SotC volumetric fog passes): after F12 (without
// RenderDoc), the next dispatch of each listed compute shader dumps every image it has bound to
// user/log/dump_N/ (raw mip 0, all layers and slices). The data is what the GPU really computed,
// unlike a RenderDoc replay of a frame that uses the sparse guest-memory buffer.
void Rasterizer::MaybeDumpImages(u64 pgm_hash) {
    // sotc: SOTC_CS_DUMP="hash,hash": after those compute shaders, read the buffer armed by
    // SOTC_CS_WATCH back from the GPU and print its first 24 dwords (first 60 times; slow).
    static const std::vector<u64> dump_after = [] {
        std::vector<u64> list;
        if (const char* env = std::getenv("SOTC_CS_DUMP")) {
            for (const char* p = env; *p;) {
                char* end = nullptr;
                const u64 v = std::strtoull(p, &end, 0);
                if (end == p) {
                    break;
                }
                list.push_back(v);
                p = *end == ',' ? end + 1 : end;
            }
        }
        return list;
    }();
    if (!dump_after.empty() &&
        std::find(dump_after.begin(), dump_after.end(), pgm_hash) != dump_after.end()) {
        static int shown = 0;
        const VAddr watch = SotcStats::watch_addr.load(std::memory_order_relaxed);
        if (watch && shown++ < 60) {
            buffer_cache.ReadMemory(watch, 24 * sizeof(u32));
            u32 v[24]{};
            memory->TryReadBacking(watch, v, sizeof(v));
            std::fprintf(stderr, "SOTC dump after %#llx:", static_cast<unsigned long long>(pgm_hash));
            for (const u32 x : v) {
                std::fprintf(stderr, " %u", x);
            }
            std::fprintf(stderr, "\n");
        }
    }
    static const std::vector<u64> targets = [] {
        std::vector<u64> list;
        const char* env = std::getenv("SOTC_DUMP");
        if (!env) {
            return list;
        }
        std::string spec = env;
        if (spec.empty() || spec == "1") {
            spec = "b33e9db6,91c6c95e,25338711,47c1dc64";
        }
        size_t pos = 0;
        while (pos < spec.size()) {
            const size_t end = std::min(spec.find(',', pos), spec.size());
            list.push_back(std::strtoull(spec.substr(pos, end - pos).c_str(), nullptr, 16));
            pos = end + 1;
        }
        return list;
    }();
    static std::vector<u64> pending;
    static std::filesystem::path dir;
    static u32 session = 0;
    static u32 seq = 0;
    if (targets.empty()) {
        return;
    }
    if (VideoCore::ConsumeImageDumpRequest()) {
        pending = targets;
        dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / fmt::format("dump_{}", ++session);
        std::filesystem::create_directories(dir);
        seq = 0;
    }
    const auto it = std::find(pending.begin(), pending.end(), pgm_hash);
    if (it == pending.end()) {
        return;
    }
    pending.erase(it);
    std::vector<VideoCore::ImageId> seen;
    u32 index = 0;
    for (const auto image_id : bound_images) {
        if (!image_id || std::find(seen.begin(), seen.end(), image_id) != seen.end()) {
            continue;
        }
        seen.push_back(image_id);
        texture_cache.DumpImage(image_id, dir / fmt::format("{:02}_cs_{:08x}_img{}", seq, pgm_hash,
                                                             index++));
    }
    ++seq;
}

void Rasterizer::ResetBindings(bool is_compute) {
    if (!BbToggle::Disabled(BbToggle::DrawFixedCostOpt)) {
        // sotc: PeekImage as in RefreshImageLayouts: bound_images were all touched by this draw.
        for (auto& image_id : bound_images) {
            texture_cache.PeekImage(image_id).binding = {};
        }
    } else {
        for (auto& image_id : bound_images) {
            texture_cache.GetImage(image_id).binding = {};
        }
    }
    for (const auto [buffer, offset, size, is_written] : bound_buffers) {
        const auto dst_stage = is_compute ? vk::PipelineStageFlagBits2::eComputeShader
                                          : vk::PipelineStageFlagBits2::eAllGraphics;
        const auto write_flag =
            is_written ? vk::AccessFlagBits2::eShaderWrite : vk::AccessFlagBits2::eNone;
        runtime.AccessBuffer(buffer, offset, size, dst_stage,
                             vk::AccessFlagBits2::eShaderRead | write_flag);
    }
    for (const auto& read : fixed_function_reads) {
        runtime.AccessBuffer(read.buffer, read.offset, read.size,
                             vk::PipelineStageFlagBits2::eVertexInput,
                             vk::AccessFlagBits2::eVertexAttributeRead |
                                 vk::AccessFlagBits2::eIndexRead);
    }
    fixed_function_reads.clear();
    bound_images.clear();
    bound_buffers.clear();
    needs_barrier = false;
}

bool Rasterizer::IsComputeMetaClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }
    // sotc: SOTC_META_CLEAR=0 runs every dispatch the heuristic below would skip (test: a
    // buffer reused from an old image's metadata range makes a real dispatch look like a clear).
    static const bool meta_clear = [] {
        const char* v = std::getenv("SOTC_META_CLEAR");
        return !v || v[0] != '0';
    }();
    if (!meta_clear) {
        return false;
    }

    // Most of the time when a metadata is updated with a shader it gets cleared. It means
    // we can skip the whole dispatch and update the tracked state instead. Also, it is not
    // intended to be consumed and in such rare cases (e.g. HTile introspection, CRAA) we
    // will need its full emulation anyways.
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);

    // Assume if a shader reads metadata, it is a copy shader.
    for (const auto& desc : info.buffers) {
        const VAddr address = desc.GetSharp(info).base_address;
        if (!desc.IsSpecial() && !desc.is_written && texture_cache.IsMeta(address)) {
            return false;
        }
    }

    // Metadata surfaces are tiled and thus need address calculation to be written properly.
    // If a shader wants to encode HTILE, for example, from a depth image it will have to compute
    // proper tile address from dispatch invocation id. This address calculation contains an xor
    // operation so use it as a heuristic for metadata writes that are probably not clears.
    if (!info.has_bitwise_xor) {
        // Assume if a shader writes metadata without address calculation, it is a clear shader.
        for (const auto& desc : info.buffers) {
            const VAddr address = desc.GetSharp(info).base_address;
            if (!desc.IsSpecial() && desc.is_written && texture_cache.ClearMeta(address)) {
                // Assume all slices were updates
                LOG_TRACE(Render_Vulkan, "Metadata update skipped");
                return true;
            }
        }
    }
    return false;
}

bool Rasterizer::IsComputeImageCopy(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->DrawCsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // Those 2 buffers must both be formatted. One must be source and another destination.
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (!desc0.is_formatted || !desc1.is_formatted || desc0.is_written == desc1.is_written) {
        return false;
    }

    // Buffers must have the same size and each thread of the dispatch must copy 1 dword of data
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    if (buf0.GetSize() != buf1.GetSize() || cs_pgm.dim_x != (buf0.GetSize() / 256)) {
        return false;
    }

    // Find images the buffer alias
    const auto image0_id = texture_cache.FindImageFromRange(buf0.base_address, buf0.GetSize());
    if (!image0_id) {
        return false;
    }
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image copy must be valid
    VideoCore::Image& image0 = texture_cache.GetImage(image0_id);
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image0.info.guest_size != image1.info.guest_size ||
        image0.info.pitch != image1.info.pitch || image0.info.guest_size != buf0.GetSize() ||
        image0.info.num_bits != image1.info.num_bits) {
        return false;
    }

    // Perform image copy
    VideoCore::Image& src_image = desc0.is_written ? image1 : image0;
    VideoCore::Image& dst_image = desc0.is_written ? image0 : image1;
    // sotc: the destination was found by address, not by a writer lookup: its first GPU write
    // moves the mip-tail epoch (CopyColorAndDepth only passes the flag on).
    VideoCore::MarkGpuModified(dst_image);
    runtime.CopyColorAndDepth(&src_image, &dst_image);
    return true;
}

bool Rasterizer::IsComputeImageClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->DrawCsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // From those 2 buffers, first must hold the clear vector and second the image being cleared
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (desc0.is_formatted || !desc1.is_formatted || desc0.is_written || !desc1.is_written) {
        return false;
    }

    // First buffer must have size of vec4 and second the size of a single layer
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    const u32 buf1_bpp = AmdGpu::NumBitsPerBlock(buf1.GetDataFmt());
    if (buf0.GetSize() != 16 || (cs_pgm.dim_x * 128ULL * (buf1_bpp / 8)) != buf1.GetSize()) {
        return false;
    }

    // Find image the buffer alias
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image clear must be valid
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image1.info.guest_size != buf1.GetSize() || image1.info.num_bits != buf1_bpp ||
        image1.info.props.is_depth) {
        return false;
    }

    // Perform image clear
    const float* values = reinterpret_cast<float*>(buf0.base_address);
    const vk::ClearValue clear = {
        .color = {.float32 = std::array<float, 4>{values[0], values[1], values[2], values[3]}},
    };
    const VideoCore::SubresourceRange range = {
        .base =
            {
                .level = 0,
                .layer = 0,
            },
        .extent = image1.info.resources,
    };
    runtime.ClearImage(&image1, range, clear);
    return true;
}

// sotc: SOTC_CS_BIND_TRACE (see BindBuffers): whether the current dispatch of the traced shader
// prints its bindings (decided at the start of its BindBuffers).
static bool CsBindTraceMatch(u64 pgm_hash) {
    // SOTC_CS_BIND_TRACE=<hash>[,<hash>...]
    static const std::vector<u64> hashes = [] {
        std::vector<u64> list;
        for (const char* p = std::getenv("SOTC_CS_BIND_TRACE"); p && *p;) {
            char* end = nullptr;
            const u64 v = std::strtoull(p, &end, 0);
            if (end == p) {
                break;
            }
            list.push_back(v);
            p = *end == ',' ? end + 1 : end;
        }
        return list;
    }();
    return !hashes.empty() && std::find(hashes.begin(), hashes.end(), pgm_hash) != hashes.end();
}
static bool CsBindTraceActive(u64 pgm_hash) {
    return cs_bind_trace_on && CsBindTraceMatch(pgm_hash);
}

// sotc: the guest VMA table generation (src/runtime_memory.c), part of the BufferBindMemo key.
extern "C" uint64_t runtime_memory_generation(void);

// sotc: BbToggle::DispatchFastPath. memory->ClampRangeSize reads the guest VMA table through a
// per-thread region cache (a 2048-slot table and a binary search on a miss, ~35 ns, 8,000 bindings a
// frame in the heavy temple). Its answer is a function of the table alone, and
// runtime_memory_generation() is even and unchanged exactly while the table is: the answer is
// remembered per (address, size) and generation. SOTC_DISPFAST_VERIFY=1 recomputes every hit.
u64 Rasterizer::ClampBindingSize(VAddr base, u64 size) {
    if (BbToggle::Disabled(BbToggle::DispatchFastPath)) {
        return memory->ClampRangeSize(base, size);
    }
    static const bool verify = [] {
        const char* v = std::getenv("SOTC_DISPFAST_VERIFY");
        return v && v[0] == '1';
    }();
    const u64 generation = runtime_memory_generation();
    if (generation & 1) {
        return memory->ClampRangeSize(base, size);
    }
    const u64 hash = (base * 0x9E3779B97F4A7C15ULL) ^ (size * 0xC2B2AE3D27D4EB4FULL);
    ClampMemo& m = clamp_memos[(hash ^ (hash >> 32)) % NumClampMemos];
    if (m.generation == generation && m.base == base && m.size == size) {
        if (verify) {
            static u64 checked = 0, bad = 0;
            const u64 fresh = memory->ClampRangeSize(base, size);
            if (runtime_memory_generation() == generation) {
                ++checked;
                if (fresh != m.result && ++bad <= 10) {
                    std::fprintf(stderr, "SOTC DISPFAST VERIFY: clamp mismatch at %#llx+%#llx memo %#llx now %#llx\n",
                                 static_cast<unsigned long long>(base), static_cast<unsigned long long>(size),
                                 static_cast<unsigned long long>(m.result), static_cast<unsigned long long>(fresh));
                }
                if ((checked & 0xFFFFF) == 0) {
                    std::fprintf(stderr, "SOTC DISPFAST VERIFY: %llu clamp hits checked, %llu mismatches\n",
                                 static_cast<unsigned long long>(checked), static_cast<unsigned long long>(bad));
                }
            }
        }
        return m.result;
    }
    const u64 result = memory->ClampRangeSize(base, size);
    if (runtime_memory_generation() == generation) {
        m = {base, size, generation, result};
    }
    return result;
}

void Rasterizer::BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                             Shader::PushData& push_data) {
    const u64 alignment = instance.StorageMinAlignment();
    if (CsBindTraceMatch(stage.pgm_hash)) {
        // New dispatch of the traced shader: pick whether this one prints.
        static u64 dispatches = 0, shown = 0;
        const u64 n = dispatches++;
        cs_bind_trace_on = (n < 6 || (n % 13) == 0) && shown++ < 60;
        if (cs_bind_trace_on) {
            const auto& flat = stage.FlatBuf();
            std::fprintf(stderr, "SOTC csbind dispatch %llu uses_dma %d flat[18..19] %#x %#x\n",
                         static_cast<unsigned long long>(n), int(stage.uses_dma),
                         flat.size() > 18 ? flat[18] : 0, flat.size() > 19 ? flat[19] : 0);
        }
    }
    for (const auto& desc : stage.buffers) {
        if (desc.IsSpecial()) {
            if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
                const auto* gds_buf = buffer_cache.GetGdsBuffer();
                buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
                needs_barrier |=
                    runtime.IsBufferAccessed(gds_buf, 0, gds_buf->SizeBytes(), desc.is_written);
                bound_buffers.emplace_back(gds_buf, 0, gds_buf->SizeBytes(), desc.is_written);
            } else if (desc.buffer_type == Shader::BufferType::Flatbuf) {
                auto& vk_buffer = buffer_cache.GetStreamBuffer();
                const u32 ubo_size = stage.FlatBuf().size() * sizeof(u32);
                const u64 offset =
                    vk_buffer.Copy(stage.FlatBuf().data(), ubo_size, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
            } else if (desc.buffer_type == Shader::BufferType::ClipPlanes) {
                // Permutations compiled without enabled planes never read the buffer, so the
                // declared binding is satisfied with a null descriptor instead of a copy.
                if (liverpool->DrawRegs().clipper_control.user_clip_plane_enable == 0) {
                    buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
                } else {
                    auto& vk_buffer = buffer_cache.GetStreamBuffer();
                    std::array<float, AmdGpu::NUM_CLIP_PLANES * 4> planes{};
                    for (u32 i = 0; i < AmdGpu::NUM_CLIP_PLANES; ++i) {
                        const auto& plane = liverpool->DrawRegs().clip_user_data[i];
                        planes[i * 4 + 0] = std::bit_cast<float>(plane.data_x);
                        planes[i * 4 + 1] = std::bit_cast<float>(plane.data_y);
                        planes[i * 4 + 2] = std::bit_cast<float>(plane.data_z);
                        planes[i * 4 + 3] = std::bit_cast<float>(plane.data_w);
                    }
                    const u32 ubo_size = static_cast<u32>(sizeof(planes));
                    const u64 offset = vk_buffer.Copy(planes.data(), ubo_size, alignment);
                    buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
                }
            } else if (desc.buffer_type == Shader::BufferType::BdaPagetable) {
                const auto* bda_buffer = buffer_cache.GetBdaPageTableBuffer();
                buffer_infos.emplace_back(bda_buffer->Handle(), 0, bda_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::FaultBuffer) {
                const auto* fault_buffer = buffer_cache.GetFaultBuffer();
                buffer_infos.emplace_back(fault_buffer->Handle(), 0, fault_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                auto& lds_buffer = buffer_cache.GetStreamBuffer();
                const auto& cs_program = liverpool->DrawCsRegs();
                const auto lds_size = cs_program.SharedMemSize() * cs_program.NumWorkgroups();
                const auto [data, offset] = lds_buffer.Map(lds_size, alignment);
                std::memset(data, 0, lds_size);
                lds_buffer.Commit();
                buffer_infos.emplace_back(lds_buffer.Handle(), offset, lds_size);
            } else {
                UNREACHABLE_MSG("Unexpected buffer type {}", u32(desc.buffer_type));
            }
            DispPhase::Mark(DispPhase::BbFlat);
            DrawPhase::Sub(DrawPhase::SBbFlat);
        } else {
            const auto vsharp = desc.GetSharp(stage);
            // Unmapped addresses show up in descriptors of bindings the shader does not use
            // (garbage left in the guest's descriptor memory); bind nothing instead of aborting.
            // Large ranges get clamped to the mapped memory at their start (0 if unmapped);
            // written bindings are checked as well, so garbage cannot become GPU-modified memory.
            // sotc: the clamp alone is 0 exactly when the base is unmapped or reserved (what the
            // two is-mapped calls checked), so one runtime call per binding instead of three.
            // SOTC_BIND_CLAMP1=0 restores the three calls.
            static const bool clamp1 = [] {
                const char* v = std::getenv("SOTC_BIND_CLAMP1");
                return !v || v[0] != '0';
            }();
            // sotc: BbToggle::BufferBindMemo, opt-in with SOTC_BUFFER_MEMO=1 (then bit 56 of the
            // toggle mask switches it off live). Measured 2026-10-22: hit rate 92-97% (attract) and
            // 80% (open field) but no gain (ns per binding and FPS equal within noise), so it is
            // off by default; see docs/sotc/LOTE-2-RESULTADOS.md. A plain
            // read-only binding (no hint, no 4 GB-style unbounded size) is a function of its V#
            // (bytes), the guest VMA table (the clamp) and what ObtainBuffer relies on (the token
            // of BufferCache::BindToken), so a binding of an equal V# under the same VMA table
            // generation and a valid token repeats the remembered result: no clamp, no window
            // logic, no ObtainBuffer. Everything per draw (descriptor, offsets, barrier check)
            // below still runs.
            static const bool buffer_memo_env = [] {
                const char* v = std::getenv("SOTC_BUFFER_MEMO");
                return v && v[0] == '1';
            }();
            BufferBindMemo* bmemo = nullptr;
            u64 bmemo_words[2]{};
            u64 bmemo_generation = 0;
            bool memo_hit = false;
            if (buffer_memo_env && clamp1 && !desc.is_written && !desc.is_formatted &&
                !cs_bind_trace_on && vsharp.base_address != 0 && vsharp.GetSize() != 0 &&
                vsharp.GetSize() < 0xFFFF0000ULL && !BbToggle::Disabled(BbToggle::BufferBindMemo)) {
                std::memcpy(bmemo_words, &vsharp, sizeof(bmemo_words));
                bmemo_generation = runtime_memory_generation();
                const u64 hash = (bmemo_words[0] * 0x9E3779B97F4A7C15ULL) ^
                                 (bmemo_words[1] * 0xC2B2AE3D27D4EB4FULL);
                bmemo = &buffer_bind_memo[(hash ^ (hash >> 29)) % NumBufferBindMemos];
                memo_hit = bmemo->words[0] == bmemo_words[0] && bmemo->words[1] == bmemo_words[1] &&
                           bmemo->generation == bmemo_generation && !(bmemo_generation & 1) &&
                           buffer_cache.BindTokenValid(bmemo->token);
                if (SotcStats::kCmdStats) {
                    auto& counter = memo_hit ? SotcStats::buffer_memo_hits
                                             : SotcStats::buffer_memo_misses;
                    counter.store(counter.load(std::memory_order_relaxed) + 1,
                                  std::memory_order_relaxed); // draw thread only: no lock xadd
                }
            }
            u64 size = 0;
            if (memo_hit) {
                size = bmemo->size;
            } else if (clamp1) {
                if (vsharp.base_address != 0 && vsharp.GetSize() != 0) {
                    size = ClampBindingSize(vsharp.base_address, vsharp.GetSize());
                }
            } else if (vsharp.base_address != 0 && vsharp.GetSize() != 0 &&
                       memory->IsValidMapping(vsharp.base_address) &&
                       (!desc.is_written || memory->IsMappedAddress(vsharp.base_address))) {
                size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
            }
            DispPhase::Mark(DispPhase::BbClamp);
            DrawPhase::Sub(DrawPhase::SBbClamp);
            if (memo_hit) {
                const VideoCore::Buffer* const buffer = bmemo->buffer;
                const u64 offset = bmemo->offset;
                const u64 offset_aligned = Common::AlignDown(offset, alignment);
                const u64 adjust = offset - offset_aligned;
                push_data.AddOffset(binding.buffer, adjust);
                buffer_infos.emplace_back(buffer->Handle(), offset_aligned, size + adjust);
                bound_buffers.emplace_back(buffer, offset, size, false);
                needs_barrier |= runtime.IsBufferAccessed(buffer, offset, size, false);
            } else if (size == 0) {
                buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
            } else {
                // sotc: known shaders that read a few KiB of an unbounded descriptor (see
                // BufferExtentHint) bind just that, not the 64 MiB window below.
                if (!desc.is_written && vsharp.GetSize() >= 0xFFFF0000ULL) {
                    const u64 hint = BufferExtentHint(
                        stage, static_cast<u32>(&desc - stage.buffers.data()), vsharp.GetStride());
                    if (hint != 0 && hint < size) {
                        size = hint;
                    }
                }
                // Guest code commonly marks a buffer as unbounded with num_records near 4GB and
                // only touches a small part of it, and descriptors of unused bindings can hold
                // garbage sizes of several GB. Backing such ranges makes the buffer cache keep
                // them resident until video memory runs out, so bindings above a sane size are
                // limited to a window from their base address.
                static const u64 unbounded_window = [] {
                    const char* v = std::getenv("SHADPS4_UNBOUNDED_BUFFER_MB");
                    return (v ? std::strtoull(v, nullptr, 10) : 64ULL) << 20;
                }();
                constexpr u64 MaxSaneBindingSize = 256ULL << 20;
                // Written bindings keep their full range: a shader writing past a shortened
                // binding would hit sparse pages that were never made resident.
                if (!desc.is_written &&
                    (vsharp.GetSize() >= 0xFFFF0000ULL || size > MaxSaneBindingSize) &&
                    unbounded_window != 0 && size > unbounded_window) {
                    static std::atomic<u32> reported{0};
                    if (vsharp.GetSize() < 0xFFFF0000ULL && reported++ < 16) {
                        LOG_WARNING(Render,
                                    "Limiting {} MB binding at {:#x} of shader {:#x} to {} MB",
                                    size >> 20, vsharp.base_address, stage.pgm_hash,
                                    unbounded_window >> 20);
                    }
                    size = unbounded_window;
                }
                if (size != vsharp.GetSize()) {
                    // sotc: the clamp is common (unbounded and leftover descriptors), so the logger
                    // map lookup behind LOG_DEBUG was paid per binding. Report the first few.
                    static std::atomic<u32> clamped_shown{0};
                    const u32 shown = clamped_shown.load(std::memory_order_relaxed);
                    if (shown < 8) {
                        clamped_shown.store(shown + 1, std::memory_order_relaxed);
                        LOG_DEBUG(Render, "Clamped size from {} to {} for stage {:#x}",
                                  vsharp.GetSize(), size, stage.pgm_hash);
                    }
                }
                // sotc: SOTC_CS_BIND_TRACE=<hash>: per binding of that compute shader (first 3
                // dispatches, then 1 in 16, 60 shown), the descriptor and what its ObtainBuffer
                // uploaded (needs SOTC_CMD_STATS=1 for the counters).
                const bool bind_trace = CsBindTraceActive(stage.pgm_hash);
                const u64 trace_up0 = bind_trace ? SotcStats::upload_bytes.load() : 0;
                const u64 trace_pr0 = bind_trace ? SotcStats::sys_protects.load() : 0;
                VideoCore::BufferCache::BindToken token;
                const auto [buffer, offset] = buffer_cache.ObtainBuffer(
                    vsharp.base_address, size, desc.is_written, desc.is_formatted,
                    bmemo ? &token : nullptr);
                DispPhase::Mark(desc.is_written ? DispPhase::BbObtW : DispPhase::BbObtR);
                DrawPhase::Sub(DrawPhase::SBbObt);
                if (bmemo && token.valid) {
                    // The V# is the key: size was derived from it (and the VMA generation read
                    // before the clamp, so a table change meanwhile makes the entry miss).
                    *bmemo = {{bmemo_words[0], bmemo_words[1]}, bmemo_generation, buffer, offset,
                              size, token};
                }
                if (bind_trace) {
                    u32 first[4]{};
                    memory->TryReadBacking(vsharp.base_address, first, sizeof(first));
                    std::fprintf(stderr,
                                 "SOTC csbind %#llx buf%u base %#llx num %#llx stride %u bound "
                                 "%#llx written %d gpu %d uploaded %llu protects %llu mem %u %u "
                                 "%u %u\n",
                                 static_cast<unsigned long long>(stage.pgm_hash),
                                 static_cast<u32>(&desc - stage.buffers.data()),
                                 static_cast<unsigned long long>(vsharp.base_address),
                                 static_cast<unsigned long long>(vsharp.GetSize()),
                                 vsharp.GetStride(), static_cast<unsigned long long>(size),
                                 int(desc.is_written),
                                 int(buffer_cache.IsRegionGpuModified(vsharp.base_address, 32)),
                                 static_cast<unsigned long long>(SotcStats::upload_bytes.load() -
                                                                 trace_up0),
                                 static_cast<unsigned long long>(SotcStats::sys_protects.load() -
                                                                 trace_pr0),
                                 first[0], first[1], first[2], first[3]);
                }
                const u64 offset_aligned = Common::AlignDown(offset, alignment);
                const u64 adjust = offset - offset_aligned;
                if (adjust % 4 != 0) {
                    // sotc: per binding; throttled (P5).
                    if (SotcStats::LogThrottled("buffer-unaligned")) {
                        LOG_WARNING(Render_Vulkan,
                                    "Buffer binding in shader {:#x} isn't dword aligned",
                                    stage.pgm_hash);
                    }
                }
                push_data.AddOffset(binding.buffer, adjust);
                buffer_infos.emplace_back(buffer->Handle(), offset_aligned, size + adjust);
                bound_buffers.emplace_back(buffer, offset, size, desc.is_written);
                if (desc.is_written) {
                    // Raw storage-buffer writes can also make an aliased cached image stale.
                    texture_cache.InvalidateMemoryFromGPU(vsharp.base_address, size);
                    Core::MemoryManager::NoteHostWrite(vsharp.base_address, size, 7,
                                                       stage.pgm_hash);
                    DispPhase::Mark(DispPhase::BbWrite);
                }
                needs_barrier |= runtime.IsBufferAccessed(buffer, offset, size, desc.is_written);
            }
        }

        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        set_write.pBufferInfo = &buffer_infos.back();
        ++binding.buffer;
        DrawPhase::Sub(DrawPhase::SBbDesc);
    }
}

// sotc: BbToggle::DrawBindMemo, SOTC_DRAW_MEMO=0 switches it off for the process.
bool Rasterizer::DrawBindMemoOn() {
    static const bool env_on = [] {
        const char* v = std::getenv("SOTC_DRAW_MEMO");
        return (!v || v[0] != '0') && std::getenv("SOTC_TSHARP_TRACE") == nullptr;
    }();
    return env_on && !BbToggle::Disabled(BbToggle::DrawBindMemo);
}

// sotc: DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL serves the depth target (stencil writes) and the
// sampling of its depth aspect, so both uses ask for the same stages and accesses: the image state
// does not change from one draw to the next and the render pass is not ended for it.
static constexpr vk::PipelineStageFlags2 kDepthReadOnlyStencilStages =
    vk::PipelineStageFlagBits2::eAllCommands;
static constexpr vk::AccessFlags2 kDepthReadOnlyStencilAccess =
    vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
    vk::AccessFlagBits2::eShaderRead;

// The second pass of BindTextures for one image: its view, the layout transition its use needs,
// the usage flags and the descriptor.
void Rasterizer::BindResolvedImage(VideoCore::ImageId image_id,
                                   VideoCore::TextureCache::BindingType type,
                                   const VideoCore::ImageViewInfo& view_info) {
    const bool is_storage = type == VideoCore::TextureCache::BindingType::Storage;
    bound_images.emplace_back(image_id);

    // FindTexture's UpdateImage touches the image (the LRU), so no GetImage touch first.
    auto& image_view = texture_cache.FindTexture(image_id, type, view_info);
    auto& image = texture_cache.PeekImage(image_id);
    const auto binding = image.binding;

    // The image is either bound as storage in a separate descriptor or bound as render
    // target in feedback loop. Depth images are excluded because they can't be bound as
    // storage and feedback loop doesn't make sense for them
    if ((binding.force_general || binding.is_target) && !image.info.props.is_depth) {
        if (instance.IsAttachmentFeedbackLoopLayoutSupported() && image.binding.is_target) {
            needs_barrier |= runtime.Transit(
                &image, vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT,
                vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead);
        } else {
            needs_barrier |= runtime.Transit(
                &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                view_info.range);
        }
    } else {
        if (is_storage) {
            needs_barrier |= runtime.Transit(
                &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                view_info.range);
        } else if (DepthSampledAsAttachment(image_id, image, view_info)) {
            // sotc: the depth buffer the draw writes stencil to is also sampled (depth aspect
            // only): DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL allows both, so the image stays in
            // the layout the render target needs. Moving it to DEPTH_STENCIL_READ_ONLY here and
            // back in BeginRendering ended the render pass at every such draw (~350 per frame in
            // the Shrine). SOTC_DEPTH_RO=0 off.
            needs_barrier |= runtime.Transit(
                &image, vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal,
                kDepthReadOnlyStencilStages, kDepthReadOnlyStencilAccess, view_info.range);
        } else {
            const auto new_layout = image.info.props.is_depth
                                        ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                        : vk::ImageLayout::eShaderReadOnlyOptimal;
            needs_barrier |= runtime.Transit(&image, new_layout,
                                             vk::PipelineStageFlagBits2::eAllCommands,
                                             vk::AccessFlagBits2::eShaderRead, view_info.range);
        }
    }
    image.usage.storage |= is_storage;
    image.usage.texture |= !is_storage;

    image_infos.emplace_back(VK_NULL_HANDLE, *image_view.image_view, image.backing->state.layout);
    image_info_images[image_infos.size() - 1] = image_id;
}

// sotc: BbToggle::TexBindFastPath (docs/sotc/GBUFFER-FASTPATH.md). BindResolvedImage of a texture
// binding (not storage) does: bound_images, FindTexture (UpdateImage: lock, TrackImage, TouchImage,
// RefreshImage; FindView), Transit to the shader-read layout, usage.texture and the descriptor
// (view, layout in force). When the image is not Dirty and tracked whole (UpdateIsNoop), has the
// backing the view was found in (views only grow, and keep their entries), needs no barrier
// (TransitIsNoop: same layout, access and stage, no pending write) and is not a render target or
// forced to General (the other branch of BindResolvedImage), all that is: the LRU touch, the usage
// flag, the bound list entry and the remembered view with the layout of the backing.
u32 Rasterizer::TexBindFastCheck(const TexBindMemoBinding& b, VideoCore::ImageId image_id) {
    if (!b.fast) {
        return 1;
    }
    if (image_id != b.bound_id) {
        return 2;
    }
    const auto& image = texture_cache.PeekImage(image_id);
    const bool is_depth = image.info.props.is_depth;
    if ((image.binding.force_general || image.binding.is_target) && !is_depth) {
        return 3;
    }
    if (is_depth && DepthSampledAsAttachment(image_id, image, b.view_info)) {
        return 7; // sotc: the slow path asks for the depth-read/stencil-attachment layout
    }
    if (image.backing != b.backing) {
        return 4;
    }
    if (!VideoCore::TextureCache::UpdateIsNoop(image)) {
        return 5;
    }
    const auto layout = is_depth ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                 : vk::ImageLayout::eShaderReadOnlyOptimal;
    if (!image.TransitIsNoop(layout, vk::AccessFlagBits2::eShaderRead,
                             vk::PipelineStageFlagBits2::eAllCommands, b.view_info.range)) {
        return 6;
    }
    return 0;
}

void Rasterizer::TexBindFastApply(const TexBindMemoBinding& b, VideoCore::ImageId image_id) {
    bound_images.emplace_back(image_id);
    texture_cache.TouchLinked(image_id); // UpdateImage's touch
    auto& image = texture_cache.PeekImage(image_id);
    image.usage.texture = 1u;
    image_infos.emplace_back(VK_NULL_HANDLE, b.view, image.backing->state.layout);
    image_info_images[image_infos.size() - 1] = image_id;
}

// sotc: a TexBindMemo whose images are still the ones their lookups find (see TexBindMemoBinding).
bool Rasterizer::TexBindMemoFresh(const TexBindMemo& memo) const {
    if (memo.gpu_epoch != VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed)) {
        return false;
    }
    for (u32 i = 0; i < memo.count; ++i) {
        const auto& b = memo.bindings[i];
        if (b.image_id &&
            (texture_cache.FindGeneration(b.addr) != b.addr_gen ||
             (b.other_addr && texture_cache.FindGeneration(b.other_addr) != b.other_gen))) {
            return false;
        }
    }
    return true;
}

// sotc: replays a TexBindMemo hit: the first and second pass of BindTextures for a stage whose
// T#s equal the remembered ones, minus the image lookups. Returns false, having changed nothing
// that matters, when an image wants a re-bind (the caller then resolves the stage as usual).
bool Rasterizer::ReplayTexBindMemo(const Shader::Info& stage, TexBindMemo& memo,
                                   const AmdGpu::Image* sharps,
                                   boost::container::small_vector<u32, 8>& array_sizes,
                                   std::array<VideoCore::ImageId, Shader::NUM_IMAGES>& bind_ids,
                                   std::array<bool, Shader::NUM_IMAGES>& bind_storage,
                                   u32& num_binds) {
    const u32 n = memo.count;
    std::array<VideoCore::ImageId, MemoMaxImages> ids;
    // What FindImage did for the image: found now. Then the stencil to depth redirect.
    for (u32 i = 0; i < n; ++i) {
        VideoCore::ImageId image_id = memo.bindings[i].image_id;
        if (image_id) {
            auto* image = &texture_cache.PeekImage(image_id);
            texture_cache.NoteFound(*image);
            if (const auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                image_id = depth_image_id;
                image = &texture_cache.GetImage(image_id);
            }
            if (image->binding.needs_rebind) {
                return false;
            }
        }
        ids[i] = image_id;
    }
    for (u32 i = 0; i < n; ++i) {
        if (sharps[i].lod_hw_cnt_en) {
            SotcLod::Mark(sharps[i].counter_bank_id); // GET_LOD_STATS (sotc_lod_stats.h)
        }
        if (ids[i]) {
            auto& image = texture_cache.PeekImage(ids[i]);
            if (image.binding.is_bound) {
                image.binding.force_general |= stage.images[i].is_written;
            }
            image.binding.is_bound = 1u;
        }
        array_sizes.push_back(1);
    }
    for (u32 i = 0; i < n; ++i) {
        const auto& b = memo.bindings[i];
        VideoCore::ImageId image_id = ids[i];
        VideoCore::TextureCache::BindingType type = b.type;
        VideoCore::ImageViewInfo view_info = b.view_info;
        if (!image_id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
            image_info_images[image_infos.size() - 1] = {};
        } else {
            if (auto& old_image = texture_cache.PeekImage(image_id);
                old_image.binding.needs_rebind) {
                // Set meanwhile (an image update freed it): as the second pass does, from a
                // descriptor made again from the T#, and forget this set.
                VideoCore::TextureCache::ImageDesc desc{sharps[i], stage.images[i]};
                old_image.binding = {};
                image_id = texture_cache.FindImage(desc);
                type = desc.type;
                view_info = desc.view_info;
                memo.stage = nullptr;
            }
            // sotc: BbToggle::TexBindFastPath (only when the image is the one the memo stored: a
            // re-bind above looked up another one and goes the long way).
            const bool same = image_id == ids[i];
            if (same && !BbToggle::Disabled(BbToggle::TexBindFastPath)) {
                using DepthFastStats::c;
                ++c.bt_cand;
                const u32 cause = TexBindFastCheck(b, image_id);
                ++c.bt_cause[cause];
                c.bt_hit += cause == 0;
                if (cause == 0 && !DepthFastStats::kBtCount) {
                    if (!DepthFastStats::kVerify) {
                        TexBindFastApply(b, image_id);
                    } else {
                        // Verify: replay, snapshot, take the replay back and bind in full on top:
                        // the descriptor and every effect on the image must be the same.
                        struct Snap { // no padding: compared with memcmp
                            u32 flags, usage, binding, subs;
                            const void* backing;
                            u64 layout, access, stage, track_a, track_b, lru, tick_found, needs,
                                pending, bound_n, infos_n;
                        };
                        const auto snap = [&] {
                            const auto& image = texture_cache.PeekImage(image_id);
                            Snap s;
                            std::memset(&s, 0, sizeof(s));
                            s.flags = static_cast<u32>(image.flags);
                            std::memcpy(&s.usage, &image.usage, sizeof(u32));
                            std::memcpy(&s.binding, &image.binding, sizeof(u32));
                            s.subs = static_cast<u32>(image.backing->subresource_states.size());
                            s.backing = image.backing;
                            s.layout = static_cast<u64>(image.backing->state.layout);
                            s.access = static_cast<u64>(image.backing->state.access_mask);
                            s.stage = static_cast<u64>(image.backing->state.pl_stage);
                            s.track_a = image.track_addr;
                            s.track_b = image.track_addr_end;
                            s.lru = texture_cache.LruTickOf(image_id);
                            s.tick_found = image.tick_accessed_last;
                            s.needs = needs_barrier ? 1 : 0;
                            s.pending = runtime.PendingImageBarriers();
                            s.bound_n = bound_images.size();
                            s.infos_n = image_infos.size();
                            return s;
                        };
                        TexBindFastApply(b, image_id);
                        const vk::ImageView pred_view = image_infos.back().imageView;
                        const vk::ImageLayout pred_layout = image_infos.back().imageLayout;
                        const Snap before = snap();
                        bound_images.pop_back();
                        image_infos.pop_back();
                        BindResolvedImage(image_id, type, view_info);
                        const Snap after = snap();
                        ++c.bt_verify;
                        const bool same_info = image_infos.back().imageView == pred_view &&
                                               image_infos.back().imageLayout == pred_layout &&
                                               bound_images.back() == image_id;
                        if (!same_info || std::memcmp(&before, &after, sizeof(Snap)) != 0) {
                            if (++c.bt_bad <= 20) {
                                std::fprintf(stderr,
                                             "SOTC BTFAST VERIFY: texture binding replay differs "
                                             "(mismatch %llu): info %d flags %u/%u usage %u/%u "
                                             "binding %u/%u layout %llu/%llu access %llx/%llx "
                                             "stage %llx/%llx needs %llu/%llu pending %llu/%llu "
                                             "lru %llu/%llu%c",
                                             static_cast<unsigned long long>(c.bt_bad),
                                             int(same_info), before.flags, after.flags,
                                             before.usage, after.usage, before.binding,
                                             after.binding,
                                             static_cast<unsigned long long>(before.layout),
                                             static_cast<unsigned long long>(after.layout),
                                             static_cast<unsigned long long>(before.access),
                                             static_cast<unsigned long long>(after.access),
                                             static_cast<unsigned long long>(before.stage),
                                             static_cast<unsigned long long>(after.stage),
                                             static_cast<unsigned long long>(before.needs),
                                             static_cast<unsigned long long>(after.needs),
                                             static_cast<unsigned long long>(before.pending),
                                             static_cast<unsigned long long>(after.pending),
                                             static_cast<unsigned long long>(before.lru),
                                             static_cast<unsigned long long>(after.lru), 10);
                            }
                        }
                    }
                    bind_ids[num_binds] = image_id;
                    bind_storage[num_binds] = false;
                    ++num_binds;
                    continue;
                }
            }
            BindResolvedImage(image_id, type, view_info);
        }
        bind_ids[num_binds] = image_id;
        bind_storage[num_binds] =
            image_id && type == VideoCore::TextureCache::BindingType::Storage;
        ++num_binds;
    }
    return true;
}

bool Rasterizer::DepthSampledAsAttachment(VideoCore::ImageId image_id, const VideoCore::Image& image,
                                          const VideoCore::ImageViewInfo& view_info) const {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_DEPTH_RO");
        return !v || v[0] != '0';
    }();
    if (!on || bind_stage_compute || !image.info.props.has_stencil || image_id != db_desc.first) {
        return false;
    }
    if (!liverpool->DrawRegs().depth_control.stencil_enable || db_desc.second.view_info.is_storage) {
        return false;
    }
    const vk::Format fmt = view_info.format;
    return Vulkan::LiverpoolToVK::IsFormatDepthCompatible(fmt) &&
           !Vulkan::LiverpoolToVK::IsFormatStencilCompatible(fmt);
}

void Rasterizer::BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding) {
    bind_stage_compute = stage.sw_stage == Shader::SwStage::Compute; // sotc: DepthSampledAsAttachment
    const auto texture_lock = texture_cache.BatchLock(); // sotc: one lock for the pass
    DrawPhase::Sub(DrawPhase::SBtPre);
    image_bindings.clear();
    const u32 first_image_idx = image_infos.size();
    // To emulate storing to explicit mip levels, build a descriptor array with each mip level.
    boost::container::small_vector<u32, 8> image_descriptor_array_sizes;

    // sotc: BbToggle::DrawBindMemo (TexBindMemo). A stage of plain images (no descriptor array, no
    // mip fallback) looks its resolved bindings up by its exact T# set; a hit replays them in
    // place of the first pass below, which then has no images to resolve.
    std::array<VideoCore::ImageId, Shader::NUM_IMAGES> bind_ids; // each binding after the 2nd pass
    std::array<bool, Shader::NUM_IMAGES> bind_storage;
    u32 num_binds = 0;
    std::array<AmdGpu::Image, MemoMaxImages> memo_sharps;
    std::array<VideoCore::ImageId, MemoMaxImages> memo_pre_ids{};
    TexBindMemo* memo = nullptr;
    bool replayed = false;
    bool memo_impure = false; // a binding depended on more than the T# bytes: do not remember
    if (DrawBindMemoOn() && stage.images.size() <= MemoMaxImages && !stage.images.empty()) {
        u64 hash = reinterpret_cast<uintptr_t>(&stage) * 0x9E3779B97F4A7C15ULL;
        u32 n = 0;
        bool plain = true;
        for (const auto& image_desc : stage.images) {
            if (image_desc.array_size != 0 ||
                image_desc.mip_fallback_mode != Shader::MipStorageFallbackMode::None) {
                plain = false;
                break;
            }
            memo_sharps[n] = image_desc.GetSharp(stage);
            std::array<u64, 4> words;
            std::memcpy(words.data(), &memo_sharps[n], sizeof(words));
            hash = (hash ^ words[0]) * 0xC2B2AE3D27D4EB4FULL;
            hash = (hash ^ words[1]) * 0x165667B19E3779F9ULL;
            hash = (hash ^ words[2]) * 0x27D4EB2F165667C5ULL;
            hash = (hash ^ words[3]) * 0x9E3779B97F4A7C15ULL;
            hash ^= hash >> 29;
            ++n;
        }
        if (plain) {
            if (!tex_bind_memos) {
                tex_bind_memos.reset(static_cast<TexBindMemo*>(
                    std::calloc(NumTexBindMemos, sizeof(TexBindMemo))));
            }
            memo = &tex_bind_memos[hash % NumTexBindMemos];
            const bool key_hit = memo->stage == &stage && memo->count == n &&
                                 std::memcmp(memo->sharps.data(), memo_sharps.data(),
                                             n * sizeof(AmdGpu::Image)) == 0;
            if (key_hit && TexBindMemoFresh(*memo)) {
                replayed = ReplayTexBindMemo(stage, *memo, memo_sharps.data(),
                                             image_descriptor_array_sizes, bind_ids, bind_storage,
                                             num_binds);
            }
            if (SotcStats::kFpsLog) { // sotc: read only by the SOTC_FPS printer (sotc_stats.h)
                (replayed ? SotcStats::bind_memo_hits : SotcStats::bind_memo_misses)
                    .fetch_add(1, std::memory_order_relaxed);
                if (!replayed) {
                    // Why: another stage / T# set in the slot, the GPU-modified epoch moved, an
                    // image's address generation moved, or the replay itself asked for a re-bind.
                    auto& cause = !key_hit ? SotcStats::tbm_miss_key
                                  : memo->gpu_epoch != VideoCore::gpu_modified_epoch.load(
                                                           std::memory_order_relaxed)
                                      ? SotcStats::tbm_miss_epoch
                                  : !TexBindMemoFresh(*memo) ? SotcStats::tbm_miss_gen
                                                             : SotcStats::tbm_miss_replay;
                    cause.fetch_add(1, std::memory_order_relaxed);
                }
            }
        } else if (SotcStats::kFpsLog) {
            SotcStats::tbm_not_plain.fetch_add(1, std::memory_order_relaxed);
        }
    }
    DispPhase::Mark(DispPhase::BtPre);
    DrawPhase::Sub(DrawPhase::SBtMemo);
    static const Shader::ImageResourceList no_images;
    const Shader::ImageResourceList& first_pass_images = replayed ? no_images : stage.images;

    for (const auto& image_desc : first_pass_images) {
        const auto tsharp = image_desc.GetSharp(stage);
        if (image_desc.array_size != 0) {
            // dynamic_tsharp_array_size (from Pink-shadPS4): one binding per table element, then a
            // null one for out-of-range indices. The shader was typed after `tsharp`, the first
            // valid element, so elements of another view type or number class stay null.
            const u32 num_bindings = image_desc.NumBindings(stage);
            const auto view_type = tsharp.GetViewType(image_desc.is_array);
            const bool is_integer = AmdGpu::IsInteger(tsharp.GetNumberFmt());
            for (u32 i = 0; i < num_bindings; ++i) {
                const auto element = i < image_desc.array_size
                                         ? image_desc.GetElementSharp(stage, i)
                                         : AmdGpu::Image::Null(image_desc.is_depth);
                const auto element_dfmt = element.GetDataFmt();
                const auto element_nfmt = element.GetNumberFmt();
                const bool bindable =
                    element && element_dfmt != AmdGpu::DataFormat::FormatInvalid &&
                    magic_enum::enum_contains(element_dfmt) &&
                    magic_enum::enum_contains(element_nfmt) &&
                    memory->IsValidGpuMapping(element.Address(), 0) &&
                    element.GetViewType(image_desc.is_array) == view_type &&
                    AmdGpu::IsInteger(element_nfmt) == is_integer &&
                    element.GetNumberConversion() == tsharp.GetNumberConversion();
                if (!bindable) {
                    image_bindings.emplace_back(std::piecewise_construct, std::tuple{},
                                                std::tuple{});
                    continue;
                }
                if (element.lod_hw_cnt_en) {
                    SotcLod::Mark(element.counter_bank_id);
                    lod_marks.push_back(element.counter_bank_id); // sotc: replayed by a reused bind
                }
                auto& [image_id, desc] = image_bindings.emplace_back(
                    std::piecewise_construct, std::tuple{}, std::tuple{element, image_desc});
                image_id = texture_cache.FindImage(desc);
                auto* image = &texture_cache.GetImage(image_id);
                if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                    image_id = depth_image_id;
                    image = &texture_cache.GetImage(image_id);
                }
                image->binding.is_bound = 1u;
            }
            image_descriptor_array_sizes.push_back(num_bindings);
            continue;
        }
        if (tsharp.lod_hw_cnt_en) {
            SotcLod::Mark(tsharp.counter_bank_id); // GET_LOD_STATS (sotc_lod_stats.h)
        }
        // sotc: SOTC_TSHARP_TRACE=1: mip fields of the large streamed textures (first 120
        // distinct address/field combinations): which mips the game says are resident.
        static const bool tsharp_trace = std::getenv("SOTC_TSHARP_TRACE") != nullptr;
        if (tsharp_trace && tsharp.width + 1 >= 1024 && tsharp.last_level > 0) {
            static std::mutex m;
            static std::set<std::tuple<u64, u32, u32, u32, u32>> seen;
            std::scoped_lock lk{m};
            const auto key = std::make_tuple(u64{tsharp.Address()}, u32(tsharp.base_level),
                                             u32(tsharp.last_level), u32(tsharp.min_lod),
                                             u32(tsharp.min_lod_warn));
            if (seen.size() < 120 && seen.insert(key).second) {
                std::fprintf(stderr,
                             "SOTC tsharp %#llx %ux%u fmt %u base %u last %u min_lod %u (%.2f) "
                             "warn %u hwcnt %u bank %u\n",
                             static_cast<unsigned long long>(tsharp.Address()), tsharp.width + 1,
                             tsharp.height + 1, u32(tsharp.GetDataFmt()), u32(tsharp.base_level),
                             u32(tsharp.last_level), u32(tsharp.min_lod), tsharp.min_lod / 256.f,
                             u32(tsharp.min_lod_warn), u32(tsharp.lod_hw_cnt_en),
                             u32(tsharp.counter_bank_id));
            }
        }
        // sotc: first few only (per-draw warning; see BufferCache::SynchronizeMemoryFromImage).
        if (static u32 shown = 0; shown < 4 && texture_cache.IsMeta(tsharp.Address())) {
            ++shown;
            LOG_WARNING(Render_Vulkan, "Unexpected metadata read by a shader (texture)");
        }

        const auto data_fmt = tsharp.GetDataFmt();
        const auto num_fmt = tsharp.GetNumberFmt();
        if (tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid) {
            image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        if (!memory->IsValidGpuMapping(tsharp.Address(), 0) ||
            !magic_enum::enum_contains(data_fmt) || !magic_enum::enum_contains(num_fmt)) {
            // sotc: per binding; throttled (P5).
            if (SotcStats::LogThrottled("image-invalid-taddr")) {
                LOG_WARNING(Render_Vulkan,
                            "Rejecting invalid T# address={:#x}, pitch={}, width={}, "
                            "data_format={}, num_format={}",
                            tsharp.Address(), tsharp.pitch, tsharp.width,
                            static_cast<u32>(data_fmt), static_cast<u32>(num_fmt));
            }
            memo_impure = true; // the mapping can change without the T# changing
            image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        const Shader::MipStorageFallbackMode mip_fallback_mode = image_desc.mip_fallback_mode;
        const u32 num_bindings = image_desc.NumBindings(stage);

        for (auto i = 0; i < num_bindings; i++) {
            auto& [image_id, desc] = image_bindings.emplace_back(
                std::piecewise_construct, std::tuple{}, std::tuple{tsharp, image_desc});

            if (mip_fallback_mode == Shader::MipStorageFallbackMode::ConstantIndex) {
                ASSERT(num_bindings == 1);
                desc.view_info.range.base.level += image_desc.constant_mip_index;
                desc.view_info.range.extent.levels = 1;
            } else if (mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                desc.view_info.range.base.level += i;
                desc.view_info.range.extent.levels = 1;
            }

            image_id = texture_cache.FindImage(desc);
            if (memo && image_bindings.size() <= MemoMaxImages) {
                memo_pre_ids[image_bindings.size() - 1] = image_id;
            }
            auto* image = &texture_cache.GetImage(image_id);
            if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                // If this image has an associated depth image, it's a stencil attachment.
                // Redirect the access to the actual depth-stencil buffer.
                image_id = depth_image_id;
                image = &texture_cache.GetImage(image_id);
            }
            if (image->binding.is_bound) {
                // The image is already bound. In case if it is about to be used as storage we
                // need to force general layout on it.
                image->binding.force_general |= image_desc.is_written;
            }
            image->binding.is_bound = 1u;
        }

        image_descriptor_array_sizes.push_back(num_bindings);
    }

    DispPhase::Mark(DispPhase::BtFind);
    // Second pass to re-bind images that were updated after binding
    bool rebind_seen = false;
    for (auto& [image_id, desc] : image_bindings) {
        const bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        if (!image_id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
            image_info_images[image_infos.size() - 1] = {};
        } else {
            if (auto& old_image = texture_cache.GetImage(image_id);
                old_image.binding.needs_rebind) {
                old_image.binding = {};
                image_id = texture_cache.FindImage(desc);
                rebind_seen = true;
            }
            BindResolvedImage(image_id, desc.type, desc.view_info);
        }
        bind_ids[num_binds] = image_id;
        bind_storage[num_binds] = is_storage;
        ++num_binds;
    }
    DispPhase::Mark(DispPhase::BtBind);
    // sotc: remember what this stage's set resolved to (plain stages only, see above).
    if (memo && !replayed && !memo_impure && !rebind_seen &&
        image_bindings.size() == stage.images.size()) {
        // Only images FindImage matched as a whole: an overlap resolution depends on more than
        // the registry (idle ticks, what is bound) and is made again each time.
        // Storage bindings are made again as well: a writer's lookup frees the mip tails over it.
        for (u32 k = 0; k < image_bindings.size() && memo; ++k) {
            if (image_bindings[k].second.type != VideoCore::TextureCache::BindingType::Texture) {
                memo_impure = true;
                break;
            }
            if (memo_pre_ids[k] &&
                !texture_cache.FoundExactly(memo_pre_ids[k], image_bindings[k].second)) {
                memo_impure = true;
                break;
            }
        }
    }
    if (memo && !replayed && !memo_impure && !rebind_seen &&
        image_bindings.size() == stage.images.size()) {
        memo->stage = &stage;
        memo->gpu_epoch = VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed);
        memo->count = static_cast<u32>(stage.images.size());
        std::memcpy(memo->sharps.data(), memo_sharps.data(),
                    memo->count * sizeof(AmdGpu::Image));
        for (u32 k = 0; k < memo->count; ++k) {
            const auto& found = image_bindings[k].second;
            auto& b = memo->bindings[k];
            b = {.image_id = memo_pre_ids[k], .view_info = found.view_info, .type = found.type};
            if (b.image_id) {
                b.addr = found.info.guest_address;
                b.addr_gen = texture_cache.FindGeneration(b.addr);
                if (found.rebase_mip != 0 && VideoCore::MipTailMode() != 0) {
                    b.other_addr = found.other_info ? found.other_info->guest_address : 0;
                    b.other_gen = b.other_addr ? texture_cache.FindGeneration(b.other_addr) : 0;
                }
                // sotc: BbToggle::TexBindFastPath: what BindResolvedImage just gave this binding
                // (image_infos holds one entry per image binding of a plain stage, in order).
                const auto final_id = image_bindings[k].first;
                const u32 info_idx = first_image_idx + k;
                if (final_id && info_idx < image_infos.size() &&
                    found.type == VideoCore::TextureCache::BindingType::Texture) {
                    const auto& image = texture_cache.PeekImage(final_id);
                    const bool special = (image.binding.force_general || image.binding.is_target) &&
                                         !image.info.props.is_depth;
                    b.bound_id = final_id;
                    b.backing = image.backing;
                    b.view = image_infos[info_idx].imageView;
                    b.fast = !special && b.view;
                }
            }
        }
    }

    // sotc: TakeHudless: the final composite samples the UI layer (an sRGB render target) and the
    // fp16 scene, nothing else. Off unless SOTC_FG_HUDLESS=1: with FSR 3 the generated frames of
    // the title screen (faded logo) and of loading screens (the scene left behind the opaque UI
    // layer shows through) came out wrong, the real frame's UI not put back over them.
    static const bool hudless_on = [] {
        const char* v = std::getenv("SOTC_FG_HUDLESS");
        return v && v[0] == '1';
    }();
    if (hudless_on && stage.sw_stage == Shader::SwStage::Fragment && num_binds == 2 &&
        bind_ids[0] && bind_ids[1] && FrameGen::Current() != FrameGen::Mode::Off) {
        const auto& ui = texture_cache.GetImage(bind_ids[0]);
        const auto& scene = texture_cache.GetImage(bind_ids[1]);
        if (ui.usage.render_target && ui.info.pixel_format == vk::Format::eR8G8B8A8Srgb &&
            scene.info.pixel_format == vk::Format::eR16G16B16A16Sfloat) {
            hudless_ui_info = static_cast<s32>(first_image_idx);
        }
    }

    DrawPhase::Sub(DrawPhase::SBtImages);
    u32 image_info_idx = first_image_idx;
    u32 image_binding_idx = 0;
    for (u32 array_size : image_descriptor_array_sizes) {
        const bool is_storage = bind_storage[image_binding_idx];
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = array_size;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[image_info_idx];

        image_info_idx += array_size;
        image_binding_idx += array_size;
        binding.unified += array_size;
    }

    // sotc: at a render size above 1080 lines the game stretches its 1080p 2D art (splash logos,
    // UI) itself, with the point sampler meant for 1:1 drawing: hard steps. A draw that samples
    // one asset texture (never a render target, storage or depth image) magnifies bilinearly
    // there. SOTC_UI_LINEAR=0/1 overrides, SOTC_UI_LINEAR_TRACE=1 lists the draws it changes.
    static const bool ui_linear = [] {
        if (const char* v = std::getenv("SOTC_UI_LINEAR"); v && *v) {
            return *v != '0';
        }
        const char* r = std::getenv("SOTC_RENDER_RES");
        return r && *r && std::strcmp(r, "1920x1080") != 0;
    }();
    const VideoCore::Image* ui_asset = nullptr;
    if (ui_linear && num_binds == 1 && bind_ids[0] && stage.images.size() == 1 &&
        stage.images[0].array_size == 0) {
        const auto& image = texture_cache.GetImage(bind_ids[0]);
        const auto& u = image.usage;
        if (!u.render_target && !u.depth_target && !u.storage && !u.vo_surface &&
            !image.binding.is_target && !image.info.props.is_depth &&
            !AmdGpu::IsInteger(stage.images[0].GetSharp(stage).GetNumberFmt())) {
            ui_asset = &image;
        }
    }
    for (const auto& sampler : stage.samplers) {
        auto ssharp = sampler.GetSharp(stage);
        if (ui_asset && ssharp.xy_mag_filter.Value() == AmdGpu::Filter::Point &&
            !sampler.is_depth) {
            ssharp.xy_mag_filter.Assign(AmdGpu::Filter::Bilinear);
            static const bool trace = std::getenv("SOTC_UI_LINEAR_TRACE") != nullptr;
            if (trace) {
                static std::mutex m;
                static std::set<u64> seen;
                std::scoped_lock lk{m};
                if (seen.size() < 200 && seen.insert(stage.pgm_hash ^ ui_asset->info.guest_address).second) {
                    std::fprintf(stderr, "SOTC ui-linear pgm %#llx tex %#llx %ux%u\n",
                                 static_cast<unsigned long long>(stage.pgm_hash),
                                 static_cast<unsigned long long>(ui_asset->info.guest_address),
                                 ui_asset->info.size.width, ui_asset->info.size.height);
                }
            }
        }
        if (!ssharp.Valid() || (ssharp.border_color_type.Value() == AmdGpu::BorderColor::Custom &&
                                liverpool->DrawRegs().ta_bc_base.Address() == 0)) {
            // sotc: per binding; throttled (P5).
            if (SotcStats::LogThrottled("sampler-invalid")) {
                LOG_WARNING(Render_Vulkan,
                            "Rejecting invalid S# max_aniso={}, filter_mode={}, mip_filter={}, "
                            "border_color_type={}, border_color_base={:#x}",
                            static_cast<u32>(ssharp.max_aniso.Value()),
                            static_cast<u32>(ssharp.filter_mode.Value()),
                            static_cast<u32>(ssharp.mip_filter.Value()),
                            static_cast<u32>(ssharp.border_color_type.Value()),
                            liverpool->DrawRegs().ta_bc_base.Address());
            }
            ssharp = AmdGpu::Sampler{};
        }
        const auto vk_sampler =
            texture_cache.GetSampler(ssharp, liverpool->DrawRegs().ta_bc_base, sampler.is_depth);
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        image_info_images[image_infos.size() - 1] = {};
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eSampler;
        set_write.pImageInfo = &image_infos.back();
    }
}

// sotc: BbToggle::DepthFastPath (docs/sotc/DEPTH-FASTPATH.md).
bool Rasterizer::DepthFastActive() const {
    return df_rel_built && !BbToggle::Disabled(BbToggle::DepthFastPath) &&
           liverpool->IsBackThreaded() && liverpool->OnBackThread();
}

/// The register dwords the render target block reads: PrepareRenderState, BeginRendering and the
/// dynamic state derivation (the list of CommitDynamicStateOld, a superset of what
/// Update{ViewportScissor,DepthStencil,Primitive,Rasterization,ColorBlending}State read). The
/// command processor sends changed dwords, so the draw thread knows per op whether one of these
/// changed in value (Liverpool::DrawRelSerial).
void Rasterizer::DepthFastEnsureRelevance() {
    if (df_rel_built || !liverpool->IsBackThreaded() || !liverpool->OnBackThread()) {
        return;
    }
    const auto& regs = liverpool->DrawRegs();
    const u32* base = regs.reg_array.data();
    std::vector<u64> depth(regs.reg_array.size() / 64 + 1, 0);
    std::vector<u64> dynamic(regs.reg_array.size() / 64 + 1, 0);
    std::vector<u64> color(regs.reg_array.size() / 64 + 1, 0);
    const auto add = [&](std::vector<u64>& mask, const void* field, size_t bytes) {
        const u32 first = static_cast<u32>(reinterpret_cast<const u32*>(field) - base);
        const u32 last = first + static_cast<u32>((bytes + 3) / 4) - 1;
        for (u32 d = first; d <= last; ++d) {
            mask[d / 64] |= u64{1} << (d % 64);
        }
    };
    const auto both = [&](const void* field, size_t bytes) {
        add(depth, field, bytes);
        add(dynamic, field, bytes);
    };
    // Dynamic state inputs only (the list CommitDynamicStateOld tracks).
    add(dynamic, &regs.screen_scissor, sizeof(regs.screen_scissor));
    add(dynamic, &regs.window_scissor, sizeof(regs.window_scissor));
    add(dynamic, &regs.generic_scissor, sizeof(regs.generic_scissor));
    add(dynamic, &regs.window_offset, sizeof(regs.window_offset));
    add(dynamic, &regs.polygon_control, sizeof(regs.polygon_control));
    add(dynamic, &regs.viewport_control, sizeof(regs.viewport_control));
    add(dynamic, &regs.viewports, sizeof(regs.viewports));
    add(dynamic, &regs.viewport_depths, sizeof(regs.viewport_depths));
    add(dynamic, &regs.clipper_control, sizeof(regs.clipper_control));
    add(dynamic, &regs.mode_control, sizeof(regs.mode_control));
    add(dynamic, &regs.viewport_scissors, sizeof(regs.viewport_scissors));
    add(dynamic, &regs.depth_bounds_min, sizeof(regs.depth_bounds_min));
    add(dynamic, &regs.depth_bounds_max, sizeof(regs.depth_bounds_max));
    add(dynamic, &regs.poly_offset, sizeof(regs.poly_offset));
    add(dynamic, &regs.stencil_control, sizeof(regs.stencil_control));
    add(dynamic, &regs.stencil_ref_front, sizeof(regs.stencil_ref_front));
    add(dynamic, &regs.stencil_ref_back, sizeof(regs.stencil_ref_back));
    add(dynamic, &regs.enable_primitive_restart, sizeof(regs.enable_primitive_restart));
    add(dynamic, &regs.primitive_type, sizeof(regs.primitive_type));
    add(dynamic, &regs.primitive_restart_index, sizeof(regs.primitive_restart_index));
    add(dynamic, &regs.line_control, sizeof(regs.line_control));
    add(dynamic, &regs.blend_constants, sizeof(regs.blend_constants));
    // Read by both the dynamic state derivation and the depth side of the render target block.
    // DB_DEPTH_CONTROL: the whole dword for the dynamic state (depth and stencil functions...), only
    // stencil_enable, depth_enable and depth_write_enable (bits 0-2) for the render targets.
    static_assert(sizeof(regs.depth_control) == sizeof(u32));
    add(dynamic, &regs.depth_control, sizeof(regs.depth_control));
    liverpool->SetRelevancePartial(
        AmdGpu::Liverpool::RelRender,
        static_cast<u32>(reinterpret_cast<const u32*>(&regs.depth_control) - base), 0x7);
    both(&regs.depth_buffer, sizeof(regs.depth_buffer));
    both(&regs.depth_render_control, sizeof(regs.depth_render_control));
    // Depth side only (PrepareRenderState, BeginRendering). color_control is here because the
    // render target memo compares cb_disabled whatever the number of colour targets.
    add(depth, &regs.color_control, sizeof(regs.color_control));
    add(depth, &regs.depth_view, sizeof(regs.depth_view));
    add(depth, &regs.depth_htile_data_base, sizeof(regs.depth_htile_data_base));
    add(depth, &regs.depth_clear, sizeof(regs.depth_clear));
    add(depth, &regs.stencil_clear, sizeof(regs.stencil_clear));
    // Colour side (a draw with colour targets needs this serial as well).
    add(color, &regs.color_buffers, sizeof(regs.color_buffers));
    add(color, &regs.color_target_mask, sizeof(regs.color_target_mask));
    liverpool->SetRenderRelevance(AmdGpu::Liverpool::RelRender, depth.data(), depth.size());
    liverpool->SetRenderRelevance(AmdGpu::Liverpool::RelDynamic, dynamic.data(), dynamic.size());
    liverpool->SetRenderRelevance(AmdGpu::Liverpool::RelColor, color.data(), color.size());
    df_rel_built = true;
}

/// 0 when the RenderState BeginRendering would derive for this depth-only draw is df_memo.state and
/// replaying its effects is all it would do; else why not. No effects.
u32 Rasterizer::DepthFastBeginCheck(const GraphicsPipeline*) {
    const auto& m = df_memo;
    if (!m.valid) {
        return 1;
    }
    if (m.rel_serial != liverpool->DrawRelSerial(AmdGpu::Liverpool::RelRender)) {
        return 2;
    }
    if (!db_desc.first || db_desc.first != m.db_id) {
        return 3;
    }
    if (m.image_generation != texture_cache.ImageSetGeneration()) {
        return 4;
    }
    if (m.gpu_epoch != VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed)) {
        return 5;
    }
    if (m.meta_epoch != texture_cache.MetaEpoch()) {
        return 6;
    }
    const auto& image = texture_cache.PeekImage(m.db_id);
    using Flags = VideoCore::ImageFlagBits;
    // FindDepthTarget: MarkWritten (flag already set), UpdateImage (not Dirty, tracked whole),
    // the ASSERT on needs_rebind; the backing is the one the views and layout were read from.
    if (False(image.flags & Flags::Registered) || False(image.flags & Flags::GpuModified) ||
        image.binding.needs_rebind || !VideoCore::TextureCache::UpdateIsNoop(image)) {
        return 7;
    }
    if (image.backing != m.backing) {
        return 8;
    }
    if (!image.TransitIsNoop(m.layout, m.access, m.stage, m.range)) {
        return 9;
    }
    return 0;
}

/// The effects of BeginRendering for a depth-only draw that DepthFastBeginCheck passed: the LRU
/// touches (depth image, linked stencil image) and the usage flag; everything else it does is a
/// no-op then.
void Rasterizer::DepthFastBeginApply() {
    attachment_feedback_loop = false;
    texture_cache.GetImage(df_memo.db_id);
    texture_cache.TouchLinked(df_memo.stencil_id);
    texture_cache.PeekImage(df_memo.db_id).usage.depth_target = true;
}

void Rasterizer::DepthFastRecord(const RenderState& state) {
    using DepthFastStats::c;
    auto& m = df_memo;
    const auto& att = state.depth_stencil_attachment;
    // A state that clears is not the state the next draw derives (the clear bit is consumed).
    if (!db_desc.first || att.depth_clear || att.stencil_clear || state.num_color_attachments != 0) {
        m.valid = false;
        ++c.begin_norecord;
        return;
    }
    const auto& image = texture_cache.PeekImage(db_desc.first);
    m.valid = true;
    m.rel_serial = liverpool->DrawRelSerial(AmdGpu::Liverpool::RelRender);
    m.image_generation = texture_cache.ImageSetGeneration();
    m.gpu_epoch = VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed);
    m.meta_epoch = texture_cache.MetaEpoch();
    m.db_id = db_desc.first;
    m.stencil_id = texture_cache.LastDepthStencil();
    m.backing = image.backing;
    m.layout = df_transit_layout;
    m.access = df_transit_access;
    m.stage = df_transit_stage;
    m.range = df_transit_range;
    m.state = state;
    m.token = df_next_token++;
    ++c.begin_record;
    if (DepthFastStats::kWhy) {
        const auto& regs = liverpool->DrawRegs();
        df_snap.depth_control = regs.depth_control;
        df_snap.depth_buffer = regs.depth_buffer;
        df_snap.depth_render_control = regs.depth_render_control;
        df_snap.color_control = regs.color_control;
        df_snap.depth_view = regs.depth_view;
        df_snap.htile = regs.depth_htile_data_base.address;
        df_snap.depth_clear = std::bit_cast<u32>(regs.depth_clear);
        df_snap.stencil_clear = regs.stencil_clear;
        df_snap.db_extent = liverpool->DrawDbExtent().raw;
    }
}

// sotc: BbToggle::GBufferFastPath (docs/sotc/GBUFFER-FASTPATH.md). The transition BeginRendering
// makes on every colour attachment, a constant of the code.
static constexpr vk::ImageLayout kGbColorLayout = vk::ImageLayout::eColorAttachmentOptimal;
static constexpr vk::PipelineStageFlags2 kGbColorStage =
    vk::PipelineStageFlagBits2::eColorAttachmentOutput;
static constexpr vk::AccessFlags2 kGbColorAccess =
    vk::AccessFlagBits2::eColorAttachmentWrite | vk::AccessFlagBits2::eColorAttachmentRead;

/// 0 when the RenderState BeginRendering would derive for this draw (colour targets) is
/// gb_memo.state and replaying its effects is all it would do; else why not (kGbCause). No effects.
u32 Rasterizer::GBufferFastCheck(const GraphicsPipeline* pipeline) {
    using Flags = VideoCore::ImageFlagBits;
    const auto& m = gb_memo;
    if (!m.valid) {
        return 1;
    }
    const auto& key = pipeline->GetGraphicsKey();
    if (m.mrt_mask != key.mrt_mask || m.samples != key.color_samples) {
        return 2;
    }
    if (m.rel_render != liverpool->DrawRelSerial(AmdGpu::Liverpool::RelRender) ||
        m.rel_color != liverpool->DrawRelSerial(AmdGpu::Liverpool::RelColor)) {
        return 3;
    }
    for (u32 cb = 0; cb < m.num_cb; ++cb) {
        if (cb_descs[cb].first != m.color_id[cb]) {
            return 4;
        }
    }
    if (db_desc.first != m.db_id) {
        return 4;
    }
    if (m.image_generation != texture_cache.ImageSetGeneration()) {
        return 5;
    }
    if (m.gpu_epoch != VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed)) {
        return 6;
    }
    if (m.meta_epoch != texture_cache.MetaEpoch()) {
        return 7;
    }
    for (u32 cb = 0; cb < m.num_cb; ++cb) {
        const auto id = m.color_id[cb];
        if (!id) {
            continue;
        }
        const auto& image = texture_cache.PeekImage(id);
        // FindRenderTarget: MarkWritten (flag already set), UpdateImage (not Dirty, tracked whole),
        // the needs_rebind re-lookup; the backing is the one the view and layout were read from.
        if (False(image.flags & Flags::Registered) || False(image.flags & Flags::GpuModified) ||
            image.binding.needs_rebind || !VideoCore::TextureCache::UpdateIsNoop(image)) {
            return 8;
        }
        if (image.binding.is_bound) { // sampled by this draw: feedback loop, full path
            return 9;
        }
        if (image.backing != m.color_backing[cb]) {
            return 10;
        }
        if (!image.TransitIsNoop(kGbColorLayout, kGbColorAccess, kGbColorStage,
                                 cb_descs[cb].second.view_info.range)) {
            return 11;
        }
    }
    if (m.num_cb != 0 && m.color_id[0] && texture_cache.PeekImage(m.color_id[0]).usage.vo_surface) {
        return 12;
    }
    if (m.db_id) {
        const auto& image = texture_cache.PeekImage(m.db_id);
        if (False(image.flags & Flags::Registered) || False(image.flags & Flags::GpuModified) ||
            image.binding.needs_rebind || !VideoCore::TextureCache::UpdateIsNoop(image)) {
            return 13;
        }
        if (image.backing != m.db_backing) {
            return 14;
        }
        if (!image.TransitIsNoop(m.db_layout, m.db_access, m.db_stage, m.db_range)) {
            return 15;
        }
    }
    return 0;
}

/// The effects of BeginRendering for a draw GBufferFastCheck passed: the LRU touches (attachment
/// images, linked stencil image) and the usage flags; everything else it does is a no-op then.
void Rasterizer::GBufferFastApply() {
    const auto& m = gb_memo;
    attachment_feedback_loop = false;
    for (u32 cb = 0; cb < m.num_cb; ++cb) {
        if (m.color_id[cb]) {
            texture_cache.GetImage(m.color_id[cb]).usage.render_target = 1u;
        }
    }
    if (m.db_id) {
        texture_cache.GetImage(m.db_id);
        texture_cache.TouchLinked(m.stencil_id);
        texture_cache.PeekImage(m.db_id).usage.depth_target = true;
    }
}

/// Remembers what the full BeginRendering that just ran derived, unless the next draw could not
/// reuse it (it cleared, sampled one of its targets, ...).
void Rasterizer::GBufferFastRecord(const RenderState& state, const GraphicsPipeline* pipeline) {
    using Flags = VideoCore::ImageFlagBits;
    using DepthFastStats::c;
    auto& m = gb_memo;
    const auto reject = [&](u32 why) {
        m.valid = false;
        ++c.gb_norecord;
        ++c.gb_norec[why];
    };
    const auto& key = pipeline->GetGraphicsKey();
    const u32 num_cb = state.num_color_attachments;
    if (num_cb > AmdGpu::NUM_COLOR_BUFFERS) {
        return reject(2);
    }
    if (attachment_feedback_loop) {
        return reject(1);
    }
    const auto& att = state.depth_stencil_attachment;
    if (att.depth_clear || att.stencil_clear) {
        return reject(0);
    }
    for (u32 cb = 0; cb < num_cb; ++cb) {
        if (state.color_attachments[cb].is_clear) { // the clear bit was consumed by this draw
            return reject(0);
        }
        const auto id = cb_descs[cb].first;
        if (!id) {
            continue;
        }
        const auto& image = texture_cache.PeekImage(id);
        if (False(image.flags & Flags::Registered) || False(image.flags & Flags::GpuModified) ||
            image.binding.needs_rebind || image.binding.is_bound ||
            !VideoCore::TextureCache::UpdateIsNoop(image) ||
            !texture_cache.RenderTargetReplayable(image)) {
            return reject(2);
        }
        if (!image.backing || image.backing->num_samples != key.color_samples[cb]) {
            return reject(6);
        }
        if (!image.TransitIsNoop(kGbColorLayout, kGbColorAccess, kGbColorStage,
                                 cb_descs[cb].second.view_info.range)) {
            return reject(5);
        }
    }
    if (num_cb != 0 && cb_descs[0].first &&
        texture_cache.PeekImage(cb_descs[0].first).usage.vo_surface) {
        return reject(3);
    }
    if (db_desc.first) {
        const auto& image = texture_cache.PeekImage(db_desc.first);
        if (False(image.flags & Flags::Registered) || False(image.flags & Flags::GpuModified) ||
            image.binding.needs_rebind || !VideoCore::TextureCache::UpdateIsNoop(image)) {
            return reject(4);
        }
        if (!image.TransitIsNoop(df_transit_layout, df_transit_access, df_transit_stage,
                                 df_transit_range)) {
            return reject(5);
        }
    }
    m.valid = true;
    m.rel_render = liverpool->DrawRelSerial(AmdGpu::Liverpool::RelRender);
    m.rel_color = liverpool->DrawRelSerial(AmdGpu::Liverpool::RelColor);
    m.mrt_mask = key.mrt_mask;
    m.num_cb = num_cb;
    m.samples = key.color_samples;
    m.image_generation = texture_cache.ImageSetGeneration();
    m.gpu_epoch = VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed);
    m.meta_epoch = texture_cache.MetaEpoch();
    for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        const auto id = cb < num_cb ? cb_descs[cb].first : VideoCore::ImageId{};
        m.color_id[cb] = id;
        m.color_backing[cb] = id ? texture_cache.PeekImage(id).backing : nullptr;
    }
    m.db_id = db_desc.first;
    m.stencil_id = db_desc.first ? texture_cache.LastDepthStencil() : VideoCore::ImageId{};
    m.db_backing = db_desc.first ? texture_cache.PeekImage(db_desc.first).backing : nullptr;
    m.db_layout = df_transit_layout;
    m.db_access = df_transit_access;
    m.db_stage = df_transit_stage;
    m.db_range = df_transit_range;
    m.state = state;
    m.token = df_next_token++;
    ++c.gb_record;
}

const RenderState& Rasterizer::GBufferFast(const GraphicsPipeline* pipeline, RenderState& storage,
                                           u64& token) {
    using DepthFastStats::c;
    ++c.gb_cand;
    const auto texture_lock = texture_cache.BatchLock(); // the flags are read under the cache lock
    const u32 cause = GBufferFastCheck(pipeline);
    ++c.gb_cause[cause];
    if (cause != 0) {
        storage = BeginRendering(pipeline);
        GBufferFastRecord(storage, pipeline);
        return storage;
    }
    ++c.gb_hit;
    if (DepthFastStats::kGbCount) {
        // Count only: the full path runs; the replay it would have made must have given its state.
        storage = BeginRendering(pipeline);
        c.gb_count_bad += !(storage == gb_memo.state);
        return storage;
    }
    if (!DepthFastStats::kVerify) {
        GBufferFastApply();
        token = gb_memo.token;
        return gb_memo.state;
    }
    // Verify: replay, then derive in full on top. The full call must change nothing more (the
    // snapshot is the same), add no barrier and return the replayed state.
    struct Snap { // no padding: compared with memcmp
        struct Img {
            u32 flags, usage, binding, meta_mask;
            const void* backing;
            u64 layout, access, stage, subs, track_a, track_b, lru, tick_found;
        };
        Img img[9];
        u64 feedback, barrier, bound_n, hudless, pending, ids, lru_stencil;
    };
    const auto snap = [&] {
        Snap s;
        std::memset(&s, 0, sizeof(s));
        const auto& regs = liverpool->DrawRegs();
        const auto fill = [&](Snap::Img& o, VideoCore::ImageId id, VAddr meta) {
            const auto& image = texture_cache.PeekImage(id);
            o.flags = static_cast<u32>(image.flags);
            std::memcpy(&o.usage, &image.usage, sizeof(u32));
            std::memcpy(&o.binding, &image.binding, sizeof(u32));
            o.meta_mask = texture_cache.MetaClearMask(meta);
            o.backing = image.backing;
            o.layout = static_cast<u64>(image.backing->state.layout);
            o.access = static_cast<u64>(image.backing->state.access_mask);
            o.stage = static_cast<u64>(image.backing->state.pl_stage);
            o.subs = image.backing->subresource_states.size();
            o.track_a = image.track_addr;
            o.track_b = image.track_addr_end;
            o.lru = texture_cache.LruTickOf(id);
            o.tick_found = image.tick_accessed_last;
        };
        for (u32 cb = 0; cb < gb_memo.num_cb; ++cb) {
            const auto id = cb_descs[cb].first;
            s.ids = s.ids * 1099511628211ull + (id ? id.index : 0xFFFFu) + 1;
            if (id) {
                fill(s.img[cb], id, regs.color_buffers[cb].CmaskAddress());
            }
        }
        s.ids = s.ids * 1099511628211ull + (db_desc.first ? db_desc.first.index : 0xFFFFu) + 1;
        if (db_desc.first) {
            fill(s.img[8], db_desc.first, regs.depth_htile_data_base.GetAddress());
        }
        if (gb_memo.stencil_id) {
            s.lru_stencil = texture_cache.LruTickOf(gb_memo.stencil_id);
        }
        s.feedback = attachment_feedback_loop ? 1 : 0;
        s.barrier = needs_barrier ? 1 : 0;
        s.bound_n = bound_images.size();
        s.hudless = hudless_vo;
        s.pending = runtime.PendingImageBarriers();
        return s;
    };
    GBufferFastApply();
    const Snap before = snap();
    storage = BeginRendering(pipeline);
    const Snap after = snap();
    ++c.gb_verify;
    const bool same_state = storage == gb_memo.state;
    const bool same_snap = std::memcmp(&before, &after, sizeof(Snap)) == 0;
    if (!same_state || !same_snap) {
        if (++c.gb_bad <= 20) {
            int first_diff = -1;
            for (int i = 0; i < 9 && first_diff < 0; ++i) {
                if (std::memcmp(&before.img[i], &after.img[i], sizeof(Snap::Img)) != 0) {
                    first_diff = i;
                }
            }
            std::fprintf(stderr,
                         "SOTC GBFAST VERIFY: BeginRendering replay differs (mismatch %llu): state "
                         "%d snapshot %d first differing attachment %d (feedback %d/%d barrier "
                         "%d/%d bound %llu/%llu hudless %llx/%llx pending %llu/%llu ids %d)%c",
                         static_cast<unsigned long long>(c.gb_bad), int(same_state), int(same_snap),
                         first_diff, int(before.feedback), int(after.feedback), int(before.barrier),
                         int(after.barrier), static_cast<unsigned long long>(before.bound_n),
                         static_cast<unsigned long long>(after.bound_n),
                         static_cast<unsigned long long>(before.hudless),
                         static_cast<unsigned long long>(after.hudless),
                         static_cast<unsigned long long>(before.pending),
                         static_cast<unsigned long long>(after.pending), int(before.ids == after.ids),
                         10);
        }
    }
    return storage;
}

const RenderState& Rasterizer::BeginRenderingFast(const GraphicsPipeline* pipeline,
                                                  RenderState& storage, u64& token) {
    token = 0;
    using DepthFastStats::c;
    if (!DepthFastActive()) {
        storage = BeginRendering(pipeline);
        return storage;
    }
    DepthFastStats::NoteDraw();
    if (pipeline->GetGraphicsKey().mrt_mask != 0) {
        if (BbToggle::Disabled(BbToggle::GBufferFastPath)) {
            storage = BeginRendering(pipeline);
            return storage;
        }
        return GBufferFast(pipeline, storage, token);
    }
    ++c.begin_cand;
    const auto texture_lock = texture_cache.BatchLock(); // the flags are read under the cache lock
    const u32 cause = DepthFastBeginCheck(pipeline);
    ++c.cause[cause];
    if (cause == 2 && DepthFastStats::kWhy) {
        const auto& regs = liverpool->DrawRegs();
        const auto differs = [](const auto& a, const auto& b) {
            return std::memcmp(&a, &b, sizeof(a)) != 0;
        };
        c.why[0] += differs(df_snap.depth_control, regs.depth_control);
        c.why[1] += differs(df_snap.depth_buffer, regs.depth_buffer);
        c.why[2] += differs(df_snap.depth_render_control, regs.depth_render_control);
        c.why[3] += differs(df_snap.color_control, regs.color_control);
        c.why[4] += differs(df_snap.depth_view, regs.depth_view);
        c.why[5] += df_snap.htile != regs.depth_htile_data_base.address;
        c.why[6] += df_snap.depth_clear != std::bit_cast<u32>(regs.depth_clear);
        c.why[7] += df_snap.stencil_clear != regs.stencil_clear;
        c.why[8] += df_snap.db_extent != liverpool->DrawDbExtent().raw;
    }
    if (cause == 0) {
        ++c.begin_hit;
        if (!DepthFastStats::kVerify) {
            DepthFastBeginApply();
            token = df_memo.token;
            return df_memo.state;
        }
        // Verify: replay, then derive in full on top. The full call must change nothing more (the
        // snapshot is the same), add no barrier and return the replayed state.
        struct Snap { // no padding: compared with memcmp
            u32 flags, usage, binding, meta_mask, feedback, barrier;
            const void* backing;
            u64 layout, access, stage;
            u64 subs;
            u64 track_a, track_b, lru_image, lru_stencil, tick_found;
            u64 pending;
        };
        const auto snap = [&] {
            const auto& image = texture_cache.PeekImage(df_memo.db_id);
            Snap s{};
            s.flags = static_cast<u32>(image.flags);
            std::memcpy(&s.usage, &image.usage, sizeof(u32));
            std::memcpy(&s.binding, &image.binding, sizeof(u32));
            s.backing = image.backing;
            s.layout = static_cast<u64>(image.backing->state.layout);
            s.access = static_cast<u64>(image.backing->state.access_mask);
            s.stage = static_cast<u64>(image.backing->state.pl_stage);
            s.subs = image.backing->subresource_states.size();
            s.track_a = image.track_addr;
            s.track_b = image.track_addr_end;
            s.lru_image = texture_cache.LruTickOf(df_memo.db_id);
            s.lru_stencil = df_memo.stencil_id ? texture_cache.LruTickOf(df_memo.stencil_id) : 0;
            s.tick_found = image.tick_accessed_last;
            s.meta_mask =
                texture_cache.MetaClearMask(liverpool->DrawRegs().depth_htile_data_base.GetAddress());
            s.feedback = attachment_feedback_loop ? 1 : 0;
            s.barrier = needs_barrier ? 1 : 0;
            s.pending = runtime.PendingImageBarriers();
            return s;
        };
        DepthFastBeginApply();
        const Snap before = snap();
        storage = BeginRendering(pipeline);
        const Snap after = snap();
        ++c.begin_verify;
        const bool same_state = storage == df_memo.state;
        const bool same_snap = std::memcmp(&before, &after, sizeof(Snap)) == 0;
        if (!same_state || !same_snap) {
            if (++c.begin_bad <= 20) {
                std::fprintf(stderr,
                             "SOTC DEPTHFAST VERIFY: BeginRendering replay differs (mismatch %llu): "
                             "state %d snapshot %d (flags %u/%u binding %u/%u layout %llu/%llu "
                             "access %llx/%llx subs %llu/%llu lru %llu/%llu stencil %llu/%llu meta "
                             "%u/%u feedback %d/%d barrier %d/%d pending %llu/%llu)%c",
                             static_cast<unsigned long long>(c.begin_bad), int(same_state),
                             int(same_snap), before.flags, after.flags, before.binding,
                             after.binding, static_cast<unsigned long long>(before.layout),
                             static_cast<unsigned long long>(after.layout),
                             static_cast<unsigned long long>(before.access),
                             static_cast<unsigned long long>(after.access),
                             static_cast<unsigned long long>(before.subs),
                             static_cast<unsigned long long>(after.subs), static_cast<unsigned long long>(before.lru_image),
                             static_cast<unsigned long long>(after.lru_image),
                             static_cast<unsigned long long>(before.lru_stencil),
                             static_cast<unsigned long long>(after.lru_stencil), before.meta_mask,
                             after.meta_mask, int(before.feedback), int(after.feedback),
                             int(before.barrier), int(after.barrier),
                             static_cast<unsigned long long>(before.pending),
                             static_cast<unsigned long long>(after.pending), 10);
            }
        }
        return storage;
    }
    storage = BeginRendering(pipeline);
    DepthFastRecord(storage);
    return storage;
}

RenderState Rasterizer::BeginRendering(const GraphicsPipeline* pipeline) {
    const auto texture_lock = texture_cache.BatchLock(); // sotc: one lock for the attachments
    DrawPhase::SubBeg(DrawPhase::SBegLock);
    attachment_feedback_loop = false;
    // sotc: SOTC_RT_FAST (default on) drops redundant attachment work: FindRenderTarget repeats
    // UpdateImage only when the image still needs it, and the CMASK/HTILE clear bit is read and
    // cleared with one lookup. SOTC_RT_FAST=0 restores the original calls and evaluation order.
    static const bool rt_fast = [] {
        const char* v = std::getenv("SOTC_RT_FAST");
        return !v || v[0] != '0';
    }();
    const auto& regs = liverpool->DrawRegs();
    const auto& key = pipeline->GetGraphicsKey();
    RenderState state;
    state.width = instance.GetMaxFramebufferWidth();
    state.height = instance.GetMaxFramebufferHeight();
    state.num_layers = std::numeric_limits<u16>::max();
    state.num_color_attachments = std::bit_width(key.mrt_mask);
    DrawPhase::LapStart();
    for (auto cb = 0u; cb < state.num_color_attachments; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            state.color_attachments[cb] = {};
            continue;
        }
        auto* image = &texture_cache.GetImage(image_id);
        if (image->binding.needs_rebind) {
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
            image = &texture_cache.GetImage(image_id);
        }
        texture_cache.UpdateImage(image_id);
        DrawPhase::Lap(0);
        runtime.SetBackingSamples(image, key.color_samples[cb]);
        DrawPhase::Lap(1);
        const auto& image_view = texture_cache.FindRenderTarget(image_id, desc, !rt_fast);
        DrawPhase::Lap(2);
        const auto slice = image_view.info.range.base.layer;
        const auto mip = image_view.info.range.base.level;

        const auto& col_buf = regs.color_buffers[cb];
        // sotc: SOTC_RT_FAST: test-and-clear the CMASK bit with one lookup.
        bool is_clear;
        if (rt_fast) {
            is_clear = texture_cache.TestAndTouchMeta(col_buf.CmaskAddress(), slice);
        } else {
            is_clear = texture_cache.IsMetaCleared(col_buf.CmaskAddress(), slice);
            texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
        }
        DrawPhase::Lap(3);

        if (image->binding.is_bound) {
            ASSERT_MSG(!image->binding.force_general,
                       "Having image both as storage and render target is unsupported");
            runtime.FlushBarriers();
            needs_barrier |=
                runtime.Transit(image,
                                instance.IsAttachmentFeedbackLoopLayoutSupported()
                                    ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                                    : vk::ImageLayout::eGeneral,
                                vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                vk::AccessFlagBits2::eColorAttachmentWrite |
                                    vk::AccessFlagBits2::eColorAttachmentRead);
            attachment_feedback_loop = true;
        } else {
            needs_barrier |= runtime.Transit(image, vk::ImageLayout::eColorAttachmentOptimal,
                                             vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                             vk::AccessFlagBits2::eColorAttachmentWrite |
                                                 vk::AccessFlagBits2::eColorAttachmentRead,
                                             desc.view_info.range);
        }

        DrawPhase::Lap(4);
        state.width = std::min<u32>(state.width, std::max(image->info.size.width >> mip, 1u));
        state.height = std::min<u32>(state.height, std::max(image->info.size.height >> mip, 1u));
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        const auto clear_value =
            is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf) : vk::ClearValue{};
        auto& attachment = state.color_attachments[cb];
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image->backing->state.layout;
        attachment.clear_value = clear_value.color.uint32;
        attachment.is_clear = is_clear;

        image->usage.render_target = 1u;
        if (cb == 0 && image->usage.vo_surface) { // sotc: TakeHudless
            hudless_vo = image->info.guest_address;
            hudless_vo_format = image_view.info.format;
            hudless_vo_width = image->info.size.width;
            hudless_vo_height = image->info.size.height;
        }
    }
    DrawPhase::Lap(5);
    for (u32 cb = state.num_color_attachments; cb < state.color_attachments.size(); ++cb) {
        state.color_attachments[cb] = {};
    }

    if (auto image_id = db_desc.first; image_id) {
        auto& desc = db_desc.second;
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& image_view = texture_cache.FindDepthTarget(image_id, desc);
        auto& image = texture_cache.GetImage(image_id);
        DrawPhase::SubBeg(DrawPhase::SBegFind);

        const auto slice = image_view.info.range.base.layer;
        const bool depth_reg_clear = regs.depth_render_control.depth_clear_enable &&
                                     regs.depth_control.depth_enable &&
                                     regs.depth_control.depth_write_enable;
        // sotc: SOTC_RT_FAST: test-and-clear the HTILE bit with one lookup. It is evaluated
        // unconditionally (no short-circuit) because the original TouchMeta always cleared it.
        bool is_depth_clear;
        if (rt_fast) {
            const bool meta_clear = texture_cache.TestAndTouchMeta(htile_address, slice);
            is_depth_clear = depth_reg_clear || meta_clear;
        } else {
            is_depth_clear = depth_reg_clear || texture_cache.IsMetaCleared(htile_address, slice);
        }
        const bool is_stencil_clear = regs.depth_render_control.stencil_clear_enable;
        if (!rt_fast) {
            texture_cache.TouchMeta(htile_address, slice, false);
        }
        DrawPhase::SubBeg(DrawPhase::SBegMeta);
        ASSERT(desc.view_info.range.extent.levels == 1 && !image.binding.needs_rebind);

        const bool has_stencil = image.info.props.has_stencil;
        // Stencil writes can be enabled while depth writes are off.
        const bool stencil_write =
            has_stencil && regs.depth_control.stencil_enable && !desc.view_info.is_storage;
        const auto new_layout = desc.view_info.is_storage
                                    ? has_stencil ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                                  : vk::ImageLayout::eDepthAttachmentOptimal
                                : stencil_write
                                    ? vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal
                                : has_stencil ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                              : vk::ImageLayout::eDepthReadOnlyOptimal;
        // sotc: DepthFastPath replays this transition's arguments (BeginRenderingFast).
        // sotc: SOTC_DEPTH_RO (upstream): the stencil-write layout asks for the stages and accesses
        // the sampling of its depth aspect asks for (BindResolvedImage), so the state stays put.
        static const bool depth_ro = [] {
            const char* v = std::getenv("SOTC_DEPTH_RO");
            return !v || v[0] != '0';
        }();
        df_transit_layout = new_layout;
        if (depth_ro && new_layout == vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal) {
            df_transit_stage = kDepthReadOnlyStencilStages;
            df_transit_access = kDepthReadOnlyStencilAccess;
        } else {
            df_transit_stage = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                               vk::PipelineStageFlagBits2::eLateFragmentTests;
            df_transit_access = vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                                vk::AccessFlagBits2::eDepthStencilAttachmentRead;
        }
        df_transit_range = desc.view_info.range;
        needs_barrier |= runtime.Transit(&image, new_layout, df_transit_stage, df_transit_access,
                                         desc.view_info.range);

        state.width = std::min<u32>(state.width, image.info.size.width);
        state.height = std::min<u32>(state.height, image.info.size.height);
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        auto& attachment = state.depth_stencil_attachment;
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image.backing->state.layout;
        attachment.clear_value = {};
        attachment.is_clear = 0;

        if (regs.depth_buffer.DepthValid()) {
            attachment.clear_value[0] = is_depth_clear ? std::bit_cast<u32>(regs.depth_clear) : 0u;
            attachment.has_depth = true;
            attachment.depth_clear = is_depth_clear;
        }
        if (regs.depth_buffer.StencilValid()) {
            attachment.clear_value[1] = is_stencil_clear ? regs.stencil_clear : 0u;
            attachment.has_stencil = true;
            attachment.stencil_clear = is_stencil_clear;
        }

        image.usage.depth_target = true;
    } else {
        state.depth_stencil_attachment = {};
    }

    if (state.num_layers == std::numeric_limits<u16>::max()) {
        state.num_layers = 1;
    }

    DrawPhase::Lap(6);
    DrawPhase::SubBeg(DrawPhase::SBegTrans);
    return state;
}

void Rasterizer::Resolve() {
    const auto& mrt0_hint = liverpool->DrawCbExtent()[0];
    const auto& mrt1_hint = liverpool->DrawCbExtent()[1];
    VideoCore::TextureCache::ImageDesc mrt0_desc{liverpool->DrawRegs().color_buffers[0], mrt0_hint};
    VideoCore::TextureCache::ImageDesc mrt1_desc{liverpool->DrawRegs().color_buffers[1], mrt1_hint};
    auto& mrt0_image = texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, true));
    auto& mrt1_image = texture_cache.GetImage(texture_cache.FindImage(mrt1_desc, true));

    ScopeMarkerBegin(fmt::format("Resolve:MRT0={:#x}:MRT1={:#x}",
                                 liverpool->DrawRegs().color_buffers[0].Address(),
                                 liverpool->DrawRegs().color_buffers[1].Address()));
    runtime.ResolveImage(&mrt0_image, &mrt1_image, mrt0_desc.view_info.range,
                         mrt1_desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::DepthStencilCopy(bool is_depth, bool is_stencil) {
    auto& regs = liverpool->DrawRegs();
    if (SotcOps::kOn) [[unlikely]] { // sotc: SOTC_OP_STATS=1
        SotcOps::NoteTouch(SotcOps::TouchCopySrc, 0, 0, false, regs.depth_buffer.DepthAddress(), 1);
        SotcOps::NoteTouch(SotcOps::TouchCopyDst, 0, 0, true, regs.depth_buffer.DepthWriteAddress(),
                           1);
    }

    auto read_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->DrawDbExtent(), false);
    auto write_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->DrawDbExtent(), true);

    auto& read_image = texture_cache.GetImage(texture_cache.FindImage(read_desc));
    auto& write_image = texture_cache.GetImage(texture_cache.FindImage(write_desc));

    VideoCore::SubresourceRange sub_range;
    sub_range.base.layer = liverpool->DrawRegs().depth_view.slice_start;
    sub_range.extent.layers = liverpool->DrawRegs().depth_view.NumSlices() - sub_range.base.layer;

    ScopeMarkerBegin(fmt::format(
        "DepthStencilCopy:DR={:#x}:SR={:#x}:DW={:#x}:SW={:#x}", regs.depth_buffer.DepthAddress(),
        regs.depth_buffer.StencilAddress(), regs.depth_buffer.DepthWriteAddress(),
        regs.depth_buffer.StencilWriteAddress()));

    runtime.CopyDepthStencil(&read_image, &write_image, sub_range);

    ScopeMarkerEnd();
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    ASSERT_MSG(address % 4 == 0 && num_bytes % 4 == 0,
               "FillBuffer address and size must be a multiple of 4 bytes");
    if (SotcOps::kOn && !is_gds) [[unlikely]] { // sotc: SOTC_OP_STATS=1
        SotcOps::NoteTouch(SotcOps::TouchFill, 0, 0, true, address, num_bytes);
    }
    if (!is_gds) {
        texture_cache.ClearMeta(address);
        if (!buffer_cache.IsRegionGpuModified(address, num_bytes)) {
            u32* buffer = std::bit_cast<u32*>(address);
            std::fill(buffer, buffer + (num_bytes / sizeof(u32)), value);
            return;
        }
    }
    const auto [buffer, offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (is_gds) {
            return {buffer_cache.GetGdsBuffer(), address};
        }
        return buffer_cache.ObtainBuffer(address, num_bytes, true);
    }();
    runtime.FillBuffer(buffer, offset, num_bytes, value);
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    if (SotcOps::kOn) [[unlikely]] { // sotc: SOTC_OP_STATS=1
        if (!src_gds) {
            SotcOps::NoteTouch(SotcOps::TouchCopySrc, 0, 0, false, src, num_bytes);
        }
        if (!dst_gds) {
            SotcOps::NoteTouch(SotcOps::TouchCopyDst, 0, 0, true, dst, num_bytes);
        }
    }
    if (!dst_gds && !buffer_cache.IsRegionGpuModified(dst, num_bytes)) {
        if (!src_gds && !buffer_cache.IsRegionGpuModified(src, num_bytes) &&
            !texture_cache.FindImageFromRange(src, num_bytes)) {
            // Both buffers were not transferred to GPU yet. Can safely copy in host memory.
            std::memcpy(std::bit_cast<void*>(dst), std::bit_cast<void*>(src), num_bytes);
            return;
        }
    }
    texture_cache.InvalidateMemoryFromGPU(dst, num_bytes);
    const auto* gds_buffer = buffer_cache.GetGdsBuffer();
    const auto [src_buffer, src_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (src_gds) {
            return {gds_buffer, src};
        }
        return buffer_cache.ObtainBuffer(src, num_bytes, false, true);
    }();
    const auto [dst_buffer, dst_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (dst_gds) {
            return {gds_buffer, dst};
        }
        return buffer_cache.ObtainBuffer(dst, num_bytes, true, true);
    }();
    const vk::BufferCopy copy = {
        .srcOffset = src_offset,
        .dstOffset = dst_offset,
        .size = num_bytes,
    };
    runtime.CopyBuffer(src_buffer, dst_buffer, std::span{&copy, 1});
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    auto* gds_buf = buffer_cache.GetGdsBuffer();
    u32 value;
    std::memcpy(&value, gds_buf->mapped_data.data() + gds_offset, sizeof(u32));
    return value;
}

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size, bool assume_locks) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.InvalidateMemory(addr, size, assume_locks);
    texture_cache.InvalidateMemory(addr, size);
    return true;
}

// sotc: a guest (not command processor) write fault. With the repeat-fault grace on, the texture
// cache goes first so the page becomes writable last, after any wait on the texture mutex (the GPU
// thread then has less time to protect it again before the guest retries its write).
bool Rasterizer::InvalidateMemoryOnGuestFault(VAddr addr, u64 size) {
    if (SotcOps::kOn) [[unlikely]] { // sotc: SOTC_OP_STATS=1
        SotcOps::NoteCpuAccess(true, addr, size);
    }
    if (!IsMapped(addr, size)) {
        return false;
    }
    if (VideoCore::FaultGrace::Pages() != 0) {
        texture_cache.InvalidateMemory(addr, size);
        buffer_cache.InvalidateMemory(addr, size, false, true);
    } else {
        buffer_cache.InvalidateMemory(addr, size, false);
        texture_cache.InvalidateMemory(addr, size);
    }
    return true;
}

bool Rasterizer::ReadMemory(VAddr addr, u64 size, bool assume_locks) {
    if (SotcOps::kOn) [[unlikely]] { // sotc: SOTC_OP_STATS=1
        SotcOps::NoteCpuAccess(false, addr, size);
    }
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.ReadMemory(addr, size, false, assume_locks);
    return true;
}

bool Rasterizer::IsMapped(VAddr addr, u64 size) {
    if (size == 0) {
        // There is no memory, so not mapped.
        return false;
    }
    if (static_cast<u64>(addr) > std::numeric_limits<u64>::max() - size) {
        // Memory range wrapped the address space, cannot be mapped.
        return false;
    }
    const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);

    Common::RecursiveSharedLock lock{mapped_ranges_mutex};
    return boost::icl::contains(mapped_ranges, range);
}

void Rasterizer::MapMemory(VAddr addr, u64 size) {
    buffer_cache.OnMappingChanged();
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges += decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::RegisterMemory(VAddr addr, u64 size) {
    page_manager.OnGpuMap(addr, size);
}

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
    buffer_cache.OnMappingChanged();
    // sotc: an unmapped range forgets its GPU-written state instead of downloading it (a 512 KB
    // window around it) into memory that is being given back: with texture streaming that
    // readback broke the game's heap within a minute of loading a save (allocator fault at guest
    // 0x1a371, garbage textures). SOTC_UNMAP_READBACK=1 restores the readback.
    static const bool unmap_readback = [] {
        const char* v = std::getenv("SOTC_UNMAP_READBACK");
        return v && v[0] == '1';
    }();
    if (unmap_readback) {
        buffer_cache.InvalidateMemory(addr, size);
    } else {
        buffer_cache.DropGpuModified(addr, size);
    }
    // Give the arena memory back; it runs on the GPU thread, ordered before later draws.
    // sotc: on by default since 2026-10-16 (SOTC_ARENA_RELEASE=0: never released). It was
    // opt-in because it raced with in-flight GPU work during loads (device lost loading a save,
    // seen before ReleaseMemory finished the GPU work first). Never releasing grew video memory
    // ~90 MB a minute in the attract demo (1.6 -> 6.0 GB in 48 minutes on a 6 GB card) until an
    // arena allocation failed (ASSERT). Released, the 25-minute curve stays ~0.7-0.9 GB lower;
    // no device loss in that run nor in three shrine runs that each load a save.
    static const bool release_enabled = [] {
        const char* v = std::getenv("SOTC_ARENA_RELEASE");
        return !v || v[0] != '0';
    }();
    if (release_enabled) {
        // sotc: ordered at the command processor's point of the stream: with the draw thread a
        // guest-thread command would run ahead of the draws already queued, which may still use
        // the range (review of #99).
        liverpool->SendOrderedCommand(
            [this, addr, size] { buffer_cache.ReleaseMemory(addr, size); });
    }
    texture_cache.UnmapMemory(addr, size);
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges -= decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::UpdateDynamicStateValues(const GraphicsPipeline* pipeline,
                                          const bool is_indexed) const {
    UpdateViewportScissorState();
    UpdateDepthStencilState();
    UpdatePrimitiveState(is_indexed);
    UpdateRasterizationState();
    UpdateColorBlendingState(pipeline);
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed) const {
    UpdateDynamicStateValues(pipeline, is_indexed);
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.Commit(instance, scheduler.CommandBuffer());
}

// sotc: BbToggle::DrawFixedCostOpt. The dynamic state object keeps the values derived for the
// last draw (the setters compare against them and Invalidate only marks them dirty again), so a
// draw whose inputs are those of the last derivation can go straight to Commit. The inputs are
// the registers Update*State read (their chunks, tracked by value on the draw thread:
// Liverpool::DrawChunksChangedSince), the pipeline's colour write masks and the feedback loop
// flag of this draw. SOTC_DRAW_OPT_VERIFY=1 derives anyway on every hit and reports a value that
// the memo would have kept stale.
// sotc: BbToggle::DepthFastPath. A draw whose dynamic state inputs are those of the last derivation
// (nothing the Update*State functions read changed in value: Liverpool::DrawRelSerial; the
// pipeline's write masks, the feedback loop flag and indexed-ness are the same) commits what the
// dynamic state object already holds; any other goes the old way and remembers its inputs.
void Rasterizer::CommitDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed) {
    if (!DepthFastActive()) {
        df_dyn.valid = false; // a derivation that does not go through the memo changes the values
        CommitDynamicStateOld(pipeline, is_indexed);
        return;
    }
    using DepthFastStats::c;
    const auto& key = pipeline->GetGraphicsKey();
    const u32 serial = liverpool->DrawRelSerial(AmdGpu::Liverpool::RelDynamic);
    if (df_dyn.valid && df_dyn.rel_serial == serial &&
        df_dyn.feedback_loop == attachment_feedback_loop && df_dyn.is_indexed == is_indexed &&
        df_dyn.write_masks == key.write_masks) {
        ++c.dyn_hit;
        auto& dynamic_state = scheduler.GetDynamicState();
        if (DepthFastStats::kVerify) {
            // Derive again with every dirty bit clear: a bit that comes up is a value the replay
            // would have left behind. The bits are merged back so the state stays right.
            const auto saved = dynamic_state.dirty_state;
            std::memset(&dynamic_state.dirty_state, 0, sizeof(dynamic_state.dirty_state));
            UpdateDynamicStateValues(pipeline, is_indexed);
            u8 now[sizeof(saved)], old[sizeof(saved)];
            std::memcpy(now, &dynamic_state.dirty_state, sizeof(now));
            std::memcpy(old, &saved, sizeof(old));
            bool stale = false;
            for (size_t i = 0; i < sizeof(now); ++i) {
                stale |= now[i] != 0;
                now[i] |= old[i];
            }
            std::memcpy(&dynamic_state.dirty_state, now, sizeof(now));
            ++c.dyn_verify;
            if (stale && ++c.dyn_bad <= 20) {
                std::fprintf(stderr,
                             "SOTC DEPTHFAST VERIFY: dynamic state replay kept a stale value "
                             "(mismatch %llu)%c",
                             static_cast<unsigned long long>(c.dyn_bad), 10);
            }
        }
        dynamic_state.Commit(instance, scheduler.CommandBuffer());
        return;
    }
    ++c.dyn_miss;
    CommitDynamicStateOld(pipeline, is_indexed);
    df_dyn.valid = true;
    df_dyn.rel_serial = serial;
    df_dyn.feedback_loop = attachment_feedback_loop;
    df_dyn.is_indexed = is_indexed;
    df_dyn.write_masks = key.write_masks;
}

void Rasterizer::CommitDynamicStateOld(const GraphicsPipeline* pipeline, const bool is_indexed) {
    static const bool verify = std::getenv("SOTC_DRAW_OPT_VERIFY") != nullptr;
    if (BbToggle::Disabled(BbToggle::DrawFixedCostOpt) || !liverpool->IsBackThreaded() ||
        !liverpool->OnBackThread()) {
        dyn_memo.valid = false;
        UpdateDynamicState(pipeline, is_indexed);
        return;
    }
    if (dyn_chunks.empty()) {
        const auto& regs = liverpool->DrawRegs();
        const u32* base = regs.reg_array.data();
        const auto add = [&](const void* field, size_t bytes) {
            const u32 first = static_cast<u32>(reinterpret_cast<const u32*>(field) - base);
            const u32 last = first + static_cast<u32>((bytes + 3) / 4) - 1;
            dyn_fields.push_back({first * 4, static_cast<u32>(bytes)});
            dyn_snap.resize(dyn_snap.size() + bytes);
            for (u32 chunk = first / 64; chunk <= last / 64; ++chunk) {
                if (std::find(dyn_chunks.begin(), dyn_chunks.end(), chunk) == dyn_chunks.end()) {
                    dyn_chunks.push_back(static_cast<u16>(chunk));
                }
            }
        };
        // Everything Update{ViewportScissor,DepthStencil,Primitive,Rasterization,ColorBlending}State
        // and the helpers they call (IsClipDisabled, NeedsBias, DepthValid, ...) read.
        add(&regs.screen_scissor, sizeof(regs.screen_scissor));
        add(&regs.window_scissor, sizeof(regs.window_scissor));
        add(&regs.generic_scissor, sizeof(regs.generic_scissor));
        add(&regs.window_offset, sizeof(regs.window_offset));
        add(&regs.polygon_control, sizeof(regs.polygon_control));
        add(&regs.viewport_control, sizeof(regs.viewport_control));
        add(&regs.viewports, sizeof(regs.viewports));
        add(&regs.viewport_depths, sizeof(regs.viewport_depths));
        add(&regs.clipper_control, sizeof(regs.clipper_control));
        add(&regs.mode_control, sizeof(regs.mode_control));
        add(&regs.viewport_scissors, sizeof(regs.viewport_scissors));
        add(&regs.depth_control, sizeof(regs.depth_control));
        add(&regs.depth_buffer, sizeof(regs.depth_buffer));
        add(&regs.depth_render_control, sizeof(regs.depth_render_control));
        add(&regs.depth_bounds_min, sizeof(regs.depth_bounds_min));
        add(&regs.depth_bounds_max, sizeof(regs.depth_bounds_max));
        add(&regs.poly_offset, sizeof(regs.poly_offset));
        add(&regs.stencil_control, sizeof(regs.stencil_control));
        add(&regs.stencil_ref_front, sizeof(regs.stencil_ref_front));
        add(&regs.stencil_ref_back, sizeof(regs.stencil_ref_back));
        add(&regs.enable_primitive_restart, sizeof(regs.enable_primitive_restart));
        add(&regs.primitive_type, sizeof(regs.primitive_type));
        add(&regs.primitive_restart_index, sizeof(regs.primitive_restart_index));
        add(&regs.line_control, sizeof(regs.line_control));
        add(&regs.blend_constants, sizeof(regs.blend_constants));
    }
    const auto& key = pipeline->GetGraphicsKey();
    const auto& live = liverpool->DrawRegs();
    const u8* live_bytes = reinterpret_cast<const u8*>(live.reg_array.data());
    bool hit = dyn_memo.valid && dyn_memo.feedback_loop == attachment_feedback_loop &&
               dyn_memo.write_masks == key.write_masks;
    if (hit && liverpool->DrawChunksChangedSince(dyn_chunks.data(), dyn_chunks.size(),
                                                 dyn_memo.serial)) {
        // A chunk the inputs live in carried changes (a chunk holds up to 64 registers): the
        // inputs themselves decide.
        size_t at = 0;
        for (const auto& [offset, bytes] : dyn_fields) {
            if (std::memcmp(live_bytes + offset, dyn_snap.data() + at, bytes) != 0) {
                hit = false;
                break;
            }
            at += bytes;
        }
        if (hit) {
            dyn_memo.serial = liverpool->DrawRegSerial();
        }
    }
    if (hit) {
        auto& dynamic_state = scheduler.GetDynamicState();
        if (verify) {
            // Derive again with every dirty bit clear: a bit that comes up is a value the memo
            // would have left behind. The bits are merged back so the state stays right.
            const auto saved = dynamic_state.dirty_state;
            std::memset(&dynamic_state.dirty_state, 0, sizeof(dynamic_state.dirty_state));
            UpdateDynamicStateValues(pipeline, is_indexed);
            u8 now[sizeof(saved)], old[sizeof(saved)];
            std::memcpy(now, &dynamic_state.dirty_state, sizeof(now));
            std::memcpy(old, &saved, sizeof(old));
            bool stale = false;
            for (size_t i = 0; i < sizeof(now); ++i) {
                stale |= now[i] != 0;
                now[i] |= old[i];
            }
            std::memcpy(&dynamic_state.dirty_state, now, sizeof(now));
            static std::atomic<u64> mismatches{0}, hits{0};
            hits.fetch_add(1, std::memory_order_relaxed);
            if (stale) {
                const u64 n = mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n <= 20) {
                    std::fprintf(stderr, "SOTC DRAW OPT VERIFY: dynamic state memo kept a stale "
                                         "value (mismatch %llu of %llu hits)\n",
                                 static_cast<unsigned long long>(n),
                                 static_cast<unsigned long long>(hits.load()));
                }
            } else if ((hits.load() & 0xFFFFF) == 0) {
                std::fprintf(stderr, "SOTC DRAW OPT VERIFY: dynamic state memo ok, %llu hits, %llu "
                                     "mismatches\n",
                             static_cast<unsigned long long>(hits.load()),
                             static_cast<unsigned long long>(mismatches.load()));
            }
        }
        DrawPhase::Sub(DrawPhase::SDynCheck);
        dynamic_state.Commit(instance, scheduler.CommandBuffer());
        DrawPhase::Sub(DrawPhase::SDynCommit);
        if (DrawPhase::kOn) {
            DrawPhase::dyn_hits.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    if (DrawPhase::kOn) {
        DrawPhase::dyn_misses.fetch_add(1, std::memory_order_relaxed);
    }
    {
        size_t at = 0;
        for (const auto& [offset, bytes] : dyn_fields) {
            std::memcpy(dyn_snap.data() + at, live_bytes + offset, bytes);
            at += bytes;
        }
    }
    dyn_memo.valid = true;
    dyn_memo.feedback_loop = attachment_feedback_loop;
    dyn_memo.write_masks = key.write_masks;
    dyn_memo.serial = liverpool->DrawRegSerial();
    UpdateDynamicState(pipeline, is_indexed);
}

void Rasterizer::UpdateViewportScissorState() const {
    const auto& regs = liverpool->DrawRegs();

    const auto combined_scissor_value_tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const auto combined_scissor_value_br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable;

    AmdGpu::Scissor scsr{};
    scsr.top_left_x = combined_scissor_value_tl(
        regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x),
        s16(regs.generic_scissor.top_left_x),
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = combined_scissor_value_tl(
        regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y),
        s16(regs.generic_scissor.top_left_y),
        enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_x,
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
        regs.generic_scissor.bottom_right_y,
        enable_offset ? regs.window_offset.window_y_offset : 0);

    boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS> viewports;
    boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS> scissors;

    if (regs.polygon_control.enable_window_offset &&
        (regs.window_offset.window_x_offset != 0 || regs.window_offset.window_y_offset != 0)) {
        LOG_ERROR(Render_Vulkan,
                  "PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE support is not yet implemented.");
    }

    const auto& vp_ctl = regs.viewport_control;
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; i++) {
        const auto& vp = regs.viewports[i];
        const auto& vp_d = regs.viewport_depths[i];
        if (vp.xscale == 0) {
            continue;
        }

        const auto zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
        const auto zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;

        vk::Viewport viewport{};

        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_pipeline_graphics.c#L688-689
        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_cmd_buffer.c#L3103-3109
        // When the clip space is ranged [-1...1], the zoffset is centered.
        // By reversing the above viewport calculations, we get the following:
        if (regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
            viewport.minDepth = zoffset - zscale;
            viewport.maxDepth = zoffset + zscale;
        } else {
            viewport.minDepth = zoffset;
            viewport.maxDepth = zoffset + zscale;
        }

        if (!instance.IsDepthRangeUnrestrictedSupported()) {
            // Unrestricted depth range not supported by device. Restrict to valid range.
            viewport.minDepth = std::max(viewport.minDepth, 0.f);
            viewport.maxDepth = std::min(viewport.maxDepth, 1.f);
        }

        if (regs.IsClipDisabled()) {
            // In case if clipping is disabled we patch the shader to convert vertex position
            // from screen space coordinates to NDC by defining a render space as full hardware
            // window range [0..16383, 0..16383] and setting the viewport to its size.
            viewport.x = 0.f;
            viewport.y = 0.f;
            viewport.width = float(std::min<u32>(instance.GetMaxViewportWidth(), 16_KB));
            viewport.height = float(std::min<u32>(instance.GetMaxViewportHeight(), 16_KB));
        } else {
            const auto xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
            const auto xscale = vp_ctl.xscale_enable ? vp.xscale : 1.f;
            const auto yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
            const auto yscale = vp_ctl.yscale_enable ? vp.yscale : 1.f;

            viewport.x = xoffset - xscale;
            viewport.y = yoffset - yscale;
            viewport.width = xscale * 2.0f;
            viewport.height = yscale * 2.0f;
        }

        viewports.push_back(viewport);

        auto vp_scsr = scsr;
        if (regs.mode_control.vport_scissor_enable) {
            vp_scsr.top_left_x =
                std::max(vp_scsr.top_left_x, s16(regs.viewport_scissors[i].top_left_x));
            vp_scsr.top_left_y =
                std::max(vp_scsr.top_left_y, s16(regs.viewport_scissors[i].top_left_y));
            vp_scsr.bottom_right_x = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_x),
                                              regs.viewport_scissors[i].bottom_right_x);
            vp_scsr.bottom_right_y = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_y),
                                              regs.viewport_scissors[i].bottom_right_y);
        }
        scissors.push_back({
            .offset = {vp_scsr.top_left_x, vp_scsr.top_left_y},
            .extent = {vp_scsr.GetWidth(), vp_scsr.GetHeight()},
        });
    }

    if (viewports.empty()) {
        // Vulkan requires providing at least one viewport.
        constexpr vk::Viewport empty_viewport = {
            .x = -1.0f,
            .y = -1.0f,
            .width = 1.0f,
            .height = 1.0f,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        constexpr vk::Rect2D empty_scissor = {
            .offset = {0, 0},
            .extent = {1, 1},
        };
        viewports.push_back(empty_viewport);
        scissors.push_back(empty_scissor);
    }

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetViewports(viewports);
    dynamic_state.SetScissors(scissors);
}

void Rasterizer::UpdateDepthStencilState() const {
    const auto& regs = liverpool->DrawRegs();
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto depth_test_enabled =
        regs.depth_control.depth_enable && regs.depth_buffer.DepthValid();
    dynamic_state.SetDepthTestEnabled(depth_test_enabled);
    if (depth_test_enabled) {
        dynamic_state.SetDepthWriteEnabled(regs.depth_control.depth_write_enable &&
                                           !regs.depth_render_control.depth_clear_enable);
        dynamic_state.SetDepthCompareOp(LiverpoolToVK::CompareOp(regs.depth_control.depth_func));
    }

    const auto depth_bounds_test_enabled = regs.depth_control.depth_bounds_enable;
    dynamic_state.SetDepthBoundsTestEnabled(depth_bounds_test_enabled);
    if (depth_bounds_test_enabled) {
        dynamic_state.SetDepthBounds(regs.depth_bounds_min, regs.depth_bounds_max);
    }

    const auto depth_bias_enabled = regs.polygon_control.NeedsBias();
    dynamic_state.SetDepthBiasEnabled(depth_bias_enabled);
    if (depth_bias_enabled) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        dynamic_state.SetDepthBias(
            front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset,
            regs.poly_offset.depth_bias,
            (front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f);
    }

    const auto stencil_test_enabled =
        regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid();
    dynamic_state.SetStencilTestEnabled(stencil_test_enabled);
    if (stencil_test_enabled) {
        const StencilOps front_ops{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_front),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_front),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_front),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_ref_func),
        };
        const StencilOps back_ops = regs.depth_control.backface_enable ? StencilOps{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_back),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_back),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_back),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_bf_func),
        } : front_ops;
        dynamic_state.SetStencilOps(front_ops, back_ops);

        const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
        const auto front = regs.stencil_ref_front;
        const auto back =
            regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
        // GCN REPLACE_OP writes DB_STENCILREFMASK.STENCILOPVAL, so a face whose stencil ops
        // include ReplaceOp takes its Vulkan reference from op_val.
        const auto& sc = regs.stencil_control;
        const auto uses_op_val = [](AmdGpu::StencilFunc fail, AmdGpu::StencilFunc zpass,
                                    AmdGpu::StencilFunc zfail) {
            return fail == AmdGpu::StencilFunc::ReplaceOp ||
                   zpass == AmdGpu::StencilFunc::ReplaceOp ||
                   zfail == AmdGpu::StencilFunc::ReplaceOp;
        };
        const bool front_op =
            uses_op_val(sc.stencil_fail_front, sc.stencil_zpass_front, sc.stencil_zfail_front);
        const bool back_op =
            regs.depth_control.backface_enable
                ? uses_op_val(sc.stencil_fail_back, sc.stencil_zpass_back, sc.stencil_zfail_back)
                : front_op;
        const auto ref_conflict = [](AmdGpu::CompareFunc func, const AmdGpu::StencilRefMask& ref) {
            return func != AmdGpu::CompareFunc::Always && func != AmdGpu::CompareFunc::Never &&
                   ref.stencil_test_val != ref.stencil_op_val;
        };
        if ((front_op && ref_conflict(regs.depth_control.stencil_ref_func, front)) ||
            (back_op && regs.depth_control.backface_enable &&
             ref_conflict(regs.depth_control.stencil_bf_func, back))) {
            // sotc: per draw with this state; throttled (P5).
            if (SotcStats::LogThrottled("stencil-ref-conflict")) {
                LOG_WARNING(Render_Vulkan,
                            "Stencil test requires test_val while ReplaceOp requires op_val; "
                            "the stencil test will use op_val");
            }
        }
        dynamic_state.SetStencilReferences(front_op ? front.stencil_op_val : front.stencil_test_val,
                                           back_op ? back.stencil_op_val : back.stencil_test_val);
        dynamic_state.SetStencilWriteMasks(!stencil_clear ? front.stencil_write_mask : 0U,
                                           !stencil_clear ? back.stencil_write_mask : 0U);
        dynamic_state.SetStencilCompareMasks(front.stencil_mask, back.stencil_mask);
    }
}

void Rasterizer::UpdatePrimitiveState(const bool is_indexed) const {
    const auto& regs = liverpool->DrawRegs();
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto is_list_topology = [](const AmdGpu::PrimitiveType type) {
        const auto topology = LiverpoolToVK::PrimitiveType(type);
        return topology == vk::PrimitiveTopology::ePointList ||
               topology == vk::PrimitiveTopology::eLineList ||
               topology == vk::PrimitiveTopology::eTriangleList ||
               topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
               topology == vk::PrimitiveTopology::eTriangleListWithAdjacency;
    };
    const auto is_patch_list_topology = [](const AmdGpu::PrimitiveType type) {
        // Quad and rect lists are emulated using tessellation.
        return type == AmdGpu::PrimitiveType::PatchPrimitive ||
               type == AmdGpu::PrimitiveType::QuadList || type == AmdGpu::PrimitiveType::RectList;
    };

    const auto prim_restart =
        (regs.enable_primitive_restart & 1) != 0 &&
        (instance.IsListRestartSupported() || !is_list_topology(regs.primitive_type)) &&
        (instance.IsPatchListRestartSupported() || !is_patch_list_topology(regs.primitive_type));
    ASSERT_MSG(!is_indexed || !prim_restart || regs.primitive_restart_index == 0xFFFF ||
                   regs.primitive_restart_index == 0xFFFFFFFF,
               "Primitive restart index other than -1 is not supported yet");

    const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                               ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                               : vk::CullModeFlagBits::eNone;
    const auto front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);

    dynamic_state.SetPrimitiveRestartEnabled(prim_restart);
    dynamic_state.SetRasterizerDiscardEnabled(regs.clipper_control.dx_rasterization_kill);
    dynamic_state.SetCullMode(cull_mode);
    dynamic_state.SetFrontFace(front_face);
}

void Rasterizer::UpdateRasterizationState() const {
    const auto& regs = liverpool->DrawRegs();
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetLineWidth(regs.line_control.Width());
}

void Rasterizer::UpdateColorBlendingState(const GraphicsPipeline* pipeline) const {
    const auto& regs = liverpool->DrawRegs();
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetBlendConstants(regs.blend_constants);
    dynamic_state.SetColorWriteMasks(pipeline->GetGraphicsKey().write_masks);
    dynamic_state.SetAttachmentFeedbackLoopEnabled(attachment_feedback_loop);
}

void Rasterizer::ScopeMarkerBegin(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopeMarkerEnd(bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.endDebugUtilsLabelEXT();
}

void Rasterizer::ScopedMarkerInsert(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                         bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
        .color = std::array<f32, 4>(
            {(f32)((color >> 16) & 0xff) / 255.0f, (f32)((color >> 8) & 0xff) / 255.0f,
             (f32)(color & 0xff) / 255.0f, (f32)((color >> 24) & 0xff) / 255.0f})});
}

const Pipeline* Rasterizer::LookupGraphicsPipeline(u16 vertex_sgpr_offset,
                                                   u16 instance_sgpr_offset) {
    const DrawIndirectParams params = {
        .vertex_sgpr_offset = vertex_sgpr_offset,
        .instance_sgpr_offset = instance_sgpr_offset,
    };
    return pipeline_cache.GetGraphicsPipeline(params);
}

const Pipeline* Rasterizer::LookupComputePipeline() {
    return pipeline_cache.GetComputePipeline();
}

bool Rasterizer::IsGpuSideThread() const {
    return liverpool->IsGpuSideThread();
}

void Rasterizer::PrepareGpuSideFault() {
    liverpool->PrepareGpuSideFault();
}

bool Rasterizer::RunsGpuWorkElsewhere() const {
    return liverpool->RunsGpuWorkElsewhere();
}

void Rasterizer::RunOnDrawThreadImpl(Common::UniqueFunction<void>&& func) {
    liverpool->RunOnBackThread(std::move(func));
}

std::thread::id Rasterizer::GetGpuCommandProcessorThread() {
    return liverpool->GetGpuCommandProcessorThread();
}

#ifdef __linux__
u32 Rasterizer::GetGpuCommandProcessorThreadId() {
    return liverpool->GetGpuCommandProcessorThreadId();
}

bool Rasterizer::IsGpuSideThreadId(u32 tid) const {
    return liverpool->IsGpuSideThreadId(tid);
}
#endif

} // namespace Vulkan
