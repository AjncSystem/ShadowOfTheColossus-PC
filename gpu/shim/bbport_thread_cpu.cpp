// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: see bbport_thread_cpu.h.

#include "bbport_thread_cpu.h"

#include <algorithm>
#include <atomic>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#else
#include <pthread.h>
#include <time.h>
#endif

namespace BbThreadCpu {
namespace {

struct Slot {
    std::atomic<bool> used{false};
    int role = 0;
#ifdef _WIN32
    HANDLE handle = nullptr;
#else
    clockid_t clock{};
#endif
    std::atomic<uint64_t> last{0}; ///< last good reading (an exited thread keeps it)
};

constexpr uint32_t MaxSlots = 64;
Slot g_slots[MaxSlots];
std::atomic<uint32_t> g_count{0};

#ifdef _WIN32
// QueryThreadCycleTime counts in time-stamp counter ticks (Windows charges a thread the TSC
// difference across its run); the TSC rate is calibrated against QueryPerformanceCounter from
// the program start, so it is exact once a second or two have passed (before any frame).
const uint64_t g_tsc0 = __rdtsc();
const int64_t g_qpc0 = [] {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return static_cast<int64_t>(v.QuadPart);
}();

double NsPerCycle() {
    static std::atomic<double> settled{0.0};
    if (const double f = settled.load(std::memory_order_relaxed); f != 0.0) {
        return f;
    }
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    const uint64_t tsc = __rdtsc();
    const double ns = static_cast<double>(now.QuadPart - g_qpc0) * 1e9 /
                      static_cast<double>(freq.QuadPart);
    if (tsc <= g_tsc0 || ns <= 0.0) {
        return 1.0 / 3.0; // not measurable yet: assume 3 GHz
    }
    const double f = ns / static_cast<double>(tsc - g_tsc0);
    if (ns > 2e9) {
        settled.store(f, std::memory_order_relaxed);
    }
    return f;
}

uint64_t ReadNs(const Slot& slot, double ns_per_cycle) {
    ULONG64 cycles = 0;
    if (!QueryThreadCycleTime(slot.handle, &cycles)) {
        return 0;
    }
    return static_cast<uint64_t>(static_cast<double>(cycles) * ns_per_cycle);
}
#else
uint64_t ReadNs(const Slot& slot, double) {
    timespec ts{};
    if (clock_gettime(slot.clock, &ts) != 0) {
        return 0; // the thread has exited
    }
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}
#endif

} // namespace

void Register(int role) {
    if (role < 0 || role >= MaxRoles) {
        return;
    }
    const uint32_t index = g_count.fetch_add(1, std::memory_order_relaxed);
    if (index >= MaxSlots) {
        return;
    }
    Slot& slot = g_slots[index];
    slot.role = role;
#ifdef _WIN32
    // GetCurrentThread is a pseudo handle meaning "the caller": duplicate it into a real one the
    // samplers can query from their own threads.
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                         &slot.handle, THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
        return;
    }
#else
    if (pthread_getcpuclockid(pthread_self(), &slot.clock) != 0) {
        return;
    }
#endif
    slot.used.store(true, std::memory_order_release);
}

void Sample(uint64_t out[MaxRoles]) {
    for (int i = 0; i < MaxRoles; ++i) {
        out[i] = 0;
    }
#ifdef _WIN32
    const double ns_per_cycle = NsPerCycle();
#else
    const double ns_per_cycle = 1.0;
#endif
    const uint32_t count = std::min(g_count.load(std::memory_order_relaxed), MaxSlots);
    for (uint32_t i = 0; i < count; ++i) {
        Slot& slot = g_slots[i];
        if (!slot.used.load(std::memory_order_acquire)) {
            continue;
        }
        uint64_t last = slot.last.load(std::memory_order_relaxed);
        uint64_t ns = ReadNs(slot, ns_per_cycle);
        // sotc: CAS max, two samplers can run at once (presenter thread and the flip printer).
        while (ns > last &&
               !slot.last.compare_exchange_weak(last, ns, std::memory_order_relaxed)) {
        }
        if (ns < last) {
            ns = last; // exited thread, or the calibration moved a little: stay monotonic
        }
        out[slot.role] += ns;
    }
}

} // namespace BbThreadCpu
