// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: engine probes (plan step 0c, docs/HOOK-DESIGN.md). Each probe file registers the
// handlers of its hooks (gpu/sotc/hooks.h) and prints its own report line. Probes only observe:
// they never change guest registers or memory, and every handler returns HookAction::Continue.
//
// Hook names (scripts/hooks.d/*.py) and their switches:
//   probe_xpr.cpp     XPR_CS_DONE_LABEL (0x925c0, entry), XPR_CONSUME_MASK (0x1a6d20, entry)
//                     switch SOTC_HOOK_XPR_PROBE
//   probe_loader.cpp  LOADER_WAIT_ALL (0x1469d0, wrap), LOADER_SYNCPOINT (0x1480f0, wrap)
//                     switch SOTC_HOOK_LOADER_PROBE
//                     LOADER_HAS_PENDING (0x147f40, wrap) is declared for step 5a; no handler yet
//                     (switch SOTC_HOOK_LOADER_NOSTREAM)
//   probe_alloc.cpp   ALLOC_BLOCK (0x479a0, wrap), FREE_BLOCK (0x1b0e0, entry)
//                     switch SOTC_HOOK_ALLOC_PROBE
//                     BPE_HEAP_CODE (0x48a70-0x4a477, range; read by the fault classes)
//                     switch SOTC_HOOK_HEAP_RANGE
//   probe_cvars.cpp   CVAR_MAX_DELTA (range in sub_262a80 with a rip_target derived field: the
//                     cvar's storage), switch SOTC_HOOK_CVAR_PROBE
//   probe_area.cpp    AREA_PRECACHE (0x533100, entry), AREA_LOADER_STATE (the
//                     cp10_setAreaLoaderState user, entry), switch SOTC_AREA_PREWARM
#pragma once

#include <string>

namespace SotcProbes {

void RegisterXpr();
void RegisterLoader();
void RegisterAlloc();
void RegisterCvars();
void RegisterArea();

/// One report line each (or nothing when the probe is off or saw nothing in the window),
/// appended to out by the driver.cpp 5 s report. seconds = window length. Resets the window.
void ReportXpr(std::string& out, double seconds);
void ReportLoader(std::string& out, double seconds);
void ReportAlloc(std::string& out, double seconds);
void ReportCvars(std::string& out, double seconds);
void ReportArea(std::string& out, double seconds);

} // namespace SotcProbes
