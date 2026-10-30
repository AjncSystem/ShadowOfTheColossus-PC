// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: SOTC_CULL_EXPRESS, see op_footprint.h.

#include <algorithm>
#include <cstring>

#include <magic_enum/magic_enum.hpp>

#include "common/alignment.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/draw_prep.h"
#include "video_core/amdgpu/op_footprint.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/texture_cache/image_info.h"

namespace AmdGpu {

namespace {

constexpr u64 PageSize = 4096;
constexpr u64 AddrLimit = 1ULL << 48; // ranges are clipped to 48 bits

/// Appends [addr, addr + size) rounded out to pages. Empty and null ranges are dropped (a null
/// descriptor binds no memory).
template <typename Vec>
void AddRange(Vec& vec, u64 addr, u64 size) {
    if (size == 0 || addr == 0) {
        return;
    }
    addr &= AddrLimit - 1;
    u64 end = size >= AddrLimit - addr ? AddrLimit : addr + size;
    const u64 begin = addr & ~(PageSize - 1);
    end = std::min<u64>((end + PageSize - 1) & ~(PageSize - 1), AddrLimit);
    if (end <= begin) {
        return;
    }
    if (!vec.empty() && vec.back().addr == begin && vec.back().size == end - begin) {
        return;
    }
    vec.push_back(OpRange{begin, end - begin});
}

/// Sorts and merges overlapping / touching ranges.
template <typename Vec>
void Normalize(Vec& vec) {
    if (vec.size() < 2) {
        return;
    }
    std::sort(vec.begin(), vec.end(),
              [](const OpRange& a, const OpRange& b) { return a.addr < b.addr; });
    size_t out = 0;
    for (size_t i = 1; i < vec.size(); ++i) {
        if (vec[i].addr <= vec[out].addr + vec[out].size) {
            const u64 end = std::max(vec[out].addr + vec[out].size, vec[i].addr + vec[i].size);
            vec[out].size = end - vec[out].addr;
        } else {
            vec[++out] = vec[i];
        }
    }
    vec.resize(out + 1);
}

bool Overlaps(const OpRange& a, const OpRange& b) noexcept {
    return a.addr < b.addr + b.size && b.addr < a.addr + a.size;
}

bool AnyOverlap(const auto& xs, const auto& ys) noexcept {
    for (const OpRange& x : xs) {
        for (const OpRange& y : ys) {
            if (Overlaps(x, y)) {
                return true;
            }
        }
    }
    return false;
}

u64 BytesPerElement(u32 bits) {
    return std::max<u64>((bits + 7) / 8, 1);
}

/// Upper bound of a texture chain in bytes, from the T# fields alone (padded dimensions, all
/// slices and samples, x4 for the mip chain and the tile / bank padding of the small mips).
u64 TextureUpperBound(const Image& image, u32 num_bits) {
    const u64 pitch = Common::AlignUp<u64>(image.Pitch(), 256);
    const u64 height = Common::AlignUp<u64>(u64{image.height} + 1, 256);
    const u64 depth = (image.GetType() == ImageType::Color3D ? u64{image.depth} + 1 : 1) * 4;
    const u64 layers = std::max<u64>(image.NumLayers(), 1) * image.NumSamples();
    return pitch * height * depth * layers * BytesPerElement(num_bits) * 4;
}

/// Bytes of an image chain the way ImageInfo lays it out, or its upper bound when the u32 sizes of
/// ImageInfo could have wrapped. `ok` false: layout not computable (unknown).
u64 TextureBytes(const Image& tsharp, const Shader::ImageResource& desc, bool& ok) {
    const auto data_fmt = tsharp.GetDataFmt();
    const auto num_fmt = tsharp.GetNumberFmt();
    ok = false;
    if (data_fmt == DataFormat::FormatInvalid || !magic_enum::enum_contains(data_fmt) ||
        !magic_enum::enum_contains(num_fmt)) {
        return 0;
    }
    const ArrayMode array_mode = GetArrayMode(tsharp.GetTileMode());
    // ImageInfo::UpdateSize asserts on these.
    if (array_mode == ArrayMode::ArrayLinearGeneral ||
        (IsBlockCoded(data_fmt) && IsMacroTiled(array_mode))) {
        return 0;
    }
    const VideoCore::ImageInfo info{tsharp, desc};
    const u64 bound = TextureUpperBound(tsharp, info.num_bits);
    ok = true;
    return bound >= (1ULL << 32) ? bound : std::max<u64>(info.guest_size, 1);
}

void AddBuffer(OpFootprint& out, const Buffer& vsharp, bool written) {
    if (vsharp.base_address <= 1 || vsharp.GetSize() == 0) { // Null() or unused
        return;
    }
    // Full descriptor range even where the binding is later clamped or windowed: those only
    // shrink what is bound, so this over-estimates.
    if (written) {
        AddRange(out.writes, vsharp.base_address, vsharp.GetSize());
    } else {
        AddRange(out.reads, vsharp.base_address, vsharp.GetSize());
    }
}

void AddImage(OpFootprint& out, const Image& tsharp, const Shader::ImageResource& desc) {
    if (tsharp.Address() == 0) {
        return;
    }
    bool ok = false;
    const u64 bytes = TextureBytes(tsharp, desc, ok);
    if (!ok) {
        // A T# with a valid address whose layout cannot be computed is a garbage / unsupported
        // descriptor; the rasterizer would not bind it either, but stay conservative.
        if (tsharp.GetDataFmt() != DataFormat::FormatInvalid) {
            out.known = false;
        }
        return;
    }
    if (desc.is_written) {
        AddRange(out.writes, tsharp.Address(), bytes);
    } else {
        AddRange(out.reads, tsharp.Address(), bytes);
    }
}

/// Bytes of a linear (non-tiled) surface of pitch x height, padded generously.
u64 LinearSurfaceBytes(u64 pitch, u64 height, u64 bytes_per_element, u64 samples, u64 layers) {
    return Common::AlignUp<u64>(pitch, 256) * Common::AlignUp<u64>(height, 256) *
           bytes_per_element * samples * layers;
}

void AddColorTarget(OpFootprint& out, const ColorBuffer& cb) {
    const u64 layers = cb.NumSlices();
    const u64 bpe = BytesPerElement(NumBitsPerBlock(DataFormat(cb.info.format)));
    u64 bytes;
    if (cb.IsTiled()) {
        bytes = u64(cb.slice.tile_max) + 1 * 64 * bpe * cb.NumSamples() * layers;
    } else {
        bytes = LinearSurfaceBytes(cb.Pitch(), cb.Height(), bpe, cb.NumSamples(), layers);
    }
    AddRange(out.writes, cb.Address(), bytes);
    const u64 pixels = u64{cb.Pitch()} * std::max<u64>(cb.Height(), 1) * layers;
    if (cb.info.fast_clear && cb.cmask_base_address != 0) {
        // CMASK: 4 bits per 8x8 tile; 1/16 byte per pixel at most. Take 1/4.
        AddRange(out.writes, cb.CmaskAddress(), pixels / 4 + PageSize);
    }
    if (cb.info.compression && cb.fmask_base_address != 0) {
        AddRange(out.writes, cb.FmaskAddress(), pixels * 4 + PageSize);
    }
}

} // namespace

void CollectResourceFootprint(std::span<const Shader::Info* const> stages,
                              const Shader::Gcn::FetchShaderData* fetch, OpFootprint& out) {
    out.reads.clear();
    out.writes.clear();
    out.known = true;
    const Shader::Info* vs_info = nullptr;
    for (const Shader::Info* stage : stages) {
        if (!stage) {
            continue;
        }
        if (stage->sw_stage == Shader::SwStage::Vertex) {
            vs_info = stage;
        }
        // Pointers the shader dereferences by itself, FMask bindings (their extent depends on the
        // render target they alias) and stages without captured user data: no range names them.
        if (stage->uses_dma || !stage->fmasks.empty() || stage->UserData().data() == nullptr) {
            out.known = false;
            return;
        }
        if (stage->FlatBuf().empty() && (!stage->buffers.empty() || !stage->images.empty())) {
            out.known = false;
            return;
        }
        for (const auto& desc : stage->buffers) {
            if (desc.IsSpecial()) {
                // GDS is global shader-visible GPU state: no range, no reorder. The rest is
                // host-side (flat buffer copy, LDS, clip planes, BDA tables, LOD feedback stats).
                if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
                    out.known = false;
                    return;
                }
                continue;
            }
            AddBuffer(out, desc.GetSharp(*stage), desc.is_written);
        }
        for (const auto& image : stage->images) {
            if (image.array_size == 0) {
                AddImage(out, image.GetSharp(*stage), image);
            } else {
                // Any element of the descriptor array may be sampled / written.
                for (u32 i = 0; i < image.array_size; ++i) {
                    AddImage(out, image.GetElementSharp(*stage, i), image);
                }
            }
            if (!out.known) {
                return;
            }
        }
    }
    if (fetch && !fetch->Empty() && !fetch->attributes.empty()) {
        if (!vs_info) {
            out.known = false;
            return;
        }
        const auto user_data = vs_info->UserData();
        for (const auto& attrib : fetch->attributes) {
            // The V# comes from a table in guest memory (read at record time), or from the user
            // data registers themselves.
            if (attrib.sgpr_base != Shader::IR::NumScalarRegs) {
                if (static_cast<size_t>(attrib.sgpr_base) + 1 >= user_data.size()) {
                    out.known = false;
                    return;
                }
                u64 ptr = 0;
                std::memcpy(&ptr, &user_data[attrib.sgpr_base], sizeof(ptr));
                ptr &= 0xFFFFFFFFFFFFULL;
                AddRange(out.reads, ptr + u64{attrib.dword_offset} * 4, sizeof(Buffer));
            } else if (u64{attrib.dword_offset} + sizeof(Buffer) / 4 > user_data.size()) {
                out.known = false;
                return;
            }
            AddBuffer(out, attrib.GetSharp(*vs_info), false);
        }
    }
    Normalize(out.reads);
    Normalize(out.writes);
}

bool ComputeDrawFootprint(const Vulkan::PreparedDraw& prep, const Regs& regs, OpFootprint& out,
                          const OpArgs* args) {
    out.reads.clear();
    out.writes.clear();
    out.known = false;
    // `args` is required for draws: without it an indirect draw cannot be told from a direct one.
    if (!args || !prep.resources_ready || !prep.resources.known) {
        return false;
    }
    using Mode = ColorControl::OperationMode;
    // Filtered / special draws (fast-clear eliminate, FMASK decompress, resolve, no-op primitive,
    // depth-stencil copy override): their memory effects are not described by the bound state.
    const Mode mode = regs.color_control.mode;
    if (mode != Mode::Disable && mode != Mode::Normal) {
        return false;
    }
    if (regs.primitive_type == PrimitiveType::None) {
        return false;
    }
    const auto& db = regs.depth_buffer;
    if (mode == Mode::Disable) {
        const auto& ov = regs.depth_render_override;
        if ((ov.force_z_dirty && ov.force_z_valid) ||
            (ov.force_stencil_dirty && ov.force_stencil_valid)) {
            return false; // DepthStencilOverrideCopy candidates
        }
    }
    out.reads = prep.resources.reads;
    out.writes = prep.resources.writes;

    // Color targets: bound when the mode is Normal, the buffer is set up, its write mask is
    // non-zero and the pixel shader exports it (mrt_mask; 0xFF when not provided).
    if (mode == Mode::Normal) {
        for (u32 cb = 0; cb < NUM_COLOR_BUFFERS; ++cb) {
            const ColorBuffer& col = regs.color_buffers[cb];
            if (!col || !regs.color_target_mask.GetMask(cb) || !(prep.mrt_mask & (1u << cb))) {
                continue;
            }
            AddColorTarget(out, col);
        }
    }

    // Depth / stencil, bound as in PrepareRenderState. Read and write addresses may differ
    // (copy-style passes): both are included, as reads always and as writes whenever the draw may
    // write them (the depth_write_enable / z_read_only refinement is not trusted).
    const bool depth_bound = regs.depth_control.depth_enable && db.DepthValid();
    const bool stencil_bound = regs.depth_control.stencil_enable && db.StencilValid();
    if (depth_bound || stencil_bound) {
        const u64 layers = regs.depth_view.NumSlices();
        const u64 samples = db.NumSamples();
        const auto& rc = regs.depth_render_control;
        const bool may_write = regs.depth_control.depth_write_enable ||
                               regs.depth_control.stencil_enable || rc.depth_clear_enable ||
                               rc.stencil_clear_enable || rc.depth_copy || rc.stencil_copy ||
                               rc.resummarize_enable || rc.decompress_enable;
        const u64 bpe = db.NumBits() >> 3;
        u64 depth_bytes;
        if (db.IsTiled()) {
            depth_bytes = u64(db.depth_slice.tile_max) + 1 * 64 * bpe * samples * layers;
        } else {
            depth_bytes = LinearSurfaceBytes(db.Pitch(), db.Height(), bpe, samples, layers);
        }
        const u64 stencil_bytes = LinearSurfaceBytes(db.Pitch(), db.Height(), 1, samples, layers);
        const auto add_surface = [&](u64 addr, u64 bytes) {
            AddRange(out.reads, addr, bytes);
            if (may_write) {
                AddRange(out.writes, addr, bytes);
            }
        };
        if (depth_bound) {
            add_surface(db.DepthAddress(), depth_bytes);
            if (db.DepthWriteValid()) {
                add_surface(db.DepthWriteAddress(), depth_bytes);
            }
        }
        if (db.StencilValid()) {
            add_surface(db.StencilAddress(), stencil_bytes);
        }
        if (db.StencilWriteValid()) {
            add_surface(db.StencilWriteAddress(), stencil_bytes);
        }
        if (db.z_info.tile_surface_enable && regs.depth_htile_data_base.address != 0) {
            // HTILE: 4 bytes per 8x8 tile = 1/16 byte per pixel (32-bit depth); take 1/4.
            const u64 pixels = u64{db.Pitch()} * std::max<u64>(db.Height(), 1) * layers;
            add_surface(regs.depth_htile_data_base.GetAddress(), pixels / 4 + PageSize);
        }
    }

    // Index buffer: from the base address to the last index the draw can reach.
    if (args->is_indexed) {
        const u64 isz = regs.index_buffer_type.index_type == IndexType::Index16 ? 2 : 4;
        AddRange(out.reads, regs.index_base_address.Address<VAddr>(),
                 (u64{args->index_offset} + std::max<u64>(regs.num_indices, 1)) * isz);
    }

    // Indirect arguments (and the count word).
    if (args->indirect) {
        AddRange(out.reads, args->address + args->offset,
                 u64{args->stride} * std::max<u32>(args->max_count, 1) + 32);
        if (args->count_address != 0) {
            AddRange(out.reads, args->count_address, 4);
        }
    }
    Normalize(out.reads);
    Normalize(out.writes);
    out.known = true;
    return true;
}

bool ComputeDispatchFootprint(const Vulkan::PreparedDraw& prep, const ComputeProgram& cs,
                              OpFootprint& out, const OpArgs* args) {
    static_cast<void>(cs);
    out.reads.clear();
    out.writes.clear();
    out.known = false;
    if (!args || !prep.resources_ready || !prep.resources.known) {
        return false;
    }
    out.reads = prep.resources.reads;
    out.writes = prep.resources.writes;
    if (args->indirect) {
        AddRange(out.reads, args->address + args->offset, std::max<u64>(args->size, 12) + 16);
    }
    Normalize(out.reads);
    Normalize(out.writes);
    out.known = true;
    return true;
}

bool Conflicts(const OpFootprint& a, const OpFootprint& b) noexcept {
    if (!a.known || !b.known) {
        return true;
    }
    return AnyOverlap(a.writes, b.reads) || AnyOverlap(a.writes, b.writes) ||
           AnyOverlap(a.reads, b.writes);
}

} // namespace AmdGpu
