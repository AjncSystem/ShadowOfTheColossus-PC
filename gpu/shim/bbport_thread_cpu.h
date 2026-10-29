// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: on-CPU time of the GPU threads, for the SOTC_FPS accounting (video_core/sotc_stats.h).
// The only platform-specific code of that accounting lives here: QueryThreadCycleTime on Windows,
// the thread CPU clock (pthread_getcpuclockid) elsewhere.

#pragma once

#include <cstdint>

namespace BbThreadCpu {

/// The calling thread records itself under `role` (an index below MaxRoles; SotcStats::CpuRole).
/// Its CPU time is then summed into Sample(out[role]). Call once, from the thread itself, at its
/// start; registrations past the fixed table size are ignored.
void Register(int role);

constexpr int MaxRoles = 8;

/// Adds, per role, the on-CPU nanoseconds every registered thread has used since it started.
/// Monotonic per thread (an exited thread keeps its last value), so differences of two samples
/// are the CPU time used between them. Callable from any thread.
void Sample(uint64_t out[MaxRoles]);

} // namespace BbThreadCpu
