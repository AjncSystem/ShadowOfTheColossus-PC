// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the engine's own profiler scopes as trace spans (trace layer L3b, scripts/hooks.d/profiler_scopes.py).
// Switch SOTC_TRACE_SCOPES; hooks PROFILER_PUSH_SCOPE (Profiler::PushScope 0x16b8c0) and
// PROFILER_POP_SCOPE (Profiler::PopScope 0x16b900), both entries. Needs SOTC_TRACE=1 or 2.
#pragma once

#include <string>

namespace SotcProbes {

/// Registers the PROFILER_PUSH_SCOPE and PROFILER_POP_SCOPE handlers. Called from RegisterAllHandlers
/// (hook_handlers.cpp). Registers nothing when the trace recorder is off.
void RegisterScopes();

/// Appends one line to out with the scope counts of the last window (pushes, pops, spans written,
/// unmatched pops, inner scopes dropped on a pop, depth overflows, unreadable names, distinct names)
/// and resets the window. Writes nothing when no scope ran in the window (or the hooks are off).
/// seconds = window length.
void ReportScopes(std::string& out, double seconds);

} // namespace SotcProbes
