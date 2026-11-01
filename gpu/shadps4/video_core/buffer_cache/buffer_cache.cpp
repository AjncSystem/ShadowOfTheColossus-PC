// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <magic_enum/magic_enum.hpp>

#include "bbport_copy.h"
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
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

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
    std::free(block_last_use);
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
}

void BufferCache::DropGpuModified(VAddr device_addr, u64 size) {
    liverpool->SendCommand<true>([this, device_addr, size] {
        {
            std::unique_lock gpu_modified_lock{gpu_modified_mutex};
            gpu_modified_ranges.Subtract(device_addr, size);
        }
        memory_tracker->UnmarkRegionAsGpuModified(device_addr, size, false);
        MarkInflightReadbacksDirty(device_addr, size);
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
        if (is_write) {
            MarkWrittenByGuest(device_addr, size);
        }
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
    // sotc: SOTC_RB_REQUEUE=0 re-reads a dirtied window synchronously at finish (submit and
    // drain on the command processor, the old behavior). On (default), the finish hands the
    // dirtied window back to this thread, which begins a fresh readback of it: another GPU
    // round trip for the guest, no drain for the frame. Two retries, then the sync path.
    static const int max_requeues = [] {
        const char* v = std::getenv("SOTC_RB_REQUEUE");
        return !v || v[0] != '0' ? 2 : 0;
    }();
    for (int tries = 0;; ++tries) {
        AsyncReadback readback{.device_addr = device_addr, .size = size, .is_write = is_write};
        liverpool->SendCommand<true>([this, &readback] { BeginAsyncReadback(readback); });
        if (!readback.waiting) {
            return;
        }
        scheduler.WaitSubmitted(readback.tick, false);
        scheduler.GetWorkSemaphore()->Wait(readback.tick);
        bool requeue = false;
        const bool allow = tries < max_requeues;
        liverpool->SendCommand<true>(
            [this, &readback, &requeue, allow] { requeue = FinishAsyncReadback(readback, allow); });
        if (!requeue) {
            return;
        }
        SotcStats::rb_requeues.fetch_add(1, std::memory_order_relaxed);
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
    NoteHotRead(readback.device_addr, readback.size);
    const u32 first_block = readback.device_addr >> block_shift;
    const u32 last_block = (readback.device_addr + readback.size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    const auto [window_start, window_end] =
        ReadbackWindow(arena, readback.device_addr, readback.size);
    readback.arena = arena;
    readback.window_start = window_start;
    readback.window_size = window_end - window_start;
    const u64 total =
        GatherDownloads(arena, window_start, readback.window_size, readback.pending, false);
    if (total == 0) {
        // Its bytes may belong to a readback still in flight (taken out of the GPU-modified set,
        // pages still protected): wait for that one and finish it if its thread has not yet.
        if (const AsyncReadback* other = FindInflightReadback(readback.device_addr, readback.size)) {
            readback.tick = other->tick;
            readback.waiting = true;
            readback.dependent = true;
            return;
        }
        if (readback.is_write) {
            MarkWrittenByGuest(readback.device_addr, readback.size);
        }
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
    inflight_readbacks.push_back(&readback);
    SotcStats::readback_finish.fetch_add(1, std::memory_order_relaxed);
}

void BufferCache::MarkInflightReadbacksDirty(VAddr addr, u64 size) {
    // sotc: the command processor (a GPU write, its own write or an applied readback after
    // which the guest may write) changed these bytes: asynchronous readbacks of them in flight
    // hold stale data (FinishAsyncReadback reads them back again).
    for (AsyncReadback* readback : inflight_readbacks) {
        for (auto& item : readback->pending) {
            if (addr < item.addr + item.size && item.addr < addr + size) {
                item.dirty = true;
                readback->dirty = true;
            }
        }
    }
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

bool BufferCache::FinishAsyncReadback(AsyncReadback& readback, bool allow_requeue) {
    if (readback.dependent) {
        // Finish the readbacks this one waited for (owners still blocked keep them alive).
        // Their owner cannot retry a readback finished on its behalf: no requeue here.
        while (AsyncReadback* other = FindInflightReadback(readback.device_addr, readback.size)) {
            FinishAsyncReadback(*other);
        }
        if (readback.is_write) {
            MarkWrittenByGuest(readback.device_addr, readback.size);
        }
        return false;
    }
    if (readback.finished) {
        if (readback.is_write) {
            MarkWrittenByGuest(readback.device_addr, readback.size);
        }
        return false;
    }
    readback.finished = true;
    std::erase(inflight_readbacks, &readback);
    const Vulkan::StagingBufferRef download{
        .buffer = readback.download_buffer,
        .offset = readback.download_offset,
        .size = readback.download_size,
        .mapped = const_cast<u8*>(readback.download_mapped),
        .type = VideoCore::MemoryType::HostCached,
    };
    // sotc: SOTC_RB_PART=0 goes back to re-reading the whole window when any of it was
    // re-written in flight. On: only the regions a GPU write actually landed on are stale.
    // The clean ones are applied as usual; a stale hot bring-along is dropped (its ranges
    // never left the GPU-modified set and its pages stay read-protected, so its own fault
    // reads it back); the window is re-read only when it was hit itself.
    static const bool partial_finish = [] {
        const char* v = std::getenv("SOTC_RB_PART");
        return !v || v[0] != '0';
    }();
    if (readback.dirty && partial_finish) {
        download.buffer->Invalidate(download.offset, download.size);
        boost::container::small_vector<ReadbackItem, 16> clean;
        bool window_dirty = false;
        for (auto& item : readback.pending) {
            if (!item.dirty) {
                clean.push_back(std::move(item));
            } else if (!item.hot) {
                window_dirty = true;
            } else {
                SotcStats::rb_dirty_hot_dropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
        ApplyDownloads(clean, download.mapped, download.offset);
        SubtractDownloaded(clean);
        staging_pool.FreeDeferred(download);
        if (window_dirty) {
            SotcStats::readback_dirty_retries.fetch_add(1, std::memory_order_relaxed);
            if (allow_requeue) {
                // The window's ranges never left the GPU-modified set and its pages stay
                // protected: the guest begins a fresh readback of it instead of a sync drain.
                return true;
            }
            DownloadMemory(readback.arena, readback.window_start, readback.window_size);
        }
    } else if (readback.dirty) {
        // A GPU write recorded after the copies (or another readback applied first and the
        // guest may have written since): the data may be stale. The ranges never left the
        // GPU-modified set; read the window back the usual way.
        SotcStats::readback_dirty_retries.fetch_add(1, std::memory_order_relaxed);
        staging_pool.FreeDeferred(download);
        DownloadMemory(readback.arena, readback.window_start, readback.window_size);
    } else {
        download.buffer->Invalidate(download.offset, download.size);
        // sotc: the ranges leave the GPU-modified set only once the backing holds their data:
        // the command processor's clean reads (ReadClean) would read the stale backing before.
        ApplyDownloads(readback.pending, download.mapped, download.offset);
        SubtractDownloaded(readback.pending);
        staging_pool.FreeDeferred(download);
    }
    if (readback.is_write) {
        MarkWrittenByGuest(readback.device_addr, readback.size);
    }
    return false;
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    SotcStats::readbacks.fetch_add(1, std::memory_order_relaxed);
    const SotcStats::ScopedTimer readback_timer{SotcStats::readback_ns};
    const SotcStats::FrameTimer frame_timer{SotcStats::FrameReadback};
    boost::container::small_vector<ReadbackItem, 16> pending;
    const u64 total_size_bytes = GatherDownloads(arena, device_addr, size, pending);
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
    bool written_hot = false;
    bool written_window = false;
    for (const auto& item : pending) {
        for (const auto& copy : item.copies) {
            if (current_writes.Intersects(item.arena->cpu_addr + copy.srcOffset, copy.size)) {
                written_by_current = true;
                (item.hot ? written_hot : written_window) = true;
            }
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
        // Did the guest ask for bytes the command buffer being built writes, or only a neighbour
        // in the window (or a hot region) did? The split names what keeps the ahead path idle.
        if (fault_size != 0 && written_by_current &&
            current_writes.Intersects(fault_addr, fault_size)) {
            SotcStats::readback_finish_asked.fetch_add(1, std::memory_order_relaxed);
        } else if (!written_by_current) {
            SotcStats::readback_finish_binds.fetch_add(1, std::memory_order_relaxed);
        } else if (written_window) {
            SotcStats::readback_finish_window.fetch_add(1, std::memory_order_relaxed);
        } else if (written_hot) {
            SotcStats::readback_finish_hot.fetch_add(1, std::memory_order_relaxed);
        }
        for (const auto& item : pending) {
            runtime.CopyBuffer(item.arena, download.buffer, item.copies);
        }
        scheduler.FinishHostRead();
    }

    download.buffer->Invalidate(download.offset, download.size);
    // sotc: GPU output of dispatches on stale descriptors must not overwrite the allocator
    // metadata of the game's BPE heap (bpe_heap_guard.cpp, from Pink-shadPS4; the loader finds
    // the heap, SOTC_HEAP_GUARD=0 turns it off).
    // The free tree is walked again only when it changed: its root (heap+0x80), node count
    // (heap+0xa8, kept by the game's tree code) and pending block (heap+0xc0) sign it. Block
    // headers are followed through the physical chain on every write anyway. Only the command
    // processor thread downloads memory.
    ApplyDownloads(pending, download.mapped, download.offset);
    SubtractDownloaded(pending); // sotc: after the backing holds the data (see FinishAsyncReadback)
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
                                 bool subtract) {
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
    const bool hot_enabled = hot_mode != 0;
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

void BufferCache::ApplyDownloads(std::span<const ReadbackItem> pending, const u8* mapped,
                                 u64 mapped_offset) {
    // sotc (#146): every heap of the guarded heap's class, not only the one picked at boot: the
    // heaps a world creates when it loads (the last colossus' area) crashed the same way
    // (eboot+0x49500, freed blocks overwritten). They come from the engine's heap table.
    // sotc: SOTC_HEAP_FAST (default on): a write into an in-use block a guard found before, or
    // above the in-use, size-0 block that ends the heap's block chain (BpeUsedBlocks), skips the
    // guard, so a guard (its build walks the whole free tree) is built only for the other writes.
    // SOTC_HEAP_FAST=0 builds every touched heap's guard for each readback batch.
    static const bool heap_fast = [] {
        const char* v = std::getenv("SOTC_HEAP_FAST");
        return !v || std::atoi(v) != 0;
    }();
    struct GuardSlot {
        VAddr heap = 0;
        std::array<u64, 3> signature{};
        std::chrono::steady_clock::time_point built{};
        std::optional<BpeHeapGuard> guard;
        std::optional<BpeUsedBlocks> used;
    };
    static std::array<GuardSlot, 8> slots;
    struct Active {
        GuardSlot* slot;
        VAddr base;
        u64 size;
        bool checked; ///< the guard was checked against the heap's signature in this batch
    };
    std::array<Active, 8> active{};
    size_t active_count = 0;
    std::array<VAddr, 8> heaps{};
    size_t heap_count = 0;
    if (const VAddr chosen = runtime_sotc_heap(); chosen != 0) {
        heaps[heap_count++] = chosen;
        const u64 heap_class = *std::bit_cast<const u64*>(chosen);
        if (const VAddr table = runtime_sotc_heap_table(); table != 0) {
            for (u32 id = 1; id < 48 && heap_count < heaps.size(); ++id) {
                const VAddr other = std::bit_cast<const VAddr*>(table)[id];
                if (other == 0 || other == chosen || !(other >= 0x10000 && other < 0x10000000000ULL)) {
                    continue;
                }
                const bool in_image = other - table < 0x20000000;
                if (!in_image && !memory->GetBackingPointer(other)) {
                    continue;
                }
                if (*std::bit_cast<const u64*>(other) == heap_class) {
                    heaps[heap_count++] = other;
                }
            }
        }
    }
    const auto find_slot = [&](VAddr heap) -> GuardSlot* {
        for (auto& candidate : slots) {
            if (candidate.heap == heap) {
                return &candidate;
            }
        }
        for (auto& candidate : slots) { // a free slot, else one of a heap not seen now
            if (candidate.heap == 0 ||
                std::find(heaps.begin(), heaps.begin() + heap_count, candidate.heap) ==
                    heaps.begin() + heap_count) {
                candidate.heap = heap;
                candidate.guard.reset();
                candidate.used.reset();
                return &candidate;
            }
        }
        return nullptr;
    };
    const auto signature_of = [](VAddr heap) -> std::array<u64, 3> {
        return {*std::bit_cast<const u64*>(heap + 0x80), *std::bit_cast<const u64*>(heap + 0xa8),
                *std::bit_cast<const u64*>(heap + 0xc0)};
    };
    for (size_t h = 0; h < heap_count; ++h) {
        const VAddr heap = heaps[h];
        // Only readbacks into the heap's own memory need it: building the guard walks the free
        // tree, too costly for every readback. The heap picked at boot lives in [heap+0x28],
        // [heap+0x30] bytes (flexible memory). sotc (#146): the heaps a world creates (the last
        // colossus' area, after the ending) grow in 1 MiB pieces over direct memory and share that
        // +0x28 pointer, so readbacks into them were never guarded and the ending still crashed
        // in their free lists (eboot+0x49500): their range comes from their own guard's free
        // blocks, refreshed every 2 s while their free tree changes, and after 250 ms already when
        // a readback lands outside the range (a piece the heap may have added since).
        VAddr heap_base = *std::bit_cast<const VAddr*>(heap + 0x28);
        u64 heap_size = *std::bit_cast<const u64*>(heap + 0x30);
        GuardSlot* known = nullptr;
        if (h != 0) {
            known = find_slot(heap);
            if (!known) {
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            const auto signature = signature_of(heap);
            const auto outside_extent = [&] {
                if (!known->guard || !known->guard->IsValid()) {
                    return true;
                }
                const auto [lo, hi] = known->guard->FreeExtent();
                constexpr u64 Margin = 1ULL << 20;
                return std::any_of(pending.begin(), pending.end(), [&](const ReadbackItem& item) {
                    return item.addr + Margin < lo || item.addr + item.size > hi + Margin;
                });
            };
            const auto age = now - known->built;
            if (!known->guard ||
                (signature != known->signature &&
                 (age > std::chrono::seconds(2) ||
                  (age > std::chrono::milliseconds(250) && outside_extent())))) {
                SotcStats::heap_guard_builds.fetch_add(1, std::memory_order_relaxed);
                known->guard.emplace(memory, heap);
                known->signature = signature;
                known->built = now;
            }
            const auto [lo, hi] = known->guard->IsValid() ? known->guard->FreeExtent()
                                                          : std::pair<VAddr, VAddr>{0, 0};
            if (hi <= lo) {
                continue;
            }
            constexpr u64 Margin = 1ULL << 20; // headers of the used blocks next to the free ones
            heap_base = lo > Margin ? lo - Margin : 0;
            heap_size = hi - heap_base + Margin;
            static std::array<VAddr, 16> reported{};
            if (std::find(reported.begin(), reported.end(), heap) == reported.end()) {
                if (auto free = std::find(reported.begin(), reported.end(), VAddr{0});
                    free != reported.end()) {
                    *free = heap;
                    std::fprintf(stderr, "SOTC heap guard: heap %#llx guarded in %#llx..%#llx\n",
                                 static_cast<unsigned long long>(heap),
                                 static_cast<unsigned long long>(lo),
                                 static_cast<unsigned long long>(hi));
                }
            }
        }
        bool touches_heap = false;
        for (const auto& item : pending) {
            touches_heap |= item.addr < heap_base + heap_size && heap_base < item.addr + item.size;
        }
        if (!touches_heap) {
            continue;
        }
        GuardSlot* slot = known;
        if (slot) {
            if (!slot->used) {
                slot->used.emplace(memory);
            }
            active[active_count++] = {slot, heap_base, heap_size, false};
            continue;
        }
        for (auto& candidate : slots) {
            if (candidate.heap == heap) {
                slot = &candidate;
                break;
            }
        }
        if (!slot) {
            for (auto& candidate : slots) {  // a free slot, else one of a heap not seen now
                if (candidate.heap == 0 ||
                    std::find(heaps.begin(), heaps.begin() + heap_count, candidate.heap) ==
                        heaps.begin() + heap_count) {
                    slot = &candidate;
                    slot->heap = heap;
                    slot->guard.reset();
                    slot->used.reset();
                    break;
                }
            }
        }
        if (!slot) {
            continue;
        }
        if (!slot->used) {
            slot->used.emplace(memory);
        }
        active[active_count++] = {slot, heap_base, heap_size, false};
    }
    // The heap's guard, built again when its free tree changed (signature) since the last build
    const auto guard_of = [&](Active& a) -> BpeHeapGuard* {
        GuardSlot& slot = *a.slot;
        if (!a.checked) {
            a.checked = true;
            const VAddr heap = slot.heap;
            const std::array<u64, 3> signature = {*std::bit_cast<const u64*>(heap + 0x80),
                                                   *std::bit_cast<const u64*>(heap + 0xa8),
                                                   *std::bit_cast<const u64*>(heap + 0xc0)};
            if (!slot.guard || signature != slot.signature) {
                SotcStats::heap_guard_builds.fetch_add(1, std::memory_order_relaxed);
                slot.guard.emplace(memory, heap);
                slot.signature = signature;
            }
        }
        return slot.guard->IsValid() ? &*slot.guard : nullptr;
    };
    if (!heap_fast) {
        for (size_t a = 0; a < active_count; ++a) {
            guard_of(active[a]);
        }
    }
    const auto active_for = [&](VAddr addr, u64 size) -> Active* {
        for (size_t a = 0; a < active_count; ++a) {
            if (addr < active[a].base + active[a].size && active[a].base < addr + size) {
                return &active[a];
            }
        }
        return nullptr;
    };
    for (const auto& item : pending) {
        for (const auto& copy : item.copies) {
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
                BpeHeapGuard* heap_guard = nullptr;
                if (Active* a = active_for(addr, size)) {
                    BpeUsedBlocks& used = *a->slot->used;
                    if (heap_fast && used.Covers(a->slot->heap, addr, addr + size)) {
                        memory->TryWriteBacking(std::bit_cast<u8*>(addr), src + (addr - dst_addr),
                                                size, 1);
                        return;
                    }
                    heap_guard = guard_of(*a);
                    int find = 0;
                    if (VAddr block = 0, next = 0;
                        heap_fast && heap_guard &&
                        heap_guard->FindUsedBlock(addr, block, next, &find)) {
                        used.Add(block, next);
                    } else if (find == 6) {
                        used.SetFence(block);
                    }
                } else if (active_count == 1 && active[0].checked) {
                    // Outside the heap: its guard when this batch built it, as before
                    heap_guard = guard_of(active[0]);
                }
                if (heap_guard) {
                    if (const u64 kept = heap_guard->Write(addr, src + (addr - dst_addr), size)) {
                        static u64 reported = 0;
                        if (reported++ < 8) {
                            LOG_WARNING(Render, "BPE heap guard kept {:#x} bytes of heap metadata at {:#x}",
                                        kept, addr);
                        }
                    }
                    return;
                }
                memory->TryWriteBacking(std::bit_cast<u8*>(addr), src + (addr - dst_addr), size,
                                        1);
            });
            });
        }
        memory_tracker->UnmarkRegionAsGpuModified(item.addr, item.size, false);
        MarkInflightReadbacksDirty(item.addr, item.size);
    }
}

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
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
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);
    // SynchronizeMemory also syncs read-only texel buffers from an aliasing image (sotc: a second
    // SynchronizeMemoryFromImage call here repeated that work, re-tiling the image twice).
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (SotcStats::OptOff() && is_texel_buffer && !is_written) {
        SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    if (is_written) {
        Core::MemoryManager::NoteHostWrite(
            device_addr, size, 8, reinterpret_cast<u64>(__builtin_return_address(0)));
        AddGpuModified(device_addr, size);
        RecordCurrentWrite(device_addr, size);
    }
    return {arena, arena->Offset(device_addr)};
}

void BufferCache::AddGpuModified(VAddr addr, u64 size) {
    MarkInflightReadbacksDirty(addr, size);
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
                         static_cast<unsigned long long>(SotcStats::current_program.load()));
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

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
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
    MarkInflightReadbacksDirty(addr, size);
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
    MarkInflightReadbacksDirty(addr, size);
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
                SynchronizeMemory(address_space[device_addr >> ARENA_PAGE_BITS], device_addr,
                                  static_cast<u32>(size), false, false);
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
                    SynchronizeMemory(address_space[run_start >> ARENA_PAGE_BITS], lo,
                                      static_cast<u32>(hi - lo), false, false);
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
            SynchronizeMemory(address_space[page], device_addr, size, false, false);
            ++ranges;
        }
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
    return new_arena;
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
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
    auto& cached = resident_cache[(first_block * 0x9E3779B97F4A7C15ull) >> 58];
    if (cached.version == resident_version && first_block >= cached.first &&
        last_block < cached.end) {
        if (cached.epoch != use_epoch) {
            stamp(cached.first, cached.end);
            cached.epoch = use_epoch;
        }
        return;
    }
    stamp(first_block, last_block + 1);
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        cached = {resident_version, first_block, last_block + 1, use_epoch};
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
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    memory_tracker->ForEachUploadRange(device_addr, size, is_written, [&](u64 addr, u64 size) {
        copies.emplace_back(total_size_bytes, addr, size);
        total_size_bytes += size;
    });
    if (!copies.empty()) {
        const SotcStats::FrameTimer frame_timer{SotcStats::FrameBuffer};
        if (SotcStats::kFpsLog) {
            SotcStats::frame_copy_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
        }
        SotcStats::uploads.fetch_add(1, std::memory_order_relaxed);
        if (SotcStats::kCmdStats) { // sotc: read only by the SOTC_CMD_STATS printer
            SotcStats::upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
        }
        const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
        // sotc: a large upload made of several ranges (a streaming burst: 200+ MB in one frame)
        // is copied in one parallel pass over 512 KiB pieces of all its ranges; per range, only
        // ranges of 2 MiB and more were split. SOTC_COPY_BATCH=0: per range.
        static const bool copy_batch = [] {
            const char* v = std::getenv("SOTC_COPY_BATCH");
            return !v || v[0] != '0';
        }();
        if (copy_batch && copies.size() > 1 && total_size_bytes >= 2_MB && BbCopy::Enabled()) {
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
        for (auto& copy : copies) {
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        staging.Flush();
        runtime.CopyBuffer(staging.buffer, arena, copies);
        for (const auto& copy : copies) {
            RecordCurrentWrite(arena->cpu_addr + copy.dstOffset, copy.size);
        }
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    // sotc: a current write is recorded only on the two paths that write the arena range. Recording
    // it unconditionally marked unwritten ranges as written by the command buffer being recorded,
    // which sent their readbacks to the slower flushing path instead of the ahead path.
    if (SotcStats::OptOff()) {
        RecordCurrentWrite(device_addr, size);
    }
    if (auto type = texture_cache.IsMeta(device_addr)) {
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
        const auto [result, memory] = instance.GetDevice().allocateMemory(alloc_info);
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
        return (v ? std::strtoull(v, nullptr, 10) : 1536ull) << 20;
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
    if (pending_eviction) {
        AdvanceUseEpoch(now);
        return FinishEviction(now, idle_epochs, false);
    }
    if (now < next_evict_check) {
        return false;
    }
    next_evict_check = now + std::chrono::seconds(2);
    AdvanceUseEpoch(now);
    const u64 resident = ArenaUsedBytes();
    if (limit == 0 || resident <= limit) {
        return false;
    }
    const SotcStats::ScopedTimer evict_timer{SotcStats::arena_evict_ns};
    // sotc: with the 64 MiB chunks the unit is a resident range (its blocks go back to their
    // chunk, and a chunk left empty to the driver), not a whole allocation.
    struct Candidate {
        u16 age{0xFFFF}; // youngest block
        bool keep{};
        u64 bytes{};
        boost::container::small_vector<Backing, 4> ranges;
    };
    std::unordered_map<u64, Candidate> candidates;
    std::erase_if(evict_retry, [&](const auto& entry) { return entry.second <= now; });
    std::vector<std::pair<u16, u64>> order;
    for (const Backing& range : resident_ranges) {
        auto& candidate = candidates[range.start];
        for (u64 block = range.start; block < range.end && !candidate.keep; ++block) {
            const u16 age = static_cast<u16>(use_epoch - block_last_use[block]);
            candidate.age = std::min(candidate.age, age);
            candidate.keep = age < idle_epochs;
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
        if (total >= step || resident - total <= limit / 8 * 7) {
            break;
        }
        const Candidate& candidate = candidates[key];
        EvictChoice& pick = chosen.emplace_back(EvictChoice{key, candidate.bytes, {}, candidate.ranges});
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
                                       now + std::chrono::seconds(5)};
    // sotc: SOTC_EVICT_ASYNC=0 waits for the read-back here, as before: scheduler.Finish() on the
    // command processor, ~50 ms per 64 MiB run, one late frame each.
    static const bool async = [] {
        const char* v = std::getenv("SOTC_EVICT_ASYNC");
        return !v || v[0] != '0';
    }();
    return async ? false : FinishEviction(now, idle_epochs, true);
}

bool BufferCache::FinishEviction(std::chrono::steady_clock::time_point now, u16 idle_epochs,
                                 bool wait) {
    // sotc: the unbind's sparse batch does not wait for earlier submits, so it goes only once
    // every command buffer submitted so far (the read-back's included) is done. The GPU catches
    // up within a frame or two at the benchmark scenes; after 5 s, or with `wait`, Finish() it.
    PendingEviction& pending = *pending_eviction;
    if (!scheduler.IsFree(pending.tick) || !scheduler.IsFree(scheduler.CurrentTick() - 1)) {
        if (!wait && now < pending.deadline) {
            return false;
        }
        scheduler.Finish();
    }
    const SotcStats::ScopedTimer evict_timer{SotcStats::arena_evict_ns};
    const auto& download = pending.download;
    download.buffer->Invalidate(download.offset, download.size);

    std::vector<Backing> released;
    std::array<u8, 4096> guest;
    for (const EvictChoice& pick : pending.chosen) {
        // Bound again, or written by the GPU, after the read-back was recorded: stays.
        bool keep = false;
        for (const Backing& range : pick.ranges) {
            for (u64 block = range.start; block < range.end && !keep; ++block) {
                keep = static_cast<u16>(use_epoch - block_last_use[block]) < idle_epochs;
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
    }
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
        return !same;
    });
    for (const Backing& range : released) {
        const VAddr addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        memory_tracker->MarkRegionAsCpuModified(addr, size);
        evicted_ranges.Add({range.start, range.end});
        SotcStats::arena_evicted.fetch_add(size, std::memory_order_relaxed);
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
    return true;
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
