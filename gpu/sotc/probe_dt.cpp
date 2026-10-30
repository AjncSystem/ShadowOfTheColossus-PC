// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: delta-time probe (report 05, section 2.3-2.4, hooks H1 and H2). Two hooks (scripts/hooks.d/dt.py),
// switch SOTC_DT_PROBE:
//   DT_CONSUMER  wrap at sub_25ccc0 (0x25ccc0). rdi = the timer object T, esi = the reset flag.
//                Pre: T+0x18 (prev, the TSC seconds of the last call) and T+0x40 (call count).
//                Post: T+0x18 is now (the TSC seconds of this call), T+0x28 the output dt, T+0x2c
//                the carry, which the consumer stores as its clamped output x (0x25ce37).
//                The incoming real dt is now - prev, for every call after the first (cnt != 0).
//   DT_UPDATE    site at 0x534a73, vmovss xmm0,[rax+0x48]. rax = the time object; the float at
//                rax + dt_offset (derived, 0x48) is the dt the update thread uses.
// Cap: the consumer clamps to *(float*)(*(uint64*)(obj+0x60)), obj = the gameMaxDeltaTime cvar object
// (the CVAR_MAX_DELTA range hook's "storage" field, scripts/hooks.d/cvars.py). The same two loads are
// made by sub_25ccc0 at 0x25ce14-0x25ce1d. Without that hook resolved the cap is unknown.
//
// Guest reads: the timer object is either heap (backing view) or on the calling thread's own stack
// (not GPU-tracked; read directly only within 64 KiB above the handler's rsp). The update float is in
// the time object on the heap, through the backing view only. A value that cannot be read safely is
// counted as unreadable and not guessed. Observe only: every handler returns HookAction::Continue.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "hooks.h"
#include "probe_dt.h"

extern "C" {
// runtime_memory.c: the unprotected backing view of a guest address, and the mapped region of one.
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
}

namespace SotcProbes {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr uint64_t kTimerPrevOffset = 0x18;  // T: double, TSC seconds of the last call (now after a call)
constexpr uint64_t kTimerDtOffset = 0x28;    // T: float, output dt
constexpr uint64_t kTimerCarryOffset = 0x2c; // T: float, carry = the clamped output x
constexpr uint64_t kTimerCountOffset = 0x40; // T: int32, call count (0 on the first call)
constexpr uint64_t kCvarStorageOffset = 0x60; // cvar object: pointer to the engine's live float
constexpr uint64_t kImageSpan = 256ull * 1024 * 1024; // guard for a pointer into the mapped image
constexpr uint64_t kStackWindow = 0x10000;   // a timer object this close above the handler's rsp is on its stack
constexpr size_t kCapacity = 4096;           // samples kept per series and window
constexpr double kNearFraction = 0.99;       // "at or near the cap": dt >= 0.99 * cap

// Reads n bytes at a guest address. Backing view first (heap, GPU-tracked pages included); when the
// address is not in a mapped region, the calling thread's own stack just above rsp (committed, never
// GPU-tracked) when allow_stack is set. False otherwise.
bool ReadGuest(uint64_t address, uint64_t rsp, void* out, size_t n, bool allow_stack) {
    if (address == 0) {
        return false;
    }
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
    if (allow_stack && rsp != 0 && address >= rsp && address - rsp < kStackWindow &&
        address + n - rsp <= kStackWindow) {
        std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(address)), n);
        return true;
    }
    return false;
}

// A host address inside the mapped eboot image.
bool InImage(uint64_t address, size_t n) {
    const uint64_t base = SotcHooks::ImageBase();
    return base != 0 && address >= base && n <= kImageSpan && address - base <= kImageSpan - n;
}

// The current gameMaxDeltaTime value, read exactly as sub_25ccc0 reads it. False when the cvar hook
// (CVAR_MAX_DELTA) is not resolved or the pointers are not in the image.
bool ReadCap(float* cap) {
    uint64_t image_off = 0;
    const uint64_t base = SotcHooks::ImageBase();
    if (base == 0 || !SotcHooks::Derived("CVAR_MAX_DELTA", "storage", &image_off)) {
        return false;
    }
    const uint64_t obj = base + image_off;
    if (!InImage(obj + kCvarStorageOffset, sizeof(uint64_t))) {
        return false;
    }
    uint64_t storage = 0;
    std::memcpy(&storage, reinterpret_cast<const void*>(static_cast<uintptr_t>(obj + kCvarStorageOffset)),
                sizeof storage);
    if (!InImage(storage, sizeof(float))) {
        return false;
    }
    std::memcpy(cap, reinterpret_cast<const void*>(static_cast<uintptr_t>(storage)), sizeof *cap);
    return std::isfinite(*cap) && *cap > 0.0f;
}

// One series of samples for the current window, plus its counters. Written by a guest thread and
// read by the report (driver thread), so every access takes the mutex. Only used while the switch is on.
struct Series {
    std::mutex mutex;
    std::array<float, kCapacity> values{};
    size_t kept = 0;
    uint64_t seen = 0;       // samples offered this window (kept or dropped)
    uint64_t near_cap = 0;   // samples with dt >= kNearFraction * cap
    uint64_t clamped = 0;    // consumer only: samples where the clamp fired (carry >= cap)
    uint64_t unreadable = 0; // samples whose value could not be read safely

    void Add(float v, bool near, bool was_clamped) {
        std::lock_guard<std::mutex> lock(mutex);
        ++seen;
        if (kept < kCapacity) {
            values[kept++] = v;
        }
        if (near) {
            ++near_cap;
        }
        if (was_clamped) {
            ++clamped;
        }
    }

    void CountUnreadable() {
        std::lock_guard<std::mutex> lock(mutex);
        ++unreadable;
    }
};

struct Window {
    std::vector<float> values; // sorted
    uint64_t seen = 0, near_cap = 0, clamped = 0, unreadable = 0;
};

// Copies one series out and resets it.
Window Take(Series& s) {
    Window w;
    std::lock_guard<std::mutex> lock(s.mutex);
    w.values.assign(s.values.begin(), s.values.begin() + static_cast<std::ptrdiff_t>(s.kept));
    w.seen = s.seen;
    w.near_cap = s.near_cap;
    w.clamped = s.clamped;
    w.unreadable = s.unreadable;
    s.kept = 0;
    s.seen = 0;
    s.near_cap = 0;
    s.clamped = 0;
    s.unreadable = 0;
    std::sort(w.values.begin(), w.values.end());
    return w;
}

Series g_out;      // DT_CONSUMER: output dt (post, T+0x28)
Series g_real;     // DT_CONSUMER: real elapsed time since the previous call (now - prev)
Series g_update;   // DT_UPDATE: the update's dt float

// State of one consumer call, kept from the pre phase to the post phase on the same guest thread.
struct CallState {
    uint64_t timer = 0;
    uint64_t rsp = 0;
    double prev = 0.0;
    int32_t count = 0;
    bool valid = false;
};
thread_local CallState t_consumer;

// The pre phase stores the state; the post phase on the same thread reads it and clears it.
HookAction ConsumerPre(GuestRegs& regs, void*) {
    CallState& s = t_consumer;
    s = CallState{};
    s.timer = regs.rdi;
    s.rsp = regs.rsp;
    double prev = 0.0;
    int32_t count = 0;
    if (ReadGuest(s.timer + kTimerPrevOffset, s.rsp, &prev, sizeof prev, true) &&
        ReadGuest(s.timer + kTimerCountOffset, s.rsp, &count, sizeof count, true)) {
        s.prev = prev;
        s.count = count;
        s.valid = true;
    }
    return HookAction::Continue;
}

void ConsumerPost(GuestRegs&, void*) {
    const CallState s = t_consumer;
    t_consumer.valid = false;
    if (!s.valid) {
        g_out.CountUnreadable();
        return;
    }
    double now = 0.0;
    float dt = 0.0f, carry = 0.0f;
    if (!ReadGuest(s.timer + kTimerPrevOffset, s.rsp, &now, sizeof now, true) ||
        !ReadGuest(s.timer + kTimerDtOffset, s.rsp, &dt, sizeof dt, true) ||
        !ReadGuest(s.timer + kTimerCarryOffset, s.rsp, &carry, sizeof carry, true)) {
        g_out.CountUnreadable();
        return;
    }
    float cap = 0.0f;
    const bool have_cap = ReadCap(&cap);
    const bool near = have_cap && dt >= static_cast<float>(kNearFraction) * cap;
    const bool clamped = have_cap && carry >= cap;
    g_out.Add(dt, near, clamped);
    if (s.count != 0) {
        // The first call (count 0) re-bases prev to now - F, so its elapsed time is not a measurement.
        g_real.Add(static_cast<float>(now - s.prev), false, false);
    }
}

HookAction UpdatePre(GuestRegs& regs, void*) {
    uint64_t dt_offset = 0;
    if (!SotcHooks::Derived("DT_UPDATE", "dt_offset", &dt_offset)) {
        g_update.CountUnreadable();
        return HookAction::Continue;
    }
    // rax is the time object at the site (the vmovss that follows reads [rax + dt_offset]).
    float dt = 0.0f;
    if (!ReadGuest(regs.rax + dt_offset, regs.rsp, &dt, sizeof dt, false)) {
        g_update.CountUnreadable();
        return HookAction::Continue;
    }
    float cap = 0.0f;
    const bool have_cap = ReadCap(&cap);
    const bool near = have_cap && dt >= static_cast<float>(kNearFraction) * cap;
    g_update.Add(dt, near, false);
    return HookAction::Continue;
}

// "min/med/max" of a sorted window, or "n/a" when it is empty.
std::string Stats(const std::vector<float>& v) {
    if (v.empty()) {
        return "n/a";
    }
    const size_t n = v.size();
    const double median = n % 2 ? v[n / 2] : 0.5 * (static_cast<double>(v[n / 2 - 1]) + v[n / 2]);
    char buf[96];
    std::snprintf(buf, sizeof buf, "%.4f/%.4f/%.4f", static_cast<double>(v.front()), median,
                  static_cast<double>(v.back()));
    return buf;
}

} // namespace

void RegisterDtProbe() {
    // Registered unconditionally: a hook that is off never dispatches to its handler.
    SotcHooks::Handler consumer;
    consumer.pre = &ConsumerPre;
    consumer.post = &ConsumerPost;
    SotcHooks::RegisterHandler("DT_CONSUMER", consumer);

    SotcHooks::Handler update;
    update.pre = &UpdatePre;
    SotcHooks::RegisterHandler("DT_UPDATE", update);
}

void ReportDtProbe(std::string& out, double seconds) {
    const bool consumer_on = SotcHooks::IsInstalled("DT_CONSUMER");
    const bool update_on = SotcHooks::IsInstalled("DT_UPDATE");
    if (!consumer_on && !update_on) {
        return;
    }
    if (seconds <= 0.0) {
        seconds = 1.0;
    }
    float cap = 0.0f;
    const bool have_cap = ReadCap(&cap);

    char head[96];
    std::snprintf(head, sizeof head, "SOTC DT %.1f s: ", seconds);
    std::string line = head;
    if (have_cap) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "cap %.4f s; ", static_cast<double>(cap));
        line += buf;
    } else {
        line += "cap unknown (SOTC_HOOK_CVAR_PROBE=1 resolves it); ";
    }

    if (consumer_on) {
        Window out_w = Take(g_out);
        Window real_w = Take(g_real);
        char buf[320];
        std::snprintf(buf, sizeof buf,
                      "consumer calls %llu, clamped %llu, at cap %llu, unreadable %llu, dt out min/med/max %s, real min/med/max %s%s; ",
                      static_cast<unsigned long long>(out_w.seen),
                      static_cast<unsigned long long>(out_w.clamped),
                      static_cast<unsigned long long>(out_w.near_cap),
                      static_cast<unsigned long long>(out_w.unreadable), Stats(out_w.values).c_str(),
                      Stats(real_w.values).c_str(),
                      out_w.seen > kCapacity ? " (samples capped)" : "");
        line += buf;
    }
    if (update_on) {
        Window upd = Take(g_update);
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "update calls %llu, at cap %llu, unreadable %llu, dt min/med/max %s",
                      static_cast<unsigned long long>(upd.seen),
                      static_cast<unsigned long long>(upd.near_cap),
                      static_cast<unsigned long long>(upd.unreadable), Stats(upd.values).c_str());
        line += buf;
    }
    out += line;
    out += '\n';
}

} // namespace SotcProbes
