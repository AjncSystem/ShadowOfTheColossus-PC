// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: per-frame budget of streamed mip copies (plan C4, docs/re-stutters/reports2/03-streaming-bursts.md).
// Hook MIP_BUDGET_COPY (scripts/hooks.d/mipbudget.py) wraps sub_98ae0, the only function that
// memcpy's a mip into the streaming pool. Switch SOTC_MIP_BUDGET:
//
//   unset or 0  off: the hook is not installed; nothing runs.
//   N >= 1      simulated budget of N copies per game frame (a frame is one game flip,
//               SotcStats::flip_count). The handler is observe only: it never skips a copy.
//
// Why observe only. A skipped copy is not retried by the engine (report 03, pseudocode):
//   - sub_13a6f0 (the default case, and case 7) and the unloading path of sub_13a3e0 call
//     sub_98ae0 once, then store the current mip (sub_98da0) into +0xb and set the state to 1
//     (Idle). The requested target is overwritten, so a skipped copy is marked done.
//   - the upgrade path (sub_13a3e0, AutomaticLoading) kicks TextureStreamingChunk::ProcessTextureData,
//     whose job runs sub_112d30 -> sub_98ae0. The caller clears the request flag at +0x98 and
//     releases the resource when ProcessTextureData returns true, which happens when the job
//     counter reaches zero, not when the copy succeeds. A skipped copy is therefore done too.
// So a real cap would lose the upgrade; the handler measures what the cap would have done instead.
//
// What the report measures, for upgrades only (target chunk > current chunk, obj+0xc0):
//   copied and not ready. The copy happens only when the resource is ready, so the current chunk
//   is read again in the post phase: equal to the target means copied.
//   The simulated cap is a FIFO: copies beyond N in a flip wait for a later flip with budget left.
//   "longest wait" is the simulated wait in flips. The arrivals are the real copies, so the
//   simulation ignores that a real cap would change the demand; treat it as an estimate.

#include "mip_budget.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "hooks.h"
#include "video_core/sotc_stats.h"

extern "C" {
// runtime_memory.c: the unprotected backing view of a guest address, and the mapped region of one.
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
}

namespace SotcMipBudget {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr uintptr_t kCurrentChunkOffset = 0xc0; // TextureManager texture: current chunk (int32)
constexpr size_t kQueueCap = 4096;              // simulated waiting copies (arrival flip each)

// SOTC_MIP_BUDGET, read once. 0 when unset, invalid or 0.
int Budget() {
    static const int budget = [] {
        const char* v = std::getenv("SOTC_MIP_BUDGET");
        if (!v) {
            return 0;
        }
        char* end = nullptr;
        const long long n = std::strtoll(v, &end, 10);
        if (end == v || n <= 0) {
            return 0;
        }
        return n > (1 << 20) ? (1 << 20) : static_cast<int>(n);
    }();
    return budget;
}

// Reads a guest uint32 through the backing view (never the GPU-protected pages). False when unmapped.
bool ReadGuestU32(uintptr_t address, uint32_t* out) {
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (!runtime_memory_region(address, &start, &end, &mapped) || !mapped) {
        return false;
    }
    if (address < start || address + 4 > end) {
        return false;
    }
    const unsigned char* p = runtime_memory_backing_pointer(address);
    if (!p) {
        return false;
    }
    std::memcpy(out, p, 4);
    return true;
}

// The upgrade seen by the pre phase, read again by the post phase on the same thread.
struct Pending {
    uintptr_t texture = 0;
    int32_t target = 0;
    int32_t before = 0;
    bool valid = false;
};
thread_local Pending t_call;

struct Window {
    uint64_t flips = 0;
    uint64_t upgrades = 0;
    uint64_t copied = 0;
    uint64_t not_ready = 0;
    uint64_t over = 0;        // copies beyond the simulated budget of their flip
    uint32_t max_per_flip = 0;
    uint64_t served = 0;
    uint64_t longest_wait = 0;
    uint64_t lost = 0;        // arrivals dropped because the simulated queue was full
};

struct State {
    std::mutex mutex;
    bool flip_seen = false;
    uint64_t flip = 0;                       // current game flip
    uint32_t served_this_flip = 0;           // simulated budget used in the current flip
    uint32_t max_this_flip = 0;              // real copies seen in the current flip
    std::array<uint64_t, kQueueCap> queue{}; // arrival flip of each waiting copy
    size_t head = 0;
    size_t count = 0;
    Window window;
};
State g;

// Serves queued copies of the current flip up to the simulated budget (caller holds g.mutex).
void ServeQueue(int budget) {
    while (g.count > 0 && g.served_this_flip < static_cast<uint32_t>(budget)) {
        const uint64_t arrival = g.queue[g.head];
        g.head = (g.head + 1) % kQueueCap;
        --g.count;
        ++g.served_this_flip;
        ++g.window.served;
        const uint64_t wait = g.flip - arrival;
        if (wait > g.window.longest_wait) {
            g.window.longest_wait = wait;
        }
    }
}

void Record(bool copied) {
    const uint64_t flip = SotcStats::flip_count.load(std::memory_order_relaxed);
    const int budget = Budget();
    std::lock_guard<std::mutex> lock(g.mutex);
    if (!g.flip_seen || flip != g.flip) {
        g.flip_seen = true;
        g.flip = flip;
        g.served_this_flip = 0;
        g.max_this_flip = 0;
        ++g.window.flips;
        ServeQueue(budget); // the queue drains into the new flip first
    }
    ++g.window.upgrades;
    if (!copied) {
        ++g.window.not_ready;
        return;
    }
    ++g.window.copied;
    ++g.max_this_flip;
    if (g.max_this_flip > g.window.max_per_flip) {
        g.window.max_per_flip = g.max_this_flip;
    }
    if (g.max_this_flip > static_cast<uint32_t>(budget)) {
        ++g.window.over;
    }
    if (g.count == kQueueCap) {
        ++g.window.lost;
    } else {
        g.queue[(g.head + g.count) % kQueueCap] = flip;
        ++g.count;
    }
    ServeQueue(budget);
}

HookAction MipPre(GuestRegs& regs, void*) {
    t_call.valid = false;
    if (Budget() == 0) {
        return HookAction::Continue;
    }
    const uintptr_t texture = static_cast<uintptr_t>(regs.rdi);
    const int32_t target = static_cast<int32_t>(static_cast<uint32_t>(regs.rsi));
    uint32_t before = 0;
    if (texture == 0 || !ReadGuestU32(texture + kCurrentChunkOffset, &before)) {
        return HookAction::Continue;
    }
    t_call.texture = texture;
    t_call.target = target;
    t_call.before = static_cast<int32_t>(before);
    t_call.valid = true;
    return HookAction::Continue;
}

void MipPost(GuestRegs&, void*) {
    const Pending p = t_call;
    t_call.valid = false;
    if (!p.valid || p.target <= p.before) {
        return; // a drop or a no-op: not a streamed copy
    }
    uint32_t after = 0;
    if (!ReadGuestU32(p.texture + kCurrentChunkOffset, &after)) {
        return;
    }
    Record(static_cast<int32_t>(after) == p.target);
}

} // namespace

void RegisterMipBudget() {
    SotcHooks::Handler handler;
    handler.pre = MipPre;
    handler.post = MipPost;
    SotcHooks::RegisterHandler("MIP_BUDGET_COPY", handler);
}

void ReportMipBudget(std::string& out, double seconds) {
    const int budget = Budget();
    if (budget == 0) {
        return;
    }
    if (seconds <= 0.0) {
        seconds = 1.0;
    }
    Window w;
    uint64_t queued = 0;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        w = g.window;
        g.window = Window{};
        queued = g.count;
    }
    if (w.upgrades == 0) {
        return;
    }
    char line[400];
    std::snprintf(line, sizeof line,
                  "SOTC MIPBUDGET N=%d: upgrades %llu (%.1f/s), copied %llu in %llu flips "
                  "(max %u per flip, %llu over N), not ready %llu; simulated: served %llu, queued %llu, "
                  "longest wait %llu flips, lost %llu (observe only, nothing skipped)",
                  budget, static_cast<unsigned long long>(w.upgrades), w.upgrades / seconds,
                  static_cast<unsigned long long>(w.copied), static_cast<unsigned long long>(w.flips),
                  w.max_per_flip, static_cast<unsigned long long>(w.over),
                  static_cast<unsigned long long>(w.not_ready), static_cast<unsigned long long>(w.served),
                  static_cast<unsigned long long>(queued), static_cast<unsigned long long>(w.longest_wait),
                  static_cast<unsigned long long>(w.lost));
    out += line;
    out += '\n';
}

} // namespace SotcMipBudget
