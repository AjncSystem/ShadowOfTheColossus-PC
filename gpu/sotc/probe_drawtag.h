// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: render-job draw tag probe (plan B3). Switch SOTC_DRAW_TAG; hook RENDER_JOB_RUN
// (scripts/hooks.d/drawtag.py, RenderThread::RunFunctionAsRenderJob at 0xc12a0, wrap).
// Observe only: it counts the render jobs per body address and times the inline ones.
#pragma once

#include <string>

namespace SotcProbes {

/// Registers the RENDER_JOB_RUN handler. Called from RegisterAllHandlers (hook_handlers.cpp).
void RegisterDrawTag();

/// Appends one line to out for the last window: the top render-job bodies by inline wall time, with
/// their image offsets (match them to tools/re/names.txt). Writes nothing when no job ran in the
/// window (or the hook is not installed). seconds = window length. Resets the window.
void ReportDrawTag(std::string& out, double seconds);

} // namespace SotcProbes
