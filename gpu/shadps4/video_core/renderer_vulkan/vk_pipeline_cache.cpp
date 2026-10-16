// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <thread>
#include <ranges>
#include <xxhash.h>

#include "common/elf_info.h"
#include "common/hash.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/sotc_frame_stats.h"
#include "video_core/sotc_stats.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/vk_driver_cache.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/sotc_stats.h"

namespace Vulkan {

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

namespace {
void BuildCommonRuntimeInfo(Shader::RuntimeInfo& info, const auto& program) {
    info.props.num_user_data = program.settings.num_user_regs;
    info.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
    info.props.num_allocated_vgprs = program.NumVgprs();
    info.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
    info.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
    info.props.fp_round_mode32 = program.settings.fp_round_mode32;
    info.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
}
} // namespace

// sotc: the vertex and pixel parts are shared with the draw preparation helper (vk_draw_prep),
// which builds them from its own copy of the registers.
void PipelineCache::FillHwVertexInfo(Shader::RuntimeInfo& info, const AmdGpu::Regs& regs) const {
    BuildCommonRuntimeInfo(info, regs.vs_program);
    info.hw.vs.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
    info.hw.vs.num_outputs = MapOutputs(info.hw.vs.outputs, regs.vs_output_control);
    info.hw.vs.emulate_depth_negative_one_to_one =
        !instance.IsDepthClipControlSupported() &&
        regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
    info.hw.vs.clip_disable = regs.IsClipDisabled();
}

void PipelineCache::FillHwFragmentInfo(Shader::RuntimeInfo& info, const AmdGpu::Regs& regs,
                                       const GraphicsPipelineKey& key) const {
    BuildCommonRuntimeInfo(info, regs.ps_program);
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
    // From Pink-shadPS4 (luizgustavs). SotC only; SOTC_EARLY_Z=0/1 overrides.
    static const bool early_z_enabled = [] {
        if (const char* v = std::getenv("SOTC_EARLY_Z")) {
            return std::atoi(v) != 0;
        }
        return Common::ElfInfo::Instance().GameSerial() == "CUSA08809";
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
        info.hw.fs.color_buffers[i] = key.color_buffers[i];
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
}

void PipelineCache::FillSwVertexInfo(Shader::RuntimeInfo& info, const AmdGpu::Regs& regs,
                                     const DrawIndirectParams& params) {
    info.sw.vs.step_rate_0 = regs.vgt_instance_step_rate_0;
    info.sw.vs.step_rate_1 = regs.vgt_instance_step_rate_1;
    info.sw.vs.vertex_sgpr_offset = params.vertex_sgpr_offset;
    info.sw.vs.instance_sgpr_offset = params.instance_sgpr_offset;
    info.sw.vs.tess_emulated_primitive =
        regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
        regs.primitive_type == AmdGpu::PrimitiveType::QuadList;
}

const Shader::RuntimeInfo& PipelineCache::BuildRuntimeInfo(HwStage stage, SwStage l_stage) {
    auto& info = runtime_infos[u32(l_stage)];
    const auto& regs = liverpool->regs;
    const auto BuildCommon = [&](const auto& program) { BuildCommonRuntimeInfo(info, program); };
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
    case HwStage::Vertex:
        FillHwVertexInfo(info, regs);
        break;
    case HwStage::Fragment:
        FillHwFragmentInfo(info, regs, graphics_key);
        break;
    case HwStage::Compute: {
        const auto& cs_pgm = liverpool->GetCsRegs();
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
        break;
    }
    default:
        break;
    }
    switch (l_stage) {
    case SwStage::Vertex:
        FillSwVertexInfo(info, regs, draw_indirect_params);
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

namespace {
u64 EnvU64(const char* name, u64 fallback) {
    const char* v = std::getenv(name);
    return v && *v ? std::strtoull(v, nullptr, 10) : fallback;
}
} // namespace

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_, u32 sparse_page_shift)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      desc_heap{instance, scheduler.GetWorkSemaphore(), DescriptorHeapSizes} {
    const auto& vk12_props = instance.GetVk12Properties();
    // Wave64 uniform branches on every driver. Tested on a Radeon RX 6650 XT (32-wide path):
    // off, the shadows next to the Wander and Agro on grass get horizontal stripes; on, the
    // image is right and the characters are not black in the shade (save with 5 colossi).
    Shader::Optimization::SetWave64UniformBranchesDefault(true);
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
        .support_fp16_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat16),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_fp32_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat32),
        .support_fp64_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat64),
        .support_fp64_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat64),
        .support_fp64_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat64),
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
        .supports_amd_shader_explicit_vertex_parameter =
            instance_.IsAmdShaderExplicitVertexParameterSupported(),
        .supports_fragment_shader_barycentric = instance_.IsFragmentShaderBarycentricSupported(),
        .supports_shader_subgroup_clock = instance_.IsShaderSubgroupClockSupported(),
        .needs_manual_interpolation = instance.IsFragmentShaderBarycentricSupported() &&
                                      instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        // sotc: also whenever the shaders are lowered for fewer than 64 lanes (AMD on the 32-wide
        // path): without the barriers the GPU hung ~2 min into the intro (issue #7).
        .needs_lds_barriers = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                              instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp ||
                              instance.SubgroupSize() < 64,
        .needs_buffer_offsets = instance.StorageMinAlignment() > 4,
        .needs_unorm_fixup = instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_clip_distance_emulation = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .supports_shader_stencil_export = instance_.IsShaderStencilExportSupported(),
    };
    InitializeDriverCache();
    WarmUp();
    SaveDriverCache(true);
    // sotc: graphics pipelines new to the cache are created on these workers (the NVIDIA
    // compiler takes 10-40 ms each). SOTC_ASYNC_PIPELINE_WORKERS=<n>, 0 disables them.
    const u64 hw = std::max(1u, std::thread::hardware_concurrency());
    const u64 workers = EnvU64("SOTC_ASYNC_PIPELINE_WORKERS", std::clamp<u64>(hw / 4, 1, 4));
    if (workers > 0) {
        static const bool separate_caches = EnvU64("SOTC_WORKER_PIPELINE_CACHES", 1) != 0;
        for (u64 i = 0; separate_caches && i < workers; ++i) {
            auto [result, cache] = instance.GetDevice().createPipelineCacheUnique({});
            if (result != vk::Result::eSuccess) {
                worker_caches.clear();
                break;
            }
            worker_caches.push_back(std::move(cache));
        }
        async_compiler = std::make_unique<AsyncCompiler>(
            static_cast<unsigned>(workers), 256,
            [](unsigned) { Common::SetCurrentThreadName("sotc:PipelineCompile"); });
    }
    // sotc (SOTC_DRAW_PREP): graphics selection prepared on the DrawScan thread (vk_draw_prep.h).
    if (DrawPrepMode() != 0 && liverpool->draw_scan) {
        draw_prep = std::make_unique<DrawPrep>(*this);
        liverpool->draw_scan->SetPreparer(draw_prep.get());
    }
}

PipelineCache::~PipelineCache() {
    if (draw_prep) {
        liverpool->draw_scan->Stop(); // the scanner thread calls into draw_prep
    }
    // Workers reference the pending jobs: let them finish before the jobs are destroyed.
    if (async_compiler) {
        async_compiler->Stop();
        PublishGraphics(true);
        pending_compute.clear(); // finished optimized builds, dropped with the cache
        deferred_compute.clear();
        async_compiler.reset();
        if (MergeWorkerCaches(), !worker_caches.empty()) {
            ++num_new_pipelines; // the merged pipelines still need the final snapshot
        }
    }
    // Final driver cache snapshot (nothing called Sync before): the pipelines of the last
    // checkpoint interval are kept for the next session.
    if (num_new_pipelines != driver_saved_pipelines) {
        SaveDriverCache(true);
    }
}

void PipelineCache::OnFrame() {
    if (!deferred_compute.empty()) {
        StartDeferredCompute();
    }
    if (num_new_pipelines != driver_saved_pipelines) {
        SaveDriverCache();
    }
}

void PipelineCache::StartDeferredCompute() {
    // SOTC_COMPUTE_OPT_DELAY_MS: quiet time (no new pipeline) before optimized builds start.
    static const auto delay = std::chrono::milliseconds(EnvU64("SOTC_COMPUTE_OPT_DELAY_MS", 3000));
    if (!async_compiler || std::chrono::steady_clock::now() - last_new_pipeline_time < delay) {
        return;
    }
    for (auto& pending : deferred_compute) {
        PendingCompute& job = *pending;
        const ComputePipelineKey key = job.key;
        const bool started = job.job.Start(*async_compiler, [this, j = &job] {
            return std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                     WorkerCache(), j->key, *j->info_copy,
                                                     j->module, j->sdata, false, false);
        });
        if (started) {
            pending_compute.emplace(key, std::move(pending));
        }
    }
    deferred_compute.clear();
}

bool PipelineCache::MergeWorkerCaches() {
    // vkMergePipelineCaches needs the destination externally synchronized: only the command
    // processor creates pipelines with it, and no snapshot may be reading it.
    if (worker_caches.empty()) {
        return true;
    }
    if (snapshot_in_flight.load(std::memory_order_acquire)) {
        return false;
    }
    boost::container::small_vector<vk::PipelineCache, 8> sources;
    for (const auto& cache : worker_caches) {
        sources.push_back(*cache);
    }
    const auto result = instance.GetDevice().mergePipelineCaches(*pipeline_cache, sources);
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(Render, "Could not merge worker pipeline caches: {}", vk::to_string(result));
    }
    return true;
}

bool PipelineCache::AsyncGraphicsEnabled() const {
    // SOTC_ASYNC_PIPELINES=0 builds every graphics pipeline on the command processor, as before.
    static const bool enabled = EnvU64("SOTC_ASYNC_PIPELINES", 1) != 0;
    return enabled && async_compiler && instance.IsVertexInputDynamicState() &&
           !EmulatorSettings.IsShaderCollect();
}

PipelineCache::PendingGraphics* PipelineCache::EnqueueGraphics() {
    auto pending = std::make_unique<PendingGraphics>();
    PendingGraphics& job = *pending;
    job.key = graphics_key;
    // Snapshot of everything the constructor reads: the live Program infos are rewritten by
    // the next draws (user data, flattened buffer), and fetch_shader points into a vector
    // that can grow.
    for (u32 i = 0; i < MaxShaderStages; ++i) {
        job.live_infos[i] = infos[i];
        if (!infos[i]) {
            continue;
        }
        job.user_data[i].assign(infos[i]->user_data.begin(), infos[i]->user_data.end());
        job.info_copy[i] = std::make_unique<Shader::Info>(*infos[i]);
        job.info_copy[i]->user_data = job.user_data[i];
        job.snap_infos[i] = job.info_copy[i].get();
    }
    job.runtime_infos = runtime_infos;
    job.modules = modules;
    if (fetch_shader) {
        job.fetch = *fetch_shader;
    }
    job.queued_flip = SotcStats::flips.load(std::memory_order_relaxed);
    job.queued = std::chrono::steady_clock::now();
    const bool started = job.job.Start(*async_compiler, [this, j = &job] {
        return std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, j->key, WorkerCache(), j->snap_infos,
            j->runtime_infos, j->fetch ? &*j->fetch : nullptr, j->modules, j->sdata, false);
    });
    if (!started) {
        return nullptr; // queue full: build it here
    }
    return pending_graphics.emplace(graphics_key, std::move(pending)).first->second.get();
}

void PipelineCache::PublishGraphics(bool wait_all) {
    boost::container::small_vector<GraphicsPipelineKey, 16> done;
    for (auto& [key, pending] : pending_graphics) {
        if (wait_all) {
            pending->job.Wait();
        }
        if (pending->job.Ready()) {
            done.push_back(key);
        }
    }
    for (const auto& key : done) {
        auto node = pending_graphics.find(key);
        PendingGraphics& job = *node->second;
        std::unique_ptr<GraphicsPipeline> pipeline = job.job.Take();
        pipeline->PublishStages(job.live_infos);
        const auto [it, is_new] = graphics_pipelines.try_emplace(key);
        if (is_new) {
            it.value() = std::move(pipeline);
            RegisterPipelineData(key, std::hash<GraphicsPipelineKey>{}(key), job.sdata);
            ++num_new_pipelines;
            last_new_pipeline_time = std::chrono::steady_clock::now();
            SotcStats::async_compiles.fetch_add(1, std::memory_order_relaxed);
        }
        if (last_pending == &job) {
            last_pending = nullptr;
        }
        pending_graphics.erase(node);
    }
    if (!done.empty() && !wait_all) {
        SaveDriverCache();
    }
}

void PipelineCache::PublishCompute() {
    boost::container::small_vector<ComputePipelineKey, 16> done;
    for (auto& [key, pending] : pending_compute) {
        if (pending->job.Ready()) {
            done.push_back(key);
        }
    }
    for (const auto& key : done) {
        auto node = pending_compute.find(key);
        PendingCompute& job = *node->second;
        std::unique_ptr<ComputePipeline> optimized = job.job.Take();
        std::array<const Shader::Info*, Shader::MaxStageTypes> stages{};
        stages[u32(Shader::SwStage::Compute)] = job.live_info;
        optimized->PublishStages(stages);
        if (const auto it = compute_pipelines.find(key); it != compute_pipelines.end()) {
            // Commands recorded so far use the unoptimized pipeline: keep it until the GPU has
            // executed them (the current tick includes everything the recorder holds).
            scheduler.DeferOperation(
                [old = std::exchange(it.value(), std::move(optimized))]() mutable { old.reset(); });
            SotcStats::compute_swaps.fetch_add(1, std::memory_order_relaxed);
        }
        pending_compute.erase(node);
    }
}

const GraphicsPipeline* PipelineCache::WaitPendingGraphics() {
    PendingGraphics* job = std::exchange(last_pending, nullptr);
    if (!job) {
        return nullptr;
    }
    {
        SotcFrameStats::Scope timing{SotcFrameStats::Kind::Graphics};
        job->job.Wait();
    }
    SotcStats::async_waits.fetch_add(1, std::memory_order_relaxed);
    const GraphicsPipelineKey key = job->key;
    PublishGraphics(false);
    const auto it = graphics_pipelines.find(key);
    return it != graphics_pipelines.end() ? it->second.get() : nullptr;
}

bool PipelineCache::PendingTooOld() const {
    if (!last_pending) {
        return false;
    }
    // SOTC_ASYNC_PIPELINE_MAX_FRAMES / _MAX_MS bound how long a skipped object stays missing.
    static const u64 max_frames = EnvU64("SOTC_ASYNC_PIPELINE_MAX_FRAMES", 4);
    static const u64 max_ms = EnvU64("SOTC_ASYNC_PIPELINE_MAX_MS", 100);
    const u64 frames = SotcStats::flips.load(std::memory_order_relaxed) - last_pending->queued_flip;
    const auto age = std::chrono::steady_clock::now() - last_pending->queued;
    return frames > max_frames || age > std::chrono::milliseconds(max_ms);
}

bool PipelineCache::StagesWriteMemory() const {
    for (const Shader::Info* info : infos) {
        if (!info) {
            continue;
        }
        for (const auto& buffer : info->buffers) {
            if (buffer.is_written) {
                return true;
            }
        }
        for (const auto& image : info->images) {
            if (image.is_written) {
                return true;
            }
        }
    }
    return false;
}

void PipelineCache::InitializeDriverCache() {
    const auto properties = instance.GetPhysicalDevice().getProperties();
    DriverCache::Identity identity{properties.vendorID, properties.deviceID, {}};
    std::memcpy(identity.uuid.data(), properties.pipelineCacheUUID.data(), identity.uuid.size());
    driver_cache_name = fmt::format("driver_{:x}_{:x}_{:016x}", identity.vendor, identity.device,
                                    XXH3_64bits(identity.uuid.data(), identity.uuid.size()));
    std::vector<u8> initial;
    if (EmulatorSettings.IsPipelineCacheEnabled()) {
        auto& storage = Storage::DataBase::Instance();
        storage.Open();
        std::vector<u8> file;
        storage.Load(Storage::BlobType::DriverBinary, driver_cache_name, file);
        initial = DriverCache::Decode(file, identity);
        if (!file.empty() && initial.empty()) {
            LOG_WARNING(Render, "Ignoring invalid/incompatible Vulkan driver cache");
        }
    }
    vk::PipelineCacheCreateInfo info{.initialDataSize = initial.size(),
                                    .pInitialData = initial.data()};
    auto [result, cache] = instance.GetDevice().createPipelineCacheUnique(info);
    if (result != vk::Result::eSuccess && !initial.empty()) {
        LOG_WARNING(Render, "Driver rejected cache ({}); starting empty", vk::to_string(result));
        auto [empty_result, empty_cache] = instance.GetDevice().createPipelineCacheUnique({});
        result = empty_result;
        cache = std::move(empty_cache);
        initial.clear();
    }
    ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
               vk::to_string(result));
    pipeline_cache = std::move(cache);
    LOG_INFO(Render, "Vulkan driver cache restored {} bytes before pipeline preload", initial.size());
}

void PipelineCache::SaveDriverCache(bool force) {
    if (!EmulatorSettings.IsPipelineCacheEnabled() || !Storage::DataBase::Instance().IsOpened()) return;
    const auto now = std::chrono::steady_clock::now();
    if (!force && (num_new_pipelines == driver_saved_pipelines ||
                   now - driver_save_time < std::chrono::seconds(30))) return;
    // sotc: checkpoints wait for a quiet moment (no new pipeline for 2 s) instead of landing in
    // a compile burst, and the snapshot (4-9 ms) runs on a compiler worker. The cache is
    // internally synchronized. SOTC_DRIVER_CACHE_ASYNC=0 takes it inline, as before.
    static const bool async_snapshot = EnvU64("SOTC_DRIVER_CACHE_ASYNC", 1) != 0;
    // The quiet gate is soft: after 120 s without a checkpoint, take one anyway (the process
    // ends with std::_Exit on window close, so no final snapshot is guaranteed).
    if (!force && now - last_new_pipeline_time < std::chrono::seconds(2) &&
        now - driver_save_time < std::chrono::seconds(120)) return;
    // One snapshot at a time: the merge below must not overlap a worker reading the cache.
    if (snapshot_in_flight.load(std::memory_order_acquire)) {
        if (!force) return;
        while (snapshot_in_flight.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    if (!MergeWorkerCaches()) return;
    if (!force && async_snapshot && async_compiler) {
        snapshot_in_flight.store(true, std::memory_order_release);
        const bool queued = async_compiler->Submit([this] {
            struct Done {
                std::atomic<bool>& flag;
                ~Done() {
                    flag.store(false, std::memory_order_release);
                }
            } done{snapshot_in_flight};
            auto [result, bytes] = instance.GetDevice().getPipelineCacheData(*pipeline_cache);
            if (result == vk::Result::eSuccess && bytes.size() >= 32 &&
                bytes.size() <= DriverCache::MaxBytes &&
                Storage::DataBase::Instance().IsOpened()) {
                Storage::DataBase::Instance().Save(Storage::BlobType::DriverBinary,
                                                  driver_cache_name, std::move(bytes));
            }
        });
        if (queued) {
            driver_saved_pipelines = num_new_pipelines;
            driver_save_time = now;
            return;
        }
        snapshot_in_flight.store(false, std::memory_order_release);
    }
    SotcFrameStats::Scope timing{SotcFrameStats::Kind::CacheSnapshot};
    auto [result, bytes] = instance.GetDevice().getPipelineCacheData(*pipeline_cache);
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(Render, "Could not snapshot Vulkan driver cache: {}", vk::to_string(result));
        return;
    }
    if (bytes.size() >= 32 && bytes.size() <= DriverCache::MaxBytes) {
        const auto size = bytes.size();
        Storage::DataBase::Instance().Save(Storage::BlobType::DriverBinary, driver_cache_name,
                                          std::move(bytes));
        LOG_INFO(Render, "Queued Vulkan driver cache checkpoint: {} bytes", size);
        driver_saved_pipelines = num_new_pipelines;
        driver_save_time = now;
    }
}

// sotc: SOTC_SEL_FAST=1 reuses the previous graphics selection when nothing it depends on
// changed (see FastSelection); 2 also runs the full selection, uses its result and counts any
// difference (SOTC sel line). 0 (default for now) always runs the full selection.
static u32 SelFastMode() {
    static const u32 mode = [] {
        const char* v = std::getenv("SOTC_SEL_FAST");
        return v && !EmulatorSettings.IsShaderCollect() ? static_cast<u32>(std::strtoul(v, nullptr, 10))
                                                        : 0u;
    }();
    return mode;
}

bool PipelineCache::TryFastSelection(const DrawIndirectParams& params) {
    const auto miss = [](u32 reason) {
        SotcStats::Bump(SotcStats::sel_fast_miss[reason]);
        return false;
    };
    const FastSelection& s = fast_sel;
    if (!s.valid) {
        return miss(0);
    }
    if (s.regs_seq != liverpool->sel_regs_seq) {
        return miss(1);
    }
    if (s.params.vertex_sgpr_offset != params.vertex_sgpr_offset ||
        s.params.instance_sgpr_offset != params.instance_sgpr_offset) {
        return miss(2);
    }
    const auto& regs = liverpool->regs;
    Shader::Backend::Bindings binding{};
    // Same order as RefreshGraphicsStages (bindings are numbered across stages).
    for (const SwStage stage : {SwStage::Fragment, SwStage::Vertex}) {
        const u32 i = static_cast<u32>(stage);
        if ((s.stage_mask & (1u << i)) == 0) {
            continue;
        }
        const FastStage& st = s.stages[i];
        const auto* pgm = regs.ProgramForStage(static_cast<u32>(st.hw));
        if (!pgm || !pgm->Address<u32*>()) {
            return miss(3);
        }
        // The code at an unchanged address may have been replaced.
        const auto p = AmdGpu::GetParams(*pgm);
        if (p.hash != st.hash) {
            return miss(3);
        }
        if (binding != st.start) {
            return miss(4);
        }
        // Everything read from guest memory, read again here exactly as the full selection
        // does (walker clean loads, faults and precise readbacks on this thread).
        auto& info = st.program->info;
        info.pgm_base = p.Base();
        info.user_data = p.user_data;
        info.RefreshFlatBuf();
        const auto& m = st.program->spec_memos[st.memo_idx];
        if (!m.valid || m.fill_seq != st.memo_fill_seq || m.pgm_base != info.pgm_base ||
            m.flat.size() != info.flattened_ud_buf.size() ||
            std::memcmp(m.flat.data(), info.flattened_ud_buf.data(),
                        m.flat.size() * sizeof(u32)) != 0) {
            return miss(5);
        }
        if (st.fetch_vs) {
            Shader::Gcn::FetchShaderData now;
            Shader::Gcn::ParseFetchShader(info, now);
            const auto& spec_fetch = st.program->modules[m.perm_idx].spec.fetch_shader_data;
            if (!(now == spec_fetch) || now.attributes.size() != m.vsharps.size()) {
                return miss(6);
            }
            for (size_t a = 0; a < m.vsharps.size(); ++a) {
                const AmdGpu::Buffer sharp = now.attributes[a].GetSharp(info);
                if (std::memcmp(&sharp, &m.vsharps[a], sizeof(sharp)) != 0) {
                    return miss(7);
                }
            }
        }
        info.AddBindings(binding);
    }
    graphics_key = s.key;
    infos = s.infos;
    modules = s.modules;
    fetch_shader = nullptr;
    if (s.stage_mask & (1u << static_cast<u32>(SwStage::Vertex))) {
        const FastStage& st = s.stages[static_cast<u32>(SwStage::Vertex)];
        // Re-derived each time: AddPermut may move the modules.
        auto& fetch =
            st.program->modules[st.program->spec_memos[st.memo_idx].perm_idx].spec.fetch_shader_data;
        if (!fetch.Empty()) {
            fetch_shader = &fetch;
        }
    }
    SotcStats::Bump(SotcStats::sel_fast_hit);
    if (!fast_regs_snapshot.empty() && fast_regs_snapshot_seq == s.regs_seq) {
        const auto& ignored = SotcSel::VerifyIgnoredWords();
        for (u32 w = 0; w < AmdGpu::Regs::NumRegs; ++w) {
            if (!ignored.test(w) && regs.reg_array[w] != fast_regs_snapshot[w]) {
                SotcStats::sel_regs_unnoticed.fetch_add(1, std::memory_order_relaxed);
                static int shown = 0;
                if (shown++ < 32) {
                    std::fprintf(stderr,
                                 "SOTC sel verify: register word %#x changed without a sequence "
                                 "bump (%#x -> %#x)\n",
                                 w, fast_regs_snapshot[w], regs.reg_array[w]);
                }
                break;
            }
        }
    }
    return true;
}

void PipelineCache::CommitFastSelection(u64 regs_seq) {
    fast_build.valid = true;
    fast_build.regs_seq = regs_seq;
    fast_build.params = draw_indirect_params;
    fast_build.key = graphics_key;
    fast_build.infos = infos;
    fast_build.modules = modules;
    fast_sel = fast_build;
    if (SelFastMode() == 2 && fast_commits++ % 64 == 0) {
        fast_regs_snapshot.assign(liverpool->regs.reg_array.begin(),
                                  liverpool->regs.reg_array.end());
        fast_regs_snapshot_seq = regs_seq;
    }
}

void PipelineCache::VerifyFastSelection(
    const GraphicsPipelineKey& fast_key,
    const std::array<const Shader::Info*, MaxShaderStages>& fast_infos,
    const std::array<vk::ShaderModule, MaxShaderStages>& fast_modules,
    const Shader::Gcn::FetchShaderData* fast_fetch, bool prepared) {
    const char* what = nullptr;
    if (std::memcmp(&fast_key, &graphics_key, sizeof(GraphicsPipelineKey)) != 0) {
        what = "key";
    }
    for (u32 i = 0; i < MaxShaderStages && !what; ++i) {
        if (fast_infos[i] != infos[i] || (infos[i] && fast_modules[i] != modules[i])) {
            what = "stage";
        }
    }
    if (!what && (!fast_fetch != !fetch_shader || (fast_fetch && !(*fast_fetch == *fetch_shader)))) {
        what = "fetch shader";
    }
    if (!what) {
        return;
    }
    (prepared ? SotcStats::prep_mismatch : SotcStats::sel_fast_mismatch)
        .fetch_add(1, std::memory_order_relaxed);
    static int shown = 0;
    if (shown++ < 32) {
        std::fprintf(stderr,
                     "SOTC %s verify: %s selection differs (%s): vs %#llx ps %#llx, "
                     "registers seq %llu\n",
                     prepared ? "prep" : "sel", prepared ? "prepared" : "reused", what,
                     static_cast<unsigned long long>(
                         graphics_key.stage_hashes[static_cast<u32>(SwStage::Vertex)]),
                     static_cast<unsigned long long>(
                         graphics_key.stage_hashes[static_cast<u32>(SwStage::Fragment)]),
                     static_cast<unsigned long long>(liverpool->sel_regs_seq));
    }
    static const bool fatal =
        EnvU64("SOTC_SEL_VERIFY_FATAL", 0) != 0 || EnvU64("SOTC_PREP_VERIFY_FATAL", 0) != 0;
    if (fatal) {
        std::abort();
    }
}

const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(const DrawIndirectParams params,
                                                           bool allow_async) {
    last_pending = nullptr;
    if (!pending_graphics.empty()) {
        PublishGraphics(false);
    }
    draw_indirect_params = params;
    // sotc: how often the registers selection reads are unchanged since the previous selection
    // (the fast-path ceiling of thread-plan 1D; SOTC sel line with SOTC_CMD_STATS=1).
    SotcStats::Bump(SotcStats::sel_draws);
    if (liverpool->sel_regs_seq == last_sel_regs_seq) {
        SotcStats::Bump(SotcStats::sel_regs_same);
    }
    last_sel_regs_seq = liverpool->sel_regs_seq;
    const u32 sel_mode = SelFastMode();
    // sotc (SOTC_DRAW_PREP): the selection the draw preparation helper proposed for this draw,
    // proven here before use (vk_draw_prep.cpp); mode 2 checks it against the full selection.
    const auto* prepared =
        static_cast<const PreparedDraw*>(std::exchange(liverpool->prepared_draw, nullptr));
    bool fast = false;
    bool from_prep = false;
    if (draw_prep) {
        if (!prepared) {
            SotcStats::Bump(SotcStats::prep_none);
        } else {
            from_prep = fast = TryPreparedSelection(*prepared, params);
        }
    }
    if (!fast) {
        fast = sel_mode != 0 && TryFastSelection(params);
    }
    if (fast && (from_prep ? DrawPrepMode() == 2 : sel_mode == 2)) {
        // Verify: the full selection's result is the one used.
        const auto fast_key = graphics_key;
        const auto fast_infos = infos;
        const auto fast_modules = modules;
        const auto* fast_fetch = fetch_shader;
        std::optional<Shader::Gcn::FetchShaderData> fetch_copy;
        if (fast_fetch) {
            fetch_copy = *fast_fetch;
        }
        fast = false;
        if (!RefreshGraphicsKey()) {
            return nullptr;
        }
        VerifyFastSelection(fast_key, fast_infos, fast_modules,
                            fetch_copy ? &*fetch_copy : nullptr, from_prep);
    } else if (!fast && !RefreshGraphicsKey()) {
        return nullptr;
    }
    if (fast) {
        // Only an existing pipeline: building or queueing one needs the runtime infos of a
        // full selection.
        if (last_graphics &&
            std::memcmp(&graphics_key, &last_graphics_key, sizeof(GraphicsPipelineKey)) == 0) {
            return last_graphics;
        }
        if (const auto found = graphics_pipelines.find(graphics_key);
            found != graphics_pipelines.end()) {
            last_graphics_key = graphics_key;
            last_graphics = found->second.get();
            return last_graphics;
        }
        SotcStats::Bump(SotcStats::sel_fast_fallback);
        if (!RefreshGraphicsKey()) {
            return nullptr;
        }
    }
    // sotc: consecutive draws mostly use the same pipeline. The key is fully rebuilt above (it
    // starts zeroed), so an identical key is the same map entry; pipelines are never removed
    // outside shader collection. SOTC_PIPE_MEMO=0 always looks the key up.
    static const bool pipe_memo =
        EnvU64("SOTC_PIPE_MEMO", 1) != 0 && !EmulatorSettings.IsShaderCollect();
    if (pipe_memo && last_graphics &&
        std::memcmp(&graphics_key, &last_graphics_key, sizeof(GraphicsPipelineKey)) == 0) {
        return last_graphics;
    }
    if (!pending_graphics.empty() || (allow_async && AsyncGraphicsEnabled())) {
        if (const auto found = graphics_pipelines.find(graphics_key);
            found != graphics_pipelines.end()) {
            last_graphics_key = graphics_key;
            last_graphics = found->second.get();
            return last_graphics;
        }
        PendingGraphics* job = nullptr;
        if (const auto it = pending_graphics.find(graphics_key); it != pending_graphics.end()) {
            job = it->second.get();
        } else if (allow_async && AsyncGraphicsEnabled() &&
                   !infos[u32(Shader::SwStage::TessellationControl)] &&
                   !infos[u32(Shader::SwStage::TessellationEval)]) {
            job = EnqueueGraphics();
        }
        if (job) {
            last_pending = job;
            if (allow_async) {
                return nullptr; // the caller skips the draw or waits
            }
            return WaitPendingGraphics();
        }
    }
    const auto [it, is_new] = graphics_pipelines.try_emplace(graphics_key);
    if (is_new) {
        SotcFrameStats::Scope timing{SotcFrameStats::Kind::Graphics};
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(graphics_key);
        LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);
        const SotcStats::ScopedTimer stall_timer{SotcStats::pipeline_ns};
        SotcStats::pipelines_new.fetch_add(1, std::memory_order_relaxed);

        GraphicsPipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, graphics_key, *pipeline_cache, infos,
            runtime_infos, fetch_shader, modules, sdata, false);

        RegisterPipelineData(graphics_key, pipeline_hash, sdata);
        ++num_new_pipelines;
        last_new_pipeline_time = std::chrono::steady_clock::now();
        SaveDriverCache();

        if (EmulatorSettings.IsShaderCollect()) {
            for (auto stage = 0; stage < MaxShaderStages; ++stage) {
                if (infos[stage]) {
                    auto& m = modules[stage];
                    module_related_pipelines[m].emplace_back(graphics_key);
                }
            }
        }
    }
    last_graphics_key = graphics_key;
    last_graphics = it->second.get();
    return last_graphics;
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    if (!pending_compute.empty()) {
        PublishCompute();
    }
    if (!RefreshComputeKey()) {
        return nullptr;
    }
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (is_new) {
        SotcFrameStats::Scope timing{SotcFrameStats::Kind::Compute};
        const auto pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
        LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);
        const SotcStats::ScopedTimer stall_timer{SotcStats::pipeline_ns};
        SotcStats::pipelines_new.fetch_add(1, std::memory_order_relaxed);

        ComputePipeline::SerializationSupport sdata{};
        const auto create_start = std::chrono::steady_clock::now();
        // sotc: dispatches cannot wait for a worker (their results feed readbacks). A scene
        // change in the attract demo creates ~90 compute pipelines at once (20-190 ms each on
        // NVIDIA, a 5.4 s freeze the first time; the next session preloads them). Tested with an
        // isolated, empty NVIDIA shader cache (__GL_SHADER_DISK_CACHE_PATH), DISABLE_OPTIMIZATION
        // did not shorten these creations, so it is off by default. SOTC_COMPUTE_FAST=1 builds
        // them unoptimized (test); SOTC_COMPUTE_OPTIMIZE=1 then also swaps in optimized copies
        // built on a worker.
        static const bool fast_first = EnvU64("SOTC_COMPUTE_FAST", 0) != 0;
        static const bool optimize_in_session = EnvU64("SOTC_COMPUTE_OPTIMIZE", 0) != 0;
        const bool two_phase = fast_first && async_compiler && optimize_in_session;
        // Only when the store can rebuild them optimized at the next boot.
        const bool fast_build = fast_first && !EmulatorSettings.IsShaderCollect() &&
                                EmulatorSettings.IsPipelineCacheEnabled() &&
                                Storage::DataBase::Instance().IsOpened();
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                       *pipeline_cache, compute_key, *infos[0],
                                                       modules[0], sdata, false, fast_build);
        if (two_phase) {
            auto pending = std::make_unique<PendingCompute>();
            PendingCompute& job = *pending;
            job.key = compute_key;
            job.live_info = infos[0];
            job.module = modules[0];
            job.user_data.assign(infos[0]->user_data.begin(), infos[0]->user_data.end());
            job.info_copy = std::make_unique<Shader::Info>(*infos[0]);
            job.info_copy->user_data = job.user_data;
            deferred_compute.push_back(std::move(pending));
        }
        const auto create_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - create_start)
                                   .count();
        if (create_ms >= 20) {
            std::fprintf(stderr, "SOTC slow compute pipeline %#llx: %lld ms (fast %d, pending %zu, deferred %zu)\n",
                         static_cast<unsigned long long>(pipeline_hash),
                         static_cast<long long>(create_ms), int(fast_build),
                         pending_compute.size(), deferred_compute.size());
        }
        RegisterPipelineData(compute_key, sdata);
        ++num_new_pipelines;
        last_new_pipeline_time = std::chrono::steady_clock::now();
        SaveDriverCache();

        if (EmulatorSettings.IsShaderCollect()) {
            auto& m = modules[0];
            module_related_pipelines[m].emplace_back(compute_key);
        }
    }
    return it->second.get();
}

// sotc: the render target parts of the key, shared with the draw preparation helper
// (vk_draw_prep), which builds them from its own copy of the registers. The first pass runs
// before the shader stages are selected, the second after (it needs the pixel shader's MRT mask).
void PipelineCache::KeyTargetsFirstPass(const AmdGpu::Regs& regs, GraphicsPipelineKey& key) {
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
}

void PipelineCache::KeyTargetsSecondPass(const AmdGpu::Regs& regs,
                                         GraphicsPipelineKey& key) const {
    const bool db_enabled = regs.depth_buffer.DepthValid() || regs.depth_buffer.StencilValid();
    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

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
}

bool PipelineCache::RefreshGraphicsKey() {
    std::memset(&graphics_key, 0, sizeof(GraphicsPipelineKey));
    const auto& regs = liverpool->regs;
    auto& key = graphics_key;
    fast_sel.valid = false;
    fast_build = {};
    fast_build.valid = SelFastMode() != 0;
    const u64 regs_seq = liverpool->sel_regs_seq;

    KeyTargetsFirstPass(regs, key);

    // Compile and bind shader stages
    if (!RefreshGraphicsStages()) {
        return false;
    }

    KeyTargetsSecondPass(regs, key);

    // sotc: only the vertex + pixel configuration with dynamic vertex input (the key then holds
    // no V#-derived format) is reused; registers must not have changed during the selection.
    if (fast_build.valid && regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::Vs &&
        instance.IsVertexInputDynamicState() && liverpool->sel_regs_seq == regs_seq) {
        CommitFastSelection(regs_seq);
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
        if (!regs.stage_enable.IsStageEnabled(stage_in_idx)) {
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
        const auto start = binding;
        std::tie(infos[stage_out_idx], modules[stage_out_idx], key.stage_hashes[stage_out_idx]) =
            GetProgram(stage_in, stage_out, params, binding);
        if (draw_prep && (stage_in == HwStage::Vertex || stage_in == HwStage::Fragment) &&
            got_program->prep_published != got_program->modules.size()) {
            draw_prep->Publish(*got_program);
        }
        if (last_hit.usable) {
            fast_build.stages[stage_out_idx] = {
                .program = last_hit.program,
                .hash = params.hash,
                .memo_idx = last_hit.memo_idx,
                .memo_fill_seq = last_hit.fill_seq,
                .start = start,
                .hw = stage_in,
                .fetch_vs = stage_out == SwStage::Vertex && last_hit.program->info.has_fetch_shader,
            };
            fast_build.stage_mask |= 1u << stage_out_idx;
        } else {
            fast_build.valid = false;
        }
        return true;
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
                                              Shader::Backend::Bindings& binding) {
    SotcFrameStats::Scope timing{SotcFrameStats::Kind::Shader};
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.hw_stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
    DumpShader(code, info.pgm_hash, info.hw_stage, perm_idx, "bin");
    const SotcStats::ScopedTimer stall_timer{SotcStats::shader_ns};
    SotcStats::shaders_compiled.fetch_add(1, std::memory_order_relaxed);

    const auto ir_program = Shader::TranslateProgram(code, pools, info, runtime_info, profile);
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
    DumpShader(spv, info.pgm_hash, info.hw_stage, perm_idx, "spv");

    vk::ShaderModule module;

    auto patch = GetShaderPatch(info.pgm_hash, info.hw_stage, perm_idx, "spv", spv);
    const bool is_patched = patch && EmulatorSettings.IsPatchShaders();
    if (is_patched) {
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.hw_stage, info.pgm_hash);
        module = CompileSPV(*patch, instance.GetDevice());
    } else {
        module = CompileSPV(spv, instance.GetDevice());
    }

    RegisterShaderBinary(std::move(spv), info.pgm_hash, perm_idx);

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
                                                Shader::Backend::Bindings& binding) {
    last_hit = {};
    auto runtime_info = BuildRuntimeInfo(hw_stage, sw_stage);
    auto [it_pgm, new_program] = program_cache.try_emplace(params.hash);
    if (new_program) {
        it_pgm.value() = std::make_unique<Program>(hw_stage, sw_stage, params);
        auto& program = it_pgm.value();
        got_program = program.get();
        auto start = binding;
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
    got_program = program.get();
    auto& info = program->info;
    info.pgm_base = params.Base(); // Needs to be actualized for inline cbuffer address fixup
    info.user_data = params.user_data;
    info.RefreshFlatBuf();

    // sotc: tessellation constants are read from guest memory, so those stages always rebuild
    // the specialization. A vertex shader with a fetch shader also depends on guest memory: the
    // fetch code and the V# of each attribute (read through a user-data pointer, not the
    // flattened buffer). Its memo re-reads exactly those (the same reads the specialization
    // makes) and compares them. SOTC_SPEC_MEMO=0 disables the memo, SOTC_SPEC_MEMO_VS=0 only
    // the vertex-shader part.
    static const bool spec_memo_enabled = [] {
        const char* v = std::getenv("SOTC_SPEC_MEMO");
        return !v || *v != '0';
    }();
    static const bool spec_memo_vs = EnvU64("SOTC_SPEC_MEMO_VS", 1) != 0;
    const bool fetch_vs = sw_stage == SwStage::Vertex && info.has_fetch_shader;
    const bool memo_ok = spec_memo_enabled && sw_stage != SwStage::TessellationControl &&
                         sw_stage != SwStage::TessellationEval && (!fetch_vs || spec_memo_vs);
    if (memo_ok) {
        std::optional<Shader::Gcn::FetchShaderData> fetch_now;
        for (u32 memo_idx = 0; memo_idx < program->spec_memos.size(); ++memo_idx) {
            const auto& m = program->spec_memos[memo_idx];
            if (!m.valid || m.start != binding || m.pgm_base != info.pgm_base ||
                m.flat.size() != info.flattened_ud_buf.size() ||
                !(m.runtime_info == runtime_info) ||
                std::memcmp(m.flat.data(), info.flattened_ud_buf.data(),
                            m.flat.size() * sizeof(u32)) != 0) {
                continue;
            }
            if (fetch_vs) {
                if (!fetch_now) {
                    fetch_now.emplace();
                    Shader::Gcn::ParseFetchShader(info, *fetch_now);
                }
                if (!(*fetch_now == program->modules[m.perm_idx].spec.fetch_shader_data) ||
                    fetch_now->attributes.size() != m.vsharps.size()) {
                    continue;
                }
                bool same = true;
                for (size_t i = 0; i < m.vsharps.size() && same; ++i) {
                    const AmdGpu::Buffer sharp = fetch_now->attributes[i].GetSharp(info);
                    same = std::memcmp(&sharp, &m.vsharps[i], sizeof(sharp)) == 0;
                }
                if (!same) {
                    continue;
                }
            }
            const u32 hit_idx = m.perm_idx;
            last_hit = {program.get(), memo_idx, m.fill_seq, true};
            info.AddBindings(binding);
            if (auto& fetch = program->modules[hit_idx].spec.fetch_shader_data; !fetch.Empty()) {
                fetch_shader = &fetch;
            }
            return std::make_tuple(&program->info, program->modules[hit_idx].module,
                                   HashCombine(params.hash, u64{hit_idx}));
        }
    }
    const auto memo_start = binding;
    auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);

    size_t perm_idx = program->modules.size();
    u64 perm_hash = HashCombine(params.hash, perm_idx);

    vk::ShaderModule module{};

    const auto it = std::ranges::find(program->modules, spec, &Program::Module::spec);
    if (it == program->modules.end()) {
        auto new_info = Shader::Info(hw_stage, sw_stage, params);
        module = CompileModule(new_info, runtime_info, params.code, perm_idx, binding);

        RegisterShaderMeta(info, spec.fetch_shader_data, spec, perm_hash, perm_idx);
        program->AddPermut(module, std::move(spec));
    } else {
        info.AddBindings(binding);
        module = it->module;
        perm_idx = std::distance(program->modules.begin(), it);
        perm_hash = HashCombine(params.hash, perm_idx);
    }
    if (memo_ok) {
        const u32 slot =
            static_cast<u32>(program->next_spec_memo++ % program->spec_memos.size());
        auto& m = program->spec_memos[slot];
        m.flat.assign(info.flattened_ud_buf.begin(), info.flattened_ud_buf.end());
        m.pgm_base = info.pgm_base;
        m.runtime_info = runtime_info;
        m.start = memo_start;
        m.perm_idx = static_cast<u32>(perm_idx);
        m.vsharps.clear();
        if (fetch_vs) {
            for (const auto& attrib : program->modules[perm_idx].spec.fetch_shader_data.attributes) {
                m.vsharps.push_back(attrib.GetSharp(info));
            }
        }
        m.valid = true;
        m.fill_seq = ++memo_fill_counter;
        last_hit = {program.get(), slot, m.fill_seq, true};
    }
    if (auto& fetch = program->modules[perm_idx].spec.fetch_shader_data; !fetch.Empty()) {
        fetch_shader = &fetch;
    }
    return std::make_tuple(&program->info, module, perm_hash);
}

std::optional<vk::ShaderModule> PipelineCache::ReplaceShader(vk::ShaderModule module,
                                                             std::span<const u32> spv_code) {
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
    if (!std::filesystem::exists(patch_dir)) {
        std::filesystem::create_directories(patch_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto filepath = patch_dir / filename;
    if (!std::filesystem::exists(filepath)) {
        return {};
    }
    const u64 generated_hash = XXH3_64bits(generated.data(), generated.size_bytes());
    auto base_path = filepath;
    base_path += ".base";
    if (std::filesystem::exists(base_path)) {
        const auto base_file = IOFile{base_path, FileAccessMode::Read};
        std::string text(base_file.GetSize(), ' ');
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
