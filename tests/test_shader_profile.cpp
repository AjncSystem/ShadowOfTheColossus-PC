#include "shader_recompiler/profile.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <new>
#include <vector>

namespace {

void CheckRoundTrip(const Shader::Profile& p) {
    const auto bytes = Shader::SerializeProfile(p);
    assert(bytes.size() == Shader::kProfileSerializedSize);
    assert(Shader::Detail::ReadU32(bytes, 0) == Shader::kProfileMagic);
    assert(Shader::Detail::ReadU32(bytes, 4) == Shader::kProfileVersion);

    Shader::Profile out{};
    bool legacy = true;
    assert(Shader::DeserializeProfile(bytes, out, &legacy));
    assert(!legacy);
    assert(out == p);
}

} // namespace

int main() {
    // 1. Default-initialized Profile round-trip.
    {
        const Shader::Profile def{};
        CheckRoundTrip(def);
    }

    // 2. Every u32 field and all 39 bool fields individually toggled and round-tripped.
    {
        Shader::Profile p{};
        p.max_viewport_width = 32768;
        p.max_viewport_height = 16384;
        p.max_shared_memory_size = 49152;
        p.supported_spirv = 0x00010600;
        p.subgroup_size = 64;
        p.sparse_page_shift = 16;
        CheckRoundTrip(p);

        bool Shader::Profile::* const bool_fields[] = {
            &Shader::Profile::support_int8,
            &Shader::Profile::support_int16,
            &Shader::Profile::support_int64,
            &Shader::Profile::support_float16,
            &Shader::Profile::support_float64,
            &Shader::Profile::supports_denorm_behavior_independence,
            &Shader::Profile::supports_rounding_mode_independence,
            &Shader::Profile::support_fp16_denorm_preserve,
            &Shader::Profile::support_fp16_denorm_flush,
            &Shader::Profile::support_fp16_round_to_zero,
            &Shader::Profile::support_fp32_denorm_preserve,
            &Shader::Profile::support_fp32_denorm_flush,
            &Shader::Profile::support_fp32_round_to_zero,
            &Shader::Profile::support_fp64_denorm_preserve,
            &Shader::Profile::support_fp64_denorm_flush,
            &Shader::Profile::support_fp64_round_to_zero,
            &Shader::Profile::support_fp16_signed_zero_inf_nan_preserve,
            &Shader::Profile::support_fp32_signed_zero_inf_nan_preserve,
            &Shader::Profile::support_fp64_signed_zero_inf_nan_preserve,
            &Shader::Profile::supports_image_load_store_lod,
            &Shader::Profile::supports_native_cube_calc,
            &Shader::Profile::supports_trinary_minmax,
            &Shader::Profile::supports_buffer_fp32_atomic_min_max,
            &Shader::Profile::supports_image_fp32_atomic_min_max,
            &Shader::Profile::supports_buffer_int64_atomics,
            &Shader::Profile::supports_shared_int64_atomics,
            &Shader::Profile::supports_workgroup_explicit_memory_layout,
            &Shader::Profile::supports_amd_shader_explicit_vertex_parameter,
            &Shader::Profile::supports_fragment_shader_barycentric,
            &Shader::Profile::supports_shader_subgroup_clock,
            &Shader::Profile::has_broken_spirv_clamp,
            &Shader::Profile::lower_left_origin_mode,
            &Shader::Profile::needs_manual_interpolation,
            &Shader::Profile::needs_lds_barriers,
            &Shader::Profile::needs_buffer_offsets,
            &Shader::Profile::needs_unorm_fixup,
            &Shader::Profile::needs_clip_distance_emulation,
            &Shader::Profile::supports_shader_stencil_export,
            &Shader::Profile::barycentric_via_gs,
        };
        static_assert(sizeof(bool_fields) / sizeof(bool_fields[0]) == 39);
        for (auto member : bool_fields) {
            Shader::Profile single{};
            single.*member = true;
            CheckRoundTrip(single);
        }
    }

    // 3. Padding independence: placement new over 0xAA vs 0x00 produces identical bytes.
    {
        alignas(Shader::Profile) u8 buf_a[sizeof(Shader::Profile)];
        alignas(Shader::Profile) u8 buf_b[sizeof(Shader::Profile)];
        std::memset(buf_a, 0x00, sizeof(buf_a));
        std::memset(buf_b, 0xAA, sizeof(buf_b));
        auto* pa = new (buf_a) Shader::Profile{};
        auto* pb = new (buf_b) Shader::Profile{};
        pa->max_viewport_width = 16384;
        pb->max_viewport_width = 16384;
        pa->needs_lds_barriers = true;
        pb->needs_lds_barriers = true;
        assert(Shader::SerializeProfile(*pa) == Shader::SerializeProfile(*pb));
    }

    // 4. Legacy raw struct compatibility (barycentric_via_gs padding byte 0xe3 -> false, 1 -> true).
    {
        Shader::Profile expected{};
        expected.max_viewport_width = 32768;
        expected.needs_manual_interpolation = true;
        std::vector<u8> raw(sizeof(Shader::Profile));
        std::memcpy(raw.data(), &expected, sizeof(expected));
        raw[offsetof(Shader::Profile, barycentric_via_gs)] = 0xe3;

        Shader::Profile out{};
        bool legacy = false;
        assert(Shader::DeserializeProfile(raw, out, &legacy));
        assert(legacy);
        assert(!out.barycentric_via_gs);
        assert(out == expected);

        raw[offsetof(Shader::Profile, barycentric_via_gs)] = 1;
        expected.barycentric_via_gs = true;
        assert(Shader::DeserializeProfile(raw, out, &legacy));
        assert(legacy);
        assert(out.barycentric_via_gs);
        assert(out == expected);
    }

    // 5. Invalid payloads rejected.
    {
        Shader::Profile out{};
        assert(!Shader::DeserializeProfile({}, out));
        std::vector<u8> bad_ver = Shader::SerializeProfile(Shader::Profile{});
        bad_ver[4] = 99;
        assert(!Shader::DeserializeProfile(bad_ver, out));
        std::vector<u8> truncated = Shader::SerializeProfile(Shader::Profile{});
        truncated.pop_back();
        assert(!Shader::DeserializeProfile(truncated, out));
        std::vector<u8> bad_size(16, 0);
        assert(!Shader::DeserializeProfile(bad_size, out));
    }

    return 0;
}
