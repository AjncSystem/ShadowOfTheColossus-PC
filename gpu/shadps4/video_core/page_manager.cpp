// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>
#include <vector>
#include <utility>
#include <boost/icl/interval_map.hpp>
#include <boost/icl/interval_set.hpp>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include "common/alignment.h"
#include "common/adaptive_mutex.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/error.h"
#include "common/multi_level_page_table.h"
#include "common/signal_context.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "common/decoder.h"
#include "core/signals.h"
#include "shader_recompiler/ir/passes/srt.h"
#include "video_core/page_manager.h"
#include "video_core/sotc_stats.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

// src/runtime_memory.c: the runtime's view of a page next to the host protection (diagnostics).
extern "C" void runtime_memory_describe(uintptr_t address, char* out, size_t capacity);

#ifndef _WIN64
#include <sys/mman.h>
#else
#include <windows.h>
#endif
#include <string>

// sotc: image base for module-relative addresses in logs (0 where unknown).
#ifdef _WIN64
extern "C" IMAGE_DOS_HEADER __ImageBase;
static u64 ImageBaseAddress() {
    return reinterpret_cast<u64>(&__ImageBase);
}
#else
static u64 ImageBaseAddress() {
    return 0;
}
#endif

#ifdef __linux__
#include <thread>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#else
#include "common/spin_lock.h"
#endif

namespace VideoCore {

namespace {
// sotc: the guest stack registry lock. SignalImpl::Protect holds it SHARED across the host
// protect, so a stack registered meanwhile can never be protected after RegisterGuestStack made it
// accessible (the old code released the lock before protecting: a just-registered fiber stack could
// be write-protected by an in-flight GPU protect, and a fault there cannot push its exception frame:
// silent death at startup). Writers never block to acquire it (LockStacksExclusive spins on try)
// and only edit the map while holding it, so no wait cycle can include this lock.
struct StackLock {
#ifdef _WIN32
    SRWLOCK l = SRWLOCK_INIT;
    void lock_shared() { AcquireSRWLockShared(&l); }
    bool try_lock_shared() { return TryAcquireSRWLockShared(&l) != 0; }
    void unlock_shared() { ReleaseSRWLockShared(&l); }
    bool try_lock() { return TryAcquireSRWLockExclusive(&l) != 0; }
    void unlock() { ReleaseSRWLockExclusive(&l); }
#else
    std::shared_mutex m;
    void lock_shared() { m.lock_shared(); }
    bool try_lock_shared() { return m.try_lock_shared(); }
    void unlock_shared() { m.unlock_shared(); }
    bool try_lock() { return m.try_lock(); }
    void unlock() { m.unlock(); }
#endif
};
StackLock g_guest_stacks_mutex;
/// SOTC_STACK_GUARD=0: the old early release in SignalImpl::Protect (A/B).
int StackGuardMode() {
    static const int v = [] {
        const char* e = std::getenv("SOTC_STACK_GUARD");
        return e ? std::atoi(e) : 1;
    }();
    return v;
}
/// Exclusive hold of the stack registry, acquired without ever blocking (see StackLock).
std::unique_lock<StackLock> LockStacksExclusive() {
    for (u32 spin = 0;; ++spin) {
        if (g_guest_stacks_mutex.try_lock()) {
            return std::unique_lock<StackLock>{g_guest_stacks_mutex, std::adopt_lock};
        }
        SotcStats::stack_reg_spins.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
        if (spin < 64) {
            YieldProcessor();
        } else if (spin < 512) {
            SwitchToThread();
        } else {
            Sleep(1);
        }
#else
        std::this_thread::yield();
#endif
    }
}
// Reference counted: the margin below one stack often covers a neighbouring stack (games pack
// fiber stacks back to back), so finalizing one must not drop the other's protection.
boost::icl::interval_map<VAddr, u32> g_guest_stacks;
// sotc: bumped (under the exclusive lock) whenever g_guest_stacks changes; readers keep a sorted
// per-thread copy of its segments and refresh it only when this moves (StackSegments).
std::atomic<u64> g_guest_stacks_generation{1};
// Re-applies the tracker's protection to pages that stop being stack pages.
std::function<void(VAddr, u64)> g_reapply_protection;

// sotc: SOTC_PROTECT_BATCH (page_manager.h, PageManager::ProtectBatch). A collected protection
// change: pages [page, page + pages) were left by their tracker update in state perms.
struct PendingProtect {
    u64 page;
    u64 pages;
    Core::MemoryPermission perms;
};
struct ProtectBatchState {
    u32 depth = 0;     ///< open ProtectBatch scopes on this thread
    u32 suspended = 0; ///< faults being handled on this thread (their updates protect at once)
    std::vector<PendingProtect> runs;
    // sotc: SOTC_PROTECT_COALESCE (PageManager::FaultProtectScope): the unprotects a guest write
    // fault collects. Always read-write runs (the perms field is kept for the shared flush).
    u32 fault_depth = 0;
    std::vector<PendingProtect> fault_runs;
};
thread_local ProtectBatchState tl_protect_batch;
// Applies this thread's collected changes (the tracker instance's FlushProtectBatch).
std::function<void()> g_protect_batch_flush;
// Applies this thread's fault runs, then makes the page writable (FlushFaultProtects).
std::function<void(VAddr)> g_fault_protect_flush;
// Makes one page writable if the tracker says so and the host does not (EnsurePageWritable).
std::function<void(VAddr)> g_fault_page_fix;

/// sotc: SOTC_PROTECT_COALESCE: pages of a hole between two runs of one protection that a flush
/// bridges with a single call (SOTC_PROTECT_GAP, default 8; 0 = no bridging, also when the switch
/// is off). A bridged page is protected only if its own state is that protection, under its page
/// lock, as every page of a run is, so bridging never changes what a page ends up with.
u64 CoalesceGapPages() {
    static const u64 v = [] {
        if (!SotcStats::kProtectCoalesce) {
            return u64{0};
        }
        const char* e = std::getenv("SOTC_PROTECT_GAP");
        return e ? std::min<u64>(std::strtoull(e, nullptr, 10), 64) : u64{8};
    }();
    return v;
}
} // namespace

// The exception frame is written below the faulting stack pointer, so a nearly full stack also
// needs the memory just under it to stay accessible.
constexpr u64 GuestStackMargin = 16_KB;

void RegisterGuestStack(VAddr address, u64 size) {
    if (size == 0) {
        return;
    }
    // Whole pages: protection is page granular, so a page shared with a stack is a stack page
    // for the tracker too (never protected, never written back over).
    const VAddr begin = Common::AlignDown(address - GuestStackMargin, VAddr{4096});
    const VAddr end = Common::AlignUp(address + size, VAddr{4096});
    {
        auto lk = LockStacksExclusive(); // in-flight SignalImpl::Protect calls hold it shared
        g_guest_stacks += std::make_pair(boost::icl::interval<VAddr>::right_open(begin, end), 1u);
        g_guest_stacks_generation.fetch_add(1, std::memory_order_release);
    }
    // Protects that computed their ranges before the insert have finished their host protect
    // (they held the lock shared); later ones see the range. Make it accessible now.
    Core::Memory::Instance()->GetAddressSpace().Protect(begin, end - begin,
                                                        Core::MemoryPermission::ReadWrite);
}

void UnregisterGuestStack(VAddr address, u64 size) {
    if (size == 0) {
        return;
    }
    const VAddr begin = Common::AlignDown(address - GuestStackMargin, VAddr{4096});
    const VAddr end = Common::AlignUp(address + size, VAddr{4096});
    {
        auto lk = LockStacksExclusive();
        g_guest_stacks -= std::make_pair(boost::icl::interval<VAddr>::right_open(begin, end), 1u);
        g_guest_stacks_generation.fetch_add(1, std::memory_order_release);
    }
    // The pages were left accessible while they were a stack: give the tracker its protection
    // back, or CPU writes there would go unnoticed (pages still used by another stack stay).
    if (g_reapply_protection) {
        g_reapply_protection(begin, end - begin);
    }
}

namespace {
// sotc: the stack segments as a sorted flat array, one copy per thread. Guest stacks change only
// when a guest thread or fiber is created or destroyed, while the readback and fault paths ask
// for every range they write: a shared lock plus an interval-tree lookup (and an interval_set
// built and destroyed) per call showed up as ~8% of the draw thread while streaming. The copy is
// refreshed only when g_guest_stacks_generation moved.
const std::vector<std::pair<VAddr, VAddr>>& StackSegments() {
    struct Cache {
        u64 generation = 0;
        std::vector<std::pair<VAddr, VAddr>> segments;
    };
    thread_local Cache cache;
    if (cache.generation != g_guest_stacks_generation.load(std::memory_order_acquire)) {
        std::shared_lock lk{g_guest_stacks_mutex};
        cache.segments.clear();
        for (const auto& [interval, count] : g_guest_stacks) {
            cache.segments.emplace_back(interval.lower(), interval.upper());
        }
        // Read under the lock: a change after it bumps the generation again.
        cache.generation = g_guest_stacks_generation.load(std::memory_order_acquire);
    }
    return cache.segments;
}
} // namespace

bool IntersectsGuestStack(VAddr address, u64 size) {
    const VAddr end = address + size;
    const auto& segments = StackSegments();
    const auto it = std::ranges::partition_point(
        segments, [address](const std::pair<VAddr, VAddr>& seg) { return seg.second <= address; });
    return it != segments.end() && it->first < end;
}

// sotc: func is a NonStackRangeFn (non-owning) instead of const std::function&; see page_manager.h.
void ForEachNonStackRange(VAddr address, u64 size, NonStackRangeFn func) {
    if (size == 0) {
        return;
    }
    const VAddr end = address + size;
    const auto& segments = StackSegments();
    // First segment ending after address (segments are sorted and disjoint).
    auto it = std::ranges::partition_point(
        segments, [address](const std::pair<VAddr, VAddr>& seg) { return seg.second <= address; });
    if (it == segments.end() || it->first >= end) {
        // Common case: nothing of the range is a stack.
        func(address, size);
        return;
    }
    static std::atomic<u32> reported{0};
    if (reported++ < 8) {
        LOG_WARNING(Render, "Not writing GPU data back over guest stack pages in {:#x}+{:#x}",
                    address, size);
    }
    VAddr cursor = address;
    for (; it != segments.end() && it->first < end; ++it) {
        if (it->first > cursor) {
            func(cursor, it->first - cursor);
        }
        cursor = std::max(cursor, it->second);
    }
    if (cursor < end) {
        func(cursor, end - cursor);
    }
}

namespace {
// sotc: SOTC_FAULT_OSCOST (MeasureFaultOsCost): the private page the measurement faults on, and
// the clock readings its handler leaves. The handler is registered at start-up when the switch is
// on (the handler list must not change while threads dispatch faults).
std::atomic<bool> g_oscost_registered{false};
std::atomic<uintptr_t> g_oscost_page{0};
std::atomic<u64> g_oscost_enter_ns{0};
std::atomic<u64> g_oscost_exit_ns{0};

bool OsCostFaultHandler(void*, void* address) {
    const uintptr_t page = g_oscost_page.load(std::memory_order_acquire);
    const auto fault = reinterpret_cast<uintptr_t>(address);
    if (page == 0 || fault < page || fault >= page + 4096) {
        return false;
    }
    // Entry of bbgpu_handle_fault when SOTC_FPS timed it (the dispatch that got here is part of
    // what the operating system and the loader add to a fault).
    const auto& tl = SotcStats::FaultStat::tl;
    // sotc: SOTC_FPS_LEAN leaves t0 at 0 (the fault is not timed): take the clock here instead.
    g_oscost_enter_ns.store(tl.active && tl.t0 ? tl.t0 : SotcStats::NowNs(),
                            std::memory_order_relaxed);
#ifdef _WIN64
    DWORD old{};
    VirtualProtect(reinterpret_cast<void*>(page), 4096, PAGE_READWRITE, &old);
#endif
    g_oscost_exit_ns.store(SotcStats::NowNs(), std::memory_order_relaxed);
    return true;
}
} // namespace

struct PageManager::Impl {
    struct PageState {
        u8 num_write_watchers;
        u8 num_read_watchers;

        Core::MemoryPermission WritePerm() const noexcept {
            return num_write_watchers == 0 ? Core::MemoryPermission::Write
                                           : Core::MemoryPermission::None;
        }

        Core::MemoryPermission ReadPerm() const noexcept {
            return num_read_watchers == 0 ? Core::MemoryPermission::Read
                                          : Core::MemoryPermission::None;
        }

        Core::MemoryPermission Perms() const noexcept {
            return ReadPerm() | WritePerm();
        }

        template <bool is_read>
        u8 GetPage() const {
            if constexpr (is_read) {
                return num_read_watchers;
            } else {
                return num_write_watchers;
            }
        }

        constexpr Core::MemoryPermission Update(PageOp write_op, bool update_write = true,
                                                PageOp read_op = PageOp::None,
                                                bool update_read = false) {
            if (update_read) {
                if (read_op == PageOp::Track) {
                    ASSERT_MSG(num_read_watchers == 0, "Too many watchers");
                } else if (read_op == PageOp::Untrack) {
                    ASSERT_MSG(num_read_watchers > 0, "Not enough watchers");
                }
                num_read_watchers += std::to_underlying(read_op);
            }
            if (update_write) {
                if (write_op == PageOp::Track) {
                    ASSERT_MSG(num_write_watchers < 255, "Too many watchers");
                } else if (write_op == PageOp::Untrack) {
                    ASSERT_MSG(num_write_watchers > 0, "Not enough watchers");
                }
                num_write_watchers += std::to_underlying(write_op);
            }
            return Perms();
        }
    };

    static constexpr size_t ADDRESS_BITS = 40;
    static constexpr size_t NUM_ADDRESS_PAGES = 1ULL << (40 - PM_PAGE_BITS);
    static constexpr size_t NUM_ADDRESS_LOCKS = NUM_ADDRESS_PAGES / NUM_REGION_PAGES;
    inline static Vulkan::Rasterizer* rasterizer;

    Impl() {
        // sotc: SOTC_PROTECT_BATCH: the batches of every thread flush through this instance.
        g_protect_batch_flush = [this] { FlushProtectBatch(); };
        g_fault_protect_flush = [this](VAddr fault_page) { FlushFaultProtects(fault_page); };
        g_fault_page_fix = [this](VAddr page) { EnsurePageWritable(page >> PM_PAGE_BITS); };
    }
    virtual ~Impl() = default;

    virtual void OnMap(VAddr address, size_t size) {
        // No-op
        EnsurePages(address, address + size);
    }

    virtual void OnUnmap(VAddr address, size_t size) {
        // No-op
    }

    virtual void Protect(VAddr address, size_t size, Core::MemoryPermission perms) = 0;

    /// Re-applies the protection implied by the watchers of every page in the range, after the
    /// host mapping was recreated with its default protection.
    void ReapplyProtection(VAddr address, u64 size) {
        const u64 page_start = address >> PM_PAGE_BITS;
        const u64 page_end = std::min<u64>(Common::DivCeil(address + size, PM_PAGE_SIZE),
                                           NUM_ADDRESS_PAGES);
        std::vector<u64> locked_pages;
        u64 run_begin{};
        u64 run_pages{};
        Core::MemoryPermission run_perms{};
        const auto flush = [&] {
            if (run_pages != 0) {
                Protect(run_begin << PM_PAGE_BITS, run_pages << PM_PAGE_BITS, run_perms);
                run_pages = 0;
            }
        };
        for (u64 page = page_start; page < page_end; ++page) {
            PageState* state = cached_pages.find(page);
            if (!state) {
                flush();
                continue;
            }
            locks[page].lock();
            locked_pages.push_back(page);
            const auto perms = state->Perms();
            if (perms == Core::MemoryPermission::ReadWrite) {
                flush();
                continue;
            }
            if (run_pages != 0 && perms == run_perms && run_begin + run_pages == page) {
                ++run_pages;
            } else {
                flush();
                run_begin = page;
                run_pages = 1;
                run_perms = perms;
            }
        }
        flush();
        for (const u64 page : locked_pages) {
            locks[page].unlock();
        }
    }

    void EnsurePages(VAddr begin, VAddr end) {
        end = std::min(end, VAddr{1} << ADDRESS_BITS) - 1;
        const size_t start_page = begin >> PM_PAGE_BITS;
        const size_t end_page = end >> PM_PAGE_BITS;
        cached_pages.reserve(start_page, end_page);
        locks.reserve(start_page, end_page);
    }

    void UpdatePageWatchers(VAddr addr, u64 size, PageOp write_op) {
        const u64 page_start = addr >> PM_PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PM_PAGE_SIZE);

        Core::MemoryPermission perms{};
        u64 range_begin = page_start;
        u64 range_pages = 0;
        u64 potential_pages = 0;
        // sotc: SOTC_PROTECT_COALESCE. A pending run whose changed pages all went from read-only
        // to read-write is collected by this thread's fault scope (PageManager::FaultProtectScope).
        const bool fault_collect = tl_protect_batch.fault_depth != 0;
        bool unprotect_only = true;

        const auto release_pending = [&] {
            if (range_pages > 0) {
                if (fault_collect && unprotect_only) {
                    CollectFaultProtect(range_begin, range_pages);
                    range_pages = 0;
                    potential_pages = 0;
                    return;
                }
                if (SotcStats::kCmdStats) SotcStats::sys_protect_src[0].fetch_add(1, std::memory_order_relaxed);
                Protect(range_begin << PM_PAGE_BITS, range_pages << PM_PAGE_BITS, perms);
                range_pages = 0;
                potential_pages = 0;
            }
        };

        // Iterate requested pages
        const u64 aligned_addr = page_start << PM_PAGE_BITS;
        const u64 aligned_end = page_end << PM_PAGE_BITS;
        if (!rasterizer->IsMapped(aligned_addr, aligned_end - aligned_addr)) {
            // sotc: frequent while the game streams; the first few are enough.
            static std::atomic<u32> reported{0};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 8) {
                LOG_WARNING(Render,
                            "Tracking memory region {:#x} - {:#x} which is not fully GPU mapped.",
                            aligned_addr, aligned_end);
            }
            EnsurePages(aligned_addr, aligned_end);
        }

        for (u64 page = page_start; page != page_end; ++page) {
            PageState* state = cached_pages.find(page);
            if (!state) {
                continue;
            }

            locks[page].lock();

            const auto old_perms = state->Perms();
            if (page == page_start) {
                perms = old_perms;
            }

            // Apply the change to the page state
            const auto new_perms = state->Update(write_op);
            if (new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_pages != 0) {
                ++potential_pages;
            }

            // If the page must be (un)protected
            if (new_perms != old_perms) {
                if (range_pages == 0) {
                    // Start a new potential range
                    range_begin = page;
                    potential_pages = 1;
                    unprotect_only = true;
                }
                unprotect_only &= old_perms == Core::MemoryPermission::Read &&
                                  new_perms == Core::MemoryPermission::ReadWrite;
                // Extend current range up to potential range
                range_pages = potential_pages;
            }
        }

        // Add pending (un)protect action
        release_pending();

        for (u64 page = page_start; page != page_end; ++page) {
            if (auto* lock = locks.find(page)) {
                lock->unlock();
            }
        }
    }

    void UpdatePageWatchersForRegion(VAddr base_addr, const Bounds& bounds,
                                     const RegionBits& write_mask, const RegionBits& read_mask,
                                     PageOp write_op, PageOp read_op, bool deferrable = false) {
        const u64 base_page = base_addr >> PM_PAGE_BITS;
        const u64 page_start = bounds.start_word * PAGES_PER_WORD + bounds.start_page;
        const u64 page_end = bounds.end_word * PAGES_PER_WORD + bounds.end_page + 1;

        Core::MemoryPermission perms{};
        u64 range_begin = base_page + page_start;
        u64 range_pages = 0;
        u64 potential_pages = 0;
        // sotc: SOTC_PROTECT_BATCH. Collected by this thread's open batch instead (the caller
        // only passes deferrable for re-protects, see PageManager::ProtectBatch).
        const bool defer = deferrable && PageManager::ProtectBatchOpen();
        // sotc: SOTC_PROTECT_COALESCE, see UpdatePageWatchers.
        const bool fault_collect = tl_protect_batch.fault_depth != 0;
        bool unprotect_only = true;

        const auto release_pending = [&] {
            if (range_pages > 0) {
                if (defer) {
                    CollectProtect(range_begin, range_pages, perms);
                    range_pages = 0;
                    potential_pages = 0;
                    return;
                }
                if (fault_collect && unprotect_only) {
                    CollectFaultProtect(range_begin, range_pages);
                    range_pages = 0;
                    potential_pages = 0;
                    return;
                }
                if (SotcStats::kCmdStats) SotcStats::sys_protect_src[1].fetch_add(1, std::memory_order_relaxed);
                Protect(range_begin << PM_PAGE_BITS, range_pages << PM_PAGE_BITS, perms);
                range_pages = 0;
                potential_pages = 0;
            }
        };

        for (u64 page = page_start; page != page_end; ++page) {
            PageState* state = cached_pages.find(base_page + page);
            if (!state) {
                continue;
            }

            locks[base_page + page].lock();

            const auto old_perms = state->Perms();
            if (page == page_start) {
                perms = old_perms;
            }

            // Apply the change to the page state
            const bool update_write = write_op != PageOp::None && write_mask.GetPage(page);
            const bool update_read = read_op != PageOp::None && read_mask.GetPage(page);
            const auto new_perms = state->Update(write_op, update_write, read_op, update_read);

            if (new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_pages != 0) {
                // If the protection did not change, extend the potential range
                ++potential_pages;
            }

            // If the page must be (un)protected
            if (new_perms != old_perms) {
                if (range_pages == 0) {
                    // Start a new potential range
                    range_begin = base_page + page;
                    potential_pages = 1;
                    unprotect_only = true;
                }
                unprotect_only &= old_perms == Core::MemoryPermission::Read &&
                                  new_perms == Core::MemoryPermission::ReadWrite;
                // Extend current rango up to potential range
                range_pages = potential_pages;
            }
        }

        // Add pending (un)protect action
        release_pending();

        for (u64 page = page_start; page != page_end; ++page) {
            if (auto* lock = locks.find(base_page + page)) {
                lock->unlock();
            }
        }
    }

    /// sotc: SOTC_PROTECT_BATCH. Adds the run to this thread's batch (page locks of the run held
    /// by the caller; the batch itself is thread-local). A run touching or overlapping the last
    /// one with the same protection extends it.
    static void CollectProtect(u64 page, u64 pages, Core::MemoryPermission perms) {
        if (SotcStats::kFpsLog) {
            SotcStats::protect_batch_requests.fetch_add(1, std::memory_order_relaxed);
        }
        auto& runs = tl_protect_batch.runs;
        if (!runs.empty()) {
            PendingProtect& last = runs.back();
            if (last.perms == perms && page <= last.page + last.pages &&
                last.page <= page + pages) {
                const u64 begin = std::min(last.page, page);
                const u64 end = std::max(last.page + last.pages, page + pages);
                last.page = begin;
                last.pages = end - begin;
                return;
            }
        }
        runs.push_back({page, pages, perms});
    }

    /// sotc: SOTC_PROTECT_COALESCE. Adds a read-write run to this thread's fault scope (page locks
    /// of the run held by the caller; the list is thread-local).
    static void CollectFaultProtect(u64 page, u64 pages) {
        if (SotcStats::kFpsLog) {
            SotcStats::coalesce_requests.fetch_add(1, std::memory_order_relaxed);
        }
        auto& runs = tl_protect_batch.fault_runs;
        if (!runs.empty()) {
            PendingProtect& last = runs.back();
            if (page <= last.page + last.pages && last.page <= page + pages) {
                const u64 begin = std::min(last.page, page);
                const u64 end = std::max(last.page + last.pages, page + pages);
                last.page = begin;
                last.pages = end - begin;
                return;
            }
        }
        runs.push_back({page, pages, Core::MemoryPermission::ReadWrite});
    }

    struct FlushResult {
        u64 calls = 0;
        u64 pages = 0;
        u64 redundant = 0;
    };

    /// sotc: applies collected runs (SOTC_PROTECT_BATCH's, or a fault scope's) and empties them.
    /// Each run's pages are locked (ascending, as every tracker update locks them; no other lock
    /// is held here) and a page is protected only while its watcher state still gives the
    /// collected protection, under its page lock, as the update would have done. A page whose
    /// state moved since is skipped: the update that moved it protected it from the state it left
    /// (under the same page lock, after ours), or collected it in its own batch. So the host
    /// protection of every page still ends as its last update's state, as without batching.
    /// gap > 0 (SOTC_PROTECT_COALESCE): runs of one protection at most gap pages apart become one
    /// run; the pages between them are checked like any other page of a run, so a page in the hole
    /// that is not in that protection splits the run again and is left alone.
    FlushResult FlushRuns(std::vector<PendingProtect>& runs, u64 gap) {
        FlushResult result;
        std::ranges::sort(runs, [](const PendingProtect& a, const PendingProtect& b) {
            return a.page != b.page ? a.page < b.page
                                    : std::to_underlying(a.perms) < std::to_underlying(b.perms);
        });
        std::size_t merged = 0;
        for (std::size_t i = 0; i < runs.size(); ++i) {
            // Joins an earlier run of the same protection that ends within gap pages (runs are
            // sorted by start, so only the last few can be near enough). gap 0: only a run that
            // touches or overlaps, and only the previous one.
            bool joined = false;
            const std::size_t span = gap == 0 ? 1 : 4;
            const std::size_t lowest = merged > span ? merged - span : 0;
            for (std::size_t j = merged; j-- > lowest && !joined;) {
                PendingProtect& earlier = runs[j];
                if (earlier.perms == runs[i].perms &&
                    earlier.page + earlier.pages + gap >= runs[i].page) {
                    earlier.pages = std::max(earlier.page + earlier.pages,
                                             runs[i].page + runs[i].pages) -
                                    earlier.page;
                    joined = true;
                }
            }
            if (!joined) {
                runs[merged++] = runs[i];
            }
        }
        thread_local std::vector<u64> locked_pages;
        for (std::size_t i = 0; i < merged; ++i) {
            const PendingProtect run = runs[i];
            const u64 run_end = run.page + run.pages;
            locked_pages.clear();
            for (u64 page = run.page; page < run_end; ++page) {
                if (cached_pages.find(page)) {
                    locks[page].lock();
                    locked_pages.push_back(page);
                }
            }
            u64 begin = 0;
            u64 count = 0;
            const auto emit = [&] {
                if (count != 0) {
                    if (SotcStats::kCmdStats) {
                        SotcStats::sys_protect_src[1].fetch_add(1, std::memory_order_relaxed);
                    }
                    Protect(begin << PM_PAGE_BITS, count << PM_PAGE_BITS, run.perms);
                    ++result.calls;
                    result.pages += count;
                    count = 0;
                }
            };
            for (const u64 page : locked_pages) {
                const PageState* state = cached_pages.find(page);
                if (state->Perms() != run.perms) {
                    ++result.redundant;
                    emit();
                    continue;
                }
                if (count != 0 && begin + count != page) {
                    emit();
                }
                if (count == 0) {
                    begin = page;
                }
                ++count;
            }
            emit();
            for (const u64 page : locked_pages) {
                locks[page].unlock();
            }
        }
        runs.clear();
        return result;
    }

    /// sotc: SOTC_PROTECT_BATCH. Applies this thread's collected runs (FlushRuns).
    void FlushProtectBatch() {
        auto& runs = tl_protect_batch.runs;
        if (runs.empty()) {
            return;
        }
        const FlushResult result = FlushRuns(runs, CoalesceGapPages());
        if (SotcStats::kFpsLog) {
            SotcStats::protect_batch_calls.fetch_add(result.calls, std::memory_order_relaxed);
            SotcStats::protect_batch_pages.fetch_add(result.pages, std::memory_order_relaxed);
            SotcStats::protect_batch_redundant.fetch_add(result.redundant,
                                                         std::memory_order_relaxed);
        }
    }

    /// sotc: SOTC_PROTECT_COALESCE. Applies this thread's fault runs (FlushRuns), then, if
    /// fault_page (0: none) was not part of them, makes it writable when its tracker state says
    /// so. That page may have been covered by another thread's pending flush (its CPU bit was set
    /// by that thread's update, so this fault's own update changed nothing): applying the state
    /// here keeps every faulting thread independent of the others' flushes.
    void FlushFaultProtects(VAddr fault_page) {
        auto& runs = tl_protect_batch.fault_runs;
        const u64 page = fault_page >> PM_PAGE_BITS;
        bool covered = false;
        for (const PendingProtect& run : runs) {
            covered |= page >= run.page && page < run.page + run.pages;
        }
        if (!runs.empty()) {
            const FlushResult result = FlushRuns(runs, CoalesceGapPages());
            if (SotcStats::kFpsLog) {
                SotcStats::coalesce_calls.fetch_add(result.calls, std::memory_order_relaxed);
                SotcStats::coalesce_pages.fetch_add(result.pages, std::memory_order_relaxed);
                SotcStats::coalesce_skipped.fetch_add(result.redundant, std::memory_order_relaxed);
            }
        }
        if (fault_page != 0 && !covered) {
            EnsurePageWritable(page);
        }
    }

    /// sotc: SOTC_PROTECT_COALESCE. Applies the tracker's state to one page when it says
    /// read-write and the host disagrees. Run for EVERY handled write fault (of a guest thread
    /// whose runs did not cover the page, of the command processor, of a fault that did not
    /// collect), so that no thread's progress depends on another thread's pending flush: the page
    /// may have been covered by a flush another thread has not applied yet (its update set the CPU
    /// bit, so this fault's own update changed nothing). Page lock held across the check and the
    /// protect, as every update; a state that is not read-write is left to the update that set it.
    void EnsurePageWritable(u64 page) {
        if (!cached_pages.find(page)) {
            return;
        }
        locks[page].lock();
        if (cached_pages.find(page)->Perms() == Core::MemoryPermission::ReadWrite) {
#ifdef _WIN32
            // The query takes no exclusive lock on the address space, a protect does: ask first
            // (an update that was applied at once, not collected, left the page writable).
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<void*>(page << PM_PAGE_BITS), &info, sizeof(info)) &&
                (info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                                 PAGE_EXECUTE_WRITECOPY)) != 0) {
                locks[page].unlock();
                return;
            }
#endif
            if (SotcStats::kFpsLog) {
                SotcStats::coalesce_self_fixes.fetch_add(1, std::memory_order_relaxed);
            }
            Protect(page << PM_PAGE_BITS, PM_PAGE_SIZE, Core::MemoryPermission::ReadWrite);
        }
        locks[page].unlock();
    }

    struct PageTraits {
        using Entry = PageState;
        static constexpr size_t ADDRESS_SPACE_BITS = ADDRESS_BITS;
        static constexpr size_t L1_BITS = 16;
        static constexpr size_t PAGE_BITS = PM_PAGE_BITS;
        static constexpr bool NULL_CHECK = false;
    };
    Common::MultiLevelPageTable<PageTraits> cached_pages;
    struct MutexTraits {
#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
        using Entry = Common::AdaptiveMutex;
#else
        using Entry = std::mutex;
#endif
        static constexpr size_t ADDRESS_SPACE_BITS = ADDRESS_BITS;
        static constexpr size_t L1_BITS = 16;
        static constexpr size_t PAGE_BITS = PM_PAGE_BITS;
        static constexpr bool NULL_CHECK = false;
    };
    Common::MultiLevelPageTable<MutexTraits> locks;
};

#ifdef __linux__
struct UffdImpl : public PageManager::Impl {
private:
    std::jthread ufd_thread;
    int uffd;

public:
    UffdImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;
        uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
        if (uffd == -1) {
            LOG_ERROR(Common_Memory,
                      "userfaultfd syscall failed: {}, falling back to signal implementation",
                      Common::GetLastErrorMsg());
            throw std::runtime_error("userfaultfd");
        }

        // Request uffdio features from kernel.
        uffdio_api api;
        api.api = UFFD_API;
        api.features = UFFD_FEATURE_THREAD_ID;
        const int ret = ioctl(uffd, UFFDIO_API, &api);
        if (ret != 0) {
            LOG_ERROR(Common_Memory,
                      "uffdio_api call failed: {}, falling back to signal implementation",
                      Common::GetLastErrorMsg());
            throw std::runtime_error("uffdio_api");
        }

        // Create uffd handler thread
        ufd_thread = std::jthread([&](std::stop_token token) { UffdHandler(token); });
    }

    ~UffdImpl() = default;

    void OnMap(VAddr address, size_t size) override {
        PageManager::Impl::OnMap(address, size);
        uffdio_register reg;
        reg.range.start = address;
        reg.range.len = size;
        reg.mode = UFFDIO_REGISTER_MODE_WP;
        const int ret = ioctl(uffd, UFFDIO_REGISTER, &reg);
        ASSERT_MSG(ret != -1, "Uffdio register failed with error: {}", Common::GetLastErrorMsg());
    }

    void OnUnmap(VAddr address, size_t size) override {
        uffdio_range range;
        range.start = address;
        range.len = size;
        const int ret = ioctl(uffd, UFFDIO_UNREGISTER, &range);
        ASSERT_MSG(ret != -1, "Uffdio unregister failed with error: {}", Common::GetLastErrorMsg());
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        bool allow_write = True(perms & Core::MemoryPermission::Write);
        uffdio_writeprotect wp;
        wp.range.start = address;
        wp.range.len = size;
        wp.mode = allow_write ? UFFDIO_WRITEPROTECT_MODE_DONTWAKE : UFFDIO_WRITEPROTECT_MODE_WP;
        const int ret = ioctl(uffd, UFFDIO_WRITEPROTECT, &wp);
        ASSERT_MSG(ret != -1, "Uffdio writeprotect failed with error: {}",
                   Common::GetLastErrorMsg());
    }

    void UffdHandler(std::stop_token token) {
        Common::SetCurrentThreadName("shadPS4:Uffd");

        auto regions = Core::Memory::Instance()->GetAddressSpace().GetUsableRegions();
        for (auto& region : regions) {
            OnMap(region.lower(), region.upper());
        }
        LOG_INFO(Common_Memory, "registered reserved memory with userfaultfd");

        while (!token.stop_requested()) {
            pollfd pollfd;
            pollfd.fd = uffd;
            pollfd.events = POLLIN;

            // Block until the descriptor is ready for data reads.
            const int pollres = poll(&pollfd, 1, -1);
            switch (pollres) {
            case -1:
                perror("Poll userfaultfd");
                continue;
                break;
            case 0:
                continue;
            case 1:
                break;
            default:
                UNREACHABLE_MSG("Unexpected number of descriptors {} out of poll", pollres);
            }

            // We don't want an error condition to have occured.
            ASSERT_MSG(!(pollfd.revents & POLLERR), "POLLERR on userfaultfd");

            // We waited until there is data to read, we don't care about anything else.
            if (!(pollfd.revents & POLLIN)) {
                continue;
            }

            // Read message from kernel.
            uffd_msg msg;
            const int readret = read(uffd, &msg, sizeof(msg));
            if (readret == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                LOG_ERROR(Common_Memory, "Unexpected result of uffd read: {}",
                          Common::GetLastErrorMsg());
                break;
            }
            ASSERT_MSG(readret == sizeof(msg), "Unexpected short read, exiting");
            ASSERT(msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP);

            // Notify rasterizer about the fault.
            const VAddr addr = msg.arg.pagefault.address;
            const auto ptid = msg.arg.pagefault.feat.ptid;
            rasterizer->InvalidateMemory(addr, 1, rasterizer->IsGpuSideThreadId(ptid));

            // Some calls to InvalidateMemory never reach the UFFDIO_WRITEPROTECT ioctl in
            // ::Protect, therefore we use MODE_DONTWAKE and wake the thread with UFFDIO_WAKE here
            uffdio_range wake;
            wake.start = msg.arg.pagefault.address;
            wake.len = PageManager::PM_PAGE_SIZE;
            const int ret = ioctl(uffd, UFFDIO_WAKE, &wake);
            ASSERT_MSG(ret != -1, "Waking thread {} failed with: {}", ptid,
                       Common::GetLastErrorMsg());
        }
    }
};
#endif // __linux__

struct SignalImpl : public PageManager::Impl {
    SignalImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;

        // Splitting a mapped placeholder on Windows recreates the neighbouring views with their
        // default protection, silently dropping the write/read tracking of those pages.
        // sotc port: the C runtime re-applies protections itself when it splits views.
        g_reapply_protection = [this](VAddr address, u64 size) {
            ReapplyProtection(address, size);
        };

        // Should be called first.
        constexpr auto priority = std::numeric_limits<u32>::min();
        Core::Signals::Instance()->RegisterAccessViolationHandler(GuestFaultSignalHandler,
                                                                  priority);
        Shader::SetSrtCleanLoad(&SrtWalkerCleanLoad);
        // sotc: SOTC_FAULT_OSCOST=1 with SOTC_FPS: see MeasureFaultOsCost.
        if (const char* oscost = std::getenv("SOTC_FAULT_OSCOST");
            SotcStats::kFpsLog && oscost && oscost[0] == '1') {
            Core::Signals::Instance()->RegisterAccessViolationHandler(OsCostFaultHandler, 2);
            g_oscost_registered.store(true, std::memory_order_release);
        }
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        RENDERER_TRACE;
        SotcTrace::NoteProtect(); // sotc: SOTC_TRACE (BindScope)
        // sotc: SOTC_FPS: time spent in protect calls made while a guest fault is handled
        const SotcStats::FaultStat::Scope protect_timed{&SotcStats::FaultStat::Tl::protect_ns};
        auto* memory = Core::Memory::Instance();
        auto& impl = memory->GetAddressSpace();
        // sotc: write-only means pages both CPU- and GPU-modified (the asserting opt4 crash, ~1 in
        // 12 Shrine runs). Protect them fully instead: an access faults, the fault reads the GPU
        // data back, and the state settles. The first occurrences log who asked.
        if (perms == Core::MemoryPermission::Write) [[unlikely]] {
            static std::atomic<u32> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
                void* frames[12]{};
                u32 count = 0;
#ifdef _WIN64
                count = CaptureStackBackTrace(1, 12, frames, nullptr);
#endif
                std::string where;
                for (u32 i = 0; i < count; ++i) {
                    where += fmt::format(" {:#x}", reinterpret_cast<u64>(frames[i]) -
                                                       ImageBaseAddress());
                }
                LOG_ERROR(Render, "Write-only protection of {:#x}+{:#x} (CPU- and GPU-modified "
                          "pages), protected fully; stack (image offsets):{}", address, size,
                          where);
            }
            perms = Core::MemoryPermission::None;
        }
        // sotc: the walker's clean-load bitmap follows the read protection: set before
        // protecting, cleared after unprotecting (a stale bit only costs a clean load).
        const bool read_protected = False(perms & Core::MemoryPermission::Read);
        if (read_protected) {
            Shader::SetSrtReadProtectedPages(address, size, true);
        }
        const auto clear_after = [&] {
            if (!read_protected) {
                Shader::SetSrtReadProtectedPages(address, size, false);
            }
        };
        if (perms != Core::MemoryPermission::ReadWrite) {
            // Keep guest stacks accessible, only protect the rest of the range. sotc: the
            // registry stays locked (shared) across the host protect, see StackLock.
            const auto window = boost::icl::interval<VAddr>::right_open(address, address + size);
            std::shared_lock lk{g_guest_stacks_mutex};
            const auto [first, last] = g_guest_stacks.equal_range(window);
            if (first == last) {
                // Common case, no stack in the range (and no interval set to allocate).
                if (StackGuardMode() == 0) {
                    lk.unlock();
                }
                impl.Protect(address, size, perms);
            } else {
                boost::icl::interval_set<VAddr> ranges;
                ranges += window;
                for (auto it = first; it != last; ++it) {
                    ranges -= it->first;
                }
                static std::atomic<u32> reported{0};
                if (reported++ < 8) {
                    LOG_ERROR(Render, "Not protecting guest stack pages in {:#x}+{:#x}", address,
                              size);
                }
                if (StackGuardMode() == 0) {
                    lk.unlock();
                }
                for (const auto& range : ranges) {
                    impl.Protect(range.lower(), range.upper() - range.lower(), perms);
                }
            }
            if (lk.owns_lock()) {
                lk.unlock();
            }
            clear_after();
            return;
        }
        impl.Protect(address, size, perms);
        clear_after();
    }

    // sotc: SOTC_RB_STATS=1 prints, every 10 s, the read faults that cost a readback, grouped by
    // reader (guest or GPU thread), faulting instruction and 64 KiB region.
    // SOTC_RB_STATS=w: the write faults instead.
    static void NoteReadFault(void* context, VAddr addr, bool is_gpu_thread, bool write = false) {
        static const char* const mode = std::getenv("SOTC_RB_STATS");
        if (!mode || (mode[0] == 'w') != write) {
            return;
        }
        static std::mutex stats_mutex;
        static std::map<std::tuple<bool, u64, VAddr>, u64> counts;
        static auto last = std::chrono::steady_clock::now();
#ifdef _WIN32
        const u64 rip = static_cast<EXCEPTION_POINTERS*>(context)->ContextRecord->Rip;
#else
        const u64 rip = 0;
#endif
        std::scoped_lock lk{stats_mutex};
        ++counts[{is_gpu_thread, rip, addr & ~VAddr{0xFFFF}}];
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::seconds(10)) {
            return;
        }
        last = now;
        std::vector<std::pair<u64, std::tuple<bool, u64, VAddr>>> sorted;
        u64 total = 0;
        for (const auto& [key, count] : counts) {
            sorted.emplace_back(count, key);
            total += count;
        }
        std::sort(sorted.rbegin(), sorted.rend());
        std::fprintf(stderr, "SOTC readback faults in 10 s: %llu\n", static_cast<unsigned long long>(total));
        for (size_t i = 0; i < sorted.size() && i < 12; ++i) {
            const auto& [count, key] = sorted[i];
            char where[MAX_PATH] = "?";
#ifdef _WIN32
            HMODULE module = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(std::get<1>(key)), &module)) {
                char path[MAX_PATH];
                GetModuleFileNameA(module, path, MAX_PATH);
                const char* name = std::strrchr(path, '\\');
                std::snprintf(where, sizeof(where), "%s+%#llx", name ? name + 1 : path,
                              static_cast<unsigned long long>(std::get<1>(key) -
                                                              reinterpret_cast<u64>(module)));
            } else {
                MEMORY_BASIC_INFORMATION info{};
                VirtualQuery(reinterpret_cast<void*>(std::get<1>(key)), &info, sizeof(info));
                std::snprintf(where, sizeof(where), "alloc %p+%#llx", info.AllocationBase,
                              static_cast<unsigned long long>(
                                  std::get<1>(key) - reinterpret_cast<u64>(info.AllocationBase)));
            }
#endif
            std::fprintf(stderr, "  %6llu %s rip=%#llx (%s) region=%#llx\n",
                         static_cast<unsigned long long>(count), std::get<0>(key) ? "gpu  " : "guest",
                         static_cast<unsigned long long>(std::get<1>(key)), where,
                         static_cast<unsigned long long>(std::get<2>(key)));
        }
        counts.clear();
    }

    // sotc: after a write fault is handled the trackers consider the page writable. The same
    // thread faulting on the same instruction and page again right away (no other fault in
    // between) means the OS protection disagrees with them: something protected the page without
    // the trackers, and the handler would "resolve" the fault forever without changing anything
    // (seen on cold starts: ~18 rounds, then the process died silently). Make the page writable,
    // which is what the trackers already believe, and log it.
    static void RepairStuckWriteFault(void* context, VAddr addr) {
        // SOTC_DESYNC_REPAIR: 0 off, 1 repair, unset/2 count only. With the repeat-fault grace
        // the storms this was meant for are gone, and its buffer-bit guard cannot see texture
        // watchers or a racing GPU sync (it could unprotect a page they just protected), so by
        // default it only counts and logs.
        static const int mode = [] {
            const char* v = std::getenv("SOTC_DESYNC_REPAIR");
            return v ? std::atoi(v) : 2;
        }();
        if (mode == 0) {
            return;
        }
        static thread_local u64 last_rip = 0;
        static thread_local VAddr last_page = 0;
        static thread_local u32 repeats = 0;
        const u64 rip = reinterpret_cast<u64>(Common::GetRip(context));
        const VAddr page = Common::AlignDown(addr, VAddr{4096});
        if (rip != last_rip || page != last_page) {
            last_rip = rip;
            last_page = page;
            repeats = 0;
            return;
        }
        if (++repeats < 2) {
            return;
        }
        repeats = 0;
        // Only a real desync is repaired: the buffer tracker wants the page writable (CPU-modified)
        // and the host still refuses writes. A tracker that wants it protected means the GPU
        // thread re-protected it after uploading (legitimate ping-pong): leave it alone, or CPU
        // writes would be missed and the GPU would keep stale data.
        if (!rasterizer->GetBufferCache().IsRegionCpuModified(page, 4096)) {
            SotcStats::desync_legit.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<u32> logged_legit{0};
            if (logged_legit.fetch_add(1, std::memory_order_relaxed) < 24) {
                char state[160];
                runtime_memory_describe(page, state, sizeof(state));
                LOG_WARNING(Render, "Repeated write fault at {:#x} (rip {:#x}), tracker wants it "
                            "protected; {}", addr, rip, state);
            }
            return;
        }
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(page), &mbi, sizeof(mbi)) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_WRITECOPY)) != 0) {
            return;
        }
#endif
        SotcStats::desync_repairs.fetch_add(1, std::memory_order_relaxed);
        if (mode != 1) {
            return;
        }
        Core::Memory::Instance()->GetAddressSpace().Protect(page, 4096,
                                                            Core::MemoryPermission::ReadWrite);
        static std::atomic<u32> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
            char state[160];
            runtime_memory_describe(page, state, sizeof(state));
            LOG_WARNING(Render, "Page tracking desync: write fault at {:#x} (rip {:#x}) kept "
                        "recurring, page made writable; now {}", addr, rip, state);
        }
    }

    static bool GuestFaultSignalHandler(void* context, void* fault_address) {
        // sotc: a fault of the command processor while the draw thread binds the draws
        // (Liverpool::IsBackThreaded) is handled on the draw thread, after what is queued: that
        // work may produce the bytes this read wants, and a readback records commands, which only
        // the draw thread may do.
        // sotc: a walker load through a null SRT pointer (address below 64 KiB, never mapped) is
        // left to SrtWalkerSignalHandler (reads zero) without waiting for the draw thread: a
        // pointer that stays null faults on every draw since the walker is no longer patched.
        if (reinterpret_cast<VAddr>(fault_address) < 0x10000 && !Common::IsWriteError(context) &&
            Shader::IsSrtWalkerCode(Common::GetRip(context))) {
            return false;
        }
        if (rasterizer->RunsGpuWorkElsewhere()) {
            bool handled = false;
            rasterizer->RunOnDrawThread(
                [&] { handled = HandleGuestFault(context, fault_address, true); });
            return handled;
        }
        return HandleGuestFault(context, fault_address, rasterizer->IsGpuSideThread());
    }

    // sotc: may a page other than the faulting one be invalidated by SOTC_FAULT_GRANULE? Not when
    // it is part of a guest stack, and not when the GPU wrote it (its read protection stands for a
    // readback; the GPU bit is checked here, and InvalidateRegion would read it back anyway).
    static bool GranulePageMayInvalidate(VAddr page) {
        u64 open = 0;
        ForEachNonStackRange(page, PageManager::PM_PAGE_SIZE,
                             [&open](VAddr, u64 size) { open += size; });
        // sotc: nor when an image lives in the page's 1 MiB block: invalidating it marks the image
        // CPU-dirty, and an image the GPU drew was then uploaded again from stale guest memory
        // (colored blocks on the title screen, 2026-10-27). Texture pages keep one fault per page.
        return open == PageManager::PM_PAGE_SIZE &&
               !rasterizer->GetBufferCache().IsRegionGpuModified(page, PageManager::PM_PAGE_SIZE) &&
               // sotc: nor when it is already CPU-dirty: it uploads anyway, and a snapshot taken
               // now would equal its bytes and drop an upload the arena still needs.
               !rasterizer->GetBufferCache().IsRegionCpuModified(page, PageManager::PM_PAGE_SIZE) &&
               !rasterizer->GetTextureCache().HasImagesNear(page);
    }

    // sotc: SOTC_WRITE_AHEAD_ADAPT: may the write-ahead window mark this page? The stop rules of
    // the granule: not a guest stack page, not in a 1 MiB block with an image (the GPU-modified
    // and already CPU-dirty pages are skipped by BufferCache::MarkWriteAheadRange itself).
    static bool AheadPageMayInvalidate(VAddr page) {
        return !IntersectsGuestStack(page, PageManager::PM_PAGE_SIZE) &&
               !rasterizer->GetTextureCache().HasImagesNear(page);
    }

    // sotc: SOTC_FAULT_GRANULE. After a guest write fault, the other pages of the faulting page's
    // aligned granule are invalidated as well (CPU-modified and unprotected, through the same
    // InvalidateMemoryOnGuestFault path), so writes that follow in that granule do not fault again.
    // Trade-off: an invalidated texture or buffer is uploaded again from guest memory, and most of
    // these pages are never written, so the uploads are wasted work; the gain is one exception per
    // granule instead of one per 4 KiB page (16k per 64 MiB streamed). Pages the GPU wrote and
    // guest stacks are skipped (they stay as they were); the faulting page is the caller's.
    static void InvalidateGranule(VAddr addr) {
        const VAddr begin = Common::AlignDown(addr, SotcStats::kFaultGranule);
        const VAddr end = begin + SotcStats::kFaultGranule;
        const VAddr fault_page = PageManager::GetPageAddr(addr);
        VAddr run_start = 0;
        u64 run_size = 0;
        // Invalidates the pages collected so far as one range (one call per run of pages).
        const auto flush = [&] {
            if (run_size == 0) {
                return;
            }
            if (SotcStats::kCmdStats) { // sotc: read only by the SOTC_CMD_STATS printer
                SotcStats::fault_granule_pages.fetch_add(run_size / PageManager::PM_PAGE_SIZE,
                                                         std::memory_order_relaxed);
            }
            // sotc: remember each page's bytes first: the buffer cache then uploads only the pages
            // the guest really writes (BufferCache::DropUnchangedGranulePages).
            {
                const SotcStats::FaultStat::StageScope hash_stage{
                    SotcStats::FaultStat::kStGranHash};
                for (VAddr page = run_start; page < run_start + run_size;
                     page += PageManager::PM_PAGE_SIZE) {
                    rasterizer->GetBufferCache().NoteGranulePage(page);
                }
            }
            rasterizer->InvalidateMemoryOnGuestFault(run_start, run_size);
            run_size = 0;
        };
        for (VAddr page = begin; page < end; page += PageManager::PM_PAGE_SIZE) {
            if (page == fault_page || !GranulePageMayInvalidate(page)) {
                flush();
                continue;
            }
            if (run_size == 0) {
                run_start = page;
            }
            run_size += PageManager::PM_PAGE_SIZE;
        }
        flush();
    }

    // sotc: SOTC_PROTECT_COALESCE: may this fault collect its unprotects (FaultProtectScope)?
    // Not when the page holds GPU data: the fault then waits for a readback (the GPU thread), and
    // other threads writing a page that is collected but not yet applied would wait for this
    // thread meanwhile. Checked from unlocked bits; a page that turns GPU-modified right after
    // only makes such a wait possible, never wrong (every faulting thread applies its own page,
    // see FlushFaultProtects).
    static bool FaultMayCoalesce(VAddr page) {
        return !rasterizer->GetBufferCache().IsRegionGpuModified(page, PageManager::PM_PAGE_SIZE);
    }

    static bool HandleGuestFault(void* context, void* fault_address, bool is_gpu_thread) {
        // sotc: SOTC_PROTECT_BATCH. The updates a fault makes protect at once, also on a thread
        // with a batch open (the draw thread also handles other threads' faults,
        // RunOnDrawThread). None of the collected re-protects can be what faulted: they only
        // remove access the host still grants until the flush.
        struct BatchSuspend {
            BatchSuspend() { ++tl_protect_batch.suspended; }
            ~BatchSuspend() { --tl_protect_batch.suspended; }
        } const batch_suspend;
        const auto addr = reinterpret_cast<VAddr>(fault_address);
        const auto size = std::min<u64>(8, PageManager::GetNextPageAddr(addr) - addr);
        if (!Common::IsWriteError(context) && is_gpu_thread && TryCleanWalkerLoad(context)) {
            if (SotcStats::kCmdStats) { // sotc: read only by the SOTC_CMD_STATS printer
                SotcStats::fault_walker.fetch_add(1, std::memory_order_relaxed);
            }
            return true;
        }
        if (Common::IsWriteError(context)) {
            // sotc: SOTC_FPS: the time from the entry of bbgpu_handle_fault to here is dispatch.
            SotcStats::FaultStat::NoteHandlerEntry();
            // sotc: read only by the SOTC_CMD_STATS printer; every guest thread faults here, and
            // the counter shared a cache line with command-processor counters.
            if (SotcStats::kCmdStats) {
                SotcStats::fault_writes.fetch_add(1, std::memory_order_relaxed);
            }
            NoteReadFault(context, addr, is_gpu_thread, true);
            const VAddr fault_page = PageManager::GetPageAddr(addr);
            // sotc: SOTC_PROTECT_COALESCE, guest threads only: the unprotects of this fault's
            // tracker updates (page, granule, write-ahead, images) are applied as one call per
            // run by protect_scope.Flush below (page_manager.h, FaultProtectScope).
            PageManager::FaultProtectScope protect_scope{SotcStats::kProtectCoalesce &&
                                                         !is_gpu_thread &&
                                                         FaultMayCoalesce(fault_page)};
            bool handled;
            {
                const SotcStats::FaultStat::StageScope stage{SotcStats::FaultStat::kStInval};
                handled = is_gpu_thread ? rasterizer->InvalidateMemory(addr, size, true)
                                        : rasterizer->InvalidateMemoryOnGuestFault(addr, size);
            }
            if (!handled) {
                return false;
            }
            // sotc: SOTC_FPS fault counters and SOTC_WRITE_AHEAD_ADAPT: classify this guest
            // thread's fault against its previous one (SotcStats::FaultStat).
            const bool track_fault =
                !is_gpu_thread && (SotcStats::kFpsLog || SotcStats::kWriteAheadAdapt);
            SotcStats::FaultStat::Plan plan;
            if (track_fault) {
                plan = SotcStats::FaultStat::Observe(fault_page);
            }
            // The extent this fault unprotects, for the next one's classification.
            VAddr extent_lo = fault_page;
            VAddr extent_hi = fault_page + PageManager::PM_PAGE_SIZE;
            // sotc: SOTC_FAULT_GRANULE, guest threads only (the command processor's writes keep
            // the single page).
            if (!is_gpu_thread && SotcStats::kFaultGranule > PageManager::PM_PAGE_SIZE) {
                {
                    const SotcStats::FaultStat::StageScope stage{SotcStats::FaultStat::kStGranule};
                    InvalidateGranule(addr);
                }
                extent_lo = Common::AlignDown(addr, SotcStats::kFaultGranule);
                extent_hi = extent_lo + SotcStats::kFaultGranule;
            }
            {
                const SotcStats::FaultStat::StageScope stage{SotcStats::FaultStat::kStAhead};
                if (SotcStats::kWriteAheadAdapt && !is_gpu_thread && plan.sequential) {
                    // sotc: SOTC_WRITE_AHEAD_ADAPT: the window of a sequential writer, in its
                    // direction of travel, snapshot-hashed (BufferCache::MarkWriteAheadRange).
                    const u64 window = u64{plan.window} * PageManager::PM_PAGE_SIZE;
                    VAddr begin;
                    VAddr end;
                    if (plan.dir < 0) {
                        begin = fault_page > window ? fault_page - window : 0;
                        end = fault_page;
                        extent_lo = std::min(extent_lo, begin);
                    } else {
                        begin = fault_page + PageManager::PM_PAGE_SIZE;
                        end = begin + window;
                        extent_hi = std::max(extent_hi, end);
                    }
                    rasterizer->GetBufferCache().MarkWriteAheadRange(begin, end,
                                                                     &AheadPageMayInvalidate);
                } else {
                    rasterizer->MarkWriteAhead(addr);
                    extent_hi = std::max(extent_hi,
                                         fault_page + (1 + BufferCache::WriteAheadPages()) *
                                                          PageManager::PM_PAGE_SIZE);
                }
            }
            if (track_fault) {
                SotcStats::FaultStat::Done(extent_lo, extent_hi);
            }
            {
                // sotc: SOTC_PROTECT_COALESCE: the page must be writable before the guest
                // retries, and before RepairStuckWriteFault looks at it.
                const SotcStats::FaultStat::StageScope stage{SotcStats::FaultStat::kStFlush};
                protect_scope.Flush(fault_page);
                // A thread without an active scope (the command processor, a page the scope did
                // not take) may have its page covered by another thread's pending flush: apply
                // the tracker's state for it here, so it never re-faults until that flush runs.
                if (SotcStats::kProtectCoalesce && !protect_scope.Active()) {
                    PageManager::EnsureFaultPageWritable(fault_page);
                }
            }
            {
                const SotcStats::FaultStat::StageScope stage{SotcStats::FaultStat::kStRepair};
                RepairStuckWriteFault(context, addr);
            }
            return true;
        } else {
            if (is_gpu_thread && SotcStats::kCmdStats) { // sotc: SOTC_CMD_STATS printer only
                SotcStats::fault_gpu_reads.fetch_add(1, std::memory_order_relaxed);
            }
            NoteReadFault(context, addr, is_gpu_thread);
            return rasterizer->ReadMemory(addr, size, is_gpu_thread);
        }
        return false;
    }

#ifdef _WIN32
    static DWORD64* ContextRegister(CONTEXT* ctx, ZydisRegister reg) {
        switch (ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg)) {
        case ZYDIS_REGISTER_RAX: return &ctx->Rax;
        case ZYDIS_REGISTER_RCX: return &ctx->Rcx;
        case ZYDIS_REGISTER_RDX: return &ctx->Rdx;
        case ZYDIS_REGISTER_RBX: return &ctx->Rbx;
        case ZYDIS_REGISTER_RSI: return &ctx->Rsi;
        case ZYDIS_REGISTER_RDI: return &ctx->Rdi;
        case ZYDIS_REGISTER_R8: return &ctx->R8;
        case ZYDIS_REGISTER_R9: return &ctx->R9;
        case ZYDIS_REGISTER_R10: return &ctx->R10;
        case ZYDIS_REGISTER_R11: return &ctx->R11;
        case ZYDIS_REGISTER_R12: return &ctx->R12;
        case ZYDIS_REGISTER_R13: return &ctx->R13;
        case ZYDIS_REGISTER_R14: return &ctx->R14;
        case ZYDIS_REGISTER_R15: return &ctx->R15;
        default: return nullptr;
        }
    }
#endif

    /// Clean-load callback of the generated walkers (GPU command processor thread).
    static bool SrtWalkerCleanLoad(u64 address, u32 size, u64* out) {
        // No GPU mapping check: without one the page is not protected and the backing holds
        // what a plain load would read.
        if (!rasterizer->IsGpuSideThread()) {
            return false;
        }
        *out = 0;
        if (!rasterizer->ReadCleanMemory(address, out, size)) {
            return false;
        }
        return true;
    }

    /// SRT walkers read user data tables that often share a page with bytes the GPU wrote. The
    /// page is read-protected for those bytes, but the backing already holds every byte the GPU
    /// did not write: complete such a load from the backing instead of a full GPU readback. The
    /// page stays protected, so reads of the GPU-written bytes still fault and read back.
    /// Idea from srt_walker_clean_reads of Pink-shadPS4 (luizgustavs). SOTC_CLEAN_READS=0 = off.
    static bool TryCleanWalkerLoad(void* context) {
#ifdef _WIN32
        static const bool enabled = [] {
            const char* v = std::getenv("SOTC_CLEAN_READS");
            return v ? std::atoi(v) != 0 : true;
        }();
        const void* rip = Common::GetRip(context);
        if (!enabled || !Shader::IsSrtWalkerCode(rip)) {
            return false;
        }
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        const auto status = Common::Decoder::Instance()->decodeInstruction(
            instruction, operands, const_cast<void*>(rip), 15);
        if (!ZYAN_SUCCESS(status) || instruction.mnemonic != ZYDIS_MNEMONIC_MOV ||
            operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
            operands[1].type != ZYDIS_OPERAND_TYPE_MEMORY) {
            return false;
        }
        auto* ctx = static_cast<EXCEPTION_POINTERS*>(context)->ContextRecord;
        const auto& mem = operands[1].mem;
        const u32 size = operands[0].size / 8;
        DWORD64* dst = ContextRegister(ctx, operands[0].reg.value);
        if (!dst || (size != 4 && size != 8) || mem.segment != ZYDIS_REGISTER_DS ||
            mem.base == ZYDIS_REGISTER_NONE) {
            return false;
        }
        const DWORD64* base = ContextRegister(ctx, mem.base);
        if (!base) {
            return false;
        }
        VAddr address = *base + static_cast<u64>(mem.disp.value);
        if (mem.index != ZYDIS_REGISTER_NONE) {
            const DWORD64* index = ContextRegister(ctx, mem.index);
            if (!index) {
                return false;
            }
            address += *index * mem.scale;
        }
        if (!rasterizer->IsMapped(address, size) || rasterizer->IsRangeGpuWritten(address, size)) {
            return false;
        }
        u64 value = 0;
        if (!Core::Memory::Instance()->TryCopySparseMemory(address, reinterpret_cast<u8*>(&value),
                                                           size)) {
            return false;
        }
        // A 32-bit destination zero-extends into the full register, as the mov would.
        *dst = value;
        ctx->Rip += instruction.length;
        // Later walker loads in this page try the clean path before faulting.
        Shader::MarkSrtCleanPages(address, size);
        return true;
#else
        return false;
#endif
    }
};

PageManager::PageManager(Vulkan::Rasterizer* rasterizer_) {
#ifdef __linux__
    if (EmulatorSettings.IsUserfaultfdTracking()) {
        try {
            impl = std::make_unique<UffdImpl>(rasterizer_);
            LOG_INFO(Config, "Memory tracking method: userfaultfd");
            return;
        } catch (const std::runtime_error& e) {
            // if uffd is unsupported, falls back to SignalImpl
        }
    }
    LOG_INFO(Config, "Memory tracking method: signals");
#endif
    impl = std::make_unique<SignalImpl>(rasterizer_);
}

PageManager::~PageManager() = default;

void PageManager::OnGpuMap(VAddr address, size_t size) {
    impl->OnMap(address, size);
}

void PageManager::OnGpuUnmap(VAddr address, size_t size) {
    impl->OnUnmap(address, size);
}

void PageManager::UpdatePageWatchers(VAddr addr, u64 size, PageOp write_op) const {
    impl->UpdatePageWatchers(addr, size, write_op);
}

void PageManager::UpdatePageWatchersForRegion(VAddr base_addr, const Bounds& bounds,
                                              const RegionBits& write_mask,
                                              const RegionBits& read_mask, PageOp write_op,
                                              PageOp read_op, bool deferrable) const {
    impl->UpdatePageWatchersForRegion(base_addr, bounds, write_mask, read_mask, write_op, read_op,
                                      deferrable);
}

// sotc: SOTC_PROTECT_BATCH (page_manager.h). The batch is per thread; the tracker has a single
// instance, whose constructor sets g_protect_batch_flush.
PageManager::ProtectBatch::ProtectBatch(bool enable) noexcept : active{enable} {
    if (active) {
        ++tl_protect_batch.depth;
    }
}

// Every scope flushes when it closes, an inner one too: applying a change early is always safe,
// late is not (the inner scope's caller reads guest memory next).
PageManager::ProtectBatch::~ProtectBatch() {
    if (active) {
        Flush();
        --tl_protect_batch.depth;
    }
}

void PageManager::ProtectBatch::Flush() {
    if (active && !tl_protect_batch.runs.empty() && g_protect_batch_flush) {
        g_protect_batch_flush();
    }
}

bool PageManager::ProtectBatchOpen() noexcept {
    return tl_protect_batch.depth > 0 && tl_protect_batch.suspended == 0;
}

void PageManager::EnsureFaultPageWritable(VAddr page) {
    if (g_fault_page_fix) {
        g_fault_page_fix(page);
    }
}

// sotc: SOTC_PROTECT_COALESCE (page_manager.h). Per thread; the tracker has a single instance,
// whose constructor sets g_fault_protect_flush.
PageManager::FaultProtectScope::FaultProtectScope(bool enable) noexcept
    : active{enable && g_fault_protect_flush != nullptr} {
    if (active) {
        ++tl_protect_batch.fault_depth;
    }
}

PageManager::FaultProtectScope::~FaultProtectScope() {
    if (active) {
        Flush(0);
        --tl_protect_batch.fault_depth;
    }
}

void PageManager::FaultProtectScope::Flush(VAddr fault_page) {
    if (active && (fault_page != 0 || !tl_protect_batch.fault_runs.empty())) {
        g_fault_protect_flush(fault_page);
    }
}

bool MeasureFaultOsCost(double& in_us, double& out_us) {
    static std::once_flag once;
    static double in = 0.0;
    static double out = 0.0;
    static bool ok = false;
    std::call_once(once, [] {
#ifdef _WIN64
        if (!g_oscost_registered.load(std::memory_order_acquire)) {
            return;
        }
        void* page = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READONLY);
        if (!page) {
            return;
        }
        g_oscost_page.store(reinterpret_cast<uintptr_t>(page), std::memory_order_release);
        std::vector<u64> ins;
        std::vector<u64> outs;
        for (int i = 0; i < 41; ++i) {
            DWORD old{};
            VirtualProtect(page, 4096, PAGE_READONLY, &old);
            g_oscost_enter_ns.store(0, std::memory_order_relaxed);
            const u64 before = SotcStats::NowNs();
            *static_cast<volatile char*>(page) = 1; // faults: the handler above makes it writable
            const u64 after = SotcStats::NowNs();
            const u64 enter = g_oscost_enter_ns.load(std::memory_order_relaxed);
            const u64 exit = g_oscost_exit_ns.load(std::memory_order_relaxed);
            if (i == 0 || enter < before || after < exit) {
                continue; // the first fault pages in the exception path
            }
            ins.push_back(enter - before);
            outs.push_back(after - exit);
        }
        g_oscost_page.store(0, std::memory_order_release);
        VirtualFree(page, 0, MEM_RELEASE);
        if (ins.size() < 8) {
            return;
        }
        std::ranges::nth_element(ins, ins.begin() + ins.size() / 2);
        std::ranges::nth_element(outs, outs.begin() + outs.size() / 2);
        in = static_cast<double>(ins[ins.size() / 2]) / 1000.0;
        out = static_cast<double>(outs[outs.size() / 2]) / 1000.0;
        SotcStats::FaultStat::os_fault_in_ns.store(ins[ins.size() / 2], std::memory_order_relaxed);
        SotcStats::FaultStat::os_fault_out_ns.store(outs[outs.size() / 2], std::memory_order_relaxed);
        ok = true;
#endif
    });
    in_us = in;
    out_us = out;
    return ok;
}

} // namespace VideoCore
