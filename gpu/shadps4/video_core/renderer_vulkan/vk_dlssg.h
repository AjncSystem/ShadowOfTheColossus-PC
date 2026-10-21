// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: DLSS Frame Generation (NVIDIA Streamline, sl.dlss_g) for the presented frames.
//
// SOTC_DLSSG=1 with the Streamline DLLs (sl.interposer.dll, sl.common.dll, sl.dlss_g.dll,
// sl.reflex.dll, sl.pcl.dll, nvngx_dlssg.dll) next to bb-probe.exe. Vulkan is then loaded
// through sl.interposer.dll, which hooks the device and the swapchain; each game frame gives
// Streamline its depth, its motion vectors and its camera, and DLSS-G inserts a generated
// frame between two presented ones. The inputs come from the game's G-buffer pass (the draws
// with an R16G16 float target in MRT 4: curUV - prevUV per pixel) and its view constants.
//
// The same inputs feed AMD FSR 3 frame generation (vk_fsrfg.cpp); vk_framegen.h chooses which
// one runs. They are captured only while a frame generation mode is on.

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/types.h"

namespace AmdGpu {
union Regs;
}
namespace VideoCore {
class TextureCache;
}

namespace Vulkan {
class Instance;
class Runtime;
class Scheduler;
}

namespace Vulkan::Dlssg {

/// Before any Vulkan call: when FrameGen::WantStreamline(), loads sl.interposer.dll and
/// initialises Streamline. Its vkGetInstanceProcAddr, or nullptr to use the Vulkan loader.
PFN_vkGetInstanceProcAddr LoadInterposer();

/// After vkCreateDevice: support checks, Reflex options, FSR 3, the frame generation mode.
void OnDeviceCreated(const Instance& instance);

/// Streamline is loaded and DLSS-G is supported on this device.
bool Enabled();

/// The current frame generation mode needs the game's inputs (DLSS-G, or FSR 3 on any GPU), or
/// DLSS super resolution is on: the rasterizer then calls NoteDraw.
bool CapturingInputs();

/// SOTC_CAM_LOG=1 while CapturingInputs() is false (no DLSS or frame generation, e.g. any card
/// without DLSS-G): the rasterizer calls NoteCamera instead of NoteDraw.
bool CameraLogOnly();

/// Command processor thread, at each draw: logs the camera position of G-buffer draws
/// ("SOTC CAM <ms> x y z", the line NoteDraw's frames print) at most every 160 ms.
void NoteCamera(const AmdGpu::Regs& regs, u32 mrt_mask);

/// The present mode DLSS-G needs (Vulkan: no vsync), when Enabled().
bool WantsImmediatePresent();

/// Command processor thread, at each draw: the first G-buffer draw of the frame gives the
/// camera (guest view constants) and the depth and motion vector images.
void NoteDraw(const AmdGpu::Regs& regs, u32 mrt_mask, VideoCore::ImageId depth_id,
              VideoCore::ImageId mvec_id, VideoCore::TextureCache& texture_cache,
              Runtime& runtime, Scheduler& scheduler);

/// Command processor thread, in PrepareFrame (no rendering active on cmdbuf): copies this
/// frame's depth and motion vectors into images of presentation frame `frame_id` and sets the
/// frame's constants. Frames without a G-buffer pass get no inputs (DLSS-G off).
void PrepareInputs(u32 frame_id, vk::CommandBuffer cmdbuf, VideoCore::TextureCache& texture_cache,
                   Runtime& runtime, u32 game_width, u32 game_height);

/// sotc: command processor thread, in PrepareFrame after PrepareInputs: presentation frame
/// `frame_id` without its HUD (the game's composite redrawn with a transparent UI, through the
/// same post-process pass), layout General until the frame is reused; a null image when it has
/// none. DLSS-G gets it as the HUD-less color so that it does not move the HUD with the scene.
void NoteHudless(u32 frame_id, vk::Image image, vk::ImageView view, vk::Format format, u32 width,
                 u32 height);

/// DLSS Super Resolution (SOTC_UPSCALER=dlss) is supported on this device.
bool SuperResolution();

/// DLSS Super Resolution works on this device, also while F9 has it switched off.
bool SuperResolutionAvailable();
/// sotc: HDR output: the composite's scene keeps highlights above 1.0 (presenter thread).
void SetHdr(bool hdr);
/// sotc: the exponent of the game's HDR composite (1.0 on the splash and menus, 1.42 in game).
void NoteHdrCurve(float k);
float HdrCurve();

/// SOTC_DLSS_COMPARE=1 (launcher debug option): F9 compares DLSS with FSR1, a label shows which.
bool CompareMode();

/// The DLSS mode in use ("DLSS Quality", ...), for the compare label.
const char* ModeName();

/// Window thread, F9 in compare mode: switch DLSS super resolution off (FSR1 and the game's
/// TAA) and back on.
void ToggleSuperResolution();

/// Command processor thread, at the game's final composite draw (UI layer over the scene, into
/// a video out surface): the scene before the UI and the UI layer of this frame.
/// sotc: the tonemap's vignette (start, end, radius scale, strength), applied after DLSS instead.
void NoteVignette(const float params[4]);
bool Vignette(float out[4]);

void NoteComposite(VideoCore::ImageId scene_id, VideoCore::ImageId ui_id,
                   VideoCore::TextureCache& texture_cache);

struct Upscaled {
    vk::ImageView scene; ///< linear colour at the output size, shader read only
    vk::ImageView ui;    ///< the game's UI layer (premultiplied, sRGB view), render size
    vk::ImageView sky;   ///< render size, g = 1 in the sky; empty without camera motion fill
};

/// Command processor thread, in PrepareFrame after PrepareInputs: DLSS upscales this frame's
/// scene to width x height. Empty when this frame has no composite or no inputs (menus,
/// loading screens): the game image is presented as before.
Upscaled Upscale(u32 frame_id, Scheduler& scheduler, VideoCore::TextureCache& texture_cache,
                 Runtime& runtime, u32 width, u32 height);

/// Command processor thread, after the frame's commands are flushed.
void FrameSubmitted(u32 frame_id);

/// A presentation frame without game inputs (blank frame, re-presented frame).
void NoInputs(u32 frame_id);

/// Present thread: options, tags and the present start marker, right before vkQueuePresentKHR.
/// `rect` is where the game image is in the swapchain image of size `extent`.
void BeforePresent(u32 frame_id, vk::Rect2D rect, vk::Extent2D extent);

/// Present thread: present end marker and status, right after vkQueuePresentKHR.
void AfterPresent(u32 frame_id);

/// Before the swapchain is recreated or the device destroyed: DLSS-G off.
void Suspend();

/// A frame's inputs for FSR 3 frame generation, from the camera like Streamline's constants.
struct FrameInputs {
    vk::Image mvec;      ///< R16G16 float, curUV - prevUV, layout ShaderReadOnlyOptimal
    vk::ImageUsageFlags mvec_usage;
    u32 width, height;   ///< of the depth and motion vectors (the game's render size)
    float jitter[2];     ///< pixels, y down
    float z_near, z_far; ///< view distances
    float fov_y;         ///< radians
    float pos[3], up[3], right[3], fwd[3];
    bool depth_inverted; ///< 1 at the near plane
    bool reset;          ///< a camera cut
};

/// Present thread: the inputs of presentation frame `frame_id`, false when it has none.
bool GetFrameInputs(u32 frame_id, FrameInputs& out);

} // namespace Vulkan::Dlssg
