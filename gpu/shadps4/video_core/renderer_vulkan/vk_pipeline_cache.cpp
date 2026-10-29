// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <system_error>
#include <thread>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <ranges>
#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <unordered_set>
#include "bbport_toggles.h"
#include "bbport_threads.h"
#include "common/elf_info.h"
#include "common/assert_scope.h"
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

// sotc: the driver's own pipeline cache (VkPipelineCache) used to start empty at every launch, so
// each pipeline the shader cache preloaded was compiled by the driver again behind the loading
// screen and at every first visit. It now lives in <cache dir>/<serial>_driver.bin (cleared with
// the shader cache), is checked against this GPU and driver before it is used, and is written
// when it grew: after the preload, every few minutes and on exit. SOTC_DRIVER_CACHE=0 turns it
// off; SOTC_DRIVER_CACHE_MAX_MB (default 512) is the size above which it is no longer written.
namespace {
std::atomic<PipelineCache*> g_driver_cache_owner{};

bool DriverCacheEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_DRIVER_CACHE");
        return !(v && v[0] == '0');
    }();
    return on;
}

std::filesystem::path DriverCachePath() {
    return Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
           (std::string{Common::ElfInfo::Instance().GameSerial()} + "_driver.bin");
}

// VkPipelineCacheHeaderVersionOne: size, version, vendor, device, UUID.
std::vector<u8> ReadDriverCache(const Instance& instance) {
    std::vector<u8> data;
    std::error_code ec;
    const auto path = DriverCachePath();
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size < 32) {
        return data;
    }
    std::ifstream in(path, std::ios::binary);
    data.resize(static_cast<size_t>(size));
    if (!in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size))) {
        data.clear();
        return data;
    }
    u32 head[4];
    std::memcpy(head, data.data(), sizeof head);
    const auto uuid = instance.GetPipelineCacheUUID();
    if (head[0] < 32 || head[1] != 1 || head[2] != instance.GetVendorID() ||
        head[3] != instance.GetDeviceID() ||
        std::memcmp(data.data() + 16, uuid.data(), VK_UUID_SIZE) != 0) {
        LOG_INFO(Render, "Driver pipeline cache is for another GPU or driver, starting empty");
        data.clear();
    }
    return data;
}
} // namespace

void PipelineCache::SaveDriverCache(bool wait) {
    if (!DriverCacheEnabled() || !pipeline_cache || driver_cache_saving.exchange(true)) {
        return;
    }
    const auto work = [this] {
        auto [result, data] = instance.GetDevice().getPipelineCacheData(*pipeline_cache);
        static const size_t max_bytes = [] {
            const char* v = std::getenv("SOTC_DRIVER_CACHE_MAX_MB");
            return static_cast<size_t>(v ? std::max(16, std::atoi(v)) : 512) << 20;
        }();
        if (result == vk::Result::eSuccess && data.size() != driver_cache_saved_size &&
            data.size() <= max_bytes) {
            const auto path = DriverCachePath();
            auto temp = path;
            temp += ".tmp";
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(data.data()),
                          static_cast<std::streamsize>(data.size()));
                if (!out) {
                    std::filesystem::remove(temp, ec);
                    driver_cache_saving = false;
                    return;
                }
            }
            std::filesystem::rename(temp, path, ec);
            if (!ec) {
                driver_cache_saved_size = data.size();
                LOG_INFO(Render, "Driver pipeline cache saved: {} KiB", data.size() >> 10);
            }
        }
        driver_cache_last_save = std::chrono::steady_clock::now();
        driver_cache_saving = false;
    };
    if (wait) {
        work();
    } else {
        std::thread{work}.detach();
    }
}

void SaveDriverPipelineCache() {
    if (PipelineCache* owner = g_driver_cache_owner.load()) {
        owner->SaveDriverCache(true);
    }
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
      desc_heap{instance, scheduler.GetWorkSemaphore(), DescriptorHeapSizes} {
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
    if (instance.IsAmdWave32Forced()) {
        std::fprintf(stderr, "GPU: AMD wave32 on: shaders lowered for 32 lanes, LDS barriers on "
                             "(SOTC_AMD_WAVE32)\n");
        // GCN: compute pipelines then require 32, below the card's minimum: outside the Vulkan
        // spec (VUID-VkPipelineShaderStageRequiredSubgroupSizeCreateInfo-requiredSubgroupSize-02761).
        if (const u32 req = instance.RequiredSubgroupSize(vk::ShaderStageFlagBits::eCompute);
            req != 0 && req < instance.MinSubgroupSize()) {
            std::fprintf(stderr, "GPU: AMD wave32 requires subgroups of %u, below the card's "
                                 "minimum %u (outside the Vulkan spec; SOTC_AMD_WAVE32=0 runs 64 "
                                 "wide)\n",
                         req, instance.MinSubgroupSize());
        }
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
    };
    // sotc: the cache is read by the Presenter once it can draw (Presenter::Presenter), with
    // the loading screen on meanwhile.

    std::vector<u8> driver_cache;
    if (DriverCacheEnabled()) {
        driver_cache = ReadDriverCache(instance);
        driver_cache_saved_size = driver_cache.size();
    }
    auto [cache_result, cache] = instance.GetDevice().createPipelineCacheUnique(
        {.initialDataSize = driver_cache.size(), .pInitialData = driver_cache.data()});
    ASSERT_MSG(cache_result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
               vk::to_string(cache_result));
    pipeline_cache = std::move(cache);
    g_driver_cache_owner = this;

    // sotc: pipeline workers for SOTC_ASYNC_SHADERS (see AsyncPipelineJob).
    if (AsyncShadersEnabled()) {
        // SOTC_PIPELINE_WORKERS=<n> sets the number of workers. Default: the hardware threads
        // less four, 2 to 12. They are busy only on a first visit, while the game's own threads
        // mostly wait for them (cold caches, attract demo, 16 threads: worst frame 4.8 s with 6
        // workers, 3.4 s with 12; one worker, the old default on 4 threads: 18 s).
        const char* workers = std::getenv("SOTC_PIPELINE_WORKERS");
        const int threads = static_cast<int>(BbThreads::Available()); // sotc: affinity mask
        const unsigned count = static_cast<unsigned>(
            workers ? std::clamp(std::atoi(workers), 1, 16) : std::clamp(threads - 4, 2, 12));
        for (unsigned i = 0; i < count; ++i) {
            async_workers.emplace_back([this](std::stop_token token) { AsyncPipelineWorker(token); });
        }
    }
}

PipelineCache::~PipelineCache() {
    g_driver_cache_owner = nullptr;
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
    // Off the command processor the guest registers belong to later draws: only pipelines whose
    // construction reads nothing from them. Vertex inputs are dynamic state (otherwise they are
    // read from the vertex buffer sharps), and no image needs its sharp for its binding count.
    if (!instance.IsVertexInputDynamicState() || EmulatorSettings.IsShaderCollect()) {
        return false;
    }
    for (const auto* info : infos) {
        if (!info) {
            continue;
        }
        for (const auto& image : info->images) {
            if (image.array_size == 0 &&
                image.mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                return false;
            }
        }
    }
    return true;
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
                                       const GraphicsPipeline::SerializationSupport* preloaded) {
    auto job = std::make_unique<AsyncPipelineJob>();
    job->key = graphics_key;
    job->hash = hash;
    if (preloaded) {
        job->preloaded = true;
        job->sdata = *preloaded; // the cached vertex layout the pipeline is built from
    }
    job->infos = infos;
    job->runtime_infos = runtime_infos;
    if (fetch_shader) {
        job->fetch_shader = *fetch_shader;
    }
    job->modules = modules;
    {
        std::scoped_lock lock{async_mutex};
        async_queue.push_back(std::move(job));
    }
    async_cv.notify_one();
}

void PipelineCache::AsyncPipelineWorker(std::stop_token token) {
    Common::SetCurrentThreadName("shadPS4:PipelineWorker");
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
                return;
            }
            job = std::move(async_queue.front());
            async_queue.pop_front();
            if (job->compute) {
                --async_compute_queued;
            } else if (job->translate) {
                if (job->translate->blocking) {
                    --async_prefetch_queued;
                } else {
                    --async_translate_queued;
                }
            }
        }
        if (job->translate) {
            RunAsyncTranslate(*job->translate);
            std::scoped_lock lock{async_mutex};
            async_done.push_back(std::move(job));
            async_done_count.fetch_add(1, std::memory_order_release);
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
            instance, scheduler, desc_heap, profile, job->key, *pipeline_cache, job->infos,
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
            // in use): the worker's copy is dropped.
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
                                        const Shader::StageSpecialization* spec, size_t perm_idx,
                                        bool blocking) {
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
    t.blocking = blocking;
    if (spec) {
        t.spec = *spec;
    }
    {
        std::scoped_lock lock{async_mutex};
        if (blocking) {
            async_queue.insert(async_queue.begin() + async_prefetch_queued++, std::move(job));
        } else {
            async_queue.insert(async_queue.begin() + async_prefetch_queued + async_compute_queued +
                                   async_translate_queued++,
                               std::move(job));
        }
    }
    async_cv.notify_one();
}

void PipelineCache::QueueComputeBuild(ComputePipeline* pipeline) {
    auto job = std::make_unique<AsyncPipelineJob>();
    job->compute = pipeline;
    {
        std::scoped_lock lock{async_mutex};
        // After the prefetch translations the command processor waits for, after the older
        // compute ones (the recording thread binds them in this order), ahead of the rest.
        async_queue.insert(async_queue.begin() + async_prefetch_queued + async_compute_queued++,
                           std::move(job));
    }
    async_cv.notify_one();
}

// sotc: SOTC_COMPUTE_SPECULATE=0 leaves the compute pipeline of a prefetched program to the first
// dispatch that uses it. Default on: the pipeline (layouts here, the driver compile on a worker)
// is made when the prefetch is installed, so a burst of new computes builds its pipelines in
// parallel while the command processor goes on, and the recording thread rarely waits for one.
// A dispatch whose tables give a different specialization looks up another key (a permutation),
// as before; the speculative pipeline is then simply never bound.
static bool ComputeSpeculateEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_COMPUTE_SPECULATE");
        return !v || v[0] != '0';
    }();
    return enabled;
}

void PipelineCache::SpeculateComputePipeline(Program& program, u64 perm_hash) {
    if (!ComputeSpeculateEnabled() || async_workers.empty() || !AsyncComputeEnabled() ||
        !scheduler.HasRecordThread() || EmulatorSettings.IsShaderCollect()) {
        return;
    }
    // The layout of an image array indexed by its own mip range depends on the sharp the
    // dispatch passes (ImageResource::NumBindings): only the dispatch knows it.
    for (const auto& image : program.info.images) {
        if (image.array_size == 0 &&
            image.mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
            return;
        }
    }
    const ComputePipelineKey key{perm_hash};
    const auto [it, is_new] = compute_pipelines.try_emplace(key);
    if (!is_new) {
        return;
    }
    SotcStats::pipelines_new.fetch_add(1, std::memory_order_relaxed);
    SotcStats::compute_speculated.fetch_add(1, std::memory_order_relaxed);
    ComputePipeline::SerializationSupport sdata{};
    it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                   *pipeline_cache, key, program.info,
                                                   program.modules[0].module, sdata, false, true);
    QueueComputeBuild(it.value().get());
    RegisterPipelineData(key, sdata);
    ++num_new_pipelines;
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
        if (t.blocking) {
            SpeculateComputePipeline(*program, HashCombine(t.hash, 0));
        }
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

const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(const DrawIndirectParams params) {
    draw_indirect_params = params;
    // sotc: every few minutes, write the driver cache if it grew (checked every 4096 draws).
    if ((++driver_cache_tick & 4095u) == 0 &&
        std::chrono::steady_clock::now() - driver_cache_last_save > std::chrono::minutes(3)) {
        SaveDriverCache(false);
    }
    // sotc: finished translations must be installed before the stages are looked up.
    if (async_done_count.load(std::memory_order_acquire) != 0) {
        CollectAsyncPipelines();
    }
    // sotc: full-screen passes (tiny draws, see below) are never skipped, so their shaders are
    // translated here. Without dynamic vertex input the key needs the vertex shader's sharps.
    async_translate_ok = !async_workers.empty() && AsyncTranslateEnabled() &&
                         liverpool->regs.num_indices > 6 && instance.IsVertexInputDynamicState() &&
                         !EmulatorSettings.IsShaderCollect();
    translation_pending = false;
    // sotc: PipelineKeyMemo (BbToggle::PipelineKeyMemo disables it, SOTC_PIPE_KEY_MEMO=0 too):
    // RefreshGraphicsKey only reads the registers and the draw's indirect parameters, so a draw
    // that wrote no register chunk and passes the same parameters would rebuild exactly the key
    // of the previous draw and then hit the pipe memo below anyway. Return its pipeline without
    // walking the shader stages (GetProgram, specializations, flat buffers: ~15% of the command
    // processor at the Shrine). Without the draw thread MarkRegs does not track chunks.
    static const bool key_memo_env = [] {
        const char* v = std::getenv("SOTC_PIPE_KEY_MEMO");
        return !v || v[0] != '0';
    }();
    // sotc: BbToggle::CpFastPaths. Clear: "no register the pipeline key reads was written since the
    // key was last built" (Liverpool::KeyDirty: the draw parameters a draw packet writes do not
    // count, and a dispatch between two draws does not hide a write). Set: no register chunk at all
    // was written since the last captured operation (the old condition).
    static const bool cp_verify = std::getenv("SOTC_CP_VERIFY") != nullptr;
    if (key_memo_env && !BbToggle::Disabled(BbToggle::PipelineKeyMemo) &&
        last_graphics_pipeline && liverpool->IsBackThreaded() &&
        params.vertex_sgpr_offset == last_graphics_params.vertex_sgpr_offset &&
        params.instance_sgpr_offset == last_graphics_params.instance_sgpr_offset) {
        const bool fine = !BbToggle::Disabled(BbToggle::CpFastPaths);
        const bool chunks_dirty = liverpool->HasDirtyRegChunks();
        if (fine ? !liverpool->KeyDirty() : !chunks_dirty) {
            if (SotcStats::kFpsLog) {
                SotcStats::pipe_key_memo_hits.fetch_add(1, std::memory_order_relaxed);
                if (chunks_dirty) {
                    SotcStats::pipe_key_memo_fine_hits.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (cp_verify) [[unlikely]] {
                VerifyKeyMemo(fine, chunks_dirty);
            }
            return last_graphics_pipeline;
        }
    }
    if (SotcStats::kFpsLog) {
        SotcStats::pipe_key_memo_misses.fetch_add(1, std::memory_order_relaxed);
    }
    const u64 key_start = SotcStats::kFpsLog ? SotcStats::NowNs() : 0;
    if (!RefreshGraphicsKey()) {
        if (translation_pending) {
            SotcStats::async_translate_skips.fetch_add(1, std::memory_order_relaxed);
        }
        last_graphics_pipeline = nullptr; // sotc: the key is not usable for the next draw
        return nullptr;
    }
    liverpool->ClearKeyDirty(); // sotc: the key now reflects every register written so far
    if (key_start) {
        SotcStats::pipe_key_ns.fetch_add(SotcStats::NowNs() - key_start,
                                         std::memory_order_relaxed);
        SotcStats::pipe_key_draws.fetch_add(1, std::memory_order_relaxed);
    }
    // sotc: consecutive draws mostly use the same pipeline: when the freshly built key equals the
    // previous draw's byte for byte, skip the map's hash and probe. The stage state (infos,
    // modules, fetch shader) was refreshed by RefreshGraphicsKey either way. SOTC_PIPE_MEMO=0.
    static const bool pipe_memo = [] {
        const char* v = std::getenv("SOTC_PIPE_MEMO");
        return !v || v[0] != '0';
    }();
    if (pipe_memo && last_graphics_pipeline &&
        std::memcmp(&graphics_key, &last_graphics_key, sizeof(graphics_key)) == 0) {
        if (SotcStats::kFpsLog) {
            SotcStats::pipe_same_key_hits.fetch_add(1, std::memory_order_relaxed);
        }
        last_graphics_params = params;
        return last_graphics_pipeline;
    }
    if (SotcStats::kFpsLog) {
        SotcStats::pipe_same_key_misses.fetch_add(1, std::memory_order_relaxed);
    }
    const auto [it, is_new] = graphics_pipelines.try_emplace(graphics_key);
    // sotc: still being built by a pipeline worker (SOTC_ASYNC_SHADERS, SOTC_ASYNC_PRELOAD). A
    // full-screen pass is not skipped: it builds the pipeline here, and the worker's copy is
    // dropped when collected.
    const bool queued = !is_new && !it->second;
    if (queued && liverpool->regs.num_indices > 6) {
        last_graphics_pipeline = nullptr; // sotc: key memo off until one is installed
        return nullptr;
    }
    if (is_new || queued) {
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(graphics_key);
        LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);
        // Full-screen passes (post-processing, composition) are tiny draws: skipping one leaves
        // a black or stale frame, so they still build here. Geometry draws are skipped while
        // their pipeline is built (a brief pop-in instead of a freeze on first sight).
        const bool tiny_draw = liverpool->regs.num_indices <= 6;
        const bool eligible = AsyncPipelineEligible();
        if (!async_workers.empty() && !tiny_draw && eligible && !queued) {
            QueueAsyncPipeline(pipeline_hash);
            last_graphics_pipeline = nullptr; // sotc: key memo off until it is installed
            return nullptr; // the draws that need it are skipped until it is ready
        }
        const SotcStats::ScopedTimer stall_timer{SotcStats::pipeline_ns};
        SotcStats::pipelines_new.fetch_add(1, std::memory_order_relaxed);
        const auto sync_start = std::chrono::steady_clock::now();

        const SotcStats::FrameTimer frame_timer{SotcStats::FramePipeline};
        SotcStats::frame_compiles.fetch_add(1, std::memory_order_relaxed);
        GraphicsPipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, graphics_key, *pipeline_cache, infos,
            runtime_infos, fetch_shader, modules, sdata, false);
        LogSyncPipeline("graphics",
                        async_workers.empty() ? "async off"
                        : tiny_draw           ? "tiny draw"
                                              : "not eligible",
                        sync_start, infos[3] ? infos[3]->pgm_hash : 0, // vertex
                        infos[0] ? infos[0]->pgm_hash : 0);              // fragment

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
    last_graphics_params = params;
    return it->second.get();
}

// sotc: SOTC_CP_VERIFY=1. A pipeline key memo hit is about to return last_graphics_pipeline: build the
// key from the registers as they are now, compare it with the key of that pipeline, and put the
// state the build touches back (the key, the stage tables and the command processor's per-draw
// shader data), so a verified run behaves as an unverified one. fine: which rule hit.
void PipelineCache::VerifyKeyMemo(bool fine, bool chunks_dirty) {
    static std::atomic<u64> checked{0}, bad{0}, bad_fine_only{0};
    const GraphicsPipelineKey saved_key = graphics_key;
    const auto saved_infos = infos;
    const auto saved_modules = modules;
    const auto saved_runtime_infos = runtime_infos;
    auto* const saved_fetch = fetch_shader;
    const bool saved_pending = translation_pending;
    std::array<Shader::InfoRuntime, MaxShaderStages> saved_runtime{};
    for (u32 i = 0; i < MaxShaderStages; ++i) {
        if (infos[i]) {
            saved_runtime[i] = const_cast<Shader::Info*>(infos[i])->runtime[0];
        }
    }
    const bool ok = RefreshGraphicsKey();
    const bool same = ok && std::memcmp(&graphics_key, &last_graphics_key, sizeof(graphics_key)) == 0;
    const u64 n = checked.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!same) {
        const u64 b = bad.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fine && chunks_dirty) {
            bad_fine_only.fetch_add(1, std::memory_order_relaxed);
        }
        if (b <= 20) {
            std::fprintf(stderr,
                         "SOTC CP VERIFY: pipeline key memo hit (%s rule, chunks dirty %d) but the "
                         "key differs (build %s)\n",
                         fine ? "fine" : "old", chunks_dirty ? 1 : 0, ok ? "ok" : "failed");
        }
    }
    if ((n & 0xFFFFF) == 0) {
        std::fprintf(stderr, "SOTC CP VERIFY: key memo hits checked %llu, bad %llu (fine rule only %llu)\n",
                     static_cast<unsigned long long>(n),
                     static_cast<unsigned long long>(bad.load()),
                     static_cast<unsigned long long>(bad_fine_only.load()));
    }
    graphics_key = saved_key;
    infos = saved_infos;
    modules = saved_modules;
    runtime_infos = saved_runtime_infos;
    fetch_shader = saved_fetch;
    translation_pending = saved_pending;
    for (u32 i = 0; i < MaxShaderStages; ++i) {
        if (saved_infos[i]) {
            const_cast<Shader::Info*>(saved_infos[i])->runtime[0] = std::move(saved_runtime[i]);
        }
    }
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    if (!RefreshComputeKey()) {
        return nullptr;
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
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                       *pipeline_cache, compute_key, *infos[0],
                                                       modules[0], sdata, false, deferred);
        if (deferred) {
            QueueComputeBuild(it.value().get());
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
    return it->second.get();
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

void PipelineCache::PrefetchComputeProgram() {
    if (async_workers.empty() || EmulatorSettings.IsShaderCollect() ||
        pending_programs.size() >= 512) {
        return;
    }
    const auto params = AmdGpu::GetParams(liverpool->GetCsRegs());
    if (program_cache.contains(params.hash) || in_place_programs.contains(params.hash) ||
        !pending_programs.insert(params.hash).second) {
        return;
    }
    Shader::Backend::Bindings binding{};
    const auto& rt = BuildRuntimeInfo(HwStage::Compute, SwStage::Compute);
    // sotc: ahead of every other job (SOTC_PREFETCH_PRIORITY=0 queues it like the others): the
    // command processor waits for it in RefreshComputeKey.
    static const bool priority = [] {
        const char* v = std::getenv("SOTC_PREFETCH_PRIORITY");
        return !v || v[0] != '0';
    }();
    QueueAsyncTranslate(HwStage::Compute, SwStage::Compute, params, rt, binding, nullptr, nullptr,
                        0, priority);
}

bool PipelineCache::RefreshComputeKey() {
    Shader::Backend::Bindings binding{};
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto cs_params = AmdGpu::GetParams(cs_pgm);
    // sotc: SOTC_ASYNC_COMPUTE_TRANSLATE=1 translates a new compute shader on a worker; the
    // dispatch is dropped until it is ready (a cold shader cache made ~900 ms of stalls per route).
    static const bool async_cs = [] {
        const char* v = std::getenv("SOTC_ASYNC_COMPUTE_TRANSLATE");
        return v && v[0] == '1';
    }();
    const bool async_ok = async_cs && !async_workers.empty() && !EmulatorSettings.IsShaderCollect();
    if (async_ok) {
        if (async_done_count.load(std::memory_order_acquire) != 0) {
            CollectAsyncPipelines();
        }
        translation_pending = false;
    }
    if (!async_ok && pending_programs.contains(cs_params.hash)) {
        // sotc: queued by PrefetchComputeProgram: wait for the worker instead of translating again.
        const auto wait_start = std::chrono::steady_clock::now();
        const auto give_up = wait_start + std::chrono::seconds(2);
        while (pending_programs.contains(cs_params.hash) &&
               std::chrono::steady_clock::now() < give_up) {
            if (async_done_count.load(std::memory_order_acquire) != 0) {
                CollectAsyncPipelines();
            } else {
                std::this_thread::yield();
            }
        }
        const auto waited = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - wait_start).count();
        if (waited > 3.0) {
            LOG_INFO(Render_Vulkan, "SOTC PREFETCH waited {:.1f} ms for compute {:#x}", waited,
                     cs_params.hash);
        }
    }
    std::tie(infos[0], modules[0], compute_key.value) =
        GetProgram(HwStage::Compute, SwStage::Compute, cs_params, binding, async_ok);
    return !translation_pending || !async_ok;
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
    const auto sync_shader_start = phase_start;
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
    // sotc: a shader translated on the command processor (not on a worker) is a stall; log slow ones.
    if (!worker_pools) {
        LogSyncPipeline("shader", info.hw_stage == HwStage::Compute ? "compute" : "graphics stage",
                        sync_shader_start, info.pgm_hash, perm_idx);
    }
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
        return {nullptr, vk::ShaderModule{}, 0};
    }
    if (it == program->modules.end()) {
        auto new_info = Shader::Info(hw_stage, sw_stage, params);
        auto runtime_info = rt;
        module = CompileModule(new_info, runtime_info, params.code, perm_idx, binding);

        RegisterShaderMeta(info, spec.fetch_shader_data, spec, perm_hash, perm_idx);
        program->AddPermut(module, std::move(spec));
    } else {
        info.AddBindings(binding);
        module = it->module;
        perm_idx = std::distance(program->modules.begin(), it);
        perm_hash = HashCombine(params.hash, perm_idx);
    }
    if (auto& fetch = program->modules[perm_idx].spec.fetch_shader_data; !fetch.Empty()) {
        fetch_shader = &fetch;
    }
    return std::make_tuple(&program->info, module, perm_hash);
}

std::optional<vk::ShaderModule> PipelineCache::ReplaceShader(vk::ShaderModule module,
                                                             std::span<const u32> spv_code) {
    last_graphics_pipeline = nullptr; // sotc: pipelines may be erased below
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
