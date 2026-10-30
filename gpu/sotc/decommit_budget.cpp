// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: texture pool decommit observer (plan C3, docs/re-stutters/reports2/03-streaming-bursts.md).
//
// The drop branch of sub_98ae0 (0x98ae0) decommits guest pool pages with sub_12950 mode 2 at 0x98bf0.
// The game has no per-frame limit on these calls; the 5 s report shows unmap waits of 2.5-2.9 s.
// This file only measures them (SOTC_DECOMMIT_BUDGET=2): count per window, count per game frame, and
// the time each call takes.
//
// Why no deferral: sub_12950 chooses its allocator from the calling thread's TLS array
// (0x12950..0x129d8: fs:[0] - 0x18 and the count at +0x400), and it returns before the caller updates
// the pool cursor and counters (0x98bf5..0x98c05). A deferred decommit would have to run later on the
// same thread with the same TLS state, through a guest call from the host, and the pool cursor would
// already point past pages that are still mapped. Neither is verified, so the switch observes only.
//
// Handlers run on the guest thread (host stack). They read SotcStats::flip_count (driver.cpp Flip)
// as the frame signal, so driver.cpp needs no change beyond the report call.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "hooks.h"
#include "decommit_budget.h"
#include "video_core/sotc_stats.h"

namespace SotcDecommitBudget {
namespace {

// Switch, read once. 0 off; 2 observe; any other non-zero value is read as 2 (deferral is not built).
int Mode() {
    static const int mode = [] {
        const char* text = std::getenv("SOTC_DECOMMIT_BUDGET");
        if (!text || !*text) {
            return 0;
        }
        const long value = std::strtol(text, nullptr, 10);
        if (value == 0) {
            return 0;
        }
        if (value != 2) {
            std::fprintf(stderr,
                         "SOTC DECOMMIT: SOTC_DECOMMIT_BUDGET=%ld: deferral is not implemented, "
                         "observing as 2 (docs/re-stutters/reports2/03-streaming-bursts.md)\n",
                         value);
        }
        return 2;
    }();
    return mode;
}

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Window counters (reset by ReportDecommitBudget). Atomics: the drop branch can run on any thread
// that updates textures, so the counters are not assumed to be single-threaded.
std::atomic<uint64_t> g_drops{0};        // DECOMMIT_BEGIN calls this window
std::atomic<uint64_t> g_unpaired{0};     // END without BEGIN, or BEGIN left open by a nested call
std::atomic<uint64_t> g_wait_ns{0};      // sum of BEGIN..END
std::atomic<uint64_t> g_max_ns{0};       // longest BEGIN..END
std::atomic<uint64_t> g_frame_drops{0};  // drops in the game frame in progress
std::atomic<uint64_t> g_frame_max{0};    // most drops in one closed frame this window
std::atomic<uint64_t> g_frames_with{0};  // closed frames with at least one drop this window
std::atomic<uint64_t> g_flip_seen{0};    // flip_count when the current frame began

// Thread-local open timer: BEGIN sets it, END clears it. The pair is never nested on one thread.
thread_local bool t_open = false;
thread_local int64_t t_begin_ns = 0;

// Closes the frame in progress: its drop count joins the window's per-frame figures.
void CloseFrame() {
    const uint64_t drops = g_frame_drops.exchange(0, std::memory_order_relaxed);
    if (drops == 0) {
        return;
    }
    g_frames_with.fetch_add(1, std::memory_order_relaxed);
    uint64_t seen = g_frame_max.load(std::memory_order_relaxed);
    while (drops > seen && !g_frame_max.compare_exchange_weak(seen, drops, std::memory_order_relaxed)) {
    }
}

// A new flip starts a new game frame. Only the handler that wins the compare closes the old frame.
void NoteFrame() {
    const uint64_t flips = SotcStats::flip_count.load(std::memory_order_relaxed);
    uint64_t seen = g_flip_seen.load(std::memory_order_relaxed);
    if (flips == seen) {
        return;
    }
    if (g_flip_seen.compare_exchange_strong(seen, flips, std::memory_order_relaxed)) {
        CloseFrame();
    }
}

SotcHooks::HookAction DecommitBegin(SotcHooks::GuestRegs&, void*) {
    NoteFrame();
    g_frame_drops.fetch_add(1, std::memory_order_relaxed);
    g_drops.fetch_add(1, std::memory_order_relaxed);
    if (t_open) {
        g_unpaired.fetch_add(1, std::memory_order_relaxed);
    }
    t_open = true;
    t_begin_ns = NowNs();
    return SotcHooks::HookAction::Continue;
}

SotcHooks::HookAction DecommitEnd(SotcHooks::GuestRegs&, void*) {
    if (!t_open) {
        g_unpaired.fetch_add(1, std::memory_order_relaxed);
        return SotcHooks::HookAction::Continue;
    }
    t_open = false;
    const uint64_t ns = static_cast<uint64_t>(std::max<int64_t>(0, NowNs() - t_begin_ns));
    g_wait_ns.fetch_add(ns, std::memory_order_relaxed);
    uint64_t seen = g_max_ns.load(std::memory_order_relaxed);
    while (ns > seen && !g_max_ns.compare_exchange_weak(seen, ns, std::memory_order_relaxed)) {
    }
    return SotcHooks::HookAction::Continue;
}

} // namespace

void RegisterDecommitBudget() {
    SotcHooks::Handler begin;
    begin.pre = &DecommitBegin;
    SotcHooks::RegisterHandler("DECOMMIT_BEGIN", begin);

    SotcHooks::Handler end;
    end.pre = &DecommitEnd;
    SotcHooks::RegisterHandler("DECOMMIT_END", end);
}

void ReportDecommitBudget(std::string& out, double seconds) {
    if (Mode() == 0) {
        return;
    }
    if (seconds <= 0.0) {
        seconds = 1.0;
    }
    // The frame in progress counts as closed at the report; the next window starts empty.
    CloseFrame();
    const uint64_t drops = g_drops.exchange(0, std::memory_order_relaxed);
    const uint64_t unpaired = g_unpaired.exchange(0, std::memory_order_relaxed);
    const uint64_t wait_ns = g_wait_ns.exchange(0, std::memory_order_relaxed);
    const uint64_t max_ns = g_max_ns.exchange(0, std::memory_order_relaxed);
    const uint64_t frame_max = g_frame_max.exchange(0, std::memory_order_relaxed);
    const uint64_t frames_with = g_frames_with.exchange(0, std::memory_order_relaxed);
    if (drops == 0 && unpaired == 0) {
        return;
    }
    char line[512];
    std::snprintf(line, sizeof(line),
                  "SOTC decommit: %llu drops in %.1f s (frames with drops %llu, most in one frame %llu); "
                  "wait %.2f ms (mean %.3f ms, max %.2f ms); deferred 0, max deferred age 0 frames "
                  "(observe only); unpaired %llu\n",
                  static_cast<unsigned long long>(drops), seconds,
                  static_cast<unsigned long long>(frames_with),
                  static_cast<unsigned long long>(frame_max), wait_ns / 1e6,
                  drops ? wait_ns / 1e6 / static_cast<double>(drops) : 0.0, max_ns / 1e6,
                  static_cast<unsigned long long>(unpaired));
    out += line;
}

} // namespace SotcDecommitBudget
