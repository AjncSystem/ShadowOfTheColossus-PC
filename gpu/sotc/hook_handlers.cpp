// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the one place that registers every engine hook handler (gpu/sotc/hooks.h). Each probe
// file exposes a Register<Name>() function; it is listed here because the GPU library is
// static and unreferenced objects would not be linked.
#include "hooks.h"
#include "probes.h"
#include "probe_buildsync.h" // sotc: Wave D
#include "probe_dt.h"
#include "probe_drawtag.h"
#include "particle_prev.h"
#include "decommit_budget.h"
#include "mip_budget.h"
#include "probe_scopes.h" // sotc: trace v2, engine profiler scopes (SOTC_TRACE_SCOPES)
#include "ctl_server.h"   // sotc: SotC Lab
#include "engine_cmd.h"
#include "freecam.h"

namespace SotcHooks {

void RegisterAllHandlers() {
    // Engine probes (gpu/sotc/probe_*.cpp). Each registers the handlers of its own hooks; a
    // hook whose switch is off installs nothing, so a registration here is harmless when off.
    SotcProbes::RegisterXpr();
    SotcProbes::RegisterLoader();
    SotcProbes::RegisterAlloc();
    SotcProbes::RegisterCvars();
    SotcProbes::RegisterArea();
    // sotc: Wave D probes and optimizations (docs/re-stutters/reports2/00-PLAN.md).
    SotcBuildSync::RegisterBuildSyncProbe();
    SotcProbes::RegisterDtProbe();
    SotcProbes::RegisterDrawTag();
    SotcProbes::RegisterParticlePrev();
    SotcDecommitBudget::RegisterDecommitBudget();
    SotcMipBudget::RegisterMipBudget();
    // sotc: trace v2, engine profiler scopes (SOTC_TRACE_SCOPES; the hook installs only when
    // SOTC_TRACE is also on, so the handler is harmless otherwise).
    SotcProbes::RegisterScopes();
    // sotc: SotC Lab (docs/sotc/LAB.md): engine commands, free-cam (+ its camera probe) and the
    // control pipe. Off unless SOTC_LAB / their own switches are set.
    SotcEngineCmd::Register();
    SotcFreecam::Register();
    SotcCtl::StartIfEnabled();
}

} // namespace SotcHooks
