// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>
#include <memory>
#include <span>
#include <vector>
#include <boost/container/static_vector.hpp>
#include <tsl/robin_map.h>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Semaphore;
class Scheduler;
class DescriptorBufferRing;

/**
 * Handles a pool of resources protected by fences. Manages resource overflow allocating more
 * resources.
 */
class ResourcePool {
public:
    explicit ResourcePool() = default;
    explicit ResourcePool(Semaphore* work_semaphore, std::size_t grow_step);
    virtual ~ResourcePool() = default;

    ResourcePool& operator=(ResourcePool&&) noexcept = default;
    ResourcePool(ResourcePool&&) noexcept = default;

    ResourcePool& operator=(const ResourcePool&) = default;
    ResourcePool(const ResourcePool&) = default;

protected:
    std::size_t CommitResource();

    /// Called when a chunk of resources have to be allocated.
    virtual void Allocate(std::size_t begin, std::size_t end) = 0;

private:
    /// Manages pool overflow allocating new resources.
    std::size_t ManageOverflow();

protected:
    Semaphore* work_semaphore{};
    std::size_t grow_step = 0;     ///< Number of new resources created after an overflow
    std::size_t hint_iterator = 0; ///< Hint to where the next free resources is likely to be found
    std::vector<u64> ticks;        ///< Ticks for each resource
};

class CommandPool final : public ResourcePool {
public:
    /// sotc: level is ePrimary for the command processor's pool, eSecondary for the pass secondaries
    explicit CommandPool(const Instance& instance, Semaphore* work_semaphore,
                         vk::CommandBufferLevel level = vk::CommandBufferLevel::ePrimary);
    ~CommandPool() override;

    void Allocate(std::size_t begin, std::size_t end) override;

    vk::CommandBuffer Commit();

    /// sotc: keeps cmd out of Commit until Release gives it a tick. Not thread safe: the caller locks.
    void Hold(vk::CommandBuffer cmd);
    /// sotc: cmd may be committed again once the GPU reaches tick. Not thread safe: the caller locks.
    void Release(vk::CommandBuffer cmd, u64 tick);

private:
    [[nodiscard]] std::size_t IndexOf(vk::CommandBuffer cmd) const;

    const Instance& instance;
    vk::CommandBufferLevel level;
    vk::UniqueCommandPool cmd_pool;
    std::vector<vk::CommandBuffer> cmd_buffers;
};

class DescriptorHeap final {
    static constexpr u32 DescriptorSetBatch = 32;

public:
    explicit DescriptorHeap(const Instance& instance, Scheduler& scheduler,
                            std::span<const vk::DescriptorPoolSize> pool_sizes,
                            u32 descriptor_heap_count = 1024);
    ~DescriptorHeap();

    vk::DescriptorSet Commit(vk::DescriptorSetLayout set_layout);

    /// sotc: the descriptor buffer ring when SOTC_DESC_BUFFER=1 (vk_descriptor_buffer.h), else null.
    [[nodiscard]] DescriptorBufferRing* GetDescriptorBuffer() const noexcept {
        return descriptor_buffer.get();
    }

    /// sotc: SOTC_BINDLESS_SAMP: the global sampler heap (set 1, texture_cache/sampler.h). Set once
    /// by the rasterizer; the pipelines that use bindless samplers bind it.
    void SetBindlessSamplers(vk::DescriptorSetLayout layout, vk::DescriptorSet set) noexcept {
        bindless_layout = layout;
        bindless_set = set;
    }
    [[nodiscard]] vk::DescriptorSetLayout BindlessSamplerLayout() const noexcept {
        return bindless_layout;
    }
    [[nodiscard]] vk::DescriptorSet BindlessSamplerSet() const noexcept {
        return bindless_set;
    }

private:
    void CreateDescriptorPool();

private:
    vk::Device device;
    Semaphore* work_semaphore;
    u32 descriptor_heap_count;
    std::span<const vk::DescriptorPoolSize> pool_sizes;
    vk::DescriptorPool curr_pool;
    std::deque<std::pair<vk::DescriptorPool, u64>> pending_pools;
    using DescSetBatch = boost::container::static_vector<vk::DescriptorSet, DescriptorSetBatch>;
    tsl::robin_map<u64, DescSetBatch> descriptor_sets;
    std::unique_ptr<DescriptorBufferRing> descriptor_buffer; // sotc: SOTC_DESC_BUFFER=1 only
    vk::DescriptorSetLayout bindless_layout{}; // sotc: SOTC_BINDLESS_SAMP, null otherwise
    vk::DescriptorSet bindless_set{};
};

} // namespace Vulkan
