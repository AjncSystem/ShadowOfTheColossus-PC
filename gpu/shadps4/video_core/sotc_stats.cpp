// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: thread CPU time for the SOTC_DRAW_SPLIT lines (sotc_stats.h).
#include "video_core/sotc_stats.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <pthread.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
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
// sotc (Linux): the registered thread's own CPU clock (pthread_getcpuclockid), in nanoseconds,
// converted to the TSC ticks of Ticks() so the SOTC_DRAW_SPLIT lines read the same as on Windows.
static std::atomic<clockid_t> split_clocks[SplitThreads]{};
static std::atomic<bool> split_registered[SplitThreads]{};

void RegisterSplitThread(SplitThread which) {
    if (!kDrawSplit) {
        return;
    }
    clockid_t clock{};
    if (pthread_getcpuclockid(pthread_self(), &clock) == 0) {
        split_clocks[which].store(clock, std::memory_order_relaxed);
        split_registered[which].store(true, std::memory_order_release);
    }
}

uint64_t SplitThreadCycles(SplitThread which) {
    if (!split_registered[which].load(std::memory_order_acquire)) {
        return 0;
    }
    timespec ts{};
    if (clock_gettime(split_clocks[which].load(std::memory_order_relaxed), &ts) != 0) {
        return 0;
    }
    const double ns = static_cast<double>(ts.tv_sec) * 1e9 + static_cast<double>(ts.tv_nsec);
    return static_cast<uint64_t>(ns * TicksPerMs() / 1e6);
}

uint64_t SplitProcessFaults() {
    rusage usage{};
    return getrusage(RUSAGE_SELF, &usage) == 0
               ? static_cast<uint64_t>(usage.ru_minflt) + static_cast<uint64_t>(usage.ru_majflt)
               : 0;
}
#endif

uint64_t SystemRamBytes() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? status.ullTotalPhys : 0;
#else
    const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    return pages > 0 && page > 0 ? static_cast<uint64_t>(pages) * static_cast<uint64_t>(page) : 0;
#endif
}

void LogSystemMemory() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        std::fprintf(stderr,
                     "SOTC system memory: commit %llu of %llu MiB in use, RAM %llu of %llu MiB free\n",
                     static_cast<unsigned long long>((status.ullTotalPageFile - status.ullAvailPageFile) >> 20),
                     static_cast<unsigned long long>(status.ullTotalPageFile >> 20),
                     static_cast<unsigned long long>(status.ullAvailPhys >> 20),
                     static_cast<unsigned long long>(status.ullTotalPhys >> 20));
    }
#endif
}

} // namespace SotcStats
