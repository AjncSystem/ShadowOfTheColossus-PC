// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <boost/container/static_vector.hpp>

#include "common/assert.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_stats.h"

namespace Vulkan {

// sotc: SOTC_DEDUP_PUSH=1 skips pushes of unchanged constants (the cache in DynamicState,
// vk_scheduler.h). Off by default: host passes (FSR, DLSS-G, detiler, fault manager) push their
// own constants on the same command buffer and must void the cache (InvalidatePushConstants);
// a missed one would leave a draw with another pass's constants. SOTC_DEDUP_STATE=0 also
// turns it off.
static const bool kDedupState = [] {
    const char* all = std::getenv("SOTC_DEDUP_STATE");
    const char* push = std::getenv("SOTC_DEDUP_PUSH");
    return !(all && all[0] == '0') && push && push[0] == '1';
}();

// sotc: off by default (new, untested in game): SOTC_LAYOUT_CACHE=1 turns it on, and
// SOTC_STAGE_BINDINGS=1 turns it on too (per-stage set layouts are meant to be shared), unless
// SOTC_LAYOUT_CACHE=0 says otherwise.
static bool LayoutCacheEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_LAYOUT_CACHE");
        if (v) {
            return v[0] == '1';
        }
        const char* stage = std::getenv("SOTC_STAGE_BINDINGS");
        return stage && stage[0] == '1';
    }();
    return enabled;
}

LayoutCache::LayoutCache(const Instance& instance_)
    : instance{instance_}, enabled{LayoutCacheEnabled()} {}

LayoutCache::~LayoutCache() {
    if (SotcStats::kCmdStats) {
        std::fprintf(stderr, "SOTC layouts: %llu created, %llu reused%s\n",
                     static_cast<unsigned long long>(num_created),
                     static_cast<unsigned long long>(num_reused),
                     enabled ? "" : " (cache off)");
    }
}

LayoutCache::Key LayoutCache::MakeSetKey(const SetRequest& set) {
    Key key;
    key.reserve(2 + set.bindings.size() * 4);
    key.push_back(static_cast<u64>(static_cast<VkFlags>(set.flags)));
    key.push_back(set.bindings.size());
    for (const auto& binding : set.bindings) {
        key.push_back(binding.binding);
        key.push_back(static_cast<u64>(binding.descriptorType));
        key.push_back(binding.descriptorCount);
        key.push_back(static_cast<u64>(static_cast<VkFlags>(binding.stageFlags)));
    }
    return key;
}

vk::UniqueDescriptorSetLayout LayoutCache::CreateSetLayout(const SetRequest& set) {
    const vk::DescriptorSetLayoutCreateInfo layout_ci = {
        .flags = set.flags,
        .bindingCount = static_cast<u32>(set.bindings.size()),
        .pBindings = set.bindings.data(),
    };
    auto [result, layout] = instance.GetDevice().createDescriptorSetLayoutUnique(layout_ci);
    ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create descriptor set layout: {}",
               vk::to_string(result));
    return std::move(layout);
}

vk::UniquePipelineLayout LayoutCache::CreatePipelineLayout(
    std::span<const vk::DescriptorSetLayout> sets, std::span<const vk::PushConstantRange> push) {
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = static_cast<u32>(sets.size()),
        .pSetLayouts = sets.data(),
        .pushConstantRangeCount = static_cast<u32>(push.size()),
        .pPushConstantRanges = push.data(),
    };
    auto [result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create pipeline layout: {}",
               vk::to_string(result));
    return std::move(layout);
}

vk::DescriptorSetLayout LayoutCache::FindSetLayout(const SetRequest& set) {
    auto [it, is_new] = set_entries.try_emplace(MakeSetKey(set));
    if (is_new) {
        it.value() = CreateSetLayout(set);
    }
    return *it.value();
}

LayoutCache::Layouts LayoutCache::Get(const Request& request, Owned& owned) {
    std::scoped_lock lock{mutex};
    ASSERT_MSG(request.sets.size() <= Shader::MaxStageDescriptorSets,
               "Pipeline layout with {} sets", request.sets.size());
    Layouts result;
    // sotc: the bindless slots are a second range after PushData (SOTC_BINDLESS_SAMP only).
    const std::array<vk::PushConstantRange, 2> ranges{request.push_constants, request.slot_constants};
    const std::span<const vk::PushConstantRange> push_ranges{
        ranges.data(), request.slot_constants.size != 0 ? 2u : 1u};
    if (!enabled) {
        owned.sets.clear();
        for (const auto& set : request.sets) {
            owned.sets.push_back(CreateSetLayout(set));
            result.sets.push_back(*owned.sets.back());
        }
        if (request.trailing_set) {
            result.sets.push_back(request.trailing_set);
        }
        owned.pipeline_layout = CreatePipelineLayout(result.sets, push_ranges);
        result.pipeline_layout = *owned.pipeline_layout;
        result.created = true;
        ++num_created;
        return result;
    }
    // The pipeline layout is keyed by its set layouts' handles, so equal sets are shared too.
    Key key;
    key.reserve(request.sets.size() + 4);
    for (const auto& set : request.sets) {
        result.sets.push_back(FindSetLayout(set));
        key.push_back(std::bit_cast<u64>(result.sets.back()));
    }
    if (request.trailing_set) {
        result.sets.push_back(request.trailing_set);
        key.push_back(std::bit_cast<u64>(request.trailing_set));
    }
    key.push_back(static_cast<u64>(static_cast<VkFlags>(request.push_constants.stageFlags)));
    key.push_back(request.push_constants.offset);
    key.push_back(request.push_constants.size);
    if (request.slot_constants.size != 0) {
        key.push_back(static_cast<u64>(static_cast<VkFlags>(request.slot_constants.stageFlags)));
        key.push_back(request.slot_constants.offset);
        key.push_back(request.slot_constants.size);
    }
    auto [it, is_new] = pipeline_entries.try_emplace(std::move(key));
    if (is_new) {
        it.value() = CreatePipelineLayout(result.sets, push_ranges);
        ++num_created;
    } else {
        ++num_reused;
    }
    result.pipeline_layout = *it.value();
    result.created = is_new;
    return result;
}

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {
    stage_sets = !is_compute && profile.stage_bindings;
}

Pipeline::~Pipeline() = default;

void Pipeline::QueryDescriptorBufferLayout(
    std::span<const vk::DescriptorSetLayoutBinding> bindings) {
    if (!instance.IsDescriptorBufferEnabled()) {
        return;
    }
    const vk::Device device = instance.GetDevice();
    desc_layout_size = device.getDescriptorSetLayoutSizeEXT(desc_layout);
    u32 num_slots = 0;
    for (const auto& binding : bindings) {
        num_slots = std::max(num_slots, binding.binding + 1);
    }
    desc_binding_offsets.assign(num_slots, 0);
    for (const auto& binding : bindings) {
        desc_binding_offsets[binding.binding] =
            device.getDescriptorSetLayoutBindingOffsetEXT(desc_layout, binding.binding);
    }
}

namespace {

/// sotc: size in bytes of one descriptor of this type in a descriptor buffer.
size_t DescriptorBufferSize(const vk::PhysicalDeviceDescriptorBufferPropertiesEXT& props,
                            vk::DescriptorType type) {
    switch (type) {
    case vk::DescriptorType::eSampler:
        return props.samplerDescriptorSize;
    case vk::DescriptorType::eSampledImage:
        return props.sampledImageDescriptorSize;
    case vk::DescriptorType::eStorageImage:
        return props.storageImageDescriptorSize;
    case vk::DescriptorType::eStorageBuffer:
        return props.storageBufferDescriptorSize;
    default:
        UNREACHABLE_MSG("Descriptor type {} has no descriptor buffer layout",
                        vk::to_string(type));
    }
}

/// sotc: encodes element `element` of a write into dst through vkGetDescriptorEXT. A null buffer
/// becomes a null descriptor (address 0), which nullDescriptor makes valid.
void WriteDescriptorElement(vk::Device device, const vk::WriteDescriptorSet& write, u32 element,
                            void* dst, size_t size) {
    VkDescriptorGetInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
        .type = static_cast<VkDescriptorType>(write.descriptorType),
    };
    VkDescriptorAddressInfoEXT address{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT,
    };
    VkDescriptorImageInfo image{};
    switch (write.descriptorType) {
    case vk::DescriptorType::eStorageBuffer: {
        const vk::DescriptorBufferInfo& buffer = write.pBufferInfo[element];
        if (buffer.buffer != vk::Buffer{}) {
            address.address =
                device.getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = buffer.buffer}) +
                buffer.offset;
            address.range = buffer.range;
        }
        info.data.pStorageBuffer = &address;
        break;
    }
    case vk::DescriptorType::eSampler:
        image = write.pImageInfo[element];
        info.data.pSampler = &image.sampler;
        break;
    case vk::DescriptorType::eSampledImage:
        image = write.pImageInfo[element];
        info.data.pSampledImage = &image;
        break;
    case vk::DescriptorType::eStorageImage:
        image = write.pImageInfo[element];
        info.data.pStorageImage = &image;
        break;
    default:
        UNREACHABLE_MSG("Descriptor type {} has no descriptor buffer encoding",
                        vk::to_string(write.descriptorType));
    }
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDescriptorEXT(static_cast<VkDevice>(device), &info, size,
                                                     dst);
}

} // Anonymous namespace

void Pipeline::BindDescriptorBuffer(const DescriptorWrites& set_writes,
                                    vk::PipelineBindPoint bind_point) const {
    DescriptorBufferRing& ring = *desc_heap.GetDescriptorBuffer();
    const auto& props = instance.GetDescriptorBufferProperties();
    const vk::Device device = instance.GetDevice();

    // One set per draw, written at its binding offsets inside the ring region.
    const auto alloc = ring.Allocate(desc_layout_size, props.descriptorBufferOffsetAlignment);
    for (const auto& write : set_writes) {
        ASSERT_MSG(write.dstBinding < desc_binding_offsets.size(),
                   "Descriptor binding {} is not in the set layout", write.dstBinding);
        const size_t size = DescriptorBufferSize(props, write.descriptorType);
        u8* base = alloc.mapped + desc_binding_offsets[write.dstBinding] +
                   write.dstArrayElement * size;
        for (u32 i = 0; i < write.descriptorCount; ++i) {
            WriteDescriptorElement(device, write, i, base + i * size, size);
        }
    }
    ring.Flush(alloc.offset, desc_layout_size);

    const auto cmdbuf = scheduler.CommandBuffer();
    // The buffer is bound once per command buffer, before its first offset is set.
    if (ring.NeedsBinding(scheduler.SessionSerial())) {
        const vk::DescriptorBufferBindingInfoEXT binding_info = {
            .address = ring.Address(),
            .usage = vk::BufferUsageFlagBits::eResourceDescriptorBufferEXT |
                     vk::BufferUsageFlagBits::eSamplerDescriptorBufferEXT,
        };
        cmdbuf.bindDescriptorBuffersEXT(1, &binding_info);
    }
    const u32 buffer_index = 0;
    cmdbuf.setDescriptorBufferOffsetsEXT(bind_point, pipeline_layout, 0, 1, &buffer_index,
                                         &alloc.offset);
}

void Pipeline::BindResources(DescriptorWrites& set_writes, std::span<const u32> write_sets,
                             const Shader::PushData& push_data,
                             const Shader::BindlessSamplerSlots& bindless_slots) const {
    const auto cmdbuf = scheduler.CommandBuffer();
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;

    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;

    // sotc: skip the push when this command buffer already holds these bytes for this layout and
    // stages. The cache is voided with the generation of DynamicState: BeginSession (a new
    // command buffer) and every Invalidate() call (other pipelines bound on it).
    auto& dynamic = scheduler.GetDynamicState();
    auto& cache = dynamic.push_constant_cache;
    const auto size = static_cast<u32>(sizeof(push_data));
    static_assert(sizeof(Shader::PushData) <= DynamicState::PushConstantCache::MaxBytes);
    const bool same = kDedupState && cache.generation == dynamic.generation &&
                      cache.layout == pipeline_layout && cache.stages == stage_flags &&
                      cache.size == size && std::memcmp(cache.bytes.data(), &push_data, size) == 0;
    if (!same) {
        cmdbuf.pushConstants(pipeline_layout, stage_flags, 0u, sizeof(push_data), &push_data);
        if (kDedupState) {
            cache.generation = dynamic.generation;
            cache.layout = pipeline_layout;
            cache.stages = stage_flags;
            cache.size = size;
            std::memcpy(cache.bytes.data(), &push_data, size);
        }
    }
    // sotc: SOTC_BINDLESS_SAMP: the sampler slots, after PushData (the second range of the layout).
    if (uses_bindless_samplers) {
        cmdbuf.pushConstants(pipeline_layout, stage_flags, sizeof(push_data), sizeof(bindless_slots),
                             &bindless_slots);
    }

    // sotc: SOTC_BINDLESS_SAMP: the global sampler set is bound beside set 0, before any early out.
    if (uses_bindless_samplers) {
        const auto bindless_set = desc_heap.BindlessSamplerSet();
        cmdbuf.bindDescriptorSets(bind_point, pipeline_layout, 1, 1, &bindless_set, 0, nullptr);
    }

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    if (stage_sets) {
        BindStageSets(set_writes, write_sets, bind_point);
        return;
    }

    if (uses_push_descriptors) {
        cmdbuf.pushDescriptorSetKHR(bind_point, pipeline_layout, 0, set_writes);
        return;
    }

    // sotc: SOTC_DESC_BUFFER=1, the set is written into the descriptor buffer ring instead.
    if (instance.IsDescriptorBufferEnabled()) {
        BindDescriptorBuffer(set_writes, bind_point);
        return;
    }

    const auto desc_set = desc_heap.Commit(desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    cmdbuf.bindDescriptorSets(bind_point, pipeline_layout, 0, desc_set, {});
}

void Pipeline::BindStageSets(DescriptorWrites& set_writes, std::span<const u32> write_sets,
                             vk::PipelineBindPoint bind_point) const {
    ASSERT_MSG(write_sets.size() == set_writes.size(), "Descriptor write without a set");
    const auto cmdbuf = scheduler.CommandBuffer();
    const u32 num_sets = static_cast<u32>(set_layouts.size());
    std::array<vk::DescriptorSet, Shader::MaxStageDescriptorSets> desc_sets{};
    for (u32 set = 0; set < num_sets; ++set) {
        desc_sets[set] = desc_heap.Commit(set_layouts[set]);
    }
    for (size_t i = 0; i < set_writes.size(); ++i) {
        ASSERT_MSG(write_sets[i] < num_sets, "Descriptor set {} is not in the layout",
                   write_sets[i]);
        set_writes[i].dstSet = desc_sets[write_sets[i]];
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    cmdbuf.bindDescriptorSets(bind_point, pipeline_layout, 0, num_sets, desc_sets.data(), 0,
                              nullptr);
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
