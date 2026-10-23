// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <utility>
#include <vector>

#include "common/types.h"

namespace Core {
class MemoryManager;
}

namespace VideoCore {

/**
 * Per-game guard for Shadow of the Colossus (GPU.bpe_heap_guard_address, 0 = off)
 *
 * The BPE heap stores a 16-byte header before each block and tree links inside free blocks
 * Stale compute descriptors can write to freed blocks, and readbacks can then overwrite that metadata
 * This caused guest access violations at eboot+0x48ac5/0x48f3d/0x490a0/0x49476
 *
 * Readbacks preserve the CPU's allocator metadata, which is never valid GPU output
 * The guard reads it through physical backing to bypass tracking protection
 */
class BpeHeapGuard {
public:
    BpeHeapGuard(Core::MemoryManager* memory, VAddr heap);

    bool IsValid() const {
        return valid;
    }

    /// Writes `size` bytes of `data` to guest `addr`, skipping allocator metadata. Returns the number of
    /// skipped bytes whose GPU value differed from the CPU value (0 when harmless)
    u64 Write(VAddr addr, const u8* data, u64 size);

    /// sotc: from the lowest to the highest free block's metadata found by the walk ({0, 0}: none).
    /// The heaps a world creates grow in 1 MiB pieces spread over direct memory, so this, not
    /// [heap+0x28] (a pointer all heaps share), tells where their metadata is.
    std::pair<VAddr, VAddr> FreeExtent() const {
        if (free_spans.empty()) {
            return {0, 0};
        }
        VAddr hi = 0;
        for (const auto& span : free_spans) {
            hi = std::max(hi, span.second);
        }
        return {free_spans.front().first, hi};
    }

    /// True when [addr, end) overlaps allocator metadata
    bool HasMetadata(VAddr addr, VAddr end) {
        return !CollectSpans(addr, end).empty();
    }

    /// The in-use block whose user area holds `addr` (its header address and the next block's), found by
    /// following the physical chain from the closest free block below it
    bool FindUsedBlock(VAddr addr, VAddr& block, VAddr& next, int* why = nullptr) const;

private:
    using Span = std::pair<VAddr, VAddr>;

    /// The spans of allocator metadata overlapping [addr, end), sorted
    std::vector<Span> CollectSpans(VAddr addr, VAddr end);

    bool Read(VAddr addr, void* out, u64 size) const;
    bool IsHeapPointer(VAddr addr) const;
    void AddFreeBlock(VAddr block);
    void CollectFreeTree();
    void CollectHeaders(VAddr lo, VAddr hi, std::vector<Span>& spans) const;
    bool NextBlock(VAddr block, VAddr& next) const;

    Core::MemoryManager* memory;
    VAddr heap;
    bool valid = false;
    std::vector<VAddr> free_blocks; // sorted block starts
    std::vector<Span> free_spans;   // sorted, header + tree links of every free block
    // Every backing run resolved during this guard's walk, sorted by start. The free-tree walk reads
    // thousands of nodes per readback; resolving each one through the VMA map was ~25 ms per SotC frame, and
    // the tree spreads over many physical areas
    struct Run {
        VAddr start;
        u64 size;
        const u8* ptr;
    };
    mutable std::vector<Run> runs;
};

/**
 * sotc: in-use heap blocks that readbacks write into. A write inside the user area of a block that is
 * still in use holds no allocator metadata (headers sit between blocks, tree links only in free
 * ones), so it needs no guard, whose build walks the whole free tree. The heap changes every frame,
 * which rebuilt the guard about once per frame (~6% of the draw thread in the Shrine). Each hit
 * re-reads the block's header and the next block's back link, so a block freed since is not trusted.
 */
class BpeUsedBlocks {
public:
    explicit BpeUsedBlocks(Core::MemoryManager* memory_) : memory{memory_} {}

    /// True when [addr, end) lies in the user area of a remembered block that is still in use
    bool Covers(VAddr heap, VAddr addr, VAddr end, int* why = nullptr);

    void Add(VAddr block, VAddr next);

    /// The in-use, size-0 block that ends the chain: the heap's tail after it holds no metadata
    void SetFence(VAddr block) {
        fence = block;
    }

    size_t Size() const {
        return blocks.size();
    }

private:
    bool StillUsed(VAddr heap, VAddr block, VAddr next) const;

    Core::MemoryManager* memory;
    std::vector<std::pair<VAddr, VAddr>> blocks; // sorted [header, next header)
    VAddr fence = 0;
};

} // namespace VideoCore
