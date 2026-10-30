// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: SOTC_HOST_IMPORT=1 (off by default). Large buffer uploads are copied by the GPU's copy
// engine straight from guest memory instead of by the CPU into a staging buffer. The guest
// memory (the runtime's second, unprotected view of the whole pool) is imported with
// VK_EXT_external_memory_host in 64 MiB chunks, on demand and up to a budget: the driver locks
// imported pages in RAM, so the whole pool (up to ~10 GiB) is never imported at once. The copy
// goes to device-local staging on the transfer queue and the CPU waits for it, so the bytes are
// taken when the upload is recorded, as with the CPU copy; the staging-to-arena copy stays on the
// graphics queue, ordered as before. A chunk the driver refuses, an unmapped range or a full
// budget sends the upload back to the CPU path.

#pragma once

#include <mutex>
#include <span>
#include <vector>
#include <boost/container/small_vector.hpp>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace VideoCore {

class HostImport {
public:
    explicit HostImport(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler);
    ~HostImport();

    HostImport(const HostImport&) = delete;
    HostImport& operator=(const HostImport&) = delete;

    /// The switch is on, the device supports it and the guest memory view exists.
    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

    /// Uploads smaller than this stay on the CPU path (a submit and its wait cost more there).
    [[nodiscard]] u64 MinBytes() const noexcept {
        return min_bytes;
    }

    /// Copies each copy's guest range (dstOffset = guest address, size) into dst at
    /// dst_base + srcOffset, on the GPU, and waits until done. Returns false, having copied
    /// nothing, when a piece cannot come from imported memory.
    bool Upload(std::span<const vk::BufferCopy> copies, vk::Buffer dst, u64 dst_base);

private:
    static constexpr u64 ChunkSize = u64{64} << 20;

    struct Chunk {
        vk::Buffer buffer{};
        vk::DeviceMemory memory{};
        u64 size{};
        u64 last_use{}; ///< upload counter of its last use (LRU)
        bool failed{};  ///< the driver refused it: never tried again
    };

    /// Looks up the guest memory view (created by the runtime after the GPU library starts).
    bool FindView();
    /// The imported chunk, importing it when needed; nullptr when it cannot be.
    Chunk* GetChunk(u64 index);
    bool Import(Chunk& chunk, u64 index);
    void Release(Chunk& chunk);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    bool enabled{};
    u64 min_bytes{};
    u64 budget_chunks{};
    u64 live_chunks{};
    u64 use_counter{};
    u32 refused{}; ///< chunks the driver refused
    const u8* base{};
    u64 view_size{};
    std::vector<Chunk> chunks;
    std::mutex mutex;
};

} // namespace VideoCore
