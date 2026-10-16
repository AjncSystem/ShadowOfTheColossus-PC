// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_recorder.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_frame_stats.h"
#include "video_core/sotc_stats.h"
#include <chrono>
#include <span>
#include <boost/container/small_vector.hpp>
#include <immintrin.h>

namespace Vulkan {

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
    SotcFrameStats::Scope timing{SotcFrameStats::Kind::SubmitWait};
    const SotcStats::ScopedTimer recorder_timer{SotcStats::recorder_wait_ns};
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
        .imageLayout = db.stencil_layout,
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

/// sotc: SOTC_BREAK_STATS=1: every 10 s, the call sites that closed render passes (offsets in
/// bb-probe.exe, for llvm-symbolizer), most frequent first.
[[gnu::noinline]] static void NoteRenderBreak(uintptr_t caller) {
    static const bool enabled = std::getenv("SOTC_BREAK_STATS") != nullptr;
    if (!enabled) {
        return;
    }
    static std::mutex mutex;
    static std::unordered_map<uintptr_t, u64> counts;
    static auto last = std::chrono::steady_clock::now();
    std::scoped_lock lk{mutex};
    ++counts[caller];
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(10)) {
        return;
    }
    last = now;
    std::vector<std::pair<u64, uintptr_t>> sorted;
    u64 total = 0;
    for (const auto& [where, n] : counts) {
        sorted.emplace_back(n, where);
        total += n;
    }
    std::sort(sorted.rbegin(), sorted.rend());
#ifdef _WIN32
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
#else
    const uintptr_t base = 0;
#endif
    std::fprintf(stderr, "SOTC render breaks in 10 s: %llu\n",
                 static_cast<unsigned long long>(total));
    for (size_t i = 0; i < sorted.size() && i < 12; ++i) {
        std::fprintf(stderr, "  %7llu exe+%#llx\n", static_cast<unsigned long long>(sorted[i].first),
                     static_cast<unsigned long long>(sorted[i].second - base + 0x140000000ULL));
    }
    counts.clear();
}

void Scheduler::EndRendering() {
    if (!is_rendering) {
        return;
    }
    SotcStats::render_breaks.fetch_add(1, std::memory_order_relaxed);
    NoteRenderBreak(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
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
    SotcStats::cp_drains[0].fetch_add(1, std::memory_order_relaxed);
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info);
    Wait(presubmit_tick);
}

void Scheduler::FinishHostRead() {
    SotcStats::cp_drains[1].fetch_add(1, std::memory_order_relaxed);
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info);
    WaitSubmitted(presubmit_tick);
    work_semaphore.WaitHostRead(presubmit_tick);
}

void Scheduler::Wait(u64 tick) {
    SotcStats::cp_drains[3].fetch_add(1, std::memory_order_relaxed);
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
    SotcStats::cp_drains[2].fetch_add(1, std::memory_order_relaxed);
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
    std::unique_lock lk(pending_ops_mutex);
    // sotc: this runs before every draw and dispatch; query the GPU timeline only when a pending
    // operation is waiting for a tick not yet known to be complete.
    // SOTC_POP_LAZY=0 refreshes on every call as before (A/B).
    static const bool lazy = [] {
        const char* v = std::getenv("SOTC_POP_LAZY");
        return !v || *v != '0';
    }();
    if (lazy && pending_ops.empty()) {
        return;
    }
    if (!lazy || pending_ops.empty() || !work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        work_semaphore.Refresh();
    }
    while (!pending_ops.empty() && work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
    }
}

static bool GpuTimeEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_GPU_TIME");
        return v && v[0] == '1';
    }();
    return enabled;
}

void Scheduler::EnableGpuTime() {
    gpu_time_on = GpuTimeEnabled();
}

void Scheduler::GpuTimeBegin(vk::CommandBuffer cmdbuf) {
    if (!gpu_time_pool) {
        const vk::QueryPoolCreateInfo pool_info = {
            .queryType = vk::QueryType::eTimestamp,
            .queryCount = GpuTimeQueries,
        };
        auto [result, pool] = instance.GetDevice().createQueryPoolUnique(pool_info);
        ASSERT_MSG(result == vk::Result::eSuccess, "SOTC_GPU_TIME: no query pool");
        gpu_time_pool = std::move(pool);
        gpu_time_period =
            instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
    }
    gpu_time_slot = (gpu_time_next++ % (GpuTimeQueries / 2)) * 2;
    cmdbuf.resetQueryPool(*gpu_time_pool, gpu_time_slot, 2);
    cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eNone, *gpu_time_pool, gpu_time_slot);
}

void Scheduler::GpuTimeEnd(vk::CommandBuffer cmdbuf, u64 tick) {
    if (gpu_time_slot == ~0u) {
        return;
    }
    cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *gpu_time_pool,
                           gpu_time_slot + 1);
    gpu_time_pending.emplace(tick, gpu_time_slot);
    gpu_time_slot = ~0u;
}

void Scheduler::GpuTimeCollect() {
    while (!gpu_time_pending.empty() && work_semaphore.IsFree(gpu_time_pending.front().first)) {
        const u32 slot = gpu_time_pending.front().second;
        std::array<u64, 2> stamps{};
        const auto result = instance.GetDevice().getQueryPoolResults(
            *gpu_time_pool, slot, 2, sizeof(stamps), stamps.data(), sizeof(u64),
            vk::QueryResultFlagBits::e64);
        if (result == vk::Result::eNotReady) {
            break;
        }
        gpu_time_pending.pop();
        if (result != vk::Result::eSuccess || stamps[1] < stamps[0]) {
            continue;
        }
        // Union of the intervals: the next command buffer may start before this one ends.
        const u64 from = std::max(stamps[0], gpu_time_last_end);
        if (stamps[1] > from) {
            SotcStats::gpu_busy_ns.fetch_add(u64((stamps[1] - from) * gpu_time_period),
                                             std::memory_order_relaxed);
        }
        if (gpu_time_last_end != 0 && stamps[0] > gpu_time_last_end) {
            SotcStats::gpu_gap_ns.fetch_add(u64((stamps[0] - gpu_time_last_end) * gpu_time_period),
                                            std::memory_order_relaxed);
        }
        gpu_time_last_end = std::max(gpu_time_last_end, stamps[1]);
        SotcStats::gpu_timed.fetch_add(1, std::memory_order_relaxed);
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
    if (gpu_time_on) {
        GpuTimeBegin(session.primary);
    }

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

    if (gpu_time_on && !sessions.empty()) {
        EndRendering();
        GpuTimeEnd(sessions.back().primary, signal_value);
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
            recorded_submit_tick.store(signal_value, std::memory_order_release);
            recorded_submit_tick.notify_all();
            recorder->Retire(static_cast<u32>(fakes.size()));
        });
        recorder->Kick();
    } else {
        QueueSubmit(instance, info, cmd_buffers);
    }

    work_semaphore.Refresh();
    if (gpu_time_on) {
        GpuTimeCollect();
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

} // namespace Vulkan
