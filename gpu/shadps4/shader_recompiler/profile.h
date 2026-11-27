// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include "common/types.h"

namespace Shader {

struct Profile {
    u32 max_viewport_width{};
    u32 max_viewport_height{};
    u32 max_shared_memory_size{};
    u32 supported_spirv{0x00010000};
    u32 subgroup_size{};
    u32 sparse_page_shift{};
    bool support_int8{};
    bool support_int16{};
    bool support_int64{};
    bool support_float16{};
    bool support_float64{};
    bool supports_denorm_behavior_independence{};
    bool supports_rounding_mode_independence{};
    bool support_fp16_denorm_preserve{};
    bool support_fp16_denorm_flush{};
    bool support_fp16_round_to_zero{};
    bool support_fp32_denorm_preserve{};
    bool support_fp32_denorm_flush{};
    bool support_fp32_round_to_zero{};
    bool support_fp64_denorm_preserve{};
    bool support_fp64_denorm_flush{};
    bool support_fp64_round_to_zero{};
    bool support_fp16_signed_zero_inf_nan_preserve{};
    bool support_fp32_signed_zero_inf_nan_preserve{};
    bool support_fp64_signed_zero_inf_nan_preserve{};
    bool supports_image_load_store_lod{};
    bool supports_native_cube_calc{};
    bool supports_trinary_minmax{};
    bool supports_buffer_fp32_atomic_min_max{};
    bool supports_image_fp32_atomic_min_max{};
    bool supports_buffer_int64_atomics{};
    bool supports_shared_int64_atomics{};
    bool supports_workgroup_explicit_memory_layout{};
    bool supports_amd_shader_explicit_vertex_parameter{};
    bool supports_fragment_shader_barycentric{};
    bool supports_shader_subgroup_clock{};
    bool has_broken_spirv_clamp{};
    bool lower_left_origin_mode{};
    bool needs_manual_interpolation{};
    bool needs_lds_barriers{};
    bool needs_buffer_offsets{};
    bool needs_unorm_fixup{};
    bool needs_clip_distance_emulation{};
    bool supports_shader_stencil_export{};
    /// sotc: supports_fragment_shader_barycentric emulated by an auxiliary geometry shader
    /// (EmitBarycentricGeometryShader): per-vertex inputs and barycentrics become plain inputs.
    /// Last, so the profiles stored in older caches still compare equal.
    bool barycentric_via_gs{};

    bool operator==(const Profile&) const = default;
};

inline constexpr u32 kProfileMagic = 0x50524F46u;
inline constexpr u32 kProfileVersion = 1u;
inline constexpr size_t kProfileSerializedSize = 8 + 6 * sizeof(u32) + sizeof(u64);

namespace Detail {
inline void AppendU32(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v & 0xFFu));
    out.push_back(static_cast<u8>((v >> 8) & 0xFFu));
    out.push_back(static_cast<u8>((v >> 16) & 0xFFu));
    out.push_back(static_cast<u8>((v >> 24) & 0xFFu));
}

inline void AppendU64(std::vector<u8>& out, u64 v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<u8>((v >> (i * 8)) & 0xFFu));
    }
}

inline u32 ReadU32(std::span<const u8> in, size_t offset) {
    return static_cast<u32>(in[offset]) | (static_cast<u32>(in[offset + 1]) << 8) |
           (static_cast<u32>(in[offset + 2]) << 16) | (static_cast<u32>(in[offset + 3]) << 24);
}

inline u64 ReadU64(std::span<const u8> in, size_t offset) {
    u64 v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<u64>(in[offset + i]) << (i * 8);
    }
    return v;
}
} // namespace Detail

inline std::vector<u8> SerializeProfile(const Profile& p) {
    std::vector<u8> out;
    out.reserve(kProfileSerializedSize);
    Detail::AppendU32(out, kProfileMagic);
    Detail::AppendU32(out, kProfileVersion);
    Detail::AppendU32(out, p.max_viewport_width);
    Detail::AppendU32(out, p.max_viewport_height);
    Detail::AppendU32(out, p.max_shared_memory_size);
    Detail::AppendU32(out, p.supported_spirv);
    Detail::AppendU32(out, p.subgroup_size);
    Detail::AppendU32(out, p.sparse_page_shift);

    u64 flags = 0;
    int bit = 0;
    const auto pack = [&](bool v) {
        if (v) {
            flags |= u64{1} << bit;
        }
        ++bit;
    };
    pack(p.support_int8);
    pack(p.support_int16);
    pack(p.support_int64);
    pack(p.support_float16);
    pack(p.support_float64);
    pack(p.supports_denorm_behavior_independence);
    pack(p.supports_rounding_mode_independence);
    pack(p.support_fp16_denorm_preserve);
    pack(p.support_fp16_denorm_flush);
    pack(p.support_fp16_round_to_zero);
    pack(p.support_fp32_denorm_preserve);
    pack(p.support_fp32_denorm_flush);
    pack(p.support_fp32_round_to_zero);
    pack(p.support_fp64_denorm_preserve);
    pack(p.support_fp64_denorm_flush);
    pack(p.support_fp64_round_to_zero);
    pack(p.support_fp16_signed_zero_inf_nan_preserve);
    pack(p.support_fp32_signed_zero_inf_nan_preserve);
    pack(p.support_fp64_signed_zero_inf_nan_preserve);
    pack(p.supports_image_load_store_lod);
    pack(p.supports_native_cube_calc);
    pack(p.supports_trinary_minmax);
    pack(p.supports_buffer_fp32_atomic_min_max);
    pack(p.supports_image_fp32_atomic_min_max);
    pack(p.supports_buffer_int64_atomics);
    pack(p.supports_shared_int64_atomics);
    pack(p.supports_workgroup_explicit_memory_layout);
    pack(p.supports_amd_shader_explicit_vertex_parameter);
    pack(p.supports_fragment_shader_barycentric);
    pack(p.supports_shader_subgroup_clock);
    pack(p.has_broken_spirv_clamp);
    pack(p.lower_left_origin_mode);
    pack(p.needs_manual_interpolation);
    pack(p.needs_lds_barriers);
    pack(p.needs_buffer_offsets);
    pack(p.needs_unorm_fixup);
    pack(p.needs_clip_distance_emulation);
    pack(p.supports_shader_stencil_export);
    pack(p.barycentric_via_gs);

    Detail::AppendU64(out, flags);
    return out;
}

inline bool DeserializeProfile(std::span<const u8> data, Profile& out,
                               bool* upgraded_from_legacy = nullptr) {
    out = Profile{};
    if (upgraded_from_legacy) {
        *upgraded_from_legacy = false;
    }
    if (data.size() >= 8 && Detail::ReadU32(data, 0) == kProfileMagic) {
        if (Detail::ReadU32(data, 4) != kProfileVersion || data.size() != kProfileSerializedSize) {
            return false;
        }
        out.max_viewport_width = Detail::ReadU32(data, 8);
        out.max_viewport_height = Detail::ReadU32(data, 12);
        out.max_shared_memory_size = Detail::ReadU32(data, 16);
        out.supported_spirv = Detail::ReadU32(data, 20);
        out.subgroup_size = Detail::ReadU32(data, 24);
        out.sparse_page_shift = Detail::ReadU32(data, 28);
        const u64 flags = Detail::ReadU64(data, 32);
        int bit = 0;
        const auto unpack = [&](bool& v) {
            v = ((flags >> bit) & u64{1}) != 0;
            ++bit;
        };
        unpack(out.support_int8);
        unpack(out.support_int16);
        unpack(out.support_int64);
        unpack(out.support_float16);
        unpack(out.support_float64);
        unpack(out.supports_denorm_behavior_independence);
        unpack(out.supports_rounding_mode_independence);
        unpack(out.support_fp16_denorm_preserve);
        unpack(out.support_fp16_denorm_flush);
        unpack(out.support_fp16_round_to_zero);
        unpack(out.support_fp32_denorm_preserve);
        unpack(out.support_fp32_denorm_flush);
        unpack(out.support_fp32_round_to_zero);
        unpack(out.support_fp64_denorm_preserve);
        unpack(out.support_fp64_denorm_flush);
        unpack(out.support_fp64_round_to_zero);
        unpack(out.support_fp16_signed_zero_inf_nan_preserve);
        unpack(out.support_fp32_signed_zero_inf_nan_preserve);
        unpack(out.support_fp64_signed_zero_inf_nan_preserve);
        unpack(out.supports_image_load_store_lod);
        unpack(out.supports_native_cube_calc);
        unpack(out.supports_trinary_minmax);
        unpack(out.supports_buffer_fp32_atomic_min_max);
        unpack(out.supports_image_fp32_atomic_min_max);
        unpack(out.supports_buffer_int64_atomics);
        unpack(out.supports_shared_int64_atomics);
        unpack(out.supports_workgroup_explicit_memory_layout);
        unpack(out.supports_amd_shader_explicit_vertex_parameter);
        unpack(out.supports_fragment_shader_barycentric);
        unpack(out.supports_shader_subgroup_clock);
        unpack(out.has_broken_spirv_clamp);
        unpack(out.lower_left_origin_mode);
        unpack(out.needs_manual_interpolation);
        unpack(out.needs_lds_barriers);
        unpack(out.needs_buffer_offsets);
        unpack(out.needs_unorm_fixup);
        unpack(out.needs_clip_distance_emulation);
        unpack(out.supports_shader_stencil_export);
        unpack(out.barycentric_via_gs);
        return true;
    }
    if (data.size() == sizeof(Profile)) {
        std::vector<u8> legacy(data.begin(), data.end());
        auto& via_gs = legacy[offsetof(Profile, barycentric_via_gs)];
        via_gs = via_gs == 1 ? 1 : 0;
        std::memcpy(&out, legacy.data(), sizeof(out));
        if (upgraded_from_legacy) {
            *upgraded_from_legacy = true;
        }
        return true;
    }
    return false;
}

} // namespace Shader
