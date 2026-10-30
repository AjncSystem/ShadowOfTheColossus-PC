// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/types.h"
#include "core/devtools/widget/gpu_memory.h"
#include "video_core/buffer_cache/async_upload.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/sotc_stats.h"

namespace Vulkan {

using VideoCore::MemoryType;

namespace {

constexpr u64 RING_IDLE_FRAMES = 300;
constexpr u64 LARGE_IDLE_FRAMES = 120;
constexpr u64 LARGE_MIN_GRANULARITY = 64_KB;
// sotc: ring blocks of SOTC_STAGING_BLOCK_MB (default 16 as upstream). An allocation costs about
// the same at any size (~2 ms), and a streaming burst asks for 200+ MB in one frame.
u64 BlockSize() {
    static const u64 size = [] {
        const char* v = std::getenv("SOTC_STAGING_BLOCK_MB");
        const u64 mb = v ? std::strtoull(v, nullptr, 10) : 16;
        return std::max<u64>(mb, 1) << 20;
    }();
    return size;
}

// sotc: SOTC_STAGING_LARGE_POW2 (on by default since 30 Oct 2026; =0 turns it off). Large uploads (streaming bursts) come in many
// sizes: matched within 1.25x they seldom found a free buffer and allocated a new one inside the
// frame that needed it (6 allocations, 240 MiB, 7-19 ms per 5 s while an area loads). With the
// switch they get power-of-two size classes and reuse any free buffer up to twice the request,
// and idle ones are kept up to SOTC_STAGING_LARGE_KEEP_MB (default 256) per memory type instead
// of being freed after 120 idle frames.
static bool StagingLargePow2() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_STAGING_LARGE_POW2");
        return !v || v[0] != '0';
    }();
    return enabled;
}
static u64 StagingLargeKeepBytes() {
    static const u64 bytes = [] {
        const char* v = std::getenv("SOTC_STAGING_LARGE_KEEP_MB");
        return (v ? std::strtoull(v, nullptr, 10) : 256ULL) << 20;
    }();
    return bytes;
}

u64 RoundAllocationSize(u64 size) {
    const u64 step = std::max(std::bit_floor(size) / 4, LARGE_MIN_GRANULARITY);
    return Common::AlignUp(size, step);
}

} // Anonymous namespace

StagingBufferPool::StagingBufferPool(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {}

StagingBufferPool::~StagingBufferPool() = default;

StagingBufferRef StagingBufferPool::Request(u64 size, MemoryType type, u64 alignment, bool deferred,
                                            bool unsynchronized) {
    Ring& ring = rings[u32(type)];
    if (deferred || size > BlockSize()) {
        return RequestLarge(size, type, deferred, unsynchronized);
    }
    return RequestFromRing(ring, size, alignment, type, unsynchronized);
}

MemoryType StagingBufferPool::AsyncCopyType() const {
    // sotc: SOTC_ASYNC_STAGE_CACHED=1 (AsyncUpload::StageCached). Without a host cached memory
    // type the copies keep the write-combined HostUncached.
    if (!VideoCore::AsyncUpload::StageCached()) {
        return MemoryType::HostUncached;
    }
    // sotc: with SOTC_REBAR_UPLOAD the HostUncached staging moves to the BAR heap (written by the
    // CPU, read by the GPU from its own memory); a cached type would silently leave it in system
    // RAM. The BAR placement wins: the switch is ignored then (one log line).
    if (instance.RebarUploadEnabled()) {
        static bool reported = false;
        if (!std::exchange(reported, true)) {
            std::fputs("SOTC_ASYNC_STAGE_CACHED ignored: SOTC_REBAR_UPLOAD is on", stderr);
            std::fputc('\n', stderr);
        }
        return MemoryType::HostUncached;
    }
    const vk::MemoryPropertyFlags wanted =
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCached;
    const auto& props = instance.GetMemoryProperties();
    for (u32 i = 0; i < props.memoryTypeCount; ++i) {
        if ((props.memoryTypes[i].propertyFlags & wanted) == wanted) {
            return MemoryType::HostCached;
        }
    }
    return MemoryType::HostUncached;
}

MemoryType StagingBufferPool::UploadType() const {
    // sotc: SOTC_STAGING_CACHED=1 (off by default). The staging of the draw thread's buffer syncs,
    // image guest copies and write-throughs. Write-combined memory held one thread's memcpy to
    // 2.6-6 GB/s in the area-load frames (92% of the draw thread's samples inside "sync copy" were
    // the memcpy). Cached system memory takes a plain memcpy at full speed; the GPU reads it over
    // PCIe. Same rules as AsyncCopyType: no cached type, or SOTC_REBAR_UPLOAD on, keeps
    // HostUncached. Resolved once and logged once: the type cannot change while the device lives.
    static const MemoryType type = [this] {
        const char* v = std::getenv("SOTC_STAGING_CACHED");
        if (!v || v[0] != '1') {
            return MemoryType::HostUncached;
        }
        MemoryType t = MemoryType::HostUncached;
        const char* why = "no host cached memory type";
        if (instance.RebarUploadEnabled()) {
            why = "SOTC_REBAR_UPLOAD is on";
        } else {
            const vk::MemoryPropertyFlags wanted =
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCached;
            const auto& props = instance.GetMemoryProperties();
            for (u32 i = 0; i < props.memoryTypeCount; ++i) {
                if ((props.memoryTypes[i].propertyFlags & wanted) == wanted) {
                    t = MemoryType::HostCached;
                    break;
                }
            }
        }
        if (t == MemoryType::HostCached) {
            std::fputs("SOTC_STAGING_CACHED: upload staging is HostCached\n", stderr);
        } else {
            std::fprintf(stderr, "SOTC_STAGING_CACHED ignored: %s\n", why);
        }
        return t;
    }();
    return type;
}

StagingBufferRef StagingBufferPool::RequestFromRing(Ring& ring, u64 size, u64 alignment,
                                                    MemoryType type, bool unsynchronized) {
    Block* best{};
    u64 best_offset{};
    for (size_t i = 0; i < ring.blocks.size(); ++i) {
        const size_t index = (ring.current + i) % ring.blocks.size();
        Block* block = &ring.blocks[index];
        if (const auto offset = block->buffer->Reserve(size, alignment, false)) {
            ring.current = index;
            best = block;
            best_offset = *offset;
            break;
        }
    }

    if (!best) {
        if (!ring.blocks.empty() && unsynchronized) {
            best = &ring.blocks[(ring.current + 1) % ring.blocks.size()];
            best_offset = 0;
        } else {
            const SotcStats::FrameTimer frame_timer{SotcStats::FrameAlloc};
            const SotcStats::AllocTrace alloc_trace{SotcStats::AllocStagingBlock, BlockSize()};
            // sotc: upload blocks move to the large BAR heap under SOTC_REBAR_UPLOAD, per block.
            const MemoryType block_type =
                type == MemoryType::HostUncached
                    ? VideoCore::ResolveRebarType(instance, BlockSize(), type)
                    : type;
            best = &ring.blocks.emplace_back(Block{
                .buffer = std::make_unique<VideoCore::StreamBuffer>(instance, scheduler, block_type,
                                                                    BlockSize()),
                .last_used_frame = frame,
            });
            ring.current = ring.blocks.size() - 1;
            best_offset = *best->buffer->Reserve(size, alignment, false);
        }
    }

    auto& buffer = *best->buffer;
    best->last_used_frame = frame;

    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = best_offset,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data() + best_offset;
    }
    return ref;
}

StagingBufferRef StagingBufferPool::RequestLarge(u64 size, MemoryType type, bool deferred,
                                                 bool unsynchronized) {
    auto& cache = large_caches[u32(type)];
    // sotc: deferred requests (asynchronous readbacks, one per guest read of GPU-written memory)
    // come in slightly different sizes; matched within 1.25x they rarely found a free buffer and
    // allocated a new one ~1,100 times in a 6-minute attract run. They get power-of-two sizes
    // instead, and reuse any free buffer of that size class.
    const bool pow2 = deferred || StagingLargePow2();
    const u64 alloc_size =
        pow2 ? std::bit_ceil(std::max(size, LARGE_MIN_GRANULARITY)) : RoundAllocationSize(size);
    const u64 max_size = deferred ? alloc_size
                         : pow2   ? alloc_size * 2
                                  : std::max(size + size / 4, RoundAllocationSize(size));

    LargeBuffer* best{};
    for (LargeBuffer& entry : cache) {
        const u64 entry_size = entry.buffer->SizeBytes();
        if (entry.held || entry_size < size || entry_size > max_size ||
            (!scheduler.IsFree(entry.tick) && !unsynchronized)) {
            continue;
        }
        if (!best || entry_size < best->buffer->SizeBytes()) {
            best = &entry;
        }
    }

    if (!best) {
        const SotcStats::FrameTimer frame_timer{SotcStats::FrameAlloc};
        const SotcStats::AllocTrace alloc_trace{SotcStats::AllocStagingLarge, alloc_size};
        // sotc: see RequestFromRing; the ReBAR choice is per allocation.
        const MemoryType alloc_type = type == MemoryType::HostUncached
                                          ? VideoCore::ResolveRebarType(instance, alloc_size, type)
                                          : type;
        best = &cache.emplace_back(LargeBuffer{
            .buffer = std::make_unique<VideoCore::Buffer>(instance, 0, alloc_size, alloc_type,
                                                          "StagingBufferPool:Dedicated"),
            .tick = 0,
            .last_used_frame = frame,
            .held = false,
        });
    }

    best->tick = scheduler.CurrentTick();
    best->last_used_frame = frame;
    best->held = deferred;

    VideoCore::Buffer& buffer = *best->buffer;
    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = 0,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data();
    }
    return ref;
}

void StagingBufferPool::FreeDeferred(const StagingBufferRef& ref) {
    auto& cache = large_caches[u32(ref.type)];
    const auto it = std::ranges::find_if(
        cache, [&](const LargeBuffer& entry) { return entry.buffer.get() == ref.buffer; });
    ASSERT_MSG(it != cache.end() && it->held, "FreeDeferred on an allocation that is not held");
    it->held = false;
    it->tick = scheduler.CurrentTick();
    it->last_used_frame = frame;
}

void StagingBufferPool::TickFrame() {
    ++frame;
    // sotc: the first SOTC_STAGING_KEEP_MB (default 0) of each host-memory ring stay allocated.
    // Trimmed after 300 idle frames, they were allocated again (~2 ms each) by the next
    // streaming burst, inside the frame that needed them: 115 allocations in a 6-minute attract
    // run. The device-local ring is still trimmed (video memory is short on 6 GB cards).
    static const size_t keep_host_blocks = [] {
        const char* v = std::getenv("SOTC_STAGING_KEEP_MB");
        return static_cast<size_t>((v ? std::strtoull(v, nullptr, 10) : 0ull) / (BlockSize() >> 20));
    }();
    for (size_t type = 0; type < rings.size(); ++type) {
        TrimRing(rings[type], type == u32(MemoryType::DeviceLocal) ? 0 : keep_host_blocks);
    }
    for (auto& cache : large_caches) {
        TrimLarge(cache);
    }
    PublishStats();
}

void StagingBufferPool::TrimRing(Ring& ring, size_t keep_blocks) {
    auto& blocks = ring.blocks;
    for (size_t i = blocks.size(); i-- > keep_blocks;) {
        const Block& block = blocks[i];
        if (frame - block.last_used_frame < RING_IDLE_FRAMES ||
            !scheduler.IsFree(block.buffer->LastTick())) {
            continue;
        }
        blocks.erase(blocks.begin() + i);
        if (i < ring.current) {
            --ring.current;
        } else if (i == ring.current) {
            ring.current = 0;
        }
    }
    if (ring.current >= blocks.size()) {
        ring.current = 0;
    }
}

void StagingBufferPool::TrimLarge(std::vector<LargeBuffer>& cache) {
    // sotc: SOTC_STAGING_LARGE_POW2: idle buffers are kept up to SOTC_STAGING_LARGE_KEEP_MB,
    // the most recently used first.
    u64 keep = StagingLargePow2() ? StagingLargeKeepBytes() : 0;
    if (keep != 0) {
        std::ranges::sort(cache, [](const LargeBuffer& a, const LargeBuffer& b) {
            return a.last_used_frame > b.last_used_frame;
        });
    }
    std::erase_if(cache, [this, &keep](const LargeBuffer& entry) {
        if (entry.held || frame - entry.last_used_frame < LARGE_IDLE_FRAMES ||
            !scheduler.IsFree(entry.tick)) {
            return false;
        }
        const u64 bytes = entry.buffer->SizeBytes();
        if (bytes <= keep) {
            keep -= bytes;
            return false;
        }
        return true;
    });
}

void StagingBufferPool::PublishStats() const {
    using namespace Core::Devtools::Widget;
    if (!GpuMemoryViewer::IsEnabled()) {
        return;
    }
    static constexpr std::array<const char*, NUM_TYPES> names = {"DeviceLocal", "HostUncached",
                                                                 "HostCached"};
    GpuMemoryGroup group{.name = "Staging"};
    for (size_t i = 0; i < NUM_TYPES; ++i) {
        u64 bytes = 0;
        for (const Block& block : rings[i].blocks) {
            bytes += block.buffer->SizeBytes();
        }
        for (const LargeBuffer& entry : large_caches[i]) {
            bytes += entry.buffer->SizeBytes();
        }
        if (bytes != 0) {
            group.rows.push_back({.name = names[i], .bytes = bytes});
        }
    }
    GpuMemoryViewer::Publish(std::move(group));
}

} // namespace Vulkan
