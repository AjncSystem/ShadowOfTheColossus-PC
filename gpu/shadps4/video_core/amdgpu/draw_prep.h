// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// sotc: SOTC_PARALLEL_RECORD=1 (default off). The per-draw inputs the GpuPrep threads compute
// ahead of the draw thread (Liverpool::PrepLoop, Rasterizer::PrepareDraw). Everything here is a
// pure function of the draw's registers, its pipeline and the per-draw shader data the command
// processor captured in the back op (user data, flattened SRT buffer): no cache lookups, no guest
// memory, no draw-thread state. The draw thread uses a slot only when it was published for its
// own op (ready_seq) and no mapping changed since (map_epoch); otherwise it computes inline.

#include <array>
#include <atomic>

#include "common/types.h"
#include "shader_recompiler/resource.h"
#include "video_core/amdgpu/op_footprint.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

/// The values Rasterizer::ApplyDynamic hands to DynamicState (the Update*State of the draw
/// path, split into a register-only computation and the setters). Fields the setters skip
/// (e.g. the compare op with the depth test off) stay zero.
struct DynamicValues {
    Viewports viewports;
    Scissors scissors;
    bool depth_test{};
    bool depth_write{};
    bool depth_bounds_test{};
    bool depth_bias{};
    bool stencil_test{};
    bool prim_restart{};
    bool rasterizer_discard{};
    vk::CompareOp depth_compare{};
    float depth_bounds_min{};
    float depth_bounds_max{};
    float bias_constant{};
    float bias_clamp{};
    float bias_slope{};
    StencilOps stencil_front{};
    StencilOps stencil_back{};
    u32 stencil_ref_front{};
    u32 stencil_ref_back{};
    u32 stencil_write_front{};
    u32 stencil_write_back{};
    u32 stencil_compare_front{};
    u32 stencil_compare_back{};
    vk::CullModeFlags cull_mode{};
    vk::FrontFace front_face{};
    float line_width{};
    std::array<float, 4> blend_constants{};
};

/// Field-wise bit equality (SOTC_PR_VERIFY): floats by their bits, the vectors by their sizes and
/// live elements only.
bool SameDynamicValues(const DynamicValues& a, const DynamicValues& b);

struct PreparedDraw {
    /// The back op sequence number this slot was published for; stored last (release).
    std::atomic<u64> ready_seq{~0ULL};
    /// Rasterizer::MapEpoch() read before the computation.
    u64 map_epoch{};
    /// false: the prep thread looked at the op but prepared nothing (not a graphics draw with a
    /// pipeline); the draw thread computes inline.
    bool valid{};
    bool is_indexed{};
    bool fast_static{};    ///< FastBindEligible without the feedback-loop test (draw thread)
    bool cache_eligible{}; ///< BindCacheEligible
    bool has_offsets{};    ///< direct draws: vertex_offset / instance_offset are set
    u32 vertex_offset{};
    u32 instance_offset{};
    u64 fast_hash{}; ///< FastBindHash(pipeline, push_data)
    /// SOTC_BIND_CACHE_FRAME: Rasterizer::BindResourceHash (the resource identity key of the bind
    /// cache; res_ok false: the pipeline binds what that key cannot name).
    u64 res_hash{};
    bool res_ok{};
    Shader::PushData push_data{};
    DynamicValues dynamic{};
    /// sotc: SOTC_CULL_EXPRESS. The shader-bound part of the op's guest memory footprint
    /// (AmdGpu::CollectResourceFootprint, op_footprint.h), filled by PrepareDraw (graphics) or by
    /// whoever prepares a dispatch, on a thread whose Shader::g_info_slot holds this op's data.
    /// resources_ready false: not computed (ComputeDrawFootprint then reports unknown).
    bool resources_ready{};
    u8 mrt_mask{0xFF}; ///< GraphicsPipeline::GetGraphicsKey().mrt_mask; all set = unknown
    AmdGpu::OpFootprint resources{};
};

} // namespace Vulkan
