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
                                 DescriptorHeap& desc_heap, const Shader::Profile& profile,
                                 vk::PipelineCache pipeline_cache, ComputePipelineKey compute_key_,
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
    for (const auto& sampler : info->samplers) {
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

    uses_push_descriptors = binding < instance.MaxPushDescriptors();
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    const auto device = instance.GetDevice();
    auto [descriptor_set_result, descriptor_set] =
        device.createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(descriptor_set_result == vk::Result::eSuccess,
               "Failed to create compute descriptor set layout: {}",
               vk::to_string(descriptor_set_result));
    desc_layout = std::move(descriptor_set);

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create compute pipeline layout: {}", vk::to_string(layout_result));
    pipeline_layout = std::move(layout);
    SetObjectName(device, *pipeline_layout, "Compute PipelineLayout {}", debug_str);

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
    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage = shader_ci,
        .layout = *pipeline_layout,
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
