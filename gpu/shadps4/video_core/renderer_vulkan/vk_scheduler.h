// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once


#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <queue>
#include <span>

#include "common/interval_set.h"
#include "common/unique_function.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"
#include "vulkan/vulkan.hpp"

namespace tracy {
class VkCtxScope;
}

namespace Vulkan {

class Instance;
class CommandRecorder;

struct RenderAttachment {
    vk::ImageView image_view;
    vk::ImageLayout image_layout;
    std::array<u32, 4> clear_value;
    union {
        u32 is_clear;
        struct {
            bool has_depth;
            bool depth_clear;
            bool has_stencil;
            bool stencil_clear;
        };
    };
};
static_assert(std::has_unique_object_representations_v<RenderAttachment>);

struct RenderState {
    std::array<RenderAttachment, 8> color_attachments;
    RenderAttachment depth_stencil_attachment;
    u16 width;
    u16 height;
    u16 num_layers;
    u16 num_color_attachments;
    // Formats and sample count of the attachments, for VkCommandBufferInheritanceRenderingInfo.
    // An undefined format means no attachment of that kind. Filled wherever attachments are set.
    std::array<vk::Format, 8> color_formats{};
    vk::Format depth_format{};
    vk::Format stencil_format{};
    vk::SampleCountFlagBits samples{vk::SampleCountFlagBits::e1};
    u32 tail_pad{}; // sotc: explicit padding, the struct must have no implicit padding (see below)

    /// sotc: the attachments decide the state. The formats and samples are informational (the
    /// inheritance of the pass secondaries, vk_recorder.cpp), so they do not restart a render pass.
    bool operator==(const RenderState& other) const noexcept {
        return std::memcmp(this, &other, offsetof(RenderState, color_formats)) == 0;
    }
};
static_assert(std::has_unique_object_representations_v<RenderState>);

struct SubmitInfo {
    std::array<vk::Semaphore, 4> wait_semas;
    std::array<u64, 4> wait_ticks;
    /// sotc: stage of each wait; 0 keeps the default of QueueSubmit for that index.
    std::array<vk::PipelineStageFlags, 4> wait_stages{};
    std::array<vk::Semaphore, 4> signal_semas;
    std::array<u64, 4> signal_ticks;
    vk::Fence fence;
    u32 num_wait_semas;
    u32 num_signal_semas;

    void AddWait(vk::Semaphore semaphore, u64 tick = 1, vk::PipelineStageFlags stage = {}) {
        wait_semas[num_wait_semas] = semaphore;
        wait_stages[num_wait_semas] = stage;
        wait_ticks[num_wait_semas++] = tick;
    }

    void AddSignal(vk::Semaphore semaphore, u64 tick = 1) {
        signal_semas[num_signal_semas] = semaphore;
        signal_ticks[num_signal_semas++] = tick;
    }

    void AddSignal(vk::Fence fence) {
        this->fence = fence;
    }
};

using Viewports = boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS>;
using Scissors = boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS>;
using ColorWriteMasks = std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS>;
struct StencilOps {
    vk::StencilOp fail_op{};
    vk::StencilOp pass_op{};
    vk::StencilOp depth_fail_op{};
    vk::CompareOp compare_op{};

    bool operator==(const StencilOps& other) const {
        return fail_op == other.fail_op && pass_op == other.pass_op &&
               depth_fail_op == other.depth_fail_op && compare_op == other.compare_op;
    }
};
struct DynamicState {
    struct {
        bool viewports : 1;
        bool scissors : 1;

        bool depth_test_enabled : 1;
        bool depth_write_enabled : 1;
        bool depth_compare_op : 1;

        bool depth_bounds_test_enabled : 1;
        bool depth_bounds : 1;

        bool depth_bias_enabled : 1;
        bool depth_bias : 1;

        bool stencil_test_enabled : 1;
        bool stencil_front_ops : 1;
        bool stencil_front_reference : 1;
        bool stencil_front_write_mask : 1;
        bool stencil_front_compare_mask : 1;
        bool stencil_back_ops : 1;
        bool stencil_back_reference : 1;
        bool stencil_back_write_mask : 1;
        bool stencil_back_compare_mask : 1;

        bool primitive_restart_enable : 1;
        bool rasterizer_discard_enable : 1;
        bool cull_mode : 1;
        bool front_face : 1;

        bool blend_constants : 1;
        bool color_write_masks : 1;
        bool line_width : 1;
        bool feedback_loop_enabled : 1;
    } dirty_state{};

    Viewports viewports{};
    Scissors scissors{};

    bool depth_test_enabled{};
    bool depth_write_enabled{};
    vk::CompareOp depth_compare_op{};

    bool depth_bounds_test_enabled{};
    float depth_bounds_min{};
    float depth_bounds_max{};

    bool depth_bias_enabled{};
    float depth_bias_constant{};
    float depth_bias_clamp{};
    float depth_bias_slope{};

    bool stencil_test_enabled{};
    StencilOps stencil_front_ops{};
    u32 stencil_front_reference{};
    u32 stencil_front_write_mask{};
    u32 stencil_front_compare_mask{};
    StencilOps stencil_back_ops{};
    u32 stencil_back_reference{};
    u32 stencil_back_write_mask{};
    u32 stencil_back_compare_mask{};

    bool primitive_restart_enable{};
    bool rasterizer_discard_enable{};
    vk::CullModeFlags cull_mode{};
    vk::FrontFace front_face{};

    std::array<float, 4> blend_constants{};
    ColorWriteMasks color_write_masks{};
    float line_width{};
    bool feedback_loop_enabled{};

    /// sotc: advanced by Invalidate(), which BeginSession calls for every new primary command
    /// buffer, and which the blit and shadow paths call after binding their own pipelines. The
    /// caches below are trusted only while their generation equals this one (SOTC_DEDUP_STATE).
    u64 generation{};

    /// sotc: the vertex input last sent with setVertexInputEXT (Rasterizer::BindVertexBuffers).
    struct VertexInputCache {
        static constexpr u32 MaxEntries = 32; ///< MaxVertexBufferCount (vk_graphics_pipeline.h)
        u64 generation{};                     ///< 0: nothing recorded
        boost::container::static_vector<vk::VertexInputAttributeDescription2EXT, MaxEntries>
            attributes;
        boost::container::static_vector<vk::VertexInputBindingDescription2EXT, MaxEntries> bindings;
    } vertex_input_cache{};

    /// sotc: the push constants last written by Pipeline::BindResources. Shared by all pipelines,
    /// so it lives here rather than in a pipeline.
    struct PushConstantCache {
        static constexpr u32 MaxBytes = 128; ///< Vulkan's guaranteed maxPushConstantsSize
        u64 generation{};                    ///< 0: nothing recorded
        vk::PipelineLayout layout{};
        vk::ShaderStageFlags stages{};
        u32 size{};
        std::array<u8, MaxBytes> bytes{};
    } push_constant_cache{};

    /// Commits the dynamic state to the provided command buffer.
    void Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf);

    /// Invalidates all dynamic state to be flushed into the next command buffer. Also voids the
    /// cached vertex inputs and push constants, which the new command buffer does not hold.
    void Invalidate() {
        std::memset(&dirty_state, 0xFF, sizeof(dirty_state));
        ++generation;
    }

    /// sotc: voids only the push constant cache. Called after any pass that binds its own
    /// pipeline or pushes its own constants on the scheduler's command buffer.
    void InvalidatePushConstants() {
        push_constant_cache.generation = 0;
    }

    void SetViewports(const Viewports& viewports_) {
        if (!std::ranges::equal(viewports, viewports_)) {
            viewports = viewports_;
            dirty_state.viewports = true;
        }
    }

    void SetScissors(const Scissors& scissors_) {
        if (!std::ranges::equal(scissors, scissors_)) {
            scissors = scissors_;
            dirty_state.scissors = true;
        }
    }

    void SetDepthTestEnabled(const bool enabled) {
        if (depth_test_enabled != enabled) {
            depth_test_enabled = enabled;
            dirty_state.depth_test_enabled = true;
        }
    }

    void SetDepthWriteEnabled(const bool enabled) {
        if (depth_write_enabled != enabled) {
            depth_write_enabled = enabled;
            dirty_state.depth_write_enabled = true;
        }
    }

    void SetDepthCompareOp(const vk::CompareOp compare_op) {
        if (depth_compare_op != compare_op) {
            depth_compare_op = compare_op;
            dirty_state.depth_compare_op = true;
        }
    }

    void SetDepthBoundsTestEnabled(const bool enabled) {
        if (depth_bounds_test_enabled != enabled) {
            depth_bounds_test_enabled = enabled;
            dirty_state.depth_bounds_test_enabled = true;
        }
    }

    void SetDepthBounds(const float min, const float max) {
        if (depth_bounds_min != min || depth_bounds_max != max) {
            depth_bounds_min = min;
            depth_bounds_max = max;
            dirty_state.depth_bounds = true;
        }
    }

    void SetDepthBiasEnabled(const bool enabled) {
        if (depth_bias_enabled != enabled) {
            depth_bias_enabled = enabled;
            dirty_state.depth_bias_enabled = true;
        }
    }

    void SetDepthBias(const float constant, const float clamp, const float slope) {
        if (depth_bias_constant != constant || depth_bias_clamp != clamp ||
            depth_bias_slope != slope) {
            depth_bias_constant = constant;
            depth_bias_clamp = clamp;
            depth_bias_slope = slope;
            dirty_state.depth_bias = true;
        }
    }

    void SetStencilTestEnabled(const bool enabled) {
        if (stencil_test_enabled != enabled) {
            stencil_test_enabled = enabled;
            dirty_state.stencil_test_enabled = true;
        }
    }

    void SetStencilOps(const StencilOps& front_ops, const StencilOps& back_ops) {
        if (stencil_front_ops != front_ops) {
            stencil_front_ops = front_ops;
            dirty_state.stencil_front_ops = true;
        }
        if (stencil_back_ops != back_ops) {
            stencil_back_ops = back_ops;
            dirty_state.stencil_back_ops = true;
        }
    }

    void SetStencilReferences(const u32 front_reference, const u32 back_reference) {
        if (stencil_front_reference != front_reference) {
            stencil_front_reference = front_reference;
            dirty_state.stencil_front_reference = true;
        }
        if (stencil_back_reference != back_reference) {
            stencil_back_reference = back_reference;
            dirty_state.stencil_back_reference = true;
        }
    }

    void SetStencilWriteMasks(const u32 front_write_mask, const u32 back_write_mask) {
        if (stencil_front_write_mask != front_write_mask) {
            stencil_front_write_mask = front_write_mask;
            dirty_state.stencil_front_write_mask = true;
        }
        if (stencil_back_write_mask != back_write_mask) {
            stencil_back_write_mask = back_write_mask;
            dirty_state.stencil_back_write_mask = true;
        }
    }

    void SetStencilCompareMasks(const u32 front_compare_mask, const u32 back_compare_mask) {
        if (stencil_front_compare_mask != front_compare_mask) {
            stencil_front_compare_mask = front_compare_mask;
            dirty_state.stencil_front_compare_mask = true;
        }
        if (stencil_back_compare_mask != back_compare_mask) {
            stencil_back_compare_mask = back_compare_mask;
            dirty_state.stencil_back_compare_mask = true;
        }
    }

    void SetPrimitiveRestartEnabled(const bool enabled) {
        if (primitive_restart_enable != enabled) {
            primitive_restart_enable = enabled;
            dirty_state.primitive_restart_enable = true;
        }
    }

    void SetCullMode(const vk::CullModeFlags cull_mode_) {
        if (cull_mode != cull_mode_) {
            cull_mode = cull_mode_;
            dirty_state.cull_mode = true;
        }
    }

    void SetFrontFace(const vk::FrontFace front_face_) {
        if (front_face != front_face_) {
            front_face = front_face_;
            dirty_state.front_face = true;
        }
    }

    void SetBlendConstants(const std::array<float, 4> blend_constants_) {
        if (blend_constants != blend_constants_) {
            blend_constants = blend_constants_;
            dirty_state.blend_constants = true;
        }
    }

    void SetRasterizerDiscardEnabled(const bool enabled) {
        if (rasterizer_discard_enable != enabled) {
            rasterizer_discard_enable = enabled;
            dirty_state.rasterizer_discard_enable = true;
        }
    }

    void SetColorWriteMasks(const ColorWriteMasks& color_write_masks_) {
        if (!std::ranges::equal(color_write_masks, color_write_masks_)) {
            color_write_masks = color_write_masks_;
            dirty_state.color_write_masks = true;
        }
    }

    void SetLineWidth(const float width) {
        if (line_width != width) {
            line_width = width;
            dirty_state.line_width = true;
        }
    }

    void SetAttachmentFeedbackLoopEnabled(const bool enabled) {
        if (feedback_loop_enabled != enabled) {
            feedback_loop_enabled = enabled;
            dirty_state.feedback_loop_enabled = true;
        }
    }
};

/// sotc: SOTC_XFER_QUEUE=1. Buffer uploads (BufferCache::SynchronizeMemory) are recorded on the
/// transfer-only queue family of Instance, one command buffer per copy, and submitted at once.
/// Each copy waits on the graphics timeline for the newest graphics submit, so it runs after
/// all graphics work already submitted. The next graphics submit waits on the copy's timeline
/// value (Scheduler::SubmitExecution), so everything recorded after the copy sees its data.
/// Staging memory is safe to reuse: its tick is the next graphics submit, which waits for the copy.
class Scheduler;

class XferQueue {
public:
    XferQueue(const Instance& instance_, Semaphore& graphics_, Scheduler* scheduler_ = nullptr);
    ~XferQueue();

    /// Records and submits a buffer copy. Returns false when nothing was submitted.
    bool Copy(vk::Buffer src, vk::Buffer dst, std::span<const vk::BufferCopy> copies);

    /// sotc: SOTC_HOST_IMPORT. One copy between two buffers.
    struct Region {
        vk::Buffer src;
        vk::Buffer dst;
        vk::BufferCopy copy;
    };
    /// sotc: SOTC_HOST_IMPORT. Submits the copies and waits on the CPU until they are done. Unlike
    /// Copy, it does not wait for the graphics work: the destinations must be memory no graphics
    /// work uses (fresh staging). The next graphics submit still waits for it (TakePendingTick),
    /// which makes the copied bytes visible to it.
    void CopyAndWait(std::span<const Region> regions);

    /// Returns and clears the timeline value the next graphics submit must wait for (0: none).
    [[nodiscard]] u64 TakePendingTick();

    /// Timeline semaphore of the copies (waited on by the graphics submits).
    [[nodiscard]] vk::Semaphore Handle() const noexcept {
        return *timeline;
    }

    /// Waits on the CPU until the newest copy has finished (readbacks ahead of the queue).
    void WaitLastCopy();

    /// sotc: SOTC_RB_TICK=1. One buffer readback copy (arena -> download staging).
    struct ReadbackRegion {
        vk::Buffer src;
        vk::Buffer dst;
        std::span<const vk::BufferCopy> copies;
    };
    /// sotc: SOTC_RB_TICK=1. Copies the regions on the transfer queue, after graphics tick
    /// graphics_tick (already flushed: the caller knows nothing newer wrote them) and nothing
    /// newer, and returns the value of the transfer timeline that is signalled when they ran and
    /// their data is visible to the host (0: no free slot, nothing submitted). The slots are a
    /// ring of their own, apart from the upload ones. Does NOT make the next graphics submit
    /// wait: RequireWait does, when a write to the bytes comes (write after read). It does bump
    /// last_tick, so Scheduler::Finish and sparse binds (unbinds) see it.
    u64 ReadbackCopy(u64 graphics_tick, std::span<const ReadbackRegion> regions);

    /// sotc: SOTC_RB_TICK=1. The next graphics submit waits for this timeline value.
    void RequireWait(u64 value);

    /// sotc: SOTC_RB_TICK=1. Counter of the transfer timeline (any thread).
    [[nodiscard]] u64 Completed() const;

    /// sotc: SOTC_RB_TICK=1. Waits until the timeline reached value: polls up to spin_us
    /// microseconds, then blocks.
    void WaitValue(u64 value, u32 spin_us);

    /// sotc: SOTC_RB_TICK=1. Value of the newest readback copy (0: none). Any thread.
    [[nodiscard]] u64 LastReadbackTick() const noexcept {
        return rb_last_tick.load(std::memory_order_acquire);
    }

    /// sotc: timeline value of the newest copy submitted (0: none yet). SOTC_RB_ASYNC_AHEAD
    /// waits for it on the GPU instead of the CPU (WaitLastCopy).
    [[nodiscard]] u64 LastTick() {
        const std::scoped_lock lk{mutex};
        return last_tick;
    }

private:
    static constexpr u32 NumSlots = 8;
    struct Slot {
        vk::CommandBuffer cmd{};
        u64 tick{}; ///< timeline value of the copy last recorded here; 0: never used
    };

    const Instance& instance;
    Semaphore* graphics;
    Scheduler* scheduler{}; ///< sotc: to wait until a graphics tick is on the queue
    vk::Queue queue;
    vk::UniqueSemaphore timeline;
    vk::UniqueCommandPool pool;
    std::array<Slot, NumSlots> slots{};
    u32 next_slot{};
    u64 next_tick{1};
    u64 last_tick{};
    u64 pending_tick{};
    // sotc: SOTC_RB_TICK: the readback command buffers (their own pool and ring).
    static constexpr u32 NumRbSlots = 16;
    vk::UniqueCommandPool rb_pool;
    std::array<Slot, NumRbSlots> rb_slots{};
    u32 rb_next_slot{};
    std::atomic<u64> rb_last_tick{0};
    bool rb_enabled{};
    bool upload_routing{}; ///< SOTC_XFER_QUEUE: uploads on this queue order against readbacks
    std::mutex mutex;
};

using SessionFunc = Common::UniqueFunction<void>;
using SubmitFunc = Common::UniqueFunction<void, SubmitInfo&>;

class Scheduler {
public:
    /// record_thread: Vulkan commands are recorded and submitted on a thread of their own
    /// (cp_record_thread of Pink-shadPS4, vk_recorder.h).
    explicit Scheduler(const Instance& instance, bool record_thread = false);
    ~Scheduler();

    /// Makes the calling thread the one recording (shutdown from another thread).
    void ClaimRecorder();

    /// Waits until the recording thread has put the submit of tick on the queue. Another
    /// thread that queues work waiting for tick on the same queue must call this first: a wait
    /// queued before its signal blocks the queue for good. kick: the recording thread only.
    void WaitSubmitted(u64 tick, bool kick = true);

    /// Sends the current execution context to the GPU
    /// and increments the scheduler timeline semaphore.
    void Flush(SubmitInfo& info);

    /// Sends the current execution context to the GPU
    /// and increments the scheduler timeline semaphore.
    void Flush();

    /// Sends the current execution context to the GPU and waits for it to complete.
    void Finish();

    /// sotc: Finish for readbacks: returns once the host can read what was submitted, which with
    /// SOTC_WAIT_MARKER=2 can be before the driver reports it (Semaphore::WaitHostRead).
    void FinishHostRead();

    /// Waits for the given tick to trigger on the GPU.
    void Wait(u64 tick);

    /// sotc: true when SOTC_XFER_QUEUE created the transfer queue (see XferQueue).
    [[nodiscard]] bool HasXferQueue() const noexcept {
        return xfer != nullptr;
    }

    /// sotc: copies buffer data on the transfer queue (XferQueue::Copy). False: not routed, the
    /// caller records the copy on the graphics queue as before.
    bool XferCopy(vk::Buffer src, vk::Buffer dst, std::span<const vk::BufferCopy> copies);

    /// sotc: SOTC_HOST_IMPORT, see XferQueue::CopyAndWait. False without the transfer queue.
    bool XferCopyAndWait(std::span<const XferQueue::Region> regions) {
        if (!xfer) {
            return false;
        }
        xfer->CopyAndWait(regions);
        return true;
    }

    /// sotc: SOTC_RB_TICK, see XferQueue::ReadbackCopy / RequireWait / Completed / WaitValue.
    /// 0 / false without the transfer queue.
    u64 XferReadbackCopy(u64 graphics_tick, std::span<const XferQueue::ReadbackRegion> regions) {
        return xfer ? xfer->ReadbackCopy(graphics_tick, regions) : 0;
    }
    void XferRequireWait(u64 value) {
        if (xfer) {
            xfer->RequireWait(value);
        }
    }
    [[nodiscard]] bool XferDone(u64 value) const {
        return !xfer || xfer->Completed() >= value;
    }
    void XferWaitValue(u64 value, u32 spin_us) {
        if (xfer) {
            xfer->WaitValue(value, spin_us);
        }
    }
    [[nodiscard]] u64 XferLastReadbackTick() const noexcept {
        return xfer ? xfer->LastReadbackTick() : 0;
    }
    [[nodiscard]] vk::Semaphore XferSemaphore() const noexcept {
        return xfer ? xfer->Handle() : vk::Semaphore{};
    }

    /// Begins a one-off command buffer that is submitted ahead of the one being recorded, for
    /// work that only depends on already submitted commands (readbacks of their results).
    vk::CommandBuffer BeginAhead();

    /// Submits the command buffer from BeginAhead after all submitted work and waits for it.
    void SubmitAheadAndWait();

    /// sotc: SOTC_RB_ASYNC_AHEAD=1 (BufferCache::BeginAsyncReadback). Producer thread only.
    /// Begins a command buffer from a pool of NumAheadSlots, for copies that read only what
    /// submitted ticks wrote. Null when the next slot's previous copy is still on the GPU (the
    /// caller takes the usual path then). Unlike BeginAhead nothing is waited for here or at the
    /// submit: the guest thread waits for the returned value (WaitAhead).
    vk::CommandBuffer BeginAheadAsync();

    /// sotc: submits the command buffer from BeginAheadAsync without waiting and returns the
    /// value of the ahead timeline it signals. With the recording thread the submit is a block of
    /// its stream, so it lands on the queue after the submits of every tick flushed before (they
    /// are earlier in the stream) and before the submit of the tick being recorded; without it,
    /// it is queued here, the flushed ticks being on the queue already. Either way it does not
    /// flush the tick being recorded nor wait for its async uploads. It waits on the GPU for the
    /// newest transfer-queue copy (the graphics queue does not order those).
    u64 SubmitAheadAsync();

    /// sotc: true once the ahead copy that signals value has completed. Any thread.
    [[nodiscard]] bool IsAheadDone(u64 value);

    /// sotc: waits until the ahead copy of value is on the queue (the recording thread may not
    /// have reached it yet), then until it completed. Any thread; for the guest's split timing.
    void WaitAheadSubmitted(u64 value);
    void WaitAhead(u64 value);

    /// Attempts to execute operations whose tick the GPU has caught up with.
    void PopPendingOperations();

    /// Starts a new rendering scope with provided state.
    void BeginRendering(const RenderState& new_state);

    /// Ends current rendering scope.
    void EndRendering();

    /// True inside a rendering scope.
    bool IsRendering() const {
        return is_rendering;
    }

    /// Starts a new session.
    void BeginSession();

    /// Returns the current command buffer used for uploads.
    vk::CommandBuffer UploadCommandBuffer();

    /// Sets a function to be called on every session finalization.
    void SetSessionCallback(SessionFunc&& on_session) {
        this->on_session = std::move(on_session);
    }

    /// Sets a function to be called on every scheduler submission.
    void SetSubmitCallback(SubmitFunc&& on_submit) {
        this->on_submit = std::move(on_submit);
    }

    /// Returns the current render state.
    const RenderState& GetRenderState() const {
        return render_state;
    }

    /// Returns the current pipeline dynamic state tracking.
    DynamicState& GetDynamicState() {
        return dynamic_state;
    }

    /// Returns the current command buffer.
    vk::CommandBuffer CommandBuffer() const {
        return sessions.back().primary;
    }

    /// sotc: number of the current session, which is the command buffer being recorded. It changes
    /// on every BeginSession, so per-command-buffer state (descriptor buffer bindings) can key on it.
    [[nodiscard]] u64 SessionSerial() const noexcept {
        return session_serial;
    }

    /// sotc: calls func with the real current command buffer at this point of the command
    /// stream: on the recording thread when there is one (func may wait there without holding
    /// up the command processor), otherwise right away.
    void RecordDeferred(Common::UniqueFunction<void, vk::CommandBuffer>&& func);

    /// sotc: true when Vulkan commands are recorded on a thread of their own.
    [[nodiscard]] bool HasRecordThread() const noexcept {
        return recorder != nullptr;
    }

    /// Returns the current command buffer tick.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return work_semaphore.CurrentTick();
    }

    /// Returns true when a tick has been triggered by the GPU.
    [[nodiscard]] bool IsFree(u64 tick) noexcept {
        if (work_semaphore.IsFree(tick)) {
            return true;
        }
        work_semaphore.Refresh();
        return work_semaphore.IsFree(tick);
    }

    /// Returns the scheduler timeline semaphore.
    [[nodiscard]] Semaphore* GetWorkSemaphore() noexcept {
        return &work_semaphore;
    }

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Will be run when submitting or calling PopPendingOperations.
    void DeferOperation(Common::UniqueFunction<void>&& func) {
        std::unique_lock lk(pending_ops_mutex);
        const u64 tick = CurrentTick();
        if (pending_ops.empty()) {
            pending_front_tick.store(tick, std::memory_order_release);
        }
        pending_ops.emplace(std::move(func), tick);
    }

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Runs as soon as possible in another thread.
    void DeferPriorityOperation(Common::UniqueFunction<void>&& func) {
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops.emplace(std::move(func), CurrentTick());
        }
        priority_pending_ops_cv.notify_one();
    }

    static std::mutex submit_mutex;

    /// sotc: SOTC_HRR=1: while host-visible arena chunks exist (set by the buffer cache), every
    /// command buffer this scheduler ends (sessions, ahead, transfer queue) finishes with a global
    /// barrier, all writes -> host reads, so the CPU may read what the GPU wrote once the tick
    /// that wrote it is waited for (BufferCache direct readbacks). Read when the command buffer
    /// is ended, on the thread that records it.
    static void SetHostVisibility(bool on);
    static inline std::atomic<bool> host_visibility{false};

private:
    void EndSession();

    void SubmitExecution(SubmitInfo& info);

    /// Recording thread: a real command buffer for the fake handle the producer uses.
    void BeginRecorded(vk::CommandBuffer fake);


    void PriorityPendingOpsThread(std::stop_token stoken);
    /// sotc: SOTC_TRACE: records when the GPU reaches each tick of this scheduler's timeline.
    void TraceWatchThread(std::stop_token stoken);

private:
    const Instance& instance;
    Semaphore work_semaphore;
    CommandPool command_pool;
    DynamicState dynamic_state;
    SessionFunc on_session{};
    SubmitFunc on_submit{};
    struct Session {
        vk::CommandBuffer upload{};
        vk::CommandBuffer primary{};
    };
    std::vector<Session> sessions;
    u64 session_serial{}; // sotc: see SessionSerial()
    std::condition_variable_any event_cv;
    struct PendingOp {
        Common::UniqueFunction<void> callback;
        u64 gpu_tick;
    };
    std::queue<PendingOp> pending_ops;
    std::recursive_mutex pending_ops_mutex;
    // sotc: tick of the front pending op (UINT64_MAX when none), kept under pending_ops_mutex and
    // read without it by PopPendingOperations' early-out (SOTC_POP_FAST=0 disables).
    std::atomic<u64> pending_front_tick{UINT64_MAX};
    std::queue<PendingOp> priority_pending_ops;
    std::mutex priority_pending_ops_mutex;
    std::condition_variable_any priority_pending_ops_cv;
    std::jthread priority_pending_ops_thread;
    u32 trace_queue{~0u};           // sotc: SOTC_TRACE queue track (SotcTrace::RegisterQueue)
    std::jthread trace_watch_thread; // sotc: SOTC_TRACE (TraceWatchThread)
    RenderState render_state;
    bool is_rendering = false;
    vk::UniqueCommandPool ahead_pool;
    vk::CommandBuffer ahead_cmdbuf;
    vk::UniqueFence ahead_fence;
    u64 ahead_sequence{};
    bool ahead_unfinished{};
    // sotc: SOTC_RB_ASYNC_AHEAD (BeginAheadAsync). One pool per slot, so recording into a slot
    // on the producer never shares a pool with a submit on the recording thread. A slot is reused
    // only after the ahead timeline reached its value.
    struct AheadSlot {
        vk::UniqueCommandPool pool;
        vk::CommandBuffer cmdbuf{};
        u64 value{}; ///< ahead timeline value of its last submit; 0: never used
    };
    static constexpr u32 NumAheadSlots = 16;
    std::array<AheadSlot, NumAheadSlots> ahead_slots{};
    vk::UniqueSemaphore ahead_timeline;
    AheadSlot* ahead_slot_open{};
    u32 ahead_slot_next{};
    u64 ahead_value_last{};                 ///< producer: newest value handed out
    std::atomic<u64> ahead_value_submitted{0}; ///< newest value on the queue
    std::atomic<u64> ahead_value_done{0};      ///< cache of the timeline's counter
    tracy::VkCtxScope* profiler_scope{};
    /// Newest tick the recording thread has put on the queue.
    std::atomic<u64> recorded_submit_tick{0};
    /// sotc: SOTC_XFER_QUEUE only; null otherwise.
    std::unique_ptr<XferQueue> xfer;
    /// Last member: stops (after running everything queued) before the rest is destroyed.
    std::unique_ptr<CommandRecorder> recorder;
};

} // namespace Vulkan
