// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/amdgpu/regs_texture.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace VideoCore {

class Sampler {
public:
    explicit Sampler(const Vulkan::Instance& instance, const AmdGpu::Sampler& sampler,
                     const AmdGpu::BorderColorBuffer border_color_base, bool is_depth);
    ~Sampler();

    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    Sampler(Sampler&&) = default;
    Sampler& operator=(Sampler&&) = default;

    vk::Sampler Handle() const noexcept {
        return *handle;
    }

    size_t lru_id{};
    /// sotc: slot in the bindless heap (BindlessSamplers), BindlessSamplers::NoSlot if none.
    u32 bindless_slot{~0u};

private:
    vk::UniqueSampler handle;
};

/// sotc: SOTC_BINDLESS_SAMP: the global sampler heap, one update-after-bind array of samplers in
/// set 1 that pipelines index through their push constants. A sampler takes a slot the first time
/// the texture cache hands it out and keeps it: slots are never freed, and the garbage collector
/// keeps samplers that hold one. Slot 0 is a default sampler, used when the heap is full.
class BindlessSamplers {
public:
    static constexpr u32 Capacity = 4096;
    /// sotc: samplers handed out at most: half of the 4000-sampler device limit of NVIDIA and Intel
    /// drivers, so the classic samplers keep room (heap samplers are never destroyed).
    static constexpr u32 MaxAllocated = 2048;
    /// sotc: slot used when the heap is full: 0 (linear, repeat) or 1 (the same with depth compare).
    static constexpr u32 DefaultSlot(bool is_depth) {
        return is_depth ? 1u : 0u;
    }
    static constexpr u32 NoSlot = ~0u;

    explicit BindlessSamplers(const Vulkan::Instance& instance);
    ~BindlessSamplers();

    BindlessSamplers(const BindlessSamplers&) = delete;
    BindlessSamplers& operator=(const BindlessSamplers&) = delete;

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }
    [[nodiscard]] vk::DescriptorSetLayout Layout() const noexcept {
        return *layout;
    }
    [[nodiscard]] vk::DescriptorSet Set() const noexcept {
        return set;
    }

    /// Writes `sampler` into the next free slot and returns it. NoSlot when the heap is full.
    /// Called with the texture cache's sampler lock held.
    u32 Allocate(vk::Sampler sampler);

private:
    void Write(u32 slot, vk::Sampler sampler);

    const Vulkan::Instance& instance;
    bool enabled{};
    vk::UniqueDescriptorSetLayout layout;
    vk::UniqueDescriptorPool pool;
    vk::UniqueSampler default_sampler;
    vk::UniqueSampler default_compare_sampler;
    vk::DescriptorSet set{};
    u32 next_slot{2};
    bool logged_full{};
};

} // namespace VideoCore
