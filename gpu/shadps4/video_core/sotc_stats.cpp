// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: thread CPU time for the SOTC_DRAW_SPLIT lines (sotc_stats.h).
#include "video_core/sotc_stats.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace SotcStats {

#ifdef _WIN32
static std::atomic<HANDLE> split_threads[SplitThreads]{};

void RegisterSplitThread(SplitThread which) {
    if (!kDrawSplit) {
        return;
    }
    HANDLE handle{};
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                        THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
        split_threads[which].store(handle, std::memory_order_release);
    }
}

uint64_t SplitThreadCycles(SplitThread which) {
    // The thread cycle time counts reference (TSC) ticks on current Windows, the unit of Ticks().
    const HANDLE handle = split_threads[which].load(std::memory_order_acquire);
    ULONG64 cycles = 0;
    if (!handle || !QueryThreadCycleTime(handle, &cycles)) {
        return 0;
    }
    return cycles;
}

uint64_t SplitProcessFaults() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    return K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))
               ? counters.PageFaultCount
               : 0;
}
#else
void RegisterSplitThread(SplitThread) {}

uint64_t SplitThreadCycles(SplitThread) {
    return 0;
}

uint64_t SplitProcessFaults() {
    return 0;
}
#endif

} // namespace SotcStats
