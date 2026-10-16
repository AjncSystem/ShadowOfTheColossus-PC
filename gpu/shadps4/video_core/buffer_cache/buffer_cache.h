// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <vector>
#include <unordered_map>
#include <unordered_set>
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
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false);

    /// sotc: like TryReplaceFromCpu, but fill() writes the new contents straight into the
    /// backing view (contiguous ranges only; false otherwise, nothing written).
    bool TryReplaceFromCpuInPlace(VAddr addr, u64 size, const std::function<void(u8*)>& fill);

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
    /// sotc: generations at the last full SynchronizeDmaBuffers (SOTC_DMA_SYNC_MEMO).
    u64 dma_synced_cpu_generation{};
    u64 dma_synced_resident_generation{~u64{0}};
    u64 resident_generation{};
    /// sotc (SOTC_DMA_HOT): pages the direct-memory pass uploaded in consecutive frames are kept
    /// CPU-modified and writable instead of protected again (no write fault, no VirtualProtect).
    struct DmaStreak {
        VAddr page = ~VAddr{0};
        u64 frame{};
        u32 streak{};
        bool hot{};
    };
    std::vector<DmaStreak> dma_streaks;
    std::vector<VAddr> dma_hot_list;
    size_t dma_hot_cursor{};
    u64 dma_hot_frame{~u64{0}};
    void UpdateDmaHotPages(std::span<const std::pair<VAddr, u64>> uploaded);

    /// Commits pending sparse buffer memory binds. Must be called before every scheduler submit.
    void SubmitPendingArenaBinds(Vulkan::SubmitInfo& info);

    /// Releases the arena memory backing a range the guest unmapped. GPU thread only.
    void ReleaseMemory(VAddr device_addr, u64 size);

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
    /// Guest reads fault on GPU-modified memory; the readback copies this window around them.
    static constexpr u64 ReadbackWindow = 512ULL * 1024;

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
                        bool subtract = true, bool hot_extras = true);
    /// Writes downloaded data (mapped, at the copies' destination offsets minus mapped_offset)
    /// into guest memory and unmarks the ranges as GPU-modified.
    void ApplyDownloads(std::span<const ReadbackItem> pending, const u8* mapped,
                        u64 mapped_offset);

    /// sotc (default, SOTC_RB_ASYNC=0 turns it off): a readback asked by a guest thread in two steps on the command
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
    /// sotc: an asynchronous readback in flight one of whose windows holds [addr, addr + size).
    AsyncReadback* FindInflightWindow(VAddr addr, u64 size);
    /// sotc: ends a guest write fault served asynchronously: GPU data written there since the
    /// readback's copy is read back first, then the bytes become CPU-modified.
    void GiveToGuestWrite(VAddr device_addr, u64 size);
    void MarkInflightReadbacksDirty(VAddr addr, u64 size);
    std::vector<AsyncReadback*> inflight_readbacks;

    /// sotc (SOTC_RB_EAGER): a read-back window copied to staging right behind the GPU work
    /// that wrote it, in the same submission. Its readback then only waits for that submission
    /// instead of for everything submitted until the guest asks (the ahead and finish paths).
    struct EagerReadback {
        const Buffer* arena{};
        VAddr window{};
        u64 size{};
        u64 tick{};
        bool dirty{};
        std::chrono::steady_clock::time_point recorded_at{};
        u64 download_offset{};
        u8* download_mapped{};
        Buffer* download_buffer{};
        u64 download_size{};
        boost::container::small_vector<ReadbackItem, 16> pending;
    };
    static constexpr size_t MaxEagerReadbacks = 8;
    std::vector<EagerReadback> eager_readbacks;
    /// Hot windows written by the command buffer being built (tick hot_written_tick).
    boost::container::small_vector<std::pair<VAddr, u64>, 8> hot_written_windows;
    u64 hot_written_tick{};
    void DropEagerReadback(size_t index, bool unused);
    /// Serves the readback of [window, +size) from its eager copy; false: use the usual path.
    bool TryEagerReadback(VAddr window, u64 size);
    void NoteHotWindowWrite(VAddr addr, u64 size);

public:
    /// 0 off, 1 on, 2 on and every hit compared with a fresh readback.
    static int EagerMode();
    /// Records the copies of the hot windows written by the command buffer being built; the
    /// caller submits it next.
    void RecordEagerReadbacks();

private:

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
    /// Block ranges already proven resident (first block, last block), by hash of the first.
    struct ResidentMemo {
        u64 first = ~0ULL;
        u64 last = 0;
    };
    std::array<ResidentMemo, 1024> resident_memo{};
    /// sotc: texel buffer ranges SynchronizeMemoryFromImage found with no metadata surface and no
    /// image starting at them, and the image and metadata sets then (TextureCache generations).
    /// Heavy frames read ~20K texel buffer ranges, none aliasing an image: two lookups each.
    struct TexelSyncMemo {
        VAddr address{};
        u32 size{};
        u64 image_set_generation{~0ULL};
        u64 meta_generation{};
        u64 meta_clear_generation{};
    };
    std::array<TexelSyncMemo, 512> texel_sync_memo{};
    std::unordered_set<VAddr> texel_alias_targets;

public:
    /// An image at this address was copied into buffer memory (texel-buffer alias).
    [[nodiscard]] bool IsTexelAliasTarget(VAddr address) const {
        return texel_alias_targets.contains(address);
    }

private:
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
    /// sotc (SOTC_RB_TRACE): the last GPU writes into each read-back window, to measure how far
    /// the writer is from the request that reads it.
    struct HotWriter {
        VAddr window = ~VAddr{0};
        u64 first_tick{};
        u64 last_tick{};
        u64 cmd_seq{};
        u64 program{};
        std::chrono::steady_clock::time_point time{};
        bool in_render{};
    };
    std::array<HotWriter, 8> hot_writers{};
    size_t hot_writers_next{};
    void NoteHotWriter(VAddr addr, u64 size);
    HotWriter* FindHotWriter(VAddr window);
    void TraceReadback(VAddr window, bool finish, bool main_written, bool any_written, u64 bytes);
    /// sotc (SOTC_RB_DEDUP): guest threads faulting on a window another thread is already
    /// reading back wait for that readback instead of queueing their own.
    struct GuestReadSlot {
        VAddr window = ~VAddr{0};
        std::atomic<u32> done{0};
    };
    static constexpr size_t NumGuestReadSlots = 16;
    std::array<GuestReadSlot, NumGuestReadSlots> guest_read_slots{};
    std::mutex guest_read_mutex;
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
    /// sotc: the range the guest touched, during the DownloadMemory of its window (size 0 else).
    VAddr fault_addr{};
    u64 fault_size{};

public:
    /// sotc: true once after the command buffer being built wrote a page the guest reads back:
    /// submitting it now lets the GPU finish it before the guest asks for it.
    bool TakeHotWrite() noexcept;
    /// sotc: whether TakeHotWrite would return true, without consuming it.
    [[nodiscard]] bool HotWritePending() const noexcept {
        return hot_write_pending;
    }

private:

    /// sotc: arena memory is carved out of chunks of ARENA_CHUNK_SIZE. One allocation per
    /// resident range reached thousands within minutes, each one a kernel call mid-frame (the
    /// open world then ran out of video memory).
    static constexpr u64 ARENA_CHUNK_SIZE = 64_MB;
    struct ArenaChunk {
        vk::DeviceMemory memory;
        u32 memory_type{};
        u64 used_blocks{};
        std::map<u64, u64> free_runs; ///< first block -> number of free blocks
    };
    std::vector<std::unique_ptr<ArenaChunk>> arena_chunks;
    std::unordered_map<VkDeviceMemory, ArenaChunk*> chunk_by_memory;
    /// Memory and first block for blocks of arena backing (video memory, else system memory).
    std::pair<vk::DeviceMemory, u64> AllocateArenaBlocks(u64 blocks);
    void FreeArenaBlocks(vk::DeviceMemory memory, u64 start, u64 blocks);

    u32 arena_memory_type_index{};
    std::optional<u32> arena_fallback_memory_type_index;
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
};

} // namespace VideoCore
