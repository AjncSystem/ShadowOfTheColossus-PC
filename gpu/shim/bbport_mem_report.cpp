// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_mem_report.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>
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

extern "C" uint64_t runtime_memory_discarded(void);

#ifdef _WIN32
namespace {
std::atomic<uint64_t> ws_copies, ws_bytes, ws_ns, ws_src_pages, ws_src_out, ws_dst_pages, ws_dst_out,
    ws_cold_copies, ws_cold_ns, ws_query_ns, ws_src_guest_in;
uint64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
// Pages of [start, start + size) that are not valid in the working set.
uint64_t PagesOut(const void* start, size_t size, uint64_t* pages) {
    constexpr uintptr_t Page = 4096;
    const uintptr_t first = reinterpret_cast<uintptr_t>(start) & ~(Page - 1);
    const uintptr_t end = reinterpret_cast<uintptr_t>(start) + size;
    PSAPI_WORKING_SET_EX_INFORMATION info[256];
    uint64_t out = 0;
    for (uintptr_t at = first; at < end;) {
        DWORD n = 0;
        for (; n < 256 && at < end; ++n, at += Page) {
            info[n].VirtualAddress = reinterpret_cast<void*>(at);
        }
        if (!QueryWorkingSetEx(GetCurrentProcess(), info, n * sizeof(info[0]))) {
            return out;
        }
        *pages += n;
        for (DWORD i = 0; i < n; ++i) {
            out += !info[i].VirtualAttributes.Valid;
        }
    }
    return out;
}
// Pages of [start, start + size) not valid in the working set whose guest address (the other view
// of the same memory) is valid: a read through start soft-faults although the page is resident.
uint64_t PagesOnlyInGuest(const void* start, const void* guest, size_t size) {
    constexpr uintptr_t Page = 4096;
    const uintptr_t first = reinterpret_cast<uintptr_t>(start) & ~(Page - 1);
    const uintptr_t delta = reinterpret_cast<uintptr_t>(guest) - reinterpret_cast<uintptr_t>(start);
    const uintptr_t end = reinterpret_cast<uintptr_t>(start) + size;
    PSAPI_WORKING_SET_EX_INFORMATION info[256];
    uint64_t only = 0;
    for (uintptr_t at = first; at < end;) {
        DWORD n = 0;
        for (; n < 128 && at < end; ++n, at += Page) {
            info[2 * n].VirtualAddress = reinterpret_cast<void*>(at);
            info[2 * n + 1].VirtualAddress = reinterpret_cast<void*>(at + delta);
        }
        if (!QueryWorkingSetEx(GetCurrentProcess(), info, 2 * n * sizeof(info[0]))) {
            return only;
        }
        for (DWORD i = 0; i < n; ++i) {
            only += !info[2 * i].VirtualAttributes.Valid && info[2 * i + 1].VirtualAttributes.Valid;
        }
    }
    return only;
}
thread_local bool ws_cold;
} // namespace
#endif

bool WsDiagEnabled() {
#ifdef _WIN32
    static const bool on = [] {
        const char* v = std::getenv("SOTC_WS_DIAG");
        return v && v[0] == '1';
    }();
    return on;
#else
    return false;
#endif
}

void WsDiagCopy(const void* source, const void* guest, const void* dest, size_t size) {
#ifdef _WIN32
    const uint64_t start = NowNs();
    uint64_t src_pages = 0, dst_pages = 0;
    const uint64_t src_out = PagesOut(source, size, &src_pages);
    const uint64_t dst_out = PagesOut(dest, size, &dst_pages);
    ws_src_pages += src_pages;
    ws_src_out += src_out;
    if (src_out && guest && guest != source) {
        ws_src_guest_in += PagesOnlyInGuest(source, guest, size);
    }
    ws_dst_pages += dst_pages;
    ws_dst_out += dst_out;
    ws_cold = src_out || dst_out;
    ws_query_ns += NowNs() - start;
#endif
}

void WsDiagCopyDone(uint64_t ns_before, size_t size) {
#ifdef _WIN32
    const uint64_t ns = NowNs() - ns_before;
    ++ws_copies;
    ws_bytes += size;
    ws_ns += ns;
    if (ws_cold) {
        ++ws_cold_copies;
        ws_cold_ns += ns;
    }
#endif
}

std::string WsDiagLine() {
#ifdef _WIN32
    if (!WsDiagEnabled()) {
        return {};
    }
    static uint64_t last_faults = 0;
    PROCESS_MEMORY_COUNTERS counters{};
    GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
    const uint64_t faults = counters.PageFaultCount - std::exchange(last_faults, counters.PageFaultCount);
    // ws_ns includes the queries; subtract them for the copy time.
    const uint64_t query = ws_query_ns.exchange(0);
    char line[640];
    std::snprintf(line, sizeof(line),
                  "copies %llu (%.0f MiB, %.1f ms copying, %.1f ms querying); source pages out of "
                  "the working set %llu of %llu (%llu valid at the guest address), staging pages %llu of %llu; copies touching them "
                  "%llu (%.1f ms); process page faults %llu, working set %.0f MiB; guest memory zeroed on release %.0f MiB",
                  static_cast<unsigned long long>(ws_copies.exchange(0)), ws_bytes.exchange(0) / 1048576.0,
                  ws_ns.exchange(0) / 1e6 - query / 1e6, query / 1e6,
                  static_cast<unsigned long long>(ws_src_out.exchange(0)),
                  static_cast<unsigned long long>(ws_src_pages.exchange(0)),
                  static_cast<unsigned long long>(ws_src_guest_in.exchange(0)),
                  static_cast<unsigned long long>(ws_dst_out.exchange(0)),
                  static_cast<unsigned long long>(ws_dst_pages.exchange(0)),
                  static_cast<unsigned long long>(ws_cold_copies.exchange(0)), ws_cold_ns.exchange(0) / 1e6,
                  static_cast<unsigned long long>(faults), counters.WorkingSetSize / 1048576.0,
                  runtime_memory_discarded() / 1048576.0);
    return line;
#else
    return {};
#endif
}

} // namespace BbMemReport
