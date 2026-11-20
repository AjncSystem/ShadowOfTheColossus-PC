// SPDX-FileCopyrightText: Copyright 2026 SotC-PC
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_CULL_QUEUE=1 -- the visibility (cull) dispatches of the programs in
// SOTC_CULL_QUEUE_PROGRAMS (default 0x50784801) run on a second queue of the graphics family
// (docs/sotc/PLANO-FILA-CULL.md). On the single graphics queue each of them started ~10 ms of GPU
// time after its inputs were ready, queued behind unrelated work, and the game waited for its
// result (the visibility bitmap) in the same frame.
//
// How it stays ordered:
// - Watched ranges: the ranges such a dispatch binds are learned the first time it runs (that run
//   takes the normal path); from then on every graphics access to them (Runtime::AccessBuffer,
//   BufferCache::RecordCurrentWrite) stamps the graphics tick that recorded it.
// - A cull dispatch waits (timeline semaphore) for the newest graphics tick that wrote what it
//   reads or touched what it writes. When that is the command buffer still being recorded, the
//   graphics work is submitted first.
// - Its commands (uploads of CPU data, barriers, bind, dispatch) are recorded straight into a
//   real command buffer of the cull queue, with a barrier tracking of its own (Runtime swap).
//   Consecutive cull dispatches share one command buffer; the first graphics command after them
//   (or any graphics submit, idle, readback) submits it.
// - Every graphics submit waits for the newest cull submit not yet finished: whatever the graphics
//   queue records after a cull runs after it (its readers, later writers, reuse of staging, stream
//   and descriptor memory, released arena blocks). Sparse binds a cull needs are submitted on
//   their own; the cull and the next graphics submit wait for them.
// - The game's reads of a range only cull dispatches wrote (no graphics write since) copy it on
//   the cull queue: the guest waits for the cull, not for the graphics queue's tail.
// - Not covered (images, GDS, texel buffers, pointers, indirect dispatches, ranges not learned
//   yet, too many watched ranges): the normal path, exactly as before.
// SOTC_CULL_QUEUE_VERIFY=1: each redirected dispatch also runs on the graphics queue at its old
// place into a scratch copy of the output; both results are compared byte by byte ("SOTC CULL
// VERIFY" lines). Off (default 0): nothing of this runs and the queues are created as before.
#pragma once

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

namespace Shader {
struct Info;
}
namespace Core {
class MemoryManager;
}
namespace VideoCore {
class BufferCache;
class TextureCache;
struct Buffer;
} // namespace VideoCore

namespace Vulkan {

class Instance;
class Scheduler;
struct SubmitInfo;

class CullQueue {
public:
    /// SOTC_CULL_QUEUE=1 and the device got the queue.
    static bool Enabled();
    /// SOTC_CULL_QUEUE_VERIFY=1.
    static bool VerifyEnabled();

    CullQueue(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
              VideoCore::BufferCache& buffer_cache, VideoCore::TextureCache& texture_cache,
              Core::MemoryManager* memory);
    ~CullQueue();

    /// A program of SOTC_CULL_QUEUE_PROGRAMS.
    static bool IsCandidate(u64 hash);
    /// Before a direct dispatch of cs is bound (draw thread). True: its commands go to the cull
    /// queue until End(); false: the normal path.
    bool Begin(const Shader::Info& cs);
    /// After the dispatch and its ResetBindings, on every path once Begin returned true.
    void End();
    [[nodiscard]] bool Recording() const noexcept {
        return recording;
    }

    /// Submits the open batch of cull dispatches, if any (draw thread).
    void SubmitOpen();
    /// The graphics scheduler's submit callback (draw thread, before the submit is queued).
    void OnGraphicsSubmit(SubmitInfo& info);
    /// A graphics access to guest memory (draw thread or command processor).
    void NoteGraphicsAccess(VAddr addr, u64 size, bool write);
    void NoteGraphicsAccess(const VideoCore::Buffer* buffer, u64 offset, u64 size, bool write);
    /// Cull work submitted or recorded and not known to be finished.
    [[nodiscard]] bool Busy();

    /// Readbacks: true when every GPU-written byte of [start, end) was written by cull
    /// dispatches only (and [addr, +size) lies in one such range); start/end come back clamped
    /// to that range, widened to tracker pages.
    bool OwnsForReadback(VAddr addr, u64 size, VAddr& start, VAddr& end);
    bool IsOwned(VAddr start, VAddr end);
    /// Records copies from arena into dst on the cull queue and submits them; the tick to wait.
    u64 SubmitReadback(const VideoCore::Buffer* arena, const VideoCore::Buffer* dst,
                       std::span<const vk::BufferCopy> copies);
    /// Guest threads: waits until the cull queue finished tick.
    void Wait(u64 tick);

    // SOTC_CULL_QUEUE_VERIFY (vk_rasterizer.cpp drives it).
    struct VerifySlot {
        std::unique_ptr<VideoCore::Buffer> scratch;
        u64 tick{}; ///< graphics tick of its last use
    };
    static constexpr u64 VerifySlotBytes = 8_MB;
    /// A scratch buffer of VerifySlotBytes no GPU work still uses (before Begin: it may wait).
    VerifySlot* VerifyAcquire();
    void VerifyCompare(VerifySlot* slot, u64 size, const u8* truth, const u8* queue);
    void VerifySkip();

private:
    struct Watch {
        VAddr end;
        u64 last_write, last_access; ///< graphics ticks
    };
    struct Pending {
        vk::CommandBuffer cmdbuf;
        u64 tick;
    };

    vk::CommandBuffer AcquireCommandBuffer();
    void OpenBatch();
    void SubmitBatch(bool host_wait);
    void QueueSubmit(vk::CommandBuffer cmdbuf, u64 signal, u64 gfx_wait, u64 bind_wait);
    void Report();
    Watch* FindWatch(VAddr start, VAddr end);
    bool Learn(VAddr start, VAddr end);
    bool OwnedRun(VAddr addr, VAddr& start, VAddr& end);

    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    VideoCore::BufferCache& buffer_cache;
    VideoCore::TextureCache& texture_cache;
    Core::MemoryManager* memory;
    vk::Queue queue;
    vk::UniqueCommandPool pool;
    Semaphore semaphore;
    std::mutex queue_mutex;
    std::vector<vk::CommandBuffer> free_cmdbufs;
    std::vector<Pending> pending;
    Runtime::BarrierState barriers;

    // Draw thread state.
    bool recording{};
    bool batch_open{};
    bool split{};          ///< a graphics submit happened while a dispatch was recorded
    vk::CommandBuffer batch_cmdbuf{};
    u64 batch_gfx_dep{};   ///< graphics tick the open batch waits for
    u64 batch_bind_dep{};  ///< memory semaphore tick of its sparse binds
    u64 next_tick{1};      ///< next cull timeline value
    u64 last_submitted{};  ///< newest cull timeline value submitted
    std::atomic<u64> queued_tick{0}; ///< newest cull submit put on the queue
    std::vector<std::pair<VAddr, VAddr>> session_written;

    // Watched and owned ranges, disjoint, by start (any thread that records graphics work:
    // spin lock).
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    std::map<VAddr, Watch> watches;
    std::map<VAddr, VAddr> owned; ///< start -> end
    std::atomic<bool> any_watch{false};
    /// sotc: SOTC_CULL_QUEUE_PAGES (default 1): the newest graphics write and access of every
    /// 64 KiB page (direct-mapped by page; a slot taken by another page means "unknown": the cull
    /// then waits for the graphics work recorded so far). The game allocates the visibility
    /// bitmaps at new addresses every frame, so learning ranges on first sight left most cull
    /// dispatches (and the game's reads of their output) on the graphics queue (18/11: 4500-7300
    /// "learning" per 5 s, 13-70 readbacks on the cull queue of ~5000).
    struct PageSlot {
        u64 page; ///< page + 1; 0 = empty
        u64 last_write, last_access;
    };
    static constexpr size_t PageSlots = size_t{1} << 20;
    std::vector<PageSlot> page_table;
    u64 dma_access_tick{}; ///< newest graphics tick of a shader with pointers (touches anything)
    /// Newest graphics tick that wrote (or, written: accessed) [start, end); current tick when a
    /// page is unknown. Under lock.
    u64 PageDep(VAddr start, VAddr end, bool written);
    static bool PagesEnabled();

    // Statistics (5 s windows, "SOTC CULL QUEUE" lines).
    u64 report_ns{};
    u64 n_redirected{}, n_flush_dep{}, n_learn{}, n_ineligible{}, n_full{}, n_batches{},
        n_via_recorder{}, n_split{}, n_binds{}, n_readbacks{}, n_dep_old{}, n_dep_none{},
        n_dma{}, n_image_alias{};
    u64 dma_tick{};
    u64 dep_age_sum{};
    u64 v_compared{}, v_diff{}, v_diff_bytes{}, v_skipped{};

    std::vector<VerifySlot> verify_slots;
    size_t verify_next{};
};

/// The cull queue when SOTC_CULL_QUEUE is on (null otherwise).
extern CullQueue* g_cull_queue;

} // namespace Vulkan
