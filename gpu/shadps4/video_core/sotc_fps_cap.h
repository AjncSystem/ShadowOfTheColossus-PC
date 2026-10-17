// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the game's 30 FPS cap, switchable while playing (F7, bbport_overlay.cpp).
//
// The game caps itself with sceVideoOutSetFlipRate(1): one flip at most every 2 vblanks of 60 Hz.
// Its frame logic follows the real time, so ignoring that rate uncaps it, as the fps60 code patch
// does (sotc_modes.py, the import stub returns at once). Here the rate the game asks is kept and
// the vblank thread (VideoOutDriver::PresentThread) applies it or not. SOTC_FPS_CAP=off starts
// uncapped (the launcher's "Uncapped FPS"), 30 or unset capped. Uncapped needs vblanks at 120 Hz
// (BB_VBLANK_HZ=120, set by the launcher and sotc.py), else a frame still waits for a 60 Hz one.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace SotcFpsCap {

inline std::atomic<bool> uncapped{[] {
    const char* v = std::getenv("SOTC_FPS_CAP");
    return v && (v[0] == 'o' || v[0] == 'O' || v[0] == '0');
}()};

/// The flip rate the game asked, -1 while it asked none (with the fps60 code patch it never does).
inline std::atomic<int> game_rate{-1};

/// The flip rate the vblank thread applies.
inline int EffectiveRate() {
    if (uncapped.load(std::memory_order_relaxed)) {
        return 0;
    }
    const int rate = game_rate.load(std::memory_order_relaxed);
    return rate >= 0 ? rate : 1;
}

/// F7: capped <-> uncapped. Returns true when now uncapped.
inline bool Toggle() {
    const bool now = !uncapped.load(std::memory_order_relaxed);
    uncapped.store(now, std::memory_order_relaxed);
    std::fprintf(stderr, "SOTC FPS CAP: %s\n", now ? "uncapped" : "capped");
    return now;
}

/// Present thread, at each game frame: SOTC_FPS_CAP_TEST_CYCLE=<seconds> toggles like F7 that
/// often (scripted tests).
inline void TestTick() {
    static const double every = [] {
        const char* v = std::getenv("SOTC_FPS_CAP_TEST_CYCLE");
        return v ? std::atof(v) : 0.0;
    }();
    if (every <= 0.0) {
        return;
    }
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - last).count() >= every) {
        last = now;
        Toggle();
    }
}

} // namespace SotcFpsCap
