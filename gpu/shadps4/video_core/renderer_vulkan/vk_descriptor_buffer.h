// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: VK_EXT_descriptor_buffer support, enabled by SOTC_DESC_BUFFER=1. The host writes each
// draw's descriptors straight into a ring of host-visible memory (vkGetDescriptorEXT) instead of
// updating pooled descriptor sets, and the command buffer reads them from that memory.

#pragma once

#include <deque>
#include <memory>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;

/// Size of the descriptor memory ring. A frame of Shadow of the Colossus draws needs a few MB.
inline constexpr u64 DescriptorBufferBytes = 128ULL << 20;

/// Ring allocator over one buffer that holds descriptor data. Regions are released once the GPU
/// has passed the tick they were allocated in, so the ring wraps without a per-draw wait.
class DescriptorBufferRing {
public:
    struct Allocation {
        u8* mapped{};         ///< Host pointer to the region.
        vk::DeviceSize offset{}; ///< Offset of the region inside the buffer.
    };

    explicit DescriptorBufferRing(const Instance& instance, Scheduler& scheduler);
    ~DescriptorBufferRing();

    DescriptorBufferRing(const DescriptorBufferRing&) = delete;
    DescriptorBufferRing& operator=(const DescriptorBufferRing&) = delete;

    /// Reserves size bytes at an offset that is a multiple of alignment (a power of two).
    [[nodiscard]] Allocation Allocate(vk::DeviceSize size, vk::DeviceSize alignment);

    /// Makes host writes into [offset, offset + size) visible to the GPU. No-op when coherent.
    void Flush(vk::DeviceSize offset, vk::DeviceSize size) const;

    /// Device address of the buffer start, for vkCmdBindDescriptorBuffersEXT.
    [[nodiscard]] vk::DeviceAddress Address() const noexcept {
        return address;
    }

    /// True the first time it is asked in a session: the buffer must be bound in that session's
    /// command buffer before its offsets are set. Sessions are the scheduler's command buffers.
    [[nodiscard]] bool NeedsBinding(u64 session) noexcept {
        if (bound_session == session) {
            return false;
        }
        bound_session = session;
        return true;
    }

private:
    struct Memory;
    /// The GPU may read a region until the tick it was allocated in has completed.
    struct Mark {
        u64 tick;
        u64 end; ///< Logical end of the region (monotonic, not wrapped).
    };

    Scheduler& scheduler;
    std::unique_ptr<Memory> memory;
    u8* mapped{};
    vk::DeviceAddress address{};
    bool coherent{};
    // Logical positions: physical offset = position % DescriptorBufferBytes.
    u64 head{};
    u64 tail{};
    std::deque<Mark> marks;
    u64 bound_session{};
};

} // namespace Vulkan
