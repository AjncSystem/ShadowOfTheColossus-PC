// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: exercise the production guard with synthetic backing, without guest files or launches.
// sotc: Build: clang++ -std=c++20 -Igpu/shim -Igpu/shadps4 tests/test_bpe_heap_guard.cpp
// sotc:        gpu/shadps4/video_core/buffer_cache/bpe_heap_guard.cpp -o out/bpe-heap-guard-test.exe
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "core/memory.h"
#include "video_core/buffer_cache/bpe_heap_guard.h"

namespace {
struct Region {
    VAddr addr;
    std::vector<u8> bytes;
};
std::vector<Region> regions;
u8* backing(VAddr addr, u64 size) {
    for (auto& r : regions) {
        if (addr >= r.addr && addr - r.addr <= r.bytes.size() &&
            size <= r.bytes.size() - (addr - r.addr)) {
            return r.bytes.data() + (addr - r.addr);
        }
    }
    return nullptr;
}
void word(VAddr addr, u64 value) {
    assert(backing(addr, 8));
    std::memcpy(backing(addr, 8), &value, 8);
}
void free_block(VAddr block, VAddr heap) {
    word(block + 8, 0x100);
    word(block + 0x10, heap + 0x80);
    word(block + 0x18, heap + 0x80);
    word(block + 0x20, block + 0x10);
    word(block + 0x28, block + 0x10);
    word(block + 0x110, block);
    word(block + 0x118, 1);
}
// sotc: a heap's extent list as the game builds it: sentinel heap+0x38 (prev +0x40, next +0x48), count u32
// +0x58, aggregate +0x30; node = [0] self, [8] prev, [0x10] next, [0x18] sentinel, [0x58] size, [0x60] base.
struct ExtentSpec {
    VAddr node, base, size;
};
void set_extents(VAddr heap, const std::vector<ExtentSpec>& list /* head first */) {
    const VAddr sentinel = heap + 0x38;
    u64 sum = 0;
    word(sentinel, 0);
    word(heap + 0x40, list.empty() ? sentinel : list.back().node);
    word(heap + 0x48, list.empty() ? sentinel : list.front().node);
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& e = list[i];
        word(e.node, e.node);
        word(e.node + 8, i == 0 ? sentinel : list[i - 1].node);
        word(e.node + 0x10, i + 1 == list.size() ? sentinel : list[i + 1].node);
        word(e.node + 0x18, sentinel);
        word(e.node + 0x58, e.size);
        word(e.node + 0x60, e.base);
        sum += e.size;
    }
    word(heap + 0x30, sum);
    word(heap + 0x58, list.size());
    // The chunk chain [heap+0xb8] -> [chunk+0] -> ...: the chunk of every extent, newest first.
    word(heap + 0xb8, list.empty() ? 0 : list.front().base);
    for (size_t i = 0; i < list.size(); ++i) {
        word(list[i].base, i + 1 < list.size() ? list[i + 1].base : 0);
    }
}
// A chunk handed to the heap from outside, as the game's AddChunk lays it out: pseudo header [0] older
// chunk, [8] = 3, first block at +0x10 (prev = chunk, size to the terminator), terminator block at
// end-0x10 (prev = the last block, size|flags = 1).
void make_chunk(VAddr chunk, u64 size, VAddr older) {
    word(chunk, older);
    word(chunk + 8, 3);
    word(chunk + 0x10, chunk);
    word(chunk + 0x18, size - 0x30);
    word(chunk + size - 0x10, chunk + 0x10);
    word(chunk + size - 8, 1);
}
} // namespace

const u8* Core::MemoryManager::GetBackingRun(VAddr addr, VAddr* start, u64* size) const {
    for (const auto& r : regions) {
        if (addr >= r.addr && addr - r.addr < r.bytes.size()) {
            *start = r.addr;
            *size = r.bytes.size();
            return r.bytes.data();
        }
    }
    return nullptr;
}
bool Core::MemoryManager::TryCopySparseMemory(VAddr addr, u8* out, u64 size) {
    const auto* p = backing(addr, size);
    if (!p) return false;
    std::memcpy(out, p, size);
    return true;
}
bool Core::MemoryManager::TryWriteBacking(void* addr, const void* in, u64 size, u32) {
    auto* p = backing(reinterpret_cast<VAddr>(addr), size);
    assert(p);
    std::memcpy(p, in, size);
    return true;
}

int main() {
    Core::MemoryManager memory;
    constexpr VAddr Heap = 0x1000000, Block = 0x1200000000ULL;
    std::array<VAddr, 10> heaps{}, blocks{};
    for (size_t i = 0; i < heaps.size(); ++i) {
        heaps[i] = Heap + i * 0x1000;
        blocks[i] = Block + i * 0x100000000ULL;
        regions.push_back({heaps[i], std::vector<u8>(0x1000)});
        regions.push_back({blocks[i], std::vector<u8>(0x1000)});
        word(heaps[i] + 0x28, Block); // shared base, far from most extensions
        word(heaps[i] + 0x30, 0x2000000);
        word(heaps[i] + 0x80, blocks[i] + 0x10);
        word(heaps[i] + 0xa8, 1);
        free_block(blocks[i], heaps[i]);
    }
    // sotc: two valid snapshots with the same root/count/pending can have different children.
    free_block(Block + 0x200, heaps[0]);
    word(Block + 0x10, Block + 0x210);
    word(heaps[0] + 0xa8, 2);
    // sotc: a different heap's pending metadata lies inside the first heap's address extent.
    word(heaps[1] + 0xc0, Block + 0x280);
    auto build = [&] {
        std::vector<VideoCore::BpeHeapGuard> guards;
        for (auto heap : heaps) guards.emplace_back(&memory, heap);
        return guards;
    };
    auto guards = build();
    // Duplicate/overlapping protection must be merged, and must not double-count bytes.
    guards.emplace_back(&memory, heaps[0]);
    std::array<u8, 0x130> gpu;
    gpu.fill(0xee);
    for (auto block : blocks) {
        std::array<u8, 0x130> before;
        std::memcpy(before.data(), backing(block, before.size()), before.size());
        const auto result = VideoCore::BpeHeapGuard::Write(&memory, guards, block, gpu.data(), gpu.size());
        assert(result.clipped == 0x48 && result.differing == 0x48);
        for (size_t j = 0; j < gpu.size(); ++j) {
            const bool metadata = j < 0x38 || (j >= 0x110 && j < 0x120);
            assert(backing(block, gpu.size())[j] == (metadata ? before[j] : 0xee));
        }
        const auto identical = VideoCore::BpeHeapGuard::Write(
            &memory, guards, block, backing(block, gpu.size()), gpu.size());
        assert(identical.clipped == 0x48 && identical.differing == 0);
    }
    // sotc: one copy intersects distinct heaps; neither guard may undo the other one's clipping.
    std::array<u8, 0xc0> original;
    std::memcpy(original.data(), backing(Block + 0x1f0, original.size()), original.size());
    const auto union_write = VideoCore::BpeHeapGuard::Write(
        &memory, guards, Block + 0x1f0, gpu.data(), original.size());
    assert(union_write.clipped == 0x68); // 0x38 at +0x200, 0x30 of +0x280
    assert(std::memcmp(backing(Block + 0x200, 0x38), original.data() + 0x10, 0x38) == 0);
    assert(std::memcmp(backing(Block + 0x280, 0x30), original.data() + 0x90, 0x30) == 0);
    // New child extension, without changing root/count/pending: a new batch must see it.
    constexpr VAddr Extension = 0x7000000000ULL;
    regions.push_back({Extension, std::vector<u8>(0x1000)});
    free_block(Extension, heaps[0]);
    word(blocks[0] + 0x10, Extension + 0x10);
    guards = build(); // fresh run pointers too: regions may have moved
    const auto grown = VideoCore::BpeHeapGuard::Write(&memory, guards, Extension, gpu.data(), 0x38);
    assert(grown.clipped == 0x38 && grown.differing == 0x38);
    assert(backing(Extension + 8, 1)[0] == 0); // size still 0x100, not GPU 0xee
    // The delayed-free block is protected even before it is linked into the tree.
    word(heaps[1] + 0xc0, Extension + 0x200);
    guards = build();
    const auto pending = VideoCore::BpeHeapGuard::Write(
        &memory, guards, Extension + 0x200, gpu.data(), 0x38);
    assert(pending.clipped == 0x38);
    // With no discovered heaps, ordinary readbacks still copy every byte.
    const auto plain = VideoCore::BpeHeapGuard::Write(&memory, {}, Extension + 0x400, gpu.data(), 0x38);
    assert(plain.clipped == 0 && std::memcmp(backing(Extension + 0x400, 0x38), gpu.data(), 0x38) == 0);
    // sotc: extent-list selection of the heaps touched by a batch. Heap with two extents far apart, the
    // second well beyond base+aggregate: only a copy overlapping a real extent selects it.
    {
        using VideoCore::BpeHeapGuard;
        using Extent = BpeHeapGuard::Extent;
        constexpr VAddr H = 0x1100000;
        constexpr VAddr E1 = 0x2000000000ULL, E2 = 0x2400000000ULL, E3 = 0x3000000000ULL;
        constexpr u64 Len = 0x100000;
        regions.push_back({H, std::vector<u8>(0x1000)});
        for (VAddr node : {0x7100000000ULL, 0x7100001000ULL, 0x7100002000ULL}) {
            regions.push_back({node, std::vector<u8>(0x1000)});
        }
        word(H + 0x28, E1);
        for (VAddr base : {E1, E2}) {
            regions.push_back({base, std::vector<u8>(0x1000)});
        }
        std::vector<Extent> scratch;
        const auto touched = [&](VAddr lo, VAddr hi) {
            const Extent copy{lo, hi};
            return BpeHeapGuard::HeapTouched(&memory, H, {&copy, 1}, scratch);
        };
        // Newest extent first, as the game links them.
        set_extents(H, {{0x7100001000ULL, E2, Len}, {0x7100000000ULL, E1, Len}});
        std::vector<Extent> got;
        int why = 0;
        assert(BpeHeapGuard::ReadExtents(&memory, H, got, &why) && got.size() == 2);
        assert(got[0].lo == E2 && got[0].hi == E2 + Len && got[1].lo == E1 && got[1].hi == E1 + Len);
        assert(touched(E1 + 0x10, E1 + 0x20));
        assert(touched(E2 + Len - 8, E2 + Len + 0x100));      // straddles the end of E2
        assert(touched(E2 - 0x100, E2 + 8));                  // straddles its start
        assert(!touched(E1 + Len, E1 + Len + 0x100));         // right after E1
        assert(!touched(E1 - 0x100, E1));                     // right before E1
        assert(!touched(E1 + 0x800000, E1 + 0x900000));       // the gap between the extents
        assert(!touched(E3, E3 + 0x1000));                    // far away
        // A heap grows a new extent between two batches: the copy that missed before is now selected,
        // and a guard built then preserves the metadata of the free block in the new extent.
        assert(!touched(E3 + 0x100, E3 + 0x200));
        regions.push_back({E3, std::vector<u8>(0x1000)});
        word(H + 0x80, E3 + 0x10);
        word(H + 0xa8, 1);
        free_block(E3, H);
        set_extents(H, {{0x7100002000ULL, E3, Len}, {0x7100001000ULL, E2, Len}, {0x7100000000ULL, E1, Len}});
        assert(touched(E3 + 0x100, E3 + 0x200));
        assert(!touched(E3 + Len, E3 + Len + 8));
        const Extent copy{E3, E3 + 0x38};
        std::vector<BpeHeapGuard> selected;
        if (BpeHeapGuard::HeapTouched(&memory, H, {&copy, 1}, scratch)) {
            selected.emplace_back(&memory, H); // aggregate 0x300000 < 16 MiB: still a valid, guarded heap
        }
        assert(selected.size() == 1 && selected[0].IsValid());
        const auto w = BpeHeapGuard::Write(&memory, selected, E3, gpu.data(), 0x38);
        assert(w.clipped == 0x38 && backing(E3 + 8, 1)[0] == 0);
        // Heaps whose list cannot be proven are never skipped.
        const auto broken = [&](auto&& damage) {
            set_extents(H, {{0x7100001000ULL, E2, Len}, {0x7100000000ULL, E1, Len}});
            damage();
            const bool ok = BpeHeapGuard::ReadExtents(&memory, H, got, &why);
            const bool skipped = !touched(E3 + 0x400000, E3 + 0x400100); // far from every extent
            return !ok && !skipped && why != 0;
        };
        assert(broken([&] { word(0x7100000000ULL + 8, 0x1234560); }));      // wrong prev link
        assert(broken([&] { word(0x7100001000ULL + 0x58, Len * 2); }));     // sizes disagree with +0x30
        assert(broken([&] { word(H + 0x58, 3); }));                         // count disagrees
        assert(broken([&] { word(0x7100001000ULL, 0x7100000000ULL); }));    // node is not self linked
        assert(broken([&] { word(0x7100001000ULL + 0x18, H + 0x40); }));    // foreign owner
        assert(broken([&] { word(H + 0x40, H + 0x38); }));                  // ring does not close on the last
        assert(broken([&] { word(0x7100000000ULL + 0x10, 0x7100001000ULL); })); // cycle, never reaches the sentinel
        assert(broken([&] { word(H + 0x48, 0); }));                         // no list at all
        // A spare, wholly free chunk is unchained by the allocator but stays in the ring (and in the
        // aggregate): its extent still counts.
        set_extents(H, {{0x7100001000ULL, E2, Len}, {0x7100000000ULL, E1, Len}});
        word(H + 0xb8, E1);
        word(E1, 0);
        assert(BpeHeapGuard::ReadExtents(&memory, H, got, &why) && got.size() == 2);
        assert(touched(E2 + 8, E2 + 16));
        // sotc: chunk handed over from outside (non-growable heap): no extent, the aggregate is its size and
        // its end is proven by the terminator block.
        constexpr VAddr C = 0x7200000000ULL;
        constexpr u64 CSize = 0x2000;
        regions.push_back({C, std::vector<u8>(CSize)});
        regions.push_back({0x7200100000ULL, std::vector<u8>(CSize)});
        const auto external = [&](u64 aggregate) {
            set_extents(H, {});
            make_chunk(C, CSize, 0);
            word(H + 0xb8, C);
            word(H + 0x30, aggregate);
            return BpeHeapGuard::ReadExtents(&memory, H, got, &why);
        };
        assert(external(CSize) && got.size() == 1 && got[0].lo == C && got[0].hi == C + CSize);
        assert(touched(C + CSize - 8, C + CSize + 8) && !touched(C + CSize, C + CSize + 8) &&
               !touched(C - 8, C));
        assert(!external(CSize + 0x100) && why == 7);        // aggregate too big: no terminator there
        assert(!external(CSize - 0x100) && why == 7);        // too small: that is mid-chunk
        external(CSize);
        word(C + CSize - 8, 0);                              // terminator damaged
        assert(!BpeHeapGuard::ReadExtents(&memory, H, got, &why) && why == 7);
        external(CSize);
        word(C + CSize - 0x10, C + 0x20);                    // predecessor that does not end there
        assert(!BpeHeapGuard::ReadExtents(&memory, H, got, &why) && why == 7);
        external(CSize);
        word(C + 8, 5);                                      // not a chunk header
        assert(!BpeHeapGuard::ReadExtents(&memory, H, got, &why) && why == 6);
        external(CSize);
        make_chunk(0x7200100000ULL, CSize, C);               // two external chunks: sizes unknown
        word(H + 0xb8, 0x7200100000ULL);
        assert(!BpeHeapGuard::ReadExtents(&memory, H, got, &why) && why == 6);
        // Extents plus one external chunk: both are heap memory.
        set_extents(H, {{0x7100000000ULL, E1, Len}});
        make_chunk(C, CSize, E1);
        word(H + 0xb8, C);
        word(H + 0x30, Len + CSize);
        assert(BpeHeapGuard::ReadExtents(&memory, H, got, &why) && got.size() == 2);
        assert(touched(C + 0x10, C + 0x20) && touched(E1, E1 + 8) && !touched(E2, E2 + 8));
        // sotc: a copy wholly inside the payload of a live block is proven harmless from the headers read
        // now, and needs no guard; anything else overlapping heap memory still does.
        set_extents(H, {{0x7100001000ULL, E2, Len}, {0x7100000000ULL, E1, Len}});
        word(H + 0xc0, 0);
        constexpr VAddr B = E1 + 0x100; // block: header [B, B+0x10), payload [B+0x10, B+0x210)
        word(B, B - 0x100);
        word(B + 8, 0x200 | 1);
        word(B + 0x210, B);             // the next block points back
        word(B + 0x218, 0x100 | 1);
        assert(!touched(B + 0x10, B + 0x210));
        assert(touched(B + 0x20, B + 0x30));                 // mid-block: no header proven right before it
        assert(touched(B + 0x10, B + 0x218));                // runs into the next header
        assert(touched(B + 0x8, B + 0x20));                  // starts inside the header
        word(B + 8, 0x200);                                  // freed: its payload carries the tree links
        assert(touched(B + 0x10, B + 0x210));
        word(B + 8, 0x200 | 1);
        word(H + 0xc0, B);                                   // the block being freed, not in the tree yet
        assert(touched(B + 0x10, B + 0x210));
        word(H + 0xc0, 0);
        word(B + 0x210, 0);                                  // successor does not point back: not a real block
        assert(touched(B + 0x10, B + 0x210));
        word(B + 0x210, B);
        assert(!touched(B + 0x10, B + 0x210));
        word(B + 8, 1);                                      // size 0, in use: a chain terminator
        assert(touched(B + 0x10, B + 0x18));
        word(B + 8, 0x200 | 1);
        {
            const std::array<Extent, 3> mixed{{{B + 0x10, B + 0x210}, {E3 + 0x40, E3 + 0x80}, {E1 + 0x800, E1 + 0x900}}};
            // E3 is outside this heap's extents now, the E1+0x800 range is heap memory without a block.
            assert(BpeHeapGuard::HeapTouched(&memory, H, mixed, scratch));
            assert(!BpeHeapGuard::HeapTouched(&memory, H, {mixed.data(), 2}, scratch));
        }
        // An empty heap (no extents, no aggregate) has nothing to protect.
        set_extents(H, {});
        assert(BpeHeapGuard::ReadExtents(&memory, H, got, &why) && got.empty());
        assert(!touched(E1, E1 + Len));
    }
    std::puts("BPE guard: extents (separated, grown, unproven, empty), 10 heaps, union, ABA, pending, counters: PASS");
}
