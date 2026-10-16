// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <magic_enum/magic_enum.hpp>

#include "common/alignment.h"
#include "common/scope_exit.h"
#include "common/thread.h"
#include <fmt/format.h>
#include "core/debug_state.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/sotc_stats.h"
#include "video_core/buffer_cache/bpe_heap_guard.h"
#include "bbport_copy.h"
#include <optional>

extern "C" uintptr_t runtime_sotc_heap(void);
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(
    unsigned long frames_to_skip, unsigned long frames_to_capture, void** back_trace,
    unsigned long* back_trace_hash);
#endif

namespace VideoCore {

void ReportBothModified(VAddr page_addr, u64 pages) {
    const u64 seen = g_both_modified_pages.fetch_add(pages, std::memory_order_relaxed);
    static std::atomic<int> shown{0};
    if (shown.fetch_add(1, std::memory_order_relaxed) >= 24) {
        return;
    }
    std::string frames;
#ifdef _WIN32
    void* stack[12]{};
    const unsigned n = RtlCaptureStackBackTrace(1, 12, stack, nullptr);
    for (unsigned i = 0; i < n; ++i) {
        frames += fmt::format(" {:#x}", reinterpret_cast<uintptr_t>(stack[i]));
    }
#endif
    std::fprintf(stderr, "SOTC page both CPU- and GPU-modified: %#llx (+%llu pages, %llu so far), "
                         "thread %s; stack%s\n",
                 static_cast<unsigned long long>(page_addr), static_cast<unsigned long long>(pages),
                 static_cast<unsigned long long>(seen + pages),
                 Common::GetCurrentThreadName().c_str(), frames.c_str());
}

namespace {
/// SOTC_TEXEL_SYNC_LEGACY=1: texel buffers synchronized twice per bind and always recorded as
/// written by the current command buffer (the behaviour before the CP-thread optimizations).
bool TexelSyncLegacy() {
    static const bool legacy = [] {
        const char* v = std::getenv("SOTC_TEXEL_SYNC_LEGACY");
        return v && *v == '1';
    }();
    return legacy;
}
} // namespace

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

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

BufferCache::~BufferCache() = default;

void BufferCache::TickFrame() {
    if (std::exchange(fault_process_pending, false)) {
        fault_manager->ProcessFaultBuffer();
    }
    // sotc: eager copies the guest did not ask for within 64 submissions.
    const u64 tick = scheduler.CurrentTick();
    for (size_t i = 0; i < eager_readbacks.size();) {
        if (eager_readbacks[i].dirty || eager_readbacks[i].tick + 64 < tick) {
            DropEagerReadback(i, !eager_readbacks[i].dirty);
        } else {
            ++i;
        }
    }
}

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    Core::MemoryManager::NoteHostWrite(device_addr, size, 9,
                                       reinterpret_cast<u64>(__builtin_return_address(0)));
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        ReadMemory(device_addr, size, true, assume_locks);
    });
}

void BufferCache::DropGpuModified(VAddr device_addr, u64 size) {
    liverpool->SendCommand<true>([this, device_addr, size] {
        gpu_modified_ranges.Subtract(device_addr, size);
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

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    const auto flush_request = [this, device_addr, size, is_write] {
        NoteHotRead(device_addr, size);
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);

        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        constexpr u64 WindowSize = ReadbackWindow;
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + WindowSize, device_addr + size), arena_end);
        Core::MemoryManager::NoteHostWrite(device_addr, size, 6, window_start);
        fault_addr = device_addr;
        fault_size = size;
        DownloadMemory(arena, window_start, window_end - window_start);
        fault_size = 0;
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    if (assume_locks) {
        flush_request();
        return;
    }
    // sotc: SOTC_RB_ASYNC=1. PR #32 turned it on (Radeon RX 6650 XT, sanctuary, three runs each:
    // 25.3-25.8 fps instead of 23.2-23.7, readback waits 100 ms/s instead of 290 ms/s). Off here:
    // on an RTX 5070 it was neutral (Track R), and on this tree (page tracking changes over 0.57)
    // the attract demo stopped on page_manager's "protect region as write-only" assertion.
    static const bool async = [] {
        const char* v = std::getenv("SOTC_RB_ASYNC");
        return v && v[0] == '1';
    }();
    // Serves the request: on the command processor (it waits for the GPU), or with
    // SOTC_RB_ASYNC=1 in two steps, the guest thread waiting for the GPU in between.
    const auto serve = [&] {
        if (!async) {
            liverpool->SendCommand<true>(std::move(flush_request));
            return;
        }
        AsyncReadback readback{.device_addr = device_addr, .size = size, .is_write = is_write};
        liverpool->SendCommand<true>([this, &readback] { BeginAsyncReadback(readback); });
        if (!readback.waiting) {
            return;
        }
        scheduler.WaitSubmitted(readback.tick, false);
        scheduler.GetWorkSemaphore()->Wait(readback.tick);
        liverpool->SendCommand<true>([this, &readback] { FinishAsyncReadback(readback); });
    };
    // sotc: several job threads fault on one window at once (the culling bitsets: ~8 calls
    // for ~3 real readbacks a frame), and each request interrupts the command processor.
    // The first thread reads the window back; the others wait for it and re-run their load,
    // which then finds the page readable. A thread faulting again on the same address right
    // after waiting (a GPU write recorded meanwhile protected the page again) goes the usual
    // way. Write faults keep the usual path (they mark the page CPU-modified on the command
    // processor). SOTC_RB_DEDUP=1 on, 2 on and count retried loads that would fault again.
    static const int dedup = [] {
        const char* v = std::getenv("SOTC_RB_DEDUP");
        return v ? std::atoi(v) : 0;
    }();
    if (dedup == 0 || is_write) {
        serve();
        return;
    }
    thread_local VAddr retried = ~VAddr{0};
    const VAddr window = Common::AlignDown(device_addr, ReadbackWindow);
    GuestReadSlot* waited = nullptr;
    GuestReadSlot* owned = nullptr;
    u32 generation = 0;
    if (std::exchange(retried, ~VAddr{0}) != device_addr) {
        std::scoped_lock lk{guest_read_mutex};
        for (auto& slot : guest_read_slots) {
            if (slot.window == window) {
                waited = &slot;
                generation = slot.done.load(std::memory_order_relaxed);
                break;
            }
        }
        if (!waited) {
            for (auto& slot : guest_read_slots) {
                if (slot.window == ~VAddr{0}) {
                    slot.window = window;
                    owned = &slot;
                    break;
                }
            }
        }
    }
    if (waited) {
        // The owner clears the slot under the mutex before bumping the generation read
        // above under it, so the wake-up cannot be missed.
        waited->done.wait(generation, std::memory_order_acquire);
        retried = device_addr;
        SotcStats::readback_dedup.fetch_add(1, std::memory_order_relaxed);
        if (dedup == 2) {
            liverpool->SendCommand<true>([this, device_addr, size] {
                if (IsRegionGpuModified(device_addr, size)) {
                    SotcStats::readback_dedup_miss.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        return;
    }
    serve();
    if (owned) {
        {
            std::scoped_lock lk{guest_read_mutex};
            owned->window = ~VAddr{0};
        }
        owned->done.fetch_add(1, std::memory_order_release);
        owned->done.notify_all();
    }
}

void BufferCache::BeginAsyncReadback(AsyncReadback& readback) {
    SotcStats::readbacks.fetch_add(1, std::memory_order_relaxed);
    NoteHotRead(readback.device_addr, readback.size);
    // sotc: the job threads fault on the same windows at once (the culling bitsets). Waiting, the
    // command processor served them one after another and the later ones found the window read
    // back already; asynchronous, they arrive while the first copy is in flight, and each recorded
    // a copy of its own: twice the readbacks (copies, submits, GPU syncs). A window in flight holds
    // every GPU-modified byte it had when copied, and a GPU write recorded since makes it dirty
    // (FinishAsyncReadback then reads it back again): waiting for it gives these bytes too.
    if (const AsyncReadback* other = FindInflightWindow(readback.device_addr, readback.size)) {
        readback.tick = other->tick;
        readback.waiting = true;
        readback.dependent = true;
        SotcStats::readback_joined.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const u32 first_block = readback.device_addr >> block_shift;
    const u32 last_block = (readback.device_addr + readback.size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    constexpr u64 WindowSize = ReadbackWindow;
    const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
    const VAddr window_start =
        std::max<VAddr>(Common::AlignDown(readback.device_addr, WindowSize), arena->cpu_addr);
    const VAddr window_end = std::min<VAddr>(
        std::max<VAddr>(window_start + WindowSize, readback.device_addr + readback.size),
        arena_end);
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
            memory_tracker->MarkRegionAsCpuModified(readback.device_addr, readback.size);
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
        for (const auto& item : readback->pending) {
            readback->dirty |= addr < item.addr + item.size && item.addr < addr + size;
        }
    }
    for (EagerReadback& eager : eager_readbacks) {
        eager.dirty |= addr < eager.window + eager.size && eager.window < addr + size;
    }
}

int BufferCache::EagerMode() {
    // sotc: SOTC_RB_EAGER=1: when the command buffer being built wrote a window the guest reads
    // back, copy that window to staging behind the writer and submit (at the next command after
    // SOTC_RB_EARLY_US, or at the label). The readback then waits for that submission only, not
    // for the ~1-2 ms of work recorded and submitted until the guest asks. A window written
    // again, or whose GPU-modified ranges changed since the copy, takes the usual path.
    // 2: also reads every hit back the usual way and compares (counts verify-bad).
    static const int mode = [] {
        const char* v = std::getenv("SOTC_RB_EAGER");
        return v ? std::atoi(v) : 0;
    }();
    return mode;
}

void BufferCache::DropEagerReadback(size_t index, bool unused) {
    EagerReadback& eager = eager_readbacks[index];
    staging_pool.FreeDeferred(Vulkan::StagingBufferRef{
        .buffer = eager.download_buffer,
        .offset = eager.download_offset,
        .size = eager.download_size,
        .mapped = eager.download_mapped,
        .type = VideoCore::MemoryType::HostCached,
    });
    if (unused) {
        SotcStats::eager_unused.fetch_add(1, std::memory_order_relaxed);
    }
    eager_readbacks.erase(eager_readbacks.begin() + index);
}

void BufferCache::NoteHotWindowWrite(VAddr addr, u64 size) {
    const VAddr end = addr + size;
    const u64 tick = scheduler.CurrentTick();
    for (const auto& hot : hot_readbacks) {
        if (!(addr < hot.addr + hot.size && hot.addr < end)) {
            continue;
        }
        if (hot_written_tick != tick) {
            hot_written_windows.clear();
            hot_written_tick = tick;
        }
        const bool known = std::ranges::any_of(hot_written_windows, [&](const auto& w) {
            return w.first == hot.addr && w.second == hot.size;
        });
        if (known) {
            hot_write_pending = true;
            continue;
        }
        // Only windows the guest still reads back.
        if (std::chrono::steady_clock::now() - hot.last_request > std::chrono::milliseconds(250)) {
            continue;
        }
        if (hot_written_windows.size() < 8) {
            hot_written_windows.emplace_back(hot.addr, hot.size);
        }
        hot_write_pending = true;
    }
}

void BufferCache::RecordEagerReadbacks() {
    const u64 tick = scheduler.CurrentTick();
    if (hot_written_tick != tick) {
        hot_written_windows.clear();
        return;
    }
    for (const auto& [window, window_size] : hot_written_windows) {
        for (size_t i = 0; i < eager_readbacks.size(); ++i) {
            if (eager_readbacks[i].window == window) {
                DropEagerReadback(i, !eager_readbacks[i].dirty);
                break;
            }
        }
        const Buffer* arena = address_space[window >> ARENA_PAGE_BITS];
        if (!arena || address_space[(window + window_size - 1) >> ARENA_PAGE_BITS] != arena) {
            continue;
        }
        EagerReadback eager{.arena = arena,
                            .window = window,
                            .size = window_size,
                            .tick = tick,
                            .recorded_at = std::chrono::steady_clock::now()};
        const u64 total = GatherDownloads(arena, window, window_size, eager.pending, false, false);
        if (total == 0) {
            continue;
        }
        const auto download =
            staging_pool.Request(total, VideoCore::MemoryType::HostCached, 0, true);
        for (auto& item : eager.pending) {
            for (auto& copy : item.copies) {
                copy.dstOffset += download.offset;
            }
            runtime.CopyBuffer(item.arena, download.buffer, item.copies);
        }
        eager.download_offset = download.offset;
        eager.download_mapped = download.mapped;
        eager.download_buffer = download.buffer;
        eager.download_size = download.size;
        if (eager_readbacks.size() >= MaxEagerReadbacks) {
            DropEagerReadback(0, true);
        }
        eager_readbacks.push_back(std::move(eager));
        SotcStats::eager_recorded.fetch_add(1, std::memory_order_relaxed);
    }
    hot_written_windows.clear();
}

bool BufferCache::TryEagerReadback(VAddr window, u64 size) {
    size_t index = 0;
    while (index < eager_readbacks.size() &&
           (eager_readbacks[index].window != window || eager_readbacks[index].size != size)) {
        ++index;
    }
    if (index == eager_readbacks.size()) {
        return false;
    }
    if (eager_readbacks[index].dirty) {
        SotcStats::eager_dirty.fetch_add(1, std::memory_order_relaxed);
        DropEagerReadback(index, false);
        return false;
    }
    // The GPU-modified ranges must be those copied: anything added since went through
    // AddGpuModified (dirty), this also catches what did not.
    boost::container::small_vector<ReadbackItem, 16> now;
    GatherDownloads(eager_readbacks[index].arena, window, size, now, false, false);
    const auto& copied = eager_readbacks[index].pending;
    bool same = now.size() == copied.size();
    for (size_t i = 0; same && i < now.size(); ++i) {
        same = now[i].copies.size() == copied[i].copies.size() &&
               now[i].gpu_ranges == copied[i].gpu_ranges;
        for (size_t c = 0; same && c < now[i].copies.size(); ++c) {
            same = now[i].copies[c].srcOffset == copied[i].copies[c].srcOffset &&
                   now[i].copies[c].size == copied[i].copies[c].size;
        }
    }
    if (!same) {
        SotcStats::eager_changed.fetch_add(1, std::memory_order_relaxed);
        DropEagerReadback(index, false);
        return false;
    }
    EagerReadback eager = std::move(eager_readbacks[index]);
    eager_readbacks.erase(eager_readbacks.begin() + index);
    // Served without GatherDownloads: keep the window hot (NoteHotWindowWrite looks at it).
    for (auto& hot : hot_readbacks) {
        if (hot.addr == window && hot.size == size) {
            hot.last_request = std::chrono::steady_clock::now();
        }
    }
    SotcStats::eager_age_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - eager.recorded_at)
                                          .count(),
                                      std::memory_order_relaxed);
    scheduler.GetWorkSemaphore()->Refresh();
    if (scheduler.GetWorkSemaphore()->IsFree(eager.tick)) {
        SotcStats::eager_ready.fetch_add(1, std::memory_order_relaxed);
    }
    {
        const SotcStats::ScopedTimer wait_timer{SotcStats::eager_wait_ns};
        const SotcStats::ScopedTimer total_timer{SotcStats::readback_wait_ns};
        scheduler.WaitSubmitted(eager.tick);
        scheduler.GetWorkSemaphore()->WaitHostRead(eager.tick);
    }
    eager.download_buffer->Invalidate(eager.download_offset, eager.download_size);
    const Vulkan::StagingBufferRef download{
        .buffer = eager.download_buffer,
        .offset = eager.download_offset,
        .size = eager.download_size,
        .mapped = eager.download_mapped,
        .type = VideoCore::MemoryType::HostCached,
    };
    if (EagerMode() == 2) {
        // Read the same ranges back the usual way (finish path) and compare.
        u64 total = 0;
        for (const auto& item : now) {
            for (const auto& copy : item.copies) {
                total = std::max<u64>(total, copy.dstOffset + copy.size);
            }
        }
        const auto fresh = staging_pool.Request(total, VideoCore::MemoryType::HostCached, 0, true);
        for (auto& item : now) {
            for (auto& copy : item.copies) {
                copy.dstOffset += fresh.offset;
            }
            runtime.CopyBuffer(item.arena, fresh.buffer, item.copies);
        }
        scheduler.FinishHostRead();
        fresh.buffer->Invalidate(fresh.offset, fresh.size);
        for (size_t i = 0; i < now.size(); ++i) {
            for (size_t c = 0; c < now[i].copies.size(); ++c) {
                const u8* a = fresh.mapped + (now[i].copies[c].dstOffset - fresh.offset);
                u8* b = eager.download_mapped +
                        (eager.pending[i].copies[c].dstOffset - eager.download_offset);
                if (std::memcmp(a, b, now[i].copies[c].size) != 0) {
                    SotcStats::eager_verify_bad.fetch_add(1, std::memory_order_relaxed);
                    static int shown = 0;
                    if (shown++ < 20) {
                        std::fprintf(stderr, "SOTC eager verify: window %#llx copy %#llx+%#llx differs\n",
                                     static_cast<unsigned long long>(window),
                                     static_cast<unsigned long long>(
                                         eager.pending[i].arena->cpu_addr +
                                         eager.pending[i].copies[c].srcOffset),
                                     static_cast<unsigned long long>(now[i].copies[c].size));
                    }
                    // Use the fresh data.
                    std::memcpy(b, a, now[i].copies[c].size);
                }
            }
        }
        staging_pool.FreeDeferred(fresh);
    }
    for (const auto& item : eager.pending) {
        for (const auto& [address, range_size] : item.gpu_ranges) {
            gpu_modified_ranges.Subtract(address, range_size);
        }
    }
    ApplyDownloads(eager.pending, eager.download_mapped, eager.download_offset);
    staging_pool.FreeDeferred(download);
    SotcStats::eager_hits.fetch_add(1, std::memory_order_relaxed);
    return true;
}

BufferCache::AsyncReadback* BufferCache::FindInflightWindow(VAddr addr, u64 size) {
    for (AsyncReadback* readback : inflight_readbacks) {
        for (const auto& item : readback->pending) {
            if (item.addr <= addr && addr + size <= item.addr + item.size) {
                return readback;
            }
        }
    }
    return nullptr;
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

void BufferCache::GiveToGuestWrite(VAddr device_addr, u64 size) {
    // sotc: a guest write fault on GPU data is served by reading the data back and then letting
    // the guest write (the page becomes CPU-modified). Waiting, both happen at one point of the
    // command stream. Asynchronously, the command processor goes on between the copy and this
    // point and may have bound these bytes for a GPU write again: the copy is stale for them, and
    // making the page CPU-modified now would leave it GPU-modified as well (no protection fits:
    // Windows has no write-only pages, and the readback of the new GPU data would overwrite the
    // guest's write). Read them back now, as the waiting path would have, first.
    if (memory_tracker->IsRegionGpuModified(device_addr, size)) {
        SotcStats::readback_write_refresh.fetch_add(1, std::memory_order_relaxed);
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, ReadbackWindow), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + ReadbackWindow, device_addr + size), arena_end);
        DownloadMemory(arena, window_start, window_end - window_start);
    }
    memory_tracker->MarkRegionAsCpuModified(device_addr, size);
}

void BufferCache::FinishAsyncReadback(AsyncReadback& readback) {
    if (readback.dependent) {
        // Finish the readbacks this one waited for (owners still blocked keep them alive): the
        // ones whose window holds its bytes, and the ones copying any of them.
        while (AsyncReadback* other = FindInflightWindow(readback.device_addr, readback.size)) {
            FinishAsyncReadback(*other);
        }
        while (AsyncReadback* other = FindInflightReadback(readback.device_addr, readback.size)) {
            FinishAsyncReadback(*other);
        }
        if (readback.is_write) {
            GiveToGuestWrite(readback.device_addr, readback.size);
        }
        return;
    }
    if (readback.finished) {
        if (readback.is_write) {
            GiveToGuestWrite(readback.device_addr, readback.size);
        }
        return;
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
    if (readback.dirty) {
        // A GPU write recorded after the copies (or another readback applied first and the
        // guest may have written since): the data may be stale. The ranges never left the
        // GPU-modified set; read the window back the usual way.
        staging_pool.FreeDeferred(download);
        DownloadMemory(readback.arena, readback.window_start, readback.window_size);
    } else {
        download.buffer->Invalidate(download.offset, download.size);
        for (const auto& item : readback.pending) {
            for (const auto& [address, range_size] : item.gpu_ranges) {
                gpu_modified_ranges.Subtract(address, range_size);
            }
        }
        ApplyDownloads(readback.pending, download.mapped, download.offset);
        staging_pool.FreeDeferred(download);
    }
    if (readback.is_write) {
        GiveToGuestWrite(readback.device_addr, readback.size);
    }
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    SotcStats::readbacks.fetch_add(1, std::memory_order_relaxed);
    const SotcStats::ScopedTimer readback_timer{SotcStats::readback_ns};
    if (EagerMode() != 0 && !eager_readbacks.empty() && TryEagerReadback(device_addr, size)) {
        return;
    }
    boost::container::small_vector<ReadbackItem, 16> pending;
    const u64 total_size_bytes = GatherDownloads(arena, device_addr, size, pending);
    if (total_size_bytes == 0) {
        SotcStats::readback_noop.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    // Was the data written by work already submitted, or by the command buffer being built?
    if (current_writes_tick != scheduler.CurrentTick()) {
        current_writes.Clear();
        current_writes_tick = scheduler.CurrentTick();
    }
    bool written_by_current = false;
    bool main_written = false;
    for (const auto& item : pending) {
        bool written = false;
        for (const auto& copy : item.copies) {
            written |= current_writes.Intersects(item.arena->cpu_addr + copy.srcOffset, copy.size);
        }
        written_by_current |= written;
        if (item.addr == device_addr && item.size == size) {
            main_written |= written;
        }
    }
    if (written_by_current && !main_written) {
        // Only a batched hot window was written by the open command buffer (SOTC_RB_HOT_SPLIT).
        SotcStats::readback_finish_extra.fetch_add(1, std::memory_order_relaxed);
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
        TraceReadback(device_addr, false, main_written, written_by_current, total_size_bytes);
        {
            const SotcStats::ScopedTimer wait_timer{SotcStats::readback_wait_ns};
            scheduler.SubmitAheadAndWait();
        }
        SotcStats::readback_ahead.fetch_add(1, std::memory_order_relaxed);
    } else {
        SotcStats::readback_finish.fetch_add(1, std::memory_order_relaxed);
        TraceReadback(device_addr, true, main_written, written_by_current, total_size_bytes);
        // Did the guest ask for bytes the command buffer being built writes, or only a neighbour
        // in the window (or a hot region) did?
        if (fault_size != 0 && written_by_current &&
            current_writes.Intersects(fault_addr, fault_size)) {
            SotcStats::readback_finish_asked.fetch_add(1, std::memory_order_relaxed);
        }
        for (const auto& item : pending) {
            runtime.CopyBuffer(item.arena, download.buffer, item.copies);
        }
        const SotcStats::ScopedTimer wait_timer{SotcStats::readback_wait_ns};
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
}

u64 BufferCache::GatherDownloads(const Buffer* arena, VAddr device_addr, u64 size,
                                 boost::container::small_vector<ReadbackItem, 16>& pending,
                                 bool subtract, bool hot_extras) {
    // A readback costs a full CPU/GPU sync. Games tend to read back the same few regions every
    // frame, one after another, so while the GPU is idle anyway also bring along the other
    // regions read back recently ("hot" readbacks) that the GPU has modified since. The reads
    // that follow then find their data already on the CPU instead of each needing a sync.
    // SOTC_RB_HOT=0: off, 1: same arena and small regions only, 2 (default): any existing arena,
    // any size. Mode 2 halves the readback syncs in SotC's open world (~+20% fps).
    static const int hot_mode = [] {
        const char* v = std::getenv("SOTC_RB_HOT");
        return v ? std::atoi(v) : 2;
    }();
    const bool hot_enabled = hot_mode != 0 && hot_extras;
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
            if (std::ranges::any_of(eager_readbacks, [&](const EagerReadback& eager) {
                    return eager.window == hot.addr && !eager.dirty;
                })) {
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
        if (!known) {
            if (hot_readbacks.size() < MaxHotReadbacks) {
                hot_readbacks.push_back({device_addr, size, now});
            } else {
                auto oldest = std::ranges::min_element(hot_readbacks, {}, &HotReadback::last_request);
                *oldest = {device_addr, size, now};
            }
        }
    }

    // sotc: when the requested window comes from submitted work, a batched hot window written
    // by the command buffer still being built would turn this readback into a full drain
    // (finish path). Leave those out: they stay GPU-modified and protected, and their own
    // request reads them back. SOTC_RB_HOT_SPLIT=1 (the asynchronous path is unchanged).
    static const bool hot_split = [] {
        const char* v = std::getenv("SOTC_RB_HOT_SPLIT");
        return v && v[0] == '1';
    }();
    // DownloadMemory resets current_writes for a new tick only after this.
    const bool open_writes = hot_split && subtract && current_writes_tick == scheduler.CurrentTick();
    bool main_written = false;
    u64 total_size_bytes = 0;
    for (size_t r = 0; r < regions.size(); ++r) {
        const Region& region = regions[r];
        if (r != 0 && open_writes && !main_written &&
            current_writes.Intersects(region.addr, region.size)) {
            SotcStats::readback_extra_deferred.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        ReadbackItem item{region.arena, region.addr, region.size, {}};
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
                gpu_modified_ranges.ForEachInRange(address, range_size, add_download);
                if (subtract) {
                    gpu_modified_ranges.Subtract(address, range_size);
                } else {
                    item.gpu_ranges.emplace_back(address, range_size);
                }
            });
        if (r == 0 && open_writes) {
            for (const auto& copy : item.copies) {
                main_written |= current_writes.Intersects(arena_base + copy.srcOffset, copy.size);
            }
        }
        if (item.copies.empty()) {
            continue;
        }
        // Large modified regions (render targets and the like) are left to their own readback.
        constexpr u64 MaxHotBytes = 64_KB;
        u64 item_bytes = 0;
        for (const auto& copy : item.copies) {
            item_bytes += copy.size;
        }
        if (hot_mode == 1 && !pending.empty() && item_bytes > MaxHotBytes) {
            // Not downloaded now: give the range back to the modified set.
            for (const auto& copy : item.copies) {
                if (subtract) {
                    AddGpuModified(arena_base + copy.srcOffset, copy.size);
                }
            }
            total_size_bytes = total_before;
            continue;
        }
        pending.push_back(std::move(item));
    }
    return total_size_bytes;
}

void BufferCache::ApplyDownloads(std::span<const ReadbackItem> pending, const u8* mapped,
                                 u64 mapped_offset) {
    static std::optional<BpeHeapGuard> cached_guard;
    static std::array<u64, 3> cached_signature{};
    BpeHeapGuard* heap_guard = nullptr;
    if (const VAddr heap = runtime_sotc_heap(); heap != 0) {
        // Only readbacks into the heap's own memory ([heap+0x28], [heap+0x30] bytes) need it:
        // building the guard walks the free tree, too costly for every readback.
        const VAddr heap_base = *std::bit_cast<const VAddr*>(heap + 0x28);
        const u64 heap_size = *std::bit_cast<const u64*>(heap + 0x30);
        bool touches_heap = false;
        for (const auto& item : pending) {
            touches_heap |= item.addr < heap_base + heap_size && heap_base < item.addr + item.size;
        }
        if (touches_heap) {
            const std::array<u64, 3> signature = {*std::bit_cast<const u64*>(heap + 0x80),
                                                   *std::bit_cast<const u64*>(heap + 0xa8),
                                                   *std::bit_cast<const u64*>(heap + 0xc0)};
            if (!cached_guard || signature != cached_signature) {
                SotcStats::heap_guard_builds.fetch_add(1, std::memory_order_relaxed);
                cached_guard.emplace(memory, heap);
                cached_signature = signature;
            }
            if (cached_guard->IsValid()) {
                heap_guard = &*cached_guard;
            }
        }
    }
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
            ForEachNonStackRange(dst_addr, copy.size, [&](VAddr addr, u64 size) {
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
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    // SynchronizeMemory already re-tiles texel buffers from their image; SOTC_TEXEL_SYNC_LEGACY=1
    // repeats it here as before (A/B).
    if (is_texel_buffer && !is_written && TexelSyncLegacy()) {
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
    // Unchanged set: every clean-page hull computed since already covers this range.
    if (!gpu_modified_ranges.Add(addr, size) || !clean_pages_used || size == 0) {
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

static int ReadbackTraceMode() {
    // SOTC_RB_TRACE=1: gap from the last GPU write of a read-back window to its request, and
    // finish readbacks whose writer was already submitted (counters on the SOTC rb/frame line).
    // 2: also prints the first 200 readbacks.
    static const int mode = [] {
        const char* v = std::getenv("SOTC_RB_TRACE");
        return v ? std::atoi(v) : 0;
    }();
    return mode;
}

BufferCache::HotWriter* BufferCache::FindHotWriter(VAddr window) {
    for (auto& writer : hot_writers) {
        if (writer.window == window) {
            return &writer;
        }
    }
    return nullptr;
}

void BufferCache::NoteHotWriter(VAddr addr, u64 size) {
    const VAddr end = addr + size;
    for (const auto& [start, stop] : hot_reads) {
        if (!(addr < stop && start < end)) {
            continue;
        }
        const VAddr window = Common::AlignDown(start, ReadbackWindow);
        HotWriter* writer = FindHotWriter(window);
        if (!writer) {
            writer = &hot_writers[hot_writers_next];
            hot_writers_next = (hot_writers_next + 1) % hot_writers.size();
            *writer = HotWriter{.window = window};
        }
        const u64 tick = scheduler.CurrentTick();
        if (writer->first_tick == 0) {
            writer->first_tick = tick;
        }
        writer->last_tick = tick;
        writer->cmd_seq = SotcStats::commands.load(std::memory_order_relaxed);
        writer->program = SotcStats::current_program.load(std::memory_order_relaxed);
        writer->time = std::chrono::steady_clock::now();
        writer->in_render = scheduler.IsRendering();
    }
}

void BufferCache::TraceReadback(VAddr window, bool finish, bool main_written, bool any_written,
                                u64 bytes) {
    const int trace = ReadbackTraceMode();
    if (trace == 0) {
        return;
    }
    HotWriter* writer = FindHotWriter(window);
    if (!writer || writer->first_tick == 0) {
        return;
    }
    const u64 tick = scheduler.CurrentTick();
    const u64 gap_us = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - writer->time)
                           .count();
    // SotcStats::commands restarts at every SOTC FPS line.
    const u64 commands_now = SotcStats::commands.load(std::memory_order_relaxed);
    const u64 gap_cmds = commands_now >= writer->cmd_seq ? commands_now - writer->cmd_seq : 0;
    SotcStats::readback_gap_us.fetch_add(gap_us, std::memory_order_relaxed);
    SotcStats::readback_gap_cmds.fetch_add(gap_cmds, std::memory_order_relaxed);
    SotcStats::readback_traced.fetch_add(1, std::memory_order_relaxed);
    const bool rewritten = finish && writer->first_tick < tick && writer->last_tick == tick;
    if (rewritten) {
        SotcStats::readback_rewritten.fetch_add(1, std::memory_order_relaxed);
    }
    static int shown = 0;
    if (trace == 2 && shown < 200) {
        ++shown;
        std::fprintf(stderr,
                     "SOTC rb %#llx %s main-written %d any-written %d rewritten %d gap %llu us "
                     "%llu cmds writer %#llx%s bytes %llu ticks %llu-%llu now %llu\n",
                     static_cast<unsigned long long>(window), finish ? "finish" : "ahead ",
                     main_written, any_written, rewritten, static_cast<unsigned long long>(gap_us),
                     static_cast<unsigned long long>(gap_cmds),
                     static_cast<unsigned long long>(writer->program),
                     writer->in_render ? " (in pass)" : "", static_cast<unsigned long long>(bytes),
                     static_cast<unsigned long long>(writer->first_tick),
                     static_cast<unsigned long long>(writer->last_tick),
                     static_cast<unsigned long long>(tick));
    }
    writer->first_tick = 0;
}

void BufferCache::RecordCurrentWrite(VAddr addr, u64 size) {
    if (ReadbackTraceMode() != 0) {
        NoteHotWriter(addr, size);
    }
    if (EagerMode() != 0) {
        if (!eager_readbacks.empty()) {
            MarkInflightReadbacksDirty(addr, size);
        }
        NoteHotWindowWrite(addr, size);
    }
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

bool BufferCache::TryReplaceFromCpuInPlace(VAddr addr, u64 size,
                                           const std::function<void(u8*)>& fill) {
    if (size == 0) {
        return true;
    }
    // The writer fills the backing view directly: it must be one contiguous run.
    u8* const first = const_cast<u8*>(memory->GetBackingPointer(addr));
    const u8* const last = memory->GetBackingPointer(addr + size - 1);
    if (!first || last != first + size - 1) {
        return false;
    }
    // Same checks and bookkeeping, in the same order, as TryReplaceFromCpu.
    const VAddr lo = Common::AlignDown(addr, VAddr{4096});
    const VAddr hi = Common::AlignUp(addr + size, VAddr{4096});
    bool foreign = false;
    gpu_modified_ranges.ForEachInRange(lo, hi - lo, [&](VAddr start, VAddr end) {
        foreign |= start < addr || end > addr + size;
    });
    if (foreign) {
        return false;
    }
    gpu_modified_ranges.Subtract(addr, size);
    memory_tracker->UnmarkRegionAsGpuModified(addr, size, false);
    MarkInflightReadbacksDirty(addr, size);
    fill(first);
    memory_tracker->MarkRegionAsCpuModified(addr, size);
    return true;
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
    gpu_modified_ranges.Subtract(addr, size);
    memory_tracker->UnmarkRegionAsGpuModified(addr, size, false);
    MarkInflightReadbacksDirty(addr, size);
    if (!memory->TryWriteBacking(std::bit_cast<void*>(addr), data, size, 10)) {
        return false;
    }
    memory_tracker->MarkRegionAsCpuModified(addr, size);
    return true;
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

void BufferCache::SynchronizeDmaBuffers() {
    fault_process_pending = true;
    // sotc: this runs before every draw and dispatch of a shader with direct memory access and
    // walks the tracking bits of all resident memory. When no page became CPU-modified and
    // residency did not change since the last full pass, everything is still uploaded: skip it.
    // SOTC_DMA_SYNC_MEMO=0 always walks, 2 walks anyway and counts uploads a skip would have
    // missed (SOTC dma line).
    static const int memo = [] {
        const char* v = std::getenv("SOTC_DMA_SYNC_MEMO");
        return v ? std::atoi(v) : 1;
    }();
    // Hot pages change without faults: no skipping while there are any.
    const u64 cpu_generation = g_cpu_dirty_generation.load(std::memory_order_acquire);
    const bool unchanged = cpu_generation == dma_synced_cpu_generation &&
                           resident_generation == dma_synced_resident_generation &&
                           dma_hot_list.empty();
    SotcStats::dma_syncs.fetch_add(1, std::memory_order_relaxed);
    if (memo != 0 && unchanged) {
        SotcStats::dma_sync_skips.fetch_add(1, std::memory_order_relaxed);
        if (memo != 2) {
            return;
        }
    }
    const u64 uploads_before = SotcStats::uploads.load(std::memory_order_relaxed);
    dma_synced_cpu_generation = cpu_generation;
    dma_synced_resident_generation = resident_generation;
    // sotc: all the uploads of the pass in one staging allocation, the guest memory copied by
    // the copy threads (~30 ranges and ~5 MB per pass in SotC, one by one on this thread
    // before). Same order as SynchronizeMemory: the ranges are marked uploaded (and protected)
    // first, then copied, then the GPU copies are recorded. SOTC_DMA_BATCH=1 (off by default:
    // measured neutral, the cost of these uploads is the protect/fault cycle, not the copy).
    static const bool batch = [] {
        const char* v = std::getenv("SOTC_DMA_BATCH");
        return v && *v == '1';
    }();
    static const bool hot_pages = [] {
        const char* v = std::getenv("SOTC_DMA_HOT");
        return v && *v == '1';
    }();
    if (batch || hot_pages) {
        struct Upload {
            const Buffer* arena;
            VAddr addr;
            u64 size;
            u64 staging_offset;
        };
        boost::container::small_vector<Upload, 64> uploads;
        u64 total = 0;
        for (const auto& range : resident_ranges) {
            const u64 page = range.start >> (ARENA_PAGE_BITS - block_shift);
            const VAddr device_addr = range.start << block_shift;
            const u64 size = (range.end - range.start) << block_shift;
            const Buffer* arena = address_space[page];
            memory_tracker->ForEachUploadRange(
                device_addr, size, false,
                [&](u64 addr, u64 bytes) {
                    uploads.push_back({arena, addr, bytes, total});
                    total += bytes;
                },
                hot_pages);
        }
        if (uploads.empty()) {
            if (hot_pages) {
                UpdateDmaHotPages({});
            }
            return;
        }
        const auto staging = staging_pool.Request(total, MemoryType::HostUncached);
        constexpr u64 Chunk = 256_KB;
        struct Piece {
            VAddr source;
            u8* dest;
            u64 size;
        };
        boost::container::small_vector<Piece, 128> pieces;
        for (const auto& upload : uploads) {
            for (u64 done = 0; done < upload.size; done += Chunk) {
                pieces.push_back({upload.addr + done, staging.mapped + upload.staging_offset + done,
                                  std::min(Chunk, upload.size - done)});
            }
        }
        if (batch) {
            BbCopy::ParallelFor(pieces.size(), [&](std::size_t i) {
                memory->CopySparseMemory(pieces[i].source, pieces[i].dest, pieces[i].size);
            });
        } else {
            for (const auto& upload : uploads) {
                memory->CopySparseMemory(upload.addr, staging.mapped + upload.staging_offset,
                                         upload.size);
            }
        }
        staging.Flush();
        boost::container::small_vector<vk::BufferCopy, 64> copies;
        for (size_t i = 0; i < uploads.size();) {
            const Buffer* arena = uploads[i].arena;
            copies.clear();
            for (; i < uploads.size() && uploads[i].arena == arena; ++i) {
                copies.push_back({staging.offset + uploads[i].staging_offset,
                                  uploads[i].addr - arena->cpu_addr, uploads[i].size});
            }
            runtime.CopyBuffer(staging.buffer, arena, copies);
            for (const auto& copy : copies) {
                RecordCurrentWrite(arena->cpu_addr + copy.dstOffset, copy.size);
            }
        }
        SotcStats::uploads.fetch_add(uploads.size(), std::memory_order_relaxed);
        SotcStats::upload_bytes.fetch_add(total, std::memory_order_relaxed);
        if (hot_pages) {
            boost::container::small_vector<std::pair<VAddr, u64>, 64> ranges;
            for (const auto& upload : uploads) {
                ranges.emplace_back(upload.addr, upload.size);
            }
            UpdateDmaHotPages(ranges);
        }
        if (memo == 2 && unchanged &&
            g_cpu_dirty_generation.load(std::memory_order_acquire) == cpu_generation) {
            SotcStats::dma_sync_bad.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (ARENA_PAGE_BITS - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        SynchronizeMemory(address_space[page], device_addr, size, false, false);
    }
    // A write fault during the walk changes the generation: not a missed upload.
    if (memo == 2 && unchanged &&
        g_cpu_dirty_generation.load(std::memory_order_acquire) == cpu_generation &&
        SotcStats::uploads.load(std::memory_order_relaxed) != uploads_before) {
        SotcStats::dma_sync_bad.fetch_add(1, std::memory_order_relaxed);
    }
}

void BufferCache::UpdateDmaHotPages(std::span<const std::pair<VAddr, u64>> uploaded) {
    static const u64 budget_pages = [] {
        const char* v = std::getenv("SOTC_DMA_HOT_KB");
        return (v ? std::strtoull(v, nullptr, 10) : 16384ULL) / 4;
    }();
    static const u32 streak_needed = [] {
        const char* v = std::getenv("SOTC_DMA_HOT_STREAK");
        return v ? static_cast<u32>(std::strtoul(v, nullptr, 10)) : 3u;
    }();
    constexpr u64 PageBits = 12;
    constexpr u64 TableBits = 14;
    if (dma_streaks.empty()) {
        dma_streaks.resize(1ULL << TableBits);
    }
    const auto entry_of = [&](VAddr page) -> DmaStreak& {
        return dma_streaks[((page >> PageBits) * 0x9E3779B97F4A7C15ULL) >> (64 - TableBits)];
    };
    const u64 frame = SotcStats::flips.load(std::memory_order_relaxed);
    // Once per frame a 1/64 of the hot pages goes back to being protected: one the game stopped
    // writing is no longer uploaded every pass; one still written faults and comes back.
    if (frame != dma_hot_frame) {
        dma_hot_frame = frame;
        const size_t count = std::max<size_t>(1, dma_hot_list.size() / 64);
        for (size_t i = 0; i < count && !dma_hot_list.empty(); ++i) {
            if (dma_hot_cursor >= dma_hot_list.size()) {
                dma_hot_cursor = 0;
            }
            const VAddr page = dma_hot_list[dma_hot_cursor];
            memory_tracker->ClearHot(page, 1ULL << PageBits);
            if (DmaStreak& e = entry_of(page); e.page == page) {
                e = DmaStreak{};
            }
            dma_hot_list[dma_hot_cursor] = dma_hot_list.back();
            dma_hot_list.pop_back();
            SotcStats::dma_hot_demotions.fetch_add(1, std::memory_order_relaxed);
        }
    }
    VAddr run_start = 0;
    u64 run_size = 0;
    const auto flush_run = [&] {
        if (run_size != 0) {
            memory_tracker->SetHot(run_start, run_size);
            run_size = 0;
        }
    };
    for (const auto& [addr, size] : uploaded) {
        for (VAddr page = addr; page < addr + size; page += 1ULL << PageBits) {
            DmaStreak& e = entry_of(page);
            if (e.page != page) {
                e = DmaStreak{.page = page, .frame = frame, .streak = 1};
                continue;
            }
            if (e.hot || e.frame == frame) {
                continue;
            }
            e.streak = e.frame + 1 == frame ? e.streak + 1 : 1;
            e.frame = frame;
            if (e.streak < streak_needed || dma_hot_list.size() >= budget_pages) {
                continue;
            }
            e.hot = true;
            dma_hot_list.push_back(page);
            SotcStats::dma_hot_promotions.fetch_add(1, std::memory_order_relaxed);
            if (run_size != 0 && run_start + run_size == page) {
                run_size += 1ULL << PageBits;
            } else {
                flush_run();
                run_start = page;
                run_size = 1ULL << PageBits;
            }
        }
    }
    flush_run();
    SotcStats::dma_hot_pages.store(dma_hot_list.size(), std::memory_order_relaxed);
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
    // Residency only grows (ReleaseMemory clears the memo): a block range proven resident once
    // needs no new search over resident_ranges. SOTC_RESIDENT_MEMO=0 disables it (A/B).
    static const bool use_memo = [] {
        const char* v = std::getenv("SOTC_RESIDENT_MEMO");
        return !v || *v != '0';
    }();
    ResidentMemo* memo = nullptr;
    if (use_memo) {
        memo = &resident_memo[(first_block * 0x9E3779B97F4A7C15ULL) >> 54];
        if (memo->first == first_block && last_block <= memo->last) {
            return;
        }
    }
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        if (memo) {
            memo->last = memo->first == first_block ? std::max(memo->last, last_block) : last_block;
            memo->first = first_block;
        }
        return;
    }

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
        ++resident_generation;

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
        SotcStats::uploads.fetch_add(1, std::memory_order_relaxed);
        SotcStats::upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
        const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
        for (auto& copy : copies) {
            memory->CopySparseMemory(copy.dstOffset, staging.mapped + copy.srcOffset, copy.size);
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
    // Only the two paths below write the arena range from the GPU. Recording a write when
    // nothing is written made readbacks of this range take the slow (finish) path.
    if (TexelSyncLegacy()) {
        RecordCurrentWrite(device_addr, size);
    }
    SotcStats::Bump(SotcStats::texel_syncs[0]);
    // sotc: SOTC_TEXEL_SYNC_MEMO=0 always looks both up (TexelSyncMemo).
    static const bool use_memo = [] {
        const char* v = std::getenv("SOTC_TEXEL_SYNC_MEMO");
        return !v || *v != '0';
    }();
    TexelSyncMemo& memo = texel_sync_memo[(device_addr * 0x9E3779B97F4A7C15ULL) >> 55];
    const u64 image_set_generation = texture_cache.ImageSetGeneration();
    const u64 meta_generation = texture_cache.MetaGeneration();
    const u64 meta_clear_generation = texture_cache.MetaClearGeneration();
    if (use_memo && memo.address == device_addr && memo.size == size &&
        memo.image_set_generation == image_set_generation &&
        memo.meta_generation == meta_generation &&
        memo.meta_clear_generation == meta_clear_generation) {
        return false;
    }
    if (auto type = texture_cache.IsMeta(device_addr)) {
        SotcStats::Bump(SotcStats::texel_syncs[1]);
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            RecordCurrentWrite(device_addr, size);
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    // sotc: only an image starting at device_addr counts, and it is registered in the granule
    // holding that address: none there (most texel buffers), none at all, without the walk.
    bool any_image = false;
    const ImageId image_id =
        texture_cache.HasImagesIn(device_addr, 1)
            ? texture_cache.FindImageFromRange(device_addr, size, true, &any_image)
            : ImageId{};
    if (!image_id) {
        if (!any_image) {
            memo = {device_addr, size, image_set_generation, meta_generation,
                    meta_clear_generation};
        }
        return false;
    }
    SotcStats::Bump(SotcStats::texel_syncs[2]);
    Image& image = texture_cache.GetImage(image_id);
    // sotc: this image's contents reach buffer memory the CPU may read back: draws into it are
    // never skipped while their pipeline compiles (Rasterizer::MaySkipForCompile).
    texel_alias_targets.insert(image.info.guest_address);
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
    // TileImage writes every copied mip: record the whole extent, at least the bound range.
    u64 extent = size;
    for (u32 mip = 0; mip < buffer_copies.size(); ++mip) {
        const auto& mip_info = image.info.mips_layout[mip];
        extent = std::max<u64>(extent, mip_info.offset + mip_info.size);
    }
    RecordCurrentWrite(device_addr, extent);
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
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
    const auto grow = [&](u32 memory_type) -> std::optional<std::pair<vk::DeviceMemory, u64>> {
        const u64 chunk_blocks = std::max<u64>(ARENA_CHUNK_SIZE >> block_shift, blocks);
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
    auto placed = grow(arena_memory_type_index);
    ASSERT_MSG(placed.has_value(), "Failed to allocate buffer arena memory");
    return *placed;
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
    boost::container::small_vector<Backing, 16> released;
    resident_ranges.ForEachInRange(first_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(first_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        if (start < end) {
            released.push_back(backing.SubRange(start, end));
        }
    });
    if (released.empty()) {
        return;
    }
    // Work recorded before the unmap may still write this memory, and sparse binds are not
    // ordered against earlier submissions: finish that work before unbinding, or the GPU
    // writes to unbound memory (WriteInvalid, device lost). Unmaps are rare, the sync is cheap.
    scheduler.Finish();
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
    std::erase_if(hot_readbacks, [&](const HotReadback& hot) {
        return hot.addr < (end_block << block_shift) &&
               (first_block << block_shift) < hot.addr + hot.size;
    });
    resident_ranges.Subtract(first_block, end_block);
    ++resident_generation;
    resident_memo.fill({});
    gpu_modified_ranges.Subtract(first_block << block_shift,
                                 (end_block - first_block) << block_shift);
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
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
