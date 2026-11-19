//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <memory>
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Frame;
class Instance;
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
        float paper_white = 200.0f; ///< sotc: hdr 2 (SDR game on HDR10): nits of SDR white
        float highlight_gain = 1.5f; ///< sotc: hdr 2: brightness gain of the highlights
        /// sotc: hdr 1: the display's peak in nits; highlights roll off into it (BT.2390 EETF)
        /// instead of clipping. 0: no roll-off (as before).
        float hdr_peak = 0.0f;
        /// sotc: hdr 1 with the UI layer (DLSS): nits of the UI's white, apart from the scene's
        /// paper white. 0: the same as hdr_white.
        float ui_white = 0.0f;
        /// sotc: hdr 1: picture controls on the scene, before the roll-off (RefreshHdrLevels).
        /// Each value here is neutral; all neutral leaves the picture as it was.
        float exposure = 1.0f;   ///< linear gain (2^EV)
        float contrast = 1.0f;   ///< power on luminance around mid grey
        float shadows = 1.0f;    ///< extra power below mid grey (< 1 lifts)
        float highlights = 1.0f; ///< extra power above mid grey (> 1 brightens)
        float saturation = 1.0f; ///< chroma scale
        float blowout = 0.0f;    ///< 0..1: highlights fade towards white near the peak
    };

    void Create(vk::Device device, vk::Format surface_format);

    void Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D input_size,
                Frame& output, Settings settings, vk::ImageView ui = {},
                vk::ImageView sky = {});

    /// sotc: the scene key buffer and its compute pipeline (pp_key.comp), after Create.
    void CreateKey(const Instance& instance);

    /// sotc: measures the scene's key for the HDR picture controls, once per presented frame
    /// before Render (hdr 1 only). `input` in shader read layout; pq: the game's own PQ frame
    /// (else the DLSS scene, linear with the composite's exponent `curve`).
    void MeasureKey(vk::CommandBuffer cmdbuf, vk::ImageView input, bool pq, float curve);

private:
    vk::UniquePipeline pipeline{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueSampler sampler{};
    // sotc: scene key (MeasureKey)
    std::unique_ptr<VideoCore::Buffer> key_buffer;
    vk::UniquePipeline key_pipeline{};
    vk::UniquePipelineLayout key_layout{};
    vk::UniqueDescriptorSetLayout key_set_layout{};
    std::chrono::steady_clock::time_point key_last{};
    bool key_measured = false;
};

} // namespace Vulkan::HostPasses
