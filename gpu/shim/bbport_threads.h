// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: helper thread sizing. Counts follow the hardware threads this process may run on
// (the affinity mask, so `taskset` can emulate a Steam Deck), and speculative helpers run as
// SCHED_IDLE: they use cores the game leaves idle and never take time from its threads.

#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <bit>
#include <windows.h>
#else
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace BbThreads {
/// GPU threads that PlaceThread can pin (SOTC_CPUSETS=1, Windows only).
/// sotc: Prep: SOTC_PARALLEL_RECORD's GpuPrep threads (normal priority, on the sibling CPUs).
enum class Role { CommandProcessor, Draw, Recorder, Present, PipelineWorker, Copy, Prep };

#ifdef _WIN32
/// Hardware threads available to the process (its affinity mask).
inline unsigned Available() {
    DWORD_PTR process = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
        return std::max(1, std::popcount(static_cast<unsigned long long>(process)));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

/// The calling thread only runs when cores are otherwise idle (lowest Windows priority).
inline void MakeBackground() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
}

/// sotc: SOTC_CPUSETS=1 enables PlaceThread. Off by default.
inline bool CpuSetsEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_CPUSETS");
        return v && v[0] == '1';
    }();
    return enabled;
}

/// sotc: CPU set layout, built once from GetSystemCpuSetInformation (on first use).
struct CpuLayout {
    bool usable = false;
    // Exclusive set per role: first logical CPU of physical cores 0..3
    // (CommandProcessor, Draw, Recorder, Present).
    ULONG exclusive[4] = {};
    // Second (SMT sibling) logical CPU of every physical core that has one.
    std::vector<ULONG> siblings;
};

inline const CpuLayout& GetCpuLayout() {
    static const CpuLayout layout = [] {
        struct Entry {
            BYTE core;
            BYTE lp;
            ULONG id;
            BYTE efficiency; // higher = faster core class (P-cores on hybrid CPUs)
        };
        CpuLayout out;

        // Only CPUs this process may run on (taskset-style masks), processor group 0 only.
        DWORD_PTR process_mask = 0, system_mask = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) ||
            !process_mask) {
            process_mask = ~DWORD_PTR{0};
        }
        constexpr unsigned kMaskBits = sizeof(DWORD_PTR) * 8;

        ULONG length = 0;
        GetSystemCpuSetInformation(nullptr, 0, &length, nullptr, 0);
        if (length == 0) {
            std::fprintf(stderr, "sotc: SOTC_CPUSETS: no CPU set information, threads not placed\n");
            return out;
        }
        std::vector<BYTE> buffer(length);
        if (!GetSystemCpuSetInformation(reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data()),
                                        length, &length, nullptr, 0)) {
            std::fprintf(stderr,
                         "sotc: SOTC_CPUSETS: GetSystemCpuSetInformation failed (%lu), threads not placed\n",
                         GetLastError());
            return out;
        }

        // Records have variable size: advance by Size.
        std::vector<Entry> entries;
        for (BYTE* p = buffer.data(); p < buffer.data() + length;) {
            auto* info = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(p);
            if (info->Size == 0) {
                break;
            }
            if (info->Type == CpuSetInformation) {
                const auto& cs = info->CpuSet;
                if (cs.Group == 0 && cs.LogicalProcessorIndex < kMaskBits &&
                    ((process_mask >> cs.LogicalProcessorIndex) & 1)) {
                    entries.push_back({cs.CoreIndex, cs.LogicalProcessorIndex, cs.Id, cs.EfficiencyClass});
                }
            }
            p += info->Size;
        }

        // Physical cores by CoreIndex; SMT siblings in LogicalProcessorIndex order.
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.core != b.core ? a.core < b.core : a.lp < b.lp;
        });
        std::vector<std::vector<Entry>> cores;
        for (const Entry& e : entries) {
            if (cores.empty() || cores.back().front().core != e.core) {
                cores.emplace_back();
            }
            cores.back().push_back(e);
        }

        // sotc: hybrid CPUs (Intel P/E cores): the GPU threads take the fastest class first;
        // the order inside a class stays by CoreIndex. Same-class CPUs are unaffected.
        std::stable_sort(cores.begin(), cores.end(), [](const auto& a, const auto& b) {
            return a.front().efficiency > b.front().efficiency;
        });
        const BYTE fastest = cores.empty() ? 0 : cores.front().front().efficiency;
        std::size_t fast_cores = 0;
        while (fast_cores < cores.size() && cores[fast_cores].front().efficiency == fastest) {
            ++fast_cores;
        }

        if (cores.size() < 6 || fast_cores < 4) {
            std::fprintf(stderr,
                         "sotc: SOTC_CPUSETS: %zu physical cores, %zu of the fastest class (need 6 "
                         "and 4), threads not placed\n",
                         cores.size(), fast_cores);
            return out;
        }
        for (std::size_t i = 0; i < 4; ++i) {
            out.exclusive[i] = cores[i].front().id;
        }
        for (const auto& core : cores) {
            if (core.front().efficiency != fastest) {
                // Slower cores (E-cores) take the compile and copy workers whole.
                for (const Entry& e : core) {
                    out.siblings.push_back(e.id);
                }
            } else if (core.size() > 1) {
                out.siblings.push_back(core[1].id);
            }
        }
        out.usable = true;

        std::fprintf(stderr,
                     "sotc: SOTC_CPUSETS layout: CommandProcessor core %u (LP %u), Draw core %u (LP %u), "
                     "Recorder core %u (LP %u), Present core %u (LP %u); PipelineWorker/Copy on %zu "
                     "SMT siblings%s\n",
                     cores[0].front().core, cores[0].front().lp, cores[1].front().core,
                     cores[1].front().lp, cores[2].front().core, cores[2].front().lp,
                     cores[3].front().core, cores[3].front().lp, out.siblings.size(),
                     out.siblings.empty() ? " (no SMT: left unpinned)" : "");
        return out;
    }();
    return layout;
}

/// sotc: pins the calling thread to its role's CPU set (SOTC_CPUSETS=1). Game threads are not
/// touched. Does nothing with fewer than 6 physical cores.
inline void PlaceThread(Role role) {
    if (!CpuSetsEnabled()) {
        return;
    }
    const CpuLayout& layout = GetCpuLayout();
    if (!layout.usable) {
        return;
    }
    const ULONG* ids = nullptr;
    ULONG count = 0;
    switch (role) {
    case Role::CommandProcessor: ids = &layout.exclusive[0]; count = 1; break;
    case Role::Draw: ids = &layout.exclusive[1]; count = 1; break;
    case Role::Recorder: ids = &layout.exclusive[2]; count = 1; break;
    case Role::Present: ids = &layout.exclusive[3]; count = 1; break;
    case Role::PipelineWorker:
    case Role::Copy:
    case Role::Prep:
        ids = layout.siblings.data();
        count = static_cast<ULONG>(layout.siblings.size());
        break;
    }
    if (ids && count) {
        SetThreadSelectedCpuSets(GetCurrentThread(), ids, count);
    }
}
#else

/// Hardware threads available to the process.
inline unsigned Available() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        return std::max(1, CPU_COUNT(&set));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

/// The calling thread only runs on otherwise idle cores (falls back to the lowest nice level).
inline void MakeBackground() {
    sched_param param{};
    if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
        setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
    }
}

// sotc: CPU set placement is Windows only; no-ops elsewhere.
inline bool CpuSetsEnabled() {
    return false;
}

inline void PlaceThread(Role) {}
#endif

} // namespace BbThreads
