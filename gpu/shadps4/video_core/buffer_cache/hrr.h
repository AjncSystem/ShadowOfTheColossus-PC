// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: SOTC_HRR (host-resident ranges), stage 2 design, DRY-RUN part. Off by default: with the
// variable unset nothing here runs and nothing changes.
//
// The idea: a few guest ranges that the guest reads back every frame (GPU-written visibility
// bitmasks of the engine's XPR renderer) would live in the guest's own memory, imported as a
// VkBuffer with VK_EXT_external_memory_host, so the GPU writes them in place and a guest read
// fault only has to wait for the writer tick instead of copying arena -> staging -> backing.
// SOTC_HRR=1 (Real()): the promoted granules (the "WOULD PROMOTE" runs below) are placed in
// host-visible arena memory and, when they are already resident in device memory, migrated there
// (BufferCache::MigrateHostResident, with the exactness argument). Any other value runs the dry
// run only: it learns which ranges would be promoted, which conditions block them, and what the
// promotion would have saved, so the selection can be checked on real scenes.
//
// Selection: the readback path (BufferCache::ReadMemory) counts, per 64 KiB granule, the guest
// readbacks that touch it. Once a second, granules with at least SOTC_HRR_MIN_RB (default 30)
// readbacks in that second are joined into runs of adjacent granules; the busiest unblocked runs
// are taken until SOTC_HRR_MB (default 16) MiB are used. Blockers, all observed (none changes
// behaviour):
//   image    a texture cache image overlaps the run (checked at bind time, BufferCache::ObtainBuffer)
//   spans    a binding crosses the run's edge (it would have to be split between two memories)
//   nobind   no binding was seen on the run in the window (written by a copy, DMA or CPU, or the
//            bind cache hid it): promoting would not stop the readbacks
//   backing  the run is not inside one mapping with a backing view (not importable)
// Not observable here (listed in the report): atomics (per-binding shader Info, rasterizer),
// GDS (separate buffer, never a guest address), stacks (not in the VMA data the GPU library sees).
//
// Everything is guarded by one mutex: the guest threads (readbacks), the command processor and
// the draw thread (bindings) all call in. Diagnostic only.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>
#include "common/types.h"
#include "core/memory.h"
#include "video_core/sotc_stats.h"

namespace VideoCore {

class HostResidentRanges {
public:
    static constexpr u64 GranuleBits = 16;
    static constexpr u64 Granule = u64{1} << GranuleBits;

    /// SOTC_HRR set and not "0" (any value runs the dry run; see the header comment).
    static bool Enabled() {
        static const bool on = [] {
            const char* v = std::getenv("SOTC_HRR");
            if (!v || v[0] == '\0' || v[0] == '0') {
                return false;
            }
            if (v[0] != 'd' && v[0] != '1') {
                std::fprintf(stderr, "SOTC HRR: unknown value, running as SOTC_HRR=dry "
                                     "(SOTC_HRR=1 enables promotion)\n");
            }
            return true;
        }();
        return on;
    }

    /// sotc: SOTC_HRR=1 exactly: promoted ranges are placed in host-visible arena memory.
    /// False when the device has no suitable host-visible memory type (the BufferCache
    /// constructor sets no_host_memory and logs once): then everything behaves as the dry run.
    static bool Real() {
        static const bool env_real = [] {
            const char* v = std::getenv("SOTC_HRR");
            return v && v[0] == '1';
        }();
        return env_real && !no_host_memory.load(std::memory_order_relaxed);
    }

    /// sotc: set by the BufferCache constructor when Real() was asked for but no host-visible
    /// arena memory type exists.
    static inline std::atomic<bool> no_host_memory{false};

    /// Guest thread: a readback of [addr, addr + size) was requested. Runs the once-a-second
    /// selection when its time has come.
    void NoteReadback(VAddr addr, u64 size) {
        if (size == 0) {
            return;
        }
        const u64 now = NowNs();
        std::scoped_lock lock{mutex};
        const u64 first = addr >> GranuleBits;
        const u64 last = std::min((addr + size - 1) >> GranuleBits, first + MaxGranulesPerCall);
        for (u64 g = first; g <= last; ++g) {
            auto it = granules.find(g);
            if (it == granules.end()) {
                if (granules.size() >= MaxGranules) {
                    continue;
                }
                it = granules.emplace(g, Granule_{}).first;
                any_granules.store(true, std::memory_order_relaxed); // sotc: see NoteBinding
            }
            ++it->second.reads;
        }
        if (next_eval_ns == 0) {
            next_eval_ns = now + EvalPeriodNs;
        } else if (now >= next_eval_ns) {
            next_eval_ns = now + EvalPeriodNs;
            Evaluate();
        }
    }

    /// Command processor / draw thread: a buffer of [addr, addr + size) is bound. Returns true
    /// when it touches a hot granule whose texture cache overlap is still unknown; the caller then
    /// reports it with NoteImageOverlap.
    bool NoteBinding(VAddr addr, u64 size, bool written) {
        if (size == 0) {
            return false;
        }
        // sotc: no granule tracked: the locked path below returns false without any side effect,
        // so the lock is skipped. Relaxed is enough: any_granules is stored under the mutex when a
        // granule is added (NoteReadback) and when Evaluate leaves none. A bind ordered after an
        // add sees it (coherence); a concurrent one linearizes before the add.
        if (!any_granules.load(std::memory_order_relaxed)) {
            return false;
        }
        std::scoped_lock lock{mutex};
        if (granules.empty()) {
            return false;
        }
        const u64 first = addr >> GranuleBits;
        const u64 last = (addr + size - 1) >> GranuleBits;
        bool want_image = false;
        for (auto it = granules.lower_bound(first); it != granules.end() && it->first <= last;
             ++it) {
            auto& g = it->second;
            g.bind_lo = std::min<VAddr>(g.bind_lo, addr);
            g.bind_hi = std::max<VAddr>(g.bind_hi, addr + size);
            ++g.binds;
            g.written |= written;
            want_image |= !g.image_checked;
        }
        return want_image;
    }

    void NoteImageOverlap(VAddr addr, u64 size, bool overlaps) {
        std::scoped_lock lock{mutex};
        const u64 first = addr >> GranuleBits;
        const u64 last = (addr + size - 1) >> GranuleBits;
        for (auto it = granules.lower_bound(first); it != granules.end() && it->first <= last;
             ++it) {
            it->second.image_checked = true;
            it->second.image |= overlaps;
        }
    }

    /// Command processor (BufferCache::BeginAsyncReadback): the readback is a would-be fault of a
    /// would-be promoted range. writer_done: the newest writer tick of the bytes had finished
    /// (the fault would not have waited).
    void NoteFault(VAddr addr, u64 size, bool writer_done) {
        std::scoped_lock lock{mutex};
        if (!InPromoted(addr, size)) {
            return;
        }
        SotcStats::hrr_faults.fetch_add(1, std::memory_order_relaxed);
        SotcStats::hrr_faults_all.fetch_add(1, std::memory_order_relaxed);
        (writer_done ? SotcStats::hrr_nowait : SotcStats::hrr_waited)
            .fetch_add(1, std::memory_order_relaxed);
    }

    /// Guest thread: the wall-clock time the readback of [addr, addr + size) actually cost (what
    /// a promotion could save at most).
    void NoteGuestTime(VAddr addr, u64 size, u64 ns) {
        std::scoped_lock lock{mutex};
        if (InPromoted(addr, size)) {
            SotcStats::hrr_rb_ns.fetch_add(ns, std::memory_order_relaxed);
            SotcStats::hrr_rb_count.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /// sotc: SOTC_HRR=1: true when a promoted granule overlaps [addr, addr + size).
    bool IsPromoted(VAddr addr, u64 size) const {
        std::scoped_lock lock{mutex};
        return InPromoted(addr, size);
    }

    /// sotc: SOTC_HRR=1: the parts of [addr, addr + size) that lie in promoted granules, sorted.
    std::vector<std::pair<VAddr, VAddr>> PromotedIn(VAddr addr, u64 size) const {
        std::vector<std::pair<VAddr, VAddr>> out;
        const VAddr end = addr + size;
        std::scoped_lock lock{mutex};
        for (const auto& [lo, hi] : promoted) {
            if (addr < hi && lo < end) {
                out.emplace_back(std::max(lo, addr), std::min(hi, end));
            }
        }
        return out;
    }

    /// sotc: SOTC_HRR=1: granules (Granule-aligned) promoted since the last call, and not promoted
    /// before. BufferCache::MigrateHostResident takes them.
    std::vector<std::pair<VAddr, VAddr>> TakeNewPromotions() {
        std::scoped_lock lock{mutex};
        has_new.store(false, std::memory_order_relaxed);
        return std::exchange(new_promotions, {});
    }

    /// sotc: SOTC_HRR=1: lock-free test: TakeNewPromotions would return something. Stale false
    /// only delays the granules by one command.
    bool HasNew() const {
        return has_new.load(std::memory_order_relaxed);
    }

private:
    static constexpr u64 EvalPeriodNs = 1'000'000'000;
    static constexpr u64 MaxGranules = 8192;
    static constexpr u64 MaxGranulesPerCall = 255; ///< one request counts for 16 MiB at most
    static constexpr u32 IdleEvals = 10;

    struct Granule_ {
        u32 reads = 0; ///< readbacks touching it in this window
        u32 idle = 0;  ///< evaluations since it was last hot
        u32 binds = 0;
        VAddr bind_lo = ~VAddr{0}; ///< widest binding seen on it this window
        VAddr bind_hi = 0;
        bool written = false;
        bool image = false;         ///< a texture cache image overlaps (latched until it ages out)
        bool image_checked = false;
    };
    struct Run {
        u64 first = 0, last = 0; ///< granule indices
        u64 reads = 0;           ///< readbacks in the window, summed over granules
        u32 blockers = 0;
    };
    enum Blocker : u32 { BImage = 1, BSpans = 2, BNoBind = 4, BBacking = 8, BCap = 16 };

    static u64 NowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    bool InPromoted(VAddr addr, u64 size) const {
        const VAddr end = addr + size;
        for (const auto& [lo, hi] : promoted) {
            if (addr < hi && lo < end) {
                return true;
            }
        }
        return false;
    }

    void Evaluate() {
        static const u32 min_rb = [] {
            const char* v = std::getenv("SOTC_HRR_MIN_RB");
            return static_cast<u32>(std::max<u64>(v ? std::strtoull(v, nullptr, 10) : 30, 1));
        }();
        static const u64 cap_bytes = [] {
            const char* v = std::getenv("SOTC_HRR_MB");
            return std::max<u64>(v ? std::strtoull(v, nullptr, 10) : 16, 1) << 20;
        }();
        std::vector<Run> runs;
        for (auto it = granules.begin(); it != granules.end();) {
            auto& g = it->second;
            const bool hot = g.reads >= min_rb;
            if (hot) {
                g.idle = 0;
                if (!runs.empty() && runs.back().last + 1 == it->first) {
                    runs.back().last = it->first;
                    runs.back().reads += g.reads;
                } else {
                    runs.push_back({.first = it->first, .last = it->first, .reads = g.reads});
                }
            } else if (++g.idle >= IdleEvals) {
                it = granules.erase(it);
                continue;
            }
            ++it;
        }
        // sotc: the erasures above may have emptied granules (see NoteBinding's early check).
        any_granules.store(!granules.empty(), std::memory_order_relaxed);
        auto& mem = *Core::Memory::Instance();
        for (auto& run : runs) {
            const VAddr lo = run.first << GranuleBits;
            const VAddr hi = (run.last + 1) << GranuleBits;
            bool bound = false;
            for (u64 g = run.first; g <= run.last; ++g) {
                const auto& e = granules.at(g);
                run.blockers |= e.image ? BImage : 0;
                bound |= e.binds != 0;
                if (e.binds != 0 && (e.bind_lo < lo || e.bind_hi > hi)) {
                    run.blockers |= BSpans;
                }
            }
            run.blockers |= bound ? 0 : BNoBind;
            VAddr run_start = 0;
            u64 run_size = 0;
            if (!mem.GetBackingRun(lo, &run_start, &run_size) || run_start > lo ||
                run_start + run_size < hi) {
                run.blockers |= BBacking;
            }
        }
        // Busiest first, until the cap is used.
        std::vector<Run*> order;
        for (auto& run : runs) {
            order.push_back(&run);
        }
        std::sort(order.begin(), order.end(),
                  [](const Run* a, const Run* b) { return a->reads > b->reads; });
        u64 used = 0;
        std::vector<std::pair<VAddr, VAddr>> next;
        for (Run* run : order) {
            const u64 bytes = (run->last - run->first + 1) << GranuleBits;
            if (run->blockers == 0 && used + bytes > cap_bytes) {
                run->blockers |= BCap;
            }
            if (run->blockers == 0) {
                used += bytes;
                next.emplace_back(run->first << GranuleBits, (run->last + 1) << GranuleBits);
            }
        }
        std::sort(next.begin(), next.end());
        // Would-be demotions: a range promoted before that no promoted range overlaps any more.
        u64 demoted = 0;
        for (const auto& [lo, hi] : promoted) {
            const bool kept = std::any_of(next.begin(), next.end(), [&](const auto& n) {
                return lo < n.second && n.first < hi;
            });
            demoted += kept ? 0 : 1;
        }
        // sotc: SOTC_HRR=1: granules promoted now and not before, for the migration (dry run: none).
        if (Real()) {
            for (const auto& [lo, hi] : next) {
                for (VAddr g = lo; g < hi; g += Granule) {
                    const bool was_promoted =
                        std::any_of(promoted.begin(), promoted.end(), [&](const auto& p) {
                            return g < p.second && p.first < g + Granule;
                        });
                    if (!was_promoted && new_promotions.size() < MaxGranules) {
                        new_promotions.emplace_back(g, g + Granule);
                        has_new.store(true, std::memory_order_relaxed);
                    }
                }
            }
        }
        const bool changed = next != promoted;
        promoted = std::move(next);
        SotcStats::hrr_ranges.store(promoted.size(), std::memory_order_relaxed);
        SotcStats::hrr_kib.store(used >> 10, std::memory_order_relaxed);
        SotcStats::hrr_demotions.fetch_add(demoted, std::memory_order_relaxed);
        for (const Run& run : runs) {
            if (run.blockers & BBacking) {
                SotcStats::hrr_import_fail.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (changed || blocked_logged != BlockedSignature(runs)) {
            blocked_logged = BlockedSignature(runs);
            int shown = 0;
            for (const Run* run : order) {
                if (shown++ >= 12) {
                    break;
                }
                char why[64] = "";
                if (run->blockers == 0) {
                    std::snprintf(why, sizeof(why), "WOULD PROMOTE");
                } else {
                    std::snprintf(why, sizeof(why), "blocked:%s%s%s%s%s",
                                  run->blockers & BImage ? " image" : "",
                                  run->blockers & BSpans ? " spans" : "",
                                  run->blockers & BNoBind ? " nobind" : "",
                                  run->blockers & BBacking ? " backing" : "",
                                  run->blockers & BCap ? " cap" : "");
                }
                std::fprintf(stderr, "SOTC HRR dry: %#llx+%llu KiB, %llu rb/s, %s\n",
                             static_cast<unsigned long long>(run->first << GranuleBits),
                             static_cast<unsigned long long>((run->last - run->first + 1) *
                                                             (Granule >> 10)),
                             static_cast<unsigned long long>(run->reads), why);
            }
        }
        // The binding observations cover one window.
        for (auto& [index, g] : granules) {
            g.reads = 0;
            g.binds = 0;
            g.bind_lo = ~VAddr{0};
            g.bind_hi = 0;
            g.written = false;
            if (g.idle != 0) {
                g.image = false;
                g.image_checked = false;
            }
        }
    }

    static u64 BlockedSignature(const std::vector<Run>& runs) {
        u64 h = 1469598103934665603ull;
        for (const Run& run : runs) {
            h = (h ^ (run.first * 31 + run.last)) * 1099511628211ull;
            h = (h ^ run.blockers) * 1099511628211ull;
        }
        return h;
    }

    mutable std::mutex mutex;
    std::map<u64, Granule_> granules; ///< by granule index
    /// sotc: SOTC_HRR: granules is not empty (stored under mutex; read relaxed by NoteBinding).
    std::atomic<bool> any_granules{false};
    std::vector<std::pair<VAddr, VAddr>> promoted; ///< would-be promoted ranges, sorted
    /// sotc: SOTC_HRR=1: granules promoted since TakeNewPromotions (empty in the dry run).
    std::vector<std::pair<VAddr, VAddr>> new_promotions;
    /// sotc: SOTC_HRR=1: new_promotions is not empty. Set under the mutex (Evaluate), cleared by
    /// TakeNewPromotions; read without the mutex by HasNew (the draw thread's per-command test).
    std::atomic<bool> has_new{false};
    u64 next_eval_ns = 0;
    u64 blocked_logged = 0;
};

} // namespace VideoCore
