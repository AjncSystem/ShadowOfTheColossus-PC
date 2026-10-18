// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: see vk_frame_pacer.h.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#ifdef _WIN32
#include <windows.h>
#endif
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "video_core/renderer_vulkan/host_passes/fg_pass.h"
#include "video_core/renderer_vulkan/vk_frame_pacer.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

namespace Vulkan {

namespace {

// SOTC_FG_DEBUG=passthrough: each real frame is presented twice, the second time half a real
// interval after the first (pacing test without frame interpolation).
bool Passthrough() {
    static const bool value = [] {
        const char* v = std::getenv("SOTC_FG_DEBUG");
        return v && std::strcmp(v, "passthrough") == 0;
    }();
    return value;
}

// SOTC_FG_DEBUG=gen: each generated frame is presented twice and no real frame is (inspection of
// the interpolation; screenshots of the window then only show generated frames).
bool GenOnly() {
    static const bool value = [] {
        const char* v = std::getenv("SOTC_FG_DEBUG");
        return v && std::strcmp(v, "gen") == 0;
    }();
    return value;
}

// SOTC_FG_MIN_RATIO (1.9): the second present of a real frame needs the real interval to span
// at least this many display refreshes. Below that both presents cannot get a vblank each, FIFO
// would push back on the draw thread, and the real frame rate would fall.
double MinRatio() {
    static const double value = [] {
        const char* v = std::getenv("SOTC_FG_MIN_RATIO");
        const double ratio = v ? std::strtod(v, nullptr) : 0.0;
        return ratio > 0.0 ? ratio : 1.9;
    }();
    return value;
}

// SOTC_FG_VBLANK_LOCK=0 keeps the guest vblank at its nominal rate (FramePacer::GuestVblankPeriod).
bool VblankLock() {
    static const bool value = [] {
        const char* v = std::getenv("SOTC_FG_VBLANK_LOCK");
        return !v || v[0] != '0';
    }();
    return value;
}

// A present not on screen after this long (minimized or hidden window) is given up on.
constexpr u64 kPresentTimeoutNs = 100'000'000;
// Real frame intervals above this (loading, shader compiles) are left out of the EMA.
constexpr double kMaxIntervalMs = 100.0;
// Pace error samples kept per stats window, so the buffer stays bounded without SOTC_FPS.
constexpr size_t kMaxPaceSamples = 4096;

double Ms(std::chrono::steady_clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

} // namespace

FramePacer::FramePacer(Presenter& presenter_)
    : presenter{presenter_},
      nominal_vblank_ns{1'000'000'000LL / EmulatorSettings.GetVblankFrequency()},
      refresh_ms{1000.0 / BbDisplayRefreshHz()}, refresh_seed_ms{refresh_ms},
      thread{[this](std::stop_token token) { Run(token); }} {
    LOG_INFO(Render_Vulkan,
             "Frame pacer (SOTC_FG): {}, display seed {} Hz, guest vblank {} Hz, vblank lock {}",
             Passthrough()                ? "passthrough (each real frame shown twice)"
             : !presenter.instance.FrameGenSupported() ? "real frames only"
             : GenOnly() ? "frame interpolation, debug: generated frames only (each shown twice)"
                         : "frame interpolation (optical flow, neutral depth and motion)",
             BbDisplayRefreshHz(), EmulatorSettings.GetVblankFrequency(), VblankLock());
}

FramePacer::~FramePacer() {
    {
        std::scoped_lock lk{mutex};
        thread.request_stop();
    }
    cv.notify_all();
    // The jthread member joins on destruction.
}

void FramePacer::Enqueue(Frame* frame, bool hdr) {
    {
        std::scoped_lock lk{mutex};
        queue.push_back({frame, hdr, Clock::now()});
    }
    cv.notify_one();
}

void FramePacer::RequestBlank() {
    {
        std::scoped_lock lk{mutex};
        blank_requested = true;
    }
    cv.notify_one();
}

bool FramePacer::Show(Frame* frame, Clock::time_point& shown) {
    Swapchain& swapchain = presenter.swapchain;
    // Normally a no-op (each present is waited for below); after a timeout it keeps the queue
    // from growing, so the present stays non-blocking.
    swapchain.WaitPresented(swapchain.LastPresentId(), kPresentTimeoutNs);
    if (!presenter.PresentImage(frame)) {
        return false;
    }
    const double lock_ms = Ms(presenter.present_lock_time);
    const bool on_screen = swapchain.WaitPresented(swapchain.LastPresentId(), kPresentTimeoutNs);
    // After a timeout the wake-up time is not a vblank: no timing sample (OnShown).
    shown = on_screen ? Clock::now() : Clock::time_point{};
    std::scoped_lock lk{stats_mutex};
    ++stats.presents;
    stats.lock_max_ms = std::max(stats.lock_max_ms, lock_ms);
    stats.timeouts += on_screen ? 0 : 1;
    return true;
}

void FramePacer::UpdateRefresh(double interval_ms) {
    // Two presents shown back to back are k refreshes apart. The seed is the display mode's
    // rate rounded to an integer; this refines it to the real one (59.94, 143.9...).
    const double refined = FramePacing::RefineRefresh(refresh_ms, interval_ms);
    if (refined == 0.0) {
        ++stats.rejected;
        return;
    }
    refresh_ms = refined;
    ++refresh_samples;
    // ponytail: rate lock only (no phase lock): the guest tick keeps an arbitrary but constant
    // offset to the vblank. Not done when the estimate left the display mode's rate (VRR, where
    // the shown intervals are the present intervals and would lock the guest to itself).
    if (!VblankLock() || refresh_samples < 120 ||
        std::abs(refresh_ms - refresh_seed_ms) > 0.01 * refresh_seed_ms) {
        return;
    }
    const double locked_ms = FramePacing::LockedVblankMs(nominal_vblank_ns / 1e6, refresh_ms);
    locked_vblank_ns.store(static_cast<s64>(locked_ms * 1e6), std::memory_order_relaxed);
}

void FramePacer::OnShown(Clock::time_point shown, double ideal_ms) {
    if (shown == Clock::time_point{}) {
        have_last_shown = false;
        return;
    }
    std::scoped_lock lk{stats_mutex};
    if (have_last_shown) {
        const double interval_ms = Ms(shown - last_shown);
        UpdateRefresh(interval_ms);
        if (ideal_ms > 0.0 && interval_ms < 2.0 * ideal_ms &&
            stats.pace_err_ms.size() < kMaxPaceSamples) {
            stats.pace_err_ms.push_back(static_cast<float>(std::abs(interval_ms - ideal_ms)));
        }
    }
    last_shown = shown;
    have_last_shown = true;
}

void FramePacer::Run(std::stop_token token) {
#ifdef _WIN32
    // Common::SetCurrentThreadName/SetCurrentThreadRealtime do nothing on Windows.
    SetThreadDescription(GetCurrentThread(), L"shadPS4:FramePacer");
#endif
    // SetThreadPriority(THREAD_PRIORITY_HIGHEST) on Windows: it sleeps nearly all the time and
    // has to wake on time for the vblank it aims at.
    Common::SetCurrentThreadPriority(Common::ThreadPriority::VeryHigh);

    const vk::Device device = presenter.instance.GetDevice();
    bool last_was_second = false;
    std::vector<Frame*> dropped;
    // sotc: frame interpolation (phase 3), recorded on the present scheduler that this thread owns.
    std::unique_ptr<HostPasses::FgPass> fg;
    if (presenter.instance.FrameGenSupported() && !Passthrough()) {
        fg = std::make_unique<HostPasses::FgPass>(presenter.instance, presenter.present_scheduler,
                                                  presenter.draw_scheduler);
    }
    bool skip_generated = false;

    while (!token.stop_requested()) {
        Item item{};
        bool blank = false;
        {
            std::unique_lock lk{mutex};
            cv.wait(lk, [&] { return token.stop_requested() || !queue.empty() || blank_requested; });
            if (token.stop_requested()) {
                break;
            }
            // Never more than one real frame waiting (latency bound): older ones are dropped.
            while (queue.size() > 1) {
                dropped.push_back(queue.front().frame);
                queue.pop_front();
            }
            if (!queue.empty()) {
                item = queue.front();
                queue.pop_front();
            }
            blank = std::exchange(blank_requested, false) && !item.frame;
        }
        for (Frame* frame : dropped) {
            presenter.ReleaseFrame(frame);
        }
        if (!dropped.empty()) {
            std::scoped_lock lk{stats_mutex};
            stats.dropped += static_cast<u32>(dropped.size());
            dropped.clear();
        }

        if (blank) {
            Frame* frame = presenter.PrepareBlankFrame(true);
            Clock::time_point shown;
            Show(frame, shown);
            presenter.ReleaseFrame(frame);
            have_last_shown = false;
            if (fg) {
                fg->Reset();
            }
            continue;
        }
        if (!item.frame) {
            continue;
        }

        // D: EMA of the real frame intervals, as handed over at the guest's vblank ticks. A much
        // longer interval (loading, shader compile, hitch) starts a new interpolation history.
        bool hitch = false;
        if (last_arrival != Clock::time_point{}) {
            const double ms = Ms(item.arrival - last_arrival);
            hitch = ms >= kMaxIntervalMs || (interval_ema_ms > 0.0 && ms > 2.0 * interval_ema_ms);
            if (ms < kMaxIntervalMs) {
                interval_ema_ms = interval_ema_ms > 0.0 ? interval_ema_ms + 0.1 * (ms - interval_ema_ms)
                                                        : ms;
            }
        }
        last_arrival = item.arrival;
        const double interval_ms = interval_ema_ms;
        // Two presents per real frame need the real interval to span enough refreshes.
        const bool two_presents = interval_ms > 0.0 && interval_ms >= MinRatio() * refresh_ms;

        presenter.SetHDR(item.hdr);

        // The frame between the previous real frame and this one, shown first.
        Frame* generated = nullptr;
        if (fg && !fg->Failed()) {
            if (!two_presents) {
                fg->Reset(); // no interpolation work while it could not be shown
            } else {
                if (hitch || std::exchange(skip_generated, false)) {
                    fg->Reset();
                }
                generated = fg->Generate(*item.frame, interval_ms);
                std::scoped_lock lk{stats_mutex};
                ++stats.interpolated;
                stats.resets += generated ? 0 : 1;
                for (const float ms : fg->TakeGpuTimes()) {
                    stats.fi_gpu_sum_ms += ms;
                    stats.fi_gpu_max_ms = std::max(stats.fi_gpu_max_ms, double(ms));
                    ++stats.fi_gpu_count;
                }
            }
        }

        Clock::time_point shown_first;
        // No swapchain image (resize): the frame is skipped. An interpolation that read it is
        // ahead of the draw thread's next use of it on the queue (FgPass::Generate's barrier).
        if (!Show(generated ? generated : item.frame, shown_first)) {
            presenter.ReleaseFrame(item.frame);
            continue;
        }
        presenter.CountGameFrame();
        OnShown(shown_first, last_was_second ? interval_ms / 2.0 : interval_ms);
        last_was_second = false;
        {
            std::scoped_lock lk{stats_mutex};
            ++stats.real;
            stats.generated += generated ? 1 : 0;
            stats.interval_ms = interval_ms;
            stats.refresh_ms = refresh_ms;
            if (!two_presents && interval_ms > 0.0 && (fg || Passthrough())) {
                ++stats.ratio;
            }
        }

        // The second present: the real frame after the generated one (the generated one again
        // with SOTC_FG_DEBUG=gen), or the real frame again in passthrough.
        Frame* second = generated ? (GenOnly() ? generated : item.frame)
                        : Passthrough() && two_presents ? item.frame
                                                        : nullptr;
        // After a generated frame the real one has to be shown, even late.
        const bool real_due = second == item.frame && generated;
        if (second) {
            const bool timed_out = shown_first == Clock::time_point{}; // no time to aim at
            bool newer = false;
            if (!timed_out) {
                // Aim the second present at the vblank nearest to the middle of the interval:
                // FIFO shows it at the first vblank after it is submitted, so submit half a
                // refresh early. ponytail: right for fixed refresh; with VRR it shows half a
                // refresh early (needs VRR detection).
                const auto due =
                    shown_first + std::chrono::duration_cast<Clock::duration>(
                                      std::chrono::duration<double, std::milli>(
                                          interval_ms / 2.0 - refresh_ms / 2.0));
                std::unique_lock lk{mutex};
                newer = cv.wait_until(lk, due, [&] {
                    return token.stop_requested() || !queue.empty();
                });
            }
            if (newer) {
                // The next real frame is here first: it goes on screen next instead. A real
                // frame after a generated one is still shown (now), and the next real frame
                // gets no generated frame, so the queue does not fall behind.
                std::scoped_lock lk{stats_mutex};
                ++stats.late;
            }
            if ((!newer && !timed_out) || real_due) {
                // A frame shown before has its blit done (its image is on screen), so its fence
                // may be reset for the second present.
                (void)device.waitForFences(second->present_done, true,
                                           std::numeric_limits<u64>::max());
                Clock::time_point shown_second;
                if (Show(second, shown_second)) {
                    const bool on_time = !newer && !timed_out;
                    OnShown(shown_second, on_time ? interval_ms / 2.0 : 0.0);
                    last_was_second = on_time;
                    std::scoped_lock lk{stats_mutex};
                    ++stats.second;
                }
                skip_generated = newer;
            }
        }
        presenter.ReleaseFrame(item.frame);
    }
}

std::string FramePacer::TakeStats(double seconds) {
    Stats s;
    {
        std::scoped_lock lk{stats_mutex};
        s = std::move(stats);
        stats = {};
        stats.interval_ms = s.interval_ms;
        stats.refresh_ms = s.refresh_ms;
    }
    std::sort(s.pace_err_ms.begin(), s.pace_err_ms.end());
    const auto pct = [&](double p) {
        return s.pace_err_ms.empty()
                   ? 0.0
                   : double(s.pace_err_ms[std::min(s.pace_err_ms.size() - 1,
                                                   static_cast<size_t>(p * s.pace_err_ms.size()))]);
    };
    const s64 locked = locked_vblank_ns.load(std::memory_order_relaxed);
    // Frame interpolation: generated frames shown, interpolations run (the ones that started a
    // new history show nothing) and their GPU time.
    char fi[192] = "";
    if (presenter.instance.FrameGenSupported() && !Passthrough()) {
        std::snprintf(fi, sizeof(fi),
                      "; FG generated %.1f/s, interpolations %.1f/s (new history %u), fi gpu avg "
                      "%.2f max %.2f ms",
                      s.generated / seconds, s.interpolated / seconds, s.resets,
                      s.fi_gpu_count ? s.fi_gpu_sum_ms / s.fi_gpu_count : 0.0, s.fi_gpu_max_ms);
    }
    char line[768];
    std::snprintf(line, sizeof(line),
                  "SOTC FG pacer%s: real %.1f/s, presents %.1f/s, second %.1f/s (skipped: late "
                  "%u, ratio %u), dropped %u; display %.3f Hz (%u rejected), real interval "
                  "%.2f ms, guest vblank %.3f Hz%s; pace err p50 %.2f p99 %.2f ms; present lock "
                  "max %.2f ms, timeouts %u%s\n",
                  Passthrough() ? " passthrough" : GenOnly() ? " gen" : "", s.real / seconds,
                  s.presents / seconds,
                  s.second / seconds, s.late, s.ratio, s.dropped,
                  s.refresh_ms > 0.0 ? 1000.0 / s.refresh_ms : 0.0, s.rejected, s.interval_ms,
                  1e9 / double(locked ? locked : nominal_vblank_ns), locked ? " (locked)" : "",
                  pct(0.5), pct(0.99), s.lock_max_ms, s.timeouts, fi);
    return line;
}

} // namespace Vulkan
