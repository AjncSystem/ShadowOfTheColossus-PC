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
#include "shader_recompiler/runtime_info.h"
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

// sotc: SOTC_PR_RESOLVE (default off). The resource inputs of one graphics draw that a GpuPrep
// thread resolves from the op's flat copy and the runtime memory table (Rasterizer::PrepareResources).
// BindBuffers and BindTextures use them in place of the inline V#/T# decode and mapping checks.
// Valid only while PreparedResources::runtime_gen equals runtime_memory_generation() at bind time.

/// One buffer descriptor. Specials (flat buffer, GDS, ...) are never resolved: computed false.
struct PrBuffer {
    VAddr base{};    ///< V#.base_address
    u64 vsize{};     ///< V#.GetSize()
    u64 mapped{};    ///< ClampRangeSize(base, vsize); 0 when base or vsize is 0
    u64 size{};      ///< mapped, limited to the unbounded window: what BindBuffers binds
    bool computed{}; ///< false: BindBuffers reads this descriptor itself
};

/// One texture descriptor (never an array descriptor): the T# and the checks BindTextures makes.
struct PrImage {
    AmdGpu::Image tsharp{};
    u8 num_bindings{1}; ///< ImageResource::NumBindings
    /// 0 not resolved (inline), 1 bindable, 2 null (address 0 or invalid format),
    /// 3 rejected (unmapped address or bad enum).
    u8 verdict{};
};

/// The prepared entries of one stage (its position in GraphicsPipeline::GetStages()).
struct PrStage {
    bool ready{};   ///< false: the stage resolves everything inline
    u8 buf_first{}; ///< index of its first PrBuffer
    u8 img_first{}; ///< index of its first PrImage
};

struct PreparedResources {
    static constexpr u32 MaxBuffers = 48; ///< all stages together; past it a stage stays inline
    static constexpr u32 MaxImages = 48;
    u64 runtime_gen{}; ///< runtime_memory_generation() before the computation; odd: unusable
    u8 num_buffers{};
    u8 num_images{};
    std::array<PrStage, Shader::MaxStageTypes> stages{};
    std::array<PrBuffer, MaxBuffers> buffers{};
    std::array<PrImage, MaxImages> images{};
};
static_assert(sizeof(PreparedResources) <= 4096, "sotc: SOTC_PR_RESOLVE slot size");

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
    /// sotc: SOTC_PR_RESOLVE. Written by Rasterizer::PrepareResources at the end of PrepareDraw;
    /// used by BindResources only while runtime_gen matches (see above).
    PreparedResources resolved{};
};

} // namespace Vulkan
