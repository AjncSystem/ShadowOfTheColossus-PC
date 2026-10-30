// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: delta-time probe (docs/re-stutters/reports2/05-frame-pacing.md, H1 and H2). Observe only.
// Hooks DT_CONSUMER and DT_UPDATE are declared in scripts/hooks.d/dt.py (switch SOTC_DT_PROBE).
#pragma once

#include <string>

namespace SotcProbes {

/// Registers the handlers of DT_CONSUMER (wrap, sub_25ccc0) and DT_UPDATE (site, 0x534a73).
/// Called from RegisterAllHandlers() (hook_handlers.cpp).
void RegisterDtProbe();

/// Appends one line for the window (min/median/max of the consumer output dt, the consumer's real
/// elapsed time and the update dt, and how many frames were at or near the gameMaxDeltaTime cap)
/// while SOTC_DT_PROBE is on. Nothing is appended when it is off. Resets the window.
/// seconds = window length (driver.cpp 5 s report).
void ReportDtProbe(std::string& out, double seconds);

} // namespace SotcProbes
