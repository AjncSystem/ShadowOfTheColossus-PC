// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <unordered_map>
#include <cstdlib>
#include <mutex>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>
#include <magic_enum/magic_enum.hpp>

#include "common/assert.h"
#include "common/elf_info.h"
#include "common/func_traits.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {
namespace {

static constexpr spv::ExecutionMode GetInputPrimitiveType(AmdGpu::PrimitiveType type) {
    switch (type) {
    case AmdGpu::PrimitiveType::PointList:
        return spv::ExecutionMode::InputPoints;
    case AmdGpu::PrimitiveType::LineList:
    case AmdGpu::PrimitiveType::LineStrip:
        return spv::ExecutionMode::InputLines;
    case AmdGpu::PrimitiveType::TriangleList:
    case AmdGpu::PrimitiveType::TriangleStrip:
    case AmdGpu::PrimitiveType::RectList:
        return spv::ExecutionMode::Triangles;
    case AmdGpu::PrimitiveType::AdjTriangleList:
        return spv::ExecutionMode::InputTrianglesAdjacency;
    case AmdGpu::PrimitiveType::AdjLineList:
        return spv::ExecutionMode::InputLinesAdjacency;
    default:
        UNREACHABLE_MSG("Unknown input primitive type {}", u32(type));
    }
}

static constexpr spv::ExecutionMode GetOutputPrimitiveType(AmdGpu::GsOutputPrimitiveType type) {
    switch (type) {
    case AmdGpu::GsOutputPrimitiveType::PointList:
        return spv::ExecutionMode::OutputVertices;
    case AmdGpu::GsOutputPrimitiveType::LineStrip:
        return spv::ExecutionMode::OutputLineStrip;
    case AmdGpu::GsOutputPrimitiveType::TriangleStrip:
        return spv::ExecutionMode::OutputTriangleStrip;
    default:
        UNREACHABLE_MSG("Unknown output primitive type {}", u32(type));
    }
}

template <auto func, typename... Args>
void SetDefinition(EmitContext& ctx, IR::Inst* inst, Args... args) {
    inst->SetDefinition<Id>(func(ctx, std::forward<Args>(args)...));
}

template <typename ArgType>
ArgType Arg(EmitContext& ctx, const IR::Value& arg) {
    if constexpr (std::is_same_v<ArgType, Id>) {
        return ctx.Def(arg);
    } else if constexpr (std::is_same_v<ArgType, const IR::Value&>) {
        return arg;
    } else if constexpr (std::is_same_v<ArgType, u32>) {
        if (const IR::Inst* handle = arg.TryInst();
            handle && handle->GetOpcode() == IR::Opcode::ImageArrayHandle) {
            // Descriptor array image: the instruction indexes the array itself (LoadImage)
            return handle->Arg(0).U32();
        }
        return arg.U32();
    } else if constexpr (std::is_same_v<ArgType, u64>) {
        return arg.U64();
    } else if constexpr (std::is_same_v<ArgType, bool>) {
        return arg.U1();
    } else if constexpr (std::is_same_v<ArgType, IR::Attribute>) {
        return arg.Attribute();
    } else if constexpr (std::is_same_v<ArgType, IR::ScalarReg>) {
        return arg.ScalarReg();
    } else if constexpr (std::is_same_v<ArgType, IR::VectorReg>) {
        return arg.VectorReg();
    } else if constexpr (std::is_same_v<ArgType, const char*>) {
        return arg.StringLiteral();
    } else if constexpr (std::is_same_v<ArgType, IR::Patch>) {
        return arg.Patch();
    }
    UNREACHABLE();
}

template <auto func, bool is_first_arg_inst, size_t... I>
void Invoke(EmitContext& ctx, IR::Inst* inst, std::index_sequence<I...>) {
    using Traits = Common::FuncTraits<decltype(func)>;
    if constexpr (std::is_same_v<typename Traits::ReturnType, Id>) {
        if constexpr (is_first_arg_inst) {
            SetDefinition<func>(
                ctx, inst, inst,
                Arg<typename Traits::template ArgType<I + 2>>(ctx, inst->Arg(I))...);
        } else {
            SetDefinition<func>(
                ctx, inst, Arg<typename Traits::template ArgType<I + 1>>(ctx, inst->Arg(I))...);
        }
    } else {
        if constexpr (is_first_arg_inst) {
            func(ctx, inst, Arg<typename Traits::template ArgType<I + 2>>(ctx, inst->Arg(I))...);
        } else {
            func(ctx, Arg<typename Traits::template ArgType<I + 1>>(ctx, inst->Arg(I))...);
        }
    }
}

template <auto func>
void Invoke(EmitContext& ctx, IR::Inst* inst) {
    using Traits = Common::FuncTraits<decltype(func)>;
    static_assert(Traits::NUM_ARGS >= 1, "Insufficient arguments");
    if constexpr (Traits::NUM_ARGS == 1) {
        Invoke<func, false>(ctx, inst, std::make_index_sequence<0>{});
    } else {
        using FirstArgType = typename Traits::template ArgType<1>;
        static constexpr bool is_first_arg_inst = std::is_same_v<FirstArgType, IR::Inst*>;
        using Indices = std::make_index_sequence<Traits::NUM_ARGS - (is_first_arg_inst ? 2 : 1)>;
        Invoke<func, is_first_arg_inst>(ctx, inst, Indices{});
    }
}

void EmitInst(EmitContext& ctx, IR::Inst* inst) {
    switch (inst->GetOpcode()) {
#define OPCODE(name, result_type, ...)                                                             \
    case IR::Opcode::name:                                                                         \
        return Invoke<&Emit##name>(ctx, inst);
#include "shader_recompiler/ir/opcodes.inc"
#undef OPCODE
    }
    UNREACHABLE_MSG("Invalid opcode {}", inst->GetOpcode());
}

Id TypeId(const EmitContext& ctx, IR::Type type) {
    switch (type) {
    case IR::Type::U1:
        return ctx.U1[1];
    case IR::Type::U32:
        return ctx.U32[1];
    case IR::Type::U32x2:
        return ctx.U32[2];
    case IR::Type::U32x3:
        return ctx.U32[3];
    case IR::Type::U32x4:
        return ctx.U32[4];
    case IR::Type::F32:
        return ctx.F32[1];
    case IR::Type::U64:
        return ctx.U64;
    default:
        UNREACHABLE_MSG("Phi node type {}", type);
    }
}

// Upper bound on the total loop iterations one shader invocation may run, across all of its
// (possibly nested) loops. Guest loops are frequently bounded by values read from memory; when
// emulation feeds a shader stale or garbage data such a loop can spin for billions of iterations,
// which ends in a GPU timeout and a lost device. Real content stays far below this budget, so
// cutting the loops short only degrades the affected frame instead of ending the session.
// SHADPS4_LOOP_LIMIT overrides it (0 disables the guard).
static u32 LoopIterationBudget() {
    static const u32 budget = [] {
        if (const char* value = std::getenv("SHADPS4_LOOP_LIMIT")) {
            return static_cast<u32>(std::strtoul(value, nullptr, 0));
        }
        // Shadow of the Colossus used 1 << 13 here, which cut its screen-space reflection march
        // short (it walks the 64 lanes of a wave one after another, each marching its ray): water
        // reflected only near the horizon and stayed dark and flat closer to the camera. Its
        // runaway loops after a save loads (stale data) are now stopped by the per-loop budget
        // below; with only 1 << 20 here they stalled the GPU for minutes.
        return 1U << 18;
    }();
    return budget;
}

// sotc: each loop also has its own budget, reset when the loop exits: a loop that runs away
// stops after this many iterations, while a shader that runs many short loops one after another
// (SotC's reflection march over the 64 lanes of a wave) only meets the invocation budget above.
// SHADPS4_LOOP_LIMIT_PER_LOOP overrides it (0 disables the per-loop budget).
static u32 PerLoopIterationBudget() {
    static const u32 budget = [] {
        if (const char* value = std::getenv("SHADPS4_LOOP_LIMIT_PER_LOOP")) {
            return static_cast<u32>(std::strtoul(value, nullptr, 0));
        }
        return Common::ElfInfo::Instance().GameSerial() == "CUSA08809" ? (1U << 13) : 0U;
    }();
    return budget;
}

void Traverse(EmitContext& ctx, const IR::Program& program) {
    const u32 loop_budget = LoopIterationBudget();
    const u32 per_loop_budget = loop_budget != 0 ? PerLoopIterationBudget() : 0;
    // Per-loop counters, found by the loop's header and merge blocks.
    std::unordered_map<const IR::Block*, Id> header_counters;
    std::unordered_map<const IR::Block*, Id> merge_counters;
    const auto new_loop_counter = [&] {
        const Id ptr_type{ctx.TypePointer(spv::StorageClass::Private, ctx.U32[1])};
        const Id counter{
            ctx.AddGlobalVariable(ptr_type, spv::StorageClass::Private, ctx.u32_zero_value)};
        ctx.interfaces.push_back(counter);
        return counter;
    };
    Id loop_counter{};
    const auto get_loop_counter = [&] {
        if (!Sirit::ValidId(loop_counter)) {
            const Id ptr_type{ctx.TypePointer(spv::StorageClass::Private, ctx.U32[1])};
            loop_counter = ctx.AddGlobalVariable(ptr_type, spv::StorageClass::Private,
                                                 ctx.u32_zero_value);
            ctx.interfaces.push_back(loop_counter);
        }
        return loop_counter;
    };

    IR::Block* current_block{};
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        switch (node.type) {
        case IR::AbstractSyntaxNode::Type::Block: {
            const Id label{node.data.block->Definition<Id>()};
            if (current_block) {
                ctx.OpBranch(label);
            }
            current_block = node.data.block;
            ctx.AddLabel(label);
            // The loop exited: the next time it starts, its budget starts over (after the
            // block's OpPhi instructions, which must come first).
            const auto reset_it = merge_counters.find(node.data.block);
            bool reset_pending = reset_it != merge_counters.end();
            for (IR::Inst& inst : node.data.block->Instructions()) {
                if (reset_pending && inst.GetOpcode() != IR::Opcode::Phi) {
                    ctx.OpStore(reset_it->second, ctx.u32_zero_value);
                    reset_pending = false;
                }
                EmitInst(ctx, &inst);
            }
            if (reset_pending) {
                ctx.OpStore(reset_it->second, ctx.u32_zero_value);
            }
            ctx.first_to_last_label_map[label.value] = ctx.last_label;
            break;
        }
        case IR::AbstractSyntaxNode::Type::If: {
            const Id if_label{node.data.if_node.body->Definition<Id>()};
            const Id endif_label{node.data.if_node.merge->Definition<Id>()};
            ctx.OpSelectionMerge(endif_label, spv::SelectionControlMask::MaskNone);
            ctx.OpBranchConditional(ctx.Def(node.data.if_node.cond), if_label, endif_label);
            break;
        }
        case IR::AbstractSyntaxNode::Type::Loop: {
            const Id body_label{node.data.loop.body->Definition<Id>()};
            const Id continue_label{node.data.loop.continue_block->Definition<Id>()};
            const Id endloop_label{node.data.loop.merge->Definition<Id>()};

            if (loop_budget != 0) {
                // The header runs once per iteration: charge it to the invocation budget.
                const Id counter{get_loop_counter()};
                const Id count{ctx.OpLoad(ctx.U32[1], counter)};
                ctx.OpStore(counter, ctx.OpIAdd(ctx.U32[1], count, ctx.ConstU32(1U)));
                if (per_loop_budget != 0 && current_block) {
                    const Id own{new_loop_counter()};
                    header_counters[current_block] = own;
                    merge_counters[node.data.loop.merge] = own;
                    const Id own_count{ctx.OpLoad(ctx.U32[1], own)};
                    ctx.OpStore(own, ctx.OpIAdd(ctx.U32[1], own_count, ctx.ConstU32(1U)));
                }
            }
            ctx.OpLoopMerge(endloop_label, continue_label, spv::LoopControlMask::MaskNone);
            ctx.OpBranch(body_label);
            break;
        }
        case IR::AbstractSyntaxNode::Type::Break: {
            const Id break_label{node.data.break_node.merge->Definition<Id>()};
            const Id skip_label{node.data.break_node.skip->Definition<Id>()};
            ctx.OpBranchConditional(ctx.Def(node.data.break_node.cond), break_label, skip_label);
            break;
        }
        case IR::AbstractSyntaxNode::Type::EndIf:
            if (current_block) {
                ctx.OpBranch(node.data.end_if.merge->Definition<Id>());
            }
            break;
        case IR::AbstractSyntaxNode::Type::Repeat: {
            Id cond{ctx.Def(node.data.repeat.cond)};
            const Id loop_header_label{node.data.repeat.loop_header->Definition<Id>()};
            const Id merge_label{node.data.repeat.merge->Definition<Id>()};
            if (loop_budget != 0) {
                const Id count{ctx.OpLoad(ctx.U32[1], get_loop_counter())};
                const Id within_budget{
                    ctx.OpULessThan(ctx.U1[1], count, ctx.ConstU32(loop_budget))};
                cond = ctx.OpLogicalAnd(ctx.U1[1], cond, within_budget);
                if (const auto it = header_counters.find(node.data.repeat.loop_header);
                    it != header_counters.end()) {
                    const Id own_count{ctx.OpLoad(ctx.U32[1], it->second)};
                    const Id within_own{
                        ctx.OpULessThan(ctx.U1[1], own_count, ctx.ConstU32(per_loop_budget))};
                    cond = ctx.OpLogicalAnd(ctx.U1[1], cond, within_own);
                }
            }
            ctx.OpBranchConditional(cond, loop_header_label, merge_label);
            break;
        }
        case IR::AbstractSyntaxNode::Type::Return:
            ctx.OpReturn();
            break;
        case IR::AbstractSyntaxNode::Type::Unreachable:
            ctx.OpUnreachable();
            break;
        }
        if (node.type != IR::AbstractSyntaxNode::Type::Block) {
            current_block = nullptr;
        }
    }
}

Id DefineMain(EmitContext& ctx, const IR::Program& program) {
    const Id void_function{ctx.TypeFunction(ctx.void_id)};
    const Id main{ctx.OpFunction(ctx.void_id, spv::FunctionControlMask::MaskNone, void_function)};
    for (IR::Block* const block : program.blocks) {
        block->SetDefinition(ctx.OpLabel());
    }
    Traverse(ctx, program);
    ctx.OpFunctionEnd();
    return main;
}

spv::ExecutionMode ExecutionMode(AmdGpu::TessellationType primitive) {
    switch (primitive) {
    case AmdGpu::TessellationType::Isoline:
        return spv::ExecutionMode::Isolines;
    case AmdGpu::TessellationType::Triangle:
        return spv::ExecutionMode::Triangles;
    case AmdGpu::TessellationType::Quad:
        return spv::ExecutionMode::Quads;
    }
    UNREACHABLE_MSG("Tessellation primitive {}", magic_enum::enum_name(primitive));
}

spv::ExecutionMode ExecutionMode(AmdGpu::TessellationPartitioning spacing) {
    switch (spacing) {
    case AmdGpu::TessellationPartitioning::Integer:
        return spv::ExecutionMode::SpacingEqual;
    case AmdGpu::TessellationPartitioning::FracOdd:
        return spv::ExecutionMode::SpacingFractionalOdd;
    case AmdGpu::TessellationPartitioning::FracEven:
        return spv::ExecutionMode::SpacingFractionalEven;
    case AmdGpu::TessellationPartitioning::Pow2:
        // Pow2 rounds tessellation factors to the nearest power of 2, which has no
        // direct Vulkan equivalent. SpacingEqual (integer) is the closest match.
        LOG_WARNING(Render_Vulkan, "Tessellation partitioning Pow2 has no Vulkan equivalent, "
                                   "falling back to SpacingEqual");
        return spv::ExecutionMode::SpacingEqual;
    default:
        break;
    }
    UNREACHABLE_MSG("Tessellation spacing {}", magic_enum::enum_name(spacing));
}

void SetupCapabilities(const Info& info, const Profile& profile, const RuntimeInfo& runtime_info,
                       EmitContext& ctx) {
    ctx.AddCapability(spv::Capability::Image1D);
    ctx.AddCapability(spv::Capability::Sampled1D);
    ctx.AddCapability(spv::Capability::ImageQuery);
    ctx.AddCapability(spv::Capability::Int8);
    ctx.AddCapability(spv::Capability::Int16);
    if (profile.support_int64 || info.uses_int64) { // sotc: Info::uses_int64
        ctx.AddCapability(spv::Capability::Int64);
    }
    ctx.AddCapability(spv::Capability::StorageBuffer8BitAccess);
    ctx.AddCapability(spv::Capability::StorageBuffer16BitAccess);
    if (info.uses_fp16) {
        ctx.AddCapability(spv::Capability::Float16);
    }
    if (info.uses_fp64) {
        ctx.AddCapability(spv::Capability::Float64);
    }
    if (info.has_storage_images) {
        ctx.AddCapability(spv::Capability::StorageImageExtendedFormats);
        ctx.AddCapability(spv::Capability::StorageImageReadWithoutFormat);
        ctx.AddCapability(spv::Capability::StorageImageWriteWithoutFormat);
        if (profile.supports_image_load_store_lod) {
            ctx.AddExtension("SPV_AMD_shader_image_load_store_lod");
            ctx.AddCapability(spv::Capability::ImageReadWriteLodAMD);
        }
    }
    if (info.has_image_gather) {
        ctx.AddCapability(spv::Capability::ImageGatherExtended);
    }
    if (info.has_image_query) {
        ctx.AddCapability(spv::Capability::ImageQuery);
    }
    if ((info.uses_image_atomic_float_min_max && profile.supports_image_fp32_atomic_min_max) ||
        (info.uses_buffer_atomic_float_min_max && profile.supports_buffer_fp32_atomic_min_max)) {
        ctx.AddExtension("SPV_EXT_shader_atomic_float_min_max");
        ctx.AddCapability(spv::Capability::AtomicFloat32MinMaxEXT);
    }
    if (info.uses_lane_id) {
        ctx.AddCapability(spv::Capability::GroupNonUniform);
    }
    if (info.uses_group_quad) {
        ctx.AddCapability(spv::Capability::GroupNonUniformQuad);
    }
    if (info.uses_group_shuffle) {
        ctx.AddCapability(spv::Capability::GroupNonUniformShuffle);
    }
    if (info.uses_group_ballot || info.loads.Get(IR::Attribute::SubgroupLtMask)) {
        ctx.AddCapability(spv::Capability::GroupNonUniformBallot);
    }
    if (info.uses_shader_clock) {
        if (ctx.profile.supports_shader_subgroup_clock) {
            ctx.AddExtension("SPV_KHR_shader_clock");
            ctx.AddCapability(spv::Capability::ShaderClockKHR);
        } else {
            LOG_WARNING(Render_Recompiler,
                        "Shader requires support for ShaderClockKHR capability"
                        " that your Vulkan instance does not advertise. Results may vary");
        }
    }
    const auto stage = info.sw_stage;
    if (stage == SwStage::Vertex) {
        ctx.AddExtension("SPV_KHR_shader_draw_parameters");
        ctx.AddCapability(spv::Capability::DrawParameters);
    }
    if (stage == SwStage::Geometry) {
        ctx.AddCapability(spv::Capability::Geometry);
    }
    if (info.hw_stage == HwStage::Fragment) {
        if (profile.supports_amd_shader_explicit_vertex_parameter) {
            ctx.AddExtension("SPV_AMD_shader_explicit_vertex_parameter");
        } else if (profile.supports_fragment_shader_barycentric) {
            if (!profile.barycentric_via_gs) { // sotc: plain inputs from the geometry shader
                ctx.AddExtension("SPV_KHR_fragment_shader_barycentric");
                ctx.AddCapability(spv::Capability::FragmentBarycentricKHR);
            }
            ctx.AddCapability(spv::Capability::InterpolationFunction);
        }
        if (info.loads.Get(IR::Attribute::SampleIndex) ||
            runtime_info.hw.fs.addr_flags.linear_sample_ena ||
            runtime_info.hw.fs.addr_flags.persp_sample_ena ||
            (!profile.supports_amd_shader_explicit_vertex_parameter &&
             profile.supports_fragment_shader_barycentric &&
             info.loads.Get(IR::Attribute::BaryCoordSmoothSample))) {
            ctx.AddCapability(spv::Capability::SampleRateShading);
        }
        if (info.loads.GetAny(IR::Attribute::RenderTargetIndex)) {
            ctx.AddCapability(spv::Capability::Geometry);
        }
        if (info.stores.Get(IR::Attribute::StencilRef) && profile.supports_shader_stencil_export) {
            ctx.AddExtension("SPV_EXT_shader_stencil_export");
            ctx.AddCapability(spv::Capability::StencilExportEXT);
        }
    }
    if (stage == SwStage::TessellationControl || stage == SwStage::TessellationEval) {
        ctx.AddCapability(spv::Capability::Tessellation);
    }
    if (stage == SwStage::Vertex || stage == SwStage::TessellationControl ||
        stage == SwStage::TessellationEval) {
        if (info.stores.GetAny(IR::Attribute::RenderTargetIndex)) {
            ctx.AddCapability(spv::Capability::ShaderLayer);
        }
        if (info.stores.GetAny(IR::Attribute::ViewportIndex)) {
            ctx.AddCapability(spv::Capability::ShaderViewportIndex);
        }
    } else if (stage == SwStage::Geometry && info.stores.GetAny(IR::Attribute::ViewportIndex)) {
        ctx.AddCapability(spv::Capability::MultiViewport);
    }
    if (info.uses_dma) {
        ctx.AddCapability(spv::Capability::PhysicalStorageBufferAddresses);
        ctx.AddExtension("SPV_KHR_physical_storage_buffer");
    }
    const auto shared_type_count = std::popcount(static_cast<u32>(info.shared_types));
    if (shared_type_count > 1 && profile.supports_workgroup_explicit_memory_layout) {
        ctx.AddExtension("SPV_KHR_workgroup_memory_explicit_layout");
        ctx.AddCapability(spv::Capability::WorkgroupMemoryExplicitLayoutKHR);
        ctx.AddCapability(spv::Capability::WorkgroupMemoryExplicitLayout16BitAccessKHR);
    }
    if (info.uses_buffer_int64_atomics || info.uses_shared_int64_atomics) {
        if (info.uses_buffer_int64_atomics) {
            ASSERT_MSG(ctx.profile.supports_buffer_int64_atomics,
                       "Shader requires support for atomic Int64 buffer operations that your "
                       "Vulkan instance does not advertise");
        }
        if (info.uses_shared_int64_atomics) {
            ASSERT_MSG(ctx.profile.supports_shared_int64_atomics,
                       "Shader requires support for atomic Int64 shared memory operations that "
                       "your Vulkan instance does not advertise");
        }
        ctx.AddCapability(spv::Capability::Int64Atomics);
    }
}

void DefineEntryPoint(const Info& info, EmitContext& ctx, Id main) {
    const std::span interfaces(ctx.interfaces.data(), ctx.interfaces.size());
    spv::ExecutionModel execution_model{};
    switch (info.sw_stage) {
    case SwStage::Compute: {
        const std::array<u32, 3> workgroup_size{ctx.runtime_info.hw.cs.workgroup_size};
        execution_model = spv::ExecutionModel::GLCompute;
        ctx.AddExecutionMode(main, spv::ExecutionMode::LocalSize, workgroup_size[0],
                             workgroup_size[1], workgroup_size[2]);
        break;
    }
    case SwStage::Vertex:
        execution_model = spv::ExecutionModel::Vertex;
        break;
    case SwStage::TessellationControl:
        execution_model = spv::ExecutionModel::TessellationControl;
        ctx.AddCapability(spv::Capability::Tessellation);
        ctx.AddExecutionMode(main, spv::ExecutionMode::OutputVertices,
                             ctx.runtime_info.sw.tcs.NumOutputControlPoints());
        break;
    case SwStage::TessellationEval: {
        execution_model = spv::ExecutionModel::TessellationEvaluation;
        ctx.AddExecutionMode(main, ExecutionMode(ctx.runtime_info.sw.tes.tess_type));
        ctx.AddExecutionMode(main, ExecutionMode(ctx.runtime_info.sw.tes.tess_partitioning));
        ctx.AddExecutionMode(main, ctx.runtime_info.sw.tes.tess_topology ==
                                           AmdGpu::TessellationTopology::TriangleCcw
                                       ? spv::ExecutionMode::VertexOrderCcw
                                       : spv::ExecutionMode::VertexOrderCw);
        break;
    }
    case SwStage::Fragment:
        execution_model = spv::ExecutionModel::Fragment;
        if (ctx.profile.lower_left_origin_mode) {
            ctx.AddExecutionMode(main, spv::ExecutionMode::OriginLowerLeft);
        } else {
            ctx.AddExecutionMode(main, spv::ExecutionMode::OriginUpperLeft);
        }
        if (info.has_discard) {
            ctx.AddCapability(spv::Capability::DemoteToHelperInvocation);
        }
        if (info.stores.GetAny(IR::Attribute::Depth)) {
            ctx.AddExecutionMode(main, spv::ExecutionMode::DepthReplacing);
        }
        // Only for shaders whose side effects depend on it: storage writes, and nothing early
        // tests would change (discard, or a depth, stencil ref or sample mask export).
        if (ctx.runtime_info.hw.fs.early_fragment_tests && info.has_storage_images &&
            !info.has_discard && !info.stores.GetAny(IR::Attribute::Depth) &&
            !info.stores.Get(IR::Attribute::StencilRef) &&
            !info.stores.Get(IR::Attribute::SampleMask)) {
            ctx.AddExecutionMode(main, spv::ExecutionMode::EarlyFragmentTests);
            LOG_INFO(Render_Recompiler, "fs {:#x}: early fragment tests", info.pgm_hash);
        }
        break;
    case SwStage::Geometry:
        execution_model = spv::ExecutionModel::Geometry;
        ctx.AddExecutionMode(main, GetInputPrimitiveType(ctx.runtime_info.hw.gs.in_primitive));
        ctx.AddExecutionMode(main, GetOutputPrimitiveType(ctx.runtime_info.hw.gs.out_primitive[0]));
        ctx.AddExecutionMode(main, spv::ExecutionMode::OutputVertices,
                             ctx.runtime_info.hw.gs.output_vertices);
        ctx.AddExecutionMode(main, spv::ExecutionMode::Invocations,
                             ctx.runtime_info.hw.gs.num_invocations);
        break;
    default:
        UNREACHABLE_MSG("Stage {}", u32(info.hw_stage));
    }
    ctx.AddEntryPoint(execution_model, main, "main", interfaces);
}

void SetupDenormFlushMode(EmitContext& ctx, const Profile& profile, const RuntimeInfo& runtime_info,
                          Id main_func) {
    const auto fp32_denorm_mode = runtime_info.props.fp_denorm_mode32;
    if (fp32_denorm_mode == AmdGpu::FpDenormMode::InOutFlush) {
        if (profile.support_fp32_denorm_flush) {
            ctx.AddCapability(spv::Capability::DenormFlushToZero);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::DenormFlushToZero, 32U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float32 denorm flushing is not supported by the GPU");
            });
        }
    } else {
        LOG_WARNING(Render_Vulkan, "Unknown FP denorm mode {}", u32(fp32_denorm_mode));
    }

    if (!ctx.info.uses_fp16 && !ctx.info.uses_fp64) {
        return;
    }

    const auto fp16_64_denorm_mode = runtime_info.props.fp_denorm_mode16_64;
    const auto same_denorm_mode = fp16_64_denorm_mode == fp32_denorm_mode;
    if (!same_denorm_mode && !profile.supports_denorm_behavior_independence) {
        static std::once_flag logged;
        std::call_once(logged, [] {
            LOG_WARNING(Render_Vulkan, "Float32 and Float16/64 denorm modes are different but the "
                                       "GPU does not support that");
        });
        return;
    }

    if (fp16_64_denorm_mode == AmdGpu::FpDenormMode::InOutFlush) {
        if (profile.support_fp16_denorm_flush) {
            ctx.AddCapability(spv::Capability::DenormFlushToZero);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::DenormFlushToZero, 16U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float16 denorm flushing is not supported by the GPU");
            });
        }
        if (profile.support_fp64_denorm_flush) {
            ctx.AddCapability(spv::Capability::DenormFlushToZero);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::DenormFlushToZero, 64U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float64 denorm flushing is not supported by the GPU");
            });
        }
    } else if (fp16_64_denorm_mode == AmdGpu::FpDenormMode::InOutAllow) {
        if (profile.support_fp16_denorm_preserve) {
            ctx.AddCapability(spv::Capability::DenormPreserve);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::DenormPreserve, 16U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float16 denorm preserving is not supported by the GPU");
            });
        }
        if (profile.support_fp64_denorm_preserve) {
            ctx.AddCapability(spv::Capability::DenormPreserve);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::DenormPreserve, 64U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float64 denorm preserving is not supported by the GPU");
            });
        }
    } else {
        LOG_WARNING(Render_Vulkan, "Unknown Float16/64 denorm mode {}", u32(fp16_64_denorm_mode));
    }
}

void SetupRoundingMode(EmitContext& ctx, const Profile& profile, const RuntimeInfo& runtime_info,
                       Id main_func) {
    const auto fp_round_mode = runtime_info.props.fp_round_mode32;
    if (fp_round_mode == AmdGpu::FpRoundMode::ToZero) {
        if (profile.support_fp32_round_to_zero) {
            ctx.AddCapability(spv::Capability::RoundingModeRTZ);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::RoundingModeRTZ, 32U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float32 rounding to zero is not supported by the GPU");
            });
        }
    } else if (fp_round_mode != AmdGpu::FpRoundMode::NearestEven) {
        LOG_WARNING(Render_Vulkan, "Unimplemented FP rounding mode {}",
                    magic_enum::enum_name(fp_round_mode));
    }

    if (ctx.info.uses_fp16 || ctx.info.uses_fp64) {
        const auto fp16_64_round_mode = runtime_info.props.fp_round_mode16_64;
        const auto same_round_mode = fp16_64_round_mode == fp_round_mode;
        if (!same_round_mode && !profile.supports_rounding_mode_independence) {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float32 and Float16/64 rounding modes are different "
                                           "but the GPU does not support that");
            });
            return;
        }

        if (fp16_64_round_mode == AmdGpu::FpRoundMode::ToZero) {
            if (ctx.info.uses_fp16) {
                if (profile.support_fp16_round_to_zero) {
                    ctx.AddCapability(spv::Capability::RoundingModeRTZ);
                    ctx.AddExecutionMode(main_func, spv::ExecutionMode::RoundingModeRTZ, 16U);
                } else {
                    static std::once_flag logged;
                    std::call_once(logged, [] {
                        LOG_WARNING(Render_Vulkan,
                                    "Float16 rounding to zero is not supported by the GPU");
                    });
                }
            }
            if (ctx.info.uses_fp64) {
                if (profile.support_fp64_round_to_zero) {
                    ctx.AddCapability(spv::Capability::RoundingModeRTZ);
                    ctx.AddExecutionMode(main_func, spv::ExecutionMode::RoundingModeRTZ, 64U);
                } else {
                    static std::once_flag logged;
                    std::call_once(logged, [] {
                        LOG_WARNING(Render_Vulkan,
                                    "Float64 rounding to zero is not supported by the GPU");
                    });
                }
            }
        } else if (fp16_64_round_mode != AmdGpu::FpRoundMode::NearestEven) {
            LOG_WARNING(Render_Vulkan, "Unknown Float16/64 rounding mode {}",
                        u32(fp16_64_round_mode));
        }
    }
}

void SetupInfNanPreserveMode(EmitContext& ctx, const Profile& profile,
                             const RuntimeInfo& runtime_info, Id main_func) {
    if (profile.support_fp16_signed_zero_inf_nan_preserve ||
        profile.support_fp32_signed_zero_inf_nan_preserve ||
        profile.support_fp64_signed_zero_inf_nan_preserve) {
        ctx.AddCapability(spv::Capability::SignedZeroInfNanPreserve);
    }

    if (profile.support_fp32_signed_zero_inf_nan_preserve) {
        ctx.AddExecutionMode(main_func, spv::ExecutionMode::SignedZeroInfNanPreserve, 32U);
    } else {
        static std::once_flag logged;
        std::call_once(logged, [] {
            LOG_WARNING(Render_Vulkan,
                        "Float32 signed zero/inf/nan preserve mode is not supported by the GPU");
        });
    }
    if (ctx.info.uses_fp16) {
        if (profile.support_fp16_signed_zero_inf_nan_preserve) {
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::SignedZeroInfNanPreserve, 16U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(
                    Render_Vulkan,
                    "Float16 signed zero/inf/nan preserve mode is not supported by the GPU");
            });
        }
    }
    if (ctx.info.uses_fp64) {
        if (profile.support_fp64_signed_zero_inf_nan_preserve) {
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::SignedZeroInfNanPreserve, 64U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(
                    Render_Vulkan,
                    "Float64 signed zero/inf/nan preserve mode is not supported by the GPU");
            });
        }
    }
}

void SetupFloatMode(EmitContext& ctx, const Profile& profile, const RuntimeInfo& runtime_info,
                    Id main_func) {
    ctx.AddExtension("SPV_KHR_float_controls");
    SetupDenormFlushMode(ctx, profile, runtime_info, main_func);
    SetupRoundingMode(ctx, profile, runtime_info, main_func);
    SetupInfNanPreserveMode(ctx, profile, runtime_info, main_func);
}

void PatchPhiNodes(const IR::Program& program, EmitContext& ctx) {
    auto inst{program.blocks.front()->begin()};
    size_t block_index{0};
    ctx.PatchDeferredPhi([&](u32 phi_arg, Id first_parent) {
        if (phi_arg == 0) {
            ++inst;
            while (inst == program.blocks[block_index]->end() ||
                   inst->GetOpcode() != IR::Opcode::Phi) {
                ++block_index;
                inst = program.blocks[block_index]->begin();
            }
        }
        ASSERT(inst != program.blocks[block_index]->end());
        const Id arg = ctx.Def(inst->Arg(phi_arg));
        const Id parent = ctx.first_to_last_label_map[first_parent.value];
        return std::make_pair(arg, parent);
    });
}
} // Anonymous namespace

std::vector<u32> EmitSPIRV(const Profile& profile, const RuntimeInfo& runtime_info,
                           const IR::Program& program, Bindings& binding) {
    EmitContext ctx{profile, runtime_info, program.info, binding};
    const Id main{DefineMain(ctx, program)};
    DefineEntryPoint(program.info, ctx, main);
    SetupCapabilities(program.info, profile, runtime_info, ctx);
    SetupFloatMode(ctx, profile, runtime_info, main);
    PatchPhiNodes(program, ctx);
    return ctx.Assemble();
}

Id EmitPhi(EmitContext& ctx, IR::Inst* inst) {
    const size_t num_args{inst->NumArgs()};
    boost::container::small_vector<Id, 32> blocks;
    blocks.reserve(num_args);
    for (size_t index = 0; index < num_args; ++index) {
        blocks.push_back(inst->PhiBlock(index)->Definition<Id>());
    }
    // The type of a phi instruction is stored in its flags
    const Id result_type{TypeId(ctx, inst->Flags<IR::Type>())};
    return ctx.DeferredOpPhi(result_type, std::span(blocks.data(), blocks.size()));
}

void EmitVoid(EmitContext&) {}

Id EmitConditionRef(EmitContext& ctx, const IR::Value& value) {
    const Id id{ctx.Def(value)};
    ASSERT_MSG(Sirit::ValidId(id), "Forward identity declaration");
    return id;
}

Id EmitGetPcLo(EmitContext& ctx, const IR::Value& value) {
    const Id id{ctx.Def(value)};
    ASSERT_MSG(Sirit::ValidId(id), "Forward identity declaration");
    return id;
}

void EmitGetScc(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetExec(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetVcc(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetVccLo(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetVccHi(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetM0(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetScc(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetExec(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetVcc(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetVccLo(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetVccHi(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetM0(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

} // namespace Shader::Backend::SPIRV
