// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: AMD FSR 3.1 temporal upscaler (FidelityFX SDK 2.3, third_party/fsr-vulkan) for the game's
// image: the game renders at SOTC_RENDER_RES (below 1080p) and this brings it to the output size
// (SOTC_OUTPUT_RES). SOTC_UPSCALER=fsr3 turns it on. Inputs are those of frame generation
// (vk_dlssg.h: the G-buffer depth and motion vectors, the camera and the TAA jitter); a frame
// without them goes through FSR 1 / bilinear as before.

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
}

namespace Vulkan::Fsr3Up {

/// After the device is created.
void Init(const Instance& instance);

/// The upscaler was asked for (SOTC_UPSCALER=fsr3) and works on this device; the game's inputs are
/// then captured (Dlssg::CapturingInputs).
bool Wanted();

/// Command processor thread (PrepareFrame, no rendering active on `cmdbuf`): copies the game image
/// `game_image` (layout TransferSrcOptimal, `width` x `height`) of presentation frame `frame_id`
/// to the upscaler's colour input of that frame. The SDK bridge makes raw Vulkan calls, which
/// cannot go through the draw thread's recorded command buffer: the dispatch is on the present
/// thread.
/// `swap_red_blue`: the image's red and blue are the other way round for the display.
void Prepare(vk::CommandBuffer cmdbuf, u32 frame_id, vk::Image game_image, u32 width, u32 height,
             bool swap_red_blue);

/// Present thread, `cmdbuf` a real command buffer of `scheduler`: upscales the colour of Prepare
/// to `out_width` x `out_height`. Returns a view (layout ShaderReadOnlyOptimal) of the result, or
/// a null view when this frame has no inputs.
vk::ImageView Dispatch(vk::CommandBuffer cmdbuf, Scheduler& scheduler, u32 frame_id, u32 out_width,
                       u32 out_height);

} // namespace Vulkan::Fsr3Up
