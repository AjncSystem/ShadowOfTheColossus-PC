// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: XPR probe (plan step 0c, docs/re-stutters/reports/01-xpr-cull.md, 02-gpu-cpu-sync.md).
// Question: does the CPU read a visibility mask before the GPU has written the cull's completion
// label? XPR_CS_DONE_LABEL (0x925c0) records the label address of each cull's EOP event; the
// consumer XPR_CONSUME_MASK (0x1a6d20) is matched to a recorded label through its context, and
// the label value is read through the backing view. Observe only: always HookAction::Continue.
//
// Context link (from the eboot pseudocode):
//   - 0x19af20 calls 0x1a6d20 with rcx = its param_2 (the frame context). [ctx] is the DCB wrapper.
//   - XPRPreRender (0xca6d0) rotates [ctx+0xe8] to the current frame slot and clears the labels
//     slot+0x40 and slot+0x68 to 0. Shadow cull (0xc6bd0) and main cull (0xc6f70) write their EOP
//     to slot+0x40 and slot+0x68.
//   So a consume's labels are [[ctx+0xe8]+0x40] and [[ctx+0xe8]+0x68]. The ring picks the most
//   recently recorded of those two addresses. That attribution is inferred: the mask's own cull is
//   not proven from the consumer's arguments. A wrong link shows up as "unknown".
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "hooks.h"
#include "probes.h"

extern "C" {
// runtime_memory.c: the unprotected backing view of a guest address (null when unmapped).
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
}

namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr unsigned kRing = 32;               // recent label records (power of two)
constexpr uint64_t kSlotOffset = 0xe8;       // ctx+0xe8: pointer to the current frame slot
constexpr uint64_t kShadowLabelOffset = 0x40; // slot+0x40: shadow cull EOP label (0xc6bd0)
constexpr uint64_t kMainLabelOffset = 0x68;   // slot+0x68: main cull EOP label (0xc6f70)

// One recorded label address. seq = record index + 1 (0 while being written).
struct RingSlot {
    std::atomic<uint64_t> seq{0};
    std::atomic<uintptr_t> addr{0};
};
RingSlot g_ring[kRing];
std::atomic<uint64_t> g_head{0}; // labels recorded so far

// Window counters, read and reset by ReportXpr.
std::atomic<uint64_t> g_labels{0};
std::atomic<uint64_t> g_consumes{0};
std::atomic<uint64_t> g_before{0};  // label read as 0: GPU has not signalled yet
std::atomic<uint64_t> g_after{0};   // label read non-zero: GPU has signalled
std::atomic<uint64_t> g_unknown{0}; // no label found in the ring, or unreadable

// Reads guest memory through the backing view (never a direct read: the GPU page tracker may
// protect the guest pages).
bool ReadGuest(uintptr_t address, void* out, size_t size) {
    const unsigned char* p = runtime_memory_backing_pointer(address);
    if (!p) {
        return false;
    }
    std::memcpy(out, p, size);
    return true;
}

// The most recent recorded label equal to one of the two candidates, or 0.
uintptr_t FindLabel(uintptr_t a, uintptr_t b) {
    const uint64_t head = g_head.load(std::memory_order_acquire);
    const uint64_t count = head < kRing ? head : kRing;
    for (uint64_t i = 1; i <= count; ++i) {
        const uint64_t index = head - i;
        const RingSlot& slot = g_ring[index % kRing];
        if (slot.seq.load(std::memory_order_acquire) != index + 1) {
            continue; // overwritten or still being written
        }
        const uintptr_t addr = slot.addr.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.seq.load(std::memory_order_relaxed) != index + 1) {
            continue;
        }
        if (addr == a || addr == b) {
            return addr;
        }
    }
    return 0;
}

// XPR_CS_DONE_LABEL (entry, 0x925c0). rsi = the label address (the EOP data address).
HookAction OnLabel(GuestRegs& regs, void*) {
    const uint64_t index = g_head.fetch_add(1, std::memory_order_acq_rel);
    RingSlot& slot = g_ring[index % kRing];
    slot.seq.store(0, std::memory_order_relaxed);
    slot.addr.store(static_cast<uintptr_t>(regs.rsi), std::memory_order_relaxed);
    slot.seq.store(index + 1, std::memory_order_release);
    g_labels.fetch_add(1, std::memory_order_relaxed);
    return HookAction::Continue;
}

// XPR_CONSUME_MASK (entry, 0x1a6d20). rsi = the visibility bitset (qwords), rcx = the frame
// context. The bitset itself is not read: the label state is the probe's question.
HookAction OnConsume(GuestRegs& regs, void*) {
    g_consumes.fetch_add(1, std::memory_order_relaxed);

    const uintptr_t ctx = static_cast<uintptr_t>(regs.rcx);
    uint64_t slot = 0;
    if (!ctx || !ReadGuest(ctx + kSlotOffset, &slot, sizeof slot) || !slot) {
        g_unknown.fetch_add(1, std::memory_order_relaxed);
        return HookAction::Continue;
    }

    const uintptr_t label = FindLabel(static_cast<uintptr_t>(slot + kShadowLabelOffset),
                                      static_cast<uintptr_t>(slot + kMainLabelOffset));
    uint32_t value = 0; // the EOP writes a 32-bit data value (1 when done)
    if (!label || !ReadGuest(label, &value, sizeof value)) {
        g_unknown.fetch_add(1, std::memory_order_relaxed);
        return HookAction::Continue;
    }
    if (value == 0) {
        g_before.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_after.fetch_add(1, std::memory_order_relaxed);
    }
    return HookAction::Continue;
}

} // namespace

namespace SotcProbes {

void RegisterXpr() {
    // Registered unconditionally: HookMode() is only known after InstallHooks, and a hook that is
    // off never dispatches to its handler.
    SotcHooks::Handler label;
    label.pre = &OnLabel;
    SotcHooks::RegisterHandler("XPR_CS_DONE_LABEL", label);

    SotcHooks::Handler consume;
    consume.pre = &OnConsume;
    SotcHooks::RegisterHandler("XPR_CONSUME_MASK", consume);
}

void ReportXpr(std::string& out, double seconds) {
    const uint64_t labels = g_labels.exchange(0, std::memory_order_relaxed);
    const uint64_t consumes = g_consumes.exchange(0, std::memory_order_relaxed);
    const uint64_t before = g_before.exchange(0, std::memory_order_relaxed);
    const uint64_t after = g_after.exchange(0, std::memory_order_relaxed);
    const uint64_t unknown = g_unknown.exchange(0, std::memory_order_relaxed);
    if (labels == 0 && consumes == 0) {
        return;
    }
    char line[256];
    std::snprintf(line, sizeof line,
                  "SOTC XPR probe: %llu labels, %llu consumes (%llu before the GPU signalled, "
                  "%llu after, %llu unknown) in %.0f s\n",
                  static_cast<unsigned long long>(labels),
                  static_cast<unsigned long long>(consumes),
                  static_cast<unsigned long long>(before),
                  static_cast<unsigned long long>(after),
                  static_cast<unsigned long long>(unknown), seconds);
    out += line;
}

} // namespace SotcProbes
