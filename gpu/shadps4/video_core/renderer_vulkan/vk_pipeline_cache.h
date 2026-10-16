// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <atomic>
#include <chrono>

#include <memory>
#include <mutex>
#include <vector>
#include <optional>
#include <variant>
#include <tsl/robin_map.h>
#include "video_core/renderer_vulkan/vk_async_compiler.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "vulkan/vulkan.hpp"

template <>
struct std::hash<vk::ShaderModule> {
    std::size_t operator()(const vk::ShaderModule& module) const noexcept {
        return std::hash<size_t>{}(reinterpret_cast<size_t>((VkShaderModule)module));
    }
};

namespace AmdGpu {
class Liverpool;
union Regs;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

class Instance;
class Scheduler;
class ShaderCache;
class DrawPrep;
struct PreparedDraw;

struct Program {
    struct Module {
        vk::ShaderModule module;
        Shader::StageSpecialization spec;
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    Shader::Info info;
    ModuleList modules{};

    /// sotc: recent (flattened user data, program base, runtime info, start bindings) inputs and
    /// the permutation they selected. StageSpecialization is a pure function of these for
    /// stages without a fetch shader or tessellation constants, so a match skips building it.
    struct SpecMemo {
        std::vector<u32> flat;
        VAddr pgm_base{};
        Shader::RuntimeInfo runtime_info{};
        Shader::Backend::Bindings start{};
        u32 perm_idx{};
        std::vector<AmdGpu::Buffer> vsharps; ///< V# of each fetch attribute (vertex shaders)
        bool valid{};
        u64 fill_seq{}; ///< changes whenever the entry is refilled (SOTC_SEL_FAST)
    };
    std::array<SpecMemo, 4> spec_memos{};
    u32 next_spec_memo{};
    /// sotc: permutations already published to the draw preparation helper (DrawPrep).
    u32 prep_published{};
    /// sotc: the flattened dwords its specialization reads (draw preparation validation).
    std::vector<u16> prep_spec_dwords;
    bool prep_spec_dwords_ready{};

    Program() = default;
    Program(Shader::HwStage stage, Shader::SwStage l_stage, Shader::ShaderParams params)
        : info{stage, l_stage, params} {}

    void AddPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec) {
        modules.emplace_back(module, std::move(spec));
    }

    void InsertPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                      size_t perm_idx) {
        modules.resize(std::max(modules.size(), perm_idx + 1)); // <-- beware of realloc
        modules[perm_idx] = {module, std::move(spec)};
    }
};

struct DrawIndirectParams {
    u16 vertex_sgpr_offset;
    u32 instance_sgpr_offset;
};

class PipelineCache {
public:
    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           AmdGpu::Liverpool* liverpool, u32 sparse_page_shift);
    ~PipelineCache();

    void WarmUp();
    void Sync();

    bool LoadComputePipeline(Serialization::Archive& ar);
    bool LoadGraphicsPipeline(Serialization::Archive& ar);
    bool LoadPipelineStage(Serialization::Archive& ar, size_t stage);

    /// With allow_async, a pipeline that is not built yet is queued on the compiler workers and
    /// nullptr is returned with LastWasPending() set; the caller then either skips the draw or
    /// calls WaitPendingGraphics(). SOTC_ASYNC_PIPELINES=0 disables it.
    const GraphicsPipeline* GetGraphicsPipeline(const DrawIndirectParams params = {},
                                                bool allow_async = false);
    [[nodiscard]] bool LastWasPending() const noexcept {
        return last_pending != nullptr;
    }
    /// Waits for the pipeline the last GetGraphicsPipeline call left pending.
    const GraphicsPipeline* WaitPendingGraphics();
    /// The pending job is older than the frame/time limits: the draw must wait for it.
    [[nodiscard]] bool PendingTooOld() const;
    /// A stage of the current draw writes buffers or storage images (results feed readbacks).
    [[nodiscard]] bool StagesWriteMemory() const;
    /// Once per guest submit: takes the driver cache checkpoint when compiling has settled.
    void OnFrame();
    [[nodiscard]] std::span<const Shader::Info* const, MaxShaderStages> CurrentInfos() const {
        return infos;
    }

    const ComputePipeline* GetComputePipeline();

    using Result = std::tuple<const Shader::Info*, vk::ShaderModule, u64>;
    Result GetProgram(Shader::HwStage hw_stage, Shader::SwStage sw_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding);

    std::optional<vk::ShaderModule> ReplaceShader(vk::ShaderModule module,
                                                  std::span<const u32> spv_code);

    static std::string GetShaderName(Shader::HwStage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

private:
    struct PendingGraphics {
        GraphicsPipelineKey key{};
        AsyncJob<std::unique_ptr<GraphicsPipeline>> job;
        std::array<std::unique_ptr<Shader::Info>, MaxShaderStages> info_copy;
        std::array<std::vector<u32>, MaxShaderStages> user_data;
        std::array<const Shader::Info*, MaxShaderStages> snap_infos{};
        std::array<const Shader::Info*, MaxShaderStages> live_infos{};
        std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
        std::array<vk::ShaderModule, MaxShaderStages> modules{};
        std::optional<Shader::Gcn::FetchShaderData> fetch;
        GraphicsPipeline::SerializationSupport sdata{};
        u64 queued_flip{};
        std::chrono::steady_clock::time_point queued{};
    };
    struct PendingCompute {
        ComputePipelineKey key{};
        AsyncJob<std::unique_ptr<ComputePipeline>> job;
        std::unique_ptr<Shader::Info> info_copy;
        std::vector<u32> user_data;
        const Shader::Info* live_info{};
        vk::ShaderModule module{};
        ComputePipeline::SerializationSupport sdata{};
    };
    /// Swaps in the optimized builds of compute pipelines first created unoptimized.
    void PublishCompute();
    bool AsyncGraphicsEnabled() const;
    PendingGraphics* EnqueueGraphics();
    void PublishGraphics(bool wait_all);

    void InitializeDriverCache();
    void SaveDriverCache(bool force = false);
    bool RefreshGraphicsKey();
    bool RefreshGraphicsStages();
    bool RefreshComputeKey();

    void DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage, size_t perm_idx,
                    std::string_view ext);
    /// A patch is SPIR-V edited from the module the recompiler generated on one GPU. When a
    /// "<patch>.base" file holds the XXH3 hash of that module, the patch only applies while the
    /// generated module still matches it (other GPUs/drivers can get a different module).
    std::optional<std::vector<u32>> GetShaderPatch(u64 hash, Shader::HwStage stage, size_t perm_idx,
                                                   std::string_view ext,
                                                   std::span<const u32> generated);
    vk::ShaderModule CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                   const std::span<const u32>& code, size_t perm_idx,
                                   Shader::Backend::Bindings& binding);
    const Shader::RuntimeInfo& BuildRuntimeInfo(Shader::HwStage stage, Shader::SwStage l_stage);

    /// sotc: parts of the graphics selection shared with the draw preparation helper
    /// (vk_draw_prep.h), which runs them on its own copy of the registers.
    friend class DrawPrep;
    static void KeyTargetsFirstPass(const AmdGpu::Regs& regs, GraphicsPipelineKey& key);
    void KeyTargetsSecondPass(const AmdGpu::Regs& regs, GraphicsPipelineKey& key) const;
    void FillHwVertexInfo(Shader::RuntimeInfo& info, const AmdGpu::Regs& regs) const;
    void FillHwFragmentInfo(Shader::RuntimeInfo& info, const AmdGpu::Regs& regs,
                            const GraphicsPipelineKey& key) const;
    static void FillSwVertexInfo(Shader::RuntimeInfo& info, const AmdGpu::Regs& regs,
                                 const DrawIndirectParams& params);
    /// Command processor: validates a helper's proposal and, if it holds, leaves the state
    /// RefreshGraphicsKey would (vk_draw_prep.cpp).
    bool TryPreparedSelection(const PreparedDraw& prepared, const DrawIndirectParams& params);

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

private:
    const Instance& instance;
    Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    u64 last_sel_regs_seq{};

    /// sotc (SOTC_SEL_FAST, thread-plan 1D): the last graphics selection, reused while no
    /// register selection reads has changed (Liverpool::sel_regs_seq), the draw's indirect
    /// parameters are equal, and every guest-memory input it read is equal again: shader code
    /// hash, flattened user data (walker re-run here, on the command processor), fetch shader
    /// and vertex V#s, compared against the exact SpecMemo entry that selection used.
    struct FastStage {
        Program* program{};
        u64 hash{};
        u32 memo_idx{};
        u64 memo_fill_seq{};
        Shader::Backend::Bindings start{};
        Shader::HwStage hw{};
        bool fetch_vs{};
    };
    struct FastSelection {
        bool valid{};
        u64 regs_seq{};
        DrawIndirectParams params{};
        u32 stage_mask{};
        std::array<FastStage, MaxShaderStages> stages{};
        GraphicsPipelineKey key{};
        std::array<const Shader::Info*, MaxShaderStages> infos{};
        std::array<vk::ShaderModule, MaxShaderStages> modules{};
    };
    struct LastProgramHit {
        Program* program{};
        u32 memo_idx{};
        u64 fill_seq{};
        bool usable{};
    };
    FastSelection fast_sel{};
    FastSelection fast_build{};
    LastProgramHit last_hit{};
    u64 memo_fill_counter{};
    u64 fast_commits{};
    /// SOTC_SEL_FAST=2: a register file seen by one committed selection in 64.
    std::vector<u32> fast_regs_snapshot;
    u64 fast_regs_snapshot_seq{~u64{0}};
    bool TryFastSelection(const DrawIndirectParams& params);
    void CommitFastSelection(u64 regs_seq);
    void VerifyFastSelection(const GraphicsPipelineKey& fast_key,
                             const std::array<const Shader::Info*, MaxShaderStages>& fast_infos,
                             const std::array<vk::ShaderModule, MaxShaderStages>& fast_modules,
                             const Shader::Gcn::FetchShaderData* fast_fetch,
                             bool prepared = false);
    /// The program the last GetProgram call used (published to the draw preparation helper).
    Program* got_program{};
    DescriptorHeap desc_heap;
    vk::UniquePipelineCache pipeline_cache;
    vk::UniquePipelineLayout pipeline_layout;
    Shader::Profile profile{};
    Shader::Pools pools;
    DrawIndirectParams draw_indirect_params{};
    tsl::robin_map<size_t, std::unique_ptr<Program>> program_cache;
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    Shader::Gcn::FetchShaderData* fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    ComputePipelineKey compute_key{};
    u32 num_new_pipelines{}; // new pipelines added to the cache since the game start
    std::unique_ptr<AsyncCompiler> async_compiler;
    /// One VkPipelineCache per compiler worker, merged into pipeline_cache at checkpoints
    /// (SOTC_WORKER_PIPELINE_CACHES=0: workers share pipeline_cache). Added to keep workers
    /// from contending with the command processor on one cache object; a later test with an
    /// isolated NVIDIA shader cache did not show that contention, so it is a precaution.
    std::vector<vk::UniquePipelineCache> worker_caches;
    std::atomic<bool> snapshot_in_flight{false};
    vk::PipelineCache WorkerCache() const {
        return worker_caches.empty() ? *pipeline_cache
                                     : *worker_caches[AsyncCompiler::WorkerIndex()];
    }
    /// False when skipped because a snapshot is reading pipeline_cache.
    bool MergeWorkerCaches();
    /// sotc: parallel warm-up. Cached pipelines are built on these threads while the store is
    /// read; results are inserted on the calling thread when the walk ends.
    std::unique_ptr<AsyncCompiler> warmup_pool;
    std::mutex warmup_mutex;
    std::vector<std::pair<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>>> warmup_graphics;
    std::vector<std::pair<ComputePipelineKey, std::unique_ptr<ComputePipeline>>> warmup_compute;
    tsl::robin_map<GraphicsPipelineKey, char> warmup_graphics_keys;
    tsl::robin_map<ComputePipelineKey, char> warmup_compute_keys;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<PendingGraphics>> pending_graphics;
    PendingGraphics* last_pending{};
    GraphicsPipelineKey last_graphics_key{};
    const GraphicsPipeline* last_graphics{};
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<PendingCompute>> pending_compute;
    /// Optimized compute builds wait for a quiet moment: started during a burst they compete
    /// with the burst's own (unoptimized) creations inside the driver.
    std::vector<std::unique_ptr<PendingCompute>> deferred_compute;
    void StartDeferredCompute();
    std::string driver_cache_name;
    u32 driver_saved_pipelines{};
    std::chrono::steady_clock::time_point last_new_pipeline_time{};
    std::chrono::steady_clock::time_point driver_save_time{};

    // Only if Config::collectShadersForDebug()
    tsl::robin_map<vk::ShaderModule,
                   std::vector<std::variant<GraphicsPipelineKey, ComputePipelineKey>>>
        module_related_pipelines;

    /// sotc (SOTC_DRAW_PREP): last member, destroyed first (it points into the programs).
    std::unique_ptr<DrawPrep> draw_prep;
};

} // namespace Vulkan
