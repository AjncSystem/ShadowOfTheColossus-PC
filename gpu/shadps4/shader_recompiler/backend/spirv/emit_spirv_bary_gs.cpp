// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <sirit/sirit.h>
#include "shader_recompiler/backend/spirv/emit_spirv_bary_gs.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {

using Sirit::Id;

constexpr u32 SPIRV_VERSION_1_5 = 0x00010500;

BaryEmulationLayout ComputeBaryEmulationLayout(const Info& fs_info,
                                               const HwFragmentRuntimeInfo& fs,
                                               const Profile& profile) {
    BaryEmulationLayout layout{};
    const u32 clip_offset = fs.clip_distance_emulation ? 1U : 0U;
    layout.num_inputs = fs.num_inputs - clip_offset;
    bool any_per_vertex = false;
    for (u32 i = 0; i < layout.num_inputs; i++) {
        const auto& input = fs.inputs[i];
        layout.location[i] = input.param_index + clip_offset;
        layout.per_vertex[i] =
            !input.IsDefault() && fs_info.fs_interpolation[i].primary == Qualifier::PerVertex;
        any_per_vertex |= layout.per_vertex[i];
    }
    if (!profile.emulate_fragment_barycentrics) {
        return layout;
    }
    const auto& loads = fs_info.loads;
    const bool smooth = loads.GetAny(IR::Attribute::BaryCoordSmooth) ||
                        loads.GetAny(IR::Attribute::BaryCoordSmoothCentroid) ||
                        loads.GetAny(IR::Attribute::BaryCoordSmoothSample) ||
                        loads.GetAny(IR::Attribute::BaryCoordPullModel);
    const bool nopersp = loads.GetAny(IR::Attribute::BaryCoordNoPersp) ||
                         loads.GetAny(IR::Attribute::BaryCoordNoPerspSample);
    if (!any_per_vertex && !smooth && !nopersp) {
        return layout;
    }

    // The geometry shader writes every fragment input, so they are packed from the start (after
    // the emulated clip distances, which stay at location 0).
    std::array<s32, 32> location = layout.location;
    u32 next = clip_offset;
    for (u32 i = 0; i < layout.num_inputs; i++) {
        if (fs.inputs[i].IsDefault()) {
            continue;
        }
        location[i] = static_cast<s32>(next);
        next += layout.per_vertex[i] ? 3U : 1U;
    }
    const s32 bary_smooth = smooth ? static_cast<s32>(next++) : BaryEmulationLayout::Unused;
    const s32 bary_nopersp = nopersp ? static_cast<s32>(next++) : BaryEmulationLayout::Unused;
    if (next > profile.max_fragment_input_locations) {
        return layout;
    }
    layout.active = true;
    layout.location = location;
    layout.bary_smooth = bary_smooth;
    layout.bary_nopersp = bary_nopersp;
    layout.num_locations = next;
    return layout;
}

namespace {

struct BaryGsEmitter : public Sirit::Module {
    BaryGsEmitter() : Sirit::Module{SPIRV_VERSION_1_5} {}

    void Emit(const BaryEmulationLayout& layout, const HwFragmentRuntimeInfo& fs,
              const BaryGsParams& params) {
        AddCapability(spv::Capability::Shader);
        AddCapability(spv::Capability::Geometry);
        if (params.num_clip_distances > 0) {
            AddCapability(spv::Capability::ClipDistance);
        }
        if (params.num_cull_distances > 0) {
            AddCapability(spv::Capability::CullDistance);
        }

        const Id void_id{TypeVoid()};
        float_id = TypeFloat(32);
        uint_id = TypeUInt(32U);
        int_id = TypeInt(32U, true);
        const Id vec3_id{TypeVector(float_id, 3)};
        vec4_id = TypeVector(float_id, 4);
        const Id float_zero{Constant(float_id, 0.0f)};
        const Id float_one{Constant(float_id, 1.0f)};

        const u32 num_vertices = params.input == BaryGsInput::Points  ? 1U
                                 : params.input == BaryGsInput::Lines ? 2U
                                                                      : 3U;

        // gl_PerVertex with the built-ins the previous stage writes.
        std::vector<Id> members{vec4_id};
        u32 clip_member{};
        u32 cull_member{};
        if (params.num_clip_distances > 0) {
            clip_member = static_cast<u32>(members.size());
            members.push_back(TypeArray(float_id, Uint(params.num_clip_distances)));
        }
        if (params.num_cull_distances > 0) {
            cull_member = static_cast<u32>(members.size());
            members.push_back(TypeArray(float_id, Uint(params.num_cull_distances)));
        }
        const Id per_vertex_type{TypeStruct(std::span<const Id>{members})};
        Decorate(per_vertex_type, spv::Decoration::Block);
        MemberDecorate(per_vertex_type, 0U, spv::Decoration::BuiltIn,
                       static_cast<u32>(spv::BuiltIn::Position));
        if (params.num_clip_distances > 0) {
            MemberDecorate(per_vertex_type, clip_member, spv::Decoration::BuiltIn,
                           static_cast<u32>(spv::BuiltIn::ClipDistance));
        }
        if (params.num_cull_distances > 0) {
            MemberDecorate(per_vertex_type, cull_member, spv::Decoration::BuiltIn,
                           static_cast<u32>(spv::BuiltIn::CullDistance));
        }
        const Id gl_in{AddInput(TypeArray(per_vertex_type, Uint(num_vertices)))};
        const Id gl_out{AddOutput(per_vertex_type)};

        const Id in_array{TypeArray(vec4_id, Uint(num_vertices))};
        Id in_cldist{};
        Id out_cldist{};
        if (params.clip_distance_attr) {
            in_cldist = AddInput(in_array);
            Decorate(in_cldist, spv::Decoration::Location, 0U);
            out_cldist = AddOutput(vec4_id);
            Decorate(out_cldist, spv::Decoration::Location, 0U);
        }

        std::array<Id, 32> inputs{};
        std::array<Id, 32> outputs{};
        for (u32 i = 0; i < layout.num_inputs; i++) {
            const auto& input = fs.inputs[i];
            if (input.IsDefault()) {
                continue;
            }
            inputs[i] = AddInput(in_array);
            Decorate(inputs[i], spv::Decoration::Location,
                     input.param_index + params.input_location_offset);
            if (layout.per_vertex[i]) {
                outputs[i] = AddOutput(TypeArray(vec4_id, Uint(3U)));
                Decorate(outputs[i], spv::Decoration::Flat);
            } else {
                outputs[i] = AddOutput(vec4_id);
            }
            Decorate(outputs[i], spv::Decoration::Location, static_cast<u32>(layout.location[i]));
        }

        Id bary_smooth{};
        Id bary_nopersp{};
        if (layout.bary_smooth != BaryEmulationLayout::Unused) {
            bary_smooth = AddOutput(vec3_id);
            Decorate(bary_smooth, spv::Decoration::Location,
                     static_cast<u32>(layout.bary_smooth));
        }
        if (layout.bary_nopersp != BaryEmulationLayout::Unused) {
            bary_nopersp = AddOutput(vec3_id);
            Decorate(bary_nopersp, spv::Decoration::Location,
                     static_cast<u32>(layout.bary_nopersp));
            Decorate(bary_nopersp, spv::Decoration::NoPerspective);
        }

        const Id main{OpFunction(void_id, spv::FunctionControlMask::MaskNone, TypeFunction(void_id))};
        switch (params.input) {
        case BaryGsInput::Points:
            AddExecutionMode(main, spv::ExecutionMode::InputPoints);
            AddExecutionMode(main, spv::ExecutionMode::OutputPoints);
            break;
        case BaryGsInput::Lines:
            AddExecutionMode(main, spv::ExecutionMode::InputLines);
            AddExecutionMode(main, spv::ExecutionMode::OutputLineStrip);
            break;
        case BaryGsInput::Triangles:
            AddExecutionMode(main, spv::ExecutionMode::Triangles);
            AddExecutionMode(main, spv::ExecutionMode::OutputTriangleStrip);
            break;
        }
        AddExecutionMode(main, spv::ExecutionMode::OutputVertices, num_vertices);
        AddExecutionMode(main, spv::ExecutionMode::Invocations, 1U);
        AddEntryPoint(spv::ExecutionModel::Geometry, main, "main", interfaces);
        AddLabel(OpLabel());

        const Id input_vec4{TypePointer(spv::StorageClass::Input, vec4_id)};
        const Id output_vec4{TypePointer(spv::StorageClass::Output, vec4_id)};
        const Id input_float{TypePointer(spv::StorageClass::Input, float_id)};
        const Id output_float{TypePointer(spv::StorageClass::Output, float_id)};

        // Every emitted vertex carries all the primitive's vertex values. Points and lines repeat
        // their last vertex; its barycentric weight stays 0.
        std::array<std::array<Id, 3>, 32> vertex_values{};
        for (u32 i = 0; i < layout.num_inputs; i++) {
            if (!layout.per_vertex[i] || !Sirit::ValidId(inputs[i])) {
                continue;
            }
            for (u32 k = 0; k < 3; k++) {
                const s32 src = static_cast<s32>(std::min(k, num_vertices - 1));
                vertex_values[i][k] =
                    OpLoad(vec4_id, OpAccessChain(input_vec4, inputs[i], Int(src)));
            }
        }

        // Emitting the vertices in input order keeps the provoking vertex for flat inputs.
        for (u32 v = 0; v < num_vertices; v++) {
            const Id vi{Int(static_cast<s32>(v))};
            const Id position{OpLoad(vec4_id, OpAccessChain(input_vec4, gl_in, vi, Int(0)))};
            OpStore(OpAccessChain(output_vec4, gl_out, Int(0)), position);
            for (u32 c = 0; c < params.num_clip_distances; c++) {
                const Id dist{OpLoad(float_id, OpAccessChain(input_float, gl_in, vi,
                                                             Int(clip_member), Int(c)))};
                OpStore(OpAccessChain(output_float, gl_out, Int(clip_member), Int(c)), dist);
            }
            for (u32 c = 0; c < params.num_cull_distances; c++) {
                const Id dist{OpLoad(float_id, OpAccessChain(input_float, gl_in, vi,
                                                             Int(cull_member), Int(c)))};
                OpStore(OpAccessChain(output_float, gl_out, Int(cull_member), Int(c)), dist);
            }
            if (params.clip_distance_attr) {
                OpStore(out_cldist, OpLoad(vec4_id, OpAccessChain(input_vec4, in_cldist, vi)));
            }
            for (u32 i = 0; i < layout.num_inputs; i++) {
                if (!Sirit::ValidId(inputs[i])) {
                    continue;
                }
                if (layout.per_vertex[i]) {
                    for (u32 k = 0; k < 3; k++) {
                        OpStore(OpAccessChain(output_vec4, outputs[i], Int(static_cast<s32>(k))),
                                vertex_values[i][k]);
                    }
                } else {
                    OpStore(outputs[i],
                            OpLoad(vec4_id, OpAccessChain(input_vec4, inputs[i], vi)));
                }
            }
            const Id bary{ConstantComposite(vec3_id, v == 0 ? float_one : float_zero,
                                            v == 1 ? float_one : float_zero,
                                            v == 2 ? float_one : float_zero)};
            if (Sirit::ValidId(bary_smooth)) {
                OpStore(bary_smooth, bary);
            }
            if (Sirit::ValidId(bary_nopersp)) {
                OpStore(bary_nopersp, bary);
            }
            OpEmitVertex();
        }
        OpEndPrimitive();

        OpReturn();
        OpFunctionEnd();
    }

private:
    Id Int(s32 value) {
        return Constant(int_id, value);
    }

    Id Uint(u32 value) {
        return Constant(uint_id, value);
    }

    Id AddInput(Id type) {
        const Id input{AddGlobalVariable(TypePointer(spv::StorageClass::Input, type),
                                         spv::StorageClass::Input)};
        interfaces.push_back(input);
        return input;
    }

    Id AddOutput(Id type) {
        const Id output{AddGlobalVariable(TypePointer(spv::StorageClass::Output, type),
                                          spv::StorageClass::Output)};
        interfaces.push_back(output);
        return output;
    }

    Id float_id;
    Id uint_id;
    Id int_id;
    Id vec4_id;
    std::vector<Id> interfaces;
};

} // Anonymous namespace

std::vector<u32> EmitBaryEmulationGeometryShader(const BaryEmulationLayout& layout,
                                                 const HwFragmentRuntimeInfo& fs,
                                                 const BaryGsParams& params) {
    BaryGsEmitter ctx;
    ctx.Emit(layout, fs, params);
    return ctx.Assemble();
}

} // namespace Shader::Backend::SPIRV
