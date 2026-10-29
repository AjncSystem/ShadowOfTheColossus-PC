// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_HLE_SHADOW_CULL=1 (default off). CPU execution of Shadow of the Colossus's shadow-scene
// frustum cull compute shader (program hash 0x4defcf7f, recorded by eboot 0x19c1a0 once per sun
// cascade and spot light). The two visibility bitsets it writes are read by the game's CPU right
// after the shadow group's end-of-pipe label; computed here, at the dispatch's place in the command
// stream, they are CPU-written memory and those reads no longer wait for the GPU. The semantics,
// the error model and the conservativeness argument are in sotc_shadow_cull.cpp.
#pragma once

#include "common/types.h"

namespace AmdGpu {
struct ComputeProgram;
}

namespace Shader {
struct Info;
}

namespace Vulkan {
class Rasterizer;
}

namespace SotcShadowCull {

inline constexpr u64 kProgramHash = 0x4defcf7f;

/// Computes the dispatch on the CPU and writes both bitsets (guest memory and, when resident, the
/// GPU copy) through the buffer cache. False: nothing observable was done, or an output write was
/// refused after the first one landed; either way the caller runs the shader on the GPU, which
/// writes both outputs in full. Call only for kProgramHash with the switch on.
bool Execute(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
             Vulkan::Rasterizer& rasterizer);

} // namespace SotcShadowCull
