// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: texture LOD statistics (PM4 GET_LOD_STATS). A T# with LOD_HW_CNT_EN counts, in its
// counter bank (COUNTER_BANK_ID, 256 banks), how its texture was sampled; GET_LOD_STATS writes
// the banks to memory. Shadow of the Colossus streams the fine mips of its textures from these
// numbers: without them it never loads more than the 256x256 mip (blurry ground and rocks,
// black water, missing waterfall sheets). The port marks the banks of the T#s bound by draws
// and dispatches; GET_LOD_STATS reports a bank marked in the last reports as sampled (the
// hardware accumulates over many frames too: a texture missing from one frame must not read as
// unused, or the game drops mips it is still loading).
// The hardware knows the LOD each texture is sampled at; the port does not, so every sampled
// bank asks for the same mip. Asking for mip 0 everywhere fills the game's streaming pool and
// the game then breaks guest memory (heap and descriptor corruption within 1-4 minutes of the
// attract demo, also with mip 1); mip 2 (512x512 for the 2048 textures, 4x the old 256x256)
// ran the whole demo, but loading a save in the waterfall area then broke it too (3 of 4 runs,
// 2026-10-13) until the GPU readback on unmap was dropped (Rasterizer::UnmapMemory); mip 2 then
// ran 25+ minutes there. SOTC_LOD_LEVEL picks another mip for tests.
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include "common/types.h"
#include "video_core/sotc_live.h"

extern "C" const unsigned char* runtime_memory_backing_pointer(uintptr_t address);

namespace SotcLod {

inline std::atomic<u32> uses[256]{};

/// sotc: free share (percent) of the game's 64 KiB page pool in direct memory, or -1 when it is
/// not there. The pool object is what the game's page allocator gets in rdi (guest 0x1a660) and
/// the crash reports in r14: +0x50 the bitmap (set bit: free page), +0x58 its u64 words, +0x60
/// the pages. When no bit is left the allocator indexes the bitmap with -1 (read fault at guest
/// 0x1a371, #186). Read through the backing view, so the GPU's page protection does not matter.
inline int PoolFreePercent() {
    constexpr uintptr_t Pool = 0x1000000140; // in the game's first direct memory mapping
    const auto view = [](uintptr_t address, u64 size) -> const u8* {
        const u8* first = runtime_memory_backing_pointer(address);
        const u8* last = first ? runtime_memory_backing_pointer(address + size - 1) : nullptr;
        return last == first + size - 1 ? first : nullptr; // one contiguous view
    };
    const u8* pool = view(Pool, 0x68);
    if (!pool) {
        return -1;
    }
    u64 bitmap, words, pages;
    std::memcpy(&bitmap, pool + 0x50, 8);
    std::memcpy(&words, pool + 0x58, 8);
    std::memcpy(&pages, pool + 0x60, 8);
    const u8* bits = words && words <= 0x10000 ? view(bitmap, words * 8) : nullptr;
    if (!bits || pages == 0 || pages > words * 64) {
        return -1;
    }
    u64 free = 0;
    for (u64 i = 0; i < words; ++i) {
        u64 word;
        std::memcpy(&word, bits + i * 8, 8);
        free += std::popcount(word);
    }
    return static_cast<int>(std::min<u64>(free, pages) * 100 / pages);
}
/// Reports a bank stays sampled after its last use. 0, like Pink-shadPS4: only the banks used
/// since the last report. 180 (about 3 s) made the game ask for more than its streaming pool holds
/// (allocator fault at guest 0x1a371 in 1 of 3 runs of the attract demo; 0 of 5 without).
constexpr u32 Linger = 0;

// sotc: Mark (Rasterizer::BindTextures) and Collect (GET_LOD_STATS in Liverpool::ProcessGraphics)
// both run on the GpuCommandProcessor thread (async compute too: the ProcessCompute coroutines
// run there), so a relaxed load + store counts exactly like fetch_add without the lock xadd per
// streamed texture bound. std::atomic stays so the code remains well-defined if that changes.
inline void Mark(u32 bank) {
    auto& c = uses[bank & 0xff];
    c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

/// The 256 bank entries the game reads (u64 each): bits 56-59 the finest mip sampled (15: not
/// sampled), bits 32-55 how much (the game's streaming priority). Called once per report.
inline void Collect(u64 (&entries)[256]) {
    // SOTC_LOD_LINGER=<reports>: how long a bank stays sampled (test knob).
    static const u32 linger = [] {
        const char* v = std::getenv("SOTC_LOD_LINGER");
        return v ? static_cast<u32>(std::strtoul(v, nullptr, 0)) : Linger;
    }();
    // SOTC_LOD_LEVEL=<mip>: the finest mip reported for a sampled bank. Default 2: mip 0 (Pink-
    // shadPS4) filled an 8 GB card and stalled the GPU after a save loaded; mip 3 made the game
    // evict mips the GPU still sampled (garbage grass).
    static const u64 level = [] {
        const char* v = std::getenv("SOTC_LOD_LEVEL");
        return v ? std::strtoull(v, nullptr, 0) & 0xf : 2;
    }();
    // The in-game menu can switch high quality textures while playing (sotc_live.h).
    const int live = SotcLive::lod_linger.load(std::memory_order_relaxed);
    const u32 linger_now = live >= 0 ? static_cast<u32>(live) : linger;
    // sotc: every sampled bank asks for `level`, the distant ones too, where the hardware would
    // report a coarse mip. In PS4 Pro mode at the Shrine the game then streams more fine mips
    // than its page pool holds: about 10% free after loading, 0% some 40 s later and the fault
    // at 0x1a371 (#186, 5696 and 9152 MiB pools alike). Below SOTC_LOD_POOL_LOW percent free
    // (default 8; 0 turns this off) sampled banks ask for one mip coarser, below half of it two,
    // so the game drops fine mips (checked every 30 reports).
    static const int pool_low = [] {
        const char* v = std::getenv("SOTC_LOD_POOL_LOW");
        return v ? std::atoi(v) : 8;
    }();
    static u64 coarser = 0;
    static u32 reports = 0;
    if (linger_now && pool_low > 0 && reports++ % 30 == 0) {
        const int free = PoolFreePercent();
        // sotc: with hysteresis. The pool hovers at 0-14% free in large areas (9152 MiB), and
        // switching at one threshold changed the mip every second or two (4, 3, 2, 3, 2...): each
        // switch makes the game drop or stream fine mips while they are drawn, and the ending
        // showed cyan, speckled blocks on carved stone. Coarser below pool_low (two levels below
        // pool_low * 3 / 8), finer again only above twice pool_low (one level back above
        // pool_low * 3 / 4).
        u64 want = coarser;
        if (free >= 0) {
            if (free < pool_low * 3 / 8) {
                want = 2;
            } else if (free < pool_low) {
                want = std::max<u64>(coarser, 1);
            } else if (coarser == 2 && free > pool_low * 3 / 4) {
                want = 1;
            }
            if (want == 1 && coarser == 1 && free > pool_low * 2) {
                want = 0;
            }
        }
        if (want != coarser) {
            coarser = want;
            std::fprintf(stderr, "SOTC LOD: game page pool %d%% free, sampled textures ask for mip %u\n",
                         free, static_cast<unsigned>(level + coarser));
        }
    }
    const u64 mip = std::min<u64>(level + coarser, 14);
    static u32 idle[256] = {}; // reports since the bank's last use (never used: all ones)
    static const bool idle_ready = [] {
        std::fill(std::begin(idle), std::end(idle), 0xffffffffu);
        return true;
    }();
    (void)idle_ready;
    static u32 last_count[256]{};
    for (u32 i = 0; i < 256; ++i) {
        const u32 n = uses[i].load(std::memory_order_relaxed); // sotc: single thread, see Mark
        if (n) {
            uses[i].store(0, std::memory_order_relaxed);
            idle[i] = 0;
            last_count[i] = std::min<u32>(n * 256u, 0xffffff);
        } else if (idle[i] < linger_now) {
            ++idle[i];
        }
        // Banks not seen lately read as not sampled (15), as on the hardware: the game may then
        // drop their fine mips. SOTC_LOD_KEEP=1 reports them as sampled with no samples instead
        // (what the zeroed buffer said before; not soak-tested with the streaming).
        static const bool keep = std::getenv("SOTC_LOD_KEEP") != nullptr;
        entries[i] = idle[i] < linger_now ? (mip << 56) | (u64{std::max<u32>(last_count[i], 1)} << 32)
                     : keep           ? level << 56
                                      : u64{0xf} << 56;
    }
}

} // namespace SotcLod
