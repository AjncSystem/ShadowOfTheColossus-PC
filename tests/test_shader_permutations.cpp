// SPDX-License-Identifier: GPL-2.0-or-later
// Resource layouts must follow their shader variant, including after cache insertion/reallocation.
#include <cassert>
#include <cstdint>
#include <memory>
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

static vk::ShaderModule Handle(uintptr_t n) {
    return vk::ShaderModule{reinterpret_cast<VkShaderModule>(n)};
}

static Shader::StageSpecialization Spec(const Shader::Info& info) {
    Shader::StageSpecialization s;
    s.info = &info;
    s.runtime_info.Initialize(Shader::HwStage::Compute, Shader::SwStage::Compute);
    return s;
}

int main() {
    Vulkan::Program p;
    p.info.images.resize(1);
    p.AddPermut(Handle(1), Spec(p.info));
    assert(&p.ModuleInfo(0) == &p.info);
    auto second = std::make_unique<Shader::Info>();
    second->images.resize(3);
    second->samplers.resize(2);
    const auto* stable = second.get();
    auto spec = Spec(*second);
    p.AddPermut(Handle(2), std::move(spec), std::move(second));
    assert(&p.ModuleInfo(1) == stable);
    assert(p.modules[1].spec.info == stable);
    assert(p.ModuleInfo(0).images.size() == 1);
    assert(p.ModuleInfo(1).images.size() == 3);
    assert(p.ModuleInfo(1).samplers.size() == 2);
    for (uintptr_t i = 3; i < 32; ++i) {
        auto info = std::make_unique<Shader::Info>();
        auto next_spec = Spec(*info);
        p.AddPermut(Handle(i), std::move(next_spec), std::move(info));
    }
    assert(&p.ModuleInfo(1) == stable && p.modules[1].spec.info == stable);

    Vulkan::Program cached;
    auto layout = std::make_unique<Shader::Info>();
    layout->images.resize(5);
    auto stored = Spec(*layout);
    const auto* cache_stable = layout.get();
    cached.InsertPermut(Handle(8), std::move(stored), 4, std::move(layout));
    assert(cached.modules.size() == 5 && !cached.modules[0].spec.Valid());
    assert(&cached.ModuleInfo(4) == cache_stable);
    assert(cached.modules[4].spec.info == cache_stable);

    Shader::Info info;
    auto a = Spec(info), b = Spec(info);
    assert(a == b);
    a.buffers.resize(2); b.buffers.resize(1);
    assert(!(a == b) && !(b == a));
    b.buffers.resize(2); a.images.resize(2); b.images.resize(1);
    assert(!(a == b) && !(b == a));
    b.images.resize(2); a.samplers.resize(2); b.samplers.resize(1);
    assert(!(a == b) && !(b == a));
    assert(!(Spec(info) == Shader::StageSpecialization{}));

    // Same-size surfaces can change between an FMask constant and a real image.
    auto mask = Spec(info), texture = Spec(info);
    mask.fmasks.resize(1); texture.fmasks.resize(1);
    mask.fmasks[0].width = texture.fmasks[0].width = 256;
    mask.fmasks[0].height = texture.fmasks[0].height = 256;
    mask.fmasks[0].is_fmask = true;
    assert(!(mask == texture) && !(texture == mask));
    auto image_a = Spec(info), image_b = Spec(info);
    image_a.images.resize(1); image_b.images.resize(1);
    image_a.bitset[0] = image_b.bitset[0] = true;
    image_a.images[0].is_fmask = true;
    assert(!(image_a == image_b) && !(image_b == image_a));
}
