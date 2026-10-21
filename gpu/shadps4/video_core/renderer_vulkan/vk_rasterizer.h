// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <unordered_map>

#include "common/recursive_lock.h"
#include "common/shared_first_mutex.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

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

    /// resolved/use_resolved: the pipeline the command processor looked up for this draw
    /// (Liverpool::IsBackThreaded), instead of looking it up here.
    void Draw(bool is_indexed, u32 index_offset = 0, const Pipeline* resolved = nullptr,
              bool use_resolved = false);
    void DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 size, u32 max_count,
                      VAddr count_address, u16 vertex_sgpr_offset, u16 instance_sgpr_offset,
                      const Pipeline* resolved = nullptr, bool use_resolved = false);

    void DispatchDirect(const Pipeline* resolved = nullptr, bool use_resolved = false);
    void DispatchIndirect(VAddr address, u32 offset, u32 size, const Pipeline* resolved = nullptr,
                          bool use_resolved = false);

    /// sotc: the pipeline lookups of the draws and dispatches alone (command processor thread).
    const Pipeline* LookupGraphicsPipeline(u16 vertex_sgpr_offset, u16 instance_sgpr_offset);
    /// sotc: the register-only test of FilterDraw: true when it will not record the draw. With
    /// the draw thread the command processor then skips the pipeline lookup (a pipeline for
    /// PrimitiveType::None would not even build).
    bool IsDrawFilteredOut() const;
    const Pipeline* LookupComputePipeline();
    bool IsGpuSideThread() const;
    void PrepareGpuSideFault();
    bool RunsGpuWorkElsewhere() const;
    template <typename F>
    void RunOnDrawThread(F&& func) {
        RunOnDrawThreadImpl(Common::UniqueFunction<void>{std::forward<F>(func)});
    }
    void RunOnDrawThreadImpl(Common::UniqueFunction<void>&& func);

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
    bool InvalidateMemoryOnGuestFault(VAddr addr, u64 size);
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
    bool IsGpuSideThreadId(u32 tid) const;
#endif

    /// sotc: frame generation without the HUD. The game composes its UI layer (an R8G8B8A8 sRGB
    /// render target) over the fp16 scene in one draw into the video out image; while frame
    /// generation (FSR 3 or DLSS-G) runs, that draw is repeated into an image of its own with a transparent UI
    /// texture (SOTC_FG_HUDLESS=0 turns it off). Thread of PrepareFrame: that image for the video
    /// out image at `address`, viewed as `format` (layout ShaderReadOnlyOptimal), when it was
    /// drawn since the last call; else a null view.
    vk::ImageView TakeHudless(VAddr address, vk::Format format);

private:
    void PrepareRenderState(const GraphicsPipeline* pipeline);
    RenderState BeginRendering(const GraphicsPipeline* pipeline);
    void RefreshImageLayouts();
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

    /// sotc: SOTC_SHADOW_REUSE: true when this draw's far sun shadow cascade is reused (moved
    /// by whole texels) instead of drawn this frame (vk_shadow_scroll.h).
    bool ReuseShadowCascade();
    std::unique_ptr<class ShadowScroll> shadow_scroll;

    void BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                     Shader::PushData& push_data);
    void BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding);
    bool BindResources(const Pipeline* pipeline);

    void BindVertexBuffers(const GraphicsPipeline* pipeline);
    void BindIndexBuffer(u32 index_offset = 0);

    void ResetBindings(bool is_compute);
    static bool TaaOff(); ///< sotc: DLSS on or SOTC_TAA_OFF=1, vk_rasterizer.cpp
    void MaybeDumpImages(u64 pgm_hash);
    void FlushPeriodic();
    u64 commands_tick{};
    u64 commands_since_submit{};
    u64 evict_poll{}; ///< sotc: FlushPeriodic commands without periodic submits
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
    boost::container::static_vector<vk::DescriptorImageInfo, Shader::NUM_IMAGES> image_infos;
    /// sotc: the image of each image_infos entry (null for samplers and empty slots). Ids, not
    /// pointers: FindImage may grow the image slot vector and move every image.
    std::array<VideoCore::ImageId, Shader::NUM_IMAGES> image_info_images{};
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
    bool attachment_feedback_loop{};
    bool needs_barrier{};

    /// sotc: TakeHudless. BeginHudless starts the redraw of this draw (the final composite) into
    /// its HUD-less image; false when this draw is not the composite.
    bool BeginHudless(const GraphicsPipeline* pipeline, const RenderState& state);
    struct HudlessImage {
        vk::Image image;
        vk::ImageView rt_view;   ///< in the composite's attachment format
        vk::ImageView read_view; ///< in the format TakeHudless asked
        vk::Format rt_format{}, read_format{};
        struct VmaAllocation_T* allocation{};
        u32 width{}, height{};
        bool fresh{};
    };
    void DestroyHudless(HudlessImage& image);
    std::unordered_map<VAddr, HudlessImage> hudless_images; ///< by video out address
    HudlessImage hudless_clear;  ///< 1x1, all zero: the UI layer of the redraw
    s32 hudless_ui_info{-1};     ///< this draw: image_infos index of the UI texture, -1 none
    VAddr hudless_vo{};          ///< this draw: the video out image it renders to, 0 none
    vk::Format hudless_vo_format{};
    u32 hudless_vo_width{}, hudless_vo_height{};

    /// sotc: DLSS super resolution draws the game's UI layer at the output size: each draw into
    /// it (Dlssg::UiTarget) is drawn again into ui_shadow with its viewport and scissor scaled,
    /// and the composite hands that to the presenter (Dlssg::NoteUiShadow). BeginUiShadow
    /// starts the redraw of this draw, EndUiShadow restores the dynamic state.
    bool BeginUiShadow(const GraphicsPipeline* pipeline, const RenderState& state);
    void EndUiShadow();
    void NoteUiShadow(); ///< at the composite: hands the shadow over, starts the next frame's
    HudlessImage ui_shadow; ///< R8G8B8A8: rt_view sRGB, read_view UNORM
    u32 ui_shadow_draws{};  ///< since the last composite
    bool ui_shadow_broken{}; ///< a draw into the UI layer was not redrawn since then
    bool ui_shadow_hit{};    ///< this draw renders into the UI layer
    vk::Format ui_shadow_format{};
    u32 ui_shadow_src_width{}, ui_shadow_src_height{}; ///< this draw: the UI layer size
    u32 ui_shadow_width{}, ui_shadow_height{};         ///< this draw: the output size
    Viewports ui_shadow_viewports;
    Scissors ui_shadow_scissors;
};

} // namespace Vulkan
