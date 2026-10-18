// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: host presentation thread of frame generation (SOTC_FG, docs/sotc/FRAMEGEN.md).
//
// With SOTC_FG the present thread no longer presents: on its vblank tick it hands the flipped
// frame to this thread (Enqueue) and goes on with the guest half of the flip (flip status,
// events, labels). This thread owns the swapchain and the present scheduler from then on. It
// presents in FIFO and, after each present, waits with VK_KHR_present_wait (outside
// Scheduler::submit_mutex) until the image is on screen, so no present is ever queued behind
// another: vkQueuePresentKHR cannot block inside the single queue's lock, and the wake-up times
// measure the display's real refresh. Real frames are paced by the EMA of their intervals.
//
// With frame interpolation (the device has what FSR 3.1.6 FI needs, host_passes/fg_pass.h), each
// real frame N first generates the frame between N-1 and N, which goes on screen when N arrives;
// N follows half a real interval later.
//
// SOTC_FG_DEBUG=passthrough presents each real frame twice instead (pacing test, no
// interpolation); SOTC_FG_DEBUG=gen presents each generated frame twice and no real frame (to
// inspect the interpolation).

#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "common/types.h"

namespace Vulkan {

namespace FramePacing {

/// The display refresh period estimate after two presents shown `interval_ms` apart: they are
/// k refreshes apart (k = 1..8). 0 when the interval is not near a whole number of refreshes.
inline double RefineRefresh(double refresh_ms, double interval_ms) {
    const double k = std::round(interval_ms / refresh_ms);
    if (k < 1.0 || k > 8.0 || std::abs(interval_ms - k * refresh_ms) > 0.25 * refresh_ms) {
        return 0.0;
    }
    return refresh_ms + 0.05 * (interval_ms / k - refresh_ms);
}

/// The guest vblank period as a whole number of display refreshes, when one is within 0.5% of
/// `guest_ms` (60 Hz guest on 59.94 or 119.88 Hz displays); 0 otherwise.
inline double LockedVblankMs(double guest_ms, double refresh_ms) {
    const double m = std::round(guest_ms / refresh_ms);
    return m >= 1.0 && std::abs(m * refresh_ms - guest_ms) < 0.005 * guest_ms ? m * refresh_ms
                                                                              : 0.0;
}

} // namespace FramePacing

struct Frame;
class Presenter;

class FramePacer {
public:
    explicit FramePacer(Presenter& presenter);
    ~FramePacer();

    /// Hands a flipped frame over (present thread, at the guest vblank that takes the flip).
    void Enqueue(Frame* frame, bool hdr);

    /// Shows a blank frame (the video port is not open yet).
    void RequestBlank();

    /// The guest vblank period to use: `nominal`, or the display's measured period times an
    /// integer when the two differ by under 0.5% (59.94 Hz, 119.88 Hz displays), so real frames
    /// do not drift against the screen and need no periodic drop or repeat. SOTC_FG_VBLANK_LOCK=0
    /// keeps `nominal`.
    std::chrono::nanoseconds GuestVblankPeriod(std::chrono::nanoseconds nominal) const {
        const s64 locked = locked_vblank_ns.load(std::memory_order_relaxed);
        return locked ? std::chrono::nanoseconds{locked} : nominal;
    }

    /// The "SOTC FG" stats line of the last `seconds` (SOTC_FPS=1), counters reset.
    std::string TakeStats(double seconds);

private:
    using Clock = std::chrono::steady_clock;

    struct Item {
        Frame* frame;
        bool hdr;
        Clock::time_point arrival;
    };

    void Run(std::stop_token token);
    /// Presents `frame` and waits until it is on screen; `shown` is when (empty after a timeout).
    /// False when the swapchain had no image for it.
    bool Show(Frame* frame, Clock::time_point& shown);
    void OnShown(Clock::time_point shown, double ideal_ms);
    void UpdateRefresh(double interval_ms);

    Presenter& presenter;
    const s64 nominal_vblank_ns;

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Item> queue;
    bool blank_requested{};

    // Pacer thread only.
    double interval_ema_ms{};     // real frame interval (D)
    double refresh_ms{};          // display refresh period (P), seeded from the display mode
    double refresh_seed_ms{};
    u32 refresh_samples{};
    Clock::time_point last_arrival{};
    Clock::time_point last_shown{};
    bool have_last_shown{};

    std::atomic<s64> locked_vblank_ns{};

    struct Stats {
        u32 real{}, presents{}, second{}, late{}, ratio{}, dropped{}, timeouts{}, rejected{};
        u32 interpolated{}, generated{}, resets{}; // frame interpolation
        double fi_gpu_sum_ms{}, fi_gpu_max_ms{};
        u32 fi_gpu_count{};
        double lock_max_ms{};
        double interval_ms{}, refresh_ms{}; // last D and P, for the stats line
        std::vector<float> pace_err_ms;
    };
    std::mutex stats_mutex;
    Stats stats;

    std::jthread thread; // last: started once everything above exists
};

} // namespace Vulkan
