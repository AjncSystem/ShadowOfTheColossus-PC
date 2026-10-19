// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: AMD FSR 3 frame generation (FidelityFX SDK 2.3 optical flow + frame interpolation,
// third_party/fsr-vulkan) for GPUs without DLSS-G: RTX 20/30, AMD, Intel.
//
// Inputs are DLSS-G's (vk_dlssg.h: the G-buffer depth and motion vectors, the camera), with the
// depth converted to R32F. At each game frame the presenter interpolates between the previous
// frame and this one, presents that image, then presents the game frame half a frame later on
// the presenter thread (VideoOutDriver::AsyncPresent). The overlay is drawn on both.

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
}

namespace Vulkan::FsrFg {

/// After the device is created: the depth conversion pipeline; checks the device can run FSR 3.
void Init(const Instance& instance);

/// FSR 3 frame generation can run on this device.
bool Supported();

/// Command processor thread (Dlssg::PrepareInputs): converts this frame's depth copy (layout
/// ShaderReadOnlyOptimal, sampled through `depth_view`) to R32F for presentation frame `frame_id`.
void ConvertDepth(u32 frame_id, vk::CommandBuffer cmdbuf, vk::ImageView depth_view, u32 width,
                  u32 height, bool inverted);

/// Presentation frame `frame_id` has no depth for FSR 3 (yet).
void NoDepth(u32 frame_id);

/// Present thread, at a game frame: the time since the last one, for the pacing.
void NoteGameFrame();

/// Present thread: records the interpolation between the last game frame and this one
/// (`frame_image`, layout General before and after, `width` x `height`). Returns the generated
/// image (RGBA8, layout General, same size) to present now, or a null image when there is none
/// (first frame, after a cut or a frame without inputs). `hudless_image`: the same frame without
/// the HUD (layout General before and after, same size), or a null image.
vk::Image Generate(vk::CommandBuffer cmdbuf, u32 frame_id, vk::Image frame_image, u32 width,
                   u32 height, vk::Image hudless_image);

/// Present thread: the commands of Generate were submitted at `tick` of `scheduler`.
void Submitted(Scheduler& scheduler, u64 tick);

/// Present thread, after the generated frame was presented: waits until the game frame is due
/// (half the frame interval later).
void WaitForGameFrame();

/// Present thread, after each present while FSR 3 runs: the pacing statistics (SOTC_FSRFG_LOG=1
/// prints them every 5 s).
void NotePresent();

} // namespace Vulkan::FsrFg
