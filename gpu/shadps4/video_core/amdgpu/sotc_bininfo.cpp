// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: shader binary info without readbacks.
//
// The command processor looks up the binary info (hash, length) of every program it binds.
// In Shadow of the Colossus the pages holding shader code share tracking regions with buffers
// the GPU writes, so with precise readbacks each lookup read-faulted and synchronized with the
// GPU (~55 times a second in the open world). The game does not modify loaded shader code:
// the lookup is cached per code address and validated against the backing view of guest
// memory, which reads the CPU-side bytes without faulting. SOTC_BININFO_CACHE=0 disables it.

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include "core/memory.h"
#include "video_core/amdgpu/regs_shader.h"

// src/runtime_memory.c: even while backing pointers obtained under it stay valid.
extern "C" uint64_t runtime_memory_generation(void);

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

// sotc: per-thread first level in front of the locked map (the command processor binds a program
// for every stage of every draw: two backing lookups, a mutex and a hash find each time). An entry
// keeps the backing pointers of the code head and of the info block, valid while the VMA table
// generation is unchanged, and byte copies of both (memcpy: BinaryInfo has tail padding), which
// are compared on every hit exactly as the map path compares them. SOTC_BININFO_L1=0 disables it.
struct L1Entry {
    VAddr address;
    u64 generation;
    const u8* head_backing;
    const u8* info_backing;
    u8 head[8];
    alignas(BinaryInfo) u8 info[sizeof(BinaryInfo)];
};
thread_local L1Entry l1[1024];

bool L1Enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_BININFO_L1");
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
    const u64 generation = L1Enabled() ? runtime_memory_generation() : 1;
    L1Entry* l1e = nullptr;
    if (!(generation & 1)) {
        l1e = &l1[((address >> 8) ^ (address >> 18)) & 1023];
        if (l1e->address == address && l1e->generation == generation &&
            std::memcmp(l1e->head_backing, l1e->head, sizeof(l1e->head)) == 0 &&
            std::memcmp(l1e->info_backing, l1e->info, sizeof(l1e->info)) == 0) {
            return *reinterpret_cast<const BinaryInfo*>(l1e->info);
        }
    }
    const u8* backing = Core::Memory::Instance()->GetBackingPointer(address);
    if (!backing) {
        return SearchBinaryInfo(code);
    }
    // Fills the first level from the backing view (never from the guest address).
    const auto fill_l1 = [&](u64 info_offset) {
        const u8* info_backing = Core::Memory::Instance()->GetBackingPointer(address + info_offset);
        if (!l1e || !info_backing) {
            return;
        }
        l1e->address = address;
        l1e->generation = generation;
        l1e->head_backing = backing;
        l1e->info_backing = info_backing;
        std::memcpy(l1e->head, backing, sizeof(l1e->head));
        std::memcpy(l1e->info, info_backing, sizeof(l1e->info));
    };
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
                fill_l1(it->second.info_offset);
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
    fill_l1(entry.info_offset);
    return entry.info;
}

} // namespace AmdGpu
