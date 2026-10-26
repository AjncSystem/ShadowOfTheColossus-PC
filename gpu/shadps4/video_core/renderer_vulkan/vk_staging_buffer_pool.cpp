// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include "bbport_toggles.h"
#include "common/alignment.h"
#include "common/assert.h"
#include "common/scope_exit.h"
#include "bbport_copy.h"
#include "common/types.h"
#include "core/devtools/widget/gpu_memory.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/sotc_stats.h"

// sotc: src/runtime_memory.c, RAM census (SOTC_RAM_CENSUS=1).
extern "C" void runtime_memory_tag(const void* p, size_t size, const char* name);

namespace Vulkan {

using VideoCore::MemoryType;

namespace {

// Device-local staging (video memory): unchanged from upstream/previous values.
constexpr u64 RING_IDLE_FRAMES = 300;
constexpr u64 LARGE_IDLE_FRAMES = 120;
// sotc: host-memory staging counts against system RAM, and one streaming burst grows the host
// rings by 200+ MB that then stayed resident for 300 (ring) / 120 (large) idle frames. Trim them
// after 60 idle frames (~1 s at 60 FPS). SOTC_STAGING_KEEP_MB still pins the first blocks of
// each host ring against re-allocation cost.
constexpr u64 HOST_RING_IDLE_FRAMES = 60;
constexpr u64 HOST_LARGE_IDLE_FRAMES = 60;
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

// sotc: names the mapped pointer of a new staging buffer for the RAM census (SOTC_RAM_CENSUS=1
// labels its top private regions); the registry dedupes per 64 KiB and drops tags when full.
// Large buffers under 4 MiB are skipped to keep the 64 tag slots for the big regions.
void TagMapped(const VideoCore::Buffer& buffer, MemoryType type, bool large) {
    static constexpr const char* ring_names[] = {"staging ring device", "staging ring host-uncached",
                                                 "staging ring host-cached"};
    static constexpr const char* large_names[] = {"staging large device",
                                                  "staging large host-uncached",
                                                  "staging large host-cached"};
    if (buffer.mapped_data.empty() || (large && buffer.SizeBytes() < 4_MB)) {
        return;
    }
    const u32 index = std::min<u32>(u32(type), 2);
    runtime_memory_tag(buffer.mapped_data.data(), buffer.mapped_data.size(),
                       large ? large_names[index] : ring_names[index]);
}

// sotc: SOTC_STAGING_PRETOUCH (default on): writes one byte per page of a new host staging buffer on
// the copy threads before its first use, instead of taking the page faults of the first guest
// copy (85 MB: 43 ms cold, 5-7 ms warm).
bool PreTouchEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_STAGING_PRETOUCH");
        return !v || v[0] != '0';
    }();
    return on;
}

void PreTouch(VideoCore::Buffer& buffer) {
    if (!PreTouchEnabled() || buffer.mapped_data.empty()) {
        return;
    }
    u8* const data = buffer.mapped_data.data();
    const u64 size = buffer.mapped_data.size();
    constexpr u64 Piece = 1_MB;
    constexpr u64 Page = 4_KB;
    const auto touch = [&](std::size_t i) {
        const u64 end = std::min(size, (i + 1) * Piece);
        for (u64 offset = i * Piece; offset < end; offset += Page) {
            *reinterpret_cast<volatile u8*>(data + offset) = 0;
        }
    };
    const std::size_t pieces = (size + Piece - 1) / Piece;
    BbCopy::ParallelFor(pieces, touch);
}

// sotc: BbToggle::StagingScale (MEMORIA-PLAN.md candidate 5). The pinned reserve and the ring are
// pre-touched host memory: 224 MiB of resident RAM on any PC. With 16 GB of RAM or less they are
// 64 + 48 MiB (a burst beyond that allocates, ~2 ms, as it already does beyond 224). SOTC_STAGING_
// RESERVE_MB / SOTC_STAGING_RING_MB still win. SOTC_STAGING_RAM_MB overrides the detected RAM (tests).
bool ScaleForSmallRam() {
    static const bool scaled = [] {
        if (BbToggle::Disabled(BbToggle::StagingScale)) {
            return false;
        }
        u64 ram_mb = 0;
        if (const char* v = std::getenv("SOTC_STAGING_RAM_MB")) {
            ram_mb = std::strtoull(v, nullptr, 10);
        }
#ifdef _WIN32
        else {
            MEMORYSTATUSEX status{};
            status.dwLength = sizeof(status);
            if (GlobalMemoryStatusEx(&status)) {
                ram_mb = status.ullTotalPhys >> 20;
            }
        }
#endif
        // A "16 GB" PC reports 15.x-16.x GiB: the threshold is 17 GiB. Unknown RAM keeps full size.
        const bool small = ram_mb != 0 && ram_mb <= 17ull * 1024;
        if (SotcStats::kFpsLog) {
            std::fprintf(stderr, "SOTC staging: %llu MiB of RAM, %s reserve and ring\n",
                         static_cast<unsigned long long>(ram_mb), small ? "reduced" : "full");
        }
        return small;
    }();
    return scaled;
}

u64 ReserveBytes() {
    static const u64 bytes = [] {
        const char* v = std::getenv("SOTC_STAGING_RESERVE_MB");
        return (v ? std::strtoull(v, nullptr, 10) : (ScaleForSmallRam() ? 64ull : 128ull)) << 20;
    }();
    return bytes;
}

/// Sizes of the reserve buffers, in the order they are made: 64 MiB ones first, then 32 MiB.
u64 ReserveSize(u32 index) {
    const u64 total = ReserveBytes();
    const u64 big = (total / 2) / 64_MB; // half of the reserve in 64 MiB buffers
    if (index < big) {
        return 64_MB;
    }
    return 32_MB;
}

u32 ReserveCount() {
    const u64 total = ReserveBytes();
    u32 count = 0;
    u64 sum = 0;
    while (sum < total && count < 64) {
        sum += ReserveSize(count++);
    }
    return count;
}

/// Host ring blocks kept allocated and touched (SOTC_STAGING_RING_MB, default 96 = 6 blocks of 16 MiB):
/// most uploads are under a block and take their memory from the ring.
u32 RingReserveBlocks() {
    static const u32 blocks = [] {
        const char* v = std::getenv("SOTC_STAGING_RING_MB");
        const u64 mb = v ? std::strtoull(v, nullptr, 10) : (ScaleForSmallRam() ? 48ull : 96ull);
        return static_cast<u32>((mb << 20) / BlockSize());
    }();
    return blocks;
}

u64 RoundAllocationSize(u64 size) {
    const u64 step = std::max(std::bit_floor(size) / 4, LARGE_MIN_GRANULARITY);
    return Common::AlignUp(size, step);
}

// sotc: SOTC_STALL_LOG, who asked for the staging memory being allocated.
thread_local const void* request_caller{};

} // Anonymous namespace

StagingBufferPool::StagingBufferPool(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {}

StagingBufferPool::~StagingBufferPool() {
    while (reserve_pending.load() != 0) { // sotc: a reserve buffer still being made
        std::this_thread::yield();
    }
}

StagingBufferRef StagingBufferPool::Request(u64 size, MemoryType type, u64 alignment, bool deferred,
                                            bool unsynchronized) {
    if (SotcStats::kStallLog) {
        request_caller = __builtin_return_address(0);
    }
    Ring& ring = rings[u32(type)];
    if (deferred || size > BlockSize()) {
        return RequestLarge(size, type, deferred, unsynchronized);
    }
    return RequestFromRing(ring, size, alignment, type, unsynchronized);
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
            const auto stall_t0 = SotcStats::kStallLog ? std::chrono::steady_clock::now()
                                                       : std::chrono::steady_clock::time_point{};
            SCOPE_EXIT {
                if (SotcStats::kStallLog) {
                    SotcStats::StallNote(SotcStats::StallMsSince(stall_t0),
                                         "staging-block alloc %.1f ms: %.1f MB type %u (ring of %zu "
                                         "blocks), ask %.2f MB, caller %+lld",
                                         SotcStats::StallMsSince(stall_t0), BlockSize() / 1048576.0,
                                         u32(type), ring.blocks.size(), size / 1048576.0,
                                         SotcStats::StallOffset(request_caller));
                }
            };
            best = &ring.blocks.emplace_back(Block{
                .buffer = std::make_unique<VideoCore::StreamBuffer>(instance, scheduler, type,
                                                                    BlockSize()),
                .last_used_frame = frame,
            });
            ring.current = ring.blocks.size() - 1;
            if (type == MemoryType::HostUncached) {
                PreTouch(*best->buffer);
            }
            TagMapped(*best->buffer, type, false);
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
    const u64 alloc_size =
        deferred ? std::bit_ceil(std::max(size, LARGE_MIN_GRANULARITY)) : RoundAllocationSize(size);
    const u64 max_size = deferred ? alloc_size : std::max(size + size / 4, RoundAllocationSize(size));

    LargeBuffer* best{};
    for (LargeBuffer& entry : cache) {
        const u64 entry_size = entry.buffer->SizeBytes();
        if (entry.held || entry_size < size || (entry_size > max_size && !(entry.pinned && !deferred)) ||
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
        const auto stall_t0 = SotcStats::kStallLog ? std::chrono::steady_clock::now()
                                                   : std::chrono::steady_clock::time_point{};
        SCOPE_EXIT {
            if (SotcStats::kStallLog) {
                SotcStats::StallAgg(SotcStats::StallStagingAlloc, u64(type),
                                    SotcStats::StallMsSince(stall_t0), alloc_size / 1048576.0);
                SotcStats::StallNote(
                    SotcStats::StallMsSince(stall_t0),
                    "staging-large alloc %.1f ms: %.2f MB type %u%s%s (cache of %zu), ask %.2f MB, "
                    "caller %+lld",
                    SotcStats::StallMsSince(stall_t0), alloc_size / 1048576.0, u32(type),
                    deferred ? " deferred" : "", unsynchronized ? " unsync" : "", cache.size(),
                    size / 1048576.0, SotcStats::StallOffset(request_caller));
            }
        };
        best = &cache.emplace_back(LargeBuffer{
            .buffer = std::make_unique<VideoCore::Buffer>(instance, 0, alloc_size, type,
                                                          "StagingBufferPool:Dedicated"),
            .tick = 0,
            .last_used_frame = frame,
            .held = false,
        });
        if (type == MemoryType::HostUncached && !deferred) {
            PreTouch(*best->buffer);
        }
        TagMapped(*best->buffer, type, true);
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
    // Trimmed after the idle frames above, they were allocated again (~2 ms each) by the next
    // streaming burst, inside the frame that needed them: 115 allocations in a 6-minute attract
    // run. The device-local ring is still trimmed (video memory is short on 6 GB cards).
    static const size_t keep_host_blocks = [] {
        const char* v = std::getenv("SOTC_STAGING_KEEP_MB");
        return std::max<size_t>(
            static_cast<size_t>((v ? std::strtoull(v, nullptr, 10) : 0ull) / (BlockSize() >> 20)),
            ReserveBytes() != 0 ? RingReserveBlocks() : 0);
    }();
    for (size_t type = 0; type < rings.size(); ++type) {
        const bool device_local = type == u32(MemoryType::DeviceLocal);
        TrimRing(rings[type], device_local ? 0 : keep_host_blocks,
                 device_local ? RING_IDLE_FRAMES : HOST_RING_IDLE_FRAMES);
        TrimLarge(large_caches[type],
                  device_local ? LARGE_IDLE_FRAMES : HOST_LARGE_IDLE_FRAMES);
    }
    MaintainReserve();
    PublishStats();
}

void StagingBufferPool::TrimRing(Ring& ring, size_t keep_blocks, u64 idle_frames) {
    auto& blocks = ring.blocks;
    for (size_t i = blocks.size(); i-- > keep_blocks;) {
        const Block& block = blocks[i];
        if (frame - block.last_used_frame < idle_frames ||
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

void StagingBufferPool::TrimLarge(std::vector<LargeBuffer>& cache, u64 idle_frames) {
    std::erase_if(cache, [this, idle_frames](const LargeBuffer& entry) {
        return !entry.held && !entry.pinned && frame - entry.last_used_frame >= idle_frames &&
               scheduler.IsFree(entry.tick);
    });
}

void StagingBufferPool::MaintainReserve() {
    if (ReserveBytes() == 0) {
        return;
    }
    {
        std::scoped_lock lock{reserve_mutex};
        for (auto& buffer : reserve_ready) {
            TagMapped(*buffer, MemoryType::HostUncached, true);
            large_caches[u32(MemoryType::HostUncached)].push_back(LargeBuffer{
                .buffer = std::move(buffer),
                .tick = 0,
                .last_used_frame = frame,
                .held = false,
                .pinned = true,
            });
        }
        reserve_ready.clear();
        auto& ring = rings[u32(MemoryType::HostUncached)];
        for (auto& block : ring_ready) {
            TagMapped(*block, MemoryType::HostUncached, false);
            ring.blocks.push_back(Block{.buffer = std::move(block), .last_used_frame = frame});
        }
        ring_ready.clear();
    }
    // Start after the first seconds (boot), one buffer at a time: ring blocks first.
    if (frame < 300 || reserve_pending.load() != 0 || !BbCopy::Enabled()) {
        return;
    }
    if (ring_reserve_made < RingReserveBlocks()) {
        ++ring_reserve_made;
        reserve_pending.store(1);
        BbCopy::Async([this] {
            auto block = std::make_unique<VideoCore::StreamBuffer>(
                instance, scheduler, MemoryType::HostUncached, BlockSize());
            PreTouch(*block);
            {
                std::scoped_lock lock{reserve_mutex};
                ring_ready.push_back(std::move(block));
            }
            reserve_pending.store(0);
        });
        return;
    }
    if (reserve_made >= ReserveCount()) {
        return;
    }
    const u64 size = ReserveSize(reserve_made++);
    reserve_pending.store(1);
    BbCopy::Async([this, size] {
        auto buffer = std::make_unique<VideoCore::Buffer>(instance, 0, size, MemoryType::HostUncached,
                                                          "StagingBufferPool:Reserve");
        PreTouch(*buffer);
        if (SotcStats::kFpsLog) {
            std::fprintf(stderr, "SOTC staging reserve: %.0f MiB ready\n", size / 1048576.0);
        }
        {
            std::scoped_lock lock{reserve_mutex};
            reserve_ready.push_back(std::move(buffer));
        }
        reserve_pending.store(0);
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
