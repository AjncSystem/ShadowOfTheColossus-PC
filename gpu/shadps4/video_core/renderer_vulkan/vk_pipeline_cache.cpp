// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <cstring>
#include <ranges>
#include <xxhash.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>
#include "common/elf_info.h"
#include "common/assert_scope.h"
#include "common/hash.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/sotc_stats.h"
#include "bbport_threads.h"

// sotc: generation of the runtime memory table (src/runtime_memory.c); odd while a change is in
// progress. Same declaration as vk_rasterizer.cpp and sotc_bininfo.cpp.
extern "C" uint64_t runtime_memory_generation(void);

namespace Vulkan {

// sotc: the saved driver pipeline cache (PipelineCache::SaveDriverCache).
static bool DriverCacheEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_DRIVER_CACHE");
        return !v || v[0] != '0';
    }();
    return enabled;
}

static std::filesystem::path DriverCachePath() {
    const auto dir = Common::FS::GetUserPath(Common::FS::PathType::CacheDir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / "sotc_vk_pipelines.bin";
}

using Shader::HwStage;
using Shader::Output;
using Shader::SwStage;

constexpr static auto SpirvVersion1_6 = 0x00010600U;

constexpr static std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 512},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
};

static u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    u32 num_outputs = 0;

    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }

    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Output::CullDist0 : Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Output::CullDist1 : Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Output::CullDist2 : Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Output::CullDist3 : Output::None);
    }

    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Output::CullDist4 : Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Output::CullDist5 : Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Output::CullDist6 : Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Output::CullDist7 : Output::None);
    }

    return num_outputs;
}

// sotc: the compute part of BuildRuntimeInfo, shared with PrefetchComputeProgram (which builds it
// from a scanned copy of the registers).
static void FillComputeRuntimeInfo(Shader::RuntimeInfo& info, const AmdGpu::ComputeProgram& cs_pgm) {
    info.props.num_user_data = cs_pgm.settings.num_user_regs;
    info.props.num_allocated_vgprs = cs_pgm.settings.num_vgprs * 4;
    info.props.fp_denorm_mode32 = cs_pgm.settings.fp_denorm_mode32;
    info.props.fp_denorm_mode16_64 = cs_pgm.settings.fp_denorm_mode64;
    info.props.fp_round_mode32 = cs_pgm.settings.fp_round_mode32;
    info.props.fp_round_mode16_64 = cs_pgm.settings.fp_round_mode64;
    info.hw.cs.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                 cs_pgm.num_thread_z.full};
    info.hw.cs.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                              cs_pgm.IsTgidEnabled(2)};
    info.hw.cs.shared_memory_size = cs_pgm.SharedMemSize();
}

const Shader::RuntimeInfo& PipelineCache::BuildRuntimeInfo(HwStage stage, SwStage l_stage) {
    auto& info = runtime_infos[u32(l_stage)];
    const auto& regs = liverpool->regs;
    const auto BuildCommon = [&](const auto& program) {
        info.props.num_user_data = program.settings.num_user_regs;
        info.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.props.num_allocated_vgprs = program.NumVgprs();
        info.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = program.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
    };
    info.Initialize(stage, l_stage);
    switch (stage) {
    case HwStage::Local: {
        BuildCommon(regs.ls_program);
        Shader::TessellationDataConstantBuffer tess_constants{};
        const auto* hull_info = infos[u32(SwStage::TessellationControl)];
        hull_info->ReadTessConstantBuffer(tess_constants);
        info.hw.ls.ls_stride = tess_constants.ls_stride;
        break;
    }
    case HwStage::Hull:
        BuildCommon(regs.hs_program);
        break;
    case HwStage::Export:
        BuildCommon(regs.es_program);
        info.hw.es.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        break;
    case HwStage::Geometry: {
        BuildCommon(regs.gs_program);
        info.hw.gs.num_outputs = MapOutputs(info.hw.gs.outputs, regs.vs_output_control);
        info.hw.gs.output_vertices = regs.vgt_gs_max_vert_out;
        info.hw.gs.num_invocations =
            regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
        if (regs.stage_enable.raw == AmdGpu::ShaderStageEnable::LsHsEsGs) {
            info.hw.gs.in_primitive = [&]() {
                switch (regs.tess_config.topology) {
                case AmdGpu::TessellationTopology::Point:
                    return AmdGpu::PrimitiveType::PointList;
                case AmdGpu::TessellationTopology::Line:
                    return AmdGpu::PrimitiveType::LineList;
                case AmdGpu::TessellationTopology::TriangleCw:
                case AmdGpu::TessellationTopology::TriangleCcw:
                    return AmdGpu::PrimitiveType::TriangleList;
                default:
                    UNREACHABLE();
                }
            }();
        } else {
            info.hw.gs.in_primitive = regs.primitive_type;
        }
        for (u32 stream_id = 0; stream_id < Shader::GsMaxOutputStreams; ++stream_id) {
            info.hw.gs.out_primitive[stream_id] =
                regs.vgt_gs_out_prim_type.GetPrimitiveType(stream_id);
        }
        info.hw.gs.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
        info.hw.gs.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
        info.hw.gs.mode = regs.vgt_gs_mode.mode;
        const auto params_vc = AmdGpu::GetParams(regs.vs_program);
        info.hw.gs.vs_copy = params_vc.code;
        info.hw.gs.vs_copy_hash = params_vc.hash;
        DumpShader(info.hw.gs.vs_copy, info.hw.gs.vs_copy_hash, Shader::HwStage::Vertex, 0,
                   "copy.bin");
        break;
    }
    case HwStage::Vertex: {
        BuildCommon(regs.vs_program);
        info.hw.vs.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
        info.hw.vs.num_outputs = MapOutputs(info.hw.vs.outputs, regs.vs_output_control);
        info.hw.vs.emulate_depth_negative_one_to_one =
            !instance.IsDepthClipControlSupported() &&
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        info.hw.vs.clip_disable = regs.IsClipDisabled();
        break;
    }
    case HwStage::Fragment: {
        BuildCommon(regs.ps_program);
        info.hw.fs.en_flags = regs.ps_input_ena;
        info.hw.fs.addr_flags = regs.ps_input_addr;
        info.hw.fs.num_inputs = regs.num_interp;
        info.hw.fs.front_face_all_bits = regs.barycentric_control.front_face_all_bits;
        info.hw.fs.num_samples =
            regs.ps_input_addr.sample_coverage_ena && regs.ps_input_ena.sample_coverage_ena
                ? regs.aa_config.NumSamples()
                : 1;
        info.hw.fs.z_export_format = regs.z_export_format;
        u8 stencil_ref_export_enable = regs.depth_shader_control.stencil_op_val_export_enable |
                                       regs.depth_shader_control.stencil_test_val_export_enable;
        info.hw.fs.mrtz_mask = regs.depth_shader_control.z_export_enable |
                               (stencil_ref_export_enable << 1) |
                               (regs.depth_shader_control.mask_export_enable << 2) |
                               (regs.depth_shader_control.coverage_to_mask_enable << 3);
        // GCN tests depth/stencil before the pixel shader when Z_ORDER asks for it, so fragments
        // that fail never run. A Vulkan shader with storage writes needs EarlyFragmentTests for
        // that; without it the writes of failing fragments land too (SotC deferred decals paint
        // their whole box footprint into the G-buffer: rectangles on the ground). Not with a
        // depth, stencil ref or sample mask export (mrtz_mask bits 0-2), the tests need them.
        // From Pink-shadPS4 (luizgustavs). SotC only (EU and US); SOTC_EARLY_Z=0/1 overrides.
        static const bool early_z_enabled = [] {
            const char* v = std::getenv("SOTC_EARLY_Z");
            const bool on = v ? std::atoi(v) != 0 : Common::ElfInfo::Instance().IsSotc();
            std::fprintf(stderr, "SOTC early fragment tests: %s\n", on ? "on" : "off");
            return on;
        }();
        const auto z_order = regs.depth_shader_control.z_order;
        info.hw.fs.early_fragment_tests =
            early_z_enabled &&
            (z_order == AmdGpu::ZOrder::EarlyZLateZ || z_order == AmdGpu::ZOrder::EarlyZReZ) &&
            (info.hw.fs.mrtz_mask & 0b111) == 0;
        const auto& cb0_blend = regs.blend_control[0];
        if (cb0_blend.enable) {
            info.hw.fs.dual_source_blending =
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_dst_factor) ||
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_src_factor);
            if (cb0_blend.separate_alpha_blend) {
                info.hw.fs.dual_source_blending |=
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_dst_factor) ||
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_src_factor);
            }
        } else {
            info.hw.fs.dual_source_blending = false;
        }
        const auto& ps_inputs = regs.ps_inputs;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.hw.fs.inputs[i] = {
                .param_index = u8(ps_inputs[i].input_offset),
                .is_default = bool(ps_inputs[i].use_default),
                .is_flat = bool(ps_inputs[i].flat_shade),
                .default_value = u8(ps_inputs[i].default_value),
            };
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {
            info.hw.fs.color_buffers[i] = graphics_key.color_buffers[i];
        }
        // Lowered user clip planes ride the same emulation path as guest-exported distances, so
        // the fragment side arms whenever the hardware vertex stage lowers them, keeping its input
        // locations in sync with the shifted vertex outputs.
        const bool lowers_user_clip_planes =
            regs.clipper_control.user_clip_plane_enable &&
            !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Geometry));
        info.hw.fs.clip_distance_emulation =
            ((regs.vs_output_control.clip_distance_enable &&
              !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Local))) ||
             lowers_user_clip_planes) &&
            profile.needs_clip_distance_emulation;
        break;
    }
    case HwStage::Compute: {
        FillComputeRuntimeInfo(info, liverpool->GetCsRegs());
        break;
    }
    default:
        break;
    }
    switch (l_stage) {
    case SwStage::Vertex:
        info.sw.vs.step_rate_0 = regs.vgt_instance_step_rate_0;
        info.sw.vs.step_rate_1 = regs.vgt_instance_step_rate_1;
        info.sw.vs.vertex_sgpr_offset = draw_indirect_params.vertex_sgpr_offset;
        info.sw.vs.instance_sgpr_offset = draw_indirect_params.instance_sgpr_offset;
        info.sw.vs.tess_emulated_primitive =
            regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
            regs.primitive_type == AmdGpu::PrimitiveType::QuadList;
        break;
    case SwStage::TessellationControl: {
        info.sw.tcs.num_input_control_points = regs.ls_hs_config.hs_input_control_points;
        info.sw.tcs.num_threads = regs.ls_hs_config.hs_output_control_points;
        info.sw.tcs.tess_type = regs.tess_config.type;
        info.sw.tcs.offchip_lds_enable = regs.hs_program.settings.oc_lds_en;
        break;
    }
    case SwStage::TessellationEval: {
        info.sw.tes.tess_type = regs.tess_config.type;
        info.sw.tes.tess_topology = regs.tess_config.topology;
        info.sw.tes.tess_partitioning = regs.tess_config.partitioning;
        break;
    }
    default:
        break;
    }
    return info;
}

// sotc: SOTC_ASYNC_SHADERS=0 builds every graphics pipeline on the command processor, as
// before (draws are never skipped for a pipeline being built).
static bool AsyncShadersEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_ASYNC_SHADERS");
        return !v || v[0] != '0';
    }();
    return enabled;
}

// sotc: SOTC_ASYNC_TRANSLATE=0 translates every graphics shader on the command processor (see
// AsyncPipelineJob::Translate).
static bool AsyncTranslateEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_ASYNC_TRANSLATE");
        return !v || v[0] != '0';
    }();
    return enabled;
}

// sotc: SOTC_ASYNC_COMPUTE=0 builds compute pipelines on the command processor (see
// AsyncPipelineJob).
static bool AsyncComputeEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_ASYNC_COMPUTE");
        return !v || v[0] != '0';
    }();
    return enabled;
}

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_, u32 sparse_page_shift)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      desc_heap{instance, scheduler, DescriptorHeapSizes}, layout_cache{instance} {
    const auto& vk12_props = instance.GetVk12Properties();
    // NVIDIA's shader compiler (seen on 617.14) hits a breakpoint on any OpFDiv in a shader
    // with RoundingModeRTZ for that width, killing the process. Prefer default rounding there.
    const bool rtz_usable = instance.GetDriverID() != vk::DriverId::eNvidiaProprietary;
    // sotc: the LDS barriers are also needed on AMD running the shaders 64 wide. GCN (RX 470-590,
    // Vega) has no 32-wide waves, so the 32-wide path never engages there, and its compiler may
    // move LDS reads before other lanes' writes just the same: white, flickering characters and
    // smoke, fog colours that changed every run (issues #152, #187, #201). The unofficial
    // SotC-AMD-Fix layer (raulbezerro-dev) fixed it there by reporting the NVIDIA driver, which
    // turns these barriers on. SOTC_AMD64_LDS=0 turns it off.
    const bool amd64_lds = [&] {
        const char* v = std::getenv("SOTC_AMD64_LDS");
        return (!v || std::atoi(v) != 0) && instance.GetVendorID() == 0x1002 &&
               instance.SubgroupSize() >= 64;
    }();
    if (amd64_lds) {
        std::fprintf(stderr, "GPU: LDS barriers on for 64-wide AMD (SOTC_AMD64_LDS=0: off)\n");
    }
    // Wave64 uniform branches on every driver. Tested on a Radeon RX 6650 XT (32-wide path):
    // off, the shadows next to the Wander and Agro on grass get horizontal stripes; on, the
    // image is right and the characters are not black in the shade (save with 5 colossi).
    Shader::Optimization::SetWave64UniformBranchesDefault(true);
    // sotc: SotC interpolates most pixel shader inputs by hand from their three vertex values.
    // Without fragment shader barycentrics (NVIDIA GTX 10xx: no textures, black geometry, issues
    // #57/#68/#102) an auxiliary geometry shader provides them (SOTC_BARYCENTRIC_GS=0 turns it
    // off). SOTC_NO_BARYCENTRIC=1 acts as if the GPU had neither extension, to test that path.
    const auto env_is = [](const char* name, const char* value) {
        const char* v = std::getenv(name);
        return v && std::strcmp(v, value) == 0;
    };
    const bool no_barycentric = env_is("SOTC_NO_BARYCENTRIC", "1");
    const bool khr_barycentric = instance_.IsFragmentShaderBarycentricSupported() && !no_barycentric;
    const bool amd_explicit_vertex =
        instance_.IsAmdShaderExplicitVertexParameterSupported() && !no_barycentric;
    const bool barycentric_via_gs = !khr_barycentric && !amd_explicit_vertex &&
                                    instance.IsGeometryStageSupported() &&
                                    instance.GetDriverID() == vk::DriverId::eNvidiaProprietary &&
                                    !env_is("SOTC_BARYCENTRIC_GS", "0");
    if (barycentric_via_gs) {
        LOG_WARNING(Render_Vulkan, "No fragment shader barycentrics: emulated by a geometry shader");
    }
    GraphicsPipeline::SerializationSupport::with_gs = barycentric_via_gs;
    profile = Shader::Profile{
        .max_viewport_width = instance.GetMaxViewportWidth(),
        .max_viewport_height = instance.GetMaxViewportHeight(),
        .max_shared_memory_size = instance.MaxComputeSharedMemorySize(),
        .supported_spirv = SpirvVersion1_6,
        .subgroup_size = instance.SubgroupSize(),
        .sparse_page_shift = sparse_page_shift,
        .support_int8 = instance.IsShaderInt8Supported(),
        .support_int16 = instance.IsShaderInt16Supported(),
        .support_int64 = instance.IsShaderInt64Supported(),
        .support_float16 = instance.IsShaderFloat16Supported(),
        .support_float64 = instance.IsShaderFloat64Supported(),
        .supports_denorm_behavior_independence =
            vk12_props.denormBehaviorIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .supports_rounding_mode_independence =
            vk12_props.roundingModeIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .support_fp16_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat16),
        .support_fp16_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat16),
        .support_fp16_round_to_zero = rtz_usable && bool(vk12_props.shaderRoundingModeRTZFloat16),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_fp32_round_to_zero = rtz_usable && bool(vk12_props.shaderRoundingModeRTZFloat32),
        .support_fp64_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat64),
        .support_fp64_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat64),
        .support_fp64_round_to_zero = rtz_usable && bool(vk12_props.shaderRoundingModeRTZFloat64),
        .support_fp16_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat16),
        .support_fp32_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat32),
        .support_fp64_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat64),
        .supports_image_load_store_lod = instance_.IsImageLoadStoreLodSupported(),
        .supports_native_cube_calc = instance_.IsAmdGcnShaderSupported(),
        .supports_trinary_minmax = instance_.IsAmdShaderTrinaryMinMaxSupported(),
        .supports_buffer_fp32_atomic_min_max =
            instance_.IsShaderAtomicFloatBuffer32MinMaxSupported(),
        .supports_image_fp32_atomic_min_max = instance_.IsShaderAtomicFloatImage32MinMaxSupported(),
        .supports_buffer_int64_atomics = instance_.IsBufferInt64AtomicsSupported(),
        .supports_shared_int64_atomics = instance_.IsSharedInt64AtomicsSupported(),
        .supports_workgroup_explicit_memory_layout =
            instance_.IsWorkgroupMemoryExplicitLayoutSupported(),
        .supports_amd_shader_explicit_vertex_parameter = amd_explicit_vertex,
        .supports_fragment_shader_barycentric = khr_barycentric || barycentric_via_gs,
        .supports_shader_subgroup_clock = instance_.IsShaderSubgroupClockSupported(),
        .needs_manual_interpolation = khr_barycentric &&
                                      instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        // sotc: also whenever the shaders are lowered for fewer than 64 lanes (AMD on the 32-wide
        // path): without the barriers the GPU hung ~2 min into the intro (issue #7).
        .needs_lds_barriers = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                              instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp ||
                              instance.SubgroupSize() < 64 || amd64_lds,
        .needs_buffer_offsets = instance.StorageMinAlignment() > 4,
        .needs_unorm_fixup = instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_clip_distance_emulation = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .supports_shader_stencil_export = instance_.IsShaderStencilExportSupported(),
        .barycentric_via_gs = barycentric_via_gs,
        // sotc: only with a set per stage fitting the device (MaxStageDescriptorSets).
        .stage_bindings = Shader::StageBindingsEnabled() &&
                          instance.GetMaxBoundDescriptorSets() >= Shader::MaxStageDescriptorSets,
    };
    // sotc: the cache is read by the Presenter once it can draw (Presenter::Presenter), with
    // the loading screen on meanwhile.

    // sotc: the VkPipelineCache starts from the copy saved on disk (SaveDriverCache). Without it
    // every start after an update of the port recompiled all ~3000 pipelines in the driver: about
    // 2 minutes of loading screen, since the driver's own cache is tied to the executable. The
    // driver checks the header (vendor, device, driver version) and ignores a stale copy.
    // SOTC_DRIVER_CACHE=0 turns it off.
    std::vector<u8> driver_cache_data;
    if (DriverCacheEnabled()) {
        const auto path = DriverCachePath();
        if (std::FILE* f = _wfopen(path.c_str(), L"rb")) {
            std::fseek(f, 0, SEEK_END);
            const long size = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (size > 0) {
                driver_cache_data.resize(static_cast<size_t>(size));
                if (std::fread(driver_cache_data.data(), 1, driver_cache_data.size(), f) !=
                    driver_cache_data.size()) {
                    driver_cache_data.clear();
                }
            }
            std::fclose(f);
        }
    }
    vk::PipelineCacheCreateInfo cache_ci{
        .initialDataSize = driver_cache_data.size(),
        .pInitialData = driver_cache_data.empty() ? nullptr : driver_cache_data.data(),
    };
    auto [cache_result, cache] = instance.GetDevice().createPipelineCacheUnique(cache_ci);
    if (cache_result != vk::Result::eSuccess && !driver_cache_data.empty()) {
        std::fprintf(stderr, "SOTC driver cache: saved copy rejected (%s), starting empty\n",
                     vk::to_string(cache_result).c_str());
        driver_cache_data.clear();
        auto retry = instance.GetDevice().createPipelineCacheUnique({});
        cache_result = retry.result;
        cache = std::move(retry.value);
    }
    ASSERT_MSG(cache_result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
               vk::to_string(cache_result));
    pipeline_cache = std::move(cache);
    saved_driver_cache_size = driver_cache_data.size();
    if (!driver_cache_data.empty()) {
        std::fprintf(stderr, "SOTC driver cache: %zu KB loaded\n", driver_cache_data.size() >> 10);
    }
    if (DriverCacheEnabled()) {
        // New pipelines (first visits) are saved every minute while playing: the game is often
        // closed by killing the process, which skips the destructor.
        driver_cache_saver = std::jthread([this](std::stop_token stop) {
            std::mutex m;
            std::condition_variable_any cv;
            while (!stop.stop_requested()) {
                std::unique_lock lk{m};
                cv.wait_for(lk, stop, std::chrono::seconds{60}, [] { return false; });
                if (!stop.stop_requested()) {
                    SaveDriverCache();
                }
            }
        });
    }

    // sotc: pipeline workers for SOTC_ASYNC_SHADERS (see AsyncPipelineJob).
    if (AsyncShadersEnabled()) {
        // SOTC_PIPELINE_WORKERS=<n> sets the number of workers. Default: the hardware threads
        // less four, 2 to 12. They are busy only on a first visit, while the game's own threads
        // mostly wait for them (cold caches, attract demo, 16 threads: worst frame 4.8 s with 6
        // workers, 3.4 s with 12; one worker, the old default on 4 threads: 18 s).
        // sotc: with SOTC_CPUSETS=1 the default is capped at 4 (see BbThreads::PlaceThread).
        const char* workers = std::getenv("SOTC_PIPELINE_WORKERS");
        const int threads = static_cast<int>(std::thread::hardware_concurrency());
        const int default_cap = BbThreads::CpuSetsEnabled() ? 4 : 12;
        const unsigned count = static_cast<unsigned>(
            workers ? std::clamp(std::atoi(workers), 1, 16) : std::clamp(threads - 4, 2, default_cap));
        for (unsigned i = 0; i < count; ++i) {
            async_workers.emplace_back([this](std::stop_token token) { AsyncPipelineWorker(token); });
        }
    }
    // sotc: SOTC_ASYNC_COMPUTE_XLATE=1 (off by default), see ComputePrefetchEnabled.
    compute_prefetch = !async_workers.empty() && [] {
        const char* v = std::getenv("SOTC_ASYNC_COMPUTE_XLATE");
        return v && v[0] == '1';
    }();
}

PipelineCache::~PipelineCache() {
    if (driver_cache_saver.joinable()) {
        driver_cache_saver.request_stop();
        driver_cache_saver.join();
        SaveDriverCache();
    }
}

void PipelineCache::SaveDriverCache() {
    if (!DriverCacheEnabled() || !pipeline_cache) {
        return;
    }
    std::scoped_lock lk{driver_cache_save_mutex};
    size_t size = 0;
    const vk::Device device = instance.GetDevice();
    if (device.getPipelineCacheData(*pipeline_cache, &size, nullptr) != vk::Result::eSuccess ||
        size == 0 || size == saved_driver_cache_size) {
        return;
    }
    std::vector<u8> data(size);
    if (device.getPipelineCacheData(*pipeline_cache, &size, data.data()) !=
        vk::Result::eSuccess) {
        return;
    }
    data.resize(size);
    // Written next to it and renamed: a crash while writing keeps the old copy.
    const auto path = DriverCachePath();
    auto tmp = path;
    tmp += L".tmp";
    std::FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) {
        return;
    }
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    std::error_code ec;
    if (ok) {
        std::filesystem::rename(tmp, path, ec);
    }
    if (ok && !ec) {
        saved_driver_cache_size = size;
        std::fprintf(stderr, "SOTC driver cache: %zu KB saved\n", size >> 10);
    } else {
        std::filesystem::remove(tmp, ec);
    }
}

// sotc: SOTC_TINY_ASYNC (see GetGraphicsPipeline). 0 (unset): a tiny draw (<= 6 indices) whose
// pipeline is missing is built on the command processor, as before. 1: it is queued at the front
// of the async queue and the draw is skipped until the pipeline is ready, as for geometry draws.
// 2: nothing changes, the build stays inline, and the draws that =1 would defer are counted and
// logged (first 20).
//
// Why tiny draws were built inline: a skipped full-screen pass leaves its target stale, or
// undefined (black) the first time. Geometry draws accept that for the build time. Mode 1 keeps
// the inline build for draws that write a display buffer: a stale or black presented frame is
// seen directly. Other tiny draws (post-processing into intermediates, HUD quads, depth writes)
// are deferred: their result is one or more frames old until the pipeline is ready. Their shaders
// are still translated on the command processor (async_translate_ok excludes tiny draws).
// Known risk of mode 1 (review, 29 Oct): a full-screen pass the game runs only once (a lookup
// table or noise texture made at boot or area load) is skipped on its first and only run, so its
// target stays wrong for good. Mode 1 stays experimental for that reason; the per-area prewarm
// (SOTC_AREA_PREWARM) is the intended fix for first-use builds, since it compiles the pipeline
// before the draw instead of skipping the draw.
static int TinyAsyncMode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_TINY_ASYNC");
        if (!v) {
            return 0;
        }
        return v[0] == '1' ? 1 : v[0] == '2' ? 2 : 0;
    }();
    return mode;
}

namespace {
// sotc: tiny jobs queued (SOTC_TINY_ASYNC=1). They form a run right after the compute and
// translation jobs, before the priority jobs. Guarded by PipelineCache::async_mutex. Only one
// PipelineCache exists per process; the workers reset it when they drain the queue.
std::size_t g_tiny_jobs_queued{};

// sotc: display buffers as guest ranges [begin, end), from SotcTinyAsyncNoteDisplayBuffer.
std::mutex g_display_mutex;
std::vector<std::pair<u64, u64>> g_display_ranges;

// sotc: counters of SOTC_TINY_ASYNC, printed by SotcTinyAsyncReport.
struct TinyAsyncStats {
    std::atomic<u64> queued{};             ///< =1: new tiny pipelines queued at the front
    std::atomic<u64> moved{};              ///< =1: queued pipelines moved to the front
    std::atomic<u64> skipped{};            ///< =1: tiny draws skipped while their pipeline builds
    std::atomic<u64> display_inline{};     ///< =1: built inline, a color target is a display buffer
    std::atomic<u64> would_defer{};        ///< =2: draws that =1 would defer
    std::atomic<u64> would_defer_display{}; ///< =2: of those, the ones on a display buffer
    std::atomic<u64> sync_builds{};        ///< tiny draws built on the command processor
    std::atomic<u64> sync_ns{};            ///< time of those builds
};
TinyAsyncStats g_tiny_stats;
std::atomic<u32> g_tiny_logged{};
} // namespace

// sotc: SOTC_TINY_ASYNC=1 treats a color target that lies in a display buffer as a presentation
// pass. Called for each display buffer VideoOut registers (address, size in bytes). While no
// display buffer is known, every draw counts as one (fail-safe: mode 1 then builds inline).
void SotcTinyAsyncNoteDisplayBuffer(u64 address, u64 size) {
    if (address == 0 || size == 0) {
        return;
    }
    std::scoped_lock lock{g_display_mutex};
    // sotc: the game registers the same buffers again (mode and area changes): keep each base once,
    // so the cap counts distinct buffers.
    for (auto& range : g_display_ranges) {
        if (range.first == address) {
            range.second = address + size;
            return;
        }
    }
    if (g_display_ranges.size() < 64) {
        g_display_ranges.emplace_back(address, address + size);
    }
}

// sotc: writes a report line for the 5 s report: counts since the previous call, and the rate of
// skipped draws over `seconds`. Call from one thread only.
void SotcTinyAsyncReport(std::string& out, double seconds) {
    static std::array<u64, 8> prev{};
    const std::array<u64, 8> now{
        g_tiny_stats.queued.load(std::memory_order_relaxed),
        g_tiny_stats.moved.load(std::memory_order_relaxed),
        g_tiny_stats.skipped.load(std::memory_order_relaxed),
        g_tiny_stats.display_inline.load(std::memory_order_relaxed),
        g_tiny_stats.would_defer.load(std::memory_order_relaxed),
        g_tiny_stats.would_defer_display.load(std::memory_order_relaxed),
        g_tiny_stats.sync_builds.load(std::memory_order_relaxed),
        g_tiny_stats.sync_ns.load(std::memory_order_relaxed),
    };
    std::array<u64, 8> d{};
    for (std::size_t i = 0; i < d.size(); ++i) {
        d[i] = now[i] - prev[i];
    }
    prev = now;
    // sotc: no line when nothing happened in the window (mode 0 has only sync-build counts).
    bool any = false;
    for (const u64 v : d) {
        any |= v != 0;
    }
    if (!any) {
        return;
    }
    const double skip_rate = seconds > 0.0 ? static_cast<double>(d[2]) / seconds : 0.0;
    char line[320];
    std::snprintf(line, sizeof(line),
                  "SOTC tiny async: mode %d, queued %llu, moved %llu, skipped %llu (%.1f/s), "
                  "display inline %llu, would defer %llu (display kept inline %llu), sync tiny "
                  "builds %llu (%.1f ms)\n",
                  TinyAsyncMode(), static_cast<unsigned long long>(d[0]),
                  static_cast<unsigned long long>(d[1]), static_cast<unsigned long long>(d[2]),
                  skip_rate, static_cast<unsigned long long>(d[3]),
                  static_cast<unsigned long long>(d[4]), static_cast<unsigned long long>(d[5]),
                  static_cast<unsigned long long>(d[6]), static_cast<double>(d[7]) / 1e6);
    out += line;
}

// sotc: a pipeline built on the command processor that took longer than 30 ms is logged with
// the reason it was not built on a pipeline worker (SOTC_SYNC_PIPELINE_MS changes the limit).
static void LogSyncPipeline(const char* kind, const char* reason,
                            std::chrono::steady_clock::time_point start, u64 hash_a, u64 hash_b) {
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    static const double min_ms = [] {
        const char* v = std::getenv("SOTC_SYNC_PIPELINE_MS");
        return v ? std::atof(v) : 30.0;
    }();
    static const auto t0 = start;
    static std::atomic<int> logged{0};
    if (ms < min_ms || logged.fetch_add(1, std::memory_order_relaxed) >= 2000) {
        return;
    }
    const double at = std::chrono::duration<double, std::milli>(start - t0).count();
    std::fprintf(stderr, "SOTC SYNC PIPELINE %s (%s) %.0f ms, shaders %016llx %016llx, at %.1f\n",
                 kind, reason, ms, static_cast<unsigned long long>(hash_a),
                 static_cast<unsigned long long>(hash_b), at);
}

bool PipelineCache::AsyncPipelineEligible() const {
    return AsyncIneligibleReason() == nullptr;
}

// sotc: why a pipeline cannot be built on a worker (nullptr: it can). The reason goes to the
// SOTC SYNC PIPELINE log, to tell which blocker costs the most stalls.
const char* PipelineCache::AsyncIneligibleReason() const {
    // Off the command processor the guest registers belong to later draws: only pipelines whose
    // construction reads nothing from them. Vertex inputs are dynamic state (otherwise they are
    // read from the vertex buffer sharps), and no image needs its sharp for its binding count.
    if (!instance.IsVertexInputDynamicState()) {
        return "no dynamic vertex input";
    }
    if (EmulatorSettings.IsShaderCollect()) {
        return "shader collect";
    }
    for (const auto* info : infos) {
        if (!info) {
            continue;
        }
        for (const auto& image : info->images) {
            if (image.array_size == 0 &&
                image.mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                return "dynamic mip index";
            }
        }
    }
    return nullptr;
}

// sotc: SOTC_ASYNC_PRELOAD=0 builds the cached pipelines at boot on the command processor, one
// after the other, as before. With an empty driver cache (a driver update) that took ~100 s of
// black window; the workers now build them while the game starts, and a draw whose pipeline is
// not built yet is skipped (a compute bind waits for it, as for any deferred compute pipeline).
bool PipelineCache::AsyncPreloadEnabled() const {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_ASYNC_PRELOAD");
        return !v || v[0] != '0';
    }();
    return enabled && !async_workers.empty() && !EmulatorSettings.IsShaderCollect();
}

void PipelineCache::QueueAsyncPipeline(u64 hash,
                                       const GraphicsPipeline::SerializationSupport* preloaded,
                                       bool priority,
                                       std::optional<std::string> remap_old_blob) {
    auto job = std::make_unique<AsyncPipelineJob>();
    job->key = graphics_key;
    job->hash = hash;
    if (preloaded) {
        job->preloaded = true;
        job->sdata = *preloaded; // the cached vertex layout the pipeline is built from
    }
    job->remap_old_blob = std::move(remap_old_blob); // sotc: SOTC_PIPE_REMAP (nullopt otherwise)
    job->infos = infos;
    job->runtime_infos = runtime_infos;
    if (fetch_shader) {
        job->fetch_shader = *fetch_shader;
    }
    job->modules = modules;
    {
        std::scoped_lock lock{async_mutex};
        // sotc: shadow (depth-only) pipelines go ahead of the other graphics pipelines (after the
        // compute and translation jobs at the front, which the recorder or a draw waits for).
        if (priority) {
            // Oldest first among them (FIFO), so the first shadow pipeline is not starved.
            job->priority = true;
            // sotc: behind the tiny run (SOTC_TINY_ASYNC), which is empty unless that is on.
            const std::size_t front = std::min<std::size_t>(
                async_queue.size(), async_compute_queued + async_translate_queued +
                                        g_tiny_jobs_queued + async_priority_queued++);
            async_queue.insert(async_queue.begin() + front, std::move(job));
        } else {
            async_queue.push_back(std::move(job));
        }
    }
    async_cv.notify_one();
}

void PipelineCache::AsyncPipelineWorker(std::stop_token token) {
    Common::SetCurrentThreadName("shadPS4:PipelineWorker");
    // sotc: up to 12 workers compile while new areas stream in; below normal they no longer take
    // CPU time from the command processor, draw and game threads. Off by default (review of
    // 2026-10-27): the recorder (high priority) blocks on compute pipelines a worker claimed, and
    // skipped draws wait for translations, so starved workers turned into stalls exactly when the
    // CPU is busiest (streaming). SOTC_WORKER_LOW_PRIO=1 turns it on.
    static const bool low_prio = [] {
        const char* v = std::getenv("SOTC_WORKER_LOW_PRIO");
        return v && v[0] == '1';
    }();
    if (low_prio) {
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
    }
    BbThreads::PlaceThread(BbThreads::Role::PipelineWorker);
    for (;;) {
        std::unique_ptr<AsyncPipelineJob> job;
        {
            std::unique_lock lock{async_mutex};
            async_cv.wait(lock, token, [this] { return !async_queue.empty(); });
            if (token.stop_requested()) {
                // A queued compute pipeline may be waited for by the recording thread: build
                // those before leaving.
                while (!async_queue.empty()) {
                    job = std::move(async_queue.front());
                    async_queue.pop_front();
                    if (job->compute) {
                        job->compute->Build();
                    }
                }
                g_tiny_jobs_queued = 0; // sotc: SOTC_TINY_ASYNC, the queue is empty now
                return;
            }
            job = std::move(async_queue.front());
            async_queue.pop_front();
            if (job->compute) {
                --async_compute_queued;
            } else if (job->translate) {
                --async_translate_queued;
            } else if (g_tiny_jobs_queued != 0) {
                // sotc: SOTC_TINY_ASYNC. The tiny run follows the compute and translation jobs, so
                // with it non-empty the head is one of its jobs.
                --g_tiny_jobs_queued;
            } else if (job->priority) {
                --async_priority_queued;
            }
        }
        if (job->translate) {
            RunAsyncTranslate(*job->translate);
            std::scoped_lock lock{async_mutex};
            async_done.push_back(std::move(job));
            async_done_count.fetch_add(1, std::memory_order_release);
            // sotc: a dispatch may wait for it (WaitPendingTranslation).
            translate_done_cv.notify_all();
            continue;
        }
        if (job->compute) {
            const auto start = std::chrono::steady_clock::now();
            job->compute->Build();
            SotcStats::async_compute_built.fetch_add(1, std::memory_order_relaxed);
            SotcStats::async_compute_build_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count(),
                std::memory_order_relaxed);
            continue;
        }
        job->pipeline = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, layout_cache, profile, job->key, *pipeline_cache,
            job->infos,
            job->runtime_infos, job->fetch_shader ? &*job->fetch_shader : nullptr, job->modules,
            job->sdata, job->preloaded, true);
        SotcStats::frame_compiles.fetch_add(1, std::memory_order_relaxed);
        std::scoped_lock lock{async_mutex};
        async_done.push_back(std::move(job));
        async_done_count.fetch_add(1, std::memory_order_release);
    }
}

void PipelineCache::CollectAsyncPipelines() {
    std::vector<std::unique_ptr<AsyncPipelineJob>> done;
    {
        std::scoped_lock lock{async_mutex};
        done.swap(async_done);
        async_done_count.store(0, std::memory_order_relaxed);
    }
    for (auto& job : done) {
        if (job->translate) {
            InstallTranslation(*job->translate);
            continue;
        }
        const auto it = graphics_pipelines.find(job->key);
        if (it == graphics_pipelines.end()) {
            continue;
        }
        if (it.value()) {
            // A full-screen pass built it on the command processor meanwhile (that one may be
            // in use): the worker's copy is dropped. A remapped job leaves its old blob alone:
            // nothing here checked the pipeline the entry holds.
            if (!job->preloaded) {
                RegisterPipelineData(job->key, job->hash, job->sdata);
                ++num_new_pipelines;
            }
            continue;
        }
        if (job->preloaded) {
            // Already in the cache. One the driver refused is dropped: the next draw that needs
            // it compiles it, as when WarmUp builds it itself and it fails.
            if (job->pipeline->Handle()) {
                it.value() = std::move(job->pipeline);
                if (job->remap_old_blob) {
                    // sotc: SOTC_PIPE_REMAP: the remapped key built. It is saved under its new
                    // hash, then the old blob goes; a save that is not queued keeps the old one.
                    if (RegisterPipelineData(job->key, job->hash, job->sdata) &&
                        Storage::DataBase::Instance().Remove(Storage::BlobType::PipelineKey,
                                                             *job->remap_old_blob)) {
                        LOG_INFO(Render_Vulkan,
                                 "SOTC_PIPE_REMAP: built under the remapped key, old key {} "
                                 "removed",
                                 *job->remap_old_blob);
                    }
                }
            } else {
                graphics_pipelines.erase(it);
            }
            continue;
        }
        it.value() = std::move(job->pipeline);
        RegisterPipelineData(job->key, job->hash, job->sdata);
        ++num_new_pipelines;
    }
}

void PipelineCache::QueueAsyncTranslate(HwStage hw_stage, SwStage sw_stage,
                                        const Shader::ShaderParams& params,
                                        const Shader::RuntimeInfo& rt,
                                        const Shader::Backend::Bindings& start, Program* program,
                                        const Shader::StageSpecialization* spec, size_t perm_idx) {
    auto job = std::make_unique<AsyncPipelineJob>();
    auto& t = *(job->translate = std::make_unique<AsyncPipelineJob::Translate>());
    t.hw_stage = hw_stage;
    t.sw_stage = sw_stage;
    t.hash = params.hash;
    // The user data registers change with the next draws: the worker reads a copy. The code
    // stays in guest memory (inline constants are addressed from it).
    std::ranges::copy(params.user_data, t.user_data.begin());
    t.code = params.code;
    t.runtime_info = rt;
    t.start = start;
    t.program = program;
    t.perm_idx = perm_idx;
    if (spec) {
        t.spec = *spec;
    }
    {
        std::scoped_lock lock{async_mutex};
        async_queue.insert(async_queue.begin() + async_compute_queued + async_translate_queued++,
                           std::move(job));
    }
    async_cv.notify_one();
}

bool PipelineCache::PrefetchComputeProgram(const AmdGpu::ComputeProgram& cs) {
    if (!compute_prefetch || EmulatorSettings.IsShaderCollect()) {
        return false;
    }
    // The registers come from a scan ahead of the dispatches: only a mapped code address is
    // read, and a failed binary info search gives up instead of stopping the port.
    const auto* code = cs.Address<const u32*>();
    if (!code || !Core::Memory::Instance()->GetBackingPointer(reinterpret_cast<VAddr>(code))) {
        return false;
    }
    std::optional<Shader::ShaderParams> params;
    const bool throws = Common::g_assert_throws;
    Common::g_assert_throws = true;
    try {
        params.emplace(AmdGpu::GetParams(cs));
    } catch (const Common::AssertFailure&) {
    }
    Common::g_assert_throws = throws;
    if (!params || params->code.empty() || IsProgramKnown(params->hash)) {
        return false;
    }
    auto rt = runtime_infos[static_cast<u32>(SwStage::Compute)]; // not default-constructible
    rt.Initialize(HwStage::Compute, SwStage::Compute);
    FillComputeRuntimeInfo(rt, cs);
    pending_programs.insert(params->hash);
    QueueAsyncTranslate(HwStage::Compute, SwStage::Compute, *params, rt,
                        Shader::Backend::Bindings{}, nullptr, nullptr, 0);
    SotcStats::compute_prefetch_queued.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void PipelineCache::WaitPendingTranslation(u64 hash) {
    const auto start = std::chrono::steady_clock::now();
    {
        // No worker started it yet: translating it here is quicker than waiting behind the
        // jobs ahead of it. Translation jobs sit right after the compute jobs.
        std::scoped_lock lock{async_mutex};
        const auto first = async_queue.begin() + static_cast<std::ptrdiff_t>(async_compute_queued);
        const auto last = first + static_cast<std::ptrdiff_t>(async_translate_queued);
        const auto it = std::find_if(first, last, [hash](const auto& job) {
            return !job->translate->program && job->translate->hash == hash;
        });
        if (it != last) {
            async_queue.erase(it);
            --async_translate_queued;
            pending_programs.erase(hash);
            SotcStats::compute_prefetch_taken_back.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    // A worker is translating it: it finishes no later than a second translation here would.
    // The deadline only guards against a worker that never finishes; past it the shader is
    // translated here and the worker's copy is dropped when installed (InstallTranslation).
    const auto deadline = start + std::chrono::seconds{1};
    for (;;) {
        if (async_done_count.load(std::memory_order_acquire) != 0) {
            last_graphics_pipeline = nullptr; // a refused preloaded pipeline may be erased
            CollectAsyncPipelines();
        }
        if (!pending_programs.contains(hash)) {
            break;
        }
        std::unique_lock lock{async_mutex};
        if (!translate_done_cv.wait_until(lock, deadline, [this] {
                return async_done_count.load(std::memory_order_relaxed) != 0;
            })) {
            SotcStats::compute_prefetch_timeouts.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    SotcStats::compute_prefetch_waits.fetch_add(1, std::memory_order_relaxed);
    SotcStats::compute_prefetch_wait_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                             start)
            .count(),
        std::memory_order_relaxed);
}

void PipelineCache::RunAsyncTranslate(AsyncPipelineJob::Translate& t) {
    static thread_local Shader::Pools worker_pools;
    const auto start_time = std::chrono::steady_clock::now();
    const Shader::ShaderParams params{
        .user_data = std::span<const u32, Shader::ShaderParams::NumShaderUserData>{t.user_data},
        .code = t.code,
        .hash = t.hash,
    };
    auto binding = t.start;
    auto runtime_info = t.runtime_info;
    // The tables the user data points to may already hold the game's next data: a translation
    // that asserts on what it read gives the shader back to the command processor.
    Common::g_assert_throws = true;
    try {
        TranslateOnWorker(t, params, binding, runtime_info, worker_pools);
    } catch (const Common::AssertFailure&) {
        t.failed = true;
        t.new_program.reset();
        if (t.module) {
            instance.GetDevice().destroyShaderModule(t.module);
            t.module = vk::ShaderModule{};
        }
    }
    Common::g_assert_throws = false;
    SotcStats::async_translate_built.fetch_add(1, std::memory_order_relaxed);
    SotcStats::async_translate_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                             start_time)
            .count(),
        std::memory_order_relaxed);
}

void PipelineCache::TranslateOnWorker(AsyncPipelineJob::Translate& t,
                                      const Shader::ShaderParams& params,
                                      Shader::Backend::Bindings& binding,
                                      Shader::RuntimeInfo& runtime_info,
                                      Shader::Pools& worker_pools) {
    if (!t.program) {
        t.new_program = std::make_unique<Program>(t.hw_stage, t.sw_stage, params);
        t.module = CompileModule(t.new_program->info, runtime_info, params.code, 0, binding,
                                 &worker_pools, &t.spv);
        t.spec.emplace(t.new_program->info, runtime_info, profile, t.start);
    } else {
        auto new_info = Shader::Info(t.hw_stage, t.sw_stage, params);
        t.module = CompileModule(new_info, runtime_info, params.code, t.perm_idx, binding,
                                 &worker_pools, &t.spv);
        // The command processor made spec from the tables of its draw; the module was made from
        // what the tables hold now. Different: installing it under spec would bind wrong code.
        const Shader::StageSpecialization worker_spec{new_info, t.runtime_info, profile,
                                                      t.start};
        t.stale = !(worker_spec == *t.spec);
    }
}

void PipelineCache::InstallTranslation(AsyncPipelineJob::Translate& t) {
    if (!t.program) {
        pending_programs.erase(t.hash);
        if (t.failed) {
            SotcStats::async_translate_stale.fetch_add(1, std::memory_order_relaxed);
            in_place_programs.insert(t.hash);
            return;
        }
        auto [it, is_new] = program_cache.try_emplace(t.hash);
        if (!is_new) {
            // Translated meanwhile on the command processor (by a draw that may not wait).
            instance.GetDevice().destroyShaderModule(t.module);
            return;
        }
        it.value() = std::move(t.new_program);
        auto& program = it.value();
        RegisterShaderBinary(std::move(t.spv), t.hash, 0);
        RegisterShaderMeta(program->info, t.spec->fetch_shader_data, *t.spec,
                           HashCombine(t.hash, 0), 0);
        program->AddPermut(t.module, std::move(*t.spec));
        // The user data seen by the worker goes away with the job; every lookup sets its own.
        program->info.UserData() = {};
        return;
    }
    auto& program = *t.program;
    if (const auto pending = std::ranges::find(program.pending_specs, *t.spec);
        pending != program.pending_specs.end()) {
        program.pending_specs.erase(pending);
    }
    if (t.stale || t.failed) {
        SotcStats::async_translate_stale.fetch_add(1, std::memory_order_relaxed);
        program.translate_in_place = true;
        instance.GetDevice().destroyShaderModule(t.module);
        return;
    }
    if (std::ranges::find(program.modules, *t.spec, &Program::Module::spec) !=
        program.modules.end()) {
        instance.GetDevice().destroyShaderModule(t.module);
        return;
    }
    const size_t perm_idx = program.modules.size();
    RegisterShaderBinary(std::move(t.spv), t.hash, perm_idx);
    RegisterShaderMeta(program.info, t.spec->fetch_shader_data, *t.spec,
                       HashCombine(t.hash, perm_idx), perm_idx);
    program.AddPermut(t.module, std::move(*t.spec));
}

// sotc: SOTC_PIPE_MEMO. Unset: the graphics key memo of GetGraphicsPipeline is on, as before, and
// the stage and compute memos are off. =0: the key memo is off too. =1: the stage memo (GetProgram)
// and the compute memo (GetComputePipeline) are on. =2: the same, and every memo answer is also
// computed by the slow path; a different answer is counted and the slow result is used.
static bool PipeMemoKeyOn() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_PIPE_MEMO");
        return !v || v[0] != '0';
    }();
    return on;
}

static int PipeMemoStageMode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_PIPE_MEMO");
        return v && v[0] == '1' ? 1 : v && v[0] == '2' ? 2 : 0;
    }();
    return mode;
}

namespace {
// sotc: counters of SOTC_PIPE_MEMO, printed by ReportPipeMemo.
// The fields are in the order of kPipeMemoCounters (ReportPipeMemo).
struct PipeMemoStats {
    std::atomic<u64> graphics_lookups{}; ///< graphics lookups (mode 1/2)
    std::atomic<u64> graphics_hits{};    ///< of those, answered by the last-pipeline memo
    std::atomic<u64> stage_lookups{};    ///< stage lookups of an existing program (mode 1/2)
    std::atomic<u64> stage_hits{};       ///< of those, answered by the stage memo
    std::atomic<u64> stage_evictions{};  ///< stage stores that replaced an entry of another key
    std::atomic<u64> stage_foreign{};    ///< GetProgram calls from a thread other than the first
    std::atomic<u64> compute_lookups{};  ///< compute lookups (mode 1/2)
    std::atomic<u64> compute_hits{};     ///< of those, answered by the last-pipeline memo
    std::atomic<u64> mismatch_graphics{}; ///< =2: graphics memo answer differed from the slow path
    std::atomic<u64> mismatch_stage_perm{};     ///< =2: stage memo permutation index differed
    std::atomic<u64> mismatch_stage_module{};   ///< =2: same permutation, other shader module
    std::atomic<u64> mismatch_stage_fetch{};    ///< =2: vertex fetch shader data differed
    std::atomic<u64> mismatch_stage_bindings{}; ///< =2: bindings after the stage differed
    std::atomic<u64> mismatch_stage_pending{};  ///< =2: memo hit, the slow path queued a translation
    std::atomic<u64> mismatch_compute{};  ///< =2: compute memo answer differed from the slow path
};
PipeMemoStats g_pipe_memo;
} // namespace

// sotc: a thread other than the first caller of GetProgram (in a memo mode) takes the slow path.
// Command processor thread only in practice; the check is one atomic load on a hit of the first
// caller and a compare-exchange on the very first call.
bool PipelineCache::PipeMemoOwnerThread() {
    const std::thread::id self = std::this_thread::get_id();
    std::thread::id owner{};
    if (pipe_memo_owner.compare_exchange_strong(owner, self, std::memory_order_acq_rel)) {
        return true;
    }
    if (owner == self) {
        return true;
    }
    g_pipe_memo.stage_foreign.fetch_add(1, std::memory_order_relaxed);
    return false;
}

size_t PipelineCache::StageMemoSet(u64 hash, Shader::HwStage hw_stage, Shader::SwStage sw_stage) {
    const u64 mix = hash ^ (u64(static_cast<u32>(hw_stage)) << 3) ^
                    (u64(static_cast<u32>(sw_stage)) << 9);
    return static_cast<size_t>((mix ^ (mix >> 29)) % kStageMemoSets);
}

// sotc: true when every input the specialization of this stage reads is the one stored in memo.
// The flattened buffer and the user data are compared as they are now (RefreshFlatBuf already ran);
// the tessellation constant buffer is read from guest memory again, as the specialization does.
// A vertex fetch shader: its code words are compared with the copy stored (read through the
// backing view, like the shader binary info, so a draw does not read-fault the guest page).
bool PipelineCache::StageMemoHit(const StageMemo& memo, const Program& program,
                                 Shader::HwStage hw_stage, Shader::SwStage sw_stage,
                                 const Shader::ShaderParams& params,
                                 const Shader::RuntimeInfo& rt,
                                 const Shader::Backend::Bindings& start,
                                 const Shader::TessellationDataConstantBuffer& tess,
                                 bool has_tess, u64 generation) const {
    if (!memo.valid || memo.program != &program || memo.hw_stage != hw_stage ||
        memo.sw_stage != sw_stage || memo.hash != params.hash ||
        memo.modules_size != program.modules.size() || memo.perm >= program.modules.size() ||
        memo.generation != generation || memo.has_tess != has_tess || !(memo.start == start) ||
        !(memo.runtime_info == rt) ||
        std::memcmp(memo.user_data.data(), params.user_data.data(), sizeof(memo.user_data)) != 0) {
        return false;
    }
    if (has_tess && std::memcmp(&memo.tess, &tess, sizeof(tess)) != 0) {
        return false;
    }
    if (!std::ranges::equal(memo.flat_buf, program.info.FlatBuf())) {
        return false;
    }
    // sotc: the same test as ParseFetchShader: a vertex stage with a fetch shader.
    const bool fetch_now =
        program.info.sw_stage == Shader::SwStage::Vertex && program.info.has_fetch_shader;
    if (memo.has_fetch != fetch_now) {
        return false;
    }
    if (!fetch_now) {
        return true;
    }
    const u8* backing = Core::Memory::Instance()->GetBackingPointer(reinterpret_cast<VAddr>(
        Shader::Gcn::GetFetchShaderCode(program.info, program.info.fetch_shader_sgpr_base)));
    return backing != nullptr &&
           (memo.fetch_code.empty() ||
            std::memcmp(backing, memo.fetch_code.data(), memo.fetch_code.size() * sizeof(u32)) == 0);
}

PipelineCache::StageMemo* PipelineCache::StageMemoVictim(StageMemo* ways, const Program& program,
                                                         Shader::HwStage hw_stage,
                                                         Shader::SwStage sw_stage, u64 hash) {
    StageMemo* pick = nullptr;
    for (size_t w = 0; w < kStageMemoWays; ++w) {
        StageMemo& m = ways[w];
        if (m.valid && m.program == &program && m.hw_stage == hw_stage &&
            m.sw_stage == sw_stage && m.hash == hash) {
            return &m; // same key, other inputs: this entry is replaced, nothing is evicted
        }
        // An empty way first, else the least recently used one.
        if (!pick || (!m.valid && pick->valid) || (m.valid && pick->valid && m.stamp < pick->stamp)) {
            pick = &m;
        }
    }
    if (pick->valid) {
        g_pipe_memo.stage_evictions.fetch_add(1, std::memory_order_relaxed);
    }
    return pick;
}

bool PipelineCache::StageMemoStore(StageMemo& memo, const Program& program,
                                   Shader::HwStage hw_stage, Shader::SwStage sw_stage,
                                   const Shader::ShaderParams& params,
                                   const Shader::RuntimeInfo& rt,
                                   const Shader::Backend::Bindings& start,
                                   const Shader::TessellationDataConstantBuffer& tess,
                                   bool has_tess, u64 generation, size_t perm,
                                   const Shader::Backend::Bindings& after) {
    const bool fetch_now =
        program.info.sw_stage == Shader::SwStage::Vertex && program.info.has_fetch_shader;
    // Read before anything in the entry changes: a failed read leaves the old entry as it was.
    const u8* fetch_backing = nullptr;
    u32 fetch_words = 0;
    if (fetch_now) {
        fetch_backing = Core::Memory::Instance()->GetBackingPointer(reinterpret_cast<VAddr>(
            Shader::Gcn::GetFetchShaderCode(program.info, program.info.fetch_shader_sgpr_base)));
        if (!fetch_backing) {
            return false;
        }
        fetch_words = program.modules[perm].spec.fetch_shader_data.size / sizeof(u32);
        memo.fetch_code.resize(fetch_words); // keeps its capacity once warm
        std::memcpy(memo.fetch_code.data(), fetch_backing, fetch_words * sizeof(u32));
    } else {
        memo.fetch_code.clear();
    }
    memo.valid = true;
    memo.program = &program;
    memo.hw_stage = hw_stage;
    memo.sw_stage = sw_stage;
    memo.hash = params.hash;
    memo.perm = static_cast<u32>(perm);
    memo.modules_size = program.modules.size();
    std::memcpy(memo.user_data.data(), params.user_data.data(), sizeof(memo.user_data));
    // assign() reuses the capacity kept from earlier stores: no allocation once warm.
    const auto& flat = program.info.FlatBuf();
    memo.flat_buf.assign(flat.begin(), flat.end());
    memo.runtime_info = rt;
    memo.start = start;
    memo.after = after;
    memo.has_tess = has_tess;
    memo.tess = tess;
    memo.has_fetch = fetch_now;
    memo.generation = generation;
    memo.stamp = ++stage_memo_tick;
    return true;
}

// sotc: writes a report line for the 5 s report: counts since the previous call. Prints nothing
// while SOTC_PIPE_MEMO is not 1 or 2, or when the window had no lookups. Call from one thread only.
void ReportPipeMemo(std::string& out, double seconds) {
    if (PipeMemoStageMode() == 0) {
        return;
    }
    // Same order as the fields of PipeMemoStats.
    const std::array<const std::atomic<u64>*, 15> counters{
        &g_pipe_memo.graphics_lookups,      &g_pipe_memo.graphics_hits,
        &g_pipe_memo.stage_lookups,         &g_pipe_memo.stage_hits,
        &g_pipe_memo.stage_evictions,       &g_pipe_memo.stage_foreign,
        &g_pipe_memo.compute_lookups,       &g_pipe_memo.compute_hits,
        &g_pipe_memo.mismatch_graphics,     &g_pipe_memo.mismatch_stage_perm,
        &g_pipe_memo.mismatch_stage_module, &g_pipe_memo.mismatch_stage_fetch,
        &g_pipe_memo.mismatch_stage_bindings, &g_pipe_memo.mismatch_stage_pending,
        &g_pipe_memo.mismatch_compute,
    };
    static std::array<u64, counters.size()> prev{};
    std::array<u64, counters.size()> d{};
    for (std::size_t i = 0; i < d.size(); ++i) {
        const u64 now = counters[i]->load(std::memory_order_relaxed);
        d[i] = now - prev[i];
        prev[i] = now;
    }
    // Indices as in the array above.
    constexpr std::size_t kGraphicsLookups = 0, kGraphicsHits = 1, kStageLookups = 2,
                          kStageHits = 3, kStageEvictions = 4, kStageForeign = 5,
                          kComputeLookups = 6, kComputeHits = 7, kMismatchGraphics = 8,
                          kMismatchPerm = 9, kMismatchModule = 10, kMismatchFetch = 11,
                          kMismatchBindings = 12, kMismatchPending = 13, kMismatchCompute = 14;
    if (d[kGraphicsLookups] == 0 && d[kStageLookups] == 0 && d[kComputeLookups] == 0 &&
        d[kStageForeign] == 0) {
        return;
    }
    const auto pct = [](u64 hits, u64 lookups) {
        return lookups ? 100.0 * static_cast<double>(hits) / static_cast<double>(lookups) : 0.0;
    };
    const u64 stage_mismatch = d[kMismatchPerm] + d[kMismatchModule] + d[kMismatchFetch] +
                               d[kMismatchBindings] + d[kMismatchPending];
    char line[640];
    std::snprintf(
        line, sizeof(line),
        "SOTC pipe memo: mode %d, graphics %llu/%llu hits (%.0f%%), stage %llu/%llu hits "
        "(%.0f%%), stage evictions %llu, stage foreign %llu, compute %llu/%llu hits (%.0f%%), "
        "mismatches %llu (graphics %llu, stage %llu: perm %llu, module %llu, fetch %llu, "
        "bindings %llu, pending %llu; compute %llu), %.1f lookups/s\n",
        PipeMemoStageMode(), static_cast<unsigned long long>(d[kGraphicsHits]),
        static_cast<unsigned long long>(d[kGraphicsLookups]), pct(d[kGraphicsHits], d[kGraphicsLookups]),
        static_cast<unsigned long long>(d[kStageHits]), static_cast<unsigned long long>(d[kStageLookups]),
        pct(d[kStageHits], d[kStageLookups]), static_cast<unsigned long long>(d[kStageEvictions]),
        static_cast<unsigned long long>(d[kStageForeign]),
        static_cast<unsigned long long>(d[kComputeHits]),
        static_cast<unsigned long long>(d[kComputeLookups]), pct(d[kComputeHits], d[kComputeLookups]),
        static_cast<unsigned long long>(d[kMismatchGraphics] + stage_mismatch + d[kMismatchCompute]),
        static_cast<unsigned long long>(d[kMismatchGraphics]), static_cast<unsigned long long>(stage_mismatch),
        static_cast<unsigned long long>(d[kMismatchPerm]), static_cast<unsigned long long>(d[kMismatchModule]),
        static_cast<unsigned long long>(d[kMismatchFetch]), static_cast<unsigned long long>(d[kMismatchBindings]),
        static_cast<unsigned long long>(d[kMismatchPending]), static_cast<unsigned long long>(d[kMismatchCompute]),
        seconds > 0.0
            ? static_cast<double>(d[kGraphicsLookups] + d[kStageLookups] + d[kComputeLookups]) / seconds
            : 0.0);
    out += line;
}

// sotc: a draw with a depth target, depth writes enabled and no color target bound (a shadow-map
// pass). The color rules are the ones RefreshGraphicsKey applies.
bool PipelineCache::IsDepthOnlyDraw() const {
    const auto& regs = liverpool->regs;
    if (!regs.depth_buffer.DepthValid() || !regs.depth_control.depth_write_enable) {
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable) {
        return true;
    }
    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        if (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb)) {
            return false;
        }
    }
    return true;
}

const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(const DrawIndirectParams params) {
    draw_indirect_params = params;
    // sotc: SOTC_DEPTH_SYNC_PIPE=1 builds depth-only (shadow) pipelines on this thread so their
    // draws are never skipped. Off by default since 2026-10-27: each build took ~36 ms and an
    // area change built several (hitches of 0.4-3.8 s). By default they are queued ahead of the
    // other pipelines instead (QueueAsyncPipeline priority), which shortens the blink.
    static const bool depth_sync = [] {
        const char* v = std::getenv("SOTC_DEPTH_SYNC_PIPE");
        return v && v[0] == '1';
    }();
    const bool depth_draw = !depth_sync && IsDepthOnlyDraw();
    // sotc: shadow casters are depth-only geometry draws. Skipping them while their pipeline is
    // built made shadows blink after loads, so they are never skipped: they build here, like the
    // full-screen passes. Their shaders are trivial, so building inline is cheap.
    const bool depth_only = depth_sync && IsDepthOnlyDraw();
    // sotc: finished translations must be installed before the stages are looked up.
    if (async_done_count.load(std::memory_order_acquire) != 0) {
        CollectAsyncPipelines();
    }
    // sotc: full-screen passes (tiny draws, see below) are never skipped, so their shaders are
    // translated here. Without dynamic vertex input the key needs the vertex shader's sharps.
    async_translate_ok = !async_workers.empty() && AsyncTranslateEnabled() &&
                         liverpool->regs.num_indices > 6 && instance.IsVertexInputDynamicState() &&
                         !EmulatorSettings.IsShaderCollect() && !depth_only;
    translation_pending = false;
    if (!RefreshGraphicsKey()) {
        if (translation_pending) {
            SotcStats::async_translate_skips.fetch_add(1, std::memory_order_relaxed);
        }
        return nullptr;
    }
    // sotc: consecutive draws mostly use the same pipeline: when the freshly built key equals the
    // previous draw's byte for byte, skip the map's hash and probe. The stage state (infos,
    // modules, fetch shader) was refreshed by RefreshGraphicsKey either way. SOTC_PIPE_MEMO=0.
    const int memo_mode = PipeMemoStageMode();
    // The map size check (an erase since the last store) is only made in modes 1 and 2.
    const bool key_hit = PipeMemoKeyOn() && last_graphics_pipeline &&
                         (memo_mode == 0 || last_graphics_size == graphics_pipelines.size()) &&
                         std::memcmp(&graphics_key, &last_graphics_key, sizeof(graphics_key)) == 0;
    if (memo_mode != 0) {
        g_pipe_memo.graphics_lookups.fetch_add(1, std::memory_order_relaxed);
    }
    if (key_hit && memo_mode != 2) {
        if (memo_mode != 0) {
            g_pipe_memo.graphics_hits.fetch_add(1, std::memory_order_relaxed);
        }
        return last_graphics_pipeline;
    }
    const auto [it, is_new] = graphics_pipelines.try_emplace(graphics_key);
    if (key_hit) { // =2 only: the slow path runs too and must give the same pipeline
        g_pipe_memo.graphics_hits.fetch_add(1, std::memory_order_relaxed);
        if (is_new || it.value().get() != last_graphics_pipeline) {
            g_pipe_memo.mismatch_graphics.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // sotc: still being built by a pipeline worker (SOTC_ASYNC_SHADERS, SOTC_ASYNC_PRELOAD). A
    // full-screen pass is not skipped: it builds the pipeline here, and the worker's copy is
    // dropped when collected.
    const bool queued = !is_new && !it->second;
    if (queued && liverpool->regs.num_indices > 6 && !depth_only) {
        return nullptr;
    }
    if (is_new || queued) {
        const bool eligible = AsyncPipelineEligible();
        // sotc: SOTC_TINY_ASYNC. Full-screen passes (post-processing, composition) are tiny draws:
        // skipping one leaves a black or stale frame, so by default they still build here (see
        // TinyAsyncMode for when mode 1 defers them).
        const bool tiny_draw = liverpool->regs.num_indices <= 6;
        const int tiny_mode = tiny_draw && !depth_only ? TinyAsyncMode() : 0;
        const bool tiny_can_defer = tiny_mode != 0 && !async_workers.empty() && eligible;
        // sotc: a color target lies in a display buffer (a presentation pass): never deferred.
        const auto tiny_writes_display = [this] {
            const auto& regs = liverpool->regs;
            std::scoped_lock lock{g_display_mutex};
            if (g_display_ranges.empty()) {
                return true; // no display buffer known: every draw counts as one
            }
            for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
                if (!regs.color_buffers[cb] || !regs.color_target_mask.GetMask(cb)) {
                    continue;
                }
                const u64 addr = regs.color_buffers[cb].Address();
                for (const auto& [begin, end] : g_display_ranges) {
                    if (addr >= begin && addr < end) {
                        return true;
                    }
                }
            }
            return false;
        };
        // sotc: moves this draw's queued pipeline to the tiny run: behind the compute and
        // translation jobs, ahead of the priority jobs. Returns false if no worker has it queued
        // (a worker may have taken it already) or it is in the run already.
        const auto move_to_tiny_run = [this] {
            std::scoped_lock lock{async_mutex};
            const auto found =
                std::find_if(async_queue.begin(), async_queue.end(), [this](const auto& job) {
                    return !job->compute && !job->translate && job->key == graphics_key;
                });
            const std::size_t head = async_compute_queued + async_translate_queued;
            if (found == async_queue.end() ||
                static_cast<std::size_t>(std::distance(async_queue.begin(), found)) <
                    head + g_tiny_jobs_queued) {
                return false;
            }
            auto job = std::move(*found);
            async_queue.erase(found);
            if (job->priority) {
                job->priority = false;
                --async_priority_queued;
            }
            const std::size_t at = std::min(head + g_tiny_jobs_queued, async_queue.size());
            async_queue.insert(async_queue.begin() + static_cast<std::ptrdiff_t>(at),
                               std::move(job));
            ++g_tiny_jobs_queued;
            return true;
        };
        // sotc: SOTC_TINY_ASYNC=1: a tiny draw whose pipeline a worker is building is skipped, as
        // a geometry draw is, and the pipeline moves to the front. A display draw builds here.
        if (queued && tiny_mode == 1 && tiny_can_defer && !tiny_writes_display()) {
            if (move_to_tiny_run()) {
                g_tiny_stats.moved.fetch_add(1, std::memory_order_relaxed);
            }
            g_tiny_stats.skipped.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(graphics_key);
        LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);
        // Geometry draws are skipped while their pipeline is built (a brief pop-in instead of a
        // freeze on first sight).
        if (!async_workers.empty() && !tiny_draw && !depth_only && eligible && !queued) {
            QueueAsyncPipeline(pipeline_hash, nullptr, depth_draw);
            return nullptr; // the draws that need it are skipped until it is ready
        }
        // sotc: SOTC_TINY_ASYNC. Mode 2 only counts and logs what mode 1 would defer. Mode 1
        // queues a new pipeline at the front of the tiny run (a queued one returned above).
        if (tiny_can_defer) {
            const bool tiny_display = tiny_writes_display();
            if (tiny_mode == 2) {
                if (tiny_display) {
                    g_tiny_stats.would_defer_display.fetch_add(1, std::memory_order_relaxed);
                } else {
                    g_tiny_stats.would_defer.fetch_add(1, std::memory_order_relaxed);
                    if (g_tiny_logged.fetch_add(1, std::memory_order_relaxed) < 20) {
                        std::fprintf(stderr, "SOTC tiny draw would defer %016llx%s\n",
                                     static_cast<unsigned long long>(pipeline_hash),
                                     queued ? " (already queued)" : "");
                    }
                }
            } else if (tiny_display) {
                g_tiny_stats.display_inline.fetch_add(1, std::memory_order_relaxed);
            } else {
                QueueAsyncPipeline(pipeline_hash, nullptr, false);
                // Queued at the back by QueueAsyncPipeline, moved to the front right after. If a
                // worker took it in between, the draw is skipped all the same.
                move_to_tiny_run();
                g_tiny_stats.queued.fetch_add(1, std::memory_order_relaxed);
                g_tiny_stats.skipped.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
        }
        const SotcStats::ScopedTimer stall_timer{SotcStats::pipeline_ns};
        SotcStats::pipelines_new.fetch_add(1, std::memory_order_relaxed);
        const auto sync_start = std::chrono::steady_clock::now();

        const SotcStats::FrameTimer frame_timer{SotcStats::FramePipeline};
        SotcStats::frame_compiles.fetch_add(1, std::memory_order_relaxed);
        GraphicsPipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, layout_cache, profile, graphics_key, *pipeline_cache,
            infos,
            runtime_infos, fetch_shader, modules, sdata, false);
        LogSyncPipeline("graphics",
                        async_workers.empty() ? "async off"
                        : tiny_draw           ? "tiny draw"
                        : depth_only          ? "depth only"
                        : AsyncIneligibleReason() ? AsyncIneligibleReason()
                                                  : "not eligible",
                        sync_start, infos[3] ? infos[3]->pgm_hash : 0, // vertex
                        infos[0] ? infos[0]->pgm_hash : 0);              // fragment

        if (tiny_draw) { // sotc: SOTC_TINY_ASYNC counters (every mode), for the 5 s report
            g_tiny_stats.sync_builds.fetch_add(1, std::memory_order_relaxed);
            g_tiny_stats.sync_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - sync_start)
                    .count(),
                std::memory_order_relaxed);
        }
        if (!queued) { // a queued one registers when collected (or was preloaded)
            RegisterPipelineData(graphics_key, pipeline_hash, sdata);
            ++num_new_pipelines;
        }

        if (EmulatorSettings.IsShaderCollect()) {
            for (auto stage = 0; stage < MaxShaderStages; ++stage) {
                if (infos[stage]) {
                    auto& m = modules[stage];
                    module_related_pipelines[m].emplace_back(graphics_key);
                }
            }
        }
    }
    std::memcpy(&last_graphics_key, &graphics_key, sizeof(graphics_key));
    last_graphics_pipeline = it->second.get();
    last_graphics_size = graphics_pipelines.size();
    return it->second.get();
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    if (!RefreshComputeKey()) {
        return nullptr;
    }
    // sotc: SOTC_PIPE_MEMO=1/2: the key of the last lookup and its pipeline. Same key, same map
    // entry: the map is probed only when the key changes or the maps changed size (an erase).
    const int memo_mode = PipeMemoStageMode();
    const bool memo_hit = memo_mode != 0 && last_compute_pipeline &&
                          compute_key == last_compute_key &&
                          last_compute_size == compute_pipelines.size();
    if (memo_mode != 0) {
        g_pipe_memo.compute_lookups.fetch_add(1, std::memory_order_relaxed);
    }
    if (memo_hit) {
        g_pipe_memo.compute_hits.fetch_add(1, std::memory_order_relaxed);
        if (memo_mode == 1) {
            return last_compute_pipeline;
        }
    }
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
        LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);
        const SotcStats::ScopedTimer stall_timer{SotcStats::pipeline_ns};
        SotcStats::pipelines_new.fetch_add(1, std::memory_order_relaxed);

        const SotcStats::FrameTimer frame_timer{SotcStats::FramePipeline};
        SotcStats::frame_compiles.fetch_add(1, std::memory_order_relaxed);
        ComputePipeline::SerializationSupport sdata{};
        // Shader collection replaces modules and erases their pipelines: built here then.
        // Without a recording thread the command processor itself would wait for the worker.
        // The presenter's Finish at shutdown runs every deferred bind before the workers stop.
        const bool deferred = !async_workers.empty() && AsyncComputeEnabled() &&
                              scheduler.HasRecordThread() && !EmulatorSettings.IsShaderCollect();
        const auto sync_start = std::chrono::steady_clock::now();
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap,
                                                       layout_cache, profile, *pipeline_cache,
                                                       compute_key, *infos[0],
                                                       modules[0], sdata, false, deferred);
        if (deferred) {
            auto job = std::make_unique<AsyncPipelineJob>();
            job->compute = it.value().get();
            {
                std::scoped_lock lock{async_mutex};
                // Ahead of the graphics jobs, after the older compute ones: the recording
                // thread binds them in this order.
                async_queue.insert(async_queue.begin() + async_compute_queued++, std::move(job));
            }
            async_cv.notify_one();
        } else {
            LogSyncPipeline("compute", async_workers.empty() ? "async off" : "not deferred",
                            sync_start, infos[0]->pgm_hash, 0);
        }
        RegisterPipelineData(compute_key, sdata);
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            auto& m = modules[0];
            module_related_pipelines[m].emplace_back(compute_key);
        }
    }
    const ComputePipeline* result = it->second.get();
    if (memo_mode != 0) {
        if (memo_hit && result != last_compute_pipeline) { // =2 only
            g_pipe_memo.mismatch_compute.fetch_add(1, std::memory_order_relaxed);
        }
        last_compute_pipeline = result;
        last_compute_key = compute_key;
        last_compute_size = compute_pipelines.size();
    }
    return result;
}

bool PipelineCache::RefreshGraphicsKey() {
    std::memset(&graphics_key, 0, sizeof(GraphicsPipelineKey));
    const auto& regs = liverpool->regs;
    auto& key = graphics_key;

    const bool db_enabled = regs.depth_buffer.DepthValid() || regs.depth_buffer.StencilValid();

    key.z_format = regs.depth_buffer.DepthValid() ? regs.depth_buffer.z_info.format
                                                  : AmdGpu::DepthBuffer::ZFormat::Invalid;
    key.stencil_format = regs.depth_buffer.StencilValid()
                             ? regs.depth_buffer.stencil_info.format
                             : AmdGpu::DepthBuffer::StencilFormat::Invalid;
    key.depth_clamp_enable = !regs.depth_render_override.disable_viewport_clamp;
    key.depth_clip_enable = regs.clipper_control.ZclipEnable();
    key.clip_space = regs.clipper_control.clip_space;
    key.provoking_vtx_last = regs.polygon_control.provoking_vtx_last;
    key.prim_type = regs.primitive_type;
    key.polygon_mode = regs.polygon_control.PolyMode();
    key.patch_control_points =
        regs.stage_enable.hs_en ? regs.ls_hs_config.hs_input_control_points : 0;
    key.logic_op = regs.color_control.rop3;
    key.depth_samples = db_enabled ? regs.depth_buffer.NumSamples() : 1;
    key.num_samples = key.depth_samples;
    key.cb_shader_mask = regs.color_shader_mask;

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    // First pass to fill render target information needed by shader recompiler
    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            // No attachment bound or writing to it is disabled.
            continue;
        }

        // Fill color target information
        auto& color_buffer = key.color_buffers[cb];
        color_buffer.data_format = col_buf.GetDataFmt();
        color_buffer.num_format = col_buf.GetNumberFmt();
        color_buffer.num_conversion = col_buf.GetNumberConversion();
        color_buffer.export_format = regs.color_export_format.GetFormat(cb);
        color_buffer.swizzle = col_buf.Swizzle();

        const auto& bc = regs.blend_control[cb];
        color_buffer.blend_self_scale =
            bc.enable && !col_buf.info.blend_bypass &&
            (bc.color_func == AmdGpu::BlendControl::BlendFunc::Min ||
             bc.color_func == AmdGpu::BlendControl::BlendFunc::Max) &&
            bc.color_src_factor == AmdGpu::BlendControl::BlendFactor::SrcColor &&
            bc.color_dst_factor == AmdGpu::BlendControl::BlendFactor::DstColor;
    }

    // Compile and bind shader stages
    if (!RefreshGraphicsStages()) {
        return false;
    }

    // Second pass to mask out render targets not written by shader and fill remaining info
    u8 color_samples = 0;
    bool all_color_samples_same = true;
    for (s32 cb = 0; cb < key.num_color_attachments && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (!col_buf || !target_mask) {
            continue;
        }
        if ((key.mrt_mask & (1u << cb)) == 0) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
            continue;
        }

        // Fill color blending information
        if (regs.blend_control[cb].enable && !col_buf.info.blend_bypass) {
            key.blend_controls[cb] = regs.blend_control[cb];
        }

        // Apply swizzle to target mask
        key.write_masks[cb] =
            vk::ColorComponentFlags{key.color_buffers[cb].swizzle.ApplyMask(target_mask)};

        // Fill color samples
        const u8 prev_color_samples = std::exchange(color_samples, col_buf.NumSamples());
        all_color_samples_same &= color_samples == prev_color_samples || prev_color_samples == 0;
        key.color_samples[cb] = color_samples;
        key.num_samples = std::max(key.num_samples, color_samples);
    }

    // Force all color samples to match depth samples to avoid unsupported MSAA configuration
    if (color_samples != 0) {
        const bool depth_mismatch = db_enabled && color_samples != key.depth_samples;
        if (!all_color_samples_same && !instance.IsMixedAnySamplesSupported() ||
            all_color_samples_same && depth_mismatch && !instance.IsMixedDepthSamplesSupported()) {
            key.color_samples.fill(key.depth_samples);
            key.num_samples = key.depth_samples;
        }
    }

    return true;
}

bool PipelineCache::RefreshGraphicsStages() {
    const auto& regs = liverpool->regs;
    auto& key = graphics_key;
    fetch_shader = nullptr;

    Shader::Backend::Bindings binding{};
    const auto bind_stage = [&](HwStage stage_in, SwStage stage_out) -> bool {
        const auto stage_in_idx = static_cast<u32>(stage_in);
        const auto stage_out_idx = static_cast<u32>(stage_out);
        // sotc: SOTC_STAGE_BINDINGS: the descriptors of this stage are numbered from 0 in its own
        // set. The buffer count is not reset: it indexes the push constant offsets, which all
        // the stages share.
        if (profile.stage_bindings) {
            binding.unified = 0;
        }
        // sotc: the bindings of the later stages follow from this one's: they wait for it.
        if (translation_pending || !regs.stage_enable.IsStageEnabled(stage_in_idx)) {
            key.stage_hashes[stage_out_idx] = 0;
            infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto* pgm = regs.ProgramForStage(stage_in_idx);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[stage_out_idx] = 0;
            infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto params = AmdGpu::GetParams(*pgm);
        std::tie(infos[stage_out_idx], modules[stage_out_idx], key.stage_hashes[stage_out_idx]) =
            GetProgram(stage_in, stage_out, params, binding, async_translate_ok);
        return !translation_pending;
    };

    infos.fill(nullptr);
    modules.fill(nullptr);

    bind_stage(HwStage::Fragment, SwStage::Fragment);

    const auto* fs_info = infos[static_cast<u32>(SwStage::Fragment)];
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;
    key.num_color_attachments = std::bit_width(key.mrt_mask);

    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Vertex, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHsEsGs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::Vs:
        bind_stage(HwStage::Vertex, SwStage::Vertex);
        break;
    default:
        LOG_WARNING(Render_Vulkan, "unimplemented shader stage {}", (u32)regs.stage_enable.raw);
        return false;
    }

    if (translation_pending) {
        return false;
    }

    const auto* vs_info = infos[static_cast<u32>(SwStage::Vertex)];
    if (vs_info && fetch_shader && !instance.IsVertexInputDynamicState()) {
        // Without vertex input dynamic state, the pipeline needs to specialize on format.
        // Stride will still be handled outside the pipeline using dynamic state.
        u32 vertex_binding = 0;
        for (const auto& attrib : fetch_shader->attributes) {
            const auto& buffer = attrib.GetSharp(*vs_info);
            ASSERT_MSG(vertex_binding < MaxVertexBufferCount,
                       "Vertex attribute binding count exceeded limit: {} >= {}", vertex_binding,
                       MaxVertexBufferCount);
            key.vertex_buffer_formats[vertex_binding++] =
                Vulkan::LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        }
    }

    return true;
}

bool PipelineCache::RefreshComputeKey() {
    Shader::Backend::Bindings binding{};
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto cs_params = AmdGpu::GetParams(cs_pgm);
    std::tie(infos[0], modules[0], compute_key.value) =
        GetProgram(HwStage::Compute, SwStage::Compute, cs_params, binding);
    return true;
}

vk::ShaderModule PipelineCache::CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                              const std::span<const u32>& code, size_t perm_idx,
                                              Shader::Backend::Bindings& binding,
                                              Shader::Pools* worker_pools,
                                              std::vector<u32>* spv_out) {
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.hw_stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
    // sotc: the frame columns are command-processor time; worker time is counted apart.
    std::optional<SotcStats::FrameTimer> frame_timer;
    if (!worker_pools) {
        frame_timer.emplace(SotcStats::FrameCompile);
        SotcStats::frame_compiles.fetch_add(1, std::memory_order_relaxed);
    }
    DumpShader(code, info.pgm_hash, info.hw_stage, perm_idx, "bin");
    const SotcStats::ScopedTimer stall_timer{SotcStats::shader_ns};
    SotcStats::shaders_compiled.fetch_add(1, std::memory_order_relaxed);

    auto phase_start = std::chrono::steady_clock::now();
    const auto phase = [&](int i) {
        if (SotcStats::kFpsLog) {
            const auto now = std::chrono::steady_clock::now();
            SotcStats::compile_phase_ns[i].fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - phase_start).count(),
                std::memory_order_relaxed);
            phase_start = now;
        }
    };
    const auto ir_program = Shader::TranslateProgram(code, worker_pools ? *worker_pools : pools,
                                                     info, runtime_info, profile);
    phase(0);
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
    DumpShader(spv, info.pgm_hash, info.hw_stage, perm_idx, "spv");
    phase(1);

    vk::ShaderModule module;

    auto patch = GetShaderPatch(info.pgm_hash, info.hw_stage, perm_idx, "spv", spv);
    phase(2);
    const bool is_patched = patch && EmulatorSettings.IsPatchShaders();
    if (is_patched) {
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.hw_stage, info.pgm_hash);
        module = CompileSPV(*patch, instance.GetDevice());
    } else {
        module = CompileSPV(spv, instance.GetDevice());
    }
    phase(3);

    if (spv_out) {
        *spv_out = std::move(spv);
    } else {
        RegisterShaderBinary(std::move(spv), info.pgm_hash, perm_idx);
    }
    phase(4);

    const auto name = GetShaderName(info.hw_stage, info.pgm_hash, perm_idx);
    Vulkan::SetObjectName(instance.GetDevice(), module, name);
    if (EmulatorSettings.IsShaderCollect()) {
        DebugState.CollectShader(name, info.sw_stage, module, spv, code,
                                 patch ? *patch : std::span<const u32>{}, is_patched);
    }
    return module;
}

PipelineCache::Result PipelineCache::GetProgram(HwStage hw_stage, SwStage sw_stage,
                                                const Shader::ShaderParams& params,
                                                Shader::Backend::Bindings& binding,
                                                bool async_ok) {
    // sotc: SOTC_ASYNC_COMPUTE_XLATE: a stage that may not wait for a worker but whose shader
    // is already on one (queued ahead by the command buffer scan) waits for that translation
    // instead of making a second one here. Compute only: installing finished jobs in the middle
    // of a graphics key would move the modules a previous stage's fetch_shader points into.
    if (hw_stage == HwStage::Compute && !async_ok && compute_prefetch &&
        pending_programs.contains(params.hash)) {
        WaitPendingTranslation(params.hash);
    }
    // sotc: copied only where CompileModule needs a mutable one (it was copied on every call).
    const auto& rt = BuildRuntimeInfo(hw_stage, sw_stage);
    if (async_ok && !program_cache.contains(params.hash) &&
        !in_place_programs.contains(params.hash)) {
        if (pending_programs.insert(params.hash).second) {
            QueueAsyncTranslate(hw_stage, sw_stage, params, rt, binding, nullptr, nullptr, 0);
        }
        translation_pending = true;
        return {nullptr, vk::ShaderModule{}, 0};
    }
    auto [it_pgm, new_program] = program_cache.try_emplace(params.hash);
    if (new_program) {
        it_pgm.value() = std::make_unique<Program>(hw_stage, sw_stage, params);
        auto& program = it_pgm.value();
        auto start = binding;
        auto runtime_info = rt;
        const auto module = CompileModule(program->info, runtime_info, params.code, 0, binding);
        auto spec = Shader::StageSpecialization(program->info, runtime_info, profile, start);
        const auto perm_hash = HashCombine(params.hash, 0);

        RegisterShaderMeta(program->info, spec.fetch_shader_data, spec, perm_hash, 0);
        program->AddPermut(module, std::move(spec));
        if (auto& fetch = program->modules[0].spec.fetch_shader_data; !fetch.Empty()) {
            fetch_shader = &fetch;
        }
        return std::make_tuple(&program->info, module, perm_hash);
    }

    auto& program = it_pgm.value();
    auto& info = program->info;
    info.PgmBase() = params.Base(); // Needs to be actualized for inline cbuffer address fixup
    info.UserData() = params.user_data;
    info.RefreshFlatBuf();

    // sotc: SOTC_PIPE_MEMO=1/2. The flattened buffer, the user data and the fetch code are the
    // memo's inputs. A hit skips the specialization and the search below; the bindings, the fetch
    // shader and the returned values are set the same way. Entries are only stored from this path
    // (an existing program), never for a translation still pending. Unset: none of the code below
    // runs; the locals only start empty.
    int memo_mode = PipeMemoStageMode();
    if (memo_mode != 0 && !PipeMemoOwnerThread()) {
        memo_mode = 0; // sotc: a call from another thread takes the slow path (counted)
    }
    Shader::Backend::Bindings memo_start{}; // the specialization starts from this
    Shader::TessellationDataConstantBuffer memo_tess{};
    StageMemo* memo_hit = nullptr;   // the way that answered the lookup
    StageMemo* memo_ways = nullptr;  // the set the lookup used (store target is chosen at store)
    u64 memo_gen = 0;
    bool memo_usable = false;        // false while the runtime memory generation is odd
    bool has_tess = false;
    if (memo_mode != 0) {
        has_tess = info.sw_stage == SwStage::TessellationControl ||
                   info.sw_stage == SwStage::TessellationEval;
        memo_start = binding;
        // Read before the slow path, so a store is tagged with the generation it started from.
        memo_gen = runtime_memory_generation();
        memo_usable = (memo_gen & 1) == 0;
        // Read as the specialization reads it (ForEachSharp / the tessellation runtime info).
        if (has_tess) {
            info.ReadTessConstantBuffer(memo_tess);
        }
        if (stage_memo.empty()) {
            stage_memo.resize(kStageMemoSets * kStageMemoWays);
        }
        memo_ways = &stage_memo[StageMemoSet(params.hash, hw_stage, sw_stage) * kStageMemoWays];
        g_pipe_memo.stage_lookups.fetch_add(1, std::memory_order_relaxed);
        for (size_t w = 0; memo_usable && !memo_hit && w < kStageMemoWays; ++w) {
            if (StageMemoHit(memo_ways[w], *program, hw_stage, sw_stage, params, rt, memo_start,
                             memo_tess, has_tess, memo_gen)) {
                memo_hit = &memo_ways[w];
            }
        }
        if (memo_hit) {
            g_pipe_memo.stage_hits.fetch_add(1, std::memory_order_relaxed);
            memo_hit->stamp = ++stage_memo_tick;
        }
        if (memo_hit && memo_mode == 1) {
            info.AddBindings(binding, Shader::UsesBindlessSamplers(info));
            auto& found = program->modules[memo_hit->perm];
            if (auto& fetch = found.spec.fetch_shader_data; !fetch.Empty()) {
                fetch_shader = &fetch;
            }
            return std::make_tuple(&program->info, found.module,
                                   HashCombine(params.hash, static_cast<size_t>(memo_hit->perm)));
        }
    }

    auto spec = Shader::StageSpecialization(info, rt, profile, binding);

    size_t perm_idx = program->modules.size();
    u64 perm_hash = HashCombine(params.hash, perm_idx);

    vk::ShaderModule module{};

    const auto it = std::ranges::find(program->modules, spec, &Program::Module::spec);
    if (it == program->modules.end() && async_ok && !program->translate_in_place) {
        if (std::ranges::find(program->pending_specs, spec) == program->pending_specs.end()) {
            QueueAsyncTranslate(hw_stage, sw_stage, params, rt, binding, program.get(), &spec,
                                perm_idx);
            program->pending_specs.push_back(spec);
        }
        translation_pending = true;
        if (memo_hit) { // =2 only: the memo found a permutation the slow path cannot give yet
            g_pipe_memo.mismatch_stage_pending.fetch_add(1, std::memory_order_relaxed);
        }
        return {nullptr, vk::ShaderModule{}, 0};
    }
    if (it == program->modules.end()) {
        auto new_info = Shader::Info(hw_stage, sw_stage, params);
        auto runtime_info = rt;
        module = CompileModule(new_info, runtime_info, params.code, perm_idx, binding);

        RegisterShaderMeta(info, spec.fetch_shader_data, spec, perm_hash, perm_idx);
        program->AddPermut(module, std::move(spec));
    } else {
        perm_idx = std::distance(program->modules.begin(), it);
        info.AddBindings(binding, Shader::UsesBindlessSamplers(info));
        module = it->module;
        perm_hash = HashCombine(params.hash, perm_idx);
    }
    if (memo_mode != 0) {
        if (memo_hit && memo_mode == 2) {
            // sotc: =2 only. The memo answer, as mode 1 would return it, against the slow result.
            // Each kind is counted on its own; the slow result is the one returned.
            const auto& memo_module = program->modules[memo_hit->perm];
            if (memo_hit->perm != perm_idx) {
                g_pipe_memo.mismatch_stage_perm.fetch_add(1, std::memory_order_relaxed);
            } else if (memo_module.module != module) {
                g_pipe_memo.mismatch_stage_module.fetch_add(1, std::memory_order_relaxed);
            }
            if (!(memo_module.spec.fetch_shader_data ==
                  program->modules[perm_idx].spec.fetch_shader_data)) {
                g_pipe_memo.mismatch_stage_fetch.fetch_add(1, std::memory_order_relaxed);
            }
            Shader::Backend::Bindings memo_after = memo_start;
            info.AddBindings(memo_after, Shader::UsesBindlessSamplers(info));
            if (!(memo_after == binding)) {
                g_pipe_memo.mismatch_stage_bindings.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (memo_usable) {
            StageMemo& target =
                memo_hit ? *memo_hit
                         : *StageMemoVictim(memo_ways, *program, hw_stage, sw_stage, params.hash);
            StageMemoStore(target, *program, hw_stage, sw_stage, params, rt, memo_start, memo_tess,
                           has_tess, memo_gen, perm_idx, binding);
        }
    }
    if (auto& fetch = program->modules[perm_idx].spec.fetch_shader_data; !fetch.Empty()) {
        fetch_shader = &fetch;
    }
    return std::make_tuple(&program->info, module, perm_hash);
}

std::optional<vk::ShaderModule> PipelineCache::ReplaceShader(vk::ShaderModule module,
                                                             std::span<const u32> spv_code) {
    last_graphics_pipeline = nullptr; // sotc: pipelines may be erased below
    last_compute_pipeline = nullptr;  // sotc: SOTC_PIPE_MEMO, same reason
    std::optional<vk::ShaderModule> new_module{};
    for (const auto& [_, program] : program_cache) {
        for (auto& m : program->modules) {
            if (m.module == module) {
                const auto& d = instance.GetDevice();
                d.destroyShaderModule(m.module);
                m.module = CompileSPV(spv_code, d);
                new_module = m.module;
            }
        }
    }
    if (module_related_pipelines.contains(module)) {
        auto& pipeline_keys = module_related_pipelines[module];
        for (auto& key : pipeline_keys) {
            if (std::holds_alternative<GraphicsPipelineKey>(key)) {
                auto& graphics_key = std::get<GraphicsPipelineKey>(key);
                graphics_pipelines.erase(graphics_key);
            } else if (std::holds_alternative<ComputePipelineKey>(key)) {
                auto& compute_key = std::get<ComputePipelineKey>(key);
                compute_pipelines.erase(compute_key);
            }
        }
    }
    return new_module;
}

std::string PipelineCache::GetShaderName(Shader::HwStage stage, u64 hash,
                                         std::optional<size_t> perm) {
    if (perm) {
        return fmt::format("{}_{:#018x}_{}", stage, hash, *perm);
    }
    return fmt::format("{}_{:#018x}", stage, hash);
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage,
                               size_t perm_idx, std::string_view ext) {
    if (!EmulatorSettings.IsDumpShaders()) {
        return;
    }

    using namespace Common::FS;
    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create};
    file.WriteSpan(code);
}

std::optional<std::vector<u32>> PipelineCache::GetShaderPatch(u64 hash, Shader::HwStage stage,
                                                              size_t perm_idx, std::string_view ext,
                                                              std::span<const u32> generated) {

    using namespace Common::FS;
    const auto patch_dir = GetUserPath(PathType::ShaderDir) / "patch";
    // sotc: the patch folder is listed once; two filesystem checks per compiled shader cost
    // ~0.34 s over a 200 s attract run with an empty shader cache (7% of the compile time).
    static const std::unordered_set<std::string> patch_files = [&] {
        std::unordered_set<std::string> names;
        std::error_code ec;
        if (!std::filesystem::exists(patch_dir, ec)) {
            std::filesystem::create_directories(patch_dir, ec);
        }
        for (const auto& entry : std::filesystem::directory_iterator(patch_dir, ec)) {
            names.insert(entry.path().filename().string());
        }
        return names;
    }();
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    if (!patch_files.contains(filename)) {
        return {};
    }
    const auto filepath = patch_dir / filename;
    const u64 generated_hash = XXH3_64bits(generated.data(), generated.size_bytes());
    auto base_path = filepath;
    base_path += ".base";
    if (std::filesystem::exists(base_path)) {
        const auto base_file = IOFile{base_path, FileAccessMode::Read};
        std::string text(base_file.GetSize(), '\0');
        base_file.ReadSpan(std::span<char>{text});
        const u64 base_hash = std::strtoull(text.c_str(), nullptr, 16);
        if (base_hash != generated_hash) {
            LOG_WARNING(Loader,
                        "Not applying patch {}: made for a different module ({:#018x}, this GPU "
                        "generated {:#018x})",
                        filename, base_hash, generated_hash);
            return {};
        }
    } else {
        LOG_INFO(Loader, "Patch {} has no .base file; generated module hash {:#018x}", filename,
                 generated_hash);
    }
    const auto file = IOFile{patch_dir / filename, FileAccessMode::Read};
    std::vector<u32> code(file.GetSize() / sizeof(u32));
    file.Read(code);
    return code;
}
} // namespace Vulkan
