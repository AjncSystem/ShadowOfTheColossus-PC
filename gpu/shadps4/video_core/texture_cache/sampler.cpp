// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_dlssg.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/texture_cache/sampler.h"

namespace VideoCore {

// sotc: with DLSS super resolution, textures are sampled as sharp as at the output size
// (NVIDIA: mip bias log2(render height / output height)). SOTC_MIP_BIAS overrides. Without
// SOTC_RENDER_RES the game renders at 1080p (sotc.py leaves it unset for 1920x1080).
static float UpscalerMipBias() {
    static const float bias = [] {
        if (const char* v = std::getenv("SOTC_MIP_BIAS"); v && *v) {
            return static_cast<float>(std::atof(v));
        }
        unsigned rw = 0, rh = 0, ow = 0, oh = 0;
        const char* r = std::getenv("SOTC_RENDER_RES");
        const char* o = std::getenv("SOTC_OUTPUT_RES");
        const char* neo = std::getenv("SOTC_NEO");
        if ((!r || !*r) && !(neo && neo[0] == '1')) { // PS4 Pro: its own sizes
            r = "1920x1080";
        }
        if (!Vulkan::Dlssg::SuperResolutionAvailable() || !o ||
            std::sscanf(r, "%ux%u", &rw, &rh) != 2 || std::sscanf(o, "%ux%u", &ow, &oh) != 2 ||
            rh == 0 || oh <= rh) {
            return 0.0f;
        }
        return std::log2(static_cast<float>(rh) / static_cast<float>(oh));
    }();
    static bool reported = false;
    if (bias != 0.0f && !std::exchange(reported, true)) {
        std::fprintf(stderr, "SOTC DLSS: texture mip bias %.2f\n", bias);
    }
    return bias;
}

Sampler::Sampler(const Vulkan::Instance& instance, const AmdGpu::Sampler& sampler,
                 const AmdGpu::BorderColorBuffer border_color_base, const bool is_depth) {
    using namespace Vulkan;
    const bool anisotropy_enable = instance.IsAnisotropicFilteringSupported() &&
                                   (AmdGpu::IsAnisoFilter(sampler.xy_mag_filter) ||
                                    AmdGpu::IsAnisoFilter(sampler.xy_min_filter));
    const float max_anisotropy =
        anisotropy_enable ? std::clamp(sampler.MaxAniso(), 1.0f, instance.MaxSamplerAnisotropy())
                          : 1.0f;
    auto border_color = LiverpoolToVK::BorderColor(sampler.border_color_type);
    if (border_color == vk::BorderColor::eFloatCustomEXT &&
        !instance.IsCustomBorderColorSupported()) {
        LOG_WARNING(Render_Vulkan, "Custom border color is not supported, falling back to black");
        border_color = vk::BorderColor::eFloatOpaqueBlack;
    }

    const auto custom_color = [&]() -> std::optional<vk::SamplerCustomBorderColorCreateInfoEXT> {
        if (border_color == vk::BorderColor::eFloatCustomEXT) {
            const auto border_color_index = sampler.border_color_ptr.Value();
            const auto border_color_buffer = border_color_base.Address<std::array<float, 4>*>();
            const auto custom_border_color_array = border_color_buffer[border_color_index];

            const vk::SamplerCustomBorderColorCreateInfoEXT ret{
                .customBorderColor =
                    vk::ClearColorValue{
                        .float32 = custom_border_color_array,
                    },
                .format = vk::Format::eR32G32B32A32Sfloat,
            };
            return ret;
        } else {
            return std::nullopt;
        }
    }();

    const float upscaler_bias = is_depth ? 0.0f : UpscalerMipBias(); // sotc: DLSS
    const vk::SamplerCreateInfo sampler_ci = {
        .pNext = custom_color ? &*custom_color : nullptr,
        .magFilter = LiverpoolToVK::Filter(sampler.xy_mag_filter),
        .minFilter = LiverpoolToVK::Filter(sampler.xy_min_filter),
        .mipmapMode = LiverpoolToVK::MipFilter(sampler.mip_filter),
        .addressModeU = LiverpoolToVK::ClampMode(sampler.clamp_x),
        .addressModeV = LiverpoolToVK::ClampMode(sampler.clamp_y),
        .addressModeW = LiverpoolToVK::ClampMode(sampler.clamp_z),
        .mipLodBias = upscaler_bias == 0.0f
                          ? std::min(sampler.LodBias(), instance.MaxSamplerLodBias())
                          : std::clamp(sampler.LodBias() + upscaler_bias,
                                       -instance.MaxSamplerLodBias(), instance.MaxSamplerLodBias()),
        .anisotropyEnable = anisotropy_enable,
        .maxAnisotropy = max_anisotropy,
        // GCN compares per instruction; a plain read of a compare sampler is undefined in Vulkan.
        .compareEnable = is_depth,
        .compareOp = LiverpoolToVK::DepthCompare(sampler.depth_compare_func),
        .minLod = sampler.MinLod(),
        .maxLod = sampler.MaxLod(),
        .borderColor = border_color,
        .unnormalizedCoordinates = false, // Handled in shader due to Vulkan limitations.
    };
    auto [sampler_result, smplr] = instance.GetDevice().createSamplerUnique(sampler_ci);
    ASSERT_MSG(sampler_result == vk::Result::eSuccess, "Failed to create sampler: {}",
               vk::to_string(sampler_result));
    handle = std::move(smplr);
}

Sampler::~Sampler() = default;

// sotc: SOTC_BINDLESS_SAMP. Off unless the instance enabled the descriptor indexing features.
BindlessSamplers::BindlessSamplers(const Vulkan::Instance& instance_) : instance{instance_} {
    enabled = instance.IsBindlessSamplersEnabled();
    if (!enabled) {
        return;
    }
    const auto device = instance.GetDevice();

    // Samplers of the heap are read by any stage, so the layout is visible to all of them.
    const vk::DescriptorSetLayoutBinding binding{
        .binding = 0,
        .descriptorType = vk::DescriptorType::eSampler,
        .descriptorCount = Capacity,
        .stageFlags = vk::ShaderStageFlagBits::eAll,
    };
    const vk::DescriptorBindingFlags binding_flags =
        vk::DescriptorBindingFlagBits::ePartiallyBound |
        vk::DescriptorBindingFlagBits::eUpdateAfterBind |
        // sotc: new samplers are written while earlier submits using the set are pending.
        vk::DescriptorBindingFlagBits::eUpdateUnusedWhilePending;
    const vk::DescriptorSetLayoutBindingFlagsCreateInfo flags_info{
        .bindingCount = 1,
        .pBindingFlags = &binding_flags,
    };
    const vk::DescriptorSetLayoutCreateInfo layout_info{
        .pNext = &flags_info,
        .flags = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool,
        .bindingCount = 1,
        .pBindings = &binding,
    };
    auto [layout_result, new_layout] = device.createDescriptorSetLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess, "Bindless sampler layout: {}",
               vk::to_string(layout_result));
    layout = std::move(new_layout);

    const vk::DescriptorPoolSize pool_size{vk::DescriptorType::eSampler, Capacity};
    const vk::DescriptorPoolCreateInfo pool_info{
        .flags = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    auto [pool_result, new_pool] = device.createDescriptorPoolUnique(pool_info);
    ASSERT_MSG(pool_result == vk::Result::eSuccess, "Bindless sampler pool: {}",
               vk::to_string(pool_result));
    pool = std::move(new_pool);

    const vk::DescriptorSetAllocateInfo alloc_info{
        .descriptorPool = *pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &*layout,
    };
    auto [alloc_result, sets] = device.allocateDescriptorSets(alloc_info);
    ASSERT_MSG(alloc_result == vk::Result::eSuccess, "Bindless sampler set: {}",
               vk::to_string(alloc_result));
    set = sets.front();

    // Slot 0: linear, repeat. Pipelines index it when the heap is full.
    const vk::SamplerCreateInfo default_info{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eLinear,
        .addressModeU = vk::SamplerAddressMode::eRepeat,
        .addressModeV = vk::SamplerAddressMode::eRepeat,
        .addressModeW = vk::SamplerAddressMode::eRepeat,
        .maxLod = VK_LOD_CLAMP_NONE,
    };
    auto [sampler_result, new_default] = device.createSamplerUnique(default_info);
    ASSERT_MSG(sampler_result == vk::Result::eSuccess, "Default sampler: {}",
               vk::to_string(sampler_result));
    default_sampler = std::move(new_default);
    Write(0, *default_sampler);
    // Slot 1: the same with depth compare (LessOrEqual), for depth samplers when the heap is full.
    vk::SamplerCreateInfo compare_info = default_info;
    compare_info.compareEnable = vk::True;
    compare_info.compareOp = vk::CompareOp::eLessOrEqual;
    auto [compare_result, new_compare] = device.createSamplerUnique(compare_info);
    ASSERT_MSG(compare_result == vk::Result::eSuccess, "Default compare sampler: {}",
               vk::to_string(compare_result));
    default_compare_sampler = std::move(new_compare);
    Write(1, *default_compare_sampler);
    LOG_INFO(Render_Vulkan, "Bindless sampler heap: {} slots", Capacity);
}

BindlessSamplers::~BindlessSamplers() = default;

u32 BindlessSamplers::Allocate(vk::Sampler sampler) {
    if (next_slot >= Capacity || next_slot >= MaxAllocated) {
        if (!logged_full) {
            logged_full = true;
            LOG_WARNING(Render_Vulkan, "Bindless sampler heap full: new samplers use the defaults");
        }
        return NoSlot;
    }
    const u32 slot = next_slot++;
    Write(slot, sampler);
    return slot;
}

void BindlessSamplers::Write(u32 slot, vk::Sampler sampler) {
    const vk::DescriptorImageInfo info{sampler, {}, vk::ImageLayout::eUndefined};
    const vk::WriteDescriptorSet write{
        .dstSet = set,
        .dstBinding = 0,
        .dstArrayElement = slot,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eSampler,
        .pImageInfo = &info,
    };
    instance.GetDevice().updateDescriptorSets(write, {});
}

} // namespace VideoCore
