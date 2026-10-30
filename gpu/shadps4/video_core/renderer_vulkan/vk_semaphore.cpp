// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"
#include "video_core/sotc_stats.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <immintrin.h>

#include "common/assert.h"
#include "video_core/sotc_stats.h"

#include <vk_mem_alloc.h>

namespace Vulkan {

u32 SotcWaitSpinUs() {
    static const u32 value = [] {
        const char* v = std::getenv("SOTC_WAIT_SPIN_US");
        return v ? static_cast<u32>(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    return value;
}

constexpr u64 WAIT_TIMEOUT = std::numeric_limits<u64>::max();
constexpr u32 MarkerStride = 64;
constexpr u32 MarkerSlots = 2;

Semaphore::Semaphore(const Instance& instance_) : instance{instance_} {
    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [semaphore_result, sem] =
        instance.GetDevice().createSemaphoreUnique(semaphore_chain.get());
    ASSERT_MSG(semaphore_result == vk::Result::eSuccess, "Failed to create master semaphore: {}",
               vk::to_string(semaphore_result));
    semaphore = std::move(sem);

    // sotc: SOTC_WAIT_MARKER=1 polls a GPU-written marker before asking the driver, 2 also lets
    // host reads (readbacks) return on it. SOTC_WAIT_SPIN_US bounds the polling (default 4 ms).
    const char* mode = std::getenv("SOTC_WAIT_MARKER");
    marker_mode = mode ? static_cast<u32>(std::strtoul(mode, nullptr, 10)) : 0u;
    if (marker_mode == 0) {
        return;
    }
    marker_spin_us = std::getenv("SOTC_WAIT_SPIN_US") ? SotcWaitSpinUs() : 4000u;
    const VkBufferCreateInfo buffer_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = MarkerStride * MarkerSlots,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    // Cached host memory: polling reads the CPU cache until the GPU write arrives.
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        .preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
    };
    VkBuffer buffer{};
    VmaAllocationInfo alloc_info{};
    const VkResult result = vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci,
                                            &buffer, &marker_allocation, &alloc_info);
    if (result != VK_SUCCESS || !alloc_info.pMappedData) {
        LOG_WARNING(Render_Vulkan, "SOTC_WAIT_MARKER: no host marker buffer");
        if (result == VK_SUCCESS) {
            vmaDestroyBuffer(instance.GetAllocator(), buffer, marker_allocation);
        }
        marker_allocation = {};
        return;
    }
    marker_buffer = vk::Buffer{buffer};
    std::memset(alloc_info.pMappedData, 0, MarkerStride * MarkerSlots);
    marker_data = static_cast<const volatile u32*>(alloc_info.pMappedData);
    LOG_INFO(Render_Vulkan, "SOTC_WAIT_MARKER {}: polling up to {} us", marker_mode,
             marker_spin_us);
}

Semaphore::~Semaphore() {
    if (marker_buffer) {
        vmaDestroyBuffer(instance.GetAllocator(), static_cast<VkBuffer>(marker_buffer),
                         marker_allocation);
    }
}

void Semaphore::RecordMarker(vk::CommandBuffer cmdbuf, MarkerSlot slot, u32 value) const {
    // Everything before, made visible to the host, then the marker itself.
    const vk::MemoryBarrier2 pre_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eClear | vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eHostRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &pre_barrier,
    });
    cmdbuf.fillBuffer(marker_buffer, static_cast<u32>(slot) * MarkerStride, sizeof(u32), value);
    const vk::MemoryBarrier2 post_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eClear,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &post_barrier,
    });
}

bool Semaphore::MarkerReached(MarkerSlot slot, u64 value) const noexcept {
    const u32 current = marker_data[static_cast<u32>(slot) * MarkerStride / sizeof(u32)];
    std::atomic_thread_fence(std::memory_order_acquire);
    return static_cast<s32>(current - static_cast<u32>(value)) >= 0;
}

bool Semaphore::SpinMarker(MarkerSlot slot, u64 value) const {
    if (!marker_buffer) {
        return false;
    }
    if (MarkerReached(slot, value)) {
        return true;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::microseconds(marker_spin_us);
    do {
        for (int i = 0; i < 64; ++i) {
            _mm_pause();
            if (MarkerReached(slot, value)) {
                return true;
            }
        }
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

void Semaphore::WaitHostRead(u64 tick) {
    if (!TrustMarker() || IsFree(tick) || tick >= CurrentTick()) {
        Wait(tick);
        return;
    }
    {
        const SotcStats::DrawWait wait{SotcStats::WaitGpu}; // sotc: SOTC_FPS accounting
        if (SpinMarker(MarkerSlot::Timeline, tick)) {
            return;
        }
    }
    Wait(tick);
}

void Semaphore::Refresh() {
    u64 this_tick{};
    u64 counter{};
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
        auto [counter_result, cntr] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
        ASSERT_MSG(counter_result == vk::Result::eSuccess,
                   "Failed to get master semaphore value: {}", vk::to_string(counter_result));
        counter = cntr;
        if (counter < this_tick) {
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));
}

void Semaphore::Wait(u64 tick) {
    // No need to wait if the GPU is ahead of the tick
    if (IsFree(tick)) {
        return;
    }
    // Update the GPU tick and try again
    Refresh();
    if (IsFree(tick)) {
        return;
    }
    const SotcStats::DrawWait gpu_wait{SotcStats::WaitGpu}; // sotc: SOTC_FPS accounting
    const SotcStats::SplitTimer gpu_split{SotcStats::SplitWaitGpu};

    // sotc: the marker comes first; the driver reports the semaphore right after it.
    if (marker_buffer && tick < CurrentTick() && SpinMarker(MarkerSlot::Timeline, tick)) {
        for (int i = 0; i < 256; ++i) {
            Refresh();
            if (IsFree(tick)) {
                return;
            }
            _mm_pause();
        }
    }

    // sotc (wait_spin_us of Pink-shadPS4): poll for a moment before sleeping; waking from a
    // blocking wait costs more than the short readback waits of Shadow of the Colossus.
    if (const u32 spin_us = SotcWaitSpinUs(); spin_us != 0) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(spin_us);
        do {
            _mm_pause();
            Refresh();
            if (IsFree(tick)) {
                return;
            }
        } while (std::chrono::steady_clock::now() < deadline);
    }

    // If none of the above is hit, fallback to a regular wait
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &semaphore.get(),
        .pValues = &tick,
    };

    for (u32 waited = 0;; ++waited) {
        // sotc: report a wait longer than 5 s (hang diagnosis), then keep waiting.
        const vk::Result result =
            instance.GetDevice().waitSemaphores(&wait_info, 5'000'000'000ULL);
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            const auto [counter_result, counter] =
                instance.GetDevice().getSemaphoreCounterValue(*semaphore);
            std::fprintf(stderr,
                         "SOTC GPU wait: tick %llu not reached after %us; semaphore at %llu, "
                         "current tick %llu\n",
                         static_cast<unsigned long long>(tick), (waited + 1) * 5,
                         static_cast<unsigned long long>(counter),
                         static_cast<unsigned long long>(CurrentTick()));
            continue;
        }
        if (result == vk::Result::eErrorDeviceLost) {
            // Spinning here would freeze the emulator forever after a GPU hang/reset.
            instance.ReportDeviceFault();
            UNREACHABLE_MSG("Device lost while waiting for GPU tick {}", tick);
        }
    }
    Refresh();
}

} // namespace Vulkan
