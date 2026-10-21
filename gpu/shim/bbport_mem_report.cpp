// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_mem_report.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#endif

namespace BbMemReport {

std::string Line() {
#ifdef _WIN32
    HANDLE heaps[512];
    const DWORD count = GetProcessHeaps(512, heaps);
    uint64_t heap_committed = 0, heap_largest = 0;
    for (DWORD i = 0; i < count && i < 512; ++i) {
        HEAP_SUMMARY summary{};
        summary.cb = sizeof(summary);
        if (HeapSummary(heaps[i], 0, &summary)) {
            heap_committed += summary.cbCommitted;
            heap_largest = std::max<uint64_t>(heap_largest, summary.cbCommitted);
        }
    }
    // Committed private memory per allocation (VirtualAlloc reservation). The heaps' segments
    // are among them: a growing heap also adds allocations of up to 16 MiB.
    std::unordered_map<uintptr_t, uint64_t> allocations;
    uint64_t privately = 0, mapped = 0;
    MEMORY_BASIC_INFORMATION info;
    for (uintptr_t address = 0; VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info));
         address = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize) {
        if (info.State != MEM_COMMIT) {
            continue;
        }
        if (info.Type == MEM_PRIVATE) {
            privately += info.RegionSize;
            allocations[reinterpret_cast<uintptr_t>(info.AllocationBase)] += info.RegionSize;
        } else if (info.Type == MEM_MAPPED) {
            mapped += info.RegionSize;
        }
    }
    // Buckets of the committed size of each allocation: < 1 MiB, < 16 MiB, < 64 MiB, more.
    uint64_t bytes[4]{}, number[4]{};
    for (const auto& [base, size] : allocations) {
        const int bucket = size < (1u << 20) ? 0 : size < (16u << 20) ? 1 : size < (64u << 20) ? 2 : 3;
        bytes[bucket] += size;
        ++number[bucket];
    }
    constexpr double MiB = 1048576.0;
    char line[320];
    std::snprintf(line, sizeof(line),
                  "private %.0f MiB (heaps %.0f MiB in %lu, largest %.0f; other %.0f; allocations <1M "
                  "%.0f MiB in %llu, <16M %.0f in %llu, <64M %.0f in %llu, larger %.0f in %llu), mapped "
                  "%.0f MiB",
                  privately / MiB, heap_committed / MiB, static_cast<unsigned long>(count),
                  heap_largest / MiB, (privately - std::min(privately, heap_committed)) / MiB,
                  bytes[0] / MiB, static_cast<unsigned long long>(number[0]),
                  bytes[1] / MiB, static_cast<unsigned long long>(number[1]), bytes[2] / MiB,
                  static_cast<unsigned long long>(number[2]), bytes[3] / MiB,
                  static_cast<unsigned long long>(number[3]), mapped / MiB);
    return line;
#else
    return {};
#endif
}

} // namespace BbMemReport
