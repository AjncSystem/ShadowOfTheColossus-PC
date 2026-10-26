// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <mutex>

#include <chrono>
#include <shared_mutex>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include <unordered_map>
#include <boost/container/small_vector.hpp>

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/buffer_cache/rb_intervals.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"

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
struct StagingBufferRef;
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

    /// sotc: changes whenever a resident arena range is added or removed, i.e. whenever a
    /// previously bound buffer handle or offset may have moved (SOTC_FAST_BIND).
    u64 BindGeneration() const noexcept {
        return resident_version;
    }

    /// sotc: stream-buffer wrap count (flat buffers and other per-draw uploads live there).
    u64 StreamGeneration() const noexcept {
        return stream_buffer.WrapGeneration();
    }

    void TickFrame();

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false,
                          bool guest_fault = false);

    /// sotc: forgets that the GPU wrote this range, without downloading it (unmap test).
    void DropGpuModified(VAddr device_addr, u64 size);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false, bool assume_locks = false);

    /// sotc: BbToggle::BufferBindMemo. What a read-only ObtainBuffer relies on, remembered by the
    /// caller that wants to repeat a binding without calling again: the buffer registry (resident
    /// set, which every arena creation also bumps; the arena table), the use epoch (EnsureResident
    /// stamps the blocks once per epoch) and the CPU bits of the range's 256 KiB words (the page
    /// look of SynchronizeMemory found none). While BindTokenValid holds, calling ObtainBuffer
    /// again with the same arguments would return the same pair and do nothing else.
    struct BindToken {
        CpuGenKey gen{};
        u64 resident_version{};
        u16 epoch{};
        bool valid{};
    };

    /// Finds a buffer for the specified region. `token` (optional): filled when the call was a
    /// plain read-only binding, ranges of at most 16 words in one region, that uploaded nothing.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                             bool is_written,
                                                             bool is_texel_buffer = false,
                                                             BindToken* token = nullptr);

    /// True while a token from ObtainBuffer still describes the registry and CPU bits.
    [[nodiscard]] bool BindTokenValid(const BindToken& token) const noexcept;

    /// Attempts to obtain a buffer without modifying the cache contents.
    /// sotc: prefer_arena: a CPU-fed image that keeps being dirtied (RefreshImage counts its uploads)
    /// goes through the synchronized arena like a buffer, so only the pages the game wrote are
    /// copied again instead of the whole image (SOTC_IMG_ARENA_MB).
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size,
                                                                     bool prefer_arena = false);

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
    /// sotc: the DMA sweep restricted to [addr, addr + size): for shaders whose raw pointer reads
    /// are known (vk_shader_hle.cpp KnownDmaRange). The range is made resident and uploaded as a
    /// binding would be; the fault buffer is still processed after the submit.
    bool SynchronizeDmaRange(VAddr addr, u64 size);

    /// Commits pending sparse buffer memory binds. Must be called before every scheduler submit.
    void SubmitPendingArenaBinds(Vulkan::SubmitInfo& info);

    /// Releases the arena memory backing a range the guest unmapped. GPU thread only.
    void ReleaseMemory(VAddr device_addr, u64 size);

    /// sotc: gives back arena allocations whose blocks were not bound for a while and hold no
    /// GPU-written data, once resident arena memory passes SOTC_ARENA_EVICT_MB. Called between
    /// commands outside a render pass. A check records the read-back of the coldest ones; a
    /// later call, once the GPU has done it, compares and unbinds. True when it submitted.
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

    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block, u8 bind_class = 2);

    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);

    /// sotc: the range [first, second) of the arena that a readback of [device_addr, +size)
    /// downloads and unmarks as GPU-modified: 512 KiB around it, smaller with SOTC_RB_NARROW.
    std::pair<VAddr, VAddr> ReadbackWindow(const Buffer* arena, VAddr device_addr, u64 size);

    /// sotc: what told MarkInflightReadbacksDirty that bytes changed (READBACK-PLAN.md).
    enum DirtyCause : u8 { CauseGpuWrite, CauseApplied, CauseDrop, CauseCpuReplace, CauseCount };
    struct StaleRange {
        VAddr lo;
        VAddr hi;
        u8 cause;
    };
    /// One range to read back: its GPU-modified parts, as copies from the arena.
    struct ReadbackItem {
        const Buffer* arena;
        VAddr addr;
        u64 size;
        boost::container::small_vector<vk::BufferCopy, 4> copies;
        /// The GPU-modified ranges downloaded; they stay in the set until SubtractDownloaded.
        boost::container::small_vector<std::pair<VAddr, u64>, 4> gpu_ranges;
        /// sotc: brought along by the hot readbacks (SOTC_RB_HOT), not asked for.
        bool hot = false;
    };
    /// Collects what a readback of [device_addr, +size) and the hot readbacks must copy. Returns
    /// the bytes to download. sotc: the ranges stay in the GPU-modified set until the caller
    /// applied the data and called SubtractDownloaded (the command processor reads clean ranges
    /// from the backing meanwhile, with the draw thread); subtract: ranges with nothing bound
    /// to copy leave the set right away.
    /// sotc: hot = false (the repeat rounds of ReadMemory) collects only [device_addr, +size) and
    /// does not touch the hot readback list.
    u64 GatherDownloads(const Buffer* arena, VAddr device_addr, u64 size,
                        boost::container::small_vector<ReadbackItem, 16>& pending,
                        bool subtract = true, bool hot = true);
    /// sotc: takes the downloaded ranges of items out of the GPU-modified set.
    void SubtractDownloaded(std::span<const ReadbackItem> items);
    /// Writes downloaded data (mapped, at the copies' destination offsets minus mapped_offset)
    /// into guest memory and unmarks the ranges as GPU-modified.
    /// sotc: partial (BbToggle::ReadbackIntervals, FinishAsyncReadbackFine): the items hold only
    /// the bytes to apply; their pages are not unmarked here but by UnmarkPagesWithoutGpuBytes
    /// after SubtractDownloaded.
    void ApplyDownloads(std::span<const ReadbackItem> pending, const u8* mapped,
                        u64 mapped_offset, bool partial = false);
    /// sotc: tells the readbacks in flight that the bytes of `item` were applied (the guest may
    /// write them now) and the pages it covers are no longer GPU-modified.
    void NoteApplied(const ReadbackItem& item);
    /// sotc: unmarks the pages of the items' gpu_ranges that hold no byte of gpu_modified_ranges
    /// (the check MarkWrittenByGuest makes); a page with GPU bytes keeps its bit and protection.
    void UnmarkPagesWithoutGpuBytes(std::span<const ReadbackItem> items);

    /// sotc (default, SOTC_RB_ASYNC=0 turns it off): a readback asked by a guest thread in two steps on the command
    /// processor: the copies are recorded and submitted without waiting (the command processor
    /// goes on with its queue), the guest thread waits for the GPU, then the data is applied
    /// unless a later GPU write hit the ranges meanwhile (then it is read back again).
    struct AsyncReadback {
        VAddr device_addr{};
        u64 size{};
        bool is_write{};
        bool narrow{}; ///< sotc: a repeat round of ReadMemory: only the asked pages, no hot readbacks
        bool again{};  ///< sotc: set by Finish (fine path): asked pages still hold GPU bytes
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
        /// sotc: the byte intervals events changed while it was in flight (every one that touches
        /// its regions or GPU page runs), with their cause (DirtyCause); `dirty` keeps the older
        /// whole-region test of the old path. More than MaxStaleRanges after merging: stale_overflow
        /// and the old path.
        boost::container::small_vector<StaleRange, 16> stale;
        bool stale_overflow{};
        u8 first_cause{0xFF};
        u64 seq{}; ///< sotc: DispatchFastPath, inflight_gen value when it entered inflight_readbacks
        /// sotc: DispatchFastPath, [hull_lo, hull_hi) holds every item region and GPU page run of
        /// `pending` (fixed once the readback is in flight): a range outside it touches none.
        VAddr hull_lo{~VAddr{0}}, hull_hi{0};
    };
    static constexpr size_t MaxStaleRanges = 128;
    void BeginAsyncReadback(AsyncReadback& readback);
    /// sotc: marks bytes a guest write fault asked for CPU-modified, unless pages of them
    /// still hold GPU data (see the definition).
    void MarkWrittenByGuest(VAddr addr, u64 size);
    /// sotc: the end of a guest fault's readback: a write is marked, a read unprotects pages that
    /// are GPU-modified with no GPU data behind them.
    void FinishGuestFault(VAddr addr, u64 size, bool is_write);
    void DropUnbackedGpuPages(VAddr addr, u64 size);
    void FinishAsyncReadback(AsyncReadback& readback);
    AsyncReadback* FindInflightReadback(VAddr addr, u64 size);
    /// sotc: bytes [addr, addr + size) changed (cause: DirtyCause). Readbacks in flight whose region
    /// overlaps get `dirty` (old path); every readback whose region or GPU page runs overlap
    /// records the interval in `stale` (READBACK-PLAN.md).
    void MarkInflightReadbacksDirty(VAddr addr, u64 size, u8 cause);
    /// The old whole-region test only (sets `dirty`, no interval).
    void MarkInflightRegionDirty(VAddr addr, u64 size);
    void RecordStale(AsyncReadback& readback, VAddr lo, VAddr hi, u8 cause);
    /// sotc: Finish of a readback whose copies were hit: applies what no interval touched.
    void FinishAsyncReadbackFine(AsyncReadback& readback, const Vulkan::StagingBufferRef& download);
    /// sotc: SOTC_RB_VERIFY=1 (diagnostic, blocks the draw thread), see the definition.
    struct FineVerify;
    void VerifyFineBefore(const AsyncReadback& readback, const Vulkan::StagingBufferRef& download,
                          std::span<const RbIntervals::Interval> merged, FineVerify& state);
    void VerifyFineAfter(const AsyncReadback& readback, std::span<const ReadbackItem> fine_items,
                         FineVerify& state);
    void NoteFinishedReadback(const AsyncReadback& readback); // sotc: SOTC_FPS telemetry
    std::vector<AsyncReadback*> inflight_readbacks;

    /// sotc: `uploaded` (optional) is set to whether any CPU-modified page was collected.
    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer, bool* uploaded = nullptr);

    /// sotc: SynchronizeMemory split in two. Collect marks the CPU-modified pages of the range as
    /// uploaded (and write-protects them, possibly batched, see ProtectBatchScope); Commit copies
    /// their bytes to the GPU and MUST run after PageManager::FlushProtectBatch(): a guest write
    /// landing after the protect faults and is captured, one before it is read by the copy.
    struct UploadJob {
        const Buffer* arena{};
        boost::container::small_vector<vk::BufferCopy, 4> copies;
        size_t total_size_bytes{};
        VAddr req_addr{};  ///< sotc: SOTC_STALL_LOG, the bound range that produced the uploads
        u32 req_size{};
        bool written{};
    };
    void CollectUploads(UploadJob& job, VAddr device_addr, u32 size, bool is_written);
    void CommitUploads(UploadJob& job);

    /// sotc: SOTC_ASYNC_UPLOAD. Guest bytes copied to staging memory by the copy threads, the
    /// submit (SubmitPendingArenaBinds) waits for them. See the definitions.
    struct GuestCopyPiece {
        VAddr source;
        u8* dest;
        u64 size;
    };
    struct GuestCopyFlush {
        Buffer* buffer;
        u64 offset, size;
    };
    bool AsyncUploadFor(u64 bytes) const;
    void StartGuestCopy(std::vector<GuestCopyPiece>&& ranges, u64 total,
                        const Vulkan::StagingBufferRef& staging);
    void WaitGuestCopies();
    std::atomic<u32> guest_copies_pending{0};
    std::mutex guest_copy_mutex;
    std::vector<GuestCopyFlush> guest_copy_flush; ///< staging ranges of non-coherent memory

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
    /// sotc: gpu_modified_ranges is changed on the GPU side (the draw thread, see
    /// Liverpool::IsBackThreaded) and read by the command processor's SRT walkers (ReadClean).
    std::shared_mutex gpu_modified_mutex;

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
        u8 cls{};    ///< sotc: bind class its blocks were marked with (ArenaBindClass)
    };
    // sotc: 64 slots before BbToggle::DispatchFastPath; the bit clear spreads over 1024 (an entry
    // is valid by its own contents, wherever it sits).
    std::array<ResidentCacheEntry, 1024> resident_cache{};

    u64 dma_runs_version = ~0ULL;
    size_t dma_runs_arenas{};
    u64 dma_runs_ranges{};
    /// sotc (SOTC_DMA_MERGE): touching resident ranges whose start lies in the same arena,
    /// merged into runs of (device address, bytes). Only rebuilt at the top of a sweep.
    std::vector<std::pair<VAddr, u64>> dma_runs;
    bool dma_full_pending{}; // sotc: the next DMA sweep visits every run (see SynchronizeDmaBuffers)
    std::vector<std::pair<VAddr, u64>> dma_spans;
    std::vector<UploadJob> dma_jobs; ///< sotc: uploads collected by one DMA sweep (reused)
    /// Readback windows requested recently; downloaded together with the next readback.
    struct HotReadback {
        VAddr addr;
        u64 size;
        std::chrono::steady_clock::time_point last_request;
    };
    static constexpr size_t MaxHotReadbacks = 64;
    /// sotc: the SOTC_RB_NARROW readback window in bytes, 0 for the 512 KiB one (ReadbackWindow).
    u64 rb_narrow_window{};
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

    /// sotc: BbToggle::WriteMarkMemo. What a written binding does after its upload check:
    /// AddGpuModified + RecordCurrentWrite, skipped when the same (address, size) found both
    /// already in force. Part A (AddGpuModified) holds while the range is still in
    /// gpu_modified_ranges (RangeSet::generation, or a look when something was removed) and no
    /// readback entered the list in flight (inflight_gen: marks on the others are idempotent).
    /// Part B (RecordCurrentWrite) holds within one command buffer while current_writes was not
    /// cleared and no hot read was added (the
    /// early-submit flag it may raise is raised again from `hot_overlap`).
    struct WriteMarkMemo {
        VAddr addr{};
        u64 size{};
        u64 gm_gen{}, inflight_gen{};
        u64 tick{}, cw_gen{}, hot_gen{};
        bool a_valid{}, b_valid{}, hot_overlap{};
    };
    static constexpr size_t NumWriteMarkMemos = 2048; // sotc: 256 before DispatchFastPath (low 256 slots)
    std::array<WriteMarkMemo, NumWriteMarkMemos> write_mark_memos{};
    u64 inflight_gen{};   ///< bumped when a readback enters inflight_readbacks
    u64 hot_reads_gen{};  ///< bumped when a hot read is added
    void MarkWrittenBinding(VAddr addr, u64 size);

    /// GPU writes recorded into the command buffer still being built (tick current_writes_tick).
    RangeSet current_writes;
    u64 current_writes_tick{};
    void RecordCurrentWrite(VAddr addr, u64 size);
    /// sotc: BbToggle::DispatchFastPath. The ranges RecordCurrentWrite appended since the last
    /// Clear and did not insert into current_writes yet (its only queries are Intersects, answered
    /// from both). Draw thread only, like current_writes.
    std::vector<std::pair<VAddr, VAddr>> pending_writes;
    static constexpr size_t MaxPendingWrites = 1024;
    void ClearCurrentWrites();
    void FlushPendingWrites();
    [[nodiscard]] bool CurrentWritesIntersects(VAddr addr, u64 size);
    /// sotc: whether marking [addr, addr + size) written would change a readback that entered
    /// inflight_readbacks after `seq` (its region or one of its GPU page runs overlaps).
    [[nodiscard]] bool InflightHitSince(u64 seq, VAddr addr, u64 size) const;
    /// sotc: the range the guest touched, during the DownloadMemory of its window (size 0 else).
    VAddr fault_addr{};
    u64 fault_size{};

public:
    /// sotc: true once after the command buffer being built wrote a page the guest reads back:
    /// submitting it now lets the GPU finish it before the guest asks for it.
    bool TakeHotWrite() noexcept;

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
    /// sotc: unbinds resident block ranges (zeroing their page table entries) and frees the
    /// allocations left without blocks. The GPU must no longer use them.
    void UnbindResident(std::span<const Backing> released);
    /// sotc: per arena block, the use epoch (seconds, wrapping, 0 = never) of its last bind.
    u16* block_last_use{};
    /// sotc (BbToggle::ArenaImageRelease): per arena block, the classes of binds that have made or
    /// kept it resident since it became resident: 1 = image source (ObtainBufferForImage),
    /// 2 = any other bind. 0 = unclassified (made resident another way). Cleared when the block
    /// is unbound. Only a block of class exactly 1 may be given back before the arena threshold.
    u8* block_class{};
    /// sotc: class the EnsureResident calls of this thread mark (2 unless ObtainBufferForImage).
    static inline thread_local u8 arena_bind_class = 2;
    /// sotc: arena memory by class of its blocks, for the SOTC MEM line (every 5 s).
    void ArenaClassCensus(u16 idle_epochs);
    /// sotc: blocks MaybeEvictCold gave back and nothing bound again yet.
    IntervalList<> evicted_ranges;
    /// sotc: evicted blocks a shader faulted back in; never evicted again.
    IntervalList<> pinned_ranges;
    /// sotc: resident ranges (by first block) whose read-back did not match guest memory,
    /// tried again after this.
    std::unordered_map<u64, std::chrono::steady_clock::time_point> evict_retry;
    /// sotc: arena memory in use (blocks handed out of the chunks), in bytes.
    u64 ArenaUsedBytes() const noexcept;
    u16 use_epoch{1};
    /// sotc: sets use_epoch from the clock (once per submit and per eviction check).
    void AdvanceUseEpoch(std::chrono::steady_clock::time_point now) noexcept {
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(now - epoch_start).count();
        use_epoch = static_cast<u16>(seconds % 65535 + 1); // 0 = never bound
    }
    std::chrono::steady_clock::time_point epoch_start{};
    std::chrono::steady_clock::time_point next_evict_check{};
    /// sotc: an eviction whose read-back is recorded; compared and unbound once the GPU is done.
    struct EvictPiece {
        VAddr addr;
        u64 size;
        u64 offset; ///< in the download
    };
    struct EvictChoice {
        u64 key; ///< first block of the resident range
        u64 bytes;
        std::vector<EvictPiece> pieces;
        boost::container::small_vector<Backing, 4> ranges;
        bool img_only{}; ///< sotc: every block bound only as an image source (ArenaImageRelease)
        u16 need_idle{}; ///< idle epochs this choice needed
    };
    struct PendingEviction {
        std::vector<EvictChoice> chosen;
        Vulkan::StagingBufferRef download;
        u64 tick{};
        std::chrono::steady_clock::time_point deadline{};
        bool over{}; ///< sotc: chosen because resident arena memory passed the threshold
    };
    std::optional<PendingEviction> pending_eviction;
    bool FinishEviction(std::chrono::steady_clock::time_point now, bool wait);

    u32 arena_memory_type_index{};
    std::optional<u32> arena_fallback_memory_type_index;
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
};

} // namespace VideoCore
