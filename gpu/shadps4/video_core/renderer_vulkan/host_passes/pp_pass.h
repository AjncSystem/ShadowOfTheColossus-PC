//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Frame;
}

namespace Vulkan::HostPasses {

class PostProcessingPass {
public:
    struct Settings {
        float gamma = 1.0f;
        u32 hdr = 0;
        u32 srgb_input = 0;
        /// sotc: 1: Render's ui image over the (DLSS upscaled) input; 2: that input with the
        /// vignette but no ui (the HUD-less frame); 0: plain.
        u32 ui = 0;
        /// sotc: the game's vignette (start, end, radius scale, strength), with ui only
        std::array<float, 4> vignette{};
        float ui_sharpen = 0.0f; ///< sotc: sharpening of the upscaled ui (0 = none)
        float sky_sharpen = 0.0f; ///< sotc: sharpening where Render's sky mask is set
        float hdr_white = 200.0f; ///< sotc: HDR paper white in nits (SOTC_HDR_WHITE)
        float hdr_curve = 1.0f; ///< sotc: exponent of the game's HDR composite
    };

    void Create(vk::Device device, vk::Format surface_format);

    void Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D input_size,
                Frame& output, Settings settings, vk::ImageView ui = {},
                vk::ImageView sky = {});

private:
    vk::UniquePipeline pipeline{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueSampler sampler{};
};

} // namespace Vulkan::HostPasses
