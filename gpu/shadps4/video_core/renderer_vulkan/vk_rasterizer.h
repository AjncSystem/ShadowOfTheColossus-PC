// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstdlib>
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

/// sotc: SOTC_DRAW_PHASES=1: TSC timers per phase of a graphics draw on the draw thread (and of
/// its capture on the command processor), by class (depth-only, G-buffer, other), printed every
/// 32768 draws as "SOTC DRAWPHASE". Off: one predictable branch per phase.
namespace DrawPhase {
inline const bool kOn = std::getenv("SOTC_DRAW_PHASES") != nullptr;
inline u64 Now() {
    return __builtin_ia32_rdtsc();
}
/// Draw thread: ExecuteBackOp's register chunk copy and per-stage user data copy, per op.
void AddOpPrefix(u64 ticks, u32 chunks, u32 dwords, u32 kind = 0);
/// sotc: SOTC_DRAW_REPLAY_PROBE=1 (docs/sotc/DRAW-REPLAY.md): per draw, is its whole bound state
/// the previous draw's? Needs SOTC_DRAW_PHASES=1 for the per-class phase timers. Printed by the
/// FPS report (ReplayReport), once per window.
inline const bool kReplay = std::getenv("SOTC_DRAW_REPLAY_PROBE") != nullptr;
void ReplayReport();
/// Command processor: 0 = pipeline lookup, 1 = BackAlloc (dirty chunk copy), 2 = CaptureBackOp.
void AddCp(u32 what, u64 ticks);
} // namespace DrawPhase

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
    /// sotc: queues the translation of the compute program in the current compute registers.
    void PrefetchComputeProgram() {
        pipeline_cache.PrefetchComputeProgram();
    }
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
    /// sotc: BbToggle::DepthFastPath (docs/sotc/DEPTH-FASTPATH.md). BeginRendering for a depth-only
    /// draw replays the RenderState of the last full one when everything it was derived from is
    /// provably the same (registers by Liverpool::DrawRelSerial, registry and metadata epochs,
    /// image flags and layout state); `storage` takes the state of a full call, `token` is what
    /// Scheduler::BeginRendering uses to skip comparing a replayed state.
    const RenderState& BeginRenderingFast(const GraphicsPipeline* pipeline, RenderState& storage,
                                          u64& token);
    bool DepthFastActive() const;
    void DepthFastEnsureRelevance();
    u32 DepthFastBeginCheck(const GraphicsPipeline* pipeline);
    void DepthFastBeginApply();
    void DepthFastRecord(const RenderState& state);
    /// sotc: BbToggle::GBufferFastPath (docs/sotc/GBUFFER-FASTPATH.md): the same for a draw with
    /// colour targets. GBufferFastCheck is 0 when the RenderState BeginRendering would derive is
    /// gb_memo.state and its effects are the few GBufferFastApply repeats; else why not.
    const RenderState& GBufferFast(const GraphicsPipeline* pipeline, RenderState& storage,
                                   u64& token);
    u32 GBufferFastCheck(const GraphicsPipeline* pipeline);
    void GBufferFastApply();
    void GBufferFastRecord(const RenderState& state, const GraphicsPipeline* pipeline);
    void RefreshImageLayouts();
    void Resolve();
    void DepthStencilCopy(bool is_depth, bool is_stencil);
    void EliminateFastClear();

    void UpdateDynamicState(const GraphicsPipeline* pipeline, bool is_indexed) const;
    /// sotc: BbToggle::DrawFixedCostOpt. UpdateDynamicState, skipped when none of the registers it
    /// reads changed in value since the last time it ran (and the pipeline's write masks and the
    /// feedback loop flag are the same): the dynamic state object still holds what it derived.
    void CommitDynamicState(const GraphicsPipeline* pipeline, bool is_indexed);
    void CommitDynamicStateOld(const GraphicsPipeline* pipeline, bool is_indexed);
    void UpdateDynamicStateValues(const GraphicsPipeline* pipeline, bool is_indexed) const;
    struct DynMemo {
        bool valid{};
        bool feedback_loop{};
        u32 serial{};
        std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS> write_masks{};
    };
    DynMemo dyn_memo;
    /// sotc: BbToggle::DepthFastPath. The inputs of the last derivation of the dynamic state values
    /// (by any draw): no relevant register changed since (Liverpool::DrawRelSerial), the pipeline's
    /// write masks, the feedback loop flag and indexed-ness are those; valid only while every
    /// derivation went through this memo (the old path clears it).
    struct DynFast {
        bool valid{};
        bool feedback_loop{};
        bool is_indexed{};
        u32 rel_serial{}; ///< Liverpool::RelDynamic
        std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS> write_masks{};
    };
    DynFast df_dyn;
    std::vector<std::pair<u32, u32>> dyn_fields; ///< byte offset in the registers and size of each input
    std::vector<u8> dyn_snap;                    ///< the inputs as of the last derivation
    /// sotc: the vertex input last recorded with setVertexInputEXT (BindVertexBuffers), and the
    /// DynamicState::invalidations count it was recorded at.
    struct ViMemo {
        bool valid{};
        u32 invalidations{};
        VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
        VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
    };
    ViMemo vi_memo;
    std::vector<u16> dyn_chunks; ///< register chunks UpdateDynamicState reads (built on first use)
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
    static bool DrawBindMemoOn();
    struct TexBindMemo;
    struct TexBindMemoBinding;
    void BindResolvedImage(VideoCore::ImageId image_id, VideoCore::TextureCache::BindingType type,
                           const VideoCore::ImageViewInfo& view_info);
    bool TexBindMemoFresh(const TexBindMemo& memo) const;
    /// sotc: BbToggle::TexBindFastPath. 0 when BindResolvedImage for this memoized binding would
    /// only add the descriptor the memo remembers (no upload, no barrier, same view); else why not.
    u32 TexBindFastCheck(const TexBindMemoBinding& b, VideoCore::ImageId image_id);
    void TexBindFastApply(const TexBindMemoBinding& b, VideoCore::ImageId image_id);
    bool ReplayTexBindMemo(const Shader::Info& stage, TexBindMemo& memo,
                           const AmdGpu::Image* sharps,
                           boost::container::small_vector<u32, 8>& array_sizes,
                           std::array<VideoCore::ImageId, Shader::NUM_IMAGES>& bind_ids,
                           std::array<bool, Shader::NUM_IMAGES>& bind_storage, u32& num_binds);
    bool BindResources(const Pipeline* pipeline);
    bool FastBindEligible(const Pipeline* pipeline) const;
    bool FastBindStaticEligible(const Pipeline* pipeline) const; ///< sotc: DrawReplay, see .cpp
    u64 FastBindHash(const Pipeline* pipeline, const Shader::PushData& push_data) const;
    /// sotc: BbToggle::DrawReplay. The fields FastBindHash hashes, kept as they are: the push data,
    /// the colour and depth target addresses and, per stage, InfoRuntime::ud_gen.
    struct FastBindKey {
        Shader::PushData push{};
        std::array<VAddr, AmdGpu::NUM_COLOR_BUFFERS + 1> addrs{};
        std::array<u64, 8> gen{};
        u32 stages{};
        bool Equal(const FastBindKey& o) const {
            return stages == o.stages && gen == o.gen && addrs == o.addrs &&
                   std::memcmp(&push, &o.push, sizeof(push)) == 0;
        }
    };
    void FastBindKeyBuild(const Pipeline* pipeline, const Shader::PushData& push,
                          FastBindKey& key) const;
    void CommitFastBind(const Pipeline* pipeline, u64 hash, bool uses_dma, u64 pre_epoch,
                        u64 pre_clobbers, const FastBindKey* key = nullptr);

    void BindVertexBuffers(const GraphicsPipeline* pipeline);
    void BindIndexBuffer(u32 index_offset = 0);

    void ResetBindings(bool is_compute);
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

    /// sotc: BbToggle::BufferBindMemo (BindBuffers). The result of a plain read-only buffer binding
    /// by its V# bytes: buffer and offset, the clamped size, the VMA table generation the clamp was
    /// read at and the BufferCache token that says the arena lookup and page look still hold.
    struct BufferBindMemo {
        std::array<u64, 2> words{};
        u64 generation{};
        const VideoCore::Buffer* buffer{};
        u64 offset{};
        u64 size{};
        VideoCore::BufferCache::BindToken token;
    };
    static constexpr size_t NumBufferBindMemos = 1024;
    std::array<BufferBindMemo, NumBufferBindMemos> buffer_bind_memo{};
    /// sotc: BbToggle::DispatchFastPath. ClampRangeSize of a binding's (address, size), which only
    /// depends on the guest VMA table: remembered per table generation (runtime_memory_generation,
    /// even and unchanged = the same table).
    struct ClampMemo {
        VAddr base{};
        u64 size{};
        u64 generation{}; ///< 0: empty (a valid generation is even and never 0 once the table has changed)
        u64 result{};
    };
    static constexpr size_t NumClampMemos = 512;
    std::array<ClampMemo, NumClampMemos> clamp_memos{};
    u64 ClampBindingSize(VAddr base, u64 size);

    u32 set_write_index{};
    Pipeline::DescriptorWrites set_writes;
    Shader::PushData push_data;

    using ImageBindingInfo = std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc>;
    boost::container::static_vector<ImageBindingInfo, Shader::NUM_IMAGES> image_bindings;

    /// sotc: BbToggle::DrawBindMemo. The texture bindings of a stage, resolved: for an exact set
    /// of T#s (what ImageDesc is built from) the image each one found and the view it asked for.
    /// A binding stays valid while the images registered at its guest address do not change (the
    /// per-address generation FindImage's own memo uses; for a mip-tail candidate also those at
    /// the address of the other form of the chain) and the GPU-modified epoch stays; the
    /// per-draw effects (found tick, binding flags, image update, layout transition, usage)
    /// are redone on every replay (BindTextures). Direct mapped on the stage and the T#s.
    struct TexBindMemoBinding {
        VideoCore::ImageId image_id{}; ///< as FindImage returned it (before the depth redirect)
        VideoCore::ImageViewInfo view_info;
        VideoCore::TextureCache::BindingType type{};
        VAddr addr{};        ///< guest address FindImage matched the image at (0: no image)
        u64 addr_gen{};      ///< TextureCache::FindGeneration(addr) when it did
        VAddr other_addr{};  ///< mip-tail candidates: the address of the other form of the chain
        u64 other_gen{};
        // sotc: BbToggle::TexBindFastPath: what BindResolvedImage produced when this binding was
        // stored (the image after the stencil redirect, its backing, the view and the layout the
        // descriptor got); `fast` only for a plain texture binding that was no render target.
        VideoCore::ImageId bound_id{};
        const void* backing{};
        vk::ImageView view{};
        bool fast{};
    };
    static constexpr u32 MemoMaxImages = 16;
    struct TexBindMemo {
        const Shader::Info* stage{};
        u64 gpu_epoch{};
        u32 count{};
        std::array<AmdGpu::Image, MemoMaxImages> sharps;
        std::array<TexBindMemoBinding, MemoMaxImages> bindings;
    };
    /// sotc: BbToggle::DrawBindMemo. What PrepareRenderState read for the last draw (the render
    /// target registers and extents, the pipeline's attachment mask) and the registry state it
    /// resolved them in. While they stay the same, cb_descs/db_desc already hold what it would
    /// compute again (the images, found by FindImage), so a draw only redoes its per-draw
    /// effects (found tick, bound list, target flag).
    struct RtMemo {
        bool valid{};
        u32 mrt_mask{};
        u32 active_mask{}; ///< attachments with a non-empty write mask
        bool cb_disabled{};
        bool depth_enable{};
        bool stencil_enable{};
        bool depth_write{}; ///< sotc: db_desc.view_info.is_storage follows depth_write_enable
        u32 htile{};
        u64 image_generation{};
        u64 gpu_epoch{};
        u32 rel_depth{}, rel_color{}; ///< sotc: Liverpool::DrawRelSerial when stored (DepthFastPath)
        AmdGpu::DepthBuffer depth_buffer{};
        AmdGpu::DepthView depth_view{};
        AmdGpu::CbDbExtent db_extent{};
        std::array<AmdGpu::CbDbExtent, AmdGpu::NUM_COLOR_BUFFERS> cb_extent{};
        std::array<AmdGpu::ColorBuffer, AmdGpu::NUM_COLOR_BUFFERS> color_buffers{};
    };
    RtMemo rt_memo;
    /// sotc: BbToggle::DepthFastPath. What the last full BeginRendering of a depth-only draw
    /// derived its RenderState from, and the state itself (see BeginRenderingFast).
    struct DepthFastMemo {
        bool valid{};
        u32 rel_serial{};
        u64 image_generation{}, gpu_epoch{}, meta_epoch{};
        VideoCore::ImageId db_id{};
        VideoCore::ImageId stencil_id{};
        const void* backing{};
        vk::ImageLayout layout{};
        vk::AccessFlags2 access{};
        vk::PipelineStageFlags2 stage{};
        std::optional<VideoCore::SubresourceRange> range;
        u64 token{};
        RenderState state{};
    };
    DepthFastMemo df_memo;
    /// sotc: BbToggle::GBufferFastPath. What the last full BeginRendering of a draw with colour
    /// targets derived its RenderState from, and the state itself (see GBufferFast).
    struct GBufferFastMemo {
        bool valid{};
        u32 rel_render{}, rel_color{}, mrt_mask{}, num_cb{};
        std::array<u8, AmdGpu::NUM_COLOR_BUFFERS> samples{};
        u64 image_generation{}, gpu_epoch{}, meta_epoch{};
        std::array<VideoCore::ImageId, AmdGpu::NUM_COLOR_BUFFERS> color_id{};
        std::array<const void*, AmdGpu::NUM_COLOR_BUFFERS> color_backing{};
        VideoCore::ImageId db_id{};
        VideoCore::ImageId stencil_id{};
        const void* db_backing{};
        vk::ImageLayout db_layout{};
        vk::AccessFlags2 db_access{};
        vk::PipelineStageFlags2 db_stage{};
        std::optional<VideoCore::SubresourceRange> db_range;
        u64 token{};
        RenderState state{};
    };
    GBufferFastMemo gb_memo;
    /// SOTC_DEPTHFAST_WHY=1: the depth side registers as of the last record, to tell which one
    /// moved when the register serial says the memo is out.
    struct DepthRegsSnap {
        AmdGpu::DepthControl depth_control{};
        AmdGpu::DepthBuffer depth_buffer{};
        AmdGpu::DepthRenderControl depth_render_control{};
        AmdGpu::ColorControl color_control{};
        AmdGpu::DepthView depth_view{};
        u32 htile{};
        u32 depth_clear{};
        u32 stencil_clear{};
        u64 db_extent{};
    };
    DepthRegsSnap df_snap;
    /// The Transit BeginRendering made on the depth image (captured there, used by the memo).
    vk::ImageLayout df_transit_layout{};
    vk::AccessFlags2 df_transit_access{};
    vk::PipelineStageFlags2 df_transit_stage{};
    std::optional<VideoCore::SubresourceRange> df_transit_range;
    u64 df_next_token{1};
    bool df_rel_built{};
    static constexpr u32 NumTexBindMemos = 4096;
    struct FreeDeleter {
        void operator()(void* p) const noexcept {
            std::free(p);
        }
    };
    std::unique_ptr<TexBindMemo[], FreeDeleter> tex_bind_memos; ///< zeroed pages, committed on use
    bool attachment_feedback_loop{};
    bool needs_barrier{};
    /// sotc: SOTC_DEPTH_RO. The depth buffer of this draw is sampled (depth aspect) while its
    /// stencil is written: one layout serves both.
    bool DepthSampledAsAttachment(VideoCore::ImageId image_id, const VideoCore::Image& image,
                                  const VideoCore::ImageViewInfo& view_info) const;
    bool bind_stage_compute{}; ///< the stage BindTextures is binding is a compute shader

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
    /// sotc: descriptor state of the previous graphics draw (SOTC_FAST_BIND). Valid only while
    /// the same command buffer, pipeline, push data and resource generations are current.
    struct FastBindState {
        const Pipeline* pipeline{};
        u64 hash{};
        u64 buffer_generation{};
        u64 texture_generation{};
        u64 stream_generation{};
        u64 mem_epoch{}; ///< SotcStats::mem_epoch: guest memory unchanged since
        u64 clobbers{};  ///< SotcStats::push_clobbers when the bind started
        u64 tick{};
        bool uses_dma{};
        bool valid{};
        bool key_valid{};  ///< sotc: DrawReplay: `key` was stored by this bind
        bool hash_valid{}; ///< sotc: DrawReplay: `hash` was computed by this bind
        FastBindKey key{};
        boost::container::small_vector<u32, 8> lod_marks{}; ///< SotcLod banks the bind marked
    };
    boost::container::small_vector<u32, 8> lod_marks; ///< banks marked by the bind in progress
    FastBindState fast_bind{};
    bool verify_pending{};
    u64 verify_uploads{};
    std::vector<vk::DescriptorBufferInfo> verify_buffers;
    std::vector<vk::DescriptorImageInfo> verify_images;
    bool fast_bind_hit{};
    /// sotc: SOTC_DRAW_REPLAY_PROBE (DrawPhase::kReplay): compares this draw's complete bound state
    /// with the previous draw's (docs/sotc/DRAW-REPLAY.md); returns the phase class of the draw.
    u32 ReplayProbeNote(u32 kind, const GraphicsPipeline* pipeline, const RenderState& state,
                        VAddr args_addr, u32 stride, u32 count, u32 first);
};

} // namespace Vulkan
