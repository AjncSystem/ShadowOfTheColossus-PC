// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include "common/logging/log.h"
#include "video_core/buffer_cache/host_import.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_stats.h"

extern "C" {
// runtime_memory.c
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
const unsigned char* runtime_memory_backing_view(uint64_t* size);
}

namespace VideoCore {

HostImport::HostImport(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    if (!instance.IsHostImportEnabled() || !scheduler.HasXferQueue()) {
        return;
    }
    // The guest memory view is created by the runtime after the GPU library starts: it is
    // looked up on the first upload (FindView).
    const u64 alignment = std::max<u64>(instance.HostImportAlignment(), 1);
    if (ChunkSize % alignment != 0) {
        LOG_WARNING(Render_Vulkan, "SOTC_HOST_IMPORT: import alignment {} not supported; uploads "
                                   "copy through the CPU",
                    alignment);
        return;
    }
    // SOTC_HOST_IMPORT_MB: imported (locked) guest memory at most, default 512 MiB.
    // SOTC_HOST_IMPORT_MIN_KB: smaller uploads stay on the CPU path, default 256 KiB.
    const char* budget_env = std::getenv("SOTC_HOST_IMPORT_MB");
    const char* min_env = std::getenv("SOTC_HOST_IMPORT_MIN_KB");
    const u64 budget_mb = budget_env ? std::clamp<u64>(std::strtoull(budget_env, nullptr, 10), 64,
                                                       8192)
                                     : 512;
    budget_chunks = std::max<u64>(budget_mb / (ChunkSize >> 20), 1);
    min_bytes = (min_env ? std::strtoull(min_env, nullptr, 10) : 256) << 10;
    enabled = true;
}

bool HostImport::FindView() {
    const u8* view = runtime_memory_backing_view(&view_size);
    if (!view || view_size == 0) {
        return false; // not created yet: asked again on the next upload
    }
    const u64 alignment = std::max<u64>(instance.HostImportAlignment(), 1);
    if (reinterpret_cast<uintptr_t>(view) % alignment != 0) {
        enabled = false;
        LOG_WARNING(Render_Vulkan, "SOTC_HOST_IMPORT: the guest memory view is not aligned to {} "
                                   "bytes; uploads copy through the CPU",
                    alignment);
        return false;
    }
    base = view;
    chunks.resize((view_size + ChunkSize - 1) / ChunkSize);
    return true;
}

HostImport::~HostImport() {
    for (auto& chunk : chunks) {
        Release(chunk);
    }
}

void HostImport::Release(Chunk& chunk) {
    const vk::Device device = instance.GetDevice();
    if (chunk.buffer) {
        device.destroyBuffer(chunk.buffer);
        chunk.buffer = vk::Buffer{};
    }
    if (chunk.memory) {
        device.freeMemory(chunk.memory);
        chunk.memory = vk::DeviceMemory{};
        --live_chunks;
        SotcStats::host_import_live_chunks.store(live_chunks, std::memory_order_relaxed);
    }
}

bool HostImport::Import(Chunk& chunk, u64 index) {
    const vk::Device device = instance.GetDevice();
    const u64 start = index * ChunkSize;
    const u64 size = std::min(ChunkSize, view_size - start);
    const u64 alignment = std::max<u64>(instance.HostImportAlignment(), 1);
    if (size % alignment != 0) {
        return false;
    }
    void* pointer = const_cast<u8*>(base + start);
    constexpr auto HandleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT;
    const auto [props_result, props] = device.getMemoryHostPointerPropertiesEXT(HandleType, pointer);
    if (props_result != vk::Result::eSuccess || props.memoryTypeBits == 0) {
        LOG_WARNING(Render_Vulkan, "SOTC_HOST_IMPORT: guest memory chunk {} refused ({})", index,
                    vk::to_string(props_result));
        return false;
    }
    const vk::ExternalMemoryBufferCreateInfo external_info = {.handleTypes = HandleType};
    const vk::BufferCreateInfo buffer_info = {
        .pNext = &external_info,
        .size = size,
        .usage = vk::BufferUsageFlagBits::eTransferSrc,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const auto [buffer_result, buffer] = device.createBuffer(buffer_info);
    if (buffer_result != vk::Result::eSuccess) {
        return false;
    }
    const auto requirements = device.getBufferMemoryRequirements(buffer);
    const u32 type_bits = props.memoryTypeBits & requirements.memoryTypeBits;
    if (type_bits == 0 || requirements.size > size) {
        device.destroyBuffer(buffer);
        return false;
    }
    const vk::ImportMemoryHostPointerInfoEXT import_info = {
        .handleType = HandleType,
        .pHostPointer = pointer,
    };
    const vk::MemoryAllocateInfo alloc_info = {
        .pNext = &import_info,
        .allocationSize = size,
        .memoryTypeIndex = static_cast<u32>(std::countr_zero(type_bits)),
    };
    const auto [memory_result, memory] = device.allocateMemory(alloc_info);
    if (memory_result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "SOTC_HOST_IMPORT: importing guest memory chunk {} failed ({})",
                    index, vk::to_string(memory_result));
        device.destroyBuffer(buffer);
        return false;
    }
    if (device.bindBufferMemory(buffer, memory, 0) != vk::Result::eSuccess) {
        device.destroyBuffer(buffer);
        device.freeMemory(memory);
        return false;
    }
    chunk.buffer = buffer;
    chunk.memory = memory;
    chunk.size = size;
    ++live_chunks;
    SotcStats::host_import_live_chunks.store(live_chunks, std::memory_order_relaxed);
    return true;
}

HostImport::Chunk* HostImport::GetChunk(u64 index) {
    if (index >= chunks.size()) {
        return nullptr;
    }
    Chunk& chunk = chunks[index];
    if (chunk.failed) {
        return nullptr;
    }
    if (!chunk.memory) {
        // Over the budget, the least recently used chunk goes; never one this upload uses.
        // Uploads wait for their copies, so no GPU work reads a chunk between two uploads.
        while (live_chunks >= budget_chunks) {
            Chunk* oldest = nullptr;
            for (auto& other : chunks) {
                if (other.memory && other.last_use != use_counter &&
                    (!oldest || other.last_use < oldest->last_use)) {
                    oldest = &other;
                }
            }
            if (!oldest) {
                return nullptr;
            }
            Release(*oldest);
        }
        if (!Import(chunk, index)) {
            chunk.failed = true;
            SotcStats::host_import_refused.fetch_add(1, std::memory_order_relaxed);
            // A driver that refuses the first chunks refuses this kind of memory: stop trying
            // (one warning per chunk otherwise, ~150 of them).
            if (++refused >= 4 && live_chunks == 0 && enabled) {
                enabled = false;
                LOG_WARNING(Render_Vulkan, "SOTC_HOST_IMPORT: the driver refuses to import guest "
                                           "memory; uploads copy through the CPU");
            }
            return nullptr;
        }
    }
    chunk.last_use = use_counter;
    return &chunk;
}

bool HostImport::Upload(std::span<const vk::BufferCopy> copies, vk::Buffer dst, u64 dst_base) {
    if (!enabled) {
        return false;
    }
    const auto start_time = std::chrono::steady_clock::now();
    std::scoped_lock lock{mutex};
    if (!base && !FindView()) {
        SotcStats::host_import_fallbacks.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ++use_counter;
    boost::container::small_vector<Vulkan::XferQueue::Region, 16> regions;
    u64 bytes = 0;
    for (const auto& copy : copies) {
        VAddr address = copy.dstOffset;
        u64 dst_offset = dst_base + copy.srcOffset;
        u64 left = copy.size;
        while (left > 0) {
            // One mapping: its backing bytes are contiguous in the view.
            uintptr_t region_start = 0, region_end = 0;
            int mapped = 0;
            if (!runtime_memory_region(address, &region_start, &region_end, &mapped) || !mapped) {
                SotcStats::host_import_fallbacks.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            const u8* backing = runtime_memory_backing_pointer(address);
            if (!backing || backing < base || backing >= base + view_size) {
                SotcStats::host_import_fallbacks.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            u64 view_offset = static_cast<u64>(backing - base);
            u64 run = std::min<u64>(left, region_end - address);
            run = std::min<u64>(run, view_size - view_offset);
            while (run > 0) {
                // And one chunk.
                const u64 index = view_offset / ChunkSize;
                Chunk* chunk = GetChunk(index);
                if (!chunk) {
                    SotcStats::host_import_fallbacks.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                const u64 in_chunk = view_offset - index * ChunkSize;
                const u64 piece = std::min(run, chunk->size - in_chunk);
                regions.push_back({chunk->buffer, dst,
                                   vk::BufferCopy{in_chunk, dst_offset, piece}});
                view_offset += piece;
                dst_offset += piece;
                address += piece;
                left -= piece;
                run -= piece;
                bytes += piece;
            }
        }
    }
    if (regions.empty() || !scheduler.XferCopyAndWait(regions)) {
        return false;
    }
    SotcStats::host_import_uploads.fetch_add(1, std::memory_order_relaxed);
    SotcStats::host_import_bytes.fetch_add(bytes, std::memory_order_relaxed);
    SotcStats::host_import_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                             start_time)
            .count(),
        std::memory_order_relaxed);
    return true;
}

} // namespace VideoCore
