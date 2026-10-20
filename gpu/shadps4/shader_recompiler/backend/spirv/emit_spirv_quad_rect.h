// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <bit>
#include <vector>
#include "common/types.h"

namespace Shader {
struct HwFragmentRuntimeInfo;
struct Info;
}

namespace Shader::Backend::SPIRV {

/// sotc: Profile::barycentric_via_gs (GPUs without fragment shader barycentrics, NVIDIA GTX 10xx):
/// the output locations of the geometry shader that hands the pixel shader the three vertex values
/// of its per-vertex inputs and the barycentrics. Per-vertex input i (bit i of per_vertex_mask)
/// gets three locations from base, in input order; the smooth and noperspective barycentrics follow.
struct BarycentricGsLayout {
    u32 base;
    u32 per_vertex_mask;

    u32 Location(u32 input) const {
        return base + 3 * std::popcount(per_vertex_mask & ((1u << input) - 1));
    }
    u32 Smooth() const {
        return base + 3 * std::popcount(per_vertex_mask);
    }
    u32 NoPerspective() const {
        return Smooth() + 1;
    }
};

[[nodiscard]] BarycentricGsLayout MakeBarycentricGsLayout(const HwFragmentRuntimeInfo& fs_runtime,
                                                          const Info& fs_info);

/// sotc: true when a pixel shader needs that geometry shader (per-vertex inputs or barycentrics).
[[nodiscard]] bool NeedsBarycentricGs(const HwFragmentRuntimeInfo& fs_runtime, const Info& fs_info);

[[nodiscard]] std::vector<u32> EmitBarycentricGeometryShader(const HwFragmentRuntimeInfo& fs_runtime,
                                                             const Info& fs_info);

enum class AuxShaderType : u32 {
    RectListTCS,
    QuadListTCS,
    PassthroughTES,
};

[[nodiscard]] std::vector<u32> EmitAuxilaryTessShader(AuxShaderType type,
                                                      const HwFragmentRuntimeInfo& fs_info);

} // namespace Shader::Backend::SPIRV
