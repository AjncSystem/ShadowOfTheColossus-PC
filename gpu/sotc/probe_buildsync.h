// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: BuildSync probe (plan B1, docs/re-stutters/reports2/17-buildsync-callers.md). Observe only.
// Hooks: BUILDSYNC_BUILD and BUILDSYNC_ENSURE (scripts/hooks.d/buildsync.py), switch
// SOTC_BUILDSYNC_PROBE. Both handlers return HookAction::Continue and write nothing in the guest.
#pragma once

#include <string>

namespace SotcBuildSync {

/// Registers the handlers of BUILDSYNC_BUILD and BUILDSYNC_ENSURE. Called from RegisterAllHandlers
/// (gpu/sotc/hook_handlers.cpp). Registered unconditionally: a hook whose switch is off never
/// dispatches to its handler.
void RegisterBuildSyncProbe();

/// Appends the window's lines to out and resets the window counters. Writes nothing unless
/// SOTC_BUILDSYNC_PROBE is set. seconds = window length (driver.cpp 5 s report).
void ReportBuildSyncProbe(std::string& out, double seconds);

} // namespace SotcBuildSync
