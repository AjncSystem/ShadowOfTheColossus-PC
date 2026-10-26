// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"

namespace Vulkan {

class Instance;
class Scheduler;

struct StagingBufferRef {
    VideoCore::Buffer* buffer{};
    u64 offset{};
    u64 size{};
    u8* mapped{};
    VideoCore::MemoryType type{};

    void Flush() const noexcept {
        buffer->Flush(offset, size);
    }

    void Invalidate() const noexcept {
        buffer->Invalidate(offset, size);
    }
};

class StagingBufferPool {
public:
    explicit StagingBufferPool(const Instance& instance, Scheduler& scheduler);
    ~StagingBufferPool();

    StagingBufferPool(const StagingBufferPool&) = delete;
    StagingBufferPool& operator=(const StagingBufferPool&) = delete;

    /// Requests size bytes of memory of the given type.
    [[nodiscard]] StagingBufferRef Request(u64 size, VideoCore::MemoryType type, u64 alignment = 0,
                                           bool deferred = false, bool unsynchronized = false);

    /// Releases a deferred allocation. It becomes reusable once the current tick completes.
    void FreeDeferred(const StagingBufferRef& ref);

    /// Releases idle memory.
    void TickFrame();

private:
    struct Block {
        std::unique_ptr<VideoCore::StreamBuffer> buffer;
        u64 last_used_frame;
    };

    struct Ring {
        std::vector<Block> blocks;
        size_t current = 0;
    };

    struct LargeBuffer {
        std::unique_ptr<VideoCore::Buffer> buffer;
        u64 tick;
        u64 last_used_frame;
        bool held;
        bool pinned = false; ///< sotc: warm reserve (SOTC_STAGING_RESERVE_MB), never trimmed
    };

    static constexpr size_t NUM_TYPES = 3;

    StagingBufferRef RequestFromRing(Ring& ring, u64 size, u64 alignment,
                                     VideoCore::MemoryType type, bool unsynchronized);
    StagingBufferRef RequestLarge(u64 size, VideoCore::MemoryType type, bool deferred,
                                  bool unsynchronized);

    void TrimRing(Ring& ring, size_t keep_blocks, u64 idle_frames);
    void TrimLarge(std::vector<LargeBuffer>& cache, u64 idle_frames);

    void PublishStats() const;

    /// sotc: keeps SOTC_STAGING_RESERVE_MB of host staging allocated and touched in advance, off
    /// the frame (a burst of uploads found no free buffer and paid the allocation plus the page
    /// faults of the first touch, 2-7x the copy time, inside the frame).
    void MaintainReserve();

    std::mutex reserve_mutex;
    std::vector<std::unique_ptr<VideoCore::Buffer>> reserve_ready; ///< made by a copy thread
    std::vector<std::unique_ptr<VideoCore::StreamBuffer>> ring_ready; ///< ring blocks, likewise
    u32 ring_reserve_made = 0;
    std::atomic<u32> reserve_pending{0};
    u32 reserve_made = 0;

    const Instance& instance;
    Scheduler& scheduler;

    std::array<Ring, NUM_TYPES> rings;
    std::array<std::vector<LargeBuffer>, NUM_TYPES> large_caches;
    u64 frame = 0;
};

} // namespace Vulkan
