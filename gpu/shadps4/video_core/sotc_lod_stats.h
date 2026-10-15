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
#include <cstdlib>
#include <iterator>
#include "common/types.h"

namespace SotcLod {

inline std::atomic<u32> uses[256]{};
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
        }
        if (n) {
            idle[i] = 0;
            last_count[i] = std::min<u32>(n * 256u, 0xffffff);
        } else if (idle[i] < linger) {
            ++idle[i];
        }
        // Banks not seen lately read as not sampled (15), as on the hardware: the game may then
        // drop their fine mips. SOTC_LOD_KEEP=1 reports them as sampled with no samples instead
        // (what the zeroed buffer said before; not soak-tested with the streaming).
        static const bool keep = std::getenv("SOTC_LOD_KEEP") != nullptr;
        entries[i] = idle[i] < linger ? (level << 56) | (u64{std::max<u32>(last_count[i], 1)} << 32)
                     : keep           ? level << 56
                                      : u64{0xf} << 56;
    }
}

} // namespace SotcLod
