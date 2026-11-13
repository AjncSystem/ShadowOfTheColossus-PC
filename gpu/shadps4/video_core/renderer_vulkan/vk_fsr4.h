// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: AMD FSR upscaling. SOTC_UPSCALER=fsr4.
//
// Windows: the official AMD FSR SDK 2.4 (amd_fidelityfx_loader_vk.dll and
// amd_fidelityfx_upscaler_vk.dll next to bb-probe.exe, loaded at run time). The SDK picks the
// newest version the GPU runs: FSR 4.1 (machine learning) on Radeon RX 6000/7000/9000 and recent
// Ryzen APUs, FSR 3.1 on every other GPU.
//
// Elsewhere: AMD ships FSR 4.1 as Windows PE binaries only, so the vendored portable
// "source-v07" provider (third_party/fsr-vulkan, MIT) runs the same FSR 4 INT8/DOT4 network as
// native Vulkan SPIR-V from an asset bundle on disk. It is not AMD's FSR 4.1.1 and needs
// shaderInt8, shaderFloat16 and shaderIntegerDotProduct (an RDNA4-class GPU in practice).
//
// The inputs are DLSS super resolution's (vk_dlssg.cpp: the scene before the UI, the G-buffer
// depth and motion vectors, the camera); Dlssg::Upscale calls Dispatch in place of DLSS.

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
}

namespace Vulkan::Fsr4 {

/// After the device is created: loads the SDK (Windows) or locates the asset bundle (elsewhere).
bool Init(const Instance& instance);

/// The upscaler loaded and this GPU can run it.
bool Available();

/// "FSR 4.1.2", "FSR 3.1.6", "FSR 4 v07"...: the implementation running here.
const char* Name();

struct Image {
    vk::Image image;
    /// The v07 resource ABI binds views, not images, so every caller passes the view it already
    /// has. A depth/stencil image's view must name exactly one aspect. Ignored on Windows.
    vk::ImageView view{};
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
    /// The scheduler tick the GPU signals when the submission carrying this dispatch is done. The
    /// provider's descriptor pools and constant-buffer ring are per frame slot and may only be
    /// reused once the slot's work finished, so frames retire against this tick and not by age.
    /// Read when the deferred recording runs, so it must be captured on the draw thread, where it
    /// is still the tick of the upcoming flush. Ignored on Windows (the SDK owns its own lifetime).
    u64 tick{};
    /// Queried for whether `tick` has been reached. Ignored on Windows.
    Scheduler* scheduler{};
};

/// Recording thread, with the real command buffer: records the upscale. Creates (or recreates, for
/// a new size or depth layout) the context first. False on failure.
bool Dispatch(VkCommandBuffer cmdbuf, const Params& params);

} // namespace Vulkan::Fsr4
