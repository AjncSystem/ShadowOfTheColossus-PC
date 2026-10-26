// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace AmdGpu {
struct ComputeProgram;
union Regs;
} // namespace AmdGpu

namespace Shader {
struct Info;
}

namespace Vulkan {

class Rasterizer;

/// sotc: bytes a read-only buffer binding of a known shader can actually touch (0 = no hint), so
/// the binding does not cover the 64 MiB window of an unbounded descriptor. `index` is the
/// position in Info::buffers; the stride must match the one the hint was derived from.
/// SOTC_BIND_HINT=0 disables the hints.
u64 BufferExtentHint(const Shader::Info& info, u32 index, u32 stride);

/// sotc: the guest range a known shader reads through raw pointers (uses_dma), so that range is
/// synchronised instead of sweeping all resident memory. False for any other shader (or when the
/// shader's hints are off, or its user data does not look as analysed): the sweep runs.
bool KnownDmaRange(const Shader::Info& info, VAddr& address, u64& size);

/// Attempts to execute a shader using HLE if possible.
bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer);

} // namespace Vulkan
