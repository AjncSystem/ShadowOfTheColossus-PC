// SPDX-FileCopyrightText: Copyright 2026 SotC-PC
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_CULL_QUEUE, see sotc_cull_queue.h.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <immintrin.h>
#include <boost/container/small_vector.hpp>
#include "common/alignment.h"
#include "common/assert.h"
#include "core/memory.h"
#include "shader_recompiler/info.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/renderer_vulkan/sotc_cull_queue.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_stats.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

CullQueue* g_cull_queue = nullptr;

namespace {

constexpr size_t MaxWatches = 8192;
constexpr size_t NumVerifySlots = 8;

/// The first entry of a disjoint interval map (start -> entry with an end) that may overlap
/// [start, end): the one starting at or before start, else the first after it.
template <typename Map>
auto FirstOverlap(Map& map, VAddr start) {
    auto it = map.upper_bound(start);
    if (it != map.begin()) {
        auto prev = std::prev(it);
        return prev;
    }
    return it;
}

VAddr EndOf(const std::pair<const VAddr, VAddr>& entry) {
    return entry.second;
}

template <typename T>
VAddr EndOf(const std::pair<const VAddr, T>& entry) {
    return entry.second.end;
}

/// Calls func(iterator) for every entry of map overlapping [start, end).
template <typename Map, typename Func>
void ForOverlaps(Map& map, VAddr start, VAddr end, Func&& func) {
    for (auto it = FirstOverlap(map, start); it != map.end() && it->first < end; ++it) {
        if (EndOf(*it) > start) {
            func(it);
        }
    }
}

bool IsCullProgram(u64 hash) {
    static const std::vector<u64> programs = [] {
        std::vector<u64> list;
        const char* v = std::getenv("SOTC_CULL_QUEUE_PROGRAMS");
        const std::string s = v ? v : "0x50784801";
        for (size_t at = 0; at < s.size();) {
            size_t end = s.find(',', at);
            if (end == std::string::npos) {
                end = s.size();
            }
            list.push_back(std::strtoull(s.substr(at, end - at).c_str(), nullptr, 16));
            at = end + 1;
        }
        return list;
    }();
    return std::find(programs.begin(), programs.end(), hash) != programs.end();
}

u64 NowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

struct SpinGuard {
    std::atomic_flag& flag;
    explicit SpinGuard(std::atomic_flag& f) : flag{f} {
        while (flag.test_and_set(std::memory_order_acquire)) {
            _mm_pause();
        }
    }
    ~SpinGuard() {
        flag.clear(std::memory_order_release);
    }
};

void FullBarrier(vk::CommandBuffer cmdbuf) {
    const vk::MemoryBarrier2 barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
}

} // namespace

bool CullQueue::Enabled() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_CULL_QUEUE");
        return v && v[0] != '0';
    }();
    return on;
}

bool CullQueue::IsCandidate(u64 hash) {
    return IsCullProgram(hash);
}

bool CullQueue::VerifyEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_CULL_QUEUE_VERIFY");
        return v && v[0] != '0';
    }();
    return on;
}

CullQueue::CullQueue(const Instance& instance_, Scheduler& scheduler_, Runtime& runtime_,
                     VideoCore::BufferCache& buffer_cache_,
                     VideoCore::TextureCache& texture_cache_, Core::MemoryManager* memory_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      buffer_cache{buffer_cache_}, texture_cache{texture_cache_}, memory{memory_}, queue{instance_.GetCullQueue()},
      semaphore{instance_} {
    const vk::CommandPoolCreateInfo pool_info = {
        .flags = vk::CommandPoolCreateFlagBits::eTransient |
                 vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
    };
    auto [result, new_pool] = instance.GetDevice().createCommandPoolUnique(pool_info);
    ASSERT_MSG(result == vk::Result::eSuccess, "SOTC CULL QUEUE: no command pool: {}",
               vk::to_string(result));
    pool = std::move(new_pool);
    report_ns = NowNs();
    scheduler.SetCullHook([this] { SubmitOpen(); });
    std::fprintf(stderr, "SOTC CULL QUEUE: on%s\n",
                 VerifyEnabled() ? ", verifying against the graphics queue" : "");
}

CullQueue::~CullQueue() {
    scheduler.SetCullHook({});
    std::scoped_lock lk{queue_mutex};
    (void)queue.waitIdle();
}

vk::CommandBuffer CullQueue::AcquireCommandBuffer() {
    // Command buffers of finished submits go back to the free list.
    if (!pending.empty()) {
        semaphore.Refresh();
        while (!pending.empty() && semaphore.IsFree(pending.front().tick)) {
            free_cmdbufs.push_back(pending.front().cmdbuf);
            pending.erase(pending.begin());
        }
    }
    vk::CommandBuffer cmdbuf;
    if (!free_cmdbufs.empty()) {
        cmdbuf = free_cmdbufs.back();
        free_cmdbufs.pop_back();
        Check(cmdbuf.reset());
    } else {
        const vk::CommandBufferAllocateInfo alloc_info = {
            .commandPool = *pool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        };
        auto [result, cmdbufs] = instance.GetDevice().allocateCommandBuffers(alloc_info);
        ASSERT_MSG(result == vk::Result::eSuccess, "SOTC CULL QUEUE: no command buffer: {}",
                   vk::to_string(result));
        cmdbuf = cmdbufs[0];
    }
    Check(cmdbuf.begin(vk::CommandBufferBeginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    }));
    return cmdbuf;
}

void CullQueue::OpenBatch() {
    if (batch_open) {
        return;
    }
    batch_cmdbuf = AcquireCommandBuffer();
    // Everything submitted before on this queue (earlier batches, readback copies) is ordered
    // before it; the graphics work it depends on comes through the semaphore wait.
    FullBarrier(batch_cmdbuf);
    // Its barrier tracking starts empty: the full barrier covers what came before.
    runtime.SwapBarrierState(barriers);
    runtime.ResetBarrierState();
    runtime.SwapBarrierState(barriers);
    batch_open = true;
    batch_gfx_dep = 0;
    batch_bind_dep = 0;
}

CullQueue::Watch* CullQueue::FindWatch(VAddr start, VAddr end) {
    auto it = watches.upper_bound(start);
    if (it == watches.begin()) {
        return nullptr;
    }
    --it;
    return it->first <= start && end <= it->second.end ? &it->second : nullptr;
}

bool CullQueue::Learn(VAddr start, VAddr end) {
    // Stamped as accessed by the command buffer being recorded: whatever touched it before did
    // so at that tick or earlier. Overlapping or touching watches merge (newest ticks kept).
    const u64 tick = scheduler.CurrentTick();
    VAddr merged_start = start;
    Watch merged{end, tick, tick};
    auto it = FirstOverlap(watches, start);
    while (it != watches.end() && it->first <= merged.end) {
        if (it->second.end < merged_start) {
            ++it;
            continue;
        }
        merged_start = std::min(merged_start, it->first);
        merged.end = std::max(merged.end, it->second.end);
        merged.last_write = std::max(merged.last_write, it->second.last_write);
        merged.last_access = std::max(merged.last_access, it->second.last_access);
        it = watches.erase(it);
    }
    if (watches.size() >= MaxWatches) {
        // Only when nothing merged: the merged ones are erased above and must come back.
        return false;
    }
    watches.emplace(merged_start, merged);
    any_watch.store(true, std::memory_order_release);
    return true;
}

bool CullQueue::Begin(const Shader::Info& cs) {
    if (recording || !IsCullProgram(cs.pgm_hash)) {
        return false;
    }
    Report();
    // Only buffers: no images (layouts belong to the graphics queue), no GDS, no pointers, no
    // texel buffers an image would be synchronized into (checked per range below).
    bool eligible = cs.images.empty() && !cs.uses_dma &&
                    !(SotcStats::kVisDirect && SotcStats::VisDirectProgram(cs.pgm_hash));
    struct Range {
        VAddr start, end;
        bool written;
    };
    boost::container::small_vector<Range, 8> ranges;
    static const u64 unbounded_window = [] {
        const char* v = std::getenv("SHADPS4_UNBOUNDED_BUFFER_MB");
        return (v ? std::strtoull(v, nullptr, 10) : 64ULL) << 20;
    }();
    for (const auto& desc : cs.buffers) {
        if (!eligible) {
            break;
        }
        if (desc.IsSpecial()) {
            eligible = desc.buffer_type == Shader::BufferType::Flatbuf ||
                       desc.buffer_type == Shader::BufferType::SharedMemory;
            continue;
        }
        // The range BindBuffers binds (same clamp and unbounded window).
        const auto vsharp = desc.GetSharp(cs);
        if (vsharp.base_address == 0 || vsharp.GetSize() == 0) {
            continue;
        }
        u64 size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
        if (size == 0) {
            continue;
        }
        constexpr u64 MaxSaneBindingSize = 256ULL << 20;
        if (!desc.is_written && (vsharp.GetSize() >= 0xFFFF0000ULL || size > MaxSaneBindingSize) &&
            unbounded_window != 0 && size > unbounded_window) {
            size = unbounded_window;
        }
        // A read-only texel buffer over an image is synchronized from it (image commands):
        // this dispatch takes the normal path.
        if (desc.is_formatted && !desc.is_written &&
            texture_cache.FindImageFromRange(vsharp.base_address, static_cast<u32>(size))) {
            ++n_image_alias;
            return false;
        }
        ranges.push_back({vsharp.base_address, vsharp.base_address + size, desc.is_written});
    }
    if (!eligible) {
        ++n_ineligible;
        static bool reported = false;
        if (!std::exchange(reported, true)) {
            std::fprintf(stderr,
                         "SOTC CULL QUEUE: program %#llx not eligible (images %zu, dma %d, or a special buffer); "
                         "normal path\n",
                         static_cast<unsigned long long>(cs.pgm_hash), cs.images.size(),
                         int(cs.uses_dma));
        }
        return false;
    }
    // The newest graphics tick that wrote what it reads or touched what it writes.
    u64 dep = 0;
    bool learned = false;
    bool full = false;
    {
        SpinGuard guard{lock};
        for (const Range& r : ranges) {
            if (const Watch* w = FindWatch(r.start, r.end)) {
                dep = std::max(dep, r.written ? w->last_access : w->last_write);
                continue;
            }
            if (PagesEnabled()) {
                // sotc: SOTC_CULL_QUEUE_PAGES: the page table knows every range (new ones too).
                dep = std::max(dep, PageDep(r.start, r.end, r.written));
                continue;
            }
            learned = true;
            full |= !Learn(r.start, r.end);
        }
    }
    if (learned) {
        // First sight of these ranges: the normal path, which stamps them.
        ++(full ? n_full : n_learn);
        return false;
    }
    const u64 current = scheduler.CurrentTick();
    if (dep >= current) {
        // Recorded in the command buffer being built: submit it first.
        scheduler.Flush();
        ++n_flush_dep;
        dep = current;
    }
    if (dep == 0) {
        ++n_dep_none;
    } else {
        dep_age_sum += scheduler.CurrentTick() - dep;
        if (scheduler.IsFree(dep)) {
            ++n_dep_old;
        }
    }
    OpenBatch();
    batch_gfx_dep = std::max(batch_gfx_dep, dep);
    session_written.clear();
    for (const Range& r : ranges) {
        if (r.written) {
            session_written.emplace_back(r.start, r.end);
        }
    }
    recording = true;
    split = false;
    scheduler.SetCullBatchOpen(false);
    runtime.SwapBarrierState(barriers);
    scheduler.SetCullOverride(batch_cmdbuf);
    return true;
}

void CullQueue::End() {
    if (!recording) {
        return;
    }
    scheduler.SetCullOverride({});
    runtime.SwapBarrierState(barriers);
    recording = false;
    if (!barriers.image_barriers.empty()) {
        // Not expected (no images, no texel buffers): dropped with a report.
        static int reported = 0;
        if (reported++ < 4) {
            std::fprintf(stderr, "SOTC CULL QUEUE: %zu image barriers on the cull queue dropped\n",
                         barriers.image_barriers.size());
        }
        barriers.image_barriers.clear();
    }
    // Sparse binds of its ranges go to the queue now; the cull waits for them.
    if (const u64 bind_tick = buffer_cache.SubmitArenaBindsNow(); bind_tick != 0) {
        batch_bind_dep = std::max(batch_bind_dep, bind_tick);
        ++n_binds;
    }
    {
        SpinGuard guard{lock};
        for (const auto& [start, end] : session_written) {
            // Its output belongs to the cull queue until a graphics write; ranges it overlaps
            // are replaced (they stay owned only as part of this one).
            auto it = FirstOverlap(owned, start);
            while (it != owned.end() && it->first < end) {
                it = it->second > start ? owned.erase(it) : std::next(it);
            }
            owned.emplace(start, end);
        }
    }
    ++n_redirected;
    if (split) {
        // A graphics submit while it was recorded: staging, stream and descriptor memory it
        // uses may be stamped with that submit, which does not wait for it. Wait for it here
        // (rare), so nothing is reused before it ran.
        ++n_split;
        SubmitBatch(true);
        return;
    }
    scheduler.SetCullBatchOpen(true);
}

void CullQueue::SubmitOpen() {
    if (batch_open && !recording) {
        SubmitBatch(false);
    }
}

void CullQueue::SubmitBatch(bool host_wait) {
    scheduler.SetCullBatchOpen(false);
    Check(batch_cmdbuf.end());
    // Staging copies of CPU data done by the copy threads must be complete before the GPU runs.
    buffer_cache.JoinAsyncUploads();
    const u64 tick = next_tick++;
    QueueSubmit(batch_cmdbuf, tick, batch_gfx_dep, batch_bind_dep);
    pending.push_back({batch_cmdbuf, tick});
    last_submitted = tick;
    batch_open = false;
    batch_cmdbuf = vk::CommandBuffer{};
    batch_gfx_dep = 0;
    batch_bind_dep = 0;
    ++n_batches;
    if (host_wait) {
        Wait(tick);
    }
    Report();
}

void CullQueue::QueueSubmit(vk::CommandBuffer cmdbuf, u64 signal, u64 gfx_wait, u64 bind_wait) {
    struct Waits {
        std::array<vk::Semaphore, 2> semas{};
        std::array<u64, 2> values{};
        std::array<vk::PipelineStageFlags, 2> stages{vk::PipelineStageFlagBits::eAllCommands,
                                                      vk::PipelineStageFlagBits::eAllCommands};
        u32 count{};
    } waits;
    if (gfx_wait != 0 && !scheduler.IsFree(gfx_wait)) {
        waits.semas[waits.count] = scheduler.GetWorkSemaphore()->Handle();
        waits.values[waits.count++] = gfx_wait;
    }
    if (bind_wait != 0) {
        waits.semas[waits.count] = buffer_cache.MemorySemaphore();
        waits.values[waits.count++] = bind_wait;
    }
    const vk::Semaphore signal_sema = semaphore.Handle();
    auto submit = [this, cmdbuf, signal, waits, signal_sema] {
        const vk::TimelineSemaphoreSubmitInfo timeline_si = {
            .waitSemaphoreValueCount = waits.count,
            .pWaitSemaphoreValues = waits.values.data(),
            .signalSemaphoreValueCount = 1,
            .pSignalSemaphoreValues = &signal,
        };
        const vk::SubmitInfo submit_info = {
            .pNext = &timeline_si,
            .waitSemaphoreCount = waits.count,
            .pWaitSemaphores = waits.semas.data(),
            .pWaitDstStageMask = waits.stages.data(),
            .commandBufferCount = 1,
            .pCommandBuffers = &cmdbuf,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &signal_sema,
        };
        vk::Result result;
        {
            std::scoped_lock lk{queue_mutex};
            result = queue.submit(submit_info, {});
        }
        if (result == vk::Result::eErrorDeviceLost) {
            instance.ReportDeviceFault();
            UNREACHABLE_MSG("Device lost during a cull queue submit");
        }
        ASSERT_MSG(result == vk::Result::eSuccess, "SOTC CULL QUEUE: submit failed: {}",
                   vk::to_string(result));
        queued_tick.store(signal, std::memory_order_release);
        queued_tick.notify_all();
    };
    // Straight to the queue when the graphics submit it waits for is on the queue already and
    // no earlier cull submit is still waiting on the recording thread (cull submits stay in
    // order); otherwise behind that graphics submit on the recording thread.
    const bool in_order = queued_tick.load(std::memory_order_acquire) + 1 == signal;
    if (in_order && (gfx_wait == 0 || scheduler.RecordedSubmitTick() >= gfx_wait)) {
        submit();
    } else {
        ++n_via_recorder;
        scheduler.RunAfterRecorded(std::move(submit));
    }
}

void CullQueue::OnGraphicsSubmit(SubmitInfo& info) {
    if (recording) {
        // Submitted in the middle of a cull dispatch's recording (rare): the open batch is not
        // finished; End() submits it and waits for it on the host.
        split = true;
    } else {
        SubmitOpen();
    }
    if (last_submitted == 0 || semaphore.IsFree(last_submitted)) {
        return;
    }
    semaphore.Refresh();
    if (semaphore.IsFree(last_submitted)) {
        return;
    }
    if (info.num_wait_semas < info.wait_semas.size()) {
        info.AddWaitAll(semaphore.Handle(), last_submitted);
    } else {
        Wait(last_submitted); // no room left: the host waits instead
    }
}

bool CullQueue::Busy() {
    if (batch_open) {
        return true;
    }
    if (last_submitted == 0 || semaphore.IsFree(last_submitted)) {
        return false;
    }
    semaphore.Refresh();
    return !semaphore.IsFree(last_submitted);
}

void CullQueue::NoteGraphicsAccess(const VideoCore::Buffer* buffer, u64 offset, u64 size,
                                   bool write) {
    if (!buffer || buffer->mem_type != VideoCore::MemoryType::Sparse) {
        return; // not guest memory (staging, stream, scratch buffers)
    }
    NoteGraphicsAccess(buffer->CpuAddr() + offset, size, write);
}

bool CullQueue::PagesEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_CULL_QUEUE_PAGES");
        return !v || v[0] != '0';
    }();
    return on;
}

u64 CullQueue::PageDep(VAddr start, VAddr end, bool written) {
    const u64 current = scheduler.CurrentTick();
    if (end <= start) {
        return 0;
    }
    const u64 first = start >> 16, last = (end - 1) >> 16;
    if (last - first > 4096 || page_table.empty()) {
        return current;
    }
    u64 dep = written ? dma_access_tick : 0;
    for (u64 page = first; page <= last; ++page) {
        const PageSlot& slot = page_table[page & (PageSlots - 1)];
        if (slot.page == 0) {
            continue; // no graphics work touched it
        }
        if (slot.page != page + 1) {
            return current; // taken by another page: unknown
        }
        dep = std::max(dep, written ? slot.last_access : slot.last_write);
    }
    return dep;
}

void CullQueue::NoteGraphicsAccess(VAddr addr, u64 size, bool write) {
    if (size == 0) {
        return;
    }
    const bool pages = PagesEnabled();
    if (!pages && !any_watch.load(std::memory_order_acquire)) {
        return;
    }
    const VAddr end = addr + size;
    const u64 tick = scheduler.CurrentTick();
    SpinGuard guard{lock};
    if (size == ~u64{0} && dma_tick != tick) {
        dma_tick = tick;
        ++n_dma; // a shader with pointers: every range counts as touched
    }
    if (pages) {
        if (size == ~u64{0}) {
            dma_access_tick = tick;
        } else if (const u64 first = addr >> 16, last = (end - 1) >> 16; last - first <= 4096) {
            if (page_table.empty()) {
                page_table.resize(PageSlots);
            }
            for (u64 page = first; page <= last; ++page) {
                PageSlot& slot = page_table[page & (PageSlots - 1)];
                if (slot.page == 0) {
                    // First touch of any page of this slot: this page has no older history.
                    slot = {page + 1, write ? tick : 0, tick};
                    continue;
                }
                if (slot.page != page + 1) {
                    // Another page's slot: its history goes (a later lookup of it sees
                    // "unknown"), and this page's own older history is unknown too, so it
                    // counts as written now.
                    slot = {page + 1, tick, tick};
                    continue;
                }
                slot.last_access = std::max(slot.last_access, tick);
                if (write) {
                    slot.last_write = std::max(slot.last_write, tick);
                }
            }
        } else {
            dma_access_tick = tick; // a huge range: treat it as touching anything
        }
    }
    ForOverlaps(watches, addr, end, [&](auto it) {
        Watch& w = it->second;
        w.last_access = std::max(w.last_access, tick);
        if (write) {
            w.last_write = std::max(w.last_write, tick);
        }
    });
    if (write && !owned.empty()) {
        auto it = FirstOverlap(owned, addr);
        while (it != owned.end() && it->first < end) {
            it = it->second > addr ? owned.erase(it) : std::next(it);
        }
    }
}

bool CullQueue::OwnedRun(VAddr addr, VAddr& start, VAddr& end) {
    // The run of touching owned ranges around addr.
    auto it = owned.upper_bound(addr);
    if (it == owned.begin()) {
        return false;
    }
    --it;
    if (it->second <= addr) {
        return false;
    }
    start = it->first;
    end = it->second;
    for (auto left = it; left != owned.begin();) {
        const auto prev = std::prev(left);
        if (prev->second != start) {
            break;
        }
        start = prev->first;
        left = prev;
    }
    for (auto right = std::next(it); right != owned.end() && right->first == end; ++right) {
        end = right->second;
    }
    return true;
}

bool CullQueue::OwnsForReadback(VAddr addr, u64 size, VAddr& start, VAddr& end) {
    SpinGuard guard{lock};
    VAddr run_start = 0, run_end = 0;
    if (!OwnedRun(addr, run_start, run_end) || addr + size > run_end) {
        return false;
    }
    start = Common::AlignDown(run_start, VideoCore::BYTES_PER_PAGE);
    end = Common::AlignUp(run_end, VideoCore::BYTES_PER_PAGE);
    return true;
}

bool CullQueue::IsOwned(VAddr start, VAddr end) {
    SpinGuard guard{lock};
    VAddr run_start = 0, run_end = 0;
    return OwnedRun(start, run_start, run_end) && end <= run_end;
}

u64 CullQueue::SubmitReadback(const VideoCore::Buffer* arena, const VideoCore::Buffer* dst,
                              std::span<const vk::BufferCopy> copies) {
    // After the open batch (the dispatches that wrote it), in a command buffer of its own.
    SubmitOpen();
    const vk::CommandBuffer cmdbuf = AcquireCommandBuffer();
    FullBarrier(cmdbuf);
    cmdbuf.copyBuffer(arena->Handle(), dst->Handle(), copies);
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
    Check(cmdbuf.end());
    const u64 tick = next_tick++;
    QueueSubmit(cmdbuf, tick, 0, 0);
    pending.push_back({cmdbuf, tick});
    last_submitted = tick;
    ++n_readbacks;
    return tick;
}

void CullQueue::Wait(u64 tick) {
    u64 queued = queued_tick.load(std::memory_order_acquire);
    while (queued < tick) {
        queued_tick.wait(queued, std::memory_order_acquire);
        queued = queued_tick.load(std::memory_order_acquire);
    }
    semaphore.Wait(tick);
}

CullQueue::VerifySlot* CullQueue::VerifyAcquire() {
    if (verify_slots.empty()) {
        verify_slots.resize(NumVerifySlots);
    }
    VerifySlot& slot = verify_slots[verify_next];
    verify_next = (verify_next + 1) % verify_slots.size();
    if (!slot.scratch) {
        slot.scratch = std::make_unique<VideoCore::Buffer>(
            instance, 0, VerifySlotBytes, VideoCore::MemoryType::DeviceLocal, "SotC cull verify");
    }
    if (slot.tick != 0 && !scheduler.IsFree(slot.tick)) {
        scheduler.Wait(slot.tick);
    }
    return &slot;
}

void CullQueue::VerifyCompare(VerifySlot* slot, u64 size, const u8* truth, const u8* queue_data) {
    (void)slot;
    ++v_compared;
    u64 diff = 0;
    u64 first = ~0ull;
    for (u64 i = 0; i < size; ++i) {
        if (truth[i] != queue_data[i]) {
            if (first == ~0ull) {
                first = i;
            }
            ++diff;
        }
    }
    if (diff != 0) {
        ++v_diff;
        v_diff_bytes += diff;
        static int shown = 0;
        if (shown++ < 16) {
            std::fprintf(stderr,
                         "SOTC CULL VERIFY: %llu of %llu bytes differ (first at +%#llx: graphics "
                         "%02x, cull queue %02x)\n",
                         static_cast<unsigned long long>(diff),
                         static_cast<unsigned long long>(size),
                         static_cast<unsigned long long>(first), truth[first], queue_data[first]);
        }
    }
}

void CullQueue::VerifySkip() {
    ++v_skipped;
}

void CullQueue::Report() {
    const u64 now = NowNs();
    if (now - report_ns < 5'000'000'000ull) {
        return;
    }
    report_ns = now;
    const u64 deps = n_redirected - n_dep_none;
    std::fprintf(stderr,
                 "SOTC CULL QUEUE (5 s): %llu dispatches on the cull queue in %llu batches "
                 "(%llu via the recording thread), %llu readbacks; graphics submitted first for "
                 "%llu; deps already done %llu, mean age %.1f ticks; split %llu, binds %llu, "
                 "pointer shaders %llu; normal path: learning %llu, too many ranges %llu, texel "
                 "buffer over an image %llu, not eligible %llu\n",
                 static_cast<unsigned long long>(n_redirected),
                 static_cast<unsigned long long>(n_batches),
                 static_cast<unsigned long long>(n_via_recorder),
                 static_cast<unsigned long long>(n_readbacks),
                 static_cast<unsigned long long>(n_flush_dep),
                 static_cast<unsigned long long>(n_dep_old),
                 deps ? double(dep_age_sum) / double(deps) : 0.0,
                 static_cast<unsigned long long>(n_split), static_cast<unsigned long long>(n_binds),
                 static_cast<unsigned long long>(n_dma), static_cast<unsigned long long>(n_learn),
                 static_cast<unsigned long long>(n_full),
                 static_cast<unsigned long long>(n_image_alias),
                 static_cast<unsigned long long>(n_ineligible));
    if (VerifyEnabled()) {
        std::fprintf(stderr,
                     "SOTC CULL VERIFY (5 s): %llu compared, %llu with differences (%llu bytes), "
                     "%llu skipped\n",
                     static_cast<unsigned long long>(v_compared),
                     static_cast<unsigned long long>(v_diff),
                     static_cast<unsigned long long>(v_diff_bytes),
                     static_cast<unsigned long long>(v_skipped));
        v_compared = v_diff = v_diff_bytes = v_skipped = 0;
    }
    n_redirected = n_flush_dep = n_learn = n_ineligible = n_full = n_batches = n_via_recorder =
        n_split = n_binds = n_readbacks = n_dep_old = n_dep_none = n_dma = n_image_alias = 0;
    dep_age_sum = 0;
}

} // namespace Vulkan
