// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
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
    /// sotc: reuse_scratch (BbToggle::BpeGuardDiff): the walk's scratch vectors (tree stack, header
    /// starts) are thread_local instead of allocated per call. Rebuild() re-targets a pooled guard.
    BpeHeapGuard(Core::MemoryManager* memory, VAddr heap, bool reuse_scratch = false);
    /// sotc: as a new guard for heap, keeping the capacity of this object's vectors. Nothing from
    /// the previous build survives (the free lists and the cached backing runs are cleared).
    void Rebuild(VAddr new_heap);

    bool IsValid() const {
        return valid;
    }
    /// Free-tree nodes visited by the walk of this guard (stats only)
    size_t TreeNodes() const {
        return tree_nodes;
    }

    // sotc: the heap's real extent list. A heap is a doubly linked list of memory extents (sentinel
    // at heap+0x38: prev heap+0x40, next heap+0x48, count u32 at heap+0x58, sum of sizes at
    // heap+0x30). Each node is a system descriptor: [0] self, [8] prev, [0x10] next, [0x18] owner
    // (the sentinel), [0x58] size, [0x60] base address; eboot FindExtent at +0x47ab0 walks it.
    struct Extent {
        VAddr lo;
        VAddr hi;
    };
    /// Reads the extent list of the heap into out. False when it cannot be proven well formed
    /// (broken links, a node that is not self owned, count or size sum that disagree with the
    /// heap's own fields, torn by a concurrent growth): the caller must then assume the heap
    /// can be anywhere. why (optional): 1 unreadable, 2 bad node, 3 broken ring, 4 count, 5 sum
    /// heap_in_place: the heap object is a global of the eboot (no backing view): its fields are read
    /// directly instead of asking the memory manager first.
    static bool ReadExtents(Core::MemoryManager* memory, VAddr heap, std::vector<Extent>& out,
                            int* why = nullptr, bool heap_in_place = false);
    /// One line (stderr) describing the heap's bookkeeping: fields, extents and the chunk chain
    /// ([heap+0xb8] -> [chunk+0] -> ...). Diagnostic only.
    static void Describe(Core::MemoryManager* memory, VAddr heap);
    /// Whether the heap needs a guard for these copies (all read fresh, nothing remembered): the
    /// extents cannot be proven (see ReadExtents), or some copy overlaps an extent of the heap
    /// without being proven to lie wholly in the payload of one live block (header right before the
    /// copy: in use, room for the copy, successor pointing back, not the block being freed at
    /// [heap+0xc0]). scratch is reused between calls.
    static bool HeapTouched(Core::MemoryManager* memory, VAddr heap, std::span<const Extent> copies,
                            std::vector<Extent>& scratch, int* why = nullptr,
                            bool heap_in_place = false);

    // sotc: one write filtered against all heaps at once. Sequential per-heap writes would
    // restore GPU bytes over metadata that an earlier heap's guard had preserved.
    struct WriteResult {
        u64 clipped = 0;
        u64 differing = 0;
    };
    /// sotc: sink (VERIFY only): when set, the bytes that would be written to guest memory go to
    /// sink[0, size) instead (a copy of the range's current bytes); guest memory is not touched.
    /// reuse_scratch: the span list is thread_local instead of allocated per call.
    static WriteResult Write(Core::MemoryManager* memory, std::span<BpeHeapGuard> guards,
                             VAddr addr, const u8* data, u64 size, u8* sink = nullptr,
                             bool reuse_scratch = false);

private:
    using Span = std::pair<VAddr, VAddr>;

    /// The spans of allocator metadata overlapping [addr, end), sorted
    std::vector<Span> CollectSpans(VAddr addr, VAddr end);
    /// sotc: appends the same spans, unsorted (Write sorts the union)
    void AppendSpans(VAddr addr, VAddr end, std::vector<Span>& spans);

    void Build(); // sotc: the constructor's walk, shared with Rebuild
    bool Read(VAddr addr, void* out, u64 size) const;
    bool IsHeapPointer(VAddr addr) const;
    void AddFreeBlock(VAddr block);
    void CollectFreeTree();
    void CollectHeaders(VAddr lo, VAddr hi, std::vector<Span>& spans) const;
    bool NextBlock(VAddr block, VAddr& next) const;

    Core::MemoryManager* memory;
    VAddr heap;
    bool valid = false;
    bool reuse = false; // sotc: thread_local scratch vectors in the walks
    size_t tree_nodes = 0;
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

} // namespace VideoCore
