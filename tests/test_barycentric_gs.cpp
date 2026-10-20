// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: Profile::barycentric_via_gs (GPUs without fragment shader barycentrics, NVIDIA GTX 10xx).
// Emits a pixel shader that reads a per-vertex input and the barycentrics, and the geometry shader
// that feeds it, without a window or the game. Validate both with spirv-val:
//   ninja -C out/gpu barycentric-gs-test && out/gpu/barycentric-gs-test.exe out/bary-gs
//   spirv-val --target-env vulkan1.2 out/bary-gs/fragment.spv && spirv-val --target-env vulkan1.2 out/bary-gs/geometry.spv
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/emit_spirv_quad_rect.h"
#include "common/elf_info.h"
#include "shader_recompiler/ir/ir_emitter.h"

#define CHECK(cond)                                                                                \
    if (!(cond)) {                                                                                 \
        std::fprintf(stderr, "check failed: %s\n", #cond);                                         \
        return 1;                                                                                  \
    }

// The recompiler asks for the game serial (SotC-only paths), as gpu/shim/bbgpu.cpp fills it.
namespace Core {
class Emulator {
public:
    static void SetSerial() {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = "CUSA08809";
    }
};
} // namespace Core

static bool Write(const std::filesystem::path& path, const std::vector<u32>& code) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
    return bool(out);
}

int main(int argc, char** argv) {
    using namespace Shader;
    const std::filesystem::path dir = argc > 1 ? argv[1] : ".";
    std::filesystem::create_directories(dir);
    Core::Emulator::SetSerial();

    Info info{};
    info.hw_stage = HwStage::Fragment;
    info.sw_stage = SwStage::Fragment;
    RuntimeInfo runtime{};
    runtime.Initialize(info.hw_stage, info.sw_stage);
    auto& fs = runtime.hw.fs;
    fs.color_buffers[0].num_format = AmdGpu::NumberFormat::Float;
    fs.clip_distance_emulation = true;
    fs.num_inputs = 4; // two params, a default one, the emulated clip distances
    fs.inputs[0] = {.param_index = 0, .is_default = false, .is_flat = true};  // per-vertex
    fs.inputs[1] = {.param_index = 2, .is_default = false, .is_flat = false}; // interpolated
    fs.inputs[2] = {.param_index = 0, .is_default = true, .is_flat = false};  // default value
    info.fs_interpolation[0].primary = Qualifier::PerVertex;
    info.fs_interpolation[1].primary = Qualifier::Smooth;

    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    const auto out = IR::Attribute::RenderTarget0;
    info.loads.Set(IR::Attribute::Param0, 0);
    info.loads.Set(IR::Attribute::Param1, 0);
    info.loads.Set(IR::Attribute::BaryCoordSmooth, 0);
    info.loads.Set(IR::Attribute::BaryCoordNoPersp, 0);
    ir.SetAttribute(out, ir.GetAttribute(IR::Attribute::Param0, 0, 2), 0);
    ir.SetAttribute(out, ir.GetAttribute(IR::Attribute::BaryCoordSmooth, 0), 1);
    ir.SetAttribute(out, ir.GetAttribute(IR::Attribute::Param1, 0), 2);
    ir.SetAttribute(out, ir.GetAttribute(IR::Attribute::BaryCoordNoPersp, 1), 3);
    for (u32 component = 0; component < 4; ++component) {
        info.stores.Set(out, component);
    }
    ir.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.syntax_list.push_back(
        {.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});

    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = true;
    profile.supports_fragment_shader_barycentric = true;
    profile.barycentric_via_gs = true;

    // Locations: the clip distances take 0, the params bind at param_index + 1 (1 and 3), so the
    // per-vertex values start at 4 and the barycentrics follow them.
    const auto layout = Backend::SPIRV::MakeBarycentricGsLayout(fs, info);
    CHECK(layout.base == 4 && layout.per_vertex_mask == 1);
    CHECK(layout.Location(0) == 4 && layout.Smooth() == 7 && layout.NoPerspective() == 8);
    CHECK(Backend::SPIRV::NeedsBarycentricGs(fs, info));

    Backend::Bindings bindings{};
    CHECK(Write(dir / "fragment.spv", Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings)));
    CHECK(Write(dir / "geometry.spv", Backend::SPIRV::EmitBarycentricGeometryShader(fs, info)));
    return 0;
}
