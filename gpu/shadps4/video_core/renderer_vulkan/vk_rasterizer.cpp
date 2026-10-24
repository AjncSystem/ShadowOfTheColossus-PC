// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <cstdio>
#include <map>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <xxhash.h>
#include "common/path_util.h"
#include "video_core/renderdoc.h"
#include "common/debug.h"
#include "common/elf_info.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/sotc_lod_stats.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_dlssg.h"
#include "video_core/renderer_vulkan/vk_shadow_scroll.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/buffer_cache/region_manager.h"
#include "video_core/sotc_live.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/sotc_stats.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/renderer_vulkan/vk_framegen.h"
// After vk_common.h (via the headers above), which sets the Vulkan header defines first.
#include <vk_mem_alloc.h>
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

static Shader::PushData MakeUserData(const AmdGpu::Regs& regs) {
    // TODO(roamic): Add support for multiple viewports and geometry shaders when ViewportIndex
    // is encountered and implemented in the recompiler.
    Shader::PushData push_data{};
    push_data.xoffset = regs.viewport_control.xoffset_enable ? regs.viewports[0].xoffset : 0.f;
    push_data.xscale = regs.viewport_control.xscale_enable ? regs.viewports[0].xscale : 1.f;
    push_data.yoffset = regs.viewport_control.yoffset_enable ? regs.viewports[0].yoffset : 0.f;
    push_data.yscale = regs.viewport_control.yscale_enable ? regs.viewports[0].yscale : 1.f;
    return push_data;
}

Rasterizer::Rasterizer(const Instance& instance_, Scheduler& scheduler_, Runtime& runtime_,
                       AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, page_manager{this},
      buffer_cache{instance, scheduler, runtime, liverpool_, texture_cache, page_manager},
      texture_cache{instance, scheduler, runtime, liverpool_, buffer_cache, page_manager},
      liverpool{liverpool_}, memory{Core::Memory::Instance()},
      pipeline_cache{instance, scheduler, liverpool, buffer_cache.GetSparsePageShift()},
      host_markers_enabled{EmulatorSettings.IsVkHostMarkersEnabled()},
      guest_markers_enabled{EmulatorSettings.IsVkGuestMarkersEnabled()} {
    if (!EmulatorSettings.IsNullGPU()) {
        liverpool->BindRasterizer(this);
    }
    memory->SetRasterizer(this);

    scheduler.SetSubmitCallback([this](Vulkan::SubmitInfo& info) {
        runtime.FlushBarriers();
        buffer_cache.SubmitPendingArenaBinds(info);
    });
}

Rasterizer::~Rasterizer() {
    for (auto& [address, image] : hudless_images) {
        DestroyHudless(image);
    }
    DestroyHudless(hudless_clear);
    DestroyHudless(ui_shadow);
}

// Games may disable color buffer and enable force depth/stencil dirty and valid to do a copy from
// one depth-stencil surface to another, without a pixel shader: {depth copy, stencil copy}.
static std::pair<bool, bool> DepthStencilOverrideCopy(const auto& regs) {
    if (regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Disable) {
        return {false, false};
    }
    const bool depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const bool stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    return {depth_copy, stencil_copy};
}

bool Rasterizer::IsDrawFilteredOut() const {
    const auto& regs = liverpool->DrawRegs();
    using Mode = AmdGpu::ColorControl::OperationMode;
    const auto mode = regs.color_control.mode;
    if (mode == Mode::EliminateFastClear || mode == Mode::FmaskDecompress ||
        mode == Mode::Resolve || regs.primitive_type == AmdGpu::PrimitiveType::None) {
        return true;
    }
    const auto [depth_copy, stencil_copy] = DepthStencilOverrideCopy(regs);
    return depth_copy || stencil_copy;
}

bool Rasterizer::FilterDraw() {
    const auto& regs = liverpool->DrawRegs();
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear) {
        // Clears the render target if FCE is launched before any draws
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        // TODO: check for a valid MRT1 to promote the draw to the resolve pass.
        LOG_TRACE(Render_Vulkan, "FMask decompression pass skipped");
        ScopedMarkerInsert("FmaskDecompress");
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        LOG_TRACE(Render_Vulkan, "Resolve pass");
        Resolve();
        return false;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        LOG_TRACE(Render_Vulkan, "Primitive type 'None' skipped");
        ScopedMarkerInsert("PrimitiveTypeNone");
        return false;
    }

    const auto [depth_copy, stencil_copy] = DepthStencilOverrideCopy(regs);
    if (depth_copy || stencil_copy) {
        // We need to detect this case and perform the copy, otherwise it will have no effect.
        LOG_TRACE(Render_Vulkan, "Performing depth-stencil override copy");
        DepthStencilCopy(depth_copy, stencil_copy);
        return false;
    }

    return true;
}

void Rasterizer::PrepareRenderState(const GraphicsPipeline* pipeline) {
    const auto texture_lock = texture_cache.BatchLock(); // sotc: one lock for the targets
    // Prefetch render targets to handle overlaps with bound textures (e.g. mipgen)
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = liverpool->DrawRegs();
    if (regs.color_control.degamma_enable) {
        LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (skip_cb_binding || !col_buf || !target_mask || (key.mrt_mask & (1 << cb)) == 0) {
            image_id = {};
            continue;
        }
        const auto& hint = liverpool->DrawCbExtent()[cb];
        std::construct_at(&desc, col_buf, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    }

    if ((regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid())) {
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& hint = liverpool->DrawDbExtent();
        auto& [image_id, desc] = db_desc;
        std::construct_at(&desc, regs.depth_buffer, regs.depth_view, regs.depth_control,
                          htile_address, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    } else {
        db_desc.first = {};
    }
}

static std::pair<u32, u32> GetDrawOffsets(const AmdGpu::Regs& regs, const Shader::Info& info,
                                          const Shader::Gcn::FetchShaderData& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (!fetch_shader.Empty()) {
        if (vertex_offset == 0 && fetch_shader.vertex_offset_sgpr != -1) {
            vertex_offset = info.UserData()[fetch_shader.vertex_offset_sgpr];
        }
        if (fetch_shader.instance_offset_sgpr != -1) {
            instance_offset = info.UserData()[fetch_shader.instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

void Rasterizer::EliminateFastClear() {
    auto& col_buf = liverpool->DrawRegs().color_buffers[0];
    if (!col_buf || !col_buf.info.fast_clear) {
        return;
    }
    VideoCore::TextureCache::ImageDesc desc(col_buf, liverpool->DrawCbExtent()[0]);
    const auto image_id = texture_cache.FindImage(desc);
    const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
    if (!texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start)) {
        return;
    }
    for (u32 slice = col_buf.view.slice_start; slice <= col_buf.view.slice_max; ++slice) {
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
    }
    auto& image = texture_cache.GetImage(image_id);
    const auto clear_value = LiverpoolToVK::ColorBufferClearValue(col_buf);

    ScopeMarkerBegin(fmt::format("EliminateFastClear:MRT={:#x}:M={:#x}", col_buf.Address(),
                                 col_buf.CmaskAddress()));
    runtime.ClearImage(&image, desc.view_info.range, clear_value);
    ScopeMarkerEnd();
}

// sotc: SOTC_PASS_TRACE=1: every 300 flips, the render passes of one frame in order (depth
// target, its size, colour target 0, screen and generic scissors, CB mode, draws) on stderr.
// A diagnostic for finding which passes are the sun shadow cascades.
template <typename Regs>
static void TracePass(const Regs& regs) {
    static const bool on = std::getenv("SOTC_PASS_TRACE") != nullptr;
    if (!on) {
        return;
    }
    struct Pass {
        u64 depth, color;
        u32 pitch, height, mode, slice0, slice1;
        s32 sx0, sy0, sx1, sy1, gx0, gy0, gx1, gy1;
        u32 draws;
        u64 mrt_key; // the bound colour targets (address and format of each)
        char mrts[320];
    };
    static std::vector<Pass> passes;
    static u64 frame_seen = 0;
    const u64 frame = SotcStats::flip_count.load(std::memory_order_relaxed);
    if (frame != frame_seen) {
        if (frame_seen % 300 == 0 && !passes.empty()) {
            std::fprintf(stderr, "SOTC PASSES frame %llu: %zu passes\n",
                         static_cast<unsigned long long>(frame_seen), passes.size());
            for (const auto& p : passes) {
                std::fprintf(stderr,
                             "  depth %#llx %ux%u slices %u-%u color %#llx mode %u screen %d,%d-%d,%d "
                             "generic %d,%d-%d,%d draws %u | mrt %s\n",
                             static_cast<unsigned long long>(p.depth), p.pitch, p.height, p.slice0, p.slice1,
                             static_cast<unsigned long long>(p.color), p.mode, p.sx0, p.sy0,
                             p.sx1, p.sy1, p.gx0, p.gy0, p.gx1, p.gy1, p.draws, p.mrts);
            }
        }
        passes.clear();
        frame_seen = frame;
    }
    if (frame % 300 != 0) {
        return;
    }
    Pass p{
        .depth = regs.depth_buffer.DepthValid() ? regs.depth_buffer.DepthWriteAddress() : 0,
        .color = regs.color_buffers[0].Address(),
        .pitch = regs.depth_buffer.Pitch(),
        .height = regs.depth_buffer.Height(),
        .mode = static_cast<u32>(regs.color_control.mode),
        .slice0 = regs.depth_view.slice_start,
        .slice1 = regs.depth_view.slice_max,
        .sx0 = regs.screen_scissor.top_left_x,
        .sy0 = regs.screen_scissor.top_left_y,
        .sx1 = regs.screen_scissor.bottom_right_x,
        .sy1 = regs.screen_scissor.bottom_right_y,
        .gx0 = regs.generic_scissor.top_left_x,
        .gy0 = regs.generic_scissor.top_left_y,
        .gx1 = regs.generic_scissor.bottom_right_x,
        .gy1 = regs.generic_scissor.bottom_right_y,
        .draws = 1,
        .mrt_key = 0,
        .mrts = {},
    };
    for (u32 i = 0; i < 8; ++i) {
        const auto& cb = regs.color_buffers[i];
        if (cb) {
            p.mrt_key = p.mrt_key * 0x100000001b3ull ^ cb.Address() ^
                        (u64(cb.info.format) << 56) ^ (u64(i) << 60);
        }
    }
    if (!passes.empty()) {
        auto& last = passes.back();
        if (last.depth == p.depth && last.color == p.color && last.pitch == p.pitch &&
            last.mrt_key == p.mrt_key &&
            last.height == p.height && last.mode == p.mode && last.slice0 == p.slice0 &&
            last.slice1 == p.slice1 && last.sx0 == p.sx0 &&
            last.sy0 == p.sy0 && last.sx1 == p.sx1 && last.sy1 == p.sy1 && last.gx0 == p.gx0 &&
            last.gy0 == p.gy0 && last.gx1 == p.gx1 && last.gy1 == p.gy1) {
            ++last.draws;
            return;
        }
    }
    int at = 0;
    for (u32 i = 0; i < 8 && at < int(sizeof(p.mrts)) - 64; ++i) {
        const auto& cb = regs.color_buffers[i];
        if (cb) {
            at += std::snprintf(p.mrts + at, sizeof(p.mrts) - at, "%u:%#llx d%u n%u %ux%u ", i,
                                static_cast<unsigned long long>(cb.Address()),
                                static_cast<u32>(cb.GetDataFmt()), static_cast<u32>(cb.GetNumberFmt()),
                                cb.Pitch(), cb.Height());
        }
    }
    passes.push_back(p);
}

// sotc: SOTC_PASS_TRACE: with the pass list, the shader hashes (all stages, as the dump file
// names) of the G-buffer draws, recognised by their R16G16 float target in MRT 4.
template <typename Regs, typename Pipe>
static void TraceGbufferShaders(const Regs& regs, const Pipe* pipeline) {
    static const bool on = std::getenv("SOTC_PASS_TRACE") != nullptr;
    if (!on || !pipeline) {
        return;
    }
    static std::map<std::array<u64, 6>, u32> counts;
    static u64 frame_seen = 0;
    const u64 frame = SotcStats::flip_count.load(std::memory_order_relaxed);
    if (frame != frame_seen) {
        if (frame_seen % 300 == 0 && !counts.empty()) {
            std::fprintf(stderr, "SOTC GBUFFER SHADERS frame %llu:\n",
                         static_cast<unsigned long long>(frame_seen));
            for (const auto& [key, draws] : counts) {
                std::fprintf(stderr, "  draws %4u:", draws);
                for (u64 hash : key) {
                    if (hash) {
                        std::fprintf(stderr, " %#018llx", static_cast<unsigned long long>(hash));
                    }
                }
                std::fprintf(stderr, "\n");
            }
        }
        counts.clear();
        frame_seen = frame;
    }
    if (frame % 300 != 0) {
        return;
    }
    const auto& cb4 = regs.color_buffers[4];
    if (!cb4 || cb4.GetDataFmt() != AmdGpu::DataFormat::Format16_16 ||
        cb4.GetNumberFmt() != AmdGpu::NumberFormat::Float) {
        return;
    }
    std::array<u64, 6> key{};
    size_t n = 0;
    for (const auto* info : pipeline->GetStages()) {
        if (info && n < key.size()) {
            key[n++] = info->pgm_hash;
        }
    }
    ++counts[key];
}

// sotc: SOTC_PASS_TRACE: the shader hashes of the draws into square depth targets of 512+ texels
// (the sun shadow map's cascades) per slice, every 300 flips.
template <typename Regs, typename Pipe>
static void TraceShadowShaders(const Regs& regs, const Pipe* pipeline) {
    static const bool on = std::getenv("SOTC_PASS_TRACE") != nullptr;
    if (!on || !pipeline || !regs.depth_buffer.DepthValid()) {
        return;
    }
    static std::map<std::pair<u32, u64>, u32> counts;
    static u64 frame_seen = 0;
    const u64 frame = SotcStats::flip_count.load(std::memory_order_relaxed);
    if (frame != frame_seen) {
        if (frame_seen % 300 == 0 && !counts.empty()) {
            std::fprintf(stderr, "SOTC SHADOW SHADERS frame %llu:\n",
                         static_cast<unsigned long long>(frame_seen));
            for (const auto& [key, draws] : counts) {
                std::fprintf(stderr, "  slice %u vs %#018llx draws %u\n", key.first,
                             static_cast<unsigned long long>(key.second), draws);
            }
        }
        counts.clear();
        frame_seen = frame;
    }
    if (frame % 300 != 0) {
        return;
    }
    const u32 pitch = regs.depth_buffer.Pitch();
    if (pitch != regs.depth_buffer.Height() || pitch < 512) {
        return;
    }
    const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
    ++counts[{regs.depth_view.slice_start, vs.pgm_hash}];
}

// sotc: SOTC_SHADOW_STAGGER=1: the two farthest sun shadow cascades are redrawn on alternate
// frames (slice 2 on odd flips, slice 3 on even ones). A skipped slice starts no render pass, so
// it is not cleared and keeps the previous frame's shadows. The sun shadow map is the square
// depth target that got the most draws in the previous frame (SotC: 1024x1024, 5 slices, ~6500
// draws per frame, 3041 of them in slice 3 and 2049 in slice 2, SOTC_PASS_TRACE). Far shadows
// then lag one frame behind the camera; the two slices cost half as much.
template <typename Regs>
static bool SkipStaggeredShadow(const Regs& regs) {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_SHADOW_STAGGER");
        return v && v[0] == '1';
    }();
    if (!on) {
        return false;
    }
    static u64 frame_seen = 0, sun_map = 0;
    static std::unordered_map<u64, u32> counts; // draws per square depth target this frame
    const u64 frame = SotcStats::flip_count.load(std::memory_order_relaxed);
    if (frame != frame_seen) {
        u64 most_addr = 0;
        u32 most = 0;
        for (const auto& [addr, draws] : counts) {
            if (draws > most) {
                most = draws;
                most_addr = addr;
            }
        }
        sun_map = most >= 500 ? most_addr : 0;
        counts.clear();
        frame_seen = frame;
    }
    if (!regs.depth_buffer.DepthValid() ||
        regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Normal) {
        return false;
    }
    const u32 pitch = regs.depth_buffer.Pitch();
    if (pitch != regs.depth_buffer.Height() || pitch < 512) {
        return false;
    }
    const u64 addr = regs.depth_buffer.DepthWriteAddress();
    ++counts[addr];
    if (addr != sun_map) {
        return false;
    }
    const u32 slice = regs.depth_view.slice_start;
    return slice == 2 ? (frame & 1) != 0 : slice == 3 ? (frame & 1) == 0 : false;
}

// sotc: SOTC_SHADOW_REUSE (default 0 = off; 1 = on, check = verify once and save the result). Off
// by default: with it, far shadows on the ground blink at 15 Hz with a still camera (the Shrine:
// a rock's shadow there on drawn frames, gone on moved ones; 0.36% of the pixels flicker per
// frame against 0.01% off). SOTC_SHADOW_STAGGER, which only skips, blinks the same, so the game
// does not keep a skipped cascade's texels until the next frame. When on: the far
// sun shadow cascades (slices 2 and 3 of the sun shadow map, the square depth target with the
// most draws in the previous frame; ~2000 draws each) are drawn on alternate frames, and on the
// other frames last frame's cascade is moved instead (vk_shadow_scroll.h). The engine snaps its
// cascades to texels: between two frames only the translation of a cascade's (orthographic)
// light matrix changes, by whole texels in x and y. Moving last frame's texels by that many
// texels, and their depths by the z change, gives the cascade a redraw would (except for things
// that moved since, whose far shadows update every other frame, and the few texels entering at
// the edge). The light matrix is the shadow view's world -> clip at +0x80 of the view
// constants, as for the camera (*(*(VS user data 0..1) + 8), vk_dlssg.cpp). Any other change
// (rotation, scale, viewport, a non-whole-texel or large move): the cascade is drawn. Unlike
// SOTC_SHADOW_STAGGER (texels of another matrix: shadows jumping as the camera moved), a reused
// cascade always matches the matrix the lighting uses.
namespace {
struct ShadowReuseState {
    static constexpr u32 kSlices = 8;
    u64 frame_seen = 0, sun_map = 0;
    std::unordered_map<u64, u32> counts; // draws per square depth target this frame
    std::array<std::array<float, 16>, kSlices> last_vp{};
    std::array<std::array<float, 3>, kSlices> last_scale{};
    std::array<bool, kSlices> have_vp{}, decided{}, skip{}, drawn_prev{}, drawn_now{};
    // The light matrix a slice was decided for this frame, and the image it was drawn into.
    std::array<std::array<float, 16>, kSlices> decided_vp{};
    std::array<VideoCore::ImageId, kSlices> last_image{};
    std::array<u64, kSlices> last_image_uid{};
    u64 prev_key = 0; // target of the previous draw (address * 8 + slice), 0: another kind
    u64 drawn = 0, scrolled = 0, refused = 0, invalid = 0, rendered_again = 0, image_changed = 0;
    s32 check_slice = -1; // verification: the slice scrolled into the scratch image
    bool check_done = false;
};
} // namespace

bool Rasterizer::ReuseShadowCascade() {
    // sotc: SOTC_SHADOW_REUSE=auto (the launcher's default): on with NVIDIA, as in 0.58/0.59, off
    // on AMD and Intel (#178). Unset: off.
    static const int env_mode = [this] {
        const char* v = std::getenv("SOTC_SHADOW_REUSE");
        if (!v) {
            return 0;
        }
        if (v[0] == 'a') {
            constexpr u32 VendorNvidia = 0x10DE;
            return instance.GetVendorID() == VendorNvidia ? 1 : 0;
        }
        return v[0] == '0' ? 0 : (v[0] == 'c' ? 2 : 1);
    }();
    // The in-game menu can switch it while playing (sotc_live.h).
    const int live = SotcLive::shadow_reuse.load(std::memory_order_relaxed);
    const int mode = live >= 0 ? live : env_mode;
    if (mode == 0) {
        return false;
    }
    static ShadowReuseState s;
    const auto& regs = liverpool->DrawRegs(); // sotc: the draw thread's copy there
    const u64 frame = SotcStats::cp_frame.load(std::memory_order_relaxed);
    if (frame != s.frame_seen) {
        u64 most_addr = 0;
        u32 most = 0;
        for (const auto& [addr, draws] : s.counts) {
            if (draws > most) {
                most = draws;
                most_addr = addr;
            }
        }
        if ((most >= 500 ? most_addr : 0) != s.sun_map) {
            s.have_vp.fill(false); // another shadow map: nothing to reuse
        }
        s.sun_map = most >= 500 ? most_addr : 0;
        s.counts.clear();
        s.drawn_prev = s.drawn_now;
        s.drawn_now.fill(false);
        s.decided.fill(false);
        s.frame_seen = frame;
        // Every ~60 s in the log (every 300 frames with SOTC_SHADOW_REUSE_STATS).
        static const bool stats = std::getenv("SOTC_SHADOW_REUSE_STATS") != nullptr;
        if (frame % (stats ? 300 : 1800) == 0) {
            std::fprintf(stderr,
                         "SOTC SHADOW REUSE: far cascades drawn %llu, moved %llu, refused %llu, "
                         "unreadable %llu, drawn twice in a frame %llu, image changed %llu\n",
                         static_cast<unsigned long long>(s.drawn),
                         static_cast<unsigned long long>(s.scrolled),
                         static_cast<unsigned long long>(s.refused),
                         static_cast<unsigned long long>(s.invalid),
                         static_cast<unsigned long long>(s.rendered_again),
                         static_cast<unsigned long long>(s.image_changed));
            s.drawn = s.scrolled = s.refused = s.invalid = s.rendered_again = s.image_changed = 0;
        }
    }
    const bool square_depth =
        regs.depth_buffer.DepthValid() &&
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Normal &&
        regs.depth_buffer.Pitch() == regs.depth_buffer.Height() && regs.depth_buffer.Pitch() >= 512;
    const u64 addr = square_depth ? regs.depth_buffer.DepthWriteAddress() : 0;
    const u32 slice = regs.depth_view.slice_start;
    const u64 key = square_depth ? addr * 8 + (slice & 7) : 0;
    const bool new_run = key != s.prev_key; // first draw after draws into another target
    s.prev_key = key;
    if (!square_depth) {
        return false;
    }
    ++s.counts[addr];
    if (addr != s.sun_map || (slice != 2 && slice != 3)) {
        return false;
    }
    if (s.decided[slice] && !new_run) {
        return s.skip[slice];
    }

    // The cascade's light matrix (rows at +0x80 of the shadow view's constants).
    constexpr u64 kMask48 = 0xFFFFFFFFFFFFull;
    const u64 ud =
        (u64(regs.vs_program.user_data[0]) | (u64(regs.vs_program.user_data[1]) << 32)) & kMask48;
    u64 cb = 0;
    std::array<float, 16> vp{};
    bool valid = ud && memory->TryReadBacking(ud + 8, &cb, sizeof(cb)) && (cb &= kMask48) &&
                 memory->TryReadBacking(cb + 0x80, vp.data(), sizeof(vp));
    if (valid) {
        for (float f : vp) {
            valid &= std::isfinite(f);
        }
        // Orthographic (sun light): the w row is (0, 0, 0, 1).
        valid &= vp[12] == 0.0f && vp[13] == 0.0f && vp[14] == 0.0f && vp[15] == 1.0f;
    }
    const auto& viewport = regs.viewports[0];
    const std::array<float, 3> scale{
        regs.viewport_control.xscale_enable ? viewport.xscale : 1.0f,
        regs.viewport_control.yscale_enable ? viewport.yscale : 1.0f,
        regs.viewport_control.zscale_enable ? viewport.zscale : 1.0f};
    // The image the cascade goes to: a moved cascade needs last frame's texels in the same image.
    VideoCore::ImageId image_id{};
    u64 image_uid = 0;
    if (valid) {
        const auto texture_lock = texture_cache.BatchLock();
        const auto htile = regs.depth_htile_data_base.GetAddress();
        VideoCore::TextureCache::ImageDesc desc(regs.depth_buffer, regs.depth_view,
                                                regs.depth_control, htile,
                                                liverpool->DrawDbExtent());
        image_id = texture_cache.FindImage(desc);
        image_uid = texture_cache.GetImage(image_id).image_uid;
    }
    if (s.decided[slice]) {
        // Another run of draws into this slice in the same frame.
        if (valid && vp == s.decided_vp[slice]) {
            return s.skip[slice]; // the same cascade, drawn in several runs
        }
        // Drawn again with another light matrix before the flip (a second scene in one frame, as
        // a slower machine may render): drawn as the game does, and a valid source next frame.
        ++s.rendered_again;
        ++s.drawn;
        s.skip[slice] = false;
        s.decided_vp[slice] = vp;
        s.last_vp[slice] = vp;
        s.last_scale[slice] = scale;
        s.have_vp[slice] = valid;
        s.drawn_now[slice] = true;
        s.last_image[slice] = image_id;
        s.last_image_uid[slice] = image_uid;
        return false;
    }
    s.decided[slice] = true;
    s.skip[slice] = false;
    s.decided_vp[slice] = vp;

    // Verification: the scratch image scrolled last frame against the cascade then drawn.
    if (mode == 2 && s.check_slice >= 0 && !s.check_done && shadow_scroll) {
        const auto texture_lock = texture_cache.BatchLock();
        const auto htile = regs.depth_htile_data_base.GetAddress();
        VideoCore::TextureCache::ImageDesc desc(regs.depth_buffer, regs.depth_view,
                                                regs.depth_control, htile,
                                                liverpool->DrawDbExtent());
        auto& image = texture_cache.GetImage(texture_cache.FindImage(desc));
        const char* dump_dir = std::getenv("BB_DUMP_DIR");
        const auto dir = std::filesystem::path(dump_dir ? dump_dir : ".");
        shadow_scroll->SaveCheck(
            image, static_cast<u32>(s.check_slice),
            (dir / ("shadow_check_slice" + std::to_string(s.check_slice))).string());
        s.check_done = true;
    }

    bool moved = false;
    const bool phase = (frame + slice) % 2 == 1; // each far cascade drawn every other frame
    if (!valid) {
        ++s.invalid;
    } else if (s.have_vp[slice] && s.drawn_prev[slice] && phase) {
        const auto& prev = s.last_vp[slice];
        // Last frame's texels are in the image only if it is the same one (not recreated).
        const bool same_image =
            image_id == s.last_image[slice] && image_uid == s.last_image_uid[slice];
        if (!same_image) {
            ++s.image_changed;
        }
        bool same_basis = same_image && s.last_scale[slice] == scale;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                same_basis &= vp[r * 4 + c] == prev[r * 4 + c];
            }
        }
        const double fx = double(scale[0]) * (double(vp[3]) - prev[3]);
        const double fy = double(scale[1]) * (double(vp[7]) - prev[7]);
        const float dz = static_cast<float>(double(scale[2]) * (double(vp[11]) - prev[11]));
        const double rx = std::round(fx), ry = std::round(fy);
        const bool whole = std::abs(fx - rx) < 0.02 && std::abs(fy - ry) < 0.02 &&
                           std::abs(rx) <= 128 && std::abs(ry) <= 128;
        static u32 logged = 0;
        if ((!same_basis || !whole) && std::getenv("SOTC_SHADOW_REUSE_STATS") && logged < 20) {
            ++logged;
            std::fprintf(stderr,
                         "SOTC SHADOW REUSE refused slice %u: basis %s, move %.4f,%.4f texels, dz "
                         "%g, scale %g %g %g (was %g %g %g)\n",
                         slice, same_basis ? "same" : "changed", fx, fy, dz, scale[0], scale[1],
                         scale[2], s.last_scale[slice][0], s.last_scale[slice][1],
                         s.last_scale[slice][2]);
        }
        if (same_basis && whole) {
            if (!shadow_scroll) {
                shadow_scroll = std::make_unique<ShadowScroll>(instance, scheduler, runtime);
            }
            const auto texture_lock = texture_cache.BatchLock();
            const auto htile = regs.depth_htile_data_base.GetAddress();
            VideoCore::TextureCache::ImageDesc desc(regs.depth_buffer, regs.depth_view,
                                                    regs.depth_control, htile,
                                                    liverpool->DrawDbExtent());
            auto& image = texture_cache.GetImage(texture_cache.FindImage(desc));
            const bool check = mode == 2;
            if (check && (s.check_slice >= 0 || (rx == 0 && ry == 0))) {
                // Verification done or pending: draw as usual.
            } else if (shadow_scroll->Scroll(image, slice, static_cast<s32>(rx),
                                             static_cast<s32>(ry), dz, regs.depth_clear, check)) {
                if (check) {
                    s.check_slice = static_cast<s32>(slice);
                    std::fprintf(stderr,
                                 "SOTC SHADOW CHECK slice %u frame %llu shift %d,%d dz %g "
                                 "(scale %g %g %g, clear %g, format %d)\n",
                                 slice, static_cast<unsigned long long>(frame), int(rx), int(ry),
                                 dz, scale[0], scale[1], scale[2], regs.depth_clear,
                                 static_cast<int>(image.info.pixel_format));
                } else {
                    moved = true;
                }
            }
        }
        if (!moved && mode != 2) {
            ++s.refused;
        }
    }
    s.last_vp[slice] = vp;
    s.last_scale[slice] = scale;
    s.have_vp[slice] = valid;
    s.last_image[slice] = image_id;
    s.last_image_uid[slice] = image_uid;
    s.skip[slice] = moved;
    s.drawn_now[slice] = !moved;
    ++(moved ? s.scrolled : s.drawn);
    return moved;
}

// sotc: TakeHudless (vk_rasterizer.h). An image of the video out size, created once per video out
// address in the format of the composite's attachment (mutable: the presenter reads it in the
// video out format).
static bool CreateHudlessImage(const Instance& instance, vk::Format format, u32 width, u32 height,
                               vk::ImageUsageFlags usage, vk::Image& image, vk::ImageView& view,
                               VmaAllocation& allocation) {
    const VkImageCreateInfo info = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = usage,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    const VmaAllocationCreateInfo alloc_info = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage raw{};
    if (vmaCreateImage(instance.GetAllocator(), &info, &alloc_info, &raw, &allocation, nullptr) !=
        VK_SUCCESS) {
        return false;
    }
    image = vk::Image{raw};
    auto [result, created] = instance.GetDevice().createImageView(vk::ImageViewCreateInfo{
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    });
    if (result != vk::Result::eSuccess) {
        vmaDestroyImage(instance.GetAllocator(), image, allocation);
        image = vk::Image{};
        return false;
    }
    view = created;
    return true;
}

void Rasterizer::DestroyHudless(HudlessImage& hudless) {
    const vk::Device device = instance.GetDevice();
    if (hudless.read_view) {
        device.destroyImageView(hudless.read_view);
    }
    if (hudless.rt_view) {
        device.destroyImageView(hudless.rt_view);
    }
    if (hudless.image) {
        vmaDestroyImage(instance.GetAllocator(), hudless.image, hudless.allocation);
    }
    hudless = {};
}

bool Rasterizer::BeginHudless(const GraphicsPipeline* pipeline, const RenderState& state) {
    // ponytail: one image per video out address, never resized: a video out of another size or
    // format (a resolution change) gets no HUD-less image; recreate it after a Finish if needed.
    auto& target = hudless_images[hudless_vo];
    if (!target.image) {
        if (!CreateHudlessImage(instance, hudless_vo_format, hudless_vo_width, hudless_vo_height,
                                vk::ImageUsageFlagBits::eColorAttachment |
                                    vk::ImageUsageFlagBits::eSampled,
                                target.image, target.rt_view, target.allocation)) {
            return false;
        }
        target.rt_format = hudless_vo_format;
        target.width = hudless_vo_width;
        target.height = hudless_vo_height;
    }
    if (target.rt_format != hudless_vo_format || target.width != hudless_vo_width ||
        target.height != hudless_vo_height) {
        return false;
    }
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::ImageSubresourceRange color_range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    if (!hudless_clear.image) {
        if (!CreateHudlessImage(instance, vk::Format::eR8G8B8A8Unorm, 1, 1,
                                vk::ImageUsageFlagBits::eSampled |
                                    vk::ImageUsageFlagBits::eTransferDst,
                                hudless_clear.image, hudless_clear.rt_view,
                                hudless_clear.allocation)) {
            return false;
        }
        const auto to_clear = vk::ImageMemoryBarrier2{
            .dstStageMask = vk::PipelineStageFlagBits2::eClear,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = hudless_clear.image,
            .subresourceRange = color_range,
        };
        cmdbuf.pipelineBarrier2(
            vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_clear});
        cmdbuf.clearColorImage(hudless_clear.image, vk::ImageLayout::eTransferDstOptimal,
                               vk::ClearColorValue{}, color_range);
        const auto to_read = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eClear,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = hudless_clear.image,
            .subresourceRange = color_range,
        };
        cmdbuf.pipelineBarrier2(
            vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    }
    // The composite writes every pixel: the last frame's content (read by the presenter) can go.
    const auto to_target = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = target.image,
        .subresourceRange = color_range,
    };
    cmdbuf.pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_target});
    auto& ui = image_infos[hudless_ui_info];
    ui.imageView = hudless_clear.rt_view;
    ui.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    pipeline->BindResources(set_writes, push_data);
    RenderState hudless_state = state;
    auto& attachment = hudless_state.color_attachments[0];
    attachment.image_view = target.rt_view;
    attachment.image_layout = vk::ImageLayout::eColorAttachmentOptimal;
    attachment.is_clear = 0;
    scheduler.BeginRendering(hudless_state);
    target.fresh = true;
    static bool reported = false;
    if (!std::exchange(reported, true)) {
        std::fprintf(stderr, "SOTC FG HUD-less: final composite into %#llx (%ux%u) redrawn without UI\n",
                     static_cast<unsigned long long>(hudless_vo), target.width, target.height);
    }
    return true;
}

vk::ImageView Rasterizer::TakeHudless(VAddr address, vk::Format format) {
    const auto it = hudless_images.find(address);
    if (it == hudless_images.end() || !std::exchange(it->second.fresh, false)) {
        return {};
    }
    auto& target = it->second;
    if (!target.read_view) {
        // As the presenter's view of the video out image: alpha excluded.
        auto [result, view] = instance.GetDevice().createImageView(vk::ImageViewCreateInfo{
            .image = target.image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .components = {.a = vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        });
        if (result != vk::Result::eSuccess) {
            return {};
        }
        target.read_view = view;
        target.read_format = format;
    }
    if (target.read_format != format) {
        return {}; // ponytail: one read format per video out image (its pixel format is fixed)
    }
    scheduler.EndRendering();
    const auto to_read = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = target.image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    scheduler.CommandBuffer().pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    return target.read_view;
}

bool Rasterizer::BeginUiShadow(const GraphicsPipeline* pipeline, const RenderState& state) {
    const u32 width = ui_shadow_width;
    const u32 height = ui_shadow_height;
    const float sx = static_cast<float>(width) / static_cast<float>(ui_shadow_src_width);
    const float sy = static_cast<float>(height) / static_cast<float>(ui_shadow_src_height);
    auto& dynamic = scheduler.GetDynamicState();
    ui_shadow_viewports = dynamic.viewports;
    ui_shadow_scissors = dynamic.scissors;
    Viewports viewports = dynamic.viewports;
    bool fits = true;
    for (auto& vp : viewports) {
        vp.x *= sx;
        vp.y *= sy;
        vp.width *= sx;
        vp.height *= sy;
        // Clipping off: the shader maps a 16K window, which scaled may exceed the limits.
        fits &= std::abs(vp.width) <= static_cast<float>(instance.GetMaxViewportWidth()) &&
                std::abs(vp.height) <= static_cast<float>(instance.GetMaxViewportHeight());
    }
    const bool one_sample = pipeline->GetGraphicsKey().color_samples[0] <= 1;
    if (!fits || !one_sample || attachment_feedback_loop || state.num_layers != 1) {
        static u32 reported = 0;
        if (reported++ < 5) {
            std::fprintf(stderr,
                         "SOTC UI: a draw into the UI layer cannot be redrawn (viewport fits %d, "
                         "1 sample %d, feedback loop %d, layers %u)\n",
                         fits, one_sample, attachment_feedback_loop, state.num_layers);
        }
        ui_shadow_broken = true;
        return false;
    }
    if (ui_shadow.width != width || ui_shadow.height != height) {
        if (ui_shadow.image) {
            (void)instance.GetDevice().waitIdle(); // earlier frames may still copy it
            DestroyHudless(ui_shadow);
        }
        ui_shadow_broken = true; // this frame's earlier draws are not in it
        if (!CreateHudlessImage(instance, vk::Format::eR8G8B8A8Srgb, width, height,
                                vk::ImageUsageFlagBits::eColorAttachment |
                                    vk::ImageUsageFlagBits::eTransferSrc,
                                ui_shadow.image, ui_shadow.rt_view, ui_shadow.allocation)) {
            return false;
        }
        auto [result, unorm_view] = instance.GetDevice().createImageView(vk::ImageViewCreateInfo{
            .image = ui_shadow.image,
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR8G8B8A8Unorm,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        });
        if (result != vk::Result::eSuccess) {
            DestroyHudless(ui_shadow);
            return false;
        }
        ui_shadow.read_view = unorm_view;
        ui_shadow.width = width;
        ui_shadow.height = height;
        std::fprintf(stderr, "SOTC UI: drawing the UI layer (%ux%u) at %ux%u\n",
                     ui_shadow_src_width, ui_shadow_src_height, width, height);
    }
    vk::ImageView view;
    if (ui_shadow_format == vk::Format::eR8G8B8A8Srgb) {
        view = ui_shadow.rt_view;
    } else if (ui_shadow_format == vk::Format::eR8G8B8A8Unorm) {
        view = ui_shadow.read_view;
    } else {
        ui_shadow_broken = true;
        return false;
    }
    scheduler.EndRendering();
    // The first draw of a frame starts it (the composite copied the last one); later ones blend
    // over the earlier ones.
    const bool first = ui_shadow_draws++ == 0;
    const auto to_target = vk::ImageMemoryBarrier2{
        .srcStageMask = first ? vk::PipelineStageFlagBits2::eAllCommands
                              : vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask =
            first ? vk::AccessFlagBits2::eNone : vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask =
            vk::AccessFlagBits2::eColorAttachmentWrite | vk::AccessFlagBits2::eColorAttachmentRead,
        .oldLayout = first ? vk::ImageLayout::eUndefined : vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = ui_shadow.image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    scheduler.CommandBuffer().pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_target});
    RenderState shadow = state;
    auto& color = shadow.color_attachments[0];
    if (first) {
        static u32 reported = 0;
        if (reported++ < 3) {
            const vk::Viewport vp = viewports.empty() ? vk::Viewport{} : viewports[0];
            std::fprintf(stderr,
                         "SOTC UI: first UI draw: clear %u, %u attachments, depth %d, "
                         "viewport %.0f,%.0f %.0fx%.0f\n",
                         color.is_clear, state.num_color_attachments,
                         state.depth_stencil_attachment.has_depth, vp.x, vp.y, vp.width,
                         vp.height);
        }
        if (!color.is_clear) {
            color.is_clear = 1;
            color.clear_value = {};
        }
    }
    color.image_view = view;
    color.image_layout = vk::ImageLayout::eColorAttachmentOptimal;
    for (u32 cb = 1; cb < shadow.num_color_attachments; ++cb) {
        shadow.color_attachments[cb].image_view = vk::ImageView{}; // of the render size
    }
    auto& depth = shadow.depth_stencil_attachment;
    depth.image_view = vk::ImageView{};
    depth.depth_clear = false;
    depth.stencil_clear = false;
    shadow.width = static_cast<u16>(width);
    shadow.height = static_cast<u16>(height);
    scheduler.BeginRendering(shadow);

    Scissors scissors = dynamic.scissors;
    for (auto& sc : scissors) {
        const s64 x0 = static_cast<s64>(std::floor(sc.offset.x * sx));
        const s64 y0 = static_cast<s64>(std::floor(sc.offset.y * sy));
        const s64 x1 = static_cast<s64>(std::ceil((sc.offset.x + s64(sc.extent.width)) * sx));
        const s64 y1 = static_cast<s64>(std::ceil((sc.offset.y + s64(sc.extent.height)) * sy));
        sc.offset = vk::Offset2D{static_cast<s32>(x0), static_cast<s32>(y0)};
        sc.extent = vk::Extent2D{static_cast<u32>(std::min<s64>(x1 - x0, 0x7fffffff - x0)),
                                 static_cast<u32>(std::min<s64>(y1 - y0, 0x7fffffff - y0))};
    }
    dynamic.SetViewports(viewports);
    dynamic.SetScissors(scissors);
    dynamic.Commit(instance, scheduler.CommandBuffer());
    return true;
}

void Rasterizer::EndUiShadow() {
    auto& dynamic = scheduler.GetDynamicState();
    dynamic.SetViewports(ui_shadow_viewports);
    dynamic.SetScissors(ui_shadow_scissors);
    dynamic.Commit(instance, scheduler.CommandBuffer());
}

void Rasterizer::NoteUiShadow() {
    static u32 reported = 0;
    if (ui_shadow_draws && reported++ < 5) {
        std::fprintf(stderr, "SOTC UI: composite after %u UI draws%s\n", ui_shadow_draws,
                     ui_shadow_broken ? ", not all redrawn: render size UI" : "");
    }
    const bool ok = ui_shadow_draws && !ui_shadow_broken;
    Dlssg::NoteUiShadow(ok ? ui_shadow.image : vk::Image{}, ui_shadow.width, ui_shadow.height);
    ui_shadow_draws = 0;
    ui_shadow_broken = false;
}

void Rasterizer::Draw(bool is_indexed, u32 index_offset, const Pipeline* resolved,
                      bool use_resolved) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();
    TracePass(liverpool->DrawRegs());

    if (!FilterDraw()) {
        return;
    }
    if (SkipStaggeredShadow(liverpool->DrawRegs()) || ReuseShadowCascade()) {
        return;
    }

    const auto& regs = liverpool->DrawRegs();
    const GraphicsPipeline* pipeline =
        use_resolved ? static_cast<const GraphicsPipeline*>(resolved)
                     : pipeline_cache.GetGraphicsPipeline();
    SotcStats::current_program.store(1, std::memory_order_relaxed);
    if (SotcStats::kCmdStats) { // sotc: read only by the SOTC_CMD_STATS printer (sotc_stats.h)
        SotcStats::cmd_kinds[0].fetch_add(1, std::memory_order_relaxed);
    }
    if (!pipeline) {
        SotcStats::Drop(0, regs.color_buffers[0].Address(), regs.stage_enable.raw);
        return;
    }
    TraceGbufferShaders(regs, pipeline);
    TraceShadowShaders(regs, pipeline);

    PrepareRenderState(pipeline);
    hudless_ui_info = -1; // sotc: TakeHudless, set by BindTextures and BeginRendering
    hudless_vo = 0;
    if (Dlssg::CapturingInputs()) { // sotc: camera and inputs of DLSS (frame generation, SR)
        Dlssg::NoteDraw(regs, pipeline->GetGraphicsKey().mrt_mask, db_desc.first,
                        cb_descs[4].first, texture_cache, runtime, scheduler);
    } else if (Dlssg::CameraLogOnly()) {
        Dlssg::NoteCamera(regs, pipeline->GetGraphicsKey().mrt_mask);
    }
    if (!BindResources(pipeline)) {
        SotcStats::Drop(1, regs.color_buffers[0].Address(), 0);
        return;
    }
    const auto state = BeginRendering(pipeline);
    RefreshImageLayouts();

    BindVertexBuffers(pipeline);
    if (is_indexed) {
        BindIndexBuffer(index_offset);
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    if (!fast_bind_hit) {
        pipeline->BindResources(set_writes, push_data);
    }
    UpdateDynamicState(pipeline, is_indexed);
    scheduler.BeginRendering(state);

    const auto& vs_info = pipeline->GetStage(Shader::SwStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    const auto [vertex_offset, instance_offset] = GetDrawOffsets(regs, vs_info, fetch_shader);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
                           s32(vertex_offset), instance_offset);
    } else {
        cmdbuf.draw(regs.num_indices, regs.num_instances.NumInstances(), vertex_offset,
                    instance_offset);
    }
    // sotc: the final composite again, without the UI layer (TakeHudless).
    if (hudless_ui_info >= 0 && hudless_vo && BeginHudless(pipeline, state)) {
        const auto hudless_cmdbuf = scheduler.CommandBuffer();
        if (is_indexed) {
            hudless_cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
                                       s32(vertex_offset), instance_offset);
        } else {
            hudless_cmdbuf.draw(regs.num_indices, regs.num_instances.NumInstances(),
                                vertex_offset, instance_offset);
        }
    }
    // sotc: a draw into the UI layer again, at the output size (BeginUiShadow).
    if (ui_shadow_hit && BeginUiShadow(pipeline, state)) {
        const auto shadow_cmdbuf = scheduler.CommandBuffer();
        if (is_indexed) {
            shadow_cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
                                      s32(vertex_offset), instance_offset);
        } else {
            shadow_cmdbuf.draw(regs.num_indices, regs.num_instances.NumInstances(), vertex_offset,
                               instance_offset);
        }
        EndUiShadow();
    }
    DebugState.IncDrawCall();

    ResetBindings(false);
    FlushPeriodic();
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address, u16 vertex_sgpr_offset,
                              u16 instance_sgpr_offset, const Pipeline* resolved,
                              bool use_resolved) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();
    TracePass(liverpool->DrawRegs());

    if (!FilterDraw()) {
        return;
    }
    if (SkipStaggeredShadow(liverpool->DrawRegs()) || ReuseShadowCascade()) {
        return;
    }

    if (SotcStats::kCmdStats) { // sotc: the shared SOTC_CMD_STATS switch (sotc_stats.h)
        const u64 args_size = u64{stride} * max_count;
        if (buffer_cache.IsRegionGpuModified(arg_address + offset, args_size) ||
            (count_address && buffer_cache.IsRegionGpuModified(count_address, 4))) {
            SotcStats::cmd_kinds[1].fetch_add(1, std::memory_order_relaxed);
        } else {
            SotcStats::cmd_kinds[2].fetch_add(1, std::memory_order_relaxed);
            u32 first[2]{1, 1};
            if (count_address == 0 && memory->TryReadBacking(arg_address + offset, first, 8) &&
                (first[0] == 0 || first[1] == 0)) {
                SotcStats::cmd_kinds[3].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    const DrawIndirectParams params = {
        .vertex_sgpr_offset = vertex_sgpr_offset,
        .instance_sgpr_offset = instance_sgpr_offset,
    };
    const GraphicsPipeline* pipeline =
        use_resolved ? static_cast<const GraphicsPipeline*>(resolved)
                     : pipeline_cache.GetGraphicsPipeline(params);
    if (!pipeline) {
        SotcStats::Drop(0, liverpool->DrawRegs().color_buffers[0].Address(), 1);
        return;
    }
    TraceShadowShaders(liverpool->DrawRegs(), pipeline);

    PrepareRenderState(pipeline);
    if (Dlssg::CapturingInputs()) { // sotc: camera and inputs of DLSS (frame generation, SR)
        Dlssg::NoteDraw(liverpool->DrawRegs(), pipeline->GetGraphicsKey().mrt_mask, db_desc.first,
                        cb_descs[4].first, texture_cache, runtime, scheduler);
    } else if (Dlssg::CameraLogOnly()) {
        Dlssg::NoteCamera(liverpool->DrawRegs(), pipeline->GetGraphicsKey().mrt_mask);
    }
    if (!BindResources(pipeline)) {
        SotcStats::Drop(1, liverpool->DrawRegs().color_buffers[0].Address(), 1);
        return;
    }
    const auto state = BeginRendering(pipeline);
    ui_shadow_broken |= ui_shadow_hit; // sotc: indirect draws are not redrawn (BeginUiShadow)
    RefreshImageLayouts();

    BindVertexBuffers(pipeline);
    if (is_indexed) {
        BindIndexBuffer();
    }

    // sotc: SotC writes the arguments of most indirect draws with WRITE_DATA right before them
    // (thousands per frame). With the arguments CPU-side, read them here and draw directly: no
    // upload of the argument range, no barrier against the next WRITE_DATA. The pipeline is the
    // same; the base vertex/instance reach the shader through the draw call in both cases.
    // SOTC_INDIRECT_DIRECT=0 keeps the indirect draws.
    static const bool direct_args = [] {
        const char* v = std::getenv("SOTC_INDIRECT_DIRECT");
        return !v || v[0] != '0';
    }();
    // Read through the backing view: the page may be read-protected for other GPU-written bytes.
    union {
        VkDrawIndexedIndirectCommand indexed;
        VkDrawIndirectCommand plain;
    } args;
    if (direct_args && count_address == 0 && max_count == 1 &&
        stride == (is_indexed ? sizeof(VkDrawIndexedIndirectCommand)
                              : sizeof(VkDrawIndirectCommand)) &&
        !buffer_cache.IsRegionGpuModified(arg_address + offset, stride) &&
        memory->TryReadBacking(arg_address + offset, &args, stride)) {
        if (needs_barrier) {
            runtime.FlushBarriers();
        }
        if (!fast_bind_hit) {
            pipeline->BindResources(set_writes, push_data);
        }
        UpdateDynamicState(pipeline, is_indexed);
        scheduler.BeginRendering(state);
        const auto cmdbuf = scheduler.CommandBuffer();
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());
        if (is_indexed) {
            const auto& a = args.indexed;
            cmdbuf.drawIndexed(a.indexCount, a.instanceCount, a.firstIndex, a.vertexOffset,
                               a.firstInstance);
        } else {
            const auto& a = args.plain;
            cmdbuf.draw(a.vertexCount, a.instanceCount, a.firstVertex, a.firstInstance);
        }
        DebugState.IncDrawCall();
        ResetBindings(false);
        FlushPeriodic();
        return;
    }

    // sotc: SOTC_INDIRECT_TRACE=1: the indirect draws that keep their GPU arguments (first 80):
    // where the arguments are and what guest memory holds there (missing GPU particles).
    static const bool indirect_trace = std::getenv("SOTC_INDIRECT_TRACE") != nullptr;
    if (indirect_trace) {
        static int shown = 0;
        if (shown++ < 80) {
            u32 first[4]{};
            const bool read = memory->TryReadBacking(arg_address + offset, first, sizeof(first));
            std::fprintf(stderr,
                         "SOTC indirect gpu %d read %d args %#llx+%#x stride %u max %u "
                         "count@%#llx mem %u %u %u %u vs %#llx\n",
                         int(buffer_cache.IsRegionGpuModified(arg_address + offset, stride)),
                         int(read), static_cast<unsigned long long>(arg_address), offset, stride,
                         max_count, static_cast<unsigned long long>(count_address), first[0],
                         first[1], first[2], first[3],
                         static_cast<unsigned long long>(
                             liverpool->DrawRegs().vs_program.Address<uintptr_t>()));
        }
    }
    const auto [buffer, base] =
        buffer_cache.ObtainBuffer(arg_address + offset, stride * max_count, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, stride * max_count);

    const VideoCore::Buffer* count_buffer;
    u64 count_offset;
    if (count_address != 0) {
        std::tie(count_buffer, count_offset) = buffer_cache.ObtainBuffer(count_address, 4, false);
        needs_barrier |= runtime.IsBufferAccessed(count_buffer, count_offset, 4);
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    if (!fast_bind_hit) {
        pipeline->BindResources(set_writes, push_data);
    }
    UpdateDynamicState(pipeline, is_indexed);
    scheduler.BeginRendering(state);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        ASSERT(sizeof(VkDrawIndexedIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndexedIndirectCount(buffer->Handle(), base, count_buffer->Handle(),
                                            count_offset, max_count, stride);
        } else {
            cmdbuf.drawIndexedIndirect(buffer->Handle(), base, max_count, stride);
        }
        DebugState.IncDrawCall();
    } else {
        ASSERT(sizeof(VkDrawIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndirectCount(buffer->Handle(), base, count_buffer->Handle(), count_offset,
                                     max_count, stride);
        } else {
            cmdbuf.drawIndirect(buffer->Handle(), base, max_count, stride);
        }
        DebugState.IncDrawCall();
    }

    // Record the indirect argument reads so a later write to the same range (e.g. a WRITE_DATA
    // packet uploading the next draw's arguments) waits for this draw instead of racing it.
    runtime.AccessBuffer(buffer, base, stride * max_count,
                         vk::PipelineStageFlagBits2::eDrawIndirect,
                         vk::AccessFlagBits2::eIndirectCommandRead);
    if (count_address != 0) {
        runtime.AccessBuffer(count_buffer, count_offset, 4,
                             vk::PipelineStageFlagBits2::eDrawIndirect,
                             vk::AccessFlagBits2::eIndirectCommandRead);
    }

    ResetBindings(false);
    FlushPeriodic();
}

// sotc: while DLSS takes the jittered frame (or SOTC_TAA_OFF=1), the game's TAA resolve runs
// with its history blend off: bit 0 of SRT dword 42. Its other work stays: exposure (the input
// colour times a scalar from a buffer) and the stencil mask in the output's alpha.
constexpr u64 kTaaResolveHash = 0xa402ddac;
constexpr u32 kTaaEnableDword = 42;
// sotc: the tonemap applies a screen space vignette: 1 + strength * (smoothstep(t) - 1),
// t = (scale * |uv - 0.5| - start) / (end - start), from flatbuf dwords 51..54. DLSS would drag
// it along with the scene (dark nested screen borders), so it is applied after upscaling.
constexpr u64 kTonemapHash = 0x291dbcd0;
// sotc: the game's HDR composite (UI over scene, ^k, Rec.2020, PQ at 300 nits).
constexpr u64 kHdrCompositeHash = 0xdfd767b6;
constexpr u32 kHdrCurveDword = 20; // its exponent k
constexpr u32 kVignetteDword = 51;
// sotc: the two grass compute shaders (grass blades; taller weeds). Their flatbufs hold the blade
// height scale and 7 view distances that thin the grass out in bands.
struct Vegetation {
    u64 hash;
    u32 height_dword;
    u32 bands_dword;
};
constexpr std::array<Vegetation, 2> kVegetation{{{0xfeaef49e, 32, 53}, {0x287f5d44, 30, 35}}};
constexpr u32 kVegetationBands = 7;
// sotc: the grass patch compute picks each patch's blade LOD from floor(k * ln(scale * size / area));
// a smaller scale gives the patches near the camera their full blade count sooner.
constexpr u64 kGrassPatchHash = 0x6172cbb0;
constexpr u32 kGrassPatchLodDword = 30;

// SOTC_GRASS="<grass distance> <grass height> <weed distance> <weed height> <patch LOD scale>"
// multipliers (launcher); the in-game menu overrides them live (SotcLive::grass).
static std::array<float, 5> VegetationScales() {
    static const std::array<float, 5> env = [] {
        std::array<float, 5> s{1.f, 1.f, 1.f, 1.f, 1.f};
        if (const char* v = std::getenv("SOTC_GRASS")) {
            std::sscanf(v, "%f %f %f %f %f", &s[0], &s[1], &s[2], &s[3], &s[4]);
        }
        return s;
    }();
    std::array<float, 5> s = env;
    for (size_t i = 0; i < s.size(); ++i) {
        if (const float live = SotcLive::grass[i].load(std::memory_order_relaxed); live >= 0.f) {
            s[i] = live;
        }
    }
    return s;
}

bool Rasterizer::TaaOff() {
    static const int forced = [] { // SOTC_TAA_OFF=1/0 forces; unset follows DLSS
        const char* v = std::getenv("SOTC_TAA_OFF");
        return v && *v ? (v[0] == '1' ? 1 : 0) : -1;
    }();
    // TAA comes back if super resolution is switched or turns itself off
    return forced < 0 ? Dlssg::SuperResolution() : forced == 1;
}

void Rasterizer::DispatchDirect(const Pipeline* resolved, bool use_resolved) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    const auto& cs_program = liverpool->DrawCsRegs();
    const ComputePipeline* pipeline = use_resolved ? static_cast<const ComputePipeline*>(resolved)
                                                   : pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        SotcStats::Drop(2, cs_program.Address<uintptr_t>(), 0);
        return;
    }

    const auto& cs = pipeline->GetStage(Shader::SwStage::Compute);
    SotcStats::current_program.store(cs.pgm_hash, std::memory_order_relaxed);
    if (SotcStats::kCmdStats) { // sotc: read only by the SOTC_CMD_STATS printer (sotc_stats.h)
        SotcStats::cmd_kinds[4].fetch_add(1, std::memory_order_relaxed);
    }
    // sotc: SOTC_CS_TRACE=<hash>: the buffers of the first 6 dispatches of that compute shader,
    // with the first dwords guest memory holds there (diagnostic for the GPU particle counts).
    static const u64 cs_trace = [] {
        const char* v = std::getenv("SOTC_CS_TRACE");
        return v ? std::strtoull(v, nullptr, 0) : u64{0};
    }();
    // SOTC_CS_WATCH=<n>: also report the GPU writes to that shader's buffer n (buffer_cache.cpp).
    static const int cs_watch = [] {
        const char* v = std::getenv("SOTC_CS_WATCH");
        return v ? std::atoi(v) : -1;
    }();
    if (cs_trace && cs.pgm_hash == cs_trace && cs_watch >= 0 &&
        SotcStats::watch_addr.load(std::memory_order_relaxed) == 0) {
        int n = 0;
        for (const auto& desc : cs.buffers) {
            if (!desc.IsSpecial() && n++ == cs_watch) {
                const VAddr a = desc.GetSharp(cs).base_address;
                SotcStats::watch_addr.store(a, std::memory_order_relaxed);
                std::fprintf(stderr, "SOTC cs watch armed at %#llx\n",
                             static_cast<unsigned long long>(a));
            }
        }
    }
    if (cs_trace && cs.pgm_hash == cs_trace) {
        static int shown = 0;
        if (shown++ < 6) {
            std::fprintf(stderr, "SOTC cs %#llx dispatch %ux%ux%u\n",
                         static_cast<unsigned long long>(cs.pgm_hash), cs_program.dim_x,
                         cs_program.dim_y, cs_program.dim_z);
            for (const auto& desc : cs.buffers) {
                if (desc.IsSpecial()) {
                    continue;
                }
                const auto sharp = desc.GetSharp(cs);
                u32 first[8]{};
                memory->TryReadBacking(sharp.base_address, first, sizeof(first));
                std::fprintf(stderr,
                             "SOTC cs   buf %#llx+%#llx written %d gpu %d mem %u %u %u %u %u %u %u "
                             "%u\n",
                             static_cast<unsigned long long>(sharp.base_address),
                             static_cast<unsigned long long>(sharp.GetSize()),
                             int(desc.is_written),
                             int(buffer_cache.IsRegionGpuModified(sharp.base_address, 32)),
                             first[0], first[1], first[2], first[3], first[4], first[5], first[6],
                             first[7]);
            }
        }
    }
    // SOTC_CMD_STATS=2: the dispatched shaders, every 10 s (command processor thread only).
    static const bool shader_stats = [] {
        const char* v = std::getenv("SOTC_CMD_STATS");
        return v && v[0] == '2';
    }();
    if (shader_stats) {
        static std::unordered_map<u64, u64> counts;
        static auto last = std::chrono::steady_clock::now();
        ++counts[cs.pgm_hash];
        if (const auto now = std::chrono::steady_clock::now(); now - last > std::chrono::seconds(10)) {
            last = now;
            std::vector<std::pair<u64, u64>> sorted;
            for (const auto& [hash, n] : counts) {
                sorted.emplace_back(n, hash);
            }
            std::sort(sorted.rbegin(), sorted.rend());
            std::string line = "SOTC dispatches in 10 s:";
            for (size_t i = 0; i < sorted.size() && i < 12; ++i) {
                line += fmt::format(" {:#x}={}", sorted[i].second, sorted[i].first);
            }
            std::fprintf(stderr, "%s\n", line.c_str());
            counts.clear();
        }
    }
    if (ExecuteShaderHLE(cs, liverpool->DrawRegs(), cs_program, *this)) {
        SotcStats::Drop(5, cs_program.Address<uintptr_t>(), 0);
        return;
    }

    if (!BindResources(pipeline)) {
        SotcStats::Drop(3, cs_program.Address<uintptr_t>(), 0);
        return;
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    scheduler.EndRendering();
    pipeline->BindResources(set_writes, push_data);

    const auto cmdbuf = scheduler.CommandBuffer();
    pipeline->BindWhenReady(); // sotc: may still be built by a worker (SOTC_ASYNC_COMPUTE)
    cmdbuf.dispatch(cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
    DebugState.IncDispatch();
    MaybeDumpImages(cs.pgm_hash);

    ResetBindings(true);
    FlushPeriodic();
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size, const Pipeline* resolved,
                                  bool use_resolved) {
    RENDERER_TRACE;
    if (SotcStats::kCmdStats) { // sotc: read only by the SOTC_CMD_STATS printer (sotc_stats.h)
        SotcStats::cmd_kinds[5].fetch_add(1, std::memory_order_relaxed);
    }

    scheduler.PopPendingOperations();

    const auto& cs_program = liverpool->DrawCsRegs();
    const ComputePipeline* pipeline = use_resolved ? static_cast<const ComputePipeline*>(resolved)
                                                   : pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        SotcStats::Drop(2, cs_program.Address<uintptr_t>(), 0);
        return;
    }

    if (!BindResources(pipeline)) {
        SotcStats::Drop(3, cs_program.Address<uintptr_t>(), 1);
        return;
    }

    const auto [buffer, base] = buffer_cache.ObtainBuffer(address + offset, size, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, size);

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    scheduler.EndRendering();
    pipeline->BindResources(set_writes, push_data);

    const auto cmdbuf = scheduler.CommandBuffer();
    pipeline->BindWhenReady(); // sotc: may still be built by a worker (SOTC_ASYNC_COMPUTE)
    cmdbuf.dispatchIndirect(buffer->Handle(), base);
    DebugState.IncDispatch();
    runtime.AccessBuffer(buffer, base, size, vk::PipelineStageFlagBits2::eDrawIndirect,
                         vk::AccessFlagBits2::eIndirectCommandRead);
    MaybeDumpImages(pipeline->GetStage(Shader::SwStage::Compute).pgm_hash);

    ResetBindings(true);
    FlushPeriodic();
}

u64 Rasterizer::Flush() {
    const u64 current_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return current_tick;
}

void Rasterizer::Finish() {
    scheduler.Finish();
}

void Rasterizer::OnSubmit() {
    buffer_cache.TickFrame();
    texture_cache.ProcessDownloadImages();
    texture_cache.RunGarbageCollector();
    runtime.TickFrame();
}

void Rasterizer::OnFence() {
    texture_cache.ProcessDownloadImages();
}

// sotc: the descriptors take each image's layout when its texture is bound, but BeginRendering may
// move a sampled image to another layout afterwards: a depth buffer both sampled and bound with
// stencil writes goes to DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL while its descriptor still said
// DEPTH_STENCIL_READ_ONLY_OPTIMAL (Vulkan validation; harmless on NVIDIA, wrong on GPUs that
// compress depth by layout). The descriptors are written after this, with the layouts in force.
void Rasterizer::RefreshImageLayouts() {
    for (size_t i = 0; i < image_infos.size(); ++i) {
        if (const VideoCore::ImageId id = image_info_images[i]) {
            image_infos[i].imageLayout = texture_cache.GetImage(id).backing->state.layout;
        }
    }
}

bool Rasterizer::FastBindEligible(const Pipeline* pipeline) const {
    if (!SotcStats::kFastBind || pipeline->IsCompute() || attachment_feedback_loop) {
        return false;
    }
    const auto& regs = liverpool->DrawRegs();
    if (regs.clipper_control.user_clip_plane_enable != 0) {
        return false;
    }
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        if (stage->has_storage_images || !stage->fmasks.empty()) {
            return false;
        }
        for (const auto& desc : stage->buffers) {
            // The stream-buffer bindings are only reusable while the ring has not wrapped;
            // StreamGeneration() in the fast-bind state checks that. Shared memory is compute.
            if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                return false;
            }
            if (!desc.IsSpecial() && desc.is_written) {
                return false;
            }
        }
        for (const auto& desc : stage->images) {
            if (desc.is_written) {
                return false;
            }
        }
    }
    return true;
}

u64 Rasterizer::FastBindHash(const Pipeline* pipeline, const Shader::PushData& push_data) const {
    const auto& regs = liverpool->DrawRegs();
    u64 hash = XXH3_64bits(&pipeline, sizeof(pipeline));
    hash = XXH3_64bits_withSeed(&push_data, sizeof(push_data), hash);
    for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        const VAddr address = regs.color_buffers[cb].Address();
        hash = XXH3_64bits_withSeed(&address, sizeof(address), hash);
    }
    const VAddr depth_address = regs.depth_buffer.DepthAddress();
    hash = XXH3_64bits_withSeed(&depth_address, sizeof(depth_address), hash);
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        const auto user_data = stage->UserData();
        if (!user_data.empty()) {
            hash = XXH3_64bits_withSeed(user_data.data(), user_data.size_bytes(), hash);
        }
        const auto& flat_buf = stage->FlatBuf();
        if (!flat_buf.empty()) {
            hash = XXH3_64bits_withSeed(flat_buf.data(), flat_buf.size() * sizeof(u32), hash);
        }
    }
    return hash;
}

void Rasterizer::CommitFastBind(const Pipeline* pipeline, u64 hash, bool uses_dma) {
    fast_bind.pipeline = pipeline;
    fast_bind.hash = hash;
    fast_bind.buffer_generation = buffer_cache.BindGeneration();
    fast_bind.texture_generation = texture_cache.BindGeneration();
    fast_bind.stream_generation = buffer_cache.StreamGeneration();
    fast_bind.mem_epoch = SotcStats::mem_epoch.load(std::memory_order_relaxed);
    fast_bind.tick = scheduler.CurrentTick();
    fast_bind.uses_dma = uses_dma;
    fast_bind.valid = true;
}

bool Rasterizer::BindResources(const Pipeline* pipeline) {
    fast_bind_hit = false;
    if (IsComputeImageCopy(pipeline) || IsComputeMetaClear(pipeline) ||
        IsComputeImageClear(pipeline)) {
        static const bool drop_trace = std::getenv("SOTC_DROP_TRACE") != nullptr;
        if (drop_trace) {
            static int shown = 0;
            if (shown++ < 12) {
                const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
                for (const auto& desc : info.buffers) {
                    if (desc.IsSpecial()) {
                        continue;
                    }
                    const auto sharp = desc.GetSharp(info);
                    std::fprintf(stderr,
                                 "SOTC drop-detail cs %#llx buf %#llx+%#llx written %d fmt %d "
                                 "meta %d xor %d images %zu\n",
                                 static_cast<unsigned long long>(info.pgm_hash),
                                 static_cast<unsigned long long>(sharp.base_address),
                                 static_cast<unsigned long long>(sharp.GetSize()),
                                 int(desc.is_written), int(desc.is_formatted),
                                 int(texture_cache.IsMeta(sharp.base_address).has_value()),
                                 int(info.has_bitwise_xor), info.images.size());
                }
            }
        }
        return false;
    }

    set_write_index = 0;
    set_writes.clear();
    buffer_infos.clear();
    image_infos.clear();

    bool uses_dma = false;

    // A stage whose user data was never attached (e.g. a program restored from the pipeline
    // cache that was not refreshed for this draw) would read its SGPRs from a null span.
    for (const auto* stage : pipeline->GetStages()) {
        if (stage && stage->UserData().data() == nullptr &&
            stage->srt_info.flattened_bufsize_dw != 0) {
            LOG_ERROR(Render_Vulkan,
                      "Skipping draw: shader {:#x} (sw stage {}, hw stage {}, info {}) has no "
                      "user data bound",
                      stage->pgm_hash, u32(stage->sw_stage), u32(stage->hw_stage),
                      fmt::ptr(stage));
            fast_bind.valid = false;
            return false;
        }
    }

    // sotc: the previous draw left these descriptors and push constants bound. Skip rebuilding
    // them when this draw needs exactly the same pipeline, push data, render targets and
    // resource handles. The generation bumps catch reallocated buffers/images and dead samplers.
    const bool fast_eligible = FastBindEligible(pipeline);
    u64 fast_hash = 0;
    if (fast_eligible) {
        SotcStats::fast_bind_eligible.fetch_add(1, std::memory_order_relaxed);
        const auto input_push = MakeUserData(liverpool->DrawRegs());
        fast_hash = FastBindHash(pipeline, input_push);
        if (fast_bind.valid && fast_bind.pipeline == pipeline &&
            fast_bind.tick == scheduler.CurrentTick() &&
            fast_bind.buffer_generation == buffer_cache.BindGeneration() &&
            fast_bind.texture_generation == texture_cache.BindGeneration() &&
            fast_bind.stream_generation == buffer_cache.StreamGeneration() &&
            fast_bind.mem_epoch == SotcStats::mem_epoch.load(std::memory_order_relaxed) &&
            fast_bind.hash == fast_hash) {
            fast_bind_hit = true;
            SotcStats::fast_bind_hits.fetch_add(1, std::memory_order_relaxed);
            if (fast_bind.uses_dma) {
                buffer_cache.SynchronizeDmaBuffers();
            }
            return true;
        }
        push_data = input_push;
    } else {
        fast_bind.valid = false;
        push_data = MakeUserData(liverpool->DrawRegs());
    }
    // Bind resource buffers and textures.
    Shader::Backend::Bindings binding{};
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        set_writes.resize(set_writes.size() + stage->buffers.size() + stage->images.size() +
                          stage->samplers.size());
        BindBuffers(*stage, binding, push_data);
        BindTextures(*stage, binding);
        uses_dma |= stage->uses_dma;
    }

    if (uses_dma) {
        buffer_cache.SynchronizeDmaBuffers();
    }

    if (fast_eligible) {
        CommitFastBind(pipeline, fast_hash, uses_dma);
    }

    return true;
}

void Rasterizer::BindVertexBuffers(const GraphicsPipeline* pipeline) {
    const auto& regs = liverpool->DrawRegs();
    VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
    VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    VertexInputs<AmdGpu::Buffer> guest_buffers;
    pipeline->GetVertexInputs(attributes, bindings, divisors, guest_buffers,
                              regs.vgt_instance_step_rate_0, regs.vgt_instance_step_rate_1);

    if (instance.IsVertexInputDynamicState()) {
        // Update current vertex inputs.
        const auto cmdbuf = scheduler.CommandBuffer();
        cmdbuf.setVertexInputEXT(bindings, attributes);
    }

    if (bindings.empty()) {
        // If there are no bindings, there is nothing further to do.
        return;
    }

    struct BufferRange {
        VAddr base_address;
        VAddr end_address;
        const VideoCore::Buffer* buffer;
        u64 offset;

        [[nodiscard]] size_t GetSize() const {
            return end_address - base_address;
        }
    };

    // Build list of ranges covering the requested buffers
    VertexInputs<BufferRange> ranges{};
    for (const auto& buffer : guest_buffers) {
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            ranges.emplace_back(buffer.base_address, buffer.base_address + buffer.GetSize());
        }
    }

    // Merge connecting ranges together
    VertexInputs<BufferRange> ranges_merged{};
    if (!ranges.empty()) {
        std::ranges::sort(ranges, [](const BufferRange& lhv, const BufferRange& rhv) {
            return lhv.base_address < rhv.base_address;
        });
        ranges_merged.emplace_back(ranges[0]);
        for (auto range : ranges) {
            auto& prev_range = ranges_merged.back();
            if (prev_range.end_address < range.base_address) {
                ranges_merged.emplace_back(range);
            } else {
                prev_range.end_address = std::max(prev_range.end_address, range.end_address);
            }
        }
    }

    // Map buffers for merged ranges
    for (auto& range : ranges_merged) {
        u64 size = memory->ClampRangeSize(range.base_address, range.GetSize());
        if (size == 0) {
            // Start is not mapped (garbage vertex sharp): keep a small range instead of GBs.
            size = std::min<u64>(range.GetSize(), 4096);
        }
        std::tie(range.buffer, range.offset) =
            buffer_cache.ObtainBuffer(range.base_address, size, false);
        needs_barrier |= runtime.IsBufferAccessed(range.buffer, range.offset, size);
        fixed_function_reads.push_back({range.buffer, range.offset, static_cast<u32>(size), false});
    }

    // Bind vertex buffers
    VertexInputs<vk::Buffer> host_buffers;
    VertexInputs<vk::DeviceSize> host_offsets;
    VertexInputs<vk::DeviceSize> host_sizes;
    VertexInputs<vk::DeviceSize> host_strides;
    for (const auto& buffer : guest_buffers) {
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            const auto host_buffer_info =
                std::ranges::find_if(ranges_merged, [&](const BufferRange& range) {
                    return buffer.base_address >= range.base_address &&
                           buffer.base_address < range.end_address;
                });
            ASSERT(host_buffer_info != ranges_merged.cend());
            host_buffers.emplace_back(host_buffer_info->buffer->Handle());
            host_offsets.push_back(host_buffer_info->offset + buffer.base_address -
                                   host_buffer_info->base_address);
        } else {
            host_buffers.emplace_back(VK_NULL_HANDLE);
            host_offsets.push_back(0);
        }
        host_sizes.push_back(buffer.GetSize());
        host_strides.push_back(buffer.GetStride());
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    const auto num_buffers = guest_buffers.size();
    if (instance.IsVertexInputDynamicState()) {
        cmdbuf.bindVertexBuffers(0, num_buffers, host_buffers.data(), host_offsets.data());
    } else {
        cmdbuf.bindVertexBuffers2(0, num_buffers, host_buffers.data(), host_offsets.data(),
                                  host_sizes.data(), host_strides.data());
    }
}

void Rasterizer::BindIndexBuffer(u32 index_offset) {
    const auto& regs = liverpool->DrawRegs();

    // Figure out index type and size.
    const bool is_index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    const vk::IndexType index_type = is_index16 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
    const VAddr index_address =
        regs.index_base_address.Address<VAddr>() + index_offset * index_size;

    // Bind index buffer.
    const u32 index_buffer_size = regs.num_indices * index_size;
    const auto [buffer, offset] =
        buffer_cache.ObtainBuffer(index_address, index_buffer_size, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, offset, index_buffer_size);
    fixed_function_reads.push_back({buffer, offset, index_buffer_size, false});
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindIndexBuffer(buffer->Handle(), offset, index_type);
}

void Rasterizer::FlushPeriodic() {
    // Readbacks drain the GPU, but between them the GPU only gets work when a readback submits:
    // it sits idle while the command processor records. Submitting every N draws/dispatches lets
    // it run the frame during the recording. A submit inside a render pass cuts it, so wait for a
    // command outside one, up to 4 * N. From periodic_flush_commands of Pink-shadPS4
    // (luizgustavs). SOTC_FLUSH_EVERY=N sets it, 0 = off.
    static const u32 every = [this] {
        if (const char* v = std::getenv("SOTC_FLUSH_EVERY")) {
            return static_cast<u32>(std::strtoul(v, nullptr, 10));
        }
        // An explicit SOTC_FLUSH_US turns the periodic submit on for any GPU (the launcher's
        // option for AMD and Intel cards, not tested there).
        if (const char* v = std::getenv("SOTC_FLUSH_US"); v && std::strtoull(v, nullptr, 10) != 0) {
            return 256u;
        }
        // SotC: +20% fps in the open world (14-15 -> 18). Other games keep the upstream behavior.
        // Pink-shadPS4 advises turning it off on AMD (black screen); with readbacks ahead on a
        // Radeon RX 6650 XT the image is right and the sanctuary goes from 17.4 to 22.4 fps.
        constexpr u32 VendorNvidia = 0x10DE;
        constexpr u32 VendorAmd = 0x1002;
        return Common::ElfInfo::Instance().IsSotc() &&
                       (instance.GetVendorID() == VendorNvidia ||
                        instance.GetVendorID() == VendorAmd)
                   ? 256u
                   : 0u;
    }();
    // sotc: the command buffer wrote a page the guest reads back: submit it now, the GPU runs
    // it while the guest gets there, and the readback finds it done (or nearly). Within noise
    // when the command processor was slower; on by default since 2026-10-16 (SOTC_RB_EARLY=0
    // off): attract demo 53.6/54.5 -> 55.3/55.7 FPS (open world: 400-490 readbacks/s waited
    // ~10% of the thread), Shrine unchanged (34.2 vs 34.1).
    if (SotcStats::kFpsLog) { // sotc: read only by the SOTC_FPS printer (sotc_stats.h)
        // sotc: written by this thread only: a plain add, no lock prefix (an atomic RMW here
        // waits for the thread's pending stores, ~1.8% of it). The printer takes deltas.
        SotcStats::commands.store(SotcStats::commands.load(std::memory_order_relaxed) + 1,
                                  std::memory_order_relaxed);
    }
    static const bool early = [] {
        const char* v = std::getenv("SOTC_RB_EARLY");
        return !v || v[0] != '0';
    }();
    if (early && !scheduler.IsRendering() && buffer_cache.TakeHotWrite()) {
        SotcStats::early_flushes.fetch_add(1, std::memory_order_relaxed);
        scheduler.Flush();
        return;
    }
    if (every == 0) {
        // sotc: without periodic submits, look for cold arena memory every 1024 commands.
        if ((++evict_poll & 1023) == 0 && !scheduler.IsRendering()) {
            buffer_cache.MaybeEvictCold(std::chrono::steady_clock::now());
        }
        return;
    }
    // sotc: submit by recording time instead (SOTC_FLUSH_US, default 1000 where the periodic
    // submit is on and SOTC_FLUSH_EVERY is not set): the GPU gets work at a steady pace whether
    // a scene has few heavy commands or thousands of light ones. Attract demo: +10-15% in most
    // scenes, canyon bridge +2% (count based: 64 helped the first and hurt the second).
    // 1000 instead of 500 (2026-10-15, RTX 3060 laptop, idle shrine ~5800 draws/frame): about +8%
    // in a single unpaired run (250: no gain, 2000: same as 1000): fewer submits, each one
    // cheaper per draw.
    static const u64 flush_us = [] {
        if (const char* v = std::getenv("SOTC_FLUSH_US")) {
            return std::strtoull(v, nullptr, 10);
        }
        return std::getenv("SOTC_FLUSH_EVERY") ? u64{0} : u64{1000};
    }();
    const u64 tick = scheduler.CurrentTick();
    if (tick != commands_tick) {
        commands_tick = tick;
        commands_since_submit = 0;
        commands_since = std::chrono::steady_clock::now();
    }
    ++commands_since_submit;
    if (flush_us != 0) {
        // sotc: the clock is read every 8th command (~40 us at the Shrine against 1000 us),
        // not on every one (~0.5% of the command processor in QueryPerformanceCounter).
        if ((commands_since_submit & 7) != 0) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = now - commands_since;
        const auto limit = std::chrono::microseconds(flush_us);
        if (elapsed < limit || (scheduler.IsRendering() && elapsed < limit * 4)) {
            return;
        }
        // sotc: cold arena memory goes back at a submit point outside a render pass.
        if (!scheduler.IsRendering() && buffer_cache.MaybeEvictCold(now)) {
            return;
        }
        scheduler.Flush();
        return;
    }
    if (commands_since_submit < every ||
        (scheduler.IsRendering() && commands_since_submit < u64{every} * 4)) {
        return;
    }
    if (!scheduler.IsRendering() &&
        buffer_cache.MaybeEvictCold(std::chrono::steady_clock::now())) {
        return;
    }
    scheduler.Flush();
}

// SOTC_DUMP="hash,hash,..." (hex, default: the SotC volumetric fog passes): after F12 (without
// RenderDoc), the next dispatch of each listed compute shader dumps every image it has bound to
// user/log/dump_N/ (raw mip 0, all layers and slices). The data is what the GPU really computed,
// unlike a RenderDoc replay of a frame that uses the sparse guest-memory buffer.
void Rasterizer::MaybeDumpImages(u64 pgm_hash) {
    // sotc: SOTC_CS_DUMP="hash,hash": after those compute shaders, read the buffer armed by
    // SOTC_CS_WATCH back from the GPU and print its first 24 dwords (first 60 times; slow).
    static const std::vector<u64> dump_after = [] {
        std::vector<u64> list;
        if (const char* env = std::getenv("SOTC_CS_DUMP")) {
            for (const char* p = env; *p;) {
                char* end = nullptr;
                const u64 v = std::strtoull(p, &end, 0);
                if (end == p) {
                    break;
                }
                list.push_back(v);
                p = *end == ',' ? end + 1 : end;
            }
        }
        return list;
    }();
    if (!dump_after.empty() &&
        std::find(dump_after.begin(), dump_after.end(), pgm_hash) != dump_after.end()) {
        static int shown = 0;
        const VAddr watch = SotcStats::watch_addr.load(std::memory_order_relaxed);
        if (watch && shown++ < 60) {
            buffer_cache.ReadMemory(watch, 24 * sizeof(u32));
            u32 v[24]{};
            memory->TryReadBacking(watch, v, sizeof(v));
            std::fprintf(stderr, "SOTC dump after %#llx:", static_cast<unsigned long long>(pgm_hash));
            for (const u32 x : v) {
                std::fprintf(stderr, " %u", x);
            }
            std::fprintf(stderr, "\n");
        }
    }
    static const std::vector<u64> targets = [] {
        std::vector<u64> list;
        const char* env = std::getenv("SOTC_DUMP");
        if (!env) {
            return list;
        }
        std::string spec = env;
        if (spec.empty() || spec == "1") {
            spec = "b33e9db6,91c6c95e,25338711,47c1dc64";
        }
        size_t pos = 0;
        while (pos < spec.size()) {
            const size_t end = std::min(spec.find(',', pos), spec.size());
            list.push_back(std::strtoull(spec.substr(pos, end - pos).c_str(), nullptr, 16));
            pos = end + 1;
        }
        return list;
    }();
    static std::vector<u64> pending;
    static std::filesystem::path dir;
    static u32 session = 0;
    static u32 seq = 0;
    if (targets.empty()) {
        return;
    }
    if (VideoCore::ConsumeImageDumpRequest()) {
        pending = targets;
        dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / fmt::format("dump_{}", ++session);
        std::filesystem::create_directories(dir);
        seq = 0;
    }
    const auto it = std::find(pending.begin(), pending.end(), pgm_hash);
    if (it == pending.end()) {
        return;
    }
    pending.erase(it);
    std::vector<VideoCore::ImageId> seen;
    u32 index = 0;
    for (const auto image_id : bound_images) {
        if (!image_id || std::find(seen.begin(), seen.end(), image_id) != seen.end()) {
            continue;
        }
        seen.push_back(image_id);
        texture_cache.DumpImage(image_id, dir / fmt::format("{:02}_cs_{:08x}_img{}", seq, pgm_hash,
                                                             index++));
    }
    ++seq;
}

void Rasterizer::ResetBindings(bool is_compute) {
    for (auto& image_id : bound_images) {
        texture_cache.GetImage(image_id).binding = {};
    }
    for (const auto [buffer, offset, size, is_written] : bound_buffers) {
        const auto dst_stage = is_compute ? vk::PipelineStageFlagBits2::eComputeShader
                                          : vk::PipelineStageFlagBits2::eAllGraphics;
        const auto write_flag =
            is_written ? vk::AccessFlagBits2::eShaderWrite : vk::AccessFlagBits2::eNone;
        runtime.AccessBuffer(buffer, offset, size, dst_stage,
                             vk::AccessFlagBits2::eShaderRead | write_flag);
    }
    for (const auto& read : fixed_function_reads) {
        runtime.AccessBuffer(read.buffer, read.offset, read.size,
                             vk::PipelineStageFlagBits2::eVertexInput,
                             vk::AccessFlagBits2::eVertexAttributeRead |
                                 vk::AccessFlagBits2::eIndexRead);
    }
    fixed_function_reads.clear();
    bound_images.clear();
    bound_buffers.clear();
    needs_barrier = false;
}

bool Rasterizer::IsComputeMetaClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }
    // sotc: SOTC_META_CLEAR=0 runs every dispatch the heuristic below would skip (test: a
    // buffer reused from an old image's metadata range makes a real dispatch look like a clear).
    static const bool meta_clear = [] {
        const char* v = std::getenv("SOTC_META_CLEAR");
        return !v || v[0] != '0';
    }();
    if (!meta_clear) {
        return false;
    }

    // Most of the time when a metadata is updated with a shader it gets cleared. It means
    // we can skip the whole dispatch and update the tracked state instead. Also, it is not
    // intended to be consumed and in such rare cases (e.g. HTile introspection, CRAA) we
    // will need its full emulation anyways.
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);

    // Assume if a shader reads metadata, it is a copy shader.
    for (const auto& desc : info.buffers) {
        const VAddr address = desc.GetSharp(info).base_address;
        if (!desc.IsSpecial() && !desc.is_written && texture_cache.IsMeta(address)) {
            return false;
        }
    }

    // Metadata surfaces are tiled and thus need address calculation to be written properly.
    // If a shader wants to encode HTILE, for example, from a depth image it will have to compute
    // proper tile address from dispatch invocation id. This address calculation contains an xor
    // operation so use it as a heuristic for metadata writes that are probably not clears.
    if (!info.has_bitwise_xor) {
        // Assume if a shader writes metadata without address calculation, it is a clear shader.
        for (const auto& desc : info.buffers) {
            const VAddr address = desc.GetSharp(info).base_address;
            if (!desc.IsSpecial() && desc.is_written && texture_cache.ClearMeta(address)) {
                // Assume all slices were updates
                LOG_TRACE(Render_Vulkan, "Metadata update skipped");
                return true;
            }
        }
    }
    return false;
}

bool Rasterizer::IsComputeImageCopy(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->DrawCsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // Those 2 buffers must both be formatted. One must be source and another destination.
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (!desc0.is_formatted || !desc1.is_formatted || desc0.is_written == desc1.is_written) {
        return false;
    }

    // Buffers must have the same size and each thread of the dispatch must copy 1 dword of data
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    if (buf0.GetSize() != buf1.GetSize() || cs_pgm.dim_x != (buf0.GetSize() / 256)) {
        return false;
    }

    // Find images the buffer alias
    const auto image0_id = texture_cache.FindImageFromRange(buf0.base_address, buf0.GetSize());
    if (!image0_id) {
        return false;
    }
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image copy must be valid
    VideoCore::Image& image0 = texture_cache.GetImage(image0_id);
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image0.info.guest_size != image1.info.guest_size ||
        image0.info.pitch != image1.info.pitch || image0.info.guest_size != buf0.GetSize() ||
        image0.info.num_bits != image1.info.num_bits) {
        return false;
    }

    // Perform image copy
    VideoCore::Image& src_image = desc0.is_written ? image1 : image0;
    VideoCore::Image& dst_image = desc0.is_written ? image0 : image1;
    // sotc: the destination was found by address, not by a writer lookup: its first GPU write
    // moves the mip-tail epoch (CopyColorAndDepth only passes the flag on).
    VideoCore::MarkGpuModified(dst_image);
    runtime.CopyColorAndDepth(&src_image, &dst_image);
    return true;
}

bool Rasterizer::IsComputeImageClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->DrawCsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // From those 2 buffers, first must hold the clear vector and second the image being cleared
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (desc0.is_formatted || !desc1.is_formatted || desc0.is_written || !desc1.is_written) {
        return false;
    }

    // First buffer must have size of vec4 and second the size of a single layer
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    const u32 buf1_bpp = AmdGpu::NumBitsPerBlock(buf1.GetDataFmt());
    if (buf0.GetSize() != 16 || (cs_pgm.dim_x * 128ULL * (buf1_bpp / 8)) != buf1.GetSize()) {
        return false;
    }

    // Find image the buffer alias
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image clear must be valid
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image1.info.guest_size != buf1.GetSize() || image1.info.num_bits != buf1_bpp ||
        image1.info.props.is_depth) {
        return false;
    }

    // Perform image clear
    const float* values = reinterpret_cast<float*>(buf0.base_address);
    const vk::ClearValue clear = {
        .color = {.float32 = std::array<float, 4>{values[0], values[1], values[2], values[3]}},
    };
    const VideoCore::SubresourceRange range = {
        .base =
            {
                .level = 0,
                .layer = 0,
            },
        .extent = image1.info.resources,
    };
    runtime.ClearImage(&image1, range, clear);
    return true;
}

void Rasterizer::BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                             Shader::PushData& push_data) {
    const u64 alignment = instance.StorageMinAlignment();
    for (const auto& desc : stage.buffers) {
        if (desc.IsSpecial()) {
            if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
                const auto* gds_buf = buffer_cache.GetGdsBuffer();
                buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
                needs_barrier |=
                    runtime.IsBufferAccessed(gds_buf, 0, gds_buf->SizeBytes(), desc.is_written);
                bound_buffers.emplace_back(gds_buf, 0, gds_buf->SizeBytes(), desc.is_written);
            } else if (desc.buffer_type == Shader::BufferType::Flatbuf) {
                auto& vk_buffer = buffer_cache.GetStreamBuffer();
                const u32 ubo_size = stage.FlatBuf().size() * sizeof(u32);
                const u32* flat = stage.FlatBuf().data();
                static std::vector<u32> taa_flat;
                if (stage.pgm_hash == kTaaResolveHash && stage.FlatBuf().size() > kTaaEnableDword &&
                    TaaOff()) {
                    taa_flat.assign(stage.FlatBuf().begin(), stage.FlatBuf().end());
                    taa_flat[kTaaEnableDword] &= ~1u;
                    flat = taa_flat.data();
                    static bool reported = false;
                    if (!std::exchange(reported, true)) {
                        std::fprintf(stderr, "SOTC TAA off: resolve without history blend\n");
                    }
                }
                static std::vector<u32> tonemap_flat;
                static const bool vignette_after = [] {
                    const char* v = std::getenv("SOTC_DLSS_VIGNETTE");
                    return !v || v[0] != '0';
                }();
                if (stage.pgm_hash == kTonemapHash && stage.FlatBuf().size() > kVignetteDword + 3 &&
                    vignette_after && Dlssg::SuperResolution()) {
                    tonemap_flat.assign(stage.FlatBuf().begin(), stage.FlatBuf().end());
                    float params[4];
                    std::memcpy(params, &tonemap_flat[kVignetteDword], sizeof(params));
                    Dlssg::NoteVignette(params);
                    tonemap_flat[kVignetteDword + 3] = 0; // strength 0.0f
                    flat = tonemap_flat.data();
                    static bool reported = false;
                    if (!std::exchange(reported, true)) {
                        std::fprintf(stderr,
                                     "SOTC DLSS: vignette after upscaling (%g %g %g %g)\n",
                                     params[0], params[1], params[2], params[3]);
                    }
                }
                const auto veg_scales = VegetationScales();
                for (size_t v = 0; v < kVegetation.size(); ++v) {
                    const auto& veg = kVegetation[v];
                    const float dist = veg_scales[v * 2], height = veg_scales[v * 2 + 1];
                    if (stage.pgm_hash != veg.hash || (dist == 1.f && height == 1.f) ||
                        stage.FlatBuf().size() < veg.bands_dword + kVegetationBands) {
                        continue;
                    }
                    static std::vector<u32> veg_flat;
                    veg_flat.assign(flat, flat + stage.FlatBuf().size());
                    const auto scale = [&](u32 dword, float m) {
                        veg_flat[dword] = std::bit_cast<u32>(std::bit_cast<float>(veg_flat[dword]) * m);
                    };
                    scale(veg.height_dword, height);
                    for (u32 i = 0; i < kVegetationBands; ++i) {
                        scale(veg.bands_dword + i, dist);
                    }
                    flat = veg_flat.data();
                }
                if (const float lod = veg_scales[4]; stage.pgm_hash == kGrassPatchHash &&
                                                             lod != 1.f &&
                                                             stage.FlatBuf().size() > kGrassPatchLodDword) {
                    static std::vector<u32> patch_flat;
                    patch_flat.assign(flat, flat + stage.FlatBuf().size());
                    patch_flat[kGrassPatchLodDword] =
                        std::bit_cast<u32>(std::bit_cast<float>(patch_flat[kGrassPatchLodDword]) * lod);
                    flat = patch_flat.data();
                }
                if (stage.pgm_hash == kHdrCompositeHash &&
                    stage.FlatBuf().size() > kHdrCurveDword) {
                    Dlssg::NoteHdrCurve(std::bit_cast<float>(flat[kHdrCurveDword]));
                }
                const u64 offset = vk_buffer.Copy(flat, ubo_size, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
            } else if (desc.buffer_type == Shader::BufferType::ClipPlanes) {
                // Permutations compiled without enabled planes never read the buffer, so the
                // declared binding is satisfied with a null descriptor instead of a copy.
                if (liverpool->DrawRegs().clipper_control.user_clip_plane_enable == 0) {
                    buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
                } else {
                    auto& vk_buffer = buffer_cache.GetStreamBuffer();
                    std::array<float, AmdGpu::NUM_CLIP_PLANES * 4> planes{};
                    for (u32 i = 0; i < AmdGpu::NUM_CLIP_PLANES; ++i) {
                        const auto& plane = liverpool->DrawRegs().clip_user_data[i];
                        planes[i * 4 + 0] = std::bit_cast<float>(plane.data_x);
                        planes[i * 4 + 1] = std::bit_cast<float>(plane.data_y);
                        planes[i * 4 + 2] = std::bit_cast<float>(plane.data_z);
                        planes[i * 4 + 3] = std::bit_cast<float>(plane.data_w);
                    }
                    const u32 ubo_size = static_cast<u32>(sizeof(planes));
                    const u64 offset = vk_buffer.Copy(planes.data(), ubo_size, alignment);
                    buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
                }
            } else if (desc.buffer_type == Shader::BufferType::BdaPagetable) {
                const auto* bda_buffer = buffer_cache.GetBdaPageTableBuffer();
                buffer_infos.emplace_back(bda_buffer->Handle(), 0, bda_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::FaultBuffer) {
                const auto* fault_buffer = buffer_cache.GetFaultBuffer();
                buffer_infos.emplace_back(fault_buffer->Handle(), 0, fault_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                auto& lds_buffer = buffer_cache.GetStreamBuffer();
                const auto& cs_program = liverpool->DrawCsRegs();
                const auto lds_size = cs_program.SharedMemSize() * cs_program.NumWorkgroups();
                const auto [data, offset] = lds_buffer.Map(lds_size, alignment);
                std::memset(data, 0, lds_size);
                lds_buffer.Commit();
                buffer_infos.emplace_back(lds_buffer.Handle(), offset, lds_size);
            } else {
                UNREACHABLE_MSG("Unexpected buffer type {}", u32(desc.buffer_type));
            }
        } else {
            const auto vsharp = desc.GetSharp(stage);
            // Unmapped addresses show up in descriptors of bindings the shader does not use
            // (garbage left in the guest's descriptor memory); bind nothing instead of aborting.
            // Large ranges get clamped to the mapped memory at their start (0 if unmapped);
            // written bindings are checked as well, so garbage cannot become GPU-modified memory.
            // sotc: the clamp alone is 0 exactly when the base is unmapped or reserved (what the
            // two is-mapped calls checked), so one runtime call per binding instead of three.
            // SOTC_BIND_CLAMP1=0 restores the three calls.
            static const bool clamp1 = [] {
                const char* v = std::getenv("SOTC_BIND_CLAMP1");
                return !v || v[0] != '0';
            }();
            u64 size = 0;
            if (clamp1) {
                if (vsharp.base_address != 0 && vsharp.GetSize() != 0) {
                    size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
                }
            } else if (vsharp.base_address != 0 && vsharp.GetSize() != 0 &&
                       memory->IsValidMapping(vsharp.base_address) &&
                       (!desc.is_written || memory->IsMappedAddress(vsharp.base_address))) {
                size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
            }
            if (size == 0) {
                buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
            } else {
                // Guest code commonly marks a buffer as unbounded with num_records near 4GB and
                // only touches a small part of it, and descriptors of unused bindings can hold
                // garbage sizes of several GB. Backing such ranges makes the buffer cache keep
                // them resident until video memory runs out, so bindings above a sane size are
                // limited to a window from their base address.
                static const u64 unbounded_window = [] {
                    const char* v = std::getenv("SHADPS4_UNBOUNDED_BUFFER_MB");
                    return (v ? std::strtoull(v, nullptr, 10) : 64ULL) << 20;
                }();
                constexpr u64 MaxSaneBindingSize = 256ULL << 20;
                // Written bindings keep their full range: a shader writing past a shortened
                // binding would hit sparse pages that were never made resident.
                if (!desc.is_written &&
                    (vsharp.GetSize() >= 0xFFFF0000ULL || size > MaxSaneBindingSize) &&
                    unbounded_window != 0 && size > unbounded_window) {
                    static std::atomic<u32> reported{0};
                    if (vsharp.GetSize() < 0xFFFF0000ULL && reported++ < 16) {
                        LOG_WARNING(Render,
                                    "Limiting {} MB binding at {:#x} of shader {:#x} to {} MB",
                                    size >> 20, vsharp.base_address, stage.pgm_hash,
                                    unbounded_window >> 20);
                    }
                    size = unbounded_window;
                }
                if (size != vsharp.GetSize()) {
                    LOG_DEBUG(Render, "Clamped size from {} to {} for stage {:#x}",
                              vsharp.GetSize(), size, stage.pgm_hash);
                }
                const auto [buffer, offset] = buffer_cache.ObtainBuffer(
                    vsharp.base_address, size, desc.is_written, desc.is_formatted);
                const u64 offset_aligned = Common::AlignDown(offset, alignment);
                const u64 adjust = offset - offset_aligned;
                if (adjust % 4 != 0) {
                    LOG_WARNING(Render_Vulkan, "Buffer binding in shader {:#x} isn't dword aligned",
                                stage.pgm_hash);
                }
                push_data.AddOffset(binding.buffer, adjust);
                buffer_infos.emplace_back(buffer->Handle(), offset_aligned, size + adjust);
                bound_buffers.emplace_back(buffer, offset, size, desc.is_written);
                if (desc.is_written) {
                    // Raw storage-buffer writes can also make an aliased cached image stale.
                    texture_cache.InvalidateMemoryFromGPU(vsharp.base_address, size);
                    Core::MemoryManager::NoteHostWrite(vsharp.base_address, size, 7,
                                                       stage.pgm_hash);
                }
                needs_barrier |= runtime.IsBufferAccessed(buffer, offset, size, desc.is_written);
            }
        }

        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        set_write.pBufferInfo = &buffer_infos.back();
        ++binding.buffer;
    }
}

void Rasterizer::BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding) {
    const auto texture_lock = texture_cache.BatchLock(); // sotc: one lock for the pass
    image_bindings.clear();
    const u32 first_image_idx = image_infos.size();
    // To emulate storing to explicit mip levels, build a descriptor array with each mip level.
    boost::container::small_vector<u32, 8> image_descriptor_array_sizes;

    for (const auto& image_desc : stage.images) {
        const auto tsharp = image_desc.GetSharp(stage);
        if (image_desc.array_size != 0) {
            // dynamic_tsharp_array_size (from Pink-shadPS4): one binding per table element, then a
            // null one for out-of-range indices. The shader was typed after `tsharp`, the first
            // valid element, so elements of another view type or number class stay null.
            const u32 num_bindings = image_desc.NumBindings(stage);
            const auto view_type = tsharp.GetViewType(image_desc.is_array);
            const bool is_integer = AmdGpu::IsInteger(tsharp.GetNumberFmt());
            for (u32 i = 0; i < num_bindings; ++i) {
                const auto element = i < image_desc.array_size
                                         ? image_desc.GetElementSharp(stage, i)
                                         : AmdGpu::Image::Null(image_desc.is_depth);
                const auto element_dfmt = element.GetDataFmt();
                const auto element_nfmt = element.GetNumberFmt();
                const bool bindable =
                    element && element_dfmt != AmdGpu::DataFormat::FormatInvalid &&
                    magic_enum::enum_contains(element_dfmt) &&
                    magic_enum::enum_contains(element_nfmt) &&
                    memory->IsValidGpuMapping(element.Address(), 0) &&
                    element.GetViewType(image_desc.is_array) == view_type &&
                    AmdGpu::IsInteger(element_nfmt) == is_integer &&
                    element.GetNumberConversion() == tsharp.GetNumberConversion();
                if (!bindable) {
                    image_bindings.emplace_back(std::piecewise_construct, std::tuple{},
                                                std::tuple{});
                    continue;
                }
                if (element.lod_hw_cnt_en) {
                    SotcLod::Mark(element.counter_bank_id);
                }
                auto& [image_id, desc] = image_bindings.emplace_back(
                    std::piecewise_construct, std::tuple{}, std::tuple{element, image_desc});
                image_id = texture_cache.FindImage(desc);
                auto* image = &texture_cache.GetImage(image_id);
                if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                    image_id = depth_image_id;
                    image = &texture_cache.GetImage(image_id);
                }
                image->binding.is_bound = 1u;
            }
            image_descriptor_array_sizes.push_back(num_bindings);
            continue;
        }
        if (tsharp.lod_hw_cnt_en) {
            SotcLod::Mark(tsharp.counter_bank_id); // GET_LOD_STATS (sotc_lod_stats.h)
        }
        // sotc: SOTC_TSHARP_TRACE=1: mip fields of the large streamed textures (first 120
        // distinct address/field combinations): which mips the game says are resident.
        static const bool tsharp_trace = std::getenv("SOTC_TSHARP_TRACE") != nullptr;
        if (tsharp_trace && tsharp.width + 1 >= 1024 && tsharp.last_level > 0) {
            static std::mutex m;
            static std::set<std::tuple<u64, u32, u32, u32, u32>> seen;
            std::scoped_lock lk{m};
            const auto key = std::make_tuple(u64{tsharp.Address()}, u32(tsharp.base_level),
                                             u32(tsharp.last_level), u32(tsharp.min_lod),
                                             u32(tsharp.min_lod_warn));
            if (seen.size() < 120 && seen.insert(key).second) {
                std::fprintf(stderr,
                             "SOTC tsharp %#llx %ux%u fmt %u base %u last %u min_lod %u (%.2f) "
                             "warn %u hwcnt %u bank %u\n",
                             static_cast<unsigned long long>(tsharp.Address()), tsharp.width + 1,
                             tsharp.height + 1, u32(tsharp.GetDataFmt()), u32(tsharp.base_level),
                             u32(tsharp.last_level), u32(tsharp.min_lod), tsharp.min_lod / 256.f,
                             u32(tsharp.min_lod_warn), u32(tsharp.lod_hw_cnt_en),
                             u32(tsharp.counter_bank_id));
            }
        }
        // sotc: first few only (per-draw warning; see BufferCache::SynchronizeMemoryFromImage).
        if (static u32 shown = 0; shown < 4 && texture_cache.IsMeta(tsharp.Address())) {
            ++shown;
            LOG_WARNING(Render_Vulkan, "Unexpected metadata read by a shader (texture)");
        }

        const auto data_fmt = tsharp.GetDataFmt();
        const auto num_fmt = tsharp.GetNumberFmt();
        if (tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid) {
            image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        if (!memory->IsValidGpuMapping(tsharp.Address(), 0) ||
            !magic_enum::enum_contains(data_fmt) || !magic_enum::enum_contains(num_fmt)) {
            LOG_WARNING(Render_Vulkan,
                        "Rejecting invalid T# address={:#x}, pitch={}, width={}, "
                        "data_format={}, num_format={}",
                        tsharp.Address(), tsharp.pitch, tsharp.width, static_cast<u32>(data_fmt),
                        static_cast<u32>(num_fmt));
            image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        const Shader::MipStorageFallbackMode mip_fallback_mode = image_desc.mip_fallback_mode;
        const u32 num_bindings = image_desc.NumBindings(stage);

        for (auto i = 0; i < num_bindings; i++) {
            auto& [image_id, desc] = image_bindings.emplace_back(
                std::piecewise_construct, std::tuple{}, std::tuple{tsharp, image_desc});

            if (mip_fallback_mode == Shader::MipStorageFallbackMode::ConstantIndex) {
                ASSERT(num_bindings == 1);
                desc.view_info.range.base.level += image_desc.constant_mip_index;
                desc.view_info.range.extent.levels = 1;
            } else if (mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                desc.view_info.range.base.level += i;
                desc.view_info.range.extent.levels = 1;
            }

            image_id = texture_cache.FindImage(desc);
            auto* image = &texture_cache.GetImage(image_id);
            if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                // If this image has an associated depth image, it's a stencil attachment.
                // Redirect the access to the actual depth-stencil buffer.
                image_id = depth_image_id;
                image = &texture_cache.GetImage(image_id);
            }
            if (image->binding.is_bound) {
                // The image is already bound. In case if it is about to be used as storage we
                // need to force general layout on it.
                image->binding.force_general |= image_desc.is_written;
            }
            image->binding.is_bound = 1u;
        }

        image_descriptor_array_sizes.push_back(num_bindings);
    }

    // Second pass to re-bind images that were updated after binding
    for (auto& [image_id, desc] : image_bindings) {
        bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        if (!image_id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
            image_info_images[image_infos.size() - 1] = {};
        } else {
            if (auto& old_image = texture_cache.GetImage(image_id);
                old_image.binding.needs_rebind) {
                old_image.binding = {};
                image_id = texture_cache.FindImage(desc);
            }

            bound_images.emplace_back(image_id);

            auto& image = texture_cache.GetImage(image_id);
            auto& image_view = texture_cache.FindTexture(image_id, desc);
            const auto binding = image.binding;

            // The image is either bound as storage in a separate descriptor or bound as render
            // target in feedback loop. Depth images are excluded because they can't be bound as
            // storage and feedback loop doesn't make sense for them
            if ((binding.force_general || binding.is_target) && !image.info.props.is_depth) {
                if (instance.IsAttachmentFeedbackLoopLayoutSupported() && image.binding.is_target) {
                    needs_barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT,
                        vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead);
                } else {
                    needs_barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        desc.view_info.range);
                }
            } else {
                if (is_storage) {
                    needs_barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        desc.view_info.range);
                } else {
                    const auto new_layout = image.info.props.is_depth
                                                ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                                : vk::ImageLayout::eShaderReadOnlyOptimal;
                    needs_barrier |= runtime.Transit(
                        &image, new_layout, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead, desc.view_info.range);
                }
            }
            image.usage.storage |= is_storage;
            image.usage.texture |= !is_storage;

            image_infos.emplace_back(VK_NULL_HANDLE, *image_view.image_view,
                                     image.backing->state.layout);
            image_info_images[image_infos.size() - 1] = image_id;
        }
    }

    // sotc: TakeHudless: the final composite samples the UI layer (an sRGB render target) and the
    // fp16 scene, nothing else. Off unless SOTC_FG_HUDLESS=1: with FSR 3 the generated frames of
    // the title screen (faded logo) and of loading screens (the scene left behind the opaque UI
    // layer shows through) came out wrong, the real frame's UI not put back over them.
    static const bool hudless_on = [] {
        const char* v = std::getenv("SOTC_FG_HUDLESS");
        return v && v[0] == '1';
    }();
    if (hudless_on && stage.sw_stage == Shader::SwStage::Fragment && image_bindings.size() == 2 &&
        image_bindings[0].first && image_bindings[1].first &&
        FrameGen::Current() != FrameGen::Mode::Off) {
        const auto& ui = texture_cache.GetImage(image_bindings[0].first);
        const auto& scene = texture_cache.GetImage(image_bindings[1].first);
        if (ui.usage.render_target && ui.info.pixel_format == vk::Format::eR8G8B8A8Srgb &&
            scene.info.pixel_format == vk::Format::eR16G16B16A16Sfloat) {
            hudless_ui_info = static_cast<s32>(first_image_idx);
        }
    }

    // sotc: DLSS super resolution: the game's final composite samples the UI layer (an sRGB render
    // target) and the tonemapped fp16 scene, nothing else, into a video out surface.
    if (Dlssg::SuperResolution() && stage.sw_stage == Shader::SwStage::Fragment &&
        image_bindings.size() == 2 && image_bindings[0].first && image_bindings[1].first &&
        cb_descs[0].first) {
        const auto& ui = texture_cache.GetImage(image_bindings[0].first);
        const auto& scene = texture_cache.GetImage(image_bindings[1].first);
        if (texture_cache.GetImage(cb_descs[0].first).usage.vo_surface &&
            ui.usage.render_target && ui.info.pixel_format == vk::Format::eR8G8B8A8Srgb &&
            scene.info.pixel_format == vk::Format::eR16G16B16A16Sfloat) {
            Dlssg::NoteComposite(image_bindings[1].first, image_bindings[0].first, texture_cache);
            NoteUiShadow();
            static bool logged = false;
            if (!std::exchange(logged, true)) {
                std::fprintf(stderr, "SOTC DLSS: composite shader %#llx\n",
                             static_cast<unsigned long long>(stage.pgm_hash));
            }
        }
    }

    u32 image_info_idx = first_image_idx;
    u32 image_binding_idx = 0;
    for (u32 array_size : image_descriptor_array_sizes) {
        const auto& [_, desc] = image_bindings[image_binding_idx];
        const bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = array_size;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[image_info_idx];

        image_info_idx += array_size;
        image_binding_idx += array_size;
        binding.unified += array_size;
    }

    // sotc: at a render size above 1080 lines the game stretches its 1080p 2D art (splash logos,
    // UI) itself, with the point sampler meant for 1:1 drawing: hard steps. A draw that samples
    // one asset texture (never a render target, storage or depth image) magnifies bilinearly
    // there. SOTC_UI_LINEAR=0/1 overrides, SOTC_UI_LINEAR_TRACE=1 lists the draws it changes.
    static const bool ui_linear = [] {
        if (const char* v = std::getenv("SOTC_UI_LINEAR"); v && *v) {
            return *v != '0';
        }
        const char* r = std::getenv("SOTC_RENDER_RES");
        return r && *r && std::strcmp(r, "1920x1080") != 0;
    }();
    const VideoCore::Image* ui_asset = nullptr;
    if (ui_linear && image_bindings.size() == 1 && image_bindings[0].first &&
        stage.images.size() == 1 && stage.images[0].array_size == 0) {
        const auto& image = texture_cache.GetImage(image_bindings[0].first);
        const auto& u = image.usage;
        if (!u.render_target && !u.depth_target && !u.storage && !u.vo_surface &&
            !image.binding.is_target && !image.info.props.is_depth &&
            !AmdGpu::IsInteger(stage.images[0].GetSharp(stage).GetNumberFmt())) {
            ui_asset = &image;
        }
    }
    for (const auto& sampler : stage.samplers) {
        auto ssharp = sampler.GetSharp(stage);
        if (ui_asset && ssharp.xy_mag_filter.Value() == AmdGpu::Filter::Point &&
            !sampler.is_depth) {
            ssharp.xy_mag_filter.Assign(AmdGpu::Filter::Bilinear);
            static const bool trace = std::getenv("SOTC_UI_LINEAR_TRACE") != nullptr;
            if (trace) {
                static std::mutex m;
                static std::set<u64> seen;
                std::scoped_lock lk{m};
                if (seen.size() < 200 && seen.insert(stage.pgm_hash ^ ui_asset->info.guest_address).second) {
                    std::fprintf(stderr, "SOTC ui-linear pgm %#llx tex %#llx %ux%u\n",
                                 static_cast<unsigned long long>(stage.pgm_hash),
                                 static_cast<unsigned long long>(ui_asset->info.guest_address),
                                 ui_asset->info.size.width, ui_asset->info.size.height);
                }
            }
        }
        if (!ssharp.Valid() || (ssharp.border_color_type.Value() == AmdGpu::BorderColor::Custom &&
                                liverpool->DrawRegs().ta_bc_base.Address() == 0)) {
            LOG_WARNING(Render_Vulkan,
                        "Rejecting invalid S# max_aniso={}, filter_mode={}, mip_filter={}, "
                        "border_color_type={}, border_color_base={:#x}",
                        static_cast<u32>(ssharp.max_aniso.Value()),
                        static_cast<u32>(ssharp.filter_mode.Value()),
                        static_cast<u32>(ssharp.mip_filter.Value()),
                        static_cast<u32>(ssharp.border_color_type.Value()),
                        liverpool->DrawRegs().ta_bc_base.Address());
            ssharp = AmdGpu::Sampler{};
        }
        const auto vk_sampler =
            texture_cache.GetSampler(ssharp, liverpool->DrawRegs().ta_bc_base, sampler.is_depth);
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        image_info_images[image_infos.size() - 1] = {};
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eSampler;
        set_write.pImageInfo = &image_infos.back();
    }
}

RenderState Rasterizer::BeginRendering(const GraphicsPipeline* pipeline) {
    const auto texture_lock = texture_cache.BatchLock(); // sotc: one lock for the attachments
    attachment_feedback_loop = false;
    // sotc: SOTC_RT_FAST (default on) drops redundant attachment work: FindRenderTarget repeats
    // UpdateImage only when the image still needs it, and the CMASK/HTILE clear bit is read and
    // cleared with one lookup. SOTC_RT_FAST=0 restores the original calls and evaluation order.
    static const bool rt_fast = [] {
        const char* v = std::getenv("SOTC_RT_FAST");
        return !v || v[0] != '0';
    }();
    const auto& regs = liverpool->DrawRegs();
    const auto& key = pipeline->GetGraphicsKey();
    RenderState state;
    ui_shadow_hit = false; // sotc: BeginUiShadow
    state.width = instance.GetMaxFramebufferWidth();
    state.height = instance.GetMaxFramebufferHeight();
    state.num_layers = std::numeric_limits<u16>::max();
    state.num_color_attachments = std::bit_width(key.mrt_mask);
    for (auto cb = 0u; cb < state.num_color_attachments; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            state.color_attachments[cb] = {};
            continue;
        }
        auto* image = &texture_cache.GetImage(image_id);
        if (image->binding.needs_rebind) {
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
            image = &texture_cache.GetImage(image_id);
        }
        texture_cache.UpdateImage(image_id);
        runtime.SetBackingSamples(image, key.color_samples[cb]);
        const auto& image_view = texture_cache.FindRenderTarget(image_id, desc, !rt_fast);
        const auto slice = image_view.info.range.base.layer;
        const auto mip = image_view.info.range.base.level;

        const auto& col_buf = regs.color_buffers[cb];
        // sotc: SOTC_RT_FAST: test-and-clear the CMASK bit with one lookup.
        bool is_clear;
        if (rt_fast) {
            is_clear = texture_cache.TestAndTouchMeta(col_buf.CmaskAddress(), slice);
        } else {
            is_clear = texture_cache.IsMetaCleared(col_buf.CmaskAddress(), slice);
            texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
        }

        if (image->binding.is_bound) {
            ASSERT_MSG(!image->binding.force_general,
                       "Having image both as storage and render target is unsupported");
            runtime.FlushBarriers();
            needs_barrier |=
                runtime.Transit(image,
                                instance.IsAttachmentFeedbackLoopLayoutSupported()
                                    ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                                    : vk::ImageLayout::eGeneral,
                                vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                vk::AccessFlagBits2::eColorAttachmentWrite |
                                    vk::AccessFlagBits2::eColorAttachmentRead);
            attachment_feedback_loop = true;
        } else {
            needs_barrier |= runtime.Transit(image, vk::ImageLayout::eColorAttachmentOptimal,
                                             vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                             vk::AccessFlagBits2::eColorAttachmentWrite |
                                                 vk::AccessFlagBits2::eColorAttachmentRead,
                                             desc.view_info.range);
        }

        state.width = std::min<u32>(state.width, std::max(image->info.size.width >> mip, 1u));
        state.height = std::min<u32>(state.height, std::max(image->info.size.height >> mip, 1u));
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        const auto clear_value =
            is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf) : vk::ClearValue{};
        auto& attachment = state.color_attachments[cb];
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image->backing->state.layout;
        attachment.clear_value = clear_value.color.uint32;
        attachment.is_clear = is_clear;

        image->usage.render_target = 1u;
        if (cb == 0 && image->usage.vo_surface) { // sotc: TakeHudless
            hudless_vo = image->info.guest_address;
            hudless_vo_format = image_view.info.format;
            hudless_vo_width = image->info.size.width;
            hudless_vo_height = image->info.size.height;
        }
        if (cb == 0 && mip == 0 && slice == 0 && // sotc: BeginUiShadow
            Dlssg::UiTarget(image->info.guest_address, image->info.size.width,
                            image->info.size.height, ui_shadow_width, ui_shadow_height)) {
            ui_shadow_hit = true;
            ui_shadow_format = image_view.info.format;
            ui_shadow_src_width = image->info.size.width;
            ui_shadow_src_height = image->info.size.height;
        }
    }
    for (u32 cb = state.num_color_attachments; cb < state.color_attachments.size(); ++cb) {
        state.color_attachments[cb] = {};
    }

    if (auto image_id = db_desc.first; image_id) {
        auto& desc = db_desc.second;
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& image_view = texture_cache.FindDepthTarget(image_id, desc);
        auto& image = texture_cache.GetImage(image_id);

        const auto slice = image_view.info.range.base.layer;
        const bool depth_reg_clear = regs.depth_render_control.depth_clear_enable &&
                                     regs.depth_control.depth_enable &&
                                     regs.depth_control.depth_write_enable;
        // sotc: SOTC_RT_FAST: test-and-clear the HTILE bit with one lookup. It is evaluated
        // unconditionally (no short-circuit) because the original TouchMeta always cleared it.
        bool is_depth_clear;
        if (rt_fast) {
            const bool meta_clear = texture_cache.TestAndTouchMeta(htile_address, slice);
            is_depth_clear = depth_reg_clear || meta_clear;
        } else {
            is_depth_clear = depth_reg_clear || texture_cache.IsMetaCleared(htile_address, slice);
        }
        const bool is_stencil_clear = regs.depth_render_control.stencil_clear_enable;
        if (!rt_fast) {
            texture_cache.TouchMeta(htile_address, slice, false);
        }
        ASSERT(desc.view_info.range.extent.levels == 1 && !image.binding.needs_rebind);

        const bool has_stencil = image.info.props.has_stencil;
        // Stencil writes can be enabled while depth writes are off.
        const bool stencil_write =
            has_stencil && regs.depth_control.stencil_enable && !desc.view_info.is_storage;
        const auto new_layout = desc.view_info.is_storage
                                    ? has_stencil ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                                  : vk::ImageLayout::eDepthAttachmentOptimal
                                : stencil_write
                                    ? vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal
                                : has_stencil ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                              : vk::ImageLayout::eDepthReadOnlyOptimal;
        needs_barrier |= runtime.Transit(&image, new_layout,
                                         vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                             vk::PipelineStageFlagBits2::eLateFragmentTests,
                                         vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                                             vk::AccessFlagBits2::eDepthStencilAttachmentRead,
                                         desc.view_info.range);

        state.width = std::min<u32>(state.width, image.info.size.width);
        state.height = std::min<u32>(state.height, image.info.size.height);
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        auto& attachment = state.depth_stencil_attachment;
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image.backing->state.layout;
        attachment.clear_value = {};
        attachment.is_clear = 0;

        if (regs.depth_buffer.DepthValid()) {
            attachment.clear_value[0] = is_depth_clear ? std::bit_cast<u32>(regs.depth_clear) : 0u;
            attachment.has_depth = true;
            attachment.depth_clear = is_depth_clear;
        }
        if (regs.depth_buffer.StencilValid()) {
            attachment.clear_value[1] = is_stencil_clear ? regs.stencil_clear : 0u;
            attachment.has_stencil = true;
            attachment.stencil_clear = is_stencil_clear;
        }

        image.usage.depth_target = true;
    } else {
        state.depth_stencil_attachment = {};
    }

    if (state.num_layers == std::numeric_limits<u16>::max()) {
        state.num_layers = 1;
    }

    return state;
}

void Rasterizer::Resolve() {
    const auto& mrt0_hint = liverpool->DrawCbExtent()[0];
    const auto& mrt1_hint = liverpool->DrawCbExtent()[1];
    VideoCore::TextureCache::ImageDesc mrt0_desc{liverpool->DrawRegs().color_buffers[0], mrt0_hint};
    VideoCore::TextureCache::ImageDesc mrt1_desc{liverpool->DrawRegs().color_buffers[1], mrt1_hint};
    auto& mrt0_image = texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, true));
    auto& mrt1_image = texture_cache.GetImage(texture_cache.FindImage(mrt1_desc, true));

    ScopeMarkerBegin(fmt::format("Resolve:MRT0={:#x}:MRT1={:#x}",
                                 liverpool->DrawRegs().color_buffers[0].Address(),
                                 liverpool->DrawRegs().color_buffers[1].Address()));
    runtime.ResolveImage(&mrt0_image, &mrt1_image, mrt0_desc.view_info.range,
                         mrt1_desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::DepthStencilCopy(bool is_depth, bool is_stencil) {
    auto& regs = liverpool->DrawRegs();

    auto read_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->DrawDbExtent(), false);
    auto write_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->DrawDbExtent(), true);

    auto& read_image = texture_cache.GetImage(texture_cache.FindImage(read_desc));
    auto& write_image = texture_cache.GetImage(texture_cache.FindImage(write_desc));

    VideoCore::SubresourceRange sub_range;
    sub_range.base.layer = liverpool->DrawRegs().depth_view.slice_start;
    sub_range.extent.layers = liverpool->DrawRegs().depth_view.NumSlices() - sub_range.base.layer;

    ScopeMarkerBegin(fmt::format(
        "DepthStencilCopy:DR={:#x}:SR={:#x}:DW={:#x}:SW={:#x}", regs.depth_buffer.DepthAddress(),
        regs.depth_buffer.StencilAddress(), regs.depth_buffer.DepthWriteAddress(),
        regs.depth_buffer.StencilWriteAddress()));

    runtime.CopyDepthStencil(&read_image, &write_image, sub_range);

    ScopeMarkerEnd();
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    ASSERT_MSG(address % 4 == 0 && num_bytes % 4 == 0,
               "FillBuffer address and size must be a multiple of 4 bytes");
    if (!is_gds) {
        texture_cache.ClearMeta(address);
        if (!buffer_cache.IsRegionGpuModified(address, num_bytes)) {
            u32* buffer = std::bit_cast<u32*>(address);
            std::fill(buffer, buffer + (num_bytes / sizeof(u32)), value);
            return;
        }
    }
    const auto [buffer, offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (is_gds) {
            return {buffer_cache.GetGdsBuffer(), address};
        }
        return buffer_cache.ObtainBuffer(address, num_bytes, true);
    }();
    runtime.FillBuffer(buffer, offset, num_bytes, value);
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    if (!dst_gds && !buffer_cache.IsRegionGpuModified(dst, num_bytes)) {
        if (!src_gds && !buffer_cache.IsRegionGpuModified(src, num_bytes) &&
            !texture_cache.FindImageFromRange(src, num_bytes)) {
            // Both buffers were not transferred to GPU yet. Can safely copy in host memory.
            std::memcpy(std::bit_cast<void*>(dst), std::bit_cast<void*>(src), num_bytes);
            return;
        }
    }
    texture_cache.InvalidateMemoryFromGPU(dst, num_bytes);
    const auto* gds_buffer = buffer_cache.GetGdsBuffer();
    const auto [src_buffer, src_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (src_gds) {
            return {gds_buffer, src};
        }
        return buffer_cache.ObtainBuffer(src, num_bytes, false, true);
    }();
    const auto [dst_buffer, dst_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (dst_gds) {
            return {gds_buffer, dst};
        }
        return buffer_cache.ObtainBuffer(dst, num_bytes, true, true);
    }();
    const vk::BufferCopy copy = {
        .srcOffset = src_offset,
        .dstOffset = dst_offset,
        .size = num_bytes,
    };
    runtime.CopyBuffer(src_buffer, dst_buffer, std::span{&copy, 1});
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    auto* gds_buf = buffer_cache.GetGdsBuffer();
    u32 value;
    std::memcpy(&value, gds_buf->mapped_data.data() + gds_offset, sizeof(u32));
    return value;
}

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size, bool assume_locks) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.InvalidateMemory(addr, size, assume_locks);
    texture_cache.InvalidateMemory(addr, size);
    return true;
}

// sotc: a guest (not command processor) write fault. With the repeat-fault grace on, the texture
// cache goes first so the page becomes writable last, after any wait on the texture mutex (the GPU
// thread then has less time to protect it again before the guest retries its write).
bool Rasterizer::InvalidateMemoryOnGuestFault(VAddr addr, u64 size) {
    if (!IsMapped(addr, size)) {
        return false;
    }
    if (VideoCore::FaultGrace::Pages() != 0) {
        texture_cache.InvalidateMemory(addr, size);
        buffer_cache.InvalidateMemory(addr, size, false, true);
    } else {
        buffer_cache.InvalidateMemory(addr, size, false);
        texture_cache.InvalidateMemory(addr, size);
    }
    return true;
}

bool Rasterizer::ReadMemory(VAddr addr, u64 size, bool assume_locks) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.ReadMemory(addr, size, false, assume_locks);
    return true;
}

bool Rasterizer::IsMapped(VAddr addr, u64 size) {
    if (size == 0) {
        // There is no memory, so not mapped.
        return false;
    }
    if (static_cast<u64>(addr) > std::numeric_limits<u64>::max() - size) {
        // Memory range wrapped the address space, cannot be mapped.
        return false;
    }
    const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);

    Common::RecursiveSharedLock lock{mapped_ranges_mutex};
    return boost::icl::contains(mapped_ranges, range);
}

void Rasterizer::MapMemory(VAddr addr, u64 size) {
    buffer_cache.OnMappingChanged();
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges += decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::RegisterMemory(VAddr addr, u64 size) {
    page_manager.OnGpuMap(addr, size);
}

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
    buffer_cache.OnMappingChanged();
    // sotc: an unmapped range forgets its GPU-written state instead of downloading it (a 512 KB
    // window around it) into memory that is being given back: with texture streaming that
    // readback broke the game's heap within a minute of loading a save (allocator fault at guest
    // 0x1a371, garbage textures). SOTC_UNMAP_READBACK=1 restores the readback.
    static const bool unmap_readback = [] {
        const char* v = std::getenv("SOTC_UNMAP_READBACK");
        return v && v[0] == '1';
    }();
    if (unmap_readback) {
        buffer_cache.InvalidateMemory(addr, size);
    } else {
        buffer_cache.DropGpuModified(addr, size);
    }
    // Give the arena memory back; it runs on the GPU thread, ordered before later draws.
    // sotc: on by default since 2026-10-16 (SOTC_ARENA_RELEASE=0: never released). It was
    // opt-in because it raced with in-flight GPU work during loads (device lost loading a save,
    // seen before ReleaseMemory finished the GPU work first). Never releasing grew video memory
    // ~90 MB a minute in the attract demo (1.6 -> 6.0 GB in 48 minutes on a 6 GB card) until an
    // arena allocation failed (ASSERT). Released, the 25-minute curve stays ~0.7-0.9 GB lower;
    // no device loss in that run nor in three shrine runs that each load a save.
    static const bool release_enabled = [] {
        const char* v = std::getenv("SOTC_ARENA_RELEASE");
        return !v || v[0] != '0';
    }();
    if (release_enabled) {
        // sotc: ordered at the command processor's point of the stream: with the draw thread a
        // guest-thread command would run ahead of the draws already queued, which may still use
        // the range (review of #99).
        liverpool->SendOrderedCommand(
            [this, addr, size] { buffer_cache.ReleaseMemory(addr, size); });
    }
    texture_cache.UnmapMemory(addr, size);
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges -= decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed) const {
    UpdateViewportScissorState();
    UpdateDepthStencilState();
    UpdatePrimitiveState(is_indexed);
    UpdateRasterizationState();
    UpdateColorBlendingState(pipeline);

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.Commit(instance, scheduler.CommandBuffer());
}

void Rasterizer::UpdateViewportScissorState() const {
    const auto& regs = liverpool->DrawRegs();

    const auto combined_scissor_value_tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const auto combined_scissor_value_br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable;

    AmdGpu::Scissor scsr{};
    scsr.top_left_x = combined_scissor_value_tl(
        regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x),
        s16(regs.generic_scissor.top_left_x),
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = combined_scissor_value_tl(
        regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y),
        s16(regs.generic_scissor.top_left_y),
        enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_x,
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
        regs.generic_scissor.bottom_right_y,
        enable_offset ? regs.window_offset.window_y_offset : 0);

    boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS> viewports;
    boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS> scissors;

    if (regs.polygon_control.enable_window_offset &&
        (regs.window_offset.window_x_offset != 0 || regs.window_offset.window_y_offset != 0)) {
        LOG_ERROR(Render_Vulkan,
                  "PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE support is not yet implemented.");
    }

    const auto& vp_ctl = regs.viewport_control;
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; i++) {
        const auto& vp = regs.viewports[i];
        const auto& vp_d = regs.viewport_depths[i];
        if (vp.xscale == 0) {
            continue;
        }

        const auto zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
        const auto zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;

        vk::Viewport viewport{};

        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_pipeline_graphics.c#L688-689
        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_cmd_buffer.c#L3103-3109
        // When the clip space is ranged [-1...1], the zoffset is centered.
        // By reversing the above viewport calculations, we get the following:
        if (regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
            viewport.minDepth = zoffset - zscale;
            viewport.maxDepth = zoffset + zscale;
        } else {
            viewport.minDepth = zoffset;
            viewport.maxDepth = zoffset + zscale;
        }

        if (!instance.IsDepthRangeUnrestrictedSupported()) {
            // Unrestricted depth range not supported by device. Restrict to valid range.
            viewport.minDepth = std::max(viewport.minDepth, 0.f);
            viewport.maxDepth = std::min(viewport.maxDepth, 1.f);
        }

        if (regs.IsClipDisabled()) {
            // In case if clipping is disabled we patch the shader to convert vertex position
            // from screen space coordinates to NDC by defining a render space as full hardware
            // window range [0..16383, 0..16383] and setting the viewport to its size.
            viewport.x = 0.f;
            viewport.y = 0.f;
            viewport.width = float(std::min<u32>(instance.GetMaxViewportWidth(), 16_KB));
            viewport.height = float(std::min<u32>(instance.GetMaxViewportHeight(), 16_KB));
        } else {
            const auto xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
            const auto xscale = vp_ctl.xscale_enable ? vp.xscale : 1.f;
            const auto yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
            const auto yscale = vp_ctl.yscale_enable ? vp.yscale : 1.f;

            viewport.x = xoffset - xscale;
            viewport.y = yoffset - yscale;
            viewport.width = xscale * 2.0f;
            viewport.height = yscale * 2.0f;
        }

        viewports.push_back(viewport);

        auto vp_scsr = scsr;
        if (regs.mode_control.vport_scissor_enable) {
            vp_scsr.top_left_x =
                std::max(vp_scsr.top_left_x, s16(regs.viewport_scissors[i].top_left_x));
            vp_scsr.top_left_y =
                std::max(vp_scsr.top_left_y, s16(regs.viewport_scissors[i].top_left_y));
            vp_scsr.bottom_right_x = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_x),
                                              regs.viewport_scissors[i].bottom_right_x);
            vp_scsr.bottom_right_y = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_y),
                                              regs.viewport_scissors[i].bottom_right_y);
        }
        scissors.push_back({
            .offset = {vp_scsr.top_left_x, vp_scsr.top_left_y},
            .extent = {vp_scsr.GetWidth(), vp_scsr.GetHeight()},
        });
    }

    if (viewports.empty()) {
        // Vulkan requires providing at least one viewport.
        constexpr vk::Viewport empty_viewport = {
            .x = -1.0f,
            .y = -1.0f,
            .width = 1.0f,
            .height = 1.0f,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        constexpr vk::Rect2D empty_scissor = {
            .offset = {0, 0},
            .extent = {1, 1},
        };
        viewports.push_back(empty_viewport);
        scissors.push_back(empty_scissor);
    }

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetViewports(viewports);
    dynamic_state.SetScissors(scissors);
}

void Rasterizer::UpdateDepthStencilState() const {
    const auto& regs = liverpool->DrawRegs();
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto depth_test_enabled =
        regs.depth_control.depth_enable && regs.depth_buffer.DepthValid();
    dynamic_state.SetDepthTestEnabled(depth_test_enabled);
    if (depth_test_enabled) {
        dynamic_state.SetDepthWriteEnabled(regs.depth_control.depth_write_enable &&
                                           !regs.depth_render_control.depth_clear_enable);
        dynamic_state.SetDepthCompareOp(LiverpoolToVK::CompareOp(regs.depth_control.depth_func));
    }

    const auto depth_bounds_test_enabled = regs.depth_control.depth_bounds_enable;
    dynamic_state.SetDepthBoundsTestEnabled(depth_bounds_test_enabled);
    if (depth_bounds_test_enabled) {
        dynamic_state.SetDepthBounds(regs.depth_bounds_min, regs.depth_bounds_max);
    }

    const auto depth_bias_enabled = regs.polygon_control.NeedsBias();
    dynamic_state.SetDepthBiasEnabled(depth_bias_enabled);
    if (depth_bias_enabled) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        dynamic_state.SetDepthBias(
            front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset,
            regs.poly_offset.depth_bias,
            (front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f);
    }

    const auto stencil_test_enabled =
        regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid();
    dynamic_state.SetStencilTestEnabled(stencil_test_enabled);
    if (stencil_test_enabled) {
        const StencilOps front_ops{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_front),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_front),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_front),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_ref_func),
        };
        const StencilOps back_ops = regs.depth_control.backface_enable ? StencilOps{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_back),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_back),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_back),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_bf_func),
        } : front_ops;
        dynamic_state.SetStencilOps(front_ops, back_ops);

        const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
        const auto front = regs.stencil_ref_front;
        const auto back =
            regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
        // GCN REPLACE_OP writes DB_STENCILREFMASK.STENCILOPVAL, so a face whose stencil ops
        // include ReplaceOp takes its Vulkan reference from op_val.
        const auto& sc = regs.stencil_control;
        const auto uses_op_val = [](AmdGpu::StencilFunc fail, AmdGpu::StencilFunc zpass,
                                    AmdGpu::StencilFunc zfail) {
            return fail == AmdGpu::StencilFunc::ReplaceOp ||
                   zpass == AmdGpu::StencilFunc::ReplaceOp ||
                   zfail == AmdGpu::StencilFunc::ReplaceOp;
        };
        const bool front_op =
            uses_op_val(sc.stencil_fail_front, sc.stencil_zpass_front, sc.stencil_zfail_front);
        const bool back_op =
            regs.depth_control.backface_enable
                ? uses_op_val(sc.stencil_fail_back, sc.stencil_zpass_back, sc.stencil_zfail_back)
                : front_op;
        const auto ref_conflict = [](AmdGpu::CompareFunc func, const AmdGpu::StencilRefMask& ref) {
            return func != AmdGpu::CompareFunc::Always && func != AmdGpu::CompareFunc::Never &&
                   ref.stencil_test_val != ref.stencil_op_val;
        };
        if ((front_op && ref_conflict(regs.depth_control.stencil_ref_func, front)) ||
            (back_op && regs.depth_control.backface_enable &&
             ref_conflict(regs.depth_control.stencil_bf_func, back))) {
            LOG_WARNING(Render_Vulkan, "Stencil test requires test_val while ReplaceOp requires "
                                       "op_val; the stencil test will use op_val");
        }
        dynamic_state.SetStencilReferences(front_op ? front.stencil_op_val : front.stencil_test_val,
                                           back_op ? back.stencil_op_val : back.stencil_test_val);
        dynamic_state.SetStencilWriteMasks(!stencil_clear ? front.stencil_write_mask : 0U,
                                           !stencil_clear ? back.stencil_write_mask : 0U);
        dynamic_state.SetStencilCompareMasks(front.stencil_mask, back.stencil_mask);
    }
}

void Rasterizer::UpdatePrimitiveState(const bool is_indexed) const {
    const auto& regs = liverpool->DrawRegs();
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto is_list_topology = [](const AmdGpu::PrimitiveType type) {
        const auto topology = LiverpoolToVK::PrimitiveType(type);
        return topology == vk::PrimitiveTopology::ePointList ||
               topology == vk::PrimitiveTopology::eLineList ||
               topology == vk::PrimitiveTopology::eTriangleList ||
               topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
               topology == vk::PrimitiveTopology::eTriangleListWithAdjacency;
    };
    const auto is_patch_list_topology = [](const AmdGpu::PrimitiveType type) {
        // Quad and rect lists are emulated using tessellation.
        return type == AmdGpu::PrimitiveType::PatchPrimitive ||
               type == AmdGpu::PrimitiveType::QuadList || type == AmdGpu::PrimitiveType::RectList;
    };

    const auto prim_restart =
        (regs.enable_primitive_restart & 1) != 0 &&
        (instance.IsListRestartSupported() || !is_list_topology(regs.primitive_type)) &&
        (instance.IsPatchListRestartSupported() || !is_patch_list_topology(regs.primitive_type));
    ASSERT_MSG(!is_indexed || !prim_restart || regs.primitive_restart_index == 0xFFFF ||
                   regs.primitive_restart_index == 0xFFFFFFFF,
               "Primitive restart index other than -1 is not supported yet");

    const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                               ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                               : vk::CullModeFlagBits::eNone;
    const auto front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);

    dynamic_state.SetPrimitiveRestartEnabled(prim_restart);
    dynamic_state.SetRasterizerDiscardEnabled(regs.clipper_control.dx_rasterization_kill);
    dynamic_state.SetCullMode(cull_mode);
    dynamic_state.SetFrontFace(front_face);
}

void Rasterizer::UpdateRasterizationState() const {
    const auto& regs = liverpool->DrawRegs();
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetLineWidth(regs.line_control.Width());
}

void Rasterizer::UpdateColorBlendingState(const GraphicsPipeline* pipeline) const {
    const auto& regs = liverpool->DrawRegs();
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetBlendConstants(regs.blend_constants);
    dynamic_state.SetColorWriteMasks(pipeline->GetGraphicsKey().write_masks);
    dynamic_state.SetAttachmentFeedbackLoopEnabled(attachment_feedback_loop);
}

void Rasterizer::ScopeMarkerBegin(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopeMarkerEnd(bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.endDebugUtilsLabelEXT();
}

void Rasterizer::ScopedMarkerInsert(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                         bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
        .color = std::array<f32, 4>(
            {(f32)((color >> 16) & 0xff) / 255.0f, (f32)((color >> 8) & 0xff) / 255.0f,
             (f32)(color & 0xff) / 255.0f, (f32)((color >> 24) & 0xff) / 255.0f})});
}

const Pipeline* Rasterizer::LookupGraphicsPipeline(u16 vertex_sgpr_offset,
                                                   u16 instance_sgpr_offset) {
    const DrawIndirectParams params = {
        .vertex_sgpr_offset = vertex_sgpr_offset,
        .instance_sgpr_offset = instance_sgpr_offset,
    };
    return pipeline_cache.GetGraphicsPipeline(params);
}

const Pipeline* Rasterizer::LookupComputePipeline() {
    return pipeline_cache.GetComputePipeline();
}

bool Rasterizer::IsGpuSideThread() const {
    return liverpool->IsGpuSideThread();
}

void Rasterizer::PrepareGpuSideFault() {
    liverpool->PrepareGpuSideFault();
}

bool Rasterizer::RunsGpuWorkElsewhere() const {
    return liverpool->RunsGpuWorkElsewhere();
}

void Rasterizer::RunOnDrawThreadImpl(Common::UniqueFunction<void>&& func) {
    liverpool->RunOnBackThread(std::move(func));
}

std::thread::id Rasterizer::GetGpuCommandProcessorThread() {
    return liverpool->GetGpuCommandProcessorThread();
}

#ifdef __linux__
u32 Rasterizer::GetGpuCommandProcessorThreadId() {
    return liverpool->GetGpuCommandProcessorThreadId();
}

bool Rasterizer::IsGpuSideThreadId(u32 tid) const {
    return liverpool->IsGpuSideThreadId(tid);
}
#endif

} // namespace Vulkan
