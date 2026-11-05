// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: AMD FSR upscaling through the official AMD FSR SDK 2.4 (amd_fidelityfx_loader_vk.dll and
// amd_fidelityfx_upscaler_vk.dll next to bb-probe.exe, loaded at run time). The SDK picks the
// newest version the GPU runs: FSR 4.1 (machine learning) on Radeon RX 6000/7000/9000 and recent
// Ryzen APUs, FSR 3.1 on every other GPU. SOTC_UPSCALER=fsr4.
//
// The inputs are DLSS super resolution's (vk_dlssg.cpp: the scene before the UI, the G-buffer
// depth and motion vectors, the camera); Dlssg::Upscale calls Dispatch in place of DLSS.

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace Vulkan::Fsr4 {

/// After the device is created: loads the SDK and lists the versions this GPU runs.
bool Init(const Instance& instance);

/// The SDK loaded and has an upscaler for this GPU.
bool Available();

/// "FSR 4.1.2", "FSR 3.1.6"...: the version the SDK runs here.
const char* Name();

struct Image {
    vk::Image image;
    vk::Format format{};
    u32 width{}, height{};
};

struct Params {
    Image color;  ///< render size, layout ShaderReadOnlyOptimal, tonemapped (0..1)
    Image depth;  ///< render size, layout ShaderReadOnlyOptimal
    Image mvec;   ///< render size, layout ShaderReadOnlyOptimal
    Image output; ///< output size, layout General (storage)
    u32 render_width{}, render_height{}, out_width{}, out_height{};
    float jitter_x{}, jitter_y{};   ///< pixels
    float mvec_scale_x{}, mvec_scale_y{}; ///< motion vector -> pixels
    float z_near{}, z_far{}, fov_y{}; ///< fov in radians
    bool depth_inverted{};
    bool reset{};
};

/// Recording thread, with the real command buffer: records the upscale. Creates (or recreates, for
/// a new size or depth layout) the context first. False on failure.
bool Dispatch(VkCommandBuffer cmdbuf, const Params& params);

} // namespace Vulkan::Fsr4
