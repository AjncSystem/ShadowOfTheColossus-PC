// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <future>
#include <optional>
#include <variant>
#include <tsl/robin_map.h>
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

struct Program {
    struct Module {
        vk::ShaderModule module;
        Shader::StageSpecialization spec;
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    Shader::Info info;
    ModuleList modules{};

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

    const GraphicsPipeline* GetGraphicsPipeline(const DrawIndirectParams params = {});

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

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

private:
    const Instance& instance;
    Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
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

    // Only if Config::collectShadersForDebug()
    tsl::robin_map<vk::ShaderModule,
                   std::vector<std::variant<GraphicsPipelineKey, ComputePipelineKey>>>
        module_related_pipelines;

    // sotc: graphics pipelines compiled on a worker thread (Intel; GetGraphicsPipeline). Their
    // graphics_pipelines entry stays empty, and the draws using them are skipped, until done.
    // Declared last: destroying it waits for the jobs, which use the members above.
    // finished_jobs counts jobs done since InstallFinishedPipelines last looked.
    std::atomic<u32> finished_jobs{0};
    struct PendingGraphicsPipeline {
        std::array<Shader::Info, MaxShaderStages> info_copies{};
        std::array<const Shader::Info*, MaxShaderStages> live_infos{};
        std::array<const Shader::Info*, MaxShaderStages> copy_ptrs{};
        std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
        std::array<vk::ShaderModule, MaxShaderStages> modules{};
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader;
        GraphicsPipeline::SerializationSupport sdata{};
        std::unique_ptr<GraphicsPipeline> pipeline;
        size_t hash{};
        std::atomic<bool> done{false};
        std::future<void> job;
    };
    // sotc: during WarmUp on Intel, Load*Pipeline queue the pipeline builds here instead of
    // building them one after another; WarmUp runs them on several threads (build), then
    // installs them in the maps on this thread (install).
    struct PreloadJob {
        std::function<void()> build;
        std::function<void()> install;
    };
    std::vector<PreloadJob>* preload_jobs{};
    bool AsyncGraphicsPipelines() const;
    void StartGraphicsPipeline(size_t pipeline_hash);
    const GraphicsPipeline* FinishGraphicsPipeline();
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<PendingGraphicsPipeline>> pending_graphics;

    // sotc: the same for compute pipelines (their dispatches are skipped until done).
    struct PendingComputePipeline {
        Shader::Info info_copy{};
        const Shader::Info* live_info{};
        vk::ShaderModule module{};
        ComputePipeline::SerializationSupport sdata{};
        std::unique_ptr<ComputePipeline> pipeline;
        std::atomic<bool> done{false};
        std::future<void> job;
    };
    bool AsyncComputePipelines() const;
    void StartComputePipeline();
    const ComputePipeline* FinishComputePipeline();
    void InstallFinishedPipelines();
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<PendingComputePipeline>> pending_compute;
};

} // namespace Vulkan
