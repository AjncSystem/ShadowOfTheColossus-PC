// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <chrono>
#include <unordered_map>

#include "common/recursive_lock.h"
#include "common/shared_first_mutex.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/range_stamps.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/amdgpu/draw_prep.h"
#include "video_core/amdgpu/regs.h"

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
    /// sotc: prepared: SOTC_PARALLEL_RECORD's inputs for this draw (draw thread), or null.
    void Draw(bool is_indexed, u32 index_offset = 0, const Pipeline* resolved = nullptr,
              bool use_resolved = false, const PreparedDraw* prepared = nullptr);
    void DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 size, u32 max_count,
                      VAddr count_address, u16 vertex_sgpr_offset, u16 instance_sgpr_offset,
                      const Pipeline* resolved = nullptr, bool use_resolved = false,
                      const PreparedDraw* prepared = nullptr);

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
    /// sotc: SOTC_ASYNC_IMAGE_CREATE: command processor thread, right after the lookup of the
    /// pipeline of a draw or dispatch (its runtime data in slot 0): queues the creation of the
    /// images its T#s need that do not exist yet (TextureCache::PrefetchTexture).
    void PrefetchImages(const Pipeline* pipeline) {
        if (pipeline && texture_cache.ImagePrefetchEnabled()) {
            PrefetchImagesImpl(pipeline);
        }
    }
    void PrefetchImagesImpl(const Pipeline* pipeline);
    /// sotc: SOTC_PARALLEL_RECORD. A GpuPrep thread: the register-only inputs of a draw
    /// (draw_prep.h) from that thread's registers and Shader::g_info_slot; and PrefetchImages
    /// for several callers.
    void PrepareDraw(const AmdGpu::Regs& regs, const Pipeline* pipeline, bool is_indexed,
                     bool direct, PreparedDraw& out) const;
    /// sotc: SOTC_PR_RESOLVE. Called at the end of PrepareDraw on a GpuPrep thread: the
    /// PreparedResources of a graphics draw (draw_prep.h). Resets everything it writes.
    void PrepareResources(const GraphicsPipeline* pipeline, PreparedResources& out) const;
    void PrefetchImagesShared(const Pipeline* pipeline);
    /// sotc: SOTC_PARALLEL_RECORD: moves on every MapMemory / UnmapMemory (a prepared draw is
    /// used only while it stands still).
    [[nodiscard]] u64 MapEpoch() const noexcept {
        return map_epoch.load(std::memory_order_acquire);
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
    /// sotc: SOTC_BIND_CACHE_RANGES: a GPU write outside BindResources (shader HLE) drops the
    /// cached binds reading [addr, addr + size). No-op otherwise.
    void BindCacheNoteWrite(VAddr addr, u64 size);
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
    /// sotc: copies the LOD feedback buffer to the host and clears it (sotc_lod_stats.h)
    void SotcLodFeedbackCycle();
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
    /// sotc: SOTC_RS_MEMO (see RsMemo below).
    u32 RsMemoBuildKey(const GraphicsPipeline* pipeline);
    bool RsMemoPrepare(const GraphicsPipeline* pipeline);
    bool RsMemoBegin(RenderState& state);
    bool RsMemoTargetClean(VideoCore::ImageId image_id);
    void RefreshImageLayouts();
    void Resolve();
    void DepthStencilCopy(bool is_depth, bool is_stencil);
    void EliminateFastClear();

    void UpdateDynamicState(const GraphicsPipeline* pipeline, bool is_indexed,
                            const PreparedDraw* prepared = nullptr) const;
    /// sotc: the former Update*State, split into a computation from the given registers (any
    /// thread; refuse: set instead of asserting on an unsupported restart index) and the
    /// DynamicState setters (draw thread).
    void ComputeDynamic(const AmdGpu::Regs& regs, bool is_indexed, DynamicValues& out,
                        bool* refuse = nullptr) const;
    void ComputeViewportScissor(const AmdGpu::Regs& regs, DynamicValues& out) const;
    void ComputeDepthStencil(const AmdGpu::Regs& regs, DynamicValues& out) const;
    void ComputePrimitive(const AmdGpu::Regs& regs, bool is_indexed, DynamicValues& out,
                          bool* refuse) const;
    void ApplyDynamic(const DynamicValues& values, const GraphicsPipeline* pipeline) const;
    /// sotc: SOTC_PR_VERIFY: the prepared inputs, or null when the inline check disagrees.
    const PreparedDraw* CheckPrepared(const PreparedDraw& prepared,
                                      const GraphicsPipeline* pipeline, bool is_indexed,
                                      bool direct);

    bool FilterDraw();

    /// sotc: SOTC_SHADOW_REUSE: true when this draw's far sun shadow cascade is reused (moved
    /// by whole texels) instead of drawn this frame (vk_shadow_scroll.h).
    bool ReuseShadowCascade();
    std::unique_ptr<class ShadowScroll> shadow_scroll;

    /// sotc: SOTC_PR_RESOLVE: pr / pr_stage (both or neither) are the prepared entries of this
    /// stage; a descriptor they do not cover is resolved inline.
    void BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                     Shader::PushData& push_data, u32 set,
                     const PreparedResources* pr = nullptr, const PrStage* pr_stage = nullptr);
    void BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding, u32 set,
                      const PreparedResources* pr = nullptr, const PrStage* pr_stage = nullptr);
    bool BindResources(const Pipeline* pipeline, const PreparedDraw* prepared = nullptr);
    bool FastBindEligible(const Pipeline* pipeline) const;
    bool FastBindStatic(const Pipeline* pipeline, const AmdGpu::Regs& regs) const;
    u64 FastBindHash(const Pipeline* pipeline, const Shader::PushData& push_data) const;
    u64 FastBindHash(const Pipeline* pipeline, const Shader::PushData& push_data,
                     const AmdGpu::Regs& regs) const;
    void CommitFastBind(const Pipeline* pipeline, u64 hash, bool uses_dma);
    /// sotc: SOTC_BIND_CACHE (vk_rasterizer.cpp, see BindCacheEntry below).
    bool BindCacheEligible(const Pipeline* pipeline) const;
    bool BindCacheEligible(const Pipeline* pipeline, const AmdGpu::Regs& regs) const;
    void BindCacheSync(const Pipeline* pipeline, bool eligible);
    bool TryBindCache(const Pipeline* pipeline, u64 hash, bool& uses_dma);
    void CommitBindCache(const Pipeline* pipeline, u64 hash, bool uses_dma,
                         size_t first_bound_buffer);
    void BindCacheFlush(u32 reason);
    /// sotc: SOTC_BIND_CACHE_FRAME (vk_rasterizer.cpp, see BindCacheEntry below). The table form of
    /// BindCacheSync / TryBindCache / CommitBindCache / BindCacheInvalidate.
    void BindCacheSyncFrame(const Pipeline* pipeline, bool eligible);
    bool TryBindCacheFrame(const Pipeline* pipeline, u64 hash, bool& uses_dma);
    void CommitBindCacheFrame(const Pipeline* pipeline, u64 hash, bool uses_dma,
                              size_t first_bound_buffer);
    void BindCacheInvalidateFrame(VAddr addr, u64 size, VideoCore::ImageId image);
    void BindCacheNoteLookup(const VideoCore::TextureCache::ImageDesc& desc,
                             VideoCore::ImageId image);
    /// sotc: SOTC_BIND_CACHE_FRAME. The key of a bind by what it binds: the pipeline, the stages'
    /// identity and layout and the T#/V#/S# the stages read, not the constants around them.
    /// false: the pipeline reads resources by something the key cannot name (descriptor arrays,
    /// sharp tables, DMA); the full FastBindHash key is used then.
    bool BindResourceHash(const Pipeline* pipeline, u64& hash) const;
    /// The flat buffer of a stage as the shader must see it, with the live-setting patches (TAA,
    /// tonemap vignette, vegetation, grass) applied; the data pointer is valid until the next call.
    const u32* FlatForBind(const Shader::Info& stage);
    /// sotc: SOTC_BIND_CACHE: a sampled image of the bind being recorded and the transition
    /// BindTextures asked for it (replayed on a hit).
    void BindCacheNoteImage(VideoCore::ImageId image_id, vk::ImageLayout layout,
                            vk::PipelineStageFlags2 stages, vk::AccessFlags2 access,
                            const VideoCore::SubresourceRange& range);
    /// sotc: SOTC_BIND_CACHE_RANGES. Drops the cached binds that read guest memory in
    /// [addr, addr + size) (a bound buffer range or a sampled image overlapping it), or that sample
    /// `image`. Called for every GPU write to guest memory or images (see BindCacheSync).
    void BindCacheInvalidate(VAddr addr, u64 size, VideoCore::ImageId image = {});
    /// sotc: SOTC_BIND_CACHE_RANGES: BindCacheInvalidate for a written image and its guest range.
    void BindCacheInvalidateImage(VideoCore::ImageId image_id);

    /// sotc: records vertex input, vertex and index buffers again when a submit happened since
    /// vertex_tick (the draw moved to a new command buffer).
    void RebindVertexIfSubmitted(const GraphicsPipeline* pipeline, u64& vertex_tick,
                                 bool is_indexed, const std::function<void()>& bind_index);
    void BindVertexBuffers(const GraphicsPipeline* pipeline);
    void BindIndexBuffer(u32 index_offset = 0);

    void ResetBindings(bool is_compute);
    void CaptureTextures();
    static bool TaaOff(); ///< sotc: DLSS on or SOTC_TAA_OFF=1, vk_rasterizer.cpp
    u32 CompositeUiIndex(); ///< sotc: which of the composite's two images is the UI layer
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
    // sotc: the finest mip per texture counter bank written by fragment shaders (device local),
    // and the host copies read when the GPU reaches them (one per area in flight).
    static constexpr u32 SotcLodFeedbackBytes = 256 * sizeof(u32); ///< + the gate word
    static constexpr u32 SotcLodFeedbackAreas = 4;
    u32 sotc_lod_cycles{};
    std::unique_ptr<VideoCore::Buffer> sotc_lod_feedback;
    std::unique_ptr<VideoCore::Buffer> sotc_lod_download;
    std::array<std::atomic<bool>, SotcLodFeedbackAreas> sotc_lod_pending{};
    u32 sotc_lod_area{};
    std::chrono::steady_clock::time_point sotc_lod_last{};
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
    std::vector<u32> set_write_sets; ///< sotc: descriptor set of each set_writes entry
    Shader::PushData push_data;
    Shader::BindlessSamplerSlots bindless_slots{}; // sotc: SOTC_BINDLESS_SAMP, pushed after push_data

    using ImageBindingInfo = std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc>;
    boost::container::static_vector<ImageBindingInfo, Shader::NUM_IMAGES> image_bindings;
    bool attachment_feedback_loop{};
    bool needs_barrier{};
    /// sotc: SOTC_DEPTH_RO. The depth buffer of this draw is sampled (depth aspect) while its
    /// stencil is written: one layout serves both.
    bool DepthSampledAsAttachment(VideoCore::ImageId image_id, const VideoCore::Image& image,
                                  const VideoCore::TextureCache::ImageDesc& desc,
                                  bool compute) const;

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
        u64 tick{};
        bool uses_dma{};
        bool valid{};
    };
    FastBindState fast_bind{};
    bool fast_bind_hit{};

    /// sotc: SOTC_BIND_CACHE=1 (default off). The results of the last full binds of eligible
    /// graphics draws (read-only resources), keyed by pipeline and FastBindHash (push data, user
    /// data, flat buffers, render target addresses). A draw whose key matches skips the sharp
    /// walks and the buffer/texture cache lookups: the descriptor arrays and push data are copied
    /// back, the image transitions and buffer access checks of the full bind are replayed, and
    /// the caller pushes them with Pipeline::BindResources. Every entry dies together
    /// (BindCacheFlush) when anything they rely on moves: the submit tick, guest memory
    /// (mem_epoch), resident buffers / registered images / samplers / the stream ring, the render
    /// targets, or a bind or DMA that writes GPU memory.
    /// sotc: SOTC_BIND_CACHE_RANGES=1 replaces the guest memory and GPU write flushes by
    /// BindCacheInvalidate on the written ranges (vk_rasterizer.cpp, BindCacheSync).
    static constexpr u32 BindCacheEntries = 32;
    struct BindCacheImage {
        VideoCore::ImageId id;
        vk::ImageLayout layout;
        vk::PipelineStageFlags2 stages;
        vk::AccessFlags2 access;
        VideoCore::SubresourceRange range;
        VAddr guest_lo{}, guest_hi{}; ///< SOTC_BIND_CACHE_RANGES: the image's guest memory
        u64 uid{};                    ///< SOTC_BIND_CACHE_FRAME: Image::image_uid (slots are reused)
    };
    /// sotc: SOTC_BIND_CACHE_XTICK. What a hit needs to synchronize a bound buffer range again
    /// (ObtainBuffer's arguments) and to see that the guest mapping under it is as it was.
    struct BindCacheRepair {
        VAddr addr{};
        u64 vsize{};  ///< the V#'s size, before the clamp to the mapping
        u64 mapped{}; ///< ClampRangeSize(addr, vsize) when the entry was made
        const VideoCore::Buffer* arena{};
        u32 size{};
        bool texel{};
    };
    /// sotc: SOTC_BIND_CACHE_XTICK. A sampler of the bind: touched again in a later tick (its LRU
    /// stamp keeps it from being collected, which would flush the cache) and checked to be the
    /// same object.
    struct BindCacheSampler {
        AmdGpu::Sampler sharp{};
        vk::Sampler handle{};
        u32 slot{}; ///< bindless heap slot (bindless only)
        bool is_depth{};
        bool bindless{};
    };
    /// sotc: SOTC_BIND_CACHE_FRAME. A flat buffer copy in the stream ring that a hit refreshes:
    /// the stage (index in GetStages), the buffer_infos element, the size, and what the entry
    /// last copied there (hash of the data, ring offset, ring wrap count).
    struct BindCacheFlatSlot {
        u32 stage{};
        u32 info{};
        u32 size{};
        u64 hash{};
        u64 offset{};
        u64 gen{~0ULL};
        /// sotc: tick of the copy. The ring keeps a region alive only for the tick that
        /// allocated it (StreamBuffer::AdvanceAndWatch), so a copy is reused only in that tick.
        u64 tick{~0ULL};
    };
    struct BindCacheEntry {
        const Pipeline* pipeline{};
        u64 hash{};
        u64 last_use{};
        bool valid{};
        bool uses_dma{};
        Shader::PushData push_data{};
        Shader::BindlessSamplerSlots bindless_slots{};
        std::vector<vk::WriteDescriptorSet> writes;
        std::vector<u32> write_sets;
        std::vector<u32> write_info; ///< index into buffer_infos (top bit) or image_infos
        std::vector<vk::DescriptorBufferInfo> buffer_infos;
        std::vector<vk::DescriptorImageInfo> image_infos;
        std::vector<VideoCore::ImageId> image_info_images;
        std::vector<BoundBuffer> buffers;
        std::vector<BindCacheImage> images;
        std::vector<u32> lod_banks; ///< SotcLod::Mark of the full bind, repeated on a hit
        /// Guest ranges of the bound buffers: a hit needs them clean (no upload pending, e.g. a
        /// page left writable by the fault grace, whose writes do not move mem_epoch).
        std::vector<std::pair<VAddr, u64>> ranges;
        /// SOTC_BIND_CACHE_RANGES: [lo, hi) bounds `ranges` and the images' guest memory, so
        /// BindCacheInvalidate rejects most entries with one comparison.
        VAddr lo{}, hi{};
        /// SOTC_BIND_CACHE_XTICK: one per range, the sampler list, the tick of the last replay of
        /// the per-tick stamps (a full bind makes them for its own tick) and the use epoch of the
        /// last block stamping (0: due).
        std::vector<BindCacheRepair> repairs;
        std::vector<BindCacheSampler> samplers;
        u64 made_tick{~0ULL};
        u64 stamp_tick{~0ULL};
        u16 stamp_epoch{};
        /// sotc: SOTC_BIND_CACHE_FRAME. The key (pipeline, hash, render targets), the cache-wide
        /// flush generation, BufferCache::BindVersion and the stream ring wrap count the entry
        /// was made at, the sequence numbers of the stamp tables it was last checked at, the ranges
        /// whose image set it depends on (looked-up T# ranges, the images', texel buffers'), and
        /// the game frame it was made in.
        u64 key{};
        u64 targets_key{};
        u64 flush_gen{};
        u64 buffer_version{};
        u64 stream_gen{};
        u64 checked_tex_seq{};
        u64 checked_write_seq{};
        u64 map_epoch{};
        u64 made_frame{};
        bool uses_stream{};    ///< holds a flat buffer copy in the stream ring
        bool map_dependent{};  ///< bound a T# the guest mapping rejected (a map can change that)
        /// Bound a mip-tail image: FindImage's tail choice depends on which overlapping images are
        /// GPU-written (VideoCore::gpu_modified_epoch), checked at the hit.
        bool tail_dependent{};
        u64 gpu_epoch{};
        /// Made under the resource identity key: the flat buffer copies and the viewport words of
        /// the push data are refreshed at every hit (flat_slots).
        bool refresh{};
        std::vector<BindCacheFlatSlot> flat_slots;
        std::vector<std::pair<VAddr, VAddr>> tex_watch;
    };
    /// The state every live entry was made in; a change flushes them all.
    struct BindCacheState {
        u64 tick{~0ULL};
        u64 buffer_generation{};
        u64 texture_generation{};
        u64 stream_generation{};
        u64 mem_epoch{};
        u64 write_epoch{};
        std::array<VideoCore::ImageId, AmdGpu::NUM_COLOR_BUFFERS + 1> targets{};
        u64 targets_key{}; ///< SOTC_BIND_CACHE_FRAME: the targets and their identities, hashed
    };
    std::array<BindCacheEntry, BindCacheEntries> bind_cache{};
    /// sotc: SOTC_BIND_CACHE_FRAME: an open-addressing table instead of `bind_cache` (allocated on
    /// first use): a key hashes to a slot and is found within FrameProbe slots from it. frame_keys
    /// holds the key of each live slot (0: empty), so a lookup touches one cache line.
    static constexpr u32 BindCacheFrameEntries = 4096;
    static constexpr u32 BindCacheFrameProbe = 8;
    std::vector<BindCacheEntry> frame_cache;
    std::vector<u64> frame_keys;
    /// A cache-wide flush (BindCacheFlush) is one increment: older entries are dropped when found.
    u64 bind_cache_flush_gen{1};
    /// GPU writes to guest ranges and images (BindCacheInvalidate) and image set changes
    /// (TextureCache::ImageChangedSince) since an entry's checkpoints.
    VideoCore::RangeStamps write_stamps;
    static constexpr u32 WriteImageStamps = 4096;
    std::array<std::atomic<u64>, WriteImageStamps> write_image_stamps{};
    /// What the full bind being recorded depends on besides the cache key.
    std::vector<std::pair<VAddr, VAddr>> bind_cache_lookups_rec; ///< T# lookup ranges
    std::vector<std::pair<VAddr, VAddr>> bind_cache_texel_rec;   ///< texel buffer ranges
    std::vector<std::pair<VAddr, VAddr>> bind_cache_tex_watch_rec; ///< CommitBindCacheFrame scratch
    bool bind_cache_stream_rec{};     ///< the bind copied a flat buffer into the stream ring
    bool bind_cache_map_dep_rec{};    ///< the bind rejected a T# for its guest mapping
    u64 bind_rec_tex_seq{};           ///< ImageChangeSeq() when the bind began
    u64 bind_rec_write_seq{};         ///< write_stamps.Seq() when the bind began
    u64 bind_rec_map_epoch{};         ///< MapEpoch() when the bind began
    u64 bind_rec_gpu_epoch{};         ///< VideoCore::gpu_modified_epoch when the bind began
    bool bind_cache_tail_rec{};       ///< the bind found a mip-tail image
    std::vector<BindCacheFlatSlot> bind_cache_flat_rec; ///< flat buffer copies of the bind
    u32 cur_bind_stage{};             ///< index (in GetStages) of the stage being bound
    bool bind_level2{};               ///< this bind's key is the resource identity
    Shader::PushData bind_input_push{}; ///< the draw's own push data (registers) before the bind
    /// SOTC_FPS only: keys looked up in this game frame (distinct keys per frame).
    struct KeySeen {
        u64 key{};
        u64 frame{};
    };
    std::array<KeySeen, 16384> bind_key_seen{};
    u64 bind_key_frame{};
    /// The image an image copy / clear dispatch wrote (IsComputeImageCopy / IsComputeImageClear).
    VideoCore::ImageId compute_written_image{};
    /// sotc: SOTC_BIND_CACHE_FRAME: the memo of read-only V# resolutions (BindBuffers): the arena
    /// and offset ObtainBuffer returned for a binding, valid while BufferCache::BindVersion and the
    /// guest mapping under the binding are as they were (the checks of a cached bind's hit).
    struct BufferMemo {
        VAddr addr{};
        u64 vsize{};  ///< the V#'s size, before the clamp to the mapping
        u64 mapped{}; ///< ClampRangeSize(addr, vsize)
        u64 size{};   ///< the size ObtainBuffer was asked for
        u64 offset{};
        u64 version{};
        const VideoCore::Buffer* arena{};
        u16 stamp_epoch{};
        bool valid{};
    };
    static constexpr u32 BufferMemoEntries = 2048;
    std::vector<BufferMemo> buffer_memo;
    bool bind_memo_ok{}; ///< this bind is a read-only graphics bind (BindResources)
    /// Whether the entry still depends on nothing that changed since it last checked (the
    /// stamps), updating its checkpoints; `reason` names what moved when not.
    bool BindCacheEntryFresh(BindCacheEntry& entry, u32& reason);
    bool BindCacheStampsFresh(BindCacheEntry& entry, u32& reason);
    void BindCacheDropFrame(BindCacheEntry& entry, u32 reason);
    BindCacheEntry* BindCacheFindFrame(u64 key, const Pipeline* pipeline, u64 hash,
                                       u64 targets_key);
    BindCacheState bind_cache_state{};
    u32 bind_cache_live{};
    u64 bind_cache_clock{};
    /// Bumped by GPU-side DMA writes (FillBuffer, CopyBuffer): flushes the cache.
    std::atomic<u64> bind_cache_write_epoch{0};
    bool bind_cache_recording{};   ///< a full bind that may be cached is running
    bool bind_cache_uncacheable{}; ///< ... and did something a hit could not repeat
    std::vector<BindCacheImage> bind_cache_images_rec;
    std::vector<u32> bind_cache_lod_rec;
    std::vector<std::pair<VAddr, u64>> bind_cache_ranges_rec;
    std::vector<BindCacheRepair> bind_cache_repairs_rec;     ///< SOTC_BIND_CACHE_XTICK
    std::vector<BindCacheSampler> bind_cache_samplers_rec;   ///< SOTC_BIND_CACHE_XTICK
    /// SOTC_BIND_CACHE_XTICK, SOTC_FPS only: keys of the last two frames that looked the cache up,
    /// direct-mapped (counter: lookups whose key was also seen in the previous frame).
    struct BindSeenSlot {
        u64 hash{};
        u64 frame_last{};
        u64 frame_prev{};
    };
    std::array<BindSeenSlot, 2048> bind_seen{};
    /// The buffer cache's side of the bind validity checks: BindVersion with
    /// SOTC_BIND_CACHE_XTICK, BindGeneration otherwise.
    u64 BufferBindGeneration() const noexcept {
        return SotcStats::kBindCacheXtick ? buffer_cache.BindVersion()
                                          : buffer_cache.BindGeneration();
    }

    /// sotc: SOTC_RS_MEMO=1 (default off). The render state of the last draw whose targets
    /// PrepareRenderState and BeginRendering looked up in full, with the registers it came from.
    /// A draw with the same target registers (and attachment part of the pipeline key), in the
    /// same submit and the render pass that draw left open, with no image registered or
    /// unregistered, no metadata clear bit set since and its targets still clean, tracked and not
    /// sampled, skips the lookups: PrepareRenderState only marks the targets, BeginRendering
    /// repeats the per-draw side effects (TextureCache::ReuseTarget, the transitions, the hudless
    /// / UI shadow tests) on the stored state. Not stored for a pass with a clear or a feedback loop.
    static constexpr size_t RsMemoKeyWords = 384;
    struct RsMemo {
        std::array<u32, RsMemoKeyWords> key{};
        u32 key_len{};
        bool valid{};
        bool cb0_base{}; ///< color target 0 is mip 0, slice 0 (BeginUiShadow)
        std::array<u8, AmdGpu::NUM_COLOR_BUFFERS> color_samples{}; ///< pipeline key's
        u64 tick{};
        u64 texture_generation{};
        u64 meta_epoch{};
        RenderState state{};
        vk::ImageLayout depth_layout{};
        vk::PipelineStageFlags2 depth_stages{};
        vk::AccessFlags2 depth_access{};
    };
    RsMemo rs_memo{};
    std::array<u32, RsMemoKeyWords> rs_key{}; ///< this draw's (RsMemoBuildKey)
    u32 rs_key_len{};
    bool rs_memo_prepared{}; ///< PrepareRenderState took the memo; BeginRendering checks it again

    /// sotc: SOTC_PARALLEL_RECORD (MapEpoch, PrefetchImagesShared, CheckPrepared).
    std::atomic<u64> map_epoch{0};
    std::mutex prep_prefetch_mutex;
    u64 pr_verify_count{};
    /// sotc: SOTC_PR_RESOLVE + SOTC_PR_VERIFY: the current draw's prepared resources differ from
    /// the inline ones (set by CheckPrepared); BindResources then resolves inline.
    bool pr_verify_off{};

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
