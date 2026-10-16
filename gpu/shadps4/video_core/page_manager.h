// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include "common/alignment.h"
#include "common/types.h"
#include "video_core/buffer_cache/region_definitions.h"

namespace Vulkan {
class Rasterizer;
}

namespace VideoCore {

/// Guest thread and fiber stacks. The page tracker never makes them inaccessible: on Windows
/// a fault whose exception frame cannot be written below the faulting stack pointer kills the
/// process without running any handler.
void RegisterGuestStack(VAddr address, u64 size);
void UnregisterGuestStack(VAddr address, u64 size);

/// Calls func(address, size) for the parts of the range outside registered guest stacks.
/// Those pages are never protected, so the CPU may have written them without the tracker
/// noticing: GPU data read back must not be written over them.
/// sotc: non-owning reference to a void(VAddr, u64) callable, replacing const std::function&
/// (which type-erased, and could heap-allocate, the caller's lambda on every readback write).
/// Valid only for the duration of the call; ForEachNonStackRange never stores it.
class NonStackRangeFn {
public:
    template <typename F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, NonStackRangeFn> &&
                 std::is_invocable_v<F&, VAddr, u64>)
    NonStackRangeFn(F&& f) noexcept
        : obj{const_cast<void*>(static_cast<const void*>(std::addressof(f)))},
          call{[](void* o, VAddr address, u64 size) {
              (*static_cast<std::remove_reference_t<F>*>(o))(address, size);
          }} {}

    void operator()(VAddr address, u64 size) const {
        call(obj, address, size);
    }

private:
    void* obj;
    void (*call)(void*, VAddr, u64);
};
void ForEachNonStackRange(VAddr address, u64 size, NonStackRangeFn func);

struct UffdImpl;
struct SignalImpl;

enum class PageOp : s8 {
    None = 0,
    Track = 1,
    Untrack = -1,
};

class PageManager {
    static constexpr size_t PM_PAGE_BITS = 12;
    static constexpr size_t PM_PAGE_SIZE = 1ULL << PM_PAGE_BITS;

public:
    explicit PageManager(Vulkan::Rasterizer* rasterizer);
    ~PageManager();

    /// Register a range of mapped gpu memory.
    void OnGpuMap(VAddr address, size_t size);

    /// Unregister a range of gpu memory that was unmapped.
    void OnGpuUnmap(VAddr address, size_t size);

    /// Updates watches in the pages touching the specified region.
    void UpdatePageWatchers(VAddr addr, u64 size, PageOp write_op) const;

    /// Updates watches in the pages touching the inclusive bounds using a mask.
    void UpdatePageWatchersForRegion(VAddr base_addr, const Bounds& bounds,
                                     const RegionBits& write_mask, const RegionBits& read_mask,
                                     PageOp write_op, PageOp read_op) const;

    /// Returns page aligned address.
    static constexpr VAddr GetPageAddr(VAddr addr) {
        return Common::AlignDown(addr, PM_PAGE_SIZE);
    }

    /// Returns address of the next page.
    static constexpr VAddr GetNextPageAddr(VAddr addr) {
        return Common::AlignUp(addr + 1, PM_PAGE_SIZE);
    }

private:
    friend struct UffdImpl;
    friend struct SignalImpl;
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VideoCore
