// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: BuildSync probe (plan B1, docs/re-stutters/reports2/17-buildsync-callers.md). Observe only;
// hook declarations in scripts/hooks.d/buildsync.py, switch SOTC_BUILDSYNC_PROBE.
//
//   BUILDSYNC_BUILD   wrap at ResourceFactory::BuildSync (0x146b60). Pre: the caller chain (return
//                     addresses at [rsp] and up the frame-pointer chain, 4 levels) and the start
//                     time. Post: the wall time. Counted per thread: sync builds, total and max ms.
//   BUILDSYNC_ENSURE  wrap at EnsureBuilt (0x14d7b0). Pre: rdi is the wrapper (the function moves it
//                     to rbx at once). Counted per thread: calls, and how often the sync precondition
//                     held: state [+0x98] == 0xb, no data [+0x20] == 0, [+0x60] == 0. The function's
//                     static dummy check is not part of the count.
//
// Threads are keyed by the name the port sets (Common::GetCurrentThreadName), read once per thread.
// The caller chain is aggregated per thread and chain (image offsets), so the report names the call
// sites, not single calls. Guest reads are guarded: the stack walk stays inside the committed stack
// window (the guest stack is not in the GPU backing view), and the wrapper goes through the backing
// view like the allocator probe does, with a direct read for the image's static wrappers.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "common/thread.h"            // Common::GetCurrentThreadName (shadPS4 common)
#include "hooks.h"
#include "probe_buildsync.h"
#include "video_core/sotc_trace.h"    // image bounds (SotcTrace::ImageBase / ImageSize)

extern "C" {
// runtime_memory.c: the unprotected backing view of a guest address, and the mapped region of one.
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
}

namespace SotcBuildSync {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr int kChainDepth = 4;        // return addresses kept per call: [rsp], then 3 frame levels
constexpr int kMaxDepth = 8;          // nested BuildSync calls per thread (defensive)
constexpr size_t kMaxThreads = 16;    // distinct threads with their own row; the rest share g_other
constexpr size_t kNameMax = 40;
constexpr size_t kMaxChains = 32;     // distinct (thread, call chain) rows per window
constexpr uint64_t kStackSpan = 16ull << 20; // frame pointers must stay within 16 MiB above rsp

// Wrapper fields read by EnsureBuilt's precondition (sub_14d7b0).
constexpr uint64_t kStateOffset = 0x98; // dword: must be 0xb
constexpr uint64_t kDataOffset = 0x20;  // qword: must be 0
constexpr uint64_t kFlagOffset = 0x60;  // dword: must be 0
constexpr size_t kWrapperSpan = kStateOffset + 4; // one read covers all three fields
constexpr uint32_t kReadyState = 0xb;

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// SOTC_BUILDSYNC_PROBE, read once. The hooks only dispatch while the switch is on; the report checks it too.
bool SwitchOn() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_BUILDSYNC_PROBE");
        return v && *v && std::strtol(v, nullptr, 10) != 0;
    }();
    return on;
}

// ---------------------------------------------------------------------------
// Per-thread rows. The counters are atomics because the report thread reads and resets them.
// ---------------------------------------------------------------------------

struct ThreadRow {
    char name[kNameMax] = {0};
    std::atomic<uint64_t> ensure_calls{0};
    std::atomic<uint64_t> ensure_held{0};       // precondition held
    std::atomic<uint64_t> ensure_unreadable{0}; // wrapper not readable through the backing view or image
    std::atomic<uint64_t> builds{0};            // BuildSync calls (sync builds)
    std::atomic<uint64_t> build_ns{0};          // total wall time in BuildSync
    std::atomic<uint64_t> build_ns_max{0};      // max wall time of one BuildSync
};

std::mutex g_slot_mutex; // guards the name of each row and the row count (first call per thread only)
std::array<ThreadRow, kMaxThreads> g_rows{};
size_t g_row_count = 0;
ThreadRow g_other;       // threads beyond kMaxThreads
thread_local int t_slot = -1; // row index of this thread, kMaxThreads for g_other

int RowIndex() {
    if (t_slot < 0) {
        std::lock_guard<std::mutex> lock(g_slot_mutex);
        if (g_row_count < kMaxThreads) {
            ThreadRow& row = g_rows[g_row_count];
            std::string name = Common::GetCurrentThreadName();
            if (name.empty() || name == "<unknown name>") {
                name = "thread" + std::to_string(g_row_count);
            }
            std::snprintf(row.name, kNameMax, "%s", name.c_str());
            t_slot = static_cast<int>(g_row_count++);
        } else {
            t_slot = static_cast<int>(kMaxThreads);
        }
    }
    return t_slot;
}

ThreadRow& RowAt(int index) {
    return index < static_cast<int>(kMaxThreads) ? g_rows[static_cast<size_t>(index)] : g_other;
}

// ---------------------------------------------------------------------------
// Guest reads. Stack words: guarded by the stack window and alignment, read directly (the guest stack
// is committed and not GPU-tracked). Wrapper: the backing view (protected pages), then the image.
// ---------------------------------------------------------------------------

bool InStack(uint64_t address, uint64_t size, uint64_t sp) {
    return address >= sp && address - sp <= kStackSpan - size && (address & 7) == 0;
}

bool ReadStackWord(uint64_t address, uint64_t sp, uint64_t* out) {
    if (!InStack(address, sizeof *out, sp)) {
        return false;
    }
    std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(address)), sizeof *out);
    return true;
}

// Image offset of a return address, or 0 when it is not in the loaded image.
uint64_t ImageOffset(uint64_t address) {
    const uint64_t base = SotcTrace::ImageBase();
    const uint64_t size = SotcTrace::ImageSize();
    if (base == 0 || size == 0 || address < base || address - base >= size) {
        return 0;
    }
    return address - base;
}

// Reads n bytes of a guest object: the backing view first, then the image. False when neither holds
// the whole range.
bool ReadObject(uint64_t address, void* out, size_t n) {
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (runtime_memory_region(static_cast<uintptr_t>(address), &start, &end, &mapped) && mapped &&
        address >= start && address + n <= end) {
        const unsigned char* p = runtime_memory_backing_pointer(static_cast<uintptr_t>(address));
        if (p) {
            std::memcpy(out, p, n);
            return true;
        }
    }
    const uint64_t base = SotcTrace::ImageBase();
    const uint64_t size = SotcTrace::ImageSize();
    if (base != 0 && size != 0 && address >= base && n <= size && address - base <= size - n) {
        std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(address)), n);
        return true;
    }
    return false;
}

// Caller chain at a function entry. site[0] = the return address at [rsp] (the immediate caller).
// site[k] for k >= 1 walks the frame-pointer chain: the caller's frame is at regs.rbp (the function
// has not set up its own frame yet), [fp+8] is its return address and [fp] the next frame pointer.
// Every read is inside the stack window, and the chain must climb the stack. Unused levels stay 0.
void WalkChain(const GuestRegs& regs, uint64_t* site) {
    for (int i = 0; i < kChainDepth; ++i) {
        site[i] = 0;
    }
    const uint64_t sp = regs.rsp;
    uint64_t ret = 0;
    if (!ReadStackWord(sp, sp, &ret)) {
        return;
    }
    site[0] = ImageOffset(ret);
    uint64_t fp = regs.rbp;
    for (int level = 1; level < kChainDepth; ++level) {
        uint64_t next_ret = 0;
        uint64_t next_fp = 0;
        if (!ReadStackWord(fp + 8, sp, &next_ret) || !ReadStackWord(fp, sp, &next_fp)) {
            return;
        }
        site[level] = ImageOffset(next_ret);
        if (next_fp <= fp) {
            return; // the chain must climb: a stale or foreign frame ends it
        }
        fp = next_fp;
    }
}

// ---------------------------------------------------------------------------
// BuildSync wrap: pre keeps the chain and the start time on this thread's stack, post finishes it.
// ---------------------------------------------------------------------------

struct BuildFrame {
    int64_t start_ns = 0;
    uint64_t site[kChainDepth] = {0};
};

struct BuildStack {
    BuildFrame frames[kMaxDepth];
    int depth = 0;
    int overflow = 0;
};

thread_local BuildStack t_build;

// Per (thread, chain) rows of the current window. Filled in post, under the lock; a sync build is
// rare next to EnsureBuilt calls, so the lock is not on the common path.
struct ChainRow {
    int slot = -1;
    uint64_t site[kChainDepth] = {0};
    uint64_t count = 0;
    uint64_t ns_total = 0;
};

std::mutex g_chain_mutex;
std::array<ChainRow, kMaxChains> g_chains{};
size_t g_chain_count = 0;
uint64_t g_chain_dropped = 0; // builds whose chain had no row (table full)

void RecordChain(int slot, const uint64_t* site, uint64_t ns) {
    std::lock_guard<std::mutex> lock(g_chain_mutex);
    for (size_t i = 0; i < g_chain_count; ++i) {
        ChainRow& c = g_chains[i];
        if (c.slot == slot && std::memcmp(c.site, site, sizeof c.site) == 0) {
            c.count += 1;
            c.ns_total += ns;
            return;
        }
    }
    if (g_chain_count == kMaxChains) {
        ++g_chain_dropped;
        return;
    }
    ChainRow& c = g_chains[g_chain_count++];
    c.slot = slot;
    std::memcpy(c.site, site, sizeof c.site);
    c.count = 1;
    c.ns_total = ns;
}

HookAction BuildPre(GuestRegs& regs, void*) {
    BuildStack& s = t_build;
    if (s.depth >= kMaxDepth) {
        ++s.overflow;
        return HookAction::Continue;
    }
    BuildFrame& f = s.frames[s.depth++];
    WalkChain(regs, f.site);
    f.start_ns = NowNs();
    return HookAction::Continue;
}

void BuildPost(GuestRegs&, void*) {
    const int64_t now = NowNs();
    BuildStack& s = t_build;
    if (s.overflow > 0) {
        --s.overflow;
        return;
    }
    if (s.depth == 0) {
        return;
    }
    const BuildFrame f = s.frames[--s.depth];
    const uint64_t ns = static_cast<uint64_t>(now > f.start_ns ? now - f.start_ns : 0);

    const int slot = RowIndex();
    ThreadRow& row = RowAt(slot);
    row.builds.fetch_add(1, std::memory_order_relaxed);
    row.build_ns.fetch_add(ns, std::memory_order_relaxed);
    uint64_t cur = row.build_ns_max.load(std::memory_order_relaxed);
    while (ns > cur && !row.build_ns_max.compare_exchange_weak(cur, ns, std::memory_order_relaxed)) {
    }
    RecordChain(slot, f.site, ns);
}

// ---------------------------------------------------------------------------
// EnsureBuilt wrap: pre reads the wrapper (rdi) and counts the sync precondition.
// ---------------------------------------------------------------------------

HookAction EnsurePre(GuestRegs& regs, void*) {
    ThreadRow& row = RowAt(RowIndex());
    row.ensure_calls.fetch_add(1, std::memory_order_relaxed);
    const uint64_t wrapper = regs.rdi;
    if (wrapper == 0) {
        row.ensure_unreadable.fetch_add(1, std::memory_order_relaxed);
        return HookAction::Continue;
    }
    unsigned char buf[kWrapperSpan] = {0};
    if (!ReadObject(wrapper, buf, sizeof buf)) {
        row.ensure_unreadable.fetch_add(1, std::memory_order_relaxed);
        return HookAction::Continue;
    }
    uint32_t state = 0;
    uint64_t data = 0;
    uint32_t flag = 0;
    std::memcpy(&state, buf + kStateOffset, sizeof state);
    std::memcpy(&data, buf + kDataOffset, sizeof data);
    std::memcpy(&flag, buf + kFlagOffset, sizeof flag);
    if (state == kReadyState && data == 0 && flag == 0) {
        row.ensure_held.fetch_add(1, std::memory_order_relaxed);
    }
    return HookAction::Continue;
}

// Chain text for the report: "0xSITE0<-0xSITE1<-..." (image offsets; empty levels dropped).
std::string ChainText(const uint64_t* site) {
    std::string text;
    for (int i = 0; i < kChainDepth; ++i) {
        if (site[i] == 0) {
            continue;
        }
        char part[32];
        std::snprintf(part, sizeof part, "%s0x%llx", text.empty() ? "" : "<-",
                      static_cast<unsigned long long>(site[i]));
        text += part;
    }
    return text.empty() ? std::string("?") : text;
}

} // namespace

void RegisterBuildSyncProbe() {
    SotcHooks::Handler build;
    build.pre = &BuildPre;
    build.post = &BuildPost;
    SotcHooks::RegisterHandler("BUILDSYNC_BUILD", build);

    SotcHooks::Handler ensure;
    ensure.pre = &EnsurePre;
    SotcHooks::RegisterHandler("BUILDSYNC_ENSURE", ensure);
}

void ReportBuildSyncProbe(std::string& out, double seconds) {
    if (!SwitchOn()) {
        return;
    }
    if (seconds <= 0.0) {
        seconds = 1.0;
    }

    // Snapshot the rows and reset the window under the slot lock, then format without it.
    struct Snap {
        char name[kNameMax] = {0};
        uint64_t ensure_calls = 0, ensure_held = 0, ensure_unreadable = 0;
        uint64_t builds = 0, build_ns = 0, build_ns_max = 0;
    };
    std::array<Snap, kMaxThreads + 1> snaps{};
    {
        std::lock_guard<std::mutex> lock(g_slot_mutex);
        const size_t count = g_row_count;
        for (size_t i = 0; i <= kMaxThreads; ++i) {
            ThreadRow& r = i < kMaxThreads ? g_rows[i] : g_other;
            Snap& s = snaps[i];
            if (i < count) {
                std::snprintf(s.name, kNameMax, "%s", r.name);
            } else if (i == kMaxThreads) {
                std::snprintf(s.name, kNameMax, "%s", "other");
            }
            s.ensure_calls = r.ensure_calls.exchange(0, std::memory_order_relaxed);
            s.ensure_held = r.ensure_held.exchange(0, std::memory_order_relaxed);
            s.ensure_unreadable = r.ensure_unreadable.exchange(0, std::memory_order_relaxed);
            s.builds = r.builds.exchange(0, std::memory_order_relaxed);
            s.build_ns = r.build_ns.exchange(0, std::memory_order_relaxed);
            s.build_ns_max = r.build_ns_max.exchange(0, std::memory_order_relaxed);
        }
    }

    std::array<ChainRow, kMaxChains> chains{};
    size_t chain_count = 0;
    uint64_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(g_chain_mutex);
        chain_count = g_chain_count;
        for (size_t i = 0; i < chain_count; ++i) {
            chains[i] = g_chains[i];
        }
        g_chain_count = 0;
        dropped = g_chain_dropped;
        g_chain_dropped = 0;
    }

    // One line per window: every thread with calls, sync builds or unreadable wrappers.
    bool any = false;
    std::string line;
    for (size_t i = 0; i <= kMaxThreads; ++i) {
        const Snap& s = snaps[i];
        if (s.ensure_calls == 0 && s.builds == 0 && s.ensure_unreadable == 0) {
            continue;
        }
        if (!any) {
            char head[64];
            std::snprintf(head, sizeof head, "SOTC BUILDSYNC probe (%.0f s):", seconds);
            line += head;
            any = true;
        }
        char unreadable[32] = "";
        if (s.ensure_unreadable) {
            std::snprintf(unreadable, sizeof unreadable, ", unreadable %llu",
                          static_cast<unsigned long long>(s.ensure_unreadable));
        }
        char part[256];
        std::snprintf(part, sizeof part,
                      " %s: EnsureBuilt %llu (sync precondition %llu%s), sync builds %llu, "
                      "%.2f ms total, %.2f ms max;",
                      s.name, static_cast<unsigned long long>(s.ensure_calls),
                      static_cast<unsigned long long>(s.ensure_held), unreadable,
                      static_cast<unsigned long long>(s.builds), s.build_ns / 1e6,
                      s.build_ns_max / 1e6);
        line += part;
    }
    if (any) {
        line += "\n";
        out += line;
    }

    if (chain_count == 0 && dropped == 0) {
        return;
    }
    // Most frequent chains first; at most 4 per window so the line stays short.
    std::array<size_t, kMaxChains> order{};
    for (size_t i = 0; i < chain_count; ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(chain_count),
              [&](size_t a, size_t b) { return chains[a].count > chains[b].count; });
    std::string chain_line = "SOTC BUILDSYNC chains:";
    const size_t shown = chain_count < 4 ? chain_count : 4;
    for (size_t k = 0; k < shown; ++k) {
        const ChainRow& c = chains[order[k]];
        const size_t row = c.slot >= 0 && c.slot < static_cast<int>(kMaxThreads)
                               ? static_cast<size_t>(c.slot)
                               : kMaxThreads;
        const std::string chain = ChainText(c.site);
        char part[256];
        std::snprintf(part, sizeof part, " [%s] %s x%llu, %.2f ms;", snaps[row].name, chain.c_str(),
                      static_cast<unsigned long long>(c.count), c.ns_total / 1e6);
        chain_line += part;
    }
    if (dropped) {
        char part[64];
        std::snprintf(part, sizeof part, " %llu builds without a chain row;",
                      static_cast<unsigned long long>(dropped));
        chain_line += part;
    }
    chain_line += "\n";
    out += chain_line;
}

} // namespace SotcBuildSync
