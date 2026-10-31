// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/memory.h"
#include "video_core/buffer_cache/bpe_heap_guard.h"
#include "video_core/sotc_stats.h"

namespace VideoCore {

namespace {
// Block header: [0] previous physical block, [8] size | flags (bits 0-2). Free blocks carry tree links after
// the header: left, right, equal-size list (two links), parent | color
constexpr u64 HeaderSize = 0x10;
constexpr u64 FreeNodeSize = 0x38;
constexpr u64 SizeMask = ~u64{7};
// The heap hands out GPU-mapped direct memory above 4 GiB (seen up to ~0x4f6 GiB); anything outside user
// space is garbage. Reads through the backing reject unmapped addresses
constexpr VAddr HeapPointerLo = 0x100000000ULL;
constexpr VAddr HeapPointerHi = 0x10000000000ULL;
constexpr u64 MaxBlockSize = 0x100000000ULL;
constexpr size_t MaxTreeNodes = 1 << 16;
constexpr size_t MaxListNodes = 1 << 10;

// sotc: the free-tree walk's set of nodes and ring members seen (AjncSystem, upstream 0.70, 7fde988).
// Open addressing, on the draw thread, kept between builds.
class SeenSet {
public:
    void Reset(size_t expected) {
        size_t capacity = 1024;
        while (capacity < expected * 2) {
            capacity <<= 1;
        }
        if (table.size() != capacity) {
            table.assign(capacity, 0);
        } else {
            std::fill(table.begin(), table.end(), 0);
        }
        count = 0;
    }
    /// false when key (never 0) was already in
    bool Insert(u64 key) {
        if ((count + 1) * 2 > table.size()) {
            Grow();
        }
        const size_t mask = table.size() - 1;
        for (size_t i = (key * 0x9E3779B97F4A7C15ULL) >> 32 & mask;; i = (i + 1) & mask) {
            if (table[i] == key) {
                return false;
            }
            if (!table[i]) {
                table[i] = key;
                ++count;
                return true;
            }
        }
    }

private:
    void Grow() {
        std::vector<u64> old;
        old.swap(table);
        table.assign(old.size() * 2, 0);
        count = 0;
        for (const u64 key : old) {
            if (key) {
                Insert(key);
            }
        }
    }
    std::vector<u64> table;
    size_t count = 0;
};
thread_local SeenSet seen;
thread_local size_t last_seen = 0;
constexpr size_t MaxChainSteps = 1 << 12;
constexpr u64 InUse = 1; // size | flags bit 0: set on allocated blocks, clear on free ones
constexpr size_t MaxExtents = 1 << 12;
constexpr VAddr ExtentPointerLo = 0x10000;
constexpr VAddr ExtentPointerHi = 0x10000000000ULL;
constexpr u64 MaxExtentSize = 1ULL << 40;

// Guest read through the backing view (bypasses GPU page tracking); the heap object itself may be a
// global of the eboot with no backing: its fields are read in place.
bool ReadGuest(Core::MemoryManager* memory, VAddr heap, VAddr addr, void* out, u64 size,
               bool in_place = false) {
    const bool heap_field = addr >= heap && addr + size <= heap + 0x1000;
    if (!(in_place && heap_field) && memory->TryReadBacking(addr, out, size)) {
        return true;
    }
    if (heap_field) {
        std::memcpy(out, std::bit_cast<const void*>(addr), size);
        return true;
    }
    return false;
}
} // namespace

BpeHeapGuard::BpeHeapGuard(Core::MemoryManager* memory_, VAddr heap_, bool reuse_scratch)
    : memory{memory_}, heap{heap_}, reuse{reuse_scratch} {
    Build();
}

void BpeHeapGuard::Rebuild(VAddr new_heap) {
    heap = new_heap;
    valid = false;
    reuse = true;
    tree_nodes = 0;
    free_blocks.clear();
    free_spans.clear();
    runs.clear(); // backing pointers resolved for the previous build must not be trusted
    Build();
}

void BpeHeapGuard::Build() {
    // sotc: heaps of the class start small and grow by extents (the 16 MiB lower bound this had left a
    // young world heap unguarded until it grew past it): any non-empty aggregate is a live heap.
    u64 heap_size = 0;
    if (!Read(heap + 0x30, &heap_size, sizeof(heap_size)) || heap_size == 0 ||
        heap_size > MaxBlockSize) {
        return;
    }
    valid = true;
    CollectFreeTree();
}

namespace {
// Reads guest memory through the backing view, keeping the run (mapping) of the last read: the extent
// descriptors of a heap sit in a few pool pages, so a walk of hundreds of nodes resolves a run once.
struct RunReader {
    Core::MemoryManager* memory;
    VAddr heap;
    bool heap_in_place;
    VAddr start = 0;
    u64 size = 0;
    const u8* ptr = nullptr;

    bool Read(VAddr addr, void* out, u64 n) {
        const bool heap_field = addr >= heap && addr + n <= heap + 0x1000;
        if (heap_field && heap_in_place) {
            std::memcpy(out, std::bit_cast<const void*>(addr), n);
            return true;
        }
        if (ptr && addr >= start && addr - start + n <= size) {
            std::memcpy(out, ptr + (addr - start), n);
            return true;
        }
        VAddr run_start = 0;
        u64 run_size = 0;
        if (const u8* run = memory->GetBackingRun(addr, &run_start, &run_size)) {
            start = run_start;
            size = run_size;
            ptr = run;
            if (addr - start + n <= size) {
                std::memcpy(out, ptr + (addr - start), n);
                return true;
            }
            return memory->TryReadBacking(addr, out, n); // crosses into the next mapping
        }
        if (heap_field) { // the heap object is a global of the eboot: no backing view
            std::memcpy(out, std::bit_cast<const void*>(addr), n);
            return true;
        }
        return false;
    }
};
} // namespace

bool BpeHeapGuard::ReadExtents(Core::MemoryManager* memory, VAddr heap, std::vector<Extent>& out,
                               int* why, bool heap_in_place) {
    int no_why = 0;
    why = why ? why : &no_why;
    out.clear();
    RunReader reader{memory, heap, heap_in_place};
    const VAddr sentinel = heap + 0x38;
    // heap+0x30 aggregate, +0x38 sentinel link, +0x40 prev (last extent), +0x48 next (first), +0x50, +0x58 count
    std::array<u64, 6> head{};
    if (!reader.Read(heap + 0x30, head.data(), sizeof(head))) {
        *why = 1;
        return false;
    }
    u64 sum = 0;
    VAddr prev_link = sentinel;
    VAddr link = head[3];
    size_t n = 0;
    for (; link != sentinel; ++n) {
        if (n >= MaxExtents || link < ExtentPointerLo || link >= ExtentPointerHi || (link & 7)) {
            *why = 3;
            return false;
        }
        std::array<u64, 13> node{};
        if (!reader.Read(link, node.data(), sizeof(node))) {
            *why = 1;
            return false;
        }
        const u64 size = node[0xb];
        const VAddr base = node[0xc];
        if (node[0] != link || node[3] != sentinel || size == 0 || size > MaxExtentSize ||
            base < ExtentPointerLo || base + size < base || base + size > (1ULL << 48)) {
            *why = 2;
            return false;
        }
        if (node[1] != prev_link) {
            *why = 3;
            return false;
        }
        out.push_back({base, base + size});
        sum += size;
        prev_link = link;
        link = node[2];
    }
    // The ring must close on the last extent, and the heap's own bookkeeping must agree with the walk
    // (also catches a growth or shrink racing with it: the fields are read again below).
    if (head[2] != prev_link) {
        *why = 3;
        return false;
    }
    if (static_cast<u32>(head[5]) != n) {
        *why = 4;
        return false;
    }
    if (head[0] < sum) {
        *why = 5;
        return false;
    }
    // The aggregate counts every byte given to the heap: the ring's extents (growth) plus chunks handed
    // over from outside (the non-growable heaps, AddChunk at eboot+0x48b70), which have no extent. Memory
    // enters a heap as chunks chained through [heap+0xb8] (newest first, [chunk+0] = the older one); a
    // growth chunk is the base of an extent (a spare, wholly free one may be unchained but stays in
    // the ring). When the aggregate has more than the ring, the rest must be exactly one external
    // chunk found in that chain, whose end is proven by the block chain's terminator (an in-use
    // block of size 0 whose predecessor ends right there).
    const u64 extra = head[0] - sum;
    VAddr chunk_head = 0;
    if (extra != 0) {
        if (!reader.Read(heap + 0xb8, &chunk_head, sizeof(chunk_head))) {
            *why = 1;
            return false;
        }
        thread_local std::vector<VAddr> bases;
        bases.clear();
        for (const Extent& extent : out) {
            bases.push_back(extent.lo);
        }
        std::sort(bases.begin(), bases.end());
        VAddr external = 0;
        size_t steps = 0;
        for (VAddr chunk = chunk_head; chunk != 0; ++steps) {
            if (steps > n + 1 || chunk < ExtentPointerLo || chunk >= ExtentPointerHi || (chunk & 7)) {
                *why = 6;
                return false;
            }
            std::array<u64, 3> words{}; // [0] older chunk, [8] the chunk's own header, [0x10] first block's prev
            if (!reader.Read(chunk, words.data(), sizeof(words))) {
                *why = 1;
                return false;
            }
            if (!std::binary_search(bases.begin(), bases.end(), chunk)) {
                if (external != 0 || words[1] != 3 || words[2] != chunk) {
                    *why = 6;
                    return false;
                }
                external = chunk;
            }
            chunk = words[0];
        }
        if (external == 0 || extra < 0x40 || extra > MaxExtentSize || (extra & 7) ||
            external + extra < external || external + extra > (1ULL << 48)) {
            *why = 5;
            return false;
        }
        const VAddr end = external + extra;
        std::array<u64, 2> fence{}; // [end-0x10] predecessor, [end-8] size|flags
        u64 pred_size = 0;
        VAddr pred = 0;
        if (!reader.Read(end - 0x10, fence.data(), sizeof(fence)) || fence[1] != 1 ||
            (pred = fence[0]) < external + 0x10 || pred >= end - 0x10 || (pred & 7) ||
            !reader.Read(pred + 8, &pred_size, sizeof(pred_size)) ||
            pred + 0x10 + (pred_size & SizeMask) != end - 0x10) {
            *why = 7;
            return false;
        }
        out.push_back({external, end});
    }
    std::array<u64, 6> again{};
    VAddr chunk_again = chunk_head;
    if (!reader.Read(heap + 0x30, again.data(), sizeof(again)) ||
        (extra != 0 && !reader.Read(heap + 0xb8, &chunk_again, sizeof(chunk_again))) ||
        again[0] != head[0] || again[2] != head[2] || again[3] != head[3] || again[5] != head[5] ||
        chunk_again != chunk_head) {
        *why = 3;
        return false;
    }
    return true;
}

void BpeHeapGuard::Describe(Core::MemoryManager* memory, VAddr heap) {
    std::array<u64, 0x1b> obj{}; // heap+0 .. heap+0xd0
    if (!ReadGuest(memory, heap, heap, obj.data(), sizeof(obj))) {
        std::fprintf(stderr, "SOTC BPE heap %#llx: unreadable\n", static_cast<unsigned long long>(heap));
        return;
    }
    std::vector<Extent> extents;
    int why = 0;
    const bool ok = ReadExtents(memory, heap, extents, &why);
    u64 sum = 0;
    size_t ring = 0;
    std::vector<VAddr> bases;
    std::string text;
    char buf[200];
    // Extents by hand (ReadExtents drops them when unproven): walk the ring again, loosely.
    VAddr link = obj[9];
    for (; ring < 4096 && link != heap + 0x38 && link >= ExtentPointerLo; ++ring) {
        std::array<u64, 13> node{};
        if (!ReadGuest(memory, heap, link, node.data(), sizeof(node))) {
            break;
        }
        if (ring < 6) {
            std::snprintf(buf, sizeof(buf), " [%#llx +%#llx]", static_cast<unsigned long long>(node[0xc]),
                          static_cast<unsigned long long>(node[0xb]));
            text += buf;
        }
        bases.push_back(node[0xc]);
        sum += node[0xb];
        link = node[2];
    }
    std::sort(bases.begin(), bases.end());
    std::string chunks;
    VAddr chunk = obj[0x17]; // heap+0xb8
    size_t chain = 0, in_ring = 0;
    for (; chain < 4096 && chunk >= ExtentPointerLo && chunk < ExtentPointerHi; ++chain) {
        std::array<u64, 4> head{};
        if (!ReadGuest(memory, heap, chunk, head.data(), sizeof(head))) {
            if (chain < 8) {
                std::snprintf(buf, sizeof(buf), " %#llx(unreadable)", static_cast<unsigned long long>(chunk));
                chunks += buf;
            }
            break;
        }
        const bool is_extent = std::binary_search(bases.begin(), bases.end(), chunk);
        in_ring += is_extent;
        if (chain < 8 || !is_extent) {
            std::snprintf(buf, sizeof(buf), " %#llx(%s older %#llx w8 %#llx w10 %#llx first %#llx)",
                          static_cast<unsigned long long>(chunk), is_extent ? "ext" : "EXTERNAL",
                          static_cast<unsigned long long>(head[0]), static_cast<unsigned long long>(head[1]),
                          static_cast<unsigned long long>(head[2]),
                          static_cast<unsigned long long>(head[3] & SizeMask));
            chunks += buf;
        }
        chunk = head[0];
    }
    std::fprintf(stderr,
                 "SOTC BPE heap %#llx: vtbl %#llx flags %02x%02x base %#llx agg %#llx count %u "
                 "ring %zu sum %#llx | proven %d(reason %d) | chain %zu (in ring %zu) end %#llx | "
                 "extents%s | chunks%s | +d0 %#llx\n",
                 static_cast<unsigned long long>(heap), static_cast<unsigned long long>(obj[0]),
                 static_cast<unsigned>((obj[1] >> 8) & 0xff), static_cast<unsigned>(obj[1] & 0xff),
                 static_cast<unsigned long long>(obj[5]), static_cast<unsigned long long>(obj[6]),
                 static_cast<unsigned>(obj[0xb] & 0xffffffffu), ring, static_cast<unsigned long long>(sum),
                 ok ? 1 : 0, why, chain, in_ring, static_cast<unsigned long long>(chunk), text.c_str(),
                 chunks.c_str(), static_cast<unsigned long long>(obj[0x1a]));
}

bool BpeHeapGuard::HeapTouched(Core::MemoryManager* memory, VAddr heap,
                               std::span<const Extent> copies, std::vector<Extent>& scratch,
                               int* why, bool heap_in_place) {
    if (!ReadExtents(memory, heap, scratch, why, heap_in_place)) {
        return true;
    }
    RunReader reader{memory, heap, heap_in_place};
    VAddr pending = 0;
    bool pending_known = false;
    for (const Extent& copy : copies) {
        bool inside = false;
        for (const Extent& extent : scratch) {
            if (copy.lo < extent.hi && extent.lo < copy.hi) {
                inside = true;
                break;
            }
        }
        if (!inside) {
            continue;
        }
        // The copy overlaps heap memory. It is harmless only when it lies wholly in the payload of one
        // live block, proven now from the headers (nothing remembered): the block's header, right
        // before the copy, says in use with room for the copy, and the block that follows points back
        // to it. Metadata is only ever in headers and in free blocks, so none lies in such a payload.
        // The block being freed ([heap+0xc0], not in the tree yet) is excluded: its payload is about to
        // take the tree links.
        if (!pending_known) {
            if (!reader.Read(heap + 0xc0, &pending, sizeof(pending))) {
                return true;
            }
            pending_known = true;
        }
        const VAddr lo = copy.lo;
        const VAddr block = lo - HeaderSize;
        std::array<u64, 2> header{};
        if (lo < HeapPointerLo + HeaderSize || (lo & 7) || copy.hi <= lo ||
            !reader.Read(block, header.data(), sizeof(header))) {
            return true;
        }
        const u64 flags_size = header[1];
        const u64 payload = flags_size & SizeMask;
        if (!(flags_size & InUse) || payload == 0 || payload >= MaxBlockSize || copy.hi - lo > payload ||
            pending == block) {
            return true;
        }
        const VAddr next = lo + payload;
        VAddr next_prev = 0;
        if (next < HeapPointerLo || next >= HeapPointerHi || (next & 7) ||
            !reader.Read(next, &next_prev, sizeof(next_prev)) || next_prev != block) {
            return true;
        }
    }
    return false;
}

bool BpeHeapGuard::Read(VAddr addr, void* out, u64 size) const {
    auto it = std::upper_bound(runs.begin(), runs.end(), addr,
                               [](VAddr value, const Run& run) { return value < run.start; });
    if (it == runs.begin() || addr - std::prev(it)->start >= std::prev(it)->size) {
        Run run{};
        run.ptr = memory->GetBackingRun(addr, &run.start, &run.size);
        if (!run.ptr) {
            // sotc: the heap object itself is a global of the eboot, outside the guest memory
            // the runtime maps (no backing view): its fields are read in place.
            if (addr >= heap && addr + size <= heap + 0x1000) {
                std::memcpy(out, std::bit_cast<const void*>(addr), size);
                return true;
            }
            return false;
        }
        it = std::next(runs.insert(it, run));
    }
    const Run& run = *std::prev(it);
    if (addr - run.start + size > run.size) {
        // Crosses into the next physical area: take the general path
        return memory->TryReadBacking(addr, out, size);
    }
    std::memcpy(out, run.ptr + (addr - run.start), size);
    return true;
}

bool BpeHeapGuard::IsHeapPointer(VAddr addr) const {
    return addr >= HeapPointerLo && addr < HeapPointerHi && (addr & 7) == 0;
}

void BpeHeapGuard::AddFreeBlock(VAddr block) {
    free_blocks.push_back(block);
}

void BpeHeapGuard::CollectFreeTree() {
    // sotc: SOTC_GUARD_DEDUP=1 skips repeats (AjncSystem, upstream 0.70, 7fde988); off by default:
    // in the user's A/B (30 Oct, same walk, 1440p DLSS-G) every session with it on had 200-575 ms
    // frames with nothing on the GPU side (391 and 200 ms; 575 and 467 ms earlier), none without it
    // (plus27 174 ms worst, upload budget alone 133 ms). Cause not found. The game changes
    // the tree while this walk reads it: at an area change a node reached twice (a link read
    // mid-change) was walked again and again up to MaxTreeNodes, each time with its ring of up to
    // MaxListNodes members, and the sort of those repeats held the draw thread 1-3 s. Nodes and ring
    // members already seen are skipped: they only added repeats, which sort/unique removed anyway.
    static const bool dedup = [] {
        const char* v = std::getenv("SOTC_GUARD_DEDUP");
        return v && v[0] == '1';
    }();
    if (dedup) {
        seen.Reset(last_seen);
    }
    size_t seen_keys = 0;
    u64 repeats = 0;
    const VAddr sentinel = heap + 0x80;
    VAddr root = 0;
    Read(sentinel, &root, sizeof(root));
    VAddr pending = 0;
    if (Read(heap + 0xc0, &pending, sizeof(pending)) && IsHeapPointer(pending)) {
        AddFreeBlock(pending);
    }
    thread_local std::vector<VAddr> pooled_stack;
    std::vector<VAddr> local_stack;
    std::vector<VAddr>& stack = reuse ? pooled_stack : local_stack;
    stack.clear();
    stack.push_back(root);
    size_t visited = 0;
    while (!stack.empty() && visited < MaxTreeNodes) {
        const VAddr node = stack.back();
        stack.pop_back();
        if (node == sentinel || !IsHeapPointer(node)) {
            continue;
        }
        if (dedup && (++seen_keys, !seen.Insert(node))) {
            ++repeats;
            continue;
        }
        std::array<u64, 7> words{};
        if (!Read(node - HeaderSize, words.data(), sizeof(words))) {
            continue;
        }
        ++visited;
        tree_nodes = visited;
        const u64 key = words[1] & SizeMask;
        if (key == 0 || key >= MaxBlockSize) {
            // Already corrupted: still keep what is left of it, but do not follow its links
            AddFreeBlock(node - HeaderSize);
            continue;
        }
        AddFreeBlock(node - HeaderSize);
        // Blocks of equal size hang off the tree node in a ring through the list links
        VAddr member = words[4];
        for (size_t i = 0; i < MaxListNodes && IsHeapPointer(member) && member != node; ++i) {
            // members tagged in bit 0 (pointers are 8-aligned), apart from tree nodes
            if (dedup && (++seen_keys, !seen.Insert(member | 1))) {
                ++repeats;
                break;
            }
            AddFreeBlock(member - HeaderSize);
            VAddr next = 0;
            if (!Read(member + 0x10, &next, sizeof(next))) {
                break;
            }
            member = next;
        }
        stack.push_back(words[2]);
        stack.push_back(words[3]);
    }
    last_seen = seen_keys;
    if (repeats) {
        SotcStats::heap_guard_repeats.fetch_add(repeats, std::memory_order_relaxed);
    }
    std::sort(free_blocks.begin(), free_blocks.end());
    free_blocks.erase(std::unique(free_blocks.begin(), free_blocks.end()), free_blocks.end());
    free_spans.reserve(free_blocks.size());
    for (const VAddr block : free_blocks) {
        free_spans.emplace_back(block, block + FreeNodeSize);
    }
}

bool BpeHeapGuard::NextBlock(VAddr block, VAddr& next) const {
    u64 size_flags = 0;
    if (!Read(block + 8, &size_flags, sizeof(size_flags))) {
        return false;
    }
    const u64 size = size_flags & SizeMask;
    if (size == 0 || size >= MaxBlockSize) {
        return false;
    }
    next = block + size + HeaderSize;
    // The next block must point back: this rejects anything that only looks like a header
    VAddr prev = 0;
    return IsHeapPointer(next) && Read(next, &prev, sizeof(prev)) && prev == block;
}

void BpeHeapGuard::CollectHeaders(VAddr lo, VAddr hi, std::vector<Span>& spans) const {
    // Start from the closest free block at or below the range, whose header is known to be real, and follow
    // the physical chain through the range
    auto it = std::upper_bound(free_blocks.begin(), free_blocks.end(), lo);
    thread_local std::vector<VAddr> pooled_starts;
    std::vector<VAddr> local_starts;
    std::vector<VAddr>& starts = reuse ? pooled_starts : local_starts;
    starts.clear();
    // Readbacks usually start at a binding base, i.e. the user pointer of an allocation: start from its
    // header too (accepted only if the next block points back to it)
    if (VAddr next = 0; lo > HeaderSize && NextBlock(lo - HeaderSize, next)) {
        starts.push_back(lo - HeaderSize);
    }
    if (it != free_blocks.begin()) {
        starts.push_back(*std::prev(it));
    }
    for (; it != free_blocks.end() && *it < hi; ++it) {
        starts.push_back(*it);
    }
    std::sort(starts.begin(), starts.end());
    VAddr covered_until = 0;
    for (VAddr block : starts) {
        if (block < covered_until) {
            continue;
        }
        for (size_t step = 0; step < MaxChainSteps && block < hi; ++step) {
            if (block + HeaderSize > lo) {
                spans.emplace_back(block, block + HeaderSize);
            }
            VAddr next = 0;
            if (!NextBlock(block, next)) {
                break;
            }
            block = next;
        }
        covered_until = block;
    }
}

void BpeHeapGuard::AppendSpans(VAddr addr, VAddr end, std::vector<Span>& spans) {
    for (auto it = std::lower_bound(free_spans.begin(), free_spans.end(),
                                    Span{addr > FreeNodeSize ? addr - FreeNodeSize : 0, 0});
         it != free_spans.end() && it->first < end; ++it) {
        if (it->second > addr) {
            spans.push_back(*it);
        }
    }
    CollectHeaders(addr, end, spans);
}

std::vector<BpeHeapGuard::Span> BpeHeapGuard::CollectSpans(VAddr addr, VAddr end) {
    std::vector<Span> spans;
    AppendSpans(addr, end, spans);
    std::sort(spans.begin(), spans.end());
    return spans;
}

BpeHeapGuard::WriteResult BpeHeapGuard::Write(Core::MemoryManager* memory,
                                           std::span<BpeHeapGuard> guards, VAddr addr,
                                           const u8* data, u64 size, u8* sink,
                                           bool reuse_scratch) {
    const VAddr end = addr + size;
    // sotc: union metadata before writing any bytes; ranges from different heaps can overlap.
    thread_local std::vector<Span> pooled_spans;
    std::vector<Span> local_spans;
    std::vector<Span>& spans = reuse_scratch ? pooled_spans : local_spans;
    spans.clear();
    for (auto& guard : guards) {
        if (reuse_scratch) {
            guard.AppendSpans(addr, end, spans);
        } else {
            const auto more = guard.CollectSpans(addr, end);
            spans.insert(spans.end(), more.begin(), more.end());
        }
    }
    std::sort(spans.begin(), spans.end());

    WriteResult result;
    VAddr cursor = addr;
    const auto write = [&](VAddr lo, VAddr hi) {
        if (hi > lo) {
            if (sink) {
                std::memcpy(sink + (lo - addr), data + (lo - addr), hi - lo); // VERIFY only
            } else {
                memory->TryWriteBacking(std::bit_cast<void*>(lo), data + (lo - addr), hi - lo, 1);
            }
        }
    };
    for (const auto& [span_lo, span_hi] : spans) {
        const VAddr lo = std::max(span_lo, addr);
        const VAddr hi = std::min(span_hi, end);
        if (hi <= lo || hi <= cursor) {
            continue;
        }
        const VAddr keep_lo = std::max(lo, cursor);
        write(cursor, keep_lo);
        std::array<u8, FreeNodeSize> current{};
        const u64 len = hi - keep_lo;
        result.clipped += len;
        if (memory->TryReadBacking(keep_lo, current.data(), len) &&
            std::memcmp(current.data(), data + (keep_lo - addr), len) != 0) {
            result.differing += len;
        }
        cursor = hi;
    }
    write(cursor, end);
    return result;
}

} // namespace VideoCore
