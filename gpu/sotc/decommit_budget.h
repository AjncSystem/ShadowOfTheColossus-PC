// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: texture pool decommit observer (plan C3, docs/re-stutters/reports2/03-streaming-bursts.md).
// Hooks: DECOMMIT_BEGIN / DECOMMIT_END (scripts/hooks.d/decommit.py), switch SOTC_DECOMMIT_BUDGET.
//   unset or 0  off: nothing is installed, nothing runs.
//   2           observe: counts the decommits of sub_98ae0's drop branch per window and per game
//               frame (the frame is the flip count, SotcStats::flip_count), and times them.
//               Every handler returns HookAction::Continue; the guest is never changed.
// Other non-zero values are read as 2 with one stderr line. Deferring the decommits is not
// implemented: see decommit.py and the reply that added this file.
#pragma once

#include <string>

namespace SotcDecommitBudget {

/// Registers the handlers of DECOMMIT_BEGIN and DECOMMIT_END. Called from RegisterAllHandlers
/// (gpu/sotc/hook_handlers.cpp). Harmless when the switch is off: the hooks are not installed.
void RegisterDecommitBudget();

/// One report line per 5 s window, appended to out: decommits this window, frames with decommits,
/// max decommits in one frame, wait (total and max), deferred (always 0) and max deferred age.
/// Writes nothing when the switch is off or the window is idle. Resets the window.
void ReportDecommitBudget(std::string& out, double seconds);

} // namespace SotcDecommitBudget
