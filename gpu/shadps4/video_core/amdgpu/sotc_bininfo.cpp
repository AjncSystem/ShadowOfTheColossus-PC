// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: shader binary info without readbacks.
//
// The command processor looks up the binary info (hash, length) of every program it binds.
// In Shadow of the Colossus the pages holding shader code share tracking regions with buffers
// the GPU writes, so with precise readbacks each lookup read-faulted and synchronized with the
// GPU (~55 times a second in the open world). The game does not modify loaded shader code:
// the lookup is cached per code address and validated against the backing view of guest
// memory, which reads the CPU-side bytes without faulting. SOTC_BININFO_CACHE=0 disables it.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include "core/memory.h"
#include "video_core/amdgpu/regs_shader.h"

extern "C" uint64_t runtime_memory_table_generation(void);

namespace AmdGpu {

namespace {
struct CachedInfo {
    u32 head[2];
    u64 info_offset; // bytes from the code start
    BinaryInfo info;
};
std::mutex cache_mutex;
std::unordered_map<VAddr, CachedInfo> cache;

bool Enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_BININFO_CACHE");
        return !v || v[0] != '0';
    }();
    return enabled;
}

/// Per-thread fast path: the backing pointers of a validated lookup stay usable while the
/// mapping table generation is unchanged, so a hit costs two compares and no lock or table
/// search. Validation is the same as the shared cache's. SOTC_BININFO_FAST=0 disables it.
struct FastEntry {
    VAddr address;
    u64 generation;
    const u8* head_backing;
    const u8* info_backing;
    u32 head[2];
    const BinaryInfo* info;
};
thread_local std::array<FastEntry, 256> fast_entries{};

bool FastEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_BININFO_FAST");
        return !v || v[0] != '0';
    }();
    return enabled;
}
} // namespace

const BinaryInfo& SearchBinaryInfoCached(const u32* code) {
    if (!Enabled()) {
        return SearchBinaryInfo(code);
    }
    const VAddr address = reinterpret_cast<VAddr>(code);
    const u64 generation = FastEnabled() ? runtime_memory_table_generation() : 1;
    FastEntry& fast = fast_entries[(address >> 8) & (fast_entries.size() - 1)];
    if (!(generation & 1) && fast.address == address && fast.generation == generation &&
        std::memcmp(fast.head_backing, fast.head, sizeof(fast.head)) == 0 &&
        std::memcmp(fast.info_backing, fast.info, sizeof(BinaryInfo)) == 0) {
        return *fast.info;
    }
    const u8* backing = Core::Memory::Instance()->GetBackingPointer(address);
    if (!backing) {
        return SearchBinaryInfo(code);
    }
    u32 head[2];
    std::memcpy(head, backing, sizeof(head));
    {
        std::scoped_lock lk{cache_mutex};
        const auto it = cache.find(address);
        if (it != cache.end() && std::memcmp(it->second.head, head, sizeof(head)) == 0) {
            const u8* info_backing =
                Core::Memory::Instance()->GetBackingPointer(address + it->second.info_offset);
            if (info_backing &&
                std::memcmp(info_backing, &it->second.info, sizeof(BinaryInfo)) == 0) {
                if (!(generation & 1) && generation == runtime_memory_table_generation()) {
                    fast = {address, generation, backing, info_backing,
                            {head[0], head[1]}, &it->second.info};
                }
                return it->second.info;
            }
        }
    }
    // First lookup (or the code changed): the regular search, then remember it.
    const BinaryInfo& found = SearchBinaryInfo(code);
    std::scoped_lock lk{cache_mutex};
    auto& entry = cache[address];
    std::memcpy(entry.head, code, sizeof(entry.head));
    entry.info_offset = reinterpret_cast<const u8*>(&found) - reinterpret_cast<const u8*>(code);
    entry.info = found;
    return entry.info;
}

bool TryBinaryInfoCached(const u32* code, BinaryInfo& out) {
    if (!Enabled()) {
        return false;
    }
    // The validation of SearchBinaryInfoCached, through the backing view only: a lookup the
    // command processor has not made yet fails instead of searching guest memory.
    const VAddr address = reinterpret_cast<VAddr>(code);
    const u64 generation = FastEnabled() ? runtime_memory_table_generation() : 1;
    FastEntry& fast = fast_entries[(address >> 8) & (fast_entries.size() - 1)];
    if (!(generation & 1) && fast.address == address && fast.generation == generation &&
        std::memcmp(fast.head_backing, fast.head, sizeof(fast.head)) == 0 &&
        std::memcmp(fast.info_backing, fast.info, sizeof(BinaryInfo)) == 0) {
        std::memcpy(&out, fast.info_backing, sizeof(BinaryInfo));
        return true;
    }
    const u8* backing = Core::Memory::Instance()->GetBackingPointer(address);
    if (!backing) {
        return false;
    }
    u32 head[2];
    std::memcpy(head, backing, sizeof(head));
    std::scoped_lock lk{cache_mutex};
    const auto it = cache.find(address);
    if (it == cache.end() || std::memcmp(it->second.head, head, sizeof(head)) != 0) {
        return false;
    }
    const u8* info_backing =
        Core::Memory::Instance()->GetBackingPointer(address + it->second.info_offset);
    if (!info_backing || std::memcmp(info_backing, &it->second.info, sizeof(BinaryInfo)) != 0) {
        return false;
    }
    if (!(generation & 1) && generation == runtime_memory_table_generation()) {
        fast = {address, generation, backing, info_backing, {head[0], head[1]}, &it->second.info};
    }
    std::memcpy(&out, info_backing, sizeof(BinaryInfo));
    return true;
}

} // namespace AmdGpu
