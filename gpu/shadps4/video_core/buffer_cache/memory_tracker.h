// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <deque>
#include <vector>

#include "common/types.h"
#include "video_core/buffer_cache/region_manager.h"
#include "video_core/sotc_stats.h"
#include "video_core/sotc_trace.h" // sotc: SOTC_TRACE (redirty instants)

namespace VideoCore {

class MemoryTracker {
public:
    static constexpr u64 MAX_CPU_PAGE_BITS = 40;
    static constexpr u64 NUM_HIGH_PAGES = 1ULL << (MAX_CPU_PAGE_BITS - HIGHER_PAGE_BITS);
    static constexpr u64 MANAGER_POOL_SIZE = 32;

public:
    explicit MemoryTracker(PageManager& tracker_)
        : tracker{&tracker_}, readbacks_mode{EmulatorSettings.GetReadbacksMode()} {}
    ~MemoryTracker() = default;

    /// Returns true if a region has been modified from the GPU
    bool IsRegionGpuModified(VAddr cpu_addr, u64 size) noexcept {
        return IteratePages(cpu_addr, size, [](RegionManager* manager, u64 offset, u64 size) {
            return manager->template IsRegionModified<Type::GPU>(offset, size);
        });
    }

    /// Returns true if a region has been modified from the CPU
    bool IsRegionCpuModified(VAddr cpu_addr, u64 size) noexcept {
        return IteratePages(cpu_addr, size, [](RegionManager* manager, u64 offset, u64 size) {
            return manager->template IsRegionModified<Type::CPU>(offset, size);
        });
    }

    /// sotc: SOTC_WRITE_DECL pre-check, lock-free (no region mutex). False only when no page of
    /// [cpu_addr, cpu_addr + size) can be write-protected by the buffer tracking: a region that
    /// does not exist protects nothing here. A page only the texture cache protects is not seen
    /// (it faults as before). Cheap for small spans: one region and a few words.
    bool MaybeWriteProtected(VAddr cpu_addr, u64 size) noexcept {
        const u64 limit = u64{1} << MAX_CPU_PAGE_BITS;
        if (cpu_addr >= limit) {
            return false;
        }
        size = std::min(size, limit - cpu_addr);
        u64 page_index = cpu_addr >> HIGHER_PAGE_BITS;
        u64 offset = cpu_addr & HIGHER_PAGE_MASK;
        for (u64 remaining = size; remaining > 0; ++page_index, offset = 0) {
            const u64 len = std::min(HIGHER_PAGE_SIZE - offset, remaining);
            remaining -= len;
            RegionManager* region = __atomic_load_n(&top_tier[page_index], __ATOMIC_ACQUIRE);
            if (region && region->MaybeWriteProtected(offset, len)) {
                return true;
            }
        }
        return false;
    }

    /// sotc: SOTC_WRITE_DECL statistics (SOTC_WRITE_DECL=2): the pages MaybeWriteProtected finds
    /// in the range, lock-free. Same bits, all pages counted.
    u64 CountWriteProtectedPages(VAddr cpu_addr, u64 size) noexcept {
        const u64 limit = u64{1} << MAX_CPU_PAGE_BITS;
        if (cpu_addr >= limit) {
            return 0;
        }
        size = std::min(size, limit - cpu_addr);
        u64 pages = 0;
        u64 page_index = cpu_addr >> HIGHER_PAGE_BITS;
        u64 offset = cpu_addr & HIGHER_PAGE_MASK;
        for (u64 remaining = size; remaining > 0; ++page_index, offset = 0) {
            const u64 len = std::min(HIGHER_PAGE_SIZE - offset, remaining);
            remaining -= len;
            RegionManager* region = __atomic_load_n(&top_tier[page_index], __ATOMIC_ACQUIRE);
            if (region) {
                pages += region->CountWriteProtectedPages(offset, len);
            }
        }
        return pages;
    }

    /// sotc: SOTC_WRITE_DECL. Calls func(cpu_addr, size) for each run of [cpu_addr, cpu_addr +
    /// size) that a declare settles (RegionManager::ForEachDeclarableRun). Unlocked look: the
    /// caller invalidates each run through the locked path, which checks the bits again.
    template <typename Func>
    void ForEachDeclarableRun(VAddr cpu_addr, u64 size, Func&& func) noexcept {
        const u64 limit = u64{1} << MAX_CPU_PAGE_BITS;
        if (cpu_addr >= limit) {
            return;
        }
        size = std::min(size, limit - cpu_addr);
        u64 page_index = cpu_addr >> HIGHER_PAGE_BITS;
        u64 offset = cpu_addr & HIGHER_PAGE_MASK;
        for (u64 remaining = size; remaining > 0; ++page_index, offset = 0) {
            const u64 len = std::min(HIGHER_PAGE_SIZE - offset, remaining);
            remaining -= len;
            RegionManager* region = __atomic_load_n(&top_tier[page_index], __ATOMIC_ACQUIRE);
            if (!region) {
                continue;
            }
            const VAddr base = page_index << HIGHER_PAGE_BITS;
            region->ForEachDeclarableRun(offset, len, [&](u64 run, u64 run_size) {
                func(base + run, run_size);
            });
        }
    }

    /// sotc: BB_READBACKS (GpuReadbacksMode)
    u32 ReadbacksMode() const noexcept {
        return readbacks_mode;
    }

    /// sotc: number of GPU-modified pages in a region (statistics only, SOTC_RB_NARROW)
    u64 CountGpuModifiedPages(VAddr cpu_addr, u64 size) noexcept {
        u64 pages = 0;
        IteratePages(cpu_addr, size, [&pages](RegionManager* manager, u64 offset, u64 size) {
            pages += manager->CountGpuModifiedPages(offset, size);
        });
        return pages;
    }

    /// sotc: SOTC_UNTRACK_AUTO: like UnmarkRegionAsGpuModified(.., false), after a download wrote
    /// the GPU's bytes into guest memory: untracked pages get their CPU bit back (see retrack in
    /// RegionManager::ChangeRegionState).
    void UnmarkDownloadedAsGpuModified(VAddr cpu_addr, u64 size) noexcept {
        IteratePages(cpu_addr, size, [](RegionManager* manager, u64 offset, u64 size) {
            manager->template ChangeRegionState<StateOp::None, StateOp::Clear>(offset, size, true);
        });
    }

    /// Unmark region as modified from the host GPU
    void UnmarkRegionAsGpuModified(VAddr cpu_addr, u64 size, bool is_write) noexcept {
        IteratePages(cpu_addr, size, [is_write](RegionManager* manager, u64 offset, u64 size) {
            if (is_write) {
                manager->template ChangeRegionState<StateOp::Set, StateOp::Clear>(offset, size);
            } else {
                manager->template ChangeRegionState<StateOp::None, StateOp::Clear>(offset, size);
            }
        });
    }

    /// sotc: after a write fault, also marks the following pages (up to size bytes from
    /// cpu_addr, page aligned) CPU-modified, which unprotects them, as long as they hold no GPU
    /// data and are still tracked. SotC copies large blocks with memcpy every frame: one fault
    /// per page otherwise. A page marked but not written only costs an upload.
    void MarkAheadAsCpuModified(VAddr cpu_addr, u64 size) noexcept {
        IteratePages(cpu_addr, size, [](RegionManager* manager, u64 offset, u64 size) {
            const auto bounds = manager->GetBounds(offset, size);
            manager->Lock(bounds);
            u64 run = 0;
            while (run < size &&
                   !manager->template IsRegionModified<Type::GPU>(offset + run, BYTES_PER_PAGE) &&
                   !manager->template IsRegionModified<Type::CPU>(offset + run, BYTES_PER_PAGE)) {
                run += BYTES_PER_PAGE;
            }
            if (run != 0) {
                manager->template ChangeRegionState<StateOp::Set, StateOp::None, false>(offset,
                                                                                        run);
                if (SotcStats::kFpsLog) { // sotc: SOTC_FPS "ahead pages"
                    SotcStats::FaultStat::ahead_pages.fetch_add(run / BYTES_PER_PAGE,
                                                                std::memory_order_relaxed);
                }
            }
            manager->Unlock(bounds);
            return run < size;
        });
    }

    /// sotc: SOTC_WRITE_AHEAD_ADAPT. Unlocked look at [cpu_addr, cpu_addr + size) (page aligned):
    /// clean[i] = 1 when page i is neither CPU- nor GPU-modified. A page of a range without a
    /// tracking region stays 0 (it is not tracked: writable already). Only a hint for what
    /// MarkAheadSnapshotted then re-checks under the region lock.
    void PeekCleanPages(VAddr cpu_addr, u64 size, std::vector<u8>& clean) {
        clean.assign(size / BYTES_PER_PAGE, 0);
        IteratePages(cpu_addr, size, [&](RegionManager* manager, u64 offset, u64 len) {
            const u64 first = (manager->CpuAddress() + offset - cpu_addr) / BYTES_PER_PAGE;
            for (u64 i = 0; i < len / BYTES_PER_PAGE; ++i) {
                const u64 off = offset + i * BYTES_PER_PAGE;
                clean[first + i] =
                    !manager->template IsRegionModified<Type::GPU>(off, BYTES_PER_PAGE) &&
                    !manager->template IsRegionModified<Type::CPU>(off, BYTES_PER_PAGE);
            }
        });
    }

    /// sotc: SOTC_WRITE_AHEAD_ADAPT. Marks CPU-modified (unprotects) the pages of [cpu_addr,
    /// cpu_addr + size) that candidates[i] allows and that, checked again under their region's
    /// lock, are still neither CPU- nor GPU-modified; pages that are not are skipped (not
    /// marking is always safe). Per run of consecutive marked pages note(address, pages) is
    /// called under the lock, before the run is unprotected: the caller records the snapshots
    /// of the pages there, and nothing can write them until the run is unprotected (they are
    /// write-protected, and any other fault on the region waits for this lock). Returns the pages
    /// marked.
    template <typename Note>
    u64 MarkAheadSnapshotted(VAddr cpu_addr, u64 size, const std::vector<u8>& candidates,
                             Note&& note) {
        u64 marked = 0;
        IteratePages(cpu_addr, size, [&](RegionManager* manager, u64 offset, u64 len) {
            const u64 first = (manager->CpuAddress() + offset - cpu_addr) / BYTES_PER_PAGE;
            const auto bounds = manager->GetBounds(offset, len);
            manager->Lock(bounds);
            u64 run_begin = 0;
            u64 run_pages = 0;
            const auto flush = [&] {
                if (run_pages == 0) {
                    return;
                }
                note(manager->CpuAddress() + run_begin, run_pages);
                manager->template ChangeRegionState<StateOp::Set, StateOp::None, false>(
                    run_begin, run_pages * BYTES_PER_PAGE);
                marked += run_pages;
                run_pages = 0;
            };
            for (u64 i = 0; i < len / BYTES_PER_PAGE; ++i) {
                const u64 off = offset + i * BYTES_PER_PAGE;
                if (candidates[first + i] &&
                    !manager->template IsRegionModified<Type::GPU>(off, BYTES_PER_PAGE) &&
                    !manager->template IsRegionModified<Type::CPU>(off, BYTES_PER_PAGE)) {
                    if (run_pages == 0) {
                        run_begin = off;
                    }
                    ++run_pages;
                } else {
                    flush();
                }
            }
            flush();
            manager->Unlock(bounds);
        });
        return marked;
    }

    /// sotc: calls write(address, size) for the runs of [cpu_addr, cpu_addr + size) whose pages
    /// are not CPU-modified, with their region's lock held. Those pages are write-protected, so
    /// a guest write to them faults into InvalidateRegion and waits for this lock: the guest's
    /// bytes land after the written ones, never under them (hot readbacks; checking first and
    /// writing after the lock was released could overwrite a write made in between). Untracked
    /// pages are written. Returns the bytes skipped.
    template <typename Func>
    u64 WriteUnlessCpuModified(VAddr cpu_addr, u64 size, Func&& write) {
        return WriteUnlessCpuModified(cpu_addr, size, write, [](VAddr, u64) {});
    }
    /// sotc: same, and on_skip(addr, size) for every part skipped because of the CPU bit (decided
    /// under the region lock, so the caller knows exactly which bytes were not written).
    template <typename Func, typename Skip>
    u64 WriteUnlessCpuModified(VAddr cpu_addr, u64 size, Func&& write, Skip&& on_skip) {
        u64 skipped = 0;
        u64 page_index = cpu_addr >> HIGHER_PAGE_BITS;
        u64 offset = cpu_addr & HIGHER_PAGE_MASK;
        for (u64 remaining = size; remaining > 0; ++page_index, offset = 0) {
            const u64 len = std::min(HIGHER_PAGE_SIZE - offset, remaining);
            const VAddr base = page_index << HIGHER_PAGE_BITS;
            remaining -= len;
            RegionManager* manager = top_tier[page_index];
            if (!manager) {
                write(base + offset, len);
                continue;
            }
            const auto bounds = manager->GetBounds(offset, len);
            manager->Lock(bounds);
            const u64 end = offset + len;
            u64 run = offset;
            for (u64 page = offset; page < end;) {
                const u64 next = std::min((page & ~(BYTES_PER_PAGE - 1)) + BYTES_PER_PAGE, end);
                if (manager->template IsRegionModified<Type::CPU>(page, next - page)) {
                    if (page > run) {
                        write(base + run, page - run);
                    }
                    run = next;
                    skipped += next - page;
                    on_skip(base + page, next - page);
                }
                page = next;
            }
            if (end > run) {
                write(base + run, end - run);
            }
            manager->Unlock(bounds);
        }
        return skipped;
    }

    /// sotc: SOTC_DROP_FAST. Marks the region CPU-modified when none of its pages is
    /// GPU-modified, checking and marking each tracking region under its lock (as a guest write
    /// fault does, InvalidateRegion). Returns false at the first GPU-modified page: the regions
    /// before it are already marked, which only costs an upload.
    bool MarkCpuModifiedUnlessGpu(VAddr cpu_addr, u64 size) noexcept {
        SotcStats::mem_epoch.fetch_add(1, std::memory_order_relaxed); // reused binds
        return !IteratePages(cpu_addr, size, [](RegionManager* manager, u64 offset, u64 size) {
            const auto bounds = manager->GetBounds(offset, size);
            manager->Lock(bounds);
            const bool gpu = manager->template IsRegionModified<Type::GPU>(offset, size);
            if (!gpu) {
                manager->template ChangeRegionState<StateOp::Set, StateOp::None, false>(offset,
                                                                                        size);
            }
            manager->Unlock(bounds);
            return gpu;
        });
    }

    /// Mark region as modified from the CPU
    void MarkRegionAsCpuModified(VAddr cpu_addr, u64 size) noexcept {
        SotcStats::mem_epoch.fetch_add(1, std::memory_order_relaxed); // reused binds
        IteratePages(cpu_addr, size, [](RegionManager* manager, u64 offset, u64 size) {
            manager->template ChangeRegionState<StateOp::Set, StateOp::None>(offset, size);
        });
    }

    /// Removes all protection from a page and ensures GPU data has been flushed if requested
    /// sotc: guest_fault (a guest thread's write fault) feeds the repeat-fault grace.
    void InvalidateRegion(VAddr cpu_addr, u64 size, auto&& on_flush,
                          bool guest_fault = false) noexcept {
        SotcStats::mem_epoch.fetch_add(1, std::memory_order_relaxed); // reused binds
        if (readbacks_mode == GpuReadbacksMode::Disabled) {
            return MarkRegionAsCpuModified(cpu_addr, size);
        }
        bool should_flush = false;
        IteratePages(cpu_addr, size, [&should_flush, guest_fault](RegionManager* manager,
                                                                 u64 offset, u64 size) {
            const auto bounds = manager->GetBounds(offset, size);
            manager->Lock(bounds);
            if (guest_fault) {
                manager->NoteGuestWriteFaultLocked(offset);
            }
            // sotc: SOTC_UNTRACK_AUTO self-check. A protected, untracked page without a GPU bit
            // faulted: the untracking did not hold, so the list is dropped (SotcUntrack::Trip).
            if (guest_fault && SotcUntrack::AutoOn()) {
                if (const u64 bad = manager->CountUntrackedFaultsLocked(offset, size)) {
                    SotcUntrack::NoteUntrackedFaults(bad);
                }
            }
            const bool modified = manager->template IsRegionModified<Type::GPU>(offset, size);
            if (!modified) {
                manager->template ChangeRegionState<StateOp::Set, StateOp::None, false>(offset,
                                                                                        size);
            }
            should_flush |= modified;
            manager->Unlock(bounds);
        });
        if (should_flush) {
            // sotc: SOTC_FPS: time the readback of a write fault ("SOTC FAULTS" line, readback)
            SotcStats::FaultStat::Scope flush_scope{&SotcStats::FaultStat::Tl::flush_ns};
            on_flush();
        }
    }

    /// Call 'func' for each CPU modified range and unmark those pages as CPU modified.
    /// sotc: SOTC_SYNC_PIPE. Returns true when every tracking region of the range was clean at its
    /// unlocked look (RegionManager::ForEachModifiedRange returned true for it). Then func was not
    /// called and no bit changed, so the range was clean before this call: its verdict equals
    /// IsSyncClean(cpu_addr, size, is_written) taken right after. False otherwise.
    bool ForEachUploadRange(VAddr cpu_addr, u64 size, bool is_written, auto&& func) {
        bool clean = size != 0; // sotc: IsSyncClean is false for an empty range, so is this
        if (size == 0) {
            return clean;
        }
        IteratePages<true>(cpu_addr, size,
                           [&func, &clean, is_written](RegionManager* manager, u64 offset,
                                                       u64 size) {
                               bool region_clean;
                               if (is_written) {
                                   region_clean = manager->template ForEachModifiedRange<
                                       Type::CPU, StateOp::Clear, StateOp::Set>(offset, size, func);
                               } else {
                                   region_clean = manager->template ForEachModifiedRange<
                                       Type::CPU, StateOp::Clear, StateOp::None>(offset, size,
                                                                                 func);
                               }
                               clean = clean && region_clean;
                           });
        return clean;
    }

    /// sotc: SOTC_SYNC_FAST. Sets gen to the sum of the write generations over [cpu_addr,
    /// cpu_addr + size) (RegionManager::SyncGenerationSum). False when the range is empty or a
    /// tracking region over it does not exist yet: ForEachUploadRange creates it with every page
    /// CPU-modified, so the full path is needed anyway.
    bool SyncGeneration(VAddr cpu_addr, u64 size, u64& gen) const noexcept {
        gen = 0;
        if (size == 0) {
            return false;
        }
        return ForEachRegionStrict(cpu_addr, size, [&gen](RegionManager* m, u64 offset, u64 len) {
            gen += m->SyncGenerationSum(offset, len);
            return true;
        });
    }

    /// sotc: SOTC_SYNC_FAST. True when ForEachUploadRange(cpu_addr, size, is_written) would find
    /// nothing to do right now (RegionManager::IsSyncClean); false for missing regions.
    bool IsSyncClean(VAddr cpu_addr, u64 size, bool is_written) const noexcept {
        if (size == 0) {
            return false;
        }
        return ForEachRegionStrict(cpu_addr, size,
                                   [is_written](RegionManager* m, u64 offset, u64 len) {
                                       return m->IsSyncClean(offset, len, is_written);
                                   });
    }

    /// Call 'func' for each GPU modified range and unmark those pages as GPU modified
    template <bool clear>
    void ForEachDownloadRange(VAddr cpu_addr, u64 size, auto&& func) {
        constexpr auto gpu_op = clear ? StateOp::Clear : StateOp::None;
        IteratePages(cpu_addr, size, [&func](RegionManager* manager, u64 offset, u64 size) {
            manager->template ForEachModifiedRange<Type::GPU, StateOp::None, gpu_op>(offset, size,
                                                                                     func);
        });
    }

private:
    /// sotc: SOTC_SYNC_FAST. func(region, offset, size) -> bool over each tracking region of the
    /// range; false at the first missing region or the first func returning false.
    template <typename Func>
    bool ForEachRegionStrict(VAddr cpu_address, u64 size, Func&& func) const noexcept {
        u64 page_index = cpu_address >> HIGHER_PAGE_BITS;
        u64 page_offset = cpu_address & HIGHER_PAGE_MASK;
        for (u64 remaining = size; remaining > 0; ++page_index, page_offset = 0) {
            const u64 len = std::min(HIGHER_PAGE_SIZE - page_offset, remaining);
            // sotc: SOTC_WRITE_DECL: lock-free reader, acquire (see CreateRegion).
            RegionManager* region = __atomic_load_n(&top_tier[page_index], __ATOMIC_ACQUIRE);
            if (!region || !func(region, page_offset, len)) {
                return false;
            }
            remaining -= len;
        }
        return true;
    }

    template <bool create_region_on_fail = false, typename Func>
    bool IteratePages(VAddr cpu_address, u64 size, Func&& func) {
        using FuncReturn = typename std::invoke_result<Func, RegionManager*, u64, size_t>::type;
        static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
        u64 remaining_size = size;
        u64 page_index = cpu_address >> HIGHER_PAGE_BITS;
        u64 page_offset = cpu_address & HIGHER_PAGE_MASK;
        while (remaining_size > 0) {
            const u64 copy_amount = std::min(HIGHER_PAGE_SIZE - page_offset, remaining_size);
            if (auto* region = top_tier[page_index]; region) {
                if constexpr (BOOL_BREAK) {
                    if (func(region, page_offset, copy_amount)) {
                        return true;
                    }
                } else {
                    func(region, page_offset, copy_amount);
                }
            } else if constexpr (create_region_on_fail) {
                region = CreateRegion(page_index);
                if constexpr (BOOL_BREAK) {
                    if (func(region, page_offset, copy_amount)) {
                        return true;
                    }
                } else {
                    func(region, page_offset, copy_amount);
                }
            }
            page_index++;
            page_offset = 0;
            remaining_size -= copy_amount;
        }
        return false;
    }

    RegionManager* CreateRegion(u64 page_index) {
        const VAddr base_cpu_addr = page_index << HIGHER_PAGE_BITS;
        if (free_managers.empty()) {
            manager_pool.emplace_back();
            auto& last_pool = manager_pool.back();
            for (size_t i = 0; i < MANAGER_POOL_SIZE; i++) {
                std::construct_at(&last_pool[i], tracker, 0);
                free_managers.push_back(&last_pool[i]);
            }
        }
        auto* new_manager = free_managers.back();
        new_manager->SetCpuAddress(base_cpu_addr);
        free_managers.pop_back();
        // sotc: SOTC_WRITE_DECL: the lock-free readers (acquire loads) see the region built above.
        __atomic_store_n(&top_tier[page_index], new_manager, __ATOMIC_RELEASE);
        // sotc: SOTC_TRACE "redirty first touch": the region's CPU bits were filled with ones in
        // the RegionManager constructor (pool slots), so its whole 16 MiB counts as CPU-modified.
        if (SotcTrace::kOn) {
            const uint64_t now = SotcTrace::Now();
            SotcTrace::PushEvent(SotcTrace::EvInstant, "redirty first touch", now, now,
                                 HIGHER_PAGE_SIZE, base_cpu_addr);
        }
        live_managers.push_back(new_manager);
        return new_manager;
    }

public:
    /// sotc: calls f(address, size) for each run of 256 KiB words whose CPU bits rose since the
    /// last call (RegionManager::TakeSweepWords). GPU command thread only (as CreateRegion).
    template <typename F>
    void ForEachSweepSpan(F&& f) {
        for (size_t i = 0; i < live_managers.size(); ++i) {
            RegionManager* manager = live_managers[i];
            u64 words = manager->TakeSweepWords();
            while (words) {
                const int first = std::countr_zero(words);
                const int count = std::countr_one(words >> first);
                f(manager->CpuAddress() + u64(first) * BYTES_PER_WORD, u64(count) * BYTES_PER_WORD);
                words = first + count >= 64 ? 0 : words & (~0ULL << (first + count));
            }
        }
    }

    /// sotc: SOTC_UNTRACK_AUTO. Gives every live region its untracked mask for `ranges` (the list
    /// the flip thread published, SotcUntrack::AutoSnapshot). GPU command (draw) thread only, as
    /// ForEachSweepSpan. With readbacks disabled nothing is untracked (the self-check is blind there).
    void ApplyUntracked(const std::vector<SotcUntrack::Range>& ranges) {
        const bool enabled = readbacks_mode != GpuReadbacksMode::Disabled;
        for (size_t i = 0; i < live_managers.size(); ++i) {
            RegionManager* manager = live_managers[i];
            manager->SetUntrackedMask(
                enabled ? RegionManager::BuildUntrackedMask(manager->CpuAddress(), ranges)
                        : RegionBits{});
        }
    }

    /// sotc: SOTC_CLASS_TRACK=2 shadow. Total GPU-marked pages that fall in `ranges` across every
    /// live region, without changing state. Used to validate the allocator classification.
    u64 CountClassGpuPages(const std::vector<SotcUntrack::Range>& ranges) {
        u64 pages = 0;
        for (RegionManager* manager : live_managers) {
            pages += manager->CountGpuInMask(
                RegionManager::BuildUntrackedMask(manager->CpuAddress(), ranges));
        }
        return pages;
    }

private:
    std::vector<RegionManager*> live_managers;

    PageManager* tracker;
    const u32 readbacks_mode;
    std::deque<std::array<RegionManager, MANAGER_POOL_SIZE>> manager_pool;
    std::vector<RegionManager*> free_managers;
    std::array<RegionManager*, NUM_HIGH_PAGES> top_tier{};
};

} // namespace VideoCore
