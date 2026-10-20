// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <vector>
#include "common/types.h"

namespace Shader {
struct Info;
struct HwFragmentRuntimeInfo;
struct Profile;
} // namespace Shader

namespace Shader::Backend::SPIRV {

/**
 * Fragment input layout for GPUs without VK_KHR_fragment_shader_barycentric or
 * VK_AMD_shader_explicit_vertex_parameter (NVIDIA Pascal, issue #57). A generated geometry
 * shader gives the fragment shader what those extensions would: the three vertex values of each
 * per-vertex input (as a flat array of 3) and the barycentric coordinates (smooth and
 * noperspective vec3 inputs). The fragment shader then interpolates by hand as on the KHR path.
 *
 * When the layout does not fit in the device limits, active is false: the fragment shader keeps
 * the vertex shader locations, reads per-vertex inputs as flat and the barycentrics as zero.
 */
struct BaryEmulationLayout {
    static constexpr s32 Unused = -1;

    bool active{};
    /// Fragment inputs without the emulated clip distance one (always at location 0).
    u32 num_inputs{};
    /// Fragment shader location of each input; the first of three when per_vertex.
    std::array<s32, 32> location{};
    std::array<bool, 32> per_vertex{};
    s32 bary_smooth{Unused};
    s32 bary_nopersp{Unused};
    u32 num_locations{};
};

[[nodiscard]] BaryEmulationLayout ComputeBaryEmulationLayout(const Info& fs_info,
                                                             const HwFragmentRuntimeInfo& fs,
                                                             const Profile& profile);

enum class BaryGsInput : u32 {
    Points,
    Lines,
    Triangles,
};

struct BaryGsParams {
    BaryGsInput input{BaryGsInput::Triangles};
    /// Added to param_index to find each input in the previous stage's outputs.
    u32 input_location_offset{};
    /// Emulated clip distances (vec4 at location 0) to forward.
    bool clip_distance_attr{};
    /// gl_ClipDistance / gl_CullDistance array sizes written by the previous stage (0: none).
    u32 num_clip_distances{};
    u32 num_cull_distances{};
};

[[nodiscard]] std::vector<u32> EmitBaryEmulationGeometryShader(const BaryEmulationLayout& layout,
                                                               const HwFragmentRuntimeInfo& fs,
                                                               const BaryGsParams& params);

} // namespace Shader::Backend::SPIRV
