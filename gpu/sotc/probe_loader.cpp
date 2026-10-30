// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: engine probe for the resource loader (plan step 0c, docs/re-stutters/reports/05-streaming.md).
// Two Wrap hooks (scripts/hooks.d/loader.py), switch SOTC_HOOK_LOADER_PROBE:
//   LOADER_WAIT_ALL   ResourceFactory::WaitForAllCmdsToFinish (0x1469d0), the frame barrier.
//   LOADER_SYNCPOINT  ResourceFactory::SyncPointProcessPendingResources (0x1480f0).
// The barrier calls SyncPoint once per loop iteration (0x146ae4), so the SyncPoint calls made
// inside a barrier on this thread are its iterations. Each barrier call is also attributed to its
// call site, from the return address on the guest stack.
// Observe only: every handler returns Continue. The return address is read through the backing
// view (runtime_memory_backing_pointer), not through the guest mapping.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "hooks.h"
#include "probes.h"

extern "C" {
// runtime_memory.c: the unprotected backing view of a guest address (null when unmapped).
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
}

namespace SotcProbes {
namespace {

constexpr uint64_t kImageSpan = 0x10000000; // the eboot's code is far below 256 MiB
constexpr int kMaxDepth = 16;               // nested calls tracked per hook and thread
constexpr size_t kMaxSites = 12;            // distinct barrier call sites per window

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Lock-free aggregate of one hook for the current report window.
struct Agg {
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> total_ns{0};
    std::atomic<uint64_t> max_ns{0};

    void Add(uint64_t ns) noexcept {
        calls.fetch_add(1, std::memory_order_relaxed);
        total_ns.fetch_add(ns, std::memory_order_relaxed);
        uint64_t seen = max_ns.load(std::memory_order_relaxed);
        while (ns > seen && !max_ns.compare_exchange_weak(seen, ns, std::memory_order_relaxed)) {
        }
    }
};

struct Window {
    uint64_t calls;
    uint64_t total_ns;
    uint64_t max_ns;
};

// Reads and resets one aggregate.
Window Take(Agg& agg) noexcept {
    return Window{agg.calls.exchange(0, std::memory_order_relaxed),
                  agg.total_ns.exchange(0, std::memory_order_relaxed),
                  agg.max_ns.exchange(0, std::memory_order_relaxed)};
}

Agg g_barrier;       // LOADER_WAIT_ALL, every call
Agg g_sync_frame;    // LOADER_SYNCPOINT called outside a barrier (the per-frame call)
Agg g_sync_barrier;  // LOADER_SYNCPOINT called inside a barrier (its loop iterations)
std::atomic<uint64_t> g_iterations{0}; // SyncPoint calls inside barriers, summed per barrier call
std::atomic<uint64_t> g_overflow{0};   // calls not tracked because nesting exceeded kMaxDepth

// Per call site of the barrier. Updated once per barrier call under a short mutex.
struct SiteRow {
    uint64_t site = 0; // call site (absolute address); 0 when the caller is unknown
    uint64_t calls = 0;
    uint64_t total_ns = 0;
    uint64_t max_ns = 0;
    uint64_t iterations = 0;
};
std::mutex g_site_mutex;
std::array<SiteRow, kMaxSites> g_sites{};
size_t g_site_rows = 0;     // guarded by g_site_mutex
uint64_t g_site_other = 0;  // guarded by g_site_mutex: calls with no free row

void AddSite(uint64_t site, uint64_t ns, uint64_t iterations) {
    std::lock_guard<std::mutex> lock(g_site_mutex);
    SiteRow* row = nullptr;
    for (size_t i = 0; i < g_site_rows; ++i) {
        if (g_sites[i].site == site) {
            row = &g_sites[i];
            break;
        }
    }
    if (!row) {
        if (g_site_rows == kMaxSites) {
            ++g_site_other;
            return;
        }
        row = &g_sites[g_site_rows++];
        *row = SiteRow{};  // the slot may hold the previous window's counters
        row->site = site;
    }
    row->calls += 1;
    row->total_ns += ns;
    row->max_ns = std::max(row->max_ns, ns);
    row->iterations += iterations;
}

// Open calls of one hook on this thread. SyncPoint runs inside the barrier, so each hook keeps its
// own stack. A call deeper than kMaxDepth is not pushed; `overflow` counts it, and its post phase
// only decrements `overflow`, so pushes and pops stay matched (calls nest, so the order holds).
struct Frame {
    int64_t start_ns = 0;
    uint64_t site = 0;
    uint64_t iterations = 0;
    bool in_barrier = false;
};

struct FrameStack {
    Frame frames[kMaxDepth];
    int depth = 0;
    int overflow = 0;
};

thread_local FrameStack t_barrier;
thread_local FrameStack t_sync;

// At function entry the return address is at [rsp] (hooks.h). The guest stacks are fiber/host
// stacks, never GPU-tracked data pages, so they are not in the backing view and are read
// directly. regs.rsp is valid on the guest thread the handler runs on.
uint64_t ReturnAddress(const SotcHooks::GuestRegs& regs) {
    uint64_t ret = 0;
    if (regs.rsp != 0) {
        std::memcpy(&ret, reinterpret_cast<const void*>(static_cast<uintptr_t>(regs.rsp)),
                    sizeof ret);
    }
    return ret;
}

// A call rel32 (opcode E8) is 5 bytes, so its address is the return address minus 5. Any other
// caller (indirect call, or a return address outside the eboot) keeps the return address.
uint64_t CallSiteOf(uint64_t ret) {
    const uint64_t base = static_cast<uint64_t>(SotcHooks::ImageBase());
    if (ret == 0 || base == 0 || ret < base + 5 || ret - base >= kImageSpan) {
        return ret;
    }
    // ret points into the eboot's code, which is always readable (not a GPU-tracked data page).
    const uint8_t* code = reinterpret_cast<const uint8_t*>(ret);
    return code[-5] == 0xE8 ? ret - 5 : ret;
}

SotcHooks::HookAction BarrierPre(SotcHooks::GuestRegs& regs, void*) {
    const int64_t now = NowNs();
    FrameStack& s = t_barrier;
    if (s.depth >= kMaxDepth) {
        ++s.overflow;
        g_overflow.fetch_add(1, std::memory_order_relaxed);
        return SotcHooks::HookAction::Continue;
    }
    Frame& f = s.frames[s.depth++];
    f.start_ns = now;
    f.site = CallSiteOf(ReturnAddress(regs));
    f.iterations = 0;
    f.in_barrier = false;
    return SotcHooks::HookAction::Continue;
}

void BarrierPost(SotcHooks::GuestRegs&, void*) {
    const int64_t now = NowNs();
    FrameStack& s = t_barrier;
    if (s.overflow > 0) {
        --s.overflow;
        return;
    }
    if (s.depth == 0) {
        return;
    }
    const Frame f = s.frames[--s.depth];
    const uint64_t ns = static_cast<uint64_t>(std::max<int64_t>(0, now - f.start_ns));
    g_barrier.Add(ns);
    g_iterations.fetch_add(f.iterations, std::memory_order_relaxed);
    AddSite(f.site, ns, f.iterations);
}

SotcHooks::HookAction SyncPre(SotcHooks::GuestRegs&, void*) {
    const int64_t now = NowNs();
    FrameStack& s = t_sync;
    FrameStack& b = t_barrier;
    if (s.depth >= kMaxDepth) {
        ++s.overflow;
        g_overflow.fetch_add(1, std::memory_order_relaxed);
        return SotcHooks::HookAction::Continue;
    }
    // Inside a barrier, this call is one of its loop iterations (the innermost barrier counts it).
    const bool in_barrier = b.depth > 0 || b.overflow > 0;
    if (b.depth > 0 && b.overflow == 0) {
        ++b.frames[b.depth - 1].iterations;
    }
    Frame& f = s.frames[s.depth++];
    f.start_ns = now;
    f.site = 0;
    f.iterations = 0;
    f.in_barrier = in_barrier;
    return SotcHooks::HookAction::Continue;
}

void SyncPost(SotcHooks::GuestRegs&, void*) {
    const int64_t now = NowNs();
    FrameStack& s = t_sync;
    if (s.overflow > 0) {
        --s.overflow;
        return;
    }
    if (s.depth == 0) {
        return;
    }
    const Frame f = s.frames[--s.depth];
    const uint64_t ns = static_cast<uint64_t>(std::max<int64_t>(0, now - f.start_ns));
    (f.in_barrier ? g_sync_barrier : g_sync_frame).Add(ns);
}

} // namespace

void RegisterLoader() {
    SotcHooks::Handler barrier;
    barrier.pre = BarrierPre;
    barrier.post = BarrierPost;
    SotcHooks::RegisterHandler("LOADER_WAIT_ALL", barrier);

    SotcHooks::Handler sync;
    sync.pre = SyncPre;
    sync.post = SyncPost;
    SotcHooks::RegisterHandler("LOADER_SYNCPOINT", sync);
}

void ReportLoader(std::string& out, double seconds) {
    if (seconds <= 0.0) {
        seconds = 1.0;
    }
    // Take the window even when nothing is printed, so counts never carry over.
    const Window barrier = Take(g_barrier);
    const Window sync_frame = Take(g_sync_frame);
    const Window sync_barrier = Take(g_sync_barrier);
    const uint64_t iterations = g_iterations.exchange(0, std::memory_order_relaxed);
    const uint64_t overflow = g_overflow.exchange(0, std::memory_order_relaxed);

    std::array<SiteRow, kMaxSites> rows{};
    size_t row_count = 0;
    uint64_t other = 0;
    {
        std::lock_guard<std::mutex> lock(g_site_mutex);
        row_count = g_site_rows;
        std::copy_n(g_sites.begin(), row_count, rows.begin());
        other = g_site_other;
        g_site_rows = 0;
        g_site_other = 0;
    }
    if (barrier.calls == 0 && sync_frame.calls == 0 && sync_barrier.calls == 0) {
        return;
    }
    std::sort(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(row_count),
              [](const SiteRow& a, const SiteRow& b) { return a.total_ns > b.total_ns; });

    const double per_call = barrier.calls ? static_cast<double>(iterations) / barrier.calls : 0.0;
    char head[512];
    std::snprintf(head, sizeof head,
                  "SOTC LOADER: barrier %llu calls (%.1f/s) total %.2f ms max %.2f ms, %.2f SyncPoint "
                  "iterations/call; syncpoint per frame %llu calls total %.2f ms max %.2f ms; "
                  "syncpoint in barrier %llu calls total %.2f ms max %.2f ms",
                  static_cast<unsigned long long>(barrier.calls), barrier.calls / seconds,
                  barrier.total_ns / 1e6, barrier.max_ns / 1e6, per_call,
                  static_cast<unsigned long long>(sync_frame.calls), sync_frame.total_ns / 1e6,
                  sync_frame.max_ns / 1e6, static_cast<unsigned long long>(sync_barrier.calls),
                  sync_barrier.total_ns / 1e6, sync_barrier.max_ns / 1e6);
    std::string line = head;

    if (row_count > 0) {
        const uint64_t base = static_cast<uint64_t>(SotcHooks::ImageBase());
        line += " | callers:";
        for (size_t i = 0; i < row_count; ++i) {
            const SiteRow& r = rows[i];
            char item[160];
            if (r.site == 0) {
                std::snprintf(item, sizeof item, " ? %llux %.2f ms (max %.2f ms)",
                              static_cast<unsigned long long>(r.calls), r.total_ns / 1e6,
                              r.max_ns / 1e6);
            } else if (base != 0 && r.site >= base && r.site - base < kImageSpan) {
                std::snprintf(item, sizeof item, " 0x%llx %llux %.2f ms (max %.2f ms, %.1f it)",
                              static_cast<unsigned long long>(r.site - base),
                              static_cast<unsigned long long>(r.calls), r.total_ns / 1e6,
                              r.max_ns / 1e6,
                              r.calls ? static_cast<double>(r.iterations) / r.calls : 0.0);
            } else {
                std::snprintf(item, sizeof item, " abs 0x%llx %llux %.2f ms (max %.2f ms)",
                              static_cast<unsigned long long>(r.site),
                              static_cast<unsigned long long>(r.calls), r.total_ns / 1e6,
                              r.max_ns / 1e6);
            }
            line += item;
            line += ';';
        }
        if (other) {
            line += " other " + std::to_string(other) + " calls;";
        }
    }
    if (overflow) {
        line += " untracked " + std::to_string(overflow) + " (nesting);";
    }
    out += line;
    out += '\n';
}

} // namespace SotcProbes
