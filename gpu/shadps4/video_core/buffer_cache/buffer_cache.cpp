// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <magic_enum/magic_enum.hpp>

#include "bbport_copy.h"
#include <immintrin.h>
#include <thread>
#include <unordered_set>
#include "bbport_toggles.h"
#include "common/alignment.h"
#include "common/scope_exit.h"
#include "core/debug_state.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/sotc_stats.h"
#include "video_core/buffer_cache/bpe_heap_guard.h"
#include <optional>

extern "C" uintptr_t runtime_sotc_heap(void);
extern "C" uintptr_t runtime_sotc_heap_table(void);
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/sotc_dispphase.h" // sotc: SOTC_DISP_PHASES
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

// sotc: src/runtime_memory.c, RAM census (SOTC_RAM_CENSUS=1).
extern "C" void runtime_memory_tag(const void* p, size_t size, const char* name);

namespace VideoCore {

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

/// sotc: SOTC_RB_NARROW=<KiB> (power of two, 4-512; 1 or unset = 64 KiB): the readback window,
/// in bytes; 0 keeps the 512 KiB one (Shrine: 46.0 FPS at 512 KiB, 47.9-48.8 at 64, 47.5 at 128,
/// 42.9 at 32, where the readbacks double). A readback unprotects every GPU-written page of its
/// window and the next GPU writes of those pages protect them again one binding at a time (most of
/// the draw thread's VirtualProtect calls), every frame; a smaller window leaves the pages the
/// guest does not read protected and GPU-modified. Read once, at static initialisation (no
/// function-local guard in the fault paths); BufferCache::rb_narrow_window applies it.
static const u64 rb_narrow_env = [] {
    const char* v = std::getenv("SOTC_RB_NARROW");
    const u64 kib = v ? std::strtoull(v, nullptr, 10) : 1;
    if (kib == 0) {
        return u64{0};
    }
    return std::bit_floor(kib == 1 ? u64{64} : std::clamp<u64>(kib, 4, 512)) * 1_KB;
}();

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    // sotc: this safety mode is unconditional; the old unsafe opt-in has been removed.
    std::fprintf(stderr, "SOTC BPE heap guard: fresh guard per readback batch for the heaps whose real extents a copy overlaps; "
                         "SOTC_HEAP_FAST=0 (forced); union of metadata, no extent cache\n");
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = ARENA_PAGE_SIZE,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = ARENA_PAGE_SIZE / block_size;
    blocks_per_arena_page_shift = ARENA_PAGE_BITS - block_shift;
    // sotc: one u16 per block of the address space (32 MiB for 64 KiB blocks); calloc leaves
    // the pages nothing touches uncommitted.
    block_last_use =
        static_cast<u16*>(std::calloc(blocks_per_arena_page * NUM_ARENA_PAGES, sizeof(u16)));
    ASSERT_MSG(block_last_use, "No memory for the arena block use table");
    runtime_memory_tag(block_last_use, blocks_per_arena_page * NUM_ARENA_PAGES * sizeof(u16),
                       "BufferCache block_last_use"); // sotc: RAM census label
    // sotc: one u8 per block (16 MiB virtual, untouched pages cost nothing): ArenaImageRelease.
    block_class = static_cast<u8*>(std::calloc(blocks_per_arena_page * NUM_ARENA_PAGES, 1));
    ASSERT_MSG(block_class, "No memory for the arena block class table");
    epoch_start = std::chrono::steady_clock::now();
    // sotc: SOTC_RB_NARROW only in the precise readback mode, where every GPU-modified page is
    // read-protected: the relaxed mode reads back only on write faults, and the wide window is
    // all that refreshes the data around them.
    if (rb_narrow_env && memory_tracker->ReadbacksMode() == GpuReadbacksMode::Precise) {
        rb_narrow_window = rb_narrow_env;
        SotcStats::rb_narrow_kib.store(rb_narrow_window / 1_KB, std::memory_order_relaxed);
    }
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();
    // System memory the sparse arenas can fall back to once video memory is exhausted.
    const auto& memory_properties = instance.GetMemoryProperties();
    for (u32 i = 0; i < memory_properties.memoryTypeCount; ++i) {
        const auto& heap = memory_properties.memoryHeaps[memory_properties.memoryTypes[i].heapIndex];
        if (((reqs.memoryTypeBits >> i) & 1) != 0 &&
            !(heap.flags & vk::MemoryHeapFlagBits::eDeviceLocal)) {
            arena_fallback_memory_type_index = i;
            break;
        }
    }

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * NUM_ARENA_PAGES) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * NUM_ARENA_PAGES);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
}

BufferCache::~BufferCache() {
    while (guest_copies_pending.load(std::memory_order_acquire) != 0) { // sotc: copy tasks use this
        std::this_thread::yield();
    }
    std::free(block_last_use);
    std::free(block_class);
}

void BufferCache::TickFrame() {
    if (std::exchange(fault_process_pending, false)) {
        fault_manager->ProcessFaultBuffer();
    }
}

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks,
                                   bool guest_fault) {
    if (SotcStats::kCmdStats) SotcStats::inv_buffer.fetch_add(size, std::memory_order_relaxed);
    Core::MemoryManager::NoteHostWrite(device_addr, size, 9,
                                       reinterpret_cast<u64>(__builtin_return_address(0)));
    memory_tracker->InvalidateRegion(
        device_addr, size,
        [this, device_addr, size, assume_locks] {
            ReadMemory(device_addr, size, true, assume_locks);
        },
        guest_fault);
    SotcStats::mem_epoch.fetch_add(1, std::memory_order_release); // after the marks (reused binds)
}

void BufferCache::DropGpuModified(VAddr device_addr, u64 size) {
    liverpool->SendCommand<true>([this, device_addr, size] {
        {
            std::unique_lock gpu_modified_lock{gpu_modified_mutex};
            gpu_modified_ranges.Subtract(device_addr, size);
        }
        memory_tracker->UnmarkRegionAsGpuModified(device_addr, size, false);
        MarkInflightReadbacksDirty(device_addr, size, CauseDrop);
        memory_tracker->MarkRegionAsCpuModified(device_addr, size);
    });
}

void BufferCache::MarkWriteAhead(VAddr addr) {
    static const u64 ahead = [] {
        const char* v = std::getenv("SOTC_WRITE_AHEAD_PAGES");
        return (v ? std::strtoull(v, nullptr, 10) : 15ULL) * 4_KB;
    }();
    if (ahead == 0) {
        return;
    }
    const VAddr next_page = Common::AlignUp(addr + 1, 4_KB);
    memory_tracker->MarkAheadAsCpuModified(next_page, ahead);
}

std::pair<VAddr, VAddr> BufferCache::ReadbackWindow(const Buffer* arena, VAddr device_addr,
                                                    u64 size) {
    // GPU-modified ranges come as many small scattered islands,
    // so the download is widened to a window around the request
    constexpr u64 WindowSize = 512_KB;
    const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
    const auto window = [&](u64 window_size) {
        const VAddr start =
            std::max<VAddr>(Common::AlignDown(device_addr, window_size), arena->cpu_addr);
        const VAddr end =
            std::min<VAddr>(std::max<VAddr>(start + window_size, device_addr + size), arena_end);
        return std::pair<VAddr, VAddr>{start, end};
    };
    if (rb_narrow_window == 0) {
        return window(WindowSize);
    }
    // sotc: SOTC_RB_NARROW. The window is the aligned granule (or granules) of the request, so
    // the same reads ask for the same window every frame and the hot readbacks (GatherDownloads)
    // bring the other granules the guest reads along with the first readback, as the wide window
    // did. Everything the guest reads is still downloaded: a page outside the window keeps its
    // GPU bit and read protection, and its own fault reads it back.
    const auto narrow = window(rb_narrow_window);
    if (SotcStats::kFpsLog) {
        // The narrow window lies inside the wide one (both power-of-two aligned around the request,
        // clipped to the same arena): count what the wide one would also have taken.
        const auto wide = window(WindowSize);
        const VAddr narrow_end =
            std::min<VAddr>(Common::AlignUp(narrow.second, BYTES_PER_PAGE), wide.second);
        const u64 kept =
            memory_tracker->CountGpuModifiedPages(wide.first, narrow.first - wide.first) +
            memory_tracker->CountGpuModifiedPages(narrow_end, wide.second - narrow_end);
        SotcStats::rb_narrow_kept_bytes.fetch_add(kept * BYTES_PER_PAGE, std::memory_order_relaxed);
    }
    return narrow;
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    const auto flush_request = [this, device_addr, size, is_write] {
        NoteHotRead(device_addr, size);
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);
        const auto [window_start, window_end] = ReadbackWindow(arena, device_addr, size);
        Core::MemoryManager::NoteHostWrite(device_addr, size, 6, window_start);
        fault_addr = device_addr;
        fault_size = size;
        DownloadMemory(arena, window_start, window_end - window_start);
        fault_size = 0;
        FinishGuestFault(device_addr, size, is_write);
    };
    if (assume_locks) {
        flush_request();
        return;
    }
    // sotc: on by default (SOTC_RB_ASYNC=0 turns it off). Radeon RX 6650 XT, sanctuary, three
    // runs each: 25.3-25.8 fps instead of 23.2-23.7, frames over 40 ms 376-424 instead of
    // 566-594, readback waits 100 ms/s instead of 290 ms/s.
    static const bool async = [] {
        const char* v = std::getenv("SOTC_RB_ASYNC");
        return !v || v[0] != '0';
    }();
    if (!async) {
        liverpool->SendCommand<true>(std::move(flush_request));
        return;
    }
    // The guest thread waits for the GPU, not the command processor.
    // sotc: BbToggle::ReadbackIntervals (default on): when the pages asked for still hold GPU bytes
    // after Finish (written since the copy), up to MaxRounds - 1 narrow rounds (only those pages,
    // no hot readbacks) go before the synchronous readback that always makes progress.
    const bool fine = !BbToggle::Disabled(BbToggle::ReadbackIntervals);
    constexpr int MaxRounds = 3;
    for (int round = 0;; ++round) {
        AsyncReadback readback{.device_addr = device_addr,
                               .size = size,
                               .is_write = is_write,
                               .narrow = round > 0};
        liverpool->SendCommand<true>([this, &readback] { BeginAsyncReadback(readback); });
        if (!readback.waiting) {
            if (fine && SotcStats::kFpsLog && round > 0) {
                SotcStats::rbs_rounds[std::min(round, 2)].fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
        scheduler.WaitSubmitted(readback.tick, false);
        scheduler.GetWorkSemaphore()->Wait(readback.tick);
        liverpool->SendCommand<true>([this, &readback] { FinishAsyncReadback(readback); });
        if (!fine || !readback.again) {
            if (fine && SotcStats::kFpsLog) {
                SotcStats::rbs_rounds[std::min(round, 2)].fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
        if (round + 1 >= MaxRounds) {
            // The GPU keeps writing the asked pages: the old synchronous readback.
            SotcStats::rbs_rounds[3].fetch_add(SotcStats::kFpsLog ? 1 : 0,
                                               std::memory_order_relaxed);
            liverpool->SendCommand<true>(flush_request);
            return;
        }
    }
}

void BufferCache::FinishGuestFault(VAddr addr, u64 size, bool is_write) {
    if (is_write) {
        MarkWrittenByGuest(addr, size);
    } else {
        DropUnbackedGpuPages(addr, size);
    }
}

void BufferCache::DropUnbackedGpuPages(VAddr addr, u64 size) {
    // sotc: a guest read fault's readback is done and its pages are still marked GPU-modified
    // although no GPU-written byte backs them (the range left the set, the page bit stayed). Left
    // set, the page stays read-protected and the load faults again forever: on a 4-thread run a
    // job worker sat on its first load and no frame came (MMadmer fork, 23 Oct). The guest then
    // reads its own memory. Pages that still hold GPU data (written since) stay protected, and a
    // readback in flight over them unprotects them itself. MarkWrittenByGuest does the same for
    // write faults.
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_READ_FAULT_DROP");
        return !v || v[0] != '0';
    }();
    if (!enabled) {
        return;
    }
    const VAddr lo = Common::AlignDown(addr, VAddr{4096});
    const VAddr hi = Common::AlignUp(addr + size, VAddr{4096});
    if (!memory_tracker->IsRegionGpuModified(lo, hi - lo) || FindInflightReadback(lo, hi - lo)) {
        return;
    }
    {
        std::shared_lock gpu_modified_lock{gpu_modified_mutex};
        if (gpu_modified_ranges.Intersects(lo, hi - lo)) {
            return;
        }
    }
    memory_tracker->UnmarkRegionAsGpuModified(lo, hi - lo, false);
    if (SotcStats::read_fault_stale_gpu.fetch_add(1, std::memory_order_relaxed) < 8) {
        std::fprintf(stderr, "SOTC read fault over a stale GPU page at %#llx: unprotected\n",
                     static_cast<unsigned long long>(addr));
    }
}

void BufferCache::MarkWrittenByGuest(VAddr addr, u64 size) {
    // sotc: a page both CPU- and GPU-modified loses its write watcher but keeps its read
    // watcher: "write-only" protection, an assertion that ended the game (opt4, ~1 in 12 Shrine
    // runs). After a write fault's readback the requested pages can still be GPU-modified: GPU
    // data recorded since (left protected: the write faults again and the next readback brings
    // it) or a page bit no GPU-written byte backs any more (cleared, or the write would fault
    // forever).
    if (memory_tracker->IsRegionGpuModified(addr, size)) {
        const VAddr lo = Common::AlignDown(addr, VAddr{4096});
        const VAddr hi = Common::AlignUp(addr + size, VAddr{4096});
        if (gpu_modified_ranges.Intersects(lo, hi - lo)) {
            SotcStats::write_fault_gpu_pending.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        SotcStats::write_fault_stale_gpu.fetch_add(1, std::memory_order_relaxed);
        memory_tracker->UnmarkRegionAsGpuModified(lo, hi - lo, false);
    }
    memory_tracker->MarkRegionAsCpuModified(addr, size);
}

void BufferCache::BeginAsyncReadback(AsyncReadback& readback) {
    SotcStats::readbacks.fetch_add(1, std::memory_order_relaxed);
    if (SotcStats::kHashStats) SotcStats::hash_tl.readbacks++;
    if (!readback.narrow) {
        NoteHotRead(readback.device_addr, readback.size);
    }
    const u32 first_block = readback.device_addr >> block_shift;
    const u32 last_block = (readback.device_addr + readback.size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    std::pair<VAddr, VAddr> window;
    if (readback.narrow) {
        // sotc: a repeat round of ReadMemory: only the pages the guest asked for.
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        window = {std::max<VAddr>(Common::AlignDown(readback.device_addr, VAddr{4096}),
                                  arena->cpu_addr),
                  std::min<VAddr>(Common::AlignUp(readback.device_addr + readback.size, VAddr{4096}),
                                  arena_end)};
    } else {
        window = ReadbackWindow(arena, readback.device_addr, readback.size);
    }
    const auto [window_start, window_end] = window;
    readback.arena = arena;
    readback.window_start = window_start;
    readback.window_size = window_end - window_start;
    const u64 total = GatherDownloads(arena, window_start, readback.window_size, readback.pending,
                                      false, !readback.narrow);
    if (total == 0) {
        // Its bytes may belong to a readback still in flight (taken out of the GPU-modified set,
        // pages still protected): wait for that one and finish it if its thread has not yet.
        if (const AsyncReadback* other = FindInflightReadback(readback.device_addr, readback.size)) {
            readback.tick = other->tick;
            readback.waiting = true;
            readback.dependent = true;
            return;
        }
        FinishGuestFault(readback.device_addr, readback.size, readback.is_write);
        return;
    }
    // Held until FinishAsyncReadback: the pool must not hand it out again before.
    const auto download =
        staging_pool.Request(total, VideoCore::MemoryType::HostCached, 0, true);
    for (auto& item : readback.pending) {
        for (auto& copy : item.copies) {
            copy.dstOffset += download.offset;
        }
        runtime.CopyBuffer(item.arena, download.buffer, item.copies);
    }
    readback.download_buffer = download.buffer;
    readback.download_size = download.size;
    readback.download_offset = download.offset;
    readback.download_mapped = download.mapped;
    readback.tick = scheduler.CurrentTick();
    scheduler.Flush();
    readback.waiting = true;
    if (SotcStats::kFpsLog) {
        // sotc: READBACK-PLAN step 0: readbacks in flight with this one, and how much of what it
        // copies another one in flight copies too.
        const u64 count = inflight_readbacks.size() + 1;
        SotcStats::rbs_inflight_samples.fetch_add(1, std::memory_order_relaxed);
        SotcStats::rbs_inflight_sum.fetch_add(count, std::memory_order_relaxed);
        u64 seen = SotcStats::rbs_inflight_max.load(std::memory_order_relaxed);
        while (seen < count && !SotcStats::rbs_inflight_max.compare_exchange_weak(
                                   seen, count, std::memory_order_relaxed)) {
        }
        u64 held = 0, total_copy = 0;
        for (const auto& item : readback.pending) {
            for (const auto& copy : item.copies) {
                const VAddr lo = item.arena->cpu_addr + copy.srcOffset;
                u64 other_bytes = 0;
                for (const AsyncReadback* other : inflight_readbacks) {
                    for (const auto& o_item : other->pending) {
                        for (const auto& o_copy : o_item.copies) {
                            const VAddr o_lo = o_item.arena->cpu_addr + o_copy.srcOffset;
                            const VAddr a = std::max(lo, o_lo);
                            const VAddr b = std::min<VAddr>(lo + copy.size, o_lo + o_copy.size);
                            other_bytes += b > a ? b - a : 0;
                        }
                    }
                }
                held += std::min<u64>(other_bytes, copy.size);
                total_copy += copy.size;
            }
        }
        SotcStats::rbs_overlap_bytes.fetch_add(held, std::memory_order_relaxed);
        SotcStats::rbs_begin_bytes.fetch_add(total_copy, std::memory_order_relaxed);
    }
    inflight_readbacks.push_back(&readback);
    ++inflight_gen; // sotc: WriteMarkMemo
    SotcStats::readback_finish.fetch_add(1, std::memory_order_relaxed);
    if (SotcStats::kHashStats) SotcStats::hash_tl.finishes++;
}

// sotc: bytes the last DownloadMemory of this thread gathered (candidate 1 telemetry).
static thread_local u64 tl_last_download_bytes = 0;

// sotc: READBACK-PLAN step 0 (SOTC_RB_STALE_CMP=1, needs SOTC_FPS): the snapshot of a dirty
// asynchronous readback, kept so the fresh download that follows can be compared with it byte by
// byte, by the class of the byte (not stale / stale by non-GpuWrite causes / stale by GpuWrite).
struct StaleCompare {
    struct Segment {
        VAddr lo, hi;
        size_t offset; // in `bytes`
    };
    struct Interval {
        VAddr lo, hi;
        u8 cls; // 1 non-GpuWrite, 2 GpuWrite
    };
    bool active = false;
    std::vector<Segment> segments;
    std::vector<u8> bytes;
    std::vector<Interval> stale;
};
static thread_local StaleCompare tl_stale_cmp;

static void StaleCompareFresh(StaleCompare& cmp, VAddr lo, const u8* fresh, u64 size) {
    thread_local std::vector<u8> cls;
    cls.assign(size, 0);
    for (int pass = 1; pass <= 2; ++pass) { // GpuWrite (2) wins over non-GpuWrite (1)
        for (const auto& interval : cmp.stale) {
            if (interval.cls != pass) {
                continue;
            }
            const VAddr a = std::max(lo, interval.lo), b = std::min<VAddr>(lo + size, interval.hi);
            for (VAddr x = a; x < b; ++x) {
                cls[x - lo] = interval.cls;
            }
        }
    }
    u64 covered = 0;
    for (const auto& seg : cmp.segments) {
        const VAddr a = std::max(lo, seg.lo), b = std::min<VAddr>(lo + size, seg.hi);
        for (VAddr x = a; x < b; ++x) {
            const u8 c = cls[x - lo];
            SotcStats::rbs_cmp_bytes[c].fetch_add(1, std::memory_order_relaxed);
            if (cmp.bytes[seg.offset + (x - seg.lo)] != fresh[x - lo]) {
                SotcStats::rbs_cmp_changed[c].fetch_add(1, std::memory_order_relaxed);
            }
        }
        covered += b > a ? b - a : 0;
    }
    SotcStats::rbs_cmp_fresh_only.fetch_add(size - std::min<u64>(size, covered),
                                            std::memory_order_relaxed);
}

/// sotc: candidate 1 telemetry (SOTC_FPS only), at the point FinishAsyncReadback decides between
/// applying the data and downloading again: how much of what the readback copied was hit.
void BufferCache::NoteFinishedReadback(const AsyncReadback& readback) {
    u64 total = 0;
    for (const auto& item : readback.pending) {
        for (const auto& copy : item.copies) {
            total += copy.size;
        }
    }
    SotcStats::rbf_finishes.fetch_add(1, std::memory_order_relaxed);
    const bool fine_mode =
        !BbToggle::Disabled(BbToggle::ReadbackIntervals) && !readback.stale_overflow;
    if (!(fine_mode ? !readback.stale.empty() : readback.dirty)) {
        SotcStats::rbf_clean_bytes.fetch_add(total, std::memory_order_relaxed);
        return;
    }
    std::vector<std::pair<VAddr, VAddr>> dirty_ranges;
    std::vector<u8> dirty_causes;
    for (const auto& s : readback.stale) {
        dirty_ranges.emplace_back(s.lo, s.hi);
        dirty_causes.push_back(s.cause);
    }
    // Union of the dirty ranges, then its intersection with the copied bytes and the asked range.
    std::vector<std::pair<VAddr, VAddr>> merged = dirty_ranges;
    std::sort(merged.begin(), merged.end());
    size_t out = 0;
    for (const auto& range : merged) {
        if (out != 0 && range.first <= merged[out - 1].second) {
            merged[out - 1].second = std::max(merged[out - 1].second, range.second);
        } else {
            merged[out++] = range;
        }
    }
    merged.resize(out);
    const auto overlap = [&](VAddr lo, VAddr hi) {
        u64 bytes = 0;
        for (const auto& range : merged) {
            const VAddr a = std::max(lo, range.first), b = std::min(hi, range.second);
            bytes += b > a ? b - a : 0;
        }
        return bytes;
    };
    u64 hit = 0;
    for (const auto& item : readback.pending) {
        for (const auto& copy : item.copies) {
            const VAddr lo = item.arena->cpu_addr + copy.srcOffset;
            hit += overlap(lo, lo + copy.size);
        }
    }
    {
        // sotc: READBACK-PLAN step 0: causes.
        bool any_gpu = false;
        const VAddr asked_lo = readback.device_addr & ~VAddr{4095};
        const VAddr asked_hi = (readback.device_addr + readback.size + 4095) & ~VAddr{4095};
        bool asked = false;
        for (size_t i = 0; i < dirty_ranges.size(); ++i) {
            if (dirty_causes[i] == CauseGpuWrite) {
                any_gpu = true;
                asked |= dirty_ranges[i].first < asked_hi && asked_lo < dirty_ranges[i].second;
            }
        }
        if (readback.first_cause < CauseCount) {
            SotcStats::rbs_first[readback.first_cause].fetch_add(1, std::memory_order_relaxed);
        }
        (any_gpu ? SotcStats::rbs_any_gpuwrite : SotcStats::rbs_no_gpuwrite)
            .fetch_add(1, std::memory_order_relaxed);
        if (asked) {
            SotcStats::rbs_gpuwrite_asked.fetch_add(1, std::memory_order_relaxed);
        }
        for (u8 cause = 0; cause < CauseCount; ++cause) {
            std::vector<std::pair<VAddr, VAddr>> of;
            for (size_t i = 0; i < dirty_ranges.size(); ++i) {
                if (dirty_causes[i] == cause) {
                    of.push_back(dirty_ranges[i]);
                }
            }
            std::sort(of.begin(), of.end());
            size_t n = 0;
            for (const auto& range : of) {
                if (n != 0 && range.first <= of[n - 1].second) {
                    of[n - 1].second = std::max(of[n - 1].second, range.second);
                } else {
                    of[n++] = range;
                }
            }
            u64 bytes = 0;
            for (const auto& item : readback.pending) {
                for (const auto& copy : item.copies) {
                    const VAddr lo = item.arena->cpu_addr + copy.srcOffset;
                    for (size_t k = 0; k < n; ++k) {
                        const VAddr a = std::max<VAddr>(lo, of[k].first);
                        const VAddr b = std::min<VAddr>(lo + copy.size, of[k].second);
                        bytes += b > a ? b - a : 0;
                    }
                }
            }
            SotcStats::rbs_cause_bytes[cause].fetch_add(bytes, std::memory_order_relaxed);
        }
    }
    SotcStats::rbf_dirty.fetch_add(1, std::memory_order_relaxed);
    SotcStats::rbf_dirty_copy_bytes.fetch_add(total, std::memory_order_relaxed);
    SotcStats::rbf_dirty_hit_bytes.fetch_add(hit, std::memory_order_relaxed);
    SotcStats::rbf_dirty_window_bytes.fetch_add(readback.window_size, std::memory_order_relaxed);
    if (overlap(readback.device_addr, readback.device_addr + readback.size) != 0) {
        SotcStats::rbf_dirty_asked.fetch_add(1, std::memory_order_relaxed);
    }
    if (hit < total) {
        SotcStats::rbf_dirty_partial.fetch_add(1, std::memory_order_relaxed);
    }
}

void BufferCache::MarkInflightReadbacksDirty(VAddr addr, u64 size, u8 cause) {
    // sotc: the command processor (a GPU write, its own write or an applied readback after
    // which the guest may write) changed these bytes: asynchronous readbacks of them in flight
    // hold stale data (FinishAsyncReadback reads them back again).
    // sotc: BbToggle::ReadbackIntervals. `dirty` is the old whole-region test (the old path reads
    // it). The interval is recorded when it touches a region or one of the GPU page runs of an item
    // (those run up to a page beyond the region): the new path applies what no interval touched.
    if (size == 0) {
        return;
    }
    for (AsyncReadback* readback : inflight_readbacks) {
        bool region_hit = false, hit = false;
        for (const auto& item : readback->pending) {
            const bool in_region = addr < item.addr + item.size && item.addr < addr + size;
            region_hit |= in_region;
            hit |= in_region;
            if (!hit) {
                for (const auto& [run_addr, run_size] : item.gpu_ranges) {
                    hit |= addr < run_addr + run_size && run_addr < addr + size;
                }
            }
        }
        if (region_hit && !readback->dirty) {
            readback->first_cause = cause;
        }
        readback->dirty |= region_hit;
        if (hit) {
            RecordStale(*readback, addr, addr + size, cause);
        }
    }
}

void BufferCache::MarkInflightRegionDirty(VAddr addr, u64 size) {
    for (AsyncReadback* readback : inflight_readbacks) {
        for (const auto& item : readback->pending) {
            if (addr < item.addr + item.size && item.addr < addr + size) {
                readback->dirty = true;
                break;
            }
        }
    }
}

void BufferCache::RecordStale(AsyncReadback& readback, VAddr lo, VAddr hi, u8 cause) {
    if (readback.stale_overflow) {
        return;
    }
    for (const auto& s : readback.stale) {
        if (s.cause == cause && s.lo <= lo && hi <= s.hi) {
            return; // the same binding written again and again
        }
    }
    if (!readback.stale.empty()) {
        auto& last = readback.stale.back();
        if (last.cause == cause && lo <= last.hi && last.lo <= hi) {
            last.lo = std::min(last.lo, lo);
            last.hi = std::max(last.hi, hi);
            return;
        }
    }
    if (readback.stale.size() >= MaxStaleRanges) {
        // Merge per cause; still too many: give up (old path: the whole readback is dirty).
        boost::container::small_vector<StaleRange, 16> merged;
        for (u8 c = 0; c < CauseCount; ++c) {
            boost::container::small_vector<RbIntervals::Interval, 16> of;
            for (const auto& s : readback.stale) {
                if (s.cause == c) {
                    of.push_back({s.lo, s.hi});
                }
            }
            RbIntervals::Normalize(of);
            for (const auto& i : of) {
                merged.push_back({i.lo, i.hi, c});
            }
        }
        readback.stale = std::move(merged);
        if (readback.stale.size() >= MaxStaleRanges) {
            readback.stale_overflow = true;
            readback.dirty = true;
            return;
        }
    }
    readback.stale.push_back({lo, hi, cause});
}

BufferCache::AsyncReadback* BufferCache::FindInflightReadback(VAddr addr, u64 size) {
    for (AsyncReadback* readback : inflight_readbacks) {
        for (const auto& item : readback->pending) {
            for (const auto& copy : item.copies) {
                const VAddr start = item.arena->cpu_addr + copy.srcOffset;
                if (addr < start + copy.size && start < addr + size) {
                    return readback;
                }
            }
        }
    }
    return nullptr;
}

void BufferCache::FinishAsyncReadback(AsyncReadback& readback) {
    // sotc: BbToggle::ReadbackIntervals: whether the pages the guest asked for still hold GPU bytes
    // (ReadMemory then repeats a narrow round).
    const auto note_again = [&] {
        if (BbToggle::Disabled(BbToggle::ReadbackIntervals)) {
            return;
        }
        const VAddr lo = Common::AlignDown(readback.device_addr, VAddr{4096});
        const VAddr hi = Common::AlignUp(readback.device_addr + readback.size, VAddr{4096});
        readback.again = gpu_modified_ranges.Intersects(lo, hi - lo);
    };
    if (readback.dependent) {
        // Finish the readbacks this one waited for (owners still blocked keep them alive).
        while (AsyncReadback* other = FindInflightReadback(readback.device_addr, readback.size)) {
            FinishAsyncReadback(*other);
        }
        FinishGuestFault(readback.device_addr, readback.size, readback.is_write);
        note_again();
        return;
    }
    if (readback.finished) {
        FinishGuestFault(readback.device_addr, readback.size, readback.is_write);
        note_again();
        return;
    }
    readback.finished = true;
    std::erase(inflight_readbacks, &readback);
    // sotc: WriteMarkMemo: no inflight_gen bump here: a readback leaving the list leaves the marks
    // already made on the others in place (only a new one needs the marks again).
    if (SotcStats::kFpsLog) {
        NoteFinishedReadback(readback);
    }
    const Vulkan::StagingBufferRef download{
        .buffer = readback.download_buffer,
        .offset = readback.download_offset,
        .size = readback.download_size,
        .mapped = const_cast<u8*>(readback.download_mapped),
        .type = VideoCore::MemoryType::HostCached,
    };
    // sotc: BbToggle::ReadbackIntervals (default on): a readback whose bytes were touched in flight
    // applies the rest (FinishAsyncReadbackFine) instead of downloading its window again.
    const bool use_fine =
        !BbToggle::Disabled(BbToggle::ReadbackIntervals) && !readback.stale_overflow;
    const bool stale_any = use_fine ? !readback.stale.empty()
                                    : (readback.dirty || readback.stale_overflow);
    if (stale_any && use_fine) {
        FinishAsyncReadbackFine(readback, download);
    } else if (stale_any) {
        // A GPU write recorded after the copies (or another readback applied first and the
        // guest may have written since): the data may be stale. The ranges never left the
        // GPU-modified set; read the window back the usual way.
        static const bool stale_cmp = [] {
            const char* v = std::getenv("SOTC_RB_STALE_CMP");
            return SotcStats::kFpsLog && v && v[0] == '1';
        }();
        if (stale_cmp) {
            download.buffer->Invalidate(download.offset, download.size);
            auto& cmp = tl_stale_cmp;
            cmp.segments.clear();
            cmp.bytes.clear();
            cmp.stale.clear();
            for (const auto& item : readback.pending) {
                for (const auto& copy : item.copies) {
                    const VAddr lo = item.arena->cpu_addr + copy.srcOffset;
                    cmp.segments.push_back({lo, lo + copy.size, cmp.bytes.size()});
                    const u8* src = download.mapped + (copy.dstOffset - download.offset);
                    cmp.bytes.insert(cmp.bytes.end(), src, src + copy.size);
                }
            }
            for (const auto& s : readback.stale) {
                cmp.stale.push_back({s.lo, s.hi, static_cast<u8>(s.cause == CauseGpuWrite ? 2 : 1)});
            }
            cmp.active = true;
        }
        staging_pool.FreeDeferred(download);
        if (SotcStats::kFpsLog) {
            // sotc: candidate 1 telemetry: what the second download of a dirty readback costs.
            const auto t0 = std::chrono::steady_clock::now();
            tl_last_download_bytes = 0;
            DownloadMemory(readback.arena, readback.window_start, readback.window_size);
            SotcStats::rbf_redownload_calls.fetch_add(1, std::memory_order_relaxed);
            SotcStats::rbf_redownload_bytes.fetch_add(tl_last_download_bytes,
                                                      std::memory_order_relaxed);
            SotcStats::rbf_redownload_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count(),
                std::memory_order_relaxed);
            tl_stale_cmp.active = false;
        } else {
            DownloadMemory(readback.arena, readback.window_start, readback.window_size);
        }
    } else {
        download.buffer->Invalidate(download.offset, download.size);
        // sotc: the ranges leave the GPU-modified set only once the backing holds their data:
        // the command processor's clean reads (ReadClean) would read the stale backing before.
        ApplyDownloads(readback.pending, download.mapped, download.offset);
        SubtractDownloaded(readback.pending);
        staging_pool.FreeDeferred(download);
    }
    FinishGuestFault(readback.device_addr, readback.size, readback.is_write);
    note_again();
}

// sotc: BbToggle::ReadbackIntervals, SOTC_RB_VERIFY=1 state: the GPU-modified set over the GPU page
// runs of every item before the apply (one byte per byte of each run), to check what the apply and
// the page unmarking changed.
struct BufferCache::FineVerify {
    bool active = false;
    std::vector<std::vector<u8>> before; // per item run, in order
};

void BufferCache::FinishAsyncReadbackFine(AsyncReadback& readback,
                                          const Vulkan::StagingBufferRef& download) {
    // sotc: what no interval touched is applied and leaves the GPU-modified set; the touched bytes
    // stay in it with their pages protected (a guest access to them faults and reads them back).
    // Invariant of the old path kept: a byte leaves the set (and its page loses its protection)
    // only once the backing holds the GPU's data for it. Every addition to the set is announced by
    // AddGpuModified to every readback in flight (MarkInflightReadbacksDirty), so the copies the
    // intervals miss hold the state of the GPU at this point of the queue.
    const auto t0 = SotcStats::kFpsLog ? std::chrono::steady_clock::now()
                                       : std::chrono::steady_clock::time_point{};
    boost::container::small_vector<RbIntervals::Interval, 16> merged;
    for (const auto& s : readback.stale) {
        merged.push_back({s.lo, s.hi});
    }
    RbIntervals::Normalize(merged);
    boost::container::small_vector<ReadbackItem, 16> fine_items;
    u64 applied_bytes = 0;
    for (const auto& item : readback.pending) {
        ReadbackItem fine{item.arena, item.addr, item.size, {}, {}};
        fine.hot = item.hot;
        const VAddr arena_base = item.arena->cpu_addr;
        for (const auto& copy : item.copies) {
            RbIntervals::ForEachCopyOutside(
                arena_base + copy.srcOffset, copy.dstOffset, copy.size, merged,
                [&](u64 lo, u64 dst, u64 size) {
                    fine.copies.push_back(vk::BufferCopy{
                        .srcOffset = lo - arena_base, .dstOffset = dst, .size = size});
                    applied_bytes += size;
                });
        }
        for (const auto& [run_addr, run_size] : item.gpu_ranges) {
            RbIntervals::ForEachOutside(run_addr, run_addr + run_size, merged,
                                        [&](u64 lo, u64 hi) { fine.gpu_ranges.emplace_back(lo, hi - lo); });
        }
        fine_items.push_back(std::move(fine));
    }
    download.buffer->Invalidate(download.offset, download.size);
    static const bool verify = [] {
        const char* v = std::getenv("SOTC_RB_VERIFY");
        return v && v[0] == '1';
    }();
    FineVerify state;
    if (verify) {
        VerifyFineBefore(readback, download, merged, state);
    }
    if (applied_bytes != 0) {
        ApplyDownloads(fine_items, download.mapped, download.offset, true);
    } else {
        for (const auto& item : fine_items) {
            NoteApplied(item);
        }
    }
    SubtractDownloaded(fine_items);
    UnmarkPagesWithoutGpuBytes(readback.pending);
    if (verify) {
        VerifyFineAfter(readback, fine_items, state);
    }
    staging_pool.FreeDeferred(download);
    if (SotcStats::kFpsLog) {
        SotcStats::rbs_fine_finishes.fetch_add(1, std::memory_order_relaxed);
        SotcStats::rbs_fine_applied_bytes.fetch_add(applied_bytes, std::memory_order_relaxed);
        SotcStats::rbs_fine_ns.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0)
                .count(),
            std::memory_order_relaxed);
    }
}

// sotc: SOTC_RB_VERIFY=1 (READBACK-PLAN.md section 5). Diagnostic only: blocks the draw thread.
// Copies every original copy again with the old synchronous route (CopyBuffer + FinishHostRead: the
// arena as it is at this point of the queue) and compares it with the snapshot:
//  (a) bytes about to be applied: snapshot == fresh, always (MISMATCH_APPLIED otherwise);
//  (b) bytes skipped for Applied/Drop/CpuReplace: snapshot == fresh, or the byte is GPU-modified
//      now (MISMATCH_SKIPPED otherwise);
//  (c) bytes skipped for GpuWrite: informational, how many really changed.
// VerifyFineAfter: (d) the GPU-modified set over the item's page runs equals what it was minus the
// bytes subtracted, byte by byte (MISMATCH_SET), and no page holding a GPU byte lost its bit
// (MISMATCH_PAGE).
void BufferCache::VerifyFineBefore(const AsyncReadback& readback,
                                   const Vulkan::StagingBufferRef& download,
                                   std::span<const RbIntervals::Interval> merged,
                                   FineVerify& state) {
    using RbIntervals::Interval;
    state.active = true;
    const std::vector<Interval> merged_vec(merged.begin(), merged.end());
    std::vector<Interval> gpu_write;
    for (const auto& s : readback.stale) {
        if (s.cause == CauseGpuWrite) {
            gpu_write.push_back({s.lo, s.hi});
        }
    }
    RbIntervals::Normalize(gpu_write);
    // The set over the page runs, before anything changes.
    for (const auto& item : readback.pending) {
        for (const auto& [run_addr, run_size] : item.gpu_ranges) {
            auto& bits = state.before.emplace_back(run_size, u8{0});
            gpu_modified_ranges.ForEachInRange(run_addr, run_size, [&](VAddr lo, VAddr hi) {
                std::fill(bits.begin() + (lo - run_addr), bits.begin() + (hi - run_addr), u8{1});
            });
        }
    }
    u64 total = 0;
    for (const auto& item : readback.pending) {
        for (const auto& copy : item.copies) {
            total += Common::AlignUp(copy.size, 64ULL);
        }
    }
    if (total == 0) {
        return;
    }
    const auto fresh = staging_pool.Request(total, VideoCore::MemoryType::HostCached);
    u64 running = 0;
    for (const auto& item : readback.pending) {
        boost::container::small_vector<vk::BufferCopy, 8> copies;
        for (const auto& copy : item.copies) {
            copies.push_back(vk::BufferCopy{.srcOffset = copy.srcOffset,
                                            .dstOffset = fresh.offset + running,
                                            .size = copy.size});
            running += Common::AlignUp(copy.size, 64ULL);
        }
        runtime.CopyBuffer(item.arena, fresh.buffer, copies);
    }
    scheduler.FinishHostRead();
    fresh.buffer->Invalidate(fresh.offset, fresh.size);
    u64 applied = 0, applied_bad = 0, skipped = 0, skipped_bad = 0, gw_total = 0, gw_changed = 0;
    u64 skipped_in_set = 0, skipped_backing = 0, skipped_cpu = 0;
    running = 0;
    for (const auto& item : readback.pending) {
        for (const auto& copy : item.copies) {
            const u8* snap = download.mapped + (copy.dstOffset - download.offset);
            const u8* now = fresh.mapped + running;
            running += Common::AlignUp(copy.size, 64ULL);
            const VAddr lo = item.arena->cpu_addr + copy.srcOffset;
            RbIntervals::ForEachOutside(lo, lo + copy.size, merged_vec, [&](u64 a, u64 b) {
                applied += b - a;
                for (u64 x = a; x < b; ++x) {
                    if (snap[x - lo] != now[x - lo]) {
                        if (applied_bad++ < 20) {
                            std::fprintf(stderr,
                                         "SOTC RB VERIFY MISMATCH_APPLIED at %#llx (copy %#llx+%#llx)\n",
                                         static_cast<unsigned long long>(x),
                                         static_cast<unsigned long long>(lo),
                                         static_cast<unsigned long long>(copy.size));
                        }
                    }
                }
            });
            RbIntervals::ForEachInside(lo, lo + copy.size, gpu_write, [&](u64 a, u64 b) {
                gw_total += b - a;
                for (u64 x = a; x < b; ++x) {
                    gw_changed += snap[x - lo] != now[x - lo];
                }
            });
            RbIntervals::ForEachInside(lo, lo + copy.size, merged_vec, [&](u64 a, u64 b) {
                RbIntervals::ForEachOutside(a, b, gpu_write, [&](u64 c, u64 d) {
                    skipped += d - c;
                    // A byte that differs is explained when the GPU set holds it (it will be read
                    // back), when guest memory already equals the arena (an applied readback or a
                    // CPU upload brought the guest's newer value to both), or when its page is
                    // CPU-modified (the upload is pending). Anything else lost a GPU write.
                    std::vector<u8> back(d - c);
                    const bool have_back = memory->TryReadBacking(c, back.data(), d - c);
                    VAddr cpu_page = ~VAddr{0};
                    bool cpu_page_modified = false;
                    for (u64 x = c; x < d; ++x) {
                        if (snap[x - lo] == now[x - lo]) {
                            continue;
                        }
                        if (gpu_modified_ranges.Intersects(x, 1)) {
                            ++skipped_in_set;
                        } else if (have_back && back[x - c] == now[x - lo]) {
                            ++skipped_backing;
                        } else {
                            if ((x >> 12) != cpu_page) {
                                cpu_page = x >> 12;
                                cpu_page_modified = memory_tracker->IsRegionCpuModified(x, 1);
                            }
                            if (cpu_page_modified) {
                                ++skipped_cpu;
                            } else if (skipped_bad++ < 20) {
                                u8 cause = 0xFF;
                                for (const auto& s : readback.stale) {
                                    if (s.lo <= x && x < s.hi && s.cause != CauseGpuWrite) {
                                        cause = s.cause;
                                        break;
                                    }
                                }
                                std::fprintf(stderr,
                                             "SOTC RB VERIFY MISMATCH_SKIPPED at %#llx (copy %#llx+%#llx, "
                                             "cause %d, snapshot %02x, arena %02x, guest %02x)\n",
                                             static_cast<unsigned long long>(x),
                                             static_cast<unsigned long long>(lo),
                                             static_cast<unsigned long long>(copy.size), int(cause),
                                             snap[x - lo], now[x - lo], have_back ? back[x - c] : 0);
                            }
                        }
                    }
                });
            });
        }
    }
    const u64 before_total = SotcStats::rbv_applied.fetch_add(applied, std::memory_order_relaxed);
    SotcStats::rbv_applied_bad.fetch_add(applied_bad, std::memory_order_relaxed);
    SotcStats::rbv_skipped.fetch_add(skipped, std::memory_order_relaxed);
    SotcStats::rbv_skipped_bad.fetch_add(skipped_bad, std::memory_order_relaxed);
    SotcStats::rbv_skipped_in_set.fetch_add(skipped_in_set, std::memory_order_relaxed);
    SotcStats::rbv_skipped_backing.fetch_add(skipped_backing, std::memory_order_relaxed);
    SotcStats::rbv_skipped_cpu.fetch_add(skipped_cpu, std::memory_order_relaxed);
    SotcStats::rbv_gpuwrite.fetch_add(gw_total, std::memory_order_relaxed);
    SotcStats::rbv_gpuwrite_changed.fetch_add(gw_changed, std::memory_order_relaxed);
    SotcStats::rbv_calls.fetch_add(1, std::memory_order_relaxed);
    if ((before_total >> 22) != ((before_total + applied) >> 22)) {
        std::fprintf(
            stderr,
            "SOTC RB VERIFY: applied %llu B (bad %llu), skipped %llu B (bad %llu), GpuWrite stale %llu B "
            "(%llu changed), set bad %llu, page bad %llu\n",
            static_cast<unsigned long long>(SotcStats::rbv_applied.load()),
            static_cast<unsigned long long>(SotcStats::rbv_applied_bad.load()),
            static_cast<unsigned long long>(SotcStats::rbv_skipped.load()),
            static_cast<unsigned long long>(SotcStats::rbv_skipped_bad.load()),
            static_cast<unsigned long long>(SotcStats::rbv_gpuwrite.load()),
            static_cast<unsigned long long>(SotcStats::rbv_gpuwrite_changed.load()),
            static_cast<unsigned long long>(SotcStats::rbv_set_bad.load()),
            static_cast<unsigned long long>(SotcStats::rbv_page_bad.load()));
    }
}

void BufferCache::VerifyFineAfter(const AsyncReadback& readback,
                                  std::span<const ReadbackItem> fine_items, FineVerify& state) {
    if (!state.active) {
        return;
    }
    size_t run_index = 0;
    u64 set_bad = 0, page_bad = 0;
    for (size_t i = 0; i < readback.pending.size(); ++i) {
        for (const auto& [run_addr, run_size] : readback.pending[i].gpu_ranges) {
            std::vector<u8> expected = state.before[run_index++];
            for (const auto& [sub_addr, sub_size] : fine_items[i].gpu_ranges) {
                const VAddr a = std::max<VAddr>(sub_addr, run_addr);
                const VAddr b = std::min<VAddr>(sub_addr + sub_size, run_addr + run_size);
                if (b > a) {
                    std::fill(expected.begin() + (a - run_addr), expected.begin() + (b - run_addr),
                              u8{0});
                }
            }
            std::vector<u8> actual(run_size, u8{0});
            gpu_modified_ranges.ForEachInRange(run_addr, run_size, [&](VAddr lo, VAddr hi) {
                std::fill(actual.begin() + (lo - run_addr), actual.begin() + (hi - run_addr), u8{1});
            });
            for (u64 x = 0; x < run_size; ++x) {
                if (expected[x] != actual[x] && set_bad++ < 20) {
                    std::fprintf(stderr, "SOTC RB VERIFY MISMATCH_SET at %#llx (expected %d, now %d)\n",
                                 static_cast<unsigned long long>(run_addr + x), expected[x], actual[x]);
                }
            }
            for (u64 page = 0; page < run_size; page += 4096) {
                bool has_bytes = false;
                for (u64 x = page; x < std::min<u64>(page + 4096, run_size) && !has_bytes; ++x) {
                    has_bytes = actual[x] != 0;
                }
                if (has_bytes && !memory_tracker->IsRegionGpuModified(run_addr + page, 4096) &&
                    page_bad++ < 20) {
                    std::fprintf(stderr, "SOTC RB VERIFY MISMATCH_PAGE at %#llx (GPU bytes, no page bit)\n",
                                 static_cast<unsigned long long>(run_addr + page));
                }
            }
        }
    }
    SotcStats::rbv_set_bad.fetch_add(set_bad, std::memory_order_relaxed);
    SotcStats::rbv_page_bad.fetch_add(page_bad, std::memory_order_relaxed);
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    SotcStats::readbacks.fetch_add(1, std::memory_order_relaxed);
    if (SotcStats::kHashStats) SotcStats::hash_tl.readbacks++;
    const SotcStats::ScopedTimer readback_timer{SotcStats::readback_ns};
    const SotcStats::FrameTimer frame_timer{SotcStats::FrameReadback};
    const auto stall_t0 = SotcStats::kStallLog ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
    boost::container::small_vector<ReadbackItem, 16> pending;
    const u64 total_size_bytes = GatherDownloads(arena, device_addr, size, pending);
    tl_last_download_bytes = total_size_bytes;
    SCOPE_EXIT {
        if (SotcStats::kStallLog) {
            SotcStats::StallAgg(SotcStats::StallReadback, SotcStats::current_program.load(),
                                SotcStats::StallMsSince(stall_t0), total_size_bytes / 1048576.0);
            SotcStats::StallNote(SotcStats::StallMsSince(stall_t0),
                                 "readback %.1f ms: %.2f MB of window %#llx + %.2f MB, program %#llx",
                                 SotcStats::StallMsSince(stall_t0), total_size_bytes / 1048576.0,
                                 static_cast<unsigned long long>(device_addr), size / 1048576.0,
                                 static_cast<unsigned long long>(SotcStats::current_program.load()));
        }
    };
    if (total_size_bytes == 0) {
        return;
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    // Was the data written by work already submitted, or by the command buffer being built?
    if (current_writes_tick != scheduler.CurrentTick()) {
        current_writes.Clear();
        current_writes_tick = scheduler.CurrentTick();
    }
    bool written_by_current = false;
    for (const auto& item : pending) {
        for (const auto& copy : item.copies) {
            written_by_current |=
                current_writes.Intersects(item.arena->cpu_addr + copy.srcOffset, copy.size);
        }
    }
    for (auto& item : pending) {
        for (auto& copy : item.copies) {
            copy.dstOffset += download.offset;
        }
    }
    // Readback ahead (idea from Pink-shadPS4, luizgustavs): when the data was written by work
    // already submitted, copy it in a command buffer of its own instead of submitting and waiting
    // for everything recorded so far. SOTC_RB_AHEAD=0/1 forces it; by default on NVIDIA and AMD
    // (Pink-shadPS4 advises turning it off on AMD; measured right on a Radeon RX 6650 XT).
    static const bool ahead_enabled = [this] {
        const char* v = std::getenv("SOTC_RB_AHEAD");
        constexpr u32 VendorNvidia = 0x10DE;
        constexpr u32 VendorAmd = 0x1002;
        return v ? std::atoi(v) != 0
                 : instance.GetVendorID() == VendorNvidia || instance.GetVendorID() == VendorAmd;
    }();
    if (ahead_enabled && !written_by_current && pending_binds.empty()) {
        const auto cmdbuf = scheduler.BeginAhead();
        const vk::MemoryBarrier2 pre_barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &pre_barrier,
        });
        for (const auto& item : pending) {
            cmdbuf.copyBuffer(item.arena->Handle(), download.buffer->Handle(), item.copies);
        }
        const vk::MemoryBarrier2 host_barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &host_barrier,
        });
        scheduler.SubmitAheadAndWait();
        SotcStats::readback_ahead.fetch_add(1, std::memory_order_relaxed);
    } else {
        SotcStats::readback_finish.fetch_add(1, std::memory_order_relaxed);
        if (SotcStats::kHashStats) SotcStats::hash_tl.finishes++;
        // Did the guest ask for bytes the command buffer being built writes, or only a neighbour
        // in the window (or a hot region) did?
        if (fault_size != 0 && written_by_current &&
            current_writes.Intersects(fault_addr, fault_size)) {
            SotcStats::readback_finish_asked.fetch_add(1, std::memory_order_relaxed);
        }
        for (const auto& item : pending) {
            runtime.CopyBuffer(item.arena, download.buffer, item.copies);
        }
        scheduler.FinishHostRead();
    }

    download.buffer->Invalidate(download.offset, download.size);
    if (tl_stale_cmp.active) {
        SotcStats::rbs_cmp_calls.fetch_add(1, std::memory_order_relaxed);
        for (const auto& item : pending) {
            for (const auto& copy : item.copies) {
                StaleCompareFresh(tl_stale_cmp, item.arena->cpu_addr + copy.srcOffset,
                                  download.mapped + (copy.dstOffset - download.offset), copy.size);
            }
        }
    }
    // sotc: GPU output of dispatches on stale descriptors must not overwrite the allocator
    // metadata of the game's BPE heap (bpe_heap_guard.cpp, from Pink-shadPS4; the loader finds
    // the heap, SOTC_HEAP_GUARD=0 turns it off).
    // sotc: ApplyDownloads rebuilds every heap's tree per batch; root/count/pending do not
    // identify all internal changes. Headers are followed through the physical chain per write.
    ApplyDownloads(pending, download.mapped, download.offset);
    SubtractDownloaded(pending); // sotc: after the backing holds the data (see FinishAsyncReadback)
}

void BufferCache::NoteApplied(const ReadbackItem& item) {
    // sotc: the old whole-region test keeps feeding `dirty` (readbacks of the old path in flight);
    // the intervals are what was applied: the item's GPU page runs.
    MarkInflightRegionDirty(item.addr, item.size);
    for (const auto& [run_addr, run_size] : item.gpu_ranges) {
        MarkInflightReadbacksDirty(run_addr, run_size, CauseApplied);
    }
}

void BufferCache::UnmarkPagesWithoutGpuBytes(std::span<const ReadbackItem> items) {
    constexpr VAddr PageSize = 4096;
    for (const auto& item : items) {
        for (const auto& [run_addr, run_size] : item.gpu_ranges) {
            VAddr cursor = run_addr;
            const VAddr run_end = run_addr + run_size;
            gpu_modified_ranges.ForEachInRange(run_addr, run_size, [&](VAddr lo, VAddr hi) {
                const VAddr page_lo = Common::AlignDown(lo, PageSize);
                if (page_lo > cursor) {
                    memory_tracker->UnmarkRegionAsGpuModified(cursor, page_lo - cursor, false);
                }
                cursor = std::max(cursor, Common::AlignUp(hi, PageSize));
            });
            if (cursor < run_end) {
                memory_tracker->UnmarkRegionAsGpuModified(cursor, run_end - cursor, false);
            }
        }
    }
}

void BufferCache::SubtractDownloaded(std::span<const ReadbackItem> items) {
    std::unique_lock gpu_modified_lock{gpu_modified_mutex};
    for (const auto& item : items) {
        for (const auto& [address, range_size] : item.gpu_ranges) {
            gpu_modified_ranges.Subtract(address, range_size);
        }
    }
}

u64 BufferCache::GatherDownloads(const Buffer* arena, VAddr device_addr, u64 size,
                                 boost::container::small_vector<ReadbackItem, 16>& pending,
                                 bool subtract, bool hot) {
    // A readback costs a full CPU/GPU sync. Games tend to read back the same few regions every
    // frame, one after another, so while the GPU is idle anyway also bring along the other
    // regions read back recently ("hot" readbacks) that the GPU has modified since. The reads
    // that follow then find their data already on the CPU instead of each needing a sync.
    // SOTC_RB_HOT=0: off, 1 (default): same arena and small regions only, 2: any existing arena,
    // any size. sotc: off from 22 Oct, on again (mode 1) on 23 Oct with the late check in
    // ApplyDownloads (pages the CPU wrote since the gather are skipped); the user's recipe (new
    // area, reload the save, 3 times) stayed clean with it. Shrine idle 34.6 -> 43.3 FPS.
    // Before that, without the check, those speculative copies landed on memory the game may have
    // reused since: rainbow, striped garbage on the Shrine's stones after loading a save again, and
    // heap free lists corrupted (0x49500, the game closing "out of nowhere", once after 23 min idle
    // in the title demo); with them off the user's recipe (a new area, then reload the save, 3
    // times) stayed clean. Attract demo, full settings: 54.8 / 36.4 FPS off against ~57 / 40 with
    // mode 2. Keeping the hot data aside until the guest reads it (no speculative writes) cost more
    // than it saved (42.7 / 23.5): copying it on the command processor thread.
    static const int hot_mode = [] {
        const char* v = std::getenv("SOTC_RB_HOT");
        return v ? std::atoi(v) : 1;
    }();
    const bool hot_enabled = hot_mode != 0 && hot;
    const auto now = std::chrono::steady_clock::now();
    struct Region {
        const Buffer* arena;
        VAddr addr;
        u64 size;
    };
    boost::container::small_vector<Region, 16> regions;
    regions.push_back({arena, device_addr, size});
    if (hot_enabled) {
        bool known = false;
        for (auto& hot : hot_readbacks) {
            if (hot.addr == device_addr && hot.size == size) {
                hot.last_request = now;
                known = true;
                continue;
            }
            if (now - hot.last_request > HotReadbackLifetime) {
                continue;
            }
            // Stay inside the arena of this readback: never create or migrate arenas here.
            const u64 first_page = hot.addr >> ARENA_PAGE_BITS;
            const u64 last_page = (hot.addr + hot.size - 1) >> ARENA_PAGE_BITS;
            const Buffer* hot_arena = address_space[first_page];
            if (!hot_arena || address_space[last_page] != hot_arena ||
                (hot_mode == 1 && hot_arena != arena)) {
                continue;
            }
            regions.push_back({hot_arena, hot.addr, hot.size});
        }
        // sotc: SOTC_RB_NARROW: the reads one wide window covered ask for several narrow ones;
        // room for up to 4 times as many.
        const size_t max_hot =
            rb_narrow_window ? MaxHotReadbacks * std::min<u64>(4, 512_KB / rb_narrow_window)
                             : MaxHotReadbacks;
        if (!known) {
            if (hot_readbacks.size() < max_hot) {
                hot_readbacks.push_back({device_addr, size, now});
            } else {
                auto oldest = std::ranges::min_element(hot_readbacks, {}, &HotReadback::last_request);
                *oldest = {device_addr, size, now};
            }
        }
        if (rb_narrow_window && SotcStats::kFpsLog) {
            SotcStats::rb_hot_entries.store(hot_readbacks.size(), std::memory_order_relaxed);
        }
    }

    u64 total_size_bytes = 0;
    for (size_t region_index = 0; region_index < regions.size(); ++region_index) {
        const Region& region = regions[region_index];
        // sotc: a hot region (brought along, not asked for) skips the pages whose CPU bit is set:
        // the guest may have written them since (repeat-fault grace leaves hot pages writable),
        // and its bytes are the right ones. Applying the GPU copy there wrote what the GPU drew
        // before the game reused the memory over new textures (rainbow garbage on the Shrine's
        // stones after a save reload, 22 Oct; SOTC_RB_HOT=0 made it go away).
        const bool hot_region = region_index > 0;
        ReadbackItem item{region.arena, region.addr, region.size, {}};
        item.hot = hot_region;
        const u64 total_before = total_size_bytes;
        const VAddr arena_base = region.arena->cpu_addr;
        memory_tracker->ForEachDownloadRange<false>(
            region.addr, region.size, [&](u64 address, u64 range_size) {
                const auto add_download = [&](VAddr start, VAddr end) {
                    // Only copy what is still bound; a released range must never be read.
                    resident_ranges.ForEachInRange(
                        start >> block_shift, ((end - 1) >> block_shift) + 1,
                        [&](const Backing& backing) {
                            const VAddr a = std::max<VAddr>(start, backing.start << block_shift);
                            const VAddr b = std::min<VAddr>(end, backing.end << block_shift);
                            if (a >= b) {
                                return;
                            }
                            item.copies.push_back(vk::BufferCopy{
                                .srcOffset = a - arena_base,
                                .dstOffset = total_size_bytes,
                                .size = b - a,
                            });
                            // Align up to avoid cache conflicts
                            constexpr u64 align = 64ULL;
                            total_size_bytes += Common::AlignUp(b - a, align);
                        });
                };
                if (!hot_region) {
                    gpu_modified_ranges.ForEachInRange(address, range_size, add_download);
                    item.gpu_ranges.emplace_back(address, range_size);
                    return;
                }
                // Page by page: runs of pages the CPU has not touched.
                VAddr run_start = address;
                const VAddr range_end = address + range_size;
                const auto flush_run = [&](VAddr end) {
                    if (end > run_start) {
                        gpu_modified_ranges.ForEachInRange(run_start, end - run_start, add_download);
                        item.gpu_ranges.emplace_back(run_start, end - run_start);
                    }
                };
                for (VAddr page = address; page < range_end;) {
                    const VAddr next = std::min(Common::AlignDown(page, 4_KB) + 4_KB, range_end);
                    if (memory_tracker->IsRegionCpuModified(page, next - page)) {
                        flush_run(page);
                        run_start = next;
                        SotcStats::rb_hot_cpu_skipped.fetch_add(next - page, std::memory_order_relaxed);
                    }
                    page = next;
                }
                flush_run(range_end);
            });
        if (item.copies.empty()) {
            if (subtract) {
                SubtractDownloaded({&item, 1}); // nothing bound to read back
            }
            continue;
        }
        // Large modified regions (render targets and the like) are left to their own readback.
        constexpr u64 MaxHotBytes = 64_KB;
        u64 item_bytes = 0;
        for (const auto& copy : item.copies) {
            item_bytes += copy.size;
        }
        if (hot_mode == 1 && !pending.empty() && item_bytes > MaxHotBytes) {
            // Not downloaded now: its ranges stay in the modified set.
            total_size_bytes = total_before;
            continue;
        }
        pending.push_back(std::move(item));
    }
    return total_size_bytes;
}

// sotc: Lote 3 (BbToggle::BpeGuardDiff). True when the guest bytes [addr, addr + size) already equal
// src, read through the backing view like the guard (never through the possibly protected guest
// address). False when any part is unmapped or unreadable: the caller then takes the old path.
static bool GuestBytesEqual(Core::MemoryManager* memory, VAddr addr, const u8* src, u64 size) {
    while (size != 0) {
        VAddr run_start = 0;
        u64 run_size = 0;
        const u8* run = memory->GetBackingRun(addr, &run_start, &run_size);
        if (!run || addr < run_start || addr - run_start >= run_size) {
            return false;
        }
        const u64 offset = addr - run_start;
        const u64 n = std::min<u64>(size, run_size - offset);
        if (std::memcmp(run + offset, src, n) != 0) {
            return false;
        }
        addr += n;
        src += n;
        size -= n;
    }
    return true;
}

void BufferCache::ApplyDownloads(std::span<const ReadbackItem> pending, const u8* mapped,
                                 u64 mapped_offset, bool partial) {
    // sotc: BPE heap guard. Correctness rule: a guard is always built fresh from the heap's free tree
    // (root/count/pending are not a generation, no remembered fence or used-block shortcuts, no
    // base+aggregate gate). Cost rule: only for the heaps whose REAL extent list (the heap's own
    // doubly linked list of memory extents, BpeHeapGuard::ReadExtents) overlaps a copy of this
    // batch and is not proven to lie wholly in the payload of one live block (headers read now); a
    // heap whose list cannot be proven well formed is always guarded. Metadata of a heap lives only
    // inside its extents (headers and free blocks), so a heap no copy reaches has nothing to preserve.
    // Each copy is filtered against the union of the touched heaps' metadata (overlapping extents
    // of different heaps included). The table has 48 entries; do not stop after eight heaps.
    //
    // sotc: Lote 3 (BbToggle::BpeGuardDiff, default on): before any of that, a copy whose bytes
    // already equal guest memory is dropped from the batch: writing equal bytes changes nothing, so
    // there is nothing to write and nothing to guard. This is decided now, from the bytes, not from
    // anything remembered. A copy that overlaps another copy of the batch is never dropped (the
    // writes of overlapping copies are order dependent). The guards of the remaining copies are built
    // exactly as before (pooled objects and scratch vectors, same reads). SOTC_L3_VERIFY=1 runs the
    // old selection and writes into scratch buffers and compares them with the new ones.
    const bool stats = SotcStats::kFpsLog;
    using Clock = std::chrono::steady_clock;
    Clock::time_point t_select{};
    const bool fast = !BbToggle::Disabled(BbToggle::BpeGuardDiff);
    static const bool verify = [] {
        const char* v = std::getenv("SOTC_L3_VERIFY");
        return v && v[0] == '1';
    }();
    thread_local std::vector<BpeHeapGuard::Extent> copies;
    thread_local std::vector<BpeHeapGuard::Extent> scratch;
    thread_local std::vector<BpeHeapGuard::Extent> all_copies; // VERIFY: every copy of the batch
    thread_local std::vector<u8> skip;                         // per copy, in batch order: 1 = equal
    thread_local std::vector<u8> overlapped;                   // per copy: shares bytes with another
    thread_local std::vector<BpeHeapGuard> pool;
    thread_local std::vector<std::array<u64, 3>> order;        // (lo, hi, copy index) sorted by lo
    std::vector<BpeHeapGuard> local_guards;

    // Select the heaps `list` touches and build a fresh guard for each. pooled: reuse the objects and
    // scratch vectors of `guards`; count: update the SOTC_FPS counters.
    const auto select_and_build = [&](const std::vector<BpeHeapGuard::Extent>& list,
                                      std::vector<BpeHeapGuard>& guards, size_t& guard_count,
                                      bool pooled, bool count) {
        guard_count = 0;
        if (!pooled) {
            guards.clear();
        }
        if (count && stats) {
            SotcStats::heap_guard_batches.fetch_add(1, std::memory_order_relaxed);
        }
        std::array<VAddr, 48> heaps{};
        std::array<bool, 48> heap_in_image{};
        size_t heap_count = 0;
        if (const VAddr chosen = runtime_sotc_heap(); chosen != 0 && !list.empty()) {
            heap_in_image[heap_count] = chosen - runtime_sotc_heap_table() < 0x20000000;
            heaps[heap_count++] = chosen;
            const u64 heap_class = *std::bit_cast<const u64*>(chosen);
            if (const VAddr table = runtime_sotc_heap_table(); table != 0) {
                for (u32 id = 1; id < 48; ++id) {
                    const VAddr other = std::bit_cast<const VAddr*>(table)[id];
                    if (other < 0x10000 || other >= 0x10000000000ULL ||
                        std::find(heaps.begin(), heaps.begin() + heap_count, other) !=
                            heaps.begin() + heap_count) {
                        continue;
                    }
                    const bool in_image = other - table < 0x20000000;
                    if (!in_image && !memory->GetBackingPointer(other)) {
                        continue;
                    }
                    if (*std::bit_cast<const u64*>(other) == heap_class) {
                        heap_in_image[heap_count] = in_image;
                        heaps[heap_count++] = other;
                    }
                }
            }
        }
        std::array<VAddr, 48> touched{};
        size_t touched_count = 0;
        if (count && stats) {
            // First sight of each heap (up to 16): describe its bookkeeping once.
            static std::array<VAddr, 16> described{};
            for (size_t h = 0; h < heap_count; ++h) {
                const auto it = std::find(described.begin(), described.end(), heaps[h]);
                if (it == described.end()) {
                    if (const auto free = std::find(described.begin(), described.end(), VAddr{0});
                        free != described.end()) {
                        *free = heaps[h];
                        BpeHeapGuard::Describe(memory, heaps[h]);
                    }
                }
            }
        }
        for (size_t h = 0; h < heap_count; ++h) {
            int why = 0;
            if (!BpeHeapGuard::HeapTouched(memory, heaps[h], list, scratch, &why,
                                           heap_in_image[h])) {
                continue;
            }
            touched[touched_count++] = heaps[h];
            if (why != 0 && count) {
                SotcStats::heap_guard_fallbacks.fetch_add(1, std::memory_order_relaxed);
                // At most one description per heap every 20 s (stats only): why the list was not proven.
                static std::array<std::pair<VAddr, Clock::time_point>, 16> last_described{};
                if (stats) {
                    auto* slot = &last_described[0];
                    for (auto& candidate : last_described) {
                        if (candidate.first == heaps[h]) {
                            slot = &candidate;
                            break;
                        }
                        if (candidate.first == 0) {
                            slot = &candidate;
                        }
                    }
                    const auto now = Clock::now();
                    if (slot->first != heaps[h] || now - slot->second > std::chrono::seconds(20)) {
                        *slot = {heaps[h], now};
                        std::fprintf(stderr, "SOTC BPE guard: heap %#llx extent list not proven (reason %d)\n",
                                     static_cast<unsigned long long>(heaps[h]), why);
                        BpeHeapGuard::Describe(memory, heaps[h]);
                    }
                }
            }
        }
        if (count && stats) {
            const auto t_build = Clock::now();
            SotcStats::heap_guard_checked.fetch_add(heap_count, std::memory_order_relaxed);
            SotcStats::heap_guard_ns_select.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t_build - t_select).count(),
                std::memory_order_relaxed);
        }
        const auto t_guards = count && stats ? Clock::now() : Clock::time_point{};
        if (!pooled) {
            guards.reserve(touched_count);
        }
        for (size_t t = 0; t < touched_count; ++t) {
            if (pooled && t < guards.size()) {
                guards[t].Rebuild(touched[t]);
            } else {
                guards.emplace_back(memory, touched[t], pooled);
            }
            if (count) {
                SotcStats::heap_guard_builds.fetch_add(1, std::memory_order_relaxed);
            }
            if (count && stats) {
                SotcStats::heap_guard_nodes.fetch_add(guards[t].TreeNodes(),
                                                      std::memory_order_relaxed);
            }
        }
        guard_count = touched_count;
        if (count && stats && touched_count) {
            SotcStats::heap_guard_ns_build.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t_guards).count(),
                std::memory_order_relaxed);
        }
    };

    // Which copies are dropped (equal to guest memory), and the list the guards are selected for.
    copies.clear();
    skip.clear();
    for (const auto& item : pending) {
        for (const auto& copy : item.copies) {
            const VAddr dst_addr = item.arena->cpu_addr + copy.srcOffset;
            copies.push_back({dst_addr, dst_addr + copy.size});
        }
    }
    const size_t copy_count = copies.size();
    if (fast && verify) {
        all_copies = copies;
    }
    if (fast && copy_count != 0) {
        const auto t_diff = stats ? Clock::now() : Clock::time_point{};
        skip.assign(copy_count, 0);
        // Copies that overlap another copy of the batch are always written.
        order.clear();
        for (size_t i = 0; i < copy_count; ++i) {
            order.push_back({copies[i].lo, copies[i].hi, i});
        }
        std::sort(order.begin(), order.end());
        overlapped.assign(copy_count, 0);
        {
            size_t group_begin = 0;
            u64 group_end = 0;
            for (size_t k = 0; k < order.size(); ++k) {
                if (k != group_begin && order[k][0] >= group_end) {
                    group_begin = k;
                }
                if (k == group_begin) {
                    group_end = order[k][1];
                } else {
                    group_end = std::max<u64>(group_end, order[k][1]);
                    for (size_t j = group_begin; j <= k; ++j) {
                        overlapped[order[j][2]] = 1;
                    }
                }
            }
        }
        size_t index = 0;
        u64 skipped = 0, skipped_bytes = 0, total_bytes = 0, overlapped_copies = 0;
        for (const auto& item : pending) {
            for (const auto& copy : item.copies) {
                total_bytes += copy.size;
                if (overlapped[index]) {
                    ++overlapped_copies;
                } else {
                    const u8* src = mapped + (copy.dstOffset - mapped_offset);
                    if (GuestBytesEqual(memory, copies[index].lo, src, copy.size)) {
                        skip[index] = 1;
                        ++skipped;
                        skipped_bytes += copy.size;
                    }
                }
                ++index;
            }
        }
        if (skipped != 0) {
            // The selection sees only the copies that will be written.
            size_t kept = 0;
            for (size_t i = 0; i < copy_count; ++i) {
                if (!skip[i]) {
                    copies[kept++] = copies[i];
                }
            }
            copies.resize(kept);
        }
        if (stats) {
            SotcStats::bpe_diff_batches.fetch_add(1, std::memory_order_relaxed);
            SotcStats::bpe_diff_copies.fetch_add(copy_count, std::memory_order_relaxed);
            SotcStats::bpe_diff_skipped.fetch_add(skipped, std::memory_order_relaxed);
            SotcStats::bpe_diff_overlapped.fetch_add(overlapped_copies, std::memory_order_relaxed);
            SotcStats::bpe_diff_bytes.fetch_add(total_bytes, std::memory_order_relaxed);
            SotcStats::bpe_diff_skipped_bytes.fetch_add(skipped_bytes, std::memory_order_relaxed);
            if (copies.empty()) {
                SotcStats::bpe_diff_empty_batches.fetch_add(1, std::memory_order_relaxed);
            }
            SotcStats::bpe_diff_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t_diff).count(),
                std::memory_order_relaxed);
        }
    }

    std::vector<BpeHeapGuard>& guards = fast ? pool : local_guards;
    size_t guard_count = 0;
    if (stats) {
        t_select = Clock::now();
    }
    select_and_build(copies, guards, guard_count, fast, true);
    const std::span<BpeHeapGuard> active{guards.data(), guard_count};

    if (verify && fast) {
        // Old behaviour in scratch buffers: guards for every copy of the batch (fresh objects), every
        // copy written with them over a copy of the current bytes. New behaviour: the same copy with
        // the new guards, or nothing when it was dropped. The bytes must agree.
        std::vector<BpeHeapGuard> old_guards;
        size_t old_count = 0;
        select_and_build(all_copies, old_guards, old_count, false, false);
        static std::atomic<u64> checked{0}, bad{0}, unreadable{0}, dropped_checked{0}, transient{0};
        thread_local std::vector<u8> old_bytes, new_bytes;
        size_t index = 0;
        for (const auto& item : pending) {
            for (const auto& copy : item.copies) {
                const bool dropped = !skip.empty() && skip[index];
                ++index;
                const VAddr dst_addr = item.arena->cpu_addr + copy.srcOffset;
                const u8* src = mapped + (copy.dstOffset - mapped_offset);
                ForEachNonStackRange(dst_addr, copy.size, [&](VAddr addr, u64 size) {
                    old_bytes.resize(size);
                    new_bytes.resize(size);
                    if (!memory->TryReadBacking(addr, old_bytes.data(), size)) {
                        unreadable.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    std::memcpy(new_bytes.data(), old_bytes.data(), size);
                    BpeHeapGuard::Write(memory, std::span<BpeHeapGuard>{old_guards.data(), old_count},
                                        addr, src + (addr - dst_addr), size, old_bytes.data());
                    if (!dropped) {
                        BpeHeapGuard::Write(memory, active, addr, src + (addr - dst_addr), size,
                                            new_bytes.data(), true);
                    } else {
                        dropped_checked.fetch_add(1, std::memory_order_relaxed);
                    }
                    const u64 n = checked.fetch_add(1, std::memory_order_relaxed) + 1;
                    bool differs = std::memcmp(old_bytes.data(), new_bytes.data(), size) != 0;
                    if (differs) {
                        // The guards of the two paths are built from snapshots taken a few
                        // microseconds apart without the guest allocator's lock: a heap changing in
                        // between gives a spurious difference. Build both again, now, and compare
                        // again; only a difference that persists is a mismatch.
                        std::vector<BpeHeapGuard> old2, new2;
                        size_t old2_count = 0, new2_count = 0;
                        select_and_build(all_copies, old2, old2_count, false, false);
                        select_and_build(copies, new2, new2_count, false, false);
                        thread_local std::vector<u8> old2_bytes, new2_bytes;
                        old2_bytes.resize(size);
                        new2_bytes.resize(size);
                        if (memory->TryReadBacking(addr, old2_bytes.data(), size)) {
                            std::memcpy(new2_bytes.data(), old2_bytes.data(), size);
                            BpeHeapGuard::Write(memory, std::span<BpeHeapGuard>{old2.data(), old2_count},
                                                addr, src + (addr - dst_addr), size,
                                                old2_bytes.data());
                            if (!dropped) {
                                BpeHeapGuard::Write(memory,
                                                    std::span<BpeHeapGuard>{new2.data(), new2_count},
                                                    addr, src + (addr - dst_addr), size,
                                                    new2_bytes.data());
                            }
                            differs = std::memcmp(old2_bytes.data(), new2_bytes.data(), size) != 0;
                        }
                        if (!differs) {
                            transient.fetch_add(1, std::memory_order_relaxed);
                            std::fprintf(stderr,
                                         "SOTC L3 VERIFY BPE guard: transient difference at %#llx+%#llx "
                                         "(equal on the second build)\n",
                                         static_cast<unsigned long long>(addr),
                                         static_cast<unsigned long long>(size));
                        }
                    }
                    if (differs) {
                        const u64 b = bad.fetch_add(1, std::memory_order_relaxed) + 1;
                        if (b <= 20) {
                            u64 first = 0;
                            while (old_bytes[first] == new_bytes[first]) {
                                ++first;
                            }
                            std::fprintf(stderr,
                                         "SOTC L3 VERIFY BPE guard MISMATCH at %#llx+%#llx (first byte "
                                         "%llu, dropped %d)\n",
                                         static_cast<unsigned long long>(addr),
                                         static_cast<unsigned long long>(size),
                                         static_cast<unsigned long long>(first), int(dropped));
                        }
                    }
                    if ((n & 0x3FFF) == 0) {
                        std::fprintf(stderr,
                                     "SOTC L3 VERIFY BPE guard: checked %llu ranges (%llu dropped, "
                                     "%llu unreadable, %llu transient), bad %llu\n",
                                     static_cast<unsigned long long>(n),
                                     static_cast<unsigned long long>(dropped_checked.load()),
                                     static_cast<unsigned long long>(unreadable.load()),
                                     static_cast<unsigned long long>(transient.load()),
                                     static_cast<unsigned long long>(bad.load()));
                    }
                });
            }
        }
    }

    const auto t_write = stats && guard_count != 0 ? Clock::now() : Clock::time_point{};
    size_t index = 0;
    for (const auto& item : pending) {
        for (const auto& copy : item.copies) {
            const bool dropped = !skip.empty() && skip[index];
            ++index;
            if (dropped) {
                continue; // bytes already equal guest memory: nothing to write
            }
            const VAddr dst_addr = item.arena->cpu_addr + copy.srcOffset;
            const u8* src = mapped + (copy.dstOffset - mapped_offset);
            static const bool change_stats = std::getenv("SOTC_RB_CHANGES") != nullptr;
            if (const u8* now = change_stats ? memory->GetBackingPointer(dst_addr) : nullptr) {
                // sotc: did the GPU actually change these bytes? (SOTC_RB_CHANGES=1, SOTC_FPS line)
                u64 changed = 0;
                for (u64 i = 0; i < copy.size; ++i) {
                    changed += now[i] != src[i];
                }
                SotcStats::readback_copies.fetch_add(1, std::memory_order_relaxed);
                SotcStats::readback_bytes.fetch_add(copy.size, std::memory_order_relaxed);
                if (changed) {
                    SotcStats::readback_copies_changed.fetch_add(1, std::memory_order_relaxed);
                    SotcStats::readback_bytes_changed.fetch_add(changed, std::memory_order_relaxed);
                }
            }
            // sotc: hot data is applied only to pages the guest has not written since the
            // gather: a write fault during the GPU wait (the range was still GPU-modified, or
            // its page left writable by the fault grace) set the CPU bit, and the guest's bytes
            // are the right ones. Checked again here, page by page, and written under the page's
            // region lock, so a write fault in between waits instead of being overwritten (the
            // game's resource loader crashed asking for 59 GiB once, 24 Oct: a size it had just
            // read from a file, replaced by stale GPU bytes).
            const auto write_pages = [&](VAddr start, u64 size, auto&& write) {
                if (!item.hot) {
                    write(start, size);
                    return;
                }
                const u64 skipped = memory_tracker->WriteUnlessCpuModified(start, size, write);
                if (skipped) {
                    SotcStats::rb_hot_late_skipped.fetch_add(skipped, std::memory_order_relaxed);
                }
            };
            write_pages(dst_addr, copy.size, [&](VAddr part_addr, u64 part_size) {
            ForEachNonStackRange(part_addr, part_size, [&](VAddr addr, u64 size) {
                const auto kept = BpeHeapGuard::Write(memory, active, addr,
                                                      src + (addr - dst_addr), size, nullptr, fast);
                // sotc: count clipped writes even when the skipped GPU bytes equal CPU metadata.
                if (SotcStats::kFpsLog && kept.clipped) {
                    SotcStats::heap_guard_clipped_writes.fetch_add(1, std::memory_order_relaxed);
                    SotcStats::heap_guard_clipped_bytes.fetch_add(kept.clipped,
                                                                 std::memory_order_relaxed);
                }
                if (kept.differing) {
                    static u64 reported = 0;
                    if (reported++ < 8) {
                        LOG_WARNING(Render, "BPE heap guard kept {:#x} bytes of heap metadata at {:#x}",
                                    kept.differing, addr);
                    }
                }
            });
            });
        }
        if (!partial) {
            memory_tracker->UnmarkRegionAsGpuModified(item.addr, item.size, false);
        }
        NoteApplied(item);
    }
    if (stats && guard_count != 0) {
        SotcStats::heap_guard_ns_write.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t_write).count(),
            std::memory_order_relaxed);
    }
}

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer,
                                                        BindToken* token) {
    // sotc: ObtainBufferForImage asks for class 1 for this one call (consumed here, whatever path
    // the call takes); every other bind is class 2 (see block_class).
    const u8 bind_class = std::exchange(arena_bind_class, u8{2});
    if (SotcStats::kStallLog) {
        SotcStats::stall_caller = __builtin_return_address(0);
    }
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    // sotc: SOTC_STREAM_THRESHOLD=<bytes> (default 16 KiB) for A/B: small read-only buffers are
    // copied into the stream buffer on every bind instead of using the synchronized arena.
    static const u64 stream_threshold = [] {
        const char* v = std::getenv("SOTC_STREAM_THRESHOLD");
        // sotc: 0 by default: with the unlocked no-change check the arena is cheaper than copying
        // (+7% on the heavy benchmark, attract demo min 20 -> 24 FPS).
        return v ? std::strtoull(v, nullptr, 0) : u64{0};
    }();
    if (!is_written && size <= stream_threshold && !IsRegionGpuModified(device_addr, size)) {
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        return {&stream_buffer, offset};
    }
    // sotc: BbToggle::BufferBindMemo, see BindToken. A caller that wants a token gets one only for
    // a plain read-only binding that found no CPU-modified page: what this function does for it is
    // then a function of the resident set, the use epoch and the CPU bits of the range, all read
    // BEFORE the work below (whatever changes meanwhile makes the token fail its next check).
    CpuGenKey snapshot{};
    bool snapshot_ok = false;
    u64 resident_version_before = 0;
    u16 epoch_before = 0;
    if (token) {
        token->valid = false;
        if (!is_written && !is_texel_buffer && size != 0) {
            resident_version_before = resident_version;
            epoch_before = use_epoch;
            snapshot_ok = memory_tracker->SnapshotCpuGen(device_addr, size, snapshot);
        }
    }
    const bool disp_dp = DispPhase::kOn && DispPhase::Active(); // sotc: SOTC_DISP_PHASES
    const u64 disp_t0 = disp_dp ? DispPhase::Now() : 0;
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block, bind_class);
    const u64 disp_t1 = disp_dp ? DispPhase::Now() : 0;
    // SynchronizeMemory also syncs read-only texel buffers from an aliasing image (sotc: a second
    // SynchronizeMemoryFromImage call here repeated that work, re-tiling the image twice).
    bool uploaded = true;
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer, &uploaded);
    const u64 disp_t2 = disp_dp ? DispPhase::Now() : 0;
    if (snapshot_ok && !uploaded) {
        *token = {snapshot, resident_version_before, epoch_before, true};
    }
    if (SotcStats::OptOff() && is_texel_buffer && !is_written) {
        SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    if (is_written) {
        Core::MemoryManager::NoteHostWrite(
            device_addr, size, 8, reinterpret_cast<u64>(__builtin_return_address(0)));
        MarkWrittenBinding(device_addr, size);
    }
    if (disp_dp) {
        auto& u = DispPhase::Subs();
        const int w = is_written ? 1 : 0;
        ++u.ob_n[w];
        u.ob_up[w] += uploaded ? 1 : 0;
        u.ob_arena[w] += disp_t1 - disp_t0;
        u.ob_sync[w] += disp_t2 - disp_t1;
        u.ob_sync_up[w] += uploaded ? disp_t2 - disp_t1 : 0;
        u.ob_mark[w] += DispPhase::Now() - disp_t2;
    }
    return {arena, arena->Offset(device_addr)};
}

// sotc: see WriteMarkMemo (buffer_cache.h). SOTC_WRITEMARK_VERIFY=1 runs the two calls on every
// hit as well and compares what they change (a hit must change nothing), counting mismatches;
// SOTC_WRITEMARK_STATS=1 prints the hit counts.
void BufferCache::MarkWrittenBinding(VAddr addr, u64 size) {
    static const bool verify = [] {
        const char* v = std::getenv("SOTC_WRITEMARK_VERIFY");
        return v && v[0] == '1';
    }();
    static const bool stats = verify || [] {
        const char* v = std::getenv("SOTC_WRITEMARK_STATS");
        return v && v[0] == '1';
    }();
    static const bool diag_watch = std::getenv("SOTC_GPUMOD_WATCH") != nullptr;
    if (size == 0 || diag_watch || BbToggle::Disabled(BbToggle::WriteMarkMemo) ||
        SotcStats::watch_addr.load(std::memory_order_relaxed) != 0) {
        AddGpuModified(addr, size);
        RecordCurrentWrite(addr, size);
        return;
    }
    const auto hot_scan = [&] {
        const VAddr end = addr + size;
        for (const auto& [start, stop] : hot_reads) {
            if (addr < stop && start < end) {
                return true;
            }
        }
        return false;
    };
    const auto inflight_state = [&] { // everything MarkInflightReadbacksDirty can change
        u64 h = inflight_readbacks.size();
        for (const AsyncReadback* r : inflight_readbacks) {
            h = h * 1000003 + (r->dirty ? 1 : 0) + (r->stale_overflow ? 2 : 0) + r->first_cause * 4 +
                r->stale.size() * 1024;
            for (const auto& s : r->stale) {
                h = (h ^ s.lo) * 0x9E3779B97F4A7C15ull;
                h = (h ^ s.hi) * 0x9E3779B97F4A7C15ull;
                h ^= s.cause;
            }
        }
        return h;
    };
    auto& m = write_mark_memos[((addr * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full)) >>
                               56];
    if (m.addr != addr || m.size != size) {
        m = {};
        m.addr = addr;
        m.size = size;
    }
    static std::atomic<u64> n_total{0}, n_a{0}, n_b{0}, n_bad{0};
    const u64 gm = __atomic_load_n(&gpu_modified_ranges.generation, __ATOMIC_ACQUIRE);
    bool a_hit = m.a_valid && m.inflight_gen == inflight_gen;
    if (a_hit && m.gm_gen != gm) {
        // Ranges left gpu_modified_ranges since (readbacks applied): the binding's own range may
        // not be among them. Contained now (generation read before the look) = Add changes nothing.
        std::shared_lock lock{gpu_modified_mutex};
        const u64 gm_now = __atomic_load_n(&gpu_modified_ranges.generation, __ATOMIC_ACQUIRE);
        if (gpu_modified_ranges.Contains(addr, size)) {
            m.gm_gen = gm_now;
        } else {
            a_hit = false;
        }
    }
    if (a_hit) {
        if (verify) {
            const u64 before = inflight_state();
            bool contained;
            {
                std::shared_lock lock{gpu_modified_mutex};
                contained = gpu_modified_ranges.Contains(addr, size);
            }
            AddGpuModified(addr, size);
            if (!contained || inflight_state() != before) {
                if (n_bad.fetch_add(1, std::memory_order_relaxed) < 10) {
                    std::fprintf(stderr, "SOTC WRITEMARK VERIFY: part A mismatch at %#llx+%#llx "
                                         "contained %d\n",
                                 static_cast<unsigned long long>(addr),
                                 static_cast<unsigned long long>(size), int(contained));
                }
            }
        }
    } else {
        AddGpuModified(addr, size);
        m.a_valid = true;
        m.gm_gen = gm;
        m.inflight_gen = inflight_gen;
    }
    const u64 tick = scheduler.CurrentTick();
    const bool b_hit = m.b_valid && m.tick == tick && current_writes_tick == tick &&
                       m.cw_gen == __atomic_load_n(&current_writes.generation, __ATOMIC_ACQUIRE) &&
                       m.hot_gen == hot_reads_gen;
    if (b_hit) {
        if (verify) {
            const bool contained = current_writes.Contains(addr, size);
            const bool pending_before = hot_write_pending;
            RecordCurrentWrite(addr, size);
            const bool expect = pending_before || m.hot_overlap;
            if (!contained || hot_write_pending != expect) {
                if (n_bad.fetch_add(1, std::memory_order_relaxed) < 10) {
                    std::fprintf(stderr, "SOTC WRITEMARK VERIFY: part B mismatch at %#llx+%#llx "
                                         "contained %d pending %d->%d expected %d\n",
                                 static_cast<unsigned long long>(addr),
                                 static_cast<unsigned long long>(size), int(contained),
                                 int(pending_before), int(hot_write_pending), int(expect));
                }
            }
        } else if (m.hot_overlap) {
            hot_write_pending = true;
        }
    } else {
        RecordCurrentWrite(addr, size);
        m.b_valid = true;
        m.tick = tick;
        m.cw_gen = __atomic_load_n(&current_writes.generation, __ATOMIC_ACQUIRE);
        m.hot_gen = hot_reads_gen;
        m.hot_overlap = hot_scan();
    }
    if (stats) {
        n_a.fetch_add(a_hit, std::memory_order_relaxed);
        n_b.fetch_add(b_hit, std::memory_order_relaxed);
        const u64 n = n_total.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((n & 0x3FFFF) == 0) {
            std::fprintf(stderr,
                         "SOTC WRITEMARK %s: written bindings %llu, part A hits %.1f%%, part B hits "
                         "%.1f%%, mismatches %llu\n",
                         verify ? "verify" : "stats", static_cast<unsigned long long>(n),
                         100.0 * double(n_a.load()) / double(n), 100.0 * double(n_b.load()) / double(n),
                         static_cast<unsigned long long>(n_bad.load()));
        }
    }
}

bool BufferCache::BindTokenValid(const BindToken& token) const noexcept {
    return token.valid && token.resident_version == resident_version && token.epoch == use_epoch &&
           MemoryTracker::CpuGenUnchanged(token.gen);
}

void BufferCache::AddGpuModified(VAddr addr, u64 size) {
    MarkInflightReadbacksDirty(addr, size, CauseGpuWrite);
    // sotc: SOTC_GPUMOD_WATCH=<address>: who marks it as GPU-written (first 40 times).
    static const VAddr env_watch = [] {
        const char* v = std::getenv("SOTC_GPUMOD_WATCH");
        return v ? static_cast<VAddr>(std::strtoull(v, nullptr, 0)) : VAddr{0};
    }();
    // SOTC_CS_WATCH (vk_rasterizer.cpp) arms it at run time.
    const VAddr watch = env_watch ? env_watch : SotcStats::watch_addr.load(std::memory_order_relaxed);
    if (watch && addr < watch + 0x10000 && watch < addr + size) {
        // The first 40 writes, then the first write of every other program (up to 64 more).
        static int shown = 0;
        static u64 programs[64]{};
        static int num_programs = 0;
        const u64 program = SotcStats::current_program.load();
        bool new_program = std::find(programs, programs + num_programs, program) ==
                           programs + num_programs;
        if (new_program && num_programs < 64) {
            programs[num_programs++] = program;
        } else {
            new_program = false;
        }
        if (shown++ < 40 || new_program) {
            std::fprintf(stderr, "SOTC gpu-modified %#llx+%#llx by %p <- %p, program %#llx\n",
                         static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size),
                         __builtin_return_address(0), __builtin_return_address(1),
                         static_cast<unsigned long long>(SotcStats::current_program.load()),
                SotcStats::StallOffset(SotcStats::stall_caller));
        }
    }
    // sotc: gpu_modified_ranges only grows here (every other use subtracts), and the clean page
    // cache below only has to drop pages whose GPU-written hull grew: when the range was already
    // contained, no page can have become dirty. SOTC_GPUMOD_NOGROW_SKIP=0 always scans.
    static const bool nogrow_skip = [] {
        const char* v = std::getenv("SOTC_GPUMOD_NOGROW_SKIP");
        return !v || v[0] != '0';
    }();
    bool grew;
    {
        std::unique_lock gpu_modified_lock{gpu_modified_mutex};
        grew = gpu_modified_ranges.Add(addr, size);
    }
    if (!clean_pages_used || size == 0 || (!grew && nogrow_skip)) {
        return;
    }
    // The GPU-written hull of these pages grows: drop them from the clean page cache.
    const u64 first = addr >> CleanPageBits;
    const u64 last = (addr + size - 1) >> CleanPageBits;
    if (last - first >= NumCleanPages) {
        for (auto& page : clean_pages) {
            const u64 index = page.page_addr >> CleanPageBits;
            if (index >= first && index <= last) {
                page.page_addr = ~0ULL;
            }
        }
        return;
    }
    for (u64 index = first; index <= last; ++index) {
        auto& page = clean_pages[index % NumCleanPages];
        if (page.page_addr == index << CleanPageBits) {
            page.page_addr = ~0ULL;
        }
    }
}

bool BufferCache::ReadClean(VAddr addr, void* out, u32 size) {
    const VAddr page_addr = addr & ~(CleanPageSize - 1);
    if (addr + size > page_addr + CleanPageSize) {
        return false;
    }
    // sotc: the command processor while the draw thread owns the GPU state: no page cache (the
    // draw thread maintains it), the ranges under the lock.
    if (liverpool->RunsGpuWorkElsewhere()) {
        const u8* backing = memory->GetBackingPointer(page_addr);
        if (!backing) {
            return false;
        }
        std::shared_lock gpu_modified_lock{gpu_modified_mutex};
        if (gpu_modified_ranges.Intersects(addr, size)) {
            return false;
        }
        std::memcpy(out, backing + (addr - page_addr), size);
        return true;
    }
    const u64 generation = clean_map_generation.load(std::memory_order_acquire);
    auto& page = clean_pages[(addr >> CleanPageBits) % NumCleanPages];
    if (page.page_addr != page_addr || page.map_generation != generation) {
        const u8* backing = memory->GetBackingPointer(page_addr);
        if (!backing) {
            return false;
        }
        u32 dirty_lo = CleanPageSize;
        u32 dirty_hi = 0;
        gpu_modified_ranges.ForEachInRange(page_addr, CleanPageSize, [&](VAddr start, VAddr end) {
            dirty_lo = std::min<u32>(dirty_lo, static_cast<u32>(start - page_addr));
            dirty_hi = std::max<u32>(dirty_hi, static_cast<u32>(end - page_addr));
        });
        clean_pages_used = true;
        page = {
            .page_addr = page_addr,
            .map_generation = generation,
            .backing = backing,
            .dirty_lo = dirty_lo,
            .dirty_hi = dirty_hi,
        };
    }
    const u32 offset = static_cast<u32>(addr - page_addr);
    if (offset < page.dirty_hi && offset + size > page.dirty_lo &&
        gpu_modified_ranges.Intersects(addr, size)) {
        return false;
    }
    std::memcpy(out, page.backing + offset, size);
    return true;
}

bool BufferCache::TakeHotWrite() noexcept {
    // Only while that write is still in the command buffer being built.
    return std::exchange(hot_write_pending, false) && current_writes_tick == scheduler.CurrentTick();
}

void BufferCache::NoteHotRead(VAddr addr, u64 size) {
    const VAddr end = addr + size;
    for (const auto& [start, stop] : hot_reads) {
        if (start == addr && stop == end) {
            return;
        }
    }
    hot_reads[hot_reads_next] = {addr, end};
    hot_reads_next = (hot_reads_next + 1) % NumHotReads;
    ++hot_reads_gen; // sotc: WriteMarkMemo
}

void BufferCache::RecordCurrentWrite(VAddr addr, u64 size) {
    if (!hot_write_pending) {
        const VAddr end = addr + size;
        for (const auto& [start, stop] : hot_reads) {
            if (addr < stop && start < end) {
                hot_write_pending = true;
                break;
            }
        }
    }
    if (current_writes_tick != scheduler.CurrentTick()) {
        current_writes.Clear();
        current_writes_tick = scheduler.CurrentTick();
    }
    current_writes.Add(addr, size);
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size,
                                                                bool prefer_arena) {
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    // sotc: SOTC_IMG_ARENA_MB (default 16, 0 = off): the images RefreshImage asks this for have been
    // uploaded before and are at least that big (texture arrays the game fills layer by layer:
    // 85 MB re-copied whole for a 1.3 MB layer, 6-43 ms each). The arena keeps their bytes on the
    // GPU, and only the pages written since are copied again; the unchanged ones are exactly
    // what the whole copy would have brought.
    static const u64 arena_min = [] {
        const char* v = std::getenv("SOTC_IMG_ARENA_MB");
        return (v ? std::strtoull(v, nullptr, 10) : 16ull) << 20;
    }();
    if (prefer_arena && arena_min != 0 && size >= arena_min) {
        // sotc: the blocks this binds are marked as image sources (consumed by ObtainBuffer).
        arena_bind_class = 1;
        return ObtainBuffer(device_addr, size, false);
    }
    const auto stall_t0 = SotcStats::kStallLog ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    const auto stall_t1 = SotcStats::kStallLog ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
    if (AsyncUploadFor(size)) {
        std::vector<GuestCopyPiece> pieces;
        pieces.push_back({device_addr, staging.mapped, staging.size});
        StartGuestCopy(std::move(pieces), staging.size, staging);
    } else {
        memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
        staging.Flush();
    }
    if (SotcStats::kStallLog) {
        SotcStats::StallNote(SotcStats::StallMsSince(stall_t0),
                             "image-source copy %.1f ms (staging %.1f, guest copy %.1f): %.2f MB at %#llx",
                             SotcStats::StallMsSince(stall_t0),
                             std::chrono::duration<double, std::milli>(stall_t1 - stall_t0).count(),
                             SotcStats::StallMsSince(stall_t1), size / 1048576.0,
                             static_cast<unsigned long long>(device_addr));
    }
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::TryReplaceFromCpu(VAddr addr, const void* data, u64 size) {
    if (size == 0) {
        return true;
    }
    if (!memory->GetBackingPointer(addr) || !memory->GetBackingPointer(addr + size - 1)) {
        return false;
    }
    // Page tracking is page granular: other GPU-written bytes on these pages would be lost.
    const VAddr lo = Common::AlignDown(addr, VAddr{4096});
    const VAddr hi = Common::AlignUp(addr + size, VAddr{4096});
    bool foreign = false;
    gpu_modified_ranges.ForEachInRange(lo, hi - lo, [&](VAddr start, VAddr end) {
        foreign |= start < addr || end > addr + size;
    });
    if (foreign) {
        return false;
    }
    {
        std::unique_lock gpu_modified_lock{gpu_modified_mutex};
        gpu_modified_ranges.Subtract(addr, size);
    }
    memory_tracker->UnmarkRegionAsGpuModified(addr, size, false);
    MarkInflightReadbacksDirty(addr, size, CauseCpuReplace);
    if (!memory->TryWriteBacking(std::bit_cast<void*>(addr), data, size, 10)) {
        return false;
    }
    memory_tracker->MarkRegionAsCpuModified(addr, size);
    return true;
}

bool BufferCache::TryWriteThroughFromCpu(VAddr addr, const void* data, u64 size) {
    if (size == 0) {
        return true;
    }
    const u64 first_block = addr >> block_shift;
    const u64 last_block = (addr + size - 1) >> block_shift;
    bool resident = true;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64, u64) { resident = false; });
    const Buffer* arena =
        resident ? address_space[first_block >> blocks_per_arena_page_shift] : nullptr;
    if (!arena || arena != address_space[last_block >> blocks_per_arena_page_shift]) {
        return TryReplaceFromCpu(addr, data, size);
    }
    if (!memory->GetBackingPointer(addr) || !memory->GetBackingPointer(addr + size - 1)) {
        return false;
    }
    // Page tracking is page granular: other GPU-written bytes on these pages would be lost.
    const VAddr lo = Common::AlignDown(addr, VAddr{4096});
    const VAddr hi = Common::AlignUp(addr + size, VAddr{4096});
    bool foreign = false;
    gpu_modified_ranges.ForEachInRange(lo, hi - lo, [&](VAddr start, VAddr end) {
        foreign |= start < addr || end > addr + size;
    });
    if (foreign) {
        return false;
    }
    {
        // sotc: the command processor reads the set under the shared lock (ReadClean).
        std::unique_lock gpu_modified_lock{gpu_modified_mutex};
        gpu_modified_ranges.Subtract(addr, size);
    }
    memory_tracker->UnmarkRegionAsGpuModified(addr, size, false);
    MarkInflightReadbacksDirty(addr, size, CauseCpuReplace);
    if (!memory->TryWriteBacking(std::bit_cast<void*>(addr), data, size, 10)) {
        return false;
    }
    // The arena gets the same bytes now, in stream order, so the CPU bits stay as they are: a
    // clean page stays clean and write-protected, a dirty one uploads the same bytes later.
    const auto staging = staging_pool.Request(size, MemoryType::HostUncached);
    std::memcpy(staging.mapped, data, size);
    staging.Flush();
    const vk::BufferCopy copy{staging.offset, arena->Offset(addr), size};
    runtime.CopyBuffer(staging.buffer, arena, std::span{&copy, 1});
    RecordCurrentWrite(addr, size);
    return true;
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

void BufferCache::SynchronizeDmaBuffers() {
    const SotcStats::FrameTimer frame_timer{SotcStats::FrameDma};
    fault_process_pending = true;
    const u64 uploads_before = SotcStats::uploads.load(std::memory_order_relaxed);
    const auto sweep_start = std::chrono::steady_clock::now();
    u64 ranges = 0;
    // sotc (SOTC_DMA_MERGE=0 disables): the sweep visited ~1200 resident ranges ~100 times a
    // second, one tracker walk + staging request each. Touching ranges whose start maps to the
    // same arena are merged into runs (one SynchronizeMemory per run), rebuilt only when
    // resident_ranges or the arena set changed. Same pages are uploaded into the same arena,
    // just batched. A run is capped (SOTC_DMA_MERGE_MAX MiB, default 16 = staging ring block,
    // max 64) so merging never pushes an upload to RequestLarge; a single range larger than the
    // cap stays one run as before. Runs hold addresses only; the arena is looked up per run at
    // sync time (a migration merges arenas, so entries sharing one keep sharing one).
    static const bool dma_merge = [] {
        const char* v = std::getenv("SOTC_DMA_MERGE");
        return !(v && v[0] == '0');
    }();
    static const u64 dma_merge_max = [] {
        const char* v = std::getenv("SOTC_DMA_MERGE_MAX");
        const u64 mib = v ? std::strtoull(v, nullptr, 10) : 0;
        return std::clamp<u64>(mib ? mib : 16, 1, 64) << 20;
    }();
    // sotc: two-phase sweep (only with SOTC_PROTECT_BATCH=1; default: one SynchronizeMemory per run). Phase 1
    // collects the CPU-modified pages of every run (their write protection is batched); one flush
    // issues the merged protects; phase 2 copies. Same order as before per run (collect, protect,
    // then copy), and the protect of every run precedes every copy.
    const bool two_phase = ProtectBatchEnabled();
    dma_jobs.clear();
    UploadJob job;
    const auto sync_run = [&](const Buffer* arena, VAddr addr, u32 size) {
        if (!two_phase) {
            SynchronizeMemory(arena, addr, size, false, false);
            return;
        }
        job.arena = arena;
        CollectUploads(job, addr, size, false);
        if (!job.copies.empty()) {
            dma_jobs.push_back(std::move(job));
        }
    };
    if (dma_merge) {
        if (dma_runs_version != resident_version || dma_runs_arenas != arenas.size()) {
            dma_runs_version = resident_version;
            dma_runs_arenas = arenas.size();
            dma_runs_ranges = resident_ranges.Size();
            dma_runs.clear();
            for (const auto& range : resident_ranges) {
                const VAddr device_addr = range.start << block_shift;
                const u64 size = (range.end - range.start) << block_shift;
                if (!dma_runs.empty()) {
                    auto& run = dma_runs.back();
                    if (run.first + run.second == device_addr &&
                        run.second + size <= dma_merge_max &&
                        address_space[run.first >> ARENA_PAGE_BITS] ==
                            address_space[device_addr >> ARENA_PAGE_BITS]) {
                        run.second += size;
                        continue;
                    }
                }
                dma_runs.emplace_back(device_addr, size);
            }
            dma_full_pending = true;
        }
        // sotc: incremental sweep (SOTC_DMA_INCR=0: every run, every time). Only the 256 KiB
        // words where a CPU bit rose since the last sweep can need an upload
        // (RegionManager::TakeSweepWords; hot pages kept by FaultGrace are marked again on each
        // upload). After the runs change (pages that became resident may hold old CPU bits) one
        // full sweep restores that. Each run is still synced once, over the hull of its dirty
        // words, so the uploads per run stay as before.
        static const bool dma_incr = [] {
            const char* v = std::getenv("SOTC_DMA_INCR");
            return !(v && v[0] == '0');
        }();
        if (!dma_incr || std::exchange(dma_full_pending, false)) {
            if (dma_incr) {
                memory_tracker->ForEachSweepSpan([](VAddr, u64) {});
            }
            // sotc: by index with a copied entry, re-reading size(): nothing nested in
            // SynchronizeMemory may invalidate the iteration even if it touched dma_runs.
            for (size_t i = 0; i < dma_runs.size(); ++i) {
                const auto [device_addr, size] = dma_runs[i];
                sync_run(address_space[device_addr >> ARENA_PAGE_BITS], device_addr,
                         static_cast<u32>(size));
            }
            ranges = dma_runs_ranges;
        } else {
            dma_spans.clear();
            memory_tracker->ForEachSweepSpan(
                [this](VAddr address, u64 size) { dma_spans.emplace_back(address, size); });
            std::ranges::sort(dma_spans);
            size_t s = 0;
            for (size_t i = 0; i < dma_runs.size() && s < dma_spans.size(); ++i) {
                const auto [run_start, run_size] = dma_runs[i];
                const VAddr run_end = run_start + run_size;
                while (s < dma_spans.size() &&
                       dma_spans[s].first + dma_spans[s].second <= run_start) {
                    ++s;
                }
                VAddr lo = ~0ULL;
                VAddr hi = 0;
                for (size_t k = s; k < dma_spans.size() && dma_spans[k].first < run_end; ++k) {
                    lo = std::min(lo, std::max(run_start, dma_spans[k].first));
                    hi = std::max(hi, std::min(run_end, dma_spans[k].first + dma_spans[k].second));
                }
                if (lo < hi) {
                    sync_run(address_space[run_start >> ARENA_PAGE_BITS], lo,
                             static_cast<u32>(hi - lo));
                    ++ranges;
                }
            }
        }
        SotcStats::dma_sweep_runs.fetch_add(dma_runs.size(), std::memory_order_relaxed);
    } else {
        for (const auto& range : resident_ranges) {
            const u64 page = range.start >> (ARENA_PAGE_BITS - block_shift);
            const VAddr device_addr = range.start << block_shift;
            const u64 size = (range.end - range.start) << block_shift;
            sync_run(address_space[page], device_addr, static_cast<u32>(size));
            ++ranges;
        }
    }
    if (two_phase) {
        PageManager::FlushProtectBatch(); // every protect before the first copy
        for (auto& collected : dma_jobs) {
            CommitUploads(collected);
        }
        dma_jobs.clear();
    }
    SotcStats::dma_sweeps.fetch_add(1, std::memory_order_relaxed);
    SotcStats::dma_sweep_ranges.fetch_add(ranges, std::memory_order_relaxed);
    const u64 sweep_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - sweep_start)
                             .count();
    SotcStats::dma_sweep_ns.fetch_add(sweep_ns, std::memory_order_relaxed);
    if (SotcStats::uploads.load(std::memory_order_relaxed) != uploads_before) {
        SotcStats::dma_sweep_uploading.fetch_add(1, std::memory_order_relaxed);
    } else {
        SotcStats::dma_sweep_idle_ns.fetch_add(sweep_ns, std::memory_order_relaxed);
    }
}

bool BufferCache::SynchronizeDmaRange(VAddr addr, u64 size) {
    if (size == 0 || size > 64_MB || !memory->GetBackingPointer(addr) ||
        !memory->GetBackingPointer(addr + size - 1)) {
        return false;
    }
    // GetArena handles a range of two arena pages at most; stay inside one 4 GiB arena group.
    if ((addr >> ARENA_PAGE_BITS) + 1 < ((addr + size - 1) >> ARENA_PAGE_BITS)) {
        return false;
    }
    fault_process_pending = true;
    ObtainBuffer(addr, static_cast<u32>(size), false);
    return true;
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space[first_page];
    const auto* last_arena = address_space[last_page];
    if (first_arena == last_arena) {
        if (!first_arena) {
            // sotc: an arena covers a group of SOTC_ARENA_GROUP pages (default 4, 16 GB) when
            // they are all free. With one page per arena, a buffer across two pages migrated
            // both arenas into a new one and rebound all their memory: a stall of 0.8-0.9 s,
            // about once a session. SotC's GPU memory lies in 0x10'0000'0000-0x13'ffff'ffff.
            static const u64 group = [] {
                const char* v = std::getenv("SOTC_ARENA_GROUP");
                const u64 n = v ? std::strtoull(v, nullptr, 10) : 4;
                return n != 0 && std::has_single_bit(n) ? n : u64{1};
            }();
            u64 group_first = first_page & ~(group - 1);
            u64 group_last = group_first + group - 1;
            for (u64 page = group_first; page <= group_last; ++page) {
                if (page >= NUM_ARENA_PAGES || address_space[page]) {
                    group_first = first_page;
                    group_last = last_page;
                    break;
                }
            }
            if (last_page > group_last) {
                group_first = first_page;
                group_last = last_page;
            }
            const u64 num_pages = group_last - group_first + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, group_first << ARENA_PAGE_BITS,
                                     num_pages << ARENA_PAGE_BITS, MemoryType::Sparse);
            for (u64 page = group_first; page <= group_last; ++page) {
                address_space[page] = new_arena;
            }
            ++resident_version; // sotc: BufferBindMemo tokens, the arena table changed
            std::fprintf(stderr, "SOTC arena at %#llx, %llu GB\n",
                         static_cast<unsigned long long>(group_first << ARENA_PAGE_BITS),
                         static_cast<unsigned long long>(num_pages << (ARENA_PAGE_BITS - 30)));
        }
        return address_space[first_page];
    }

    const auto migrate_start = std::chrono::steady_clock::now();
    SCOPE_EXIT {
        LOG_WARNING(Render, "Migrating arena: {} + {} MB of address space, {} ms",
                    (first_arena ? first_arena->size_bytes : ARENA_PAGE_SIZE) >> 20,
                    (last_arena ? last_arena->size_bytes : ARENA_PAGE_SIZE) >> 20,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - migrate_start)
                        .count());
    };

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << ARENA_PAGE_BITS);
    const u64 first_size = first_arena ? first_arena->size_bytes : ARENA_PAGE_SIZE;
    const u64 last_size = last_arena ? last_arena->size_bytes : ARENA_PAGE_SIZE;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = (backing.offset + start - backing.start) << block_shift,
        });
    });

    u64 base_page = first_addr >> ARENA_PAGE_BITS;
    for (u32 page = 0; page < (first_size >> ARENA_PAGE_BITS); ++page) {
        address_space[base_page + page] = new_arena;
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> ARENA_PAGE_BITS); ++page) {
        address_space[base_page + page] = new_arena;
    }
    ++resident_version; // sotc: BufferBindMemo tokens, the arena table changed
    return new_arena;
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block,
                                 u8 bind_class) {
    // sotc: binds check the same ranges thousands of times a frame, each a search of ~2,600
    // resident ranges (~2% of the command processor): ranges found resident are remembered
    // (64 slots by first block) until the resident set changes (resident_version).
    // sotc: the bind keeps its blocks out of MaybeEvictCold for SOTC_ARENA_EVICT_IDLE_S. A
    // cached range is stamped as a whole once per epoch (a 64 MiB binding is 1,024 blocks).
    const auto stamp = [this](u64 first, u64 end) {
        for (u64 block = first; block < end; ++block) {
            block_last_use[block] = use_epoch;
        }
    };
    // sotc: a block records the classes of the binds that touched it (block_class). A cached
    // range stands for the binds of its own class only: another class takes the slow path below
    // once, which marks it.
    auto& cached = resident_cache[(first_block * 0x9E3779B97F4A7C15ull) >> 58];
    if (cached.version == resident_version && first_block >= cached.first &&
        last_block < cached.end && cached.cls == bind_class) {
        if (cached.epoch != use_epoch) {
            stamp(cached.first, cached.end);
            cached.epoch = use_epoch;
        }
        return;
    }
    stamp(first_block, last_block + 1);
    for (u64 block = first_block; block <= last_block; ++block) {
        if ((block_class[block] & bind_class) != bind_class) {
            __atomic_fetch_or(&block_class[block], bind_class, __ATOMIC_RELAXED);
        }
    }
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        cached = {resident_version, first_block, last_block + 1, use_epoch, bind_class};
        return;
    }
    if (!evicted_ranges.Empty()) {
        for (const auto& range : bind_ranges) {
            evicted_ranges.ForEachInRange(range.start, range.end, [&](const Interval& evicted) {
                const u64 start = std::max(range.start, evicted.start);
                const u64 end = std::min(range.end, evicted.end);
                SotcStats::arena_rebound.fetch_add((end - start) << block_shift,
                                                   std::memory_order_relaxed);
            });
            evicted_ranges.Subtract(range.start, range.end);
        }
    }
    const SotcStats::FrameTimer frame_timer{SotcStats::FrameAlloc};
    const SotcStats::AllocTrace alloc_trace{SotcStats::AllocArena, u64(resident_blocks) << block_shift};
    const auto stall_t0 = SotcStats::kStallLog ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};

    // Memory first: running out of it may submit work (see AllocateArenaBlocks), which must not
    // happen with half of the binds queued.
    boost::container::small_vector<std::pair<vk::DeviceMemory, u64>, 8> placements;
    for (const auto& range : bind_ranges) {
        placements.push_back(AllocateArenaBlocks(range.end - range.start));
    }

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(resident_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);

    ArenaBinds* binds = BindsForArena(arena);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    size_t placement_index = 0;
    for (const auto& range : bind_ranges) {
        const auto [device_memory, memory_block] = placements[placement_index++];
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_block;
        resident_ranges.Add(backing);
        ++resident_version; // sotc: invalidates the merged DMA runs

        LOG_INFO(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
            .size = (range.end - range.start) << block_shift,
            .memory = device_memory,
            .memoryOffset = memory_block << block_shift,
        });

        for (u32 block = 0; block < bind.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + bind.resourceOffset + block;
        }
        const u64 copy_size = (backing.end - backing.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, backing.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
    if (SotcStats::kStallLog) {
        SotcStats::StallNote(SotcStats::StallMsSince(stall_t0),
                             "arena-resident %.1f ms: %.1f MB in %zu ranges at %#llx, caller %+lld",
                             SotcStats::StallMsSince(stall_t0),
                             (u64(resident_blocks) << block_shift) / 1048576.0, bind_ranges.Size(),
                             static_cast<unsigned long long>(first_block << block_shift),
                             SotcStats::StallOffset(SotcStats::stall_caller));
    }
}

void BufferCache::CollectUploads(UploadJob& job, VAddr device_addr, u32 size, bool is_written) {
    job.copies.clear();
    job.total_size_bytes = 0;
    job.req_addr = device_addr;
    job.req_size = size;
    job.written = is_written;
    memory_tracker->ForEachUploadRange(device_addr, size, is_written, [&](u64 addr, u64 size) {
        job.copies.emplace_back(job.total_size_bytes, addr, size);
        job.total_size_bytes += size;
    });
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer, bool* uploaded) {
    UploadJob job;
    job.arena = arena;
    CollectUploads(job, device_addr, size, is_written);
    if (uploaded) {
        *uploaded = !job.copies.empty();
    }
    if (!job.copies.empty()) {
        // sotc: the write protection of the collected pages may still be batched (draw thread,
        // ProtectBatchScope): it must be in force before the first byte is copied, or a guest
        // write between the copy and the protect would be lost.
        PageManager::FlushProtectBatch();
        CommitUploads(job);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

void BufferCache::CommitUploads(UploadJob& job) {
    const Buffer* const arena = job.arena;
    auto& copies = job.copies;
    const size_t total_size_bytes = job.total_size_bytes;
    {
        const SotcStats::FrameTimer frame_timer{SotcStats::FrameBuffer};
        if (SotcStats::kFpsLog) {
            SotcStats::frame_copy_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
        }
        SotcStats::uploads.fetch_add(1, std::memory_order_relaxed);
        ++SotcStats::thread_data_events;
        if (SotcStats::kCmdStats) { // sotc: read only by the SOTC_CMD_STATS printer
            SotcStats::upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
            if (SotcStats::kHashStats) SotcStats::hash_tl.upload += total_size_bytes;
        }
        // sotc: SOTC_STALL_LOG: phase times of this upload (staging request, guest copy, Vulkan copy).
        const auto stall_t0 = SotcStats::kStallLog ? std::chrono::steady_clock::now()
                                                   : std::chrono::steady_clock::time_point{};
        double stall_stage_ms = 0.0, stall_copy_ms = 0.0;
        const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
        std::chrono::steady_clock::time_point stall_t1{};
        if (SotcStats::kStallLog) {
            stall_t1 = std::chrono::steady_clock::now();
            stall_stage_ms = std::chrono::duration<double, std::milli>(stall_t1 - stall_t0).count();
        }
        // sotc: a large upload made of several ranges (a streaming burst: 200+ MB in one frame)
        // is copied in one parallel pass over 512 KiB pieces of all its ranges; per range, only
        // ranges of 2 MiB and more were split. SOTC_COPY_BATCH=0: per range.
        static const bool copy_batch = [] {
            const char* v = std::getenv("SOTC_COPY_BATCH");
            return !v || v[0] != '0';
        }();
        bool copied_async = false;
        if (AsyncUploadFor(total_size_bytes)) {
            // sotc: the guest bytes are copied by the copy threads while the draw thread goes on;
            // the submit waits for them (SubmitPendingArenaBinds -> WaitGuestCopies).
            std::vector<GuestCopyPiece> pieces;
            pieces.reserve(copies.size());
            for (const auto& copy : copies) {
                pieces.push_back({copy.dstOffset, staging.mapped + copy.srcOffset, copy.size});
            }
            StartGuestCopy(std::move(pieces), total_size_bytes, staging);
            copied_async = true;
        } else if (copy_batch && copies.size() > 1 && total_size_bytes >= 2_MB &&
                   BbCopy::Enabled()) {
            struct Piece {
                VAddr source;
                u8* dest;
                u64 size;
            };
            constexpr u64 PieceSize = 512_KB;
            boost::container::small_vector<Piece, 64> pieces;
            for (const auto& copy : copies) {
                for (u64 done = 0; done < copy.size; done += PieceSize) {
                    pieces.push_back({copy.dstOffset + done, staging.mapped + copy.srcOffset + done,
                                      std::min(PieceSize, copy.size - done)});
                }
            }
            BbCopy::ParallelFor(pieces.size(), [&](std::size_t i) {
                memory->CopySparseMemory(pieces[i].source, pieces[i].dest, pieces[i].size);
            });
        } else {
            for (const auto& copy : copies) {
                memory->CopySparseMemory(copy.dstOffset, staging.mapped + copy.srcOffset,
                                         copy.size);
            }
        }
        if (SotcStats::kStallLog) {
            stall_copy_ms = SotcStats::StallMsSince(stall_t1);
        }
        u64 stall_big_addr = 0, stall_big_size = 0;
        if (SotcStats::kStallLog) {
            for (const auto& copy : copies) {
                if (copy.size > stall_big_size) {
                    stall_big_size = copy.size;
                    stall_big_addr = copy.dstOffset;
                }
            }
        }
        for (auto& copy : copies) {
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        if (!copied_async) {
            staging.Flush();
        }
        runtime.CopyBuffer(staging.buffer, arena, copies);
        for (const auto& copy : copies) {
            RecordCurrentWrite(arena->cpu_addr + copy.dstOffset, copy.size);
        }
        if (SotcStats::kStallLog) {
            const double total_ms = SotcStats::StallMsSince(stall_t0);
            SotcStats::StallAgg(SotcStats::StallBufferUpload, SotcStats::current_program.load(),
                                total_ms, total_size_bytes / 1048576.0, stall_stage_ms,
                                stall_copy_ms);
            SotcStats::StallNote(
                total_ms,
                "buffer-upload %.1f ms (staging %.1f, guest copy %.1f, vk %.1f): %.2f MB in %zu "
                "ranges, window %#llx + %.2f MB%s, biggest %#llx + %.2f MB, program %#llx, caller %+lld",
                total_ms, stall_stage_ms, stall_copy_ms,
                total_ms - stall_stage_ms - stall_copy_ms, total_size_bytes / 1048576.0,
                copies.size(), static_cast<unsigned long long>(job.req_addr),
                job.req_size / 1048576.0, job.written ? " (written)" : "",
                static_cast<unsigned long long>(stall_big_addr), stall_big_size / 1048576.0,
                static_cast<unsigned long long>(SotcStats::current_program.load()),
                SotcStats::StallOffset(SotcStats::stall_caller));
        }
    }
}

// sotc: SOTC_ASYNC_UPLOAD (default on; 0 = the draw thread copies, as before). In a streaming burst the
// draw thread spent 86 of 209 ms copying 270 MB in 1,749 uploads of ~150 KB each, serially (3 GB/s;
// 12-16 GB/s when several threads copy). The copy of each upload goes to the copy threads, and the
// draw thread waits for all of them only when the command buffer is submitted (the GPU reads the
// staging memory after the submit, never before). SOTC_ASYNC_UPLOAD_MIN_KB (default 64): smaller
// uploads are copied inline.
bool BufferCache::AsyncUploadFor(u64 bytes) const {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_ASYNC_UPLOAD");
        return !v || v[0] != '0';
    }();
    static const u64 min_bytes = [] {
        const char* v = std::getenv("SOTC_ASYNC_UPLOAD_MIN_KB");
        return (v ? std::strtoull(v, nullptr, 10) : 64ull) << 10;
    }();
    return enabled && bytes >= min_bytes && BbCopy::Enabled();
}

void BufferCache::StartGuestCopy(std::vector<GuestCopyPiece>&& ranges, u64 total,
                                 const Vulkan::StagingBufferRef& staging) {
    // Pieces of at most `budget` bytes, grouped into tasks of about that size: one wake-up per task.
    const u64 budget = std::clamp<u64>(total / 8, 256_KB, 4_MB);
    auto task = std::make_shared<std::vector<GuestCopyPiece>>();
    u64 task_bytes = 0;
    const auto launch = [&] {
        if (task->empty()) {
            return;
        }
        guest_copies_pending.fetch_add(1, std::memory_order_relaxed);
        BbCopy::Async([this, pieces = std::move(task)] {
            for (const auto& piece : *pieces) {
                memory->CopySparseMemory(piece.source, piece.dest, piece.size);
            }
            guest_copies_pending.fetch_sub(1, std::memory_order_release);
        });
        task = std::make_shared<std::vector<GuestCopyPiece>>();
        task_bytes = 0;
    };
    for (const auto& range : ranges) {
        for (u64 done = 0; done < range.size;) {
            const u64 n = std::min(range.size - done, budget - task_bytes);
            task->push_back({range.source + done, range.dest + done, n});
            task_bytes += n;
            done += n;
            if (task_bytes >= budget) {
                launch();
            }
        }
    }
    launch();
    if (!staging.buffer->is_coherent) {
        std::scoped_lock lock{guest_copy_mutex};
        guest_copy_flush.push_back({staging.buffer, staging.offset, staging.size});
    }
}

void BufferCache::WaitGuestCopies() {
    const auto flush_pending = [this] {
        std::scoped_lock lock{guest_copy_mutex};
        for (const auto& entry : guest_copy_flush) {
            entry.buffer->Flush(entry.offset, entry.size);
        }
        guest_copy_flush.clear();
    };
    if (guest_copies_pending.load(std::memory_order_acquire) == 0) {
        flush_pending();
        return;
    }
    const auto start = SotcStats::kStallLog ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
    u32 spins = 0;
    while (guest_copies_pending.load(std::memory_order_acquire) != 0) {
        if (++spins < 2000) {
            _mm_pause();
        } else {
            std::this_thread::yield();
        }
    }
    flush_pending();
    if (SotcStats::kStallLog) {
        const double ms = SotcStats::StallMsSince(start);
        SotcStats::StallAgg(SotcStats::StallGuestCopyWait, 0, ms, 0.0);
        SotcStats::StallNote(ms, "submit waited %.1f ms for the guest copies", ms);
    }
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    // sotc: a current write is recorded only on the two paths that write the arena range. Recording
    // it unconditionally marked unwritten ranges as written by the command buffer being recorded,
    // which sent their readbacks to the slower flushing path instead of the ahead path.
    if (SotcStats::OptOff()) {
        RecordCurrentWrite(device_addr, size);
    }
    if (SotcStats::kFpsLog) { // sotc: Phase 1 counters (how many formatted bindings find an image)
        SotcStats::fmt_sync_calls.fetch_add(1, std::memory_order_relaxed);
    }
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (SotcStats::kFpsLog) {
            SotcStats::fmt_sync_meta.fetch_add(1, std::memory_order_relaxed);
        }
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            RecordCurrentWrite(device_addr, size);
            return true;
        } else if (static u32 shown = 0; shown < 4) {
            // sotc: first few only. In PS4 Pro performance mode the game reads CMask surfaces as
            // texel buffers hundreds of times a second; each stderr line (piped to the launcher)
            // took milliseconds on the draw thread: 28 vs 16 ms per frame in the same scene.
            ++shown;
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {} (logged {} of 4)",
                        magic_enum::enum_name(*type), shown);
        }
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        return false;
    }
    if (SotcStats::kFpsLog) {
        SotcStats::fmt_sync_image.fetch_add(1, std::memory_order_relaxed);
    }
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    RecordCurrentWrite(device_addr, size);
    return true;
}

std::pair<vk::DeviceMemory, u64> BufferCache::AllocateArenaBlocks(u64 blocks) {
    // Room in a chunk of video memory, then in a chunk of system memory.
    const auto take = [&](u32 memory_type) -> std::optional<std::pair<vk::DeviceMemory, u64>> {
        for (auto& chunk : arena_chunks) {
            if (chunk->memory_type != memory_type) {
                continue;
            }
            for (auto it = chunk->free_runs.begin(); it != chunk->free_runs.end(); ++it) {
                if (it->second < blocks) {
                    continue;
                }
                const auto [start, length] = *it;
                chunk->free_runs.erase(it);
                if (length > blocks) {
                    chunk->free_runs.emplace(start + blocks, length - blocks);
                }
                chunk->used_blocks += blocks;
                return std::make_pair(chunk->memory, start);
            }
        }
        return std::nullopt;
    };
    const auto grow = [&](u32 memory_type, bool exact = false)
        -> std::optional<std::pair<vk::DeviceMemory, u64>> {
        // exact: only the asked size, for a nearly full card that has no 64 MiB left (#137).
        const u64 chunk_blocks = exact ? blocks : std::max<u64>(ARENA_CHUNK_SIZE >> block_shift, blocks);
        const vk::MemoryAllocateInfo alloc_info = {
            .allocationSize = chunk_blocks << block_shift,
            .memoryTypeIndex = memory_type,
        };
        const auto stall_t0 = SotcStats::kStallLog ? std::chrono::steady_clock::now()
                                                   : std::chrono::steady_clock::time_point{};
        const auto [result, memory] = instance.GetDevice().allocateMemory(alloc_info);
        if (SotcStats::kStallLog) {
            SotcStats::StallNote(SotcStats::StallMsSince(stall_t0),
                                 "arena-chunk vkAllocateMemory %.1f ms: %.1f MB type %u (%s)",
                                 SotcStats::StallMsSince(stall_t0),
                                 alloc_info.allocationSize / 1048576.0, memory_type,
                                 result == vk::Result::eSuccess ? "ok" : "failed");
        }
        if (result != vk::Result::eSuccess) {
            return std::nullopt;
        }
        SotcStats::arena_allocs.fetch_add(1, std::memory_order_relaxed);
        SotcStats::arena_bytes.fetch_add(alloc_info.allocationSize, std::memory_order_relaxed);
        auto& chunk = arena_chunks.emplace_back(std::make_unique<ArenaChunk>());
        chunk->memory = memory;
        chunk->memory_type = memory_type;
        chunk->free_runs.emplace(0, chunk_blocks);
        chunk_by_memory.emplace(static_cast<VkDeviceMemory>(memory), chunk.get());
        return take(memory_type);
    };
    if (auto placed = take(arena_memory_type_index)) {
        return *placed;
    }
    if (auto placed = grow(arena_memory_type_index)) {
        return *placed;
    }
    if (arena_fallback_memory_type_index) {
        // Keep running from system memory (slower GPU access) instead of aborting.
        static bool warned = false;
        if (!std::exchange(warned, true)) {
            LOG_WARNING(Render, "Video memory exhausted, placing new buffer memory in system RAM");
        }
        if (auto placed = take(*arena_fallback_memory_type_index)) {
            return *placed;
        }
        if (auto placed = grow(*arena_fallback_memory_type_index)) {
            return *placed;
        }
    }
    // Drop old cached images, wait for the GPU so their memory is really released, try again.
    LOG_WARNING(Render,
                "Video memory exhausted, collecting cached images (asked {} KB; in use {} MB of {} "
                "MB; arenas {} chunks, {} MB)",
                (blocks << block_shift) >> 10, instance.GetDeviceMemoryUsage() >> 20,
                SotcStats::vram_budget.load() >> 20, SotcStats::arena_allocs.load(),
                SotcStats::arena_bytes.load() >> 20);
    texture_cache.EmergencyCollect();
    scheduler.Finish();
    scheduler.PopPendingOperations();
    if (auto placed = grow(arena_memory_type_index)) {
        return *placed;
    }
    // sotc: still no room for a whole chunk (#137: 6 GB cards): the asked size alone, then
    // system memory, before giving up.
    if (auto placed = grow(arena_memory_type_index, true)) {
        return *placed;
    }
    if (arena_fallback_memory_type_index) {
        if (auto placed = grow(*arena_fallback_memory_type_index, true)) {
            return *placed;
        }
    }
    LOG_CRITICAL(Render, "Out of video and system memory for buffers; lower the video memory use "
                         "setting in the launcher (Economy)");
    ASSERT_MSG(false, "Failed to allocate buffer arena memory");
    return {};
}

u64 BufferCache::ArenaUsedBytes() const noexcept {
    u64 blocks = 0;
    for (const auto& chunk : arena_chunks) {
        blocks += chunk->used_blocks;
    }
    return blocks << block_shift;
}

void BufferCache::FreeArenaBlocks(vk::DeviceMemory memory, u64 start, u64 blocks) {
    const auto found = chunk_by_memory.find(static_cast<VkDeviceMemory>(memory));
    if (found == chunk_by_memory.end()) {
        return;
    }
    ArenaChunk& chunk = *found->second;
    chunk.used_blocks -= std::min(chunk.used_blocks, blocks);
    // Give the run back, merged with free neighbours.
    auto next = chunk.free_runs.lower_bound(start);
    if (next != chunk.free_runs.end() && next->first == start + blocks) {
        blocks += next->second;
        next = chunk.free_runs.erase(next);
    }
    if (next != chunk.free_runs.begin()) {
        const auto prev = std::prev(next);
        if (prev->first + prev->second == start) {
            start = prev->first;
            blocks += prev->second;
            chunk.free_runs.erase(prev);
        }
    }
    chunk.free_runs.emplace(start, blocks);
    if (chunk.used_blocks != 0) {
        return;
    }
    // Empty: give the memory back. The unbind is submitted before this tick's work, which
    // waits for it.
    SotcStats::arena_allocs.fetch_sub(1, std::memory_order_relaxed);
    chunk_by_memory.erase(found);
    std::erase_if(arena_chunks, [&](const auto& c) {
        if (c->memory != memory) {
            return false;
        }
        u64 size = 0;
        for (const auto& [run_start, run_blocks] : c->free_runs) {
            size += run_blocks;
        }
        SotcStats::arena_bytes.fetch_sub(size << block_shift, std::memory_order_relaxed);
        return true;
    });
    scheduler.DeferOperation([device = instance.GetDevice(), memory] { device.freeMemory(memory); });
}

void BufferCache::ReleaseMemory(VAddr device_addr, u64 size) {
    // Only whole blocks inside the unmapped range can go; partial blocks may back neighbours.
    const u64 first_block = Common::AlignUp(device_addr, u64{block_size}) >> block_shift;
    const u64 end_block = (device_addr + size) >> block_shift;
    if (first_block >= end_block) {
        return;
    }
    evicted_ranges.Subtract(first_block, end_block);
    pinned_ranges.Subtract(first_block, end_block);
    boost::container::small_vector<Backing, 16> released;
    const auto collect = [&] {
        released.clear();
        resident_ranges.ForEachInRange(first_block, end_block, [&](const Backing& backing) {
            const u64 start = std::max(first_block, backing.start);
            const u64 end = std::min(end_block, backing.end);
            if (start < end) {
                released.push_back(backing.SubRange(start, end));
            }
        });
    };
    collect();
    if (released.empty()) {
        return;
    }
    // Work recorded before the unmap may still write this memory, and sparse binds are not
    // ordered against earlier submissions: finish that work before unbinding, or the GPU
    // writes to unbound memory (WriteInvalid, device lost). Unmaps are rare, the sync is cheap.
    scheduler.Finish();
    // sotc: the deferred operations Finish ran (fault buffer rebinds) may have bound more of
    // the range, and their binds must not share the unbind's sparse batch.
    collect();
    if (!pending_binds.empty()) {
        scheduler.Flush();
    }
    UnbindResident(released);
    std::erase_if(hot_readbacks, [&](const HotReadback& hot) {
        return hot.addr < (end_block << block_shift) &&
               (first_block << block_shift) < hot.addr + hot.size;
    });
    resident_ranges.Subtract(first_block, end_block);
    ++resident_version; // sotc: invalidates the merged DMA runs
    {
        std::unique_lock gpu_modified_lock{gpu_modified_mutex};
        gpu_modified_ranges.Subtract(first_block << block_shift,
                                     (end_block - first_block) << block_shift);
    }
    // sotc: a bind of the same blocks (the guest maps them again) must not share the unbind's
    // sparse bind batch: binds of one range within a batch are not ordered (the evicted blocks
    // of MaybeEvictCold came back unbound: black scenes).
    scheduler.Flush();
}

void BufferCache::UnbindResident(std::span<const Backing> released) {
    for (const Backing& range : released) {
        // sotc: unbound blocks start again unclassified when they become resident.
        std::memset(block_class + range.start, 0, range.end - range.start);
        // Split at arena page boundaries, each page may belong to a different arena.
        for (u64 start = range.start; start < range.end;) {
            const u64 page = start >> blocks_per_arena_page_shift;
            const u64 end = std::min(range.end, (page + 1) << blocks_per_arena_page_shift);
            if (const Buffer* arena = address_space[page]) {
                BindsForArena(arena)->binds.push_back(vk::SparseMemoryBind{
                    .resourceOffset = (start << block_shift) - arena->cpu_addr,
                    .size = (end - start) << block_shift,
                    .memory = {},
                    .memoryOffset = 0,
                });
            }
            start = end;
        }
        // Shaders reach arena memory through the BDA page table; a null entry faults the block
        // back in (fault buffer) instead of touching unbound memory.
        runtime.FillBuffer(bda_pagetable_buffer.get(), range.start * sizeof(vk::DeviceAddress),
                           (range.end - range.start) * sizeof(vk::DeviceAddress), 0u);
        FreeArenaBlocks(range.memory, range.offset, range.end - range.start);
    }
}

bool BufferCache::MaybeEvictCold(std::chrono::steady_clock::time_point now) {
    // sotc: the game keeps its 4 GiB direct memory pool mapped and hands its addresses out
    // again and again, so arena blocks became resident once and stayed: ~45 MB/min in the
    // attract demo, over the 6 GB card's budget after ~40 minutes. Once resident arena memory
    // passes SOTC_ARENA_EVICT_MB (default 1536, 0 = never), allocations (4 MiB slabs or exact
    // ones) whose blocks were not bound for SOTC_ARENA_EVICT_IDLE_S (default 30) seconds and
    // hold no GPU-modified data go back, coldest first, up to SOTC_ARENA_EVICT_STEP_MB (default
    // 64) per check (every 2 s). Their guest pages are marked CPU-modified, so the next bind
    // uploads them again.
    // The arena does not always match guest memory on pages the tracker calls clean (attract
    // demo: 0-13,000 of 2,000-65,000 pages per run; mostly not image memory). Uploading guest
    // memory over those blanked whole scenes, so each allocation is read back first and goes
    // only when every page that is not CPU-modified is equal (a mismatch is tried again after
    // 60 s). Shaders reading arena memory through pointers fault an evicted block back in (one
    // frame of zeros); blocks that came back that way stay resident from then on.
    static const u64 limit = [] {
        const char* v = std::getenv("SOTC_ARENA_EVICT_MB");
        static constexpr u64 tier_mb[4] = {1024, 1536, 4096, 0}; // sotc_stats.h VramTier
        return (v ? std::strtoull(v, nullptr, 10) : tier_mb[SotcStats::VramTier()]) << 20;
    }();
    static const u64 step = [] {
        const char* v = std::getenv("SOTC_ARENA_EVICT_STEP_MB");
        return std::max<u64>(v ? std::strtoull(v, nullptr, 10) : 64ull, 1) << 20;
    }();
    static const u16 idle_epochs = [] {
        const char* v = std::getenv("SOTC_ARENA_EVICT_IDLE_S");
        return static_cast<u16>(std::clamp<unsigned long>(v ? std::strtoul(v, nullptr, 10) : 30ul,
                                                          2ul, 30000ul));
    }();
    // sotc: BbToggle::ArenaImageRelease (SOTC_ARENA_IMG_IDLE_S, default 30; 0 = off): the arena
    // copy RefreshImage makes of a big, re-uploaded image has no reader but the next upload of
    // that image. Blocks that only image uploads ever bound (block_class 1) and that nothing bound
    // for that long go back whatever the arena size, through the same read-back comparison, so
    // what leaves is exactly guest memory and the next upload copies it again whole.
    static const u16 img_idle_epochs = [] {
        const char* v = std::getenv("SOTC_ARENA_IMG_IDLE_S");
        const unsigned long s = v ? std::strtoul(v, nullptr, 10) : 30ul;
        return static_cast<u16>(s == 0 ? 0 : std::clamp<unsigned long>(s, 2ul, 30000ul));
    }();
    if (pending_eviction) {
        AdvanceUseEpoch(now);
        return FinishEviction(now, false);
    }
    if (now < next_evict_check) {
        return false;
    }
    next_evict_check = now + std::chrono::seconds(2);
    AdvanceUseEpoch(now);
    const u64 resident = ArenaUsedBytes();
    SotcStats::arena_used.store(resident, std::memory_order_relaxed);
    if (SotcStats::kFpsLog) {
        ArenaClassCensus(img_idle_epochs ? img_idle_epochs : 30);
    }
    const bool over = limit != 0 && resident > limit;
    const bool img_release = img_idle_epochs != 0 && !BbToggle::Disabled(BbToggle::ArenaImageRelease);
    if (!over && !img_release) {
        return false;
    }
    const SotcStats::ScopedTimer evict_timer{SotcStats::arena_evict_ns};
    // sotc: with the 64 MiB chunks the unit is a resident range (its blocks go back to their
    // chunk, and a chunk left empty to the driver), not a whole allocation.
    struct Candidate {
        u16 age{0xFFFF}; // youngest block
        bool keep{};
        bool img_only{}; // sotc: every block bound only as an image source
        u64 bytes{};
        boost::container::small_vector<Backing, 4> ranges;
    };
    std::unordered_map<u64, Candidate> candidates;
    std::erase_if(evict_retry, [&](const auto& entry) { return entry.second <= now; });
    std::vector<std::pair<u16, u64>> order;
    for (const Backing& range : resident_ranges) {
        // sotc: ArenaImageRelease: a range whose blocks are all class 1 goes after the shorter idle
        // time; with the arena under its threshold nothing else is a candidate.
        bool img_only = img_release;
        for (u64 block = range.start; img_only && block < range.end; ++block) {
            img_only = block_class[block] == 1;
        }
        if (!over && !img_only) {
            continue;
        }
        const u16 need_idle = img_only ? img_idle_epochs : idle_epochs;
        auto& candidate = candidates[range.start];
        candidate.img_only = img_only;
        for (u64 block = range.start; block < range.end && !candidate.keep; ++block) {
            const u16 age = static_cast<u16>(use_epoch - block_last_use[block]);
            candidate.age = std::min(candidate.age, age);
            candidate.keep = age < need_idle;
        }
        const VAddr addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        candidate.keep = candidate.keep || pinned_ranges.Overlaps(range.start, range.end) ||
                         memory_tracker->IsRegionGpuModified(addr, size) ||
                         [&] {
                             std::shared_lock gpu_modified_lock{gpu_modified_mutex};
                             return gpu_modified_ranges.Intersects(addr, size);
                         }() ||
                         evict_retry.contains(range.start) || size > step;
        candidate.bytes = size;
        candidate.ranges.push_back(range);
        if (!candidate.keep) {
            order.emplace_back(candidate.age, range.start);
        }
    }
    if (order.empty()) {
        next_evict_check = now + std::chrono::seconds(10); // all of it in use: look again later
        return false;
    }
    std::ranges::sort(order, std::greater{});

    // Read the coldest allocations back, up to `step` bytes of blocks.
    std::vector<EvictChoice> chosen;
    u64 total = 0;
    for (const auto& [age, key] : order) {
        if (total >= step || (over && resident - total <= limit / 8 * 7)) {
            break;
        }
        const Candidate& candidate = candidates[key];
        EvictChoice& pick = chosen.emplace_back(EvictChoice{key, candidate.bytes, {}, candidate.ranges,
                                                            candidate.img_only,
                                                            candidate.img_only ? img_idle_epochs
                                                                               : idle_epochs});
        for (const Backing& range : candidate.ranges) {
            // Split at arena page boundaries, each page may belong to a different arena.
            for (u64 start = range.start; start < range.end;) {
                const u64 page = start >> blocks_per_arena_page_shift;
                const u64 end = std::min(range.end, (page + 1) << blocks_per_arena_page_shift);
                pick.pieces.push_back({start << block_shift, (end - start) << block_shift, total});
                total += (end - start) << block_shift;
                start = end;
            }
        }
    }
    const auto download = staging_pool.Request(total, MemoryType::HostCached, 0, true);
    for (const EvictChoice& pick : chosen) {
        for (const EvictPiece& piece : pick.pieces) {
            const Buffer* arena = address_space[piece.addr >> ARENA_PAGE_BITS];
            if (!arena) {
                continue; // never resident: the comparison fails and the allocation stays
            }
            const vk::BufferCopy copy{arena->Offset(piece.addr), download.offset + piece.offset,
                                      piece.size};
            runtime.CopyBuffer(arena, download.buffer, std::span{&copy, 1});
        }
    }
    pending_eviction = PendingEviction{std::move(chosen), download, scheduler.CurrentTick(),
                                       now + std::chrono::seconds(5), over};
    // sotc: SOTC_EVICT_ASYNC=0 waits for the read-back here, as before: scheduler.Finish() on the
    // command processor, ~50 ms per 64 MiB run, one late frame each.
    static const bool async = [] {
        const char* v = std::getenv("SOTC_EVICT_ASYNC");
        return !v || v[0] != '0';
    }();
    return async ? false : FinishEviction(now, true);
}

bool BufferCache::FinishEviction(std::chrono::steady_clock::time_point now, bool wait) {
    // sotc: the unbind's sparse batch does not wait for earlier submits, so it goes only once
    // every command buffer submitted so far (the read-back's included) is done. The GPU catches
    // up within a frame or two at the benchmark scenes; after 5 s, or with `wait`, it is waited for.
    PendingEviction& pending = *pending_eviction;
    if (!wait && now < pending.deadline &&
        (!scheduler.IsFree(pending.tick) || !scheduler.IsFree(scheduler.CurrentTick() - 1))) {
        return false;
    }
    // Draws recorded since the last submit would run after the unbind and before the BDA page
    // table fill below (reads of unbound memory, no fault): Finish() submits and waits for them.
    scheduler.Finish();
    const SotcStats::ScopedTimer evict_timer{SotcStats::arena_evict_ns};
    const auto& download = pending.download;
    download.buffer->Invalidate(download.offset, download.size);

    std::vector<Backing> released;
    std::array<u8, 4096> guest;
    std::unordered_set<u64> img_keys; // sotc: released ranges that were image sources only
    for (const EvictChoice& pick : pending.chosen) {
        // Bound again, or written by the GPU, after the read-back was recorded: stays.
        bool keep = false;
        for (const Backing& range : pick.ranges) {
            for (u64 block = range.start; block < range.end && !keep; ++block) {
                keep = static_cast<u16>(use_epoch - block_last_use[block]) < pick.need_idle;
            }
            const VAddr addr = range.start << block_shift;
            const u64 size = (range.end - range.start) << block_shift;
            keep = keep || pinned_ranges.Overlaps(range.start, range.end) ||
                   memory_tracker->IsRegionGpuModified(addr, size) || [&] {
                       std::shared_lock gpu_modified_lock{gpu_modified_mutex};
                       return gpu_modified_ranges.Intersects(addr, size);
                   }();
        }
        if (keep) {
            continue;
        }
        bool equal = true;
        for (const EvictPiece& piece : pick.pieces) {
            for (u64 at = 0; at < piece.size && equal; at += guest.size()) {
                const VAddr addr = piece.addr + at;
                if (memory_tracker->IsRegionCpuModified(addr, guest.size())) {
                    continue; // uploaded at the next bind anyway
                }
                if (!memory->TryCopySparseMemory(addr, guest.data(), guest.size())) {
                    continue; // unmapped: nothing can read it
                }
                equal = std::memcmp(download.mapped + piece.offset + at, guest.data(),
                                    guest.size()) == 0;
            }
        }
        if (!equal) {
            evict_retry.emplace(pick.key, now + std::chrono::seconds(60));
            SotcStats::arena_evict_mismatch.fetch_add(pick.bytes, std::memory_order_relaxed);
            continue;
        }
        released.insert(released.end(), pick.ranges.begin(), pick.ranges.end());
        if (pick.img_only) {
            img_keys.insert(pick.key);
        }
    }
    const bool over = pending.over;
    staging_pool.FreeDeferred(download);
    pending_eviction.reset();
    // Binds queued by Finish's deferred operations (fault rebinds, arena migration) must not
    // share the unbind's sparse batch; the chosen ranges must still be resident as chosen.
    if (!pending_binds.empty()) {
        scheduler.Flush();
    }
    std::erase_if(released, [&](const Backing& range) {
        bool same = false;
        resident_ranges.ForEachInRange(range.start, range.end, [&](const Backing& now_range) {
            same |= now_range.start <= range.start && range.end <= now_range.end &&
                    now_range.memory == range.memory;
        });
        if (same && img_keys.contains(range.start)) {
            // sotc: the GPU wait above let other threads bind: a block that also got a bind of
            // another class since the scan is no longer an image source only.
            for (u64 block = range.start; same && block < range.end; ++block) {
                same = block_class[block] == 1;
            }
        }
        return !same;
    });
    for (const Backing& range : released) {
        const VAddr addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        memory_tracker->MarkRegionAsCpuModified(addr, size);
        evicted_ranges.Add({range.start, range.end});
        SotcStats::arena_evicted.fetch_add(size, std::memory_order_relaxed);
        if (img_keys.contains(range.start)) {
            SotcStats::arena_img_released.fetch_add(size, std::memory_order_relaxed);
        }
        std::erase_if(hot_readbacks, [&](const HotReadback& hot) {
            return hot.addr < addr + size && addr < hot.addr + hot.size;
        });
        resident_ranges.Subtract(range.start, range.end);
    }
    if (!released.empty()) {
        UnbindResident(released);
        ++resident_version; // sotc: invalidates the merged DMA runs and the resident cache
        // A bind of the same blocks recorded before the next submit would share one sparse
        // bind batch with the unbind; submit the unbind on its own.
        scheduler.Flush();
    }
    SotcStats::arena_evict_runs.fetch_add(1, std::memory_order_relaxed);
    if (!over) {
        // sotc: an image-source release alone is at most every 10 s (it waits for the GPU).
        SotcStats::arena_img_runs.fetch_add(1, std::memory_order_relaxed);
        next_evict_check = now + std::chrono::seconds(10);
    }
    return true;
}

// sotc: arena bytes by class of the blocks' binds (see block_class), for the SOTC MEM line.
void BufferCache::ArenaClassCensus(u16 idle_epochs) {
    u64 image_only = 0, other_only = 0, mixed = 0, image_idle = 0, other_idle = 0;
    for (const Backing& range : resident_ranges) {
        for (u64 block = range.start; block < range.end; ++block) {
            switch (block_class[block]) {
            case 1:
                ++image_only;
                if (static_cast<u16>(use_epoch - block_last_use[block]) >= idle_epochs) {
                    ++image_idle;
                }
                break;
            case 2:
                ++other_only;
                if (static_cast<u16>(use_epoch - block_last_use[block]) >= idle_epochs) {
                    ++other_idle;
                }
                break;
            case 3:
                ++mixed;
                break;
            default:
                break;
            }
        }
    }
    SotcStats::arena_img_only.store(image_only << block_shift, std::memory_order_relaxed);
    SotcStats::arena_other_only.store(other_only << block_shift, std::memory_order_relaxed);
    SotcStats::arena_mixed.store(mixed << block_shift, std::memory_order_relaxed);
    SotcStats::arena_img_idle.store(image_idle << block_shift, std::memory_order_relaxed);
    SotcStats::arena_other_idle.store(other_idle << block_shift, std::memory_order_relaxed);
}

void BufferCache::NoteFaultRebind(VAddr addr, u64 size) {
    const u64 first_block = addr >> block_shift;
    const u64 end_block = (addr + size + block_size - 1) >> block_shift;
    evicted_ranges.ForEachInRange(first_block, end_block, [&](const Interval& evicted) {
        const u64 start = std::max(first_block, evicted.start);
        const u64 end = std::min(end_block, evicted.end);
        pinned_ranges.Add({start, end});
        SotcStats::arena_pinned.fetch_add((end - start) << block_shift, std::memory_order_relaxed);
    });
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    // sotc: the staging memory the recorded copies read must be complete before the GPU can run them.
    WaitGuestCopies();
    // sotc: binds stamp blocks with use_epoch; keep it current even when no eviction check runs.
    AdvanceUseEpoch(std::chrono::steady_clock::now());
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    // Queue access is serialized by submit_mutex, which the scheduler no longer holds here.
    std::scoped_lock lk{Vulkan::Scheduler::submit_mutex};
    const auto bind_start = std::chrono::steady_clock::now();
    auto submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");
    // sotc: a bind of many ranges (an arena migration rebinds all of them) can take long.
    if (const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - bind_start)
                            .count();
        ms >= 20) {
        size_t num_binds = 0;
        for (const auto& binds : pending_binds) {
            num_binds += binds.binds.size();
        }
        LOG_WARNING(Render, "Sparse bind of {} ranges in {} arenas took {} ms", num_binds,
                    pending_binds.size(), ms);
    }

    pending_binds.clear();
}

} // namespace VideoCore
