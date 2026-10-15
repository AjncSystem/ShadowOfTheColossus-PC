// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>

#include "common/recursive_lock.h"
#include "common/shared_first_mutex.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/sotc_stats.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {

class GraphicsPipeline;
class Runtime;

class Rasterizer {
public:
    explicit Rasterizer(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
                        AmdGpu::Liverpool* liverpool);
    ~Rasterizer();

    [[nodiscard]] Scheduler& GetScheduler() noexcept {
        return scheduler;
    }

    [[nodiscard]] Runtime& GetRuntime() noexcept {
        return runtime;
    }

    [[nodiscard]] VideoCore::BufferCache& GetBufferCache() noexcept {
        return buffer_cache;
    }

    [[nodiscard]] VideoCore::TextureCache& GetTextureCache() noexcept {
        return texture_cache;
    }

    void Draw(bool is_indexed, u32 index_offset = 0);
    void DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 size, u32 max_count,
                      VAddr count_address, u16 vertex_sgpr_offset, u16 instance_sgpr_offset);

    void DispatchDirect();
    void DispatchIndirect(VAddr address, u32 offset, u32 size);

    void ScopeMarker(fmt::string_view fmt, fmt::format_args args, auto&& func) {
        if (host_markers_enabled) {
            ScopeMarkerBegin(fmt::vformat(fmt, args));
            func();
            ScopeMarkerEnd();
        } else {
            func();
        }
    }

    void ScopeMarkerBegin(const std::string_view& str, bool from_guest = false);
    void ScopeMarkerEnd(bool from_guest = false);
    void ScopedMarkerInsert(const std::string_view& str, bool from_guest = false);
    void ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                 bool from_guest = false);

    void FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds);
    void CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds);
    u32 ReadDataFromGds(u32 gsd_offset);
    bool InvalidateMemory(VAddr addr, u64 size, bool assume_locks = false);
    /// sotc: after a guest write fault at addr (BufferCache::MarkWriteAhead).
    void MarkWriteAhead(VAddr addr) {
        buffer_cache.MarkWriteAhead(addr);
    }
    bool ReadMemory(VAddr addr, u64 size, bool assume_locks = false);
    bool IsMapped(VAddr addr, u64 size);
    /// Command processor thread only, see BufferCache::IsRangeGpuWritten.
    bool IsRangeGpuWritten(VAddr addr, u64 size) const {
        return buffer_cache.IsRangeGpuWritten(addr, size);
    }
    /// Command processor thread only, see BufferCache::ReadClean.
    bool ReadCleanMemory(VAddr addr, void* out, u32 size) {
        return buffer_cache.ReadClean(addr, out, size);
    }
    void MapMemory(VAddr addr, u64 size);
    void RegisterMemory(VAddr addr, u64 size);
    void UnmapMemory(VAddr addr, u64 size);

    u64 Flush();
    void Finish();
    void OnSubmit();
    void OnFence();

    PipelineCache& GetPipelineCache() {
        return pipeline_cache;
    }

    template <typename Func>
    void ForEachMappedRangeInRange(VAddr addr, u64 size, Func&& func) {
        const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
        Common::RecursiveSharedLock lock{mapped_ranges_mutex};
        for (const auto& mapped_range : (mapped_ranges & range)) {
            func(mapped_range);
        }
    }

    std::thread::id GetGpuCommandProcessorThread();
#ifdef __linux__
    u32 GetGpuCommandProcessorThreadId();
#endif

private:
    /// sotc: async pipelines. A draw whose pipeline is still compiling may be skipped only when
    /// nothing the CPU reads back depends on it (see MaySkipForCompile).
    const GraphicsPipeline* GetDrawPipeline(DrawIndirectParams params = {});
    bool MaySkipForCompile();
    void MarkLodBanksOfCurrentStages();
    void NoteTarget(VAddr address);
    [[nodiscard]] bool TargetRecurring(VAddr address) const;
    struct TargetSeen {
        u64 first_flip;
        u64 last_flip;
    };
    tsl::robin_map<VAddr, TargetSeen> target_history;
    std::array<std::pair<VAddr, u64>, 64> target_recent{};
    u64 target_prune_flip = ~0ULL;

    void PrepareRenderState(const GraphicsPipeline* pipeline);
    RenderState BeginRendering(const GraphicsPipeline* pipeline);
    RenderState DeriveRenderState(const GraphicsPipeline* pipeline);
    const RenderState* ReusableRenderState(const GraphicsPipeline* pipeline);
    void RememberRenderState(const GraphicsPipeline* pipeline, const RenderState& state);
    void Resolve();
    void DepthStencilCopy(bool is_depth, bool is_stencil);
    void EliminateFastClear();

    void UpdateDynamicState(const GraphicsPipeline* pipeline, bool is_indexed) const;
    void UpdateViewportScissorState() const;
    void UpdateDepthStencilState() const;
    void UpdatePrimitiveState(bool is_indexed) const;
    void UpdateRasterizationState() const;
    void UpdateColorBlendingState(const GraphicsPipeline* pipeline) const;

    bool FilterDraw();

    void BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                     Shader::PushData& push_data);
    void BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding);
    bool BindResources(const Pipeline* pipeline);

    void BindVertexBuffers(const GraphicsPipeline* pipeline);
    void BindIndexBuffer(u32 index_offset = 0);

    void ResetBindings(bool is_compute);
    void MaybeDumpImages(u64 pgm_hash);
    void FlushPeriodic();
    u64 commands_tick{};
    u64 commands_since_submit{};
    std::chrono::steady_clock::time_point commands_since{};

    bool IsComputeMetaClear(const Pipeline* pipeline);
    bool IsComputeImageCopy(const Pipeline* pipeline);
    bool IsComputeImageClear(const Pipeline* pipeline);

private:
    friend class VideoCore::BufferCache;

    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    VideoCore::PageManager page_manager;
    VideoCore::BufferCache buffer_cache;
    VideoCore::TextureCache texture_cache;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    boost::icl::interval_set<VAddr> mapped_ranges;
    Common::SharedFirstMutex mapped_ranges_mutex;
    PipelineCache pipeline_cache;
    const bool host_markers_enabled;
    const bool guest_markers_enabled;

    using RenderTargetInfo = std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc>;
    std::array<RenderTargetInfo, AmdGpu::NUM_COLOR_BUFFERS> cb_descs;
    std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc> db_desc;
    /// sotc: the registers cb_descs[i] / db_desc were last built from, and the image FindImage
    /// gave for them (PrepareRenderState). SOTC_RT_MEMO=0 disables.
    struct ColorTargetMemo {
        bool valid{};
        std::array<u8, sizeof(AmdGpu::ColorBuffer)> regs{};
        u32 hint{};
        u64 generation{};
        VideoCore::ImageId image_id{};
    };
    struct DepthTargetMemo {
        bool valid{};
        std::array<u8, sizeof(AmdGpu::DepthBuffer)> buffer{};
        std::array<u8, sizeof(AmdGpu::DepthView)> view{};
        std::array<u8, sizeof(AmdGpu::DepthControl)> control{};
        VAddr htile{};
        u32 hint{};
        u64 generation{};
        VideoCore::ImageId image_id{};
    };
    std::array<ColorTargetMemo, AmdGpu::NUM_COLOR_BUFFERS> cb_memos{};
    DepthTargetMemo db_memo{};
    /// sotc: the fast clear state BeginRendering last found for each target slot
    /// (TextureCache::TakeMetaCleared).
    std::array<VideoCore::TextureCache::MetaClearMemo, AmdGpu::NUM_COLOR_BUFFERS> cb_meta_memos{};
    VideoCore::TextureCache::MetaClearMemo db_meta_memo{};
    /// sotc: every target of the current draw came from its memo in PrepareRenderState (the
    /// descriptions are the ones built when the memos were filled).
    bool targets_from_memo{};
    /// sotc: the render state BeginRendering last derived in full without barriers, and what it
    /// derived it from: the targets (images, backings and their state versions), the registers
    /// besides the targets' own, and the texture cache state it read. Equal inputs give an equal
    /// render state and no barriers (BeginRendering).
    struct RenderStateMemo {
        bool valid{};
        RenderState state{};
        u32 mrt_mask{};
        std::array<u8, AmdGpu::NUM_COLOR_BUFFERS> color_samples{};
        std::array<VideoCore::ImageId, AmdGpu::NUM_COLOR_BUFFERS + 1> ids{};
        std::array<const void*, AmdGpu::NUM_COLOR_BUFFERS + 1> backings{};
        std::array<u32, AmdGpu::NUM_COLOR_BUFFERS + 1> versions{};
        u64 image_set_generation{};
        u64 meta_generation{};
        u64 meta_clear_generation{};
        std::array<u8, 64> depth_regs{};
    };
    RenderStateMemo render_state_memo{};
    /// Barriers NoteBarrier recorded (BeginRendering tells its own from the others).
    u32 noted_barriers{};
    /// A target needed FindImage again in BeginRendering (its description may have changed).
    bool targets_rebound{};
    static bool RtMemo() {
        static const bool enabled = [] {
            const char* v = std::getenv("SOTC_RT_MEMO");
            return !v || v[0] != '0';
        }();
        return enabled;
    }
    boost::container::static_vector<vk::DescriptorImageInfo, Shader::NUM_IMAGES> image_infos;
    boost::container::static_vector<vk::DescriptorBufferInfo, Shader::NUM_BUFFERS> buffer_infos;
    boost::container::static_vector<VideoCore::ImageId, Shader::NUM_IMAGES> bound_images;
    struct BoundBuffer {
        const VideoCore::Buffer* buffer;
        u64 offset;
        u32 size;
        bool is_written;
    };
    boost::container::static_vector<BoundBuffer, Shader::NUM_BUFFERS> bound_buffers;
    /// Vertex/index buffers read by fixed-function stages of the current draw. They are recorded
    /// as reads after the draw so later writes to the same ranges wait for it.
    std::vector<BoundBuffer> fixed_function_reads;

    u32 set_write_index{};
    Pipeline::DescriptorWrites set_writes;
    Shader::PushData push_data;

    using ImageBindingInfo = std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc>;
    boost::container::static_vector<ImageBindingInfo, Shader::NUM_IMAGES> image_bindings;
    /// sotc: BindTextures' texture lookups (T#, the image resource bits that shape the
    /// description, binding index) -> the description and image FindImage gave, valid while the
    /// texture cache's image set generation is unchanged (BindTextures).
    struct TexBindMemo {
        std::array<u64, 4> sharp{};
        u32 bits{~0U};
        u64 generation{~0ULL};
        VideoCore::ImageId image_id{};
        VideoCore::TextureCache::ImageDesc desc{};
    };
    static constexpr size_t NumTexBindMemos = 512;
    std::unique_ptr<std::array<TexBindMemo, NumTexBindMemos>> tex_bind_memos =
        std::make_unique<std::array<TexBindMemo, NumTexBindMemos>>();
    bool attachment_feedback_loop{};
    bool needs_barrier{};
    /// sotc: why needs_barrier was set for the current draw/dispatch (SotcStats::barrier_breaks
    /// counts them when the barrier closes a render pass).
    enum BarrierReason : u32 {
        BarrierIndirectArgs = 1u << 0,
        BarrierVertex = 1u << 1,
        BarrierIndex = 1u << 2,
        BarrierGds = 1u << 3,
        BarrierBufferRead = 1u << 4,
        BarrierBufferWrite = 1u << 5,
        BarrierTexture = 1u << 6,
        BarrierColorTarget = 1u << 7,
        BarrierDepthTarget = 1u << 8,
    };
    u32 barrier_reasons{};
    void NoteBarrier(u32 reason, bool needed) {
        needs_barrier |= needed;
        barrier_reasons |= needed ? reason : 0u;
        noted_barriers += needed;
    }
    void CountBarrierBreak() {
        if (!scheduler.IsRendering()) {
            return;
        }
        for (u32 i = 0; i < 9; ++i) {
            if (barrier_reasons & (1u << i)) {
                SotcStats::barrier_breaks[i].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
};

} // namespace Vulkan
