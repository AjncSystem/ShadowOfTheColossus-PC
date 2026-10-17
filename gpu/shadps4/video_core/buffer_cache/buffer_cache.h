// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include <chrono>
#include <deque>
#include <span>
#include <vector>
#include <unordered_map>
#include <boost/container/small_vector.hpp>

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
struct SubmitInfo;
class Runtime;
class StagingBufferPool;
} // namespace Vulkan

namespace VideoCore {

class TextureCache;
class MemoryTracker;
class PageManager;

class BufferCache {
    static constexpr u64 ADDRESS_SPACE_BITS = 40;
    static constexpr u64 ARENA_PAGE_BITS = 32;
    static constexpr u64 ARENA_PAGE_SIZE = u64{1} << ARENA_PAGE_BITS;
    static constexpr u64 NUM_ARENA_PAGES = u64{1} << (ADDRESS_SPACE_BITS - ARENA_PAGE_BITS);
    static constexpr u64 MIN_BLOCK_SIZE = 16_KB;
    static constexpr u64 STREAM_THRESHOLD = 16_KB;

public:
    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool,
                         TextureCache& texture_cache, PageManager& tracker);
    ~BufferCache();

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept {
        return bda_pagetable_buffer.get();
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager->GetFaultBuffer();
    }

    /// Retrieves the stream buffer.
    [[nodiscard]] StreamBuffer& GetStreamBuffer() noexcept {
        return stream_buffer;
    }

    /// Returns minimum granularity of a sparse memory bind.
    u32 GetSparsePageShift() const noexcept {
        return block_shift;
    }

    void TickFrame();

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false,
                          bool guest_fault = false);

    /// sotc: forgets that the GPU wrote this range, without downloading it (unmap test).
    void DropGpuModified(VAddr device_addr, u64 size);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false, bool assume_locks = false);

    /// Finds a buffer for the specified region.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                             bool is_written,
                                                             bool is_texel_buffer = false);

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// sotc: a guest write fault at addr: unprotect the following pages too (MemoryTracker::
    /// MarkAheadAsCpuModified). SOTC_WRITE_AHEAD_PAGES (default 15, 0 = off).
    void MarkWriteAhead(VAddr addr);

    /// sotc: writes `data` to guest memory from the CPU as a GPU write would, when no other
    /// GPU-written bytes share its pages: the range stops being GPU-modified, so reading it
    /// back costs no readback. False (nothing done) otherwise.
    bool TryReplaceFromCpu(VAddr addr, const void* data, u64 size);

    /// sotc: TryReplaceFromCpu that also copies `data` into the resident arena right away, so
    /// the range is not marked CPU-written: no unprotect, upload and protect again later (two
    /// page protection system calls per replaced range). Data the guest never writes itself
    /// (fill-shader tables written by the command processor). Not resident: TryReplaceFromCpu.
    bool TryWriteThroughFromCpu(VAddr addr, const void* data, u64 size);

    /// Byte-precise: true when a written binding covered any byte of the range since its last
    /// readback. Readbacks copy only these bytes, so the backing holds every other byte.
    /// Command processor thread only.
    [[nodiscard]] bool IsRangeGpuWritten(VAddr addr, size_t size) const {
        return gpu_modified_ranges.Intersects(addr, size);
    }

    /// Reads [addr, addr + size) from the backing when no byte of it is GPU-written, through a
    /// cache of pages holding their backing pointer and the hull of their GPU-written bytes.
    /// From Pink-shadPS4 (luizgustavs). Command processor thread only.
    bool ReadClean(VAddr addr, void* out, u32 size);

    /// A GPU mapping changed: cached backing pointers may be stale. Any thread.
    void OnMappingChanged() {
        clean_map_generation.fetch_add(1, std::memory_order_release);
    }

    /// Synchronizes all buffers needed for DMA.
    void SynchronizeDmaBuffers();

    /// Commits pending sparse buffer memory binds. Must be called before every scheduler submit.
    void SubmitPendingArenaBinds(Vulkan::SubmitInfo& info);

    /// Releases the arena memory backing a range the guest unmapped. GPU thread only.
    void ReleaseMemory(VAddr device_addr, u64 size);

    /// sotc: gives back arena allocations whose blocks were not bound for a while and hold no
    /// GPU-written data, once resident arena memory passes SOTC_ARENA_EVICT_MB. Called between
    /// commands outside a render pass; true when it submitted (it waits for the GPU).
    bool MaybeEvictCold(std::chrono::steady_clock::time_point now);

    /// sotc: a shader reached [addr, addr + size) through a pointer and faulted: blocks of it
    /// that MaybeEvictCold gave back stay resident from now on. Before the rebind.
    void NoteFaultRebind(VAddr addr, u64 size);

private:
    struct ArenaBinds {
        const Buffer* arena;
        boost::container::small_vector<vk::SparseMemoryBind, 32> binds;
    };

    ArenaBinds* BindsForArena(const Buffer* arena) {
        auto it = std::ranges::find(pending_binds, arena, &ArenaBinds::arena);
        if (it != pending_binds.end()) {
            return std::addressof(*it);
        }
        return &pending_binds.emplace_back(arena);
    }

    const Buffer* GetArena(u64 first_block, u64 last_block);

    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block);

    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);

    /// One range to read back: its GPU-modified parts, as copies from the arena.
    struct ReadbackItem {
        const Buffer* arena;
        VAddr addr;
        u64 size;
        boost::container::small_vector<vk::BufferCopy, 4> copies;
        /// The GPU-modified ranges downloaded, when GatherDownloads leaves them in the set.
        boost::container::small_vector<std::pair<VAddr, u64>, 4> gpu_ranges;
    };
    /// Collects what a readback of [device_addr, +size) and the hot readbacks must copy, taking
    /// it out of the GPU-modified set. Returns the bytes to download.
    u64 GatherDownloads(const Buffer* arena, VAddr device_addr, u64 size,
                        boost::container::small_vector<ReadbackItem, 16>& pending,
                        bool subtract = true);
    /// Writes downloaded data (mapped, at the copies' destination offsets minus mapped_offset)
    /// into guest memory and unmarks the ranges as GPU-modified.
    void ApplyDownloads(std::span<const ReadbackItem> pending, const u8* mapped,
                        u64 mapped_offset);

    /// sotc (SOTC_RB_ASYNC=1): a readback asked by a guest thread in two steps on the command
    /// processor: the copies are recorded and submitted without waiting (the command processor
    /// goes on with its queue), the guest thread waits for the GPU, then the data is applied
    /// unless a later GPU write hit the ranges meanwhile (then it is read back again).
    struct AsyncReadback {
        VAddr device_addr{};
        u64 size{};
        bool is_write{};
        bool waiting{};
        bool dirty{};
        bool dependent{};
        bool finished{};
        u64 tick{};
        const Buffer* arena{};
        VAddr window_start{};
        u64 window_size{};
        u64 download_offset{};
        const u8* download_mapped{};
        Buffer* download_buffer{};
        u64 download_size{};
        boost::container::small_vector<ReadbackItem, 16> pending;
    };
    void BeginAsyncReadback(AsyncReadback& readback);
    void FinishAsyncReadback(AsyncReadback& readback);
    AsyncReadback* FindInflightReadback(VAddr addr, u64 size);
    void MarkInflightReadbacksDirty(VAddr addr, u64 size);
    std::vector<AsyncReadback*> inflight_readbacks;

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);

    bool SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    Vulkan::Runtime& runtime;
    Vulkan::StagingBufferPool& staging_pool;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    std::unique_ptr<MemoryTracker> memory_tracker;

    StreamBuffer stream_buffer;
    Buffer gds_buffer;
    RangeSet gpu_modified_ranges;

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;
    bool fault_process_pending{};

    std::array<const Buffer*, NUM_ARENA_PAGES> address_space{};
    std::deque<Buffer> arenas;
    std::vector<ArenaBinds> pending_binds;
    Vulkan::Semaphore memory_semaphore;

    struct Backing : public Interval {
        vk::DeviceMemory memory;
        u64 offset;
        constexpr bool CanMergeWith(const Backing& other) const noexcept {
            return memory == other.memory && offset + (end - start) == other.offset;
        }
        constexpr Backing SubRange(u64 a, u64 b) const noexcept {
            return {{a, b}, memory, offset + (a - start)};
        }
    };
    IntervalList<Backing> resident_ranges;
    /// sotc: bumped at every resident_ranges.Add/Subtract. SynchronizeDmaBuffers rebuilds its
    /// merged runs only when this (or the arena count) changed since the last rebuild.
    u64 resident_version{};
    /// sotc: block ranges found resident at a resident_version (EnsureResident).
    struct ResidentCacheEntry {
        u64 version{~u64{0}};
        u64 first{};
        u64 end{};
        u16 epoch{}; ///< sotc: use epoch its blocks were last stamped with
    };
    std::array<ResidentCacheEntry, 64> resident_cache{};
    u64 dma_runs_version = ~0ULL;
    size_t dma_runs_arenas{};
    u64 dma_runs_ranges{};
    /// sotc (SOTC_DMA_MERGE): touching resident ranges whose start lies in the same arena,
    /// merged into runs of (device address, bytes). Only rebuilt at the top of a sweep.
    std::vector<std::pair<VAddr, u64>> dma_runs;
    /// Readback windows requested recently; downloaded together with the next readback.
    struct HotReadback {
        VAddr addr;
        u64 size;
        std::chrono::steady_clock::time_point last_request;
    };
    static constexpr size_t MaxHotReadbacks = 64;
    static constexpr auto HotReadbackLifetime = std::chrono::seconds(2);
    std::vector<HotReadback> hot_readbacks;
    /// sotc: pages the guest read back recently (the faulting ranges, not the windows). A GPU
    /// write into one of them asks for an early submit (TakeHotWrite).
    static constexpr size_t NumHotReads = 32;
    std::array<std::pair<VAddr, VAddr>, NumHotReads> hot_reads{};
    size_t hot_reads_next{};
    bool hot_write_pending{};
    void NoteHotRead(VAddr addr, u64 size);
    struct CleanPage {
        VAddr page_addr = ~0ULL;
        u64 map_generation{};
        const u8* backing{};
        u32 dirty_lo{};
        u32 dirty_hi{};
    };
    static constexpr u64 CleanPageBits = 12;
    static constexpr u64 CleanPageSize = 1ULL << CleanPageBits;
    static constexpr size_t NumCleanPages = 1024;
    std::array<CleanPage, NumCleanPages> clean_pages{};
    bool clean_pages_used = false;
    std::atomic<u64> clean_map_generation{};
    void AddGpuModified(VAddr addr, u64 size);

    /// GPU writes recorded into the command buffer still being built (tick current_writes_tick).
    RangeSet current_writes;
    u64 current_writes_tick{};
    void RecordCurrentWrite(VAddr addr, u64 size);

public:
    /// sotc: true once after the command buffer being built wrote a page the guest reads back:
    /// submitting it now lets the GPU finish it before the guest asks for it.
    bool TakeHotWrite() noexcept;

private:

    /// Number of resident blocks still bound to each backing allocation.
    std::unordered_map<VkDeviceMemory, u64> backing_blocks;
    /// sotc: size of each backing allocation (resident arena memory accounting).
    std::unordered_map<VkDeviceMemory, u64> backing_bytes;
    /// sotc: unbinds resident block ranges (zeroing their page table entries) and frees the
    /// allocations left without blocks. The GPU must no longer use them.
    void UnbindResident(std::span<const Backing> released);
    /// sotc: per arena block, the use epoch (seconds, wrapping, 0 = never) of its last bind.
    u16* block_last_use{};
    /// sotc: blocks MaybeEvictCold gave back and nothing bound again yet.
    IntervalList<> evicted_ranges;
    /// sotc: evicted blocks a shader faulted back in; never evicted again.
    IntervalList<> pinned_ranges;
    /// sotc: allocations whose read-back did not match guest memory, tried again after this.
    std::unordered_map<VkDeviceMemory, std::chrono::steady_clock::time_point> evict_retry;
    u16 use_epoch{1};
    /// sotc: sets use_epoch from the clock (once per submit and per eviction check).
    void AdvanceUseEpoch(std::chrono::steady_clock::time_point now) noexcept {
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(now - epoch_start).count();
        use_epoch = static_cast<u16>(seconds % 65535 + 1); // 0 = never bound
    }
    std::chrono::steady_clock::time_point epoch_start{};
    std::chrono::steady_clock::time_point next_evict_check{};
    /// sotc: the arena slab EnsureResident hands block runs out of, its size and use (bytes).
    vk::DeviceMemory arena_slab{};
    u64 arena_slab_size{};
    u64 arena_slab_used{};

    u32 arena_memory_type_index{};
    std::optional<u32> arena_fallback_memory_type_index;
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
};

} // namespace VideoCore
