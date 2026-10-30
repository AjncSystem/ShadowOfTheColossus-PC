// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: particle-system count readback, plan C7 (docs/re-stutters/reports2/07-readback-consumers.md,
// section 2.3). Observe only: the handler always returns HookAction::Continue.
//
// ParticleSystemPool::ProcessStats (eboot 0x1018c0) keeps a three-slot ring of per-system count
// arrays, descriptors at pool+0xf8 + slot*0x28. With c = the pool counter before the call
// (pool+0x64), from the disassembly (0x101982-0x1019af):
//   - the dispatch (sub_928f0) writes the count pass into slot c%3;
//   - the read goes through sub_8c1b0 (0x8c1b0) with the descriptor of slot (c+1)%3.
// sub_8c1b0 takes a descriptor pointer and returns a guest pointer: low dword | (high dword & 0xfff)
// << 32, the base of the count array (the caller then reads array[system index] at 0x101a98).
// It does not choose the slot; the caller does. The slot read is therefore the one dispatched two
// calls earlier, and the prev slot (read slot + 1) % 3 is the one dispatched one call earlier.
//
// Why no previous-frame serving here: the read slot is already the oldest slot of the ring. The
// slot dispatched last is the newer one, and the GPU may still be running it, so pointing the read
// at it makes the CPU wait for newer GPU work. Older GPU data does not exist in a three-slot ring
// (the third slot is the one this call dispatches). A stall-free read needs a port-owned copy of a
// finished slot in memory the GPU page tracker does not protect, with the GPU data downloaded into
// it first. That is not built here. The stall itself comes from the 64 KiB page being shared with
// GPU-written buffers (CHANGELOG, SOTC_RB_EXACT), not from which slot is read.
//
// So SOTC_PARTICLE_PREV is observe-only in both modes, and it changes no guest state:
//   1 = count the ProcessStats reads and whether the prev slot's array is readable (a serving mode
//       would need exactly that); nothing is served ("served=0" in the report).
//   2 = as 1, and compare the counts of the read slot and the prev slot for the systems the caller
//       reads (active bitmap at pool+0x198, bound pool+0x1a8). Counts differing systems. The values
//       come from the backing view, not from the guest read, so they show what a non-faulting
//       reader would see; they do not prove what the game reads.
// Only the return address 0x1019b4 (ProcessStats) is measured. WaterRendering::ReadQueries calls
// the same accessor from 0xe4930 and is ignored.
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "hooks.h"
#include "particle_prev.h"

extern "C" {
// runtime_memory.c: the unprotected backing view of a guest address (null when unmapped).
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
}

namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr const char* kHook = "PARTICLE_COUNT_READ";
constexpr uint64_t kProcessStatsReturn = 0x1019b4; // eboot offset after "call sub_8c1b0" at 0x1019af
constexpr uint64_t kPoolRing = 0xf8;               // pool+0xf8: three descriptors
constexpr uint64_t kSlotStride = 0x28;             // descriptor stride
constexpr uint64_t kPoolBitmapPtr = 0x198;         // pool+0x198: u64* active-system bitmap
constexpr uint64_t kPoolBitmapWords = 0x1a0;       // pool+0x1a0: u32 number of bitmap words
constexpr uint64_t kPoolSystemLimit = 0x1a8;       // pool+0x1a8: u64 bound on the system index
constexpr uint64_t kMaxWords = 64;                 // observation cap: 4096 systems

// Window counters, read and reset by ReportParticlePrev.
std::atomic<uint64_t> g_calls{0};           // ProcessStats reads seen
std::atomic<uint64_t> g_read_unreadable{0}; // read slot (the one the game reads) not readable
std::atomic<uint64_t> g_prev_unreadable{0}; // prev slot not readable
std::atomic<uint64_t> g_systems{0};         // mode 2: active systems compared
std::atomic<uint64_t> g_differ{0};          // mode 2: systems whose counts differ
std::atomic<uint64_t> g_calls_differ{0};    // mode 2: calls with at least one difference

// Reads size bytes at a guest address through the backing view (never a direct read: the GPU page
// tracker may protect these pages). False when unmapped or when the value would cross a page.
bool ReadGuest(uint64_t address, void* out, size_t size) {
    if ((address & 0xfff) + size > 0x1000) {
        return false;
    }
    const unsigned char* p = runtime_memory_backing_pointer(static_cast<uintptr_t>(address));
    if (!p) {
        return false;
    }
    std::memcpy(out, p, size);
    return true;
}

// The guest address of a count array, from its descriptor: the same arithmetic as sub_8c1b0.
bool ArrayAddress(uint64_t descriptor, uint64_t* array) {
    uint32_t lo = 0;
    uint32_t hi = 0;
    if (!ReadGuest(descriptor, &lo, 4) || !ReadGuest(descriptor + 4, &hi, 4)) {
        return false;
    }
    *array = uint64_t{lo} | ((uint64_t{hi} & 0xfff) << 32);
    return true;
}

HookAction OnCountRead(GuestRegs& regs, void*) {
    // Read once, after the hooks are installed (the first call comes from the game).
    static const int mode = SotcHooks::HookMode(kHook);
    if (mode == 0) {
        return HookAction::Continue;
    }

    // Only the ProcessStats call site. At entry, [rsp] is the return address.
    const uint64_t image = static_cast<uint64_t>(SotcHooks::ImageBase());
    uint64_t ret = 0;
    std::memcpy(&ret, reinterpret_cast<const void*>(static_cast<uintptr_t>(regs.rsp)), 8);
    if (image == 0 || ret != image + kProcessStatsReturn) {
        return HookAction::Continue;
    }

    // rdi is the descriptor of the slot being read; r13 is the pool object (ProcessStats keeps
    // param_1 there). Anything else is not the pattern we checked, so leave the call alone.
    const uint64_t pool = regs.r13;
    const uint64_t read_desc = regs.rdi;
    if (read_desc < pool + kPoolRing || (read_desc - pool - kPoolRing) % kSlotStride != 0) {
        return HookAction::Continue;
    }
    const uint64_t read_slot = (read_desc - pool - kPoolRing) / kSlotStride;
    if (read_slot >= 3) {
        return HookAction::Continue;
    }
    // The slot dispatched one call before the read: (read + 1) % 3 (see the file comment).
    const uint64_t prev_desc = pool + kPoolRing + ((read_slot + 1) % 3) * kSlotStride;

    g_calls.fetch_add(1, std::memory_order_relaxed);

    uint64_t read_array = 0;
    uint64_t prev_array = 0;
    uint32_t sample = 0;
    const bool read_ok = ArrayAddress(read_desc, &read_array) && ReadGuest(read_array, &sample, 4);
    const bool prev_ok = ArrayAddress(prev_desc, &prev_array) && ReadGuest(prev_array, &sample, 4);
    if (!read_ok) {
        g_read_unreadable.fetch_add(1, std::memory_order_relaxed);
    }
    if (!prev_ok) {
        g_prev_unreadable.fetch_add(1, std::memory_order_relaxed);
    }
    if (mode != 2 || !read_ok || !prev_ok) {
        return HookAction::Continue;
    }

    // Systems the caller reads: bit i of the active bitmap, with i < pool+0x1a8 (as at 0x101a98).
    uint64_t bitmap = 0;
    uint64_t limit = 0;
    uint32_t words = 0;
    if (!ReadGuest(pool + kPoolBitmapPtr, &bitmap, 8) || !ReadGuest(pool + kPoolBitmapWords, &words, 4) ||
        !ReadGuest(pool + kPoolSystemLimit, &limit, 8)) {
        return HookAction::Continue;
    }
    if (words > kMaxWords) {
        words = static_cast<uint32_t>(kMaxWords);
    }

    uint64_t systems = 0;
    uint64_t differ = 0;
    for (uint64_t w = 0; w < words; ++w) {
        uint64_t bits = 0;
        if (!ReadGuest(bitmap + 8 * w, &bits, 8)) {
            break;
        }
        while (bits != 0) {
            const uint64_t i = w * 64 + static_cast<uint64_t>(std::countr_zero(bits));
            bits &= bits - 1;
            if (i >= limit) {
                break;
            }
            uint32_t cur = 0;
            uint32_t prev = 0;
            if (!ReadGuest(read_array + 4 * i, &cur, 4) || !ReadGuest(prev_array + 4 * i, &prev, 4)) {
                continue;
            }
            ++systems;
            if (cur != prev) {
                ++differ;
            }
        }
    }
    g_systems.fetch_add(systems, std::memory_order_relaxed);
    g_differ.fetch_add(differ, std::memory_order_relaxed);
    if (differ != 0) {
        g_calls_differ.fetch_add(1, std::memory_order_relaxed);
    }
    return HookAction::Continue;
}

} // namespace

namespace SotcProbes {

void RegisterParticlePrev() {
    SotcHooks::Handler handler;
    handler.pre = &OnCountRead;
    handler.post = nullptr;
    handler.user = nullptr;
    SotcHooks::RegisterHandler(kHook, handler);
}

void ReportParticlePrev(std::string& out, double seconds) {
    if (!SotcHooks::IsInstalled(kHook)) {
        return;
    }
    const int mode = SotcHooks::HookMode(kHook);
    const uint64_t calls = g_calls.exchange(0, std::memory_order_relaxed);
    const uint64_t read_bad = g_read_unreadable.exchange(0, std::memory_order_relaxed);
    const uint64_t prev_bad = g_prev_unreadable.exchange(0, std::memory_order_relaxed);
    const uint64_t systems = g_systems.exchange(0, std::memory_order_relaxed);
    const uint64_t differ = g_differ.exchange(0, std::memory_order_relaxed);
    const uint64_t calls_differ = g_calls_differ.exchange(0, std::memory_order_relaxed);
    const double rate = seconds > 0 ? static_cast<double>(calls) / seconds : 0.0;

    char line[400];
    int n = std::snprintf(line, sizeof(line),
                          "SOTC PARTICLE_PREV mode=%d served=0 (observe only): ProcessStats reads %llu (%.2f/s), "
                          "read slot unreadable %llu, prev slot unreadable %llu",
                          mode, static_cast<unsigned long long>(calls), rate,
                          static_cast<unsigned long long>(read_bad), static_cast<unsigned long long>(prev_bad));
    if (mode == 2 && n > 0 && n < static_cast<int>(sizeof(line))) {
        const double pct = systems > 0 ? 100.0 * static_cast<double>(differ) / static_cast<double>(systems) : 0.0;
        std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
                      ", systems compared %llu, differing %llu (%.2f%%), calls with a difference %llu",
                      static_cast<unsigned long long>(systems), static_cast<unsigned long long>(differ), pct,
                      static_cast<unsigned long long>(calls_differ));
    }
    out += line;
    out += '\n';
}

} // namespace SotcProbes
