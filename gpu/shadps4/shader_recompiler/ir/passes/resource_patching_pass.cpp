// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>
#include <optional>
#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/opcodes.h"
#include "shader_recompiler/ir/operand_helper.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/passes/resource_pass.h"
#include "shader_recompiler/ir/reinterpret.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/resource.h"
#include "video_core/amdgpu/resource.h"

namespace Shader::Optimization {
namespace {

IR::Type BufferDataType(const IR::Inst& inst, const Profile& profile) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::LoadBufferU8:
    case IR::Opcode::StoreBufferU8:
        return IR::Type::U8;
    case IR::Opcode::LoadBufferU16:
    case IR::Opcode::StoreBufferU16:
        return IR::Type::U16;
    case IR::Opcode::LoadBufferU64:
    case IR::Opcode::StoreBufferU64:
    case IR::Opcode::BufferAtomicIAdd64:
    case IR::Opcode::BufferAtomicSMax64:
    case IR::Opcode::BufferAtomicSMin64:
    case IR::Opcode::BufferAtomicUMax64:
    case IR::Opcode::BufferAtomicUMin64:
        return IR::Type::U64;
    case IR::Opcode::BufferAtomicFMax32:
    case IR::Opcode::BufferAtomicFMin32:
        return profile.supports_buffer_fp32_atomic_min_max ? IR::Type::F32 : IR::Type::U32;
    case IR::Opcode::LoadBufferFormatF32:
    case IR::Opcode::StoreBufferFormatF32:
        // Formatted buffer loads can use a variety of types.
        return IR::Type::U32 | IR::Type::F32 | IR::Type::U16 | IR::Type::U8;
    default:
        return IR::Type::U32;
    }
}

u32 BufferAddressShift(const IR::Inst& inst, AmdGpu::DataFormat data_format) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::LoadBufferU8:
    case IR::Opcode::StoreBufferU8:
        return 0;
    case IR::Opcode::LoadBufferU16:
    case IR::Opcode::StoreBufferU16:
        return 1;
    case IR::Opcode::LoadBufferU64:
    case IR::Opcode::StoreBufferU64:
    case IR::Opcode::BufferAtomicIAdd64:
    case IR::Opcode::BufferAtomicSMax64:
    case IR::Opcode::BufferAtomicSMin64:
    case IR::Opcode::BufferAtomicUMax64:
    case IR::Opcode::BufferAtomicUMin64:
        return 3;
    case IR::Opcode::LoadBufferFormatF32:
    case IR::Opcode::StoreBufferFormatF32: {
        switch (data_format) {
        case AmdGpu::DataFormat::Format8:
            return 0;
        case AmdGpu::DataFormat::Format8_8:
        case AmdGpu::DataFormat::Format16:
            return 1;
        case AmdGpu::DataFormat::Format8_8_8_8:
        case AmdGpu::DataFormat::Format16_16:
        case AmdGpu::DataFormat::Format10_11_11:
        case AmdGpu::DataFormat::Format2_10_10_10:
        case AmdGpu::DataFormat::Format16_16_16_16:
        case AmdGpu::DataFormat::Format32:
        case AmdGpu::DataFormat::Format32_32:
        case AmdGpu::DataFormat::Format32_32_32:
        case AmdGpu::DataFormat::Format32_32_32_32:
            return 2;
        default:
            return 0;
        }
        break;
    }
    case IR::Opcode::ReadConstBuffer:
        // Provided address is already in dwords
        return 0;
    default:
        return 2;
    }
}

class Descriptors {
public:
    explicit Descriptors(Info& info_)
        : info{info_}, buffer_resources{info_.buffers}, image_resources{info_.images},
          sampler_resources{info_.samplers}, fmask_resources(info_.fmasks) {}

    u32 Add(const BufferResource& desc) {
        const u32 index{Add(buffer_resources, desc, [&desc](const auto& existing) {
            return desc.sharp_fetch == existing.sharp_fetch && desc.post_op == existing.post_op &&
                   desc.post_op_dw1_mask == existing.post_op_dw1_mask &&
                   desc.buffer_type == existing.buffer_type;
        })};
        auto& buffer = buffer_resources[index];
        buffer.used_types |= desc.used_types;
        buffer.is_written |= desc.is_written;
        buffer.is_formatted |= desc.is_formatted;
        return index;
    }

    u32 Add(const ImageResource& desc) {
        const u32 index{Add(image_resources, desc, [&desc](const auto& existing) {
            return desc.sharp_fetch == existing.sharp_fetch && desc.is_array == existing.is_array &&
                   desc.mip_fallback_mode == existing.mip_fallback_mode &&
                   desc.constant_mip_index == existing.constant_mip_index &&
                   desc.post_op == existing.post_op && desc.array_size == existing.array_size;
        })};
        auto& image = image_resources[index];
        image.is_atomic |= desc.is_atomic;
        image.is_written |= desc.is_written;
        return index;
    }

    u32 Add(const SamplerResource& desc) {
        const u32 index{Add(sampler_resources, desc, [this, &desc](const auto& existing) {
            return desc.sharp_fetch == existing.sharp_fetch && desc.post_op == existing.post_op &&
                   desc.post_op_tsharp_dw3_off == existing.post_op_tsharp_dw3_off &&
                   desc.is_depth == existing.is_depth;
        })};
        return index;
    }

    u32 Add(const FMaskResource& desc) {
        u32 index = Add(fmask_resources, desc, [&desc](const auto& existing) {
            return desc.sharp_idx == existing.sharp_idx;
        });
        return index;
    }

private:
    template <typename Descriptors, typename Descriptor, typename Func>
    static u32 Add(Descriptors& descriptors, const Descriptor& desc, Func&& pred) {
        const auto it{std::ranges::find_if(descriptors, pred)};
        if (it != descriptors.end()) {
            return static_cast<u32>(std::distance(descriptors.begin(), it));
        }
        descriptors.push_back(desc);
        return static_cast<u32>(descriptors.size()) - 1;
    }

    const Info& info;
    BufferResourceList& buffer_resources;
    ImageResourceList& image_resources;
    SamplerResourceList& sampler_resources;
    FMaskResourceList& fmask_resources;
};

} // Anonymous namespace

using SharpSources = boost::container::small_vector<const IR::Inst*, 4>;

SharpLocation SharpLocationFromSource(const IR::Inst* inst) {
    SharpLocation location{};
    if (inst->GetOpcode() == IR::Opcode::GetUserData) {
        return static_cast<SharpLocation>(inst->Arg(0).ScalarReg());
    } else if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
        location = inst->Flags<IR::BufferInstInfo>().flatbuf_off_dw;
    } else {
        // A sharp fetched with a runtime index has no fixed place in the flattened buffer;
        // the dynamic window only serves data loads.
        if (inst->Flags<u32>() & ReadConstDynamicWindowFlag) {
            LOG_WARNING(Render_Recompiler, "Sharp source uses a runtime index");
            return UNKNOWN_LOCATION;
        }
        location = inst->Flags<SharpLocation>();
    }
    if (location == 0) {
        LOG_WARNING(Render_Recompiler, "Sharp source was not flatenned");
        return UNKNOWN_LOCATION;
    }
    return location;
}

template <typename T>
SharpFetch<T> ConstructSharpFetch(const SharpReference& sharp) {
    using Summary = SharpFetch<T>::Summary;
    SharpFetch<T> sharp_fetch{};
    for (u32 i = 0; i < sharp.num_dwords; i++) {
        auto dword = sharp.dwords[i];
        if (dword.IsImmediate()) {
            sharp_fetch.immediates[i] = dword.U32();
        } else {
            sharp_fetch.offsets[i] = SharpLocationFromSource(dword.Inst());
            sharp_fetch.load_mask |= (1 << i);
            if (sharp_fetch.offsets[i] == UNKNOWN_LOCATION) {
                sharp_fetch.summary = Summary::Invalid;
            }
        }
    }
    // An immediate dword has no flat buffer location (its offset stays 0), so it needs the per-dword fetch
    const u32 all_loaded = (1u << sharp.num_dwords) - 1;
    if (sharp_fetch.summary != Summary::Invalid && sharp_fetch.load_mask == all_loaded) {
        const u32 base = sharp_fetch.offsets[0];
        for (u32 i = 1; i < sharp.num_dwords; ++i) {
            if (sharp_fetch.offsets[i] - base != i) {
                return sharp_fetch;
            }
        }
        sharp_fetch.summary = Summary::SingleLoad;
    }
    return sharp_fetch;
}

/// A ReadConst dword offset written as element * scale + offset, element being the only non-constant term
/// (empty if the offset is constant)
struct AffineOffset {
    IR::Value element;
    u32 scale;
    u32 offset;
};

AffineOffset MatchAffineOffset(const IR::Value& value, u32 depth = 0) {
    if (value.IsImmediate()) {
        return {{}, 0, value.U32()};
    }
    const AffineOffset leaf{value, 1, 0};
    const IR::Inst* inst = value.Inst();
    if (depth == 8) {
        return leaf;
    }
    switch (inst->GetOpcode()) {
    case IR::Opcode::IAdd32: {
        const AffineOffset a = MatchAffineOffset(inst->Arg(0), depth + 1);
        const AffineOffset b = MatchAffineOffset(inst->Arg(1), depth + 1);
        if (!a.element.IsEmpty() && !b.element.IsEmpty() && a.element != b.element) {
            return leaf;
        }
        return {a.element.IsEmpty() ? b.element : a.element, a.scale + b.scale,
                a.offset + b.offset};
    }
    case IR::Opcode::IMul32: {
        const bool imm0 = inst->Arg(0).IsImmediate();
        if (imm0 == inst->Arg(1).IsImmediate()) {
            return leaf;
        }
        const u32 factor = inst->Arg(imm0 ? 0 : 1).U32();
        const AffineOffset a = MatchAffineOffset(inst->Arg(imm0 ? 1 : 0), depth + 1);
        return {a.element, a.scale * factor, a.offset * factor};
    }
    case IR::Opcode::ShiftLeftLogical32: {
        if (!inst->Arg(1).IsImmediate() || inst->Arg(1).U32() >= 32) {
            return leaf;
        }
        const u32 shift = inst->Arg(1).U32();
        const AffineOffset a = MatchAffineOffset(inst->Arg(0), depth + 1);
        return {a.element, a.scale << shift, a.offset << shift};
    }
    case IR::Opcode::ShiftRightLogical32: {
        // Exact only when both terms are multiples of the divisor (byte to dword offsets)
        if (!inst->Arg(1).IsImmediate() || inst->Arg(1).U32() >= 32) {
            return leaf;
        }
        const u32 shift = inst->Arg(1).U32();
        const u32 mask = (1u << shift) - 1;
        const AffineOffset a = MatchAffineOffset(inst->Arg(0), depth + 1);
        if ((a.scale & mask) != 0 || (a.offset & mask) != 0) {
            return leaf;
        }
        return {a.element, a.scale >> shift, a.offset >> shift};
    }
    default:
        return leaf;
    }
}

/// Flat buffer location of a flattened pointer dword, without the warning of SharpLocationFromSource
std::optional<SharpLocation> FlatLocation(const IR::Value& value) {
    const IR::Inst* inst = value.TryInst();
    if (!inst) {
        return std::nullopt;
    }
    if (inst->GetOpcode() == IR::Opcode::GetUserData) {
        return static_cast<SharpLocation>(inst->Arg(0).ScalarReg());
    }
    if (inst->GetOpcode() == IR::Opcode::ReadConst && inst->Flags<SharpLocation>() != 0) {
        return inst->Flags<SharpLocation>();
    }
    if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer &&
        inst->Flags<IR::BufferInstInfo>().flatbuf_off_dw != 0) {
        return static_cast<SharpLocation>(inst->Flags<IR::BufferInstInfo>().flatbuf_off_dw);
    }
    return std::nullopt;
}

struct SharpTableMatch {
    SharpLocation ptr_lo;
    SharpLocation ptr_hi;
    IR::Value element;
    u32 stride_dw;
    u32 first_dw;
};

/// Per-game dynamic_tsharp_array_size (from Pink-shadPS4): recognizes a sharp whose dwords are all ReadConst(ptr, element *
/// stride + first + k), ptr being flattened (SotC cs 0xfb974c0e reads the reflection probe cubemap at P +
/// slot * 32 + 14480, slot coming from the probe record)
std::optional<SharpTableMatch> MatchSharpTable(const SharpReference& sharp) {
    std::optional<SharpTableMatch> match;
    for (u32 i = 0; i < sharp.num_dwords; ++i) {
        const IR::Inst* dword = sharp.dwords[i].TryInst();
        // sotc: our flattening marks runtime-indexed loads with a dynamic window (srt.h); the
        // table itself is still addressed by the ReadConst arguments.
        if (!dword || dword->GetOpcode() != IR::Opcode::ReadConst ||
            (dword->Flags<u32>() != 0 && !(dword->Flags<u32>() & ReadConstDynamicWindowFlag))) {
            return std::nullopt;
        }
        const IR::Inst* base = dword->Arg(0).TryInst();
        if (!base || base->GetOpcode() != IR::Opcode::CompositeConstructU32x2) {
            return std::nullopt;
        }
        const auto ptr_lo = FlatLocation(base->Arg(0));
        const auto ptr_hi = FlatLocation(base->Arg(1));
        const AffineOffset offset = MatchAffineOffset(dword->Arg(1));
        if (!ptr_lo || !ptr_hi || offset.element.IsEmpty() || offset.scale < sharp.num_dwords ||
            offset.scale > 256 || offset.offset < i) {
            return std::nullopt;
        }
        if (!match) {
            match = SharpTableMatch{*ptr_lo, *ptr_hi, offset.element, offset.scale,
                                    offset.offset - i};
        } else if (*ptr_lo != match->ptr_lo || *ptr_hi != match->ptr_hi ||
                   offset.element != match->element || offset.scale != match->stride_dw ||
                   offset.offset != match->first_dw + i) {
            return std::nullopt;
        }
    }
    return match;
}

/// Finds or appends the SharpTable holding the matched sharp and returns the fetch of its element 0 (see
/// ImageResource::array_size)
std::optional<SharpFetch<AmdGpu::Image>> AddSharpTable(Info& info, const SharpTableMatch& match,
                                                       u32 num_dwords, u32 num_elements) {
    auto it = std::ranges::find_if(info.sharp_tables, [&](const SharpTable& table) {
        return table.ptr_lo == match.ptr_lo && table.ptr_hi == match.ptr_hi &&
               table.stride_dw == match.stride_dw && table.num_elements == num_elements &&
               match.first_dw >= table.offset_dw &&
               match.first_dw + num_dwords <= table.offset_dw + table.stride_dw;
    });
    if (it == info.sharp_tables.end()) {
        const u32 flatbuf_off = info.srt_info.flattened_bufsize_dw;
        if (info.sharp_tables.size() == info.sharp_tables.capacity() ||
            flatbuf_off + num_elements * match.stride_dw >= UNKNOWN_LOCATION) {
            return std::nullopt;
        }
        info.sharp_tables.push_back({
            .ptr_lo = match.ptr_lo,
            .ptr_hi = match.ptr_hi,
            .flatbuf_off = static_cast<SharpLocation>(flatbuf_off),
            .num_elements = static_cast<u16>(num_elements),
            .offset_dw = match.first_dw,
            .stride_dw = match.stride_dw,
        });
        info.srt_info.flattened_bufsize_dw += num_elements * match.stride_dw;
        // Make the elements visible to the rest of the compilation (image type, cube fixup)
        info.RefreshFlatBuf();
        it = std::prev(info.sharp_tables.end());
    }
    SharpFetch<AmdGpu::Image> fetch{};
    for (u32 i = 0; i < num_dwords; ++i) {
        fetch.offsets[i] =
            static_cast<SharpLocation>(it->flatbuf_off + match.first_dw - it->offset_dw + i);
        fetch.load_mask |= 1u << i;
    }
    LOG_WARNING(Render_Recompiler,
                "Workaround dynamic_tsharp_array_size: shader {:#x} indexes {} T#s at "
                "flat[{}:{}] + {:#x}, stride {} bytes",
                info.pgm_hash, num_elements, match.ptr_lo, match.ptr_hi, match.first_dw * 4,
                match.stride_dw * 4);
    return fetch;
}

/// Descriptors the dynamic_tsharp_array_size arrays of the shader take so far
u32 NumArrayDescriptors(const Info& info) {
    u32 count = 0;
    for (const ImageResource& image : info.images) {
        count += image.array_size != 0 ? image.array_size + 1u : 0u;
    }
    return count;
}

/// Packed image | sampler << 16 bindings of an image instruction handle
u32 ImageHandleBindings(const IR::Value& handle) {
    if (const IR::Inst* array_handle = handle.TryInst()) {
        ASSERT(array_handle->GetOpcode() == IR::Opcode::ImageArrayHandle);
        return array_handle->Arg(0).U32();
    }
    return handle.U32();
}

void PatchBufferSharp(const ResourceDiscovery& resource, Info& info, Descriptors& descriptors,
                      const Profile& profile) {
    IR::Inst& inst = *resource.user;

    const u32 buffer_binding = descriptors.Add(BufferResource{
        .sharp_fetch = ConstructSharpFetch<AmdGpu::Buffer>(resource.sharps[0]),
        .used_types = BufferDataType(inst, profile),
        .buffer_type = BufferType::Guest,
        .is_written = IsBufferStore(inst),
        .is_formatted = inst.GetOpcode() == IR::Opcode::LoadBufferFormatF32 ||
                        inst.GetOpcode() == IR::Opcode::StoreBufferFormatF32,
        .post_op = resource.sharps[0].post_op,
        .post_op_dw1_mask = resource.sharps[0].post_op_data.dw1_mask,
    });

    // Replace handle with binding index in buffer resource list.
    IR::IREmitter ir{*inst.GetParent(), IR::Block::InstructionList::s_iterator_to(inst)};
    inst.SetArg(0, ir.Imm32(buffer_binding));
}

void PatchImageSharp(const ResourceDiscovery& resource, Info& info, Descriptors& descriptors,
                     const Profile& profile) {
    IR::Inst& inst = *resource.user;

    // Read image sharp.
    const auto inst_info = inst.Flags<IR::TextureInstInfo>();
    const bool is_atomic = IsImageAtomicInstruction(inst);
    const bool is_written = inst.GetOpcode() == IR::Opcode::ImageWrite || is_atomic;
    // ImageRead with !is_written gets emitted as OpImageFetch with LOD operand, doesn't
    // need fallback (TODO is this 100% true?)
    const bool needs_mip_storage_fallback =
        inst_info.has_lod && is_written && !profile.supports_image_load_store_lod;
    // Per-game (Pink-shadPS4): a sampled T# read at a dynamic offset becomes a descriptor array.
    // Compute only: SotC's vs 0x9d6f9bf4 matches too (shadow map tables) and hung the GPU with it
    const u32 array_size = info.hw_stage == HwStage::Compute ? DynamicTsharpArraySize() : 0;
    auto table =
        array_size != 0 && !is_written ? MatchSharpTable(resource.sharps[0]) : std::nullopt;
    if (table && NumArrayDescriptors(info) + array_size + 1 > NUM_IMAGE_ARRAY_DESCRIPTORS) {
        // The binder has room for NUM_IMAGE_ARRAY_DESCRIPTORS; this sharp keeps the default path
        LOG_WARNING(Render_Recompiler,
                    "Workaround dynamic_tsharp_array_size: shader {:#x} has no room for another "
                    "array of {} T#s",
                    info.pgm_hash, array_size);
        table.reset();
    }
    const auto table_fetch =
        table ? AddSharpTable(info, *table, resource.sharps[0].num_dwords, array_size)
              : std::nullopt;
    ImageResource image_res = {
        .sharp_fetch = table_fetch ? *table_fetch
                                   : ConstructSharpFetch<AmdGpu::Image>(resource.sharps[0]),
        .is_depth = bool(inst_info.is_depth),
        .is_atomic = is_atomic,
        .is_array = bool(inst_info.is_array),
        .is_written = is_written,
        .is_r128 = bool(inst_info.is_r128),
        .post_op = resource.sharps[0].post_op,
        .array_size = static_cast<u16>(table_fetch ? array_size : 0),
        .array_stride_dw = static_cast<u16>(table_fetch ? table->stride_dw : 0),
    };

    auto image = image_res.GetSharp(info);
    ASSERT(image.GetType() != AmdGpu::ImageType::Invalid);

    if (needs_mip_storage_fallback) {
        // If the mip level to IMAGE_(LOAD/STORE)_MIP is a constant, set up ImageResource
        // so that we will only bind a single level.
        // If index is dynamic, we will bind levels as an array
        const auto view_type = image.GetViewType(image_res.is_array);

        IR::Inst* body = inst.Arg(1).Inst();
        const auto lod_arg = [&] -> IR::Value {
            switch (view_type) {
            case AmdGpu::ImageType::Color1D: // x, [lod]
                return body->Arg(1);
            case AmdGpu::ImageType::Color1DArray: // x, slice, [lod]
            case AmdGpu::ImageType::Color2D:      // x, y, [lod]
                return body->Arg(2);
            case AmdGpu::ImageType::Color2DArray: // x, y, slice, [lod]
            case AmdGpu::ImageType::Cube:         // x, y, face, [lod]
            case AmdGpu::ImageType::Color3D:      // x, y, z, [lod]
                return body->Arg(3);
            case AmdGpu::ImageType::Color2DMsaa:
            case AmdGpu::ImageType::Color2DMsaaArray:
            default:
                UNREACHABLE_MSG("Invalid image type {}", view_type);
            }
        }();

        if (lod_arg.IsImmediate()) {
            image_res.mip_fallback_mode = MipStorageFallbackMode::ConstantIndex;
            image_res.constant_mip_index = lod_arg.U32();
        } else {
            image_res.mip_fallback_mode = MipStorageFallbackMode::DynamicIndex;
        }
    }

    // Patch image instruction if image is FMask.
    if (AmdGpu::IsFmask(image.GetDataFmt())) {
        ASSERT_MSG(!is_written, "FMask storage instructions are not supported");

        IR::IREmitter ir{*inst.GetParent(), IR::Block::InstructionList::s_iterator_to(inst)};
        switch (inst.GetOpcode()) {
        case IR::Opcode::ImageRead:
        case IR::Opcode::ImageSampleRaw: {
            // The read becomes a constant, but its descriptor type remains a specialization
            // dependency. Otherwise a later ordinary texture at the same T# reuses this
            // resource-free variant forever and its real image/sampler bindings disappear.
            descriptors.Add(FMaskResource{
                .sharp_idx = SharpLocationFromSource(resource.sharps[0].dwords[0].Inst()),
            });
            IR::F32 fmaskx = ir.BitCast<IR::F32>(ir.Imm32(0x76543210));
            IR::F32 fmasky = ir.BitCast<IR::F32>(ir.Imm32(0xfedcba98));
            inst.ReplaceUsesWith(ir.CompositeConstruct(fmaskx, fmasky));
            return;
        }
        case IR::Opcode::ImageQueryLod:
            inst.ReplaceUsesWith(ir.Imm32(1));
            return;
        case IR::Opcode::ImageQueryDimensions: {
            IR::Value dims = ir.CompositeConstruct(ir.Imm32(static_cast<u32>(image.width)),  // x
                                                   ir.Imm32(static_cast<u32>(image.height)), // y
                                                   ir.Imm32(1), ir.Imm32(1)); // depth, mip
            inst.ReplaceUsesWith(dims);

            // Track FMask resource to do specialization.
            descriptors.Add(FMaskResource{
                .sharp_idx = SharpLocationFromSource(resource.sharps[0].dwords[0].Inst()),
            });
            return;
        }
        default:
            UNREACHABLE_MSG("Can't patch fmask instruction {}", inst.GetOpcode());
        }
    }

    u32 image_binding = descriptors.Add(image_res);

    IR::IREmitter ir{*inst.GetParent(), IR::Block::InstructionList::s_iterator_to(inst)};

    u32 bindings = image_binding;
    if (inst.GetOpcode() == IR::Opcode::ImageSampleRaw) {
        auto& lod_prod = resource.sharps[1].post_op_data.lod_prod;
        const u32 sampler_binding = descriptors.Add(SamplerResource{
            .sharp_fetch = ConstructSharpFetch<AmdGpu::Sampler>(resource.sharps[1]),
            .post_op = resource.sharps[1].post_op,
            .post_op_tsharp_dw3_off =
                lod_prod.IsEmpty() ? UNKNOWN_LOCATION : SharpLocationFromSource(lod_prod.Inst()),
            .is_depth = bool(inst_info.is_depth), // true for the _C (compare) opcodes
        });
        bindings |= sampler_binding << 16;
    }
    if (image_res.array_size != 0) {
        // Elements past the table hit the extra null binding at index array_size
        const IR::U32 element = ir.UMin(IR::U32{table->element}, ir.Imm32(u32{array_size}));
        inst.SetArg(0, ir.ImageArrayHandle(ir.Imm32(bindings), element));
    } else {
        inst.SetArg(0, ir.Imm32(bindings));
    }
}

static IR::Value IsMbcntWithExec(IR::Value value, bool is_hi, IR::U1 exec) {
    if (value.IsImmediate()) {
        return {};
    }
    const IR::Inst* inst = value.Inst();
    if (inst->GetOpcode() != IR::Opcode::MaskedBitCount32 || inst->Arg(0).IsImmediate() ||
        inst->Arg(2).U1() != is_hi) {
        return {};
    }
    IR::Inst* prod = inst->Arg(0).Inst();
    if (prod->GetOpcode() != IR::Opcode::CompositeExtractU32x2 || prod->Arg(0).IsImmediate() ||
        prod->Arg(1).U32() != is_hi) {
        return {};
    }
    prod = prod->Arg(0).Inst();
    if (prod->GetOpcode() != IR::Opcode::UnpackUint2x32 || prod->Arg(0).IsImmediate()) {
        return {};
    }
    prod = prod->Arg(0).Inst();
    if (prod->GetOpcode() != IR::Opcode::Ballot || (prod->Arg(0) != exec && !is_hi)) {
        return {};
    }
    return inst->Arg(1);
}

static IR::Use FindUniqueUser(IR::Inst* inst, auto&& pred) {
    IR::Use picked{};
    for (auto& use : inst->Uses()) {
        if (pred(use.user)) {
            ASSERT(!picked.user);
            picked = use;
        }
    }
    ASSERT(picked.user);
    return picked;
}

static void RemoveAppendBufferLaneOffset(IR::Inst& vx) {
    // Attempt to detect either of the following patterns and
    // remove the lane id addition/subtraction to ds instruction.
    //
    // ds_append       vX gds
    // v_mbcnt_hi_u32_b32 vY, exec_hi, vX
    // v_mbcnt_lo_u32_b32 idx, exec_lo, vY
    //
    // v_mbcnt_hi_u32_b32 vY, exec_hi, 0
    // v_mbcnt_lo_u32_b32 vZ, exec_lo, vY
    // ds_append       vX gds
    // v_add_i32       idx, vcc, vX, vZ
    //
    // ds_consume      vX gds
    // v_mbcnt_hi_u32_b32 vY, exec_hi, 0
    // v_mbcnt_lo_u32_b32 vZ, exec_lo, vY
    // v_sub_i32       vK, vcc, vX, vZ
    // v_subrev_i32    idx, vcc, 1, vK / v_add_i32    idx, vcc, -1, vK
    //
    // ds_consume      vX gds
    // v_mbcnt_hi_u32_b32 vY, exec_hi, 0
    // v_mbcnt_lo_u32_b32 vZ, exec_lo, vY
    // v_add_i32       vK, vcc, -1, vX
    // v_sub_i32       idx, vcc, vK, vZ

    IR::U1 exec{vx.Arg(1)};
    const auto uses = vx.Uses();
    for (auto use : uses) {
        const auto& [user, operand] = use;
        if (user->GetOpcode() == IR::Opcode::MaskedBitCount32) {
            // First pattern
            ASSERT(vx.GetOpcode() == IR::Opcode::DataAppend);
            IR::Inst* vy = user;
            auto vx_2 = IsMbcntWithExec(IR::Value{vy}, true, exec);
            ASSERT(!vx_2.IsEmpty() && vx_2 == IR::Value{&vx});
            const auto [idx, operand] = FindUniqueUser(vy, [](const IR::Inst* inst) {
                return inst->GetOpcode() == IR::Opcode::MaskedBitCount32;
            });
            auto vy_2 = IsMbcntWithExec(IR::Value{idx}, false, exec);
            ASSERT(!vy_2.IsEmpty() && vy_2 == IR::Value{vy});
            idx->ReplaceUsesWithAndRemove(IR::Value{&vx});
            continue;
        }
        if (user->GetOpcode() != IR::Opcode::IAdd32 && user->GetOpcode() != IR::Opcode::ISub32) {
            continue;
        }
        const auto other = user->Arg(1 - operand);
        if (other.IsImmediate()) {
            ASSERT_MSG(other.U32() == 1u || other.U32() == std::numeric_limits<u32>::max() &&
                                                vx.GetOpcode() == IR::Opcode::DataConsume,
                       "Unexpected constant offset {} to DataConsume result", other.U32());
            use = FindUniqueUser(user, [](const IR::Inst* inst) {
                return inst->GetOpcode() == IR::Opcode::IAdd32 ||
                       inst->GetOpcode() == IR::Opcode::ISub32;
            });
        }

        // Other patterns
        auto vz = user->Arg(1 - operand);
        auto vy = IsMbcntWithExec(vz, false, exec);
        ASSERT(!vy.IsEmpty() && !vy.IsImmediate());
        auto zero_const = IsMbcntWithExec(vy, true, exec);
        ASSERT(!zero_const.IsEmpty() && zero_const.IsImmediate() && zero_const.U32() == 0u);
        user->SetArg(1 - operand, IR::Value{u32{0u}});
    }
}

void PatchGlobalDataShareAccess(IR::Inst& inst, Info& info, Descriptors& descriptors,
                                const Profile& profile) {
    const u32 binding = descriptors.Add(BufferResource{
        .used_types = IR::Type::U32,
        .buffer_type = BufferType::GdsBuffer,
        .is_written = true,
    });

    IR::IREmitter ir{*inst.GetParent(), IR::Block::InstructionList::s_iterator_to(inst)};

    if (inst.GetOpcode() == IR::Opcode::DataAppend || inst.GetOpcode() == IR::Opcode::DataConsume) {
        RemoveAppendBufferLaneOffset(inst);
        const IR::Value replacement =
            inst.GetOpcode() == IR::Opcode::DataAppend
                ? ir.BufferAtomicIAdd(ir.Imm32(binding), inst.Arg(0), ir.Imm32(1u), {})
                : ir.BufferAtomicISub(ir.Imm32(binding), inst.Arg(0), ir.Imm32(1u), {});
        inst.ReplaceUsesWithAndRemove(replacement);
        return;
    }

    // Convert shared memory opcode to storage buffer atomic to GDS buffer.
    auto& buffer = info.buffers[binding];
    const IR::U32 offset = IR::U32{inst.Arg(0)};
    const IR::U32 address_words = ir.ShiftRightLogical(offset, ir.Imm32(1));
    const IR::U32 address_dwords = ir.ShiftRightLogical(offset, ir.Imm32(2));
    const IR::U32 address_qwords = ir.ShiftRightLogical(offset, ir.Imm32(3));
    const IR::U32 handle = ir.Imm32(binding);
    switch (inst.GetOpcode()) {
    case IR::Opcode::SharedAtomicIAdd32:
        inst.ReplaceUsesWith(ir.BufferAtomicIAdd(handle, address_dwords, inst.Arg(1), {}));
        break;
    case IR::Opcode::SharedAtomicIAdd64:
        inst.ReplaceUsesWith(ir.BufferAtomicIAdd(handle, address_qwords, IR::U64{inst.Arg(1)}, {}));
        break;
    case IR::Opcode::SharedAtomicISub32:
        inst.ReplaceUsesWith(ir.BufferAtomicISub(handle, address_dwords, inst.Arg(1), {}));
        break;
    case IR::Opcode::SharedAtomicSMin32:
    case IR::Opcode::SharedAtomicUMin32: {
        const bool is_signed = inst.GetOpcode() == IR::Opcode::SharedAtomicSMin32;
        inst.ReplaceUsesWith(
            ir.BufferAtomicIMin(handle, address_dwords, inst.Arg(1), is_signed, {}));
        break;
    }
    case IR::Opcode::SharedAtomicSMax32:
    case IR::Opcode::SharedAtomicUMax32: {
        const bool is_signed = inst.GetOpcode() == IR::Opcode::SharedAtomicSMax32;
        inst.ReplaceUsesWith(
            ir.BufferAtomicIMax(handle, address_dwords, inst.Arg(1), is_signed, {}));
        break;
    }
    case IR::Opcode::SharedAtomicInc32:
        inst.ReplaceUsesWith(ir.BufferAtomicInc(handle, address_dwords, {}));
        break;
    case IR::Opcode::SharedAtomicDec32:
        inst.ReplaceUsesWith(ir.BufferAtomicDec(handle, address_dwords, {}));
        break;
    case IR::Opcode::SharedAtomicAnd32:
        inst.ReplaceUsesWith(ir.BufferAtomicAnd(handle, address_dwords, inst.Arg(1), {}));
        break;
    case IR::Opcode::SharedAtomicOr32:
        inst.ReplaceUsesWith(ir.BufferAtomicOr(handle, address_dwords, inst.Arg(1), {}));
        break;
    case IR::Opcode::SharedAtomicXor32:
        inst.ReplaceUsesWith(ir.BufferAtomicXor(handle, address_dwords, inst.Arg(1), {}));
        break;
    case IR::Opcode::SharedAtomicCmpSwap32:
        // Args are (address, value, cmp_value)
        inst.ReplaceUsesWith(
            ir.BufferAtomicCmpSwap(handle, address_dwords, inst.Arg(1), inst.Arg(2), {}));
        break;
    case IR::Opcode::LoadSharedU16: {
        inst.ReplaceUsesWith(ir.LoadBufferU16(handle, address_words, {}));
        buffer.used_types |= IR::Type::U16;
        break;
    }
    case IR::Opcode::LoadSharedU32:
        inst.ReplaceUsesWith(ir.LoadBufferU32(1, handle, address_dwords, {}));
        break;
    case IR::Opcode::LoadSharedU64: {
        inst.ReplaceUsesWith(ir.LoadBufferU64(handle, address_qwords, {}));
        buffer.used_types |= IR::Type::U64;
        break;
    }
    case IR::Opcode::WriteSharedU16: {
        ir.StoreBufferU16(handle, address_words, IR::U16{inst.Arg(1)}, {});
        inst.Invalidate();
        buffer.used_types |= IR::Type::U16;
        break;
    }
    case IR::Opcode::WriteSharedU32:
        ir.StoreBufferU32(1, handle, address_dwords, inst.Arg(1), {});
        inst.Invalidate();
        break;
    case IR::Opcode::WriteSharedU64: {
        ir.StoreBufferU64(handle, address_qwords, IR::U64{inst.Arg(1)}, {});
        inst.Invalidate();
        buffer.used_types |= IR::Type::U64;
        break;
    }
    default:
        UNREACHABLE_MSG("Unexpected opcode {}", inst.GetOpcode());
    }
}

IR::U32 CalculateBufferAddress(IR::IREmitter& ir, const IR::Inst& inst, const Info& info,
                               const AmdGpu::Buffer& buffer, u32 stride) {
    const auto inst_info = inst.Flags<IR::BufferInstInfo>();
    const u32 inst_offset = inst_info.inst_offset.Value();
    const auto is_inst_typed = inst_info.inst_data_fmt != AmdGpu::DataFormat::FormatInvalid;
    const auto data_format = is_inst_typed
                                 ? AmdGpu::RemapDataFormat(inst_info.inst_data_fmt.Value())
                                 : buffer.GetDataFmt();
    const u32 shift = BufferAddressShift(inst, data_format);
    const u32 mask = (1 << shift) - 1;
    const IR::U32 soffset = IR::GetBufferSOffsetArg(&inst);

    // If address calculation is of the form "index * const_stride + offset" with offset constant
    // and both const_stride and offset are divisible with the element size, apply shift directly.
    if (inst_info.index_enable && !inst_info.voffset_enable && soffset.IsImmediate() &&
        !buffer.swizzle_enable && !buffer.add_tid_enable && (stride & mask) == 0) {
        const u32 total_offset = soffset.U32() + inst_offset;
        if ((total_offset & mask) == 0) {
            // buffer_offset = index * (const_stride >> shift) + (offset >> shift)
            const IR::U32 index = IR::GetBufferIndexArg(&inst);
            return ir.IAdd(ir.IMul(index, ir.Imm32(stride >> shift)),
                           ir.Imm32(total_offset >> shift));
        }
    }

    // index = (inst_idxen ? vgpr_index : 0) + (const_add_tid_enable ? thread_id[5:0] : 0)
    IR::U32 index = ir.Imm32(0U);
    if (inst_info.index_enable) {
        const IR::U32 vgpr_index = IR::GetBufferIndexArg(&inst);
        index = ir.IAdd(index, vgpr_index);
    }
    if (buffer.add_tid_enable) {
        // thread_id[5:0] is the lane index within the wave. SubgroupLocalInvocationId provides it
        // in every stage (flat outside vertex), so graphics stages can use the same lowering as
        // compute instead of aborting.
        if (info.sw_stage != SwStage::Compute) {
            static bool warned = false;
            if (!std::exchange(warned, true)) {
                LOG_WARNING(Render_Recompiler,
                            "Thread ID buffer addressing in non-compute stage {}, using lane id",
                            u32(info.sw_stage));
            }
        }
        const IR::U32 thread_id{ir.LaneId()};
        index = ir.IAdd(index, thread_id);
    }
    // offset = (inst_offen ? vgpr_offset : 0) + inst_offset
    IR::U32 offset = ir.Imm32(inst_offset);
    offset = ir.IAdd(offset, soffset);
    if (inst_info.voffset_enable) {
        const IR::U32 voffset = IR::GetBufferVOffsetArg(&inst);
        offset = ir.IAdd(offset, voffset);
    }
    const IR::U32 const_stride = ir.Imm32(stride);
    IR::U32 buffer_offset;
    if (buffer.swizzle_enable) {
        const IR::U32 const_index_stride = ir.Imm32(buffer.GetIndexStride());
        const IR::U32 const_element_size = ir.Imm32(buffer.GetElementSize());
        // index_msb = index / const_index_stride
        const IR::U32 index_msb{ir.IDiv(index, const_index_stride)};
        // index_lsb = index % const_index_stride
        const IR::U32 index_lsb{ir.IMod(index, const_index_stride)};
        // offset_msb = offset / const_element_size
        const IR::U32 offset_msb{ir.IDiv(offset, const_element_size)};
        // offset_lsb = offset % const_element_size
        const IR::U32 offset_lsb{ir.IMod(offset, const_element_size)};
        // buffer_offset =
        //     (index_msb * const_stride + offset_msb * const_element_size) * const_index_stride
        //     + index_lsb * const_element_size + offset_lsb
        const IR::U32 buffer_offset_msb = ir.IMul(
            ir.IAdd(ir.IMul(index_msb, const_stride), ir.IMul(offset_msb, const_element_size)),
            const_index_stride);
        const IR::U32 buffer_offset_lsb =
            ir.IAdd(ir.IMul(index_lsb, const_element_size), offset_lsb);
        buffer_offset = ir.IAdd(buffer_offset_msb, buffer_offset_lsb);
    } else {
        // buffer_offset = index * const_stride + offset
        buffer_offset = ir.IAdd(ir.IMul(index, const_stride), offset);
    }
    if (shift != 0) {
        buffer_offset = ir.ShiftRightLogical(buffer_offset, ir.Imm32(shift));
    }
    return buffer_offset;
}

void PatchBufferArgs(IR::Inst& inst, Info& info) {
    const auto handle = inst.Arg(0);
    const auto buffer_res = info.buffers[handle.U32()];
    const auto buffer = buffer_res.GetSharp(info);

    // Address of constant buffer reads can be calculated at IR emission time.
    if (inst.GetOpcode() == IR::Opcode::ReadConstBuffer) {
        return;
    }

    IR::IREmitter ir{*inst.GetParent(), IR::Block::InstructionList::s_iterator_to(inst)};
    inst.SetArg(IR::LoadBufferArgs::Address,
                CalculateBufferAddress(ir, inst, info, buffer, buffer.stride));
}

void PatchImageSampleArgs(IR::Inst& inst, Info& info, const ImageResource& image_res,
                          const AmdGpu::Image& image) {
    const auto handle = inst.Arg(0);
    const auto& sampler_res = info.samplers[(ImageHandleBindings(handle) >> 16) & 0xFFFF];
    const auto sampler = sampler_res.GetSharp(info);

    IR::IREmitter ir{*inst.GetParent(), IR::Block::InstructionList::s_iterator_to(inst)};
    const auto inst_info = inst.Flags<IR::TextureInstInfo>();
    const auto view_type = image.GetViewType(image_res.is_array);

    IR::Inst* body1 = inst.Arg(2).Inst();
    IR::Inst* body2 = inst.Arg(3).Inst();
    IR::Inst* body3 = inst.Arg(4).Inst();
    IR::F32 body4 = IR::F32{inst.Arg(5)};
    const auto get_addr_reg = [&](u32 index) -> IR::F32 {
        if (index <= 3) {
            return IR::F32{body1->Arg(index)};
        }
        if (index >= 4 && index <= 7) {
            return IR::F32{body2->Arg(index - 4)};
        }
        if (index >= 8 && index <= 11) {
            return IR::F32{body3->Arg(index - 8)};
        }
        if (index == 12) {
            return body4;
        }
        UNREACHABLE();
    };
    u32 addr_reg = 0;

    // Load first address components as denoted in 8.2.4 VGPR Usage Sea Islands Series Instruction
    // Set Architecture
    const IR::Value offset = [&] -> IR::Value {
        if (!inst_info.has_offset) {
            return IR::U32{};
        }

        // The offsets are six-bit signed integers: X=[5:0], Y=[13:8], and Z=[21:16].
        IR::Value arg = get_addr_reg(addr_reg++);
        if (const IR::Inst* offset_inst = arg.TryInst()) {
            ASSERT(offset_inst->GetOpcode() == IR::Opcode::BitCastF32U32);
            arg = offset_inst->Arg(0);
        }

        const auto read = [&](u32 off) -> IR::U32 {
            if (arg.IsImmediate()) {
                const u32 imm =
                    arg.Type() == IR::Type::F32 ? std::bit_cast<u32>(arg.F32()) : arg.U32();
                const u16 comp = (imm >> off) & 0x3F;
                return ir.Imm32(s32(comp << 26) >> 26);
            }
            return ir.BitFieldExtract(IR::U32{arg}, ir.Imm32(off), ir.Imm32(6), true);
        };

        switch (view_type) {
        case AmdGpu::ImageType::Color1D:
        case AmdGpu::ImageType::Color1DArray:
            return read(0);
        case AmdGpu::ImageType::Color2D:
        case AmdGpu::ImageType::Color2DMsaa:
        case AmdGpu::ImageType::Color2DArray:
        case AmdGpu::ImageType::Cube:
            return ir.CompositeConstruct(read(0), read(8));
        case AmdGpu::ImageType::Color3D:
            return ir.CompositeConstruct(read(0), read(8), read(16));
        default:
            UNREACHABLE();
        }
    }();
    const IR::F32 bias = inst_info.has_bias ? get_addr_reg(addr_reg++) : IR::F32{};
    const IR::F32 dref = inst_info.is_depth ? get_addr_reg(addr_reg++) : IR::F32{};
    const auto [derivatives_dx, derivatives_dy] = [&] -> std::pair<IR::Value, IR::Value> {
        if (!inst_info.has_derivatives) {
            return {};
        }
        switch (view_type) {
        case AmdGpu::ImageType::Color1D:
        case AmdGpu::ImageType::Color1DArray:
            // du/dx, du/dy
            addr_reg = addr_reg + 2;
            return {get_addr_reg(addr_reg - 2), get_addr_reg(addr_reg - 1)};
        case AmdGpu::ImageType::Color2D:
        case AmdGpu::ImageType::Color2DMsaa:
        case AmdGpu::ImageType::Color2DArray:
        case AmdGpu::ImageType::Cube:
            // (du/dx, dv/dx), (du/dy, dv/dy)
            addr_reg = addr_reg + 4;
            return {ir.CompositeConstruct(get_addr_reg(addr_reg - 4), get_addr_reg(addr_reg - 3)),
                    ir.CompositeConstruct(get_addr_reg(addr_reg - 2), get_addr_reg(addr_reg - 1))};
        case AmdGpu::ImageType::Color3D:
            // (du/dx, dv/dx, dw/dx), (du/dy, dv/dy, dw/dy)
            addr_reg = addr_reg + 6;
            return {ir.CompositeConstruct(get_addr_reg(addr_reg - 6), get_addr_reg(addr_reg - 5),
                                          get_addr_reg(addr_reg - 4)),
                    ir.CompositeConstruct(get_addr_reg(addr_reg - 3), get_addr_reg(addr_reg - 2),
                                          get_addr_reg(addr_reg - 1))};
        default:
            UNREACHABLE();
        }
    }();

    const bool is_msaa = view_type == AmdGpu::ImageType::Color2DMsaa ||
                         view_type == AmdGpu::ImageType::Color2DMsaaArray;
    const bool unnormalized = sampler.force_unnormalized || inst_info.is_unnormalized;
    const bool needs_dimentions = (!is_msaa && unnormalized) || (is_msaa && !unnormalized);
    const auto dimensions =
        needs_dimentions ? ir.ImageQueryDimension(handle, ir.Imm32(0u), ir.Imm1(false), inst_info)
                         : IR::Value{};
    const auto get_coord = [&](u32 coord_idx, u32 dim_idx) -> IR::Value {
        const auto coord = get_addr_reg(coord_idx);
        if (is_msaa) {
            // For MSAA images preserve the unnormalized coord or manually unnormalize it
            if (unnormalized) {
                return ir.ConvertFToU(32, coord);
            } else {
                const auto dim =
                    ir.ConvertUToF(32, 32, IR::U32{ir.CompositeExtract(dimensions, dim_idx)});
                return ir.ConvertFToU(32, ir.FPMul(coord, dim));
            }
        }
        if (unnormalized) {
            // Normalize the coordinate for sampling, dividing by its corresponding dimension.
            const auto dim =
                ir.ConvertUToF(32, 32, IR::U32{ir.CompositeExtract(dimensions, dim_idx)});
            return ir.FPDiv(coord, dim);
        }
        return coord;
    };

    // Now we can load body components as noted in Table 8.9 Image Opcodes with Sampler
    const IR::Value coords = [&] -> IR::Value {
        switch (view_type) {
        case AmdGpu::ImageType::Color1D: // x
            addr_reg = addr_reg + 1;
            return get_coord(addr_reg - 1, 0);
        case AmdGpu::ImageType::Color1DArray: // x, slice
        case AmdGpu::ImageType::Color2D:      // x, y
        case AmdGpu::ImageType::Color2DMsaa:  // x, y
            addr_reg = addr_reg + 2;
            return ir.CompositeConstruct(get_coord(addr_reg - 2, 0), get_coord(addr_reg - 1, 1));
        case AmdGpu::ImageType::Color2DArray: // x, y, slice
        case AmdGpu::ImageType::Cube:         // x, y, face
        case AmdGpu::ImageType::Color3D:      // x, y, z
            addr_reg = addr_reg + 3;
            return ir.CompositeConstruct(get_coord(addr_reg - 3, 0), get_coord(addr_reg - 2, 1),
                                         get_coord(addr_reg - 1, 2));
        default:
            UNREACHABLE();
        }
    }();

    ASSERT(!inst_info.has_lod || !inst_info.has_lod_clamp);
    const bool explicit_lod = inst_info.has_lod || inst_info.force_level0;
    const IR::F32 lod = inst_info.has_lod        ? get_addr_reg(addr_reg++)
                        : inst_info.force_level0 ? ir.Imm32(0.0f)
                                                 : IR::F32{};
    const IR::F32 lod_clamp = inst_info.has_lod_clamp ? get_addr_reg(addr_reg++) : IR::F32{};

    // sotc: the LOD feedback buffer is declared from the instruction, not from what the bound
    // texture made of it. A multisampled T# turns the sample into a read, so a permutation
    // translated with one had no implicit LOD sample and no feedback buffer, while the next
    // permutation had both: every permutation shares the first one's pipeline layout, and the
    // second was created against a layout one binding short (colossus 4 and 9, fs 0xea0a2a97:
    // VUID-VkGraphicsPipelineCreateInfo-layout-07988/07990, then draws with each texture one
    // slot off). The info collection pass sets the same flag for the samples that remain.
    if (!inst_info.is_gather && !inst_info.has_derivatives && !explicit_lod &&
        info.sw_stage == SwStage::Fragment && LodFeedbackEnabled()) {
        info.uses_lod_feedback = true;
    }

    auto texel = [&] -> IR::Value {
        if (is_msaa) {
            return ir.ImageRead(handle, coords, {}, ir.Imm32(0U), inst_info);
        }
        if (inst_info.is_gather) {
            if (inst_info.is_depth) {
                return ir.ImageGatherDref(handle, coords, offset, dref, inst_info);
            }
            return ir.ImageGather(handle, coords, offset, inst_info);
        }
        if (inst_info.has_derivatives) {
            return ir.ImageGradient(handle, coords, derivatives_dx, derivatives_dy, offset,
                                    lod_clamp, inst_info);
        }
        if (inst_info.is_depth) {
            if (explicit_lod) {
                return ir.ImageSampleDrefExplicitLod(handle, coords, dref, lod, offset, inst_info);
            }
            return ir.ImageSampleDrefImplicitLod(handle, coords, dref, bias, offset, inst_info);
        }
        if (explicit_lod) {
            return ir.ImageSampleExplicitLod(handle, coords, lod, offset, inst_info);
        }
        return ir.ImageSampleImplicitLod(handle, coords, bias, offset, inst_info);
    }();

    auto converted = ApplyReadNumberConversionVec4(ir, texel, image.GetNumberConversion());
    if (sampler.force_degamma && image.GetNumberFmt() != AmdGpu::NumberFormat::Srgb) {
        converted = ApplyForceDegamma(ir, texel);
    }
    inst.ReplaceUsesWith(converted);
}

void PatchImageArgs(IR::Inst& inst, Info& info) {
    // Nothing to patch for dimension query.
    if (inst.GetOpcode() == IR::Opcode::ImageQueryDimensions) {
        return;
    }

    const auto image_handle = inst.Arg(0);
    const auto binding_index = ImageHandleBindings(image_handle) & 0xFFFF;
    const auto& image_res = info.images[binding_index];
    auto image = image_res.GetSharp(info);

    // Sample instructions must be handled separately using address register data.
    if (inst.GetOpcode() == IR::Opcode::ImageSampleRaw) {
        return PatchImageSampleArgs(inst, info, image_res, image);
    }

    IR::IREmitter ir{*inst.GetParent(), IR::Block::InstructionList::s_iterator_to(inst)};
    auto inst_info = inst.Flags<IR::TextureInstInfo>();
    const auto view_type = image.GetViewType(image_res.is_array);

    // Now that we know the image type, adjust texture coordinate vector.
    IR::Inst* body = inst.Arg(1).Inst();
    const auto [coords, arg] = [&] -> std::pair<IR::Value, IR::Value> {
        switch (view_type) {
        case AmdGpu::ImageType::Color1D: // x, [lod]
            return {body->Arg(0), body->Arg(1)};
        case AmdGpu::ImageType::Color1DArray: // x, slice, [lod]
        case AmdGpu::ImageType::Color2D:      // x, y, [lod]
            return {ir.CompositeConstruct(body->Arg(0), body->Arg(1)), body->Arg(2)};
        case AmdGpu::ImageType::Color2DMsaa: // x, y. (sample is passed on different argument)
        {
            auto skip_lod = false;
            if (inst_info.has_lod) {
                const auto mipid = body->Arg(2);
                if (mipid.IsImmediate() && mipid.U32() == 0) {
                    // if image_x_mip refers to a MSAA image, and mipid is 0, it is safe to be
                    // skipped and fragid is taken from the next arg
                    LOG_WARNING(Render_Recompiler, "Encountered a _mip instruction with MSAA "
                                                   "image, and mipid is 0, skipping LoD");
                    inst_info.has_lod.Assign(false);
                    skip_lod = true;
                } else {
                    UNREACHABLE_MSG(
                        "Encountered a _mip instruction with MSAA image, and mipid is non-zero");
                }
            }
            return {ir.CompositeConstruct(body->Arg(0), body->Arg(1)), body->Arg(skip_lod ? 3 : 2)};
        }
        case AmdGpu::ImageType::Color2DArray:     // x, y, slice, [lod]
        case AmdGpu::ImageType::Color2DMsaaArray: // x, y, slice. (sample is passed on different
                                                  // argument)
        case AmdGpu::ImageType::Cube:             // x, y, face, [lod]
        case AmdGpu::ImageType::Color3D:          // x, y, z, [lod]
            return {ir.CompositeConstruct(body->Arg(0), body->Arg(1), body->Arg(2)), body->Arg(3)};
        default:
            UNREACHABLE_MSG("Unknown image type {}", view_type);
        }
    }();

    const auto has_ms = view_type == AmdGpu::ImageType::Color2DMsaa ||
                        view_type == AmdGpu::ImageType::Color2DMsaaArray;
    ASSERT(!inst_info.has_lod || !has_ms);
    // If we are binding a single mip level as fallback, drop the argument
    const auto lod =
        (inst_info.has_lod && image_res.mip_fallback_mode != MipStorageFallbackMode::ConstantIndex)
            ? IR::U32{arg}
            : IR::U32{};
    const auto ms = has_ms ? IR::U32{arg} : IR::U32{};

    const auto is_storage = image_res.is_written;
    if (inst.GetOpcode() == IR::Opcode::ImageRead) {
        auto texel = ir.ImageRead(image_handle, coords, lod, ms, inst_info);
        if (is_storage) {
            // Storage image requires shader swizzle.
            texel = ApplySwizzle(ir, texel, image.DstSelect());
        }
        const auto converted =
            ApplyReadNumberConversionVec4(ir, texel, image.GetNumberConversion());
        inst.ReplaceUsesWith(converted);
    } else {
        inst.SetArg(1, coords);
        if (inst.GetOpcode() == IR::Opcode::ImageWrite) {
            inst.SetArg(2, lod);
            inst.SetArg(3, ms);

            auto texel = inst.Arg(4);
            if (is_storage) {
                // Storage image requires shader swizzle.
                texel = ApplySwizzle(ir, texel, image.DstSelect().Inverse());
            }
            const auto converted =
                ApplyWriteNumberConversionVec4(ir, texel, image.GetNumberConversion());
            inst.SetArg(4, converted);
        }
    }
}

void ResourcePatchingPass(Shader::Info& info, const ResourceDiscoveryList& resources,
                          const Profile& profile) {
    // Iterate over discovered resources and patch them after finding the sharp.
    // Pass 1: Track resource sharps
    Descriptors descriptors{info};
    for (const auto& usage : resources) {
        IR::Inst& inst = *usage.user;
        if (IsBufferInstruction(inst)) {
            PatchBufferSharp(usage, info, descriptors, profile);
        } else if (IsImageInstruction(inst)) {
            PatchImageSharp(usage, info, descriptors, profile);
        }
    }

    // Pass 2: Patch instruction args
    for (const auto& usage : resources) {
        IR::Inst& inst = *usage.user;
        if (IsBufferInstruction(inst)) {
            PatchBufferArgs(inst, info);
        } else if (IsImageInstruction(inst)) {
            PatchImageArgs(inst, info);
        } else if (IsDataRingInstruction(inst)) {
            PatchGlobalDataShareAccess(inst, info, descriptors, profile);
        }
    }
}

} // namespace Shader::Optimization
