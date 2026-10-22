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
#include "video_core/sotc_event.h"
#include "video_core/sotc_live.h"
#include "video_core/sotc_ui_lang.h"

extern "C" const unsigned char* runtime_memory_backing_pointer(uintptr_t address);

namespace SotcLod {

inline std::atomic<u32> uses[256]{};

/// sotc: the finest mip + 1 each bank was sampled at since the last report, measured by the
/// fragment shaders (Rasterizer::SotcLodFeedbackCycle; 0: no sample). The hardware counts
/// exactly this; without it every sampled bank asked for one fixed mip, distant ones too, and
/// with texture level "Maximum" in PS4 Pro mode the game's page pool stayed full until the game
/// overwrote textures still in use (colour garbage on the Shrine after some loads, 22 Oct; the
/// F9 reload from guest memory left it as it was).
inline std::atomic<u32> gpu_mip[256]{};

/// Merges one host copy of the feedback buffer (u32 per bank, ~0u: not sampled).
inline void MergeGpu(const u32* banks) {
    for (u32 i = 0; i < 256; ++i) {
        if (banks[i] > 14) {
            continue;
        }
        const u32 want = banks[i] + 1;
        u32 cur = gpu_mip[i].load(std::memory_order_relaxed);
        while ((cur == 0 || want < cur) &&
               !gpu_mip[i].compare_exchange_weak(cur, want, std::memory_order_relaxed)) {
        }
    }
}

struct PoolPages {
    s64 free = -1; ///< -1: the pool is not there
    u64 total = 0;
};

/// sotc: free and total pages of the game's 64 KiB page pool in direct memory. The pool object is what the game's page allocator gets in rdi (guest 0x1a660) and
/// the crash reports in r14: +0x50 the bitmap (set bit: free page), +0x58 its u64 words, +0x60
/// the pages. When no bit is left the allocator indexes the bitmap with -1 (read fault at guest
/// 0x1a371, #186). Read through the backing view, so the GPU's page protection does not matter.
inline PoolPages PoolFree() {
    constexpr uintptr_t Pool = 0x1000000140; // in the game's first direct memory mapping
    const auto view = [](uintptr_t address, u64 size) -> const u8* {
        const u8* first = runtime_memory_backing_pointer(address);
        const u8* last = first ? runtime_memory_backing_pointer(address + size - 1) : nullptr;
        return last == first + size - 1 ? first : nullptr; // one contiguous view
    };
    const u8* pool = view(Pool, 0x68);
    if (!pool) {
        return {};
    }
    u64 bitmap, words, pages;
    std::memcpy(&bitmap, pool + 0x50, 8);
    std::memcpy(&words, pool + 0x58, 8);
    std::memcpy(&pages, pool + 0x60, 8);
    const u8* bits = words && words <= 0x10000 ? view(bitmap, words * 8) : nullptr;
    if (!bits || pages == 0 || pages > words * 64) {
        return {};
    }
    u64 free = 0;
    for (u64 i = 0; i < words; ++i) {
        u64 word;
        std::memcpy(&word, bits + i * 8, 8);
        free += std::popcount(word);
    }
    return {static_cast<s64>(std::min<u64>(free, pages)), pages};
}

/// Free share (percent) of the page pool, or -1 when it is not there
inline int PoolFreePercent() {
    const PoolPages p = PoolFree();
    return p.free < 0 ? -1 : static_cast<int>(p.free * 100 / static_cast<s64>(p.total));
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
    // Measured on 21 Oct (SOTC_LOD_POOL_LOG, switch off, PS4 Pro, 9152 MiB, 5 min attract): the
    // pool (146416 pages) sits at 0-15 free pages for minutes, 0 at times, without a fault. The
    // allocator first lets the game release pages (call 0x1abf0 from 0x1a2f0) and only faults
    // when nothing could be released and no bit is set; so a low free count is the game's normal
    // state and this switch, at any threshold, keeps sampled textures coarser in large areas. The
    // signal it needs is a failed release; that waits for a repro of the fault.
    static const int env_pool_low = [] {
        const char* v = std::getenv("SOTC_LOD_POOL_LOW");
        return v ? std::atoi(v) : 8;
    }();
    // The texture level (launcher, in-game menu): "High" keeps the switch, "Maximum" sets 0.
    const int live_low = SotcLive::lod_pool_low.load(std::memory_order_relaxed);
    const int pool_low = live_low >= 0 ? live_low : env_pool_low;
    static u32 last_linger = ~0u;
    static int last_pool_low = -1;
    if (linger_now != last_linger || pool_low != last_pool_low) {
        last_linger = linger_now;
        last_pool_low = pool_low;
        SotcEvent::Log("TEXTURE", SotcUi::T("Nível de textura em uso: %s", "Texture level in use: %s"),
                       !linger_now ? SotcUi::T("Desligada", "Off")
                       : pool_low > 0 ? SotcUi::T("Alta", "High") : SotcUi::T("Máxima", "Maximum"));
        std::fprintf(stderr, "SOTC LOD: texture level %s\n",
                     !linger_now ? "off" : pool_low > 0 ? "high" : "maximum");
    }
    static u64 coarser = 0;
    static u32 reports = 0;
    // SOTC_LOD_POOL_LOG=1: the pool's free share at every check (measuring texture budgets).
    static const bool pool_log = std::getenv("SOTC_LOD_POOL_LOG") != nullptr;
    if (pool_log && reports % 30 == 0) {
        const PoolPages p = PoolFree();
        std::fprintf(stderr, "SOTC LOD POOL: %d%% free (%lld of %llu pages)\n", PoolFreePercent(),
                     static_cast<long long>(p.free), static_cast<unsigned long long>(p.total));
    }
    if (pool_log && (!linger_now || pool_low <= 0)) {
        ++reports;
    }
    if (pool_low <= 0 && coarser != 0) { // switched to "Maximum" while coarser
        coarser = 0;
        std::fprintf(stderr, "SOTC LOD: page pool switch off, sampled textures ask for mip %u\n",
                     static_cast<unsigned>(level));
    }
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
            } else if (coarser == 2) {
                want = 1;
            }
            if (want == 1 && coarser == 1 && free > pool_low * 2) {
                want = 0;
            }
        }
        if (want != coarser) {
            coarser = want;
            SotcEvent::Log("TEXTURE",
                           SotcUi::T("Memória de texturas do jogo com %d%% livre: detalhe das texturas vistas no nível %u (2 = o mais fino)",
                                     "Game texture memory %d%% free: seen textures at detail level %u (2 = finest)"),
                           free, static_cast<unsigned>(level + want));
            std::fprintf(stderr, "SOTC LOD: game page pool %d%% free, sampled textures ask for mip %u\n",
                         free, static_cast<unsigned>(level + coarser));
        }
    }
    const u64 mip = std::min<u64>(level + coarser, 14);
    // sotc: the measured mips (gpu_mip). Without SOTC_LOD_FEEDBACK=1: the fixed mip above for every
    // sampled bank. SOTC_LOD_FB_BIAS=<n> asks n mips coarser (negative: finer), SOTC_LOD_FB_MIN
    // the finest mip ever asked for. A bank keeps its last measured mip for SOTC_LOD_FB_KEEP
    // reports (default 60) after its last sample; the page pool switch still adds `coarser`.
    static const bool feedback = [] { // Shader::LodFeedbackEnabled: off unless =1
        const char* v = std::getenv("SOTC_LOD_FEEDBACK");
        return v && v[0] == '1';
    }();
    static const int fb_bias = [] {
        const char* v = std::getenv("SOTC_LOD_FB_BIAS");
        return v ? std::atoi(v) : 0;
    }();
    // Default: the fixed mip (level). Measured near textures asking for mip 0 made the game
    // stream far more than its page pool and the disk give (9 FPS, stalls of 7-9 s and the
    // allocator fault at 0x1a371 within 6 minutes, PS4 Pro + Maximum, 22 Oct): near textures keep
    // the old detail and only the distant ones ask for less.
    static const int fb_min = [] {
        const char* v = std::getenv("SOTC_LOD_FB_MIN");
        return v ? std::atoi(v) : int(level);
    }();
    static const u32 fb_keep = [] {
        const char* v = std::getenv("SOTC_LOD_FB_KEEP");
        return v ? static_cast<u32>(std::strtoul(v, nullptr, 0)) : 60u;
    }();
    // SOTC_LOD_FB_HOLD=<reports> (default 120, about 2 s): a bank asks for a finer mip at once
    // but for a coarser one only after measuring it that long. Measured mips move between
    // neighbours from one report to the next (a texture at the edge of a mip boundary), and every
    // switch makes the game drop or stream mips: stalls down to 0.1 FPS without this (22 Oct).
    static const u32 fb_hold = [] {
        const char* v = std::getenv("SOTC_LOD_FB_HOLD");
        return v ? static_cast<u32>(std::strtoul(v, nullptr, 0)) : 120u;
    }();
    // SOTC_LOD_FB_USE=0: measure (and log) without changing what the game reads (shader cost).
    static const bool fb_use = [] {
        const char* v = std::getenv("SOTC_LOD_FB_USE");
        return !v || v[0] != '0';
    }();
    static u8 gpu_last[256];
    static u32 gpu_age[256];
    static u32 gpu_coarse_for[256];
    static const bool gpu_ready = [] {
        std::fill(std::begin(gpu_last), std::end(gpu_last), u8{0xff});
        std::fill(std::begin(gpu_age), std::end(gpu_age), 0xffffffffu);
        return true;
    }();
    (void)gpu_ready;
    u32 measured = 0, mip_hist[4] = {};
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
        u64 bank_mip = mip;
        bool gpu_fresh = false;
        if (feedback) {
            if (const u32 g = gpu_mip[i].exchange(0, std::memory_order_relaxed)) {
                const u8 now = static_cast<u8>(g - 1);
                if (gpu_last[i] == 0xff || now <= gpu_last[i]) {
                    gpu_last[i] = now;
                    gpu_coarse_for[i] = 0;
                } else if (++gpu_coarse_for[i] >= fb_hold) {
                    gpu_last[i] = now;
                    gpu_coarse_for[i] = 0;
                }
                gpu_age[i] = 0;
                gpu_fresh = true;
            } else if (gpu_age[i] != 0xffffffffu) {
                ++gpu_age[i];
            }
            // Only banks the binds marked count as sampled, as before: the measurement makes a
            // sampled bank coarser, never adds one.
            if (gpu_last[i] != 0xff && gpu_age[i] <= fb_keep) {
                const int m = std::max(int(gpu_last[i]) + fb_bias, fb_min) + int(coarser);
                const u64 measured_mip = static_cast<u64>(std::clamp(m, 0, 14));
                ++measured;
                ++mip_hist[std::min<u64>(measured_mip, 3)];
                if (fb_use) {
                    bank_mip = measured_mip;
                }
            }
            (void)gpu_fresh;
        }
        entries[i] = idle[i] < linger_now ? (bank_mip << 56) | (u64{std::max<u32>(last_count[i], 1)} << 32)
                     : keep           ? level << 56
                                      : u64{0xf} << 56;
    }
    static u32 fb_reports = 0;
    if (pool_log && feedback && fb_reports++ % 30 == 0) {
        std::fprintf(stderr, "SOTC LOD FEEDBACK: %u banks measured, mips 0/1/2/3+: %u/%u/%u/%u\n",
                     measured, mip_hist[0], mip_hist[1], mip_hist[2], mip_hist[3]);
    }
}

} // namespace SotcLod
