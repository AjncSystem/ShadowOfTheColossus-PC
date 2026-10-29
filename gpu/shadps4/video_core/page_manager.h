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
/// sotc: any byte of the range is in a registered guest stack (never write-protected).
bool IntersectsGuestStack(VAddr address, u64 size);

/// sotc: SOTC_FAULT_OSCOST=1 (needs SOTC_FPS): what the operating system adds to a write fault
/// around the handler, in microseconds: from the faulting store to the entry of
/// bbgpu_handle_fault, and from the handler's exit to the store completing. Measured once (the
/// median of a few faults on a private page); false when unavailable.
bool MeasureFaultOsCost(double& in_us, double& out_us);

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
    /// sotc: deferrable (SOTC_PROTECT_BATCH): the host protection change may be collected by
    /// this thread's open ProtectBatch instead of being applied now. Only re-protects of pages
    /// no GPU data lives in qualify (see ProtectBatch); the caller decides.
    void UpdatePageWatchersForRegion(VAddr base_addr, const Bounds& bounds,
                                     const RegionBits& write_mask, const RegionBits& read_mask,
                                     PageOp write_op, PageOp read_op,
                                     bool deferrable = false) const;

    /// sotc: SOTC_PROTECT_BATCH=1 (off by default). While a ProtectBatch is open on a thread,
    /// the deferrable protection changes that thread makes (upload re-protects,
    /// BufferCache::SynchronizeMemoryFull) are collected instead of each issuing its own host
    /// protect, and applied when the batch closes (any open one: an inner scope flushes the
    /// outer one's changes too) or Flush() is called: adjacent runs
    /// (also across tracking regions) as one call. The flush re-reads every page's watcher state
    /// under its page lock and protects only pages whose state still is the collected one; a page
    /// whose state moved since was protected by the update that moved it (every update protects
    /// the pages it changes, under the page lock, from the state it leaves). The caller must
    /// flush before it reads guest memory that relies on the protection being in place.
    class ProtectBatch {
    public:
        explicit ProtectBatch(bool enable) noexcept;
        ~ProtectBatch();
        ProtectBatch(const ProtectBatch&) = delete;
        ProtectBatch& operator=(const ProtectBatch&) = delete;
        /// Applies what was collected so far (the batch stays open).
        void Flush();

    private:
        bool active;
    };
    /// sotc: SOTC_PROTECT_BATCH. A ProtectBatch is open on this thread (and not suspended by a
    /// fault being handled on it).
    static bool ProtectBatchOpen() noexcept;

    /// sotc: SOTC_PROTECT_COALESCE. Makes the page at `page` writable on the host if the tracker's
    /// state for it is read-write and the host protection is not (see FaultProtectScope).
    static void EnsureFaultPageWritable(VAddr page);

    /// sotc: SOTC_PROTECT_COALESCE=1 (off by default). Opened by the guest write fault handler:
    /// while it is open on a thread, the tracker updates that thread makes which only REMOVE
    /// restrictions (a page going from read-only to read-write: the faulting page, the fault
    /// granule, the write-ahead, an image's untracked pages) collect their host protect instead of
    /// issuing one call each; Flush() (and the destructor) applies them as one call per run.
    ///
    /// Why a write cannot be missed. The invariant that keeps the GPU's view exact is: a page whose
    /// CPU bit is clear (clean, uploaded) is write-protected on the host whenever no page lock is
    /// held. Deferring an unprotect only keeps the host MORE restrictive than the tracker for a
    /// while, never less, so a clean page is never writable. The cost of the delay is liveness of
    /// the guest, which the handler pays itself: the faulting thread does not return before it has
    /// applied its own changes, and it makes its own page writable if the tracker says so (another
    /// thread's pending flush may be what covered it), so no thread ever waits for another's flush.
    /// Applying is a function of the CURRENT state: the flush locks each page and protects it only
    /// if its tracker state still is the collected one (read-write); a page whose state moved on
    /// (the draw thread uploaded it and protected it again, a texture tracked it) was protected by
    /// the update that moved it, under the same page lock, from the state it left. So the host
    /// protection of every page still ends as its last update's state, as without coalescing.
    /// Updates that restrict (re-protects after an upload, GPU read tracking, readbacks) and
    /// anything from another thread are never collected.
    ///
    /// Every handled write fault, with or without an active scope (the command processor's, one
    /// that did not collect), applies the tracker's state for its own page at the end
    /// (EnsureFaultPageWritable), so a page covered by another thread's pending flush never makes
    /// a second thread fault until that flush runs, whatever the first thread blocks on meanwhile.
    class FaultProtectScope {
    public:
        explicit FaultProtectScope(bool enable) noexcept;
        ~FaultProtectScope();
        FaultProtectScope(const FaultProtectScope&) = delete;
        FaultProtectScope& operator=(const FaultProtectScope&) = delete;
        /// Applies what was collected so far, then makes fault_page (0: none) writable if the
        /// tracker wants it writable and this scope's runs did not cover it.
        void Flush(VAddr fault_page);
        /// The scope collects (SOTC_PROTECT_COALESCE on, a guest thread's fault).
        bool Active() const noexcept { return active; }

    private:
        bool active;
    };

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
