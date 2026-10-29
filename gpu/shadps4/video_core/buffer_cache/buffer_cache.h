// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <thread>

#include <chrono>
#include <shared_mutex>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>
#include <unordered_map>
#include <boost/container/small_vector.hpp>

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/host_import.h"
#include "video_core/buffer_cache/hrr.h"
#include "video_core/buffer_cache/async_upload.h"

extern "C" {
// runtime_memory.c (sotc: SOTC_ASYNC_UPLOAD resolves guest ranges to the backing view)
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
const unsigned char* runtime_memory_backing_view(uint64_t* size);
}
#include "video_core/buffer_cache/range_set.h"
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

    /// sotc: SOTC_BIND_CACHE_XTICK. Like BindGeneration, but bumped only when a binding that
    /// existed may have become invalid: resident ranges unbound (release, eviction) or an arena
    /// replaced. Making more of the address space resident (EnsureResident) moves no handle or
    /// offset an earlier binding holds, so it leaves this alone.
    u64 BindVersion() const noexcept {
        return bind_version;
    }

    /// sotc: SOTC_BIND_CACHE_XTICK. The use epoch EnsureResident stamps blocks with.
    u16 UseEpoch() const noexcept {
        return use_epoch;
    }

    /// sotc: SOTC_BIND_CACHE_XTICK. What EnsureResident does for a range that is already resident:
    /// stamps its blocks with the use epoch, so MaybeEvictCold sees a bind that a cached bind
    /// repeats without going through ObtainBuffer.
    void NoteBindUse(VAddr device_addr, u64 size) noexcept;

    /// sotc: SOTC_BIND_CACHE_XTICK. The synchronization ObtainBuffer does for a read-only
    /// binding (uploads the CPU-modified pages of the range into `arena`).
    void SynchronizeBinding(const Buffer* arena, VAddr device_addr, u32 size, bool is_texel_buffer);

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

    /// Finds a buffer for the specified region.
    /// sotc: SOTC_WRITE_EXTENT: for is_written, [mark_offset, mark_offset + mark_size) of the range
    /// is what the GPU may write (marked GPU-modified, page-protected for reads); the rest is
    /// bound and synchronized like a read-only range. mark_size == ~0u: the whole range.
    /// sotc: a bind's GPU writes are recorded (RecordCurrentWrite) when its buffers are obtained,
    /// but its command is recorded after the whole bind. A submit in between (arena allocation,
    /// eviction, staging flush) moves the command into the next tick: EndWriteScope then records
    /// the scope's writes again in the tick the command lands in, so readback eligibility
    /// (current_writes, the writer-tick ring) never trusts a tick that does not hold the write.
    void BeginWriteScope();
    void EndWriteScope();

    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                             bool is_written,
                                                             bool is_texel_buffer = false,
                                                             u32 mark_offset = 0,
                                                             u32 mark_size = ~0u);

    /// Attempts to obtain a buffer without modifying the cache contents.
    /// sotc: pages_protected: the image's tracking write-protects all its pages (its guest copy
    /// may then run on the copy threads, SOTC_ASYNC_TEXTURE_UPLOAD).
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size,
                                                                     bool pages_protected = false);
    /// sotc: guest memory in a staging buffer, whatever the GPU copy holds (F9 diagnostic).
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainGuestCopyForImage(VAddr device_addr,
                                                                       u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// sotc: a guest write fault at addr: unprotect the following pages too (MemoryTracker::
    /// MarkAheadAsCpuModified). SOTC_WRITE_AHEAD_PAGES (default 15, 0 = off).
    void MarkWriteAhead(VAddr addr);
    /// sotc: SOTC_WRITE_AHEAD_PAGES (the fixed write-ahead of MarkWriteAhead), in pages.
    static u64 WriteAheadPages();
    /// sotc: SOTC_WRITE_AHEAD_ADAPT. Marks CPU-modified the pages of [begin, end) (4 KiB aligned)
    /// that are neither CPU- nor GPU-modified and that allow(page) accepts, after taking a
    /// snapshot of their bytes (NoteGranulePage's mechanism, tagged as write-ahead), so those the
    /// guest does not write are not uploaded. Returns the pages marked.
    u64 MarkWriteAheadRange(VAddr begin, VAddr end, bool (*allow)(VAddr page));

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
    /// sotc: SOTC_RB_EXACT: bytes still owed to guest memory count as GPU-written too.
    [[nodiscard]] bool IsRangeGpuWritten(VAddr addr, size_t size) const {
        return gpu_modified_ranges.Intersects(addr, size) || RbxOwedIntersects(addr, size);
    }

    // sotc: SOTC_RB_EXACT=1 API (see the big comment above BufferCache::TryServeExact). All are
    // no-ops returning neutral values when the switch is off. Draw thread unless noted.
    /// SOTC_RB_EXACT=1 or =publish (and SOTC_RB_OWNER=1 and asynchronous readbacks).
    static bool RbExactEnabled() noexcept;
    /// sotc: SOTC_RB_EXACT=publish only: the lean mode. No writer map, no owed releases (TryServeExact,
    /// RbxNoteWrite, RbxPrune and SettleOwedUpTo do nothing); the label downloads stay.
    static bool RbExactPublish() noexcept;
    /// Liverpool constructor: true iff its label side is active. Every SOTC_RB_EXACT behaviour
    /// requires the switch and this.
    static void SetLabelSideActive(bool on) noexcept;
    /// Liverpool BackLoop start: the draw thread identity.
    static void SetDrawThread(std::thread::id id) noexcept;
    /// Called on a guest thread for every guest readback that waited for the GPU on hot data:
    /// (newest hot seq among the writers waited for, the guest's wall-clock wait in ns). Needs
    /// SOTC_RB_EXACT active (the seq comes from its writer map). One atomic load when unset.
    using WaitObserver = void (*)(u64 writer_seq, u64 wait_ns);
    static void SetWaitObserver(WaitObserver fn) noexcept;
    /// Count of GPU writes (AddGpuModified) that hit a hot window (a recurring readback granule).
    /// Any thread. Calling it also tells the cache that a label/CPU-visible write may carry this
    /// snapshot: ranges written at or before it are never released as "owed" afterwards. Call it
    /// when processing a label or CPU-visible write packet, not on other packets.
    u64 HotWriteSeq() const noexcept;
    /// A CPU-visible GPU write carrying this HotWriteSeq snapshot reached guest memory. Any thread
    /// (atomics only; the diagnostics run only on the draw thread).
    void NoteLabelWritten(u64 hot_seq) noexcept;
    /// Applies every complete, clean owed download and label download to guest memory (hot items)
    /// before a deferred label is written. Must run on the draw thread (scheduler.DeferOperation).
    /// True: everything the label announces is applied, write it now. False (wrong thread, or
    /// re-entered from inside a buffer-cache operation): requeue the op and retry later.
    bool ApplyOwedBeforeLabel();
    /// Draw thread. Before an immediate (non-deferred) announcement carrying this HotWriteSeq
    /// snapshot: applies the owed downloads holding ranges at or below it and the pending label
    /// downloads, flushing and waiting for their copies when needed.
    void SettleOwedUpTo(u64 seq);
    /// Called when a label is deferred: records, in the tick being recorded, the downloads of every
    /// GPU-modified range in hot windows written since the previous call (GPU-modified state
    /// kept). True when something was recorded.
    bool RecordLabelDownloads();

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

    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block);

    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);

    /// sotc: the range [first, second) of the arena that a readback of [device_addr, +size)
    /// downloads and unmarks as GPU-modified: 512 KiB around it, smaller with SOTC_RB_NARROW.
    /// sotc: stream: a larger window for a guest thread reading GPU-written memory in sequence
    /// (SOTC_RB_STREAM, ReadMemory), 0 for the usual one.
    std::pair<VAddr, VAddr> ReadbackWindow(const Buffer* arena, VAddr device_addr, u64 size,
                                           u64 stream = 0);

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
        /// sotc: SOTC_RB_EXACT: an owed download item: byte-precise ranges that already left the
        /// GPU-modified set; applying it writes guest bytes only (no tracker or set changes).
        bool owed = false;
        /// sotc: SOTC_HRR=1: every copy's bytes are read from the host-visible arena chunk they sit
        /// in, not from a staging copy (no vkCmdCopyBuffer; see BeginAsyncReadbackImpl).
        bool direct = false;
    };
    /// Collects what a readback of [device_addr, +size) and the hot readbacks must copy. Returns
    /// the bytes to download. sotc: the ranges stay in the GPU-modified set until the caller
    /// applied the data and called SubtractDownloaded (the command processor reads clean ranges
    /// from the backing meanwhile, with the draw thread); subtract: ranges with nothing bound
    /// to copy leave the set right away.
    u64 GatherDownloads(const Buffer* arena, VAddr device_addr, u64 size,
                        boost::container::small_vector<ReadbackItem, 16>& pending,
                        bool subtract = true);
    /// sotc: takes the downloaded ranges of items out of the GPU-modified set.
    void SubtractDownloaded(std::span<const ReadbackItem> items);
    /// Writes downloaded data (mapped, at the copies' destination offsets minus mapped_offset)
    /// into guest memory and unmarks the ranges as GPU-modified.
    /// sotc: keep_gpu (SOTC_RB_KEEP_LATER): ranges [start, end) whose pages stay GPU-modified
    /// (a GPU write recorded after the readback's copies covers them).
    void ApplyDownloads(std::span<const ReadbackItem> pending, const u8* mapped,
                        u64 mapped_offset,
                        std::span<const std::pair<VAddr, VAddr>> keep_gpu = {});

    /// sotc (default, SOTC_RB_ASYNC=0 turns it off): a readback asked by a guest thread in two steps on the command
    /// processor: the copies are recorded and submitted without waiting (the command processor
    /// goes on with its queue), the guest thread waits for the GPU, then the data is applied
    /// unless a later GPU write hit the ranges meanwhile (then it is read back again).
    struct AsyncReadback {
        VAddr device_addr{};
        u64 size{};
        u64 stream{}; ///< sotc: ReadbackWindow's stream size (SOTC_RB_STREAM), 0 none
        bool is_write{};
        bool waiting{};
        bool dirty{};
        bool dependent{};
        bool finished{};
        /// sotc: SOTC_RB_KEEP_LATER: parts [start, end) of the pending ranges a GPU write recorded
        /// after the copies covers. The read data is still applied (it is what the memory held
        /// when the guest asked), and these parts stay GPU-modified for the next read.
        boost::container::small_vector<std::pair<VAddr, VAddr>, 4> later_gpu;
        /// sotc: SOTC_RB_OWNER: part of the asked range belongs to readbacks in flight when it
        /// started (it did not copy those pages): they are finished with it.
        bool has_owners{};
        /// sotc: SOTC_HRR=1: no copy was recorded for it; the bytes are read from the host-visible
        /// arena chunks at FinishAsyncReadback (every item is direct, see BeginAsyncReadbackImpl).
        bool direct{};
        /// sotc: SOTC_RB_ASYNC_AHEAD: the ahead timeline value of its own copies when they went
        /// in an ahead command buffer (Scheduler::SubmitAheadAsync; tick is then 0 or the newest
        /// owner tick), 0 otherwise. wait_ahead: the ahead value the guest waits for (its own or
        /// its owners').
        u64 ahead_value{};
        u64 wait_ahead{};
        /// sotc: SOTC_RB_TICK: the transfer timeline value of its own copies when they went on the
        /// transfer queue (Scheduler::XferReadbackCopy), 0 otherwise. wait_xfer: the value the guest
        /// waits for (its own or its owners').
        u64 xfer_value{};
        u64 wait_xfer{};
        u64 tick{};
        const Buffer* arena{};
        VAddr window_start{};
        u64 window_size{};
        u64 download_offset{};
        const u8* download_mapped{};
        Buffer* download_buffer{};
        u64 download_size{};
        /// sotc: SOTC_RB_GROW: the window size the guest thread chose (0: the usual
        /// ReadbackWindow) and its direction (see GrowReadbackWindow).
        u64 grow_window{};
        bool grow_backward{};
        /// sotc: SOTC_FPS: bytes GatherDownloads collected for it (0 when it copied nothing).
        u64 gathered_bytes{};
        /// sotc: SOTC_RB_PREFETCH: started by the cache (PrefetchIssue), not by a guest thread.
        /// It is applied only when a guest read fault reaches it (see PrefetchAtFault); any write
        /// recorded after its copies makes it dirty, and a dirty one is freed, never applied.
        bool prefetch{};
        /// sotc: SOTC_RB_PREFETCH: a guest readback waits for its tick (an owner of the asked
        /// range): it does not expire.
        bool pf_claimed{};
        /// sotc: SOTC_RB_PREFETCH: hull [pf_lo, pf_hi) of its gpu_ranges (cheap dirty tests) and
        /// the staging bytes it holds (the pool's size class).
        VAddr pf_lo{};
        VAddr pf_hi{};
        u64 pf_bytes{};
        /// sotc: SOTC_RB_MATERIALIZE: every range it copied was written by the GPU in this frame or
        /// the previous one (pf_fresh), so PrefetchMaterialize may apply it without a guest fault.
        bool pf_fresh_ok{};
        /// sotc: SOTC_RB_PREV: the download of what TryServePrevious released without a readback
        /// (the GPU's newer bytes, still owed to guest memory). Applied by PrefetchMaterialize when
        /// done and clean; never expires by age.
        bool pf_owed{};
        /// sotc: SOTC_RB_EXACT: owed download (kept in rbx_live, not in inflight_readbacks), its
        /// newest hot sequence number and the staging bytes it holds.
        bool pf_rbx{};
        u64 rbx_seq{};
        u64 rbx_seq_min{~u64{0}};
        /// sotc: SetWaitObserver: newest hot seq among the writers the guest waited for.
        u64 wait_seq{};
        u64 rbx_bytes{};
        /// sotc: SOTC_RB_EXACT: label download (in pf_live and inflight_readbacks like a
        /// prefetch, never expires before its tick completes).
        bool pf_label{};
        /// sotc: SOTC_RB_GROUP: group download (in pf_live and inflight_readbacks like a
        /// prefetch; a later write trims its pages instead of making it dirty) and the staging
        /// bytes it holds. group_tick: on a guest readback, the tick of the group download its
        /// fault recorded (0: none).
        bool pf_group{};
        u64 group_bytes{};
        u64 group_tick{};
        boost::container::small_vector<ReadbackItem, 16> pending;
    };
    /// sotc: SOTC_RB_GROW: the window [first, second) of readback.grow_window bytes, aligned to
    /// the base (SOTC_RB_NARROW) window, from the request's granule forward (or backward, ending
    /// with it), clamped to the arena and always covering the request.
    std::pair<VAddr, VAddr> GrowReadbackWindow(const Buffer* arena, const AsyncReadback& readback);
    /// sotc: SOTC_RB_GROW / SOTC_FPS: on the guest thread, before the readback is sent: classifies
    /// the request against the thread's previous one (pattern counters) and picks its window size.
    void PrepareGuestReadback(AsyncReadback& readback, u64 now_ns);
    /// sotc: ... and after it returned: remembers its window as the thread's previous one.
    void NoteGuestReadbackDone(const AsyncReadback& readback);
    void BeginAsyncReadback(AsyncReadback& readback);
    void BeginAsyncReadbackImpl(AsyncReadback& readback);
    /// sotc: newest hot seq among the pending writers a readback waits for (0: none).
    u64 RbxWaitSeq(const AsyncReadback& readback);
    /// sotc: marks bytes a guest write fault asked for CPU-modified, unless pages of them
    /// still hold GPU data (see the definition).
    void MarkWrittenByGuest(VAddr addr, u64 size);
    void FinishAsyncReadback(AsyncReadback& readback);
    /// sotc: true once the GPU copied an in-flight readback's data (its ahead copy, or its tick).
    bool IsReadbackComplete(const AsyncReadback& readback);
    AsyncReadback* FindInflightReadback(VAddr addr, u64 size);
    /// sotc: why in-flight readbacks of [addr, addr + size) are no longer exact.
    enum class StaleReason : u32 { Unmap, Applied, GpuWrite, CpuWrite, Count };
    void MarkInflightReadbacksDirty(VAddr addr, u64 size, StaleReason reason,
                                    bool keep_owed = false);
    /// sotc: SOTC_RB_OWNER: pages [start, end) that readbacks in flight are downloading (sorted,
    /// merged); GatherDownloads leaves them out while set.
    const std::vector<std::pair<VAddr, VAddr>>* rb_owned = nullptr;
    /// sotc: SOTC_RB_OWNER: finishes the readbacks in flight over [addr, addr + size) whose GPU
    /// work is done (the asking guest thread waited for their ticks).
    void FinishReadyOwners(VAddr addr, u64 size);
    std::vector<AsyncReadback*> inflight_readbacks;
    /// sotc: SOTC_HRR=1: direct readbacks in inflight_readbacks (counted when they start, released
    /// in FinishAsyncReadback). RecordCurrentWrite tests it before marking them stale.
    std::atomic<u32> direct_inflight{};

    /// sotc: SOTC_RB_PREFETCH=1 (off by default; needs SOTC_RB_OWNER=1). Draw thread only.
    /// Base granules (the SOTC_RB_NARROW window, 512 KiB without it) the guest read back, with a
    /// bit per game frame (bit 0: frame `frame`, bit i: frame - i).
    struct PfHistory {
        u64 frame{};
        u32 mask{};
    };
    std::unordered_map<VAddr, PfHistory> pf_history;
    /// Granules read back in at least 2 of the 3 previous frames, sorted by key; rebuilt once per
    /// frame (PrefetchRebuild). armed: a GPU write hit it since its last prefetch.
    struct PfWindow {
        VAddr key{};
        u64 issued_tick{};
        bool armed{};
    };
    std::vector<PfWindow> pf_recurring;
    std::vector<u32> pf_armed; ///< indices into pf_recurring
    VAddr pf_rec_lo{};
    VAddr pf_rec_hi{};
    u64 pf_frame = ~u64{0};
    /// Prefetches not yet freed (in inflight_readbacks until applied or dropped), the staging
    /// they hold, the hull of their gpu_ranges, and the tick of the last expiry pass.
    std::vector<std::unique_ptr<AsyncReadback>> pf_live;
    u64 pf_bytes_held{};
    VAddr pf_lo{};
    VAddr pf_hi{};
    u64 pf_reap_tick{};
    /// sotc: SOTC_RB_MATERIALIZE: per recurring window (key), the ranges [lo, hi) the GPU wrote
    /// into it and the frame of the last write (PrefetchNoteWrite); entries older than the
    /// previous frame do not count and are pruned in PrefetchRebuild.
    struct PfFresh {
        VAddr lo{};
        VAddr hi{};
        u64 frame{};
        u64 ns{}; ///< steady-clock time of its first write (frames stop during loads and pauses)
    };
    std::unordered_map<VAddr, boost::container::small_vector<PfFresh, 4>> pf_fresh;
    /// sotc: SOTC_RB_PREV: per recurring window, the frames the GPU wrote into it (bit 0: frame,
    /// bit n: frame - n), from PrefetchNoteWrite.
    struct PfWriteMask {
        u64 frame{};
        u32 mask{};
    };
    std::unordered_map<VAddr, PfWriteMask> pf_wmask;
    /// sotc: SOTC_RB_PREV: per window key, the byte ranges ApplyDownloads wrote into guest
    /// memory, with the frame and time of the write (same merge rules as pf_fresh). Erased when
    /// the memory is unmapped or released; any thread may erase, hence the mutex.
    std::mutex prev_mutex;
    std::unordered_map<VAddr, boost::container::small_vector<PfFresh, 4>> pf_applied;
    void PrevNoteApplied(VAddr addr, u64 size);
    void PrevErase(VAddr addr, u64 size);
    /// sotc: SOTC_RB_PREV: serves a guest read fault from the bytes already in guest memory (the
    /// last applied version, at most SOTC_RB_PREV_FRAMES frames old) instead of waiting for the
    /// GPU. True when it did (the readback then has nothing to wait for).
    bool TryServePrevious(AsyncReadback& readback);
    /// sotc: SOTC_RB_MATERIALIZE: steady-clock ns of the last PrefetchMaterialize pass.
    u64 pf_mat_last_ns{};
    /// sotc: SOTC_RB_MATERIALIZE: true when every gpu_range of p lies inside one fresh range
    /// (this frame or the previous one) of window key.
    bool PrefetchFreshCovers(VAddr key, const AsyncReadback& p) const;
    /// sotc: SOTC_RB_MATERIALIZE: applies the finished, clean, fresh prefetches without waiting
    /// for a guest fault (see the comment above it in buffer_cache.cpp).
    void PrefetchMaterialize();
    /// GatherDownloads leaves the hot readbacks (SOTC_RB_HOT) out while set.
    bool rb_gather_no_hot{};
    u64 PfGranule() const noexcept {
        return rb_narrow_window ? rb_narrow_window : u64{512} * 1024;
    }
    void PrefetchNoteRead(VAddr addr);
    void PrefetchNoteWrite(VAddr addr, u64 size);
    void PrefetchRebuild(u64 frame);
    /// Marks the prefetches whose gpu_ranges intersect [addr, addr + size) dirty.
    void PrefetchMarkDirty(VAddr addr, u64 size);
    /// A guest readback of [addr, addr + size) begins: applies the finished prefetches of its
    /// granules (no GPU wait), drops dirty unclaimed ones, and makes sure the tick of a pending one
    /// is sent.
    void PrefetchAtFault(VAddr addr, u64 size);
    /// Frees pf_live[index] (not applied: its pages stay GPU-modified and protected).
    void PrefetchDrop(size_t index);
    void PrefetchUpdateHull();

    // sotc: SOTC_RB_EXACT state. Draw thread only unless noted.
    /// Newest writer of every GPU-written byte whose writer may still be pending: [lo, hi) ->
    /// (hi, tick, hot sequence number; 0 when not in a hot window). Entries whose tick is done
    /// are pruned (rbx_pruned_tick: newest pruned tick); rbx_lost_tick: newest tick of entries
    /// dropped without being done (overflow): an absent byte is "done" only if it is done.
    struct RbxWriter {
        VAddr hi;
        u64 tick;
        u64 seq;
        /// seq of the OLDEST GPU version of these bytes not yet delivered to guest memory (still
        /// GPU-modified, owed, or under a label download) when this write landed; 0: unknown.
        /// A pending range is released as owed only if the floor is above every announced seq.
        u64 floor;
    };
    std::map<VAddr, RbxWriter> rbx_writers;
    u64 rbx_lost_tick{};
    u64 rbx_pruned_tick{};
    u64 rbx_notes{};
    std::atomic<u64> rbx_hot_seq{0};
    std::atomic<u64> rbx_announced{0};
    mutable std::atomic<u64> rbx_seq_queried{0};
    int rbx_depth{}; ///< inside a buffer-cache operation that a label op must not re-enter
    struct RbxDepth {
        int& d;
        explicit RbxDepth(int& depth) : d(depth) { ++d; }
        ~RbxDepth() { --d; }
        RbxDepth(const RbxDepth&) = delete;
        RbxDepth& operator=(const RbxDepth&) = delete;
    };
    u64 inflight_version{}, rbx_owned_version = ~u64{0};
    std::vector<std::pair<VAddr, VAddr>> rbx_owned_cache;
    const std::vector<std::pair<VAddr, VAddr>>& RbxOwnedAll();
    void RbxSettleWritePages(VAddr addr, u64 size, bool assume_locks);
    /// Owed downloads (pf_rbx) and the union of their byte ranges (any thread reads it).
    std::vector<std::unique_ptr<AsyncReadback>> rbx_live;
    u64 rbx_owed_bytes{};
    mutable std::mutex rbx_owed_mutex;
    RangeSet rbx_owed_ranges;
    std::atomic<u32> rbx_owed_count{0};
    u64 rbx_apply_ns{};
    bool rbx_in_apply{};
    /// Hot-window GPU writes since the last RecordLabelDownloads, and the label downloads' bytes.
    RangeSet rbx_since_label;
    u64 rbx_label_bytes{};
    std::vector<u64> rbx_scope_seq; ///< parallel to write_scope
    enum class ExactResult { Fallback, Served, Download };
    ExactResult TryServeExact(AsyncReadback& readback, const Buffer* arena, VAddr ws, VAddr we,
                              u64& total_out, u64& writer_out);
    u64 RbxHotSeq(VAddr addr, u64 size);
    void RbxNoteWrite(VAddr addr, u64 size, u64 tick, u64 seq, bool gpu_write);
    bool ReadCleanBacking(VAddr addr, void* out, u32 size);
    void RbxPrune();
    void RbxTrim(VAddr lo, VAddr hi);
    bool RbxOwedIntersects(VAddr addr, u64 size) const;
    void RbxUnregister(AsyncReadback& o);
    void RbxDrop(size_t index);
    void RbxApplyOwed(bool throttled);
    void RbxFinishOverlapping(VAddr addr, u64 size, bool hot);
    u64 RbxOwedTick(VAddr addr, u64 size, bool& unsent);
    void RbxSettleSync(VAddr addr, u64 size);
    void AddResidentCopies(ReadbackItem& item, VAddr start, VAddr end, u64& total);
    void FinishOwners(VAddr addr, u64 size, bool hot);

    // sotc: SOTC_RB_GROUP state (see the comment above BufferCache::RbgAtFault). Draw thread.
    /// The GPU-modified ranges one bind (BeginWriteScope .. EndWriteScope: one draw or dispatch)
    /// marked, in the order marked. id: 1, 2, ... in bind order; key: identity of the bind across
    /// frames (program hash, range count and sizes), to learn which binds the guest reads back
    /// in several pieces; hits: guest read faults that landed on its bytes.
    struct RbgGroup {
        u64 id{};
        u64 key{};
        u64 frame{};
        u64 ns{};
        u64 bytes{};
        u32 hits{};
        bool delivered{}; ///< a group download was recorded (or refused for good) for it
        bool oversize{};  ///< more ranges than RbgRangesMax: never delivered
        std::vector<std::pair<VAddr, u64>> ranges;
    };
    /// Newest group of every byte a bind marked GPU-modified: [lo, hi) -> group id.
    struct RbgRef {
        VAddr hi;
        u64 id;
    };
    std::deque<RbgGroup> rbg_groups; ///< closed groups, oldest first (ids ascending)
    std::map<VAddr, RbgRef> rbg_index;
    RbgGroup rbg_open;  ///< the group of the bind in progress
    u64 rbg_next_id{1};
    u64 rbg_closed{};
    std::unordered_map<u64, u8> rbg_hot_keys; ///< keys that were read back in several faults
    u32 rbg_live{};                           ///< group downloads in pf_live
    u64 rbg_bytes_held{};
    u64 rbg_apply_ns{};
    void RbgNoteWrite(VAddr addr, u64 size);
    void RbgCloseScope();
    void RbgIndexInsert(VAddr lo, VAddr hi, u64 id);
    void RbgPruneIndex();
    /// A guest read fault on [readback.device_addr, +size) whose readback window is [ws, we):
    /// records the group download of the bind that wrote it (see the comment). Returns its tick.
    u64 RbgAtFault(AsyncReadback& readback, VAddr ws, VAddr we);
    /// Takes the pages of [lo, hi) out of a group download (nothing left: dirty).
    void RbgTrim(AsyncReadback& g, VAddr lo, VAddr hi);
    /// Applies the complete, clean group downloads (throttled: at most every
    /// SOTC_RB_MATERIALIZE_US) and frees the finished or dirty ones.
    void RbgApplyReady(bool throttled);

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);
    /// sotc: the full synchronization; SynchronizeMemory adds the SOTC_SYNC_FAST skip around it.
    bool SynchronizeMemoryFull(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                               bool is_texel_buffer);

    bool SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size);

    /// sotc: drops the hashed uploads overlapping [addr, addr + size). Any write into the arena
    /// (RecordCurrentWrite) and any unbind (UnbindResident) make the stored hash stale.
    void EraseHashedUploads(VAddr addr, u64 size);
    struct HashedUpload {
        u64 size;
        u64 hash;
    };
    std::mutex hashed_uploads_mutex; // unbinds run on guest threads (ReleaseMemory)
    std::multimap<VAddr, HashedUpload> hashed_uploads;
    std::atomic<size_t> hashed_upload_count{0};
    /// sotc: SOTC_HASH_EPOCH=1 (off by default): hull [lo, hi) of the ranges in hashed_uploads.
    /// Written only under hashed_uploads_mutex (insert, clear, reset when the map empties); read
    /// relaxed without it by EraseHashedUploads, which skips the lock for a range outside it.
    std::atomic<VAddr> hashed_lo{~VAddr{0}};
    std::atomic<VAddr> hashed_hi{0};

public:
    /// sotc: SOTC_FAULT_GRANULE. Called from the guest write fault handler for each page of the
    /// faulting granule it marks CPU-modified: remembers a hash of the page's current bytes.
    void NoteGranulePage(VAddr page);
    /// sotc: forgets the granule snapshots in [addr, addr + size) (unbind, unmap): the arena no
    /// longer holds what the guest held then.
    void EraseGranuleSnapshots(VAddr addr, u64 size);

private:
    /// sotc: drops from `copies` (address-based, before staging offsets) the 4 KiB pages that a
    /// granule marked dirty but whose bytes still equal the hash taken then: the guest did not
    /// write them, so the arena keeps what it holds (it can differ from guest memory on pages the
    /// tracker calls clean, and uploading guest bytes over it broke scenes).
    void DropUnchangedGranulePages(boost::container::small_vector<vk::BufferCopy, 4>& copies);
    std::mutex granule_mutex;
    std::unordered_map<VAddr, u64> granule_snapshots;
    std::atomic<size_t> granule_snapshot_count{0};

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
    std::unique_ptr<HostImport> host_import; ///< sotc: SOTC_HOST_IMPORT, null when off
    std::unique_ptr<HostResidentRanges> hrr; ///< sotc: SOTC_HRR dry run, null when off
    /// sotc: SOTC_ASYNC_UPLOAD: queues the CPU copy of an upload for the copy threads and
    /// records its staging-to-arena copy. False (nothing done) when it must stay here.
    /// sotc: SOTC_ASYNC_UPLOAD: queues the copies (srcOffset: offset in staging, dstOffset: guest
    /// address) for the copy threads. False, nothing queued, when a range has no backing view.
    bool QueueAsyncGuestCopy(AsyncUpload& async_upload, std::span<const vk::BufferCopy> copies,
                             const Vulkan::StagingBufferRef& staging, u64 total_size);
    bool TryUploadAsync(AsyncUpload& async_upload, const Buffer* arena,
                        boost::container::small_vector<vk::BufferCopy, 4>& copies,
                        u64& total_size_bytes);
    std::unique_ptr<Buffer> bda_pagetable_buffer;
    bool fault_process_pending{};

    std::array<const Buffer*, NUM_ARENA_PAGES> address_space{};
    std::deque<Buffer> arenas;
    std::vector<ArenaBinds> pending_binds;
    Vulkan::Semaphore memory_semaphore;
    /// sotc: SOTC_UNBIND_GPU_WAIT: work timeline tick the next sparse bind waits for (0 = none).
    u64 pending_bind_wait_tick{};
    /// sotc: SOTC_UNBIND_GPU_WAIT: the next bind carries an unbind (set by ReleaseMemory).
    bool unbind_bind_next{};
    /// sotc: SOTC_UNBIND_GPU_WAIT: memory_semaphore tick of the last bind that carried an unbind.
    /// Every later bind waits for it, so blocks an unbind freed are not bound again before the
    /// unbind has run on the GPU (the CPU side of an unbind frees them at once).
    u64 unbind_bind_tick{};

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
    /// sotc: BindVersion(); bumped by UnbindResident and the arena migration in GetArena.
    u64 bind_version{};
    /// sotc: SOTC_RB_TICK: tick being recorded when GetArena last replaced an arena (its sparse binds
    /// are submitted after the writers of earlier ticks; a transfer-queue readback copy through the
    /// new arena would not wait for them).
    u64 arena_migrate_tick{};
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
    bool dma_full_pending{}; // sotc: the next DMA sweep visits every run (see SynchronizeDmaBuffers)
    u64 untrack_applied{0};  // sotc: SOTC_UNTRACK_AUTO, the published list version applied last
    std::vector<std::pair<VAddr, u64>> dma_spans;
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
    u64 AddGpuModified(VAddr addr, u64 size); ///< sotc: returns the hot sequence number (0: none)

    /// GPU writes recorded into the command buffer still being built (tick current_writes_tick).
    RangeSet current_writes;
    u64 current_writes_tick{};
    void RecordCurrentWrite(VAddr addr, u64 size, u64 seq = 0, bool gpu_write = false);
    bool write_scope_on{};       ///< sotc: see BeginWriteScope
    u64 write_scope_tick{};
    std::vector<std::pair<VAddr, u64>> write_scope;
    /// sotc: SOTC_RB_TICK / SOTC_FPS: when the tick being recorded changed, files current_writes
    /// into write_ring (the write sets of the last NumWriteRing ticks that wrote anything) and
    /// starts an empty set. Without both switches it only clears, as before.
    void RollCurrentWrites();
    struct WriteRingEntry {
        u64 tick{};
        RangeSet set;
    };
    static constexpr u32 NumWriteRing = 8;
    std::array<WriteRingEntry, NumWriteRing> write_ring; // default-initialized (RangeSet ctor)
    u32 write_ring_head{};
    /// Newest tick evicted from the ring: writes at ticks <= this are no longer tracked.
    u64 write_ring_floor{};
    /// sotc: tick of the newest write to any byte the copies of pending cover: CurrentTick() for
    /// the tick being recorded, a ring tick, else the ring floor (older), else unknown (true) and
    /// CurrentTick() - 1.
    u64 ReadbackWriterTick(std::span<const ReadbackItem> pending, bool& unknown);
    /// sotc: SOTC_RB_TICK: ranges a readback copy on the transfer queue reads until it completes
    /// (value of the transfer timeline). A write recorded over one makes the next graphics submit
    /// wait for that value (write after read); see NoteXferRead.
    struct XferRead {
        VAddr lo;
        VAddr hi;
        u64 value;
        u64 required_tick; ///< tick for which the wait was required last (0: none)
    };
    boost::container::small_vector<XferRead, 16> xfer_reads;
    void NoteXferRead(std::span<const ReadbackItem> pending, u64 value);
    void NoteWriteAgainstXferReads(VAddr addr, u64 size);
    void PruneXferReads();
    /// sotc: the range the guest touched, during the DownloadMemory of its window (size 0 else).
    VAddr fault_addr{};
    u64 fault_size{};

public:
    /// sotc: true once after the command buffer being built wrote a page the guest reads back:
    /// submitting it now lets the GPU finish it before the guest asks for it.
    bool TakeHotWrite() noexcept;

    /// sotc: SOTC_RB_PREFETCH: between commands, outside a render pass (Rasterizer::
    /// FlushPeriodic): records the copies of the recurring readback windows the GPU wrote since
    /// their last prefetch, and frees expired or dirty prefetches. True when it recorded copies.
    bool PrefetchIssue();

private:

    /// sotc: arena memory is carved out of chunks of ARENA_CHUNK_SIZE. One allocation per
    /// resident range reached thousands within minutes, each one a kernel call mid-frame (the
    /// open world then ran out of video memory).
    static constexpr u64 ARENA_CHUNK_SIZE = 64_MB;
    /// sotc: SOTC_HRR=1: host-visible chunks (AllocateArenaBlocksHost), mapped whole.
    static constexpr u64 ARENA_HOST_CHUNK_SIZE = 16_MB;
    struct ArenaChunk {
        vk::DeviceMemory memory;
        u32 memory_type{};
        u64 used_blocks{};
        std::map<u64, u64> free_runs; ///< first block -> number of free blocks
        /// sotc: SOTC_HRR=1: whole-chunk mapping of a host-visible chunk, nullptr for the others.
        u8* mapped{};
    };
    std::vector<std::unique_ptr<ArenaChunk>> arena_chunks;
    std::unordered_map<VkDeviceMemory, ArenaChunk*> chunk_by_memory;
    /// Memory and first block for blocks of arena backing (video memory, else system memory).
    std::pair<vk::DeviceMemory, u64> AllocateArenaBlocks(u64 blocks);
    /// sotc: SOTC_HRR=1 (hrr.h): like AllocateArenaBlocks, but only in host-visible memory
    /// (arena_host_memory_type_index), in chunks that are mapped when created. {nullptr, 0} when
    /// there is no such memory or no room: the caller then uses AllocateArenaBlocks.
    std::pair<vk::DeviceMemory, u64> AllocateArenaBlocksHost(u64 blocks);
    /// sotc: mapped pointer of [addr, addr + size) when every block of it is resident in one host
    /// chunk, contiguously; nullptr otherwise.
    u8* ArenaHostPointer(VAddr addr, u64 size) const;
    /// sotc: ArenaHostPointer, also giving the memory object and the byte offset in it.
    u8* ArenaHostLocate(VAddr addr, u64 size, vk::DeviceMemory& memory, u64& offset) const;
    /// sotc: SOTC_HRR=1: true when every copy of a direct readback still lies in a mapped host
    /// chunk (ArenaHostPointer): the bytes FinishAsyncReadback reads are there.
    bool DirectReadable(const AsyncReadback& readback) const;
    /// sotc: SOTC_HRR=1: number of host-visible arena chunks (AllocateArenaBlocksHost, ReturnArenaRun).
    u32 arena_host_chunks{};
    void FreeArenaBlocks(vk::DeviceMemory memory, u64 start, u64 blocks);
    void ReturnArenaRun(vk::DeviceMemory memory, u64 start, u64 blocks);
    /// sotc: SOTC_ARENA_TICK_FREE=1: runs freed by FreeArenaBlocks wait here, stamped with the
    /// scheduler tick, until ReclaimArenaRuns sees the GPU past it. Ticks only grow, so the
    /// front is the oldest.
    struct PendingArenaRun {
        vk::DeviceMemory memory;
        u32 memory_type;
        u64 start;
        u64 blocks;
        u64 tick;
    };
    std::deque<PendingArenaRun> pending_arena_runs;
    void ReclaimArenaRuns();
    /// sotc: unbinds resident block ranges (zeroing their page table entries) and frees the
    /// allocations left without blocks. The GPU must no longer use them.
    void UnbindResident(std::span<const Backing> released);
    /// sotc: per arena block, the use epoch (seconds, wrapping, 0 = never) of its last bind.
    u16* block_last_use{};
    /// sotc: blocks MaybeEvictCold gave back and nothing bound again yet.
    IntervalList<> evicted_ranges;
    /// sotc: evicted blocks a shader faulted back in; never evicted again.
    IntervalList<> pinned_ranges;
    /// sotc: resident ranges (by first block) whose read-back did not match guest memory,
    /// tried again after this.
    std::unordered_map<u64, std::chrono::steady_clock::time_point> evict_retry;
    /// sotc: device arena memory in use (blocks handed out of the device chunks), in bytes. Host
    /// chunks (SOTC_HRR) are not counted: the eviction limit is about video memory.
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
    };
    struct PendingEviction {
        std::vector<EvictChoice> chosen;
        Vulkan::StagingBufferRef download;
        u64 tick{};
        std::chrono::steady_clock::time_point deadline{};
    };
    std::optional<PendingEviction> pending_eviction;
    /// migration: true when called by MigrateHostResident (keeps hot_readbacks: the granule stays
    /// hot, it only moves to host memory).
    bool FinishEviction(std::chrono::steady_clock::time_point now, u16 idle_epochs, bool wait,
                        bool migration = false);
    /// sotc: SOTC_HRR=1 (hrr.h): moves a few newly promoted granules from device memory to host
    /// memory, through FinishEviction's own checks and unbind. Called from PrefetchIssue (a
    /// command boundary outside a render pass, draw thread). Granules that fail a check stay where
    /// they are: they are tried again up to 3 times, at most once a second. Synchronous.
    void MigrateHostResident();
    /// sotc: SOTC_HRR=1: a promoted granule waiting for MigrateHostResident (draw thread only).
    /// tries: failed attempts so far; next: the earliest time of the next attempt.
    struct MigrateItem {
        VAddr lo{};
        u32 tries{};
        std::chrono::steady_clock::time_point next{};
    };
    std::vector<MigrateItem> hrr_migrate_queue;
    /// sotc: SOTC_HRR=1: migrated granules per game frame (SotcStats::flip_count) and the frame
    /// they were counted in.
    u64 hrr_frame{};
    u32 hrr_frame_granules{};

    u32 arena_memory_type_index{};
    /// sotc: SOTC_HRR=1: host-visible memory type for promoted arena ranges (AllocateArenaBlocksHost):
    /// HOST_VISIBLE|HOST_CACHED outside device-local heaps if any, else inside them. Without a
    /// HOST_CACHED type there is none (Real() then runs as the dry run).
    std::optional<u32> arena_host_memory_type_index;
    bool arena_host_coherent{};
    std::optional<u32> arena_fallback_memory_type_index;
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
};

/// sotc: SOTC_RB_BATCH=1 (off by default). While one is alive on a thread (the draw thread's
/// command queue batch, Liverpool::ProcessCommands), the readbacks started there share one GPU
/// submit, made when the last scope ends, instead of one submit each.
class ReadbackBatchScope {
public:
    ReadbackBatchScope();
    ~ReadbackBatchScope();
    ReadbackBatchScope(const ReadbackBatchScope&) = delete;
    ReadbackBatchScope& operator=(const ReadbackBatchScope&) = delete;
    /// From BeginAsyncReadback: true when the submit was left to the scope.
    static bool Defer(Vulkan::Scheduler& scheduler);
};

} // namespace VideoCore
