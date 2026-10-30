// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the engine's own profiler scopes as trace spans (trace layer L3b). Switch SOTC_TRACE_SCOPES;
// hooks PROFILER_PUSH_SCOPE and PROFILER_POP_SCOPE (scripts/hooks.d/profiler_scopes.py), both entries.
// Observe only: every handler returns HookAction::Continue and nothing in the guest is written.
//
// Reverse engineering (EU 1.01 eboot, tools/re/eboot_re.py dis/sig/callers, out/re/decomp):
//
// Profiler::PushScope 0x16b8c0 (50 bytes), Profiler::PopScope 0x16b900 (50 bytes).
//   PushScope(rdi = scope object, rsi = name, rdx = ctx, ecx = category bits):
//     - stores ctx at [rdi] and the bits at [rdi+8] (the scope object is a 16-byte caller stack local);
//     - checks the engine's enable mask: (ctx[0x48]->[+0x28] & bits). The check is inside the function,
//       after the store, so the entry hook runs for every call, enabled or not;
//     - when enabled, tail-jumps to sub_86f2a0(ctx[0], rsi) (ctx[0] non-null) or to sub_884f10(ctx[1]+0x10,
//       rsi). Both strlen() the name and insert it as a GNM push marker (sceGnmInsertPushMarker) into a
//       command buffer. So rsi is the NUL-terminated name, forwarded unchanged.
//   PopScope(rdi = the same scope object):
//     - reads ctx from [rdi] and the bits from [rdi+8]; the same enable test runs again (the mask may have
//       changed since the push); then tail-jumps to sub_86f3a0 / sub_885010 = sceGnmInsertPopMarker(6 dwords).
//     - No name. A pop removes the newest marker of that command buffer (LIFO in the GPU stream); the
//       scope object is only the key of the command buffer. Pairing is by the scope object (rdi) here.
//   Name argument: rsi holds a const char* in every call site that was read (169 decompiled functions
//     call PushScope). Most are string literals in the image ("RenderShadows", "ApplyPostFX", ...). Some
//     are char* from data tables (PTR_s_DecalLayer2..., (&PTR_s_kRS_Depth_Occluders)[param_4]) and some are
//     char* values of runtime fields (param_1[0xe], lVar25, pcVar2), which may point to heap. There is no
//     hashed id and no struct with a name pointer: the handler reads the bytes at rsi (image directly,
//     other addresses through the backing view), never trusts the pointer.
//   Category bits: ecx is 1 for most scopes, 2, 4 and 0x20 for some ("sort ...", "merge blocks").
//   Nesting: the scope object is a stack local and the callers nest by code structure, so calls nest per
//     thread (LIFO). The handler keeps a thread_local stack and pairs a pop with the scope object; the
//     mismatch counters check this at run time.
//   Engine profiler off: the mask test is inside both functions, so an entry hook sees every call. The
//     handler records every call (the mask only decides whether the engine inserts a marker). If the mask
//     changes between a push and its pop the engine's own marker stack unbalances; the scope-object pairing
//     here does not.
//   Threads: markers go into the command buffer the caller records. Callers include RenderView::MainCamera,
//     BPXPR_* (culling/draws), Frame::InsertWaitFlipDone, ParticleRenderer::*, RenderShadows and game-side
//     update code. The thread of each scope is not fixed statically; check the trace's thread tracks at run time.
//   Detour rules: neither function reads its return address; both finish with a tail jump; the first 6 bytes
//     (two movs, the 8-byte signature) contain no RIP-relative operand and no branch. The entry stub may
//     therefore relocate them unchanged. The handler reads [rsp] (the return address of the caller) only to
//     name the call site; it does not change it. Checked: the two functions' disassembly and the pseudocode of
//     a sample of callers, not all 169.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "hooks.h"
#include "probe_scopes.h"
#include "video_core/sotc_trace.h" // Now, Span, InternString, image bounds

extern "C" {
// runtime_memory.c: the unprotected backing view of a guest address, and the mapped region of one.
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
}

namespace SotcProbes {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr int kDepth = 64;              // open scopes per thread; deeper pushes are counted, not recorded
constexpr size_t kNameMax = 96;         // longest name read (longer names are cut)
constexpr size_t kNameSlots = 4096;     // distinct names; power of two, insert-only table
constexpr const char kUnreadable[] = "(unreadable scope)";

struct Frame {
    uint64_t scope;    // the scope object (PushScope rdi, PopScope rdi)
    uint64_t t0;       // SotcTrace::Now() at the push
    const char* name;  // interned name
    uint64_t site;     // return address of the PushScope call (eboot offset in the image)
    uint64_t category; // ecx of the push
};

thread_local Frame tl_stack[kDepth];
thread_local int tl_depth = 0;
thread_local uint32_t tl_overflow = 0; // pushes past kDepth; their pops come first (LIFO)

// Window counters (ReportScopes resets them) and totals.
std::atomic<uint64_t> g_push{0};
std::atomic<uint64_t> g_pop{0};
std::atomic<uint64_t> g_spans{0};
std::atomic<uint64_t> g_unmatched{0};   // pops with no open push on this thread
std::atomic<uint64_t> g_dropped{0};     // open inner scopes discarded by a pop of an outer one
std::atomic<uint64_t> g_overflow{0};    // pushes past kDepth
std::atomic<uint64_t> g_unreadable{0};  // names that could not be read
std::atomic<uint64_t> g_uncached{0};    // interned names not kept in the table (table full)
std::atomic<uint32_t> g_names{0};       // distinct names in the table

// Reads the NUL-terminated name at address into buf (kNameMax bytes). Returns its length, or -1 when the
// start is unreadable or the mapped region ends before a NUL. The game image is read directly; other
// addresses through the backing view of their mapped region (the same rules as probe_alloc.cpp).
int ReadName(uintptr_t address, char* buf) {
    const unsigned char* p = nullptr;
    size_t avail = 0;
    const uint64_t base = SotcTrace::ImageBase();
    const uint64_t size = SotcTrace::ImageSize();
    if (base != 0 && size != 0 && address >= base && address - base < size) {
        p = reinterpret_cast<const unsigned char*>(address);
        avail = static_cast<size_t>(base + size - address);
    } else {
        uintptr_t start = 0, end = 0;
        int mapped = 0;
        if (runtime_memory_region(address, &start, &end, &mapped) && mapped && address >= start &&
            address < end) {
            p = runtime_memory_backing_pointer(address);
            avail = static_cast<size_t>(end - address);
        }
    }
    if (!p || avail == 0) {
        return -1;
    }
    size_t n = 0;
    while (n < kNameMax - 1 && n < avail) {
        const char c = static_cast<char>(p[n]);
        if (c == '\0') {
            buf[n] = '\0';
            return static_cast<int>(n);
        }
        buf[n++] = c;
    }
    if (n >= avail) {
        return -1; // the region ended before a NUL
    }
    buf[n] = '\0'; // cut at kNameMax - 1
    return static_cast<int>(n);
}

uint64_t HashName(const char* s, size_t n) {
    uint64_t h = 1469598103934665603ULL; // FNV-1a
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<unsigned char>(s[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

// Interned names, keyed by content. Lookups take no lock: a slot is written once (hash first, then the
// text with release), and an empty text ends a probe chain. Inserts take g_insert_mutex. Keyed by content,
// not by address: the same address can hold a different name later when the name lives on the heap.
struct NameSlot {
    std::atomic<uint64_t> hash{0};
    std::atomic<const char*> text{nullptr};
};
NameSlot g_slots[kNameSlots];
std::mutex g_insert_mutex;

const char* FindName(uint64_t h, const char* s, size_t n) {
    size_t idx = static_cast<size_t>(h) & (kNameSlots - 1);
    for (size_t probes = 0; probes < kNameSlots; ++probes) {
        const char* text = g_slots[idx].text.load(std::memory_order_acquire);
        if (!text) {
            return nullptr;
        }
        if (g_slots[idx].hash.load(std::memory_order_relaxed) == h && std::strlen(text) == n &&
            std::memcmp(text, s, n) == 0) {
            return text;
        }
        idx = (idx + 1) & (kNameSlots - 1);
    }
    return nullptr;
}

const char* ResolveName(const char* s, size_t n) {
    const uint64_t h = HashName(s, n);
    if (const char* text = FindName(h, s, n)) {
        return text;
    }
    std::lock_guard<std::mutex> lock(g_insert_mutex);
    if (const char* text = FindName(h, s, n)) {
        return text;
    }
    const char* text = SotcTrace::InternString(s, n);
    if (g_names.load(std::memory_order_relaxed) >= kNameSlots * 3 / 4) {
        g_uncached.fetch_add(1, std::memory_order_relaxed);
        return text;
    }
    size_t idx = static_cast<size_t>(h) & (kNameSlots - 1);
    while (g_slots[idx].text.load(std::memory_order_relaxed) != nullptr) {
        idx = (idx + 1) & (kNameSlots - 1);
    }
    g_slots[idx].hash.store(h, std::memory_order_relaxed);
    g_slots[idx].text.store(text, std::memory_order_release);
    g_names.fetch_add(1, std::memory_order_relaxed);
    return text;
}

// PROFILER_PUSH_SCOPE. rsi = name, rdi = scope object, rsp -> the return address of the caller.
HookAction PushPre(GuestRegs& regs, void*) {
    if (!SotcTrace::kOn) {
        return HookAction::Continue;
    }
    g_push.fetch_add(1, std::memory_order_relaxed);
    if (tl_depth >= kDepth) {
        ++tl_overflow;
        g_overflow.fetch_add(1, std::memory_order_relaxed);
        return HookAction::Continue;
    }
    char buf[kNameMax];
    const char* name = kUnreadable;
    if (ReadName(static_cast<uintptr_t>(regs.rsi), buf) >= 0) {
        name = ResolveName(buf, std::strlen(buf));
    } else {
        g_unreadable.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t site = 0;
    if (regs.rsp) {
        std::memcpy(&site, reinterpret_cast<const void*>(static_cast<uintptr_t>(regs.rsp)), sizeof site);
    }
    tl_stack[tl_depth++] = Frame{regs.rdi, SotcTrace::Now(), name, site, regs.rcx};
    return HookAction::Continue;
}

// PROFILER_POP_SCOPE. rdi = the scope object of the matching push.
HookAction PopPre(GuestRegs& regs, void*) {
    if (!SotcTrace::kOn) {
        return HookAction::Continue;
    }
    g_pop.fetch_add(1, std::memory_order_relaxed);
    if (tl_overflow > 0) {
        --tl_overflow; // the newest push was past kDepth and was not recorded
        return HookAction::Continue;
    }
    const uint64_t scope = regs.rdi;
    int i = tl_depth - 1;
    while (i >= 0 && tl_stack[i].scope != scope) {
        --i;
    }
    if (i < 0) {
        g_unmatched.fetch_add(1, std::memory_order_relaxed);
        return HookAction::Continue;
    }
    if (i < tl_depth - 1) {
        g_dropped.fetch_add(static_cast<uint64_t>(tl_depth - 1 - i), std::memory_order_relaxed);
    }
    const Frame f = tl_stack[i];
    tl_depth = i;
    SotcTrace::Span(f.name, f.t0, SotcTrace::Now(), f.site, f.category);
    g_spans.fetch_add(1, std::memory_order_relaxed);
    return HookAction::Continue;
}

} // namespace

void RegisterScopes() {
    if (!SotcTrace::kOn) {
        return;
    }
    SotcHooks::Handler push;
    push.pre = PushPre;
    SotcHooks::RegisterHandler("PROFILER_PUSH_SCOPE", push);
    SotcHooks::Handler pop;
    pop.pre = PopPre;
    SotcHooks::RegisterHandler("PROFILER_POP_SCOPE", pop);
}

void ReportScopes(std::string& out, double seconds) {
    const uint64_t pushes = g_push.exchange(0, std::memory_order_relaxed);
    const uint64_t pops = g_pop.exchange(0, std::memory_order_relaxed);
    const uint64_t spans = g_spans.exchange(0, std::memory_order_relaxed);
    const uint64_t unmatched = g_unmatched.exchange(0, std::memory_order_relaxed);
    const uint64_t dropped = g_dropped.exchange(0, std::memory_order_relaxed);
    const uint64_t overflow = g_overflow.exchange(0, std::memory_order_relaxed);
    const uint64_t unreadable = g_unreadable.exchange(0, std::memory_order_relaxed);
    const uint64_t uncached = g_uncached.exchange(0, std::memory_order_relaxed);
    if (pushes == 0 && pops == 0) {
        return;
    }
    if (seconds <= 0.0) {
        seconds = 1.0;
    }
    char line[320];
    std::snprintf(line, sizeof line,
                  "SOTC SCOPES: push %llu (%.0f/s) pop %llu spans %llu unmatched %llu dropped %llu "
                  "overflow %llu unreadable %llu names %u uncached %llu\n",
                  static_cast<unsigned long long>(pushes), static_cast<double>(pushes) / seconds,
                  static_cast<unsigned long long>(pops), static_cast<unsigned long long>(spans),
                  static_cast<unsigned long long>(unmatched), static_cast<unsigned long long>(dropped),
                  static_cast<unsigned long long>(overflow), static_cast<unsigned long long>(unreadable),
                  static_cast<unsigned>(g_names.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(uncached));
    out += line;
}

} // namespace SotcProbes
