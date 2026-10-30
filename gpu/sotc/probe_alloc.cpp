// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: block-allocator probe (plan step 0c, docs/HOOK-DESIGN.md). Two hooks (scripts/hooks.d/alloc.py),
// switch SOTC_HOOK_ALLOC_PROBE:
//   ALLOC_BLOCK  wrap at mem_allocator_alloc (0x479a0). Pre: rdi = memory-family object
//                (name char* at +0x10, kind int32 at +0x18: 1 = CPU, 2 = GPU), rsi = requested size.
//                Post: rax = block node (0 on failure); node base at +0x60, size at +0x58.
//   FREE_BLOCK   entry at pool_free_block (0x1b0e0). rdi = the node ALLOC_BLOCK returned; size at +0x58.
// The probe aggregates, per allocator name + kind, the number of allocations, the live bytes
// (allocations minus frees) and the peak. A free carries only the node, so each live block's pool and
// size are remembered in a fixed node table and looked up when it is freed. Observe only: every handler
// returns HookAction::Continue and nothing in the guest is written.
//
// Guest reads go through the backing view (runtime_memory_backing_pointer / runtime_memory_region),
// never the guest mapping, which the GPU page tracker may protect. Addresses inside the loaded game
// image (static allocator objects, name strings) are read directly: the image is not in that view.

#include <array>
#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "hooks.h"
#include "mem_classes.h" // sotc: step 6 feed (CPU-only ranges)
#include "probes.h"
#include "video_core/sotc_trace.h" // image bounds (SotcTrace::ImageBase/ImageSize)

extern "C" {
// runtime_memory.c: the unprotected backing view of a guest address, and the mapped region of one.
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
}

namespace SotcProbes {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr uint64_t kNameOffset = 0x10; // family object: char* allocator name
constexpr uint64_t kKindOffset = 0x18; // family object: int32 kind (1 CPU, 2 GPU)
constexpr uint64_t kNodeSizeOffset = 0x58; // block node: allocated size
constexpr uint64_t kNodeBaseOffset = 0x60; // block node: guest base address of the allocation

constexpr size_t kNameMax = 48;   // stored allocator name length (NUL-terminated)
constexpr size_t kMaxPools = 48;  // distinct name+kind rows
// Live blocks tracked (open addressing, power of two). The game keeps tens of thousands of blocks
// live (~300 more per second during loads), so 8192 filled in seconds.
constexpr size_t kMaxNodes = 65536;
constexpr int kMaxDepth = 8;      // nested ALLOC_BLOCK calls per thread (defensive; it does not nest)

// Reads n bytes of guest memory through the backing view, bounded to the mapped region so a read near
// a region edge cannot run past it. False when unmapped or partly outside the region.
bool ReadGuest(uintptr_t address, void* out, size_t n) {
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (!runtime_memory_region(address, &start, &end, &mapped) || !mapped) {
        return false;
    }
    if (address < start || address + n > end) {
        return false;
    }
    const unsigned char* p = runtime_memory_backing_pointer(address);
    if (!p) {
        return false;
    }
    std::memcpy(out, p, n);
    return true;
}

// sotc: the loaded game image (eboot + linked modules) is mapped directly and is not in the GPU
// backing view: the allocator families' static objects and the allocator name strings live there,
// so reads through the backing view alone came back empty (names "?", kinds unknown).
bool InImage(uintptr_t address, size_t n) {
    const uint64_t base = SotcTrace::ImageBase();
    const uint64_t size = SotcTrace::ImageSize();
    return base != 0 && size != 0 && address >= base && n <= size && address - base <= size - n;
}

// Guest memory first through the backing view (heap memory the GPU tracker may protect), then the
// image directly. False when neither holds the whole range.
bool ReadAny(uintptr_t address, void* out, size_t n) {
    if (ReadGuest(address, out, n)) {
        return true;
    }
    if (InImage(address, n)) {
        std::memcpy(out, reinterpret_cast<const void*>(address), n);
        return true;
    }
    return false;
}

// Copies a NUL-terminated guest C string into out (always terminated), bounded to the mapped region
// (backing view) or to the image.
void ReadGuestString(uintptr_t address, char* out, size_t cap) {
    out[0] = '\0';
    if (!address || cap == 0) {
        return;
    }
    const unsigned char* p = nullptr;
    size_t avail = 0;
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (runtime_memory_region(address, &start, &end, &mapped) && mapped && address >= start) {
        p = runtime_memory_backing_pointer(address);
        avail = static_cast<size_t>(end - address);
    }
    if (!p && InImage(address, 1)) {
        p = reinterpret_cast<const unsigned char*>(address);
        avail = static_cast<size_t>(SotcTrace::ImageBase() + SotcTrace::ImageSize() - address);
    }
    if (!p) {
        return;
    }
    size_t limit = std::min(cap - 1, avail);
    size_t i = 0;
    for (; i < limit && p[i] != 0; ++i) {
        out[i] = static_cast<char>(p[i]);
    }
    out[i] = '\0';
}

// One allocator pool, keyed by name + kind. Live/peak are lifetime; the w_* fields are the current
// report window and are reset by ReportAlloc.
struct PoolRow {
    char name[kNameMax] = {0};
    int32_t kind = 0;
    uint64_t alloc_count = 0; // lifetime allocations
    int64_t live_bytes = 0;   // lifetime allocations minus frees (clamped at 0)
    uint64_t peak_bytes = 0;  // lifetime peak of live_bytes
    uint64_t w_allocs = 0;    // window: allocations
    uint64_t w_frees = 0;     // window: frees
    uint64_t w_alloc_bytes = 0;
    uint64_t w_free_bytes = 0;
};

// node pointer -> pool row + remembered size, so a free (which carries only the node) is attributed.
struct NodeEntry {
    uint64_t key = 0; // guest node pointer; 0 = empty slot
    uint32_t pool = 0;
    uint64_t size = 0;
    uint64_t base = 0; // guest base address (for the step-6 CPU-only range table)
};

std::mutex g_mutex; // guards the pool table and the node table (block allocs are infrequent)
std::array<PoolRow, kMaxPools> g_pools{};
size_t g_pool_count = 0;
std::array<NodeEntry, kMaxNodes> g_nodes{};
size_t g_node_live = 0;

std::atomic<uint64_t> g_missed_frees{0}; // frees of a node we never recorded (pre-probe, or table full)
std::atomic<uint64_t> g_dropped_allocs{0}; // allocs we could not record (pool or node table full)
std::atomic<uint64_t> g_overflow{0};       // ALLOC_BLOCK nested past kMaxDepth

// Finds or creates the pool row for name+kind. Returns -1 when the table is full. Caller holds g_mutex.
int FindPool(const char* name, int32_t kind) {
    // An unreadable name is stored as "?"; compare with the same key, or every such allocation
    // opened a new row and filled the table in seconds.
    if (!name[0]) {
        name = "?";
    }
    for (size_t i = 0; i < g_pool_count; ++i) {
        if (g_pools[i].kind == kind && std::strncmp(g_pools[i].name, name, kNameMax) == 0) {
            return static_cast<int>(i);
        }
    }
    if (g_pool_count == kMaxPools) {
        return -1;
    }
    PoolRow& row = g_pools[g_pool_count];
    row = PoolRow{};
    std::snprintf(row.name, kNameMax, "%s", name);
    row.kind = kind;
    return static_cast<int>(g_pool_count++);
}

constexpr size_t kNodeMask = kMaxNodes - 1;
static_assert((kMaxNodes & kNodeMask) == 0, "kMaxNodes must be a power of two");

// Inserts a live node. Caller holds g_mutex. Returns false when the table is (near) full.
bool NodeInsert(uint64_t key, uint32_t pool, uint64_t size, uint64_t base) {
    if (!key || g_node_live >= kMaxNodes - (kMaxNodes / 8)) {
        return false; // keep the table under 7/8 so lookups stay short
    }
    size_t i = static_cast<size_t>(key) & kNodeMask;
    for (size_t probe = 0; probe < kMaxNodes; ++probe) {
        NodeEntry& e = g_nodes[i];
        if (e.key == 0) {
            e.key = key;
            e.pool = pool;
            e.size = size;
            e.base = base;
            ++g_node_live;
            return true;
        }
        if (e.key == key) { // same node re-allocated without a free: overwrite
            e.pool = pool;
            e.size = size;
            e.base = base;
            return true;
        }
        i = (i + 1) & kNodeMask;
    }
    return false;
}

// Removes a live node, returning its pool, size and base. Caller holds g_mutex. False when not found.
bool NodeRemove(uint64_t key, uint32_t* pool, uint64_t* size, uint64_t* base) {
    if (!key) {
        return false;
    }
    size_t i = static_cast<size_t>(key) & kNodeMask;
    for (size_t probe = 0; probe < kMaxNodes; ++probe) {
        NodeEntry& e = g_nodes[i];
        if (e.key == 0) {
            return false;
        }
        if (e.key == key) {
            *pool = e.pool;
            *size = e.size;
            *base = e.base;
            e.key = 0;
            --g_node_live;
            // Backward-shift deletion keeps the table tombstone-free.
            size_t j = (i + 1) & kNodeMask;
            while (g_nodes[j].key != 0) {
                size_t home = static_cast<size_t>(g_nodes[j].key) & kNodeMask;
                // Move entry j back to i if i lies in [home, j) cyclically.
                bool movable = (i <= j) ? (home <= i || home > j) : (home <= i && home > j);
                if (movable) {
                    g_nodes[i] = g_nodes[j];
                    g_nodes[j].key = 0;
                    i = j;
                }
                j = (j + 1) & kNodeMask;
            }
            return true;
        }
        i = (i + 1) & kNodeMask;
    }
    return false;
}

// ALLOC_BLOCK is a wrap: pre carries the family object and requested size to post, which sees the node.
struct AllocFrame {
    uint64_t family = 0;
    uint64_t req_size = 0;
};
struct FrameStack {
    AllocFrame frames[kMaxDepth];
    int depth = 0;
    int overflow = 0;
};
thread_local FrameStack t_alloc;

HookAction AllocPre(GuestRegs& regs, void*) {
    FrameStack& s = t_alloc;
    if (s.depth >= kMaxDepth) {
        ++s.overflow;
        g_overflow.fetch_add(1, std::memory_order_relaxed);
        return HookAction::Continue;
    }
    AllocFrame& f = s.frames[s.depth++];
    f.family = regs.rdi;
    f.req_size = regs.rsi;
    return HookAction::Continue;
}

void AllocPost(GuestRegs& regs, void*) {
    FrameStack& s = t_alloc;
    if (s.overflow > 0) {
        --s.overflow;
        return;
    }
    if (s.depth == 0) {
        return;
    }
    const AllocFrame f = s.frames[--s.depth];

    const uint64_t node = regs.rax;
    if (!node) {
        return; // allocation failed: nothing live
    }
    char name[kNameMax] = {0};
    uint64_t name_ptr = 0;
    if (ReadAny(f.family + kNameOffset, &name_ptr, sizeof name_ptr)) {
        ReadGuestString(static_cast<uintptr_t>(name_ptr), name, kNameMax);
    }
    int32_t kind = 0;
    ReadAny(f.family + kKindOffset, &kind, sizeof kind);

    uint64_t size = f.req_size; // fall back to the requested size if the node field is unreadable
    uint64_t node_size = 0;
    if (ReadAny(node + kNodeSizeOffset, &node_size, sizeof node_size)) {
        size = node_size;
    }
    uint64_t base = 0;
    ReadAny(node + kNodeBaseOffset, &base, sizeof base);

    std::lock_guard<std::mutex> lock(g_mutex);
    const int pool = FindPool(name, kind);
    if (pool < 0) {
        g_dropped_allocs.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!NodeInsert(node, static_cast<uint32_t>(pool), size, base)) {
        g_dropped_allocs.fetch_add(1, std::memory_order_relaxed);
        return; // do not count an alloc whose matching free we could not track
    }
    // sotc: step 6. A CPU-only range is a CPUMemory pool (kind 1) that is not a GPU upload pool.
    // "staging-*" are the upload staging pools, which the GPU reads, so they are never CPU-only.
    const bool cpu_only = kind == 1 && std::strncmp(name, "staging", 7) != 0;
    SotcMemClasses::OnAlloc(base, size, cpu_only, name);
    PoolRow& row = g_pools[pool];
    row.alloc_count += 1;
    row.live_bytes += static_cast<int64_t>(size);
    if (row.live_bytes > static_cast<int64_t>(row.peak_bytes)) {
        row.peak_bytes = static_cast<uint64_t>(row.live_bytes);
    }
    row.w_allocs += 1;
    row.w_alloc_bytes += size;
}

HookAction FreePre(GuestRegs& regs, void*) {
    const uint64_t node = regs.rdi;
    std::lock_guard<std::mutex> lock(g_mutex);
    uint32_t pool = 0;
    uint64_t size = 0;
    uint64_t base = 0;
    if (!NodeRemove(node, &pool, &size, &base)) {
        g_missed_frees.fetch_add(1, std::memory_order_relaxed);
        return HookAction::Continue;
    }
    SotcMemClasses::OnFree(base); // sotc: step 6 (no-op for ranges it never tracked)
    PoolRow& row = g_pools[pool];
    row.live_bytes -= static_cast<int64_t>(size);
    if (row.live_bytes < 0) {
        row.live_bytes = 0;
    }
    row.w_frees += 1;
    row.w_free_bytes += size;
    return HookAction::Continue;
}

double Mib(uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

const char* KindName(int32_t kind) {
    switch (kind) {
    case 1:
        return "CPU";
    case 2:
        return "GPU";
    default:
        return "?";
    }
}

} // namespace

void RegisterAlloc() {
    // Registered unconditionally: a hook that is off never dispatches to its handler.
    SotcHooks::Handler alloc;
    alloc.pre = &AllocPre;
    alloc.post = &AllocPost;
    SotcHooks::RegisterHandler("ALLOC_BLOCK", alloc);

    SotcHooks::Handler freed;
    freed.pre = &FreePre;
    SotcHooks::RegisterHandler("FREE_BLOCK", freed);
}

void ReportAlloc(std::string& out, double seconds) {
    if (seconds <= 0.0) {
        seconds = 1.0;
    }
    // Snapshot the pools and reset the window counters under the lock, then format without it.
    std::array<PoolRow, kMaxPools> rows{};
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        count = g_pool_count;
        for (size_t i = 0; i < count; ++i) {
            rows[i] = g_pools[i];
            g_pools[i].w_allocs = 0;
            g_pools[i].w_frees = 0;
            g_pools[i].w_alloc_bytes = 0;
            g_pools[i].w_free_bytes = 0;
        }
    }
    const uint64_t missed = g_missed_frees.exchange(0, std::memory_order_relaxed);
    const uint64_t dropped = g_dropped_allocs.exchange(0, std::memory_order_relaxed);
    const uint64_t overflow = g_overflow.exchange(0, std::memory_order_relaxed);

    bool moved = false;
    for (size_t i = 0; i < count; ++i) {
        if (rows[i].w_allocs || rows[i].w_frees) {
            moved = true;
            break;
        }
    }
    if (!moved && missed == 0 && dropped == 0 && overflow == 0) {
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        const PoolRow& r = rows[i];
        if (r.w_allocs == 0 && r.w_frees == 0) {
            continue; // one line per pool, only when something moved this window
        }
        char line[256];
        std::snprintf(line, sizeof line,
                      "SOTC ALLOC probe: %s (%s): +%llu allocs (%.2f MiB), -%llu frees (%.2f MiB) in "
                      "%.0f s; live %.2f MiB (peak %.2f MiB), %llu allocs total\n",
                      r.name[0] ? r.name : "?", KindName(r.kind),
                      static_cast<unsigned long long>(r.w_allocs), Mib(r.w_alloc_bytes),
                      static_cast<unsigned long long>(r.w_frees), Mib(r.w_free_bytes), seconds,
                      Mib(static_cast<uint64_t>(r.live_bytes)), Mib(r.peak_bytes),
                      static_cast<unsigned long long>(r.alloc_count));
        out += line;
    }
    if (missed || dropped || overflow) {
        char note[192];
        std::snprintf(note, sizeof note,
                      "SOTC ALLOC probe: %llu untracked frees, %llu dropped allocs (table full), "
                      "%llu nesting overflows in %.0f s\n",
                      static_cast<unsigned long long>(missed),
                      static_cast<unsigned long long>(dropped),
                      static_cast<unsigned long long>(overflow), seconds);
        out += note;
    }
}

} // namespace SotcProbes
