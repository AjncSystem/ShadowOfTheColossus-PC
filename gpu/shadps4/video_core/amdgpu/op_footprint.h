// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// sotc: SOTC_CULL_EXPRESS. The guest memory a draw or dispatch reads and writes, as address ranges,
// so the draw thread can tell whether two queued ops may swap places. Conservative by contract:
// an extra range only costs a missed reorder, a missing one lets a reorder corrupt rendering.
// Every range is rounded out to 4 KiB pages (the granularity the buffer / texture caches track
// CPU and GPU modifications at); `known == false` blocks any reorder across the op.
//
// Two stages:
//  1. CollectResourceFootprint: the sharps (V#/T#) the shaders of one op bind, the vertex buffers
//     and the vertex-fetch tables. It reads the per-draw shader data (Shader::Info::runtime[
//     g_info_slot]: user data and flat buffer), so it must run on a thread whose slot holds the
//     data of THIS op (a GpuPrep thread after the PrepLoop setup, or the draw thread's slot).
//     The result is stored in PreparedDraw::resources (draw_prep.h).
//  2. ComputeDrawFootprint / ComputeDispatchFootprint: add the register-derived parts (render
//     targets, depth/stencil, HTILE/CMASK/FMASK, index buffer, indirect arguments) and return the
//     complete footprint. Pure functions of their arguments.

#include <span>

#include <boost/container/small_vector.hpp>

#include "common/types.h"

namespace Shader {
struct Info;
namespace Gcn {
struct FetchShaderData;
}
} // namespace Shader

namespace Vulkan {
struct PreparedDraw;
}

namespace AmdGpu {

union Regs;
struct ComputeProgram;

struct OpRange {
    VAddr addr;
    u64 size;
};

struct OpFootprint {
    /// buffers, textures, vertex/index, indirect args, guest memory read at record time
    boost::container::small_vector<OpRange, 16> reads;
    /// storage buffers, render targets, depth, written images
    boost::container::small_vector<OpRange, 8> writes;
    /// false = unknown footprint: blocks reordering
    bool known = false;
};

/// The op-packet arguments the registers do not carry (BackOp fields of the same names).
struct OpArgs {
    bool indirect{};    ///< DrawIndirect / DispatchIndirect
    bool is_indexed{};  ///< draws
    u32 index_offset{}; ///< direct indexed draws: first index (BackOp::index_offset)
    VAddr address{};    ///< indirect: argument buffer base
    u32 offset{};       ///< indirect: byte offset into it
    u32 stride{};       ///< indirect draws: bytes between argument records
    u32 max_count{};    ///< indirect draws
    VAddr count_address{}; ///< indirect draws with a count buffer (0: none)
    u32 size{};         ///< indirect dispatch: argument bytes (BackOp::size)
};

/// Stage 1. `stages`: the shader stages of the op (Pipeline::GetStages(); null entries allowed).
/// `fetch`: the pipeline's vertex fetch shader (null / empty for compute and fetch-less
/// vertex shaders); its attributes are read through the Vertex stage's user data.
/// Fills out.reads / out.writes with the shader-bound buffers, images and vertex buffers, and
/// out.known = false when the shaders touch memory no range can name (DMA pointers, GDS, FMask
/// bindings, bad sharps tables). Replaces the contents of `out`.
void CollectResourceFootprint(std::span<const Shader::Info* const> stages,
                              const Shader::Gcn::FetchShaderData* fetch, OpFootprint& out);

/// Stage 2, graphics. Needs prep.resources_ready (else known = false). `args` is required (null:
/// unknown): it tells direct from indirect and indexed from not. Returns out.known.
bool ComputeDrawFootprint(const Vulkan::PreparedDraw& prep, const Regs& regs, OpFootprint& out,
                          const OpArgs* args);

/// Stage 2, compute. Same contract; `cs` is the dispatch's register state.
bool ComputeDispatchFootprint(const Vulkan::PreparedDraw& prep, const ComputeProgram& cs,
                              OpFootprint& out, const OpArgs* args);

/// True when the two ops may not swap: some write of one overlaps a read or write of the other,
/// or either footprint is unknown.
bool Conflicts(const OpFootprint& a, const OpFootprint& b) noexcept;

} // namespace AmdGpu
