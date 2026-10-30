// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>
#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/buffer_cache/async_upload.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_recorder.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_stats.h"
#include "sotc_gpu_trace.h"
#include <chrono>
#include <span>
#include <boost/container/small_vector.hpp>
#include <immintrin.h>

namespace Vulkan {

std::mutex Scheduler::submit_mutex;

// sotc: SOTC_FPS (B5). Takes submit_mutex; with SOTC_FPS on, the wait is timed and counted (see
// Scheduler::submit_lock_draw_ns). With SOTC_FPS off it is the plain lock.
static void LockSubmit(std::unique_lock<std::mutex>& lk) {
    if (!SotcStats::kFpsLog) {
        lk.lock();
        return;
    }
    const uint64_t start = SotcStats::NowNs();
    lk.lock();
    const uint64_t waited = SotcStats::NowNs() - start;
    if (SotcStats::t_draw_thread) {
        Scheduler::submit_lock_draw_ns.fetch_add(waited, std::memory_order_relaxed);
        Scheduler::submit_lock_draw_count.fetch_add(1, std::memory_order_relaxed);
    } else {
        Scheduler::submit_lock_other_ns.fetch_add(waited, std::memory_order_relaxed);
        Scheduler::submit_lock_other_count.fetch_add(1, std::memory_order_relaxed);
    }
}

void Scheduler::SetHostVisibility(bool on) {
    host_visibility.store(on, std::memory_order_release);
}

// sotc: SOTC_HRR=1 (see Scheduler::SetHostVisibility). Called right before each command buffer
// ends, outside any render pass. A global memory barrier: every write of the command buffer, and
// of the ones before it on the queue, is made available to host reads.
static void RecordHostVisibility(vk::CommandBuffer cmdbuf) {
    if (!Scheduler::host_visibility.load(std::memory_order_acquire)) {
        return;
    }
    const vk::MemoryBarrier2 host_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &host_barrier,
    });
}

Scheduler::Scheduler(const Instance& instance, bool record_thread)
    : instance{instance}, work_semaphore{instance}, command_pool{instance, &work_semaphore} {
    if (record_thread) {
        recorder = std::make_unique<CommandRecorder>(instance, &work_semaphore);
        LOG_INFO(Render_Vulkan, "Vulkan commands are recorded on their own thread");
    }
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    if (instance.HasXferQueue()) {
        xfer = std::make_unique<XferQueue>(instance, work_semaphore, this);
    }
    // sotc: SOTC_TRACE_GPU: GPU timestamps of the draw scheduler's command buffers. Only with the
    // record thread, as the watch thread that reads them is started only then.
    if (SotcGpuTrace::kOn && record_thread) {
        gpu_trace = SotcGpuTrace::Timeline::Create(instance);
    }
    BeginSession();
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
    // sotc: SOTC_TRACE: only the draw scheduler (the one with its own recording thread) is
    // watched; the present and flip schedulers do little GPU work.
    if (SotcTrace::kOn && record_thread) {
        trace_queue = SotcTrace::RegisterQueue("GPU: draw scheduler queue");
        trace_watch_thread = std::jthread(std::bind_front(&Scheduler::TraceWatchThread, this));
    }
}

void Scheduler::TraceWatchThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:SotcTraceGpuWatch");
    const auto device = instance.GetDevice();
    const vk::Semaphore semaphore = work_semaphore.Handle();
    u64 done = 0;
    if (const auto [result, value] = device.getSemaphoreCounterValue(semaphore);
        result == vk::Result::eSuccess) {
        done = value;
    }
    // Polls the counter instead of vkWaitSemaphores: on NVIDIA, ~500 timed-out waits per second
    // made every vkQueueSubmit slower and slower (1 -> 74 ms per frame over a minute). The
    // completion time is therefore known to the sleep granularity (about 0.5-1 ms).
    while (!stoken.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        if (gpu_trace) { // sotc: SOTC_TRACE_GPU calibration, about once a second
            gpu_trace->Poll(SotcTrace::Now());
        }
        const auto [result, value] = device.getSemaphoreCounterValue(semaphore);
        if (result != vk::Result::eSuccess) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2)); // device lost etc.
            continue;
        }
        if (value <= done) {
            continue;
        }
        const u64 first = value - done > 4096 ? value - 4095 : done + 1;
        SotcTrace::Completed(trace_queue, first, value, SotcStats::NowNs());
        if (gpu_trace) { // sotc: SOTC_TRACE_GPU, the timestamps of the ticks that completed
            gpu_trace->Completed(first, value);
        }
        done = value;
    }
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
    recorder->Run(CommandRecorder::BlockCategory::Other, [this, cmdbuf, func = std::move(func)] {
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
    const SotcStats::DrawWait wait{SotcStats::WaitRecorder}; // sotc: SOTC_FPS accounting
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

    if (recorder) {
        // sotc: the secondary command buffer of this pass (SOTC_PASS_SEC) inherits these formats
        PassFormats formats{};
        formats.valid = true;
        formats.num_colors = render_state.num_color_attachments;
        for (u32 i = 0; i < formats.num_colors; ++i) {
            formats.colors[i] = static_cast<VkFormat>(render_state.color_formats[i]);
        }
        formats.depth = static_cast<VkFormat>(render_state.depth_format);
        formats.stencil = static_cast<VkFormat>(render_state.stencil_format);
        formats.samples = static_cast<VkSampleCountFlagBits>(render_state.samples);
        recorder->SetPassFormats(formats);
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
}

vk::CommandBuffer Scheduler::UploadCommandBuffer() {
    auto& upload_cmdbuf = sessions.back().upload;
    if (upload_cmdbuf) {
        return upload_cmdbuf;
    }
    if (recorder) {
        upload_cmdbuf = recorder->NewFakeHandle();
        recorder->Run(CommandRecorder::BlockCategory::Other,
                      [this, fake = upload_cmdbuf] { BeginRecorded(fake); });
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
            const SotcStats::DrawWait wait{SotcStats::WaitGpu}; // sotc: SOTC_FPS accounting
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
    RecordHostVisibility(ahead_cmdbuf);
    Check(ahead_cmdbuf.end());
    // With the recorder, the earlier submits must have reached the queue first.
    WaitSubmitted(CurrentTick() - 1);
    // sotc: SOTC_FPS accounting: from here on the draw thread waits for the GPU (the submit
    // itself is short next to the wait for the work queued before the copy).
    const SotcStats::DrawWait gpu_wait{SotcStats::WaitGpu};
    // sotc: the readback may read what the transfer queue wrote; the queue does not order them.
    if (xfer) {
        xfer->WaitLastCopy();
    }
    // Queue submission order and the barriers recorded in the command buffer order it after
    // all work submitted before; the command buffer being recorded is not involved.
    const vk::SubmitInfo submit_info = {
        .commandBufferCount = 1U,
        .pCommandBuffers = &ahead_cmdbuf,
    };
    {
        std::unique_lock lk{submit_mutex, std::defer_lock};
        LockSubmit(lk);
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

vk::CommandBuffer Scheduler::BeginAheadAsync() {
    const vk::Device device = instance.GetDevice();
    if (!ahead_timeline) {
        const vk::StructureChain semaphore_chain = {
            vk::SemaphoreCreateInfo{},
            vk::SemaphoreTypeCreateInfo{
                .semaphoreType = vk::SemaphoreType::eTimeline,
                .initialValue = 0,
            },
        };
        auto [sem_result, sem] = device.createSemaphoreUnique(semaphore_chain.get());
        ASSERT_MSG(sem_result == vk::Result::eSuccess, "Failed to create the ahead timeline: {}",
                   vk::to_string(sem_result));
        ahead_timeline = std::move(sem);
        for (AheadSlot& slot : ahead_slots) {
            const vk::CommandPoolCreateInfo pool_info = {
                .flags = vk::CommandPoolCreateFlagBits::eTransient,
                .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
            };
            auto [pool_result, pool] = device.createCommandPoolUnique(pool_info);
            ASSERT_MSG(pool_result == vk::Result::eSuccess,
                       "Failed to create an ahead slot pool: {}", vk::to_string(pool_result));
            slot.pool = std::move(pool);
            const vk::CommandBufferAllocateInfo alloc_info = {
                .commandPool = *slot.pool,
                .level = vk::CommandBufferLevel::ePrimary,
                .commandBufferCount = 1,
            };
            auto [alloc_result, cmdbufs] = device.allocateCommandBuffers(alloc_info);
            ASSERT_MSG(alloc_result == vk::Result::eSuccess,
                       "Failed to allocate an ahead slot command buffer: {}",
                       vk::to_string(alloc_result));
            slot.cmdbuf = cmdbufs[0];
        }
    }
    AheadSlot& slot = ahead_slots[ahead_slot_next];
    // Its previous copy completed means its submit ran too (the value is signalled by it), so the
    // recording thread no longer touches the command buffer.
    if (slot.value != 0 && !IsAheadDone(slot.value)) {
        return {};
    }
    ahead_slot_next = (ahead_slot_next + 1) % NumAheadSlots;
    Check(device.resetCommandPool(*slot.pool));
    Check(slot.cmdbuf.begin(vk::CommandBufferBeginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    }));
    ahead_slot_open = &slot;
    return slot.cmdbuf;
}

u64 Scheduler::SubmitAheadAsync() {
    AheadSlot& slot = *std::exchange(ahead_slot_open, nullptr);
    const vk::CommandBuffer cmdbuf = slot.cmdbuf;
    RecordHostVisibility(cmdbuf);
    Check(cmdbuf.end());
    // Values are handed out in the order of the submits: both happen in producer order (the
    // recording thread runs its stream in order).
    const u64 value = ++ahead_value_last;
    slot.value = value;
    // Transfer-queue copies (uploads into arenas) are not ordered by the graphics queue: wait on
    // the GPU for the newest one. Its submit is on the transfer queue already (XferQueue::Copy
    // submits at once, and waits only for graphics ticks already queued), so this is no wait
    // before signal.
    const u64 xfer_wait = xfer ? xfer->LastTick() : 0;
    const vk::Semaphore xfer_sem = xfer ? xfer->Handle() : vk::Semaphore{};
    const vk::Semaphore signal_sem = *ahead_timeline;
    auto submit = [this, cmdbuf, value, xfer_wait, xfer_sem, signal_sem] {
        const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
        const vk::TimelineSemaphoreSubmitInfo timeline_si = {
            .waitSemaphoreValueCount = xfer_wait != 0 ? 1u : 0u,
            .pWaitSemaphoreValues = &xfer_wait,
            .signalSemaphoreValueCount = 1u,
            .pSignalSemaphoreValues = &value,
        };
        const vk::SubmitInfo submit_info = {
            .pNext = &timeline_si,
            .waitSemaphoreCount = xfer_wait != 0 ? 1u : 0u,
            .pWaitSemaphores = &xfer_sem,
            .pWaitDstStageMask = &wait_stage,
            .commandBufferCount = 1u,
            .pCommandBuffers = &cmdbuf,
            .signalSemaphoreCount = 1u,
            .pSignalSemaphores = &signal_sem,
        };
        {
            std::unique_lock lk{submit_mutex, std::defer_lock};
            LockSubmit(lk);
            const auto submit_result = instance.GetGraphicsQueue().submit(submit_info, vk::Fence{});
            if (submit_result == vk::Result::eErrorDeviceLost) {
                instance.ReportDeviceFault();
                UNREACHABLE_MSG("Device lost during an async ahead submit");
            }
            ASSERT_MSG(submit_result == vk::Result::eSuccess, "Async ahead submit failed: {}",
                       vk::to_string(submit_result));
        }
        ahead_value_submitted.store(value, std::memory_order_release);
        ahead_value_submitted.notify_all();
    };
    if (recorder) {
        recorder->Run(CommandRecorder::BlockCategory::Other, std::move(submit));
        recorder->Kick();
    } else {
        submit();
    }
    return value;
}

bool Scheduler::IsAheadDone(u64 value) {
    if (ahead_value_done.load(std::memory_order_acquire) >= value) {
        return true;
    }
    const auto [result, counter] = instance.GetDevice().getSemaphoreCounterValue(*ahead_timeline);
    if (result == vk::Result::eErrorDeviceLost) {
        instance.ReportDeviceFault();
        UNREACHABLE_MSG("Device lost while reading the ahead timeline");
    }
    u64 seen = ahead_value_done.load(std::memory_order_relaxed);
    while (seen < counter &&
           !ahead_value_done.compare_exchange_weak(seen, counter, std::memory_order_acq_rel)) {
    }
    return counter >= value;
}

void Scheduler::WaitAheadSubmitted(u64 value) {
    u64 submitted = ahead_value_submitted.load(std::memory_order_acquire);
    for (u32 spins = 0; spins < 4096 && submitted < value; ++spins) {
        _mm_pause();
        submitted = ahead_value_submitted.load(std::memory_order_acquire);
    }
    while (submitted < value) {
        ahead_value_submitted.wait(submitted, std::memory_order_acquire);
        submitted = ahead_value_submitted.load(std::memory_order_acquire);
    }
}

void Scheduler::WaitAhead(u64 value) {
    WaitAheadSubmitted(value);
    if (IsAheadDone(value)) {
        return;
    }
    const vk::Semaphore sem = *ahead_timeline;
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &sem,
        .pValues = &value,
    };
    const vk::Result wait_result =
        instance.GetDevice().waitSemaphores(wait_info, std::numeric_limits<u64>::max());
    if (wait_result == vk::Result::eErrorDeviceLost) {
        instance.ReportDeviceFault();
        UNREACHABLE_MSG("Device lost during an async ahead wait");
    }
    ASSERT_MSG(wait_result == vk::Result::eSuccess, "Async ahead wait failed: {}",
               vk::to_string(wait_result));
    u64 seen = ahead_value_done.load(std::memory_order_relaxed);
    while (seen < value &&
           !ahead_value_done.compare_exchange_weak(seen, value, std::memory_order_acq_rel)) {
    }
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
    ++session_serial;

    auto& session = sessions.emplace_back();

    if (recorder) {
        session.primary = recorder->NewFakeHandle();
        recorder->Run(CommandRecorder::BlockCategory::Other,
                      [this, fake = session.primary] { BeginRecorded(fake); });
    } else {
        const vk::CommandBufferBeginInfo begin_info = {
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
        };
        session.primary = command_pool.Commit();
        Check(session.primary.begin(begin_info));
    }

    // sotc: SOTC_TRACE_GPU: reset and begin stamp of this command buffer (first in the session).
    if (gpu_trace) {
        session.gpu_pair = gpu_trace->Begin(session.primary);
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
        RecordHostVisibility(session.upload);
        Check(session.upload.end());
    }

    EndRendering();
    if (gpu_trace) { // sotc: SOTC_TRACE_GPU, end stamp (outside the render pass)
        gpu_trace->End(session.primary, session.gpu_pair);
    }
    RecordHostVisibility(session.primary);
    Check(session.primary.end());
}

XferQueue::XferQueue(const Instance& instance_, Semaphore& graphics_, Scheduler* scheduler_)
    : instance{instance_}, graphics{&graphics_}, scheduler{scheduler_} {
    const vk::Device device = instance.GetDevice();
    queue = instance.GetXferQueue();

    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [sem_result, sem] = device.createSemaphoreUnique(semaphore_chain.get());
    ASSERT_MSG(sem_result == vk::Result::eSuccess, "Failed to create the transfer semaphore: {}",
               vk::to_string(sem_result));
    timeline = std::move(sem);

    const vk::CommandPoolCreateInfo pool_info = {
        .flags = vk::CommandPoolCreateFlagBits::eTransient |
                 vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = instance.GetXferQueueFamilyIndex(),
    };
    auto [pool_result, pool_handle] = device.createCommandPoolUnique(pool_info);
    ASSERT_MSG(pool_result == vk::Result::eSuccess, "Failed to create the transfer pool: {}",
               vk::to_string(pool_result));
    pool = std::move(pool_handle);

    const vk::CommandBufferAllocateInfo alloc_info = {
        .commandPool = *pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = NumSlots,
    };
    auto [alloc_result, cmdbufs] = device.allocateCommandBuffers(alloc_info);
    ASSERT_MSG(alloc_result == vk::Result::eSuccess,
               "Failed to allocate the transfer command buffers: {}",
               vk::to_string(alloc_result));
    for (u32 i = 0; i < NumSlots; ++i) {
        slots[i].cmd = cmdbufs[i];
    }

    // sotc: SOTC_RB_TICK: readback copies get a pool and a slot ring of their own.
    upload_routing = instance.IsXferRoutingEnabled();
    rb_enabled = instance.IsRbTickEnabled();
    if (rb_enabled) {
        auto [rb_pool_result, rb_pool_handle] = device.createCommandPoolUnique(pool_info);
        ASSERT_MSG(rb_pool_result == vk::Result::eSuccess,
                   "Failed to create the transfer readback pool: {}",
                   vk::to_string(rb_pool_result));
        rb_pool = std::move(rb_pool_handle);
        const vk::CommandBufferAllocateInfo rb_alloc_info = {
            .commandPool = *rb_pool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = NumRbSlots,
        };
        auto [rb_alloc_result, rb_cmdbufs] = device.allocateCommandBuffers(rb_alloc_info);
        ASSERT_MSG(rb_alloc_result == vk::Result::eSuccess,
                   "Failed to allocate the transfer readback command buffers: {}",
                   vk::to_string(rb_alloc_result));
        for (u32 i = 0; i < NumRbSlots; ++i) {
            rb_slots[i].cmd = rb_cmdbufs[i];
        }
    }
}

XferQueue::~XferQueue() {
    // Nothing of the copies may still run when the pool and the semaphore are destroyed.
    (void)queue.waitIdle();
}

bool XferQueue::Copy(vk::Buffer src, vk::Buffer dst, std::span<const vk::BufferCopy> copies) {
    // Newest graphics submit (the tick being recorded is CurrentTick(); ticks start at 1).
    const u64 graphics_tick = graphics->CurrentTick() - 1;
    // sotc: with the recording thread, the submit of graphics_tick may still be queued there; it
    // collects the pending copy (TakePendingTick) when it is built, so it would wait for this copy
    // while this copy waits for it: a GPU deadlock (the game froze at the logos with
    // SOTC_BINDLESS_SAMP=1, 2026-10-27). Wait until that submit is on the queue first, outside the
    // mutex the recording thread takes to collect the pending tick.
    if (graphics_tick != 0 && scheduler) {
        scheduler->WaitSubmitted(graphics_tick);
    }
    const std::scoped_lock lk{mutex};
    const vk::Device device = instance.GetDevice();

    Slot& slot = slots[next_slot];
    next_slot = (next_slot + 1) % NumSlots;
    if (slot.tick != 0) {
        // A command buffer is reused only after the copy recorded in it has finished.
        const vk::Semaphore wait_sem = *timeline;
        const vk::SemaphoreWaitInfo wait_info = {
            .semaphoreCount = 1,
            .pSemaphores = &wait_sem,
            .pValues = &slot.tick,
        };
        const vk::Result wait_result =
            device.waitSemaphores(wait_info, std::numeric_limits<u64>::max());
        ASSERT_MSG(wait_result == vk::Result::eSuccess, "Transfer slot wait failed: {}",
                   vk::to_string(wait_result));
    }

    const u64 tick = next_tick++;
    Check(slot.cmd.begin(vk::CommandBufferBeginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    }));
    slot.cmd.copyBuffer(src, dst, copies);
    RecordHostVisibility(slot.cmd);
    Check(slot.cmd.end());

    const vk::Semaphore xfer_sem = *timeline;
    const vk::Semaphore graphics_sem = graphics->Handle();
    const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eAllCommands;
    // sotc: SOTC_RB_TICK with uploads routed too: this copy may write bytes an earlier readback
    // copy (same queue) reads, and two submits of one queue are not ordered against each other's
    // memory accesses; so it also waits for the newest readback copy (write after read). The
    // value was signalled by an earlier submit of this queue, so the wait cannot dangle.
    std::array<vk::Semaphore, 2> wait_sems{};
    std::array<u64, 2> wait_values{};
    std::array<vk::PipelineStageFlags, 2> wait_stages{wait_stage, wait_stage};
    u32 num_waits = 0;
    if (graphics_tick != 0) {
        wait_sems[num_waits] = graphics_sem;
        wait_values[num_waits++] = graphics_tick;
    }
    if (rb_enabled) {
        if (const u64 rb_tick = rb_last_tick.load(std::memory_order_acquire); rb_tick != 0) {
            wait_sems[num_waits] = xfer_sem;
            wait_values[num_waits++] = rb_tick;
        }
    }
    const vk::TimelineSemaphoreSubmitInfo timeline_info = {
        .waitSemaphoreValueCount = num_waits,
        .pWaitSemaphoreValues = wait_values.data(),
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &tick,
    };
    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_info,
        .waitSemaphoreCount = num_waits,
        .pWaitSemaphores = wait_sems.data(),
        .pWaitDstStageMask = wait_stages.data(),
        .commandBufferCount = 1u,
        .pCommandBuffers = &slot.cmd,
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &xfer_sem,
    };
    const vk::Result submit_result = queue.submit(submit_info, vk::Fence{});
    if (submit_result == vk::Result::eErrorDeviceLost) {
        instance.ReportDeviceFault();
        UNREACHABLE_MSG("Device lost during transfer submit");
    }
    ASSERT_MSG(submit_result == vk::Result::eSuccess, "Transfer submit failed: {}",
               vk::to_string(submit_result));

    slot.tick = tick;
    last_tick = tick;
    pending_tick = tick;
    return true;
}

void XferQueue::CopyAndWait(std::span<const Region> regions) {
    const vk::Device device = instance.GetDevice();
    const vk::Semaphore xfer_sem = *timeline;
    u64 tick;
    {
        const std::scoped_lock lk{mutex};
        Slot& slot = slots[next_slot];
        next_slot = (next_slot + 1) % NumSlots;
        if (slot.tick != 0) {
            // A command buffer is reused only after the copy recorded in it has finished.
            const vk::SemaphoreWaitInfo wait_info = {
                .semaphoreCount = 1,
                .pSemaphores = &xfer_sem,
                .pValues = &slot.tick,
            };
            const vk::Result wait_result =
                device.waitSemaphores(wait_info, std::numeric_limits<u64>::max());
            ASSERT_MSG(wait_result == vk::Result::eSuccess, "Transfer slot wait failed: {}",
                       vk::to_string(wait_result));
        }
        tick = next_tick++;
        Check(slot.cmd.begin(vk::CommandBufferBeginInfo{
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
        }));
        for (const auto& region : regions) {
            slot.cmd.copyBuffer(region.src, region.dst, region.copy);
        }
        RecordHostVisibility(slot.cmd);
        Check(slot.cmd.end());
        const vk::TimelineSemaphoreSubmitInfo timeline_info = {
            .signalSemaphoreValueCount = 1u,
            .pSignalSemaphoreValues = &tick,
        };
        const vk::SubmitInfo submit_info = {
            .pNext = &timeline_info,
            .commandBufferCount = 1u,
            .pCommandBuffers = &slot.cmd,
            .signalSemaphoreCount = 1u,
            .pSignalSemaphores = &xfer_sem,
        };
        const vk::Result submit_result = queue.submit(submit_info, vk::Fence{});
        if (submit_result == vk::Result::eErrorDeviceLost) {
            instance.ReportDeviceFault();
            UNREACHABLE_MSG("Device lost during transfer submit");
        }
        ASSERT_MSG(submit_result == vk::Result::eSuccess, "Transfer submit failed: {}",
                   vk::to_string(submit_result));
        slot.tick = tick;
        last_tick = tick;
        pending_tick = tick;
    }
    // Outside the mutex: the recording thread takes it to collect the pending tick.
    const SotcStats::DrawWait wait{SotcStats::WaitUpload}; // sotc: SOTC_FPS accounting
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &xfer_sem,
        .pValues = &tick,
    };
    const vk::Result wait_result = device.waitSemaphores(wait_info, std::numeric_limits<u64>::max());
    if (wait_result == vk::Result::eErrorDeviceLost) {
        instance.ReportDeviceFault();
        UNREACHABLE_MSG("Device lost during a transfer wait");
    }
    ASSERT_MSG(wait_result == vk::Result::eSuccess, "Transfer wait failed: {}",
               vk::to_string(wait_result));
}

u64 XferQueue::TakePendingTick() {
    const std::scoped_lock lk{mutex};
    return std::exchange(pending_tick, u64{0});
}

void XferQueue::WaitLastCopy() {
    const std::scoped_lock lk{mutex};
    if (last_tick == 0) {
        return;
    }
    const SotcStats::DrawWait wait{SotcStats::WaitGpu}; // sotc: SOTC_FPS accounting
    const vk::Semaphore wait_sem = *timeline;
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &wait_sem,
        .pValues = &last_tick,
    };
    const vk::Result wait_result =
        instance.GetDevice().waitSemaphores(wait_info, std::numeric_limits<u64>::max());
    ASSERT_MSG(wait_result == vk::Result::eSuccess, "Transfer wait failed: {}",
               vk::to_string(wait_result));
}

u64 XferQueue::ReadbackCopy(u64 graphics_tick, std::span<const ReadbackRegion> regions) {
    if (!rb_enabled) {
        return 0;
    }
    // The submit of graphics_tick may still be queued on the recording thread; a wait queued
    // before its signal would block this queue for good (see Copy). Outside the mutex, which the
    // recording thread takes to collect the pending tick.
    if (graphics_tick != 0 && scheduler) {
        scheduler->WaitSubmitted(graphics_tick);
    }
    const std::scoped_lock lk{mutex};
    Slot& slot = rb_slots[rb_next_slot];
    if (slot.tick != 0 && Completed() < slot.tick) {
        return 0; // the previous copy of this command buffer is still running: usual path
    }
    rb_next_slot = (rb_next_slot + 1) % NumRbSlots;

    const u64 tick = next_tick++;
    Check(slot.cmd.begin(vk::CommandBufferBeginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    }));
    for (const auto& region : regions) {
        slot.cmd.copyBuffer(region.src, region.dst, region.copies);
    }
    // Transfer write -> host read; the guest reads the staging after waiting on the timeline.
    const vk::MemoryBarrier2 host_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    slot.cmd.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &host_barrier,
    });
    RecordHostVisibility(slot.cmd);
    Check(slot.cmd.end());

    // RAW: the wait on the graphics timeline value of the writer tick makes the writes of that
    // tick and of every earlier submit available and visible to the copy (semaphore memory
    // dependency); the caller guarantees no newer tick wrote these bytes.
    const vk::Semaphore xfer_sem = *timeline;
    const vk::Semaphore graphics_sem = graphics->Handle();
    const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
    const vk::TimelineSemaphoreSubmitInfo timeline_info = {
        .waitSemaphoreValueCount = graphics_tick != 0 ? 1u : 0u,
        .pWaitSemaphoreValues = &graphics_tick,
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &tick,
    };
    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_info,
        .waitSemaphoreCount = graphics_tick != 0 ? 1u : 0u,
        .pWaitSemaphores = &graphics_sem,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1u,
        .pCommandBuffers = &slot.cmd,
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &xfer_sem,
    };
    const vk::Result submit_result = queue.submit(submit_info, vk::Fence{});
    if (submit_result == vk::Result::eErrorDeviceLost) {
        instance.ReportDeviceFault();
        UNREACHABLE_MSG("Device lost during transfer readback submit");
    }
    ASSERT_MSG(submit_result == vk::Result::eSuccess, "Transfer readback submit failed: {}",
               vk::to_string(submit_result));
    slot.tick = tick;
    // Finish, sparse unbinds and ahead copies wait for last_tick: they see this copy too.
    last_tick = tick;
    rb_last_tick.store(tick, std::memory_order_release);
    return tick;
}

void XferQueue::RequireWait(u64 value) {
    const std::scoped_lock lk{mutex};
    pending_tick = std::max(pending_tick, value);
    last_tick = std::max(last_tick, value);
}

u64 XferQueue::Completed() const {
    const auto [result, counter] = instance.GetDevice().getSemaphoreCounterValue(*timeline);
    if (result == vk::Result::eErrorDeviceLost) {
        instance.ReportDeviceFault();
        UNREACHABLE_MSG("Device lost while reading the transfer timeline");
    }
    return result == vk::Result::eSuccess ? counter : 0;
}

void XferQueue::WaitValue(u64 value, u32 spin_us) {
    if (spin_us != 0) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(spin_us);
        do {
            for (int i = 0; i < 32; ++i) {
                _mm_pause();
            }
            if (Completed() >= value) {
                return;
            }
        } while (std::chrono::steady_clock::now() < deadline);
    }
    const vk::Semaphore wait_sem = *timeline;
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &wait_sem,
        .pValues = &value,
    };
    const vk::Result wait_result =
        instance.GetDevice().waitSemaphores(wait_info, std::numeric_limits<u64>::max());
    if (wait_result == vk::Result::eErrorDeviceLost) {
        instance.ReportDeviceFault();
        UNREACHABLE_MSG("Device lost during a transfer readback wait");
    }
    ASSERT_MSG(wait_result == vk::Result::eSuccess, "Transfer readback wait failed: {}",
               vk::to_string(wait_result));
}

bool Scheduler::XferCopy(vk::Buffer src, vk::Buffer dst, std::span<const vk::BufferCopy> copies) {
    return xfer && xfer->Copy(src, dst, copies);
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

    // Wait stages: the default per wait index, unless the wait names its own (transfer waits).
    std::array<vk::PipelineStageFlags, 4> wait_stages = wait_stage_masks;
    for (u32 i = 0; i < info.num_wait_semas; ++i) {
        if (info.wait_stages[i] != vk::PipelineStageFlags{}) {
            wait_stages[i] = info.wait_stages[i];
        }
    }

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
        .pWaitDstStageMask = wait_stages.data(),
        .commandBufferCount = static_cast<u32>(cmd_buffers.size()),
        .pCommandBuffers = cmd_buffers.data(),
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    std::unique_lock lk{Scheduler::submit_mutex, std::defer_lock};
    LockSubmit(lk);
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

    // sotc: SOTC_TRACE_GPU: the timestamp pair of the primary command buffer submitted here.
    const u32 gpu_pair = gpu_trace && !sessions.empty() ? sessions.back().gpu_pair
                                                        : SotcGpuTrace::Timeline::kNoPair;

    boost::container::small_vector<vk::CommandBuffer, 8> cmd_buffers;
    for (const auto& session : sessions) {
        if (session.upload) {
            cmd_buffers.push_back(session.upload);
        }
        cmd_buffers.push_back(session.primary);
    }
    sessions.clear();

    // sotc: SOTC_XFER_QUEUE: this submit waits for the uploads copied since the previous one.
    if (xfer) {
        if (const u64 xfer_tick = xfer->TakePendingTick(); xfer_tick != 0) {
            if (info.num_wait_semas < info.wait_semas.size()) {
                info.AddWait(xfer->Handle(), xfer_tick, vk::PipelineStageFlagBits::eAllCommands);
            } else {
                xfer->WaitLastCopy(); // no wait slot left: wait on the CPU instead
            }
        }
    }

    const vk::Semaphore timeline = work_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    if (recorder) {
        // The submit runs on the recording thread after the commands of these command buffers.
        recorder->Run(CommandRecorder::BlockCategory::Other,
                      [this, info, fakes = std::move(cmd_buffers), signal_value, gpu_pair] {
            boost::container::small_vector<vk::CommandBuffer, 8> real;
            for (const vk::CommandBuffer fake : fakes) {
                real.push_back(recorder->Real(fake));
            }
            // sotc: the pass secondaries of these command buffers are released at signal_value
            recorder->PassSubmitted(std::span<const vk::CommandBuffer>{fakes.data(), fakes.size()},
                                    signal_value);
            // sotc: SOTC_ASYNC_UPLOAD: the staging these copies read is filled first.
            if (auto* async_upload = VideoCore::AsyncUpload::Get()) {
                async_upload->WaitUpTo(this, signal_value);
            }
            {
                // sotc: SOTC_FPS accounting: vkQueueSubmit time of the recording thread
                const SotcStats::RoleTimer submit_time{SotcStats::RoleRecorder,
                                                       SotcStats::rec_submit_ns, "queue submit"};
                // sotc: SOTC_TRACE: stored before the submit, so the watcher never sees the
                // tick complete without it
                if (SotcTrace::kOn) {
                    SotcTrace::Submitted(trace_queue, signal_value, SotcStats::NowNs());
                }
                if (gpu_trace) { // sotc: SOTC_TRACE_GPU, before the submit as well
                    gpu_trace->Submitted(gpu_pair, signal_value, SotcTrace::Now());
                }
                QueueSubmit(instance, info, real);
            }
            recorded_submit_tick.store(signal_value, std::memory_order_release);
            recorded_submit_tick.notify_all();
            recorder->Retire(static_cast<u32>(fakes.size()));
        });
        recorder->Kick();
    } else {
        if (auto* async_upload = VideoCore::AsyncUpload::Get()) { // sotc: SOTC_ASYNC_UPLOAD
            async_upload->WaitUpTo(this, signal_value);
        }
        if (SotcTrace::kOn) { // sotc: SOTC_TRACE (before the submit, see above)
            SotcTrace::Submitted(trace_queue, signal_value, SotcStats::NowNs());
        }
        if (gpu_trace) { // sotc: SOTC_TRACE_GPU, before the submit
            gpu_trace->Submitted(gpu_pair, signal_value, SotcTrace::Now());
        }
        QueueSubmit(instance, info, cmd_buffers);
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
