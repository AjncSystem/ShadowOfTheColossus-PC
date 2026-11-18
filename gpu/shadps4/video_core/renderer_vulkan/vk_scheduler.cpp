// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <deque>
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
#include <span>
#include <boost/container/small_vector.hpp>
#include <immintrin.h>

namespace Vulkan {

std::mutex Scheduler::submit_mutex;

struct Scheduler::GpuTiming {
    static constexpr u32 SpanCount = 512;
    static constexpr u64 FrameStride = 16;

    struct Span {
        u64 frame;
        u64 tick;
        u32 query;
        const char* label;
    };

    vk::UniqueQueryPool pool;
    std::deque<Span> pending;
    u64 frame = 1;
    u32 next_slot = 0;
    u32 lost = 0;
    bool ending_frame = false;
};

void Scheduler::StartGpuTiming() {
    const char* flag = std::getenv("SOTC_GPU_TIMING");
    if (!flag || flag[0] != '1') {
        return;
    }
    const auto physical = instance.GetPhysicalDevice();
    const u32 bits = physical.getQueueFamilyProperties()[instance.GetGraphicsQueueFamilyIndex()]
                         .timestampValidBits;
    if (!bits) {
        std::fprintf(stderr, "SOTC GPU TIMING unsupported on graphics queue\n");
        return;
    }
    const vk::QueryPoolCreateInfo info{
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = GpuTiming::SpanCount * 2,
    };
    auto [result, pool] = instance.GetDevice().createQueryPoolUnique(info);
    if (result != vk::Result::eSuccess) {
        std::fprintf(stderr, "SOTC GPU TIMING query pool unavailable (%d)\n", int(result));
        return;
    }
    gpu_timing = std::make_unique<GpuTiming>();
    gpu_timing->pool = std::move(pool);
    std::fprintf(stderr, "SOTC GPU TIMING period_ns=%.9g bits=%u stride=%llu\n",
                 double(physical.getProperties().limits.timestampPeriod), bits,
                 static_cast<unsigned long long>(GpuTiming::FrameStride));
}

void Scheduler::MarkGpuFrameEnd() {
    if (gpu_timing) {
        gpu_timing->ending_frame = true;
    }
}

u32 Scheduler::BeginGpuSpan(vk::CommandBuffer cmdbuf, const char* label) {
    if (!gpu_timing || gpu_timing->frame % GpuTiming::FrameStride != 0) {
        return ~0u;
    }
    auto& timing = *gpu_timing;
    if (timing.pending.size() == GpuTiming::SpanCount) {
        ++timing.lost;
        return ~0u;
    }
    const u32 query = timing.next_slot * 2;
    timing.next_slot = (timing.next_slot + 1) % GpuTiming::SpanCount;
    timing.pending.push_back({timing.frame, CurrentTick(), query, label});
    WriteGpuTimestamp(cmdbuf, query, true);
    return query;
}

void Scheduler::EndGpuSpan(vk::CommandBuffer cmdbuf, u32 query) {
    if (query != ~0u) {
        WriteGpuTimestamp(cmdbuf, query, false);
    }
}

void Scheduler::WriteGpuTimestamp(vk::CommandBuffer cmdbuf, u32 query, bool begin) {
    auto record = [pool = *gpu_timing->pool, query, begin](vk::CommandBuffer real) {
        if (begin) {
            real.resetQueryPool(pool, query, 2);
            real.writeTimestamp2(vk::PipelineStageFlagBits2::eTopOfPipe, pool, query);
        } else {
            real.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe, pool, query + 1);
        }
    };
    if (recorder && CommandRecorder::IsFake(cmdbuf)) {
        recorder->Run([this, cmdbuf, record] { record(recorder->Real(cmdbuf)); });
    } else {
        record(cmdbuf);
    }
}

void Scheduler::CollectGpuTiming() {
    if (!gpu_timing) {
        return;
    }
    auto& timing = *gpu_timing;
    const u64 complete = work_semaphore.KnownGpuTick();
    while (!timing.pending.empty() && timing.pending.front().tick <= complete) {
        const auto span = timing.pending.front();
        std::array<u64, 2> values{};
        const auto result = instance.GetDevice().getQueryPoolResults(
            *timing.pool, span.query, 2, sizeof(values), values.data(), sizeof(u64),
            vk::QueryResultFlagBits::e64);
        if (result == vk::Result::eSuccess) {
            std::fprintf(stderr, "SOTC GPU SPAN %llu %llu %s %llu %llu\n",
                         static_cast<unsigned long long>(span.frame),
                         static_cast<unsigned long long>(span.tick), span.label,
                         static_cast<unsigned long long>(values[0]),
                         static_cast<unsigned long long>(values[1]));
        } else {
            std::fprintf(stderr, "SOTC GPU TIMING query %u failed (%d)\n", span.query,
                         int(result));
        }
        timing.pending.pop_front();
    }
}

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

void Scheduler::StartTickWatch() {
    if (!SotcStats::kTickWatch || tickwatch) {
        return;
    }
    tickwatch = true;
    tickwatch_thread = std::jthread([this](std::stop_token stoken) {
        Common::SetCurrentThreadName("shadPS4:SotcTickWatch");
        u64 last_gpu = ~0ull;
        u64 last_sub = ~0ull;
        while (!stoken.stop_requested()) {
            work_semaphore.Refresh();
            const u64 gpu = work_semaphore.KnownGpuTick();
            const u64 sub = work_semaphore.CurrentTick();
            if (gpu != last_gpu || sub != last_sub) {
                SotcStats::PushTick(0, gpu, sub);
                last_gpu = gpu;
                last_sub = sub;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });
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

void Scheduler::RunAfterSubmit(Common::UniqueFunction<void>&& func) {
    if (!recorder) {
        func();
        return;
    }
    recorder->Run(std::move(func));
    recorder->Kick();
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
    const SotcStats::SplitTimer submit_split{SotcStats::SplitWaitSubmit};
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

void Scheduler::BeginRendering(const RenderState& new_state, u64 token) {
    if (is_rendering) {
        if (token != 0 && token == render_state_token) {
            return;
        }
        if (render_state == new_state) {
            render_state_token = token;
            return;
        }
    }
    EndRendering();
    is_rendering = true;
    render_state = new_state;
    render_state_token = token;

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
}

vk::CommandBuffer Scheduler::UploadCommandBuffer() {
    auto& upload_cmdbuf = sessions.back().upload;
    if (upload_cmdbuf) {
        return upload_cmdbuf;
    }
    if (recorder) {
        upload_cmdbuf = recorder->NewFakeHandle();
        recorder->Run([this, fake = upload_cmdbuf] { BeginRecorded(fake); });
        sessions.back().upload_query = BeginGpuSpan(upload_cmdbuf, "upload");
        return upload_cmdbuf;
    }
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };
    upload_cmdbuf = command_pool.Commit();
    Check(upload_cmdbuf.begin(begin_info));
    sessions.back().upload_query = BeginGpuSpan(upload_cmdbuf, "upload");
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
    ahead_query = BeginGpuSpan(ahead_cmdbuf, "readback");
    return ahead_cmdbuf;
}

void Scheduler::SubmitAheadAndWait() {
    if (work_semaphore.HasMarker()) {
        work_semaphore.RecordMarker(ahead_cmdbuf, Semaphore::MarkerSlot::Ahead,
                                    static_cast<u32>(++ahead_sequence));
    }
    EndGpuSpan(ahead_cmdbuf, ahead_query);
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
        const s64 tw0 = SotcStats::kTickWatch ? static_cast<s64>(SotcStats::NowNs()) : 0;
        std::scoped_lock lk{submit_mutex};
        const s64 tw1 = SotcStats::kTickWatch ? static_cast<s64>(SotcStats::NowNs()) : 0;
        const auto submit_result = instance.GetGraphicsQueue().submit(submit_info, *ahead_fence);
        if (submit_result == vk::Result::eErrorDeviceLost) {
            instance.ReportDeviceFault();
            UNREACHABLE_MSG("Device lost during ahead submit");
        }
        ASSERT_MSG(submit_result == vk::Result::eSuccess, "Ahead submit failed: {}",
                   vk::to_string(submit_result));
        if (SotcStats::kTickWatch) {
            const s64 tw2 = static_cast<s64>(SotcStats::NowNs());
            SotcStats::PushTick(3, 0,
                                (static_cast<u64>((tw1 - tw0) / 1000) << 32) |
                                    static_cast<u64>((tw2 - tw1) / 1000));
        }
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

    session.primary_query = BeginGpuSpan(session.primary, "graphics");
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
        EndGpuSpan(session.upload, session.upload_query);
        Check(session.upload.end());
    }

    EndRendering();
    EndGpuSpan(session.primary, session.primary_query);
    Check(session.primary.end());
}

// The queue submit of SubmitExecution, from the command processor or the recording thread.
// Returns (submit_mutex wait us << 32) | vkQueueSubmit us when SOTC_TICKWATCH is on, else 0.
static u64 QueueSubmit(const Instance& instance, const SubmitInfo& info,
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

    const s64 t0 = SotcStats::kTickWatch ? static_cast<s64>(SotcStats::NowNs()) : 0;
    std::scoped_lock lk{Scheduler::submit_mutex};
    const s64 t1 = SotcStats::kTickWatch ? static_cast<s64>(SotcStats::NowNs()) : 0;
    ImGui::Core::TextureManager::Submit();
    auto submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
    if (submit_result == vk::Result::eErrorDeviceLost) {
        instance.ReportDeviceFault();
        UNREACHABLE_MSG("Device lost during submit");
    }
    if (!SotcStats::kTickWatch) {
        return 0;
    }
    const s64 t2 = static_cast<s64>(SotcStats::NowNs());
    return (static_cast<u64>((t1 - t0) / 1000) << 32) | static_cast<u64>((t2 - t1) / 1000);
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

    if (tickwatch) {
        SotcStats::PushTick(1, signal_value,
                            (static_cast<u64>(info.num_wait_semas) << 8) | cmd_buffers.size());
    }

    if (recorder) {
        // The submit runs on the recording thread after the commands of these command buffers.
        recorder->Run([this, info, fakes = std::move(cmd_buffers), signal_value] {
            boost::container::small_vector<vk::CommandBuffer, 8> real;
            for (const vk::CommandBuffer fake : fakes) {
                real.push_back(recorder->Real(fake));
            }
            const u64 queue_us = QueueSubmit(instance, info, real);
            if (tickwatch) {
                SotcStats::PushTick(2, signal_value, queue_us);
            }
            recorded_submit_tick.store(signal_value, std::memory_order_release);
            recorded_submit_tick.notify_all();
            recorder->Retire(static_cast<u32>(fakes.size()));
        });
        recorder->Kick();
    } else {
        const u64 queue_us = QueueSubmit(instance, info, cmd_buffers);
        if (tickwatch) {
            SotcStats::PushTick(2, signal_value, queue_us);
        }
    }

    work_semaphore.Refresh();
    CollectGpuTiming();
    if (gpu_timing && std::exchange(gpu_timing->ending_frame, false)) {
        auto& timing = *gpu_timing;
        if (timing.frame % GpuTiming::FrameStride == 0) {
            std::fprintf(stderr, "SOTC GPU FRAME %llu %llu %llu %u\n",
                         static_cast<unsigned long long>(timing.frame),
                         static_cast<unsigned long long>(signal_value),
                         static_cast<unsigned long long>(SotcStats::NowNs()), timing.lost);
            timing.lost = 0;
        }
        ++timing.frame;
    }
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

u64 Scheduler::EnqueueAsyncAhead(std::vector<AsyncAheadCopy> copies) {
    const u64 seq = async_ahead_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    if (recorder) {
        recorder->Run(
            [this, seq, copies = std::move(copies)] { ExecuteAsyncAhead(seq, copies); });
        recorder->Kick();
    } else {
        ExecuteAsyncAhead(seq, copies);
    }
    return seq;
}

void Scheduler::ExecuteAsyncAhead(u64 seq, const std::vector<AsyncAheadCopy>& copies) {
    const vk::Device device = instance.GetDevice();
    if (!async_ahead_pool) {
        const vk::CommandPoolCreateInfo pool_info = {
            .flags = vk::CommandPoolCreateFlagBits::eTransient |
                     vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
        };
        auto [pool_res, pool] = device.createCommandPoolUnique(pool_info);
        ASSERT_MSG(pool_res == vk::Result::eSuccess, "Failed to create async ahead pool: {}",
                   vk::to_string(pool_res));
        async_ahead_pool = std::move(pool);
        const vk::CommandBufferAllocateInfo alloc_info = {
            .commandPool = *async_ahead_pool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = NumAsyncAheadCmds,
        };
        auto [alloc_res, cmdbufs] = device.allocateCommandBuffers(alloc_info);
        ASSERT_MSG(alloc_res == vk::Result::eSuccess,
                   "Failed to allocate async ahead command buffers: {}", vk::to_string(alloc_res));
        std::copy_n(cmdbufs.begin(), NumAsyncAheadCmds, async_ahead_cmdbufs.begin());
        const vk::StructureChain sem_chain = {
            vk::SemaphoreCreateInfo{},
            vk::SemaphoreTypeCreateInfo{
                .semaphoreType = vk::SemaphoreType::eTimeline,
                .initialValue = 0,
            },
        };
        auto [sem_res, sem] = device.createSemaphoreUnique(sem_chain.get());
        ASSERT_MSG(sem_res == vk::Result::eSuccess,
                   "Failed to create async ahead semaphore: {}", vk::to_string(sem_res));
        async_ahead_sem = std::move(sem);
    }
    const u32 slot = static_cast<u32>((seq - 1) & (NumAsyncAheadCmds - 1));
    if (const u64 prev_seq = async_ahead_cmdbuf_seqs[slot]; prev_seq != 0) {
        const auto [cnt_res, cnt] = device.getSemaphoreCounterValue(*async_ahead_sem);
        if (cnt_res != vk::Result::eSuccess || cnt < prev_seq) {
            const vk::SemaphoreWaitInfo wait_info = {
                .semaphoreCount = 1,
                .pSemaphores = &*async_ahead_sem,
                .pValues = &prev_seq,
            };
            Check(device.waitSemaphores(wait_info, std::numeric_limits<u64>::max()));
        }
    }
    const vk::CommandBuffer cmdbuf = async_ahead_cmdbufs[slot];
    Check(cmdbuf.reset());
    Check(cmdbuf.begin(vk::CommandBufferBeginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    }));
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
    for (const auto& c : copies) {
        cmdbuf.copyBuffer(c.src, c.dst, c.regions);
    }
    const vk::MemoryBarrier2 post_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &post_barrier,
    });
    Check(cmdbuf.end());
    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues = &seq,
    };
    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmdbuf,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &*async_ahead_sem,
    };
    {
        std::scoped_lock lk{submit_mutex};
        const auto submit_result = instance.GetGraphicsQueue().submit(submit_info);
        if (submit_result == vk::Result::eErrorDeviceLost) {
            instance.ReportDeviceFault();
            UNREACHABLE_MSG("Device lost during async ahead submit");
        }
        ASSERT_MSG(submit_result == vk::Result::eSuccess, "Async ahead submit failed: {}",
                   vk::to_string(submit_result));
    }
    async_ahead_cmdbuf_seqs[slot] = seq;
    async_ahead_submitted.store(seq, std::memory_order_release);
    async_ahead_submitted.notify_all();
}

void Scheduler::WaitAsyncAhead(u64 seq) {
    u64 sub = async_ahead_submitted.load(std::memory_order_acquire);
    while (sub < seq) {
        async_ahead_submitted.wait(sub, std::memory_order_acquire);
        sub = async_ahead_submitted.load(std::memory_order_acquire);
    }
    const vk::Device device = instance.GetDevice();
    const auto [cnt_res, cnt] = device.getSemaphoreCounterValue(*async_ahead_sem);
    if (cnt_res == vk::Result::eSuccess && cnt >= seq) {
        return;
    }
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &*async_ahead_sem,
        .pValues = &seq,
    };
    const vk::Result wait_result =
        device.waitSemaphores(wait_info, std::numeric_limits<u64>::max());
    if (wait_result == vk::Result::eErrorDeviceLost) {
        instance.ReportDeviceFault();
        UNREACHABLE_MSG("Device lost during async ahead wait");
    }
    ASSERT_MSG(wait_result == vk::Result::eSuccess, "Async ahead wait failed: {}",
               vk::to_string(wait_result));
}

} // namespace Vulkan
