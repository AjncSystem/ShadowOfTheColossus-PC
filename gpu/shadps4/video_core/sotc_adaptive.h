// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_ADAPTIVE_LOAD=1 (off by default; unset or 0 changes nothing): a load governor that
// lightens the port's own work only while the game runs below its frame target.
//
// Input: the interval between two flips on the vblank thread (VideoOutDriver::PresentThread),
// i.e. the frame time the player sees, against the target the live cap sets (sotc_fps_cap.h):
// capped, (flip rate + 1) / 60 s (33.3 ms at the game's 30 FPS); uncapped, the frame limit
// (SOTC_FPS_LIMIT or the in-game menu's 60); uncapped without a limit there is no target and the
// governor stays normal. Intervals over 250 ms (loading, pause, a compile stall) are neither slow
// nor fast: they only break the runs below.
//
// States, with hysteresis:
//   normal -> relieved: 2 consecutive frames over SOTC_ADAPTIVE_HIGH_MS (default 36 = 27.8 FPS);
//   relieved -> normal: every frame at or under SOTC_ADAPTIVE_LOW_MS (default 34.5) for
//                       SOTC_ADAPTIVE_HOLD_MS (default 1000).
// LOW is above 33.3 because the capped interval cannot be shorter than the cap: at the cap a
// frame on time measures 33.3 ms plus timer jitter, and a late one at least one vblank more
// (41.7 ms at 120 Hz vblanks, 50 ms at 60 Hz). So "fast" means "on time", and the hold is the
// margin. Both values are for the 30 FPS target and scale with the target interval for others
// (x target / 33.33 ms: 18 / 17.25 ms at 60 FPS). Against oscillation (a scene that is fast only
// because it is relieved): a re-engage within 3 s of a release doubles the hold (up to 8x); 10 s
// in normal state reset it.
//
// While relieved (each part has a switch, all on by default with SOTC_ADAPTIVE_LOAD=1):
//   SOTC_ADAPTIVE_SHADOW=0  keeps the far sun shadow cascades drawn every frame. Else they are
//                           drawn on alternate frames and moved by whole texels on the others, as
//                           SOTC_SHADOW_REUSE=1 does (Rasterizer::ReuseShadowCascade); where the
//                           move is refused the cascade is drawn (never skipped).
//   SOTC_ADAPTIVE_FLUSH_US  the periodic submit interval (default 4000 instead of SOTC_FLUSH_US).
//   SOTC_ADAPTIVE_DEFER=0   keeps the background work: else the texture collector's passes below
//                           its pressure point and new cold arena checks (MaybeEvictCold, up to
//                           8 s late) wait. Pressure and emergency paths always run.
//
// Threads: the state machine runs on the vblank thread only (single writer, OnFlip); any thread
// reads Relieved() (a relaxed atomic: a reader may act one command late, never on torn state).

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>

#include "video_core/sotc_stats.h"

namespace SotcAdaptive {

namespace Detail {
inline bool EnvOn(const char* name, bool def) {
    const char* v = std::getenv(name);
    return v && v[0] ? v[0] != '0' : def;
}
inline double EnvNumber(const char* name, double def) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) {
        return def;
    }
    const double value = std::strtod(v, nullptr);
    return value > 0.0 ? value : def;
}
inline std::atomic<bool> relieved{false};
} // namespace Detail

/// Read once at static initialisation, like SotcStats::kFpsLog.
inline const bool kEnabled = [] {
    const char* v = std::getenv("SOTC_ADAPTIVE_LOAD");
    return v && v[0] == '1';
}();
inline const bool kShadows = kEnabled && Detail::EnvOn("SOTC_ADAPTIVE_SHADOW", true);
inline const bool kDefer = kEnabled && Detail::EnvOn("SOTC_ADAPTIVE_DEFER", true);
inline const uint64_t kFlushUs =
    kEnabled ? static_cast<uint64_t>(Detail::EnvNumber("SOTC_ADAPTIVE_FLUSH_US", 4000.0)) : 0;

/// Any thread: the game is below its target and the port sheds load.
inline bool Relieved() {
    return kEnabled && Detail::relieved.load(std::memory_order_relaxed);
}
/// While relieved: far cascades amortised (ReuseShadowCascade, which latches it per frame).
inline bool AmortiseShadows() {
    return kShadows && Relieved();
}
/// While relieved: background work below the memory pressure points waits.
inline bool DeferBackground() {
    return kDefer && Relieved();
}
/// While relieved: the periodic submit interval in microseconds, 0 when normal.
inline uint64_t FlushUs() {
    return kFlushUs != 0 && Relieved() ? kFlushUs : 0;
}

/// Vblank thread, at every flip taken. now_ns: steady clock; target_ns: the interval the cap or
/// the frame limit aims at, 0 for none (uncapped without a limit).
inline void OnFlip(uint64_t now_ns, uint64_t target_ns) {
    if (!kEnabled) {
        return;
    }
    static const double high_ms = Detail::EnvNumber("SOTC_ADAPTIVE_HIGH_MS", 36.0);
    // sotc: kept positive (a HIGH_MS under 0.5 made it negative: UB in the uint64_t cast below).
    static const double low_ms = std::max(
        std::min(Detail::EnvNumber("SOTC_ADAPTIVE_LOW_MS", 34.5), high_ms - 0.5), high_ms * 0.5);
    static const uint64_t hold_ns =
        static_cast<uint64_t>(Detail::EnvNumber("SOTC_ADAPTIVE_HOLD_MS", 1000.0) * 1e6);
    constexpr uint64_t kIgnoreNs = 250'000'000;    // longer: loading or a stall, not load
    constexpr uint64_t kBackoffNs = 3'000'000'000; // re-engaged this soon: longer hold
    constexpr uint64_t kResetNs = 10'000'000'000;  // normal this long: hold back to its value
    constexpr double kRefNs = 1e9 / 30.0;          // the thresholds are for the 30 FPS cap

    struct State {
        uint64_t last_ns = 0;
        uint64_t fast_since_ns = 0; // relieved: start of the run of frames on time, 0 for none
        uint64_t released_ns = 0;
        uint64_t hold_cur_ns = 0;
        uint32_t slow_run = 0;
        bool on = false;
    };
    static State s; // vblank thread only
    if (s.hold_cur_ns == 0) {
        s.hold_cur_ns = hold_ns;
    }
    const auto set = [&](bool on) {
        s.on = on;
        s.slow_run = 0;
        s.fast_since_ns = 0;
        Detail::relieved.store(on, std::memory_order_relaxed);
    };

    const uint64_t prev = s.last_ns;
    s.last_ns = now_ns;
    if (prev == 0 || now_ns <= prev) {
        return;
    }
    const uint64_t dt = now_ns - prev;
    if (SotcStats::kFpsLog) {
        SotcStats::adaptive_flips.fetch_add(1, std::memory_order_relaxed);
        if (s.on) {
            SotcStats::adaptive_relieved_flips.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (target_ns == 0) { // no target to defend
        if (s.on) {
            set(false);
            s.released_ns = now_ns;
        }
        return;
    }
    if (dt > kIgnoreNs) {
        s.slow_run = 0;
        s.fast_since_ns = 0;
        return;
    }
    const double scale = static_cast<double>(target_ns) / kRefNs;
    const auto high_ns = static_cast<uint64_t>(high_ms * 1e6 * scale);
    const auto low_ns = static_cast<uint64_t>(low_ms * 1e6 * scale);

    if (!s.on) {
        s.slow_run = dt > high_ns ? s.slow_run + 1 : 0;
        if (s.slow_run >= 2) {
            if (s.released_ns != 0 && now_ns - s.released_ns < kBackoffNs) {
                s.hold_cur_ns = std::min(s.hold_cur_ns * 2, hold_ns * 8);
            }
            set(true);
            if (SotcStats::kFpsLog) {
                SotcStats::adaptive_engaged.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (s.hold_cur_ns != hold_ns && s.released_ns != 0 &&
                   now_ns - s.released_ns >= kResetNs) {
            s.hold_cur_ns = hold_ns;
        }
        return;
    }
    if (dt > low_ns) {
        s.fast_since_ns = 0;
        return;
    }
    if (s.fast_since_ns == 0) {
        s.fast_since_ns = prev; // the run starts with this frame
    }
    if (now_ns - s.fast_since_ns >= s.hold_cur_ns) {
        set(false);
        s.released_ns = now_ns;
    }
}

} // namespace SotcAdaptive
