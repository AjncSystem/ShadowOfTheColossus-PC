// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: particle-system count readback probe (plan C7). Hook PARTICLE_COUNT_READ, declared in
// scripts/hooks.d/particle_prev.py, switch SOTC_PARTICLE_PREV. See particle_prev.cpp.
#pragma once

#include <string>

namespace SotcProbes {

/// Registers the handler of PARTICLE_COUNT_READ (gpu/sotc/hook_handlers.cpp calls it).
void RegisterParticlePrev();

/// One line per window while SOTC_PARTICLE_PREV is set and the hook resolved (nothing otherwise).
/// Appends to out. seconds = window length. Resets the window counters.
void ReportParticlePrev(std::string& out, double seconds);

} // namespace SotcProbes
