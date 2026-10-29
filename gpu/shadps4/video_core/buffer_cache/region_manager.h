// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <mutex>
#include <vector>

#include "common/adaptive_mutex.h"
#include "common/types.h"
#include "core/emulator_settings.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/page_manager.h"
#include "video_core/sotc_stats.h"

namespace VideoCore {

/// sotc: repeat-fault grace. The GPU command thread clears a page's CPU bit and write-protects it
/// again right after uploading it (every DMA draw syncs every resident range), so a guest writing
/// that page could fault again and again before its instruction completed (thousands of times per
/// boot, and the startup deaths came out of such storms). A page with a second guest write fault
/// within 1-2 quanta is "hot" for 1-2 quanta: syncs still collect and upload it but leave its CPU
/// bit set, so it is not protected again and the write goes through. Nothing CPU-written is ever
/// hidden from the GPU. SOTC_FAULT_GRACE=0 turns it off.
struct FaultGrace {
    static u32 Pages() { // hot pages allowed per quantum, all regions together (0 = off)
        static const u32 v = [] {
            const char* e = std::getenv("SOTC_FAULT_GRACE");
            if (e && e[0] == '0') {
                return 0u;
            }
            const char* p = std::getenv("SOTC_FAULT_GRACE_PAGES");
            return p ? static_cast<u32>(std::strtoul(p, nullptr, 10)) : 32u;
        }();
        return v;
    }
    static u32 Now() { // the current quantum (SOTC_FAULT_GRACE_US, default 1000)
        static const u64 us = [] {
            const char* e = std::getenv("SOTC_FAULT_GRACE_US");
            return std::clamp<u64>(e ? std::strtoull(e, nullptr, 10) : 1000ULL, 100, 100000);
        }();
        return static_cast<u32>(std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count() /
                                us);
    }
    inline static std::atomic<u32> budget_epoch{0};
    inline static std::atomic<u32> budget_used{0};
    static bool TakeBudget(u32 now) { // approximate under races, which is fine for a cap
        u32 epoch = budget_epoch.load(std::memory_order_relaxed);
        if (epoch != now &&
            budget_epoch.compare_exchange_strong(epoch, now, std::memory_order_relaxed)) {
            budget_used.store(0, std::memory_order_relaxed);
        }
        return budget_used.fetch_add(1, std::memory_order_relaxed) < Pages();
    }
};

/// sotc: SOTC_HOT_PAGES=1 (off by default). FaultGrace keeps a page writable for 1-2 ms, which
/// stops fault storms but not the pages the game rewrites every frame: those fault again every
/// frame (a guest exception round trip, then a VirtualProtect on the draw thread after the next
/// upload). A page that takes a guest write fault again within one or two windows of
/// SOTC_HOT_PAGES_WINDOW_MS (default 50) becomes sticky: uploads leave its CPU bit set, so it stays
/// writable and is uploaded at every sync (SOTC_HASH_SMALL then skips the unchanged ones), until
/// its lifetime ends and its next upload protects it again. The lifetime is SOTC_HOT_PAGES_MS
/// (default 2000) at most, and at least half of it: the sticky pages of a region are kept in two
/// generations of MS/2 each (a page marked just before a rotation used to lose its whole life).
///
/// Why it is exact: a sticky page keeps its CPU bit. Everything that reads a page for the GPU
/// (BufferCache::SynchronizeMemory*, the DMA sweep) collects CPU-modified pages, so a write at any
/// time lands in a page the next sync uploads; the bit is never cleared behind a write. The only
/// price is that the page is uploaded again at every sync of a binding that covers it.
///
/// SOTC_HOT_PAGES_MAX (default 1024) is the number of sticky pages, all regions together, the cap
/// starts at. With SOTC_HOT_PAGES_ADAPT not 0 it adapts: 32 refusals (a page that would be sticky
/// but the cap is full) double it, up to SOTC_HOT_PAGES_MAX_CEIL (default 16384 = 64 MiB), and a
/// second without refusals while less than half of it is used takes a quarter back (down to the
/// start value).
struct HotPages {
    static bool Enabled() {
        static const bool v = [] {
            const char* e = std::getenv("SOTC_HOT_PAGES");
            return e && e[0] == '1';
        }();
        return v;
    }
    static u64 NowMs() {
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
    }
    static u64 WindowMs() {
        static const u64 ms = [] {
            const char* e = std::getenv("SOTC_HOT_PAGES_WINDOW_MS");
            return std::clamp<u64>(e ? std::strtoull(e, nullptr, 10) : 50ULL, 5, 1000);
        }();
        return ms;
    }
    /// Length of one sticky generation: half of SOTC_HOT_PAGES_MS.
    static u64 LifeStepMs() {
        static const u64 ms = [] {
            const char* e = std::getenv("SOTC_HOT_PAGES_MS");
            return std::clamp<u64>(e ? std::strtoull(e, nullptr, 10) : 2000ULL, 100, 60000) / 2;
        }();
        return ms;
    }
    static u32 BaseMax() {
        static const u32 v = [] {
            const char* e = std::getenv("SOTC_HOT_PAGES_MAX");
            return e ? static_cast<u32>(std::strtoul(e, nullptr, 10)) : 1024u;
        }();
        return v;
    }
    static u32 Ceil() {
        static const u32 v = [] {
            const char* e = std::getenv("SOTC_HOT_PAGES_MAX_CEIL");
            const u32 c = e ? static_cast<u32>(std::strtoul(e, nullptr, 10)) : 16384u;
            return std::max(c, BaseMax());
        }();
        return v;
    }
    static bool Adaptive() {
        static const bool v = [] {
            const char* e = std::getenv("SOTC_HOT_PAGES_ADAPT");
            return !e || e[0] != '0';
        }();
        return v;
    }
    inline static std::atomic<u32> live{0}; ///< sticky pages now, all regions
    static std::atomic<u32>& CapRef() {     ///< the cap now
        static std::atomic<u32> cap{BaseMax()};
        return cap;
    }
    static u32 Max() {
        return CapRef().load(std::memory_order_relaxed);
    }
    inline static std::atomic<u32> demand{0};    ///< refusals since the cap last grew
    inline static std::atomic<u32> tick_hits{0}; ///< refusals in the current second
    inline static std::atomic<u64> tick{0};      ///< the current second (ms / 1000)
    /// A page that would be made sticky was refused at the cap (counted; may grow the cap).
    static void NoteRefused() {
        SotcStats::hot_pages_cap_hits.fetch_add(1, std::memory_order_relaxed);
        tick_hits.fetch_add(1, std::memory_order_relaxed);
        if (!Adaptive()) {
            return;
        }
        constexpr u32 GrowAfter = 32;
        if (demand.fetch_add(1, std::memory_order_relaxed) + 1 < GrowAfter) {
            return;
        }
        demand.store(0, std::memory_order_relaxed);
        auto& cap = CapRef();
        u32 now = cap.load(std::memory_order_relaxed);
        const u32 grown = static_cast<u32>(std::min<u64>(Ceil(), u64{now} * 2 + 64));
        if (grown > now && cap.compare_exchange_strong(now, grown, std::memory_order_relaxed)) {
            SotcStats::hot_pages_cap_grows.fetch_add(1, std::memory_order_relaxed);
        }
    }
    /// Once a second, from the fault path: a second without refusals and with the cap more than
    /// half empty gives a quarter of the growth back.
    static void MaybeShrink(u64 now_ms) {
        const u64 second = now_ms / 1000;
        u64 seen = tick.load(std::memory_order_relaxed);
        if (second == seen ||
            !tick.compare_exchange_strong(seen, second, std::memory_order_relaxed)) {
            return;
        }
        const u32 hits = tick_hits.exchange(0, std::memory_order_relaxed);
        if (!Adaptive() || hits != 0) {
            return;
        }
        auto& cap = CapRef();
        u32 now = cap.load(std::memory_order_relaxed);
        if (now <= BaseMax() || live.load(std::memory_order_relaxed) >= now / 2) {
            return;
        }
        const u32 shrunk = std::max(BaseMax(), now - now / 4);
        if (cap.compare_exchange_strong(now, shrunk, std::memory_order_relaxed)) {
            SotcStats::hot_pages_cap_shrinks.fetch_add(1, std::memory_order_relaxed);
        }
    }
};

/// sotc: SOTC_UNTRACK_RANGES=<hexaddr:hexsize,...> (off when unset). Guest ranges the port never
/// write-protects. A page of such a range keeps its CPU bit after every upload that collects it
/// (RegionManager::ForEachModifiedRange, grace_op), so it is never Track()ed again and every bind
/// that covers it uploads it again: a guest write is seen by the next bind without a write fault.
/// The price is one upload per bind for every untracked page, so only a range the game rewrites
/// every frame is worth listing. Read bindings only: a page the GPU wrote is never kept (a CPU
/// upload over it would undo the GPU result), it takes the normal path and is protected again.
/// Disabled unless SOTC_CP_AHEAD is unset or 0.
struct SotcUntrack {
    struct Range {
        u64 addr;
        u64 size;
        bool operator==(const Range&) const = default;
    };
    /// The parsed list, read once when the first region is created. Malformed entries are skipped.
    static const std::vector<Range>& Ranges() {
        static const std::vector<Range> ranges = Parse();
        return ranges;
    }
    /// Untracked pages whose upload kept their CPU bit, summed over all uploads (SOTC_UNTRACK_LOG).
    static u64 PagesKept() {
        return pages_kept.load(std::memory_order_relaxed);
    }
    inline static std::atomic<u64> pages_kept{0};

    /// sotc: SOTC_UNTRACK_AUTO=1 (off by default; off too when SOTC_CP_AHEAD is set, as Parse()).
    /// The flip thread publishes the live chunk list of the game's heap (driver.cpp,
    /// UntrackAutoTick); the draw thread applies it to the regions (BufferCache::SynchronizeDmaBuffers).
    /// Only the list is shared between the two threads.
    static bool AutoOn() {
        static const bool on = [] {
            const char* v = std::getenv("SOTC_UNTRACK_AUTO");
            const char* ahead = std::getenv("SOTC_CP_AHEAD");
            return v && v[0] == '1' && !(ahead && std::atoi(ahead) != 0);
        }();
        return on;
    }
    /// sotc: flip thread. Publishes a live list, clipped in list order to SOTC_UNTRACK_MAX_MB (16
    /// when unset). An unchanged list publishes nothing; nothing is published after a trip.
    static void Publish(std::vector<Range> live) {
        static const u64 cap = [] {
            const char* v = std::getenv("SOTC_UNTRACK_MAX_MB");
            const u64 mib = v ? std::strtoull(v, nullptr, 10) : 16;
            return std::min<u64>(mib, 1024) << 20;
        }();
        std::vector<Range> clipped;
        u64 total = 0;
        for (const Range& r : live) {
            if (total >= cap) {
                break;
            }
            const u64 size = std::min(r.size, cap - total);
            clipped.push_back(Range{r.addr, size});
            total += size;
        }
        std::scoped_lock lock{AutoMutex()};
        if (tripped.load(std::memory_order_relaxed) || clipped == AutoActive()) {
            return;
        }
        AutoActive() = std::move(clipped);
        auto_bytes.store(total, std::memory_order_relaxed);
        auto_version.fetch_add(1, std::memory_order_release);
        std::fprintf(stderr, "SOTC untrack auto: %zu ranges, %.1f MiB untracked (cap %llu MiB)\n",
                     AutoActive().size(), total / 1048576.0,
                     static_cast<unsigned long long>(cap >> 20));
    }
    /// sotc: any thread. The published list (copied) and its version.
    static std::vector<Range> AutoSnapshot(u64& version) {
        std::scoped_lock lock{AutoMutex()};
        version = auto_version.load(std::memory_order_relaxed);
        return AutoActive();
    }
    /// sotc: the version the draw thread compares with the one it applied last.
    static u64 AutoVersion() {
        return auto_version.load(std::memory_order_acquire);
    }
    /// sotc: self-check. `bad` guest write faults hit pages that were untracked, had no GPU bit and
    /// were protected (MemoryTracker::InvalidateRegion, region mutex held). Fails closed (Trip).
    static void NoteUntrackedFaults(u64 bad) {
        faults_untracked.fetch_add(bad, std::memory_order_relaxed);
        Trip();
    }
    /// sotc: fail closed. Publishing stops and the empty list is applied, so the draw thread clears
    /// every mask and the normal upload path re-protects those pages. Prints one line.
    static void Trip() {
        std::scoped_lock lock{AutoMutex()};
        if (tripped.exchange(true, std::memory_order_relaxed)) {
            return;
        }
        AutoActive().clear();
        auto_bytes.store(0, std::memory_order_relaxed);
        auto_version.fetch_add(1, std::memory_order_release);
        std::fprintf(stderr,
                     "SOTC untrack auto: disabled after a guest write fault inside an untracked page "
                     "(%llu pages so far); untracked pages are released\n",
                     static_cast<unsigned long long>(
                         faults_untracked.load(std::memory_order_relaxed)));
    }
    /// sotc: SOTC_UNTRACK_AUTO report (SOTC FPS line).
    static u64 AutoBytes() {
        return auto_bytes.load(std::memory_order_relaxed);
    }
    static size_t AutoRanges() {
        std::scoped_lock lock{AutoMutex()};
        return AutoActive().size();
    }
    static u64 FaultsUntracked() {
        return faults_untracked.load(std::memory_order_relaxed);
    }
    static bool Tripped() {
        return tripped.load(std::memory_order_relaxed);
    }
    inline static std::atomic<u64> auto_version{0};
    inline static std::atomic<u64> auto_bytes{0};
    inline static std::atomic<u64> faults_untracked{0};
    inline static std::atomic<bool> tripped{false};

private:
    static std::mutex& AutoMutex() {
        static std::mutex m;
        return m;
    }
    static std::vector<Range>& AutoActive() {
        static std::vector<Range> v;
        return v;
    }
    static std::vector<Range> Parse() {
        std::vector<Range> out;
        const char* env = std::getenv("SOTC_UNTRACK_RANGES");
        if (!env || !env[0]) {
            return out;
        }
        // Liverpool::CpAheadMode() read directly (liverpool.h is not included here). The other
        // SOTC_CP_AHEAD modes move the uploads, which this has not been checked against.
        const char* ahead = std::getenv("SOTC_CP_AHEAD");
        if (ahead && std::atoi(ahead) != 0) {
            std::fprintf(stderr, "SOTC untrack: off, SOTC_CP_AHEAD is set\n");
            return out;
        }
        for (const char* p = env; *p;) {
            char* after_addr = nullptr;
            const u64 addr = std::strtoull(p, &after_addr, 16);
            bool ok = after_addr != p && *after_addr == ':';
            if (ok) {
                const char* size_text = after_addr + 1;
                char* after_size = nullptr;
                const u64 size = std::strtoull(size_text, &after_size, 16);
                // The size cap keeps addr + size from wrapping.
                ok = after_size != size_text && size != 0 && size <= (1ULL << 32) &&
                     addr < (1ULL << 48);
                if (ok) {
                    out.push_back(Range{addr, size});
                }
            }
            const char* comma = std::strchr(p, ',');
            if (!comma) {
                break;
            }
            p = comma + 1;
        }
        std::fprintf(stderr, "SOTC untrack: %zu ranges\n", out.size());
        for (const Range& r : out) {
            std::fprintf(stderr, "SOTC untrack: range %llx +%llx\n",
                         static_cast<unsigned long long>(r.addr),
                         static_cast<unsigned long long>(r.size));
        }
        return out;
    }
};

#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
using LockType = Common::AdaptiveMutex;
#else
using LockType = std::mutex;
#endif

/**
 * Allows tracking CPU and GPU modification of pages in a contigious virtual address region.
 * Information is stored in bitsets for spacial locality and fast update of single pages.
 */
class RegionManager {
public:
    explicit RegionManager(PageManager* tracker_, VAddr cpu_addr_)
        : tracker{tracker_}, cpu_addr{cpu_addr_},
          readbacks_mode{EmulatorSettings.GetReadbacksMode()} {
        cpu.Fill(~0ULL);
        gpu.Fill(0ULL);
        ResetGrace();
        ComputeUntracked(); // sotc: SOTC_UNTRACK_RANGES
    }
    explicit RegionManager() = default;

    void SetCpuAddress(VAddr new_cpu_addr) {
        cpu_addr = new_cpu_addr;
        ComputeUntracked(); // sotc: SOTC_UNTRACK_RANGES
        sweep_words.store(~0ULL, std::memory_order_relaxed);
        ResetGrace();
    }

    /// sotc: SOTC_UNTRACK_AUTO (draw thread, via MemoryTracker::ApplyUntracked). Replaces the
    /// untracked mask of the whole region. A page that becomes untracked, has no GPU bit and is
    /// protected (CPU bit 0) is unprotected and CPU-modified now, and its sweep word is raised, so
    /// the next DMA sweep uploads it. A page that stops being untracked keeps its bits: the normal
    /// upload re-protects it.
    void SetUntrackedMask(RegionBits next) {
        const Bounds full = GetBounds(0, HIGHER_PAGE_SIZE);
        RegionBits write_prot{};
        RegionBits read_prot{};
        u64 raised = 0;
        Lock(full);
        for (u64 index = 0; index < NUM_REGION_WORDS; ++index) {
            const u64 added = next[index] & ~untracked[index] & ~gpu[index] & ~cpu[index];
            untracked[index] = next[index];
            if (added) {
                UpdateStateAndProtection<StateOp::Set, StateOp::None>(write_prot, read_prot, index,
                                                                      added);
                raised |= 1ULL << index;
            }
        }
        if (raised) {
            sweep_words.fetch_or(raised, std::memory_order_release);
            Bounds watcher_bounds;
            if (GetWatcherBounds<StateOp::Set, StateOp::None>(full, write_prot, read_prot,
                                                              watcher_bounds)) {
                tracker->UpdatePageWatchersForRegion(cpu_addr, watcher_bounds, write_prot, read_prot,
                                                     PageOp::Untrack, PageOp::None);
            }
        }
        Unlock(full);
    }

    /// sotc: the 256 KiB words where a CPU bit rose (or FaultGrace kept one) since the DMA sweep
    /// last took them (BufferCache::SynchronizeDmaBuffers sweeps only those).
    u64 TakeSweepWords() noexcept {
        return sweep_words.load(std::memory_order_relaxed)
                   ? sweep_words.exchange(0, std::memory_order_acq_rel)
                   : 0;
    }

    VAddr CpuAddress() const noexcept {
        return cpu_addr;
    }

    /// sotc: SOTC_UNTRACK_AUTO self-check (region mutex held). Pages of [offset, offset + size)
    /// that are untracked, have no GPU bit and are protected (CPU bit 0): a guest write fault on one
    /// of them means the untracking did not hold.
    u64 CountUntrackedFaultsLocked(u64 offset, u64 size) noexcept {
        u64 bad = 0;
        IterateWords(GetBounds(offset, size), [&](u64 index, u64 mask) {
            bad += static_cast<u64>(
                std::popcount(untracked[index] & ~gpu[index] & ~cpu[index] & mask));
        });
        return bad;
    }

    /// sotc: a guest write fault on the page at offset (region mutex held, from
    /// MemoryTracker::InvalidateRegion). The second one within 1-2 quanta makes the page hot.
    void NoteGuestWriteFaultLocked(u64 offset) {
        if (HotPages::Enabled()) {
            NoteHotPageFaultLocked(offset);
        }
        if (FaultGrace::Pages() == 0) {
            return;
        }
        const u64 page = offset / BYTES_PER_PAGE;
        const u64 index = page / PAGES_PER_WORD;
        const u64 bit = 1ULL << (page % PAGES_PER_WORD);
        const u32 now = FaultGrace::Now();
        if (grace_any) {
            RotateGraceLocked(now);
        }
        if (!grace_any) {
            grace_epoch = now;
            grace_any = true;
        }
        const bool repeat = ((fault_cur[index] | fault_prev[index]) & bit) != 0;
        fault_cur[index] |= bit;
        if (!repeat || ((hot_cur[index] | hot_prev[index]) & bit) != 0) {
            return;
        }
        if (FaultGrace::TakeBudget(now)) {
            hot_cur[index] |= bit;
            SotcStats::fault_grace_marks.fetch_add(1, std::memory_order_relaxed);
        } else {
            SotcStats::fault_grace_cap_hits.fetch_add(1, std::memory_order_relaxed);
        }
    }

    static constexpr Bounds GetBounds(u64 offset, u64 size) {
        const u64 end_address = offset + size - 1;
        return Bounds{
            .start_word = offset / BYTES_PER_WORD,
            .start_page = (offset) / BYTES_PER_PAGE,
            .end_word = end_address / BYTES_PER_WORD,
            .end_page = (end_address) / BYTES_PER_PAGE,
        };
    }

    static constexpr std::pair<u64, u64> GetMasks(u64 start_page, u64 end_page) {
        const u64 start_mask = ~u64{0} << (start_page & (PAGES_PER_WORD - 1));
        const u64 end_mask = ~u64{0} >> (63 - (end_page & (PAGES_PER_WORD - 1)));
        return std::make_pair(start_mask, end_mask);
    }

    static constexpr void IterateWords(Bounds bounds, auto&& func) {
        const auto [start_word, start_page, end_word, end_page] = bounds;
        const auto [start_mask, end_mask] = GetMasks(start_page, end_page);
        if (start_word == end_word) [[likely]] {
            func(start_word, start_mask & end_mask);
        } else {
            func(start_word, start_mask);
            for (s64 i = start_word + 1; i < end_word; ++i) {
                func(i, ~0ULL);
            }
            func(end_word, end_mask);
        }
    }

    static constexpr void IteratePages(u64 word, auto&& func) {
        u64 offset{};
        while (word != 0) {
            const u64 empty_bits = std::countr_zero(word);
            offset += empty_bits;
            word >>= empty_bits;
            const u64 set_bits = std::countr_one(word);
            func(offset, set_bits);
            word = set_bits < PAGES_PER_WORD ? (word >> set_bits) : 0;
            offset += set_bits;
        }
    }

    /// sotc: retrack (SOTC_UNTRACK_AUTO): the GPU bits are cleared because a download just wrote
    /// the GPU's bytes into guest memory (ApplyDownloads). Only then may an untracked page get its
    /// CPU bit back; a GPU bit dropped without a download must not, or the next bind would upload
    /// stale guest bytes over the GPU's data.
    template <StateOp cpu_op, StateOp gpu_op, bool locked = true>
    void ChangeRegionState(u64 offset, u64 size, bool retrack = false) {
        RegionBits write_prot;
        RegionBits read_prot;
        auto bounds = GetBounds(offset, size);
        Bounds watcher_bounds;
        if constexpr (locked) {
            LockTimed();
        }
        [[maybe_unused]] u64 raised = 0;
        IterateWords(bounds, [&](u64 index, u64 mask) {
            UpdateStateAndProtection<cpu_op, gpu_op>(write_prot, read_prot, index, mask);
            if constexpr (cpu_op == StateOp::Set) {
                if (write_prot[index]) { // CPU bits that went 0 -> 1
                    raised |= 1ULL << index;
                }
            }
        });
        if constexpr (cpu_op == StateOp::Set) {
            if (raised) {
                sweep_words.fetch_or(raised, std::memory_order_release);
            }
        }
        const auto write_op = GetPageOp<Type::CPU>(cpu_op);
        const auto read_op = GetPageOp<Type::GPU>(gpu_op);
        const bool update_watchers = write_op != PageOp::None || read_op != PageOp::None;
        if (update_watchers &&
            GetWatcherBounds<cpu_op, gpu_op>(bounds, write_prot, read_prot, watcher_bounds)) {
            tracker->UpdatePageWatchersForRegion(cpu_addr, watcher_bounds, write_prot, read_prot,
                                                 write_op, read_op);
        }
        if constexpr (cpu_op == StateOp::None && gpu_op == StateOp::Clear) {
            if (retrack) {
                RetrackDownloadedLocked(bounds); // sotc: SOTC_UNTRACK_AUTO (after a download only)
            }
        }
        if constexpr (locked) {
            mutex.unlock();
        }
    }

    /// sotc: SOTC_SYNC_PIPE. Returns true only when the unlocked look below found nothing to do: no
    /// CPU bit in the range and, for gpu_op Set, every GPU bit set. Then func was not called and no
    /// bit changed, so the range was clean before this call (the verdict of IsSyncClean). Every
    /// other path returns false.
    template <Type type, StateOp cpu_op, StateOp gpu_op, bool locked = true>
    bool ForEachModifiedRange(u64 offset, s64 size, auto&& func) {
        // sotc: CPU bits rise only in ChangeRegionState (it marks sweep_words).
        static_assert(!(type == Type::CPU && cpu_op == StateOp::Set));
        auto& state = GetRegionBits<type>();
        // sotc: every draw synchronizes its buffers here, and the region mutex is contended by
        // the guest threads' write faults. Nothing CPU-modified and no GPU state to change is the
        // common case: answered from an unlocked look at the bits (aligned 64-bit loads); a write
        // fault landing right after it is the same as one landing right after the locked scan.
        if constexpr (type == Type::CPU &&
                      (cpu_op == StateOp::Clear || cpu_op == StateOp::None) &&
                      (gpu_op == StateOp::None || gpu_op == StateOp::Set)) {
            bool any_cpu = false;
            bool all_gpu = true;
            IterateWords(GetBounds(offset, size), [&](u64 index, u64 mask) {
                any_cpu |= (__atomic_load_n(&cpu[index], __ATOMIC_ACQUIRE) & mask) != 0;
                if constexpr (gpu_op == StateOp::Set) {
                    all_gpu &= (__atomic_load_n(&gpu[index], __ATOMIC_ACQUIRE) & mask) == mask;
                }
            });
            if (!any_cpu && (gpu_op == StateOp::None || all_gpu)) {
                return true;
            }
        }
        RegionBits write_prot;
        RegionBits read_prot;
        u64 start_page{};
        u64 end_page{};
        auto bounds = GetBounds(offset, size);
        Bounds watcher_bounds;
        if constexpr (locked) {
            mutex.lock();
        }
        // sotc: uploads (<CPU, Clear, None>) keep the CPU bit of hot pages (FaultGrace). Never for
        // written bindings (<CPU, Clear, Set>): keeping the bit there would upload CPU data over
        // GPU results later.
        constexpr bool grace_op =
            type == Type::CPU && cpu_op == StateOp::Clear && gpu_op == StateOp::None;
        if constexpr (grace_op) {
            if (grace_any) {
                RotateGraceLocked(FaultGrace::Now());
            }
            if (sticky_any) {
                // sotc: SOTC_HOT_PAGES
                ExpireStickyLocked(static_cast<u32>(HotPages::NowMs() / HotPages::LifeStepMs()));
            }
        }
        [[maybe_unused]] u64 kept_words = 0;
        IterateWords(bounds, [&](u64 index, u64 mask) {
            const u64 base_page = index * PAGES_PER_WORD;
            const u64 word = state[index] & mask; // collected before the change: hot pages too
            u64 keep = 0;
            if constexpr (grace_op) {
                if (sticky_any) {
                    keep = (sticky[index] | sticky_old[index]) & word; // sotc: SOTC_HOT_PAGES
                    if (keep) {
                        kept_words |= 1ULL << index;
                    }
                }
                if (grace_any) {
                    keep |= (hot_cur[index] | hot_prev[index]) & word;
                    if (keep) {
                        kept_words |= 1ULL << index;
                    }
                    if (keep) {
                        SotcStats::fault_grace_hot_upload_bytes.fetch_add(
                            std::popcount(keep) * BYTES_PER_PAGE, std::memory_order_relaxed);
                    }
                }
                // sotc: SOTC_UNTRACK_RANGES. An untracked page keeps its CPU bit (see SotcUntrack),
                // so it is not re-protected and every bind re-uploads it. A page the GPU wrote is
                // excluded: keeping its CPU bit would upload CPU bytes over the GPU result on every
                // read binding, so it takes the normal path and is protected again.
                const u64 untrack = untracked[index] & word & ~gpu[index];
                if (untrack) {
                    keep |= untrack;
                    kept_words |= 1ULL << index;
                    SotcUntrack::pages_kept.fetch_add(static_cast<u64>(std::popcount(untrack)),
                                                      std::memory_order_relaxed);
                }
            }
            UpdateStateAndProtection<cpu_op, gpu_op>(write_prot, read_prot, index, mask, keep);
            IteratePages(word, [&](u64 pages_offset, u64 pages_size) {
                if (end_page == base_page + pages_offset) {
                    end_page += pages_size;
                    return;
                }
                if (end_page) {
                    func(cpu_addr + start_page * BYTES_PER_PAGE,
                         (end_page - start_page) * BYTES_PER_PAGE);
                }
                start_page = base_page + pages_offset;
                end_page = start_page + pages_size;
            });
        });
        if (end_page) {
            func(cpu_addr + start_page * BYTES_PER_PAGE, (end_page - start_page) * BYTES_PER_PAGE);
        }
        if constexpr (grace_op) {
            // Hot pages keep their CPU bit without a new fault: the next sweep uploads them again.
            if (kept_words) {
                sweep_words.fetch_or(kept_words, std::memory_order_release);
            }
        }
        const auto write_op = GetPageOp<Type::CPU>(cpu_op);
        const auto read_op = GetPageOp<Type::GPU>(gpu_op);
        const bool update_watchers = write_op != PageOp::None || read_op != PageOp::None;
        if (update_watchers &&
            GetWatcherBounds<cpu_op, gpu_op>(bounds, write_prot, read_prot, watcher_bounds)) {
            // sotc: SOTC_PROTECT_BATCH. Only an upload's re-protect (CPU bits cleared, GPU bits
            // untouched) may wait for the batch flush, and only when no page it protects holds
            // GPU data: a readback writing GPU bytes into such a page (WriteUnlessCpuModified)
            // relies on the page being write-protected while it holds this lock, so that a guest
            // write lands after its bytes. Pages without GPU data are only read by the upload that
            // collected them, after its flush (BufferCache::SynchronizeMemoryFull).
            bool deferrable = false;
            if constexpr (type == Type::CPU && cpu_op == StateOp::Clear &&
                          gpu_op == StateOp::None) {
                if (SotcStats::kProtectBatch && PageManager::ProtectBatchOpen()) {
                    deferrable = true;
                    IterateWords(watcher_bounds, [&](u64 index, u64 mask) {
                        deferrable &= (gpu[index] & write_prot[index] & mask) == 0;
                    });
                }
            }
            tracker->UpdatePageWatchersForRegion(cpu_addr, watcher_bounds, write_prot, read_prot,
                                                 write_op, read_op, deferrable);
        }
        if constexpr (locked) {
            mutex.unlock();
        }
        return false;
    }

    template <Type type>
    bool IsRegionModified(u64 offset, u64 size) noexcept {
        auto& state = GetRegionBits<type>();
        const auto [start_word, start_page, end_word, end_page] = GetBounds(offset, size);
        const auto [start_mask, end_mask] = GetMasks(start_page, end_page);
        if (start_word == end_word) [[likely]] {
            return state[start_word] & (start_mask & end_mask);
        } else {
            if (state[start_word] & start_mask) {
                return true;
            }
            for (s64 i = start_word + 1; i < end_word; ++i) {
                if (state[i]) {
                    return true;
                }
            }
            return state[end_word] & end_mask;
        }
    }

    /// sotc: GPU-modified pages in the range, from an unlocked look at the bits (statistics only,
    /// SOTC_RB_NARROW).
    u64 CountGpuModifiedPages(u64 offset, u64 size) noexcept {
        u64 pages = 0;
        IterateWords(GetBounds(offset, size), [&](u64 index, u64 mask) {
            pages += std::popcount(__atomic_load_n(&gpu[index], __ATOMIC_RELAXED) & mask);
        });
        return pages;
    }

    /// sotc: SOTC_SYNC_FAST. Words (256 KiB each) per write generation: 8 = 2 MiB granules.
    static constexpr u64 SYNC_GEN_WORDS = 8;
    static constexpr u64 NUM_SYNC_GENS = NUM_REGION_WORDS / SYNC_GEN_WORDS;

    /// sotc: SOTC_SYNC_FAST. Sum of the write generations of the granules over [offset,
    /// offset + size) (size > 0). Every generation only grows, so an unchanged sum means no
    /// granule moved. Acquire loads: see UpdateStateAndProtection for the ordering.
    u64 SyncGenerationSum(u64 offset, u64 size) const noexcept {
        const auto bounds = GetBounds(offset, size);
        u64 sum = 0;
        for (u64 g = bounds.start_word / SYNC_GEN_WORDS; g <= bounds.end_word / SYNC_GEN_WORDS;
             ++g) {
            sum += sync_gen[g].load(std::memory_order_acquire);
        }
        return sum;
    }

    /// sotc: SOTC_SYNC_FAST. True when no page of [offset, offset + size) is CPU-modified and,
    /// for a written binding, every page is GPU-modified: then ForEachModifiedRange<CPU, Clear,
    /// None or Set> has nothing to do there (its own unlocked early return). Unlocked look at the
    /// bits, as that early return.
    bool IsSyncClean(u64 offset, u64 size, bool is_written) noexcept {
        bool clean = true;
        IterateWords(GetBounds(offset, size), [&](u64 index, u64 mask) {
            clean &= (__atomic_load_n(&cpu[index], __ATOMIC_ACQUIRE) & mask) == 0;
            if (is_written) {
                clean &= (__atomic_load_n(&gpu[index], __ATOMIC_ACQUIRE) & mask) == mask;
            }
        });
        return clean;
    }

    void Lock(const Bounds& bounds) noexcept {
        LockTimed();
    }

    /// sotc: locks the region mutex; with SOTC_FPS, a guest fault being handled on this thread
    /// (SotcStats::FaultStat) counts the wait.
    void LockTimed() noexcept {
        if (SotcStats::kFpsLog && SotcStats::FaultStat::tl.active) {
            const SotcStats::FaultStat::Scope timed{&SotcStats::FaultStat::Tl::mutex_ns};
            mutex.lock();
            return;
        }
        mutex.lock();
    }

    void Unlock(const Bounds& bounds) noexcept {
        mutex.unlock();
    }

private:
    static_assert(NUM_REGION_WORDS == 64);
    std::atomic<u64> sweep_words{~0ULL}; // sotc: see TakeSweepWords
    // sotc: SOTC_SYNC_FAST write generations, one per 2 MiB (see UpdateStateAndProtection).
    std::array<std::atomic<u64>, NUM_SYNC_GENS> sync_gen{};
public:
    /// sotc: SOTC_UNTRACK_RANGES / SOTC_UNTRACK_AUTO. Pure mask builder: the pages of the region
    /// based at cpu_addr that lie in one of the ranges.
    static RegionBits BuildUntrackedMask(VAddr cpu_addr,
                                         const std::vector<SotcUntrack::Range>& ranges) {
        RegionBits mask{};
        mask.Fill(0);
        const u64 region_end = cpu_addr + HIGHER_PAGE_SIZE;
        for (const auto& range : ranges) {
            const u64 lo = std::max<u64>(range.addr, cpu_addr);
            const u64 hi = std::min<u64>(range.addr + range.size, region_end);
            if (lo >= hi) {
                continue;
            }
            const u64 first_page = (lo - cpu_addr) / BYTES_PER_PAGE;
            const u64 last_page = (hi - 1 - cpu_addr) / BYTES_PER_PAGE;
            for (u64 page = first_page; page <= last_page; ++page) {
                mask[page / PAGES_PER_WORD] |= 1ULL << (page % PAGES_PER_WORD);
            }
        }
        return mask;
    }

private:
    /// sotc: SOTC_UNTRACK_RANGES / SOTC_UNTRACK_AUTO. Marks the pages of this region (based at
    /// cpu_addr) that lie in an untracked range. Called when the region is created or moved, before
    /// it is shared.
    void ComputeUntracked() {
        if (SotcUntrack::AutoOn()) {
            u64 version = 0;
            untracked = BuildUntrackedMask(cpu_addr, SotcUntrack::AutoSnapshot(version));
            return;
        }
        untracked = BuildUntrackedMask(cpu_addr, SotcUntrack::Ranges());
    }

    /// sotc: SOTC_UNTRACK_AUTO (region mutex held, after GPU bits were cleared over bounds by a
    /// download or ChangeRegionState<None, Clear>). An untracked page with no GPU bit that is still
    /// protected (CPU bit 0) lost its GPU bit here: it is unprotected and CPU-modified again, as
    /// SetUntrackedMask does. Without this the self-check would count the next guest write to it.
    void RetrackDownloadedLocked(const Bounds& bounds) {
        if (!SotcUntrack::AutoOn()) {
            return;
        }
        RegionBits write_prot{};
        RegionBits read_prot{};
        u64 raised = 0;
        IterateWords(bounds, [&](u64 index, u64 mask) {
            const u64 back = untracked[index] & mask & ~gpu[index] & ~cpu[index];
            if (back) {
                UpdateStateAndProtection<StateOp::Set, StateOp::None>(write_prot, read_prot, index,
                                                                      back);
                raised |= 1ULL << index;
            }
        });
        if (!raised) {
            return;
        }
        sweep_words.fetch_or(raised, std::memory_order_release);
        Bounds watcher_bounds;
        if (GetWatcherBounds<StateOp::Set, StateOp::None>(bounds, write_prot, read_prot,
                                                          watcher_bounds)) {
            tracker->UpdatePageWatchersForRegion(cpu_addr, watcher_bounds, write_prot, read_prot,
                                                 PageOp::Untrack, PageOp::None);
        }
    }

    void ResetGrace() {
        fault_cur.Fill(0);
        fault_prev.Fill(0);
        hot_cur.Fill(0);
        hot_prev.Fill(0);
        grace_epoch = 0;
        grace_any = false;
        ClearStickyLocked();
        sticky.Fill(0);
        sticky_old.Fill(0);
        fault_long_cur.Fill(0);
        fault_long_prev.Fill(0);
        long_epoch = 0;
    }

    /// sotc: SOTC_HOT_PAGES. Drops every sticky page of this region (region mutex held, or the
    /// region not in use yet).
    void ClearStickyLocked() {
        if (!sticky_any) {
            return;
        }
        u32 count = 0;
        for (u64 i = 0; i < NUM_REGION_WORDS; ++i) {
            count += static_cast<u32>(std::popcount(sticky[i]) + std::popcount(sticky_old[i]));
        }
        sticky.Fill(0);
        sticky_old.Fill(0);
        sticky_any = false;
        HotPages::live.fetch_sub(count, std::memory_order_relaxed);
    }

    /// sotc: SOTC_HOT_PAGES. Ages the sticky generations to the quantum `now` (HotPages::
    /// LifeStepMs; region mutex held): the generation marked in the previous quantum becomes the
    /// old one, the old one ends. A page therefore stays sticky for one to two quanta. After a
    /// longer pause nothing is left.
    void ExpireStickyLocked(u32 now) {
        if (!sticky_any || now == sticky_epoch) {
            return;
        }
        const bool adjacent = now - sticky_epoch == 1;
        u32 dropped = 0;
        bool any_left = false;
        for (u64 i = 0; i < NUM_REGION_WORDS; ++i) {
            dropped += static_cast<u32>(std::popcount(sticky_old[i]));
            if (adjacent) {
                sticky_old[i] = sticky[i];
                any_left |= sticky[i] != 0;
            } else {
                dropped += static_cast<u32>(std::popcount(sticky[i]));
                sticky_old[i] = 0;
            }
            sticky[i] = 0;
        }
        sticky_any = any_left;
        sticky_epoch = now;
        if (dropped) {
            HotPages::live.fetch_sub(dropped, std::memory_order_relaxed);
        }
    }

    /// sotc: SOTC_HOT_PAGES. A guest write fault on the page at offset (region mutex held): the
    /// second one within one or two windows makes the page sticky, within the global cap.
    void NoteHotPageFaultLocked(u64 offset) {
        const u64 page = offset / BYTES_PER_PAGE;
        const u64 index = page / PAGES_PER_WORD;
        const u64 bit = 1ULL << (page % PAGES_PER_WORD);
        // A page the GPU wrote: this fault only reads the GPU's bytes back (the page is not marked
        // CPU-modified yet), and the guest's retry faults again. That is one write, not a repeat.
        if ((gpu[index] & bit) != 0) {
            return;
        }
        const u64 ms = HotPages::NowMs();
        HotPages::MaybeShrink(ms);
        ExpireStickyLocked(static_cast<u32>(ms / HotPages::LifeStepMs()));
        const u32 window = static_cast<u32>(ms / HotPages::WindowMs());
        if (window != long_epoch) {
            if (window - long_epoch == 1) {
                fault_long_prev = fault_long_cur;
            } else {
                fault_long_prev.Fill(0);
            }
            fault_long_cur.Fill(0);
            long_epoch = window;
        }
        const bool again = ((fault_long_cur[index] | fault_long_prev[index]) & bit) != 0;
        fault_long_cur[index] |= bit;
        if (!again || ((sticky[index] | sticky_old[index]) & bit) != 0) {
            return;
        }
        if (HotPages::live.load(std::memory_order_relaxed) >= HotPages::Max()) {
            HotPages::NoteRefused();
            return;
        }
        if (!sticky_any) {
            sticky_epoch = static_cast<u32>(ms / HotPages::LifeStepMs());
            sticky_any = true;
        }
        sticky[index] |= bit;
        HotPages::live.fetch_add(1, std::memory_order_relaxed);
        SotcStats::hot_pages_marks.fetch_add(1, std::memory_order_relaxed);
    }

    /// Ages the grace bits to the quantum `now` (region mutex held). Invariant: !grace_any means
    /// every grace bit is 0.
    void RotateGraceLocked(u32 now) {
        if (!grace_any || now == grace_epoch) {
            return;
        }
        const bool adjacent = now - grace_epoch == 1;
        if (adjacent) {
            fault_prev = fault_cur;
            hot_prev = hot_cur;
        } else {
            fault_prev.Fill(0);
            hot_prev.Fill(0);
        }
        fault_cur.Fill(0);
        hot_cur.Fill(0);
        grace_any = adjacent;
        grace_epoch = now;
    }

    template <StateOp cpu_op, StateOp gpu_op>
    void UpdateStateAndProtection(RegionBits& write_prot, RegionBits& read_prot, u64 index,
                                  u64 mask, u64 keep = 0) {
        if constexpr (cpu_op != StateOp::None) {
            const u64 prev = cpu[index];
            if constexpr (cpu_op == StateOp::Clear) {
                cpu[index] &= ~(mask & ~keep); // kept bits: no change, so no write Track
            } else {
                cpu[index] |= mask;
            }
            write_prot[index] = (cpu[index] ^ prev) & mask;
        }
        if constexpr (gpu_op != StateOp::None) {
            const u64 prev = gpu[index];
            if constexpr (gpu_op == StateOp::Clear) {
                gpu[index] &= ~mask;
            } else {
                gpu[index] |= mask;
            }
            read_prot[index] = (gpu[index] ^ prev) & mask;
        }
        // sotc: SOTC_SYNC_FAST. A CPU bit that rose or a GPU bit that fell is a change a cached
        // "range clean" answer must see (BufferCache::SynchronizeMemory): bump the granule's
        // generation AFTER the bit store, with release. A reader that loads the generation
        // (acquire) and sees this bump therefore also sees the new bits; a reader whose load
        // misses it sees a newer generation at its next check. The bump also comes before the
        // page is unprotected (UpdatePageWatchersForRegion, after this), so the guest's own write
        // to the page always happens after it. Every bit transition goes through here: the region
        // mutex is held by every caller (ChangeRegionState, ForEachModifiedRange, or the
        // locked = false callers that took Lock()).
        if constexpr (cpu_op == StateOp::Set || gpu_op == StateOp::Clear) {
            if (SotcStats::kSyncFast) {
                u64 changed = 0;
                if constexpr (cpu_op == StateOp::Set) {
                    changed |= write_prot[index];
                }
                if constexpr (gpu_op == StateOp::Clear) {
                    changed |= read_prot[index];
                }
                if (changed) {
                    sync_gen[index / SYNC_GEN_WORDS].fetch_add(1, std::memory_order_release);
                }
            }
        }
    }

    template <StateOp cpu_op, StateOp gpu_op>
    static bool GetWatcherBounds(const Bounds& bounds, RegionBits& write_prot,
                                 RegionBits& read_prot, Bounds& watcher_bounds) {
        const auto prot = [&](u64 index) {
            u64 word{};
            if constexpr (cpu_op != StateOp::None) {
                word |= write_prot[index];
            }
            if constexpr (gpu_op != StateOp::None) {
                word |= read_prot[index];
            }
            return word;
        };
        u64 start_word = bounds.start_word;
        while (prot(start_word) == 0) {
            if (start_word == bounds.end_word) {
                return false;
            }
            ++start_word;
        }
        u64 end_word = bounds.end_word;
        while (prot(end_word) == 0) {
            --end_word;
        }
        const u64 start_prot = prot(start_word);
        const u64 end_prot = prot(end_word);
        watcher_bounds = Bounds{
            .start_word = start_word,
            .start_page = static_cast<u64>(std::countr_zero(start_prot)),
            .end_word = end_word,
            .end_page = PAGES_PER_WORD - std::countl_zero(end_prot) - 1,
        };
        return true;
    }

    template <Type type>
        requires(std::popcount(std::to_underlying(type)) == 1)
    constexpr PageOp GetPageOp(StateOp state_op) {
        if constexpr (type == Type::CPU) {
            if (state_op == StateOp::Set) {
                return PageOp::Untrack;
            } else if (state_op == StateOp::Clear) {
                return PageOp::Track;
            }
        } else if (type == Type::GPU && readbacks_mode == GpuReadbacksMode::Precise) {
            if (state_op == StateOp::Set) {
                return PageOp::Track;
            } else if (state_op == StateOp::Clear) {
                return PageOp::Untrack;
            }
        }
        return PageOp::None;
    }

    template <Type type>
        requires(std::popcount(std::to_underlying(type)) == 1)
    RegionBits& GetRegionBits() noexcept {
        if constexpr (type == Type::CPU) {
            return cpu;
        } else if constexpr (type == Type::GPU) {
            return gpu;
        }
    }

    PageManager* tracker;
    VAddr cpu_addr{};
    u32 readbacks_mode;
    RegionBits cpu;
    RegionBits gpu;
    // sotc: FaultGrace state (two rotating quanta).
    RegionBits fault_cur;
    RegionBits fault_prev;
    RegionBits hot_cur;
    RegionBits hot_prev;
    u32 grace_epoch{};
    bool grace_any{false};
    // sotc: SOTC_HOT_PAGES state (see HotPages).
    RegionBits fault_long_cur;
    RegionBits fault_long_prev;
    RegionBits sticky;     // marked in the current life quantum
    RegionBits sticky_old; // marked in the previous one
    u32 long_epoch{};
    u32 sticky_epoch{};
    bool sticky_any{false};
    RegionBits untracked{}; // sotc: SOTC_UNTRACK_RANGES (ComputeUntracked), zero when unset
    LockType mutex;
};

} // namespace VideoCore
