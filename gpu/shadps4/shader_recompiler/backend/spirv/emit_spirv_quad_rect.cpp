// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <optional>
#include <sirit/sirit.h>
#include "shader_recompiler/backend/spirv/emit_spirv_quad_rect.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {

using Sirit::Id;

constexpr u32 SPIRV_VERSION_1_5 = 0x00010500;

struct QuadRectListEmitter : public Sirit::Module {
    explicit QuadRectListEmitter(const HwFragmentRuntimeInfo& fs_info_)
        : Sirit::Module{SPIRV_VERSION_1_5}, fs_info{fs_info_} {
        void_id = TypeVoid();
        bool_id = TypeBool();
        float_id = TypeFloat(32);
        uint_id = TypeUInt(32U);
        int_id = TypeInt(32U, true);
        bvec2_id = TypeVector(bool_id, 2);
        vec2_id = TypeVector(float_id, 2);
        vec3_id = TypeVector(float_id, 3);
        vec4_id = TypeVector(float_id, 4);

        float_one = Constant(float_id, 1.0f);
        float_min_one = Constant(float_id, -1.0f);
        int_zero = Constant(int_id, 0);

        const Id float_arr{TypeArray(float_id, Constant(uint_id, 1U))};
        gl_per_vertex_type = TypeStruct(vec4_id, float_id, float_arr, float_arr);
        Decorate(gl_per_vertex_type, spv::Decoration::Block);
        MemberDecorate(gl_per_vertex_type, 0U, spv::Decoration::BuiltIn,
                       static_cast<u32>(spv::BuiltIn::Position));
        MemberDecorate(gl_per_vertex_type, 1U, spv::Decoration::BuiltIn,
                       static_cast<u32>(spv::BuiltIn::PointSize));
        MemberDecorate(gl_per_vertex_type, 2U, spv::Decoration::BuiltIn,
                       static_cast<u32>(spv::BuiltIn::ClipDistance));
        MemberDecorate(gl_per_vertex_type, 3U, spv::Decoration::BuiltIn,
                       static_cast<u32>(spv::BuiltIn::CullDistance));
    }

    /// Emits tessellation control shader for interpolating the 4th vertex of rectange primitive
    void EmitRectListTCS() {
        DefineEntry(spv::ExecutionModel::TessellationControl);

        // Set passthrough tessellation factors
        const Id output_float_id{TypePointer(spv::StorageClass::Output, float_id)};
        for (int i = 0; i < 4; i++) {
            const Id ptr{OpAccessChain(output_float_id, gl_tess_level_outer, Int(i))};
            OpStore(ptr, float_one);
        }
        for (int i = 0; i < 2; i++) {
            const Id ptr{OpAccessChain(output_float_id, gl_tess_level_inner, Int(i))};
            OpStore(ptr, float_one);
        }

        const Id input_vec4{TypePointer(spv::StorageClass::Input, vec4_id)};
        const Id output_vec4{TypePointer(spv::StorageClass::Output, vec4_id)};

        // Emit interpolation block of the 4th vertex in rect.
        // Load positions
        std::array<Id, 3> pos;
        for (int i = 0; i < 3; i++) {
            pos[i] = OpLoad(vec4_id, OpAccessChain(input_vec4, gl_in, Int(i), int_zero));
        }

        std::array<Id, 3> point_coord_equal;
        for (int i = 0; i < 3; i++) {
            // point_coord_equal[i] = equal(gl_in[i].gl_Position.xy, gl_in[(i + 1) %
            // 3].gl_Position.xy);
            const Id pos_l_xy{OpVectorShuffle(vec2_id, pos[i], pos[i], 0, 1)};
            const Id pos_r_xy{OpVectorShuffle(vec2_id, pos[(i + 1) % 3], pos[(i + 1) % 3], 0, 1)};
            point_coord_equal[i] = OpFOrdEqual(bvec2_id, pos_l_xy, pos_r_xy);
        }

        std::array<Id, 3> bary_coord;
        std::array<Id, 3> is_edge_vertex;
        for (int i = 0; i < 3; i++) {
            // bool xy_equal = point_coord_equal[i].x && point_coord_equal[(i + 2) % 3].y;
            const Id xy_equal{
                OpLogicalAnd(bool_id, OpCompositeExtract(bool_id, point_coord_equal[i], 0),
                             OpCompositeExtract(bool_id, point_coord_equal[(i + 2) % 3], 1))};
            // bool yx_equal = point_coord_equal[i].y && point_coord_equal[(i + 2) % 3].x;
            const Id yx_equal{
                OpLogicalAnd(bool_id, OpCompositeExtract(bool_id, point_coord_equal[i], 1),
                             OpCompositeExtract(bool_id, point_coord_equal[(i + 2) % 3], 0))};
            // bary_coord[i] = (xy_equal || yx_equal) ? -1.f : 1.f;
            is_edge_vertex[i] = OpLogicalOr(bool_id, xy_equal, yx_equal);
            bary_coord[i] = OpSelect(float_id, is_edge_vertex[i], float_min_one, float_one);
        }

        const auto interpolate = [&](Id v0, Id v1, Id v2) {
            // return v0 * bary_coord.x + v1 * bary_coord.y + v2 * bary_coord.z;
            const Id p0{OpVectorTimesScalar(vec4_id, v0, bary_coord[0])};
            const Id p1{OpVectorTimesScalar(vec4_id, v1, bary_coord[1])};
            const Id p2{OpVectorTimesScalar(vec4_id, v2, bary_coord[2])};
            return OpFAdd(vec4_id, p0, OpFAdd(vec4_id, p1, p2));
        };

        // int vertex_index_id = is_edge_vertex[1] ? 1 : (is_edge_vertex[2] ? 2 : 0);
        Id vertex_index{OpSelect(int_id, is_edge_vertex[2], Int(2), Int(0))};
        vertex_index = OpSelect(int_id, is_edge_vertex[1], Int(1), vertex_index);

        // int index = (vertex_index_id + gl_InvocationID) % 3;
        const Id invocation_id{OpLoad(int_id, gl_invocation_id)};
        const Id invocation_3{OpIEqual(bool_id, invocation_id, Int(3))};
        const Id index{OpSMod(int_id, OpIAdd(int_id, vertex_index, invocation_id), Int(3))};

        // gl_out[gl_InvocationID].gl_Position = gl_InvocationID == 3 ? pos3 :
        // gl_in[index].gl_Position;
        const Id pos3{interpolate(pos[0], pos[1], pos[2])};
        const Id in_ptr{OpAccessChain(input_vec4, gl_in, index, Int(0))};
        const Id position{OpSelect(vec4_id, invocation_3, pos3, OpLoad(vec4_id, in_ptr))};
        OpStore(OpAccessChain(output_vec4, gl_out, invocation_id, Int(0)), position);

        // Set attributes
        for (int i = 0; i < inputs.size(); i++) {
            // vec4 in_paramN3 = interpolate(bary_coord, in_paramN[0], in_paramN[1], in_paramN[2]);
            const Id v0{OpLoad(vec4_id, OpAccessChain(input_vec4, inputs[i], Int(0)))};
            const Id v1{OpLoad(vec4_id, OpAccessChain(input_vec4, inputs[i], Int(1)))};
            const Id v2{OpLoad(vec4_id, OpAccessChain(input_vec4, inputs[i], Int(2)))};
            const Id in_param3{interpolate(v0, v1, v2)};
            // out_paramN[gl_InvocationID] = gl_InvocationID == 3 ? in_paramN3 : in_paramN[index];
            const Id in_param{OpLoad(vec4_id, OpAccessChain(input_vec4, inputs[i], index))};
            const Id out_param{OpSelect(vec4_id, invocation_3, in_param3, in_param)};
            OpStore(OpAccessChain(output_vec4, outputs[i], invocation_id), out_param);
        }

        OpReturn();
        OpFunctionEnd();
    }

    /// Emits a passthrough quad tessellation control shader that outputs 4 control points.
    void EmitQuadListTCS() {
        DefineEntry(spv::ExecutionModel::TessellationControl);
        const Id array_type{TypeArray(int_id, Int(4))};
        const Id values{ConstantComposite(array_type, Int(1), Int(2), Int(0), Int(3))};
        const Id indices{AddLocalVariable(TypePointer(spv::StorageClass::Function, array_type),
                                          spv::StorageClass::Function, values)};

        // Set passthrough tessellation factors
        const Id output_float{TypePointer(spv::StorageClass::Output, float_id)};
        for (int i = 0; i < 4; i++) {
            const Id ptr{OpAccessChain(output_float, gl_tess_level_outer, Int(i))};
            OpStore(ptr, float_one);
        }
        for (int i = 0; i < 2; i++) {
            const Id ptr{OpAccessChain(output_float, gl_tess_level_inner, Int(i))};
            OpStore(ptr, float_one);
        }

        const Id input_vec4{TypePointer(spv::StorageClass::Input, vec4_id)};
        const Id output_vec4{TypePointer(spv::StorageClass::Output, vec4_id)};
        const Id func_int{TypePointer(spv::StorageClass::Function, int_id)};
        const Id invocation_id{OpLoad(int_id, gl_invocation_id)};
        const Id index{OpLoad(int_id, OpAccessChain(func_int, indices, invocation_id))};

        // gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
        const Id in_position{OpLoad(vec4_id, OpAccessChain(input_vec4, gl_in, index, Int(0)))};
        OpStore(OpAccessChain(output_vec4, gl_out, invocation_id, Int(0)), in_position);

        for (int i = 0; i < inputs.size(); i++) {
            // out_paramN[gl_InvocationID] = in_paramN[gl_InvocationID];
            const Id in_param{OpLoad(vec4_id, OpAccessChain(input_vec4, inputs[i], index))};
            OpStore(OpAccessChain(output_vec4, outputs[i], invocation_id), in_param);
        }

        OpReturn();
        OpFunctionEnd();
    }

    /// Emits a passthrough quad tessellation evaluation shader that outputs 4 control points.
    void EmitPassthroughTES() {
        DefineEntry(spv::ExecutionModel::TessellationEvaluation);

        // const int index = int(gl_TessCoord.y) * 2 + int(gl_TessCoord.x);
        const Id input_float{TypePointer(spv::StorageClass::Input, float_id)};
        const Id tess_coord_x{OpLoad(float_id, OpAccessChain(input_float, gl_tess_coord, Int(0)))};
        const Id tess_coord_y{OpLoad(float_id, OpAccessChain(input_float, gl_tess_coord, Int(1)))};
        const Id index{OpIAdd(int_id, OpIMul(int_id, OpConvertFToS(int_id, tess_coord_y), Int(2)),
                              OpConvertFToS(int_id, tess_coord_x))};

        // gl_Position = gl_in[index].gl_Position;
        const Id input_vec4{TypePointer(spv::StorageClass::Input, vec4_id)};
        const Id output_vec4{TypePointer(spv::StorageClass::Output, vec4_id)};
        const Id position{OpLoad(vec4_id, OpAccessChain(input_vec4, gl_in, index, Int(0)))};
        OpStore(OpAccessChain(output_vec4, gl_per_vertex, Int(0)), position);

        // out_paramN = in_paramN[index];
        for (int i = 0; i < inputs.size(); i++) {
            const Id param{OpLoad(vec4_id, OpAccessChain(input_vec4, inputs[i], index))};
            OpStore(outputs[i], param);
        }

        OpReturn();
        OpFunctionEnd();
    }

private:
    Id Int(s32 value) {
        return Constant(int_id, value);
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

    void DefineEntry(spv::ExecutionModel model) {
        AddCapability(spv::Capability::Shader);
        AddCapability(spv::Capability::Tessellation);
        const Id void_function{TypeFunction(void_id)};
        main = OpFunction(void_id, spv::FunctionControlMask::MaskNone, void_function);
        if (model == spv::ExecutionModel::TessellationControl) {
            AddExecutionMode(main, spv::ExecutionMode::OutputVertices, 4U);
        } else {
            AddExecutionMode(main, spv::ExecutionMode::Quads);
            AddExecutionMode(main, spv::ExecutionMode::SpacingEqual);
            AddExecutionMode(main, spv::ExecutionMode::VertexOrderCw);
        }
        DefineInputs(model);
        DefineOutputs(model);
        AddEntryPoint(model, main, "main", interfaces);
        AddLabel(OpLabel());
    }

    void DefineOutputs(spv::ExecutionModel model) {
        if (model == spv::ExecutionModel::TessellationControl) {
            const Id gl_per_vertex_array{TypeArray(gl_per_vertex_type, Constant(uint_id, 4U))};
            gl_out = AddOutput(gl_per_vertex_array);

            const Id arr2_id{TypeArray(float_id, Constant(uint_id, 2U))};
            gl_tess_level_inner = AddOutput(arr2_id);
            Decorate(gl_tess_level_inner, spv::Decoration::BuiltIn, spv::BuiltIn::TessLevelInner);
            Decorate(gl_tess_level_inner, spv::Decoration::Patch);

            const Id arr4_id{TypeArray(float_id, Constant(uint_id, 4U))};
            gl_tess_level_outer = AddOutput(arr4_id);
            Decorate(gl_tess_level_outer, spv::Decoration::BuiltIn, spv::BuiltIn::TessLevelOuter);
            Decorate(gl_tess_level_outer, spv::Decoration::Patch);
        } else {
            gl_per_vertex = AddOutput(gl_per_vertex_type);
        }
        outputs.reserve(fs_info.num_inputs);
        for (int i = 0; i < fs_info.num_inputs; i++) {
            const auto& input = fs_info.inputs[i];
            if (input.IsDefault()) {
                continue;
            }
            outputs.emplace_back(AddOutput(model == spv::ExecutionModel::TessellationControl
                                               ? TypeArray(vec4_id, Int(4))
                                               : vec4_id));
            Decorate(outputs.back(), spv::Decoration::Location, input.param_index);
        }
    }

    void DefineInputs(spv::ExecutionModel model) {
        if (model == spv::ExecutionModel::TessellationEvaluation) {
            gl_tess_coord = AddInput(vec3_id);
            Decorate(gl_tess_coord, spv::Decoration::BuiltIn, spv::BuiltIn::TessCoord);
        } else {
            gl_invocation_id = AddInput(int_id);
            Decorate(gl_invocation_id, spv::Decoration::BuiltIn, spv::BuiltIn::InvocationId);
        }
        const Id gl_per_vertex_array{TypeArray(gl_per_vertex_type, Constant(uint_id, 32U))};
        gl_in = AddInput(gl_per_vertex_array);
        const Id float_arr{TypeArray(vec4_id, Int(32))};
        inputs.reserve(fs_info.num_inputs);
        for (int i = 0; i < fs_info.num_inputs; i++) {
            const auto& input = fs_info.inputs[i];
            if (input.IsDefault()) {
                continue;
            }
            inputs.emplace_back(AddInput(float_arr));
            Decorate(inputs.back(), spv::Decoration::Location, input.param_index);
        }
    }

private:
    HwFragmentRuntimeInfo fs_info;
    Id main;
    Id void_id;
    Id bool_id;
    Id float_id;
    Id uint_id;
    Id int_id;
    Id bvec2_id;
    Id vec2_id;
    Id vec3_id;
    Id vec4_id;
    Id float_one;
    Id float_min_one;
    Id int_zero;
    Id gl_per_vertex_type;
    Id gl_in;
    union {
        Id gl_out;
        Id gl_per_vertex;
    };
    Id gl_tess_level_inner;
    Id gl_tess_level_outer;
    union {
        Id gl_tess_coord;
        Id gl_invocation_id;
    };
    std::vector<Id> inputs;
    std::vector<Id> outputs;
    std::vector<Id> interfaces;
};

std::vector<u32> EmitAuxilaryTessShader(AuxShaderType type, const HwFragmentRuntimeInfo& fs_info) {
    QuadRectListEmitter ctx{fs_info};
    switch (type) {
    case AuxShaderType::RectListTCS:
        ctx.EmitRectListTCS();
        break;
    case AuxShaderType::QuadListTCS:
        ctx.EmitQuadListTCS();
        break;
    case AuxShaderType::PassthroughTES:
        ctx.EmitPassthroughTES();
        break;
    }
    return ctx.Assemble();
}

namespace {

// sotc: pixel shader inputs as the SPIR-V backend declares them: clip distances (emulated) first.
u32 NumParams(const HwFragmentRuntimeInfo& fs) {
    return fs.num_inputs - (fs.clip_distance_emulation ? 1 : 0);
}

u32 BindLocation(const HwFragmentRuntimeInfo& fs, u32 input) {
    return fs.inputs[input].param_index + (fs.clip_distance_emulation ? 1 : 0);
}

} // Anonymous namespace

BarycentricGsLayout MakeBarycentricGsLayout(const HwFragmentRuntimeInfo& fs_runtime,
                                            const Info& fs_info) {
    BarycentricGsLayout layout{fs_runtime.clip_distance_emulation ? 1u : 0u, 0};
    for (u32 i = 0; i < NumParams(fs_runtime); ++i) {
        if (fs_runtime.inputs[i].IsDefault()) {
            continue;
        }
        layout.base = std::max(layout.base, BindLocation(fs_runtime, i) + 1);
        if (fs_info.fs_interpolation[i].primary == Qualifier::PerVertex) {
            layout.per_vertex_mask |= 1u << i;
        }
    }
    return layout;
}

bool NeedsBarycentricGs(const HwFragmentRuntimeInfo& fs_runtime, const Info& fs_info) {
    for (u32 attr = u32(IR::Attribute::BaryCoordNoPersp);
         attr <= u32(IR::Attribute::BaryCoordPullModel); ++attr) {
        if (fs_info.loads.GetAny(IR::Attribute(attr))) {
            return true;
        }
    }
    return MakeBarycentricGsLayout(fs_runtime, fs_info).per_vertex_mask != 0;
}

std::vector<u32> EmitBarycentricGeometryShader(const HwFragmentRuntimeInfo& fs_runtime,
                                               const Info& fs_info) {
    const BarycentricGsLayout layout = MakeBarycentricGsLayout(fs_runtime, fs_info);
    Sirit::Module m{SPIRV_VERSION_1_5};
    m.AddCapability(spv::Capability::Shader);
    m.AddCapability(spv::Capability::Geometry);
    const Id void_id{m.TypeVoid()};
    const Id float_id{m.TypeFloat(32)};
    const Id uint_id{m.TypeUInt(32U)};
    const Id int_id{m.TypeInt(32U, true)};
    const Id vec3_id{m.TypeVector(float_id, 3)};
    const Id vec4_id{m.TypeVector(float_id, 4)};
    const Id vec4_arr3{m.TypeArray(vec4_id, m.Constant(uint_id, 3U))};
    const Id in_vec4{m.TypePointer(spv::StorageClass::Input, vec4_id)};
    const Id out_vec4{m.TypePointer(spv::StorageClass::Output, vec4_id)};
    const auto index = [&](s32 value) { return m.Constant(int_id, value); };

    const Id float_arr{m.TypeArray(float_id, m.Constant(uint_id, 1U))};
    const Id per_vertex{m.TypeStruct(vec4_id, float_id, float_arr, float_arr)};
    m.Decorate(per_vertex, spv::Decoration::Block);
    m.MemberDecorate(per_vertex, 0U, spv::Decoration::BuiltIn, u32(spv::BuiltIn::Position));
    m.MemberDecorate(per_vertex, 1U, spv::Decoration::BuiltIn, u32(spv::BuiltIn::PointSize));
    m.MemberDecorate(per_vertex, 2U, spv::Decoration::BuiltIn, u32(spv::BuiltIn::ClipDistance));
    m.MemberDecorate(per_vertex, 3U, spv::Decoration::BuiltIn, u32(spv::BuiltIn::CullDistance));

    std::vector<Id> interfaces;
    const auto variable = [&](spv::StorageClass storage, Id type, std::optional<u32> location) {
        const Id id{m.AddGlobalVariable(m.TypePointer(storage, type), storage)};
        if (location) {
            m.Decorate(id, spv::Decoration::Location, *location);
        }
        interfaces.push_back(id);
        return id;
    };
    const Id gl_in{variable(spv::StorageClass::Input,
                            m.TypeArray(per_vertex, m.Constant(uint_id, 3U)), std::nullopt)};
    const Id gl_out{variable(spv::StorageClass::Output, per_vertex, std::nullopt)};

    // Inputs as the vertex shader writes them; outputs as the pixel shader reads them.
    struct Param {
        Id in, out;
        bool per_vertex;
    };
    std::vector<Param> params;
    if (fs_runtime.clip_distance_emulation) {
        params.push_back({variable(spv::StorageClass::Input, vec4_arr3, 0U),
                          variable(spv::StorageClass::Output, vec4_id, 0U), false});
    }
    for (u32 i = 0; i < NumParams(fs_runtime); ++i) {
        if (fs_runtime.inputs[i].IsDefault()) {
            continue;
        }
        const Id in{variable(spv::StorageClass::Input, vec4_arr3, BindLocation(fs_runtime, i))};
        const auto primary = fs_info.fs_interpolation[i].primary;
        if (layout.per_vertex_mask & (1u << i)) {
            const Id out{variable(spv::StorageClass::Output, vec4_arr3, layout.Location(i))};
            m.Decorate(out, spv::Decoration::Flat);
            params.push_back({in, out, true});
            continue;
        }
        const Id out{variable(spv::StorageClass::Output, vec4_id, BindLocation(fs_runtime, i))};
        if (primary != Qualifier::Smooth) { // as the pixel shader decorates it
            m.Decorate(out, primary == Qualifier::Flat ? spv::Decoration::Flat
                                                       : spv::Decoration::NoPerspective);
        }
        params.push_back({in, out, false});
    }
    const Id bary_smooth{variable(spv::StorageClass::Output, vec3_id, layout.Smooth())};
    const Id bary_nopersp{variable(spv::StorageClass::Output, vec3_id, layout.NoPerspective())};
    m.Decorate(bary_nopersp, spv::Decoration::NoPerspective);

    const Id main{m.OpFunction(void_id, spv::FunctionControlMask::MaskNone, m.TypeFunction(void_id))};
    m.AddExecutionMode(main, spv::ExecutionMode::Triangles);
    m.AddExecutionMode(main, spv::ExecutionMode::OutputTriangleStrip);
    m.AddExecutionMode(main, spv::ExecutionMode::OutputVertices, 3U);
    m.AddExecutionMode(main, spv::ExecutionMode::Invocations, 1U);
    m.AddEntryPoint(spv::ExecutionModel::Geometry, main, "main", interfaces);
    m.AddLabel(m.OpLabel());
    const Id zero{m.Constant(float_id, 0.0f)};
    const Id one{m.Constant(float_id, 1.0f)};
    for (s32 v = 0; v < 3; ++v) {
        const Id position{m.OpLoad(vec4_id, m.OpAccessChain(in_vec4, gl_in, index(v), index(0)))};
        m.OpStore(m.OpAccessChain(out_vec4, gl_out, index(0)), position);
        for (const Param& param : params) {
            if (param.per_vertex) {
                for (s32 k = 0; k < 3; ++k) {
                    m.OpStore(m.OpAccessChain(out_vec4, param.out, index(k)),
                              m.OpLoad(vec4_id, m.OpAccessChain(in_vec4, param.in, index(k))));
                }
            } else {
                m.OpStore(param.out,
                          m.OpLoad(vec4_id, m.OpAccessChain(in_vec4, param.in, index(v))));
            }
        }
        const Id bary{m.ConstantComposite(vec3_id, v == 0 ? one : zero, v == 1 ? one : zero,
                                          v == 2 ? one : zero)};
        m.OpStore(bary_smooth, bary);
        m.OpStore(bary_nopersp, bary);
        m.OpEmitVertex();
    }
    m.OpEndPrimitive();
    m.OpReturn();
    m.OpFunctionEnd();
    return m.Assemble();
}

} // namespace Shader::Backend::SPIRV
