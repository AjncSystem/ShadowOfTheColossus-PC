// SPDX-FileCopyrightText: Copyright 2026 SotC-PC
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: SOTC_GPU_TIMES=1 -- where the GPU time of a frame goes. Every SOTC_GPU_TIMES_EVERY (120)
// game flips, the draws and dispatches recorded during one flip interval get a GPU timestamp
// before and after (bottom of pipe: when everything before has finished). "op" is the time of
// a draw or dispatch itself, "between" the time from the previous op's end to this one's start
// (barriers, copies, texture uploads and clears recorded between them, render pass changes).
// The report sums each 5 s window's samples per frame: totals by kind, the costliest shaders,
// and the costliest "between" gaps by the op that follows them. Draw thread only (or the command
// processor without it); off: nothing is recorded.

#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/sotc_gpu_times_note.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan::SotcGpuTimes {

enum Kind : u8 { KDraw, KDrawIndirect, KDispatch, KDispatchIndirect, KCount };

inline const char* const kKindNames[KCount] = {"draw", "draw-indirect", "dispatch",
                                               "dispatch-indirect"};

struct State {
    static constexpr u32 kCap = 16384; ///< ops per sample (two queries each)
    vk::Device device{};
    vk::QueryPool pool{};
    double ns_per_tick = 1.0;
    enum Phase { Idle, Armed, Recording, Pending } phase = Idle;
    u64 arm_flip = 0, rec_flip = 0, next_flip = 0, done_tick = 0;
    struct Label {
        u64 hash;
        Kind kind;
        bool after_submit; ///< a submission ended between the previous op and this one
        u32 causes;        ///< Cause bits recorded since the previous op
        u64 op;            ///< op_counter index
        u64 dep;           ///< newest op whose writes it reads (cull programs; ~0: none)
    };
    u32 dep_count = 0;
    double dep_slack = 0, dep_slack_first = 0, dep_slack_max = 0;
    std::unordered_map<u64, std::pair<double, u32>> slack_by_hash;
    std::unordered_map<u32, std::pair<double, u32>> between_by_cause;
    std::vector<Label> labels;
    u32 open = ~0u; ///< index of the op whose "after" timestamp is still to be written
    u64 last_tick = 0; ///< the command buffer tick when the previous op was recorded
    // window totals (ns), reset by Report
    u32 samples = 0, submits = 0;
    double span = 0, op_total = 0, between_total = 0, between_submit = 0, kind_total[KCount]{},
           kind_count[KCount]{};
    std::unordered_map<u64, std::pair<double, u32>> by_hash, between_by_hash, submit_by_hash;
    std::mutex report_mutex;
};

inline State& Get() {
    static State s;
    return s;
}

inline u32 Every() {
    static const u32 every = [] {
        const char* v = std::getenv("SOTC_GPU_TIMES_EVERY");
        const long n = v ? std::strtol(v, nullptr, 10) : 0;
        return n > 0 ? u32(n) : 120u;
    }();
    return every;
}

/// Reads a finished sample into the window totals.
inline void Collect(State& s) {
    const u32 n = static_cast<u32>(s.labels.size());
    if (n == 0) {
        return;
    }
    std::vector<u64> ts(2 * n);
    const auto result = s.device.getQueryPoolResults(
        s.pool, 0, 2 * n, ts.size() * sizeof(u64), ts.data(), sizeof(u64),
        vk::QueryResultFlagBits::e64);
    if (result != vk::Result::eSuccess) {
        return;
    }
    const auto ns = [&](u64 a, u64 b) { return b > a ? double(b - a) * s.ns_per_tick : 0.0; };
    std::scoped_lock lock{s.report_mutex}; // Report runs on the report thread
    ++s.samples;
    // Cull dispatches: GPU time from the end of the newest op they depend on (the sample's start
    // when that op came before it) to their own start.
    const u64 base = s.labels[0].op;
    for (u32 i = 0; i < n; ++i) {
        const auto& l = s.labels[i];
        if (l.dep == ~0ull || l.dep >= l.op) {
            continue;
        }
        const u64 ready = l.dep >= base ? ts[2 * (l.dep - base) + 1] : ts[0];
        const double slack = ns(ready, ts[2 * i]);
        ++s.dep_count;
        s.dep_slack += slack;
        s.dep_slack_max = std::max(s.dep_slack_max, slack);
        if (l.after_submit) {
            s.dep_slack_first += slack;
        }
        auto& h = s.slack_by_hash[l.hash];
        h.first += slack;
        ++h.second;
    }
    s.span += ns(ts[0], ts[2 * n - 1]);
    for (u32 i = 0; i < n; ++i) {
        const double op = ns(ts[2 * i], ts[2 * i + 1]);
        const double between = i ? ns(ts[2 * i - 1], ts[2 * i]) : 0.0;
        const auto& l = s.labels[i];
        s.op_total += op;
        s.kind_total[l.kind] += op;
        s.kind_count[l.kind] += 1;
        auto& h = s.by_hash[(u64(l.kind) << 56) ^ l.hash];
        h.first += op;
        ++h.second;
        // Across a submission the gap holds the GPU waiting for the next one (idle); inside a
        // command buffer it is barriers, copies and render pass changes.
        if (i && l.after_submit) {
            s.between_submit += between;
            ++s.submits;
            auto& b = s.submit_by_hash[(u64(l.kind) << 56) ^ l.hash];
            b.first += between;
            ++b.second;
        } else {
            s.between_total += between;
            auto& b = s.between_by_hash[(u64(l.kind) << 56) ^ l.hash];
            b.first += between;
            ++b.second;
            auto& c = s.between_by_cause[l.causes];
            c.first += between;
            ++c.second;
        }
    }
}

/// Before a draw or dispatch is recorded (cmdbuf: the command buffer it goes into).
template <typename Cmd>
inline void Before(const Instance& instance, Scheduler& scheduler, Cmd cmdbuf, Kind kind,
                   u64 hash, u64 flips) {
    State& s = Get();
    const u64 op = op_counter++;
    const u64 dep = std::exchange(pending_dep, ~0ull);
    if (!s.pool) {
        s.device = instance.GetDevice();
        const auto props = instance.GetPhysicalDevice().getProperties();
        s.ns_per_tick = props.limits.timestampPeriod;
        const vk::QueryPoolCreateInfo info{.queryType = vk::QueryType::eTimestamp,
                                           .queryCount = 2 * State::kCap};
        const auto [res, pool] = s.device.createQueryPool(info);
        if (res != vk::Result::eSuccess) {
            return;
        }
        s.pool = pool;
        s.next_flip = flips + Every();
    }
    switch (s.phase) {
    case State::Pending:
        if (!scheduler.IsFree(s.done_tick)) {
            return;
        }
        Collect(s);
        s.phase = State::Idle;
        [[fallthrough]];
    case State::Idle:
        if (flips < s.next_flip) {
            return;
        }
        s.phase = State::Armed;
        s.arm_flip = flips;
        return;
    case State::Armed:
        if (flips == s.arm_flip) {
            return;
        }
        s.device.resetQueryPool(s.pool, 0, 2 * State::kCap);
        s.labels.clear();
        pending_causes.store(0, std::memory_order_relaxed);
        s.phase = State::Recording;
        s.rec_flip = flips;
        break;
    case State::Recording:
        if (flips != s.rec_flip || s.labels.size() >= State::kCap) {
            s.phase = State::Pending;
            s.done_tick = scheduler.CurrentTick();
            s.next_flip = s.rec_flip + Every();
            return;
        }
        break;
    }
    const u32 i = static_cast<u32>(s.labels.size());
    const u64 tick = scheduler.CurrentTick();
    s.labels.push_back({hash, kind, tick != s.last_tick,
                        pending_causes.exchange(0, std::memory_order_relaxed), op, dep});
    s.last_tick = tick;
    cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, s.pool, 2 * i);
    s.open = i;
}

/// After it was recorded, into the same command buffer.
template <typename Cmd>
inline void After(Cmd cmdbuf) {
    State& s = Get();
    if (s.open == ~0u) {
        return;
    }
    cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, s.pool, 2 * s.open + 1);
    s.open = ~0u;
}

/// The window's report lines (empty when nothing was sampled); resets the totals.
inline std::string Report() {
    State& s = Get();
    if (!kOn) {
        return {};
    }
    std::scoped_lock lock{s.report_mutex};
    if (s.samples == 0) {
        return {};
    }
    const double k = 1.0 / (1e6 * s.samples); // ns per window -> ms per frame
    char line[512];
    std::string out;
    std::snprintf(line, sizeof(line),
                  "SOTC GPU TIMES (%u frames): span %.2f ms, ops %.2f, between %.2f in a command "
                  "buffer, %.2f across %u submissions;",
                  s.samples, s.span * k, s.op_total * k, s.between_total * k,
                  s.between_submit * k, s.submits / s.samples);
    out += line;
    for (int i = 0; i < KCount; ++i) {
        if (s.kind_count[i]) {
            std::snprintf(line, sizeof(line), " %s %.2f ms (%.0f)", kKindNames[i],
                          s.kind_total[i] * k, s.kind_count[i] / s.samples);
            out += line;
        }
    }
    out += "\n";
    const auto top = [&](auto& map, const char* title) {
        std::vector<std::pair<u64, std::pair<double, u32>>> v(map.begin(), map.end());
        std::sort(v.begin(), v.end(),
                  [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
        out += title;
        for (size_t i = 0; i < v.size() && i < 14; ++i) {
            std::snprintf(line, sizeof(line), " %s %#llx %.2f ms/%u;",
                          kKindNames[v[i].first >> 56],
                          static_cast<unsigned long long>(v[i].first & ((1ull << 56) - 1)),
                          v[i].second.first * k, v[i].second.second / s.samples);
            out += line;
        }
        out += "\n";
    };
    top(s.by_hash, "SOTC GPU TIMES ops by shader:");
    top(s.between_by_hash, "SOTC GPU TIMES between (same command buffer), before:");
    top(s.submit_by_hash, "SOTC GPU TIMES across submissions, before:");
    if (s.dep_count) {
        std::snprintf(line, sizeof(line),
                      "SOTC GPU TIMES cull deps: %u dispatches per frame, GPU time between their "
                      "inputs being written and their start %.2f ms per frame (%.2f for the first "
                      "after a submission), largest %.2f ms;",
                      s.dep_count / s.samples, s.dep_slack * k, s.dep_slack_first * k,
                      s.dep_slack_max / 1e6);
        out += line;
        std::vector<std::pair<u64, std::pair<double, u32>>> v(s.slack_by_hash.begin(),
                                                              s.slack_by_hash.end());
        std::sort(v.begin(), v.end(),
                  [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
        for (size_t i = 0; i < v.size() && i < 8; ++i) {
            std::snprintf(line, sizeof(line), " %#llx %.2f ms/%u;",
                          static_cast<unsigned long long>(v[i].first), v[i].second.first * k,
                          v[i].second.second / s.samples);
            out += line;
        }
        out += "\n";
    }
    {
        // Who reads the cull programs' output on the GPU (since the start, not per window).
        std::scoped_lock deps_lock{deps_mutex};
        if (!consumers.empty()) {
            out += "SOTC GPU TIMES readers of cull output (program: reads, closest in ops):";
            for (const auto& [program, c] : consumers) {
                std::snprintf(line, sizeof(line), " %#llx %llu/%llu;",
                              static_cast<unsigned long long>(program),
                              static_cast<unsigned long long>(c.reads),
                              static_cast<unsigned long long>(c.min_distance));
                out += line;
            }
            out += "\n";
        }
    }
    s.dep_count = 0;
    s.dep_slack = s.dep_slack_first = s.dep_slack_max = 0;
    s.slack_by_hash.clear();
    {
        std::vector<std::pair<u32, std::pair<double, u32>>> v(s.between_by_cause.begin(),
                                                              s.between_by_cause.end());
        std::sort(v.begin(), v.end(),
                  [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
        out += "SOTC GPU TIMES between by what was recorded:";
        for (size_t i = 0; i < v.size() && i < 12; ++i) {
            std::string name;
            for (int b = 0; b < CCauseBits; ++b) {
                if (v[i].first & (1u << b)) {
                    name += name.empty() ? "" : "+";
                    name += kCauseNames[b];
                }
            }
            std::snprintf(line, sizeof(line), " %s %.2f ms/%u;",
                          name.empty() ? "nothing" : name.c_str(), v[i].second.first * k,
                          v[i].second.second / s.samples);
            out += line;
        }
        out += "\n";
        s.between_by_cause.clear();
    }
    s.samples = s.submits = 0;
    s.span = s.op_total = s.between_total = s.between_submit = 0;
    std::fill(std::begin(s.kind_total), std::end(s.kind_total), 0.0);
    std::fill(std::begin(s.kind_count), std::end(s.kind_count), 0.0);
    s.by_hash.clear();
    s.between_by_hash.clear();
    s.submit_by_hash.clear();
    return out;
}

} // namespace Vulkan::SotcGpuTimes
