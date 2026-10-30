// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: render-job probe (plan B3, docs/re-stutters/reports2/14-draw-sources.md H1, 04-compute-passes.md).
// One Wrap hook (scripts/hooks.d/drawtag.py), switch SOTC_DRAW_TAG:
//   RENDER_JOB_RUN  RenderThread::RunFunctionAsRenderJob (0xc12a0). rdi = the callable block.
//
// Observed layout of the callable (eboot disassembly, RunFunctionAsRenderJob 0xc12a0 and its callers
// RenderShadows 0xcc9b0, calls at 0xccc3d and 0xccce1):
//   +0x28  this: the block's own address (the callers store lea of +0x28 there)
//   +0x30  member pointer: the trampoline at 0xd1b00 (mov rax,rdi; mov rdi,rsi; jmp [rax+0x18])
//   +0x38  this adjustment (0)
//   +0x40  the render-job body: the trampoline jumps to [this+0x18], which is block+0x40
// The job function calls the trampoline with rdi = this + adjustment, rsi = the job. So the body
// address is read from the block when the block has that layout. Any other layout is counted under
// the job key "?" (unresolved) and is not guessed.
//
// Inline timing: the body call at 0xc1423 is reached only when the flags (ecx) have bit 0 clear and
// bit 1 or 2 set (tests at 0xc12c7 and 0xc12d9), and the byte at [[rsi+0x48]+0x13] is zero (0xc12e7).
// This probe times calls by the flag test only; the byte check is not repeated. The other flag
// paths (0xc1476, 0xc1624) were not read in full: their wall time may not be the body's run time.
// Those calls are counted only (calls and inline_calls differ); no time is recorded for them.
//
// Times are wall clock (steady_clock). Inclusive time contains the inline jobs nested in the body;
// self time subtracts them. Observe only: every handler returns HookAction::Continue and nothing in
// the guest is written. The callable block is on the guest stack (host-owned memory, never GPU
// tracked), read the same way probe_loader.cpp reads its stack; no heap memory is read.

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

namespace SotcProbes {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr uint64_t kImageSpan = 0x10000000;     // the eboot's code is far below 256 MiB
constexpr uint64_t kTrampolineOffset = 0xd1b00; // image offset of the job invoker trampoline
constexpr uint64_t kBlockThis = 0x28;           // callable block: this (points at the block + 0x28)
constexpr uint64_t kBlockPointer = 0x30;        // callable block: member pointer (the trampoline)
constexpr uint64_t kBlockAdjust = 0x38;         // callable block: this adjustment
constexpr uint64_t kBlockBody = 0x40;           // callable block: render-job body (guest address)
constexpr int kMaxDepth = 32;                   // nested RunFunctionAsRenderJob calls per thread
constexpr size_t kMaxJobs = 64;                 // distinct job bodies per window
constexpr size_t kTopRows = 8;                  // rows printed per report line

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Image offset of the job body held by the callable block at c, or 0 when the block does not have
// the observed layout (or the body is outside the eboot's code).
uint64_t BodyOffset(uintptr_t c, uint64_t base) {
    if (c == 0 || base == 0) {
        return 0;
    }
    // Four qwords: +0x28 .. +0x48, the bytes the job function itself reads from this block.
    uint64_t block[4] = {};
    std::memcpy(block, reinterpret_cast<const void*>(c + kBlockThis), sizeof block);
    const uint64_t self = c + kBlockThis;
    const uint64_t thisp = block[0];
    const uint64_t pointer = block[(kBlockPointer - kBlockThis) / 8];
    const uint64_t adjust = block[(kBlockAdjust - kBlockThis) / 8];
    const uint64_t body = block[(kBlockBody - kBlockThis) / 8];
    if (thisp != self || adjust != 0 || pointer != base + kTrampolineOffset) {
        return 0;
    }
    if (body < base || body - base >= kImageSpan) {
        return 0;
    }
    return body - base;
}

// One job body in the current report window. key 0 = unresolved block layout.
struct JobRow {
    uint64_t key = 0;
    uint64_t calls = 0;        // every RunFunctionAsRenderJob call with this body
    uint64_t inline_calls = 0; // calls on the path where the body runs inside the call
    uint64_t incl_ns = 0;      // inline calls: wall time, nested inline jobs included
    uint64_t self_ns = 0;      // inline calls: wall time without nested inline jobs
    uint64_t max_ns = 0;       // inline calls: longest single call (inclusive)
};

std::mutex g_mutex; // guards the rows; one short lock per completed call
std::array<JobRow, kMaxJobs> g_rows{};
size_t g_row_count = 0;      // guarded by g_mutex
uint64_t g_other = 0;        // guarded by g_mutex: calls with no free row
std::atomic<uint64_t> g_overflow{0}; // calls not tracked because nesting exceeded kMaxDepth

void AddCall(uint64_t key, bool timed, uint64_t incl, uint64_t self) {
    std::lock_guard<std::mutex> lock(g_mutex);
    JobRow* row = nullptr;
    for (size_t i = 0; i < g_row_count; ++i) {
        if (g_rows[i].key == key) {
            row = &g_rows[i];
            break;
        }
    }
    if (!row) {
        if (g_row_count == kMaxJobs) {
            ++g_other;
            return;
        }
        row = &g_rows[g_row_count++];
        row->key = key;
    }
    row->calls += 1;
    if (timed) {
        row->inline_calls += 1;
        row->incl_ns += incl;
        row->self_ns += self;
        row->max_ns = std::max(row->max_ns, incl);
    }
}

// Open calls of this hook on this thread. A call deeper than kMaxDepth is not pushed; `overflow`
// counts it, and its post phase only decrements `overflow`, so pushes and pops stay matched.
struct Frame {
    int64_t start_ns = 0;
    int64_t child_ns = 0; // inline time of nested inline jobs (filled in by them)
    uint64_t key = 0;
    bool timed = false;   // inline call: the body runs inside this call
};

struct FrameStack {
    Frame frames[kMaxDepth];
    int depth = 0;
    int overflow = 0;
};

thread_local FrameStack t_jobs;

SotcHooks::HookAction JobPre(GuestRegs& regs, void*) {
    FrameStack& s = t_jobs;
    if (s.depth >= kMaxDepth) {
        ++s.overflow;
        g_overflow.fetch_add(1, std::memory_order_relaxed);
        return HookAction::Continue;
    }
    // ecx = the flags (r13d in the function): bit 0 set or bits 1-2 clear means the job is queued.
    const uint32_t flags = static_cast<uint32_t>(regs.rcx);
    const bool timed = (flags & 1) == 0 && (flags & 6) != 0;
    Frame& f = s.frames[s.depth++];
    f.key = BodyOffset(static_cast<uintptr_t>(regs.rdi), static_cast<uint64_t>(SotcHooks::ImageBase()));
    f.timed = timed;
    f.child_ns = 0;
    f.start_ns = timed ? NowNs() : 0;
    return HookAction::Continue;
}

void JobPost(GuestRegs&, void*) {
    FrameStack& s = t_jobs;
    if (s.overflow > 0) {
        --s.overflow;
        return;
    }
    if (s.depth == 0) {
        return;
    }
    const Frame f = s.frames[--s.depth];
    uint64_t incl = 0, self = 0;
    if (f.timed) {
        const int64_t now = NowNs();
        incl = static_cast<uint64_t>(std::max<int64_t>(0, now - f.start_ns));
        const uint64_t child = static_cast<uint64_t>(std::max<int64_t>(0, f.child_ns));
        self = incl > child ? incl - child : 0;
        // The nearest inline ancestor does not count this job's time as its own.
        for (int i = s.depth - 1; i >= 0; --i) {
            if (s.frames[i].timed) {
                s.frames[i].child_ns += static_cast<int64_t>(incl);
                break;
            }
        }
    }
    AddCall(f.key, f.timed, incl, self);
}

} // namespace

void RegisterDrawTag() {
    SotcHooks::Handler job;
    job.pre = JobPre;
    job.post = JobPost;
    SotcHooks::RegisterHandler("RENDER_JOB_RUN", job);
}

void ReportDrawTag(std::string& out, double seconds) {
    if (seconds <= 0.0) {
        seconds = 1.0;
    }
    std::array<JobRow, kMaxJobs> rows{};
    size_t row_count = 0;
    uint64_t other = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        row_count = g_row_count;
        std::copy_n(g_rows.begin(), row_count, rows.begin());
        other = g_other;
        g_row_count = 0;
        g_other = 0;
    }
    const uint64_t overflow = g_overflow.exchange(0, std::memory_order_relaxed);
    if (row_count == 0) {
        return;
    }

    uint64_t calls = 0, inline_calls = 0, incl_total = 0;
    for (size_t i = 0; i < row_count; ++i) {
        calls += rows[i].calls;
        inline_calls += rows[i].inline_calls;
        incl_total += rows[i].incl_ns;
    }
    // Inline time first: that is the number the question needs. Ties keep the call count order.
    std::sort(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(row_count),
              [](const JobRow& a, const JobRow& b) {
                  if (a.self_ns != b.self_ns) {
                      return a.self_ns > b.self_ns;
                  }
                  return a.calls > b.calls;
              });

    char head[256];
    std::snprintf(head, sizeof head,
                  "SOTC DRAWTAG: %llu render jobs (%.1f/s), %llu inline, inline total %.2f ms; "
                  "%zu bodies",
                  static_cast<unsigned long long>(calls), calls / seconds,
                  static_cast<unsigned long long>(inline_calls), incl_total / 1e6, row_count);
    std::string line = head;
    line += " | top by self time:";
    const size_t shown = std::min(kTopRows, row_count);
    for (size_t i = 0; i < shown; ++i) {
        const JobRow& r = rows[i];
        char item[200];
        if (r.key == 0) {
            std::snprintf(item, sizeof item, " ? %llux (inline %llux, %.2f ms self)",
                          static_cast<unsigned long long>(r.calls),
                          static_cast<unsigned long long>(r.inline_calls), r.self_ns / 1e6);
        } else {
            std::snprintf(item, sizeof item,
                          " 0x%llx %llux (inline %llux, incl %.2f ms, self %.2f ms, max %.2f ms)",
                          static_cast<unsigned long long>(r.key),
                          static_cast<unsigned long long>(r.calls),
                          static_cast<unsigned long long>(r.inline_calls), r.incl_ns / 1e6,
                          r.self_ns / 1e6, r.max_ns / 1e6);
        }
        line += item;
        line += ';';
    }
    if (row_count > shown) {
        line += " +" + std::to_string(row_count - shown) + " more;";
    }
    if (other) {
        line += " untracked bodies " + std::to_string(other) + " calls;";
    }
    if (overflow) {
        line += " untracked " + std::to_string(overflow) + " (nesting);";
    }
    out += line;
    out += '\n';
}

} // namespace SotcProbes
