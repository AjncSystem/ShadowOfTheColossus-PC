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
#include <unordered_set>
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
#include "video_core/renderer_vulkan/sotc_guest_memory.h"
#include "video_core/renderer_vulkan/vk_dlssg.h"
#include "video_core/renderer_vulkan/vk_shadow_scroll.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/buffer_cache/region_manager.h"
#include "video_core/sotc_live.h"
#include "video_core/sotc_adaptive.h"
#include "video_core/sotc_write_extent.h"
#include "video_core/sotc_op_phases.h" // sotc: SOTC_OP_PHASES
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

// sotc: SOTC_PR_RESOLVE. Generation of the runtime memory table (src/runtime_memory.c); odd while
// a change is in progress. Same declaration as sotc_bininfo.cpp.
extern "C" uint64_t runtime_memory_generation(void);

namespace Vulkan {

// sotc: SOTC_DEDUP_STATE=0 sends the vertex inputs on every draw again (vertex_input_cache in
// DynamicState, vk_scheduler.h, is then never read or written).
static const bool kDedupState = [] {
    const char* v = std::getenv("SOTC_DEDUP_STATE");
    return !v || v[0] != '0';
}();

// sotc: SOTC_BINDLESS_SAMP: the samplers of this stage come from the global heap, not from set 0.
// The same rule as Shader::UsesBindlessSamplers, whose profile flag is the instance flag.
static bool IsBindlessStage(const Instance& instance, const Shader::Info& stage) {
    return instance.IsBindlessSamplersEnabled() && !stage.samplers.empty() &&
           stage.samplers.size() <= Shader::GetBindlessSamplerRange(stage.sw_stage).count;
}

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
    SotcGuestMemory::Init(instance); // sotc: SOTC_PC_MEMORY=1 (docs/sotc/MODELO-MEMORIA-PC.md)

    // sotc: SOTC_BINDLESS_SAMP: the pipelines bind the global sampler set of the texture cache.
    const auto& bindless = texture_cache.GetBindlessSamplers();
    if (bindless.Enabled()) {
        pipeline_cache.GetDescriptorHeap().SetBindlessSamplers(bindless.Layout(), bindless.Set());
    }

    scheduler.SetSubmitCallback([this](Vulkan::SubmitInfo& info) {
        runtime.FlushBarriers();
        buffer_cache.SubmitPendingArenaBinds(info);
    });

    if (Shader::LodFeedbackEnabled()) {
        sotc_lod_feedback = std::make_unique<VideoCore::Buffer>(
            instance, 0, SotcLodFeedbackBytes + sizeof(u32), VideoCore::MemoryType::DeviceLocal,
            "SotC LOD Feedback");
        runtime.FillBuffer(sotc_lod_feedback.get(), SotcLodFeedbackBytes, sizeof(u32), 1u);
        sotc_lod_download = std::make_unique<VideoCore::Buffer>(
            instance, 0, SotcLodFeedbackBytes * SotcLodFeedbackAreas,
            VideoCore::MemoryType::HostCached, "SotC LOD Feedback Download");
        runtime.FillBuffer(sotc_lod_feedback.get(), 0u, SotcLodFeedbackBytes, ~0u);
    }
}

void Rasterizer::SotcLodFeedbackCycle() {
    if (!sotc_lod_feedback) {
        return;
    }
    // Every 8 ms is plenty: the game asks for the statistics about once a frame and the banks
    // stay sampled for a while (SotcLod::Collect).
    const auto now = std::chrono::steady_clock::now();
    if (now - sotc_lod_last < std::chrono::milliseconds{8} ||
        sotc_lod_pending[sotc_lod_area].load(std::memory_order_acquire)) {
        return;
    }
    sotc_lod_last = now;
    const u32 area = sotc_lod_area;
    sotc_lod_area = (sotc_lod_area + 1) % SotcLodFeedbackAreas;
    const vk::Buffer feedback = sotc_lod_feedback->Handle();
    const auto barrier = [&](vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                             vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
        const vk::BufferMemoryBarrier2 buffer_barrier = {
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .buffer = feedback,
            .offset = 0,
            .size = SotcLodFeedbackBytes + sizeof(u32),
        };
        scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &buffer_barrier,
        });
    };
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    barrier(vk::PipelineStageFlagBits2::eAllCommands,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
            vk::PipelineStageFlagBits2::eTransfer,
            vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite);
    const vk::BufferCopy copy = {
        .srcOffset = 0,
        .dstOffset = area * SotcLodFeedbackBytes,
        .size = SotcLodFeedbackBytes,
    };
    cmdbuf.copyBuffer(feedback, sotc_lod_download->Handle(), copy);
    barrier(vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead,
            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
    cmdbuf.fillBuffer(feedback, 0, SotcLodFeedbackBytes, ~0u);
    // The next cycle measures only when this is the fourth (gate word, DefineLodFeedback).
    // SOTC_LOD_FB_EVERY=<n>: one cycle in n (1: always).
    static const u32 every = [] {
        const char* v = std::getenv("SOTC_LOD_FB_EVERY");
        const u32 n = v ? static_cast<u32>(std::strtoul(v, nullptr, 0)) : 4u;
        return n ? n : 1u;
    }();
    cmdbuf.fillBuffer(feedback, SotcLodFeedbackBytes, sizeof(u32),
                      ++sotc_lod_cycles % every == 0 ? 1u : 0u);
    barrier(vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eAllCommands,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
    const vk::MemoryBarrier2 host_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &host_barrier,
    });
    sotc_lod_pending[area].store(true, std::memory_order_release);
    scheduler.DeferOperation([this, area] {
        sotc_lod_download->Invalidate(area * SotcLodFeedbackBytes, SotcLodFeedbackBytes);
        SotcLod::MergeGpu(reinterpret_cast<const u32*>(sotc_lod_download->mapped_data.data() +
                                                       area * SotcLodFeedbackBytes));
        sotc_lod_pending[area].store(false, std::memory_order_release);
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
    const SotcOpPhases::Scope ph{SotcOpPhases::PhRenderState}; // sotc: SOTC_OP_PHASES
    const auto texture_lock = texture_cache.BatchLock(); // sotc: one lock for the targets
    // Prefetch render targets to handle overlaps with bound textures (e.g. mipgen)
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = liverpool->DrawRegs();
    if (regs.color_control.degamma_enable) {
        LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
    }
    rs_memo_prepared = false;
    rs_memo_verify = false;
    if (SotcStats::kRsMemo && RsMemoPrepare(pipeline)) { // sotc: SOTC_RS_MEMO
        rs_memo_prepared = true;
        return;
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

// sotc: SOTC_RS_MEMO. The memo repeats SOTC_RT_FAST's evaluation (one TestAndTouchMeta per
// target), so it is off with SOTC_RT_FAST=0.
static bool RsMemoEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_RT_FAST");
        return SotcStats::kRsMemo && (!v || v[0] != '0');
    }();
    return enabled;
}

static constexpr size_t RsMemoMaxWords =
    (sizeof(u32) * 2 + 8 + sizeof(AmdGpu::ColorControl) + sizeof(AmdGpu::ColorBufferMask) +
     AmdGpu::NUM_COLOR_BUFFERS * (sizeof(AmdGpu::ColorBuffer) + sizeof(AmdGpu::CbDbExtent)) +
     sizeof(AmdGpu::DepthControl) + sizeof(AmdGpu::DepthBuffer) + sizeof(AmdGpu::DepthView) +
     sizeof(AmdGpu::Address) + sizeof(AmdGpu::DepthRenderControl) + sizeof(u32) + sizeof(float) +
     sizeof(AmdGpu::CbDbExtent) + 3) /
    4;

u32 Rasterizer::RsMemoBuildKey(const GraphicsPipeline* pipeline) {
    // Every input of PrepareRenderState and BeginRendering besides the image state: the
    // attachment part of the pipeline key, the target registers and the extent hints.
    static_assert(Rasterizer::RsMemoKeyWords >= RsMemoMaxWords);
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = liverpool->DrawRegs();
    u32 n = 0;
    const auto put = [&](const auto& v) {
        static_assert(sizeof(v) % sizeof(u32) == 0);
        std::memcpy(rs_key.data() + n, &v, sizeof(v));
        n += static_cast<u32>(sizeof(v) / sizeof(u32));
    };
    put(key.mrt_mask);
    put(u32{key.num_samples});
    put(key.color_samples);
    put(regs.color_control);
    put(regs.color_target_mask);
    const u32 num_cb =
        std::min<u32>(std::bit_width(key.mrt_mask), AmdGpu::NUM_COLOR_BUFFERS);
    for (u32 cb = 0; cb < num_cb; ++cb) {
        put(regs.color_buffers[cb]);
        put(liverpool->DrawCbExtent()[cb]);
    }
    put(regs.depth_control);
    put(regs.depth_buffer);
    put(regs.depth_view);
    put(regs.depth_htile_data_base);
    put(regs.depth_render_control);
    put(regs.stencil_clear);
    put(regs.depth_clear);
    put(liverpool->DrawDbExtent());
    return n;
}

bool Rasterizer::RsMemoTargetClean(VideoCore::ImageId image_id) {
    // What lets the full path's UpdateImage / RefreshImage / TrackImage and the needs_rebind
    // re-lookup be skipped (the same test as TryBindCache's).
    const auto& image = texture_cache.GetImage(image_id);
    return image.backing && True(image.flags & VideoCore::ImageFlagBits::Registered) &&
           False(image.flags & VideoCore::ImageFlagBits::Dirty) &&
           image.track_addr == image.info.guest_address &&
           image.track_addr_end == image.info.guest_address + image.info.guest_size &&
           !image.binding.needs_rebind;
}

bool Rasterizer::RsMemoPrepare(const GraphicsPipeline* pipeline) {
    // sotc: SOTC_RS_MEMO, under PrepareRenderState's batch lock. A miss drops the memo: the full
    // path rewrites cb_descs / db_desc, which a later hit would read.
    if (!RsMemoEnabled()) {
        return false;
    }
    rs_key_len = RsMemoBuildKey(pipeline);
    if (SotcStats::kFpsLog) {
        SotcStats::rs_memo_lookups.fetch_add(1, std::memory_order_relaxed);
    }
    const auto miss = [&](u32 reason) {
        rs_memo.valid = false;
        if (SotcStats::kFpsLog || SotcOpPhases::kOn) { // sotc: SOTC_OP_PHASES reads the misses too
            SotcStats::rs_memo_misses[reason].fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    };
    if (!rs_memo.valid) {
        return miss(SotcStats::RsMissNone);
    }
    const bool same_tick = rs_memo.tick == scheduler.CurrentTick();
    if (!same_tick && !SotcStats::kRsMemoCross) {
        return miss(SotcStats::RsMissTick);
    }
    if (rs_memo.texture_generation != texture_cache.BindGeneration()) {
        return miss(SotcStats::RsMissResources);
    }
    if (rs_memo.key_len != rs_key_len ||
        std::memcmp(rs_memo.key.data(), rs_key.data(), rs_key_len * sizeof(u32)) != 0) {
        return miss(SotcStats::RsMissRegs);
    }
    const u32 num_cb = rs_memo.state.num_color_attachments;
    if (same_tick) {
        if (!scheduler.IsRendering() || !(scheduler.GetRenderState() == rs_memo.state)) {
            return miss(SotcStats::RsMissPass);
        }
        for (u32 cb = 0; cb < num_cb; ++cb) {
            if (const auto id = cb_descs[cb].first; id && !RsMemoTargetClean(id)) {
                return miss(SotcStats::RsMissImage);
            }
        }
        if (const auto id = db_desc.first; id && !RsMemoTargetClean(id)) {
            return miss(SotcStats::RsMissImage);
        }
    } else {
        // sotc: SOTC_RS_MEMO=2, an earlier submit. The memo's own targets are checked (cb_descs may
        // hold another draw's), and the pass need not be open: BeginRendering checks the bindings
        // and opens it.
        if (rs_memo.meta_epoch != texture_cache.MetaEpoch()) {
            return miss(SotcStats::RsMissMeta);
        }
        for (u32 slot = 0; slot < rs_memo.target_id.size(); ++slot) {
            const auto id = rs_memo.target_id[slot];
            if (!RsMemoSameImage(slot, id)) {
                return miss(SotcStats::RsMissIdentity);
            }
            if (id && !RsMemoTargetClean(id)) {
                return miss(SotcStats::RsMissImage);
            }
        }
    }
    if (SotcStats::kRsMemoVerify) { // sotc: SOTC_RS_MEMO=3, the full path runs and is compared
        rs_memo_verify = true;
        return false;
    }
    if (!same_tick) {
        // sotc: the draw's targets are the memo's (checked above). The marks below and the
        // BeginRendering hit read cb_descs / db_desc.
        for (u32 cb = 0; cb < num_cb; ++cb) {
            cb_descs[cb].first = rs_memo.target_id[cb];
        }
        db_desc.first = rs_memo.target_id[AmdGpu::NUM_COLOR_BUFFERS];
    }
    // The targets of this draw, as the full path marks them (ResetBindings cleared them).
    for (u32 cb = 0; cb < num_cb; ++cb) {
        if (const auto id = cb_descs[cb].first) {
            bound_images.emplace_back(id);
            texture_cache.GetImage(id).binding.is_target = 1u;
        }
    }
    if (const auto id = db_desc.first) {
        bound_images.emplace_back(id);
        texture_cache.GetImage(id).binding.is_target = 1u;
    }
    return true;
}

// sotc: SOTC_RS_MEMO=2. True when Image::GetBarriers would emit no barrier and change no state for
// this transition, so a replay of an earlier submit may skip Runtime::Transit. Mirrors its
// full-resource branch (image.cpp): a partial range or per-subresource states take the other
// branch, and a write access is left to the full path (SkipSameGeneral).
static bool RsMemoTransitIsNoop(const VideoCore::Image& image, vk::ImageLayout layout,
                                vk::AccessFlags2 access, const VideoCore::SubresourceRange& range) {
    const auto& backing = *image.backing;
    if (range.base != VideoCore::SubresourceBase{} || range.extent != image.info.resources ||
        !backing.subresource_states.empty()) {
        return false;
    }
    constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                 vk::AccessFlagBits2::eShaderWrite |
                                 vk::AccessFlagBits2::eMemoryWrite;
    return backing.state.layout == layout && backing.state.access_mask == access &&
           !static_cast<bool>(backing.state.access_mask & write_flags);
}

bool Rasterizer::RsMemoSameImage(u32 slot, VideoCore::ImageId image_id) {
    // sotc: SOTC_RS_MEMO=2. The image in a memo slot is still the one the memo drew: same uid
    // (slots are reused) and same backing (its state views and sample count are the memo's).
    if (!image_id) {
        return true;
    }
    const auto& image = texture_cache.GetImage(image_id);
    return image.image_uid == rs_memo.target_uid[slot] &&
           image.backing == rs_memo.target_backing[slot];
}

Rasterizer::RsMemoHit Rasterizer::RsMemoBeginCheck() {
    // sotc: SOTC_RS_MEMO, under BeginRendering's batch lock, after the binds of this draw (they
    // may have submitted, uploaded, ended the pass, registered images or sampled a target).
    // Read-only apart from the miss counters. Same tick: the checks of mode 1, unchanged. Another
    // tick (SOTC_RS_MEMO=2): the pass need not be open, and the targets must be the memo's own.
    const auto miss = [&](u32 reason) {
        if (SotcStats::kFpsLog || SotcOpPhases::kOn) { // sotc: SOTC_OP_PHASES reads the misses too
            SotcStats::rs_memo_misses[reason].fetch_add(1, std::memory_order_relaxed);
        }
        return RsMemoHit::Miss;
    };
    const bool same_tick = rs_memo.tick == scheduler.CurrentTick();
    if (!same_tick && !SotcStats::kRsMemoCross) {
        return miss(SotcStats::RsMissTick);
    }
    if (rs_memo.texture_generation != texture_cache.BindGeneration()) {
        return miss(SotcStats::RsMissResources);
    }
    if (rs_memo.meta_epoch != texture_cache.MetaEpoch()) {
        return miss(SotcStats::RsMissMeta);
    }
    if (same_tick &&
        (!scheduler.IsRendering() || !(scheduler.GetRenderState() == rs_memo.state))) {
        return miss(SotcStats::RsMissPass);
    }
    const u32 num_cb = rs_memo.state.num_color_attachments;
    for (u32 cb = 0; cb < num_cb; ++cb) {
        const auto id = cb_descs[cb].first;
        if (!same_tick && (id != rs_memo.target_id[cb] || !RsMemoSameImage(cb, id))) {
            return miss(SotcStats::RsMissIdentity);
        }
        if (!id) {
            continue;
        }
        const auto& image = texture_cache.GetImage(id);
        // A sampled target (feedback loop) or a backing of another sample count
        // (SetBackingSamples would swap it) takes the full path.
        if (!RsMemoTargetClean(id) || image.binding.is_bound ||
            image.backing->num_samples != rs_memo.color_samples[cb]) {
            return miss(SotcStats::RsMissImage);
        }
    }
    constexpr u32 depth_slot = AmdGpu::NUM_COLOR_BUFFERS;
    const auto db_id = db_desc.first;
    if (!same_tick &&
        (db_id != rs_memo.target_id[depth_slot] || !RsMemoSameImage(depth_slot, db_id))) {
        return miss(SotcStats::RsMissIdentity);
    }
    if (db_id && !RsMemoTargetClean(db_id)) {
        return miss(SotcStats::RsMissImage);
    }
    return same_tick ? RsMemoHit::Same : RsMemoHit::Cross;
}

void Rasterizer::RsMemoVerify(const RenderState& full, RsMemoHit hit) {
    // sotc: SOTC_RS_MEMO=3. The full path's result against what a hit would have replayed: the
    // stored state with the layouts the replay sets (the backings after the full path's
    // transitions), and the memo's image ids. The result of the full path is what is used.
    if (SotcStats::kFpsLog) { // sotc: candidates are counted as checks, not as hits
        SotcStats::rs_memo_verify_checks.fetch_add(1, std::memory_order_relaxed);
    }
    RenderState expect = rs_memo.state;
    bool same = true;
    const u32 num_cb = rs_memo.state.num_color_attachments;
    for (u32 cb = 0; cb < num_cb; ++cb) {
        const auto id = cb_descs[cb].first;
        same &= id == rs_memo.target_id[cb];
        if (id) {
            expect.color_attachments[cb].image_layout =
                texture_cache.GetImage(id).backing->state.layout;
        }
    }
    constexpr u32 depth_slot = AmdGpu::NUM_COLOR_BUFFERS;
    const auto db_id = db_desc.first;
    same &= db_id == rs_memo.target_id[depth_slot];
    if (db_id) {
        expect.depth_stencil_attachment.image_layout =
            texture_cache.GetImage(db_id).backing->state.layout;
    }
    // RenderState has no padding (static_assert in vk_scheduler.h), so memcmp compares values.
    same &= std::memcmp(&expect, &full, sizeof(RenderState)) == 0;
    // A transition a cross replay would skip must be one the full path did not change.
    if (hit == RsMemoHit::Cross) {
        const auto unchanged = [&](u32 slot, VideoCore::ImageId id) {
            const auto& pred = rs_verify_pred[slot];
            if (!id || !pred.noop) {
                return true;
            }
            const auto& state = texture_cache.GetImage(id).backing->state;
            return state.layout == pred.layout && state.access_mask == pred.access;
        };
        for (u32 cb = 0; cb < num_cb; ++cb) {
            same &= unchanged(cb, cb_descs[cb].first);
        }
        same &= unchanged(depth_slot, db_desc.first);
    }
    if (!same) {
        SotcStats::rs_memo_verify_mismatches.fetch_add(1, std::memory_order_relaxed);
    }
}

bool Rasterizer::RsMemoBegin(RenderState& state) {
    const RsMemoHit hit = RsMemoBeginCheck();
    if (hit == RsMemoHit::Miss) {
        return false;
    }
    const bool cross = hit == RsMemoHit::Cross;
    const u32 num_cb = rs_memo.state.num_color_attachments;

    state = rs_memo.state;
    for (u32 cb = 0; cb < num_cb; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            continue;
        }
        auto* image = &texture_cache.GetImage(image_id);
        texture_cache.ReuseTarget(image_id, false);
        // sotc: a replay of an earlier submit runs the transition unless it is a provable no-op.
        const auto access = vk::AccessFlagBits2::eColorAttachmentWrite |
                            vk::AccessFlagBits2::eColorAttachmentRead;
        if (!cross || !RsMemoTransitIsNoop(*image, vk::ImageLayout::eColorAttachmentOptimal, access,
                                           desc.view_info.range)) {
            needs_barrier |= runtime.Transit(image, vk::ImageLayout::eColorAttachmentOptimal,
                                             vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                             access, desc.view_info.range);
        }
        state.color_attachments[cb].image_layout = image->backing->state.layout;
        if (cb == 0 && image->usage.vo_surface) { // sotc: TakeHudless
            hudless_vo = image->info.guest_address;
            hudless_vo_format = state.color_formats[0];
            hudless_vo_width = image->info.size.width;
            hudless_vo_height = image->info.size.height;
        }
        if (cb == 0 && rs_memo.cb0_base && // sotc: BeginUiShadow
            Dlssg::UiTarget(image->info.guest_address, image->info.size.width,
                            image->info.size.height, ui_shadow_width, ui_shadow_height)) {
            ui_shadow_hit = true;
            ui_shadow_format = state.color_formats[0];
            ui_shadow_src_width = image->info.size.width;
            ui_shadow_src_height = image->info.size.height;
        }
    }
    if (const auto image_id = db_desc.first) {
        auto& image = texture_cache.GetImage(image_id);
        texture_cache.ReuseTarget(image_id, true);
        if (!cross || !RsMemoTransitIsNoop(image, rs_memo.depth_layout, rs_memo.depth_access,
                                           db_desc.second.view_info.range)) {
            needs_barrier |= runtime.Transit(&image, rs_memo.depth_layout, rs_memo.depth_stages,
                                             rs_memo.depth_access, db_desc.second.view_info.range);
        }
        state.depth_stencil_attachment.image_layout = image.backing->state.layout;
    }
    if (SotcStats::kFpsLog) {
        SotcStats::rs_memo_hits.fetch_add(1, std::memory_order_relaxed);
        if (cross) {
            SotcStats::rs_memo_cross_hits.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (cross) {
        // sotc: the memo belongs to this submit now: its tick, and its state with the layouts the
        // replay set, so the pass the caller opens compares equal to it (same-tick checks).
        rs_memo.tick = scheduler.CurrentTick();
        rs_memo.state = state;
    }
    return true;
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
    BindCacheInvalidateImage(image_id); // sotc: SOTC_BIND_CACHE_RANGES (no-op otherwise)
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
// SOTC_ADAPTIVE_LOAD never uses this skip: a skipped slice holds texels of another light matrix
// at best and, as measured (see SOTC_SHADOW_REUSE below), none at all: missing far shadows.
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
    int mode = live >= 0 ? live : env_mode;
    // sotc: SOTC_ADAPTIVE_LOAD (sotc_adaptive.h): with the reuse off by the launcher/env (and no
    // choice in the in-game menu, which keeps priority), it is on while the governor is relieved.
    // The governor's state is latched at the first draw of a frame, so a frame never switches
    // between moving and drawing a cascade halfway (a pass begun after a moved part could clear
    // it). An explicit SOTC_SHADOW_REUSE=1/check is left as it is.
    bool adaptive = false;
    if (mode == 0 && live < 0 && SotcAdaptive::kShadows) {
        static u64 latch_frame = ~u64{0};
        static bool latch = false;
        const u64 now_frame = SotcStats::cp_frame.load(std::memory_order_relaxed);
        if (now_frame != latch_frame) {
            latch_frame = now_frame;
            latch = SotcAdaptive::AmortiseShadows();
        }
        adaptive = latch;
        mode = latch ? 1 : 0;
    }
    if (mode == 0) {
        return false;
    }
    static ShadowReuseState s;
    const auto& regs = liverpool->DrawRegs(); // sotc: the draw thread's copy there
    const u64 frame = SotcStats::cp_frame.load(std::memory_order_relaxed);
    if (frame != s.frame_seen && SotcAdaptive::kEnabled && frame != s.frame_seen + 1) {
        // sotc: SOTC_ADAPTIVE_LOAD: frames went by untracked (the reuse was off): the counts, the
        // matrices and "drawn last frame" are from an older frame, whose texels are gone. Start
        // over: this frame only finds the shadow map, the next draws and records each cascade,
        // and only the one after can move last frame's texels.
        s.counts.clear();
        s.sun_map = 0;
        s.have_vp.fill(false);
        s.drawn_now.fill(false);
    }
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
    if (moved && adaptive && SotcStats::kFpsLog) {
        SotcStats::adaptive_amortised.fetch_add(1, std::memory_order_relaxed);
    }
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
    // sotc: the descriptor arrays of the last bind change here (the UI image becomes the clear
    // image): no later fast-bind reuse of them, wide or not.
    fast_bind.valid = false;
    auto& ui = image_infos[hudless_ui_info];
    ui.imageView = hudless_clear.rt_view;
    ui.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    pipeline->BindResources(set_writes, set_write_sets, push_data, bindless_slots);
    RenderState hudless_state = state;
    auto& attachment = hudless_state.color_attachments[0];
    attachment.image_view = target.rt_view;
    attachment.image_layout = vk::ImageLayout::eColorAttachmentOptimal;
    attachment.is_clear = 0;
    hudless_state.color_formats[0] = target.rt_format;
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
    shadow.color_formats[0] = ui_shadow_format;
    for (u32 cb = 1; cb < shadow.num_color_attachments; ++cb) {
        shadow.color_attachments[cb].image_view = vk::ImageView{}; // of the render size
        shadow.color_formats[cb] = vk::Format{};
    }
    auto& depth = shadow.depth_stencil_attachment;
    depth.image_view = vk::ImageView{};
    depth.depth_clear = false;
    depth.stencil_clear = false;
    shadow.depth_format = vk::Format{};
    shadow.stencil_format = vk::Format{};
    shadow.samples = vk::SampleCountFlagBits::e1; // the UI target is a single sample image
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

// sotc: SOTC_MDI_STATS=1 (off by default; a measurement for a possible multi-draw merge). The
// XPR cull emits, inside one batch, consecutive indirect draws whose only difference is the
// argument offset (+stride per section; eboot 0x1a6d20). Two consecutive indirect draws are
// "mergeable" when nothing ran between them (no direct draw or dispatch), the second one hit the
// fast bind (same pipeline, push data, targets and resources), and its arguments directly follow
// the first's in the same buffer, in the same submit. Counts the run lengths; every 5 s one line.
namespace {
const bool kMdiStats = [] {
    const char* v = std::getenv("SOTC_MDI_STATS");
    return v && v[0] == '1';
}();
struct MdiState {
    const void* pipeline = nullptr;
    VAddr base = 0;
    u64 next_offset = 0;
    u32 stride = 0;
    u64 tick = 0;
    bool indexed = false;
    bool open = false;
    u64 run = 0;
    u64 draws = 0, hits = 0, mergeable = 0, gpu_args = 0;
    u64 buckets[7]{}; // run length 1, 2, 3-4, 5-8, 9-16, 17-64, 65+
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
MdiState g_mdi; // the draw thread only
void MdiCloseRun() {
    if (g_mdi.open && g_mdi.run) {
        const u64 r = g_mdi.run;
        const int b = r <= 1 ? 0 : r == 2 ? 1 : r <= 4 ? 2 : r <= 8 ? 3 : r <= 16 ? 4 : r <= 64 ? 5 : 6;
        ++g_mdi.buckets[b];
        g_mdi.mergeable += r - 1;
    }
    g_mdi.open = false;
    g_mdi.run = 0;
}
void MdiBreak() { // a direct draw or a dispatch ends the run
    if (kMdiStats) {
        MdiCloseRun();
    }
}
void MdiNote(const void* pipeline, bool indexed, VAddr base, u64 offset, u32 stride, u32 max_count,
             VAddr count_address, bool fast_hit, bool gpu_args, u64 tick) {
    ++g_mdi.draws;
    g_mdi.hits += fast_hit ? 1 : 0;
    g_mdi.gpu_args += gpu_args ? 1 : 0;
    const bool single = max_count == 1 && count_address == 0;
    const bool joins = g_mdi.open && single && fast_hit && pipeline == g_mdi.pipeline &&
                       indexed == g_mdi.indexed && base == g_mdi.base && stride == g_mdi.stride &&
                       offset == g_mdi.next_offset && tick == g_mdi.tick;
    if (joins) {
        ++g_mdi.run;
    } else {
        MdiCloseRun();
        if (single) {
            g_mdi.open = true;
            g_mdi.run = 1;
        }
    }
    g_mdi.pipeline = pipeline;
    g_mdi.indexed = indexed;
    g_mdi.base = base;
    g_mdi.stride = stride;
    g_mdi.next_offset = offset + stride;
    g_mdi.tick = tick;
    if ((g_mdi.draws & 1023) == 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now - g_mdi.last >= std::chrono::seconds(5)) {
            g_mdi.last = now;
            const auto& k = g_mdi.buckets;
            std::fprintf(stderr,
                         "SOTC MDI: indirect %llu (GPU args %llu), fast-bind hits %llu, mergeable "
                         "%llu (%.1f%%); runs: 1 %llu, 2 %llu, 3-4 %llu, 5-8 %llu, 9-16 %llu, "
                         "17-64 %llu, 65+ %llu\n",
                         (unsigned long long)g_mdi.draws, (unsigned long long)g_mdi.gpu_args,
                         (unsigned long long)g_mdi.hits, (unsigned long long)g_mdi.mergeable,
                         g_mdi.draws ? 100.0 * double(g_mdi.mergeable) / double(g_mdi.draws) : 0.0,
                         (unsigned long long)k[0], (unsigned long long)k[1],
                         (unsigned long long)k[2], (unsigned long long)k[3],
                         (unsigned long long)k[4], (unsigned long long)k[5],
                         (unsigned long long)k[6]);
            const auto last = g_mdi.last;
            g_mdi = MdiState{};
            g_mdi.last = last;
        }
    }
}

// sotc: SOTC_CS_STATS=1 (off by default; a measurement for a possible compute fast bind). Each
// dispatch's bind state is keyed by the pipeline, the resource sharps it binds and its user data
// and flat buffer. Counts how many dispatches repeat the previous dispatch's key ("same"), and of
// those how many also saw no buffer/texture/memory change in between in the same command buffer
// ("clean": what a fast bind could skip), or repeat any key bound earlier in the same command
// buffer ("seen"); and how much time BindResources takes for each group. One line every 5 s.
const bool kCsStats = [] {
    const char* v = std::getenv("SOTC_CS_STATS");
    return v && v[0] == '1';
}();
struct CsStatsState {
    u64 prev_key = 0, prev_gen = 0, tick = ~u64{0};
    bool prev_ok = false;
    std::unordered_set<u64> seen;
    u64 dispatches = 0, keyed = 0, writing = 0, same = 0, clean = 0, seen_hits = 0;
    u64 ns_all = 0, ns_same = 0, ns_clean = 0, ns_seen = 0;
    // The same with the resources-only key (BindResourceHash: the sharps, no constants).
    u64 res_prev = 0, res_prev_gen = 0;
    bool res_prev_ok = false;
    std::unordered_set<u64> res_seen;
    u64 res_keyed = 0, res_same = 0, res_clean = 0, res_seen_hits = 0;
    u64 ns_res_same = 0, ns_res_clean = 0, ns_res_seen = 0;
    // The descriptors the bind produced (buffer_infos / image_infos): identical to the previous
    // dispatch's, or the same buffers, ranges and images with only the buffer offsets moved.
    u64 desc_prev = 0, handle_prev = 0;
    bool desc_prev_ok = false;
    u64 desc_keyed = 0, desc_same = 0, offsets_only = 0;
    u64 ns_desc_same = 0, ns_offsets_only = 0;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
CsStatsState g_cs; // the draw thread only

/// The key of a compute bind; false when the pipeline binds by constants (BindResourceHash).
bool CsStatsKey(const Vulkan::Pipeline* pipeline, u64& key, bool& writes) {
    writes = false;
    u64 hash = XXH3_64bits(&pipeline, sizeof(pipeline));
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        if (stage->uses_dma || !stage->sharp_tables.empty() || stage->UserData().data() == nullptr) {
            return false;
        }
        writes |= stage->has_storage_images;
        for (const auto& desc : stage->buffers) {
            writes |= !desc.IsSpecial() && desc.is_written;
        }
        for (const auto& desc : stage->images) {
            writes |= desc.is_written;
        }
        const auto user_data = stage->UserData();
        hash = XXH3_64bits_withSeed(user_data.data(), user_data.size_bytes(), hash);
        const auto& flat_buf = stage->FlatBuf();
        if (!flat_buf.empty()) {
            hash = XXH3_64bits_withSeed(flat_buf.data(), flat_buf.size() * sizeof(u32), hash);
        }
    }
    key = hash;
    return true;
}

/// The descriptors of a finished bind: with the buffer offsets (desc) and without them (handle).
template <typename BufInfos, typename ImgInfos>
void CsDescKeys(const Vulkan::Pipeline* pipeline, const BufInfos& bufs, const ImgInfos& imgs,
                u64& desc, u64& handle) {
    u64 d = XXH3_64bits(&pipeline, sizeof(pipeline));
    u64 h = d;
    for (const auto& b : bufs) {
        const VkBuffer vb = b.buffer;
        const u64 off = b.offset, range = b.range;
        h = XXH3_64bits_withSeed(&vb, sizeof(vb), h);
        h = XXH3_64bits_withSeed(&range, sizeof(range), h);
        d = XXH3_64bits_withSeed(&vb, sizeof(vb), d);
        d = XXH3_64bits_withSeed(&range, sizeof(range), d);
        d = XXH3_64bits_withSeed(&off, sizeof(off), d);
    }
    for (const auto& i : imgs) {
        const VkImageView v = i.imageView;
        const VkSampler smp = i.sampler;
        const u32 layout = static_cast<u32>(i.imageLayout);
        for (u64* k : {&d, &h}) {
            *k = XXH3_64bits_withSeed(&v, sizeof(v), *k);
            *k = XXH3_64bits_withSeed(&smp, sizeof(smp), *k);
            *k = XXH3_64bits_withSeed(&layout, sizeof(layout), *k);
        }
    }
    desc = d;
    handle = h;
}

void CsStatsNote(bool ok, u64 key, bool res_ok, u64 res_key, bool writes, u64 gen, u64 tick,
                 u64 ns, bool desc_ok, u64 desc_key, u64 handle_key) {
    auto& s = g_cs;
    if (tick != s.tick) {
        s.tick = tick;
        s.seen.clear();
        s.prev_ok = false;
        s.res_seen.clear();
        s.res_prev_ok = false;
        s.desc_prev_ok = false;
    }
    if (desc_ok) {
        ++s.desc_keyed;
        if (s.desc_prev_ok && handle_key == s.handle_prev) {
            if (desc_key == s.desc_prev) {
                ++s.desc_same;
                s.ns_desc_same += ns;
            } else {
                ++s.offsets_only;
                s.ns_offsets_only += ns;
            }
        }
        s.desc_prev = desc_key;
        s.handle_prev = handle_key;
    }
    s.desc_prev_ok = desc_ok;
    if (res_ok) {
        ++s.res_keyed;
        if (s.res_prev_ok && res_key == s.res_prev) {
            ++s.res_same;
            s.ns_res_same += ns;
            if (gen == s.res_prev_gen) {
                ++s.res_clean;
                s.ns_res_clean += ns;
            }
        }
        if (!s.res_seen.insert(res_key).second) {
            ++s.res_seen_hits;
            s.ns_res_seen += ns;
        }
        s.res_prev = res_key;
        s.res_prev_gen = gen;
    }
    s.res_prev_ok = res_ok;
    ++s.dispatches;
    s.ns_all += ns;
    s.writing += writes ? 1 : 0;
    if (ok) {
        ++s.keyed;
        if (s.prev_ok && key == s.prev_key) {
            ++s.same;
            s.ns_same += ns;
            if (gen == s.prev_gen) {
                ++s.clean;
                s.ns_clean += ns;
            }
        }
        if (!s.seen.insert(key).second) {
            ++s.seen_hits;
            s.ns_seen += ns;
        }
        s.prev_key = key;
        s.prev_gen = gen;
    }
    s.prev_ok = ok;
    if ((s.dispatches & 255) == 0) {
        const auto now = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(now - s.last).count();
        if (secs >= 5.0) {
            const auto ms = [&](u64 v) { return double(v) / 1e6 / secs; };
            std::fprintf(stderr,
                         "SOTC CS STATS: dispatches %llu (keyed %llu, writing %llu), same as "
                         "previous %llu (clean %llu), seen in this submit %llu; bind ms/s: all "
                         "%.1f, same %.1f, clean %.1f, seen %.1f\n",
                         (unsigned long long)s.dispatches, (unsigned long long)s.keyed,
                         (unsigned long long)s.writing, (unsigned long long)s.same,
                         (unsigned long long)s.clean, (unsigned long long)s.seen_hits,
                         ms(s.ns_all), ms(s.ns_same), ms(s.ns_clean), ms(s.ns_seen));
            std::fprintf(stderr,
                         "SOTC CS STATS resources only: keyed %llu, same as previous %llu "
                         "(clean %llu), seen in this submit %llu; bind ms/s: same %.1f, clean "
                         "%.1f, seen %.1f\n",
                         (unsigned long long)s.res_keyed, (unsigned long long)s.res_same,
                         (unsigned long long)s.res_clean, (unsigned long long)s.res_seen_hits,
                         ms(s.ns_res_same), ms(s.ns_res_clean), ms(s.ns_res_seen));
            std::fprintf(stderr,
                         "SOTC CS STATS descriptors: bound %llu, identical to previous %llu, "
                         "same buffers with other offsets %llu; bind ms/s: identical %.1f, "
                         "offsets only %.1f\n",
                         (unsigned long long)s.desc_keyed, (unsigned long long)s.desc_same,
                         (unsigned long long)s.offsets_only, ms(s.ns_desc_same),
                         ms(s.ns_offsets_only));
            const auto tick_keep = s.tick;
            s = CsStatsState{};
            s.last = now;
            s.tick = tick_keep;
        }
    }
}
} // namespace

void Rasterizer::Draw(bool is_indexed, u32 index_offset, const Pipeline* resolved,
                      bool use_resolved, const PreparedDraw* prepared) {
    RENDERER_TRACE;
    MdiBreak(); // sotc: SOTC_MDI_STATS
    const SotcOpPhases::OpScope op_scope{SotcOpPhases::OpDraw}; // sotc: SOTC_OP_PHASES

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
                     : [&] { // sotc: SOTC_TRACE
                           const SotcTrace::ScopeMin trace_step{"get pipeline"};
                           const SotcOpPhases::Scope ph{SotcOpPhases::PhPipeline}; // sotc: SOTC_OP_PHASES
                           return pipeline_cache.GetGraphicsPipeline();
                       }();
    SotcStats::current_program.store(1, std::memory_order_relaxed);
    if (SotcStats::kCmdStats) { // sotc: read only by the SOTC_CMD_STATS printer (sotc_stats.h)
        SotcStats::cmd_kinds[0].fetch_add(1, std::memory_order_relaxed);
        if (regs.num_indices == 0 || regs.num_instances.NumInstances() == 0) {
            SotcStats::cmd_kinds[6].fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!pipeline) {
        SotcStats::Drop(0, regs.color_buffers[0].Address(), regs.stage_enable.raw);
        return;
    }
    TraceGbufferShaders(regs, pipeline);
    TraceShadowShaders(regs, pipeline);
    if (prepared) { // sotc: SOTC_PARALLEL_RECORD (SOTC_PR_VERIFY compares it)
        prepared = CheckPrepared(*prepared, pipeline, is_indexed, true);
    }

    {
        const SotcTrace::ScopeMin trace_step{"render state"}; // sotc: SOTC_TRACE
        PrepareRenderState(pipeline);
    }
    hudless_ui_info = -1; // sotc: TakeHudless, set by BindTextures and BeginRendering
    hudless_vo = 0;
    if (Dlssg::CapturingInputs()) { // sotc: camera and inputs of DLSS (frame generation, SR)
        Dlssg::NoteDraw(regs, pipeline->GetGraphicsKey().mrt_mask, db_desc.first,
                        cb_descs[4].first, texture_cache, runtime, scheduler);
    }
    bool bound; // sotc: SOTC_TRACE
    {
        const SotcTrace::ScopeMin trace_step{"bind resources"};
        bound = BindResources(pipeline, prepared);
    }
    if (!bound) {
        SotcStats::Drop(1, regs.color_buffers[0].Address(), 0);
        return;
    }
    const auto state = [&] { // sotc: SOTC_TRACE
        const SotcTrace::ScopeMin trace_step{"begin rendering"};
        return BeginRendering(pipeline);
    }();
    RefreshImageLayouts();

    // sotc: the binds below can submit the command buffer (arena allocation, readbacks,
    // releases); what was recorded before lands in the old one. Re-record vertex state then.
    u64 vertex_tick = scheduler.CurrentTick();
    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhVertexIndex}; // sotc: SOTC_OP_PHASES
        {
            const SotcTrace::BindScope trace_step{"vertex buffers"}; // sotc: SOTC_TRACE
            BindVertexBuffers(pipeline);
        }
        if (is_indexed) {
            BindIndexBuffer(index_offset);
        }
        RebindVertexIfSubmitted(pipeline, vertex_tick, is_indexed, [&] { BindIndexBuffer(index_offset); });
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhDescriptorPush}; // sotc: SOTC_OP_PHASES
        if (!fast_bind_hit || fast_bind.tick != scheduler.CurrentTick()) {
            pipeline->BindResources(set_writes, set_write_sets, push_data, bindless_slots);
        }
    }
    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhDynamicState}; // sotc: SOTC_OP_PHASES
        UpdateDynamicState(pipeline, is_indexed, prepared);
    }
    scheduler.BeginRendering(state);

    const auto& vs_info = pipeline->GetStage(Shader::SwStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    const auto [vertex_offset, instance_offset] =
        prepared && prepared->has_offsets
            ? std::pair<u32, u32>{prepared->vertex_offset, prepared->instance_offset}
            : GetDrawOffsets(regs, vs_info, fetch_shader);

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

    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhReset}; // sotc: SOTC_OP_PHASES
        ResetBindings(false);
        FlushPeriodic();
    }
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address, u16 vertex_sgpr_offset,
                              u16 instance_sgpr_offset, const Pipeline* resolved,
                              bool use_resolved, const PreparedDraw* prepared) {
    RENDERER_TRACE;
    const SotcOpPhases::OpScope op_scope{SotcOpPhases::OpDrawIndirect}; // sotc: SOTC_OP_PHASES

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
                     : [&] { // sotc: SOTC_TRACE
                           const SotcTrace::ScopeMin trace_step{"get pipeline"};
                           const SotcOpPhases::Scope ph{SotcOpPhases::PhPipeline}; // sotc: SOTC_OP_PHASES
                           return pipeline_cache.GetGraphicsPipeline(params);
                       }();
    if (!pipeline) {
        SotcStats::Drop(0, liverpool->DrawRegs().color_buffers[0].Address(), 1);
        return;
    }
    TraceShadowShaders(liverpool->DrawRegs(), pipeline);
    if (prepared) { // sotc: SOTC_PARALLEL_RECORD (SOTC_PR_VERIFY compares it)
        prepared = CheckPrepared(*prepared, pipeline, is_indexed, false);
    }

    {
        const SotcTrace::ScopeMin trace_step{"render state"}; // sotc: SOTC_TRACE
        PrepareRenderState(pipeline);
    }
    if (Dlssg::CapturingInputs()) { // sotc: camera and inputs of DLSS (frame generation, SR)
        Dlssg::NoteDraw(liverpool->DrawRegs(), pipeline->GetGraphicsKey().mrt_mask, db_desc.first,
                        cb_descs[4].first, texture_cache, runtime, scheduler);
    }
    bool bound; // sotc: SOTC_TRACE
    {
        const SotcTrace::ScopeMin trace_step{"bind resources"};
        bound = BindResources(pipeline, prepared);
    }
    if (!bound) {
        SotcStats::Drop(1, liverpool->DrawRegs().color_buffers[0].Address(), 1);
        return;
    }
    if (kMdiStats) { // sotc: SOTC_MDI_STATS (fast_bind_hit is this draw's, set by BindResources)
        MdiNote(pipeline, is_indexed, arg_address, offset, stride, max_count, count_address,
                fast_bind_hit, buffer_cache.IsRegionGpuModified(arg_address + offset, stride),
                scheduler.CurrentTick());
    }
    const auto state = [&] { // sotc: SOTC_TRACE
        const SotcTrace::ScopeMin trace_step{"begin rendering"};
        return BeginRendering(pipeline);
    }();
    ui_shadow_broken |= ui_shadow_hit; // sotc: indirect draws are not redrawn (BeginUiShadow)
    RefreshImageLayouts();

    u64 vertex_tick = scheduler.CurrentTick(); // sotc: see the direct draw
    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhVertexIndex}; // sotc: SOTC_OP_PHASES
        {
            const SotcTrace::BindScope trace_step{"vertex buffers"}; // sotc: SOTC_TRACE
            BindVertexBuffers(pipeline);
        }
        if (is_indexed) {
            BindIndexBuffer();
        }
        RebindVertexIfSubmitted(pipeline, vertex_tick, is_indexed, [&] { BindIndexBuffer(); });
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
        {
            const SotcOpPhases::Scope ph{SotcOpPhases::PhDescriptorPush}; // sotc: SOTC_OP_PHASES
            if (!fast_bind_hit || fast_bind.tick != scheduler.CurrentTick()) {
                pipeline->BindResources(set_writes, set_write_sets, push_data, bindless_slots);
            }
        }
        {
            const SotcOpPhases::Scope ph{SotcOpPhases::PhDynamicState}; // sotc: SOTC_OP_PHASES
            UpdateDynamicState(pipeline, is_indexed, prepared);
        }
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
        {
            const SotcOpPhases::Scope ph{SotcOpPhases::PhReset}; // sotc: SOTC_OP_PHASES
            ResetBindings(false);
            FlushPeriodic();
        }
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
    const auto [buffer, base] = [&] { // sotc: SOTC_TRACE
        const SotcTrace::BindScope trace_step{"indirect args"};
        return buffer_cache.ObtainBuffer(arg_address + offset, stride * max_count, false);
    }();
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, stride * max_count);

    const VideoCore::Buffer* count_buffer;
    u64 count_offset;
    if (count_address != 0) {
        std::tie(count_buffer, count_offset) = buffer_cache.ObtainBuffer(count_address, 4, false);
        needs_barrier |= runtime.IsBufferAccessed(count_buffer, count_offset, 4);
    }
    // sotc: the argument buffers can submit too.
    RebindVertexIfSubmitted(pipeline, vertex_tick, is_indexed, [&] { BindIndexBuffer(); });

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhDescriptorPush}; // sotc: SOTC_OP_PHASES
        if (!fast_bind_hit || fast_bind.tick != scheduler.CurrentTick()) {
            pipeline->BindResources(set_writes, set_write_sets, push_data, bindless_slots);
        }
    }
    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhDynamicState}; // sotc: SOTC_OP_PHASES
        UpdateDynamicState(pipeline, is_indexed, prepared);
    }
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

    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhReset}; // sotc: SOTC_OP_PHASES
        ResetBindings(false);
        FlushPeriodic();
    }
}

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

// sotc: while DLSS takes the jittered frame (or SOTC_TAA_OFF=1), the game's TAA resolve runs
// with its history blend off: bit 0 of SRT dword 42. Its other work stays: exposure (the input
// colour times a scalar from a buffer) and the stencil mask in the output's alpha.
constexpr u64 kTaaResolveHash = 0xa402ddac;
constexpr u32 kTaaEnableDword = 42;
// sotc: the tonemap applies a screen space vignette: 1 + strength * (smoothstep(t) - 1),
// t = (scale * |uv - 0.5| - start) / (end - start), from flatbuf dwords 51..54. DLSS would drag
// it along with the scene (dark nested screen borders), so it is applied after upscaling.
constexpr u64 kTonemapHash = 0x291dbcd0;
constexpr u32 kVignetteDword = 51;

bool Rasterizer::TaaOff() {
    static const int forced = [] { // SOTC_TAA_OFF=1/0 forces; unset follows DLSS
        const char* v = std::getenv("SOTC_TAA_OFF");
        return v && *v ? (v[0] == '1' ? 1 : 0) : -1;
    }();
    // TAA comes back if super resolution is switched or turns itself off
    return forced < 0 ? Dlssg::SuperResolution() : forced == 1;
}

namespace {
// sotc: SOTC_WRITE_EXTENT / SOTC_FPS counter: the size of the direct dispatch whose resources are
// being bound (BindBuffers reads it). groups == 0: not a direct dispatch (graphics, indirect),
// nothing is narrowed there.
struct DirectDispatchScope {
    u64 hash{}, groups{}, threads{};
};
thread_local DirectDispatchScope t_direct_dispatch;
struct DirectDispatchGuard {
    ~DirectDispatchGuard() {
        t_direct_dispatch = {};
    }
};
} // namespace

void Rasterizer::DispatchDirect(const Pipeline* resolved, bool use_resolved) {
    RENDERER_TRACE;
    MdiBreak(); // sotc: SOTC_MDI_STATS
    const SotcOpPhases::OpScope op_scope{SotcOpPhases::OpDispatch}; // sotc: SOTC_OP_PHASES

    scheduler.PopPendingOperations();

    const auto& cs_program = liverpool->DrawCsRegs();
    const ComputePipeline* pipeline = use_resolved ? static_cast<const ComputePipeline*>(resolved)
                                                   : [&] { // sotc: SOTC_OP_PHASES
                                                         const SotcOpPhases::Scope ph{SotcOpPhases::PhPipeline};
                                                         return pipeline_cache.GetComputePipeline();
                                                     }();
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
    bool hle_done;
    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhHle}; // sotc: SOTC_OP_PHASES
        hle_done = ExecuteShaderHLE(cs, liverpool->DrawRegs(), cs_program, *this);
    }
    if (hle_done) {
        SotcStats::Drop(5, cs_program.Address<uintptr_t>(), 0);
        return;
    }

    // sotc: dispatch size for BindBuffers (SOTC_WRITE_EXTENT, "SOTC GPU writes by shader"). A
    // non-zero start offset shifts the thread ids: no extent then (groups stays 0), counter only.
    const DirectDispatchGuard dispatch_guard;
    // sotc: the dispatch extent only feeds the write-extent narrowing and the write marks (kWriteMarks).
    if (SotcWriteExtent::kEnabled || SotcStats::kWriteMarks) {
        const u64 groups = u64{cs_program.dim_x} * cs_program.dim_y * cs_program.dim_z;
        const u64 local = u64{std::max<u32>(cs_program.num_thread_x.full, 1)} *
                          std::max<u32>(cs_program.num_thread_y.full, 1) *
                          std::max<u32>(cs_program.num_thread_z.full, 1);
        const bool plain = cs_program.start_x == 0 && cs_program.start_y == 0 &&
                           cs_program.start_z == 0 && groups != 0;
        t_direct_dispatch = {cs.pgm_hash, plain ? groups : 0, groups * local};
    }
    // sotc: SOTC_CS_STATS (measurement only)
    u64 cs_key = 0;
    bool cs_key_ok = false, cs_writes = false, cs_res_ok = false;
    u64 cs_res_key = 0;
    u64 cs_gen = 0, cs_tick = 0;
    std::chrono::steady_clock::time_point cs_t0{};
    if (kCsStats) {
        cs_key_ok = CsStatsKey(pipeline, cs_key, cs_writes);
        cs_res_ok = BindResourceHash(pipeline, cs_res_key);
        cs_tick = scheduler.CurrentTick();
        const u64 gens[4] = {BufferBindGeneration(), texture_cache.BindGeneration(),
                             buffer_cache.StreamGeneration(),
                             SotcStats::mem_epoch.load(std::memory_order_relaxed)};
        cs_gen = XXH3_64bits(gens, sizeof(gens));
        cs_t0 = std::chrono::steady_clock::now();
    }
    const bool cs_bound = BindResources(pipeline);
    if (kCsStats) {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - cs_t0)
                            .count();
        u64 cs_desc = 0, cs_handle = 0;
        if (cs_bound) {
            CsDescKeys(pipeline, buffer_infos, image_infos, cs_desc, cs_handle);
        }
        CsStatsNote(cs_key_ok, cs_key, cs_res_ok, cs_res_key, cs_writes, cs_gen, cs_tick,
                    static_cast<u64>(ns), cs_bound, cs_desc, cs_handle);
    }
    if (!cs_bound) {
        SotcStats::Drop(3, cs_program.Address<uintptr_t>(), 0);
        return;
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    scheduler.EndRendering();
    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhDescriptorPush}; // sotc: SOTC_OP_PHASES
        pipeline->BindResources(set_writes, set_write_sets, push_data, bindless_slots);
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    pipeline->BindWhenReady(); // sotc: may still be built by a worker (SOTC_ASYNC_COMPUTE)
    cmdbuf.dispatch(cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
    DebugState.IncDispatch();
    MaybeDumpImages(cs.pgm_hash);

    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhReset}; // sotc: SOTC_OP_PHASES
        ResetBindings(true);
        FlushPeriodic();
    }
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size, const Pipeline* resolved,
                                  bool use_resolved) {
    RENDERER_TRACE;
    const SotcOpPhases::OpScope op_scope{SotcOpPhases::OpDispatchIndirect}; // sotc: SOTC_OP_PHASES
    MdiBreak(); // sotc: SOTC_MDI_STATS
    if (SotcStats::kCmdStats) { // sotc: read only by the SOTC_CMD_STATS printer (sotc_stats.h)
        SotcStats::cmd_kinds[5].fetch_add(1, std::memory_order_relaxed);
    }

    scheduler.PopPendingOperations();

    const auto& cs_program = liverpool->DrawCsRegs();
    const ComputePipeline* pipeline = use_resolved ? static_cast<const ComputePipeline*>(resolved)
                                                   : [&] { // sotc: SOTC_OP_PHASES
                                                         const SotcOpPhases::Scope ph{SotcOpPhases::PhPipeline};
                                                         return pipeline_cache.GetComputePipeline();
                                                     }();
    if (!pipeline) {
        SotcStats::Drop(2, cs_program.Address<uintptr_t>(), 0);
        return;
    }

    // sotc: SOTC_CS_STATS (measurement only)
    u64 cs_key = 0;
    bool cs_key_ok = false, cs_writes = false, cs_res_ok = false;
    u64 cs_res_key = 0;
    u64 cs_gen = 0, cs_tick = 0;
    std::chrono::steady_clock::time_point cs_t0{};
    if (kCsStats) {
        cs_key_ok = CsStatsKey(pipeline, cs_key, cs_writes);
        cs_res_ok = BindResourceHash(pipeline, cs_res_key);
        cs_tick = scheduler.CurrentTick();
        const u64 gens[4] = {BufferBindGeneration(), texture_cache.BindGeneration(),
                             buffer_cache.StreamGeneration(),
                             SotcStats::mem_epoch.load(std::memory_order_relaxed)};
        cs_gen = XXH3_64bits(gens, sizeof(gens));
        cs_t0 = std::chrono::steady_clock::now();
    }
    const bool cs_bound = BindResources(pipeline);
    if (kCsStats) {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - cs_t0)
                            .count();
        u64 cs_desc = 0, cs_handle = 0;
        if (cs_bound) {
            CsDescKeys(pipeline, buffer_infos, image_infos, cs_desc, cs_handle);
        }
        CsStatsNote(cs_key_ok, cs_key, cs_res_ok, cs_res_key, cs_writes, cs_gen, cs_tick,
                    static_cast<u64>(ns), cs_bound, cs_desc, cs_handle);
    }
    if (!cs_bound) {
        SotcStats::Drop(3, cs_program.Address<uintptr_t>(), 1);
        return;
    }

    const auto [buffer, base] = buffer_cache.ObtainBuffer(address + offset, size, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, size);

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    scheduler.EndRendering();
    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhDescriptorPush}; // sotc: SOTC_OP_PHASES
        pipeline->BindResources(set_writes, set_write_sets, push_data, bindless_slots);
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    pipeline->BindWhenReady(); // sotc: may still be built by a worker (SOTC_ASYNC_COMPUTE)
    cmdbuf.dispatchIndirect(buffer->Handle(), base);
    DebugState.IncDispatch();
    runtime.AccessBuffer(buffer, base, size, vk::PipelineStageFlagBits2::eDrawIndirect,
                         vk::AccessFlagBits2::eIndirectCommandRead);
    MaybeDumpImages(pipeline->GetStage(Shader::SwStage::Compute).pgm_hash);

    {
        const SotcOpPhases::Scope ph{SotcOpPhases::PhReset}; // sotc: SOTC_OP_PHASES
        ResetBindings(true);
        FlushPeriodic();
    }
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
    SotcGuestMemory::Ready();
    SotcLodFeedbackCycle();
    buffer_cache.TickFrame();
    texture_cache.ProcessDownloadImages();
    texture_cache.RunGarbageCollector();
    runtime.TickFrame();
}

void Rasterizer::OnFence() {
    ++VideoCore::g_sotc_fence_epoch;
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
    return !attachment_feedback_loop && FastBindStatic(pipeline, liverpool->DrawRegs());
}

bool Rasterizer::FastBindStatic(const Pipeline* pipeline, const AmdGpu::Regs& regs) const {
    // sotc: FastBindEligible without its draw-thread input (attachment_feedback_loop): the
    // registers and the pipeline only, so SOTC_PARALLEL_RECORD's prep threads can run it.
    if (!SotcStats::kFastBind || pipeline->IsCompute()) {
        return false;
    }
    if (regs.clipper_control.user_clip_plane_enable != 0) {
        return false;
    }
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        // sotc: the bindless slots are set in the push data of the slow path only.
        if (IsBindlessStage(instance, *stage)) {
            return false;
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
    return FastBindHash(pipeline, push_data, liverpool->DrawRegs());
}

u64 Rasterizer::FastBindHash(const Pipeline* pipeline, const Shader::PushData& push_data,
                             const AmdGpu::Regs& regs) const {
    // sotc: the user data and flat buffers are this thread's (Shader::g_info_slot).
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

namespace {
/// sotc: SOTC_BIND_CACHE_FRAME. A stage's identity for the bind cache key: its shader and the
/// descriptor layout (kinds and order) a cached entry's writes name.
u64 BindCacheStageIdentity(const Shader::Info& stage) {
    const u64 words[3] = {stage.pgm_hash,
                          (u64{stage.buffers.size()} << 40) | (u64{stage.images.size()} << 20) |
                              u64{stage.samplers.size()},
                          static_cast<u64>(stage.sw_stage)};
    u64 ident = XXH3_64bits(words, sizeof(words));
    u64 layout = 0;
    for (const auto& b : stage.buffers) {
        layout = layout * 0x100000001B3ULL + ((u64{static_cast<u8>(b.buffer_type)} << 2) |
                                              (u64{b.is_written} << 1) | u64{b.is_formatted});
    }
    for (const auto& i : stage.images) {
        layout = layout * 0x100000001B3ULL +
                 (u64{i.is_depth} | (u64{i.is_array} << 1) | (u64{i.is_written} << 2) |
                  (u64{i.array_size} << 3) | (u64{i.is_r128} << 20) |
                  (u64{static_cast<u16>(i.mip_fallback_mode)} << 24));
    }
    for (const auto& smp : stage.samplers) {
        layout = layout * 0x100000001B3ULL + u64{smp.is_depth} + 7;
    }
    return XXH3_64bits_withSeed(&layout, sizeof(layout), ident);
}
} // namespace

bool Rasterizer::BindResourceHash(const Pipeline* pipeline, u64& out) const {
    // sotc: SOTC_BIND_CACHE_FRAME. What the descriptors of a bind are made of: the pipeline, the
    // T#/V#/S# words each stage reads (GetSharp: the flat buffer words located by the stage's
    // sharp fetches, with the post operations, exactly what BindBuffers / BindTextures use) and
    // the stage identity. The rest of the flat buffer (user data registers, SRT pointers and
    // values: constants the shader reads from its stream-ring copy, which a hit re-copies) and
    // the viewport words of the push data are not in the key. A pointer the shader dereferences
    // by itself (DMA), a table or an array it indexes at run time are resources chosen by
    // constants: those pipelines return false and keep the full key.
    u64 hash = 0x4C32525300000001ULL;
    hash = XXH3_64bits_withSeed(&pipeline, sizeof(pipeline), hash);
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        if (stage->uses_dma || !stage->sharp_tables.empty() || !stage->fmasks.empty() ||
            stage->has_storage_images || stage->UserData().data() == nullptr) {
            return false;
        }
        for (const auto& image : stage->images) {
            if (image.array_size != 0) {
                return false;
            }
        }
        const u64 ident = BindCacheStageIdentity(*stage);
        hash = XXH3_64bits_withSeed(&ident, sizeof(ident), hash);
        for (const auto& desc : stage->buffers) {
            if (!desc.IsSpecial()) {
                const auto sharp = desc.GetSharp(*stage);
                hash = XXH3_64bits_withSeed(&sharp, sizeof(sharp), hash);
            }
        }
        for (const auto& image : stage->images) {
            const auto sharp = image.GetSharp(*stage);
            hash = XXH3_64bits_withSeed(&sharp, sizeof(sharp), hash);
        }
        for (const auto& sampler : stage->samplers) {
            const auto sharp = sampler.GetSharp(*stage);
            hash = XXH3_64bits_withSeed(&sharp, sizeof(sharp), hash);
        }
    }
    out = hash;
    return true;
}

void Rasterizer::CommitFastBind(const Pipeline* pipeline, u64 hash, bool uses_dma) {
    fast_bind.pipeline = pipeline;
    fast_bind.hash = hash;
    fast_bind.buffer_generation = BufferBindGeneration(); // sotc: BindVersion with BIND_CACHE_XTICK
    fast_bind.texture_generation = texture_cache.BindGeneration();
    fast_bind.stream_generation = buffer_cache.StreamGeneration();
    fast_bind.mem_epoch = SotcStats::mem_epoch.load(std::memory_order_relaxed);
    fast_bind.tick = scheduler.CurrentTick();
    fast_bind.uses_dma = uses_dma;
    fast_bind.valid = true;
}

// sotc: SOTC_PR_RESOLVE (default off; acts only with SOTC_PARALLEL_RECORD, since the prepared
// data is produced on the GpuPrep threads). Resource inputs of a graphics draw, resolved ahead of
// the draw thread: see PreparedResources (draw_prep.h).
static bool SotcPrResolveOn() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PR_RESOLVE");
        return v && v[0] == '1';
    }();
    return on;
}

// sotc: SOTC_PR_RESOLVE. FlatForBind rewrites the flat buffer of these stages (TAA resolve, tonemap
// vignette, vegetation scales, grass patch LOD), so their V#s and T#s are read inline. Keep in sync
// with FlatForBind; SOTC_PR_VERIFY recomputes the prepared values and catches a drift.
static bool FlatMayBePatched(const Shader::Info& s) {
    return s.pgm_hash == kTaaResolveHash || s.pgm_hash == kTonemapHash ||
           s.pgm_hash == kVegetation[0].hash || s.pgm_hash == kVegetation[1].hash ||
           s.pgm_hash == kGrassPatchHash;
}

// sotc: SOTC_PR_RESOLVE. Stages the prepared path leaves to the draw thread: DMA pointers, sharp
// tables, fmasks, storage images, descriptor arrays, and stages FlatForBind may rewrite.
static bool PrStageResolvable(const Shader::Info& s) {
    if (s.uses_dma || !s.sharp_tables.empty() || !s.fmasks.empty() || s.has_storage_images ||
        FlatMayBePatched(s)) {
        return false;
    }
    for (const auto& image : s.images) {
        if (image.array_size != 0) {
            return false;
        }
    }
    return true;
}

// sotc: SOTC_PR_RESOLVE. The values BindBuffers computes for a guest buffer binding (its clamp to
// the mapping and its unbounded-window limit). The window warning stays with the inline path.
static PrBuffer PrResolveBuffer(const Shader::Info& stage, const Shader::BufferResource& desc,
                                Core::MemoryManager* mem) {
    PrBuffer out{};
    if (desc.IsSpecial()) {
        return out;
    }
    const auto vsharp = desc.GetSharp(stage);
    out.base = vsharp.base_address;
    out.vsize = vsharp.GetSize();
    u64 size = 0;
    if (out.base != 0 && out.vsize != 0) {
        size = mem->ClampRangeSize(out.base, out.vsize);
    }
    out.mapped = size;
    // The same limit as BindBuffers (SHADPS4_UNBOUNDED_BUFFER_MB, 64 MB by default).
    static const u64 unbounded_window = [] {
        const char* v = std::getenv("SHADPS4_UNBOUNDED_BUFFER_MB");
        return (v ? std::strtoull(v, nullptr, 10) : 64ULL) << 20;
    }();
    constexpr u64 MaxSaneBindingSize = 256ULL << 20;
    if (!desc.is_written && (out.vsize >= 0xFFFF0000ULL || size > MaxSaneBindingSize) &&
        unbounded_window != 0 && size > unbounded_window) {
        size = unbounded_window;
    }
    out.size = size;
    out.computed = true;
    return out;
}

// sotc: SOTC_PR_RESOLVE. The T# and the validity verdict BindTextures makes on it (non-array).
static PrImage PrResolveImage(const Shader::Info& stage, const Shader::ImageResource& image,
                              Core::MemoryManager* mem) {
    PrImage out{};
    const u32 num_bindings = image.NumBindings(stage);
    if (num_bindings > 0xFF) {
        return out; // verdict 0: BindTextures resolves it inline
    }
    out.tsharp = image.GetSharp(stage);
    out.num_bindings = static_cast<u8>(num_bindings);
    const auto data_fmt = out.tsharp.GetDataFmt();
    const auto num_fmt = out.tsharp.GetNumberFmt();
    if (out.tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid) {
        out.verdict = 2;
    } else if (!mem->IsValidGpuMapping(out.tsharp.Address(), 0) ||
               !magic_enum::enum_contains(data_fmt) || !magic_enum::enum_contains(num_fmt)) {
        out.verdict = 3;
    } else {
        out.verdict = 1;
    }
    return out;
}

// sotc: SOTC_PR_RESOLVE. Equality of the resolved values (SOTC_PR_VERIFY); runtime_gen is not
// compared.
static bool SamePreparedResources(const PreparedResources& a, const PreparedResources& b) {
    if (a.num_buffers != b.num_buffers || a.num_images != b.num_images) {
        return false;
    }
    for (size_t s = 0; s < a.stages.size(); ++s) {
        if (a.stages[s].ready != b.stages[s].ready ||
            a.stages[s].buf_first != b.stages[s].buf_first ||
            a.stages[s].img_first != b.stages[s].img_first) {
            return false;
        }
    }
    for (u32 i = 0; i < a.num_buffers; ++i) {
        const auto& x = a.buffers[i];
        const auto& y = b.buffers[i];
        if (x.base != y.base || x.vsize != y.vsize || x.mapped != y.mapped || x.size != y.size ||
            x.computed != y.computed) {
            return false;
        }
    }
    for (u32 i = 0; i < a.num_images; ++i) {
        const auto& x = a.images[i];
        const auto& y = b.images[i];
        if (std::memcmp(&x.tsharp, &y.tsharp, sizeof(x.tsharp)) != 0 ||
            x.num_bindings != y.num_bindings || x.verdict != y.verdict) {
            return false;
        }
    }
    return true;
}

// sotc: SOTC_BIND_DIFF=1 (off by default; a measurement). For every fast-bind-eligible draw that
// misses the fast bind, which of its conditions failed, as a bit mask: the 6-8 most frequent masks
// are printed every 5 s. "epoch" alone means nothing the draw binds changed, only the global
// mem_epoch counter that any write anywhere bumps (memory_tracker.h, NoteHostWrite).
namespace {
const bool kBindDiff = [] {
    const char* v = std::getenv("SOTC_BIND_DIFF");
    return v && v[0] == '1';
}();
enum BindDiffBit : u32 {
    BdInvalid = 1, BdPipeline = 2, BdBufGen = 4, BdTexGen = 8, BdStreamGen = 16, BdEpoch = 32,
    BdHash = 64, BdTick = 128,
};
struct BindDiffState {
    u64 counts[256]{};
    u64 eligible = 0, hits = 0;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
BindDiffState g_bind_diff; // the draw thread only
void BindDiffNote(u32 mask, bool hit) {
    auto& d = g_bind_diff;
    ++d.eligible;
    if (hit) {
        ++d.hits;
    } else {
        ++d.counts[mask & 255];
    }
    if ((d.eligible & 1023) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - d.last < std::chrono::seconds(5)) {
        return;
    }
    d.last = now;
    u64 misses = 0;
    for (u64 c : d.counts) {
        misses += c;
    }
    std::string line = "SOTC BIND DIFF: eligible " + std::to_string(d.eligible) + ", hits " +
                       std::to_string(d.hits) + ", misses " + std::to_string(misses) + ":";
    for (int k = 0; k < 8; ++k) {
        u32 best = 0;
        for (u32 m = 1; m < 256; ++m) {
            if (d.counts[m] > d.counts[best]) {
                best = m;
            }
        }
        if (d.counts[best] == 0) {
            break;
        }
        std::string name;
        static constexpr const char* kNames[8] = {"invalid", "pipeline", "bufgen", "texgen",
                                                  "streamgen", "epoch", "hash", "tick"};
        for (int bit = 0; bit < 8; ++bit) {
            if (best & (1u << bit)) {
                name += (name.empty() ? "" : "+");
                name += kNames[bit];
            }
        }
        char item[96];
        std::snprintf(item, sizeof(item), " %s %.1f%%", name.c_str(),
                      misses ? 100.0 * double(d.counts[best]) / double(misses) : 0.0);
        line += item;
        d.counts[best] = 0;
    }
    std::fprintf(stderr, "%s\n", line.c_str());
    const auto last = d.last;
    d = BindDiffState{};
    d.last = last;
}
} // namespace

bool Rasterizer::BindResources(const Pipeline* pipeline, const PreparedDraw* prepared) {
    // sotc: SOTC_SYNC_PIPE counters: this call is one op (its Protect calls are counted).
    const SotcStats::SyncPipe::OpScope sync_pipe_op;
    const SotcOpPhases::Scope ph{SotcOpPhases::PhBindResources}; // sotc: SOTC_OP_PHASES
    // sotc: writes recorded during this bind follow its command into the tick it lands in.
    buffer_cache.BeginWriteScope();
    struct WriteScopeGuard {
        VideoCore::BufferCache& cache;
        ~WriteScopeGuard() {
            cache.EndWriteScope();
        }
    } write_scope_guard{buffer_cache};
    const SotcStats::SplitTimer bind_split{SotcStats::SplitBind}; // sotc: SOTC_DRAW_SPLIT
    fast_bind_hit = false;
    // sotc: SOTC_BIND_CACHE: flush the cached binds when the state they were made in moved, or
    // when this bind is not a read-only graphics bind (its writes and copies run before them).
    // sotc: SOTC_PARALLEL_RECORD: `prepared` holds the register-only inputs of this bind
    // (eligibility, push data, FastBindHash), computed ahead by a prep thread.
    const bool cache_eligible =
        SotcStats::kBindCache &&
        (prepared ? prepared->cache_eligible : BindCacheEligible(pipeline));
    if (SotcStats::kBindCache) {
        if (SotcStats::kBindCacheFrame) {
            BindCacheSyncFrame(pipeline, cache_eligible);
        } else {
            BindCacheSync(pipeline, cache_eligible);
        }
    }
    // sotc: SOTC_BIND_CACHE_FRAME, or SOTC_BUFFER_MEMO alone (the memo without the frame cache)
    bind_memo_ok = (SotcStats::kBindCacheFrame || SotcStats::kBufferMemo) && cache_eligible;
    compute_written_image = {};
    if (IsComputeImageCopy(pipeline) || IsComputeMetaClear(pipeline) ||
        IsComputeImageClear(pipeline)) {
        // sotc: SOTC_BIND_CACHE_RANGES: these replace the dispatch by an image copy / clear or a
        // metadata state change, with no write recorded by range: every cached bind dies.
        // sotc: SOTC_BIND_CACHE_FRAME: an image copy or clear is a GPU write to one image, which
        // is what BindCacheInvalidateImage drops the readers of (as Resolve and DepthStencilCopy
        // do); the metadata state change has no range and still flushes everything.
        if (SotcStats::kBindCacheFrame && compute_written_image) {
            BindCacheInvalidateImage(compute_written_image);
            if (SotcStats::kFpsLog) {
                SotcStats::bf_narrowed.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (SotcStats::kBindCacheRanges) {
            BindCacheFlush(SotcStats::BindCacheUnsafe);
        }
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
    const bool fast_eligible = prepared ? prepared->fast_static && !attachment_feedback_loop
                                        : FastBindEligible(pipeline);
    u64 fast_hash = 0;
    if (fast_eligible) {
        SotcStats::fast_bind_eligible.fetch_add(1, std::memory_order_relaxed);
        const auto input_push = prepared ? prepared->push_data : MakeUserData(liverpool->DrawRegs());
        fast_hash = prepared ? prepared->fast_hash : FastBindHash(pipeline, input_push);
        // Same state as the last full bind, ignoring the submit tick (SOTC_FASTBIND_WIDE needs it).
        const bool fast_state_same =
            fast_bind.valid && fast_bind.pipeline == pipeline &&
            fast_bind.buffer_generation == BufferBindGeneration() &&
            fast_bind.texture_generation == texture_cache.BindGeneration() &&
            fast_bind.stream_generation == buffer_cache.StreamGeneration() &&
            fast_bind.mem_epoch == SotcStats::mem_epoch.load(std::memory_order_relaxed) &&
            fast_bind.hash == fast_hash;
        if (kBindDiff) { // sotc: SOTC_BIND_DIFF: which conditions failed
            u32 mask = 0;
            if (!fast_bind.valid) {
                mask = BdInvalid;
            } else {
                mask |= fast_bind.pipeline != pipeline ? BdPipeline : 0u;
                mask |= fast_bind.buffer_generation != BufferBindGeneration() ? BdBufGen : 0u;
                mask |= fast_bind.texture_generation != texture_cache.BindGeneration() ? BdTexGen
                                                                                        : 0u;
                mask |= fast_bind.stream_generation != buffer_cache.StreamGeneration() ? BdStreamGen
                                                                                       : 0u;
                mask |= fast_bind.mem_epoch != SotcStats::mem_epoch.load(std::memory_order_relaxed)
                            ? BdEpoch
                            : 0u;
                mask |= fast_bind.hash != fast_hash ? BdHash : 0u;
                mask |= fast_bind.tick != scheduler.CurrentTick() ? BdTick : 0u;
            }
            BindDiffNote(mask, mask == 0);
        }
        if (fast_state_same && fast_bind.tick == scheduler.CurrentTick()) {
            fast_bind_hit = true;
            SotcStats::fast_bind_hits.fetch_add(1, std::memory_order_relaxed);
            if (fast_bind.uses_dma) {
                const SotcStats::SplitTimer dma_split{SotcStats::SplitBindDma};
                buffer_cache.SynchronizeDmaBuffers();
            }
            return true;
        }
        if (fast_state_same && SotcStats::kFastBindWide) {
            // sotc: SOTC_FASTBIND_WIDE: the same state in a later submit. The new command buffer
            // holds none of the push constants or descriptors, so the caller still runs
            // Pipeline::BindResources (fast_bind_hit stays false) with the descriptor arrays kept
            // from the last full bind. fast_bind.valid guarantees no other BindResources call
            // replaced them since (the others clear it), and the generations above guarantee the
            // handles and residency they name are still current.
            push_data = input_push;
            fast_bind.tick = scheduler.CurrentTick();
            if (SotcStats::kFpsLog) {
                SotcStats::fast_bind_wide_hits.fetch_add(1, std::memory_order_relaxed);
            }
            if (fast_bind.uses_dma) {
                buffer_cache.SynchronizeDmaBuffers();
            }
            return true;
        }
        if (SotcStats::kFpsLog) {
            SotcStats::fast_bind_misses.fetch_add(1, std::memory_order_relaxed);
            if (fast_state_same) {
                SotcStats::fast_bind_tick_misses.fetch_add(1, std::memory_order_relaxed);
            }
        }
        push_data = input_push;
    } else {
        fast_bind.valid = false;
        push_data = prepared ? prepared->push_data : MakeUserData(liverpool->DrawRegs());
    }

    // sotc: SOTC_BIND_CACHE: the same state as an earlier full bind of this submit.
    size_t first_bound_buffer = 0;
    u64 cache_hash = 0;
    if (cache_eligible) {
        // The border colour table of custom-border samplers (GetSampler) is a register too.
        const u64 bc_base = liverpool->DrawRegs().ta_bc_base.Address();
        bind_level2 = false;
        if (SotcStats::kBindCacheFrame) {
            // sotc: SOTC_BIND_CACHE_FRAME: the key is the pipeline and the resources it binds
            // (BindResourceHash); the constants around them are refreshed by the hit. A pipeline
            // that binds by constants keeps the full key below.
            u64 res_hash = 0;
            const bool res_ok = prepared ? prepared->res_ok : BindResourceHash(pipeline, res_hash);
            if (prepared) {
                res_hash = prepared->res_hash;
            }
            if (res_ok) {
                cache_hash = XXH3_64bits_withSeed(&bc_base, sizeof(bc_base), res_hash);
                bind_level2 = true;
            }
        }
        if (!bind_level2) {
            cache_hash = fast_eligible ? fast_hash
                         : prepared    ? prepared->fast_hash
                                       : FastBindHash(pipeline, push_data);
            cache_hash = XXH3_64bits_withSeed(&bc_base, sizeof(bc_base), cache_hash);
            if (SotcStats::kBindCacheFrame) {
                // sotc: entries now outlive many frames: a pipeline erased and another made at the
                // same address must not take its entries, so the stages' identity is in the key.
                u64 ident = 0;
                for (const auto* stage : pipeline->GetStages()) {
                    if (stage) {
                        const u64 id = BindCacheStageIdentity(*stage);
                        ident = XXH3_64bits_withSeed(&id, sizeof(id), ident);
                    }
                }
                cache_hash = XXH3_64bits_withSeed(&ident, sizeof(ident), cache_hash);
            }
        }
        bind_input_push = push_data; // the draw's own viewport words (refreshed on a hit)
        if (bool hit_dma = false; SotcStats::kBindCacheFrame
                                      ? TryBindCacheFrame(pipeline, cache_hash, hit_dma)
                                      : TryBindCache(pipeline, cache_hash, hit_dma)) {
            // The arrays now hold this state: the next identical draw may take the fast bind.
            if (fast_eligible) {
                CommitFastBind(pipeline, fast_hash, hit_dma);
            }
            return true;
        }
        bind_cache_recording = true;
        bind_cache_uncacheable = false;
        bind_cache_images_rec.clear();
        bind_cache_lod_rec.clear();
        bind_cache_ranges_rec.clear();
        bind_cache_repairs_rec.clear();  // sotc: SOTC_BIND_CACHE_XTICK
        bind_cache_samplers_rec.clear(); // sotc: SOTC_BIND_CACHE_XTICK
        if (SotcStats::kBindCacheFrame) { // sotc: what the entry depends on, from this point on
            bind_cache_lookups_rec.clear();
            bind_cache_texel_rec.clear();
            bind_cache_stream_rec = false;
            bind_cache_map_dep_rec = false;
            bind_rec_tex_seq = texture_cache.ImageChangeSeq();
            bind_rec_write_seq = write_stamps.Seq();
            bind_rec_map_epoch = MapEpoch();
            bind_rec_gpu_epoch = VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed);
            bind_cache_tail_rec = false;
            bind_cache_flat_rec.clear();
        }
        first_bound_buffer = bound_buffers.size();
    }

    set_write_index = 0;
    // sotc: the heap indices of this draw; each bindless stage fills its own words in BindTextures.
    if (instance.IsBindlessSamplersEnabled()) {
        bindless_slots = {};
    }
    set_writes.clear();
    buffer_infos.clear();
    image_infos.clear();

    // Bind resource buffers and textures.
    Shader::Backend::Bindings binding{};
    const bool staged = pipeline->UsesStageSets();
    u32 stage_counter = ~0u;
    // sotc: SOTC_PR_RESOLVE: the prepared resource inputs, when they were computed under the table
    // generation this bind sees. Null: every stage resolves inline, as before.
    const PreparedResources* resolved = nullptr;
    if (prepared && SotcPrResolveOn() && !pr_verify_off) {
        const u64 gen = runtime_memory_generation();
        if (!(gen & 1) && prepared->resolved.runtime_gen == gen) {
            resolved = &prepared->resolved;
        }
    }
    for (const auto* stage : pipeline->GetStages()) {
        ++stage_counter;
        if (!stage) {
            continue;
        }
        cur_bind_stage = stage_counter; // sotc: SOTC_BIND_CACHE_FRAME (flat buffer slots)
        // sotc: SOTC_STAGE_BINDINGS: each stage numbers its descriptors from 0 in its own set. The
        // buffer count runs on across the stages: it indexes the push constant offsets they share.
        const u32 set = staged ? Shader::StageDescriptorSet(stage->sw_stage) : 0u;
        if (staged) {
            binding.unified = 0;
        }
        // sotc: bindless samplers have no descriptor write (BindTextures sets their push slots).
        const size_t num_sampler_writes =
            IsBindlessStage(instance, *stage) ? 0 : stage->samplers.size();
        set_writes.resize(set_writes.size() + stage->buffers.size() + stage->images.size() +
                          num_sampler_writes);
        set_write_sets.resize(set_writes.size(), 0u);
        // sotc: SOTC_PR_RESOLVE: this stage's prepared entries (stage_counter is its position in
        // GetStages(), as the prep thread numbered it). FlatForBind stages stay inline.
        const PrStage* pr_stage = resolved && resolved->stages[stage_counter].ready &&
                                          !FlatMayBePatched(*stage)
                                      ? &resolved->stages[stage_counter]
                                      : nullptr;
        {
            const SotcTrace::BindScope trace_step{"bind buffers"}; // sotc: a = bytes, b = protects
            const SotcStats::SplitTimer buffers_split{SotcStats::SplitBindBuffers};
            BindBuffers(*stage, binding, push_data, set, pr_stage ? resolved : nullptr, pr_stage);
        }
        {
            const SotcTrace::ScopeMin trace_step{"bind textures", stage->images.size()}; // sotc
            const SotcOpPhases::Scope ph{SotcOpPhases::PhBindTextures}; // sotc: SOTC_OP_PHASES
            const SotcStats::SplitTimer textures_split{SotcStats::SplitBindTextures};
            BindTextures(*stage, binding, set, pr_stage ? resolved : nullptr, pr_stage);
        }
        uses_dma |= stage->uses_dma;
    }

    if (uses_dma) {
        const SotcStats::SplitTimer dma_split{SotcStats::SplitBindDma};
        buffer_cache.SynchronizeDmaBuffers();
    }

    if (fast_eligible) {
        CommitFastBind(pipeline, fast_hash, uses_dma);
    }
    if (bind_cache_recording) {
        bind_cache_recording = false;
        if (SotcStats::kBindCacheFrame) {
            CommitBindCacheFrame(pipeline, cache_hash, uses_dma, first_bound_buffer);
        } else {
            CommitBindCache(pipeline, cache_hash, uses_dma, first_bound_buffer);
        }
    }

    return true;
}

bool Rasterizer::BindCacheEligible(const Pipeline* pipeline) const {
    return BindCacheEligible(pipeline, liverpool->DrawRegs());
}

bool Rasterizer::BindCacheEligible(const Pipeline* pipeline, const AmdGpu::Regs& regs) const {
    // sotc: SOTC_BIND_CACHE: graphics binds whose resources are all read-only (as FastBindEligible,
    // bindless sampler stages included: their heap slots are kept in the entry). A bind that
    // writes buffers or images, compute included, is not eligible and flushes the cache.
    if (pipeline->IsCompute() || regs.clipper_control.user_clip_plane_enable != 0) {
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
            if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                return false;
            }
            if (desc.is_written && (!desc.IsSpecial() ||
                                    desc.buffer_type == Shader::BufferType::GdsBuffer)) {
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

void Rasterizer::BindCacheFlush(u32 reason) {
    if (bind_cache_live == 0) {
        return;
    }
    if (SotcStats::kBindCacheFrame) {
        // sotc: SOTC_BIND_CACHE_FRAME: older entries are dropped when a lookup finds them.
        ++bind_cache_flush_gen;
        if (SotcStats::kFpsLog) {
            SotcStats::bind_cache_flushes[reason].fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    for (auto& entry : bind_cache) {
        entry.valid = false;
    }
    bind_cache_live = 0;
    if (SotcStats::kFpsLog) {
        SotcStats::bind_cache_flushes[reason].fetch_add(1, std::memory_order_relaxed);
    }
}

void Rasterizer::BindCacheSync(const Pipeline* pipeline, bool eligible) {
    BindCacheState now{
        .tick = scheduler.CurrentTick(),
        .buffer_generation = BufferBindGeneration(),
        .texture_generation = texture_cache.BindGeneration(),
        .stream_generation = buffer_cache.StreamGeneration(),
        .mem_epoch = SotcStats::mem_epoch.load(std::memory_order_relaxed),
        .write_epoch = bind_cache_write_epoch.load(std::memory_order_relaxed),
    };
    if (!pipeline->IsCompute()) {
        // The targets PrepareRenderState found for this draw (cb_descs past the MRT count are
        // stale, so they are left out).
        const auto& key = static_cast<const GraphicsPipeline*>(pipeline)->GetGraphicsKey();
        const u32 num_cb = std::min<u32>(std::bit_width(key.mrt_mask), AmdGpu::NUM_COLOR_BUFFERS);
        for (u32 cb = 0; cb < num_cb; ++cb) {
            now.targets[cb] = cb_descs[cb].first;
        }
        now.targets[AmdGpu::NUM_COLOR_BUFFERS] = db_desc.first;
    } else if (SotcStats::kBindCacheRanges) {
        // A dispatch renders into no target: the draws after it still use the targets the
        // entries were made with when the draws before it did.
        now.targets = bind_cache_state.targets;
    }
    const auto& old = bind_cache_state;
    if (SotcStats::kBindCacheRanges) {
        // sotc: SOTC_BIND_CACHE_RANGES. The tick, resource generation and render target rules
        // stay. Writes no longer flush everything: each one drops the entries reading what it
        // writes (BindCacheInvalidate), and guest memory changes are left to the checks of each
        // hit. Why that covers every writer (TryBindCache refuses a CPU-modified buffer range,
        // and a Dirty, untracked, unregistered or re-bound image):
        //  - guest CPU writes to tracked pages fault: Rasterizer::InvalidateMemory(OnGuestFault)
        //    -> MemoryTracker::InvalidateRegion sets the CPU bit (directly, or after the readback
        //    of a GPU-written page in MarkWrittenByGuest; a page still GPU-pending stays protected
        //    and the write faults again), and TextureCache::InvalidateMemory marks the images
        //    CpuDirty / MaybeCpuDirty or untracks them. Pages left writable (fault grace, hot
        //    pages, write-ahead) keep their CPU bit, as the comment in TryBindCache says.
        //  - command processor writes done on the host (WRITE_DATA, DUMP_CONST_RAM, the CPU paths
        //    of FillBuffer / CopyBuffer, fence and label writes) are CPU writes: they fault the
        //    same way on a protected page, or call InvalidateMemory (labels, video decode).
        //  - unmaps (DropGpuModified, fast or not) mark the range CPU-modified; freed images
        //    move the texture generation (resources flush).
        //  - shader HLE: the replicate fill writes the backing (TryReplaceFromCpu marks it
        //    CPU-modified; TryWriteThroughFromCpu also copies it into the arena) and the copy
        //    shader writes the arena: both call BindCacheNoteWrite (vk_shader_hle.cpp).
        //  - readbacks write guest memory with what the GPU already holds: nothing to drop.
        //  - GPU writes: written buffers and storage images of non-eligible binds (BindBuffers,
        //    BindTextures) and their render targets (below), GPU fills and copies (FillBuffer,
        //    CopyBuffer; GDS is not guest memory and its barrier is replayed by a hit), Resolve,
        //    DepthStencilCopy and EliminateFastClear call BindCacheInvalidate; the compute
        //    image copy / clear / metadata clear paths flush (BindResources).
        // Inputs a hit cannot revalidate are not stored at all (BindBuffers): texel buffers at an
        // image's address (ObtainBuffer re-tiles a GPU-written image into them on every bind)
        // and stream-buffer copies (a snapshot of guest memory, SOTC_STREAM_THRESHOLD).
        // sotc: SOTC_BIND_CACHE_XTICK: the tick is not a flush reason. What it protected is
        // replayed by the first hit of each entry in a new tick (TryBindCache): the image and
        // sampler stamps and the block stamps; descriptors, push data, transitions and buffer
        // access checks are re-issued by every hit anyway.
        if (now.tick != old.tick && !SotcStats::kBindCacheXtick) {
            BindCacheFlush(SotcStats::BindCacheTick);
        } else if (now.buffer_generation != old.buffer_generation ||
                   now.texture_generation != old.texture_generation ||
                   now.stream_generation != old.stream_generation) {
            BindCacheFlush(SotcStats::BindCacheResources);
        } else if (now.targets != old.targets) {
            BindCacheFlush(SotcStats::BindCacheTargets);
        }
        bind_cache_state = now;
        // A draw that writes buffers or images is not eligible: its render targets are written
        // along with the rest (BindBuffers / BindTextures drop the entries for those).
        if (!eligible && !pipeline->IsCompute() && bind_cache_live != 0) {
            const auto& key = static_cast<const GraphicsPipeline*>(pipeline)->GetGraphicsKey();
            const u32 num_cb =
                std::min<u32>(std::bit_width(key.mrt_mask), AmdGpu::NUM_COLOR_BUFFERS);
            for (u32 cb = 0; cb < num_cb; ++cb) {
                if (const auto id = cb_descs[cb].first) {
                    BindCacheInvalidateImage(id);
                }
            }
            if (const auto id = db_desc.first) {
                BindCacheInvalidateImage(id);
            }
        }
        return;
    }
    if (!eligible || now.write_epoch != old.write_epoch) {
        BindCacheFlush(SotcStats::BindCacheUnsafe);
    } else if (now.tick != old.tick) {
        BindCacheFlush(SotcStats::BindCacheTick);
    } else if (now.mem_epoch != old.mem_epoch) {
        BindCacheFlush(SotcStats::BindCacheMemory);
    } else if (now.buffer_generation != old.buffer_generation ||
               now.texture_generation != old.texture_generation ||
               now.stream_generation != old.stream_generation) {
        BindCacheFlush(SotcStats::BindCacheResources);
    } else if (now.targets != old.targets) {
        BindCacheFlush(SotcStats::BindCacheTargets);
    }
    bind_cache_state = now;
}

void Rasterizer::BindCacheNoteImage(VideoCore::ImageId image_id, vk::ImageLayout layout,
                                    vk::PipelineStageFlags2 stages, vk::AccessFlags2 access,
                                    const VideoCore::SubresourceRange& range) {
    // The depth target of this draw sampled: its layout depends on registers outside the key
    // (stencil enable), so the bind is not stored.
    if (image_id == db_desc.first) {
        bind_cache_uncacheable = true;
        return;
    }
    // Called under BindTextures' batch lock: the image info is stable here.
    const auto& info = texture_cache.GetImage(image_id).info;
    const VAddr guest_lo = info.guest_address - info.sotc_dropped_bytes; // a mip tail: its chain
    const u64 uid = texture_cache.GetImage(image_id).image_uid;
    bind_cache_images_rec.push_back({image_id, layout, stages, access, range, guest_lo,
                                     info.guest_address + info.guest_size, uid});
}

void Rasterizer::BindCacheInvalidate(VAddr addr, u64 size, VideoCore::ImageId image) {
    if (SotcStats::kBindCacheFrame) { // sotc: SOTC_BIND_CACHE_FRAME
        BindCacheInvalidateFrame(addr, size, image);
        return;
    }
    // sotc: SOTC_BIND_CACHE_RANGES. 32 entries: a linear scan, most rejected by the bounds.
    if (bind_cache_live == 0) {
        return;
    }
    const VAddr end = addr + size;
    const auto overlaps = [&](VAddr lo, VAddr hi) { return size != 0 && lo < end && addr < hi; };
    u32 dropped = 0;
    for (auto& entry : bind_cache) {
        if (!entry.valid) {
            continue;
        }
        bool hit = false;
        if (image) {
            for (const auto& img : entry.images) {
                if (img.id == image) {
                    hit = true;
                    break;
                }
            }
        }
        if (!hit && overlaps(entry.lo, entry.hi)) {
            for (const auto& [lo, len] : entry.ranges) {
                if (overlaps(lo, lo + len)) {
                    hit = true;
                    break;
                }
            }
            for (size_t i = 0; !hit && i < entry.images.size(); ++i) {
                hit = overlaps(entry.images[i].guest_lo, entry.images[i].guest_hi);
            }
        }
        if (hit) {
            entry.valid = false;
            --bind_cache_live;
            ++dropped;
        }
    }
    if (SotcStats::kFpsLog) {
        SotcStats::bind_cache_range_writes.fetch_add(1, std::memory_order_relaxed);
        if (dropped) {
            SotcStats::bind_cache_range_entries.fetch_add(dropped, std::memory_order_relaxed);
        }
    }
}

void Rasterizer::BindCacheInvalidateImage(VideoCore::ImageId image_id) {
    // sotc: SOTC_BIND_CACHE_FRAME notes the write even with an empty cache: a bind in flight stores
    // its entry afterwards and compares it with the writes noted since it began.
    if (!SotcStats::kBindCacheRanges || (bind_cache_live == 0 && !SotcStats::kBindCacheFrame) ||
        !image_id) {
        return;
    }
    const auto& info = texture_cache.GetImage(image_id).info;
    const VAddr lo = info.guest_address - info.sotc_dropped_bytes;
    BindCacheInvalidate(lo, info.guest_address + info.guest_size - lo, image_id);
}

void Rasterizer::BindCacheNoteWrite(VAddr addr, u64 size) {
    if (SotcStats::kBindCacheRanges) {
        BindCacheInvalidate(addr, size);
    }
}

bool Rasterizer::TryBindCache(const Pipeline* pipeline, u64 hash, bool& uses_dma) {
    // sotc: SOTC_BIND_CACHE. BindCacheSync has just flushed every entry made in another tick,
    // with other guest memory, other resident buffers / images / samplers / stream ring, other
    // render targets, or before a GPU write; a live entry is the result of a full bind of this
    // very state (same pipeline and FastBindHash inputs) in this command buffer, after which
    // only read-only draws into these same targets ran. With SOTC_BIND_CACHE_RANGES, other
    // binds and DMA may have run too, none of them writing what the entry reads
    // (BindCacheInvalidate), and guest memory may have changed: the checks below catch that.
    if (SotcStats::kFpsLog) {
        SotcStats::bind_cache_lookups.fetch_add(1, std::memory_order_relaxed);
        if (SotcStats::kBindCacheXtick) {
            // sotc: SOTC_BIND_CACHE_XTICK: was this key looked up in the previous game frame too?
            const u64 frame = SotcStats::cp_frame.load(std::memory_order_relaxed);
            auto& seen = bind_seen[(hash ^ (hash >> 29)) % bind_seen.size()];
            bool prev_frame = false;
            if (seen.hash == hash) {
                prev_frame = seen.frame_last == frame ? seen.frame_prev + 1 == frame
                                                      : seen.frame_last + 1 == frame;
                if (seen.frame_last != frame) {
                    seen.frame_prev = seen.frame_last;
                    seen.frame_last = frame;
                }
            } else {
                seen = {hash, frame, 0};
            }
            if (prev_frame) {
                SotcStats::bind_cache_seen_prev_frame.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    BindCacheEntry* entry = nullptr;
    for (auto& e : bind_cache) {
        if (e.valid && e.pipeline == pipeline && e.hash == hash) {
            entry = &e;
            break;
        }
    }
    if (!entry) {
        return false;
    }
    const auto refuse = [] {
        if (SotcStats::kFpsLog) {
            SotcStats::bind_cache_refused.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    };
    const bool xtick = SotcStats::kBindCacheXtick;
    u64 current_tick = scheduler.CurrentTick();
    // sotc: SOTC_BIND_CACHE_XTICK. What a full bind does once per submit tick (or per use epoch)
    // that a hit would skip, and how it is replayed here, for an entry made in an earlier tick:
    //  - TextureCache::FindImage: tick_prev_accessed / tick_accessed_last stamps and the LRU
    //    touch of each sampled image (the image GC and the host image copy idle test read them):
    //    TextureCache::NoteBoundUse below, once per entry and tick.
    //  - TextureCache::GetSampler: the sampler LRU touch (an untouched sampler is collected,
    //    which moves the texture generation and flushes the cache): GetSampler again, checked
    //    to return the same handle and bindless slot.
    //  - BufferCache::EnsureResident: block_last_use stamps (MaybeEvictCold's idle test): one
    //    stamping per use epoch (seconds), BufferCache::NoteBindUse.
    //  - memory->ClampRangeSize of each V#: the mapped extent the binding was made for (a guest
    //    unmap or a map next to it); checked once per tick and before every repair.
    // Not needed: descriptor and push data (re-issued by every hit), image layout transitions
    // and buffer access barriers (replayed by every hit), SotcLod marks (every hit), the stream
    // buffer (its wrap generation is a flush reason), the resident set (BindVersion is a flush
    // reason: removals and arena replacement; additions move nothing a binding holds).
    bool replay = xtick && entry->stamp_tick != current_tick;
    bool mapping_checked = false;
    const auto mapping_changed = [&] {
        mapping_checked = true;
        for (const auto& r : entry->repairs) {
            if (memory->ClampRangeSize(r.addr, r.vsize) != r.mapped) {
                return true;
            }
        }
        return false;
    };
    if (replay && mapping_changed()) {
        entry->valid = false;
        --bind_cache_live;
        return refuse();
    }
    if (xtick && buffer_cache.BindVersion() != bind_cache_state.buffer_generation) {
        BindCacheFlush(SotcStats::BindCacheResources); // an unmap / eviction since BindCacheSync
        return false;
    }
    // ObtainBuffer would upload CPU-modified pages. Pages kept writable after a fault (fault
    // grace, SOTC_HOT_PAGES) take guest writes without moving mem_epoch: checked per range.
    if (!xtick) {
        for (const auto& [addr, size] : entry->ranges) {
            if (buffer_cache.IsRegionCpuModified(addr, size)) {
                return refuse();
            }
        }
    } else {
        // SOTC_BIND_CACHE_XTICK: repair instead of refusing. A CPU-modified range is
        // synchronized as ObtainBuffer does (the same arena and arguments: the cached handle is
        // still the binding, BindVersion), if the guest mapping under it is still the one the
        // entry was made for; the descriptors stay valid. Images and the rest still refuse.
        u32 repaired = 0;
        for (const auto& r : entry->repairs) {
            if (!r.arena || !buffer_cache.IsRegionCpuModified(r.addr, r.size)) {
                continue;
            }
            if (memory->ClampRangeSize(r.addr, r.vsize) != r.mapped) {
                return refuse();
            }
            buffer_cache.SynchronizeBinding(r.arena, r.addr, r.size, r.texel);
            ++repaired;
        }
        if (repaired != 0) {
            // The upload may have submitted, evicted or wrapped the stream ring: the entry would
            // be stale. Nothing of this hit has been replayed yet, the full bind follows.
            if (buffer_cache.BindVersion() != bind_cache_state.buffer_generation ||
                buffer_cache.StreamGeneration() != bind_cache_state.stream_generation) {
                BindCacheFlush(SotcStats::BindCacheResources);
                return false;
            }
            // sotc: the synchronization may have dropped this very entry (a BindCacheInvalidate
            // for a range it reads, reached through the upload); a dropped entry is not replayed.
            if (!entry->valid) {
                return false;
            }
            if (SotcStats::kFpsLog) {
                SotcStats::bind_cache_xtick_repaired.fetch_add(1, std::memory_order_relaxed);
            }
            // The synchronization may have submitted: the stamps are for the tick being recorded.
            current_tick = scheduler.CurrentTick();
            replay = entry->stamp_tick != current_tick;
            if (replay && !mapping_checked && mapping_changed()) {
                entry->valid = false;
                --bind_cache_live;
                return refuse();
            }
        }
    }
    if (xtick && replay) {
        for (const auto& s : entry->samplers) {
            u32 slot = 0;
            const vk::Sampler handle = texture_cache.GetSampler(
                s.sharp, liverpool->DrawRegs().ta_bc_base, s.is_depth, &slot); // as BindTextures
            if (handle != s.handle || slot != s.slot) {
                entry->valid = false;
                --bind_cache_live;
                return refuse();
            }
        }
    }
    {
        const auto texture_lock = texture_cache.BatchLock();
        // The guest may unmap (unregister) images from its own threads: checked under the lock.
        if (texture_cache.BindGeneration() != bind_cache_state.texture_generation) {
            BindCacheFlush(SotcStats::BindCacheResources);
            return false;
        }
        // What the full bind would do differently: an image to upload again or re-track
        // (TextureCache::UpdateImage), one replaced since (needs_rebind), or one that became a
        // target or storage image of this draw (general / feedback loop layouts).
        for (const auto& img : entry->images) {
            const auto& image = texture_cache.GetImage(img.id);
            const bool ok =
                True(image.flags & VideoCore::ImageFlagBits::Registered) &&
                False(image.flags & VideoCore::ImageFlagBits::Dirty) &&
                image.track_addr == image.info.guest_address &&
                image.track_addr_end == image.info.guest_address + image.info.guest_size &&
                !image.binding.needs_rebind && !image.binding.force_general &&
                !image.binding.is_target && img.id != db_desc.first;
            if (!ok) {
                return refuse();
            }
        }
        // The side effects of BindTextures' second pass: bound for this draw (BeginRendering's
        // feedback test, ResetBindings), and in the layout the descriptors name, with a barrier
        // when another command changed it since.
        for (const auto& img : entry->images) {
            auto& image = texture_cache.GetImage(img.id);
            image.binding.is_bound = 1u;
            bound_images.emplace_back(img.id);
            needs_barrier |= runtime.Transit(&image, img.layout, img.stages, img.access, img.range);
            image.usage.texture = 1u;
            if (replay) {
                texture_cache.NoteBoundUse(img.id); // sotc: SOTC_BIND_CACHE_XTICK, see above
            }
        }
    }
    if (xtick) {
        entry->stamp_tick = current_tick;
        if (const u16 epoch = buffer_cache.UseEpoch(); entry->stamp_epoch != epoch) {
            for (const auto& r : entry->repairs) {
                if (r.arena) {
                    buffer_cache.NoteBindUse(r.addr, r.size);
                }
            }
            entry->stamp_epoch = epoch;
        }
        if (SotcStats::kFpsLog) {
            if (replay) {
                SotcStats::bind_cache_xtick_replays.fetch_add(1, std::memory_order_relaxed);
            }
            if (entry->made_tick != current_tick) {
                SotcStats::bind_cache_xtick_hits.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    // The buffer side of BindBuffers: barrier against earlier writes, and the reads recorded
    // after the draw (ResetBindings).
    for (const auto& buf : entry->buffers) {
        needs_barrier |= runtime.IsBufferAccessed(buf.buffer, buf.offset, buf.size, buf.is_written);
        bound_buffers.emplace_back(buf);
    }
    for (const u32 bank : entry->lod_banks) {
        SotcLod::Mark(bank);
    }

    buffer_infos.assign(entry->buffer_infos.begin(), entry->buffer_infos.end());
    image_infos.assign(entry->image_infos.begin(), entry->image_infos.end());
    std::copy(entry->image_info_images.begin(), entry->image_info_images.end(),
              image_info_images.begin());
    set_writes.assign(entry->writes.begin(), entry->writes.end());
    set_write_sets.assign(entry->write_sets.begin(), entry->write_sets.end());
    for (size_t i = 0; i < set_writes.size(); ++i) {
        const u32 info = entry->write_info[i];
        auto& write = set_writes[i];
        write.dstSet = VK_NULL_HANDLE;
        if (info & 0x80000000u) {
            write.pBufferInfo = buffer_infos.data() + (info & 0x7FFFFFFFu);
        } else {
            write.pImageInfo = image_infos.data() + info;
        }
    }
    set_write_index = static_cast<u32>(set_writes.size());
    push_data = entry->push_data;
    bindless_slots = entry->bindless_slots;
    entry->last_use = ++bind_cache_clock;

    uses_dma = entry->uses_dma;
    if (uses_dma) {
        buffer_cache.SynchronizeDmaBuffers();
    }
    if (SotcStats::kFpsLog) {
        SotcStats::bind_cache_hits.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void Rasterizer::CommitBindCache(const Pipeline* pipeline, u64 hash, bool uses_dma,
                                 size_t first_bound_buffer) {
    // sotc: SOTC_BIND_CACHE. Not stored when the bind did something a hit would not repeat, or
    // when the state moved while it ran (a submit, a guest write, a new image...): the next
    // BindCacheSync flushes then.
    const auto& state = bind_cache_state;
    const auto vo_target = [&] {
        const auto id = state.targets[0];
        return id && texture_cache.GetImage(id).usage.vo_surface;
    };
    // SOTC_BIND_CACHE_RANGES: a guest write while this bind ran (another thread's fault) leaves
    // its pages CPU-modified or its images dirty, which the next hit refuses: mem_epoch is not
    // needed here either.
    if (bind_cache_uncacheable || vo_target() || scheduler.CurrentTick() != state.tick ||
        (!SotcStats::kBindCacheRanges &&
         SotcStats::mem_epoch.load(std::memory_order_relaxed) != state.mem_epoch) ||
        bind_cache_write_epoch.load(std::memory_order_relaxed) != state.write_epoch ||
        BufferBindGeneration() != state.buffer_generation ||
        texture_cache.BindGeneration() != state.texture_generation ||
        buffer_cache.StreamGeneration() != state.stream_generation ||
        set_writes.size() != set_write_index) {
        if (SotcStats::kFpsLog) {
            SotcStats::bind_cache_uncacheable.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    BindCacheEntry* entry = nullptr;
    for (auto& e : bind_cache) {
        if (e.valid && e.pipeline == pipeline && e.hash == hash) {
            entry = &e; // a refused match: replaced
            break;
        }
    }
    if (!entry) {
        entry = &bind_cache[0];
        for (auto& e : bind_cache) {
            if (!e.valid) {
                entry = &e;
                break;
            }
            if (e.last_use < entry->last_use) {
                entry = &e;
            }
        }
        if (!entry->valid) {
            ++bind_cache_live;
        }
    }
    entry->pipeline = pipeline;
    entry->hash = hash;
    entry->last_use = ++bind_cache_clock;
    entry->uses_dma = uses_dma;
    entry->push_data = push_data;
    entry->bindless_slots = bindless_slots;
    entry->writes.assign(set_writes.begin(), set_writes.end());
    entry->write_sets.assign(set_write_sets.begin(), set_write_sets.end());
    entry->write_info.resize(set_writes.size());
    for (size_t i = 0; i < set_writes.size(); ++i) {
        const auto& write = set_writes[i];
        if (write.descriptorType == vk::DescriptorType::eStorageBuffer) {
            entry->write_info[i] =
                0x80000000u | static_cast<u32>(write.pBufferInfo - buffer_infos.data());
        } else {
            entry->write_info[i] = static_cast<u32>(write.pImageInfo - image_infos.data());
        }
    }
    entry->buffer_infos.assign(buffer_infos.begin(), buffer_infos.end());
    entry->image_infos.assign(image_infos.begin(), image_infos.end());
    entry->image_info_images.assign(image_info_images.begin(),
                                    image_info_images.begin() + image_infos.size());
    entry->buffers.assign(bound_buffers.begin() + first_bound_buffer, bound_buffers.end());
    entry->images.assign(bind_cache_images_rec.begin(), bind_cache_images_rec.end());
    entry->lod_banks.assign(bind_cache_lod_rec.begin(), bind_cache_lod_rec.end());
    entry->ranges.assign(bind_cache_ranges_rec.begin(), bind_cache_ranges_rec.end());
    if (SotcStats::kBindCacheXtick) {
        entry->repairs.assign(bind_cache_repairs_rec.begin(), bind_cache_repairs_rec.end());
        entry->samplers.assign(bind_cache_samplers_rec.begin(), bind_cache_samplers_rec.end());
        entry->made_tick = state.tick;
        entry->stamp_tick = state.tick; // the full bind just made this tick's stamps
        entry->stamp_epoch = 0;         // block stamps due at the first hit
    }
    // SOTC_BIND_CACHE_RANGES: the bounds of everything the entry reads (BindCacheInvalidate).
    VAddr lo = ~VAddr{0}, hi = 0;
    for (const auto& [addr, size] : entry->ranges) {
        lo = std::min(lo, addr);
        hi = std::max(hi, addr + size);
    }
    for (const auto& img : entry->images) {
        lo = std::min(lo, img.guest_lo);
        hi = std::max(hi, img.guest_hi);
    }
    entry->lo = lo;
    entry->hi = hi;
    entry->valid = true;
}

// ---------------------------------------------------------------------------------------------
// sotc: SOTC_BIND_CACHE_FRAME. The bind cache that survives the frame.
//
// What changes against the SOTC_BIND_CACHE_XTICK cache above, and why each old flush reason is
// still covered:
//  - Table: 4096 entries, open addressing (FrameProbe slots), keyed by pipeline, FastBindHash
//    (plus the pipeline's stage identity) and the render targets.
//  - Render targets (and their image identities) are part of the key instead of a flush reason.
//    What the targets guarded stays per hit: a sampled image that became a target of this draw
//    (binding.is_target) or is its depth target refuses the hit.
//  - The texture generation (every RegisterImage / UnregisterImage) no longer wipes the cache.
//    FindImage's answer for a T# depends on the registered images overlapping the range it looked
//    up, so the entry keeps those ranges (T# ranges, the found images', texel buffers') and asks
//    TextureCache::ImageChangedSince whether an image was (un)registered over any of them since
//    it last looked. An unrelated registration invalidates nothing. A freed image's slot can be
//    reused: the unregistration is over the image's own range, which the entry watches, and the
//    image uid is compared at the hit as well. The sampler half of the generation (a destroyed
//    sampler leaves stale handles) stays a cache-wide flush.
//  - The buffer generation (BindVersion: unbind, eviction, arena replacement) is compared per
//    entry; an entry made before the change is dropped when found, one made after survives. (A
//    per-range version would drop fewer: see the report.)
//  - The stream ring wrap count is compared for entries that hold a flat buffer copy there.
//  - GPU writes (BindCacheInvalidate) are range stamps checked by the entries that read the
//    range or sample the image, as the scan of the old table did, at the next hit.
//  - Not stored when any of it moved while the bind ran (the old code: texture generation).
//  - A cache-wide flush (BindCacheFlush) is a generation counter.
// Stamps are compared at the hit only when the global sequence number moved since the entry last
// looked, so a hit between two events costs what it did.
// ---------------------------------------------------------------------------------------------

namespace {
u64 BindCacheFrameKey(const Pipeline* pipeline, u64 hash, u64 targets_key) {
    u64 key = hash ^ (reinterpret_cast<uintptr_t>(pipeline) * 0x9E3779B97F4A7C15ULL) ^
              (targets_key * 0xC2B2AE3D27D4EB4FULL);
    key ^= key >> 31;
    return key | 1; // 0 marks an empty slot
}
} // namespace

void Rasterizer::BindCacheSyncFrame(const Pipeline* pipeline, bool eligible) {
    if (frame_cache.empty()) {
        frame_cache.resize(BindCacheFrameEntries);
        frame_keys.assign(BindCacheFrameEntries, 0);
    }
    BindCacheState now{
        .tick = scheduler.CurrentTick(),
        .buffer_generation = buffer_cache.BindVersion(),
        .texture_generation = texture_cache.SamplerGeneration(),
        .stream_generation = buffer_cache.StreamGeneration(),
        .mem_epoch = SotcStats::mem_epoch.load(std::memory_order_relaxed),
        .write_epoch = bind_cache_write_epoch.load(std::memory_order_relaxed),
    };
    if (!pipeline->IsCompute()) {
        // The targets PrepareRenderState found for this draw (cb_descs past the MRT count are
        // stale, so they are left out). Their identities (image uid) are in the key: a slot
        // reused by another image is another target.
        const auto& key = static_cast<const GraphicsPipeline*>(pipeline)->GetGraphicsKey();
        const u32 num_cb = std::min<u32>(std::bit_width(key.mrt_mask), AmdGpu::NUM_COLOR_BUFFERS);
        std::array<u64, AmdGpu::NUM_COLOR_BUFFERS + 1> identity{};
        for (u32 cb = 0; cb < num_cb; ++cb) {
            now.targets[cb] = cb_descs[cb].first;
            if (now.targets[cb]) {
                identity[cb] = texture_cache.GetImage(now.targets[cb]).image_uid;
            }
        }
        now.targets[AmdGpu::NUM_COLOR_BUFFERS] = db_desc.first;
        if (db_desc.first) {
            identity[AmdGpu::NUM_COLOR_BUFFERS] = texture_cache.GetImage(db_desc.first).image_uid;
        }
        now.targets_key = XXH3_64bits(identity.data(), identity.size() * sizeof(u64));
    } else {
        // A dispatch renders into no target.
        now.targets = bind_cache_state.targets;
        now.targets_key = bind_cache_state.targets_key;
    }
    const bool samplers_moved = now.texture_generation != bind_cache_state.texture_generation;
    bind_cache_state = now;
    if (samplers_moved) {
        BindCacheFlush(SotcStats::BindCacheResources);
    }
    // A draw that writes buffers or images is not eligible: its render targets are written along
    // with the rest (BindBuffers / BindTextures note the writes of those).
    if (!eligible && !pipeline->IsCompute()) {
        const auto& key = static_cast<const GraphicsPipeline*>(pipeline)->GetGraphicsKey();
        const u32 num_cb = std::min<u32>(std::bit_width(key.mrt_mask), AmdGpu::NUM_COLOR_BUFFERS);
        for (u32 cb = 0; cb < num_cb; ++cb) {
            if (const auto id = cb_descs[cb].first) {
                BindCacheInvalidateImage(id);
            }
        }
        if (const auto id = db_desc.first) {
            BindCacheInvalidateImage(id);
        }
    }
}

void Rasterizer::BindCacheInvalidateFrame(VAddr addr, u64 size, VideoCore::ImageId image) {
    // Only the draw thread notes writes (the scan this replaces was not thread safe either).
    write_stamps.Note(addr, addr + std::max<u64>(size, 1),
                      image ? &write_image_stamps[image.index % WriteImageStamps] : nullptr);
    if (SotcStats::kFpsLog) {
        SotcStats::bind_cache_range_writes.fetch_add(1, std::memory_order_relaxed);
    }
}

namespace {
/// Whether the GPU writes and image set changes noted after the checkpoints leave a bind that
/// reads `ranges`, samples `images` and looked up `tex_watch` alone. The checkpoints move up only
/// when it does.
template <typename Ranges, typename Images>
bool BindCacheStampsOk(const VideoCore::RangeStamps& writes,
                       const std::array<std::atomic<u64>, 4096>& image_stamps,
                       const VideoCore::TextureCache& textures, const Ranges& ranges,
                       const Images& images, const std::vector<std::pair<VAddr, VAddr>>& tex_watch,
                       u64& checked_write, u64& checked_tex, u32& reason) {
    // The checkpoint is the sequence number read before the stamps: an event published while
    // looking is newer than it and is found by the next check.
    const u64 write_seq = writes.Seq();
    if (write_seq != checked_write) {
        for (const auto& [addr, size] : ranges) {
            if (writes.ChangedSince(addr, addr + size, checked_write)) {
                reason = SotcStats::BfWrites;
                return false;
            }
        }
        for (const auto& img : images) {
            if (image_stamps[img.id.index % 4096].load(std::memory_order_relaxed) >
                    checked_write ||
                writes.ChangedSince(img.guest_lo, img.guest_hi, checked_write)) {
                reason = SotcStats::BfWrites;
                return false;
            }
        }
    }
    const u64 tex_seq = textures.ImageChangeSeq();
    if (tex_seq != checked_tex) {
        for (const auto& [lo, hi] : tex_watch) {
            if (textures.ImageChangedSince(lo, hi, checked_tex)) {
                reason = SotcStats::BfImages;
                return false;
            }
        }
    }
    checked_write = write_seq;
    checked_tex = tex_seq;
    return true;
}
} // namespace

bool Rasterizer::BindCacheStampsFresh(BindCacheEntry& entry, u32& reason) {
    if (entry.map_dependent && entry.map_epoch != MapEpoch()) {
        reason = SotcStats::BfMapping;
        return false;
    }
    if (entry.tail_dependent &&
        entry.gpu_epoch != VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed)) {
        reason = SotcStats::BfImages;
        return false;
    }
    return BindCacheStampsOk(write_stamps, write_image_stamps, texture_cache, entry.ranges,
                             entry.images, entry.tex_watch, entry.checked_write_seq,
                             entry.checked_tex_seq, reason);
}

bool Rasterizer::BindCacheEntryFresh(BindCacheEntry& entry, u32& reason) {
    if (entry.flush_gen != bind_cache_flush_gen) {
        reason = SotcStats::BfFlush;
        return false;
    }
    if (entry.buffer_version != buffer_cache.BindVersion()) {
        reason = SotcStats::BfBuffers;
        return false;
    }
    if (entry.uses_stream && entry.stream_gen != buffer_cache.StreamGeneration()) {
        reason = SotcStats::BfStream;
        return false;
    }
    return BindCacheStampsFresh(entry, reason);
}

void Rasterizer::BindCacheDropFrame(BindCacheEntry& entry, u32 reason) {
    const size_t index = static_cast<size_t>(&entry - frame_cache.data());
    if (index < frame_keys.size() && frame_keys[index] != 0) {
        frame_keys[index] = 0;
        --bind_cache_live;
        if (SotcStats::kFpsLog) {
            SotcStats::bf_live.store(bind_cache_live, std::memory_order_relaxed);
            SotcStats::bf_drop[reason].fetch_add(1, std::memory_order_relaxed);
        }
    }
}

Rasterizer::BindCacheEntry* Rasterizer::BindCacheFindFrame(u64 key, const Pipeline* pipeline,
                                                           u64 hash, u64 targets_key) {
    const size_t base = static_cast<size_t>((key * 0x9E3779B97F4A7C15ULL) >> 52); // 12 bits
    for (u32 probe = 0; probe < BindCacheFrameProbe; ++probe) {
        const size_t index = (base + probe) & (BindCacheFrameEntries - 1);
        if (frame_keys[index] != key) {
            continue;
        }
        auto& entry = frame_cache[index];
        if (entry.pipeline == pipeline && entry.hash == hash && entry.targets_key == targets_key) {
            return &entry;
        }
    }
    return nullptr;
}

void Rasterizer::BindCacheNoteLookup(const VideoCore::TextureCache::ImageDesc& desc,
                                     VideoCore::ImageId image) {
    // sotc: SOTC_BIND_CACHE_FRAME. What FindImage looked at: the T#'s range (with its mip tail
    // alternative), and the image it settled on.
    const auto add = [&](VAddr lo, VAddr hi) {
        if (!bind_cache_lookups_rec.empty() && bind_cache_lookups_rec.back() == std::pair{lo, hi}) {
            return;
        }
        bind_cache_lookups_rec.emplace_back(lo, hi);
    };
    add(desc.info.guest_address - desc.info.sotc_dropped_bytes,
        desc.info.guest_address + desc.info.guest_size);
    if (desc.other_info) {
        add(desc.other_info->guest_address - desc.other_info->sotc_dropped_bytes,
            desc.other_info->guest_address + desc.other_info->guest_size);
    }
    if (image) {
        const auto& info = texture_cache.GetImage(image).info;
        add(info.guest_address - info.sotc_dropped_bytes, info.guest_address + info.guest_size);
        bind_cache_tail_rec |= info.sotc_first_mip != 0;
    }
}

bool Rasterizer::TryBindCacheFrame(const Pipeline* pipeline, u64 hash, bool& uses_dma) {
    // sotc: SOTC_BIND_CACHE_FRAME, see the comment above BindCacheSyncFrame. The rest follows
    // TryBindCache (SOTC_BIND_CACHE_XTICK): what a full bind does that a hit would skip is
    // replayed the same way.
    const bool stats = SotcStats::kFpsLog;
    if (stats) {
        SotcStats::bind_cache_lookups.fetch_add(1, std::memory_order_relaxed);
        const u64 frame = SotcStats::cp_frame.load(std::memory_order_relaxed);
        auto& seen = bind_seen[(hash ^ (hash >> 29)) % bind_seen.size()];
        bool prev_frame = false;
        if (seen.hash == hash) {
            prev_frame = seen.frame_last == frame ? seen.frame_prev + 1 == frame
                                                  : seen.frame_last + 1 == frame;
            if (seen.frame_last != frame) {
                seen.frame_prev = seen.frame_last;
                seen.frame_last = frame;
            }
        } else {
            seen = {hash, frame, 0};
        }
        if (prev_frame) {
            SotcStats::bind_cache_seen_prev_frame.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const u64 targets_key = bind_cache_state.targets_key;
    const u64 lookup_key = BindCacheFrameKey(pipeline, hash, targets_key);
    if (stats) {
        // distinct keys looked up per game frame (does the table hold a frame's worth?)
        const u64 frame = SotcStats::cp_frame.load(std::memory_order_relaxed);
        if (frame != bind_key_frame) {
            bind_key_frame = frame;
            SotcStats::bf_frames.fetch_add(1, std::memory_order_relaxed);
        }
        auto& seen_key = bind_key_seen[(lookup_key >> 20) % bind_key_seen.size()];
        if (seen_key.key != lookup_key || seen_key.frame != frame) {
            seen_key = {lookup_key, frame};
            SotcStats::bf_distinct_keys.fetch_add(1, std::memory_order_relaxed);
        }
    }
    BindCacheEntry* entry = BindCacheFindFrame(lookup_key, pipeline, hash, targets_key);
    const auto miss = [&](u32 reason) {
        if (stats) {
            SotcStats::bf_miss[reason].fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    };
    // The entry is found and no longer valid: dropped, the full bind makes a new one.
    const auto stale = [&](u32 reason) {
        BindCacheDropFrame(*entry, reason);
        return miss(reason);
    };
    const auto refuse = [&](bool drop) {
        if (stats) {
            SotcStats::bind_cache_refused.fetch_add(1, std::memory_order_relaxed);
        }
        if (drop) {
            BindCacheDropFrame(*entry, SotcStats::BfRefused);
        }
        return miss(SotcStats::BfRefused);
    };
    if (!entry) {
        return miss(SotcStats::BfNoEntry);
    }
    if (u32 reason; !BindCacheEntryFresh(*entry, reason)) {
        return stale(reason);
    }
    // sotc: an entry made under the resource identity key: this draw's constants. The flat
    // buffer is copied into the stream ring again (patched as BindBuffers would), unless it is
    // the very data the entry copied last and the ring has not wrapped since; the copy's offset
    // goes into the descriptor below. The viewport words of the push data are set below too.
    if (!entry->flat_slots.empty()) {
        const auto stages = pipeline->GetStages();
        const u64 alignment = instance.StorageMinAlignment();
        auto& vk_buffer = buffer_cache.GetStreamBuffer();
        for (auto& slot : entry->flat_slots) {
            const Shader::Info* st = slot.stage < stages.size() ? stages[slot.stage] : nullptr;
            if (!st || st->FlatBuf().size() * sizeof(u32) != slot.size) {
                return refuse(true);
            }
            const u32* flat = FlatForBind(*st);
            const u64 hash = XXH3_64bits(flat, slot.size);
            // sotc: only a copy made in this tick may be reused: a later tick reusing it does not
            // extend the ring's watch over it, and a wrap would overwrite it while that tick's
            // draws still read it on the GPU (review of 2026-10-29).
            if (hash == slot.hash && slot.gen == buffer_cache.StreamGeneration() &&
                slot.tick == scheduler.CurrentTick()) {
                if (stats) {
                    SotcStats::bf_flat_reused.fetch_add(1, std::memory_order_relaxed);
                }
                continue;
            }
            slot.offset = vk_buffer.Copy(flat, slot.size, alignment);
            slot.hash = hash;
            slot.gen = buffer_cache.StreamGeneration();
            slot.tick = scheduler.CurrentTick();
            if (stats) {
                SotcStats::bf_flat_copied.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    u64 current_tick = scheduler.CurrentTick();
    bool replay = entry->stamp_tick != current_tick;
    bool mapping_checked = false;
    const auto mapping_changed = [&] {
        mapping_checked = true;
        for (const auto& r : entry->repairs) {
            if (memory->ClampRangeSize(r.addr, r.vsize) != r.mapped) {
                return true;
            }
        }
        return false;
    };
    if (replay && mapping_changed()) {
        return refuse(true);
    }
    // A CPU-modified range is synchronized as ObtainBuffer does (the same arena and arguments:
    // the cached handle is still the binding, BindVersion), if the guest mapping under it is
    // still the one the entry was made for; the descriptors stay valid.
    u32 repaired = 0;
    for (const auto& r : entry->repairs) {
        if (!r.arena || !buffer_cache.IsRegionCpuModified(r.addr, r.size)) {
            continue;
        }
        if (memory->ClampRangeSize(r.addr, r.vsize) != r.mapped) {
            return refuse(false);
        }
        buffer_cache.SynchronizeBinding(r.arena, r.addr, r.size, r.texel);
        ++repaired;
    }
    if (repaired != 0) {
        // The upload may have submitted, evicted or wrapped the stream ring, or reached a
        // BindCacheInvalidate for a range this entry reads: it must be fresh again, or the full
        // bind follows (nothing of this hit has been replayed yet).
        if (u32 reason; !BindCacheEntryFresh(*entry, reason)) {
            return stale(reason);
        }
        // sotc: the flat copies made or reused above must still be in this lap and tick of the
        // stream ring (the synchronization may have wrapped it or submitted).
        for (const auto& slot : entry->flat_slots) {
            if (slot.gen != buffer_cache.StreamGeneration() ||
                slot.tick != scheduler.CurrentTick()) {
                return refuse(true);
            }
        }
        if (stats) {
            SotcStats::bind_cache_xtick_repaired.fetch_add(1, std::memory_order_relaxed);
        }
        // The synchronization may have submitted: the stamps are for the tick being recorded.
        current_tick = scheduler.CurrentTick();
        replay = entry->stamp_tick != current_tick;
        if (replay && !mapping_checked && mapping_changed()) {
            return refuse(true);
        }
    }
    if (replay) {
        for (const auto& s : entry->samplers) {
            u32 slot = 0;
            const vk::Sampler handle = texture_cache.GetSampler(
                s.sharp, liverpool->DrawRegs().ta_bc_base, s.is_depth, &slot); // as BindTextures
            if (handle != s.handle || slot != s.slot) {
                return refuse(true);
            }
        }
    }
    {
        const auto texture_lock = texture_cache.BatchLock();
        // The guest may unmap (unregister) images from its own threads: checked under the lock,
        // before the entry's images are touched (a freed image's slot may hold another image).
        if (texture_cache.SamplerGeneration() != bind_cache_state.texture_generation) {
            BindCacheFlush(SotcStats::BindCacheResources);
            return miss(SotcStats::BfFlush);
        }
        if (u32 reason; !BindCacheStampsFresh(*entry, reason)) {
            return stale(reason);
        }
        // What the full bind would do differently: an image to upload again or re-track
        // (TextureCache::UpdateImage), one replaced since (needs_rebind), or one that became a
        // target or storage image of this draw (general / feedback loop layouts).
        for (const auto& img : entry->images) {
            const auto& image = texture_cache.GetImage(img.id);
            const bool ok =
                True(image.flags & VideoCore::ImageFlagBits::Registered) &&
                image.image_uid == img.uid &&
                False(image.flags & VideoCore::ImageFlagBits::Dirty) &&
                image.track_addr == image.info.guest_address &&
                image.track_addr_end == image.info.guest_address + image.info.guest_size &&
                !image.binding.needs_rebind && !image.binding.force_general &&
                !image.binding.is_target && img.id != db_desc.first;
            if (!ok) {
                return refuse(false);
            }
        }
        // The side effects of BindTextures' second pass: bound for this draw (BeginRendering's
        // feedback test, ResetBindings), and in the layout the descriptors name, with a barrier
        // when another command changed it since.
        for (const auto& img : entry->images) {
            auto& image = texture_cache.GetImage(img.id);
            image.binding.is_bound = 1u;
            bound_images.emplace_back(img.id);
            needs_barrier |= runtime.Transit(&image, img.layout, img.stages, img.access, img.range);
            image.usage.texture = 1u;
            if (replay) {
                texture_cache.NoteBoundUse(img.id);
            }
        }
    }
    entry->stamp_tick = current_tick;
    if (const u16 epoch = buffer_cache.UseEpoch(); entry->stamp_epoch != epoch) {
        for (const auto& r : entry->repairs) {
            if (r.arena) {
                buffer_cache.NoteBindUse(r.addr, r.size);
            }
        }
        entry->stamp_epoch = epoch;
    }
    if (stats) {
        if (replay) {
            SotcStats::bind_cache_xtick_replays.fetch_add(1, std::memory_order_relaxed);
        }
        if (entry->made_tick != current_tick) {
            SotcStats::bind_cache_xtick_hits.fetch_add(1, std::memory_order_relaxed);
        }
        if (entry->made_frame != SotcStats::cp_frame.load(std::memory_order_relaxed)) {
            SotcStats::bf_cross_frame_hits.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // The buffer side of BindBuffers: barrier against earlier writes, and the reads recorded
    // after the draw (ResetBindings).
    for (const auto& buf : entry->buffers) {
        needs_barrier |= runtime.IsBufferAccessed(buf.buffer, buf.offset, buf.size, buf.is_written);
        bound_buffers.emplace_back(buf);
    }
    for (const u32 bank : entry->lod_banks) {
        SotcLod::Mark(bank);
    }

    buffer_infos.assign(entry->buffer_infos.begin(), entry->buffer_infos.end());
    image_infos.assign(entry->image_infos.begin(), entry->image_infos.end());
    std::copy(entry->image_info_images.begin(), entry->image_info_images.end(),
              image_info_images.begin());
    set_writes.assign(entry->writes.begin(), entry->writes.end());
    set_write_sets.assign(entry->write_sets.begin(), entry->write_sets.end());
    for (size_t i = 0; i < set_writes.size(); ++i) {
        const u32 info = entry->write_info[i];
        auto& write = set_writes[i];
        write.dstSet = VK_NULL_HANDLE;
        if (info & 0x80000000u) {
            write.pBufferInfo = buffer_infos.data() + (info & 0x7FFFFFFFu);
        } else {
            write.pImageInfo = image_infos.data() + info;
        }
    }
    set_write_index = static_cast<u32>(set_writes.size());
    push_data = entry->push_data;
    // The viewport words are registers of this draw (the buffer offsets stay the entry's).
    push_data.xoffset = bind_input_push.xoffset;
    push_data.yoffset = bind_input_push.yoffset;
    push_data.xscale = bind_input_push.xscale;
    push_data.yscale = bind_input_push.yscale;
    for (const auto& slot : entry->flat_slots) {
        buffer_infos[slot.info].offset = slot.offset;
    }
    bindless_slots = entry->bindless_slots;
    entry->last_use = ++bind_cache_clock;

    uses_dma = entry->uses_dma;
    if (uses_dma) {
        buffer_cache.SynchronizeDmaBuffers();
    }
    if (stats) {
        SotcStats::bind_cache_hits.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void Rasterizer::CommitBindCacheFrame(const Pipeline* pipeline, u64 hash, bool uses_dma,
                                      size_t first_bound_buffer) {
    // sotc: SOTC_BIND_CACHE_FRAME. Not stored when the bind did something a hit would not repeat,
    // or when what the entry depends on moved while the bind ran (a submit, a new image over a
    // range it looked up, a GPU write to what it reads...).
    const auto& state = bind_cache_state;
    const auto vo_target = [&] {
        const auto id = state.targets[0];
        return id && texture_cache.GetImage(id).usage.vo_surface;
    };
    // Under the resource identity key the flat buffer copies are refreshed by every hit (their ring
    // generation is per slot), so the entry itself does not depend on the ring.
    const bool uses_stream = bind_cache_stream_rec && !bind_level2;
    if (bind_cache_uncacheable || vo_target() || scheduler.CurrentTick() != state.tick ||
        bind_cache_write_epoch.load(std::memory_order_relaxed) != state.write_epoch ||
        buffer_cache.BindVersion() != state.buffer_generation ||
        texture_cache.SamplerGeneration() != state.texture_generation ||
        (uses_stream && buffer_cache.StreamGeneration() != state.stream_generation) ||
        set_writes.size() != set_write_index) {
        if (SotcStats::kFpsLog) {
            SotcStats::bind_cache_uncacheable.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    // The ranges whose image set the entry depends on: the T# lookups, the sampled images and
    // the texel buffers (an image registered over one makes ObtainBuffer re-tile it).
    auto& tex_watch = bind_cache_tex_watch_rec;
    tex_watch.assign(bind_cache_lookups_rec.begin(), bind_cache_lookups_rec.end());
    tex_watch.insert(tex_watch.end(), bind_cache_texel_rec.begin(), bind_cache_texel_rec.end());
    for (const auto& img : bind_cache_images_rec) {
        tex_watch.emplace_back(img.guest_lo, img.guest_hi);
    }
    // Checked against the sequence numbers read when the bind began: what moved during it
    // (including what this very bind did, e.g. creating an image) is not trusted.
    u64 checked_write = bind_rec_write_seq;
    u64 checked_tex = bind_rec_tex_seq;
    if (u32 reason; (bind_cache_map_dep_rec && bind_rec_map_epoch != MapEpoch()) ||
                    (bind_cache_tail_rec &&
                     bind_rec_gpu_epoch !=
                         VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed)) ||
                    !BindCacheStampsOk(write_stamps, write_image_stamps, texture_cache,
                                       bind_cache_ranges_rec, bind_cache_images_rec, tex_watch,
                                       checked_write, checked_tex, reason)) {
        if (SotcStats::kFpsLog) {
            SotcStats::bf_not_stored_moved.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    const u64 key = BindCacheFrameKey(pipeline, hash, state.targets_key);
    const size_t base = static_cast<size_t>((key * 0x9E3779B97F4A7C15ULL) >> 52);
    size_t chosen = ~size_t{0};
    bool occupied = false;
    // A match is replaced (a refused one); else an empty slot; else a stale one (made before a
    // flush); else the least recently used of the probe window.
    u64 best_score = ~0ULL;
    for (u32 probe = 0; probe < BindCacheFrameProbe; ++probe) {
        const size_t index = (base + probe) & (BindCacheFrameEntries - 1);
        if (frame_keys[index] == 0) {
            if (best_score != 0) {
                chosen = index;
                occupied = false;
                best_score = 0;
            }
            continue;
        }
        const auto& e = frame_cache[index];
        if (frame_keys[index] == key && e.pipeline == pipeline && e.hash == hash &&
            e.targets_key == state.targets_key) {
            chosen = index;
            occupied = true;
            best_score = 0;
            break;
        }
        const u64 score = e.flush_gen != bind_cache_flush_gen ? 1 : 2 + e.last_use;
        if (score < best_score) {
            chosen = index;
            occupied = true;
            best_score = score;
        }
    }
    auto* entry = &frame_cache[chosen];
    if (occupied) {
        if (SotcStats::kFpsLog && frame_keys[chosen] != key) {
            SotcStats::bf_replaced.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        ++bind_cache_live;
    }
    frame_keys[chosen] = 0; // not findable while it is rewritten
    entry->pipeline = pipeline;
    entry->hash = hash;
    entry->last_use = ++bind_cache_clock;
    entry->uses_dma = uses_dma;
    entry->push_data = push_data;
    entry->bindless_slots = bindless_slots;
    entry->writes.assign(set_writes.begin(), set_writes.end());
    entry->write_sets.assign(set_write_sets.begin(), set_write_sets.end());
    entry->write_info.resize(set_writes.size());
    for (size_t i = 0; i < set_writes.size(); ++i) {
        const auto& write = set_writes[i];
        if (write.descriptorType == vk::DescriptorType::eStorageBuffer) {
            entry->write_info[i] =
                0x80000000u | static_cast<u32>(write.pBufferInfo - buffer_infos.data());
        } else {
            entry->write_info[i] = static_cast<u32>(write.pImageInfo - image_infos.data());
        }
    }
    entry->buffer_infos.assign(buffer_infos.begin(), buffer_infos.end());
    entry->image_infos.assign(image_infos.begin(), image_infos.end());
    entry->image_info_images.assign(image_info_images.begin(),
                                    image_info_images.begin() + image_infos.size());
    entry->buffers.assign(bound_buffers.begin() + first_bound_buffer, bound_buffers.end());
    entry->images.assign(bind_cache_images_rec.begin(), bind_cache_images_rec.end());
    entry->lod_banks.assign(bind_cache_lod_rec.begin(), bind_cache_lod_rec.end());
    entry->ranges.assign(bind_cache_ranges_rec.begin(), bind_cache_ranges_rec.end());
    entry->repairs.assign(bind_cache_repairs_rec.begin(), bind_cache_repairs_rec.end());
    entry->samplers.assign(bind_cache_samplers_rec.begin(), bind_cache_samplers_rec.end());
    entry->tex_watch.assign(tex_watch.begin(), tex_watch.end());
    entry->made_tick = state.tick;
    entry->stamp_tick = state.tick; // the full bind just made this tick's stamps
    entry->stamp_epoch = 0;         // block stamps due at the first hit
    entry->key = key;
    entry->targets_key = state.targets_key;
    entry->flush_gen = bind_cache_flush_gen;
    entry->buffer_version = state.buffer_generation;
    entry->stream_gen = state.stream_generation;
    entry->uses_stream = uses_stream;
    entry->refresh = bind_level2;
    if (bind_level2) {
        entry->flat_slots.assign(bind_cache_flat_rec.begin(), bind_cache_flat_rec.end());
    } else {
        entry->flat_slots.clear();
    }
    entry->map_dependent = bind_cache_map_dep_rec;
    entry->map_epoch = bind_rec_map_epoch;
    entry->tail_dependent = bind_cache_tail_rec;
    entry->gpu_epoch = bind_rec_gpu_epoch;
    entry->checked_write_seq = checked_write;
    entry->checked_tex_seq = checked_tex;
    entry->made_frame = SotcStats::cp_frame.load(std::memory_order_relaxed);
    entry->valid = true;
    frame_keys[chosen] = key;
    if (SotcStats::kFpsLog) {
        SotcStats::bf_stored.fetch_add(1, std::memory_order_relaxed);
        SotcStats::bf_live.store(bind_cache_live, std::memory_order_relaxed);
    }
}

void Rasterizer::RebindVertexIfSubmitted(const GraphicsPipeline* pipeline, u64& vertex_tick,
                                         bool is_indexed,
                                         const std::function<void()>& bind_index) {
    // sotc: a submit while binding (arena allocation, readback, release) moved recording to a new
    // command buffer, and the vertex input, vertex and index buffers recorded before it stayed in
    // the old one: the draw would run without them (broken vertices; review of 2026-10-27).
    // Record them again; a second submit while doing so is rare, so retry a few times.
    for (int tries = 0; tries < 4 && vertex_tick != scheduler.CurrentTick(); ++tries) {
        vertex_tick = scheduler.CurrentTick();
        BindVertexBuffers(pipeline);
        if (is_indexed) {
            bind_index();
        }
    }
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
        // Update current vertex inputs, unless this command buffer already holds them.
        // sotc: the cache is voided with the generation of DynamicState (SOTC_DEDUP_STATE).
        auto& dynamic = scheduler.GetDynamicState();
        auto& cache = dynamic.vertex_input_cache;
        static_assert(MaxVertexBufferCount <= DynamicState::VertexInputCache::MaxEntries);
        const bool same = kDedupState && cache.generation == dynamic.generation &&
                          std::ranges::equal(cache.attributes, attributes) &&
                          std::ranges::equal(cache.bindings, bindings);
        if (!same) {
            const auto cmdbuf = scheduler.CommandBuffer();
            cmdbuf.setVertexInputEXT(bindings, attributes);
            if (kDedupState) {
                cache.generation = dynamic.generation;
                cache.attributes.assign(attributes.begin(), attributes.end());
                cache.bindings.assign(bindings.begin(), bindings.end());
            }
        }
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
    // sotc: SOTC_RB_PREFETCH (off by default): a command boundary outside a render pass, where
    // every write of the commands so far is recorded; the prefetch copies go here, in the tick
    // being recorded, and ride on its submit (the early flush below, or the periodic one).
    if (!scheduler.IsRendering()) {
        buffer_cache.PrefetchIssue();
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
        // sotc: SOTC_ADAPTIVE_LOAD: while the governor is relieved, SOTC_ADAPTIVE_FLUSH_US
        // (default 4000) between submits outside a render pass; inside one the longest window
        // stays the larger of that and 4 * SOTC_FLUSH_US. Nothing needs the 1 ms cadence for
        // correctness: readbacks and the hot-write submit above submit on their own, buffers that
        // fill up within a tick wait (submit) as at any tick length, and the GPUs without the
        // periodic submit run whole frames in one tick. Normal: limit and pass_limit as before.
        const auto base = std::chrono::microseconds(flush_us);
        const u64 relieved_us = SotcAdaptive::FlushUs();
        const auto limit = relieved_us > flush_us ? std::chrono::microseconds(relieved_us) : base;
        const auto pass_limit = std::max(limit, base * 4);
        if (elapsed < limit || (scheduler.IsRendering() && elapsed < pass_limit)) {
            return;
        }
        if (SotcStats::kFpsLog && limit != base && !scheduler.IsRendering()) {
            const auto windows = static_cast<u64>(elapsed / base);
            if (windows > 1) {
                SotcStats::adaptive_flushes_saved.fetch_add(windows - 1,
                                                            std::memory_order_relaxed);
            }
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

// sotc: F5 (SotcStats::tex_capture): every sampled image of the next 30000 draws and dispatches
// is saved once to user/log/texcap_N, named by guest address, size, format, mips and flags, to
// find which texture shows garbage and where it lives.
void Rasterizer::CaptureTextures() {
    static u32 budget = 0, session = 0, count = 0;
    static std::filesystem::path dir;
    static std::set<u64> seen;
    // SOTC_TEXCAP_AFTER=<seconds>: one capture that long after the first draw (tests).
    static const auto first_draw = std::chrono::steady_clock::now();
    static long after = [] {
        const char* v = std::getenv("SOTC_TEXCAP_AFTER");
        return v ? std::atol(v) : 0L;
    }();
    if (after > 0 && std::chrono::steady_clock::now() - first_draw > std::chrono::seconds{after}) {
        after = 0;
        SotcStats::tex_capture.store(true, std::memory_order_relaxed);
    }
    if (SotcStats::tex_capture.exchange(false, std::memory_order_relaxed)) {
        budget = 30000;
        count = 0;
        seen.clear();
        dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir) /
              fmt::format("texcap_{}", ++session);
        std::filesystem::create_directories(dir);
        std::fprintf(stderr, "SOTC TEXCAP %u started\n", session);
    }
    if (!budget) {
        return;
    }
    if (--budget == 0) {
        std::fprintf(stderr, "SOTC TEXCAP %u done: %u textures\n", session, count);
    }
    for (const auto image_id : bound_images) {
        if (!image_id || count >= 600) {
            continue;
        }
        auto& image = texture_cache.GetImage(image_id);
        // At most 64 MiB per image and 1 GiB per capture: each one is downloaded through a
        // host staging buffer, and 600 full chains filled the system heap (out of memory, 26 Oct).
        static u64 captured_bytes = 0;
        if (count == 0) {
            captured_bytes = 0;
        }
        const u64 bytes = image.info.guest_size;
        if (image.info.size.width < 64 || image.info.size.height < 64 || bytes > 64_MB ||
            captured_bytes + bytes > 1_GB) {
            continue;
        }
        captured_bytes += bytes;
        const u64 key = (u64{image.info.guest_address} << 8) ^ u64(image.info.pixel_format);
        if (!seen.insert(key).second) {
            continue;
        }
        ++count;
        texture_cache.DumpImage(image_id,
                                dir / fmt::format("{:03}_{}_m{}f{}_fl{:x}_u{}{}{}", count,
                                                  vk::to_string(image.info.pixel_format),
                                                  image.info.resources.levels,
                                                  image.info.sotc_first_mip, u32(image.flags),
                                                  image.usage.render_target ? "R" : "",
                                                  image.usage.storage ? "S" : "",
                                                  image.usage.depth_target ? "D" : ""));
    }
}

void Rasterizer::ResetBindings(bool is_compute) {
    CaptureTextures();
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
    compute_written_image = desc0.is_written ? image0_id : image1_id; // sotc: BindResources
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
    compute_written_image = image1_id; // sotc: SOTC_BIND_CACHE_FRAME (BindResources)
    runtime.ClearImage(&image1, range, clear);
    return true;
}

const u32* Rasterizer::FlatForBind(const Shader::Info& stage) {
    // sotc: the live-setting patches of the flat buffer (moved out of BindBuffers so that a hit of
    // the resource identity bind cache, SOTC_BIND_CACHE_FRAME, applies them to the data it
    // re-copies). Every bind-time rewrite of the flat buffer is here: TAA resolve off, the tonemap
    // vignette moved after upscaling, the vegetation distance / height scales and the grass patch
    // LOD scale.
    // sotc: SOTC_PR_RESOLVE: FlatMayBePatched lists the stages these rewrites apply to (their
    // prepared resources are not used); keep the two in sync.
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
    return flat;
}

// sotc: SOTC_BIND_MARK_ONCE=1 (off by default). A written binding's guest write is noted by
// BufferCache::ObtainBuffer for the same range (the same NoteHostWrite call), so BindBuffers skips
// its second call when ObtainBuffer ran for that binding. NoteHostWrite only bumps
// SotcStats::mem_epoch, and every reader of that counter compares it for equality, so one bump
// (made before any reader can run) is as good as two; the stage hash and tag are not used.
static const bool kBindMarkOnce = [] {
    const char* v = std::getenv("SOTC_BIND_MARK_ONCE");
    return v && v[0] == '1';
}();

void Rasterizer::BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                             Shader::PushData& push_data, u32 set, const PreparedResources* pr,
                             const PrStage* pr_stage) {
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
                const u32* flat = FlatForBind(stage);
                // sotc: SOTC_BIND_CACHE: flat buffers changed by live settings are not cached.
                if (bind_cache_recording &&
                    (flat != stage.FlatBuf().data() || stage.pgm_hash == kTaaResolveHash ||
                     stage.pgm_hash == kTonemapHash)) {
                    bind_cache_uncacheable = true;
                }
                const u64 offset = vk_buffer.Copy(flat, ubo_size, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
                bind_cache_stream_rec = true; // sotc: SOTC_BIND_CACHE_FRAME: a copy in the ring
                if (bind_cache_recording && SotcStats::kBindCacheFrame) {
                    bind_cache_flat_rec.push_back({cur_bind_stage,
                                                   static_cast<u32>(buffer_infos.size() - 1),
                                                   ubo_size, XXH3_64bits(flat, ubo_size), offset,
                                                   buffer_cache.StreamGeneration(),
                                                   scheduler.CurrentTick()});
                }
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
            } else if (desc.buffer_type == Shader::BufferType::LodFeedback) {
                buffer_infos.emplace_back(sotc_lod_feedback->Handle(), 0,
                                          SotcLodFeedbackBytes + sizeof(u32));
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
            // sotc: SOTC_OP_PHASES: a binding is clean, dirty (its upload bytes moved) or written.
            const u64 ph_t0 = SotcOpPhases::NowBinding(), ph_upload0 = SotcOpPhases::UploadBytes();
            // sotc: SOTC_PR_RESOLVE: the values the prep thread computed for this descriptor;
            // otherwise the V# is read here.
            const u32 buf_index = static_cast<u32>(&desc - stage.buffers.data());
            const PrBuffer* pb =
                pr && pr_stage && pr->buffers[pr_stage->buf_first + buf_index].computed
                    ? &pr->buffers[pr_stage->buf_first + buf_index]
                    : nullptr;
            AmdGpu::Buffer vsharp{};
            if (!pb) {
                vsharp = desc.GetSharp(stage);
            }
            const VAddr vbase = pb ? pb->base : vsharp.base_address;
            const u32 vsize = pb ? static_cast<u32>(pb->vsize) : vsharp.GetSize();
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
            if (pb) {
                size = pb->size; // sotc: SOTC_PR_RESOLVE: clamped and limited by the prep thread
            } else if (clamp1) {
                if (vbase != 0 && vsize != 0) {
                    size = memory->ClampRangeSize(vbase, vsize);
                }
            } else if (vbase != 0 && vsize != 0 && memory->IsValidMapping(vbase) &&
                       (!desc.is_written || memory->IsMappedAddress(vbase))) {
                size = memory->ClampRangeSize(vbase, vsize);
            }
            // sotc: SOTC_BIND_CACHE_XTICK (BindCacheRepair)
            const u64 mapped_size = pb ? pb->mapped : size;
            if (size == 0) {
                buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
                if (bind_cache_recording && SotcStats::kBindCacheXtick &&
                    vbase != 0 && vsize != 0) {
                    bind_cache_map_dep_rec = true; // sotc: SOTC_BIND_CACHE_FRAME: a map can fill it
                    // sotc: SOTC_BIND_CACHE_XTICK: a null binding stays null only while the
                    // guest memory under it stays unmapped (checked once per tick).
                    bind_cache_repairs_rec.push_back({vbase, vsize, 0,
                                                      nullptr, 0, false});
                }
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
                // sotc: SOTC_PR_RESOLVE: a prepared size is limited already (its warning is inline only).
                if (!pb && !desc.is_written &&
                    (vsize >= 0xFFFF0000ULL || size > MaxSaneBindingSize) &&
                    unbounded_window != 0 && size > unbounded_window) {
                    static std::atomic<u32> reported{0};
                    if (vsize < 0xFFFF0000ULL && reported++ < 16) {
                        LOG_WARNING(Render,
                                    "Limiting {} MB binding at {:#x} of shader {:#x} to {} MB",
                                    size >> 20, vbase, stage.pgm_hash,
                                    unbounded_window >> 20);
                    }
                    size = unbounded_window;
                }
                if (size != vsize) {
                    LOG_DEBUG(Render, "Clamped size from {} to {} for stage {:#x}",
                              vsize, size, stage.pgm_hash);
                }
                // sotc: SOTC_WRITE_EXTENT. A written binding of a direct dispatch with a rule in
                // user/write_extent.txt is marked only over the window the rule proves (the
                // binding itself, its barriers and the aliased image / bind cache invalidations
                // below keep the full range). Texel buffers and everything else: full range.
                u32 mark_offset = 0;
                u32 mark_size = ~0u;
                if (SotcWriteExtent::kEnabled && desc.is_written && !desc.is_formatted &&
                    size <= 0xFFFFFFF0ULL && t_direct_dispatch.groups != 0 &&
                    t_direct_dispatch.hash == stage.pgm_hash) {
                    u64 window_off = 0, window_len = 0;
                    if (SotcWriteExtent::Narrow(stage.pgm_hash, buf_index, t_direct_dispatch.groups,
                                                t_direct_dispatch.threads, size, window_off,
                                                window_len)) {
                        mark_offset = static_cast<u32>(window_off);
                        mark_size = static_cast<u32>(window_len);
                    }
                }
                // sotc: SOTC_BIND_CACHE_FRAME. A read-only V# of a read-only graphics bind: the
                // arena and offset ObtainBuffer returned last time are still the answer while
                // BufferCache::BindVersion and the mapping under it are as they were (the checks
                // of a cached bind's hit, see TryBindCacheFrame); a CPU-modified range is
                // synchronized as there, and the block stamps are made once per use epoch.
                // Texel buffers (re-tiled from an aliasing image by every bind) and the stream
                // ring (small buffers with SOTC_STREAM_THRESHOLD) are not memoized.
                const bool memo_on = bind_memo_ok && !desc.is_written && !desc.is_formatted;
                BufferMemo* memo = nullptr;
                std::pair<const VideoCore::Buffer*, u64> obtained{};
                bool obtain_called = false; // sotc: SOTC_BIND_MARK_ONCE: ObtainBuffer ran here
                if (memo_on) {
                    if (buffer_memo.empty()) {
                        buffer_memo.resize(BufferMemoEntries);
                    }
                    const u64 h = (vbase * 0x9E3779B97F4A7C15ULL) ^
                                  (u64{vsize} * 0xC2B2AE3D27D4EB4FULL) ^ size;
                    memo = &buffer_memo[(h ^ (h >> 29)) % BufferMemoEntries];
                    if (memo->valid && memo->addr == vbase &&
                        memo->vsize == vsize && memo->mapped == mapped_size &&
                        memo->size == size && memo->version == buffer_cache.BindVersion()) {
                        bool usable = true;
                        if (buffer_cache.IsRegionCpuModified(memo->addr, memo->size)) {
                            buffer_cache.SynchronizeBinding(memo->arena, memo->addr,
                                                            static_cast<u32>(memo->size), false);
                            // The upload may have unbound or replaced what the memo names.
                            usable = memo->version == buffer_cache.BindVersion();
                            if (SotcStats::kFpsLog) {
                                SotcStats::bf_memo_repaired.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                        if (usable) {
                            if (const u16 epoch = buffer_cache.UseEpoch();
                                memo->stamp_epoch != epoch) {
                                buffer_cache.NoteBindUse(memo->addr, memo->size);
                                memo->stamp_epoch = epoch;
                            }
                            obtained = {memo->arena, memo->offset};
                            if (SotcStats::kFpsLog) {
                                SotcStats::bf_memo_hits.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                }
                if (!obtained.first) {
                    obtain_called = true;
                    // sotc: SOTC_TRACE (a = bytes uploaded, b = protect calls)
                    const SotcTrace::BindScope trace_obtain{"obtain buffer"};
                    obtained = buffer_cache.ObtainBuffer(vbase, size, desc.is_written,
                                                         desc.is_formatted, mark_offset, mark_size);
                    if (memo) {
                        // The version after the call: it may have unbound or evicted other ranges.
                        const bool stream =
                            static_cast<const void*>(obtained.first) ==
                            static_cast<const void*>(&buffer_cache.GetStreamBuffer());
                        *memo = BufferMemo{vbase,
                                           vsize,
                                           mapped_size,
                                           size,
                                           obtained.second,
                                           buffer_cache.BindVersion(),
                                           obtained.first,
                                           buffer_cache.UseEpoch(),
                                           !stream};
                        if (SotcStats::kFpsLog) {
                            SotcStats::bf_memo_misses.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
                const auto [buffer, offset] = obtained;
                if (SotcStats::kWriteMarks && desc.is_written) { // sotc: kWriteMarks (SOTC_FPS_LEAN)
                    SotcWriteExtent::NoteMark(
                        stage.pgm_hash, buf_index, vbase, size,
                        vbase + (mark_size != ~0u ? mark_offset : 0),
                        mark_size != ~0u ? mark_size : size,
                        t_direct_dispatch.hash == stage.pgm_hash ? t_direct_dispatch.threads : 0);
                }
                const u64 offset_aligned = Common::AlignDown(offset, alignment);
                const u64 adjust = offset - offset_aligned;
                if (adjust % 4 != 0) {
                    LOG_WARNING(Render_Vulkan, "Buffer binding in shader {:#x} isn't dword aligned",
                                stage.pgm_hash);
                }
                push_data.AddOffset(binding.buffer, adjust);
                buffer_infos.emplace_back(buffer->Handle(), offset_aligned, size + adjust);
                bound_buffers.emplace_back(buffer, offset, size, desc.is_written);
                if (bind_cache_recording) { // sotc: SOTC_BIND_CACHE checks it is still clean
                    bind_cache_ranges_rec.emplace_back(vbase, size);
                    if (SotcStats::kBindCacheFrame && desc.is_formatted) {
                        // sotc: an image registered over a texel buffer changes its binding
                        bind_cache_texel_rec.emplace_back(vbase,
                                                          vbase + size);
                    }
                    if (SotcStats::kBindCacheXtick) {
                        bind_cache_repairs_rec.push_back({vbase, vsize,
                                                          mapped_size, buffer,
                                                          static_cast<u32>(size),
                                                          desc.is_formatted});
                    }
                    // sotc: SOTC_BIND_CACHE_RANGES: what a hit cannot check. A stream-buffer copy
                    // is a snapshot of guest memory; a texel buffer at an image's address is
                    // re-tiled from that image by every full bind once the GPU wrote it
                    // (SynchronizeMemoryFromImage), and HTile is refilled.
                    if (SotcStats::kBindCacheRanges &&
                        (static_cast<const void*>(buffer) ==
                             static_cast<const void*>(&buffer_cache.GetStreamBuffer()) ||
                         (desc.is_formatted && [&] {
                             if (texture_cache.IsMeta(vbase)) {
                                 return true;
                             }
                             // Any image FindImageFromRange could return now or once the GPU
                             // wrote it (it matches the base address only).
                             const auto texture_lock = texture_cache.BatchLock();
                             bool aliased = false;
                             texture_cache.ForEachImageInRegion(
                                 vbase, size,
                                 [&](VideoCore::ImageId, VideoCore::Image& image) {
                                     aliased |= image.info.guest_address == vbase;
                                     return aliased;
                                 });
                             return aliased;
                         }()))) {
                        bind_cache_uncacheable = true;
                    }
                }
                if (desc.is_written) {
                    // Raw storage-buffer writes can also make an aliased cached image stale.
                    const SotcTrace::ScopeMin trace_inval{"invalidate images"}; // sotc: SOTC_TRACE
                    texture_cache.InvalidateMemoryFromGPU(vbase, size);
                    // sotc: SOTC_BIND_MARK_ONCE (kBindMarkOnce): ObtainBuffer already noted this
                    // range when it ran for this binding; the second note only bumps mem_epoch.
                    if (!(kBindMarkOnce && obtain_called)) {
                        Core::MemoryManager::NoteHostWrite(vbase, size, 7,
                                                           stage.pgm_hash);
                    }
                    if (SotcStats::kBindCacheRanges) { // sotc: drops the binds reading it
                        BindCacheInvalidate(vbase, size);
                    }
                }
                needs_barrier |= runtime.IsBufferAccessed(buffer, offset, size, desc.is_written);
                SotcOpPhases::ChargeBinding(desc.is_written, ph_upload0, ph_t0); // sotc: SOTC_OP_PHASES
            }
        }

        set_write_sets[set_write_index] = set;
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

// sotc: the final composite samples the UI layer and the scene: UI first in the base game's
// shader, scene first in the PS4 Pro one (0x9e6767fc). The UI layer is the output-size sRGB image.
u32 Rasterizer::CompositeUiIndex() {
    const auto& first = texture_cache.GetImage(image_bindings[0].first);
    return first.info.pixel_format == vk::Format::eR16G16B16A16Sfloat ? 1 : 0;
}


// sotc: DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL serves the depth target (stencil writes) and the
// sampling of its depth aspect, so both uses ask for the same stages and accesses: the image state
// does not change from one draw to the next and the render pass is not ended for it.
static constexpr vk::PipelineStageFlags2 kDepthReadOnlyStencilStages =
    vk::PipelineStageFlagBits2::eAllCommands;
static constexpr vk::AccessFlags2 kDepthReadOnlyStencilAccess =
    vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
    vk::AccessFlagBits2::eShaderRead;

bool Rasterizer::DepthSampledAsAttachment(VideoCore::ImageId image_id, const VideoCore::Image& image,
                                          const VideoCore::TextureCache::ImageDesc& desc,
                                          bool compute) const {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_DEPTH_RO");
        return !v || v[0] != '0';
    }();
    if (!on || compute || !image.info.props.has_stencil || image_id != db_desc.first) {
        return false;
    }
    if (!liverpool->DrawRegs().depth_control.stencil_enable || db_desc.second.view_info.is_storage) {
        return false;
    }
    const vk::Format fmt = desc.view_info.format;
    return Vulkan::LiverpoolToVK::IsFormatDepthCompatible(fmt) &&
           !Vulkan::LiverpoolToVK::IsFormatStencilCompatible(fmt);
}

void Rasterizer::PrefetchImagesImpl(const Pipeline* pipeline) {
    const u64 now = SotcStats::NowNs(); // sotc: one clock read per draw
    // sotc: SOTC_ASYNC_IMAGE_CREATE. Reads the T#s as BindTextures will on the draw thread (from
    // this thread's copy of the per-draw data, Shader::g_info_slot 0) with the same validity
    // checks; descriptor arrays are left out.
    for (const Shader::Info* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        for (const auto& image_desc : stage->images) {
            if (image_desc.array_size != 0) {
                continue;
            }
            const auto tsharp = image_desc.GetSharp(*stage);
            const auto data_fmt = tsharp.GetDataFmt();
            const auto num_fmt = tsharp.GetNumberFmt();
            if (tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid ||
                !memory->IsValidGpuMapping(tsharp.Address(), 0) ||
                !magic_enum::enum_contains(data_fmt) || !magic_enum::enum_contains(num_fmt)) {
                continue;
            }
            texture_cache.PrefetchTexture(tsharp, image_desc, now);
        }
    }
}

namespace {
// sotc: SOTC_TEX_BLOCK counters, one line on stderr per 5 s (TexBlockReport).
std::atomic<u64> tex_block_lookups{}, tex_block_hits{}, tex_block_stores{};
std::atomic<u64> tex_block_mismatches{};
std::atomic<s64> tex_block_report_due{};

void TexBlockReport() {
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    s64 due = tex_block_report_due.load(std::memory_order_relaxed);
    if (now < due || !tex_block_report_due.compare_exchange_strong(due, now + 5'000'000'000LL)) {
        return;
    }
    std::fprintf(stderr,
                 "SOTC TEX BLOCK: lookups %llu, hits %llu, stores %llu, verify mismatches %llu\n",
                 static_cast<unsigned long long>(tex_block_lookups.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(tex_block_hits.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(tex_block_stores.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     tex_block_mismatches.load(std::memory_order_relaxed)));
}
} // namespace

int Rasterizer::TexBlockMode() {
    // sotc: SOTC_TEX_BLOCK, read once. 1: a stage's texture block is replayed from the memo when
    // it can be; 2: also runs the full bind on a would-be hit and compares the two (verify).
    static const int mode = [] {
        const char* v = std::getenv("SOTC_TEX_BLOCK");
        return v && v[0] == '1' ? 1 : v && v[0] == '2' ? 2 : 0;
    }();
    return mode;
}

bool Rasterizer::MakeTextureBlockKey(const Shader::Info& stage, u32 set,
                                     const PreparedResources* pr, const PrStage* pr_stage,
                                     TexBlockKey& key) {
    // Not keyed: the binds the memo does not answer (storage and fmask stages, descriptor arrays,
    // samplers the full bind rewrites), the composite paths that act on a fragment stage's two
    // images (TakeHudless, DLSS, SOTC_DLSS_PROBE: their side effects are not replayed), and stages
    // without images or samplers. sotc: while the bind cache records, the memo still answers and
    // replays the cache records (see TryTextureBlock), except with SOTC_BIND_CACHE_FRAME (its
    // lookups are not replayed) or a composite probe (its log is not).
    static const bool composite_probe = [] {
        const char* h = std::getenv("SOTC_FG_HUDLESS");
        return (h && h[0] == '1') || std::getenv("SOTC_DLSS_PROBE") != nullptr;
    }();
    if ((bind_cache_recording && (SotcStats::kBindCacheFrame || composite_probe)) ||
        stage.images.size() > MaxBlockImages || stage.samplers.size() > MaxBlockSamplers ||
        (stage.images.empty() && stage.samplers.empty()) || !stage.fmasks.empty()) {
        return false;
    }
    if (stage.sw_stage == Shader::SwStage::Fragment && stage.images.size() <= 2 &&
        (composite_probe || Dlssg::SuperResolution())) {
        return false;
    }
    auto& k = key.key;
    u32 n = 0;
    k[n++] = u64{stage.images.size()} | u64{stage.samplers.size()} << 8 | u64{set} << 16 |
             u64(static_cast<u32>(stage.sw_stage)) << 24;
    k[n++] = stage.pgm_hash;
    const VAddr bc_base = liverpool->DrawRegs().ta_bc_base.Address();
    k[n++] = bc_base;
    for (u32 index = 0; index < stage.images.size(); ++index) {
        const auto& image_desc = stage.images[index];
        if (image_desc.is_written || image_desc.array_size != 0) {
            return false;
        }
        // The T# the full bind reads: the prepared one when its entry is resolved.
        const PrImage* pi = pr && pr_stage ? &pr->images[pr_stage->img_first + index] : nullptr;
        if (pi && pi->verdict == 0) {
            pi = nullptr;
        }
        const AmdGpu::Image tsharp = pi ? pi->tsharp : image_desc.GetSharp(stage);
        k[n++] = u64{image_desc.is_depth} | u64{image_desc.is_atomic} << 1 |
                 u64{image_desc.is_array} << 2 | u64{image_desc.is_r128} << 3 |
                 u64{image_desc.constant_mip_index} << 8 |
                 u64(image_desc.mip_fallback_mode) << 16 | u64(image_desc.post_op) << 24;
        static_assert(sizeof(tsharp) == 4 * sizeof(u64));
        std::memcpy(&k[n], &tsharp, sizeof(tsharp));
        n += 4;
    }
    for (const auto& sampler : stage.samplers) {
        const AmdGpu::Sampler ssharp = sampler.GetSharp(stage);
        if (!ssharp.Valid() || (ssharp.border_color_type.Value() == AmdGpu::BorderColor::Custom &&
                                bc_base == 0)) {
            return false; // the full bind logs and rewrites these
        }
        k[n++] = u64{sampler.is_depth};
        static_assert(sizeof(ssharp) == 2 * sizeof(u64));
        std::memcpy(&k[n], &ssharp, sizeof(ssharp));
        n += 2;
    }
    u64 hash = 0x9E3779B97F4A7C15ULL;
    for (u32 i = 0; i < n; ++i) {
        hash = (hash ^ k[i]) * 0xC2B2AE3D27D4EB4FULL;
        hash ^= hash >> 29;
    }
    key.words = n;
    key.hash = hash;
    key.slot = static_cast<u32>((hash ^ (hash >> 32)) % NumTexBlockMemos);
    return true;
}

bool Rasterizer::TexBlockMatches(const TexBlockMemo& memo, const TexBlockKey& key) {
    return memo.valid && memo.key_words == key.words &&
           std::memcmp(memo.key.data(), key.key.data(), key.words * sizeof(u64)) == 0;
}

bool Rasterizer::TryTextureBlock(const Shader::Info& stage, const TexBlockKey& key, u32 set,
                                 Shader::Backend::Bindings& binding, bool mark_lods) {
    const TexBlockMemo& memo = (*tex_block_memos)[key.slot];
    if (!TexBlockMatches(memo, key)) {
        return false;
    }
    // A block that may hold a mip tail depends on the whole image set (DecideRebase looks at the
    // images overlapping the tail) and on the GPU-written tails: those two must not have moved.
    if (memo.candidate &&
        (texture_cache.BindGeneration() != memo.bind_generation ||
         VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed) != memo.gpu_epoch)) {
        return false;
    }
    // The bound image is the one stored, and its backing is in the state the bind left it in
    // (state_version), with nothing that makes the bind go another way (needs_rebind, a render
    // target or a feedback loop).
    const auto bound_current = [&](const TexBlockEntry& e) {
        const VideoCore::Image* image = texture_cache.TryGetImage(e.bound, e.bound_uid);
        return image && image->backing == e.backing &&
               image->backing->state_version == e.version && !image->binding.needs_rebind &&
               (image->info.props.is_depth ||
                (!image->binding.force_general && !image->binding.is_target));
    };
    // Checks only, nothing changed yet: the lookup's generation and image, the bound image.
    for (u32 i = 0; i < memo.num_entries; ++i) {
        const TexBlockEntry& e = memo.entries[i];
        if (!e.null && (texture_cache.ImageAddrGeneration(e.addr) != e.addr_gen ||
                        !texture_cache.TryGetImage(e.found, e.found_uid) || !bound_current(e))) {
            return false;
        }
    }

    // From here the bind's side effects are made in the order of the full bind: the lookups'
    // stamps, the depth links and bound flags, then per image UpdateImage (FindTexture) and the
    // transition. A miss after any of them gives the stamps back: the full bind makes them again.
    const u64 tick = scheduler.CurrentTick();
    std::array<std::pair<u64, u64>, MaxBlockEntries> stamps{}; // (tick_prev, tick_accessed) before
    const auto restore_stamps = [&] {
        for (u32 i = memo.num_entries; i-- > 0;) {
            const TexBlockEntry& e = memo.entries[i];
            if (!e.null) {
                VideoCore::Image& found = texture_cache.GetImage(e.found);
                found.tick_prev_accessed = stamps[i].first;
                found.tick_accessed_last = stamps[i].second;
            }
        }
    };
    for (u32 i = 0; i < memo.num_entries; ++i) {
        const TexBlockEntry& e = memo.entries[i];
        if (e.null) {
            continue;
        }
        VideoCore::Image& found = texture_cache.GetImage(e.found);
        stamps[i] = {found.tick_prev_accessed, found.tick_accessed_last};
        found.tick_prev_accessed = found.tick_accessed_last;
        found.tick_accessed_last = tick;
    }
    for (u32 i = 0; i < memo.num_entries; ++i) {
        const TexBlockEntry& e = memo.entries[i];
        if (e.null) {
            continue;
        }
        const VideoCore::ImageId depth =
            texture_cache.GetAssociatedDepth(texture_cache.GetImage(e.found));
        if (depth != (e.redirect ? e.bound : VideoCore::ImageId{})) {
            restore_stamps();
            return false;
        }
        texture_cache.GetImage(e.bound).binding.is_bound = 1u;
    }

    std::array<vk::DescriptorImageInfo, MaxBlockEntries> infos{};
    std::array<VideoCore::ImageId, MaxBlockEntries> ids{};
    for (u32 i = 0; i < memo.num_entries; ++i) {
        const TexBlockEntry& e = memo.entries[i];
        if (e.null) {
            infos[i] = vk::DescriptorImageInfo(VK_NULL_HANDLE, VK_NULL_HANDLE,
                                               vk::ImageLayout::eGeneral);
            continue;
        }
        // FindTexture's UpdateImage (uploads what the guest wrote), then the second pass's checks
        // on what that changed.
        if (!bound_current(e)) {
            restore_stamps();
            return false;
        }
        texture_cache.UpdateImage(e.bound);
        if (!bound_current(e)) {
            restore_stamps();
            return false;
        }
        VideoCore::Image& image = texture_cache.GetImage(e.bound);
        // As the second pass: no depth-stencil image (TryTextureBlock stores none), so the layout
        // is the sampled one of its kind.
        const auto new_layout = image.info.props.is_depth
                                    ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                    : vk::ImageLayout::eShaderReadOnlyOptimal;
        needs_barrier |= runtime.Transit(&image, new_layout,
                                         vk::PipelineStageFlagBits2::eAllCommands,
                                         vk::AccessFlagBits2::eShaderRead, e.range);
        image.usage.texture = 1u;
        infos[i] = vk::DescriptorImageInfo(VK_NULL_HANDLE, e.view, image.backing->state.layout);
        ids[i] = e.bound;
    }

    // sotc: SOTC_BIND_CACHE: the records the full bind makes, in its order (LOD banks in its first
    // pass, sampled images in its second). Nothing below can return false.
    if (bind_cache_recording) {
        for (u32 i = 0; i < memo.num_lods; ++i) {
            bind_cache_lod_rec.push_back(memo.lod_banks[i]);
        }
        for (u32 i = 0; i < memo.num_entries; ++i) {
            const TexBlockEntry& e = memo.entries[i];
            if (e.null) {
                continue;
            }
            // The sampled layout, as the infos loop above.
            const auto new_layout = texture_cache.GetImage(e.bound).info.props.is_depth
                                        ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                        : vk::ImageLayout::eShaderReadOnlyOptimal;
            BindCacheNoteImage(e.bound, new_layout, vk::PipelineStageFlagBits2::eAllCommands,
                               vk::AccessFlagBits2::eShaderRead, e.range);
        }
    }
    // The bindings, as BindTexturesFull makes them: the bound images (each descriptor's infos
    // and one write), then the LOD marks and the samplers.
    for (u32 i = 0; mark_lods && i < memo.num_lods; ++i) {
        SotcLod::Mark(memo.lod_banks[i]);
    }
    u32 entry = 0;
    for (u32 d = 0; d < memo.num_descs; ++d) {
        const u32 count = memo.desc_counts[d];
        const u32 info_index = image_infos.size();
        for (u32 j = 0; j < count; ++j, ++entry) {
            image_infos.push_back(infos[entry]);
            image_info_images[image_infos.size() - 1] = ids[entry];
            if (ids[entry]) {
                bound_images.emplace_back(ids[entry]);
            }
        }
        set_write_sets[set_write_index] = set;
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = count;
        set_write.descriptorType = vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[info_index];
        binding.unified += count;
    }
    const VideoCore::ImageId first = memo.num_entries == 1 ? ids[0] : VideoCore::ImageId{};
    const VideoCore::Image* ui_asset = UiLinearAsset(stage, memo.num_entries, first);
    // sotc: SOTC_BIND_CACHE: as BindTexturesFull. The composite probes are refused while recording
    // (MakeTextureBlockKey), so only the UI filter and hudless_ui_info can set it here.
    if (bind_cache_recording && (ui_asset || hudless_ui_info >= 0)) {
        bind_cache_uncacheable = true;
    }
    BindSamplers(stage, binding, set, ui_asset);
    return true;
}

void Rasterizer::BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                              u32 set, const PreparedResources* pr, const PrStage* pr_stage) {
    const auto texture_lock = texture_cache.BatchLock(); // sotc: one lock for the pass
    const int mode = TexBlockMode();
    TexBlockKey key;
    if (mode == 0 || !MakeTextureBlockKey(stage, set, pr, pr_stage, key)) {
        BindTexturesFull(stage, binding, set, pr, pr_stage, nullptr, nullptr);
        return;
    }
    TexBlockMemo& slot = (*tex_block_memos)[key.slot];
    tex_block_lookups.fetch_add(1, std::memory_order_relaxed);
    TexBlockReport();
    if (mode == 1) {
        if (TryTextureBlock(stage, key, set, binding)) {
            tex_block_hits.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        BindTexturesFull(stage, binding, set, pr, pr_stage, &key, &slot);
        return;
    }

    // SOTC_TEX_BLOCK=2 (verify): a would-be hit is replayed, its outputs and the lookups' stamps
    // are taken back, the full bind is made and the two are compared. The full bind's result is
    // the one kept. The replay's UpdateImage and transitions run twice: both are idempotent here.
    // sotc: while SOTC_BIND_CACHE records, the replay's cache records and uncacheable flag are
    // compared with the full bind's the same way.
    const u32 info_base = image_infos.size();
    const u32 write_base = set_write_index;
    const u32 bound_base = bound_images.size();
    const Shader::Backend::Bindings binding_base = binding;
    struct Stamp {
        VideoCore::ImageId id;
        u64 prev;
        u64 accessed;
    };
    std::array<Stamp, MaxBlockEntries> stamps{};
    u32 num_stamps = 0;
    if (TexBlockMatches(slot, key)) {
        for (u32 i = 0; i < slot.num_entries; ++i) {
            const TexBlockEntry& e = slot.entries[i];
            const VideoCore::Image* found =
                e.null ? nullptr : texture_cache.TryGetImage(e.found, e.found_uid);
            if (found) {
                stamps[num_stamps++] = {e.found, found->tick_prev_accessed,
                                        found->tick_accessed_last};
            }
        }
    }
    // sotc: SOTC_BIND_CACHE records of the replay: copied for the comparison below, then taken
    // back with its outputs. The full bind makes its own.
    const size_t lod0 = bind_cache_lod_rec.size();
    const size_t img0 = bind_cache_images_rec.size();
    const size_t smp0 = bind_cache_samplers_rec.size();
    const bool unc0 = bind_cache_uncacheable;
    // sotc: each side starts from a clear flag, so what this stage's replay and full bind set is
    // compared even when an earlier stage already set it (unc0 is put back after both).
    bind_cache_uncacheable = false;
    // sotc: no LOD marks from the replay: the full bind below makes them once, as in mode 0.
    const bool replayed = TryTextureBlock(stage, key, set, binding, false);
    const std::vector<u32> r_lods(bind_cache_lod_rec.begin() + lod0, bind_cache_lod_rec.end());
    const std::vector<BindCacheImage> r_recs(bind_cache_images_rec.begin() + img0,
                                             bind_cache_images_rec.end());
    const std::vector<BindCacheSampler> r_samplers(bind_cache_samplers_rec.begin() + smp0,
                                                   bind_cache_samplers_rec.end());
    const bool r_uncacheable = bind_cache_uncacheable;
    bind_cache_lod_rec.erase(bind_cache_lod_rec.begin() + lod0, bind_cache_lod_rec.end());
    bind_cache_images_rec.erase(bind_cache_images_rec.begin() + img0, bind_cache_images_rec.end());
    bind_cache_samplers_rec.erase(bind_cache_samplers_rec.begin() + smp0,
                                  bind_cache_samplers_rec.end());
    bind_cache_uncacheable = unc0;
    if (!replayed) {
        BindTexturesFull(stage, binding, set, pr, pr_stage, &key, &slot);
        return;
    }
    tex_block_hits.fetch_add(1, std::memory_order_relaxed);
    const std::vector<vk::DescriptorImageInfo> r_infos(image_infos.begin() + info_base,
                                                       image_infos.end());
    const std::vector<VideoCore::ImageId> r_ids(image_info_images.begin() + info_base,
                                                image_info_images.begin() + image_infos.size());
    const std::vector<VideoCore::ImageId> r_bound(bound_images.begin() + bound_base,
                                                  bound_images.end());
    using WriteKey = std::array<s64, 4>; // dstBinding, count, type, index into image_infos
    const auto write_key = [&](const vk::WriteDescriptorSet& w) {
        return WriteKey{s64(w.dstBinding), s64(w.descriptorCount),
                        s64(static_cast<u32>(w.descriptorType)),
                        static_cast<s64>(static_cast<const vk::DescriptorImageInfo*>(w.pImageInfo) -
                                         image_infos.data())};
    };
    std::vector<WriteKey> r_writes;
    for (u32 i = write_base; i < set_write_index; ++i) {
        r_writes.push_back(write_key(set_writes[i]));
    }
    const u32 r_unified = binding.unified;
    // Take the replay's outputs back, and the stamps it made.
    image_infos.resize(info_base);
    set_write_index = write_base;
    bound_images.resize(bound_base);
    binding = binding_base;
    for (u32 i = num_stamps; i-- > 0;) {
        VideoCore::Image& found = texture_cache.GetImage(stamps[i].id);
        found.tick_prev_accessed = stamps[i].prev;
        found.tick_accessed_last = stamps[i].accessed;
    }

    bind_cache_uncacheable = false; // sotc: this stage's own flag (see unc0 above)
    BindTexturesFull(stage, binding, set, pr, pr_stage, &key, &slot);
    const bool f_uncacheable = bind_cache_uncacheable;
    bind_cache_uncacheable = unc0 || f_uncacheable; // the full bind only ever sets it

    bool same = binding.unified == r_unified && image_infos.size() - info_base == r_infos.size() &&
                set_write_index - write_base == r_writes.size() &&
                bound_images.size() - bound_base == r_bound.size();
    for (u32 i = 0; same && i < r_infos.size(); ++i) {
        const auto& a = r_infos[i];
        const auto& b = image_infos[info_base + i];
        same = a.sampler == b.sampler && a.imageView == b.imageView &&
               a.imageLayout == b.imageLayout && r_ids[i] == image_info_images[info_base + i];
    }
    for (u32 i = 0; same && i < r_writes.size(); ++i) {
        same = r_writes[i] == write_key(set_writes[write_base + i]);
    }
    for (u32 i = 0; same && i < r_bound.size(); ++i) {
        same = r_bound[i] == bound_images[bound_base + i];
    }
    // sotc: SOTC_BIND_CACHE records of the full bind against the replay's. Neither adds any
    // while not recording, so the counts match then.
    bool rec_same = bind_cache_lod_rec.size() - lod0 == r_lods.size() &&
                    bind_cache_images_rec.size() - img0 == r_recs.size() &&
                    bind_cache_samplers_rec.size() - smp0 == r_samplers.size();
    for (size_t i = 0; rec_same && i < r_samplers.size(); ++i) {
        const BindCacheSampler& a = r_samplers[i];
        const BindCacheSampler& b = bind_cache_samplers_rec[smp0 + i];
        rec_same = std::memcmp(&a.sharp, &b.sharp, sizeof(a.sharp)) == 0 &&
                   a.handle == b.handle && a.slot == b.slot && a.is_depth == b.is_depth &&
                   a.bindless == b.bindless;
    }
    for (size_t i = 0; rec_same && i < r_lods.size(); ++i) {
        rec_same = r_lods[i] == bind_cache_lod_rec[lod0 + i];
    }
    for (size_t i = 0; rec_same && i < r_recs.size(); ++i) {
        const BindCacheImage& a = r_recs[i];
        const BindCacheImage& b = bind_cache_images_rec[img0 + i];
        rec_same = a.id == b.id && a.layout == b.layout && a.stages == b.stages &&
                   a.access == b.access && a.range.base.level == b.range.base.level &&
                   a.range.base.layer == b.range.base.layer &&
                   a.range.extent.levels == b.range.extent.levels &&
                   a.range.extent.layers == b.range.extent.layers && a.guest_lo == b.guest_lo &&
                   a.guest_hi == b.guest_hi && a.uid == b.uid;
    }
    // Both start from a clear flag. The replay only sets it while recording, so compare then only.
    if (bind_cache_recording) {
        rec_same = rec_same && f_uncacheable == r_uncacheable;
    }
    same = same && rec_same;
    if (!same) {
        tex_block_mismatches.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr,
                     "SOTC TEX BLOCK mismatch: shader %#llx, infos %zu/%zu, writes %zu/%zu, "
                     "images %zu/%zu, lods %zu/%zu, recs %zu/%zu\n",
                     static_cast<unsigned long long>(stage.pgm_hash), r_infos.size(),
                     static_cast<size_t>(image_infos.size() - info_base), r_writes.size(),
                     static_cast<size_t>(set_write_index - write_base), r_bound.size(),
                     static_cast<size_t>(bound_images.size() - bound_base), r_lods.size(),
                     static_cast<size_t>(bind_cache_lod_rec.size() - lod0), r_recs.size(),
                     static_cast<size_t>(bind_cache_images_rec.size() - img0));
    }
}

void Rasterizer::BindTexturesFull(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                                  u32 set, const PreparedResources* pr, const PrStage* pr_stage,
                                  const TexBlockKey* tex_key, TexBlockMemo* rec) {
    image_bindings.clear();
    // sotc: SOTC_TEX_BLOCK: what the memo records of this bind (rec, see TryTextureBlock).
    // store_ok goes false on anything the replay does not repeat.
    bool store_ok = true;
    if (rec) {
        rec->valid = false;
        rec->key_words = tex_key->words;
        std::copy_n(tex_key->key.begin(), tex_key->words, rec->key.begin());
        rec->candidate = false;
        rec->num_lods = 0;
        rec->bind_generation = texture_cache.BindGeneration();
        rec->gpu_epoch = VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed);
    }
    const u32 first_image_idx = image_infos.size();
    // To emulate storing to explicit mip levels, build a descriptor array with each mip level.
    boost::container::small_vector<u32, 8> image_descriptor_array_sizes;

    u32 image_index = 0;
    for (const auto& image_desc : stage.images) {
        // sotc: SOTC_PR_RESOLVE: the T# and its verdict as the prep thread computed them (PrImage).
        const PrImage* pi =
            pr && pr_stage ? &pr->images[pr_stage->img_first + image_index] : nullptr;
        if (pi && pi->verdict == 0) {
            pi = nullptr;
        }
        ++image_index;
        const auto tsharp = pi ? pi->tsharp : image_desc.GetSharp(stage);
        if (image_desc.array_size != 0) {
            store_ok = false; // sotc: SOTC_TEX_BLOCK: descriptor arrays are not stored
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
                    // sotc: SOTC_BIND_CACHE_FRAME: a guest map can make the element bindable
                    bind_cache_map_dep_rec |= bind_cache_recording;
                    image_bindings.emplace_back(std::piecewise_construct, std::tuple{},
                                                std::tuple{});
                    continue;
                }
                if (element.lod_hw_cnt_en) {
                    SotcLod::Mark(element.counter_bank_id);
                    if (bind_cache_recording) { // sotc: SOTC_BIND_CACHE repeats it on a hit
                        bind_cache_lod_rec.push_back(element.counter_bank_id);
                    }
                }
                auto& [image_id, desc] = image_bindings.emplace_back(
                    std::piecewise_construct, std::tuple{}, std::tuple{element, image_desc});
                image_id = texture_cache.FindImage(desc);
                if (bind_cache_recording && SotcStats::kBindCacheFrame) {
                    BindCacheNoteLookup(desc, image_id); // sotc: SOTC_BIND_CACHE_FRAME
                }
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
            if (bind_cache_recording) { // sotc: SOTC_BIND_CACHE repeats it on a hit
                bind_cache_lod_rec.push_back(tsharp.counter_bank_id);
            }
            if (rec) { // sotc: SOTC_TEX_BLOCK repeats the mark on a hit
                if (rec->num_lods < MaxBlockImages) {
                    rec->lod_banks[rec->num_lods++] = tsharp.counter_bank_id;
                } else {
                    store_ok = false;
                }
            }
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
        // sotc: SOTC_POOL_WATCH: the T#s bound at an address whose pages the game freed while the
        // texture was in use (TextureCache::PoolWatch), each distinct mip field set once.
        if (const u32 watched = SotcStats::pool_watch_count.load(std::memory_order_relaxed)) {
            const u64 addr = tsharp.Address();
            for (u32 k = 0; k < std::min<u32>(watched, 64); ++k) {
                if (SotcStats::pool_watch_addr[k].load(std::memory_order_relaxed) != addr) {
                    continue;
                }
                static std::mutex m;
                static std::set<std::tuple<u64, u32, u32, u32>> seen;
                std::scoped_lock lk{m};
                if (seen.size() < 400 &&
                    seen.insert({addr, u32(tsharp.base_level), u32(tsharp.last_level),
                                 u32(tsharp.min_lod)}).second) {
                    std::fprintf(stderr,
                                 "SOTC POOLWATCH tsharp %#llx %ux%u base %u last %u min_lod %.2f "
                                 "bank %u hwcnt %u\n",
                                 static_cast<unsigned long long>(addr), tsharp.width + 1,
                                 tsharp.height + 1, u32(tsharp.base_level), u32(tsharp.last_level),
                                 tsharp.min_lod / 256.f, u32(tsharp.counter_bank_id),
                                 u32(tsharp.lod_hw_cnt_en));
                }
                break;
            }
        }
        // sotc: first few only (per-draw warning; see BufferCache::SynchronizeMemoryFromImage).
        if (static u32 shown = 0; shown < 4 && texture_cache.IsMeta(tsharp.Address())) {
            ++shown;
            LOG_WARNING(Render_Vulkan, "Unexpected metadata read by a shader (texture)");
        }

        const auto data_fmt = tsharp.GetDataFmt();
        const auto num_fmt = tsharp.GetNumberFmt();
        if (pi ? pi->verdict == 2
               : (tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid)) {
            image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
            image_descriptor_array_sizes.push_back(1);
            if (rec && image_bindings.size() <= MaxBlockEntries) { // sotc: a null binding
                rec->entries[image_bindings.size() - 1] = TexBlockEntry{.null = true};
            }
            continue;
        }

        if (pi ? pi->verdict == 3
               : (!memory->IsValidGpuMapping(tsharp.Address(), 0) ||
                  !magic_enum::enum_contains(data_fmt) || !magic_enum::enum_contains(num_fmt))) {
            LOG_WARNING(Render_Vulkan,
                        "Rejecting invalid T# address={:#x}, pitch={}, width={}, "
                        "data_format={}, num_format={}",
                        tsharp.Address(), tsharp.pitch, tsharp.width, static_cast<u32>(data_fmt),
                        static_cast<u32>(num_fmt));
            // sotc: SOTC_BIND_CACHE_FRAME: a guest map can make the T# valid
            bind_cache_map_dep_rec |= bind_cache_recording;
            store_ok = false; // sotc: SOTC_TEX_BLOCK: the memory map decided this one
            image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        const Shader::MipStorageFallbackMode mip_fallback_mode = image_desc.mip_fallback_mode;
        const u32 num_bindings = pi ? pi->num_bindings : image_desc.NumBindings(stage);

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
            if (rec) { // sotc: SOTC_TEX_BLOCK: the lookup as TryTextureBlock replays it
                const size_t index = image_bindings.size() - 1;
                if (!texture_cache.IsPerfectFind(image_id, desc.info)) {
                    store_ok = false; // an overlap resolution, not a perfect match
                }
                const VideoCore::Image& found = texture_cache.GetImage(image_id);
                // A mip-tail decision (DecideRebase, MemoLookup) depends on the whole image set and
                // on the GPU-written tails: such a block is checked against both (TryTextureBlock).
                rec->candidate = rec->candidate || found.info.sotc_first_mip != 0 ||
                                 (desc.rebase_mip != 0 && VideoCore::MipTailMode() != 0);
                if (index < MaxBlockEntries) {
                    rec->entries[index] = TexBlockEntry{};
                    rec->entries[index].found = image_id;
                    rec->entries[index].found_uid = found.image_uid;
                    rec->entries[index].addr = found.info.guest_address;
                    rec->entries[index].addr_gen =
                        texture_cache.ImageAddrGeneration(found.info.guest_address);
                } else {
                    store_ok = false;
                }
            }
            if (bind_cache_recording && SotcStats::kBindCacheFrame) {
                BindCacheNoteLookup(desc, image_id); // sotc: SOTC_BIND_CACHE_FRAME
            }
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
    for (size_t entry = 0; entry < image_bindings.size(); ++entry) {
        auto& [image_id, desc] = image_bindings[entry];
        bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        if (!image_id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
            image_info_images[image_infos.size() - 1] = {};
        } else {
            if (auto& old_image = texture_cache.GetImage(image_id);
                old_image.binding.needs_rebind) {
                old_image.binding = {};
                store_ok = false; // sotc: SOTC_TEX_BLOCK: found again, not stored
                image_id = texture_cache.FindImage(desc);
                if (bind_cache_recording && SotcStats::kBindCacheFrame) {
                    BindCacheNoteLookup(desc, image_id); // sotc: SOTC_BIND_CACHE_FRAME
                }
            }

            bound_images.emplace_back(image_id);
            if (is_storage && SotcStats::kBindCacheRanges) {
                BindCacheInvalidateImage(image_id); // sotc: drops the binds sampling it
            }

            auto& image = texture_cache.GetImage(image_id);
            auto& image_view = texture_cache.FindTexture(image_id, desc);
            const auto binding = image.binding;

            // The image is either bound as storage in a separate descriptor or bound as render
            // target in feedback loop. Depth images are excluded because they can't be bound as
            // storage and feedback loop doesn't make sense for them
            if ((binding.force_general || binding.is_target) && !image.info.props.is_depth) {
                bind_cache_uncacheable = true; // sotc: SOTC_BIND_CACHE: feedback loop / storage
                store_ok = false;              // sotc: SOTC_TEX_BLOCK: a target binding
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
                    bind_cache_uncacheable = true; // sotc: SOTC_BIND_CACHE
                    needs_barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        desc.view_info.range);
                } else {
                    // sotc: the depth buffer the draw writes stencil to is also sampled (depth
                    // aspect only): DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL allows both, so the
                    // image stays in the layout the render target needs. Moving it to
                    // DEPTH_STENCIL_READ_ONLY here and back in BeginRendering ended the render
                    // pass at every such draw (~350 per frame in the Shrine). SOTC_DEPTH_RO=0 off.
                    if (DepthSampledAsAttachment(image_id, image, desc,
                                                 stage.sw_stage == Shader::SwStage::Compute)) {
                        bind_cache_uncacheable = true; // sotc: SOTC_BIND_CACHE: register-dependent
                        needs_barrier |= runtime.Transit(
                            &image, vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal,
                            kDepthReadOnlyStencilStages, kDepthReadOnlyStencilAccess,
                            desc.view_info.range);
                    } else {
                        const auto new_layout = image.info.props.is_depth
                                                    ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                                    : vk::ImageLayout::eShaderReadOnlyOptimal;
                        needs_barrier |= runtime.Transit(
                            &image, new_layout, vk::PipelineStageFlagBits2::eAllCommands,
                            vk::AccessFlagBits2::eShaderRead, desc.view_info.range);
                        if (bind_cache_recording) { // sotc: SOTC_BIND_CACHE replays it on a hit
                            BindCacheNoteImage(image_id, new_layout,
                                               vk::PipelineStageFlagBits2::eAllCommands,
                                               vk::AccessFlagBits2::eShaderRead,
                                               desc.view_info.range);
                        }
                    }
                }
            }
            // sotc: SOTC_TEX_BLOCK: the bound image, its view and the backing state the bind left.
            if (image.info.props.has_stencil) {
                store_ok = false; // its transition depends on the draw's depth registers
            }
            if (rec && entry < MaxBlockEntries) {
                TexBlockEntry& e = rec->entries[entry];
                e.bound = image_id;
                e.bound_uid = image.image_uid;
                e.redirect = image_id != e.found;
                e.range = desc.view_info.range;
                e.view = *image_view.image_view;
                e.backing = image.backing;
                e.version = image.backing->state_version;
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
        const u32 ui_i = CompositeUiIndex();
        const auto& ui = texture_cache.GetImage(image_bindings[ui_i].first);
        const auto& scene = texture_cache.GetImage(image_bindings[ui_i ^ 1].first);
        if (ui.usage.render_target && ui.info.pixel_format == vk::Format::eR8G8B8A8Srgb &&
            scene.info.pixel_format == vk::Format::eR16G16B16A16Sfloat) {
            hudless_ui_info = static_cast<s32>(first_image_idx + ui_i);
        }
    }

    // sotc: SOTC_DLSS_PROBE=1 logs each fragment shader that draws into a video out surface once:
    // its images (format, size, render target or not), to find the final composite.
    static const bool dlss_probe = std::getenv("SOTC_DLSS_PROBE") != nullptr;
    if (dlss_probe && stage.sw_stage == Shader::SwStage::Fragment && cb_descs[0].first &&
        texture_cache.GetImage(cb_descs[0].first).usage.vo_surface) {
        static std::unordered_set<std::string> seen;
        std::string line = fmt::format("{:#x}", stage.pgm_hash);
        {
            for (const auto& [id, desc] : image_bindings) {
                if (!id) {
                    line += " [none]";
                    continue;
                }
                const auto& img = texture_cache.GetImage(id);
                line += fmt::format(" [{} {}x{}{}]", vk::to_string(img.info.pixel_format),
                                    img.info.size.width, img.info.size.height,
                                    img.usage.render_target ? " rt" : "");
            }
            const auto& out = texture_cache.GetImage(cb_descs[0].first);
            if (seen.size() < 60 && seen.insert(line).second)
            std::fprintf(stderr, "SOTC DLSS PROBE: shader %#llx -> %s %ux%u, %zu images:%s\n",
                         static_cast<unsigned long long>(stage.pgm_hash),
                         vk::to_string(out.info.pixel_format).c_str(), out.info.size.width,
                         out.info.size.height, image_bindings.size(), line.c_str());
        }
    }

    // sotc: DLSS super resolution: the game's final composite samples the UI layer (an sRGB render
    // target) and the tonemapped fp16 scene, nothing else, into a video out surface.
    if (Dlssg::SuperResolution() && stage.sw_stage == Shader::SwStage::Fragment &&
        image_bindings.size() == 2 && image_bindings[0].first && image_bindings[1].first &&
        cb_descs[0].first) {
        const u32 ui_i = CompositeUiIndex();
        const auto& ui = texture_cache.GetImage(image_bindings[ui_i].first);
        const auto& scene = texture_cache.GetImage(image_bindings[ui_i ^ 1].first);
        if (texture_cache.GetImage(cb_descs[0].first).usage.vo_surface &&
            ui.usage.render_target && ui.info.pixel_format == vk::Format::eR8G8B8A8Srgb &&
            scene.info.pixel_format == vk::Format::eR16G16B16A16Sfloat) {
            bind_cache_uncacheable = true; // sotc: SOTC_BIND_CACHE
            Dlssg::NoteComposite(image_bindings[ui_i ^ 1].first, image_bindings[ui_i].first,
                                 texture_cache);
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
        set_write_sets[set_write_index] = set;
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

    const VideoCore::Image* ui_asset = UiLinearAsset(
        stage, image_bindings.size(),
        image_bindings.size() == 1 ? image_bindings[0].first : VideoCore::ImageId{});
    // sotc: SOTC_BIND_CACHE: the composite (TakeHudless, DLSS), the probe and the bilinear UI
    // filter depend on more than the cache key; those binds are not stored.
    if (ui_asset || hudless_ui_info >= 0 || dlss_probe) {
        bind_cache_uncacheable = true;
    }
    BindSamplers(stage, binding, set, ui_asset);

    // sotc: SOTC_TEX_BLOCK: store the block when TryTextureBlock can answer it (TexBlockMemo).
    if (rec) {
        const size_t num_entries = image_bindings.size();
        bool ok = store_ok && num_entries <= MaxBlockEntries &&
                  stage.images.size() <= MaxBlockImages &&
                  image_descriptor_array_sizes.size() == stage.images.size();
        if (ok && rec->candidate) {
            ok = texture_cache.BindGeneration() == rec->bind_generation &&
                 VideoCore::gpu_modified_epoch.load(std::memory_order_relaxed) == rec->gpu_epoch;
        }
        for (size_t i = 0; ok && i < num_entries; ++i) {
            const TexBlockEntry& e = rec->entries[i];
            if (e.null) {
                continue;
            }
            VideoCore::Image* found = texture_cache.TryGetImage(e.found, e.found_uid);
            ok = found && texture_cache.TryGetImage(e.bound, e.bound_uid) &&
                 texture_cache.ImageAddrGeneration(e.addr) == e.addr_gen &&
                 texture_cache.GetAssociatedDepth(*found) ==
                     (e.redirect ? e.bound : VideoCore::ImageId{});
        }
        rec->valid = ok;
        if (ok) {
            rec->num_entries = static_cast<u32>(num_entries);
            rec->num_descs = static_cast<u32>(stage.images.size());
            for (size_t d = 0; d < stage.images.size(); ++d) {
                rec->desc_counts[d] = static_cast<u8>(image_descriptor_array_sizes[d]);
            }
            tex_block_stores.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

const VideoCore::Image* Rasterizer::UiLinearAsset(const Shader::Info& stage, size_t num_bindings,
                                                  VideoCore::ImageId first) {
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
    if (!ui_linear || num_bindings != 1 || !first || stage.images.size() != 1 ||
        stage.images[0].array_size != 0) {
        return nullptr;
    }
    const auto& image = texture_cache.GetImage(first);
    const auto& u = image.usage;
    if (u.render_target || u.depth_target || u.storage || u.vo_surface || image.binding.is_target ||
        image.info.props.is_depth ||
        AmdGpu::IsInteger(stage.images[0].GetSharp(stage).GetNumberFmt())) {
        return nullptr;
    }
    return &image;
}

void Rasterizer::BindSamplers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                              u32 set, const VideoCore::Image* ui_asset) {
    const bool bindless = IsBindlessStage(instance, stage);
    const auto bindless_range = Shader::GetBindlessSamplerRange(stage.sw_stage);
    for (u32 sampler_index = 0; sampler_index < stage.samplers.size(); ++sampler_index) {
        const auto& sampler = stage.samplers[sampler_index];
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
        u32 heap_slot = 0;
        const auto vk_sampler = texture_cache.GetSampler(
            ssharp, liverpool->DrawRegs().ta_bc_base, sampler.is_depth, &heap_slot);
        if (bind_cache_recording && SotcStats::kBindCacheXtick) {
            // sotc: SOTC_BIND_CACHE_XTICK: touched again by the first hit of each later tick
            bind_cache_samplers_rec.push_back(
                {ssharp, vk_sampler, heap_slot, sampler.is_depth, bindless});
        }
        if (bindless) {
            // sotc: the shader reads the heap index from this slot of the push constants.
            const u32 slot = bindless_range.base + sampler_index;
            const u32 shift = (slot & 1) * 16;
            auto& word = bindless_slots.words[slot / 2];
            word = (word & ~(0xFFFFu << shift)) | (heap_slot << shift);
            continue;
        }
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        image_info_images[image_infos.size() - 1] = {};
        set_write_sets[set_write_index] = set;
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
    const SotcOpPhases::Scope ph{SotcOpPhases::PhBeginRendering}; // sotc: SOTC_OP_PHASES
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
    rs_verify_hit = RsMemoHit::Miss; // sotc: SOTC_RS_MEMO=3
    if (rs_memo_prepared) { // sotc: SOTC_RS_MEMO
        rs_memo_prepared = false;
        if (RsMemoBegin(state)) {
            return state;
        }
    } else if (rs_memo_verify) { // sotc: SOTC_RS_MEMO=3, the full path runs below
        rs_memo_verify = false;
        rs_verify_hit = RsMemoBeginCheck();
        rs_verify_pred = {};
        if (rs_verify_hit == RsMemoHit::Cross) {
            // sotc: the transitions a cross replay would skip, before the full path runs them.
            const auto note = [&](u32 slot, VideoCore::ImageId id, vk::ImageLayout layout,
                                  vk::AccessFlags2 access, const VideoCore::SubresourceRange& range) {
                if (!id) {
                    return;
                }
                const auto& image = texture_cache.GetImage(id);
                auto& pred = rs_verify_pred[slot];
                pred.noop = RsMemoTransitIsNoop(image, layout, access, range);
                pred.layout = image.backing->state.layout;
                pred.access = image.backing->state.access_mask;
            };
            for (u32 cb = 0; cb < rs_memo.state.num_color_attachments; ++cb) {
                note(cb, cb_descs[cb].first, vk::ImageLayout::eColorAttachmentOptimal,
                     vk::AccessFlagBits2::eColorAttachmentWrite |
                         vk::AccessFlagBits2::eColorAttachmentRead,
                     cb_descs[cb].second.view_info.range);
            }
            note(AmdGpu::NUM_COLOR_BUFFERS, db_desc.first, rs_memo.depth_layout,
                 rs_memo.depth_access, db_desc.second.view_info.range);
        }
    }
    bool rs_cb0_base = false;
    vk::ImageLayout rs_depth_layout{};
    vk::PipelineStageFlags2 rs_depth_stages{};
    vk::AccessFlags2 rs_depth_access{};
    state.width = instance.GetMaxFramebufferWidth();
    state.height = instance.GetMaxFramebufferHeight();
    state.num_layers = std::numeric_limits<u16>::max();
    state.num_color_attachments = std::bit_width(key.mrt_mask);
    // Same count as the pipeline's rasterizationSamples (vk_graphics_pipeline.cpp).
    state.samples = LiverpoolToVK::NumSamples(
        key.num_samples, instance.GetColorSampleCounts() & instance.GetDepthSampleCounts());
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
        state.color_formats[cb] = image_view.info.format;

        image->usage.render_target = 1u;
        if (cb == 0 && image->usage.vo_surface) { // sotc: TakeHudless
            hudless_vo = image->info.guest_address;
            hudless_vo_format = image_view.info.format;
            hudless_vo_width = image->info.size.width;
            hudless_vo_height = image->info.size.height;
        }
        rs_cb0_base |= cb == 0 && mip == 0 && slice == 0; // sotc: SOTC_RS_MEMO
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
        static const bool depth_ro = [] {
            const char* v = std::getenv("SOTC_DEPTH_RO");
            return !v || v[0] != '0';
        }();
        // sotc: the stages and access are kept for SOTC_RS_MEMO (same Transit as before).
        const bool depth_ro_layout =
            depth_ro && new_layout == vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal;
        rs_depth_layout = new_layout;
        rs_depth_stages = depth_ro_layout ? kDepthReadOnlyStencilStages
                                          : vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                                vk::PipelineStageFlagBits2::eLateFragmentTests;
        rs_depth_access = depth_ro_layout ? kDepthReadOnlyStencilAccess
                                          : vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                                                vk::AccessFlagBits2::eDepthStencilAttachmentRead;
        needs_barrier |= runtime.Transit(&image, new_layout, rs_depth_stages, rs_depth_access,
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
        // Same aspects as the Scheduler's pDepthAttachment / pStencilAttachment.
        state.depth_format = attachment.has_depth ? image_view.info.format : vk::Format{};
        state.stencil_format = attachment.has_stencil ? image_view.info.format : vk::Format{};

        image.usage.depth_target = true;
    } else {
        state.depth_stencil_attachment = {};
    }

    if (state.num_layers == std::numeric_limits<u16>::max()) {
        state.num_layers = 1;
    }

    if (rs_verify_hit != RsMemoHit::Miss) { // sotc: SOTC_RS_MEMO=3, before the memo is replaced
        RsMemoVerify(state, rs_verify_hit);
        rs_verify_hit = RsMemoHit::Miss;
    }
    if (RsMemoEnabled()) { // sotc: SOTC_RS_MEMO: kept unless the pass clears or loops back
        bool storable = !attachment_feedback_loop;
        for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
            storable &= !state.color_attachments[cb].is_clear;
        }
        const auto& ds = state.depth_stencil_attachment;
        storable &= !ds.depth_clear && !ds.stencil_clear;
        rs_memo.valid = storable;
        if (storable) {
            rs_memo.key_len = rs_key_len;
            std::memcpy(rs_memo.key.data(), rs_key.data(), rs_key_len * sizeof(u32));
            rs_memo.cb0_base = rs_cb0_base;
            rs_memo.color_samples = key.color_samples;
            rs_memo.tick = scheduler.CurrentTick();
            rs_memo.texture_generation = texture_cache.BindGeneration();
            rs_memo.meta_epoch = texture_cache.MetaEpoch();
            rs_memo.state = state;
            rs_memo.depth_layout = rs_depth_layout;
            rs_memo.depth_stages = rs_depth_stages;
            rs_memo.depth_access = rs_depth_access;
            // sotc: SOTC_RS_MEMO=2, the images of this draw's targets (id, uid, backing).
            for (u32 slot = 0; slot < rs_memo.target_id.size(); ++slot) {
                const bool is_depth = slot == AmdGpu::NUM_COLOR_BUFFERS;
                const auto id = is_depth ? db_desc.first
                                : slot < state.num_color_attachments ? cb_descs[slot].first
                                                                     : VideoCore::ImageId{};
                rs_memo.target_id[slot] = id;
                rs_memo.target_uid[slot] = 0;
                rs_memo.target_backing[slot] = nullptr;
                if (id) {
                    const auto& image = texture_cache.GetImage(id);
                    rs_memo.target_uid[slot] = image.image_uid;
                    rs_memo.target_backing[slot] = image.backing;
                }
            }
        } else if (SotcStats::kFpsLog) {
            SotcStats::rs_memo_unstored.fetch_add(1, std::memory_order_relaxed);
        }
    }

    return state;
}

void Rasterizer::Resolve() {
    const auto& mrt0_hint = liverpool->DrawCbExtent()[0];
    const auto& mrt1_hint = liverpool->DrawCbExtent()[1];
    VideoCore::TextureCache::ImageDesc mrt0_desc{liverpool->DrawRegs().color_buffers[0], mrt0_hint};
    VideoCore::TextureCache::ImageDesc mrt1_desc{liverpool->DrawRegs().color_buffers[1], mrt1_hint};
    auto& mrt0_image = texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, true));
    const auto mrt1_id = texture_cache.FindImage(mrt1_desc, true);
    auto& mrt1_image = texture_cache.GetImage(mrt1_id);

    ScopeMarkerBegin(fmt::format("Resolve:MRT0={:#x}:MRT1={:#x}",
                                 liverpool->DrawRegs().color_buffers[0].Address(),
                                 liverpool->DrawRegs().color_buffers[1].Address()));
    runtime.ResolveImage(&mrt0_image, &mrt1_image, mrt0_desc.view_info.range,
                         mrt1_desc.view_info.range);
    ScopeMarkerEnd();
    if (SotcStats::kBindCacheRanges) { // sotc: drops the binds reading the written image
        BindCacheInvalidateImage(mrt1_id);
    } else if (SotcStats::kBindCache) { // sotc: a GPU image write a cached texel buffer may alias
        bind_cache_write_epoch.fetch_add(1, std::memory_order_relaxed);
    }
}

void Rasterizer::DepthStencilCopy(bool is_depth, bool is_stencil) {
    auto& regs = liverpool->DrawRegs();
    // sotc: a GPU image write a cached texel buffer may alias (SOTC_BIND_CACHE_RANGES: the
    // written image is dropped below instead).
    if (SotcStats::kBindCache && !SotcStats::kBindCacheRanges) {
        bind_cache_write_epoch.fetch_add(1, std::memory_order_relaxed);
    }

    auto read_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->DrawDbExtent(), false);
    auto write_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->DrawDbExtent(), true);

    auto& read_image = texture_cache.GetImage(texture_cache.FindImage(read_desc));
    const auto write_id = texture_cache.FindImage(write_desc);
    auto& write_image = texture_cache.GetImage(write_id);
    BindCacheInvalidateImage(write_id); // sotc: SOTC_BIND_CACHE_RANGES (no-op otherwise)

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
    if (!is_gds) {
        texture_cache.InvalidateMemoryFromGPU(address, num_bytes, true);
    }
    if (SotcStats::kBindCacheRanges) {
        // sotc: drops the binds reading the range. GDS is not guest memory, and a hit replays
        // the GDS buffer's access check (barrier) as the full bind does.
        if (!is_gds) {
            BindCacheInvalidate(address, num_bytes);
        }
    } else if (SotcStats::kBindCache) { // sotc: a GPU write: no cached bind survives it
        bind_cache_write_epoch.fetch_add(1, std::memory_order_relaxed);
    }
    runtime.FillBuffer(buffer, offset, num_bytes, value);
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    if (!dst_gds && !buffer_cache.IsRegionGpuModified(dst, num_bytes)) {
        if (!src_gds && !buffer_cache.IsRegionGpuModified(src, num_bytes) &&
            !texture_cache.FindImageFromRange(src, num_bytes)) {
            // Both buffers were not transferred to GPU yet. Can safely copy in host memory.
            std::memcpy(std::bit_cast<void*>(dst), std::bit_cast<void*>(src), num_bytes);
            SotcStats::dma_cpu_copies.fetch_add(1, std::memory_order_relaxed);
            SotcStats::dma_cpu_bytes.fetch_add(num_bytes, std::memory_order_relaxed);
            return;
        }
    }
    if (!dst_gds) {
        texture_cache.InvalidateMemoryFromGPU(dst, num_bytes, true);
    }
    if (SotcStats::kBindCacheRanges) {
        if (!dst_gds) { // sotc: drops the binds reading the destination (GDS: see FillBuffer)
            BindCacheInvalidate(dst, num_bytes);
        }
    } else if (SotcStats::kBindCache) { // sotc: a GPU write: no cached bind survives it
        bind_cache_write_epoch.fetch_add(1, std::memory_order_relaxed);
    }
    SotcStats::dma_gpu_copies.fetch_add(1, std::memory_order_relaxed);
    SotcStats::dma_gpu_bytes.fetch_add(num_bytes, std::memory_order_relaxed);
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
    // sotc: the texture cache part is timed as the fault stage "tc" (SotcStats::FaultStat).
    if (VideoCore::FaultGrace::Pages() != 0) {
        {
            const SotcStats::FaultStat::StageScope tc{SotcStats::FaultStat::kStTc};
            texture_cache.InvalidateMemory(addr, size);
        }
        buffer_cache.InvalidateMemory(addr, size, false, true);
    } else {
        buffer_cache.InvalidateMemory(addr, size, false);
        {
            const SotcStats::FaultStat::StageScope tc{SotcStats::FaultStat::kStTc};
            texture_cache.InvalidateMemory(addr, size);
        }
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
    map_epoch.fetch_add(1, std::memory_order_acq_rel); // sotc: SOTC_PARALLEL_RECORD
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
    map_epoch.fetch_add(1, std::memory_order_acq_rel); // sotc: SOTC_PARALLEL_RECORD
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

void Rasterizer::PrepareDraw(const AmdGpu::Regs& regs, const Pipeline* resolved, bool is_indexed,
                             bool direct, PreparedDraw& out) const {
    // sotc: SOTC_PARALLEL_RECORD, on a GpuPrep thread: its own registers (replayed up to this op)
    // and its own Shader::g_info_slot, filled from the op like the draw thread's. Only the
    // register-only inputs the draw thread would compute the same way; the decisions stay there.
    out.valid = false;
    out.has_offsets = false;
    if (!resolved || resolved->IsCompute()) {
        return;
    }
    const auto* pipeline = static_cast<const GraphicsPipeline*>(resolved);
    // BindResources drops a draw with a stage without user data (GetDrawOffsets would read it).
    for (const auto* stage : pipeline->GetStages()) {
        if (stage && stage->UserData().data() == nullptr) {
            return;
        }
    }
    bool refuse = false;
    out.dynamic = DynamicValues{}; // the fields a disabled test leaves alone stay zero (verify)
    ComputeDynamic(regs, is_indexed, out.dynamic, &refuse);
    if (refuse) {
        return;
    }
    out.is_indexed = is_indexed;
    out.push_data = MakeUserData(regs);
    out.fast_static = FastBindStatic(pipeline, regs);
    out.cache_eligible = BindCacheEligible(pipeline, regs);
    out.fast_hash = FastBindHash(pipeline, out.push_data, regs);
    out.res_ok = SotcStats::kBindCacheFrame && out.cache_eligible &&
                 BindResourceHash(pipeline, out.res_hash);
    if (direct) {
        const auto& vs_info = pipeline->GetStage(Shader::SwStage::Vertex);
        const auto& fetch_shader = pipeline->GetFetchShader();
        const auto user_data = vs_info.UserData();
        const auto in_range = [&](s32 sgpr) {
            return sgpr == -1 || static_cast<u32>(sgpr) < user_data.size();
        };
        if (fetch_shader.Empty() ||
            (in_range(fetch_shader.vertex_offset_sgpr) &&
             in_range(fetch_shader.instance_offset_sgpr))) {
            std::tie(out.vertex_offset, out.instance_offset) =
                GetDrawOffsets(regs, vs_info, fetch_shader);
            out.has_offsets = true;
        }
    }
    out.valid = true;
    if (SotcPrResolveOn()) {
        // sotc: SOTC_PR_RESOLVE: after setup_stages (this op's flat copy) and MapEpoch (PrepLoop).
        PrepareResources(pipeline, out.resolved);
    }
}

void Rasterizer::PrepareResources(const GraphicsPipeline* pipeline, PreparedResources& out) const {
    // sotc: SOTC_PR_RESOLVE, on a GpuPrep thread. Resets everything it writes: the slot keeps the
    // previous op's data until it is overwritten.
    out.num_buffers = 0;
    out.num_images = 0;
    out.stages = {};
    // Read before anything else: an odd value is a change in progress, and BindResources accepts
    // the data only while the generation still equals this one.
    const u64 gen = runtime_memory_generation();
    out.runtime_gen = gen;
    // SOTC_BIND_CLAMP1=0 selects another validity path in BindBuffers: it stays inline.
    static const bool clamp1 = [] {
        const char* v = std::getenv("SOTC_BIND_CLAMP1");
        return !v || v[0] != '0';
    }();
    if ((gen & 1) || !clamp1) {
        return;
    }
    u32 stage_index = 0;
    for (const auto* stage : pipeline->GetStages()) {
        PrStage& ps = out.stages[stage_index++];
        if (!stage || !PrStageResolvable(*stage) ||
            out.num_buffers + stage->buffers.size() > PreparedResources::MaxBuffers ||
            out.num_images + stage->images.size() > PreparedResources::MaxImages) {
            continue;
        }
        ps.ready = true;
        ps.buf_first = out.num_buffers;
        ps.img_first = out.num_images;
        for (const auto& desc : stage->buffers) {
            out.buffers[out.num_buffers++] = PrResolveBuffer(*stage, desc, memory);
        }
        for (const auto& image : stage->images) {
            out.images[out.num_images++] = PrResolveImage(*stage, image, memory);
        }
    }
}

void Rasterizer::PrefetchImagesShared(const Pipeline* pipeline) {
    // sotc: SOTC_PARALLEL_RECORD: the prep threads take PrefetchImages off the command processor.
    // TextureCache::PrefetchTexture keeps per-T# state for a single caller, hence the lock.
    if (pipeline && texture_cache.ImagePrefetchEnabled()) {
        std::scoped_lock lock{prep_prefetch_mutex};
        PrefetchImagesImpl(pipeline);
    }
}

const PreparedDraw* Rasterizer::CheckPrepared(const PreparedDraw& prepared,
                                              const GraphicsPipeline* pipeline, bool is_indexed,
                                              bool direct) {
    // sotc: SOTC_PR_VERIFY=N: every Nth prepared draw is computed inline as well; a difference is
    // logged (first 16), counted, and the inline values are used.
    static const u32 verify_every = [] {
        const char* v = std::getenv("SOTC_PR_VERIFY");
        return v ? static_cast<u32>(std::strtoul(v, nullptr, 0)) : 0u;
    }();
    pr_verify_off = false; // sotc: SOTC_PR_RESOLVE (set below when the verify differs)
    if (prepared.is_indexed != is_indexed) {
        return nullptr; // cannot happen (same op); refused rather than trusted
    }
    if (verify_every == 0 || ++pr_verify_count % verify_every != 0) {
        return &prepared;
    }
    const auto& regs = liverpool->DrawRegs();
    DynamicValues dynamic;
    ComputeDynamic(regs, is_indexed, dynamic);
    const auto push = MakeUserData(regs);
    const bool fast_static = FastBindStatic(pipeline, regs);
    const bool cache_eligible = BindCacheEligible(pipeline, regs);
    const u64 fast_hash = FastBindHash(pipeline, push, regs);
    u32 mismatch = 0;
    mismatch |= SameDynamicValues(dynamic, prepared.dynamic) ? 0u : 1u;
    mismatch |= std::memcmp(&push, &prepared.push_data, sizeof(push)) == 0 ? 0u : 2u;
    mismatch |= fast_static == prepared.fast_static ? 0u : 4u;
    mismatch |= cache_eligible == prepared.cache_eligible ? 0u : 8u;
    mismatch |= fast_hash == prepared.fast_hash ? 0u : 16u;
    if (direct && prepared.has_offsets) {
        const auto offsets = GetDrawOffsets(regs, pipeline->GetStage(Shader::SwStage::Vertex),
                                            pipeline->GetFetchShader());
        mismatch |= offsets == std::pair<u32, u32>{prepared.vertex_offset,
                                                   prepared.instance_offset}
                        ? 0u
                        : 32u;
    }
    // sotc: SOTC_PR_RESOLVE: the resource inputs, recomputed here. A difference makes BindResources
    // resolve this draw inline; the return value is unchanged, so the other values fail separately.
    // A table generation that moved since the prep is not compared (BindResources rejects it).
    if (SotcPrResolveOn() && prepared.resolved.runtime_gen == runtime_memory_generation()) {
        PreparedResources fresh{};
        PrepareResources(pipeline, fresh);
        if (!SamePreparedResources(fresh, prepared.resolved)) {
            pr_verify_off = true;
            static std::atomic<u32> shown_resolved{0};
            if (shown_resolved.fetch_add(1, std::memory_order_relaxed) < 20) {
                std::fprintf(stderr,
                             "SOTC PR resolve mismatch (draw %llu): the draw resolves inline\n",
                             static_cast<unsigned long long>(pr_verify_count));
            }
        }
    }
    if (mismatch == 0) {
        return &prepared;
    }
    SotcStats::pr_verify_mismatches.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<u32> shown{0};
    if (shown.fetch_add(1, std::memory_order_relaxed) < 16) {
        std::fprintf(stderr,
                     "SOTC PR verify mismatch (draw %llu): mask %#x (1 dynamic, 2 push data, 4 fast "
                     "eligible, 8 cache eligible, 16 fast hash, 32 offsets), vs %#llx ps %#llx\n",
                     static_cast<unsigned long long>(pr_verify_count), mismatch,
                     static_cast<unsigned long long>(regs.vs_program.Address<uintptr_t>()),
                     static_cast<unsigned long long>(regs.ps_program.Address<uintptr_t>()));
    }
    return nullptr;
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed,
                                    const PreparedDraw* prepared) const {
    // sotc: the values are computed from the registers alone (ComputeDynamic, also run ahead by
    // SOTC_PARALLEL_RECORD's prep threads) and handed to DynamicState in the order of the former
    // Update*State calls (ApplyDynamic).
    if (prepared) {
        ApplyDynamic(prepared->dynamic, pipeline);
    } else {
        DynamicValues values;
        ComputeDynamic(liverpool->DrawRegs(), is_indexed, values);
        ApplyDynamic(values, pipeline);
    }

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.Commit(instance, scheduler.CommandBuffer());
}

void Rasterizer::ComputeDynamic(const AmdGpu::Regs& regs, const bool is_indexed,
                                DynamicValues& out, bool* refuse) const {
    ComputeViewportScissor(regs, out);
    ComputeDepthStencil(regs, out);
    ComputePrimitive(regs, is_indexed, out, refuse);
    out.line_width = regs.line_control.Width();
    out.blend_constants = regs.blend_constants;
}

void Rasterizer::ApplyDynamic(const DynamicValues& values, const GraphicsPipeline* pipeline) const {
    auto& dynamic_state = scheduler.GetDynamicState();
    // UpdateViewportScissorState
    dynamic_state.SetViewports(values.viewports);
    dynamic_state.SetScissors(values.scissors);
    // UpdateDepthStencilState
    dynamic_state.SetDepthTestEnabled(values.depth_test);
    if (values.depth_test) {
        dynamic_state.SetDepthWriteEnabled(values.depth_write);
        dynamic_state.SetDepthCompareOp(values.depth_compare);
    }
    dynamic_state.SetDepthBoundsTestEnabled(values.depth_bounds_test);
    if (values.depth_bounds_test) {
        dynamic_state.SetDepthBounds(values.depth_bounds_min, values.depth_bounds_max);
    }
    dynamic_state.SetDepthBiasEnabled(values.depth_bias);
    if (values.depth_bias) {
        dynamic_state.SetDepthBias(values.bias_constant, values.bias_clamp, values.bias_slope);
    }
    dynamic_state.SetStencilTestEnabled(values.stencil_test);
    if (values.stencil_test) {
        dynamic_state.SetStencilOps(values.stencil_front, values.stencil_back);
        dynamic_state.SetStencilReferences(values.stencil_ref_front, values.stencil_ref_back);
        dynamic_state.SetStencilWriteMasks(values.stencil_write_front, values.stencil_write_back);
        dynamic_state.SetStencilCompareMasks(values.stencil_compare_front,
                                             values.stencil_compare_back);
    }
    // UpdatePrimitiveState
    dynamic_state.SetPrimitiveRestartEnabled(values.prim_restart);
    dynamic_state.SetRasterizerDiscardEnabled(values.rasterizer_discard);
    dynamic_state.SetCullMode(values.cull_mode);
    dynamic_state.SetFrontFace(values.front_face);
    // UpdateRasterizationState
    dynamic_state.SetLineWidth(values.line_width);
    // UpdateColorBlendingState
    dynamic_state.SetBlendConstants(values.blend_constants);
    dynamic_state.SetColorWriteMasks(pipeline->GetGraphicsKey().write_masks);
    dynamic_state.SetAttachmentFeedbackLoopEnabled(attachment_feedback_loop);
}

void Rasterizer::ComputeViewportScissor(const AmdGpu::Regs& regs, DynamicValues& out) const {

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

    auto& viewports = out.viewports;
    auto& scissors = out.scissors;
    viewports.clear();
    scissors.clear();

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
}

void Rasterizer::ComputeDepthStencil(const AmdGpu::Regs& regs, DynamicValues& out) const {
    const auto depth_test_enabled =
        regs.depth_control.depth_enable && regs.depth_buffer.DepthValid();
    out.depth_test = depth_test_enabled;
    if (depth_test_enabled) {
        out.depth_write = regs.depth_control.depth_write_enable &&
                          !regs.depth_render_control.depth_clear_enable;
        out.depth_compare = LiverpoolToVK::CompareOp(regs.depth_control.depth_func);
    }

    const auto depth_bounds_test_enabled = regs.depth_control.depth_bounds_enable;
    out.depth_bounds_test = depth_bounds_test_enabled;
    if (depth_bounds_test_enabled) {
        out.depth_bounds_min = regs.depth_bounds_min;
        out.depth_bounds_max = regs.depth_bounds_max;
    }

    const auto depth_bias_enabled = regs.polygon_control.NeedsBias();
    out.depth_bias = depth_bias_enabled;
    if (depth_bias_enabled) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        out.bias_constant = front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset;
        out.bias_clamp = regs.poly_offset.depth_bias;
        out.bias_slope =
            (front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f;
    }

    const auto stencil_test_enabled =
        regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid();
    out.stencil_test = stencil_test_enabled;
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
        out.stencil_front = front_ops;
        out.stencil_back = back_ops;

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
        out.stencil_ref_front = front_op ? front.stencil_op_val : front.stencil_test_val;
        out.stencil_ref_back = back_op ? back.stencil_op_val : back.stencil_test_val;
        out.stencil_write_front = !stencil_clear ? front.stencil_write_mask : 0U;
        out.stencil_write_back = !stencil_clear ? back.stencil_write_mask : 0U;
        out.stencil_compare_front = front.stencil_mask;
        out.stencil_compare_back = back.stencil_mask;
    }
}

void Rasterizer::ComputePrimitive(const AmdGpu::Regs& regs, const bool is_indexed,
                                  DynamicValues& out, bool* refuse) const {

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
    // sotc: a prep thread (SOTC_PARALLEL_RECORD) refuses the draw instead of asserting: the
    // draw thread may still drop it, and asserts inline otherwise.
    const bool restart_supported = !is_indexed || !prim_restart ||
                                   regs.primitive_restart_index == 0xFFFF ||
                                   regs.primitive_restart_index == 0xFFFFFFFF;
    if (refuse && !restart_supported) {
        *refuse = true;
    }
    ASSERT_MSG(refuse || restart_supported,
               "Primitive restart index other than -1 is not supported yet");

    const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                               ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                               : vk::CullModeFlagBits::eNone;
    const auto front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);

    out.prim_restart = prim_restart;
    out.rasterizer_discard = regs.clipper_control.dx_rasterization_kill;
    out.cull_mode = cull_mode;
    out.front_face = front_face;
}

bool SameDynamicValues(const DynamicValues& a, const DynamicValues& b) {
    // sotc: SOTC_PR_VERIFY: bit equality of every field (the vectors' live elements only).
    const auto same = [](const auto& x, const auto& y) {
        return std::memcmp(&x, &y, sizeof(x)) == 0;
    };
    if (a.viewports.size() != b.viewports.size() || a.scissors.size() != b.scissors.size()) {
        return false;
    }
    for (size_t i = 0; i < a.viewports.size(); ++i) {
        if (!same(a.viewports[i], b.viewports[i]) || !same(a.scissors[i], b.scissors[i])) {
            return false;
        }
    }
    return a.depth_test == b.depth_test && a.depth_write == b.depth_write &&
           a.depth_bounds_test == b.depth_bounds_test && a.depth_bias == b.depth_bias &&
           a.stencil_test == b.stencil_test && a.prim_restart == b.prim_restart &&
           a.rasterizer_discard == b.rasterizer_discard && a.depth_compare == b.depth_compare &&
           same(a.depth_bounds_min, b.depth_bounds_min) &&
           same(a.depth_bounds_max, b.depth_bounds_max) &&
           same(a.bias_constant, b.bias_constant) && same(a.bias_clamp, b.bias_clamp) &&
           same(a.bias_slope, b.bias_slope) && a.stencil_front == b.stencil_front &&
           a.stencil_back == b.stencil_back && a.stencil_ref_front == b.stencil_ref_front &&
           a.stencil_ref_back == b.stencil_ref_back &&
           a.stencil_write_front == b.stencil_write_front &&
           a.stencil_write_back == b.stencil_write_back &&
           a.stencil_compare_front == b.stencil_compare_front &&
           a.stencil_compare_back == b.stencil_compare_back && a.cull_mode == b.cull_mode &&
           a.front_face == b.front_face && same(a.line_width, b.line_width) &&
           same(a.blend_constants, b.blend_constants);
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
