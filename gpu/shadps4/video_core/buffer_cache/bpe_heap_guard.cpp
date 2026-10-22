// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/memory.h"
#include "video_core/buffer_cache/bpe_heap_guard.h"

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
constexpr size_t MaxChainSteps = 1 << 12;
constexpr u64 InUse = 1; // size | flags bit 0: set on allocated blocks, clear on free ones
constexpr size_t MaxUsedBlocks = 1 << 12;
} // namespace

BpeHeapGuard::BpeHeapGuard(Core::MemoryManager* memory_, VAddr heap_)
    : memory{memory_}, heap{heap_} {
    u64 heap_size = 0;
    if (!Read(heap + 0x30, &heap_size, sizeof(heap_size)) || heap_size < 0x1000000 ||
        heap_size > MaxBlockSize) {
        return;
    }
    valid = true;
    CollectFreeTree();
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
    const VAddr sentinel = heap + 0x80;
    VAddr root = 0;
    Read(sentinel, &root, sizeof(root));
    VAddr pending = 0;
    if (Read(heap + 0xc0, &pending, sizeof(pending)) && IsHeapPointer(pending)) {
        AddFreeBlock(pending);
    }
    std::vector<VAddr> stack{root};
    size_t visited = 0;
    while (!stack.empty() && visited < MaxTreeNodes) {
        const VAddr node = stack.back();
        stack.pop_back();
        if (node == sentinel || !IsHeapPointer(node)) {
            continue;
        }
        std::array<u64, 7> words{};
        if (!Read(node - HeaderSize, words.data(), sizeof(words))) {
            continue;
        }
        ++visited;
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
    std::vector<VAddr> starts;
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

std::vector<BpeHeapGuard::Span> BpeHeapGuard::CollectSpans(VAddr addr, VAddr end) {
    std::vector<Span> spans;
    for (auto it = std::lower_bound(free_spans.begin(), free_spans.end(),
                                    Span{addr > FreeNodeSize ? addr - FreeNodeSize : 0, 0});
         it != free_spans.end() && it->first < end; ++it) {
        if (it->second > addr) {
            spans.push_back(*it);
        }
    }
    CollectHeaders(addr, end, spans);
    std::sort(spans.begin(), spans.end());
    return spans;
}

u64 BpeHeapGuard::Write(VAddr addr, const u8* data, u64 size) {
    const VAddr end = addr + size;
    const std::vector<Span> spans = CollectSpans(addr, end);

    u64 differing = 0;
    VAddr cursor = addr;
    const auto write = [&](VAddr lo, VAddr hi) {
        if (hi > lo) {
            memory->TryWriteBacking(std::bit_cast<void*>(lo), data + (lo - addr), hi - lo);
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
        if (Read(keep_lo, current.data(), len) &&
            std::memcmp(current.data(), data + (keep_lo - addr), len) != 0) {
            differing += len;
        }
        cursor = hi;
    }
    write(cursor, end);
    return differing;
}

bool BpeHeapGuard::FindUsedBlock(VAddr addr, VAddr& block, VAddr& next, int* why) const {
    int no_why = 0;
    why = why ? why : &no_why;
    const auto it = std::upper_bound(free_blocks.begin(), free_blocks.end(), addr);
    // why: 1 no free block below, 2 broken chain, 3 too many steps, 4 in a header, 5 free block,
    // 6 past the fence that ends the chain (`block` is the fence)
    if (it == free_blocks.begin()) {
        *why = 1;
        return false;
    }
    block = *std::prev(it);
    for (size_t step = 0; step < MaxChainSteps; ++step) {
        if (!NextBlock(block, next)) {
            // An in-use block of size 0 ends the chain: the heap's last 1 MiB ([heap+0xd0]) after it is
            // not split into blocks, and the culling readbacks land there
            u64 size_flags = 0;
            if (addr >= block + HeaderSize && Read(block + 8, &size_flags, sizeof(size_flags)) &&
                size_flags == InUse) {
                *why = 6;
                next = block + HeaderSize;
                return false;
            }
            *why = 2;
            return false;
        }
        if (addr < next) {
            u64 size_flags = 0;
            if (addr < block + HeaderSize) {
                *why = 4;
                return false;
            }
            if (!Read(block + 8, &size_flags, sizeof(size_flags)) || !(size_flags & InUse) ||
                std::binary_search(free_blocks.begin(), free_blocks.end(), block)) {
                *why = 5;
                return false;
            }
            return true;
        }
        block = next;
    }
    *why = 3;
    return false;
}

bool BpeUsedBlocks::StillUsed(VAddr heap, VAddr block, VAddr next) const {
    // The block being freed is kept at [heap+0xc0] before it joins the tree
    const VAddr pending = *std::bit_cast<const VAddr*>(heap + 0xc0);
    u64 size_flags = 0;
    VAddr prev = 0;
    return pending != block && pending != block + HeaderSize &&
           memory->TryReadBacking(block + 8, &size_flags, sizeof(size_flags)) &&
           (size_flags & InUse) && block + (size_flags & SizeMask) + HeaderSize == next &&
           memory->TryReadBacking(next, &prev, sizeof(prev)) && prev == block;
}

bool BpeUsedBlocks::Covers(VAddr heap, VAddr addr, VAddr end, int* why) {
    int no_why = 0;
    why = why ? why : &no_why;
    // Only the unsplit tail right after the fence ([heap+0xd0], 1 MiB) is free of metadata: the
    // world heaps span many 1 MiB pieces, and the free blocks of the later ones lie above it too
    u64 tail = *std::bit_cast<const u64*>(heap + 0xd0);
    if (tail < (64ULL << 10) || tail > (16ULL << 20)) {
        tail = 1ULL << 20;
    }
    if (fence && addr >= fence + HeaderSize && end <= fence + HeaderSize + tail) {
        u64 size_flags = 0;
        if (memory->TryReadBacking(fence + 8, &size_flags, sizeof(size_flags)) && size_flags == InUse) {
            return true;
        }
        fence = 0;
    }
    auto it = std::upper_bound(blocks.begin(), blocks.end(), std::pair{addr, ~VAddr{0}});
    // why: 1 no block remembered there, 2 the write leaves the block, 3 block no longer in use
    if (it == blocks.begin()) {
        *why = 1;
        return false;
    }
    --it;
    const auto [block, next] = *it;
    if (addr >= next) {
        *why = 1;
        return false;
    }
    if (addr < block + HeaderSize || end > next) {
        *why = 2;
        return false;
    }
    if (!StillUsed(heap, block, next)) {
        *why = 3;
        blocks.erase(it);
        return false;
    }
    return true;
}

void BpeUsedBlocks::Add(VAddr block, VAddr next) {
    if (blocks.size() >= MaxUsedBlocks) {
        blocks.clear();
    }
    const auto it = std::lower_bound(blocks.begin(), blocks.end(), std::pair{block, VAddr{0}});
    if (it != blocks.end() && it->first == block) {
        it->second = next;
    } else {
        blocks.insert(it, {block, next});
    }
}

} // namespace VideoCore
