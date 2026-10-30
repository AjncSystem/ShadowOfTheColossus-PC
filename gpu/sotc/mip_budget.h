// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: per-frame budget of streamed mip copies (plan C4, docs/re-stutters/reports2/03-streaming-bursts.md).
// Hook: MIP_BUDGET_COPY (scripts/hooks.d/mipbudget.py, sub_98ae0), switch SOTC_MIP_BUDGET.
// Implementation and the reason it observes only: mip_budget.cpp.
#pragma once

#include <string>

namespace SotcMipBudget {

/// Registers the MIP_BUDGET_COPY handler. Called from RegisterAllHandlers (hook_handlers.cpp).
void RegisterMipBudget();

/// One report line per window, appended to out by driver.cpp. Nothing when the switch is off or no
/// upgrade copy was seen in the window. seconds = window length. Resets the window.
void ReportMipBudget(std::string& out, double seconds);

} // namespace SotcMipBudget
