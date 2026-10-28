// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the game's memory for the GPU in place (sotc_guest_memory.h).

#include <chrono>
#include <cstdio>
#include <mutex>
#include <vector>

#include "video_core/renderer_vulkan/sotc_guest_memory.h"
#include "video_core/renderer_vulkan/vk_instance.h"

extern "C" const unsigned char* runtime_memory_backing_view(uint64_t* size);

namespace Vulkan::SotcGuestMemory {

namespace {

constexpr u64 kChunk = 64ULL << 20;

struct Chunk {
    vk::DeviceMemory memory;
    vk::Buffer buffer;
    bool failed{}; ///< refused once: not asked again
};

const Instance* g_instance{};
const u8* g_base{};
u64 g_size{};
u32 g_memory_type{};
std::mutex g_mutex;
std::vector<Chunk> g_chunks;
u64 g_imported_bytes{};
bool g_enabled{};

bool Import(Chunk& chunk, u64 index) {
    const vk::Device device = g_instance->GetDevice();
    const u64 size = std::min(kChunk, g_size - index * kChunk);
    const vk::ImportMemoryHostPointerInfoEXT import_info{
        .handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
        .pHostPointer = const_cast<u8*>(g_base + index * kChunk),
    };
    const auto [memory_result, memory] = device.allocateMemory(vk::MemoryAllocateInfo{
        .pNext = &import_info,
        .allocationSize = size,
        .memoryTypeIndex = g_memory_type,
    });
    if (memory_result != vk::Result::eSuccess) {
        std::fprintf(stderr, "SOTC PC memory: chunk %llu not imported (%s)\n",
                     static_cast<unsigned long long>(index), vk::to_string(memory_result).c_str());
        return false;
    }
    const vk::ExternalMemoryBufferCreateInfo external{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
    };
    const auto [buffer_result, buffer] = device.createBuffer(vk::BufferCreateInfo{
        .pNext = &external,
        .size = size,
        .usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
                 vk::BufferUsageFlagBits::eStorageBuffer,
    });
    if (buffer_result != vk::Result::eSuccess ||
        device.bindBufferMemory(buffer, memory, 0) != vk::Result::eSuccess) {
        std::fprintf(stderr, "SOTC PC memory: chunk %llu has no buffer\n",
                     static_cast<unsigned long long>(index));
        if (buffer_result == vk::Result::eSuccess) {
            device.destroyBuffer(buffer);
        }
        device.freeMemory(memory);
        return false;
    }
    chunk.memory = memory;
    chunk.buffer = buffer;
    g_imported_bytes += size;
    return true;
}

} // namespace

namespace {
bool g_resolved{}; ///< the backing view was looked at (it exists only once the game maps memory)

void Resolve() {
    const Instance& instance = *g_instance;
    g_base = runtime_memory_backing_view(&g_size);
    if (!g_base || g_size == 0) {
        return; // not yet: the next submission looks again
    }
    g_resolved = true;
    const vk::Device device = instance.GetDevice();
    const auto [properties_result, host_properties] = device.getMemoryHostPointerPropertiesEXT(
        vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, g_base);
    if (properties_result != vk::Result::eSuccess || host_properties.memoryTypeBits == 0) {
        std::fprintf(stderr, "SOTC PC memory: the backing view cannot be imported (%s), off\n",
                     vk::to_string(properties_result).c_str());
        return;
    }
    // A host-visible, coherent type: what the GPU writes is the game's memory as soon as the
    // work is done, with no flush.
    const auto memory = instance.GetPhysicalDevice().getMemoryProperties();
    constexpr auto wanted =
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    bool found = false;
    for (u32 i = 0; i < memory.memoryTypeCount && !found; ++i) {
        if ((host_properties.memoryTypeBits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
            g_memory_type = i;
            found = true;
        }
    }
    if (!found) {
        std::fprintf(stderr, "SOTC PC memory: no coherent host memory type (bits 0x%x), off\n",
                     host_properties.memoryTypeBits);
        return;
    }
    g_chunks.resize((g_size + kChunk - 1) / kChunk);
    // The first chunk now: proves the import works on this driver before anything relies on it.
    const auto start = std::chrono::steady_clock::now();
    if (!Import(g_chunks[0], 0)) {
        g_chunks[0].failed = true;
        std::fprintf(stderr, "SOTC PC memory: import refused by the driver, off\n");
        return;
    }
    g_enabled = true;
    std::fprintf(stderr,
                 "SOTC PC memory: %llu MiB of game memory, %zu chunks of 64 MiB imported on use "
                 "(memory type %u); chunk 0 in %.1f ms\n",
                 static_cast<unsigned long long>(g_size >> 20), g_chunks.size(), g_memory_type,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                           start)
                     .count());
}
} // namespace

void Init(const Instance& instance) {
    if (instance.IsExternalMemoryHostEnabled()) {
        g_instance = &instance;
    }
}

bool Ready() {
    if (g_instance && !g_resolved) {
        std::scoped_lock lock{g_mutex};
        if (!g_resolved) {
            Resolve();
        }
    }
    return g_enabled;
}

bool Enabled() {
    return g_enabled;
}

Span Get(const u8* host, u64 size) {
    if (!g_enabled || host < g_base || size == 0) {
        return {};
    }
    const u64 offset = static_cast<u64>(host - g_base);
    if (offset + size > g_size || offset / kChunk != (offset + size - 1) / kChunk) {
        return {};
    }
    const u64 index = offset / kChunk;
    std::scoped_lock lock{g_mutex};
    Chunk& chunk = g_chunks[index];
    if (!chunk.buffer) {
        if (chunk.failed || !Import(chunk, index)) {
            chunk.failed = true;
            return {};
        }
    }
    return {chunk.buffer, offset - index * kChunk};
}

} // namespace Vulkan::SotcGuestMemory
