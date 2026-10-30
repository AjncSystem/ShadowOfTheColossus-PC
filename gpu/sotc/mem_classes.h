// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: memory classes (plan step 6). The block-allocator probe (gpu/sotc/probe_alloc.cpp) feeds
// every live allocation here with its guest range, its allocator pool name and whether the pool is
// CPU-only (kind 1, CPUMemory, and not a GPU upload/staging pool). This builds a range -> class
// table that marks the ranges the port never needs to CPU-write-protect.
//
// SOTC_CLASS_TRACK:
//   0  off (default): OnAlloc/OnFree are cheap no-ops; nothing changes.
//   2  shadow: the draw thread (BufferCache::SynchronizeDmaBuffers) counts GPU-marked pages that
//      fall inside the CPU-only ranges, per pool. It must stay ~0 over a long session; a non-zero
//      count means a range the allocator called CPU-only was written by the GPU, so untracking it
//      would be unsafe. No behaviour change. Needs SOTC_HOOK_ALLOC_PROBE=1 (the feed) and SOTC_FPS=1.
//   1  apply: the CPU-only ranges of the pools in SOTC_CLASS_POOLS (default "PageAllocator") are
//      published through SotcUntrack (region_manager.h) and kept unprotected, like SOTC_UNTRACK_AUTO.
//      Needs SOTC_HOOK_ALLOC_PROBE=1. Refused (mode 0, one log line) with SOTC_UNTRACK_AUTO=1 or
//      SOTC_CP_AHEAD set. SotcUntrack's self-check still trips and releases everything on a bad fault.
//
// Both non-zero modes count guest write faults per pool (SOTC_FPS report, "faults N/s").
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SotcMemClasses {

constexpr uint32_t kMaxPools = 64; ///< distinct pool names; later ones share the last slot

// 0 off, 1 apply, 2 shadow. Read once from SOTC_CLASS_TRACK. Mode 1 is refused (returns 0) when
// SOTC_UNTRACK_AUTO=1 or SOTC_CP_AHEAD is set.
int Mode();
inline bool On() {
    return Mode() != 0;
}

// Fed by the allocator probe. cpu_only: the pool is CPUMemory (kind 1) and not a GPU staging pool.
// base/size are the allocation's guest range; pool is its allocator name ("?" when unreadable).
// A range with size 0, size > 4 GiB, base >= 2^48 or base + size overflowing is skipped (the bounds
// of SotcUntrack::Parse) and counted in the report. Cheap no-op when Mode() == 0.
void OnAlloc(uint64_t base, uint64_t size, bool cpu_only, const char* pool);
void OnFree(uint64_t base);

// A guest range classified CPU-only (addr/size match VideoCore::SotcUntrack::Range's fields) and the
// index of its pool (< kMaxPools).
struct Range {
    uint64_t addr;
    uint64_t size;
    uint32_t pool;
};

// The current CPU-only ranges and a version that bumps on every change. Any thread.
std::vector<Range> Snapshot(uint64_t& version);

// Mode 1: the CPU-only ranges of the apply pools (SOTC_CLASS_POOLS), and the table version they
// came from. Draw thread.
std::vector<Range> ApplyRanges(uint64_t& version);

// The current table version (an atomic load, no lock). Draw thread, every sync.
uint64_t Version();

// Rebuilds the lock-free lookup used by NoteWriteFault when the table changed since the last
// rebuild. Cheap when nothing changed. Throttled: a rebuild runs at most once every 16 syncs and at
// most once every 100 ms. While the table is dirty and throttled, the lookup is older than the
// table, so faults in a range allocated since the last rebuild are not counted yet, and faults in a
// range freed since then still are (lag of a few syncs; the counts are for the report only, the
// protection logic does not read them). Draw thread, every sync in modes 1 and 2.
void RefreshLookup();

// Guest write fault at addr (page_manager.cpp, the write branch of HandleGuestFault). Lock-free,
// no allocation: finds the CPU-only range holding addr in the published lookup and counts the fault
// for its pool. Call only when Mode() != 0.
void NoteWriteFault(uint64_t addr);

// Mode 1 state, from the draw thread. NotePublish: the list was handed to SotcUntrack (the ranges
// and bytes it now holds). NoteApplyState: every sync, the trip state and the untracked-fault count.
void NotePublish(uint64_t ranges, uint64_t bytes);
void NoteApplyState(bool tripped, uint64_t faults_untracked);

// Shadow results for the window, called by the draw thread: the totals, and the GPU-marked pages
// found inside one pool's ranges (should be ~0 for every pool).
void NoteShadow(uint64_t ranges, uint64_t cpu_only_bytes, uint64_t gpu_pages);
void NoteShadowPool(uint32_t pool, uint64_t gpu_pages);
// Ranges of a pool that overlap a texture-cache image, counted by the draw thread (both modes).
// Render targets and storage images do not set region GPU bits, so the GPU-page count cannot see
// their writes; mode 1 leaves such ranges tracked.
void NoteImageRanges(uint32_t pool, uint64_t ranges);

// Report lines (driver.cpp 5 s report), reset for the next window. Empty when off or idle.
void Report(std::string& out, double seconds);

} // namespace SotcMemClasses
