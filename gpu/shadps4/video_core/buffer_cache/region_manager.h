// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <utility>

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
    }
    explicit RegionManager() = default;

    void SetCpuAddress(VAddr new_cpu_addr) {
        cpu_addr = new_cpu_addr;
        ResetGrace();
    }

    /// sotc: a guest write fault on the page at offset (region mutex held, from
    /// MemoryTracker::InvalidateRegion). The second one within 1-2 quanta makes the page hot.
    void NoteGuestWriteFaultLocked(u64 offset) {
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

    template <StateOp cpu_op, StateOp gpu_op, bool locked = true>
    void ChangeRegionState(u64 offset, u64 size) {
        RegionBits write_prot;
        RegionBits read_prot;
        auto bounds = GetBounds(offset, size);
        Bounds watcher_bounds;
        if constexpr (locked) {
            mutex.lock();
        }
        IterateWords(bounds, [&](u64 index, u64 mask) {
            UpdateStateAndProtection<cpu_op, gpu_op>(write_prot, read_prot, index, mask);
        });
        const auto write_op = GetPageOp<Type::CPU>(cpu_op);
        const auto read_op = GetPageOp<Type::GPU>(gpu_op);
        const bool update_watchers = write_op != PageOp::None || read_op != PageOp::None;
        if (update_watchers &&
            GetWatcherBounds<cpu_op, gpu_op>(bounds, write_prot, read_prot, watcher_bounds)) {
            tracker->UpdatePageWatchersForRegion(cpu_addr, watcher_bounds, write_prot, read_prot,
                                                 write_op, read_op);
        }
        if constexpr (locked) {
            mutex.unlock();
        }
    }

    template <Type type, StateOp cpu_op, StateOp gpu_op, bool locked = true>
    void ForEachModifiedRange(u64 offset, s64 size, auto&& func) {
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
                return;
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
        }
        IterateWords(bounds, [&](u64 index, u64 mask) {
            const u64 base_page = index * PAGES_PER_WORD;
            const u64 word = state[index] & mask; // collected before the change: hot pages too
            u64 keep = 0;
            if constexpr (grace_op) {
                if (grace_any) {
                    keep = (hot_cur[index] | hot_prev[index]) & word;
                    if (keep) {
                        SotcStats::fault_grace_hot_upload_bytes.fetch_add(
                            std::popcount(keep) * BYTES_PER_PAGE, std::memory_order_relaxed);
                    }
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
        const auto write_op = GetPageOp<Type::CPU>(cpu_op);
        const auto read_op = GetPageOp<Type::GPU>(gpu_op);
        const bool update_watchers = write_op != PageOp::None || read_op != PageOp::None;
        if (update_watchers &&
            GetWatcherBounds<cpu_op, gpu_op>(bounds, write_prot, read_prot, watcher_bounds)) {
            tracker->UpdatePageWatchersForRegion(cpu_addr, watcher_bounds, write_prot, read_prot,
                                                 write_op, read_op);
        }
        if constexpr (locked) {
            mutex.unlock();
        }
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

    void Lock(const Bounds& bounds) noexcept {
        mutex.lock();
    }

    void Unlock(const Bounds& bounds) noexcept {
        mutex.unlock();
    }

private:
    void ResetGrace() {
        fault_cur.Fill(0);
        fault_prev.Fill(0);
        hot_cur.Fill(0);
        hot_prev.Fill(0);
        grace_epoch = 0;
        grace_any = false;
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
    LockType mutex;
};

} // namespace VideoCore
