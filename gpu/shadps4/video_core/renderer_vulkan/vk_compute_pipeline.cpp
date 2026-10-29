// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <boost/container/small_vector.hpp>

#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_stats.h"

namespace Vulkan {

ComputePipeline::ComputePipeline(const Instance& instance, Scheduler& scheduler,
                                 DescriptorHeap& desc_heap, LayoutCache& layout_cache,
                                 const Shader::Profile& profile, vk::PipelineCache pipeline_cache,
                                 ComputePipelineKey compute_key_,
                                 const Shader::Info& info_, vk::ShaderModule module_,
                                 SerializationSupport& sdata, bool preloading /*=false*/,
                                 bool deferred /*=false*/)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache, true},
      compute_key{compute_key_}, cache{pipeline_cache}, module{module_}, from_cache{preloading} {
    auto& info = stages[int(Shader::SwStage::Compute)];
    info = &info_;
    const auto debug_str = GetDebugString();

    u32 binding{};
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    for (const auto& buffer : info->buffers) {
        // During deserialization, we don't have access to the UD to fetch sharp data. To address
        // this properly we need to track shaprs or portion of them in `sdata`, but since we're
        // interested only in "is storage" flag (which is not even effective atm), we can take a
        // shortcut there.
        const auto sharp = preloading ? AmdGpu::Buffer{} : buffer.GetSharp(*info);
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }
    for (const auto& image : info->images) {
        const u32 num_bindings = image.NumBindings(*info);
        bindings.push_back({
            .binding = binding,
            .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                               : vk::DescriptorType::eSampledImage,
            .descriptorCount = num_bindings,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
        binding += num_bindings;
    }
    // sotc: SOTC_BINDLESS_SAMP: the samplers come from the global heap (set 1) instead.
    uses_bindless_samplers = Shader::UsesBindlessSamplers(*info);
    for (const auto& sampler : info->samplers) {
        if (uses_bindless_samplers) {
            break;
        }
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }

    const vk::PushConstantRange push_constants = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };

    // sotc: descriptor buffers replace push descriptors (the two layout flags are exclusive).
    const bool descriptor_buffer = instance.IsDescriptorBufferEnabled();
    uses_push_descriptors = !descriptor_buffer && binding < instance.MaxPushDescriptors();
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                       : descriptor_buffer
                           ? vk::DescriptorSetLayoutCreateFlagBits::eDescriptorBufferEXT
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    // Compute has one set, set 0, whatever SOTC_STAGE_BINDINGS says.
    const LayoutCache::SetRequest set_request{
        .bindings = bindings,
        .flags = flags,
    };
    const LayoutCache::Request request{
        .sets = std::span{&set_request, 1},
        .push_constants = push_constants,
        .trailing_set = uses_bindless_samplers ? desc_heap.BindlessSamplerLayout()
                                               : vk::DescriptorSetLayout{},
        .slot_constants = uses_bindless_samplers
                              ? vk::PushConstantRange{
                                    .stageFlags = vk::ShaderStageFlagBits::eCompute,
                                    .offset = sizeof(Shader::PushData),
                                    .size = sizeof(Shader::BindlessSamplerSlots),
                                }
                              : vk::PushConstantRange{},
    };
    const auto layouts = layout_cache.Get(request, owned_layouts);
    set_layouts = layouts.sets;
    desc_layout = set_layouts[0];
    pipeline_layout = layouts.pipeline_layout;
    // sotc: the offsets are per set layout, so every pipeline queries them from the shared handle.
    QueryDescriptorBufferLayout(bindings);
    if (layouts.created) {
        SetObjectName(instance.GetDevice(), pipeline_layout, "Compute PipelineLayout {}",
                      debug_str);
    }

    if (deferred) {
        ready.store(false, std::memory_order_relaxed);
        return;
    }
    CreatePipeline();
}

ComputePipeline::~ComputePipeline() = default;

void ComputePipeline::CreatePipeline() {
    // sotc: 64 where every stage can be pinned to it, otherwise 32 when the device could pick a
    // wider size (AMD): the shaders were lowered for 32 lanes (vk_instance.h). On Intel the
    // recompiler's lane count (Instance::RequiredSubgroupSize).
    const u32 required_size = instance.RequiredSubgroupSize(vk::ShaderStageFlagBits::eCompute);
    const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci = {
        .requiredSubgroupSize = required_size,
    };
    const bool require_size = required_size != 0;
    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .pNext = require_size ? &subgroup_size_ci : nullptr,
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
    };
    // sotc: pipelines whose layouts are descriptor buffer layouts carry this flag.
    const vk::PipelineCreateFlags pipeline_flags =
        instance.IsDescriptorBufferEnabled()
            ? vk::PipelineCreateFlags(vk::PipelineCreateFlagBits::eDescriptorBufferEXT)
            : vk::PipelineCreateFlags{};
    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .flags = pipeline_flags,
        .stage = shader_ci,
        .layout = pipeline_layout,
    };
    auto [pipeline_result, pipe] =
        instance.GetDevice().createComputePipelineUnique(cache, compute_pipeline_ci);
    if (pipeline_result != vk::Result::eSuccess && from_cache) {
        // sotc: as for graphics pipelines, a cached one the driver refuses is dropped.
        LOG_WARNING(Render_Vulkan, "Cached compute pipeline {} failed to build: {}", GetDebugString(),
                    vk::to_string(pipeline_result));
        return;
    }
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create compute pipeline: {}",
               vk::to_string(pipeline_result));
    pipeline = std::move(pipe);
    SetObjectName(instance.GetDevice(), *pipeline, "Compute Pipeline {}", GetDebugString());
}

void ComputePipeline::Build() {
    if (claimed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    CreatePipeline();
    ready.store(true, std::memory_order_release);
    ready.notify_all();
}

void ComputePipeline::WaitBuilt() {
    if (!claimed.load(std::memory_order_acquire)) {
        Build();
    }
    ready.wait(false, std::memory_order_acquire);
}

void ComputePipeline::BindWhenReady() const {
    if (ready.load(std::memory_order_acquire)) {
        scheduler.CommandBuffer().bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
        return;
    }
    scheduler.RecordDeferred([this](vk::CommandBuffer cmdbuf) {
        if (!ready.load(std::memory_order_acquire)) {
            const auto start = std::chrono::steady_clock::now();
            if (!claimed.load(std::memory_order_acquire)) {
                const_cast<ComputePipeline*>(this)->Build();
                SotcStats::async_compute_self_built.fetch_add(1, std::memory_order_relaxed);
            }
            ready.wait(false, std::memory_order_acquire);
            SotcStats::async_compute_wait_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count(),
                std::memory_order_relaxed);
            SotcStats::async_compute_waits.fetch_add(1, std::memory_order_relaxed);
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    });
}

} // namespace Vulkan
