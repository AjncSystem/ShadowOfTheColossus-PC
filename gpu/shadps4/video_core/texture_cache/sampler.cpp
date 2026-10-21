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

} // namespace VideoCore
