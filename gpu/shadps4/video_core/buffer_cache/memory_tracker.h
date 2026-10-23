// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <deque>
#include <vector>

#include "common/types.h"
#include "video_core/buffer_cache/region_manager.h"
#include "video_core/sotc_stats.h"

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
            }
            manager->Unlock(bounds);
            return run < size;
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
            const bool modified = manager->template IsRegionModified<Type::GPU>(offset, size);
            if (!modified) {
                manager->template ChangeRegionState<StateOp::Set, StateOp::None, false>(offset,
                                                                                        size);
            }
            should_flush |= modified;
            manager->Unlock(bounds);
        });
        if (should_flush) {
            on_flush();
        }
    }

    /// Call 'func' for each CPU modified range and unmark those pages as CPU modified
    void ForEachUploadRange(VAddr cpu_addr, u64 size, bool is_written, auto&& func) {
        IteratePages<true>(
            cpu_addr, size, [&func, is_written](RegionManager* manager, u64 offset, u64 size) {
                if (is_written) {
                    manager->template ForEachModifiedRange<Type::CPU, StateOp::Clear, StateOp::Set>(
                        offset, size, func);
                } else {
                    manager
                        ->template ForEachModifiedRange<Type::CPU, StateOp::Clear, StateOp::None>(
                            offset, size, func);
                }
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
        top_tier[page_index] = new_manager;
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

private:
    std::vector<RegionManager*> live_managers;

    PageManager* tracker;
    const u32 readbacks_mode;
    std::deque<std::array<RegionManager, MANAGER_POOL_SIZE>> manager_pool;
    std::vector<RegionManager*> free_managers;
    std::array<RegionManager*, NUM_HIGH_PAGES> top_tier{};
};

} // namespace VideoCore
