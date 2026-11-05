// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the game's memory for the GPU in place (sotc_guest_memory.h).

#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "video_core/buffer_cache/buffer.h"
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
    const VideoCore::Buffer* wrapper{}; ///< GetBuffer's view of buffer, made once, never freed
    bool queued{};                      ///< handed to the importer thread
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
void QueueImport(Chunk& chunk, u64 index);
void LogWhenAllIn();
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
    // sotc: SOTC_PC_MEMORY_PREIMPORT=1: every chunk imported now, at boot, on the importer thread:
    // an import while playing stalls the draw thread's own Vulkan calls (the driver locks while
    // it pins 64 MiB), the first-entry hitch SOTC_TEX_DIRECT was meant to remove.
    if (const char* v = std::getenv("SOTC_PC_MEMORY_PREIMPORT"); v && v[0] == '1') {
        LogWhenAllIn();
        for (u64 i = 1; i < g_chunks.size(); ++i) {
            QueueImport(g_chunks[i], i);
        }
    }
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

namespace {
std::mutex g_queue_mutex;
std::chrono::steady_clock::time_point g_preimport_start;
std::condition_variable g_queue_cv;
std::deque<u64> g_queue;

void ImporterLoop() {
    for (;;) {
        u64 index;
        {
            std::unique_lock lock{g_queue_mutex};
            g_queue_cv.wait(lock, [] { return !g_queue.empty(); });
            index = g_queue.front();
            g_queue.pop_front();
        }
        // The import itself outside g_mutex: Get keeps answering for the chunks already in.
        Chunk fresh;
        const bool ok = Import(fresh, index);
        std::scoped_lock lock{g_mutex};
        Chunk& chunk = g_chunks[index];
        if (chunk.buffer) { // imported meanwhile by Get (the visibility path imports in place)
            if (ok) {
                g_instance->GetDevice().destroyBuffer(fresh.buffer);
                g_instance->GetDevice().freeMemory(fresh.memory);
            }
            continue;
        }
        if (ok) {
            chunk.memory = fresh.memory;
            chunk.buffer = fresh.buffer;
        } else {
            chunk.failed = true;
        }
        std::unique_lock queue_lock{g_queue_mutex};
        if (g_queue.empty() && g_preimport_start.time_since_epoch().count()) {
            std::fprintf(stderr, "SOTC PC memory: %llu MiB imported in %.1f s\n",
                         static_cast<unsigned long long>(g_imported_bytes >> 20),
                         std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                       g_preimport_start).count());
            g_preimport_start = {};
        }
    }
}

void LogWhenAllIn() {
    g_preimport_start = std::chrono::steady_clock::now();
}

void QueueImport(Chunk& chunk, u64 index) {
    if (chunk.queued || chunk.failed) {
        return;
    }
    chunk.queued = true;
    static std::once_flag started;
    std::call_once(started, [] { std::thread{ImporterLoop}.detach(); });
    {
        std::scoped_lock lock{g_queue_mutex};
        g_queue.push_back(index);
    }
    g_queue_cv.notify_one();
}
} // namespace

const VideoCore::Buffer* GetBuffer(const u8* host, u64 size, u64* offset) {
    if (!g_enabled || host < g_base || size == 0) {
        return nullptr;
    }
    const u64 host_offset = static_cast<u64>(host - g_base);
    if (host_offset + size > g_size || host_offset / kChunk != (host_offset + size - 1) / kChunk) {
        return nullptr;
    }
    const u64 index = host_offset / kChunk;
    std::scoped_lock lock{g_mutex};
    Chunk& chunk = g_chunks[index];
    if (!chunk.buffer) {
        QueueImport(chunk, index);
        return nullptr;
    }
    const Span span{chunk.buffer, host_offset - index * kChunk};
    if (!chunk.wrapper) {
        chunk.wrapper = new VideoCore::Buffer(g_instance->GetDevice(), chunk.buffer,
                                              std::min(kChunk, g_size - index * kChunk));
    }
    *offset = span.offset;
    return chunk.wrapper;
}

} // namespace Vulkan::SotcGuestMemory
