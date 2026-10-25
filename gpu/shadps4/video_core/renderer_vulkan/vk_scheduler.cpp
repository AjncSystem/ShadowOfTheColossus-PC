// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>
#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_recorder.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_stats.h"
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <thread>
#include <mutex>
#include <deque>
#include <span>
#include <boost/container/small_vector.hpp>
#include <immintrin.h>

namespace Vulkan {

// sotc: SOTC_GPU_TIME=1 (needs the GPU marker, SOTC_WAIT_MARKER=1 or 2): once a second, how long the
// GPU worked on the submissions (busy: from the later of the queue submit and the previous
// submission's end, to the end of this one) and how long it sat without submitted work (starved).
// Busy + starved is the whole second: a large starved share means the GPU waits for the CPU.
namespace {
struct GpuTimeDiag {
    std::mutex mutex;
    struct Submit {
        u64 tick;
        std::chrono::steady_clock::time_point at;
        Semaphore* sem;
    };
    std::vector<Submit> pending;
    std::jthread thread;
    // SOTC_GPU_TIME=2: GPU timestamps at the start and end of every rendering interval (Begin/
    // EndRendering). The gaps between an end and the next start are the GPU work outside render
    // passes (compute, copies, barriers, waits).
    struct Stamp {
        u32 slot;
        bool end;
        u64 tick;
        Semaphore* sem;
    };
    static constexpr u32 kSlots = 16384;
    bool stamps_on{};
    vk::Device device;
    vk::QueryPool pool;
    double period_ms{};
    std::atomic<u32> next_slot{0};
    std::atomic<u64> recorded{0};
    std::vector<Stamp> pending_stamps;

    /// Called from the recording thread right where a rendering interval starts (end false) or ends.
    void Stamp_(const vk::Device& dev, double period, vk::CommandBuffer cmdbuf, Semaphore* sem, u64 tick, bool end) {
        if (!pool) {
            std::scoped_lock lock{mutex};
            if (!pool) {
                device = dev;
                period_ms = period * 1e-6;
                const vk::QueryPoolCreateInfo info{.queryType = vk::QueryType::eTimestamp, .queryCount = kSlots};
                pool = device.createQueryPool(info).value;
            }
        }
        const u32 slot = next_slot.fetch_add(1, std::memory_order_relaxed) % kSlots;
        recorded.fetch_add(1, std::memory_order_relaxed);
        cmdbuf.resetQueryPool(pool, slot, 1);
        cmdbuf.writeTimestamp(end ? vk::PipelineStageFlagBits::eBottomOfPipe : vk::PipelineStageFlagBits::eTopOfPipe,
                              pool, slot);
        std::scoped_lock lock{mutex};
        pending_stamps.push_back({slot, end, tick, sem});
    }

    void Note(Semaphore* sem, u64 tick) {
        std::scoped_lock lock{mutex};
        if (!thread.joinable()) {
            thread = std::jthread([this](std::stop_token stop) { Run(stop); });
        }
        pending.push_back({tick, std::chrono::steady_clock::now(), sem});
    }

    void Run(std::stop_token stop) {
        using clock = std::chrono::steady_clock;
        auto last_done = clock::now();
        auto window = last_done;
        double busy = 0, starved = 0;
        u64 ticks = 0;
        std::vector<Submit> work;
        while (!stop.stop_requested()) {
            {
                std::scoped_lock lock{mutex};
                work.swap(pending);
            }
            static std::deque<Submit> queue;
            static std::deque<Stamp> stamp_queue;
            static double last_end = -1, begin_at = 0, pass_ms = 0, gap_ms = 0, big_gap_ms = 0;
            static u64 passes = 0, big_gaps = 0, not_ready = 0, filtered = 0, read_ok = 0, last_recorded = 0;
            {
                std::scoped_lock lock{mutex};
                for (const auto& st : pending_stamps) {
                    stamp_queue.push_back(st);
                }
                pending_stamps.clear();
            }
            while (!stamp_queue.empty() &&
                   stamp_queue.front().sem->MarkerReached(Semaphore::MarkerSlot::Timeline, stamp_queue.front().tick)) {
                // A run of stamps in consecutive slots (the recording order) whose submissions are done.
                const u32 first = stamp_queue.front().slot;
                u32 count = 0;
                while (count < stamp_queue.size() && count < 2048 && first + count < kSlots &&
                       stamp_queue[count].slot == first + count &&
                       stamp_queue[count].sem->MarkerReached(Semaphore::MarkerSlot::Timeline, stamp_queue[count].tick)) {
                    ++count;
                }
                static std::vector<u64> values;
                values.assign(count, 0);
                if (device.getQueryPoolResults(pool, first, count, count * sizeof(u64), values.data(), sizeof(u64),
                                               vk::QueryResultFlagBits::e64) != vk::Result::eSuccess) {
                    not_ready += count;
                    stamp_queue.erase(stamp_queue.begin(), stamp_queue.begin() + count);
                    continue;
                }
                for (u32 i = 0; i < count; ++i) {
                    const Stamp st = stamp_queue[i];
                    const double t = static_cast<double>(values[i]) * period_ms;
                    ++read_ok;
                    if (!st.end) {
                        if (last_end >= 0 && t >= last_end && t - last_end < 50.0) {
                            gap_ms += t - last_end;
                            if (t - last_end > 0.25) {
                                big_gap_ms += t - last_end;
                                ++big_gaps;
                            }
                        }
                        begin_at = t;
                    } else {
                        if (t >= begin_at && t - begin_at < 50.0) {
                            pass_ms += t - begin_at;
                            ++passes;
                        } else {
                            ++filtered;
                        }
                        last_end = t;
                    }
                }
                stamp_queue.erase(stamp_queue.begin(), stamp_queue.begin() + count);
            }
            if (stamps_on && clock::now() - window >= std::chrono::seconds(1)) {
                std::fprintf(stderr,
                             "SOTC GPU PASSES: %llu rendering intervals/s, in them %.0f ms/s, between them %.0f ms/s "
                             "(of which %llu gaps over 0.25 ms = %.0f ms/s)\n",
                             static_cast<unsigned long long>(passes), pass_ms, gap_ms,
                             static_cast<unsigned long long>(big_gaps), big_gap_ms);
                const u64 rec = recorded.load(std::memory_order_relaxed);
                std::fprintf(stderr, "SOTC GPU PASSES stamps: queued %zu, recorded %llu/s, read %llu, not ready %llu, filtered %llu\n",
                             stamp_queue.size(), static_cast<unsigned long long>(rec - last_recorded), static_cast<unsigned long long>(read_ok),
                             static_cast<unsigned long long>(not_ready), static_cast<unsigned long long>(filtered));
                last_recorded = rec;
                passes = big_gaps = not_ready = filtered = read_ok = 0;
                pass_ms = gap_ms = big_gap_ms = 0;
            }
            for (auto& item : work) {
                queue.push_back(item);
            }
            work.clear();
            const auto now = clock::now();
            while (!queue.empty() && queue.front().sem->MarkerReached(Semaphore::MarkerSlot::Timeline, queue.front().tick)) {
                const auto done = clock::now();
                const auto submit = queue.front().at;
                const auto start = std::max(submit, last_done);
                busy += std::chrono::duration<double, std::milli>(done - start).count();
                if (submit > last_done) {
                    starved += std::chrono::duration<double, std::milli>(submit - last_done).count();
                }
                last_done = done;
                ++ticks;
                queue.pop_front();
            }
            if (now - window >= std::chrono::seconds(1)) {
                std::fprintf(stderr, "SOTC GPU TIME: busy %.0f ms/s, no work queued %.0f ms/s, %llu submissions/s\n", busy,
                             starved, static_cast<unsigned long long>(ticks));
                busy = starved = 0;
                ticks = 0;
                window = now;
            }
            _mm_pause();
            std::this_thread::yield();
        }
    }
};

GpuTimeDiag* GpuTime() {
    static GpuTimeDiag* diag = [] {
        const char* v = std::getenv("SOTC_GPU_TIME");
        if (!v || (v[0] != '1' && v[0] != '2')) {
            return static_cast<GpuTimeDiag*>(nullptr);
        }
        auto* d = new GpuTimeDiag;
        d->stamps_on = v[0] == '2';
        return d;
    }();
    return diag;
}
} // namespace

std::mutex Scheduler::submit_mutex;

Scheduler::Scheduler(const Instance& instance, bool record_thread)
    : instance{instance}, work_semaphore{instance}, command_pool{instance, &work_semaphore} {
    if (record_thread) {
        recorder = std::make_unique<CommandRecorder>();
        LOG_INFO(Render_Vulkan, "Vulkan commands are recorded on their own thread");
    }
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    BeginSession();
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

void Scheduler::ClaimRecorder() {
    if (recorder) {
        recorder->ClaimProducer();
    }
}

void Scheduler::RecordDeferred(Common::UniqueFunction<void, vk::CommandBuffer>&& func) {
    const vk::CommandBuffer cmdbuf = CommandBuffer();
    if (!recorder || !CommandRecorder::IsFake(cmdbuf)) {
        func(vk::CommandBuffer{cmdbuf});
        return;
    }
    recorder->CountCommand();
    recorder->Run([this, cmdbuf, func = std::move(func)] {
        func(vk::CommandBuffer{recorder->Real(cmdbuf)});
    });
}

void Scheduler::BeginRecorded(vk::CommandBuffer fake) {
    const vk::CommandBuffer real = command_pool.Commit();
    Check(real.begin(vk::CommandBufferBeginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    }));
    recorder->Bind(fake, real);
}

void Scheduler::WaitSubmitted(u64 tick, bool kick) {
    if (!recorder || recorded_submit_tick.load(std::memory_order_acquire) >= tick) {
        return;
    }
    if (kick) {
        recorder->Kick();
    }
    u64 submitted = recorded_submit_tick.load(std::memory_order_acquire);
    for (u32 spins = 0; spins < 4096 && submitted < tick; ++spins) {
        _mm_pause();
        submitted = recorded_submit_tick.load(std::memory_order_acquire);
    }
    while (submitted < tick) {
        recorded_submit_tick.wait(submitted, std::memory_order_acquire);
        submitted = recorded_submit_tick.load(std::memory_order_acquire);
    }
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    if (is_rendering && render_state == new_state) {
        return;
    }
    EndRendering();
    is_rendering = true;
    render_state = new_state;

    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    if (auto* diag = GpuTime(); diag && diag->stamps_on && work_semaphore.HasMarker()) {
        diag->Stamp_(instance.GetDevice(), instance.GetPhysicalDevice().getProperties().limits.timestampPeriod,
                     CommandBuffer(), &work_semaphore, work_semaphore.CurrentTick(), false);
    }
    CommandBuffer().beginRendering(rendering_info);
}

void Scheduler::EndRendering() {
    if (!is_rendering) {
        return;
    }
    if (SotcStats::kCmdStats) { // sotc: read only by the SOTC_CMD_STATS printer (sotc_stats.h)
        SotcStats::render_breaks.fetch_add(1, std::memory_order_relaxed);
    }
    is_rendering = false;
    CommandBuffer().endRendering();
    if (auto* diag = GpuTime(); diag && diag->stamps_on && work_semaphore.HasMarker()) {
        diag->Stamp_(instance.GetDevice(), instance.GetPhysicalDevice().getProperties().limits.timestampPeriod,
                     CommandBuffer(), &work_semaphore, work_semaphore.CurrentTick(), true);
    }
}

vk::CommandBuffer Scheduler::UploadCommandBuffer() {
    auto& upload_cmdbuf = sessions.back().upload;
    if (upload_cmdbuf) {
        return upload_cmdbuf;
    }
    if (recorder) {
        upload_cmdbuf = recorder->NewFakeHandle();
        recorder->Run([this, fake = upload_cmdbuf] { BeginRecorded(fake); });
        return upload_cmdbuf;
    }
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };
    upload_cmdbuf = command_pool.Commit();
    Check(upload_cmdbuf.begin(begin_info));
    return upload_cmdbuf;
}

void Scheduler::Flush(SubmitInfo& info) {
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info);
}

void Scheduler::Flush() {
    SubmitInfo info{};
    Flush(info);
}

void Scheduler::Finish() {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info);
    Wait(presubmit_tick);
}

void Scheduler::FinishHostRead() {
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info);
    WaitSubmitted(presubmit_tick);
    work_semaphore.WaitHostRead(presubmit_tick);
}

void Scheduler::Wait(u64 tick) {
    if (tick >= work_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    WaitSubmitted(tick);
    work_semaphore.Wait(tick);
}

vk::CommandBuffer Scheduler::BeginAhead() {
    // Idea from the readback ahead of Pink-shadPS4 (luizgustavs), on the graphics queue only.
    const vk::Device device = instance.GetDevice();
    if (!ahead_pool) {
        const vk::CommandPoolCreateInfo pool_info = {
            .flags = vk::CommandPoolCreateFlagBits::eTransient |
                     vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
        };
        auto [pool_result, pool] = device.createCommandPoolUnique(pool_info);
        ASSERT_MSG(pool_result == vk::Result::eSuccess, "Failed to create the ahead pool: {}",
                   vk::to_string(pool_result));
        ahead_pool = std::move(pool);
        const vk::CommandBufferAllocateInfo alloc_info = {
            .commandPool = *ahead_pool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        };
        auto [alloc_result, cmdbufs] = device.allocateCommandBuffers(alloc_info);
        ASSERT_MSG(alloc_result == vk::Result::eSuccess,
                   "Failed to allocate the ahead command buffer: {}", vk::to_string(alloc_result));
        ahead_cmdbuf = cmdbufs[0];
        auto [fence_result, fence] = device.createFenceUnique({});
        ASSERT_MSG(fence_result == vk::Result::eSuccess, "Failed to create the ahead fence: {}",
                   vk::to_string(fence_result));
        ahead_fence = std::move(fence);
    } else {
        // An ahead copy that returned on its marker may still be pending for the driver.
        if (std::exchange(ahead_unfinished, false)) {
            Check(device.waitForFences(*ahead_fence, true, std::numeric_limits<u64>::max()));
        }
        Check(device.resetFences(*ahead_fence));
    }
    Check(ahead_cmdbuf.reset());
    Check(ahead_cmdbuf.begin(vk::CommandBufferBeginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    }));
    return ahead_cmdbuf;
}

void Scheduler::SubmitAheadAndWait() {
    if (work_semaphore.HasMarker()) {
        work_semaphore.RecordMarker(ahead_cmdbuf, Semaphore::MarkerSlot::Ahead,
                                    static_cast<u32>(++ahead_sequence));
    }
    Check(ahead_cmdbuf.end());
    // With the recorder, the earlier submits must have reached the queue first.
    WaitSubmitted(CurrentTick() - 1);
    // Queue submission order and the barriers recorded in the command buffer order it after
    // all work submitted before; the command buffer being recorded is not involved.
    const vk::SubmitInfo submit_info = {
        .commandBufferCount = 1U,
        .pCommandBuffers = &ahead_cmdbuf,
    };
    {
        std::scoped_lock lk{submit_mutex};
        const auto submit_result = instance.GetGraphicsQueue().submit(submit_info, *ahead_fence);
        if (submit_result == vk::Result::eErrorDeviceLost) {
            instance.ReportDeviceFault();
            UNREACHABLE_MSG("Device lost during ahead submit");
        }
        ASSERT_MSG(submit_result == vk::Result::eSuccess, "Ahead submit failed: {}",
                   vk::to_string(submit_result));
    }
    if (work_semaphore.SpinMarker(Semaphore::MarkerSlot::Ahead, ahead_sequence) &&
        work_semaphore.TrustMarker()) {
        // The copy is readable; the fence is waited for before the command buffer is reused.
        ahead_unfinished = true;
        return;
    }
    if (const u32 spin_us = SotcWaitSpinUs(); spin_us != 0) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(spin_us);
        while (instance.GetDevice().getFenceStatus(*ahead_fence) == vk::Result::eNotReady &&
               std::chrono::steady_clock::now() < deadline) {
            _mm_pause();
        }
    }
    const auto wait_result =
        instance.GetDevice().waitForFences(*ahead_fence, true, std::numeric_limits<u64>::max());
    if (wait_result == vk::Result::eErrorDeviceLost) {
        instance.ReportDeviceFault();
        UNREACHABLE_MSG("Device lost during ahead wait");
    }
    ASSERT_MSG(wait_result == vk::Result::eSuccess, "Ahead wait failed: {}",
               vk::to_string(wait_result));
    // The ahead copy waited for all submitted work.
    work_semaphore.Refresh();
}

void Scheduler::PopPendingOperations() {
    // sotc: runs for many draws. Nothing can be done when the queue is empty or its front op was
    // deferred into the command buffer still being recorded (that tick is not submitted yet), so
    // skip the recursive lock and the semaphore query then. An op is moved out and popped before
    // its callback runs, so a callback reaching here again does not run it twice.
    static const bool fast = [] {
        const char* v = std::getenv("SOTC_POP_FAST");
        return !v || v[0] != '0';
    }();
    if (!fast) {
        std::unique_lock lk(pending_ops_mutex);
        work_semaphore.Refresh();
        while (!pending_ops.empty() && work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
            pending_ops.front().callback();
            pending_ops.pop();
        }
        pending_front_tick.store(pending_ops.empty() ? UINT64_MAX : pending_ops.front().gpu_tick,
                                 std::memory_order_release);
        return;
    }
    if (pending_front_tick.load(std::memory_order_acquire) >= CurrentTick()) {
        return;
    }
    std::unique_lock lk(pending_ops_mutex);
    if (pending_ops.empty()) {
        return;
    }
    if (!work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        work_semaphore.Refresh();
    }
    while (!pending_ops.empty() && work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        PendingOp op = std::move(pending_ops.front());
        pending_ops.pop();
        pending_front_tick.store(pending_ops.empty() ? UINT64_MAX : pending_ops.front().gpu_tick,
                                 std::memory_order_release);
        op.callback();
    }
}

void Scheduler::BeginSession() {
    EndSession();

    auto& session = sessions.emplace_back();

    if (recorder) {
        session.primary = recorder->NewFakeHandle();
        recorder->Run([this, fake = session.primary] { BeginRecorded(fake); });
    } else {
        const vk::CommandBufferBeginInfo begin_info = {
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
        };
        session.primary = command_pool.Commit();
        Check(session.primary.begin(begin_info));
    }

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

void Scheduler::EndSession() {
    if (sessions.empty()) {
        return;
    }

    if (on_session) {
        on_session();
    }

    const auto& session = sessions.back();
    if (session.upload) {
        Check(session.upload.end());
    }

    EndRendering();
    Check(session.primary.end());
}

// The queue submit of SubmitExecution, from the command processor or the recording thread.
static void QueueSubmit(const Instance& instance, const SubmitInfo& info,
                        std::span<const vk::CommandBuffer> cmd_buffers) {
    // One stage mask per wait semaphore; SubmitInfo can carry up to 4 waits (it used to provide
    // only 2 masks, so a third/fourth wait read past the array).
    static constexpr std::array<vk::PipelineStageFlags, 4> wait_stage_masks = {
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eAllCommands,
    };
    static_assert(wait_stage_masks.size() >= std::tuple_size_v<decltype(info.wait_semas)>);

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = static_cast<u32>(cmd_buffers.size()),
        .pCommandBuffers = cmd_buffers.data(),
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    std::scoped_lock lk{Scheduler::submit_mutex};
    ImGui::Core::TextureManager::Submit();
    auto submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
    if (submit_result == vk::Result::eErrorDeviceLost) {
        instance.ReportDeviceFault();
        UNREACHABLE_MSG("Device lost during submit");
    }
}

void Scheduler::SubmitExecution(SubmitInfo& info) {
    // Only the queue operations take submit_mutex (the sparse binds of on_submit take it on
    // their own): with the recorder, holding it while queueing commands could wait for the
    // recording thread, which needs it for its submits.
    const u64 signal_value = work_semaphore.NextTick();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, current_cmdbuf);
    }
#endif

    if (on_submit) {
        on_submit(info);
    }

    // sotc: the GPU writes this tick to host memory once everything before is visible there.
    if (work_semaphore.HasMarker() && !sessions.empty()) {
        EndRendering();
        work_semaphore.RecordMarker(sessions.back().primary, Semaphore::MarkerSlot::Timeline,
                                    static_cast<u32>(signal_value));
    }

    EndSession();

    boost::container::small_vector<vk::CommandBuffer, 8> cmd_buffers;
    for (const auto& session : sessions) {
        if (session.upload) {
            cmd_buffers.push_back(session.upload);
        }
        cmd_buffers.push_back(session.primary);
    }
    sessions.clear();

    const vk::Semaphore timeline = work_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    if (recorder) {
        // The submit runs on the recording thread after the commands of these command buffers.
        recorder->Run([this, info, fakes = std::move(cmd_buffers), signal_value] {
            boost::container::small_vector<vk::CommandBuffer, 8> real;
            for (const vk::CommandBuffer fake : fakes) {
                real.push_back(recorder->Real(fake));
            }
            QueueSubmit(instance, info, real);
            if (auto* diag = GpuTime(); diag && work_semaphore.HasMarker()) {
                diag->Note(&work_semaphore, signal_value);
            }
            recorded_submit_tick.store(signal_value, std::memory_order_release);
            recorded_submit_tick.notify_all();
            recorder->Retire(static_cast<u32>(fakes.size()));
        });
        recorder->Kick();
    } else {
        QueueSubmit(instance, info, cmd_buffers);
        if (auto* diag = GpuTime(); diag && work_semaphore.HasMarker()) {
            diag->Note(&work_semaphore, signal_value);
        }
    }

    work_semaphore.Refresh();
    BeginSession();

    // Apply pending operations
    PopPendingOperations();
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        work_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

void DynamicState::Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf) {
    if (dirty_state.viewports) {
        dirty_state.viewports = false;
        cmdbuf.setViewportWithCount(viewports);
    }
    if (dirty_state.scissors) {
        dirty_state.scissors = false;
        cmdbuf.setScissorWithCount(scissors);
    }
    if (dirty_state.depth_test_enabled) {
        dirty_state.depth_test_enabled = false;
        cmdbuf.setDepthTestEnable(depth_test_enabled);
    }
    if (dirty_state.depth_write_enabled) {
        dirty_state.depth_write_enabled = false;
        // Note that this must be set in a command buffer even if depth test is disabled.
        cmdbuf.setDepthWriteEnable(depth_write_enabled);
    }
    if (depth_test_enabled && dirty_state.depth_compare_op) {
        dirty_state.depth_compare_op = false;
        cmdbuf.setDepthCompareOp(depth_compare_op);
    }
    if (dirty_state.depth_bounds_test_enabled) {
        dirty_state.depth_bounds_test_enabled = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBoundsTestEnable(depth_bounds_test_enabled);
        }
    }
    if (depth_bounds_test_enabled && dirty_state.depth_bounds) {
        dirty_state.depth_bounds = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBounds(depth_bounds_min, depth_bounds_max);
        }
    }
    if (dirty_state.depth_bias_enabled) {
        dirty_state.depth_bias_enabled = false;
        cmdbuf.setDepthBiasEnable(depth_bias_enabled);
    }
    if (depth_bias_enabled && dirty_state.depth_bias) {
        dirty_state.depth_bias = false;
        cmdbuf.setDepthBias(depth_bias_constant, depth_bias_clamp, depth_bias_slope);
    }
    if (dirty_state.stencil_test_enabled) {
        dirty_state.stencil_test_enabled = false;
        cmdbuf.setStencilTestEnable(stencil_test_enabled);
    }
    if (stencil_test_enabled) {
        if (dirty_state.stencil_front_ops && dirty_state.stencil_back_ops &&
            stencil_front_ops == stencil_back_ops) {
            dirty_state.stencil_front_ops = false;
            dirty_state.stencil_back_ops = false;
            cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFrontAndBack, stencil_front_ops.fail_op,
                                stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                stencil_front_ops.compare_op);
        } else {
            if (dirty_state.stencil_front_ops) {
                dirty_state.stencil_front_ops = false;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFront, stencil_front_ops.fail_op,
                                    stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                    stencil_front_ops.compare_op);
            }
            if (dirty_state.stencil_back_ops) {
                dirty_state.stencil_back_ops = false;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eBack, stencil_back_ops.fail_op,
                                    stencil_back_ops.pass_op, stencil_back_ops.depth_fail_op,
                                    stencil_back_ops.compare_op);
            }
        }
        if (dirty_state.stencil_front_reference && dirty_state.stencil_back_reference &&
            stencil_front_reference == stencil_back_reference) {
            dirty_state.stencil_front_reference = false;
            dirty_state.stencil_back_reference = false;
            cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_reference);
        } else {
            if (dirty_state.stencil_front_reference) {
                dirty_state.stencil_front_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_reference);
            }
            if (dirty_state.stencil_back_reference) {
                dirty_state.stencil_back_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eBack, stencil_back_reference);
            }
        }
        if (dirty_state.stencil_front_write_mask && dirty_state.stencil_back_write_mask &&
            stencil_front_write_mask == stencil_back_write_mask) {
            dirty_state.stencil_front_write_mask = false;
            dirty_state.stencil_back_write_mask = false;
            cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_write_mask);
        } else {
            if (dirty_state.stencil_front_write_mask) {
                dirty_state.stencil_front_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_write_mask);
            }
            if (dirty_state.stencil_back_write_mask) {
                dirty_state.stencil_back_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eBack, stencil_back_write_mask);
            }
        }
        if (dirty_state.stencil_front_compare_mask && dirty_state.stencil_back_compare_mask &&
            stencil_front_compare_mask == stencil_back_compare_mask) {
            dirty_state.stencil_front_compare_mask = false;
            dirty_state.stencil_back_compare_mask = false;
            cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                         stencil_front_compare_mask);
        } else {
            if (dirty_state.stencil_front_compare_mask) {
                dirty_state.stencil_front_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFront,
                                             stencil_front_compare_mask);
            }
            if (dirty_state.stencil_back_compare_mask) {
                dirty_state.stencil_back_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eBack,
                                             stencil_back_compare_mask);
            }
        }
    }
    if (dirty_state.primitive_restart_enable) {
        dirty_state.primitive_restart_enable = false;
        cmdbuf.setPrimitiveRestartEnable(primitive_restart_enable);
    }
    if (dirty_state.rasterizer_discard_enable) {
        dirty_state.rasterizer_discard_enable = false;
        cmdbuf.setRasterizerDiscardEnable(rasterizer_discard_enable);
    }
    if (dirty_state.cull_mode) {
        dirty_state.cull_mode = false;
        cmdbuf.setCullMode(cull_mode);
    }
    if (dirty_state.front_face) {
        dirty_state.front_face = false;
        cmdbuf.setFrontFace(front_face);
    }
    if (dirty_state.blend_constants) {
        dirty_state.blend_constants = false;
        cmdbuf.setBlendConstants(blend_constants.data());
    }
    if (dirty_state.color_write_masks) {
        dirty_state.color_write_masks = false;
        if (instance.IsDynamicColorWriteMaskSupported()) {
            cmdbuf.setColorWriteMaskEXT(0, color_write_masks);
        }
    }
    if (dirty_state.line_width) {
        dirty_state.line_width = false;
        cmdbuf.setLineWidth(line_width);
    }
    if (dirty_state.feedback_loop_enabled && instance.IsAttachmentFeedbackLoopLayoutSupported()) {
        dirty_state.feedback_loop_enabled = false;
        cmdbuf.setAttachmentFeedbackLoopEnableEXT(feedback_loop_enabled
                                                      ? vk::ImageAspectFlagBits::eColor
                                                      : vk::ImageAspectFlagBits::eNone);
    }
}

} // namespace Vulkan
