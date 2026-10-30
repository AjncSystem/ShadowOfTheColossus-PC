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
//   1  apply: feed the CPU-only ranges to the SotcUntrack machinery (region_manager.h). Only after
//      a clean shadow session. Not wired yet; it prints a note pointing at the shadow mode.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SotcMemClasses {

constexpr uint32_t kMaxPools = 64; ///< distinct pool names; later ones share the last slot

// 0 off, 1 apply, 2 shadow. Read once from SOTC_CLASS_TRACK.
int Mode();
inline bool On() {
    return Mode() != 0;
}

// Fed by the allocator probe. cpu_only: the pool is CPUMemory (kind 1) and not a GPU staging pool.
// base/size are the allocation's guest range; pool is its allocator name ("?" when unreadable).
// Cheap no-op when Mode() == 0.
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

// Shadow results for the window, called by the draw thread: the totals, and the GPU-marked pages
// found inside one pool's ranges (should be ~0 for every pool).
void NoteShadow(uint64_t ranges, uint64_t cpu_only_bytes, uint64_t gpu_pages);
void NoteShadowPool(uint32_t pool, uint64_t gpu_pages);

// Report lines (driver.cpp 5 s report), reset for the next window. Empty when off or idle.
void Report(std::string& out, double seconds);

} // namespace SotcMemClasses
